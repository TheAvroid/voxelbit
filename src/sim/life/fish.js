  // ── FISH 2.0 CONFIG ── every swimmer tunable in ONE bag, live-tweakable from the console (__vb.fishCfg.xxx = …).
  // The AI itself runs in the creature tick (SWIM ⇄ FLEE states + ballistic AIR arcs) on the shared body predicate
  // fishBodyAt/fishFits/fishReach — the planner can only ever choose moves the mover will accept, which is the
  // anti-deadlock invariant that fixed "endlessly swims against the terrain".
  const FISH_CFG = {
    baseSpeed: 22,          // vox/s normal cruise — the tail-beat plays at animFps at this speed
    fleeMult: 2.0,          // flee speed = baseSpeed × this, EXACTLY (the animation scales with it: 24 → 48 fps)
    animFps: 24,            // swim-strip rate at baseSpeed, on the SIM clock — render fps never touches it
    threatR: 28,            // player/predator distance (vox) that triggers the flee state
    fleeHold: 1.2,          // s the flee state lingers after the threat leaves the radius (no flicker at the boundary)
    predatorKinds: [3],     // wbf creature kinds fish flee from (3 = ducks); set [] to disable predator flight
    schoolSpecies: [],      // NO schooling (user: fish must spread out EVENLY + consistently). Schooling spawned fish ±4.5 vox around a schoolmate — a knot that bypassed the ≥14-vox even-spacing AND wasn't re-validated for deep water (so shallow-placed schoolmates got recycled → 'sometimes no fish'). Empty = every fish swims alone, spread ≥14 apart across the water spots. Re-add ['salmon','minnow'] to restore shoaling.
    yawRate: 2.2,           // rad/s heading-change cap while cruising…
    fleeYawRate: 6.0,       // …and while fleeing (double speed needs sharper banking to keep clear of banks)
    pitchMax: 0.30,         // SUBTLE nose pitch cap (rad, ~17°) for climbs/dives — yaw is free, roll is impossible by construction
    pitchGain: 1.6,         // how strongly vertical velocity tips the nose
    replanS: 1.3,           // s between channel re-aims (jittered per fish so the pods don't re-plan in lockstep)
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
