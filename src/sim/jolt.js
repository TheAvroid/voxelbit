  // @module - JOLT-BACKED RIGID BODY SOLVER (user 2026-09-01: "use JoltPhysics.js for the physics")
  // @exports joltReady, joltStep, joltTerrain, JOLT, JOLT_ON
  //
  // WHAT THIS REPLACES, AND WHAT IT DELIBERATELY DOES NOT.
  // sim/solver.js integrates every rigid body by hand: gravity, the contact solve against W, sleep and
  // the settle rules. THAT is what moves here. What does NOT move is the body RECORD or the way it
  // reaches the GPU: main/tick-emit.js reads b.pos and b.q off each entry in PH.bodies and writes them
  // into u.physB, and the trace walks them from there. So Jolt owns the MOTION and the existing record
  // stays the interchange format - one solver swapped underneath a render path that already works.
  // Replacing both at once would mean debugging two unknowns with no working reference to bisect on.
  //
  // THE LIBRARY IS VENDORED, NOT FETCHED. game/vendor/jolt-physics.wasm-compat.js is the variant with
  // the WASM base64-embedded, so there is ONE file and no second request. voxelbit ships as a local
  // browser game on the player's own machine, so a runtime unpkg import would mean no physics offline.
  //
  // TERRAIN IS A HEIGHTFIELD, NOT A BODY PER VOXEL. A voxel-per-body world is not representable: the
  // near window alone is 2048 x 2048 columns. Jolt's HeightFieldShape takes exactly what hmap already
  // is, one height per column, so the static world costs ONE body. Its limit is worth writing down: a
  // heightfield cannot express an overhang, so nothing can land on the underside of anything. Nothing
  // in this world has an underside yet (the caves were deleted 2026-08-05); when something does, the
  // answer is a second static body for that object, not abandoning the field.
  const JOLT = {
    on: false, ready: false, J: null, iface: null, ps: null, bi: null,
    bodies: new Map(),                                 // voxelbit body  ->  Jolt body id
    terrainId: null, terrainOX: 1e9, terrainOZ: 1e9,   // the heightfield's current window origin
    NL: 0, ML: 1,                                      // object layers: NON_MOVING, MOVING
    stats: { added: 0, dropped: 0, steps: 0, lastMs: 0 },
  };
  const JOLT_HF = 128;                                 // heightfield samples per side
  // ── ON BY DEFAULT, WITH AN ESCAPE HATCH (user 2026-09-01: "completely rebuild the systems") ── it
  // shipped behind ?jolt while the two coexisted and it has now been through the whole lifecycle: a
  // pine felled, fallen under Jolt gravity, landed, slept and retired back into W, with tracked ==
  // bodies at every sample across 165 adds and 80 drops and no errors. ?nojolt still selects the
  // hand-written integrator, which is what a bisect needs when something looks wrong later - the old
  // solver is not deleted, it is the reference.
  const JOLT_ON = !location.search.includes('nojolt');

  const joltInit = async () => {
    if (JOLT.iface || !JOLT_ON) return;
    const mod = await import('./vendor/jolt-physics.wasm-compat.js');
    const J = await mod.default();
    JOLT.J = J;
    const NUM_OL = 2, BP_N = 0, BP_M = 1, NUM_BP = 2;
    const ol = new J.ObjectLayerPairFilterTable(NUM_OL);
    ol.EnableCollision(JOLT.NL, JOLT.ML);
    ol.EnableCollision(JOLT.ML, JOLT.ML);
    const bp = new J.BroadPhaseLayerInterfaceTable(NUM_OL, NUM_BP);
    bp.MapObjectToBroadPhaseLayer(JOLT.NL, new J.BroadPhaseLayer(BP_N));
    bp.MapObjectToBroadPhaseLayer(JOLT.ML, new J.BroadPhaseLayer(BP_M));
    const st = new J.JoltSettings();
    st.mObjectLayerPairFilter = ol;
    st.mBroadPhaseLayerInterface = bp;
    st.mObjectVsBroadPhaseLayerFilter = new J.ObjectVsBroadPhaseLayerFilterTable(bp, NUM_BP, ol, NUM_OL);
    JOLT.iface = new J.JoltInterface(st);
    JOLT.ps = JOLT.iface.GetPhysicsSystem();
    JOLT.bi = JOLT.ps.GetBodyInterface();
    JOLT.ready = true; JOLT.on = true;
    console.log('[vb] jolt ready');
  };

  // Rebuilt when the player leaves the field's middle third - the same rule the terrain streamer uses.
  // Heights come from hmap, so the collision surface IS the surface the renderer draws.
  const joltTerrain = (px, pz) => {
    if (!JOLT.ready) return;
    const J = JOLT.J, half = JOLT_HF >> 1;
    const ox = Math.round(px) - half, oz = Math.round(pz) - half;
    if (Math.abs(ox - JOLT.terrainOX) < (half >> 1) && Math.abs(oz - JOLT.terrainOZ) < (half >> 1)) return;
    if (JOLT.terrainId !== null) { JOLT.bi.RemoveBody(JOLT.terrainId); JOLT.bi.DestroyBody(JOLT.terrainId); JOLT.terrainId = null; }
    const n = JOLT_HF, samples = new Float32Array(n * n);
    for (let z = 0; z < n; z++) for (let x = 0; x < n; x++) {
      const h = hmap[gwrap(ox + x, WX) + gwrap(oz + z, WZ) * WX];
      samples[x + z * n] = h > 0 ? h : WL;             // an unwritten column reads 0; the waterline is the honest stand-in
    }
    // ── FILLING THE SAMPLES THROUGH THE VECTOR, NOT THROUGH _malloc ── the first cut called J._malloc
    // and J.HEAPF32.set, which is the plain-emscripten idiom and is NOT what this build exposes: the
    // wasm-compat bundle has _webidl_malloc, getPointer and the heaps, and it throws
    // "J._malloc is not a function" the moment the first heightfield is built. mHeightSamples is a
    // bound std::vector, so the honest route is resize it, ask for its data pointer, and write
    // straight into the WASM heap through a view — no allocation of our own to leak.
    const hfs = new J.HeightFieldShapeSettings();
    hfs.mSampleCount = n;
    hfs.mOffset = new J.Vec3(0, 0, 0);
    hfs.mScale = new J.Vec3(1, 1, 1);
    hfs.mHeightSamples.resize(n * n);
    const view = new Float32Array(J.HEAPF32.buffer, J.getPointer(hfs.mHeightSamples.data()), n * n);
    view.set(samples);
    const shape = hfs.Create().Get();
    J.destroy(hfs);                                    // the settings have done their job; the shape holds its own copy of the samples. Not freeing this leaks n*n floats of WASM heap on EVERY field rebuild, and the field rebuilds as the player walks.
    const bcs = new J.BodyCreationSettings(shape, new J.RVec3(ox, 0, oz), new J.Quat(0, 0, 0, 1), J.EMotionType_Static, JOLT.NL);
    JOLT.terrainId = JOLT.bi.CreateAndAddBody(bcs, J.EActivation_DontActivate);
    JOLT.terrainOX = ox; JOLT.terrainOZ = oz;
  };

  // A chopped component becomes ONE dynamic body: a BOX of its own extents, not a compound of its
  // voxels. A felled pine is ~14,000 voxels and a compound of 14,000 boxes is not a shape, it is a
  // stall - Teardown itself traces a voxel model inside an OBB rather than handing the solver voxels.
  // Mass follows the voxel COUNT, so a crown that is mostly air stays light.
  const joltAddBody = (b) => {
    if (!JOLT.ready || JOLT.bodies.has(b)) return;
    const J = JOLT.J;
    // ── NOTHING NON-FINITE CROSSES INTO WASM (user 2026-09-01: "memory access out of bounds" every frame) ──
    // Jolt is a C++ library behind an emscripten binding: hand it a NaN position and it does not throw, it
    // corrupts, and every later Step reads off the end of the heap - which is exactly the shape of the
    // report, one throw per frame for ever rather than one throw once. And this world HAS a NaN class:
    // a perched songbird keeps its height in perchFeet and leaves B.y as NaN deliberately (see the note
    // in main/tick-creatures.js), so any body built from one arrives here with a NaN in it.
    // Refusing the body is the right failure: it stays in PH.bodies and the hand-written solver's
    // bookkeeping still sees it, so it is not lost - it simply is not simulated by Jolt.
    const fin = (v) => Number.isFinite(v);
    if (!fin(b.pos[0]) || !fin(b.pos[1]) || !fin(b.pos[2]) ||
        !fin(b.q[0]) || !fin(b.q[1]) || !fin(b.q[2]) || !fin(b.q[3]) ||
        !fin(b.vel[0]) || !fin(b.vel[1]) || !fin(b.vel[2]) ||
        !fin(b.omega[0]) || !fin(b.omega[1]) || !fin(b.omega[2])) {
      JOLT.stats.refused = (JOLT.stats.refused | 0) + 1; return;
    }
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9;
    for (let i = 0; i < b.n; i++) {
      if (b.lx[i] < x0) x0 = b.lx[i]; if (b.lx[i] > x1) x1 = b.lx[i];
      if (b.ly[i] < y0) y0 = b.ly[i]; if (b.ly[i] > y1) y1 = b.ly[i];
      if (b.lz[i] < z0) z0 = b.lz[i]; if (b.lz[i] > z1) z1 = b.lz[i];
    }
    const hx = Math.max(0.5, (x1 - x0 + 1) * 0.5), hy = Math.max(0.5, (y1 - y0 + 1) * 0.5), hz = Math.max(0.5, (z1 - z0 + 1) * 0.5);
    const shape = new J.BoxShapeSettings(new J.Vec3(hx, hy, hz)).Create().Get();
    const bcs = new J.BodyCreationSettings(shape, new J.RVec3(b.pos[0], b.pos[1], b.pos[2]),
      new J.Quat(b.q[0], b.q[1], b.q[2], b.q[3]), J.EMotionType_Dynamic, JOLT.ML);
    const id = JOLT.bi.CreateAndAddBody(bcs, J.EActivation_Activate);
    JOLT.bi.SetLinearVelocity(id, new J.Vec3(b.vel[0], b.vel[1], b.vel[2]));
    JOLT.bi.SetAngularVelocity(id, new J.Vec3(b.omega[0], b.omega[1], b.omega[2]));
    JOLT.bodies.set(b, id); JOLT.stats.added++;
  };

  // ── DROPPING A BODY IS THE DANGEROUS DIRECTION (user 2026-09-01: "memory access out of bounds" every
  // frame) ── reproduced under fell stress, and the stack ends in Destroy. Two faults, both here:
  //   · RemoveBody was called unconditionally. Jolt does not tolerate being told to remove a body it has
  //     already removed - it is not a no-op, it corrupts, and every Step after that reads off the heap.
  //     That is why one bad drop turns into a throw EVERY frame for ever rather than one throw once.
  //   · the map entry was deleted last. Anything that re-entered between the destroy and the delete -
  //     and joltStep's writeback guard now calls this from inside its own loop over the map - could find
  //     the entry still there and destroy the same id twice.
  // So: delete the mapping FIRST, so no path can find this body again; ask before removing; and wrap the
  // pair, because a WASM abort must not take the frame down with it.
  const joltDropBody = (b) => {
    if (!JOLT.ready) return;
    const id = JOLT.bodies.get(b); if (id === undefined) return;
    JOLT.bodies.delete(b);
    try {
      if (JOLT.bi.IsAdded(id)) { JOLT.bi.RemoveBody(id); }
      JOLT.bi.DestroyBody(id);
    } catch (e) { JOLT.stats.dropErr = (JOLT.stats.dropErr | 0) + 1; }
    JOLT.stats.dropped++;
  };

  // Step, then write the transforms back. b.pos and b.q are what main/tick-emit.js publishes, so once
  // these are Jolt's the renderer is drawing Jolt's world with no change to the upload or the shader.
  const joltStep = (dt) => {
    if (!JOLT.ready) return;
    const t0 = performance.now();
    // SYNC THE SET HERE, NOT AT EVERY CREATION SITE. Bodies are born in half a dozen places across
    // chop.js, chop-tree.js and reactions.js, and each of those would need a joltAddBody beside it -
    // six edits that can silently fall out of step the moment a seventh site is added. PH.bodies is
    // already the single list of what exists, so diffing against it once a frame is both cheaper to
    // write and impossible to forget. A retired body leaves PH.bodies, so it leaves Jolt on the same
    // frame and cannot go on colliding invisibly.
    for (let i = 0; i < PH.bodies.length; i++) { const b = PH.bodies[i]; if (!JOLT.bodies.has(b)) joltAddBody(b); }
    if (JOLT.bodies.size > PH.bodies.length) {
      const live = new Set(PH.bodies);
      for (const b of Array.from(JOLT.bodies.keys())) if (!live.has(b)) joltDropBody(b);
    }
    JOLT.iface.Step(Math.min(0.05, dt), 1);
    for (const [b, id] of JOLT.bodies) {
      // ── ASK ONLY ABOUT BODIES JOLT STILL HAS ── this guard was dropped when the sync moved in here,
      // and Jolt does not answer politely: GetPosition on an id it has destroyed reads off the end of
      // the WASM heap and the whole tick dies with "RuntimeError: memory access out of bounds", which
      // surfaces as a frozen sim behind a rendered frame. A body can leave between the sync above and
      // this loop because physStep retires and evicts inside the same frame.
      // Not just forget it - DESTROY it. Deleting the mapping alone left the Jolt body allocated and
      // unreachable: a leak, and worse, an id nothing owns any more that a later drop could still name.
      if (!JOLT.bi.IsAdded(id)) { joltDropBody(b); continue; }
      const p = JOLT.bi.GetPosition(id), q = JOLT.bi.GetRotation(id);
      // …and nothing non-finite comes back out either. If a body ever does go bad inside the solver, the
      // damage stops at this line instead of being written into b.pos, where main/tick-emit.js would
      // publish it into u.physB and hand the corruption to the shader as well.
      if (!Number.isFinite(p.GetX()) || !Number.isFinite(p.GetY()) || !Number.isFinite(p.GetZ())) {
        JOLT.stats.refused = (JOLT.stats.refused | 0) + 1; joltDropBody(b); continue;
      }
      b.pos[0] = p.GetX(); b.pos[1] = p.GetY(); b.pos[2] = p.GetZ();
      b.q[0] = q.GetX(); b.q[1] = q.GetY(); b.q[2] = q.GetZ(); b.q[3] = q.GetW();
      const v = JOLT.bi.GetLinearVelocity(id);
      b.vel[0] = v.GetX(); b.vel[1] = v.GetY(); b.vel[2] = v.GetZ();
      b.sleeping = !JOLT.bi.IsActive(id);
    }
    JOLT.stats.steps++; JOLT.stats.lastMs = performance.now() - t0;
  };
  const joltReady = () => JOLT.ready;

  // ── AND IT STARTS ITSELF, FROM HERE ── the first cut called joltInit() from world/build.js, which is
  // manifest line 25 against this file's 49: every fragment shares ONE scope, so that call ran before
  // this const existed and boot died on a ReferenceError with a blank screen and no console — the
  // const-before-declaration failure this codebase keeps producing. Starting it at the bottom of its
  // own module cannot be ordered wrong, because there is nothing after it to be ordered against.
  // Never awaited: every entry point above is guarded on JOLT.ready and main/tick-body.js keeps using
  // the hand-written solver until it flips, so 3.8 MB of WASM never holds up the world.
  joltInit().catch((e) => console.error('[vb] jolt init failed:', e));
