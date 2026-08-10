  // ── SUPPORT FLOOD ── one component out of one seed. Class-restricted, 26-connected, capped, with the
  // O(1) hmap anchor as an early-out on every pop, so an ordinary ground carve is answered on the FIRST
  // pop and costs nothing. Visited is a Set of flat indices: measured 300-600 ns/cell and zero extra
  // memory, against 192 MiB for a full-window bitset at this machine's 2048x384x2048 rung — on top of W's
  // 1.5 GiB, in a title that ships as a local browser game on players' own hardware.
  // A CONDUIT cell (a creature grid stamp, whatever its palette id) is traversable in the DRAPE flood but
  // never lands in `comp`: it is not liftable, and it is never an anchor either.
  const supWorldX = (gx) => winOX + (((gx - gwrap(winOX, WX)) % WX) + WX) % WX;   // window column -> the world column it currently stands for; bodies live in world space, so a component must be un-wrapped before it becomes one
  const supWorldZ = (gz) => winOZ + (((gz - gwrap(winOZ, WZ)) % WZ) + WZ) % WZ;
  // ── OBJECT IDENTITY: TRIED 2026-08-08, REVERTED, KEEP THE MEASUREMENTS ─────────────────────────────────
  // The plan was to give the support graph OBJECT identity (which model owns this voxel, derived from the
  // placement grids — W is ~1.5 GiB, a per-voxel id array is not affordable) and to refuse drape edges between
  // two different trees, so a severed trunk could not be held up by the neighbouring canopy.
  // It works as far as it goes and it is NOT enough, twice over:
  //   * A crown also reaches DOWN past the cut to its own stump, so foliage still carries support across a
  //     structural break inside ONE tree. Object identity cannot see that; only the tree's own root flood can.
  //   * Cutting cross-tree edges splits every interlocked canopy into its own component, which multiplies the
  //     drape floods. MEASURED on the regression gate: queue `carried` 0 -> 39,442 after a storm alone and
  //     61,455 after felling, resolution 2.5 s -> never, and 3 real floaters left standing. Net worse.
  // Pairing it with a tree re-flood on any wood removal (stage 2) did fix the severed trunk — 401 wood voxels
  // above the cut -> 0, deterministic over two runs, with standing pines intact (13 wood / 19,790 foliage lost)
  // — but it also fragmented felling: a trunk came loose in 25 swings instead of ~90, as three bodies, and the
  // drape carry stopped (snow on the trunk 290 -> 0).
  // CONCLUSION: the direction is right and the cost lives in the DRAPE flood being per-component. Doing this
  // properly needs the canopy to stop being a flood at all — foliage should be resolved by the object that
  // owns it, not by walking it — which is a bigger change than a gate on one edge.
  const SUPWHY = { why: '-', d: 0 };                    // WHICH branch declared a component anchored — diagnostic for the probe
  const supFlood = (ii0, probe, depth) => {
    const v0 = W[ii0]; if (!v0) return null;
    if (stampedIdx.has(ii0)) return null;              // CONDUIT: never liftable, so never a seed either
    const cls0 = SUP.CLASS[v0];
    if (cls0 === SUP.FLUID) return null;               // a lake is anchored by construction and is never lifted
    const drape = cls0 === SUP.DRAPE, cap = drape ? SUP.drapeCap : SUP.cap, dep = (depth | 0) + 1;
    const seen = new Set([ii0]), st = [ii0], comp = [];
    SUP.busy.add(ii0);
    let anchored = false, capped = false;
    SUPWHY.why = '-'; SUPWHY.d = 0;
    while (st.length) {
      const ii = st.pop();
      const iCone = drape && !!coneTab[W[ii]];
      const iSnow = drape && !!snowTab[W[ii]];       // …and whether it is SNOW — see the cantilever rule below        // constant across this cell's 26 neighbours — see the cone rule below
      const icond = stampedIdx.has(ii);
      if (!icond) {
        if (supAnchored(ii)) { anchored = true; SUPWHY.why = 'supAnchored'; break; }
        comp.push(ii);
        if (comp.length >= cap) { capped = true; break; }
      }
      const gx = ii % WX, gy = ((ii / WX) | 0) % WY, gz = (ii / (WX * WY)) | 0;
      for (let dy = 1; dy >= -1 && !anchored; dy--) {  // DOWNWARD LAST, so a LIFO pop takes it FIRST: the anchor is always below, and diving beats wandering the canopy. Measured on a mid-trunk seed this is the difference between ~960 cells visited and a handful.
        const ny = gy + dy; if (ny < 1 || ny >= WY) continue;
        for (let dz = -1; dz <= 1 && !anchored; dz++) for (let dx = -1; dx <= 1; dx++) {
          if (!dx && !dy && !dz) continue;
          const nn = gwrap(gx + dx, WX) + ny * WX + gwrap(gz + dz, WZ) * WX * WY;
          const nv = W[nn]; if (!nv) continue;
          const ncls = stampedIdx.has(nn) ? SUP.CONDUIT : SUP.CLASS[nv];
          if (ncls === SUP.FLUID) { anchored = true; SUPWHY.why = 'fluid'; break; }   // fluid terminates BOTH floods
          if (drape) {
            // ── A PINECONE HANGS FROM WHAT IS DIRECTLY ABOVE IT ── and this has to be decided BEFORE the
            // structure test below, or a cone merely brushing a BRANCH on the diagonal is declared held by it.
            // ── SNOW DOES NOT CANTILEVER (user 2026-08-07: cutting a tree while it snows leaves the snow
            // hanging exactly where it was) ── a blanket was one 26-connected DRAPE component, anchored if ANY
            // part of it reached held structure. So a sheet spanning a felled crown and a neighbouring one
            // stayed anchored THROUGH THE NEIGHBOUR, and the span over the now-empty gap hung in the air —
            // measured 10 voxels with a 14-voxel drop, all reported anchored on a 409-voxel component. Snow is
            // held by what is directly UNDER it and nothing else, so a link touching snow has to be vertical.
            if ((iSnow || !!snowTab[nv]) && !(dx === 0 && dz === 0)) { continue; }
            if (iCone !== !!coneTab[nv]) {
              if (!(dx === 0 && dz === 0 && dy === (iCone ? 1 : -1))) { continue; }
              // ── SNOW IS A LOAD, NEVER A HANGER (user 2026-08-07: "floating pinecones WITH snow") ── the link
              // above says a cone may reach its hanger straight up, and a snow cap sitting ON a cone is straight
              // up. So the cone joined the snow, the snow joined the wider blanket, the blanket reached a crown,
              // and the resolver called the cone ANCHORED by its own snow cap. That is why probing reported zero
              // floaters while they were plainly hanging there. Snow rests ON things and holds nothing up.
              if (iCone ? !!snowTab[nv] : !!snowTab[W[ii]]) { continue; }
            }
            if (ncls === SUP.STRUCTURE) { if (supHeld(nn, dep)) { anchored = true; SUPWHY.why = 'drape->heldStructure id' + W[nn]; break; } continue; }   // TERMINATE on a held structure; never ENTER one, and never be held up by one that is itself floating
            if (seen.has(nn)) continue;
            seen.add(nn); SUP.busy.add(nn); st.push(nn);        // DRAPE and CONDUIT only
          } else {
            // ── FOLIAGE HOLDING UP STRUCTURE: TRIED TO REMOVE THIS, MEASURED, PUT BACK (2026-08-08) ──
            // This bridge is the mechanism behind the floating trunk: cut a pine anywhere and its own crown
            // reaches down past the cut to the stump, so the severed section is "attached" through needles.
            // MEASURED with supWhy on a trunk severed outside the chop path: `struct->heldDrape id50`, 401
            // wood voxels above the cut, still there after a forced flush. Deleting the line does fix exactly
            // that — the trunk falls, 401 -> 0 — and a forest census over a full storm came back 0 wood, 0
            // foliage, 0 cones lost, which looks like a clean win and is not one.
            // The census was the wrong instrument: lifted wood LEAVES W, falls, and physRetire bakes it back,
            // so the net count is unchanged while the churn is enormous. The honest numbers, calm forest:
            //     liftedVox 1783 -> 8091   (4.5x more geometry torn loose)
            //     frame      4.60 -> 11.60 ms      storm 13.40 -> 11.40, thaw 5.10 -> 11.20
            // That is pine tips coming loose and raining down continuously, because a pine's upper trunk
            // genuinely is not one connected WOOD shape — its tip is wood embedded in needles. The crutch is
            // still load-bearing.
            //
            // ── AND EVERY GRAPH-LEVEL REPAIR IS DEAD, MEASURED (2026-08-08) ── four were built and priced;
            // none can work, because the cause is in the ASSET rather than in this graph:
            //   * remove this bridge          -> pine tips decapitate; liftedVox 1783 -> 8091, 4.6 -> 11.6 ms
            //   * object identity (refuse     -> interlocked canopies fragment into separate components,
            //     drape edges between trees)     SUP queue 0 -> 40k, frame 28-41 ms
            //   * let the tree's OWN root      -> it does not sever either: after 25 axe swings through a
            //     flood decide (phFlood)          trunk phFlood still reaches 8204 of 8286 voxels, because it
            //                                     walks the model INCLUDING foliage and the crown spans the
            //                                     cut. It also costs 3.70 ms/call against a 2 ms budget.
            //   * wood-only structural flood  -> MEASURED on a standing pine: 26-connected, wood only, from
            //                                     the buried base, it reaches 2638 of 2784 wood voxels and
            //                                     stops 23 VOXELS SHORT OF THE TOP.
            // That last number is the whole story: pine5.vox's bole is discontinuous near the tip, so needles
            // are load-bearing there by construction, and any rule that stops foliage carrying wood takes the
            // tree's point off. The fix belongs in the MODEL — make the bole continuous wood to the apex —
            // after which this bridge can simply be deleted and a severed trunk falls on its own.
            if (ncls === SUP.DRAPE) { if (supHeld(nn, dep)) { anchored = true; SUPWHY.why = 'struct->heldDrape id' + W[nn]; break; } continue; }   // the same rule the other way round: ASK, never enter
            if (ncls !== SUP.STRUCTURE || seen.has(nn)) continue;   // fluid is handled above; a creature stamp is a hole in this graph, exactly as stampApply guarantees
            seen.add(nn); SUP.busy.add(nn); st.push(nn);
          }
        }
      }
      if (anchored) break;
    }
    if (drape) SUP.stats.drapeFloods++; else SUP.stats.structFloods++;
    if (capped) SUP.stats.capHits++;
    // ── A CAP HIT IS "I DID NOT FINISH", NOT "IT IS ATTACHED" (user 2026-08-07: "if anything in the entire
    // game is deemed floating it falls instantly") ── this used to answer ANCHORED whenever the walk ran out of
    // budget, and then MEMOISE that answer over every cell it had visited, which is what made a big floater
    // permanent rather than merely late. An unfinished walk now memoises NOTHING and reports undecided, so the
    // next pass re-asks with a clean slate. It still never drops what it could not prove loose — that would be
    // the destructive failure this whole design refuses — it simply stops claiming to know.
    const held = anchored;
    if (capped) { for (const s of seen) SUP.busy.delete(s); }
    else { const dst = held ? SUP.ancS : SUP.flS;      // memoise the verdict for the WHOLE component, both ways, for the rest of this pass
           for (const s of seen) { SUP.busy.delete(s); dst.add(s); } }
    if (probe) return { cls: drape ? 'drape' : 'structure', anchored, capped, n: comp.length, visited: seen.size };
    if (capped) return { anchored: false, capped: true, undecided: true, drape, comp: [], seen: new Set() };   // empty comp: supFlush skips it, and the seed stays queued for another try
    return { anchored: held, capped, drape, comp, seen };   // the caller also marks every visited cell resolved: abandoning a component mid-walk is what let the old passes re-discover the remainder as fresh, SMALLER, droppable components
  };
  // ── IS THIS CELL'S COMPONENT HELD? ── the cross-class anchor query, and the one place the two graphs are
  // allowed to speak to each other.
  //
  // TRAVERSAL stays strictly asymmetric: the structure flood never ENTERS a drape cell and the drape flood
  // never enters a structure cell, so a crown can never join a wood component and can never be LIFTED WHOLE.
  // That is the mechanism behind the measured 86 -> 1007 blow-up, and it stays structurally impossible.
  // What crosses is only the VERDICT; each component is still adjudicated, and lifted, entirely on its own.
  //
  // It has to cross, because MEASURED the pine model's upper trunk is not one connected wood shape. Walking
  // a standing trunk with a structure-only flood, every course up to y = ground+84 reached the buried roots
  // and every course above it came back a 50-voxel island anchored to nothing: the tip is wood embedded in
  // needles. Without this the resolver decapitated every pine in the world the moment one snowflake landed
  // in its crown — a false positive, i.e. precisely the failure this whole design refuses.
  //
  // Cycles are broken by SUP.busy: a component still being walked cannot serve as anyone else's anchor,
  // which is both the correct graph answer and what stops wood and needles vouching for each other in a
  // ring. The depth guard is a backstop only, and it errs ANCHORED — leaving a floater, never destroying
  // geometry, which is the direction every other choice in here also leans.
  //
  // ── DO NOT TRY TO "UN-MEMOISE" THE GUESS (tried and reverted 2026-08-08) ── erring anchored is durable: the
  // verdict is written into SUP.ancS for every cell the walk visited, so one recursion-limited guess makes the
  // whole component attached for the rest of the pass. Treating that like a cap hit — memoise nothing, report
  // undecided, re-ask next pass — looks obviously right and is catastrophic, because the anchored memo is what
  // makes supHeld affordable at all: without it every cross-class query re-floods from scratch, recursion runs
  // straight to the limit, and each new guess suppresses more memoisation. MEASURED on that version:
  // depthHits 2,264,153 and a single supFlush taking 3,399 ms. The memo is load-bearing. If the false anchor
  // ever proves to matter, the fix is a cheaper anchor oracle, not a weaker memo.
  const SUP_MAX_DEPTH = 32;
  const supHeld = (ii, depth) => {
    if (supAnchored(ii)) return true;
    if (SUP.ancS.has(ii)) return true;
    if (SUP.flS.has(ii)) return false;
    if (SUP.busy.has(ii)) return false;                // already on the stack: it cannot vouch for us, because we are what it is waiting on
    if ((depth | 0) >= SUP_MAX_DEPTH) { SUPWHY.d++; SUP.stats.depthHits++; return true; }
    const r = supFlood(ii, false, depth);
    return r ? r.anchored : false;
  };
  // ── ONE COMPONENT, ONE BODY ── extracted from supResolve so the OVERSIZE path can call it per part: a
  // component too big to be a single rigid body is bisected and each piece comes through here, which is what
  // lets a severed pine come down as four logs instead of hanging in the air (user 2026-08-07).
  const supDrop = (comp0, drape) => {
    // ── AND IT TAKES ITS SNOW WITH IT (user 2026-08-07: "the snow should stick to the pine tree as it falls") ──
    // Snow is a SEPARATE voxel resting one layer above whatever holds it, so a crown lifting out from under its
    // blanket left the white hanging in mid-air. This is the same rule the chop bite already follows: the snow
    // joins the BODY, rides down with the tree and lands with it, instead of being stranded or shed separately.
    // DRAPE lifts too, and they are the ones that matter: the structure flood carries WOOD only, so a felled
    // pine's needles come out as their own drape component — and canopy snow rests on NEEDLES, not on the trunk.
    // Walk up from any component voxel that is not itself snow, deduped against the component so a snow stack
    // that is already part of it is never added twice.
    var comp = comp0;
    if (comp0.length) {
      const inComp = new Set(comp0), addS = [];
      for (const ii of comp0) {
        if (snowTab[W[ii]]) continue;                    // a snow voxel's own stack is already in here
        const gy0 = ((ii / WX) | 0) % WY;
        for (let y2 = gy0 + 1; y2 < WY - 1; y2++) {
          const jj = ii + (y2 - gy0) * WX;               // +1 in y is +WX in the flat index
          const v2 = W[jj]; if (!v2 || !snowTab[v2]) break;
          if (inComp.has(jj)) break;
          inComp.add(jj); addS.push(jj);
        }
      }
      if (addS.length) comp = comp0.concat(addS);
    }
    const n = comp.length; if (!n) return 0;
    if (PH.bodies.length >= PH.maxBodies && !phMakeRoom()) {   // ── SHED THE OLDEST BODY FIRST (user 2026-08-07: "pinecones are still floating after the pine tree has fallen down") ── a cone is 13 voxels and fails none of the size caps; it failed HERE. One felling fills all 16 slots (the toppling trunk plus every needle tuft phSeparate requeues), so each cone hit this branch and was requeued forever. The SUP_BLOCK_MAX release valve could never fire either: SUP.blocked resets to 0 on any frame the 2 ms slice does not REACH the blocked component, which on a long post-felling queue is every frame — a starvation livelock, not a cap. Same idiom phChopLeaves and phChopDecor already use, and it still never erases world geometry.
      SUP.blockedNow = 1; supPush(comp[0]);
      return 0;
    }
    const cells = new Array(n);
    const id0 = W[comp[0]] || 0;
    for (let i = 0; i < n; i++) { const ii = comp[i];
      cells[i] = [supWorldX(ii % WX), ((ii / WX) | 0) % WY, supWorldZ((ii / (WX * WY)) | 0)]; }
    phSrc = 'support';
    const fb = phBodyFromCells(cells);
    if (!fb) return 0;
    fb.omega[0] = (Math.random() - 0.5) * 1.4;         // it lost its hold rather than being struck: it drops, it does not fly
    fb.omega[1] = (Math.random() - 0.5) * 1.4;
    fb.omega[2] = (Math.random() - 0.5) * 1.4;
    // ── IT FELL, IT WAS NOT CUT: NO FLIGHT INTO YOUR CHEST (user 2026-08-05, pinecones off a felled pine) ──
    // a body with absorbAt set SKIPS THE DISTANCE GATE ENTIRELY — the range check lives inside the
    // `no timer yet` branch of the absorb step, so a body whose timer has elapsed falls straight through to
    // absorbing at any range. Every cone a felled pine shed therefore came flying across the clearing 450 ms
    // later. The timer is right for a chunk the player CARVED — they aimed at it and it is owed to them — and
    // wrong for anything that merely lost its support. Same rule the arrow's debris already follows: it lies
    // where it lands and is collected by walking up to it, inside PH.absorbR.
    fb.absorbAt = 0;
    fb.nearR = PH.absorbR;
    PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(fb); PH.stats.chunks++;
    { let dx0 = 1e9, dx1 = -1e9, dy0 = 1e9, dy1 = -1e9, dz0 = 1e9, dz1 = -1e9;   // the FELL itself comes through here
      for (const ii of comp) {
        const gx2 = ii % WX, gy2 = ((ii / WX) | 0) % WY, gz2 = (ii / (WX * WY)) | 0;
        if (gx2 < dx0) dx0 = gx2; if (gx2 > dx1) dx1 = gx2;
        if (gy2 < dy0) dy0 = gy2; if (gy2 > dy1) dy1 = gy2;
        if (gz2 < dz0) dz0 = gz2; if (gz2 > dz1) dz1 = gz2;
      }
      coneWake(dx0, dx1, dy0, dy1, dz0, dz1); }
    SUP.stats.lifted++; SUP.stats.liftedVox += n;
    if (drape) SUP.stats.liftedDrape += n; else SUP.stats.liftedStruct += n;
    if (SUP.log.length < 64) SUP.log.push([n, id0, cells[0][1], drape ? 'D' : 'S']);   // a short, capped ring for __vb.support(): size, palette id, height, class — enough to tell a stranded needle clump from a hillside
    return 1;
  };
  // ── OUTCOME ── one table, replacing four. The only erase path is n < 3.
  const supResolve = (r) => {
    const comp = r.comp, n = comp.length;
    if (n < 3) {                                       // a lone detached voxel is litter, needle or not (the shipped rule)
      const gone = [];
      for (const ii of comp) if (W[ii]) { W[ii] = 0; gone.push(ii); }
      if (gone.length) { gpuPatch(gone, false); PH.stats.dustVox += gone.length; SUP.stats.erasedSmall += gone.length; }
      wakeFrom(gone, 6);                // leaves gone -> whatever hung on them must be re-asked
      return 1;
    }
    if (n > PH.absorbMax) {                            // ── TOO BIG FOR ONE BODY IS NOT TOO BIG TO FALL (user 2026-08-07) ── this returned 0 and left the
      // component hanging. A rigid body has a size ceiling; gravity does not. Bisect along the longest axis
      // until every part fits, then drop the parts — a severed pine comes down as four logs instead of not at
      // all. Parts are spatially coherent boxes, not arbitrary slices, so they read as the thing breaking up.
      const parts = [], stack = [comp];
      while (stack.length) {
        const cur = stack.pop();
        if (cur.length <= PH.absorbMax) { parts.push(cur); continue; }
        let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9;
        for (const ii of cur) { const gx = ii % WX, gy = ((ii / WX) | 0) % WY, gz = (ii / (WX * WY)) | 0;
          if (gx < x0) x0 = gx; if (gx > x1) x1 = gx; if (gy < y0) y0 = gy; if (gy > y1) y1 = gy; if (gz < z0) z0 = gz; if (gz > z1) z1 = gz; }
        const ex = x1 - x0, ey = y1 - y0, ez = z1 - z0;
        const ax = (ey >= ex && ey >= ez) ? 1 : (ex >= ez ? 0 : 2);
        const mid = ax === 1 ? (y0 + y1) * 0.5 : (ax === 0 ? (x0 + x1) * 0.5 : (z0 + z1) * 0.5);
        const lo = [], hi = [];
        for (const ii of cur) { const gx = ii % WX, gy = ((ii / WX) | 0) % WY, gz = (ii / (WX * WY)) | 0;
          const k = ax === 1 ? gy : (ax === 0 ? gx : gz);
          (k <= mid ? lo : hi).push(ii); }
        if (!lo.length || !hi.length) { parts.push(cur); continue; }   // degenerate split (every cell on one side) — take it as it is rather than looping
        stack.push(lo, hi);
      }
      let did = 0;
      for (const pc of parts) did += supDrop(pc, r.drape);
      if (did) return did;
      SUP.stats.tooBig = (SUP.stats.tooBig | 0) + 1;
      // ── A REFUSAL IS NOT AN ANSWER: PUT IT BACK IN THE QUEUE (2026-08-08) ── every part failed to become a
      // body, which in practice means the body budget was exhausted at this instant. Returning 0 and walking
      // away left a component the resolver had PROVEN detached sitting in W with nothing scheduled to look at
      // it again: its seed had already been consumed, and no later pass revisits a cell unless something within
      // 26 of it is disturbed. Re-seeding costs one queue entry and turns a permanent floater into a late one.
      supPush(comp[0]);
      if (SUP.refused.length < 32) SUP.refused.push({ n, id: W[comp[0]] || 0, x: supWorldX(comp[0] % WX), y: ((comp[0] / WX) | 0) % WY, z: supWorldZ((comp[0] / (WX * WY)) | 0), why: 'tooBig', cap: PH.absorbMax });
      return 0; }   // cannot become a rigid body at all, so the only choices are leave it or erase it — and erasing real geometry is worse than a floater
    return supDrop(comp, r.drape);
  };
  const SUP_BLOCK_MAX = 30;                            // frames the resolver may sit blocked on a full body budget before it forces a slot
  // ── THE PASS ── seeds are every enqueued cell that is occupied PLUS every occupied 26-neighbour of it.
  // That is the fix for the deepest defect in the scheme this replaces: all four old tests were boxes
  // centred on the IMPACT POINT while the geometry that changed is elsewhere — phSeparate removes a crown
  // 40-90 voxels above the cut, and the two sweeps that followed were centred on the cut.
  // Lifts append their vacated cells to the same queue (through gpuPatch), so cascades settle inside this
  // pass or carry to the next frame. The queue is persistent; nothing is ever dropped on the floor.
  const supFlush = (force, budget) => {
    if (!SUP.on || SUP.qh >= SUP.q.length) { SUP.blockedNow = 0; return 0; }
    const t0 = performance.now();
    let acted = 0, pass = 0;
    const maxPass = force ? 64 : SUP.maxPasses;
    const lim = force ? (budget === undefined ? Infinity : budget) : SUP.msBudget;   // a forced flush still takes a ceiling: a caller that needs the answer NOW (see arrowChop) must not be able to stall a frame on a storm-sized queue
    SUP.blockedNow = 0;
    while (SUP.qh < SUP.q.length && pass < maxPass) {
      pass++; SUP.stats.passes++;
      SUP.res.clear(); SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();   // cleared BETWEEN passes, never within one: dedup inside a pass, honest re-adjudication after a lift
      const end = SUP.q.length;                        // lifts append beyond this — they are the NEXT cascade round, not this one
      let out = false;
      while (SUP.qh < end) {
        if (performance.now() - t0 > lim) { out = true; break; }   // checked BETWEEN queued cells, never mid-flood: a component is always resolved as a whole
        const q0 = SUP.q[SUP.qh++]; SUP.qs.delete(q0);
        let redo = false;                            // any undecided flood off this seed -> ask again on a later flush
        const gx = q0 % WX, gy = ((q0 / WX) | 0) % WY, gz = (q0 / (WX * WY)) | 0;
        for (let dy = -1; dy <= 1; dy++) {
          const ny = gy + dy; if (ny < 1 || ny >= WY) continue;
          for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
            const ii = gwrap(gx + dx, WX) + ny * WX + gwrap(gz + dz, WZ) * WX * WY;
            if (!W[ii] || SUP.res.has(ii)) continue;
            const r = supFlood(ii, false);
            if (!r) { SUP.res.add(ii); continue; }
            if (r.undecided) { redo = true; continue; }   // unfinished walk: it memoised nothing, so re-seed it rather than drop the question on the floor
            for (const s of r.seen) SUP.res.add(s);    // anchored, capped or lifted — the whole component has had its answer for this pass
            if (r.anchored || !r.comp.length) continue;
            acted += supResolve(r);
          }
        }
        if (redo && SUP.retry.length < 4096) SUP.retry.push(q0);
      }
      if (out) break;
    }
    // Re-seed AFTER the pass loop, never inside it: appending mid-flush would let one capped component spin
    // through every remaining pass of the same frame instead of yielding and being re-asked on the next.
    if (SUP.retry.length) { for (const s2 of SUP.retry) if (!SUP.qs.has(s2)) { SUP.qs.add(s2); SUP.q.push(s2); } SUP.retry.length = 0; }
    if (SUP.qh > 8192) { SUP.q = SUP.q.slice(SUP.qh); SUP.qh = 0; }   // in-place-ish compaction, same shape as the snow queues
    SUP.stats.carried = SUP.q.length - SUP.qh;
    if (SUP.blockedNow) { SUP.stats.blockedFrames++;
      if (++SUP.blocked >= SUP_BLOCK_MAX) { SUP.blocked = 0; if (phMakeRoom()) SUP.stats.reclaims++; } }
    else SUP.blocked = 0;
    const ms = performance.now() - t0;
    SUP.stats.ms = +ms.toFixed(3);
    if (ms > SUP.stats.msMax) SUP.stats.msMax = +ms.toFixed(3);
    return acted;
  };
  // ── CHOP LEAVES ── foliage comes off with ANY tool (user). Deliberately NOT routed through
  // phChopDecor: that runs an orphan sweep afterwards, and a canopy is precisely the shape it misjudges.
  // Leaves have no hitbox and no physics, so there is nothing to drop — they just go.
  // ── phFallLoose IS GONE ── it was the PROXIMITY rule: a multi-source 26-connected walk out of every
  // occupied non-loose voxel, through loose ones, capped at FALL_HOPS = 26. Four things were wrong with
  // it and none of them was tunable. Its support set was never itself validated, so a needle clump three
  // hops from a severed HANGING log read as held. It could not see solid material at all — wood, rock,
  // soil and ferns could only ever BE support, never fall. Its wall and over bail-outs silently left
  // floaters, and its size guard LEAKED: the break mid-walk left the remainder discoverable as fresh,
  // smaller, DROPPABLE components. And it erased any 2000-3000 voxel needle mass — a genuine crown — as
  // a routine path. It cost 1.3 ms of scan plus a 21 ms walk over a 73-cubed box, fired four times a
  // second whether or not anything had been carved, against a 16.7 ms frame. supFlush replaces all of it
  // at a 2 ms/frame ceiling that is only ever paid when something actually changed.
  const phChopLeaves = (wx, wy, wz, rad, bite) => { phSrc = 'chopLeaves';
    const r2 = rad * rad, ri = Math.ceil(rad), cx0 = Math.round(wx), cy0 = Math.round(wy), cz0 = Math.round(wz);
    const cD = [], cI = [], cX = [], cY = [], cZ = [];   // world coords too: the chunk is built from cells, not indices
    for (let dy = -ri; dy <= ri; dy++) for (let dz = -ri; dz <= ri; dz++) for (let dx = -ri; dx <= ri; dx++) {
      const d2 = dx * dx + dy * dy + dz * dz; if (d2 > r2) continue;
      const y = cy0 + dy; if (y < 1 || y >= WY) continue;
      const x = cx0 + dx, z = cz0 + dz;
      const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
      if (!foliaTab[W[ii]]) continue;
      cD.push(d2); cI.push(ii); cX.push(x); cY.push(y); cZ.push(z);
    }
    if (!cD.length) return false;
    const ord = new Int32Array(cD.length);               // nearest first, so a swing takes the leaves it was aimed at
    for (let k = 0; k < ord.length; k++) ord[k] = k;
    ord.sort((a, b) => cD[a] - cD[b]);
    const take = Math.min(bite, ord.length), cells = [];
    for (let k = 0; k < take; k++) { const j = ord[k]; if (W[cI[j]]) cells.push([cX[j], cY[j], cZ[j]]); }
    if (!cells.length) return false;
    // ── IT BREAKS OFF ── a clump of leaves comes away as a chunk and falls (user), the same as any other
    // material. phBodyFromCells clears the cells from the grid and hands back the body.
    if (PH.bodies.length >= PH.maxBodies && !phMakeRoom()) {   // no room for another body: erase rather than leave it hanging
      const gone = [];
      for (const c of cells) { const ii = gwrap(c[0], WX) + c[1] * WX + gwrap(c[2], WZ) * WX * WY; if (W[ii]) { W[ii] = 0; gone.push(ii); } }
      if (gone.length) { gpuPatch(gone, false); PH.stats.dustVox += gone.length; }
      wakeFrom(gone, 6);                // leaves gone -> whatever hung on them must be re-asked
      return gone.length > 0;
    }
    // (the 1-hop, erase-only scatter pass that used to sit here is gone: it asked only whether a
    //  neighbour still had ONE occupied non-floatTab 6-neighbour, and erased it outright if not.
    //  supFlush answers the same question over the whole component and DROPS what it takes.)
    const lb = phBodyFromCells(cells);
    if (!lb) return false;
    lb.omega[0] = (Math.random() - 0.5) * 2.2;         // it was knocked loose, not struck: a lazy tumble, not a spin
    lb.omega[1] = (Math.random() - 0.5) * 2.2;
    lb.omega[2] = (Math.random() - 0.5) * 2.2;
    lb.absorbAt = performance.now() + PH.absorbMs;     // …and it can be gathered up like any other chunk
    PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(lb); PH.stats.chunks++;
    return true;
  };
  // ── CHOP DECOR ── a mushroom belongs to no tree, so physChopAt (which works in a pine's model frame)
  // can never reach it. This is the same nearest-first, fixed-count carve done straight in world
  // coordinates, so a bite out of a mushroom is the same size as a bite out of a trunk.
  const phChopDecor = (wx, wy, wz, rad, bite, ok) => {   // ok(v): which materials THIS tool may take. Without it the sphere eats anything choppable it reaches, whatever it was aimed at.
    const r = rad, r2 = r * r;
    const cD = [], cI = [], cX = [], cY = [], cZ = [], cV = [];
    const ri = Math.ceil(r);                         // WHOLE-voxel steps (see physChopAt) — a fractional radius made every index undefined
    const want = bite === undefined ? PH.chopBite : bite;
    const cx0 = Math.round(wx), cy0 = Math.round(wy), cz0 = Math.round(wz);
    let spare = -1;                                  // shells still to walk once we already hold enough candidates
    for (let sh = 0; sh <= ri; sh++) {               // Chebyshev shells, nearest first — see the note above
      if (spare === 0) break;                        // enough in hand, and one further shell already walked to cover the corners of the last one
      if (spare > 0) spare--;
      for (let dy = -sh; dy <= sh; dy++) for (let dz = -sh; dz <= sh; dz++) for (let dx = -sh; dx <= sh; dx++) {
        if (Math.max(Math.abs(dx), Math.abs(dy), Math.abs(dz)) !== sh) continue;   // shell surface only; the interior was covered by earlier shells
        const d2 = dx * dx + dy * dy + dz * dz; if (d2 > r2) continue;
        const y = cy0 + dy; if (y < 1 || y >= WY) continue;
        const x = cx0 + dx, z = cz0 + dz;
        const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
        const v = W[ii]; if (!v || !decorTab[v]) continue;
        if (ok && !ok(v)) continue;                    // wrong material for the tool in hand (user): the shovel takes soil and leaves the stone, the pick the reverse
        if (stampedIdx.has(ii)) continue;            // a creature standing on one of the shared palette ids is not mushroom geometry. stampedIdx, NOT cellStamped: that one only scans animals within 20 voxels of the PLAYER, so a bird 30 voxels away was unprotected and got carved into chunks (user 2026-08-05).
        cD.push(d2); cI.push(ii); cX.push(x); cY.push(y); cZ.push(z); cV.push(v);
      }
      if (spare < 0 && cD.length >= want) spare = 1;   // walk ONE more shell, then stop: shells go out nearest-first, so the true nearest `want` are all in hand by then
    }
    if (!cD.length) return false;
    const ord = new Int32Array(cD.length);
    for (let k = 0; k < ord.length; k++) ord[k] = k;
    ord.sort((a2, b2) => cD[a2] - cD[b2]);
    const take = Math.min(bite === undefined ? PH.chopBite : bite, ord.length);
    let x0 = 1e9, y0 = 1e9, z0 = 1e9, x1 = -1e9, y1 = -1e9, z1 = -1e9;
    for (let k = 0; k < take; k++) { const j = ord[k];
      if (cX[j] < x0) x0 = cX[j]; if (cX[j] > x1) x1 = cX[j];
      if (cY[j] < y0) y0 = cY[j]; if (cY[j] > y1) y1 = cY[j];
      if (cZ[j] < z0) z0 = cZ[j]; if (cZ[j] > z1) z1 = cZ[j]; }
    // ── THE BITE TAKES ITS SNOW WITH IT (user 2026-08-07: "the snow just floats on top of the missing chunk") ──
    // snow is a separate voxel resting one layer above the ground, and no tool filter claims it, so a bite out
    // of snow-covered ground left the cap hanging in the air over the hole. Every column this bite empties hands
    // its whole stack of snow to the CHUNK instead: it becomes part of the rigid body, so it rides along
    // stationary on top of the piece and is collected with it. Scanned per taken voxel and deduped by world
    // index, since several taken voxels can share a column. The chunk's local box has to grow first — the cell
    // key below is computed against y1, and a cap above it would fold onto another slot.
    const snowIdx = [], snowSeen = new Set();
    for (let k = 0; k < take; k++) { const j = ord[k];
      for (let y = cY[j] + 1; y < WY; y++) {
        const ii = gwrap(cX[j], WX) + y * WX + gwrap(cZ[j], WZ) * WX * WY;
        const v = W[ii]; if (!v || !snowTab[v]) break;   // not snow directly above → this column carries no cap
        if (snowSeen.has(ii)) break;
        snowSeen.add(ii); snowIdx.push(ii, cX[j], y, cZ[j], v);
        if (y > y1) y1 = y;
      } }
    for (let k = 0; k < take; k++) { const j = ord[k];   // ICE: log the hole so the thaw can fill it back in
      if (cV[j] !== WATER_T && cV[j] !== WATER_B) continue;   // BOTH shades: the surface skin is one voxel thick and the body under it is WATER_B, so logging only WATER_T restored a lid over a hole
      if (iceCutN >= iceCutI.length && iceCutI.length < (1 << 20)) {
        const g = new Int32Array(iceCutI.length * 2); g.set(iceCutI); iceCutI = g;
        const g2 = new Uint8Array(iceCutI.length); g2.set(iceCutV); iceCutV = g2;
      }
      if (iceCutN < iceCutI.length) { iceCutV[iceCutN] = cV[j]; iceCutI[iceCutN++] = cI[j]; }
    }
    const sx = x1 - x0 + 1, sz = z1 - z0 + 1;
    const cells = [], idMap = new Map(), out = [];
    for (let k = 0; k < take; k++) { const j = ord[k];
      const kk = (cX[j] - x0) + (cZ[j] - z0) * sx + (cY[j] - y0) * sx * sz;
      if (idMap.has(kk)) continue;                   // the window wrapped two cells onto one local slot — keep the first
      cells.push(kk); idMap.set(kk, cV[j]);
      W[cI[j]] = 0; out.push(cI[j]);
    }
    for (let k = 0; k < snowIdx.length; k += 5) {       // …and the caps, into the same body
      const ii = snowIdx[k], xS = snowIdx[k + 1], yS = snowIdx[k + 2], zS = snowIdx[k + 3], vS = snowIdx[k + 4];
      const kk = (xS - x0) + (zS - z0) * sx + (yS - y0) * sx * sz;
      if (idMap.has(kk)) continue;
      cells.push(kk); idMap.set(kk, vS);
      W[ii] = 0; out.push(ii);
      PH.stats.snowCarried = (PH.stats.snowCarried | 0) + 1;   // how many snow voxels rode away on chunks — the one honest read that the cap went WITH the bite
    }
    // (the upward 10-voxel column scan that used to sit here is gone: it was column-only,
    //  gravity-only and erase-only, so a strand held up SIDEWAYS by what the bite took was invisible
    //  to it and a strand it did see was destroyed rather than dropped.)
    if (!cells.length) return false;
    gpuPatch(out, false);
    wakeFrom(out, 6);
    { const cols = new Set();                        // DIGGING lowers the ground: re-derive the surface of every column this bite emptied
      for (const ii of out) cols.add(ii % WX + ((ii / (WX * WY)) | 0) * WX);
      for (const ci of cols) { let hy = hmap[ci]; while (hy > 1 && !W[(ci % WX) + (hy - 1) * WX + ((ci / WX) | 0) * WX * WY]) hy--; hmap[ci] = hy; } }
    spawnChopSparks(wx, wy, wz);                      // …and off mushrooms, ferns and logs
    PH.stats.chops++; PH.stats.voxRemoved += out.length;
    if (PH.bodies.length < PH.maxBodies || phMakeRoom()) {
      phSrc = 'decorBite';
      const b = phBuildBody({ bx: x0, gy: y0, bz: z0 }, cells, { sx, sz }, idMap);
      b.vel[0] = phFallDir[0] * 6 + (Math.random() - 0.5) * 4;
      b.vel[1] = 4 + Math.random() * 3;
      b.vel[2] = phFallDir[2] * 6 + (Math.random() - 0.5) * 4;
      b.omega[0] = (Math.random() - 0.5) * 3; b.omega[1] = (Math.random() - 0.5) * 3; b.omega[2] = (Math.random() - 0.5) * 3;
      b.absorbAt = performance.now() + PH.absorbMs;
      PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(b); PH.stats.chunks++;
    }
    // (what a bite left standing is adjudicated by supFlush, off the cells this carve actually
    //  changed — see the gpuPatch above. The phDecorOrphans sweep that used to run here is gone:
    //  it seeded ONLY the box floor, so any mass whose path to ground left through a side or the
    //  top was declared unsupported, and with NO size cap at all it cut gorge arches out of the
    //  world. And because woodTab is a subset of decorTab while the foliage ids are not, a shovel
    //  bite at a pine’s base lifted the trunk section and left the crown hanging in the sky.)
    return true;
  };
