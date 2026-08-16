  // ── input ──────────────────────────────────────────────────────────────────
  let cmpOn = true; try { cmpOn = localStorage.getItem('vb_cmp') !== '0'; } catch (e) {}   // compass on/off, persisted, default ON — declared up here with vigOn for the same TDZ reason
  let lightMode = false;                               // L: cursor released for the light panel only — no esc menu, no camera drift
  let locked = false;
  // lockEl (the esc menu) stays hidden at boot — after loading, the overlay is removed to reveal the live game and a
  // click on the canvas takes control; the esc menu only appears on the first pointer-unlock (pressing Esc mid-game).
  const CDPTEST = location.search.includes('cdp');   // headless test harness: pointer lock in headless Chrome still ClipCursor-pins the REAL Windows cursor to the hidden window — never request it under test
  const LIFE_TRACE = !location.search.includes('oldlife');   // ── DYNAMIC LIFE ── trace-injected creature rendering (full SVGF path); ?oldlife falls back to the analytic composite path for A/B
  let lifeDbg = 0;                                     // life debug view (u.lifeCfg.x) — __vb.lifedbg(mode): 1 slot ids, 2 history confidence, 3 motion vectors, 4 raw AO
  // ── COMPASS ── a scrolling heading ribbon at top centre. yaw 0 faces +Z, so N=+Z, E=+X, S=-Z, W=-X and
  // turning right walks the ribbon left. Three full turns are laid out so the wrap seam is never on screen.
  const CMP_W = 240, CMP_PPD = 2;                      // container width, px per degree (360° = 720 px)
  const cmpTrack = $('cmpTrack'), cmpEl = $('compass');
  {
    const marks = [[0, 'N'], [45, 'ne'], [90, 'E'], [135, 'se'], [180, 'S'], [225, 'sw'], [270, 'W'], [315, 'nw']];
    let html = '';
    for (let turn = 0; turn < 3; turn++) {
      for (const [b, t] of marks) html += '<span style="left:' + ((turn * 360 + b) * CMP_PPD) + 'px">' + t + '</span>';
      for (let b = 15; b < 360; b += 15) if (b % 45) html += '<span class="tick" style="left:' + ((turn * 360 + b) * CMP_PPD) + 'px">·</span>';
    }
    cmpTrack.innerHTML = html;
    cmpTrack.style.width = (3 * 360 * CMP_PPD) + 'px';
  }
  const cmpVis = () => { const showCmp = locked && cmpOn;   // compass shows while LOCKED + setting on — in the ASSET EDITOR too now (user), where it plays over the top-centre
    cmpEl.classList.toggle('hidden', !showCmp);
    if (ED.on) { const h = $('edHud'); h.classList.remove('hidden'); h.classList.toggle('below', showCmp); } };   // editor: the frame counter always shows; while the compass is up it drops BELOW it (centred), otherwise sits at the top (user)
  const cmpUpdate = () => {                            // middle turn is the live one, so the ribbon never runs off either end
    if (!cmpOn) return;                                // switched off — don't pay for a layout write nobody sees
    const hdg = ((P.yaw * 180 / Math.PI) % 360 + 360) % 360;
    cmpTrack.style.transform = 'translateX(' + (CMP_W / 2 - (360 + hdg) * CMP_PPD) + 'px)';
  };
  { const cmpBtn = $('cmpBtn');                        // the icon toggles the compass (user)
    const cmpShow = () => { cmpBtn.classList.toggle('on', cmpOn); cmpBtn.title = cmpOn ? 'compass: on' : 'compass: off'; cmpVis(); };
    cmpBtn.addEventListener('pointerdown', (e) => e.stopPropagation());
    cmpBtn.addEventListener('click', (e) => { e.stopPropagation(); cmpOn = !cmpOn;
      try { localStorage.setItem('vb_cmp', cmpOn ? '1' : '0'); } catch (e2) {}
      cmpShow(); });
    cmpShow(); }
  const tryLock = () => { if (CDPTEST) { locked = true; lockEl.classList.add('hidden'); crossEl.classList.remove('hidden'); cmpVis(); cursSync(); return; } canvas.requestPointerLock(); };
  const setLightMode = (on) => {                        // L: hand the cursor to the light panel, and nothing else
    lightMode = !!on;
    if (lightMode) { try { document.exitPointerLock(); } catch (e) {} }
    else if (!locked) tryLock();
    lockEl.classList.toggle('hidden', locked || dead || lightMode || !vePanel.classList.contains('hidden'));
    cursSync();
  };
  document.addEventListener('keydown', (e) => {        // ── , / . ── step the held bow through its draw frames and PIN it there, so the arrow can be placed frame by frame (user)
    if ((e.code !== 'Comma' && e.code !== 'Period') || e.repeat || listenAction || !BOW_FRAMES) return;
    if (ED.on || CMD.open || dead || !vePanel.classList.contains('hidden')) return;   // the asset editor owns these two keys while it is up
    if (document.activeElement && /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName)) return;
    e.preventDefault();
    const n = BOW_FRAMES;                              // …and one step past the last frame hands the draw back to the game
    bowLock = e.code === 'Period' ? (bowLock < 0 ? 0 : (bowLock + 1 >= n ? -1 : bowLock + 1))
                                  : (bowLock < 0 ? n - 1 : (bowLock - 1 < 0 ? -1 : bowLock - 1));
    arwSync();
  });
  // ── L DOES NOTHING ── it hands the cursor to the water panel, and with that panel off screen all it does is
  // take the pointer away mid-game. The listener is gone; setLightMode and lightMode stay wired (the esc-menu
  // suppression and cursSync still read them, harmlessly false forever), so restoring the key is re-adding
  // this one listener.
  lockEl.addEventListener('click', tryLock);
  canvas.addEventListener('click', () => { if (!locked) { lightMode = false; tryLock(); } });   // clicking the world takes control back and ends light mode
  document.addEventListener('keydown', (e) => {        // "press any button" — ANY key leaves the start prompt (a click still works via the canvas handler above)
    if (locked || $('playHint').classList.contains('hidden')) return;
    $('playHint').classList.add('hidden');             // drop the text the instant a key is pressed — don't wait on a successful lock (Esc can't lock, so it would otherwise stay stuck on screen)
    if (e.code === 'Escape') lockEl.classList.remove('hidden');   // Esc can't request pointer lock → count it as a button press and just open the esc menu
    else tryLock();
  });
  document.addEventListener('pointerlockerror', () => { if (!locked && performance.now() - CMD.escAt > 1600) lockEl.classList.remove('hidden'); });   // …but never right after the command line was dismissed with Escape: the refusal IS the browser's exit cooldown (user)   // if a lock request is refused, fall back to the esc menu so the player is never stuck
  // ── CUSTOM FREE-MOUSE CURSOR ── shown whenever the pointer is NOT locked (menus, the video editor, the death screen).
  // Follows the mouse as a crosshair and morphs to a square over anything clickable; the native cursor is hidden so the
  // two never double up. While locked the game's own centre crosshair takes over and this is hidden.
  const cursEl = $('curs');
  const stackEl = $('stackN');                         // the x2..x8 badge; only redrawn when the count actually changes
  let stackShown = -1;
  const cursSync = () => { document.body.classList.toggle('freecur', !locked); if (locked) cursEl.classList.remove('sq');
    $('arwPanel').classList.toggle('hidden', locked || !$('over').classList.contains('hidden'));   // the arrow buttons are only REACHABLE with a free cursor, so that is exactly when they show — but never over the death screen. (Read from the DOM, not `dead`: cursSync runs once during init, BEFORE that binding exists.)
    // ── THE WATER PANEL SHOWS ON THE SAME RULE ── free cursor, never over the death screen, plus the video
    // editor, which owns the screen outright. Moot while the CSS keeps it off screen, but the class stays
    // honest so deleting that one rule is all it takes to bring it back.
    $('lgtPanel').classList.toggle('hidden', (locked && !lightMode) || !$('over').classList.contains('hidden') || !vePanel.classList.contains('hidden'));
    $('lgtPanel').classList.toggle('lit', lightMode); };   // the panel highlights while it has the cursor, so the mode is never ambiguous
  const CURS_HIT = 'button, .veTool, .veCtl, a, input:not([type="range"]), select, [role="button"]';   // ONLY genuinely clickable controls square the cursor. RANGE sliders are excluded (user): the cursor stays a cross over them and the KNOB grows on hover instead (see the slider CSS). `#lock` (the whole esc-menu backdrop) used to be in here too, so the cursor was a square across the ENTIRE menu; the buttons/anchors inside already match on their own.
  cursSync();                                        // the game boots UNLOCKED (click-to-enter), so the custom cursor is live from the first frame
  document.addEventListener('mousemove', (e) => {      // separate listener: the game's own mousemove early-returns while unlocked
    if (locked) return;
    cursEl.style.transform = 'translate3d(' + e.clientX + 'px,' + e.clientY + 'px,0)';
    const t = e.target;
    const over = !!(t && t.closest && t.closest(CURS_HIT) && !(t.closest('button') && t.closest('button').disabled));
    cursEl.classList.toggle('sq', over);
  }, true);
  let freshLock = false;                               // set when pointer lock is (re)acquired — the FIRST mousemove after it can carry a huge accumulated delta (the browser warps the cursor to centre on lock); swallow that one so the camera never snaps (user: "camera angle changes abruptly")
  document.addEventListener('pointerlockchange', () => {
    const wasLocked = locked;
    locked = document.pointerLockElement === canvas;
    if (locked && !wasLocked) freshLock = true;
    lockEl.classList.toggle('hidden', locked || dead || lightMode || CMD.open || performance.now() - CMD.escAt < 1600 || !vePanel.classList.contains('hidden'));   // …or while the COMMAND LINE is up, or across the re-lock retry after Escape dismissed it (user)   // never surface the esc menu on top of the open video editor — or over LIGHT MODE, which released the cursor on purpose
    if (locked) $('playHint').classList.add('hidden');   // taken control → drop the click-to-enter prompt (the esc menu handles re-entry from here)
    crossEl.classList.toggle('hidden', !locked);
    cmpVis();                                    // the compass follows the crosshair, gated by the settings toggle
    cursSync();
    keys.clear(); mouse0 = false; mouse2 = false;   // losing the pointer drops the throw wind-up too
  });
  document.addEventListener('mousemove', (e) => {
    if (!locked) return;
    if (!vePanel.classList.contains('hidden')) return;   // VIDEO EDITOR open over the screen → the mouse drives its UI, never the camera behind it (user)
    if (cineMode && !ED.on) return;                    // CINEMA owns the camera (user): cineUpdate overwrote yaw/pitch every frame anyway, so dragging did nothing but fight it — and a fast flick still showed as a one-frame jolt before it was corrected
    if (ED.on && ED.dragAxis >= 0) {                    // dragging a move-gizmo arrow: map mouse motion onto the axis' on-screen direction, not the camera
      const A = ED.dragAxis, cp = Math.cos(P.pitch), sp = Math.sin(P.pitch);
      const fwd = [Math.sin(P.yaw) * cp, sp, Math.cos(P.yaw) * cp], rgt = [Math.cos(P.yaw), 0, -Math.sin(P.yaw)];
      const up = [fwd[1] * rgt[2] - fwd[2] * rgt[1], fwd[2] * rgt[0] - fwd[0] * rgt[2], fwd[0] * rgt[1] - fwd[1] * rgt[0]];
      const axS = A === 0 ? rgt[0] : A === 1 ? rgt[1] : rgt[2];   // this axis' screen-right + screen-up components
      const ayS = A === 0 ? up[0] : A === 1 ? up[1] : up[2];
      ED.dragAcc += (e.movementX * axS - e.movementY * ayS) * 0.05;
      while (ED.dragAcc >= 1) { edOffset(A, 1); ED.dragAcc -= 1; }
      while (ED.dragAcc <= -1) { edOffset(A, -1); ED.dragAcc += 1; }
      return;                                           // suppress camera look while dragging
    }
    if (ED.on && ED.dragRing >= 0) {                    // dragging a rotation RING: yaw = drag left/right, pitch = drag up/down → 90° steps, direction from the drag sign
      const RSTEP = 55;
      ED.dragRAcc += ED.dragRing === 0 ? e.movementX : -e.movementY;
      while (ED.dragRAcc >= RSTEP) { edApplyRot(ED.dragRing === 0 ? 'yaw' : 'pitch', 1); ED.dragRAcc -= RSTEP; }
      while (ED.dragRAcc <= -RSTEP) { edApplyRot(ED.dragRing === 0 ? 'yaw' : 'pitch', -1); ED.dragRAcc += RSTEP; }
      return;                                           // suppress camera look while rotating
    }
    if (freshLock) { freshLock = false; return; }      // drop the first post-lock event: its delta is the cursor-warp jump, not real aim
    const ls = lookMul();
    const CAMCAP = 260;                                // one genuine flick never exceeds this per mousemove; a larger delta is a driver/relock spike → clamp it so the view can't teleport
    const dx = Math.max(-CAMCAP, Math.min(CAMCAP, e.movementX)), dy = Math.max(-CAMCAP, Math.min(CAMCAP, e.movementY));
    P.yaw += dx * ls;
    P.pitch = Math.max(-1.55, Math.min(1.55, P.pitch - dy * ls));
  });
  let dead = false;
  let vbLavaT = 0, vbSandT = 0, vbDrownT = 0, vbCactT = 0;   // hazard damage cooldowns — a hazard ticks damage, it no longer kills on contact
  let uwT = 0;                                          // ── DROWN CLOCK ── seconds the EYE has been continuously submerged; hits DROWN_T → game over. Reset on surfacing/respawn; frozen (not reset) while paused/editor.
  const DROWN_T = 10;                                  // you can hold your breath for 10 s underwater (user)
  const SINK_IN = 2.2, SINK_OUT = 14;                  // quicksand: sink 22 cm/s standing on a sand flat, climb back out ~6× faster once you're off it
  const die = (why) => {                               // every death routes through here so the game-over screen always says what killed you
    if (dead) return;
    dead = true;
    const w = $('overWhy'); if (w) w.textContent = why;
    $('over').classList.remove('hidden');
    lockEl.classList.add('hidden');                    // never show the ESC menu under the game-over screen
    try { document.exitPointerLock(); } catch (e) {}
  };
  VIT.onDeath = die;                                   // the vitals own mortality now; die() is just the game-over screen
  $('over').addEventListener('click', () => { dead = false; $('over').classList.add('hidden'); if (ED.on) edExit();   // NEVER respawn in the asset editor (user) — force-exit it, and lock straight back into the GAME rather than the menu, so a respawn-click can't land on the editor button
    respawn(); tryLock(); });
  let tday = 7 / 24, cycleSpeed = 1, godRays = true;   // day/night cycle: 20-min day at 1x; STARTS AT 7:00 am on load (user)
  let moonDay = 4;                                     // moon PHASE day counter — starts at 4/8 = full moon; each in-game day advances the phase
  // ── THE SCROLL-UP HINT (user 2026-08-07) ── shown once, ten seconds into a session, and retired the first
  // time the player scrolls up. `shArmT` is set when they actually take control rather than at page load, so
  // the ten seconds is PLAYTIME and the clock does not run out behind the loading video or the esc menu. There
  // is no localStorage: the user asked for it to come back on every refresh, so the state is a plain variable.
  const shEl = $('scrollHint');
  // ── AND IT BELONGS TO THE GAME, NOT THE MENU (user 2026-08-07) ── pressing Esc takes it off screen and
  // coming back brings it straight back. The ten seconds is PLAYTIME, so the clock only accumulates while the
  // player actually has control: time spent reading the esc menu does not bring the hint any closer.
  // DISABLED (user 2026-08-15). shDone is the flag the hint already uses to mean "retired, never show again",
  // so starting it true retires it before it can arm - no other branch has to change, and re-enabling is
  // flipping this one word back to false. The element and its timer logic are left intact below.
  let shElapsed = 0, shLast = 0, shShown = false, shDone = true, shVis = false, shHideT = 0;
  const shTick = (nowMs) => {
    if (shDone) return;
    if (locked) { if (shLast) shElapsed += nowMs - shLast; shLast = nowMs; } else shLast = 0;
    if (!shShown && shElapsed >= 10000) shShown = true;
    const want = shShown && locked;
    if (want === shVis) return;
    shVis = want;
    if (want) { shEl.classList.remove('hidden'); shHideT = 0;
                requestAnimationFrame(() => { if (shVis) shEl.classList.add('show'); }); }   // un-hide first, THEN add the class — a display change in the same frame as the opacity change gives no transition to run
    else { shEl.classList.remove('show'); shHideT = nowMs + 700; }
    };
  const shDismiss = () => {                            // scrolled — fade out and never again this refresh
    if (shDone) return;
    shDone = true; shVis = false;
    shEl.classList.remove('show');
    setTimeout(() => shEl.classList.add('hidden'), 700);   // after the 0.6 s opacity transition, stop painting it
  };
  addEventListener('wheel', (e) => {                   // scroll = cycle hand slots; ALT + scroll = cycle speed (sensitive: x1.6 per notch)
    if (e.deltaY) shDismiss();                         // …and ANY scroll retires it (user 2026-08-07): a player who scrolls at all has found the control, so scrolling DOWN must fade it out too rather than leaving it up as a nag. Sits here — before the lock/dead guards below, which must not keep it on screen. Not gated on shShown: a player who already scrolls up inside the first ten seconds has demonstrated they know, so the hint must never appear at all (user 2026-08-07)
    if (e.altKey) {
      e.preventDefault();
      cycleSpeed = Math.max(0.25, Math.min(512, cycleSpeed * (e.deltaY < 0 ? 1.6 : 1 / 1.6)));
      return;
    }
    if (!locked || dead || !e.deltaY) return;
    e.preventDefault();
    selSlot = (selSlot + (e.deltaY < 0 ? 1 : slots.length - 1)) % slots.length;   // wraps the WHOLE list now, however long it has grown   // SCROLL UP ADVANCES (user 2026-08-07): up from the axe reaches the pick, then the shovel — the direction the on-screen hint is pointing
  }, { passive: false });
  // (The L relief knob is GONE — the desert relief is fixed at DESREL = 24 in world/window.js, so there is
  //  nothing left to tune. It set ?desrel and reloaded; both the flag and the panel were removed together.)
  document.addEventListener('keydown', (e) => {
    if (CMD.open) return;                               // the COMMAND LINE has the keyboard (user)
    if (!locked) return;
    if (e.code === 'KeyT' && !ED.on) { e.preventDefault(); cmdShow(true); return; }   // ── T ── open the command line (user)
    keys.add(e.code);
    if (e.code === binds.drop) { dropHeld(); }
    if (e.code === binds.fly) { P.fly = !P.fly; P.vy = 0; }   // toggle fly (user re-added the F keybind 2026-07-22)
    // R-key recording is BACK ON with the #veBtn button (user 2026-08-02, reversing the 2026-07-23 disable).
    if (e.code === binds.record && (!ED.on || !ED.paused)) { veToggleRec(); }   // R records / stops the screen; in the editor it STILL records unless the bunny is already selected (then 'r' rotates the frame — see below)
    if (e.code === 'KeyH' && !ED.on) { rerollSpawn(); }      // H — RESET the spawn to a fresh random patch of the world (console logs the coords to bake into the code)
    if (e.code === 'KeyP') { snowOn = !snowOn;               // P — toggle the snow storm (user); mirrors the settings snow button EXACTLY so the two stay in sync
      if (snowOn) { snowEndT = performance.now() + 60000; } else { snowNextT = performance.now() + 300000; }
      snowBtnSync(); }
    if (e.code === binds.scaledown) { resNudge(-1); }   // [ and ] — one 10% step on the same grid the settings slider uses (see resNudge)
    if (e.code === binds.scaleup) { resNudge(1); }
    if (e.code === 'KeyC' && !ED.on) { cineMode = !cineMode; document.body.classList.toggle('noui', cineMode);   // C — CINEMATIC still hold: freezes the camera + hides the HUD for a clean framed view; press again to exit + regain control
      if (cineMode) { cine.baseYaw = P.yaw; cine.basePitch = P.pitch; cine.baseX = P.x; cine.baseY = P.y; cine.baseZ = P.z; cine.bobT = 0; cine.roll = 0; P.fly = true; P.vy = 0; } }   // STILL HOLD (user): snapshot the exact framed shot; the camera stays put and only sways
    else if (e.code === 'KeyC' && ED.on) { document.body.classList.toggle('noui'); }   // in the editor, C still just toggles the HUD
    if (ED.on) {                                       // asset editor: , / . scrub frames, E move the frame, R rotate it, ← / → reorder
      if (e.code === 'Comma') edSelStep(-1);
      if (e.code === 'Period') edSelStep(1);
      if (e.code === 'ArrowLeft') edMoveStep(-1);
      if (e.code === 'ArrowRight') edMoveStep(1);
      if (e.code === 'KeyE') { ED.paused = true; ED.giz = !ED.giz; if (ED.giz) ED.rgiz = false; edEnsureGizCols(); edLayout(); }   // toggle the XYZ move-gizmo on the selected frame
      if (e.code === 'KeyR' && ED.paused) { ED.rgiz = !ED.rgiz; if (ED.rgiz) ED.giz = false; edEnsureRgizCols(); edLayout(); }   // toggle the ROTATE-gizmo — ONLY when the bunny is already selected (user); an R press with nothing selected records instead (never auto-selects)
      if (e.code === 'KeyB') edSwapBunnies();          // swap which bunny (left/jump ↔ right/rotate) is the EDITABLE one
      // (KeyH per-heading alignment REMOVED — alignment is authored ONCE in SOUTH; armOffset auto-derives every heading with the rigid rotation + parity correction, so there is nothing to cycle through or export per heading.)
    }
    if (e.code === binds.jump || e.code === binds.crouch) e.preventDefault();   // Space (scroll) and whatever crouch is bound to — Alt used to be stolen by the browser for the menu bar. NOTE preventDefault cannot stop CAPS LOCK toggling the OS state; it only keeps the key out of the browser's own handling.
  });
  document.addEventListener('keyup', (e) => keys.delete(e.code));

