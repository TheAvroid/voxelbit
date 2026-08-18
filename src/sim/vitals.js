  // @module - player vitals: a FIVE-POINT health bar, the damage the hazards take off it, and the food that puts it back
  // @exports VIT, VIT_HP_MAX, vitFoods, vitTick, vitHurt, vitEat, vitReset, vitSprintOK, vitOnAttack, vitOnMine, vitRedLevel
  // ── WHY THIS EXISTS ── until this module `die(why)` WAS the whole of mortality: every hazard killed outright and
  // the player had no health at all (see the comment at the cobra contact test). Health only means something once the
  // hazards stop one-shotting you, so this module owns both halves — the vitals, and the `vitHurt` entry point the
  // hazards now call instead of `die`.
  //
  // ── FIVE POINTS, AND THE PIXELATED SCREEN IS THE BAR (user 2026-08-17: "theres only 5 health points. the health is
  // represented by the pixelated screen. each food eaten increase the health by one point") ── this file used to be a
  // port of Minecraft's FoodStats: a 20-point health bar, a 20-point hunger bar, hidden saturation, a hidden
  // exhaustion accumulator, two regeneration branches, starvation damage, and — because none of that was ever drawn —
  // a SECOND death path that counted five hits. Four representations of "how hurt am I", three of them invisible.
  //
  // There is now exactly ONE. `VIT.hp` is an integer 0..5. `vitRedLevel()` is `5 - hp`, which is precisely the 0..4
  // the hurt-flash block in render/wgsl/blit.js already reads off `u.hurtV.z` to decide how far the red pixels creep
  // in ("the band creeps inward one step per heart"). So the pixelation is not a cue ABOUT the health — it IS the
  // health bar, and it has five states because the bar has five points. The floating heart row is the same number
  // read a second way: tick-camera divides `VIT.hp` by `VIT_HP_MAX / HEART_N`, and with both of those now 5 that
  // division is by one — five hearts, one per point, no conversion left to get wrong.
  //
  // ── AND HUNGER IS GONE (user 2026-08-17: "can you remove the hunger mechanic. instead food is going to replenish
  // health. health doesnt replenish on its own … this simplifies the hunger system, the player doesnt have to eat all
  // the time, unless of course he is taking damage all the time") ── not switched off behind a flag as it was on
  // 2026-08-16, DELETED. Gone with it: the hunger bar, saturation, the exhaustion accumulator and every EXH_
  // constant, both regeneration branches, starvation damage, and the 20 Hz fixed step that existed solely because
  // every one of those constants was a Minecraft tick count. Nothing in here now runs on a clock at all.
  const VIT_HP_MAX = 5;

  // ── WHAT A HAZARD'S `amount` MEANS ── every caller of `vitHurt` is in another fragment (a cobra strike in
  // tick-creatures, drowning in tick-camera, lava/quicksand/cactus/falling in tick-body) and every one of them quotes
  // its damage on Minecraft's 20-point scale, where a heart is 4 points. Rewriting nine hazard sites to a new unit is
  // how a tuned curve gets quietly detuned, so the numbers stay where they are and the conversion happens HERE, at
  // the single door they all come through: one health point per heart's worth of damage, and never less than one, so
  // no blow is free.
  //
  // This is deliberately not a re-balance. The old system's real killer was already a count of five — five hits ended
  // the run "whatever each one took off" — so a cactus, a quicksand tick and a drowning tick each cost exactly what
  // they always did: one of five. What the divisor adds back is the severity the hit count threw away: a cobra (5) is
  // worth two points where a scorpion (3) is worth one, and the fall curve keeps its authored shape — a 15 m drop
  // works out at 20 damage, which was "a full 20-point bar" then and is a full five-point bar now, so the height that
  // killed a healthy player still kills them.
  const VIT_DMG_PER_POINT = 4;

  // ── FOOD ── { hp: health points restored }. One point per item, flat, which is the user's own number and the reason
  // this table no longer carries Minecraft's { h, s } pair: hunger restored and saturation modifier were inputs to a
  // regeneration system that no longer exists, and raw meat's 3-and-0.3 described a bar the player can no longer see.
  // The table survives because it is also the answer to "what is edible" — ui/audio.js derives EDIBLE from these
  // keys rather than keeping a second list that could drift out of step.
  //
  // ── AND THERE ARE THREE FOODS NOW (user 2026-08-17: "then the player can right click to eat it. (if down
  // hearts.) apply this to the orange as well") ── an apple and an orange, picked off an oak. ALL THREE RESTORE
  // ONE POINT, and that is not indifference about balance: it is the user's own rule from earlier the same day,
  // and it is the reason this table lost its { h, s } pair — "each food eaten increase the health by one point".
  // A fruit worth 2 would contradict that sentence, and a fruit worth half a point would break something harder:
  // VIT.hp is an integer 0..5, and both vitRedLevel and the floating heart row index straight off it, so a
  // fractional heal would leave the pixelation between two of its five states and the row between two hearts.
  // What separates a fruit from a steak is therefore NOT the number, and it should not be. Meat costs a kill;
  // fruit is forage. The scarcity is already in the world — 10% of oak TREES carry any, three to nine each — and
  // the pacing is already in EAT_MS, one bite per 900 ms whatever is in your hand. Fruit stacks to 8, so a good
  // tree is about one full bar's worth of healing that you have to go and find.
  // THE TWO FRUIT MATCH EACH OTHER for the same reason: as far as the bar is concerned they are one fruit in two
  // colours, and an orange that healed differently would be a rule with nothing on screen to explain it.
  // APPLE_IT and ORANGE_IT are the FIRST frame of each eat strip (assets/held-items.js) — the only id of either
  // run that can ever sit in a hotbar slot — so registering the head registers exactly the right thing, and the
  // twelve frames behind it stay correctly inedible.
  //
  // BOUND LAZILY, and that is not a style choice. The item ids are assigned by an ASYNC asset load, so writing this
  // table from held-items.js — fragment 22, against this module's fragment 56 — threw "Cannot access VIT_FOODS before
  // initialization" and left the game on a black screen. Registering from the other side is a TDZ hazard in one
  // direction and a table keyed on id 0 in the other. Rebuilding on first use after the id changes is immune to both,
  // and costs one string compare per lookup. The key is all THREE ids and not just the meat's: they are assigned by
  // three different loads, and keying on one of them would freeze the table at whatever the other two were when the
  // meat first resolved — which, for the fruit, is 0.
  const VIT_FOODS = {};
  let vitFoodKey = '';
  function vitFoods() {
    const k9 = MEAT_IT + '/' + APPLE_IT + '/' + ORANGE_IT;
    if (vitFoodKey !== k9) {
      vitFoodKey = k9;
      for (const k in VIT_FOODS) delete VIT_FOODS[k];
      if (MEAT_IT) VIT_FOODS[MEAT_IT] = { hp: 1 };
      if (APPLE_IT) VIT_FOODS[APPLE_IT] = { hp: 1 };
      if (ORANGE_IT) VIT_FOODS[ORANGE_IT] = { hp: 1 };
    }
    return VIT_FOODS;
  }
  const vitFoodOf = (it) => (it && vitFoods()[it]) || null;

  // hurtT is the only thing left with a clock: it rides 1 -> 0 over 0.55 s and drives the hit KICK — the swell the
  // floating heart row gives on a blow, and the spike the pixelation paints on top of the standing level. It is an
  // event, not a state, which is exactly why it is separate from hp.
  const VIT = { hp: VIT_HP_MAX, hurtT: 0, onDeath: null };

  function vitReset() { VIT.hp = VIT_HP_MAX; VIT.hurtT = 0; }

  // ── NOTHING GATES SPRINTING ANY MORE ── this was Minecraft's "no sprinting at hunger 6 or below", and tick-body
  // stopped consulting it on 2026-08-16 because a hidden rule that disables a movement key reads as a broken key
  // ("the sprint button stop working randomly"). With hunger deleted there is no number left that could gate it even
  // in principle, so sprinting is now gated by exactly two things and both are visible to the player: the sprint key,
  // and not being crouched. The predicate survives only because main/debug-api.js reports it; it is a constant true
  // and no caller branches on it.
  const vitSprintOK = () => true;
  // Swinging and digging used to charge exhaustion, which is how an active player got hungry. There is no exhaustion
  // to charge. They stay as no-ops because sim/life/reactions.js and main/tick-camera.js call them on every landed
  // blow and every chop, and a missing name there is a ReferenceError mid-swing rather than a compile error.
  const vitOnAttack = () => {};
  const vitOnMine = () => {};

  // `bypass` was "damage armour could never have stopped" — Minecraft charges no exhaustion for drowning, falling or
  // lava, which is what stopped starvation feeding itself into a death spiral. There is no exhaustion and no
  // starvation, so it now reads as nothing at all. The parameter is kept because five call sites pass it and because
  // it still documents, at those call sites, which hazards are unblockable.
  function vitHurt(amount, why, bypass) {
    if (amount <= 0 || VIT.hp <= 0) return;
    VIT.hp = Math.max(0, VIT.hp - Math.max(1, Math.ceil(amount / VIT_DMG_PER_POINT)));
    VIT.hurtT = 1;
    vitHurtFx();
    // ── ONE DEATH, ONE THRESHOLD ── there were two paths here (hp reaching zero, and a five-hit run) plus a third
    // that starved you. Starvation went with hunger, and the hit run went because it WAS this bar all along: it
    // counted to five because the player could not read a health total, and now the screen shows them one. A player
    // who is two points from dying can no longer be looking at a clear screen, so nothing has to reconcile a hidden
    // counter against a visible bar.
    if (VIT.hp <= 0 && VIT.onDeath) VIT.onDeath(why || 'you died');
  }

  // ── HOW RED THE SCREEN IS (user 2026-08-16: "full hearts is no shade, then heart 0 is the gameover screen") ── 0 at
  // full health and 1..4 as the points go, which is four shades because the fifth step is death, not a colour. It used
  // to be the WORSE of two readings, hp and the hit run, because either could kill you; with one bar it is a
  // subtraction. NOTHING FADES IT — the level only comes down when hp goes up, and hp only goes up when you eat. That
  // is the whole mechanic made visible: the red on the screen is a standing bill, and food is the only way to pay it.
  const vitRedLevel = () => Math.max(0, Math.min(4, VIT_HP_MAX - VIT.hp));

  // ── THE HURT SPURT ── literally the call a struck creature makes, aimed at the player instead:
  // spawnHitSparks throws the four red HITRED_IT voxels the blood burst uses, so taking a hit and landing one
  // spit the same 10 cm voxels. Placed in FRONT of and below the eye — at the camera itself all four spawn
  // inside the near plane and are never seen.
  function vitHurtFx() {
    // DISTANCE MATTERS MORE THAN SIZE HERE. These are the same 10 cm voxels a struck creature throws, but at
    // the 3 voxels (30 cm) I first used they sit inside arm's reach of the camera and perspective blows each
    // one up into a metre-wide slab across the view. 14 voxels (~1.4 m) forward and 4 down puts them out where
    // the player reads them as flecks coming off their own body, which is what they are.
    const cp = Math.cos(P.pitch);
    const fx = Math.sin(P.yaw) * cp, fz = Math.cos(P.yaw) * cp;
    // ── THEY COME OUT OF THE PLAYER (user 2026-08-16) ── spawnHitSparks throws its four voxels from one point
    // with a random ring of velocity, which at a fixed 8 voxels ahead reads as a burst hanging in the air in
    // front of the camera rather than something leaving the body. So the burst is authored here instead: it
    // STARTS on the player's own chest, just clear of the near plane, and every voxel is given velocity pointing
    // AWAY from that chest — forward-biased so they travel out into view instead of past the ear. The motion is
    // what sells the origin: the eye reads four flecks receding from a point on the body, and the point they
    // recede from is the player. Same 10 cm HITRED_IT voxels, same lifetime, same red flag as a struck creature.
    const cx = P.x + fx * 2.2, cy = smoothEye - 3.2, cz = P.z + fz * 2.2;
    const rx = fz, rz = -fx;                                   // the player's right, for the lateral spread
    for (let i = 0; i < 4; i++) {
      const side = (i - 1.5) * 0.55 + (Math.random() - 0.5) * 0.5;   // fan across the chest, one either side of centre
      const out = 26 + Math.random() * 22;                     // outward speed — fast enough that frame one is already leaving
      sparks3d[i] = { x: cx + rx * side * 1.8, y: cy + (Math.random() - 0.5) * 2.2, z: cz + rz * side * 1.8,
        vx: (fx * 0.85 + rx * side * 0.9) * out, vy: 10 + Math.random() * 16, vz: (fz * 0.85 + rz * side * 0.9) * out,
        born: performance.now(), life: 0.4 + Math.random() * 0.3, ph: Math.random() * 6.283, smoke: false, red: true };
    }
    // ── AND THE SAME VOICE (user 2026-08-16: "play the same sound that plays when the player hits life") ──
    // playLifeHit is the pool every blow that lands on a living thing already uses, and it attenuates with
    // distance from the ear. Passing the player's OWN position puts them at zero range, so a hit taken is the
    // same sound at full strength as a hit landed — which is what "the same sound" has to mean here.
    playLifeHit(P.x, smoothEye, P.z);   // 8, not 14 (user 2026-08-16: "bring the blood voxels closer into the player"). It started at 3, where perspective blew each 10 cm voxel into a metre-wide slab across the view; 14 read as distant flecks. 8 keeps them clearly the player's own without filling the screen
    hurtScreen();
  }

  // ── ONE BITE, ONE POINT ── and the refusal is a FULL-HEALTH test, not the full-hunger test it replaced. That test
  // was the bug: with hunger frozen at max the bar was always full, so `tryEat` refused every bite and a hurt player
  // could not eat at all. The rule it was written for still holds and is the reason a test belongs here — a bite that
  // would heal nothing must not consume the item — it just has to ask about the bar the food actually fills.
  function vitEat(it) {
    const f = vitFoodOf(it);
    if (!f || VIT.hp >= VIT_HP_MAX) return false;
    VIT.hp = Math.min(VIT_HP_MAX, VIT.hp + f.hp);
    return true;
  }

  // ── AND THAT IS THE WHOLE TICK ── it used to spend exhaustion, run one of three regen/starve branches on a 20 Hz
  // fixed step, charge exhaustion per metre walked, and decay a hit run. Health no longer changes with time in ANY
  // direction: the only two things that move it are `vitHurt` and `vitEat`. What is left is the hit kick riding down
  // over 0.55 s, which is an animation, not a mechanic. Still called unconditionally from tick-body (it once sat
  // inside the `if (P.fly) … else` movement branch and silently stopped the moment fly mode engaged).
  function vitTick(dt) {
    if (VIT.hurtT > 0) VIT.hurtT = Math.max(0, VIT.hurtT - dt / 0.55);
  }
