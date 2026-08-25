  // @module - the four land mammals' ground seat and their walking navigation
  // @exports MAM_SPAN, mamSeatG, mamSeatN, mamSeatSteps, mamStandAt, navBrake2, navReachWalk, navSteer2, navWalkFree, navWalkOK
  // ── THE ONE SEAT EVERY LAND MAMMAL USES ── one function, called by BOTH render paths, so a bunny and a
  // skunk cannot disagree about where the ground is and neither can the same animal before and after it
  // crosses the trace radius. It is the model's own occupied footprint (MAMFIT), oriented by heading, sampled
  // 3×3 and reduced with MAX — the max is what makes clipping unrepresentable, since no part of the body can
  // end up under a surface at or below the number its feet rest on.
  // ── HOW MANY CONTACT POINTS (user 2026-08-07: "increase the contact points maybe?") ── a 3×3 grid samples
  // an 11-voxel skunk at −5.5, 0 and +5.5 along its length: a rock or ridge anywhere in those 5.5-voxel gaps
  // is simply not seen, and the body settles straight through it. Measured, that left terrain inside the
  // walkers on ~10-19% of frames — and, tellingly, at the SAME rate slow as fast (skunk 10% under 8 vox/s and
  // 10% over), so it was never the speed the animal was travelling at. The step count is derived from the
  // model rather than fixed, so every animal gets the same ~2-voxel resolution under it whatever its size,
  // and a longer animal automatically gets more points instead of coarser ones.
  // ── HOW MUCH OF THE BODY'S LENGTH CARRIES ITS WEIGHT (user 2026-08-07: "continue to improve terrain contact
  // with the skunk") ── the seat is the MAX over the footprint, which is what makes sinking impossible, but on a
  // DESCENT the highest sample is always the tail, so the whole body was held up at the uphill end and the nose
  // hung in the air. A real quadruped's weight is on its feet, not on the tip of its nose and tail, so only the
  // middle 75% of the length feeds the seat and the overhang at each end is allowed to graze. Measured on the
  // skunk descending: median float +0.45 -> 0.00 and airborne 48% -> 10%, against a small rise in grazing that
  // is mostly walk-through clutter. 0.55 was tried and overshot — it sank into real ground on 46% of frames.
  const MAM_SPAN = 0.75;
  const MAM_STEP = 2.0;                                // target spacing between contact points, in voxels
  // ── AND, FOR THE DESERT BAND ONLY, NOT ON A ROCK OR A CACTUS (user 2026-08-16) ── the MAX is what makes
  // clipping unrepresentable, but it is a max over navWalkStand, and navWalkStand cannot see through a mode-2
  // stamp: a desert rock RAISES the heightmap, so the clamp that normally refuses a surface more than one
  // step-up above the column is comparing the rock against itself and always passes. The result is that a
  // creature standing on the sand BESIDE a boulder, with one footprint sample over it, is seated on the
  // boulder's top and rides up its flank without ever stepping onto it — measured at 36% of frames and up to
  // 22 voxels. With `sand` set the max is taken over the samples that are on SAND, and the full max is kept
  // only as the fallback for a body that is entirely over stone (it was placed there, or the world changed
  // under it) — that one still needs a floor, and the floor it has is the rock.
  const mamSeatN = (B, fit, nA, nC, sand) => {                // …at an explicit resolution, so a test can compare densities on ONE pose instead of across two wandering runs
    const th = B.th || 0, hx = Math.sin(th), hz = Math.cos(th);
    const hd = fit ? fit.hd : 3, hw = fit ? fit.hw : 2;
    let g = -1e9, gs = -1e9;
    for (let u = -nA; u <= nA; u++) for (let v = -nC; v <= nC; v++) {
      const sa = hd * MAM_SPAN * (u / nA), sc = hw * (v / nC);    // the extremes stay exactly on the footprint edge; the extra points fill in between
      const px = B.x + hx * sa + hz * sc, pz = B.z + hz * sa - hx * sc;
      const q = navWalkStand(px, pz);
      if (q > g) g = q;
      if (sand && q > gs && navSand(px, pz)) gs = q; }   // the sand-only max, built alongside. Every other band passes no flag, so the second test is never even reached and their seat is the same number to the last bit.
    return (sand && gs > -1e8) ? gs : g;
  };
  const mamStandAt = (x, z, sand) => (sand && !navSand(x, z)) ? -1e9 : navWalkStand(x, z);   // ONE point of the same surface, under the same rule — the forward look that starts a walker's ramp before a step reads this, so it cannot ramp onto something the seat has just refused to sit on
  const mamSeatSteps = (fit) => [Math.max(1, Math.min(3, Math.ceil((fit ? fit.hd : 3) / MAM_STEP))),
                                 Math.max(1, Math.min(3, Math.ceil((fit ? fit.hw : 2) / MAM_STEP)))];
  const mamSeatG = (B, fit, sand) => { const st = mamSeatSteps(fit); return mamSeatN(B, fit, st[0], st[1], sand); };
  const navWalkOK = (x, z, gc, up, down, clr) => {   // THE walker answer — navLandOK with the clutter exemption the marchers' own bfObstW has always carried
    const ci = nvIdx(x, z), f = nvF[ci];
    if (!(f & NVF_BUILT)) { const g = navBed(x, z);   // UNVOUCHED → the honest point probe the marcher used before, on ITS obstacle rule (nvObstW). Never a blanket refusal: a mammal at the streaming frontier must not freeze waiting for a cell to be built.
      return !navWet(x, z) && g > WL && g - gc <= up && gc - g <= down && !nvObstW(x, g + 2, z) && !nvObstW(x, g + clr, z); }
    if (f & NVF_WATER) return false;                 // any water in the 2×2 — a land creature stops AT the waterline, not one step past it
    const d = nvClutD(ci), g = nvY[ci] - d + 1;      // …the same +1: g is the first free voxel above the walker's floor, exactly like the marcher's own gA = bfSurf
    return g > WL && g - gc <= up && gc - g <= down && nvC[ci] + d >= clr + 1; };   // clr is the marcher's BODY-TOP PROBE OFFSET above gA, so the body spans g … g+clr — that is clr+1 free voxels counted up from the solid top, which is what nvC + d counts
  const navWalkFree = (x, z, clr) => {               // the RELAXED gate the boxed-in and watchdog escapes are chosen with AND advanced under: land + body-clear, step limit ignored. Same relation to navWalkOK that the hand-rolled walkFree had to walkOK, so an exit is still picked and taken under one rule.
    const g = navBed(x, z);                          // deliberately the PER-COLUMN point probe, built cell or not — this is the ONE place the 2×2 MAX is the wrong reduction. A cell straddling a tree TRUNK has its clearance measured above the trunk, which is open sky, so a cell-based relaxed gate would wave an escaping animal straight into a pine; the hand-rolled walkFree this replaces probed the column and refused. nvObstW still reads the voxels, so the rock and decor the heightmap never saw are in the test — only the conservative neighbour-max is dropped, which is what "relaxed" means here.
    return !navWet(x, z) && g > WL && !nvObstW(x, g + 2, z) && !nvObstW(x, g + clr, z); };
  const navReachWalk = (x, z, th, maxD, gc, up, down, clr) => {   // navReachLand's DDA on navWalkOK: every cell the path crosses is tested and the step limit is carried FORWARD, so a long slope stays a lane instead of reading as a wall from where the creature started
    const dx = Math.sin(th), dz = Math.cos(th), CS = 1 << NVSH;
    const adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
    const cx = Math.floor(x / CS), cz = Math.floor(z / CS);
    const dtX = adx < 1e-9 ? Infinity : CS / adx, dtZ = adz < 1e-9 ? Infinity : CS / adz;
    let tX = adx < 1e-9 ? Infinity : (dx > 0 ? (cx + 1) * CS - x : x - cx * CS) / adx;
    let tZ = adz < 1e-9 ? Infinity : (dz > 0 ? (cz + 1) * CS - z : z - cz * CS) / adz;
    let t = 0, g = gc;
    for (let k = 0; k < 80; k++) {
      t = tX <= tZ ? tX : tZ;
      if (t >= maxD) return maxD;
      if (tX <= tZ) tX += dtX; else tZ += dtZ;
      const px = x + dx * (t + 0.02), pz = z + dz * (t + 0.02);
      if (!navWalkOK(px, pz, g, up, down, clr)) return t;
      g = navWalkGround(px, pz);
    }
    return t; };
  const navSteer2 = (B, here, reachOf, reachMax, w0, goalTh, goalOn, omCap, gain) => {   // → B.omT. The same 16 compass candidates + hold-current as navSteerAir, scored on the same terms, over the band's own reach. goalTh folds every soft pull the band used to apply by writing B.th behind the planner's back (home leash, keep-apart) into ONE scored term. `reachOf` is a function, not a point predicate: a WALKER's reach has to carry the step limit forward from cell to cell, and a swimmer's does not.
    navTicks++;
    const wth = B.navWander === undefined ? B.th : B.navWander;
    const minR = reachMax * 0.1;                     // "has a lane" scales with the horizon — 4 voxels of 40 for a flyer, 2 of 20 for a worm
    let bestTh = B.th, bestS = -1e9, bestReach = 0;
    if (here) for (let k = 0; k <= 16; k++) {          // if the creature's OWN cell is infeasible the fan is moot — every candidate starts from a place the mover will not accept
      const th = k === 16 ? B.th : NAV_TH[k];
      const sx = k === 16 ? Math.sin(th) : NAV_SIN[k], cz = k === 16 ? Math.cos(th) : NAV_COS[k];
      const reach = reachOf(th, reachMax);
      if (reach < minR) continue;
      const ax = B.x + sx * 6, az = B.z + cz * 6;
      const dTh = navAng(th - B.th);
      let s = (reach / reachMax) * NAV_W[w0 + NAV_W_REACH]
            + (nvO[nvIdx(ax, az)] / NV_OMAX) * NAV_W[w0 + NAV_W_OPEN]
            + Math.cos(dTh) * NAV_W[w0 + NAV_W_KEEP]
            + Math.cos(navAng(th - wth)) * NAV_W[w0 + NAV_W_WANDER]
            - (dTh < 0 ? -dTh : dTh) * (NAV_W[w0 + NAV_W_TURN] / Math.PI);
      if (goalOn) s += Math.cos(navAng(th - goalTh)) * NAV_W[w0 + NAV_W_HOME];
      if (s > bestS) { bestS = s; bestTh = th; bestReach = reach; }
    }
    if (bestReach < minR) {                          // ── LAYER 1 ── no candidate has a lane. Take the steepest openness ascent rather than spin: nvO is a chamfer over the travelable set, so from any cell with nvO > 0 an ascent to more open space EXISTS.
      navNoLane++;
      let bo = B.th, bs = -1e9;
      for (let k = 0; k < 16; k++) {
        const px = B.x + NAV_SIN[k] * 4, pz = B.z + NAV_COS[k] * 4;
        const s = reachOf(NAV_TH[k], 8) * 40 + nvO[nvIdx(px, pz)] - Math.abs(navAng(NAV_TH[k] - B.th)) * 6;
        if (s > bs) { bs = s; bo = NAV_TH[k]; } }
      bestTh = bo;
    }
    B.navTh = bestTh; B.navReach = bestReach;
    const d = navAng(bestTh - B.th);
    B.omT = Math.max(-omCap, Math.min(omCap, d * gain)); };
  const navBrake2 = (B, reachOf, mv, dt, look, clr, brk2, ki) => {   // navBrakeAir's curve, per band. `ki` overrides the counter ROW ONLY (never the returned step): the land mammals share B.kind 2 with the worm, and a shared row would let one band's brake hide inside the other's. The mover translates along B.th, which LAGS the planned heading while the eased turn integrator catches up; braking on the reach of the lane the creature is ACTUALLY travelling means the step can no longer END past that lane, so it can no longer be rejected.
    const rr = reachOf(B.th, look);
    B.navClear = rr;
    if (rr >= look) return mv;                       // clear to the horizon — the common case: one bounded DDA and out
    const kk = ki === undefined ? (B.kind | 0) : ki;
    navBrkK[kk]++;                                   // NOT navBrakeN: arbBrakeN/arbBrakePct/arbBrakeVox stay FLYER-only so they remain comparable with the flyer baselines they were tuned against
    let m = Math.sqrt(brk2 * (rr > clr ? rr - clr : 0)) * dt;   // v = sqrt(2·a·d): continuous in reach, so the speed RAMPS instead of stepping, and it self-scales with the creature
    if (m > mv) m = mv;
    const cap = rr - 0.05;                           // GEOMETRIC BACKSTOP — the curve alone can still overshoot on a long frame, and one overshoot is one rejection
    if (m > cap) m = cap > 0 ? cap : 0;
    if (m < mv) navBrkVoxK[kk] += mv - m;   // voxels of travel actually WITHHELD — the only honest measure of whether a cap made a band slower; the frame COUNT above is dominated by frames where the curve exceeded the creature's own speed and the cap did nothing
    return m; };
  __vb.navstat = () => ({
    on: nvOn, arbiter: NAVARB ? 1 : 0, cells: NVN, cellVox: 1 << NVSH,
    bytes: NV_BYTES, MB: +(NV_BYTES / 1048576).toFixed(2),
    bootMs: +nvFullMs.toFixed(1), builtCells: nvBuiltTotal,
    builtPct: +((() => { let n = 0; for (let i = 0; i < NVN; i += 7) if (nvF[i] & NVF_BUILT) n++; return n / Math.ceil(NVN / 7) * 100; })()).toFixed(1),
    dirtyQ: nvQn, qDrops: nvQdrop, sweeping: nvSweep, chamCycles: nvChCycles,
    flushMsAvg: +((() => { let s = 0; for (let i = 0; i < nvMSn; i++) s += nvMS[i]; return nvMSn ? s / nvMSn : 0; })()).toFixed(3),
    flushMsP99: +((() => { if (!nvMSn) return 0; const a = Array.from(nvMS.subarray(0, nvMSn)).sort((p, q) => p - q); return a[Math.min(nvMSn - 1, Math.floor(nvMSn * 0.99))]; })()).toFixed(3),
    arbTicks: navTicks, arbNoLane: navNoLane, arbVetoY: navVetoY, arbRejects: navRejects, arbEgress: navEgressN,
    brake: NAVBRK ? 1 : 0, arbMoveN: navMoveN, arbRejPct: navMoveN ? +(navRejects / navMoveN * 100).toFixed(3) : -1,
    arbBrakeN: navBrakeN, arbBrakePct: navMoveN ? +(navBrakeN / navMoveN * 100).toFixed(2) : -1, arbBrakeVox: +navBrakeVox.toFixed(1),
    arbRejGeom: navRejGeom, arbRejSub: navRejSub, arbRejUnb: navRejUnb,
    byKindMove: Array.from(navMoveK), byKindRej: Array.from(navRejK), byKindEgress: Array.from(navEgrK),
    byKindVetoY: Array.from(navVetK), byKindBrake: Array.from(navBrkK), byKindBrakeVox: Array.from(navBrkVoxK).map((v) => +v.toFixed(1)) });
  __vb.navreset = () => { navTicks = navNoLane = navVetoY = navRejects = navEgressN = 0; navMoveN = navBrakeN = navBrakeVox = navRejGeom = navRejSub = navRejUnb = 0; navMoveK.fill(0); navRejK.fill(0); navEgrK.fill(0); navVetK.fill(0); navBrkK.fill(0); navBrkVoxK.fill(0); nvMSn = 0; nvMSi = 0; nvBuiltTotal = 0; return true; };
  __vb.navVerify = (n) => {                          // THE regression gate. Three independent questions, none of them asked with the field's own accessors.
    n = n || 20000;
    if (!nvOn) return { on: 0 };
    const x0 = Math.max(rect.xlo + 8, P.x - 320), x1 = Math.min(rect.xhi - 8, P.x + 320);
    const z0 = Math.max(rect.zlo + 8, P.z - 320), z1 = Math.min(rect.zhi - 8, P.z + 320);
    if (!(x1 > x0 && z1 > z0)) return null;
    const refSolid = (gx, gy, gz) => {               // bfObst's rule, written out again — this validates nvPass, the brick skipping AND staleness, none of which it shares
      const ii = gx + gy * WX + gz * WX * WY, id = W[ii];
      return id !== 0 && solidTab[id] === 1 && !foliaTab[id] && !coneTab[id] && !snowPassTab[id] && !snowFernTab[id] && !stampedIdx.has(ii); };
    const refSolidW = (gx, gy, gz) => {               // …and the WALKER's rule, written out again the same way: the same voxel minus the small ground clutter bfObstW passes. Independent of nvClut, of nvColTop's single-scan trick and of nvK.
      const ii = gx + gy * WX + gz * WX * WY, id = W[ii];
      return refSolid(gx, gy, gz) && !WORM_PASS.has(id); };
    let cells = 0, fresh = 0, staleY = 0, staleC = 0, staleD = 0, staleK = 0, worst = 0, worstK = 0, unbuilt = 0;
    for (let q = 0; q < n; q++) {                    // (1) IS THE STORED CELL WHAT A PLAIN SCAN WOULD SAY RIGHT NOW — the maintenance invariant
      const x = x0 + Math.random() * (x1 - x0), z = z0 + Math.random() * (z1 - z0);
      const ci = nvIdx(x, z);
      if (!(nvF[ci] & NVF_BUILT)) { unbuilt++; continue; }
      const cx = ci % NVX, cz2 = (ci - cx) / NVX, gx0 = cx << NVSH, gz0 = cz2 << NVSH;
      let top = -30000, wetN = 0, wtop = -30000;
      const cts = [0, 0, 0, 0], cwet = [0, 0, 0, 0];
      for (let qq = 0; qq < 4; qq++) {
        const gx = gx0 + (qq & 1), gz = gz0 + (qq >> 1);
        let t = -1, tw = -1;
        for (let y = Math.min(WY - 2, hmap[gx + gz * WX] + 20); y >= Math.max(1, hmap[gx + gz * WX] - 20); y--) if (refSolid(gx, y, gz)) { t = y; break; }
        for (let y = Math.min(WY - 2, hmap[gx + gz * WX] + 20); y >= Math.max(1, hmap[gx + gz * WX] - 20); y--) if (refSolidW(gx, y, gz)) { tw = y; break; }
        if (t < 0) t = Math.max(1, hmap[gx + gz * WX] - 20) - 1;
        if (tw < 0) tw = Math.max(1, hmap[gx + gz * WX] - 20) - 1;
        if (tw > wtop) wtop = tw;
        cts[qq] = t; if (t > top) top = t;
        const wv = W[gx + WL * WX + gz * WX * WY];
        cwet[qq] = (wv === WATER_T || wv === WATER_B) ? 1 : 0; wetN += cwet[qq];
      }
      const ts = (wetN !== 0 && top < WL) ? WL : top;
      let cmin = 255, dmin = 255;
      for (let qq = 0; qq < 4; qq++) {
        const gx = gx0 + (qq & 1), gz = gz0 + (qq >> 1);
        let c = 0; for (let y = ts + 1; y <= Math.min(WY - 1, ts + NV_CCAP); y++) { if (refSolid(gx, y, gz)) break; c++; }
        if (c < cmin) cmin = c;
        let d = 0; if (cwet[qq]) { for (let y = cts[qq] + 1; y <= Math.min(WY - 2, WL); y++) { if (refSolid(gx, y, gz)) break; d++; } }
        if (d < dmin) dmin = d;
      }
      cells++;
      const dy = Math.abs(nvY[ci] - top), dc = Math.abs(nvC[ci] - cmin), dd = Math.abs(nvD[ci] - dmin);
      let kref = top - wtop; if (kref < 0) kref = 0; if (kref > 15) kref = 15;   // the SAME clamp the builder applies, so the reference and the plane are asked the identical question
      const dk = Math.abs(nvClutD(ci) - kref);
      if (dy) staleY++; if (dc) staleC++; if (dd) staleD++; if (dk) staleK++;
      if (dk > worstK) worstK = dk;
      if (dy === 0 && dc === 0 && dd === 0) fresh++;
      const m = Math.max(dy, dc, dd); if (m > worst) worst = m;
    }
    let air = 0, airFree = 0, consViol = 0, solViol = 0, solViolHard = 0;
    for (let q = 0; q < n; q++) {                    // (2)+(3) does the field ever claim room the voxels deny — vs bfObst, and vs the PLAYER's own solid()
      const x = x0 + Math.random() * (x1 - x0), z = z0 + Math.random() * (z1 - z0);
      const g = hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX];
      const y = Math.max(1, Math.min(WY - 4, Math.floor(g - 6 + Math.random() * 60)));
      air++;
      if (!navFitsAir(x, y, z)) continue;
      airFree++;
      const rgx = gwrap(Math.floor(x), WX), rgz = gwrap(Math.floor(z), WZ);
      if (refSolid(rgx, y, rgz) || refSolid(rgx, y + 2, rgz)) consViol++;   // the CONSERVATIVE gate is answered by refSolid, not by the field's own tables — a circular check would report clean while the field lied
      if (solid(Math.floor(x), y, Math.floor(z))) { solViol++;
        const id = W[gwrap(Math.floor(x), WX) + y * WX + gwrap(Math.floor(z), WZ) * WX * WY];
        if (!(foliaTab[id] || coneTab[id] || snowPassTab[id] || snowFernTab[id])) solViolHard++; }
    }
    return { cells, unbuilt, fresh: cells ? +(fresh / cells).toFixed(4) : -1, staleY, staleC, staleD, staleK, worstDelta: worst, worstK,
      airSamples: air, airFree, conservative: airFree ? +(1 - consViol / airFree).toFixed(4) : -1, consViol,
      vsSolidViol: solViol, vsSolidHard: solViolHard,
      chamCycles: nvChCycles }; };
  __vb.navplanes = (x, z) => { const ci = nvIdx(x === undefined ? P.x : x, z === undefined ? P.z : z);
    return { ci, y: nvY[ci], ts: nvTop(ci), c: nvC[ci], d: nvD[ci], k: nvClutD(ci), walkY: navWalkGround(x === undefined ? P.x : x, z === undefined ? P.z : z), standY: navWalkStand(x === undefined ? P.x : x, z === undefined ? P.z : z), o: nvO[ci], f: nvF[ci], cls: nvClassOf(ci),
      water: !!(nvF[ci] & NVF_WATER), swim: !!(nvF[ci] & NVF_SWIM), land: !!(nvF[ci] & NVF_LAND), roof: !!(nvF[ci] & NVF_ROOF), built: !!(nvF[ci] & NVF_BUILT) }; };

