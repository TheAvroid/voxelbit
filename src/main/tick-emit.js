    // ── THE ASSET EDITOR'S FREE-MOVING EXHIBITS ── staged into emitBuf exactly as a world creature is, which is
    // the whole point of them: a lane is grid-stamped and therefore integer-positioned and axis-aligned, while
    // anything that arrives here carries a float position and a free heading and is depth-tested against the
    // scene. Nothing in the render path was ever gated on the editor — ED.on appears three times in the creature
    // code and all three are in the SIM (stepping and spawning), so the emit has always been able to draw these;
    // it simply had nothing feeding it. ui/editor.js edExStage builds the list, edExStep moves it.
    // The frame is the creature emit's own level frame (main/tick-creatures.js: Xw = [Hz, 0, -Hx], Yw = [-Hx, 0,
    // -Hz], Zw = world up), copied rather than derived so an exhibit faces its heading the way every other
    // creature does. Getting that convention wrong by hand is what had the first ladybug flying backwards.
    if (ED.on && ED.ex.length) {
      for (let ei = 0; ei < ED.ex.length && emitN < EMIT_CAP; ei++) {
        const E = ED.ex[ei], o4 = emitN * 16;
        const ex9 = E.hx + E.x, ey9 = E.hy + E.y, ez9 = E.hz + E.z;
        const rx9 = ex9 - cam[0], ry9 = ey9 - cam[1], rz9 = ez9 - cam[2];
        // ── THIS MODEL'S HEAD IS AT +y, AND THE CONVENTION IS −y ── every world creature is authored with its
        // head down model −y, which is why the creature basis (main/tick-creatures.js) is Yw = −flight: model
        // +y points backwards, so the head leads. Measured off ladybug.vox rather than guessed from a
        // screenshot, which is what got this wrong twice: down its 4-deep axis, y=0 is 8 voxels and mostly red
        // (the elytra) while y=2 and y=3 are 2 voxels each and entirely dark — the narrow black head, at +y.
        // Its 8-wide axis is the WINGSPAN, not the body: the dark band there runs down the MIDDLE (x=3,4),
        // which is the elytra split, not a head at one end.
        // So half a turn on the RENDER heading, and `flip` per exhibit rather than a constant, because the next
        // model may well be authored the usual way round. The motion still integrates E.th untouched, so it
        // goes exactly where it was going and now faces it.
        const thR = E.flip ? E.th + Math.PI : E.th;
        const Hx9 = Math.sin(thR), Hz9 = Math.cos(thR);
        const Xw9 = [Hz9, 0, -Hx9], Yw9 = [-Hx9, 0, -Hz9];
        emitBuf[o4] = rx9 * right[0] + ry9 * right[1] + rz9 * right[2]; emitBuf[o4 + 1] = rx9 * up[0] + ry9 * up[1] + rz9 * up[2]; emitBuf[o4 + 2] = rx9 * fwd[0] + ry9 * fwd[1] + rz9 * fwd[2]; emitBuf[o4 + 3] = 1;
        emitBuf[o4 + 4] = Xw9[0] * right[0] + Xw9[2] * right[2]; emitBuf[o4 + 5] = Xw9[0] * up[0] + Xw9[2] * up[2]; emitBuf[o4 + 6] = Xw9[0] * fwd[0] + Xw9[2] * fwd[2];
        emitBuf[o4 + 7] = E.item0 + (E.ph === 'land' ? 0 : Math.floor(E.aclk) % E.n);   // E.aclk is an accumulated FRAME position (ui/editor.js), so a rate change speeds the beat up instead of jumping it — and a LANDED exhibit holds frame 00 (user)   // its own clock, so the whole school does not flap in lockstep
        emitBuf[o4 + 8] = Yw9[0] * right[0] + Yw9[2] * right[2]; emitBuf[o4 + 9] = Yw9[0] * up[0] + Yw9[2] * up[2]; emitBuf[o4 + 10] = Yw9[0] * fwd[0] + Yw9[2] * fwd[2]; emitBuf[o4 + 11] = 0;
        emitBuf[o4 + 12] = right[1]; emitBuf[o4 + 13] = up[1]; emitBuf[o4 + 14] = fwd[1]; emitBuf[o4 + 15] = 0;
        // emitMust = 1: an exhibit has NO second render path. A world mammal that loses the drop-slot
        // competition still has its grid stamp to fall back on; this has nothing, and would simply vanish.
        emitWho[emitN] = ei; emitAna[emitN] = 0; emitKnd[emitN] = LIFE_K_FLYER; emitMust[emitN] = 1;
        emitAnc[emitN * 3] = ex9; emitAnc[emitN * 3 + 1] = ey9; emitAnc[emitN * 3 + 2] = ez9;
        { const cx8 = emitBuf[o4], cy8 = emitBuf[o4 + 1], cz8 = emitBuf[o4 + 2];
          emitVis[emitN] = (cz8 + LIFE_FRUST_R > 0 && (Math.abs(cx8) - fsX * cz8) * fnX <= LIFE_FRUST_R && (Math.abs(cy8) - fsY * cz8) * fnY <= LIFE_FRUST_R) ? 1 : 0; }
        emitDp[emitN] = rx9 * rx9 + ry9 * ry9 + rz9 * rz9; emitN++;
      }
    }
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
        // ── AND A PERCHED BIRD HAS NO FALLBACK EITHER (user 2026-08-21: "I believe I saw a pink bird
        // disappear") ── emitMust is the bit sim/life/slots.js added on 2026-08-20 for the land mammals, whose
        // note says "the four land mammals are the only life with TWO render paths". They are not. A perched
        // bird has exactly the same deal and gives up exactly the same thing: crossing inside UNI_BIRD_R it
        // calls unstampWorm and drops its grid stamp (sim/life/stamped.js), then relies on a drop slot. Lose
        // the competition with the stamp already surrendered and it is drawn by NEITHER path — the precise
        // failure that note describes for an armadillo at 291 voxels, on a population of 421 instead of 96.
        // IT WAS ALSO READING A STALE BIT. emitMust is a frame-reused Uint8Array and only the creature loop
        // (main/tick-creatures.js) writes it; this block never did. So a bird inherited whatever creature held
        // that emit index last frame, which is why the symptom is intermittent and why it moves around: the
        // same bird is "must draw" or not depending on what was staged before it.
        emitMust[emitN] = 1;
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
      for (let i = 0; i < emitN && nT < avail; i++) {  // PASS 0: bodies with NO other render path (see emitMust) — a mammal that dropped its grid stamp to be traced
        const j = idx[i];
        if (!emitMust[j]) continue;
        if (!emitVis[j] || emitDp[j] > LIFE_DRAW2) break;   // same visible-then-near order as the passes below, so this cannot reserve a slot for a speck
        emitTake[j] = 1; nT++; emitKcnt[emitKnd[j]]++;      // counted against its kind, so PASS 1 does not hand the same kind its floor a second time
      }
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
      // ── AND THE SLOTS GO OUT IN A STABLE ORDER (user 2026-08-27: "audit the entire game for this flicker
      // effect. seems to happen on unstatic objects") ── the three passes above decide WHO is drawn; this loop
      // only decides WHICH SLOT each one lands in, and it was walking the distance order. A drop slot is not a
      // neutral container: lifeUid/lifeUidPrev are compared per slot and a changed occupant sets the history-
      // rejection bit a few lines below (`fl9 |= 4`, "compaction churn"), so every time two creatures swapped
      // distance rank they swapped slots and BOTH had their temporal history thrown away — while being drawn
      // perfectly, every frame. MEASURED beside an oak wood: 1,629 slot changes in 1,200 frames, 1.36 a frame,
      // and all 53 drawn creatures moved at least once — roughly nine history resets a second, each. That is a
      // shimmer on everything that moves, which is exactly the shape of the report.
      // Sorting the chosen set by emitWho — the creature's own pool slot, an identity that never changes —
      // means a creature keeps its drop slot for as long as it stays in the drawn SET, and the only churn left
      // is the real one: something entering or leaving that set. Order is free to change because nothing reads
      // it: the trace takes the NEAREST hit (`tHit < bestT`), not the first, and the slots stay contiguous, so
      // UF[1103] still bounds the shader's loop exactly as before.
      idx.sort((a, b) => (emitTake[b] - emitTake[a]) || (emitWho[a] - emitWho[b]));   // taken first, then by identity — in place, no allocation
      for (let i = 0; i < emitN; i++) {
        const j = idx[i]; if (!emitTake[j]) break;
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
    // ── NO LEAVES WHILE THE EYE IS INSIDE A CROWN (user 2026-08-19: "when Im inside the tree I can see the
    // leaves falling, remove this from happening") ── canopy is SEE-THROUGH to the primary ray (foliaTab, the
    // isFol test in render/wgsl/dda.js) so that a crown you clip into does not black the screen out. The side
    // effect is that everything inside it stays visible against that darkness, and the falling leaves — which
    // spawn off the crown's own underside and so are always right there — read as drifting through solid wood.
    // One voxel read a frame, and it zeroes the whole petal band rather than refusing to spawn: a leaf already
    // in the air has to go too, and it comes back the moment you step out of the tree.
    // ── …AND THE ANSWER IS LATCHED (user 2026-08-28: "the falling leaves still flicker sometimes") ── the read
    // below is one voxel and it is exact, but a crown is see-through geometry the camera brushes constantly on
    // any walk through a wood, and this gate is GLOBAL: a graze of three frames blanks every falling leaf in the
    // world and then restores them. MEASURED, 2,664 frames of walking: all 286 all-dark frames were
    // eye-in-foliage frames — 1:1, so the gate is the only cause — in six runs of 3/4/4/4/131/140. The 131 and
    // 140 are the feature; the four short ones are the flicker. PETAL_HIDE_LAG (sim/particles.js) makes a new
    // answer wait until it has held, so a graze can never move the latch. See the note there for why it is
    // symmetric and why it is in milliseconds rather than frames.
    const eyeFolRaw = (() => { const ey = Math.floor(smoothEye);
      if (ey < 1 || ey >= WY) return false;
      const v = W[gwrap(Math.floor(P.x), WX) + ey * WX + gwrap(Math.floor(P.z), WZ) * WX * WY];
      return !!(v && foliaTab[v]); })();
    if (eyeFolRaw !== PETAL_HIDE.raw) { PETAL_HIDE.raw = eyeFolRaw; PETAL_HIDE.since = now; }   // the raw answer changed — start its clock over
    if (PETAL_HIDE.on !== PETAL_HIDE.raw && now - PETAL_HIDE.since >= PETAL_HIDE.lag) PETAL_HIDE.on = PETAL_HIDE.raw;   // …and it only lands once it has held
    const eyeFol = PETAL_HIDE.on;
    for (let si2 = 0; si2 < sparks3d.length; si2++) {   // ── clash/death sparks + death SMOKE + tears + POLLEN → drop slots 5-28 ── read off the array rather than a literal 20, so growing the pool needs one edit and not three ── real voxels on short arcs, fading out (sp4.smoke picks the look)
      const sl3 = sparkSlot[si2];
      if (sl3 < 0) { const spX = sparks3d[si2]; if (spX && (now - spX.born) / 1000 > spX.life) sparks3d[si2] = null; continue; }   // no slot = not alive; still reap the expired one
      const o3 = dropOff(sl3), sp4 = sparks3d[si2];   // dropOff, not 68 + slot*16 — a compacted band still has to stitch the two halves   // …9, not 5: the item-drop band is 0-7 now and the cardinal took 8 (user 2026-08-20)
      const tS = sp4 ? (now - sp4.born) / 1000 : 1e9;
      if (!sp4 || tS > sp4.life) { UF[o3 + 7] = 0; if (sp4) sparks3d[si2] = null; continue; }
      if (eyeFol && sp4.petal) { UF[o3 + 7] = 0; continue; }   // …and the leaf is hidden, not retired: sparks3d[si2] is left alone so it resumes falling from the right place when the eye leaves the crown
      const smoke = !!sp4.smoke, petal = !!sp4.petal, grav = smoke ? 1.5 : (petal ? 0 : 85);   // the HUNGER fleck briefly had a sixth of this (a `light` flag) to make it linger; it read as floating rather than as something thrown off a body, and is gone — gold and blood are one burst with one set of physics again   // a PETAL is not ballistic at all: it descends at a constant rate, so gravity is off and the fall is written straight into py4 below   // SMOKE keeps RISING (near-zero gravity → floats up like a snowflake in reverse, doesn't fall back); SPARKS arc down fast
      let px4 = sp4.x + sp4.vx * tS, py4 = sp4.y + sp4.vy * tS - grav * tS * tS, pz4 = sp4.z + sp4.vz * tS;
      // ── THE PLAYER'S MOMENTUM, BLED OFF ── ivx/ivz is the body's own velocity at the moment the burst left it
      // (sim/vitals.js). Integrating v0*exp(-t/T) gives v0*T*(1 - exp(-t/T)): the fleck matches the player for
      // the first instant, then the carry converges on a FIXED offset of v0*T and stops contributing at all.
      // At a 60 vox/s sprint that is 6 voxels of travel, 86% of it inside the first 0.2 s — so the fleck comes
      // off a moving body and then behaves exactly as it does when the player is standing still, which is what
      // the user asked for. T is the ONE dial: bigger carries further, and 0 is the old leave-it-behind bug.
      if (sp4.ivx || sp4.ivz) { const cK = SPK_CARRY_TAU * (1 - Math.exp(-tS / SPK_CARRY_TAU));
        px4 += sp4.ivx * cK; pz4 += sp4.ivz * cK; }
      const a4 = petal ? 0 : sp4.ph + tS * (smoke ? (sp4.spin || 3) : 7), ca4 = Math.cos(a4), sa4 = Math.sin(a4);   // …and it does NOT spin (user): pinning the angle to 0 leaves the basis below at identity, which is the whole of it — a petal rocks, it does not yaw   // SMOKE voxels TWIRL like the snowflakes (user): about the vertical at a per-particle rate (~2-4.5 rad/s); sparks spin faster
      const Xs = [ca4, 0, sa4], Ys = [sa4, 0, -ca4], Zs = [0, 1, 0];   // spin about vertical (like a snowflake), right-handed
      let itn = petal ? (sp4.pit || PETAL_IT) : ((sp4.foam && FOAM_IT) ? FOAM_IT : ((sp4.red && HITRED_IT) ? HITRED_IT : ((sp4.gold && HITGOLD_IT) ? HITGOLD_IT : SPARK_IT))), vsc = 1.0;   // …the only place the three differ
      if (petal) {                                     // ── THE CRADLE ── one sine along the petal's own fixed horizontal axis, so it swings left-right across the fall line the way a leaf does. The descent stays linear: no acceleration, no yaw, nothing that reads as tumbling.
        const sw4 = Math.sin(tS * sp4.rate + sp4.ph) * sp4.sway;   // sp4.sway, not the PETAL_SWAY constant: each petal is issued its OWN swing width at birth (see spawnPetal), so a drift differs in the size of its arcs as well as in their phase
        py4 = sp4.y - PETAL_FALL * tS;                 // …plus the WORLD WIND the petal was BORN into (sp4.wvx/wvz, frozen in spawnPetal)
        px4 = sp4.x + sp4.ax * sw4 + (sp4.wvx || 0) * tS;
        pz4 = sp4.z + sp4.az * sw4 + (sp4.wvz || 0) * tS;
        // ── AND IT LANDS ON THE GROUND IT IS OVER NOW (user 2026-08-20: "the swaying falling leaves seem to be
        // glitching out") ── spawnPetal fixes `life` at birth as (spawnY - ground UNDER THE TREE) / PETAL_FALL,
        // which was exact while a petal fell straight down. The wind carries it up to ~35 voxels sideways, so
        // the ground it actually arrives over is not the ground it was measured against: onto rising ground it
        // kept falling and BURROWED (measured 136 samples inside terrain, up to 6.9 voxels deep), and over
        // falling ground its clock ran out and it BLINKED OUT in mid-air (13 of them, up to 5 voxels up).
        // Both are one line of arithmetic that stopped being true the moment the petal could travel.
        // Re-aimed every frame at the column it is really above: clamp so it can never sink, and re-time the
        // remaining fall so it neither dies early nor outlives PETAL_MAXLIFE. One hmap read for at most 32
        // petals — the same lookup the fish and the flakes already do per body, per frame.
        // ── THE SAME HEIGHT SOURCE spawnPetal USED (user 2026-08-20: "the swaying voxel leafs keep
        // dissapearing") ── this first read hmap while spawnPetal measures the ground with H(). Where the two
        // disagree the petal is judged to have ALREADY LANDED on the frame it is born, `life` is re-timed to
        // its current age, and it expires immediately — a leaf that appears in the crown and is gone before it
        // has fallen a voxel. One source for both, so "where is the ground here" cannot have two answers.
        { const gp = H(Math.round(px4), Math.round(pz4));
          if (py4 <= gp) py4 = gp;                     // landed: rest ON the hillside, never inside it
          sp4.life = Math.min(PETAL_MAXLIFE, tS + Math.max(0, (py4 - gp) / PETAL_FALL)); }   // …and it lives exactly as long as the fall it still has left
      } else if (smoke) {                                     // ONE individual voxel, off the grid like a snowflake: a per-voxel wandering DRIFT (sin/cos, like the flakes' wind sway) as it floats up, then CENTRE it on its position so it spins about its own middle (not a corner)
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
      UF[1528] = lifeDbg; UF[1529] = LIFE_TRACE ? 1 : 0; UF[1530] = UNI_SEC; UF[1531] = lifeSlotBase;   // lifeCfg.w = first NON-particle slot, now the COMPACTED one   // lifeCfg.w = the first NON-particle drop slot — the composite reads THIS instead of a literal that went stale when the spark pool grew (user 2026-08-20)   // lifeCfg.z = which secondary rays see creatures (see UNI_SEC_DEF). REPORTING ONLY since the fold - no shader reads it any more, so changing it mid-session does nothing; pick the config with ?uni&sec=N or window.__SEC before load.
    }
    // ── HOW FAR A BODY REACHES FROM ITS ANCHOR ── and the anchor is the CENTRE OF MASS, not the middle of
    // the box, so a half-diagonal is the wrong number: it is the radius about the box centre. comL says
    // where the COM sits inside the box, and the farthest corner from an interior point is the one built
    // from the larger span on each axis. See the note beside radB in render/wgsl/dda.js for what the old
    // number cost - a felled birch had 15 voxels of itself outside its own cull sphere and did not draw.
    const bodyRad = (g) => Math.hypot(Math.max(g.comL[0], g.bw - g.comL[0]),
                                      Math.max(g.comL[1], g.bh - g.comL[1]),
                                      Math.max(g.comL[2], g.bd - g.comL[2])) + 1;
    {                                                  // ── RIGID BODIES → u.physB ──
      // Same convention the creature models use: anchor + the three LOCAL axes, all expressed in CAMERA
      // space, so the shader's DDA runs in the body's own grid with no matrix inverse.
      // Bodies are published in WINDOW/WORLD space (not camera space) so ONE shader function can serve
      // the primary ray and every secondary ray. physBound is a sphere over all of them: shadow and AO
      // rays that cannot touch any body reject in a single compare.
      let nb = 0, bcx = 0, bcy = 0, bcz = 0, brad = 0;
      // ── PUBLISH THEM IN SPATIAL (MORTON) ORDER ── u.physG holds one bounding sphere per PHYS_GRP
      // CONSECUTIVE bodies, so what the array is sorted by decides whether those spheres are tight enough to
      // cull anything. Camera distance was the obvious key and is the wrong one: it makes each group a thin
      // depth slab that still spans the pile sideways, so looking down at a felled tree every sphere covers
      // the whole debris field and none of them ever reject. MEASURED that way, 249 chunks from above: 10.4 ms
      // sorted vs 9.1 ms unsorted — the cull was pure overhead. Morton order groups bodies that are near each
      // other in ALL THREE axes, which is what makes a group sphere small, and it does not depend on where the
      // camera is standing.
      const mor9 = (n9) => { n9 = n9 & 1023; n9 = (n9 | (n9 << 16)) & 0x30000ff; n9 = (n9 | (n9 << 8)) & 0x300f00f;
        n9 = (n9 | (n9 << 4)) & 0x30c30c3; n9 = (n9 | (n9 << 2)) & 0x9249249; return n9; };
      const ord9 = [];
      for (const b of PH.bodies) {
        if (!b.gpu) continue;
        const qx9 = Math.max(0, Math.min(1023, ((b.pos[0] - winOX) >> 2))), qy9 = Math.max(0, Math.min(1023, (b.pos[1] >> 2)));
        const qz9 = Math.max(0, Math.min(1023, ((b.pos[2] - winOZ) >> 2)));
        ord9.push([mor9(qx9) | (mor9(qy9) << 1) | (mor9(qz9) << 2), b]);
      }
      // ── AND WHICH BODIES GET THE SLOTS IS DECIDED BY DISTANCE, NOT BY THE MORTON KEY ── the publish below
      // stops at PHYS_MAX, so when there are more bodies than slots the sort order decides who is drawn. Morton
      // is exactly the right key for GROUPING (see above) and exactly the wrong one for CHOOSING: it is a
      // spatial hash, so the cut falls wherever the interleave happens to put it, and a chunk in front of the
      // player is dropped as readily as one behind the hill. Worse, a tumbling chunk's Morton code changes as
      // it moves, so the set on the wrong side of the cut churns frame to frame — which is a pile of debris
      // blinking, the same shape of bug as a creature losing its draw slot to a ranking that will not sit still.
      // So: pick the nearest PHYS_MAX first, then Morton-sort THOSE for publication, which keeps the group
      // spheres as tight as they were. When the cap is not binding — the ordinary case — this does nothing at
      // all beyond one length test, and the published set is identical.
      if (ord9.length > PHYS_MAX) {
        for (const e9 of ord9) { const b9 = e9[1];
          const dx9 = b9.pos[0] - P.x, dy9 = b9.pos[1] - P.y, dz9 = b9.pos[2] - P.z;
          e9[2] = dx9 * dx9 + dy9 * dy9 + dz9 * dz9; }
        ord9.sort((p9, q9) => p9[2] - q9[2]);
        ord9.length = PHYS_MAX;
      }
      ord9.sort((p9, q9) => p9[0] - q9[0]);
      for (const pr9 of ord9) {
        const b = pr9[1];
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
          const half = bodyRad(g);                       // …from the COM, not the box centre — see bodyRad
          const d = Math.hypot(ax - bcx, ay - bcy, az - bcz) + half;
          if (d > brad) brad = d;
        } }
      // ── AND A SPHERE PER GROUP OF PHYS_GRP ── bodyTrace walks the array in publish order, which is
      // nearest-first, so each group is a depth slab and the slabs themselves run front to back. A ray that
      // misses a slab skips all PHYS_GRP of its bodies on one compare, and once the ray has hit something every
      // slab beyond it rejects the same way. Radius <= 0 marks a group with nothing in it.
      for (let g9 = 0; g9 < PHYS_NG; g9++) {
        const lo9 = g9 * PHYS_GRP, hi9 = Math.min(lo9 + PHYS_GRP, nb), o9 = UF_PHYSG + g9 * 4;
        if (lo9 >= hi9) { UF[o9 + 3] = -1; continue; }
        let gx9 = 0, gy9 = 0, gz9 = 0;
        for (let k9 = lo9; k9 < hi9; k9++) { const q9 = UF_PHYSB + k9 * 20; gx9 += UF[q9]; gy9 += UF[q9 + 1]; gz9 += UF[q9 + 2]; }
        const m9 = hi9 - lo9; gx9 /= m9; gy9 /= m9; gz9 /= m9;
        let gr9 = 0;
        for (let k9 = lo9; k9 < hi9; k9++) { const q9 = UF_PHYSB + k9 * 20;
          const half9 = UF[q9 + 3] * (Math.hypot(Math.max(UF[q9 + 16], UF[q9 + 7] - UF[q9 + 16]),
                                                 Math.max(UF[q9 + 17], UF[q9 + 11] - UF[q9 + 17]),
                                                 Math.max(UF[q9 + 18], UF[q9 + 15] - UF[q9 + 18])) + 1);   // the same radius the shader builds radB from, so the group can never be tighter than its members
          const d9 = Math.hypot(UF[q9] - gx9, UF[q9 + 1] - gy9, UF[q9 + 2] - gz9) + half9;
          if (d9 > gr9) gr9 = d9; }
        UF[o9] = gx9; UF[o9 + 1] = gy9; UF[o9 + 2] = gz9; UF[o9 + 3] = gr9;
      }
      UF[UF_PHYSBOUND] = bcx; UF[UF_PHYSBOUND + 1] = bcy; UF[UF_PHYSBOUND + 2] = bcz; UF[UF_PHYSBOUND + 3] = brad;
      UF[UF_PHYSC] = nb; UF[UF_PHYSC + 1] = Math.max(0, 1 - (now - reactT0) / REACT_FADE); UF[UF_PHYSC + 2] = 0; UF[UF_PHYSC + 3] = 0;   // y = REACTIVE STRENGTH, eased — the mask keys off this, not off nb
    }
    // AO_REACH rides in physC.z. Written HERE, outside the body block above, because that block only runs when
    // there are rigid bodies — leaving the lane stale (and the AO reach wrong) in an empty scene.
    UF[UF_PHYSC + 2] = AO_REACH;
    device.queue.writeBuffer(uniBuf, 0, UF);
    if (CPROF) cpMark(6);

