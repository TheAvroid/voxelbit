  // @module — arrows and spears: launch, arc, impact, and the pick-up flood
  // @exports ARROW_ROLL, ARROW_UP, ARROW_V, FLOWER_CAP, FRUIT_CAP, FRUIT_IDS, FRUIT_MIN, PASSTHRU, PICK_APPLE, PICK_BOULDER, PICK_CONE, PICK_FLOWER, PICK_ORANGE, PICK_ROCK, PICK_STICK, PICK_TWIG, SPEAR_WIND_MS, TWIG_CAP, TWIG_MAX, WORM_PASS, arrowChop, floodRemove, floodScan, fruitAt, launchThrown, shootArrow, throwSpear, twigLeafCells
  // ── LOOSE THE ARROW ── the same arc integration the thrown rock uses, at the hurl profile. The BOW is
  // not consumed: an arrow is ammunition, and it lands as an ordinary drop that can be picked up again.
  const ARROW_V = HURL_V * 2, ARROW_UP = HURL_UP * 2;  // TWICE the hurl profile (user) — a bow beats an arm, and the flatter arc is the point of it
  // ── AN ARROW BITES LIKE A TOOL ── it strikes rock or soil and a chunk comes away (user), just smaller
  // than a swing's: an arrowhead is not an axe. And you do NOT get it from across the clearing — the
  // chunk lies where it fell until you walk up to it, at a fraction of the usual reach.
  // ARROW_ABSORB_R was 30 — three metres, and WIDER than the 16 an ordinary chunk uses, which is backwards:
  // a piece knocked off from across the clearing should be no easier to collect than one you cut standing
  // over it. Now it is exactly PH.absorbR, the one reach the game already uses for a settled chunk and for
  // AUTO_PICK_R, so "walk up to it" means the same distance whatever knocked it loose (user 2026-08-05, and
  // the reason this needed several passes is that the radius was never the leak — see the tagging note below).
  // ARROW_CHOP_MIN: the smallest bite worth taking, and deliberately well UNDER the bite itself. Setting the
  // two equal (as the axe does, where C_MIN === C_BITE across a radius-10 sphere) made the arrow useless
  // exactly where the screenshots show it being used — a treetop, where the trunk is a couple of voxels thick
  // and a radius-3 sphere simply cannot offer ten wood voxels. MEASURED: every shot from 72% of the way up a
  // pine returned hit:false and did nothing at all. Four still rules out the single-voxel success this was
  // added to prevent, and `take` is min(bite, whatever is there), so a thin top yields a smaller chunk rather
  // than nothing.
  const ARROW_ROLL = 9.0;                              // rad/s the arrow rolls about its own shaft in flight — ~1.4 turns a second: fast enough to read as a spin, slow enough not to smear into a blur at 24 fps.
  const ARROW_CHOP_RAD = 3, ARROW_CHOP_BITE = 10, ARROW_CHOP_MIN = 4, ARROW_ABSORB_R = 16;
  // ── AN ARROW TAKES A CHUNK, IT DOES NOT DEMOLISH THE TREE (user 2026-08-05) ── one shaft used to strip a
  // pine. MEASURED at a seeded tree, per shot: at the TRUNK 48 wood voxels gone for a bite of 10, at a BRANCH
  // 28 for a bite of 10, and into pure NEEDLES 35 foliage voxels for a bite the arrow had no business taking
  // at all. Two independent faults, and the ROUTING below is the larger of them — see the note at the call.
  // This half is the smaller one: the tree path was handed minBite 1 and no material filter, so a shot into
  // the canopy carved needles (foliage is deliberately carvable in physChopAt) and could succeed on a single
  // voxel. The arrow now swings under the axe's own discipline, scaled down: WOOD ONLY, so a shaft in the
  // needles sticks and nothing else happens, exactly as a swing aimed at a leaf does; and a minimum bite EQUAL
  // to the bite, the same minBite === bite rule chopSwing uses, so a place that cannot give a full 10-voxel
  // piece is not chopped at all and the sphere can never spend itself on one load-bearing cell. Felling stays
  // the axe's job — 10 voxels a shot against a trunk of hundreds is a notch — and the flood keeps its honest
  // say about anything that genuinely does come free.
  const arrowWood = (v) => !!woodTab[v];             // bark and branch, never needles — chopSwing's isWood, by another name
  const arrowChop = (ix, iy, iz) => {
    if (!PH.on || ED.on || iy < 1 || iy >= WY) return false;
    const id = W[gwrap(ix, WX) + iy * WX + gwrap(iz, WZ) * WX * WY];
    if (!id) return false;
    for (let wk = 0; wk < DES_END; wk++) { const B = wbf[wk];   // …and NOTHING alive gets carved (user): a stamped animal's voxels are not terrain
      if (!B || !B.init || !B.sB || !B.sN) continue;
      const q = B.sB;
      if (ix >= q[0] - 1 && ix <= q[3] + 1 && iy >= q[1] - 1 && iy <= q[4] + 1 && iz >= q[2] - 1 && iz <= q[5] + 1) return false;
    }
    // …confined to the material it struck, exactly as a swing is: the sphere cannot spill into the next one
    const okMat = digOnlyTab[id] ? ((v) => !!digOnlyTab[v]) : (pickOnlyTab[id] ? ((v) => !!pickOnlyTab[v]) : ((v) => !digOnlyTab[v] && !pickOnlyTab[v] && !axeOnlyTab[v]));   // …WOOD included, the same third owned material chopSwing's okMat was missing (user 2026-08-28) — the caller hands `arrowWood` when the shaft actually struck a trunk, so this arm only ever sees SOFT decor, and a shot into the grass at a bole should not chip the bole
    const tArr = performance.now();                   // …not PH.bodies.length: see the tagging loop at the end
    // ── A STANDING TREE IS NOT DECOR ── and this ordering is the whole of the bug. phChopDecor ran FIRST and
    // trunk ids are in decorTab, so every shot into a pine was handled as though it were a mushroom: it took
    // its bite and then ran phDecorOrphans — a sweep written for a cap left hanging over a cut stem — across
    // tree geometry, where a limb reads as unsupported the moment the cut touches its join. chopSwing has
    // always guarded against exactly this (`decorTab[id] && !woodTab[id]` — "a standing trunk belongs to the
    // tree path below"); the arrow simply never inherited the guard. Standing wood now goes to the TREE path,
    // which knows what holds a pine up, and phChopDecor is left the mushrooms, ferns and fallen logs it was
    // written for.
    const S = treeShapeAt(ix, iz);
    const standing = !!(S && woodTab[id]);            // this voxel belongs to a pine that is still up
    let hit = standing ? physChopAt(ix, iy, iz, ARROW_CHOP_RAD, S, ARROW_CHOP_MIN, ARROW_CHOP_BITE, arrowWood).hit : false;
    // ── THE FILTER FOLLOWS THE MATERIAL, NOT `standing` (2026-08-17) ── `standing` is false for anything
    // treeShapeAt does not recognise, and treeShapeAt is pine-only by construction (treeAt/MROT/TCELL).
    // So every OAK hit fell through to okMat, which is the permissive soil-and-stone filter, and the
    // bite took the leaves around the shaft — reintroducing the exact 'aiming at the trunk gets the
    // needles' complaint the paragraph above was written to fix. Wood is wood whether or not this code
    // knows which tree it belongs to.
    // ── AND A BEEHIVE IS THE ONE THING HERE THAT ANSWERS BACK (2026-08-19) ── the SILENT PATH: this reaches
    // phChopDecor by its own route and had no hook at all, so an arrow could take a hive apart and nothing
    // ever reached the ledger. It was not really covered by the 1 Hz watch in main/tick-creatures.js either —
    // that watch is scoped to the SWARM slots and to the hive nearest the PLAYER, so a forager-only population
    // or a shot at any hive that is not the closest one is still silent, and even in the covered case the
    // answer arrives up to a second late. The hook is one line and it is tools.js's, verbatim: asked AFTER the
    // carve, because the question is "is the hive open now", and hiveChopped does nothing until BEE_BREAK_F.
    // The FILTER is tools.js's too, for the same reason: within ARROW_CHOP_RAD of a hive voxel is the oak
    // branch it hangs from, and bark is decorTab, so the permissive arm would spend the shaft's bite on wood.
    if (!hit && !standing) hit = phChopDecor(ix, iy, iz, ARROW_CHOP_RAD, ARROW_CHOP_BITE, woodTab[id] ? arrowWood : (HIVE_TAB[id] ? ((v) => !!HIVE_TAB[v]) : okMat));   // mushrooms, ferns, ground logs — and soil and stone, which is what okMat is for
    if (!hit) return false;
    if (HIVE_TAB[id]) hiveChopped(ix, iy, iz);         // …and the swing that opens it is the one that lets the bees out — the same ledger the axe posts to
    // ── RESOLVE BEFORE TAGGING ── whatever that bite was holding up comes down through supFlush, off the cells the
    // carve actually changed. It is run to completion HERE, synchronously, rather than left to the frame
    // loop, because the tagging loop below has to see the bodies it makes — that ordering is load-bearing
    // and was measured (see the note under it). Bounded, so a storm-sized queue can never stall the frame.
    supFlush(true, 12);
    // ── TAG EVERYTHING THIS SHOT CREATED, AND TAG IT LAST (user 2026-08-05: "the player can still absorb
    // things at a very far distance when shooting with a bow") ── this loop used to run BEFORE the sweep,
    // so it only ever caught the bodies the carve itself made. Every clump the sweep then cut free kept the
    // ordinary absorbAt timer, and a timed body skips the distance gate entirely (see the absorb step: the
    // range check lives inside the `no timer yet` branch, and a body whose timer has elapsed falls straight
    // through to absorbing). MEASURED standing 120 voxels from the tree and never moving: one shot left 8
    // untagged bodies and 6 chunks flew across the clearing into the player.
    // …and it walks the list by BIRTH TIME, not by an index range captured beforehand. That was the second
    // half of the same bug and the reason it survived earlier passes: phMakeRoom and phReclaim splice the
    // OLDEST body out of the MIDDLE of PH.bodies to stay under the cap, which shifts every later index down,
    // so a saved `n0` stops pointing at the first new body the moment a carve runs at capacity — exactly when
    // a shot into a tree makes the most debris. Every body records `born`, and that survives any amount of
    // splicing. Between the two fixes there is no longer any path from a bowshot to a body that collects itself.
    for (let i = 0; i < PH.bodies.length; i++) { const b = PH.bodies[i];
      if (b.born < tArr) continue;                     // it was already lying there before this shot — not ours to re-tag
      b.absorbAt = 0;                                  // no timed flight into your chest: it has to be COLLECTED
      b.nearR = ARROW_ABSORB_R;                        // …and only from close up (user)
    }
    return true;
  };
  const ARROWS_ON_FIELD = 3;                         // how many loosed arrows may lie about at once — one drop slot is always kept free for the next shot
  // LEAVES CATCH ARROWS (user). solid() is the WALKING test and pine needles are deliberately walk-through,
  // so a shaft flew clean through a canopy. A flying shaft is not a walking player: foliage stops it.
  const arrowBlocked = (x, y, z) => {
    if (y < 1) return true;
    if (y >= WY) return false;
    const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    return !!(id && (solidTab[id] === 1 || foliaTab[id])) || phBodySolid(x, y, z);
  };
  // ONE launcher for everything thrown point-first: the arrow off the string and the SPEAR out of the hand
  // (user). They differ only in speed, in what they cost you, and in how hard they land.
  const launchThrown = (it, V, UPK, kind) => {
    if (!it || dead || ED.on) return false;
    const pr = prevCam;
    // ── IT LEAVES THE BOW ── the viewmodel sits about a unit from the eye, far too close to spawn a
    // full-size projectile, so take the bow's OWN sideways/vertical offset and carry it out along the
    // view to where the arrow can be drawn. Same line of sight as the bow, so it reads as leaving it.
    const LAUNCH_D = 6;
    const ho = heldOff || [0, 0, 1];
    const k = LAUNCH_D / Math.max(0.2, ho[2]);
    const sx = pr.pos[0] + pr.right[0] * ho[0] * k + pr.up[0] * ho[1] * k + pr.fwd[0] * LAUNCH_D,
          sy2 = pr.pos[1] + pr.right[1] * ho[0] * k + pr.up[1] * ho[1] * k + pr.fwd[1] * LAUNCH_D,
          sz = pr.pos[2] + pr.right[2] * ho[0] * k + pr.up[2] * ho[1] * k + pr.fwd[2] * LAUNCH_D;
    // …and it must still go WHERE YOU AIMED. Leaving the bow means leaving from a point ~6 voxels to the
    // side of the eye, so firing straight down the view direction sends the arrow along a parallel line
    // that never crosses the crosshair. Aim at a distant point ON the view ray instead: the flight
    // converges onto the sight line within a few voxels, the way a real bow sight does.
    const AIM_FAR = 300;
    const tx3 = pr.pos[0] + pr.fwd[0] * AIM_FAR - sx, ty3 = pr.pos[1] + pr.fwd[1] * AIM_FAR - sy2, tz3 = pr.pos[2] + pr.fwd[2] * AIM_FAR - sz;
    const tl3 = Math.hypot(tx3, ty3, tz3) || 1;
    const vx = (tx3 / tl3) * V, vy0 = (ty3 / tl3) * V + UPK, vz = (tz3 / tl3) * V;
    // ── WHERE IT STICKS ── march the arc against the WORLD, not the height map: an arrow buries itself in
    // whatever it meets first, be that ground, a trunk or a branch (user). EIGHT samples per 5 ms step —
    // at 480 vox/s a whole step spans two voxels and would otherwise tunnel clean through a tree.
    const SUB = 8;
    // ── AND LIVING THINGS ── an arrow that passes through an animal wounds it exactly as a swing does
    // (user). Candidates are filtered by distance ONCE, then tested along the arc. Their positions are
    // read at release, like the rest of the flight: the whole arc is integrated up front.
    const targets = [];
    for (let wk = 0; wk < DES_END; wk++) { const B = wbf[wk];
      if (!B || !B.init || B.dying) continue;
      if (Math.abs(B.x - P.x) > 700 || Math.abs(B.z - P.z) > 700) continue;
      // ── AN ORIENTED BOX THE SIZE OF THE ANIMAL (user 2026-08-07: "improve the hitbox of the arrow against
      // life") ── this was a SPHERE whose radius was the MEAN of the three half-extents, which is the one shape
      // that fits nothing: on a skunk (11 long, 3 wide) the mean is ~4, so the sphere bulged a voxel past each
      // flank — arrows that visibly missed to the side connected — while falling two voxels short of the nose
      // and tail, where arrows that visibly hit passed through. Grid-stamped creatures publish a true world AABB
      // in sB; traced ones get their model box turned by their own heading. Same small pad either way.
      const AR_PAD = 1.2;                                // forgiveness, so a moving bird is still catchable
      let bx9, by9, bz9, hx9, hy9, hz9, th9 = 0;
      if (B.sB && B.sN) {                                // stamped: sB holds voxel INDICES, so the span is [q0, q3+1]
        bx9 = (B.sB[0] + B.sB[3] + 1) * 0.5; by9 = (B.sB[1] + B.sB[4] + 1) * 0.5; bz9 = (B.sB[2] + B.sB[5] + 1) * 0.5;
        hx9 = (B.sB[3] - B.sB[0] + 1) * 0.5; hy9 = (B.sB[4] - B.sB[1] + 1) * 0.5; hz9 = (B.sB[5] - B.sB[2] + 1) * 0.5;
      } else {
        const it9 = B.ragIt && itemsRef ? itemsRef[(B.ragIt | 0) - 1] : null;
        if (it9) { hx9 = it9.w * 0.5; hz9 = it9.d * 0.5; hy9 = it9.h * 0.5; th9 = B.th || 0; }
        else { const q = AIM_R[B.kind | 0] || 3.0; hx9 = hy9 = hz9 = q; }
        bx9 = B.x; bz9 = B.z; by9 = (B.kind | 0) === 5 ? (B.perchFeet || 0) + 3 : B.y;
      }
      targets.push({ wk, B, bx: bx9, by: by9, bz: bz9,
                     hx: hx9 + AR_PAD, hy: hy9 + AR_PAD, hz: hz9 + AR_PAD, th: th9 });
    }
    // …and the FLYING songbirds too (user). They live in birds[], not the creature pool, so they need their
    // own entry in the scan and their own death — but they are shot exactly the same way.
    const bTargets = [];
    for (let bi = 0; bi < birds.length; bi++) { const B = birds[bi];
      if (!B || !B.init) continue;
      if (Math.abs(B.x - P.x) > 700 || Math.abs(B.z - P.z) > 700) continue;
      // Forgiveness GROWS WITH RANGE. A flat 6 voxels is a fair target at arm's length and about three pixels of
      // aim tolerance on a bird a hundred voxels up, which is where they actually live (BIRD_ALT is 140). Close
      // shots stay as precise as they were; distance is what gets the help, so it never reads as a magnet.
      const bD = Math.hypot(B.x - P.x, B.y - P.y, B.z - P.z), bR = 6 + bD * 0.035;
      bTargets.push({ bi, B, r2: bR * bR });
    }
    let hitSlot = -1, hitBird = -1;
    // ── THE ARC IS MARCHED TO ITS END, NOT TO A BUDGET (user 2026-08-08: an arrow loosed straight up stopped
    // dead in mid-air on the way back down — an "invisible ceiling") ── this loop ran a flat 900 steps, i.e.
    // 4.5 s of flight, and whatever the arrow was doing when the count ran out got written out as the
    // IMPACT: T, ex/ey/ez and stick:true all came from a point in open sky, so the shaft hung there for
    // good. A full draw is 498 vox/s straight up — 2.9 s to the apex, 5.9 s back down to the eye — so every
    // steep shot outlived the budget, while a 3/4 draw (4.4 s) landed fine; the height it froze at moved
    // with the draw, which is exactly what made it read as a ceiling. The march now covers the whole
    // ballistic flight, and the part of it spent ABOVE the world is nearly free: nothing up there can be
    // struck, so the eight-sample subdivision is skipped until the arrow falls back within reach.
    const STEPS = 4000;                                // 20 s — a runaway guard rather than a real limit: far past the ~6 s a full-draw vertical shot needs
    let yCeil = WY;                                    // above this there is nothing to hit at all: the grid ends, and every animal lives inside it
    for (const t of targets) yCeil = Math.max(yCeil, t.by + t.hy + 1);
    for (const t of bTargets) yCeil = Math.max(yCeil, t.B.y + Math.sqrt(t.r2) + 1);
    // ── AND IT COLLIDES WITH THE WORLD FROM THE FIRST STEP (2026-08-10) ── the impact test below was gated on
    // `T > 0.02`, and T is the flight time at the START of a 5 ms step, so the first FIVE steps were never
    // tested at all. At a full draw that is 12 voxels, on top of the 6 the shaft already spawns ahead of the
    // eye: nothing within ~18 voxels could stop an arrow, so a boulder at 1.5 m was passed clean through and
    // the carve landed on whatever the shaft eventually met across the clearing. Creature hits were never
    // gated this way, which is why only the world half of it was wrong.
    // What that gate was really protecting is the MUZZLE, and that part is real: the launch point sits
    // LAUNCH_D = 6 voxels out along the view (see above), so it can start life inside a trunk, a wall or the
    // ground, and an ungated test would then stick the arrow instantly at arm's length. Ask that question
    // directly instead of buying it with a blanket head start — is the SPAWN POINT itself blocked, and has the
    // arc reached open air since — and the first two metres come back with the guard intact.
    let muzzleFree = !arrowBlocked(Math.round(sx), Math.round(sy2), Math.round(sz));   // false = launched from inside something: no impact until the arc is out in the open
    let px = sx, py = sy2, pz = sz, vy = vy0, T = 0, landed = false;
    for (let i = 0; i < STEPS; i++) {
      const nvy = vy + TOSS_G * 0.005;
      const dx = vx * 0.005, dy = (vy + nvy) * 0.5 * 0.005, dz = vz * 0.005;
      if (py >= yCeil && py + dy >= yCeil) { px += dx; py += dy; pz += dz; vy = nvy; T += 0.005; continue; }   // the whole step is out over the top of the world — skip the hit tests, they cannot connect
      let struck = false;
      for (let s = 1; s <= SUB; s++) {
        const f = s / SUB, qx = px + dx * f, qy = py + dy * f, qz = pz + dz * f;
        for (const t of targets) {
          const ax = t.bx - qx, ay = t.by - qy, az = t.bz - qz;
          if (ay < -t.hy || ay > t.hy) continue;
          let lx9 = ax, lz9 = az;
          if (t.th) { const c9 = Math.cos(t.th), s9 = Math.sin(t.th); lx9 = ax * c9 - az * s9; lz9 = ax * s9 + az * c9; }   // into the animal's own frame — a long body must not be tested against a square
          if (lx9 >= -t.hx && lx9 <= t.hx && lz9 >= -t.hz && lz9 <= t.hz) { hitSlot = t.wk; break; }
        }
        if (hitSlot < 0) for (const t of bTargets) { const B = t.B;
          // Deliberately NOT led. Leading is the physically honest thing, but the test is what decides whether a
          // shot connects, and against a 55 vox/s songbird it turns "aim at the bird" into "aim where it will be",
          // which is strictly harder — the opposite of what was asked for. Measured 5-7 hits in 13-14 straight-aimed
          // shots at 60-115 voxels either way, so the honesty bought nothing and cost aim.
          const ax = B.x - qx, ay = B.y - qy, az = B.z - qz;
          if (ax * ax + ay * ay + az * az <= t.r2) { hitBird = t.bi; break; }
        }
        const blk = arrowBlocked(Math.round(qx), Math.round(qy), Math.round(qz));
        if (!blk) muzzleFree = true;                   // out in the open — everything solid from here on is a real impact, however close it is
        if (hitSlot >= 0 || hitBird >= 0 || (muzzleFree && blk)) {
          px = qx; py = qy; pz = qz; T += 0.005 * f; struck = true; break;
        }
      }
      if (struck) { landed = true; break; }
      px += dx; py += dy; pz += dz; vy = nvy; T += 0.005;
    }
    // …and if even THAT ran out (it cannot on any real shot), set it down on the ground under wherever it
    // got to rather than leaving it hanging: a stuck arrow with nothing under it is the bug just fixed.
    if (!landed) { const ix9 = Math.round(px), iz9 = Math.round(pz);
      for (let y9 = Math.min(WY - 1, Math.round(py)); y9 >= 0; y9--) if (arrowBlocked(ix9, y9, iz9)) { py = y9 + 1; break; }
    }
    // ── NEVER CARVE AN ANIMAL ── a mammal is STAMPED into the world grid, so its voxels read as ordinary
    // terrain: a shaft that came down on a porcupine's quills (outside the body sphere tested above, but
    // still its voxels) knocked a CHUNK out of it. Anything landing inside a live creature's stamp is a
    // hit on that creature, full stop (user).
    if (hitSlot < 0) {
      const ix = Math.round(px), iy = Math.round(py), iz = Math.round(pz);
      for (let wk = 0; wk < DES_END; wk++) { const B = wbf[wk];
        if (!B || !B.init || B.dying || !B.sB || !B.sN) continue;
        const q = B.sB;
        if (ix >= q[0] - 1 && ix <= q[3] + 1 && iy >= q[1] - 1 && iy <= q[4] + 1 && iz >= q[2] - 1 && iz <= q[5] + 1) { hitSlot = wk; break; }
      }
    }
    const lx = Math.round(px), lz = Math.round(pz);
    const Xh = [Math.sin(P.yaw), 0, Math.cos(P.yaw)];  // it lands pointing the way it flew
    // ── ROOM FOR THE NEXT SHOT ── the composite renders only the FIRST FOUR drops, so arrows piling up in
    // the list silently swallowed every shot after the third: the new one existed but was never drawn, and
    // the bow looked broken. Retire the OLDEST arrow on the field instead (user).
    for (let n = drops.reduce((k, d) => k + (d.it === it ? 1 : 0), 0); n >= ARROWS_ON_FIELD; n--) {
      const i = drops.findIndex((d) => d.it === it);
      if (i < 0) break;
      drops.splice(i, 1);
    }
    drops.push({ x: lx, y: Math.round(py), z: lz, it, ph: Math.random() * 6.28, born: performance.now(),
      T, sx, sy: sy2, sz, vx, vy: vy0, vz, aim: true, stick: true, hitSlot, kind,   // aim: it flies POINT-FIRST, nose following the arc; stick: and STAYS where it struck; hitSlot: what it is about to wound (user)
      hitBird,                                         // …or the songbird it is about to bring down
      chopAt: hitSlot < 0 && hitBird < 0 && py >= 1 ? [Math.round(px), Math.round(py), Math.round(pz)] : null,   // …or the voxel it is about to knock a chunk out of
      ex: px, ey: py, ez: pz,                          // the real impact point, not the analytic parabola's guess
      q0: m2q(Xh, [Xh[2], 0, -Xh[0]], [0, 1, 0]) });
    if (drops.length > 8) drops.shift();   // ── 8 ITEM DROPS, NOT 4 (user 2026-08-20: "have the max number of floating hand held items on the field 8 instead of 4") ── drops.shift() DELETES the oldest, so a fifth drop did not just stop being drawn, it stopped existing. The render band moved with it: see the band map at dropCursor in main/tick-life.js
    unlockProjectile();                                // it is away and flying — the discovery is earned (user)
    return true;
  };
  // ── THE BOW ── the draw IS the power (user). Loosing is only allowed past half draw, so this runs 0.5 at
  // the earliest release to 1.0 at a full pull; ARROW_V is what a full draw is worth.
  // ── AND A SHOT COSTS AN ARROW (user 2026-08-20: "when the player shoots an arrow with bow, have the number
  // of arrows go down") ── the bow was never consumed and neither was its ammunition, so the eight the player
  // spawns with were infinite. The arrows are NOT in the hand — the hand holds the bow — so this spends one
  // from wherever the hotbar keeps them, the way craftTake spends materials across slots.
  // PAID BEFORE IT FLIES, which is the rule the spear below records the hard way: check the slot first, and a
  // launch that cannot be paid for never happens. Here that also means an empty quiver simply releases the
  // string with nothing on it, rather than firing a shaft the player does not own.
  const arrowTake = () => {
    if (!ARROW_IT) return false;
    for (let i = 0; i < slots.length; i++) { const s9 = slots[i];
      if (s9 && s9.it === ARROW_IT && (s9.n | 0) > 0) { s9.n -= 1; if (s9.n <= 0) slots[i] = null; slotTidy(); return true; } }
    return false;
  };
  // ── THE BOW IS UNLIMITED (user 2026-08-20: "just have the bow shoot unlimited arrows. keep the shooting down
  // the arrow mechanic though") ── one switch, and everything above it stays: arrowTake, the pay-before-it-flies
  // ordering and the refund are all still here and still correct, they are simply not consulted. Set ARROW_COST
  // true and the quiver is live again with no other edit — the same shape CRAFT_ON and DUAL_ON use.
  // Arrows still EXIST: a loosed shaft lands as an ordinary drop and can be picked up, stacked and thrown, and
  // the badge bake for them is still in ui/hud.js. What changes is only whether drawing the bow costs one.
  const ARROW_COST = false;
  const shootArrow = () => { const dk = Math.min(1, Math.max(0, (bowRel - bowT0) / BOW_DRAW_MS));
    if (ARROW_COST && !arrowTake()) return false;      // empty quiver — nothing flies, and nothing is spent
    if (launchThrown(ARROW_IT, ARROW_V * dk, ARROW_UP * dk, 'arrow')) return true;
    if (ARROW_COST) addItem(ARROW_IT);                 // …and the launch itself refused (dead, editor): give it straight back rather than eat it
    return false; };
  // ── THE SPEAR ── heavier and slower than an arrow, and it LEAVES YOUR HAND: the spear is the projectile,
  // so throwing it costs you the item until you walk over and pick it up again (user).
  const SPEAR_WIND_MS = 320, SPEAR_V = ARROW_V * 0.62, SPEAR_UP = ARROW_UP;
  const throwSpear = () => {
    if (!SPEAR_IT || dead || ED.on) return false;
    // ── A THROW HAS TO BE PAID FOR BEFORE IT HAPPENS ── the spear IS the projectile, so the launch and the
    // `--sel.n` are one transaction. Launching first and checking the slot after meant a throw that could not
    // be paid for still flew: heldIt() answers with grabAnim.it while a pickup is still FLYING toward an empty
    // hand (ui/hud.js), so right-clicking a spear off the ground and releasing past the 90 ms threshold threw a
    // spear the hand did not hold yet — one landed in the world and the original arrived 360 ms later, turning
    // one spear into two, repeatably. Checked up front, like dropHeld's own `if (!sel) return`.
    const sel = slots[selSlot];
    if (!sel || sel.it !== SPEAR_IT) return false;
    const wk = Math.min(1, Math.max(0, (bowRel - bowT0) / SPEAR_WIND_MS));
    if (!launchThrown(SPEAR_IT, SPEAR_V * (0.55 + 0.45 * wk), SPEAR_UP * wk, 'spear')) return false;
    if (--sel.n <= 0) { slots[selSlot] = null; slotTidy(); }   // …and it is gone from the hand the instant it flies
    return true;
  };
  const PICK_ROCK = new Set(PEBBLE), PICK_STICK = new Set([STICK_S]), PICK_BOULDER = new Set(BROCK);
  const PICK_CONE = new Set(); if (CONEV) for (const p of CONEV.vox) PICK_CONE.add(p >>> 24);   // pinecone ids (ground cones AND tree-hung ones) → pinecone item
  if (ROCKV) for (const p of ROCKV.vox) PICK_ROCK.add(p >>> 24);                 // the field stone (rock.vox) → rock item
  for (const sm of STICKV) for (const p of sm.vox) PICK_STICK.add(p >>> 24);     // stick_1/stick_2 → twig item
  // ── AND THE BLOSSOM TWIG'S LEAF IS DELIBERATELY NOT IN HERE ── STICKB wears BLOSLEAF ids, which are the
  // CANOPY's ids, and PICK_STICK is walked by the right-click flood: adding them would make every pink crown in
  // the world right-click up as a twig, which is verbatim the pinecone/stick collision recorded at palOwn in
  // assets/palette.js. So a blossom twig picks up by its BROWNS, exactly the way a fruit picks up by its flesh
  // and leaves its oak-leaf stalk behind (see PICK_APPLE below) — the leaf is scenery on a pickable object, and
  // that is the established design here rather than a shortfall.
  for (const sm of STICKB) for (const p of sm.vox) { const id = p >>> 24; if (!BLOSLEAF.includes(id)) PICK_STICK.add(id); }
  const PICK_TWIG = new Set([...PICK_STICK, ...PICK_CONE]);   // the one set the right-click flood walks: a stick and a pinecone are told apart by the ids the COMPONENT turns out to contain, not by which set the first voxel matched. Kept as a union so the classifier still works if the two ever share ids again (they did — see palOwn).
  // ── AND A FALLEN LOG IS NOT A TWIG (user 2026-08-20: "audit all objects in the terrain") ── found by
  // __vb.pickTable(): log.vox wears ids 49/50/51/52 and every one of them is in PICK_STICK, because both are
  // pine-trunk browns out of a FULL 256-entry palette. So the right-click flood claimed a 372-voxel log,
  // stopped at the twig's 24-cell cap, handed the player a twig and left a 24-voxel bite out of the log.
  // The cone/stick collision two lines up was solved by asking what ids the component CONTAINS; that cannot
  // work here, because the log's ids are a SUBSET of the stick's — there is no id a log has that a stick does
  // not. SIZE is the honest discriminator and it is not close: the biggest stick model is 17 voxels and the
  // log is 372. Read off the models rather than written down, so re-authoring stick_2.vox cannot leave it stale.
  const TWIG_MAX = STICKV.concat(STICKB).reduce((a, m) => Math.max(a, m.vox.length), 0) || 24;
  const TWIG_CAP = TWIG_MAX * 2 + 1;                   // …and TWO fused sticks are still two sticks: the cap admits a pair (a scatter can drop them touching) and the +1 is what lets the caller SEE that it overflowed rather than guessing at a full bucket
  // ── AND THE FRUIT (user 2026-08-17: "have the player able to right click an apple from a tree and pick it
  // up") ── the apple and the orange do NOT share the twig's problem, and that is worth saying because it is
  // why this is two sets instead of one union with a component classifier. FRUITC is minted in palette.js and
  // RESERVED in palOwn, so palShare can never hand an apple's red to anything else: an id in PICK_APPLE is an
  // apple, full stop, and a flood started on one can never wander into an orange even where two crowns
  // interleave. Species is decided by which set the hit voxel matched, not by what the flood turns up.
  //
  // THE FLOOD IS THE FLESH ONLY, AND THAT IS THE DESIGN, not a limitation. A stamped fruit is 24 voxels: 19 of
  // flesh plus a 5-voxel stalk-and-leaf that wears an OAK LEAF id (assets/bow.js). Flooding those in would run
  // straight out into the crown they are hanging in and take the whole tree. So the flesh comes away and the
  // stalk stays on the branch, which is also what picking an apple looks like.
  //
  // TWO NUMBERS, BOTH MEASURED AGAINST THE ART RATHER THAN GUESSED:
  //   FRUIT_CAP 26, against one fruit's 19. Over the cap floodScan refuses the whole region — the rule the
  //     boulders already run on, "never leave a half-eaten stump" — so the cap has to clear one fruit and stop
  //     short of two. It does: two fruit close enough to fuse would be 38. That refusal is not free (a fused
  //     pair becomes unpickable) so it was measured before being accepted — simulating stampOak's own anchor
  //     draw over all six fruiting crowns, 24,000 trees, a pair lands close enough to touch on 0.10% of them.
  //   FRUIT_MIN 8, against the CHERRY. The berry bushes wear FRUITC[0] as well — the same red serves the apple
  //     and the cherry, which is how three ids covered four things — so without a floor, right-clicking a bush
  //     would hand you a whole apple for a 1-voxel berry. Berries are scattered one per angular sector: over
  //     both bushes the largest 26-connected clump measured is TWO voxels, so 8 separates them by a factor of
  //     four in both directions and still accepts an apple the player has already chopped half of.
  const PICK_APPLE = new Set([FRUITC[0]]), PICK_ORANGE = new Set([FRUITC[2]]);
  const FRUIT_IDS = new Set([...PICK_APPLE, ...PICK_ORANGE]);   // the cheap reject the pick ray tests before paying for a flood
  const FRUIT_MIN = 8, FRUIT_CAP = 26;
  // READ-ONLY, and deliberately so: it decides WHAT is there and hands back the cells, and the caller does the
  // removing. That is what lets tryPickup and pickAim (sim/life/stamped.js) run the identical test, so the
  // crosshair square can never promise a pick that the click then refuses.
  // ── …AND THE STEM AND THE LEAF COME WITH IT (user 2026-08-17: "when I pick up the apple, only the red
  // base comes with me. the brown stem and the leaf stay behind as static voxels") ── the flood above is over
  // the FLESH id alone, because that is the id the crosshair test keys on; the fruit's other five voxels wear
  // two different ones (assets/bow.js: a minted brown for the stalk, and an OAK LEAF id for the blade) so they
  // were never in the region and stayed hanging in the crown.
  //
  // WHY THIS IS NOT SIMPLY A WIDER FLOOD, which is the obvious fix and is wrong: the blade wears the SAME id as
  // every leaf in the tree, and world/terrain.js hangs each fruit one column under a canopy anchor — so the
  // fruit's top voxel is face-adjacent (or diagonal) to a crown leaf, and one 26-connected flood over that id
  // walks straight out into the canopy. It cannot be separated by CONNECTIVITY either: the fruit's blade and
  // the leaf above it are one component, which is the same wall assets/bow.js records for the stem/leaf split.
  //
  // So it is bounded by GEOMETRY, which the models make exact. Both fruit are authored on a 4x3x5 grid with the
  // flesh at the bottom and the stem/leaf in the two courses above it, and stampModel only ever rotates them
  // about vertical — so relative to the flesh's own box every extra voxel is at most +2 UP and one across,
  // while the anchor leaf that must not be taken is +3. A box of (+/-1, -1..+2, +/-1) around the flesh
  // therefore contains the whole fruit and excludes the tree by construction.
  // Everything is measured in LOCAL offsets from the picked voxel rather than in grid coordinates, because a
  // min/max over wrapped x/z is the toroidal-seam bug that once produced a 2048-wide box.
  const FRUIT_XR = 4;                                  // half-width of the local scan. The widest fruit is 4 across and 5 tall, and the pick can land on any of its flesh voxels, so this reaches the whole model from anywhere inside it.
  // …AND HOW MANY OF THEM THERE ARE IS NOT A GUESS EITHER. The box alone is not tight enough: MEASURED on a
  // real orange in a crown, it handed back 8 extra voxels where the model has 3 — the other 5 were canopy
  // leaves packed against the fruit, wearing the same id, inside the same box. So the count comes from the
  // MODEL: assets/bow.js resolved every fruit voxel to an id, so "how many are not flesh" is a fact about the
  // art, and the nearest that many to the flesh win. On both fruit every one of its own cells is nearer the
  // flesh than any canopy leaf can be (the model is 4x3x5 and the crown starts outside it), so this is exact
  // in the ordinary case and degrades to "a leaf or two off the wrong blob" rather than a notch in the tree.
  const fruitExtraN = (fleshId) => {
    for (const f of (typeof FRUITV === 'undefined' ? [] : FRUITV)) {   // the model that IS this fruit, matched on its FLESH ID rather than on an index into fruit.json's order
      let body = 0;
      for (const p of f.vox) if ((p >>> 24) === fleshId) body++;
      if (body) return f.vox.length - body;
    }
    return 0;
  };
  const fruitCrownCells = (x, y, z, set) => {
    const idAt = (dx, dy, dz) => (y + dy < 1 || y + dy >= WY) ? 0 : W[gwrap(x + dx, WX) + (y + dy) * WX + gwrap(z + dz, WZ) * WX * WY];
    const iiAt = (dx, dy, dz) => gwrap(x + dx, WX) + (y + dy) * WX + gwrap(z + dz, WZ) * WX * WY;
    const extra = new Set([FRUIT_STEM_ID, FRUIT_LEAF_ID].filter(Boolean));   // read at CALL time: both are assigned during boot, and one of them is 0 until then
    if (!extra.size) return [];
    let lo = null, hi = null;                          // the FLESH's own local box — the thing the +2 is measured from
    for (let dz = -FRUIT_XR; dz <= FRUIT_XR; dz++) for (let dy = -FRUIT_XR; dy <= FRUIT_XR; dy++) for (let dx = -FRUIT_XR; dx <= FRUIT_XR; dx++) {
      if (!set.has(idAt(dx, dy, dz))) continue;
      if (!lo) { lo = [dx, dy, dz]; hi = [dx, dy, dz]; continue; }
      for (let k = 0; k < 3; k++) { const d = [dx, dy, dz][k]; if (d < lo[k]) lo[k] = d; if (d > hi[k]) hi[k] = d; }
    }
    if (!lo) return [];
    const inBox = (dx, dy, dz) => dx >= lo[0] - 1 && dx <= hi[0] + 1 && dz >= lo[2] - 1 && dz <= hi[2] + 1 && dy >= lo[1] - 1 && dy <= hi[1] + 2;
    const want = fruitExtraN([...set][0]);            // the model's own non-flesh voxel count — 5 on the apple (2 stalk + 3 blade), 3 on the orange
    if (!want) return [];
    const cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;   // …and the flesh's own centre, which is what "nearest" is measured from
    const out = [], seen = new Set(), q = [];
    const push = (dx, dy, dz) => { const k = dx + '|' + dy + '|' + dz;
      if (seen.has(k) || !inBox(dx, dy, dz) || !extra.has(idAt(dx, dy, dz))) return;
      seen.add(k); q.push([dx, dy, dz]);
      out.push({ ii: iiAt(dx, dy, dz), d2: (dx - cx) * (dx - cx) + (dy - cy) * (dy - cy) + (dz - cz) * (dz - cz) }); };
    for (let dz = lo[2] - 1; dz <= hi[2] + 1; dz++) for (let dy = lo[1] - 1; dy <= hi[1] + 2; dy++) for (let dx = lo[0] - 1; dx <= hi[0] + 1; dx++) {
      if (!set.has(idAt(dx, dy, dz))) continue;        // seed off the FLESH: the first course of stem/leaf is whatever touches it
      for (let nz = -1; nz <= 1; nz++) for (let ny = -1; ny <= 1; ny++) for (let nx = -1; nx <= 1; nx++)
        if (nx || ny || nz) push(dx + nx, dy + ny, dz + nz);
    }
    while (q.length) { const [qx, qy, qz] = q.pop();   // …then out through the rest of the stalk/blade, still inside the box
      for (let nz = -1; nz <= 1; nz++) for (let ny = -1; ny <= 1; ny++) for (let nx = -1; nx <= 1; nx++)
        if (nx || ny || nz) push(qx + nx, qy + ny, qz + nz); }
    out.sort((a, b) => a.d2 - b.d2 || a.ii - b.ii);   // ties on the index so a fruit comes apart the same way every time
    return out.slice(0, want).map((e) => e.ii);
  };
  // ══ THE SAME BOUND, FOR THE TWIG'S LEAF (user 2026-08-18: "when picking up the stick, you leave the pink
  // leaves behind") ══ generalised out of fruitCrownCells above rather than written a second time, because it is
  // the same problem with the same correct answer and two copies of it would drift.
  //
  // THE PROBLEM. A pickup has to answer two DIFFERENT questions and they were being answered with one set:
  //   * what may TRIGGER this pickup — and the blossom leaf must not, or every pink crown in the world
  //     right-clicks up as a twig (the pinecone/stick collision recorded at palOwn in assets/palette.js);
  //   * what the pickup REMOVES once it has decided what the thing is — and here the leaf must be included,
  //     or it is left hanging in the air with its twig gone from under it.
  // Excluding it from both is what left the leaf floating. So the trigger set stays clean and the removal is
  // widened, bounded exactly the way the fruit's stalk is: seeded off the body, flooded only through the extra
  // ids, clipped to the body's own box, and capped at the MODEL'S OWN count of them. A wider flood is what must
  // never happen — a twig lying under a cherry tree would take the whole crown.
  const extraCells = (cells, extra, want) => {
    if (!extra.size || !want || !cells.length) return [];
    let lo = null, hi = null;                          // the BODY's own box, in world voxels this time (the fruit works in local offsets because it is asked about a point, not given a component)
    const xyz = (ii) => [ii % WX, ((ii / WX) | 0) % WY, (ii / (WX * WY)) | 0];
    for (const ii of cells) { const p = xyz(ii);
      if (!lo) { lo = p.slice(); hi = p.slice(); continue; }
      for (let k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; } }
    const pad = 1;                                     // one voxel: a leaf is face- or diagonally-adjacent to the twig it grew on, never further
    const inBox = (gx, gy, gz) => gx >= lo[0] - pad && gx <= hi[0] + pad && gy >= lo[1] - pad && gy <= hi[1] + pad && gz >= lo[2] - pad && gz <= hi[2] + pad;
    const iiOf = (gx, gy, gz) => gx + gy * WX + gz * WX * WY;
    const cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
    const out = [], seen = new Set(), q = [];
    const push = (gx, gy, gz) => {
      if (gy < 1 || gy >= WY || !inBox(gx, gy, gz)) return;
      const ii = iiOf(gx, gy, gz); if (seen.has(ii) || !extra.has(W[ii])) return;
      seen.add(ii); q.push([gx, gy, gz]);
      out.push({ ii, d2: (gx - cx) * (gx - cx) + (gy - cy) * (gy - cy) + (gz - cz) * (gz - cz) }); };
    for (const ii of cells) { const [gx, gy, gz] = xyz(ii);
      for (let nz = -1; nz <= 1; nz++) for (let ny = -1; ny <= 1; ny++) for (let nx = -1; nx <= 1; nx++)
        if (nx || ny || nz) push(gx + nx, gy + ny, gz + nz); }
    while (q.length) { const [qx, qy, qz] = q.pop();
      for (let nz = -1; nz <= 1; nz++) for (let ny = -1; ny <= 1; ny++) for (let nx = -1; nx <= 1; nx++)
        if (nx || ny || nz) push(qx + nx, qy + ny, qz + nz); }
    out.sort((a, b) => a.d2 - b.d2 || a.ii - b.ii);    // ties on the index, so the same twig comes apart the same way every time
    return out.slice(0, want).map((e) => e.ii);
  };
  // The blossom leaf ids a twig can carry, and HOW MANY — read off the models themselves so re-authoring
  // stick_1.vox cannot leave this number stale. Empty when the cherry forest's models never built.
  const TWIG_EXTRA = new Set((typeof TWIGPINK === 'undefined' ? [] : TWIGPINK).concat(typeof TWIGWHITE === 'undefined' ? [] : TWIGWHITE));   // the AUTHORED pink twigs' own leaf ids (assets/bow.js), NOT the canopy's BLOSLEAF: the user's pink_stick art carries a ramp of its own, and keying this on the canopy would both miss the real leaf and risk the flood reaching a crown
  const TWIG_EXTRA_N = (typeof STICKB === 'undefined' ? [] : STICKB)
    .reduce((n, m) => Math.max(n, m.vox.filter((p) => TWIG_EXTRA.has(p >>> 24)).length), 0);
  const twigLeafCells = (cells) => extraCells(cells, TWIG_EXTRA, TWIG_EXTRA_N);
  const fruitAt = (x, y, z) => {
    if (!APPLE_IT) return null;                        // no fruit item loaded (assets/food/apple missing) — nothing to pick up into
    const v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    const set = PICK_APPLE.has(v) ? PICK_APPLE : (PICK_ORANGE.has(v) ? PICK_ORANGE : null);
    if (!set) return null;
    const sc = floodScan(x, y, z, set, FRUIT_CAP);     // over FRUIT_CAP comes back empty, which the length test below reads as "not one fruit"
    if (sc.cells.length < FRUIT_MIN) return null;      // a berry, or a stub too small to be worth a whole fruit. Asked of the FLESH alone, before the crown is added: the flesh count is what identifies a fruit, and a berry bush has no stem to confuse it
    return { it: set === PICK_ORANGE ? ORANGE_IT : APPLE_IT, cells: sc.cells.concat(fruitCrownCells(x, y, z, set)) };
  };
  // ── SHRUBS ARE PASSABLE TO WILDLIFE, SOLID TO THE PLAYER (user 2026-08-16: "the cobra got stuck on a
  // shrub") ── giving the shrubs a hitbox an hour earlier was the user's own request, and it immediately
  // snagged a 19-segment snake on a knee-high bush. Both wants are satisfiable at once because they read
  // DIFFERENT tables: the player collides through `solid()` on solidTab, while a walking creature asks
  // nvClut/WORM_PASS. Listing the shrub ids here leaves the player's hitbox exactly as asked and lets the
  // animals walk through, which is also what they did for the whole time the shrubs were soft.
  const WORM_PASS = new Set([...PICK_CONE, ...PICK_STICK, ...PICK_ROCK, ...SHRUBC, ...SHRUBF]);        // worms crawl OVER small ground clutter (pinecones/sticks/field stones) instead of tripping on it → getting stuck → teleporting (user)
  const PASSTHRU = new Set([...GRASS, ...FERNIDS, WATER_T, WATER_B]);   // the pick ray sees through soft decor + water
  // ── THE MEADOW FLOWER, PICKABLE (user 2026-08-20: "have the flowers in the terrain be able to be picked up
  // via right click") ── the PETALS only. The plant's one green (id 62, measured via __vbFlowerMat) is a GRASS
  // id — the flowers share the meadow's palette, which is why they cost the full table nothing — so flooding
  // over it would eat the grass the flower is standing in. Petals are unambiguous and they are the whole of
  // what reads as the flower.
  // AND THE TWO GREEN VOXELS ARE LEFT STANDING, deliberately. They sit at model z=0..1, they wear the grass's
  // own id, and they are surrounded by grass of that id: what is left behind after the bloom comes away is
  // indistinguishable from the grass beside it. Removing them would mean guessing which of several identical
  // green voxels in that column belonged to the plant, and guessing wrong takes the player's meadow apart.
  // IT LIVES *HERE*, BELOW PASSTHRU, AND NOT UP WITH THE OTHER PICK_ SETS. It is derived by subtracting
  // PASSTHRU, and PASSTHRU is a `const` declared further down this file: reading it from the PICK_ block
  // above is a temporal-dead-zone throw at load, which in this codebase is a game that never boots (it did —
  // "Cannot access 'PASSTHRU' before initialization", caught by __vb.errLog).
  // BUILT OFF THE MODELS, NOT OFF FLOWERIDS. FLOWERIDS is derived in assets/bow.js from FLOWERV's voxels
  // BEFORE the pink twin is minted (its own comment says so), so it can miss an id the blossom variant wears —
  // __vb.pickTable() caught exactly one, id 80, sitting on a plant no right-click could trigger. Reading the
  // models themselves is the same source stampFlower draws from, so the trigger set and the thing standing in
  // the world cannot disagree however the derivation changes later.
  const PICK_FLOWER = new Set();
  for (const m of (typeof FLOWERV === 'undefined' ? [] : FLOWERV).concat(typeof FLOWERV_CH === 'undefined' ? [] : FLOWERV_CH))
    for (const q of m.vox) { const i = q >>> 24; if (!PASSTHRU.has(i)) PICK_FLOWER.add(i); }
  const FLOWER_CAP = (typeof FLOWERV === 'undefined' ? [] : FLOWERV.concat(typeof FLOWERV_CH === 'undefined' ? [] : FLOWERV_CH))
    .reduce((a, m) => Math.max(a, m.vox.length), 0) + 4;   // the biggest flower model, plus slack: a cap under the model leaves half a bloom standing
  function floodScan(x, y, z, ids, cap) {              // READ-ONLY half of floodRemove: which cells the region covers, and which ids it is MADE of.
    const found = []; const kinds = new Set(); const q = [[x, y, z]]; const seen = new Set();   // `kinds` is what lets a caller tell two decorations apart when they share palette ids
    while (q.length && found.length <= cap) {
      const [qx, qy, qz] = q.pop(); const key = qx + '|' + qy + '|' + qz;
      if (seen.has(key) || qy < 0 || qy >= WY) continue; seen.add(key);
      const ii = gwrap(qx, WX) + qy * WX + gwrap(qz, WZ) * WX * WY;
      if (!ids.has(W[ii])) continue;
      found.push(ii); kinds.add(W[ii]);
      for (let nz = -1; nz <= 1; nz++) for (let ny = -1; ny <= 1; ny++) for (let nx = -1; nx <= 1; nx++)   // 26-connectivity — curved sticks step diagonally
        if (nx || ny || nz) q.push([qx + nx, qy + ny, qz + nz]);
    }
    return found.length > cap ? { cells: [], kinds } : { cells: found, kinds };   // over cap = refuse the whole region, never leave a half-eaten stump
  }
  function floodRemove(x, y, z, ids, cap) {            // TWO-PASS: collect first, remove only if the whole region fits the cap —
    const found = floodScan(x, y, z, ids, cap).cells;   // truncating mid-flood left half-eaten rocks (flat-faced grey "stone cube" stumps, e.g. two touching boulders > cap)
    for (const ii of found) W[ii] = 0;
    return found;
  }
