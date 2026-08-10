    // ── voxel snow ── NEAR the player, landings are the ACTUAL rendered flakes touching down: JS mirrors the shader's
    // deterministic flake lattice (same ih3 hash, same jitter, same integrated fall/wind) and places a voxel at the very
    // column and moment a flake reaches the surface. Beyond flake-visibility range a statistical sprinkle fills the distance.
    // Placement SETTLES SMOOTHLY (a flake rolls to the lowest neighbouring column, ≤2 hops) and melt is TOP-FIRST per column.
    if (ED.on) {                                       // ── INVISIBLE BARRIER (user) ── clamp the player inside the stage rim so they can't walk/fly off into the void; no voxels, nothing rendered. Replaces the old fall-recovery teleport.
      const bx0 = ED.x0 + HW, bx1 = ED.x0 + ED.pw - HW, bz0 = ED.z0 + HW, bz1 = ED.z0 + ED.pd - HW;   // half-width inset so the player BOX (not just its centre) stops at the edge
      if (P.x < bx0) { P.x = bx0; P.hvx = Math.max(0, P.hvx); } else if (P.x > bx1) { P.x = bx1; P.hvx = Math.min(0, P.hvx); }   // zero the outward velocity so they don't stick jittering against the wall
      if (P.z < bz0) { P.z = bz0; P.hvz = Math.max(0, P.hvz); } else if (P.z > bz1) { P.z = bz1; P.hvz = Math.min(0, P.hvz); }
    }
    { const cells = snowCells; cells.length = 0; const meltCells = snowMeltCells; meltCells.length = 0;   // meltCells: the thaw's own list, patched WITHOUT a support seed (see the melt block below). Pooled the same way.
      stopF = (stopF + 1) & 0xffff; if (stopF === 0) { stopF = 1; stopS.fill(0); }       // POOLED across frames — this list reaches thousands of entries during a storm
      if (snowOn !== snowPrevOn) { snowPrevOn = snowOn;   // storm edges: no time-melt DURING a storm (the blanket must not thin while flakes still fall)
        if (snowOn) { snowOnT0 = now; snowFreezeAt = now + SNOW_FREEZE_DELAY; snowWMelting = false; snowWMeltAt = Infinity; snowGMelting = false; snowGMeltAt = Infinity; }   // the water may not skin over until 10 s in (user); a new storm re-arms BOTH melts — snow falling on a thawing blanket must stop it thawing, or the two fight and the ground never settles either way
        else { snowOffT0 = now; snowFreezeAt = Infinity; snowWMeltAt = now + SNOW_WATER_GRACE;   // WATER snow starts its one continuous drain a few seconds after the storm ends
          snowGMeltAt = now + SNOW_MELT; }                      // …and the GROUND starts its own, SNOW_MELT after the storm ends. One clock for the whole blanket instead of one per voxel: the old per-voxel expiries were set at LANDING time and had to be rebased by the storm's length here, so a blanket laid over a five-minute storm melted in the order it fell and in visible bursts. Now it lies for SNOW_MELT and then drains steadily to nothing.
      }
      {                                              // ── STORM EDGES ── both sweep DOWN from just off the top of the screen at SNOW_SWEEP.
        const topY = P.y + SNOW_HEAD;                  // Runs EVERY frame but must sit after the transition block above, which is what sets
        snowLeadY = snowOn ? topY - ((now - snowOnT0) / 1000) * SNOW_SWEEP : -1e9;   // snowOnT0/snowOffT0. Computing it from a stale start time let the
        snowTrailY = snowOn ? 1e9 : topY - ((now - snowOffT0) / 1000) * SNOW_SWEEP;  // first frame think the edge had already passed and dump ~200 voxels
        snowVis = snowOn || snowTrailY > -40;          // of blanket under an empty sky; computing it INSIDE that block froze the sweep entirely.
      }
      const scanTop = (wx, wz, buried) => {            // topmost landing surface at a column — passes through (and records) soft decor
        const gx = gwrap(wx, WX), gz = gwrap(wz, WZ);
        const scol = gx + gz * WX;                     // CACHED unless the caller wants the buried list back (see stopY)
        if (!buried && STOP_CACHE) { const st = stopS[scol]; if (st !== 0 && ((stopF - st) & 0xffff) < STOP_TTL) return stopY[scol]; }
        const b2 = gx + gz * WX * WY;
        const bcol = (gx >> 3) + (gz >> 3) * BX * BY;  // this column's brick index base (+ (y>>3)*BX per level)
        let y = Math.min(WY - 2, hmap[gx + gz * WX] + 118);
        // The scan starts 118 above the terrain (tallest-tree allowance) but most columns are BARE — the old
        // voxel-at-a-time descent burned ~118 array reads of empty air per column, ~6,400 columns per storm frame,
        // and profiling put the whole storm CPU side at ~10 ms. An empty 8³ occupancy brick proves the next 8 rows
        // of THIS column are air, so leap them — exact, and it cuts the scan to a handful of reads.
        while (y >= WL) {
          const idv = W[b2 + y * WX];
          if (!idv) {
            const bb = bcol + (y >> 3) * BX;
            if (!((bricks[bb >> 5] >>> (bb & 31)) & 1)) { y = ((y >> 3) << 3) - 1; continue; }   // whole brick row empty → skip past it
            y--; continue;
          }
          if (SNOW_PASS.has(idv)) { if (buried) buried.push(b2 + y * WX); y--; continue; }
          break;
        }
        if (!buried) { stopY[scol] = y; stopS[scol] = stopF; }
        return y;
      };
      const landSnowAt = (wx0, wz0) => {
        if ((snowQN - snowHead) + (snowWN - snowWHead) >= SNOW_MAX) return;   // at the live cap: stop ACCUMULATING — never melt existing blanket to make room
        // ORDER (user): the FLAKES arrive first, the blanket second. The leading edge sweeps down from the sky, so
        // until it has passed THIS column's surface no flake has reached the ground here yet — laying snow before
        // then made the blanket appear under a still-empty sky. Tested per column, not globally, so a valley floor
        // starts collecting after the ridge beside it rather than at the same instant.
        if (snowLeadY > scanTop(wx0, wz0, null)) return;
        let wx = wx0, wz = wz0;
        const top0 = W[gwrap(wx0, WX) + scanTop(wx0, wz0, null) * WX + gwrap(wz0, WZ) * WX * WY];
        for (let hop = 0; hop < ((SNOW_FERN.has(top0) || foliaTab[top0]) ? 0 : 2); hop++) {   // SMOOTH SETTLING: roll to the lowest 4-neighbour column so the blanket levels instead of spiking — unless a fern catches the flake, and a CROWN is the same case for the same reason (user 2026-08-07: canopy coverage measured 18% against the ground's 44%): a canopy sits above the ground beside it, so rolling always sheds the flake off the tree. Bounding the drop only got it to 24% — skipping the roll outright is what the fern precedent already does
          const t0 = scanTop(wx, wz, null);
          let bx = wx, bz = wz, bt = t0;
          const rMax = SNOW_ROLL_MAX || 1e9;             // the roll LEVELS a blanket; it must not tip one off a tree
          const n1 = scanTop(wx + 1, wz, null); if (n1 < bt && t0 - n1 <= rMax) { bt = n1; bx = wx + 1; bz = wz; }
          const n2 = scanTop(wx - 1, wz, null); if (n2 < bt && t0 - n2 <= rMax) { bt = n2; bx = wx - 1; bz = wz; }
          const n3 = scanTop(wx, wz + 1, null); if (n3 < bt && t0 - n3 <= rMax) { bt = n3; bx = wx; bz = wz + 1; }
          const n4 = scanTop(wx, wz - 1, null); if (n4 < bt && t0 - n4 <= rMax) { bt = n4; bx = wx; bz = wz - 1; }
          if (bx === wx && bz === wz) { break; }
          wx = bx; wz = bz;
        }
        buriedTmp.length = 0;                          // REUSED: a fresh [] here was 98.5% of the snowfall allocation profile (one per candidate column, thousands per frame)
        const buried = buriedTmp;
        const y = scanTop(wx, wz, buried);
        const gx = gwrap(wx, WX), gz = gwrap(wz, WZ), b2 = gx + gz * WX * WY;
        const top = W[b2 + y * WX];
        if (!top || y < WL || y >= WY - 2 || SNOW_SKIP.has(top)) return;   // y == WL allowed — ice and waterline beach sand collect snow; lava never does
        // ── AN ANIMAL IS NOT A LANDING SURFACE (2026-08-08) ── scanTop reads W, and a grid-stamped creature
        // IS in W, so a perched bird's back read as ground and collected a snow cap. Two things wrong with
        // that: the blanket rode the bird, and when it flew off the snow was left hanging in mid-air with
        // nothing beneath it — the resolver never re-asks, because the stamp patch is uploaded track=false
        // (see unstampWorm). MEASURED as the source of every floater a storm alone produced: 4 components,
        // all snow, all sitting on conduit cells, with no chopping anywhere in the session. Skipping the
        // column costs nothing — the storm re-offers it every tick, so it collects the moment the bird moves.
        if (stampedIdx.has(b2 + y * WX)) return;
        // ── SNOW SETTLES ON THE CROWN ── (user 2026-08-07, twice: "the snow is still not landing on the pine
        // trees"). Foliage used to be refused outright, so nothing EVER settled on a pine and every white pixel
        // in a canopy was a falling flake — which stops dead when the storm ends while the ground keeps its melt
        // timer, and that is what read as "melts off the trees faster". The refusal was itself a fix ("snow seems
        // to be floating"): one needle is one thin voxel, so a cap on it hangs with sky underneath. Requiring the
        // crown to be at least TWO voxels thick under the flake keeps that honest without demanding a dense shelf
        // — an earlier attempt wanted 3 of 4 sides too and let essentially nothing through. Landing here joins
        // the ordinary ground queue below, so it melts on exactly the same timer, which is what was asked for.
        if (foliaTab[top]) {                           // ── SNOW SITS ON A CROWN, NOT ON A NEEDLE TIP ──
          if (!SNOW_ON_CROWN) return;
          // A pine crown is sparse, so a flake that lands on an isolated needle has open sky directly beneath it
          // and reads as a floating white cube — which is what the blanket refusal was originally added to stop,
          // and what came back with the v0.9 gate (user 2026-08-07: "now snow AND pinecones are floating").
          // Require the needle to be part of a SHELF instead: something under it, and at least two of its four
          // sides carrying crown as well. Two, not three — three let almost nothing through when tried earlier.
          if (SNOW_SHELF > 0 && !W[b2 + (y - 1) * WX]) return;
          var shN = 0;
          if (W[gwrap(wx + 1, WX) + y * WX + gz * WX * WY]) shN++;
          if (W[gwrap(wx - 1, WX) + y * WX + gz * WX * WY]) shN++;
          if (W[gx + y * WX + gwrap(wz + 1, WZ) * WX * WY]) shN++;
          if (W[gx + y * WX + gwrap(wz - 1, WZ) * WX * WY]) shN++;
          if (shN < SNOW_SHELF) return;
        }   // …and FOLIAGE collects it too, exactly as v0.9 did (user: "the snow landing on the trees worked in v0.9", 1c794ab). The later blanket refusal was a fix for snow reading as floating on a lone needle; the user has asked for the v0.9 look back twice, so it is the default and the toggle stays for A/B.
        if (freezeK < 0.6 && waterAt(wx, WL, wz)) return;    // liquid water sheds snow — it only settles once the surface has properly frozen. Asks the COLUMN, not the top voxel: `top === WATER_T` missed every water column topped by a LILY PAD (stamped at WL+1) or by snow left from the previous freeze, and those collected white cubes on fully liquid water at freezeK = 0.
        if ((top === SNOW[0] || top === SNOW[1]) &&
            (W[b2 + (y - 1) * WX] === SNOW[0] || W[b2 + (y - 1) * WX] === SNOW[1]) &&
            (W[b2 + (y - 2) * WX] === SNOW[0] || W[b2 + (y - 2) * WX] === SNOW[1])) return;   // 3 layers max — the settle-roll keeps neighbouring stacks within 1 layer, so deeper stacks stay smooth
        for (const bi of buried) { W[bi] = 0; cells.push(bi); }
        const ii = b2 + (y + 1) * WX;
        W[ii] = SNOW[(wx ^ wz) & 1];
        stopY[gx + gz * WX] = y + 1; stopS[gx + gz * WX] = stopF;   // the new top is known exactly; gpuPatch only runs at the end of the tick, and until then the settle-roll and the 3-layer cap must not read the old surface
        cells.push(ii);
        if (waterAt(wx, WL, wz)) { if (snowRoomW()) snowWI[snowWN++] = ii; }   // landed on the frozen surface — drained continuously after the storm, not on a per-voxel timer
        else if (snowRoomQ()) { snowQI[snowQN++] = ii; }   // landed on GROUND — no expiry stamp; the whole queue drains together once the thaw latches
      };
      if (snowOn && !dead && !ED.on) {                 // no weather inside the asset-editor void
        const t3 = now / 1000;                         // ── EXACT LANDINGS ── mirror of the shader flake field (u.time = now/1000)
        const fX = -windAX + Math.sin(t3 * 0.6) * 0.8, fZ = -windAZ + Math.cos(t3 * 0.5) * 0.8, fY = snowFallAcc;
        const drop = snowFallV * dt + 0.002;
        const R2 = Math.min(120, renderDist);
        const px0 = Math.floor((P.x - R2 + fX) / 3), px1 = Math.ceil((P.x + R2 + fX) / 3);
        const pz0 = Math.floor((P.z - R2 + fZ) / 3), pz1 = Math.ceil((P.z + R2 + fZ) / 3);
        // ── EXACT NEAR-FIELD LANDINGS REMOVED (user 2026-08-07) ── flakes used to touch down at the very
        // column the rendered flake fell into, mirroring the shader lattice over a 120-radius disc. Measured
        // in-session it was ~1.1 ms of a ~2 ms snow budget — about HALF the whole cost of snowfall — and it
        // was also why crowns collected less than the ground: it brackets which lattice layers to test from
        // the CELL-CENTRE column, and one 3-voxel cell can hold a gap and a crown forty voxels apart, so
        // canopy columns were served by the sprinkle alone at half the rate. The uniform sprinkle below now
        // owns the whole disc: 1.1 ms back, and canopy coverage went 24% -> 39% against the ground's 50%.
        // ── BEYOND R2 ── ONE uniform-area sprinkle out to the FULL render distance, matched EXACTLY to the
        // near-field per-column landing rate (lattice occupancy × fall speed ÷ cell volume) — the blanket fills
        // at the SAME pace at every range, no fast disc near the player, no sparse ring past it.
        const R2s = 0;                                 // the sprinkle owns the WHOLE disc now — see above
        const R4 = Math.max(R2, renderDist);
        const rate = 0.0245 * snowFallV / 27;          // landings per voxel² per second — 0.0245 MUST match the shader's h1 > 0.9755 lattice test
        snowSprinkAcc += rate * Math.PI * Math.max(0, R4 * R4 - R2s * R2s) * dt;
        let tries = Math.min(Math.max(200, Math.round(dt * 40000)), snowSprinkAcc | 0); snowSprinkAcc -= tries;   // dt-scaled cap (like the melt drain) — the old flat 140/frame starved the far ring at big view radii, so the blanket filled visibly slower with distance
        if (snowSprinkAcc > 4000) snowSprinkAcc = 4000;   // starvation carry-over stays bounded — a lag spike must not dump minutes of backlog in one frame
        const fadeR = R4 * 0.62;                       // ── EDGE FADE (user: "make this snow cutoff less obvious") ── the sprinkle used to run at FULL density right up
        const fadeSpan = Math.max(1, R4 - fadeR);      // to R4 and then stop, drawing a hard circular rim across open ground and frozen lakes alike. Past fadeR the
        for (let k = 0; k < tries; k++) {              // landing chance now rolls off quadratically to zero at the rim, so the blanket THINS into bare ground over
          const ang2 = Math.random() * 6.283, rad2 = Math.sqrt(R2s * R2s + Math.random() * Math.max(0, R4 * R4 - R2s * R2s));   // ~380 vox and the boundary reads as
          if (rad2 > fadeR) { const tE = (rad2 - fadeR) / fadeSpan; if (Math.random() < tE * tE) continue; }                // weather, not a line. Only the outer band
          const wx = Math.floor(P.x + Math.sin(ang2) * rad2), wz = Math.floor(P.z + Math.cos(ang2) * rad2);                 // is thinned — near-field density is untouched.
          if (wx <= rect.xlo || wx >= rect.xhi || wz <= rect.zlo || wz >= rect.zhi) continue;
          landSnowAt(wx, wz);
        }
      }
      let melted = 0;                                  // drain expired snow — dt-scaled so melt pace is fps-independent too; ONLY time-based and ONLY between storms (the cap drops landings instead)
      const meltCap = Math.max(8, Math.round(dt * SNOW_GROUND_RATE));
      // ── WATER SNOW: ONE CONTINUOUS MELT (user) ── the per-voxel expiry timers made the lake melt in STAGGERED BURSTS
      // and only START ~97 s after the storm (SNOW_MELT rebased by the storm length). Instead, SNOW_WATER_GRACE seconds
      // after the storm ends we latch `snowWMelting` and drain the WHOLE water queue at a steady rate with NO per-voxel
      // timing — so a frozen lake sheds its blanket in one smooth, rapid, uninterrupted sweep (~8 s) and is clear long
      // before the ground.
      const wCap = Math.max(8, Math.round(dt * SNOW_WATER_RATE));
      if (!snowOn && !snowWMelting && (now >= snowWMeltAt || freezeK < 0.92)) snowWMelting = true;   // …and start draining as soon as the ice starts to go, rather than 6 s later, so the lake does not sit pinned at 0.4 waiting on a clock
      // ── A MELTED FLAKE ORPHANS NOTHING, BY CONSTRUCTION (2026-08-08) ── the mirror of the landing rule a few
      // lines up in supPush, and the fix for the one measured way a genuine floater is left hanging in front of
      // the player. Both melts climb to the TOP snow voxel of their column before clearing it, so nothing can
      // be resting on the cell that goes: snow above would have been climbed past, a cone HANGS from what is
      // over it and the drape flood already refuses to let snow hold one up, and a creature stamp is CONDUIT
      // and is never held by anything. Yet every cleared cell was queued anyway, because supPush's "is there
      // anything in the layer above" probe is a 3x3 that a neighbouring column's blanket satisfies on the
      // diagonal — and snow does not cantilever, so that neighbour was never this column's problem.
      // MEASURED: a thaw put 93,958 cells into a queue with a 2 ms/frame budget. Nothing in there could ever
      // be lifted; it just sat in front of everything that could. That is what makes a floater the player
      // just created hang around for a minute instead of a second, and why it only ever happens after snow.
      // These cells go through gpuPatch exactly as before — brick occupancy, the scanTop cache and the
      // navfield all still get told. It is only the support queue that is spared the question.
      // ── ONE ENTRY MELTS ONE LAYER (bug, fixed 2026-08-09) ── every landed flake queues exactly one entry, so
      // N entries must clear N voxels or the blanket cannot reach zero. Climbing to the column's TOP is right —
      // it is what stops a lower layer vanishing under an upper one — but it means the entry queued for layer 1
      // clears layer 2, and when layer 2's own entry comes up its cell is already empty, so it cleared NOTHING.
      // Two entries, one layer gone: every column that ever held two or more layers kept its bottom one for
      // good. MEASURED on a lake: 404 voxels over 171 columns at storm end (4 one-deep, 101 two, 66 three) drained
      // to exactly 167 columns of exactly 1 layer and stayed there — 101 + 66, i.e. precisely the columns that had
      // stacked. That is the residue left floating on the water (user 2026-08-09).
      // The fix is to resolve the entry to whatever snow the column still HAS rather than to the cell it was
      // queued for: step DOWN to the first snow at or below it, then climb to the top of that stack as before.
      // Order is unchanged (still strictly top-down, still no floating snow) and the pairing is now 1:1.
      const meltTop = (jj) => {                        // → the index actually cleared, or -1 if this column is already bare
        if (!(W[jj] === SNOW[0] || W[jj] === SNOW[1])) {
          let d = 0;
          while (d < 4 && jj >= WX && !(W[jj] === SNOW[0] || W[jj] === SNOW[1])) { jj -= WX; d++; }
          if (!(W[jj] === SNOW[0] || W[jj] === SNOW[1])) return -1;
        }
        for (let up = 0; up < 4 && jj + WX < W.length && (W[jj + WX] === SNOW[0] || W[jj + WX] === SNOW[1]); up++) jj += WX;   // climb to the column's TOP snow voxel
        W[jj] = 0;                                     // melt TOP-DOWN — a lower layer never vanishes under an upper one, so no floating snow
        return jj;
      };
      if (snowWMelting) { let wMelt = 0;
        while (snowWHead < snowWN && wMelt < wCap) {
          const got = meltTop(snowWI[snowWHead++]);
          if (got >= 0) meltCells.push(got);
          wMelt++;
        }
        if (snowWHead >= snowWN) { snowWMelting = false; snowWMeltAt = Infinity; }   // queue emptied — stand down until the next storm
      }
      // ── GROUND: ONE CONTINUOUS MELT (user 2026-08-09) ── the same shape as the water drain above, on a longer
      // clock. It latches once, empties the whole queue at a fixed voxels-per-second, and stands down only when
      // the queue is actually empty — so "melting" is a single steady sweep with a beginning and an end, and
      // there is no state in which some of the blanket has expired and the rest has not.
      if (!snowOn && !snowGMelting && now >= snowGMeltAt) snowGMelting = true;
      if (snowGMelting && !snowOn) {
        while (snowHead < snowQN && melted < meltCap) {
          const got = meltTop(snowQI[snowHead++]);
          if (got >= 0) meltCells.push(got);
          melted++;
        }
        if (snowHead >= snowQN) { snowGMelting = false; snowGMeltAt = Infinity; }   // queue emptied — stand down until the next storm
      }
      if (snowWHead > 8192) snowCompactW();            // in-place, no reallocation (was .slice() of a multi-hundred-thousand-entry array)
      if (snowHead > 8192) snowCompactQ();
      if (cells.length) { if (CPROF) cpEvt |= 32; gpuPatch(cells, false); }   // dirty-word brick upload, not the 774 KB whole-table re-upload — same bits, ~1000x less traffic (verified by __vb.bdiff())
      // …and the thaw's cells the same way, but with the support seed muted. NOT gpuPatch's `track=false`,
      // which would also drop nvTouch and the hmap lower — a melting blanket does change what a creature can
      // walk on. Only the SUP queue is skipped, for the reason set out at the melt itself.
      if (meltCells.length) { supMute = true; try { gpuPatch(meltCells, false); } finally { supMute = false; } }   // try/finally, NOT two bare statements: tick.js deliberately keeps the loop alive after a throw, so one exception in here would latch the mute for the whole session — no terrain edit ever seeds the support queue again, chopped trees stop falling, carved rock leaves floating slabs, and nothing reports why
    }
    if (CPROF) cpMark(3);

