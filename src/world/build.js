  async function buildWorld(withStages) {             // boot builds EVERYTHING the saved view distance can reach — no in-game waiting, like the classic full builds
    const bh = Math.min(HALF, Math.ceil((RD_FIXED + 96) / 8) * 8);
    const cx = winOX + HALF, cz = winOZ + HALF;
    rect.xlo = cx - bh; rect.xhi = cx + bh; rect.zlo = cz - bh; rect.zhi = cz + bh;
    let built = false;
    if (poolOk) {                                      // fan the whole build across every core, blit slabs as they land
      const jobs = [];
      for (let s = 0; s < 32; s++) jobs.push(...poolChunks(rect.xlo, rect.xhi, rect.zlo + (((bh * 2) * s) >> 5), rect.zlo + (((bh * 2) * (s + 1)) >> 5)));
      let done = 0, lastPct = -1;
      for (const j of jobs) {
        while (poolOk && !j.done) {
          if (withStages) { const pct = (done * 100 / jobs.length) | 0; if (pct !== lastPct) { lastPct = pct; loadMsgEl.textContent = 'growing forest…'; setLoad(22 + done / jobs.length * 60); } }
          await poolAwait();                       // wakes on the next finished slab, not on a clamped timer
        }
        if (!poolOk) break;
        const oq = j.msg && j.msg.orph;
        blitSlab(j);

        if (oq && oq.seeds && oq.seeds.length) {   // now the voxels are really in W, ask the resolver about the ambiguous ones
          for (let q = 0; q < oq.seeds.length; q += 3) {
            const ii = gwrap(oq.seeds[q], WX) + oq.seeds[q + 1] * WX + gwrap(oq.seeds[q + 2], WZ) * WX * WY;
            if (W[ii]) { supPush(ii); ORPH.seeded++; }
          }
        }
        done++;   // ── THE COUNTER NOBODY WAS COUNTING (2026-08-19) ── `done` was declared at the top of this
        // loop, read by the progress line above, and incremented NOWHERE, so pct was permanently 0: the bar sat
        // at its 22% floor for the whole build and the message never moved off the first 'growing forest'.
        // It went unnoticed because setLoad was a no-op — the CSS trickle owned the bar, so the dead counter
        // fed a function that threw its argument away. The moment the bar started tracking real progress
        // (core/boot.js) this became the thing holding it still.
        jobById.delete(j.id); j.msg = null;            // …and DROP the slab. `jobs` outlives the loop, so the map delete alone freed nothing: msg owns the transferred W/hmap/bb/wb, and this rect is the whole window, so every slab held to the end summed to a second complete copy of the world buffer (~226 MB) for the entire boot. Blitted and its orphan seeds read (oq is captured above) — nothing reads it again. Same release poolFree does for the streaming path.
      }
      built = poolOk;
      if (!built) { poolQueue.length = 0; jobById.clear(); }
    }
    if (!built) for (let s = 0; s < 32; s++) {         // no pool (or it died mid-build) — the inline path regenerates everything identically
      if (withStages) { setLoad(22 + s / 32 * 60); await stage('growing desert…'); }
      genRegion(rect.xlo, rect.xhi, rect.zlo + (((bh * 2) * s) >> 5), rect.zlo + (((bh * 2) * (s + 1)) >> 5), true);
    }
    if (withStages) { setLoad(86); await stage('baking occupancy…'); }
    // ── L2 ONLY AFTER A POOLED BUILD ── every gen worker scans its own slab's 8-cube occupancy and blitSlab
    // merges those bits, so the L1 tables are already complete here; the closing full-world rebuildBricks was
    // re-deriving them from 226 MB of voxels for nothing. Measured over two boots: 244 ms and 273 ms for ZERO
    // changed bits in either bricks or wbricks. L2 is NOT worker-computed, so it still has to be built — which
    // is exactly what the pooled BAND path at rebuildBricks2W already does after its own slab merges.
    if (built) rebuildBricks2(0, WX, 0, WZ); else rebuildBricks(0, WX, 0, WZ);
  }
  // ── SPAWN BESIDE A LAKE (user 2026-07-20) ── walk the river systems for the nearest reservoir/tail lake, then step out
  // along a ray from its centre to the first DRY column past the shoreline and stand a few voxels inland. Purely analytic
  // (riverAt + H), so it runs before the world is built and lands the same spot every refresh for a given seed. If nothing
  // qualifies within reach, fall through to the original eastward walk — spawning is never allowed to fail.
  {
    const lakeSpawn = () => {
      const cands = [];
      const c0x = Math.floor(SPWX / RIVCELL), c0z = Math.floor(SPWZ / RIVCELL);
      for (let r = 0; r <= 4 && !cands.length; r++)    // rings outward — the NEAREST qualifying lake wins
        for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
          if (Math.max(Math.abs(dx), Math.abs(dz)) !== r) continue;
          const R = riverAt(c0x + dx, c0z + dz); if (!R) continue;
          for (const L of R.lakes) if (L.r >= 120) cands.push(L);   // reservoirs/tail lakes only — a headwater pond is too small to read as "next to a lake"
        }
      if (!cands.length) return null;
      cands.sort((a, b) => ((a.x - SPWX) ** 2 + (a.z - SPWZ) ** 2) - ((b.x - SPWX) ** 2 + (b.z - SPWZ) ** 2));
      const L = cands[0];
      for (let k = 0; k < 32; k++) {                   // sweep headings around the lake for a clean, dry, open shore
        const a = k * (Math.PI * 2 / 32);
        const ux = Math.cos(a), uz = Math.sin(a);
        for (let d = Math.max(20, L.r - 40); d <= L.r + 90; d += 2) {
          const x = Math.round(L.x + ux * d), z = Math.round(L.z + uz * d);
          if (H(x, z) <= WL + 1) continue;             // still water/beach-flush — keep walking outward
          const sx = Math.round(L.x + ux * (d + 6)), sz = Math.round(L.z + uz * (d + 6));   // stand a little inland of the waterline
          if (H(sx, sz) <= WL + 1 || nearCave(sx, sz)) break;
          let shore = false;                           // …and confirm real lake water is actually in view from there
          for (let b = 0; b < 8; b++) { const ba = b * 0.7854;
            if (H(Math.round(sx + Math.cos(ba) * 14), Math.round(sz + Math.sin(ba) * 14)) <= WL) { shore = true; break; } }
          if (shore) return { x: sx, z: sz, yaw: Math.atan2(L.x - sx, L.z - sz) };   // …and face the water on arrival
          break;                                       // this heading's shoreline is not a usable bank — try the next
        }
      }
      return null;
    };
    // ── ?spawn=X,Z PINS IT, AND THAT IS A TEST INSTRUMENT, NOT A FEATURE (audit 2026-08-31) ── the spawn is
    // random per refresh, which is right for play and ruinous for measurement: every reload lands the camera on
    // different ground, so two builds can never be compared. A whole afternoon of terrain tuning was spent
    // chasing that - the same constant measured 17.9% and 71.6% on consecutive runs with nothing changed but
    // the world under it, and a one-patch reading of "8.3% risers" led to a diagnosis that was simply wrong.
    // The noise FIELDS are pure functions of world position; only the band masks depend on SPWX/SPWZ. So
    // pinning those two numbers makes an absolute coordinate mean the same terrain across reloads, which is
    // what an A/B needs. Absent the flag nothing changes.
    const _sp = /[?&]spawn=(-?\d+),(-?\d+)/.exec(location.search);
    SPWX = _sp ? +_sp[1] : Math.round((Math.random() - 0.5) * 400000);
    SPWZ = _sp ? +_sp[2] : Math.round((Math.random() - 0.5) * 400000);
    console.log('[vb] spawn ' + (_sp ? 'PINNED' : 'RANDOM'), SPWX, SPWZ);
  }
  // ── SPAWN IN THE OAK FOREST (user 2026-08-19: "have the player spawn in the oak forest on refresh") ── NOT
  // done here, and the reason is worth writing down: walking SPWX cannot work. chNear/cherryM/oakM in
  // world/window.js are all anchored to SPWX itself (chNear is pwrap(x - (SPWX - CHOFF))), so the bands are
  // positioned RELATIVE to the spawn point — move the spawn and the whole arrangement moves with it, and the
  // player lands in exactly the same biome they started in. A first attempt walked east testing oakM and
  // subtracting the blossom, and landed in cherry on 5 boots out of 5. The fix is in the OFFSETS instead: see
  // CHOFF / OAKOFF / OAKWOFF in world/window.js, which now place spawn in the middle of the oak strip east of
  // the blossom rather than 140 voxels inside the blossom's own east edge.
  { let g = 0; while ((H(SPWX, SPWZ) <= WL + 6 || nearCave(SPWX, SPWZ)) && g++ < 8000) SPWX += 16; }   // never spawn in a lake or a gorge — capped so a random pick in open water cannot hang the boot
  // 2160 = the middle of the oak strip east of the anchor (OAKOFF is -1080, so the oak/pine line is 1080 east
  // and the pure strip runs 1080..3240). Same water/gorge guard the anchor gets, applied at the offset spot.
  // ── SPAWN IN THE BIRCH FOREST (user) ── SPOX, and ONLY SPOX. The note above says why walking SPWX cannot
  // work: every band is anchored to SPWX, so moving the anchor moves the whole arrangement and lands the
  // player in the same biome. SPOX is the other half of that pair - a step taken INSIDE the fixed
  // arrangement, so it is the one number that can change which strip the player stands in without moving a
  // single boundary. -2160 is the birch band's own centre in anchor coordinates:
  // BIRCHC = BAND_MIRROR * (BIRCHOFF + BIRCHH), the centre of the birch strip. It was +2160, the middle of
  // the oak strip.
  // ── AND IT IS DERIVED, NOT WRITTEN OUT (2026-08-29) ── this was the literal -2160, which was BIRCHC at the
  // time. Inserting the ARCTIC band moved the birch one strip west to -4320 and left the literal pointing at
  // the arctic's centre instead, so the player spawned on a glacier. The whole point of the note above is that
  // SPOX means "the middle of the band I want to stand in", and only BIRCHC can say where that is.
  // ── SPAWN IN THE ARCTIC (user 2026-08-29) ── SPBAND is the ONE place the spawn biome is chosen, and it is a
  // band CENTRE rather than a number for the reason the note above gives: a literal goes stale the moment a
  // band is inserted, which is exactly how the player ended up on a glacier by accident an hour before being
  // put on one deliberately. Point it at BIRCHC to go back to the birch forest, OAKC for the oak, and the two
  // guards below follow automatically because they test SPBANDM rather than naming a mask.
  const SPBAND = ARCTC, SPBANDM = arcticM;
  SPOX = SPBAND;
  // ── AND ON TOP OF A GLACIER, IN THE MIDDLE OF THE ICE (user 2026-08-30: "spawn the player in the middle
  // of the arctic. ontop of a mountain") ── the eastward walk this replaces could deliver NEITHER, and one fact
  // explains both failures: in the arctic H is the SEABED. The band is open sea (ARCT_SEA, world/window.js)
  // and the glaciers are stamped above the waterline by world/terrain.js without ever raising hmap, so
  // `H > WL + 6` is false for every column of the deep arctic. The walk ran its full 400 steps east without
  // once finding "dry ground", the band test then sent it back west from the centre, and it stopped at the
  // first thing that IS above the water ─ the snowy rim, most of a kilometre outside the ice. The player was
  // spawning at the arctic's EDGE, at sea level, every boot.
  // So the search asks the function that knows where the surface of an arctic column actually is. arctIceTop
  // is what the penguins are sited with: it re-derives the cap/sheet stamp from the same three fields instead
  // of reading W, so it can be asked about a column before the world is built, and it returns the y a body
  // STANDS at (or -1 for open water).
  // ── IT MOVES SPWZ, AND THAT IS NOT A NEW OFFSET IN DISGUISE ── a summit search that scans x alone is one
  // line drawn across ice whose floes are ~380 voxels wide (ARCT_FLOEF): it crosses six of them and can miss
  // every crest. Scanning latitude as well is safe here for one specific reason ─ every band centre is
  // written `+ desWob(z) - desWob(SPWZ)`, so the wobble cancels at z == SPWZ and the arctic's centre is
  // SPWX + ARCTC at ANY latitude. Each ROW is therefore scored with SPWZ already set to that row's own z; the
  // field is only self-consistent that way, and a grid swept under one fixed SPWZ would be ranking columns
  // against a mask they will not be generated with. Nothing is generated at this point in the boot ─
  // gen-pool.js captures the anchor later and it rides with every job ─ so SPWZ is still free to move.
  // ── AND IT NEEDS NO BIOME GATE ── point SPBAND at BIRCHC or OAKC and arctIceTop answers -1 for every column
  // it is asked about (its first line is the arctic snow mask), the sweep finds nothing, SPWZ is put back and
  // the walk below runs exactly as it always did. The summit search is self-limiting for the same reason the
  // two guards are: it tests the FIELD rather than naming the band.
  const ARCT_SPR = 1152;                               // how far either side of the band centre a summit may be found. arcticM is a flat 1 out to ARCTH - ARCTB/2 = 1260, so every candidate is honestly the MIDDLE of the ice and not its rim
  const ARCT_SPSTEP = 96;                              // coarse grid pitch, both axes…
  const ARCT_SPFINE = 8;                               // …and the pitch of the second pass, which re-searches one coarse cell around the winner: a 96-voxel grid lands NEAR a crest rather than on it
  const ARCT_SPMIN = 24;                               // voxels above the waterline before a column counts as a glacier at all ─ clear of ARCT_CAPMAX and the flat sheets, the same line PENG_MINTOP draws for the penguins
  const ARCT_SPBENCH = 3, ARCT_SPTOL = 2;              // …and it has to be a BENCH: all EIGHT neighbours this far out — the player's own footprint, since HW is 2.6 — standing within this many voxels of it. Without the test the tallest column on a crevassed glacier is a serac and the player starts balanced on its point; with it loose (±4 at a tolerance of 6) the solver still had a 6-voxel wall under one edge of the box to resolve, which showed up as the spawn being lifted 1-3 voxels off the y the search chose, and once as a boot that ended on the seabed two hundred voxels below. Flat means flat: a neighbour is not allowed to stand ABOVE the summit either
  {
    const zc = SPWZ, xc = SPWX + SPBAND;
    // ── AND IT IS CLAMPED THE WAY THE STAMP IS ── the cap loop in world/terrain.js writes up to
    // `Math.min(WY - 1, WL + capH)` and arctIceTop does not, so on the 192- and 256-tall world tiers (core/gpu.js
    // picks WY from the GPU's storage limit, and LIFT is 0 below 384) a tall berg's predicted crown is above a
    // ceiling the ice never actually reaches. Un-clamped, the search would stand the player in the air over it.
    const spTop = (x, z) => Math.min(WY - 1, arctIceTop(x, z));
    const bench = (x, z, ty) => {
      for (let bz = -ARCT_SPBENCH; bz <= ARCT_SPBENCH; bz += ARCT_SPBENCH) for (let bx = -ARCT_SPBENCH; bx <= ARCT_SPBENCH; bx += ARCT_SPBENCH)
        if (bx || bz) { const t = spTop(x + bx, z + bz); if (t < 0 || Math.abs(t - ty) > ARCT_SPTOL) return false; }   // t < 0 is open water: an edge column, and the player's box would hang over it
      return true;
    };
    const sweep = (cx, cz, r, step) => {
      let best = null, bs = -Infinity;
      for (let dz = -r; dz <= r; dz += step) {
        SPWZ = cz + dz;                                // the row's own latitude IS the anchor while the row is scored ─ see the note above
        for (let dx = -r; dx <= r; dx += step) {
          const x = cx + dx, ty = spTop(x, SPWZ);
          if (ty < WL + ARCT_SPMIN || nearCave(x, SPWZ)) continue;
          const s = ty - (Math.abs(x - xc) + Math.abs(SPWZ - zc)) * 0.01;   // a mild pull back toward the middle, so a peak one voxel taller a kilometre away cannot win on a rounding error
          if (s > bs && bench(x, SPWZ, ty)) { bs = s; best = { x, z: SPWZ, y: ty }; }
        }
      }
      return best;
    };
    const coarse = sweep(xc, zc, ARCT_SPR, ARCT_SPSTEP);
    const site = coarse && (sweep(coarse.x, coarse.z, ARCT_SPSTEP >> 1, ARCT_SPFINE) || coarse);
    if (site) {
      SPOX = site.x - SPWX; SPWZ = site.z; SPY = site.y;   // …and TELL sim/player.js the y, rather than leave it to work out: hmap is the seabed under the ice, and the obvious alternative — scan W down for the first solid voxel — lands on the PENGUIN worldgen stamped on the summit. See SPY in world/window.js
      console.log('[vb] spawn ARCTIC summit', site.x - xc, site.z - zc, 'from the band centre ─ standing at y', site.y, '(' + (site.y - WL) + ' over the water)');
    } else {                                           // no ice anywhere in the search stood high enough. Spawning is NEVER allowed to fail, so fall back to the walk this replaced and take dry land wherever it is
      SPWZ = zc;
      let g = 0; while ((H(SPWX + SPOX, SPWZ) <= WL + 6 || nearCave(SPWX + SPOX, SPWZ)) && g++ < 400) SPOX += 16;
      // That walk goes EAST in 16s and has a half-strip to cross before it would leave the band, so it gets a
      // band test too: if it carried the player out, walk back WEST from the centre instead. Without it a spawn
      // over water on the east half could deposit the player in the pine strip, which is the exact bug the
      // "walking SPWX cannot work" note above is about.
      if (SPBANDM(SPWX + SPOX, SPWZ) <= 0) { SPOX = SPBAND; g = 0; while ((H(SPWX + SPOX, SPWZ) <= WL + 6 || nearCave(SPWX + SPOX, SPWZ)) && g++ < 400) SPOX -= 16; }
    }
  }
  winOX = Math.round((SPWX + SPOX) / 32) * 32 - HALF; winOZ = Math.round(SPWZ / 32) * 32 - HALF;   // 32-ALIGNED origin — the L2 occupancy wrap needs off % 32 == 0

