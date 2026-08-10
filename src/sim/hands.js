  // ── right-click pickup: pebbles + twigs ─────────────────────────────────────
  // ── HAND SLOTS ── UNLIMITED (user, for now): the hotbar grows as things are picked up and always keeps
  // one trailing empty, so a pickup can never be refused. Scroll wheel cycles the whole list; Q drops
  // from the selected slot. Same-type items still stack rather than taking a new slot each time.
  // THE ORDER IS THE USER'S: axe, pick, shovel, hoe, knife, bow, spear. Anything whose .vox is missing
  // simply drops out of the line rather than leaving a hole.
  const slots = [1, PICK_IT, SHOVEL_IT, HOE_IT, KNIFE_IT, BOW_IT, SPEAR_IT].filter(Boolean).map((it) => ({ it, n: 1 }));   // the KNIFE is in the rotation too (user)   // …and the BOW is back (user)   // stone axe, PICK, SHOVEL — and the last slot stays EMPTY (user), so there is always somewhere for a pickup to land. The BOW is no longer in the starting kit for that reason; __vb.giveIt(__vb.bowId()) puts it in hand to tune.
  let selSlot = 0;
  const STACK_MAX = 8;                                 // how deep a stackable item goes (user)
  const stackable = (it) => !(it === 1 || it === KNIFE_IT || it === PICK_IT || it === SHOVEL_IT || it === BOW_IT || it === HOE_IT || it === SPEAR_IT);   // TOOLS AND WEAPONS DO NOT STACK (user) — each takes its own slot; everything else (raw meat, rocks, twigs, cones) stacks to STACK_MAX
  const canAdd = () => true;                           // …so nothing is ever un-pickable for want of room (see slotTidy)
  const slotTidy = () => {                             // keep EXACTLY one trailing empty: pickups always have a home, and the wheel never lands on a run of blanks
    while (slots.length > 1 && !slots[slots.length - 1] && !slots[slots.length - 2]) slots.pop();
    if (slots.length === 0 || slots[slots.length - 1]) slots.push(null);
    if (selSlot >= slots.length) selSlot = slots.length - 1;
  };
  slotTidy();                                          // start with the trailing empty already in place
  function addItem(it) {                               // prefer stacking, then the SELECTED slot, then any empty, then a NEW one. Never fails.
    if (stackable(it)) for (let i = 0; i < slots.length; i++) if (slots[i] && slots[i].it === it && slots[i].n < STACK_MAX) { slots[i].n++; return i; }   // stack first, up to the cap
    if (!slots[selSlot]) { slots[selSlot] = { it, n: 1 }; slotTidy(); return selSlot; }
    for (let i = 0; i < slots.length; i++) if (!slots[i]) { slots[i] = { it, n: 1 }; slotTidy(); return i; }
    slots.push({ it, n: 1 }); slotTidy(); return slots.length - 2;   // grew the hotbar (user) — -2 because slotTidy has just appended the next empty
  }
  // ── THE HOE'S TILLED-EARTH ID, DECLARED HERE AND NOWHERE ELSE ── it is minted at RUNTIME by hoeTill
  // (sim/tools.js) the first time the hoe is swung, and read by main/debug-api.js's tillInfo. It cannot live
  // in sim/tools.js: that fragment is a `// @module`, and a module hands the shared scope a CONST SNAPSHOT
  // taken at module-init — a `let` the module assigns ITSELF would leave every reader outside frozen at the
  // 0 it started as, silently (tillInfo reported id 0 / col null forever, and lint-vb check 10 refuses the
  // export for exactly that reason). Declared in a plain fragment above tools.js, the write lands on the one
  // binding everybody holds. hoeTill is the only writer.
  let TILL_ID = 0;
  // quaternion helpers — item orientations interpolate as ROTATIONS (slerp), never as three independently
  // lerped axis vectors (that collapses mid-flight and reads as the item flipping/glitching)
  const m2q = (X, Y, Z) => { const t = X[0] + Y[1] + Z[2];                      // columns of local→space matrix, [x,y,z,w]
    if (t > 0) { const s = Math.sqrt(t + 1) * 2; return [(Y[2] - Z[1]) / s, (Z[0] - X[2]) / s, (X[1] - Y[0]) / s, s / 4]; }
    if (X[0] > Y[1] && X[0] > Z[2]) { const s = Math.sqrt(1 + X[0] - Y[1] - Z[2]) * 2; return [s / 4, (Y[0] + X[1]) / s, (Z[0] + X[2]) / s, (Y[2] - Z[1]) / s]; }
    if (Y[1] > Z[2]) { const s = Math.sqrt(1 + Y[1] - X[0] - Z[2]) * 2; return [(Y[0] + X[1]) / s, s / 4, (Z[1] + Y[2]) / s, (Z[0] - X[2]) / s]; }
    { const s = Math.sqrt(1 + Z[2] - X[0] - Y[1]) * 2; return [(Z[0] + X[2]) / s, (Z[1] + Y[2]) / s, s / 4, (X[1] - Y[0]) / s]; } };
  const q2m = (q) => { const [x, y, z, w] = q; return [
    [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
    [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
    [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]]; };
  const qslerp = (a, b, t) => { let d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], bb = b;
    if (d < 0) { d = -d; bb = [-b[0], -b[1], -b[2], -b[3]]; }                   // shortest arc
    if (d > 0.9995) { const v = [0, 1, 2, 3].map((i) => a[i] + (bb[i] - a[i]) * t); const l = Math.hypot(v[0], v[1], v[2], v[3]) || 1; return [v[0] / l, v[1] / l, v[2] / l, v[3] / l]; }
    const th = Math.acos(Math.min(1, d)), s = Math.sin(th), ka = Math.sin((1 - t) * th) / s, kb = Math.sin(t * th) / s;
    return [a[0] * ka + bb[0] * kb, a[1] * ka + bb[1] * kb, a[2] * ka + bb[2] * kb, a[3] * ka + bb[3] * kb]; };
  let grabAnim = null;                                 // {t0, it, x, y, z, aPh} — a grabbed item FLYING from its world spot into the hand
  // ── HOW LONG THAT FLIGHT TAKES ── 340 → 180 ms (user 2026-08-05: faster) → 360 ms (user 2026-08-06: "twice as slow", it
  // read as too fast). KNOWN COST OF THE SLOWER FLIGHT, which is why it was shortened in the first place: only ONE grab
  // may be in the air at a time — tryPickup and autoPickup both bail while grabAnim is set — so a PILE of items (a
  // scatter of loosed arrows, say) drains at one per flight and the wait compounds at twice the old rate. If that ever
  // becomes the complaint, the fix is to allow concurrent grabs rather than to shorten the flight again.
  // Chunks a swing knocks loose keep their OWN, deliberately separate timings (PH.absorbMs / PH.absorbFly).
  const GRAB_MS = 360;
  const drops = [];                                    // DROPPED ITEMS (Q) — TOSSED on a ballistic arc, then hover + spin; right-click to grab
  const DROP_REST_MS = 30000, DROP_REST_EASE = 1000;  // a dropped item hovers for this long, then eases to a dead stop on the ground (user)
  const TOSS_G = -170, TOSS_V = 55, TOSS_UP = 18;      // vox/s² gravity, throw speed along the view ray, extra up-kick
  const HURL_V = 240, HURL_UP = 9;                     // …and the THROWN profile (right-hold + left-click on a rock). Level-aimed it carries ~2x a Q-toss;
                                                       // the small up-kick is what stops a flat throw burying itself a few metres out — gravity here is 170 vox/s²,
                                                       // so with none at all the rock is on the ground in a third of a second no matter how fast it leaves the hand.
  function dropHeld(hurl) {                            // hurl = thrown hard along the view ray (see HURL_V) rather than lobbed
    const sel = slots[selSlot];
    if (!sel || dead) return;
    const it = sel.it;
    if (--sel.n <= 0) { slots[selSlot] = null; slotTidy(); }   // …and slotTidy, like every other consumer (projectiles.js, ui/audio.js): emptying a slot without it broke the "exactly one trailing empty" invariant, so Q-dropping two middle slots left three blanks in a row for the scroll wheel to cycle through   // (a Q-tossed WORM flies the normal ballistic arc like the axe — it converts to a LIVE worm when it LANDS, see the drops update)
    // launch from the held item's true world spot (held units ÷ scale = voxels), with the held orientation — it FLIES out of the hand
    const cfg = heldCfg(it);
    const s = 1 / cfg.scale, pr = prevCam;
    const cx2 = cfg.x * s, cy2 = cfg.y * s, cz2 = cfg.z * s;
    const cy = Math.cos(cfg.yaw), sy = Math.sin(cfg.yaw), cp2 = Math.cos(cfg.pitch), sp2 = Math.sin(cfg.pitch), cr2 = Math.cos(cfg.roll), sr2 = Math.sin(cfg.roll);
    const toW = (v) => [pr.right[0] * v[0] + pr.up[0] * v[1] + pr.fwd[0] * v[2], pr.right[1] * v[0] + pr.up[1] * v[1] + pr.fwd[1] * v[2], pr.right[2] * v[0] + pr.up[2] * v[1] + pr.fwd[2] * v[2]];
    const sx = pr.pos[0] + pr.right[0] * cx2 + pr.up[0] * cy2 + pr.fwd[0] * cz2;
    const sy2 = pr.pos[1] + pr.right[1] * cx2 + pr.up[1] * cy2 + pr.fwd[1] * cz2;
    const sz = pr.pos[2] + pr.right[2] * cx2 + pr.up[2] * cy2 + pr.fwd[2] * cz2;
    const cpv = Math.cos(P.pitch), spv = Math.sin(P.pitch);                     // TOSS along the view ray + an up-kick, integrated against the terrain
    const tV = hurl ? HURL_V : TOSS_V, tU = hurl ? HURL_UP : TOSS_UP;
    const vx = Math.sin(P.yaw) * cpv * tV, vy0 = spv * tV + tU, vz = Math.cos(P.yaw) * cpv * tV;
    let px = sx, py = sy2, pz = sz, vy = vy0, T = 0;
    for (let i = 0; i < 360; i++) {                                             // 1.8 s cap, 5 ms steps (trapezoid = exact for constant g)
      const nvy = vy + TOSS_G * 0.005;
      px += vx * 0.005; py += (vy + nvy) * 0.5 * 0.005; pz += vz * 0.005; vy = nvy; T += 0.005;
      if (T > 0.12 && py <= hmap[gwrap(Math.round(px), WX) + gwrap(Math.round(pz), WZ) * WX] + 9.0) break;
    }
    const lx = Math.round(px), lz = Math.round(pz);
    const aX = toW([cr2 * cy, sr2 * cp2 + cr2 * sy * sp2, sr2 * sp2 - cr2 * sy * cp2]);
    const hl = Math.hypot(aX[0], aX[2]) || 1, Xh = [aX[0] / hl, 0, aX[2] / hl];  // UPRIGHT at release — keep the hand's heading, level everything else
    // The BOW hovers z-DOWN (its art's +z is the underside — see the drop update). The launch frame has to
    // agree: released z-UP, the flight slerped a full 180° into the hover, through a degenerate axis, and
    // the bow thrashed on the way down (user: "severely glitched"). Both ends now share the same up.
    const upFlip = !!(BOW_IT && it === BOW_IT);
    drops.push({ x: lx, y: hmap[gwrap(lx, WX) + gwrap(lz, WZ) * WX], z: lz, it, ph: Math.random() * 6.28, born: performance.now(),
      T, sx, sy: sy2, sz, vx, vy: vy0, vz,   // …the BOW hovers and spins like everything else again (user)
      ex: sx + vx * T, ey: sy2 + vy0 * T + 0.5 * TOSS_G * T * T, ez: sz + vz * T,   // exact arc endpoint — the settle blend starts here
      q0: upFlip ? m2q(Xh, [-Xh[2], 0, Xh[0]], [0, -1, 0]) : m2q(Xh, [Xh[2], 0, -Xh[0]], [0, 1, 0]) });
    if (drops.length > 4) drops.shift();               // the composite renders up to 4 at once
  }
