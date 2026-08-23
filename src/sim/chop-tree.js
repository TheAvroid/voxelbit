  // ── CHOP A RIGID BODY ── the felled trunk is not in W (that is the whole point of the off-grid
  // representation), so physChopAt cannot touch it and the axe used to swing straight through a tree
  // lying on the ground. Same carve, done in the body's own frame instead: transform the impact point
  // into body-local coords with the cached world axes phBodySolid already uses, take a sphere out of the
  // voxel list, then rebuild — the remainder is re-split into 6-connected pieces (bucking a log in half
  // really does give you two logs) and the bite flies off as a chunk on the usual absorb path.
  // ── ok(id): WHICH MATERIALS THIS SWING MAY TAKE (user 2026-08-07: "attempts to hit the wood, it hits the
  // leaves instead") ── without it the sphere sorts EVERY voxel it reaches nearest-first and takes the front
  // 30, and on a felled pine the front 30 are needles every time: the crown wraps the bole, so the trunk the
  // player is pointing at sits BEHIND a shell of foliage that belongs to the very same body. physChopAt (the
  // standing-tree path) was given exactly this filter for exactly this reason; the off-grid path never got it.
  // Filtered-out voxels stay in the body (keepC) — they are not deleted, just not this swing's chunk.
  const phChopBody = (wx, wy, wz, rad, minBite, bite, ok) => {
    if (!PH.on) return false;
    const t0 = performance.now();
    const r2 = rad * rad;
    for (let i = PH.bodies.length - 1; i >= 0; i--) {
      const b = PH.bodies[i];
      if (!b.cpuGrid || b.absorbing || b.rag || b.n < PH.chopMinBody) continue;   // flying debris is not a chopping target — and neither is a CORPSE: a ragdoll is mid-death and lives half a second, so letting the next swing of an auto-repeating chop mince it is exactly the "chunks break off the animal" the ragdoll was meant to replace (user 2026-08-05)
      const dx = wx + 0.5 - b.pos[0], dy = wy + 0.5 - b.pos[1], dz = wz + 0.5 - b.pos[2];
      const rr = b.rMax + rad + 2;
      if (dx * dx + dy * dy + dz * dz > rr * rr) continue;
      // impact point in the body's own frame (same basis phBodySolid uses, but un-floored)
      const cx = dx * b.ax[0] + dy * b.ax[1] + dz * b.ax[2] + b.com[0];
      const cy = dx * b.ay[0] + dy * b.ay[1] + dz * b.ay[2] + b.com[1];
      const cz = dx * b.az[0] + dy * b.az[1] + dz * b.az[2] + b.com[2];
      // ── CHEAP REJECT ── count the bite in the body's dense grid first. The full partition below is
      // O(voxels), and a swing samples ~82 points along the ray against every body in range — on a
      // 7500-voxel trunk that is ~600k iterations per click, a visible hitch. The grid answers "is there
      // even anything here" in ~340 lookups, and the O(n) pass then runs at most once per swing.
      const G = b.cpuGrid, bw = b.gpu.bw, bh = b.gpu.bh, bd = b.gpu.bd;
      const gx = dx * b.ax[0] + dy * b.ax[1] + dz * b.ax[2] + b.gpu.comL[0];
      const gy = dx * b.ay[0] + dy * b.ay[1] + dz * b.ay[2] + b.gpu.comL[1];
      const gz = dx * b.az[0] + dy * b.az[1] + dz * b.az[2] + b.gpu.comL[2];
      let avail = 0;
      for (let z2 = Math.max(0, Math.floor(gz - rad)), zE = Math.min(bd - 1, Math.ceil(gz + rad)); z2 <= zE && avail < minBite; z2++)
        for (let y2 = Math.max(0, Math.floor(gy - rad)), yE = Math.min(bh - 1, Math.ceil(gy + rad)); y2 <= yE && avail < minBite; y2++)
          for (let x2 = Math.max(0, Math.floor(gx - rad)), xE = Math.min(bw - 1, Math.ceil(gx + rad)); x2 <= xE; x2++) {
            const ex = x2 + 0.5 - gx, ey = y2 + 0.5 - gy, ez = z2 + 0.5 - gz;
            if (ex * ex + ey * ey + ez * ez > r2) continue;
            const gid = G[x2 + y2 * bw + z2 * bw * bh];
            if (gid && (!ok || ok(gid))) avail++;      // the reject has to ask the same question the partition will, or a crown full of needles reads as a full bite and then yields nothing
          }
      if (avail < minBite) continue;                   // grazed it — keep marching, something further along may hold a real bite
      const sx = b.sx, sz = b.sz, key = (mx, my, mz) => mx + mz * sx + my * sx * sz;
      const cutC = [], keepC = [], idMap = new Map(), inD = [], inK = [];
      for (let k = 0; k < b.n; k++) {
        const ex = b.lx[k] + 0.5 - cx, ey = b.ly[k] + 0.5 - cy, ez = b.lz[k] + 0.5 - cz;
        const kk = key(b.lx[k], b.ly[k], b.lz[k]);
        idMap.set(kk, b.id[k]);
        const d2 = ex * ex + ey * ey + ez * ez;
        if (d2 <= r2 && (!ok || ok(b.id[k]))) { inD.push(d2); inK.push(kk); } else keepC.push(kk);   // wrong material for the tool in hand → it stays part of the body
      }
      const ord2 = new Int32Array(inD.length);         // nearest-first, then take a FIXED count — same rule, same size chunk as a standing trunk
      for (let k = 0; k < ord2.length; k++) ord2[k] = k;
      ord2.sort((a2, b2) => inD[a2] - inD[b2]);
      const takeB = Math.min(bite === undefined ? PH.chopBite : bite, ord2.length);
      for (let k = 0; k < ord2.length; k++) { if (k < takeB) cutC.push(inK[ord2[k]]); else keepC.push(inK[ord2[k]]); }
      PH.bodies.splice(i, 1);                          // out of the list BEFORE rebuilding: phBuildBody's reclaim may splice it too
      PH.stats.bodyChops++; PH.stats.voxRemoved += cutC.length;
      { let w9 = 0, f9 = 0;                            // WHAT THIS BITE WAS MADE OF — the one unambiguous read of "the axe was on bark and came away with needles" (user 2026-08-07); a test cannot infer it from the body list, because the remainder and the chip are pushed in the same millisecond
        for (const k9 of cutC) { const q9 = idMap.get(k9); if (woodTab[q9]) w9++; else if (foliaTab[q9]) f9++; }
        PH.stats.lastChip = { vox: cutC.length, wood: w9, fol: f9, filtered: !!ok, n: (PH.stats.lastChip ? PH.stats.lastChip.n + 1 : 1) }; }
      // ── the remainder ── 6-connected pieces of what is left, largest first so a big trunk half never
      // loses its slot to a splinter (the same rule phSeparate uses).
      // ── …AND 26-CONNECTED FOR A BODY THAT CAME OFF AN OAK (2026-08-17) ── b.c26 is set by phBuildBody off
      // the shape the body was cut from. An oak crown is one piece only 26-connected (see oakShape in
      // sim/physics.js), so re-splitting a felled oak 6-connected hands back the main shell PLUS the 233
      // diagonal clusters as separate components — one swing at a log on the ground and the crown flies apart
      // into two dozen clumps with the rest folded into the largest piece. Same rule the flood and phComponent
      // use, applied to the off-grid twin of the same geometry. A pine's bodies carry c26 = 0 and take the
      // original loop untouched.
      const left = new Set(keepC), comps = [], c26 = !!b.c26;
      while (left.size) {
        const start = left.values().next().value;
        const comp = [], st = [start]; left.delete(start);
        while (st.length) {
          const k2 = st.pop(); comp.push(k2);
          const mx = k2 % sx, mz = ((k2 / sx) | 0) % sz, my = (k2 / (sx * sz)) | 0;
          if (c26) {
            for (let d = 0; d < 78; d += 3) {
              const nx = mx + phNb26[d], ny = my + phNb26[d + 1], nz = mz + phNb26[d + 2];
              if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
              const nk = key(nx, ny, nz);
              if (left.has(nk)) { left.delete(nk); st.push(nk); }
            }
            continue;
          }
          for (let d = 0; d < 6; d++) {
            const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
            const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
            const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
            if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
            const nk = key(nx, ny, nz);
            if (left.has(nk)) { left.delete(nk); st.push(nk); }
          }
        }
        comps.push(comp);
      }
      comps.sort((a2, b2) => b2.length - a2.length);
      if (comps.length > 1) PH.stats.bodySplits++;
      // ── A CHUNK MUST BEHAVE LIKE ANY OTHER MATERIAL (user 2026-08-08: "sometimes when hitting the tree
      // chunk, the chunk disappears" / "make the tree chunks behave like everything else") ── and this is the
      // one place in the game where it could not, because a chunk is the one thing you can hit that is NOT in
      // the world grid. Chopping terrain CLEARS voxels and the rest of the world simply stays; chopping a chunk
      // DESTROYS THE BODY AND REBUILDS IT — the splice above has already removed the original — so a failure to
      // rebuild is not a lost edit, it is lost matter. The old loop `break`ed the instant the body budget was
      // exhausted and silently discarded every piece it had not reached, which is the chunk vanishing under the
      // tool. (The absorbing guard added alongside this makes phMakeRoom refuse more often, so without this it
      // would have got worse, not better.)
      //
      // Every piece is now given a home BEFORE anything is built: whatever cannot have its own slot — a
      // homeless component, a one-voxel sliver, even the bite itself — is folded into the largest piece rather
      // than dropped. Folded pieces travel as one body, which is slightly wrong and enormously better than
      // evaporating, and it restores the invariant the world grid has always had: what you hit stays in the
      // world. The first piece always has a slot, because the original vacated one on its way in.
      // ── AND THE GUARD HAS TO COUNT THE PIECES STILL IN OUR HAND ── the splice above already took the chopped
      // body out, and nothing is pushed until the build loop below, so PH.bodies.length is ALWAYS under the cap
      // here: the old `PH.bodies.length + keep.length < PH.maxBodies || phMakeRoom()` fell through to a
      // phMakeRoom that made its own decision on PH.bodies.length alone, returned true without freeing anything,
      // and let keep grow without limit — so this net never fired once and PH.bodies simply overran maxBodies.
      // phMakeRoom now takes the pending count, which is the only number that makes the question answerable.
      const keep = [];
      for (const comp of comps) {
        if (keep.length && (comp.length < 2 || !phMakeRoom(keep.length))) {
          keep[0] = keep[0].concat(comp);
          PH.stats.chopMerged = (PH.stats.chopMerged | 0) + comp.length;
          continue;
        }
        keep.push(comp);
      }
      // the BITE is the thing the player just carved and is owed to them, so it gets the last slot if there is
      // one; if there is not, its voxels ride with the main piece instead of ceasing to exist.
      let chipC = cutC;
      if (chipC.length && !phMakeRoom(keep.length)) {   // …counting the keep pieces that are still waiting to be pushed (see above)
        if (keep.length) { keep[0] = keep[0].concat(chipC); PH.stats.chopMerged = (PH.stats.chopMerged | 0) + chipC.length; chipC = null; }
      }
      for (const comp of keep) {
        const nb = phSubBody(b, comp, idMap);
        nb.c26 = b.c26;                                // phSubBody builds through a {bx,gy,bz} pseudo-shape, which has no oak flag — carry the parent's, or the piece forgets how it is connected and the NEXT swing shatters it
        nb.sleeping = false; nb.sleepT = 0;            // the shape changed under it — let it re-settle
        PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(nb);
      }
      // ── the bite ── one chunk, thrown clear along the swing, collected like every other chip
      if (chipC && chipC.length) {
        const chip = phSubBody(b, chipC, idMap);
        chip.c26 = b.c26;
        chip.vel[0] += phFallDir[0] * 6 + (Math.random() - 0.5) * 4;
        chip.vel[1] += 4 + Math.random() * 3;
        chip.vel[2] += phFallDir[2] * 6 + (Math.random() - 0.5) * 4;
        chip.omega[0] = (Math.random() - 0.5) * 3; chip.omega[1] = (Math.random() - 0.5) * 3; chip.omega[2] = (Math.random() - 0.5) * 3;
        chip.absorbAt = performance.now() + PH.absorbMs;
        PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(chip);
        PH.stats.chunks++;
      }
      spawnChopSparks(wx, wy, wz);                   // …and off a felled trunk too
      PH.stats.lastBodyChopMs = +(performance.now() - t0).toFixed(2);
      return true;
    }
    return false;
  };
  // ── CHOP ── carve real trunk voxels in a sphere, then re-test connectivity. Foliage is NOT chopped:
  // the canopy is walk-through decor, and cutting it would let the crown fall off its own branches
  // instead of the tree falling at the trunk.
  // ok(v): which of the tree's materials THIS swing may take. Without it the sphere takes whatever tree
  // voxel is nearest, and NEEDLES outnumber bark everywhere in the crown — see the gather loop below.
  // == DOES THIS CUT DROP THE TREE? == everything that happens AFTER a swing has removed wood: the
  // connectivity flood, the birds in a crown that just came loose, the separation into rigid bodies, and
  // the hive that was hanging in it. Lifted out of physChopAt (2026-08-20) so the axe's DECOR-style carve
  // can run the identical settle: the axe now chunks wood through phChopDecor, exactly as the pick chunks
  // stone, and a carve that cannot fell a tree would have quietly turned felling off.
  // ONE copy, called from both, so the two paths can never drift on what makes a tree fall.
  const phTreeSettle = (S) => {
    let f = phFlood(S);
    if (f.orphans > PH.fellOrphans && phFlushBirds(S)) f = phFlood(S);   // a whole crown just came loose: the birds in it die with it, then re-test with their grid stamp gone (see phFlushBirds)
    const bodies = f.orphans > 0 ? phSeparate(S, f) : [];
    // ── FELLING THE OAK BREAKS THE HIVE HANGING IN IT (user 2026-08-19: "make the bees attack the player if
    // the player cuts down the tree that the beehive was located on") ── and it posts the SAME record a smashed
    // hive does, into the same ledger, rather than being a second kind of event. The hive is destroyed either
    // way — it is not part of the oak's shape (stampModel writes it beside the tree, so phPresent answers 0 for
    // every one of its voxels and phFlood never sees it), so the moment its branch leaves W it is hanging on
    // nothing and the support sweep drops it — and everything the ledger already carries is exactly what this
    // event needs: recruiting at BEE_BREAK_R, the 18 s clock, the leash, HIVE_DONE so it can only happen once.
    // hiveBroke is idempotent, so this and the 1 Hz watch in main/tick-creatures.js racing to the same hive
    // still post one record between them.
    // ── WHAT IS TESTED IS THE HANGER, NOT THE SEPARATION ── `bodies.length` is the wrong question: phSeparate
    // makes a body out of ANY orphaned component, so a clipped leaf clump would post a break and the swarm
    // would ambush a player who never touched the hive. The right question is the one the hive's own placement
    // answers: it hangs from ONE bark voxel, face-adjacent to its top-centre course (world/terrain.js resolves
    // the anchor at hy + HIVEV.sz, directly over the stamp column). If that voxel is still bark the hive is
    // still hanging; if this cut took it — into the felled trunk, into a severed limb, or out as chips — the
    // hive is coming down and the swarm is owed its answer. One W read on the 3% of oaks that carry a hive.
    // ── AND THE RECORD GOES AT THE HIVE, NOT AT THE NOTCH ── BEE_RAGE_LEASH measures wreck-to-PLAYER and is
    // the escape, and a feller is standing at the BASE of the tree: anchoring there would put the wreck under
    // the player's own feet and hand the swarm a leash centred on wherever the axe was. hiveBroke posts at
    // h.wx/wy/wz, so a felled hive and a smashed one give the player the identical 220 to run.
    // The course is read over the hive's own 3x3 footprint rather than at the single anchor voxel: a limb is
    // several voxels wide, and testing one cell makes the whole hook hostage to that cell being bark at boot
    // (stampOak stamps in MODE 1 and refuses a cell terrain already holds, so a blocked anchor would read
    // "not hanging" on an untouched world and ambush the first swing at the tree). Nine reads, and it goes
    // false only when the limb over the hive is gone entirely — which is what felling the tree does.
    if (S.oak && S.tr && S.tr.hv) { const hvF = S.tr.hv, hay = hvF.by + hvF.sz;   // the branch course: the hive's top voxel sits one below it (world/terrain.js resolves the anchor at hy + HIVEV.sz)
      if (hay >= 1 && hay < WY) { let hang = 0;
        for (let dz = -1; dz <= 1 && !hang; dz++) for (let dx = -1; dx <= 1; dx++) {
          if (woodTab[W[gwrap(hvF.bx + dx, WX) + hay * WX + gwrap(hvF.bz + dz, WZ) * WX * WY]]) { hang = 1; break; } }
        if (!hang) hiveBroke(hvF); }
    }
    PH.stats.lastFlood = { total: f.total, reached: f.reached, orphans: f.orphans,
      detached: bodies.length, n: (PH.stats.lastFlood ? PH.stats.lastFlood.n + 1 : 1) };
    return { f, bodies };                             // the caller wants both: the flood's counts and the bodies the separation actually made
  };
  const physChopAt = (wx, wy, wz, rad, S0, minBite, bite, ok) => {
    const S = S0 || treeShapeAt(Math.round(wx), Math.round(wz));
    if (!S) return { hit: false, why: 'no tree here' };   // …'pine' until 2026-08-17: treeShapeAt answers for oaks now too
    const r = rad === undefined ? 3 : rad, r2 = r * r;
    phFlushBirdsNear(wx, wy, wz, r + 8);              // a bird standing on what is about to be cut leaves FIRST (see phFlushBirdsNear)
    const cellsOut = []; let removed = 0;
    const chipCells = [];                             // (mx,my,mz,id) of everything the axe carves out
    // COUNT FIRST. Once a notch exists the sphere is mostly air, so a naive carve often found a single
    // voxel and the swing "succeeded" having removed almost nothing — chunk sizes came out wildly
    // inconsistent. With a minimum bite the swing keeps looking further along the ray for somewhere with
    // real material instead of settling for a speck.
    // gather every voxel the sphere could take, with its distance from the impact
    const cD = [], cX = [], cY = [], cZ = [], cV = [];
    const ri = Math.ceil(r);                         // WHOLE-voxel steps; r2 below still tests the true radius
    for (let dy = -ri; dy <= ri; dy++) for (let dz = -ri; dz <= ri; dz++) for (let dx = -ri; dx <= ri; dx++) {
      const d2 = dx * dx + dy * dy + dz * dz; if (d2 > r2) continue;
      const mx = Math.round(wx) + dx - S.bx, my = Math.round(wy) + dy - S.gy, mz = Math.round(wz) + dz - S.bz;
      if (mx < 0 || mx >= S.R.sx || mz < 0 || mz >= S.R.sz || my < 0 || my >= MSZ) continue;
      const v = phPresent(S, mx, my, mz); if (!v) continue;
      if (PICK_CONE.has(v)) continue;                // cones are pickable ITEMS, not material. Foliage IS carvable (user: break the canopy into chunks while the tree still stands) — the flood below then decides honestly whether what is left is still attached.
      if (ok && !ok(v)) continue;                    // …but a swing aimed at the TRUNK passes a wood-only filter, or it comes back full of needles (see chopSwing)
      if (my <= S.root) continue;                    // never cut the root courses out from under the anchor — the pine's buried sink, and an oak's first courses clear of the ground (see OAK_ROOT)
      cX.push(mx); cY.push(my); cZ.push(mz); cV.push(v);
    }
    if (cX.length < minBite) return { hit: false, why: 'thin bite (' + cX.length + ')' };
    // ── THE CHUNK COMES OUT WHERE THE AXE LANDED (user 2026-08-07: "it seems to cut 'around' the center
    // point") ── this used to rank the material by distance from the CENTRE OF MASS of everything carvable in
    // the sphere, so on a trunk the notch appeared centred on the bole's own axis whatever part of it you were
    // pointing at. That was a workaround for an impact point that was not trustworthy: the carve fired from
    // wherever treeShapeAt first answered, which is open air short of the tree, and a nearest-first set about
    // an air point is a flat cap on the near face — hence "the chunks are often flat pieces". The impact point
    // is now the voxel the crosshair rests on (see aimT in chopSwing), pushed C_DEEP into the wood, so ranking
    // from it gives the same round ball centred where the blow actually fell. phChopBody has always done it
    // this way; the two paths now agree.
    const bcx = Math.round(wx) - S.bx, bcy = Math.round(wy) - S.gy, bcz = Math.round(wz) - S.bz;
    for (let k = 0; k < cX.length; k++) {
      const ex = cX[k] - bcx, ey = cY[k] - bcy, ez = cZ[k] - bcz;
      cD.push(ex * ex + ey * ey + ez * ez);          // plain Euclidean from the IMPACT POINT → the roundest set the geometry allows, about the blow
    }
    const ord = new Int32Array(cD.length);
    for (let k = 0; k < ord.length; k++) ord[k] = k;
    ord.sort((a2, b2) => cD[a2] - cD[b2]);           // nearest-first about the material's centre
    const take = Math.min(bite === undefined ? PH.chopBite : bite, ord.length);  // ALWAYS this many — that is what makes the chunk the same size every swing
    for (let k = 0; k < take; k++) {
      const j = ord[k], mx = cX[j], my = cY[j], mz = cZ[j];
      const ii = phWorldIdx(S, mx, my, mz);
      W[ii] = 0; cellsOut.push(ii); removed++;
      chipCells.push(mx, my, mz, cV[j]);              // keep the carved voxel — it becomes a falling chunk, not litter
    }
    if (!S0) phSetFallDir(Math.round(wx) - P.x, Math.round(wz) - P.z);   // console chop: away from the player
    if (!removed) return { hit: false, why: 'no trunk voxel in range' };
    { let qx = 0, qy = 0, qz = 0;                     // WHERE THE NOTCH LANDED, in world voxels — the one number that says whether the axe cut where it was aimed
      for (let k = 0; k < chipCells.length; k += 4) { qx += chipCells[k]; qy += chipCells[k + 1]; qz += chipCells[k + 2]; }
      const nq = chipCells.length / 4;
      PH.stats.lastCut = { x: S.bx + qx / nq, y: S.gy + qy / nq, z: S.bz + qz / nq,
        ix: Math.round(wx), iy: Math.round(wy), iz: Math.round(wz), n: nq,
        seq: (PH.stats.lastCut ? PH.stats.lastCut.seq + 1 : 1) }; }
    gpuPatch(cellsOut, false);
    wakeFrom(cellsOut, 6);            // a chop can orphan a cone or its snow cap
    spawnChopSparks(wx, wy, wz);                      // the bite landed — 4 embers at the impact (user)
    PH.stats.chops++; PH.stats.voxRemoved += removed;
    phSpawnChunk(S, chipCells);                       // the bite the axe took + the shaken leaves fly off as real bodies
    const { f, bodies } = phTreeSettle(S);
    return { hit: true, removed, total: f.total, reached: f.reached, orphans: f.orphans,
             detached: bodies.length, bodyVox: bodies.map((b) => b.n),
             floodMs: PH.stats.lastFloodMs, sepMs: PH.stats.lastSepMs };
  };
  // ── phSeveredFall IS GONE ── it seeded EVERY FACE of a 37x37x39 box as support, which made a false
  // positive structurally impossible and the sweep itself pointless: any component larger than the box
  // in any axis — a severed crown, a felled trunk, a mined-out ledge — always touches a face. MEASURED,
  // it fired 0 times. Its solidHere also admitted WATER, so the day the box rule ever did fire it would
  // have lifted a lake pocket as a rigid body; and its size guard leaked exactly as phFallLoose’s did.
  // The unified resolver answers the same question honestly — does this component reach the static
  // ground — with no box at all, so there is no face to hide behind.
  // ── A BODY FROM EXPLICIT VOXELS ── same build as phBodyFromCells, but the caller supplies the palette id
  // per cell and W is never read or written. That is what a creature needs: a duck is TRACE-INJECTED, so it
  // has no world voxels to lift out — its ragdoll is built from the model it is drawn from.
  // cells: [x, y, z, palId] in WORLD coordinates.
  const phBodyFromVoxels = (cells) => { phSrc = 'ragdoll';
    let x0 = 1e9, y0 = 1e9, z0 = 1e9, x1 = -1e9, y1 = -1e9, z1 = -1e9;
    for (const c of cells) {
      if (c[0] < x0) x0 = c[0]; if (c[0] > x1) x1 = c[0];
      if (c[1] < y0) y0 = c[1]; if (c[1] > y1) y1 = c[1];
      if (c[2] < z0) z0 = c[2]; if (c[2] > z1) z1 = c[2];
    }
    if (x1 < x0) return null;
    const sx = x1 - x0 + 1, sz = z1 - z0 + 1;
    const keys = [], idMap = new Map();
    for (const c of cells) {
      if (!c[3]) continue;
      const kk = (c[0] - x0) + (c[2] - z0) * sx + (c[1] - y0) * sx * sz;
      if (idMap.has(kk)) continue;
      keys.push(kk); idMap.set(kk, c[3]);
    }
    if (!keys.length) return null;
    return phBuildBody({ bx: x0, gy: y0, bz: z0 }, keys, { sx, sz }, idMap);
  };
  // Lift a list of world cells out of W and hand them back as one rigid body on the parent's spot.
  const phBodyFromCells = (cells) => {
    let x0 = 1e9, y0 = 1e9, z0 = 1e9, x1 = -1e9, y1 = -1e9, z1 = -1e9;
    for (const c of cells) {
      if (c[0] < x0) x0 = c[0]; if (c[0] > x1) x1 = c[0];
      if (c[1] < y0) y0 = c[1]; if (c[1] > y1) y1 = c[1];
      if (c[2] < z0) z0 = c[2]; if (c[2] > z1) z1 = c[2];
    }
    const sx = x1 - x0 + 1, sz = z1 - z0 + 1;
    const keys = [], idMap = new Map(), out = [];
    for (const c of cells) {
      const ii = gwrap(c[0], WX) + c[1] * WX + gwrap(c[2], WZ) * WX * WY;
      const v = W[ii]; if (!v) continue;
      const kk = (c[0] - x0) + (c[2] - z0) * sx + (c[1] - y0) * sx * sz;
      if (idMap.has(kk)) continue;                     // the toroidal window folded two cells onto one slot
      keys.push(kk); idMap.set(kk, v);
      W[ii] = 0; out.push(ii);
    }
    if (!keys.length) return null;
    gpuPatch(out, false);
    wakeFrom(out, 6);
    return phBuildBody({ bx: x0, gy: y0, bz: z0 }, keys, { sx, sz }, idMap);
  };

  // ── A FELLED TREE BREAKS WHEN IT LANDS, NOT WHEN IT IS CUT (user 2026-08-22: "can you make the trees break
  // in pieces when it falls over? not at the moment the player chomps the tree down, but the moment the tree
  // hits the terrain. Then the player can just absorb the tree all in one go.") ── the fell already produces
  // ONE body for the trunk-and-crown (phSeparate sorts components largest-first precisely so the trunk is not
  // starved of a slot), and it stays one body on the ground, so the tree had to be chopped up again where it
  // lay. This breaks it on impact instead and makes the pieces collectable, which is the second half of the
  // ask: noAbsorb is what marks a toppling trunk as scenery rather than loot, and once it is DOWN it is loot.
  //
  // Split along the body's LONGEST local axis, which for a trunk is its length — so the pieces are logs rather
  // than an arbitrary dice. The parent is spliced out first for the same reason the chop path does it: the
  // rebuild may reclaim slots and would otherwise trip over the body it is replacing.
  // ── AND THE STUMP BREAKS TOO (user 2026-08-22: "have the tree trunk thats still connected to the terrain
  // break into chunks as well") ── the part below the cut is rooted in the ground, so phSeparate never orphans
  // it and it survives the fell as world voxels. This lifts it out of W at the same moment the trunk breaks and
  // hands it back as the same connected, chunk9-sized bodies, so the whole tree ends up as collectable chunks
  // and no stump is left standing. Wood only: the grass and soil around the base are not the tree.
  const phShatterStump = (b) => {
    if (!b.origin || !b.sx || !b.sz) return 0;
    const ox = b.origin[0] | 0, oy = b.origin[1] | 0, oz = b.origin[2] | 0, sx = b.sx | 0, sz = b.sz | 0;
    const cells = [], idMap = new Map(), out = [];
    const sxz = sx * sz;
    for (let my = 0; my < 40; my++) {
      const wy = oy + my; if (wy < 1 || wy >= WY) continue;
      for (let mz = 0; mz < sz; mz++) for (let mx = 0; mx < sx; mx++) {
        const ii = gwrap(ox + mx, WX) + wy * WX + gwrap(oz + mz, WZ) * WX * WY;
        const v = W[ii]; if (!v || !woodTab[v]) continue;
        const kk = mx + mz * sx + my * sxz;
        cells.push(kk); idMap.set(kk, v); out.push(ii);
      }
    }
    if (cells.length < 8) return 0;                    // nothing left worth breaking — a cut flush with the ground
    for (const ii of out) W[ii] = 0;
    gpuPatch(out, false);
    wakeFrom(out, 6);                                  // whatever was resting on the stump is asked again
    const present = new Set(cells), used = new Set();
    const free9 = Math.max(1, PH.maxBodies - PH.bodies.length);
    const chunk9 = Math.max(PH.fellChunkVox, Math.ceil(cells.length / free9));
    let made = 0;
    for (let q = 0; q < cells.length; q++) {
      const seed = cells[q]; if (used.has(seed)) continue;
      const grp = [seed]; used.add(seed);
      for (let h = 0; h < grp.length && grp.length < chunk9; h++) {
        const kk = grp[h];
        const mx = kk % sx, mz = ((kk / sx) | 0) % sz, my = (kk / sxz) | 0;
        for (let d = 0; d < 27 && grp.length < chunk9; d++) {
          const ax = mx + (d % 3) - 1, ay = my + ((((d / 3) | 0) % 3)) - 1, az = mz + (((d / 9) | 0)) - 1;
          if (ax < 0 || ax >= sx || az < 0 || az >= sz || ay < 0) continue;
          const nk = ax + az * sx + ay * sxz;
          if (!present.has(nk) || used.has(nk)) continue;
          used.add(nk); grp.push(nk);
        }
      }
      if (PH.bodies.length >= PH.maxBodies) break;     // out of slots: what is left simply stays out of W rather than being destroyed… see below
      const nb = phBuildBody({ bx: ox, gy: oy, bz: oz }, grp, { sx, sz }, idMap);
      if (!nb) break;
      nb.fellLoot = 1; nb.noAbsorb = false;
      nb.absorbAt = undefined;
      nb.noRest = performance.now() + PH.fellSettleMs;
      PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(nb); made++;
    }
    PH.stats.stumpChunks = (PH.stats.stumpChunks | 0) + made;
    return made;
  };
  const phShatterTree = (b) => { phSrc = 'treeLand';
    const i = PH.bodies.indexOf(b); if (i < 0) return 0;
    const sx = b.sx, sz = b.sz, key = (mx, my, mz) => mx + mz * sx + my * sx * sz;
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9;
    for (let k = 0; k < b.n; k++) {
      if (b.lx[k] < x0) x0 = b.lx[k]; if (b.lx[k] > x1) x1 = b.lx[k];
      if (b.ly[k] < y0) y0 = b.ly[k]; if (b.ly[k] > y1) y1 = b.ly[k];
      if (b.lz[k] < z0) z0 = b.lz[k]; if (b.lz[k] > z1) z1 = b.lz[k];
    }
    const LO = [x0, y0, z0], EX = [x1 - x0, y1 - y0, z1 - z0];
    // ── TWO CUTS, NOT ONE (user 2026-08-22: "the big trees seem to break away in discs, cut the discs in half,
    // so they break apart in chunks instead") ── slicing on the longest axis alone gives slabs that span the
    // WHOLE crown on the other two, which on a big oak is a dinner plate of leaves: correct as physics, wrong
    // as a broken tree. The second cut is along the next-longest axis, so a piece is bounded on two sides and
    // reads as a chunk. Two divisions there rather than a free count — this is "cut the disc in half", and the
    // piece budget is spent on length first, which is where a trunk actually wants to break.
    // ── EVERY PIECE THE SAME SIZE (user 2026-08-22: "all of the felled tree chunks have the exact same chunk
    // size") ── a spatial GRID cannot do that: a crown is dense in the middle and thin at the rim, so equal
    // boxes hold wildly unequal numbers of voxels (measured: median 261, largest 1105 — a 4x spread from cells
    // that were the same shape). Counting is the only way to make the SIZE the constant, so the cells are
    // ordered and then cut every PH.fellChunkVox of them, which makes each piece exactly that many voxels.
    // MORTON order, not raw x/y/z: interleaving the bits keeps the run of cells that lands in one piece
    // spatially COMPACT, so a chunk is a lump of tree rather than a thin sheet spanning the whole crown. That
    // is the whole reason to pay for a sort here — sorting on one axis would give equal counts and terrible
    // shapes. 8 bits per axis covers the largest model this game has.
    const mort9 = (x, y, z) => { let m = 0;
      for (let q = 0; q < 8; q++) m |= (((x >> q) & 1) << (3 * q)) | (((y >> q) & 1) << (3 * q + 1)) | (((z >> q) & 1) << (3 * q + 2));
      return m; };
    const cellsM = [], idMap = new Map();
    for (let k = 0; k < b.n; k++) {
      const kk = key(b.lx[k], b.ly[k], b.lz[k]);
      idMap.set(kk, b.id[k]);
      cellsM.push([mort9(b.lx[k] - LO[0], b.ly[k] - LO[1], b.lz[k] - LO[2]), kk]);
    }
    cellsM.sort((p9, q9) => p9[0] - q9[0]);
    // The count is still bounded by the slots that are actually free — a piece with nowhere to live cannot be
    // made — so a very large tree in a busy scene gets fewer, bigger chunks rather than losing any of itself.
    // ── THE CHUNK SIZE NEVER GROWS; THE BREAK WAITS INSTEAD (user 2026-08-22: "some chunks are still too big.
    // they arent even being absorbed") ── this used to raise chunk9 when slots were short, which is how a
    // second tree felled into the first one's debris came out in 10,027-voxel lumps, 30 of them over the
    // absorb limit. Size is the thing the player feels, so it is fixed: if there is not room to break this
    // tree into uniform pieces right now, the tree stays whole and tries again on the next tick — a few
    // seconds late once the earlier debris has been collected or expired, and always the right size.
    const free9 = Math.max(1, PH.maxBodies - PH.bodies.length - 1);
    const chunk9 = PH.fellChunkVox;
    if (free9 < Math.ceil(b.n / chunk9)) return 0;      // NOT spliced out, fellWhole NOT cleared — see the caller
    // ── A PIECE MUST BE ONE CONNECTED LUMP (user 2026-08-22: "make it where chunks voxels have to be touching
    // eachother and cannot be seperated by air") ── slicing the Morton order every chunk9 cells gets the SIZE
    // right and says nothing about connectivity: Morton keeps neighbours near each other in the ordering but
    // does not guarantee a run is contiguous, so a piece could hold two separate blobs bound together only by
    // the body transform — and the far one then hangs in the air with nothing around it, which is what a
    // "floating voxel" off a chunk actually is. So a piece is GROWN instead: seed at the lowest unused cell in
    // Morton order (compact and deterministic) and flood 26-connected through unused cells until it has
    // chunk9 of them. Connectivity is a hard constraint and the size is the target: where a connected region
    // runs out early the piece is simply smaller, because the alternative is a piece with a hole of air in it.
    const present = new Set();
    for (let q = 0; q < cellsM.length; q++) present.add(cellsM[q][1]);
    const sxz = sx * sz;
    // ── AND THE PIECE COUNT MUST FIT THE BODY CAP ── growth is greedy, so it makes MORE pieces than
    // ceil(n / chunk9): a run stops when it has enough, and the pocket it walked past becomes a piece of its
    // own. MEASURED on an 86k oak at chunk9 350: 493 pieces against a PHYS_MAX of 256 — and a body past the
    // cap cannot even be uploaded (main/tick-emit.js skips nb >= PHYS_MAX), so it would be invisible AND
    // physical. Growing chunk9 and re-running is the fix that keeps every piece CONNECTED: bigger pieces, same
    // rule, never a piece stitched together across air just to hit a number.
    // ── BALANCED MULTI-SOURCE GROWTH ── every piece must be ONE CONNECTED LUMP (voxels touching, never split
    // by air) and they must all be about the SAME SIZE. Growing pieces one at a time cannot do both: a greedy
    // blob takes its quota and walks past cells that then have nobody to join, and folding those pockets back
    // in afterwards either lets one piece run away (measured: median 353, largest 1749) or, with a ceiling on
    // the host, leaves the pockets stranded as scraps (median 8, p90 1072 — bimodal, which is worse).
    // So all K pieces grow AT ONCE from K seeds spread through the Morton order, claiming one cell each per
    // round. Nothing is stranded because every frontier advances together, every piece is connected because it
    // only ever claims a neighbour of a cell it already owns, and the sizes come out even because they grow at
    // the same rate. Anything left over is a region no seed could reach — genuinely disconnected geometry —
    // and becomes its own piece rather than being stitched across air to something else.
    const K = Math.max(2, Math.ceil(b.n / chunk9));   // room for this many was checked above
    const ownOf = new Map();
    const queues = [], buckets = [];
    for (let q = 0; q < K; q++) {
      const seed = cellsM[Math.min(cellsM.length - 1, Math.floor(q * cellsM.length / K))][1];
      if (ownOf.has(seed)) { queues.push([]); buckets.push([]); continue; }
      ownOf.set(seed, q); queues.push([seed]); buckets.push([seed]);
    }
    const nbrs9 = (kk, out) => { let m = 0;
      const mx = kk % sx, mz = ((kk / sx) | 0) % sz, my = (kk / sxz) | 0;
      for (let d = 0; d < 27; d++) {
        const ax = mx + (d % 3) - 1, ay = my + ((((d / 3) | 0) % 3)) - 1, az = mz + (((d / 9) | 0)) - 1;
        if (ax < 0 || ax >= sx || az < 0 || az >= sz || ay < 0) continue;   // model-local bounds: no wrap, or a piece would reach round to the far face
        out[m++] = ax + az * sx + ay * sxz;
      }
      return m; };
    const nb9 = new Int32Array(27);
    // One SHELL per piece per round: pop a single frontier cell and claim every unclaimed neighbour of it.
    // All K frontiers advance at the same rate, so the pieces come out the same size, and a cell with no
    // unclaimed neighbours is simply dropped from the frontier — the earlier version pushed it BACK, which
    // made queues that never emptied, stalled the growth, and left 61,024 cells for the island pass to sweep
    // up as one piece. Head pointers rather than shift(): these arrays run to tens of thousands of entries.
    const heads = new Int32Array(K);
    let live = true;
    while (live) {
      live = false;
      for (let q = 0; q < K; q++) {
        const qq = queues[q];
        if (heads[q] >= qq.length) continue;
        const kk = qq[heads[q]++];
        const m = nbrs9(kk, nb9);
        for (let t = 0; t < m; t++) { const nk = nb9[t];
          if (!present.has(nk) || ownOf.has(nk)) continue;
          ownOf.set(nk, q); buckets[q].push(nk); qq.push(nk);
        }
        if (heads[q] < qq.length) live = true;
      }
    }
    // Whatever no seed reached is a disconnected island: give each its own piece rather than attaching it to
    // something it does not touch. This is the case that MUST stay separate — it is exactly the floating-voxel
    // bug when it is not.
    for (let q = 0; q < cellsM.length; q++) { const kk = cellsM[q][1];
      if (ownOf.has(kk)) continue;
      const grp = [kk]; ownOf.set(kk, -1);
      for (let h = 0; h < grp.length; h++) { const m = nbrs9(grp[h], nb9);
        for (let t = 0; t < m; t++) { const nk = nb9[t];
          if (!present.has(nk) || ownOf.has(nk)) continue;
          ownOf.set(nk, -1); grp.push(nk); } }
      buckets.push(grp);
    }
    PH.bodies.splice(i, 1);                            // out of the list BEFORE rebuilding, exactly as the body-chop path above does
    // ── AND NOTHING IS THROWN AWAY TO FIT ── this used to filter buckets to >= 2 cells and `break` out of the
    // build when phMakeRoom refused, which silently DELETED every remaining piece: a tree that broke late in a
    // busy scene could lose most of itself. Same merge rule the body-chop path above uses — a piece that cannot
    // have a slot rides with the first one instead of ceasing to exist.
    const keep = [];
    for (const comp of buckets) {
      if (!comp.length) continue;
      // ── A FREE SLOT, NOT A MADE ONE (user 2026-08-22: "when the tree lands on the terrain from being felled,
      // some of the chunks dissapear. dont let chunks dissapear, unless absorbed by the player") ── this asked
      // phMakeRoom, and phMakeRoom MAKES room by deleting the oldest body outright. Called once per piece with
      // up to 48 pieces, it was evicting the earlier pieces of the very tree it was breaking, which is exactly
      // the chunks vanishing on impact. A plain capacity test cannot destroy anything: what will not fit merges
      // into the first piece instead, so the tree keeps every voxel it had however tight the budget is.
      // The bucket COUNT already fits the free slots, so nothing here has to be merged away to make space —
      // only true slivers (a bucket the partition left with a voxel or two) ride with the first piece.
      if (keep.length && comp.length < 2) { keep[0] = keep[0].concat(comp); continue; }
      keep.push(comp);
    }
    if (keep.length < 2) {                             // nothing to gain — put it back untouched rather than rebuild an identical body
      PH.bodies.push(b); b.fellWhole = 0; b.noAbsorb = false; b.fellLoot = 1; return 0;   // same rule for the un-split trunk: loot, but walked up to
    }
    let made = 0;
    for (const comp of keep) {
      if (PH.bodies.length >= PH.maxBodies) break;     // hard guard: a body past the cap is one the uniform cannot carry
      const nb = phSubBody(b, comp, idMap);
      nb.c26 = b.c26;
      nb.sleeping = false; nb.sleepT = 0;
      nb.fellWhole = 0;                                // a piece never re-shatters
      nb.noAbsorb = false;                             // …and IS loot now: this is the "absorb the tree all in one go" half
      // ── AND SIZE MUST NOT REFUSE IT (user 2026-08-22: "I want the player to be able to absorb the entire
      // tree that fell") ── PH.absorbSize is 200 voxels and a crown chunk is thousands, so every piece was
      // being marked tooBig and left as scenery however finely it was cut: getting a 28,000-voxel crown under
      // 200 would take seven more halvings, which is not a broken tree, it is sawdust. The limit exists to
      // stop someone pocketing a hillside, and a tree they have just felled is the one mass they are OWED —
      // so the pieces of a fell carry an exemption instead of the number being lowered for everything.
      nb.fellLoot = 1;
      nb.noRest = performance.now() + PH.fellSettleMs;   // …and it may not rest on its siblings for a moment (see PH.fellSettleMs)
      // ── COLLECTED BY WALKING UP TO IT, NOT FROM ACROSS THE MAP (user 2026-08-22: "the player can seem to
      // just obsorb the tree chunks at any distnace. make it the same absorb distance as the raw steak") ──
      // this used to set absorbAt, and in sim/solver.js that is not a delay, it is a BYPASS: once the timer
      // elapses the whole distance test is skipped, because absorbAt means "the player carved this, it is
      // owed to them" and a carved chip flies to the hand. A felled tree is not a carve — leaving absorbAt
      // unset drops these into the ordinary loose-debris path, which requires the piece to have settled and
      // to be inside PH.absorbR: exactly the reach a dropped steak uses.
      nb.omega[0] += (Math.random() - 0.5) * 0.4; nb.omega[2] += (Math.random() - 0.5) * 0.4;   // a nudge so the break reads as a break rather than a seam appearing — NO upward kick, which threw pieces on top of each other and left them propped in the air
      PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(nb); made++;
    }
    if (!made) { PH.bodies.push(b); b.fellWhole = 0; return 0; }   // never destroy the tree because the budget was full
    phShatterStump(b);                                 // …and the part still rooted in the ground goes with it
    PH.stats.fellBreaks = (PH.stats.fellBreaks | 0) + 1;
    PH.stats.fellPieces = (PH.stats.fellPieces | 0) + made;
    return made;
  };
