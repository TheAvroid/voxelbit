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
  { let g = 0; while ((H(SPWX, SPWZ) <= WL + 6 || nearCave(SPWX, SPWZ)) && g++ < 8000) SPWX += 16; }   // never spawn in a lake or a gorge — capped so a random point over a huge basin can't loop forever   // …and never onto SAND: quicksand (tick-body.js) swallows the player on any sandTab voxel, and fillColumn lays beach/lakebed sand on every `shore` column — h <= WL + 6, plus a dithered band above. The world is not built yet at this point so there is no voxel to read, but H is analytic and it is the same condition the surface branch uses, so WL + 6 rejects exactly the columns that could come up sand.
  winOX = Math.round(SPWX / 32) * 32 - HALF; winOZ = Math.round(SPWZ / 32) * 32 - HALF;   // 32-ALIGNED origin — the L2 occupancy wrap needs off % 32 == 0

