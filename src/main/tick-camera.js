    // ── day/night cycle ── 20-minute full cycle at 1x; tday 0 = midnight, 0.5 = noon; ALT+scroll scales speed, L pauses
    { const ntd = window.__TFREEZE ? tday : (tday + dt * cycleSpeed / 1200) % 1;   // __TFREEZE pins the sun for perf A/Bs (the cycle moved measured trace cost 45% over one 7-minute run). Not __vb.tod(): that also sets resetHist, so pinning with it would measure a permanently-cold denoiser.
      if (ntd < tday) moonDay++;                       // a day rolled over → the moon advances one phase step (8-day cycle)
      tday = ntd; }
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
    cmpUpdate();                                       // ── compass ── one transform write per frame
    { const hs = slots[selSlot];                       // ── HELD-STACK COUNT ── x2..x8 top right, hidden at one (user)
      const nHold = hs && !dead ? (hs.n | 0) : 0;
      if (nHold !== stackShown) { stackShown = nHold;
        // ── STACKBADGE ── stackShown is the latch; the uniform write itself lives in the per-frame heldCfg line, which would otherwise zero it.
        stackEl.classList.add('hidden'); } }   // the old top-right HTML badge is retired — the count is drawn in the image now (user)
    crossEl.classList.toggle('sq', locked && !ED.on && pickAim());   // crosshair morphs + → □ ONLY over something RIGHT-CLICK can pick up (user 2026-08-02 — it used to include aimedCreature(), so it also lit up on anything killable, which is a LEFT-click action and made the square mean two different things)
    const cam = [P.x, smoothEye, P.z];                 // no bob — dead-steady camera
    if (!locked && !lightMode) {                       // ESC menu: the whole CAMERA drifts gently — the entire image sways.
                                                       // NOT in light mode: comparing two lighting terms needs the camera to hold perfectly still.
      const ts = now * 0.001;
      cam[0] += Math.sin(ts * 0.42) * 0.9;
      cam[1] += Math.sin(ts * 0.61 + 1.7) * 0.55;
      cam[2] += Math.cos(ts * 0.37) * 0.9;
    }
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
      if (uwT >= DROWN_T) die('you drowned');
    } else if (!uw || P.fly) { uwT = 0; }              // surfaced or flew out → catch your breath (menu/editor/cinematic just FREEZE the clock — they don't refill it)
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
      UF[1860] = heldSunV; UF[1861] = heldSkyV; UF[UF_HELDCFG + 2] = Math.max(0, stackShown); UF[1863] = 0;   // ── STACKBADGE ── z is the held stack count the BLIT draws beside the hand. It MUST be written here: this line runs every frame and used to zero z, which silently clobbered a write made earlier in the frame. “Spare” meant actively zeroed, not unused.
      UF[1864] = lgtMask; UF[1865] = wReflK; UF[1866] = lgtMask2; UF[1867] = 0;   // ── WATER PANEL ── x = term mask (LG), y = the REFLECTION STRENGTH slider, z = the SECOND term mask (LG2 — lgt.x is full at 24 bits)
      // ── HIT FLASH ── published AFTER the creature stamps (see the block by UF[1875] further down), because a
      // land mammal re-stamps itself later in this same tick — bounce included — and a box measured here would
      // describe where it WAS last frame while the GPU shades where it IS now.
    }
    set3(32, sun, resetHist);
    UF[36] = RW; UF[37] = RH; UF[38] = j[0]; UF[39] = j[1];
    UF[40] = prevCam.jit[0]; UF[41] = prevCam.jit[1]; UF[42] = CW; UF[43] = CH;
    UF[44] = winOX; UF[45] = winOZ; UF[46] = gwrap(winOX, WX); UF[47] = gwrap(winOZ, WZ);
    { if (mouse0 && locked && !dead && now - swingStart >= 570) { swingStart = now; pendKillT = swingStart + 250; }   // hold left click → continuous swinging (each auto-repeat re-arms the impact-timed hit)
      reapDeaths(now);                                 // creatures whose red flash has run out now actually die (see tryKillCreature)
      shTick(now);                                     // …and the scroll-up hint appears ten seconds into play, and rides the esc menu (see shTick)
      if (shHideT && now >= shHideT) { shHideT = 0; if (!shVis) shEl.classList.add('hidden'); }   // its fade-out has finished — stop painting it
      tillRevert(now);                                 // …and untended tilled soil closes back over
      if (pendKillT && now >= pendKillT) { pendKillT = 0;
        const am9 = aimedCreature();                     // ANY tool: whatever the crosshair is genuinely on takes the swing (user)
        if (am9 >= 0) hitCreature(am9);                  // …otherwise chopSwing carves a grid-stamped animal's voxels out of the world and it comes apart in pieces instead of dying
        else if (chopSwing()) playToolHit(); else { tryKillCreature(); if (aimHitId()) playBlocked(); } }   // …and if it bit NOTHING but the crosshair was on something solid, it BOUNCED — the thud (user 2026-08-07). Nothing under the crosshair at all stays silent: a whiff is not a bounce   // it BIT something — one of the four break takes (user)   // an axe that bites a trunk spends the swing on the tree; otherwise it can still kill   // the axe LANDS on screen ~250 ms into the chop → NOW the hit registers (1 hit = 1 kill; the death poof — 4 sparks + 4 smoke columns together — fires from the creature)
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
      { const hNow = heldIt() || 0; if (hNow !== prevHeldIt) { prevHeldIt = hNow; swapT0 = now; } }   // ── TOOL SWAP ── keyed off the ITEM, so a pickup or a craft landing in the hand animates too
      const swapR = Math.max(0, 1 - (now - swapT0) / SWAP_MS);
      const swapF = swapR * swapR * (3 - 2 * swapR);   // 1 → 0 across the swap, SMOOTHSTEPPED and not quantised: this is a camera move, not a character animation, and stepping it at 24 fps read as a stutter (user)
      const hcfg = heldCfg(heldIt() || 1);             // the shown item's OWN pose (during a grab flight this is the grabbed item's)
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
      const spd2 = Math.hypot(P.hvx, P.hvz);           // view-model BOB — phase tied to distance walked (smooth at any speed), amplitude eased in/out
      bobAmp += ((P.onGround && !P.fly ? Math.min(1, spd2 / WALK) : 0) - bobAmp) * (1 - Math.exp(-8 * dt));
      bobPh += spd2 * dt * 0.225;                      // slower phase + wide sweep = long smooth strides
      hx += Math.sin(bobPh) * 0.075 * bobAmp + Math.sin(now * 0.0013) * 0.0069 + Math.sin(now * 0.00073 + 1.7) * 0.0039;       // + idle breathing sway (tripled)
      // …+ the draw tremble. SLOW and CONTINUOUS (user: smoother, not violent): a held arm drifts, it does
      // not buzz — so this is two long rates at a third of the old amplitude, and NOT stepped to 24 fps.
      // Quantising it made a 12 Hz judder, which is exactly what read as violent; the view-model bob above
      // is left continuous for the same reason.
      if (bowShk > 0) { hx += (Math.sin(now * 0.0062) + 0.5 * Math.sin(now * 0.0111 + 2.1)) * 0.0150 * bowShk;   // …travel DOUBLED (user) — same slow rates, twice the distance
                        hy += (Math.sin(now * 0.0049 + 1.1) + 0.5 * Math.sin(now * 0.0093) ) * 0.0120 * bowShk; }
      hy += -Math.abs(Math.cos(bobPh)) * 0.028 * bobAmp + Math.sin(now * 0.0017 + 0.9) * 0.0069 + Math.sin(now * 0.00091) * 0.0036;
      let AX = [cr2 * cy, sr2 * cp2 + cr2 * sy * sp2, sr2 * sp2 - cr2 * sy * cp2];
      let AY = [-sr2 * cy, cr2 * cp2 - sr2 * sy * sp2, cr2 * sp2 + sr2 * sy * cp2];
      let AZ = [sy, -cy * sp2, cy * cp2];
      let showId = dead ? 0 : (slots[selSlot] ? slots[selSlot].it : 0);          // held item = the SELECTED hand slot (0 = empty hand)
      if (clashK > 0 && (showId === 2 || showId === KNIFE_IT)) { hx += (0.13 - hx) * clashK; hy += (-0.17 - hy) * clashK; hz += (0.92 - hz) * clashK; }   // right rock drives to centre; the newborn knife eases back from there
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
          if (as >= 0 && wasEmpty) selSlot = as;
          grabAnim = null; grabGhost = null; }
      } else grabGhost = null;
      set3(48, [hx, hy, hz], hcfg.scale);
      heldOff = [hx, hy, hz];                          // published for shootArrow: the bow's own place in the frame
      if (BOW_IT && showId === BOW_IT) {            // BOW DRAW (user): the strip swaps under the one held pose…
        const bf = bowFrame(now);                   // …and once the string is loosed the bare strip takes over, so the arrow is GONE from the bow (user)
        if (bowLoosed && bowAtRest(now)) { bowLoosed = false; playBowReload(); }   // back at rest: the bow is nocked again, ready for the next draw — and you hear the next arrow go on (user)
        showId = ((BOW_NOCK && bowLoosed && bf > 0) ? BOW_NOCK : BOW_IT) + bf;
      }
      shownIt = showId;
      if (WORM_NFRAMES && showId >= WORM_ITEM0 && showId < WORM_ITEM0 + WORM_NFRAMES) showId = WORM_ITEM0 + Math.floor(now * 0.024) % WORM_NFRAMES;   // the caught worm SQUIGGLES — its 24 fps crawl cycle keeps playing in the hand (and through the grab flight)
      set3(52, AX, showId);
      set3(56, AY, snowFallAcc);                     // u.pickY.w = integrated snow fall
      set3(60, AZ, freezeK);                        // u.pickZ.w = gradual freeze 0..1
      { // ── LEFT HAND ── shows the second rock when dual-wielding (selected rock stack n ≥ 2); also hosts a 2nd-rock grab flight
        //   (The arrow used to be drawn resting here; the user took it back off the bow. It is still
        //   launched as a projectile on release — see shootArrow.)
        const lgrab = grabAnim && grabAnim.left && !dead;
        {
        const lid = lgrab ? grabAnim.it : ((!dead && slots[selSlot] && slots[selSlot].it === 2 && slots[selSlot].n >= 2) ? 2 : 0);
        if (!lid) UF[1095] = 0;                      // pick2A.w = 0 → zero bounding radius hides the left hand (pick2 sits after the 64 drop slots: 1092..1107)
        else {
          const c2 = heldCfg(2);                     // mirrored rock pose: anchor x negated, yaw/roll negated (still a proper rotation — no handedness flip)
          const cy4 = Math.cos(-c2.yaw), sy4 = Math.sin(-c2.yaw), cp4 = Math.cos(c2.pitch), sp4 = Math.sin(c2.pitch), cr4 = Math.cos(-c2.roll), sr4 = Math.sin(-c2.roll);
          let LAX = [cr4 * cy4, sr4 * cp4 + cr4 * sy4 * sp4, sr4 * sp4 - cr4 * sy4 * cp4];
          let LAY = [-sr4 * cy4, cr4 * cp4 - sr4 * sy4 * sp4, cr4 * sp4 + sr4 * sy4 * cp4];
          let LAZ = [sy4, -cy4 * sp4, cy4 * cp4];
          let lx = -c2.x - Math.sin(bobPh) * 0.075 * bobAmp - Math.sin(now * 0.0013 + 0.6) * 0.0069 - Math.sin(now * 0.00073 + 2.3) * 0.0039,
              ly = c2.y - Math.abs(Math.cos(bobPh)) * 0.028 * bobAmp + Math.sin(now * 0.0017 + 2.1) * 0.0069 + Math.sin(now * 0.00091 + 1.1) * 0.0036,
              lz = c2.z;
          if (clashK > 0) { lx += (-0.13 - lx) * clashK; ly += (-0.17 - ly) * clashK; lz += (0.92 - lz) * clashK; }   // left rock mirrors the drive to centre
          if (lgrab) {                               // fly from the world spot into the LEFT rest pose — same glide as the right hand
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
        } }
      }
    }
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
