  // ══ NAVFIELD ══ ONE truth about space, shared by every creature, at HALF XZ resolution over the whole
  // toroidal window. Five parallel planes, one nav cell = 2×2 voxels. Built from the SAME solidity the
  // player collides with — solidTab, minus foliage/cones/soft strands, minus another creature's own stamp
  // per-CELL through stampedIdx — never from a palette-id list (see the note above bfObst: CREATURE_IDS
  // aliased 84 of 256 ids and made a sixth of the world invisible to every animal).
  // The reduction is MAX ground / MIN clearance over the four covered columns, so the field is
  // CONSERVATIVE BY CONSTRUCTION: it can refuse a one-voxel gap, it can never claim room that is not
  // there. That error direction is the whole point — it lets the PLANNER and the MOVER share one
  // predicate, so "the planner chose a move the mover refuses" stops being expressible.
  //   nvY  Int16  ground/bed top, MAX of the 4 columns
  //   nvC  Uint8  free voxels above the TRAVEL SURFACE (nvY on land, WL on water), MIN of the 4, ≤ NV_CCAP
  //   nvD  Uint8  free swim band between bed and waterline, MIN of the 4 (0 = dry / no room)
  //   nvO  Uint8  openness: chamfer distance, in fifths of a cell, to the nearest cell of a DIFFERENT
  //               medium (water↔land↔wall). Water cells therefore measure distance-to-bank and land
  //               cells distance-to-water-or-wall, which is what channel centring and cover both want.
  //   nvF  Uint8  flags (see NVF_*)
  const NVSH = 1;                                    // nav cell = 2×2 voxels
  const NVX = WX >> NVSH, NVZ = WZ >> NVSH, NVN = NVX * NVZ;
  const NV_CCAP = 40;                                // clearance is counted to this ceiling and no further — 4 m of headroom is past anything that walks or flutters here
  const NV_OSTEP = 5, NV_ODIAG = 7, NV_OMAX = 120;   // 5/7 chamfer (7/5 = 1.4 ≈ √2) in FIFTHS of a cell; 120 = 24 cells = 48 voxels
  const NV_MINC = 6, NV_MIND = 3;                    // a LAND cell needs this headroom / a WATER cell this swim band before it counts as travelable
  const NVF_WATER = 1, NVF_SWIM = 2, NVF_LAND = 4, NVF_ROOF = 8, NVF_STEEP = 16, NVF_DEEP = 32, NVF_AIR = 64, NVF_BUILT = 128;
  const nvY = new Int16Array(NVN);
  const nvC = new Uint8Array(NVN);
  const nvD = new Uint8Array(NVN);
  const nvO = new Uint8Array(NVN);
  const nvF = new Uint8Array(NVN);
  //   nvK  4 bits  CLUTTER DEPTH: nvY minus the surface a WALKER actually stands on. A bunny steps over a
  //               pinecone, a stick and a field stone — bfObstW has always passed WORM_PASS — but nvY cannot,
  //               because nvY/nvC are the planes navFitsAir reads and the planes navVerify proves CONSERVATIVE,
  //               and a flyer must never be told a stick is not there. So the exemption lives in a SECOND plane
  //               that only the walker predicates subtract, and nvY/nvC/the flyer proof are untouched.
  //               Stored as a DEPTH, not a second Int16 ground: the walker's column top is at or below the
  //               obstacle top BY CONSTRUCTION (a walker passes a SUPERSET of ids), so the difference is small,
  //               non-negative, and 4 bits hold it — 0.50 MB against 2.00 MB for a second Int16 plane, for the
  //               same answer. Measured over 11836 cells of forest: 98.05% are 0 and the deepest is 4, so the
  //               15 ceiling is ~4x headroom; and a clamp there degrades to nvY itself, which is exactly
  //               today's behaviour (a conservative REFUSAL), never to a claim of room that is not there.
  const nvK = new Uint8Array((NVN + 1) >> 1);
  const nvDirtyBits = new Uint32Array((NVN + 31) >> 5);
  const NV_BYTES = nvY.byteLength + nvC.byteLength + nvD.byteLength + nvO.byteLength + nvF.byteLength + nvK.byteLength + nvDirtyBits.byteLength;
  const NAVOFF = location.search.includes('nonav');  // ?nonav — the field is never built and every consumer falls back to the point probes it used before
  const NAVARB = !NAVOFF && !location.search.includes('noarb');   // ?noarb — field ON, arbiter OFF (the A/B that separates the field's COST from the arbiter's EFFECT)
  const nvPass = new Uint8Array(256);                // per palette id: 1 = a creature passes straight through this voxel. Mirrors bfObst's id rule exactly.
  let nvPassWater = -1;                              // solidTab[WATER_T] as of the last table build — it FLIPS to 1 when a lake freezes, which collapses every swim band
  const nvClut = new Uint8Array(256);                // per palette id: 1 = an OBSTACLE voxel a WALKER steps straight over. Exactly the WORM_PASS set bfObstW passes (pinecones, sticks, field stones) minus whatever nvPass already lets through, so this table only ever fires on ids that are walls to everything else.
  const nvInitTabs = () => {
    for (let id = 0; id < 256; id++) nvPass[id] = (solidTab[id] !== 1 || foliaTab[id] || coneTab[id] || SNOW_PASS.has(id) || SNOW_FERN.has(id)) ? 1 : 0;
    nvPass[0] = 1;
    for (let id = 0; id < 256; id++) nvClut[id] = (nvPass[id] === 0 && WORM_PASS.has(id)) ? 1 : 0;   // the Set is read ONCE per table build, never per voxel — the column scan below indexes this array
    nvPassWater = solidTab[WATER_T];
  };
  const nvSolidII = (ii) => {                        // the one per-voxel obstacle rule the field is built from
    const id = W[ii];
    if (nvPass[id]) return false;
    return !(CREA_FLAG[id] === 1 && stampedIdx.size !== 0 && stampedIdx.has(ii)); };   // CREA_FLAG first: it turns ~1M Set lookups per rebuild into a handful, and a stamped cell always carries a creature id
  const nvObst = (x, y, z) => {                     // the per-voxel obstacle rule at MODULE scope. bfObst itself is a per-frame LOCAL inside the tick body, so the field cannot call it; this is the same rule over the same tables.
    const yy = y < 1 ? 1 : (y > WY - 1 ? WY - 1 : Math.floor(y));
    return nvSolidII(gwrap(Math.floor(x), WX) + yy * WX + gwrap(Math.floor(z), WZ) * WX * WY); };
  const nvObstW = (x, y, z) => {                    // the WALKER's per-voxel rule at MODULE scope — nvObst minus the ground clutter, i.e. bfObstW written against the field's own tables. Consumed only by the UNVOUCHED fallback, where there is no built cell to read.
    const yy = y < 1 ? 1 : (y > WY - 1 ? WY - 1 : Math.floor(y));
    const ii = gwrap(Math.floor(x), WX) + yy * WX + gwrap(Math.floor(z), WZ) * WX * WY;
    return nvClut[W[ii]] === 0 && nvSolidII(ii); };
  let nvColW = 0;                                    // OUT-PARAMETER of nvColTop: the same column's WALKER top, found in the SAME downward scan. A second scan would have doubled the most expensive part of a rebuild; instead, because the walker top is always at or below the obstacle top, one pass finds both — and the 98% of columns with no clutter stop on exactly the voxel they stop on today.
  const nvColTop = (gx, gz) => {                     // topmost obstacle voxel in the band around the heightmap hint. Empty 8³ BRICKS are skipped whole, so open air above the terrain costs one bit test per 8 voxels.
    const hm = hmap[gx + gz * WX];
    const base = gx + gz * WX * WY, gx8 = gx >> 3, gz8 = gz >> 3;
    const lo = hm - 20 < 1 ? 1 : hm - 20;
    let y = hm + 20 > WY - 2 ? WY - 2 : hm + 20, top = -30000;
    while (y >= lo) {
      const b = gx8 + (y >> 3) * BX + gz8 * BX * BY;
      if (((bricks[b >> 5] >>> (b & 31)) & 1) === 0) { y = (y & ~7) - 1; continue; }
      let yb = y & ~7; if (yb < lo) yb = lo;
      for (; y >= yb; y--) { const ii = base + y * WX;
        if (!nvSolidII(ii)) continue;
        if (top === -30000) top = y;                 // the OBSTACLE top — what nvY has always stored, unchanged
        if (nvClut[W[ii]] === 0) { nvColW = y; return top; } }   // …and the first voxel a WALKER cannot pass is its ground. Both found, one scan, and the common column returns on its first solid voxel exactly as before.
    }
    nvColW = lo - 1;
    return top === -30000 ? lo - 1 : top; };
  const nvFreeAbove = (gx, gz, y0, cap) => {         // free voxels upward from y0, stopping at the first obstacle or the cap
    if (y0 < 1) y0 = 1;
    const base = gx + gz * WX * WY, gx8 = gx >> 3, gz8 = gz >> 3;
    const yEnd = y0 + cap > WY - 1 ? WY - 1 : y0 + cap;
    let y = y0, n = 0;
    while (y < yEnd) {
      const b = gx8 + (y >> 3) * BX + gz8 * BX * BY;
      if (((bricks[b >> 5] >>> (b & 31)) & 1) === 0) { let st = 8 - (y & 7); if (y + st > yEnd) st = yEnd - y; n += st; y += st; continue; }
      if (nvSolidII(base + y * WX)) break;
      n++; y++;
    }
    return n; };
  const nvSwimBand = (gx, gz, ct) => {               // free voxels between the bed and the waterline — 0 the moment the ice makes WATER_T solid
    const base = gx + gz * WX * WY, y1 = WL > WY - 2 ? WY - 2 : WL;
    let n = 0;
    for (let y = ct + 1 < 1 ? 1 : ct + 1; y <= y1; y++) { if (nvSolidII(base + y * WX)) break; n++; }
    return n > 255 ? 255 : n; };
  const nvCtA = new Int32Array(4), nvWetA = new Uint8Array(4);   // preallocated — a rebuild must not allocate
  const nvBuildCell = (ci) => {
    const cx = ci % NVX, cz = (ci - cx) / NVX, gx0 = cx << NVSH, gz0 = cz << NVSH;
    let top = -30000, low = 30000, wetN = 0, wtop = -30000;
    for (let q = 0; q < 4; q++) {
      const gx = gx0 + (q & 1), gz = gz0 + (q >> 1);
      const ct = nvColTop(gx, gz);
      if (nvColW > wtop) wtop = nvColW;              // MAX of the four walker tops, the same reduction and the same direction of error as `top` — a walker is never told a step is smaller than it is
      nvCtA[q] = ct;
      const wv = W[gx + WL * WX + gz * WX * WY];
      const wet = (wv === WATER_T || wv === WATER_B) ? 1 : 0;
      nvWetA[q] = wet; wetN += wet;
      if (ct > top) top = ct;
      if (ct < low) low = ct;
    }
    const ts = (wetN !== 0 && top < WL) ? WL : top;  // TRAVEL SURFACE — the waterline on water, the ground on land. Clearance is measured from here, for all four columns, so a shore column inside a water cell reports 0 and the MIN refuses the cell.
    let cmin = 255, dmin = 255;
    for (let q = 0; q < 4; q++) {
      const gx = gx0 + (q & 1), gz = gz0 + (q >> 1);
      const c = nvFreeAbove(gx, gz, ts + 1, NV_CCAP);
      if (c < cmin) cmin = c;
      const d = nvWetA[q] ? nvSwimBand(gx, gz, nvCtA[q]) : 0;
      if (d < dmin) dmin = d;
    }
    let f = NVF_BUILT;
    if (wetN !== 0) f |= NVF_WATER;
    if (wetN === 4 && dmin >= NV_MIND) f |= NVF_SWIM;
    if (wetN === 0 && top > WL && cmin >= NV_MINC) f |= NVF_LAND;
    if (cmin < 14) f |= NVF_ROOF;
    if (top - low > 3) f |= NVF_STEEP;
    if ((f & NVF_SWIM) && dmin >= 8) f |= NVF_DEEP;
    if (cmin >= 8) f |= NVF_AIR;
    nvY[ci] = top < -32768 ? -32768 : (top > 32767 ? 32767 : top);
    nvC[ci] = cmin;
    nvD[ci] = dmin > 255 ? 255 : dmin;
    let cd = top - wtop; if (cd < 0) cd = 0; if (cd > 15) cd = 15;   // ≥ 0 by construction; the clamp is a belt, and it fails toward nvY — i.e. toward today's answer
    nvK[ci >> 1] = (nvK[ci >> 1] & ((ci & 1) ? 0x0f : 0xf0)) | (cd << ((ci & 1) << 2));   // read-modify-write is safe: every write to this plane comes from nvBuildCell, and nvBuildCell only ever runs on the main thread
    nvF[ci] = f; };
  const nvIdx = (x, z) => (gwrap(Math.floor(x), WX) >> NVSH) + (gwrap(Math.floor(z), WZ) >> NVSH) * NVX;
  const nvTop = (ci) => { const g = nvY[ci]; return ((nvF[ci] & NVF_WATER) && g < WL) ? WL : g; };   // the travel surface this cell's clearance was measured from
  const nvTopAir = (ci) => { const g = nvY[ci] + 1; return ((nvF[ci] & NVF_WATER) && g < WL) ? WL : g; };   // …the same surface in HMAP UNITS: +1 because nvY is the topmost SOLID voxel while hmap/bfSurf is the FIRST AIR above it. Mixing the two is a silent one-voxel bias — see navWalkGround.
  const nvClutD = (ci) => (nvK[ci >> 1] >>> ((ci & 1) << 2)) & 15;   // how far BELOW nvY a walker's foot actually lands here. Subtract it from nvY for the walker's ground; ADD it to nvC for the walker's headroom — the voxels between the two tops are, by the definition of "topmost non-clutter voxel", clutter or air, so they are free to a walker and the sum is the exact clearance above its own ground.
  // ── OPENNESS ── a chamfer distance transform per MEDIUM: within a region of one class, distance grows
  // normally; any neighbour of a DIFFERENT class (including wall) is a zero-distance boundary. So a fish
  // in a creek reads distance-to-bank and a skunk on a ridge reads distance-to-cliff, out of one plane.
  // Run as a rolling full-plane job, ~0.35 ms/frame, forward pass then backward pass then repeat: nvO is a
  // SCORE term, never a gate, so a couple of seconds of staleness after a chop costs nothing. The array
  // seam (the toroidal wrap) is treated as a hard edge rather than wrapped — it sits ~1000 voxels from the
  // player at all times and wrapping it would cost a modulo per cell for an answer nothing can observe.
  const nvClassOf = (ci) => { const f = nvF[ci]; return (f & NVF_SWIM) ? 2 : ((f & NVF_LAND) ? 1 : 0); };
  const NV_CHAM_S = 2.0;                               // seconds per complete forward+backward cycle. WALL-CLOCK paced, not per-frame: headless free-runs at 700+ fps and a per-frame row quota burned 0.35 ms every one of them for 14 full cycles a second of an answer that changes when a tree falls.
  let nvChPhase = 0, nvChRow = 0, nvChCycles = 0, nvChCredit = 0, nvChLast = 0;
  const nvChamRow = (z, back) => {
    const r = z * NVX;
    if (!back) {
      const rp = (z - 1) * NVX;
      for (let x = 0; x < NVX; x++) {
        const ci = r + x, k = nvClassOf(ci);
        if (k === 0) { nvO[ci] = 0; continue; }
        let d = NV_OMAX, n, dn;
        if (x > 0) { n = ci - 1; dn = nvClassOf(n) === k ? nvO[n] + NV_OSTEP : NV_OSTEP; if (dn < d) d = dn; }
        if (z > 0) {
          n = rp + x; dn = nvClassOf(n) === k ? nvO[n] + NV_OSTEP : NV_OSTEP; if (dn < d) d = dn;
          if (x > 0) { n = rp + x - 1; dn = nvClassOf(n) === k ? nvO[n] + NV_ODIAG : NV_ODIAG; if (dn < d) d = dn; }
          if (x < NVX - 1) { n = rp + x + 1; dn = nvClassOf(n) === k ? nvO[n] + NV_ODIAG : NV_ODIAG; if (dn < d) d = dn; }
        }
        nvO[ci] = d > NV_OMAX ? NV_OMAX : d;
      }
    } else {
      const rn = (z + 1) * NVX;
      for (let x = NVX - 1; x >= 0; x--) {
        const ci = r + x, k = nvClassOf(ci);
        if (k === 0) { nvO[ci] = 0; continue; }
        let d = nvO[ci], n, dn;
        if (x < NVX - 1) { n = ci + 1; dn = nvClassOf(n) === k ? nvO[n] + NV_OSTEP : NV_OSTEP; if (dn < d) d = dn; }
        if (z < NVZ - 1) {
          n = rn + x; dn = nvClassOf(n) === k ? nvO[n] + NV_OSTEP : NV_OSTEP; if (dn < d) d = dn;
          if (x < NVX - 1) { n = rn + x + 1; dn = nvClassOf(n) === k ? nvO[n] + NV_ODIAG : NV_ODIAG; if (dn < d) d = dn; }
          if (x > 0) { n = rn + x - 1; dn = nvClassOf(n) === k ? nvO[n] + NV_ODIAG : NV_ODIAG; if (dn < d) d = dn; }
        }
        nvO[ci] = d > NV_OMAX ? NV_OMAX : d;
      }
    } };
  // ── MAINTENANCE ── every runtime mutation of W already funnels through gpuPatch, so nvTouch hangs off
  // THAT choke point and nothing else has to remember to call it. Streamed bands mark their own rect.
  // Both feed one dirty-cell queue drained inside the streaming step, where stepShifts early-returns at
  // deficit <= 0 and spends none of its 7-18 ms allowance in steady state.
  let nvOn = 0, nvQn = 0, nvQdrop = 0, nvSweep = 0, nvSweepI = 0, nvBuiltTotal = 0, nvFullMs = 0;
  let nvQ = new Int32Array(1 << 16);
  const nvMS = new Float32Array(256); let nvMSi = 0, nvMSn = 0;
  const nvMark = (ci) => {
    const w = ci >> 5, b = 1 << (ci & 31);
    if (nvDirtyBits[w] & b) return;
    if (nvQn >= nvQ.length) {
      if (nvQ.length >= (1 << 19)) { nvQdrop++; nvSweep = 1; return; }   // pathological churn — hand the whole plane to the background sweep instead of growing an 8 MB queue
      const g = new Int32Array(nvQ.length << 1); g.set(nvQ); nvQ = g; }
    nvDirtyBits[w] |= b; nvQ[nvQn++] = ci; };
  const nvTouch = (gx, gz) => { if (nvOn) nvMark((gx >> NVSH) + (gz >> NVSH) * NVX); };   // gx/gz are ALREADY window-wrapped (gpuPatch derives them from the flat index)
  const nvDirtyRect = (wx0, wx1, wz0, wz1) => {      // world-coordinate rect → every nav cell it covers
    if (!nvOn) return;
    for (let z = wz0; z < wz1; z += 2) { const cz = (gwrap(z, WZ) >> NVSH) * NVX;
      for (let x = wx0; x < wx1; x += 2) nvMark((gwrap(x, WX) >> NVSH) + cz); } };
  const nvDrain = (budget) => {                      // rebuild dirty cells, then the background sweep, then a slice of the chamfer
    const t0 = performance.now();
    if (solidTab[WATER_T] !== nvPassWater) { nvInitTabs(); nvSweep = 1; }   // the lake froze or thawed: water changed medium under the whole field
    let did = 0;
    while (nvQn > 0) {
      const ci = nvQ[--nvQn];
      nvDirtyBits[ci >> 5] &= ~(1 << (ci & 31));
      nvBuildCell(ci); did++;
      if ((did & 127) === 0 && performance.now() - t0 > budget) break;
    }
    if (nvSweep && nvQn === 0) {
      while (performance.now() - t0 < budget) {
        for (let k = 0; k < 128; k++) { nvBuildCell(nvSweepI); did++; if (++nvSweepI >= NVN) { nvSweepI = 0; nvSweep = 0; break; } }
        if (!nvSweep) break;
      }
    }
    nvBuiltTotal += did;
    return did; };
  const nvFlush = () => {
    if (!nvOn || ED.on) return;                      // the asset-editor world is FROZEN and its stage is a borrowed plane — nothing about it is terrain the field should learn
    const t0 = performance.now();
    let budget = 0.6 + nvQn / 12000; if (budget > 4) budget = 4;
    if (nvSweep && budget < 2) budget = 2;           // a whole-window sweep (boot / recentre / the ice flipping water to solid) is worth 2 ms a frame — measured ~3.4 k cells/ms, so the full 1.05 M-cell window lands in ~2.6 s at 60 fps
    const did = nvDrain(budget);
    const t1 = performance.now();
    if (nvChLast) { nvChCredit += (t1 - nvChLast) * (NVZ * 2 / (NV_CHAM_S * 1000)); if (nvChCredit > NVZ) nvChCredit = NVZ; }
    nvChLast = t1;
    let rows = 0;
    while (nvChCredit >= 1 && rows < 512 && performance.now() - t1 < 1.5) {   // rolling chamfer — one row is NVX cells × 4 neighbour tests
      nvChamRow(nvChPhase ? NVZ - 1 - nvChRow : nvChRow, nvChPhase);
      rows++; nvChCredit--;
      if (++nvChRow >= NVZ) { nvChRow = 0; nvChPhase ^= 1; if (!nvChPhase) nvChCycles++; }
    }
    nvMS[nvMSi] = performance.now() - t0; nvMSi = (nvMSi + 1) & 255; if (nvMSn < 256) nvMSn++;
    if (CPROF && (did || rows)) cpEvt |= 256; };
  const NV_BOOTR = 384;                              // radius, in voxels, of the neighbourhood built SYNCHRONOUSLY at boot / after a recentre
  const nvBoot = () => {                             // build the player's own neighbourhood now; hand the rest of the window to the background sweep
    if (NAVOFF) return 0;
    const t0 = performance.now();
    nvInitTabs(); nvOn = 1;
    nvF.fill(0); nvQn = 0; nvDirtyBits.fill(0);      // NVF_BUILT clear everywhere = "the field vouches for nothing here"; every consumer falls back to the point probes it used before, so a half-built field is never WRONG, only unhelpful
    nvDirtyRect(Math.max(rect.xlo, Math.floor(P.x) - NV_BOOTR), Math.min(rect.xhi, Math.floor(P.x) + NV_BOOTR),
                Math.max(rect.zlo, Math.floor(P.z) - NV_BOOTR), Math.min(rect.zhi, Math.floor(P.z) + NV_BOOTR));
    nvDrain(1e9);
    nvSweep = 1; nvSweepI = 0;                       // the remaining ~1M cells fill in at a couple of hundred µs a frame — a full-window synchronous build would be a second of boot for terrain no creature has reached yet
    nvFullMs = performance.now() - t0;
    return nvFullMs; };
  // ══ ARBITER ══ ONE writer of motion. Five uncoordinated controllers writing B.th in sequence (fan →
  // flee → repulsion → backstop → leash), three of them using DIFFERENT terrain predicates and two
  // translating the creature with no predicate at all, is the mechanism behind every "stuck" report in
  // this file. The replacement is a single scored candidate fan per creature per sense tick, and exactly
  // ONE feasibility answer — navFits — consulted by the planner, the mover, the vertical step and the
  // escape alike. A planner that can only choose moves the mover will accept cannot deadlock.
  // Wired LIVE for the flyer band (kinds 0/1) in this pass; the land/water predicates below are built and
  // verified but not yet consumed (see navVerify).
  const NAV_TH = new Float32Array(16), NAV_SIN = new Float32Array(16), NAV_COS = new Float32Array(16);
  for (let k = 0; k < 16; k++) { const a = k * 0.39269908; NAV_TH[k] = a; NAV_SIN[k] = Math.sin(a); NAV_COS[k] = Math.cos(a); }
  const NAV_HZ = 1 / 12, NAV_REACH = 40;             // 12 Hz sense tick, staggered by slot; the existing eased integrator carries the heading between ticks
  const navAng = (a) => { let d = a; while (d > Math.PI) d -= 6.283185307; while (d < -Math.PI) d += 6.283185307; return d; };
  const navFitsAir = (x, y, z) => {                  // can a flyer's body be here? Conservative inside the vouched band; an honest point probe above it.
    const ci = nvIdx(x, z), f = nvF[ci];
    if (!(f & NVF_BUILT)) return !nvObst(x, y, z) && !nvObst(x, y + 2, z);
    const g = nvY[ci], ts = ((f & NVF_WATER) && g < WL) ? WL : g, c = nvC[ci];
    if (y >= ts + 2 && y + 2 <= ts + c) return true;                             // inside the free band the field vouches for
    if (c >= NV_CCAP && y + 2 > ts + c) return !nvObst(x, y, z) && !nvObst(x, y + 2, z);   // ≥ NV_CCAP free above the surface means no trunk in this cell — above that the field stops vouching, so ask the voxel. The test is on the BODY TOP, not the centre: `y > ts + c` left a two-voxel hole at the band ceiling where neither branch matched and everything read blocked.
    return false; };                                                            // c < NV_CCAP means a REAL obstacle capped the band: refuse, and never guess past it
  const navWet = (x, z) => { const v = W[gwrap(Math.floor(x), WX) + WL * WX + gwrap(Math.floor(z), WZ) * WX * WY]; return v === WATER_T || v === WATER_B; };   // module-scope mirrors of bfWater / bfBed — those are per-frame LOCALS inside the tick body, so the field cannot call them
  const navBed = (x, z) => hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX];
  const navFitsLand = (x, y, z) => {                 // walkable ground under a body of this height. The marchers navigate on navLandOK below, which carries their own step limits; this is the kind-dispatched form navFits asks for.
    const ci = nvIdx(x, z), f = nvF[ci];
    if (!(f & NVF_BUILT)) return !navWet(x, z) && navBed(x, z) > WL && !nvObst(x, y, z) && !nvObst(x, y + 2, z);   // UNVOUCHED → the honest point probe, never a blanket refusal: `false` here would freeze every ground creature at the streaming frontier, which is the exact failure the arbiter exists to remove
    return (f & NVF_LAND) !== 0 && y >= nvY[ci] && y + 2 <= nvY[ci] + nvC[ci]; };
  const navFitsSwim = (x, y, z) => {                 // real, deep-enough water with the body inside the band — THE fish's water answer
    const ci = nvIdx(x, z), f = nvF[ci];
    if (!(f & NVF_BUILT)) { const b = navBed(x, z); return navWet(x, z) && WL - b >= NV_MIND && y > b + 1 && y < WL; }   // …and the same for water: an unbuilt cell falls back to the probe the swimmer used before
    return (f & NVF_SWIM) !== 0 && y > nvY[ci] + 1 && y < WL; };
  const navFits = (kind, x, y, z) => kind < 2 ? navFitsAir(x, y, z)      // THE one feasibility answer, dispatched by kind. Flyers 0/1 are live; 2 (worm/mammal) and 3/4/6 (duck/lily/fish) resolve to the land and swim predicates the field already carries but their controllers do not consult yet; 5 is the perched songbird, which does not navigate at all.
    : (kind === 2 ? navFitsLand(x, y, z) : (kind === 5 ? false : navFitsSwim(x, y, z)));
  const navReachAir = (x, y, z, th, maxD) => {       // how far the body can actually travel along a heading. A DDA over NAV CELLS, not a fixed stride: a 2-voxel stride down a diagonal skips the two cells either side of the corner, and a butterfly moving 0.06 vox a frame then EDGE-LOCKED against a cell the planner had never sampled while its reach still read 40. Every cell the path crosses is tested, so the planner cannot approve a lane the mover will refuse.
    const dx = Math.sin(th), dz = Math.cos(th), CS = 1 << NVSH;
    const adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
    const cx = Math.floor(x / CS), cz = Math.floor(z / CS);
    const dtX = adx < 1e-9 ? Infinity : CS / adx, dtZ = adz < 1e-9 ? Infinity : CS / adz;
    let tX = adx < 1e-9 ? Infinity : (dx > 0 ? (cx + 1) * CS - x : x - cx * CS) / adx;
    let tZ = adz < 1e-9 ? Infinity : (dz > 0 ? (cz + 1) * CS - z : z - cz * CS) / adz;
    let t = 0;
    for (let g = 0; g < 80; g++) {
      t = tX <= tZ ? tX : tZ;
      if (t >= maxD) return maxD;
      if (tX <= tZ) tX += dtX; else tZ += dtZ;
      if (!navFitsAir(x + dx * (t + 0.02), y, z + dz * (t + 0.02))) return t;   // the clear run ends AT this boundary
    }
    return t; };
  const navRoofAir = (x, y, z) => !navFitsAir(x, y + 7, z) || !navFitsAir(x, y + 13, z);   // same one predicate bfRoofed asks of bfObst, asked of the field instead
  let navTicks = 0, navNoLane = 0, navVetoY = 0, navRejects = 0, navEgressN = 0;
  const navMoveK = new Float64Array(8), navRejK = new Float64Array(8), navEgrK = new Float64Array(8), navVetK = new Float64Array(8), navBrkK = new Float64Array(8), navBrkVoxK = new Float64Array(8);   // the same counters SPLIT BY B.kind, so a landing in one band can never hide behind another's total — the flyer's rejection count has to stay visibly 0 while a new band is wired
  let navMoveN = 0, navBrakeN = 0, navBrakeVox = 0, navRejGeom = 0, navRejSub = 0, navRejUnb = 0;   // flyer move-frames, frames the brake bound on, voxels of travel it withheld, and the REJECTION CENSUS: the lane genuinely ended inside the step / the DDA and the endpoint probe disagreed inside one cell / the destination cell is not built. A single arbRejects number cannot tell a brake failure from a sub-cell probe, and those want opposite fixes.
  const NAVBRK = NAVARB && !location.search.includes('nobrake');   // ?nobrake — arbiter ON, speed cap OFF. The A/B that isolates the cap from everything else the arbiter already does, so its effect is never inferred from a build difference.
  // NO FISH STEP CAP. One was built and measured on 3 seeds × 240 s: it cut a dense body audit from 12.0% to 5.6% of frames, but that audit's every hit was +-1 voxel LATERAL proximity (the game's own 5-station body stencil read 0.00% before and after, and the fish's centre was never once in solid), and it bought that by slowing the fish from 16.2 to 7.6 vox/s near any bank — 12% of the band's distance travelled and a longest standstill of 1.41 s against 0.26 s. The flyers' cap earns its place because their rejections were real; here there was nothing left to reject, so the cap was pure cost. The shared predicate and the vertical veto below are what the fish band keeps.
  const NAV_LOOK = 14, NAV_CLR = 1.5, NAV_BRK2 = 252;   // brake horizon in voxels, the gap the body never closes inside, and 2·deceleration. sqrt(NAV_BRK2 · (NAV_LOOK − NAV_CLR)) = 56.1 vox/s, one notch ABOVE the butterfly's 56, so an open lane is full cruise and the cap costs exactly nothing until something is inside 14 voxels — the same warning distance the pre-arbiter point probe used.
  const navBrakeAir = (B, mv, dt) => {               // ── CAP THE STEP BY REACH ALONG THE HEADING ── the arbiter's remaining 2.8% of rejections are all one thing: the mover translates along B.th, which LAGS B.navTh while the eased turn integrator catches up, so for a few frames the creature is aimed down a lane the planner never chose and the shared predicate refuses it. Braking on the reach of the lane the mover is ACTUALLY flying makes that disagreement unreachable — the step can no longer END past the lane, so it can no longer be rejected — and the creature arrives at the turn already slow instead of arriving at full speed and hard-stopping for a frame.
    const rr = navReachAir(B.x, B.y, B.z, B.th, NAV_LOOK);
    B.navClear = rr;
    if (rr >= NAV_LOOK) return mv;                   // clear to the horizon — the common case: one bounded DDA and out
    navBrakeN++;
    let m = Math.sqrt(NAV_BRK2 * (rr > NAV_CLR ? rr - NAV_CLR : 0)) * dt;   // v = sqrt(2·a·d), the braking curve: continuous in reach, so the speed RAMPS instead of stepping, and it self-scales with the creature — a 26 vox/s firefly sheds nothing until 4 voxels out while a 56 vox/s butterfly starts at 14
    if (m > mv) m = mv;
    const cap = rr - 0.05;                           // GEOMETRIC BACKSTOP — the curve alone can still overshoot on a long frame (dt is clamped at 0.15 s, and 0.15 s is 8 voxels), and one overshoot is one rejection
    if (m > cap) m = cap > 0 ? cap : 0;
    if (m < mv) navBrakeVox += mv - m;
    return m; };
  // ── SCORE WEIGHTS ── the TERMS are universal, the WEIGHTS are per kind. One flat Float64Array indexed
  // kind*NAV_WN + term, so a per-kind lookup allocates nothing and a later kind is a row, not a branch.
  // Rows for kinds 2-6 are present and inert: those bands are still on their own controllers this pass.
  const NAV_WN = 7, NAV_W_REACH = 0, NAV_W_OPEN = 1, NAV_W_KEEP = 2, NAV_W_WANDER = 3, NAV_W_HOME = 4, NAV_W_ROOF = 5, NAV_W_TURN = 6;
  const NAV_W = new Float64Array(7 * NAV_WN);        // Float64, not Float32: a weight table must hold the authored constants EXACTLY, or every score shifts by a rounding step and a seeded A/B stops comparing the same thing
  {                                                  // reach, openness, hold-heading, wander, home-leash, roof penalty, turn cost
    const rows = { 0: [1.00, 0.45, 0.55, 0.70, 0.95, 0.80, 0.30],   // butterfly / dragonfly — 56 vox/s, 11-vox turn radius: reach dominates, the wander keeps the flutter
                   1: [1.00, 0.45, 0.55, 0.70, 0.95, 0.80, 0.30],   // firefly — 26 vox/s; identical for now so the arbiter's first landing changes exactly one thing
                   2: [1.00, 0.45, 0.55, 0.70, 0.95, 0.80, 0.30] }; // worm — 16 vox/s. Deliberately the flyer row: a band's FIRST landing must change exactly one thing (the predicate), so a weight difference can never be mistaken for the arbiter's effect. (No duck row: the ducks keep their own edge-avoidance fan — see duckFit — so nothing reads one.)
    for (const k in rows) for (let t = 0; t < NAV_WN; t++) NAV_W[k * NAV_WN + t] = rows[k][t];
  }
  const navSteerAir = (B, homeTh, leashOut) => {     // → B.omT. 16 compass candidates + hold-current, each scored on the SAME terms.
    navTicks++;
    const w0 = (B.kind | 0) * NAV_WN;
    const wth = B.navWander === undefined ? B.th : B.navWander;
    let bestTh = B.th, bestS = -1e9, bestReach = 0;
    const here = navFitsAir(B.x, B.y, B.z);            // if the creature's OWN cell is infeasible the fan is moot — every candidate starts from a place the mover will not accept
    if (here) for (let k = 0; k <= 16; k++) {
      const th = k === 16 ? B.th : NAV_TH[k];
      const sx = k === 16 ? Math.sin(th) : NAV_SIN[k], cz = k === 16 ? Math.cos(th) : NAV_COS[k];
      const reach = navReachAir(B.x, B.y, B.z, th, NAV_REACH);
      if (reach < 4) continue;
      const ax = B.x + sx * 6, az = B.z + cz * 6;
      const dTh = navAng(th - B.th);
      let s = (reach / NAV_REACH) * NAV_W[w0 + NAV_W_REACH]
            + (nvO[nvIdx(ax, az)] / NV_OMAX) * NAV_W[w0 + NAV_W_OPEN]
            + Math.cos(dTh) * NAV_W[w0 + NAV_W_KEEP]
            + Math.cos(navAng(th - wth)) * NAV_W[w0 + NAV_W_WANDER]
            - (dTh < 0 ? -dTh : dTh) * (NAV_W[w0 + NAV_W_TURN] / Math.PI);
      if (leashOut) s += Math.cos(navAng(th - homeTh)) * NAV_W[w0 + NAV_W_HOME];
      if (navRoofAir(ax, B.y, az)) s -= NAV_W[w0 + NAV_W_ROOF];
      if (s > bestS) { bestS = s; bestTh = th; bestReach = reach; }
    }
    if (bestReach < 4) {                             // ── LAYER 1 ── no candidate has a lane. Take the steepest openness ascent instead of spinning: nvO is a chamfer over the travelable set, so from any cell with nvO > 0 an ascent to more open space EXISTS.
      navNoLane++;
      let bo = B.th, bs = -1e9;
      for (let k = 0; k < 16; k++) {
        const px = B.x + NAV_SIN[k] * 4, pz = B.z + NAV_COS[k] * 4;
        const s = navReachAir(B.x, B.y, B.z, NAV_TH[k], 8) * 40 + nvO[nvIdx(px, pz)] - Math.abs(navAng(NAV_TH[k] - B.th)) * 6;
        if (s > bs) { bs = s; bo = NAV_TH[k]; } }
      bestTh = bo;
    }
    B.navTh = bestTh; B.navReach = bestReach;
    const d = navAng(bestTh - B.th);
    B.omT = Math.max(-6.5, Math.min(6.5, d * 4.2)); };
  // ══ THE OTHER BANDS ══ the flyer's three arbiter pieces — the cell DDA, the scored candidate fan and
  // the reach brake — written once more against a plain 2-D FEASIBILITY CLOSURE instead of navFitsAir.
  // A ground or surface creature navigates in x/z and takes y from a servo, so ONE closure per creature
  // per sense tick carries everything a band's character needs (step limits, head clearance, body length)
  // without threading a predicate's argument list through the DDA. The flyer functions above are left
  // CHARACTER FOR CHARACTER alone on purpose: they are the measured control, and folding them into a
  // shared generic would have moved their arithmetic by a rounding step and re-rolled a 240 s soak.
  const NAV_WREACH = 20;                             // the WORM planner's horizon: a 16 vox/s crawler does not need the butterfly's 40, and the DDA cost is linear in it
  const NAV_WUP = 2, NAV_WDN = 2, NAV_WCLR = 3;      // WORM step up / step down / head clearance — the legacy wormOK numbers verbatim (|Δground| ≤ 2, bfObstW probes at +2 and +3), asked of the field instead of the heightmap
  const NAV_DLOOK = 10, NAV_DBCLR = 1.0, NAV_DBRK2 = 14;
  const NAV_FLOOK = 14, NAV_FBCLR = 2.0, NAV_FBRK2 = 46;   // FISH brake: sqrt(46 · (14 − 2)) = 23.5 vox/s against a 22 vox/s cruise, so OPEN water is untouched and this only bites as a bank closes in. The fish already fans whiskers to pick a lane; what it lacked was any reason to SLOW as that lane shortened, so it arrived at the bank still at full cruise, ground against it until the trap timer fired, and got re-placed — which reads as a teleport (user 2026-08-06). Braking gives the eased turn time to finish, so it comes about instead.   // DUCK brake: sqrt(14 · (10 − 1)) = 11.2 vox/s against a 7 vox/s paddle (10 for a duckling scrambling to heel), so an open lane is full pace and the cap only ramps once the lane is under ~5 voxels — well inside the 7-voxel head buffer the duck already keeps.
  const NAV_WLOOK = 6, NAV_WBCLR = 1.0, NAV_WBRK2 = 58;   // …and its brake: sqrt(58 · (6 − 1)) = 17.0 vox/s against a 16 vox/s crawl, so an open lane is full pace and the cap withholds nothing until the lane is under ~5 voxels
  const navGroundAt = (x, z) => { const ci = nvIdx(x, z);   // the travel surface a LAND creature stands on: the field's 2×2 max where it is vouched for, the heightmap where it is not. Same fix as the flyer's gAir — a servo riding bfSurf while the predicate measures clearance from nvY is the planner/mover split reintroduced on the vertical axis.
    if (!(nvF[ci] & NVF_BUILT)) return navBed(x, z) > WL ? navBed(x, z) : WL;
    const g = nvY[ci] + 1;                           // ── UNITS ── exactly navWalkGround's +1, and for exactly its reason: nvY is the topmost SOLID voxel while hmap is the FIRST AIR voxel above it, and hmap is what the unvouched branch one line up returns, what bfSurf returns, and what every worm step limit and probe offset was authored against. Without it the two branches disagreed by a voxel at every streaming frontier — a legitimate 2-voxel step read as 3 and the lane was refused — and the worm's y servo (tick-creatures gcW) sat a voxel below the pre-arbiter bfSurf target everywhere else.
    return ((nvF[ci] & NVF_WATER) && g < WL) ? WL : g; };   // nvTop's own water clamp, kept: a wet cell's travel surface is the waterline
  const navLandOK = (x, z, gc, up, down, clr) => {   // THE land answer. The step limits and head clearance passed in are the marchers' OWN legacy numbers (±2 for a worm, +3/−4 for a mammal; the bfObstW probe heights) — measured against the field instead of the heightmap, so the decor and rock hmap never saw are finally in the test.
    const ci = nvIdx(x, z), f = nvF[ci];
    if (!(f & NVF_BUILT)) { const g = navBed(x, z);   // UNVOUCHED → the honest point probe the marcher used before. Never a blanket refusal: a creature at the streaming frontier must not freeze waiting for a cell to be built.
      return !navWet(x, z) && g > WL && g - gc <= up && gc - g <= down && !nvObst(x, g + 2, z) && !nvObst(x, g + clr, z); }
    if (f & NVF_WATER) return false;                 // any water in the 2×2 — a land creature stops AT the waterline, not one step past it
    const g = nvY[ci] + 1;                           // …and the SAME +1 as navGroundAt above, so gc (which comes from it) and g are the same quantity on both branches. nvC is already measured from here — nvBuildCell counts free voxels from ts + 1 — so only the step comparison and the `g > WL` land test move, and both move ONTO the unvouched branch's answer rather than away from it.
    return g > WL && g - gc <= up && gc - g <= down && nvC[ci] >= clr; };
  // (no navSurfOK: a field-gated surface-water predicate was built for the ducks and measured off —
  //  see the duckFit comment in the tick body. The swim band is consumed by the FISH, via navFitsSwim.)
  const navReach2 = (fit, x, z, th, maxD) => {       // navReachAir's DDA over a 2-D closure — every cell the path crosses is tested, so the planner cannot approve a lane the mover will refuse
    const dx = Math.sin(th), dz = Math.cos(th), CS = 1 << NVSH;
    const adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
    const cx = Math.floor(x / CS), cz = Math.floor(z / CS);
    const dtX = adx < 1e-9 ? Infinity : CS / adx, dtZ = adz < 1e-9 ? Infinity : CS / adz;
    let tX = adx < 1e-9 ? Infinity : (dx > 0 ? (cx + 1) * CS - x : x - cx * CS) / adx;
    let tZ = adz < 1e-9 ? Infinity : (dz > 0 ? (cz + 1) * CS - z : z - cz * CS) / adz;
    let t = 0;
    for (let g = 0; g < 80; g++) {
      t = tX <= tZ ? tX : tZ;
      if (t >= maxD) return maxD;
      if (tX <= tZ) tX += dtX; else tZ += dtZ;
      if (!fit(x + dx * (t + 0.02), z + dz * (t + 0.02))) return t;   // the clear run ends AT this boundary
    }
    return t; };
  const navReachLand = (x, z, th, maxD, gc, up, down, clr) => {   // how far a WALKER can actually travel along a heading. Same cell DDA, one difference that matters: the step limit is carried FORWARD from cell to cell instead of being measured from the origin. A worm climbs a long slope two voxels at a time, and a reach anchored on where it started would have called every hill a wall and left the planner with no lane anywhere but flat ground.
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
      if (!navLandOK(px, pz, g, up, down, clr)) return t;   // the walkable run ends AT this boundary
      g = navGroundAt(px, pz);                          // …and the next cell is judged from THIS one's surface
    }
    return t; };
  // ══ THE WALKER ══ the four functions above, once more over the SAME planes minus the ground clutter.
  // They are a deliberate copy, not a parameter on the worm's: navLandOK/navGroundAt/navReachLand are the
  // measured control for a band that is already shipped, and the file's own policy (see navReachAir vs
  // navReach2) is that a wired band's arithmetic is left character for character alone. The ONLY
  // difference in the bodies below is nvClutD — nvY minus it is the walker's floor, nvC plus it is the
  // walker's headroom — so nvY, nvC, navFitsAir and the conservativeness proof are all untouched.
  const NAV_MUP = 3, NAV_MDN = 4, NAV_MCLR = 4;      // MAMMAL step up / step down / body-top PROBE OFFSET — the marchers' OWN walkOK numbers verbatim ((gA−cur) ≤ 3, (cur−gA) ≤ 4, bfObstW probed at +2 and +4, so the body occupies g+1…g+4 and 4 contiguous free voxels is that same body)
  const NAV_MSTA = 5;                                // the marcher's lookahead station, 5 voxels ahead — the distance its own walkOK gate was authored at
  const NAV_MLOOK = 8, NAV_MBCLR = 1.0, NAV_MBRK2 = 340;   // MAMMAL brake: sqrt(340 · (8 − 1)) = 48.8 vox/s, one notch above the fleeing skunk's 48, so an open lane is full pace and the cap withholds exactly nothing until the lane is under 8 voxels
  const navWalkGround = (x, z) => { const ci = nvIdx(x, z);   // the CELL travel surface for a walker: navGroundAt minus the clutter it steps over. Conservative by construction (2x2 MAX), so it is the right number for judging the world AHEAD — see navWalkStand below for the number a body is SEATED on, which is not the same question.
    if (!(nvF[ci] & NVF_BUILT)) return navBed(x, z) > WL ? navBed(x, z) : WL;
    const g = nvY[ci] - nvClutD(ci) + 1;             // ── UNITS ── +1 because nvY is the topmost SOLID voxel while hmap is the FIRST AIR voxel above it (worldgen writes the surface at h-1), and hmap is what bfSurf returns and what every step limit and probe offset in the marchers was authored against. Without it the whole band walked one voxel INSIDE the ground — and the unvouched branch one line up, which returns hmap, would have disagreed with the vouched one by that same voxel at every streaming frontier.
    return ((nvF[ci] & NVF_WATER) && g < WL) ? WL : g; };
  const navWalkStand = (x, z) => {                    // the surface a walker's FEET are on — which is NOT the surface it judges the world ahead with. navWalkGround is the 2×2 MAX, and that is exactly right for "can I go there": it can only ever refuse too much. But a cell straddling a TREE TRUNK reports the trunk's top, and a body seated on that number is standing in mid-air — measured on live mammals, 0.33% of frames and up to 16 voxels of lift. A walker's feet can only rest on something it could have stepped onto, so anything more than one step-up above the heightmap column is the field describing the neighbour that EXCLUDED this cell, not the floor under this animal.
    const h = navBed(x, z) > WL ? navBed(x, z) : WL, g = navWalkGround(x, z);
    return g - h > NAV_MUP ? h : g; };
