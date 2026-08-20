  // ── GPU buffers ─────────────────────────────────────────────────────────────
  setLoad(94); await stage('uploading world…');
  const worldBuf = device.createBuffer({ size: W.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });   // COPY_SRC: __vb.gpudiff() reads the GPU world back to verify it matches W
  const uploadWorld = () => { const CH = 128 << 20;    // chunked — a single 1.5 GB writeBuffer would need a same-sized staging allocation
    for (let o = 0; o < W.byteLength; o += CH) device.queue.writeBuffer(worldBuf, o, W.buffer, o, Math.min(CH, W.byteLength - o)); };
  uploadWorld();
  const brickBuf = device.createBuffer({ size: bricks.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC, mappedAtCreation: true });   // COPY_SRC: __vb.bdiff() reads occupancy back to verify the dirty-word uploads
  new Uint32Array(brickBuf.getMappedRange()).set(bricks); brickBuf.unmap();
  const brick2Buf = device.createBuffer({ size: bricks2.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC, mappedAtCreation: true });
  new Uint32Array(brick2Buf.getMappedRange()).set(bricks2); brick2Buf.unmap();
  const wbrickBuf = device.createBuffer({ size: wbricks.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC, mappedAtCreation: true });
  new Uint32Array(wbrickBuf.getMappedRange()).set(wbricks); wbrickBuf.unmap();
  const uploadBricks = () => { if (CPROF) cpEvt |= 4; device.queue.writeBuffer(brickBuf, 0, bricks); device.queue.writeBuffer(brick2Buf, 0, bricks2); device.queue.writeBuffer(wbrickBuf, 0, wbricks); };
  // ── Z-BAND OCCUPANCY UPLOAD ── a z-band's bricks are CONTIGUOUS in both tables: the flat index is
  // bx + by*BX + bz*BX*BY (and cx + cy*B2X + cz*B2X*B2Y), with the z axis outermost. So a band that only
  // grew rows [gz0,gz1) touches exactly one slice, and the whole 384 KB table no longer has to be re-sent
  // for every 8-voxel band. BX*BY (12288) and B2X*B2Y (768) are both multiples of 32, so the bit ranges
  // land on u32 word boundaries and the slice is exact — no neighbouring bits are clipped or clobbered.
  // Only used where the caller KNOWS the change is confined to that z range (genBandGen's z path).
  const uploadBricksZ = (gz0, gz1) => {
    if (CPROF) cpEvt |= 4;
    const b0 = (gz0 >> 3) * BX * BY, b1 = ((gz1 + 7) >> 3) * BX * BY;   // L1 bit range
    const w0 = b0 >> 5, w1 = (b1 + 31) >> 5;
    device.queue.writeBuffer(brickBuf, w0 * 4, bricks.buffer, w0 * 4, (w1 - w0) * 4);
    device.queue.writeBuffer(wbrickBuf, w0 * 4, wbricks.buffer, w0 * 4, (w1 - w0) * 4);
    const c0 = (gz0 >> 5) * B2X * B2Y, c1 = ((gz1 + 31) >> 5) * B2X * B2Y;   // L2 bit range (32-voxel super-bricks)
    const v0 = c0 >> 5, v1 = (c1 + 31) >> 5;
    device.queue.writeBuffer(brick2Buf, v0 * 4, bricks2.buffer, v0 * 4, (v1 - v0) * 4);
  };
  // Coalesce a set of dirty u32 word indices into contiguous runs — one writeBuffer per run instead of per word.
  // Bridging a small gap re-uploads a few CLEAN words, which is always safe: the CPU array is authoritative,
  // so copying more of it can only bring the GPU closer to it, never further away.
  const WRUN_GAP = 16;
  const wrunTmp = [];
  const writeWordRuns = (buf, src, wset) => {
    if (!wset || !wset.size) return;
    wrunTmp.length = 0;
    for (const w of wset) wrunTmp.push(w);
    wrunTmp.sort((p, q) => p - q);
    let s = wrunTmp[0], e = s + 1;
    for (let i = 1; i < wrunTmp.length; i++) {
      if (wrunTmp[i] <= e + WRUN_GAP) { e = wrunTmp[i] + 1; continue; }
      device.queue.writeBuffer(buf, s * 4, src, s * 4, (e - s) * 4); s = wrunTmp[i]; e = s + 1;
    }
    device.queue.writeBuffer(buf, s * 4, src, s * 4, (e - s) * 4);
  };
  // ── FRAME-LEVEL BRICK UPLOAD BATCHING ── gpuPatch used to run writeWordRuns three times PER CALL, and
  // gpuPatch fires from ~20 sites a frame (snow landing, worm stamps, chop, dig, pickups, melt), so the
  // same buffers were re-visited over and over. The touched words now accumulate across the WHOLE frame
  // and coalesce ONCE, in patchEncode, ahead of the trace that reads them. Identical bytes and identical
  // bits reach the GPU — only the call count changes: 47 → 37 a frame walking, 223 → 85 in a snowstorm.
  //
  // Measured honestly: that call reduction is NOT worth any frame time. A seeded, uncontended A/B put
  // tick p50 at 2.20 ms both before and after, with the other percentiles moving in both directions —
  // i.e. noise. Dawn's per-writeBuffer overhead is evidently far below the 1-3 µs that would have made
  // 138 calls matter. A follow-up that bridged the gaps BETWEEN runs to cut calls further was tried and
  // removed: it cost 7× the bandwidth (27 → 184 KB a frame) and bought nothing measurable. This is kept
  // only because one flush per frame is simpler than three per call, not because it is faster.
  const dirtyBW = new Set(), dirtyC2W = new Set();
  const brickFlush = () => {
    if (dirtyBW.size) { writeWordRuns(brickBuf, bricks.buffer, dirtyBW); writeWordRuns(wbrickBuf, wbricks.buffer, dirtyBW); dirtyBW.clear(); }
    if (dirtyC2W.size) { writeWordRuns(brick2Buf, bricks2.buffer, dirtyC2W); dirtyC2W.clear(); }
  };
  // Scratch sets reused by gpuPatch, which used to allocate four Sets per call. Do NOT read this as a GC
  // fix: Chrome's sampling heap profiler puts the engine's REAL JS garbage at ~0.6 MB per 20 s, and only
  // 1 of 40 logged frame spikes was GC-attributed (36 were the terrain-stream budget). __vb.ft()'s
  // allocMBs/gcFrames look alarming but derive from usedJSHeapSize, which counts WebGPU staging memory,
  // so they mostly report upload traffic rather than garbage. This is tidiness, not a measured win.
  // Verified non-reentrant first: supPush, nvTouch and phWakeNear never call back into gpuPatch.
  const pgBset = new Set(), pgC2set = new Set();
  // Which palette ids belong to a GRID-STAMPED creature (mammals + perched songbirds). Rides in the unused
  // 4th float of each palette entry, so the tracer can ask "is this voxel part of an animal?" for free. The
  // hit flash needs it: its box is an AABB, and an AABB around an animal also contains the grass between its
  // legs and the ground under its belly — which is the red square the user saw stamped on the terrain.
  // Declared HERE, above palSync, so palSync can never read it through the temporal dead zone.
  const CREA_FLAG = new Uint8Array(256);
  const palBuf = device.createBuffer({ size: 256 * 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST, mappedAtCreation: true });
  { const f = new Float32Array(palBuf.getMappedRange());           // sRGB → linear; shaders work in linear
    for (let i = 0; i < palette.length; i++) { const p = palette[i]; f[i * 4] = Math.pow(p[0] / 255, 2.2); f[i * 4 + 1] = Math.pow(p[1] / 255, 2.2); f[i * 4 + 2] = Math.pow(p[2] / 255, 2.2); f[i * 4 + 3] = 0; } }
  palBuf.unmap();
  const palSync = () => { const f = new Float32Array(256 * 4);     // asset-editor imports addCol at RUNTIME — re-upload the whole palette (COPY_DST above exists for this)
    for (let i = 0; i < palette.length; i++) { const p = palette[i]; f[i * 4] = Math.pow(p[0] / 255, 2.2); f[i * 4 + 1] = Math.pow(p[1] / 255, 2.2); f[i * 4 + 2] = Math.pow(p[2] / 255, 2.2); f[i * 4 + 3] = CREA_FLAG[i]; }   // .a = grid-stamped-creature flag (see CREA_FLAG) — every pose builder re-syncs after registering its ids
    device.queue.writeBuffer(palBuf, 0, f); };
  const itemMapBuf = device.createBuffer({ size: Math.max(16, itemMapF32.byteLength), usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST, mappedAtCreation: true });   // COPY_DST: turning the arrow rewrites the bow strip's block in place (bowRefit)   // held/drop item voxel colors — see pickWGSL
  new Float32Array(itemMapBuf.getMappedRange()).set(itemMapF32); itemMapBuf.unmap();
  // ── DROP SLOTS ── 128: 25 fixed (4 dropped items, the editor cardinal, 20 death-burst sparks/smoke) +
  // the drawn flock + every trace-injected creature. 128 is exactly four 32-bit tile-mask words and exactly
  // 8 bits of slot id in the SVGF slot texture; a 129th slot needs a fifth word AND a wider id field.
  // The array's SECOND HALF (64..127) is appended at the END of the uniform struct as dropsB/lifeMotB —
  // growing `drops` in place would have shifted every hardcoded UF index past it (1092…1875) and fed each
  // field its neighbour's numbers with no error. dropOff/lifeMotOff below are the only places that know.
  const UNI_BIRDS = false;   // == WHAT ?uni ACTUALLY MOVES (user 2026-08-06) == only the THREE moving land mammals: bunny, armadillo, porcupine.
  // The 180 perched songbirds stay GRID-STAMPED and always will: they never move, so trace-injection buys them nothing, while stamped they get the exact SVGF/GI
  // solution for free. Measured why it matters: tracing them cost 14 slots and took FISH from 32/32 down to 7/32 and dragonflies to 0/3, because 24 traced mammals
  // plus 14 traced birds is 38 slots of new demand against 17 of headroom. Dropping the birds pays for the mammals outright. The SKUNK now traces too (user 2026-08-06) - all four land mammals are on the same path, and the songbirds alone pay for it.
  // budget reason - it is the asset-editor object, and the three world mammals are what the player watches move.
  // The line this draws is not ‘all life renders identically’ but ‘everything that MOVES renders identically’, which is the property that was actually producing bugs.
  const LIFE_UNI = !location.search.includes('nouni');   // == UNIFIED LIFE RENDERING: ON (user 2026-08-06, folded in) == was opt-in behind ?uni while it was measured; it now ships. ?nouni turns it off again - kept deliberately, because this is the first change here that alters how the game LOOKS in normal play, and being able to A/B it in one session is exactly how the slot-starvation regression (fish 32/32 -> 7/32) was caught before it shipped.   // == UNIFIED LIFE RENDERING (default OFF) == when set, the perched songbirds and the four land mammals TRACE-INJECT like every other creature and the secondary rays (sun + AO) test creature models, which is what closes the lighting gap the grid stamp was covering. Everything it adds is compiled in from here, so the OFF build is the shader we ship today.
  const UNI_SEC_DEF = 1 | 2 | 8;                      // the shipping config the cost study recommended: 1 creatures in the SUN ray, 2 creatures in the AO ray, 8 suppress the legacy 16-box u.cshad list (4 = water reflect/refract, deliberately NOT set: the intersector returns distance only, so it buys a dark silhouette rather than a reflection)
  const UNI_SEC_URL = (location.search.match(/[?&]sec=([0-9]+)/) || [])[1];   // ?uni&sec=N selects the config at BUILD time now that it is folded; window.__SEC (set before the page loads) does the same for the CDP harness
  const UNI_SEC = !LIFE_UNI ? 0 : (window.__SEC !== undefined ? (window.__SEC | 0) : (UNI_SEC_URL !== undefined ? (UNI_SEC_URL | 0) : UNI_SEC_DEF));   // == COMPILE-TIME == every ray test below folds to a literal here. It used to be read per pixel out of u.lifeCfg.z, which kept creaSec AND the legacy 16-box cast-shadow loop alive inside TRACE behind a dynamic branch: measured +0.169 ms at 1024x576 with every ray switched off, ~16% of the whole pass for a feature doing nothing. A whole-pass percentage is register pressure, not work, and the cure is to not compile the dead paths at all.
  const UNI_RAY = LIFE_UNI && (UNI_SEC & 3) !== 0;   // does ANY secondary ray look at creatures? If not, the intersector, its SEC_R radius, the grown tile mask and the 4 extra VIS words per tile are all skipped and VIS_W halves back to 4
  const UNI_CSHAD = (LIFE_UNI && (UNI_SEC & 8) !== 0) ? 'false' : ('LG(2u) && ' + (location.search.includes('nocshad') ? 0 : 1) + ' == 1 && sunV > 0.0 && cSlot == 0u');   // the legacy 16-box creature cast-shadow test, as a literal: under ?uni the real creature models cast the shadow instead, and emitting `false` deletes the loop rather than branching past it every pixel
  const VIS_W = UNI_RAY ? 8 : 4;                     // u32 words of tile mask per 8x8 tile: 4 = the primary mask alone, 8 = primary (0-3) + the SECONDARY grown mask (4-7)
  const UNI_CONST = UNI_RAY ? 'const SEC_R : f32 = 40.0;' : '';   // how far a SECONDARY ray looks for creatures, and the radius the VIS prepass grows each slot sphere by - the two MUST be the same number or a ray goes looking at slots the tile mask never listed. 40 is the AO ray's own 24 rounded up past a ground creature's cast-shadow reach, and it is the cutoff the 16-box cshad list already used.
  const UNI_FN = !UNI_RAY ? '' : `
    fn creaSec(roW : vec3<f32>, rdW : vec3<f32>, maxT : f32, w0 : u32, w1 : u32, w2 : u32, w3 : u32, dropN : i32) -> vec2<f32> {
      let rel = roW - u.camPos;
      let roC = vec3<f32>(dot(rel, u.right), dot(rel, u.up), dot(rel, u.fwd));
      let rdC = vec3<f32>(dot(rdW, u.right), dot(rdW, u.up), dot(rdW, u.fwd));
      var bt = maxT; var best = -1.0; var mov = 0.0;
      for (var di = 4; di < dropN; di++) {
        if (di >= 5 && di <= 24) { di = 24; continue; }
        { let mw = select(select(w0, w1, di >= 32), select(w2, w3, di >= 96), di >= 64); let mrem = mw >> (u32(di) & 31u); if (mrem == 0u) { di = i32(u32(di) | 31u); continue; } if ((mrem & 1u) == 0u) { di += i32(countTrailingZeros(mrem)) - 1; continue; } }
        let mvv = lifeMotV(di);
        if ((u32(mvv.w + 0.5) & 1u) != 0u) { continue; }
        let dXv = dropV(di * 4 + 1);
        let dit = i32(dXv.w + 0.5);
        if (dit < 1) { continue; }
        let dA = dropV(di * 4);
        let it3 = clamp(dit - 1, 0, ITEMN - 1);
        let eW = ITEMD[it3].x; let eD = ITEMD[it3].y; let eH = ITEMD[it3].z; let eOff = ITEMD[it3].w;
        if (eW < 1) { continue; }
        let vsD = dA.w;
        let ew2 = f32(eW) * 0.5; let ed2 = f32(eD) * 0.5; let eh2 = f32(eH) * 0.5;
        let radD = vsD * (sqrt(ew2 * ew2 + ed2 * ed2 + eh2 * eh2) + 1.0);
        let oc = dA.xyz - roC;
        let tcD = dot(oc, rdC);
        if (tcD < -radD || tcD - radD > bt) { continue; }
        if (dot(oc, oc) - tcD * tcD > radD * radD) { continue; }
        let dYv = dropV(di * 4 + 2); let dZv = dropV(di * 4 + 3);
        let ro0 = roC - dA.xyz;
        let roD = vec3<f32>(dot(ro0, dXv.xyz), dot(ro0, dYv.xyz), dot(ro0, dZv.xyz)) / vsD + vec3<f32>(ew2, ed2, eh2);
        var rdD = vec3<f32>(dot(rdC, dXv.xyz), dot(rdC, dYv.xyz), dot(rdC, dZv.xyz));
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
        var vcD = clamp(vec3<i32>(floor(roD + rdD * (teD + 1e-4))), vec3<i32>(0), vec3<i32>(eW - 1, eD - 1, eH - 1));
        let istD = vec3<i32>(sign(rdD));
        var vNxD = (vec3<f32>(vcD + max(istD, vec3<i32>(0))) - roD) * invD;
        var tHit = teD;
        var iMapD = eOff + vcD.x + vcD.y * eW + vcD.z * eW * eD;
        for (var i = 0; i < PICKSTEPS; i++) {
          if (ITEMMAP[u32(iMapD)].w > 0.99) {   // OPAQUE only, exactly as in the primary trace - a half-transparent wing does not stop a sun or AO ray either, so what you see and what the light sees still agree
            let tw = tHit * vsD;
            if (tw > vsD * 0.5 && tw < bt) { bt = tw; best = tw; mov = select(0.0, 1.0, dot(mvv.xyz, mvv.xyz) > 4e-4); }
            break;
          }
          if (vNxD.x <= vNxD.y && vNxD.x <= vNxD.z) { tHit = vNxD.x; vNxD.x += abs(invD.x); vcD.x += istD.x; iMapD += istD.x; }
          else if (vNxD.y <= vNxD.z) { tHit = vNxD.y; vNxD.y += abs(invD.y); vcD.y += istD.y; iMapD += istD.y * eW; }
          else { tHit = vNxD.z; vNxD.z += abs(invD.z); vcD.z += istD.z; iMapD += istD.z * eW * eD; }
          if (tHit * vsD > bt) { break; }
          if (any(vcD < vec3<i32>(0)) || any(vcD >= vec3<i32>(eW, eD, eH))) { break; }
        }
      }
      return vec2<f32>(best, mov);
    }
  `;
  const DROP_SLOTS = 128, DROP_HALF = 64;             // DROP_HALF = how many slots live in the original 'drops' array; every slot at or above it lives in 'dropsB'
  const PHYS_MAX = 24;                                // ── RIGID BODY CAPACITY (user 2026-08-11, was 16) ── the ONE number. PH.maxBodies takes it, physB's WGSL length is 5 vec4 x it, and EVERY offset below is derived from it.
  //   physB sits in the MIDDLE of the struct, so raising this moves the entire tail (physC, physBound, heldCfg, lgt, hurtB, hurtH, dropsB, lifeMot, dof). Those used to be literals in tick-emit/tick-camera/debug-api; they are named now, because a
  //   missed literal here does not throw - it silently feeds each field its neighbour's numbers (a wrong hit-flash box, held-item lighting reading the light-debug mask). Never write a literal float index at or past UF_PHYSB again.
  const UF_PHYSB = 1532;                              // physB base - everything BEFORE it is fixed history (see UF_OLD_LEN)
  const UF_PHYSC = UF_PHYSB + PHYS_MAX * 20;          // x = live body count, y = reactive strength
  const UF_PHYSBOUND = UF_PHYSC + 4;                  // sphere enclosing every live body - the one-compare reject
  const UF_HELDCFG = UF_PHYSBOUND + 4;   // heldCfg base: x = sun visibility, y = sky visibility, z = STACKBADGE count (was spare). Named, not inlined, so the badge does not depend on counting floats in a struct other work is actively appending to.
  const UF_LGT = UF_HELDCFG + 4;                      // light-debug bitmask / water reflection strength
  const UF_HURTB = UF_LGT + 4, UF_HURTH = UF_HURTB + 4;   // the knife's red hit-flash box, then its half-extents (+3 = the drop slot the wounded animal is drawn in)
  const UF_DROPSB = UF_HURTH + 4, UF_LIFEMOTB = UF_DROPSB + (DROP_SLOTS - DROP_HALF) * 16;
  const UF_DOF = UF_LIFEMOTB + (DROP_SLOTS - DROP_HALF) * 4;   // ── DEPTH OF FIELD ── x = focus distance (voxels; 0 = off), y = max CoC radius in canvas px. LAST in the struct, so no existing offset moves.
  // ── FLOATING HEARTS ── the health readout is five real voxels hanging in front of the eye (see the heart
  // block in COMPOSITE), so it needs a lane of its own: heart = {anchor.xyz in CAMERA space, voxel scale},
  // heartC = {item id, health in hearts, the gap between two of them, hurt kick 0..1}. Appended at the very
  // END, after dof, for the reason every field back here is: the JS writes this buffer at fixed float
  // indices, so a field inserted anywhere above silently feeds every one below it its neighbour's numbers.
  const UF_HEART = UF_DOF + 4;
  // ── THE HURT FLASH ── x = strength 0..1, y = the per-hit dither seed. Appended after heartC, which is the
  // rule every field back here follows: the JS writes this buffer at fixed float indices, so a lane inserted
  // anywhere above silently feeds every one below it its neighbour's numbers. See hurtV in the struct in PRE.
  const UF_HURTV = UF_HEART + 8;
  // ── RAIN SKY ── (user 2026-08-17: "when it rains can you make the sky more cloudy and darken the clouds as
  // well … also when it rains the sun should dim a bit") ── ONE 0..1 scalar drives the whole thing, and this is
  // where it rides: u.hurtV.w, the LAST float of the last vec4 of the whole uniform struct.
  //
  // WHY THERE, and not a lane of its own. Every field back here carries the same warning — the JS writes this
  // buffer at fixed float indices, so a field INSERTED anywhere shifts every one below it and silently feeds
  // each its neighbour's numbers — and the usual answer is to append a fresh vec4 at the end. That was not open
  // here: the struct those fields are DECLARED in lives in render/wgsl/pre.js, which this change does not own,
  // so a new field would have been a JS offset with no shader field behind it. u.misc, the obvious home, is full
  // in all four lanes (x cinematic vignette, y/z the storm edges, w the eye-inside-a-voxel fill) and every one
  // of them is actively read. hurtV.w is genuinely spare and provably so: UF is a zero-initialised Float32Array,
  // tick-camera writes only hurtV x/y/z (flash strength, per-hit dither seed, standing heart level), BLIT reads
  // only those three, and pre.js documents .w as spare. Being the FINAL float in the buffer also means taking it
  // cannot move anything: there is nothing below it. It costs no bytes and no bandwidth — UF already ends here.
  const UF_RAINK = UF_HURTV + 3;
  // ── STACK-BADGE PLACEMENT ── x/y = the CANVAS PIXEL the badge starts at (the held model's own projected
  // top-right corner, plus the panel's nudge), z size, w tilt.
  // A FRESH vec4 appended after hurtV rather than a borrowed lane, because hurtV has no spare float
  // left (see the rain note above) — and appending is free here for the reason the note gives: there is
  // nothing below it to shift. ui/hud.js owns the numbers, main/tick-camera.js writes them, BLIT reads them.
  const UF_BADGE = UF_HURTV + 4;
  // ── THE HUNGER LEVEL (user 2026-08-19: hunger re-introduced, drawn as GOLD pixels) ── a fresh vec4 appended
  // after badge, for the reason every lane back here is appended rather than borrowed: the JS writes this buffer
  // at fixed float indices, so a lane inserted anywhere above silently feeds every one below it its neighbour's
  // numbers. badge is full (x/y pixel, z size, w tilt) and hurtV has been full since UF_RAINK took its last
  // float, so there was nothing to borrow. Appending is free here — there is nothing below it to shift.
  // x = vitGoldLevel() 0..4. y/z/w spare, and deliberately: the next screen-space readout gets them without
  // moving anything, which is exactly the property this end of the buffer exists to have.
  const UF_VITG = UF_BADGE + 4;
  // ── THE CRAFT PREVIEW'S HAND (user 2026-08-19, the STONE AGE bench) ── a THIRD held-item lane, laid out
  // exactly like pickA/X/Y/Z and pick2A/X/Y/Z so COMPOSITE's hand loop reads it with the same four lines: A =
  // anchor xyz in camera space + voxel size, X/Y/Z = the item's axes, and X.w = the show id (0 = hidden).
  // Appended at the END rather than beside pick2, because everything from UF_PHYSB down is written at fixed
  // float indices and inserting a lane in the middle silently feeds every field below it its neighbour's
  // numbers. Sixteen floats is the price of the preview being a real voxel model rather than a 2D icon.
  const UF_PICK3 = UF_VITG + 4;
  // ── AND THE OFF-HAND'S STACK BADGE (user 2026-08-19: "the rock in the left hand doesnt have the stack
  // number") ── the same four numbers `badge` carries for the right hand: x/y = the canvas PIXEL the glyphs
  // start at, z = size, w = tilt. It needs its own because both are per-ITEM and the two hands hold different
  // things. The COUNT does not need a lane of its own — it goes in vitG.y, which that block reserved as spare
  // for exactly this: 'the next screen-space readout gets them without moving anything'.
  const UF_BADGE2 = UF_PICK3 + 16;
  // ── AND THE FIVE TUNING NUMBERS, HERE rather than beside snowOn in ui/settings.js ── COMPOSITE_SRC is invoked
  // from render/wgsl/vis.js, manifest line 35; ui/settings.js is line 60. A const declared with the storm state
  // is therefore in its temporal dead zone when the shader template interpolates it, and the symptom is the one
  // documented in the house rules: the bundle builds, the linter passes, and the game boots to a black screen
  // with nothing useful in the console. Anything the WGSL reads has to be declared at or above line 35; this
  // file is line 26. RAIN_SUN_DIM is read by BOTH sides (the shader's sunTintR and tick-camera's heldCfg.x), so
  // one declaration is also what stops the hand and the world disagreeing about how bright the sun is.
  const RAIN_CLOUD_THR = 0.165;    // how far cloudDen's density cut DROPS at full rain: 0.565 → 0.400. That noise is three value-noise octaves, mean ≈ 0.50 and σ ≈ 0.15, so 0.565 is the +0.44σ cut behind the existing "~35% coverage" note and 0.400 is −0.68σ, i.e. ~75%. Overcast WITH BREAKS IN IT, deliberately not a lid: past ~90% the deck loses all structure and reads as fog rather than as weather.
  const RAIN_CLOUD_DARK = 0.45;    // …and the deck falls to 55% of its fair-weather brightness. Applied to the march's ACCUMULATOR rather than inside the loop — acc is a linear sum in the per-step colour, so a uniform scale on that colour is exactly a uniform scale on acc, and doing it once after the loop costs one multiply per sky pixel instead of one per step.
  const RAIN_SKY_DESAT = 0.35;     // the blue BETWEEN the clouds goes 35% of the way to its own luminance…
  const RAIN_SKY_DIM = 0.20;       // …and 20% down with it. This pair is also the only handle this change has on the sun DISC and its glare, which are drawn inside skyColor() in pre.js — a file it does not own — so it is the literal reading of "the sun should dim a bit". At night it is what puts the stars and the moon behind the overcast.
  const RAIN_SUN_DIM = 0.35;       // the DIRECT sun at 65% of normal: about the half-stop of key light a thin overcast costs, which reads as flatter contrast rather than as evening. It rides on sunTint() — the ILLUMINANT'S colour — and deliberately NOT on dayScale(), the global light level: a cloud deck kills the direct beam and leaves the sky-diffuse term nearly intact, and that split is exactly what makes an overcast scene look FLAT instead of DARK. Scaling dayScale() would have taken the ambient, the water scatter, the ice, the bounce and the held item down together — "night", not "a bit" — and dayScale() lives in pre.js besides.
  // HOW MANY HEARTS THE BAR IS. Five, which is what the removed DOM readout drew and therefore what the
  // player already knows: VIT_HP_MAX is 20, so one heart is 4 HP. Read by BOTH sides — tick-camera turns
  // hp into hearts with it, and COMPOSITE's loop bound is interpolated from it — so the two cannot disagree.
  const HEART_N = 5;
  // WHERE THE ROW HANGS, in CAMERA space and voxel units - the same space the held item's pickA lives in, so
  // z 1.10 means "11 cm in front of the eye" exactly as the axe's 0.96 does. Centred under the crosshair and
  // low, which is the one part of the frame nothing else claims: the right hand sits at x +0.91, the second
  // rock at -0.75, and a swing drives the tool to the MIDDLE of the screen, well above this. At vs 0.055 and
  // z 1.10 a heart subtends ~3.4% of the window height (~30 px at 865) and the whole bar ~12% of its width -
  // big enough to read without a glance, small enough that the view is untouched.
  // `rig` = the SHARE OF THE HAND'S BOB the row rides (heldBob, written in tick-camera). It is the other half
  // of "in the game like the stone tools": a readout nailed dead still to the screen while the tool beside it
  // sways is a HUD layer no matter how it is lit. Not 1.0, and the reason is geometric rather than taste — the
  // hand's walk sway is +-0.075 in x, and on a row five hearts wide that is 1.5 whole heart-gaps of side-to-side
  // slosh; on a single tool the same number reads as a stride. Half of it keeps the row plainly attached to the
  // player and still readable at a sprint. The idle breathing term is inside heldBob too, and at +-0.011 it is
  // untouched by the halving either way.
  const HEART_POSE = { x0: -0.200, y: -0.52, z: 1.10, vs: 0.055, gap: 0.100, rig: 0.5 };   // x0 = the FIRST heart, so x0 = -gap*(HEART_N-1)/2 keeps the row centred
  let heartShow = 1;                                  // __vb.hearts(false) hides the row - the A/B lever for what the block costs, and the only way to take a clean screenshot of the frame without it
  const UF = new Float32Array(UF_BADGE2 + 4);   // …+ dof 3316..3319, heart 3320..3323, heartC 3324..3327, hurtV 3328..3331 (3331 = UF_RAINK, the rain-sky scalar — the last float of the buffer, see the note above)   // AT PHYS_MAX = 24: …+ heldCfg 2020..2023 (x = held-item sun visibility, y = its SKY visibility) + lgt 2024..2027 (light-debug bitmask) + hurtB 2028..2031 + hurtH 2032..2035 (the knife's red hit-flash box) + dropsB 2036..3059 + lifeMotB 3060..3315
  const dropOff = (s) => (s < DROP_HALF ? 68 + s * 16 : UF_DROPSB + (s - DROP_HALF) * 16);      // float index of drop slot s — the ONE place the two halves are stitched on the JS side
  const lifeMotOff = (s) => (s < DROP_HALF ? 1272 + s * 4 : UF_LIFEMOTB + (s - DROP_HALF) * 4);   // …and of its lifeMot entry
  const UF_OLD_LEN = UF_HELDCFG;   // …+ physB PHYS_MAX bodies x 5 vec4 from 1532 + physC + physBound → here (voxel rigid bodies). At 24 bodies: physB 1532..2011, physC 2012..2015, physBound 2016..2019 → 2020                   // …+ drops: 4 items end at 132, cardinal (slot 4) → 148, 4 clash sparks (slots 5-8) → 212, 55 creature slots (9-63: flyers/ducks/worms/lilies) → 1092; pick2 (left hand) 1092..1107; 8 firefly lights 1108..1139; 16 creature-shadow boxes (2 vec4 each) 1140..1267; misc 1268..1271 (x = cinematic vignette depth); lifeMot 64 vec4s 1272..1527 (per-slot world motion delta + flags — dynamic-life temporal reprojection); lifeCfg 1528..1531 → 1532
  const uniBuf = device.createBuffer({ size: UF.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const STRIPW = (8 * WX * WY) >> 2;                   // strip staging, u32 words (x-shifts scatter through a tiny compute pass)
  const stag = new Uint32Array(STRIPW);
  const stag64 = new Float64Array(stag.buffer);        // whole-f64 view for the repack loop — one store per 8 voxels (same trick blitSlab's narrow-run path uses)
  const stagBuf = device.createBuffer({ size: stag.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const scatBuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  // ── VOXEL PATCH SCATTER ── (creature stamps, snow landings, pickups, editor edits)
  // gpuPatch used to issue ONE 4-byte device.queue.writeBuffer per touched word. A busy frame touches
  // 2-3k words, and that many staging writes stalls the driver's upload ring: MEASURED as 25-37 ms frame
  // gaps with only ~2 ms of CPU inside tickBody (the stall lands in submit/present, outside our code).
  // Now the touched word INDICES are staged and applied by one writeBuffer + one tiny compute dispatch,
  // so the GPU call count is O(1) in the number of edited voxels instead of O(n).
  const PATCHMAX = 1 << 16;                            // word indices staged per dispatch; a bigger frame flushes early and loops
  const patchIdx = new Uint32Array(PATCHMAX);          // staged word indices (duplicates allowed — see below)
  const patchPairs = new Uint32Array(PATCHMAX * 2);    // (index, value) upload image, packed at FLUSH time
  let patchN = 0;
  const patchCntTmp = new Uint32Array(4);              // reused every frame: the count upload used to allocate a fresh Uint32Array per dispatch
  const patchBuf = device.createBuffer({ size: patchPairs.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const patchCnt = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  // ── RIGID BODY VOXELS ── one dense grid per live body (palette id per cell, 0 = empty), sub-allocated
  // back to back. Nothing is written into W: a detached body exists ONLY as this buffer plus its
  // transform, which is what keeps the world grid authoritative and free of moving-object stamps.
  // ── 2 << 20 -> 6 << 20 (2026-08-17, THE FELLED OAKS) ── this was sized against a PINE, whose dense body
  // box is 35*36*116 = 146k cells, so 2M held a dozen of them. An oak is a different object: the biggest
  // model's box is 114*112*114 = 1.455M cells, so ONE felled giant took 70% of the buffer and a second
  // made phReclaim evict the first - which is the 'the tree that fell has completely disappeared' failure,
  // and it would have looked like a felling bug rather than a buffer one. 6M cells = 24 MB of VRAM and
  // room for four giants down at once; 4 << 20 would buy only two, which a player clearing a stand hits
  // immediately. The GPU cost is address space, not bandwidth: the trace only reads the cells a body
  // actually occupies.
  const BODYCAP = 6 << 20;                             // 6M cells = 24 MB; a whole pine box is 35*36*116 = 146k, a giant OAK's is 1.455M
  const bodyBuf = device.createBuffer({ size: BODYCAP * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
  let bodyTop = 0;                                     // bump allocator; reset when no body references it

  // ── GERSTNER WAVE FIELD ── (user: physically-based water) 4 deep-water waves — a long primary swell, a crossing
  // secondary, two short chop waves — with the deep-water dispersion ω = c·√(g·k) (g ≈ 98 vox/s², slowed 0.42× for a
  // calm-lake feel). HEIGHT is the plain 4-cos sum (what the voxel-stepped crests + the JS floater mirror quantize);
  // the SHADING normal adds the Gerstner Q (crest-pinch) term, so surfaces read sharp-crested without breaking the
  // column grid. ONE table drives the shader AND the JS mirror — they can never drift apart.
  const GW = [                                         // [dirX, dirZ, k = 2π/λ, amp (vox), Q steepness, phase]
    [0.834, 0.552, 6.2831853 / 52, 1.30, 0.55, 0.0],
    [-0.416, 0.909, 6.2831853 / 23, 0.72, 0.50, 2.1],
    [0.966, -0.259, 6.2831853 / 11, 0.34, 0.45, 4.4],
    [0.309, -0.951, 6.2831853 / 6.7, 0.20, 0.40, 1.3]];
  const DUCK_SWAY = 0.5;                               // how much of the swell's rise and fall a DUCK actually rides — halved (user 2026-08-05); see the wave-riding block
  const GWOM = GW.map((w) => 0.21 * Math.sqrt(98 * w[2]));   // dispersion per wave — HALVED (user 2026-08-02: water 50% slower). ONE table drives the shader AND the JS floater mirror, so ducks/lilies stay bit-matched automatically.
  const gerstHJS = (wx, wz, t) => {                    // the JS mirror — ducks/lilies ride EXACTLY the surface the shader draws
    let h = 0; for (let i = 0; i < 4; i++) { const w = GW[i]; h += w[3] * Math.cos(w[2] * (w[0] * wx + w[1] * wz) - GWOM[i] * t + w[5]); } return h; };
  const GERSTH_WGSL = GW.map((w, i) =>
    `h_ += ${w[3]} * cos(${(w[2] * w[0]).toFixed(7)} * wx + ${(w[2] * w[1]).toFixed(7)} * wz - ${GWOM[i].toFixed(7)} * u.time + ${w[5]});`).join('\n      ');
  const GERSTN_WGSL = GW.map((w, i) =>
    `{ let ph = ${(w[2] * w[0]).toFixed(7)} * wx + ${(w[2] * w[1]).toFixed(7)} * wz - ${GWOM[i].toFixed(7)} * u.time + ${w[5]};
        let s_ = sin(ph); nx_ += ${(w[0] * w[2] * w[3]).toFixed(7)} * s_; nz_ += ${(w[1] * w[2] * w[3]).toFixed(7)} * s_; ny_ -= ${(w[4] * w[2] * w[3]).toFixed(7)} * cos(ph); }`).join('\n      ');

