  // ── GEN WORKER ── the height + moss math (the heaviest part of a fill) runs CONCURRENTLY on a worker thread.
  // The worker is built from THE SAME function source (fn.toString()) — bit-exact by construction, zero divergence
  // risk. Bands prefetch their rows a band ahead, so the main thread almost never waits; if the worker is missing
  // or slow, generation falls back to the identical inline path. Shared source is only half the contract: the
  // TRANSFER buffers must carry the same precision as the inline arrays they stand in for (hs/Int16Array against
  // hM/hC/hP, ms/Float64Array against mossRow/mossCol) or the identical math still lands on a different answer.
  let genWorker = null, genWorkerOk = false;
  const rowJobs = new Map();                           // key → {done, hs, ms}
  try {
    const wsrc = 'const LIFT=' + LIFT + ',WL=' + WL + ',HMAX=' + HMAX + ',RIVCELL=' + RIVCELL + ',RIVINF=' + RIVINF + ';\n' +
      'const ihash=' + ihash.toString() + ';\nconst sstep=' + sstep.toString() + ';\n' +
      'const rivCache=new Map();\nconst riverAt=' + riverAt.toString() + ';\nconst rivEval=' + rivEval.toString() + ';\n' +
      'let rivScope=null;\n' + gatherRivers.toString() + '\n' + riverS.toString() + '\n' +
      rowNoise.toString() + '\n' + colNoise.toString() + '\n' + makeHRow.toString() + '\n' + makeHCol.toString() + '\n' +
      makeMossRow.toString() + '\n' + makeMossCol.toString() + '\n' +
      'onmessage = (e) => {\n' +
      '  const { key, x0, x1, z0, z1, tr } = e.data;\n' +
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
    const gurl = URL.createObjectURL(new Blob([wsrc], { type: 'text/javascript' }));
    genWorker = new Worker(gurl);
    URL.revokeObjectURL(gurl);                         // the worker holds its own copy of the script; the blob URL would otherwise pin the source for the page's lifetime
    genWorker.onmessage = (e) => { const j = rowJobs.get(e.data.key); if (j) { j.hs = e.data.hs; j.ms = e.data.ms; j.done = true; } };
    genWorker.onerror = (e) => { console.warn('[vb] gen worker error — inline generation', e.message || e); genWorkerOk = false; };
    genWorkerOk = true;
  } catch (e) { console.warn('[vb] gen worker unavailable — inline generation', e); }
  const rowKey = (x0, x1, z0, z1, tr) => x0 + ',' + x1 + ',' + z0 + ',' + z1 + ',' + (tr ? 1 : 0);
  function reqRows(x0, x1, z0, z1) {                   // idempotent prefetch of a region's height/moss rows
    if (!genWorkerOk) return;
    const tr = (x1 - x0) < (z1 - z0);
    const key = rowKey(x0, x1, z0, z1, tr);
    if (!rowJobs.has(key)) {
      if (rowJobs.size > 6) { const first = rowJobs.keys().next().value; rowJobs.delete(first); }   // drop stale mispredictions
      rowJobs.set(key, { done: false });
      genWorker.postMessage({ key, x0, x1, z0, z1, tr });
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

