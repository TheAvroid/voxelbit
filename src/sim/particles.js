  // @module - splash, spark and debris particle pools and their per-frame step
  // @exports ARROW_HITS_TO_KILL, CRY_GAP, HITS_TO_KILL, SPLASH_HI, SPLASH_LIFE, SPLASH_LO, TEAR_HI, TEAR_LO, aimedCreature, hitSpot, hurtHop, lifeDrawnPrev, lifeIsDrawn, spawnDeathBurst, spawnSplash, spawnTear, startCrying
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
  const HITS_TO_KILL = 3;                              // hits a non-AXE tool needs to kill a life form (user). The axe is the killing tool and ignores this entirely.
  const ARROW_HITS_TO_KILL = 2;                        // …and an ARROW takes two (user) — between the axe's one blow and every hand tool's three
