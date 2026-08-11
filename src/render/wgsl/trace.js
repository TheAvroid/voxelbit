  const TRACE_SRC = ({ DDAW, FLAKEBLK, pickWGSL }) => /* wgsl */`
    @group(0) @binding(1) var<storage, read> world : array<u32>;
    @group(0) @binding(2) var<storage, read> bricks : array<u32>;
    @group(0) @binding(3) var<storage, read> pal : array<vec4<f32>>;
    @group(0) @binding(4) var gAlbedo : texture_storage_2d<rgba8unorm, write>;
    @group(0) @binding(5) var gIrr : texture_storage_2d<rgba16float, write>;
    @group(0) @binding(7) var<storage, read> wbricks : array<u32>;   // water-only brick bits — skipW rays stride these
    @group(0) @binding(8) var slotOut : texture_storage_2d<r32uint, write>;   // ── DYNAMIC LIFE ── per-pixel creature id: bits 0-7 = drop slot + 1 (0 = terrain/sky), bits 8-10 = model-space hit axis*2+signBit (composite rebuilds the TRUE rotated normal from it)
    ${pickWGSL}
    ${DDAW}
    @group(0) @binding(6) var<storage, read> bricks2 : array<u32>;   // 32-voxel SUPER-brick occupancy (window origin is 32-aligned, so off>>5 is exact)
    const SNV : u32 = ${location.search.includes('flakedbg') ? LAVA_T : SNOW[0]}u;                                   // falling-flake voxel id — flakes are REAL primary hits (?flakedbg paints them as emissive lava to bisect trace-vs-downstream)
    const LVT : u32 = ${LAVA_T}u; const LVB : u32 = ${LAVA_B}u; const LVR : u32 = ${LAVA_R}u; const LVY : u32 = ${LAVA_Y}u;
    ${FLAKEBLK}
    fn onbT(n : vec3<f32>) -> vec3<f32> {
      return normalize(select(cross(n, vec3<f32>(0.0, 1.0, 0.0)), cross(n, vec3<f32>(1.0, 0.0, 0.0)), abs(n.y) > 0.9));
    }
    fn cosHemi(n : vec3<f32>, r1 : f32, r2 : f32) -> vec3<f32> {
      let a = 6.2831853 * r1; let r = sqrt(r2);
      let t = onbT(n); let b = cross(n, t);
      return t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(max(1.0 - r2, 0.0));
    }
    fn ign(p : vec2<f32>) -> f32 { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }
    @group(0) @binding(23) var<storage, read> bodyVox : array<u32>;  // ── RIGID BODIES ── dense per-body voxel grids (palette id per cell, 0 = empty), sub-allocated back to back
    ${UNI_FN}
    @group(0) @binding(9) var<storage, read> visb : array<u32>;      // ── DYNAMIC LIFE tile cull ── per-8×8-tile drop-slot visibility bitmask (4×u32/tile), computed ONCE per frame by the tiny VIS prepass and shared with the composite — replaces the per-workgroup recompute + workgroupBarrier that ran here even on creature-free frames
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>, @builtin(workgroup_id) wgid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let px = vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y);
      let rd = rayDir(px);
      let ro = u.camPos;                                             // window-relative
      ${!UNI_RAY ? '' : 'let tiS = (wgid.y * ((u32(u.res.x) + 7u) / 8u) + wgid.x) * 8u + 4u; let sg0 = visb[tiS]; let sg1 = visb[tiS + 1u]; let sg2 = visb[tiS + 2u]; let sg3 = visb[tiS + 3u]; let secN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);'}
      let skipW = (u32(u.fx) & 2u) != 0u;                            // camera underwater → water voxels are see-through
      let tcap = u.rdist.x / max(length(vec2<f32>(rd.x, rd.z)), 0.05);   // CIRCULAR render distance (slider) — the world ends at a radius, not a square edge
      var tEye = 0.0;                                                // CLIP-THROUGH FIX: the eye poking into canopy/terrain voxels made every ray hit unlit solid at t=0 — a full BLACK frame.
      {                                                              // Slide this ray's start past up to 3 voxels of solid; deeper stays dark (no x-ray through real walls). Water is
        let ec0 = vec3<i32>(floor(ro));                              // exempt on both ends — swimming already handles it via skipW.
        let v0 = voxAt(clamp(ec0, vec3<i32>(0), vec3<i32>(WX - 1, WY - 1, WZ - 1)));
        if (ec0.y > 0 && ec0.y < WY - 1 && v0 != 0u && v0 != WTv && v0 != WBv) {
          for (var q = 1; q <= 12; q++) {
            let tq = f32(q) * 0.25;
            let pq = vec3<i32>(floor(ro + rd * tq));
            if (pq.y <= 0 || pq.y >= WY - 1) { tEye = tq; break; }
            let vq = voxAt(clamp(pq, vec3<i32>(0), vec3<i32>(WX - 1, WY - 1, WZ - 1)));
            if (vq == 0u || vq == WTv || vq == WBv) { tEye = tq; break; }
          }
        }
      }
      if (FOLSKIP) {                                                 // ── FOLIAGE SEE-THROUGH ── if the eye sits inside a leaf voxel, make near foliage transparent to the PRIMARY ray only
        let ce = vec3<i32>(floor(ro));                               // (this whole block only exists in the see-through pipeline variant — the CPU picks it when the eye is within 1 voxel of foliage)
        if (ce.y > 0 && ce.y < WY - 1 && isFol(voxAt(clamp(ce, vec3<i32>(0), vec3<i32>(WX - 1, WY - 1, WZ - 1))))) { folSkipD = 30.0; }   // ~3 m: clears the crown you clipped into; distant canopy still renders normally
      }
      var h = trace(ro + rd * tEye, rd, min(4000.0, tcap) - tEye, skipW);
      folSkipD = 0.0;                                                 // reset before the sun / AO / lava / reflection traces below — they must still see the canopy as solid
      if (h.t >= 0.0) { h.t += tEye; }
      var flakeHit = false; var flakeFade = 1.0; var flakeUnder = 0u;                                        // set when the primary hit IS a falling flake — the lighting below gives those a scatter floor (see the sunV/skyV clamp)
      if ((u32(u.fx) & 16u) != 0u) {                               // FALLING SNOW — flakes are REAL VOXELS in the primary trace: grain, the jittered sun ray, AO, fog and the denoiser treat them exactly like the static world
        var rdS = rd;
        if (abs(rdS.x) < 1e-6) { rdS.x = 1e-6; } if (abs(rdS.y) < 1e-6) { rdS.y = 1e-6; } if (abs(rdS.z) < 1e-6) { rdS.z = 1e-6; }
        let inv = 1.0 / rdS;
        let winOf = vec3<f32>(u.winO.x, 0.0, u.winO.y);
        var fte = 1e9;
        var fnn = vec3<f32>(0.0); var fface = 0u; var fkAw = 1.0; var fkUw = 0u;   // …and the winner's fade + what is under it
        {                                                            // near field: DDA over the 3-voxel flake lattice — a flake stays WHOLE from sky to ground.
          // A tile-binned producer pass was tried here (enumerate+bin flakes once, pixels read their tile): the TRACE
          // side dropped to +0.35 ms, but on this driver ANY producer dispatch with atomics OR workgroup barriers costs
          // ~3 ms flat (hash-only = 0.04 ms — measured either way), so the march wins on total. Its own order is
          // OPTIMIZED: hash gate → storm-band check → free conservative slab vs the spin bound → exact rotated hit →
          // flakeBlocked (~38 scattered loads) LAST, only for a ray that genuinely strikes the flake. All pure
          // predicates — the image is identical to the original ordering.
          let fall = vec3<f32>(-u.rdist.z + sin(u.time * 0.6) * 0.8, u.pickY.w, -u.rdist.w + cos(u.time * 0.5) * 0.8);   // u.pickY.w = integrated fall (doubles when standing still) + gusting wind
          // ── LARGE-COORDINATE PRECISION (user: "the snow clips into 2 sections") ── the old march ran on ro+winOf+fall
          // directly: at a far spawn (|world| ~1e5-4e5) f32 keeps only ~0.03 vox, and every slab/DDA test inherited that
          // error → flakes silently lost in view-dependent sectors. Split the big offset into an EXACT integer cell
          // offset (offC, fed to the hash) + a small [0,3) remainder (offR) and run the whole march in window-local
          // floats — bit-identical cells, full sub-voxel precision at ANY world position.
          let off = winOf + fall;
          let offC = vec3<i32>(floor(off * (1.0 / 3.0)));            // integer 3-vox-cell offset — exact (|off|/3 ≤ ~1.4e5, and f32 holds integers to 16.7M)
          let offR = off - vec3<f32>(offC) * 3.0;                    // remainder in [0,3) — small, continuous as the wind/fall drift
          let roP = ro + offR;                                       // window-local march origin (≤ ~2051 in magnitude → ~1e-4 vox precision)
          let maxNear = min(min(select(1e9, h.t, h.t >= 0.0), u.rdist.x), 150.0);
          var c = vec3<i32>(floor((roP + rd * 0.3) * (1.0 / 3.0)));  // exact third (0.3333 undershot by 1e-4 relative — harmless here, but keep it exact)
          let stp = vec3<i32>(i32(sign(rdS.x)), i32(sign(rdS.y)), i32(sign(rdS.z)));
          let tDel = abs(inv) * 3.0;
          var tMax = ((vec3<f32>(c) + max(vec3<f32>(stp), vec3<f32>(0.0))) * 3.0 - roP) * inv;
          for (var it = 0; it < 96; it++) {
            let h1 = ih3(c.x + offC.x, (c.y + offC.y) * 7, c.z + offC.z);   // hash on the WORLD cell (local + exact offset) — the same integers as before, so the flake set is unchanged
            if (h1 > 0.9755) {                                       // ~2.45% of cells (halved again per user)
              let ctr = (vec3<f32>(c) + vec3<f32>(0.17 + 0.66 * fract(h1 * 43758.5), 0.17 + 0.66 * fract(h1 * 12345.7), 0.17 + 0.66 * fract(h1 * 7777.3))) * 3.0;
              let cwy = ctr.y - offR.y;                              // storm-band pre-check — ctr is window-local, so minus the small remainder IS the world y (winOf.y = 0)
              if (cwy >= u.misc.y && cwy <= u.misc.z) {
              let roL0 = roP - ctr;
              let taC = (vec3<f32>(-0.708, -0.5, -0.708) - roL0) * inv;   // free conservative slab vs the flake's rotation bound (a spinning 1³ voxel about Y stays inside 0.708/0.5/0.708) — no trig yet
              let tbC = (vec3<f32>(0.708, 0.5, 0.708) - roL0) * inv;
              let tnC = min(taC, tbC); let tfC = max(taC, tbC);
              if (max(max(tnC.x, tnC.y), max(tnC.z, 0.3)) < min(min(min(tfC.x, tfC.y), tfC.z), maxNear)) {
              let spin = u.time * (2.0 + h1 * 2.5) + h1 * 47.0;      // each flake TWIRLS at its own rate and phase
              let cs = cos(spin); let sn = sin(spin);
              var rdL = vec3<f32>(rdS.x * cs - rdS.z * sn, rdS.y, rdS.x * sn + rdS.z * cs);   // ray → flake-local (rotate about Y by −spin)
              if (abs(rdL.x) < 1e-6) { rdL.x = 1e-6; } if (abs(rdL.z) < 1e-6) { rdL.z = 1e-6; }
              let invL = 1.0 / rdL;
              let roL = vec3<f32>(roL0.x * cs - roL0.z * sn, roL0.y, roL0.x * sn + roL0.z * cs);
              // ── IT FADES OUT, IT DOES NOT BLINK OUT ── a flake is an OPAQUE voxel, so the moment it fell past the
              // surface it lost the depth test and vanished between two frames; near the ground that reads as a flicker.
              // It keeps its FULL SIZE (user 2026-08-07: "do not shrink the snowflakes, just make them fade out") and
              // dissolves instead: over the last two voxels of the fall its colour is blended toward the ground directly
              // beneath it, which is what the pixel would show through a translucent flake anyway. ctr lives in roP space
              // and roP is ro + offR, so subtracting offR gives the window-local integers voxAt expects.
              var fkA = 1.0; var fkUnder = 0u;
              { let fb = vec3<i32>(floor(ctr - offR));
                let fy = fract(ctr.y - offR.y);
                let b1 = voxAt(vec3<i32>(fb.x, fb.y - 1, fb.z));
                if (b1 != 0u) { fkA = clamp(fy, 0.0, 1.0); fkUnder = b1; }
                else { let b2 = voxAt(vec3<i32>(fb.x, fb.y - 2, fb.z));
                  if (b2 != 0u) { fkA = clamp(0.5 + 0.5 * fy, 0.0, 1.0); fkUnder = b2; } } }
              let ta2 = (vec3<f32>(-0.5) - roL) * invL;              // FULL 10 cm voxel — size is never touched
              let tb2 = (vec3<f32>(0.5) - roL) * invL;
              let tn = min(ta2, tb2); let tf = max(ta2, tb2);
              let te = max(max(tn.x, tn.y), max(tn.z, 0.3));
              let tl = min(min(tf.x, tf.y), tf.z);
              if (te > 1.0 && te < tl && te < maxNear && !flakeBlocked(ctr - offR)) {   // te > 1: a flake that reaches the eye "hits your face" is culled; the open-air test runs LAST, only on a real strike. ctr is window-local now — minus the small remainder = the window position (exact at any world coordinate)
                fte = te; fkAw = fkA; fkUw = fkUnder;
                var nl = vec3<f32>(0.0);
                if (tn.x >= tn.y && tn.x >= tn.z) { nl.x = -sign(rdL.x); }
                else if (tn.y >= tn.z) { nl.y = -sign(rdL.y); }
                else { nl.z = -sign(rdL.z); }
                fnn = vec3<f32>(nl.x * cs + nl.z * sn, nl.y, -nl.x * sn + nl.z * cs);   // local normal → world (rotate back)
                if (abs(fnn.y) >= abs(fnn.x) && abs(fnn.y) >= abs(fnn.z)) { fface = select(2u, 3u, fnn.y < 0.0); }
                else if (abs(fnn.x) >= abs(fnn.z)) { fface = select(0u, 1u, fnn.x < 0.0); }
                else { fface = select(4u, 5u, fnn.z < 0.0); }
                break;
              }
              }
              }
            }
            let tNext = min(tMax.x, min(tMax.y, tMax.z));
            if (tNext > maxNear) { break; }
            if (tMax.x <= tMax.y && tMax.x <= tMax.z) { tMax.x += tDel.x; c.x += stp.x; }
            else if (tMax.y <= tMax.z) { tMax.y += tDel.y; c.y += stp.y; }
            else { tMax.z += tDel.z; c.z += stp.z; }
          }
        }
        if (fte < 8e8 && (h.t < 0.0 || fte < h.t)) {                 // a flake beat the world hit — it IS a snow voxel from here on
          h.t = fte; h.vox = SNV; h.n = u.sunDir; h.face = fface; flakeHit = true; flakeFade = fkAw; flakeUnder = fkUw;   // SUN-FACING normal (user: "the snowflakes split into 2 chunks"): per-face cube shading tinted anti-sunward flakes grey vs white sunward ones — a visible tonal divide. A flake SCATTERS, so its lighting must not depend on face orientation; n = sunDir makes every flake's dot(n,sun) = 1 → one uniform field. (fface keeps the real face for grain.)
        }
      }
      // ── DYNAMIC LIFE ── every creature is a rigid voxel MODEL evaluated right here in the primary trace (the proven
      // snow-flake pattern, generalized): a model-space DDA with the slot's continuous sub-voxel transform. A nearer
      // creature OVERRIDES the hit, so the sun ray, the AO ray, the glow field, fog, water and the whole SVGF chain
      // treat it exactly like terrain — real contact AO and accumulated GI, no analytic stand-ins. Nothing is ever
      // written into any world grid: terrain corruption, stamp clearing and creature-overlap conflicts are impossible
      // by construction, and motion stays continuous (no grid snapping). Fireflies/sparks/dropped items keep the
      // composite path (emissive + translucency), as does any creature seen THROUGH a water surface (Beer–Lambert).
      var cSlot = 0u; var cCell = vec3<f32>(0.0); var cVc = vec3<i32>(0); var cAxis = 0u; var cN = vec3<f32>(0.0, 1.0, 0.0);
      var bHit = false; var bCol = vec3<f32>(0.0); var bN = vec3<f32>(0.0, 1.0, 0.0); var bVc = vec3<i32>(0);   // rigid-body (felled chunk) hit
      var bestT = select(1e9, h.t, h.t >= 0.0);                      // nearest hit so far (world / flake) — SHARED by the creature loop and the rigid-body trace below, which is why it lives outside the dynamic-life gate
      if (ITEMN > 0 && u.lifeCfg.y > 0.5) {
        let ndc3 = (px / u.res) * 2.0 - 1.0;
        let dc3 = normalize(vec3<f32>(ndc3.x * u.tanH * u.aspect, -ndc3.y * u.tanH, 1.0));   // camera-space twin of rd — the drop transforms live in camera space
        let dropN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);
        let tiV = (wgid.y * ((u32(u.res.x) + 7u) / 8u) + wgid.x) * ${VIS_W}u;   // this workgroup IS one 8×8 tile — read its four prepass mask words (under ?uni the stride is 8: words 0-3 primary, 4-7 the grown SECONDARY mask)
        let visM0 = visb[tiV]; let visM1 = visb[tiV + 1u]; let visM2 = visb[tiV + 2u]; let visM3 = visb[tiV + 3u];   // FOUR words now (128 slots) and all four stay in REGISTERS: re-fetching the word from storage per iteration measured 4× the per-slot cost
        for (var di = 4; di < dropN; di++) {                         // 4 = flying cardinal, 25+ = live creatures; 0-3 drops + 5-24 sparks/smoke stay analytic
          if (di >= 5 && di <= 24) { di = 24; continue; }             // the 20 death-burst slots are analytic-only — step OVER the whole band, don't walk it
          { let mw = select(select(visM0, visM1, di >= 32), select(visM2, visM3, di >= 96), di >= 64); let mrem = mw >> (u32(di) & 31u); if (mrem == 0u) { di = i32(u32(di) | 31u); continue; } if ((mrem & 1u) == 0u) { di += i32(countTrailingZeros(mrem)) - 1; continue; } }   // ── TILE CULL, BIT-SCANNED ── the mask word says which slots can touch this 8×8 tile; jump straight to the next SET bit (or over an empty word entirely) instead of paying one loop iteration per slot. The iteration was the whole cost — an empty slot cost the same as a live one — so this is what makes the array size stop mattering. NOTE: it mutates di; a later edit that assumes di advances by one, or any drift between dropN and the mask, makes creatures vanish silently.
          if ((u32(lifeMotV(di).w + 0.5) & 1u) != 0u) { continue; } // analytic-only (firefly / empty)
          let dXv = dropV(di * 4 + 1);
          let dit = i32(dXv.w + 0.5);
          if (dit < 1) { continue; }
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
          var iMapD = eOff + vcD.x + vcD.y * eW + vcD.z * eW * eD;
          for (var i = 0; i < PICKSTEPS; i++) {
            let cell = ITEMMAP[u32(iMapD)];
            if (cell.w > 0.5) {
              if (tHit * vsD < bestT) {
                bestT = tHit * vsD;
                cSlot = u32(di + 1);
                cCell = cell.rgb; cVc = vcD;
                var nl = vec3<f32>(0.0);
                if (vaxD == 0) { nl.x = -f32(istD.x); } else if (vaxD == 1) { nl.y = -f32(istD.y); } else { nl.z = -f32(istD.z); }
                let nc = dXv.xyz * nl.x + dYv.xyz * nl.y + dZv.xyz * nl.z;
                cN = normalize(u.right * nc.x + u.up * nc.y + u.fwd * nc.z);   // TRUE rotated world normal — drives the sun ray + AO hemisphere (and the composite, via the axis bits)
                let sgn = select(0u, 1u, (vaxD == 0 && nl.x > 0.0) || (vaxD == 1 && nl.y > 0.0) || (vaxD == 2 && nl.z > 0.0));
                cAxis = u32(vaxD) * 2u + sgn;
              }
              break;
            }
            if (vNxD.x <= vNxD.y && vNxD.x <= vNxD.z) { tHit = vNxD.x; vNxD.x += abs(invD.x); vcD.x += istD.x; iMapD += istD.x; vaxD = 0; }
            else if (vNxD.y <= vNxD.z) { tHit = vNxD.y; vNxD.y += abs(invD.y); vcD.y += istD.y; iMapD += istD.y * eW; vaxD = 1; }
            else { tHit = vNxD.z; vNxD.z += abs(invD.z); vcD.z += istD.z; iMapD += istD.z * eW * eD; vaxD = 2; }
            if (any(vcD < vec3<i32>(0)) || any(vcD >= vec3<i32>(eW, eD, eH))) { break; }
          }
        }
      }                                                              // …end of the DYNAMIC-LIFE gate. The rigid-body trace below is deliberately OUTSIDE it.
      {                                                              // ── RIGID BODIES ── same traversal the shadow/AO rays use, so what you see and what the light sees can never disagree.
        // NOT gated on dynamic life (u.lifeCfg.y / ITEMN): a felled chunk has nothing to do with creature
        // trace-injection, and the secondary rays call traceAll unconditionally. While this sat inside that
        // gate, ?oldlife (or a failed .vox fetch leaving ITEMN == 0) made a chunk INVISIBLE to the camera while
        // it still cast a shadow, occluded AO and appeared in the water reflection — the exact disagreement the
        // line above says is impossible. Free when there is nothing to hit: bodyTrace returns on one compare
        // while physC.x is 0, which is every frame no tree has been felled.
        let bh2 = bodyTrace(ro, rd, bestT);
        if (bh2.t >= 0.0) { bestT = bh2.t; bHit = true; bCol = pal[bh2.vox].rgb; bN = bh2.n;
          bVc = bh2.vc; }                                          // BODY-LOCAL cell (see Hit.vc): the grain rides with the wood instead of the trunk sliding through a world-anchored noise field
      }
      if (bHit) {                                                    // a felled chunk is the primary hit — terrain-identical shading from here on
        h.t = bestT; h.vox = 0u; h.n = bN;
        cSlot = 0u; cAxis = 0u;                                      // …and this pixel is NOT a creature any more, however the creature loop above left it. bodyTrace ran with maxT = bestT, so a chunk only wins by being strictly NEARER — but cSlot survived, and slotOut then told COMPOSITE to rebuild the normal from that animal's model axes and told DENOISE/TAA to reproject the pixel by the animal's motion. Fell a pine across a bird and the trunk pixels were lit with the bird's normal and smeared temporally.
        if (abs(bN.y) >= abs(bN.x) && abs(bN.y) >= abs(bN.z)) { h.face = select(2u, 3u, bN.y < 0.0); }
        else if (abs(bN.x) >= abs(bN.z)) { h.face = select(0u, 1u, bN.x < 0.0); }
        else { h.face = select(4u, 5u, bN.z < 0.0); }
      } else if (cSlot != 0u) {                                      // the creature IS the primary hit from here on
        h.t = bestT; h.vox = 0u; h.n = cN;                           // vox 0 → the water/lava id checks below can't misfire
        if (abs(cN.y) >= abs(cN.x) && abs(cN.y) >= abs(cN.z)) { h.face = select(2u, 3u, cN.y < 0.0); }
        else if (abs(cN.x) >= abs(cN.z)) { h.face = select(0u, 1u, cN.x < 0.0); }
        else { h.face = select(4u, 5u, cN.z < 0.0); }                // nearest-axis face: the denoiser's edge tests + a composite fallback; true shading normal comes from the axis bits
      }
      var albedo = vec3<f32>(0.0);
      var faceId = 7u; var t = -1.0;
      var hurtGlow = 0.0;                                            // >0 on a pixel inside the hit flash: it is emissive, so it must not be left to whatever light happens to reach it
      var sunV = 0.0; var skyV = 0.0;   // (bit 15 zeroes skyV at the end of the lighting block — the ambient/sky term, as opposed to the AO ray that modulates it)
      var creReact = 0.0;                                            // set inside the creature-shadow loop below: this pixel sits in a MOVING shadow's penumbra, so the reactive mask must cap its history (declared out here — the reactive mask is written well past the shading block's scope)
      if (h.t >= 0.0) {
        t = h.t; faceId = h.face;
        if ((h.vox == WTv || h.vox == WBv) && h.face == 2u) { faceId = 6u; }   // water top → reflective shading in the composite
        if (h.vox == LVT || h.vox == LVB || h.vox == LVR || h.vox == LVY) { faceId = 8u; }   // lava → emissive
        let pos = ro + rd * t;
        let vcW = vec3<i32>(floor(pos - h.n * 0.01)) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));   // WORLD coords — grain must not swim when the window shifts
        if (bHit) { albedo = bCol * select(1.0, 0.88 + 0.24 * ih3(bVc.x, bVc.y, bVc.z), LG(9u)); }   // felled chunk: palette colour + genuinely MODEL-LOCAL grain, at the SAME +/-12% amplitude static terrain uses, so a fallen trunk reads exactly like a standing one
        else if (cSlot != 0u) { albedo = cCell * select(1.0, 0.95 + 0.10 * ih3(cVc.x, cVc.y, cVc.z), LG(11u)); }   // creature: its cell color (baked self-AO included) + MODEL-LOCAL grain — GENTLE (±5%) so a model's authored colour transitions dominate the random per-voxel noise (user: adjacent whites read very differently); stable as it moves/rotates, matches the analytic path
        else { albedo = pal[h.vox].rgb * select(1.0, 0.88 + 0.24 * ivhash(vcW), LG(10u)); }
        if (u.hurtB.w > 0.0) {                                       // ── HIT FLASH ── the animal just hit, blinking red (user)
          // WHOSE pixel is this? A trace-injected creature carries its dynamic-life slot in cSlot, so the
          // wounded animal is identified exactly, pixel for pixel, however it moves. The old box could
          // never do that: a worm renders OFF-GRID, so it had no stamped bounds and fell back to a
          // generous cube that slid around independently of the animal inside it (user).
          let isMe = (cSlot != 0u && cSlot == u32(u.hurtH.w + 0.5));
          // Grid-stamped animals (mammals, perched birds) are ordinary world voxels with no cSlot — but
          // hurtBox hands over their exact stamped bounds, so testing against those hugs the animal.
          let dHit = abs(pos - u.hurtB.xyz) / max(u.hurtH.xyz, vec3<f32>(0.001));
          // …and the voxel has to BE the animal: pal[].a carries the grid-stamped-creature flag (CREA_FLAG).
          // The box alone is an AABB, so it also holds the grass between the animal's legs and the ground
          // under its belly — painting those was the red square on the terrain (user). With the flag the
          // grid-stamped test is as exact as the trace-injected one: creature voxels only, terrain never.
          // …and a RAGDOLL is neither: on the killing blow the animal becomes a rigid BODY (bHit), which
          // carries no palette flag and — because the bHit branch above now clears it — no cSlot either
          // (it used to keep whatever slot the creature loop had just set). hurtBox tracks that body's live centre while it falls, so
          // testing a body pixel against the box is what keeps it red the whole way down (user 2026-08-05:
          // "red and rigid at the same time"). Only bodies INSIDE the box are touched, and the box is the
          // dead animal's own radius, so an ordinary chopped chunk lying elsewhere is never painted.
          if (isMe || (u.hurtH.w < 0.5 && bHit && all(dHit <= vec3<f32>(1.0)))
                   || (u.hurtH.w < 0.5 && !bHit && pal[h.vox].a > 0.5 && all(dHit <= vec3<f32>(1.0)))) {   // the WHOLE stamp, not an ellipsoid inside it (user)
            // NO SLACK (user: "the land mammals cast a red square on the terrain"). The box used to carry a
            // whole voxel of padding on every side, and everything in that shell — the ground under the
            // animal's feet, the grass beside it — was painted red too: a square of stained terrain around
            // the animal. hurtBox now hands over the animal's EXACT geometric bounds, so the only terrain
            // that can still be caught is the surface a neighbouring voxel shares with the animal's own
            // outer face, which the animal itself hides. Measuring the fade against those same bounds keeps
            // every voxel of the creature at mTrue <= 1, i.e. fully lit, with nothing left outside to fade.
            let hTrue = max(u.hurtH.xyz, vec3<f32>(0.001));
            let mTrue = max(abs(pos.x - u.hurtB.x) / hTrue.x, max(abs(pos.y - u.hurtB.y) / hTrue.y, abs(pos.z - u.hurtB.z) / hTrue.z));
            let fEdge = select(1.0 - smoothstep(1.0, 1.7, mTrue), 1.0, isMe);
            albedo = mix(albedo, HURT_RED, fEdge);    // DARKER (user): 9.0 saturated the red channel outright and tonemapped to a flat pale pink — this still reads as emissive but keeps its colour as RED
            hurtGlow = u.hurtB.w * fEdge;                            // …and it lights itself — see the sun/AO block
          }
          // Nothing else is touched here on purpose: the light this throws on its surroundings is the
          // point light the wound already puts in the fflies lane, which falls off smoothly. Painting a
          // second region by hand is what produced the hard-edged red square around it.
        }
        if (skipW && pos.y < WLF + 1.0) {                            // swimming: caustic webs dance on everything below the surface
          albedo = albedo * (1.0 + select(0.0, 1.3, LG(13u)) * caust(floor(vec2<f32>(pos.x + u.winO.x, pos.z + u.winO.y)) + vec2<f32>(0.5)) * smoothstep(-0.02, 0.12, u.sunDir.y));
        }
        var foamK = 0.0;
        if (faceId == 6u && h.face == 2u && rd.y < -0.01 && u.pickZ.w < 0.4) {   // VOXEL OCEAN WAVES — still while the surface is more than half frozen
          let baseTop = pos.y;
          var tq = max(max(t - 3.8 / max(abs(rd.y), 0.08), t - 34.0), 0.0);
          let pW = ro + rd * tq;
          var cw = vec2<i32>(floor(vec2<f32>(pW.x, pW.z)));
          let sx2 = select(-1.0, 1.0, rd.x >= 0.0);
          let sz2 = select(-1.0, 1.0, rd.z >= 0.0);
          let adx = max(abs(rd.x), 1e-5); let adz = max(abs(rd.z), 1e-5);
          var tmx = ((f32(cw.x) + max(sx2, 0.0)) - pW.x) / (sx2 * adx) + tq;
          var tmz = ((f32(cw.y) + max(sz2, 0.0)) - pW.z) / (sz2 * adz) + tq;
          var wSide = false; var wCrest = -9.0; var whF = baseTop;
          for (var wi = 0; wi < 22; wi++) {
            let wxw = f32(cw.x) + u.winO.x; let wzw = f32(cw.y) + u.winO.y;
            let wv = gerstH(wxw, wzw);                                                                   // GERSTNER height field (see PRE) — same sum the JS floater mirror rides
            // ── THE FOAM RING STANDS A VOXEL PROUD (user 2026-08-05) ── done HERE, in the surface march, not
            // after it. Lifting t once the hit was already found only moved that pixel's DEPTH: the foam
            // kept the silhouette of the flat water because the pixels it should have grown into were never
            // tested against the water at all. Raising the column's surface height BEFORE the intersection
            // test is what gives the band a real edge you can see standing above the swell.
            // 4 probes, not the shading pass's 8: this only decides WHICH columns are lifted, and the ±3 ring
            // is the same shoreline the foam itself is drawn on.
            var lift = 0.0;
            { let ciL = vec3<i32>(cw.x, i32(baseTop - 0.5), cw.y);
              for (var s3 = 0; s3 < 4; s3++) {
                var nb3 = ciL;
                if (s3 == 0) { nb3.x += 3; } else if (s3 == 1) { nb3.x -= 3; } else if (s3 == 2) { nb3.z += 3; } else { nb3.z -= 3; }
                let nv3 = voxAt(nb3);
                if (nv3 != 0u && nv3 != WTv && nv3 != WBv) { lift = 1.0; break; }
              } }
            let wh = baseTop + floor(wv + 0.5) + lift;
            let tNext = min(tmx, tmz);
            if (ro.y + rd.y * tq <= wh) { t = tq; wSide = true; wCrest = wv; whF = wh; break; }          // SIDE face of a wave step
            if (ro.y + rd.y * tNext <= wh) { t = (wh - ro.y) / rd.y; wCrest = wv; whF = wh; break; }     // TOP face within this column
            if (tmx < tmz) { tq = tmx; tmx += 1.0 / adx; cw.x += i32(sx2); }
            else { tq = tmz; tmz += 1.0 / adz; cw.y += i32(sz2); }
            if (tq > t + 38.0) { break; }
          }
          if (wCrest > -8.0) {
            let hp = ro + rd * t;
            let vcW2 = vec3<i32>(i32(floor(hp.x)), i32(whF - 0.5), i32(floor(hp.z))) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));
            albedo = pal[h.vox].rgb * (0.90 + 0.20 * ivhash(vcW2));                       // grain follows the WAVE voxel, not the flat plane
            if (wSide) { albedo *= 0.74; }                                                // darker step sides give the swell its silhouette
            var foam = 0.0;                                                                       // no mid-water whitecaps — foam only rings the shoreline
            let ci = vec3<i32>(i32(floor(hp.x)), i32(baseTop - 0.5), i32(floor(hp.z)));
            for (var s2 = 0; s2 < 8; s2++) {                                              // probes at ±2 AND ±4 → a surf band twice as thick
              var nb = ci;
              let pr = select(2, 4, s2 >= 4);
              let a2 = s2 & 3;
              if (a2 == 0) { nb.x += pr; } else if (a2 == 1) { nb.x -= pr; } else if (a2 == 2) { nb.z += pr; } else { nb.z -= pr; }
              let nv = voxAt(nb);
              if (nv != 0u && nv != WTv && nv != WBv) {                                   // churned SURF ring wherever water meets land
                foam = max(foam, step(0.35, ivhash(ci + vec3<i32>(s2 * 13, 11, 5)) * (0.55 + 0.45 * sin(u.time * 1.6 + f32(ci.x * 7 + ci.z * 5) * 0.7))));
              }
            }
            if (foam > 0.5) { let tFo = (whF + 2.0 - ro.y) / rd.y; if (tFo > 0.0) { t = min(t, tFo); } }   // …and the shaded surface rides on top of that raised column (the +1 now comes from the lift in the march above). GUARDED: this block only runs for rd.y < -0.01, so an eye BELOW that plane (whF is baseTop + floor(wv+0.5) + lift, i.e. up to ~6 voxels above the water — where the swim spring parks you) makes the quotient NEGATIVE and min() took it. A t behind the camera made TEMPORAL drop the pixel, COMPOSITE shade it as unlit water and the reflection ray start behind the eye: dark blotches trailing the shoreline foam ring while swimming. Below the plane there is nothing to clamp to — the ray is already under it.
            foamK = clamp(foam, 0.0, 1.0) * 0.8;
            albedo = mix(albedo, FOAM_C, foamK);
          }
        }
        // (the old 50%-translucent bed mix moved to the COMPOSITE: it now arrives via a REAL refracted ray with
        // Beer–Lambert absorption — the G-buffer albedo stays the pure surface color + foam.)
        var seed = ((gid.x * 1973u) ^ (gid.y * 9277u) ^ (u32(u.frame) * 26699u)) | 1u;
        let sp = pos + h.n * 0.02;
        if (${location.search.includes('nosun') ? 0 : 1} == 1 && (dot(h.n, u.sunDir) > 0.0 || (FOLBACK && isFol(h.vox))) && u.sunDir.y > -0.04) {        // cone-jittered sun ray; skipped entirely at night (?nosun disables for A/B)
          let st = onbT(u.sunDir); let sb = cross(u.sunDir, st);
          let jitK = select(0.0, 0.028, LG(12u));                   // bit 12: sun PENUMBRA — off = a pin-sharp, perfectly hard shadow edge
          let sdir = normalize(u.sunDir + st * ((rand(&seed) * 2.0 - 1.0) * jitK) + sb * ((rand(&seed) * 2.0 - 1.0) * jitK));
          // -- BACK-LIT LEAF -- sp sits 0.02 along the NORMAL, which on a face the sun is BEHIND is the dark
          // side: the ray then walks straight back through the leaf's own voxel, hits it, and the pixel reads
          // as fully shadowed. sunV was therefore 0 on every pixel the transmission term targets, which is
          // exactly why it looked dead. Start on the sun side of the cell instead (1.25 voxels clears it), so
          // sunV answers the question transmission actually asks: does the sun reach the FAR face?
          // select() is false for every non-foliage surface here (the gate above only admits dot > 0 unless it
          // is a leaf), so ordinary shadows are bit-identical.
          let sunOrg = select(sp, pos + u.sunDir * 1.25, dot(h.n, u.sunDir) <= 0.0);
          let ceilY = f32((u32(u.fx) >> 8u) & 31u) * 32.0;           // world ceiling (u.fx bits 8+): no solid above it → a climbing ray is clear once past it
          let sCap = select(1200.0, min(1200.0, (ceilY - sunOrg.y) / max(sdir.y, 1e-4)), sdir.y > 1e-4);
          if (sCap <= 0.0) { sunV = 1.0; }                           // already above everything and going up — full sun, no ray
          else {
          if (LG(0u)) { let shT = trace(sunOrg, sdir, sCap, skipW);   // ── OCCLUSION, NOT NEAREST HIT ── this ray only ever asks "is anything in the way".
                        var occ = shT.t >= 0.0;                            // traceAll walks the terrain AND then bodyTrace to find which of the two is CLOSER,
                        if (!occ) { let shB = bodyTrace(sunOrg, sdir, sCap); occ = shB.t >= 0.0; }   // which this caller throws away. Once terrain has blocked the ray the answer
                        sunV = select(1.0, 0.0, occ); }                    // cannot change, so the body walk is pure waste. Bit-identical: occluded is occluded.
                        // (bodies included → a felled tree still casts a REAL shadow)
          else { sunV = 1.0; }                        // sun shadows OFF — every sunward surface fully lit
          }
          ${!(LIFE_UNI && (UNI_SEC & 1)) ? '' : 'if (sunV > 0.0) { let cs = creaSec(sp, sdir, SEC_R, sg0, sg1, sg2, sg3, secN); if (cs.x >= 0.0) { sunV = 0.0; creReact = max(creReact, cs.y); } }'}
          if (${UNI_CSHAD}) {   // CREATURE CAST SHADOWS — the sun ray tests the creature AABBs so they shadow ground/water. SKIPPED for a trace-injected creature pixel: its own box wraps the surface point and every sun ray would 'self-shadow' (cross-creature shadows are a lesser loss than a permanently dark body)
            for (var s = 0; s < 16; s++) {
              let CA = u.cshad[s * 2];
              if (CA.w < 0.5) { continue; }
              let d0 = sp - CA.xyz;                                  // both window-relative
              if (d0.x * d0.x + d0.z * d0.z > 1600.0) { continue; }  // a ground creature's shadow lands within a few metres
              let CB = u.cshad[s * 2 + 1];
              let he = vec3<f32>(CB.x, CB.y, CB.x);                  // half-extents (horizontal, vertical, horizontal) — axis-aligned box
              let inv2 = 1.0 / select(sdir, vec3<f32>(1e-5), abs(sdir) < vec3<f32>(1e-5));
              // GROWN box first. A pixel this ray only just misses is a pixel the shadow is about to
              // sweep over (or has just left), and it is exactly those pixels the 64-frame accumulator
              // smears. Flagging them reactive costs one extra slab test on the rare near-miss, and the
              // exact test below only runs when the grown one already hit — so the common case, a ray
              // nowhere near this creature, is unchanged.
              let heR = he * 1.6 + vec3<f32>(0.8);
              let taR = (CA.xyz - heR - sp) * inv2; let tbR = (CA.xyz + heR - sp) * inv2;
              let tnR = min(taR, tbR); let tfR = max(taR, tbR);
              if (max(max(tnR.x, tnR.y), tnR.z) >= min(min(tfR.x, tfR.y), tfR.z) || min(min(tfR.x, tfR.y), tfR.z) <= 0.05) { continue; }
              creReact = 1.0;                                        // moving shadow may touch this pixel → short history (see the reactive mask below)
              let ta2 = (CA.xyz - he - sp) * inv2; let tb2 = (CA.xyz + he - sp) * inv2;
              let tn2 = min(ta2, tb2); let tf2 = max(ta2, tb2);
              let teB = max(max(tn2.x, tn2.y), tn2.z);
              let tlB = min(min(tf2.x, tf2.y), tf2.z);
              if (teB < tlB && tlB > 0.05 && teB < 60.0) { sunV = 0.0; break; }   // the sun ray enters the creature box ahead → shadowed
            }
          }
        }
        if (${location.search.includes('noao') ? 0 : 1} == 1 && h.t < 500.0) {
          // Teardown AO: distance before collision ≈ indirect light (?noao disables for A/B). ONE ray per frame, not two — the profiler put the
          // white-noise 2-ray version at 4.3 ms (45% of the whole frame; incoherent directions thrash the DDA's L0 level
          // and diverge the warps). The temporal accumulator (maxHist 64) + the 12-tap spatial already average dozens of
          // frames, so the CONVERGED image is the sampler's mean either way — ray count only buys convergence speed. What
          // actually buys back the lost speed is stratification: an R2 low-discrepancy sequence over frames, IGN-offset
          // per pixel, covers the hemisphere evenly where rand() clumped, so 1 stratified ray converges about as fast as
          // the 2 white-noise rays it replaces. Same mean, ~half the trace cost.
          let fN = f32(u32(u.frame) & 1023u);
          let r1 = fract(ign(vec2<f32>(gid.xy)) + fN * 0.7548777);   // R2 sequence (plastic constants) — azimuth
          let r2 = fract(ign(vec2<f32>(gid.xy) + vec2<f32>(47.0, 17.0)) + fN * 0.5698403);   // decorrelated elevation lane
          let d = cosHemi(h.n, r1, r2);
          if (LG(1u)) { let ah = traceAll(sp, d, 24.0, skipW);      // bodies included → real contact AO and self-shadowing, no bake needed
                        var aT = select(24.0, ah.t, ah.t >= 0.0);
                        ${!(LIFE_UNI && (UNI_SEC & 2)) ? '' : '{ let cs2 = creaSec(sp, d, aT, sg0, sg1, sg2, sg3, secN); if (cs2.x >= 0.0) { aT = cs2.x; creReact = max(creReact, cs2.y); } }'}
                        skyV = clamp(aT / 24.0, 0.0, 1.0); }
          else { skyV = 1.0; }                        // AO OFF — no contact darkening anywhere
        } else { skyV = 0.85; }                                      // past 50 m AO detail is sub-pixel — flat ambient, the ray saved on every far pixel
        if (flakeHit && h.vox == SNV) { sunV = max(sunV, 0.6); skyV = max(skyV, 0.85);
          if (flakeFade < 1.0 && flakeUnder != 0u) { albedo = mix(pal[flakeUnder].rgb, albedo, flakeFade); }   // dissolving into what it is landing on
        }
        if (hurtGlow > 0.0) { sunV = max(sunV, hurtGlow * 0.55); skyV = max(skyV, hurtGlow * 0.75); }   // the flash glows in shadow too (user) — otherwise an animal hit under a canopy barely changed colour   // SOFTER (user): pinning both terms to FULL erased the creature's own sun/AO shading, so the whole animal went flat and blew out; lifting them part-way keeps its form readable while a wound in deep shade still reads

        if (!LG(15u)) { skyV = 0.0; }                              // bit 15: the SKY/AMBIENT term entirely — leaves only direct sun, so anything not in sunlight goes black   // FALLING FLAKES SCATTER — real snow is white from EVERY direction, so a flake never shows a dark shaded face. Without this floor, anti-sunward flakes shaded to ~sky luminance and vanished — the "snow band clipping" wedge the user saw (the void tracked the anti-sun azimuth, verified via the ?flakedbg emissive A/B). Scales with the sun/moon term downstream, so night stays subtle.
      }
      var lavaG = 0u;                                                  // soft light cast by nearby lava — deterministic probe, no flicker
      if (LG(3u)) {                                                    // LIGHT DEBUG bit 3: lava + firefly point lights
      // lava only exists at bedrock (y ≤ 8) and the probe reaches 18 down — pixels above y 28 can NEVER see glow, so 99.9% of pixels skip a whole ray
      if (t >= 0.0 && faceId != 8u && faceId != 7u && (ro.y + rd.y * t) < 28.0) {
        let pos2 = ro + rd * t + h.n * 0.02;
        let dh = trace(pos2, vec3<f32>(0.0, -1.0, 0.0), 18.0, true);
        if (dh.t >= 0.0 && (dh.vox == LVT || dh.vox == LVB || dh.vox == LVR || dh.vox == LVY)) {
          lavaG = u32(clamp((1.0 - dh.t / 18.0) * 14.0 + 0.5, 0.0, 14.0));
        }
      } else if (t >= 0.0 && faceId != 8u && faceId != 7u) {           // FIREFLY LIGHT — same 4-bit glow field (lava lives below y 28, fireflies above: never both).
        let pos2 = ro + rd * t;                                        // Teardown-style AREA light (juandiegomontoya breakdown): jittered target on the glow sphere,
        var best = 0.0; var bi = -1;                                   // sphere-light falloff window (inner full → outer ZERO), temporal dither — TAA resolves the noise.
        for (var f = 0; f < 8; f++) {
          let F = u.fflies[f];
          if (F.w <= 0.0) { continue; }
          let dv = F.xyz - pos2;
          let d2 = dot(dv, dv);
          if (d2 > 484.0) { continue; }                                // 2.2 m reach
          let win = 1.0 - smoothstep(220.0, 484.0, d2);                // "outer radius where the intensity falls off to zero" — kills the hard circle edge
          let k = F.w * max(dot(h.n, normalize(dv)), 0.0) * win / (1.0 + d2 * 0.06);
          if (k > best) { best = k; bi = f; }
        }
        if (bi >= 0 && best > 0.015) {
          var s2 = ((gid.x * 2467u) ^ (gid.y * 8837u) ^ (u32(u.frame) * 15013u)) | 1u;
          let jit3 = vec3<f32>(rand(&s2), rand(&s2), rand(&s2)) * 1.8 - vec3<f32>(0.9);   // "a ray to a RANDOM POINT on the light's surface" — soft area-light penumbra via TAA
          let dv = u.fflies[bi].xyz + jit3 - (pos2 + h.n * 0.02);
          let dist = length(dv);
          let sh = traceAll(pos2 + h.n * 0.02, dv / max(dist, 1e-4), max(dist - 0.6, 0.0), true);
          if (sh.t < 0.0) {
            let ign = fract(52.9829189 * fract(0.06711056 * f32(gid.x) + 0.00583715 * f32(gid.y)) + f32(u32(u.frame) & 63u) * 0.618034);   // temporal IGN dither — TAA melts the 14 quantization steps into a smooth gradient
            lavaG = u32(clamp(sqrt(best) * 5.2 + ign, 0.0, 14.0));     // sqrt-encoded: extra levels in the dim tail, where banding rings were most visible
          }
        }
      }
      }                                                                // …end LIGHT DEBUG bit 3 (glow)
      textureStore(gAlbedo, vec2<i32>(gid.xy), vec4<f32>(sqrt(albedo), (f32(faceId) + f32(lavaG) * 16.0) / 255.0));
      // ── REACTIVE MASK ── the temporal pass blends 1/hist per frame with hist up to maxHist = 64, so a
      // pixel newly covered or uncovered by a MOVING shadow needs ~64 frames to catch up: the shadow
      // visibly lags and smears behind the body that casts it. Flag any pixel whose sun ray could have
      // interacted with a rigid body and let TEMPORAL cap that pixel's history. Cheap — it reuses the same
      // enclosing-sphere test bodyTrace already does. Teardown carries an equivalent reactive mask in its
      // motion G-buffer for exactly this reason.
      var reactive = select(0.0, creReact, LG(4u));
      if (LG(4u) && t >= 0.0 && u.physC.y > 0.004) {                           // only while something is actually moving (eased, not switched — see physC.y): a settled trunk must not pin every nearby pixel's history forever (see physC)
        let sp2 = ro + rd * t;
        let dc2 = u.physBound.xyz - sp2;
        let sdir2 = select(u.sunDir, vec3<f32>(0.0, 1.0, 0.0), u.sunDir.y <= 0.0);
        let tc2 = dot(dc2, sdir2);
        if (tc2 > -u.physBound.w && dot(dc2, dc2) - tc2 * tc2 < u.physBound.w * u.physBound.w) { reactive = max(reactive, u.physC.y); }
        // …and the AO ray, which the sun-cylinder test above misses entirely. AO reaches 24 voxels in
        // EVERY direction, so the contact darkening a moving trunk casts on the ground beside it was
        // still converging at maxHist and trailed the trunk by a good half second.
        let aoR = u.physBound.w + 24.0;
        if (dot(dc2, dc2) < aoR * aoR) { reactive = max(reactive, u.physC.y); }
      }
      textureStore(gIrr, vec2<i32>(gid.xy), vec4<f32>(sunV, skyV, t, reactive));
      textureStore(slotOut, vec2<i32>(gid.xy), vec4<u32>(cSlot | (cAxis << 8u) | (select(0u, 1u, t >= 0.0 && isFol(h.vox)) << 11u), 0u, 0u, 0u));   // dynamic-life id + hit-axis bits — temporal identity/motion + composite true-normal reconstruction   // ...and bit 11 = IS THIS A LEAF. It rides here because the word had 21 unused bits and there is no spare g-buffer channel (gIrr is sun/sky/distance/history, gAlbedo is rgb + face); the composite cannot know a needle from bark otherwise. Every existing reader masks (& 255u, >> 8u & 7u), so this is invisible to them.
    }
  `;

  // strip scatter — copies a repacked 8-voxel X-strip from staging into its strided home in the world buffer
