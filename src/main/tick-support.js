    // ── DEPTH OF FIELD: AUTOFOCUS ── the focal plane is whatever the crosshair is resting on, found with the
    // same exact DDA the tools aim with, and then RACKED rather than snapped. The easing runs in 1/d — dioptres,
    // which is what a focus ring actually turns through — so a rack onto something close is quick and one out to
    // the treeline is lazy, and a voxel of error at the far end (where the blur has saturated anyway) costs
    // nothing. Thin decor is stepped THROUGH for the same reason the aim ray steps through it: a grass strand a
    // metre away must not seize the focus and soften the whole valley behind it.
    if (!ED.on && CH * dofCocK >= 0.5) {                    // a strength of 0 (or so near it that the blit would skip every pixel anyway) is the same picture as the toggle being off — and this way it costs the aim ray nothing either
      const cpD = Math.cos(P.pitch);
      const dxD = Math.sin(P.yaw) * cpD, dyD = Math.sin(P.pitch), dzD = Math.cos(P.yaw) * cpD;
      const FMAX = Math.max(48, Math.min(renderDist, 320));          // past this the circle of confusion has saturated, so a longer march buys nothing
      let hitT = FMAX;
      voxRay(P.x, smoothEye, P.z, dxD, dyD, dzD, FMAX, (x, y, z, t) => {
        if (y < 0 || y >= WY) return 0;
        const vD = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
        if (!vD || floatTab[vD]) return 0;                           // air, or a strand/bloom/twig the eye reads straight past
        hitT = Math.max(1.5, t); return 1;                           // t is where the ray ENTERED that voxel — the surface itself
      });
      dofInv += (1 / hitT - dofInv) * (1 - Math.exp(-dt / DOF_RACK));
      UF[UF_DOF] = dofLock > 0 ? dofLock : 1 / Math.max(1e-4, dofInv);
      UF[UF_DOF + 2] = dofTapK;                                      // gather taps per pixel of radius (see the BLIT loop)
      UF[UF_DOF + 1] = CH * dofCocK;                                 // max circle of confusion in CANVAS pixels (the blit gathers in canvas space, not render space) — a fraction of the HEIGHT, so the strength setting means the same thing at every resolution
    } else { UF[UF_DOF] = 0; UF[UF_DOF + 1] = 0; }
                   // 0 = off: the composite writes a flat CoC and the blit's gather never runs
    UF[1268] = vigOn ? Math.max(BASE_VIG, cineBlurK * CINE_VIG) : 0;   // u.misc.x — BASE vignette in normal play, deepening to the cinematic value while the camera flies; max() so leaving cinema eases back to base instead of snapping to zero                   // u.misc.x — cinematic VIGNETTE depth (written unconditionally; the blit reads it every frame)
    // ── NOTHING IS LEFT HANGING ── the unified resolver, EVERY frame and time-sliced, driven by the cells
    // that ACTUALLY CHANGED. It costs nothing on a frame where nothing was carved, which is the overwhelming
    // majority of them — where the ambient sweep this replaces paid its full ~24 ms four times a second,
    // centred on the player, whether or not anything had been touched. That hitch class is simply gone.
    if (!ED.on) supFlush(false);
    if (!ED.on) orphDrain(1.0);                        // 1 ms/frame: a rect drains in a few seconds and never shows as a hitch
    if (!ED.on) autoPickup();                          // AUTO-PICKUP (user): a free hand slot vacuums up nearby floating items — no right-click, no aiming
    for (const dr of drops) {                          // ── THE ARROW ARRIVES ── the wound (or the carve) registers when it LANDS, not when it was loosed, so the blink and the chunk line up with the impact (user)
      if (!dr || dr.hitDone || dr.T === undefined) continue;
      if ((now - dr.born) / 1000 < dr.T) continue;
      const shaft8 = dr.kind === 'arrow' || dr.kind === 'spear';   // `drops` also carries items flying to the hand — those have no impact and must stay silent
      if (dr.hitBird !== undefined && dr.hitBird >= 0) { dr.hitDone = true;   // ── A BIRD OUT OF THE SKY ── one shaft is enough (user)
        if (shaft8) playArrowHit(dr.ex, dr.ey, dr.ez);
        playLifeHit(dr.ex, dr.ey, dr.ez);              // a flying bird lives in birds[], not the creature pool, so it never reaches hitCreature — it still got hit (user 2026-08-08)
        if (!birdShot(birds[dr.hitBird])) spawnHitSparks(dr.ex, dr.ey, dr.ez);
        dr.gone = true;                                // …and the shaft goes down WITH it: struck in mid-air, `stick` would leave it hanging in the sky (user)
        continue; }
      if (dr.hitSlot !== undefined && dr.hitSlot >= 0) { dr.hitDone = true;
        if (shaft8) playArrowHit(dr.ex, dr.ey, dr.ez);   // …and it lands with a thud (user)
        hitCreature(dr.hitSlot, (dr.kind || 'arrow') + dr.born);
        spawnHitSparks(dr.ex, dr.ey, dr.ez);           // …and it STRIKES: sparks off the impact (user)
        // ── THE SHAFT IS IN THE ANIMAL NOW (user 2026-08-05: "make the arrow disappear when the life
        // disappears") ── a stuck arrow used to outlive whatever it hit, so once the creature died or simply
        // walked out of range the shaft was left hanging in clear air where the animal had been. Remember the
        // slot AND the occupant: B.born is stamped fresh on every placement, so it doubles as a generation
        // number and a recycled slot can never be mistaken for the same animal still carrying the arrow.
        { const BH = wbf[dr.hitSlot]; if (BH) { dr.stuckSlot = dr.hitSlot; dr.stuckBorn = BH.born;
          // ── AND IT RIDES THE ANIMAL (user 2026-08-07: "make the arrow stick to the life as it moves") ── the
          // shaft used to be pinned to the world point where it struck, so a wounded animal walked out from
          // under it and left it hanging in clear air. Remember the impact point in the animal's OWN frame —
          // offset plus the heading it had at that instant — and the sweep below replays it every frame, so the
          // arrow stays in the same spot on the body and turns with it.
          const byS = (BH.kind | 0) === 5 ? (BH.perchFeet || 0) + 3 : BH.y;
          dr.stTh = BH.th || 0;
          const cS = Math.cos(-dr.stTh), sS = Math.sin(-dr.stTh), ox = dr.ex - BH.x, oz = dr.ez - BH.z;
          dr.stOx = ox * cS - oz * sS; dr.stOz = ox * sS + oz * cS;   // into the animal's frame once, so the replay is a single rotation back out
          dr.stOy = dr.ey - byS; } }
        continue; }
      if (!dr.chopAt) continue;
      dr.hitDone = true;
      if (shaft8) playArrowHit(dr.chopAt[0], dr.chopAt[1], dr.chopAt[2]);   // …into wood, rock or dirt: the same strike, wherever it stops (user)
      arrowChop(dr.chopAt[0], dr.chopAt[1], dr.chopAt[2]);
    }
    for (const dr of drops) {                          // ── AND IT GOES WITH IT ── an arrow stuck in a creature dies the moment that creature does
      if (!dr || dr.stuckSlot === undefined) continue;
      const BS = wbf[dr.stuckSlot];
      // !init covers every way life leaves: the death poof at the end of the blink (reapDeaths), a dusk/dawn
      // handover, and an ordinary recycle out at LIFE_KEEP. born !== stuckBorn catches the one case init misses,
      // a slot re-filled by a NEW animal in the same frame the old one left.
      if (!BS || !BS.init || BS.slain || BS.born !== dr.stuckBorn) { dr.gone = true; continue; }
      if (dr.stOx === undefined) continue;               // staked by a test tap rather than a real shot — nothing to replay
      const byR = (BS.kind | 0) === 5 ? (BS.perchFeet || 0) + 3 : BS.y;
      const thR = BS.th || 0, cR = Math.cos(thR), sR = Math.sin(thR);
      dr.ex = BS.x + (dr.stOx * cR - dr.stOz * sR);      // …back out of the animal's frame at its CURRENT heading
      dr.ez = BS.z + (dr.stOx * sR + dr.stOz * cR);
      dr.ey = byR + dr.stOy;
      dr.stDth = thR - dr.stTh;                          // how far the body has turned since — the shaft turns with it (see the STUCK render branch)
      dr.x = Math.round(dr.ex); dr.y = Math.round(dr.ey); dr.z = Math.round(dr.ez);   // the collection radius and every distance read work off these
    }
    for (let i = drops.length - 1; i >= 0; i--) if (drops[i] && drops[i].gone) drops.splice(i, 1);   // …swept up here
    for (let i = drops.length - 1; i >= 0; i--) {      // a TOSSED WORM converts to a LIVE crawler the moment its arc lands — it 'snaps back to the grid' and walks off like the others
      const dr = drops[i];
      if (!(WORM_NFRAMES && dr.it === WORM_ITEM0 && dr.T && (now - dr.born) / 1000 > dr.T + 0.24)) continue;
      let wi = 32, fd = -1;
      for (let j = WORM_0; j < WORM_END; j++) { const B2 = wbf[j];   // worm pool slots (WORM_N = 32 — DOUBLED AGAIN 2026-07-18)
        if (!B2.init) { wi = j; fd = -2; break; }
        const d2w = (B2.x - P.x) * (B2.x - P.x) + (B2.z - P.z) * (B2.z - P.z);
        if (d2w > fd) { fd = d2w; wi = j; }            // all live → reuse the farthest (least visible) slot
      }
      const B = wbf[wi];
      B.x = dr.ex + 0.5; B.z = dr.ez + 0.5;
      B.gRef = Math.max(hmap[gwrap(Math.floor(B.x), WX) + gwrap(Math.floor(B.z), WZ) * WX], WL);
      B.y = B.gRef + 2; B.th = Math.random() * 6.283; B.om = 0; B.omT = 0; B.tRe = 0; B.trap = 0;
      B.born = now; B.kind = 2; B.dieT = 0; B.glow = false; B.glowT = 0; B.rel = true; B.init = true;
      drops.splice(i, 1);
    }
    for (let di = 0; di < 4; di++) {                   // dropped items → camera space (voxel units), hovering + spinning (+ launch flight out of the hand)
      const o2 = 68 + di * 16;
      if (grabGhost && di === 0) {                     // ── THE FLIGHT ── a world object in the first drop slot, so the hand is never borrowed (user)
        const g = grabGhost, rx = g.x - cam[0], ry = g.y - cam[1], rz = g.z - cam[2];
        const a3 = g.ph + now * 0.004, ca3 = Math.cos(a3), sa3 = Math.sin(a3);
        const GX = [ca3, 0, sa3], GY = [sa3, 0, -ca3], GZ = [0, 1, 0];   // the ordinary drop frame, spinning as it comes
        UF[o2] = rx * right[0] + ry * right[1] + rz * right[2]; UF[o2 + 1] = rx * up[0] + ry * up[1] + rz * up[2]; UF[o2 + 2] = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]; UF[o2 + 3] = g.vs;
        UF[o2 + 4] = GX[0] * right[0] + GX[1] * right[1] + GX[2] * right[2]; UF[o2 + 5] = GX[0] * up[0] + GX[1] * up[1] + GX[2] * up[2]; UF[o2 + 6] = GX[0] * fwd[0] + GX[1] * fwd[1] + GX[2] * fwd[2];
        UF[o2 + 7] = g.it;
        UF[o2 + 8] = GY[0] * right[0] + GY[1] * right[1] + GY[2] * right[2]; UF[o2 + 9] = GY[0] * up[0] + GY[1] * up[1] + GY[2] * up[2]; UF[o2 + 10] = GY[0] * fwd[0] + GY[1] * fwd[1] + GY[2] * fwd[2]; UF[o2 + 11] = 0;
        UF[o2 + 12] = GZ[0] * right[0] + GZ[1] * right[1] + GZ[2] * right[2]; UF[o2 + 13] = GZ[0] * up[0] + GZ[1] * up[1] + GZ[2] * up[2]; UF[o2 + 14] = GZ[0] * fwd[0] + GZ[1] * fwd[1] + GZ[2] * fwd[2]; UF[o2 + 15] = 0;
        continue;
      }
      const dr = drops[grabGhost ? di - 1 : di];        // …the real drops shuffle along behind it for those 340 ms
      if (!dr) { UF[o2 + 7] = 0; continue; }
      // ── SETTLE ── after DROP_REST_MS the hover ends (user): the spin decays to a stop, the bob fades
      // out and the item lowers until its base sits on the ground. Eased over a second so it slows to a
      // halt. The spin is ACCUMULATED rather than derived from `now`, which is the only way to decelerate
      // it smoothly — scaling a now-based angle runs the item backwards as the factor falls.
      const rK = Math.min(1, Math.max(0, (now - dr.born - DROP_REST_MS) / DROP_REST_EASE));
      const rE = rK * rK * (3 - 2 * rK);
      dr.spin = (dr.spin === undefined ? dr.ph : dr.spin) + Math.min(64, now - (dr.spinT || now)) * 0.0012 * (1 - rE);
      dr.spinT = now;
      const restY = itemHalfH ? (itemHalfH[dr.it - 1] || 4.5) : 4.5;   // bottom on the ground: dr.y is the first AIR voxel of the column
      const a2 = dr.spin, ca2 = Math.cos(a2), sa2 = Math.sin(a2);
      let px3 = dr.x + 0.5, py3 = dr.y + (9.0 + (restY - 9.0) * rE) + Math.sin(now * 0.002 + dr.ph) * 1.3 * (1 - rE), pz3 = dr.z + 0.5;   // anchor 9 up while it hovers: the axe (half-height 4.5) bottoms out >= 3 voxels clear of the ground
      // The BOW's art has its +z on the UNDERSIDE — the held pose (pitch ≈ +90°) turns that face downward,
      // so a drop that maps +z to world up shows it upside down (user). Flip the rest frame for the strip.
      const flipUp = !!(BOW_IT && BOW_NOCK && dr.it >= BOW_IT && dr.it < BOW_NOCK + BOW_FRAMES);
      let Xw = [ca2, 0, sa2], Yw = flipUp ? [-sa2, 0, ca2] : [sa2, 0, -ca2], Zw = flipUp ? [0, -1, 0] : [0, 1, 0];   // RIGHT-handed (Y = Z×X) either way — an improper frame here turns into a bogus quat mid-flight
      const tE = (now - dr.born) / 1000;
      const spinQ0 = m2q(Xw, Yw, Zw);
      // ── POINT-FIRST FLIGHT ── an arrow is not a tumbling drop: its nose tracks the VELOCITY the whole
      // way down the arc, so the stone head leads and the fletching trails (user). The art runs tip →
      // fletching along local +y, so -y is what must face the direction of travel.
      const aimAxes = (t) => {
        const vyN = dr.vy + TOSS_G * t, L = Math.hypot(dr.vx, vyN, dr.vz) || 1;
        const F = [dr.vx / L, vyN / L, dr.vz / L];      // where it is going, right now
        const U = Math.abs(F[1]) > 0.98 ? [1, 0, 0] : [0, 1, 0];   // a straight-up shot needs a different reference or the cross product collapses
        const cross = (a, b2) => { const c = [a[1] * b2[2] - a[2] * b2[1], a[2] * b2[0] - a[0] * b2[2], a[0] * b2[1] - a[1] * b2[0]];
          const l = Math.hypot(c[0], c[1], c[2]) || 1; return [c[0] / l, c[1] / l, c[2] / l]; };
        // THE TWO SHAFTS ARE MODELLED ALONG DIFFERENT AXES: the arrow runs tip→fletching along its y, so -y
        // leads; the spear runs butt→stone-head along its z, so +z leads. Flying one by the other's rule is
        // what had the spear leave the hand nose-down (user).
        if (dr.kind === 'spear') { const Z = F, X = cross(U, Z); return [X, cross(Z, X), Z]; }
        const Y = [-F[0], -F[1], -F[2]], X0 = cross(U, Y), Z0 = cross(X0, Y);   // Z = X×Y keeps the frame right-handed (Y = Z×X)
        // ── ARROW ROLL (user 2026-08-06: “tumble through the air… spin like a plane”) ── a fletched arrow spins about its OWN shaft, which is Y here.
        // Rolling X and Z together about Y leaves the nose exactly on the velocity vector, so flight, aim and the stick-on-impact test are all unchanged;
        // only the roll about the shaft moves. The SPEAR is excluded above and keeps its thrown attitude. The STUCK branch calls this with dr.T, so a
        // buried arrow freezes at the roll it struck with rather than snapping upright.
        const rl = t * ARROW_ROLL, cr9 = Math.cos(rl), sr9 = Math.sin(rl);
        return [[X0[0] * cr9 + Z0[0] * sr9, X0[1] * cr9 + Z0[1] * sr9, X0[2] * cr9 + Z0[2] * sr9], Y,
                [Z0[0] * cr9 - X0[0] * sr9, Z0[1] * cr9 - X0[1] * sr9, Z0[2] * cr9 - X0[2] * sr9]];
      };
      if (dr.stick && dr.T && tE >= dr.T) {            // ── STUCK ── it is buried in whatever it hit: no hover, no bob, no spin, and it keeps the attitude it struck at (user)
        px3 = dr.ex; py3 = dr.ey; pz3 = dr.ez;
        const A = aimAxes(dr.T);
        if (dr.stDth) {                                  // planted in something that has turned since — carry the shaft round with it about world up
          const cD = Math.cos(dr.stDth), sD = Math.sin(dr.stDth);
          for (const v9 of A) { const ax = v9[0], az = v9[2]; v9[0] = ax * cD - az * sD; v9[2] = ax * sD + az * cD; }
        }
        Xw = A[0]; Yw = A[1]; Zw = A[2];
      } else if (dr.T && tE < dr.T + 0.22) {
        const spinQ = spinQ0;
        if (tE < dr.T) {                               // ballistic TOSS: exact parabola from the hand — NO tumble, it keeps its hand
          const k = tE / dr.T;                         // orientation and eases UPRIGHT into the hover pose (user: no wobbling)
          px3 = dr.sx + dr.vx * tE; py3 = dr.sy + dr.vy * tE + 0.5 * TOSS_G * tE * tE; pz3 = dr.sz + dr.vz * tE;
          const M = dr.aim ? aimAxes(tE) : q2m(qslerp(dr.q0, spinQ, k * k * (3 - 2 * k)));
          Xw = M[0]; Yw = M[1]; Zw = M[2];
        } else {                                       // touchdown: ease the arc endpoint into the hover bob
          const e2 = (tE - dr.T) / 0.22, e3 = e2 * e2 * (3 - 2 * e2);
          px3 = dr.ex + (px3 - dr.ex) * e3; py3 = dr.ey + (py3 - dr.ey) * e3; pz3 = dr.ez + (pz3 - dr.ez) * e3;
          if (dr.aim) { const A = aimAxes(dr.T), M = q2m(qslerp(m2q(A[0], A[1], A[2]), spinQ, e3));   // …and the nose rotates out of its flight attitude only once it has landed
            Xw = M[0]; Yw = M[1]; Zw = M[2]; }
        }
      }
      const rx = px3 - cam[0], ry = py3 - cam[1], rz = pz3 - cam[2];
      UF[o2] = rx * right[0] + ry * right[1] + rz * right[2]; UF[o2 + 1] = rx * up[0] + ry * up[1] + rz * up[2]; UF[o2 + 2] = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]; UF[o2 + 3] = 1.0;
      UF[o2 + 4] = Xw[0] * right[0] + Xw[1] * right[1] + Xw[2] * right[2]; UF[o2 + 5] = Xw[0] * up[0] + Xw[1] * up[1] + Xw[2] * up[2]; UF[o2 + 6] = Xw[0] * fwd[0] + Xw[1] * fwd[1] + Xw[2] * fwd[2];
      UF[o2 + 7] = (WORM_NFRAMES && dr.it >= WORM_ITEM0 && dr.it < WORM_ITEM0 + WORM_NFRAMES) ? WORM_ITEM0 + Math.floor(now * 0.024) % WORM_NFRAMES : dr.it;   // a tossed worm keeps squiggling on the ground too
      UF[o2 + 8] = Yw[0] * right[0] + Yw[1] * right[1] + Yw[2] * right[2]; UF[o2 + 9] = Yw[0] * up[0] + Yw[1] * up[1] + Yw[2] * up[2]; UF[o2 + 10] = Yw[0] * fwd[0] + Yw[1] * fwd[1] + Yw[2] * fwd[2]; UF[o2 + 11] = 0;
      UF[o2 + 12] = Zw[0] * right[0] + Zw[1] * right[1] + Zw[2] * right[2]; UF[o2 + 13] = Zw[0] * up[0] + Zw[1] * up[1] + Zw[2] * up[2]; UF[o2 + 14] = Zw[0] * fwd[0] + Zw[1] * fwd[1] + Zw[2] * fwd[2]; UF[o2 + 15] = 0;
    }
    if (CPROF) cpMark(4);
    lifeUid.fill(-1); lifeAna.fill(1);                 // ── dynamic-life ledger reset ── slots not claimed this frame stay analytic-only/empty
    { const bSlot = 4, o2 = dropOff(bSlot);             // ── flying cardinal → drop slot 4 ── off-grid DDA model, free wandering flight (no home; never follows the player). ONE slot number, stitched to its float offset by the one function that knows how: the raw UF writes below want the offset, birdWrite wants the INDEX
      if (ED.on) {                                       // ── EDITOR STAGE ── advance the animation; the model is ALWAYS GRID-STAMPED (real world voxels → full lighting), exactly like every other model — NO trace-inject exception (user)
        UF[o2 + 7] = 0; for (const B2 of birdBoxes) B2.active = false;   // item id 0 hides the drop slot (no trace-inject in the editor)
        if (ED.frames.length && !ED.paused && ED.bun) {  // ── BEHAVIOR state machine: wander (75% HOP / 25% ROTATE 50-50) at 24 fps; FLEE the player at 48 fps when near. Whole-hop-per-action INTEGER position, baked rotation (ED.spin=0). ──
          const B = ED.bun, nJ = B.nJ, block = nJ * 3, HOP_END = 6;
          const rot2 = (ox, oz, h) => { let x = ox, z = oz; for (let k = 0; k < (h & 3); k++) { const nx = -z, nz = x; x = nx; z = nz; } return [x, z]; };
          const DIR = [[0, -1], [1, 0], [0, 1], [-1, 0]];   // hop/facing direction per heading h (world x,z) — matches rot2/yrot
          const bcx = ED.box ? ED.box.cx : (ED.x0 + (ED.pw >> 1) + B.px), bcz = ED.box ? ED.box.cz : (ED.z0 + (ED.pd >> 1) + B.pz);   // bunny world centre
          const adx = bcx - P.x, adz = bcz - P.z, d2 = adx * adx + adz * adz;   // vector pointing AWAY from the player (horizontal)
          B.fleeing = d2 < (B.fleeing ? 55 * 55 : 40 * 40);   // enter flee within 40 voxels; keep fleeing until >55 (hysteresis → no fps flip-flop)
          const targetFps = B.fleeing ? 48 : 24, CYCLE_FRAMES = nJ + 6;   // FLEE = 48 fps (user). FRAME-based clock (11 anim + 6 pause frames): fps only scales the advance SPEED, never jumps the current frame.
          B.fps += (targetFps - B.fps) * Math.min(1, dt * 6);   // EASE the speed toward the target (τ≈0.17s) so 24↔48 ramps smoothly — no abrupt speed jerk at the transition (user)
          B.animT += dt * B.fps;                         // B.animT is a continuous FRAME position (not ms); B.fps scales how fast it advances
          if (B.animT >= CYCLE_FRAMES) { B.animT -= CYCLE_FRAMES;   // one action finished → apply its effect, then roll the next
            const MX = Math.max(2, (ED.pw >> 1) - 10), MZ = Math.max(2, (ED.pd >> 1) - 10);   // stage bounds (margin covers footprint + one hop's lunge)
            const blocked = (h) => { const [dx, dz] = rot2(0, -HOP_END, h); const nx = B.px + dx, nz = B.pz + dz; return nx < -MX || nx > MX || nz < -MZ || nz > MZ; };   // would a hop at heading h cross the invisible barrier?
            if (B.action === 0) { const [dx, dz] = rot2(0, -HOP_END, B.h); B.px += dx; B.pz += dz;   // HOP: advance one whole hop in the facing direction
              B.px = Math.max(-MX, Math.min(MX, B.px)); B.pz = Math.max(-MZ, Math.min(MZ, B.pz)); }   // …clamped (safety; the guard below normally stops a blocked hop being chosen)
            else if (B.action === 1) B.h = (B.h + 1) & 3;   // ROTATE-LEFT  → heading +1
            else B.h = (B.h + 3) & 3;                       // ROTATE-RIGHT → heading −1
            if (B.fleeing) {                                // FLEE: face directly AWAY from the player (the heading whose hop moves most away), then dart
              let bestH = 0, bestDot = -1e9;
              for (let h = 0; h < 4; h++) { const dot = DIR[h][0] * adx + DIR[h][1] * adz; if (dot > bestDot) { bestDot = dot; bestH = h; } }
              const diff = (bestH - B.h + 4) & 3;           // 0 = already facing away → hop; 1 = one left; 3 = one right; 2 = 180° (turn left, finish next cycle)
              B.action = diff === 0 ? 0 : diff === 3 ? 2 : 1;
            } else B.action = Math.random() < 0.75 ? 0 : (Math.random() < 0.5 ? 1 : 2);   // WANDER: 75% hop, else a rotate split 50/50 left/right
            if (B.action === 0 && blocked(B.h)) {           // BARRIER GUARD (user): never hop INTO the barrier — rotate toward the most open heading instead so it never stalls against the edge
              let goalH = -1, best = -1e9;
              for (let h = 0; h < 4; h++) { if (blocked(h)) continue; const s = -DIR[h][0] * B.px - DIR[h][1] * B.pz; if (s > best) { best = s; goalH = h; } }   // most open = hop stays in bounds AND heads back toward the stage centre
              B.action = (goalH >= 0 && ((goalH - B.h + 4) & 3) === 3) ? 2 : 1;   // turn ONE step toward it (right if it's CW from here, else left)
            }
          }
          const af = Math.min(nJ - 1, Math.floor(B.animT));   // frame index within the action (0..nJ-1), held at the last frame during the pause tail
          const sel = (B.h & 3) * block + B.action * nJ + af;
          const blink = (now % 3400) < 160;
          if (sel !== ED.sel || blink !== ED.blink || B.px !== ED.bhx || B.pz !== ED.bhz) {
            ED.sel = sel; ED.blink = blink; ED.spin = 0; ED.bhx = B.px; ED.bhz = B.pz;
            ED.hopX = B.px; ED.hopY = 0; ED.hopZ = B.pz; ED.hop2X = ED.hop2Y = ED.hop2Z = 0;
            edLayout(); }
        }
        else if (ED.frames.length && !ED.paused && ED.arm) {   // ── ARMADILLO (user): walks around the stage, turning ONLY 90° — moves in perpendicular (cardinal) directions, never diagonally ──
          const A = ED.arm, n = ED.frames.length, frameMs = 1000 / (ED.name1 === 'skunk' ? 12 : 24);   // SKUNK previews at its true 12 fps ship rate so the editor stage matches the pine-forest skunk (armadillo stays 24)
          const DIR = [[0, -1], [1, 0], [0, 1], [-1, 0]];   // cardinal march per heading; ED.spin faces the same way (the old KeyH frozen-heading alignment mode is gone — headings auto-derive)
          const acx = ED.x0 + (ED.pw >> 1) + A.px, acz = ED.z0 + (ED.pd >> 1) + A.pz;   // skunk's world centre on the stage
          const adx = acx - P.x, adz = acz - P.z, ad2 = adx * adx + adz * adz;          // vector pointing AWAY from the player (horizontal)
          A.flee = (ED.name1 === 'skunk' || ED.name1 === 'porcupine') && ad2 < (A.flee ? 46 * 46 : 30 * 30);          // player in the vicinity → FLEE (skunk + porcupine; same 30/46 hysteresis as the world creature, like the editor bunny)
          const edTgt = ED.name1 === 'skunk' ? (A.flee ? 48 : 24) : (ED.name1 === 'porcupine' ? (A.flee ? 18 : 9) : 9);   // MOTION on flee: skunk 24→48, PORCUPINE 9→18 — DOUBLE the pace when the player is near (user); armadillo/others constant 9
          A.spd = (A.spd === undefined) ? edTgt : A.spd + (edTgt - A.spd) * Math.min(1, dt * 6);   // ease the pace toward the target so the speed-up/-down ramps (bunny-style), no snap
          A.px += DIR[A.hd][0] * A.spd * dt; A.pz += DIR[A.hd][1] * A.spd * dt;   // march forward across the stage
          const MX = Math.max(4, (ED.pw >> 1) - 8), MZ = Math.max(4, (ED.pd >> 1) - 8);
          if (A.px < -MX || A.px > MX || A.pz < -MZ || A.pz > MZ) {   // reached a stage edge → clamp + make a 90° turn (perpendicular) back onto the stage
            A.px = Math.max(-MX, Math.min(MX, A.px)); A.pz = Math.max(-MZ, Math.min(MZ, A.pz));
            A.hd = (A.hd + (Math.random() < 0.5 ? 1 : 3)) & 3; A.tRe = now + 1600 + Math.random() * 2600;
          } else if (A.flee) {                              // FLEE: face the cardinal that moves most AWAY from the player, then dart (like the editor bunny)
            let bestH = A.hd, bestDot = -1e9;
            for (let h = 0; h < 4; h++) { const dot = DIR[h][0] * adx + DIR[h][1] * adz; if (dot > bestDot) { bestDot = dot; bestH = h; } }
            if (bestH !== (A.hd & 3) && now > A.tRe) { A.hd = bestH; A.tRe = now + 400; }   // steer away, cooldown so it doesn't jitter-turn every frame
          } else if (now > A.tRe) { A.tRe = now + 1600 + Math.random() * 2600; if (Math.random() < 0.45) A.hd = (A.hd + (Math.random() < 0.5 ? 1 : 3)) & 3; }   // occasional random 90° turn
          if (ED.name1 === 'skunk' || ED.name1 === 'porcupine') { const fps = (A.flee ? 24 : 12) * (ED.name1 === 'skunk' ? SKUNK_ANIM_MUL : 1); A.afps = (A.afps === undefined) ? fps : A.afps + (fps - A.afps) * Math.min(1, dt * 6); A.aframe = (A.aframe || 0) + dt * A.afps; }   // SKUNK + PORCUPINE (user): ease 12↔24 fps (double when fleeing) on a frame-position clock — matches the world creature, no frame jump   // skunk at HALF rate here too, so the editor preview matches the world (user 2026-08-06)
          const fi = ((ED.name1 === 'skunk' || ED.name1 === 'porcupine') ? Math.floor(A.aframe) : Math.floor(now / frameMs)) % n, blink = (now % 3400) < 160, hx = Math.round(A.px), hz = Math.round(A.pz);
          if (fi !== (((ED.sel % n) + n) % n) || blink !== ED.blink || hx !== ED.hopX || hz !== ED.hopZ || (A.hd & 3) !== ED.spin) {
            ED.sel = fi; ED.blink = blink; ED.spin = A.hd & 3;   // face the march direction (cardinal)
            ED.hopX = hx; ED.hopY = 0; ED.hopZ = hz; ED.hop2X = ED.hop2Y = ED.hop2Z = 0;
            edLayout(); }
        }
        else if (ED.frames.length && !ED.paused) {       // ── playing → 24 fps + continuous forward march (manual .vox imports) ──
          const n = ED.frames.length, frameMs = 1000 / 24, pauseMs = 600, cyc = n * frameMs + pauseMs;
          const t = now % cyc, fi = t < n * frameMs ? Math.floor(t / frameMs) : n - 1;
          const blink = (now % 3400) < 160;
          if (fi !== (((ED.sel % n) + n) % n) || blink !== ED.blink) { ED.sel = fi; ED.blink = blink; ED.spin = 0;
            const k = Math.floor(now / cyc);
            const lf = ED.frames[n - 1] || {}, f0 = ED.frames[0] || {};
            ED.hopX = k * ((lf.ox || 0) - (f0.ox || 0)); ED.hopY = k * ((lf.oy || 0) - (f0.oy || 0)); ED.hopZ = k * ((lf.oz || 0) - (f0.oz || 0));
            const n2 = ED.frames2.length, l2 = n2 ? ED.frames2[n2 - 1] : {}, g2 = n2 ? ED.frames2[0] : {};
            ED.hop2X = k * ((l2.ox || 0) - (g2.ox || 0)); ED.hop2Y = k * ((l2.oy || 0) - (g2.oy || 0)); ED.hop2Z = k * ((l2.oz || 0) - (g2.oz || 0));
            edLayout(); }
        }
        if (ED.box) { birdBox.cx = ED.box.cx; birdBox.cy = ED.box.cy; birdBox.cz = ED.box.cz; birdBox.hx = ED.box.hx; birdBox.hy = ED.box.hy; birdBox.hz = ED.box.hz; birdBox.active = true; }   // SOLID hitbox for the EDITABLE bunny — republished every frame from the last edLayout AABB; boxFree() already tests every birdBox
        if (ED.box2) { const B3 = birdBoxes[1]; B3.cx = ED.box2.cx; B3.cy = ED.box2.cy; B3.cz = ED.box2.cz; B3.hx = ED.box2.hx; B3.hy = ED.box2.hy; B3.hz = ED.box2.hz; B3.active = true; }   // …and the PREVIEW bunny (slot 1) so you can't walk through either
      } else if (FLYERS.length > 0 && !birdRagTick(bird)) {   // the WORLD bird (only when NOT in the editor) — and not while its corpse is falling
        const ps = birdStep(bird, 0, now / 1000, dt);    // bird 0 keeps the dedicated slot; the rest are emitted with the creatures below
        // ── NO LEGAL SKY (the desert) ── birdStep returns null when its whole respawn ring is sand. Written as
        // an if/else and NOT as an early `return`: this fragment is the BODY of tickBody() (main/tick-body.js
        // opens the function, main/tick-passes.js closes it), so a `return` here would abandon the rest of the
        // frame — support, nav, life, creatures, emit and every render pass — for one bird.
        if (!ps) { UF[o2 + 7] = 0; birdBox.active = false; }
        else {
          birdPose(bird, ps);                            // …the pose the ragdoll will be rebuilt from
          ps.uid = 0;                                    // stable identity for the dynamic-life temporal reprojection
          birdWrite(bSlot, ps, cam, right, up, fwd);     // the SLOT INDEX, never o2: birdWrite derives the offset itself, so passing 132 wrote dropOff(132) — deep inside lifeMotB — and left drop slot 4's item id at 0, i.e. bird 0 flew invisible while still solid and still shootable (tick-life.js's call site already passed dropCursor; this one was missed when the signature changed)
          birdHit(birdBox, ps);
        }
      } else { UF[o2 + 7] = 0; birdBox.active = false; }
    }
