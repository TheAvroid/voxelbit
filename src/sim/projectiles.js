  // @module — arrows and spears: launch, arc, impact, and the pick-up flood
  // @exports ARROW_ROLL, ARROW_UP, ARROW_V, PASSTHRU, PICK_BOULDER, PICK_CONE, PICK_ROCK, PICK_STICK, PICK_TWIG, SPEAR_WIND_MS, WORM_PASS, arrowChop, floodRemove, floodScan, launchThrown, shootArrow, throwSpear
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
    const okMat = digOnlyTab[id] ? ((v) => !!digOnlyTab[v]) : (pickOnlyTab[id] ? ((v) => !!pickOnlyTab[v]) : ((v) => !digOnlyTab[v] && !pickOnlyTab[v]));
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
    if (!hit && !standing) hit = phChopDecor(ix, iy, iz, ARROW_CHOP_RAD, ARROW_CHOP_BITE, okMat);   // mushrooms, ferns, ground logs — and soil and stone, which is what okMat is for
    if (!hit) return false;
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
    if (drops.length > 4) drops.shift();                // …and never more than the composite can draw
    unlockProjectile();                                // it is away and flying — the discovery is earned (user)
    return true;
  };
  // ── THE BOW ── the draw IS the power (user). Loosing is only allowed past half draw, so this runs 0.5 at
  // the earliest release to 1.0 at a full pull; ARROW_V is what a full draw is worth.
  const shootArrow = () => { const dk = Math.min(1, Math.max(0, (bowRel - bowT0) / BOW_DRAW_MS));
    return launchThrown(ARROW_IT, ARROW_V * dk, ARROW_UP * dk, 'arrow'); };
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
  const PICK_TWIG = new Set([...PICK_STICK, ...PICK_CONE]);   // the one set the right-click flood walks: a stick and a pinecone are told apart by the ids the COMPONENT turns out to contain, not by which set the first voxel matched. Kept as a union so the classifier still works if the two ever share ids again (they did — see palOwn).
  // ── SHRUBS ARE PASSABLE TO WILDLIFE, SOLID TO THE PLAYER (user 2026-08-16: "the cobra got stuck on a
  // shrub") ── giving the shrubs a hitbox an hour earlier was the user's own request, and it immediately
  // snagged a 19-segment snake on a knee-high bush. Both wants are satisfiable at once because they read
  // DIFFERENT tables: the player collides through `solid()` on solidTab, while a walking creature asks
  // nvClut/WORM_PASS. Listing the shrub ids here leaves the player's hitbox exactly as asked and lets the
  // animals walk through, which is also what they did for the whole time the shrubs were soft.
  const WORM_PASS = new Set([...PICK_CONE, ...PICK_STICK, ...PICK_ROCK, ...SHRUBC, ...SHRUBF]);        // worms crawl OVER small ground clutter (pinecones/sticks/field stones) instead of tripping on it → getting stuck → teleporting (user)
  const PASSTHRU = new Set([...GRASS, ...FERNIDS, WATER_T, WATER_B]);   // the pick ray sees through soft decor + water
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
