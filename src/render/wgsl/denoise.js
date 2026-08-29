  // ── AO HISTORY CEILING ── the SKY/AO channel's own accumulation window, split out from the sun's
  // (u.maxHist) because the two terms are not the same kind of signal. AO is STATIONARY: the geometry a
  // cosine-hemisphere ray hits does not move, so its history could in principle run forever and every extra
  // frame is free convergence. Sun visibility is NOT: the sun creeps every frame, so an edge pixel's true
  // value drifts all day and a long window can only ever lag it. Sharing one counter meant capping the sun
  // forced the same cap onto AO and threw away the convergence the one stationary term could actually keep.
  // 256 is a HARD ceiling and not a taste call: the history texture is rgba16float, so near 1.0 an f16 step is
  // ~0.001 while a 1/256 blend moves the accumulator by ~0.004 of the delta. At 1/512 the increment lands
  // inside the quantisation and the accumulator simply stalls — it stops converging rather than converging
  // slowly, which looks like a frozen, permanently-wrong AO term.
  // Sweep with ?aohist=N. The DEFAULT is 64, which makes this whole change bit-identical to the previous
  // single-counter code (see the equivalence note in TEMPORAL) — the split ships as a mechanism, and the
  // number it is worth is the user's to pick off a measured sweep.
  const AO_HIST = (() => { const m = /[?&]aohist=([0-9.]+)/.exec(location.search); return m ? Math.max(1, Math.min(256, Math.round(parseFloat(m[1])))) : 64; })();

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
            // ── TWO CEILINGS, ONE COUNTER ── the stored .a is now the RAW frame count since the last
            // reject, and each channel divides by its OWN ceiling at use time. That works because the two
            // terms are only ever invalidated TOGETHER — the slot test, the flag bits and the depth test
            // above all reject the whole pixel, never one channel of it — so the count is genuinely shared
            // and only the rate differs. No second counter, no extra texture, no uniform lane.
            let sBase = select(1.0, u.maxHist, LG(6u));              // bit 6 off → both ceilings pinned at 1 = no irradiance history at all
            let aBase = select(1.0, ${AO_HIST}.0, LG(6u));           // …and AO keeps its own, which is why speeding the day/night cycle no longer destroys AO history along with the sun's (tick-camera drops maxHist to 10 at >4x; AO is stationary and has no reason to pay for that)
            // Reactive pixels ease toward a 10-frame floor rather than snapping to 4. Snapping was
            // the flicker: the mask flipped every time a resting trunk woke, and the noise floor
            // jumped with it. 10 rather than 4 because 4 samples of a binary sun ray still sparkle.
            // It caps BOTH terms: a body moving through a pixel changes what the AO ray hits just as much
            // as what the sun ray hits, so the stationarity that earns AO its long window is gone there too.
            let rk = clamp(cur.a, 0.0, 1.0);
            let capS = mix(sBase, min(sBase, 10.0), rk);             // reactive pixel: converge in ~4 frames so a moving shadow TRACKS its body instead of trailing it
            let capA = mix(aBase, min(aBase, 10.0), rk);
            hist = min(abs(prev.a) + 1.0, max(capS, capA));          // the counter runs to the LONGER of the two; each channel clamps it down to its own below
            acc = vec2<f32>(mix(prev.r, cur.r, 1.0 / min(hist, capS)),
                            mix(prev.g, cur.g, 1.0 / min(hist, capA)));
            // EQUIVALENCE: at ?aohist=64 (the default) capS == capA, so hist == min(prev.a + 1, capS) and
            // both blends are 1/hist — exactly the single-counter line this replaced. Bit-identical.
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
      // Since the split this is the RAW count, which can now run past the sun's ceiling to the AO one. The
      // radius below saturates at 8 frames and both ceilings floor at 10, so every pixel that reaches the
      // ramp at all reaches the same end of it: unchanged at any aohist. It is a DISOCCLUSION-freshness
      // signal, not a per-channel noise estimate, which is why one radius still serves both channels — and
      // the taps fetch a whole rgba texel anyway, so filtering them apart would cost real bandwidth to
      // slightly under-blur a term that is low-frequency by construction.
      let hA = abs(c.a);
      // A reactive pixel is held at low history deliberately, not because it is a disocclusion, so the
      // usual "low history => blur hard" rule would smear the very edge we capped history to keep sharp.
      // Filter it at a fixed tight radius instead: a little more grain for a few frames, in exchange for
      // a shadow that stays on the voxels it belongs to.
      let radius = select(mix(${(() => { const m = /[?&]blur=([0-9.]+)/.exec(location.search); return m ? Math.max(1, Math.min(8, parseFloat(m[1]))) : 4.0; })()}, 1.5, clamp(hA / 8.0, 0.0, 1.0)), 2.6, c.a < 0.0);   // see the note above: 8 px was smearing shadows across voxel faces on any surface whose history cannot converge
      // ── SUBPIXEL GEOMETRY FALLBACK ── a pixel that just lost its history has NO temporal average behind
      // it, so this pass is the only thing standing between it and a raw one-ray sample. On a silhouette one
      // pixel wide the face test then rejects all twelve taps: the only neighbours a grass blade has are the
      // ground behind it (a TOP face against the blade's SIDE), so wsum stays at 1.0 and the filter runs and
      // does nothing. That is the grain in the grass, and the history view names it exactly — solid green
      // across the near ground where blades are fat enough to have an interior, and a dense thicket of red
      // in the mid-distance where every blade has gone subpixel and IS its own outline.
      // Note the depth test is NOT what rejects them: at 30 voxels it allows 3.2, and the ground behind a
      // blade is well inside that. The face test is the binding one, which is why this is where the fix goes.
      // So for those pixels only, take a cross-face tap at QUARTER weight instead of dropping it. It biases a
      // blade edge slightly toward the ground it is standing on — the right direction, and a bias worth
      // paying, because the alternative on these pixels is not "less bias", it is no filtering at all.
      // A converged pixel (hA > 4) keeps the hard test, so open ground, trunks and flat faces are untouched.
      // LG2 bit 1 is the A/B and it is LIVE (__vb.lgt2(1) off / __vb.lgt2(3) on) rather than a URL flag,
      // because the world seed differs between loads — a cross-load comparison here is two different forests.
      let xface = hA <= 4.0 && LG2(1u);
      var sum = c.rg; var wsum = 1.0;
      for (var i = 0; i < 12; i++) {
        if (!LG(7u)) { break; }                                  // bit 7 off → raw, unfiltered irradiance
        let q = vec2<i32>(vec2<f32>(gid.xy) + P[i] * radius + 0.5);
        if (q.x < 0 || q.y < 0 || q.x >= i32(u.res.x) || q.y >= i32(u.res.y)) { continue; }
        let s = textureLoad(hist, q, 0);
        let f2 = gbFace(textureLoad(gAlbedo, q, 0).a);
        let fOK = f2 == faceC;
        if (s.b <= 0.0 || abs(s.b - c.b) > 0.04 * c.b + 2.0) { continue; }
        if (!fOK && !xface) { continue; }                        // the old hard reject, still hard for anything that has a history
        if ((c.a < 0.0) != (s.a < 0.0)) { continue; }   // never mix a moving-shadow pixel with a settled one — that is how the smear crosses the edge
        let w = exp(-2.0 * dot(P[i], P[i])) * select(0.25, 1.0, fOK);
        sum += s.rg * w; wsum += w;
      }
      textureStore(irrOut, vec2<i32>(gid.xy), vec4<f32>(sum / wsum, c.b, hA));   // sign consumed here; downstream sees plain history
    }
  `;

