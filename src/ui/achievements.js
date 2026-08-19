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
                    null, null, null, null];   // +16 for FALLING PETALS (user 2026-08-18) — a band rather than a share, same reasoning as the pollen's: a petal lives ~10 s where a droplet lives 0.9, so sharing would have had petals squatting on every splash slot in the blossom   // 20 death-burst slots (drops uniform 5-24): CLASH uses the first 4 as embers; a DEATH fills 4 sparks + 16 INDIVIDUAL smoke voxels (10 cm each → the plume needs more of them to read as dense) — world-space particles
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
