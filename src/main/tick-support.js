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
    for (const dr of drops) {                          // ── AND A SHAFT IN A FELLED TREE RIDES IT DOWN (user 2026-08-28) ── sim/physics.js's phRideBody attached it at the moment the body was built; this is the replay, and it is the rigid-body twin of the creature sweep above
      if (!dr || !dr.rideB) continue;
      const RB = dr.rideB;
      if (PH.bodies.indexOf(RB) < 0) {                 // the body is gone — absorbed, expired, or broken into chunks
        // ── ONE FRAME OF GRACE ── a body that SHATTERS is spliced out and its children built inside the same
        // tick, but not necessarily before this sweep runs, and reaping on the first miss would delete a shaft
        // that is about to be re-attached to the chunk it is actually in. Two consecutive misses means nothing
        // claimed it, and then it goes the way an arrow in a dead animal goes (user 2026-08-05).
        if (++dr.rideMiss < 2) continue;
        dr.rideB = null; dr.rideM = null; dr.gone = true; continue; }
      dr.rideMiss = 0;
      dr.ex = RB.pos[0] + dr.rOx * RB.ax[0] + dr.rOy * RB.ay[0] + dr.rOz * RB.az[0];   // back out of the body's frame at its CURRENT attitude
      dr.ey = RB.pos[1] + dr.rOx * RB.ax[1] + dr.rOy * RB.ay[1] + dr.rOz * RB.az[1];
      dr.ez = RB.pos[2] + dr.rOx * RB.ax[2] + dr.rOy * RB.ay[2] + dr.rOz * RB.az[2];
      // …and the ATTITUDE turns with it. D = R_now^T · R_attach maps a vector that was fixed in the world when
      // the shaft attached to where that vector points now, which is exactly what the shaft's three axes want.
      // A full 3x3 and not stDth's yaw: a trunk topples about a HORIZONTAL axis, so a yaw-only carry would keep
      // the arrow pointing level while the tree it is buried in lies on its side.
      const M = dr.rideM || (dr.rideM = new Float32Array(9));
      for (let r9 = 0; r9 < 3; r9++) for (let c9 = 0; c9 < 3; c9++)
        M[r9 * 3 + c9] = RB.ax[r9] * dr.rAx[c9] + RB.ay[r9] * dr.rAy[c9] + RB.az[r9] * dr.rAz[c9];
      dr.x = Math.round(dr.ex); dr.y = Math.round(dr.ey); dr.z = Math.round(dr.ez);   // the collection radius and every distance read work off these, exactly as the creature sweep leaves them
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
    for (let di = 0; di < 8; di++) {                   // dropped items → camera space (voxel units), hovering + spinning (+ launch flight out of the hand)
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
      const restY = dropRestY(dr.it);                  // bottom on the ground: dr.y is the first AIR voxel of the column   // …and an UPRIGHT drop rests on its length, not its height — see dropRestY in assets/held-items.js
      const a2 = dr.spin, ca2 = Math.cos(a2), sa2 = Math.sin(a2);
      let px3 = dr.x + 0.5, py3 = dr.y + (9.0 + (restY - 9.0) * rE) + Math.sin(now * 0.002 + dr.ph) * 1.3 * (1 - rE), pz3 = dr.z + 0.5;   // anchor 9 up while it hovers: the axe (half-height 4.5) bottoms out >= 3 voxels clear of the ground
      // The BOW's art has its +z on the UNDERSIDE — the held pose (pitch ≈ +90°) turns that face downward,
      // so a drop that maps +z to world up shows it upside down (user). Flip the rest frame for the strip.
      const flipUp = !!(BOW_IT && BOW_NOCK && dr.it >= BOW_IT && dr.it < BOW_NOCK + BOW_FRAMES);
      let Xw = [ca2, 0, sa2], Yw = flipUp ? [-sa2, 0, ca2] : [sa2, 0, -ca2], Zw = flipUp ? [0, -1, 0] : [0, 1, 0];   // RIGHT-handed (Y = Z×X) either way — an improper frame here turns into a bogus quat mid-flight
      // ── AN ARROW COMES TO REST STANDING, STONE HEAD UP (user 2026-08-20) ── the frame above lays a model's
      // local z along world up, which is right for an axe or an apple and wrong for a shaft: the arrow runs
      // tip -> fletching along local +y (see the POINT-FIRST note below), so that frame put its length flat on
      // the ground. Mapping local -y to world up stands it on its fletching with the stone leading skyward.
      // X stays the spinning horizontal axis so the settle still turns it, and Z is chosen to keep the frame
      // RIGHT-handed: Z x X = [sa,0,-ca] x [ca,0,sa] = [0,-1,0] = Y, which is the same Y = Z×X test the line
      // above is written to satisfy. An improper frame here becomes a bogus quaternion, not a visible mistake.
      if (dropUpright(dr.it)) { Xw = [ca2, 0, sa2]; Yw = [0, -1, 0]; Zw = [sa2, 0, -ca2]; }
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
        if (dr.rideM) {                                  // planted in a RIGID BODY that has moved since — the full delta rotation, so the shaft tumbles with a falling trunk instead of staying level in it
          const M9 = dr.rideM;
          for (const v9 of A) { const a0 = v9[0], a1 = v9[1], a2 = v9[2];
            v9[0] = M9[0] * a0 + M9[1] * a1 + M9[2] * a2;
            v9[1] = M9[3] * a0 + M9[4] * a1 + M9[5] * a2;
            v9[2] = M9[6] * a0 + M9[7] * a1 + M9[8] * a2; }
        } else if (dr.stDth) {                           // planted in a CREATURE that has turned since — carry the shaft round with it about world up
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
    { const bSlot = 8, o2 = dropOff(bSlot);             // ── flying cardinal → drop slot 8 (was 4, moved when the item-drop band grew to 8) ── off-grid DDA model, free wandering flight (no home; never follows the player). ONE slot number, stitched to its float offset by the one function that knows how: the raw UF writes below want the offset, birdWrite wants the INDEX
      if (ED.on) {                                       // ── EDITOR STAGE ── advance the animation; the model is ALWAYS GRID-STAMPED (real world voxels → full lighting), exactly like every other model — NO trace-inject exception (user)
        for (const B2 of birdBoxes) B2.active = false;
        UF[o2 + 7] = 0;   // the cardinal's slot stays empty on the stage; the editor's free-moving exhibits ride the live-creature band instead (main/tick-emit.js)
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
            ED.sel = sel; ED.sel2 = sel; ED.blink = blink; ED.spin = 0; ED.bhx = B.px; ED.bhz = B.pz;
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
            ED.sel = fi; ED.sel2 = fi; ED.blink = blink; ED.spin = A.hd & 3;   // face the march direction (cardinal); sel2 tracks sel here because these branches drive ONE model and any compare variant beside it is the same animation at the same length
            ED.hopX = hx; ED.hopY = 0; ED.hopZ = hz; ED.hop2X = ED.hop2Y = ED.hop2Z = 0;
            edLayout(); }
        }
        else if (ED.frames.length && ED.paused) { ED.playT0 = 0; ED.mixT0 = 0;   // PARKED for alignment: drop the run's clock so resuming starts a fresh run from the base position rather than dropping the model back into the middle of a field it cannot be aligned in
          if (ED.blinkE || ED.blink) { ED.blinkE = 0; ED.blink = false; edLayout(); } }   // …and open the eyes: a model parked mid-wink would be aligned against a lid, and nothing else repaints while paused
        else if (ED.frames.length && !ED.paused) {       // ── playing → 24 fps + continuous forward march (manual .vox imports) ──
          // ── THE FLAMINGO LOOPS WITHOUT THE TAIL (user 2026-08-18: "have the flamingo cycle through the first
          // 10 frames. repeatedly.") ── every other import gets a 600 ms hold on its last frame, which suits a
          // one-shot action like a jump: it reads as the pose landing before the cycle restarts. A WALK has no
          // such beat, and on the flamingo the hold looked like the bird stopping dead once a second. Zero
          // pause for it, so the ten frames run straight into each other at a flat 24 fps.
          // FLAMINGO AT 12 fps (user 2026-08-18: "play the flamingo at 12 fps in the editor") — the same
          // arrangement the skunk already has on the walking branch above, and for the same reason: the editor
          // should preview a creature at the rate it actually ships at, not at the 24 fps house default.
          // ── THE CLOCK STARTS WHEN THE RUN DOES (user 2026-08-21: "play it in a cycle, moving forward … just make
          // sure the jumping animation has the correct positionings") ── `now` is the page clock, so a cycle
          // counter taken straight off it is however many cycles have elapsed since LOAD, not since the model
          // started playing. That is the real reason the marching frog was wrong the moment its hop was baked:
          // opening the editor nine cycles into the page put the first hop 90 voxels downrange, and it had
          // wrapped off the far side of the stage before anybody saw the first frame. Anchoring the clock at the
          // start of the run makes cycle 0 the first cycle, so the run begins at the base position, under the
          // camera edEnter framed, and every hop after it continues from where the last one landed.
          // The anchor is cleared on PAUSE (just above this branch), so aligning a frame and resuming starts a
          // fresh run from the base rather than resuming mid-field.
          if (!ED.playT0) ED.playT0 = now;
          const tt = now - ED.playT0;
          edExStep(dt);
          // ── AND NOTHING HOLDS ON ITS LAST FRAME ANY MORE (user 2026-08-22: "when I drag in a animation
          // sequence. have it play the frames continously. right now its plays till the end pauses, then plays
          // the frames again") ── this was 600 ms for everything except the flamingo. The hold was written for
          // a ONE-SHOT action, where the beat reads as the pose landing before the cycle restarts, and the
          // flamingo was special-cased to 0 because a walk has no such beat. Now that the stage opens empty and
          // every model arrives by drag-and-drop, a walk is the common case and the one-shot is the exception —
          // so the exception is what needs asking for, not the loop.
          const frameMs = 1000 / (ED.name1 === 'flamingo' ? 12 : 24), pauseMs = 0;
          // ── A WEIGHTED PLAYLIST ON THE EDITABLE LANE (user 2026-08-22: "jump = 50%, ribbet = 40%,
          // tongue = 10%") ── ED.mix holds the whole cycles the model may play and their weights; at every
          // cycle boundary the lane draws its next one and plays it whole. An EMPTY mix is every other load
          // there has ever been — one sequence, looping — and takes none of this.
          // The clock cannot be `tt % cyc` while mixing: the cycles are different lengths (hop 17 frames,
          // ribbet 14, tongue 24), so there is no one modulus. ED.mixT0 is when the live cycle began and
          // advances by that cycle's own length, which keeps the boundaries exact rather than accumulating
          // float drift off `now`.
          let mixSwap = false;
          if (ED.mix.length) {
            if (!ED.mixT0) ED.mixT0 = now;              // first frame of the run, and after a pause dropped the clock
            // ── THE CROSSING WRAPS AT A CYCLE BOUNDARY, NOT WHEREVER THE MODEL HAPPENS TO BE (user 2026-08-22:
            // "when it reaches the wall, then gets teleported to the other side, the positionings are not
            // correct anymore") ── the border crossing was being done by edLayout's wrapIn, which folds the
            // TOTAL position: base + this frame's own offset + the accumulator. Two things follow from that and
            // both are wrong. It fires mid-leap — measured, the frog went from −116 to +117 with the accumulator
            // unchanged at −110, i.e. it teleported partway through a hop. And wrapIn's period is
            // `ED.pd - rv.sy`, the CURRENT frame's rotated footprint, which the hop changes as the frog stretches
            // (9x7 to 7x10): consecutive frames straddling the boundary fold by different amounts, so the arc
            // itself comes apart on the far side. That is the "positionings are not correct anymore".
            // So the ACCUMULATOR wraps instead, and only here — at a cycle boundary, where the frog is grounded
            // between leaps. The margin keeps the per-frame total (up to 10 more in a leap) inside wrapIn's
            // range, so wrapIn stays the safety net it was and never fires on the stage's own model.
            const halfRun = Math.max(8, (Math.min(ED.pw, ED.pd) >> 1) - 14), fullRun = halfRun * 2;
            // A `for`, not an `if`: a stalled tab or a long frame can leave several cycles' worth of time on
            // the clock, and stepping one entry per tick would play them in slow motion until it caught up.
            // Bounded so a wildly stale clock cannot spin here — past that it simply re-anchors.
            for (let g = 0; ; g++) {
              const nn = ED.frames.length, cy = nn * frameMs + pauseMs;
              if (now - ED.mixT0 < cy) break;
              if (g >= 8) { ED.mixT0 = now; break; }
              // ── BANK THE FINISHED CYCLE'S TRAVEL ── each cycle contributes its OWN last-minus-first offset,
              // so a croak or a tongue-flick (delta 0) leaves the frog where it stands and only a leap moves
              // it. An ACCUMULATOR rather than the cycle-count × delta used below: with a mix there is no
              // single delta to multiply, and the number of leaps so far is not the number of cycles so far.
              const lf = ED.frames[nn - 1] || {}, f0 = ED.frames[0] || {};
              let ax = ED.hopX + ((lf.ox || 0) - (f0.ox || 0)), ay = ED.hopY + ((lf.oy || 0) - (f0.oy || 0)), az = ED.hopZ + ((lf.oz || 0) - (f0.oz || 0));
              if (ax > halfRun) ax -= fullRun; else if (ax < -halfRun) ax += fullRun;   // off one edge of the platform and straight back on at the other, which is what "goes to the border and disappears to the other side" means
              if (az > halfRun) az -= fullRun; else if (az < -halfRun) az += fullRun;
              if (Math.abs(ay) > 48) { ay = 0; }   // …but a model may NOT climb the sky one cycle at a time: vertical drift is a fault, not a crossing, so it is reset rather than wrapped
              ED.hopX = ax; ED.hopY = ay; ED.hopZ = az;
              ED.mixT0 += cy;
              edMixPick();
              mixSwap = true;
            }
          }
          const n = ED.frames.length, cyc = n * frameMs + pauseMs;
          const t = ED.mix.length ? (now - ED.mixT0) : (tt % cyc);
          const fi = t < n * frameMs ? Math.floor(t / frameMs) : n - 1;
          // ── THE SIDE LANE RUNS ITS OWN CLOCK ── it used to be indexed straight off `fi`, which is only
          // correct while both lanes hold animations of the SAME length: true for the two bunny variants this
          // was written for, false for two builds out of one scene-graph file (tongue 24 frames against the
          // concatenated croak-and-leap's 31). Driven off a 24-frame clock the longer one would play frames
          // 0-23 and then sit on 23 for the hold, so the frog would crouch into its leap and never land.
          // Same frame rate and same hold, its own length: two animations that do not divide simply drift
          // apart, which is what two independent exhibits should do.
          const n2c = ED.frames2.length, frameMs2 = 1000 / (ED.name2 === 'flamingo' ? 12 : 24);
          // NO END-OF-CYCLE HOLD FOR A FLYER. The 600 ms beat suits a one-shot action — it reads as the pose
          // landing before the cycle restarts — and is exactly wrong on a WING FLAP, which has to loop
          // seamlessly: held, the ladybug stops dead with its wings open six times a second's worth of frames
          // into every second. The RATE stays the house 24 fps.
          const pauseMs2 = (ED.name2 === 'flamingo' || ED.flyer2) ? 0 : 600;
          const cyc2 = n2c ? n2c * frameMs2 + pauseMs2 : 1, t2 = tt % cyc2;
          const fi2 = n2c ? (t2 < n2c * frameMs2 ? Math.floor(t2 / frameMs2) : n2c - 1) : 0;
          // ── THE SIDE LANE CAN FLY (user 2026-08-22: the ladybug "has to behave like the other flying
          // insects, like the butterfly for example") ── the same eased-turn wander the world's butterflies and
          // dragonflies fly on, with their numbers: retarget the turn every 0.4-1.2 s to ±2 rad/s, ease the
          // angular velocity toward it at 9/s, integrate the heading, advance along it. That integrator is what
          // gives a flyer its characteristic loose, fluttery arc rather than a circle or a straight line, so it
          // is copied rather than re-invented (main/tick-creatures.js, the kind-0 block).
          // SPEED IS THE ONE NUMBER NOT COPIED: the world flies them at 56 vox/s, which crosses everything the
          // stage camera can see in about two seconds. 22 is the same motion at a pace you can actually watch,
          // and the stage is a preview, not the world.
          // The wander is CONTAINED: past ED_FLY_R from its home it steers back in, so it orbits the side lane
          // rather than leaving the shot or reaching the stage edge, where edLayout's wrapIn would fold it
          // across to the far side.
          const ED_FLY_SPD = 22, ED_FLY_R = 20, ED_FLY_Y = 14, ED_FLY_BOB = 4;
          let flyMoved = false;
          if (ED.flyer2 && n2c) {
            const F = bfly;
            if (!F.init) { F.init = true; F.x = 0; F.z = 0; F.y = ED_FLY_Y; F.th = Math.random() * 6.2831853; F.om = 0; F.omT = 0; F.tRe = 0; F.t = 0; }
            F.t += dt;
            if (F.t > F.tRe) { F.omT = (Math.random() - 0.5) * 4.0; F.tRe = F.t + 0.4 + Math.random() * 0.8; }
            if (F.x * F.x + F.z * F.z > ED_FLY_R * ED_FLY_R) {   // outside its orbit → turn back toward home, hard enough to actually come about
              const inTh = Math.atan2(-F.x, -F.z), d9 = Math.atan2(Math.sin(inTh - F.th), Math.cos(inTh - F.th));
              F.omT = Math.max(-5, Math.min(5, d9 * 3));
            }
            F.om += (F.omT - F.om) * (1 - Math.exp(-9 * dt)); F.th += F.om * dt;
            F.x += Math.sin(F.th) * ED_FLY_SPD * dt; F.z += Math.cos(F.th) * ED_FLY_SPD * dt;
            F.y = ED_FLY_Y + Math.sin(F.t * 2.2) * ED_FLY_BOB;
            // ── FACING, WITH A DEAD BAND ── the model is GRID-STAMPED, so it can only face the four cardinals;
            // snapping on the nearest one alone makes a heading drifting along a quadrant boundary flip back and
            // forth every few frames. Re-snap only once the heading is 56° off what it is showing — 45° plus a
            // hysteresis band — so a turn reads as one clean step.
            const want = ((-Math.round(F.th / (Math.PI / 2))) % 4 + 4) % 4;   // th 0 is +z. The mapping was 180 degrees out and the ladybug flew backwards (user 2026-08-22); the stamp's cardinal order runs the opposite way round from the heading angle
            if (want !== ED.spin2) {
              const cur = [Math.PI, Math.PI / 2, 0, -Math.PI / 2][ED.spin2 | 0];
              if (Math.abs(Math.atan2(Math.sin(F.th - cur), Math.cos(F.th - cur))) > 0.98) { ED.spin2 = want; flyMoved = true; }
            }
            const hx = Math.round(F.x), hy = Math.round(F.y), hz = Math.round(F.z);
            if (hx !== ED.hop2X || hy !== ED.hop2Y || hz !== ED.hop2Z) { ED.hop2X = hx; ED.hop2Y = hy; ED.hop2Z = hz; flyMoved = true; }
          }
          // ── BLINKING, ON THE WORLD'S OWN CLOCK (user 2026-08-21: "make the frogs red eyes blink just like the
          // life in the world" / "have one eye blink first, then .5 seconds later the other eye") ── the world's
          // universal blink (main/tick-creatures.js) is 150 ms shut and then 1.1-2.4 s open, randomised per
          // creature; this branch had its own 160-every-3400 instead, which is neither. So take the world's
          // numbers, and stagger the pair: the near eye shuts, the far one follows half a second later, and the
          // random gap is measured from the end of the pair so the rhythm stays the world's rather than becoming
          // a metronome. SHUT + STAGGER never overlap (150 < 500), so no both-eyes-closed variant is needed.
          // Only a model with a per-side pair takes this path — see edBuildFrames. Everything else keeps the
          // both-eyes blink it has always had, on the cadence it has always had, so no other creature moves.
          const BLINK_SHUT = 150, BLINK_STAG = 500, BLINK_PAIR = BLINK_STAG + BLINK_SHUT;
          if (!ED.blinkT0 || now >= ED.blinkT0 + BLINK_PAIR + ED.blinkGap) { ED.blinkT0 = now; ED.blinkGap = 1100 + Math.random() * 1300; }
          const bt = now - ED.blinkT0, winks = !!(ED.frames[0] && ED.frames[0].voxBlinkL);
          const eyeP = !winks ? 0 : bt < BLINK_SHUT ? 1 : (bt >= BLINK_STAG && bt < BLINK_PAIR) ? 2 : 0;
          const blink = winks ? false : (now % 3400) < 160;
          if (fi !== (((ED.sel % n) + n) % n) || fi2 !== ED.sel2 || mixSwap || flyMoved || blink !== ED.blink || eyeP !== ED.blinkE) { ED.sel = fi; ED.sel2 = fi2; ED.blink = blink; ED.blinkE = eyeP; ED.spin = 0;
            // ── THE FLAMINGO ANIMATES IN PLACE (user 2026-08-18: "in the center of it, animated") ── this
            // branch normally walks the model forward by the per-frame offset delta once per cycle, which is
            // what makes an imported walk cycle travel. The flamingo is the stage's opening exhibit and is
            // meant to stay centred, so its cycle count is pinned to 0 and every hop below multiplies out to
            // nothing. Its frames are base-aligned anyway (tools/align_frames.py) so the delta is already ~0 —
            // this states it rather than relying on it, because a re-bake carrying offsets would otherwise
            // quietly walk it off the middle.
            // A ZEROED k RATHER THAN AN EARLY RETURN: the ED.box / ED.box2 publishes below this block still
            // have to run, and returning here would silently stop the hit box following the model.
            // ── THE STAGED MODEL STAYS WHERE YOU PUT IT (user 2026-08-21: "I cant click on the frog anymore to
            // edit it" / "you also didnt bake the frog correctly") ── k was the CYCLE COUNT, and the offsets it
            // multiplies are last-frame-minus-first, so any sequence with a forward delta crept across the stage
            // one cycle-length per cycle, forever, with wrapIn teleporting it out the far side when it ran off.
            // Invisible while every import sat at zero offsets (delta 0 = k times nothing); the moment the frog's
            // hop was baked the delta became -10 in z and the frog marched away and wrapped — measured hopZ
            // -90, -100, -110 … and the stamped centre jumping -114 -> +116 across the stage. That breaks the one
            // thing the stage is for: you cannot align a model you have to chase, and the click test could not
            // find it either.
            // So the accumulation is gone and the cycle plays IN PLACE. The travel the animation itself carries
            // is untouched — the frog still leaps its metre inside the cycle, because that lives in the frames'
            // own offsets, which edLayout applies either way; it simply starts each hop from the same spot
            // instead of from where the last one ended. The flamingo was already pinned to 0 by name for exactly
            // this reason ("in the center of it, animated"), which made it two out of two for this branch, so the
            // special case is now the rule and the name test is gone with it.
            // ── AND THE CYCLES ACCUMULATE, SO THE FROG TRAVELS ── k is the number of COMPLETED runs, off the
            // anchored clock above. The frame's own offset already carries the hop's travel inside one cycle
            // (0 → −10 in z for the frog), so k × that delta is exactly where the previous hops left it: the
            // arithmetic is continuous across the boundary by construction — the last frame of run k sits at
            // (k+1)·delta and the first frame of run k+1 sits at the same place — which is what keeps the
            // positions right rather than snapping between hops.
            // Nothing else is added: no wander, no bob, no physics. The only thing that ever moves the model
            // besides its own frames is this one multiple of its own authored travel.
            // ── AND THE RUN RESTARTS ON A CYCLE BOUNDARY, NEVER MID-HOP ── the stage is 242 voxels and the frog
            // covers 10 a cycle, so a run that accumulates forever eventually walks off it. edLayout's wrapIn
            // catches that and folds the model back inside, but wrapIn works on the TOTAL position, so it fires
            // whenever the model happens to cross the edge — measured at frame 6 of a hop, teleporting the frog
            // from z −115 to +113 halfway through its leap. A hop cut in half is precisely the "wrong
            // positioning" this is meant to avoid.
            // So bound the cycle COUNT instead of the position. k only changes between cycles, so wrapping k can
            // only ever move the model at a boundary — where it is grounded, at the end of a completed hop — and
            // the run simply starts again from the base position it was framed at. kmax is how many whole cycles
            // of this model's own travel fit on the stage from the centre out, so the frog gets 10 hops of
            // runway before it starts over, and a sequence that travels nowhere (delta 0) is unaffected.
            const lf0 = ED.frames[n - 1] || {}, ff0 = ED.frames[0] || {};
            const dxC = (lf0.ox || 0) - (ff0.ox || 0), dyC = (lf0.oy || 0) - (ff0.oy || 0), dzC = (lf0.oz || 0) - (ff0.oz || 0);
            const spanXZ = Math.max(1, (Math.min(ED.pw, ED.pd) >> 1) - 16), spanY = 48;   // stage half-width less a margin for the model itself; the vertical allowance is deliberately small — nothing should be climbing the sky one cycle at a time
            const kmax = Math.min(dxC ? Math.floor(spanXZ / Math.abs(dxC)) : 1e9,
                                  dzC ? Math.floor(spanXZ / Math.abs(dzC)) : 1e9,
                                  dyC ? Math.floor(spanY / Math.abs(dyC)) : 1e9);
            const k = kmax >= 1e9 ? 0 : Math.floor(tt / cyc) % (kmax + 1);   // a sequence that goes nowhere stays at k=0 rather than multiplying zero forever
            const lf = ED.frames[n - 1] || {}, f0 = ED.frames[0] || {};
            // Skipped while a PLAYLIST is running: k × delta assumes every cycle travels the same distance,
            // which is the one thing a mix of croaks, flicks and leaps does not do. The accumulator above owns
            // ED.hop* in that case, and has already banked this boundary.
            if (!ED.mix.length) { ED.hopX = k * ((lf.ox || 0) - (f0.ox || 0)); ED.hopY = k * ((lf.oy || 0) - (f0.oy || 0)); ED.hopZ = k * ((lf.oz || 0) - (f0.oz || 0)); }
            // …and the side lane's march is counted on ITS cycle, for the same reason its frame index is:
            // k is "completed runs", so multiplying the side lane's own travel by the CENTRE lane's run count
            // put it wherever the other animation's length happened to leave it — the two would only agree if
            // the cycles were the same length, which is exactly what they are not. ED_LANE_RUN, not spanXZ:
            // the side lane is bounded by what stays IN SHOT from the stage camera, not by what fits on the
            // stage — see the constant in ui/editor.js.
            const l2 = n2c ? ED.frames2[n2c - 1] : {}, g2 = n2c ? ED.frames2[0] : {};
            const dx2 = (l2.ox || 0) - (g2.ox || 0), dy2 = (l2.oy || 0) - (g2.oy || 0), dz2 = (l2.oz || 0) - (g2.oz || 0);
            const kmax2 = Math.min(dx2 ? Math.floor(ED_LANE_RUN / Math.abs(dx2)) : 1e9,
                                   dz2 ? Math.floor(ED_LANE_RUN / Math.abs(dz2)) : 1e9,
                                   dy2 ? Math.floor(spanY / Math.abs(dy2)) : 1e9);
            const k2 = kmax2 >= 1e9 ? 0 : Math.floor(tt / cyc2) % (kmax2 + 1);
            // Not while it FLIES: the wander above owns ED.hop2* in that case, and a cycle march would fight it.
            if (!ED.flyer2) { ED.hop2X = k2 * dx2; ED.hop2Y = k2 * dy2; ED.hop2Z = k2 * dz2; }
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
