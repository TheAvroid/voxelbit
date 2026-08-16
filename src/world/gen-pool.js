  // ── GEN POOL ── the ENTIRE generator (terrain sweep + every stamp pass) runs on a pool of worker threads.
  // Each worker is built from THE SAME function sources (fn.toString() — bit-exact by construction, like the row
  // worker above) and fills a private region slab it transfers back; the main thread's only remaining work is a
  // straight memcpy into W/hmap. Slabs use ORIGIN-SHIFTED indexing in place of the toroidal wrap (the stride is
  // padded when sx === sz so gwrap's axis test stays unambiguous). Boot fans all bands across every core;
  // streaming bands are chunked along their long axis and prefetched one band ahead. If workers are unavailable
  // or die, every call site falls back to the identical inline path. `?nopool` forces inline for A/B testing.
  const ORPH = { slabs: 0, swept: 0, skipped: 0, cut: 0, seeded: 0, tiles: 0, tileCut: 0 };   // generation orphan sweep telemetry
  // ── THE MAIN-THREAD REGEN PATH ── a long teleport rebuilds the whole rect here rather than through the
  // slab pool, so the worker's sweep never sees it. The rect is up to 448x448x384, far too big to flood in
  // one go, so it is queued as tiles and drained on a budget like every other deferred queue in here.
  const ORPHQ = [];
  const ORPH_TILE = 48;
  const orphQueueRect = (x0, x1, z0, z1) => {
    for (let z = z0; z < z1; z += ORPH_TILE) for (let x = x0; x < x1; x += ORPH_TILE)
      ORPHQ.push(x, Math.min(x1, x + ORPH_TILE), z, Math.min(z1, z + ORPH_TILE));
  };
  const orphDrain = (ms) => {
    if (!ORPHQ.length) return;
    const t0 = performance.now();
    while (ORPHQ.length && performance.now() - t0 < ms) {
      const z1 = ORPHQ.pop(), z0 = ORPHQ.pop(), x1 = ORPHQ.pop(), x0 = ORPHQ.pop();
      const r = sweepOrphans(x0, x1, z0, z1);
      if (!r) continue;                            // no gorge in this tile
      ORPH.tiles++; ORPH.tileCut += r.cut;
      for (let q = 0; q < r.seeds.length; q += 3) {   // ambiguous at a tile edge -> let the resolver decide
        const ii = gwrap(r.seeds[q], WX) + r.seeds[q + 1] * WX + gwrap(r.seeds[q + 2], WZ) * WX * WY;
        if (W[ii]) { supPush(ii); ORPH.seeded++; }
      }
      if (r.gone.length) {                           // publish exactly the voxels that went, not the whole world
        const patch = [];
        for (let q = 0; q < r.gone.length; q += 3) patch.push(gwrap(r.gone[q], WX) + r.gone[q + 1] * WX + gwrap(r.gone[q + 2], WZ) * WX * WY);
        gpuPatch(patch, false);
      }
    }
  };
  let genPool = null, poolOk = false, poolWait = false, poolSeq = 0;
  // ── SLAB-READY DOORBELL ── buildWorld used to wait on `setTimeout(…, 2)`, which Chrome clamps once the
  // chain is a few levels deep: measured 181 sleeps averaging 4.78 ms for a 2 ms request. With the workers
  // ~89% saturated that is mostly honest waiting, but it also means every finished slab sits unblitted for
  // up to a clamp tick, and those add up over 512 of them. Wake on the message instead. The 20 ms fallback
  // exists only so a dropped message can never hang the boot — the doorbell is what normally fires.
  let poolWake = null;
  const poolNotify = () => { if (poolWake) { const f = poolWake; poolWake = null; f(); } };
  const poolAwait = () => new Promise((r) => { poolWake = r; setTimeout(poolNotify, 20); });
  const jobById = new Map(), poolQueue = [], regionJobs = new Map();
  const rgnKey = (x0, x1, z0, z1) => x0 + ',' + x1 + ',' + z0 + ',' + z1;
  if (!location.search.includes('nopool')) try {
    const consts = { SHRUB_ON, SPYAW, SPVIEW_D, SPVIEW_W, WY, LIFT, WL, HMAX, RIVCELL, RIVINF, ROCKSTEP, DECOR_MIN, TCELL, CACCELL, DRCELL, SHCELL, TMARGIN, CAVE_CELL, CAVE_MARGIN, CAVE_WMAX, CAVE_FLOOR_MAX, OCELL, BCELL, F2CELL, MUCELL, PCCELL, SCELL, LGCELL, LILYCELL, LGIGCELL, MSX, MSY, MSZ, SPWX, SPWZ, DESOFF, DESB, DESW, DESY, DESREL, DESDUNE, WATER_T, WATER_B, LAVA_T, LAVA_B, LAVA_R, LAVA_Y, STICK_S, STICK_M };
    const tables = { NEEDLE, MOSS, DIRT, DSAND, ROCK, ROCKX, BROCK, SHRUBC, SHRUBF, SAND, ORECOAL, OREIRON, OREGOLD, ORECRYS, GRASS, PEBBLE, BLOOM, FERN2V, MUSHV, LILYPAD_GIGV, CONEV, CONEVL, LILYV, STICKV, LOGV, ROCKV, ROCKVU, ROCK26, R26DMAP, REDROCK, CACTI, SHRUBV, DROCK, DROCKS, DROCKM, DROCKB, R26S, R26M, R26B, PINE_ANCH };
    const fns = { ihash, sstep, vnoise, vnoise3, fbm, baseH, basinM, riverAt, rivEval, gatherRivers, riverS, desWob, desertM, duneH, H, groundMin, rockSeatY, rowNoise, makeHRow, makeMossRow, colNoise, makeHCol, makeMossCol, fillColumn, rockRowSpan, stampModel, boulderAt, stampBoulder, cactusAt, stampCactus, drockAt, stampDrock, shrubAt, stampShrub, caveAt, caveHitsBox, stampCave, nearCave, oreAt, stampOre, fern2At, stampFern2, mushAt, stampMush, pconeAt, stampPcone, stickAt, stampStick, logAt, stampLog, lilyAt, stampLily, lilyGigAt, stampLilyGig, treeAt, stampTree, treesInRegion, stampCellsGen, genRegionGen, genRegion, sweepOrphans };
    let wsrc2 = '';
    for (const k in consts) wsrc2 += 'const ' + k + ' = ' + consts[k] + ';\n';
    for (const k in tables) wsrc2 += 'const ' + k + ' = ' + JSON.stringify(tables[k]) + ';\n';
    wsrc2 += 'let WX = 0, WZ = 0, OX = 0, OZ = 0, BX = 0, W = null, hmap = null, touched = null, MROT = null, remap = null, rivScope = null;\n' +
      'const gwrap = (v, n) => v - (n === WX ? OX : OZ);\n' +
      'const rivCache = new Map(), caveCache = new Map();\n' +
      'const takeRows = () => null;\n';
    // (ROCK26D — the Colorado-sandstone twin of ROCK26 — is no longer derived here: the desert rocks went
    //  back to stock grey, so nothing in the worker references it. bow.js still builds it and R26DMAP is
    //  still registered, so restoring is re-adding one line:
    //  wsrc2 += 'const ROCK26D = ROCK26.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (R26DMAP[p >>> 24] << 24)) }));' + String.fromCharCode(10);)
    wsrc2 += 'const ORPH_SCRATCH = { mark: null, stk: null };\n';   // sweepOrphans reuses these; the worker needs its own copy
    wsrc2 += 'const ORPHAN_OK = new Uint8Array(' + JSON.stringify(Array.from(ORPHAN_OK)) + ');\n';
    for (const k in fns) wsrc2 += 'const ' + k + ' = ' + fns[k].toString() + ';\n';
    wsrc2 += 'onmessage = (e) => {\n' +
      '  const d = e.data;\n' +
      '  if (d.init) { MROT = d.MROT; remap = d.remap; return; }\n' +
      '  const sx = d.x1 - d.x0, sz = d.z1 - d.z0;\n' +
      '  WX = sx === sz ? sx + 8 : sx; WZ = sz; OX = d.x0; OZ = d.z0; BX = WX >> 3;\n' +
      '  W = new Uint8Array(WX * WY * WZ); hmap = new Int16Array(WX * WZ);\n' +
      '  touched = new Uint8Array(((WX >> 3) + 1) * ((WZ >> 3) + 2));\n' +
      '  genRegion(d.x0, d.x1, d.z0, d.z1, true);\n' +
      '  const orph = sweepOrphans(d.x0, d.x1, d.z0, d.z1);\n' +
      '  const nbx = sx >> 3, nbz = sz >> 3, nby = WY >> 3;\n' +               // ── SLAB BRICK BITS ── the 8³ occupancy scan runs HERE, in parallel, instead of on the main thread after the blit
      '  const bb = new Uint32Array(((nbx * nby * nbz) + 31) >> 5);\n' +
      '  const wb = new Uint32Array(((nbx * nby * nbz) + 31) >> 5);\n' +       // parallel WATER-ONLY bits (skipW brick striding)
      '  const W32b = new Uint32Array(W.buffer);\n' +
      '  for (let bz = 0; bz < nbz; bz++) for (let bx = 0; bx < nbx; bx++) {\n' +
      '    let maxH = 0, cav = 0;\n' +
      '    for (let z = bz * 8; z < bz * 8 + 8; z++) for (let x = bx * 8; x < bx * 8 + 8; x++) { const hv = hmap[x + z * WX]; if (hv > maxH) maxH = hv; if (hv <= CAVE_FLOOR_MAX) cav = 1; }\n' +
      '    if (cav && maxH < HMAX) maxH = HMAX;\n' +                           // GORGE TILE: hmap is the carved floor, but stampCave's wall jag leaves wall standing to the pristine surface — cap off the terrain ceiling instead or the intact wall is force-cleared and rays pass straight through it. Identical to rebuildBricks in terrain.js; __vb.gtest diffs the two.
      '    const byCap = Math.min(nby, ((maxH + 122) >> 3) + 1);\n' +          // sky-cap: nothing exists above terrain + the tallest pine
      '    for (let by = 0; by < byCap; by++) {\n' +
      '      let occ = 0;\n' +
      '      scan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {\n' +
      '        const rw = (y * WX + z * WX * WY + bx * 8) >> 2;\n' +
      '        if (W32b[rw] | W32b[rw + 1]) { occ = 1; break scan; }\n' +
      '      }\n' +
      '      if (occ) { const b = bx + by * nbx + bz * nbx * nby; bb[b >> 5] |= 1 << (b & 31);\n' +
      '        if (by * 8 <= WL) { let wonly = 1;\n' +
      '          wscan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {\n' +
      '            const r0 = y * WX + z * WX * WY + bx * 8;\n' +
      '            for (let x = 0; x < 8; x++) { const v = W[r0 + x]; if (v !== 0 && v !== WATER_T && v !== WATER_B) { wonly = 0; break wscan; } }\n' +
      '          }\n' +
      '          if (wonly) wb[b >> 5] |= 1 << (b & 31);\n' +
      '        }\n' +
      '      }\n' +
      '    }\n' +
      '  }\n' +
      '  postMessage({ id: d.id, stride: WX, W, hmap, bb, wb, nbx, nby, nbz, orph }, [W.buffer, hmap.buffer, bb.buffer, wb.buffer]);\n' +
      '  W = hmap = touched = null;\n' +
      '};';
    if (location.search.includes('wsrc')) window.__vbWSRC = wsrc2;   // ?wsrc — hand the assembled worker source out so a syntax error in it can be located instead of guessed at
    const purl = URL.createObjectURL(new Blob([wsrc2], { type: 'text/javascript' }));
    const NPOOL = Math.max(2, Math.min(16, (navigator.hardwareConcurrency || 4) - 4));   // cap 8 -> 12 -> 20 (user 2026-08-07: make the initial load faster). Boot is dominated by worldgen: measured 9.34 s of a 10.54 s load, with assets/bricks/upload together under 1.2 s. On a 28-thread box the old min(12, hc-2) left more than half the machine idle. hc-4 keeps headroom for the main thread, the row worker and the browser.
    genPool = [];
    for (let i = 0; i < NPOOL; i++) {
      const w = new Worker(purl);
      w.busyId = 0;
      w.onmessage = (e) => { w.busyId = 0; if (e.data.orph !== undefined) { ORPH.slabs++; if (!e.data.orph) ORPH.skipped++; else { ORPH.swept++; ORPH.cut += e.data.orph.cut; } } const j = jobById.get(e.data.id); if (j) { j.msg = e.data; j.done = true; } poolPump(); poolNotify(); };
      w.onerror = (e) => { console.warn('[vb] gen pool error — inline generation', e.message || e); poolOk = false; };
      w.postMessage({ init: 1, MROT, remap });
      genPool.push(w);
    }
    URL.revokeObjectURL(purl);                         // every worker is constructed by now — drop the blob (see genWorker above)
    poolOk = true;
    console.log('[vb] gen pool:', NPOOL, 'workers');
  } catch (e) { console.warn('[vb] gen pool unavailable — inline generation', e); }
  function poolPump() {                                // hand queued chunks to idle workers, FIFO
    if (!poolOk) return;
    for (const w of genPool) {
      if (w.busyId) continue;
      const j = poolQueue.shift(); if (!j) return;
      w.busyId = j.id;
      w.postMessage({ id: j.id, x0: j.x0, x1: j.x1, z0: j.z0, z1: j.z1 });
    }
  }
  function poolChunks(x0, x1, z0, z1) {                // fan one region across the pool — 8-aligned splits along the LONGER axis. Splitting the long axis keeps chunks
    const jobs = [];                                   // CHUNKY (256×32) instead of SLIVERS (2048×8): a tree/rock model overlapping a chunk border re-iterates its whole
    const push = (a, b, c, d) => { const j = { id: ++poolSeq, x0: a, x1: b, z0: c, z1: d, done: false, msg: null }; jobById.set(j.id, j); poolQueue.push(j); jobs.push(j); };   // sparse vox list in BOTH chunks, so fewer/shorter borders = less redundant stamp work per band
    if (x1 - x0 >= z1 - z0) { const step = Math.max(64, Math.ceil((x1 - x0) / genPool.length / 8) * 8);
      for (let x = x0; x < x1; x += step) push(x, Math.min(x1, x + step), z0, z1);
    } else { const step = Math.max(64, Math.ceil((z1 - z0) / genPool.length / 8) * 8);
      for (let z = z0; z < z1; z += step) push(x0, x1, z, Math.min(z1, z + step));
    }
    poolPump();
    return jobs;
  }
  function poolFree(key) {
    const R = regionJobs.get(key); if (!R) return;
    regionJobs.delete(key);
    for (const j of R) { jobById.delete(j.id); j.msg = null; const qi = poolQueue.indexOf(j); if (qi >= 0) poolQueue.splice(qi, 1); }
  }
  function poolRegion(x0, x1, z0, z1) {                // idempotent band dispatch — genBandGen prefetches the NEXT band through this
    const key = rgnKey(x0, x1, z0, z1);
    let R = regionJobs.get(key);
    if (!R) {
      if (regionJobs.size > 3) poolFree(regionJobs.keys().next().value);   // drop stale mispredictions
      R = poolChunks(x0, x1, z0, z1);
      regionJobs.set(key, R);
    }
    return R;
  }
  const W64 = new Float64Array(W.buffer);
  function blitSlab(j) {                               // memcpy one finished slab into the toroidal window: W + hmap + touched
    const m = j.msg, S = m.W, HM = m.hmap, st = m.stride, S64 = new Float64Array(S.buffer);
    const segs = [];                                   // x runs contiguous in BOTH slab and window (all x extents are 8-aligned)
    for (let a = j.x0; a < j.x1;) { const g = gwrap(a, WX), len = Math.min(j.x1 - a, WX - g); segs.push([a - j.x0, g, len]); a += len; }
    for (let z = j.z0; z < j.z1; z++) {
      const gz = gwrap(z, WZ), hs = (z - j.z0) * st, hd = gz * WX;
      for (const [o, g, len] of segs) hmap.set(HM.subarray(hs + o, hs + o + len), hd + g);
      const zs = (z - j.z0) * st * WY, zd = gz * WX * WY;
      for (const [o, g, len] of segs) {
        if (len >= 128) { for (let y = 0; y < WY; y++) { const s0 = zs + y * st + o; W.set(S.subarray(s0, s0 + len), zd + y * WX + g); } }
        else { const n = len >> 3;                     // narrow runs (8-wide x-bands): whole-f64 stores skip ~790k subarray allocs per band
          for (let y = 0; y < WY; y++) { const s8 = (zs + y * st + o) >> 3, d8 = (zd + y * WX + g) >> 3;
            for (let k = 0; k < n; k++) W64[d8 + k] = S64[s8 + k]; } }
      }
    }
    for (let z = j.z0 & ~7; z < j.z1; z += 8) { const tr = (gwrap(z, WZ) >> 3) * BX;   // blitted memory is no longer virgin
      for (let x = j.x0; x < j.x1; x += 8) touched[(gwrap(x, WX) >> 3) + tr] = 1;
    }
    if (m.bb) {                                        // ── WORKER BRICK MERGE ── the slab's 8³ occupancy was scanned IN the worker; copy its bits into the global tables (chunks are 8-aligned, so every brick belongs wholly to this slab)
      const nbx = m.nbx, nby = m.nby, nbz = m.nbz, bb = m.bb, wb = m.wb;
      for (let bz = 0; bz < nbz; bz++) { const gbz = gwrap(j.z0 + bz * 8, WZ) >> 3;
        for (let bx = 0; bx < nbx; bx++) { const gbx = gwrap(j.x0 + bx * 8, WX) >> 3;
          for (let by = 0; by < nby; by++) {
            const s = bx + by * nbx + bz * nbx * nby, g = gbx + by * BX + gbz * BX * BY;
            if ((bb[s >> 5] >>> (s & 31)) & 1) bricks[g >> 5] |= 1 << (g & 31); else bricks[g >> 5] &= ~(1 << (g & 31));
            if (wb && ((wb[s >> 5] >>> (s & 31)) & 1)) wbricks[g >> 5] |= 1 << (g & 31); else wbricks[g >> 5] &= ~(1 << (g & 31));
          } } }
    }
  }
  function* genRegionThreaded(x0, x1, z0, z1) {        // pool-generated region; falls back to the identical inline sweep if the pool dies or stalls. Returns TRUE when the pooled path ran (slab brick bits already merged — only the L2 rebuild remains for the caller)
    if (poolOk) {
      const key = rgnKey(x0, x1, z0, z1), jobs = poolRegion(x0, x1, z0, z1);
      let spin = 0;
      while (poolOk && spin < 1800 && jobs.some(jb => !jb.done)) { spin++; poolWait = true; yield; }
      poolWait = false;
      if (jobs.every(jb => jb.done)) { for (const jb of jobs) { blitSlab(jb); yield; } poolFree(key); return true; }
      poolFree(key);                                   // dead or stalled — regenerate the whole region inline (deterministic, so overlap is harmless)
    }
    reqRows(x0, x1, z0, z1);
    for (let spin = 0; spin < 90 && rowsPending(x0, x1, z0, z1); spin++) { yield; }
    yield* genRegionGen(x0, x1, z0, z1, false);
    return false;
  }
  await buildWorld(true);
  // (cardinal is not stamped into the world — it's an off-grid DDA model driven per-frame in the tick loop; see "flying cardinal → drop slot 4")
  console.log('[vb] world built at', winOX, winOZ, 'spawn', SPWX, SPWZ);

