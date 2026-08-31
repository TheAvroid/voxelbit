  // ── GPU buffers ─────────────────────────────────────────────────────────────
  setLoad(94); await stage('uploading world…');
  // ── THERE IS NO DENSE GPU WORLD ── W (CPU) is the source of truth; the GPU sees only the paged brick
  // pool below, which is its derived cache. That is the whole 1.5 GB: at a 2048 window the same world costs
  // ~260 MB pooled, because ~47% of it is all-air brick with no payload and another ~42% is rock sealed
  // behind rock that no ray can reach, sharing ONE page between all of them.
  // ── THE L1 OCCUPANCY BITMASK NO LONGER GOES TO THE GPU ── bdesc replaced it: a nonzero descriptor IS
  // the occupancy bit, which is what kept TRACE inside the 8-storage-buffer cap. `bricks` is still built and
  // maintained on the CPU, because poolFlush and gpuPatch both read it, but nothing on the GPU binds it, so
  // uploading it was 393 KB of pure waste on every band and every dirty-word flush. L2 (bricks2) and the
  // water-only bits (wbricks) are still read by the DDA and still upload.
  // ── PAGED BRICK POOL (read path) ─────────────────────────────────────────────────────────────
  // Measured 2026-08-16: 45-49% of the window is all-air brick and 45-46% is air-free rock sealed
  // behind more air-free rock; only 6-9% is the surface shell anything can ever see. This is the
  // first half of spending storage on that instead of on a flat array: an EMPTY brick keeps no
  // payload at all, only its 4-byte descriptor. The dense buffer is still the source of truth and
  // every write path still targets it - poolBuild() re-derives the pool from W on demand, which is
  // enough to verify the read path is bit-identical and to measure what it costs.
  // NOTHING here is allocated unless the probe is on. Sized for players it would be ~1 GB of JS heap and
  // ~1 GB of GPU buffer that no frame ever reads - a far bigger regression than the feature is a win.
  const POOL_FRAC = 0.25;                              // slots as a fraction of all bricks. With sealed rock sharing one page the real figure is ~12%; this is 2x headroom for the transient before sealing settles.
  // ── AND THE RING IS SIZED OFF THE GPU GRID, NOT THE CPU ONE ── a storage buffer cannot grow, so the pool
  // has to be allocated for the widest window it will ever hold. At GMUL 2 that is four times the bricks, and
  // the fraction comes down because the measured steady state is ~13%: the 2x headroom POOL_FRAC carries is
  // there for the sealing transient, and a ring tile is sealed correctly on its FIRST pass (it has its whole
  // slab in hand), so it never needs that headroom.
  // THE ALLOCATION IS WHAT DECIDES WHETHER A MACHINE CAN RUN THIS, not the fraction of it that ends up
  // occupied: ~1.03 GB at GMUL 2 against 402 MB at GMUL 1. That is the whole reason GMUL rides the adapter.
  const POOL_FRAC_RING = 0.17;
  const POOL_SLOTS = GMUL > 1 ? Math.ceil(GBX * GBY * GBZ * POOL_FRAC_RING) : Math.ceil(BX * BY * BZ * POOL_FRAC);
  // bdesc, the water bits and the L2 bits are all on the GPU GRID (see TWO WINDOWS in world/window.js):
  // they cover the far ring as well, and the shader indexes nothing else. airFree stays on the CPU grid,
  // because deciding it means reading W, and W only exists for the near window.
  const bdesc = new Uint32Array(GBX * GBY * GBZ);       // 0 = all air, else slot+1
  const gwb = new Uint32Array((GBX * GBY * GBZ) >> 5);  // water-only brick bits, GPU grid — skipW rays stride these
  const gb2 = new Uint32Array(((GBX >> 2) * (GBY >> 2) * (GBZ >> 2)) >> 5);   // L2 32-voxel super-brick occupancy, GPU grid
  const GB2X = GBX >> 2, GB2Y = GBY >> 2;
  // CPU brick index -> GPU brick index. The CPU array holds worldBX mod BX and the GPU array holds
  // worldBX mod GBX, so the world coord has to be recovered through the window origin before it can be
  // re-wrapped. Both windows are concentric and slide together, so this never has to handle them drifting.
  const cpu2gpu = (b) => {
    const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0;
    const ox = winOX >> 3, oz = winOZ >> 3;
    const wbx = ox + (((bx - ox) % BX) + BX) % BX, wbz = oz + (((bz - oz) % BZ) + BZ) % BZ;
    return ((wbx % GBX) + GBX) % GBX + by * GBX + ((((wbz % GBZ) + GBZ) % GBZ)) * GBX * GBY;
  };
  // A full rebuild used to gather into a POOL_SLOTS * 512 staging array - 400 MB of JS heap, alive forever,
  // touched only by poolBuild. Slots are handed out sequentially there, so a fixed CHUNK of them can be filled
  // and flushed instead: same number of bytes uploaded, 8 MB of heap rather than 400.
  const POOL_CHUNK = 16384;                            // slots per staged upload (8 MB)
  const poolCPU = new Uint8Array(POOL_CHUNK * 512);
  const bdescBuf = device.createBuffer({ size: bdesc.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });   // COPY_SRC for __vb.gpudiff()
  const gwbBuf = device.createBuffer({ size: gwb.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const gb2Buf = device.createBuffer({ size: gb2.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const poolBuf = device.createBuffer({ size: POOL_SLOTS * 512, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });   // COPY_SRC: __vb.gpudiff() reads the pool back to verify it still matches W
  const bdescRead = device.createBuffer({ size: bdesc.byteLength, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  let poolUsed = 0, poolOverflow = 0, poolSealed = 0;
  // ── WHAT A SEALED BRICK READS AS, AND IT MUST BE ROCK ── every sealed brick in the window shares ONE page,
  // so whatever fills that page is what shows anywhere a ray does reach one. This was the literal 71, chosen
  // in 2026-08-16 as "the commonest deep-strata id" off a census — and the palette has been reordered and
  // extended several times since. Id 71 is now [220, 126, 159]: CHERRY BLOSSOM PINK. That is the flat pink
  // slab under the far terrain, and it had been there since the pool came back.
  // Taken from the ROCK ramp itself now, so a palette reorder can never point it at somebody else's colour
  // again. A hard-coded id into a table that other people edit is the whole class of bug here.
  const STONE_ID = ROCK[0];
  // ── SLOT ALLOCATOR ── a free-list stack. A brick that goes air->occupied takes a slot, one that
  // goes occupied->air gives it back. The pool is a DERIVED CACHE of W: every write path in the game
  // still targets the dense array exactly as before, and the only thing they owe the pool is a call to
  // poolTouch() for each brick they changed. That keeps the whole migration out of gpuPatch, blitSlab
  // and the scatter shaders - they are unchanged - at the cost of re-uploading a whole 512-byte brick
  // when one voxel in it moved. Edits are sparse, so that is cheap; a streaming strip re-uploads about
  // what the dense z-band upload it replaces did anyway.
  const poolFree32 = new Int32Array(POOL_SLOTS);       // free-list stack of slot ids
  let poolFreeN = 0;
  const poolDirty = new Set();
  // ── A BRICK THAT FOUND NO SLOT IS NOT FINISHED WITH ── it used to be counted and then dropped out of the
  // dirty set, which made "the pool is momentarily full" a PERMANENT hole rather than a wait.
  // The transient is real and it is not a sizing bug. Sealing LAGS: a brick can only be sealed once its six
  // neighbours are known to be airless, and after a recentre poolBuild() runs on the starter square while
  // ~750k bricks are still to stream in. Each one arrives at the frontier with undecided neighbours, so it
  // takes a real 512-byte page; only when the neighbour lands does the re-queue below revisit it and hand the
  // page back. Peak demand during that convergence is several times the steady state (measured: 402k slots
  // used after a full build, against a 786k allocation, but 752k OVERFLOWED while streaming in behind a
  // teleport). Retrying is what turns that peak into a delay instead of a defect, and it converges because
  // every sealed brick returns a slot.
  const poolRetry = [];
  const gb2Dirty = new Set();                          // GPU-grid L2 super-cells whose 4³ of bricks changed
  // ── UPLOAD THE WORDS THAT CHANGED, NOT THE TABLE ── bdesc is 50 MB on the GPU grid, and re-sending all of
  // it on any frame that touched one brick was 11 ms of CPU a frame: measured as a 3.4 -> 10.5 ms frame-time
  // regression with the GPU passes still at 3 ms, i.e. entirely upload, not render. One u32 per brick means a
  // dirty brick is exactly one dirty word, so the same writeWordRuns coalescing the occupancy bits have used
  // all along applies unchanged.
  const descDirtyW = new Set(), gwbDirtyW = new Set();
  const gSuper = (gb) => ((gb % GBX) >> 2) + ((((gb / GBX) | 0) % GBY) >> 2) * GB2X + (((gb / (GBX * GBY)) | 0) >> 2) * GB2X * GB2Y;
  const W64p = new Float64Array(W.buffer);             // W is 8-aligned by construction (WX is a multiple of 8)
  // ── QUEUE A BRICK, AND SAY WHETHER ITS AIRLESSNESS IS ALREADY KNOWN ── `air` is -1 from every edit path
  // (dig, chop, snow, creature stamps): the brick changed, afGet must re-derive it, and that is one isAirFree
  // over 64 rows. Generated terrain arrives with the answer attached — the gen worker computes it on the
  // thread that already has the slab (see gen-pool.js) — and a streamed band is thousands of bricks at once,
  // so seeding is the difference between a band costing a scan per brick and costing a bit test per brick.
  // The neighbour propagation has to happen HERE when seeding, because poolFlush's own copy of it compares
  // afGet against airFree and a seeded brick has already made those agree — there would be nothing left to
  // notice. Six neighbours whose SEALED-ness may have flipped, exactly as poolFlush would have queued them.
  let AIRSEED = 1;                                     // dev switch, so the seeding can be A/B'd inside one session — the world reseeds on every reload
  // ══ A REGENERATED BRICK KEEPS SHOWING THE LAST PLACE'S TERRAIN UNTIL THE DRAIN REACHES IT ══ OPEN BUG,
  // and the obvious fix is a trap: measured twice, do not rebuild it.
  // W is toroidal, so when the window slides a brick slot is REUSED for a world column 2048 voxels away. The
  // streamer rewrites the voxels and queues the brick, but its descriptor still names the page it had before,
  // and poolFlush drains on a budget. Until the drain arrives the tracer walks that page and draws the OLD
  // location's voxels at the new position. gen-pool.js merges a slab with `by` INNERMOST, so bricks enter
  // poolDirty as vertical columns, and a budget cut-off leaves a contiguous vertical run stale — which draws
  // as a tall thin pillar, one or two bricks wide and many tall, standing in the world. That is the shape in
  // the reported clips, and holding the drain back (__vb.poolMs 0.05) freezes it on screen: a slab of forest
  // floor with pine trees still on it, hanging in the air over the arctic.
  // THE FIX THAT DOES NOT WORK: clearing the descriptor here, so an undrained brick renders as air rather than
  // as alien terrain. It is the rule the far ring already follows for a tile it cannot finish, and it is right
  // in principle — but a blanked brick loses its SLOT, so the drain must allocate and upload a whole fresh
  // page instead of overwriting the one it already owned, and every blank adds a descriptor upload of its own.
  // Controlled A/B, one anchor, queue drained to empty before each leg, alternating twice:
  //     peak backlog   OFF 134k / 117k     ON 2.41M / 2.34M      nineteen times WORSE, reproducibly
  // and the screenshots show the near world simply never arriving — flat empty ground out to the treeline.
  // It trades a few seconds of wrong terrain for a standing absence of terrain, which is worse.
  // WHERE TO LOOK INSTEAD: the drain walks poolDirty in INSERTION order, which has nothing to do with where
  // the player is looking. Spending the same budget NEAREST-FIRST would leave the stale bricks out at the
  // frontier where the view clamp and the distance fog already hide them, and costs no extra paging at all —
  // it changes which bricks get the budget, not how many. Untested.
  const poolTouch = (b, air) => {
    poolDirty.add(b);
    if (!AIRSEED || air === undefined || air < 0) { afDone[b] = 0; return; }
    if (airFree[b] !== air) { airFree[b] = air; for (const q of nbrOf(b)) if (q >= 0) poolDirty.add(q); }
    afDone[b] = 1;
  };
  const poolRelease = (slot) => {                      // the one place a slot goes back, so the sealed-page guard cannot be forgotten
    if (slot < 0 || slot === SEALED_SLOT || uniShared.has(slot)) return;   // shared pages (stone, and the per-id uniform ones): never freed, never reused
    if (poolFreeN < POOL_SLOTS) poolFree32[poolFreeN++] = slot;
  };
  // ── SEALED ROCK ── 40-41% of every brick in the window (measured) is rock with no air in it whose six
  // neighbours are also airless. A ray cannot reach one: to enter it, it must first cross a neighbour, and
  // the neighbour is solid, so it stops there. Those bricks all point at ONE shared slot of stone instead
  // of owning 512 bytes each. No shader change and no unreachable-brick special case: if a ray somehow did
  // enter, it reads uniform stone, which is what is actually there.
  // Regenerating one when the player digs through to it is just poolFill from W - the CPU still holds the
  // dense world, so nothing has to be re-derived from the generator.
  const airFree = new Uint8Array(BX * BY * BZ);         // 1 = brick is opaque to EVERY ray kind (see opaqueTab)
  let SEALED_SLOT = -1;                                // the one shared stone page every sealed brick points at
  // ══ AND ONE SHARED PAGE PER UNIFORM ID, FOR THE SAME REASON ══ measured with poolCensus after the pool hit
  // a sustained 99.9% in the arctic: 19.7% of all live pages were 512 identical bytes of WATER_B — four
  // hundred thousand copies of one page. Water can never take the sealed path (OPAQTAB excludes it on
  // purpose: sealing points at the STONE page, which would be the wrong content inside water), but a page
  // whose content is a single id is position-independent, so ONE copy serves every such brick bit-exactly.
  // Unlike SEALED this is not an unreachability trick — the shared page IS the brick's real content, so rays,
  // refraction and gpudiff's payload check all behave identically. uniSlotOf maps id -> slot, allocated on
  // first use and NEVER freed (poolRelease guards it like SEALED_SLOT); poolBuild resets both, because a
  // rebuild resets the allocator underneath them.
  const uniSlotOf = new Int32Array(256).fill(-1);
  const uniShared = new Set();
  const uniPage = new Uint8Array(512);
  const uniformSlot = (id) => {
    let us = uniSlotOf[id];
    if (us >= 0) return us;
    if (poolFreeN > 0) us = poolFree32[--poolFreeN];
    else if (poolUsed < POOL_SLOTS) us = poolUsed++;
    else return -1;                                    // pool full: the caller falls through to the real-page path and its own pressure handling
    uniPage.fill(id);
    device.queue.writeBuffer(poolBuf, us * 512, uniPage);
    uniSlotOf[id] = us; uniShared.add(us);
    return us;
  };
  // one brick of W, answered as "a single id, or 0" — the near-window twin of the worker's ub scan. The u32
  // packed test makes the mixed case (the overwhelming majority) exit on the first word.
  let W32P = null;
  const uniformIdOfW = (b) => {
    if (!W32P) W32P = new Uint32Array(W.buffer, W.byteOffset, W.byteLength >> 2);
    const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0;
    const w0 = W32P[((by * 8) * WX + (bz * 8) * WX * WY + bx * 8) >> 2] | 0;
    if ((((w0 & 255) * 0x01010101) | 0) !== w0 || !(w0 & 255)) return 0;
    for (let lz = 0; lz < 8; lz++) for (let ly = 0; ly < 8; ly++) {
      const rw = ((by * 8 + ly) * WX + (bz * 8 + lz) * WX * WY + bx * 8) >> 2;
      if ((W32P[rw] | 0) !== w0 || (W32P[rw + 1] | 0) !== w0) return 0;
    }
    return w0 & 255;
  };
  // ── WHAT COUNTS AS OPAQUE ── "contains no air" is NOT enough, and getting that wrong put uniform stone
  // across 17% of the screen. skipW rays (underwater eye, reflection, refraction) treat WATER as air, and
  // the FOLSKIP variant treats near foliage as air, so a brick packed solid with water or leaves is still
  // see-through to some rays - and anything it was "sealing" is reachable after all. A neighbour only seals
  // if every one of its voxels stops every ray.
  const opaqueTab = new Uint8Array(256);
  for (let i = 1; i < 256; i++) opaqueTab[i] = (i === WATER_T || i === WATER_B || foliaTab[i]) ? 0 : 1;
  const isAirFree = (b) => {
    const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0;
    for (let lz = 0; lz < 8; lz++) for (let ly = 0; ly < 8; ly++) {
      const r0 = bx * 8 + (by * 8 + ly) * WX + (bz * 8 + lz) * WX * WY;
      const a = W32[r0 >> 2], c = W32[(r0 >> 2) + 1];
      if ((a - 0x01010101) & ~a & 0x80808080) return 0;          // fast reject: any air byte at all
      if ((c - 0x01010101) & ~c & 0x80808080) return 0;
      for (let x = 0; x < 8; x++) if (!opaqueTab[W[r0 + x]]) return 0;   // …then the water/foliage test
    }
    return 1;
  };
  const nbrOf = (b) => {                               // the six face neighbours, or -1 at a window edge
    const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0;
    return [bx > 0 ? b - 1 : -1, bx < BX - 1 ? b + 1 : -1,
            by > 0 ? b - BX : -2, by < BY - 1 ? b + BX : -1,   // -2: BELOW THE WORLD, which no ray can come from — the fence may treat it as airless (measured: declining it cost 249,344 real pages of unreachable bedrock, layer by=0 at 95% occupancy with ZERO sealed)
            bz > 0 ? b - BX * BY : -1, bz < BZ - 1 ? b + BX * BY : -1];
  };
  // ── AIRLESSNESS IS DECIDED ON DEMAND, NOT WHEN A BRICK HAPPENS TO BE VISITED ── this is the difference
  // between the pool peaking at its steady size and peaking at the whole underground.
  // isSealed needs the six neighbours' airFree. Reading them straight out of the array means reading whatever
  // the last flush left there, and after a recentre that is 0 for everything that has not streamed in yet. So
  // every brick arriving at the streaming frontier looked UNSEALED, took a real 512-byte page, and only gave
  // it back much later when its neighbour finally landed and re-queued it. Measured: 752k bricks overflowed a
  // 786k pool behind one teleport, and with a 1.4 M-entry dirty queue draining at 6144 a frame the revisits
  // that would have freed the pages were hundreds of frames behind the allocations that exhausted it.
  // afDone is the fix: airFree[q] is computed the first time anyone asks for it after q was last written, and
  // poolTouch clears the flag. A brick is then sealed or not sealed correctly on its FIRST visit and a sealed
  // one never takes a page at all. The work is the same isAirFree() poolBuild already does for every brick,
  // just spread across the streaming budget instead of done in one pass.
  const afDone = new Uint8Array(BX * BY * BZ);
  const afGet = (q) => {
    if (q < 0) return 0;
    if (!afDone[q]) { afDone[q] = 1; airFree[q] = ((bricks[q >> 5] >>> (q & 31)) & 1) ? isAirFree(q) : 0; }
    return airFree[q];
  };
  const isSealed = (b) => {                            // airless AND fenced in by airless neighbours
    if (!afGet(b)) return false;
    for (const q of nbrOf(b)) { if (q === -2) continue; if (q < 0 || !afGet(q)) return false; }   // -2 = the world floor: airless by construction
    return true;
  };
  // ══ THE NEAR WINDOW'S PAGES GO UP IN SLOT RUNS TOO ══ poolFill used to be a gather AND a writeBuffer, one
  // call per brick, and POOL_BUDGET lets 6144 bricks through in a frame. Measured flying the arctic: ~600
  // separate 512-byte uploads on a spike frame, and the 'encode' phase — which is where they all land —
  // going from 0.5 ms typical to 5.6-8.3 ms on exactly those frames.
  // It is the same problem the far ring had and it takes the same answer: decide first, then sort the
  // frame's assignments by SLOT and push consecutive slots as one upload. The gather is unchanged; only the
  // call count moves. Nothing here can be deferred to a later frame the way the ring's can — a near brick is
  // in front of the player — so the batch is flushed within the same poolFlush, before the descriptors that
  // name it go up.
  const PF_CAP = 4096;
  const pfB = new Int32Array(PF_CAP), pfS = new Int32Array(PF_CAP), pfO = new Int32Array(PF_CAP);
  let pfN = 0;
  const poolFill = (b, slot) => {                     // queue brick b for the frame's batched upload
    pfB[pfN] = b; pfS[pfN] = slot; pfN++;
    if (pfN >= PF_CAP) pfUpload();
  };
  function pfUpload() {                               // gather each brick's 512 voxels out of W in x + y*8 + z*64 order, in slot order
    if (!pfN) return;
    const ord = pfO.subarray(0, pfN);
    for (let i = 0; i < pfN; i++) ord[i] = i;
    ord.sort((a, b2) => pfS[a] - pfS[b2]);
    let runS = -1, runN = 0;
    const runFlush = () => { if (runN) { device.queue.writeBuffer(poolBuf, runS * 512, ringRun, 0, runN * 512); runN = 0; runS = -1; } };
    for (let i = 0; i < pfN; i++) {
      const q = ord[i], slot = pfS[q], b = pfB[q];
      if (runN && (slot !== runS + runN || runN >= RING_RUN)) runFlush();
      if (!runN) runS = slot;
      // Row-at-a-time f64 moves, for the reason the ring's gather gives: a brick row is 8 voxels, both ends
      // are 8-aligned, and `set(subarray(...))` for eight bytes allocates and calls sixty-four times per brick.
      const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0, ro8 = runN * 64;
      for (let lz = 0; lz < 8; lz++) for (let ly = 0; ly < 8; ly++) {
        const src = bx * 8 + (by * 8 + ly) * WX + (bz * 8 + lz) * WX * WY;
        ringRun64[ro8 + ly + lz * 8] = W64p[src >> 3];
      }
      runN++;
    }
    runFlush();
    pfN = 0;
  }
  // ══ THE DRAIN WAS LOSING TO FLIGHT BY A HAIR, AND THAT IS WHERE THE STALE TERRAIN COMES FROM ══
  // At fly sprint (255 vox/s = 4.25 a frame) the window slides ~0.53 bricks a frame, and one brick-step of
  // the window is 1 x BY x BZ = 12,288 bricks. So ~6,500 bricks arrive per frame against a cap of 6,144:
  // structurally unable to keep up, and the queue grows for as long as you hold sprint. A brick sitting in
  // that queue still has its OLD descriptor, so it draws the last world column's voxels at its new position
  // (see the note on poolTouch above) — the backlog IS the artifact's lifetime.
  // MEASURED, one anchor, queue drained to empty before each leg, alternating, four legs a run, twice:
  //   6144/3ms   peak 74-97k   END 53-89k   <- never catches up: permanently behind by tens of thousands
  //   12288/6ms  peak 27-40k   END 0-26k    <- drains to zero
  //   24576/9ms  peak 28k      END 0        <- no further gain, which is what a relieved constraint looks like
  // and the TAIL did not pay for it: p99 25.1/25.1 against the shipped 27.8/26.1, 1% low 39.8/39.9 against
  // 36.0/38.4. That is the right sign and it is not luck — a queue that never catches up is saturated every
  // frame and spikes on catch-up bursts, so keeping up REMOVES pile-ups rather than adding work.
  // `avg` could not be resolved: two legs of the SAME config drifted 6.05 -> 8.77 ms, which is larger than
  // the effect under test, so do not read the averages in those runs as a comparison.
  // POOL_MS moves with it on purpose: past ~6 ms the time cap stops binding and the brick cap is the only
  // thing stopping the drain (measured: 3ms 89/75k, 6ms 60/59k, 9ms 82/88k — non-monotonic, i.e. noise once
  // the brick cap took over). Raising either one alone does nothing.
  let POOL_BUDGET = 12288;                             // bricks refreshed per frame — a `let` so __vb.poolBudget(n) can sweep it in ONE session (the world reseeds on reload, so a cross-reload sweep compares two different worlds). A recentre or a teleport dirties the whole
  // window at once (~400k bricks); doing them all in one frame is a multi-second freeze, so the queue drains
  // over ~60 frames instead and the far terrain resolves progressively. Same shape as the terrain stream budget.
  // ══ …AND A COUNT IS NOT A BUDGET, BECAUSE BRICKS ARE NOT THE SAME PRICE ══ a brick that is still sealed
  // costs a descriptor compare; one whose airlessness was invalidated pays isAirFree over 64 rows, six
  // neighbour tests, and a 512-byte gather. Measured flying the arctic, 6144 of the expensive kind is 22.65 ms
  // in one frame — and that single number was the whole of the 'encode' spike the frame-time p99 was made of
  // (the sub-timers put passes at 1.64 ms worst, getCurrentTexture at 0.10 and submit at 0.08, so it was never
  // GPU pacing). So the ceiling stays as a bound on the queue, and the real budget is TIME, checked every 256
  // bricks the way nvFlush checks its own. What does not fit rides to the next frame, which is what the queue
  // is for.
  let POOL_MS = 6;                                     // …and the time half of the same budget: 3 ms cut the drain off before it could use the brick cap above (see the note there)
  let poolDrainMax = 0, poolDrainN = 0, poolPaged = 0;
  let poolLastN = 0;                                   // bricks the LAST drain actually got through — poolDrainN is a since-boot high-water and cannot answer 'is the budget keeping up right now'
  // ══ THE BUDGET IS PER FRAME AND THE DEMAND IS PER MOVEMENT, SO A FIXED CAP IS WRONG AT EVERY OTHER FPS ══
  // Fly speed is 255 vox/s, so voxels-per-frame is frame rate restated: 4.25 at 60 fps, 8.5 at 30. The window
  // slides twice as far per frame at half the rate, so twice as many bricks arrive — against a cap that did
  // not move. MEASURED in the arctic (the densest biome: a ring tile is ~86 pages in forest and 4422 here),
  // same route, same build:
  //     4.25 vox/frame (60 fps)   dirty med 1,942     filled 1920        holeReal 0
  //     8.50 vox/frame (30 fps)   dirty med 360,900   filled 1002-1627   holeReal 13,375
  // — a 186x backlog, the view distance cut in half, and thousands of see-through bricks. overflow was +0 in
  // BOTH, so this is not pool capacity (POOL_FRAC_RING); the drain is simply being outrun.
  // SCALE ON DISTANCE MOVED, NOT ON dt. Time is the wrong variable: a player standing still on a 30 fps
  // machine creates no streaming demand at all, and would get a needlessly quadrupled drain. What actually
  // sizes the work is how far the window slid since the last drain, which is exactly what this measures.
  // Clamped to 4x — past that the machine is in trouble for other reasons, and an unbounded drain would feed
  // back (longer drain -> longer frame -> further moved -> bigger budget). EMA'd so one teleport or hitch
  // cannot spike it. Below the baseline it stays at 1x: the cap is a ceiling, never a floor to spend up to.
  let PF_ADAPT = 1;                                    // __vb.poolAdapt(0|1) A/Bs it in ONE session
  const PF_BASE = 4.25;                                // vox/frame the shipped cap was sized for — fly sprint at 60 fps
  let pfPX = null, pfPZ = null, pfMoveEma = PF_BASE, pfScale = 1;
  const pfBudget = () => {
    if (pfPX !== null) {
      const dx = P.x - pfPX, dz = P.z - pfPZ;
      const d = Math.min(64, Math.sqrt(dx * dx + dz * dz));   // 64 clamps a teleport out of the average
      pfMoveEma += (d - pfMoveEma) * 0.12;
    }
    pfPX = P.x; pfPZ = P.z;
    pfScale = PF_ADAPT ? Math.max(1, Math.min(4, pfMoveEma / PF_BASE)) : 1;
    return pfScale;
  };
  const poolFlush = (all) => {                         // called from brickFlush, so the pool lands in the same frame the occupancy bits do
    // ── THE EARLY-OUT MUST ASK ABOUT THE RING'S WORK TOO, AND THIS WAS THE FAR FIELD'S REAL BUG ──
    // ringUpdate runs immediately before this and fills descDirtyW / gwbDirtyW / gb2Dirty with the tiles it
    // just paged; the drain for all three lives at the bottom of THIS function. Guarding only on poolDirty
    // meant that on every frame the near window happened to be quiet — which is most frames once streaming
    // settles — the ring's PAGES went to the GPU (ringUpload writes those itself) while its DESCRIPTORS did
    // not. The shader then read whatever bdesc held before, which for a fresh grid is the shared sealed-stone
    // page: a flat slab of stone across the far terrain that resolved into real ground as you walked into it
    // and the near window took over. With STONE_ID's old value that slab was bright pink.
    if (!poolDirty.size && !descDirtyW.size && !gwbDirtyW.size && !gb2Dirty.size) return 0;
    let n = 0, seen = 0, stopped = 0;
    const pfK = pfBudget(), pfCap = POOL_BUDGET * pfK, pfMs = POOL_MS * pfK;
    const t0 = performance.now();
    for (const b of poolDirty) {
      if (!all && (seen >= pfCap || ((seen & 255) === 255 && performance.now() - t0 > pfMs))) { stopped = 1; break; }   // rest stays queued for next frame — cap and ms both scale with the frame interval (see pfBudget)
      seen++;
      const gb = cpu2gpu(b);
      const occ = (bricks[b >> 5] >>> (b & 31)) & 1;
      const had = bdesc[gb];
      const wob = gwb[gb >> 5];
      if ((wbricks[b >> 5] >>> (b & 31)) & 1) gwb[gb >> 5] |= 1 << (gb & 31); else gwb[gb >> 5] &= ~(1 << (gb & 31));
      if (gwb[gb >> 5] !== wob) gwbDirtyW.add(gb >> 5);
      // ── THE L2 CELL ONLY CARES WHETHER THE BRICK EXISTS, SO ONLY TELL IT WHEN THAT CHANGES ── gb2 is one bit
      // per 4x4x4 block of bricks, set if ANY descriptor in the block is non-zero, and rebuilding one cell reads
      // all 64. This used to be queued for every dirty brick: a streamed band is thousands of bricks, most of
      // them re-paging terrain that was already there, so the drain rescanned hundreds of thousands of
      // descriptors to conclude nothing had changed. A brick swapping one page for another, or an ordinary page
      // for the shared sealed one, cannot move the cell's bit — only 0 <-> non-zero can. `gb2Add` below is
      // called on exactly those transitions.
      // NEVER return SEALED_SLOT to the free list. It is SHARED by every sealed brick, so freeing it once
      // hands it out as an ordinary page and every sealed brick in the window instantly aliases whatever
      // gets written there. That is a double-free, and it shows up as corruption far from its cause.
      if (!occ) { if (had) { poolRelease(had - 1); bdesc[gb] = 0; descDirtyW.add(gb); gb2Dirty.add(gSuper(gb)); } n++; continue; }
      const wasAF = airFree[b], af = afGet(b);        // afGet recomputes only if poolTouch invalidated it, and it is what isSealed reads below
      // A neighbour whose turn has ALREADY passed this drain is not re-queued by add() -- a Set add of a
      // present entry does not move it back into line -- so it would keep a verdict computed from this
      // brick's old airlessness and be trimmed away unqueued. That looked like the source of the unqueued
      // faults and it is NOT: instrumented over four 900-frame sprint flights, the case fired ONCE in total.
      // Left as a note rather than a guard, because the guard cost a Set insert on every drained brick
      // (6144 a frame) to catch one event.
      if (af !== wasAF) { for (const q of nbrOf(b)) if (q >= 0) poolDirty.add(q); }   // my airlessness changed => my neighbours' sealed-ness may have too (theirs has not, so do NOT clear their afDone)
      const sealed = isSealed(b);
      if (sealed) {                                    // costs no payload at all - just point at the shared stone page
        if (had) { poolRelease(had - 1); }
        if (had - 1 !== SEALED_SLOT) { bdesc[gb] = SEALED_SLOT + 1; descDirtyW.add(gb); if (!had) gb2Dirty.add(gSuper(gb)); }
        n++; continue;
      }
      // …not sealed: if the whole brick is ONE id, point it at that id's shared page instead of paying for a
      // copy. Gated on airless-or-water-only so the scan runs only where uniformity is possible at all.
      const wonlyB = (wbricks[b >> 5] >>> (b & 31)) & 1;
      if (af || wonlyB) {
        const uid = uniformIdOfW(b);
        if (uid) { const us = uniformSlot(uid);
          if (us >= 0) {
            if (had && had - 1 !== us) poolRelease(had - 1);
            if (had - 1 !== us) { bdesc[gb] = us + 1; descDirtyW.add(gb); if (!had) gb2Dirty.add(gSuper(gb)); }
            n++; continue;
          } }
      }
      let slot = had - 1;
      if (!had || slot === SEALED_SLOT || uniShared.has(slot)) {   // air->occupied, a sealed brick just exposed, or an EDITED uniform brick: it needs a real page now — poolFill must NEVER write into a shared page, every brick in the window aliases it
        if (poolFreeN > 0) slot = poolFree32[--poolFreeN];
        else if (poolUsed < POOL_SLOTS) slot = poolUsed++;
        else { poolOverflow++; poolRetry.push(b); continue; }   // pool full FOR NOW: keep it queued (see poolRetry) — dropping it is what made an overflow permanent
        bdesc[gb] = slot + 1; descDirtyW.add(gb); if (!had) gb2Dirty.add(gSuper(gb));
      }
      poolFill(b, slot); poolPaged++; n++;
    }
    // …and the ones that WERE done come off the queue. By `seen`, not by `n`: a brick that found no slot took
    // its turn and went into poolRetry without incrementing n, so trimming by n left it in place to be walked
    // again next frame, at the front, forever.
    if (stopped) { let k = 0; for (const b of poolDirty) { poolDirty.delete(b); if (++k >= seen) break; } }
    else poolDirty.clear();
    // …and the ones that found no slot go straight back in. AFTER the trim/clear above, or the clear would
    // drop them again — which is exactly the bug this fixes.
    if (poolRetry.length) { for (const b of poolRetry) poolDirty.add(b); poolRetry.length = 0; }
    if (gb2Dirty.size) {                               // recompute only the super-cells a touched brick sits in
      for (const c of gb2Dirty) {
        const cx = c % GB2X, cy = ((c / GB2X) | 0) % GB2Y, cz = (c / (GB2X * GB2Y)) | 0;
        let occ = 0;
        scan2: for (let bz = cz * 4; bz < cz * 4 + 4; bz++) for (let by = cy * 4; by < cy * 4 + 4; by++) for (let bx = cx * 4; bx < cx * 4 + 4; bx++) {
          if (bdesc[bx + by * GBX + bz * GBX * GBY]) { occ = 1; break scan2; }
        }
        if (occ) gb2[c >> 5] |= 1 << (c & 31); else gb2[c >> 5] &= ~(1 << (c & 31));
      }
      gb2Dirty.clear(); device.queue.writeBuffer(gb2Buf, 0, gb2);
    }
    pfUpload();                                      // …the frame's pages, batched — and BEFORE the descriptors below, which are what makes them visible
    if (descDirtyW.size) { writeWordRuns(bdescBuf, bdesc.buffer, descDirtyW); descDirtyW.clear(); }
    if (gwbDirtyW.size) { writeWordRuns(gwbBuf, gwb.buffer, gwbDirtyW); gwbDirtyW.clear(); }
    poolLastN = seen;
    { const el = performance.now() - t0; if (el > poolDrainMax) { poolDrainMax = el; poolDrainN = seen; } }   // the worst drain SINCE BOOT, not since the last read
    return n;
  };
  const poolBuild = () => {                            // full rebuild from W - O(window), not a streaming path
    const t0 = performance.now();
    // ── AND THE RING GOES WITH IT ── poolBuild resets the slot allocator and zeroes every descriptor, so any
    // ring tile still holding (brick, slot) pairs is describing a pool that no longer exists: the very next
    // allocation hands its slots out again and two owners write one page. Dropping the residency here makes
    // the tiles regenerate, which is the only correct answer after the world underneath them was replaced.
    ringTiles.clear(); ringHanded.clear();   // …and nothing may be adopted across a rebuild: poolBuild zeroed every descriptor those tiles were relying on
    bdesc.fill(0); gwb.fill(0); gb2.fill(0); poolUsed = 0; poolOverflow = 0; poolFreeN = 0; poolDirty.clear(); gb2Dirty.clear(); descDirtyW.clear(); gwbDirtyW.clear();
    uniSlotOf.fill(-1); uniShared.clear(); W32P = null;   // the allocator under the shared pages was just reset, and W may be a fresh buffer
    const nB = BX * BY * BZ;
    for (let b = 0; b < nB; b++) airFree[b] = ((bricks[b >> 5] >>> (b & 31)) & 1) ? isAirFree(b) : 0;
    afDone.fill(1);                                    // a full build decides every brick, so afGet has nothing left to recompute until something is written again
    // Slots are handed out in order here, so the staging chunk covers slots [chunk0, poolUsed) and is flushed
    // whenever it fills. The shared stone page is slot 0, which is why the chunk is seeded with it.
    let chunk0 = 0;
    const flush = () => { if (poolUsed > chunk0) device.queue.writeBuffer(poolBuf, chunk0 * 512, poolCPU.buffer, 0, (poolUsed - chunk0) * 512); chunk0 = poolUsed; };
    SEALED_SLOT = poolUsed++;                          // slot 0 is the shared stone page every sealed brick points at
    poolCPU.fill(STONE_ID, 0, 512);
    let sealedN = 0;
    for (let bz = 0; bz < BZ; bz++) for (let by = 0; by < BY; by++) for (let bx = 0; bx < BX; bx++) {
      const b = bx + by * BX + bz * BX * BY;
      if (!((bricks[b >> 5] >>> (b & 31)) & 1)) continue;   // all air: no payload, descriptor stays 0
      const gb = cpu2gpu(b);
      if ((wbricks[b >> 5] >>> (b & 31)) & 1) gwb[gb >> 5] |= 1 << (gb & 31);
      if (isSealed(b)) { bdesc[gb] = SEALED_SLOT + 1; sealedN++; continue; }
      if (poolUsed >= POOL_SLOTS) { poolOverflow++; continue; }
      if (poolUsed - chunk0 >= POOL_CHUNK) flush();
      { const uid = uniformIdOfW(b);                   // single-id brick: share the id's page. STAGED as well as written — the bulk flush below covers [chunk0, poolUsed) from poolCPU, so a slot allocated here must have its bytes in the staging too or the flush overwrites the page with garbage
        if (uid) { const us = uniformSlot(uid);
          if (us >= 0) { if (us >= chunk0) poolCPU.fill(uid, (us - chunk0) * 512, (us - chunk0) * 512 + 512); bdesc[gb] = us + 1; continue; } } }
      const slot = poolUsed++;
      bdesc[gb] = slot + 1;
      let o = (slot - chunk0) * 512;
      for (let lz = 0; lz < 8; lz++) for (let ly = 0; ly < 8; ly++) {   // local order is x + y*8 + z*64, matching the shader
        const src = bx * 8 + (by * 8 + ly) * WX + (bz * 8 + lz) * WX * WY;
        poolCPU.set(W.subarray(src, src + 8), o + ly * 8 + lz * 64);
      }
    }
    flush();
    poolSealed = sealedN;
    for (let c = 0; c < gb2.length * 32; c++) {         // L2 from the finished descriptors, whole grid
      const cx = c % GB2X, cy = ((c / GB2X) | 0) % GB2Y, cz = (c / (GB2X * GB2Y)) | 0;
      if (cz >= (GBZ >> 2)) break;
      let occ = 0;
      scan2: for (let bz = cz * 4; bz < cz * 4 + 4; bz++) for (let by = cy * 4; by < cy * 4 + 4; by++) for (let bx = cx * 4; bx < cx * 4 + 4; bx++) {
        if (bdesc[bx + by * GBX + bz * GBX * GBY]) { occ = 1; break scan2; }
      }
      if (occ) gb2[c >> 5] |= 1 << (c & 31);
    }
    device.queue.writeBuffer(bdescBuf, 0, bdesc);
    device.queue.writeBuffer(gwbBuf, 0, gwb);
    device.queue.writeBuffer(gb2Buf, 0, gb2);
    return { slots: POOL_SLOTS, used: poolUsed, sealed: poolSealed, overflow: poolOverflow,
      denseMB: +(WX * WY * WZ / 1048576).toFixed(1),
      pooledMB: +((bdesc.byteLength + gwb.byteLength + gb2.byteLength + poolUsed * 512) / 1048576).toFixed(1),
      saving: +((WX * WY * WZ) / (bdesc.byteLength + poolUsed * 512)).toFixed(2),
      ms: Math.round(performance.now() - t0) };
  };
  // ── NOR DO THE CPU-GRID L2 AND WATER TABLES ── the shader reads gb2/gwb, which are on the GPU grid and
  // cover the far ring too. bricks2 and wbricks are still built and read on the CPU (the ceiling probe, the
  // snow leap), so the arrays stay; only their GPU buffers and the uploads that fed them are gone.
  const uploadBricks = () => { if (CPROF) cpEvt |= 4; };   // the tables it used to push have no GPU reader left; the pool's own flush is what uploads now
  // ── Z-BAND OCCUPANCY UPLOAD ── a z-band's bricks are CONTIGUOUS in both tables: the flat index is
  // bx + by*BX + bz*BX*BY (and cx + cy*B2X + cz*B2X*B2Y), with the z axis outermost. So a band that only
  // grew rows [gz0,gz1) touches exactly one slice, and the whole 384 KB table no longer has to be re-sent
  // for every 8-voxel band. BX*BY (12288) and B2X*B2Y (768) are both multiples of 32, so the bit ranges
  // land on u32 word boundaries and the slice is exact — no neighbouring bits are clipped or clobbered.
  // Only used where the caller KNOWS the change is confined to that z range (genBandGen's z path).
  const uploadBricksZ = (gz0, gz1) => {
    if (CPROF) cpEvt |= 4;
    const b0 = (gz0 >> 3) * BX * BY, b1 = ((gz1 + 7) >> 3) * BX * BY;   // L1 bit range
    const w0 = b0 >> 5, w1 = (b1 + 31) >> 5;
  };
  // Coalesce a set of dirty u32 word indices into contiguous runs — one writeBuffer per run instead of per word.
  // Bridging a small gap re-uploads a few CLEAN words, which is always safe: the CPU array is authoritative,
  // so copying more of it can only bring the GPU closer to it, never further away.
  let wrunN = 0, wrunB = 0, wrunWords = 0;             // …how the word-run coalescer is actually doing: calls, bytes, and how many words it was asked for
  // ── THE COALESCING GAP, AND IT IS STILL NOT A LEVER ── measured 2026-08-22 at ~80 calls a frame: sweeping
  // it 16 -> 1024 moved the call count 78.7 -> 76.6, inside noise. RE-MEASURED 2026-08-30 in the regime that
  // measurement did not cover — the far ring publishes descriptors for ~700 bricks a frame, scattered by GBX
  // (512 words) between brick rows, so at gap 16 nothing merges and it is 405 calls a frame of 79 bytes each,
  // landing in 'encode', the phase that spikes. Gap 512 merges a whole brick COLUMN and takes that to 22 calls
  // a frame. Frame time did not care: avg 9.06 -> 8.99, p50 7.22 -> 7.16, p99 26.09 -> 25.95, 1% low 38.3 ->
  // 38.5 — noise in every column, for 23x the bytes (31.7 KB -> 722.3 KB a frame). Two independent
  // measurements, an order of magnitude apart in call count, both say the same thing: writeBuffer call
  // overhead is not what this costs. Do not re-try it a third time.
  let WRUN_GAP = 16;
  const wrunTmp = [];
  const writeWordRuns = (buf, src, wset) => {
    if (!wset || !wset.size) return;
    wrunWords += wset.size;
    wrunTmp.length = 0;
    for (const w of wset) wrunTmp.push(w);
    wrunTmp.sort((p, q) => p - q);
    let s = wrunTmp[0], e = s + 1;
    for (let i = 1; i < wrunTmp.length; i++) {
      if (wrunTmp[i] <= e + WRUN_GAP) { e = wrunTmp[i] + 1; continue; }
      device.queue.writeBuffer(buf, s * 4, src, s * 4, (e - s) * 4); wrunN++; wrunB += (e - s) * 4; s = wrunTmp[i]; e = s + 1;
    }
    device.queue.writeBuffer(buf, s * 4, src, s * 4, (e - s) * 4); wrunN++; wrunB += (e - s) * 4;
  };
  // ── FRAME-LEVEL BRICK UPLOAD BATCHING ── gpuPatch used to run writeWordRuns three times PER CALL, and
  // gpuPatch fires from ~20 sites a frame (snow landing, worm stamps, chop, dig, pickups, melt), so the
  // same buffers were re-visited over and over. The touched words now accumulate across the WHOLE frame
  // and coalesce ONCE, in patchEncode, ahead of the trace that reads them. Identical bytes and identical
  // bits reach the GPU — only the call count changes: 47 → 37 a frame walking, 223 → 85 in a snowstorm.
  //
  // Measured honestly: that call reduction is NOT worth any frame time. A seeded, uncontended A/B put
  // tick p50 at 2.20 ms both before and after, with the other percentiles moving in both directions —
  // i.e. noise. Dawn's per-writeBuffer overhead is evidently far below the 1-3 µs that would have made
  // 138 calls matter. A follow-up that bridged the gaps BETWEEN runs to cut calls further was tried and
  // removed: it cost 7× the bandwidth (27 → 184 KB a frame) and bought nothing measurable. This is kept
  // only because one flush per frame is simpler than three per call, not because it is faster.
  const dirtyBW = new Set(), dirtyC2W = new Set();

  // ══════════════════════════════════════════════════════════════════════════════════════════════════════
  //  THE FAR RING  —  terrain beyond the CPU window, generated straight into pool pages
  // ══════════════════════════════════════════════════════════════════════════════════════════════════════
  // This is what actually buys the longer view. The pool made a WIDER GPU window affordable (see TWO WINDOWS
  // in world/window.js); it did not put anything in it. poolBuild and poolFlush only ever walk the CPU brick
  // grid, so every descriptor outside the CPU window stayed 0 = air, and GMUL 2 rendered exactly as far as
  // GMUL 1 did.
  //
  // THE RING HAS NO DENSE BACKING AND DOES NOT WANT ONE. W is 1.5 GB at 2048 and could not be grown; but the
  // ring is render-only — nothing out there collides, is edited, is walked on or spawns life — so the only
  // thing that has to exist is the 512-byte pages a ray reads. A ring tile is therefore generated into a
  // PRIVATE slab by the existing gen pool (the same generator, the same world, `ring: 1` on the job so the
  // worker hands the slab back instead of blitting it into W), paged, and the slab is dropped.
  //
  // TILES, NOT BANDS. The near window streams as 8-voxel bands because it is toroidal and a shift wraps a
  // strip. The ring is not toroidal in any useful sense — a tile is wanted or it is not — so residency is a
  // SET of world-aligned tiles, which makes both arrival and eviction ordinary set arithmetic instead of
  // rectangle bookkeeping. It also means a tile is generated exactly once for a given world position.
  //
  // EVICTION CANNOT DOUBLE-FREE, and that is the one invariant worth stating plainly, because the sealed-slot
  // note above records what happens when it does. A tile records the (brick, slot) pairs it wrote; on evict it
  // frees a slot ONLY where bdesc still holds exactly that slot. If the window moved and something else has
  // taken the descriptor, the pair is skipped. Self-correcting, and it cannot hand one page to two owners.
  const RING_TILE = 128;                               // voxels per tile side. 16x16 bricks in x/z: big enough that one worker job is worth dispatching, small enough that a tile is a fine-grained unit of arrival
  const RING_TB = RING_TILE >> 3;                      // …in bricks
  const RING_JOBS = 12;                                 // tiles generating at once. The pool has NPOOL workers and the near window's own streaming has first call on them   // …raising this to 32 for the boot fill was tried and bought NOTHING (14.1 s either way): `pend` pinned at 12 was job SATURATION, not the limiter. The limiter is the page budget below
  const RING_LAND = 3;
  let ringLandN = 3;                                   // …RING_LAND scaled by the movement EMA each frame, see ringBudget below                                 // new tiles STARTED per frame — this caps only the per-brick decide pass now; the uploads they generate are capped by RING_PAGE instead. // …and at most this many LAND per frame. One was set when a tile cost ~7 ms; the packed-word airless scan took that down far enough that the limit was starving the ring instead of protecting the frame — while flying, eviction outran refill by thirty tiles to one and the far field collapsed. Three is the balance: the fill keeps up with flight and the paging stays inside the streaming budget's own spikes
  // ── THE BOOT FILL IS NOT BUDGET-BOUND, SO DO NOT TRY TO BUY IT WITH BUDGET ── all three of these were
  // raised behind the loading overlay and measured, and NONE of them moved the load by a millisecond:
  // RING_JOBS 12 -> 32, RING_PAGE 4096 -> 65536, tiles-landing 3 -> 24. Load stayed 14.0-14.1 s every time.
  // What the trace actually shows: 12 ring jobs pending with `gen().busy` at ZERO for ~2.5 s, and then 689
  // tiles landing in the following 5 s once the workers wake. The boot wait is a gen-pool STARTUP STALL
  // followed by honest worldgen throughput — the budgets were never the constraint.
  const RING_PAGE = 4096;                              // PAGES uploaded per frame, shared across tiles — the budget that actually bounds the spike, see ringUpload. ~1.2 us a page measured, so ~1.8 ms worst case
  let ringBudget = 0;
  const ringTiles = new Map();                         // key -> { tx, tz, job, gb: Int32Array, sl: Int32Array, n, done }
  const tileLive = (tx, tz) => {                        // live descriptors under one ring tile — the adoption's own audit
    const bx0 = tx * (RING_TILE >> 3), bz0 = tz * (RING_TILE >> 3), nb = RING_TILE >> 3;
    let live = 0;
    for (let dz = 0; dz < nb; dz++) { const wz = (((bz0 + dz) % GBZ) + GBZ) % GBZ;
      for (let dx = 0; dx < nb; dx++) { const wx = (((bx0 + dx) % GBX) + GBX) % GBX;
        for (let by = 0; by < GBY; by++) if (bdesc[wx + by * GBX + wz * GBX * GBY]) live++;
      } }
    return live;
  };
  let ringAbandon = 0;                                 // tiles dropped whole because they could not be paged in full — see ringPageTile
  let ringAdoptClear = 0;                              // descriptors an evicted ADOPTED tile left behind — see ringEvict
  const adoptLive = [];                                // live-descriptor counts of recently adopted tiles — a healthy tile is thousands (see ringStats maxN)
  let ringEvictLRU = 0;                                // tiles evicted to make room for a NEARER one — see ringEvictFurthest
  let ringMaxN = 0, ringRuns = 0, ringDecMs = 0, ringLive = 0, ringPaged = 0, ringEvicted = 0, ringOverflow = 0, ringMs = 0, ringHandOver = 0, ringAdopt = 0;
  // ── TILES THE CPU WINDOW BORROWED ── and this set is what stops the far field collapsing whenever you move.
  // A tile that enters the CPU window is handed over: its pages stay in bdesc and its record is dropped. When
  // the window slides past and the world position becomes ring territory again, the ring used to see a MISSING
  // tile and queue it for regeneration — a full 144-cubed worker job for terrain that is already correctly
  // paged and has not changed, because the world is deterministic and the CPU path maintained those exact
  // descriptors the whole time it owned them.
  // The cost of not knowing that was the whole bug. The re-fetch queue sits just BEHIND the player, the view
  // clamp is a RADIUS rather than a direction, so a hole 1050 voxels behind clamped the view 1050 voxels
  // AHEAD — and every step re-opened it. Measured while flying: the filled radius fell from 1920 to ~1050 and
  // then oscillated 1022/1058/1090/1122 step after step, which is exactly the flicker.
  // Adopting instead costs nothing and is correct: the pages are already there and already right.
  const ringHanded = new Set();
  // Bounded in normal use — the scan only adds tiles the near window currently covers (~256 of them) and
  // deletes each one as it leaves. A guard rather than a leak: if the two ever fall out of step the worst case
  // is a pass of regeneration, not unbounded growth.
  const RING_HANDCAP = 4096;
  const ringKey = (tx, tz) => (tx & 0xffff) * 65536 + (tz & 0xffff);
  const EMPTY_I32 = new Int32Array(0);                 // an adopted tile owns no slots of its own — the pages it points at were paged by whoever held the descriptor before it
  // ── HOW FAR THE RING REACHES ── the GPU window, less a margin, and never further than the view actually
  // needs. Squared-off rather than circular: tiles are square and a circle would only save the corners.
  // ── AND THE REACH YIELDS TO THE POOL ── the pool is a fixed size and the arctic is the densest thing in the
  // world; measured across three fresh worlds on one fixed flight, peak residency came out 93.0%, 99.5% and
  // 100%, so whether a given world overflows is close to a coin flip. A ring that keeps asking for tiles it
  // cannot page just thrashes. Giving up a tile of reach per overflow, and taking it back once residency falls
  // well clear, turns "some worlds tear" into "dense worlds render slightly nearer" — which is the trade the
  // view clamp is already built to express.
  let ringSquash = 0;                                  // voxels of reach conceded to pool pressure
  let ringSquashWant = 0;                              // …an abandoned tile ASKS for a step; ringUpdate grants at most one a frame
  const ringReach = () => Math.max(RING_TILE * 3, Math.min((GHALF - RING_TILE) | 0, (RD_DBG || renderDist) + 96) - ringSquash);
  // ── AND IT DOES NOT EVICT AT THE SAME RADIUS IT FETCHES AT ── that is what made the view FLICKER. Fetching
  // and evicting on one boundary means a tile sitting on it is dropped and re-fetched as the player wobbles
  // across a single voxel, and since a re-fetch is a worker round trip the ring loses ground every time.
  // Measured while flying: tiles fell 689 -> 260, the filled radius collapsed from 1920 to ~1050 and then
  // OSCILLATED (1048, 1080, 1112, 1144, 1048...) — and the view clamp follows the filled radius, so the whole
  // far field pumped in and out every few frames. That oscillation IS the flicker.
  // A keep radius one and a half tiles beyond the fetch radius costs a handful of tiles of memory and turns a
  // boundary into a band. Standard streaming hysteresis; the near window's own rect has the same shape.
  // ══ KEEP MUST EXCEED FETCH, OR THE OUTER RING THRASHES ══ the prefetch fetches to R + RING_PREFETCH
  // tiles, and this used to keep to R + 1.5 tiles — the SAME boundary. A tile at the edge was therefore
  // fetched and evicted and fetched again, forever, even standing still: the acceptance gate caught it as
  // avg 3.79 -> 6.04 ms and p99 6.50 -> 20.58, with the 1% low at 48 fps. Keep now trails the fetch radius
  // by a tile and a half so the outermost ring the scan asks for is always inside the eviction boundary.
  const ringKeep = () => Math.min(GHALF | 0, ringReach() + RING_TILE * (RING_PREFETCH + 1.5));
  // ── PAGE ONE TILE OUT OF ITS SLAB ── the slab is (sx, WY, sz) in the worker's own layout, x fastest, and
  // `bb` is its 8³ occupancy scanned in the worker.
  // THE SLAB IS ONE BRICK WIDER THAN THE TILE ON EVERY SIDE, and that margin is not a nicety. Sealing asks
  // whether all six neighbours are airless, so a brick on the slab's edge has no answer and has to decline —
  // and on a 16x16x48 tile the x/z edges alone are a quarter of the bricks. Measured without the margin: 23%
  // of every ring tile took a real 512-byte page against a near-window steady state of 13%, and the pool ran
  // to 93% full with tiles still arriving. Generating 144 wide and paging the inner 128 costs 27% more
  // generation, which the pool has spare, and buys back nearly half the ring's memory.
  // RING_M is that margin in bricks; the page loop skips it, and the GPU index is taken from the tile's own
  // origin so the margin is generated, consulted, and thrown away.
  // UPLOADS ARE BATCHED. Slots come off poolUsed++ in order while the free list is empty, which is the whole
  // of a cold fill, so consecutive slots are contiguous in the buffer and a run of them is ONE writeBuffer
  // instead of one per brick. A tile is ~1600 pages; unbatched that is 1600 queue submissions a frame.
  const RING_M = 1;                                    // slab margin in bricks, so every paged brick has six real neighbours to seal against
  const ringStage = new Uint8Array(512);
  const RING_RUN = 512;                                // slots per batched upload (256 KB)
  const ringRun = new Uint8Array(RING_RUN * 512);
  const ringRun64 = new Float64Array(ringRun.buffer);   // the same staging buffer, moved a row at a time
  function ringPageTile(T, m, x0, z0) {                 // x0/z0 are the TILE's origin; the slab starts RING_M bricks before it
    const t0 = performance.now();
    const SW = m.stride, sx = m.nbx * 8, sz = m.nbz * 8, SB = m.W, bb = m.bb, wb = m.wb;
    const nbx = m.nbx, nby = m.nby, nbz = m.nbz;
    // ── THE AIRLESS SCAN IS THE EXPENSIVE PART OF A TILE, so it gets isAirFree's own trick ── a brick is
    // 64 rows of 8 bytes and reading them one at a time is 8 M byte reads per tile. `(a - 0x01010101) & ~a &
    // 0x80808080` is nonzero exactly when one of four packed bytes is ZERO, so two u32 loads reject any row
    // containing air outright, and only rows that survive that pay the per-byte water/foliage test. Rows are
    // 8-aligned here (the slab stride is a multiple of 8 and bx*8 is too), so the u32 view is safe.
    const SB32 = new Uint32Array(SB.buffer, SB.byteOffset, SB.length >> 2);
    // ── AND AN f64 VIEW FOR THE GATHER ── a brick row is exactly 8 voxels and both ends are 8-aligned (the
    // slab stride is a multiple of 8 and so is bx*8; the staging offset is ly*8 + lz*64), so one f64 store
    // moves a whole row. The obvious `dst.set(src.subarray(o, o + 8), d)` allocates a subarray object and
    // makes a call FOR EIGHT BYTES, sixty-four times per brick — on a tile of 4400 pages that is 280,000
    // allocations and calls, and it was most of the ~8 ms per frame the ring was costing.
    const SB64 = new Float64Array(SB.buffer, SB.byteOffset, SB.length >> 3);
    const nb = nbx * nby * nbz;
    const air = new Uint8Array(nb);                    // "no ray of any kind gets through this brick" — the slab's own airFree
    // ── …AND THE WORKER HAS USUALLY ALREADY ANSWERED IT ── world/gen-pool.js runs the identical scan on the
    // generating thread for ring jobs and hands back a bitmask, which unpacks in ~0.02 ms instead of the
    // 3.85 ms a tile the scan cost here. The loop below stays as the fallback for a slab that arrives without
    // one, so the two can never disagree about what sealed means: it IS the same code.
    const abW = m.ab;
    const ubW = m.ub;                                  // per-brick uniform id from the worker (0 = mixed), computed beside ab in the same slab scan
    if (abW) { for (let b = 0; b < nb; b++) air[b] = (abW[b >> 5] >>> (b & 31)) & 1; }
    else for (let b = 0; b < nb; b++) {
      if (!((bb[b >> 5] >>> (b & 31)) & 1)) continue;   // all air
      const bx = b % nbx, by = ((b / nbx) | 0) % nby, bz = (b / (nbx * nby)) | 0;
      let ok = 1;
      scan: for (let lz = 0; lz < 8; lz++) for (let ly = 0; ly < 8; ly++) {
        const r0 = bx * 8 + (by * 8 + ly) * SW + (bz * 8 + lz) * SW * WY, w = r0 >> 2;
        const a = SB32[w], c = SB32[w + 1];
        if (((a - 0x01010101) & ~a & 0x80808080) || ((c - 0x01010101) & ~c & 0x80808080)) { ok = 0; break scan; }
        for (let q = 0; q < 8; q++) if (!opaqueTab[SB[r0 + q]]) { ok = 0; break scan; }
      }
      air[b] = ok;
    }
    const gbA = new Int32Array(nbx * nby * nbz), slA = new Int32Array(nbx * nby * nbz), bA = new Int32Array(nbx * nby * nbz);
    const sgb = new Int32Array(nbx * nby * nbz), sval = new Int32Array(nbx * nby * nbz);   // the AIR (-1) and SEALED descriptor writes, held back until the tile is whole
    T.bA = bA;
    let n = 0, sn = 0, ovfN = 0;
    for (let bz = RING_M; bz < nbz - RING_M; bz++) for (let by = 0; by < nby; by++) for (let bx = RING_M; bx < nbx - RING_M; bx++) {
      const b = bx + by * nbx + bz * nbx * nby;
      const wbx = ((x0 >> 3) + bx - RING_M), wbz = ((z0 >> 3) + bz - RING_M);
      const gb = (((wbx % GBX) + GBX) % GBX) + by * GBX + ((((wbz % GBZ) + GBZ) % GBZ)) * GBX * GBY;
      // ── THE WATER MASK IS UPDATED BEFORE THE AIR EXIT, WHICH IS WHERE poolFlush ALREADY DOES IT ── gwb is one
      // bit per brick saying "everything in here is water", and skipW rays (the underwater eye, reflections,
      // refraction) stride a set brick instead of walking it. The near window writes that bit for every brick
      // it drains, occupied or not, so a brick that becomes air has its bit cleared. This path used to `continue`
      // on the air case FIRST and never reach the line below, so a far brick that had been water and became air
      // kept a stale bit set — and a stale "all water here" tells those rays to skip a brick that no longer
      // holds any. Two paging paths, one buffer, and only one of them was maintaining it.
      const wbit = wb && ((wb[b >> 5] >>> (b & 31)) & 1);
      const wob = gwb[gb >> 5];
      if (wbit) gwb[gb >> 5] |= 1 << (gb & 31); else gwb[gb >> 5] &= ~(1 << (gb & 31));
      if (gwb[gb >> 5] !== wob) gwbDirtyW.add(gb >> 5);
      if (!((bb[b >> 5] >>> (b & 31)) & 1)) { if (bdesc[gb]) { sgb[sn] = gb; sval[sn] = -1; sn++; } continue; }   // …deferred to the publish step with everything else, see below
                                                       // …gb2 only on a 0 <-> non-zero transition, for the reason poolFlush gives
      // sealed: airless, and fenced by airless neighbours INSIDE this slab (edges decline — except the WORLD
      // FLOOR: by 0 has no below-neighbour because there is nothing below the world, and no ray can arrive
      // from there. Declining it cost 249,344 real pages of unreachable bedrock, an eighth of the pool,
      // measured by poolByHist: layer by=0 at 95% occupancy with ZERO sealed.)
      let sealed = air[b] === 1;
      if (sealed) {
        if (bx === 0 || bx === nbx - 1 || by === nby - 1 || bz === 0 || bz === nbz - 1) sealed = false;
        else sealed = !!(air[b - 1] && air[b + 1] && (by === 0 || air[b - nbx]) && air[b + nbx] && air[b - nbx * nby] && air[b + nbx * nby]);
      }
      // ── WHATEVER HELD THIS DESCRIPTOR BEFORE GIVES ITS PAGE BACK ── a tile handed over to the CPU window
      // keeps its pages and drops its record (see the eviction note), and the CPU path abandons a descriptor
      // outright when the toroidal window wraps that world position away. Either way the slot has an owner in
      // bdesc and no owner in any list, so overwriting the entry without reclaiming it first LEAKS the page —
      // permanently, once per CPU/ring transition, which under sustained flight is thousands of pages.
      // Safe here because a ring tile never overlaps the CPU window: nothing else can be using this entry.
      // ALL THREE EXITS OWE THIS, not just the paged one: a brick that has become air, or become sealed, is
      // just as capable of holding someone's page, and those two only ever cleared the descriptor.
      // ══ AND THE SEALED BRICKS WAIT FOR THE REST OF THE TILE ══ this used to publish immediately, right here
      // in the decide pass, while the tile's REAL pages went up over the following frames. A tile is ~40%
      // sealed rock, so for those frames the far field showed that 40% and nothing else: a mass of bricks all
      // pointing at the ONE shared stone page, which is uniform ROCK[0] and therefore renders as a dead-flat,
      // untextured, brick-crenellated slab hanging in the world until the rest of the tile caught up and buried
      // it. That is the beige plate lying on the arctic sea in the bug clip, and the reason it comes and goes.
      // Fixing the paged bricks alone (see ringUpload) was not enough precisely because it left this asymmetry:
      // one class of brick still led, and it happened to be the class that all shares one uniform page.
      if (sealed) { if (bdesc[gb] !== SEALED_SLOT + 1) { sgb[sn] = gb; sval[sn] = SEALED_SLOT; sn++; } continue; }
      // …not sealed but SINGLE-ID (the worker's ub scan): the id's shared page is bit-exact content, so the
      // brick costs no slot. Published through the same deferred sgb/sval step as sealed, so it cannot lead
      // its tile. Falls through to a real page only if the pool cannot seat the id's one page.
      const uid = ubW ? ubW[b] : 0;
      if (uid) { const us = uniformSlot(uid);
        if (us >= 0) { if (bdesc[gb] !== us + 1) { sgb[sn] = gb; sval[sn] = us; sn++; } continue; } }
      let slot;
      if (poolFreeN > 0) slot = poolFree32[--poolFreeN];
      else if (poolUsed < POOL_SLOTS) slot = poolUsed++;
      else if (ringEvictFurthest(T)) { slot = poolFree32[--poolFreeN]; }   // …the pool is a CACHE: make room instead of failing (see ringEvictFurthest)
      else { ringOverflow++; ovfN++; continue; }        // …and nothing was further away than this tile, so there is genuinely no room: abandoned whole, below
      gbA[n] = gb; slA[n] = slot; bA[n] = b; n++;      // decided and allocated; the descriptor goes live in ringUpload, once the page behind it exists
    }
    // ══ UPLOAD IN SLOT ORDER, AND THAT IS WORTH 350x ══ the pages are written as RUNS of consecutive slots,
    // one writeBuffer per run. Allocating and uploading in BRICK order only produces runs while slots are
    // coming off `poolUsed++`, which is the cold fill. As soon as anything has been evicted the slots come off
    // the free list — a LIFO stack that a tile filled in ascending order, so it pops them back DESCENDING, and
    // a descending sequence matches no ascending run at all.
    // Measured: 0.0023 writeBuffer calls per page during the cold fill, 0.826 during flight — about seventy
    // thousand separate GPU uploads over five steps, and roughly 9 ms of main thread per frame. That is the
    // lag spike; frames hit the 50 ms telemetry clamp and the 1% low fell to 20.
    // Sorting the tile's own assignments by slot costs one sort of a few thousand ints and restores the runs
    // whichever direction the allocator handed them out in.
    // ══ A TILE THAT COULD NOT GET EVERY PAGE IS ABANDONED WHOLE ══ sealed bricks share SEALED_SLOT, so they
    // never take a pool slot and can NEVER overflow; only a tile's real pages can. Publishing what survived
    // therefore does not degrade gracefully — it publishes the sealed 40% of the tile and nothing to bury it,
    // which is a mass of bricks all pointing at the one uniform stone page: the dead-flat beige plate from the
    // bug clips, except that under exhaustion it is PERMANENT rather than a frame of upload lag.
    // A hole is the honest failure. The view clamp already understands holes — ringGap pulls the far plane in
    // front of one — so abandoning the tile costs view distance and keeps the picture true, while publishing a
    // partial tile costs nothing and lies. Dropping the record entirely makes the scan re-fetch it once the
    // pool has room, which the squash below arranges.
    if (ovfN) {
      ringAbandon++;                                   // …counted APART from ringOverflow, which fires per refused brick at the allocation site and therefore still climbs even when this path is working perfectly. overflow says "the pool was tight"; this says "a tile was dropped whole rather than published half-empty", and the two answer different questions
      for (let i = 0; i < n; i++) poolRelease(slA[i]);
      // …and it is REQUESTED, not applied. A quarter tile per overflowing TILE still stacks: measured, three
      // tiles were abandoned inside a six-frame burst and the far plane moved 59 voxels in one frame — better
      // than the 127 a whole-tile step gave, but still nearly two increments at once, because a burst of
      // contending tiles all concede on the same frame. Rate-limiting to one step per FRAME is what actually
      // bounds the visible jump, and it costs nothing: the burst simply walks the radius in over three frames
      // instead of dropping it in one.
      ringSquashWant = 1;
      T.up = null; T.done = false; if (T.job) { poolRingDrop(T.job); T.job = null; }
      ringTiles.delete(ringKey(T.tx, T.tz));
      ringDecMs += performance.now() - t0;
      return;
    }
    const oa = new Int32Array(n);
    for (let i = 0; i < n; i++) oa[i] = i;
    oa.sort((a, b2) => slA[a] - slA[b2]);
    T.gb = gbA.subarray(0, n); T.sl = slA.subarray(0, n); T.n = n;
    T.up = { oa, SB64, SW, nbx, nby, i: 0, sgb: sgb.subarray(0, sn), sval: sval.subarray(0, sn) };
    if (n > ringMaxN) ringMaxN = n;
    ringDecMs += performance.now() - t0;
  }
  // ══ THE UPLOAD IS BUDGETED IN PAGES, NOT TILES ══ "one tile a frame" is only a budget if tiles cost the
  // same, and they do not: measured, a tile averages ~86 pages across the world and reaches 4422 in the
  // arctic, where 176-voxel glaciers stand over a third of the surface. The same code under the same budget
  // therefore cost 0.12 ms a frame in a forest and 4.5 ms on the ice — thirty times over, for one tile either
  // way. Everything ABOVE this line is proportional to BRICKS (the airless scan, the decide loop) and has to
  // run in one go, because sealing needs the whole slab in hand; everything BELOW is proportional to PAGES
  // and can stop anywhere. So it stops on a page count and resumes next frame, and how long a frame takes no
  // longer depends on which biome the tile happened to land in.
  // THE DESCRIPTORS GO LIVE ALL AT ONCE, ON THE FRAME THE LAST PAGE LANDS. Two things forced that. A
  // descriptor published before its page points the tracer at whatever the slot's previous owner left there,
  // so it can never lead. But publishing each one as its page lands — which is what this did first — is just
  // as wrong for a different reason: the upload walks the tile in SLOT order, and slot order has nothing to do
  // with space, so a tile spread over several frames appears as a random scatter of bricks materialising out
  // of nothing. The atomic version got both for free and it was worth keeping: pages stream in on a budget,
  // and the tile becomes visible in one step at the end, exactly as it used to.
  // The old pages stay live and correct throughout, because a tile allocates NEW slots and only releases the
  // ones it displaces at publish time.
  const ringUpload = (T) => {
    const t0 = performance.now();
    const u = T.up, oa = u.oa, SB64 = u.SB64, SW = u.SW, nbx = u.nbx, nby = u.nby;
    const gbA = T.gb, slA = T.sl, bA = T.bA, n = T.n;
    let runS = -1, runN = 0;
    const runFlush = () => { if (runN) { ringRuns++; device.queue.writeBuffer(poolBuf, runS * 512, ringRun, 0, runN * 512); runN = 0; runS = -1; } };
    const i0 = u.i, end = Math.min(n, i0 + ringBudget);
    for (let i = i0; i < end; i++) {
      const q = oa[i], slot = slA[q], b = bA[q];
      const bx = b % nbx, by = ((b / nbx) | 0) % nby, bz = (b / (nbx * nby)) | 0;
      if (runN && (slot !== runS + runN || runN >= RING_RUN)) runFlush();   // a slot that does not continue the run ends it
      if (!runN) runS = slot;
      const ro8 = runN * 64;                           // …in f64 words: 512 bytes is 64 of them
      for (let lz = 0; lz < 8; lz++) for (let ly = 0; ly < 8; ly++) {
        const src = bx * 8 + (by * 8 + ly) * SW + (bz * 8 + lz) * SW * WY;
        ringRun64[ro8 + ly + lz * 8] = SB64[src >> 3];
      }
      runN++;
    }
    runFlush();
    u.i = end; ringPaged += end - i0; ringBudget -= end - i0;
    if (end >= n) {                                    // …every page is on the GPU: publish the whole tile in one step
      for (let i = 0; i < n; i++) {
        const gb = gbA[i], slot = slA[i], had0 = bdesc[gb];
        if (had0 && had0 - 1 !== slot) poolRelease(had0 - 1);
        bdesc[gb] = slot + 1; descDirtyW.add(gb); if (!had0) gb2Dirty.add(gSuper(gb));
      }
      for (let i = 0; i < u.sgb.length; i++) {         // …and the air/sealed ones in the SAME step, or they lead
        const gb = u.sgb[i], v = u.sval[i], had0 = bdesc[gb];
        if (had0 && had0 - 1 !== v) poolRelease(had0 - 1);
        bdesc[gb] = v + 1; descDirtyW.add(gb);
        if (!had0 !== !(v + 1)) gb2Dirty.add(gSuper(gb));   // only a 0 <-> non-zero transition moves the L2 bit
      }
      T.up = null; T.done = true; poolRingDrop(T.job); T.job = null;
    }
    ringMs += performance.now() - t0;
  };
  // ══ WHEN THE POOL IS FULL, EVICT THE FURTHEST TILE RATHER THAN REFUSE THE NEAREST ══
  // A player's own flight recorder (F9, 12 s of ordinary play at ~157 fps, fly cruise) caught this exactly:
  //     pool occupancy 87.6% climbing to 100.0% of 2,139,096 slots
  //     one frame at 2,137,757 live refused 1,534 allocations and ABANDONED A TILE WHOLE
  // A dropped tile is 128 voxels of terrain vanishing, which is the reported flashing. It fits every clue:
  // only while moving (the pool fills as new ground streams), arrived with the doubled view distance (which
  // is what sized the demand), and worst in the arctic, where a tile costs ~4422 pages against ~86 in forest.
  // Short harness flights from a fresh boot peak at 71-90% and never see it, which is why this was measured
  // as "not pool capacity" earlier and wrongly dismissed.
  // The pool is a CACHE. Refusing a nearby tile while holding pages for one far behind the player is the
  // wrong trade in every case, and raising POOL_FRAC_RING to buy headroom costs ~308 MB of VRAM on machines
  // this game now ships to. So: find the furthest resident tile, and if it is genuinely further from the
  // player than the one asking, evict it and hand over its pages. Returns true when it freed something.
  // Only tiles FURTHER than the caller are eligible, so this cannot cycle: each eviction strictly improves
  // the set, and when nothing is further away the old refusal path still runs.
  function ringEvictFurthest(want) {
    const px = Math.round(P.x), pz = Math.round(P.z);
    const dOf = (tx, tz) => { const cx = tx * RING_TILE + (RING_TILE >> 1), cz = tz * RING_TILE + (RING_TILE >> 1);
      return Math.max(Math.abs(cx - px), Math.abs(cz - pz)); };
    const mine = dOf(want.tx, want.tz);
    let far = null, fd = mine;
    for (const [, T] of ringTiles) {
      if (T === want || T.up || !T.done) continue;     // never take pages from a tile mid-upload or still generating
      const d = dOf(T.tx, T.tz);
      if (d > fd) { fd = d; far = T; }
    }
    if (!far) return false;
    ringEvict(far);
    ringTiles.delete(ringKey(far.tx, far.tz));
    ringEvictLRU++;
    return poolFreeN > 0;
  }
  function ringEvict(T) {
    // A tile can leave the ring part-way through its upload now. Its unpublished slots are allocated but named
    // by nothing in bdesc, so the descriptor test below would skip them and leak the pages; free them by hand.
    // ALL of them, not just the ones past the cursor: a tile mid-upload has published NOTHING (the descriptors
    // go live in one step at the end, see ringUpload), so every slot it holds is named by nothing in bdesc and
    // the descriptor test below would skip the lot.
    if (T.up) { for (let j = 0; j < T.n; j++) poolRelease(T.sl[j]); T.up = null; if (T.job) { poolRingDrop(T.job); T.job = null; } }
    // ══ AN ADOPTED TILE OWNS DESCRIPTORS AND NO LIST, AND LEAVING THEM LIVE IS THE FLICKER ══ a tile adopted
    // back from the near window is recorded as done with n = 0 and an EMPTY gb array, because its pages were
    // written by poolFlush and the ring never allocated them. The loop below is therefore a no-op for it: on
    // eviction it frees nothing and, far worse, CLEARS NOTHING. Its descriptors stay live pointing at pages
    // nobody owns any more.
    // gb names a world position MODULO the GPU grid (4096 voxels), so those stale descriptors keep drawing the
    // terrain of a place the player has left, at the wrapped position — pieces of landscape that should not be
    // there, appearing in the far field and vanishing again as the real tile for that cell finally lands on top
    // of them. That is the reported flicker, and it only became reachable when the view distance doubled and
    // the ring started adopting and evicting tiles in bulk.
    // Safe to sweep the whole block: a tile only reaches ringEvict when it is OUT of the keep box (>= 2048
    // away), so it cannot overlap the CPU window, and every gb here belongs to this tile alone.
    // …and the safety condition is CHECKED PER TILE, because the blanket version was both wrong and silently
    // self-disabling. `ringKeep() * 2 < GWX` looks like the right invariant and is false by exactly one voxel:
    // ringKeep clamps to GHALF, GHALF is GWX/2, so the test is 4096 < 4096 and the sweep never ran once —
    // adoptClear sat at 0 through 488 evictions and read as "nothing to reclaim" rather than "never executed".
    // The real question is not how far the ring spans, it is whether ANOTHER LIVE TILE shares this gb block.
    // gb wraps every GBX bricks, so the only tiles that can collide are the ones exactly one wrap away in x or
    // z — TPW tiles, and no others. Ask ringTiles about those eight directly: exact, eight map lookups, and it
    // keeps working if the view distance ever grows.
    const TPW = GBX / (RING_TILE >> 3);
    let aliased = false;
    if (!T.n) { for (let ax = -1; ax <= 1 && !aliased; ax++) for (let az = -1; az <= 1; az++) {
      if (!ax && !az) continue;
      if (ringTiles.has(ringKey(T.tx + ax * TPW, T.tz + az * TPW))) { aliased = true; break; } } }
    if (!T.n && !aliased) {
      const bx0 = T.tx * (RING_TILE >> 3), bz0 = T.tz * (RING_TILE >> 3), nb = RING_TILE >> 3;
      for (let dz = 0; dz < nb; dz++) { const wz = (((bz0 + dz) % GBZ) + GBZ) % GBZ;
        for (let dx = 0; dx < nb; dx++) { const wx = (((bx0 + dx) % GBX) + GBX) % GBX;
          for (let by = 0; by < GBY; by++) {
            const gb = wx + by * GBX + wz * GBX * GBY, d = bdesc[gb];
            if (!d) continue;
            poolRelease(d - 1); bdesc[gb] = 0; descDirtyW.add(gb); gb2Dirty.add(gSuper(gb)); ringAdoptClear++;
          } } }
    }
    for (let i = 0; i < T.n; i++) {                     // ONLY where the descriptor still names this tile's own slot — see the note above
      const gb = T.gb[i], sl = T.sl[i];
      if (bdesc[gb] === sl + 1) { poolRelease(sl); bdesc[gb] = 0; descDirtyW.add(gb); gb2Dirty.add(gSuper(gb)); }
    }
    ringEvicted++;
  }
  // ── ONE PASS A FRAME ── what the window wants, what it holds, and one step toward agreement. Deliberately
  // cheap when GMUL is 1: the whole subsystem is one compare away in that case, because there is no ring.
  let ringErr = null;                                  // ── A THROW IN HERE IS SILENT ── this runs inside brickFlush, inside the frame, and a ReferenceError there leaves a perfectly rendered stale frame with the ring simply never advancing (see [[voxelbit-tick-throw-silent]]). Recorded so __vb.ring() can say so.
  function ringUpdate() { try { ringUpdate_(); } catch (e) { if (!ringErr) { ringErr = String(e && e.stack || e).slice(0, 400); console.error('[vb] ring:', e); } } }
  function ringUpdate_() {
    if (GMUL <= 1 || !poolOk) return;
    // ══ FETCH A RING FURTHER OUT THAN THE VIEW IS ALLOWED TO SEE ══ the wanted set was exactly the view
    // radius, and both are quantised to RING_TILE, so every time the player crosses a 128-voxel tile line a
    // whole ROW of tiles entered the wanted set AT ONCE, all of them missing. ringGap is the distance to the
    // nearest missing tile, so it collapsed to that row in a single frame and the far plane jumped with it.
    // MEASURED, sprint flight, sampled every frame in-page: tile events land on 23 of 699 frames and never
    // singly — 31 evictions and 17 adoptions in ONE frame — with |dFilled| hitting 97 voxels in a frame and
    // a p99 of 77. That is a chunk-level discontinuity about twice a second, only while moving, quantised to
    // the ring's own tile size. It is the reported "flashing", and it exists only because the ring exists,
    // which is why it arrived with the doubled view distance.
    // Damping the plane cannot fix it: the drop is REQUIRED, because rendering past a tile that has not been
    // fetched shows sky. (Tried anyway — a rate limiter on the advance moved the swing 330 -> 367, noise, and
    // cost 60 voxels of average view distance. Reverted.)
    // So fetch a tile-and-a-half FURTHER than the view may reach. The row is then already resident when the
    // box advances onto it, the gap never opens, and the plane never jumps. The view clamp still uses R, so
    // this buys stability without ever showing ground the ring has not got.
    const R = ringReach(), K = ringKeep();
    const RF = R + Math.round(RING_TILE * RING_PREFETCH);   // FETCH radius — what the scan asks for. __vb.ringPrefetch(0|1) A/Bs it in ONE session (the world reseeds on reload, so a cross-reload comparison is two different worlds)
    const px = Math.round(P.x), pz = Math.round(P.z);
    const t0x = Math.floor((px - RF) / RING_TILE), t1x = Math.floor((px + RF) / RING_TILE);
    const t0z = Math.floor((pz - RF) / RING_TILE), t1z = Math.floor((pz + RF) / RING_TILE);
    const k0x = Math.floor((px - K) / RING_TILE), k1x = Math.floor((px + K) / RING_TILE);
    const k0z = Math.floor((pz - K) / RING_TILE), k1z = Math.floor((pz + K) / RING_TILE);
    // ══ TWO OWNERS OF ONE DESCRIPTOR IS THE ONE THING THAT CANNOT HAPPEN ══ and the first cut let it, which
    // is what produced the pink-and-cyan garbage across the arctic: `near` used HALF - RING_TILE, so a tile
    // whose centre sat between 896 and 1024 voxels from the player was NOT dropped even though the CPU window
    // (half-width HALF = 1024) already covered it. Both paths then wrote the same bdesc entry, each freed the
    // other's slot on its next pass, and pages ended up serving bricks from somewhere else entirely.
    // The test is now the EXACT rectangle overlap against the window's real extent — winOX/winOZ, not the
    // player, because the window snaps to 32 and lags the player by up to that much.
    // AND THE TWO EVICTIONS ARE NOT THE SAME EVICTION:
    //   * OUT OF REACH — the tile is gone from the GPU window. Free its slots and clear its descriptors.
    //   * OVERTAKEN BY THE CPU WINDOW — the tile's pages are still exactly right (same world, same content);
    //     only the OWNER changes. Clearing them would blank 128 voxels of world that the near path has no
    //     reason to re-touch (streaming only regenerates the 8-voxel strip that wrapped), so the tile is
    //     simply forgotten and bdesc keeps the pages. poolFlush then treats them like any other descriptor it
    //     finds — releasing and reallocating on the first change — so nothing leaks either.
    // ══ EVICTION IS RATE-LIMITED, BECAUSE A BURST OF IT IS THE VISIBLE FAULT ══
    // Caught in a player's own flight recorder, in the two frames before they pressed the key on a "flash":
    //     idx 714  dt 23.9 ms  28 tiles evicted  poolLive -65,004 pages in ONE frame
    //     idx 715  dt  2.4 ms  moved 5.04        (the long frame carried the camera four times as far)
    //     idx 716  dt 23.9 ms  17 more evicted   dirty 35,977 appears
    // 45 tiles retired across two frames is 45 chunks of far field dropped at once, and their replacements
    // are not back for several frames. `filled` moved by 2-4 voxels through all of it, so the view clamp was
    // never the problem — the terrain simply left.
    // And it FEEDS ITSELF: clearing descriptors for 45 tiles is what makes the frame 24 ms, a 24 ms frame
    // carries the player four times the usual distance, and that pushes the next batch out of the keep box
    // all at once. Rate-limiting breaks the loop at its cheapest point.
    // Safe to defer: everything here is already OUTSIDE the keep box, which is beyond anything the view clamp
    // will let a ray reach, so a tile lingering a few frames costs only the pages it is still holding — and
    // the pool now evicts the furthest tile on demand if it actually needs them (see ringEvictFurthest).
    const EVICT_MAX = 6;                                 // tiles retired per frame
    let evicted = 0;
    for (const [k, T] of ringTiles) {
      const x0t = T.tx * RING_TILE, z0t = T.tz * RING_TILE;
      const out = T.tx < k0x || T.tx > k1x || T.tz < k0z || T.tz > k1z;   // the KEEP box, not the fetch box — see ringKeep
      const ovl = x0t < winOX + WX && x0t + RING_TILE > winOX && z0t < winOZ + WZ && z0t + RING_TILE > winOZ;
      if (!out && !ovl) continue;
      if (out && !ovl && ++evicted > EVICT_MAX) continue;   // …over budget this frame: it stays, harmlessly, until the next. A HANDOVER (ovl) is not deferred — the CPU window is about to own those descriptors and two owners is the one thing that cannot happen
      // …AND `!done` NO LONGER MEANS `owns nothing`. It used to: a tile was either still generating or fully
      // paged, so dropping the job was the whole reclaim. A tile part-way through its upload is neither — it
      // holds allocated slots that bdesc does not name yet, which ringEvict knows how to give back and
      // poolRingDrop does not. It cannot be handed to the CPU window either: half its pages are published and
      // half are not, and "the pages are already right" is the entire premise of a handover.
      if (T.up) ringEvict(T);
      else if (!T.done) poolRingDrop(T.job);
      else if (out) ringEvict(T);                      // gone from the window: reclaim
      else ringHandOver++;                             // handed to the CPU window: keep the pages, drop the record. The scan below is what remembers it (see ringHanded)
      ringTiles.delete(k);
    }
    // …and take one step toward filling what is missing, nearest first so the view grows outward
    let jobs = 0; for (const [, T] of ringTiles) if (!T.done) jobs++;
    // one step a frame, in either direction — the request from any number of abandoned tiles collapses to a
    // single increment, and release is the same size, so the radius moves at one rate and only one rate.
    if (ringSquashWant) { ringSquash = Math.min(ringSquash + (RING_TILE >> 2), ((GHALF - RING_TILE) | 0) - RING_TILE * 3); ringSquashWant = 0; }
    else if (ringSquash > 0 && poolUsed - poolFreeN < POOL_SLOTS * 0.86) ringSquash = Math.max(0, ringSquash - (RING_TILE >> 2));   // …handed back gradually, so the radius eases out rather than snapping
    // ══ AND THE RING'S OWN BUDGETS SCALE WITH MOVEMENT TOO ══ the near drain was fixed for this (see
    // pfBudget) and the ring was left on fixed per-frame caps, which is the same mistake in the other half:
    // EVICTION scales with speed and REFILL did not, so at sprint the ring loses tiles and the view plane
    // it feeds collapses. MEASURED in the arctic, same route, tiles held / evict per second / filled:
    //     standing still   672 tiles    0 evict/s   filled 1920 stable
    //     fly cruise 1.5   703 tiles   44 evict/s   filled 1920 PINNED
    //     fly sprint 4.25  543 tiles  119 evict/s   filled 1614-1788, oscillating ~174 voxels
    // A view plane swinging 174 voxels is distant terrain culled and restored over and over — the reported
    // "flashing terrain", which the user confirmed happens ONLY while flying and never standing still, and
    // which arrived with the doubled render distance because that is when this ring started existing.
    // Same scale as the drain, from the same EMA of distance moved, so the two halves cannot disagree.
    ringBudget = Math.round(RING_PAGE * pfScale);
    ringLandN = Math.max(RING_LAND, Math.round(RING_LAND * pfScale));
    for (const [, T] of ringTiles) {                    // …anything already part-way through goes FIRST: it is holding a slab checked out and a hole in the view
      if (T.up) { ringUpload(T); if (ringBudget <= 0) break; }
    }
    let landed = 0;
    for (const [, T] of ringTiles) {                    // then start what the frame still has room for
      if (ringBudget <= 0 || landed >= ringLandN) break;
      if (T.done || T.up || !T.job || !T.job.done) continue;
      ringPageTile(T, T.job.msg, T.tx * RING_TILE, T.tz * RING_TILE);
      jobs--; landed++;                                // …its slab stays checked out until ringUpload has finished with it
      ringUpload(T);
    }
    // ── ONE SCAN, TWO ANSWERS: what to fetch next, and where the first HOLE is ── they come from the same
    // pass because they are the same question asked twice. A tile counts as missing whether it has never been
    // asked for or is still generating, and the nearest missing one is what bounds the view (ringGap): a ray
    // may not be allowed past it, or it would walk into descriptors that are still zero and render sky.
    // Chebyshev, not Euclidean, for the gap — tiles are squares and the view clamp is a radius, so the
    // distance that matters is to the nearest FACE of the hole, not to its centre.
    let best = null, bestD = 1e18;
    // ── HOLDING THE VIEW A TILE INSIDE THE FETCH RADIUS DOES NOT HELP EITHER ── the idea was that an edge
    // row still arriving would then be beyond what a ray can reach, so the gap could never collapse onto it,
    // and unlike RING_PREFETCH it holds no extra tiles. Measured, in-session ABBA: mean per-frame jump 4.67
    // WITH it against 1.3 without (after the first warm-up leg), i.e. worse, and p99 barely moved. Reverted.
    ringGap = R;
    for (let tz = t0z; tz <= t1z; tz++) for (let tx = t0x; tx <= t1x; tx++) {
      const cx = tx * RING_TILE + (RING_TILE >> 1), cz = tz * RING_TILE + (RING_TILE >> 1);
      const x0t = tx * RING_TILE, z0t = tz * RING_TILE;
      const k = ringKey(tx, tz);
      // ── WHAT THE CPU WINDOW COVERS IS ALREADY PAGED, AND THAT IS THE WHOLE ADOPTION RULE ── every brick
      // inside the near window has a live descriptor: the streaming path poolTouch()es whatever it writes and
      // poolBuild covers a recentre, so nothing in there is unpaged. Mark it while it is covered; when it
      // stops being covered, its pages are still correct, because the world is deterministic and nothing out
      // there has changed. Adopt it instead of spending a 144-cubed worker job regenerating it.
      // THE FIRST VERSION MARKED ONLY TILES THE RING ITSELF HANDED OVER, and that was the bug: measured while
      // flying, `adopted` stayed 0 while the scan hit 3100 handed-and-still-covered tiles. The ring was so far
      // behind that most tiles crossed the near window WITHOUT EVER HAVING BEEN RING-OWNED, so there was
      // nothing to hand over and nothing to adopt — and every one of them came out the back as a fresh hole.
      // Those holes sit just behind the player, and the view clamp is a radius, so they clamped the view AHEAD.
      // ── AND THE PREMISE HAS BEEN TESTED, BECAUSE IT IS A CLAIM ABOUT A QUEUE ── poolFlush drains on a
      // budget, so "covered by the near window" could in principle mean "queued but not yet paged", and
      // adopting a tile in that state would certify a HOLE as filled: done:true with n:0, never fetched again,
      // and nothing in the near window maps back to those descriptors. It would be a permanent see-through
      // patch at mid distance, visible only while moving — which is exactly what a rendering complaint looks
      // like, so it was worth ruling out rather than assuming.
      // MEASURED (adoptLive in ringStats, the live-descriptor count under an adopted tile): 4544-7489 per
      // tile, median ~5600-6800, and not one sample below 200 over two runs. Adopted tiles are fully paged.
      // The alternative — only marking on a frame where poolDirty is empty — was built and A/B'd in the same
      // session and takes adoption to ZERO (272 marks lost, 0 adopted per run), because the queue is almost
      // never empty under sustained flight. It is every tile re-fetched through a worker for no defect. The
      // rule below stands as written; adoptLive is left in place so the premise keeps being checked.
      if (x0t < winOX + WX && x0t + RING_TILE > winOX && z0t < winOZ + WZ && z0t + RING_TILE > winOZ) {
        if (ringHanded.size < RING_HANDCAP) ringHanded.add(k); continue; }
      let T = ringTiles.get(k);
      if (!T && ringHanded.has(k)) {                   // …it has just left the near window, on a frame where the pool was drained: already paged, so it is filled
        ringHanded.delete(k); ringAdopt++;
        if ((ringAdopt & 7) === 0) { if (adoptLive.length >= 64) adoptLive.shift(); adoptLive.push(tileLive(tx, tz)); }   // …every 8th, count what it actually adopted. A tile of real terrain pages thousands of bricks (ringStats maxN); an adopted tile that comes back near zero is a hole being certified as filled
        T = { tx, tz, job: null, gb: EMPTY_I32, sl: EMPTY_I32, n: 0, done: true };
        ringTiles.set(k, T);
      }
      if (T && T.done) continue;                       // filled
      const cheb = Math.max(Math.abs(cx - px), Math.abs(cz - pz)) - (RING_TILE >> 1);
      if (cheb < ringGap) ringGap = cheb;
      if (T) continue;                                 // already on its way
      const d = (cx - px) * (cx - px) + (cz - pz) * (cz - pz);
      if (d < bestD) { bestD = d; best = [tx, tz, k]; }
    }
    // ── AND DO NOT RESERVE JOB SLOTS FOR THE VISIBLE RING ── tried: a prefetch tile could only start while
    // 4 of the 12 slots stayed free for tiles inside R. It recovered view distance (1465 -> 1580 average) and
    // made the TAIL WORSE — p99 57 -> 87, max 90 -> 121 — because a starved prefetch row is not ready when
    // the box crosses onto it, so the gap opens anyway AND the slots were spent. Unreserved is strictly
    // better on smoothness (p99 46-49). Reverted.
    while (jobs < RING_JOBS && best) {                  // keep the pool fed: one scan, several dispatches
      const M = RING_M * 8;
      const j2 = poolRingJob(best[0] * RING_TILE - M, best[0] * RING_TILE + RING_TILE + M,
                             best[1] * RING_TILE - M, best[1] * RING_TILE + RING_TILE + M);
      if (!j2) break;
      ringTiles.set(best[2], { tx: best[0], tz: best[1], job: j2, gb: null, sl: null, n: 0, done: false });
      jobs++;
      best = null; bestD = 1e18;                       // re-scan for the next nearest, now that this one is taken
      for (let tz = t0z; tz <= t1z; tz++) for (let tx = t0x; tx <= t1x; tx++) {
        const cx = tx * RING_TILE + (RING_TILE >> 1), cz = tz * RING_TILE + (RING_TILE >> 1);
        const x2 = tx * RING_TILE, z2 = tz * RING_TILE;
        if (x2 < winOX + WX && x2 + RING_TILE > winOX && z2 < winOZ + WZ && z2 + RING_TILE > winOZ) continue;
        if (ringTiles.has(ringKey(tx, tz))) continue;
        const d = (cx - px) * (cx - px) + (cz - pz) * (cz - pz);
        if (d < bestD) { bestD = d; best = [tx, tz, ringKey(tx, tz)]; }
      }
    }
  }
  // ── HOW FAR THE RING HAS ACTUALLY FILLED ── the view clamp reads this, so outrunning the ring shortens the
  // view smoothly exactly as outrunning the near stream already does, instead of showing a wall of air.
  // IT IS THE NEAREST HOLE, NOT THE FURTHEST TILE, and the difference is not academic: tiles arrive
  // nearest-first, but after a teleport a handful of far ones can still be resident while everything between
  // is gone. Taking the maximum reported 2032 voxels of fill with 972 tiles just evicted — a view licensed to
  // walk straight through the gap and out the other side, where every descriptor is zero and a ray finds sky.
  // ringGap is computed by the residency scan above, which already has to visit every wanted tile.
  let ringGap = 0;
  // ── THE VIEW PLANE DOES NOT STROBE, SO DO NOT DAMP IT ── ringGap's RANGE over a few seconds of flight is
  // 93-527 voxels, which looks alarming and is not a strobe: sampled EVERY FRAME in-page it moves at most
  // ~9 voxels per frame, i.e. the horizon drifts, it does not jump. A rate limiter on the advance was built
  // and measured anyway: swing 330 -> 367 (noise) while the average plane fell 1509 -> 1449. It costs view
  // distance and buys nothing. Reverted; do not rebuild it without per-FRAME evidence of an actual jump.
  function ringFilled() { return GMUL <= 1 ? HALF : Math.max(HALF - RING_TILE, ringGap); }
  // ══ FLIGHT RECORDER ══ the streaming faults that read as "flashing terrain" last a frame or two and only
  // happen while MOVING, so nothing that has to be set up in advance can catch one: by the time a harness is
  // pointed at the right place the event is over, and reproducing a player's exact conditions in a headless
  // window has repeatedly disagreed with what the player actually sees. So the game records itself, always,
  // into a fixed ring buffer, and a key dumps the last few seconds AFTER the fact — press it when you SEE
  // something and the numbers for the moment before are already captured.
  // Cost is one typed-array write per field per frame into a preallocated buffer: no allocation, no branch
  // on a debug flag, nothing to remember to turn on. REC_N frames at 60 fps is about 12 seconds.
  const REC_N = 720, REC_W = 14;
  const recBuf = new Float32Array(REC_N * REC_W);
  let recI = 0, recFilled = 0, recPX = 0, recPZ = 0, recT = 0;
  let REC_ON = 1;                                      // __vb.recOn(0|1) — A/B the recorder's own cost in ONE session
  const recTick = () => {
    if (!REC_ON) return;
    const now = performance.now();
    const dx = P.x - recPX, dz = P.z - recPZ;
    const moved = recT ? Math.min(999, Math.sqrt(dx * dx + dz * dz)) : 0;
    const o = (recI % REC_N) * REC_W;
    recBuf[o] = recI;                       recBuf[o + 1] = recT ? Math.min(999, now - recT) : 0;
    recBuf[o + 2] = P.x;                    recBuf[o + 3] = P.y;
    recBuf[o + 4] = P.z;                    recBuf[o + 5] = moved;
    recBuf[o + 6] = ringFilled();           recBuf[o + 7] = ringTiles.size;
    recBuf[o + 8] = ringEvicted;            recBuf[o + 9] = ringAdopt;
    recBuf[o + 10] = ringOverflow;          recBuf[o + 11] = ringAbandon;
    recBuf[o + 12] = poolDirty.size;        recBuf[o + 13] = poolUsed - poolFreeN;
    recI++; recPX = P.x; recPZ = P.z; recT = now;
    if (recFilled < REC_N) recFilled++;
  };
  // …and the read-out, oldest first, with the per-frame DELTAS already differenced so the interesting
  // columns (how far the view plane jumped, how many tiles turned over) can be read without post-processing.
  const recDump = () => {
    const F = ['frame', 'dtMs', 'x', 'y', 'z', 'moved', 'filled', 'tiles', 'evicted', 'adopted', 'overflow', 'abandoned', 'dirty', 'poolLive'];
    const rows = [], n = recFilled, start = recI - n;
    let pf = null, pe = null, pa = null;
    for (let k = 0; k < n; k++) {
      const o = ((start + k) % REC_N) * REC_W, r = {};
      for (let j = 0; j < REC_W; j++) r[F[j]] = +recBuf[o + j].toFixed(2);
      r.dFilled = pf === null ? 0 : Math.round(r.filled - pf);
      r.dEvicted = pe === null ? 0 : Math.round(r.evicted - pe);
      r.dAdopted = pa === null ? 0 : Math.round(r.adopted - pa);
      pf = r.filled; pe = r.evicted; pa = r.adopted;
      rows.push(r);
    }
    return rows;
  };
  const recOnSet = (v) => { if (v !== undefined) REC_ON = v ? 1 : 0; return { REC_ON }; };
  const ringStats = () => { let pend = 0, jdone = 0, ready = 0; const some = [];
    for (const [, T] of ringTiles) { if (T.job) { pend++; if (T.job.done) jdone++; } if (T.done) ready++;
      if (some.length < 4) some.push([T.tx, T.tz, !!T.job, T.job ? !!T.job.done : null, T.done]); }
    return Object.assign({ pend, jdone, ready, some }, ringStats0()); };
  // ══ DOES THE RING STILL OWN WHAT IT THINKS IT OWNS? ══ every fault that shows as scattered geometry in the
  // far field is two owners of one descriptor, and the ring is the only party that keeps a record of what it
  // wrote: (gb, slot) pairs per tile. Walking them against bdesc is therefore the one check that can see the
  // far field at all — poolAudit reaches the near window through cpu2gpu and structurally cannot look out here.
  //   stale  the ring wrote this descriptor and something else has since overwritten it. Its page is still on
  //          the ring's books, so the ring will free it on eviction and pull the rug from whoever took it.
  //   zero   the ring wrote it and it is now 0: that brick is a HOLE, and the tile still reports done.
  // A handed-over tile is excluded by construction — those keep their pages and drop their record, so there is
  // nothing to check. An ADOPTED tile has n = 0 and is invisible to this too, which is worth remembering.
  const ringOwnAudit = () => {
    let checked = 0, stale = 0, zero = 0, tiles = 0, badTiles = 0;
    const ex = [];
    for (const [, T] of ringTiles) {
      if (!T.done || !T.n) continue;
      tiles++; let bad = 0;
      for (let i = 0; i < T.n; i++) {
        const gb = T.gb[i], want = T.sl[i] + 1, got = bdesc[gb];
        checked++;
        if (got === want) continue;
        bad++; if (got === 0) zero++; else stale++;
        if (ex.length < 6) ex.push({ tx: T.tx, tz: T.tz, gb, want: want - 1, got: got - 1 });
      }
      if (bad) badTiles++;
    }
    return { tiles, badTiles, checked, stale, zero, ex };
  };
  // ══ DRIVE THE SQUASH DIRECTLY, BECAUSE OVERFLOW CANNOT BE ORDERED ══ the far plane pops, and the two
  // candidate causes — the reach conceding to pool pressure, and a tile being abandoned whole — only ever
  // happen TOGETHER, because an abandon is what asks for a concession. Four runs of correlation therefore
  // cannot separate them: squashMax 256/96/0/0 against worstDrop 127/59/6/7 fits either story exactly.
  // Nor can the experiment be ordered on demand — overflow is a cold-fill transient and three identical cold
  // runs gave 2144, 4399 and 0 refusals, so "fly until it overflows" is not an instrument.
  // This moves ringSquash with no overflow, no abandon and no eviction anywhere in the picture. If `filled`
  // tracks it one-for-one then the reach is the pop and the rate limiter is the fix; if `filled` does not
  // move, the reach is innocent and the abandon owns it. One tap answers the question the flights could not.
  const ringSquashSet = (v) => { const b4 = ringFilled(); ringSquash = Math.max(0, Math.min(((GHALF - RING_TILE) | 0) - RING_TILE * 3, v | 0));
    return { squash: ringSquash, reach: ringReach(), filledBefore: b4, gap: ringGap, abandoned: ringAbandon, overflow: ringOverflow }; };
  const poolBudgetSet = (v) => { if (v !== undefined) POOL_BUDGET = Math.max(256, v | 0); return { POOL_BUDGET, POOL_MS }; };
  // ══ PREFETCH IS OFF BY DEFAULT: IT FIXES THE POP AND COSTS A FIFTH OF A FRAME ══
  // The pop it targets is real and measured: the wanted-tile box is quantised to RING_TILE, so crossing a
  // tile line admits a whole ROW at once, all missing, and ringGap collapses to it. Sampled every frame at
  // fly sprint: tile events land on 23 of 699 frames and never singly (31 evictions, 17 adoptions in ONE
  // frame), with |dFilled| reaching 97 voxels in a single frame. That is a chunk-scale discontinuity about
  // twice a second, only while moving — the reported "flashing", and it arrived with the doubled view
  // distance because that is when this ring began to exist.
  // Fetching beyond the view radius genuinely fixes it. In-session ABBA, two independent worlds:
  //     mean per-frame jump 5.01 -> 3.39 and 4.65 -> 1.70   p99 66-68 -> 46-49   jumps>40 17 -> 6.8-12.8
  // and it causes NO pool overflow at any depth (the only refusals measured happened with it OFF).
  // BUT THE ACCEPTANCE GATE, WHICH RUNS AT REST, SAYS NO: avg 3.79 -> 6.04 ms and p99 6.50 -> 20.58 with the
  // fetch radius at the old keep boundary, and avg 8.80 / p99 20.43 after widening keep to clear it — the
  // wider keep holds ~44% more tiles and each one costs pages and scan work every frame, which in the arctic
  // is ~4400 pages a tile. The flight measurements could not see any of this because they measure the ring,
  // not the frame. EVERY ring change needs BOTH.
  // Left switchable rather than deleted: the mechanism and the numbers are right, only the price is wrong.
  // __vb.ringPrefetch(1.5) turns it on for anyone who wants to look at the trade.
  let RING_PREFETCH = 0;                               // TILES fetched beyond the view radius; 0 = ship default
  const ringPrefetchSet = (v) => { if (v !== undefined) RING_PREFETCH = Math.max(0, Math.min(6, +v || 0)); return { RING_PREFETCH }; };
  const poolAdaptSet = (v) => { if (v !== undefined) PF_ADAPT = v ? 1 : 0; return { PF_ADAPT, movePerFrame: +pfMoveEma.toFixed(2), scale: +pfScale.toFixed(2), effCap: Math.round(POOL_BUDGET * pfScale) }; };
  const ringStats0 = () => ({ err: ringErr, evictLRU: ringEvictLRU, squash: ringSquash, abandoned: ringAbandon, adoptClear: ringAdoptClear, own: ringOwnAudit(), adoptLive: adoptLive.slice(), drainMs: +poolDrainMax.toFixed(2), drainN: poolDrainN, nearPaged: poolPaged, dirty: poolDirty.size, wrunN, wrunB, wrunWords, decMs: ringDecMs, runs: ringRuns, maxN: ringMaxN, mul: GMUL, tiles: ringTiles.size, handOver: ringHandOver, adopted: ringAdopt, handed: ringHanded.size, paged: ringPaged, evicted: ringEvicted,
    overflow: ringOverflow, filled: Math.round(ringFilled()), gap: Math.round(ringGap), reach: ringReach(), ms: +ringMs.toFixed(0),   // `gap` is the RAW distance to the nearest unfilled tile; `filled` is that with the HALF - RING_TILE floor applied. They differ exactly when the floor is lying, which is what the view clamp has to care about.
    poolUsed, free: poolFreeN, poolSlots: POOL_SLOTS });   // poolUsed is a HIGH-WATER MARK — slots ever handed out, never decremented by poolRelease — so it climbs to POOL_SLOTS in any long run and says NOTHING about how full the pool is. LIVE occupancy is poolUsed - free, which is what the squash below tests and what anyone outside must use. A day was spent reading the high-water mark as residency.
  poolTouchHook = poolTouch;                           // terrain.js and gen-pool.js run BEFORE this fragment, so they reach the pool through this hook rather than naming it
  poolBuild();                                         // …and the world they already filled predates the hook, so the first pool comes from a full derive. This is where uploadWorld() used to push 1.5 GB.
  const brickFlush = (all) => {
    ringUpdate();                                      // ── THE FAR RING ── lands its tiles' descriptors in the same dirty sets poolFlush drains below, so a ring tile and a near edit upload in one pass rather than two
    poolFlush(all);
    if (dirtyBW.size) { dirtyBW.clear(); }             // the CPU tables these tracked no longer upload; the sets are kept because gpuPatch still fills them and clearing is what bounds them
    if (dirtyC2W.size) { dirtyC2W.clear(); }
  };
  // Scratch sets reused by gpuPatch, which used to allocate four Sets per call. Do NOT read this as a GC
  // fix: Chrome's sampling heap profiler puts the engine's REAL JS garbage at ~0.6 MB per 20 s, and only
  // 1 of 40 logged frame spikes was GC-attributed (36 were the terrain-stream budget). __vb.ft()'s
  // allocMBs/gcFrames look alarming but derive from usedJSHeapSize, which counts WebGPU staging memory,
  // so they mostly report upload traffic rather than garbage. This is tidiness, not a measured win.
  // Verified non-reentrant first: supPush, nvTouch and phWakeNear never call back into gpuPatch.
  const pgBset = new Set(), pgC2set = new Set();
  // ── DID THIS BATCH LEAVE ANYTHING IN THE BRICK? ── one byte per brick, set while gpuPatch walks the cells
  // and CLEARED as that brick is processed, so it never needs a sweep. It exists to skip the occupancy
  // rescan: see the note at that loop in world/patch.js. BX*BY*BZ bytes = ~3 MB at the 2048x384 window,
  // against worldMB's 1536 — the same ratio as the brick bitmask it stands beside.
  const pgBocc = new Uint8Array(BX * BY * BZ);
  // Which palette ids belong to a GRID-STAMPED creature (mammals + perched songbirds). Rides in the unused
  // 4th float of each palette entry, so the tracer can ask "is this voxel part of an animal?" for free. The
  // hit flash needs it: its box is an AABB, and an AABB around an animal also contains the grass between its
  // legs and the ground under its belly — which is the red square the user saw stamped on the terrain.
  // Declared HERE, above palSync, so palSync can never read it through the temporal dead zone.
  const CREA_FLAG = new Uint8Array(256);
  const palBuf = device.createBuffer({ size: 256 * 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST, mappedAtCreation: true });
  { const f = new Float32Array(palBuf.getMappedRange());           // sRGB → linear; shaders work in linear
    for (let i = 0; i < palette.length; i++) { const p = palette[i]; f[i * 4] = Math.pow(p[0] / 255, 2.2); f[i * 4 + 1] = Math.pow(p[1] / 255, 2.2); f[i * 4 + 2] = Math.pow(p[2] / 255, 2.2); f[i * 4 + 3] = 0; } }
  palBuf.unmap();
  const palSync = () => { const f = new Float32Array(256 * 4);     // asset-editor imports addCol at RUNTIME — re-upload the whole palette (COPY_DST above exists for this)
    for (let i = 0; i < palette.length; i++) { const p = palette[i]; f[i * 4] = Math.pow(p[0] / 255, 2.2); f[i * 4 + 1] = Math.pow(p[1] / 255, 2.2); f[i * 4 + 2] = Math.pow(p[2] / 255, 2.2); f[i * 4 + 3] = CREA_FLAG[i]; }   // .a = grid-stamped-creature flag (see CREA_FLAG) — every pose builder re-syncs after registering its ids
    device.queue.writeBuffer(palBuf, 0, f); };
  const itemMapBuf = device.createBuffer({ size: Math.max(16, itemMapF32.byteLength), usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST, mappedAtCreation: true });   // COPY_DST: turning the arrow rewrites the bow strip's block in place (bowRefit)   // held/drop item voxel colors — see pickWGSL
  new Float32Array(itemMapBuf.getMappedRange()).set(itemMapF32); itemMapBuf.unmap();
  // ── DROP SLOTS ── 128: 25 fixed (4 dropped items, the editor cardinal, 20 death-burst sparks/smoke) +
  // the drawn flock + every trace-injected creature. 128 is exactly four 32-bit tile-mask words and exactly
  // 8 bits of slot id in the SVGF slot texture; a 129th slot needs a fifth word AND a wider id field.
  // The array's SECOND HALF (64..127) is appended at the END of the uniform struct as dropsB/lifeMotB —
  // growing `drops` in place would have shifted every hardcoded UF index past it (1092…1875) and fed each
  // field its neighbour's numbers with no error. dropOff/lifeMotOff below are the only places that know.
  const UNI_BIRDS = false;   // == WHAT ?uni ACTUALLY MOVES (user 2026-08-06) == only the THREE moving land mammals: bunny, armadillo, porcupine.
  // The 180 perched songbirds stay GRID-STAMPED and always will: they never move, so trace-injection buys them nothing, while stamped they get the exact SVGF/GI
  // solution for free. Measured why it matters: tracing them cost 14 slots and took FISH from 32/32 down to 7/32 and dragonflies to 0/3, because 24 traced mammals
  // plus 14 traced birds is 38 slots of new demand against 17 of headroom. Dropping the birds pays for the mammals outright. The SKUNK now traces too (user 2026-08-06) - all four land mammals are on the same path, and the songbirds alone pay for it.
  // budget reason - it is the asset-editor object, and the three world mammals are what the player watches move.
  // The line this draws is not ‘all life renders identically’ but ‘everything that MOVES renders identically’, which is the property that was actually producing bugs.
  const LIFE_UNI = !location.search.includes('nouni');   // == UNIFIED LIFE RENDERING: ON (user 2026-08-06, folded in) == was opt-in behind ?uni while it was measured; it now ships. ?nouni turns it off again - kept deliberately, because this is the first change here that alters how the game LOOKS in normal play, and being able to A/B it in one session is exactly how the slot-starvation regression (fish 32/32 -> 7/32) was caught before it shipped.   // == UNIFIED LIFE RENDERING (default OFF) == when set, the perched songbirds and the four land mammals TRACE-INJECT like every other creature and the secondary rays (sun + AO) test creature models, which is what closes the lighting gap the grid stamp was covering. Everything it adds is compiled in from here, so the OFF build is the shader we ship today.
  const UNI_SEC_DEF = 1 | 2 | 8;                      // the shipping config the cost study recommended: 1 creatures in the SUN ray, 2 creatures in the AO ray, 8 suppress the legacy 16-box u.cshad list (4 = water reflect/refract, deliberately NOT set: the intersector returns distance only, so it buys a dark silhouette rather than a reflection)
  const UNI_SEC_URL = (location.search.match(/[?&]sec=([0-9]+)/) || [])[1];   // ?uni&sec=N selects the config at BUILD time now that it is folded; window.__SEC (set before the page loads) does the same for the CDP harness
  const UNI_SEC = !LIFE_UNI ? 0 : (window.__SEC !== undefined ? (window.__SEC | 0) : (UNI_SEC_URL !== undefined ? (UNI_SEC_URL | 0) : UNI_SEC_DEF));   // == COMPILE-TIME == every ray test below folds to a literal here. It used to be read per pixel out of u.lifeCfg.z, which kept creaSec AND the legacy 16-box cast-shadow loop alive inside TRACE behind a dynamic branch: measured +0.169 ms at 1024x576 with every ray switched off, ~16% of the whole pass for a feature doing nothing. A whole-pass percentage is register pressure, not work, and the cure is to not compile the dead paths at all.
  const UNI_RAY = LIFE_UNI && (UNI_SEC & 3) !== 0;   // does ANY secondary ray look at creatures? If not, the intersector, its SEC_R radius, the grown tile mask and the 4 extra VIS words per tile are all skipped and VIS_W halves back to 4
  const UNI_CSHAD = (LIFE_UNI && (UNI_SEC & 8) !== 0) ? 'false' : ('LG(2u) && ' + (location.search.includes('nocshad') ? 0 : 1) + ' == 1 && sunV > 0.0 && cSlot == 0u');   // the legacy 16-box creature cast-shadow test, as a literal: under ?uni the real creature models cast the shadow instead, and emitting `false` deletes the loop rather than branching past it every pixel
  const VIS_W = UNI_RAY ? 8 : 4;                     // u32 words of tile mask per 8x8 tile: 4 = the primary mask alone, 8 = primary (0-3) + the SECONDARY grown mask (4-7)
  const UNI_CONST = UNI_RAY ? 'const SEC_R : f32 = 40.0;' : '';   // how far a SECONDARY ray looks for creatures, and the radius the VIS prepass grows each slot sphere by - the two MUST be the same number or a ray goes looking at slots the tile mask never listed. 40 is the AO ray's own 24 rounded up past a ground creature's cast-shadow reach, and it is the cutoff the 16-box cshad list already used.
  const UNI_FN = !UNI_RAY ? '' : `
    fn creaSec(roW : vec3<f32>, rdW : vec3<f32>, maxT : f32, w0 : u32, w1 : u32, w2 : u32, w3 : u32, dropN : i32) -> vec2<f32> {
      let rel = roW - u.camPos;
      let roC = vec3<f32>(dot(rel, u.right), dot(rel, u.up), dot(rel, u.fwd));
      let rdC = vec3<f32>(dot(rdW, u.right), dot(rdW, u.up), dot(rdW, u.fwd));
      var bt = maxT; var best = -1.0; var mov = 0.0;
      for (var di = 4; di < dropN; di++) {
        if (di >= 5 && di <= 24) { di = 24; continue; }
        { let mw = select(select(w0, w1, di >= 32), select(w2, w3, di >= 96), di >= 64); let mrem = mw >> (u32(di) & 31u); if (mrem == 0u) { di = i32(u32(di) | 31u); continue; } if ((mrem & 1u) == 0u) { di += i32(countTrailingZeros(mrem)) - 1; continue; } }
        let mvv = lifeMotV(di);
        if ((u32(mvv.w + 0.5) & 1u) != 0u) { continue; }
        let dXv = dropV(di * 4 + 1);
        let dit = i32(dXv.w + 0.5);
        if (dit < 1) { continue; }
        let dA = dropV(di * 4);
        let it3 = clamp(dit - 1, 0, ITEMN - 1);
        let eW = ITEMD[it3].x; let eD = ITEMD[it3].y; let eH = ITEMD[it3].z; let eOff = ITEMD[it3].w;
        if (eW < 1) { continue; }
        let vsD = dA.w;
        let ew2 = f32(eW) * 0.5; let ed2 = f32(eD) * 0.5; let eh2 = f32(eH) * 0.5;
        let radD = vsD * (sqrt(ew2 * ew2 + ed2 * ed2 + eh2 * eh2) + 1.0);
        let oc = dA.xyz - roC;
        let tcD = dot(oc, rdC);
        if (tcD < -radD || tcD - radD > bt) { continue; }
        if (dot(oc, oc) - tcD * tcD > radD * radD) { continue; }
        let dYv = dropV(di * 4 + 2); let dZv = dropV(di * 4 + 3);
        let ro0 = roC - dA.xyz;
        let roD = vec3<f32>(dot(ro0, dXv.xyz), dot(ro0, dYv.xyz), dot(ro0, dZv.xyz)) / vsD + vec3<f32>(ew2, ed2, eh2);
        var rdD = vec3<f32>(dot(rdC, dXv.xyz), dot(rdC, dYv.xyz), dot(rdC, dZv.xyz));
        if (abs(rdD.x) < 1e-6) { rdD.x = 1e-6; }
        if (abs(rdD.y) < 1e-6) { rdD.y = 1e-6; }
        if (abs(rdD.z) < 1e-6) { rdD.z = 1e-6; }
        let invD = 1.0 / rdD;
        let taD = -roD * invD;
        let tbD = (vec3<f32>(f32(eW), f32(eD), f32(eH)) - roD) * invD;
        let tnD = min(taD, tbD); let tfD = max(taD, tbD);
        let teD = max(max(tnD.x, tnD.y), max(tnD.z, 0.0));
        let tlD = min(min(tfD.x, tfD.y), tfD.z);
        if (teD >= tlD) { continue; }
        var vcD = clamp(vec3<i32>(floor(roD + rdD * (teD + 1e-4))), vec3<i32>(0), vec3<i32>(eW - 1, eD - 1, eH - 1));
        let istD = vec3<i32>(sign(rdD));
        var vNxD = (vec3<f32>(vcD + max(istD, vec3<i32>(0))) - roD) * invD;
        var tHit = teD;
        var iMapD = eOff + vcD.x + vcD.y * eW + vcD.z * eW * eD;
        for (var i = 0; i < PICKSTEPS; i++) {
          if (ITEMMAP[u32(iMapD)].w > 0.99) {   // OPAQUE only, exactly as in the primary trace - a half-transparent wing does not stop a sun or AO ray either, so what you see and what the light sees still agree
            let tw = tHit * vsD;
            if (tw > vsD * 0.5 && tw < bt) { bt = tw; best = tw; mov = select(0.0, 1.0, dot(mvv.xyz, mvv.xyz) > 4e-4); }
            break;
          }
          if (vNxD.x <= vNxD.y && vNxD.x <= vNxD.z) { tHit = vNxD.x; vNxD.x += abs(invD.x); vcD.x += istD.x; iMapD += istD.x; }
          else if (vNxD.y <= vNxD.z) { tHit = vNxD.y; vNxD.y += abs(invD.y); vcD.y += istD.y; iMapD += istD.y * eW; }
          else { tHit = vNxD.z; vNxD.z += abs(invD.z); vcD.z += istD.z; iMapD += istD.z * eW * eD; }
          if (tHit * vsD > bt) { break; }
          if (any(vcD < vec3<i32>(0)) || any(vcD >= vec3<i32>(eW, eD, eH))) { break; }
        }
      }
      return vec2<f32>(best, mov);
    }
  `;
  const DROP_SLOTS = 128, DROP_HALF = 64;             // DROP_HALF = how many slots live in the original 'drops' array; every slot at or above it lives in 'dropsB'
  const PHYS_MAX = 256;                               // ── RIGID BODY CAPACITY (user 2026-08-22, was 128/48/24, was 16) ── the ONE number. PH.maxBodies takes it, physB's WGSL length is 5 vec4 x it, and EVERY offset below is derived from it.
  //   physB sits in the MIDDLE of the struct, so raising this moves the entire tail (physC, physBound, heldCfg, lgt, hurtB, hurtH, dropsB, lifeMot, dof). Those used to be literals in tick-emit/tick-camera/debug-api; they are named now, because a
  //   missed literal here does not throw - it silently feeds each field its neighbour's numbers (a wrong hit-flash box, held-item lighting reading the light-debug mask). Never write a literal float index at or past UF_PHYSB again.
  const PHYS_GRP = 16, PHYS_NG = 16;   // bodies per group sphere, and how many groups — 16 * 16 = 256 = PHYS_MAX. BOTH LITERALS ON PURPOSE: tools/lint-vb.py evaluates the constants in this file to check every ${} array length in the uniform struct, and its parser takes + - * ( ) only, so a PHYS_MAX / PHYS_GRP here would silently stop the struct being checked at all. The guard below is what keeps them honest instead.
  if (PHYS_GRP * PHYS_NG < PHYS_MAX) console.error('[vb] PHYS_NG is too small for PHYS_MAX: bodies past ' + (PHYS_GRP * PHYS_NG) + ' would sit in no group sphere and bodyTrace would never reach them');
  const UF_PHYSB = 1532;                              // physB base - everything BEFORE it is fixed history (see UF_OLD_LEN)
  const UF_PHYSC = UF_PHYSB + PHYS_MAX * 20;          // x = live body count, y = reactive strength
  const UF_PHYSBOUND = UF_PHYSC + 4;                  // sphere enclosing every live body - the one-compare reject
  const UF_HELDCFG = UF_PHYSBOUND + 4;   // heldCfg base: x = sun visibility, y = sky visibility, z = STACKBADGE count (was spare). Named, not inlined, so the badge does not depend on counting floats in a struct other work is actively appending to.
  const UF_LGT = UF_HELDCFG + 4;                      // light-debug bitmask / water reflection strength
  const UF_HURTB = UF_LGT + 4, UF_HURTH = UF_HURTB + 4;   // the knife's red hit-flash box, then its half-extents (+3 = the drop slot the wounded animal is drawn in)
  const UF_DROPSB = UF_HURTH + 4, UF_LIFEMOTB = UF_DROPSB + (DROP_SLOTS - DROP_HALF) * 16;
  const UF_DOF = UF_LIFEMOTB + (DROP_SLOTS - DROP_HALF) * 4;   // ── DEPTH OF FIELD ── x = focus distance (voxels; 0 = off), y = max CoC radius in canvas px. LAST in the struct, so no existing offset moves.
  // ── FLOATING HEARTS ── the health readout is five real voxels hanging in front of the eye (see the heart
  // block in COMPOSITE), so it needs a lane of its own: heart = {anchor.xyz in CAMERA space, voxel scale},
  // heartC = {item id, health in hearts, the gap between two of them, hurt kick 0..1}. Appended at the very
  // END, after dof, for the reason every field back here is: the JS writes this buffer at fixed float
  // indices, so a field inserted anywhere above silently feeds every one below it its neighbour's numbers.
  const UF_HEART = UF_DOF + 4;
  // ── THE HURT FLASH ── x = strength 0..1, y = the per-hit dither seed. Appended after heartC, which is the
  // rule every field back here follows: the JS writes this buffer at fixed float indices, so a lane inserted
  // anywhere above silently feeds every one below it its neighbour's numbers. See hurtV in the struct in PRE.
  const UF_HURTV = UF_HEART + 8;
  // ── RAIN SKY ── (user 2026-08-17: "when it rains can you make the sky more cloudy and darken the clouds as
  // well … also when it rains the sun should dim a bit") ── ONE 0..1 scalar drives the whole thing, and this is
  // where it rides: u.hurtV.w, the LAST float of the last vec4 of the whole uniform struct.
  //
  // WHY THERE, and not a lane of its own. Every field back here carries the same warning — the JS writes this
  // buffer at fixed float indices, so a field INSERTED anywhere shifts every one below it and silently feeds
  // each its neighbour's numbers — and the usual answer is to append a fresh vec4 at the end. That was not open
  // here: the struct those fields are DECLARED in lives in render/wgsl/pre.js, which this change does not own,
  // so a new field would have been a JS offset with no shader field behind it. u.misc, the obvious home, is full
  // in all four lanes (x cinematic vignette, y/z the storm edges, w the eye-inside-a-voxel fill) and every one
  // of them is actively read. hurtV.w is genuinely spare and provably so: UF is a zero-initialised Float32Array,
  // tick-camera writes only hurtV x/y/z (flash strength, per-hit dither seed, standing heart level), BLIT reads
  // only those three, and pre.js documents .w as spare. Being the FINAL float in the buffer also means taking it
  // cannot move anything: there is nothing below it. It costs no bytes and no bandwidth — UF already ends here.
  const UF_RAINK = UF_HURTV + 3;
  // ── STACK-BADGE PLACEMENT ── x/y = the CANVAS PIXEL the badge starts at (the held model's own projected
  // top-right corner, plus the panel's nudge), z size, w tilt.
  // A FRESH vec4 appended after hurtV rather than a borrowed lane, because hurtV has no spare float
  // left (see the rain note above) — and appending is free here for the reason the note gives: there is
  // nothing below it to shift. ui/hud.js owns the numbers, main/tick-camera.js writes them, BLIT reads them.
  const UF_BADGE = UF_HURTV + 4;
  // ── THE HUNGER LEVEL (user 2026-08-19: hunger re-introduced, drawn as GOLD pixels) ── a fresh vec4 appended
  // after badge, for the reason every lane back here is appended rather than borrowed: the JS writes this buffer
  // at fixed float indices, so a lane inserted anywhere above silently feeds every one below it its neighbour's
  // numbers. badge is full (x/y pixel, z size, w tilt) and hurtV has been full since UF_RAINK took its last
  // float, so there was nothing to borrow. Appending is free here — there is nothing below it to shift.
  // x = vitGoldLevel() 0..4. y/z/w spare, and deliberately: the next screen-space readout gets them without
  // moving anything, which is exactly the property this end of the buffer exists to have.
  const UF_VITG = UF_BADGE + 4;
  // ── THE CRAFT PREVIEW'S HAND (user 2026-08-19, the STONE AGE bench) ── a THIRD held-item lane, laid out
  // exactly like pickA/X/Y/Z and pick2A/X/Y/Z so COMPOSITE's hand loop reads it with the same four lines: A =
  // anchor xyz in camera space + voxel size, X/Y/Z = the item's axes, and X.w = the show id (0 = hidden).
  // Appended at the END rather than beside pick2, because everything from UF_PHYSB down is written at fixed
  // float indices and inserting a lane in the middle silently feeds every field below it its neighbour's
  // numbers. Sixteen floats is the price of the preview being a real voxel model rather than a 2D icon.
  const UF_PICK3 = UF_VITG + 4;
  // ── AND THE OFF-HAND'S STACK BADGE (user 2026-08-19: "the rock in the left hand doesnt have the stack
  // number") ── the same four numbers `badge` carries for the right hand: x/y = the canvas PIXEL the glyphs
  // start at, z = size, w = tilt. It needs its own because both are per-ITEM and the two hands hold different
  // things. The COUNT does not need a lane of its own — it goes in vitG.y, which that block reserved as spare
  // for exactly this: 'the next screen-space readout gets them without moving anything'.
  const UF_BADGE2 = UF_PICK3 + 16;
  // ── AND THE FIVE TUNING NUMBERS, HERE rather than beside snowOn in ui/settings.js ── COMPOSITE_SRC is invoked
  // from render/wgsl/vis.js, manifest line 35; ui/settings.js is line 60. A const declared with the storm state
  // is therefore in its temporal dead zone when the shader template interpolates it, and the symptom is the one
  // documented in the house rules: the bundle builds, the linter passes, and the game boots to a black screen
  // with nothing useful in the console. Anything the WGSL reads has to be declared at or above line 35; this
  // file is line 26. RAIN_SUN_DIM is read by BOTH sides (the shader's sunTintR and tick-camera's heldCfg.x), so
  // one declaration is also what stops the hand and the world disagreeing about how bright the sun is.
  const RAIN_CLOUD_DARK = 0.45;    // …and the deck falls to 55% of its fair-weather brightness. Applied to the march's ACCUMULATOR rather than inside the loop — acc is a linear sum in the per-step colour, so a uniform scale on that colour is exactly a uniform scale on acc, and doing it once after the loop costs one multiply per sky pixel instead of one per step.
  const RAIN_SKY_DESAT = 0.35;     // the blue BETWEEN the clouds goes 35% of the way to its own luminance…
  const RAIN_SKY_DIM = 0.20;       // …and 20% down with it. This pair is also the only handle this change has on the sun DISC and its glare, which are drawn inside skyColor() in pre.js — a file it does not own — so it is the literal reading of "the sun should dim a bit". At night it is what puts the stars and the moon behind the overcast.
  const RAIN_SUN_DIM = 0.35;       // the DIRECT sun at 65% of normal: about the half-stop of key light a thin overcast costs, which reads as flatter contrast rather than as evening. It rides on sunTint() — the ILLUMINANT'S colour — and deliberately NOT on dayScale(), the global light level: a cloud deck kills the direct beam and leaves the sky-diffuse term nearly intact, and that split is exactly what makes an overcast scene look FLAT instead of DARK. Scaling dayScale() would have taken the ambient, the water scatter, the ice, the bounce and the held item down together — "night", not "a bit" — and dayScale() lives in pre.js besides.
  // HOW MANY HEARTS THE BAR IS. Five, which is what the removed DOM readout drew and therefore what the
  // player already knows: VIT_HP_MAX is 20, so one heart is 4 HP. Read by BOTH sides — tick-camera turns
  // hp into hearts with it, and COMPOSITE's loop bound is interpolated from it — so the two cannot disagree.
  const HEART_N = 5;
  // WHERE THE ROW HANGS, in CAMERA space and voxel units - the same space the held item's pickA lives in, so
  // z 1.10 means "11 cm in front of the eye" exactly as the axe's 0.96 does. Centred under the crosshair and
  // low, which is the one part of the frame nothing else claims: the right hand sits at x +0.91, the second
  // rock at -0.75, and a swing drives the tool to the MIDDLE of the screen, well above this. At vs 0.055 and
  // z 1.10 a heart subtends ~3.4% of the window height (~30 px at 865) and the whole bar ~12% of its width -
  // big enough to read without a glance, small enough that the view is untouched.
  // `rig` = the SHARE OF THE HAND'S BOB the row rides (heldBob, written in tick-camera). It is the other half
  // of "in the game like the stone tools": a readout nailed dead still to the screen while the tool beside it
  // sways is a HUD layer no matter how it is lit. Not 1.0, and the reason is geometric rather than taste — the
  // hand's walk sway is +-0.075 in x, and on a row five hearts wide that is 1.5 whole heart-gaps of side-to-side
  // slosh; on a single tool the same number reads as a stride. Half of it keeps the row plainly attached to the
  // player and still readable at a sprint. The idle breathing term is inside heldBob too, and at +-0.011 it is
  // untouched by the halving either way.
  const HEART_POSE = { x0: -0.200, y: -0.52, z: 1.10, vs: 0.055, gap: 0.100, rig: 0.5 };   // x0 = the FIRST heart, so x0 = -gap*(HEART_N-1)/2 keeps the row centred
  let heartShow = 1;                                  // __vb.hearts(false) hides the row - the A/B lever for what the block costs, and the only way to take a clean screenshot of the frame without it
  // ── RIGID-BODY GROUP SPHERES ── one bounding sphere per PHYS_GRP consecutive bodies, so bodyTrace can skip a
  // whole slab of the debris on one compare instead of rejecting 16 bodies one at a time. Bodies are published
  // NEAREST-FIRST (main/tick-emit.js), so a group is a depth slab and the groups themselves come front-to-back.
  // Appended at the VERY end, after badge2, for the reason every field back here is: the JS writes this buffer at
  // fixed float indices, so a lane inserted above would silently feed each field below it its neighbour's numbers.
  const UF_PHYSG = UF_BADGE2 + 4;
  // ── THE CLOUD CLOCK ── appended after physG, which is the rule every field back here follows: the JS
  // writes this buffer at fixed float indices, so a lane inserted anywhere above would silently feed each
  // field below it its neighbour's numbers. x = cloud time in seconds; y/z/w spare.
  const UF_CLOUDT = UF_PHYSG + PHYS_NG * 4;
  // ── DECLARED HERE, NOT IN world/window.js, AND THAT IS NOT A PREFERENCE ── tools/lint-vb.py resolves the
  // WGSL struct's array lengths by evaluating the top-level integer constants of THIS FILE and no other
  // (uf_consts), so `array<vec4<f32>, ${RIP_N}>` in PRE is only checkable if RIP_N lives in buffers.js.
  // Everything that pushes a ring — the splash, the player, the ducks — is later in the manifest than
  // this file, so nothing had to move with it.
  // ══ SURFACE DISTURBANCES ── ONE ring buffer, TWO features (LG2 bit 2, the [Y] key) ══ a splash and a wake
  // are the same thing to the water: something pushed the surface at a point, and a ring left that point and
  // spread. So there is one list and two writers. spawnSplash pushes ONE ring; a body moving through water
  // pushes one every RIP_STEP voxels it travels, and the overlapping trail those leave IS the wake — no
  // separate V-shaped Kelvin machinery, the envelope falls out of a line of expanding circles on its own.
  // Compacted and OLDEST-FIRST-OUT: the shader walks it until the first empty slot and stops, so the cost is
  // the number of rings actually alive rather than the size of the array (see ripHF in render/wgsl/pre.js —
  // it is called inside the wave march, which is the one place in this renderer where a loop is expensive).
  const RIP_N = 20;                                    // ring slots. A swimmer alone holds ~7 alive; the rest is headroom for the ducks and splashes now that RIP_FAR reaches across a lake. The SHADER cost follows the number ALIVE, not this number — ripHF breaks at the first empty slot and tick-camera keeps the list compacted — so the ceiling is cheap to raise and the floor is what actually gets paid
  const RIP_LIFE = 1.8;                                // seconds a ring lives — also how long a wake trails behind you
  const RIP_SPD = 7.0;                                 // voxels/s the ring expands
  const RIP_AMP = 1.35;                                // crest height in voxels. The wave column quantizes with floor(h + 0.5), so anything under ~0.5 would round away and never show as geometry at all; this lands a ONE-voxel step, which is the shape this engine draws waves with
  const RIP_W = 1.7;                                   // half-width of the ring, in voxels
  const RIP_STEP = 2.0;                                // how far a swimmer travels between rings. Distance-based, not time-based, so a wake looks the same whatever the frame rate and stops the moment you stop
  const RIP = new Float32Array(RIP_N * 4);             // x, z, birth (seconds, u.time's clock), strength
  let ripN = 0;                                        // how many slots are live — the shader reads exactly this many
  // ── HOW FAR OUT A RING IS STILL WORTH A SLOT ── it was 110, and that was too clever by half: it fixed the
  // duck saturation below but it also meant a duck 150 voxels away, in plain view, left NO wake at all
  // (user 2026-08-29: "I can't see the splash rings and wakes at a distance"). 420 is roughly where a
  // two-voxel ring stops being resolvable at this field of view; past it there is genuinely nothing to draw.
  const RIP_FAR = 420;
  function ripAdd(wx, wz, k) {                         // …push one ring. Called by spawnSplash and by anything swimming
    const dxr = wx - P.x, dzr = wz - P.z, d2 = dxr * dxr + dzr * dzr;
    if (d2 > RIP_FAR * RIP_FAR) { return; }            // …see RIP_FAR. P is declared later in the manifest than this file, which is fine: nothing calls ripAdd until long after sim/player.js has run
    var o;
    if (ripN < RIP_N) { o = ripN * 4; ripN++; }
    else {
      // ── FULL: THE LIST HOLDS THE NEAREST RINGS, NOT THE NEWEST ── this used to shift slot 0 out and append,
      // i.e. oldest-first-out, which is the wrong question once the radius is wide. Eight ducks spread across
      // a lake would keep evicting the ring breaking at your feet in favour of one at the far shore. Now a new
      // ring only takes a slot if something FURTHER AWAY is holding one, so what you are looking at wins and
      // the far side of the lake fills in whatever is left. The scan is RIP_N long and runs a few times a
      // second, against a shader loop that runs per column of every water pixel — this is the cheap side.
      let worst = -1, worstD = d2;
      for (let i = 0; i < RIP_N; i++) { const q = i * 4;
        const ex = RIP[q] - P.x, ez = RIP[q + 1] - P.z, ed = ex * ex + ez * ez;
        if (ed > worstD) { worstD = ed; worst = i; } }
      if (worst < 0) { return; }                       // every ring held is nearer than this one — it has not earned a slot
      o = worst * 4;
    }
    // performance.now()/1000 is the SAME clock u.time carries (tick-camera writes now/1000), so the shader's
    // age arithmetic needs no offset. The comment sits ABOVE the line and not inside it: a `//` partway
    // into a dense one-liner eats the rest of it, and when this said `... / 1000;   // …note RIP[o+3] = k`
    // the STRENGTH was never assigned — every ring uploaded with w = 0, the shader broke out of its loop
    // on the first slot, and the whole feature drew nothing while every JS-side tap looked correct.
    RIP[o] = wx; RIP[o + 1] = wz; RIP[o + 2] = performance.now() / 1000; RIP[o + 3] = k === undefined ? 1 : k;
  }
  const UF_RIP = UF_CLOUDT + 4;                       // ── SURFACE DISTURBANCES ── RIP_N x vec4 (x, z, birth, strength); see ripAdd in world/window.js. Appended after cloudT for the reason everything back here is appended: every write past 'drops' is a hardcoded float index
  const UF = new Float32Array(UF_RIP + RIP_N * 4);   // …+ dof 3316..3319, heart 3320..3323, heartC 3324..3327, hurtV 3328..3331 (3331 = UF_RAINK, the rain-sky scalar — the last float of the buffer, see the note above)   // AT PHYS_MAX = 24: …+ heldCfg 2020..2023 (x = held-item sun visibility, y = its SKY visibility) + lgt 2024..2027 (light-debug bitmask) + hurtB 2028..2031 + hurtH 2032..2035 (the knife's red hit-flash box) + dropsB 2036..3059 + lifeMotB 3060..3315
  const dropOff = (s) => (s < DROP_HALF ? 68 + s * 16 : UF_DROPSB + (s - DROP_HALF) * 16);      // float index of drop slot s — the ONE place the two halves are stitched on the JS side
  const lifeMotOff = (s) => (s < DROP_HALF ? 1272 + s * 4 : UF_LIFEMOTB + (s - DROP_HALF) * 4);   // …and of its lifeMot entry
  const UF_OLD_LEN = UF_HELDCFG;   // …+ physB PHYS_MAX bodies x 5 vec4 from 1532 + physC + physBound → here (voxel rigid bodies). At 24 bodies: physB 1532..2011, physC 2012..2015, physBound 2016..2019 → 2020                   // …+ drops: 4 items end at 132, cardinal (slot 4) → 148, 4 clash sparks (slots 5-8) → 212, 55 creature slots (9-63: flyers/ducks/worms/lilies) → 1092; pick2 (left hand) 1092..1107; 8 firefly lights 1108..1139; 16 creature-shadow boxes (2 vec4 each) 1140..1267; misc 1268..1271 (x = cinematic vignette depth); lifeMot 64 vec4s 1272..1527 (per-slot world motion delta + flags — dynamic-life temporal reprojection); lifeCfg 1528..1531 → 1532
  const uniBuf = device.createBuffer({ size: UF.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const scatBuf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  // ── VOXEL PATCH SCATTER ── (creature stamps, snow landings, pickups, editor edits)
  // gpuPatch used to issue ONE 4-byte device.queue.writeBuffer per touched word. A busy frame touches
  // 2-3k words, and that many staging writes stalls the driver's upload ring: MEASURED as 25-37 ms frame
  // gaps with only ~2 ms of CPU inside tickBody (the stall lands in submit/present, outside our code).
  // Now the touched word INDICES are staged and applied by one writeBuffer + one tiny compute dispatch,
  // so the GPU call count is O(1) in the number of edited voxels instead of O(n).
  // ── RIGID BODY VOXELS ── one dense grid per live body (palette id per cell, 0 = empty), sub-allocated
  // back to back. Nothing is written into W: a detached body exists ONLY as this buffer plus its
  // transform, which is what keeps the world grid authoritative and free of moving-object stamps.
  // ── 2 << 20 -> 6 << 20 (2026-08-17, THE FELLED OAKS) ── this was sized against a PINE, whose dense body
  // box is 35*36*116 = 146k cells, so 2M held a dozen of them. An oak is a different object: the biggest
  // model's box is 114*112*114 = 1.455M cells, so ONE felled giant took 70% of the buffer and a second
  // made phReclaim evict the first - which is the 'the tree that fell has completely disappeared' failure,
  // and it would have looked like a felling bug rather than a buffer one. 6M cells = 24 MB of VRAM and
  // room for four giants down at once; 4 << 20 would buy only two, which a player clearing a stand hits
  // immediately. The GPU cost is address space, not bandwidth: the trace only reads the cells a body
  // actually occupies.
  const BODYCAP = 6 << 20;                             // 6M cells = 24 MB; a whole pine box is 35*36*116 = 146k, a giant OAK's is 1.455M
  const bodyBuf = device.createBuffer({ size: BODYCAP * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
  let bodyTop = 0;                                     // bump allocator; reset when no body references it

  // ── GERSTNER WAVE FIELD ── (user: physically-based water) 4 deep-water waves — a long primary swell, a crossing
  // secondary, two short chop waves — with the deep-water dispersion ω = c·√(g·k) (g ≈ 98 vox/s², slowed 0.42× for a
  // calm-lake feel). HEIGHT is the plain 4-cos sum (what the voxel-stepped crests + the JS floater mirror quantize);
  // the SHADING normal adds the Gerstner Q (crest-pinch) term, so surfaces read sharp-crested without breaking the
  // column grid. ONE table drives the shader AND the JS mirror — they can never drift apart.
  const GW = [                                         // [dirX, dirZ, k = 2π/λ, amp (vox), Q steepness, phase]
    [0.834, 0.552, 6.2831853 / 52, 1.30, 0.55, 0.0],
    [-0.416, 0.909, 6.2831853 / 23, 0.72, 0.50, 2.1],
    [0.966, -0.259, 6.2831853 / 11, 0.34, 0.45, 4.4],
    [0.309, -0.951, 6.2831853 / 6.7, 0.20, 0.40, 1.3]];
  const DUCK_SWAY = 0.5;                               // how much of the swell's rise and fall a DUCK actually rides — halved (user 2026-08-05); see the wave-riding block
  const GWOM = GW.map((w) => 0.21 * Math.sqrt(98 * w[2]));   // dispersion per wave — HALVED (user 2026-08-02: water 50% slower). ONE table drives the shader AND the JS floater mirror, so ducks/lilies stay bit-matched automatically.
  const gerstHJS = (wx, wz, t) => {                    // the JS mirror — ducks/lilies ride EXACTLY the surface the shader draws
    let h = 0; for (let i = 0; i < 4; i++) { const w = GW[i]; h += w[3] * Math.cos(w[2] * (w[0] * wx + w[1] * wz) - GWOM[i] * t + w[5]); } return h; };
  const GERSTH_WGSL = GW.map((w, i) =>
    `h_ += ${w[3]} * cos(${(w[2] * w[0]).toFixed(7)} * wx + ${(w[2] * w[1]).toFixed(7)} * wz - ${GWOM[i].toFixed(7)} * u.time + ${w[5]});`).join('\n      ');
  const GERSTN_WGSL = GW.map((w, i) =>
    `{ let ph = ${(w[2] * w[0]).toFixed(7)} * wx + ${(w[2] * w[1]).toFixed(7)} * wz - ${GWOM[i].toFixed(7)} * u.time + ${w[5]};
        let s_ = sin(ph); nx_ += ${(w[0] * w[2] * w[3]).toFixed(7)} * s_; nz_ += ${(w[1] * w[2] * w[3]).toFixed(7)} * s_; ny_ -= ${(w[4] * w[2] * w[3]).toFixed(7)} * cos(ph); }`).join('\n      ');

