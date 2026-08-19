  // ── asset editor state ── declared HERE (before solid/stepShifts, which gate on it); the editor itself lives lower down
  // pw/pd are the STAGE footprint, doubled from 121 to 242 (user 2026-08-18) so a grove fits on it. The note is
  // HERE and not on the line itself: that line is one dense object literal, and a `//` part-way into it ate
  // fcells/ring/paused and left the editor throwing "ED.fcells is not iterable" the moment it opened.
  // Doubling it has a cost worth knowing: edEnter picks the stage height by clearing the tallest terrain on the
  // footprint, so four times the area finds taller terrain and the stage sits HIGHER, leaving less headroom for
  // whatever is staged. Measured after the change: 46 voxels, which is why the birch grove is sized to fit that.
  const ED = { on: false, ret: null, prev: new Map(), frames: [], sel: 0, x0: 0, z0: 0, y: 0, pw: 242, pd: 242, fcells: [], ring: [], paused: false, animT: 0, giz: false, dragAxis: -1, dragAcc: 0, gizBoxes: [], blink: false, spin: 0, bunny: false, rgiz: false, dragRing: -1, dragRAcc: 0, rgizBoxes: [],
    frames2: [], fcells2: [], box2: null, off2: '', name1: '', name2: '', hopX: 0, hopY: 0, hopZ: 0, hop2X: 0, hop2Y: 0, hop2Z: 0 };   // TWO bunnies on the stage (user): frames = LEFT/editable lane, frames2 = RIGHT/preview lane; fcells2/box2 = the preview's stamped cells + hitbox; off2 = the preview's offset namespace; name1/name2 = which variant is in each lane (for the combined export); hop*/hop2* = per-lane forward-march accumulation

  // ── player ── 10 cm voxels: 1.8 m tall, eye 1.65 m; old-engine feel
  const EYE = 18.5, HEIGHT = 20, CR_EYE = 11.5, CR_HEIGHT = 13, HW = 2.6;   // a proper 2 m / 20-voxel person
  const WALK = 46, SPRINT = 1.85, CROUCHM = 0.45, JUMP = 66, GRAVITY = 200;
  const SWIM_UP = 18, SWIM_SINK = 13, SWIM_EASE = 6.5;  // rise / sink ceilings in vox/s and how briskly the body eases between them (1/s). ALL RAISED HARD (user 2026-08-07: floating was "very slow and mushy", the bob has to be obvious): 2x the ceilings and 3x the ease, so a tap of Space lifts you visibly and letting go drops you back just as plainly
  // SWIM_DEEP: how deep the water must be over the FEET before the body starts swimming instead of walking.
  // 10 (waist) was right in principle and wrong in practice — MEASURED, the lakes in this world are only 4-5
  // voxels deep, so a waist test almost never fired and the bob the user was looking for simply never ran.
  // 5 is knee-deep: it engages in an ordinary lake while still leaving a shallow wade as walking.
  const SWIM_DEEP = 5;
  const SWIM_K = 2.0, SWIM_RISE = 11, SWIM_BOB = 4.5;                  // spring gain on the eye-vs-waterline error, and how far holding Space lifts the float line. Gain 0.8 -> 2.0 and lift 7 -> 11: the drive is what made it feel mushy, since a weak gain on a small error is a long, soft approach. SWIM_BOB is the STROKE amplitude — it rides on the Space lift only, never idle (user 2026-08-07)
  const WATER_SPD = 0.344;                             // +25% (user 2026-08-07). HALVED (user 2026-08-05) — was 0.55. Wading and swimming are the same multiplier: it scales the horizontal speed the moment the body is in water, so this slows both.   // +20% base speed
  const BOUNCE_V0 = 116, BOUNCE_DV = 27, BOUNCE_MAX = 239;   // MUSHROOM TRAMPOLINE: first bounce ≈2× a normal jump, +DV each consecutive bounce, capped. 1.5× HIGHER (user): apex goes with v², so every speed here is the old one × √1.5, not × 1.5 (≈9 m → ≈13.5 m at the cap)
  const P = { x: SPWX + 0.5, y: 0, z: SPWZ + 0.5, vy: 0, yaw: SPYAW, pitch: SPPITCH, roll: 0, onGround: true, fly: false, crouch: false, sprintJump: false, hvx: 0, hvz: 0, bounceN: 0, fallT: 0, sink: 0 };
  P.y = hmap[gwrap(SPWX, WX) + gwrap(SPWZ, WZ) * WX];
  let smoothEye = P.y + EYE;
  const keys = new Set();
  // Is this world point inside a rigid body? A fallen tree is NOT in W — that is the whole point of the
  // off-grid representation — so without this the player walks straight through a felled trunk. Cheap:
  // one sphere reject per body, then a direct grid lookup. No search, the same property Teardown relies
  // on for voxel-vs-voxel collision.
  // ── BROAD PHASE: A REAL WORLD AABB, NOT A SPHERE ── phBodySolid is the single hottest function in the
  // game: MEASURED at 26.6% of all JS samples (~1.5 ms/frame) in a calm forest, because every solidity
  // query in the game — player collision, creature nav, fish, mammal seating — walked all 16 bodies, and the
  // only reject was a sphere of rMax + 2. rMax is the distance to the FARTHEST voxel, so for a felled pine
  // it is ~60 and that sphere encloses ~113,000 voxels of volume around a 7,800-voxel trunk: near a felled
  // tree it essentially never rejected, and every query paid the full transform on every body.
  //
  // b.ab is the exact world-space box of the body's own grid, from the standard rotated-extent identity
  // (half-extent along world axis k = |ax[k]|·ex + |ay[k]|·ey + |az[k]|·ez), so it is tight rather than
  // conservative and costs 6 compares instead of a transform. PH.abAll is their union: a query nowhere near
  // any body — which is nearly all of them — now exits on 6 compares total instead of 16 sphere tests.
  // Rebuilt once per frame for every body (see phAabbAll), which is 16 boxes against thousands of queries.
  const phAabb = (b) => {
    const g = b.gpu;
    if (!g) { b.ab = null; return; }
    const ex = g.bw * 0.5, ey = g.bh * 0.5, ez = g.bd * 0.5;
    const cx = ex - g.comL[0], cy = ey - g.comL[1], cz = ez - g.comL[2];   // box centre, relative to the COM, in the body's own frame
    const ax = b.ax, ay = b.ay, az = b.az;
    const ab = b.ab || (b.ab = new Float64Array(6));
    for (let k = 0; k < 3; k++) {
      const c = b.pos[k] + cx * ax[k] + cy * ay[k] + cz * az[k];
      const h = Math.abs(ax[k]) * ex + Math.abs(ay[k]) * ey + Math.abs(az[k]) * ez + 1.0;   // +1: the query tests a voxel CENTRE against a box measured in whole voxels
      ab[k] = c - h; ab[k + 3] = c + h;
    }
  };
  const phAabbAll = () => {                          // once per frame: every body's box, and their union
    const A = PH.abAll;
    A[0] = A[1] = A[2] = 1e30; A[3] = A[4] = A[5] = -1e30;
    for (let i = 0; i < PH.bodies.length; i++) {
      const b = PH.bodies[i];
      phAabb(b);
      const ab = b.ab; if (!ab || b.absorbing) continue;
      for (let k = 0; k < 3; k++) { if (ab[k] < A[k]) A[k] = ab[k]; if (ab[k + 3] > A[k + 3]) A[k + 3] = ab[k + 3]; }
    }
  };
  const phBodySolid = (x, y, z) => {
    if (!PH.on) return false;
    const A = PH.abAll, px = x + 0.5, py = y + 0.5, pz = z + 0.5;
    if (px < A[0] || px > A[3] || py < A[1] || py > A[4] || pz < A[2] || pz > A[5]) return false;   // nowhere near ANY body
    for (let i = 0; i < PH.bodies.length; i++) {
      const b = PH.bodies[i], g = b.cpuGrid; if (!g || !b.gpu || b.absorbing) continue;   // a chunk on its way into the player must not shove them
      const ab = b.ab;
      if (ab && (px < ab[0] || px > ab[3] || py < ab[1] || py > ab[4] || pz < ab[2] || pz > ab[5])) continue;
      const dx = x + 0.5 - b.pos[0], dy = y + 0.5 - b.pos[1], dz = z + 0.5 - b.pos[2];
      const lx2 = Math.floor(dx * b.ax[0] + dy * b.ax[1] + dz * b.ax[2] + b.gpu.comL[0]);
      const ly2 = Math.floor(dx * b.ay[0] + dy * b.ay[1] + dz * b.ay[2] + b.gpu.comL[1]);
      const lz2 = Math.floor(dx * b.az[0] + dy * b.az[1] + dz * b.az[2] + b.gpu.comL[2]);
      if (lx2 < 0 || ly2 < 0 || lz2 < 0 || lx2 >= b.gpu.bw || ly2 >= b.gpu.bh || lz2 >= b.gpu.bd) continue;
      const bid = g[lx2 + ly2 * b.gpu.bw + lz2 * b.gpu.bw * b.gpu.bh];
      if (foliaTab[bid]) continue;                   // …and the player walks through a fallen crown's needles, same as a standing one
      if (bid && solidTab[bid] === 1) return true;
    }
    return false;
  };
  // ── WHAT A RIGID BODY HAS AT THIS WORLD VOXEL ── phBodySolid's question is "does this block me", which
  // is not the tool's question: a felled tree is off-grid, so W reads AIR everywhere the crown and the bole
  // actually are, and every aim test that only consulted W was blind to the whole tree once it hit the
  // ground. This returns the palette id under the point instead, so the crosshair can tell needles from bark
  // on a fallen pine exactly as it does on a standing one. Same eligibility filter as phChopBody — a flying
  // chip, a corpse or a scrap too small to chop is not something you can aim at.
  const phBodyIdAt = (x, y, z) => {
    if (!PH.on) return 0;
    for (let i = 0; i < PH.bodies.length; i++) {
      const b = PH.bodies[i], g = b.cpuGrid;
      if (!g || !b.gpu || b.absorbing || b.rag || b.n < PH.chopMinBody) continue;
      const ab = b.ab;                               // …the same tight box the collision path uses (see phAabb)
      if (ab && (x + 0.5 < ab[0] || x + 0.5 > ab[3] || y + 0.5 < ab[1] || y + 0.5 > ab[4] || z + 0.5 < ab[2] || z + 0.5 > ab[5])) continue;
      const dx = x + 0.5 - b.pos[0], dy = y + 0.5 - b.pos[1], dz = z + 0.5 - b.pos[2];
      const lx2 = Math.floor(dx * b.ax[0] + dy * b.ax[1] + dz * b.ax[2] + b.gpu.comL[0]);
      const ly2 = Math.floor(dx * b.ay[0] + dy * b.ay[1] + dz * b.ay[2] + b.gpu.comL[1]);
      const lz2 = Math.floor(dx * b.az[0] + dy * b.az[1] + dz * b.az[2] + b.gpu.comL[2]);
      if (lx2 < 0 || ly2 < 0 || lz2 < 0 || lx2 >= b.gpu.bw || ly2 >= b.gpu.bh || lz2 >= b.gpu.bd) continue;
      const bid = g[lx2 + ly2 * b.gpu.bw + lz2 * b.gpu.bw * b.gpu.bh];
      if (bid) return bid;
    }
    return 0;
  };
  const solid = (x, y, z) => {                         // world coords, toroidal — the window always surrounds the player
    if (y < 0) return true;
    if (y >= WY) return false;
    const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    if (ED.on) return id === ED_WHITE || id === ED_GREY || id === ED_HLITE;   // editor world: ONLY the stage collides — the hidden forest must not be an invisible wall
    // ── PINECONES HAVE NO HITBOX FOR THE PLAYER (user 2026-08-05) ── you walk straight through them. Done
    // HERE and not by clearing solidTab, because a cone's palette ids are also the pine's own WOOD ids: the
    // model shares entries with the bark, so zeroing solidTab for them would take the hitbox off the trunks
    // too. Scoping it to the player's own collision leaves the tool paths, the chop gates and the physics
    // reading exactly what they read before.
    // ── AND NEITHER DO NEEDLES (user 2026-08-05: "the foliage of the trees … blocks the player sometimes") ──
    // "sometimes" was the clue and it is a palette collision, not a rule. The canopy is walk-through
    // everywhere it is described in this file, and MEASURED it very nearly is: of the eight foliage ids
    // (46-53) exactly ONE, 51, comes back solid from the player's own test — the other seven do not. 51 is
    // also carried by a model markSolid marks as a real obstacle (logs, rocks and mushrooms all go through
    // that loop), and the 256-entry palette is full, so the two share the entry. One needle voxel in eight
    // being an invisible wall at head height is exactly what walking into a crown felt like.
    // Scoped to the PLAYER's collision for the same reason the cones are: the id is genuinely shared, so
    // clearing solidTab would take the hitbox off that decor everywhere, while this leaves the tool paths,
    // the chop gates, the arrow's own blocker and the physics reading precisely what they read before.
    // The cost is that whatever decor owns 51 is walk-through for the player too; a canopy you cannot push
    // through is the worse of the two by a long way.
    if (coneTab[id] || foliaTab[id]) return phBodySolid(x, y, z);
    return solidTab[id] === 1 || phBodySolid(x, y, z);   // a felled tree keeps its hitbox even though it lives off the grid
  };
  const waterAt = (x, y, z) => {
    if (y < 0 || y >= WY) return false;
    const v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    return v === WATER_T || v === WATER_B;
  };
  const lavaAt = (x, y, z) => {
    if (y < 0 || y >= WY) return false;
    const v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
    return v === LAVA_T || v === LAVA_B || v === LAVA_R || v === LAVA_Y;
  };
  // ── CONTACT MARGIN ── how close the player's own COLLISION BOX has to come to a cactus voxel to be stung.
  // The three reaches before this (0.45 → 0.95 → 1.6) all sampled a 3x3 of columns at x ± reach and could
  // NOT fire, for a reason that is arithmetic rather than tuning: cacti are markSolid, so the body never
  // gets inside one — moveAxis parks it at floor(x + HW) - HW - 0.001, i.e. the box FACE 1 mm off the
  // cactus's face and the box CENTRE a full HW (2.6) further back. A probe that only ever looks 1.6 out
  // from the centre is still ~1.5 voxels short of the column it is standing against, so "walked into a
  // saguaro" measured false every single time. Widening the reach was never going to reach it either:
  // a 3-point sample SKIPS columns (from x = 100.4, floor(x + 1.6) = 102 — column 101 is never read at all).
  // So the test is now the collision box itself, swept over the CONTIGUOUS columns it overlaps. No distance
  // guess is left in it: touching is touching. The margin is under half a voxel, which the 1 mm resting gap
  // clears easily while a player standing even one clear voxel off is plainly outside it.
  const CACT_MARGIN = 0.4;
  // ── …AND HOW OFTEN IT STINGS ONCE YOU ARE ON IT ── named here, beside the reach, because tick-body has to
  // use the same number in two places now: the gap between stings, and the value the timer RESTS at while
  // you are clear of one. Those two drifting apart is the difference between "it stings the moment you
  // touch it" and "it stings twice".
  const CACT_CD = 0.9;
  // ── A CACTUS STINGS (user 2026-08-15: "take away health if the player rubs up against it") ── the player's
  // whole body brushes it, not just the eye: a saguaro arm at knee height must count, so this walks the
  // body's live height (crouched or standing — tick-body passes the same hh collision uses) over the whole
  // footprint. Feet-up only: a cactus BURIED in the sand beside you sits below floor(y) and must not sting.
  const cactusHurtAt = (x, y, z, hh) => {   // NOT cactusAt - that name is terrain.js's scatter function
    const h = hh || HEIGHT, r = HW + CACT_MARGIN;
    const x0 = Math.floor(x - r), x1 = Math.floor(x + r), z0 = Math.floor(z - r), z1 = Math.floor(z + r);
    const y0 = Math.max(0, Math.floor(y)), y1 = Math.min(WY - 1, Math.floor(y + h - 0.1));
    for (let yy = y0; yy <= y1; yy++) for (let zz = z0; zz <= z1; zz++) for (let xx = x0; xx <= x1; xx++) {
      const v = W[gwrap(xx, WX) + yy * WX + gwrap(zz, WZ) * WX * WY];
      if (v && cactusTab[v]) return true;
    }
    return false;
  };
  // ── WHAT REFUSED THE LAST boxFree ── a creature's AABB, or a solid voxel? The two want different answers
  // from moveAxis (see the clamp there), and boxFree's bare `false` cannot tell them apart. Written on every
  // call, so it is only meaningful immediately after one.
  let boxBodyHit = false;
  // `noBody` asks the VOXEL GRID only, skipping the creature boxes — moveAxis needs that one question to work
  // out whether a player an animal has walked into can move without being shoved into terrain.
  const boxFree = (px, py, pz, hh, noBody) => {
    boxBodyHit = false;
    for (let bb = 0; !noBody && bb < birdBoxes.length; bb++) {             // SOLID cardinals (Task 2): the player's box may not overlap any bird's world-AABB
      const B2 = birdBoxes[bb];
      if (B2.active
        && px + HW > B2.cx - B2.hx && px - HW < B2.cx + B2.hx
        && pz + HW > B2.cz - B2.hz && pz - HW < B2.cz + B2.hz
        && py + hh > B2.cy - B2.hy && py       < B2.cy + B2.hy) { boxBodyHit = true; return false; }
    }
    for (let bb = 0; !noBody && bb < bunnyBoxes.length; bb++) {            // SOLID BUNNIES (user): the player can't run through a nearby world bunny
      const B2 = bunnyBoxes[bb];
      if (B2.active
        && px + HW > B2.cx - B2.hx && px - HW < B2.cx + B2.hx
        && pz + HW > B2.cz - B2.hz && pz - HW < B2.cz + B2.hz
        && py + hh > B2.cy - B2.hy && py       < B2.cy + B2.hy) { boxBodyHit = true; return false; }
    }
    for (let bb = 0; !noBody && bb < armBoxes.length; bb++) {              // SOLID ARMADILLOS: the player can't run through a nearby world armadillo
      const B2 = armBoxes[bb];
      if (B2.active
        && px + HW > B2.cx - B2.hx && px - HW < B2.cx + B2.hx
        && pz + HW > B2.cz - B2.hz && pz - HW < B2.cz + B2.hz
        && py + hh > B2.cy - B2.hy && py       < B2.cy + B2.hy) { boxBodyHit = true; return false; }
    }
    for (let bb = 0; !noBody && bb < skunkBoxes.length; bb++) {             // SOLID SKUNKS: the player can't run through a nearby world skunk
      const B2 = skunkBoxes[bb];
      if (B2.active
        && px + HW > B2.cx - B2.hx && px - HW < B2.cx + B2.hx
        && pz + HW > B2.cz - B2.hz && pz - HW < B2.cz + B2.hz
        && py + hh > B2.cy - B2.hy && py       < B2.cy + B2.hy) { boxBodyHit = true; return false; }
    }
    for (let bb = 0; !noBody && bb < flamBoxes.length; bb++) {             // SOLID FLAMINGOS: same test, same reason — it was the one land-band creature with no box at all
      const B3 = flamBoxes[bb];
      if (B3.active
        && px + HW > B3.cx - B3.hx && px - HW < B3.cx + B3.hx
        && pz + HW > B3.cz - B3.hz && pz - HW < B3.cz + B3.hz
        && py + hh > B3.cy - B3.hy && py       < B3.cy + B3.hy) { boxBodyHit = true; return false; }
    }
    for (let bb = 0; !noBody && bb < porcBoxes.length; bb++) {             // SOLID PORCUPINES: the player can't run through a nearby world porcupine
      const B2 = porcBoxes[bb];
      if (B2.active
        && px + HW > B2.cx - B2.hx && px - HW < B2.cx + B2.hx
        && pz + HW > B2.cz - B2.hz && pz - HW < B2.cz + B2.hz
        && py + hh > B2.cy - B2.hy && py       < B2.cy + B2.hy) { boxBodyHit = true; return false; }
    }
    const x0 = Math.floor(px - HW), x1 = Math.floor(px + HW), z0 = Math.floor(pz - HW), z1 = Math.floor(pz + HW);
    const y0 = Math.floor(py + 0.001), y1 = Math.floor(py + hh - 0.001);
    for (let y = y0; y <= y1; y++) for (let z = z0; z <= z1; z++) for (let x = x0; x <= x1; x++) if (solid(x, y, z)) return false;
    return true;
  };
  // Is this world cell currently owned by a GRID-STAMPED creature (perched songbirds 64-243, land
  // mammals 276-371)? Needed because mushTab is keyed by PALETTE ID, and the palette is full at 256:
  // edCol nearest-matches a new model colour onto an existing entry, so the armadillo's browns landed
  // on mushroom browns (ids 175/176) and the player trampolined off its back. stampApply only ever
  // writes into AIR or FOLIAGE, and a real mushroom voxel is neither — so a bouncy id inside a live
  // stamp is ALWAYS this false positive, never real mushroom geometry.
  // Cheap by construction: onMushroom() runs on a LANDING, not per frame, and the radius test rejects
  // every creature that cannot be under the player's feet before touching its cell list.
  const cellStamped = (ii) => {
    for (let j = CARD_0; j < DES_END; j++) {       // CARD_0 = the first grid-stamped band; nothing below it stamps into W
      const B = wbf[j];
      if (!B || !B.sN) continue;
      const dx = B.x - P.x, dz = B.z - P.z;
      if (dx * dx + dz * dz > 400) continue;           // >20 vox away — cannot be the voxel under the feet
      const c = B.sCells;
      for (let i = 0; i < B.sN; i++) if (c[i] === ii) return true;
    }
    return false;
  };
  const onMushroom = () => {                            // is the player's box resting on a bouncy mushroom voxel? (feet sit at floor(P.y); the surface is the voxel below)
    const gy = Math.floor(P.y) - 1; if (gy < 0 || gy >= WY) return false;
    const x0 = Math.floor(P.x - HW), x1 = Math.floor(P.x + HW), z0 = Math.floor(P.z - HW), z1 = Math.floor(P.z + HW);
    for (let z = z0; z <= z1; z++) for (let x = x0; x <= x1; x++) {
      const ii = gwrap(x, WX) + gy * WX + gwrap(z, WZ) * WX * WY;
      if (mushTab[W[ii]] && !cellStamped(ii)) return true;   // real mushroom only — a creature standing on one of the shared ids must not trampoline
    }
    return false;
  };
  const onSand = () => {                                // is the player standing on sand? (beach/lakebed) → slow their movement
    const gy = Math.floor(P.y) - 1; if (gy < 0 || gy >= WY) return false;
    const x0 = Math.floor(P.x - HW), x1 = Math.floor(P.x + HW), z0 = Math.floor(P.z - HW), z1 = Math.floor(P.z + HW);
    for (let z = z0; z <= z1; z++) for (let x = x0; x <= x1; x++) if (sandTab[W[gwrap(x, WX) + gy * WX + gwrap(z, WZ) * WX * WY]]) return true;
    return false;
  };
  const QS_DRY = 10, QS_SAND = 6;                       // quicksand needs NO water within QS_DRY, and unbroken sand out to QS_SAND in all 8 directions
  const onFlatSand = () => {                            // QUICKSAND only swallows you on a real SAND FLAT — sloped beaches, dune faces and the thin sand RIM that outlines every lake and river stay ordinary ground
    if (!onSand()) return false;
    if (waterAt(Math.floor(P.x), Math.floor(P.y), Math.floor(P.z))) return false;   // SUBMERGED sand is just lakebed, not quicksand (user): the voxel the body stands in is water → wading/standing on a bed never sinks you. A DRY beach top sits at WL with the player at WL+1, so this never fires on the sand flats that should swallow you.
    const cx = Math.floor(P.x), cz = Math.floor(P.z);
    const h0 = hmap[gwrap(cx, WX) + gwrap(cz, WZ) * WX];
    for (let dz = -3; dz <= 3; dz += 3) for (let dx = -3; dx <= 3; dx += 3)
      if (Math.abs(hmap[gwrap(cx + dx, WX) + gwrap(cz + dz, WZ) * WX] - h0) > 1) return false;   // any real step within 30 cm → it's a slope, not a sand flat
    // ── SHORELINE GUARD (user: "you're making me sink in the sand that makes up the OUTLINE of water") ── the beach rim
    // that traces every waterline is flat sand too, so flatness alone was swallowing people at the water's edge. A real
    // sand PAN is broad and dry: no water anywhere within QS_DRY, and still pure sand underfoot out at QS_SAND on every
    // heading. The shore rim fails both — water sits a few voxels away and dirt/grass takes over just inland.
    for (let k = 0; k < 8; k++) { const a = k * 0.785398, sa = Math.sin(a), ca = Math.cos(a);
      for (let d = 2; d <= QS_DRY; d += 2) if (waterAt(Math.round(cx + sa * d), WL, Math.round(cz + ca * d))) return false;
      const px = Math.round(cx + sa * QS_SAND), pz = Math.round(cz + ca * QS_SAND);
      const gpx = gwrap(px, WX), gpz = gwrap(pz, WZ), py = hmap[gpx + gpz * WX] - 1;
      if (py < 0 || py >= WY || !sandTab[W[gpx + py * WX + gpz * WX * WY]]) return false;
    }
    return true;
  };
  function moveAxis(axis, d, hh) {
    if (d === 0) return false;                         // a zero move must never snap-shove an embedded player sideways
    let hit = false;
    const steps = Math.max(1, Math.ceil(Math.abs(d) / 0.85));
    const dd = d / steps;
    for (let s = 0; s < steps; s++) {
      if (axis === 0) P.x += dd; else if (axis === 1) P.y += dd; else P.z += dd;
      if (boxFree(P.x, P.y, P.z, hh)) continue;
      const bodyBlk = boxBodyHit;                      // WHAT refused, captured NOW: every boxFree below overwrites the flag
      if (axis !== 1 && (P.onGround || !boxFree(P.x, P.y - 1.4, P.z, hh))) {   // step-up even during downhill micro-air — no brief catches
        let stepped = false;
        for (let up = 1; up <= 5; up++) if (boxFree(P.x, P.y + up, P.z, hh)) { P.y += up; stepped = true; break; }   // auto-step up to 5 voxels / 50 cm (user; was 3)
        if (stepped) continue;
      }
      // ── A CREATURE'S BOX IS NOT THE VOXEL GRID (2026-08-10) ── the clamps below resolve against
      // Math.floor(P.x ± HW) / Math.floor(P.y), which is the right answer for a solid voxel and an arbitrary
      // one for an animal's AABB: those boxes are deliberately wider than the voxels the creature stamps, so a
      // refusal can come from a box with no solid cell anywhere near the face the clamp snaps to. That snapped
      // point is then a FIXED POINT in both directions — every later move clamps straight back onto it — the
      // step-up above cannot clear a box that spans more than 5 voxels, and the axis-1 clamp cancels the jump
      // that would have escaped, so the player stood locked in x, z AND y until the animal wandered off.
      // A box refusal now simply backs the sub-step out, which is a position that was free a moment ago. And
      // if the animal has already walked INTO us — the spot we came from is inside its box too — the move is
      // let THROUGH instead, provided the voxel grid ahead is clear, so an animal can never be the reason the
      // player cannot move. Solid geometry keeps the clamp exactly as it was.
      if (bodyBlk) {
        const bx = axis === 0 ? P.x - dd : P.x, by = axis === 1 ? P.y - dd : P.y, bz = axis === 2 ? P.z - dd : P.z;   // where this sub-step started
        const engulf = !boxFree(bx, by, bz, hh) && boxBodyHit;   // …and it was already inside a creature box before we moved
        if (engulf && boxFree(P.x, P.y, P.z, hh, true)) continue;   // walk out through the animal rather than stand pinned — the grid ahead is empty, so this cannot push anyone into terrain
        P.x = bx; P.y = by; P.z = bz;                  // ordinary contact: stop AT the animal, no snap to a voxel face
        hit = true; break;
      }
      if (axis === 0) { P.x = dd > 0 ? Math.floor(P.x + HW) - HW - 0.001 : Math.floor(P.x - HW) + 1 + HW + 0.001; }
      else if (axis === 2) { P.z = dd > 0 ? Math.floor(P.z + HW) - HW - 0.001 : Math.floor(P.z - HW) + 1 + HW + 0.001; }
      else { P.y = dd > 0 ? Math.floor(P.y + hh) - hh - 0.001 : Math.floor(P.y) + 1 + 0.001; }
      hit = true; break;
    }
    return hit;
  }
  function maybeRecenter() {
    const thr = Math.max(200, HALF * 0.45);
    if (Math.abs(P.x - (winOX + HALF)) > thr || Math.abs(P.z - (winOZ + HALF)) > thr) recenter(P.x, P.z);
    else resetHist = 1;
  }
  function respawn() {
    P.x = SPWX + 0.5; P.z = SPWZ + 0.5; P.yaw = SPYAW; P.pitch = SPPITCH; P.vy = 0; P.hvx = 0; P.hvz = 0; P.sink = 0;   // respawning always lifts you back out of the sand + faces the baked spawn direction
    maybeRecenter();
    P.y = hmap[gwrap(SPWX, WX) + gwrap(SPWZ, WZ) * WX];
    while (P.y < WY - 20 && !boxFree(P.x, P.y, P.z, HEIGHT)) P.y += 1;
    P.fallT = 0; uwT = 0;                               // fresh lungs on respawn
    vitReset();                                         // …and a full bar of hearts and hunger
    smoothEye = P.y + EYE;
  }
  let spawnBake = 'let SPWX = ' + SPWX + ', SPWZ = ' + SPWZ + ';';
  function rerollSpawn() {                              // H — jump to a fresh random patch of the (infinite, DETERMINISTIC) world, then bake the good one into the code
    SPWX = Math.round((Math.random() - 0.5) * 400000);
    SPWZ = Math.round((Math.random() - 0.5) * 400000);
    let g = 0;
    while ((H(SPWX, SPWZ) <= WL + 6 || nearCave(SPWX, SPWZ)) && g++ < 6000) SPWX += 16;   // valid land, off water/gorges/SAND (H and nearCave are both deterministic → work anywhere). WL + 6 is the quicksand guard — see the note in world/build.js
    respawn();                                          // teleport + maybeRecenter regenerates the world at the new spawn
    spawnBake = 'let SPWX = ' + SPWX + ', SPWZ = ' + SPWZ + ';';
    console.log('[vb] SPAWN RESET [H] → paste this over line ~156 to BAKE it:   ' + spawnBake);
  }

