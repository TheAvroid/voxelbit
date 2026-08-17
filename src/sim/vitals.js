  // @module - player vitals: hearts, hunger, saturation and exhaustion, ported from Minecraft's FoodStats
  // @exports VIT, VIT_HP_MAX, VIT_FOOD_MAX, vitFoods, vitTick, vitHurt, vitEat, vitReset, vitSprintOK, vitOnAttack, vitOnMine, vitRedLevel
  // ── WHY THIS EXISTS ── until now `die(why)` WAS the whole of mortality: every hazard killed outright and the
  // player had no health at all (see the comment at the cobra contact test). Hearts only mean something once the
  // hazards stop one-shotting you, so this module owns both halves — the vitals, and the `vitHurt` entry point
  // the hazards now call instead of `die`.
  //
  // ── RESOLUTION ── 20 points, Minecraft's own scale, because every constant below is quoted against it.
  // These were drawn as five voxels a side until the user removed the UI; the numbers stayed on the 20-point
  // bar then and still do, so the mechanics remain the ones they were copied from.
  const VIT_HP_MAX = 20, VIT_FOOD_MAX = 20;
  // Minecraft is a 20 tps simulation and every constant below is a TICK COUNT, so the vitals run on their own
  // fixed 20 Hz step rather than on the render frame. Ticking this per frame would make hunger drain ~3x faster
  // at 60 fps than at 20, and faster still on a 240 Hz monitor — the mechanics would depend on the hardware.
  const VIT_TPS = 20, VIT_DTT = 1 / VIT_TPS;

  // ── HUNGER IS OFF (user 2026-08-16: "disable the hunger mechanics, but keep the code for later") ── ONE
  // switch, and everything below it is intact: the exhaustion accumulator still totals up, eating still works,
  // the food table still resolves, and every constant keeps its measured Minecraft value. What this skips is
  // the SPEND — the step that turns exhaustion into lost saturation and lost hunger. With nothing draining,
  // `food` stays at max, which means the starvation branch can never be reached (it needs food 0) and the
  // regen branches stay satisfied (they need food >= 18), so health still recovers. Flip to true to bring the
  // whole system back; nothing else needs changing. NOTE if it does come back, hunger needs to be VISIBLE
  // again before anything is gated on it — a hidden hunger gate on the sprint key read as a random failure.
  const VIT_HUNGER_ON = false;

  // ── EXHAUSTION ── the hidden accumulator that actually drives hunger. Actions add to it; when it passes
  // EXH_STEP, ONE point comes off saturation, or off hunger when saturation is already empty. That ordering is
  // the entire reason saturation is invisible and still matters.
  // NOTE the two details the wiki gets wrong and the source does not: the test is STRICTLY greater-than, and it
  // SUBTRACTS 4 rather than resetting to 0, so the remainder carries. And only ONE unit is spent per tick, so a
  // big spike drains at most one point per tick instead of cascading.
  const EXH_STEP = 4.0, EXH_CAP = 40.0;
  const EXH_SPRINT = 0.1, EXH_SWIM = 0.01, EXH_JUMP = 0.05, EXH_SPRINT_JUMP = 0.2;
  const EXH_HURT = 0.1, EXH_ATTACK = 0.1, EXH_MINE = 0.005, EXH_REGEN = 6.0;
  // WALKING IS FREE in modern Minecraft — the source literally reads addExhaustion(0.0F * distance), a vestige
  // of the value removed around 1.9. Only sprinting, swimming and jumping cost anything.
  const EXH_WALK = 0.0;
  // ── …WHICH LEAVES A STANDING PLAYER'S HUNGER FROZEN FOREVER ── and the user asked for the bar to "slowly
  // deplete". This trickle is therefore a DELIBERATE departure from Minecraft, and the only one in this file.
  // At 0.02/s it costs a hunger point roughly every 200 s and empties a full bar in a little under an hour.
  const EXH_IDLE = 0.02;

  // ── REGEN ── two branches, and they are not one rule with different numbers. Saturation regen needs a FULL
  // bar plus saturation to spend and heals up to 2 HP/s; normal regen only needs 18 and heals 1 HP per 4 s.
  // Both charge exhaustion, which is what stops regeneration being free. The wiki claims non-zero saturation
  // alone also enables normal regen — the decompiled source disagrees, and this follows the source.
  const REGEN_FOOD = 18, REGEN_TICKS = 80, SATREGEN_TICKS = 10, STARVE_TICKS = 80;
  // Starvation will not finish you off: this is Minecraft's NORMAL difficulty rule, damage only while above 1.
  const STARVE_FLOOR = 1;
  // ── FIVE HITS IN SUCCESSION KILL, AND TEN QUIET SECONDS UNDO IT (user 2026-08-16) ── a COUNT, not a health
  // total, because with no UI the player cannot read a bar: what they can feel is "I keep getting hit". The
  // counter is what actually kills — five stings end the run whatever each one took off — and any ten seconds
  // without damage opens regeneration and starts the run draining. Deliberately behind the scenes: no display,
  // the only signals are the hurt flash, the blood voxels and eventually the game-over screen.
  const VIT_HITS_FATAL = 5, VIT_CALM = 10.0;
  // ── AND IT DRAINS ONE HIT AT A TIME (user 2026-08-17: "let the player heal in steps … it should heal in the
  // same amount of steps as the player takes damage") ── the run used to be zeroed in a single assignment, so
  // four hits of red left the screen in ONE frame while the hp half of vitRedLevel was still stepping. 2.4 s is
  // the rate the other half already moves at: saturation regen heals 0.8333 hp per 0.5 s, so a 4-hp heart — one
  // shade of the vignette — takes exactly 2.4 s to come back. Both halves of that max() now walk down together
  // instead of one of them snapping past the other. Nothing decays inside the calm window, which is what keeps
  // five hits in succession fatal: a hit zeroes calm, so a run of them never gets a step back.
  const VIT_HIT_DECAY = 2.4;
  const SPRINT_FOOD = 6;

  // ── FOOD ── { h: hunger restored, s: saturation modifier }. Saturation gained is h * s * 2, capped at the
  // hunger level AFTER eating. Raw meat carries Minecraft's raw-beef numbers; it is the only food the game
  // currently gives you (the apple frames under assets/food/apple are modelled but not yet an item).
  //
  // BOUND LAZILY, and that is not a style choice. The item ids are assigned by an ASYNC asset load, so writing
  // this table from held-items.js — fragment 22, against this module's fragment 56 — threw "Cannot access
  // VIT_FOODS before initialization" and left the game on a black screen. Registering from the other side is a
  // TDZ hazard in one direction and a table keyed on id 0 in the other. Rebuilding on first use after the id
  // changes is immune to both, and costs one integer compare per lookup.
  const VIT_FOODS = {};
  let vitFoodKey = -1;
  function vitFoods() {
    if (vitFoodKey !== MEAT_IT) {
      vitFoodKey = MEAT_IT;
      for (const k in VIT_FOODS) delete VIT_FOODS[k];
      if (MEAT_IT) VIT_FOODS[MEAT_IT] = { h: 3, s: 0.3 };
    }
    return VIT_FOODS;
  }
  const vitFoodOf = (it) => (it && vitFoods()[it]) || null;

  const VIT = { hp: VIT_HP_MAX, food: VIT_FOOD_MAX, sat: 5, exh: 0, timer: 0, acc: 0,
    hurtT: 0, hits: 0, hitT: 0, calm: VIT_CALM, lx: 0, lz: 0, wasAir: false, onDeath: null, started: false };

  function vitReset() {
    VIT.hp = VIT_HP_MAX; VIT.food = VIT_FOOD_MAX; VIT.sat = 5; VIT.exh = 0;
    VIT.timer = 0; VIT.acc = 0; VIT.hurtT = 0; VIT.hits = 0; VIT.hitT = 0; VIT.calm = VIT_CALM;
    VIT.lx = P.x; VIT.lz = P.z; VIT.wasAir = false; VIT.started = true;
  }

  const vitExhaust = (e) => { VIT.exh = Math.min(VIT.exh + e, EXH_CAP); };
  const vitSprintOK = () => VIT.food > SPRINT_FOOD;
  // Swinging and digging cost exhaustion too — small per action, but they are why an active player gets hungry
  // and an idle one barely does. Wrapped as named calls so the constants stay in here with the rest of them.
  const vitOnAttack = () => vitExhaust(EXH_ATTACK);
  const vitOnMine = () => vitExhaust(EXH_MINE);

  // `bypass` = damage armour could never have stopped (starving, drowning, falling, lava). Minecraft charges no
  // exhaustion for those, which is what stops starvation from feeding itself into a death spiral.
  function vitHurt(amount, why, bypass) {
    if (amount <= 0 || VIT.hp <= 0) return;
    VIT.hp = Math.max(0, VIT.hp - amount);
    if (!bypass) vitExhaust(EXH_HURT);
    VIT.hurtT = 1;
    VIT.hits++; VIT.calm = 0;                          // …and the run of hits grows; any quiet spell resets it in vitTick
    vitHurtFx();
    if ((VIT.hp <= 0 || VIT.hits >= VIT_HITS_FATAL) && VIT.onDeath) VIT.onDeath(why || 'you died');
  }

  // ── HOW RED THE SCREEN IS (user 2026-08-16: "full hearts is no shade, then heart 0 is the gameover
  // screen") ── 0 at full health and 1..4 as the hearts go, which is exactly four shades because there are five
  // hearts and the fifth step is death, not a colour. Death has TWO paths — hp reaching zero, and five hits in
  // succession — and a run of quick hits can kill with most of the bar still showing, so the level is the WORSE
  // of the two readings. Otherwise a player two hits from dying by the hit-run could be looking at a clear
  // screen. Regeneration walks it back down on its own: after the calm window the hit run drains a step at a time
  // and hp climbing raises the heart count, so recovering visibly steps the red off without anything here
  // having to fade it — and both terms step at the same 2.4 s, so neither one skips shades the other is showing.
  const vitRedLevel = () => {
    const hearts = Math.ceil(VIT.hp / (VIT_HP_MAX / 5));
    return Math.max(0, Math.min(4, Math.max(5 - hearts, VIT.hits | 0)));
  };

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

  function vitEat(it) {
    const f = vitFoodOf(it);
    if (!f || VIT.food >= VIT_FOOD_MAX) return false;
    VIT.food = Math.min(VIT_FOOD_MAX, VIT.food + f.h);
    // capped at the hunger level AFTER eating, not at the food's own value, so a rich food eaten on a nearly
    // empty bar banks far less saturation than the same food eaten when nearly full.
    VIT.sat = Math.min(VIT.food, VIT.sat + f.h * f.s * 2);
    return true;
  }

  // ── ONE 20 Hz TICK ── the order here IS the mechanic. Exhaustion is spent first, then exactly one of the
  // regen/starve branches runs, and any branch that does not apply RESETS the timer, discarding partial
  // progress. Healing to full, or dropping to hunger 17, therefore throws away a part-finished regen cycle.
  function vitStep() {
    if (VIT_HUNGER_ON && VIT.exh > EXH_STEP) {
      VIT.exh -= EXH_STEP;
      if (VIT.sat > 0) VIT.sat = Math.max(0, VIT.sat - 1);
      else VIT.food = Math.max(0, VIT.food - 1);
    }
    const hurt = VIT.hp > 0 && VIT.hp < VIT_HP_MAX && VIT.calm >= VIT_CALM;   // no healing mid-fight: ten quiet seconds first, the same window that clears the hit run
    if (VIT.sat > 0 && hurt && (!VIT_HUNGER_ON || VIT.food >= VIT_FOOD_MAX)) {
      if (++VIT.timer >= SATREGEN_TICKS) {
        // heals a FRACTION when saturation is low rather than a flat point: min(sat, 6) / 6 HP, charged as f
        // exhaustion. That keeps the identity 1 HP = 6 exhaustion = 1.5 saturation true at every level.
        const f = Math.min(VIT.sat, 6.0);
        VIT.hp = Math.min(VIT_HP_MAX, VIT.hp + f / 6.0);
        vitExhaust(f);
        VIT.timer = 0;
      }
    } else if (hurt && (!VIT_HUNGER_ON || VIT.food >= REGEN_FOOD)) {   // with hunger OFF, regen stops consulting it entirely — otherwise 'disabled' still left healing hostage to a number nothing maintains
      if (++VIT.timer >= REGEN_TICKS) { VIT.hp = Math.min(VIT_HP_MAX, VIT.hp + 1); vitExhaust(EXH_REGEN); VIT.timer = 0; }
    } else if (VIT_HUNGER_ON && VIT.food <= 0) {   // …and starvation is off with it: gating only the SPEND left this branch live, so anything that set food to 0 (a debug tap, a future feature) still bit for 1 hp every 4 s
      if (++VIT.timer >= STARVE_TICKS) { if (VIT.hp > STARVE_FLOOR) vitHurt(1, 'you starved', true); VIT.timer = 0; }
    } else VIT.timer = 0;
  }

  function vitTick(dt) {
    if (!VIT.started) { vitReset(); return; }
    if (VIT.hurtT > 0) VIT.hurtT = Math.max(0, VIT.hurtT - dt / 0.55);
    if (VIT.hp <= 0) return;
    // the calm clock: it only ever grows here, and vitHurt is the one thing that zeroes it
    VIT.calm += dt;
    // …and once it is up the run comes off one hit per VIT_HIT_DECAY rather than all at once. A fresh hit puts
    // calm back to 0 and falls through the first branch, discarding the part-finished step — the same thing
    // vitStep does to a part-finished regen cycle, and the reason a decay can never soften a run of hits.
    if (VIT.calm < VIT_CALM) VIT.hitT = 0;
    else if (VIT.hits > 0 && (VIT.hitT += dt) >= VIT_HIT_DECAY) { VIT.hits--; VIT.hitT = 0; }

    // ── EXHAUSTION FROM MOVEMENT ── charged per METRE, as Minecraft does, so a stroll and a sprint over the
    // same ground do not cost the same. Distance comes from the real position delta rather than the input keys,
    // so being shoved, sliding or swimming all count honestly. A voxel is 10 cm, hence the 0.1.
    if (!P.fly) {
      const dx = P.x - VIT.lx, dz = P.z - VIT.lz;
      const dist = Math.sqrt(dx * dx + dz * dz) * 0.1;
      if (dist > 0 && dist < 3) {
        const swim = P.y < WL;
        const sprint = (dist / Math.max(dt, 1e-4)) > 0.62;
        vitExhaust(dist * (swim ? EXH_SWIM : sprint ? EXH_SPRINT : EXH_WALK));
      }
      if (!P.onGround && !VIT.wasAir) vitExhaust(P.sprintJump ? EXH_SPRINT_JUMP : EXH_JUMP);
      VIT.wasAir = !P.onGround;
      vitExhaust(EXH_IDLE * dt);
    }
    VIT.lx = P.x; VIT.lz = P.z;

    VIT.acc += dt;
    let guard = 0;
    while (VIT.acc >= VIT_DTT && guard++ < 8) { VIT.acc -= VIT_DTT; vitStep(); }
    if (guard >= 8) VIT.acc = 0;
  }
