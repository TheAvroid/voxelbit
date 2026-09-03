  // ── FISH 2.0 CONFIG ── every swimmer tunable in ONE bag, live-tweakable from the console (__vb.fishCfg.xxx = …).
  // The AI itself runs in the creature tick (SWIM ⇄ FLEE states + ballistic AIR arcs) on the shared body predicate
  // fishBodyAt/fishFits/fishReach — the planner can only ever choose moves the mover will accept, which is the
  // anti-deadlock invariant that fixed "endlessly swims against the terrain".
  // ── FLYER FLIGHT RESPONSE ── butterflies and dragonflies (creature kind 0) bolt at double speed when the
  // player closes on them, the way the fish do. Kept beside FISH_CFG because it is the same mechanism and the
  // two want reading together; the firefly (kind 1) is deliberately outside it — it drifts at 26 and a startled
  // firefly is not a thing the night should have.
  // The MULTIPLIER matches the fish exactly. The RADIUS does not, and should not: a fish cruises at 22 and a
  // butterfly at 56, so the fish's 56-voxel sphere is two and a half seconds of fish travel but one second of
  // butterfly travel — shared, it would leave every butterfly permanently spooked. 30 is about half a second
  // of its own cruise, which is the same "it noticed you" beat the fish get.
  const FLY_THREAT_R = 30, FLY_FLEE_HOLD = 1.2, FLY_FLEE_MULT = 2.0;
  const FISH_CFG = {
    baseSpeed: 22,          // vox/s normal cruise — the tail-beat plays at animFps at this speed
    fleeMult: 2.0,          // flee speed = baseSpeed × this, EXACTLY (the animation scales with it: 24 → 48 fps)
    animFps: 24,            // swim-strip rate at baseSpeed, on the SIM clock — render fps never touches it
    // ── SPHERE OF INFLUENCE ── DOUBLED 28 -> 56 (user 2026-08-22: "double the sphere of influence of the koi
    // fish ... make all the fish have the same soi"). They already did: this is one shared number with exactly
    // two readers — the creature tick's threat scan for every world fish, and the editor stage's koi, which was
    // wired to read this same bag rather than a copy. There is no per-species threat radius to bring into line,
    // so raising it here raises it for the koi, the salmon, the minnow, the bass, the blue gill, the catfish and
    // the betta together. 5.6 m: a fish now breaks well before you are on top of it.
    threatR: 56,            // player/predator distance (vox) that triggers the flee state
    fleeHold: 1.2,          // s the flee state lingers after the threat leaves the radius (no flicker at the boundary)
    predatorKinds: [3],     // wbf creature kinds fish flee from (3 = ducks); set [] to disable predator flight
    schoolSpecies: [],      // NO schooling (user: fish must spread out EVENLY + consistently). Schooling spawned fish ±4.5 vox around a schoolmate — a knot that bypassed the ≥14-vox even-spacing AND wasn't re-validated for deep water (so shallow-placed schoolmates got recycled → 'sometimes no fish'). Empty = every fish swims alone, spread ≥14 apart across the water spots. Re-add ['salmon','minnow'] to restore shoaling.
    yawRate: 2.2,           // rad/s heading-change cap while cruising…
    fleeYawRate: 6.0,       // …and while fleeing (double speed needs sharper banking to keep clear of banks)
    pitchMax: 0.30,         // SUBTLE nose pitch cap (rad, ~17°) for climbs/dives — yaw is free, roll is impossible by construction
    pitchGain: 1.6,         // how strongly vertical velocity tips the nose
    // (NO replanS here on purpose: it advertised a 1.3 s channel re-aim cadence that nothing read — the field it was
    //  meant to drive, B.chRe, is assigned and never consulted. The real cadence is the ~14 Hz sense tick, B.senseRe,
    //  hard-coded in the creature tick's fish branch. Anything added to this bag must have a reader, or it is a dial
    //  wired to nothing and a tuner turns it for hours with no effect.)
    // ── SEPARATION (user 2026-09-02: "sometimes the fish cluster up in one area. fix this") ── FISH_APART in
    // sim/life/slots.js is a SPAWN rule and only a spawn rule: it places fish 28 apart and then nothing ever
    // looks at their spacing again. Measured on one pool — nearest-neighbour distance was min 33 / median 72
    // at spawn, and after the same fish had been swimming a while it was min 13, with four pairs inside the
    // very floor they were placed on. They do not converge globally (the median actually rises); a couple of
    // them simply drift into the same water and stay there, which is the clump you see.
    // It rides the BANK REPULSION vector in the creature tick rather than the whisker fan on purpose: the fan
    // picks ONE heading and a neighbour push is a nudge, not a lane choice, and the repulsion path is already
    // dt-scaled and frame-rate independent. sepK is scaled against a bank hit, which contributes (10 - d) for
    // d in 3.5..9 — so a fish at touching distance pushes about as hard as one shore sample, and a fish at
    // sepR pushes not at all. Schooling is OFF (see schoolSpecies), so nothing pulls the other way.
    // ── AND THE RANGE IS THE EVENNESS KNOB (user 2026-09-02: "make sure the fish are evenly spread out in the
    // water") ── 26 was sized to DEFEND the spawn floor, which it does: no pair inside 14 any more. But merely
    // not-touching is not evenly spread. Measured with 28 fish, the within-pool nearest-neighbour ran p10 20 /
    // median 43 in water wide enough to hold them much further apart, because a push that reaches 26 voxels
    // stops caring the moment a neighbour is 27 away and the fish then wanders as if alone.
    // Raising the RANGE and softening the PUSH is the pair that matters: a long, gentle field keeps working
    // across the whole pool, where a short hard one only ever settles a collision. Note what is NOT changed —
    // FISH_APART (the spawn rule) and the per-pool cap, which is proportional to a pool's own census area and
    // is already the right rule for even DENSITY: a small pond holding one fish and a big lake holding seven
    // is even spread, not uneven, and pushing on that would empty the ponds instead.
    sepR: 52,               // vox: how far another fish is still felt — 26 -> 52, so the field spans a pool rather than a collision
    sepK: 3.4,              // …and how hard, at touching distance, falling linearly to zero at sepR. SOFTENED with the longer reach (5 -> 3.4): the same push over twice the distance would out-shout the whisker fan's own lane choice and fish would stop following channels
    lookMin: 12,            // reactive wall-backstop lookahead floor (scales up with speed)
    turnCost: 2.0,          // ── HOW DEARLY THE PROBE PRICES A TURN (user 2026-08-07: "it keeps running into the banks") ── was 5 hard-coded. A fish cruises at 22 vox/s and banks at yawRate 2.2 rad/s, so a 90° turn costs it ~16 voxels of travel: in a river, the open lane is often only a few voxels longer than the blocked one, and at 5 the turn penalty ate that difference whole. The fish under-turned, closed on the bank, and the backstop then took over. At 2.0 it commits to the open lane while the bank is still far off, which is the whole point of a long-range probe.
    jump: {                 // ── LEAP ── frequency / height / distance / cooldown all here (user)
      cooldownMin: 9, cooldownMax: 31,   // s between leap attempts, divided by the species multiplier below
      vMin: 65, vMax: 88,   // launch speed up (vox/s) → peak ≈ v²/2g ≈ 12.8-23.5 vox above the surface (HEIGHT dial). DOUBLED 2026-07-20 (user): peak scales with v², so v went up by √2
      gravity: 165,         // vox/s² — the arc's fall (matches the game's ballistic feel)
      forward: 26,          // horizontal speed carried through the arc (vox/s) → DISTANCE ≈ forward · 2v/g ≈ 14-20 vox
      minDepth: 5,          // only water this deep can launch a leap (never from a shelf it could land back onto)
      armS: 4,              // max s the surface run-up may take before the attempt stands down
      species: { salmon: 1.0, minnow: 0.3, bass: 0.6, blue_gill: 0.9, catfish: 0.1 },   // per-species FREQUENCY multiplier (0 = that species never leaps). blue_gill 0.4 -> 0.9 (user asked whether it jumps: it did, but ~2.5x rarer than a salmon, so it read as never)
    },
  };
