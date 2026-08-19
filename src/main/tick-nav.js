    // ── NAVIGATION SOLIDITY ── these answer "is this voxel a wall" for every creature, and they must answer it
    // the SAME WAY the player's own solid() does. They used to decide it from a PALETTE-ID list (CREATURE_IDS),
    // which is the set of ids the perched songbirds happen to stamp — and because the 256-entry palette is full
    // and shared, that list aliased 84 of 256 ids. Measured: id 11 alone is ~16% of the solid geometry in a
    // sampled volume, so roughly a sixth of the world was a wall to the player and INVISIBLE to every creature.
    // The exclusion a creature actually needs is "don't treat another creature's stamped voxels as terrain",
    // and that is `stampedIdx` — exact, per-CELL, and already maintained. CREATURE_IDS is no longer a
    // navigation concept (it survives only as the render/animation flag it always was).
    const bfObst = (x, y, z) => {                      // obstacle test — anything the PLAYER would collide with, minus soft strands flyers pass through
      const yy = Math.max(1, Math.min(WY - 1, Math.floor(y)));
      const ii = gwrap(Math.floor(x), WX) + yy * WX + gwrap(Math.floor(z), WZ) * WX * WY;
      const id = W[ii];
      if (id === 0 || solidTab[id] !== 1) return false;   // palette truth, exactly as solid() reads it
      if (foliaTab[id] || coneTab[id]) return false;      // LEAVES ARE NOT AN OBSTACLE (user) — flyers pass straight through a crown; cones have no hitbox for the player either
      if (SNOW_PASS.has(id) || SNOW_FERN.has(id)) return false;   // soft grass/bloom/fern strands
      return !stampedIdx.has(ii); };                      // …and another creature's own stamped body is not terrain
    const bfObstW = (x, y, z) => {                     // WORM obstacle test — same as bfObst but also passes small ground clutter (cones/sticks/field stones) so a crawling worm never pins on them
      const yy = Math.max(1, Math.min(WY - 1, Math.floor(y)));
      const ii = gwrap(Math.floor(x), WX) + yy * WX + gwrap(Math.floor(z), WZ) * WX * WY;
      const id = W[ii];
      if (id === 0 || solidTab[id] !== 1) return false;
      if (foliaTab[id] || coneTab[id]) return false;
      if (SNOW_PASS.has(id) || SNOW_FERN.has(id) || WORM_PASS.has(id)) return false;
      return !stampedIdx.has(ii); };
    const bfSurf = (x, z) => Math.max(hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX], WL);
    const bfGlide = (x, z) => {                        // MAX terrain over a wide stencil → the flight height RIDES THE RIMS, gliding over ravines/gorges instead of diving in
      let m = bfSurf(x, z);
      m = Math.max(m, bfSurf(x + 22, z), bfSurf(x - 22, z), bfSurf(x, z + 22), bfSurf(x, z - 22));
      m = Math.max(m, bfSurf(x + 48, z), bfSurf(x - 48, z), bfSurf(x, z + 48), bfSurf(x, z - 48));
      return m; };
    const bfRoofed = (x, y, z) => bfObst(x, y + 7, z) || bfObst(x, y + 13, z);   // solid within ~1.3 m overhead → under canopy/overhang or inside a cave
    const bfSky = (x, y, z) => {                       // truly OPEN TO THE SKY: no SOLID voxel (rock/dirt — cave roofs) anywhere in ~10 m overhead. Foliage doesn't count — lakeside trees may lean over the water.
      const gx = gwrap(Math.floor(x), WX), gz = gwrap(Math.floor(z), WZ), b2 = gx + gz * WX * WY;
      const yTop = Math.min(WY - 1, Math.floor(y) + 104);
      for (let yy = Math.floor(y) + 5; yy <= yTop; yy += 3) { const id = W[b2 + yy * WX]; if (id !== 0 && solidTab[id]) return false; }
      return true; };
    const bfWater = (x, z) => waterAt(Math.floor(x), WL, Math.floor(z));   // REAL water surface voxel present — NOT just "ground below WL". A dry ravine/gorge (rock + lava, carved deep) has hmap < WL so bfSurf lies (returns WL), but has NO WATER_T here → this is the truth.
    const bfBed = (x, z) => hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX];   // the RAW ground height — under a lake this is the BED (bfSurf clamps to WL and lies); the fish's floor
    const bfOpenW = (x, z) => {                        // LAKE water only (user: no ducks/lilies in gorge/ravine/cave water): the spot must sit in WIDE OPEN, REAL water.
      if (!bfWater(x, z)) return false;                // (1) actually ON water — kills dry sub-WL ravines outright; (2) WIDE — a water gorge strip is narrow, a lake is broad.
      let w9 = 0, w18 = 0;
      for (let k = 0; k < 8; k++) {
        const a = k * 0.785398, sa = Math.sin(a), ca = Math.cos(a);
        if (bfWater(x + sa * 9, z + ca * 9)) w9++;
        if (bfWater(x + sa * 18, z + ca * 18)) w18++;
      }
      return w9 >= 7 && w18 >= 5; };
    __vb.bfOpenW = bfOpenW; __vb.bfSurf = bfSurf; __vb.bfWater = bfWater;   // test taps — headless checks must use the game's OWN gates (worldgen H is pre-carve, voxel scans count canopy)
    __vb.bfObst = bfObst; __vb.bfObstW = bfObstW;
    __vb.obstAudit = (n) => {                          // how far the CREATURES' notion of a wall has drifted from the PLAYER's. Sampled over the built rect.
      n = n || 200000;
      let solidN = 0, obstN = 0, ghost = 0, phantom = 0, sampled = 0;   // ghost = solid to the player, passable to a creature (the bug); phantom = the reverse
      let ghostOld = 0, phantomOld = 0, obstOld = 0;                    // …and the same three under the PRE-FIX palette-id rule, so before/after come out of one run
      const bfObstLegacy = (xx, yy2, zz) => { const yv = Math.max(1, Math.min(WY - 1, Math.floor(yy2)));
        const idv = W[gwrap(Math.floor(xx), WX) + yv * WX + gwrap(Math.floor(zz), WZ) * WX * WY];
        return idv !== 0 && !foliaTab[idv] && !SNOW_PASS.has(idv) && !SNOW_FERN.has(idv) && !CREATURE_IDS.has(idv); };
      const byId = {}, byIdOld = {};
      const x0 = Math.max(rect.xlo + 8, P.x - 320), x1 = Math.min(rect.xhi - 8, P.x + 320);
      const z0 = Math.max(rect.zlo + 8, P.z - 320), z1 = Math.min(rect.zhi - 8, P.z + 320);
      if (!(x1 > x0 && z1 > z0)) return null;
      for (let q = 0; q < n; q++) {
        const x = x0 + Math.random() * (x1 - x0), z = z0 + Math.random() * (z1 - z0);
        const g = hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX];
        const y = Math.max(1, Math.min(WY - 2, Math.floor(g - 20 + Math.random() * 130)));   // the band creatures actually fly/walk/burrow through
        sampled++;
        const ii = gwrap(Math.floor(x), WX) + y * WX + gwrap(Math.floor(z), WZ) * WX * WY;
        const id = W[ii];
        const sp = solid(Math.floor(x), y, Math.floor(z));   // the PLAYER's own answer
        const ob = bfObst(x, y, z), obL = bfObstLegacy(x, y, z);
        if (sp) solidN++;
        if (ob) obstN++;
        if (obL) obstOld++;
        if (sp && !ob) { ghost++; byId[id] = (byId[id] || 0) + 1; }
        if (ob && !sp) phantom++;
        if (sp && !obL) { ghostOld++; byIdOld[id] = (byIdOld[id] || 0) + 1; }
        if (obL && !sp) phantomOld++;
      }
      const topOf = (m) => Object.entries(m).sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, v]) => [+k, +(v / Math.max(1, solidN) * 100).toFixed(2)]);
      return { sampled, solidN, obstN, obstOld, ghost, phantom, ghostOld, phantomOld,
        ghostPctOfSolid: +(ghost / Math.max(1, solidN) * 100).toFixed(2),
        ghostPctOfSolidOld: +(ghostOld / Math.max(1, solidN) * 100).toFixed(2),
        phantomPctOfSolid: +(phantom / Math.max(1, solidN) * 100).toFixed(2),
        phantomPctOfSolidOld: +(phantomOld / Math.max(1, solidN) * 100).toFixed(2),
        topGhostIds: topOf(byId), topGhostIdsOld: topOf(byIdOld) }; };
    // ── FISH NAVIGATION PROBES ── ONE feasibility predicate, shared by the swimmer's PLANNER and its BLOCKED-ESCAPE so the
    // two can never disagree. That disagreement was the "endlessly swims against the terrain" bug (user): the planner
    // sampled a SINGLE COLUMN every 4 voxels while the mover tested the whole ~10-voxel body, so it kept picking headings
    // the body could not fit through — the move was rejected every frame, the heading was never revised, and the fish
    // ground into the rock until the 12 s mercy-recycle. A planner that can only choose moves the mover will accept
    // cannot deadlock this way.
    const fishBodyAt = (cx, cz, th, ay, half) => {     // whole body (nose→tail, over its full height) clear of solid — THE one body test: the planner, stepOK and the render hitbox all call this, so a sample set can never drift apart again (the tail was -4.5 in stepOK but -5 in the hitbox, so a move the planner allowed could still leave the tail clipped)
      const hx = Math.sin(th), hz = Math.cos(th), hf = half || 5;   // scaled to the SPECIES: a minnow is a fraction of a salmon and must not be navigated as one
      for (const a of [hf, hf * 0.5, 0, -hf * 0.5, -hf]) { const bx = Math.floor(cx + hx * a), bz = Math.floor(cz + hz * a);
        if (solid(bx, ay - 1, bz) || solid(bx, ay, bz) || solid(bx, ay + 1, bz) || solid(bx, ay + 2, bz)) return false; }
      return true; };
    const fishWaterOK = (x, z, y) => (NAVARB && nvOn) ? navFitsSwim(x, y, z) : (bfWater(x, z) && WL - bfBed(x, z) >= 3);   // ── THE FISH'S WATER ANSWER ── bfBed is hmap, the RAW heightmap: it knows nothing of the rock, shells and seagrass stamped onto the bed, so "deep enough to hold a body" was measured against a floor that is not there. nvD counts the swim band VOXEL BY VOXEL from the real bed to the waterline, and NVF_SWIM demands the whole 2×2 be wet. Same threshold (NV_MIND = 3), honest floor.
    const fishFitsAt = (B, x, z, th) => fishWaterOK(x, z, Math.floor(B.y)) && fishBodyAt(x, z, th, Math.floor(B.y), B.fhalf);   // THE one fish answer, at a POINT. The heading stays a parameter: a fish's body sweeps a different footprint at every yaw, so the safe pose is heading-dependent by construction and must not be collapsed onto B.th.
    const fishFits = (B, th, dist) => fishFitsAt(B, B.x + Math.sin(th) * dist, B.z + Math.cos(th) * dist, th);   // …and along a heading, exactly as before — the whisker fan, the escape, the leap validation and stepOK all still come through here
    const fishReach = (B, th, maxD) => {               // how far the BODY can actually travel along a heading.
      // STEP = the body's own half-length, floored at 2.5. Each fishFits samples nose→tail over ~2·fhalf voxels, so
      // consecutive probes spaced fhalf apart still cover EVERY voxel along the ray with 2× overlap — nothing can hide
      // between samples. The old flat 2.5 stride re-tested the same voxels ~4× per ray, and since a CLEAR heading never
      // breaks early it was the most expensive case: fish nav hit 17.9% of the main thread at 24 fish. Same coverage,
      // same decisions, roughly half the probes.
      const st = Math.max(2.5, (B.fhalf || 5) * 0.9);
      let r = 0;
      for (let d = st; d <= maxD; d += st) { if (!fishFits(B, th, d)) break; r = d; }
      return r; };
    // ── PERCHED CARDINALS ── the rotate-frame cardinal sits on a pine crown (feet on the green needles) and plays its spin animation. Uses the disabled lily slots (40-54).
    const foliageSet = new Uint8Array(256); for (const f of foliageIds) foliageSet[f] = 1;   // green canopy palette ids → the perch surface. VERIFIED to already cover the OAKS: assets/material-tabs.js line 139 pushes every OAKLEAF id into foliageIds beside the pine's needles, and that fragment is 55 places above this one in src/manifest.txt, so this array is built after them. The perch SURFACE test therefore needed no change at all for the new biome — only the walk that visits it.
    const isAir = (x, y, z) => !W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    // ── ONE CROWN WALK, TWO TREES ── green voxels on the OUTER EDGE of the crown (air above + air to a side,
    // clear of the trunk), BELOW the crown tip → perch on the branch tips, NOT the very top (user). That
    // question is the same for a pine and for an oak; the SHAPE of the crown is not, so the shape — and only
    // the shape — is a parameter. The pine's arguments below are its original numbers, unchanged.
    //   rad   half-width of the walk, in voxels either side of the trunk
    //   st    column stride, so a crown 6x wider does not cost 36x the columns for the same three perches
    //   tL1   trunk core skipped as a DIAMOND (|dx|+|dz|) — the pine's own test, a 4-voxel spine is a line
    //   tCh   …or as a SQUARE (max(|dx|,|dz|)) — an oak bole is a fat column, not a line. One or the other.
    //   tip   how far below the crown's highest candidate a perch must sit
    //   scanH how high above the local ground the downward column scan starts
    //   n     birds this tree carries, so the stride that keeps them off each other's perch scales with it
    const crownEdgePerch = (tx, tz, bi, n, rad, st, tL1, tCh, tip, scanH) => {
      const cands = []; let crownTop = -1;
      for (let dx = -rad; dx <= rad; dx += st) for (let dz = -rad; dz <= rad; dz += st) {
        if (Math.abs(dx) + Math.abs(dz) < tL1) continue;   // skip the trunk core (pine)
        if (Math.max(Math.abs(dx), Math.abs(dz)) < tCh) continue;   // …or the bole (oak)
        const x = tx + dx, z = tz + dz, gx = gwrap(x, WX), gz = gwrap(z, WZ), col = gx + gz * WX * WY;
        const yTop = Math.min(WY - 2, hmap[gx + gz * WX] + scanH);
        for (let y = yTop; y > WL; y--) { const id = W[col + y * WX]; if (!id) continue;   // first voxel from the top (air above by construction)
          if (foliageSet[id]) { if (y > crownTop) crownTop = y;
            if (isAir(x + 1, y, z) || isAir(x - 1, y, z) || isAir(x, y, z + 1) || isAir(x, y, z - 1)) cands.push([x, y, z]); }   // outer rim = a horizontal neighbour is open air
          break; }
      }
      const low = cands.filter((c) => c[1] <= crownTop - tip);   // keep clear of the crown TIP → the wider side branches
      const pool = low.length ? low : cands;
      if (!pool.length) return null;
      // PROCEDURAL: the candidate list is already in deterministic scan order over deterministic world voxels, so
      // hashing the tree + bird index picks the SAME needle every time. Birds are now a property of the forest, not
      // of when you happened to walk past. The stride keeps the birds on one tree off each other's perch — floored
      // at 3 so a pine (n ≤ 3) divides by exactly the 3 it always did, and widened for an oak, where n can reach 6
      // and a fixed /3 would have wrapped bi=3 back onto bi=0's own branch.
      const k0 = (ihash(tx * 7 + 3, tz * 11 + 5) * pool.length) | 0;
      return pool[(k0 + bi * Math.max(1, (pool.length / Math.max(3, n)) | 0)) % pool.length];
    };
    const pineEdgePerch = (tx, tz, bi, n) => crownEdgePerch(tx, tz, bi, n, 9, 1, 3, 0, 4, 124);   // pine5.vox: ONE model, 35 x 36 across and 116 tall, so every number here can be a constant. Unchanged in value and in scan order — this call reproduces the old body exactly.
    // ── AND THE SAME WALK RE-DERIVED FOR AN OAK ── not one of the pine's four shape numbers survives the move,
    // and each is measured off game/assets/decoration/oak_trees.json, the bake oakAt itself indexes:
    //   rad  the crown's own half-width, straight off the model box: max(sx, sy) >> 1. Checked against the bake,
    //        that is EXACTLY the widest leaf voxel's Chebyshev distance from the trunk on all seven models
    //        (17/22/23/30/37/50/57, against the pine's 17). Rotation only swaps sx and sy, so the max is
    //        rotation-invariant and the walk never needs tr.rot. The pine's flat ±9 would have seen the middle
    //        19 voxels of a 115-voxel footprint — 2.7% of its columns, and all of them under the crown CAP,
    //        which is the one part `tip` then throws away. That is the whole of the miss-oak-perches half.
    //   st   …which is also why the walk cannot stay dense: 115 x 115 columns is 36x the pine's 361 for the same
    //        handful of perches. Stride so every tree costs ~19x19 samples whatever its size; falls out at 1 for
    //        the small oaks and 7 for the giant, i.e. the giant is CHEAPER to walk than a pine. A ≥ st gap
    //        between candidates is a free bonus: on a big oak it already exceeds CARD_SEP.
    //   tCh  the BOLE, measured off the bake's own non-foliage voxels below the first fork — the bottom sz/8,
    //        which sits clear beneath every model's fork (k1 z12, k2 z17, k3 z12, k4 z16, k5 z25, k6 z17). Bole
    //        radius comes out 2-8 voxels, plus half the bird's own 8-voxel footprint, so 6-12 against the pine
    //        diamond's 2. Deliberately measured BELOW the fork: higher up the bark reaches 43 voxels out on the
    //        giant, but that is a LIMB, and a limb is a perch, not an obstacle.
    //   tip  a pine tapers to a point and 4 voxels clears it; a dome does not. Scaled to the model at sz >> 3
    //        (6/6/8/10/13/14). Measured against the bake's top-voxel profile that is what drops the crown cap
    //        and keeps the flank: on OAKV[6] the inner ring's median top voxel is 100 of 113 and the rim's is 62.
    //   scan the pine walk starts a blanket 124 above the ground because a pine is 116 tall. An oak is 21 to 114,
    //        so start from the model's own height plus a slope allowance — 24 voxels covers a 2.4 m fall from
    //        the trunk out to the drip line, and ground steeper than that would not be holding a tree up anyway.
    //        Starting too low only finds a LOWER perch, never a wrong one, so this margin is soft.
    const OKPERCH = [];                                // per-model perch geometry, measured once off the bake and cached — OAKV[6] is 86k voxels and this walks it exactly once per session
    const oakPerchGeo = (k) => {
      let g = OKPERCH[k]; if (g) return g;
      const m = OAKV[k]; if (!m) return null;
      const lowZ = m.sz >> 3, ax = m.sx >> 1, ay = m.sy >> 1;   // ax/ay = where stampModel puts the world anchor in model space (bottom-CENTRE)
      let bole = 0;
      for (let i = 0; i < m.vox.length; i++) { const p = m.vox[i];
        if (((p >> 16) & 255) > lowZ || foliaTab[p >>> 24]) continue;   // wood only, below the first fork — the bush tier is leaf to the ground and must not read as a 12-voxel trunk
        const r = Math.max(Math.abs((p & 255) - ax), Math.abs(((p >> 8) & 255) - ay));
        if (r > bole) bole = r; }
      const rad = Math.max(m.sx, m.sy) >> 1;
      OKPERCH[k] = g = { rad, st: Math.max(1, Math.ceil(rad / 9)), tCh: bole + (CARD_SEP >> 1), tip: Math.max(4, m.sz >> 3), scan: m.sz + 24 };
      return g;
    };
    const oakEdgePerch = (tx, tz, k, bi, n) => { const g = oakPerchGeo(k); return g ? crownEdgePerch(tx, tz, bi, n, g.rad, g.st, 0, g.tCh, g.tip, g.scan) : null; };
    // ── PERCHED BIRD PLACEMENT ── This used to throw 28 random darts at the disc and keep the first that hit a pine.
    // Random darts give a random DENSITY: some stretches of forest end up thick with birds and others bare, which is
    // the "not consistent throughout the forest" complaint. It also could not see which pines were free, so it wasted
    // most of its tries re-drawing occupied ones. Now every pine in range is enumerated once per frame, the occupied
    // ones are removed, and a placement draws UNIFORMLY from what is left — so bird density is even everywhere the
    // pines are, by construction rather than by luck.
    let cardCandF = -1;
    // ── PROCEDURAL BUTTERFLIES ── a butterfly WANDERS, so unlike a perched bird it cannot be pinned to a voxel.
    // What is procedural is its HOME: a deterministic point per 128-vox cell, ~half of cells occupied. Slots activate
    // the nearest free homes, the butterfly is leashed loosely to its home, and it recycles when the HOME leaves
    // range rather than when the insect does — so the meadow you walked through has the same butterflies in it when
    // you come back, and density is a property of the world instead of of when you happened to look.
    const FLY_CELL = 128, FLY_LEASH = 84;              // home cell size / how far a butterfly may drift from its home
    const flyHome = (cx, cz) => {
      if (ihash(cx * 0x27D4 + 91, cz * 0x1656 + 37) >= 0.5) return null;
      return { x: cx * FLY_CELL + 16 + ihash(cx * 13 + 5, cz * 17 + 9) * (FLY_CELL - 32),
               z: cz * FLY_CELL + 16 + ihash(cx * 19 + 3, cz * 23 + 7) * (FLY_CELL - 32), cx, cz };
    };
    let flyCandF = -1;
    const flyCand = [];
    const findFlyHome = (selfWk) => {                  // nearest home no flyer slot holds yet
      if (flyCandF !== frame) {
        flyCandF = frame; flyCand.length = 0;
        const owned = new Set();
        for (let j = FLY_0; j < FLY_END; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 0 && !O.dfly && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }   // a dragonfly's water cell must NOT block a butterfly's meadow home
        const R = Math.ceil(LIFE_OUT / FLY_CELL);
        const c0x = Math.floor(P.x / FLY_CELL), c0z = Math.floor(P.z / FLY_CELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = flyHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > LIFE_OUT * LIFE_OUT) continue;
          h.d2 = d2; h.ord = ihash(h.cx * 421 + 17, h.cz * 419 + 31); flyCand.push(h);   // ord = this species' OWN scatter key (see the skunk finder below — same fix, and this is the half of it that was never applied)
        }
      }
      if (!flyCand.length) return null;
      let k = 0;
      // ── HASH-ORDER SCATTER, NOT NEAREST-FIRST (user 2026-08-18: "the life seems to still be clustering on
      // players spawn ... this has been a very persistent problem") ── it is persistent because the fix below
      // for the four land mammals was never applied here. Nearest-first hands the 16 butterflies the 16
      // CLOSEST homes, so at a home density of ~3.05e-5 per vox² they all land inside r ~409 of a 977 disc —
      // 17.5% of the area holding 100% of them, ~5.7x over-density, and NONE outside it. Homes are written once
      // and never re-rolled, so standing at spawn freezes that fill permanently: measured 20.4%/19.8%/20.2%/
      // 20.2% of all life inside LIFE_IN over 150 s, i.e. dead stable rather than drifting.
      // The hash is area-uniform over the whole disc because the candidate LIST is, so picking the smallest
      // arbitrary key picks uniformly from it.
      for (let q = 1; q < flyCand.length; q++) if (flyCand[q].ord < flyCand[k].ord) k = q;
      const h = flyCand[k];
      flyCand[k] = flyCand[flyCand.length - 1]; flyCand.pop();
      return h;
    };
    // ── PROCEDURAL WORMS ── same argument as the butterflies: a worm crawls, so what is fixed by the world is its
    // HOME, not its position. One deterministic point per 160-vox cell, ~55% occupied, which matches the old ~11
    // worms across the ring. Slots take the nearest free home, the worm is leashed to it, and it recycles when the
    // HOME leaves range — so a patch of ground keeps its worms instead of re-rolling them every time you look away.
    const WORM_HCELL = 160, WORM_LEASH = 70;
    const wormHome = (cx, cz) => {
      if (ihash(cx * 0x3B9A + 29, cz * 0x51ED + 61) >= 0.55) return null;
      return { x: cx * WORM_HCELL + 20 + ihash(cx * 31 + 7, cz * 37 + 11) * (WORM_HCELL - 40),
               z: cz * WORM_HCELL + 20 + ihash(cx * 41 + 13, cz * 43 + 17) * (WORM_HCELL - 40), cx, cz };
    };
    let wormCandF = -1;
    const wormCand = [];
    const findWormHome = (selfWk) => {
      if (wormCandF !== frame) {
        wormCandF = frame; wormCand.length = 0;
        const owned = new Set();
        for (let j = WORM_0; j < WORM_END; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 2 && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }   // the worm grid is now WORMS-ONLY: every land mammal (bunny/armadillo/skunk/porcupine) reserves on its OWN offset grid (findBunnyHome/findArmHome/findSkunkHome/findPorcHome) — the bunny was the last still sharing, and worms (22 cells) starved it at some locations (measured b=11 vs 14/14/14)
        const R = Math.ceil(LIFE_OUT / WORM_HCELL);
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = wormHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > LIFE_OUT * LIFE_OUT) continue;
          h.d2 = d2; h.ord = ihash(h.cx * 409 + 53, h.cz * 401 + 11); wormCand.push(h);   // ord = this species' OWN scatter key (see the skunk finder)
        }
      }
      if (!wormCand.length) return null;
      // ── AND THE WORMS KEEP THE FOG FLOOR TOO (user 2026-08-18) ── this used to be 0 for a worm, on the
      // argument that a worm is tiny enough to appear near you without reading as pop-in. True on its own, but
      // combined with the nearest-first pick below it meant 22 worms took the 22 closest cells every time —
      // ~2.9x over-density inside r 571 and nothing beyond. With the hash scatter the floor costs nothing and
      // the near field stops being the only place a worm can be.
      const minD2 = LIFE_IN * LIFE_IN;
      let k = -1;
      for (let q = 0; q < wormCand.length; q++) if (wormCand[q].d2 >= minD2 && (k < 0 || wormCand[q].ord < wormCand[k].ord)) k = q;   // HASH-ORDER SCATTER — see findFlyHome above
      if (k < 0) return null;
      const h = wormCand[k];
      wormCand[k] = wormCand[wormCand.length - 1]; wormCand.pop();
      return h;
    };
    const skunkHome = (cx, cz) => {                         // SKUNK home grid — CENTRED IN THE GAPS of the worm/bunny/armadillo grid ((cx+0.5) half-cell offset) with its OWN hash, so skunks get a full, INDEPENDENT cell supply (they no longer lose the home race to the earlier bands → count matches the armadillo/bunny) and stay spatially clear of the other creatures
      if (ihash(cx * 0x4D2F + 83, cz * 0x71A3 + 97) >= 0.85) return null;
      return { x: (cx + 0.5) * WORM_HCELL + 20 + ihash(cx * 53 + 19, cz * 59 + 23) * (WORM_HCELL - 40),
               z: (cz + 0.5) * WORM_HCELL + 20 + ihash(cx * 61 + 29, cz * 67 + 31) * (WORM_HCELL - 40), cx, cz };
    };
    let skunkCandF = -1;
    const skunkCand = [];
    const findSkunkHome = () => {                           // identical machinery to findWormHome, but on the skunk grid + reserving ONLY against other skunks (SKUNK_0..SKUNK_END) — its own cache
      if (skunkCandF !== frame) {
        skunkCandF = frame; skunkCand.length = 0;
        const owned = new Set();
        for (let j = SKUNK_0; j < SKUNK_END; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (680·0.94) — LIFE_OUT under-scanned at default views, silently capping the reach at ~480
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = skunkHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // NO inner floor + MAM_OUT ceiling (user): mammals fill the forest like the perched songbirds — nearest-first from the player out to the BIRD reach (~536, rect-clipped). Tradeoff: a mammal can occasionally recycle into view as you walk (the perched-bird model has no near floor either).
          h.d2 = d2; h.ord = ihash(h.cx * 431 + 9, h.cz * 433 + 21); skunkCand.push(h);   // ── ONE SCATTER KEY PER SPECIES ── this hash was copy-pasted identically into all four finders, and because every one of these grids is 85% occupied nearly every 160-cell offers a home to all four. A shared key means all four RANK the cells the same way, so bunny, armadillo, skunk and porcupine kept choosing the same cells and landed 40-113 voxels apart inside them — four-species knots that only the 70-voxel cross-species floor below prised open. MEASURED over 6 boots before changing it: every mammal's nearest neighbour was a DIFFERENT species, pooled nearest-neighbour min 70.2 / p10 79.9 — the whole population jammed against that floor, while the same-species gap sat at 250-280. Four independent keys make the four scatters independent, which is what the separate grids were for.
        }
      }
      if (!skunkCand.length) return null;
      let k = 0;
      for (let q = 1; q < skunkCand.length; q++) if (skunkCand[q].ord < skunkCand[k].ord) k = q;   // HASH-ORDER SCATTER (user: "bunched up by the player") — nearest-first spent the whole 14-slot pool on the near cells (measured: density collapsed past 400 vox). Picking by a per-cell hash instead samples the WHOLE disc area-uniformly out to MAM_OUT, deterministic per world spot, still stratified by the 160-vox grid.
      const h = skunkCand[k];
      skunkCand[k] = skunkCand[skunkCand.length - 1]; skunkCand.pop();
      return h;
    };
    const porcHome = (cx, cz) => {                          // PORCUPINE home grid — offset (cx, cz+0.5) into the OTHER gap of the worm/bunny/armadillo grid, its OWN hash → independent cell supply, spatially clear of the skunk's (cx+0.5, cz+0.5) grid too (user's 4th land mammal)
      if (ihash(cx * 0x2F1B + 41, cz * 0x63C5 + 53) >= 0.85) return null;
      return { x: cx * WORM_HCELL + 20 + ihash(cx * 71 + 37, cz * 73 + 41) * (WORM_HCELL - 40),
               z: (cz + 0.5) * WORM_HCELL + 20 + ihash(cx * 79 + 43, cz * 83 + 47) * (WORM_HCELL - 40), cx, cz };
    };
    let porcCandF = -1;
    const porcCand = [];
    // ── THE FLAMINGO'S OWN HOME GRID ── the same shape as porcHome on its own salt, so its cells are an
    // independent supply rather than a share of the porcupine's. A borrowed grid was only half the bug: the
    // borrowed RESERVATION band was the other half, and together they meant the porcupine (which runs first)
    // emptied the candidate list and the flamingo never got a home.
    const flamHome = (cx, cz) => {
      if (ihash(cx * 0x4D2F + 67, cz * 0x71A3 + 71) >= 0.85) return null;
      return { x: cx * WORM_HCELL + 20 + ihash(cx * 89 + 53, cz * 97 + 59) * (WORM_HCELL - 40),
               z: (cz + 0.5) * WORM_HCELL + 20 + ihash(cx * 101 + 61, cz * 103 + 67) * (WORM_HCELL - 40), cx, cz };
    };
    let flamCandF = -1;
    const flamCand = [];
    const findFlamHome = () => {                            // ── THE FLAMINGO GETS ITS OWN (audit 2026-08-18) ── it borrowed findPorcHome, whose reservation band and
    // candidate list belong to the porcupine; the porcupine runs first and emptied the list, so the flamingo
    // never received a home at all. Its own grid, its own reservations, salted apart so the two do not stack.
    // (identical machinery to findSkunkHome otherwise — see there. Original note: PORC_0..MAM_END) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (flamCandF !== frame) {
        flamCandF = frame; flamCand.length = 0;
        // A COUNT, not a set: the cell stays a candidate until it holds a full pair. Nothing outside this block
        // reads it — the pair test below is inside the block with it, and the MATE OFFSET is not decided here at
        // all but in main/tick-creatures.js, off the live census, where a placement is known to have succeeded.
        // Both ReferenceErrors this file threw at the game loop were the same slip: a `const` (and then a `let`
        // that survived its last reader) declared in this block and named further down, outside it.
        const owned = new Map();
        for (let j = FLAM_0; j < FLAM_END; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) {
          const kF = O.hcx + ',' + O.hcz; owned.set(kF, (owned.get(kF) || 0) + 1); } }
        // PORC_0..FLAM_0, not ..MAM_END: MAM_END grew to take in the flamingo band, so this reserved against flamingos too — and since porcupines run FIRST they drained the candidate list and every flamingo got null. A reservation band must be the species' own slots, not "the rest of the pool"
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = flamHome(c0x + dx, c0z + dz); if (!h) continue;
          if ((owned.get(h.cx + ',' + h.cz) || 0) >= FLAM_PAIR) continue;   // …not `has`: one bird no longer closes the cell, a PAIR does
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 439 + 37, h.cz * 443 + 53); flamCand.push(h);   // ord = this species' OWN scatter key (see the skunk finder)
        }
      }
      if (!flamCand.length) return null;
      let k = 0;
      for (let q = 1; q < flamCand.length; q++) if (flamCand[q].ord < flamCand[k].ord) k = q;   // HASH-ORDER SCATTER — area-uniform over the disc, not nearest-first (see the skunk pick)
      const h = flamCand[k];
      flamCand[k] = flamCand[flamCand.length - 1]; flamCand.pop();
      // NOTE: the PAIRING itself is not done here. This function is asked for a home up to 12 times per slot
      // per frame and most of those tries are then rejected downstream (biome, spacing, rock, obstruction), so
      // anything counted here counts ATTEMPTS, not birds — a cell retired after two rejected tries takes its
      // partner slot with it, which measured as one couple and two singles. The mate offset lives in
      // main/tick-creatures.js instead, on the live census, where a placement is known to have succeeded.
      return h;
    };
    const findPorcHome = () => {                            // identical machinery to findSkunkHome, but on the porcupine grid + reserving ONLY against other porcupines (PORC_0..MAM_END) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (porcCandF !== frame) {
        porcCandF = frame; porcCand.length = 0;
        const owned = new Set();
        for (let j = PORC_0; j < FLAM_0; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }   // PORC_0..FLAM_0, not ..MAM_END: MAM_END grew to take in the flamingo band, so this reserved against flamingos too — and since porcupines run FIRST they drained the candidate list and every flamingo got null. A reservation band must be the species' own slots, not "the rest of the pool"
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = porcHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 439 + 37, h.cz * 443 + 53); porcCand.push(h);   // ord = this species' OWN scatter key (see the skunk finder)
        }
      }
      if (!porcCand.length) return null;
      let k = 0;
      for (let q = 1; q < porcCand.length; q++) if (porcCand[q].ord < porcCand[k].ord) k = q;   // HASH-ORDER SCATTER — area-uniform over the disc, not nearest-first (see the skunk pick)
      const h = porcCand[k];
      porcCand[k] = porcCand[porcCand.length - 1]; porcCand.pop();
      return h;
    };
    const bunnyHome = (cx, cz) => {                        // BUNNY home grid — quarter-offset (cx+0.25, cz+0.25) with its OWN hash → INDEPENDENT cell supply. FIX (measured 2026-07-22: bunny=11 vs 14/14/14): the bunny was the LAST mammal sharing the integer worm grid, and the worms' 22 cells starved it at worm-dense locations. All four land mammals now own a private grid.
      if (ihash(cx * 0x3A87 + 19, cz * 0x59D1 + 71) >= 0.85) return null;
      return { x: (cx + 0.25) * WORM_HCELL + 20 + ihash(cx * 107 + 77, cz * 109 + 81) * (WORM_HCELL - 40),
               z: (cz + 0.25) * WORM_HCELL + 20 + ihash(cx * 113 + 87, cz * 127 + 91) * (WORM_HCELL - 40), cx, cz };
    };
    let bunnyCandF = -1;
    const bunnyCand = [];
    const findBunnyHome = () => {                           // identical machinery to findSkunkHome, on the bunny grid + reserving ONLY against other bunnies (BUNNY_0..BUNNY_END) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (bunnyCandF !== frame) {
        bunnyCandF = frame; bunnyCand.length = 0;
        const owned = new Set();
        for (let j = BUNNY_0; j < BUNNY_END; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = bunnyHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 449 + 67, h.cz * 457 + 89); bunnyCand.push(h);   // ord = this species' OWN scatter key (see the skunk finder)
        }
      }
      if (!bunnyCand.length) return null;
      let k = 0;
      for (let q = 1; q < bunnyCand.length; q++) if (bunnyCand[q].ord < bunnyCand[k].ord) k = q;   // HASH-ORDER SCATTER — area-uniform over the disc, not nearest-first (see the skunk pick)
      const h = bunnyCand[k];
      bunnyCand[k] = bunnyCand[bunnyCand.length - 1]; bunnyCand.pop();
      return h;
    };
    const armHome = (cx, cz) => {                          // ARMADILLO home grid — the 4th quadrant offset (cx+0.5, cz), its OWN hash → INDEPENDENT cell supply. FIX (measured 2026-07-22: armadillo=6 vs 14 for the others): the armadillo used to share the integer worm grid with worms+bunnies and, processed LAST, was starved of free cells. Its own grid fixes the imbalance, matching skunk/porcupine.
      if (ihash(cx * 0x1BD7 + 67, cz * 0x5C9F + 29) >= 0.85) return null;
      return { x: (cx + 0.5) * WORM_HCELL + 20 + ihash(cx * 89 + 51, cz * 97 + 59) * (WORM_HCELL - 40),
               z: cz * WORM_HCELL + 20 + ihash(cx * 101 + 63, cz * 103 + 71) * (WORM_HCELL - 40), cx, cz };
    };
    let armCandF = -1;
    const armCand = [];
    const findArmHome = () => {                             // identical machinery to findSkunkHome, on the armadillo grid + reserving ONLY against other armadillos (ARM_0..ARM_END) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (armCandF !== frame) {
        armCandF = frame; armCand.length = 0;
        const owned = new Set();
        for (let j = ARM_0; j < ARM_END; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = armHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 461 + 97, h.cz * 463 + 113); armCand.push(h);   // ord = this species' OWN scatter key (see the skunk finder)
        }
      }
      if (!armCand.length) return null;
      let k = 0;
      for (let q = 1; q < armCand.length; q++) if (armCand[q].ord < armCand[k].ord) k = q;   // HASH-ORDER SCATTER — area-uniform over the disc, not nearest-first (see the skunk pick)
      const h = armCand[k];
      armCand[k] = armCand[armCand.length - 1]; armCand.pop();
      return h;
    };
    const CARD_PER_TREE = 3;                           // several birds to a pine, but not a roost — unlimited clumped them and the spread measured worse
    const cardCand = [];
    const buildCardCand = () => {
      cardCand.length = 0;
      const owned = new Set();                         // (tree, index) pairs a slot already holds
      for (let j = CARD_0; j < CARD_END; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 5) owned.add(O.tx + ',' + O.tz + ',' + O.bi); }
      const R = Math.ceil(CARD_KEEP / TCELL);
      const c0x = Math.floor(P.x / TCELL), c0z = Math.floor(P.z / TCELL);
      for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
        const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
        if (tr.tx <= rect.xlo + 10 || tr.tx >= rect.xhi - 10 || tr.tz <= rect.zlo + 10 || tr.tz >= rect.zhi - 10) continue;
        const ddx = tr.tx - P.x, ddz = tr.tz - P.z, d2 = ddx * ddx + ddz * ddz;
        if (d2 > CARD_KEEP * CARD_KEEP) continue;
        const n = birdsOnPine(tr.tx, tr.tz);
        for (let i = 0; i < n; i++) if (!owned.has(tr.tx + ',' + tr.tz + ',' + i)) cardCand.push({ tx: tr.tx, tz: tr.tz, k: -1, bi: i, n, d2 , ord: ihash(tr.tx * 397 + 7, tr.tz * 389 + 23) });
      }
      // ── AND THE OAKS, ON THEIR OWN GRID (user 2026-08-17) ── the walk above enumerates PINES, and treeAt now
      // returns null everywhere oakM > 0.5, so in the biome the player actually spawns in it found nothing at
      // all: only the x > border segment of the CARD_KEEP disc held a candidate, none of it nearer than the 420
      // voxels to the first pine, and ~180 of the 180 songbird slots went unused. Same enumeration, same
      // occupancy set, same distance ceiling — a second grid, because oakAt's cell is 112 and treeAt's is 45,
      // and one loop over the coarser of the two would miss most pines. The two passes can never double-count:
      // their biome gates are mutually exclusive by construction (terrain.js gates both on oakM at 0.5).
      const RO = Math.ceil(CARD_KEEP / OKCELL);
      const o0x = Math.floor(P.x / OKCELL), o0z = Math.floor(P.z / OKCELL);
      for (let dz = -RO; dz <= RO; dz++) for (let dx = -RO; dx <= RO; dx++) {
        const tr = oakAt(o0x + dx, o0z + dz); if (!tr) continue;
        const n = birdsOnOak(tr.wx, tr.wz, tr.k); if (!n) continue;   // asked FIRST: the bush tier scores 0 and is then never geometry-tested at all
        const g = oakPerchGeo(tr.k); if (!g) continue;
        const mg = g.rad + 2;                          // …and the rect margin is the CROWN's, not the pine's flat 10: the walk samples out to rad, and past the generated rect that reads stale toroidal window data and could perch a bird on a crown that is not there
        if (tr.wx <= rect.xlo + mg || tr.wx >= rect.xhi - mg || tr.wz <= rect.zlo + mg || tr.wz >= rect.zhi - mg) continue;
        const ddx = tr.wx - P.x, ddz = tr.wz - P.z, d2 = ddx * ddx + ddz * ddz;
        if (d2 > CARD_KEEP * CARD_KEEP) continue;
        for (let i = 0; i < n; i++) if (!owned.has(tr.wx + ',' + tr.wz + ',' + i)) cardCand.push({ tx: tr.wx, tz: tr.wz, k: tr.k, bi: i, n, d2 , ord: ihash(tr.wx * 397 + 7, tr.wz * 389 + 23) });
      }
    };
    // == PERCHES THE PLAYER HAS CLEARED == the clash test below only sees ACTIVE slots, and a slain bird has init=false, so the perch it just
    // vacated became the NEAREST free candidate again and the next spare slot took it — kill a bird and another lands in the same spot (user 2026-08-06).
    // Killing one empties that branch for the session, matching the rule the slot itself already follows: a slain creature is gone, not relocated.
    // Capped so a long session cannot grow it without bound; the cap is far past what anyone shoots in one sitting.
    const findPineCrown = (selfWk) => {                // the NEAREST procedural bird that no slot holds yet — PINE **or** OAK now; the name is what tick-creatures.js calls
      if (cardCandF !== frame) { cardCandF = frame; buildCardCand(); }
      for (let t = 0; t < 12 && cardCand.length; t++) {
        // HASH-ORDER SCATTER, not nearest-first (user 2026-08-18). The old comment here said the pool was
        // "always spent on the forest around you" — which is exactly the complaint: 421 perch slots, ~64% of
        // the whole creature pool, all spent on the nearest trees. The perch supply is deliberately larger than
        // CARD_N (see sim/life/slots.js), so whichever end of it goes unspent is the end that looks empty, and
        // nearest-first guarantees that end is the far one. One key per TREE so a tree's perches stay together.
        let k = 0;
        for (let q = 1; q < cardCand.length; q++) if (cardCand[q].ord < cardCand[k].ord) k = q;
        const c = cardCand[k];
        cardCand[k] = cardCand[cardCand.length - 1]; cardCand.pop();
        if (cardSlainPerch.has(cardPerchKey(c.tx, c.tz, c.bi))) continue;   // a bird was killed on this branch — leave it empty
        const cr = c.k < 0 ? pineEdgePerch(c.tx, c.tz, c.bi, c.n) : oakEdgePerch(c.tx, c.tz, c.k, c.bi, c.n);   // c.k = the OAKV size tier, or -1 for a pine (one model, no tier)
        if (!cr) continue;
        let clash = false;                             // the cardinal model is ~7 vox across; two stamps closer than that corrupt each other
        for (let j = CARD_0; j < CARD_END; j++) {
          const O = wbf[j];
          if (j === selfWk || !O || !O.init || (O.kind | 0) !== 5) continue;
          if (Math.abs(O.x - (cr[0] + 0.5)) < CARD_SEP && Math.abs(O.z - (cr[2] + 0.5)) < CARD_SEP &&
              Math.abs((O.perchFeet || 0) - (cr[1] + 1)) < CARD_SEP) { clash = true; break; }
        }
        if (clash) continue;
        return { tx: c.tx, tz: c.tz, bi: c.bi, x: cr[0] + 0.5, y: cr[1], z: cr[2] + 0.5 };
      }
      return null;
    };
