    // ── PROCEDURAL POPULATION ── the active butterfly count scales with the visible AREA (density ≈ 1 per (200 vox)²),
    // so the world stays populated at every render distance. Each is recycled to a fresh procedural cell when it leaves
    // the fog ring; color comes from a spatial hash of that cell so orange/red/blue are sprinkled across the world.
    const rdV = Math.max(140, renderDist);
    // ── ONE SPAWN RULE FOR ALL LIFE ── (user) every creature now follows the cardinal/worm mechanic. Two things fix the
    // "it appeared out of nowhere beside me" pop-in: (a) LIFE_IN — nothing is EVER placed closer than this, so life fades
    // in through the far fog and you walk up on it, instead of being re-placed 45 vox away in plain view; (b) hysteresis —
    // the spawn ring (LIFE_OUT) sits strictly inside the despawn ring (LIFE_KEEP), so a creature dropped at the rim can't
    // immediately recycle and thrash. LIFE_KEEP stays CAPPED because stationary life (cardinals) and slow life (worms)
    // clump behind a fast player when the ring never recycles.
    // DOUBLED 2026-07-18 (user). The hard ceiling is the GENERATED RECT, not the cap: rect reaches renderDist+96 and a
    // spawn outside it is rejected outright, so LIFE_KEEP can only reach a little past the view. At the default view
    // (368) that lands the inner ring at ~337 instead of ~202 — the most the built world allows. Raising the view
    // slider is what actually unlocks the full doubling: at a 1000 view the inner ring goes 286 -> 811.
    const LIFE_CAP = 1040;                             // was 520 — doubled, so a long view distance is no longer clamped by the cap
    const LIFE_KEEP = Math.min(rdV + 64, LIFE_CAP);    // DESPAWN radius (every kind) — slightly PAST the view, still safely inside the generated rect
    const LIFE_OUT = LIFE_KEEP * 0.94;                 // outermost SPAWN radius — the gap to LIFE_KEEP is the hysteresis band
    const LIFE_IN = Math.min(LIFE_KEEP * 0.78, LIFE_OUT - 24);   // innermost SPAWN radius — out past the fog, never in clear view
    const MAM_KEEP = CARD_KEEP;                        // LAND MAMMALS reach EXACTLY as far as the perched songbirds (user): the birds' FIXED 680 — not max(LIFE_KEEP,…), which let a big view slider push mammals past the birds and dilute the 56-head pool over an oversized disc (measured: mammals at 1020 vox, in-view rings near-empty)
    const MAM_OUT = MAM_KEEP * 0.94;                   // …and their outer SPAWN ring matches, with the same 6% hysteresis gap. Rect-clipped like the birds, so the effective reach lands wherever the generated world ends.
    const nActD = Math.max(3, Math.min(16, Math.round(Math.PI * rdV * rdV / (200 * 200) / 2)));   // HALF the original density (user); flyers cap at 16 — slots 16-19 moms, 20-31 ducklings, 32-39 worms, 40-54 lilies
    const nAct = moonMode ? Math.max(2, nActD >> 1) : Math.min(16, Math.round(nActD * 2.5));   // BUTTERFLIES doubled (user 2026-07-18): 1.25 -> 2.5, still bounded by the 16 flyer slots   // FIREFLIES half as frequent (night); BUTTERFLIES +25% (user 2026-07-18), still capped by the 16 flyer slots (0-15)
    const nWorm = WORM_NFRAMES ? Math.max(11, Math.min(22, Math.round(nActD * 1.4))) : 0;   // ground worms, day AND night — CUT 30% (user 2026-07-18): 32→22 cap, 16→11 floor
    const nBunny = BUNNY_ITEM0 ? (Math.max(3, Math.min(7, Math.round(nActD * 0.405))) & ~1) : 0;   // ground BUNNIES (slots 276-299) — kind 2, hop through the forest. & ~1 → EVEN count (user)
    const nArmadillo = ARMADILLO_ITEM0 ? (Math.max(3, Math.min(7, Math.round(nActD * 0.405))) & ~1) : 0;   // ground ARMADILLOS (slots 300-323) — kind 2, WALK the forest floor at ~9 vox/s. MATCHES the bunny formula/count, & ~1 → EVEN (user)
    const nSkunk = SKUNK_WALK.length ? (Math.max(3, Math.min(7, Math.round(nActD * 0.405))) & ~1) : 0;   // ground SKUNKS (slots 324-347) — kind 2, same cardinal walk as the armadillo. SAME formula/count as the bunny + armadillo, & ~1 → EVEN (user)
    const nPorcupine = PORCUPINE_WALK.length ? (Math.max(3, Math.min(7, Math.round(nActD * 0.405))) & ~1) : 0;   // ground PORCUPINES (slots 348-371) — kind 2, restored to the pine forest 2026-07-22 after the user re-edited it in the asset editor. SAME formula/count as the other three land mammals, & ~1 → EVEN
    const nDfly = (DFLY_NFRAMES && waterSpots.length) ? Math.min(3, 1 + (waterSpots.length >> 2)) : 0;   // DRAGONFLIES scale with how much water is in view and take the TOP of the flyer band, so with no water nearby the flock is 100% butterflies exactly as before. RATE HALVED 2026-07-20 (user): cap 6→3, per-spot growth >>1→>>2 — half as many at every water amount
    const nDuck = DUCK_ITEM0 ? Math.min(4, lakeSpots.length + 1) : 0;   // MOTHER ducks (slots 16-19) — 1-2 families PER lake, at LEAST 1 in every lake (user): lakes+1 covers every detected lake and lets one get a 2nd family; capped by the 4 mom slots. lakeSpots is last frame's census.
    const nLily = 0;                                   // LIVE drifting lily pads DISABLED (user 2026-07-18) — only the STATIC stamped pads remain. Was `LILY_ITEM0 ? 12 : 0`.
    const nFish = (FISHES.length && waterSpots.length) ? Math.min(24, 6 + waterSpots.length * 6) : 0;   // FISH scale with how much water is in view (lakes AND rivers). CUT 25% (user 2026-08-09): ceiling 32→24, base 8→6, per-spot 8→6 — a uniform quarter off the WHOLE curve, so the cut is the same at every water amount rather than only biting where the ceiling clamped. The per-pool density cap below (~1 fish per 3 census samples) is deliberately UNTOUCHED: it is the anti-cramming ceiling, not the population, and lowering it too would thin small ponds twice. Slot band 244-275 now runs 244-267; species stays an even split (wk % FISHES.length). Was DOUBLED 2026-07-21: base 4→8, per-spot 4→8, ceiling 24→32
    // Count scales with the AREA the birds have to cover. The 60 cap was set when the spawn ring was ~331 vox; the
    // ring is now ~406, which is 1.5x the area, so the same 60 birds read as a thinner, patchier forest. This keeps
    // the ORIGINAL density over the larger disc (~90 at the default view) and is still inside the 180-slot pool.
    const nCard = CARD_NFRAMES ? 180 : 0;              // the FULL pool — 120 -> 180 so the ROBIN is ADDED to the songbird population rather than splitting the cardinal/blue-bird share three ways (user)

    const ffLights = [];                               // glowing fireflies + live clash/death SPARKS this frame → the u.fflies point-light slots (window coords + intensity)
    const cshadList = [];                              // ground/water creatures this frame → the u.cshad sun-shadow boxes (window coords + half-extents) so moving lilies/ducks/worms cast shadows
    if ((DUCK_ITEM0 || DFLY_NFRAMES || FISHES.length) && !ED.on && now > lakeScanT) {   // ── LAKE + WATER CENSUS (every 2.5 s) ── grid-sample the view for wide-open-water spots, cluster them into lakes; fish home on the same census;
      lakeScanT = now + 2500; if (CPROF) cpEvt |= 1;                          //    mother ducks below TARGET these so every visible lake gets its own family (pool cap 2 families)
      lakeSpots.length = 0; waterSpots.length = 0;
      const R7 = LIFE_OUT;                             // lake census reaches the full spawn ring — at 0.8·rdV no lake was ever detected out where ducks now spawn, so they could never appear there
      for (let gx7 = -R7; gx7 <= R7; gx7 += 20) for (let gz7 = -R7; gz7 <= R7; gz7 += 20) {
        if (gx7 * gx7 + gz7 * gz7 > R7 * R7) continue;
        const sx8 = P.x + gx7, sz8 = P.z + gz7;
        if (sx8 <= rect.xlo + 4 || sx8 >= rect.xhi - 4 || sz8 <= rect.zlo + 4 || sz8 >= rect.zhi - 4) continue;
        if (!bfWater(sx8, sz8) || !bfSky(sx8, WL + 2, sz8)) continue;   // REAL open-sky water of any width — this is the dragonfly's pool (rivers included)
        { const cw = waterSpots.find((c) => (c.x - sx8) * (c.x - sx8) + (c.z - sz8) * (c.z - sz8) < 90 * 90);   // loose 90-vox merge: a river stays several spots along its length rather than collapsing to one
          if (cw) cw.n++; else waterSpots.push({ x: sx8, z: sz8, n: 1 }); }
        if (!bfOpenW(sx8, sz8)) continue;              // …and from here on, LAKES only (width test) — dry ravines/cave pools never seed a lake spot
        const cl7 = lakeSpots.find((c) => (c.x - sx8) * (c.x - sx8) + (c.z - sz8) * (c.z - sz8) < 260 * 260);   // MERGE a whole lake into ONE spot (was 110 → a big lake split into 3-4 spots → 4 families on it, user); 260 covers a typical lake, still separates distinct ones
        if (cl7) { cl7.n++; cl7.x += (sx8 - cl7.x) / cl7.n; cl7.z += (sz8 - cl7.z) / cl7.n; }   // centroid = lake identity; ax/az stays a KNOWN-GOOD water sample (the centroid of an L-shaped lake can be ashore)
        else lakeSpots.push({ x: sx8, z: sz8, ax: sx8, az: sz8, n: 1 });
      }
    }
    let dropCursor = 25;                               // COMPACTION: live creatures emit to consecutive slots from 25; the count goes to the shader in pick2Y.w so its per-pixel loop covers only what exists. (5-24 = the 20 fixed death-burst slots: 4 sparks + 16 individual smoke voxels, user)
    // ── THE FLOCK NO LONGER TAKES THE WHOLE BUDGET (user 2026-08-05: ducklings, salmon and ducks all missing) ──
    // all eleven remaining songbirds used to be written unconditionally, ahead of every other creature, which
    // left 64 - 25 - 11 = 28 slots for the entire rest of the world's life to share. They are also the one kind
    // that is always overhead and always the same, so they were the cheapest thing to bound. Every bird still
    // FLIES — birdStep runs for all of them, so the flock and its hitboxes are unchanged — but only the nearest
    // BIRD_SLOTS are drawn, which hands six slots straight to the creatures the player was losing.
    if (!ED.on && FLYERS.length > 0) {
      const tbF = now / 1000;
      const bOrd = [];
      for (let bi = 1; bi < BIRD_N; bi++) {
        if (birdRagTick(birds[bi])) { birdBoxes[bi].active = false; continue; }   // it is the rigid body now — not stepped, not drawn
        const ps = birdStep(birds[bi], bi, tbF, dt);   // stepped for ALL of them, drawn for some
        birdPose(birds[bi], ps);                       // …and the pose it was drawn at, for the ragdoll
        ps.uid = bi;                                   // stable identity for the dynamic-life temporal reprojection
        birdHit(birdBoxes[bi], ps);                    // every cardinal is solid, same as the first
        bOrd.push({ ps, d: (ps.x - P.x) * (ps.x - P.x) + (ps.z - P.z) * (ps.z - P.z) });
      }
      bOrd.sort((a, b) => a.d - b.d);
      for (let i = 0; i < bOrd.length && i < BIRD_SLOTS && dropCursor < DROP_SLOTS; i++) {
        birdWrite(dropCursor, bOrd[i].ps, cam, right, up, fwd);
        dropCursor++;
      }
    } else for (let bi = 1; bi < BIRD_N; bi++) birdBoxes[bi].active = false;
    const fsX = tanH * aspect, fsY = tanH;             // frustum half-slopes + their plane normalisers — the sphere-vs-plane test each staged creature is ranked by
    const fnX = 1 / Math.sqrt(1 + fsX * fsX), fnY = 1 / Math.sqrt(1 + fsY * fsY);
    const dropBase = dropCursor;                       // first slot free after the flying songbirds — the wbf creatures (staged below) fill from here by distance
    let emitN = 0;                                     // wbf poses staged into emitBuf this frame; sorted + emitted after the loop
    let bunnyBoxN = 0; for (const bx9 of bunnyBoxes) bx9.active = false;   // rebuild the solid-bunny hitboxes each frame from the nearest bunnies
    let armBoxN = 0; for (const bx9 of armBoxes) bx9.active = false;   // …and the solid-armadillo hitboxes
    let skunkBoxN = 0; for (const bx9 of skunkBoxes) bx9.active = false;   // …and the solid-skunk hitboxes
    let porcBoxN = 0; for (const bx9 of porcBoxes) bx9.active = false;   // …and the solid-porcupine hitboxes
