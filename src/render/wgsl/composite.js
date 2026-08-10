  const COMPOSITE_SRC = ({ DDAW, pickWGSL }) => /* wgsl */`
    @group(0) @binding(1) var gAlbedo : texture_2d<f32>;
    @group(0) @binding(2) var irrF : texture_2d<f32>;
    @group(0) @binding(3) var colorOut : texture_storage_2d<rgba8unorm, write>;
    @group(0) @binding(16) var<storage, read> world : array<u32>;   // world + occupancy for the shared DDA — creature pixels trace a REAL sun ray
    @group(0) @binding(17) var<storage, read> bricks : array<u32>;  // so moving things sit in tree shade exactly like the static terrain
    @group(0) @binding(18) var<storage, read> bricks2 : array<u32>;
    @group(0) @binding(19) var<storage, read> pal : array<vec4<f32>>;   // palette — the traced water reflection/refraction shades its hits here
    @group(0) @binding(20) var<storage, read> wbricks : array<u32>;     // water-only brick bits — skipW rays stride these
    @group(0) @binding(23) var<storage, read> bodyVox : array<u32>;  // ── RIGID BODIES ── shared with TRACE; DDAW's bodyTrace() reads it
    @group(0) @binding(21) var slotT : texture_2d<u32>;             // ── DYNAMIC LIFE ── TRACE's per-pixel creature id + hit-axis bits (see slotOut) — true-normal reconstruction + debug views
    ${DDAW}
    ${pickWGSL}
    // ── DEPTH OF FIELD ── the signed circle of confusion for a surface d voxels away. Written into colorOut.a
    // and gathered by the BLIT. Thin-lens shape (1 - focus/d): exactly 0 on the focal plane, saturating at +1
    // however far past it you go — the sky included — and going negative in front of it. The dead zone holds a
    // band either side of the focus perfectly sharp, so the picture has somewhere to BE in focus rather than
    // being softened everywhere by a fraction of a pixel; and the near side is scaled down because a lens blurs
    // the foreground far harder than the background, which at voxel scale reads as a smear over half the screen
    // rather than as depth.
    const DOF_DEAD : f32 = 0.16;                                     // fraction of the 0..1 range that stays sharp either side of the focal plane
    const DOF_NEAR : f32 = 0.55;                                     // foreground blur as a fraction of the background's
    fn dofCoc(d : f32) -> f32 {
      if (u.dof.x <= 0.0) { return 0.0; }
      let dd = select(u.dof.x * 64.0, d, d > 0.0);                   // d < 0 is SKY: infinitely far, so it sits at the far stop
      var c = 1.0 - u.dof.x / max(dd, 0.05);
      c = sign(c) * max(abs(c) - DOF_DEAD, 0.0) / (1.0 - DOF_DEAD);
      c = clamp(c, -1.0, 1.0);
      return select(c, c * DOF_NEAR, c < 0.0);
    }
    fn vn3(p : vec3<f32>) -> f32 {
      let f = floor(p); let i = vec3<i32>(f);
      var w = p - f; w = w * w * (3.0 - 2.0 * w);
      let a = mix(ih3(i.x, i.y, i.z), ih3(i.x + 1, i.y, i.z), w.x);
      let b = mix(ih3(i.x, i.y, i.z + 1), ih3(i.x + 1, i.y, i.z + 1), w.x);
      let c = mix(ih3(i.x, i.y + 1, i.z), ih3(i.x + 1, i.y + 1, i.z), w.x);
      let d = mix(ih3(i.x, i.y + 1, i.z + 1), ih3(i.x + 1, i.y + 1, i.z + 1), w.x);
      return mix(mix(a, b, w.z), mix(c, d, w.z), w.y);
    }
    fn cloudDen(p : vec3<f32>) -> f32 {                              // cumulus deck between y 480–800, wind-drifted
      let hf = clamp((p.y - 480.0) / 320.0, 0.0, 1.0);
      let q = vec3<f32>(p.x + u.time * 9.0, p.y, p.z + u.time * 3.5) * 0.0021;
      let o1 = vn3(q * vec3<f32>(1.0, 2.4, 1.0));
      if (o1 * 0.60 + 0.40 < 0.565) { return 0.0; }                  // EXACT short-circuit: octaves 2+3 sum to at most 0.28+0.12, so below this bound the
      var n = o1 * 0.60 + vn3(q * 2.6 + vec3<f32>(7.7)) * 0.28 + vn3(q * 6.1 + vec3<f32>(19.3)) * 0.12;   // clamp gate at 0.565 CANNOT open — same result, two of three noise octaves skipped
      n = n * smoothstep(0.0, 0.16, hf) * smoothstep(1.0, 0.70, hf);
      return clamp((n - 0.565) * 3.4, 0.0, 1.0);                     // ~35% coverage — scattered cumulus, plenty of blue
    }
    @group(0) @binding(22) var<storage, read> visb : array<u32>;     // per-8×8-tile drop-slot visibility bitmask (4×u32/tile) — computed ONCE per frame by the VIS prepass and shared with TRACE (this pass used to recompute it per workgroup behind a barrier)
    // ── VOLUMETRIC LIGHT ── march the camera ray, gathering in-scatter from the emissive point lights
    // and testing visibility from each step to the light. Teardown's method; see the call site for how
    // this differs from theirs.
    fn volLight(ro : vec3<f32>, rd : vec3<f32>, tMax : f32, dither : f32) -> vec3<f32> {
      var acc = vec3<f32>(0.0);
      var far = min(tMax, 90.0);
      if (far <= 0.2) { return acc; }
      // ── CULL AGAINST THE RAY ── one dot product per light drops every one the ray never passes near,
      // and narrows the march to the span that can actually receive anything.
      var mask = 0u; var lo = far; var hi = 0.0;
      for (var f = 0; f < 8; f++) {
        let F = u.fflies[f];
        if (F.w <= 0.0) { continue; }
        let dv = F.xyz - ro;
        let tc = clamp(dot(dv, rd), 0.0, far);                   // closest approach along the ray
        if (dot(dv, dv) - tc * tc > 900.0) { continue; }         // the ray never comes within reach of this light
        mask |= (1u << u32(f));
        lo = min(lo, max(tc - 30.0, 0.0));                       // …and only this span of the ray matters
        hi = max(hi, min(tc + 30.0, far));
      }
      if (mask == 0u || hi <= lo) { return acc; }                // nothing on this ray — the common case, and it costs 8 dots
      let STEPS = 8;                                             // fewer steps than before; the dither and TAA carry the smoothing
      let dt = (hi - lo) / f32(STEPS);
      for (var i = 0; i < STEPS; i++) {
        let ps = ro + rd * (lo + (f32(i) + dither) * dt);
        // strongest contributor at this point — ONE trace serves it, as the surface glow term does
        var bw = 0.0; var bi = -1; var bd2 = 0.0;
        for (var f = 0; f < 8; f++) {
          if ((mask & (1u << u32(f))) == 0u) { continue; }
          let F = u.fflies[f];
          let dv = F.xyz - ps;
          let d2 = dot(dv, dv);
          if (d2 > 900.0) { continue; }
          let w = F.w / (1.0 + d2 * 0.05) * (1.0 - smoothstep(400.0, 900.0, d2));
          if (w > bw) { bw = w; bi = f; bd2 = d2; }
        }
        if (bi < 0) { continue; }
        let dvB = u.fflies[bi].xyz - ps;
        let dist = sqrt(max(bd2, 1e-4));
        let sh = traceAll(ps, dvB / dist, max(dist - 0.7, 0.0), true);   // the visibility ray the article describes
        if (sh.t >= 0.0) { continue; }                           // solid between the medium and the light — this step is in shadow
        acc += vec3<f32>(1.0, 0.34, 0.16) * bw;                  // warm: everything in this lane is an ember, a firefly or a wound
      }
      return acc * dt * 0.010;                                   // scatter per unit length, HALVED (user) — the march integrates it
    }
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>, @builtin(workgroup_id) wgid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let px = vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y);
      let rd = rayDir(px);
      let alb4 = textureLoad(gAlbedo, vec2<i32>(gid.xy), 0);
      let face = u32(alb4.a * 255.0 + 0.5) & 15u;
      let lavaG = f32(u32(alb4.a * 255.0 + 0.5) >> 4u) / 14.0;
      var col : vec3<f32>;
      // ── SURFACE TERMS, HOISTED ── the creature/drop path far below composites submerged hits through the
      // water, and to do that the way the BED does it needs the same three numbers the bed uses: the water's
      // own in-scatter glow, the mirror, and the Fresnel split between them. Without them it was faking the
      // whole thing by fading the creature toward the finished SURFACE colour, which already has the sky
      // reflection mixed in -- so a fish saturated toward a bright grey and read as a pale blob, brighter
      // than the water it was supposedly inside.
      var waterScat = vec3<f32>(0.0);
      var waterRefl = vec3<f32>(0.0);
      var waterFres = 0.0;
      var waterIceC = vec3<f32>(0.0);                                // …and the ICE the surface turned into, with the amount of it: a submerged fish has to hide behind the SAME sheet the lakebed hides behind (user 2026-08-08)
      var waterIceK = 0.0;
      if (u.misc.w > 0.5) {                                          // ── EYE INSIDE A VOXEL ── show the MATERIAL you are buried in as chunky pixels, not a black void
        let pk = u32(u.misc.w + 0.5) - 1u;                           // packed sRGB of that voxel: stone, needle green, trunk brown…
        let base = pow(vec3<f32>(f32(pk & 255u), f32((pk >> 8u) & 255u), f32((pk >> 16u) & 255u)) / 255.0, vec3<f32>(2.2));
        let bq = vec2<i32>(vec2<f32>(gid.xy) * 0.04);                // ~24 px blocks — 4x coarser than the first pass (user), so it reads as chunky voxel material
        var hh = u32(bq.x) * 374761393u + u32(bq.y) * 668265263u;    // (ivhash lives in TRACE, not PRE — this is the same mix, inlined)
        hh = (hh ^ (hh >> 13u)) * 1274126177u;
        let h1 = f32((hh ^ (hh >> 16u)) & 1023u) / 1023.0;
        var hc = u32(bq.x >> 2) * 2246822519u + u32(bq.y >> 2) * 374761393u;   // a coarser second octave breaks up the regular grid
        hc = (hc ^ (hc >> 13u)) * 668265263u;
        let h2 = f32((hc ^ (hc >> 16u)) & 1023u) / 1023.0;
        let k = 0.30 + 0.42 * h1 + 0.24 * h2;                        // per-block shade — you are INSIDE it, so it stays dim whatever the material
        textureStore(colorOut, vec2<i32>(gid.xy), vec4<f32>(sqrt(base * k), 0.5));   // alpha 0.5 = a circle of confusion of ZERO: your eye is inside the voxel, there is no depth here to blur by
        return;
      }
      if (face == 7u) {
        col = skyColor(rd);
        if (rd.y > 0.02) {                                           // VOLUMETRIC CLOUDS — raymarched slab, beer-lambert with a sun tap
          let camY = u.camPos.y;
          let t0c = (480.0 - camY) / rd.y;
          let t1c = (800.0 - camY) / rd.y;
          let ta = max(min(t0c, t1c), 0.0);
          let tb = min(max(t0c, t1c), 12000.0);
          if (tb > ta) {
            let roW = vec3<f32>(u.camPos.x + u.winO.x, camY, u.camPos.z + u.winO.y);
            let dtc = (tb - ta) / 18.0;
            var T = 1.0;
            var acc = vec3<f32>(0.0);
            var seedC = ((gid.x * 1447u) ^ (gid.y * 8191u) ^ (u32(u.frame) * 2657u)) | 1u;
            var tcc = ta + dtc * rand(&seedC);                       // jittered start — kills the slab step-banding
            for (var ci = 0; ci < 18; ci++) {
              let p = roW + rd * tcc;
              let d = cloudDen(p);
              if (d > 0.002) {
                let li = 0.25 + 0.75 * exp(-cloudDen(p + u.sunDir * 60.0) * 2.4);   // one-tap self-shadow toward the sun
                let cc = sunTint() * li * 0.55 + mix(HORIZON, ZENITH, 0.4) * 0.55 * dayScale();
                let a = 1.0 - exp(-d * dtc * 0.02);
                acc += T * a * cc;
                T = T * (1.0 - a);
                if (T < 0.05) { break; }
              }
              tcc += dtc;
            }
            let fade = exp(-ta * 0.00035);                           // distant decks melt into the horizon haze
            col = mix(col, acc + col * T, fade);
          }
        }
      } else if (face == 8u) {                                       // LAVA: emissive — burns through the fog
        let irr = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        let alb = alb4.rgb * alb4.rgb;
        col = alb * (3.1 + 0.6 * sin(u.time * 2.1)) + vec3<f32>(0.65, 0.13, 0.0);   // molten orange, glowing, gentle pulse
        var fogA = 1.0 - exp(-irr.b * 0.0006);
        fogA = max(fogA, smoothstep(u.rdist.x - 72.0, u.rdist.x - 6.0, irr.b * length(vec2<f32>(rd.x, rd.z))));
        if (!LG(5u)) { fogA = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
        col = mix(col, skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogA);
      } else if (face == 6u) {                                       // ── PHYSICALLY-BASED WATER ── Gerstner surface, RAY-TRACED reflection + refraction, Beer–Lambert absorption + single scattering. The voxel aesthetic survives on purpose: the surface is still stepped 10 cm columns, the mirror image is the voxel world itself, glints stay discrete.
        let irr = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        let alb = alb4.rgb * alb4.rgb;
        let tWat = irr.b;
        let pw2 = u.camPos + rd * tWat;
        let wx2 = pw2.x + u.winO.x; let wz2 = pw2.z + u.winO.y;
        let nW = select(vec3<f32>(0.0, 1.0, 0.0), gerstN(wx2, wz2), LG(23u));   // bit 23: WATER WAVES — off = a flat mirror plane, so wave shape can be told apart from wave lighting                                   // the GERSTNER normal — same field that raises the voxel crests, crest-pinched by the Q term
        let foamW = smoothstep(0.16, 0.50, alb.g) * (1.0 - u.pickZ.w);   // foam carries a bright albedo → shade it as SURFACE, not window (ice path handles frozen)
        let cosI = clamp(-dot(rd, nW), 0.02, 1.0);
        let fres = 0.02 + 0.98 * pow(1.0 - cosI, 5.0);               // true Schlick, F0 = 0.02 (air→water)
        let sunW = smoothstep(-0.02, 0.12, u.sunDir.y) * irr.r;      // sun above horizon AND this surface point actually sees it
        // ── RAY-TRACED REFLECTION ── the mirrored ray walks the real voxel scene; misses (and far hits) fall back to the sky.
        // PERF (user: "fps tanks"): the killer was GRAZING views over big lakes — every distant water pixel launched a ray
        // that SKIMS the surface for hundreds of voxels. Now only NEAR water traces (≤110 — where mirrored trees actually
        // resolve; beyond that the mirror is sub-pixel mush and sky+glitter reads identically), near-vertical views skip
        // (fres ≈ 0.02 → the mirror is invisible anyway), and the cap+fade tightened so the trace never outlives its
        // visible contribution. Same look where it counts, a fraction of the rays.
        var refl = reflect(rd, nW);
        if (refl.y < 0.03) { refl = normalize(vec3<f32>(refl.x, 0.06 - refl.y * 0.5, refl.z)); }   // a grazing mirror ray that would dive back under the surface folds just above it — no self-hit acne
        var reflC = skyBase(refl);
        if (tWat < 110.0 && fres > 0.045) {
          let rh = traceAll(pw2 + vec3<f32>(0.0, 0.06, 0.0), refl, 140.0, true);   // skipW: the folded mirror ray can never re-enter the flat water plane (crests live in the analytic field, not the grid) — output-identical, and it STRIDES the water-only bricks it skims instead of fine-stepping them
          if (rh.t >= 0.0) {
            let rpos = pw2 + refl * rh.t;
            let rvc = vec3<i32>(floor(rpos - rh.n * 0.01)) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));
            let ralb = pal[rh.vox].rgb * (0.88 + 0.24 * ivhash(rvc));
            let rlit = sunTint() * (max(dot(rh.n, u.sunDir), 0.0) * 0.9 * irr.r) + mix(HORIZON, ZENITH, 0.5 + 0.5 * rh.n.y) * 0.95 * dayScale() + vec3<f32>(0.012, 0.013, 0.016);
            reflC = mix(ralb * rlit, skyBase(refl), 1.0 - exp(-rh.t * 0.014));   // the mirror fades into sky with distance, like the world fades into haze
          }
        }
        // ── REFRACTION + BEER–LAMBERT ── the transmitted ray bends by Snell (η = 1/1.33) and marches to the bed; what
        // comes back is absorbed per-channel over the traveled water path — red dies first, blue carries.
        // PERF: capped at 34 (beyond that Beer–Lambert leaves <3% — invisible), and skipped entirely at grazing angles
        // (fres > 0.8 → the Fresnel split hands nearly everything to the mirror; the in-scatter constant stands in).
        let sigT = WATER_SIG;                                        // extinction per voxel (10 cm) — 20% MORE TRANSPARENT (user 2026-08-06): every channel scaled by 0.8, so the RATIO is untouched and the water keeps its colour (red still dies first, blue carries) while you see 25% deeper before the same amount is absorbed. Was (0.30, 0.115, 0.052).
        let scatC = vec3<f32>(0.018, 0.070, 0.092) * (0.30 + 0.70 * sunW) * dayScale() * (0.45 + 0.55 * irr.g);   // single-scatter source — the water's own glow, lit by sun + sky
        var refrC = scatC;                                           // no bed within reach → the column saturates to pure in-scatter
        if (fres < 0.80 && LG(19u)) {                                // bit 19: WATER REFRACTION — off leaves refrC at the pure in-scatter colour, no bed, no Beer-Lambert
          var refr = refract(rd, nW, 0.752);
          if (dot(refr, refr) < 1e-5) { refr = rd; }                 // grazing/TIR fallback: continue straight
          let bh = traceAll(pw2 + rd * 0.02, refr, 34.0, true);
          if (bh.t >= 0.0) {
            let bpos = pw2 + rd * 0.02 + refr * bh.t;
            let bvc = vec3<i32>(floor(bpos - bh.n * 0.01)) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));
            let ca = caust(floor(vec2<f32>(bpos.x + u.winO.x, bpos.z + u.winO.y)) + vec2<f32>(0.5));   // caustic webs dance on the refracted bed
            let balb = pal[bh.vox].rgb * (0.88 + 0.24 * ivhash(bvc)) * (1.0 + 1.6 * ca * sunW);
            let blit = (0.45 + 0.55 * irr.g) * dayScale() * (0.55 + 0.45 * irr.r);
            let trB = exp(-sigT * bh.t);                             // Beer–Lambert over the in-water path
            refrC = balb * blit * trB + scatC * (vec3<f32>(1.0) - trB);
          }
        }
        waterScat = scatC; waterRefl = reflC;                        // …the creature path below composites against these, so a fish sits in the same water the bed sits in
        waterFres = clamp(select(0.0, fres * u.lgt.y, LG(18u)), 0.0, 1.0);
        // energy split by Fresnel — transmission vs mirror. Bit 18: WATER REFLECTION — off hands the whole
        // surface to refraction. u.lgt.y is the panel's REFLECTION STRENGTH slider (1 = physical Schlick).
        col = mix(refrC, reflC, clamp(select(0.0, fres * u.lgt.y, LG(18u)), 0.0, 1.0));
        col = mix(col, alb * (0.55 + 0.45 * irr.g) * (0.60 + 0.40 * irr.r) * dayScale() * 1.30, select(0.0, foamW, LG(20u)));   // bit 20: WATER FOAM   // FOAM voxels stay bright chunky surface
        if (u.pickZ.w > 0.015 && LG(21u)) {                           // bit 21: WATER ICE — off keeps the surface liquid-looking however frozen it is.   // FREEZING/FROZEN — the ice look BLENDS in over ~25 s as the lake freezes, and back out as it thaws
          let nI = vec3<f32>(0.0, 1.0, 0.0);
          let fresI = 0.03 + 0.22 * pow(1.0 - clamp(-dot(rd, nI), 0.0, 1.0), 4.0);
          let frost = 0.9 + 0.2 * fract(sin(floor(wx2) * 12.9898 + floor(wz2) * 78.233) * 43758.5453);
          var iceC = mix(alb, vec3<f32>(0.74, 0.81, 0.90), 0.62) * frost * (0.5 + 0.5 * irr.g) * dayScale() + skyBase(reflect(rd, nI)) * fresI;
          // -- ICE GLISTEN (same lgt.x bit 22 as the liquid one) -- the frozen surface wears the SAME discrete 10 cm cube
          // glint the water wears (user 2026-08-09: "make the ice glisten like the water does"). Same cell grid, same phase
          // and pick hashes, same duty window, same reflection column off the same flat surface normal -- so as a lake skins
          // over, the glitter path stays exactly where it was and only its brightness key changes (see sparkCI). Folded into
          // iceC BEFORE waterIceC is captured, so a fish sealed under the sheet is covered by GLINTING ice rather than by a
          // dull fish-shaped patch of it -- the same trap the 2026-08-08 refraction bug fell into.
          // A per-cell frost-facet tilt was tried here first and removed: scattering the mirror ray by +-30 deg drops
          // pow(., 26) to a few percent on every cell but the untilted ones, leaving a sparse speckle, not a glitter path.
          if (LG(22u)) {
            let gkI = select(smoothstep(-0.02, 0.10, u.sunDir.y), 0.6, isMoon());
            if (gkI > 0.01) {
              let cellI = floor(vec2<f32>(wx2, wz2));                 // one glint cell = one 10 cm voxel, exactly as on water
              let columnI = pow(max(dot(reflect(rd, nI), u.sunDir), 0.0), 26.0);   // the liquid glint's reflection column, unchanged -- and nI is the same flat normal the ice above is shaded with
              let phI = fract(sin(cellI.x * 91.7 + cellI.y * 47.3) * 4321.7) * 6.2831853;   // SAME phase + pick hashes as the liquid glint, so the crossfade below hands the surface over without the lit cells jumping to a different set
              let twI = sin(u.time * 1.6 + phI) * 0.5 + 0.5;
              let pickI = step(0.5, fract(sin(cellI.x * 12.9898 + cellI.y * 78.233) * 43758.5453));
              let sparkI = smoothstep(0.30, 0.85, twI) * pickI;       // the same wide 0.30-0.85 duty window: a glint that arrives and leaves, never blinks
              let sparkCI = max(select(vec3<f32>(1.30, 1.34, 1.42), vec3<f32>(1.15, 1.18, 1.30), isMoon()) * (0.35 + 0.65 * dayScale()), iceC * 3.4);   // -- THE WATER GLINT COLOUR, FLOORED UP TO 3.4x THE ICE UNDER IT -- the liquid glint mixes ~1.35 into a DARK blue surface, so it pops. Daylight ice is already ~1.8 linear (206/255 after ACES, essentially clipped white) and mixing 1.35 into that is invisible: measured p99 210 -> 210, the term did nothing. Keying the spark to the surface fixes both ends at once. In daylight it rises to ~6 and the glitter path reads as bright cubes on white; by moonlight the ice is dark again, the floor takes over, and it lands on the water's own night value instead of the blizzard of white static a fixed daylight constant produced there.
              iceC = mix(iceC, sparkCI, sparkI * columnI * gkI * 0.85 * irr.r);
            }
          }
          waterIceC = iceC; waterIceK = min(1.0, u.pickZ.w * 1.12);   // ── AND THE CREATURE PATH BELOW MUST SEE THIS ── the ice used to be mixed into the surface AFTER waterScat/waterRefl/waterFres were captured, so a fish under a frozen lake was still composited through LIQUID water: at 12 voxels' depth Beer-Lambert ate it down to the water's own in-scatter blue and painted that fish-shaped patch of lake over a white ice sheet. It read as a hole punched in the ice (user 2026-08-08: "the visuals are messed up when the fish are frozen"). The bed is fully hidden at freezeK 1, so the fish must be too.
          col = mix(col, iceC, waterIceK);
        }
        // ── PIXEL GLISTEN (lgt.x bit 22) ── discrete 10 cm cubes flashing on and off (the engine.html look).
        // It used to have a companion, a smooth SOFT sheen on lgt.z bit 0, sharing this same light column;
        // that one is gone (user 2026-08-09) and the column now serves the pixel glint alone.
        if (waterIceK < 0.998 && LG(22u)) {                            // ...and the LIQUID glint fades out exactly as fast as the ice glint above fades in (1 - waterIceK), so a freezing lake never loses its sparkle for a moment. It used to hard-cut at freezeK 0.4 and go dead until the thaw.
          let gk = select(smoothstep(-0.02, 0.10, u.sunDir.y), 0.6, isMoon());
          if (gk > 0.01) {
            let column = pow(max(dot(refl, u.sunDir), 0.0), 26.0);   // reflection column toward the light — the glint lives ONLY inside this
            let cell = floor(vec2<f32>(wx2, wz2));                 // one glint cell = one 10 cm voxel
            let ph = fract(sin(cell.x * 91.7 + cell.y * 47.3) * 4321.7) * 6.2831853;   // per-voxel random phase
            let tw = sin(u.time * 1.6 + ph) * 0.5 + 0.5;   // glint twinkle halved to match
            let pick = step(0.5, fract(sin(cell.x * 12.9898 + cell.y * 78.233) * 43758.5453));   // ~half the voxels participate — scattered, not a sheet
            // ── NO GAPS (user 2026-08-05: "it changes its pattern, removes itself, then changes again") ──
            // the window used to be smoothstep(0.82, 1.0): a cell sat above 0.82 for only 28% of its 3.9 s
            // cycle and reached full brightness for a sliver of that, so at any instant barely 5% of the
            // water carried a glint. The eye reads that as a patch lighting up, going out, and a DIFFERENT
            // patch lighting up — the gap is simply the off part of a very low duty cycle. Widened to
            // 0.30-0.85: lit 63% of the cycle instead of 28%, and the long ramp means a cell fades in and
            // out rather than snapping, so the field is continuously populated and the transitions blend.
            let spark = smoothstep(0.30, 0.85, tw) * pick;         // a soft rise and fall → a glint that arrives and leaves, never blinks
            let sparkC = select(vec3<f32>(1.35, 1.25, 1.0), vec3<f32>(1.15, 1.18, 1.3), isMoon()) * (0.35 + 0.65 * dayScale());
            col = mix(col, sparkC, spark * column * gk * 0.85 * irr.r * (1.0 - waterIceK));   // translucent voxel glint (mix, not add — reads as a bright cube on the surface)
          }
        }
        var fogA = 1.0 - exp(-irr.b * 0.0006);
        fogA = max(fogA, smoothstep(u.rdist.x - 72.0, u.rdist.x - 6.0, irr.b * length(vec2<f32>(rd.x, rd.z))));
        if (!LG(5u)) { fogA = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
        col = mix(col, skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogA);
      } else {
        let irr = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        let alb = alb4.rgb * alb4.rgb;
        var n = faceN(face);
        let slRaw = textureLoad(slotT, vec2<i32>(gid.xy), 0).r;
        if ((slRaw & 255u) != 0u) {                                  // ── DYNAMIC LIFE ── a trace-injected creature: rebuild its TRUE rotated normal from the slot's
          let s4 = (i32(slRaw & 255u) - 1) * 4;                      // model axes + the stored hit-axis bits, so shading doesn't quantize to world axes and pop as it turns
          let ax = (slRaw >> 8u) & 7u;
          var nl = vec3<f32>(0.0);
          let sv = select(-1.0, 1.0, (ax & 1u) != 0u);
          if ((ax >> 1u) == 0u) { nl.x = sv; } else if ((ax >> 1u) == 1u) { nl.y = sv; } else { nl.z = sv; }
          let nc = dropV(s4 + 1).xyz * nl.x + dropV(s4 + 2).xyz * nl.y + dropV(s4 + 3).xyz * nl.z;
          n = normalize(u.right * nc.x + u.up * nc.y + u.fwd * nc.z);
        }
        let direct = sunTint() * (irr.r * max(dot(n, u.sunDir), 0.0));
        let skyIrr = mix(HORIZON, ZENITH, 0.5 + 0.5 * n.y) * 0.95 * dayScale();
        let bounce = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * n.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());
        col = alb * (direct + (skyIrr + bounce) * irr.g + vec3<f32>(0.012, 0.013, 0.016));   // faint cave ambient
        // ── BACK-LIT FOLIAGE ── a needle is thin enough to pass light, and the whole reason a forest reads as
        // a forest when you look toward the sun is that the canopy GLOWS rather than silhouetting flat. Land
        // shading is pure lambert, so until now a leaf facing away from the sun was simply dark.
        // Two factors, and both have to be present: tr is the forward-scatter lobe, so this only fires when
        // you are looking INTO the sun, and wrap is how far the face is turned AWAY from it, which is exactly
        // the side light has to travel through. irr.r carries whether the sun actually reaches the far side —
        // the TRACE gate above was widened to shoot that ray for leaves, or it would be zero on every one of
        // these pixels and nothing would ever glow. So a leaf deep inside a crown stays dark and only the
        // canopy edge lights up, which is the real behaviour.
        if (FOLBACK && ((slRaw >> 11u) & 1u) != 0u) {
          let tr = pow(max(dot(rd, u.sunDir), 0.0), FOL_LOBE);      // rd runs FROM the eye INTO the scene, so looking toward the sun is dot(rd, sunDir) -> 1. Do NOT flip this: dot(-rd, ...) peaks when the sun is BEHIND you, which is exactly when wrap is 0, so the two factors can never both be large and the whole term goes dead.
          let wrap = clamp(-dot(n, u.sunDir), 0.0, 1.0);            // …how far the face is turned AWAY from the sun, which is the side the light has to travel through
          col += alb * vec3<f32>(1.15, 1.35, 0.70) * sunTint() * (tr * wrap * irr.r * FOL_STR);   // transmitted light is warmer and more saturated than the reflected colour — it has been filtered by the leaf. irr.r is what keeps a leaf deep inside a crown dark: see the sunOrg note in TRACE, without which that term is zero on every pixel this fires on.
        }
        let glowY = u.camPos.y + rd.y * irr.b;                                                   // the shared 4-bit glow field: bedrock hits = LAVA orange, surface hits = FIREFLY warm yellow
        if (glowY < 28.0) { col += alb * lavaG * vec3<f32>(1.0, 0.44, 0.13) * 3.6; }             // lava: linear decode, unchanged
        else { col += alb * lavaG * lavaG * vec3<f32>(1.0, 0.82, 0.30) * 4.6; }                  // firefly: sqrt-encoded in TRACE → SQUARED decode restores the physical falloff curve
        var fogA = 1.0 - exp(-irr.b * 0.0006);
        fogA = max(fogA, smoothstep(u.rdist.x - 72.0, u.rdist.x - 6.0, irr.b * length(vec2<f32>(rd.x, rd.z))));
        if (!LG(5u)) { fogA = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
        col = mix(col, skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogA);
      }
      // Distance to the nearest FOREGROUND surface drawn over the g-buffer (creature, drop, held item), or -1 if the scene
      // itself is what you see. The UNDERWATER block at the bottom needs this: it attenuates by the in-water path, and using
      // the SCENE's depth for a pixel covered by a fish 1.4 m away absorbed that fish as if it were the far lakebed.
      var fgT = -1.0;
      var dofHeld = false;                                         // …and whether that foreground surface is the TOOL IN YOUR HANDS, which depth of field leaves alone (see the store at the end of main)
      if (ITEMN > 0) {                                             // DROPPED ITEMS — hovering, spinning, world-scale voxels, depth-tested against the scene
        let ib = textureLoad(irrF, vec2<i32>(gid.xy), 0).b;
        var bestT = select(1e9, ib, ib > 0.0);
        var waterT = -1.0;                                           // primary hit is a WATER surface → its distance; a drop beyond it is UNDERWATER and composites THROUGH the surface (user: see the fish)
        var waterCol = col;                                          // the shaded surface color (body + sheen + glitter) — what the underwater hit blends toward with depth
        if (face == 6u && ib > 0.0) { waterT = ib; bestT = ib + 48.0; }   // extend the occlusion bound into the water — same 48-vox see-through range as the translucent bed
        let ndc3 = (px / u.res) * 2.0 - 1.0;
        let dc3 = normalize(vec3<f32>(ndc3.x * u.tanH * u.aspect, -ndc3.y * u.tanH, 1.0));
        let dropN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);             // JS COMPACTS live creatures into consecutive slots from 9 and passes the count — the loop never wastes pixels on empty slots (the fixed 64 loop tanked fps over busy water)
        let tiV = (wgid.y * ((u32(u.res.x) + 7u) / 8u) + wgid.x) * ${VIS_W}u;   // this workgroup IS one 8×8 tile — read its four prepass mask words (under ?uni the stride is 8: words 0-3 primary, 4-7 the grown SECONDARY mask)
        let visM0 = visb[tiV]; let visM1 = visb[tiV + 1u]; let visM2 = visb[tiV + 2u]; let visM3 = visb[tiV + 3u];   // FOUR words now (128 slots) and all four stay in REGISTERS: re-fetching the word from storage per iteration measured 4× the per-slot cost
        for (var di = 0; di < dropN; di++) {                         // slots 0..3 = dropped items, slot 4 = the flying cardinal, slots 5..8 = clash sparks, 9+ = live creatures (compacted)
          { let mw = select(select(visM0, visM1, di >= 32), select(visM2, visM3, di >= 96), di >= 64); let mrem = mw >> (u32(di) & 31u); if (mrem == 0u) { di = i32(u32(di) | 31u); continue; } if ((mrem & 1u) == 0u) { di += i32(countTrailingZeros(mrem)) - 1; continue; } }   // ── TILE CULL, BIT-SCANNED ── same mask, same slots visited, but the loop JUMPS to the next slot whose sphere touches this 8×8 tile rather than testing them one at a time. The mask words stay in registers on purpose: re-fetching the word from storage each iteration measured 4× the per-slot cost.
          if (u.lifeCfg.y > 0.5 && (di == 4 || di >= 25) && face != 6u && (u32(lifeMotV(di).w + 0.5) & 1u) == 0u) { continue; }   // ── DYNAMIC LIFE ── trace-injected creatures were ALREADY drawn by TRACE with full SVGF; the analytic path only remains for pixels that look THROUGH a water surface (Beer–Lambert) and for the analytic-flagged slots (fireflies). Creature base → 25 (20 death-burst slots 5-24: 4 sparks + 16 individual smoke voxels, user).
          let dXv = dropV(di * 4 + 1);
          let dit = i32(dXv.w + 0.5);
          if (dit < 1) { continue; }                                 // itemId checked FIRST — an empty slot costs one uniform load, not four
          let dA = dropV(di * 4); let dYv = dropV(di * 4 + 2); let dZv = dropV(di * 4 + 3);
          let it3 = clamp(dit - 1, 0, ITEMN - 1);
          let eW = ITEMD[it3].x; let eD = ITEMD[it3].y; let eH = ITEMD[it3].z; let eOff = ITEMD[it3].w;
          if (eW < 1) { continue; }
          let vsD = dA.w;
          let ew2 = f32(eW) * 0.5; let ed2 = f32(eD) * 0.5; let eh2 = f32(eH) * 0.5;
          let radD = vsD * (sqrt(ew2 * ew2 + ed2 * ed2 + eh2 * eh2) + 1.0);
          let tcD = dot(dA.xyz, dc3);
          if (tcD <= 0.0 || tcD - radD > bestT || length(dc3 * tcD - dA.xyz) > radD) { continue; }
          let roD = vec3<f32>(-dot(dA.xyz, dXv.xyz), -dot(dA.xyz, dYv.xyz), -dot(dA.xyz, dZv.xyz)) / vsD + vec3<f32>(ew2, ed2, eh2);
          var rdD = vec3<f32>(dot(dc3, dXv.xyz), dot(dc3, dYv.xyz), dot(dc3, dZv.xyz));
          if (abs(rdD.x) < 1e-6) { rdD.x = 1e-6; }
          if (abs(rdD.y) < 1e-6) { rdD.y = 1e-6; }
          if (abs(rdD.z) < 1e-6) { rdD.z = 1e-6; }
          let invD = 1.0 / rdD;
          let taD = -roD * invD;
          let tbD = (vec3<f32>(f32(eW), f32(eD), f32(eH)) - roD) * invD;
          let tnD = min(taD, tbD); let tfD = max(taD, tbD);
          let teD = max(max(tnD.x, tnD.y), max(tnD.z, 0.0));
          let tlD = min(min(tfD.x, tfD.y), tfD.z);
          if (teD >= tlD) { continue; }
          var vaxD = 0;
          if (tnD.y == teD) { vaxD = 1; } if (tnD.z == teD) { vaxD = 2; }
          var vcD = clamp(vec3<i32>(floor(roD + rdD * (teD + 1e-4))), vec3<i32>(0), vec3<i32>(eW - 1, eD - 1, eH - 1));
          let istD = vec3<i32>(sign(rdD));
          var vNxD = (vec3<f32>(vcD + max(istD, vec3<i32>(0))) - roD) * invD;
          var tHit = teD;
          var iMapD = eOff + vcD.x + vcD.y * eW + vcD.z * eW * eD;   // running flat index — one add per DDA step
          for (var i = 0; i < PICKSTEPS; i++) {
            let cell = ITEMMAP[u32(iMapD)];
            if (cell.w > 0.5) {
              if (tHit * vsD < bestT) {                              // scene + nearer-drop occlusion
                bestT = tHit * vsD;
                fgT = bestT;                                         // this pixel now shows a creature/drop — the underwater pass must absorb over THIS distance
                var nl = vec3<f32>(0.0);
                if (vaxD == 0) { nl.x = -f32(istD.x); } else if (vaxD == 1) { nl.y = -f32(istD.y); } else { nl.z = -f32(istD.z); }
                let nc = dXv.xyz * nl.x + dYv.xyz * nl.y + dZv.xyz * nl.z;
                let nw = u.right * nc.x + u.up * nc.y + u.fwd * nc.z;
                if (di == 4 || di >= 25) {                           // the CARDINAL + ALL WORLD CREATURES incl. worms + lily pads — the EXACT world surface model (shadowed sun + sky + bounce + fog). Creature base → 25 (20 death-burst slots 5-24).
                  let behind = col;                                  // scene color behind this creature — the translucent wing blend needs it
                  var sunC = 0.0;                                    // REAL sun occlusion: a creature under the canopy sits in tree shade exactly like the ground below it
                  if (${location.search.includes('noshadow') ? 0 : 1} == 1 && dot(nw, u.sunDir) > 0.0 && u.sunDir.y > -0.04) {   // (deterministic un-jittered ray, no denoiser in the path — none of the irr-coupling translucency/shimmer; ?noshadow disables for A/B)
                    let cSp = u.camPos + rd * bestT + nw * 0.6;
                    let cCeil = f32((u32(u.fx) >> 8u) & 31u) * 32.0;   // same world-ceiling cap as the terrain sun ray — a flying bird is above most of it
                    let cCap = select(900.0, min(900.0, (cCeil - cSp.y) / max(u.sunDir.y, 1e-4)), u.sunDir.y > 1e-4);
                    if (cCap <= 0.0) { sunC = 1.0; }
                    else {
                    // skipW was hardcoded false, so for a creature UNDER the surface the first thing this ray
                    // hit was the water itself and every fish came back fully shadowed -- no direct term at all,
                    // lit by sky alone. Water is not an occluder for sunlight; the bed under it is lit through
                    // the surface by its own blit term. The select is false for anything above water, so a duck
                    // still takes tree shade exactly as before.
                    let shC = trace(cSp, u.sunDir, cCap, waterT > 0.0 && bestT > waterT);
                    sunC = select(1.0, 0.0, shC.t >= 0.0);
                    }
                  } else if (${location.search.includes('noshadow') ? 1 : 0} == 1) { sunC = 1.0; }
                  let direct = sunTint() * (sunC * max(dot(nw, u.sunDir), 0.0));
                  let skyIrr = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale();
                  let bounceC = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * nw.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());
                  var alb2 = cell.rgb;                                 // DUCK EYE BLINK: the black eye voxel flashes the head-GREEN when the slot's blink lane (dYv.w) is lit (user)
                  if (${DUCK_ITEM0 > 0 ? 1 : 0} == 1 && dYv.w > 0.5 && (dit == ${DUCK_ITEM0 || 9999} || dit == ${DUCKB_ITEM0 || 9998}) && alb2.r < 0.02 && alb2.g < 0.02 && alb2.b < 0.02) { alb2 = vec3<f32>(${DUCK_GREEN[0].toFixed(4)}, ${DUCK_GREEN[1].toFixed(4)}, ${DUCK_GREEN[2].toFixed(4)}); }
                  let grain = 0.95 + 0.10 * ih3(vcD.x, vcD.y, vcD.z);   // per-voxel texture — GENTLE (±5%) so authored colour transitions dominate the noise (user); hashed on the MODEL-local voxel so it's stable as the creature drifts/rotates
                  // ── CAUSTIC WEBS ON A SUBMERGED CREATURE (user 2026-08-08: "increase the quality of the fish
                  // through the water") ── the lakebed under this same water gets caustics multiplied into its
                  // albedo (see the refraction block); a fish swimming just above that bed got none, so the one
                  // cue that most says "this is underwater" was missing from the animal and present on the
                  // ground behind it. Same caust() call, same 1.6 gain, keyed on the creature's OWN sun
                  // visibility rather than the surface's, so a fish under a tree's shadow is not painted with
                  // sunlit webs. Gated on being submerged: a duck, a bird and every land animal are untouched.
                  //
                  // ── AND THE LIGHT REACHING IT IS *NOT* ATTENUATED HERE (tried twice, rejected by eye) ──
                  // it looks obviously right on paper: the sun crosses the water column before it reaches the
                  // fish, so absorb it over depth/sinAlt. Both a full-strength version and a 0.35-scaled one
                  // turned the fish into a black silhouette, because the composite further down ALREADY
                  // absorbs the view path back to the eye — the depth was being paid for twice — and the slant
                  // path multiplies it again at a low sun. The bed does not attenuate its own incoming term
                  // either, for exactly this reason. If this is revisited, the missing piece is a scattered
                  // in-water light term to fade toward, not a bigger exponent.
                  var alb3 = alb2;
                  if (waterT > 0.0 && bestT > waterT) {
                    let wpC = u.camPos + rd * bestT;
                    let caC = caust(floor(vec2<f32>(wpC.x + u.winO.x, wpC.z + u.winO.y)) + vec2<f32>(0.5));
                    alb3 = alb2 * (1.0 + 1.6 * caC * smoothstep(-0.02, 0.12, u.sunDir.y) * sunC * (1.0 - waterIceK));   // …and the webs stop as the lid closes: caustics need a moving surface to focus the sun through
                  }
                  col = alb3 * (direct + (skyIrr + bounceC) * 0.85 + vec3<f32>(0.012, 0.013, 0.016)) * grain;   // 0.85 ≈ the terrain's typical open-air AO term — same formula as the static world (irr coupling stays banned: it read as translucent)
                  if (${WORM_ITEM0 > 0 ? 1 : 0} == 1 && dit >= ${WORM_ITEM0 || 9999} && dit < ${(WORM_ITEM0 || 9999) + (WORM_NFRAMES || 1)}) {   // WORM fake-AO — the smooth off-grid worm's stand-in for the grid-stamped cardinal's REAL contact AO (user chose 'fake AO, stay smooth'): darken the underside + lower body so it reads GROUNDED, no grid-stamp shimmer
                    let upN = clamp(0.5 + 0.5 * nw.y, 0.0, 1.0);              // 1 = top-facing, 0 = down-facing (the shaded underside against the ground)
                    let hN = f32(vcD.z) / max(1.0, f32(eH - 1));             // 0 = ground side of the body, 1 = its top
                    col = col * mix(0.5, 1.0, 0.65 * upN + 0.35 * smoothstep(0.0, 0.9, hN));   // contact shadow: darkest at the bottom + underside, full-lit on top
                  }
                  if (${FFLY_ITEM0 > 0 ? 1 : 0} == 1 && dit >= ${FFLY_ITEM0 || 9999} && dit < ${(FFLY_ITEM0 || 9999) + (FFLY_NFRAMES || 1)} && dYv.w > 0.0 && cell.r > 0.6 && cell.g > 0.4 && cell.b < 0.1) { col = cell.rgb * (0.4 + dYv.w); }   // FIREFLY abdomen — yellow voxel EMISSIVE (FFLY-range-gated so a blinking duckling's yellow doesn't glow)
                  var fogB = 1.0 - exp(-bestT * 0.0006);
                  fogB = max(fogB, smoothstep(u.rdist.x - 72.0, u.rdist.x - 6.0, bestT * length(vec2<f32>(rd.x, rd.z))));
                  if (!LG(5u)) { fogB = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
                  col = mix(col, skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogB);
                  if (${FFLY_ITEM0 > 0 ? 1 : 0} == 1 && dit >= ${FFLY_ITEM0 || 9999} && dit < ${(FFLY_ITEM0 || 9999) + (FFLY_NFRAMES || 1)} &&
                      cell.r > 0.88 && cell.g > 0.88 && cell.b > 0.88) { col = mix(behind, col, 0.6); }   // FIREFLY WINGS (the white voxels) — 40% translucent
                } else if (di >= 5) {                                // slots 5-24: clash/death SPARKS + death SMOKE (dYv.w = fade)
                  if (dit == ${SMOKE_IT || 9997}) {                  // DEATH SMOKE — each slot is ONE individual white VOXEL (like a snowflake), 24% opacity, fading (col here still holds the scene BEHIND it → mix = true translucency). Off-grid look comes from its own continuous position + snowflake spin in the emit.
                    col = mix(col, vec3<f32>(1.0), 0.24 * dYv.w);
                  } else if (dit == ${HITRED_IT || 9996}) {          // ── BLOOD ── the SAME red the animal itself flashes (user 2026-08-05). It reuses the hit
                    // flash's own albedo constant rather than a palette colour, so the two can never drift apart, and
                    // it is lit the way the flash lights the animal: the flash floors sun visibility at 0.55 and sky at
                    // 0.75 of its strength, so a blood voxel carries those same floors. That is what makes a voxel in
                    // the air read as the same material as the red on the animal it came off. It does NOT pulse or fade
                    // with dYv.w — the flash is a steady colour for the half second it lasts, and so is this.
                    let bDir = sunTint() * max(dot(nw, u.sunDir), 0.0) * 0.55;
                    let bSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale() * 0.75;
                    col = HURT_RED * (bDir + bSky + vec3<f32>(0.012, 0.013, 0.016));
                  } else if (dit == ${FOAM_IT || 9995}) {            // ── SPLASH ── a droplet of the SAME foam the shoreline draws (FOAM_C), so a burst
                    // off the surface reads as water torn off the water rather than a white speck. Lit as a
                    // diffuse surface, NOT emissive like a spark: foam does not glow. It thins out as it dies
                    // (mix toward the scene behind), which is what sells it as spray rather than a solid cube.
                    let sDir = sunTint() * max(dot(nw, u.sunDir), 0.0) * 0.55;
                    let sSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale() * 0.85;
                    col = mix(col, FOAM_C * (sDir + sSky + vec3<f32>(0.02, 0.022, 0.025)), 0.35 + 0.65 * dYv.w);
                  } else {                                           // SPARK — emissive 10 cm ember
                    col = cell.rgb * (0.8 + 2.6 * dYv.w);
                  }
                } else {                                             // ── DROPPED ITEMS ── (user 2026-08-07: "can you ray trace floating objects")
                  // They were always trace-injected — they come down this very DDA — they just opted OUT of
                  // lighting: a flat lambert with no shadow ray, no sky term, no bounce and no grain, which is
                  // why a dropped mushroom read as a sticker pasted over the forest instead of an object in it.
                  // This is the SAME formula the creature branch above uses, so a drop now sits in tree shade,
                  // takes the sky's colour on its upper faces and picks up ground bounce underneath.
                  var sunD = 0.0;
                  if (${location.search.includes('noshadow') ? 0 : 1} == 1 && dot(nw, u.sunDir) > 0.0 && u.sunDir.y > -0.04) {
                    let dSp = u.camPos + rd * bestT + nw * 0.6;
                    let dCeil = f32((u32(u.fx) >> 8u) & 31u) * 32.0;
                    let dCap = select(900.0, min(900.0, (dCeil - dSp.y) / max(u.sunDir.y, 1e-4)), u.sunDir.y > 1e-4);
                    if (dCap <= 0.0) { sunD = 1.0; }
                    else { let dsh = trace(dSp, u.sunDir, dCap, false); sunD = select(1.0, 0.0, dsh.t >= 0.0); }
                  } else if (${location.search.includes('noshadow') ? 1 : 0} == 1) { sunD = 1.0; }
                  let dDirect = sunTint() * (sunD * max(dot(nw, u.sunDir), 0.0));
                  let dSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale();
                  let dBounce = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * nw.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());
                  let dGrain = 0.95 + 0.10 * ih3(vcD.x, vcD.y, vcD.z);
                  col = cell.rgb * (dDirect + (dSky + dBounce) * 0.85 + vec3<f32>(0.012, 0.013, 0.016)) * dGrain;
                }
                // ── ONLY WHAT IS BEHIND THE SURFACE ── keyed on waterT, i.e. on this pixel's primary hit being
                // the water surface. That IS a discontinuity: where the pixel behind a fish shows the bed or the
                // far shore instead, the same fish draws untinted, and panning across that edge pops it. A
                // geometric replacement (measure the submerged path from the waterline crossing) was tried on
                // 2026-08-07 and REVERTED — with no surface pixel to borrow a colour from it saturated toward a
                // stand-in tint that is brighter than the water, turning every fish into a pale cyan blob. Any
                // retry needs a real water colour for that case, not a scaled constant.
                if (waterT > 0.0 && bestT > waterT) {                // the hit sits UNDER the water surface, seen THROUGH it
                  // ── COMPOSITED LIKE THE BED, NOT LIKE A DECAL ── the lakebed reads correctly through this
                  // exact water, so a fish in front of it should be built the same way, and it was not:
                  //   · it faded toward waterCol, the FINISHED surface colour with the sky mirror already
                  //     mixed in, so depth pushed it toward a bright grey instead of into the water;
                  //   · it never took the Fresnel split at all, so the mirror arrived as a function of DEPTH
                  //     rather than of viewing angle;
                  //   · and its extinction was a second literal that missed the 2026-08-06 re-tune.
                  // Now: Beer-Lambert on the shared constant, fade into the in-scatter the bed fades into,
                  // then the same Fresnel mix against the same mirror. Straight down the fish is clear; at a
                  // grazing angle the reflection covers it, which is what water actually does.
                  let trF = exp(-WATER_SIG * (bestT - waterT));
                  let through = col * trF + waterScat * (vec3<f32>(1.0) - trF);
                  col = mix(mix(through, waterRefl, waterFres), waterIceC, waterIceK);   // …then the ICE goes over the top, exactly as it goes over the surface, so a freezing lake closes over the fish on the same 5 s ramp instead of leaving them printed on it
                }
                // ── AND THE DISTANCE FOG, WHICH THE SCENE GETS AND THIS DID NOT ── the fog block above runs on
                // the g-buffer BEFORE this DDA and uses the SCENE's depth, so every creature and drop was
                // composited over an already-hazed world carrying no haze of its own. A fish thirty voxels out
                // read as a crisp sticker pasted on a soft background, and it got worse the further away it was
                // (user 2026-08-07). Same curve, same horizon roll-off, measured against the creature's OWN
                // distance. The held tool is centimetres away, so its fog term is zero and it is unaffected.
                var fogC = 1.0 - exp(-bestT * 0.0006);
                fogC = max(fogC, smoothstep(u.rdist.x - 72.0, u.rdist.x - 6.0, bestT * length(vec2<f32>(rd.x, rd.z))));
                if (!LG(5u)) { fogC = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
                col = mix(col, skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogC);
                // (No submerged tint here: the UNDERWATER block at the end of main now absorbs this pixel over fgT — the
                //  creature's OWN in-water path — so it dims exactly like the world does, with no double attenuation.)
              }
              break;
            }
            if (vNxD.x <= vNxD.y && vNxD.x <= vNxD.z) { tHit = vNxD.x; vNxD.x += abs(invD.x); vcD.x += istD.x; iMapD += istD.x; vaxD = 0; }
            else if (vNxD.y <= vNxD.z) { tHit = vNxD.y; vNxD.y += abs(invD.y); vcD.y += istD.y; iMapD += istD.y * eW; vaxD = 1; }
            else { tHit = vNxD.z; vNxD.z += abs(invD.z); vcD.z += istD.z; iMapD += istD.z * eW * eD; vaxD = 2; }
            if (any(vcD < vec3<i32>(0)) || any(vcD >= vec3<i32>(eW, eD, eH))) { break; }
          }
        }
      }
      if (ITEMN > 0) {                                             // HELD ITEMS — RIGHT hand + LEFT hand (dual-wield rocks) as TRUE 3D voxels, DDA-walked in camera space; the nearer hand wins overlaps
        var heldT = 1e18;
        let ndc2 = (px / u.res) * 2.0 - 1.0;
        let dc = normalize(vec3<f32>(ndc2.x * u.tanH * u.aspect, -ndc2.y * u.tanH, 1.0));
        for (var hand = 0; hand < 2; hand = hand + 1) {
          var pA = u.pickA; var pX = u.pickX; var pY = u.pickY; var pZ = u.pickZ;
          if (hand == 1) { pA = u.pick2A; pX = u.pick2X; pY = u.pick2Y; pZ = u.pick2Z; }
          if (pX.w < 0.5) { continue; }
          let it = clamp(i32(pX.w + 0.5) - 1, 0, ITEMN - 1);
          let PICKW = ITEMD[it].x; let PICKD = ITEMD[it].y; let PICKH = ITEMD[it].z; let IOFF = ITEMD[it].w;
          let C = pA.xyz;
          let vs = pA.w;
          let hw = f32(PICKW) * 0.5; let hd = f32(PICKD) * 0.5; let hh = f32(PICKH) * 0.5;
          let rad = vs * (sqrt(hw * hw + hd * hd + hh * hh) + 1.0);
          let tc = dot(C, dc);
          if (PICKW > 0 && tc > 0.0 && length(dc * tc - C) < rad) {
            // item-local grid space (voxel units): x along pX (width), y along pY (depth), z along pZ (height)
            let roL = vec3<f32>(-dot(C, pX.xyz), -dot(C, pY.xyz), -dot(C, pZ.xyz)) / vs + vec3<f32>(hw, hd, hh);
            var rdL = vec3<f32>(dot(dc, pX.xyz), dot(dc, pY.xyz), dot(dc, pZ.xyz));
            if (abs(rdL.x) < 1e-6) { rdL.x = 1e-6; }
            if (abs(rdL.y) < 1e-6) { rdL.y = 1e-6; }
            if (abs(rdL.z) < 1e-6) { rdL.z = 1e-6; }
            let invL = 1.0 / rdL;
            let ta2 = -roL * invL;
            let tb2 = (vec3<f32>(f32(PICKW), f32(PICKD), f32(PICKH)) - roL) * invL;
            let tn2 = min(ta2, tb2); let tf2 = max(ta2, tb2);
            let te = max(max(tn2.x, tn2.y), max(tn2.z, 0.0));
            let tl = min(min(tf2.x, tf2.y), tf2.z);
            if (te < tl) {
              var vax = 0;
              if (tn2.y == te) { vax = 1; } if (tn2.z == te) { vax = 2; }
              var vc = clamp(vec3<i32>(floor(roL + rdL * (te + 1e-4))), vec3<i32>(0), vec3<i32>(PICKW - 1, PICKD - 1, PICKH - 1));
              let istep = vec3<i32>(sign(rdL));
              var vNext = (vec3<f32>(vc + max(istep, vec3<i32>(0))) - roL) * invL;
              var tCur = te;
              for (var i = 0; i < PICKSTEPS; i++) {
                let cell = ITEMMAP[IOFF + vc.x + vc.y * PICKW + vc.z * PICKW * PICKD];
                if (cell.w > 0.5) {
                  if (tCur * vs < heldT) {                         // both hands can cover a pixel (the spark clash) — keep the nearer surface
                    heldT = tCur * vs;
                    dofHeld = true;                                 // this pixel IS the held tool — depth of field must not touch it (see the store at the end of main)
                    if (fgT < 0.0 || heldT < fgT) { fgT = heldT; }   // the tool in your hands is centimetres away — it must not be absorbed like the far lakebed either
                    var nl = vec3<f32>(0.0);                       // CUBE face normal from the axis the ray entered through — real edges, not a flat card
                    if (vax == 0) { nl.x = -f32(istep.x); } else if (vax == 1) { nl.y = -f32(istep.y); } else { nl.z = -f32(istep.z); }
                    let nc = pX.xyz * nl.x + pY.xyz * nl.y + pZ.xyz * nl.z;
                    let nw = u.right * nc.x + u.up * nc.y + u.fwd * nc.z;
                    let direct = sunTint() * max(dot(nw, u.sunDir), 0.0) * u.heldCfg.x * select(0.0, 1.0, LG(16u));   // bit 16: the held item's DIRECT sun term    // world-matched LIGHTING (sun + sky + ground bounce + ambient) INCLUDING the sun-visibility term the world gets (u.heldCfg.x) — without it a tool stayed fully lit in shade. No per-voxel grain though —
                    let skyIrr = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale();   // grain is ±12% per voxel and scrambles hand-authored .vox gradients (~5% steps on the axe handle)
                    let bounce = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * nw.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());   // warm ground bounce on side/under faces — without it side faces are sky-only (cool + dark), the axe handle read grey-brown
                    var aoF = 1.0;
                    if (i32(pX.w + 0.5) == 1) {                     // AXE ONLY (user): cheap voxel cavity AO — the exposed face darkens where the 4 in-plane neighbours are solid (head↔handle join + crevices). Geometry-based, so it adds depth WITHOUT scrambling the hand-authored gradient like grain would
                      var nlo = vec3<i32>(0);
                      if (vax == 0) { nlo.x = -istep.x; } else if (vax == 1) { nlo.y = -istep.y; } else { nlo.z = -istep.z; }
                      let oc = vc + nlo;               // the empty cell just outside the hit face
                      var t1 = vec3<i32>(0); var t2 = vec3<i32>(0);
                      if (vax == 0) { t1.y = 1; t2.z = 1; } else if (vax == 1) { t1.x = 1; t2.z = 1; } else { t1.x = 1; t2.y = 1; }
                      let dims = vec3<i32>(PICKW, PICKD, PICKH);
                      var occ = 0;
                      for (var s = 0; s < 4; s = s + 1) {
                        var p = oc + t1;
                        if (s == 1) { p = oc - t1; } else if (s == 2) { p = oc + t2; } else if (s == 3) { p = oc - t2; }
                        if (all(p >= vec3<i32>(0)) && all(p < dims) && ITEMMAP[IOFF + p.x + p.y * PICKW + p.z * PICKW * PICKD].w > 0.5) { occ = occ + 1; }
                      }
                      aoF = 1.0 - 0.14 * f32(occ);    // up to ~0.44 in a full crevice; ~0.86–0.72 for typical creases
                    }
                    // …and OCCLUDE the ambient exactly as the world does. The static path multiplies its own
                    // (skyIrr + bounce) by irr.g; the held item had no irr.g at all
                    // (it is composited past the g-buffer, so it has no traced irradiance of its own) and so kept
                    // the full open-sky term wherever it went. u.heldCfg.y is the eye's own sky visibility, marched
                    // in JS alongside the sun ray, and it stands in for irr.g here. Gated on the AO debug bit so
                    // it switches with the world's AO rather than against it.
                    let skyOcc = select(1.0, u.heldCfg.y, LG(1u));
                    col = cell.rgb * (direct + (skyIrr + bounce) * skyOcc + vec3<f32>(0.012, 0.013, 0.016)) * aoF;
                  }
                  break;
                }
                if (vNext.x <= vNext.y && vNext.x <= vNext.z) { tCur = vNext.x; vNext.x += abs(invL.x); vc.x += istep.x; vax = 0; }
                else if (vNext.y <= vNext.z) { tCur = vNext.y; vNext.y += abs(invL.y); vc.y += istep.y; vax = 1; }
                else { tCur = vNext.z; vNext.z += abs(invL.z); vc.z += istep.z; vax = 2; }
                if (any(vc < vec3<i32>(0)) || any(vc >= vec3<i32>(PICKW, PICKD, PICKH))) { break; }
              }
            }
          }
        }
      }
      if (u.lifeCfg.x > 0.5) {                                       // ── DYNAMIC-LIFE DEBUG VIEWS (__vb.lifedbg) ── 1: slot ids (occupancy/object identity) · 2: history confidence (red = rejected/fresh, green = converged) · 3: per-slot motion vectors · 4: raw denoised AO
        let dbgm = i32(u.lifeCfg.x + 0.5);
        let slD = textureLoad(slotT, vec2<i32>(gid.xy), 0).r & 255u;
        let irrD = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        if (dbgm == 1 && slD != 0u) { col = mix(col, vec3<f32>(fract(f32(slD) * 0.61803), fract(f32(slD) * 0.3247 + 0.33), fract(f32(slD) * 0.7548 + 0.66)), 0.72); }
        else if (dbgm == 2) {                                          // history confidence — RED fresh, GREEN converged, and BLUE for a pixel with NO g-buffer depth at all
          if (irrD.b > 0.0) { let hc = clamp(irrD.a / u.maxHist, 0.0, 1.0); col = mix(vec3<f32>(0.9, 0.05, 0.05), vec3<f32>(0.05, 0.85, 0.15), hc); }
          else { col = vec3<f32>(0.05, 0.25, 1.0); }                     // t <= 0: TEMPORAL skips it, so it gets no irradiance (renders black) AND the creature occlusion bound goes infinite (they draw through). If the bad shadow lights up BLUE, that is the bug.
        }
        else if (dbgm == 3 && slD != 0u) { let mv = lifeMotV(i32(slD) - 1).xyz; col = clamp(abs(mv) * 2.5, vec3<f32>(0.06), vec3<f32>(1.0)); }
        else if (dbgm == 4 && irrD.b > 0.0) { col = vec3<f32>(irrD.g); }
        else if (dbgm == 5 && irrD.b > 0.0) { col = vec3<f32>(irrD.r); }   // DENOISED sun visibility alone — no AO, no albedo. If the blobby dark regions are ABSENT here, they are the AO term, not a shadow; if they are present and still blobby, the filter is smearing a hard shadow.
      }
      if ((u32(u.fx) & 2u) != 0u) {                                  // ── UNDERWATER ── (camera submerged) Beer–Lambert absorption over the in-water path + a RAY-MARCHED single-scatter with caustic-modulated sun shafts (replaces the old flat blue tint)
        let irrU = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        var wD = select(1e4, irrU.b, irrU.b > 0.0);                  // in-water path toward the hit (sky pixels: the whole march range)
        if (fgT > 0.0) { wD = fgT; }                                 // …but if a CREATURE / drop / held item is what this pixel actually shows, the water only reaches THAT far. Using the scene depth here absorbed a fish 14 vox away as if it were the 160-vox background (exp(-0.062*160) ≈ 5e-5) — fish, drops and the held tool all vanished the moment you swam under. This is what "I can't see the fish underwater" was.
        if (rd.y > 0.001) { wD = min(wD, max((WLF + 1.0 - u.camPos.y) / rd.y, 0.0)); }   // the ray exits through the surface — only the submerged stretch attenuates
        wD = clamp(wD, 0.0, 160.0);
        let sigU = vec3<f32>(0.16, 0.062, 0.030);                    // gentler than the surface view — swimming has to stay playable
        let trU = exp(-sigU * wD);
        var seedW = ((gid.x * 7817u) ^ (gid.y * 45589u) ^ (u32(u.frame) * 2657u)) | 1u;
        var accW = vec3<f32>(0.0);
        let sunUp = smoothstep(-0.02, 0.12, u.sunDir.y);
        let dtW = wD / 4.0;                                          // 4 jittered steps (was 6) — the estimator is unbiased, so TAA converges to the SAME image; only the per-frame noise rises a hair (near-lossless)
        var tw2 = dtW * rand(&seedW);                                // jittered march start — TAA melts the step banding
        let sunInv = 1.0 / max(u.sunDir.y, 0.25);
        for (var si = 0; si < 4; si++) {
          let p = u.camPos + rd * tw2;
          let dBelow = max(WLF + 1.0 - p.y, 0.0);
          let lightT = exp(-sigU * dBelow * sunInv);                 // the sun's own path through the water down to this point
          let caW = 0.45 + 1.75 * caust(floor(vec2<f32>(p.x + u.winO.x + dBelow * u.sunDir.x * sunInv, p.z + u.winO.y + dBelow * u.sunDir.z * sunInv)) + vec2<f32>(0.5));   // project along the sun to the surface → dancing god-ray shafts
          accW += vec3<f32>(0.020, 0.078, 0.098) * (0.22 + 0.78 * sunUp * caW) * lightT * dayScale() * exp(-sigU * tw2) * (sigU * dtW * 2.6);
          tw2 += dtW;
        }
        col = col * trU + accW;
      }
      col = aces(col * 0.95);
      col = pow(col, vec3<f32>(1.0 / 2.2));
      // ── VOLUMETRICS ── added on top of the shaded image, the way Teardown composites its volumetric
      // buffer alongside diffuse and specular. Gated on the lane holding anything at all, so a scene
      // with no embers, fireflies or wounded animals in it pays one compare.
      if (LG(17u)) {
        var anyL = false;
        for (var f = 0; f < 8; f++) { if (u.fflies[f].w > 0.0) { anyL = true; break; } }
        if (anyL) {
          let vD = textureLoad(irrF, vec2<i32>(gid.xy), 0).b;      // distance to whatever this pixel shows
          let wDepth = select(90.0, vD, vD > 0.0);                 // stop the march at that surface — past it the medium is behind geometry
          // interleaved-gradient noise, inlined: ign() lives in the tracer module, not here
          let ig = fract(52.9829189 * fract(0.06711056 * f32(gid.x) + 0.00583715 * f32(gid.y)));
          let dth = fract(ig + f32(u32(u.frame) & 31u) * 0.7548777);   // …rotated per frame so TAA averages the march offsets away
          col += volLight(u.camPos, rayDir(vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y)), wDepth, dth);
        }
      }
      // ── DEPTH OF FIELD ── alpha carries the signed circle of confusion, encoded to 0..1. It is computed HERE
      // rather than in the blit because this is the only pass that knows what the pixel actually SHOWS: the
      // g-buffer distance behind a fish, a dropped rock or the held axe is the hillside they stand against, and
      // blurring a foreground object by the depth of the background behind it is precisely the halo artefact
      // depth of field is notorious for. The held tool is exempted outright: it sits two voxels from the lens, so
      // any focal plane out in the world puts it at the near stop, and a permanently smeared axe is not depth —
      // it is just a blurry axe. Everything else blurs by its own distance.
      var dofD = textureLoad(irrF, vec2<i32>(gid.xy), 0).b;          // scene depth; < 0 = sky
      if (fgT > 0.0) { dofD = fgT; }                                 // …but a creature, a dropped item or the held tool is nearer, and IT is what you see
      let cocS = select(dofCoc(dofD), 0.0, dofHeld);
      textureStore(colorOut, vec2<i32>(gid.xy), vec4<f32>(col, cocS * 0.5 + 0.5));
    }
  `;

