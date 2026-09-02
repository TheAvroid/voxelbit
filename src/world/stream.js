  // ── DEMAND-DRIVEN STREAMING ── the world holds a fully-generated RECTANGLE (`rect`, world coords, 8-aligned)
  // sized to the view distance. Moving grows it band-by-band toward where the player is heading — bands are only
  // as long as the view needs, so generation keeps pace with SPRINT speed instead of paying for the full window.
  // The window origin slides for free (no strip regen); anything wrapped away just falls out of the rect, and the
  // view clamp guarantees only rect-interior terrain is ever traced.
  let xStripPending = -1;                              // grid x of a repacked strip awaiting its scatter dispatch
  let genJob = null;                                   // the ONE background generator — grows rect one 8-voxel band at a time
  let genMs = 0, genBands = 0, genSpinMs = 0;          // streaming-gen telemetry (__vb.gen()): main-thread ms in stepShifts, completed bands, ms spent merely waiting on the pool
  function rectTarget() {                              // where rect should reach: view distance + margin around the player, inside the window
    const R = Math.min(HALF, renderDist + 96);
    return { xlo: Math.max(winOX, (Math.floor((P.x - R) / 8)) * 8), xhi: Math.min(winOX + WX, (Math.ceil((P.x + R) / 8)) * 8),
             zlo: Math.max(winOZ, (Math.floor((P.z - R) / 8)) * 8), zhi: Math.min(winOZ + WZ, (Math.ceil((P.z + R) / 8)) * 8) };
  }
  const splitWrap = (w0, w1, N) => { const g0 = gwrap(w0, N), len = w1 - w0; return g0 + len <= N ? [[g0, g0 + len]] : [[g0, N], [0, g0 + len - N]]; };
  function rebuildBricksW(wx0, wx1, wz0, wz1) {        // world-coord occupancy rebuild, split across the toroidal seam
    for (const [a, b] of splitWrap(wx0, wx1, WX)) for (const [c, d] of splitWrap(wz0, wz1, WZ)) rebuildBricks(a, b, c, d);
  }
  function rebuildBricks2W(wx0, wx1, wz0, wz1) {       // L2-only variant — after a pooled band whose L1 bits were merged straight from the worker slabs
    for (const [a, b] of splitWrap(wx0, wx1, WX)) for (const [c, d] of splitWrap(wz0, wz1, WZ)) rebuildBricks2(a, b, c, d);
  }
  function bandSpec(r) {                               // the most urgent deficient side of rectangle r vs the view target
    const t = rectTarget();
    const cand = [];
    if (r.xlo > t.xlo) cand.push(['xlo', P.x - r.xlo, r.xlo - t.xlo]);
    if (r.xhi < t.xhi) cand.push(['xhi', r.xhi - P.x, t.xhi - r.xhi]);
    if (r.zlo > t.zlo) cand.push(['zlo', P.z - r.zlo, r.zlo - t.zlo]);
    if (r.zhi < t.zhi) cand.push(['zhi', r.zhi - P.z, t.zhi - r.zhi]);
    if (!cand.length) return null;
    cand.sort((a, b) => a[1] - b[1]);                  // the side the player is closest to outrunning goes first
    const side = cand[0][0];
    // ── ADAPTIVE THICKNESS ── barely behind → an 8-band (lowest latency); far behind → up to 32 in one region. One 32-thick
    // region is MUCH cheaper than four 8-bands: a tree/rock spanning ~50 voxels re-iterates its whole sparse model in every
    // band that clips it, and the per-band fixed costs (river gather, brick rebuild+upload, blit rounds) amortize 4×.
    const th = Math.max(8, Math.min(64, cand[0][2]));  // deficits are 8-aligned, so th is too; the 64 ceiling is CATCH-UP mode (post-teleport / sprint debt) — max stamp amortization per region
    if (side === 'zlo') return { side, th, x0: r.xlo, x1: r.xhi, z0: r.zlo - th, z1: r.zlo };
    if (side === 'zhi') return { side, th, x0: r.xlo, x1: r.xhi, z0: r.zhi, z1: r.zhi + th };
    if (side === 'xlo') return { side, th, x0: r.xlo - th, x1: r.xlo, z0: r.zlo, z1: r.zhi };
    return { side, th, x0: r.xhi, x1: r.xhi + th, z0: r.zlo, z1: r.zhi };
  }
  function* genBandGen() {                             // grow rect by ONE band (8-32 thick, see bandSpec) on the most urgent side, then finish
    const s = bandSpec(rect);
    if (!s) return;
    if (poolOk) poolRegion(s.x0, s.x1, s.z0, s.z1);    // queue the CURRENT band ahead of the next-band prefetch below (usually already in flight)
    { const nr = { xlo: rect.xlo, xhi: rect.xhi, zlo: rect.zlo, zhi: rect.zhi };   // PREFETCH the next band while this one fills
      if (s.side === 'zlo') nr.zlo -= s.th; else if (s.side === 'zhi') nr.zhi += s.th; else if (s.side === 'xlo') nr.xlo -= s.th; else nr.xhi += s.th;
      const ns = bandSpec(nr);
      if (ns) { if (poolOk) poolRegion(ns.x0, ns.x1, ns.z0, ns.z1); else reqRows(ns.x0, ns.x1, ns.z0, ns.z1); }
    }
    if (s.side === 'zlo' || s.side === 'zhi') {
      const pooled = yield* genRegionThreaded(s.x0, s.x1, s.z0, s.z1);
      if (pooled) rebuildBricks2W(s.x0, s.x1, s.z0, s.z1); else rebuildBricksW(s.x0, s.x1, s.z0, s.z1);   // pooled slabs delivered their L1 bits — only L2 remains
      nvDirtyRect(s.x0, s.x1, s.z0, s.z1);             // ── NAVFIELD ── the band's nav columns are stale; the flush rebuilds them inside the streaming budget (the brick bits above are already merged, so the column scan can skip empty air)
      yield;
      while (xStripPending >= 0) { yield; }            // ⚠ ORDERING: a pending x-strip scatter holds a PRE-band snapshot of these very rows (its "stale, outside rect" corner).
      // Uploading now would queue BEFORE that scatter executes — the scatter then stomps this fresh corner with stale wrapped
      // terrain (underground stone rendered as floating grey boxes; CPU W stays correct, so only F5 cured it). Wait it out first.
      if (CPROF) cpEvt |= 2;
      for (const [ga, gb] of splitWrap(s.z0, s.z1, WZ)) {   // z-bands are contiguous grid rows; a >8-thick band may cross the toroidal seam → write per contiguous segment
        // …the VOXELS this band wrote are already queued: the slab merge in gen-pool.js poolTouch()es every brick.
        uploadBricksZ(ga, gb);                         // only THIS band's occupancy slice — the change is confined to these rows
      }
      if (s.side === 'zlo') rect.zlo = s.z0; else rect.zhi = s.z1;
      genBands += s.th >> 3;
    } else {
      const pooled = yield* genRegionThreaded(s.x0, s.x1, s.z0, s.z1);
      if (pooled) rebuildBricks2W(s.x0, s.x1, s.z0, s.z1); else rebuildBricksW(s.x0, s.x1, s.z0, s.z1);
      nvDirtyRect(s.x0, s.x1, s.z0, s.z1);             // ── NAVFIELD ── same for an x-side band
      yield;
      yield;                                         // ── THE DENSE X-STRIP SCATTER IS GONE ── it repacked a column strip into stag64/stagBuf and scattered it
      // into the dense GPU world. There is none: the slab merge queues the band's bricks and poolFlush drains
      // them in the same frame the occupancy lands.
      uploadBricks();
      if (s.side === 'xlo') rect.xlo = s.x0; else rect.xhi = s.x1;
      genBands += s.th >> 3;
    }
  }
  function stepShifts() {
    if (ED.on) return;                                 // the asset-editor world is FROZEN — no window shifts, no worldgen, nothing to relight the hidden forest
    if (!Number.isFinite(P.x) || !Number.isFinite(P.z) || !Number.isFinite(P.y)) {   // ── FREEZE GUARD ── a non-finite position spins the window-shift loops below FOREVER (Infinity never catches up) → a permanent hard freeze needing a reload. Recover to the window centre instead of hanging (user: "sometimes the game freezes" while walking).
      P.x = winOX + HALF + 0.5; P.z = winOZ + HALF + 0.5; P.vy = P.hvx = P.hvz = 0;
      P.y = Math.max(hmap[gwrap(Math.floor(P.x), WX) + gwrap(Math.floor(P.z), WZ) * WX], WL) + EYE;
      console.warn('[vb] non-finite player position — recovered to window centre (was a freeze)');
    }
    let moved = false, guard = 0;                      // the origin slides INSTANTLY (32-aligned for L2 wrap) — wrapped rows simply leave the rect; the ±4096 cap is a backstop so a pathological coordinate can never lock the frame loop
    while (P.x - (winOX + HALF) >= 32 && guard++ < 4096) { winOX += 32; moved = true; }
    while (P.x - (winOX + HALF) <= -32 && guard++ < 4096) { winOX -= 32; moved = true; }
    while (P.z - (winOZ + HALF) >= 32 && guard++ < 4096) { winOZ += 32; moved = true; }
    while (P.z - (winOZ + HALF) <= -32 && guard++ < 4096) { winOZ -= 32; moved = true; }
    if (moved) {
      rect.xlo = Math.max(rect.xlo, winOX); rect.xhi = Math.min(rect.xhi, winOX + WX);
      rect.zlo = Math.max(rect.zlo, winOZ); rect.zhi = Math.min(rect.zhi, winOZ + WZ);
    }
    const tT = rectTarget();
    const deficit = Math.max(rect.xlo - tT.xlo, tT.xhi - rect.xhi, rect.zlo - tT.zlo, tT.zhi - rect.zhi);
    if (!genJob && deficit <= 0) return;
    // budget = the MEASURED CPU idle time inside the frame pipeline, but never below a floor while terrain is
    // owed — the early-return above means the floor only applies while there is actually work to do. A LARGE
    // deficit (teleport / recenter) still raises it to 18: the user prioritizes terrain CATCH-UP RATE over fps
    // while the frontier is far behind, and that path already shows the regen flash.
    //
    // STEADY-STATE FLOOR 7 -> 3 (user 2026-08-09: "there seems to be some stuttering"). The 7 ms floor was
    // buying NOTHING. Seeded A/B, identical spawn/world/walk, 45 s scripted sprint, snow pinned off, deficit
    // sampled every frame (floor / tickP99 = the JS spike this budget controls / max deficit reached):
    //     7 -> 11.0, 11.0, 11.6, 11.0 ms | 32       5 -> 8.5 ms | 32       4 -> 8.4 ms | 32
    //     3 ->  6.2,  6.9,  6.8,  8.2 ms | 32       2 -> falls behind | 64  0.5 -> falls behind | 64
    // The deficit NEVER exceeds 32 (one band quantum) at any floor >= 3 — i.e. the streamer is not throughput
    // limited here, it was just doing the same work in bigger, burstier chunks. Only ~13-19% of frames stream
    // at all, so the identical work spread over more frames costs no catch-up and cuts the worst-case JS frame
    // by ~40%. Below 3 the deficit doubles to 64 AND frame times get worse, so 3 is the floor of the sweet spot.
    // Measured with vsync OFF: when the adapter DOES have headroom the paceWaited arm below already grants up to
    // 14 ms, so this floor only governs the no-headroom case — which is exactly when forcing 7 ms drops a frame.
    const budget = Math.max(deficit >= 128 ? 18 : 3, paceWaited ? Math.min(14, waitEma * 0.85) : Math.min(4, 250 / Math.max(fpsEma, 30)));
    const t0 = performance.now();
    while (performance.now() - t0 < budget) {          // chain bands back-to-back — no budget goes unused
      if (!genJob) { const t2 = rectTarget(); if (rect.xlo <= t2.xlo && rect.xhi >= t2.xhi && rect.zlo <= t2.zlo && rect.zhi >= t2.zhi) break; genJob = genBandGen(); }
      if (genJob.next().done) genJob = null;           // (genBands counts 8-band equivalents inside genBandGen — a thick band scores its true width)
      if (poolWait) { genSpinMs += performance.now() - t0; break; }   // the band is cooking on the worker pool — don't burn the budget on empty spins
    }
    genMs += performance.now() - t0;
  }
  function recenter(x, z) {                            // teleport-scale move: rebuild a starter square, the rest streams in
    if (CPROF) cpEvt |= 8;
    loadEl.classList.remove('hidden'); loadMsgEl.textContent = 'regenerating forest…'; $('loadMeter').style.display = 'none';   // in-game regen is a brief synchronous flash — no 0-100 progress to show, so hide the boot meter
    genJob = null; xStripPending = -1;
    winOX = Math.round(x / 32) * 32 - HALF; winOZ = Math.round(z / 32) * 32 - HALF;
    W.fill(0); touched.fill(0);
    const bh = Math.min(224, HALF >> 1);
    rect.xlo = Math.round(x / 8) * 8 - bh; rect.xhi = rect.xlo + bh * 2;
    rect.zlo = Math.round(z / 8) * 8 - bh; rect.zhi = rect.zlo + bh * 2;
    supCarved.fill(0);                                 // BEFORE genRegion, never after: worldgen re-stamps the models and each one re-marks its own columns (see stampModel mode 2). Clearing this afterwards would wipe exactly the marks the new world just earned.
    genRegion(rect.xlo, rect.xhi, rect.zlo, rect.zhi, true);
    orphQueueRect(rect.xlo, rect.xhi, rect.zlo, rect.zhi);   // the pool never saw this rect - sweep it here, on a budget
    rebuildBricks(0, WX, 0, WZ);
    poolBuild();                                       // a recentre replaces the whole window, so the pool is re-derived rather than patched brick by brick. This is where the dense uploadWorld() used to sit.
    uploadBricks();
    loadEl.classList.add('hidden');
    resetHist = 1;
    // ── AND THE STAMP INDEX GOES WITH IT ── W.fill(0) wiped the world but stampedIdx kept every stale flat
    // index, and every support test treats a stamped cell as neither liftable nor support. After one
    // recentre thousands of scattered cells in the NEW world were permanently exempt from every test.
    stampedIdx.clear();
    // ── AND SO DOES THE SUPPORT QUEUE (2026-08-08) ── same defect, same cause: SUP.q holds flat indices into
    // the WRAPPED WINDOW, and the window has just been re-pointed at different world columns. Every pending
    // seed now names a cell somewhere else entirely, so the resolver spends its 2 ms/frame budget
    // adjudicating freshly generated terrain that nothing has touched — while a genuine floater queued behind
    // it waits. (supCarved is cleared UP THERE, before genRegion — see the note at the fill.)
    SUP.q.length = 0; SUP.qh = 0; SUP.qs.clear(); SUP.retry.length = 0;
    SUP.res.clear(); SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();
    // …and the reset loop must cover the LAND MAMMALS too. It ran j < 244 while bunnies/armadillos/skunks/
    // porcupines live in 276-371 (unstampAllWorms walks 16 < 372 for exactly this reason), so one survived
    // the recentre with B.sN intact and its next unstampWorm wrote old sPrev ids back into freshly
    // generated terrain at unrelated coordinates — creature-shaped dirt floating in the new world.
    for (let j = 16; j < DES_END; j++) { const B = wbf[j]; if (B) { B.sN = 0; B.sKey = null; } }   // world was zeroed above — force every grid-stamped creature to re-stamp fresh next frame; the slots that carry no stamps cost nothing to clear
    if (nvOn) nvBoot();                                // ── AND THE NAVFIELD GOES WITH IT ── W.fill(0) invalidated every cell; nvBoot clears NVF_BUILT (so the field vouches for nothing until rebuilt), rebuilds the player's neighbourhood and re-arms the background sweep
  }

