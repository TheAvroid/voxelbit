  // @module - splash, spark and debris particle pools and their per-frame step
  // @exports ARROW_HITS_TO_KILL, KNIFE_HITS_TO_KILL, CRY_GAP, PETAL_FALL, PETAL_MAXLIFE, POL_GAP, POL_MS, HITS_TO_KILL, SPLASH_HI, SPLASH_LIFE, SPLASH_LO, TEAR_HI, TEAR_LO, aimedCreature, hitSpot, hurtHop, lifeDrawnPrev, lifeIsDrawn, petalTick, spawnDeathBurst, spawnPollen, spawnSplash, spawnTear, startCrying, SPK_CARRY_TAU, FLAM_ARROW_HITS
  // ── SPLASH (user 2026-08-05) ── the spark burst, in FOAM: 4 droplets thrown off the WATERLINE whenever
  // something breaks the surface — a fish launching, the same fish coming back down, and the player going
  // either way. Same ballistic arc as a spark; the colour, the spread and the life differ. A splash crown is
  // WIDER than an ember burst, so the horizontal speed is up.
  // ── HIGHER (user 2026-08-05) ── the first version lifted 9-22, which under the shared 85 gravity peaked
  // barely 3 voxels above the surface: it read as a ripple, not a splash. 26-48 throws the column ~4-13
  // voxels (0.4-1.3 m) clear of the water. The LIFE had to grow with it — at 48 a droplet is still climbing
  // 0.56 s in, so the old 0.34-0.56 killed it mid-rise and nothing ever came back down.
  // ── CONSISTENT (user 2026-08-05: "sometimes the splash voxels disappear too quickly") ── the life is now
  // FIXED rather than a 0.62-0.92 roll, so no droplet is visibly shorter-lived than its neighbours, and the
  // burst draws from the splash BAND (see bandSlot) instead of always stamping slots 0-3, which is what let
  // the next splash — or a chop spark — delete a droplet mid-arc.
  // `k` scales the whole thing: a leaping salmon throws more than a duckling-sized ripple.
  const SPLASH_LIFE = 0.9;
  // ── ONLY WHERE YOU CAN SEE THE FISH (user 2026-08-05: "the splash voxels show even when the fish is not
  // visible jumping out of the water") ── a leap threw a splash wherever it happened, and MEASURED on a lake
  // the median leap is 179 voxels from the player and 43 of 53 are past 120. The fish itself breaches fine
  // (peak 10-20 voxels clear of the surface, never a dud) but at that range it is a speck, while four bright
  // foam voxels at the waterline still read clearly — so the splash arrived on its own. Beyond this radius a
  // leap is silent. The PLAYER's own splash is never gated: it is always right under the camera.
  // ── AND THE WHOLE LEAP DECIDES ONCE (user 2026-08-05: "sometimes they play and sometimes they don't") ──
  // the range was re-tested at launch AND again at splash-down, but a leap carries the fish 14-20 voxels
  // sideways, so one that took off at 105 landed at 125: the launch splashed and the re-entry did not. Same
  // jump, one splash. The test now runs ONCE, when the arc is committed, and both ends honour that answer.
  // Radius raised 110 -> 150: 110 was cutting leaps that are plainly visible, which is the other half of the
  // same complaint. The far ones this exists to silence sit at 180-420.
  // ── THE SPLASH FOLLOWS THE FISH, NOT A RADIUS (user 2026-08-05: "splash particles are showing up, but
  // theres no fish being rendered") ── a radius was the wrong test twice over. At 150 it silenced two thirds
  // of the leaps you could see; widened to LIFE_DRAW it started splashing for fish that are inside the radius
  // but LOST THE SLOT CONTEST, because how far a fish is and whether it is drawn are two different questions —
  // the budget hands out 28 slots nearest-first, so a fish at 300 can be well inside 420 and still not be on
  // screen. The only rule that cannot disagree with the renderer is the renderer's own ledger: splash if this
  // fish is actually in a drop slot. Decided once at launch and honoured at re-entry, so an arc still splashes
  // at both ends or neither.
  // lifeUidPrev, not lifeUid: the ledger is wiped at the top of every frame and refilled by the emit, which
  // runs AFTER the creature loop asks this question — reading the live one gave a splash of exactly zero.
  // Last frame is the right answer anyway: it is what the player actually saw a sixtieth of a second ago.
  // Rebuilt from lifeUidPrev at the ledger handoff (same instant, same numbers), so this is the O(1) form of
  // "is 2000 + wk somewhere in lifeUidPrev". It is asked once per fish per frame for the splash, and now once
  // per staged creature for the frustum hysteresis too — a linear scan of every drop slot for both would not do.
  const lifeDrawnPrev = new Uint8Array(wbf.length);
  const lifeIsDrawn = (wk) => lifeDrawnPrev[wk] === 1;
  let splashLast = 0;
  function spawnSplash(wx, wz, k) {
    const now = performance.now();
    splashLast = now;                                  // …no same-frame guard: with the radius now matching the draw distance, two fish really can break the surface in one frame, and the band already refuses to steal a live droplet, so the second one must get its own burst rather than be swallowed
    const s = k === undefined ? 1 : k;
    let placed = 0;
    for (let i = 0; i < 4; i++) {
      const slot = bandSlot(SPLASH_LO, SPLASH_HI); if (slot < 0) break;   // band full — place fewer droplets rather than delete one that is still in the air
      placed++;
      const a = Math.random() * 6.283, sp = (14 + Math.random() * 16) * s;
      sparks3d[slot] = { x: wx + Math.cos(a) * 0.6, y: WL + 0.5, z: wz + Math.sin(a) * 0.6,
        vx: Math.cos(a) * sp, vy: (26 + Math.random() * 22) * s, vz: Math.sin(a) * sp,
        born: now, life: SPLASH_LIFE, ph: Math.random() * 6.283, smoke: false, foam: true };
    }
    PH.stats.splashTry = (PH.stats.splashTry | 0) + 1;
    PH.stats.splashDrops = (PH.stats.splashDrops | 0) + placed;
    if (placed < 4) PH.stats.splashShort = (PH.stats.splashShort | 0) + 1;
  }
  // ── A TEAR (user 2026-08-05) ── one droplet welling out of an orphaned duckling's eye. Same foam voxel
  // the splash throws, but it does not burst: it barely leaves the eye and falls, which is what reads as
  // crying rather than spraying.
  // ── SLOT BANDS ── the 20 particle slots, divided so one effect can never cut another short. Splashes were
  // writing 0-3 unconditionally, which is why they "sometimes disappeared too quickly" (user 2026-08-05): on
  // open water a fish launching, the same fish landing and the player wading all land inside a second, and
  // each burst overwrote the last one MID-FLIGHT. Two bursts' worth of splash slots plus free-slot-first
  // allocation means a droplet now always lives out its full life.
  // ── REBALANCED (user 2026-08-05) ── once the splash radius grew to the draw distance every visible leap
  // started asking for a burst, and eight slots could not hold them: MEASURED, 20 of 69 bursts came out short,
  // averaging 2.8 droplets instead of 4. Tears are the band that can spare them — a brood cries three at a
  // time at CRY_GAP, so four slots is already more than that effect can ever use, while splashes now overlap
  // three deep on open water.
  const SPLASH_LO = 4, SPLASH_HI = 16;                 // 12 slots = three complete bursts in the air at once
  const TEAR_LO = 16, TEAR_HI = 20;                    // 4 slots — three ducklings at CRY_GAP never need more
  const POL_LO = 20, POL_HI = 24;                      // 4 slots — the BEES' pollen trail (user 2026-08-18). Its OWN band, not a share of the tears': several bees can leave a flower at once and each trails repeatedly, so sharing would have had ducklings and bees cutting each other short — the exact failure the note above records being fixed twice.
  const PETAL_LO = 24, PETAL_HI = 56;                 // 32 slots — the FALLING LEAVES (user 2026-08-19: "double the rate of the leafs falling from the trees"; was 16 for the cherry alone). THE BAND HAD TO MOVE WITH THE GAP, and halving the gap on its own would have changed NOTHING: at a ~9.8 s mean life and one spawn per attempt the old 260 ms gap already offered ~37 petals a lifetime into 16 slots, so the band was saturated and every extra attempt hit a full band and returned. The band is the population — that is what this number means and it is the only lever that makes more leaves visible. It also now feeds THREE tree kinds instead of one, over a biome several times the cherry band's size. The band is the population: a petal is spawned whenever one is free, so this number IS how many are in the air, and it self-limits without a counter.
  // Take the next slot in a band: FREE first, then the oldest one holding DEATH SMOKE, else -1.
  // ── NEVER STEAL A DROPLET (user 2026-08-05: "some are cut off real short and others are fine") ── the
  // first version fell back to the oldest live slot whatever it held, which is exactly a droplet being
  // deleted mid-arc. MEASURED on a lake: 60 of 180 droplets — one in three — were taken over at 0.33 s and
  // 0.65 s of their 0.9 s life.
  // ── BUT SMOKE MUST YIELD (user 2026-08-05: "now I don't see the splash droplets 100% of the time") ── a
  // death poof writes 4 sparks + 16 SMOKE across the whole pool, so for the 1.0-1.5 s that plume lives it
  // squats on all eight splash slots and a burst placed ZERO droplets — MEASURED: 8 smoke voxels in the
  // splash band, 0 of 4 droplets placed, the splash simply did not happen. Smoke is a 16-voxel column where
  // one missing voxel cannot be seen; a missing splash can. So smoke yields, and only smoke.
  const bandSlot = (lo, hi) => {
    const now = performance.now();
    let smokeSlot = -1, smokeBorn = Infinity;
    for (let i = lo; i < hi; i++) { const s = sparks3d[i];
      if (!s || (now - s.born) / 1000 >= s.life) return i;
      if (s.smoke && s.born < smokeBorn) { smokeBorn = s.born; smokeSlot = i; }
    }
    return smokeSlot;
  };
  const TEAR_LIFE = 0.7;                               // FIXED, like the splash — a tear that lasted a random 0.75-1.0 read as flickering
  function spawnTear(wx, wy, wz) {
    const slot = bandSlot(TEAR_LO, TEAR_HI); if (slot < 0) return;   // same rule as the splash: skip this tear rather than cut a live one short
    const a = Math.random() * 6.283;
    sparks3d[slot] = { x: wx, y: wy, z: wz,
      vx: Math.cos(a) * 1.6, vy: 1.5 + Math.random() * 2.5, vz: Math.sin(a) * 1.6,   // a gentle well-up, then gravity takes it straight down the cheek
      born: performance.now(), life: TEAR_LIFE, ph: Math.random() * 6.283, smoke: false, foam: true };
  }
  // ── POLLEN (user 2026-08-18: "as the bee leaves the flower, have white voxels come out of its tail end
  // repeatedly ... to represent the bee pollinating the flower") ── the duckling's tear, re-aimed. Same foam
  // voxel and the same band discipline, three differences that make it read as shed dust rather than crying:
  //   * it BARELY moves sideways and drifts DOWN slowly (a tear wells up and falls; pollen just sinks),
  //   * it lives a little longer, so a trail is several grains at once rather than one blinking,
  //   * and it is emitted from a moving bee, so the grains are strung out along its flight by themselves —
  //     no spread is needed to make a trail, the motion supplies it.
  const POL_LIFE = 1.1, POL_MS = 2600, POL_GAP = 190;   // …trailing for 2.6 s after leaving a bloom, a grain every 190 ms. 110 was too fast for a 4-slot band shared by every nearby bee: most grains were refused outright and the survivors popped at random, which is what read as artifacts rather than as a trail
  function spawnPollen(wx, wy, wz) {
    const slot = bandSlot(POL_LO, POL_HI); if (slot < 0) return;   // skip rather than cut a live grain short — the splash rule
    const a = Math.random() * 6.283;
    sparks3d[slot] = { x: wx, y: wy, z: wz,
      vx: Math.cos(a) * 0.5, vy: 0.4 + Math.random() * 0.5, vz: Math.sin(a) * 0.5,   // a puff, not a burst: it leaves the abdomen and settles
      born: performance.now(), life: POL_LIFE, ph: Math.random() * 6.283, smoke: false, foam: true };
  }

  // ── FALLING PETALS (user 2026-08-18: "can you make single voxels fall from the cherry trees. single pink
  // voxels falling ... give them a falling leaf animation sway. how a leaf would expect to move when falling
  // from a tree. a smooth left to right cradle motion. dont make them spin") ──
  // Hosted in the particle pool rather than the snow lattice, because a petal has to know which TREE it left:
  // the white variety is a per-tree flag (oakAt's `wht`), and the lattice marches cells, not trees. Spawning off
  // oakAt gives it the tree for free, and the same call already answers "is this one of the blossom trees".
  // THE CRADLE IS NOT A SPIN. A real petal rocks side to side along ONE horizontal axis as it sinks — it does not
  // yaw. So each petal picks a fixed axis (ax, az) at birth and slides along it on a sine; the emit's rotation
  // is pinned to identity for these, which is the whole of "dont make them spin".
  const PETAL_GAP = 65;   // ── DOUBLED 2026-08-20 (user: "double the rate at which petals fall from all the trees … not the speed but the frequency") ── 130 -> 65 ms between attempts. NOTE the band (PETAL_LO..PETAL_HI, 32 slots) is the real ceiling: a petal lives up to PETAL_MAXLIFE, so once the band is full a shorter gap buys nothing and the extra attempts are simply skipped. Halving the gap raises the rate wherever the band has room — under a lone tree, at the edge of a wood — and is a no-op in a dense canopy that was already saturating it. Raising the CEILING means growing sparks3d, which comes straight out of the creature draw budget (see voxelbit-drop-slot-bands), so it is deliberately not done here.                               // ms between attempts — HALVED with the band above (user 2026-08-19); one of the two is useless without the other — with 16 slots and a ~10 s fall this keeps the band saturated, so the air stays evenly dressed rather than pulsing
  const PETAL_FALL = 3.5;                              // vox/s. A leaf does not drop, it descends: 0.35 m/s reads as weightless where the snow's own fall reads as weather
  // ── THE CRADLE HAD TO GET WIDER *AND* QUICKER (user 2026-08-19: "make the leafs have more of a left to right
  // sway motion vs just falling down straight") ── amplitude on its own is not the lever, and the arithmetic
  // says why. What the eye reads is the SHAPE OF THE PATH: how wide one swing is against how far the petal
  // sinks while making it. Half a swing is 2·SWAY across and π·PETAL_FALL/rate down, so at the old 1.2 voxels
  // and a 1.1 rad/s mean rock the path was 2.4 voxels wide over a 10.0 voxel drop — 1 : 4.2, and 24 cm of
  // travel spread over a metre of fall reads as a plumb line, which is exactly "just falling down straight"
  // even though the tangent at the crossing is a nominal 20.6°. Widening SWAY alone only stretches that same
  // thin zig sideways: it takes ~5 voxels to open it up, and a metre-wide arc completed once every 5.7 s reads
  // as WIND CARRYING the petal, not as the petal rocking — the failure mode a slow wide sine always has.
  // So both dials move together and it is their RATIO that is aimed at. SWAY·rate / PETAL_FALL is the tangent
  // of the path's steepest angle off vertical: at 1.75 × 2.0 / 3.5 = 1.0 that is 45°, and one half-swing is
  // 3.5 voxels across against 5.5 down — 1 : 1.6 where it was 1 : 4.2. A petal also completes ~4.5 rocks in
  // the 14 s it can live against the 2.5 it managed before, so the motion is legible on a petal you only
  // watch for a second or two rather than needing the whole fall to declare itself.
  // NOT stepped to 24 fps, and deliberately: this is a rigid voxel travelling through space, not a frame flip.
  // hurtHop below records the same call for the same reason — quantising continuous motion reads as a stutter.
  // ── AND A WIND THAT ACTUALLY CARRIES THEM (user 2026-08-20: "can you make them sway in the wind, they just
  // seem to fall down vertically. make them go more horizontally") ── the cradle below is a SYMMETRIC rock: it
  // swings a petal either side of its own fall line and returns it, so however wide it is set the petal still
  // lands on the spot it left. The note further down even records reading-as-wind as the failure mode it was
  // tuned AWAY from. That was the wrong target: what is wanted is a net horizontal CARRY, so a petal leaves the
  // crown and travels.
  // ONE wind for the whole world, turning slowly, so every petal in view drifts the same way at the same
  // moment — that coherence is what reads as weather rather than as each leaf wandering on its own. It is a
  // pure function of the clock (no state, nothing to desync across a reload) and it is deliberately never
  // still: the slowest term never brings the speed to zero, so there is no dead calm to notice.
  const PETAL_WIND = 2.9;                              // vox/s of carry at full strength — against PETAL_FALL 3.5 that is a ~40 degree slant, so a petal from a 60-voxel crown travels ~50 voxels downwind
  const petalWind = (ms) => {                          // → [vx, vz] this instant
    const a = 2.0 + Math.sin(ms * 0.000041) * 1.25;    // direction wanders ~±72 degrees over ~2.5 min
    const g = PETAL_WIND * (0.66 + 0.34 * Math.sin(ms * 0.000087 + 1.7));   // …and gusts between 2/3 and full over ~1.2 min
    return [Math.cos(a) * g, Math.sin(a) * g];
  };
  const PETAL_SWAY = 1.75;                             // voxels either side of the fall line, before the per-petal ±25% (was 1.2)
  const PETAL_ROCK = 2.0;                              // rad/s, the MEAN rock rate — a 3.1 s swing where it was 5.7 s
  const PETAL_MAXLIFE = 14;                            // a petal from a giant's crown would otherwise hold a slot for 20 s
  function spawnPetal(wx, wy, wz, pit) {               // pit = which leaf voxel this tree sheds (pink / cream / green) — an ITEM ID rather than the old `white` boolean, because there are three varieties now and a second boolean would have made the emit read a 2-bit code spread over two flags
    const slot = bandSlot(PETAL_LO, PETAL_HI); if (slot < 0) return;   // band full → skip, never cut a live petal short (the splash's rule)
    const g = H(Math.round(wx), Math.round(wz));
    const a = Math.random() * 6.283;
    sparks3d[slot] = { x: wx, y: wy, z: wz, vx: 0, vy: 0, vz: 0,
      // Dies as it reaches the ground rather than at a fixed age, so it is not cut off in mid-air — except
      // where PETAL_MAXLIFE bites, and moving the spawn into the crown is most of what stopped it biting.
      // MEASURED over 3,000 draws of each resolver on one blossom stand: the old apex spawn dropped a median
      // 63 voxels, 70.6% of petals hit the 14 s ceiling and the median one blinked out 14 voxels (1.4 m) above
      // the grass. Off the canopy anchors the median drop is 35 voxels, 25.2% reach the ceiling, and the median
      // petal now lands. Mean life falls 13.3 s → 9.8 s with it, which is turnover, not population: the band is
      // the population and it still measures 15.6 petals in the air against the old 15.8.
      born: performance.now(), life: Math.max(1, Math.min(PETAL_MAXLIFE, (wy - g) / PETAL_FALL)),
      ph: Math.random() * 6.283, smoke: false, foam: false,
      petal: true, pit: pit || PETAL_IT, ax: Math.cos(a), az: Math.sin(a),   // the fixed cradle axis
      rate: PETAL_ROCK * (0.8 + Math.random() * 0.4),  // its own rock rate (±20% of the mean), so a drift of petals is never in step
      // ── THE WIND IS SAMPLED ONCE, HERE (user 2026-08-20: "the swaying falling leaves seem to be glitching
      // out") ── the carry is applied downstream as `wind * age`, so reading the LIVE wind there recomputed a
      // petal's entire accumulated drift against a vector that had since turned: every time the wind moved, all
      // the petals jumped sideways at once and the oldest jumped furthest, because their age is the multiplier.
      // A drift has to be integrated, not re-derived. Freezing the vector at birth is that integral for the
      // only interval that matters: the wind wanders ~±72 degrees over ~2.5 minutes and a petal lives at most
      // 14 seconds, so over one lifetime it is very nearly constant anyway — and petals born minutes apart
      // still ride different gusts, which is the variety the live sample was reaching for in the first place.
      wvx: petalWind(performance.now())[0], wvz: petalWind(performance.now())[1],
      sway: PETAL_SWAY * (0.75 + Math.random() * 0.5) };   // …and its own swing WIDTH. The rate spread alone only de-PHASES them: sixteen petals tracing arcs of identical width still read as one emitter running sixteen copies of one animation, which is the look the wider cradle would otherwise have made more obvious rather than less
  }
  // ── WHERE A PETAL LEAVES THE TREE (user 2026-08-19: "the falling leaf voxels seem to fall from above the
  // tree. make sure the leaves fall below the leafs") ── and it did, three quarters of the time. The first
  // version spawned into a CYLINDER inscribed in the model's bounding box: a random angle, a radius of
  // 0.35-1.0 of max(sx, sy)/2, and a height in the top 35% of sz measured down from the box's apex. Every one
  // of those three terms is a box measurement, and a crown is not a box.
  //   * `top` is ground + sz - sink, one course ABOVE the tallest voxel the model owns, and it took its
  //     ground from H(centre) where the stamp seats on groundMin(centre, 4) — the LOW corner of a 4-voxel
  //     footprint — so on any slope it started several voxels high again before the band was even applied.
  //   * the band is 35% of the WHOLE model, trunk included, so on a giant it is the top 40 courses; but a
  //     crown TAPERS, and at 0.95 sz the widest voxel of the biggest oak sits 19 voxels from the axis while
  //     the formula was still drawing radii out to 57.
  //   * so the corner case is every case: full crown radius at the apex is open sky.
  // MEASURED over the six tiers a cherry tree can be, 20,000 draws each of the exact old formula against the
  // models' own voxels: 55 / 87 / 62 / 78 / 74 / 83% of spawns were strictly ABOVE the topmost voxel within
  // three columns of them, and 65-93% were more than two voxels from ANY voxel of the tree. Tier-weighted
  // that is 72% of petals born in open air over the canopy, falling past the crown from above it.
  //
  // THE FIX IS GEOMETRIC, NOT A FUDGE. Subtracting a constant would only move the same wrong cylinder down.
  // The spawn is now an actual voxel of the actual crown: OAK_ANCH[k] (assets/material-tabs.js) is the
  // per-model list of canopy LEAF voxels with four cells of clear EXTERIOR air below them, angle-sorted and
  // capped at 96 — built once at load, already registered, already the thing this tree's fruit hangs from, and
  // exactly the surface a petal should come off: the underside and rim of the crown, never its interior or its
  // top shell. Correct by construction rather than by tuning, and it costs one array index.
  // The anchors are read off OAKV (green), and that is right for a blossom tree too: assets/bow.js's blosRemap
  // returns `(p & 0xffffff) | newid`, so the pink and white sets are the SAME GEOMETRY with the leaf ids
  // repainted. Same reason stampOak can share one anchor table across all three sets.
  function petalPoint(t) {                             // → [wx, wy, wz], the open cell directly beneath one canopy leaf of this tree, or null
    const m = OAKV[t.k], A = OAK_ANCH[t.k];
    if (!m || !A || !A.length) return null;            // ?nooaks, or a tier whose crown offered no anchor — try another tree rather than fall back to a formula that is wrong
    const a = A[(Math.random() * A.length) | 0];       // uniform over the angle-trimmed list, so the drift rings the crown instead of pouring off one side
    const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
    let rx, rz;                                        // …stampOak's rotation, verbatim. A spawn that disagrees with the stamp by one step of rot is a petal beside the crown instead of under it, and it would only be visible on three quarters of the trees
    if (t.rot === 0) { rx = ax; rz = ay; }
    else if (t.rot === 1) { rx = m.sy - 1 - ay; rz = ax; }
    else if (t.rot === 2) { rx = m.sx - 1 - ax; rz = m.sy - 1 - ay; }
    else { rx = ay; rz = m.sx - 1 - ax; }
    const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
    const gy = groundMin(t.wx, t.wz, 4) - t.sink;      // stampOak's OWN seat, not H(): the two differ by the whole fall of the slope under the crown
    // az - 1: the first of the four clear cells the anchor test guaranteed, not the leaf itself. Born inside
    // the leaf a petal is an occluded voxel for the 0.29 s it takes to descend out of it, so it would appear
    // from nowhere a third of a metre down; born one below, it is visible from its first frame with the leaf
    // it just left directly overhead. The 0.1-0.9 jitter keeps it inside that same cell — 96 anchors are
    // plenty of variety between trees, but two petals off one anchor should not share a start point exactly.
    return [t.wx - (fw >> 1) + rx + 0.1 + Math.random() * 0.8, gy + az - 1,
            t.wz - (fd >> 1) + rz + 0.1 + Math.random() * 0.8];
  }
  // ── AND THE PINE SHEDS TOO (user 2026-08-19: "make the pine tree have falling leaves as well") ── the oak's
  // petalPoint, re-derived for the ONE pine model. Three things differ and nothing else:
  //   * the anchors are PINE_ANCH, which already exists and is already registered — it is what the pinecones
  //     hang from, and it is built from exactly the right surface (canopy foliage with an EMPTY cell below), so
  //     a needle leaves the underside of the crown rather than appearing inside it or on top of it.
  //   * the pine is a single model, so there is no per-tier index: PINE_ANCH is one flat list against MSX/MSY.
  //   * the seat is stampTree's own — groundMin(tx, tz, 2) - sink, and the base corner is off MROT[rot], NOT
  //     the model's raw dims. A rotated pine's box is not square, so using MSX/MSY here would put the needles
  //     beside the trunk on two of the four rotations.
  // The rotation cases are stampTree's, verbatim, for the same reason petalPoint copies stampOak's: a spawn that
  // disagrees with the stamp by one step of rot is a needle falling next to the tree instead of out of it.
  function pinePoint(t) {                              // → [wx, wy, wz] under one canopy voxel of this pine, or null
    if (typeof PINE_ANCH === 'undefined' || !PINE_ANCH.length) return null;
    const a = PINE_ANCH[(Math.random() * PINE_ANCH.length) | 0];   // uniform over the angle-sorted list, so the drift rings the crown instead of pouring off one side
    const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
    let rx, rz;
    if (t.rot === 0) { rx = ax; rz = ay; }
    else if (t.rot === 1) { rx = MSY - 1 - ay; rz = ax; }
    else if (t.rot === 2) { rx = MSX - 1 - ax; rz = MSY - 1 - ay; }
    else { rx = ay; rz = MSX - 1 - ax; }
    const R = MROT[t.rot];
    const bx = t.tx - (R.sx >> 1), bz = t.tz - (R.sz >> 1);
    const gy = groundMin(t.tx, t.tz, 2) - t.sink;
    return [bx + rx + 0.1 + Math.random() * 0.8, gy + az - 1,
            bz + rz + 0.1 + Math.random() * 0.8];   // az - 1: the empty cell the anchor test guaranteed, so the needle is visible from its first frame with the crown it left directly overhead
  }
  // ── A FELLED TREE STOPS SHEDDING (user 2026-08-19: "have the leafs stop falling from the tree after the tree
  // has been felled by the player") ── oakAt/treeAt are PROCEDURAL: they answer "is there a tree in this cell"
  // from the world seed and have no idea the player has since cut it down, so a stump kept dropping leaves out
  // of thin air where its crown used to be. Rather than track felled trees in a list — which would have to be
  // seeded, streamed and invalidated alongside the world — this asks the WORLD, which already knows: the anchor
  // is a canopy voxel by construction (that is what OAK_ANCH and PINE_ANCH are built from), so if the cell one
  // above the spawn point is no longer foliage then that crown is not there any more.
  // It is exact for every way a crown can stop existing — felled, chopped out, burned, never generated — and it
  // costs ONE voxel read per spawn attempt, against the six oakAt calls the same attempt already pays for.
  const stillLeafy = (q) => {                          // q = a petalPoint/pinePoint result; true if its crown is still overhead
    const y = Math.round(q[1]) + 1;
    if (y < 1 || y >= WY) return false;
    const v = W[gwrap(Math.floor(q[0]), WX) + y * WX + gwrap(Math.floor(q[2]), WZ) * WX * WY];
    return !!(v && foliaTab[v]);
  };
  let petalNext = 0;
  function petalTick() {
    if (!PETAL_IT || !PETALG_IT) return;               // OAKV.length is no longer part of the gate: the pine sheds too, and petalPoint already refuses per-tree if the oak set is missing
    const now = performance.now();
    if (now < petalNext) return;
    petalNext = now + PETAL_GAP;
    const c0x = Math.floor(P.x / OKCELL), c0z = Math.floor(P.z / OKCELL);
    // ── THE SHED IS SHARED OUT BY CROWN SIZE, NOT PER TREE (user 2026-08-20: "the smaller trees seem to have
    // the leafs fall at a faster rate then the bigger trees … not faster in terms of movement, but just more
    // single voxel leafs are falling. balance this out") ──
    // The old loop drew cells until it found a tree and shed from THAT one, so every tree in the world shed at
    // the same rate whatever its size. Equal counts are not equal DENSITY: oak footprints run 1088 to 12768
    // voxels of ground (8 models, 11.7x), so the same leaves-per-second poured into a twelfth of the area under
    // a sapling is twelve times the visible fall. That is exactly the report — the little trees look like they
    // are shedding harder, and arithmetically they are.
    // WEIGHTED RESERVOIR, not a rejection test. A per-draw "accept with probability size/maxSize" would have
    // corrected the balance by throwing attempts away, and the band is a POPULATION (see PETAL_LO): fewer
    // accepted attempts is fewer leaves in the air everywhere, which is not what "balance this out" asks for.
    // Every draw is still made, exactly one tree is still chosen, and the choice is proportional to crown
    // footprint — so the total rate is unchanged to the frame and only its distribution moves.
    // The weight is FOOTPRINT AREA (sx*sy) rather than voxel count: leaves land on the ground under the crown,
    // so what the eye compares is leaves per square metre of shade, not leaves per leaf.
    let pk = null, pw = 0;                             // reservoir: the tree chosen so far, and the total weight seen
    const offer = (kind, t, w) => { pw += w; if (Math.random() * pw < w) pk = { kind, t }; };   // A-Res, one slot: P(keep) = w / sum-so-far leaves each candidate holding exactly its own share
    for (let att = 0; att < 6; att++) {                // a few draws, not a scan: out of the blossom every draw misses and the whole thing costs six oakAt calls every 130 ms
      const t = oakAt(c0x + ((Math.random() * 5) | 0) - 2, c0z + ((Math.random() * 5) | 0) - 2);
      if (!t) {                                      // ── NO OAK IN THIS CELL: TRY A PINE (user 2026-08-19) ── the same draw, the same attempt budget, the same band. Oaks are asked FIRST only because the oak grid is the one this loop was already walking; in the pine forest every oak draw misses and the pine gets the attempt, and in the oak forest the reverse. Neither can starve the other of the band, because a band slot is taken by whichever tree the cell actually holds
        if (!PETALN_IT) continue;
        const cp = Math.floor(P.x / TCELL), czp = Math.floor(P.z / TCELL);   // the PINE grid is TCELL, not OKCELL — the two forests are on different cell sizes and reusing the oak's would sample the wrong lattice
        const tp = treeAt(cp + ((Math.random() * 5) | 0) - 2, czp + ((Math.random() * 5) | 0) - 2);
        if (tp) offer(1, tp, MSX * MSY);               // the pine is ONE model, so every pine carries one weight — the same footprint measure the oaks are weighed on, so a pine and an oak of equal shade shed equally
        continue;
      }                                // ── EVERY OAK SHEDS NOW, NOT ONLY THE BLOSSOM (user 2026-08-19) ── the `!t.blos` reject that stood here is gone; the flag survives as the thing that picks WHICH leaf falls. Four varieties, and each reads its own off the SAME descriptor the stamp used, so a leaf can never disagree with the crown it came from — the cherry's pink/white split (`wht`) and the oak's dark/light split (`lite`, user 2026-08-19) are the same arrangement, one flag choosing between two remapped ramps
      const om = OAKV[t.k];
      if (om) offer(0, t, om.sx * om.sy);
    }
    if (!pk) return;
    if (pk.kind === 1) { const qp = pinePoint(pk.t); if (!qp || !stillLeafy(qp)) return; spawnPetal(qp[0], qp[1], qp[2], PETALN_IT); return; }
    const q = petalPoint(pk.t); if (!q || !stillLeafy(q)) return;
    spawnPetal(q[0], q[1], q[2], pk.t.blos ? (pk.t.wht ? PETALW_IT : PETAL_IT) : (pk.t.lite && PETALGL_IT ? PETALGL_IT : PETALG_IT));
  }
  // ── A LIVE TAP ── window.__vbPetal rather than a __vb.* method: main/debug-api.js is one shared fragment and
  // this probes one effect, so it is handed out the way world/gen-pool.js hands out __vbOak. Costs nothing
  // until it is called.
  //   __vbPetal.live()     every petal in the air: where it was born, where it is now, and how far its cradle
  //                        has carried it off the fall line (`off`, and `wide` = the widest it will ever go)
  //   __vbPetal.spawn(n)   run the SPAWN RESOLVER n times over the blossom oaks around the player without
  //                        actually spawning anything, and hand back the world points it chose. Pair each one
  //                        with __vb.idAt() up its own column and the "is it inside the canopy" question is
  //                        answered against the world's real voxels rather than against the model.
  window.__vbPetal = {
    // ── WHO ACTUALLY SHEDS ── runs the reservoir's own draw n times over the trees around the player and
    // returns how often each oak MODEL won, beside that model's ground footprint. The two columns should be in
    // the same ratio: that IS the 2026-08-20 balance fix, and a histogram is the only way to see it (a leaf in
    // the air carries its leaf id, not the tree it came off).
    pick(n) {
      const cnt = {}, c0x = Math.floor(P.x / OKCELL), c0z = Math.floor(P.z / OKCELL);
      const cp = Math.floor(P.x / TCELL), czp = Math.floor(P.z / TCELL);
      for (let i = 0; i < (n || 2000); i++) {
        let pk = null, pw = 0;
        const offer = (kind, k, w) => { pw += w; if (Math.random() * pw < w) pk = kind + ':' + k; };
        for (let att = 0; att < 6; att++) {
          const t = oakAt(c0x + ((Math.random() * 5) | 0) - 2, c0z + ((Math.random() * 5) | 0) - 2);
          if (!t) { const tp = treeAt(cp + ((Math.random() * 5) | 0) - 2, czp + ((Math.random() * 5) | 0) - 2);
                    if (tp) offer('pine', 0, MSX * MSY); continue; }
          const om = OAKV[t.k]; if (om) offer('oak', t.wx + ',' + t.wz + ',' + t.k, om.sx * om.sy);
        }
        if (pk) cnt[pk] = (cnt[pk] || 0) + 1;
      }
      // Keyed on the TREE, not the model: a histogram by model only says how many of each kind stand nearby.
      // picks/foot is the number that must come out flat — that is "every crown sheds at the same density".
      const rows = Object.keys(cnt).map((k) => { const p3 = k.split(':')[1].split(',');
        const foot = k.startsWith('pine') ? MSX * MSY : OAKV[+p3[2]].sx * OAKV[+p3[2]].sy;
        return { tree: k.split(':')[0] + ' ' + (p3[2] === undefined ? '' : 'k' + p3[2]), picks: cnt[k], foot, per1k: +(cnt[k] / foot * 1000).toFixed(2) }; });
      rows.sort((a, b) => a.foot - b.foot);
      const pk9 = rows.map((r) => r.per1k);
      return { trees: rows.length, rows, per1kMin: Math.min(...pk9), per1kMax: Math.max(...pk9) };
    },
    live() { const n = performance.now();
      return sparks3d.slice(PETAL_LO, PETAL_HI).map((s) => {
        if (!s || !s.petal) return null;
        const tS = (n - s.born) / 1000; if (tS > s.life) return null;
        const sw = Math.sin(tS * s.rate + s.ph) * s.sway;
        return { t: +tS.toFixed(2), life: +s.life.toFixed(1), it: s.pit | 0, rate: +s.rate.toFixed(2), wide: +s.sway.toFixed(2),
          x0: +s.x.toFixed(2), y0: +s.y.toFixed(2), z0: +s.z.toFixed(2), off: +sw.toFixed(2),
          wvx: +(s.wvx || 0).toFixed(2), wvz: +(s.wvz || 0).toFixed(2),   // the WIND this petal was born into — the tap has to carry it or it reports a position the emit does not draw
          x: +(s.x + s.ax * sw + (s.wvx || 0) * tS).toFixed(2), y: +(s.y - PETAL_FALL * tS).toFixed(2), z: +(s.z + s.az * sw + (s.wvz || 0) * tS).toFixed(2) }; }); },
    spawn(n) { const out = [], c0x = Math.floor(P.x / OKCELL), c0z = Math.floor(P.z / OKCELL);
      for (let i = 0; i < (n || 200); i++) {
        const t = oakAt(c0x + ((Math.random() * 5) | 0) - 2, c0z + ((Math.random() * 5) | 0) - 2);
        if (!t || !t.blos) continue;
        const q = petalPoint(t); if (!q) continue;
        out.push({ x: +q[0].toFixed(2), y: q[1], z: +q[2].toFixed(2), k: t.k, rot: t.rot, wht: !!t.wht,
          tx: t.wx, tz: t.wz, sz: OAKV[t.k].sz, gy: groundMin(t.wx, t.wz, 4) - t.sink, anch: OAK_ANCH[t.k].length });
      }
      return out; } };
  const CRY_WAIT = 900, CRY_MS = 3000, CRY_GAP = 260;   // …starts after the mother's death poof has cleared, runs 3 s (user), one tear every 260 ms — "one after the other"
  function startCrying(momSlot) {                       // her whole brood, at the moment she is confirmed dead
    const b0 = BABY_0 + (momSlot - DUCK_0) * 3;
    for (let j = b0; j < b0 + 3; j++) { const B = wbf[j]; if (!B || !B.init) continue;
      B.cryFrom = performance.now() + CRY_WAIT; B.cryTo = B.cryFrom + CRY_MS; B.cryNext = B.cryFrom; B.cryEye = 0; }
  }
  function spawnDeathBurst(wx, wy, wz) {                // DEATH POOF: 4 bright sparks + 16 INDIVIDUAL smoke voxels fire TOGETHER (user), slots 5-24. Each smoke voxel floats up + twirls independently, like a snowflake.
    const cy = wy + 2;                                  // aim at the body centre
    for (let i = 0; i < 20; i++) {
      const a = Math.random() * 6.283;
      if (i < 4) { const sp = 14 + Math.random() * 22;  // SPARK — fast ballistic ember bursting outward, short life
        sparks3d[i] = { x: wx, y: cy, z: wz, vx: Math.cos(a) * sp, vy: 12 + Math.random() * 22, vz: Math.sin(a) * sp,
          born: performance.now(), life: 0.4 + Math.random() * 0.25, ph: Math.random() * 6.283, smoke: false };
      } else { const r = 0.5 + Math.random() * 1.8;     // SMOKE — 16 INDIVIDUAL 10 cm voxels rising as one loose column: tight spawn radius, gentle outward drift + strong lift, staggered heights so they form a continuous stream, each with its OWN snowflake spin/drift
        // ── THE PLUME YIELDS TO A LIVE SPLASH (user 2026-08-05: "I don't see the splash droplets 100% of the
        // time") ── this used to stamp slots 4-19 outright, deleting any droplet or tear in flight. MEASURED:
        // 24 droplets cut short over 25 s with a death every 3 s. Smoke now takes free slots and other smoke
        // only; if none is left it places one voxel fewer. A 15-voxel column against a 16-voxel one cannot be
        // told apart — a splash that never appears can.
        const ds = bandSlot(4, 20); if (ds < 0) continue;
        sparks3d[ds] = { x: wx + Math.cos(a) * r, y: cy + (i - 4) * 0.55, z: wz + Math.sin(a) * r,   // (i-4)*0.55 → the 16 voxels start stacked up a ~8-vox column (denser than the old 8-voxel stream)
          vx: Math.cos(a) * (1 + Math.random() * 2), vy: 12 + Math.random() * 6, vz: Math.sin(a) * (1 + Math.random() * 2),
          born: performance.now(), life: 1.0 + Math.random() * 0.5, ph: Math.random() * 6.283, spin: 2 + Math.random() * 2.5, smoke: true };   // spin = snowflake twirl rate (2 + rand·2.5 rad/s), random phase
      }
    }
  }
  const aimedCreature = () => {                         // the nearest reachable, hittable life form under the crosshair right now → its wbf slot (or -1). Shared by the KILL and the crosshair-□ mirror so they agree exactly.
    if (ED.on || dead) return -1;
    const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch);
    const vx = Math.sin(P.yaw) * cp, vy = sp, vz = Math.cos(P.yaw) * cp;   // view direction (yaw/pitch → x=sin·cos, y=sin, z=cos·cos)
    const HREACH = REACH_H, MAX3 = REACH_3D;   // TRIPLED reach (user 2026-07-22): ≤4.8 m HORIZONTAL reach (the eye sits ~1.85 m up, so measuring 3D-from-eye would put a ground creature permanently out of range) + a 3D cap + a ~35° crosshair cone. Shared with the axe — see REACH_H.
    let best = -1, bestD = 1e9;
    for (let wk = 0; wk < DES_END; wk++) { const B = wbf[wk];   // the WHOLE pool = every SPAWNED life form (flyers/ducks/worms/perched birds/fish/mammals/desert). Flying songbirds live in birds[] and are excluded (out of melee reach anyway).
      if (!B || !B.init) continue;
      const by = (B.kind | 0) === 5 ? (B.perchFeet || 0) + 3 : B.y + 2;   // a PERCHED BIRD's height lives in perchFeet (B.y is stale for kind 5) — +3 ≈ body centre. This is what makes the songbirds in the crowns KILLABLE (user).
      const dx = B.x - P.x, dy = by - smoothEye, dz = B.z - P.z;
      const dh = Math.hypot(dx, dz), d3 = Math.hypot(dx, dy, dz);
      if (dh > HREACH || d3 > MAX3 || d3 < 0.5) continue;
      // RAY vs the creature, not a cone. proj = how far along the view it sits; perp = how far the
      // view ray passes from it. A miss by more than its own radius is a miss.
      const proj = dx * vx + dy * vy + dz * vz;
      if (proj <= 0) continue;                                     // behind the camera
      // MEAN half-extent, not the half-diagonal: a sphere drawn around the corners of the box is far
      // wider than the animal actually looks, and that generosity is the complaint being fixed.
      const q = B.sB && B.sN
        ? ((B.sB[3] - B.sB[0]) + (B.sB[4] - B.sB[1]) + (B.sB[5] - B.sB[2])) / 6
        : (AIM_R[B.kind | 0] || 3.0);
      const rr = q + AIM_FORGIVE;
      if (d3 * d3 - proj * proj > rr * rr) continue;               // the ray passes wide of it
      if (d3 < bestD) { bestD = d3; best = wk; }
    }
    return best;
  };
  const HOP_H = 2.6;                                   // voxels a creature is knocked UP when hit (user) — an arc over the flash window, so the blink and the bounce read as one impact
  const hurtHop = (B) => {                             // …the offset itself: 0 unless this creature is mid-flash
    if (!B || B.hopT0 === undefined) return 0;
    const e = (performance.now() - B.hopT0) / HURT_MS;
    if (e < 0 || e >= 1) return 0;
    return Math.sin(e * Math.PI) * HOP_H;              // up and back down across the window — CONTINUOUS, not stepped: at 500 ms a 24 fps quantisation reads as a stutter rather than a hop (user)
  };
  const hitSpot = (B) => {                             // where a blow LANDS on this creature — its stamped body centre when it has one, so the sparks fly off the animal
    const q = B && B.sB && B.sN ? B.sB : null;         // rather than off the point the AI happens to track. Grid-stamped life (mammals, perched birds) always has bounds;
    if (q) return [(q[0] + q[3] + 1) * 0.5, (q[1] + q[4] + 1) * 0.5, (q[2] + q[5] + 1) * 0.5];   // +1 because sB holds voxel INDICES and voxel v fills [v, v+1) — same convention as hurtBox
    return [B.x, ((B.kind | 0) === 5 ? (B.perchFeet || 0) : B.y) + 2, B.z];   // off-grid life: its own position, lifted to the body middle (a perched bird's height lives in perchFeet)
  };
  // ── HOW FAST A THROWN VOXEL FORGETS THE BODY IT CAME OFF ── seconds. The exponential time constant the
  // player-momentum carry decays on (see the ivx/ivz term in main/tick-emit.js). 0.1 s is "almost at once" at
  // 24 fps — the carry is 86% spent within five frames — which is the sharp drop-off the user asked for, while
  // still being long enough that a fleck visibly leaves a MOVING body rather than appearing behind it.
  const SPK_CARRY_TAU = 0.10;
  const HITS_TO_KILL = 3;                              // hits a non-AXE tool needs to kill a life form (user). The axe is the killing tool and ignores this entirely.
  const KNIFE_HITS_TO_KILL = 2;                        // …and the STONE KNIFE takes two (user 2026-08-19, was three) — it is a cutting edge, so it sits with the arrow between the axe's one blow and every blunt hand tool's three
  const ARROW_HITS_TO_KILL = 1;                        // ── A BOW KILLS OUTRIGHT (user 2026-08-19: "make life one shot with the bow/arrow") ── was two. The bow is now the axe's peer: the one weapon that ends it in a single hit, and the only one that does so at range
  const FLAM_ARROW_HITS = 2;                           // …except the FLAMINGO, which takes two (user). Its own constant rather than a branch: the exception is a property of the bird, and the next one to want it is one entry in the same place
