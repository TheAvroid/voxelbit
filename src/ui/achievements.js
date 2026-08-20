  // ── DUAL-WIELD rock clash ── shift + left click with a rock in EACH hand: both rocks accelerate to screen centre,
  // collide with a spark burst, and the first collision unlocks the "sharp edge" discovery achievement.
  let clashT0 = -1e9, clashSparked = true;
  let vbAch = {};                                      // achievements RESET on refresh (was localStorage vb_ach — user asked for per-session)
  const achSnd = regSnd(new Audio('sound/high_score.mp4'), 0.09);   // achievement jingle — 0.5 → 0.25 (2026-07-16), then 40% off (user 2026-07-20): 0.25 → 0.15, then 40% again (user 2026-08-07): 0.15 → 0.09
  const dualRocks = () => !dead && !grabAnim && !!(slots[selSlot] && slots[selSlot].it === 2 && slots[selSlot].n >= 2);
  const achEl = $('achv'), achNameEl = $('achName'), clashHintEl = $('clashHint');
  let achHideT = 0;
  // 24 slots: 20 as before, plus a 4-slot POLLEN band at the top (user 2026-08-18 — the bees). The tear band's
  // own note says four is already more than crying can use and that tears were the band with slack; pollen has
  // no such slack, because several bees leave flowers at once and each trails repeatedly. Sharing would have had
  // ducklings and bees cutting each other's effects short, which is the exact bug sim/particles.js records being
  // fixed twice. THREE places know this number: the emit loop in main/tick-emit.js, the drop-slot reserve in
  // main/tick-life.js, and the bands in sim/particles.js — all three moved together.
  const sparks3d = [null, null, null, null, null, null, null, null, null, null, null, null,
                    null, null, null, null, null, null, null, null, null, null, null, null,
                    null, null, null, null, null, null, null, null, null, null, null, null,
                    null, null, null, null,
                    null, null, null, null, null, null, null, null,
                    null, null, null, null, null, null, null, null];   // +16 for FALLING PETALS (user 2026-08-18), then +16 MORE (user 2026-08-19: "double the rate of the leafs falling") — the band IS the population, so the pool is where a rate change lands. main/tick-emit.js and main/tick-life.js both read sparks3d.length rather than a literal, so those two follow this on their own — a band rather than a share, same reasoning as the pollen's: a petal lives ~10 s where a droplet lives 0.9, so sharing would have had petals squatting on every splash slot in the blossom   // 20 death-burst slots (drops uniform 5-24): CLASH uses the first 4 as embers; a DEATH fills 4 sparks + 16 INDIVIDUAL smoke voxels (10 cm each → the plume needs more of them to read as dense) — world-space particles
  // ── ONE DISCOVERY, THEN ANY NUMBER OF THEM (user 2026-08-07) ── the banner used to be hard-wired to
  // "sharp edge" in the markup, so a second discovery starts by splitting the two things it needs: `key` is
  // what makes it fire exactly once, `label` is what the player reads. Both stay session-only, like the
  // first. Earning one while another is still on screen retitles the banner and restarts its five seconds —
  // the real events are seconds apart in play, and a proper queue would hold the second one back past the
  // moment that earned it, which is the one thing a discovery must not do.
  function unlockAch(key, label) {
    if (vbAch[key]) return;
    vbAch[key] = true;                                 // session-only — refresh resets the discoveries (and brings the hints back)
    achNameEl.textContent = label;
    try { achSnd.currentTime = 0; const p = achSnd.play(); if (p) p.catch(() => {}); } catch (e) {}
    achEl.style.opacity = 1; achHideT = performance.now() + 5200;
  }
  function unlockSharpEdge() { unlockAch('sharpEdge', 'sharp edge'); }

  // ══ THE STONE AGE BENCH (user 2026-08-19) ══ "the exact same mechanics as combining 2 rocks together, but now
  // allow the player to connect a rock and a stick together. it doesnt matter which item is in which hand."
  // The clash above is the model and this is deliberately its sibling: same shift+click gesture, same 700 ms
  // re-arm, same discovery banner. What differs is that a clash PRODUCES one fixed thing (the knife) while this
  // opens a CHOICE, so it has a state a clash does not: a bench that stays open until the player commits.
  //
  // WHY THE PAIR IS NOT `slots[selSlot].n >= 2`. dualRocks() reads two rocks out of ONE slot — the left hand is
  // the same stack drawn twice, which is why it can only ever dual-wield identical items. A rock and a stick are
  // two different items and therefore two different slots, so the pair is "the selected slot holds one half and
  // some other slot holds the other". Order does not matter, which is the user's own rule, and it falls out of
  // testing the pair as a SET rather than as (left, right).
  const CRAFT_A = 2, CRAFT_B = 3;                      // rock, stick — the two halves, by item id
  const craftPair = () => {                            // → { rock: slotIdx, stick: slotIdx } or null
    if (dead || grabAnim) return null;
    const sel = slots[selSlot]; if (!sel) return null;
    if (sel.it !== CRAFT_A && sel.it !== CRAFT_B) return null;   // one half must be IN HAND — this is a two-handed gesture, not an inventory recipe
    const want = sel.it === CRAFT_A ? CRAFT_B : CRAFT_A;
    for (let i = 0; i < slots.length; i++) if (i !== selSlot && slots[i] && slots[i].it === want)
      return sel.it === CRAFT_A ? { rock: selSlot, stick: i } : { rock: i, stick: selSlot };
    return null;
  };
  // WHAT THE BENCH CAN MAKE. The axe is first because the user named it as the default, and the rest is every
  // stone tool the game already models. Three of them — hoe, knife, spear — were taken out of the starting
  // rotation earlier the same day, so this is now where they come from: the tools are FOUND by making them
  // instead of being issued, which is what the discovery is called after.
  // Built lazily and filtered: an id is 0 until its .vox has loaded, and a 0 would draw as an empty preview.
  // ── WHAT EACH TOOL COSTS (user 2026-08-19) ── sticks and rocks, counted across the WHOLE hotbar rather than
  // out of the two hands: the gesture is what opens the bench, but a three-rock axe cannot be paid for by two
  // hands holding one each. Keyed by item id and built lazily, because every id but the axe's is 0 until its
  // .vox has loaded.
  //   axe   2 sticks 3 rocks     shovel 2 sticks 1 rock
  //   pick  2 sticks 3 rocks     hoe    2 sticks 2 rocks
  //   spear 3 sticks 1 rock
  // ── AND THE KNIFE IS NOT ON THIS BENCH ── it was, before the costs existed, and the user's list does not
  // include it. That is consistent rather than an omission: a knife is the one stone tool with a recipe of its
  // own already — CLASH TWO ROCKS (see the block at the top of this file) — so putting it here too would give
  // it two prices. Dropping it from the menu keeps one recipe per tool. Put `KNIFE_IT` back in the list below
  // and give it a cost to undo this.
  const craftCostFor = (it) => {
    if (it === 1 || (PICK_IT && it === PICK_IT)) return { stick: 2, rock: 3 };
    if (SHOVEL_IT && it === SHOVEL_IT) return { stick: 2, rock: 1 };
    if (HOE_IT && it === HOE_IT) return { stick: 2, rock: 2 };
    if (SPEAR_IT && it === SPEAR_IT) return { stick: 3, rock: 1 };
    return { stick: 1, rock: 1 };                      // anything added to the menu without a price still costs the pair that opened the bench, so it can never be free
  };
  const craftHave = () => { let rock = 0, stick = 0;   // …and what the player is actually carrying, across every slot
    for (let i = 0; i < slots.length; i++) { const s2 = slots[i]; if (!s2) continue;
      if (s2.it === CRAFT_A) rock += s2.n | 0; else if (s2.it === CRAFT_B) stick += s2.n | 0; }
    return { rock, stick }; };
  const craftAfford = (it) => { const c = craftCostFor(it), h = craftHave(); return h.rock >= c.rock && h.stick >= c.stick; };
  // Spend n of one item across however many slots hold it — a cost of 3 rocks may be a stack of 3, or three
  // separate slots, and the bench must not care which.
  const craftTake = (it, n) => { let left = n;
    for (let i = 0; i < slots.length && left > 0; i++) { const s2 = slots[i]; if (!s2 || s2.it !== it) continue;
      const take = Math.min(left, s2.n | 0); s2.n -= take; left -= take; if (s2.n <= 0) slots[i] = null; }
    return left === 0; };
  const craftMenu = () => [1, PICK_IT, SHOVEL_IT, HOE_IT, SPEAR_IT].filter(Boolean);
  // ── THE GESTURE HAS THE CLASH'S SHAPE, SO IT HAS THE CLASH'S PHASES (user 2026-08-19) ── t0 is when the
  // shift+click landed, and everything after it is read off that one clock, exactly as the rock clash reads
  // everything off clashT0:
  //   t0 .. t0+IMPACT   both hands drive to screen centre
  //   IMPACT            sparks, and the two halves are GONE — `lit` hides both hands from there on
  //   after            the preview hangs in the gap they left; cycling happens here
  //   Enter            `fly` starts, the preview glides to the right hand, and only when it ARRIVES is the
  //                    tool actually added — so the object you watched land is the object you now hold
  // `lit` is a latch rather than a time test because tick-camera has to spawn the sparks EXACTLY once, which
  // is the same reason the clash carries clashSparked beside clashT0.
  const CRAFT_IMPACT = 260, CRAFT_FLY = 300;         // ms: hands meeting, and the chosen tool's glide to the hand
  const CRAFT = { open: false, idx: 0, t0: 0, lit: false, fly: 0 };
  const craftK = (now) => {                          // 0..1, how far the two hands have closed on centre
    if (!CRAFT.open) return 0;
    const k = (now - CRAFT.t0) / CRAFT_IMPACT;
    return k <= 0 ? 0 : (k >= 1 ? 1 : k * k);        // accelerating in, like the clash's own drive
  };
  const craftOpen = () => {
    if (CRAFT.open || !craftPair()) return false;
    CRAFT.open = true; CRAFT.idx = 0; CRAFT.t0 = performance.now(); CRAFT.lit = false; CRAFT.fly = 0;
    return true;
  };
  const craftClose = () => { CRAFT.open = false; CRAFT.lit = false; CRAFT.fly = 0; };
  const craftCycle = (d) => { const m = craftMenu(); if (!CRAFT.open || !m.length) return;
    CRAFT.idx = ((CRAFT.idx + (d < 0 ? -1 : 1)) % m.length + m.length) % m.length; };   // wraps both ways, so the list has no ends to get stuck against
  const craftItem = () => { const m = craftMenu(); return m.length ? m[CRAFT.idx % m.length] : 0; };
  // ── COMMIT ── spends ONE of each half and puts the tool in the hand. Ordered so nothing is ever spent for
  // nothing: refuse first if there is no room, then take the materials, then hand over the tool.
  // ── ENTER STARTS THE GLIDE; ARRIVING IS WHAT COMMITS ── it used to spend and hand over in the same frame,
  // so the tool blinked from the middle of the screen into the fist. Splitting it means the flight cannot
  // desync from the result: nothing is spent until craftLand runs, and craftLand only runs when the preview
  // has actually reached the hand. Both refusals still happen up front, so a full hotbar never starts a
  // flight it cannot finish.
  const craftConfirm = () => {
    const p = craftPair(); const it = craftItem();
    if (!CRAFT.open || CRAFT.fly || !p || !it) { if (!p || !it) craftClose(); return false; }
    if (!craftAfford(it)) return false;                // not enough sticks or rocks for THIS tool: the bench stays open on it, so the player can cycle to one they can pay for
    if (!canAdd(it)) return false;                     // no slot free: the bench stays open rather than eating the materials
    CRAFT.fly = performance.now();
    return true;
  };
  const craftLand = () => {                            // the glide finished — spend the halves and hand the tool over
    const p = craftPair(); const it = craftItem();
    if (!p || !it) { craftClose(); return; }
    const c = craftCostFor(it);
    if (!craftAfford(it)) { craftClose(); return; }    // re-checked at the LANDING, not just at Enter: the glide is 300 ms and a stack can be dropped or thrown in that time
    craftTake(CRAFT_A, c.rock); craftTake(CRAFT_B, c.stick); slotTidy();
    const k = addItem(it); if (k >= 0) selSlot = k;     // …and it lands IN THE RIGHT HAND, selected, exactly as a crafted knife does
    craftClose();
    unlockAch('stoneAge', 'stone age');
  };
  // ── PROJECTILE (user 2026-08-07) ── the first shaft the player ever sends downrange, arrow OR spear.
  // It is armed inside launchThrown rather than at the two call sites because that is the single point where
  // a shot has actually COMMITTED: shootArrow and throwSpear can both bail before it (no item, dead, editor
  // open), and a discovery that fires on a swallowed shot is a discovery for pressing a button.
  function unlockProjectile() { unlockAch('projectile', 'projectile'); }
  function spawnSparks() {                             // 4 voxel sparks burst from where the rocks meet (the held anchor ~1.15 m ahead, slightly below eye line)
    const cpv = Math.cos(P.pitch), spv = Math.sin(P.pitch);
    const fwd3 = [Math.sin(P.yaw) * cpv, spv, Math.cos(P.yaw) * cpv];
    const right3 = [Math.cos(P.yaw), 0, -Math.sin(P.yaw)];
    const up3 = [right3[1] * fwd3[2] - right3[2] * fwd3[1], right3[2] * fwd3[0] - right3[0] * fwd3[2], right3[0] * fwd3[1] - right3[1] * fwd3[0]];
    const ox = P.x + fwd3[0] * 11.5 - up3[0] * 2.1, oy = smoothEye + fwd3[1] * 11.5 - up3[1] * 2.1, oz = P.z + fwd3[2] * 11.5 - up3[2] * 2.1;
    for (let i = 0; i < 4; i++) {
      const a = Math.random() * 6.283, sp3 = 12 + Math.random() * 22;
      sparks3d[i] = { x: ox, y: oy, z: oz, vx: Math.cos(a) * sp3, vy: 16 + Math.random() * 22, vz: Math.sin(a) * sp3,
        born: performance.now(), life: 0.45 + Math.random() * 0.3, ph: Math.random() * 6.283, smoke: false };
    }
  }
  function spawnHitSparks(wx, wy, wz) {               // a shaft striking flesh throws sparks the way the rocks do — same embers, at the impact rather than the hands (user)
    for (let i = 0; i < 4; i++) {
      const a = Math.random() * 6.283, sp3 = 12 + Math.random() * 20;
      sparks3d[i] = { x: wx, y: wy, z: wz, vx: Math.cos(a) * sp3, vy: 14 + Math.random() * 20, vz: Math.sin(a) * sp3,
        born: performance.now(), life: 0.4 + Math.random() * 0.3, ph: Math.random() * 6.283, smoke: false, red: true };   // red: the HIT colour (user) — spawnDeathBurst leaves it unset, so a kill still throws the 4 ordinary sparks
    }
  }
