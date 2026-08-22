  // ── DEPTH OF FIELD ── persisted (vb_dof), default ON, and declared HERE for the same reason vigOn is: the
  // settings-panel wiring further down reads it, so a declaration below that point is a TDZ error at boot.
  const DOF_COC = 0.0105;                              // max circle of confusion as a fraction of the canvas HEIGHT (~11 px at 1080p) — a fraction, not a pixel count, so the look is identical at every resolution and render scale
  const DOF_RACK = 0.13;                               // autofocus time constant, seconds. A lens racks, it does not cut: focus that snapped the instant the view moved read as the picture breathing
  let dofInv = 1 / 24;                                 // the eased focal distance, carried as 1/d — see the autofocus block in tickBody
  // ── STRENGTH (settings slider, persisted vb_dofstr) ── carried as a MULTIPLE of DOF_COC rather than as a raw
  // radius, so 100% is always the tuned default whatever the number behind it becomes, and the slider stays
  // meaningful if DOF_COC is ever re-tuned. 0% is the same picture as the toggle being off; 200% is roughly a
  // 22 px circle at 1080p, which is as shallow as this scene takes before the far field stops reading as depth.
  let dofStr = 0.3; try { const v9 = parseFloat(localStorage.getItem('vb_dofstr2')); if (v9 >= 0 && v9 <= 2) dofStr = Math.min(v9, 1); } catch (e) {}   // …CLAMPED TO 1 (user 2026-08-09: the slider stops at 100%): the range used to run to 200%, so a saved 1.5 would otherwise leave the knob parked at the end of a bar that no longer means what the value does   // 30% of DOF_COC (user 2026-08-08) — ~3 px at 1080p: enough that distance reads as distance, well short of the shallow look the top of the slider gives
  const DOF_TAPS = 1.6;                                // gather taps per PIXEL of blur radius — see the loop in BLIT. 1.6 x the 21 px widest circle = the 32 taps the effect was tuned at, so the top of the strength slider is unchanged and everything below it costs what its own area is worth.
  let dofTapK = DOF_TAPS;                              // …pinnable with __vb.dof({taps: k}) to A/B the sampling against the old flat 32 (k = 40 forces 32 everywhere)
  let dofLock = 0, dofCocK = DOF_COC * dofStr;         // __vb.dof() overrides: pin the focal plane / the aperture to A/B the effect at a fixed strength (0 = autofocus)
  let vigOn = true; try { vigOn = localStorage.getItem('vb_vig') !== '0'; } catch (e) {}
  let snowOn = false;                                  // ── VOXEL SNOW ── starts CLEAR, then the first storm arrives 60 s after refresh (user 2026-08-06). Weather-driven; the button (and P) still forces one on/off by hand.
  let snowEndT = 0, snowNextT = 120000;   // ── FIRST STORM 120 s AFTER REFRESH (user 2026-08-06, raised 60 -> 120 on 2026-08-17) ── snowNextT is the ARRIVAL of the first storm, snowEndT the end of the CURRENT one. Only the arrival moved: an event still RUNS for 60 s and still re-arms on the normal 5-minute cadence, both of which live on the toggle path in ui/input.js and below. Note snowOn:true alone would NOT work: that tick ends a storm the instant now > snowEndT, so an unscheduled 'on' is switched straight back off on frame one. Note snowOn:true alone would NOT work: that tick ends a storm the instant now > snowEndT, so an unscheduled 'on' is switched straight back off on frame one.
  // ── AND IT DOES NOT SNOW AT NIGHT (user 2026-08-19: "dont make it snow at night") ── the gate is on the
  // storm's ARRIVAL only, in main/tick-body.js. sunUp() mirrors main/tick-camera.js's own sun elevation
  // (ang = tday*2pi - pi/2, el = sin(ang)*1.05, elevation = sin(el)) and uses the SAME -0.06 threshold that
  // file uses to hand the world over to the moon, so "not night" here means exactly what the player sees as
  // night rather than a second, nearby definition that can drift from it. tday lives in ui/input.js, which the
  // manifest places after this file but before every tick fragment, so this is only ever CALLED once it exists.
  // A storm already running when the sun sets is left alone to finish its 60 s: the lead/trail sweep in
  // main/tick-snow.js is derived from snowOnT0/snowOffT0, and cutting it mid-flight strands that edge.
  const SNOW_DAY_ONLY = true;
  const sunUp = () => Math.sin(Math.sin(tday * Math.PI * 2 - Math.PI / 2) * 1.05) >= -0.06;
  const SNOW_NIGHT_RETRY = 15000;                      // due but dark: re-ask in 15 s rather than computing the next sunrise — the cycle is 20 min at 1x but ALT+scroll rescales it, so a computed wake-up would be wrong the moment the speed changed
  // TO RESTORE the weather cycle, put this back to `performance.now() + 120000` (first storm 2 min after refresh, then every 5 min).
  // The snow button / P key still forces a storm on by hand, and that path re-arms snowNextT normally.
  // ── RAIN SKY (user 2026-08-17: "when it rains can you make the sky more cloudy and darken the clouds as well.
  // then when the rain is gone, the clouds return to normal. also when it rains the sun should dim a bit") ──
  // rainSkyK is HOW FAR THE OVERCAST HAS COME IN, 0..1. It is ramped in tick-camera and published to the GPU as
  // u.hurtV.w (see UF_RAINK) multiplied by oakM AT THE CAMERA, and that multiply is the whole of "it is RAIN,
  // not the storm": the same event drops SNOW on the pine forest and the desert, so those two skies have to come
  // out of all of this unchanged. Keeping the storm ramp and the biome weight as separate factors is also what
  // makes walking the border work — the ramp is a clock, oakM is a position, and multiplying them greys the sky
  // across the 450-voxel blend band at exactly the rate the player crosses it, with no lag to unwind and nothing
  // to re-ramp. Easing the PRODUCT instead would have put a 20-second tail on a teleport, which is the one case
  // where an instant answer is the correct one: you did not walk into that weather, you arrived in it.
  let rainSkyK = 0;
  // SECONDS to cloud over, and to clear again. Linear rather than exponential, so each has an end you can state
  // rather than an asymptote, and ASYMMETRIC on purpose. 10 s in is a little over twice the ~4.9 s the existing
  // storm edge takes to sweep from SNOW_HEAD (220 voxels up) down to the player at SNOW_SWEEP 45 vox/s, so the
  // sky is already visibly grey by the time the first drops reach eye level and fully overcast a few seconds
  // after — the sky leads the rain, which is the order it happens in. 20 s out is the half the user actually
  // asked about: the trailing edge sweeps the last drops away in ~5 s and the deck then takes another quarter of
  // a minute to break up, so "the rain stopped" and "the sky cleared" are two beats you watch happen instead of
  // one switch. Both finish far inside the 300 s gap between storms, so the sky is back to its fair-weather self
  // — bit-identical, not merely close: see the rainK note in COMPOSITE — long before the next storm arrives.
  const RAIN_SKY_IN = 10, RAIN_SKY_OUT = 20;
  const SNOW_AUTO_OFF = false;   // storms RECUR again (user 2026-08-06): each event 60 s, then every 5 min. true made the first storm the last one — it armed snowNextT to Infinity on the way out.
  let heldSunV = 1;                                    // eased sun visibility at the player — gates the held item's DIRECT term so a tool in shade reads like the world around it (u.heldCfg.x)
  let heldSkyV = 1;                                    // …and eased SKY visibility — gates its AMBIENT + ground bounce, the term the world gets from irr.g (u.heldCfg.y). Without it a tool kept full open-sky ambient under a canopy or in a cave while the world around it went dark.
  const HELD_SKY_DIRS = [[0, 1, 0], [0.707, 0.707, 0], [-0.707, 0.707, 0], [0, 0.707, 0.707], [0, 0.707, -0.707]];   // straight up + four at 45°; each ray's y component IS its cosine weight, so the average is cosine-weighted like the world's hemisphere ray
  const HELD_SKY_R = 24;                               // …and the SAME 24-voxel range the world normalises its AO ray against (clamp(ah.t / 24)), so the two terms mean the same thing
  let freezeK = 0, iceSolid = false;                   // lakes freeze/thaw GRADUALLY — freezeK eases 0..1 (u.pickZ.w), physics flips at 0.6
  let snowBtnSync = () => {};
  let vigToggle = () => {};                            // set by the settings-panel wiring below; the L keybind calls the SAME path so the button label stays in sync
  // FIFO as parallel TYPED arrays (cell idx / melt time) — object entries at 1.6M live voxels were ~10× the memory.
  // These were plain Arrays compacted with .slice(head); at ~450k live voxels each slice copied the whole queue and
  // the pushes re-grew it, which the heap profiler put at ~99% of ALL snowfall garbage. Now capacity doubles on
  // demand (bounded by SNOW_MAX) and compaction is an in-place copyWithin — zero allocation in steady state.
  let snowQI = new Int32Array(1 << 16), snowQN = 0, snowHead = 0;   // GROUND snow, in landing order. No per-voxel expiry any more (user 2026-08-09): the whole queue drains at one steady rate once the thaw starts — see snowGMelting.
  let snowWI = new Int32Array(1 << 14), snowWN = 0, snowWHead = 0;   // snow that landed on WATER/ICE — drained in one continuous sweep after the storm (snowWMeltAt), no per-voxel timers
  const snowCompactQ = () => { if (snowHead <= 0) return; snowQI.copyWithin(0, snowHead, snowQN); snowQN -= snowHead; snowHead = 0; };
  const snowCompactW = () => { if (snowWHead <= 0) return; snowWI.copyWithin(0, snowWHead, snowWN); snowWN -= snowWHead; snowWHead = 0; };
  const snowRoomQ = () => {                            // ensure one free slot; compact first, grow only if that is not enough
    if (snowQN < snowQI.length) return true;
    snowCompactQ();
    if (snowQN < snowQI.length) return true;
    const cap = Math.min(SNOW_MAX + 4096, snowQI.length * 2);
    if (cap <= snowQI.length) return false;
    const a = new Int32Array(cap); a.set(snowQI.subarray(0, snowQN)); snowQI = a;
    return true;
  };
  const snowRoomW = () => {
    if (snowWN < snowWI.length) return true;
    snowCompactW();
    if (snowWN < snowWI.length) return true;
    const cap = Math.min(SNOW_MAX + 4096, snowWI.length * 2);
    if (cap <= snowWI.length) return false;
    const a = new Int32Array(cap); a.set(snowWI.subarray(0, snowWN)); snowWI = a;
    return true;
  };
  const SNOW_WATER_GRACE = 6000;                       // ms after a storm ends before the frozen lake starts shedding its snow
  const SNOW_WATER_RATE = 9000;                        // voxels/s the water blanket drains at once it starts — rapid + steady, ~8 s for a full lake (user)
  let snowWMelting = false, snowWMeltAt = Infinity;    // latched into ONE continuous drain once SNOW_WATER_GRACE has passed since the storm (user: no per-voxel-timed bursts)
  let snowGMelting = false, snowGMeltAt = Infinity;    // …and the GROUND blanket does exactly the same, on its own longer clock (user 2026-08-09: "melt at a consistent rate until it is completely gone")
  let snowFreezeAt = Infinity;                         // ── FREEZE DELAY ── water may not start freezing until this moment. Gating on "a flake has landed"
  const SNOW_FREEZE_DELAY = 10000;                     // looked instant (1.6 s) because the far statistical SPRINKLE ring lands the moment the storm flag flips,
                                                       // while the flakes you can actually SEE are still falling from the cloud deck. A flat 10 s wall-clock delay
                                                       // after the storm starts is what the user asked for and matches what the eye sees hit the ground.
  let snowOffT0 = -1e9;                                // when the last storm ENDED — drives the trailing edge sweep
  let snowLeadY = -1e9, snowTrailY = 1e9;              // the live storm edges (world y), computed once per frame and used by BOTH the landing pass and the shader
  const SNOW_SWEEP = 45, SNOW_HEAD = 220;              // edge sweep speed (vox/s) and how far above the player an edge starts, i.e. just off the top of the screen
  let snowVis = false;                                 // flakes are DRAWN while the storm runs AND while its tail is still falling
  let snowPrevOn = false, snowOnT0 = 0;                // storm edge tracking — melt timers are FROZEN while it snows and rebased by the storm's length when it ends
  let snowSprinkAcc = 0;                               // fractional carry for the uniform-area sprinkle — low fps or thin rings never quantize to zero
  let windAX = 0, windAZ = 0;                          // ── WIND ── integrated wind displacement (vox): direction wanders over minutes, strength GUSTS over seconds; snow flakes advect along it
  let snowFallAcc = 0, snowFallV = 11;                 // integrated flake fall: 11 vox/s while the player moves, eases to 22 when standing still (u.pickY.w)
  const ih3s = (x, y, z) => {                          // bit-exact JS twin of the shader's ih3 — the landing code must see the SAME flakes the GPU draws
    let h = (Math.imul(x, 374761393) + Math.imul(y, -1028477379) + Math.imul(z, 668265263)) | 0;
    h = Math.imul(h ^ (h >>> 13), 1274126177);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  };
  const frac9 = (v) => v - Math.floor(v);
  const SNOW_MAX = 1600000, SNOW_MELT = 95000;         // cap on live snow voxels — at the cap NEW landings are DROPPED (the blanket must never be deleted mid-storm; the old force-melt-oldest WAS the 'snow deletes while snowing' bug); ms the blanket LIES before the thaw begins
  const SNOW_GROUND_RATE = 24000;                      // voxels/s the ground blanket drains at once it starts — the same ceiling the old per-voxel drain used, but now it is the ACTUAL rate rather than a cap the timers rarely reached
  let SNOW_ROLL_MAX = 1;                               // how far the settle-roll may DROP a flake (0 = unlimited). At a crown's edge the lowest neighbour is the forest floor, so an unlimited roll tips canopy snow off the tree — measured 18% canopy coverage against 44% ground
  let SNOW_SHELF = 0;                                  // how many of a crown voxel's 4 sides must also carry crown before snow may settle. DEFAULT 0 = the v0.9 gate. A/B measured 2026-08-07: shelf 2 halved canopy coverage (38.7%% -> 17.0%%) and cut snow-with-nothing-under-it only from 2 to 1 in a 61x61 sample, i.e. it costs most of the crown snow and buys nothing. Kept as a knob, off.
  let SNOW_ON_CROWN = 1;                               // runtime A/B switch for canopy snow — the only honest way to price it is in ONE session
  const SNOW_PASS = new Set([...GRASS, ...FLOWERIDS]);   // the WHOLE flower, stem included — a flake should fall through the plant, not perch on its stalk (was BLOOM's six heads; user 2026-08-18)   // landings fall THROUGH grass/blooms and bury them — ferns are OUT: snow settles ON their fronds
  const SNOW_FERN = new Set(FERNIDS);                  // fern-topped columns skip the settle-roll (fronds sit above the ground beside them, so rolling would always shed the flake)
  const SNOW_SKIP = new Set([WATER_B, LAVA_T, LAVA_B, LAVA_R, LAVA_Y]);                                    // …but never land on lava; surface water is FROZEN while it snows, so flakes settle on the ice
  // ── THE HUD TOGGLES ARE REMEMBERED TOO (user 2026-08-21: "have the browser remember my settings on refresh") ──
  // coords and time were the two that wrote their key and never read it: mkToggle below persists every button it
  // wires, so the write was already there and only the boot-time read was missing. Both used to start hidden on
  // purpose, under the same rule the four volume sliders followed (see ui/audio.js); that rule is what the user
  // has now reversed, and fps/res were already reading their keys, so this makes all four buttons behave alike.
  let showCoords = false; try { showCoords = localStorage.getItem('vb_coords') === '1'; } catch (e) {}   // COORDS HUD (user) — default OFF, persisted vb_coords
  let showFps = true; try { showFps = localStorage.getItem('vb_fps') !== '0'; } catch (e) {}            // FPS HUD toggle (user) — persisted vb_fps
  let showTime = false; try { showTime = localStorage.getItem('vb_time') === '1'; } catch (e) {}     // TIME (clock) HUD (user) — default OFF, persisted vb_time
  let showRes = false; try { showRes = localStorage.getItem('vb_res') === '1'; } catch (e) {}          // RESOLUTION HUD readout toggle (user): default OFF, persisted vb_res — toggled in settings like fps
  { const snowBtn = $('snowBtn');
    const mkToggle = (id, get, set, key) => {            // wire a green/red on-off HUD toggle button (coords/fps/time)
      const btn = $(id); const show = () => { btn.textContent = get() ? 'on' : 'off'; btn.classList.toggle('on', get()); };
      btn.addEventListener('pointerdown', (e) => e.stopPropagation());
      btn.addEventListener('click', (e) => { e.stopPropagation(); set(!get()); try { localStorage.setItem(key, get() ? '1' : '0'); } catch (e2) {} show(); });
      show(); };
    // ── WATER PANEL ── one row per WATER term, each a bit in u.lgt.x, bit order shared with LG() in the
    // shader. Persisted, so a setting survives the reload you will inevitably do.
    // WATER ONLY (user 2026-08-05). The other 18 terms this panel used to carry — caustics, volumetric,
    // sun shadow, penumbra, AO, sky/ambient, bounce, creature shadow, glow, fog, the three grains, held-item
    // lighting, reactive mask, irr history, spatial blur, TAA — are whole-scene switches, which is why
    // clicking them changed the environment. They are forced ON at load (see LGT_WATER) and are reachable
    // only from the console now: __vb.lgt(mask). Every bit BELOW is confined to the `face == 6u` water
    // branch of the shader and cannot touch a land pixel.
    // [label, bit, mask] — mask 1 = u.lgt.x (LG), mask 2 = u.lgt.z (LG2). The GLISTEN is two separate
    // effects sharing one light column (user 2026-08-05): a smooth added SHEEN and discrete flashing
    // VOXELS. One button could not tell you which of them you were looking at, so they are two.
    const LGT = [['reflect', 18, 1], ['refract', 19, 1], ['foam', 20, 1], ['ice', 21, 1],
                 ['pixel glisten', 22, 1], ['waves', 23, 1]];
    const lgtGet = (b, m) => ((m === 2 ? lgtMask2 : lgtMask) & (1 << b)) !== 0;
    {
      const panel = $('lgtPanel');
      const bid = (b, m) => 'lgtB' + m + '_' + b;
      lgtPaint = () => { for (const [, b, m] of LGT) { const btn = $(bid(b, m));
        const on = lgtGet(b, m); btn.textContent = on ? 'on' : 'off'; btn.classList.toggle('on', on); } };
      const addRows = (list) => { for (const [label, b, m] of list) {
        const row = document.createElement('div'); row.className = 'lgtRow';
        row.innerHTML = '<span></span><button id="' + bid(b, m) + '">on</button>';
        row.firstChild.textContent = label;
        panel.appendChild(row);
        const btn = row.lastChild;
        btn.addEventListener('pointerdown', (e) => e.stopPropagation());
        btn.addEventListener('click', (e) => { e.stopPropagation(); resetHist = 1;   // the filters hold seconds of history; drop it so the change is visible immediately
          if (m === 2) { lgtMask2 ^= (1 << b); }             // no row uses the second mask today; kept so adding one needs no wiring
          else { lgtMask ^= (1 << b); try { localStorage.setItem('vb_lgt', String(lgtMask)); } catch (e2) {} }
          lgtPaint(); });
      } };
      addRows(LGT);
      // ── REFLECTION STRENGTH (user 2026-08-05) ── how much of the Fresnel split goes to the mirror.
      // 1.00 is physical Schlick (the look it has always had); 0 kills the mirror, 2 doubles it. Lives in
      // u.lgt.y, so it costs nothing extra — the reflection ray is traced either way, this only weights it.
      const sld = document.createElement('div'); sld.className = 'lgtRow lgtSld';
      sld.innerHTML = '<span>reflection</span><input id="lgtRefl" type="range" min="0" max="2" step="0.05"><span class="lgtVal" id="lgtReflV"></span>';
      panel.appendChild(sld);
      const reflIn = $('lgtRefl'), reflVal = $('lgtReflV');
      const reflPaint = () => { reflIn.value = String(wReflK); reflVal.textContent = wReflK.toFixed(2); };
      reflIn.addEventListener('pointerdown', (e) => e.stopPropagation());
      reflIn.addEventListener('input', (e) => { e.stopPropagation(); wReflK = +reflIn.value; resetHist = 1;
        try { localStorage.setItem('vb_wrefl', String(wReflK)); } catch (e2) {} reflPaint(); });
      reflPaint();
      // ── BAKE ── the whole water setup as ONE line, ready to paste over the WATER_BAKE declaration in the
      // source (same idea as the arrow panel's bake row). What is written there is what a fresh player gets.
      const addBake = (id, strFn) => {
        const bake = document.createElement('div'); bake.className = 'lgtRow lgtBake';
        bake.innerHTML = '<input type="text" id="' + id + '" readonly><button id="' + id + 'C">copy</button>';
        panel.appendChild(bake);
        const bakeIn = $(id), bakeBtn = $(id + 'C');
        bakeIn.addEventListener('pointerdown', (e) => e.stopPropagation());
        bakeIn.addEventListener('click', (e) => { e.stopPropagation(); bakeIn.select(); });
        bakeBtn.addEventListener('pointerdown', (e) => e.stopPropagation());
        bakeBtn.addEventListener('click', (e) => { e.stopPropagation();
          const done = () => { bakeBtn.textContent = 'copied!'; setTimeout(() => { bakeBtn.textContent = 'copy'; }, 1200); };
          if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(bakeIn.value).then(done, () => { bakeIn.select(); document.execCommand('copy'); done(); });
          else { bakeIn.select(); document.execCommand('copy'); done(); } });
        return () => { bakeIn.value = strFn(); };
      };
      const bakePaint = addBake('lgtBake', () => 'const WATER_BAKE = { ' + [
        'reflect: ' + (lgtGet(18, 1) ? 1 : 0), 'refract: ' + (lgtGet(19, 1) ? 1 : 0),
        'foam: ' + (lgtGet(20, 1) ? 1 : 0), 'ice: ' + (lgtGet(21, 1) ? 1 : 0),
        'pixelGlisten: ' + (lgtGet(22, 1) ? 1 : 0),
        'waves: ' + (lgtGet(23, 1) ? 1 : 0), 'reflection: ' + (+wReflK).toFixed(2),
      ].join(', ') + ' };');
      { const base = lgtPaint; lgtPaint = () => { base(); reflPaint(); bakePaint(); }; }   // one repaint covers buttons, slider AND the bake line, so __vb.wrefl()/__vb.lgt() from the console can never disagree with what is drawn
      const hint = document.createElement('div'); hint.className = 'lgtHint';
      hint.textContent = 'L — cursor on/off';   // the panel says how to reach it; nothing else in the HUD would
      panel.appendChild(hint);
      $('lgtAll').addEventListener('pointerdown', (e) => e.stopPropagation());
      $('lgtAll').addEventListener('click', (e) => { e.stopPropagation();   // reset = back to whatever is BAKED in the source, not to all-on
        lgtMask = wBakeMask(); lgtMask2 = wBakeMask2(); wReflK = wBakeRefl(); resetHist = 1;
        try { localStorage.setItem('vb_lgt', String(lgtMask)); localStorage.setItem('vb_wrefl', String(wReflK)); } catch (e2) {} lgtPaint(); });
      lgtPaint();
    }
    {   // ── THE ADJUST BOX ── quarter turns and voxel nudges for WHATEVER IS IN HAND (user). Holding the
      // bow it does what it always did — moves the arrow inside the strip, per draw frame. Holding anything
      // else it turns and shifts that item's own HELD POSE, which is the only thing a plain tool has.
      const ap = $('arwPanel'), now = $('arwNow');
      let arwDirty = false;
      const onBow = () => !!(BOW_IT && heldIt() === BOW_IT);
      const tgt = () => heldIt() || 1;                   // empty hand tunes the axe, exactly as the slider panel does
      const nudge = {};                                  // per item: where its three sliders currently sit, so they read as absolute
      const arwF = () => (bowLock >= 0 ? bowLock : 0);   // the sliders edit the frame you are LOOKING at; unpinned, that is the bow at rest
      const arwFlush = () => { arwDirty = false; if (bowRefit) bowRefit(ARROW_ROT, ARROW_POS); resetHist = 1; };   // the denoiser holds seconds of history; drop it so the change shows at once
      const arwQueue = () => { if (arwDirty) return; arwDirty = true; requestAnimationFrame(arwFlush); };   // a slider drag fires dozens of events a second — one re-cut per FRAME is plenty
      const poseStr = (c) => JSON.stringify(Object.fromEntries(Object.entries(c).map(([k, v]) => [k, +(+v).toFixed(3)])));
      const arwBakeStr = () => onBow()
        ? 'rot ' + JSON.stringify(ARROW_ROT) + ' pos ' + JSON.stringify(ARROW_POS)
        : ITEM_NAMES[tgt()] + ' ' + poseStr(pickCfgs[tgt()] || pickCfgs[1]);
      // ── POSE MATHS ── the pose is stored as Euler (R = Rx(pitch)·Ry(yaw)·Rz(roll)); a quarter turn is far
      // easier applied to the axis triple, so build it, turn it, and read the angles back out.
      const poseAxes = (c) => { const cy = Math.cos(c.yaw), sy = Math.sin(c.yaw), cp = Math.cos(c.pitch), sp = Math.sin(c.pitch), cr = Math.cos(c.roll), sr = Math.sin(c.roll);
        return [[cr * cy, sr * cp + cr * sy * sp, sr * sp - cr * sy * cp],
                [-sr * cy, cr * cp - sr * sy * sp, cr * sp + sr * sy * cp],
                [sy, -cy * sp, cy * cp]]; };
      const axesToPose = (c, A) => { const [X, Y, Z] = A;
        c.yaw = Math.asin(Math.max(-1, Math.min(1, Z[0])));
        c.pitch = Math.atan2(-Z[1], Z[2]);
        c.roll = Math.atan2(-Y[0], X[0]); };
      const spinPose = (ax, dir) => { const c = pickCfgs[tgt()]; if (!c) return;   // a quarter turn about the item's OWN axis
        const A = poseAxes(c), s = dir > 0 ? 1 : -1;
        const rot = (a, b) => { const na = A[a].map((v, i) => A[b][i] * s), nb = A[b].map((v, i) => -A[a][i] * s); A[a] = na; A[b] = nb; };
        if (ax === 0) rot(1, 2); else if (ax === 1) rot(2, 0); else rot(0, 1);
        axesToPose(c, A); pickSave(); };
      const movePose = (ax, d) => { const c = pickCfgs[tgt()]; if (!c || !d) return;   // ONE VOXEL per notch, along the view's own right / up / forward
        const k = ['x', 'y', 'z'][ax];
        c[k] = +(c[k] + d * c.scale).toFixed(4); pickSave(); };
      const arwPaint = () => {
        const bow = onBow(), it = tgt(), p = bow ? (ARROW_POS[arwF()] || [0, 0, 0]) : (nudge[it] || (nudge[it] = [0, 0, 0]));
        now.textContent = bow ? ((bowLock >= 0 ? 'frame ' + String(arwF()).padStart(2, '0') : 'frame 00 · , . to pin') + '  ·  ' + p.join(' '))
                              : (ITEM_NAMES[it] || 'item') + '  ·  ' + p.join(' ');
        ap.querySelector('.arwHead span').textContent = bow ? 'arrow' : (ITEM_NAMES[it] || 'held item');
        $('arwBake').value = arwBakeStr();
        for (const s of ap.querySelectorAll('input[data-ax]')) { const k = +s.dataset.ax;
          s.value = p[k]; if (s.nextElementSibling) s.nextElementSibling.textContent = p[k]; } };
      arwSync = arwPaint;                                // , and . repaint the sliders onto the newly pinned frame
      let arwWatch = 0;
      const arwApply = () => { try { localStorage.setItem('vb_arrowRot', JSON.stringify(ARROW_ROT));
        localStorage.setItem('vb_arrowPos', JSON.stringify(ARROW_POS)); } catch (e) {}
        arwQueue(); arwPaint(); };
      for (const b of ap.querySelectorAll('button[data-ax]')) {
        b.addEventListener('pointerdown', (e) => e.stopPropagation());
        b.addEventListener('click', (e) => { e.stopPropagation();
          const ax = +b.dataset.ax, d = +b.dataset.d;
          if (onBow()) { ARROW_ROT[ax] = (ARROW_ROT[ax] + d + 4) & 3; arwApply(); }
          else { spinPose(ax, d); resetHist = 1; arwPaint(); } });
      }
      for (const s of ap.querySelectorAll('input[data-ax]')) {   // ── POSITION ── one voxel per notch: the arrow within the strip, or the held item in the frame
        s.addEventListener('pointerdown', (e) => e.stopPropagation());
        s.addEventListener('input', (e) => { e.stopPropagation();
          const ax = +s.dataset.ax, v = Math.max(-ARROW_POS_R, Math.min(ARROW_POS_R, parseInt(s.value, 10) || 0));
          if (onBow()) { const f = arwF(); if (!ARROW_POS[f]) ARROW_POS[f] = [0, 0, 0]; ARROW_POS[f][ax] = v; arwApply(); return; }
          const it = tgt(), n = nudge[it] || (nudge[it] = [0, 0, 0]);
          movePose(ax, v - n[ax]); n[ax] = v; resetHist = 1; arwPaint(); });
      }
      { const bi = $('arwBake'), bc = $('arwCopy');    // ── BAKE ── the whole placement as one line, ready to paste back into the source
        bi.addEventListener('pointerdown', (e) => e.stopPropagation());
        bi.addEventListener('click', (e) => { e.stopPropagation(); bi.select(); });
        bc.addEventListener('pointerdown', (e) => e.stopPropagation());
        bc.addEventListener('click', (e) => { e.stopPropagation();
          const done = () => { bc.textContent = 'copied!'; setTimeout(() => { bc.textContent = 'copy'; }, 1200); };
          if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(bi.value).then(done, () => { bi.select(); document.execCommand('copy'); done(); });
          else { bi.select(); document.execCommand('copy'); done(); } }); }
      $('arwReset').addEventListener('pointerdown', (e) => e.stopPropagation());
      $('arwReset').addEventListener('click', (e) => { e.stopPropagation();
        if (onBow()) { ARROW_ROT = ARROW_ROT0.slice(); ARROW_POS = ARROW_POS_DEF(); arwApply(); return; }
        const it = tgt(); if (pickCfgs[it] && PICK_DEFS[it]) Object.assign(pickCfgs[it], PICK_DEFS[it]);
        nudge[it] = [0, 0, 0]; pickSave(); resetHist = 1; arwPaint(); });
      requestAnimationFrame(() => { arwPaint();       // …first paint DEFERRED: heldIt / pickCfgs / ITEM_NAMES are declared below this block, and reading them during init throws (dead zone) and the loader never finishes
        setInterval(() => { const sig = (onBow() ? 'b' : '') + tgt() + '|' + bowLock; if (sig === arwWatch) return; arwWatch = sig; arwPaint(); }, 200); });   // the hand can change without the panel being touched
    }
    mkToggle('crdBtn', () => showCoords, (v) => showCoords = v, 'vb_coords');   // COORDS on/off
    mkToggle('fpsBtn', () => showFps, (v) => showFps = v, 'vb_fps');            // FPS on/off
    mkToggle('timeBtn', () => showTime, (v) => showTime = v, 'vb_time');        // TIME on/off
    mkToggle('resHudBtn', () => showRes, (v) => showRes = v, 'vb_res');         // RESOLUTION readout on/off
    // ── BACK-LIT FOLIAGE ── L is the only control, so the readout is the only feedback: it flashes the new
    // state and fades. One timer, restarted on every press, so holding L cannot stack fades.
    const vigBtn = $('vigBtn');
    const vigShow = () => { vigBtn.textContent = vigOn ? 'on' : 'off'; vigBtn.classList.toggle('on', vigOn); };
    vigBtn.addEventListener('pointerdown', (e) => e.stopPropagation());
    vigToggle = () => { vigOn = !vigOn;
      try { localStorage.setItem('vb_vig', vigOn ? '1' : '0'); } catch (e2) {}
      vigShow(); };
    vigBtn.addEventListener('click', (e) => { e.stopPropagation(); vigToggle(); });
    vigShow();
    const snowShow = () => { snowBtn.textContent = snowOn ? 'on' : 'off'; snowBtn.classList.toggle('on', snowOn); };
    snowBtnSync = snowShow;
    snowShow();
    snowBtn.addEventListener('pointerdown', (e) => e.stopPropagation());
    snowBtn.addEventListener('click', (e) => { e.stopPropagation(); snowOn = !snowOn;
      if (snowOn) { snowEndT = performance.now() + 60000; } else { snowNextT = performance.now() + 300000; }
      snowShow(); });
    // ── FULLSCREEN toggle (user) ── requestFullscreen needs a user gesture; the click qualifies. The label + .on state
    // follow the real fullscreen status (F11 or the OS chrome can change it out from under us), so fullscreenchange drives the sync.
    const fsBtn = $('fsBtn');
    const fsShow = () => { const on = !!document.fullscreenElement; fsBtn.textContent = on ? 'on' : 'off'; fsBtn.classList.toggle('on', on); };
    fsBtn.addEventListener('pointerdown', (e) => e.stopPropagation());
    fsBtn.addEventListener('click', (e) => { e.stopPropagation();
      try {
        if (document.fullscreenElement) { (document.exitFullscreen || document.webkitExitFullscreen).call(document); }
        else { const el = document.documentElement; const req = el.requestFullscreen || el.webkitRequestFullscreen;
          const p = req.call(el); if (p && p.catch) p.catch(() => {}); }
      } catch (err) {}
    });
    // ── AND ENGAGE THE ESCAPE LOCK ON THE WAY IN ── navigator.keyboard.lock() only works in fullscreen, so a
    // player who is already pointer-locked and THEN goes fullscreen would otherwise keep getting Chrome's
    // "press Esc to show your cursor" bubble until the next re-lock. escLock is a const in ui/input.js, which
    // the manifest places after this file — safe here because this only ever runs from an event, long after
    // both fragments have executed. See the note at escLock for why the bubble cannot simply be removed.
    const fsEsc = () => { fsShow(); try { if (document.fullscreenElement && typeof escLock === 'function') escLock(); } catch (e) {} };
    document.addEventListener('fullscreenchange', fsEsc);
    document.addEventListener('webkitfullscreenchange', fsEsc);
    fsShow(); }
  const sliderFill = (sl) => { const lo = +sl.min, hi = +sl.max, pct = hi > lo ? (sl.value - lo) / (hi - lo) * 100 : 0;
    sl.style.setProperty('--fill', pct + '%'); };   // paints the GREEN LINE up to the knob — re-run on every value change (input AND the per-frame tod sync)
  const volSlider = $('volSlider'), volLabel = $('volLabel');   // MASTER VOLUME — scales every registered sound's base volume (persisted vb_vol)
  volSlider.value = Math.round(sndVol * 100); sliderFill(volSlider);
  const volShow = () => { volLabel.textContent = 'volume ' + Math.round(sndVol * 100) + '%'; };
  volShow();
  volSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  volSlider.addEventListener('click', (e) => e.stopPropagation());
  volSlider.addEventListener('input', (e) => { e.stopPropagation(); sndVol = parseFloat(volSlider.value) / 100; applyVol(); volShow(); sliderFill(volSlider);
    try { localStorage.setItem('vb_vol', String(sndVol)); } catch (err) {} });
  const sfxSlider = $('sfxSlider'), sfxLabel = $('sfxLabel');   // SOUND EFFECTS — the second audio bus (persisted vb_sfx, and RESTORED on load like the master — user 2026-08-21). applyVol() re-levels every registered sound, so this reaches sounds that are already playing, not just the next one to start.
  sfxSlider.value = Math.round(sfxVol * 100); sliderFill(sfxSlider);
  const sfxShow = () => { sfxLabel.textContent = 'sfx ' + Math.round(sfxVol * 100) + '%'; };
  sfxShow();
  sfxSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  sfxSlider.addEventListener('click', (e) => e.stopPropagation());
  sfxSlider.addEventListener('input', (e) => { e.stopPropagation(); sfxVol = parseFloat(sfxSlider.value) / 100; applyVol(); sfxShow(); sliderFill(sfxSlider);
    try { localStorage.setItem('vb_sfx', String(sfxVol)); } catch (err) {} });
  const musSlider = $('musSlider'), musLabel = $('musLabel');   // MUSIC — the third audio bus (persisted vb_mus, and RESTORED on load like the two above it — user 2026-08-21). applyVol() re-levels every registered sound, so dragging this mid-anthem moves the track that is ALREADY playing, not just the next one.
  musSlider.value = Math.round(musVol * 100); sliderFill(musSlider);
  const musShow = () => { musLabel.textContent = 'music ' + Math.round(musVol * 100) + '%'; };
  musShow();
  musSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  musSlider.addEventListener('click', (e) => e.stopPropagation());
  musSlider.addEventListener('input', (e) => { e.stopPropagation(); musVol = parseFloat(musSlider.value) / 100; applyVol(); musShow(); sliderFill(musSlider);
    try { localStorage.setItem('vb_mus', String(musVol)); } catch (err) {} });
  const ambSlider = $('ambSlider'), ambLabel = $('ambLabel');   // AMBIENCE — the fourth bus: the forest and desert beds (persisted vb_amb, and RESTORED on load like the three above it — user 2026-08-21). applyVol() re-levels every registered sound, and ambBiomeTick recomputes the two beds every frame, so this moves the bed that is playing rather than the next one to start.
  ambSlider.value = Math.round(ambVol * 100); sliderFill(ambSlider);
  const ambShow = () => { ambLabel.textContent = 'ambience ' + Math.round(ambVol * 100) + '%'; };
  ambShow();
  ambSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  ambSlider.addEventListener('click', (e) => e.stopPropagation());
  ambSlider.addEventListener('input', (e) => { e.stopPropagation(); ambVol = parseFloat(ambSlider.value) / 100; applyVol(); ambShow(); sliderFill(ambSlider);
    try { localStorage.setItem('vb_amb', String(ambVol)); } catch (err) {} });
  const sensSlider = $('sensSlider'), sensLabel = $('sensLabel');   // MOUSE LOOK SENSITIVITY — scales the yaw/pitch multiplier (persisted vb_sens)
  sensSlider.value = Math.round(lookSens * 100); sliderFill(sensSlider);
  const sensShow = () => { sensLabel.textContent = 'sensitivity ' + Math.round(lookSens * 100) + '%'; };
  sensShow();
  sensSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  sensSlider.addEventListener('click', (e) => e.stopPropagation());
  sensSlider.addEventListener('input', (e) => { e.stopPropagation(); lookSens = parseFloat(sensSlider.value) / 100; sensShow(); sliderFill(sensSlider);
    try { localStorage.setItem('vb_sens', String(lookSens)); } catch (err) {} });
  const resSlider = $('resSlider'), resLabel = $('resLabel');   // RESOLUTION — render scale (persisted vb_scale); the [ ] keys nudge it too and resSync() keeps this in step
  const resShow = () => { resLabel.textContent = 'resolution ' + Math.round(renderScale * 100) + '%'; };
  const resSync = () => { resSlider.value = Math.round(renderScale * 100); sliderFill(resSlider); resShow(); };
  resSync();
  resSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  resSlider.addEventListener('click', (e) => e.stopPropagation());
  resSlider.addEventListener('input', (e) => { e.stopPropagation(); renderScale = Math.max(0.375, Math.min(1.0, parseFloat(resSlider.value) / 100)); makeTargets(true); resShow(); sliderFill(resSlider);
    try { localStorage.setItem('vb_scale', String(renderScale)); } catch (err) {} });
  // ── [ AND ] STEP IN 10s (user 2026-08-07) ── they moved in 0.125 before, off a 0.375 floor, so the keys and
  // the slider disagreed about which numbers exist: nudging landed on 62.5% or 87.5%, which the slider (40..100
  // in 10s) cannot represent, and resSync() then snapped its knob to a value the renderer was not using. One
  // grid now, in TENTHS as integers so repeated presses cannot drift the way 0.1 addition does. The floor is the
  // slider's 40%, not the old 37.5% — a value that low is still reachable, it is just no longer somewhere the
  // keys can strand you between two stops.
  const RES_LO = 4, RES_HI = 10;                       // tenths
  const resNudge = (d) => { renderScale = Math.max(RES_LO, Math.min(RES_HI, Math.round(renderScale * 10) + d)) / 10;
    makeTargets(true); resSync();
    try { localStorage.setItem('vb_scale', String(renderScale)); } catch (err) {} };
  const dofStrSlider = $('dofStrSlider'), dofStrLabel = $('dofStrLabel');   // DEPTH OF FIELD strength — scales the aperture (persisted vb_dofstr). Reads as a percentage OF THE DEFAULT, so 100% is the tuned look and the number means something without knowing what a circle of confusion is.
  const dofStrShow = () => { dofStrLabel.textContent = 'depth of field ' + Math.round(dofStr * 100) + '%'; };
  const dofStrSync = () => { dofStrSlider.value = Math.round(dofStr * 100); sliderFill(dofStrSlider); dofStrShow(); };   // named like resSync, and for the same reason: __vb.dof({strength}) drives the same state and the panel must not go stale behind it
  dofStrSync();
  dofStrSlider.addEventListener('pointerdown', (e) => e.stopPropagation());
  dofStrSlider.addEventListener('click', (e) => e.stopPropagation());
  dofStrSlider.addEventListener('input', (e) => { e.stopPropagation();
    const wasOff = dofStr <= 0;
    dofStr = parseFloat(dofStrSlider.value) / 100; dofCocK = DOF_COC * dofStr; dofStrShow(); sliderFill(dofStrSlider);
    if (wasOff && dofStr > 0) { dofInv = 1 / 24; }    // coming back up off zero, the STORED 1/d would rack the lens in from wherever it was parked — usually infinity after a spell looking at the horizon. This is the one thing the on/off button used to do that the slider has to inherit.
    try { localStorage.setItem('vb_dofstr2', String(dofStr)); } catch (err) {} });
  let todDrag = false;
  const todSlider = $('todSlider'), todLabel = $('todLabel');
  sliderFill(todSlider);
  todSlider.addEventListener('pointerdown', (e) => { todDrag = true; e.stopPropagation(); });
  addEventListener('pointerup', () => { todDrag = false; });
  todSlider.addEventListener('click', (e) => e.stopPropagation());
  todSlider.addEventListener('input', (e) => { e.stopPropagation(); tday = todSlider.value / 1440; sliderFill(todSlider); });
  $('kbClose').addEventListener('click', (e) => { e.stopPropagation(); kbPanel.classList.add('hidden'); listenAction = null; });
  kbPanel.addEventListener('click', (e) => { e.stopPropagation();   // stopPropagation so a click in here never reaches the document handler that enters the game
    if (!e.target.closest('#kbCard2, #kbCard3, #kbCard4')) { kbPanel.classList.add('hidden'); listenAction = null; } });   // ONE selector list, not a chain of closest() calls: a card added to the panel and forgotten here would close the whole thing the moment you touched it. Clicked ANYWHERE outside the boxes — the backdrop OR the kbWrap gap/under a shorter box — closes settings and falls back to the esc menu (user)
  document.addEventListener('keydown', (e) => {                      // capture phase — rebinding swallows the key
    // Escape closes the tuning panel through its own DONE BUTTON rather than hiding the element (user 2026-08-18).
    // Hiding it raw skipped pkShow → setLightMode(false), so `lightMode` stayed true after the panel was gone: the
    // pointer stayed unlocked, and the esc-menu suppression that reads lightMode (ui/input.js) kept the pause menu
    // from opening. Only a canvas click recovered. The button is wired to pkShow, so the click is the whole fix.
    if (!listenAction) { if (e.code === 'Escape') { kbPanel.classList.add('hidden');
      const pk = $('pkPanel'), pc = $('pkClose'); if (pk && !pk.classList.contains('hidden')) { if (pc) pc.click(); else pk.classList.add('hidden'); } } return; }
    e.preventDefault(); e.stopPropagation();
    if (e.code !== 'Escape') { binds[listenAction] = e.code; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (err) {} }
    listenAction = null; kbRefresh();
  }, true);

  while (P.y < WY - 20 && !boxFree(P.x, P.y, P.z, HEIGHT)) P.y += 1;   // spawn embedded in a log/pebble → pop up, don't crawl sideways
  smoothEye = P.y + EYE;

