  function tickBody(now) {
    tickReq = false;
    // ── RECORDING CADENCE ── (see the block above veStartRec) hold the paint rate to the capture rate
    // so captureStream's sampler finds exactly one fresh frame per slot instead of beating against a
    // 120 Hz paint. This gate is at the TOP on purpose: skipping a WHOLE frame is safe, but returning
    // part-way through skips the strip-scatter dispatch with xStripPending still set and breaks the
    // streaming order (that mistake threw a tick exception).
    // ── RECORDING PUSHES A FRAME, IT NO LONGER SKIPS ONE (user 2026-08-19) ── this gate used to RETURN here to
    // hold the paint rate down to the capture rate, which is what halved a 120 Hz game's frame rate for the
    // length of a take. The cadence argument behind it was right and is kept, but it is served the other way
    // round now: the canvas track is in manual-push mode (captureStream(0), see veStartRec) and the render loop
    // hands it one frame every capEvery-th paint. Spacing is exact BY CONSTRUCTION — it is an integer division
    // of the paint rate rather than a sampler's own clock beating against it — and the game paints every frame.
    // It sits at the TOP for the same reason the old gate did: this must not run part-way through a tick.
    // The push captures the frame PRESENTED LAST tick, which is what makes it a whole frame rather than a
    // half-drawn one.
    const dt = Math.min(0.05, (now - prevT) / 1000); prevT = now;
    // ── VITALS ── UNCONDITIONAL, and that matters: this first sat further down beside the hazard checks, which
    // are inside the `if (P.fly) … else` movement branch, so the vitals silently stopped the moment fly mode
    // engaged (measured: it ran exactly twice, then never again). Health is not part of movement.
    // Since the hunger rework (2026-08-17) this only rides the hit KICK down — health itself no longer changes
    // with time in either direction, so there is nothing here a dropped frame could get wrong.
    vitTick(dt);
    if (locked && anthemArmed && !anthemDone) { playSecs += dt; if (playSecs >= anthemNextAt) playAnthem(); }   // ── PLAY CLOCK ── counts only while the player HAS THE CONTROLS: the loading screen, the press-any-button prompt, the esc menu and the death screen are not gameplay, and a backgrounded tab stops rAF outright. Two minutes of PLAYING, not two minutes of the page being open.
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
    // ── NOTHING GATES SPRINTING BUT THE KEY AND THE CROUCH ── it was `&& vitSprintOK()`, i.e. Minecraft's "no
    // sprinting at hunger 6 or below". Faithful, and wrong here: sprinting COST 0.1 exhaustion per metre, so a long
    // sprint drained hunger past the threshold and switched itself off, with nothing on screen that could explain
    // why ("the sprint button stop working randomly", user 2026-08-16). A hidden rule that disables a movement key
    // is a bug however correct the rule is. Hunger was then deleted outright (2026-08-17), so there is no longer a
    // number in the game that COULD gate this — `vitSprintOK` is a constant true kept only for __vb.vit(). Both of
    // the conditions below are ones the player can see themselves doing, which is the property that matters.
    const sprint = keys.has(binds.sprint) && !crouching;
    P.sprint = sprint;                                 // …published because the footstep loop runs at twice the rate while sprinting (ui/audio.js), and it must read the KEY, not a speed threshold: sprintJump and the sand slowdown both move the speed without changing what the player is doing
    const fx = Math.sin(P.yaw), fz = Math.cos(P.yaw);
    let mx = 0, mz = 0;
    if (keys.has(binds.forward)) { mx += fx; mz += fz; }
    if (keys.has(binds.back)) { mx -= fx; mz -= fz; }
    if (keys.has(binds.left)) { mx -= fz; mz += fx; }
    if (keys.has(binds.right)) { mx += fz; mz -= fx; }
    if (dead) { mx = 0; mz = 0; }
    const ml = Math.hypot(mx, mz); if (ml > 0) { mx /= ml; mz /= ml; }
    if (P.fly) {
      const spd = sprint ? 255 : 90;              // ── x1.5 (user 2026-08-21: "make the fly speed 1.5x faster") ── was 170/60. BOTH arms scale, so sprint keeps the 2.83x ratio over the cruise it has always had; and vy reads the same `spd`, so up/down speeds up with forward instead of the climb quietly falling behind.
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
      P.swim = swimming;                               // published for moveAxis's step-up gate (sim/player.js) — set HERE, above the two horizontal moves below, so it is this frame's answer and not last frame's
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
      // ── FALL DAMAGE: REMEMBER THE HIGHEST POINT OF THE FALL ── not the fall TIME (P.fallT) and not the
      // impact speed, because both lie on a staircase: a player bouncing down a dune resets fallT on every
      // contact and never accumulates, while a long shallow slide builds speed without ever really dropping.
      // The peak-minus-landing height is the one quantity that means "how far did I actually fall".
      if (!P.onGround && !P.fly) { if (P.fallPk === undefined || P.y > P.fallPk) P.fallPk = P.y; }
      const impV = P.vy;                              // impact velocity before the collision zeroes it
      const hitY = moveAxis(1, P.vy * dt, hh);
      if (hitY) {
        if (impV < 0 && !inWater && !dead && onMushroom()) {   // BOUNCY MUSHROOM — relaunch UP instead of sticking, higher each consecutive bounce
          P.bounceN = Math.min(P.bounceN + 1, 10);
          P.vy = Math.min(BOUNCE_MAX, BOUNCE_V0 + BOUNCE_DV * (P.bounceN - 1));
          P.onGround = false; P.sprintJump = false;
        } else {
          if (P.vy < 0) {                             // landed — the next fall winds up from zero again
            // ── AND PAY FOR IT ── Minecraft's curve, in this game's units: a voxel is 10 cm and HEIGHT is 20,
            // so a 2 m person is 20 voxels and one METRE is 10. Nothing under FALL_FREE metres hurts, then one
            // point per further metre. Landing in water is free, which is what makes a lake a way down.
            // `bypass` = true: armour could never have stopped a fall, and Minecraft charges no exhaustion for
            // it — the same flag drowning and starving already pass.
            const drop = (P.fallPk === undefined ? P.y : P.fallPk) - P.y;
            if (!dead && !P.fly && !inWater && !P.noFall && drop > 0) {
              // ── …AND THE +1e-3 IS THE COLLISION EPSILON, NOT A FUDGE ── moveAxis parks a landed body at
              // floor(y) + 1 + 0.001 (player.js), so both ends of this subtraction carry a millimetre that
              // very nearly cancels — but only very nearly: float drift in the sub-stepped move leaves the
              // measured drop up to ~0.0012 voxels SHORT of the real one. On any height that is not an exact
              // metre nobody could tell, but Math.floor cuts exactly on the metre, so a true 4 m step off a
              // ledge measured 39.9991 and bucketed as 3 m — no damage — on 6 falls out of 25. The tolerance
              // is a thousandth of a metre = 0.01 voxels, roughly 8x the observed drift so it clears it with
              // margin, while still being 100x smaller than the metre this floor() buckets on: it can only
              // rescue a drop already sitting on a boundary, and cannot promote one that is genuinely short
              // (39 voxels stayed at 0 damage across 25 trials after the change).
              const md = Math.floor(drop / 10 + 1e-3);      // whole metres, the figure the death line quotes
              const m = md - FALL_FREE;
              if (m > 0) vitHurt(md >= FALL_KILL ? 999 : Math.round(m + m * m / FALL_K), 'you fell ' + md + ' m', true);   // 999 rather than the max-health constant: vitHurt floors hp at 0, so any number past the bar is death, and this one cannot go stale if the bar changes. See FALL_K / FALL_KILL in input.js for the curve.
            }
            P.fallPk = undefined; P.noFall = 0;   // the free landing is spent
            P.onGround = true; P.sprintJump = false; P.fallT = 0;
          }
          P.bounceN = 0;                              // landed on normal ground / hit a ceiling → reset the bounce streak
          P.vy = 0;
        }
      }
      // LAVA no longer kills on contact: 4 damage twice a second, which is Minecraft's rate and gives a player
      // who steps in the edge of a pool about two and a half seconds to get out again.
      if (!dead && !P.fly && (lavaAt(Math.floor(P.x), Math.floor(P.y), Math.floor(P.z)) || lavaAt(Math.floor(P.x), Math.floor(P.y + 1), Math.floor(P.z)))) {
        vbLavaT += dt;
        if (vbLavaT >= 0.5) { vbLavaT = 0; vitHurt(4, 'you burned in the lava', true); }
      } else vbLavaT = 0;
      // ── QUICKSAND ── a sand flat swallows you a little more every second you stand on it. The sink is applied to the
      // EYE, not to P.y: the player still stands on solid ground for collision, so nothing here can shove them through
      // the world — the camera simply slides down into the sand. Once the sand closes over the eye, that's the death.
      if (!dead && !P.fly && P.onGround && onFlatSand()) P.sink = Math.min(eyeH + 2, P.sink + SINK_IN * dt);
      else P.sink = Math.max(0, P.sink - SINK_OUT * dt);   // stepped off / jumped / flying → haul yourself back out
      // …and the sand suffocates rather than swallowing you whole: once it closes over the eye it takes a
      // point a second, so there is a window to jump clear if you notice the screen going under.
      if (!dead && !P.fly && P.sink >= eyeH) {
        vbSandT += dt;
        if (vbSandT >= 1.0) { vbSandT = 0; vitHurt(1, 'you sank into the quicksand', true); }
      } else vbSandT = 0;
      // ── CACTUS SPINES (user 2026-08-15) ── leaning on one costs a point, on a cooldown so standing against
      // a saguaro drains you steadily rather than instantly. `hh` is the same live collision height moveAxis
      // is given above, so a crouched player is tested at 13 voxels and not 20 — the test asks about the body
      // that is actually there. Cacti are markSolid, so the usual way to earn this is to WALK INTO one: the
      // collision clamp leaves the box 1 mm off the trunk and cactusHurtAt's margin covers exactly that.
      // ── …AND THE FIRST ONE LANDS THE INSTANT YOU TOUCH IT (user 2026-08-17: "have the cactus in the desert
      // damage the player immediatly upon the player touching it … rubbing up against it I mean") ── the timer
      // counted UP FROM ZERO on contact, so brushing a saguaro cost nothing for the first 0.9 s and a player
      // who bumped one and stepped away was never hurt at all. It is a LEADING edge now, and the whole of the
      // change is what the clear branch rests the timer at: primed rather than zeroed, so the very first frame
      // of contact is already over the line. The cadence after that is untouched — leaning on one still costs
      // a point every CACT_CD, which is the part the 2026-08-15 note argues for.
      // Same reason the LAVA and QUICKSAND timers below/above are deliberately NOT changed: those are hazards
      // you stand IN and they are meant to ramp; this is a thing you brush PAST, and it has to bite once.
      if (!dead && !P.fly && cactusHurtAt(P.x, P.y, P.z, hh)) {
        vbCactT += dt;
        if (vbCactT >= CACT_CD) { vbCactT = 0; vitHurt(1, 'the cactus spines got you'); }
      } else vbCactT = CACT_CD;
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
      if (snowOn && now > snowEndT) { snowOn = false; snowRearm(); snowBtnSync(); }   // a snow EVENT lasts 60 s…
      else if (!snowOn && now > snowNextT) {          // …then waits 5 min for the next (first one is 2 min after refresh)
        // ── BUT NOT AFTER DARK (user 2026-08-19: "dont make it snow at night") ── the arrival is DEFERRED, not
        // cancelled: pushing snowNextT forward by SNOW_NIGHT_RETRY re-asks the same question a few seconds later
        // and the storm lands on the first tick after sunrise. Cancelling instead (snowNextT = now + 300000)
        // would silently drop every storm whose 5-minute slot happened to fall in the dark, which at the 20-min
        // cycle is half of them. sunUp()/SNOW_DAY_ONLY are in ui/settings.js beside snowNextT itself.
        if (SNOW_DAY_ONLY && !sunUp()) { snowNextT = now + SNOW_NIGHT_RETRY; }
        else { snowOn = true; snowEndT = now + 60000; snowBtnSync(); }
      }
      // ── THE ICE MUST OUTLIVE ITS OWN BLANKET (user 2026-08-07: "snow forms square artifacts when it lands on
      // the water") ── landing is governed by the ice (freezeK >= 0.6) but REMOVAL was governed by a wall clock
      // (snowWMeltAt, +6 s after the storm), and the two never overlapped: freezeK reached 0 about 5 s after a
      // storm ended, the shader turned the Gerstner waves back on at 0.4, and the blanket then sat on fully
      // liquid moving water for seconds — a field of isolated white cubes bobbing on the swell, which is exactly
      // the screenshot. Holding at 0.4 (the shader's own wave-suppression threshold) until the last water-snow
      // voxel has drained means snow is never seen on wavy water; the sheet then thaws normally.
      // ── AND IT MUST BE SNOWING WHERE YOU ARE, NOT MERELY STORMING (2026-08-17) ── freezeK gated on
      // `snowOn` alone, which was exactly right while a storm meant snow everywhere. It now RAINS in the
      // oak forest, and rain was skinning the lakes there with ice: water freezing over while liquid drops
      // fall through it. rainSkyK * oakM is the same scalar the sky dims on, so ice fades out on the same
      // ramp the weather does instead of switching on a line.
      // NOTE WHAT THIS CAN AND CANNOT BE: freezeK is ONE GLOBAL scalar - it drives solidTab[WATER_T], a
      // material flag for the whole world - so ice has never been per-column and this cannot make it so. The
      // test is therefore CAMERA-RELATIVE, exactly like the sky dimming it shares its scalar with: stand in
      // the oak forest and lakes do not freeze, walk into the pines and they do. That is the right
      // approximation while you can only ever see the water near you, and per-column ice is a far larger
      // change than this line.
      // RAIN_ON false => `snowOn` alone, the test this was before the rain landed: it snows in the oak
      // forest again, so its lakes are meant to freeze again.
      // ── IS IT SNOWING WHERE THE PLAYER IS? ── freezeK is ONE GLOBAL scalar driving solidTab[WATER_T], so
      // this has always been camera-relative and cannot be per-column. Two ways the answer is now no in
      // the oak forest: it is raining there (the rain ramp), or nothing falls there at all (OAK_SNOW).
      // ── AND "NOT THE OAK FOREST" IS NOT THE SAME QUESTION AS "IS IT SNOWING HERE" (user 2026-08-21: "the oak
      // forest water freezes over when it snows in other biomes. prevent this from happening") ── the test below
      // asked only about oak, so every column that is not oak counted as snowing. THE DESERT IS NOT OAK AND GETS
      // NO SNOW: stand on the sand during a storm and snowHere went true, freezeK climbed, solidTab[WATER_T]
      // flipped — and because that flag is GLOBAL, every lake in the world skinned over, the oak forest's
      // included. That is the report almost word for word: it snows in another biome and your water freezes.
      // dryHere is the BLANKET'S OWN TEST, verbatim — tick-snow.js computes wSharp(max(desertM, oakWeather)) per
      // settling column and refuses the column when it is high. Asking the identical expression here is what
      // makes "there is ice" and "there is snow on the ground" answer the same way at both borders instead of
      // only at the oak one. The OAK_SNOW arm is carried through so all four states of the matrix at the top of
      // world/window.js stay coherent: with snow switched back on in the oak, the desert must still stay liquid.
      // ── …AND THE OAK BAND NEVER ICES AT ALL, BLOSSOM INCLUDED ── the second half of the same request, and it
      // needs its own term because dryHere cannot express it. cherryW ramps over CHBW = 300, so oakWeather is
      // dragged down for 300 voxels of PURE OAK either side of the blossom, and wSharp turns that into a `< 0.5`
      // well before the pink starts: standing among oak trees beside an oak lake, watching it freeze. Keying the
      // refusal on oakM — the BIOME, not the weather — is exact, and it costs the blossom its ice. That is a
      // deliberate trade and not an oversight: the blossom is inside the oak forest, so "the oak forest does not
      // freeze" has to include it or the border comes straight back. It still SNOWS there (the 2026-08-18 rule
      // is untouched — this line moves ice only).
      // WHAT THIS STILL CANNOT DO: freezeK is ONE GLOBAL scalar driving a material flag, so ice has never been
      // per-column and this does not make it so. Stand in the pines and an oak lake across the treeline still
      // freezes with everything else. Fixing THAT is per-column ice, which is a far larger change.
      const oakHere = Math.max(0, Math.min(1, wSharp(oakWeather(P.x, P.z))));   // wSharp so the ice follows the same tightened weather border the blanket and the flakes do   // oakWeather, not oakM (user 2026-08-18): the blossom band is inside oakM but it SNOWS there now, so its lakes freeze like the pines'
      const dryHere = Math.max(0, Math.min(1, wSharp(Math.max(desertM(P.x, P.z), OAK_SNOW ? 0 : oakWeather(P.x, P.z)))));   // 1 = no snow falls on this column — tick-snow.js's dmS, asked at the camera
      const oakBand = oakM(P.x, P.z) > 0.5;            // the BIOME, not the weather: the one test cherryW cannot drag down
      const snowHere = snowOn && !oakBand && dryHere < 0.5 && (!RAIN_ON || rainSkyK * oakHere < 0.5);
      // ── AND THE ANTI-WAVE FLOOR IS ABOUT WHERE THE SNOW IS, NOT WHERE YOU ARE (user 2026-08-18: "the water in
      // the oak forest seems to freeze") ── the 0.4 below pins freezeK at the shader's wave-suppression
      // threshold while water-snow is still draining, so a white blanket is never seen bobbing on a swell.
      // But snowWN/snowWHead is a GLOBAL queue and freezeK is a GLOBAL scalar, so a storm draining anywhere
      // held EVERY lake wave-flat — and wave-flat water reads as ice, even though 0.4 is well below the 0.6
      // that actually flips solidTab. The blossom band sitting inside the oak forest is what made this
      // constant: oak now borders snow country on both sides instead of only at the pine treeline.
      // Safe to drop the floor here because there is no water-snow near the camera to protect: landSnowAt
      // refuses to settle any where oakWeather says oak (the dmS gate in tick-snow.js), so the case the floor
      // exists for cannot arise in the biome this exempts.
      const oakDry = oakBand || dryHere >= 0.5;        // the SAME two terms snowHere splits on, so the two can never disagree about whether this column gets weather
      freezeK = (snowHere && now >= snowFreezeAt) ? Math.min(1, freezeK + dt / 5)
        : Math.max((snowWN > snowWHead && !oakDry) ? 0.4 : 0, freezeK - dt / 5);   // water freezes over 5 s and thaws back over 5 s — but only from SNOW_FREEZE_DELAY (10 s) after the storm starts, so the flakes you can see have reached the ground before the river skins over
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

