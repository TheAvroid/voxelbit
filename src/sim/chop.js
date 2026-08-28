  // ── SHED SCRAP, NOT SCENERY (user 2026-08-07: "when I break into a new tree, the old tree that fell
  // completely disappears") ── this took the OLDEST body, full stop, and after one felling the oldest body in
  // the world is that tree's trunk. Starting a second tree then called it repeatedly and stripped the first one
  // out of existence. A body over absorbSize is too big to pick up, which is exactly the line between "a chip
  // in flight" and "a log lying in the forest the player made": scrap goes first, and a trunk is only ever
  // touched when there is nothing else left at all.
  // ── A CHUNK ALREADY FLYING INTO YOUR HAND IS OWED TO YOU (user 2026-08-08: "dig a chunk, but if another
  // chunk is dug before the first is fully absorbed, it disappears") ── this evicts the OLDEST body that is
  // small enough to count as scrap, and a chunk mid-flight is exactly that: small, and the oldest, because it
  // was dug first. So the second dig deleted the first one out of the air. The absorb loop already refuses to
  // expire a flying chunk for the same reason ("one already flying into the player finishes its flight"); the
  // eviction paths simply never learned it. b.absorbing means the simulation has already let go of it and it
  // is on a curve into the chest — there is no state left to reclaim, only a promise to keep.
  // `pending` = slots the caller has already promised to pieces it has built up but not yet pushed. Without it
  // this answered "there is room" for a list that is only under the cap because those pieces are still in the
  // caller's hand — which is what made phChopBody's fold-into-the-largest-piece safety net dead code and let
  // PH.bodies grow past PH.maxBodies (see the keep loop there).
  const phMakeRoom = (pending) => {
    if (PH.bodies.length + (pending | 0) < PH.maxBodies) return true;
    let oi = -1;
    for (let i = 0; i < PH.bodies.length; i++) { const b = PH.bodies[i];
      if (b.absorbing) continue;                       // in flight to the player — never
      if (b.n > PH.absorbSize) continue;               // scenery — skip it on this pass
      if (oi < 0 || b.born < PH.bodies[oi].born) oi = i; }
    if (oi < 0) for (let i = 0; i < PH.bodies.length; i++) { if (PH.bodies[i].absorbing) continue;
      if (oi < 0 || PH.bodies[i].born < PH.bodies[oi].born) oi = i; }   // nothing but scenery left: the budget is genuinely exhausted, so fall back to the oldest — still never one in flight
    if (oi < 0) return false;
    // ── AND RECLAIMING IS NOT DESTROYING ── this used to splice the victim and let its voxels cease to exist.
    // physRetire writes a body's voxels back into W at its resting pose (into EMPTY cells only, so it can never
    // overwrite terrain) and queues them for the support resolver, so what it gives back either stands where it
    // fell or is dropped like any other orphan. Deleting is now only the fallback for a body that cannot be
    // written back at all. Same rule the user asked for on the felled tree: nothing disappears un-absorbed.
    const vb9 = PH.bodies[oi];
    if (!physRetire(vb9)) PH.stats.reclaimLost = (PH.stats.reclaimLost | 0) + 1;
    PH.bodies.splice(oi, 1); PH.stats.reclaimed++;
    return true;
  };
  // ── THE ONLY THING THAT GIVES SHAPE MEMORY BACK ── bodyTop is a bump allocator and nothing decrements it, so
  // every PH.bodies.splice() in the game — expiry, voidFall, offRect, a finished absorb, phMakeRoom above, the
  // target splice in phChopBody — abandons that body's cells in bodyBuf forever. Repacking the live list from
  // the front is what recovers them, and it is O(live bodies) uploads, so it is only ever run on demand.
  const phPack = () => {
    bodyTop = 0;                                     // repack the survivors from the start of the buffer
    for (const b2 of PH.bodies) {
      if (!b2.gpu || !b2.cpuGrid) continue;
      device.queue.writeBuffer(bodyBuf, bodyTop * 4, b2.cpuGrid.buffer, 0, b2.cpuGrid.length * 4);
      b2.gpu.off = bodyTop; bodyTop += b2.cpuGrid.length;
    }
  };
  // ── COMPACT BEFORE EVICTING ── this used to splice a LIVE body first and only then repack, so it destroyed
  // settled chunks beside the player to make room that the abandoned allocations above would have yielded on
  // their own. Worse, its `PH.bodies.length &&` guard meant that when phChopBody had just spliced out the ONLY
  // live body — which it does on every swing at a felled log — nothing ran at all, bodyTop still held the
  // stale allocation, and phBuildBody's ceiling test then handed back a body with gpu = null and no cpuGrid:
  // the log the player was chopping was not drawn, not solid and not hittable, while still holding a slot.
  const phReclaim = (need) => {
    if (BODYCAP - bodyTop >= need) return;           // fits as the buffer stands — no upload, and nothing is ever evicted for a body that already had room
    phPack();                                        // …then take back what the splices abandoned BEFORE destroying anything the player can see
    let guard = 0;
    while (PH.bodies.length && BODYCAP - bodyTop < need && guard++ < 64) {
      let oi = -1;                                     // …and the same rule here: a chunk in flight is never the one retired (see phMakeRoom)
      for (let i = 0; i < PH.bodies.length; i++) { if (PH.bodies[i].absorbing) continue;
        if (oi < 0 || PH.bodies[i].born < PH.bodies[oi].born) oi = i; }
      if (oi < 0) break;
      PH.bodies.splice(oi, 1);
      PH.stats.reclaimed++;
      phPack();
    }
  };
  const phQRot = (q, v, o) => {                      // o = q * v * q^-1
    const x = q[0], y = q[1], z = q[2], w = q[3];
    const tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
    return o;
  };
  const phQNorm = (q) => { const l = Math.hypot(q[0], q[1], q[2], q[3]) || 1; q[0] /= l; q[1] /= l; q[2] /= l; q[3] /= l; };
  const phTmp = [0, 0, 0], phTmp2 = [0, 0, 0], phNrm = [0, 0, 0];
  const PHX = [1, 0, 0], PHY = [0, 1, 0], PHZ = [0, 0, 1], PHAX = [0, 0, 0], PHAY = [0, 0, 0], PHAZ = [0, 0, 0];
  // STATIC terrain only — bodies do not collide with each other in this pass.
  // FOLIAGE COUNTS HERE (user 2026-08-02: "leaves have hitboxes for everything but the player"). The
  // canopy is walk-through for the PLAYER via solidTab, and stays that way — this is a separate test used
  // only by the rigid-body solver, so a falling trunk now catches on neighbouring branches the way a real
  // one hangs up, while you can still walk through the crown.
  const phSolidAt = (x, y, z) => {
    if (y < 0) return true;
    if (y >= WY) return false;
    const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    return solidTab[id] === 1;                       // FOLIAGE IS NOT SOLID (user): leaves have no hitbox, so a crown swings through the next tree's needles and a chunk falls past them to the ground rather than hanging in mid-air
  };
  // Outward normal of a solid cell: point toward whichever faces are open. Gives real normals on slopes
  // and walls instead of assuming the ground is flat.
  const phNormalAt = (x, y, z, out) => {
    let nx = 0, ny = 0, nz = 0;
    if (!phSolidAt(x + 1, y, z)) nx += 1; if (!phSolidAt(x - 1, y, z)) nx -= 1;
    if (!phSolidAt(x, y + 1, z)) ny += 1; if (!phSolidAt(x, y - 1, z)) ny -= 1;
    if (!phSolidAt(x, y, z + 1)) nz += 1; if (!phSolidAt(x, y, z - 1)) nz -= 1;
    const l = Math.hypot(nx, ny, nz);
    if (l < 1e-6) { out[0] = 0; out[1] = 1; out[2] = 0; return out; }   // fully buried — push straight up
    out[0] = nx / l; out[1] = ny / l; out[2] = nz / l; return out;
  };
  // Contact scratch (preallocated; the solver carries NO state between steps, like Teardown)
  const cR = new Float64Array(PH.maxContacts * 3), cN = new Float64Array(PH.maxContacts * 3), cD = new Float64Array(PH.maxContacts);
  // Direction a newly severed piece should fall. Set from the swing (you fell a tree away from
  // yourself); falls back to "away from the player" when chopped programmatically.
  const phFallDir = [0, 0, 1];
  const phSetFallDir = (dx, dz) => { const l = Math.hypot(dx, dz); if (l > 1e-4) { phFallDir[0] = dx / l; phFallDir[2] = dz / l; } };
  // ── AXE BITE -> FALLING CHUNK ── the voxels the axe carves out used to be deleted outright, so a hit
  // just made material vanish. They are now kept: split into 6-connected pieces and thrown clear as real
  // bodies. A LONE TRUNK voxel is dropped rather than spawned (single brown specks tumbling off every
  // swing read as litter, not debris); a lone FOLIAGE voxel is kept, because scattered green flecks
  // falling out of the canopy is exactly what a struck tree should shed.
  const PHSRC = {};                                    // how many bodies each path built — which one actually fells a tree?
  let phSrc = '?';   // which path built the body — stamped onto it below. Cheap, and it is what identified a sweep that was tearing perched birds into debris (user 2026-08-05).
  const phSpawnChunk = (S, quads) => { phSrc = 'treeChunk';
    if (!quads.length) return;
    const sx = S.R.sx, sz = S.R.sz, hM = S.hMax || MSZ;
    const key = (mx, my, mz) => mx + mz * sx + my * sx * sz;
    const idMap = new Map();
    for (let i = 0; i < quads.length; i += 4) idMap.set(key(quads[i], quads[i + 1], quads[i + 2]), quads[i + 3]);
    const left = new Set(idMap.keys());
    const f = { sx, sz };
    // One body per 6-CONNECTED PIECE of the bite (user reverted the single-body version): a cut that
    // clips two branches throws two chunks, which is what it looks like it should do.
    while (left.size) {
      const start = left.values().next().value;
      const comp = []; const st = [start]; left.delete(start);
      while (st.length) {
        const k = st.pop(); comp.push(k);
        const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
        for (let d = 0; d < 6; d++) {
          const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
          const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
          const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= hM) continue;
          const nk = key(nx, ny, nz);
          if (left.has(nk)) { left.delete(nk); st.push(nk); }
        }
      }
      if (comp.length < 2) continue;                   // a lone speck is litter, not a chunk
      if (PH.bodies.length >= PH.maxBodies && !phMakeRoom()) continue;
      const b = phBuildBody(S, comp, f, idMap);
      b.vel[0] = phFallDir[0] * 6 + (Math.random() - 0.5) * 4;     // thrown clear of the cut
      b.vel[1] = 4 + Math.random() * 3;
      b.vel[2] = phFallDir[2] * 6 + (Math.random() - 0.5) * 4;
      b.omega[0] = (Math.random() - 0.5) * 3; b.omega[1] = (Math.random() - 0.5) * 3; b.omega[2] = (Math.random() - 0.5) * 3;
      b.absorbAt = performance.now() + PH.absorbMs;
      PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(b);
      PH.stats.chunks++;
    }
  };
  // ── WAKE EVERY HANGER-ON AROUND SOMETHING THAT JUST LEFT ── a pinecone hangs from ONE anchor, and it is stamped
  // CENTRED on that anchor, so what it actually rests against is usually DIAGONAL and often more than one voxel
  // away. Hooks keyed on "a neighbour of a cleared voxel" therefore keep missing them. This asks a blunter
  // question instead: whatever just came out of the world, hand the resolver every cone standing in its volume
  // widened by a CROWN RADIUS, and let the flood judge each cluster as a whole. Bounded by the component's own
  // bounding box and it runs once, at the moment of the lift.
  const CONE_WAKE_PAD = 26;                            // a crown radius, for a LIFT whose bbox is only the trunk
  const coneWake = (x0, x1, y0, y1, z0, z1, pad) => {
    const PADW = pad === undefined ? CONE_WAKE_PAD : pad;   // cones, canopy snow AND fruit — all three hang off a crown and all three are stranded when it goes
    for (let z2 = z0 - PADW; z2 <= z1 + PADW; z2++)
      for (let y2 = Math.max(1, y0 - PADW); y2 <= Math.min(WY - 1, y1 + PADW); y2++)
        for (let x2 = x0 - PADW; x2 <= x1 + PADW; x2++) {
          const jj = gwrap(x2, WX) + y2 * WX + gwrap(z2, WZ) * WX * WY;
          const v2 = W[jj]; if (!v2 || !(coneTab[v2] || snowTab[v2] || hangTab[v2])) continue;   // hangTab is the FRUIT (assets/material-tabs.js) — cones and snow were the only two hangers this knew about, so a felled oak left its apples and oranges in the air
          // ── ONLY WAKE WHAT COULD ACTUALLY BE HANGING ── waking every cone and snow voxel in the box pushed
          // HUNDREDS OF THOUSANDS of cells during a storm (measured: 400k queued, a 368k backlog against a
          // 2 ms/frame budget). The resolver then took minutes to reach a genuine floater, so cones and snow sat
          // in mid-air — and only ever when it had snowed, because snow is what filled the queue. Anything
          // resting on something is not a candidate, and that is almost all of it.
          if (y2 > 1 && W[jj - WX]) continue;
          supPush(jj);
        }
  };
  // Every path that ERASES world voxels can strand what was hanging on them. A chop that takes NEEDLES is the
  // worst of them: cones and canopy snow both rest on needles, and phChopLeaves runs on every swing that grazes
  // foliage. Small pad for a bite (the removed voxels ARE the anchors), crown-wide only for a LIFT, whose bbox
  // is just the trunk while its cones ring the whole crown.
  const wakeFrom = (cleared, pad) => {
    if (!cleared || !cleared.length) return;
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9;
    // ── WORLD COORDS, NOT GRID COORDS ── x/z are toroidal, so a raw min/max over grid indices gives a
    // 2048-WIDE box for any cleared set that straddles the wrap, and coneWake iterates the box VOLUME.
    // Same defect, same fix as supDrop (2026-08-10, measured there at 88.3 ms in one call).
    for (const ii of cleared) {
      const gy2 = ((ii / WX) | 0) % WY;
      const wx2 = supWorldX(ii % WX), wz2 = supWorldZ((ii / (WX * WY)) | 0);
      if (wx2 < x0) x0 = wx2; if (wx2 > x1) x1 = wx2;
      if (gy2 < y0) y0 = gy2; if (gy2 > y1) y1 = gy2;
      if (wz2 < z0) z0 = wz2; if (wz2 > z1) z1 = wz2;
    }
    coneWake(x0, x1, y0, y1, z0, z1, pad);
  };
  // ── THE BLANKET COMES DOWN WITH THE TREE (user 2026-08-08: "make the snow stay with the pine tree as it
  // falls") ── canopy snow is a SEPARATE voxel resting one course above the needles it settled on, and it
  // belongs to no tree model: phPresent answers 0 for it, so the tree flood never sees it and the severed crown
  // lifted out from under its own blanket. What stayed behind was a VERTICAL COLUMN of at most 3 snow voxels
  // (supFlood forbids horizontal links through snow — the cantilever rule), i.e. a component of n < 3, which
  // supResolve erases as litter. That is why the snow used to hang and now vanishes the instant the tree moves.
  // Hand those cells to the BODY instead — the same thing supDrop already does for a drape lift — so the snow
  // rides the trunk down and lands with it.
  //
  // Walk straight UP from every non-snow cell of the component; landSnowAt caps a stack at 3, so the loop is
  // never more than a few steps. `claimed` stops two components of the same fell taking the same voxel twice.
  // The head room past the model box is what carries the snow on the very APEX: the pine fills its .vox box to
  // the last course (z 115 of 116), so its topmost cap sits outside. Nothing indexes phMark or R.A with these
  // cells — they are body-local coordinates from here on — and phBuildBody only ever bounds-checks them.
  // ── …AND SO DO THE PINECONES (user 2026-08-08, with a photograph of them hanging in open sky) ── the exact
  // mirror. Snow RESTS ON the crown, so it is found by walking UP; a cone HANGS FROM it, so it is found by
  // walking DOWN. Both are the drape flood's own rules, applied at the moment of the lift instead of after it.
  // MEASURED before this: felling one pine left `floaters 5 (64 vox) — cone x13, cone x13, cone x13, cone x13`
  // in the air, cleared by the resolver about 3 seconds later. Three seconds is long enough to see, long
  // enough to photograph, and behind a post-storm thaw backlog it was very much longer. Carrying them costs
  // nothing and removes ~20 rigid-body slot demands from every felling as well.
  //
  // A cone is one cluster hanging from ONE overhead anchor, so the whole cluster is taken or none of it is:
  // flood it 26-connected through cone ids, and require every non-cone voxel sitting directly on top of any
  // cluster cell to be leaving with us. If ANY overhead anchor stays — a neighbouring pine's needle in the
  // overlap, a branch on the standing stump — the cone still has a hanger and must stay where it is. Anything
  // that does not fit the box, the size cap, or that test is simply left to the resolver, exactly as before.
  const SNOW_CAP_HEAD = 3;                             // courses above the model box a carried stack may reach — landSnowAt's own 3-layer cap
  const CONE_CLUSTER_MAX = 64;                         // a pinecone is ~13 voxels; this is a runaway guard, not a tuning knob
  const phDrapeWith = (S, comp, f, claimed) => {
    const add = [], sx = f.sx, sz = f.sz, hM = S.hMax || MSZ;
    const li = (mx, my, mz) => mx + mz * sx + my * sx * sz;
    const inComp = new Set(comp);
    const inBox = (mx, my, mz) => mx >= 0 && mx < sx && mz >= 0 && mz < sz && my >= 0 && my < hM + SNOW_CAP_HEAD && S.gy + my > 0 && S.gy + my < WY - 1;
    let nSnow = 0, nCone = 0;
    for (const k of comp) {
      const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      // ── IT RESTS ON US: SNOW, STRAIGHT UP ── landSnowAt caps a stack at 3, so this is a few steps at most
      if (!snowTab[W[phWorldIdx(S, mx, my, mz)]]) {     // a snow voxel's own stack is already in here
        for (let y2 = my + 1; y2 < hM + SNOW_CAP_HEAD && S.gy + y2 < WY - 1; y2++) {
          const ii = phWorldIdx(S, mx, y2, mz);
          if (!snowTab[W[ii]] || claimed.has(ii)) break;
          claimed.add(ii); add.push(li(mx, y2, mz)); nSnow++;
        }
      }
      // ── IT HANGS FROM US: A CONE, STRAIGHT DOWN ── one cell below, then the whole cluster it belongs to
      if (my < 1 || !inBox(mx, my - 1, mz)) continue;
      const j0 = phWorldIdx(S, mx, my - 1, mz);
      if (!coneTab[W[j0]] || claimed.has(j0)) continue;
      const cl = [], st = [[mx, my - 1, mz]], seenC = new Set([li(mx, my - 1, mz)]);
      let ok = true;
      while (st.length && ok) {
        const cur = st.pop(), cx = cur[0], cy = cur[1], cz = cur[2];
        if (cl.length >= CONE_CLUSTER_MAX) { ok = false; break; }
        if (claimed.has(phWorldIdx(S, cx, cy, cz))) { ok = false; break; }   // another component already took it
        cl.push(cur);
        for (let d = 0; d < 27 && ok; d++) {
          const nx = cx + (d % 3) - 1, ny = cy + (((d / 3) | 0) % 3) - 1, nz = cz + ((d / 9) | 0) - 1;
          if (nx === cx && ny === cy && nz === cz) continue;
          if (!inBox(nx, ny, nz)) { ok = false; break; }   // the cluster runs out of the model frame — not ours to carry
          const nv = W[phWorldIdx(S, nx, ny, nz)];
          if (!nv) continue;
          if (coneTab[nv]) { const nk = li(nx, ny, nz); if (!seenC.has(nk)) { seenC.add(nk); st.push([nx, ny, nz]); } continue; }
          // a NON-cone voxel directly overhead is a hanger: it has to be one of ours, or the cone stays put
          if (ny === cy + 1 && nx === cx && nz === cz && !inComp.has(li(nx, ny, nz))) ok = false;
        }
      }
      if (!ok) continue;
      for (const q of cl) { claimed.add(phWorldIdx(S, q[0], q[1], q[2])); add.push(li(q[0], q[1], q[2])); nCone++; }
    }
    PH.stats.snowCarried += nSnow; PH.stats.coneCarried += nCone;
    return add.length ? comp.concat(add) : comp;
  };
  const phSeparate = (S, f) => { phSrc = 'treeSeparate';                     // orphans -> rigid bodies; the SAME cell list drives the world erase, so a voxel is in exactly one state
    const t0 = performance.now();
    const made = [], cellsOut = [], comps = [], snowClaim = new Set();
    // ── SCAN THIS TREE'S BOX, NOT THE BUFFER'S (2026-08-19, found while measuring the fell spike) ── phMark is
    // allocated once and only ever GROWS: phFlood sizes it to sx * sz * MSZ and reuses it, so after one big oak
    // it is ~1.17M entries and stays that way. Scanning phMark.length therefore made every later separation pay
    // the LARGEST tree ever chopped this session — a pine's own box is 35 x 36 x MSZ = 146k, so a pine felled
    // after an oak was walking 8x the cells it has, decomposing each into an mx/my/mz that is outside the model
    // and asking phPresent about it. Harmless (S.R.A returns undefined and the cell is skipped) and pure waste.
    // The bound is phFlood's own formula, so the two cannot drift apart.
    // ── AND IT WALKS THE TREE'S OWN VOXELS, NOT ITS BOX (2026-08-19, the felling spike) ── S.cells is the
    // SPARSE list of occupied model indices, built in sim/physics.js beside the rotated shape,
    // in exactly the li = mx + mz*sx + my*sx*sz layout this loop decodes. Its own comment there records why it
    // exists: it is what makes the FLOOD's seed pass O(voxels) instead of O(box). This scan was the one place
    // that had not been given it.
    // MEASURED on the oak the user felled: the box is 101 x 100 x MSZ = 1,171,600 cells and the tree is 67,204
    // voxels — a 17x difference, and every one of those 1.1M iterations was doing two divisions and a modulo to
    // rebuild coordinates for a cell that is empty 94% of the time.
    // The result is IDENTICAL, not approximate: phPresent can only return non-zero for a cell the model
    // occupies, so the cells this skips are exactly the ones the old loop rejected. The phMark and phPresent
    // tests stay — a listed cell can still have been marked by an earlier component, or been removed from W.
    // A PINE keeps the box scan (its shape carries cells: null) and does not care: 36 x 35 x MSZ is 146k, an
    // eighth of the oak's box, and it is the oak that is felt.
    const list9 = S.cells, nAll9 = list9 ? list9.length : f.sx * f.sz * (S.hMax || MSZ);
    for (let q9 = 0; q9 < nAll9; q9++) {
      const k = list9 ? list9[q9] : q9;
      if (phMark[k] !== 0) continue;
      const mx0 = k % f.sx, mz0 = ((k / f.sx) | 0) % f.sz, my0 = (k / (f.sx * f.sz)) | 0;
      if (!phPresent(S, mx0, my0, mz0)) continue;
      comps.push(phComponent(S, f, k));
    }
    comps.sort((a, b2) => b2.length - a.length);     // LARGEST FIRST — a shower of chips must never starve the trunk of a slot
    for (const comp of comps) {
      if (comp.length < 2) {                          // ── LITTER FIRST, AND IT NEVER COSTS A SLOT ── this sat BELOW the shedding loop, where a one-voxel component can only ever fail `sn < comp.length` (sn < 1 is false for any size) and fall through to phMakeRoom, which splices a LIVE body. So a single detached needle destroyed a settled chunk to buy a slot it was never going to use — the voxel is erased as litter three lines later. Comps arrive largest-first, so these are all at the tail anyway.
        const c0 = comp[0], mxa = c0 % f.sx, mza = ((c0 / f.sx) | 0) % f.sz, mya = (c0 / (f.sx * f.sz)) | 0;
        const ii0 = phWorldIdx(S, mxa, mya, mza);
        phMark[c0] = 3;
        W[ii0] = 0; cellsOut.push(ii0); PH.stats.dustVox++;   // a lone detached voxel is litter, needle or not — no falling leaves (user 2026-08-02)
        continue;
      }
      // ── A SEVERED TREE MUST GET A SLOT (user 2026-08-07: "a pine tree floating entirely from the base where
      // it was broken") ── this used to try ONE eviction and ONE phMakeRoom and then give up, leaving the whole
      // component in W and handing it to the support resolver. That is a dead end for anything tree-sized: the
      // resolver's structure flood caps at SUP.cap (2000) and reads "capped" as ANCHORED, so a 7000-voxel pine
      // it was asked to rescue is declared attached and hangs off its own cut base forever. Rare precisely
      // because it needs all 16 slots busy at the instant of the fell. Now it keeps shedding — the smallest
      // live body first while that body is smaller than this component, then the oldest airborne debris — until
      // there is room or there is genuinely nothing left to shed. Components arrive largest-first, so the trunk
      // gets first refusal and a shower of chips can never outbid it.
      // …and it sheds SCRAP only (see phMakeRoom): evicting by size alone made the first felled tree the prize
      // the second one paid for, since a trunk is smaller than nothing and larger than every chip.
      let shed9 = 0;
      while (PH.bodies.length >= PH.maxBodies && shed9 < 6) {
        let si = -1, sn = Infinity;
        for (let q2 = 0; q2 < PH.bodies.length; q2++) { const b2 = PH.bodies[q2];
          if (b2.absorbing) continue;                  // …nor a chunk already on its way to the player (see phMakeRoom)
          if (b2.n > PH.absorbSize) continue;          // never trade a felled log for another one
          if (b2.n < sn) { sn = b2.n; si = q2; } }
        // ── AND SHEDDING IS NOT DESTROYING EITHER ── this spliced the victim outright, which is the one path
        // in the fell that let real geometry cease to exist: the very next line calls erasing "the last
        // resort, not the first" and phMakeRoom below has retired its victims since the reclaim work, but
        // this arm — which runs FIRST, and is the one that actually fires when a stand is being cleared —
        // never learned it. MEASURED felling 22 birches from one spot: evicted 6, i.e. six live chunks of
        // already-felled trees deleted out of the world to seat the next trunk. physRetire writes them back
        // into W at their resting pose (empty cells only, then queued for the support resolver), so a shed
        // chunk stays on the ground to be chopped instead of vanishing while the player watches.
        if (si >= 0 && sn < comp.length) { PH.stats.evicted++;
          if (!physRetire(PH.bodies[si])) PH.stats.evictLost = (PH.stats.evictLost | 0) + 1;
          PH.bodies.splice(si, 1); shed9++; continue; }
        if (phMakeRoom()) { shed9++; continue; }        // shed an airborne leaf — erasing real geometry is the last resort, not the first
        break;                                         // nothing left to shed: fall through to the requeue below, which still never erases
      }
      if (PH.bodies.length >= PH.maxBodies) {         // still full — REQUEUE, never erase (the invariant: no component of >= 3 voxels is ever destroyed by the support system)
        // COUNTED, because this arm is invisible from outside and it is the one that turns "I knocked it down"
        // into "it did not break": the trunk never becomes a body here, so nothing ever arms fellWhole, and the
        // only thing that can still rescue it is the support resolver forcing a slot some frames later.
        // fellNoSlot counts every component; fellNoSlotBig counts the tree-sized ones, which are the reports.
        PH.stats.fellNoSlot = (PH.stats.fellNoSlot | 0) + 1;
        if (comp.length > PH.fellChunkVox) PH.stats.fellNoSlotBig = (PH.stats.fellNoSlotBig | 0) + 1;
        // This used to delete the whole component outright, which is a silent hole in the world rather
        // than a floater, and it stranded whatever had been resting on it. The cells stay exactly where
        // they are and go to the resolver instead: it retries every frame, and after SUP_BLOCK_MAX blocked
        // frames it forces a slot. A body that lives one second longer than it should is invisible; a
        // 900-voxel crown deleted because sixteen chips were in flight is not.
        for (const c of comp) { phMark[c] = 3;
          const mx2 = c % f.sx, mz2 = ((c / f.sx) | 0) % f.sz, my2 = (c / (f.sx * f.sz)) | 0;
          supPush(phWorldIdx(S, mx2, my2, mz2)); }
        continue;
      }
      const compS = phDrapeWith(S, comp, f, snowClaim);
      const b = phBuildBody(S, compS, f);
      for (const c of compS) {
        const mx2 = c % f.sx, mz2 = ((c / f.sx) | 0) % f.sz, my2 = (c / (f.sx * f.sz)) | 0;
        const ii = phWorldIdx(S, mx2, my2, mz2); W[ii] = 0; cellsOut.push(ii);
      }
      for (let lift = 0; lift < 32; lift++) {        // spawn de-penetration: a component cut near the ground can be born inside terrain
        let inside = 0;
        for (let i = 0; i < b.probes.length && !inside; i++) { const pi = b.probes[i];
          if (phSolidAt(Math.floor(b.pos[0] + b.lx[pi] + 0.5 - b.com[0]), Math.floor(b.pos[1] + b.ly[pi] + 0.5 - b.com[1]),
                        Math.floor(b.pos[2] + b.lz[pi] + 0.5 - b.com[2]))) inside = 1; }
        if (!inside) break;
        b.pos[1] += 1;
      }
      // ── TOPPLE ── a trunk cut through sits with its centre of mass directly over the stump: that is
      // unstable equilibrium, and with no nudge it just drops vertically THROUGH the stump. Give it an
      // initial spin about the horizontal axis perpendicular to the fall direction, which tips the top
      // toward the cut. omega = normalize(up x d) makes the point at +up move toward +d, so the crown
      // leans the way the notch faces and gravity takes over from there. Only pieces tall enough to
      // topple get it — chips just fall.
      if (b.n > PH.retireMax * 4) {
        // scatter the drop line about the notch direction, so felling from the same spot twice does not
        // lay both trunks along the same line (user)
        const sa = Math.atan2(phFallDir[0], phFallDir[2]) + (Math.random() * 2 - 1) * PH.fellSpread;
        phFallDir[0] = Math.sin(sa); phFallDir[2] = Math.cos(sa);
        PH.stats.lastFellDeg = Math.round(sa * 57.2958);
        const ax = -phFallDir[2], az = phFallDir[0];   // up x d — makes the point at +up move toward +d
        const al = Math.hypot(ax, az) || 1;
        b.tipAx = ax / al; b.tipAz = az / al;          // the topple axis, held for the whole fall (see phStep)
        // ── ARMED, NOT DRIVING ── the drive PRESCRIBES rotation, which the contact solver cannot argue
        // with, so starting it at the cut let the trunk rotate straight through its own stump (user).
        // Drop cleanly onto the cut face first; phStep starts the topple once it has actually landed.
        // ── THE TOPPLE DRIVE, BACK (user 2026-08-22: "can you bring back the tilting physics for the trees?")
        // ── removed earlier the same day when the shared fall direction read as an auto-tilt; the tree standing
        // straight up on its stump was worse. It keeps its own random scatter (PH.fellSpread) so trees do not
        // all go the same way, and it now runs at FULL gravity rather than the old 0.39 slow-motion.
        b.tipArm = 1; b.tipArmT = performance.now();
        b.noAbsorb = true;                           // the TOPPLING TRUNK is the tree falling, not debris — it must never fly into the player
        b.fellWhole = 1;                             // …and it BREAKS when it lands (sim/solver.js arms this, sim/chop-tree.js does it), which is also where noAbsorb is lifted
        // ── FULL GRAVITY (user 2026-08-22: "the trees seem to have space like gravity" / "make it more
        // realistic") ── slowFall is a MULTIPLIER on gravity in sim/solver.js, not a flag: undefined means 1.
        // Setting it to 0 when the staged fall was removed meant gravity x 0 — the trunk had none at all, and
        // what little motion it had came from the one nudge at the cut. 1 is ordinary gravity, which is also
        // what "more realistic" asks for: the old 0.39 was a deliberate slow-motion for the staged topple and
        // there is no staged topple any more.
        b.slowFall = 1;
        b.omega[0] = b.omega[1] = b.omega[2] = 0;    // the drive owns the rotation from here — no spin until it is going over
        b.vel[0] = b.vel[2] = 0;                     // …and straight down onto the cut face, so it meets the stump square
      }
      if (!b.noAbsorb) b.absorbAt = performance.now() + PH.absorbMs;   // BROKEN PIECES are collectable too (user): before this only swing-carved chunks had a timer, so separated pieces lay on the ground forever
      PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(b); made.push(b);
    }
    if (cellsOut.length) gpuPatch(cellsOut, false);
    if (cellsOut.length) {                           // whatever came off the body takes its cones with it
      let bx0 = 1e9, bx1 = -1e9, by0 = 1e9, by1 = -1e9, bz0 = 1e9, bz1 = -1e9;
      // ── WORLD COORDS, NOT GRID COORDS ── x/z are toroidal, so a raw min/max over grid indices gives a
      // 2048-WIDE box for any cleared set that straddles the wrap, and coneWake iterates the box VOLUME.
      // Same defect, same fix as supDrop (2026-08-10, measured there at 88.3 ms in one call).
      for (const ii of cellsOut) {
        const gy2 = ((ii / WX) | 0) % WY;
        const wx2 = supWorldX(ii % WX), wz2 = supWorldZ((ii / (WX * WY)) | 0);
        if (wx2 < bx0) bx0 = wx2; if (wx2 > bx1) bx1 = wx2;
        if (gy2 < by0) by0 = gy2; if (gy2 > by1) by1 = gy2;
        if (wz2 < bz0) bz0 = wz2; if (wz2 > bz1) bz1 = wz2;
      }
      // ── THE 26 PAD IS FOR A TRUNK-SHAPED BOX, AND A FELLED OAK'S IS NOT ── CONE_WAKE_PAD is "a crown
      // radius", and it exists because a LIFT whose bbox is only the trunk has to reach out to the cones and
      // canopy snow hanging off a crown that is nowhere near that box. When the component IS the crown, the
      // box already contains all of it and the pad is pure volume: coneWake iterates the box, so a 114-wide
      // oak went 114+52 cubed = 4.5M cells (~20-30 ms) on the one frame the tree comes down, which is the
      // largest single term in that hitch. A box wider than a crown radius cannot be a bare trunk, so it does
      // not need a crown radius of reach — 4 covers the voxel or two of drape that can overhang a lifted mass.
      // Chosen at 40 so nothing that exists today changes: a felled PINE is 36 wide (MSX/MSY) and keeps the
      // 26 it has always had; only the oaks, whose crowns run 44 to 114, take the cheap path.
      // ── AND 40 WAS TUNED AROUND THE TWO TREES THAT EXISTED (2026-08-26) ── the note above picked 40 so a
      // felled PINE (36 wide) kept the 26 it had always had and only the oaks took the cheap path. A BIRCH
      // crown is 29 to 61, which lands ON that line: MEASURED, a fell with wSpan 32 took the expensive arm
      // and coneWake cost ~18 ms of a 27 ms phSeparate, iterating (32 + 52)^3 = 593,000 cells on the one
      // frame the tree comes down. Two voxels wider and the same tree pays 40^3 = 64,000. That is both the
      // fall hitch and why it is INTERMITTENT - the birch straddles the threshold.
      // The question the pad actually answers is "could drape be hanging OUTSIDE this component's box", and
      // the thing that answers it is whether the box is TRUNK-shaped, not whether it is oak-sized. A bare
      // trunk section is 3 to 8 voxels across; every crown that exists is 29 or more. 16 sits in the middle
      // of a gap that wide, so the classification is unambiguous for every model, and the pine joins the oak
      // on the cheap path for exactly the reason the oak is already on it: when the component IS the crown,
      // its own box already contains the drape and the pad is pure volume.
      const CONE_WAKE_TRUNK = 16;                    // a box wider than this is a crown, not a bole
      const wSpan = Math.max(bx1 - bx0, bz1 - bz0);
      coneWake(bx0, bx1, by0, by1, bz0, bz1, wSpan >= CONE_WAKE_TRUNK ? 4 : undefined);
      petalClearBox(bx0, bx1, bz0, bz1);             // …and the leaves already falling out of the crown that just left: see the note on petalClearBox
    }
    PH.stats.separations += made.length;
    PH.stats.lastSepMs = +(performance.now() - t0).toFixed(2);
    return made;
  };
  // Build a body out of a subset of ANOTHER body's voxels, keeping the parent's pose. phBuildBody works
  // in the tree-model frame the parent's lx/ly/lz are still expressed in, so replaying the parent's
  // origin as a pseudo-shape is enough to reuse it wholesale. The new COM sits somewhere else in that
  // frame, so the world position has to be carried across through the parent's rotation — otherwise the
  // piece teleports back to where the tree originally grew.
  const phSubBody = (b, cells, idMap) => { phSrc = 'subBody';
    const nb = phBuildBody({ bx: b.origin[0], gy: b.origin[1], bz: b.origin[2] }, cells, { sx: b.sx, sz: b.sz }, idMap);
    phTmp[0] = nb.com[0] - b.com[0]; phTmp[1] = nb.com[1] - b.com[1]; phTmp[2] = nb.com[2] - b.com[2];
    phQRot(b.q, phTmp, phTmp2);
    nb.pos[0] = b.pos[0] + phTmp2[0]; nb.pos[1] = b.pos[1] + phTmp2[1]; nb.pos[2] = b.pos[2] + phTmp2[2];
    nb.q = [b.q[0], b.q[1], b.q[2], b.q[3]];
    phQRot(nb.q, PHX, nb.ax); phQRot(nb.q, PHY, nb.ay); phQRot(nb.q, PHZ, nb.az);
    nb.vel[0] = b.vel[0]; nb.vel[1] = b.vel[1]; nb.vel[2] = b.vel[2];
    nb.omega[0] = b.omega[0]; nb.omega[1] = b.omega[1]; nb.omega[2] = b.omega[2];
    return nb;
  };
