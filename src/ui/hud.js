  // ── frame loop ─────────────────────────────────────────────────────────────
  const halton = (i, b) => { let f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i = (i / b) | 0; } return r; };
  const JIT = Array.from({ length: 8 }, (_, i) => [halton(i + 1, 2) - 0.5, halton(i + 1, 3) - 0.5]);
  let frame = 0, prevT = performance.now(), fpsEma = 60, hudT = 0;
  let profQS = null, profRes = null, profStg = null, profBusy = false, profNew = false;   // per-pass GPU timing — armed via __vb.prof(true)
  const profEma = [0, 0, 0, 0, 0, 0, 0, 0], PROF_NAMES = ['trace', 'temporal', 'spatial', 'composite', 'taa', 'vis', 'blit', 'god'];
  // ── RAW PER-FRAME MINIMUM ── the EMA is unusable as an A/B statistic while ANOTHER process is using the
  // GPU: a timestamp query measures wall time on the GPU timeline, so a preempted pass reports inflated
  // duration, and a LONG pass (trace) absorbs far more of that than a short one (blit). Measured on this
  // box against a byte-identical build under two names: trace swung 0.77 → 3.55 ms while blit held
  // 0.123-0.125. The MINIMUM over a few hundred readbacks is the pass's uncontended cost — the camera is
  // static and the only per-frame variation is the AO/sun jitter, so the cheapest frame is the honest one.
  const profMin = [1e9, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9]; let profSamp = 0;
  function profArm(on) {
    if (on && PROF && !profQS) {
      profQS = device.createQuerySet({ type: 'timestamp', count: 16 });   // 5 run() passes + the VIS prepass (10/11) + the blit render pass (12/13) + the half-res god rays (14/15).
      // A pass with no query pair is INVISIBLE here while still costing frame time: when the god march moved out of BLIT it took 1.3 ms of
      // measured cost with it and reappeared nowhere, so __vb.prof()'s total silently understated the frame. Index i reads q[2i], q[2i+1] —
      // adding a pass means a name, a query pair, the count, both buffer sizes, the resolve length and the readback bound, or the totals lie.
      profRes = device.createBuffer({ size: 128, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
      profStg = device.createBuffer({ size: 128, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
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
  const heldBob = [0, 0];                              // …and the VIEW-MODEL BOB alone (walk sway + idle breathing, x/y in the same camera units), split out of that anchor so the health row can ride the same rig without inheriting the tool's pose, swing or swap. Written once per frame in tick-camera, read by the heart block below it — one expression, so the row and the hand can never sway differently.
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
  // …and the steak is a STRIP now too (it is eaten down like the fruit — see eatStrip in assets/held-items.js),
  // so its one baked pose is copied across the run for the reason the bow's and the fruit's are: the meat is
  // eaten in the hand, and the hand does not move.
  if (MEAT_IT) { const mp = { x: 0.8, y: -0.18, z: 0.92, yaw: 0.67, pitch: 0.97, roll: 2.1, scale: 0.07 };   // raw meat — user bake 2026-08-03
    for (let f = 0; f < FOOD_EAT_N; f++) PICK_DEFS[MEAT_IT + f] = { ...mp }; }
  // LOOSE ARROW (the one that flies and lands) — the tool family's own anchor and scale, but stood UP with the
  // STONE HEAD AT THE TOP (user). The art runs tip → shaft → fletching along its depth axis, so pitch π turns
  // that axis to point straight down and the head ends up highest; the small roll gives it the tools' lean.
  if (ARROW_IT) PICK_DEFS[ARROW_IT] = { x: 0.91, y: -0.1, z: 0.96, yaw: 0, pitch: 3.14, roll: 0.08, scale: 0.08 };   // arrow — user bake 2026-08-04
  if (BOW_IT) PICK_DEFS[BOW_IT] = { x: 1.09, y: -0.14, z: 1.02, yaw: 0.01, pitch: 1.57, roll: -0.06, scale: 0.106 };   // bow — user bake 2026-08-04   // the tool family's anchor, but a BOW-sized model (user): its art is 1 voxel wide, so the axe's scale read as a sliver   // bow — user bake 2026-08-04   // bow — held upright across the hand, smaller scale because the model is much longer than a hand tool. Tune live in the held-item panel.
  if (WORM_ITEM0) PICK_DEFS[WORM_ITEM0] = { x: 0.81, y: -0.22, z: 1.13, yaw: 0.3, pitch: -0.14, roll: -3.14, scale: 0.08 };   // live worm (user bake 2026-07-17)
  // ── THE TWO FRUIT ── EVERY frame of an eat strip carries the SAME pose, exactly as the bow's draw frames do
  // and for the same reason: the apple is eaten in the hand, the hand does not move. So this is one pose copied
  // across the run, and a tug on the sliders moves all thirteen together.
  // STARTED FROM THE RAW MEAT'S BAKE, which is the only food pose the user has actually tuned, with two changes.
  // The scale is up a touch because the fruit is a 4x3x5 ball where the steak is a 5x6x1 slab. And x is nudged
  // right to cancel the eat strip's PADDING: assets/held-items.js grows every frame to one shared box so the
  // core cannot swim as the leaf tumbles, the DDA centres a model on that box, and the apple itself lives in the
  // left of it — so without this the fruit would hang a couple of voxels inboard of where the steak does.
  // A STARTING BAKE, not a finished one. Poses save by NAME to localStorage, so dragging these in the held-item
  // panel (__vb.pick()) sticks, and the copy row prints the line to paste back here.
  // ── THE USER'S OWN BAKE (2026-08-17), copied from the held-item panel's copy row ── and the ORANGE takes
  // the IDENTICAL pose, at the user's request: the two models are the same 4x3x5 ball, so one pose is
  // right for both and having them differ would only be an accident nobody chose. One object, spread
  // across all 26 items (13 apple frames + 13 orange), so a future tug in the panel still moves a whole
  // strip together and the two fruit cannot drift apart in code — only a deliberate edit here separates
  // them. NOTE poses also save by NAME to localStorage and a SAVED pose wins over this default, so if you
  // have already dragged the orange's sliders this session, clear it (or drag it) to see this take.
  if (APPLE_IT) { const fp = { x: 1.09, y: -0.26, z: 0.92, yaw: 0.12, pitch: -1.32, roll: 1.01, scale: 0.075 };
    for (let f = 0; f < FOOD_EAT_N; f++) { PICK_DEFS[APPLE_IT + f] = { ...fp }; PICK_DEFS[ORANGE_IT + f] = { ...fp }; } }
  if (STICK_BLOS_IT) PICK_DEFS[STICK_BLOS_IT] = { ...PICK_DEFS[3] };   // the blossom twig is stick_1 with a recoloured leaf — same geometry, same anchor, so it holds in the hand exactly as the green one does and starts on its bake
  const pickCfgs = { 1: { ...PICK_DEFS[1] }, 2: { ...PICK_DEFS[2] }, 3: { ...PICK_DEFS[3] }, 4: { ...PICK_DEFS[4] } };
  if (STICK_BLOS_IT) pickCfgs[STICK_BLOS_IT] = { ...PICK_DEFS[STICK_BLOS_IT] };
  pickCfgs[KNIFE_IT] = { ...PICK_DEFS[KNIFE_IT] };
  if (PICK_IT) pickCfgs[PICK_IT] = { ...PICK_DEFS[PICK_IT] };
  if (SHOVEL_IT) pickCfgs[SHOVEL_IT] = { ...PICK_DEFS[SHOVEL_IT] };
  if (ARROW_IT) pickCfgs[ARROW_IT] = { ...PICK_DEFS[ARROW_IT] };
  if (BOW_IT) { const bp = { ...PICK_DEFS[BOW_IT] };   // EVERY draw frame carries the SAME pose (user): the bow bends in the hand, the hand does not move
    for (let f = 0; f < BOW_FRAMES; f++) { PICK_DEFS[BOW_IT + f] = { ...bp }; pickCfgs[BOW_IT + f] = { ...bp }; } }
  if (MEAT_IT) for (let f = 0; f < FOOD_EAT_N; f++) pickCfgs[MEAT_IT + f] = { ...PICK_DEFS[MEAT_IT + f] };
  if (HOE_IT) pickCfgs[HOE_IT] = { ...PICK_DEFS[HOE_IT] };
  if (SPEAR_IT) pickCfgs[SPEAR_IT] = { ...PICK_DEFS[SPEAR_IT] };
  if (WORM_ITEM0) pickCfgs[WORM_ITEM0] = { ...PICK_DEFS[WORM_ITEM0] };
  if (APPLE_IT) for (let f = 0; f < FOOD_EAT_N; f++) { pickCfgs[APPLE_IT + f] = { ...PICK_DEFS[APPLE_IT + f] }; pickCfgs[ORANGE_IT + f] = { ...PICK_DEFS[ORANGE_IT + f] }; }
  const ITEM_NAMES = { 1: 'axe', 2: 'rock', 3: 'twig', 4: 'pinecone' };
  if (STICK_BLOS_IT) ITEM_NAMES[STICK_BLOS_IT] = 'twig';   // ── THE SAME NAME AS ITEM 3, DELIBERATELY ── the held-pose and stack-badge panels group by NAME (see the peers helpers below), so the green twig and the blossom twig share one pose, one badge placement and one saved bake. Without this the pink one is a nameless id: no pose entry, no badge trim, and neither panel can bind to it.
  ITEM_NAMES[KNIFE_IT] = 'knife';
  if (PICK_IT) ITEM_NAMES[PICK_IT] = 'pick';
  if (SHOVEL_IT) ITEM_NAMES[SHOVEL_IT] = 'shovel';
  if (ARROW_IT) ITEM_NAMES[ARROW_IT] = 'arrow';
  if (HOE_IT) ITEM_NAMES[HOE_IT] = 'hoe';
  if (SPEAR_IT) ITEM_NAMES[SPEAR_IT] = 'spear';
  if (BOW_IT) for (let f = 0; f < BOW_FRAMES; f++) ITEM_NAMES[BOW_IT + f] = 'bow';   // every frame answers to the same name in the held-item editor
  if (MEAT_IT) for (let f = 0; f < FOOD_EAT_N; f++) ITEM_NAMES[MEAT_IT + f] = 'raw meat';   // every eat frame answers to the one name — that is what makes ONE saved pose cover the whole strip
  if (APPLE_IT) for (let f = 0; f < FOOD_EAT_N; f++) { ITEM_NAMES[APPLE_IT + f] = 'apple'; ITEM_NAMES[ORANGE_IT + f] = 'orange'; }   // every eat frame answers to the one name, like the bow's draw frames — that is what makes ONE saved pose cover the whole strip, and it is also what makes `/spawn apple` work without a CMD_FILES entry (ui/console.js falls back to a name search over this table)
  if (WORM_ITEM0) ITEM_NAMES[WORM_ITEM0] = 'worm';
  // ── THE SEVEN NUMBERS A POSE IS, AND ITS FINGERPRINT ── the signature is of the BAKED DEFAULT a saved
  // pose was tuned against, and it is what makes editing PICK_DEFS above take effect (user 2026-08-17:
  // "the baked apple position is not carying over when baking it in here"). See the restore below.
  const POSE_K = ['x', 'y', 'z', 'yaw', 'pitch', 'roll', 'scale'];
  const poseSig = (p) => p ? POSE_K.map((k) => +(+p[k]).toFixed(4)).join(',') : '';
  try {                                              // ── SAVED POSES ── keyed by NAME (see ITEM_NAMES above); ids are positional and shift whenever the item list changes
    // ── …AND A RE-BAKE IN THE CODE WINS OVER A STALE SAVE (user 2026-08-17) ── a saved pose used to win
    // unconditionally and forever, so pasting the panel's copy row back into PICK_DEFS above changed
    // nothing you could see: localStorage still held whatever was there before and quietly overwrote it on
    // every boot. The only way to see a new bake was to clear the key by hand, which is not a thing a bake
    // should require. So each entry now records the DEFAULT IT WAS TUNED AGAINST, and a saved pose is kept
    // only while that default still matches the code. Edit the line above and the save is recognised as
    // being about a bake that no longer exists, and is dropped — which is exactly what "bake it in here"
    // is supposed to mean. Drag the sliders again and the new save fingerprints the new default.
    // NOT MIGRATED FROM vb_pick4, deliberately: an old entry carries no fingerprint, so there is no way to
    // tell a deliberate tuning from the stale copy that was masking a bake — and every pose in PICK_DEFS is
    // itself a user bake, so falling back to the code is falling back to the user's own numbers. The old
    // key is left on disk rather than deleted, so nothing is actually destroyed.
    const byName = JSON.parse(localStorage.getItem('vb_pick5') || '{}');   // MUST stay BELOW the ITEM_NAMES block: this loop reads it, and above the declaration every restore threw a dead-zone ReferenceError into the catch — every tuned pose silently lost on refresh (settings.js defers its own first paint for exactly this hazard)
    const stale = [];
    for (const id in pickCfgs) { const nm = ITEM_NAMES[id], sv = nm ? byName[nm] : null; if (!sv) continue;
      if (sv._b !== poseSig(PICK_DEFS[id])) { if (stale.indexOf(nm) < 0) stale.push(nm); continue; }   // the code has been re-baked since this was saved — the code wins
      for (const k of POSE_K) if (typeof sv[k] === 'number') pickCfgs[id][k] = sv[k]; }
    if (stale.length) console.log('[vb] held-item poses: the code bake moved for', stale.join(', '), '— those saved poses were dropped so the new default takes');
  } catch (e) { console.warn('[vb] held-item poses could not be restored', e); }   // NOT silent: a bare catch here is what hid a dead-zone bug for as long as it existed; a blocked/corrupt localStorage still degrades to the baked defaults, it just says so.
  // ══ THE STACK BADGE (user 2026-08-17: "give me some sliders to adjust the positioning of the x(#) … put
  // it on the l keybind underneath the current item positioning box") ══ the x2..x8 that hangs beside the hand
  // when you carry more than one of something. It is drawn INTO THE IMAGE by BLIT (not the DOM — the old
  // top-right HTML badge is retired), which is why it needs a uniform lane and a panel rather than a CSS rule:
  // u.badge, written every frame in main/tick-camera.js. These four numbers were literals in the shader.
  // WHERE IT SITS IS NOT TUNED ANY MORE (user 2026-08-17: "auto adjust the number count in the top right of
  // the held item in hand. always in the top right"). main/tick-camera.js measures the held model's own
  // apparent box every frame and puts the badge on the top-right of it, so WHERE it sits already tracks
  // whatever is in the hand — a 5x6x1 steak and a 7x3x6 apple get different pixels with nothing tuned.
  // What the panel holds is the TRIM on top of that:
  //   x / y  a NUDGE off that corner, measured in badge pixels so it holds at any resolution. +y is DOWN.
  //   size   a multiplier on the glyph pixel, which BLIT floors to whole screen pixels so it cannot shimmer.
  //   tilt   radians. Tilt + shear is what makes the badge sit IN the scene rather than flat on the glass.
  // ── AND THE TRIM IS PER ITEM (user 2026-08-17: "make it where each item can have its own number bake") ──
  // one object per id on exactly the machinery the held POSES above run on, and for the same reasons: keyed
  // by NAME in storage because ids are positional and shift whenever the item list changes, fingerprinted
  // with `_b` so editing a bake here beats a stale save, and moved as a NAME GROUP by the panel so every
  // frame of an eat strip or a bow draw carries one placement and the badge cannot jump mid-animation.
  // Anything not named below starts at SB_BASE; paste the panel's copy row in to bake one.
  const SB_K = ['x', 'y', 'size', 'tilt'];
  const SB_BASE = { x: 1, y: 0, size: 1, tilt: -0.26 };   // one badge pixel clear of the corner, top-aligned with it
  const sbSig = (p) => p ? SB_K.map((k) => +(+p[k]).toFixed(4)).join(',') : '';
  const SB_DEFS = {};
  for (const id in ITEM_NAMES) SB_DEFS[id] = { ...SB_BASE };   // every id that answers to a name — which is exactly the set the hand can show and the set a save can be keyed by
  // ── PER-ITEM BAKES ── paste a copy row here, spread across the strip if the item is one:
  //   if (APPLE_IT) for (let f = 0; f < FOOD_EAT_N; f++) SB_DEFS[APPLE_IT + f] = { x: 0.1, y: 5, size: 1, tilt: -0.26 };
  // THE FRUIT (user 2026-08-18). Spread across the whole eat strip, not written to the whole
  // fruit at once: every frame of a bite answers to the same name, so a placement on frame 0 alone
  // would let the badge jump the moment you took a bite. The orange carries the apple's numbers
  // deliberately (user: "use the same stack position for the orange as well") — the two models are
  // the same size and sit in the same pose, so one placement is correct for both; a shared const
  // rather than two literals so they cannot drift apart in a later edit.
  // Note x = -48: the old slider stopped at -20 and could not have reached this, which is the
  // clamp/range fix above earning its keep rather than a coincidence.
  const SB_FRUIT = { x: -48, y: 23, size: 1, tilt: -0.26 };
  for (const base of [APPLE_IT, ORANGE_IT]) if (base) for (let f = 0; f < FOOD_EAT_N; f++) SB_DEFS[base + f] = { ...SB_FRUIT };
  // THE HELD STICK (user 2026-08-18). One id, not a strip — held-items.js pushes a single item for stick_1.vox
  // — and it answers to 'twig' rather than 'stick': ITEM_NAMES hardcodes 3:'twig' and ui/console.js maps every
  // spelling (stick, stick_1, stick_2, twig) onto that same id, so the panel titles it "stack count — twig".
  // Keyed on STICK_IT and not the literal 3 so an item-list change cannot silently move this onto something else.
  if (STICK_IT) SB_DEFS[STICK_IT] = { x: -51.5, y: 0, size: 1, tilt: -0.26 };
  if (STICK_BLOS_IT) SB_DEFS[STICK_BLOS_IT] = { x: -51.5, y: 0, size: 1, tilt: -0.26 };   // the blossom twig is the same object in the hand, so it carries the same badge placement
  const sbCfgs = {}; for (const id in SB_DEFS) sbCfgs[id] = { ...SB_DEFS[id] };
  const sbFor = (it) => sbCfgs[it] || SB_BASE;         // read-only fallback: an id with no name (nothing the hand shows) takes the base rather than minting an entry from a render loop
  // ── THE KEY IS BUMPED TO …3 (user 2026-08-18) ── the trim's UNIT changed with the slider fix: x/y used to be
  // multiplied by the FLOORED glyph pixel and are now multiplied by the unfloored one (main/tick-camera.js), so at a
  // 865-tall canvas the very same stored number renders 35% further out than it did when it was tuned. `_b` cannot
  // catch that — it fingerprints the DEFAULTS, and the defaults did not move — so a stale save would have quietly
  // relocated every hand-placed badge with nothing on screen to explain it. Dropping the old key is the honest
  // migration and costs little: every placement in it was tuned through the clamp bug this change fixes, i.e. through
  // a slider that could not reach most of its own range. vb_stackbadge2 is left on disk rather than deleted.
  try { const byName = JSON.parse(localStorage.getItem('vb_stackbadge3') || '{}');
    const stale = [];
    for (const id in sbCfgs) { const nm = ITEM_NAMES[id], sv = nm ? byName[nm] : null; if (!sv) continue;
      if (sv._b !== sbSig(SB_DEFS[id])) { if (stale.indexOf(nm) < 0) stale.push(nm); continue; }   // the code bake moved since this was saved — the code wins
      for (const k of SB_K) if (typeof sv[k] === 'number') sbCfgs[id][k] = sv[k]; }
    if (stale.length) console.log('[vb] stack badge: the code bake moved for', stale.join(', '), '— those saved placements were dropped so the new ones take');
  } catch (e) { console.warn('[vb] stack-badge placements could not be restored', e); }
  const sbSave = () => { try {
    const byName = {}; for (const id in sbCfgs) { const nm = ITEM_NAMES[id]; if (nm) byName[nm] = Object.assign({ _b: sbSig(SB_DEFS[id]) }, sbCfgs[id]); }
    localStorage.setItem('vb_stackbadge3', JSON.stringify(byName)); } catch (e) {} };
  const pkPanelEl = $('pkPanel');                      // …read once per frame by tick-camera: while this panel is up the badge is FORCED visible (see the note there), or there is nothing on screen to drag the sliders against
  const sbOpen = () => !pkPanelEl.classList.contains('hidden');
  const heldIt = () => (grabAnim && !grabAnim.left && !slots[selSlot] ? grabAnim.it : (slots[selSlot] ? slots[selSlot].it : 0));   // the item the RIGHT hand is showing (0 = empty). A grab flight into a FULL hand does not borrow it (user): that pickup flies as its own world object so your tool stays out — see grabGhost. An EMPTY hand borrows it again, the way it did originally, so the incoming item flies into the HELD POSE itself and there is no tool to lose.
  let grabGhost = null;                                // the item mid-flight, drawn through the DROP path instead of the hand
  const heldCfg = (it) => pickCfgs[it] || pickCfgs[1];
  const pickSave = () => { try {                     // …written back BY NAME, so a future item-list change cannot scramble them again
    // Every id wearing a name writes the same entry, and after the panel fix below every frame of a strip
    // genuinely holds the same pose — which is what makes last-id-wins harmless here. It was NOT harmless
    // before: the panel edited the ONE frame in your hand while this loop stored the LAST id of the strip,
    // so a tug on the apple's sliders saved frame 12's untouched copy and threw the tuning away. That
    // stored copy was the old baked default, and it then masked every re-bake for good — the other half of
    // the same report. `_b` is the default it was tuned against; the restore above is where it earns its keep.
    const byName = {}; for (const id in pickCfgs) { const nm = ITEM_NAMES[id]; if (nm) byName[nm] = Object.assign({ _b: poseSig(PICK_DEFS[id]) }, pickCfgs[id]); }
    localStorage.setItem('vb_pick5', JSON.stringify(byName)); } catch (e) {} };
  {                                                    // ── held-item position panel ── live sliders over the CURRENT item's own pose; the item renders behind the menu as you drag
    const pkPanel = $('pkPanel'), pkRows = $('pkRows');
    const PKDEF = [['x', -1.5, 1.5, 0.01, 'move right'], ['y', -1.5, 0.5, 0.01, 'move up'], ['z', 0.2, 2.5, 0.01, 'move forward'],
                   ['yaw', -3.14, 3.14, 0.01, 'rotate yaw'], ['pitch', -3.14, 3.14, 0.01, 'rotate pitch'], ['roll', -3.14, 3.14, 0.01, 'rotate roll'], ['scale', 0.01, 0.15, 0.002, 'size']];
    let pkIt = 1;                                      // which item the sliders are bound to — resolved from the hand each time the panel refreshes
    // ── …AND EVERY ITEM THAT ANSWERS TO ITS NAME MOVES WITH IT ── the apple, the orange and the bow are
    // STRIPS: thirteen (or eight) ids that share one name and are meant to carry one pose, so that the
    // fruit does not jump the moment you bite it and the bow does not jump as it draws. That was written
    // down where the defaults are set and never implemented here — the sliders bound to pickCfgs[pkIt]
    // alone, i.e. to whichever single frame happened to be in the hand. Two bugs fell out of it and this
    // is the fix for both: the pose you dragged applied to one frame of the animation, and pickSave then
    // stored a DIFFERENT frame's copy under the shared name (user 2026-08-17).
    const pkPeers = () => { const nm = ITEM_NAMES[pkIt], out = [];
      for (const id in pickCfgs) if (ITEM_NAMES[id] === nm) out.push(pickCfgs[id]);
      return out.length ? out : [pickCfgs[pkIt]]; };
    const pkStr = () => JSON.stringify(Object.fromEntries(Object.entries(pickCfgs[pkIt]).map(([k, v]) => [k, +(+v).toFixed(3)])));
    function pkRefresh() {
      pkIt = heldIt() || 1;                            // empty hand → tune the axe
      const cfg = pickCfgs[pkIt];
      pkPanel.querySelector('h2').textContent = 'held item — ' + ITEM_NAMES[pkIt];
      pkRows.innerHTML = '';
      for (const [k, mn, mx, st, name] of PKDEF) {
        const row = document.createElement('div'); row.className = 'pkRow';
        const lbl = document.createElement('span'); lbl.textContent = name;
        const inp = document.createElement('input'); inp.type = 'range'; inp.min = mn; inp.max = mx; inp.step = st; inp.value = cfg[k]; sliderFill(inp);   // …and the gold rail is PAINTED (ui/settings.js). Without this call the CSS var falls back to 50%, so every knob would sit on a half-filled bar whatever its value — worse than no fill, because it reads as a real reading
        const val = document.createElement('span'); val.className = 'pkVal'; val.textContent = (+cfg[k]).toFixed(k === 'scale' ? 3 : 2);
        inp.addEventListener('input', (e) => { e.stopPropagation(); const v9 = parseFloat(inp.value); for (const c9 of pkPeers()) c9[k] = v9; val.textContent = (+cfg[k]).toFixed(k === 'scale' ? 3 : 2); sliderFill(inp); pickSave();
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
    // ── …AND THE SECOND CARD, UNDER IT ── same row markup, same copy-to-bake row, same save-by-fingerprint
    // rule, and now the same PER-ITEM binding as the card above it: the sliders bind to whatever the hand is
    // showing and a drag moves every id that answers to its name, so a strip keeps one placement.
    // ── THE RANGES ARE THE OTHER HALF OF THE "BARELY MOVES" REPORT (user 2026-08-18) ── the real culprit was a
    // clamp in main/tick-camera.js that outranked the slider (fixed there), but even unblocked these numbers
    // were thin: a badge pixel is CH/320, so at a 865-tall canvas the old +/-20 was +/-54 screen px of total
    // authority over a placement that can start a couple of hundred px away. +/-60 badge px is the same nudge
    // in spirit — still measured off the model's own corner, still resolution independent — with enough reach
    // to actually land it. SIZE went the other way, from 0.05 to 0.25: BLIT floors the glyph pixel to a whole
    // screen pixel (it must, or the badge shimmers), so at 865 the old step gave 55 slider positions and only
    // SEVEN distinct outcomes — 89% of a drag did nothing, and the bottom third was one flat plateau. A step
    // that cannot be seen is not precision, it is a slider that reads as broken; 0.25 puts most positions on a
    // different glyph pixel. The MINIMUM stays low (0.25, not 0.5) because the plateau is a property of THIS height,
    // not of the setting: at 865 everything under ~0.74 floors to the same 2 px, but on a 4K panel 0.25 and 0.5 are
    // genuinely different glyph pixels, and a range trimmed to what one monitor can show is the same mistake in
    // miniature. Every stop is an exact multiple of the step, so no saved value lands off-grid.
    // Bake finer numbers by hand in SB_DEFS if a specific item ever wants one.
    const SBDEF = [['x', -60, 60, 0.5, 'nudge right'], ['y', -60, 60, 0.5, 'nudge down'], ['size', 0.25, 3, 0.25, 'size'], ['tilt', -1.57, 1.57, 0.01, 'tilt']];
    const sbRows = $('sbRows'), sbTitle = $('sbCard').querySelector('h2');
    let sbIt = 1;                                      // which item the badge sliders are bound to — resolved from the hand each time the panel refreshes, exactly as pkIt is
    const sbPeers = () => { const nm = ITEM_NAMES[sbIt], out = [];
      for (const id in sbCfgs) if (ITEM_NAMES[id] === nm) out.push(sbCfgs[id]);
      return out.length ? out : [sbFor(sbIt)]; };
    const sbStr = () => JSON.stringify(Object.fromEntries(SB_K.map((k) => [k, +(+sbFor(sbIt)[k]).toFixed(3)])));
    function sbRefresh() {
      sbIt = heldIt() || 1;                            // empty hand → tune the axe, the same fallback the pose card takes
      const cfg = sbFor(sbIt);
      sbTitle.textContent = 'stack count — ' + (ITEM_NAMES[sbIt] || '?');
      sbRows.innerHTML = '';
      for (const [k, mn, mx, st, name] of SBDEF) {
        const row = document.createElement('div'); row.className = 'pkRow';
        const lbl = document.createElement('span'); lbl.textContent = name;
        const inp = document.createElement('input'); inp.type = 'range'; inp.min = mn; inp.max = mx; inp.step = st; inp.value = cfg[k]; sliderFill(inp);
        const val = document.createElement('span'); val.className = 'pkVal'; val.textContent = (+cfg[k]).toFixed(2);
        inp.addEventListener('input', (e) => { e.stopPropagation(); const v9 = parseFloat(inp.value); for (const c9 of sbPeers()) c9[k] = v9; val.textContent = (+cfg[k]).toFixed(2); sliderFill(inp); sbSave();
          const j = document.getElementById('sbJson'); if (j) j.value = sbStr(); });
        inp.addEventListener('pointerdown', (e) => e.stopPropagation());
        row.appendChild(lbl); row.appendChild(inp); row.appendChild(val); sbRows.appendChild(row);
      }
      const row = document.createElement('div'); row.className = 'pkRow';   // COPY row — paste this string into SB_DEFS above, under this item's id, to bake its placement into the code
      const inp = document.createElement('input'); inp.type = 'text'; inp.readOnly = true; inp.id = 'sbJson'; inp.value = sbStr();
      inp.style.cssText = 'flex:1;background:#0c1118;border:1px solid #33405a;border-radius:8px;color:#9fd07a;font:inherit;padding:4px 6px;min-width:0;';
      inp.addEventListener('click', (e) => { e.stopPropagation(); inp.select(); });
      const btn = document.createElement('button'); btn.className = 'kbKey'; btn.textContent = 'copy'; btn.style.minWidth = '56px';
      btn.addEventListener('click', (e) => { e.stopPropagation();
        const done = () => { btn.textContent = 'copied!'; setTimeout(() => { btn.textContent = 'copy'; }, 1200); };
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(inp.value).then(done, () => { inp.select(); document.execCommand('copy'); done(); });
        else { inp.select(); document.execCommand('copy'); done(); } });
      row.appendChild(inp); row.appendChild(btn); sbRows.appendChild(row);
    }
    $('sbReset').addEventListener('click', (e) => { e.stopPropagation();   // …and reset takes the whole name group back, for the same reason a drag moves it
      for (const id in sbCfgs) if (ITEM_NAMES[id] === ITEM_NAMES[sbIt]) Object.assign(sbCfgs[id], SB_DEFS[id]);
      sbSave(); sbRefresh(); });
    // the ESC-menu button was removed (user 2026-08-05); the panel and every slider below stay wired, so
    // __vb.pick() and a re-added button both still work. Guarded because the element is no longer there.
    { const pb = $('pkBtn'); if (pb) pb.addEventListener('click', (e) => { e.stopPropagation(); pkShow(true); }); }
    $('pkClose').addEventListener('click', (e) => { e.stopPropagation(); pkShow(false); });
    // ── L OPENS AND CLOSES IT (user 2026-08-17) ── bound HERE and not in ui/input.js because pkPanel and
    // pkRefresh are declared in this fragment, which the manifest places BELOW input.js: reaching them
    // from there is a const-before-declaration, the black-screen failure this codebase is prone to.
    // L was FREE — the vignette that the stale comment in ui/settings.js still credits to it was on K and
    // was unbound on 2026-08-06 (see the migration in ui/keybinds.js), so nothing had to be taken away.
    // Guarded the same way T is: not while the command line owns the keyboard, and not in the editor.
    // ── …AND IT HANDS OVER THE CURSOR (user 2026-08-17: "when the player presses l, have it free the cursor
    // so I can adjust the settings") ── the panel opened under POINTER LOCK, so the mouse was still driving the
    // camera and the sliders could not be dragged at all. setLightMode (ui/input.js) is the mechanism that
    // already exists for exactly this and is reused rather than re-derived: it drops pointer lock, keeps the
    // ESC MENU from surfacing over the released cursor — which is the part that is easy to miss, and is why
    // a bare exitPointerLock() here would have put the pause menu on top of the panel — and syncs the custom
    // free-mouse cursor. Its own listener was retired in 2026-08-05 and the light panel it was named for is
    // display:none, so nothing else is riding it. Closing hands the pointer straight back.
    const pkShow = (on) => { pkPanel.classList.toggle('hidden', !on); setLightMode(on); if (on) { pkRefresh(); sbRefresh(); } };
    document.addEventListener('keydown', (e) => {
      if (e.code !== 'KeyL' || CMD.open || ED.on || e.repeat) return;
      // …and NOT while a control in the panel has the keyboard (user 2026-08-18). Click a slider, then press L —
      // an arrow-key nudge is the precise way to use these, and 'l' is one keystroke from the arrow cluster — and the
      // panel you are tuning vanished. ui/input.js:47 already guards its own keys this way; this one never did.
      const ae = document.activeElement; if (ae && /^(INPUT|TEXTAREA|SELECT)$/.test(ae.tagName)) return;
      e.preventDefault();
      pkShow(pkPanel.classList.contains('hidden'));   // rebinds the sliders to whatever is in the hand right now
    });
    $('pkReset').addEventListener('click', (e) => { e.stopPropagation();   // …and reset takes the whole strip back too, for the same reason a drag moves it
      for (const id in pickCfgs) if (ITEM_NAMES[id] === ITEM_NAMES[pkIt]) Object.assign(pickCfgs[id], PICK_DEFS[id]);
      pickSave(); pkRefresh(); });
    pkPanel.addEventListener('click', (e) => { e.stopPropagation();
      if (e.target === pkPanel) pkShow(false); });   // backdrop click closes it too, matching the settings panel
  }

