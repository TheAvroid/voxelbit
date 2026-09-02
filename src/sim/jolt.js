  // @module - the Jolt Physics backend: voxel bodies and voxel terrain expressed as Jolt shapes
  // @exports joltActive, joltStepBodies, joltOn, joltStats, joltInvalidate, joltCCD
  // ── WHY THIS EXISTS AND WHAT IT DOES *NOT* TOUCH (user 2026-09-01: "rewrite the physics system using
  // JoltPhysics.js … keep the chunking mechanic we have") ── the scope is RIGID BODIES ONLY. Jolt replaces
  // the integration and contact half of the simulation — phStep in sim/solver.js — and nothing else:
  //   · sim/support.js + support-rules.js decide WHAT detaches. Untouched. That is the chunking mechanic.
  //   · sim/chop.js / chop-tree.js carve the world and sever a trunk. Untouched.
  //   · phBuildBody in sim/physics.js turns a voxel cluster into a body. Untouched — it is still what makes
  //     a pick-chunk out of rock and what shatters a felled tree into pieces.
  //   · the renderer is untouched, and that is the load-bearing fact: bodies are TRACE-INJECTED into the
  //     WGSL tracer as voxel grids with a rigid transform (u.physB = pos + quat + local dims), so a body
  //     must keep its voxel arrays and only needs pos/q written each step. Jolt supplies exactly that.
  //   · the player, creatures, ragdolls and projectiles stay on their own code.
  // So a Jolt body here is a SHADOW of a PH body: same mass, same COM, same inertia, driven by Jolt and
  // read back into b.pos / b.q / b.vel / b.omega. PH.bodies remains the single source of truth for every
  // other system that reads it (nav, snow, floaters, tick-emit, the debug API).
  //
  // ── THE SHAPE PROBLEM, WHICH IS THE WHOLE DIFFICULTY ── a body is an arbitrary CONCAVE voxel blob; a
  // felled oak crown is ~15.6k voxels. A convex hull is the wrong shape for it (it would fill the gaps
  // between branches), and one Jolt box per voxel is 15.6k sub-shapes. So the blob is GREEDY-BOXED first:
  // runs along x, extended in z, then extended in y, largest-box-first. That is the same transform a
  // greedy mesher does, applied to solids instead of faces, and it collapses a blob by 20-60x.
  //
  // ── AND THE TERRAIN IS THE SAME PROBLEM AT WORLD SCALE ── Jolt needs static colliders for ground that is
  // a streaming 2048-wide voxel window. It gets them lazily, per 16-voxel TILE, only where an awake body
  // actually is, and they are dropped again when no body is near. A tile is greedy-boxed exactly like a
  // body. Tiles are keyed on world coordinates and invalidated by gpuPatch, so digging changes collision.
  const JOLT_URL = 'assets/lib/jolt-physics.wasm.js';   // vendored, jolt-physics 1.1.0 — see game/assets/lib
  const J_LAYER_STATIC = 0, J_LAYER_MOVING = 1;
  // ── TILE SIZE IS A BROADPHASE DECISION, NOT A SCAN DECISION ── every tile is its own STATIC BODY, so
  // the count lands in Jolt's broadphase and in the add/remove traffic against it, and that is where the
  // step time went: 3638 tiles for one fell, with the step at 70.7% of wall while tile BUILDING was only
  // 18.4 ms of it. 32 is 8x fewer bodies for the same ground; the extra voxels per tile cost nothing now
  // that hmap bounds the scan (see joltBuildTile), and a compound shape is a tree, so more boxes inside one
  // tile is cheaper than the same boxes spread over eight bodies.
  const JT = 32;                                     // terrain collider tile, in voxels
  // …and NO 3D SHELL OF MARGIN. A body collides with what its own box overlaps; the ground it is falling
  // toward is joltEnsureUnder's job and that reaches DOWN specifically. A 2-tile shell in every direction
  // asked for 6^3 tiles per body and multiplied by every body in a fell.
  const JT_PAD = 0;                                  // tiles of margin kept around each awake body
  const J_MAXBOX = 3072;                             // sub-shape ceiling for one body — past this the blob is boxed at 2x scale (see joltBoxes)
  const J_TILE_CAP = 4096;                           // live terrain colliders, LRU-evicted — see the eviction note in joltStepBodies
  let jFrame = 0;
  const J_TILE_BUDGET = 12;                          // tiles built per frame, so a teleport into fresh ground cannot stall a frame
  let JL = null;                                     // the Jolt module once initialised
  let jIface = null, jSys = null, jBI = null;        // JoltInterface / PhysicsSystem / BodyInterface
  let jReady = false, jBooting = false, jErr = null;
  // ── SWEPT COLLISION IS OFF BY DEFAULT, AND THAT IS A MEASURED CHOICE ── it was added to stop chunks
  // tunnelling and it did, but so did joltEnsureUnder, which is the other half of that fix and the one that
  // actually earns it: with LinearCast OFF and the ground guaranteed, no body ends up under the terrain
  // (sunk 0, median gap 0.9 over a 64-body fell). What it costs is the whole frame — 83.2% of wall against
  // 18.3%. Speed-gating it does not rescue it either: gravity is 200, so a falling chunk is past any
  // sensible threshold within a fifth of a second and the gate is on almost always (measured: 80.3%).
  // __vb.joltCCD(1) turns it back on, and the per-body raise below then applies it only while a body is
  // moving more than half a voxel per step — which is the right shape for it if tunnelling ever reappears.
  let J_CCD = 0;
  let JOLT_ON = 0;                                   // __vb.jolt(1) turns it on; the legacy solver runs until then, so the two A/B inside ONE session
  const jTiles = new Map();                          // "tx,ty,tz" -> { id, boxes }  static terrain colliders
  const jShadow = new Set();                         // every PH body that currently owns a Jolt shadow — the registry the sweep below reconciles against
  const jTileQ = [];                                 // tiles wanted but not built yet — drained J_TILE_BUDGET per frame
  const jStats = { bodies: 0, tiles: 0, boxes: 0, buildMs: 0, stepMs: 0, queued: 0 };
  let jV1 = null, jV2 = null, jQ1 = null, jR1 = null;   // scratch — Emscripten objects are heap allocations, so never make them per body

  // ── GREEDY BOXING ── occ is a dense byte grid of sx*sy*sz, consumed destructively. Emits [x,y,z,w,h,d]
  // in grid units. The order is x-runs, then z-extension, then y-extension: a flat slab collapses to one
  // box, a tree trunk to a handful, and a leaf cluster to a few hundred instead of a few thousand.
  const joltGreedy = (occ, sx, sy, sz, out) => {
    const idx = (x, y, z) => x + z * sx + y * sx * sz;
    for (let y = 0; y < sy; y++) for (let z = 0; z < sz; z++) for (let x = 0; x < sx; x++) {
      if (!occ[idx(x, y, z)]) continue;
      let w = 1; while (x + w < sx && occ[idx(x + w, y, z)]) w++;   // 1. the x-run
      let d = 1;                                                   // 2. extend in z while the whole run is present
      grow: while (z + d < sz) {
        for (let i = 0; i < w; i++) if (!occ[idx(x + i, y, z + d)]) break grow;
        d++;
      }
      let hgt = 1;                                                 // 3. extend in y while the whole w*d slab is present
      growY: while (y + hgt < sy) {
        for (let k = 0; k < d; k++) for (let i = 0; i < w; i++) if (!occ[idx(x + i, y + hgt, z + k)]) break growY;
        hgt++;
      }
      for (let j = 0; j < hgt; j++) for (let k = 0; k < d; k++) for (let i = 0; i < w; i++) occ[idx(x + i, y + j, z + k)] = 0;
      out.push(x, y, z, w, hgt, d);
      x += w - 1;
    }
    return out;
  };

  // ── A BODY'S BLOB → BOXES ── local voxel coords are integers, so the box spans [x, x+w) and its centre
  // sits at x + w/2, MINUS the COM because the Jolt body's origin is the centre of mass (that is what
  // b.pos means, and keeping the two frames identical is what lets pos be written straight back).
  const joltBoxes = (b) => {
    let sc = 1, boxes = null;
    for (let attempt = 0; attempt < 3; attempt++) {   // coarsen rather than give up: 1x, then 2x, then 4x voxels per cell
      let x0 = 1e9, y0 = 1e9, z0 = 1e9, x1 = -1e9, y1 = -1e9, z1 = -1e9;
      for (let i = 0; i < b.n; i++) {
        const x = (b.lx[i] / sc) | 0, y = (b.ly[i] / sc) | 0, z = (b.lz[i] / sc) | 0;
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
        if (z < z0) z0 = z; if (z > z1) z1 = z;
      }
      const sx = x1 - x0 + 1, sy = y1 - y0 + 1, sz = z1 - z0 + 1;
      const occ = new Uint8Array(sx * sy * sz);
      for (let i = 0; i < b.n; i++) {
        const x = ((b.lx[i] / sc) | 0) - x0, y = ((b.ly[i] / sc) | 0) - y0, z = ((b.lz[i] / sc) | 0) - z0;
        occ[x + z * sx + y * sx * sz] = 1;
      }
      const g = joltGreedy(occ, sx, sy, sz, []);
      boxes = { g, x0, y0, z0, sc };
      if (g.length / 6 <= J_MAXBOX) break;
      sc *= 2;
    }
    return boxes;
  };

  const joltShapeFromBoxes = (bx, ox, oy, oz) => {   // ox/oy/oz: what to subtract, in VOXELS, to put the shape in its body frame
    const { g, x0, y0, z0, sc } = bx;
    const cs = new JL.StaticCompoundShapeSettings();
    let n = 0;
    for (let i = 0; i < g.length; i += 6) {
      const w = g[i + 3] * sc, h = g[i + 4] * sc, d = g[i + 5] * sc;
      const cx = (g[i] + x0) * sc + w * 0.5 - ox, cy = (g[i + 1] + y0) * sc + h * 0.5 - oy, cz = (g[i + 2] + z0) * sc + d * 0.5 - oz;
      jV1.Set(w * 0.5, h * 0.5, d * 0.5);
      jV2.Set(cx, cy, cz);
      cs.AddShape(jV2, jQ1, new JL.BoxShapeSettings(jV1, 0.02), 0);   // 0.02 convex radius: a voxel is 1 unit, so the default 0.05 would round the corners visibly
      n++;
    }
    const res = cs.Create();
    const shape = res.IsValid() ? res.Get() : null;
    JL.destroy(cs);
    jStats.boxes += n;
    return shape;
  };

  // ── TERRAIN TILES ── one static compound per 16^3 of solid world voxels. Keyed on the tile's world
  // origin, so it survives the CPU window sliding under it and is invalidated only by an actual edit.
  const joltTileKey = (tx, ty, tz) => tx + ',' + ty + ',' + tz;
  const joltBuildTile = (tx, ty, tz) => {
    const key = joltTileKey(tx, ty, tz);
    if (jTiles.has(key)) return;
    const bx0 = tx * JT, by0 = ty * JT, bz0 = tz * JT;
    if (by0 + JT <= 0 || by0 >= WY) { jTiles.set(key, null); return; }
    // ── hmap FIRST, AND IT IS AN EXACT ORACLE HERE ── a tile is 4096 voxel reads of W and most requested
    // tiles are empty sky above a falling chunk. hmap is the GROUND height per column, so 256 reads decide
    // whether the other 4096 are worth doing. That is only sound because these colliders are GROUND ONLY:
    // ── AND TREES ARE NOT TERRAIN (user 2026-09-01: "dont have the trees register the other trees") ──
    // wood is skipped, so a felled trunk tilts through the next tree here exactly as it does on the legacy
    // path (see phTreeBlock in sim/chop.js). Foliage was never solid to begin with. With both gone, nothing
    // a tile can contain sits above hmap, and the early-out is exact rather than a heuristic.
    let maxH = 0;
    for (let z = 0; z < JT; z++) for (let x = 0; x < JT; x++) {
      const h = hmap[gwrap(bx0 + x, WX) + gwrap(bz0 + z, WZ) * WX]; if (h > maxH) maxH = h;
    }
    if (by0 > maxH) { jTiles.set(key, null); return; }               // entirely above the ground: no scan at all
    const occ = new Uint8Array(JT * JT * JT);
    let any = 0;
    const yEnd = Math.min(JT, maxH - by0 + 1);                       // …and the rows above the ground inside a straddling tile are skipped too
    for (let y = 0; y < yEnd; y++) {
      const wy = by0 + y; if (wy < 0 || wy >= WY) continue;
      for (let z = 0; z < JT; z++) for (let x = 0; x < JT; x++) {
        const id = W[gwrap(bx0 + x, WX) + wy * WX + gwrap(bz0 + z, WZ) * WX * WY];
        if (id && solidTab[id] && !woodTab[id]) { occ[x + z * JT + y * JT * JT] = 1; any = 1; }
      }
    }
    if (!any) { jTiles.set(key, null); return; }                    // empty air: remembered, so it is not rescanned every frame
    const g = joltGreedy(occ, JT, JT, JT, []);
    const shape = joltShapeFromBoxes({ g, x0: 0, y0: 0, z0: 0, sc: 1 }, 0, 0, 0);
    if (!shape) { jTiles.set(key, null); return; }
    jR1.Set(bx0, by0, bz0);
    const bcs = new JL.BodyCreationSettings(shape, jR1, jQ1, JL.EMotionType_Static, J_LAYER_STATIC);
    const body = jBI.CreateBody(bcs);
    JL.destroy(bcs);
    jBI.AddBody(body.GetID(), JL.EActivation_DontActivate);
    jTiles.set(key, { id: body.GetID(), n: g.length / 6 });
    jStats.tiles = jTiles.size;
  };
  const joltDropTile = (key) => {
    const t = jTiles.get(key);
    if (t) { jBI.RemoveBody(t.id); jBI.DestroyBody(t.id); }
    jTiles.delete(key);
    jStats.tiles = jTiles.size;
  };
  // gpuPatch calls this for every edited column, so digging a hole changes what a chunk falls through.
  // gx/gz arrive WINDOW-WRAPPED (that is what gpuPatch holds); tiles are keyed in WORLD space, so the
  // window origin is what converts between them — the same unwrap the rest of the engine does.
  const joltInvalidate = (gx, gy, gz) => {
    if (!jReady || !jTiles.size) return;
    const wx = winOX + ((gx - gwrap(winOX, WX) + WX) % WX), wz = winOZ + ((gz - gwrap(winOZ, WZ) + WZ) % WZ);
    const tx = Math.floor(wx / JT), ty = Math.floor(gy / JT), tz = Math.floor(wz / JT);
    for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++)
      joltDropTile(joltTileKey(tx + dx, ty + dy, tz + dz));   // the 3x3x3 neighbourhood: an edit on a tile boundary changes the box runs either side of it
  };

  // ── THE GROUND UNDER A BODY, SYNCHRONOUSLY ── the per-frame queue is a fairness budget for the wide
  // neighbourhood; it is the wrong tool for the column a body is about to fall down. This builds that
  // column now, from the body down to a tile below the terrain, and it is bounded by J_UNDER tiles so a
  // body spawned in the sky cannot stall the frame.
  const J_UNDER = 10;
  const joltEnsureUnder = (b) => {
    const hx = b.jHx !== undefined ? b.jHx : b.rMax;
    const t0x = Math.floor((b.pos[0] - hx) / JT), t1x = Math.floor((b.pos[0] + hx) / JT);
    const t0z = Math.floor((b.pos[2] - hx) / JT), t1z = Math.floor((b.pos[2] + hx) / JT);
    const ty1 = Math.floor((b.pos[1] + hx) / JT);
    let gy = 0;                                      // the terrain under the body's footprint — nothing below it can ever be reached
    for (let x = Math.floor(b.pos[0] - hx); x <= b.pos[0] + hx; x += 4) for (let z = Math.floor(b.pos[2] - hx); z <= b.pos[2] + hx; z += 4) {
      const h = hmap[gwrap(x, WX) + gwrap(z, WZ) * WX]; if (h > gy) gy = h;
    }
    const ty0 = Math.max(Math.floor((gy - JT) / JT), ty1 - J_UNDER);
    for (let ty = ty1; ty >= ty0; ty--) for (let tz = t0z; tz <= t1z; tz++) for (let tx = t0x; tx <= t1x; tx++)
      if (!jTiles.has(joltTileKey(tx, ty, tz))) joltBuildTile(tx, ty, tz);
  };

  // ── A PH BODY'S JOLT SHADOW ──
  const joltAddBody = (b) => {
    const t0 = performance.now();
    const bx = joltBoxes(b);
    const shape = joltShapeFromBoxes(bx, b.com[0], b.com[1], b.com[2]);
    if (!shape) { b.jNo = 1; return; }
    jR1.Set(b.pos[0], b.pos[1], b.pos[2]);
    jQ1.Set(b.q[0], b.q[1], b.q[2], b.q[3]);
    const bcs = new JL.BodyCreationSettings(shape, jR1, jQ1, JL.EMotionType_Dynamic, J_LAYER_MOVING);
    // MASS AND INERTIA COME FROM THE BLOB, NOT FROM THE BOXES. phBuildBody already computed both exactly
    // (mass = voxel count at unit density, I = the diagonal about the COM), and the greedy boxes are a
    // COLLISION approximation — letting Jolt derive mass properties from them would make a coarsened body
    // heavier than the voxels it is drawn from, and the fall would not match what the player sees.
    bcs.mOverrideMassProperties = JL.EOverrideMassProperties_MassAndInertiaProvided;
    bcs.mMassPropertiesOverride.mMass = b.mass;
    jV1.Set(b.I[0], b.I[1], b.I[2]);
    bcs.mMassPropertiesOverride.mInertia.SetDiagonal3(jV1);
    bcs.mLinearDamping = 0.05; bcs.mAngularDamping = 0.1;
    bcs.mFriction = 0.6; bcs.mRestitution = 0.0;    // voxel rubble does not bounce
    bcs.mAllowSleeping = true;
    // ── SWEPT, NOT DISCRETE ── the legacy solver carried a hand-written CCD-LITE for exactly this reason
    // (see the note in physStep): gravity here is 200, so a chunk one second into a fall moves 3.3 voxels
    // per step and a discrete solver walks it straight through the ground it should have hit. MEASURED
    // without this: bodies came to rest 31 voxels UNDER a terrain surface at y=160.
    bcs.mMotionQuality = JL.EMotionQuality_Discrete;   // …and it is raised to LinearCast only while a body is actually fast enough to tunnel — see the note in joltStepBodies
    const body = jBI.CreateBody(bcs);
    JL.destroy(bcs);
    jBI.AddBody(body.GetID(), JL.EActivation_Activate);
    // the body's own half-extent about its COM, which is what bounds the terrain it can touch. One pass,
    // once, at creation — and NOT rMax, which is the distance to the farthest voxel and so describes a
    // sphere that a long thin body (a felled trunk) massively overstates in its two short axes.
    let ax0 = 1e9, ay0 = 1e9, az0 = 1e9, ax1 = -1e9, ay1 = -1e9, az1 = -1e9;
    for (let i = 0; i < b.n; i++) {
      if (b.lx[i] < ax0) ax0 = b.lx[i]; if (b.lx[i] > ax1) ax1 = b.lx[i];
      if (b.ly[i] < ay0) ay0 = b.ly[i]; if (b.ly[i] > ay1) ay1 = b.ly[i];
      if (b.lz[i] < az0) az0 = b.lz[i]; if (b.lz[i] > az1) az1 = b.lz[i];
    }
    b.jHx = Math.max(b.com[0] - ax0, ax1 + 1 - b.com[0], b.com[1] - ay0, ay1 + 1 - b.com[1], b.com[2] - az0, az1 + 1 - b.com[2]);
    b.jID = body.GetID();
    jShadow.add(b);
    b.jBody = body;                                // the BODY, not just its id: BodyInterface has no GetCenterOfMassPosition in this build, and reading off the object skips an id lookup per field anyway
    b.jUnderY = b.pos[1]; joltEnsureUnder(b);         // …and its ground exists BEFORE it is ever stepped: a 12-tile/frame queue cannot outrun a body that starts 120 voxels up
    jV1.Set(b.vel[0], b.vel[1], b.vel[2]); body.SetLinearVelocity(jV1);
    jV1.Set(b.omega[0], b.omega[1], b.omega[2]); body.SetAngularVelocity(jV1);
    jStats.bodies++;
    jStats.buildMs += performance.now() - t0;
  };
  const joltDropBody = (b) => {
    if (!jReady || b.jID === undefined) return;
    jBI.RemoveBody(b.jID); jBI.DestroyBody(b.jID);
    b.jID = undefined; b.jBody = undefined; jShadow.delete(b); jStats.bodies--;
  };

  // ── THE STEP ── PH.bodies in, one Jolt step, PH.bodies out. Everything around it (the break clock, the
  // absorb flight, retirement, the topple drive) stays in sim/solver.js and keeps owning the game rules;
  // this owns only where a body ends up.
  const joltStepBodies = (dt) => {
    if (!jReady) return false;
    const t0 = performance.now();
    // 1. bodies that appeared or vanished since the last step.
    // ── THE SWEEP IS NOT OPTIONAL ── a body leaves PH.bodies through a dozen different splices (expiry,
    // voidFall, offRect, a finished absorb, phMakeRoom, the target splice in phChopBody, the re-split in
    // chop-tree) and none of them know Jolt exists. Hooking them all is a rename sweep that would rot; a
    // mark-and-sweep against the live list catches every path including ones added later. MEASURED without
    // it: 128 Jolt shadows against 59 live bodies after one fell, which is 69 invisible colliders left
    // standing in the world.
    for (const b of PH.bodies) { b.jSeen = jFrame; if (b.jID === undefined && !b.jNo && !b.absorbing) joltAddBody(b); }
    if (jShadow.size !== PH.bodies.length) {
      for (const b of [...jShadow]) if (b.jSeen !== jFrame) joltDropBody(b);
    }
    // 2. terrain under everything awake, queued rather than built inline
    jTileQ.length = 0;
    for (const b of PH.bodies) {
      if (b.sleeping || b.jID === undefined) continue;
      // the body's own extent, not rMax: rMax is the distance to its FARTHEST voxel, so a felled pine's
      // sphere is 50 voxels in every direction including straight up, and 26 chunks of one tree asked for
      // 4096 tiles and 84.6k boxes. A chunk only ever collides with what its box overlaps, plus a margin.
      const hx = (b.jHx !== undefined ? b.jHx : b.rMax), pad = JT_PAD * JT;
      const t0x = Math.floor((b.pos[0] - hx - pad) / JT), t1x = Math.floor((b.pos[0] + hx + pad) / JT);
      const t0y = Math.floor((b.pos[1] - hx - pad) / JT), t1y = Math.floor((b.pos[1] + hx + pad) / JT);
      const t0z = Math.floor((b.pos[2] - hx - pad) / JT), t1z = Math.floor((b.pos[2] + hx + pad) / JT);
      for (let ty = t0y; ty <= t1y; ty++) for (let tz = t0z; tz <= t1z; tz++) for (let tx = t0x; tx <= t1x; tx++) {
        const key = joltTileKey(tx, ty, tz);
        const t = jTiles.get(key);
        if (t === undefined) jTileQ.push(tx, ty, tz); else if (t) t.used = jFrame;
      }
    }
    jStats.queued = jTileQ.length / 3;
    for (let i = 0, built = 0; i < jTileQ.length && built < J_TILE_BUDGET; i += 3, built++)
      joltBuildTile(jTileQ[i], jTileQ[i + 1], jTileQ[i + 2]);
    // ── AND THEY ARE EVICTED, OR THE SET ONLY EVER GROWS ── a tile is a static collider plus its boxes,
    // and nothing frees it when the chunk that wanted it settles and the next fell happens 200 voxels away.
    // Evicted by AGE, not on sight: at a 1024 cap with ~950 live tiles the set sat on the boundary and
    // rebuilt what it had just dropped every frame, which is the most expensive way to hold a cache.
    if (jTiles.size > J_TILE_CAP) {
      for (const [key, t] of jTiles) {
        if (jTiles.size <= J_TILE_CAP * 0.75) break;
        if (t && jFrame - (t.used || 0) < 240) continue;   // seen in the last ~4 s at 60 Hz
        joltDropTile(key);
      }
    }
    jFrame++;
    // 3. push any velocity the game authored this frame (the topple drive writes b.vel/b.omega directly)
    for (const b of PH.bodies) {
      if (b.jID === undefined || b.sleeping) continue;
      if (b.jPush) {
        jV1.Set(b.vel[0], b.vel[1], b.vel[2]); b.jBody.SetLinearVelocity(jV1);
        jV1.Set(b.omega[0], b.omega[1], b.omega[2]); b.jBody.SetAngularVelocity(jV1);
        b.jPush = 0;
      }
    }
    // …and only when it has actually moved a tile's worth. This ran per falling body PER STEP, and with 70
    // chunks off one fell it was the single biggest cost in the step (measured: buildMs 76 ms, step at 80%
    // of wall time). A body that has not crossed half a tile since its last ensure cannot have outrun it.
    // ── CCD IS PER BODY AND PER MOMENT, NOT A GLOBAL SETTING ── LinearCast shape-casts the whole compound
    // every step, and on these shapes that is the most expensive thing in the frame: MEASURED over one fell,
    // the step went 83.2% of wall with it on against 18.3% with it off, a 4.5x difference, and with it OFF
    // no body ended up under the ground anyway (sunk 0, median gap 0.9) because joltEnsureUnder already
    // guarantees the ground exists before a body is stepped. So it is not needed for the common case and is
    // far too expensive to leave on. A body only tunnels if it crosses more than half a voxel per step, so
    // that — and only that — is when the swept solver is worth paying for.
    const vTun = 0.5 / PH.dt;                        // vox/s at which a step is longer than half a voxel
    for (const b of PH.bodies) {
      if (b.sleeping || b.jID === undefined) continue;
      const sp = Math.hypot(b.vel[0], b.vel[1], b.vel[2]);
      const want = J_CCD && sp > vTun ? 1 : 0;
      if (want !== (b.jCCD | 0)) { jBI.SetMotionQuality(b.jID, want ? JL.EMotionQuality_LinearCast : JL.EMotionQuality_Discrete); b.jCCD = want; }
      if (b.vel[1] >= -8) continue;
      if (b.jUnderY !== undefined && b.jUnderY - b.pos[1] < JT * 0.5) continue;
      b.jUnderY = b.pos[1]; joltEnsureUnder(b);
    }
    jIface.Step(dt, 1);
    // 4. read back
    for (const b of PH.bodies) {
      if (b.jID === undefined) continue;
      const J = b.jBody;
      const p = J.GetCenterOfMassPosition(), q = J.GetRotation();
      b.pos[0] = p.GetX(); b.pos[1] = p.GetY(); b.pos[2] = p.GetZ();
      b.q[0] = q.GetX(); b.q[1] = q.GetY(); b.q[2] = q.GetZ(); b.q[3] = q.GetW();
      const v = J.GetLinearVelocity(), w = J.GetAngularVelocity();
      b.vel[0] = v.GetX(); b.vel[1] = v.GetY(); b.vel[2] = v.GetZ();
      b.omega[0] = w.GetX(); b.omega[1] = w.GetY(); b.omega[2] = w.GetZ();
      b.sleeping = !J.IsActive();
    }
    jStats.stepMs += performance.now() - t0;
    return true;
  };

  // ── BOOT ── a dynamic import, so nothing about the 3 MB wasm is on the critical path of a normal load:
  // the legacy solver runs until Jolt reports ready, and JOLT_ON only flips once it has.
  const joltBoot = async () => {
    if (jBooting || jReady) return jReady;
    jBooting = true;
    try {
      const mod = await import('./' + JOLT_URL);
      JL = await mod.default({ locateFile: (p) => 'assets/lib/' + p });
      const objFilter = new JL.ObjectLayerPairFilterTable(2);
      objFilter.EnableCollision(J_LAYER_STATIC, J_LAYER_MOVING);
      objFilter.EnableCollision(J_LAYER_MOVING, J_LAYER_MOVING);
      const bpi = new JL.BroadPhaseLayerInterfaceTable(2, 2);
      bpi.MapObjectToBroadPhaseLayer(J_LAYER_STATIC, new JL.BroadPhaseLayer(0));
      bpi.MapObjectToBroadPhaseLayer(J_LAYER_MOVING, new JL.BroadPhaseLayer(1));
      const st = new JL.JoltSettings();
      st.mObjectLayerPairFilter = objFilter;
      st.mBroadPhaseLayerInterface = bpi;
      st.mObjectVsBroadPhaseLayerFilter = new JL.ObjectVsBroadPhaseLayerFilterTable(bpi, 2, objFilter, 2);
      jIface = new JL.JoltInterface(st);
      JL.destroy(st);
      jSys = jIface.GetPhysicsSystem();
      jBI = jSys.GetBodyInterface();
      jV1 = new JL.Vec3(0, 0, 0); jV2 = new JL.Vec3(0, 0, 0);
      jQ1 = new JL.Quat(0, 0, 0, 1); jR1 = new JL.RVec3(0, 0, 0);
      jV1.Set(0, -PH.gravity, 0); jSys.SetGravity(jV1);   // the SAME gravity the legacy solver and the player use, so a fall reads at one scale
      jReady = true;
    } catch (e) {
      jErr = String(e && e.message || e);
      console.warn('[vb] jolt boot failed:', jErr);
    }
    jBooting = false;
    return jReady;
  };
  const joltReady = () => jReady;
  const joltCCD = (v) => { if (v !== undefined) J_CCD = v ? 1 : 0; return J_CCD; };
  const joltOn = (v) => {                            // __vb.jolt(1) — the A/B, in one session, against the legacy solver
    if (v === undefined) return { on: JOLT_ON, ready: jReady, err: jErr };
    if (v && !jReady) { joltBoot(); return { on: 0, ready: false, booting: true, err: jErr }; }
    if (!v) for (const b of PH.bodies) joltDropBody(b);   // handing control back: the shadows go, PH.bodies keeps its state and the legacy solver picks it up mid-air
    JOLT_ON = v ? 1 : 0;
    return { on: JOLT_ON, ready: jReady, err: jErr };
  };
  const joltActive = () => JOLT_ON && jReady;
  const joltStats = () => ({ ...jStats, on: JOLT_ON, ready: jReady, err: jErr, tiles: jTiles.size,
    buildMs: +jStats.buildMs.toFixed(1), stepMs: +jStats.stepMs.toFixed(1) });
