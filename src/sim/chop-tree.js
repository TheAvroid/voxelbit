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
      const left = new Set(keepC), comps = [];
      while (left.size) {
        const start = left.values().next().value;
        const comp = [], st = [start]; left.delete(start);
        while (st.length) {
          const k2 = st.pop(); comp.push(k2);
          const mx = k2 % sx, mz = ((k2 / sx) | 0) % sz, my = (k2 / (sx * sz)) | 0;
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
      const keep = [];
      for (const comp of comps) {
        if (keep.length && (comp.length < 2 || !(PH.bodies.length + keep.length < PH.maxBodies || phMakeRoom()))) {
          keep[0] = keep[0].concat(comp);
          PH.stats.chopMerged = (PH.stats.chopMerged | 0) + comp.length;
          continue;
        }
        keep.push(comp);
      }
      // the BITE is the thing the player just carved and is owed to them, so it gets the last slot if there is
      // one; if there is not, its voxels ride with the main piece instead of ceasing to exist.
      let chipC = cutC;
      if (chipC.length && !(PH.bodies.length + keep.length < PH.maxBodies || phMakeRoom())) {
        if (keep.length) { keep[0] = keep[0].concat(chipC); PH.stats.chopMerged = (PH.stats.chopMerged | 0) + chipC.length; chipC = null; }
      }
      for (const comp of keep) {
        const nb = phSubBody(b, comp, idMap);
        nb.sleeping = false; nb.sleepT = 0;            // the shape changed under it — let it re-settle
        PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(nb);
      }
      // ── the bite ── one chunk, thrown clear along the swing, collected like every other chip
      if (chipC && chipC.length) {
        const chip = phSubBody(b, chipC, idMap);
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
  const physChopAt = (wx, wy, wz, rad, S0, minBite, bite, ok) => {
    const S = S0 || treeShapeAt(Math.round(wx), Math.round(wz));
    if (!S) return { hit: false, why: 'no pine here' };
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
      if (my <= S.tr.sink) continue;                 // never cut the buried root courses out from under the anchor
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
    let f = phFlood(S);
    if (f.orphans > PH.fellOrphans && phFlushBirds(S)) f = phFlood(S);   // a whole crown just came loose: the birds in it die with it, then re-test with their grid stamp gone (see phFlushBirds)
    const bodies = f.orphans > 0 ? phSeparate(S, f) : [];
    PH.stats.lastFlood = { total: f.total, reached: f.reached, orphans: f.orphans,   // WHAT THE TREE FLOOD DECIDED — the one read that separates "the game thinks it is still attached" (orphans 0) from "it came loose but no body was made" (orphans > 0, detached 0)
      detached: bodies.length, n: (PH.stats.lastFlood ? PH.stats.lastFlood.n + 1 : 1) };
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
