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
    SPWX = Math.round((Math.random() - 0.5) * 400000);   // ── RANDOM SPAWN each refresh (user) ── land anywhere in a ±20 km span; the capped nudge below walks east to the first valid dry, cave-free ground. lakeSpawn() above is left defined but unused (easy re-enable).
    SPWZ = Math.round((Math.random() - 0.5) * 400000);
    console.log('[vb] spawn RANDOM', SPWX, SPWZ);
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
  // BIRCHC = BAND_MIRROR * (BIRCHOFF + BIRCHH) = -(1080 + 1080), pure strip -3240 .. -1080. It was +2160,
  // the middle of the oak strip.
  SPOX = -2160;
  { let g = 0; while ((H(SPWX + SPOX, SPWZ) <= WL + 6 || nearCave(SPWX + SPOX, SPWZ)) && g++ < 400) SPOX += 16; }
  // The guard above walks EAST in 16s, and from the band centre it has 1080 of birch to cross before it would
  // leave the strip. So it is given a birch test as well: if the walk carried the player out of the band, walk
  // back WEST from the centre instead. Without it a spawn over a lake on the east half could deposit the
  // player in the pine strip, which is the exact bug the "walking SPWX cannot work" note is about.
  if (birchM(SPWX + SPOX, SPWZ) <= 0) {
    SPOX = -2160;
    let g = 0;
    while ((H(SPWX + SPOX, SPWZ) <= WL + 6 || nearCave(SPWX + SPOX, SPWZ)) && g++ < 400) SPOX -= 16;
  }
  winOX = Math.round((SPWX + SPOX) / 32) * 32 - HALF; winOZ = Math.round(SPWZ / 32) * 32 - HALF;   // 32-ALIGNED origin — the L2 occupancy wrap needs off % 32 == 0

