  // ── frame loop ─────────────────────────────────────────────────────────────
  const halton = (i, b) => { let f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i = (i / b) | 0; } return r; };
  const JIT = Array.from({ length: 8 }, (_, i) => [halton(i + 1, 2) - 0.5, halton(i + 1, 3) - 0.5]);
  let frame = 0, prevT = performance.now(), fpsEma = 60, hudT = 0;
  let profQS = null, profRes = null, profStg = null, profBusy = false, profNew = false;   // per-pass GPU timing — armed via __vb.prof(true)
  const profEma = [0, 0, 0, 0, 0, 0, 0], PROF_NAMES = ['trace', 'temporal', 'spatial', 'composite', 'taa', 'vis', 'blit'];
  // ── RAW PER-FRAME MINIMUM ── the EMA is unusable as an A/B statistic while ANOTHER process is using the
  // GPU: a timestamp query measures wall time on the GPU timeline, so a preempted pass reports inflated
  // duration, and a LONG pass (trace) absorbs far more of that than a short one (blit). Measured on this
  // box against a byte-identical build under two names: trace swung 0.77 → 3.55 ms while blit held
  // 0.123-0.125. The MINIMUM over a few hundred readbacks is the pass's uncontended cost — the camera is
  // static and the only per-frame variation is the AO/sun jitter, so the cheapest frame is the honest one.
  const profMin = [1e9, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9]; let profSamp = 0;
  function profArm(on) {
    if (on && PROF && !profQS) {
      profQS = device.createQuerySet({ type: 'timestamp', count: 14 });   // 5 run() passes + the VIS prepass (10/11) + the blit render pass (12/13)
      profRes = device.createBuffer({ size: 112, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
      profStg = device.createBuffer({ size: 112, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    } else if (!on && profQS) { profQS.destroy(); profRes.destroy(); profStg.destroy(); profQS = profRes = profStg = null; }
  }
  let waitEma = 4, paceWaited = true, paceTs = 0;      // measured pipeline-wait (CPU idle) — sizes the worldgen budget precisely
  // Pacing continuations, hoisted out of the frame loop so no closure is allocated per frame.
  // paceFail keeps the old contract: a REJECTED onSubmittedWorkDone (device reset mid-walk) must not end the
  // .then chain silently — that was a PERMANENT FREEZE with no error. Fall back to rAF pacing instead.
  const paceOk = () => { waitEma += (Math.min(30, performance.now() - paceTs) - waitEma) * 0.08; paceWaited = true; tick(performance.now()); };
  const paceFail = (err) => { vbNoteErr('gpu pacing rejected', err); paceWaited = false; requestAnimationFrame(tick); };
  let gpuPrevDone = Promise.resolve();                 // completion of frame N-1 — pacing on it keeps 2 frames in flight, CPU prep overlaps GPU work
  const prevCam = { pos: [0, 0, 0], right: [1, 0, 0], up: [0, 1, 0], fwd: [0, 0, 1], tanH: 1, aspect: 1, jit: [0, 0] };   // pos in WORLD coords
  let heldOff = null;                                  // the held item's anchor in CAMERA space (right/up/fwd) — where the bow actually sits, so an arrow can leave from IT and not the player's middle
  const FOV = 72 * Math.PI / 180;

  finishLoad();                                        // world ready → sweep the meter to full SMOOTHLY (the trickle owns 0→90; this is the last 90→100)
  const revealGame = () => loadEl.classList.add('hidden');   // drop the whole overlay, revealing the LIVE game; a canvas click locks the mouse and takes control (user)
  if (CDPTEST) revealGame(); else setTimeout(revealGame, 1150);   // let the 0.85 s finish GLIDE complete, then hold the full 100% bar a beat so it's actually seen, before the overlay drops (tests reveal instantly)
  $('playHint').textContent = isMobile ? 'find a good computer' : 'press any button';   // MOBILE (user): tell a handheld to go find a real computer instead of "press any button"
  if (!CDPTEST) setTimeout(() => { if (!locked) $('playHint').classList.remove('hidden'); }, 1150);   // reveal the prompt with the game, after the overlay lifts
  const PICK_DEFS = {                                  // PER-ITEM held poses — tuning one item never moves the others (panel copy-pose exports)
    1: { x: 0.91, y: -0.1, z: 0.96, yaw: 0.04, pitch: -1.42, roll: 1.58, scale: 0.08 },   // stone axe (user bake 2026-07-15)
    2: { x: 0.75, y: -0.24, z: 0.96, yaw: -0.41, pitch: -2.28, roll: 1.52, scale: 0.08 },   // small rock (user bake 2026-07-16)
    3: { x: 0.75, y: -0.1, z: 0.84, yaw: -0.08, pitch: -2.812, roll: -1.64, scale: 0.08 },   // twig (user bake 2026-08-04)
    4: { x: 0.9, y: -0.24, z: 0.96, yaw: -0.09, pitch: -1.29, roll: 1.52, scale: 0.08 },   // pinecone (user bake 2026-07-16)
  };
  PICK_DEFS[KNIFE_IT] = { x: 0.75, y: -0.1, z: 0.84, yaw: 0.07, pitch: -1.32, roll: 3.13, scale: 0.07 };   // stone knife (user bake 2026-07-16)
  if (PICK_IT) PICK_DEFS[PICK_IT] = { x: 0.91, y: -0.1, z: 0.96, yaw: 0.04, pitch: -1.42, roll: 1.58, scale: 0.08 };   // stone pick — starts on the AXE's baked pose (same haft, same swing); tune it live in the held-item panel and re-bake
  if (SHOVEL_IT) PICK_DEFS[SHOVEL_IT] = { x: 0.91, y: -0.1, z: 0.96, yaw: 0.04, pitch: 1.72, roll: 3.15, scale: 0.08 };   // stone shovel — the axe's pose with the roll flipped a half-turn, so the SPADE points UP (user)
  if (HOE_IT) PICK_DEFS[HOE_IT] = { x: 0.91, y: -0.1, z: 0.96, yaw: 0.04, pitch: -1.42, roll: -1.562, scale: 0.08 };   // stone hoe — user bake 2026-08-04
  if (SPEAR_IT) PICK_DEFS[SPEAR_IT] = { x: 0.91, y: -0.1, z: 0.96, yaw: 0.04, pitch: -1.42, roll: 1.58, scale: 0.08 };   // stone spear — likewise
  if (MEAT_IT) PICK_DEFS[MEAT_IT] = { x: 0.8, y: -0.18, z: 0.92, yaw: 0.67, pitch: 0.97, roll: 2.1, scale: 0.07 };   // raw meat — user bake 2026-08-03
  // LOOSE ARROW (the one that flies and lands) — the tool family's own anchor and scale, but stood UP with the
  // STONE HEAD AT THE TOP (user). The art runs tip → shaft → fletching along its depth axis, so pitch π turns
  // that axis to point straight down and the head ends up highest; the small roll gives it the tools' lean.
  if (ARROW_IT) PICK_DEFS[ARROW_IT] = { x: 0.91, y: -0.1, z: 0.96, yaw: 0, pitch: 3.14, roll: 0.08, scale: 0.08 };   // arrow — user bake 2026-08-04
  if (BOW_IT) PICK_DEFS[BOW_IT] = { x: 1.09, y: -0.14, z: 1.02, yaw: 0.01, pitch: 1.57, roll: -0.06, scale: 0.106 };   // bow — user bake 2026-08-04   // the tool family's anchor, but a BOW-sized model (user): its art is 1 voxel wide, so the axe's scale read as a sliver   // bow — user bake 2026-08-04   // bow — held upright across the hand, smaller scale because the model is much longer than a hand tool. Tune live in the held-item panel.
  if (WORM_ITEM0) PICK_DEFS[WORM_ITEM0] = { x: 0.81, y: -0.22, z: 1.13, yaw: 0.3, pitch: -0.14, roll: -3.14, scale: 0.08 };   // live worm (user bake 2026-07-17)
  const pickCfgs = { 1: { ...PICK_DEFS[1] }, 2: { ...PICK_DEFS[2] }, 3: { ...PICK_DEFS[3] }, 4: { ...PICK_DEFS[4] } };
  pickCfgs[KNIFE_IT] = { ...PICK_DEFS[KNIFE_IT] };
  if (PICK_IT) pickCfgs[PICK_IT] = { ...PICK_DEFS[PICK_IT] };
  if (SHOVEL_IT) pickCfgs[SHOVEL_IT] = { ...PICK_DEFS[SHOVEL_IT] };
  if (ARROW_IT) pickCfgs[ARROW_IT] = { ...PICK_DEFS[ARROW_IT] };
  if (BOW_IT) { const bp = { ...PICK_DEFS[BOW_IT] };   // EVERY draw frame carries the SAME pose (user): the bow bends in the hand, the hand does not move
    for (let f = 0; f < BOW_FRAMES; f++) { PICK_DEFS[BOW_IT + f] = { ...bp }; pickCfgs[BOW_IT + f] = { ...bp }; } }
  if (MEAT_IT) pickCfgs[MEAT_IT] = { ...PICK_DEFS[MEAT_IT] };
  if (HOE_IT) pickCfgs[HOE_IT] = { ...PICK_DEFS[HOE_IT] };
  if (SPEAR_IT) pickCfgs[SPEAR_IT] = { ...PICK_DEFS[SPEAR_IT] };
  if (WORM_ITEM0) pickCfgs[WORM_ITEM0] = { ...PICK_DEFS[WORM_ITEM0] };
  const ITEM_NAMES = { 1: 'axe', 2: 'rock', 3: 'twig', 4: 'pinecone' };
  ITEM_NAMES[KNIFE_IT] = 'knife';
  if (PICK_IT) ITEM_NAMES[PICK_IT] = 'pick';
  if (SHOVEL_IT) ITEM_NAMES[SHOVEL_IT] = 'shovel';
  if (ARROW_IT) ITEM_NAMES[ARROW_IT] = 'arrow';
  if (HOE_IT) ITEM_NAMES[HOE_IT] = 'hoe';
  if (SPEAR_IT) ITEM_NAMES[SPEAR_IT] = 'spear';
  if (BOW_IT) for (let f = 0; f < BOW_FRAMES; f++) ITEM_NAMES[BOW_IT + f] = 'bow';   // every frame answers to the same name in the held-item editor
  if (MEAT_IT) ITEM_NAMES[MEAT_IT] = 'raw meat';
  if (WORM_ITEM0) ITEM_NAMES[WORM_ITEM0] = 'worm';
  try {                                              // ── SAVED POSES ── keyed by NAME (see ITEM_NAMES above); ids are positional and shift whenever the item list changes
    const byName = JSON.parse(localStorage.getItem('vb_pick4') || '{}');   // MUST stay BELOW the ITEM_NAMES block: this loop reads it, and above the declaration every restore threw a dead-zone ReferenceError into the catch — every tuned pose silently lost on refresh, and the vb_pick3 migration never ran (settings.js defers its own first paint for exactly this hazard)
    for (const id in pickCfgs) { const nm = ITEM_NAMES[id]; if (nm && byName[nm]) Object.assign(pickCfgs[id], byName[nm]); }
    if (!localStorage.getItem('vb_pick4')) {         // one-time migration off the id-keyed store
      const old = JSON.parse(localStorage.getItem('vb_pick3') || '{}');
      for (const k in old) { const id = +k;
        if (!BOW_IT || id < BOW_IT) { if (pickCfgs[id]) Object.assign(pickCfgs[id], old[k]); }   // below the bow the numbering never moved — those are still valid
      }                                              // …and everything from the bow up is dropped: its number no longer means what it did
    }
  } catch (e) { console.warn('[vb] held-item poses could not be restored', e); }   // key bumped from vb_pick2 — the old value was one shared pose. NOT silent any more: a bare catch here is what hid the dead-zone bug above for as long as it existed; a blocked/corrupt localStorage still degrades to the baked defaults, it just says so.
  const heldIt = () => (grabAnim && !grabAnim.left && !slots[selSlot] ? grabAnim.it : (slots[selSlot] ? slots[selSlot].it : 0));   // the item the RIGHT hand is showing (0 = empty). A grab flight into a FULL hand does not borrow it (user): that pickup flies as its own world object so your tool stays out — see grabGhost. An EMPTY hand borrows it again, the way it did originally, so the incoming item flies into the HELD POSE itself and there is no tool to lose.
  let grabGhost = null;                                // the item mid-flight, drawn through the DROP path instead of the hand
  const heldCfg = (it) => pickCfgs[it] || pickCfgs[1];
  const pickSave = () => { try {                     // …written back BY NAME, so a future item-list change cannot scramble them again
    const byName = {}; for (const id in pickCfgs) { const nm = ITEM_NAMES[id]; if (nm) byName[nm] = pickCfgs[id]; }
    localStorage.setItem('vb_pick4', JSON.stringify(byName)); } catch (e) {} };
  {                                                    // ── held-item position panel ── live sliders over the CURRENT item's own pose; the item renders behind the menu as you drag
    const pkPanel = $('pkPanel'), pkRows = $('pkRows');
    const PKDEF = [['x', -1.5, 1.5, 0.01, 'move right'], ['y', -1.5, 0.5, 0.01, 'move up'], ['z', 0.2, 2.5, 0.01, 'move forward'],
                   ['yaw', -3.14, 3.14, 0.01, 'rotate yaw'], ['pitch', -3.14, 3.14, 0.01, 'rotate pitch'], ['roll', -3.14, 3.14, 0.01, 'rotate roll'], ['scale', 0.01, 0.15, 0.002, 'size']];
    let pkIt = 1;                                      // which item the sliders are bound to — resolved from the hand each time the panel refreshes
    const pkStr = () => JSON.stringify(Object.fromEntries(Object.entries(pickCfgs[pkIt]).map(([k, v]) => [k, +(+v).toFixed(3)])));
    function pkRefresh() {
      pkIt = heldIt() || 1;                            // empty hand → tune the axe
      const cfg = pickCfgs[pkIt];
      pkPanel.querySelector('h2').textContent = 'held item — ' + ITEM_NAMES[pkIt];
      pkRows.innerHTML = '';
      for (const [k, mn, mx, st, name] of PKDEF) {
        const row = document.createElement('div'); row.className = 'pkRow';
        const lbl = document.createElement('span'); lbl.textContent = name;
        const inp = document.createElement('input'); inp.type = 'range'; inp.min = mn; inp.max = mx; inp.step = st; inp.value = cfg[k];
        const val = document.createElement('span'); val.className = 'pkVal'; val.textContent = (+cfg[k]).toFixed(k === 'scale' ? 3 : 2);
        inp.addEventListener('input', (e) => { e.stopPropagation(); cfg[k] = parseFloat(inp.value); val.textContent = (+cfg[k]).toFixed(k === 'scale' ? 3 : 2); pickSave();
          const j = document.getElementById('pkJson'); if (j) j.value = pkStr(); });
        inp.addEventListener('pointerdown', (e) => e.stopPropagation());
        row.appendChild(lbl); row.appendChild(inp); row.appendChild(val); pkRows.appendChild(row);
      }
      const row = document.createElement('div'); row.className = 'pkRow';   // COPY row — paste this string back to bake a pose into the code as the default
      const inp = document.createElement('input'); inp.type = 'text'; inp.readOnly = true; inp.id = 'pkJson'; inp.value = pkStr();
      inp.style.cssText = 'flex:1;background:#0c1118;border:1px solid #33405a;border-radius:8px;color:#9fd07a;font:inherit;padding:4px 6px;min-width:0;';
      inp.addEventListener('click', (e) => { e.stopPropagation(); inp.select(); });
      const btn = document.createElement('button'); btn.className = 'kbKey'; btn.textContent = 'copy'; btn.style.minWidth = '56px';
      btn.addEventListener('click', (e) => { e.stopPropagation();
        const done = () => { btn.textContent = 'copied!'; setTimeout(() => { btn.textContent = 'copy'; }, 1200); };
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(inp.value).then(done, () => { inp.select(); document.execCommand('copy'); done(); });
        else { inp.select(); document.execCommand('copy'); done(); } });
      row.appendChild(inp); row.appendChild(btn); pkRows.appendChild(row);
    }
    // the ESC-menu button was removed (user 2026-08-05); the panel and every slider below stay wired, so
    // __vb.pick() and a re-added button both still work. Guarded because the element is no longer there.
    { const pb = $('pkBtn'); if (pb) pb.addEventListener('click', (e) => { e.stopPropagation(); pkPanel.classList.remove('hidden'); pkRefresh(); }); }
    $('pkClose').addEventListener('click', (e) => { e.stopPropagation(); pkPanel.classList.add('hidden'); });
    $('pkReset').addEventListener('click', (e) => { e.stopPropagation(); Object.assign(pickCfgs[pkIt], PICK_DEFS[pkIt]); pickSave(); pkRefresh(); });
    pkPanel.addEventListener('click', (e) => { e.stopPropagation();
      if (e.target === pkPanel) pkPanel.classList.add('hidden'); });   // backdrop click closes it too, matching the settings panel
  }

