  function tickBody(now) {
    tickReq = false;
    // ── RECORDING CADENCE ── (see the block above veStartRec) hold the paint rate to the capture rate
    // so captureStream's sampler finds exactly one fresh frame per slot instead of beating against a
    // 120 Hz paint. This gate is at the TOP on purpose: skipping a WHOLE frame is safe, but returning
    // part-way through skips the strip-scatter dispatch with xStripPending still set and breaks the
    // streaming order (that mistake threw a tick exception).
    if (VE.recording && now - VE.lastPaint < VE_CAP_MS) { tickReq = true; paceWaited = false; requestAnimationFrame(tick); return; }
    if (VE.recording) VE.lastPaint = now - Math.min(VE_CAP_MS * 0.5, (now - VE.lastPaint) - VE_CAP_MS);   // carry the overshoot so the cadence cannot drift slow
    const dt = Math.min(0.05, (now - prevT) / 1000); prevT = now;
    if (locked && !anthemDone) { playSecs += dt; if (playSecs >= anthemNextAt) playAnthem(); }   // ── PLAY CLOCK ── counts only while the player HAS THE CONTROLS: the loading screen, the press-any-button prompt, the esc menu and the death screen are not gameplay, and a backgrounded tab stops rAF outright. Two minutes of PLAYING, not two minutes of the page being open.
    fpsEma = fpsEma * 0.95 + (1 / Math.max(dt, 1e-4)) * 0.05;
    FT[ftI] = dt * 1000;                               // frame-time ring (1% lows / spike hunting); FTB is filled at the end of the body
    if (CPROF) { cpLast = performance.now(); tbT0 = cpLast; upN = 0; upB = 0; cpEvt = 0;
      const hm = performance.memory; if (hm) { const h = hm.usedJSHeapSize;
        if (heapPrev) { const d = h - heapPrev; if (d < 0) { heapDrops++; cpEvt |= 128; } else heapAlloc += d; }
        heapPrev = h; } }

    // ── player physics ──
    const crouching = keys.has(binds.crouch) && !P.fly;
    P.crouch = crouching;
    P.crouchT = (P.crouchT || 0) + ((crouching ? 1 : 0) - (P.crouchT || 0)) * (1 - Math.exp(-13 * dt));   // SMOOTH crouch — the eye eases down over ~150 ms instead of snapping (user); collision height stays instant so tight gaps still fit
    if (P.crouchT < 0.001) P.crouchT = 0;
    const hh = crouching ? CR_HEIGHT : HEIGHT, eyeH = EYE + (CR_EYE - EYE) * P.crouchT;   // eyeH interpolates → smoothEye tracks it gently (no more ±3-clamp snap)
    if (cineMode && !ED.on && !dead) { cineUpdate(dt, now); smoothEye += (P.y + eyeH - smoothEye) * (1 - Math.exp(-6 * dt));
      if (!blurLock) cineBlurK += (1 - cineBlurK) * (1 - Math.exp(-2.5 * dt)); }
    else {
      if (!blurLock) cineBlurK = cineBlurK < 0.002 ? 0 : cineBlurK * Math.exp(-5 * dt);   // fade the blur out with the roll — an instant cut back to a sharp image is as jarring as popping it on
      if (P.roll) P.roll = Math.abs(P.roll) < 0.002 ? 0 : P.roll * Math.exp(-7 * dt);   // leaving cine mode (or dying in it) levels the horizon out smoothly instead of snapping upright
    const sprint = keys.has(binds.sprint) && !crouching;
    const fx = Math.sin(P.yaw), fz = Math.cos(P.yaw);
    let mx = 0, mz = 0;
    if (keys.has(binds.forward)) { mx += fx; mz += fz; }
    if (keys.has(binds.back)) { mx -= fx; mz -= fz; }
    if (keys.has(binds.left)) { mx -= fz; mz += fx; }
    if (keys.has(binds.right)) { mx += fz; mz -= fx; }
    if (dead) { mx = 0; mz = 0; }
    const ml = Math.hypot(mx, mz); if (ml > 0) { mx /= ml; mz /= ml; }
    if (P.fly) {
      const spd = sprint ? 170 : 60;
      const k = 1 - Math.exp(-10 * dt);
      P.hvx += (mx * spd - P.hvx) * k; P.hvz += (mz * spd - P.hvz) * k;
      let vy = 0; if (keys.has(binds.jump)) vy += spd; if (keys.has(binds.crouch)) vy -= spd;   // fly down = crouch (Alt) only; Ctrl no longer descends (user removed it)
      P.x += P.hvx * dt; P.z += P.hvz * dt; P.y += vy * dt;
      P.y = Math.max(-40, Math.min(WY + 240, P.y));
      P.onGround = false;
      smoothEye = P.y + eyeH;
    } else {
      const inWater = waterAt(Math.floor(P.x), Math.floor(P.y + 3), Math.floor(P.z));
      // ── ARE WE ACTUALLY SWIMMING? ── inWater probes 3 voxels above the FEET, which on a 20-voxel person is
      // ankle height. That is the whole reason holding Space felt like skidding across the surface: the paddle
      // shut off the moment the ankles cleared the water, so the body was driven up until it stood ON the
      // waterline with 17 of its 20 voxels in open air. Swimming is a WAIST-DEEP question (user 2026-08-05).
      // inWater keeps the old probe: it gates horizontal drag and drowning, where ankle-deep is the right call.
      const swimming = waterAt(Math.floor(P.x), Math.floor(P.y + SWIM_DEEP), Math.floor(P.z));
      // ── THE PLAYER BREAKS THE SURFACE ── same splash a fish throws, either direction (user 2026-08-05).
      // Keyed off the STATE CHANGE, so wading along at the waterline cannot chatter; scaled by how hard the
      // crossing was, so a dive throws spray and stepping in off a beach barely ripples.
      if (P.wasWet === undefined) P.wasWet = inWater;
      if (inWater !== P.wasWet) { P.wasWet = inWater;
        if (!dead) spawnSplash(P.x, P.z, Math.max(0.55, Math.min(1.6, Math.abs(P.vy) / 60 + 0.55))); }
      const spd = WALK * (sprint ? SPRINT : 1) * (crouching ? CROUCHM : 1) * (inWater ? WATER_SPD : 1) * (P.sprintJump ? 1.2 : 1) * (onSand() ? 0.5 : 1);   // sand pits slow the player 50% (user)
      const k = 1 - Math.exp(-(P.onGround ? 14 : 3.2) * dt);
      P.hvx += (mx * spd - P.hvx) * k; P.hvz += (mz * spd - P.hvz) * k;
      moveAxis(0, P.hvx * dt, hh);
      moveAxis(2, P.hvz * dt, hh);
      if (P.onGround && P.vy <= 0) {                   // GROUND-STICK: descending voxel steps snaps to the slope instead of micro-falling each stride
        if (boxFree(P.x, P.y - 2.6, P.z, hh)) P.onGround = false;   // a real ledge → actually fall
        else if (!boxFree(P.x, P.y - 0.05, P.z, hh)) { /* already in contact */ }
        else moveAxis(1, -2.6, hh);                    // sub-stepped, stops exactly at contact
      }
      if (P.onGround && keys.has(binds.jump) && !swimming && !dead) { P.vy = JUMP; P.onGround = false; P.sprintJump = sprint; }   // a swimmer never gets a standing jump: with the old ankle-height test a held Space re-fired a full JUMP (66) every time the body came back down to the surface, which was the pogo   // sprint-jump carries +20% air speed
      if (swimming) {
        // ── SPACE IS THE ONLY THING HOLDING YOU UP (user 2026-08-09: "the player seems to be floating
        // automatically in the water… if the player is not pressing spacebar, just have the player sink all the
        // way to the bottom to the sea floor") ── the body no longer has a resting depth at all. There is no
        // buoyancy term left: releasing Space is releasing the ONLY upward force, so you go down.
        //
        // WHAT THIS REPLACED, and why the old shape could not simply be re-tuned: the target used to be a damped
        // spring on the EYE against a float line at WL + 1, i.e. a POSITION the body was pulled back to from
        // either side. Below the line that error is positive and the spring pushes UP — that push IS the
        // automatic floating, and it is not a stray constant that could be turned down. It is the whole
        // mechanism, and any gain above zero re-creates it, which is why the spring is gone on the release
        // branch rather than weakened.
        // Holding Space keeps its spring, unchanged, because that half still wants a resting position: it is
        // what makes you climb in strokes and settle AT the surface instead of launching clear of it, and the
        // sin() rock on the line is what makes the climb read as swimming rather than as a lift.
        const eyeY = P.y + (crouching ? CR_EYE : EYE);
        const rising = keys.has(binds.jump);
        // SINK is a plain terminal velocity, not an acceleration: a body in water reaches its settling speed
        // almost at once, and SWIM_EASE below is already the ramp onto it. Gravity is deliberately NOT used —
        // it winds up to -345 and would drop the player through 4-5 voxels of lake like a stone through air.
        const tgt = rising
          ? Math.max(-SWIM_SINK, Math.min(SWIM_UP, (WL + 1 + SWIM_RISE + Math.sin(now * 0.009) * SWIM_BOB - eyeY) * SWIM_K))
          : -SWIM_SINK;
        P.vy += (tgt - P.vy) * (1 - Math.exp(-SWIM_EASE * dt));
      } else {                                       // ── FALLING GAINS MOMENTUM ── (user) a flat GRAVITY into a -160 terminal hit its cap in 0.8 s, so anything
        P.fallT = P.vy < 0 ? P.fallT + dt : 0;         // past a short drop fell at a CONSTANT speed and read as floating. Gravity now ramps with time spent
        const gK = 1 + Math.min(1.125, P.fallT * 0.41); // falling and the terminal is higher, so a long drop keeps visibly winding up until it lands.
        P.vy = Math.max(-345, P.vy - GRAVITY * gK * dt);   // ramp starts at 1.0 so jump arcs are unchanged. Boost and terminal both CUT 25% (user 2026-07-18): was 1.5 / -460.
      }
      const impV = P.vy;                              // impact velocity before the collision zeroes it
      const hitY = moveAxis(1, P.vy * dt, hh);
      if (hitY) {
        if (impV < 0 && !inWater && !dead && onMushroom()) {   // BOUNCY MUSHROOM — relaunch UP instead of sticking, higher each consecutive bounce
          P.bounceN = Math.min(P.bounceN + 1, 10);
          P.vy = Math.min(BOUNCE_MAX, BOUNCE_V0 + BOUNCE_DV * (P.bounceN - 1));
          P.onGround = false; P.sprintJump = false;
        } else {
          if (P.vy < 0) { P.onGround = true; P.sprintJump = false; P.fallT = 0; }   // landed — the next fall winds up from zero again
          P.bounceN = 0;                              // landed on normal ground / hit a ceiling → reset the bounce streak
          P.vy = 0;
        }
      }
      if (!dead && !P.fly && (lavaAt(Math.floor(P.x), Math.floor(P.y), Math.floor(P.z)) || lavaAt(Math.floor(P.x), Math.floor(P.y + 1), Math.floor(P.z)))) die('you fell into the lava');
      // ── QUICKSAND ── a sand flat swallows you a little more every second you stand on it. The sink is applied to the
      // EYE, not to P.y: the player still stands on solid ground for collision, so nothing here can shove them through
      // the world — the camera simply slides down into the sand. Once the sand closes over the eye, that's the death.
      if (!dead && !P.fly && P.onGround && onFlatSand()) P.sink = Math.min(eyeH + 2, P.sink + SINK_IN * dt);
      else P.sink = Math.max(0, P.sink - SINK_OUT * dt);   // stepped off / jumped / flying → haul yourself back out
      if (!dead && !P.fly && P.sink >= eyeH) die('you sank into the quicksand');
      if (P.y < -60) respawn();
      // eye smoothing — step-ups AND stick-downs ease over ~80 ms instead of popping; exact tracking while airborne
      const targetEye = P.y + eyeH - P.sink;           // …minus how far the quicksand has swallowed us
      smoothEye += (targetEye - smoothEye) * (1 - Math.exp(-(P.onGround ? 12 : 60) * dt));
      smoothEye = Math.max(targetEye - 3, Math.min(targetEye + 3, smoothEye));   // never lag more than 3 voxels
    }
    }

    // ── stream the world window toward the player ──
    if (CPROF) cpMark(0);
    stepShifts();
    nvFlush();                                         // ── NAVFIELD ── inside the STREAM phase, beside stepShifts, whose 7-18 ms allowance goes entirely unspent once the frontier has caught up
    if (CPROF) cpMark(1);

    { const gust = Math.max(0, 0.55 + 0.45 * Math.sin(now * 0.00037) + 0.35 * Math.sin(now * 0.00113 + 2.1));   // ── WIND ── strength gusts (0..~1.35) over ~10-30 s
      const wSpd = 8.0 + 16.0 * gust;                 // 0.8–3 m/s of sideways drift — flakes travel at a marked HORIZONTAL angle, not straight down
      const wAng = Math.sin(now * 0.000021) * 2.4 + Math.sin(now * 0.000007) * 1.9;   // direction wanders over minutes
      windAX += Math.cos(wAng) * wSpd * dt; windAZ += Math.sin(wAng) * wSpd * dt;
      snowFallV = 11.0;                                // CONSTANT fall speed — no motion-based modulation (it read as far flakes speeding up while running)
      snowFallAcc += snowFallV * dt;
      if (snowOn && now > snowEndT) { snowOn = false; snowNextT = SNOW_AUTO_OFF ? Infinity : now + 300000; snowBtnSync(); }   // a snow EVENT lasts 60 s…
      else if (!snowOn && now > snowNextT) { snowOn = true; snowEndT = now + 60000; snowBtnSync(); }   // …then waits 5 min for the next (first one is 2 min after refresh)
      // ── THE ICE MUST OUTLIVE ITS OWN BLANKET (user 2026-08-07: "snow forms square artifacts when it lands on
      // the water") ── landing is governed by the ice (freezeK >= 0.6) but REMOVAL was governed by a wall clock
      // (snowWMeltAt, +6 s after the storm), and the two never overlapped: freezeK reached 0 about 5 s after a
      // storm ended, the shader turned the Gerstner waves back on at 0.4, and the blanket then sat on fully
      // liquid moving water for seconds — a field of isolated white cubes bobbing on the swell, which is exactly
      // the screenshot. Holding at 0.4 (the shader's own wave-suppression threshold) until the last water-snow
      // voxel has drained means snow is never seen on wavy water; the sheet then thaws normally.
      freezeK = (snowOn && now >= snowFreezeAt) ? Math.min(1, freezeK + dt / 5) : Math.max(snowWN > snowWHead ? 0.4 : 0, freezeK - dt / 5);   // water freezes over 5 s and thaws back over 5 s — but only from SNOW_FREEZE_DELAY (10 s) after the storm starts, so the flakes you can see have reached the ground before the river skins over
      const nowSolid = freezeK > 0.6;
      if (nowSolid !== iceSolid) { iceSolid = nowSolid; solidTab[WATER_T] = nowSolid ? 1 : 0;
        // Frozen water is choppable, and PICK-ONLY: an axe should bounce off a lake exactly as it bounces off
        // a boulder, and the shovel's okMat already refuses anything outside digOnlyTab.
        // BOTH water shades, or the pick can only ever take the one-voxel WATER_T skin and the hole never
        // gets any deeper (user 2026-08-07: "digging into ice just removes the top layer vs the chunk as a
        // whole"). Measured in a lake volume: 488 WATER_T voxels against 5033 WATER_B. solidTab is deliberately
        // NOT touched for WATER_B — you stand on the frozen skin, and cutting through it should drop you in.
        decorTab[WATER_T] = nowSolid ? 1 : 0; pickOnlyTab[WATER_T] = nowSolid ? 1 : 0;
        decorTab[WATER_B] = nowSolid ? 1 : 0; pickOnlyTab[WATER_B] = nowSolid ? 1 : 0;
        if (!nowSolid && iceCutN) {                    // ── THE THAW GIVES THE HOLE BACK ──
          const back = [];
          for (let k = 0; k < iceCutN; k++) { const ii = iceCutI[k];
            if (W[ii]) continue;                       // something settled in the hole meanwhile — leave it be
            W[ii] = iceCutV[k] || WATER_T; back.push(ii); }
          iceCutN = 0;
          if (back.length) gpuPatch(back, false);
        }
        if (nowSolid && waterAt(Math.floor(P.x), Math.floor(P.y + 2), Math.floor(P.z))) { P.y = WL + 1.5; P.vy = 0; } } }
    if (CPROF) cpMark(2);
    physStep(dt);                                      // voxel rigid bodies — FIXED 60 Hz inside (see PH.dt)

