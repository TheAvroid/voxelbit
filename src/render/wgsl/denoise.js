  const TEMPORAL_SRC = () => /* wgsl */`
    @group(0) @binding(1) var gIrr : texture_2d<f32>;
    @group(0) @binding(2) var histPrev : texture_2d<f32>;
    @group(0) @binding(3) var samp : sampler;
    @group(0) @binding(4) var histOut : texture_storage_2d<rgba16float, write>;
    @group(0) @binding(5) var slotCur : texture_2d<u32>;             // ── DYNAMIC LIFE ── this frame's creature ids (TRACE)…
    @group(0) @binding(6) var slotPrev : texture_2d<u32>;            // …and last frame's — identity must match at the reprojected pixel or history is another surface's
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let cur = textureLoad(gIrr, vec2<i32>(gid.xy), 0);
      if (cur.b < 0.0) { textureStore(histOut, vec2<i32>(gid.xy), vec4<f32>(0.0, 0.0, -1.0, 0.0)); return; }
      let px = vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y);
      let wp = u.camPos + rayDir(px) * cur.b;
      let sl = textureLoad(slotCur, vec2<i32>(gid.xy), 0).r & 255u;
      var wpp = wp;                                                  // where this surface point WAS last frame:
      var lifeReject = false;
      if (sl != 0u) {                                                // a creature pixel reprojects through the slot's RIGID world motion — the held-item lesson:
        let mot = lifeMotV(i32(sl) - 1);                                // rigid anchor motion keeps SVGF history valid, per-voxel churn does not
        if ((u32(mot.w + 0.5) & 6u) != 0u) { lifeReject = true; }    // animation frame flipped / occupant changed / teleported → the model's voxels are different, reject outright
        wpp = wp - mot.xyz;
      }
      let uv = prevUVd(wpp - u.pPos);
      var hist = 1.0;
      var acc = cur.rg;
      if (!lifeReject && u.reset < 0.5 && uv.x > 0.001 && uv.y > 0.001 && uv.x < 0.999 && uv.y < 0.999) {
        let ps = textureLoad(slotPrev, vec2<i32>(uv * u.res), 0).r & 255u;
        if (ps == sl) {                                              // same identity (terrain↔terrain or same creature) — a vacated/covered pixel is a disocclusion, rejected here
          let prev = textureSampleLevel(histPrev, samp, uv, 0.0);
          let expT = length(wpp - u.pPos);
          if (prev.b > 0.0 && abs(prev.b - expT) < 0.02 * expT + 1.5) {   // rotation (not in the rigid delta) and newly revealed surfaces fail this distance check naturally
            let hBase = select(1.0, u.maxHist, LG(6u));              // bit 6 off → hist pinned at 1 = no irradiance history at all
            // Reactive pixels ease toward a 10-frame floor rather than snapping to 4. Snapping was
            // the flicker: the mask flipped every time a resting trunk woke, and the noise floor
            // jumped with it. 10 rather than 4 because 4 samples of a binary sun ray still sparkle.
            hist = min(abs(prev.a) + 1.0, mix(hBase, min(hBase, 10.0), clamp(cur.a, 0.0, 1.0)));   // reactive pixel: converge in ~4 frames so a moving shadow TRACKS its body instead of trailing it
            acc = mix(prev.rg, cur.rg, 1.0 / hist);
          }
        }
      }
      textureStore(histOut, vec2<i32>(gid.xy), vec4<f32>(acc, cur.b, select(hist, -hist, cur.a > 0.5)));   // SIGN carries the reactive flag to SPATIAL (magnitude unchanged) — see the note there
    }
  `;

  const SPATIAL_SRC = () => /* wgsl */`
    @group(0) @binding(1) var hist : texture_2d<f32>;
    @group(0) @binding(2) var gAlbedo : texture_2d<f32>;
    @group(0) @binding(3) var irrOut : texture_storage_2d<rgba16float, write>;
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let c = textureLoad(hist, vec2<i32>(gid.xy), 0);
      if (c.b < 0.0) { textureStore(irrOut, vec2<i32>(gid.xy), c); return; }
      let faceC = gbFace(textureLoad(gAlbedo, vec2<i32>(gid.xy), 0).a);   // gbFace, not a raw mask: SANDF is a TOP face wearing a different id, and comparing the raw ids would stop a sand pixel sharing irradiance with the grass beside it
      var P = array<vec2<f32>, 12>(
        vec2<f32>(-0.326, -0.406), vec2<f32>(-0.840, -0.074), vec2<f32>(-0.696, 0.457), vec2<f32>(-0.203, 0.621),
        vec2<f32>(0.962, -0.195), vec2<f32>(0.473, -0.480), vec2<f32>(0.519, 0.767), vec2<f32>(0.185, -0.893),
        vec2<f32>(0.507, 0.064), vec2<f32>(0.896, 0.412), vec2<f32>(-0.322, -0.933), vec2<f32>(-0.792, -0.598));
      // history magnitude; a NEGATIVE value means TEMPORAL flagged this pixel reactive (see there)
      let hA = abs(c.a);
      // A reactive pixel is held at low history deliberately, not because it is a disocclusion, so the
      // usual "low history => blur hard" rule would smear the very edge we capped history to keep sharp.
      // Filter it at a fixed tight radius instead: a little more grain for a few frames, in exchange for
      // a shadow that stays on the voxels it belongs to.
      let radius = select(mix(${(() => { const m = /[?&]blur=([0-9.]+)/.exec(location.search); return m ? Math.max(1, Math.min(8, parseFloat(m[1]))) : 4.0; })()}, 1.5, clamp(hA / 8.0, 0.0, 1.0)), 2.6, c.a < 0.0);   // see the note above: 8 px was smearing shadows across voxel faces on any surface whose history cannot converge
      var sum = c.rg; var wsum = 1.0;
      for (var i = 0; i < 12; i++) {
        if (!LG(7u)) { break; }                                  // bit 7 off → raw, unfiltered irradiance
        let q = vec2<i32>(vec2<f32>(gid.xy) + P[i] * radius + 0.5);
        if (q.x < 0 || q.y < 0 || q.x >= i32(u.res.x) || q.y >= i32(u.res.y)) { continue; }
        let s = textureLoad(hist, q, 0);
        let f2 = gbFace(textureLoad(gAlbedo, q, 0).a);
        if (s.b <= 0.0 || f2 != faceC || abs(s.b - c.b) > 0.04 * c.b + 2.0) { continue; }
        if ((c.a < 0.0) != (s.a < 0.0)) { continue; }   // never mix a moving-shadow pixel with a settled one — that is how the smear crosses the edge
        let w = exp(-2.0 * dot(P[i], P[i]));
        sum += s.rg * w; wsum += w;
      }
      textureStore(irrOut, vec2<i32>(gid.xy), vec4<f32>(sum / wsum, c.b, hA));   // sign consumed here; downstream sees plain history
    }
  `;

