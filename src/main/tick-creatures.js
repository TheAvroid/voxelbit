    for (let wk = 0; wk < DES_END; wk++) {
      const B = wbf[wk];
      if (B.slain) continue;                           // KILLED BY THE PLAYER (user): a slain slot never recycles or re-places — the creature is gone for the session (kill already cleared its stamp; init=false keeps it out of every emit/census)
      if (B.rag) continue;                             // RAGDOLLED: the rigid body IS the animal now. Steering, animating or emitting it again would draw a second copy standing where it used to be while the real one falls.
      const desSlot = wk >= MAM_END, desSp = desSlot ? ((wk - MAM_END) / DES_PER) | 0 : 0;
      const DES_FPS = { scorpion: 12 };                   // ── PER-SPECIES ANIMATION RATE ── unlisted species keep the 24 fps house rule
      const DES_SPD = { desert_mouse: 32, gecko: 32 };    // ── PER-SPECIES TRAVEL SPEED ── doubled from the shared 16 (user). Purely spatial: animClk accumulates raw dt and never reads speed, so the animation rate is unchanged
      // ── WHO BOLTS WHEN YOU GET CLOSE (user 2026-08-15) ── doubles its travel speed inside DES_DASH_R. Applied
      // to the SPEED, not to the animation rate, so the gecko keeps its 24 fps gait and simply covers ground
      // faster — the same split the mouse's speed doubling used.
      // ── WHO CHANGES PACE WHEN THE PLAYER IS CLOSE ── x2 inside DES_DASH_R, whichever way they are pointing.
      // The gecko and the mouse spend it RUNNING (the flee block steers them away); the cobra and the scorpion
      // spend it CHARGING, because DES_HUNT already steers them at the player and the flee block excludes any
      // hunter. So one table drives two opposite behaviours and neither species needs a special case.
      const DES_DASH = { gecko: 2, desert_mouse: 2, cobra: 2, scorpion: 2 }, DES_DASH_R = 70;
      const DES_HUNT = { cobra: 1, scorpion: 1 };         // ── WHO HUNTS THE PLAYER ── (user 2026-08-15)                   // ── PER-SPECIES ANIMATION RATE ── the scorpion reads slow at the house 24 (user 2026-08-15); everything unlisted stays 24
      const antLead = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'ant' && ((wk - MAM_END) % DES_PER) === 0;   // ── THE ANT COLUMN'S HEAD ── slot 0 of the ant band is the only ant that decides anything: it marches on the compass (its own branch in the steering chain below), and every other ant is PLACED on the path it recorded. Keyed on the NAME like desFly, so re-ordering the load list cannot promote some other animal to leader.
      const desFly = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'fly';   // ── THE FLY FLIES (user 2026-08-15) ── keyed on the NAME, not the index, so re-ordering the load list cannot silently turn some other animal into a flyer
      const DES_FLY_UP = 16;                             // …and rides this much higher than a butterfly's glide line   // ── DESERT CREATURES ── appended after the mammals, DES_PER slots per species, species index off the slot the same way the fish take B.fsp
      const porcSlot = wk >= 348 && wk < MAM_END, skunkSlot = wk >= 324 && wk < 348, armSlot = wk >= 300 && wk < 324, bunnySlot = wk >= 276 && wk < 300, fishSlot = wk >= 244 && wk < 276, wormSlot = wk >= 32 && wk < 64, duckSlot = wk >= 16 && wk < 32, lilySlot = wk >= 64 && wk < 244;
      const isBaby = wk >= 20 && wk < 32, sib = isBaby ? (wk - 20) % 3 : 0;
      const mom5 = isBaby ? wbf[16 + (((wk - 20) / 3) | 0)] : null;   // ducklings 20-22 belong to mom 16, 23-25→17, 26-28→18, 29-31→19 (3 each)
      // ── ORPHANED (user 2026-08-05: "killing the mom kills all the baby ducks — each duck has to be killed")
      // A brood used to be defined out of existence the instant its mother's slot went inactive, so one hit on
      // the mom silently deleted three ducklings that were never touched. Only a SLAIN mother makes orphans:
      // she is gone for the session, so the deactivation is permanent and the brood must stand on its own.
      // A merely RECYCLED mother (walked out of range, window recentred) still takes her babies with her —
      // that is population churn far from the player, not a kill, and the pair must recycle together.
      const orphan = isBaby && !!mom5.slain;
      const wantK = desFly ? 0 : desSlot ? 2 : (bunnySlot || armSlot || skunkSlot || porcSlot) ? 2 : (fishSlot ? 6 : (lilySlot ? 5 : (wormSlot ? 2 : (duckSlot ? 3 : (moonMode ? 1 : 0)))));   // BUNNIES + ARMADILLOS + SKUNKS + PORCUPINES are kind 2 (worm machinery) — the slot band, not the kind, tells them apart at emit/AI
      const isDfly = !desSlot && wantK === 0 && nDfly > 0 && waterSpots.length > 0 && wk >= nAct - nDfly;   // !desSlot: a desert slot number is far above nAct, so this would call the fly a dragonfly and then refuse to place it anywhere but water   // DRAGONFLIES take the TOP of the active flyer band — butterflies only give up slots when there is water in view   // day = butterflies, night = FIREFLIES; 16-19 MOM DUCKS, 20-31 ducklings, 32-63 WORMS (32), 64-243 PERCHED SONGBIRDS (180), 244-259 FISH
      const active = desSlot ? (desSp < DESERTS.length && ((wk - MAM_END) % DES_PER) < nDesertOf(desSp))
        : (porcSlot ? (wk - 348 < nPorcupine)
        : (skunkSlot ? (wk - 324 < nSkunk)
        : (armSlot ? (wk - 300 < nArmadillo)
        : (bunnySlot ? (wk - 276 < nBunny)
        : (fishSlot ? (wk - 244 < nFish)
        : (lilySlot ? (wk - 64 < nCard)
        : (wormSlot ? (wk - 32 < nWorm || (B.init && B.rel))
        : (isBaby ? (!!DUCKB_ITEM0 && (orphan ? B.init : (mom5.init && sib < (mom5.nBab || 0))))   // a duckling exists only while its mother does, up to her brood size — unless she was KILLED, in which case it carries on alone until it is killed too
        : (duckSlot ? (wk - 16 < nDuck) : (wk < nAct))))))))));   // B.rel = a worm the player RELEASED with Q — lives beyond the population cap until it recycles  [three extra ) close the porcSlot + skunkSlot + armSlot branches]
      if (ED.on || dead || !active || (porcSlot ? !PORCUPINE_WALK.length : (skunkSlot ? !SKUNK_WALK.length : (armSlot ? !ARMADILLO_ITEM0 : (bunnySlot ? !BUNNY_ITEM0 : (wantK === 6 ? !FISHES.length : (wantK === 5 ? !CARD_NFRAMES : (wantK === 2 ? !WORM_NFRAMES : (wantK === 3 ? !DUCK_ITEM0 : (wantK === 1 ? !FFLY_NFRAMES : !BFLY_COLS.length)))))))))) { if (B.sCells) unstampWorm(B); B.init = false; continue; }   // an inactive/hidden grid-stamped creature (worm/duck/skunk/porcupine) must clear its stamp
      const tb3 = now / 1000;
      if (B.init && (B.kind | 0) !== wantK && !B.dieT) B.dieT = now;   // dusk/dawn — the wrong creature shrinks away over 0.7 s, the right one fades in at a fresh spot
      // ── FLYER COMPOSITION DRIFT ── isDfly is read ONLY at spawn, so the butterfly/dragonfly split freezes into the band
      // the moment slots stop recycling. Constant mercy-recycle churn used to re-roll it by accident; now that the arbiter
      // keeps flyers alive indefinitely, a flock that filled before the lake census saw water stays 100% butterfly forever
      // (measured: 0 dragonfly-minutes over 240 s on all three seeds). Re-check against the LIVE split and retire the
      // mismatch through the SAME 0.7 s fade dusk/dawn uses — never a conversion in place, so a butterfly never pops into
      // a dragonfly mid-flight and the replacement re-places under the over-water rule. The 3 s dwell outlasts one 2.5 s
      // census tick, so a waterSpots count wobbling across a >>2 boundary cannot churn the band.
      if (wantK === 0 && B.init && !B.dieT) { if (!!B.dfly === isDfly) B.dfMis = 0; else if (!B.dfMis) B.dfMis = now; else if (now - B.dfMis > 3000) { B.dfMis = 0; B.dieT = now; } }
      if (B.dieT && now - B.dieT > 700) { B.dieT = 0; if (B.sN) unstampWorm(B); B.init = false; }   // …and the STAMP goes with it: deactivating a grid-stamped creature without unstamping leaves its voxels in W and its cells in stampedIdx, permanently exempt from every support test
      // Perched birds recycle at CARD_KEEP, just past where the 180-slot pool runs out at this density (~375 vox).
      // Because the frontier sits far beyond anything you can pick out, a bird activating there is invisible — which
      // is why this no longer needs the "only spawn behind the player" rule the random placement depended on.
      const rdK = wantK === 5 ? CARD_KEEP : ((bunnySlot || armSlot || skunkSlot || porcSlot) ? MAM_KEEP : LIFE_KEEP);   // land mammals persist to the bird keep radius (user: "spawn as far as the perched birds"); worms/ducks/fish keep LIFE_KEEP
      const dp2 = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z);
      const hd2 = ((B.kind | 0) === 0 || (B.kind | 0) === 2 || (B.kind | 0) === 6) && B.hcx !== undefined ? (B.hx - P.x) * (B.hx - P.x) + (B.hz - P.z) * (B.hz - P.z) : dp2;
      const far = hd2 > rdK * rdK;                     // butterflies (and fish) judge by their HOME, everything else by where it is
      if (!B.init || far || (B.trap > 12 && (B.kind < 3 || B.kind === 6 || !bfWater(B.x, B.z))) ||   // trap > 12 s = escape truly failed — mercy recycle; a duck/lily wedged ON real water NEVER teleports (the unstick frees it), but a FISH sealed in a rock pocket the escape can't solve does (respawns in open water)
          B.x <= rect.xlo + 4 || B.x >= rect.xhi - 4 || B.z <= rect.zlo + 4 || B.z >= rect.zhi - 4 ||   // the window recentred/shrank under it — this ground is stale garbage, leave it
          (isBaby && !orphan && (B.x - mom5.x) * (B.x - mom5.x) + (B.z - mom5.z) * (B.z - mom5.z) > 40 * 40) ||   // a duckling stranded from its (recycled) mother rejoins her — an ORPHAN has nowhere to rejoin, and must not be teleported to where its mother died
          ((B.kind === 3 || B.kind === 4 || B.kind === 6) && !isBaby && ((frame + wk) & 63) === 0 && !bfWater(B.x, B.z)) ||   // recycle ONLY a DUCK/LILY/FISH genuinely OFF real water (dry ravine/land after a recenter) — NOT a perched cardinal (kind 5, never on water) and NOT one merely near a shore
          (B.trap > 4 && (dp2 > 0.7 * 0.7 * rdK * rdK || (B.kind < 3 && bfObst(B.x, B.y, B.z))))) {   // stuck 4 s: recycle if far in the fog OR body-in-solid (kinds <3 only — a duck's body dips into the WATER voxel in wave troughs, which is not 'stuck')
        if (B.rel) B.rel = false;                      // a released worm that leaves/loses its spot rejoins the normal population rules
        let placed = false;
        if (wantK === 5) {                             // PERCHED CARDINAL: find a nearby pine crown to sit on (its own search — not the generic open-spot one)
          const pc = findPineCrown(wk);
          if (pc) { B.tx = pc.tx; B.tz = pc.tz; B.bi = pc.bi; B.x = pc.x; B.z = pc.z; B.perchFeet = pc.y + 1; placed = true; }   // feet rest ON TOP of the crown needle
        }
        if (isBaby && !orphan) {                       // ducklings hatch right behind their mother, in line order (an orphan has none — it takes the generic open-water spot below, like a lone adult)
          let hx6 = mom5.x - Math.sin(mom5.th) * (4 + sib * 3.5) + (Math.random() - 0.5) * 2;
          let hz6 = mom5.z - Math.cos(mom5.th) * (4 + sib * 3.5) + (Math.random() - 0.5) * 2;
          if (!bfWater(hx6, hz6) || !bfSky(hx6, WL + 2, hz6)) { hx6 = mom5.x; hz6 = mom5.z; }   // the trail spot is ashore/dry-ravine/inside the bank (mom hugging the shore) — hatch AT mom instead; she's on valid water by construction
          B.x = hx6; B.z = hz6;
          B.y = WL + 2.5; placed = true;
        }
        const tooClose = (j0, j1, minD) => {           // EVEN DISTRIBUTION — reject spawns bunched against a live same-kind neighbour
          for (let j = j0; j < j1; j++) { const O = wbf[j];
            if (O !== B && O.init && (O.x - sx7) * (O.x - sx7) + (O.z - sz7) * (O.z - sz7) < minD * minD) return true; }
          return false; };
        let sx7 = 0, sz7 = 0, hcx, hcz;
        for (let tries = 0; tries < 12 && !placed && wantK !== 5; tries++) {
          let sx, sz;
          if (wantK === 3 && lakeSpots.length) {       // MOTHER DUCKS TARGET LAKES: prefer a lake NO other mom (16-19) already holds — one family per visible lake
            const free = lakeSpots.filter((L) => {
              for (let m = 16; m < 20; m++) { const O = wbf[m]; if (O !== B && O.init && (O.x - L.x) * (O.x - L.x) + (O.z - L.z) * (O.z - L.z) < 150 * 150) return false; }
              return true; });
            const pool7 = free.length ? free : lakeSpots;
            const L7 = pool7[tries % pool7.length];
            sx = L7.ax + (Math.random() - 0.5) * 18; sz = L7.az + (Math.random() - 0.5) * 18;
          } else if (wantK === 0 && isDfly) {          // DRAGONFLY: same kind-0 creature, but its home is WATER (user: only over lakes and rivers).
            const L8 = waterSpots[(wk + tries) % waterSpots.length];   // walk the spots as tries advance so one crowded pool can't monopolise every slot
            sx = L8.x + (Math.random() - 0.5) * 30; sz = L8.z + (Math.random() - 0.5) * 30;
            hcx = Math.floor(sx / FLY_CELL); hcz = Math.floor(sz / FLY_CELL);   // give it a home cell so the butterfly LEASH applies and it stays over its water
          } else if (wantK === 6) {                    // FISH: home is a water spot (lakes AND rivers), body UNDER the surface. Capacity SCALES with the pool's SIZE so a small pond isn't crammed (user)
            if (!waterSpots.length) break;             // the census can empty waterSpots THIS frame, after nFish was computed from last frame's — a `% 0` here made `L9.x` throw (this was a permanent game-freeze before the tick wrapper)
            let L9 = null;
            for (let k = 0; k < waterSpots.length; k++) { const cand = waterSpots[(wk + tries + k) % waterSpots.length];   // walk the spots so a full pool yields to a hungrier one
              const cap = Math.max(1, Math.min(16, Math.round(cand.n / 3)));   // UNIFORM density: ~1 fish per 3 census samples (~1200 vox² of water). The old 1-per-sample slope CRAMMED ponds (a 9-sample pond held 9 fish) while big-lake spots were clamped at 16 regardless of area — density inverted (user)
              let near = 0; for (let f = 244; f < 276; f++) { const F = wbf[f]; if (F && F.init && (F.kind | 0) === 6 && F.hx !== undefined && (F.hx - cand.x) * (F.hx - cand.x) + (F.hz - cand.z) * (F.hz - cand.z) < 45 * 45) near++; }   // count by HOME, not wander position — spots are pairwise >90 apart so 45-vox home-discs are DISJOINT; the old 90-vox position count tallied one lake fish against ALL its spots and starved big lakes (user)
              if (near < cap) { L9 = cand; break; } }
            if (!L9) break;                            // every pool already holds its size-capped share → add no more fish
            sx = L9.x + (Math.random() - 0.5) * 24; sz = L9.z + (Math.random() - 0.5) * 24;
            hcx = Math.floor(sx / FLY_CELL); hcz = Math.floor(sz / FLY_CELL);   // a home cell → the leash keeps it in its pool, the recycle judges by home like a butterfly's
          } else if (!desSlot && (wantK === 0 || wantK === 2)) {   // desSlot deliberately falls through to the ANNULUS below: the home-finders (findWormHome/findSkunkHome/…) pick from procedural grids that do no biome test, so a desert creature routed through one is placed in the forest and then rejected by the gate on every retry — it would simply never spawn     // BUTTERFLY / WORM: take a procedural home rather than a random point on the ring
            const h = wantK === 0 ? findFlyHome(wk) : (porcSlot ? findPorcHome() : (skunkSlot ? findSkunkHome() : (armSlot ? findArmHome() : (bunnySlot ? findBunnyHome() : findWormHome(wk))))); if (!h) break;
          if (wantK === 2) {                           // a home inside a trunk is unreachable, and the leash below would grind the worm against it forever
            const tc = treeAt(Math.floor(h.x / TCELL), Math.floor(h.z / TCELL));
            if (tc && (tc.tx - h.x) * (tc.tx - h.x) + (tc.tz - h.z) * (tc.tz - h.z) < 14 * 14) continue;
          }
            sx = h.x; sz = h.z; hcx = h.cx; hcz = h.cz;
          } else {
            const a5 = Math.random() * 6.283;
            // ── THE DESERT BAND GETS A MUCH WIDER RING (user 2026-08-16: life "all seem to spawn near each
            // other, then vast areas where nothing spawns") ── measured before changing anything: with the
            // shared LIFE_IN (0.78 x LIFE_KEEP ~ 811) the ring is only ~31% of the disc it sits in and NOTHING
            // is ever placed inside 811, so 61% of bodies sat beyond 770 voxels and the ground within 110 of
            // the player held 1.3% of them. That reads exactly as a distant band with empty desert in front.
            // Halving the inner radius quadruples the usable area and lets the same 31 bodies cover it.
            // Pop-in is the reason the floor exists, and it is why this is scoped to the DESERT band alone:
            // these are 1-3 voxel animals, sub-pixel at 400+ voxels, where a bunny or a duck is not.
            // ── AND NOT THE LAND MAMMALS ── a `mamB` arm sat here giving bunnies/armadillos/skunks/porcupines
            // a 0.55 inner floor, on the same argument. It was INERT: every one of the four is kind 2 with
            // desSlot false, so the branch above routes all of them into findBunnyHome/findArmHome/
            // findSkunkHome/findPorcHome and nothing in the band can ever reach this draw. Measured before
            // removing it — over six boots the mammals' distance distribution (p25 323 / median 437 / p75 543
            // / max 638) is a dead match for an area-uniform disc of radius MAM_OUT = 639, which is what the
            // home finders produce and is nothing like this annulus. Their spread is decided by the scatter
            // key in those finders, so that is where the even-spread work belongs and where it now is.
            const inR = desSlot ? Math.min(LIFE_IN, LIFE_KEEP * 0.40) : LIFE_IN;
            const d5 = Math.sqrt(inR * inR + Math.random() * (LIFE_OUT * LIFE_OUT - inR * inR));   // worms, flyers and fallback ducks all share the one AREA-uniform annulus now (was three different inner floors: 40 / 50 / 50)
            sx = P.x + Math.sin(a5) * d5; sz = P.z + Math.cos(a5) * d5;
          }
          const antHeel = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'ant' && ((wk - MAM_END) % DES_PER) > 0;
          if (antHeel) {
            const ld = wbf[wk - 1];                    // hatch behind the ant ahead, in line order — the duckling spawn, minus the water tests
            if (!ld || !ld.init) continue;             // …and NEVER anywhere else. Falling through to the annulus put followers 130-600 voxels from
            sx = ld.x - Math.sin(ld.th) * 3.2 + (Math.random() - 0.5) * 1.2;   // their leader, and at 16 vox/s they never closed it before being recycled.
            sz = ld.z - Math.cos(ld.th) * 3.2 + (Math.random() - 0.5) * 1.2;   // Waiting a frame for the leader is free — the retry loop comes straight back.
          }
          if ((desertM(sx, sz) > 0.85) !== desSlot) continue;   // ── BIOME GATE, BOTH WAYS ── the desert band wants open desert and nothing else does. 0.85 (not 0.5) is an ADMIT test for the desert creatures, so none of them stands in the dithered treeline; for every other band it is the original reject, one notch stricter.  // was: NO LIFE IN THE DESERT (user) ── placed on the ONE annulus every land, flying and worm spawn funnels through, after both the home-finder and the fallback branch have chosen a point, so no species can route around it
          if (sx <= rect.xlo + 4 || sx >= rect.xhi - 4 || sz <= rect.zlo + 4 || sz >= rect.zhi - 4) continue;   // NEVER spawn outside the GENERATED rect — hmap there is garbage (the snow-landing guard; embedded 'cave' creatures after recenters)
          sx7 = sx; sz7 = sz;
          const gS = bfSurf(sx, sz);
          if (wantK === 2 && gS <= WL + 0.5) continue; // worms spawn on LAND only
          // ── DESERT LIFE SPREADS OUT (user 2026-08-15: "there shouldnt be geckos right next to each other") ──
          // The desert band was the ONE band with no spacing floor: every other kind has had a tooClose gate for
          // ages (mammals 70, ducks 200, fireflies 40, fish 14) and desSlot was simply never wired into any of
          // them, so its 42 creatures were placed by uniform random darts. Uniform random is not even — it
          // CLUMPS (Poisson), which is exactly the pair of touching geckos. There is no shortage of room: the
          // annulus is ~934k vox², so the mean gap is already ~105 vox overall and ~280 within a species. The
          // floors below are about a HALF of those means, which is the usual Poisson-disc working point.
          // TWO floors, because one cannot do both jobs: the same-species one is what the user actually sees
          // (six geckos reading as a litter), the cross-species one just stops a pile-up of mixed bugs.
          if (desSlot && !antHeel) {
            // RELAXED BY TRY, so a floor can never STARVE a slot. Near the biome edge only a slice of the
            // annulus passes the desertM gate, and a hard floor there would leave slots permanently uninit —
            // i.e. fewer creatures, a worse bug than the one being fixed. Try 0 demands the full gap and the
            // last try demands nothing, so a cramped spot still fills, just less evenly.
            // CUBIC, and it now bottoms out at HALF rather than at zero. Cubic because a linear decay had
            // already given away half the gap by try 5. The floor under it is the newer part: letting spread
            // reach 0 meant the last try placed with NO spacing at all, which is how a pair ends up touching.
            // With the desert thinned to ~31 bodies in a ~934k vox² annulus even the full 160 is easy to
            // satisfy, so half of it is never a starvation risk — measured, the population still fills.
            const spread = 1 - 0.5 * Math.pow(tries / 11, 3);
            const sp0 = MAM_END + desSp * DES_PER;
            if (tooClose(sp0, sp0 + DES_PER, DES_APART * spread)) continue;
            if (tooClose(MAM_END, DES_END, DES_APART_ANY * spread)) continue;
          }
          if (desSlot && wantK === 2 && !navSand(sx, sz)) continue;   // ── NO DESERT WALKER STARTS ON A ROCK OR A CACTUS (user 2026-08-16) ── the same rule its every step is judged by, asked once at placement. Without it a body placed on stone spends its whole life in the egress path, which is the one branch that moves without asking. The forest's own version of this test (isRockSurf over the MAMFIT footprint, below) is deliberately left where it is: it answers a different question, about worldgen strata, for a band this change does not touch.
          // CROSS-SPECIES floor: never spawn a land mammal within MAM_FLOOR of ANY live mammal (bunny/
          // armadillo/skunk/porcupine) — breaks up the multi-species KNOTS (user: skunk 24 vox from a bunny
          // etc.). 70 was count-starving under NEAREST-FIRST (even 24 left skunk 2 short — rejects had nowhere
          // else to go), but the HASH-ORDER scatter retries land anywhere in the disc, so counts hold.
          // RELAXED BY TRY (user 2026-08-16: "make sure that the land mammals are evenly spread out across the
          // pine forest"). The flat 70 was not spacing the band, it was DEFINING it: measured over six boots
          // the pooled nearest-neighbour distribution sat at min 70.2 / p10 79.9 / median 104.7, i.e. the tenth
          // percentile was standing on the gate. Asking for MAM_APART on the first try and decaying to the old
          // 70 by try MAM_RELAX gives the placement room to find a real gap while leaving the worst case
          // exactly where it has always been — so this cannot place anything closer than the flat rule did.
          // It reaches the floor at MAM_RELAX rather than at the last try for a measured reason: decaying
          // across the whole budget spent every try on a gate stricter than the old one, and a slot that used
          // to fill could exhaust all twelve (measured 23 of 24 alive in 2 of 12 samples — transient, the slot
          // retries next frame, but real). The last few tries now run at exactly the historical gate, so the
          // band cannot do worse than it always has.
          if (bunnySlot || armSlot || skunkSlot || porcSlot) {
            const mamGap = MAM_APART - (MAM_APART - MAM_FLOOR) * Math.pow(Math.min(1, tries / MAM_RELAX), 3);
            if (tooClose(276, 372, mamGap)) continue;
          }
          // ── NO LAND MAMMAL STARTS ON STONE (user 2026-08-07) ── this used to sample ONE column, at the spawn
          // point, and reject only when it found GREY three voxels deep. Two ways through it: a boulder or a
          // shallow outcrop is not three deep, and an animal is not a column — a skunk is 11 voxels long, so its
          // centre could sit on grass with half its body over rock. Now it is the model's own footprint (MAMFIT,
          // the same extents the ground servo seats on) against a real id table rather than a colour guess, which
          // also picks up BOULDERS: those live on dedicated palette ids and the grey test only caught them by
          // accident of shade.
          if (bunnySlot || armSlot || skunkSlot || porcSlot) {
            const fitS = bunnySlot ? MAMFIT.bunny : (armSlot ? MAMFIT.arm : (skunkSlot ? MAMFIT.skunk : MAMFIT.porc));
            const rS = fitS ? Math.max(fitS.hw, fitS.hd) : 3;   // heading is not chosen yet, so take the longer half-extent both ways — conservative by construction
            let onRock = false;
            for (let ux = -1; ux <= 1 && !onRock; ux++) for (let uz = -1; uz <= 1; uz++) {
              const qx = sx + ux * rS, qz = sz + uz * rS, qg = bfSurf(qx, qz);
              if (qg <= WL + 0.5) continue;                // underwater column — the land tests elsewhere own that
              // ── SCAN THE COLUMN, DO NOT TRUST hmap ── bfSurf is the HEIGHTMAP, and a stamped boulder sits ON
              // TOP of it: hmap never saw the stamp, so `gS - 1` reads the dirt underneath and every boulder in
              // the world was invisible to this test. Measured: 18,000 land columns, not one rock found through
              // hmap. Walk down from a little above the heightmap instead and take the first solid voxel — the
              // same thing the player sees underfoot.
              const bx9 = gwrap(Math.floor(qx), WX), bz9 = gwrap(Math.floor(qz), WZ) * WX * WY;
              for (let y9 = Math.min(WY - 1, Math.floor(qg) + 8); y9 >= Math.max(1, Math.floor(qg) - 2); y9--) {
                const v9 = W[bx9 + y9 * WX + bz9]; if (!v9 || solidTab[v9] !== 1) continue;
                if (isRockSurf(v9)) onRock = true;
                break;                                     // first solid from above IS the surface — whatever it is, stop
              }
              if (onRock) break;
            }
            if (onRock) { PH.stats.mamRockRej = (PH.stats.mamRockRej | 0) + 1; continue; }
            PH.stats.mamRockOK = (PH.stats.mamRockOK | 0) + 1;
          }
          if (wantK === 0 && isDfly && !bfWater(sx, sz)) continue;    // the jitter may have thrown it ashore — a dragonfly only ever starts over real water
          if (wantK === 0 && !isDfly && bfWater(sx, sz)) continue;    // …and a BUTTERFLY never starts over water (user: limit butterflies over lakes); it may still drift out over one
          if ((wantK === 3 || wantK === 4) && (!bfSky(sx, WL + 2, sz) || !bfOpenW(sx, sz))) continue;   // ducks + lilies spawn on OPEN-SKY, WIDE REAL-WATER only (bfOpenW now tests actual water voxels) — cave pools AND dry gorge/ravine floors are out
          if (wantK === 6 && (!bfWater(sx, sz) || WL - bfBed(sx, sz) < 4 || tooClose(244, 276, 14))) continue;   // FISH need REAL water DEEP enough for a body below the surface (≥4 vox) — rivers qualify, shore shelves don't; ≥14 apart so a pool holds a loose school, not a knot
          if (wantK === 1 && tooClose(0, 16, 40)) continue;   // FIREFLIES still spread by proximity. BUTTERFLIES must NOT: their homes are already 128 vox apart by the cell grid, and testing against other butterflies' WANDERING positions made placement depend on where they happened to be — which is exactly what stopped the set being reproducible.             // FLYERS spread ≥40 vox apart at spawn → even distribution, never clustered (user)
          // (worms no longer use tooClose: their homes are already 160 vox apart by the cell grid, and testing against other worms' CRAWLING positions made placement depend on where they had got to)          // worms spread WIDE — never bunched (user, doubled with the population)
          if (wantK === 3 && tooClose(16, 20, 200)) continue;         // mother ducks spawn ≥200 vox apart → at most ~2 families fit on a typical lake (user: 'only 1-2 per lake')
          if (wantK === 4 && tooClose(40, 55, 12)) continue;          // lilies scatter
          const sy = wantK === 6 ? (bfBed(sx, sz) + WL) * 0.5 : (wantK === 2 ? gS + ((desSlot && DESERTS[desSp] && MAMFIT[DESERTS[desSp].name]) ? MAMFIT[DESERTS[desSp].name].seat : 2) : (wantK === 3 ? WL + 4 : (wantK === 4 ? WL + 1.4 : (desFly ? DES_FLY_UP : 0) + bfGlide(sx, sz) + (wantK ? 9 : 14))));   // fish hang at MID-DEPTH; ducks/lilies float; duck FEET sit inside the water voxel layer
          if (wantK === 6 || ((wantK === 2 || !bfRoofed(sx, sy, sz)) && !bfObst(sx, sy, sz))) {   // a fish spot was already fully validated (real deep water) — bfObst would see the WATER voxels as solid and reject every one; worms live happily UNDER the canopy — the roof test starved dense-forest placement ('worms only near spawn')
            B.x = sx; B.z = sz; B.y = sy; placed = true;
            const cx = Math.floor(sx / 64), cz = Math.floor(sz / 64);   // color = spatial hash of the 64-vox cell → world-anchored variety, all 3 colors present
            B.col = ((Math.imul(cx, 374761393) ^ Math.imul(cz, 668265263)) >>> 0) % Math.max(1, BFLY_COLS.length);
          }
        }
        if (!placed) { if (B.sN) unstampWorm(B); B.init = false; continue; }     // no open spot this frame — stay hidden, retry next
        if (wantK === 0 || wantK === 2 || wantK === 6) { B.hx = sx7; B.hz = sz7; B.hcx = hcx; B.hcz = hcz; } else { B.hcx = undefined; }
        B.dfly = (wantK === 0 && isDfly); B.dfMis = 0;              // the strip + frame count key off this; everything else about the creature is butterfly
        if (wantK === 6) { B.spd = FISH_CFG.baseSpeed * 0.8; B.spdT = FISH_CFG.baseSpeed; B.dT = 0.3 + Math.random() * 0.5; B.dRe = 0; B.animClk = Math.random() * 40; B.cx = B.x; B.cy = B.y; B.cz = B.z; B.vyS = 0;   // FISH: ease up into the configured cruise + desynced tail-beat; cx/cy/cz = last body-clear pose (terrain hitbox revert)
          B.fsp = wk % Math.max(1, FISHES.length);     // species fixed by SLOT (like the songbirds) — an exact even split that never drifts as fish recycle
          B.fhalf = (FISHES[B.fsp] || {}).half || 5;              // body half-length for every navigation probe — per species, not a salmon-sized guess
          { const sn = (FISHES[B.fsp] || {}).name;                // per-species LEAP frequency multiplier from the config (unlisted species get a modest default)
            B.jumpMul = FISH_CFG.jump.species[sn] !== undefined ? FISH_CFG.jump.species[sn] : 0.25;
            B.schools = FISH_CFG.schoolSpecies.indexOf(sn) >= 0; }   // …and whether this species shoals at all
          B.jumpV = undefined; B.jumpArm = undefined; B.fleeT = 0; B.spookT = 0;   // a recycled slot must never inherit a live arc, a stale flee window or the last occupant's wound-panic
          B.jumpRe = tb3 + (3 + Math.random() * 14) / Math.max(0.05, B.jumpMul);
          // ── SCHOOL ── join a same-species school that still has room (spawning right beside it), else start a fresh school of 3-6, else swim alone (~28%)
          B.school = -1; B.schoolCap = 0;
          for (let m = 244; m < 276; m++) { const O = wbf[m]; if (O === B || !O.init || (O.kind | 0) !== 6 || O.school < 0 || O.fsp !== B.fsp) continue;
            if ((O.x - B.hx) * (O.x - B.hx) + (O.z - B.hz) * (O.z - B.hz) > 60 * 60) continue;   // LOCAL schools only — joining used to TELEPORT the spawn beside a school in a DIFFERENT pool, bypassing that pool's cap (this piled extra fish into small ponds, and a pond-locked fish could never swim back to its real home)
            let cnt = 0; for (let q = 244; q < 276; q++) { const Q = wbf[q]; if (Q.init && Q.school === O.school) cnt++; }
            if (cnt < O.schoolCap && bfWater(O.x, O.z) && WL - bfBed(O.x, O.z) >= 3) { B.school = O.school; B.schoolCap = O.schoolCap; B.x = O.x + (Math.random() - 0.5) * 9; B.z = O.z + (Math.random() - 0.5) * 9; break; } }
          if (B.school < 0 && B.schools && Math.random() > 0.18) { B.school = (fishSchoolSeq = (fishSchoolSeq + 1) & 0x3fffffff); B.schoolCap = 4 + (Math.random() * 5 | 0); } }   // 4-8 per school; only the SCHOOLING species form them (user: salmon + minnow for now), and they do so more often now that there are twice as many fish
        B.th = isBaby ? mom5.th : Math.random() * 6.283;
        B.om = 0; B.omT = 0; B.turnAcc = 0; B.tRe = 0; B.trap = 0; B.born = now; B.init = true; B.hurt = 0; B.hits = 0; B.dying = false; B.blinked = false; B.hopT0 = undefined; B.lastSwing = undefined; B.spookT = 0; B.trail = undefined;   // fresh occupant — never inherits the last one's knife wound, pending death, spent flash, panic, wound-up yaw or (ant leader) RECORDED PATH: a stale trail is a line of followers snapped back to where the previous occupant walked (turnAcc: a recycled duck inheriting most of a circle reads its very first bank as an over-wound spin and unwinds the long way round for nothing)
        // bh/ah undefined → a (re)spawned bunny/armadillo reinits its cardinal state machine from the fresh heading.
        // THIS LINE WAS DEAD: it used to sit after a `//` on the line above, so the trailing comment ate it. The
        // bunny is the one creature whose position is ASSIGNED rather than integrated (B.x = B.bpx + bake offset),
        // so a stale bh meant a stale bpx/bpz base cell: the frame after it was re-placed near the player it
        // snapped straight back to its old spot hundreds of voxels away, went out of range again, was re-placed
        // again… every single frame. That is the bunny "glitching" and being impossible to hit. The armadillo and
        // the other marchers survived it because they integrate (B.x += …), so a stale heading is harmless.
        B.bh = undefined; B.ah = undefined;
        B.kind = wantK; B.dieT = 0; B.glow = false; B.glowT = 0; B.gRef = bfSurf(B.x, B.z);
        if (duckSlot && !isBaby) B.nBab = 3;           // every mother leads exactly 3 ducklings (user)
        if (lilySlot) {                                // PERCHED BIRD init (lily slots repurposed): a random animation phase so they're DESYNCED, + red cardinals / BLUE BIRDS (the reskin) — both grid-stamped by stampCardinal
          B.phase = Math.random() * 100000;
          // INTERMIX THE SONGBIRDS (user: "you're clustering the colored ones with each other"). An independent
          // 50/50 flip per bird is what produced the clumps — random runs of one colour are normal in a coin flip
          // and read as deliberate grouping. Instead take the colour that is UNDER-represented among the perched
          // birds ALREADY nearby, so every neighbourhood self-balances; ties still flip a coin so it never
          // degenerates into a rigid checkerboard. Population stays ~50/50 globally.
          // COLOUR COMES FROM THE PINE, not from a coin flip at spawn: a DIAGONAL 3-colouring over 40-vox blocks
          // (cardinal / blue bird / robin), with ~30% of trees kicked to one of the other two so it never reads as a
          // literal grid. Deterministic per tree, so the same pine is always the same colour and the mix cannot drift
          // or clump as birds recycle. The 3-colouring guarantees ADJACENT blocks never share a colour, which is what
          // keeps the songbirds intermixed now that there are three of them.
          B.bird = birdColour(B.tx, B.tz, B.bi);
        }
      }
      const fadeIn = Math.min(1, (now - B.born) / 700);   // grow in over 0.7 s so a recycled creature materialises smoothly instead of popping into view
      const fadeOut = B.dieT ? Math.max(0, 1 - (now - B.dieT) / 700) : 1;   // …and shrinks away at the day/night handover
      const bScale = 0.12 + 0.88 * fadeIn * fadeIn * (3 - 2 * fadeIn) * fadeOut;
      if (B.kind === 5) { stampCardinal(B, now, wk); continue; }   // ── PERCHED CARDINAL ── GRID-STAMPED (aligned to grid, identical grid lighting + rotation + offsets to the asset-editor cardinal); stationary, so no shimmer beyond the editor's
      let Hx2 = Math.sin(B.th), Hz2 = Math.cos(B.th);
      const duckArb = NAVARB && (B.kind | 0) === 3;
      const mamArb = NAVARB && (B.kind | 0) === 2 && (armSlot || skunkSlot || porcSlot);   // BUNNY DELIBERATELY EXCLUDED (measured 2026-08-06): on seed 909091 it lost 12-16% of its distance and speed on the arbiter, and an order-swapped rerun reproduced it, so it follows the BUILD and not run position. It is the only one of the four that HOPS 5-6 voxels instead of walking, so a single refused station costs it a whole hop where a walker just takes a shorter step. The other three gained (armadillo path +6%, hard-stops -33 to -40%; skunk and porcupine hard-stops -67 to -96%) and keep the arbiter. Re-including the bunny needs the hop to be planned as one committed arc rather than per-station.   // ── LAND MAMMALS, ON THE ARBITER ── they share B.kind 2 with the worm and drive their own march, so the worm's wiring is not theirs; this flag is the whole of the band's opt-in and ?noarb turns it off without touching one character of the marcher's own arithmetic.
      const walkFree = (tx, tz) => mamArb ? navWalkFree(tx, tz, NAV_MCLR)   // the RELAXED gate the boxed-in and watchdog escapes are chosen with AND advanced under: land + body-clear, step limit ignored. It was hand-rolled identically in three places (two marcher escapes and the bunny's), and is now ONE function at a scope BOTH mammal controllers can see, because a marcher picking an exit under one rule and advancing under another is the original 'stuck on terrain' bug.
        : (bfSurf(tx, tz) > WL + 0.5 && !bfObstW(tx, bfSurf(tx, tz) + 2, tz) && !bfObstW(tx, bfSurf(tx, tz) + 4, tz));
      const duckFit = (px, pz) => bfWater(px, pz) && bfSky(px, WL + 2, pz);   // ── THE DUCK'S ONE ANSWER ── real water under open sky, which is what every one of its consumers was already reaching for; they just each asked a different subset. It is now literally ONE function behind the planner's lookahead, the reach fan, the blocked-escape probe, stepOK and the head buffer. It is deliberately NOT the field's swim band: gating on nvD >= 3 was built and measured, and it refused 3.6%% of a mother duck's steps on her own lake (322 stop events per creature-minute against 0.8, and 6%% off her distance) for a band that had zero stuck-seconds to begin with. The arbiter's job in this band is to remove the disagreement, not to add a rule.
      if (B.kind === 4) {                              // LILY PAD: slow drift on the water + constant free rotation; movement heading (mth) is independent of the visual spin
        if (tb3 > B.tRe) { B.mth += (Math.random() - 0.5) * 1.2; B.tRe = tb3 + 3 + Math.random() * 4; }
        if (!bfWater(B.x + Math.sin(B.mth) * 5, B.z + Math.cos(B.mth) * 5)) B.mth += 2.6 * dt;   // shore/dry ahead — curl the drift away
      } else if (B.kind === 2 && (armSlot || skunkSlot || porcSlot)) {   // ARMADILLO + SKUNK + PORCUPINE (user): a continuous cardinal WALK on the forest floor — marches ~9 vox/s, turns only 90° (never diagonal), follows the terrain, breaks out of dead-ends (never spins). Editor behaviour, world terrain. (Same AI; the slot band picks the stamp model — porcupine is armadillo-like, skunk doubles on flee.)
        const DIRa = [[0, -1], [1, 0], [0, 1], [-1, 0]];
        const cardTh = (h) => Math.atan2(DIRa[h & 3][0], DIRa[h & 3][1]);
        const gcM = mamArb ? navWalkStand(B.x, B.z) : 0;   // the marcher's OWN travel surface, from the field — ONE number shared by its lookahead, its brake, its step test and its y servo, so all four agree on where the ground is
        const walkOK = (h) => { if (mamArb) return navReachWalk(B.x, B.z, cardTh(h), NAV_MSTA, gcM, NAV_MUP, NAV_MDN, NAV_MCLR) >= NAV_MSTA;   // ── THE WHOLE FIVE VOXELS, NOT THE POINT AT FIVE ── while the marcher walks a straight line the point probe sweeps the corridor for it, but the instant it TURNS, the new heading's first voxels have never been sampled at all: the point at 5 can be clear over a rock at 1, so the turn is approved and the marcher then walks into the rock and grinds. The DDA is the same one the brake and the step test use, so the three cannot disagree.
          const tx = B.x + DIRa[h & 3][0] * 5, tz = B.z + DIRa[h & 3][1] * 5, gA = bfSurf(tx, tz), cur = bfSurf(B.x, B.z);   // 5 vox ahead must be climbable LAND, clear over the body
          return gA > WL + 0.5 && (gA - cur) <= 3 && (cur - gA) <= 4 && !bfObstW(tx, gA + 2, tz) && !bfObstW(tx, gA + 4, tz); };   // STEP UP ≤3 blocks (raised from 2 — the ≤2-over-5-vox gate stalled every marcher on ordinary pine-forest slopes, user; matches the bunny's ±3 hopOK) + drop ≤4 so it doesn't get boxed at a ledge
        if (B.ah === undefined) { B.ah = ((Math.round(B.th / (Math.PI / 2)) % 4) + 4) % 4; B.aWhim = 0; B.aesc = 0; B.aTurnT = 0; }
        const adx = B.x - P.x, adz = B.z - P.z, ady = P.y - B.y;
        B.aflee = spooked(B) || (adx * adx + ady * ady + adz * adz) < (B.aflee ? 46 * 46 : 30 * 30);   // SPHERE flee — flying overhead won't spook it, but a hit does, at any range
        if (!walkOK(B.ah)) {                            // ── BLOCKED AHEAD ── stop, turn 90° in a COMMITTED direction (can't oscillate), spaced by a cooldown so it never spins
          if (tb3 > B.aTurnT) {
            if (walkOK(B.ah + 1) || walkOK(B.ah + 2) || walkOK(B.ah + 3)) {
              if (!B.aesc) B.aesc = walkOK(B.ah + 1) ? 1 : (walkOK(B.ah + 3) ? 3 : 1);
              B.ah = (B.ah + B.aesc) & 3; B.aTurnT = tb3 + 0.28;
            } else {                                     // BOXED IN — break out over a RELAXED step (down a ledge / over a gap), or respawn if truly islanded
              B.aesc = 0; let best = -1, bgv = -1e9;
              for (let h = 0; h < 4; h++) { const tx = B.x + DIRa[h][0] * 5, tz = B.z + DIRa[h][1] * 5, gA = mamArb ? navWalkStand(tx, tz) : bfSurf(tx, tz);
                if (walkFree(tx, tz) && gA > bgv) { bgv = gA; best = h; } }
              if (best >= 0) { B.ah = best; B.aTurnT = tb3 + 0.28; B.aRelax = 1.2; } else { if (B.sN) unstampWorm(B); B.init = false; continue; }   // arm a RELAXED-ADVANCE window: the picked heading fails walkOK by construction, so without this the creature faced its escape route but the walkOK-gated march never moved — the actual "stuck on terrain" bug (user). The wedge EXIT must unstamp, or the creature's voxels stay in W and in stampedIdx for good.
            }
          }
        } else {                                         // ── CLEAR AHEAD ── walk; occasionally re-head (flee away from the player / random whim)
          B.aesc = 0;
          if (B.aflee) { let best = B.ah, bd = -1e9; for (let h = 0; h < 4; h++) if (walkOK(h)) { const d = DIRa[h][0] * adx + DIRa[h][1] * adz; if (d > bd) { bd = d; best = h; } } if (best !== B.ah && tb3 > B.aTurnT) { B.ah = best; B.aTurnT = tb3 + 0.28; } }
          else if (B.hx !== undefined && (B.x - B.hx) * (B.x - B.hx) + (B.z - B.hz) * (B.z - B.hz) > 60 * 60) {   // LEASH: marched too far from its RESERVED home cell → head back (same 60-vox leash as the bunny), so armadillos/skunks stay evenly spread and never drift together into a bunch (user)
            if (tb3 > B.aTurnT) { const hx = B.hx - B.x, hz = B.hz - B.z; let best = B.ah, bd = -1e9;
              for (let h = 0; h < 4; h++) if (walkOK(h)) { const d = DIRa[h][0] * hx + DIRa[h][1] * hz; if (d > bd) { bd = d; best = h; } }
              if (best !== B.ah) { B.ah = best; B.aTurnT = tb3 + 0.28; } } }
          else if (tb3 > B.aWhim) { B.aWhim = tb3 + 1 + Math.random() * 0.6; if (Math.random() < 0.25) { const t = Math.random() < 0.5 ? 1 : 3; if (walkOK(B.ah + t)) { B.ah = (B.ah + t) & 3; B.aTurnT = tb3 + 0.28; } } }   // each ~1-1.6 s step: 75% keep walking forward / 25% rotate 90° (50/50 left/right) — user
        }
        const spdTgt = skunkSlot ? (B.aflee ? 48 : 24) : (porcSlot ? (B.aflee ? 18 : 9) : 9);   // SKUNK: 24→48 on flee. PORCUPINE: 9→18 — DOUBLE the pace when the player is near (user). ARMADILLO (else): constant 9. All eased via B.aspd below.
        B.aspd = (B.aspd === undefined) ? spdTgt : B.aspd + (spdTgt - B.aspd) * Math.min(1, dt * 6);   // ease the pace toward the target (bunny-style ramp) so the speed-up/-down isn't an instant snap
        B.th = cardTh(B.ah);                             // HOISTED from below the advance — nothing between the two positions ever read B.th, so the non-arbiter path is unchanged; the brake needs the heading the marcher is ACTUALLY walking this frame, not last frame's
        if (walkOK(B.ah)) {
          if (mamArb) { navMoveK[7]++;                   // row 7 is the LAND MAMMAL row: this band shares B.kind 2 with the worm, and one row for both would let a landing in either hide inside the other's total
            let mvM = B.aspd * dt;
            if (NAVBRK) mvM = navBrake2(B, (th7, L7) => navReachWalk(B.x, B.z, th7, L7, gcM, NAV_MUP, NAV_MDN, NAV_MCLR), mvM, dt, NAV_MLOOK, NAV_MBCLR, NAV_MBRK2, 7);   // ── THE BRAKE ── the lookahead above already refuses a lane under 5 voxels, so the cap only ever ramps between 5 and 8 and the 9 vox/s armadillo never feels it at all; what it removes is the fleeing skunk arriving at a turn at 48 vox/s and hard-stopping for a frame
            const nxM = B.x + DIRa[B.ah][0] * mvM, nzM = B.z + DIRa[B.ah][1] * mvM;
            if (navWalkOK(nxM, nzM, gcM, NAV_MUP, NAV_MDN, NAV_MCLR)) { B.x = nxM; B.z = nzM; }   // THE SAME answer the lookahead and the brake scored with, asked of the actual step
            else { navRejK[7]++;
              if (!navWalkOK(B.x, B.z, gcM, NAV_MUP, NAV_MDN, NAV_MCLR)) { B.x = nxM; B.z = nzM; navEgrK[7]++; } }   // ── EGRESS ── the marcher is already OUTSIDE the travelable set (a tree fell on it, it streamed in beside rock). Refusing the step would pin it there until the watchdog respawns it; it walks out along its heading instead. Displacement, never a teleport.
            B.aRelax = 0; }
          else { B.x += DIRa[B.ah][0] * B.aspd * dt; B.z += DIRa[B.ah][1] * B.aspd * dt; B.aRelax = 0; }   // advance ONLY when clear → never walks into a rock/water (clear again → drop any relaxed window). CHARACTER FOR CHARACTER the original expression: `DIRa[..] * mvM` is not `DIRa[..] * B.aspd * dt`, float multiply does not reassociate, and routing the control build through the arbiter's variable would move its step by an ULP a frame and re-roll the whole soak.
        }
        else if ((B.aRelax || 0) > 0) { B.aRelax -= dt;   // BOXED-IN ESCAPE (timed): advance under the SAME relaxed gate the escape was picked with — land + body-clear, step limit ignored — so it actually climbs/drops out of the pocket instead of facing the exit forever
          const tx = B.x + DIRa[B.ah][0] * 5, tz = B.z + DIRa[B.ah][1] * 5;
          if (walkFree(tx, tz)) { B.x += DIRa[B.ah][0] * B.aspd * dt; B.z += DIRa[B.ah][1] * B.aspd * dt; } }
        if (B.aAncX === undefined || (B.x - B.aAncX) * (B.x - B.aAncX) + (B.z - B.aAncZ) * (B.z - B.aAncZ) > 2.5 * 2.5) { B.aAncX = B.x; B.aAncZ = B.z; B.aStk = 0; B.aStkN = 0; }   // ── STEEP-TERRAIN STUCK WATCHDOG (user) ── judge by NET DISPLACEMENT, not blockage: a marcher can pass every walkOK test yet oscillate in place at a slope base (leash pulls uphill → blocked → contour turn → pulled back…). Real walking resets the anchor constantly; only genuine pinning accumulates.
        else if ((B.aStk = (B.aStk || 0) + dt) > 3 && !(B.aRelax > 0)) {
          B.aStk = 0; B.aStkN = (B.aStkN || 0) + 1;
          let best = -1, bgv = -1e9;                     // force the RELAXED escape (step limit ignored — same gate as boxed-in) toward the most climbable neighbour
          for (let h = 0; h < 4; h++) { const tx = B.x + DIRa[h][0] * 5, tz = B.z + DIRa[h][1] * 5, gA = mamArb ? navWalkStand(tx, tz) : bfSurf(tx, tz);
            if (walkFree(tx, tz) && gA > bgv) { bgv = gA; best = h; } }
          if (best < 0 || B.aStkN >= 3) { if (B.sN) unstampWorm(B); B.init = false; continue; }   // three failed escapes (or nowhere to go) = truly wedged → respawn on fresh ground
          B.ah = best; B.aTurnT = tb3 + 0.28; B.aRelax = 1.4;
        }
        B.animClk = (B.animClk || 0) + dt;   // armadillo walk clock (24 fps, read by stampArmadillo)
        if (skunkSlot || porcSlot) { const fps = (B.aflee ? 24 : 12) * (skunkSlot ? SKUNK_ANIM_MUL : 1); B.afps = (B.afps === undefined) ? fps : B.afps + (fps - B.afps) * Math.min(1, dt * 6); B.aframe = ((B.aframe || 0) + dt * B.afps) % (skunkSlot ? SKUNK_WALK.length : PORCUPINE_WALK.length); }   // SKUNK + PORCUPINE (user): ease 12↔24 fps (DOUBLE the walk cycle when fleeing, bunny-style) on a frame-position clock so switching rate never jumps a frame; read by stampSkunk/stampPorcupine   // ── SKUNK AT HALF SPEED (user 2026-08-06) ── 6 fps walking, 12 fleeing; it shared this line with the porcupine so the rate is SPLIT, not halved for both. The 24 fps house rule still holds everywhere else.
        B.om = 0; B.omT = 0; B.bspd = 0;                 // kill the shared glide/steering — the march above is the motion. Alignment lives in the GRID-STAMP poses (armOffset baked in), not a render offset — the armadillo grid-stamps again (user).
      } else if (B.kind === 2 && bunnySlot) {          // BUNNY (user): its MOTION IS THE BAKED ANIMATION — I apply no glide or turn of my own. The jump bake carries the −6 forward march; the rotate bake carries the 90° yaw. I only choose which sequence to play + follow the terrain. Cardinal grid, editor-identical.
        const DIRb = [[0, -1], [1, 0], [0, 1], [-1, 0]];
        const cardTh = (h) => Math.atan2(DIRb[h & 3][0], DIRb[h & 3][1]);
        const rot2b = (ox, oz, h) => { let x = ox, z = oz; for (let k = 0; k < (h & 3); k++) { const nx = -z, nz = x; x = nx; z = nz; } return [x, z]; };
        const pad2b = (n) => (n < 10 ? '0' + n : '' + n) + '.vox';
        const hopOK = (h) => { const dx = DIRb[h & 3][0], dz = DIRb[h & 3][1];   // check ALONG the whole −6 hop path (2,4,6 ahead), not just the landing — a trunk/rock the lunge would pass THROUGH must block it too (user: stuck against trees/terrain)
          for (let dd = 2; dd <= 6; dd += 2) { const tx = B.bpx + dx * dd, tz = B.bpz + dz * dd;
            if (mamArb) { if (!navWalkOK(tx, tz, B.bg, 3, 3, NAV_MCLR)) return false; continue; }   // the SAME three stations, asked of the field. Not the reach DDA the marchers use: a hop is ONE ballistic commitment from B.bg, so every station is judged against the TAKE-OFF ground (±3), never carried forward — carrying it would approve a 9-voxel climb the baked animation cannot make.
            const gA = navWalkStand(tx, tz);          // …and the SAME surface off the arbiter (user 2026-08-07): the hop is planned against whatever number the body will be SEATED on, or the bunny commits to an arc measured on the raw heightmap and lands on a nav-field surface up to 3 voxels elsewhere — the planner/mover split this project keeps rediscovering
            if (!(gA > WL + 0.5 && Math.abs(gA - B.bg) <= 3 && !bfObstW(tx, gA + 2, tz) && !bfObstW(tx, gA + 4, tz))) return false; }
          return true; };
        if (B.bh === undefined) { B.bh = ((Math.round(B.th / (Math.PI / 2)) % 4) + 4) % 4; B.bst = 0; B.bfclk = 0; B.bpx = B.x; B.bpz = B.z; B.bg = navWalkStand(B.x, B.z); B.bWhim = 0; B.bfps = 24; B.besc = 0; }   // bpx/bpz = base cell; bst: 0 jump | 1 rotate-left | 2 rotate-right; besc = committed escape-turn direction
        const NFR = 11;                                 // frames per baked sequence (00-10)
        const adx = B.bpx - P.x, adz = B.bpz - P.z, ady = P.y - (B.bg !== undefined ? B.bg : B.y);   // ady = vertical gap → the flee radius is now a SPHERE, not a ground circle: flying high overhead no longer spooks it (user)
        B.bflee = spooked(B) || (adx * adx + ady * ady + adz * adz) < (B.bflee ? 55 * 55 : 40 * 40);   // …or it has just been hit, which spooks it wherever the player is standing
        B.bfps += ((B.bflee ? 48 : 24) - B.bfps) * Math.min(1, dt * 6);   // ease the PLAY SPEED (flee = 2×) — the only thing I vary; the animation does the moving
        B.bfclk += dt * B.bfps;
        if (B.bfclk >= NFR) { B.bfclk -= NFR;           // a baked sequence just finished → commit its motion, pick the next
          if (B.bst === 0) { const [dx, dz] = rot2b(0, -6, B.bh); B.bpx += dx; B.bpz += dz; B.bg = navWalkStand(B.bpx, B.bpz); if (mamArb) navMoveK[7]++; }   // jump baked −6 forward → advance the base cell. The bunny's ground anchor comes from the SAME plane its hop gate reads, or the gate would measure ±3 from a surface the creature is not standing on.
          else B.bh = (B.bst === 1 ? B.bh + 1 : B.bh + 3) & 3;   // rotate baked a 90° yaw → commit the new cardinal
          if (B.bAncX === undefined || (B.bpx - B.bAncX) * (B.bpx - B.bAncX) + (B.bpz - B.bAncZ) * (B.bpz - B.bAncZ) > 5 * 5) { B.bAncX = B.bpx; B.bAncZ = B.bpz; B.bStkT = tb3; B.bStkN = 0; }   // ── STEEP-TERRAIN STUCK WATCHDOG (user) ── same net-displacement rule as the marchers: hopping around normally moves the base cell and resets the anchor; only real pinning ages the clock
          const bForce = tb3 - (B.bStkT === undefined ? tb3 : B.bStkT) > 4;
          if (bForce) { B.bStkT = tb3; B.bStkN = (B.bStkN || 0) + 1; }
          if (bForce || !hopOK(B.bh)) {                  // ── OBSTACLE AHEAD (user) ── (or the watchdog fired — then skip the polite turn and break out)
            if (!bForce && (hopOK(B.bh + 1) || hopOK(B.bh + 2) || hopOK(B.bh + 3))) {   // a clean cardinal EXISTS → commit ONE turn toward it, persisted so the escape can't oscillate (clears a corner in one turn, a dead-end in two)
              if (!B.besc) B.besc = hopOK(B.bh + 1) ? 1 : (hopOK(B.bh + 3) ? 2 : 1);
              B.bst = B.besc;
            } else {                                     // BOXED IN — no clean hop in ANY direction. THIS is what made it turn forever; break out instead of spinning (user: "gets stuck and rotates endlessly")
              B.besc = 0;
              let best = -1, bgv = -1e9;                 // the most climbable LAND neighbour, IGNORING the ±3 step limit → hop DOWN a ledge / over a small gap to escape the pocket
              for (let h = 0; h < 4; h++) { const tx = B.bpx + DIRb[h][0] * 6, tz = B.bpz + DIRb[h][1] * 6, gA = mamArb ? navWalkStand(tx, tz) : bfSurf(tx, tz);
                if (walkFree(tx, tz) && gA > bgv) { bgv = gA; best = h; } }   // the THIRD hand-rolled copy of the relaxed gate, now the shared walkFree — this one used to read the escape under a rule of its own while the two marcher escapes read another
              if (best >= 0 && (B.bStkN || 0) < 3) { B.bh = best; B.bst = 0; }   // snap to face the escape + HOP out (the vertical snap is worth breaking the spin)
              else if (mamArb && (B.bStkN || 0) < 3 && !navWalkOK(B.bpx, B.bpz, B.bg, 3, 3, NAV_MCLR)) { B.bst = 0; navEgrK[7]++; }   // ── EGRESS ── nothing around it is free AND its own cell is outside the travelable set: it is boxed in by terrain that arrived after it did. Hop along the heading rather than respawn — displacement, never a teleport, which is the whole difference between the arbiter's escape and the mercy recycle.
              else { if (B.sN) unstampWorm(B); B.init = false; continue; }   // truly islanded (only water/void within reach) OR three watchdog escapes went nowhere → respawn on fresh land
            }
          } else {                                       // ── CLEAR AHEAD ── escape done; resume normal behaviour
            B.besc = 0;
            if (B.bflee) { let best = B.bh, bd = -1e9; for (let h = 0; h < 4; h++) { const d = DIRb[h][0] * adx + DIRb[h][1] * adz; if (d > bd) { bd = d; best = h; } } const diff = (best - B.bh + 4) & 3; B.bst = diff === 0 ? 0 : diff === 3 ? 2 : 1; }   // face away from the player
            else if (B.hx !== undefined && (B.bpx - B.hx) * (B.bpx - B.hx) + (B.bpz - B.hz) * (B.bpz - B.hz) > 60 * 60) {   // LEASH: wandered too far from its RESERVED home cell → head back, so bunnies stay evenly spread and don't bunch up (user)
              const hx = B.hx - B.bpx, hz = B.hz - B.bpz; let best = B.bh, bd = -1e9; for (let h = 0; h < 4; h++) { const d = DIRb[h][0] * hx + DIRb[h][1] * hz; if (d > bd) { bd = d; best = h; } }
              const diff = (best - B.bh + 4) & 3; B.bst = diff === 0 ? 0 : diff === 3 ? 2 : 1; }
            else if (tb3 > B.bWhim) { B.bWhim = tb3 + 1.6 + Math.random() * 3; B.bst = Math.random() < 0.75 ? 0 : (Math.random() < 0.5 ? 1 : 2); }   // editor odds: 75% hop, 25% turn
            else B.bst = 0;
          }
        }
        const fi = Math.min(NFR - 1, Math.floor(B.bfclk));
        const bake = B.bst === 0 ? BUNNY_JUMP_BAKE : (B.bst === 1 ? BUNNY_ROT_BAKE : BUNNY_ROT_BAKE_R);
        const off = bake[pad2b(fi)] || [0, 0, 0];
        const [lx, lz] = rot2b(off[0] || 0, off[2] || 0, B.bh);   // THIS FRAME's baked lunge, rotated into the heading (rotate bakes have oz=0 → in place)
        B.x = B.bpx + lx; B.z = B.bpz + lz; B.bOy = off[1] || 0;   // baked bob (oy) is lifted in the emit
        B.th = (B.bst !== 0 && off[3] && off[3].length) ? cardTh(B.bst === 1 ? B.bh + 1 : B.bh + 3) : cardTh(B.bh);   // the baked 90° yaw sits on frames 6-10 → the facing SNAPS there, exactly like the editor (no manual interp)
        B.om = 0; B.omT = 0; B.bspd = 0; B.animClk = B.bfclk / 24;   // kill the shared glide/steering; animClk (=frames/24) feeds the emit's frame index
      } else if (antLead) {                          // ── THE ANT MARCHES ON THE COMPASS (user 2026-08-16: "just have it move forward. when it want to turn, rotate the ant 90 degrees") ──
        // The leader's whole controller, and it REPLACES the worm arbiter's scored heading fan for this one
        // slot rather than post-processing it. Rounding the fan's output was tried on paper and cannot work:
        // the fan re-rolls a wander of ±0.7 rad ABOUT THE CURRENT HEADING, which is narrower than the 0.785
        // a quarter turn needs, so a quantised fan marches dead straight forever — and widening it to reach
        // the boundary puts the ant back to flickering across it. An INTEGER heading is the honest way to say
        // "cardinal": B.ah indexes ANT_DIR, a turn is ±1 on it, and a diagonal is not expressible.
        // The MOVER below is deliberately untouched — it still translates along B.th and still gates the step
        // on navLandOK — so the ant keeps the band's brake, step rule and egress. Only the choosing changed.
        B.animClk = (B.animClk || 0) + dt;
        if (B.ah === undefined) { B.ah = ((Math.round(B.th / (Math.PI / 2)) % 4) + 4) % 4; B.aTurnT = 0; B.aesc = 0; B.aWhim = tb3 + ANT_WHIM; }   // the spawn heading, snapped to the nearest cardinal once
        const antOK = (h) => { const tx = B.x + ANT_DIR[h & 3][0] * ANT_LOOK, tz = B.z + ANT_DIR[h & 3][1] * ANT_LOOK, gA = bfSurf(tx, tz);
          return gA > WL + 0.5 && Math.abs(gA - bfSurf(B.x, B.z)) <= 2 && !bfObstW(tx, gA + 2, tz) && !bfObstW(tx, gA + 3, tz); };   // the WORM'S OWN step rule, asked ANT_LOOK voxels down a cardinal — so a heading the ant picks is one the mover will accept
        const antTurn = (d) => {                       // ── THE QUARTER TURN ── instant (one frame), and it pins a crumb AT THE CORNER first…
          if (B.trail && B.trail.length) B.trail.unshift({ x: B.x, z: B.z, th: B.th });   // …so the recorded path stays a polyline of exactly axis-aligned segments: every follower then turns on the same spot the leader did instead of cutting the corner across one 0.75-voxel diagonal chord
          B.ah = (B.ah + d) & 3; B.aTurnT = tb3 + ANT_TURN_HOLD;
        };
        if (!antOK(B.ah) || B.antBlk) {                // BLOCKED — either the lookahead refuses the lane, or the mover refused the actual step last frame (the backstop: the two predicates are close but not identical, and an ant that cannot move must never just grind)
          if (tb3 > B.aTurnT) {                        // commit to ONE side and hold it for ANT_TURN_HOLD, exactly as the marching mammals do, so a corner can never become a spin
            if (!B.aesc) B.aesc = antOK(B.ah + 1) ? 1 : (antOK(B.ah + 3) ? 3 : 1);
            antTurn(B.aesc);
          }
        } else {
          B.aesc = 0;                                  // clear again — the next block gets a fresh choice of side
          if (tb3 > B.aWhim) { B.aWhim = tb3 + ANT_WHIM + Math.random() * ANT_WHIM;
            if (Math.random() < ANT_WHIM_P) { const t = Math.random() < 0.5 ? 1 : 3; if (antOK(B.ah + t)) antTurn(t); } }   // …and an unblocked ant still turns now and then, 50/50 left or right, but only onto a lane it can actually walk
        }
        B.antBlk = 0;
        B.th = Math.atan2(ANT_DIR[B.ah][0], ANT_DIR[B.ah][1]);   // the ONE writer of B.th for this ant, and always exactly a cardinal — so the mover's step, the render yaw and every crumb it records are axis-aligned by construction
        B.om = 0; B.omT = 0;                           // kill the eased turn integrator: it is the continuous controller this branch exists to replace, and any non-zero om would drift B.th back off the compass one frame later
      } else if (B.kind === 2 && NAVARB) {              // ── WORM, ON THE ARBITER ── the crawl had FOUR writers of B.th: a random wander, a keep-apart push, a two-probe obstacle turn, and the home leash further down. Three of them used bfSurf — the HEIGHTMAP — while the mover's step rule used it at a different distance, so the planner could commit to a heading the mover then refused every frame, which is the whole of the 6 s/creature-min the band was losing. One scored fan at 12 Hz on ONE predicate replaces all four.
        B.animClk = (B.animClk || 0) + dt;
        if (tb3 > B.tRe) { B.navWander = B.th + (Math.random() - 0.5) * 1.4; B.tRe = tb3 + 1.5 + Math.random() * 2.5; }   // the WANDER survives as a score term instead of a direct omT write. Same two RNG draws at the same cadence as the line it replaces, so a seeded A/B still lines up.
        if (B.navRe === undefined) B.navRe = tb3 + (wk & 31) * 0.0026;   // stagger the sense tick across the band so 32 worms never think on the same frame
        if (tb3 >= B.navRe) {
          B.navRe = tb3 + NAV_HZ;
          let gx8 = 0, gz8 = 0;                          // ── ONE GOAL VECTOR ── the keep-apart push and the home leash, resolved into a single direction BEFORE the fan. They used to be two separate writers, one of them steering the worm straight into the trunk the other was trying to avoid.
          for (let j = 32; j < 64; j++) { const O = wbf[j]; if (O === B || !O.init || (O.kind | 0) !== 2) continue;
            const dxw = B.x - O.x, dzw = B.z - O.z, d2w = dxw * dxw + dzw * dzw;
            if (d2w < 26 * 26 && d2w > 0.01) { const il = 1 / Math.sqrt(d2w); gx8 += dxw * il * (26 - Math.sqrt(d2w)) / 26; gz8 += dzw * il * (26 - Math.sqrt(d2w)) / 26; } }   // unit vectors, weighted by closeness — the old raw sum let one very near worm swamp the leash entirely
          if (B.hcx !== undefined && (B.trap || 0) <= 0.35) { const lx = B.x - B.hx, lz = B.z - B.hz, l2 = lx * lx + lz * lz;   // STUCK (trap > 0.35): drop the pull toward home so the fan can steer AROUND the trunk instead of into it — the same exemption the direct leash carried
            if (l2 > WORM_LEASH * WORM_LEASH) { const il = 1.6 / Math.sqrt(l2); gx8 -= lx * il; gz8 -= lz * il; } }
          // ── AND THE FLEE / HUNT BEARING IS A GOAL, NOT AN OVERRIDE (user 2026-08-17: life "is also getting
          // stuck on objects. cactus for example") ── it used to be written straight into B.omT further down,
          // AFTER this fan, with no terrain in it at all: inside DES_DASH_R a gecko was pointed dead away from
          // the player, and where a cactus trunk stood on that bearing the brake clamped its step to exactly
          // zero and held it there — a few centimetres of travel over 2.3 s in the user's clip. That is the same
          // defect the keep-apart push and the home leash had before they were folded in above, so it is folded
          // in the same way: one more contributor to the ONE goal vector. Same bearing and the same two radii
          // the overrides carried, but the fan must now satisfy "away from the player" AND "down an open lane"
          // at once, which is what sends the animal AROUND the trunk instead of into it.
          if (desSlot && DESERTS[desSp]) {
            const hunt8 = !!DES_HUNT[DESERTS[desSp].name], run8 = !!DES_DASH[DESERTS[desSp].name] && !hunt8;   // ant, spider and every unlisted species are in neither table, so their goal vector stays empty and their plan is bit-identical
            const px8 = hunt8 ? P.x - B.x : B.x - P.x, pz8 = hunt8 ? P.z - B.z : B.z - P.z;
            const pd8 = px8 * px8 + pz8 * pz8, pr8 = hunt8 ? 90 : DES_DASH_R;
            if ((hunt8 || run8) && pd8 < pr8 * pr8 && pd8 > 0.01) { const il8 = 1 / Math.sqrt(pd8); gx8 += px8 * il8; gz8 += pz8 * il8; } }   // a UNIT vector like the keep-apart terms: navSteer2 reads only the DIRECTION, and no other term writes this vector for the desert band, so the length is free
          const gOn = gx8 * gx8 + gz8 * gz8 > 0.01;
          const gc8 = navGroundAt(B.x, B.z);
          const bioH8 = desertM(B.x, B.z) > 0.85;
          // Only a walker standing on its OWN ground clips against the line — one already stranded the wrong side
          // must be free to walk home, and the |dm-0.85| band keeps the extra desertM samples off the 90-odd
          // percent of the band that is nowhere near the boundary. The band is far wider than a plan tick of
          // travel, so a walker is always inside it well before the line is within reach.
          const bioOn8 = bioH8 === !!desSlot && Math.abs(desertM(B.x, B.z) - 0.85) < 0.15;
          navSteer2(B, navLandOK(B.x, B.z, gc8, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot),
            (th7, L7) => { const r7 = navReachLand(B.x, B.z, th7, L7, gc8, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot);
              return bioOn8 ? navBioClip(B.x, B.z, th7, r7, bioH8) : r7; },
            NAV_WREACH, 2 * NAV_WN, gOn ? Math.atan2(gx8, gz8) : 0, gOn, 2.6, 2.4);   // the same ±2.6 clamp and 2.4 gain the keep-apart push used, so the crawl's turn character is unchanged
        }
      } else if (B.kind === 2) {                       // WORM: smooth continuous meandering crawl (the random pause was removed — user)
        B.animClk = (B.animClk || 0) + dt;
        // ── ANT LINES (user 2026-08-15) ── the duckling follow-chain, reused whole. Slot 0 of a species is the
        // leader and 1..DES_PER-1 heel behind it, each steering for a point just behind the one AHEAD rather
        // than all at the leader — a chain, not a star — and falling back up the chain when a link dies. This
        // works here only because two things do NOT apply to the desert band: the keep-apart repulsion below
        // scans slots 32..64 (worms) so ants never shove each other out of line, and the home leash is gated on
        // B.hcx, which the annulus spawn never sets for them. Either would have fought the formation.
        const wormOK = (th6, dist) => { const ax = B.x + Math.sin(th6) * dist, az = B.z + Math.cos(th6) * dist, gA = bfSurf(ax, az);
          return gA > WL + 0.5 && Math.abs(gA - bfSurf(B.x, B.z)) <= 2 && !bfObstW(ax, gA + 2, az) && !bfObstW(ax, gA + 3, az); };   // ≤2 matches the STEP rule; +3 probe = body TOP clearance (bfObstW passes small ground clutter)
        if (tb3 > B.tRe) { B.omT = (Math.random() - 0.5) * 1.4; B.tRe = tb3 + 1.5 + Math.random() * 2.5; }
        let wrx = 0, wrz = 0;                           // SPREAD OUT — steer away from any nearby worm so they never bunch up (user); placement is already ≥60 apart, this stops runtime clumping
        for (let j = 32; j < 64; j++) { const O = wbf[j]; if (O === B || !O.init || (O.kind | 0) !== 2) continue;
          const dxw = B.x - O.x, dzw = B.z - O.z, d2w = dxw * dxw + dzw * dzw;
          if (d2w < 26 * 26 && d2w > 0.01) { const w = 26 - Math.sqrt(d2w); wrx += dxw * w; wrz += dzw * w; } }
        if (wrx * wrx + wrz * wrz > 1) {                // a worm is within ~2.6 m — bias the heading toward the away direction
          let dthw = Math.atan2(wrx, wrz) - B.th; dthw = Math.atan2(Math.sin(dthw), Math.cos(dthw));
          B.omT = Math.max(-2.6, Math.min(2.6, dthw * 2.4));
        }
        if (!wormOK(B.th, 5) || !wormOK(B.th, 2.5)) {  // unclimbable ahead (rock face / shoreline / cliff) — commit to a firm turn toward the open side (obstacle avoidance wins over the spread bias)
          const pF = wormOK(B.th + 1.1, 5), nF = wormOK(B.th - 1.1, 5);
          B.omT = pF && !nF ? 2.6 : (nF && !pF ? -2.6 : (B.om >= 0 ? 2.6 : -2.6));
        }
      } else if (B.kind === 3) {                       // DUCK: paddles its lake — veer away from the shore, gentle drifting turns
        if (isBaby && !orphan) {                       // DUCKLING: steer for a spot just behind its leader — mom for the first, the sibling ahead for the rest (a follow line)
          const lead = (sib === 0 || !wbf[wk - 1].init) ? mom5 : wbf[wk - 1];
          const tx = lead.x - Math.sin(lead.th) * 4.5, tz = lead.z - Math.cos(lead.th) * 4.5;
          let dth = Math.atan2(tx - B.x, tz - B.z) - B.th; dth = Math.atan2(Math.sin(dth), Math.cos(dth));
          B.omT = Math.max(-2.8, Math.min(2.8, dth * 2.2));
          B.chase = Math.hypot(tx - B.x, tz - B.z);    // distance to heel — drives the paddle speed
        } else {
          if (orphan) B.chase = 4;                     // …an ORPHAN has no line to hold, so it paddles the lake on its own: the edge-avoidance wander below, at the steady 7 the speed table gives for a chase over 3.5 (never the 10 of a duckling scrambling to catch up)
          const duckOK = (th6, dist) => duckFit(B.x + Math.sin(th6) * dist, B.z + Math.cos(th6) * dist);   // …and the lookahead is now literally the move rule: ONE function, so the planner cannot pick a lane the mover refuses
          let rx = 0, rz = 0;                          // PROACTIVE EDGE AVOIDANCE (user: 'keep the ducks further away from the terrain'): a stronger, WIDER repulsion so the duck banks off well before it can touch a shore/rock
          for (let k = 0; k < 8; k++) { const a = k * 0.785398, sa = Math.sin(a), ca = Math.cos(a);
            for (let d = 4; d <= 15; d += 3) { if (!bfWater(B.x + sa * d, B.z + ca * d)) { rx -= sa * (17 - d); rz -= ca * (17 - d); break; } } }   // sees banks out to 15 vox; closer edge → stronger push. This one stays on the bare water probe on purpose: it is a SOFT term that only bends the heading, stepOK re-asks the full question anyway, and a 33-voxel sky scan inside an 8×4 loop is ~1.7M voxel reads a second for nothing.
          if (rx * rx + rz * rz > 0.02) {              // a bank is within ~15 vox — ease the heading toward the resultant OPEN-water direction
            let dth = Math.atan2(rx, rz) - B.th; dth = Math.atan2(Math.sin(dth), Math.cos(dth));
            // If it has already wound up most of a turn, prefer unwinding the long way round to the
            // same heading rather than completing the loop.
            if (Math.abs(B.turnAcc || 0) > 4.0 && Math.sign(dth) === Math.sign(B.turnAcc)) dth -= Math.sign(dth) * 6.283185;
            B.omT = Math.max(-2.8, Math.min(2.8, dth * 2.8));
          } else if (tb3 > B.tRe) { B.omT = (Math.random() - 0.5) * 1.0; B.tRe = tb3 + 2 + Math.random() * 3; }   // open water — gentle wander
          // signed yaw carried over the last several seconds: ~±6.3 means a full circle one way
          B.turnAcc = (B.turnAcc || 0) * Math.exp(-0.48193 * dt) + (B.om || 0) * dt;   // PER-SECOND leak (0.48193 = −60·ln 0.992), IDENTICAL at 60 fps and unchanged in character. The old 0.992 was applied once per FRAME against a per-second input, so the steady value was 125·om·dt — ~5.8 at 60 fps but only 1.2-2.4 at the 185-400 fps this ships at, which left BOTH thresholds below (4.0 and 2.5) permanently dead: a mother duck circling a small bay never got the unwind-the-long-way-round correction or the reverse-direction penalty and orbited forever, on a fast machine only.
          if (!duckOK(B.th, 13) || !duckOK(B.th, 7)) {  // imminent bank ahead — turn down the LONGEST open lane (backs up the soft repulsion; earlier lookahead so it never reaches the shore)
            const duckReach = (th6) => { for (let d = 5; d <= 26; d += 3) if (!duckOK(th6, d)) return d; return 26; };
            const spin = Math.abs(B.turnAcc) > 2.5 ? Math.sign(B.turnAcc) : 0;   // already part-way round? penalise going further that way
            let bo = 0, bs = -1e9;
            for (const off of [0.6, -0.6, 1.2, -1.2, 1.9, -1.9, 2.7, -2.7]) {
              const sc = duckReach(B.th + off) - Math.abs(off) * 2.2 - (Math.sign(off) === spin ? 9 : 0);
              if (sc > bs) { bs = sc; bo = off; }
            }
            B.omT = Math.max(-3.4, Math.min(3.4, bo * 2.4));
          }
        }
      } else if (B.kind === 6) {                       // ── FISH 2.0 ── config-driven swimmer (FISH_CFG): SWIM ⇄ FLEE states in the water, dead-straight in the AIR.
        const FC = FISH_CFG, CRZ = FC.baseSpeed;
        if (freezeK < 0.4) B.animClk = (B.animClk || 0) + dt * ((B.spd || CRZ) / CRZ) * (FC.animFps / 24);   // tail-beat LOCKED to swim speed on the SIM clock (the frame index floors animClk·24): baseSpeed → animFps, double speed → EXACTLY 2×animFps. Render fps never enters the equation. FROZEN in the ice (freezeK≥0.4) → hold the tail frame too, not just movement (user)
        const ay = Math.floor(B.y);
        const fishOK = (th6, dist) => fishFits(B, th6, dist);      // BODY-aware + deep-enough water (see fishFits) — identical to what stepOK will accept
        const reachOf = (th6, maxD) => fishReach(B, th6, maxD);    // how far the open water runs along a heading — the compass for smooth channel-following
        if (B.jumpV === undefined) {                   // all steering is water-state only; an airborne fish holds its validated launch line
          // ── THREAT SCAN ── the player plus every configured predator creature (FC.predatorKinds — ducks by default);
          // the NEAREST one inside threatR (re)starts the flee window, and fleeHold keeps the state from flickering at the rim.
          let td2 = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z) + (B.y - P.y - 2) * (B.y - P.y - 2), tx9 = P.x, tz9 = P.z;
          if (FC.predatorKinds.length) for (let m9 = 16; m9 < 32; m9++) { const O = wbf[m9];   // ducks live in 16-31; widen the scan if other kinds ever join the list
            if (!O || !O.init || FC.predatorKinds.indexOf(O.kind | 0) < 0) continue;
            const q2 = (B.x - O.x) * (B.x - O.x) + (B.z - O.z) * (B.z - O.z) + (B.y - O.y) * (B.y - O.y);
            if (q2 < td2) { td2 = q2; tx9 = O.x; tz9 = O.z; } }
          if (td2 < FC.threatR * FC.threatR) { B.fleeT = tb3 + FC.fleeHold; B.thrX = tx9; B.thrZ = tz9; }
          if (tb3 < (B.fleeT || 0) || spooked(B)) {    // ── FLEE ── EXACTLY fleeMult × base speed, angling away along the CLEAREST open water (never bolt into a bank)   // …or struck: hitCreature sets the spook window AND thrX/thrZ, so a shot fish bolts off the player exactly as it bolts off a duck
            if (tb3 > (B.flRe || 0)) {                 // the escape-heading FAN re-plans at 8 Hz, not per render frame — at 700 fps the every-frame fan was most of the fish AI cost for zero behavioural gain (the om ease bridges 125 ms invisibly)
              B.flRe = tb3 + 0.125;
              const away = Math.atan2(B.x - (B.thrX !== undefined ? B.thrX : P.x), B.z - (B.thrZ !== undefined ? B.thrZ : P.z));
              let bo = 0, bs = -1e9;
              for (const off of [0, 0.7, -0.7, 1.6, -1.6]) { const sc = reachOf(away + off, 30) - Math.abs(off) * 6; if (sc > bs) { bs = sc; bo = off; } }   // 5 headings (was 9) — plenty to pick an open escape lane
              B.flTh = away + bo;
            }
            const fth = B.flTh !== undefined ? B.flTh : B.th;
            B.omT = Math.max(-FC.fleeYawRate, Math.min(FC.fleeYawRate, Math.atan2(Math.sin(fth - B.th), Math.cos(fth - B.th)) * 5));
            B.spdT = CRZ * FC.fleeMult; B.dT = 0.6 + Math.random() * 0.3;
            B.fleeing = true;
          } else {                                     // ── CRUISE ── the LOCAL NAVIGATION PROBE (in the sense block below) is now the compass: a whisker fan
            B.fleeing = false;                         // scanning the water every tick, so the cruise here only sets the long-range INTENT (drift straight, held by
            // navTh) plus school cohesion. The probe then bends that intent onto the freest lane, which is what keeps the
            // fish clear of banks and rocks proactively instead of only reacting once one is in its face (user).
            let want = B.navTh !== undefined ? B.navTh : B.th;
            if (B.school >= 0) {                       // schooling BLENDS with the channel: mostly stick with the group, but bend toward open water so the school never piles into a bank
              let cx = 0, cz = 0, hx = 0, hz = 0, sxx = 0, szz = 0, n = 0;
              for (let m = 244; m < 276; m++) { const O = wbf[m]; if (O === B || !O.init || O.school !== B.school) continue;
                const dxs = O.x - B.x, dzs = O.z - B.z, d2 = dxs * dxs + dzs * dzs; if (d2 > 42 * 42) continue;
                cx += O.x; cz += O.z; hx += Math.sin(O.th); hz += Math.cos(O.th); n++;
                if (d2 < 7 * 7 && d2 > 0.01) { const w = 7 - Math.sqrt(d2); sxx -= dxs * w; szz -= dzs * w; } }
              if (n > 0) { const gx = (cx / n - B.x) + sxx * 1.6, gz = (cz / n - B.z) + szz * 1.6;
                const sth = (gx * gx + gz * gz > 1) ? Math.atan2(gx, gz) : Math.atan2(hx, hz);
                want = Math.atan2(0.68 * Math.sin(sth) + 0.32 * Math.sin(want), 0.68 * Math.cos(sth) + 0.32 * Math.cos(want)); } }
            B.navGoal = want;                          // the intent handed to the whisker probe — it steers toward the freest lane NEAR this, not blindly at it
            B.spdT = CRZ;                              // NORMAL swimming runs at the configured base speed, exactly — the flee state is the sole 2× (user)
          }
          B.spd = (B.spd || CRZ) + ((B.spdT || CRZ) - (B.spd || CRZ)) * (1 - Math.exp(-((B.spdT || 0) > (B.spd || CRZ) ? 6 : 1.4) * dt));   // asymmetric ease: the kick to double is FAST, the bleed-off slow — dart, then coast
          if (tb3 > (B.dRe || 0)) { B.dT = 0.25 + Math.random() * 0.6; B.dRe = tb3 + 2 + Math.random() * 4; }   // depth wander — a new hold-depth every few seconds
          // ── SENSE AT ~14 Hz, ACT EVERY FRAME ── the voxel probing re-evaluates on a ~70 ms cadence (a fish covers a
          // voxel or two between checks — nothing sneaks inside the multi-body-length whiskers), while the cached RESULT
          // (nav heading, push vector, backstop, speed cap) applies every frame so motion stays perfectly smooth. At
          // uncapped render rates the per-frame probing WAS the fish AI cost.
          if (tb3 > (B.senseRe || 0)) { B.senseRe = tb3 + 0.07;
            // ── LOCAL NAVIGATION PROBE ── the "sensors constantly scanning the water" (user). A whisker fan sweeps the
            // front arc around the current intent; each whisker measures how far the BODY can travel down it (fishReach)
            // AND whether both flanks a few body-lengths ahead are open water (so it stays CENTRED in a channel, off the
            // banks). The fish continuously banks toward the freest, best-centred lane — it curves around a shore, rock
            // or river bend from many voxels out instead of only reacting once the wall is in its face. probeMax scales
            // with speed, so a fast fish looks correspondingly further ahead.
            const goal = B.navGoal !== undefined ? B.navGoal : B.th;
            const probeMax = Math.max(20, B.spd * 1.1);
            const flankD = Math.max(5, (B.fhalf || 5) + 2);
            const flankClear = (th, side) => { const qx = B.x + Math.sin(th + side) * flankD, qz = B.z + Math.cos(th + side) * flankD;
              return (bfWater(qx, qz) && WL - bfBed(qx, qz) >= 3 && !solid(Math.floor(qx), ay, Math.floor(qz))) ? 1 : 0; };
            let bestTh = goal, best = -1e9, bestReach = 0;
            for (const off of [0, 0.3, -0.3, 0.62, -0.62, 1.0, -1.0, 1.5, -1.5, 2.2, -2.2]) {
              const th = goal + off;
              const reach = fishReach(B, th, probeMax);
              if (reach < 3) continue;                  // the body can't even start down this whisker — never a candidate
              const flank = flankClear(th, 1.05) + flankClear(th, -1.05);   // 0..2 — lanes with water on both sides win, centring the fish
              const turn = Math.abs(off) + Math.abs(Math.atan2(Math.sin(th - B.th), Math.cos(th - B.th))) * 0.5;   // prefer the goal AND the current heading → smooth, no dithering
              const sc = Math.min(reach, probeMax) + flank * 6 - turn * FC.turnCost;
              if (sc > best) { best = sc; bestTh = th; bestReach = reach; }
            }
            if (best <= -1e9) { bestTh = B.th + (B.om >= 0 ? 3.14159 : -3.14159); bestReach = 0; }   // every whisker blocked → about-face
            B.navTh = bestTh; B.navReach = bestReach;

            let rx = 0, rz = 0;                         // ── BANK REPULSION ── a safety NET for a body that has already drifted a nose/tail into terrain even though the centre is in water: sum a push off any shore/rock within 9 vox and slide toward open water. With the probe steering proactively this rarely fires, but it is what physically peels a fish off an edge steering alone cannot.
            for (let k = 0; k < 8; k++) { const sa = Math.sin(k * 0.7854), ca = Math.cos(k * 0.7854);
              for (let d = 3.5; d <= 9; d += 2.75) { const qx = B.x + sa * d, qz = B.z + ca * d;
                if (WL - bfBed(qx, qz) < 3 || solid(Math.floor(qx), ay, Math.floor(qz)) || solid(Math.floor(qx), ay + 1, Math.floor(qz))) { rx -= sa * (10 - d); rz -= ca * (10 - d); break; } } }
            B.repX = rx; B.repZ = rz;
            const look = Math.max(FC.lookMin, B.spd * 0.62);   // ── REACTIVE WALL BACKSTOP ── last-ditch: terrain inside the immediate lookahead → hard override + slow. The probe should prevent this from ever tripping, so it is now purely a fail-safe.
            B.bkOn = !fishOK(B.th, look) || !fishOK(B.th, 5);
            if (B.bkOn && tb3 > (B.bkRe || 0)) {
              B.bkRe = tb3 + 0.1;
              let bo = 0, bs = -1e9;
              for (const off of [0, 0.5, -0.5, 1.1, -1.1, 2.0, -2.0]) { const sc = reachOf(B.th + off, look) - Math.abs(off) * 3.5 - (off * B.om < 0 ? 4 : 0); if (sc > bs) { bs = sc; bo = off; } }
              // ── THE ABOUT-FACE IS THE LAST RESORT, NOT THE SECOND (user 2026-08-07) ── `bs` is scored with
              // reachOf capped at `look`, so in any confined water it sits under 4 routinely and this used to spin
              // the fish through 180° on a short-range opinion while the long-range probe still had a perfectly good
              // lane. Ask the probe first: only when IT has nothing either (navReach under a body length) is the water
              // genuinely a dead end. Otherwise take the scan's own offset and let it add urgency to the probe's turn.
              const probeStuck = (B.navReach === undefined ? 0 : B.navReach) < 4;
              B.bkTh = (bs < 4 && probeStuck ? (B.om >= 0 ? 3.14159 : -3.14159) : bo); B.bkTight = bs < look * 0.5;
            }
          }
          if (!B.fleeing) {                             // ── STEER toward the probe's chosen lane every frame (cached navTh) ── the eased om below smooths the ~70 ms cadence into a continuous glide
            let dth = Math.atan2(Math.sin((B.navTh !== undefined ? B.navTh : B.th) - B.th), Math.cos((B.navTh !== undefined ? B.navTh : B.th) - B.th));
            B.omT = Math.max(-FC.yawRate, Math.min(FC.yawRate, dth * 2.2));
            const nr = B.navReach !== undefined ? B.navReach : 30;   // PROACTIVE SLOWDOWN: the open lane is short → ease off well before the backstop so tight water is taken at a controlled pace
            if (nr < 16) B.spd = Math.min(B.spd, Math.max(9, nr));
          }
          { const rx = B.repX || 0, rz = B.repZ || 0;   // apply the CACHED repulsion each frame (dt-scaled → frame-rate independent slide)
            if (rx * rx + rz * rz > 0.3) { const rl = Math.hypot(rx, rz), nrx = rx / rl, nrz = rz / rl;
              if (bfWater(B.x + nrx * 2, B.z + nrz * 2) && WL - bfBed(B.x + nrx * 2, B.z + nrz * 2) >= 3) { B.x += nrx * Math.min(7, rl) * dt * 1.9; B.z += nrz * Math.min(7, rl) * dt * 1.9; }   // slide toward open water, but only if that step is itself clear (never shove into the opposite bank)
              const dr = Math.atan2(Math.sin(Math.atan2(nrx, nrz) - B.th), Math.cos(Math.atan2(nrx, nrz) - B.th));
              B.omT = Math.max(-5, Math.min(5, B.omT + dr * 2.2)); } }
          if (B.bkOn) {                                 // ── THE BACKSTOP ADDS URGENCY; IT NO LONGER REPLACES THE PLAN (user 2026-08-07) ──
            // It used to overwrite omT outright. But `look` is max(12, spd·0.62) ≈ 13.6 voxels, and in a river a point
            // 13.6 ahead is land most of the time — measured, this "fail-safe" was tripping on 79% of frames. So four
            // frames in five the fish threw away the whisker probe's 24-voxel, flank-centred lane and steered instead on
            // a 7-way scan capped at 13.6 that about-faces whenever nothing scores above 4. That is the ping-pong
            // between banks, and it is why steering felt reactive however good the probe got. Adding to the probe's turn
            // keeps the long-range lane authoritative while a genuine wall still dominates: an about-face is ±π here, so
            // it swamps the sum by construction, while a mild bkTh only nudges.
            B.omT = Math.max(-6.5, Math.min(6.5, B.omT + (B.bkTh || 0) * 2.5));
            B.spd = Math.min(B.spd, B.bkTight ? 8 : 16);     // tight gap → crawl; comfortable gap → shed the dart but keep way on
          }
        } else { B.omT = 0; B.fleeing = false; }       // AIRBORNE: no steering — the splash-down was validated for the launch line and must stay where it was aimed
      } else if (NAVARB) {                             // ── FLYER, ON THE ARBITER ── ONE scored candidate fan at 12 Hz, and ONE feasibility answer (navFitsAir) shared by this planner, the mover below, the vertical step and the escape — so "the planner chose a move the mover refuses" is not expressible. It replaces two POINT probes at 7 and 13 voxels: at 56 vox/s against an 11.2-vox turn radius, 13 vox is 0.23 s of warning, and a trunk at 10 voxels was invisible to the planner while the mover advanced 0.93 vox a frame straight into it.
        if (tb3 > B.tRe) { B.navWander = B.th + (Math.random() - 0.5) * 2.2; B.tRe = tb3 + 0.4 + Math.random() * 0.8; }   // the WANDER survives as a score term instead of a direct omT write. Same two RNG draws at the same cadence as the line it replaces, so a seeded A/B still lines up.
        if (B.navRe === undefined) B.navRe = tb3 + (wk & 15) * 0.0052;   // stagger the sense tick across the band so 16 flyers never think on the same frame
        if (tb3 >= B.navRe) {
          B.navRe = tb3 + NAV_HZ;
          let homeTh = 0, leashOut = false;
          if (B.hcx !== undefined) { const lx = B.x - B.hx, lz = B.z - B.hz;   // the LEASH becomes a score term too — the block below used to bend B.th directly, which is a second writer steering against the avoidance
            if (lx * lx + lz * lz > FLY_LEASH * FLY_LEASH) { leashOut = true; homeTh = Math.atan2(-lx, -lz); } }
          navSteerAir(B, homeTh, leashOut);
        }
      } else {
        if (tb3 > B.tRe) { B.omT = (Math.random() - 0.5) * 4.0; B.tRe = tb3 + 0.4 + Math.random() * 0.8; }
        const la5 = 13;
        if (bfObst(B.x + Hx2 * la5, B.y, B.z + Hz2 * la5) || bfObst(B.x + Hx2 * 7, B.y + 1, B.z + Hz2 * 7)) {   // flyers see obstacles EARLY so the EASED turn has room — no last-moment snap
          const pFree = !bfObst(B.x + Math.sin(B.th + 1.0) * la5, B.y, B.z + Math.cos(B.th + 1.0) * la5);
          const nFree = !bfObst(B.x + Math.sin(B.th - 1.0) * la5, B.y, B.z + Math.cos(B.th - 1.0) * la5);
          B.omT = pFree && !nFree ? 5.0 : (nFree && !pFree ? -5.0
            : (!pFree && !nFree ? (!bfObst(B.x + Math.sin(B.th + 2.4) * 9, B.y, B.z + Math.cos(B.th + 2.4) * 9) ? 6.4 : -6.4)   // fully cornered (dense canopy pocket) — swing back toward the way it came
            : (B.om >= 0 ? 5.0 : -5.0)));              // steer toward the open side; the ease below smooths the heading — no discontinuous snap
        }
      }
      const iceLock = (B.kind === 3 || B.kind === 6) && freezeK >= 0.4;   // DUCK on the surface / FISH under it → locked in the ice when the water turns to ice: no paddling/swimming, turning or depth-wander. 0.4 = where the waves flatten + the ice look is set in (user)
      if (iceLock) { B.om = 0; B.omT = 0; B.trap = 0; }   // hold the heading + clear any pending turn/stuck state so it resumes cleanly on thaw
      else { B.om += (B.omT - B.om) * (1 - Math.exp(-9 * dt)); B.th += B.om * dt; }
      if (B.kind === 4) { B.th += B.spin * dt; Hx2 = Math.sin(B.mth); Hz2 = Math.cos(B.mth); }   // lily: spin the MODEL (th) freely while drifting along mth
      else { Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th); }
      const spd5 = iceLock ? 0 : (B.kind === 6 ? (B.spd || 6) : (B.kind === 4 ? 1.1 : (B.kind === 3 ? (isBaby ? (B.chase > 9 ? 10 : (B.chase > 3.5 ? 7 : 1.5)) : 7)   // fish ride their live burst-glide speed; lilies drift; ducklings hustle when behind, dawdle at heel
        : (B.kind === 2 ? ((bunnySlot || armSlot || skunkSlot || porcSlot) ? (B.bspd || 0) : (desSlot && DESERTS[desSp] ? ((B.chase > 6 ? 34 : (B.chase > 3.6 ? 22 : (B.chase > 0 ? 13 : 0))) || DES_SPD[DESERTS[desSp].name] || 16) * ((DES_DASH[DESERTS[desSp].name] && ((P.x - B.x) * (P.x - B.x) + (P.z - B.z) * (P.z - B.z)) < DES_DASH_R * DES_DASH_R) ? DES_DASH[DESERTS[desSp].name] : 1) : 16)) : (B.kind === 1 ? 26 : 56)))));   // bunny/armadillo/skunk/porcupine drive their OWN motion (bspd 0 → no shared glide); worm 1.6 m/s (continuous), firefly 2.6, butterfly 5.6  [extra ) closes the iceLock ternary]
      const mamSlot = bunnySlot || armSlot || skunkSlot || porcSlot;   // the LAND MAMMALS share B.kind 2 with the worm but drive their own march; the worm's arbiter wiring is theirs alone
      // ── DESERT STEERING OVERRIDE ── runs AFTER both kind-2 branches have chosen a heading, because the
      // desert creatures take the NAV-ARBITER branch (wormArb below is true for them) and anything written
      // into the plain worm branch never executes for them at all. Setting B.omT here wins either way.
      // ── TURN AWAY FROM THE BIOME EDGE (user 2026-08-16: life "gets stuck on the invisible wall between
      // biomes") ── for the bands that are NOT on the arbiter. The mammals, the bunny and the ant leader each
      // drive their own walk and never reach navSteer2, so the reach clamp above cannot help them; without this
      // they walk into the line and the step rule holds them there. Measured: this block first went in INSIDE
      // the desert gate below, which meant every pine-forest mammal — the whole population on the other side of
      // the line — had no avoidance whatsoever, and the residual stall was mostly them.
      if ((B.kind | 0) === 2 && B.init && (mamSlot || bunnySlot || antLead)) {
        const home9 = !!desSlot;
        if ((desertM(B.x + Hx2 * BIO_LOOK, B.z + Hz2 * BIO_LOOK) > 0.85) !== home9) {
          const lx9 = Math.sin(B.th + 1.2), lz9 = Math.cos(B.th + 1.2);
          const rx9 = Math.sin(B.th - 1.2), rz9 = Math.cos(B.th - 1.2);
          const lOK9 = (desertM(B.x + lx9 * BIO_LOOK, B.z + lz9 * BIO_LOOK) > 0.85) === home9;
          const rOK9 = (desertM(B.x + rx9 * BIO_LOOK, B.z + rz9 * BIO_LOOK) > 0.85) === home9;
          const turn9 = lOK9 ? 1.2 : (rOK9 ? -1.2 : 3.14159);
          B.th += Math.max(-2.4 * dt, Math.min(2.4 * dt, turn9));
          Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
          B.mth = B.th;
        }
      }
      if (desSlot && (B.kind | 0) === 2) {
        const desLine = DESERTS[desSp] && DESERTS[desSp].name === 'ant';
        const desIdx = (wk - MAM_END) % DES_PER;
        // ── ANT FOLLOWERS ARE NO LONGER STEERED AT ALL ── they are PLACED on the leader's own path, after the
        // move, further down this loop. Steering them could not work and the measurements say why: a follower
        // aimed at a point behind the leader's INSTANTANEOUS heading, and its turn-rate cap gives it a minimum
        // turn radius of 4.6-12.1 voxels against a 3.2 voxel spacing. Since each ant's own jittery heading was
        // the reference for the next one, the error amplified down the chain — a string instability. Measured
        // gaps ran 6.8 / 17.4 / 25.5 / 29.4 / 30.9 against an intended 3.2, they overtook each other (slot
        // order matched travel order in only 20% of frames) and passed straight through one another.
        if (desLine && desIdx > 0) B.chase = 0;
        // ── THE HUNTERS ── cobra and scorpion steer AT the player instead of wandering. The mammals' flee
        // maths inverted: they maximise the dot with the away vector, this maximises it with the toward one.
        // ── THE PREY RUNS THE OTHER WAY (user 2026-08-16: "if the mouse or gecko come near the player, have
        // them run in the opposite direction. keep the 2x speed") ── the mirror image of the hunt block below.
        // Same radius as DES_DASH, so the animal starts sprinting and starts fleeing on the same step rather
        // than bolting in whatever direction it happened to be pointing. The speed itself is untouched —
        // DES_DASH still doubles it — this only decides WHERE the doubled speed is spent.
        // ── BOTH BEARINGS NOW GO IN THROUGH THE PLANNER ── the B.omT write that used to sit in each of these
        // two blocks is gone; it ran every frame AFTER the fan and threw the fan's answer away, which is the
        // whole of the cactus stall. What is left here is the WANDER target, which is the one thing the fan
        // cannot derive for itself: pointing it along the run keeps the wander term (0.70) pulling the same way
        // as the goal term (0.95) instead of dragging the sprint off toward a stale random bearing.
        // ── AND THE RE-ROLL IS DEFERRED, NOT KILLED ── this was `tb3 + 1e9`: one pass within DES_DASH_R froze
        // B.navWander for the rest of the creature's life, since nothing outside a slot recycle ever wrote tRe
        // again, so long after the player had gone the animal was still steering at one fixed compass bearing.
        // Re-armed 0.5 s ahead every frame the chase is live, it is suppressed for exactly as long as the run
        // lasts and the ordinary 1.5-4 s re-roll resumes half a second after it ends.
        if (desSlot && DESERTS[desSp] && DES_DASH[DESERTS[desSp].name] && !DES_HUNT[DESERTS[desSp].name]) {
          const fdx = B.x - P.x, fdz = B.z - P.z, fd2 = fdx * fdx + fdz * fdz;
          if (fd2 < DES_DASH_R * DES_DASH_R && fd2 > 0.01) {
            B.navWander = Math.atan2(fdx, fdz);
            B.tRe = tb3 + 0.5;
          }
        }
        if (desSlot && DESERTS[desSp] && DES_HUNT[DESERTS[desSp].name]) {
          const hdx = P.x - B.x, hdz = P.z - B.z, hd2 = hdx * hdx + hdz * hdz;
          if (hd2 < 90 * 90) {
            B.navWander = Math.atan2(hdx, hdz);        // the charge bearing, same as the flee above: the goal term steers, the wander term stops fighting it
            B.tRe = tb3 + 0.5;
            // CONTACT. This used to call die() outright — the comment here noted that the moment a health
            // counter existed this is where it would be decremented instead, and that is now what happens.
            // A cobra bites harder than a scorpion, and both have a cooldown so standing in one cannot drain
            // the whole bar in a single second.
            // ── 6.5, NOT 2.2, AND THE OLD NUMBER WAS UNREACHABLE ── hd2 is CENTRE to CENTRE, and the player's
            // own half-width is HW = 2.6, so a snake pressed against the player is already 2.6+ away before
            // its own body is counted: the 2.2 test could never be true and the cobra never bit at all. The
            // scorpion only ever landed the one hit it got while closing head-on. 6.5 is the player's half
            // width plus the attacker's, plus a little, so a creature CIRCLING at contact range keeps testing
            // true and keeps biting on the cooldown — which is what the user asked for. Exactly the same
            // mistake the cactus contact test made; see cactusHurtAt, where the fix was a box sweep.
            // ── THE REACH IS THE ANIMAL'S OWN SIZE, NOT A CONSTANT ── measured: at contact the SCORPION sits
            // 4.7 away with its centre 3.7 above the player's feet, but the COBRA sits 12.2 away with its
            // centre 8.4 up. It is a 19-segment model, so its bulk holds it further out and its seat lifts
            // its centre far higher — a single pair of constants cannot cover both, and every constant tried
            // so far (2.2, then 6.5) simply excluded the cobra, which is why it never bit ONCE while the
            // scorpion stung fine. Both bounds now scale with the creature's own MAMFIT footprint and seat.
            const fB = DESERTS[desSp] ? MAMFIT[DESERTS[desSp].name] : null;
            const bReach = 5.0 + (fB ? fB.hd : 2), bRise = 4.0 + (fB ? fB.seat : 2);
            if (!dead && !P.fly && hd2 < bReach * bReach && Math.abs(P.y - B.y) < bRise && tb3 > (B.bitT || 0)) {
              B.bitT = tb3 + 1.0;
              const cobra = DESERTS[desSp].name === 'cobra';
              vitHurt(cobra ? 5 : 3, cobra ? 'a cobra struck you' : 'a scorpion stung you');
            }
          }
        }
      }
      const wormArb = NAVARB && (B.kind | 0) === 2 && !mamSlot;
      const gcW = wormArb ? navGroundAt(B.x, B.z) : 0;   // the worm's OWN travel surface, from the field — one number shared by its brake, its step test and its y servo, so all three agree on where the ground is
      let mv5 = spd5 * dt, nx5, nz5, wBrk0 = false;   // the frame's step LENGTH as its own variable, so the flyer brake has something to cap (wBrk0: the worm brake clamped it to exactly zero — see below)
      if (NAVBRK && (B.kind | 0) === 6 && B.jumpV === undefined) {   // ── FISH, ON THE BRAKE ── airborne is exempt: a salmon's leap was validated at launch and must fly its arc at full speed.
        mv5 = navBrake2(B, (th7, L7) => fishReach(B, th7, L7), mv5, dt, NAV_FLOOK, NAV_FBCLR, NAV_FBRK2, 6);
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      if (NAVBRK && duckArb && !iceLock) {            // ── DUCK, ON THE BRAKE ── the paddle translates along B.th, which lags the planned heading while the eased turn integrator catches up. The head buffer already refuses a step with under 7 voxels of water ahead, so without a cap the duck's only way to honour it is a hard STOP; the ramp turns that into an arrival that is already slow.
        mv5 = navBrake2(B, (th7, L7) => navReach2(duckFit, B.x, B.z, th7, L7), mv5, dt, NAV_DLOOK, NAV_DBCLR, NAV_DBRK2);
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      else if (NAVBRK && wormArb) {                   // ── WORM, ON THE BRAKE ── the mover translates along B.th, which lags the planned heading while the eased turn integrator catches up; capping the step by the reach of the lane actually being crawled means the step can no longer END past it
        mv5 = navBrake2(B, (th7, L7) => navReachLand(B.x, B.z, th7, L7, gcW, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot), mv5, dt, NAV_WLOOK, NAV_WBCLR, NAV_WBRK2);
        if (B.navClear < NAV_WLOOK && B.navRe - tb3 > 0.034) B.navRe = tb3 + 0.034;   // a braking worm re-plans at 30 Hz instead of 12 — a slowed creature must not creep at the obstacle for the rest of an 83 ms tick
        // ── A BRAKE-TO-ZERO IS A BLOCKED FRAME, AND IT USED TO BE INVISIBLE ── at reach ≤ NAV_WBCLR the curve's
        // sqrt term is 0 and the geometric cap holds it there, so mv5 is EXACTLY 0 and nx5/nz5 come out equal to
        // B.x/B.z. The step test below is then asked about the creature's own cell, which is legal sand by
        // construction, so it passed and the accepted-move line cleared trap/stuck/noMove every single frame:
        // the animal was pinned a voxel off a cactus with every stall counter reading zero, the blocked branch
        // never ran, and the 12 s mercy recycle could never fire. This flag changes no motion whatsoever — mv5
        // is already 0 — it only lets the counters see what the brake is doing, so the recycle is a real
        // backstop again if a heading the fan cannot solve ever gets through.
        wBrk0 = B.navClear <= NAV_WBCLR;
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      else if (NAVBRK && B.kind < 2) { mv5 = navBrakeAir(B, mv5, dt);
        if (B.navClear < NAV_LOOK && B.navRe - tb3 > 0.034) B.navRe = tb3 + 0.034;   // a BRAKING flyer re-plans at 30 Hz instead of 12. The rejection used to force that re-plan (B.navRe = 0 below) and closing the rejection would otherwise have taken the re-plan away with it, leaving a slowed creature to creep at the obstacle for the remainder of an 83 ms tick.
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      else { nx5 = B.x + Hx2 * spd5 * dt; nz5 = B.z + Hz2 * spd5 * dt; }   // EVERY OTHER BAND KEEPS THE ORIGINAL EXPRESSION, CHARACTER FOR CHARACTER. `Hx2 * mv5` is not `Hx2 * spd5 * dt`: float multiply does not reassociate, and routing a worm through the flyer's variable moved its step by 1 ULP a frame — enough to send a 240 s soak down a different trajectory and make a chaotic re-roll look like a regression in a band this change does not touch. ?nobrake is now bit-exact for kinds 2-6.
      let stepOK;
      if (B.kind === 2 && NAVARB && !mamSlot) {        // ── WORM, THE SAME ANSWER THE FAN SCORED WITH ── so "the planner chose a move the mover refuses" is not expressible
        navMoveK[2]++; stepOK = navLandOK(nx5, nz5, gcW, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot);   // …and for the DESERT band the same predicate also refuses a rock or a cactus outright, so it walks around one instead of up it (user 2026-08-16)
        if (!stepOK) { navRejK[2]++; B.navRe = 0;       // a rejection can only mean the world moved under the worm between sense ticks, so it forces an immediate re-plan
          if (!navLandOK(B.x, B.z, gcW, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot)) { stepOK = true; navEgrK[2]++; } }   // ── EGRESS ── the worm is already OUTSIDE the travelable set (a tree landed on it, it streamed in beside rock). Refusing the step would pin it there until the mercy recycle; it crawls out along its heading instead. Displacement, never a teleport.
      } else if (B.kind === 2) {                        // STEP-AWARE crawl: accept the move if the destination surface is land, within a 2-voxel step, and clear at ITS height AND above the body
        const gN = bfSurf(nx5, nz5);
        stepOK = gN > WL + 0.5 && Math.abs(gN - bfSurf(B.x, B.z)) <= 2 && !bfObstW(nx5, gN + 2, nz5) && !bfObstW(nx5, gN + 3, nz5);
      } else if (B.kind === 6) {                       // ── FISH TERRAIN HITBOX (user) ── the WHOLE BODY (a ~10-vox model) must clear solid, not just the centre point — otherwise the long nose pokes into a wall the centre hasn't reached. Sample nose→tail across the body height; any point in rock/ground/shore rejects the move. NO player hitbox — the fish still passes freely through you.
        stepOK = B.jumpV !== undefined ||              // AIRBORNE: mid-arc + splash-down were validated at launch — the water tests below would halt the arc in mid-air over a pad or bank lip
          fishFitsAt(B, nx5, nz5, B.th);               // …otherwise THE one answer: real, deep-enough water AND the whole body fitting there, so the planner can only ever be told to go where it can actually go
      } else if (duckArb) {                            // ── DUCK, THE SAME ANSWER THE PLANNER SCORED WITH ──
        navMoveK[3]++;
        stepOK = duckFit(nx5, nz5) && (bfOpenW(nx5, nz5) || !bfOpenW(B.x, B.z));   // bfOpenW stays MOVER-ONLY and stays out of the shared answer on purpose: it is the no-gorge rule, and because a duck already inside a narrow arm is always allowed to move, it can refuse a step but can never pin one — the planner has open lanes by construction wherever this term bites.
        if (stepOK) { const hl = isBaby ? 5.0 : 7.0; stepOK = duckFit(nx5 + Hx2 * hl, nz5 + Hz2 * hl); }   // DUCK HEAD BUFFER (user: 'keep further away, still running into it'): require water WELL ahead (7 vox mom / 5 baby) so it stops with a clear margin, never touching the bank
        if (!stepOK) { navRejK[3]++;
          if (!duckFit(B.x, B.z)) { stepOK = true; navEgrK[3]++; } }   // ── EGRESS ── the duck is already outside the travelable set (the lake iced over and thawed under it, the window recentred). Refusing the step would pin it; it paddles out along its heading instead.
      } else if (B.kind >= 3) { stepOK = bfWater(nx5, nz5) && bfSky(nx5, WL + 2, nz5)   // ducks + lilies only ever move onto OPEN-SKY REAL water — never onto a dry ravine floor or under a cave roof…
          && (bfOpenW(nx5, nz5) || !bfOpenW(B.x, B.z));  // …and never INTO a narrow gorge/ravine arm (escape moves out of one are always allowed so nothing gets pinned)
        if (B.kind === 3 && stepOK) { const hl = isBaby ? 5.0 : 7.0; stepOK = bfWater(nx5 + Hx2 * hl, nz5 + Hz2 * hl); }   // DUCK HEAD BUFFER (user: 'keep further away, still running into it'): require water WELL ahead (7 vox mom / 5 baby) so it stops with a clear margin, never touching the bank
      }
      else if (NAVARB) { navMoveN++; stepOK = navFitsAir(nx5, B.y, nz5);
        if (!stepOK) { navRejects++; B.navRe = 0;      // THE SAME predicate the fan above scored with. A rejection can only mean the world moved under the creature between sense ticks, so it forces an immediate re-plan: the planner and the mover cannot disagree for longer than one frame.
          if (!navFitsAir(B.x, B.y, B.z)) { stepOK = true; navEgressN++; }   // ── EGRESS ── the creature is already OUTSIDE the travelable set (the world was edited under it, a tree landed on it, it streamed in beside rock). Refusing the step would pin it there until the mercy recycle; it walks out along the escape heading at its OWN speed instead. Displacement, never a teleport.
          else if (!(nvF[nvIdx(nx5, nz5)] & NVF_BUILT)) navRejUnb++;   // the destination cell is not vouched for, so navFitsAir fell back to a POINT probe there while the DDA sampled a DIFFERENT point of the same 2×2 — sub-cell disagreement that no lookahead measured in cells can see
          else if (navReachAir(B.x, B.y, B.z, B.th, mv5 + 0.05) > mv5) navRejSub++;   // the DDA calls the whole step clear and the endpoint probe refuses it: navFitsAir is cell-uniform only BELOW the NV_CCAP band ceiling, above which it point-probes too
          else navRejGeom++; } }   // the lane really did end inside this step — the cap was off, or the field changed under the creature between the DDA and the move
      else stepOK = !bfObst(nx5, B.y, nz5) && !bfObst(nx5, B.y + 2, nz5);
      if (((((B.kind | 0) === 0) && !NAVARB) || ((B.kind | 0) === 2 && !wormArb) || (B.kind | 0) === 6) && B.hcx !== undefined && B.jumpV === undefined) {   // LEASH: a worm/fish drifts around its home rather than away from it (never mid-LEAP — bending the heading in the air would curve the arc off its validated splash-down). The BUTTERFLY is off this path when the arbiter is live: its leash is a scored term up in navSteerAir, not a direct write to B.th behind the planner's back.
        const lx = B.x - B.hx, lz = B.z - B.hz; let l2 = lx * lx + lz * lz;
        const leash = (B.kind | 0) === 2 ? WORM_LEASH : ((B.kind | 0) === 6 ? 52 : FLY_LEASH);   // a fish patrols ~5 m around its pool spot — enough for river runs, never leaves its water body
        if ((B.kind | 0) === 2 && (B.trap || 0) > 0.35) l2 = 0;   // STUCK: stop pulling toward home so the normal avoidance can steer it around the trunk instead of into it
        if (l2 > leash * leash) {              // ease the heading back toward home — a hard clamp would make it skid along an invisible wall
          let want = Math.atan2(lx, lz) + Math.PI, d7 = want - B.th;
          while (d7 > Math.PI) d7 -= 6.283; while (d7 < -Math.PI) d7 += 6.283;
          B.th += d7 * Math.min(1, dt * 1.6);
          Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
        }
      }
      // ── NEITHER BIOME'S LIFE WALKS INTO THE OTHER'S (user 2026-08-16) ── the SPAWN gate further up already
      // admits desert species only where desertM > 0.85 and everything else only outside it, but that is a
      // one-time test: nothing stopped a bunny strolling east into the sand or a scorpion into the pines.
      // This refuses the STEP itself, at the midline rather than at the spawn threshold, so a creature turns
      // back at the boundary instead of being trapped inside the narrower band it was born in. Only walkers
      // are gated (kind 2): fish and ducks live in water that crosses the border, and flyers are not the
      // complaint. A creature that somehow starts on the wrong side is NOT frozen — the test only refuses a
      // step that would move it further in, so it can always walk home.
      if (stepOK && (B.kind | 0) === 2) {
        // 0.85, the SAME threshold the spawn gate admits on — not 0.5. Measured at 0.5, forest walkers showed
        // up on the "desert" side in 15% of samples, and they were not trespassing: forest life legally spawns
        // anywhere up to dm 0.85, so a midline test called a large legal band wrong and the creatures were
        // simply standing where they were born. Matching the spawn threshold makes the two agree.
        const dmN = desertM(nx5, nz5) > 0.85;
        if (dmN !== !!desSlot && dmN !== (desertM(B.x, B.z) > 0.85)) stepOK = false;
      }
      if (stepOK) { B.x = nx5; B.z = nz5;
        if (wBrk0) { B.trap = (B.trap || 0) + dt; B.noMove = (B.noMove || 0) + dt; }   // ACCEPTED, but the brake made it a zero-length step: count it exactly as the refused branch below does, or a stall against a cactus reports as a clean walk forever
        else { B.trap = Math.max(0, (B.trap || 0) - dt * 3); B.stuck = 0; B.noMove = 0; } }
      // ── THE ANT COLUMN ── a breadcrumb snake. The LEADER walks normally and drops a crumb every ANT_CRUMB
      // voxels; each follower is then placed at a fixed ARC LENGTH back along that recorded path, facing along
      // it. Following the leader's PATH rather than its current heading is the whole fix — the path is what a
      // real column walks, and it cannot oscillate because it is history, not a moving target.
      // Placed kinematically (B.x/B.z/B.th written directly) rather than steered, which also puts the ants out
      // of the nav arbiter's reach — the arbiter used to rewrite their heading 12 times a second.
      // ── A DEAD ANT STOPS BEING DRIVEN (user 2026-08-16: killing one leaves it "static into the terrain") ──
      // the ant is the ONE creature placed kinematically: the leader writes B.x/B.z/B.th directly and each
      // follower is set on the leader's breadcrumb path every frame, bypassing the mover entirely. Every other
      // species dies by becoming a rigid body that falls, but this block kept writing the corpse's position
      // back on top of the ragdoll, pinning it where it stood. Excluding a dying or slain body hands it to the
      // same death path the rest of the life uses. A dead LEADER also stops laying crumbs, so the followers
      // fall back to the head of the line rather than trailing a body that no longer moves.
      if (desSlot && (B.kind | 0) === 2 && !B.dying && !B.slain && DESERTS[desSp] && DESERTS[desSp].name === 'ant') {
        const aIdx = (wk - MAM_END) % DES_PER;
        if (aIdx === 0) {
          if (!B.trail) B.trail = [{ x: B.x, z: B.z, th: B.th }];
          const t0 = B.trail[0];
          const md = Math.hypot(B.x - t0.x, B.z - t0.z);
          if (md >= ANT_CRUMB) {
            B.trail.unshift({ x: B.x, z: B.z, th: B.th });   // the heading rides ALONG WITH the crumb: it is the cardinal the leader walked to reach this point, so a follower can wear it verbatim instead of re-deriving it from the chord
            // enough crumbs to cover the whole column with room to spare, and no more: an unbounded trail on a
            // creature that lives for minutes is a slow leak.
            const need = Math.ceil((DES_PER * ANT_GAP) / ANT_CRUMB) + 4;
            if (B.trail.length > need) B.trail.length = need;
          }
        } else {
          const lead = wbf[wk - aIdx];
          if (lead && lead.init && lead.trail && lead.trail.length > 1) {
            // ── THE PATH STARTS AT THE LEADER'S LIVE POSITION, NOT AT ITS LAST CRUMB ── this walked from
            // tr[0], and a crumb is only pushed once the leader has moved a whole ANT_CRUMB (0.75) from the
            // previous one. So the head of the measured path JUMPED forward 0.75 at a time while the leader
            // itself glided, and every follower inherited that stutter: the leader read as moving smoothly
            // off-grid and the line behind it as stepping from cell to cell (user 2026-08-16). Prepending the
            // leader's live x/z makes the arc length continuous, and costs one array entry per frame.
            let want = aIdx * ANT_GAP, k = 0, acc = 0;
            const tr = [{ x: lead.x, z: lead.z, th: lead.th }, ...lead.trail];
            while (k < tr.length - 1) {
              const seg = Math.hypot(tr[k + 1].x - tr[k].x, tr[k + 1].z - tr[k].z);
              if (acc + seg >= want) break;
              acc += seg; k++;
            }
            if (k < tr.length - 1) {
              const seg = Math.max(1e-4, Math.hypot(tr[k + 1].x - tr[k].x, tr[k + 1].z - tr[k].z));
              const f = Math.max(0, Math.min(1, (want - acc) / seg));
              B.x = tr[k].x + (tr[k + 1].x - tr[k].x) * f;
              B.z = tr[k].z + (tr[k + 1].z - tr[k].z) * f;
              B.th = tr[k].th !== undefined ? tr[k].th : Math.atan2(tr[k].x - tr[k + 1].x, tr[k].z - tr[k + 1].z);   // face the way the leader WENT — the crumb's OWN recorded heading, not the chord between two crumbs. They agree to the last bit on a straight run, but the chord across a corner crumb is a diagonal, and reading it would have been the one place a follower ever pointed off the compass.
              B.trap = 0; B.stuck = 0; B.noMove = 0;   // it is riding a path the leader already proved walkable
            }
          }
        }
      }
      if (!stepOK) {                                 // ── THE STEP WAS REFUSED ── written as its own test, NOT as an `else`: the ant-column block above was inserted between `if (stepOK)` and this branch, which silently re-bound the `else` to the ANT test — so every non-ant creature in the pool ran the blocked path on every frame, escape probes and all, however well its step had gone.
        B.trap = (B.trap || 0) + dt;                   // blocked (cornered) — count it
        if (antLead) B.antBlk = 1;                     // …and the ant's compass reads this next frame as "turn": its lookahead and the mover's gate are close but not the same predicate, so this is what guarantees a leader can never grind at a wall the lookahead called clear
        // ── HARD ANTI-STALL (user: "fish are still getting stuck on rocks and terrain") ── `trap` is NOT a reliable
        // stuck signal for a fish: the escape below clears it whenever it finds *somewhere* to nudge toward, so a fish
        // wedged where its 10-voxel body fits NO heading jiggles on the spot with trap pinned at 0 and never reaches
        // the 12 s mercy-recycle. Measured: one fish stalled 55 s of a 90 s soak. noMove counts REAL rejected motion
        // and nothing resets it except an accepted move, so it always fires.
        B.noMove = (B.noMove || 0) + dt;
        if (B.kind === 6) {                            // ── FISH ESCAPE ── body-aware, and it can go OVER an obstruction as well as around it (real fish rise over a boulder rather than grind its face).
          let bestA = -1, bestScore = -1e9;
          for (let k9 = 0; k9 < 16; k9++) { const th9 = k9 * 0.3927;
            if (!fishFits(B, th9, 3.5)) continue;      // the first real step must fit the WHOLE BODY — the old centre-column probe passed gaps the fish could never enter, which is how it stayed pinned
            let reach = 3.5;
            for (let d9 = 6; d9 <= 20; d9 += 3) { if (fishFits(B, th9, d9)) reach = d9; else break; }
            const turn = Math.abs(Math.atan2(Math.sin(th9 - B.th), Math.cos(th9 - B.th)));
            const sc = reach - turn * 1.6;             // widest water wins, with a mild preference for not spinning right around
            if (sc > bestScore) { bestScore = sc; bestA = th9; } }
          if (bestA >= 0) {                            // peel off toward that water at a real pace and EASE the heading over (a hard snap read as a glitchy instant spin)
            const mv = Math.max(spd5, 11) * dt;
            B.x += Math.sin(bestA) * mv; B.z += Math.cos(bestA) * mv;
            const dthr = Math.atan2(Math.sin(bestA - B.th), Math.cos(bestA - B.th));
            B.omT = Math.max(-4.5, Math.min(4.5, dthr * 5.5)); B.trap = 0; B.stuck = 0;
          } else {                                     // NOTHING fits at this depth → CLIMB toward the surface (dT drives the depth target) and come about; next frame re-probes at the new height
            B.dT = Math.min(1, (B.dT !== undefined ? B.dT : 0.5) + 1.8 * dt);
            B.dRe = tb3 + 1.5;                         // hold the raised target briefly so the depth wander can't immediately undo the climb
            B.omT = (B.om >= 0 ? 3.6 : -3.6);
          }
        } else if (B.kind >= 3) {                      // WATER creature blocked at a bank/rock (user: 'still getting stuck against shore/rocks'): don't just TURN — TURNING alone left it grazing corners.
          let bestA = -1, bestReach = -1;              // Actively STEP toward the clearest open water EVERY blocked frame so it physically peels off the bank and can never pin.
          const ay9 = Math.floor(B.y), fishClear = (px9, pz9) => B.kind !== 6 || (WL - bfBed(px9, pz9) >= 3 && !solid(Math.floor(px9), ay9 - 1, Math.floor(pz9)) && !solid(Math.floor(px9), ay9, Math.floor(pz9)) && !solid(Math.floor(px9), ay9 + 1, Math.floor(pz9)) && !solid(Math.floor(px9), ay9 + 2, Math.floor(pz9)));   // for a fish, "clear" also means deep enough AND no rock across the body at the swim depth
          for (let k9 = 0; k9 < 16; k9++) { const th9 = k9 * 0.3927;
            const sx9 = B.x + Math.sin(th9) * 2.5, sz9 = B.z + Math.cos(th9) * 2.5;   // probe a touch further so the fish peels FULLY off the rock, not just onto its edge
            if (!duckFit(sx9, sz9) || !fishClear(sx9, sz9)) continue;   // this heading's immediate step must land on water the MOVER would accept — the escape used to probe a looser rule than stepOK, so it could peel a duck toward a spot the very next step refused
            let reach = 2.5;
            for (let d9 = 4; d9 <= 16; d9 += 2.0) { const rx9 = B.x + Math.sin(th9) * d9, rz9 = B.z + Math.cos(th9) * d9; if (bfWater(rx9, rz9) && fishClear(rx9, rz9)) reach = d9; else break; }   // the RANKING term stays on the bare water probe: the first-step GATE above it is the shared answer, this only orders the candidates, and leaving it alone keeps ?noarb bit-exact against the pre-arbiter build so the control is provably the same code
            if (reach > bestReach) { bestReach = reach; bestA = th9; }   // widest contiguous open water wins → steer toward the OPEN lake, away from the bank/boulder
          }
          if (bestReach > 0) {                         // MOVE toward that water at normal paddle speed + face it + clear trap → never pins, never mercy-recycles
            const mv = spd5 * dt;
            B.x += Math.sin(bestA) * mv; B.z += Math.cos(bestA) * mv;   // physically peel off toward the open water
            const dthr = Math.atan2(Math.sin(bestA - B.th), Math.cos(bestA - B.th));   // EASE the heading toward the escape instead of a HARD snap (the snap read as a glitchy instant model spin — user: 'glitching out') — the om ease smooths it over a few frames
            B.omT = Math.max(-3.4, Math.min(3.4, dthr * 4.5)); B.trap = 0; B.stuck = 0;
          }
        }
      }
      const gLoc = bfSurf(B.x, B.z);                   // the actual ground right here
      const gAir = (NAVARB && B.kind < 2 && nvOn) ? Math.max(gLoc, nvTopAir(nvIdx(B.x, B.z))) : gLoc;   // ── FLYERS TAKE THEIR GROUND FROM THE FIELD ── bfSurf is ONE column of hmap clamped to sea level; the field's travel surface is the MAX over the 2×2 and counts decor and rock the heightmap never saw. Leaving the altitude servo on bfSurf floored butterflies BELOW the surface the predicate measures clearance from, so every frame the mover refused a move the planner had just approved — the exact planner/mover split the arbiter exists to close, reintroduced on the vertical axis.
      const yPrev5 = B.y;                            // total vertical motion this frame is budget-capped below — branches must never STACK into a visible jump
      let yBudD = 0;                                 // …and a SEATED creature may raise that budget to its own pace (see the desert servo below); 0 leaves the shared 30/34 exactly as it was
      if (B.kind === 2) {                              // WORM / BUNNY: rides the terrain SMOOTHLY — target blends the ground AHEAD (starts the ramp before a step) and eases at a gentle rate
        // ── ONE SEAT FOR ALL FOUR LAND MAMMALS (user 2026-08-07: "do they all behave the same way?") ── they did
        // not. The ground SOURCE was tied to arbiter membership: `mamArb ? navWalkStand : bfSurf`, and the bunny is
        // deliberately off the arbiter (a hop is one committed arc, see mamArb), so it alone read the raw heightmap
        // while the other three read the nav field. Measured: those two disagree at the same column on 20-34% of
        // frames, by up to 3 voxels — bfSurf is blind to the rock and decor stamped on top of it, which is precisely
        // what a body gets seated inside. Which planner an animal uses and which surface its feet rest on are
        // different questions; only the second one belongs here, and it is now the same answer for every mammal.
        const fitM = desSlot ? (DESERTS[desSp] ? MAMFIT[DESERTS[desSp].name] : null) : bunnySlot ? MAMFIT.bunny : (armSlot ? MAMFIT.arm : (skunkSlot ? MAMFIT.skunk : (porcSlot ? MAMFIT.porc : null)));   // …and the desert band, or its measured seat is dead data: without a branch here every one of them falls to the worm default (yoff = 2), which is 1.5 voxels of air under a 1-voxel ant and buries the lower half of a 9-voxel cobra
        const gW = wormArb ? gcW : (fitM ? navWalkStand(B.x, B.z) : gLoc);
        // ── THE FORWARD LOOK IS A SEAT, SO IT READS THE SEAT'S SURFACE ── it used to be keyed on arbiter
        // membership rather than on having a footprint at all, which sent the desert band (wormArb) to
        // navGroundAt: the RAW 2x2 cell top, with no clutter subtracted and none of navWalkStand's step-up
        // sanity. A saguaro three voxels ahead was therefore a legal answer to "how high is my floor", and the
        // creature rose 20 voxels up its side without ever stepping onto it. Anything with a MAMFIT now takes
        // this from the same plane mamSeatG does — and, in the desert, under the same sand rule.
        const gFwd = fitM ? mamStandAt(B.x + Hx2 * 3, B.z + Hz2 * 3, desSlot)
          : (wormArb ? navGroundAt(B.x + Hx2 * 3, B.z + Hz2 * 3) : bfSurf(B.x + Hx2 * 3, B.z + Hz2 * 3));
        const yoff = fitM ? fitM.seat : 2, yflr = fitM ? fitM.seat : 1.6;   // the lift is the MODEL's own half-height above its lowest occupied layer (see MAMFIT), so a new animal can never inherit another's by accident
        // ── A BODY IS NOT A COLUMN (user 2026-08-07: "it appears to clip through the terrain") ── mamSeatG scans
        // the model's own occupied footprint, oriented by heading, and reduces with MAX; the forward look folds
        // into the same max, which keeps the reason it existed (start the ramp before the step) without a blend
        // that lands between two ground heights. The WORM keeps its 2:1 blend below: it is 3 voxels long, so its
        // body really is a column.
        let gBody = gW;
        if (fitM) { gBody = mamSeatG(B, fitM, desSlot); if (gFwd > gBody) gBody = gFwd; }
        const gT7 = (fitM ? gBody : (gW * 2 + gFwd) / 3) + yoff;   // the WORM keeps its blend: it is 3 voxels long, so its body really is a column
        // ── THE SERVO IS PACED IN VOXELS TRAVELLED, NOT SECONDS (user 2026-08-07: "still not making contact when
        // going DOWN steeper terrain") ── 12 vox/s and tau = 1/7 s were authored against a 9 vox/s armadillo. The
        // skunk walks at 24 and flees at 48, so on anything steeper than 12/24 = 1:2 the ground drops away faster
        // than this line is ALLOWED to follow it, and the body floats off at (spd*slope - 12) vox/s for the whole
        // descent — 12 vox/s on a 1:1, more than its own 8-voxel height across a 20-voxel hillside — then takes as
        // long again to settle at the bottom. Uphill never showed it: yoff === yflr for a mammal, so gT7 IS the hard
        // floor one line below, every upward correction is done instantly by that clamp, and the Math.min branch
        // here is unreachable. Scaling both terms by the animal's own pace makes the smoothing a constant ~1.3
        // voxels of TRAVEL for every mammal instead of 3.4 for a walking skunk and 6.9 for a fleeing one.
        // aspd is 9 for the armadillo/porcupine and undefined for the bunny and the worm, so kV7 is exactly 1.0
        // there and every product is bit-identical: x * 1.0 === x, and (7 * 1.0) * dt is the same double as 7 * dt.
        // The DESERT band never had a pace to scale by: B.aspd is written only by the land mammals' own marcher, so
        // every one of the seven fell to the 9 vox/s default and rode the armadillo's servo at 32 vox/s (64 when a
        // gecko bolts inside DES_DASH_R). That is the SAME defect this line was written to fix for the skunk, and it
        // is why the float got worse the faster the animal moved. Their pace is spd5, the shared glide they are
        // actually advanced by. Bit-identical everywhere else: a mammal short-circuits on B.aspd, and the bunny and
        // the worm are not desSlot, so `0 || 9` is the same 9 the old expression produced.
        const kV7 = fitM ? Math.max(1, (B.aspd || (desSlot ? spd5 : 0) || 9) / 9) : 1;
        // ── AND THE GLOBAL BUDGET IS PART OF THE SERVO ── the 30/34 vox/s clamp below is applied to every creature
        // AFTER these branches, so raising the servo's own rate alone changed almost nothing on a steep descent: a
        // gecko bolting at 64 vox/s down a 1:2 slope needs 32 vox/s of drop and the clamp only allowed 30, which is
        // where the last of the float lived (measured: 10 voxels of air, held for ~0.5 s at a time). The CLIMB side is
        // the same defect pointing the other way — it undoes the hard body floor one line below, which is why the
        // dashing gecko was also SINKING into steep ascents. The raised budget is 1.33x the animal's own travel speed,
        // so the vertical rate can never outrun the horizontal one and a teleport stays unreachable. Scoped to the
        // desert band: the fleeing skunk at 48 vox/s clips this same ceiling more mildly, and it is a shipped,
        // signed-off animal — leaving yBudD at 0 keeps every other creature's arithmetic character for character.
        if (desSlot) yBudD = 12 * kV7;
        B.y += Math.max(-12 * kV7 * dt, Math.min(12 * kV7 * dt, (gT7 - B.y) * (1 - Math.exp(-7 * kV7 * dt)) + Math.sign(gT7 - B.y) * 2 * kV7 * dt));
        if (B.y < gBody + yflr) B.y = gBody + yflr;    // hard body floor — the ease could lag uphill and sink it INTO the slope ('clips through the ground')
        if (bfObst(B.x, B.y, B.z)) B.y = gBody + yoff; // body ended up inside something (streamed terrain/step edge) — re-seat on the local surface instead of hiding+vanishing
      } else if (B.kind === 6) {                       // FISH: hold a WANDERING depth between the bed and the underside of the surface; LEAPS are real ballistics
        const FC9 = FISH_CFG, bed9 = bfBed(B.x, B.z);
        const lo9 = bed9 + 1.6, hi9 = Math.max(WL - 1.4, lo9 + 0.2);
        if (B.jumpV !== undefined) {                   // ── AIRBORNE ── pure ballistics; the depth ease/clamp is bypassed and the global vertical budget exempts the arc
          B.y += B.jumpV * dt; B.jumpV -= FC9.jump.gravity * dt;
          if (B.jumpV < 0 && B.y <= hi9) {             // ── REENTRY ── falling back through the surface → swimming again, cleanly: hold-depth resumes, the terrain
            B.y = hi9; B.jumpV = undefined;            // hitbox below re-validates the whole body, and the per-species cooldown keeps leaps occasional
            if (B.jumpSpl) spawnSplash(B.x, B.z, 1.15); if (B.jumpSpl) PH.stats.leapDown = (PH.stats.leapDown|0)+1;   // …and it goes back in with a splash (user) — a shade bigger than the launch, it is coming down with the arc behind it. Whatever the LAUNCH decided: a leap must not splash at one end only.
            B.jumpRe = tb3 + (FC9.jump.cooldownMin + Math.random() * (FC9.jump.cooldownMax - FC9.jump.cooldownMin)) / Math.max(0.05, B.jumpMul || 1);
            B.spdT = FC9.baseSpeed; B.dT = 0.35 + Math.random() * 0.3;   // glide off the splash back into a mid-depth cruise
          }
        } else {
          if ((snowOn || freezeK >= 0.02) && B.jumpArm !== undefined) { B.jumpArm = undefined; B.jumpRe = tb3 + 5; }   // snow began mid-run-up → abandon the leap (a fish must never breach a snowing/frozen lake — user)
          const tgt9 = lo9 + (hi9 - lo9) * (B.dT !== undefined ? B.dT : 0.5);
          if (!iceLock) B.y += Math.max(-7 * dt, Math.min(7 * dt, (tgt9 - B.y) * (1 - Math.exp(-2.2 * dt))));   // FROZEN fish holds its depth — no wander (user)
          B.y = Math.max(lo9, Math.min(hi9, B.y));     // hard clamp — the body NEVER breaks the surface and never grinds the bed
          // ── LEAP, TWO-PHASE ── ARM a run-up to the surface first (waiting for the depth wander to leave the fish
          // shallow practically never fired), then BREACH only after the WHOLE ARC validates. Every dial lives in
          // FISH_CFG.jump: the cooldown window ÷ species multiplier = frequency, vMin/vMax = height, forward = distance.
          if ((B.jumpMul || 0) > 0 && !snowOn && freezeK < 0.02 && B.jumpArm === undefined && tb3 > (B.jumpRe || 0) && hi9 - lo9 > FC9.jump.minDepth
              && bfSky(B.x, WL + 2, B.z) && fishFits(B, B.th, 10)) {
            B.jumpArm = tb3 + FC9.jump.armS;           // ARM: climb for up to armS seconds…
            B.dT = 1; B.dRe = tb3 + FC9.jump.armS + 1; // …holding the shallow depth target so the wander can't pull it back down mid-ascent
          }
          if (B.jumpArm !== undefined) {
            if (B.y > hi9 - 1.6 && bfSky(B.x, WL + 2, B.z)) {   // reached the surface — now prove the ARC itself before committing:
              const jv = FC9.jump.vMin + Math.random() * (FC9.jump.vMax - FC9.jump.vMin);
              const hang = 2 * jv / FC9.jump.gravity;  // total air time → splash-down = launch + heading · forward · hang
              const lx9 = B.x + Math.sin(B.th) * FC9.jump.forward * hang, lz9 = B.z + Math.cos(B.th) * FC9.jump.forward * hang;
              const mx9 = (B.x + lx9) * 0.5, mz9 = (B.z + lz9) * 0.5;
              if (bfWater(lx9, lz9) && WL - bfBed(lx9, lz9) >= 3 && bfSky(lx9, WL + 2, lz9) &&   // splash-down = REAL deep water under open sky — never land, never a shelf…
                  bfWater(mx9, mz9) && bfSky(mx9, WL + 2, mz9) && fishFits(B, B.th, 8)) {        // …and the mid-arc + immediate exit are clear too
                B.jumpV = jv; B.spd = FC9.jump.forward; B.spdT = FC9.jump.forward;   // launch, carrying the configured forward speed through the whole arc
                B.om = 0; B.omT = 0; B.jumpArm = undefined;                          // dead-straight in the air — the validated splash-down must stay where it was aimed
                B.jumpSpl = lifeIsDrawn(wk);       // decided HERE, for the whole arc — see lifeIsDrawn
                if (B.jumpSpl) spawnSplash(B.x, B.z, 1); if (B.jumpSpl) PH.stats.leapUp = (PH.stats.leapUp|0)+1;   // …and it breaks the surface with a splash (user), if it is near enough to be seen doing it
              } else { B.jumpArm = undefined; B.jumpRe = tb3 + 4 + Math.random() * 6; B.dT = 0.3 + Math.random() * 0.4; }   // the arc lands badly (bank/shallow/roof) — stand down, sink back, try elsewhere
            } else if (tb3 > B.jumpArm) { B.jumpArm = undefined; B.jumpRe = tb3 + 6 + Math.random() * 12; }   // never reached the surface (roofed/penned in) — stand down
          }
        }
        if (NAVARB && B.jumpV === undefined && Math.floor(B.y) !== Math.floor(yPrev5)
            && !fishBodyAt(B.x, B.z, B.th, Math.floor(B.y), B.fhalf) && fishBodyAt(B.x, B.z, B.th, Math.floor(yPrev5), B.fhalf)) { B.y = yPrev5; navVetK[6]++; }   // ── VERTICAL, SAME PREDICATE ── stepOK validates the body at the PRE-step y; the depth ease then moved y with no body test at all, so a move approved at the old height could clip at the new one. The hitbox below reverted the pose and the fish "moved" every frame while sitting still (measured: 60 s at spd 8 with noMove reading 0). Vetoing the y step makes that oscillation unreachable instead of catching it afterwards. A LEAP is exempt — its arc is real ballistics.
        B.vyS = (B.vyS || 0) + ((B.y - yPrev5) / Math.max(dt, 1e-4) - (B.vyS || 0)) * (1 - Math.exp(-6 * dt));   // low-passed climb/dive — drives the SUBTLE nose pitch in the render frame below
      } else if (B.kind >= 3) {                        // DUCK / LILY: ride the CONTINUOUS swell (JS mirror of the TRACE wave field) — smooth by construction, no quantized ratcheting
        let wvMax = 0;
        if (freezeK < 0.4) {                           // waves are gated exactly like the renderer — a frozen lake is flat
          const wvAt = (wx, wz) => gerstHJS(Math.floor(wx), Math.floor(wz), tb3);   // the GERSTNER mirror — bit-matched to the shader's gerstH, so floats ride EXACTLY the drawn surface
                                                       // MAX over the body FOOTPRINT — the waves are per-column, and a crest under an edge column must lift the whole float
          const fr9 = B.kind === 4 ? 4 : 2.5;          // lily pads are WIDE and 1 voxel thin — the footprint must reach the pad edge or an edge-column crest swallows the whole pad
          wvMax = Math.max(wvAt(B.x, B.z), wvAt(B.x + fr9, B.z), wvAt(B.x - fr9, B.z), wvAt(B.x, B.z + fr9), wvAt(B.x, B.z - fr9));
          if (B.kind === 4) { const fd9 = 2.8; wvMax = Math.max(wvMax, wvAt(B.x + fd9, B.z + fd9), wvAt(B.x - fd9, B.z + fd9), wvAt(B.x + fd9, B.z - fd9), wvAt(B.x - fd9, B.z - fd9)); }
          wvMax += 0.5;                                // +0.5 = the renderer's floor(wv + 0.5) worst case — the float stays AT or ABOVE the drawn surface at all times
        }
        const base9 = B.kind === 4 ? WL + 1.6 + 0.5 * (LILY_SZ[B.col % Math.max(1, LILY_SZ.length)] || 1)   // lily rests ON the surface with clearance — a 1-voxel-thin pad has NO sacrificial bottom row
          : (isBaby ? WL + 1.9 : WL + 3.4);            // ducks: ONE VOXEL LOWER (user 2026-08-05) — they sat too high on the surface; the bottom row now dips properly   // ducks: ride high — only (part of) the BOTTOM voxel row ever dips
        // ── HALF THE SWAY (user 2026-08-05: "reduce the ducks up and down sway by 50%") ── damping wvMax
        // itself would drop the duck at every crest, because that number carries the renderer's +0.5 floor
        // that keeps the body AT or ABOVE the drawn surface. So the ride is damped about its OWN slow mean
        // instead: the average waterline is exactly what it was, only the excursion either side of it halves.
        // The mean is per-float and self-correcting, so it follows the duck across water of any roughness.
        B.wvM = B.wvM === undefined ? wvMax : B.wvM + (wvMax - B.wvM) * (1 - Math.exp(-0.08 * dt));  // ~12 s low-pass. It has to be well SLOWER than the swell itself (GWOM puts the four waves at 3-4 s periods) or the mean tracks the very rise and fall it exists to average out — at 2 s the damping came out 43%%, not 50%%
        const wvUse = B.kind === 3 ? B.wvM + (wvMax - B.wvM) * DUCK_SWAY : wvMax;   // lilies keep the full ride — a pad sits flat ON the surface and reads wrong if it lags the swell
        const tgt9 = base9 + wvUse;
        const ek9 = B.kind === 4 ? 14 : 5, ec9 = B.kind === 4 ? 10 : 7;   // lily tracks TIGHT (lag ~0.2 vox — it has no draft to hide lag in); ducks keep the lazy bob-along ease
        B.y += Math.max(-ec9 * dt, Math.min(ec9 * dt, (tgt9 - B.y) * (1 - Math.exp(-ek9 * dt))));   // ONE symmetric ease — the swell moves ≤ ~2.5 vox/s, so this tracks it closely and SMOOTHLY
      } else if (B.trap > 0.5 && !NAVARB) {            // STUCK: dive for the open understory when there's air below, else flutter up if the sky above is clear. RETIRED under the arbiter: it is a third writer of y that answers to a counter rather than to the field, and it parks a roofed flyer at ground+8 UNDER the canopy it is trying to leave.
        if (!bfObst(B.x, B.y - 2, B.z) && B.y - 2 > gLoc + 6) B.y -= 34 * dt;
        else if (!bfRoofed(B.x, B.y, B.z)) B.y += 40 * dt;
      } else {
        B.gRef = Math.max(gAir, (B.gRef || gAir) - 9 * dt);   // GROUND MEMORY: rises instantly with terrain, sinks slowly — stays HIGH crossing gorges (no diving in) but settles to the
        const cruise = B.gRef + (B.kind ? 8 : 12);            // local ground on flats/slopes, so the cruise line sits in the open UNDERSTORY below the canopy ('caught on trees' fix —
        let stepY = (cruise - B.y) * (1 - Math.exp(-4 * dt)); // the old terrain-max stencil pinned cruise AT canopy height on any slope)
        stepY = Math.max(-26 * dt, Math.min(30 * dt, stepY)); // RATE-CAPPED — a reference jump must never become a visible teleport
        if (!(stepY > 0 && (bfObst(B.x, B.y + 3, B.z) || bfObst(B.x, B.y + 6, B.z)))) B.y += stepY;   // never ease UP into foliage overhead (two probes — gappy pine crowns fooled one)
        if (B.y < B.gRef + 7 + (desFly ? DES_FLY_UP : 0)) B.y = Math.min(B.gRef + 7 + (desFly ? DES_FLY_UP : 0), B.y + 34 * dt);   // floors are APPROACHED at climb speed, never snapped
      }
      if (B.kind < 2 && B.y < gAir + 6 + (desFly ? DES_FLY_UP : 0)) B.y = Math.min(gAir + 6 + (desFly ? DES_FLY_UP : 0), B.y + 34 * dt);   // the fly's floor rides DES_FLY_UP above every other flyer's   // absolute local-ground floor (FLYERS only — a worm lives at ground+2, a duck at the waterline); gAir so the floor and the feasibility predicate agree on where the ground is
      if (NAVARB && B.kind < 2 && B.y !== yPrev5 && !navFitsAir(B.x, B.y, B.z) && navFitsAir(B.x, yPrev5, B.z)) { B.y = yPrev5; navVetoY++; }   // ── VERTICAL, SAME PREDICATE ── the altitude servo is the flyer's other motion axis and it used to write y with no feasibility test at all. It is not a teleport, so it is not rewritten here; it is VETOED by navFitsAir, so both axes now answer to one predicate.
      if (B.jumpV === undefined) { const yDn9 = yBudD > 30 ? yBudD : 30, yUp9 = yBudD > 34 ? yBudD : 34;
        B.y = Math.max(yPrev5 - yDn9 * dt, Math.min(yPrev5 + yUp9 * dt, B.y)); }   // GLOBAL vertical budget: whatever the branches above did, the frame's total climb/descent stays at flutter speed — teleports impossible. A LEAPING salmon is exempt: its arc is real ballistics and this cap would flatten the rise and make the fall float.
      if (B.kind === 6) {                              // ── FISH TERRAIN HITBOX ── a hard guarantee: never render with the body in terrain. If any part of the long body overlaps solid, RESOLVE it — push the centre out to the nearest clear spot (works WITH the repulsion, not against it); only if there's no clear spot within reach does it fall back to the last clear pose. This is the "hitbox for terrain, not for the player" the user asked for.
        const bodyClear6 = (cx6, cz6) => fishBodyAt(cx6, cz6, B.th, Math.floor(B.y), B.fhalf);   // same single body test as the planner and stepOK
        if (bodyClear6(B.x, B.z)) { if (B.jumpV === undefined) { B.cx = B.x; B.cy = B.y; B.cz = B.z; B.cth = B.th; } }   // clear → remember this pose AND ITS HEADING (never an AIRBORNE one: a later revert would teleport the fish back into the sky)
        else {                                                             // clipping → move the centre to the NEAREST genuinely clear spot
          let fixed = false;                                               // Expanding-ring search on the real body test. The old version derived a push NORMAL from 8 probes at r=4 and one
          for (let s6 = 1; s6 <= 10 && !fixed; s6++) {                     // height, so anything those probes missed (a nose buried 5+ vox out, rock above/below that height) produced no
            for (let k6 = 0; k6 < 16; k6++) { const a6 = k6 * 0.3927;      // push at all and it fell straight through to the revert.
              const qx = B.x + Math.sin(a6) * s6, qz = B.z + Math.cos(a6) * s6;
              if (fishWaterOK(qx, qz, Math.floor(B.y)) && bodyClear6(qx, qz)) {   // the SAME water answer stepOK uses — a ring that offered a spot the mover would refuse just re-clipped the fish on the next frame
                B.x = qx; B.z = qz; B.cx = qx; B.cy = B.y; B.cz = qz; B.cth = B.th; fixed = true; break; } } }
          if (!fixed) for (let u6 = 1; u6 <= 6 && !fixed; u6++) {          // pinned horizontally → RISE out of it (a fish wedged beside a boulder has open water straight up)
            const qy = Math.min(WL - 1.4, B.y + u6);
            if (fishBodyAt(B.x, B.z, B.th, Math.floor(qy), B.fhalf)) { B.y = qy; B.cx = B.x; B.cy = qy; B.cz = B.z; B.cth = B.th; fixed = true; } }
          if (!fixed && B.cx !== undefined) { B.x = B.cx; B.y = B.cy; B.z = B.cz; if (B.cth !== undefined) B.th = B.cth;   // last-resort revert — RESTORE THE HEADING TOO: the body test depends on it, so a pose remembered at
            B.trap = (B.trap || 0) + dt;                                   // one heading can clip at another, and reverting position alone left a fish embedded for ~10 s at a time (measured).
            B.noMove = (B.noMove || 0) + dt;           // …and this counts as NOT MOVING. stepOK validates the body at the PRE-step y, then the depth ease shifts y and this
          }                                            // resolve rejects at the new y and restores the same pose — the move "succeeded" every frame while the fish sat
        }                                              // frozen (measured: 60 s at spd 8 with noMove stuck at 0). Both stall routes must feed the same counter.
      }
      // ── STALL WATCHDOG ── purely OBSERVATIONAL, and it is what finally kills the "stuck on rocks" report. Counting
      // rejected moves was not enough: the post-move hitbox often "fixes" a clipping fish by nudging it, the next frame
      // re-clips and nudges it back, and it oscillates on the spot with every internal counter reading healthy. So judge
      // the only thing that matters — did it actually TRAVEL — and require the terrain backstop to be engaged so a fish
      // merely circling in open water is never touched.
      let stallHit = false;
      if (B.kind === 6 && B.jumpV === undefined) {
        if (B.stallT === undefined) { B.stallT = tb3; B.stallX = B.x; B.stallZ = B.z; }
        else if (tb3 - B.stallT > 1.8) {
          if (Math.hypot(B.x - B.stallX, B.z - B.stallZ) < 7 && B.bkOn) stallHit = true;   // < 3.9 vox/s of real progress while grinding terrain
          B.stallT = tb3; B.stallX = B.x; B.stallZ = B.z;
        }
      }
      if (B.kind === 6 && ((B.noMove || 0) > 3 || stallHit) && B.jumpV === undefined) {
        // Sweep outward for somewhere the body genuinely fits and relocate; if the whole neighbourhood
        // is unusable, drop the slot so the spawner re-places it in open water. Either way no fish parks in the scenery.
        let done = false;
        for (let rr = 6; rr <= 40 && !done; rr += 4) {
          for (let k8 = 0; k8 < 12; k8++) { const a8 = k8 * 0.5236 + B.th;
            const qx = B.x + Math.sin(a8) * rr, qz = B.z + Math.cos(a8) * rr;
            if (!bfWater(qx, qz) || WL - bfBed(qx, qz) < 5) continue;
            const qy = Math.max(bfBed(qx, qz) + 2.2, Math.min(WL - 2, (bfBed(qx, qz) + WL) * 0.5));   // re-seat at MID depth — the stalls all began in shallow shelf water
            if (!fishBodyAt(qx, qz, a8, Math.floor(qy), B.fhalf)) continue;
            B.x = qx; B.z = qz; B.y = qy; B.th = a8; B.om = 0; B.omT = 0; B.dT = 0.5;
            B.cx = qx; B.cy = qy; B.cz = qz; B.cth = a8; B.hx = qx; B.hz = qz;   // re-home too, or the leash drags it straight back into the pocket
            B.noMove = 0; B.trap = 0; B.bkOn = false; B.senseRe = 0;
            B.stallT = tb3; B.stallX = qx; B.stallZ = qz; done = true; break; }
        }
        if (!done) { B.init = false; B.noMove = 0; B.stallT = undefined; }   // nowhere within 4 m works — recycle rather than leave a fish parked in the scenery
      }
      if (B.kind < 2 && (bfObst(B.x, B.y, B.z) || bfRoofed(B.x, B.y, B.z))) {   // under a roof (cave/overhang) — steer OUT horizontally (eased, never climb into it); count toward a recycle. Worms/ducks don't care about roofs.
        if (!NAVARB) B.omT = (B.om >= 0 ? 5.0 : -5.0);   // …but NOT while the arbiter is live: an unconditional omT write here is exactly the "fifth uncoordinated controller" the arbiter exists to remove, and roofedness is already one of its scored terms (navRoofAir, asked of the same predicate)
        B.trap = (B.trap || 0) + dt;
      }
      if (B.kind < 3 && bfObst(B.x, B.y, B.z)) continue;   // body-in-solid hide (skip emission) — LAND/AIR creatures only: a duck/lily bobbing into the water voxel layer is normal
      const bobA = iceLock ? 0 : (B.kind === 2 || B.kind === 6 ? 0 : (B.kind === 4 ? 0.12 : (B.kind === 3 ? (isBaby ? 0.18 : 0.25) : (B.kind === 1 ? 1.1 : 2.2))));   // frozen duck → dead still (no bob); else worms/fish don't bob, water floats keep the cosmetic bob SMALL — the wave riding is the real motion, a deep bob dip re-sank the bottom rows
      const bobF = B.kind === 4 ? 1.1 : (B.kind === 3 ? (isBaby ? 2.3 : 1.6) : (B.kind === 1 ? 2.6 : 6.8));
      const hop3 = hurtHop(B);   // BOUNCE (user): a short arc up and back down over the flash window
      const px3 = B.x + (armSlot ? (B.aRoX || 0) : 0), pz3 = B.z + (armSlot ? (B.aRoZ || 0) : 0), py3 = hop3 + B.y + (bunnySlot ? (B.bOy || 0) : (armSlot ? (B.aRoY || 0) : Math.sin(tb3 * bobF + wk * 1.9) * bobA));   // ARMADILLO: shift by its per-heading/per-frame alignment offset so it centres like the editor (user). BUNNY: the lift is the BAKED oy. Worm bob stays 0.
      if (B.kind >= 2 && B.kind !== 6 && freezeK < 0.4) {   // GROUND/WATER creatures cast a sun shadow (lily/duck/worm) — flyers (0/1) skip, FISH skip (submerged: the surface owns the light there); frozen lakes are flat & shadow-free like the water
        const hxz = B.kind === 4 ? 5.0 : (B.kind === 3 ? (isBaby ? 1.8 : 3.0) : 3.0);
        const hy = B.kind === 4 ? 1.3 : (B.kind === 3 ? (isBaby ? 1.5 : 2.5) : 1.2);   // lily box a touch taller so the pad's shadow projects clear of the pad (not tucked underneath)
        cshadList.push([px3 - winOX, py3, pz3 - winOZ, hxz, hy, dp2]);
      }
      const HxR = Math.sin(B.th), HzR = Math.cos(B.th);   // RENDER yaw — for lilies this spins freely, independent of drift
      const mamMir = (bunnySlot || armSlot || skunkSlot || porcSlot) ? -1 : 1;   // HANDEDNESS: the grid stamp maps model (x,y,z) to world (+x,+z,+y) - determinant -1, a REFLECTION - while the emit basis below is right-handed, so the two paths draw one .vox as mirror images. The four land mammals are grid-stamped, so the emit branch below never runs for them and has never been corrected; -1 is that correction, scoped to them so nothing already on the trace path moves.
      let Xw = [HzR * mamMir, 0, -HxR * mamMir], Yw = [-HxR, 0, -HzR], Zw = null;   // level frame — Zw = up, right-handed (Yw×Zw = Xw); model −y (the head end) leads
      if (B.kind === 6 && B.jumpV === undefined) {     // ── FISH ORIENTATION ── upright always: free yaw, a SUBTLE nose pitch from the low-passed climb/dive, and ROLL
        // IMPOSSIBLE BY CONSTRUCTION — the lateral axis Xw stays exactly horizontal and Zw completes the right-handed frame
        // from it, so no path through this code can bank or flip the body. AIRBORNE leaps stay dead level (user).
        const p9 = Math.max(-FISH_CFG.pitchMax, Math.min(FISH_CFG.pitchMax, Math.atan2((B.vyS || 0) * FISH_CFG.pitchGain, Math.max(6, B.spd || FISH_CFG.baseSpeed))));
        if (p9 * p9 > 1e-6) { const cp9 = Math.cos(p9), sp9 = Math.sin(p9);
          Yw = [-HxR * cp9, -sp9, -HzR * cp9]; Zw = [-HxR * sp9, cp9, -HzR * sp9]; }   // head dir = −Yw tips up when climbing; Zw = Xw×Yw stays roll-free
      }
      // ── CRYING (user 2026-08-05) ── an orphaned duckling weeps for 3 s, one tear at a time, alternating
      // eyes. Done HERE because this is where the model's world frame exists: an eye is a model-space cell,
      // and (px3,py3,pz3) + Xw/Yw/Zw + bScale is exactly the basis the emit hands the tracer, so a tear
      // leaves the eye the renderer actually draws — at any heading, and riding the same wave bob.
      if (isBaby && B.cryTo && DUCKB_EYES.length) {
        if (now > B.cryTo) B.cryTo = 0;
        else if (now >= B.cryNext) {
          B.cryNext = now + CRY_GAP;
          B.cryEye++;                                  // a free-running counter now, NOT wrapped to the eye list: baby.vox
          const e = DUCKB_EYES[B.cryEye % DUCKB_EYES.length];   // is one voxel wide at the head, so it has ONE black voxel and `% length` pinned this to 0 forever
          // …and the droplet wells out of the SIDE of that voxel, alternating cheeks — half a cell along the
          // model's own X puts it exactly on the eye's outer face, so it is visible the instant it is born
          // instead of spending its first frames buried inside the head.
          const ex = e[0] + ((B.cryEye & 1) ? 0.5 : -0.5), zW = Zw || [0, 1, 0];
          spawnTear(px3 + (Xw[0] * ex + Yw[0] * e[1] + zW[0] * e[2]) * bScale,
                    py3 + (Xw[1] * ex + Yw[1] * e[1] + zW[1] * e[2]) * bScale,
                    pz3 + (Xw[2] * ex + Yw[2] * e[1] + zW[2] * e[2]) * bScale);
        }
      }
      // ── A DASHING ANIMAL ANIMATES AS FAST AS IT MOVES (user 2026-08-16) ── the same DES_DASH factor and the
      // same DES_DASH_R that double the travel speed now double the frame rate, so legs keep pace with ground
      // covered instead of a sprinting gecko sliding on a walk cycle. It multiplies each species' OWN rate
      // rather than setting a flat 48: everything runs at 24 and doubles to 48, but the scorpion runs at 12
      // and doubles to 24, which keeps the slower gait the user asked for.
      const desRate = (desSlot && DESERTS[desSp])
        ? (DES_FPS[DESERTS[desSp].name] || 24) * ((DES_DASH[DESERTS[desSp].name] && ((P.x - B.x) * (P.x - B.x) + (P.z - B.z) * (P.z - B.z)) < DES_DASH_R * DES_DASH_R) ? DES_DASH[DESERTS[desSp].name] : 1)
        : 24;
      const nfr = desSlot ? (DESERTS[desSp] ? DESERTS[desSp].n : 1) : B.kind === 6 ? FISHES[B.fsp || 0].n : (B.kind >= 3 ? 1 : (B.kind === 2 ? (bunnySlot ? (B.bst ? BUNNY_NFRAMES : BUNNY_JUMP_NFRAMES) : (armSlot ? ARMADILLO_NFRAMES : (skunkSlot ? SKUNK_NFRAMES : (porcSlot ? PORCUPINE_NFRAMES : WORM_NFRAMES)))) : (B.kind === 1 ? FFLY_NFRAMES : (B.dfly ? DFLY_NFRAMES : BFLY_NFRAMES))));
      // ── THE SKUNK AND PORCUPINE RUN OFF THE SAME CLOCK IN BOTH RENDER PATHS (user 2026-08-07: "cut the
      // skunk's animation speed in half") ── it already had been, once, and it never showed: the GRID-STAMPED
      // path reads B.aframe, which carries the eased 12↔24 fps rate and the skunk's own ×0.5 on top, while
      // this emit read animClk at a flat 24. So the authored rate only ever applied beyond the trace radius,
      // and the animal the player was actually looking at ran at 24 fps — and jumped 4× the moment it crossed
      // the boundary. Same clock now, so the rate is what the marcher set, near and far alike.
      const fi3 = (B.kind === 2 && (skunkSlot || porcSlot)) ? (Math.floor(B.aframe || 0) % nfr)
        : (B.kind === 2 || B.kind === 6) ? (Math.floor((B.animClk || 0) * desRate) % nfr)   // …and the desert rate applies HERE, which is the branch a kind-2 creature actually takes — putting it only on the line below meant the scorpion silently stayed at 24   // WORM/FISH: the frame runs off the creature's OWN clock — the worm's freezes with its pauses, the fish's scales with its swim speed
        : Math.floor((tb3 + wk * 0.37) * desRate) % nfr;   // per-species rate for the desert set; everything else keeps the 24 fps house rule                  // 24 fps cycle, desynced per creature (duck/lily are single static models)
      let glow = 0;
      if (B.kind === 1) {                              // GLOW (fireflies only): random dark spell, then the yellow abdomen holds BRIGHT for a full 2 s
        if (!B.glowT || now > B.glowT) { B.glow = !B.glow; B.glowT = now + (B.glow ? 2000 : 1500 + Math.random() * 3500); }
        glow = B.glow ? 2.8 : 0;
        if (glow > 0) ffLights.push([B.x - winOX, py3, B.z - winOZ, glow * fadeIn * fadeOut, dp2]);   // this one casts LIGHT — window coords for the tracer
      } else if (B.kind === 3) {                       // DUCK EYE BLINK (user): the black eye voxel flashes green ~every 2.5-5 s for a beat; glow lane carries 0/1 to the shader
        if (!B.blinkT || now > B.blinkT) { B.blink = !B.blink; B.blinkT = now + (B.blink ? 150 : 2200 + Math.random() * 2600); }
        glow = B.blink ? 1 : 0;
      }
      const rx = px3 - cam[0], ry = py3 - cam[1], rz = pz3 - cam[2];
      if (bunnySlot && dp2 < 30 * 30 && bunnyBoxN < bunnyBoxes.length) { const bx9 = bunnyBoxes[bunnyBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // SOLID (user): publish a hitbox for each near bunny so the player can't run through it
      if (LIFE_UNI && B.sN && uniTraced(B)) unstampWorm(B);   // ── CROSSING THE TRACE RADIUS ── a mammal that was grid-stamped and is now traced MUST drop its stamp, or the old voxels stay welded into the world and read as an animal frozen in the terrain (user 2026-08-06). The songbird path already did this; the mammal branches below fell straight through to the emit without it.
      if (bunnySlot && (!LIFE_UNI || !uniTraced(B))) { stampBunny(B, Math.round(mamSeatG(B, MAMFIT.bunny) + hurtHop(B))); continue; }   // …seated by the SAME footprint scan the traced path uses (user 2026-08-07)   // …including the bounce (user)   // BUNNY is GRID-STAMPED into W (editor-identical, user) — stamp instead of trace-emit, and SKIP the emit below (hitbox already published above)
      if (armSlot && dp2 < 30 * 30 && armBoxN < armBoxes.length) { const bx9 = armBoxes[armBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and each near armadillo
      if (armSlot && (!LIFE_UNI || !uniTraced(B))) { stampArmadillo(B, Math.round(mamSeatG(B, MAMFIT.arm) + hurtHop(B))); continue; }   // …same seat   // …including the bounce (user)   // ARMADILLO is GRID-STAMPED into W again (editor-identical alignment, user) — stamp instead of trace-emit, skip the emit below
      if (skunkSlot && dp2 < 30 * 30 && skunkBoxN < skunkBoxes.length) { const bx9 = skunkBoxes[skunkBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and each near skunk
      if (skunkSlot && (!LIFE_UNI || !uniTraced(B))) { stampSkunk(B, Math.round(mamSeatG(B, MAMFIT.skunk) + hurtHop(B)), now); continue; }   // …same seat   // …including the bounce (user)   // SKUNK is GRID-STAMPED into W (editor-identical alignment + blink, user) — stamp instead of trace-emit, skip the emit below
      if (porcSlot && dp2 < 30 * 30 && porcBoxN < porcBoxes.length) { const bx9 = porcBoxes[porcBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and each near porcupine
      if (porcSlot && (!LIFE_UNI || !uniTraced(B))) {   // ── AND THE PORCUPINE'S NOSE HACK IS GONE (user 2026-08-07) ──
        // it used to take the stamp height at the CENTRE and then add half a voxel while ASCENDING, because the
        // model's nose reaches ~3 voxels ahead and buried itself climbing. That was a one-animal patch for the
        // general fault: a seat sampled at one column under a body that is not a column. mamSeatG scans the whole
        // footprint — the nose included — so the lift it was approximating now falls out of the measurement, in
        // every direction rather than only uphill, and for all four animals rather than this one.
        stampPorcupine(B, Math.round(mamSeatG(B, MAMFIT.porc) + hurtHop(B))); continue; }   // …including the bounce (user)   // PORCUPINE is GRID-STAMPED into W (armadillo-style, user's 4th land mammal) — stamp instead of trace-emit, skip the emit below
      if (emitN >= EMIT_CAP) continue;                 // stage the pose (never emit directly) — the nearest are chosen after the loop
      const o4 = emitN * 16;
      emitBuf[o4] = rx * right[0] + ry * right[1] + rz * right[2]; emitBuf[o4 + 1] = rx * up[0] + ry * up[1] + rz * up[2]; emitBuf[o4 + 2] = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]; emitBuf[o4 + 3] = bScale;
      emitBuf[o4 + 7] = (desSlot ? (DESERTS[desSp] ? DESERTS[desSp].item0 : 0) : B.kind === 6 ? FISHES[B.fsp || 0].item0 : (B.kind === 4 ? LILY_ITEM0 + (B.col % Math.max(1, LILY_SZ.length)) : (B.kind === 3 ? (isBaby ? DUCKB_ITEM0 : DUCK_ITEM0) : (B.kind === 2 ? (bunnySlot ? (B.bst ? BUNNY_ITEM0 : BUNNY_JUMP_ITEM0) : (armSlot ? ARMADILLO_ITEM0 : (skunkSlot ? SKUNK_ITEM0 : (porcSlot ? PORCUPINE_ITEM0 : WORM_ITEM0)))) : (B.kind === 1 ? FFLY_ITEM0 : (B.dfly ? DFLY_ITEM0 : BFLY_COLS[B.col])))))) + fi3;
      emitBuf[o4 + 11] = glow;
      // ── THE POSE THE RENDERER USED ── cached so the RAGDOLL can rebuild this creature's voxels in world
      // space at the instant it dies. Nothing extra is allocated: Xw/Yw/Zw are this frame's own arrays, and
      // the anchor is three numbers written over three numbers.
      B.ragIt = emitBuf[o4 + 7]; B.ragA0 = px3; B.ragA1 = py3; B.ragA2 = pz3;
      B.ragX = Xw; B.ragY = Yw; B.ragZ = Zw || RAG_UP; B.ragS = bScale;
      if (Zw) {                                        // FULL 3D frame (fish): every axis has a y component — the level-frame fast path below assumes Zw = world-up
        emitBuf[o4 + 4] = Xw[0] * right[0] + Xw[1] * right[1] + Xw[2] * right[2]; emitBuf[o4 + 5] = Xw[0] * up[0] + Xw[1] * up[1] + Xw[2] * up[2]; emitBuf[o4 + 6] = Xw[0] * fwd[0] + Xw[1] * fwd[1] + Xw[2] * fwd[2];
        emitBuf[o4 + 8] = Yw[0] * right[0] + Yw[1] * right[1] + Yw[2] * right[2]; emitBuf[o4 + 9] = Yw[0] * up[0] + Yw[1] * up[1] + Yw[2] * up[2]; emitBuf[o4 + 10] = Yw[0] * fwd[0] + Yw[1] * fwd[1] + Yw[2] * fwd[2];
        emitBuf[o4 + 12] = Zw[0] * right[0] + Zw[1] * right[1] + Zw[2] * right[2]; emitBuf[o4 + 13] = Zw[0] * up[0] + Zw[1] * up[1] + Zw[2] * up[2]; emitBuf[o4 + 14] = Zw[0] * fwd[0] + Zw[1] * fwd[1] + Zw[2] * fwd[2]; emitBuf[o4 + 15] = 0;
      } else {                                         // LEVEL frame (everything else) — Xw[1] = Yw[1] = 0 and Zw = [0,1,0], so the dots collapse
        emitBuf[o4 + 4] = Xw[0] * right[0] + Xw[2] * right[2]; emitBuf[o4 + 5] = Xw[0] * up[0] + Xw[2] * up[2]; emitBuf[o4 + 6] = Xw[0] * fwd[0] + Xw[2] * fwd[2];
        emitBuf[o4 + 8] = Yw[0] * right[0] + Yw[2] * right[2]; emitBuf[o4 + 9] = Yw[0] * up[0] + Yw[2] * up[2]; emitBuf[o4 + 10] = Yw[0] * fwd[0] + Yw[2] * fwd[2];
        emitBuf[o4 + 12] = right[1]; emitBuf[o4 + 13] = up[1]; emitBuf[o4 + 14] = fwd[1]; emitBuf[o4 + 15] = 0;
      }
      emitWho[emitN] = wk; emitAna[emitN] = B.kind === 1 ? 1 : 0;   // FIREFLIES stay analytic (emissive abdomen + translucent wings need the composite path)
      // which KIND is contending for a slot — the fair-share allocator's only input besides distance. Ducklings
      // are counted apart from their mothers on purpose: a brood is a dozen bodies against her one, so pooled
      // they would spend the whole duck allowance on babies and the mother would never be the one drawn.
      emitKnd[emitN] = fishSlot ? (LIFE_K_FISH + Math.min(LIFE_FISH_MAX - 1, B.fsp | 0)) : (wormSlot ? LIFE_K_WORM : (isBaby ? LIFE_K_BABY : (duckSlot ? LIFE_K_DUCK
        : (wk < 16 ? (B.dfly ? LIFE_K_DFLY : LIFE_K_FLYER) : LIFE_K_OTHER))));   // wk < 16 is the flyer band: butterflies by day, fireflies by night, dragonflies at the top of it
      emitAnc[emitN * 3] = px3; emitAnc[emitN * 3 + 1] = py3; emitAnc[emitN * 3 + 2] = pz3;
      { const cx8 = emitBuf[o4], cy8 = emitBuf[o4 + 1], cz8 = emitBuf[o4 + 2];   // the camera-space anchor written above
        const r8 = LIFE_FRUST_R + (lifeIsDrawn(wk) ? LIFE_FRUST_HYST : 0);
        emitVis[emitN] = (cz8 + r8 > 0 && (Math.abs(cx8) - fsX * cz8) * fnX <= r8 && (Math.abs(cy8) - fsY * cz8) * fnY <= r8) ? 1 : 0; }
      emitDp[emitN] = dp2; emitN++;
    }
    if (CPROF) cpMark(5);
