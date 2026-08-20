  // @module - player vitals: a FIVE-POINT health bar, the damage the hazards take off it, and the food that puts it back
  // @exports VIT, VIT_HP_MAX, VIT_FOOD_MAX, vitFoods, vitTick, vitHurt, vitEat, vitReset, vitSprintOK, vitOnAttack, vitOnMine, vitRedLevel, vitGoldLevel, vitExhaust, EXH_STEP
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
  // ── ONE TABLE, AND `strip` IS PART OF IT (user 2026-08-18: "make worms edible ... have all edible things share
  // the same eating animation") ── the animation used to be armed by a hand-written id test in ui/audio.js
  // (`bit === APPLE_IT || bit === ORANGE_IT || bit === MEAT_IT`), which is a second list of what is edible sitting
  // a file away from this one. Two lists is how the worm would have become edible WITHOUT an animation, and how
  // the next food after it would too. So the strip is declared HERE, beside the hp, and audio.js arms the chew
  // from the food's own entry. Adding a food is now one line in one place, and it cannot be half-added.
  //   strip: true  — carries a FOOD_EAT_N run of carved frames and is eaten down to a remnant
  //   strip: false — a single-model food; it still chews on the same clock, it just has no carved frames to show
  // The WORM is strip:false because assets/life/worm is a crawl cycle, not a bite-down strip: indexing a
  // FOOD_EAT_N run off WORM_ITEM0 would walk straight into the next creature's frames.
  function vitFoods() {
    const k9 = MEAT_IT + '/' + APPLE_IT + '/' + ORANGE_IT + '/' + WORM_ITEM0;
    if (vitFoodKey !== k9) {
      vitFoodKey = k9;
      for (const k in VIT_FOODS) delete VIT_FOODS[k];
      if (MEAT_IT) VIT_FOODS[MEAT_IT] = { hp: 1, strip: true };
      if (APPLE_IT) VIT_FOODS[APPLE_IT] = { hp: 1, strip: true };
      if (ORANGE_IT) VIT_FOODS[ORANGE_IT] = { hp: 1, strip: true };
      if (WORM_ITEM0) VIT_FOODS[WORM_ITEM0] = { hp: 1, strip: !!WORM_EAT0, eat: WORM_EAT0 };   // `eat` = where the carved run STARTS when it is not the held id itself. Every other food is its own strip head, so the field is absent for them and the animation falls back to the held id   // ── THE WORM (user 2026-08-18) ── one health point, the same as everything else on this table
    }
    return VIT_FOODS;
  }
  const vitFoodOf = (it) => (it && vitFoods()[it]) || null;

  // hurtT is the only thing left with a clock: it rides 1 -> 0 over 0.55 s and drives the hit KICK — the swell the
  // floating heart row gives on a blow, and the spike the pixelation paints on top of the standing level. It is an
  // event, not a state, which is exactly why it is separate from hp.
  // ── HUNGER, BACK, AND ON THE SAME FIVE-POINT SCALE AS HEALTH (user 2026-08-19: "can you re-introduce the
  // hunger mechanics ... it follows the hunger system of minecraft. so there will be 5 hunger points like there
  // are 5 health points") ── the 20-point FoodStats port this file used to be was DELETED on 2026-08-17, not
  // flagged off, so this is a rebuild from that history against the bar the game actually has now.
  // WHAT CAME BACK: the exhaustion accumulator and the per-metre charge that fills it, the drain, and
  // starvation. WHAT DID NOT: saturation, the two regeneration branches, and the 20 Hz fixed step — the first
  // was a hidden second bar, and the other two only existed to move health on a clock, which is the one thing
  // the user ruled out ("health doesnt replenish on its own"). That ruling still stands, so eating heals
  // DIRECTLY here as it has since, and hunger is what decides how often you may.
  // EXH_STEP IS 8x MINECRAFT'S. Every other constant below is Minecraft's own number, charged per metre, and
  // they are worth keeping because they are a tuned curve. But one drain step there spends 1 of 20 points and
  // here it would spend 1 of 5 — four times the bar for the same walk, hence a 4x step. Scaling the STEP rather
  // than the nine charges keeps the authored ratios between sprinting, swimming and jumping intact and moves
  // only the pace, which is the same reasoning VIT_DMG_PER_POINT uses to leave nine hazard call sites alone.
  // ── AND THEN DOUBLED AGAIN (user 2026-08-19: "double the length of the time it takes for the player to get
  // hungry and lose the first point") ── 16 -> 32, which is the whole change: this is the ONE number every
  // route to a lost point divides by, so a doubling here doubles the sprint distance, the idle clock, the swim,
  // the jumps, the swings and the digs together and cannot put them out of step with each other.
  // NOTE it doubles EVERY point, not only the first — the bar has no per-point pacing to single one out. If
  // what is wanted is specifically a grace period before the visible bar moves at all, that is Minecraft's
  // SATURATION, a hidden pool spent ahead of hunger; it was deliberately deleted (see the block above) and
  // would come back as a second bar rather than as a number here.
  const VIT_FOOD_MAX = 5;
  const EXH_STEP = 32.0;                              // 4.0 x 4 x 2 — one point of a FIVE-point bar, at half pace
  const EXH_WALK = 0.0, EXH_SPRINT = 0.1, EXH_SWIM = 0.01;   // per metre. Walking is FREE, exactly as it was before the delete: a stroll that starves you turns exploring into a chore
  const EXH_JUMP = 0.05, EXH_SPRINT_JUMP = 0.2, EXH_IDLE = 0.02;
  const EXH_HURT = 0.1, EXH_ATTACK = 0.1, EXH_MINE = 0.005;
  // ── STARVATION ── the only thing that moves health on a clock, and it is a hazard rather than a mechanic:
  // it fires ONLY at an empty bar, which the gold pixels have been telling you about for four whole points.
  // It goes through vitHurt with bypass, so it charges no exhaustion of its own — that is what stopped the
  // original from feeding itself into a spiral — and it can kill, because this game has exactly one death
  // threshold and a second floor here would be a hidden rule the screen does not show.
  const STARVE_SECS = 4.0;
  const VIT = { hp: VIT_HP_MAX, food: VIT_FOOD_MAX, exh: 0, starveT: 0, lx: 0, lz: 0, wasAir: false, hurtT: 0, onDeath: null };

  function vitReset() { VIT.hp = VIT_HP_MAX; VIT.food = VIT_FOOD_MAX; VIT.exh = 0; VIT.starveT = 0;
    VIT.lx = P.x; VIT.lz = P.z; VIT.wasAir = false; VIT.hurtT = 0; }   // lx/lz seeded from the CURRENT position: left at 0 the first tick charges a walk of ~150,000 voxels and empties the bar before the player has moved

  // ── NOTHING GATES SPRINTING ANY MORE ── this was Minecraft's "no sprinting at hunger 6 or below", and tick-body
  // stopped consulting it on 2026-08-16 because a hidden rule that disables a movement key reads as a broken key
  // ("the sprint button stop working randomly"). With hunger deleted there is no number left that could gate it even
  // in principle, so sprinting is now gated by exactly two things and both are visible to the player: the sprint key,
  // and not being crouched. The predicate survives only because main/debug-api.js reports it; it is a constant true
  // and no caller branches on it.
  const vitSprintOK = () => true;
  // ── SPENDING THE BAR ── one place, so every charge below goes through the same clamp and the same drain.
  // The while loop (not an if) matters at low frame rates: a single tick can carry more than one step's worth
  // of exhaustion, and an `if` would silently discard the remainder and make the drain frame-rate dependent.
  const vitExhaust = (e) => {
    if (!(e > 0) || VIT.hp <= 0) return;
    VIT.exh = Math.min(VIT.exh + e, EXH_STEP * 4);   // capped: a long fly/idle stretch must not bank a debt that empties the bar the instant it lands
    let g = 0;
    while (VIT.exh >= EXH_STEP && g++ < 8) { VIT.exh -= EXH_STEP; if (VIT.food > 0) { VIT.food--; vitFoodFx(); } }   // the burst fires HERE, at the one place a point is actually spent, so every route into the drain throws it and none of them has to remember to
  };
  // Swinging and digging used to charge exhaustion, which is how an active player got hungry. There is no exhaustion
  // to charge. They stay as no-ops because sim/life/reactions.js and main/tick-camera.js call them on every landed
  // blow and every chop, and a missing name there is a ReferenceError mid-swing rather than a compile error.
  const vitOnAttack = () => vitExhaust(EXH_ATTACK);   // live again — swinging and digging are how an active player gets hungry
  const vitOnMine = () => vitExhaust(EXH_MINE);

  // `bypass` was "damage armour could never have stopped" — Minecraft charges no exhaustion for drowning, falling or
  // lava, which is what stopped starvation feeding itself into a death spiral. There is no exhaustion and no
  // starvation, so it now reads as nothing at all. The parameter is kept because five call sites pass it and because
  // it still documents, at those call sites, which hazards are unblockable.
  function vitHurt(amount, why, bypass) {
    if (amount <= 0 || VIT.hp <= 0) return;
    VIT.hp = Math.max(0, VIT.hp - Math.max(1, Math.ceil(amount / VIT_DMG_PER_POINT)));
    if (!bypass) vitExhaust(EXH_HURT);                // `bypass` is back to meaning what it always meant: damage that charges no exhaustion, which is what stops starvation feeding itself
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
  // ── AND HOW GOLD THE SCREEN IS ── the same subtraction on the other bar, so BLIT can draw one effect twice
  // in two colours instead of carrying two ideas of "how far gone am I". 0 at a full stomach, 4 at an empty
  // one — and 4 is where starvation starts, so the gold reaching its widest IS the warning.
  const vitGoldLevel = () => Math.max(0, Math.min(4, VIT_FOOD_MAX - VIT.food));

  // ── THE HURT SPURT ── literally the call a struck creature makes, aimed at the player instead:
  // spawnHitSparks throws the four red HITRED_IT voxels the blood burst uses, so taking a hit and landing one
  // spit the same 10 cm voxels. Placed in FRONT of and below the eye — at the camera itself all four spawn
  // inside the near plane and are never seen.
  // ── ONE BURST, TWO COLOURS (user 2026-08-19: gold voxels for hunger, "render the same except gold color of
  // course") ── `gold` is the ONLY thing that differs, so it is an argument rather than a second copy of this
  // function: every number below was tuned by eye (the 2.2 forward, the -3.2 drop, the 0.55 fan, the 26..48
  // outward speed) and a duplicate would drift the moment either was touched again. The spark band is slots
  // 0..3, which both bursts want in full — losing a health point and a hunger point in the same frame is rare
  // and momentary, and the alternative is a burst of two flecks, which reads as a glitch rather than as a hit.
  function vitBodyBurst(gold) {
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
        // ── AND THEY CARRY THE PLAYER'S OWN MOTION (user 2026-08-19: "the player is moving forward and when the
        // gold voxels come out, theyre coming out behind the player") ── the burst was authored in WORLD space
        // with no inherited velocity, so a spark left the chest and then simply stopped travelling with the body
        // it came off. That is wrong for any moving player and it is worst for THIS burst, for two compounding
        // reasons: hunger is spent by SPRINTING, so the player is never standing still when a gold point drops,
        // and the gold flecks were given nearly three times the blood burst's life to make them readable — which
        // is three times as long to be outrun. At a ~60 vox/s sprint over 1.3 s that is 78 voxels of separation:
        // the flecks were behind the camera before the eye ever found them, which is the whole of the report.
        // P.hvx/P.hvz are voxels per SECOND (main/tick-body.js integrates them as `P.x += P.hvx * dt`), the same
        // units a spark's own vx/vz are read in.
        // ── AND IT IS A CARRY THAT DIES, NOT A VELOCITY (user 2026-08-19: "the momentum needs to drop off
        // sharply. it needs to look like when the player is standing still") ── the first cut simply added
        // P.hvx to vx, and a spark's motion is LINEAR (`x + vx * t`, main/tick-emit.js), so the inherited speed
        // never decayed: the flecks kept the player's 60 vox/s for their whole 1.3 s and shot off across the
        // clearing. Both readings are wrong in the same way — one leaves them behind the player, the other
        // throws them ahead of it — because a fleck is not a thing that keeps moving at your speed OR at zero.
        // It leaves your body with your momentum and loses it to the air almost at once.
        // So the inherited part is stored SEPARATELY as ivx/ivz and integrated with an exponential decay in
        // tick-emit, which is what "drops off sharply" means as arithmetic. It cannot be folded into vx: vx is
        // the burst's OWN velocity and must not decay, or the flecks would stop dead in mid-air.
        // HORIZONTAL ONLY. Inheriting P.vy as well would be equally correct, but a burst thrown while falling
        // would then be flung straight down at terminal velocity, and nothing about the report is vertical.
        // BOTH COLOURS, not just the gold: it is one burst in two colours by construction (the user's own
        // "render the same except gold"), and the blood has exactly the same flaw — it is merely hidden by a
        // 0.4 s life, which still drifts ~24 voxels at a sprint.
        vx: (fx * 0.85 + rx * side * 0.9) * out, vy: 10 + Math.random() * 16, vz: (fz * 0.85 + rz * side * 0.9) * out,
        ivx: P.hvx, ivz: P.hvz,                          // …CARRIED, not added to vx: see the note above
        born: performance.now(), life: 0.4 + Math.random() * 0.3, ph: Math.random() * 6.283, smoke: false, red: !gold, gold: !!gold };
      // ── ONE BURST, ONE SET OF PHYSICS (user 2026-08-19: "they need to have the same momentum as they did
      // before any of these changes took place. just make it come out of the player properly") ── an earlier cut
      // gave the gold flecks a sixth of the gravity and nearly three times the life, on the theory that a hunger
      // point has no sound and no screen kick to announce it and so needed longer to be read. It bought that
      // legibility with the one property that actually mattered: a fleck that falls slowly does not spurt off a
      // body, it FLOATS, and the burst stopped reading as something coming off the player at all.
      // Gold and red are physically identical again — same life, same gravity, same speeds, same four voxels —
      // and the only differences left are the colour and the sound/screen kick the red one also fires. That was
      // the original brief ("render the same except gold color of course") and it was right: what made the gold
      // hard to see was never the physics, it was being left behind a moving player, which ivx/ivz above fixes.
    }
    // ── AND THE SAME VOICE (user 2026-08-16: "play the same sound that plays when the player hits life") ──
    // playLifeHit is the pool every blow that lands on a living thing already uses, and it attenuates with
    // distance from the ear. Passing the player's OWN position puts them at zero range, so a hit taken is the
    // same sound at full strength as a hit landed — which is what "the same sound" has to mean here.
    if (gold) return;                               // the two lines below are the HIT's, not the bar's: a hunger point is a slow drain, so it throws the flecks and nothing else — no blow landed, so there is no blow to hear and no screen to kick
    playLifeHit(P.x, smoothEye, P.z);   // 8, not 14 (user 2026-08-16: "bring the blood voxels closer into the player"). It started at 3, where perspective blew each 10 cm voxel into a metre-wide slab across the view; 14 read as distant flecks. 8 keeps them clearly the player's own without filling the screen
    hurtScreen();
  }
  const vitHurtFx = () => vitBodyBurst(false);        // a health point: flecks + the sound + the screen kick
  const vitFoodFx = () => vitBodyBurst(true);         // a hunger point: the same flecks, in gold, and nothing else

  // ── ONE BITE, ONE POINT ── and the refusal is a FULL-HEALTH test, not the full-hunger test it replaced. That test
  // was the bug: with hunger frozen at max the bar was always full, so `tryEat` refused every bite and a hurt player
  // could not eat at all. The rule it was written for still holds and is the reason a test belongs here — a bite that
  // would heal nothing must not consume the item — it just has to ask about the bar the food actually fills.
  // ── A BITE FEEDS BOTH BARS ── and the refusal is now "there is nothing to gain", not "health is full".
  // Minecraft refuses on a full HUNGER bar alone, which here would lock a hurt player out of healing until they
  // had run around to burn a point off a bar they cannot spend on purpose — a rule the screen never explains.
  // Refusing only when BOTH are full keeps the thing that rule is actually for (no food spent on nothing) and
  // drops the part that only frustrates. Hunger is still what paces healing, because it is what empties.
  function vitEat(it) {
    const f = vitFoodOf(it);
    if (!f || (VIT.hp >= VIT_HP_MAX && VIT.food >= VIT_FOOD_MAX)) return false;
    VIT.hp = Math.min(VIT_HP_MAX, VIT.hp + f.hp);
    VIT.food = Math.min(VIT_FOOD_MAX, VIT.food + f.hp);   // one point of each per item, the user's own flat number for health and the same for hunger
    VIT.starveT = 0;                                      // eating clears the starvation clock outright, so a bite is never followed by the tick it had already earned
    return true;
  }

  // ── AND THAT IS THE WHOLE TICK ── it used to spend exhaustion, run one of three regen/starve branches on a 20 Hz
  // fixed step, charge exhaustion per metre walked, and decay a hit run. Health no longer changes with time in ANY
  // direction: the only two things that move it are `vitHurt` and `vitEat`. What is left is the hit kick riding down
  // over 0.55 s, which is an animation, not a mechanic. Still called unconditionally from tick-body (it once sat
  // inside the `if (P.fly) … else` movement branch and silently stopped the moment fly mode engaged).
  function vitTick(dt) {
    if (VIT.hurtT > 0) VIT.hurtT = Math.max(0, VIT.hurtT - dt / 0.55);
    if (VIT.hp <= 0) return;
    // ── EXHAUSTION FROM MOVEMENT ── charged per METRE off the REAL position delta rather than off the input
    // keys, so being shoved, sliding or swimming all count honestly. A voxel is 10 cm, hence the 0.1. The
    // dist < 3 guard drops teleports (the debug tp, a respawn, a world wrap) — otherwise one jump across the
    // map charges thousands of metres and empties the bar in a frame.
    // FLYING IS FREE, as it was before the delete: it is a debug mode, not a way to play.
    if (!P.fly) {
      const dx = P.x - VIT.lx, dz = P.z - VIT.lz;
      const dist = Math.sqrt(dx * dx + dz * dz) * 0.1;
      if (dist > 0 && dist < 3) {
        const swim = P.y < WL;
        const sprint = (dist / Math.max(dt, 1e-4)) > 0.62;   // metres/s, not the sprint KEY: what costs is the ground actually covered
        vitExhaust(dist * (swim ? EXH_SWIM : sprint ? EXH_SPRINT : EXH_WALK));
      }
      if (!P.onGround && !VIT.wasAir) vitExhaust(P.sprintJump ? EXH_SPRINT_JUMP : EXH_JUMP);   // the takeoff EDGE, so one jump is charged once rather than every airborne frame
      VIT.wasAir = !P.onGround;
      vitExhaust(EXH_IDLE * dt);
    }
    VIT.lx = P.x; VIT.lz = P.z;
    // ── STARVATION ── the clock only runs on an empty bar and is reset by any bite. Real seconds, not a fixed
    // 20 Hz step: nothing else in this file runs on a tick counter any more, and one hazard does not justify
    // bringing the accumulator back.
    if (VIT.food <= 0) {
      VIT.starveT += dt;
      if (VIT.starveT >= STARVE_SECS) { VIT.starveT = 0; vitFoodFx(); vitHurt(VIT_DMG_PER_POINT, 'you starved', true); }   // …and it throws the GOLD burst as well as the red one vitHurt fires: starving is the one hunger event that costs health, so both bars are being hit and both should say so. Fired FIRST, so the red burst lands on top of it rather than being overwritten by it
    } else VIT.starveT = 0;
  }
