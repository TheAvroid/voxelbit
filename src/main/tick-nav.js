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
    const foliageSet = new Uint8Array(256); for (const f of foliageIds) foliageSet[f] = 1;   // green canopy palette ids → the perch surface
    const isAir = (x, y, z) => !W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    const pineEdgePerch = (tx, tz, bi) => {                // green voxels on the OUTER EDGE of the crown (air above + air to a side, ≥3 from the trunk), BELOW the crown tip → perch on the branch tips, NOT the very top (user)
      const cands = []; let crownTop = -1;
      for (let dx = -9; dx <= 9; dx++) for (let dz = -9; dz <= 9; dz++) {
        if (Math.abs(dx) + Math.abs(dz) < 3) continue;   // skip the trunk core
        const x = tx + dx, z = tz + dz, gx = gwrap(x, WX), gz = gwrap(z, WZ), col = gx + gz * WX * WY;
        const yTop = Math.min(WY - 2, hmap[gx + gz * WX] + 124);
        for (let y = yTop; y > WL; y--) { const id = W[col + y * WX]; if (!id) continue;   // first voxel from the top (air above by construction)
          if (foliageSet[id]) { if (y > crownTop) crownTop = y;
            if (isAir(x + 1, y, z) || isAir(x - 1, y, z) || isAir(x, y, z + 1) || isAir(x, y, z - 1)) cands.push([x, y, z]); }   // outer rim = a horizontal neighbour is open air
          break; }
      }
      const low = cands.filter((c) => c[1] <= crownTop - 4);   // keep clear of the narrow crown TIP → the wider side branches
      const pool = low.length ? low : cands;
      if (!pool.length) return null;
      // PROCEDURAL: the candidate list is already in deterministic scan order over deterministic world voxels, so
      // hashing the tree + bird index picks the SAME needle every time. Birds are now a property of the forest, not
      // of when you happened to walk past. The stride keeps the birds on one pine off each other's perch.
      const k0 = (ihash(tx * 7 + 3, tz * 11 + 5) * pool.length) | 0;
      return pool[(k0 + bi * Math.max(1, (pool.length / 3) | 0)) % pool.length];
    };
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
        for (let j = 0; j < 16; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 0 && !O.dfly && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }   // a dragonfly's water cell must NOT block a butterfly's meadow home
        const R = Math.ceil(LIFE_OUT / FLY_CELL);
        const c0x = Math.floor(P.x / FLY_CELL), c0z = Math.floor(P.z / FLY_CELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = flyHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > LIFE_OUT * LIFE_OUT) continue;
          h.d2 = d2; flyCand.push(h);
        }
      }
      if (!flyCand.length) return null;
      let k = 0;
      for (let q = 1; q < flyCand.length; q++) if (flyCand[q].d2 < flyCand[k].d2) k = q;
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
        for (let j = 32; j < 64; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 2 && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }   // the worm grid is now WORMS-ONLY: every land mammal (bunny/armadillo/skunk/porcupine) reserves on its OWN offset grid (findBunnyHome/findArmHome/findSkunkHome/findPorcHome) — the bunny was the last still sharing, and worms (22 cells) starved it at some locations (measured b=11 vs 14/14/14)
        const R = Math.ceil(LIFE_OUT / WORM_HCELL);
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = wormHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > LIFE_OUT * LIFE_OUT) continue;
          h.d2 = d2; wormCand.push(h);
        }
      }
      if (!wormCand.length) return null;
      const minD2 = selfWk >= 276 ? LIFE_IN * LIFE_IN : 0;   // BUNNIES/ARMADILLOS spawn out past the fog (LIFE_IN floor) like everything else; WORMS may still take the near cells (tiny — a near worm spawn is invisible, and mammals no longer inherit those cells)
      let k = -1;
      for (let q = 0; q < wormCand.length; q++) if (wormCand[q].d2 >= minD2 && (k < 0 || wormCand[q].d2 < wormCand[k].d2)) k = q;
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
    const findSkunkHome = () => {                           // identical machinery to findWormHome, but on the skunk grid + reserving ONLY against other skunks (324-347) — its own cache
      if (skunkCandF !== frame) {
        skunkCandF = frame; skunkCand.length = 0;
        const owned = new Set();
        for (let j = 324; j < 348; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (680·0.94) — LIFE_OUT under-scanned at default views, silently capping the reach at ~480
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = skunkHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // NO inner floor + MAM_OUT ceiling (user): mammals fill the forest like the perched songbirds — nearest-first from the player out to the BIRD reach (~536, rect-clipped). Tradeoff: a mammal can occasionally recycle into view as you walk (the perched-bird model has no near floor either).
          h.d2 = d2; h.ord = ihash(h.cx * 431 + 9, h.cz * 433 + 21); skunkCand.push(h);   // ord = deterministic scatter key (see the pick below)
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
    const findPorcHome = () => {                            // identical machinery to findSkunkHome, but on the porcupine grid + reserving ONLY against other porcupines (348-371) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (porcCandF !== frame) {
        porcCandF = frame; porcCand.length = 0;
        const owned = new Set();
        for (let j = 348; j < 372; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = porcHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 431 + 9, h.cz * 433 + 21); porcCand.push(h);   // ord = deterministic scatter key (see the pick below)
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
    const findBunnyHome = () => {                           // identical machinery to findSkunkHome, on the bunny grid + reserving ONLY against other bunnies (276-299) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (bunnyCandF !== frame) {
        bunnyCandF = frame; bunnyCand.length = 0;
        const owned = new Set();
        for (let j = 276; j < 300; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = bunnyHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 431 + 9, h.cz * 433 + 21); bunnyCand.push(h);   // ord = deterministic scatter key (see the pick below)
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
    const findArmHome = () => {                             // identical machinery to findSkunkHome, on the armadillo grid + reserving ONLY against other armadillos (300-323) — its own cache + the LIFE_IN..LIFE_OUT spawn band
      if (armCandF !== frame) {
        armCandF = frame; armCand.length = 0;
        const owned = new Set();
        for (let j = 300; j < 324; j++) { const O = wbf[j]; if (O && O.init && O.hcx !== undefined) owned.add(O.hcx + ',' + O.hcz); }
        const R = Math.ceil(MAM_OUT / WORM_HCELL);     // scan to the MAMMAL ring (see skunk)
        const c0x = Math.floor(P.x / WORM_HCELL), c0z = Math.floor(P.z / WORM_HCELL);
        for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
          const h = armHome(c0x + dx, c0z + dz); if (!h) continue;
          if (owned.has(h.cx + ',' + h.cz)) continue;
          if (h.x <= rect.xlo + 8 || h.x >= rect.xhi - 8 || h.z <= rect.zlo + 8 || h.z >= rect.zhi - 8) continue;
          const ddx = h.x - P.x, ddz = h.z - P.z, d2 = ddx * ddx + ddz * ddz;
          if (d2 > MAM_OUT * MAM_OUT) continue;              // no inner floor + MAM_OUT ceiling — fills the forest out to the bird reach (see skunk)
          h.d2 = d2; h.ord = ihash(h.cx * 431 + 9, h.cz * 433 + 21); armCand.push(h);   // ord = deterministic scatter key (see the pick below)
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
      for (let j = 64; j < 244; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 5) owned.add(O.tx + ',' + O.tz + ',' + O.bi); }
      const R = Math.ceil(CARD_KEEP / TCELL);
      const c0x = Math.floor(P.x / TCELL), c0z = Math.floor(P.z / TCELL);
      for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) {
        const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
        if (tr.tx <= rect.xlo + 10 || tr.tx >= rect.xhi - 10 || tr.tz <= rect.zlo + 10 || tr.tz >= rect.zhi - 10) continue;
        const ddx = tr.tx - P.x, ddz = tr.tz - P.z, d2 = ddx * ddx + ddz * ddz;
        if (d2 > CARD_KEEP * CARD_KEEP) continue;
        const n = birdsOnPine(tr.tx, tr.tz);
        for (let i = 0; i < n; i++) if (!owned.has(tr.tx + ',' + tr.tz + ',' + i)) cardCand.push({ tr, bi: i, d2 });
      }
    };
    // == PERCHES THE PLAYER HAS CLEARED == the clash test below only sees ACTIVE slots, and a slain bird has init=false, so the perch it just
    // vacated became the NEAREST free candidate again and the next spare slot took it — kill a bird and another lands in the same spot (user 2026-08-06).
    // Killing one empties that branch for the session, matching the rule the slot itself already follows: a slain creature is gone, not relocated.
    // Capped so a long session cannot grow it without bound; the cap is far past what anyone shoots in one sitting.
    const findPineCrown = (selfWk) => {                // the NEAREST procedural bird that no slot holds yet
      if (cardCandF !== frame) { cardCandF = frame; buildCardCand(); }
      for (let t = 0; t < 12 && cardCand.length; t++) {
        let k = 0;                                     // nearest first, so the pool is always spent on the forest around you.
        for (let q = 1; q < cardCand.length; q++) if (cardCand[q].d2 < cardCand[k].d2) k = q;
        const c = cardCand[k];
        cardCand[k] = cardCand[cardCand.length - 1]; cardCand.pop();
        if (cardSlainPerch.has(cardPerchKey(c.tr.tx, c.tr.tz, c.bi))) continue;   // a bird was killed on this branch — leave it empty
        const cr = pineEdgePerch(c.tr.tx, c.tr.tz, c.bi); if (!cr) continue;
        let clash = false;                             // the cardinal model is ~7 vox across; two stamps closer than that corrupt each other
        for (let j = 64; j < 244; j++) {
          const O = wbf[j];
          if (j === selfWk || !O || !O.init || (O.kind | 0) !== 5) continue;
          if (Math.abs(O.x - (cr[0] + 0.5)) < CARD_SEP && Math.abs(O.z - (cr[2] + 0.5)) < CARD_SEP &&
              Math.abs((O.perchFeet || 0) - (cr[1] + 1)) < CARD_SEP) { clash = true; break; }
        }
        if (clash) continue;
        return { tx: c.tr.tx, tz: c.tr.tz, bi: c.bi, x: cr[0] + 0.5, y: cr[1], z: cr[2] + 0.5 };
      }
      return null;
    };
