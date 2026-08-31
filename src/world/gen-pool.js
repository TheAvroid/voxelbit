  // ── GEN POOL ── the ENTIRE generator (terrain sweep + every stamp pass) runs on a pool of worker threads.
  // Each worker is built from THE SAME function sources (fn.toString() — bit-exact by construction, like the row
  // worker above) and fills a private region slab it transfers back; the main thread's only remaining work is a
  // straight memcpy into W/hmap. Slabs use ORIGIN-SHIFTED indexing in place of the toroidal wrap (the stride is
  // padded when sx === sz so gwrap's axis test stays unambiguous). Boot fans all bands across every core;
  // streaming bands are chunked along their long axis and prefetched one band ahead. If workers are unavailable
  // or die, every call site falls back to the identical inline path. `?nopool` forces inline for A/B testing.
  const ORPH = { slabs: 0, swept: 0, skipped: 0, cut: 0, seeded: 0, tiles: 0, tileCut: 0, wblit: 0 };   // wblit: slabs the WORKER wrote into the shared world itself   // generation orphan sweep telemetry
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
    // ── THE OPACITY TABLE THE FAR RING SEALS AGAINST ── the same one render/buffers.js derives, serialised so
    // the airless scan can run on the worker instead of the main thread (see the ab bitmask below). Reading
    // foliaTab HERE is safe and not by luck: every write to it lives in assets/palette.js and
    // assets/material-tabs.js, manifest lines 17 and 20, and this file is line 26. If a decoration is ever
    // given foliage flags LATER than this point, this copy goes stale and far bricks holding that foliage seal
    // into solid stone — so a new markFolia() belongs in material-tabs.js with the rest, not wherever it is
    // convenient.
    const OPAQTAB = []; for (let i = 0; i < 256; i++) OPAQTAB.push(i && i !== WATER_T && i !== WATER_B && !foliaTab[i] ? 1 : 0);
    const consts = { CANOPY, BIRCHNB, WOB_DES1, WOB_DES2, WOB_OAK, WOB_CH, SHRUB_ON, PETAL_ON, SPYAW, SPVIEW_D, SPVIEW_W, WY, LIFT, WL, HMAX, RIVLAND, RIVCELL, RIVINF, RIVNEAR_CAP, ROCKSTEP, DECOR_MIN, TCELL, CACCELL, DRCELL, SHCELL, TMARGIN, CAVE_CELL, CAVE_MARGIN, CAVE_WMAX, CAVE_FLOOR_MAX, OCELL, BCELL, F2CELL, MUCELL, FLWCELL, FLWPATCH, BLOSCHERRY, PCCELL, SCELL, LGCELL, LILYCELL, LGIGCELL, MSX, MSY, MSZ, SPWX, SPWZ, SPOX, BIOP, DESOFF, DESB, DESW, DESC, DESH, DESY, DESREL, DESDUNE, OAKOFF, OAKB, OAKW, OAKY, OAKHILL, OAKFAR, OAKNEAR, OAKWOFF, OAKC, OAKH, OAKWFAR, OAKWNEAR, OAKBANKR, OAKBANKY, OAKBRISE, OAKBEACH, OAKBEACHY, BKCELL, BK_BOLE, BK_LEAN, BK_SPAWN, BKMARGIN, BKHIVE, BIRCHOFF, BIRCHB, BIRCHH, BIRCHC, BIRCHWMAX, BIRCHFAR, BIRCHWFAR, BASIN_T, BASIN_ARCT, BASIN_LOW, BASIN_ARCTLIFT, ARCT_SNOWR, ARCT_SNOWN, ARCT_FLOE, ARCT_FLOE_RIV, ARCT_FLOEH, ARCT_FLOESPAN, ARCT_FLOEF, ARCT_SEA, ARCT_SEAREL, ARCT_SEAF, ARCT_SEAF2, ARCT_SEAMIX, ARCT_SEAPOW, ARCT_DOMEC, ARCT_DOMEK, ARCT_GBLEND, ARCT_STAND, ARCT_KEELK, ARCT_STEP, ARCT_JAG, ARCT_JAGF, ARCT_JAG2, ARCT_JAGF2, ARCT_CREVF1, ARCT_CREVF2, ARCT_CREVT, ARCT_CREVW, ARCT_CREVD, ARCT_CREVK, ARCT_ICEF, ARCT_ICE, ARCT_CAPMIN, ARCT_CAPMAX, ARCT_CAPHF, ARCT_CAPIRR, ARCT_CAPIRRF, ARCT_CAPIRR2, ARCT_CAPIRRF2, PENGCELL, PENG_RATE, PENG_INNER, PENG_MIN, PENG_MAX, PENG_SPACE, PENG_JIT, PENG_MINTOP, PENG_CHICK, PENG_CHICKR, ARCTOFF, ARCTB, ARCTH, ARCTC, ARCTWMAX, ARCTFAR, ARCTWFAR, ARCTIC_SNOW, ARCT_BARE, ARCT_GROUND, OKCELL, OKMARGIN, OKVIEW_W, OKFRUIT, OKHIVE, CHOFF, CHHALF, CHWHALF, CHB, CHW, CHREACH, WATER_T, WATER_B, LAVA_T, LAVA_B, LAVA_R, LAVA_Y, STICK_S, STICK_M, PINEY, PINEHILL, PINE_LAKE, PINEBEACH, PINERISE, BIORW, BIORSAT, BIORIV_ON, BIORVALL, BIORVK };
    const tables = { ASNOW, PENGUINV, PENGBABYV, NEEDLE, MOSS, BIRCHMOSS, BIRCHENC, BIRCHIDS, BIRCHBARK, DIRT, DSAND, ROCK, ROCKX, BROCK, SHRUBC, SHRUBF, BLOSLEAF, BLOSWHITE, OAKMOSS, TWIGPINK, TWIGWHITE, SAND, ORECOAL, OREIRON, OREGOLD, ORECRYS, GRASS, PEBBLE, FLOWERV, FLOWERV_CH, FERN2V, MUSHV, LILYPAD_GIGV, CONEV, CONEVL, LILYV, STICKV, STICKB, STICKBIRCH, LOGV, ROCKV, ROCKVU, ROCK26, R26DMAP, REDROCK, CACTI, SHRUBV, DROCK, DROCKS, DROCKM, DROCKB, R26S, R26M, R26B, PINE_ANCH, OAKV, OAK_ANCH, OAK_BANCH, FRUITV, HIVEV, BLOSRANK, OAKLITER, OPAQTAB };   // BLOSRANK replaces BLOSMAP/BLOSMAPW: one rank table, the ramp is an argument (assets/bow.js)   // BLOSMAPW rides beside BLOSMAP, where the pink map was already registered   // OAKLITER: the LIGHT green oak variety's 4-step ramp — four numbers, and the same argument BLOSLEAF/BLOSWHITE are (the derived MODELS are rebuilt below, never shipped)
    const fns = { ihash, sstep, pwrap, vnoise, vnoise3, fbm, baseH, basinM, riverAt, rivEval, gatherRivers, riversNear, riverS, bankEval, bankDist, desWob, desertM, birchM, arcticM, bioPin, bioWobZ, bioEdge, bioRivS, basinT, basinLow, arctSnow, arctGB, arctSeaH, arctH, arctDome, arctRidge, arctCliff, arctIceTop, arctBare, oakWob, oakM, pineH, chWob, cherryM, chNear, oakH, oakRoll, oakBank, duneH, H, groundMin, rockSeatY, rowNoise, makeHRow, makeMossRow, colNoise, makeHCol, makeMossCol, fillColumn, rockRowSpan, stampModel, boulderAt, stampBoulder, cactusAt, stampCactus, drockAt, stampDrock, shrubAt, stampShrub, caveAt, caveHitsBox, stampCave, nearCave, oreAt, stampOre, fern2At, stampFern2, mushAt, flowerAt, stampFlower, blosRemap, mossCap, stampMush, pconeAt, stampPcone, stickAt, stampStick, logAt, stampLog, oakAt, stampOak, stampBirch, birchAt, birchPick, birchBanch, birchTrunkW, birchTrunkC, birchDec, hiveAt, lilyAt, stampLily, lilyGigAt, stampLilyGig, treeAt, stampTree, penguinAt, stampPenguin, stampPenguinOne, treesInRegion, stampCellsGen, genRegionGen, genRegion, sweepOrphans };
    let wsrc2 = '';
    for (const k in consts) wsrc2 += 'const ' + k + ' = ' + consts[k] + ';\n';
    for (const k in tables) wsrc2 += 'const ' + k + ' = ' + JSON.stringify(tables[k]) + ';\n';
    wsrc2 += 'let GW = null, GW64 = null, GWX = 0, GWZ = 0;\n' +
      'let WX = 0, WZ = 0, OX = 0, OZ = 0, BX = 0, W = null, hmap = null, touched = null, MROT = null, remap = null, rivScope = null;\n' +
      'const gwrap = (v, n) => v - (n === WX ? OX : OZ);\n' +
      'const rivCache = new Map(), caveCache = new Map();\n' +
      'const rivNear = new Map();\n' +   // riversNear's store — every top-level the registered fns close over must be declared here too
      'let bpZ = null, bpD = 0, bpO = 0, bpC = 0, bwZ = null, bwD = 0, bwO = 0, bwC = 0, bwM = 0, bwW = 0;\n' +   // …and the border rivers' two 1-entry memos (see BIOME BORDER RIVERS in world/window.js), for the same reason
      'const takeRows = () => null;\n';
    // (ROCK26D — the Colorado-sandstone twin of ROCK26 — is no longer derived here: the desert rocks went
    //  back to stock grey, so nothing in the worker references it. bow.js still builds it and R26DMAP is
    //  still registered, so restoring is re-adding one line:
    //  wsrc2 += 'const ROCK26D = ROCK26.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (R26DMAP[p >>> 24] << 24)) }));' + String.fromCharCode(10);)
    // ── THE PINK CROWNS ARE REBUILT HERE, NOT SHIPPED ── the line the ROCK26D note above leaves as a recipe,
    // finally used for what it describes. OAKV is 218,367 voxels and handing each worker a second copy is what
    // stopped the boot completing the last time somebody shipped a derived model set. BLOSRANK is 256 numbers.
    wsrc2 += 'const ORPH_SCRATCH = { mark: null, stk: null };\n';   // sweepOrphans reuses these; the worker needs its own copy
    wsrc2 += 'const ORPHAN_OK = new Uint8Array(' + JSON.stringify(Array.from(ORPHAN_OK)) + ');\n';
    for (const k in fns) wsrc2 += 'const ' + k + ' = ' + fns[k].toString() + ';\n';
    // ── THESE TWO MOVED BELOW THE fns LOOP (2026-08-18) ── they used to be inline id->id remaps that referenced
    // nothing but a table, so emitting them here was fine. They call blosRemap now, and every fn is emitted as a
    // `const` AFTER this point — so at worker startup the call would land in blosRemap's temporal dead zone and
    // throw ReferenceError before a single region generated. Anything here that calls a fn must come after them.
    wsrc2 += 'const OAKBLOSV = OAKV.map((m) => blosRemap(m, BLOSLEAF, BLOSRANK, BLOSCHERRY));' + String.fromCharCode(10);
    // ── THE BIRCH MODELS ARE REBUILT HERE, NOT SHIPPED ── the same move OAKBLOSV makes just above, and for a
    // far larger reason: BIRCHV is ~205k voxels, so JSON.stringifying it into this source would be megabytes
    // of JavaScript that every one of NPOOL workers parses. BIRCHENC is the delta-varint the loader already
    // produced, and birchDec turns it back into typed arrays per worker. BIRCHPICK is derived from it for the
    // same reason. Both are listed in tools/lint-vb.py's WORKER_DECLS, which is how check 9 knows they exist.
    wsrc2 += 'const BIRCHV = BIRCHENC.map(birchDec);' + String.fromCharCode(10);
    wsrc2 += 'const BIRCHPICK = birchPick(BIRCHV);' + String.fromCharCode(10);
    wsrc2 += 'const BIRCH_BANCH = birchBanch(BIRCHV);' + String.fromCharCode(10);   // …and the hive anchors, derived per worker for the same reason (assets/bow.js)
    wsrc2 += 'const OAKWHITV = OAKV.map((m) => blosRemap(m, BLOSWHITE, BLOSRANK, BLOSCHERRY));' + String.fromCharCode(10);   // the WHITE crowns, same recipe and the same rank table — another 256 bytes rather than another 218,367 voxels
    wsrc2 += 'const OAKLITEV = OAKLITER.length ? OAKV.map((m) => blosRemap(m, OAKLITER, BLOSRANK, 0)) : [];' + String.fromCharCode(10);   // …and the LIGHT GREEN crowns, third of three, same recipe and the same rank table. The 0 is blosRemap's fruit argument: the cherry scatter is blossom-only, so a green oak must not grow cherries out of its own leaves
    wsrc2 += 'onmessage = (e) => {\n' +
      '  const d = e.data;\n' +
      '  if (d.init) { MROT = d.MROT; remap = d.remap;\n' +
      '    if (d.gW) { GW = new Uint8Array(d.gW); GW64 = new Float64Array(d.gW); GWX = d.GWX; GWZ = d.GWZ; }\n' +
      '    return; }\n' +
      '  const sx = d.x1 - d.x0, sz = d.z1 - d.z0;\n' +
      '  WX = sx === sz ? sx + 8 : sx; WZ = sz; OX = d.x0; OZ = d.z0; BX = WX >> 3;\n' +
      '  W = new Uint8Array(WX * WY * WZ); hmap = new Int16Array(WX * WZ);\n' +
      '  touched = new Uint8Array(((WX >> 3) + 1) * ((WZ >> 3) + 2));\n' +
      '  genRegion(d.x0, d.x1, d.z0, d.z1, true);\n' +
      // ══ THE SWEEP RUNS FOR THE RING TOO, BECAUSE IT MUTATES THE SLAB ══ it was skipped here on the
      // argument that nothing in the far field is supported, walked on or edited, so the sweep is pure cost out
      // there. That reads the function as if it only REPORTED orphans. It does not: it writes `W[...] = 0`,
      // deleting the floating stone a gorge carve leaves behind. Skipping it made the far ring a THIRD
      // generation path that disagrees with the other two about what the world contains — the same coordinates
      // come out WITH floating rock when the ring pages them and WITHOUT it when the near window does, so the
      // rubble pops out of existence as you walk up to a gorge. Nothing catches it either: __vb.gtest diffs the
      // pool against the INLINE sweep over the near window, and neither of those is the ring.
      // The cost argument survives intact, because the expense was never the sweep — it was its GATE passing
      // ~90% of slabs (see the boot-time note). The gate is exact now and only a slab with a gorge in it pays.
      // What the ring genuinely does not need is the RESULT: `orph` carries seeds for the support system, and
      // that stays near-window only.
      '  const orph0 = sweepOrphans(d.x0, d.x1, d.z0, d.z1);\n' +
      '  const orph = d.ring ? null : orph0;\n' +
      '  const nbx = sx >> 3, nbz = sz >> 3, nby = WY >> 3;\n' +               // ── SLAB BRICK BITS ── the 8³ occupancy scan runs HERE, in parallel, instead of on the main thread after the blit
      '  const bb = new Uint32Array(((nbx * nby * nbz) + 31) >> 5);\n' +
      '  const wb = new Uint32Array(((nbx * nby * nbz) + 31) >> 5);\n' +       // parallel WATER-ONLY bits (skipW brick striding)
      // ── AND THE AIRLESS BITS ── "is every voxel in this brick opaque" is what decides whether a brick gets
      // its own 512-byte page or joins the shared sealed one, and it was being answered on the main thread
      // after the slab arrived: 3.85 ms per far tile, measured, against 0.38 ms for the upload it was lumped in
      // with — and for the NEAR window, once per streamed band for every brick in it, inside the phase whose
      // worst frame was 22.65 ms. It is a pure function of the slab, the slab is right here, and this thread is
      // one of twelve.
      // The near window still OWNS its airFree — an edit invalidates a brick and the main thread re-decides it
      // (see poolTouch). This only seeds it at the one moment the answer is known for free and is otherwise
      // recomputed for thousands of bricks at once.
      '  const ab = new Uint32Array(((nbx * nby * nbz) + 31) >> 5);\n' +
      '  const ub = new Uint8Array(nbx * nby * nbz);\n' +
      '  const W32b = new Uint32Array(W.buffer);\n' +
      '  for (let bz = 0; bz < nbz; bz++) for (let bx = 0; bx < nbx; bx++) {\n' +
      '    let maxH = 0, cav = 0;\n' +
      '    for (let z = bz * 8; z < bz * 8 + 8; z++) for (let x = bx * 8; x < bx * 8 + 8; x++) { const hv = hmap[x + z * WX]; if (hv > maxH) maxH = hv; if (hv <= CAVE_FLOOR_MAX) cav = 1; }\n' +
      '    if (cav && maxH < HMAX) maxH = HMAX;\n' +                           // GORGE TILE: hmap is the carved floor, but stampCave's wall jag leaves wall standing to the pristine surface — cap off the terrain ceiling instead or the intact wall is force-cleared and rays pass straight through it. Identical to rebuildBricks in terrain.js; __vb.gtest diffs the two.
      '    const byCap = Math.min(nby, ((maxH + CANOPY) >> 3) + 1);\n' +          // sky-cap: nothing exists above terrain + CANOPY (world/window.js) - MUST stay identical to terrain.js rebuildBricks; __vb.gtest diffs the two
      '    for (let by = 0; by < byCap; by++) {\n' +
      '      let occ = 0;\n' +
      '      scan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {\n' +
      '        const rw = (y * WX + z * WX * WY + bx * 8) >> 2;\n' +
      '        if (W32b[rw] | W32b[rw + 1]) { occ = 1; break scan; }\n' +
      '      }\n' +
      '      if (occ) { const b = bx + by * nbx + bz * nbx * nby; bb[b >> 5] |= 1 << (b & 31);\n' +
      '        { let ok = 1;\n' +                                            // …airless AND fully opaque — the packed zero-byte test rejects any row containing air in two loads, exactly as isAirFree does
      '          ascan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {\n' +
      '            const r0 = y * WX + z * WX * WY + bx * 8, rw = r0 >> 2;\n' +
      '            const a2 = W32b[rw], c2 = W32b[rw + 1];\n' +
      '            if (((a2 - 0x01010101) & ~a2 & 0x80808080) || ((c2 - 0x01010101) & ~c2 & 0x80808080)) { ok = 0; break ascan; }\n' +
      '            for (let q = 0; q < 8; q++) if (!OPAQTAB[W[r0 + q]]) { ok = 0; break ascan; }\n' +
      '          }\n' +
      '          if (ok) ab[b >> 5] |= 1 << (b & 31);\n' +
      '        }\n' +
      '        { let uni = 1; const w0 = W32b[((by * 8) * WX + (bz * 8) * WX * WY + bx * 8) >> 2] | 0;\n' +
      '          if ((((w0 & 255) * 0x01010101) | 0) !== w0 || !(w0 & 255)) uni = 0;\n' +
      '          else uscan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {\n' +
      '            const rw = (y * WX + z * WX * WY + bx * 8) >> 2;\n' +
      '            if ((W32b[rw] | 0) !== w0 || (W32b[rw + 1] | 0) !== w0) { uni = 0; break uscan; }\n' +
      '          }\n' +
      '          if (uni) ub[b] = w0 & 255;\n' +
      '        }\n' +
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
      // ── AND THE SLAB GOES STRAIGHT INTO THE WORLD, ON THIS CORE ── the identical copy blitSlab does on the
      // main thread, run here instead: same bytes, same order, same toroidal mapping, different thread. That
      // copy is what the whole change is for — 1,900 ms of main-thread time per 20 s of sprinting, against
      // 6 ms actually spent waiting on the pool. No lock is needed because the region is PRIVATE until the
      // message below: world/stream.js reads or uploads a band only once every job reports done, and the rect
      // only expands past it after that. Both of blitSlab's paths are mirrored, narrow-run f64 stores included
      // — a band on an x side is only 8-32 wide, so the narrow path is the common one.
      // ── …UNLESS THE CALLER ASKED FOR THE SLAB ITSELF (d.ring) ── the FAR RING has no dense CPU array to
      // blit into: it is outside the toroidal window entirely, and blitting it would overwrite near terrain
      // at the wrapped address. So a ring job takes the transfer path instead and hands its private slab back
      // for render/buffers.js to page straight into the brick pool. One flag, and the generator itself is
      // untouched — the ring is the SAME world, generated the same way, going somewhere else.
      '  var blitted = 0;\n' +
      '  if (GW && !d.ring) {\n' +
      '    const gw2 = (v, n) => ((v % n) + n) % n;\n' +
      '    const segs = []; const S64 = new Float64Array(W.buffer);\n' +
      '    for (let a = d.x0; a < d.x1;) { const g = gw2(a, GWX), ln = Math.min(d.x1 - a, GWX - g); segs.push([a - d.x0, g, ln]); a += ln; }\n' +
      '    for (let z = d.z0; z < d.z1; z++) { const gz = gw2(z, GWZ);\n' +
      '      const zs = (z - d.z0) * WX * WY, zd = gz * GWX * WY;\n' +
      '      for (let si = 0; si < segs.length; si++) { const o = segs[si][0], g = segs[si][1], ln = segs[si][2];\n' +
      '        if (ln >= 128) { for (let y = 0; y < WY; y++) { const s0 = zs + y * WX + o; GW.set(W.subarray(s0, s0 + ln), zd + y * GWX + g); } }\n' +
      '        else { const n8 = ln >> 3;\n' +
      '          for (let y = 0; y < WY; y++) { const s8 = (zs + y * WX + o) >> 3, d8 = (zd + y * GWX + g) >> 3;\n' +
      '            for (let k = 0; k < n8; k++) GW64[d8 + k] = S64[s8 + k]; } }\n' +
      '      }\n' +
      '    }\n' +
      '    blitted = 1;\n' +
      '  }\n' +
      '  if (blitted) postMessage({ id: d.id, stride: WX, blitted: 1, hmap, bb, wb, ab, ub, nbx, nby, nbz, orph }, [hmap.buffer, bb.buffer, wb.buffer, ab.buffer, ub.buffer]);\n' +
      '  else postMessage({ id: d.id, stride: WX, W, hmap, bb, wb, ab, ub, nbx, nby, nbz, orph }, [W.buffer, hmap.buffer, bb.buffer, wb.buffer, ab.buffer, ub.buffer]);\n' +
      '  W = hmap = touched = null;\n' +
      '};';
    if (location.search.includes('wsrc')) window.__vbWSRC = wsrc2;   // ?wsrc — hand the assembled worker source out so a syntax error in it can be located instead of guessed at
    const purl = URL.createObjectURL(new Blob([wsrc2], { type: 'text/javascript' }));
    const NPOOL = Math.max(2, Math.min(16, (navigator.hardwareConcurrency || 4) - 4));   // cap 8 -> 12 -> 20 (user 2026-08-07: make the initial load faster). Boot is dominated by worldgen: measured 9.34 s of a 10.54 s load, with assets/bricks/upload together under 1.2 s. On a 28-thread box the old min(12, hc-2) left more than half the machine idle. hc-4 keeps headroom for the main thread, the row worker and the browser.
    genPool = [];
    for (let i = 0; i < NPOOL; i++) {
      const w = new Worker(purl);
      w.busyId = 0;
      w.onmessage = (e) => { w.busyId = 0; if (e.data.blitted) ORPH.wblit++; if (e.data.orph !== undefined) { ORPH.slabs++; if (!e.data.orph) ORPH.skipped++; else { ORPH.swept++; ORPH.cut += e.data.orph.cut; } } const j = jobById.get(e.data.id); if (j) { j.msg = e.data; j.done = true; } poolPump(); poolNotify(); };
      w.onerror = (e) => { console.warn('[vb] gen pool error — inline generation', e.message || e); poolOk = false; };
      w.postMessage(W_SHARED ? { init: 1, MROT, remap, gW: W.buffer, GWX: WX, GWZ: WZ } : { init: 1, MROT, remap });   // a SharedArrayBuffer is POSTED, never transferred — both sides address the same memory
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
      w.postMessage({ id: j.id, x0: j.x0, x1: j.x1, z0: j.z0, z1: j.z1, ring: j.ring ? 1 : 0 });
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
  // ── ONE RING TILE, ONE JOB ── deliberately NOT poolRegion/poolChunks: those exist to fan a streaming band
  // across every worker and to cache the dispatch so a prefetch can be re-asked for. A ring tile is a square
  // that one worker generates whole, it is asked for exactly once, and splitting it would only multiply the
  // slab count. Returns the job so the caller can poll j.done and take j.msg.
  function poolRingJob(x0, x1, z0, z1) {
    if (!poolOk) return null;
    const j = { id: ++poolSeq, x0, x1, z0, z1, ring: 1, done: false, msg: null };
    jobById.set(j.id, j); poolQueue.push(j); poolPump();
    return j;
  }
  function poolRingDrop(j) { if (!j) return; jobById.delete(j.id); const qi = poolQueue.indexOf(j); if (qi >= 0) poolQueue.splice(qi, 1); j.msg = null; }
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
  function blitSlab(j) {                               // merge one finished slab into the toroidal window: W + hmap + touched
    // ── THE W COPY MAY ALREADY BE DONE ── when the world grid is shared (world/window.js), the worker blits
    // its own slab straight into it and sends `blitted`, so there is no m.W here to copy and this pass has
    // only the small per-COLUMN work left: hmap, the touched tiles and the brick bits. Those are one entry per
    // column against W's WY (384) rows, which is why sharing W alone takes nearly all of the cost.
    const m = j.msg, HM = m.hmap, st = m.stride;
    const segs = [];                                   // x runs contiguous in BOTH slab and window (all x extents are 8-aligned)
    for (let a = j.x0; a < j.x1;) { const g = gwrap(a, WX), len = Math.min(j.x1 - a, WX - g); segs.push([a - j.x0, g, len]); a += len; }
    for (let z = j.z0; z < j.z1; z++) {                // hmap ALWAYS: it is one row per column and is still transferred, shared or not
      const gz = gwrap(z, WZ), hs = (z - j.z0) * st, hd = gz * WX;
      for (const [o, g, len] of segs) hmap.set(HM.subarray(hs + o, hs + o + len), hd + g);
    }
    if (!m.blitted) {                                  // …and W only when the worker could NOT write it itself
      const S = m.W, S64 = new Float64Array(S.buffer);
      for (let z = j.z0; z < j.z1; z++) {
        const gz = gwrap(z, WZ);
        const zs = (z - j.z0) * st * WY, zd = gz * WX * WY;
        for (const [o, g, len] of segs) {
          if (len >= 128) { for (let y = 0; y < WY; y++) { const s0 = zs + y * st + o; W.set(S.subarray(s0, s0 + len), zd + y * WX + g); } }
          else { const n = len >> 3;                   // narrow runs (8-wide x-bands): whole-f64 stores skip ~790k subarray allocs per band
            for (let y = 0; y < WY; y++) { const s8 = (zs + y * st + o) >> 3, d8 = (zd + y * WX + g) >> 3;
              for (let k = 0; k < n; k++) W64[d8 + k] = S64[s8 + k]; } }
        }
      }
    }
    for (let z = j.z0 & ~7; z < j.z1; z += 8) { const tr = (gwrap(z, WZ) >> 3) * BX;   // blitted memory is no longer virgin
      for (let x = j.x0; x < j.x1; x += 8) touched[(gwrap(x, WX) >> 3) + tr] = 1;
    }
    if (m.bb) {                                        // ── WORKER BRICK MERGE ── the slab's 8³ occupancy was scanned IN the worker; copy its bits into the global tables (chunks are 8-aligned, so every brick belongs wholly to this slab)
      const nbx = m.nbx, nby = m.nby, nbz = m.nbz, bb = m.bb, wb = m.wb, ab = m.ab;
      for (let bz = 0; bz < nbz; bz++) { const gbz = gwrap(j.z0 + bz * 8, WZ) >> 3;
        for (let bx = 0; bx < nbx; bx++) { const gbx = gwrap(j.x0 + bx * 8, WX) >> 3;
          for (let by = 0; by < nby; by++) {
            const s = bx + by * nbx + bz * nbx * nby, g = gbx + by * BX + gbz * BX * BY;
            if ((bb[s >> 5] >>> (s & 31)) & 1) bricks[g >> 5] |= 1 << (g & 31); else bricks[g >> 5] &= ~(1 << (g & 31));
            if (poolTouchHook) poolTouchHook(g, ab ? ((ab[s >> 5] >>> (s & 31)) & 1) : -1);   // …with the worker's airless verdict, so the pool does not re-derive it for every brick of every band
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
  // ── CONSOLE TAP: THE OAKS' FURNITURE ── berries, fruit and beehives, and where they are. It lives here rather
  // than in main/debug-api.js for one structural reason: hiveAt and oakAt belong to world/terrain.js, terrain.js
  // is a MODULE, and this fragment is the first one after it that already reaches for both — so this is the
  // cheapest honest place to hand them to a human. window.__vbRow in world/gen-worker.js sets the precedent.
  //   __vbOak.ids()            every id the feature spends and what all nine material tables say about each
  //   __vbOak.tiers()          the eight OAKV models, so you can see the bush tier became two berry bushes
  //   __vbOak.hives(x, z, r)   every beehive whose oak candidate lies in a square around a world point
  //   __vbOak.fruit(x, z, r)   …and every fruit tree, with its species and its crop
  //   __vbOak.hiveAt / oakAt   the raw cell queries, both on the SAME (cx, cz) OKCELL grid
  window.__vbOak = { hiveAt, oakAt, OKCELL, OKMARGIN,
    ids() { const row = (id, what) => ({ id, what, col: palette[id], solid: !!solidTab[id], folia: !!foliaTab[id],
        wood: !!woodTab[id], decor: !!decorTab[id], axe: !!axeOnlyTab[id], pick: !!pickOnlyTab[id],
        dig: !!digOnlyTab[id], sup: SUP.CLASS[id], orphanOK: !!ORPHAN_OK[id] });
      return { palette: { used: palette.length, free: 256 - palette.length },
        ids: [row(FRUITC[0], 'cherry + apple flesh'), row(FRUITC[1], 'blueberry'), row(FRUITC[2], 'orange flesh')]
          .concat(HIVEC.map((i, n) => row(i, n ? 'hive panels' : 'hive bands'))) }; },
    tiers() { return OAKV.map((m, i) => ({ i, sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox.length,
      berries: m.vox.reduce((a, p) => a + ((p >>> 24) === FRUITC[0] || (p >>> 24) === FRUITC[1] ? 1 : 0), 0) })); },
    hives(x, z, r) { const R = r || 512, out = [];
      for (let cz = Math.floor((z - R) / OKCELL); cz <= Math.floor((z + R) / OKCELL); cz++)
        for (let cx = Math.floor((x - R) / OKCELL); cx <= Math.floor((x + R) / OKCELL); cx++) {
          const h = hiveAt(cx, cz); if (h) out.push(h); }
      const side = R * 2 / 10;
      return { n: out.length, everyM: out.length ? +Math.sqrt(side * side / out.length).toFixed(1) : null, hives: out }; },
    fruit(x, z, r) { const R = r || 512, out = []; let oaks = 0;
      for (let cz = Math.floor((z - R) / OKCELL); cz <= Math.floor((z + R) / OKCELL); cz++)
        for (let cx = Math.floor((x - R) / OKCELL); cx <= Math.floor((x + R) / OKCELL); cx++) {
          const t = oakAt(cx, cz); if (!t) continue; oaks++;
          if (t.fn) out.push({ wx: t.wx, wz: t.wz, k: t.k, kind: FRUITV[t.fk] ? FRUITV[t.fk].name : t.fk, n: t.fn }); }
      return { oaks, trees: out.length, pct: oaks ? +(100 * out.length / oaks).toFixed(1) : 0, list: out }; } };
  await buildWorld(true);
  // (cardinal is not stamped into the world — it's an off-grid DDA model driven per-frame in the tick loop; see "flying cardinal → drop slot 4")
  console.log('[vb] world built at', winOX, winOZ, 'spawn', SPWX, SPWZ);

