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
    acc: 0, bodies: [], maxBodies: PHYS_MAX,   // 24 (user 2026-08-11, was 16) — MUST come from the uniform capacity: physB has room for exactly PHYS_MAX bodies and the emit loop clips to it, so a larger sim cap would simply never be drawn
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
    treeLifeMs: 600000, chunkLifeMs: 600000,         // ── LIFETIMES (user 2026-08-11) ── EVERYTHING the player made unstatic is deleted 10 min after it broke loose — a felled trunk and a chop chunk alike (was 5 min / 60 s). Two knobs so the trunk can still be retuned apart from the debris, but they are deliberately equal now.
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
             retired: 0, evicted: 0, sparks: 0, unstuck: 0, topples: 0, toppleMaxTilt: 0, ccd: 1, chunks: 0, reclaimed: 0, absorbed: 0, dropped: 0, birdsKilled: 0, bodyChops: 0, bodySplits: 0, decorFalls: 0, snowCarried: 0, coneCarried: 0, lastFellDeg: 0, lastBodyChopMs: 0, lastFloodMs: 0, lastSepMs: 0, stepMs: 0, substeps: 0, contacts: 0, oakShapes: 0 },   // oakShapes = (model, rotation) oak shapes built this session; each is a 10-15 ms build, and a number that keeps climbing means the 3-entry LRU is thrashing
  };
  // ── THE OAKS FALL TOO (user 2026-08-17: "make the oak trees fall with the axe") ─────────────────────
  // Everything below treeShapeAt — the carve, the root flood, the separate, the topple — is written
  // against ONE interface: a shape S that answers "is local cell (mx,my,mz) still this tree's material,
  // and where does it sit in W". A pine satisfies it with MROT, a pre-rotated DENSE array palette.js builds
  // once at load. An oak cannot: OAKV is a SPARSE list of packed voxels per model, there are seven models at
  // four rotations, and the biggest is 86,365 voxels in a 114x112x114 box. Walking that list per query is out
  // of the question, and pre-rotating all 28 up front is 20+ MB of shapes almost none of which will ever be
  // chopped.
  // So the oak's shape is built ON DEMAND and cached: the same dense array a pine gets, minted the first time
  // the axe asks about that (model, rotation) and held in a 3-entry LRU. A build is ~86k scattered writes plus
  // one 6-connected labelling pass — call it 10-15 ms, once, on the first swing at a size of tree this session
  // has not chopped yet; every swing after that is a plain array read exactly as a pine's is.
  const OAK_SHP_MAX = 3;                               // cached (model, rotation) shapes — up to 3 MB each at the giant end. Sized for how the axe is used (one tree at a time, its neighbours the same size), not for how many shapes exist.
  const OAKSHP = new Map();                            // key = k*4 + rot -> the dense rotated shape. Map keeps insertion order, which is what makes the LRU a delete + re-set.
  const OAKID = (() => { const a = new Uint8Array(256); for (let i = 0; i < 256; i++) a[i] = i; return a; })();   // the oak's `remap`: bow.js already resolved OAKV's palette to WORLD ids (stampModel writes `p >>> 24` raw where stampTree writes remap[v]), so the model id IS the world id and phPresent's table is the identity
  const phNb26 = (() => { const a = []; for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) if (dx || dy || dz) a.push(dx, dy, dz); return a; })();   // 26 (dx,dy,dz) triples, flat — see the glue pass in oakShape and phComponent
  let OAK_MAXD = -1;                                   // widest model footprint in voxels; -1 until first asked
  const oakRad = () => {                               // …as a radius in OKCELLs, the oak twin of treeShapeAt's own `r`
    if (OAK_MAXD < 0) { let d = 0; for (const m of OAKV) d = Math.max(d, m.sx, m.sy); OAK_MAXD = d; }
    return Math.ceil(OAK_MAXD / OKCELL) + 1;
  };
  // ── ONE OAK'S SHAPE, ROTATED AND DENSE ── the rotation is stampModel's, copied literally rather than
  // re-derived, because a shape that disagrees with the stamp by one voxel makes phPresent answer 0 for the
  // whole tree and the axe finds nothing at all.
  //
  // ── AND THE GLUE, WHICH IS THE HALF THAT IS NOT OBVIOUS ── a pine is 8,440 voxels in ONE 6-connected piece
  // plus 31 stray specks, so the root flood can be 6-connected and nobody notices. An oak baked from a .glb
  // is not: MEASURED over the seven models, 6-connected they come apart into 48 / 140 / 73 / 454 / 1468 / 526
  // / 1556 pieces, and for the giant that is 233 clusters of 2+ voxels and 1,323 lone specks attached to the
  // main body by nothing but a DIAGONAL. Run the pine's 6-connected flood over that and every one of them is
  // an orphan on the FIRST swing: 232 rigid bodies fighting over 24 slots and 1,323 voxels erased as litter,
  // before the tree has even been notched. 26-connected, every model is exactly ONE piece — the same fact
  // sim/support-rules.js records for its drape cap, off the same asset.
  // The honest answer is a 26-connected flood and it costs 4.3x the neighbour probes (2.25M against 518k on
  // the giant) on EVERY swing. So connectivity is decided once, here, instead: label the 6-components, find
  // the main one, and flag both ends of every 26-link that joins a stray to something else. The flood then
  // walks 6-connected — the pine's own loop, untouched — and expands to 26 only at the flagged cells, which
  // MEASURED are 427 / 1287 / 508 / 3928 / 9769 / 4552 / 12505 of the seven models. On the giant that is 14%
  // of the pops, so 844k probes against 2.25M: same reachability on an intact tree at 37% of the cost.
  // A cut cannot resurrect a glue link the model did not have, and the trunk and the crown are both inside
  // the MAIN component, so no glue link spans the notch: sever the bole and the crown still comes away.
  const oakShape = (k, rot) => {
    const key = k * 4 + rot;
    const hit = OAKSHP.get(key);
    if (hit) { OAKSHP.delete(key); OAKSHP.set(key, hit); return hit; }   // LRU touch — re-inserting moves it to the young end
    const m = OAKV[k];
    const sx = (rot & 1) ? m.sy : m.sx, sz = (rot & 1) ? m.sx : m.sy, h = m.sz;
    const A = new Uint8Array(sx * sz * h);
    const cells = new Int32Array(m.vox.length);        // the SPARSE index list — what makes the flood's seed pass O(voxels) instead of O(box), which at 114x112x116 is a 17x difference
    let n = 0;
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i], x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
      let rx, rz;                                      // …stampModel's rotation, verbatim
      if (rot === 0) { rx = x; rz = y; }
      else if (rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      const li = rx + rz * sx + z * sx * sz;
      if (!A[li]) cells[n++] = li;
      A[li] = p >>> 24;
    }
    const lab = new Int32Array(A.length);              // 6-component label per cell, 0 = none. Transient: it exists only to find the strays.
    const st = new Int32Array(n);
    let nc = 0, best = 0, bestN = 0;
    for (let i = 0; i < n; i++) {
      const s0 = cells[i]; if (lab[s0]) continue;
      const id2 = ++nc; let sp = 0, cnt = 0;
      lab[s0] = id2; st[sp++] = s0;
      while (sp > 0) {
        const c = st[--sp]; cnt++;
        const cx = c % sx, cz = ((c / sx) | 0) % sz, cy = (c / (sx * sz)) | 0;
        for (let d = 0; d < 6; d++) {
          const nx = cx + (d === 0 ? 1 : d === 1 ? -1 : 0);
          const ny = cy + (d === 2 ? 1 : d === 3 ? -1 : 0);
          const nz = cz + (d === 4 ? 1 : d === 5 ? -1 : 0);
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= h) continue;
          const nk = nx + nz * sx + ny * sx * sz;
          if (!A[nk] || lab[nk]) continue;
          lab[nk] = id2; st[sp++] = nk;
        }
      }
      if (cnt > bestN) { bestN = cnt; best = id2; }
    }
    let g = null;
    if (bestN < n) {                                   // there ARE strays — and only they and their partners need the 26 treatment, so this scan is over ~1,500 cells, not 86,000
      g = new Uint8Array(A.length);
      for (let i = 0; i < n; i++) {
        const c = cells[i]; if (lab[c] === best) continue;
        const cx = c % sx, cz = ((c / sx) | 0) % sz, cy = (c / (sx * sz)) | 0;
        for (let d = 0; d < 78; d += 3) {
          const nx = cx + phNb26[d], ny = cy + phNb26[d + 1], nz = cz + phNb26[d + 2];
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= h) continue;
          const nk = nx + nz * sx + ny * sx * sz;
          if (!A[nk] || lab[nk] === lab[c]) continue;  // the same 6-piece: the flood already crosses this edge on its own
          g[c] = 1; g[nk] = 1;                         // BOTH ends, because the flood can arrive from either side
        }
      }
    }
    // ── WHERE THE BOLE STANDS IN THIS SHAPE'S OWN FRAME ── the anchor band is measured at the TRUNK, and the
    // trunk is not at the middle of the model box: MEASURED, the bark centroid of the bottom course is off
    // the bbox centre by (-5.2, +6.0) on the 7 m oak and (+0.8, -2.9) on the 11.7 m one, because the box is
    // sized by the CROWN. Reading the ground at the planting column instead would be a metre out on the
    // models that lean, which on bumpy forest floor is the difference between an anchored tree and one that
    // orphans itself whole the first time it is touched. Rotated with everything else, so it is the bole's
    // real footprint whichever way the tree was placed.
    let wz0 = 255;
    for (let i = 0; i < m.vox.length; i++) { const p = m.vox[i]; if (!woodTab[p >>> 24]) continue; const z = (p >> 16) & 255; if (z < wz0) wz0 = z; }
    let tx0 = 1e9, tx1 = -1e9, tz0 = 1e9, tz1 = -1e9;
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i], z = (p >> 16) & 255;
      if (z > wz0 + 2 || !woodTab[p >>> 24]) continue;   // the lowest three courses of BARK — the bole where it meets the ground
      const x = p & 255, y = (p >> 8) & 255;
      let rx, rz;
      if (rot === 0) { rx = x; rz = y; }
      else if (rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      if (rx < tx0) tx0 = rx; if (rx > tx1) tx1 = rx;
      if (rz < tz0) tz0 = rz; if (rz > tz1) tz1 = rz;
    }
    if (tx1 < tx0) { tx0 = 0; tx1 = sx - 1; tz0 = 0; tz1 = sz - 1; }   // a model with no bark at all: fall back to the whole box rather than an empty scan
    const shp = { A, sx, sz, h, g, cells: n === cells.length ? cells : cells.subarray(0, n), tx0, tx1, tz0, tz1 };
    OAKSHP.set(key, shp);
    while (OAKSHP.size > OAK_SHP_MAX) OAKSHP.delete(OAKSHP.keys().next().value);   // oldest out
    PH.stats.oakShapes = (PH.stats.oakShapes | 0) + 1;
    return shp;
  };
  // ── HOW HIGH THE ROOT REACHES ── a pine goes into W through stampTree, which OVERWRITES: its sink 5-8
  // courses are real bark buried in the hill, and `my <= sink` is both the flood's anchor and the carve's
  // "never cut the roots out from under it" guard. stampOak stamps in MODE 1, which REFUSES every cell that
  // already holds terrain, so an oak has NO buried courses at all — `my <= sink` would seed a band that is
  // not there, the flood would find no anchor, and the tree would orphan itself whole on the first tap.
  //
  // The oak's anchor is therefore measured rather than assumed: walk up the bole's own footprint until the
  // first course of bark that actually made it into W, and take that plus OAK_ROOT. Measured, because how far
  // the ground rises across a trunk is exactly what the constant cannot know — stampOak seats on groundMin
  // over radius 4, so on bumpy floor the first standing course is several above the nominal seat, and it
  // moves with the terrain the player has dug as well.
  // The band is also the STUMP, since physChopAt refuses to cut it: a felled oak leaves ~30 cm of trunk,
  // which chopSwing's orphaned-wood path then takes down like any other stump.
  // ── AND ONLY BARK ANCHORS (see the seed pass in phFlood) ── the band is 3 courses, and on ground that rises
  // under the trunk it can reach course 8, which is exactly where the lowest leaves of the 11.7 m oak hang.
  // Let one of those be an anchor and the crown holds ITSELF up through its own skirt: notch the bole clean
  // through and the tree still stands. A leaf never held a tree up.
  const OAK_ROOT = 2;
  const oakShapeAt = (wx, wz) => {                     // which oak covers this world column — the exact mirror of the pine loop below
    if (!OAKV.length) return null;
    const r = oakRad();
    const c0x = Math.floor(wx / OKCELL), c0z = Math.floor(wz / OKCELL);
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
      const t = oakAt(c0x + dx, c0z + dz); if (!t) continue;
      const m = OAKV[t.k];
      const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;   // …anchored bottom-centre, stampModel's own arithmetic
      const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
      if (wx < bx || wx >= bx + fw || wz < bz || wz >= bz + fd) continue;
      const R = oakShape(t.k, t.rot);
      const S = { tr: t, R, bx, bz, gy: groundMin(t.wx, t.wz, 4) - t.sink,   // gy = stampOak's own seat, to the voxel
                  rm: OAKID, g: R.g, cells: R.cells, root: 0, oak: 1 };
      let lo = R.h;                                    // the lowest course of the bole still standing in W — bounded by the bole's own footprint, so ~60 probes and the inner loop shrinks as soon as one column answers
      for (let mz = R.tz0; mz <= R.tz1; mz++) for (let mx = R.tx0; mx <= R.tx1; mx++)
        for (let my = 0; my < lo; my++) { const v = phPresent(S, mx, my, mz); if (v && woodTab[v]) { lo = my; break; } }
      S.root = lo >= R.h ? -1 : lo + OAK_ROOT;         // -1 = no bark left at the base at all, i.e. nothing is holding this tree up any more: no seeds, and whatever still stands comes down on the next touch
      return S;
    }
    return null;
  };
  const treeShapeAt = (wx, wz) => {                  // which pine covers this world column, and its exact local frame
    const c0x = Math.floor(wx / TCELL), c0z = Math.floor(wz / TCELL);
    const r = Math.ceil(Math.max(MSX, MSY) / TCELL) + 1;   // a crown can overhang several cells
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
      const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
      const R = MROT[tr.rot], bx = tr.tx - (R.sx >> 1), bz = tr.tz - (R.sz >> 1);
      if (wx < bx || wx >= bx + R.sx || wz < bz || wz >= bz + R.sz) continue;
      return { tr, R, bx, bz, gy: groundMin(tr.tx, tr.tz, 2) - tr.sink, rm: remap, g: null, cells: null,
               root: tr.sink, oak: 0 };   // …the same key set in the same order as the oak's, so both kinds of shape share one hidden class at every site that reads them
    }
    // ── AND OTHERWISE, AN OAK ── extending this rather than adding a second entry point is deliberate: every
    // caller of treeShapeAt (chopSwing, the arrow's carve in sim/projectiles.js, __vb.physChopFull) is asking
    // "which standing tree owns this column", and none of them wants to ask it twice. The two scans can never
    // both answer, because the biome masks are exclusive — treeAt refuses oakM > 0.5 and oakAt refuses
    // oakM < 0.5 — so in the pine forest this costs one bounded scan over empty cells.
    return oakShapeAt(wx, wz);
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
  // WORLD space, so the x/z bounds are un-wrapped through supWorldX/supWorldZ themselves. y needs no conversion.
  // ── IT USED TO INLINE THEM AND GET IT WRONG ── the inlined form was `gx + (winOX - gwrap(winOX, WX))`, which
  // drops the modulo and so only agrees with supWorldX for columns at or right of the wrap point. With WX 768
  // and winOX 800 the wrap point is 32, so window column 10 is world column 1546 and this answered 778: outside
  // the window entirely, matching no body. The player stands at window column (wrap + HALF) mod WX, which is in
  // the broken half whenever the wrap point is past the middle — so digging under your own feet woke nothing,
  // and since this is the game's ONLY world-change wake path, a settled chunk whose support you removed hung
  // there for good. Waking a body that did not need it is free (the solver re-tests contacts and puts it
  // straight back to sleep); failing to wake one is permanent, so the seam case widens rather than guesses.
  const phWakeNear = (gx0, gx1, y0, y1, gz0, gz1) => {
    if (!PH.bodies.length) return;
    let x0 = supWorldX(gx0), x1 = supWorldX(gx1), z0 = supWorldZ(gz0), z1 = supWorldZ(gz1);
    if (x1 < x0) { x0 = winOX; x1 = winOX + WX - 1; }   // the batch straddles the window's wrap seam: in world space that is TWO intervals, so take the whole span rather than the empty one between them
    if (z1 < z0) { z0 = winOZ; z1 = winOZ + WZ - 1; }
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
  // CARD_0..CARD_END is the perched-bird band; kind 5 is the perched-bird kind. Runs ONLY on the swing that
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
    for (let j = CARD_0; j < CARD_END; j++) {
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
    for (let j = CARD_0; j < CARD_END; j++) {
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
  // ── S.rm IS THE MODEL-ID -> WORLD-ID TABLE ── `remap` for a pine, the identity for an oak, because
  // stampTree writes remap[v] and stampModel writes the packed id straight through. One property read; the
  // pine's answer is the same byte it always was.
  // ── AND AN OAK ACCEPTS ANY LEAF WHERE ITS OWN LEAF SHOULD BE ── OKCELL is 112 and the widest crown is 114,
  // so oak footprints OVERLAP, and stampModel mode 1 lets the later tree's leaves overwrite the earlier
  // tree's (leaf ids are minted past DECOR_MIN, i.e. soft decor; bark is not and is never overwritten). Held
  // to an exact id match, the earlier oak reads a hole through its own crown wherever a neighbour's canopy
  // crosses it — and a hole in a 6-connected crown is a spurious ORPHAN, i.e. clumps of leaves falling off a
  // tree on the first tap. Whichever oak is felled first takes the shared leaves with it and leaves a small
  // gap in the other's canopy, which is cosmetic and costs nothing: leaves have no hitbox and the drape
  // resolver re-adjudicates whatever was hanging on them. BARK stays an exact match, so a fell can never
  // punch the neighbouring tree's TRUNK out — the failure the pinecone clause above was deleted for.
  const phPresent = (S, mx, my, mz) => {
    const y = S.gy + my; if (y < 1 || y >= WY) return 0;
    const v = S.R.A[mx + mz * S.R.sx + my * S.R.sx * S.R.sz];
    if (!v) return 0;
    const wid = W[phWorldIdx(S, mx, my, mz)];
    if (wid === S.rm[v]) return wid;
    return (S.oak && foliaTab[v] && foliaTab[wid]) ? wid : 0;   // an overlapping oak's canopy standing in for our own
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
    const root = S.root, g = S.g;                     // root: the anchored courses (see OAK_ROOT). g: the 26-glue flags, null for a pine.
    // ── THE SEED PASS WALKS THE MODEL, NOT THE BOX, WHEN IT CAN ── a pine's box is 35x36x116 = 146k cells and
    // scanning all of it costs nothing. An oak's is 114x112x116 = 1.48M for 86k voxels, so the same scan is
    // 17x the work for the same answer. S.cells is the shape's own sparse index list; the pine has none and
    // keeps its original triple loop, byte for byte.
    if (S.cells) {                                    // …the oak arm: sparse, and BARK ONLY in the anchor band (see OAK_ROOT)
      const C = S.cells;
      for (let i = 0; i < C.length; i++) {
        const k = C[i];
        const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
        const v = phPresent(S, mx, my, mz);
        if (!v) continue;
        total++;
        if (my <= root && woodTab[v] && !phMark[k]) { phMark[k] = 1; phStack[sp++] = k; }
      }
    } else {
      for (let my = 0; my < MSZ; my++) for (let mz = 0; mz < sz; mz++) for (let mx = 0; mx < sx; mx++)
        if (phPresent(S, mx, my, mz)) { total++;
          if (my <= root) { const k = li(mx, my, mz); if (!phMark[k]) { phMark[k] = 1; phStack[sp++] = k; } } }
    }
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
      if (g && g[k]) {                                // ── THE DIAGONAL LINKS ── 12,505 flagged cells of 86,365 on the giant, so this runs on 14% of the pops and buys 26-connected reachability at 37% of a 26-connected flood (see oakShape)
        for (let d = 0; d < 78; d += 3) {
          const nx = mx + phNb26[d], ny = my + phNb26[d + 1], nz = mz + phNb26[d + 2];
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
          const nk = li(nx, ny, nz);
          if (phMark[nk] || !phPresent(S, nx, ny, nz)) continue;
          phMark[nk] = 1; phStack[sp++] = nk;
        }
      }
    }
    PH.stats.floods++; PH.stats.floodVox += total;
    PH.stats.lastFloodMs = +(performance.now() - t0).toFixed(2);
    return { total, reached, orphans: total - reached, sx, sz, li };
  };
  // ── …AND AN OAK'S ORPHANS ARE GATHERED 26-CONNECTED ── the flood decides WHAT came loose and this decides
  // how it is PARCELLED UP, and for an oak the two want different neighbourhoods. 6-connected, a severed oak
  // crown is not one piece: it is the main shell plus the 233 diagonal clusters oakShape glued back on, and
  // phSeparate would ask for a rigid-body slot for each of them. 26-connected it is one body, which is what a
  // falling tree is. Only ever reached when something really did detach, so the 4.3x neighbour cost is paid
  // once per felling rather than once per swing. The pine keeps its own loop verbatim.
  const phComponent = (S, f, start) => {             // one connected component of the ORPHANED cells
    const { sx, sz, li } = f;
    const cells = [];
    const c26 = !!S.oak;
    let sp = 0; phStack[sp++] = start; phMark[start] = 2;
    while (sp > 0) {
      const k = phStack[--sp]; cells.push(k);
      const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      if (c26) {
        for (let d = 0; d < 78; d += 3) {
          const nx = mx + phNb26[d], ny = my + phNb26[d + 1], nz = mz + phNb26[d + 2];
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= MSZ) continue;
          const nk = li(nx, ny, nz);
          if (phMark[nk] || !phPresent(S, nx, ny, nz)) continue;
          phMark[nk] = 2; phStack[sp++] = nk;
        }
        continue;
      }
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
      c26: S.oak ? 1 : 0,                            // ── THIS BODY'S OWN CONNECTIVITY ── an oak is one piece only 26-connected (see oakShape), so chopping a FELLED oak has to re-split it the same way or a single swing shatters the crown into 200 clumps. 0 for everything else, including the {bx,gy,bz} pseudo-shapes phSubBody and phBodyFromCells pass in; phChopBody carries it across to the pieces it makes.
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
    const seenB = new Set(), pr = [], inPr = new Set();
    for (let i = 0; i < rank.length && pr.length < PH.maxProbes; i++) {
      const idx = rank[i][1];
      const bkey = (lx[idx] >> 3) | ((ly[idx] >> 3) << 8) | ((lz[idx] >> 3) << 16);
      if (seenB.has(bkey)) continue;
      seenB.add(bkey); pr.push(idx); inPr.add(idx);
    }
    for (let i = 0; i < rank.length && pr.length < PH.maxProbes; i++) {
      const idx = rank[i][1];
      if (!inPr.has(idx)) { pr.push(idx); inPr.add(idx); }   // was pr.indexOf(idx) — the same answer (the bucket pass only ever pushes distinct indices) at O(1). On a pine's ~2k surface cells the linear scan is invisible; a felled oak has 8,852 bark voxels against a 512-probe budget, which is 4.5M comparisons on the one swing that already costs the most.
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
