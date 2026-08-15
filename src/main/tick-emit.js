    if (uniBirds.length) {                             // == UNIFIED PERCHED SONGBIRDS == staged like every other creature so PASS 1's floor and PASS 2's distance ranking both apply to them; injecting them AFTER the allocator measured 0 of 180 drawn, because PASS 2 always fills what is left.
      uniBirdWant = uniBirds.length; uniBirdN = 0;
      uniBirds.sort((a9, b9) => a9[6] - b9[6]);        // nearest first: EMIT_CAP is 216 staged poses and 180 birds on top of ~110 other creatures would overrun it, so the far ones are dropped BEFORE the ranking rather than corrupting it
      for (let ri = 0; ri < uniBirds.length && emitN < EMIT_CAP; ri++) {
        const R9 = uniBirds[ri], o4 = emitN * 16;
        const rx9 = R9[0] - cam[0], ry9 = R9[1] - cam[1], rz9 = R9[2] - cam[2];
        const th9 = R9[4], Hx9 = Math.sin(th9), Hz9 = Math.cos(th9);
        const m9 = R9[7], Xw9 = [Hz9 * m9, 0, -Hx9 * m9], Yw9 = [-Hx9, 0, -Hz9];   // SAME form as the creature emit's own frame, with mamMir's -1 in m9: the grid stamp's model->world map has determinant -1, so a right-handed emit frame draws the bird MIRRORED
        emitBuf[o4] = rx9 * right[0] + ry9 * right[1] + rz9 * right[2]; emitBuf[o4 + 1] = rx9 * up[0] + ry9 * up[1] + rz9 * up[2]; emitBuf[o4 + 2] = rx9 * fwd[0] + ry9 * fwd[1] + rz9 * fwd[2]; emitBuf[o4 + 3] = 1;
        emitBuf[o4 + 4] = Xw9[0] * right[0] + Xw9[2] * right[2]; emitBuf[o4 + 5] = Xw9[0] * up[0] + Xw9[2] * up[2]; emitBuf[o4 + 6] = Xw9[0] * fwd[0] + Xw9[2] * fwd[2]; emitBuf[o4 + 7] = R9[3];
        emitBuf[o4 + 8] = Yw9[0] * right[0] + Yw9[2] * right[2]; emitBuf[o4 + 9] = Yw9[0] * up[0] + Yw9[2] * up[2]; emitBuf[o4 + 10] = Yw9[0] * fwd[0] + Yw9[2] * fwd[2]; emitBuf[o4 + 11] = 0;
        emitBuf[o4 + 12] = right[1]; emitBuf[o4 + 13] = up[1]; emitBuf[o4 + 14] = fwd[1]; emitBuf[o4 + 15] = 0;   // LEVEL frame: Zw is world up, exactly as the emit's own fast path assumes
        emitWho[emitN] = R9[5]; emitAna[emitN] = 0; emitKnd[emitN] = LIFE_K_BIRD;
        emitAnc[emitN * 3] = R9[0]; emitAnc[emitN * 3 + 1] = R9[1]; emitAnc[emitN * 3 + 2] = R9[2];
        { const cx8 = emitBuf[o4], cy8 = emitBuf[o4 + 1], cz8 = emitBuf[o4 + 2];
          const r8 = LIFE_FRUST_R + (lifeIsDrawn(R9[5]) ? LIFE_FRUST_HYST : 0);
          emitVis[emitN] = (cz8 + r8 > 0 && (Math.abs(cx8) - fsX * cz8) * fnX <= r8 && (Math.abs(cy8) - fsY * cz8) * fnY <= r8) ? 1 : 0; }
        emitDp[emitN] = R9[6]; emitN++; uniBirdN++;
      }
      uniBirds.length = 0;
    }
    { const avail = DROP_SLOTS - dropCursor;           // ── FAIR-SHARE EMIT ── copy the chosen staged poses into the real drop slots; see LIFE_FLOOR
      const idx = emitIdx.subarray(0, emitN);
      for (let i = 0; i < emitN; i++) idx[i] = i;
      idx.sort((a, b) => (emitVis[b] - emitVis[a]) || (emitDp[a] - emitDp[b]));   // IN THE FRUSTUM first, then nearest — both passes below walk this one order
      emitTake.fill(0, 0, emitN); emitKcnt.fill(0);
      let nT = 0;
      for (let i = 0; i < emitN && nT < avail; i++) {  // PASS 1: the nearest LIFE_FLOOR of every kind present, so none can be shut out
        const j = idx[i], k = emitKnd[j];
        if (!emitVis[j] || emitDp[j] > LIFE_DRAW2) break;   // visible-first then near-first, so the first one off-frustum OR past the draw radius ends the guarantee for everybody. A kind with nothing on screen reserves nothing, which is the point: a slot held for a creature behind you is a slot not spent on one in front.
        if (emitKcnt[k] >= lifeFloorOf(k)) continue;
        emitKcnt[k]++; emitTake[j] = 1; nT++;
      }
      for (let i = 0; i < emitN && nT < avail; i++) {  // PASS 2: everything still free goes to whatever is closest, kind no longer considered
        const j = idx[i];
        if (emitTake[j]) continue;
        emitTake[j] = 1; nT++;
      }
      for (let i = 0; i < emitN; i++) {                // …emitted in distance order regardless of which pass claimed them
        const j = idx[i]; if (!emitTake[j]) continue;
        const src = j * 16, slot = dropCursor, o4 = dropOff(slot); dropCursor++;
        for (let k = 0; k < 16; k++) UF[o4 + k] = emitBuf[src + k];
        lifeSlotSet(slot, 2000 + emitWho[j], emitAnc[j * 3], emitAnc[j * 3 + 1], emitAnc[j * 3 + 2], Math.round(emitBuf[src + 7]), emitAna[j] === 1); }
    }
    if (wormPatchN) { if (CPROF) cpEvt |= 64; gpuPatch(wormPatch, false, wormPatchN, false); wormPatchN = 0; }   // track=false: a creature grid stamp is CONDUIT, not terrain (see SUP). Re-adjudicating the world every time a perched bird shifts its feet would be both wrong and, at 24 fps across ~180 stamped creatures, ruinous.   // one light brick-word patch for all worm (un)stamps this frame (incl. any queued by a pickup/toss)
    { // ── HIT FLASH ── the knife-wounded animal blinks red, then stops. Stepped at 24 fps like every
      // other animation here, so it reads as a deliberate blink rather than a smooth throb.
      // PUBLISHED HERE, not up with the rest of the uniforms: the four land mammals are grid-stamped and they
      // re-stamp themselves — walk plus the hit's bounce — in the creature loop just above. Reading their bounds
      // before that ran left the box a whole frame behind the voxels the GPU was about to shade, and a 1-voxel
      // lag against a 3-voxel half-height drops the animal's entire top row out of the box (measured: frames
      // losing 6-83% of the animal to no red at all). Nothing else here moves while grid-stamped, which is why
      // it was the mammals that broke and the perched birds that did not.
      const he = (now - HURT.t0) / HURT_MS;
      let hk = 0;
      if (he >= 0 && he < 1) { const hf = Math.floor(he * 12);                 // 12 phases over the blink, stepped at 24 fps like every other animation here
        hk = 1 - hf / 12;                               // ONE flash (user): full at the instant of the hit, fading out across the half second
        if (HURT.hold) hk = 1;                          // test hold (__vb.hurtTest(slot, true)): pins it lit so the tint can be checked without racing the capture
        if (HURT.slot >= 0) { const HB = wbf[HURT.slot]; if (HB && HB.init) hurtBox(HB); else hk = 0; }   // it died or despawned mid-blink — nothing left to stain. SLOT -1 IS NOT THAT CASE: it is the deliberate AABB path a shot SKY BIRD takes (birds live in birds[], not the pool, so there is no wbf entry to re-read — birdShot sets the box and birdRagTick re-publishes it every frame). Zeroing hk here made hurtB.w 0, which is the shader's whole gate, so a shot bird never flashed red and the HURT.k wound light below never lit either
      }
      UF[UF_HURTB] = HURT.cx - winOX; UF[UF_HURTB + 1] = HURT.cy; UF[UF_HURTB + 2] = HURT.cz - winOZ; UF[UF_HURTB + 3] = hk;
      HURT.k = hk;                                    // …and the wound CASTS light too (user). It cannot write a point-light slot from HERE — the ffLights
                                                      // publish runs later in this same tick and rewrites all 8 — so it goes into that list instead.
      UF[UF_HURTH] = HURT.hx; UF[UF_HURTH + 1] = HURT.hy; UF[UF_HURTH + 2] = HURT.hz;
      let hs = 0;                                     // ── WHICH SLOT IS THE WOUNDED ANIMAL DRAWN IN? ── the shader matches cSlot against this to
      // A RAGDOLL is drawn in NO slot — it is a rigid body now — so it must report 0 and be matched by the
      // box instead. Left to the search below, a ledger entry left over from the frame before it died would
      // point the flash at whatever creature has since been compacted into that slot.
      if (HURT.slot >= 0 && hk > 0 && !(wbf[HURT.slot] && wbf[HURT.slot].rag)) {   // paint the flash on its actual pixels. 0 = grid-stamped, and the bounds test covers that case.
        const want = 2000 + HURT.slot;                // lifeSlotSet stamps 2000 + the creature's pool slot into the ledger
        for (let s = 0; s < DROP_SLOTS; s++) if (lifeUid[s] === want) { hs = s + 1; break; }   // cSlot is the slot index PLUS ONE (0 means 'no creature here'). It is 8 bits wide in the SVGF slot texture, so slot 127 -> 128 still fits.
      }
      UF[UF_HURTH + 3] = hs; }
    UF[1103] = dropCursor;                             // pick2Y.w = live slot count — the composite loop's bound (always ≥ 9 so drops/cardinal/sparks are covered)
    cshadList.sort((a, b) => a[5] - b[5]);             // nearest 16 ground/water creatures cast shadows (the sun ray tests these boxes)
    for (let ci = 0; ci < 16; ci++) { const o6 = 1140 + ci * 8, C = cshadList[ci];
      if (C) { UF[o6] = C[0]; UF[o6 + 1] = C[1]; UF[o6 + 2] = C[2]; UF[o6 + 3] = 1; UF[o6 + 4] = C[3]; UF[o6 + 5] = C[4]; UF[o6 + 6] = 0; UF[o6 + 7] = 0; }
      else UF[o6 + 3] = 0; }
    for (let si2 = 0; si2 < 20; si2++) {               // ── clash/death sparks + death SMOKE → drop slots 5-24 ── real voxels on short arcs, fading out (sp4.smoke picks the look)
      const o3 = 68 + (5 + si2) * 16, sp4 = sparks3d[si2];
      const tS = sp4 ? (now - sp4.born) / 1000 : 1e9;
      if (!sp4 || tS > sp4.life) { UF[o3 + 7] = 0; if (sp4) sparks3d[si2] = null; continue; }
      const smoke = !!sp4.smoke, grav = smoke ? 1.5 : 85;   // SMOKE keeps RISING (near-zero gravity → floats up like a snowflake in reverse, doesn't fall back); SPARKS arc down fast
      let px4 = sp4.x + sp4.vx * tS, py4 = sp4.y + sp4.vy * tS - grav * tS * tS, pz4 = sp4.z + sp4.vz * tS;
      const a4 = sp4.ph + tS * (smoke ? (sp4.spin || 3) : 7), ca4 = Math.cos(a4), sa4 = Math.sin(a4);   // SMOKE voxels TWIRL like the snowflakes (user): about the vertical at a per-particle rate (~2-4.5 rad/s); sparks spin faster
      const Xs = [ca4, 0, sa4], Ys = [sa4, 0, -ca4], Zs = [0, 1, 0];   // spin about vertical (like a snowflake), right-handed
      let itn = (sp4.foam && FOAM_IT) ? FOAM_IT : ((sp4.red && HITRED_IT) ? HITRED_IT : SPARK_IT), vsc = 1.0;   // …the only place the three differ
      if (smoke) {                                     // ONE individual voxel, off the grid like a snowflake: a per-voxel wandering DRIFT (sin/cos, like the flakes' wind sway) as it floats up, then CENTRE it on its position so it spins about its own middle (not a corner)
        itn = SMOKE_IT; vsc = 1.0;                     // 10 cm voxel — exactly a snowflake's size (user)
        px4 += Math.sin(tS * 1.6 + sp4.ph) * 1.4;
        pz4 += Math.cos(tS * 1.9 + sp4.ph * 1.7) * 1.4;
        px4 -= (Xs[0] + Ys[0] + Zs[0]) * 0.5 * vsc;    // model centre (0.5,0.5,0.5)·scale → the drops path's min-corner anchor, so the spin is about the voxel's centre
        py4 -= (Xs[1] + Ys[1] + Zs[1]) * 0.5 * vsc;
        pz4 -= (Xs[2] + Ys[2] + Zs[2]) * 0.5 * vsc;
      }
      // ── A SPARK NO LONGER CASTS LIGHT (user 2026-08-05: "this spark lighting costs too much") ── it used
      // to push itself into the firefly point-light lane, and those are TRACED, SHADOWED area lights: four
      // embers off a single chop claimed half the eight slots and made the tracer resolve four extra lights
      // for the third of a second they lived, on every swing. The ember itself is unchanged — it is an
      // EMISSIVE voxel drawn through the drops path, which is what it looked like before the light was added.
      const rx = px4 - cam[0], ry = py4 - cam[1], rz = pz4 - cam[2];
      UF[o3] = rx * right[0] + ry * right[1] + rz * right[2]; UF[o3 + 1] = rx * up[0] + ry * up[1] + rz * up[2]; UF[o3 + 2] = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]; UF[o3 + 3] = vsc;
      UF[o3 + 4] = Xs[0] * right[0] + Xs[2] * right[2]; UF[o3 + 5] = Xs[0] * up[0] + Xs[2] * up[2]; UF[o3 + 6] = Xs[0] * fwd[0] + Xs[2] * fwd[2]; UF[o3 + 7] = itn;
      UF[o3 + 8] = Ys[0] * right[0] + Ys[2] * right[2]; UF[o3 + 9] = Ys[0] * up[0] + Ys[2] * up[2]; UF[o3 + 10] = Ys[0] * fwd[0] + Ys[2] * fwd[2]; UF[o3 + 11] = 1 - tS / sp4.life;
      UF[o3 + 12] = Zs[1] * right[1]; UF[o3 + 13] = Zs[1] * up[1]; UF[o3 + 14] = Zs[1] * fwd[1]; UF[o3 + 15] = 0;
    }
    if (HURT.k > 0) {                                  // ── THE WOUND ── a hit creature glows red for the half second it blinks, and that glow scatters
      const dxH = HURT.cx - P.x, dzH = HURT.cz - P.z;  // through the air like an ember does — same lane, same sort, so neither can clobber the other
      ffLights.push([HURT.cx - winOX, HURT.cy + 1, HURT.cz - winOZ, HURT.k * 2.4, dxH * dxH + dzH * dzH]);
    }
    ffLights.sort((a, b) => a[4] - b[4]);              // nearest glowing lights (fireflies + live sparks) win the 8 point-light slots — sorted AFTER the spark loop so a death flash can claim a slot
    for (let li = 0; li < 8; li++) { const o5 = 1108 + li * 4, L = ffLights[li];
      UF[o5] = L ? L[0] : 0; UF[o5 + 1] = L ? L[1] : 0; UF[o5 + 2] = L ? L[2] : 0; UF[o5 + 3] = L ? L[3] : 0; }
    {                                                  // ── DYNAMIC-LIFE MOTION + FLAGS (u.lifeMot) ── per-slot rigid world delta for temporal/TAA reprojection of
      for (let s9 = 0; s9 < DROP_SLOTS; s9++) {        // trace-injected creature pixels, plus the history-rejection bits the temporal pass needs. Cur→prev handoff below.
        const o9 = lifeMotOff(s9);
        let fl9 = 0, dx9 = 0, dy9 = 0, dz9 = 0;
        if (lifeUid[s9] < 0 || lifeAna[s9]) fl9 |= 1;  // analytic-only (empty slot, drop, spark, firefly) — TRACE skips it, composite draws it
        if (lifeUid[s9] >= 0) {
          if (lifeUid[s9] !== lifeUidPrev[s9]) fl9 |= 4;   // occupant changed (spawn / compaction churn / recycle) → history is another creature's, reject
          else {
            dx9 = lifeAnc[s9 * 3] - lifeAncPrev[s9 * 3]; dy9 = lifeAnc[s9 * 3 + 1] - lifeAncPrev[s9 * 3 + 1]; dz9 = lifeAnc[s9 * 3 + 2] - lifeAncPrev[s9 * 3 + 2];
            if (dx9 * dx9 + dy9 * dy9 + dz9 * dz9 > 900) fl9 |= 4;   // teleport-scale jump (>3 m in one frame) → hard reject
            if (lifeItem[s9] !== lifeItemPrev[s9]) fl9 |= 2;         // animation frame flipped → the model's voxels changed, irradiance history is stale
          }
        }
        UF[o9] = dx9; UF[o9 + 1] = dy9; UF[o9 + 2] = dz9; UF[o9 + 3] = fl9;
      }
      lifeDrawnPrev.fill(0);                           // …and the O(1) "was this pool creature drawn" flags, rebuilt from the same numbers
      for (let s9 = 0; s9 < DROP_SLOTS; s9++) { const u9 = lifeUid[s9]; if (u9 >= 2000) lifeDrawnPrev[u9 - 2000] = 1; }
      lifeUidPrev.set(lifeUid); lifeAncPrev.set(lifeAnc); lifeItemPrev.set(lifeItem);
      UF[1528] = lifeDbg; UF[1529] = LIFE_TRACE ? 1 : 0; UF[1530] = UNI_SEC; UF[1531] = 0;   // lifeCfg.z = which secondary rays see creatures (see UNI_SEC_DEF). REPORTING ONLY since the fold - no shader reads it any more, so changing it mid-session does nothing; pick the config with ?uni&sec=N or window.__SEC before load.
    }
    {                                                  // ── RIGID BODIES → u.physB ──
      // Same convention the creature models use: anchor + the three LOCAL axes, all expressed in CAMERA
      // space, so the shader's DDA runs in the body's own grid with no matrix inverse.
      // Bodies are published in WINDOW/WORLD space (not camera space) so ONE shader function can serve
      // the primary ray and every secondary ray. physBound is a sphere over all of them: shadow and AO
      // rays that cannot touch any body reject in a single compare.
      let nb = 0, bcx = 0, bcy = 0, bcz = 0, brad = 0;
      for (const b of PH.bodies) {
        if (nb >= PHYS_MAX || !b.gpu) continue;
        phQRot(b.q, PHX, PHAX); phQRot(b.q, PHY, PHAY); phQRot(b.q, PHZ, PHAZ);   // local axes → world
        const o = UF_PHYSB + nb * 20, g = b.gpu;
        const ax = b.pos[0] - winOX, ay = b.pos[1], az = b.pos[2] - winOZ;        // window-relative, matching u.camPos
        UF[o] = ax; UF[o + 1] = ay; UF[o + 2] = az; UF[o + 3] = b.scale === undefined ? 1.0 : b.scale;   // absorbing chunks shrink as they arrive
        UF[o + 4] = PHAX[0]; UF[o + 5] = PHAX[1]; UF[o + 6] = PHAX[2]; UF[o + 7] = g.bw;
        UF[o + 8] = PHAY[0]; UF[o + 9] = PHAY[1]; UF[o + 10] = PHAY[2]; UF[o + 11] = g.bh;
        UF[o + 12] = PHAZ[0]; UF[o + 13] = PHAZ[1]; UF[o + 14] = PHAZ[2]; UF[o + 15] = g.bd;
        UF[o + 16] = g.comL[0]; UF[o + 17] = g.comL[1]; UF[o + 18] = g.comL[2]; UF[o + 19] = g.off;
        bcx += ax; bcy += ay; bcz += az; nb++;
        if (!b.sleeping) reactT0 = now;            // …and WHEN a body was last in motion (see physC.y): the mask eases out from here instead of switching off the instant the solver parks it
      }
      if (nb) { bcx /= nb; bcy /= nb; bcz /= nb;
        for (const b of PH.bodies) {                     // radius = farthest body centre + its own half-diagonal
          if (!b.gpu) continue;
          const g = b.gpu, ax = b.pos[0] - winOX, ay = b.pos[1], az = b.pos[2] - winOZ;
          const half = Math.hypot(g.bw, g.bh, g.bd) * 0.5 + 1;
          const d = Math.hypot(ax - bcx, ay - bcy, az - bcz) + half;
          if (d > brad) brad = d;
        } }
      UF[UF_PHYSBOUND] = bcx; UF[UF_PHYSBOUND + 1] = bcy; UF[UF_PHYSBOUND + 2] = bcz; UF[UF_PHYSBOUND + 3] = brad;
      UF[UF_PHYSC] = nb; UF[UF_PHYSC + 1] = Math.max(0, 1 - (now - reactT0) / REACT_FADE); UF[UF_PHYSC + 2] = 0; UF[UF_PHYSC + 3] = 0;   // y = REACTIVE STRENGTH, eased — the mask keys off this, not off nb
    }
    device.queue.writeBuffer(uniBuf, 0, UF);
    if (CPROF) cpMark(6);

