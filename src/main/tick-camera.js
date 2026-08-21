    ambBiomeTick();                                    // fade the FOREST ambience bed out over the desert — see ui/audio.js
    // ── day/night cycle ── 20-minute full cycle at 1x; tday 0 = midnight, 0.5 = noon; ALT+scroll scales speed, L pauses
    { const ntd = window.__TFREEZE ? tday : (tday + dt * cycleSpeed / 1200) % 1;   // __TFREEZE pins the sun for perf A/Bs (the cycle moved measured trace cost 45% over one 7-minute run). Not __vb.tod(): that also sets resetHist, so pinning with it would measure a permanently-cold denoiser.
      if (ntd < tday) moonDay++;                       // a day rolled over → the moon advances one phase step (8-day cycle)
      tday = ntd; }
    // ── RAIN SKY ── the one scalar the whole overcast hangs off, computed here beside the day/night cycle
    // because it is the same kind of thing: an environment state the frame is shaded under, not an event.
    // TWO FACTORS, kept apart deliberately (see rainSkyK in ui/settings.js for why):
    //   rainSkyK — a CLOCK. Ramps 0→1 over RAIN_SKY_IN while a storm runs and 1→0 over RAIN_SKY_OUT once it
    //     ends, linearly and in real seconds, so it is frame-rate independent and it finishes. It follows
    //     snowOn, which means the snow button, the P key and __vb.snow() all drive it exactly as the weather
    //     tick does — nothing here needs to know which one turned the storm on.
    //   oakM  — a POSITION. 1 deep in the oak forest, 0 in the pines, smoothstepped across the 450-voxel border.
    //     This is what makes it RAIN rather than STORM: the pine forest and the desert take snow from the same
    //     event and must keep the sky they have today, and they do, because oakM is 0 there and every rain term
    //     in COMPOSITE is written to reproduce the fair-weather expression exactly at 0.
    // Six vnoise once a frame; oakM is the same function the rain march in TRACE is a bit-for-bit port of, and
    // the same one landSnowAt uses to refuse a flake, so the sky, the drops and the bare ground under them can
    // never disagree about where the border is.
    rainSkyK = snowOn ? Math.min(1, rainSkyK + dt / RAIN_SKY_IN) : Math.max(0, rainSkyK - dt / RAIN_SKY_OUT);
    // RAIN_ON false => 0, so the cloud thickening, the cloud darkening and the sun dim all switch off with
    // the drops. The ramp itself keeps running; only its weight is zeroed, so turning rain back on picks
    // up mid-storm without a jump.
    const rainK = RAIN_ON ? rainSkyK * oakWeather(P.x, P.z) : 0;   // oakWeather: if rain is ever switched back on it must not fall on the blossom band, which now takes snow instead
    UF[UF_RAINK] = rainK;                              // u.hurtV.w — the last float of the uniform buffer; see UF_RAINK in render/buffers.js for why that lane and why it is safe
    const ang = tday * Math.PI * 2 - Math.PI / 2;
    const el = Math.sin(ang) * 1.05;
    const ce2 = Math.cos(el);
    let sun = [Math.cos(ang) * ce2, Math.sin(el), Math.sin(ang) * ce2];
    let moonMode = false;
    if (sun[1] < -0.06) {                              // sun has set — the MOON (opposite point) lights the world
      const moon = [-sun[0], -sun[1], -sun[2]];
      if (moon[1] > 0.04) { sun = moon; moonMode = true; }
    }

    // ── camera basis ── (positions sent to the GPU are WINDOW-RELATIVE for f32 precision at any distance)
    const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch);
    const fwd = [Math.sin(P.yaw) * cp, sp, Math.cos(P.yaw) * cp];
    let right = [Math.cos(P.yaw), 0, -Math.sin(P.yaw)];
    let up = [-Math.sin(P.yaw) * sp, cp, -Math.cos(P.yaw) * sp];
    if (P.roll) {                                      // BANK: spin the basis about fwd (fwd itself is untouched, so aim is unchanged and temporal reprojection — which stores full basis vectors — follows for free)
      const cr = Math.cos(P.roll), sr = Math.sin(P.roll);
      const r2 = [right[0] * cr + up[0] * sr, right[1] * cr + up[1] * sr, right[2] * cr + up[2] * sr];
      up = [up[0] * cr - right[0] * sr, up[1] * cr - right[1] * sr, up[2] * cr - right[2] * sr];
      right = r2;
    }
    giveStartKit();                                    // ── THE STARTING KIT, once ── a no-op after the first frame that finds the tools loaded; see giveStartKit in assets/held-items.js for why it cannot be called from the loader itself
    cmpUpdate();                                       // ── compass ── one transform write per frame
    // ── DUAL-WIELD ── the left hand is showing a second one out of the SELECTED stack. Declared out here, and
    // not inside either block that wants it, because the LEFT HAND block below and the badge count directly
    // above disagreed about it for as long as both existed — and one condition spelled twice is how they got
    // to disagree. Both now read this.
    // 0 = the off hand shows nothing on its own account; otherwise the ITEM it is showing. The split itself is
    // OFF (DUAL_ON in ui/achievements.js), so in practice the only thing that fills this is the ROCK CLASH —
    // and only while the clash is actually running, the same rule the craft pair follows: a gesture shows both
    // hands because it is happening, never as a standing wield.
    const dualIt = dualHeldIt() || ((now - clashT0) < 620 && dualRocks() ? 2 : 0);
    { const hs = slots[selSlot];                       // ── HELD-STACK COUNT ── x2..x8 top right, hidden at one (user)
      // ── THE ROCK IN THE LEFT HAND IS NOT IN THE PILE (user 2026-08-18) ── the badge counted the whole stack
      // while dual-wielding, so two rocks read "x2" with both of them visibly in your hands and nothing else to
      // count. The badge is meant to say what you are CARRYING BEYOND what it is drawing, so the left hand's
      // rock comes off the total: two rocks now show no badge at all (BLIT draws nothing at or below 1), three
      // show x2, and the number always matches the rocks you cannot already see. Only the steady dual-wield
      // subtracts — a left-hand GRAB FLIGHT is an incoming pickup, not one of these rocks, and lgrab below is
      // deliberately not part of this.
      const nHold = hs && !dead ? Math.max(0, (hs.n | 0) - (dualIt ? 1 : 0)) : 0;
      stackFull = !!(hs && !dead && stackable(hs.it) && (hs.n | 0) >= STACK_MAX);   // ── FULL STACK ── on the TRUE count, not nHold, and only for something that stacks at all: a tool is a stack of one and must never read as maxed (user 2026-08-19)
      if (nHold !== stackShown) { stackShown = nHold;
        // ── STACKBADGE ── stackShown is the latch; the uniform write itself lives in the per-frame heldCfg line, which would otherwise zero it.
        stackEl.classList.add('hidden'); } }   // the old top-right HTML badge is retired — the count is drawn in the image now (user)
    crossEl.classList.toggle('sq', locked && !ED.on && pickAim());   // crosshair morphs + → □ ONLY over something RIGHT-CLICK can pick up (user 2026-08-02 — it used to include aimedCreature(), so it also lit up on anything killable, which is a LEFT-click action and made the square mean two different things)
    const cam = [P.x, smoothEye + camBobY, P.z];       // …plus the walk/sprint head bob (user 2026-08-19). camBobY is written further down this same tick, so this reads LAST frame's value — one frame at 60+ fps, and using it here keeps the bob out of the eye height that physics and eyeSync care about
    // ── NO MENU DRIFT (user 2026-08-11: "keep the camera still") ── the esc menu used to sway the whole camera
    // on a sine (±0.9 vox in x/z, ±0.55 in y). Because the sine is sampled from the wall clock, the frame Esc
    // was pressed picked it up mid-cycle, so the image JUMPED to wherever the wave happened to be and then
    // wandered. Unlocking now changes nothing about the view: the menu opens over exactly the frame you paused.
    const tanH = Math.tan(FOV / 2), aspect = RW / RH;
    const j = JIT[frame & 7];

    // ── uniforms ──
    const set3 = (o, v3, w) => { UF[o] = v3[0]; UF[o + 1] = v3[1]; UF[o + 2] = v3[2]; UF[o + 3] = w; };
    set3(0, [cam[0] - winOX, cam[1], cam[2] - winOZ], tanH);
    set3(4, right, aspect); set3(8, up, frame); set3(12, fwd, now / 1000);
    set3(16, [prevCam.pos[0] - winOX, prevCam.pos[1], prevCam.pos[2] - winOZ], prevCam.tanH);
    set3(20, prevCam.right, prevCam.aspect);
    const uw = waterAt(Math.floor(cam[0]), Math.floor(cam[1]), Math.floor(cam[2]));
    if (uw && !dead && !P.fly && !ED.on && !cineMode && locked) {   // ── DROWNING ── head submerged while actively swimming → lungs run out after DROWN_T
      uwT += dt;
      // out of breath = 2 damage a second, not instant death, so surfacing late is survivable
      if (uwT >= DROWN_T) { vbDrownT += dt; if (vbDrownT >= 1.0) { vbDrownT = 0; vitHurt(2, 'you drowned', true); } }
    } else if (!uw || P.fly) { uwT = 0; vbDrownT = 0; }              // surfaced or flew out → catch your breath (menu/editor/cinematic just FREEZE the clock — they don't refill it)
    // WORLD CEILING for shadow rays: nothing solid exists above the highest occupied 32-voxel slab, so a sun ray
    // can stop the moment it climbs past it instead of marching its full range through empty sky. Scanned top-down
    // over the L2 occupancy (a few hundred u32 reads — the top slabs are empty and OR to zero fast); exact by
    // construction, so shadows are bit-identical.
    let ceilB2 = 0;
    for (let y2 = B2Y - 1; y2 >= 0 && !ceilB2; y2--) {
      const w0 = (y2 * B2X) >> 5;                      // slab y2 = words for all x at each z: base (x0..63) is 2 aligned words per z row
      for (let z2 = 0; z2 < B2Z; z2++) { const b0 = w0 + ((z2 * B2X * B2Y) >> 5);
        if (bricks2[b0] | bricks2[b0 + 1]) { ceilB2 = y2 + 1; break; } }
    }
    set3(24, prevCam.up, cycleSpeed > 4 ? 10 : 64); set3(28, prevCam.fwd, (godRays ? 1 : 0) + (uw ? 2 : 0) + (moonMode ? 8 : 0) + (snowVis && !ED.on ? 16 : 0) + (ceilB2 << 8));
    // ── HELD-ITEM SUN VISIBILITY ── march the world from the eye toward the sun; one ray a frame.
    {
      let vis = 1;
      if (sun[1] > 0.02) {
        const ox = P.x, oy = smoothEye + 1, oz = P.z;
        for (let d = 1.5; d < 150; d += 1.0) {
          const gy = Math.floor(oy + sun[1] * d); if (gy >= WY) break; if (gy < 1) continue;
          // ANY voxel blocks, not just solidTab ones. Foliage is deliberately non-solid (line 915, so a crown has no
          // hitbox), but the WORLD's sun shadow is traceAll, which hits needles like anything else — so the ground
          // under a pine was shaded while the tool in your hands stayed in full sun. This line's own comment below
          // already assumed the canopy dappled it; it never could. Same test the sky ray below uses.
          if (W[gwrap(Math.floor(ox + sun[0] * d), WX) + gy * WX + gwrap(Math.floor(oz + sun[2] * d), WZ) * WX * WY]) { vis = 0; break; }
        }
      } else vis = 0;                                  // below the horizon: no direct sun on the tool either
      heldSunV += (vis - heldSunV) * (1 - Math.exp(-8 * dt));   // ~0.15 s ease so dappled canopy fades instead of strobing
      // ── HELD-ITEM SKY VISIBILITY ── the world multiplies its ambient + ground bounce by irr.g, the traced sky
      // visibility. The held item had NO such term, so a tool carried the full open-sky value everywhere: under a
      // pine crown or inside a cave it kept glowing while the world around it went dark — the biggest reason held
      // lighting did not match static lighting. Same trick as the sun ray above, one short march per direction from
      // the eye, averaging how far each gets: that ratio is exactly what the world's AO ray measures. Five rays of
      // ~16 steps is cheaper than the single 150-step sun ray. TESTED AGAINST RAW VOXELS, not solidTab — foliage is
      // deliberately non-solid (line 915) and a pine canopy is the single biggest sky occluder there is; the world's
      // traceAll hits it, so this must too.
      { const ox2 = P.x, oy2 = smoothEye + 1, oz2 = P.z;
        let acc = 0, wsum = 0;
        for (let i = 0; i < HELD_SKY_DIRS.length; i++) {
          const d3 = HELD_SKY_DIRS[i], wgt = d3[1];    // y component = cos(elevation) = this ray's share of the hemisphere
          let reach = HELD_SKY_R;
          for (let s = 1.5; s < HELD_SKY_R; s += 1.5) {
            const gy = Math.floor(oy2 + d3[1] * s); if (gy >= WY) break; if (gy < 1) continue;
            if (W[gwrap(Math.floor(ox2 + d3[0] * s), WX) + gy * WX + gwrap(Math.floor(oz2 + d3[2] * s), WZ) * WX * WY]) { reach = s; break; }
          }
          acc += (reach / HELD_SKY_R) * wgt; wsum += wgt;
        }
        heldSkyV += ((wsum > 0 ? acc / wsum : 1) - heldSkyV) * (1 - Math.exp(-8 * dt)); }   // the SAME ~0.15 s ease the sun term uses — walking under a crown must fade, not strobe
      // ── …AND THE RAIN DIMS THE TOOL IN YOUR HAND WITH THE WORLD ── heldCfg.x is "how much sun reaches the
      // player", the scalar that gates the held item's DIRECT term (see heldLight in PRE), and a cloud deck
      // overhead is precisely a reduction in how much sun reaches the player — so this is the same physical
      // quantity rather than a cosmetic match. It has to be done HERE and not in the shader: heldLight lives in
      // render/wgsl/pre.js, so the view-model is the one lit surface the sunTintR() swap in COMPOSITE cannot
      // reach, and without this line the axe keeps a full-strength sun edge while the forest it is swinging at
      // has lost a third of its own. RAIN_SUN_DIM is the SAME constant sunTintR uses, read from one declaration.
      // The stack COUNT is forced to 2 while the held-item panel is open: the badge only draws above 1, so
      // tuning it with a single item in hand would otherwise be dragging sliders against a blank screen.
      // (u.badge itself is written further down, where the held model's own corners are known.)
      UF[UF_HELDCFG] = heldSunV * (1 - RAIN_SUN_DIM * rainK); UF[UF_HELDCFG + 1] = heldSkyV; UF[UF_HELDCFG + 2] = Math.max(sbOpen() ? 2 : 0, stackShown) + (stackFull ? 100 : 0);   // …+100 flags a FULL stack for the badge's colour. An OFFSET and not a negation: blit gates the badge on z > 1.5, and a negative count would fail that test and hide the number entirely UF[UF_HELDCFG + 3] = 0;   // ── STACKBADGE ── z is the held stack count the BLIT draws beside the hand. It MUST be written here: this line runs every frame and used to zero z, which silently clobbered a write made earlier in the frame. “Spare” meant actively zeroed, not unused.
      UF[UF_LGT] = lgtMask; UF[UF_LGT + 1] = wReflK; UF[UF_LGT + 2] = lgtMask2; UF[UF_LGT + 3] = nightMask;   // ── WATER PANEL ── x = term mask (LG), y = the REFLECTION STRENGTH slider, z = the SECOND term mask (LG2 — lgt.x is full at 24 bits)   // …and w is the NIGHT PANEL's mask, read by NG() in the shaders (ui/hud.js owns it, L opens the panel). This lane was already here and was written a literal 0 every frame, so the night features needed no new uniform field — no struct reorder, no offset shift, nothing downstream to re-verify.
      // ── HIT FLASH ── published AFTER the creature stamps (see the block by UF[UF_HURTH + 3] further down), because a
      // land mammal re-stamps itself later in this same tick — bounce included — and a box measured here would
      // describe where it WAS last frame while the GPU shades where it IS now.
    }
    set3(32, sun, resetHist);
    UF[36] = RW; UF[37] = RH; UF[38] = j[0]; UF[39] = j[1];
    UF[40] = prevCam.jit[0]; UF[41] = prevCam.jit[1]; UF[42] = CW; UF[43] = CH;
    UF[44] = winOX; UF[45] = winOZ; UF[46] = gwrap(winOX, WX); UF[47] = gwrap(winOZ, WZ);
    { if (mouse0 && locked && !dead && !CRAFT.open && now - swingStart >= 570) { swingStart = now; pendKillT = swingStart + 250; }   // hold left click → continuous swinging (each auto-repeat re-arms the impact-timed hit)   // …but never THROUGH the stone age bench (user 2026-08-20): the mousedown guard in reactions.js stops a fresh click, and this stops a button that was already down from re-arming a swing every 570 ms while the player chooses
      if (mouse2 && eatHold && locked) tryEat();                  // ── HOLD RIGHT CLICK TO KEEP EATING (user 2026-08-11) ── the mousedown takes the first bite; from here the held button takes another every EAT_MS until the stack is gone. tryEat itself carries the rest of the rule (dead / editor / mid-pickup / nothing edible in hand / the bite floor), so holding a bow or a rock still just draws or winds up; `eatHold` is the one thing it cannot know — whether this press was a grab or a bite. Same auto-repeat the left button has had.
      reapDeaths(now);                                 // creatures whose red flash has run out now actually die (see tryKillCreature)
      shTick(now);                                     // …and the scroll-up hint appears ten seconds into play, and rides the esc menu (see shTick)
      if (shHideT && now >= shHideT) { shHideT = 0; if (!shVis) shEl.classList.add('hidden'); }   // its fade-out has finished — stop painting it
      tillRevert(now);                                 // …and untended tilled soil closes back over
      if (pendKillT && now >= pendKillT) { pendKillT = 0;
        const am9 = aimedCreature();                     // ANY tool: whatever the crosshair is genuinely on takes the swing (user)
        if (am9 >= 0) hitCreature(am9);                  // …otherwise chopSwing carves a grid-stamped animal's voxels out of the world and it comes apart in pieces instead of dying
        else if (chopSwing()) { playToolHit(); vitOnMine(); } else { tryKillCreature(); if (aimHitId()) playBlocked(); } }   // …and if it bit NOTHING but the crosshair was on something solid, it BOUNCED — the thud (user 2026-08-07). Nothing under the crosshair at all stays silent: a whiff is not a bounce   // it BIT something — one of the four break takes (user)   // an axe that bites a trunk spends the swing on the tree; otherwise it can still kill   // the axe LANDS on screen ~250 ms into the chop → NOW the hit registers (1 hit = 1 kill; the death poof — 4 sparks + 4 smoke columns together — fires from the creature)
      // SWING — Teardown-style 3 phases over 570 ms (user-tuned): WINDUP raises the tool up-back (0–35%),
      // the STRIKE slams it down to the MIDDLE of the screen (35–55%, accelerating), then it eases back to rest (55–100%).
      const swT = (now - swingStart) / 570;
      let wind = 0, chop = 0;
      if (swT < 1) {
        if (swT < 0.35) { const k = swT / 0.35; wind = k * k * (3 - 2 * k); }
        else if (swT < 0.55) { const k = (swT - 0.35) / 0.2; wind = 1 - k; chop = k * k; }
        else { const k = (swT - 0.55) / 0.45; chop = 1 - k * k * (3 - 2 * k); }
      }
      const clT = (now - clashT0) / 620;               // ROCK CLASH — 0..0.42 both rocks accelerate to centre, impact (sparks), then ease back
      let clashK = 0;
      // ── THE CRAFT GESTURE'S OWN DRIVE ── deliberately NOT clashK: that one is gated on the hand holding a
      // rock or a knife (see the showId test where it is applied), which is right for a clash and wrong here,
      // because one of these two hands is holding a STICK. Its own value, applied to both hands unconditionally.
      const crK = (typeof craftK === 'function') ? craftK(now) : 0;
      // IMPACT — the same sparks the clash throws, once, and from that frame the two halves are gone from the
      // hands. Latched in CRAFT.lit rather than tested against the clock, so a slow frame cannot fire it twice.
      if (crK >= 1 && CRAFT.open && !CRAFT.lit) { CRAFT.lit = true; spawnSparks(); }
      if (clT >= 0 && clT < 1) clashK = clT < 0.42 ? (clT / 0.42) * (clT / 0.42) : 1 - (() => { const q = (clT - 0.42) / 0.58; return q * q * (3 - 2 * q); })();
      if (clT >= 0.42 && clT < 1 && !clashSparked) {   // IMPACT — sparks fly and the two rocks become a stone knife
        clashSparked = true; spawnSparks(); unlockSharpEdge();
        const cs = slots[selSlot];
        if (cs && cs.it === 2 && cs.n >= 2 && KNIFE_IT) {
          cs.n -= 2; if (!cs.n) slots[selSlot] = null;
          if (!slots[selSlot]) slots[selSlot] = { it: KNIFE_IT, n: 1 };   // the newborn knife lands IN THE HAND every craft — addItem's stack-first rule silently merged repeat crafts into an existing knife stack ("the knife never spawns")
          else if (addItem(KNIFE_IT) === -1) cs.n += 2;   // nowhere to hold it (guarded at clash start, but never eat rocks for nothing)
        }
      }
      // ── THE FRUIT BEING EATEN ── advanced ONCE per frame, here, because eatAnimFrame is what retires the
      // animation when its last frame has been shown and calling it twice would step it twice. `eatIt` is the
      // fruit the strip belongs to, and everything below reads `heldIt() || eatIt` rather than `heldIt()` for
      // one reason: eating the LAST apple of a stack empties the slot on the same frame the animation starts,
      // so from that frame on the hand is officially holding nothing. Without the fallback the pose would snap
      // to the axe's, the swap would fire and drop the apple out of frame, and the core would never be seen.
      const eatF = eatAnimFrame(now), eatIt = eatF >= 0 ? eatAnim.it : 0;
      const eatBase = eatF >= 0 ? (eatAnim.base || eatAnim.it) : 0;   // the run the frame counter walks. Same as eatIt for every food that IS its own strip head; the worm's held id heads a crawl cycle instead, so its strip lives elsewhere (assets/held-items.js WORM_EAT0)
      // ── TOOL SWAP ── keyed off the ITEM, so a pickup or a craft landing in the hand animates too. The empty
      // hand a finished apple leaves behind still swaps — eatIt goes to 0 with the animation, one frame after
      // the core.
      // ── …AND OFF THE SLOT AS WELL (user 2026-08-20: "if theres 2 of the same objects in the player inventory
      // … theres no transition animation like there is with everything else") ── the item id alone cannot see a
      // scroll from one apple stack to another. A stack caps at STACK_MAX, so the next apple opens a SECOND
      // slot, and wheeling between the two changed `heldIt()` from apple to apple: no change, no swap, and the
      // hand simply cut from one to the next while every other scroll animated.
      // THE SLOT OBJECT, NOT ITS INDEX. slotTidy compacts the hotbar and moves stacks between indices without
      // anything changing in the hand, and an index test would fire a swap for that. The reference survives the
      // move, so it is stable exactly when the held thing is: eating the last apple nulls the slot (reference
      // changes -> swap), a pickup merging into the stack keeps it (no swap, the stack merely grew), and two
      // full stacks of the same fruit are two different objects (swap, which is the report).
      // ── …BUT A BITE IS NOT A SWAP (user 2026-08-21: "when the player eats, it drops down the food in hand.
      // it glitches out first thing. dont drop the food in hand. just play the eating animation") ── the note
      // four lines up already predicts this failure in the user's own words — "the swap would fire and drop the
      // apple out of frame" — and `eatIt` was added to prevent it. It prevents the ITEM half only. The SLOT half
      // was added later (2026-08-20, two stacks of the same fruit) and walked straight back into it: tryEat
      // spends the stack in the same call that arms the animation, so eating the LAST of a stack sets
      // slots[selSlot] = null on the very first frame of the chew, sNow goes object -> null, the swap arms, and
      // `hy -= 0.62 * swapF * swapF` drops the food out of frame as you bite it. First thing, every time.
      // THE CHANGE IS STILL CONSUMED — prevHeldIt/prevHeldSlot are updated whether or not the swap arms — so
      // this cannot merely DEFER the drop to a later frame. Only the arming is skipped.
      // AND ONLY WHILE THE HAND IS STILL SHOWING THE FOOD. `!heldIt() || heldIt() === eatIt` is the same pair of
      // states the showId line further down already calls the only two possible during a bite: the stack is
      // spent (0) or it still has fruit in it (eatIt). Scroll to an axe mid-chew and heldIt() is neither, so
      // that swap fires exactly as it should. When the strip finally retires, eatIt drops to 0, hNow changes
      // once more and the empty hand a finished apple leaves behind still swaps — which is the documented
      // behaviour and is untouched.
      { const hNow = heldIt() || eatIt || 0, sNow = slots[selSlot] || null;
        if (hNow !== prevHeldIt || sNow !== prevHeldSlot) { prevHeldIt = hNow; prevHeldSlot = sNow;
          if (!(eatF >= 0 && (!heldIt() || heldIt() === eatIt))) swapT0 = now; } }
      const swapR = Math.max(0, 1 - (now - swapT0) / SWAP_MS);
      const swapF = swapR * swapR * (3 - 2 * swapR);   // 1 → 0 across the swap, SMOOTHSTEPPED and not quantised: this is a camera move, not a character animation, and stepping it at 24 fps read as a stutter (user)
      const hcfg = heldCfg(heldIt() || eatIt || 1);    // the shown item's OWN pose (during a grab flight this is the grabbed item's; through a bite it stays the fruit's even once the stack is gone)
      // ── SPEAR OVERHEAD ── holding the right button cocks it back over the shoulder, ready to throw; the
      // release sends it (user). Eased over SPEAR_WIND_MS so it lifts rather than snapping into place.
      // ── HURRY ── a bow held at full draw shakes, and worse the longer you hold it: the arm is under load
      // and the shot wants taking (user). Quantised to 24 fps like every other animation here.
      const bowHold = (BOW_IT && heldIt() === BOW_IT && mouse2) ? Math.min(1, Math.max(0, (now - bowT0 - 90) / 2400)) : 0;   // starts almost as soon as you pull (user), and keeps building
      const bowShk = bowHold * bowHold * (3 - 2 * bowHold);   // smoothstepped: it eases in rather than switching on
      const spWind = (SPEAR_IT && heldIt() === SPEAR_IT && mouse2) ? Math.min(1, (now - bowT0) / SPEAR_WIND_MS) : 0;
      const spE = spWind * spWind * (3 - 2 * spWind);
      const swPitch = -0.9 * wind + 1.35 * chop + 1.15 * spE;       // windup tips the head back/up, the strike slams it forward-down; the cocked SPEAR levels FORWARD, ready to go (user)
      const cy = Math.cos(hcfg.yaw), sy = Math.sin(hcfg.yaw), cp2 = Math.cos(hcfg.pitch + swPitch), sp2 = Math.sin(hcfg.pitch + swPitch), cr2 = Math.cos(hcfg.roll), sr2 = Math.sin(hcfg.roll);
      // R = Rx(pitch)·Ry(yaw)·Rz(roll) — pitch OUTERMOST, so the gimbal singularity sits at yaw ±90°, far from the default pose (all three sliders stay distinct)
      let hx = hcfg.x * (1 + 0.06 * wind - 0.85 * chop),                       // strike pulls the anchor to the screen CENTRE…
          hy = hcfg.y + 0.22 * wind - 0.18 * chop + 0.46 * spE,                // …windup lifts it, the strike drives it down, and the SPEAR goes up over the head…
          hz = hcfg.z - 0.05 * wind + 0.18 * chop + 0.16 * spE;                // …and reaches forward into the blow — the cocked spear reaches forward too (user)
      hy -= 0.62 * swapF * swapF;                                              // SWAP: the tool drops out of frame and rises back in. Squared so it leaves fast and settles softly.
      hz -= 0.10 * swapF;
      const CAM_BOB = 0.55, CAM_BOB_RUN = 0.45;        // vertical travel in voxels at walking pace, and how much more a full sprint adds.
                                                       // ── 0.13 -> 0.55 (user 2026-08-19: "I dont see any bob on the player", then "the tool bobs
                                                       // yes, I need the camera to bob as well") ── the mechanism was working the whole time: measured
                                                       // walking, camBobY swung -0.115..+0.124 with bobAmp at 1.0. It was simply too small to SEE. A
                                                       // held tool sits inches from the lens, so its bob is magnified by parallax; the camera's own is a
                                                       // world translation, and 0.13 voxels is 1.3 cm — under a pixel of apparent motion at any sane FOV.
                                                       // 0.55 is 5.5 cm, which is roughly a real stride's head travel and is what reads on screen.
      const spd2 = Math.hypot(P.hvx, P.hvz);           // view-model BOB — phase tied to distance walked (smooth at any speed), amplitude eased in/out
      bobAmp += ((P.onGround && !P.fly ? Math.min(1, spd2 / WALK) : 0) - bobAmp) * (1 - Math.exp(-8 * dt));
      bobPh += spd2 * dt * 0.225;                      // slower phase + wide sweep = long smooth strides
      // ── AND THE CAMERA TAKES A SHARE OF IT (user 2026-08-19: "add camera bob to the player while he is
      // walking/sprinting. dont apply this to flying") ── off the SAME bobPh/bobAmp the hand uses, so the head
      // and the tool can never drift out of step, and the flying/airborne case is already handled: bobAmp eases
      // to 0 whenever `P.onGround && !P.fly` is false, so a jump or a flight settles the camera on its own.
      // VERTICAL ONLY, and deliberately small. A head bob was tried on 2026-08-18 and reverted by the user the
      // same day; lateral sway is the part that reads as seasickness, so it is left out entirely, and 0.13
      // voxels is 1.3 cm at this scale — about what a real stride does, rather than the exaggerated dip games
      // usually add. cos at TWICE the stride frequency because both footfalls dip the head, not every other one.
      // HALF RATE (user 2026-08-19: "halve the rate of the bob up and down rate. it bobs too fast") — it was
      // cos(bobPh * 2), i.e. a dip per FOOTFALL, which is what a real head does but reads as a jitter at this
      // stride frequency. cos(bobPh) is one dip per STRIDE. The held item keeps its own doubled cadence, so the
      // hand still ticks along at the walking pace while the head swings under it.
      // SPRINT is the same curve pushed a little further rather than a second rule: bobAmp saturates at WALK,
      // so the extra term reads how far PAST walking pace the player is and adds up to 45% more travel.
      camBobY = -Math.cos(bobPh) * CAM_BOB * bobAmp * (1 + CAM_BOB_RUN * Math.max(0, Math.min(1, spd2 / WALK - 1)));
      // …computed ONCE, into heldBob, because the health row rides it too (see the heart lane at the foot of
      // this file). Written as its own pair rather than read back out of hx/hy: those carry the tool's pose,
      // swing, swap and bow tremble as well, and the row wants the BOB and nothing else.
      heldBob[0] = Math.sin(bobPh) * 0.075 * bobAmp + Math.sin(now * 0.0013) * 0.0069 + Math.sin(now * 0.00073 + 1.7) * 0.0039;   // + idle breathing sway (tripled)
      heldBob[1] = -Math.abs(Math.cos(bobPh)) * 0.028 * bobAmp + Math.sin(now * 0.0017 + 0.9) * 0.0069 + Math.sin(now * 0.00091) * 0.0036;
      hx += heldBob[0];
      // …+ the draw tremble. SLOW and CONTINUOUS (user: smoother, not violent): a held arm drifts, it does
      // not buzz — so this is two long rates at a third of the old amplitude, and NOT stepped to 24 fps.
      // Quantising it made a 12 Hz judder, which is exactly what read as violent; the view-model bob above
      // is left continuous for the same reason.
      if (bowShk > 0) { hx += (Math.sin(now * 0.0062) + 0.5 * Math.sin(now * 0.0111 + 2.1)) * 0.0150 * bowShk;   // …travel DOUBLED (user) — same slow rates, twice the distance
                        hy += (Math.sin(now * 0.0049 + 1.1) + 0.5 * Math.sin(now * 0.0093) ) * 0.0120 * bowShk; }
      hy += heldBob[1];
      let AX = [cr2 * cy, sr2 * cp2 + cr2 * sy * sp2, sr2 * sp2 - cr2 * sy * cp2];
      let AY = [-sr2 * cy, cr2 * cp2 - sr2 * sy * sp2, cr2 * sp2 + sr2 * sy * cp2];
      let AZ = [sy, -cy * sp2, cy * cp2];
      let showId = dead ? 0 : (slots[selSlot] ? slots[selSlot].it : 0);          // held item = the SELECTED hand slot (0 = empty hand)
      if (clashK > 0 && (showId === 2 || showId === KNIFE_IT)) { hx += (0.13 - hx) * clashK; hy += (-0.17 - hy) * clashK; hz += (0.92 - hz) * clashK; }
      if (crK > 0) { hx += (0.13 - hx) * crK; hy += (-0.17 - hy) * crK; hz += (0.92 - hz) * crK; }   // …and the CRAFT drives it whatever it is holding — a stick meets the rock halfway instead of staying put   // right rock drives to centre; the newborn knife eases back from there
      if (grabAnim && !grabAnim.left) {                // FLY-TO-HAND — the pickup glides in as ITS OWN object; the hand keeps whatever it was holding (user)
        const k = Math.min(1, (now - grabAnim.t0) / GRAB_MS);
        const e2 = k * k * (3 - 2 * k);
        if (!slots[selSlot]) {
          // ── EMPTY HAND: STRAIGHT INTO THE HAND (user) ── the original flight, restored. The item IS the held
          // item for the length of the trip: its world spot is expressed in the HELD frame and eased into the
          // held pose, so it lands exactly where the hand holds it, at matching angular size the whole way.
          // The version this replaces flew a world-space ghost toward
          //   cam + right*hx + up*hy + fwd*hz
          // which mixes units — hx/hy/hz are HELD units and world = held ÷ scale (see the throw, which divides).
          // Multiplying instead of dividing collapsed the target onto the camera, so the pickup flew to the
          // middle of the player and only reached the hand when the flight ended and the item was granted.
          const rx = grabAnim.x - cam[0], ry = grabAnim.y - cam[1], rz = grabAnim.z - cam[2];
          const sxc = (rx * right[0] + ry * right[1] + rz * right[2]) * hcfg.scale;
          const syc = (rx * up[0] + ry * up[1] + rz * up[2]) * hcfg.scale;
          const szc = (rx * fwd[0] + ry * fwd[1] + rz * fwd[2]) * hcfg.scale;
          hx = sxc + (hx - sxc) * e2; hy = syc + (hy - syc) * e2; hz = szc + (hz - szc) * e2;
          const ca3 = Math.cos(grabAnim.aPh), sa3 = Math.sin(grabAnim.aPh);   // the hover SPIN slerped into the held orientation, so it settles rather than snapping
          const qg = qslerp(m2q([ca3 * right[0] + sa3 * right[2], ca3 * up[0] + sa3 * up[2], ca3 * fwd[0] + sa3 * fwd[2]],
                                [sa3 * right[0] - ca3 * right[2], sa3 * up[0] - ca3 * up[2], sa3 * fwd[0] - ca3 * fwd[2]],   // Y = Z×X — matches the (right-handed) hover frame
                                [right[1], up[1], fwd[1]]),
                            m2q(AX, AY, AZ), e2);
          const M = q2m(qg); AX = M[0]; AY = M[1]; AZ = M[2];
          showId = dead ? 0 : grabAnim.it;
          grabGhost = null;                            // it is the HELD item now, not a world object — drawing both would double it
        } else {
          // ── HAND FULL ── it is drawn in to the same point on the player a carved chunk is, as its own world
          // object, so the tool you are holding stays out (user). FULL SIZE the whole way on the same gentle
          // arc: shrinking it read as the item evaporating rather than being picked up.
          const tw = [P.x, smoothEye + PH.absorbY, P.z];
          grabGhost = { it: grabAnim.it, ph: grabAnim.aPh || 0,
            x: grabAnim.x + (tw[0] - grabAnim.x) * e2,
            y: grabAnim.y + (tw[1] - grabAnim.y) * e2 + Math.sin(e2 * Math.PI) * 3,
            z: grabAnim.z + (tw[2] - grabAnim.z) * e2,
            vs: 1 };
        }
        // …and the hand KEEPS WHAT IT WAS HOLDING (user). This used to select the slot the pickup landed in,
        // so walking over a twig put the twig in your hand and your axe away — reversed here: the pickup
        // goes to its slot and the tool stays out. An EMPTY hand still takes it, since there is nothing to lose.
        if (k >= 1) { const wasEmpty = !slots[selSlot];
          const as = addItem(grabAnim.it);
          if (as >= 0 && wasEmpty) { selSlot = as;
            // ── AND IT DOES NOT "SWAP" IN (user 2026-08-20: "when picking up an item, it glitches … it like
            // glitches downwards") ── the same double animation the craft glide had. The empty-hand branch
            // above eases the item's world position INTO the held pose, so at k = 1 it is already sitting
            // exactly where the hand holds it; granting it then changed heldIt(), the view-model's swap fired
            // on the NEXT frame, and the object the player had just watched fly into their fist was dropped
            // 0.62 out of frame and lifted back in. Measured: a 0.76 dip on an empty-hand pickup.
            // The swap comment upstream says a pickup landing in the hand should animate — and it should, but
            // the FLIGHT is that animation now. It was written when the flight flew to the middle of the
            // player and only reached the hand at the end (see the note in this very branch), which is when a
            // swap was the only thing that presented the item at all.
            // Consuming BOTH halves of the swap key, for the reason craftLand does: leaving the slot half
            // would fire the animation this line exists to suppress.
            prevHeldIt = heldIt() || 0; prevHeldSlot = slots[selSlot] || null; swapT0 = -1e9; }
          grabAnim = null; grabGhost = null; }
      } else grabGhost = null;
      set3(48, [hx, hy, hz], hcfg.scale);
      heldOff = [hx, hy, hz];                          // published for shootArrow: the bow's own place in the frame
      if (BOW_IT && showId === BOW_IT) {            // BOW DRAW (user): the strip swaps under the one held pose…
        const bf = bowFrame(now);                   // …and once the string is loosed the bare strip takes over, so the arrow is GONE from the bow (user)
        if (bowLoosed && bowAtRest(now)) { bowLoosed = false; playBowReload(); }   // back at rest: the bow is nocked again, ready for the next draw — and you hear the next arrow go on (user)
        showId = ((BOW_NOCK && bowLoosed && bf > 0) ? BOW_NOCK : BOW_IT) + bf;
      }
      // ── THE APPLE GOES DOWN TO A CORE ── the same swap the bow's draw makes, under the same held pose: one
      // run of consecutive item ids, indexed by a frame counter. TWO ids are allowed to be showing when it
      // starts and they are the only two that can be: `eatIt`, meaning the stack still has fruit in it, and 0,
      // meaning that bite was the last one. Anything else means the player scrolled to another slot mid-chew,
      // and then the strip simply is not drawn — the tool they switched to wins, and the animation retires on
      // its own clock a few frames later.
      if (eatF >= 0 && !dead && (showId === eatIt || showId === 0)) showId = eatBase + eatF;
      shownIt = showId;
      if (eatF < 0 && WORM_NFRAMES && showId >= WORM_ITEM0 && showId < WORM_ITEM0 + WORM_NFRAMES) showId = WORM_ITEM0 + Math.floor(now * 0.024) % WORM_NFRAMES;   // eatF < 0: a worm being EATEN is showing its bite-down strip, and squiggling it would drop the chew back onto the crawl cycle mid-bite   // the caught worm SQUIGGLES — its 24 fps crawl cycle keeps playing in the hand (and through the grab flight)
      // …and the RIGHT hand's half goes with the left one at impact (user 2026-08-19): both halves are consumed
      // into the bench, so neither hand may still be holding one while the preview hangs between them.
      if (typeof CRAFT !== 'undefined' && CRAFT.open && CRAFT.lit) showId = 0;
      set3(52, AX, showId);
      // ── THE STACK BADGE RIDES THE MODEL'S TOP-RIGHT CORNER (user 2026-08-17: "always in the top right") ──
      // BLIT draws the x{n} beside the hand and used to place it at a fixed offset from the ANCHOR, which is
      // the middle of the model's box — so it sat correctly on one item and half inside the next. The corner
      // is a projection, and everything it needs is right here and nowhere else: the anchor and voxel scale
      // (set3(48) above), the item's three local axes in camera space, and its GRID DIMENSIONS off the item
      // table. All eight corners are projected rather than a guessed one, because which corner reads as
      // "top right" changes with the pose — the axes are rotated by yaw/pitch/roll and the bob.
      // The projection is character for character the one TRACE and BLIT use (camera y is DOWN here, hence the
      // negate), so the pixel handed over is the pixel the badge lands on.
      { const bi = (itemsRef && itemsRef[showId - 1]) || null, bs = hcfg.scale;
        if (bi && bi.w && UF[50] > 0.05) {             // pickA.z: the same in-front-of-the-camera gate BLIT applies before it draws anything
          // The model is centred on the anchor with half-extents vs*(w/2, d/2, h/2) along its three local
          // axes (see the held-item DDA in COMPOSITE), so its apparent half-size on screen is the sum of the
          // three axes' contributions to that screen direction. Measured at the ANCHOR'S depth rather than
          // per corner, deliberately: at a viewmodel's z (~1) the perspective is steep enough that a corner
          // swinging toward the eye projects far outside the shape you can actually see, and a box built from
          // those corners put the badge half a screen away from the rock. This is the apparent box.
          const hx2 = bi.w * 0.5 * bs, hy2 = bi.d * 0.5 * bs, hz2 = bi.h * 0.5 * bs;
          const tH = UF[3], asp = UF[7], CWb = UF[42], CHb = UF[43], az = Math.max(0.05, hz);
          const exX = Math.abs(hx2 * AX[0]) + Math.abs(hy2 * AY[0]) + Math.abs(hz2 * AZ[0]);
          const exY = Math.abs(hx2 * AX[1]) + Math.abs(hy2 * AY[1]) + Math.abs(hz2 * AZ[1]);
          // …and the projection is CALIBRATED, not assumed: writing the bare anchor here and screenshotting it
          // is what settled the y sign (the anchor landed on the rock at 726 and 371 px away at the other
          // sign). Larger camera y is UP the screen in this space, whatever the note in BLIT says.
          const cxP = (((hx / az) / (tH * asp)) * 0.5 + 0.5) * CWb, cyP = (0.5 - ((hy / az) / tH) * 0.5) * CHb;
          const rxP = ((exX / az) / (tH * asp)) * 0.5 * CWb, ryP = ((exY / az) / tH) * 0.5 * CHb;
          const sbC = sbFor(showId);                   // this item's OWN trim (ui/hud.js). Keyed on the DRAWN id, so an eat strip or a bow draw — whose frames all share a name and therefore one placement — cannot shift the badge mid-animation
          const gp = Math.max(2, Math.floor(CHb / 320 * Math.max(0.2, sbC.size)));   // the same glyph pixel BLIT derives, so the nudge is in badge pixels and holds at any resolution
          // …AND IT IS KEPT ON SCREEN. The tool poses sit the held model hard against the right edge, so its own
          // top-right corner is genuinely off the canvas for the fruit — measured at 2212 px on a 2240 px
          // canvas, which would have run three glyphs into the bezel. A badge that has to be readable cannot
          // follow the corner past the edge, so it stops at it. The run's own width is what it is clamped by.
          // ── THE RUN IS ONE DIGIT, ALWAYS ── this used to be measured from the live count via a `nB9 >= 10`
          // test, which was dead arithmetic dressed up as a safeguard: STACK_MAX is 8 (sim/hands.js), so the
          // count BLIT is handed never reaches two digits and its own `digB` is permanently 1. Spelling the
          // width as a constant says that out loud and, because it cannot change, hands the clamp the
          // stability the old expression only looked like it had — the count no longer moves the right edge,
          // so a placement tuned with the panel open (which forces the count to 2) still holds when it closes.
          // Raise this the day the cap goes past 9, together with GLYPH5's bounds in blit.js.
          const runW = (1 + 1) * 6 * gp;
          // ── THE CORNER IS CLAMPED FIRST, THEN THE TRIM IS ADDED (user 2026-08-18: the sliders "barely move") ──
          // this was one expression, `min(hi, max(lo, corner + trim))`, and the min was the OUTER operation, so
          // the right-edge clamp had the last word over the slider. For any item whose corner sits past that
          // edge — which is most of them, the tool poses hold the model hard right, and the 2212-on-2240
          // measurement above is exactly such a case — a leftward nudge only walked the sum back DOWN toward a
          // ceiling that kept returning the same pinned pixel. Anything more than 20 badge px past the edge was
          // frozen solid across the whole slider, and the +1 default sat inside the dead zone, so the panel
          // opened on a slider that did nothing. Pulling the corner on screen BEFORE the trim is added gives the
          // trim a live value to work from: the outer clamp then only stops the badge leaving the canvas, which
          // is all it was ever there to do, and a nudge left always moves the badge left.
          const bLoX = 2, bHiX = CWb - runW - 2, bLoY = 2, bHiY = CHb - 5 * gp - 2;
          const bCX = Math.min(bHiX, Math.max(bLoX, cxP + rxP)), bCY = Math.min(bHiY, Math.max(bLoY, cyP - ryP));
          // …and the trim's OWN unit is the unfloored glyph pixel. gp is floored because BLIT draws whole screen
          // pixels and a fractional one would shimmer, but a floored NUDGE is a second quantization on top of
          // that: it pinned the step to exactly 1 px at every height below 960 and made the x/y sliders jump
          // sideways whenever the size slider crossed a floor threshold. The badge's final position is snapped
          // to a whole pixel by BLIT regardless, so the nudge can be smooth here for free.
          const gpN = Math.max(2, CHb / 320 * Math.max(0.2, sbC.size));
          UF[UF_BADGE] = Math.min(bHiX, Math.max(bLoX, bCX + sbC.x * gpN));           // the model's own top-right, pulled on screen, plus this item's trim
          UF[UF_BADGE + 1] = Math.min(bHiY, Math.max(bLoY, bCY + sbC.y * gpN));
        } else { UF[UF_BADGE] = -1e4; UF[UF_BADGE + 1] = -1e4; }   // no item, or it is behind the eye: park the glyphs off screen rather than at a stale pixel
        { const sbD = sbFor(showId); UF[UF_BADGE + 2] = sbD.size; UF[UF_BADGE + 3] = sbD.tilt; } }
      set3(56, AY, snowFallAcc);                     // u.pickY.w = integrated snow fall
      set3(60, AZ, freezeK);                        // u.pickZ.w = gradual freeze 0..1
      { // ── LEFT HAND ── shows the second rock when dual-wielding (selected rock stack n ≥ 2); also hosts a 2nd-rock grab flight
        //   (The arrow used to be drawn resting here; the user took it back off the bow. It is still
        //   launched as a projectile on release — see shootArrow.)
        const lgrab = grabAnim && grabAnim.left && !dead;
        {
        // ── AND THE OFF-HAND HOLDS THE OTHER HALF OF A CRAFT PAIR (user 2026-08-19) ── dualIt draws the SAME
        // stack twice, so it can only ever show a second of what the right hand holds; a rock-and-stick pair is
        // two different items in two different slots, so the id has to be looked up rather than assumed. It
        // also takes PRECEDENCE below, because a split stack is a pose the player asked for. craftOther is
        // whichever half is NOT selected, so the gesture reads the same whichever hand the player filled first,
        // which is the user's own rule. It shows as soon as the pair EXISTS, so the bench advertises itself.
        // ── AND ONLY WHILE THE BENCH GESTURE IS ACTUALLY RUNNING (user 2026-08-20) ── this used to show the
        // moment a rock and a stick were both in the hotbar, on the argument that the bench should advertise
        // itself (2026-08-19). It is the same thing the player is now reporting as dual wield they did not ask
        // for: carrying materials filled the off hand on its own. CRAFT.open is the honest gate — the two
        // halves are shown meeting each other because the gesture is happening, not as a standing hint.
        const cpair = (typeof CRAFT !== 'undefined' && CRAFT.open && typeof craftPair === 'function') ? craftPair() : null;
        const craftOther = cpair ? (slots[selSlot] && slots[selSlot].it === 2 ? 3 : 2) : 0;
        // Once the two halves have met they are GONE — the same disappearance the rock clash does when its two
        // rocks become a knife. From CRAFT.lit the hands are empty and only the preview remains.
        const craftGone = (typeof CRAFT !== 'undefined') && CRAFT.open && CRAFT.lit;
        // ── A PICKUP NEVER EVICTS WHAT THE HAND IS ALREADY HOLDING (user 2026-08-19: "the rock that is currently
        // in hand dissapears, while the new rock in the terrain takes its place. have the rock thats currently
        // in hand stay put, while the new rock stacks on ontop") ── this read `lgrab ? grabAnim.it : …`, so the
        // moment a flight started the off-hand STOPPED drawing the rock it was holding and drew the incoming
        // one instead. With two identical rocks that reads exactly as the report: the held one vanishes and the
        // one off the ground takes its place, when what actually happens is that the stack grows by one.
        // The resting item now wins. A flight only takes the hand when the hand is EMPTY — which is the case
        // the fly-to-hand animation was written for, and the case where there is nothing to evict.
        const lRest = craftGone ? 0 : (dualIt || craftOther || 0);   // what this hand is holding on its own account
        const lid = lRest || (lgrab ? grabAnim.it : 0);
        if (lid !== prevLeftIt) { prevLeftIt = lid; lSwapT0 = now; }   // …the off hand's swap clock, armed on ITS item changing   // …the same predicate the badge count subtracts by, so the hand and the number can never disagree about whether a rock is being dual-wielded
        if (!lid) { UF[1095] = 0; UF[UF_VITG + 1] = 0; }   // …and the off-hand badge goes with the hand: a count left standing would draw glyphs beside nothing                      // pick2A.w = 0 → zero bounding radius hides the left hand (pick2 sits after the 64 drop slots: 1092..1107)
        else {
          const c2 = heldCfg(lgrab ? grabAnim.it : (dualIt || craftOther || 2));   // mirrored pose OF WHATEVER IS SHOWN: anchor x negated, yaw/roll negated (still a proper rotation — no handedness flip). It read heldCfg(2) unconditionally, which hung a stick on the rock's pose the moment the off-hand could hold something else
          const cy4 = Math.cos(-c2.yaw), sy4 = Math.sin(-c2.yaw), cp4 = Math.cos(c2.pitch), sp4 = Math.sin(c2.pitch), cr4 = Math.cos(-c2.roll), sr4 = Math.sin(-c2.roll);
          let LAX = [cr4 * cy4, sr4 * cp4 + cr4 * sy4 * sp4, sr4 * sp4 - cr4 * sy4 * cp4];
          let LAY = [-sr4 * cy4, cr4 * cp4 - sr4 * sy4 * sp4, cr4 * sp4 + sr4 * sy4 * cp4];
          let LAZ = [sy4, -cy4 * sp4, cy4 * cp4];
          // ── THE OFF-HAND TWIG STANDS UP (user 2026-08-19: "flip the left hand stick vertically, it apears
          // upside down. the leaf should be pointing upwards") ── the left hand is the right hand's pose
          // MIRRORED (anchor x negated, yaw and roll negated), which is right for a rock and reads as upside
          // down for anything with a top and a bottom. A twig has both: the leaf is the top.
          // THE AXIS IS NOT A GUESS — stick_1.vox is 8 x 5 x 3 with the leaf at x 5..7 and the wood at x 0..5,
          // so the twig's LENGTH is the width axis. Turning it end over end therefore means rotating about a
          // PERPENDICULAR axis. The first attempt negated LAY and LAZ, which is a rotation about the width
          // axis — it spins the twig about its own length and cannot swap the ends, which is exactly what it
          // did: nothing moved. Negating LAX and LAZ turns it about the DEPTH axis, which does swap them.
          // Two axes flipped, never one, so the basis stays right-handed and the model is turned OVER rather
          // than mirrored into its own reflection.
          // If it ever needs the other way up, the remaining perpendicular is LAX/LAY (about the height axis).
          // Only the twig: a rock has no up, and flipping it would be a change with nothing to show for it.
          if (lid === 3 || (STICK_BLOS_IT && lid === STICK_BLOS_IT)) {
            LAX = [-LAX[0], -LAX[1], -LAX[2]]; LAZ = [-LAZ[0], -LAZ[1], -LAZ[2]];
          }
          let lx = -c2.x - Math.sin(bobPh) * 0.075 * bobAmp - Math.sin(now * 0.0013 + 0.6) * 0.0069 - Math.sin(now * 0.00073 + 2.3) * 0.0039,
              ly = c2.y - Math.abs(Math.cos(bobPh)) * 0.028 * bobAmp + Math.sin(now * 0.0017 + 2.1) * 0.0069 + Math.sin(now * 0.00091 + 1.1) * 0.0036,
              lz = c2.z;
          if (clashK > 0) { lx += (-0.13 - lx) * clashK; ly += (-0.17 - ly) * clashK; lz += (0.92 - lz) * clashK; }   // left rock mirrors the drive to centre
          if (crK > 0) { lx += (-0.13 - lx) * crK; ly += (-0.17 - ly) * crK; lz += (0.92 - lz) * crK; }   // …and mirrors the CRAFT drive the same way
          // ── AND THE OFF HAND SWAPS LIKE THE RIGHT ONE (user 2026-08-19: "the left hand should share similar
          // mechanics to the right hand") ── the right hand drops its tool out of frame and lifts the new one
          // back in whenever the held item changes; the left hand simply POPPED, so a second rock or the other
          // half of a craft pair appeared out of nothing. Same curve and the same two constants, on this hand's
          // OWN clock: the hands change item independently, and sharing swapT0 would make picking up a rock
          // twitch the tool you are holding.
          // NOT during a grab flight — that already animates the item in from the world, and a swap on top of it
          // would drop the thing mid-arrival.
          if (!lgrab) {
            const lSwapR = Math.max(0, 1 - (now - lSwapT0) / SWAP_MS);
            const lSwapF = lSwapR * lSwapR * (3 - 2 * lSwapR);
            ly -= 0.62 * lSwapF * lSwapF; lz -= 0.10 * lSwapF;
          }
          // ── …AND WHEN IT IS NOT EMPTY, THE INCOMING ONE FLIES AS ITS OWN OBJECT ── the same split the RIGHT
          // hand makes: an empty hand takes the item directly (the block below), a full hand watches it arrive
          // as a world object and keeps holding what it had. Without this the flight moved the RESTING rock
          // along the path, which is the eviction the user reported wearing a different disguise.
          // The target is this hand's own world point — held units divided by the scale, which is the
          // conversion the throw uses and the inverse of the one four lines below.
          if (lgrab && lRest) {
            const k = Math.min(1, (now - grabAnim.t0) / GRAB_MS);
            const e2 = k * k * (3 - 2 * k);
            const inv = 1 / Math.max(1e-4, c2.scale);
            const tw = [cam[0] + (right[0] * lx + up[0] * ly + fwd[0] * lz) * inv,
                        cam[1] + (right[1] * lx + up[1] * ly + fwd[1] * lz) * inv,
                        cam[2] + (right[2] * lx + up[2] * ly + fwd[2] * lz) * inv];
            grabGhost = { it: grabAnim.it, ph: grabAnim.aPh || 0,
              x: grabAnim.x + (tw[0] - grabAnim.x) * e2,
              y: grabAnim.y + (tw[1] - grabAnim.y) * e2 + Math.sin(e2 * Math.PI) * 3,
              z: grabAnim.z + (tw[2] - grabAnim.z) * e2, vs: 1 };   // FULL SIZE the whole way, like the right hand's: shrinking reads as the item evaporating rather than being carried in
            if (k >= 1) { addItem(grabAnim.it); grabAnim = null; grabGhost = null; }   // …and it joins the stack, which is what makes it read as landing ON the rock already there
          }
          if (lgrab && !lRest) {                     // fly from the world spot into the LEFT rest pose — same glide as the right hand
            const k = Math.min(1, (now - grabAnim.t0) / GRAB_MS);
            const e2 = k * k * (3 - 2 * k);
            const rx = grabAnim.x - cam[0], ry = grabAnim.y - cam[1], rz = grabAnim.z - cam[2];
            const sxc = (rx * right[0] + ry * right[1] + rz * right[2]) * c2.scale;
            const syc = (rx * up[0] + ry * up[1] + rz * up[2]) * c2.scale;
            const szc = (rx * fwd[0] + ry * fwd[1] + rz * fwd[2]) * c2.scale;
            lx = sxc + (lx - sxc) * e2; ly = syc + (ly - syc) * e2; lz = szc + (lz - szc) * e2;
            const ca3 = Math.cos(grabAnim.aPh), sa3 = Math.sin(grabAnim.aPh);
            const q = qslerp(m2q([ca3 * right[0] + sa3 * right[2], ca3 * up[0] + sa3 * up[2], ca3 * fwd[0] + sa3 * fwd[2]],
                                 [sa3 * right[0] - ca3 * right[2], sa3 * up[0] - ca3 * up[2], sa3 * fwd[0] - ca3 * fwd[2]],
                                 [right[1], up[1], fwd[1]]),
                             m2q(LAX, LAY, LAZ), e2);
            const M = q2m(q); LAX = M[0]; LAY = M[1]; LAZ = M[2];
            if (k >= 1) { addItem(grabAnim.it); grabAnim = null; }
          }
          set3(1092, [lx, ly, lz], c2.scale);
          set3(1096, LAX, lid);
          set3(1100, LAY, 0);
          set3(1104, LAZ, 0);
          // ── AND THE OFF-HAND'S OWN STACK BADGE (user 2026-08-19: "the rock in the left hand doesnt have the
          // stack number") ── the right hand's placement, verbatim, against THIS hand's numbers: its anchor,
          // its axes, its scale and its item. A badge is a property of a held STACK, and the off-hand holds
          // one whenever it is dual-wielding rocks or showing the other half of a craft pair.
          // The COUNT is the same stack the right hand would report if this item were selected — for the
          // dual-wield that is the selected rock stack itself, and for a craft pair it is the counterpart's
          // own slot. Read off the slots rather than from a latch, because nothing animates it.
          { const bi2 = (itemsRef && itemsRef[lid - 1]) || null, bs2 = c2.scale;
            // ── DUAL WIELD SHOWS NO OFF-HAND BADGE, AND THAT IS NOT AN OMISSION ── the two hands are ONE stack
            // drawn twice, and the right hand's own count already subtracts the rock this hand is holding (see
            // the heldCfg.z write). So the off-hand holds exactly one rock, a badge of 1 is never drawn anyway,
            // and reporting the whole stack here would have the two hands claiming 2 and 2 for two rocks.
            // Measured before this: right 1, left 2.
            // A CRAFT PAIR is the opposite case — the counterpart is its OWN slot, so its count is real and the
            // badge means what it says. cpair names both slots, so the off-hand's is simply the one that is not
            // selected, which needs no search and cannot pick up a third slot that happens to hold the same item.
            let n2 = 0;
            if (!dualIt && cpair) { const si2 = (cpair.rock === selSlot) ? cpair.stick : cpair.rock;
              if (slots[si2]) n2 = slots[si2].n | 0; }
            if (bi2 && bi2.w && n2 > 1 && lz > 0.05) {
              const tH2 = UF[3], asp2 = UF[7], CW2 = UF[42], CH2 = UF[43], az2 = Math.max(0.05, lz);
              const hx3 = bi2.w * 0.5 * bs2, hy3 = bi2.d * 0.5 * bs2, hz3 = bi2.h * 0.5 * bs2;
              const ex3 = Math.abs(hx3 * LAX[0]) + Math.abs(hy3 * LAY[0]) + Math.abs(hz3 * LAZ[0]);
              const ey3 = Math.abs(hx3 * LAX[1]) + Math.abs(hy3 * LAY[1]) + Math.abs(hz3 * LAZ[1]);
              const cx3 = (((lx / az2) / (tH2 * asp2)) * 0.5 + 0.5) * CW2, cy3 = (0.5 - ((ly / az2) / tH2) * 0.5) * CH2;
              const rx3 = ((ex3 / az2) / (tH2 * asp2)) * 0.5 * CW2, ry3 = ((ey3 / az2) / tH2) * 0.5 * CH2;
              const sb2 = sbFor(lid);
              const gp2 = Math.max(2, Math.floor(CH2 / 320 * Math.max(0.2, sb2.size)));
              const run2 = (1 + 1) * 6 * gp2;
              const lo2X = 2, hi2X = CW2 - run2 - 2, lo2Y = 2, hi2Y = CH2 - 5 * gp2 - 2;
              const c2X = Math.min(hi2X, Math.max(lo2X, cx3 + rx3)), c2Y = Math.min(hi2Y, Math.max(lo2Y, cy3 - ry3));
              const gpN2 = Math.max(2, CH2 / 320 * Math.max(0.2, sb2.size));
              UF[UF_BADGE2] = Math.min(hi2X, Math.max(lo2X, c2X + sb2.x * gpN2));
              UF[UF_BADGE2 + 1] = Math.min(hi2Y, Math.max(lo2Y, c2Y + sb2.y * gpN2));
              UF[UF_BADGE2 + 2] = sb2.size; UF[UF_BADGE2 + 3] = sb2.tilt;
              UF[UF_VITG + 1] = n2 + (n2 >= STACK_MAX ? 100 : 0);   // vitG.y — the off-hand count, GOLD at a full stack exactly as the right hand's is
            } else { UF[UF_VITG + 1] = 0; }
          }
        } }
      }
    }
    { // ══ THE STONE AGE BENCH'S PREVIEW ══ the tool being chosen, hovering between the two hands (user
      // 2026-08-19). It is a THIRD held item — the same DDA, the same lighting, the same uniform layout as the
      // two hands (see UF_PICK3 in render/buffers.js) — rather than a drop slot or a 2D icon, for two reasons:
      // a drop is depth-tested against the world and would be swallowed by a wall the player happened to face,
      // and an icon would not be the object. This is the actual voxel model of the actual tool.
      // CENTRED AND SLIGHTLY HIGH: x = 0 puts it on the crosshair, between the left hand at -x and the right at
      // +x, and it sits a little further out than either so it reads as held up BETWEEN them rather than as a
      // third thing in one of them.
      // No bob and no sway: the hands drift with the walk, and a chooser that drifted with them would be hard
      // to read. It turns slowly on its own axis instead, which is what makes it read as being INSPECTED.
      const cOpen = (typeof CRAFT !== 'undefined') && CRAFT.open;
      const cIt = (cOpen && CRAFT.lit) ? craftItem() : 0;   // …only from IMPACT: while the two halves are still closing on each other there is nothing to preview yet, and a tool hanging there before they met would give the result away before the gesture finished
      if (!cIt) UF[UF_PICK3 + 3] = 0;                  // pick3A.w = 0 → zero bounding radius, the same way the left hand hides
      else {
        const c3 = heldCfg(cIt);
        const spin = (now - CRAFT.t0 - CRAFT_IMPACT) * 0.0016;   // a slow turn, so every face of the tool comes round
        const ease = Math.min(1, Math.max(0, (now - CRAFT.t0 - CRAFT_IMPACT) / 220));   // measured from IMPACT, not from the click, so it grows out of the spark rather than part-way through its own ease
        const cy5 = Math.cos(spin), sy5 = Math.sin(spin), cp5 = Math.cos(c3.pitch), sp5 = Math.sin(c3.pitch);
        let CAX = [cy5, 0, -sy5];
        let CAY = [sy5 * sp5, cp5, cy5 * sp5];
        let CAZ = [sy5 * cp5, -sp5, cy5 * cp5];
        // FURTHER OUT AND SMALLER THAN A HELD TOOL, not bigger. The first cut sat it at hand distance and 1.35x
        // scale, and dead centre with nothing clipping it that filled half the frame — apparent size is
        // scale/z, so being centred already makes it read larger than the same model does in the hand. At 0.85x
        // and 1.35 out it comes to ~60% of a held tool's apparent size: clearly the object, clearly being
        // offered rather than wielded, and it leaves the view behind it readable while you choose.
        // ── AND ON ENTER IT GLIDES HOME (user 2026-08-19: "have it smoothly guide from the middle to their
        // right hand, instead of instantly") ── CRAFT.fly is the moment Enter landed; from there the preview
        // eases from where it hangs to the right hand's own rest pose, and craftLand — which is what actually
        // spends the halves and hands the tool over — is called on ARRIVAL. So the object that lands in the
        // fist is the one that was flying, rather than a new one blinking into place while a ghost finishes
        // its trip. heldCfg(cIt) is the same pose the hand will hold it at, so the two meet exactly.
        // ── THE GLIDE JITTERED, AND BOTH CAUSES WERE THE HOVER STILL RUNNING UNDER IT (user 2026-08-19: "have
        // the tool glide from the middle to the right hand smoothly. theres some jitter") ──
        //   * the hover BOB was folded into cy5v before the lerp, so the thing being interpolated FROM moved
        //     every frame. Lerping toward a fixed target from a source that is itself oscillating is a wobble
        //     that shrinks rather than a glide. It is separated out and damped by (1 - fe), so the hover fades
        //     as the flight takes over and is exactly zero on arrival.
        //   * the SPIN kept turning at a constant rate all the way in, so the tool was still rotating when it
        //     reached the hand and then SNAPPED to the held pose. It is slerped into the right hand's own axes
        //     instead — the same qslerp the right hand's pickup uses for the same reason, and the same reason
        //     that block gives: three axes lerped independently collapse mid-flight and read as a flip.
        // ── AND THE TARGET IS THE POSE THE HAND WILL ACTUALLY BE IN, WHICH IS NOT THE HAND'S CURRENT ONE ──
        // (user 2026-08-20: "the transition … is very buggy"). Three separate things were wrong, and all three
        // are about aiming at the wrong pose rather than about the interpolation:
        //   * ROTATION. It slerped into AX/AY/AZ, the RIGHT HAND'S LIVE AXES. Those are built from
        //     `heldCfg(heldIt())` — and while the bench is open the hand is still holding the STICK or the
        //     ROCK that opened it, so the preview turned itself to match a twig and then snapped to the axe's
        //     pose the frame it landed. The target is the CRAFTED TOOL'S own pose, c3, which is what the hand
        //     will be showing one frame later — built here from the same three lines the hand builds its axes
        //     with (see the AX/AY/AZ triple far above), so the two agree by construction.
        //   * POSITION. It flew to c3.x/y/z, the bare rest anchor, while the hand draws every item at that
        //     anchor PLUS the walk/breathe bob. Landing a bob-width off and then popping is exactly the jitter
        //     the user is describing when they are moving. heldBob is that offset, computed once per frame for
        //     precisely this kind of re-use, so the flight ends where the tool is about to be drawn.
        //   * `crK` is NOT read here on purpose, even though the hand's own anchor is driven to screen centre
        //     by it while the bench is open. That drive dies with the bench on the very frame this lands, so
        //     the hand the tool is joining is the RESTING hand, not the one still reaching into the gesture.
        const cyT = Math.cos(c3.yaw), syT = Math.sin(c3.yaw), cpT = Math.cos(c3.pitch), spT = Math.sin(c3.pitch), crT = Math.cos(c3.roll), srT = Math.sin(c3.roll);
        const TAX = [crT * cyT, srT * cpT + crT * syT * spT, srT * spT - crT * syT * cpT];
        const TAY = [-srT * cyT, crT * cpT - srT * syT * spT, crT * spT + srT * syT * cpT];
        const TAZ = [syT, -cyT * spT, cyT * cpT];
        const tx5 = c3.x + heldBob[0], ty5 = c3.y + heldBob[1], tz5 = c3.z;   // where the hand will draw it next frame — anchor + the bob it rides
        const bob5 = Math.sin(now * 0.0021) * 0.010;
        let cx5 = 0, cy5v = -0.055, cz5 = 1.35 + 0.25 * ease, cs5 = c3.scale * 0.85, fe = 0;
        // ── THE REFUSAL LANE IS READ BEFORE THE FLIGHT, NOT AFTER IT (user 2026-08-20: "when the new item is
        // crafted, it blinks red briefly") ── craftLand SPENDS the sticks and rocks, and it runs from inside the
        // block below, i.e. part-way through this frame. Asking craftAfford afterwards asks it of a hotbar that
        // has just paid: on the single frame the tool lands, the answer flipped to "cannot afford" and the
        // preview painted itself HURT_RED for that one frame before vanishing into the hand. That is the blink.
        // Latched here, before anything is spent, and `flying` holds it at 0 for the rest of the glide —
        // craftConfirm already refuses an unaffordable Enter, so a preview in flight is by construction one
        // that was paid for and can never legitimately turn red mid-air.
        const flying = !!CRAFT.fly;
        const noPay5 = !flying && !craftAfford(cIt);
        if (CRAFT.fly) {
          const fk = Math.min(1, (now - CRAFT.fly) / CRAFT_FLY);
          fe = fk * fk * (3 - 2 * fk);              // smoothstepped: it leaves gently and arrives gently
          cx5 += (tx5 - cx5) * fe; cy5v += (ty5 - cy5v) * fe; cz5 += (tz5 - cz5) * fe;
          cs5 += (c3.scale - cs5) * fe;
          const M5 = q2m(qslerp(m2q(CAX, CAY, CAZ), m2q(TAX, TAY, TAZ), fe));   // …into the pose the TOOL is held at, so it arrives already held rather than mid-turn
          CAX = M5[0]; CAY = M5[1]; CAZ = M5[2];
          if (fk >= 1) craftLand();                  // arrived — spend the halves, hand the tool over, close the bench
        }
        cy5v += bob5 * (1 - fe);                     // the hover, damped out by the flight
        set3(UF_PICK3, [cx5, cy5v, cz5], cs5);   // …and smaller than in the hand while it hovers: it is being shown to you, not swung. The glide above grows it back to the hand's own scale on the way in
        set3(UF_PICK3 + 4, CAX, cIt);
        // ── AND IT GOES RED WHEN YOU CANNOT PAY FOR IT (user 2026-08-20) ── pick3Y.w is the refusal lane: 1 when
        // the hotbar is short of the sticks or rocks THIS tool costs, 0 when it can be made. The composite paints
        // the whole preview in HURT_RED from it, which is the same wound red an animal flashes when it is hit, so
        // "cannot afford" reads at a glance and reads as the colour this game already means "no" with.
        // Recomputed per frame rather than latched at cycle time: a stack can be thrown or eaten while the bench
        // hangs open, and the answer has to follow the hotbar, not the moment the player scrolled onto the tool.
        // …but NOT during the flight — see the noPay5 latch above the glide.
        set3(UF_PICK3 + 8, CAY, noPay5 ? 1 : 0);
        set3(UF_PICK3 + 12, CAZ, 0);
      }
    }
    { // -- THE HEALTH ROW -> u.heart / u.heartC -- five real voxels carried in front of the eye, drawn by the
      // held item's own DDA and lit by the held item's own light (see heldLight in PRE and the heart block in
      // COMPOSITE). The REST POSE is constant, so this publishes three live things: where the hand happens to
      // be in its bob, how much health is left, and how recently you were hit. Written every frame anyway
      // rather than on change, because the lane is otherwise untouched and a stale item id after an asset
      // reload would leave the bar drawn from nothing. HIDDEN in the asset editor (the stage is not the world
      // and ED.on freezes the sim) and while DEAD, the two cases the held item hides for as well.
      // ── ONE ANCHOR FOR THE WHOLE ROW ── the bob is added HERE, to the anchor the shader offsets each heart
      // from, never to the hearts individually: the row moves as one rigid object, which is the rule every
      // view-model animation in this engine follows.
      const hpH = VIT.hp / (VIT_HP_MAX / HEART_N);   // 20 hp over 5 hearts = 4 hp each, so this is "hearts remaining" with the partial one as a fraction
      UF[UF_HEART] = HEART_POSE.x0 + heldBob[0] * HEART_POSE.rig; UF[UF_HEART + 1] = HEART_POSE.y + heldBob[1] * HEART_POSE.rig; UF[UF_HEART + 2] = HEART_POSE.z; UF[UF_HEART + 3] = HEART_POSE.vs;
      UF[UF_HEART + 4] = (HEART_IT && heartShow && !ED.on && !dead) ? HEART_IT : 0;   // 0 = the whole shader block is one compare and draws nothing
      UF[UF_HEART + 5] = hpH; UF[UF_HEART + 6] = HEART_POSE.gap;
      UF[UF_HEART + 7] = Math.round(VIT.hurtT * 13) / 13; }   // the hit kick, STEPPED: VIT.hurtT falls 1 -> 0 over 0.55 s, and 13 steps across it is 24 fps - the rate every animation in this game runs at
    // -- THE HURT FLASH -> u.hurtV -- the red vignette BLIT paints over the finished image. Its own clock, not
    // VIT.hurtT: the kick above runs 0.55 s and the flash has always been the CSS keyframe's 0.42 s, and driving
    // both off one lane would have silently retimed one of them. Written every frame; hurtVig() is 0 for all but
    // the ~10 frames after a hit, and 0 is the one compare that skips the whole block in the shader.
    UF[UF_HURTV] = hurtVig(); UF[UF_HURTV + 1] = HURTV.seed;
    UF[UF_HURTV + 2] = vitRedLevel();                 // z: the standing heart level BLIT tints from — .x is the per-hit spike on top of it, and the two are independent on purpose (one is an event, one is a state)
    UF[UF_VITG] = vitGoldLevel();                     // …and the HUNGER bar's own level, on its own lane — see UF_VITG in render/buffers.js. Written every frame like the red one, so BLIT never reads the zero a cold buffer would hand it
    UF[64] = ED.on ? Math.max(64, renderDist)            // editor world: nothing stale exists (occupancy is empty beyond the stage) — no rect clamp
      : Math.max(64, Math.min(renderDist,                // the view never reaches past the GENERATED rect — outrunning gen shrinks it smoothly
      P.x - rect.xlo - 12, rect.xhi - P.x - 12, P.z - rect.zlo - 12, rect.zhi - P.z - 12));
    UF[65] = ((moonDay % 8) + tday) / 8; UF[66] = windAX; UF[67] = windAZ;   // moon phase in u.rdist.y; integrated WIND displacement in u.rdist.z/.w
    UF[1269] = snowLeadY; UF[1270] = snowTrailY;   // u.misc.y/z — the storm edges computed in the weather block above
    // u.misc.w — is the EYE buried in solid rock? Inside a voxel every primary ray hits at t~0 with no light reaching
    // it, so the screen went pitch black and only the off-grid creatures (which skip the world trace) stayed visible.
    { // u.misc.w — WHICH voxel is the eye inside? Packed sRGB (+1 so 0 means 'not buried'), so the fill takes the
      // material's own colour: stone underground, needle-green inside a pine crown, trunk brown inside a log.
      // Read W directly rather than solid(): FOLIAGE is walk-through, so the collision test never fires in a canopy —
      // which is exactly the case being fixed here.
      let vid = 0;
      const ex = Math.floor(P.x), ey = Math.floor(smoothEye), ez = Math.floor(P.z);
      if (!ED.on && ey >= 0 && ey < WY) vid = W[gwrap(ex, WX) + ey * WX + gwrap(ez, WZ) * WX * WY];
      const opaque = vid && !foliaTab[vid] && vid !== WATER_T && vid !== WATER_B && vid !== LAVA_T && vid !== LAVA_B && vid !== LAVA_R && vid !== LAVA_Y;   // water has its own underwater tint; lava is emissive; FOLIAGE is now SEE-THROUGH (the primary ray skips near leaves — user), so no chunky green fill when you clip into a crown
      const pc = opaque ? palette[vid] : null;
      UF[1271] = pc ? (1 + pc[0] + pc[1] * 256 + pc[2] * 65536) : 0;
      eyeFolV = 0;                                       // ── TRACE variant pick ── the see-through pipeline only when foliage is within 1 voxel of the eye (a strict SUPERSET of the shader's own eye-in-leaf test, so behaviour is exactly the old rule); everywhere else the fast variant runs with the check compiled out of the hot DDA loop
      if (!ED.on) for (let ny = Math.max(1, ey - 1); ny <= Math.min(WY - 2, ey + 1) && !eyeFolV; ny++) for (let nx = ex - 1; nx <= ex + 1 && !eyeFolV; nx++) for (let nz = ez - 1; nz <= ez + 1; nz++)
        if (foliaTab[W[gwrap(nx, WX) + ny * WX + gwrap(nz, WZ) * WX * WY]]) { eyeFolV = 1; break; } }
