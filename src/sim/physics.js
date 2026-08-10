  // ── VOXEL DESTRUCTION + RIGID BODIES ───────────────────────────────────────
  // Modelled on Teardown (Tuxedo Labs), whose approach is documented by its author:
  //   · detachment is a plain 6-CONNECTED FACE-ONLY flood fill, run per breakage event, not per frame
  //     ("it's just a flood fill, there's absolutely nothing special to it")
  //   · mass / centre of mass / inertia are BRUTE-FORCE SUMS over the voxels, recomputed on every
  //     break ("you just sum up all the voxels ... there is no hand tweaking")
  //   · there is deliberately NO structural-stress model — connectivity alone decides what falls
  //   · voxels collide as SPHERES, not cubes
  //   · contacts are pruned by classifying voxels edge/corner/face/interior; a shipping container goes
  //     from 1000+ candidate contacts to 4 ("that's kind of the key to making it run at this speed")
  //   · solver is sequential impulse / PGS at a FIXED 60 Hz with 8 iterations, and carries NO contact
  //     or pair state between frames — only body state persists
  //
  // A pine's world voxels are a PURE FUNCTION of its tree cell: stampTree writes
  //   A[mx + mz*R.sx + my*R.sx*R.sz]  ->  world (bx+mx, gy+my, bz+mz)
  // so local<->world is an exact affine map and no per-voxel ownership table is needed; treeAt() is the
  // deterministic reverse lookup. That is what makes an "object-local destructible shape" free here.
  const PH = {
    on: !location.search.includes('nophys'),
    dt: 1 / 60,                                      // FIXED 60 Hz, like Teardown — the sim must not vary with render fps
    iters: 8,                                        // sequential-impulse iterations per step (Teardown ships 8)
    acc: 0, bodies: [], maxBodies: 16,
    gravity: 200,                                    // matches the player's GRAVITY so falls read at the same scale
    linDamp: 0.05, angDamp: 0.18, restitution: 0.0, friction: 0.6,
    sleepLin: 1.2, sleepAng: 0.30, sleepFrames: 40,
    abAll: new Float64Array([1e30, 1e30, 1e30, -1e30, -1e30, -1e30]),   // union of every live body's world AABB — phBodySolid's O(1) "nowhere near anything" reject
    sleepAngFree: 24,                                // at or under this many voxels, rest is judged on LINEAR motion alone — a cone is 13 and its spin is solver noise, not motion
    retireFar: 64,                                   // …and a SETTLED body up to this size is baked back too, once it is further than retireFarR away — see the sleep site. 64 covers a cone (13) and a needle tuft without touching anything the player would call a log.
    retireFarR: 48,                                  // 3x absorbR: far enough that nobody is mid-walk toward it
    retireMax: 6,                                    // a settled body this small is written back into the grid and its slot freed
    maxProbes: 512, maxContacts: 96, maxCCD: 12,     // maxCCD caps the adaptive substepping (see physStep).
    //   maxProbes was 160, but an 8-voxel bucketing of a 35x93x36 crown yields ~300 buckets — so barely
    //   half the body's surface cells carried a probe, and a narrow stump could pass clean between them.
    //   Probes only cost a grid lookup during contact GENERATION (once per substep), so 512 is cheap.
    treeLifeMs: 300000, chunkLifeMs: 60000,          // ── LIFETIMES (user) ── a felled tree is gone after 5 min, any chunk the player never absorbed after 60 s.
    //                                                 Told apart by noAbsorb, which marks the toppling trunk and nothing else.
    absorbR: 16,                                     // …and a body already AT REST on the ground is drawn in from this far (vox, user) — matches AUTO_PICK_R so items and chunks vacuum up at the same range
    absorbMax: 2000, absorbMs: 450, absorbFly: 420,  // absorbMax: the ceiling on what may become a rigid BODY at all — a bigger separated component is dusted instead (see the flood-separate path). NOT an absorb limit; that is absorbSize below.    // absorbMs = the WAIT after breaking off before the chunk comes to you (halved from 900, user 2026-08-02);
    absorbSize: 200,                                 // ── TOO BIG TO CARRY (user) ── a chunk over this many voxels REFUSES to be absorbed: break it down first. Measured on a felled pine, one tree yields chunks of ~7, 12 and 139 voxels plus 800+ voxel trunk sections; 200 lets an armful through and turns the big sections into something you have to work on. Chopping one splits/shrinks it (phChopBody takes a ~30-voxel bite and can sever a long piece in two) until the parts drop under this and vacuum up normally.
    //                                                 absorbFly = the flight itself, left at 420 ms so the transition stays smooth rather than snapping in
    chopBite: 30,                                    // voxels an axe swing takes out — a FIXED count, so every chunk is the same size (user). 40 -> 30, a 25% reduction (user 2026-08-02)
    absorbY: -12,                                     // height the chunk is absorbed at, RELATIVE TO THE EYE — dropped 3 MORE voxels (user, -9 → -12), so it arrives lower still than the waist chest. (Briefly raised to +1 on 2026-08-02, reverted at user request.) One number to retune.                 // a felled pine is far more complex than Teardown's flat-bottomed container
    chopMinBody: 24, chopBodyBite: 4,                // a rigid body this big is choppable, and this many voxels is a real bite out of one (tiny debris is not a target)
    fellOrphans: 40,                                 // orphan voxels that mean a CROWN came loose (not just an axe chip) — the threshold that kills the birds perched in it
    fallSlow: 0.39,                                  // gravity multiplier for a FELLED TRUNK = 1/k^2 with k = 1.6 — the fall plays out ~60% slower (user). Chips/leaves/decor are unaffected.
    tipHoldMs: 380,                                  // after touchdown the top SITS on the base this long before it starts to go over (user: land on the base every time, then tilt)
    tipArmMs: 1600, tipBlockDepth: 2.2,              // a severed top DROPS onto the stump first and only then starts to tip (user). tipArmMs is the fallback if it never lands; tipBlockDepth pauses the drive while it is DEEPLY penetrating something, so it can never rotate THROUGH the stump. Must sit well above the ~1.3 vox a trunk normally rests at, or the guard fires constantly and the tree stalls half-tipped (measured).
    fellSpread: 1.5,                                 // rad of RANDOM spread either side of the notch direction when a trunk is severed (user: trees all dropped the same way)
    crashThrough: 200,                               // "this body is a TRUNK, not a chunk" — gates the topple drive and its speed ceiling (see phStep). It used to also exempt trunks from foliage contacts; leaves have no hitbox for anything now, so that part is gone.
    stuckMs: 2500, stuckNudge: 40,                   // …and if one hangs up on something SOLID, re-arm the topple every stuckMs with a downward shove
    tipSeed: 0.18,                                   // rad/s the topple STARTS at — a barely-perceptible lean, so the fall has somewhere to accelerate from (user: momentum)
    tipRate: 0.34,                                   // …the linear part of the ramp, reached about halfway over (see the topple drive in phStep)
    tipAccel: 1.3,                                   // …rising to tipRate + tipAccel as it goes over, so it accelerates past balance like a real tree (also /k)
    tipDone: 0.34,                                   // stop driving once the trunk's own up-axis has tilted past ~70 deg from vertical (cos 70 deg) — physics takes the landing from there
    tipMax: 1.0,                                     // rad/s ceiling on a felled trunk's rotation, drive or no drive — the top speed, not the average (user)
    tipMaxMs: 15000,   // …and the drive's hard stop stretches with it
    tipDamp: 0.6,                                    // per-SUBSTEP retention of any rotation that is not about the topple axis — solver noise, and the source of the shudder                                  // hard stop: never drive a topple longer than this, whatever it is caught on
    stats: { chops: 0, voxRemoved: 0, separations: 0, floods: 0, floodVox: 0, dustVox: 0,
             retired: 0, evicted: 0, sparks: 0, unstuck: 0, topples: 0, toppleMaxTilt: 0, ccd: 1, chunks: 0, reclaimed: 0, absorbed: 0, dropped: 0, birdsKilled: 0, bodyChops: 0, bodySplits: 0, decorFalls: 0, snowCarried: 0, coneCarried: 0, lastFellDeg: 0, lastBodyChopMs: 0, lastFloodMs: 0, lastSepMs: 0, stepMs: 0, substeps: 0, contacts: 0 },
  };
  const treeShapeAt = (wx, wz) => {                  // which pine covers this world column, and its exact local frame
    const c0x = Math.floor(wx / TCELL), c0z = Math.floor(wz / TCELL);
    const r = Math.ceil(Math.max(MSX, MSY) / TCELL) + 1;   // a crown can overhang several cells
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
      const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
      const R = MROT[tr.rot], bx = tr.tx - (R.sx >> 1), bz = tr.tz - (R.sz >> 1);
      if (wx < bx || wx >= bx + R.sx || wz < bz || wz >= bz + R.sz) continue;
      return { tr, R, bx, bz, gy: groundMin(tr.tx, tr.tz, 2) - tr.sink };
    }
    return null;
  };
  // 4 emissive embers at an impact point — the death poof's spark half, reused for a landed chop (user).
  // sparks3d is declared further down; this only ever runs from a swing, long after that.
  const spawnChopSparks = (wx, wy, wz) => {
    for (let i = 0; i < 4; i++) {
      const a = Math.random() * 6.283, sp = 14 + Math.random() * 22;
      sparks3d[i] = { x: wx, y: wy, z: wz, vx: Math.cos(a) * sp, vy: 12 + Math.random() * 22, vz: Math.sin(a) * sp,
        born: performance.now(), life: 0.4 + Math.random() * 0.25, ph: Math.random() * 6.283, smoke: false };
    }
    PH.stats.sparks++;
  };
  // ── A SLEEPING BODY MUST NOT OUTLIVE ITS SUPPORT (2026-08-08) ── phStep returns immediately for a sleeping
  // body, and NOTHING in the game re-examined one when the world changed: gpuPatch told the support queue and
  // the navfield and never PH.bodies. So a chunk that fell, landed and dozed off stayed exactly where it was
  // when the player carved away whatever it had been resting on — hanging in the air, permanently.
  //
  // That is the floater every audit in this file is blind to, and the reason the reports never matched the
  // measurements: `floatAudit` reads W, `whyFloating` marches W, and a rigid body is NOT IN W — that is the
  // whole point of the off-grid representation. Aimed at one of these, whyFloating answers "the view ray hit
  // nothing solid within 120 voxels", which is exactly what came back from the player's session.
  //
  // Cheap by construction: one call per gpuPatch BATCH (not per cell) against at most PH.maxBodies bodies,
  // and only when that batch actually cleared something. Waking is free — the solver re-tests contacts on the
  // next step and puts it straight back to sleep if it really was supported.
  // The box arrives in WINDOW coordinates (gpuPatch derives them from the flat index) and the bodies live in
  // WORLD space, so the x/z bounds are un-wrapped first — the same arithmetic supWorldX/supWorldZ use, inlined
  // because those are declared with the resolver, further down. y needs no conversion.
  const phWakeNear = (gx0, gx1, y0, y1, gz0, gz1) => {
    if (!PH.bodies.length) return;
    const ox = winOX - gwrap(winOX, WX), oz = winOZ - gwrap(winOZ, WZ);
    const x0 = gx0 + ox, x1 = gx1 + ox, z0 = gz0 + oz, z1 = gz1 + oz;
    let woke = 0;
    for (let i = 0; i < PH.bodies.length; i++) {
      const b = PH.bodies[i];
      if (!b.sleeping) continue;
      const r = b.rMax + 2;                            // the body's own reach, plus a voxel of contact slop
      if (b.pos[0] + r < x0 || b.pos[0] - r > x1) continue;
      if (b.pos[1] + r < y0 || b.pos[1] - r > y1) continue;
      if (b.pos[2] + r < z0 || b.pos[2] - r > z1) continue;
      b.sleeping = false; b.sleepT = 0; woke++;
    }
    if (woke) PH.stats.woken = (PH.stats.woken | 0) + woke;
  };
  phWakeHook = phWakeNear;                             // gpuPatch sits above this block — see the hook's declaration
  const phWorldIdx = (S, mx, my, mz) => gwrap(S.bx + mx, WX) + (S.gy + my) * WX + gwrap(S.bz + mz, WZ) * WX * WY;
  // Kill every perched songbird in this pine (see the block comment above phFlushBirds' first use).
  // Slots 64-243 are the perched birds; kind 5 is the perched-bird kind. Runs ONLY on the swing that
  // actually fells the tree.
  //
  // The bird DIES here rather than being recycled. Clearing init alone sent it back through the normal
  // population loop, which tries findPineCrown and then falls through to the GENERIC open-spot
  // placement when that misses — the "bird teleports to the ground" the user saw. This is the exact
  // triple tryKillCreature uses: the slot is dead for the session, so nothing re-places it anywhere.
  // Birds within `rad` of a single impact — used on EVERY landed chop, since carving foliage can pull
  // a perch out from under a bird without the felling gate ever firing.
  // ── A BIRD NEVER JUST VANISHES (user 2026-08-05: "hitting a bird in the tree, it just disappears instead
  // of going through the death sequence") ── this runs on EVERY landed chop, at the tool's own radius + 8, so
  // a swing that misses the bird and bites the branch beside it used to delete the bird outright: no flash,
  // no poof, nothing. It still dies — a perch cut out from under it is fatal — but it now dies the way
  // everything else does. birdDeath is assigned once the death machinery exists further down; until then the
  // old silent teardown stands in, which only matters during load when nothing is chopping anyway.
  let birdDeath = null;
  const phKillBird = (B) => {
    // ONCE. A ragdolled bird keeps B.init set for the half second its death plays out, and physChopAt runs
    // the near-flush AND (when the crown comes loose) the whole-tree flush in the same swing — so without
    // this the second pass killed the same bird again, which re-ran the teardown on a body it no longer had
    // and tore the death sequence back down to the silent vanish this exists to remove.
    if (B.rag || B.slain) return false;
    if (birdDeath) { birdDeath(B); return true; }
    if (B.sN) unstampWorm(B);                          // needles restored while the crown is still there, so they leave with it
    B.init = false; B.dieT = 0; B.slain = true;
    return true;
  };
  const phFlushBirdsNear = (wx, wy, wz, rad) => {
    let n = 0;
    for (let j = 64; j < 244; j++) {
      const B = wbf[j];
      if (!B || !B.init || (B.kind | 0) !== 5) continue;
      const dx = B.x - wx, dz = B.z - wz, dy = (B.perchFeet || 0) - wy;
      if (dx * dx + dz * dz + dy * dy > rad * rad) continue;
      if (phKillBird(B)) n++;
    }
    PH.stats.birdsKilled += n;
    return n;
  };
  const phFlushBirds = (S) => {
    let n = 0;
    for (let j = 64; j < 244; j++) {
      const B = wbf[j];
      if (!B || !B.init || (B.kind | 0) !== 5) continue;
      const mx = Math.round(B.x) - S.bx, mz = Math.round(B.z) - S.bz;
      if (mx < -2 || mx >= S.R.sx + 2 || mz < -2 || mz >= S.R.sz + 2) continue;   // perched in some other pine
      if (phKillBird(B)) n++;                          // …the same visible death (see phKillBird) — a whole crown coming down should not silently swallow the birds in it either
    }
    PH.stats.birdsKilled += n;
    return n;
  };
  // Returns the WORLD PALETTE ID when this local cell still belongs to the tree, else 0: a pine model voxel
  // not since replaced (by snow, a creature stamp or an earlier chop).
  // ── THE HUNG-PINECONE CLAUSE IS GONE ── it used to claim any PICK_CONE id sitting above sink+12 in a cell
  // where R.A is empty, so that a felled tree carried its cones with it. But PICK_CONE is a SUBSET of the
  // pine's own BARK ids, and trees are placed on a 45-voxel grid with jitter while a footprint is ~35 wide
  // — overlapping footprints are the NORMAL case, and treeShapeAt returns whichever pine it finds first. In
  // the overlap, tree B's bark sits exactly where tree A's R.A is empty, so this clause called it tree A's
  // material, found it unreachable from A's roots, and punched it out of tree B.
  // Hung cones need no special case now: a cone touches needles, the needles touch their branch, the branch
  // is anchored — so the DRAPE flood holds it as ordinary drape, and when the branch goes the cone goes with
  // the same component. That is the whole point of not making the two graphs symmetric.
  const phPresent = (S, mx, my, mz) => {
    const y = S.gy + my; if (y < 1 || y >= WY) return 0;
    const v = S.R.A[mx + mz * S.R.sx + my * S.R.sx * S.R.sz];
    if (!v) return 0;
    const wid = W[phWorldIdx(S, mx, my, mz)];
    return wid === remap[v] ? wid : 0;
  };
  // ── 6-NEIGHBOUR CONNECTIVITY over ONE tree ── seeds are the buried courses (my <= sink): the root
  // anchor. Anything the flood cannot reach from the root is detached. Flat mark buffer sized to the
  // model box, allocated once and reused, so a chop allocates nothing.
  let phMark = null, phStack = null;
  const phFlood = (S) => {
    const sx = S.R.sx, sz = S.R.sz, nAll = sx * sz * MSZ;
    if (!phMark || phMark.length < nAll) { phMark = new Uint8Array(nAll); phStack = new Int32Array(nAll); }
    else phMark.fill(0, 0, nAll);
    const li = (mx, my, mz) => mx + mz * sx + my * sx * sz;
    let sp = 0, reached = 0, total = 0;
    const t0 = performance.now();
    for (let my = 0; my < MSZ; my++) for (let mz = 0; mz < sz; mz++) for (let mx = 0; mx < sx; mx++)
      if (phPresent(S, mx, my, mz)) { total++;
        if (my <= S.tr.sink) { const k = li(mx, my, mz); if (!phMark[k]) { phMark[k] = 1; phStack[sp++] = k; } } }
    while (sp > 0) {
      const k = phStack[--sp]; reached++;
      const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      for (let d = 0; d < 6; d++) {
        const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
        const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
        const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
        const nk = li(nx, ny, nz);
        if (phMark[nk] || !phPresent(S, nx, ny, nz)) continue;
        phMark[nk] = 1; phStack[sp++] = nk;
      }
    }
    PH.stats.floods++; PH.stats.floodVox += total;
    PH.stats.lastFloodMs = +(performance.now() - t0).toFixed(2);
    return { total, reached, orphans: total - reached, sx, sz, li };
  };
  const phComponent = (S, f, start) => {             // one connected component of the ORPHANED cells
    const { sx, sz, li } = f;
    const cells = [];
    let sp = 0; phStack[sp++] = start; phMark[start] = 2;
    while (sp > 0) {
      const k = phStack[--sp]; cells.push(k);
      const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      for (let d = 0; d < 6; d++) {
        const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
        const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
        const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
        const nk = li(nx, ny, nz);
        if (phMark[nk] || !phPresent(S, nx, ny, nz)) continue;
        phMark[nk] = 2; phStack[sp++] = nk;
      }
    }
    return cells;
  };
  // ── BODY BUILD ── mass / COM / inertia straight from the voxels, plus Teardown's contact
  // classification: count filled 6-neighbours per voxel — 6 = interior (can never touch anything, never
  // probed), 5 = face, 4 = edge, <=3 = corner. Probes are taken CORNERS FIRST, which is what collapses a
  // flat-bottomed body from hundreds of candidate contacts to a handful.
  const phBuildBody = (S, cells, f, idMap) => {
    const { sx, sz } = f, N = cells.length;
    const lx = new Int16Array(N), ly = new Int16Array(N), lz = new Int16Array(N), id = new Uint8Array(N);
    let cxs = 0, cys = 0, czs = 0;
    const inComp = new Set(cells);
    for (let i = 0; i < N; i++) {
      const k = cells[i], mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      lx[i] = mx; ly[i] = my; lz[i] = mz;
      id[i] = idMap ? idMap.get(k) : W[phWorldIdx(S, mx, my, mz)];   // LIVE voxel (covers pinecones, absent from R.A) unless the caller already carved it out
      cxs += mx + 0.5; cys += my + 0.5; czs += mz + 0.5;
    }
    const mass = N, com = [cxs / N, cys / N, czs / N];
    let Ixx = 0, Iyy = 0, Izz = 0;
    const cube = 1 / 6;                              // a solid 1x1x1 voxel about its own centre
    for (let i = 0; i < N; i++) {
      const rx = lx[i] + 0.5 - com[0], ry = ly[i] + 0.5 - com[1], rz = lz[i] + 0.5 - com[2];
      Ixx += ry * ry + rz * rz + 2 * cube; Iyy += rx * rx + rz * rz + 2 * cube; Izz += rx * rx + ry * ry + 2 * cube;
    }
    let rMax = 0;                                    // farthest voxel from the COM — sets the tip speed, which sets how finely this body must be stepped
    for (let i = 0; i < N; i++) {
      const rx = lx[i] + 0.5 - com[0], ry = ly[i] + 0.5 - com[1], rz = lz[i] + 0.5 - com[2];
      const d2 = rx * rx + ry * ry + rz * rz; if (d2 > rMax) rMax = d2;
    }
    rMax = Math.sqrt(rMax);
    const b = { n: N, mass, com, id, lx, ly, lz, rMax, I: [Ixx, Iyy, Izz],
      src: phSrc,                                  // …readable via __vb.bodyMats()
      pos: [S.bx + com[0], S.gy + com[1], S.bz + com[2]],   // world position OF THE COM
      origin: [S.bx, S.gy, S.bz],                    // where the local frame sat in W — physValidate proves no duplicate remains
      vel: [0, 0, 0], omega: [0, 0, 0], q: [0, 0, 0, 1],
      sleeping: false, sleepT: 0, born: performance.now(), sx, sz,
      ax: [1, 0, 0], ay: [0, 1, 0], az: [0, 0, 1] };   // cached world axes — refreshed whenever the body moves, read by phBodySolid
    const rank = [], rankFol = [];                   // 0 = corner (best probe) … 3 = interior (never); rankFol = the foliage cells held back in case the body turns out to be nothing else
    for (let i = 0; i < N; i++) {
      const mx = lx[i], my = ly[i], mz = lz[i];
      let filled = 0;
      for (let d = 0; d < 6; d++) {
        const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
        const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
        const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
        if (inComp.has(nx + nz * sx + ny * sx * sz)) filled++;
      }
      if (filled >= 6) continue;                     // INTERIOR — skipped entirely
      if (foliaTab[id[i]]) { rankFol.push([filled, i]); continue; }   // LEAVES HAVE NO HITBOX (user): needles never generate a contact, so a crown clips into the ground instead of standing the trunk up on it
      rank.push([filled, i]);
    }
    if (!rank.length) { for (const r of rankFol) rank.push(r); }   // …unless the body is ALL leaves (a chunk chopped out of pure canopy), which would otherwise have no contacts at all and fall through the world
    rank.sort((p, q) => p[0] - q[0]);                // corners first
    b.surfN = rank.length;
    // SPATIAL STRATIFICATION. Corner-first alone samples only the extremities — for a felled trunk that
    // is the crown tips, and the flat CUT FACE got no probes at all, so the body sank straight through
    // the stump it should have been resting on. Bucket the surface into 8-voxel cells, take the
    // best-ranked (most corner-like) voxel from each, and only then fill any remaining slots with the
    // next-best overall. Keeps Teardown's corner preference AND guarantees every face is represented.
    const seenB = new Set(), pr = [];
    for (let i = 0; i < rank.length && pr.length < PH.maxProbes; i++) {
      const idx = rank[i][1];
      const bkey = (lx[idx] >> 3) | ((ly[idx] >> 3) << 8) | ((lz[idx] >> 3) << 16);
      if (seenB.has(bkey)) continue;
      seenB.add(bkey); pr.push(idx);
    }
    for (let i = 0; i < rank.length && pr.length < PH.maxProbes; i++) {
      const idx = rank[i][1];
      if (pr.indexOf(idx) < 0) pr.push(idx);
    }
    b.probes = new Int32Array(pr);
    // ── GPU SHAPE ── a TIGHT bbox around the component (a felled crown is a fraction of the full model
    // box) uploaded as one dense id grid. The renderer DDAs this; W never sees it.
    let x0 = 1e9, y0 = 1e9, z0 = 1e9, x1 = -1e9, y1 = -1e9, z1 = -1e9;
    for (let i = 0; i < N; i++) {
      if (lx[i] < x0) x0 = lx[i]; if (lx[i] > x1) x1 = lx[i];
      if (ly[i] < y0) y0 = ly[i]; if (ly[i] > y1) y1 = ly[i];
      if (lz[i] < z0) z0 = lz[i]; if (lz[i] > z1) z1 = lz[i];
    }
    const bw = x1 - x0 + 1, bh = y1 - y0 + 1, bd = z1 - z0 + 1, cells2 = bw * bh * bd;
    phReclaim(cells2);                               // make room by retiring the oldest fallen debris (see phReclaim)
    if (bodyTop + cells2 > BODYCAP) { b.gpu = null; PH.stats.noGpu = (PH.stats.noGpu | 0) + 1; return b; }   // still no room (one body larger than the whole buffer) — simulates but is NOT DRAWN, which on screen is indistinguishable from the chunk vanishing. Counted so that case can be told apart from a body that was genuinely lost.
    const grid = new Uint32Array(cells2);
    for (let i = 0; i < N; i++) grid[(lx[i] - x0) + (ly[i] - y0) * bw + (lz[i] - z0) * bw * bh] = id[i];
    device.queue.writeBuffer(bodyBuf, bodyTop * 4, grid.buffer, 0, cells2 * 4);
    b.cpuGrid = grid;                              // CPU copy so player collision can query the body (see phBodySolid)
    b.gpu = { off: bodyTop, bw, bh, bd, cells: cells2, comL: [com[0] - x0, com[1] - y0, com[2] - z0] };
    bodyTop += cells2;
    phAabb(b);                                     // a body queried before its first physStep still needs a valid broad-phase box
    if (b.ab) { const A = PH.abAll; for (let k = 0; k < 3; k++) { if (b.ab[k] < A[k]) A[k] = b.ab[k]; if (b.ab[k + 3] > A[k + 3]) A[k + 3] = b.ab[k + 3]; } }
    return b;
  };
  // Evict the OLDEST bodies until `need` cells fit, compacting the shape buffer as we go. The user's
  // call: old fallen debris may disappear to make room for new. Compaction re-uploads the survivors from
  // their CPU grids, which is why every drawn body keeps one.
  // Free ONE body slot by dropping the oldest. Chunk spawning used to just `continue` when the slot
  // budget was full, so once a few felled trunks (which never retire) filled the list, the axe stopped
  // producing chunks entirely — the "chunks stop working after a couple of trees" report. Old fallen
  // debris giving way to new is the user's stated preference.
