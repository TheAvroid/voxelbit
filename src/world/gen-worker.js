  // @module - the gen worker's row-job queue - the main thread's half of the worker contract
  // @exports reqRows, rowsPending, takeRows
  // ── GEN WORKER ── the height + moss math (the heaviest part of a fill) runs CONCURRENTLY on a worker thread.
  // The worker is built from THE SAME function source (fn.toString()) — bit-exact by construction, zero divergence
  // risk. Bands prefetch their rows a band ahead, so the main thread almost never waits; if the worker is missing
  // or slow, generation falls back to the identical inline path. Shared source is only half the contract: the
  // TRANSFER buffers must carry the same precision as the inline arrays they stand in for (hs/Int16Array against
  // hM/hC/hP, ms/Float64Array against mossRow/mossCol) or the identical math still lands on a different answer.
  //
  // ── THE PREAMBLE IS A REGISTRY, NOT A HAND-WRITTEN LIST (2026-08-17) ── fn.toString() copies TEXT, so every
  // name a serialized function mentions has to be re-declared inside the worker. This preamble used to name its
  // seventeen by hand, and it rotted the first day the height function grew a term: the desert biome put
  // desertM/duneH/fbm/DESY/DESREL into makeHRow, the oak forest later added oakRoll/oakBank and their whole
  // transitive tail, and none of it was in the string. The worker threw ReferenceError on its FIRST job, onerror
  // set genWorkerOk = false, and every band quietly went back to the identical inline path — right answers, no
  // prefetch, for weeks, behind a console.warn nobody reads. Three things changed so it cannot recur:
  //   1. the preamble is the same consts/fns registry world/gen-pool.js uses, so adding a term to the height
  //      function is a ONE-LINE registry edit instead of a silent breakage;
  //   2. lint-vb.py check 9 now walks THIS registry too — it proves every top-level name reachable from a
  //      registered function is itself registered, and fails the build when one is not;
  //   3. the failure is LOUD: console.error naming the symbol the worker could not find (see rowDead).
  let genWorker = null, genWorkerOk = false;
  const rowJobs = new Map();                           // key → {done, hs, ms}
  let rowErr = null, rowSent = 0, rowGot = 0;          // …and the telemetry behind window.__vbRow()
  const rowDead = (m) => {                             // ONE place says the prefetch is gone, and it says it in RED
    genWorkerOk = false;
    rowErr = String((m && m.message) || m || 'unknown');
    console.error('[vb] GEN ROW WORKER DEAD — ' + rowErr +
      '\n      the height/moss prefetch is OFF; generation falls back to the identical inline path (right answers, slower).' +
      '\n      "X is not defined" here means a MISSING REGISTRY ENTRY: add X to consts/fns in src/world/gen-worker.js.' +
      '\n      python tools/lint-vb.py (check 9) is what is supposed to catch that before the browser ever does.');
  };
  try {
    // ── EVERY NAME HERE MUST BE DECLARED ABOVE THIS FRAGMENT ── src/manifest.txt line 15, the third of the
    // world/ fragments: world/window.js (13) and world/gen-noise.js (14) are the only two files these names can
    // come out of. Anything from world/terrain.js (23) or world/gen-pool.js (25) is still in its temporal dead
    // zone on this line, and reading it would throw INSIDE this try — which swallows it and falls back, the same
    // invisible failure the missing names caused. Check 9 enforces the ordering as well as the completeness.
    const consts = { WOB_DES1, WOB_DES2, WOB_OAK, WOB_CH, LIFT, WL, HMAX, RIVLAND, RIVCELL, RIVINF, RIVNEAR_CAP, BIOP, DESOFF, DESB, DESW, DESC, DESH, DESY, DESREL, DESDUNE, OAKOFF, OAKB, OAKW, OAKY, OAKHILL, OAKFAR, OAKNEAR, OAKWOFF, OAKC, OAKH, OAKWFAR, OAKWNEAR, OAKBANKR, OAKBANKY, OAKBRISE, OAKBEACH, OAKBEACHY, BIRCHOFF, BIRCHB, BIRCHH, BIRCHC, BIRCHWMAX, BIRCHFAR, BIRCHWFAR, BASIN_T, BASIN_ARCT, BASIN_LOW, BASIN_ARCTLIFT, ARCTOFF, ARCTB, ARCTH, ARCTC, ARCTWMAX, ARCTFAR, ARCTWFAR, ARCTIC_SNOW, ARCT_BARE, ARCT_GROUND, ARCT_SEA, ARCT_SEAREL, ARCT_SEAF, ARCT_SEAF2, ARCT_SEAMIX, ARCT_SEAPOW, ARCT_STAND, PINEY, PINEHILL, PINE_LAKE, PINEBEACH, PINERISE, CHOFF, CHHALF, CHWHALF, CHB, CHW, BIORW, BIORSAT, BIORIV_ON, BIORVALL, BIORVK };   // the BIRCH band: oakRoll/oakBank read its mask and reach, and this worker calls both
    const fns = { ihash, sstep, pwrap, vnoise, fbm, riverAt, rivEval, gatherRivers, riversNear, riverS, bankEval, bankDist, desWob, desertM, birchM, arcticM, chWob, bioPin, bioWobZ, bioEdge, bioRivS, basinT, basinLow, arctSeaH, arctH, oakWob, oakM, pineH, oakH, oakRoll, oakBank, duneH, rowNoise, makeHRow, makeMossRow, colNoise, makeHCol, makeMossCol };
    let wsrc = '';
    for (const k in consts) wsrc += 'const ' + k + ' = ' + consts[k] + ';\n';
    // ── SPWX/SPWZ RIDE WITH THE JOB, they are not baked ── spawn is randomised in world/build.js, nine fragments
    // BELOW this one, so both are still 0 while this preamble is assembled, and rerollSpawn (__vb.reroll) moves
    // them again at runtime. oakM and desertM are anchored to them, so a baked pair would put the biome
    // boundaries somewhere the main thread's H() does not agree with. rowKey carries them too, which is what
    // makes a prefetch issued before a reroll simply unusable after one rather than quietly wrong.
    wsrc += 'let SPWX = 0, SPWZ = 0, rivScope = null;\nconst rivCache = new Map();\nconst rivNear = new Map();\n';   // riversNear's own store — the worker must declare every top-level the registered fns close over
    wsrc += 'let bpZ = null, bpD = 0, bpO = 0, bpC = 0, bwZ = null, bwD = 0, bwO = 0, bwC = 0, bwM = 0, bwW = 0;\n';   // the border rivers' two 1-entry memos (see BIOME BORDER RIVERS in world/window.js) — a registered fn's top-level state has to be declared on this side too
    for (const k in fns) wsrc += 'const ' + k + ' = ' + fns[k].toString() + ';\n';
    wsrc += 'onmessage = (e) => {\n' +
      '  const { key, x0, x1, z0, z1, tr } = e.data;\n' +
      '  SPWX = e.data.spwx; SPWZ = e.data.spwz;\n' +
      '  rivScope = gatherRivers(x0 - 300, x1 + 300, z0 - 300, z1 + 300);\n' +
      '  let hs, ms;\n' +
      '  if (!tr) {\n' +
      '    const w = (x1 - x0) + 2, rows = (z1 - z0) + 2;\n' +
      '    hs = new Int16Array(w * rows); ms = new Float64Array((x1 - x0) * (z1 - z0));\n' +   // moss is f64, NOT f32 — these rows stand in for genRegionGen's own mossRow, which is a Float64Array (and the gen POOL stubs takeRows to null, so its workers always compute moss inline at f64). mossV feeds one threshold, mossy = mossV > 0.52, so an f32 round here flipped the surface material of any column within ~3e-8 of it: ?nopool and the pool-stall fallback disagreed with the pooled path, and ?nopool exists precisely to be bit-comparable.
      '    for (let r = 0; r < rows; r++) { const f = makeHRow(z0 - 1 + r); for (let i = 0; i < w; i++) hs[r * w + i] = f(x0 - 1 + i); }\n' +
      '    for (let r = 0; r < z1 - z0; r++) { const f = makeMossRow(z0 + r); for (let i = 0; i < x1 - x0; i++) ms[r * (x1 - x0) + i] = f(x0 + i); }\n' +
      '  } else {\n' +
      '    const w = (z1 - z0) + 2, cols = (x1 - x0) + 2;\n' +
      '    hs = new Int16Array(w * cols); ms = new Float64Array((z1 - z0) * (x1 - x0));\n' +   // …and the transposed path likewise, against mossCol
      '    for (let c = 0; c < cols; c++) { const f = makeHCol(x0 - 1 + c); for (let i = 0; i < w; i++) hs[c * w + i] = f(z0 - 1 + i); }\n' +
      '    for (let c = 0; c < x1 - x0; c++) { const f = makeMossCol(x0 + c); for (let i = 0; i < z1 - z0; i++) ms[c * (z1 - z0) + i] = f(z0 + i); }\n' +
      '  }\n' +
      '  postMessage({ key, hs, ms }, [hs.buffer, ms.buffer]);\n' +
      '};';
    if (location.search.includes('wsrc')) window.__vbRowSrc = wsrc;   // ?wsrc — hand the assembled source out so a syntax error in it can be located instead of guessed at (world/gen-pool.js offers the same tap as __vbWSRC)
    const gurl = URL.createObjectURL(new Blob([wsrc], { type: 'text/javascript' }));
    genWorker = new Worker(gurl);
    URL.revokeObjectURL(gurl);                         // the worker holds its own copy of the script; the blob URL would otherwise pin the source for the page's lifetime
    genWorker.onmessage = (e) => { const j = rowJobs.get(e.data.key); if (j) { j.hs = e.data.hs; j.ms = e.data.ms; j.done = true; } if (!rowGot++) console.log('[vb] gen row worker: live'); };   // the first reply is the only proof the blob actually RUNS — say so once, so "on" and "off" are both visible
    genWorker.onerror = (e) => rowDead(e && (e.message || e.error || e));
    genWorkerOk = true;
  } catch (e) { rowDead(e); }
  window.__vbRow = () => ({ ok: genWorkerOk, err: rowErr, sent: rowSent, got: rowGot, queued: rowJobs.size });   // console tap: `__vbRow()` — ok:false with err set is a dead prefetch, ok:true with got:0 is one that never replied
  const rowKey = (x0, x1, z0, z1, tr) => x0 + ',' + x1 + ',' + z0 + ',' + z1 + ',' + (tr ? 1 : 0) + ',' + SPWX + ',' + SPWZ;   // spawn is IN the key: rerollSpawn moves the biome boundaries, so a job computed under the old spawn must never be handed to a sweep running under the new one
  function reqRows(x0, x1, z0, z1) {                   // idempotent prefetch of a region's height/moss rows
    if (!genWorkerOk) return;
    const tr = (x1 - x0) < (z1 - z0);
    const key = rowKey(x0, x1, z0, z1, tr);
    if (!rowJobs.has(key)) {
      if (rowJobs.size > 6) { const first = rowJobs.keys().next().value; rowJobs.delete(first); }   // drop stale mispredictions
      rowJobs.set(key, { done: false });
      genWorker.postMessage({ key, x0, x1, z0, z1, tr, spwx: SPWX, spwz: SPWZ });
      if (!rowSent++) setTimeout(() => { if (genWorkerOk && !rowGot) console.error('[vb] gen row worker: 15 s and not one reply — the prefetch is idle, bands are generating inline. window.__vbRow() for the counters.'); }, 15000);   // the one failure onerror cannot see: a worker that neither throws nor answers
    }
  }
  function takeRows(x0, x1, z0, z1, tr) {
    const key = rowKey(x0, x1, z0, z1, tr);
    const j = rowJobs.get(key);
    if (j && j.done) { rowJobs.delete(key); return j; }
    return null;
  }
  function rowsPending(x0, x1, z0, z1) {
    if (!genWorkerOk) return false;
    const j = rowJobs.get(rowKey(x0, x1, z0, z1, (x1 - x0) < (z1 - z0)));
    return !!j && !j.done;
  }
  const nearLake = (x, z) => basinM(x, z) > 0.03;
