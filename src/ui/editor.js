  // @module — the asset editor: platform, gizmos, .vox parse, pose bakes
  // @exports BUNNY_JUMP_BAKE, BUNNY_ROT_BAKE, BUNNY_ROT_BAKE_R, ED_LANE_RUN, edExStep, edApplyRot, edCol, edCopyOffsets, edEnsureGizCols, edEnsureRgizCols, edEnter, edExit, edExportSeq, edHudUpd, edImportBufs, edBorrowN, edLayout, edLoadVox, edLoadVox2, edMixPick, edMoveStep, edOffset, edParseVox, edRotVox, edRotate, edSaveOffsets, edSelStep, edSeqsAt, edSnapCount, edSnapErrs, edSwapBunnies, stampArmadillo, stampBunny, stampPorcupine, stampSkunk
  // ── ASSET EDITOR PLATFORM ── a floating white stage (1-voxel-thick plane, 1 m grid in light grey) stamped just above
  // the tallest content near the player. Import .vox frames (multi-file or multi-model), they line up left→right in
  // sequence order; , / . cycle the selected frame (amber ring), ←/→ move it within the sequence. Every voxel write goes
  // through edSet which records the ORIGINAL world content once — exiting restores the world exactly and returns the player.
  const edHudEl = $('edHud'), edRowEl = $('edRow'), edBtnEl = $('edBtn'), edFileEl = $('edFile');   // ED state itself is declared up by the player block
  const edIdx = (x, y, z) => gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
  const edSet = (x, y, z, id, cells) => { const ii = edIdx(x, y, z);
    if (!ED.prev.has(ii)) ED.prev.set(ii, W[ii]);
    if (W[ii] !== id) { W[ii] = id; cells.push(ii); }
    return ii; };
  const edPlaneId = (x, z) => ((x - ED.x0) % 10 === 0 || (z - ED.z0) % 10 === 0) ? ED_GREY : ED_WHITE;   // 1 m squares = every 10th voxel is a grey gridline
  // ── WHERE THE SIDE LANE STANDS ── three numbers, all measured against the camera edEnter sets up (50 voxels
  // back from the stage centre, aimed at the middle), because that is the shot the editor opens on.
  // ACROSS: −44, four gridlines and a bit clear of the subject — far enough to read as a second stand rather
  // than a model that has drifted, near enough to stay in frame. It is NEGATIVE for a reason worth stating,
  // since either sign looks equally arbitrary in the source: POSITIVE puts the exhibit directly BEHIND THE
  // HELD-ITEM VIEWMODEL, which occupies the lower right of every frame — screenshotted, the frog on that side
  // spent its whole run peeking out from around the pick handle. The left half of the shot is empty.
  // UP-STAGE: +50, which is exactly the camera's own distance from the stage centre. A sequence that TRAVELS
  // (the croak-and-leap carries −10 in z a cycle) otherwise starts level with the middle and spends the back
  // half of its run BEHIND the viewer — measured, it left the frame three leaps in and did not come back until
  // the run restarted. Starting it a camera-distance up-stage puts the whole run in front of you: it hops down
  // the side of the stage toward you, and begins again at the far end.
  // RUNWAY: 50 voxels, five leaps, and NOT the whole 105 of stage the centre lane gets (tick-support.js bounds
  // that one by the stage). The three are solved together, against that shot and the WORST aspect ratio a
  // player is likely to have:
  //     FOV 72 deg (ui/hud.js) -> tan 36 = 0.727 of the distance in visible half-height,
  //     x 16:9 = 1.29*d of visible half-WIDTH; anything wider only helps.
  //     closest approach  d = back(50) + ED_LANE_BACK(50) - ED_LANE_RUN(50) = 50  ->  half-width 64
  //     the model needs  |ED_LANE| + its own half-width (~5) = 49  <  64, a quarter of the frame in hand.
  // Checked the other way too: at the START of the run (d = 100) the lane sits at 0.34 of the half-width, well
  // inside the frame. −48 across with a 60 runway was tried first and rejected — that closes to d = 40, i.e. 51
  // of half-width against a 53 requirement, and the frog was half off the edge on its last hop. What showed it
  // was a screenshot at the closest hop on a 16:9 window, not the arithmetic; the arithmetic came after.
  // ED_LANE_RUN is the one of the three that leaves this module (@exports, top of file): the march it bounds is
  // counted in main/tick-support.js, while where the lane STANDS is decided here. A fragment is its own scope —
  // reading it over there without exporting it throws inside tickBody, which freezes the sim behind a perfectly
  // rendered frame and says nothing (only __vb.errLog() knows). It cost a debugging round once already.
  // A FLYER ORBITS, so it needs its own, tighter offset: measured at the -44 lane with a 30 orbit it swung
  // between 14 and 74 voxels off centre, and the far half of that is outside the ~51 the shot holds at that
  // depth, so it spent much of its run off the left edge. -26 with the 20 orbit below keeps it between 6 and 46.
  const ED_LANE = -44, ED_FLY_LANE = -26, ED_LANE_BACK = 50, ED_LANE_RUN = 50;
  const edHudUpd = () => { edHudEl.textContent = ED.frames.length
    ? 'frame ' + ED.sel + '/' + (ED.frames.length - 1)                 // just the frame counter — no vox filename, no keybind hints (user)
    : 'asset editor — press ESC, then import a .vox to begin'; };
  let gizCol = null;                                   // [e] MOVE-GIZMO (Task 1): 3 stubby arrows (X red / Y green-up / Z blue) you aim at + drag to nudge the frame into alignment
  const edEnsureGizCols = () => { if (!gizCol) { gizCol = [edCol(240, 60, 55), edCol(70, 210, 70), edCol(80, 130, 255)]; palSync(); } };
  const GIZ_LEN = 13, GIZ_GAP = 2, GIZ_HEAD = 3;
  const edGizmos = (f, bx2, bz2, cells) => {           // stamp the 3 arrows out of the frame's + faces and record their pick-AABBs
    edEnsureGizCols(); ED.gizBoxes = [];
    const SX = f.sxR || f.sx, SY = f.syR || f.sy;        // rotated footprint (marcher spin) so the arrows centre on the spun model, not its base orientation
    const ax0 = bx2, ay0 = ED.y + 1 + (f.oy || 0), az0 = bz2;
    const cx = ax0 + (SX >> 1), cy = ay0 + (f.sz >> 1), cz = az0 + (SY >> 1);
    const put = (x, y, z, id) => { if (x < ED.x0 || x >= ED.x0 + ED.pw || z < ED.z0 || z >= ED.z0 + ED.pd || y <= ED.y || y >= WY - 1) return; ED.fcells.push(edSet(x, y, z, id, cells)); };
    const arrow = (axis) => {                           // axis 0=X(+x) 1=Y(+y up) 2=Z(+z)
      const s = axis === 0 ? ax0 + SX + GIZ_GAP : axis === 1 ? ay0 + f.sz + GIZ_GAP : az0 + SY + GIZ_GAP;
      const id = gizCol[axis];
      for (let i = 0; i < GIZ_LEN; i++) {               // thin 1-voxel shaft; a pyramidal arrowhead over the last GIZ_HEAD voxels (widest at the head's base, a point at the tip)
        const tipDist = GIZ_LEN - 1 - i, r = tipDist < GIZ_HEAD ? tipDist : 0;
        for (let a = -r; a <= r; a++) for (let b = -r; b <= r; b++) {
          if (Math.abs(a) + Math.abs(b) > r) continue;   // DIAMOND head — corners of each square slice cut out (user)
          if (axis === 0) put(s + i, cy + a, cz + b, id); else if (axis === 1) put(cx + a, s + i, cz + b, id); else put(cx + a, cy + b, s + i, id);
        }
      }
      const lo = s - 1, hi = s + GIZ_LEN + 1, t = GIZ_HEAD;   // pick-AABB: full length along the axis, ±(head) thick on the other two
      ED.gizBoxes.push({ axis, min: axis === 0 ? [lo, cy - t, cz - t] : axis === 1 ? [cx - t, lo, cz - t] : [cx - t, cy - t, lo],
                                max: axis === 0 ? [hi, cy + t, cz + t] : axis === 1 ? [cx + t, hi, cz + t] : [cx + t, cy + t, hi] });
    };
    arrow(0); arrow(1); arrow(2);
  };
  let rgizCol = null;
  const edEnsureRgizCols = () => { if (!rgizCol) { rgizCol = [edCol(255, 190, 40), edCol(180, 90, 255)]; palSync(); } };   // yaw ring = amber, pitch ring = violet
  const edRotGizmos = (f, bx2, bz2, cells) => {         // ROTATE gizmo: 2 circular voxel rings around the model — a FLAT ring (yaw / horizontal spin) + an UPRIGHT ring (pitch / vertical tumble)
    edEnsureRgizCols(); ED.rgizBoxes = [];
    const oy = f.oy || 0, SX = f.sxR || f.sx, SY = f.syR || f.sy;   // rotated footprint (marcher spin) so the rings sit around the spun model
    const cx = bx2 + (SX >> 1), cy = ED.y + 1 + oy + (f.sz >> 1), cz = bz2 + (SY >> 1);   // model centre (world voxels)
    const Rr = (Math.max(SX, SY, f.sz) >> 1) + 4;   // ring radius — just outside the model
    const put = (x, y, z, id) => { if (x < ED.x0 || x >= ED.x0 + ED.pw || z < ED.z0 || z >= ED.z0 + ED.pd || y <= ED.y || y >= WY - 1) return; ED.fcells.push(edSet(x, y, z, id, cells)); };
    const seen = new Set();
    const ringPt = (x, y, z, id) => { const k = x + ',' + y + ',' + z; if (seen.has(k)) return; seen.add(k); put(x, y, z, id); };
    const STEPS = 96;
    for (let i = 0; i < STEPS; i++) { const a = i / STEPS * 6.28318, c = Math.round(Math.cos(a) * Rr), s = Math.round(Math.sin(a) * Rr);
      ringPt(cx + c, cy, cz + s, rgizCol[0]);           // YAW ring — flat (world XZ) around the waist → drag to spin about vertical
      ringPt(cx, cy + c, cz + s, rgizCol[1]); }         // PITCH ring — upright (world YZ, ⟂ X) → drag to tumble about the horizontal X axis
    const t = 3;
    ED.rgizBoxes.push({ kind: 'yaw',   min: [cx - Rr - 1, cy - t, cz - Rr - 1], max: [cx + Rr + 1, cy + t, cz + Rr + 1] });   // flat pick-slab, thin in Y
    ED.rgizBoxes.push({ kind: 'pitch', min: [cx - t, cy - Rr - 1, cz - Rr - 1], max: [cx + t, cy + Rr + 1, cz + Rr + 1] });   // upright pick-slab, thin in X
  };
  // ── MOVING A FRAME CARRIES EVERY FRAME AFTER IT (user 2026-08-21: "if I move a frame (for example frame 5) I
  // want all of the frames that come after it to be moved to the current frames position") ── aligning an
  // imported sequence used to be N separate jobs: each frame is box-centred on its OWN footprint (see edLayout),
  // and a cycle whose model grows or shrinks — the frog hop runs 9x7 to 7x10 — lands every frame somewhere
  // slightly different, so the same nudge had to be repeated frame by frame down the whole strip. Now the tail
  // of the sequence is dragged along with whatever frame you are holding, so walking FORWARD through a cycle
  // costs one correction per frame instead of one per frame per frame after it: set frame 0, step to frame 1
  // (which is already sitting where 0 is), correct only the DIFFERENCE, and so on.
  // It is an ABSOLUTE copy, not a relative ripple, which is what the request says and it matters in one place:
  // going BACK to an earlier frame after later ones were tuned individually overwrites that later tuning rather
  // than shifting it. That is also the only way "moved to the current frame's position" can mean anything exact.
  // Flip ED_FOLLOW to 0 for the old one-frame-at-a-time behaviour, or to 2 for the relative ripple.
  // Every mover goes through here — the [e] gizmo drag (ui/input.js), the arrow nudges and __vb.edOff — so this
  // is the one place it has to be said. Rotation is deliberately NOT carried: [r] is per-frame by nature, and a
  // sequence whose frames are rotated to a common heading is exactly what the bake tables are for.
  const ED_FOLLOW = 1;                                 // 1 = later frames snap to this frame's offset; 2 = they SHIFT by the same delta (relative); 0 = off
  const edOffset = (axis, d) => { const n = ED.frames.length; if (!n) return; const s = ((ED.sel % n) + n) % n; const f = ED.frames[s];   // nudge the selected frame one voxel along an axis + restamp
    f.ox = (f.ox || 0) + (axis === 0 ? d : 0); f.oy = (f.oy || 0) + (axis === 1 ? d : 0); f.oz = (f.oz || 0) + (axis === 2 ? d : 0);
    if (ED_FOLLOW) for (let i = s + 1; i < n; i++) { const g = ED.frames[i];
      if (ED_FOLLOW === 2) { g.ox = (g.ox || 0) + (axis === 0 ? d : 0); g.oy = (g.oy || 0) + (axis === 1 ? d : 0); g.oz = (g.oz || 0) + (axis === 2 ? d : 0); }
      else { g.ox = f.ox; g.oy = f.oy; g.oz = f.oz; } }   // …the tail of the strip comes with it. Only frames AFTER the selected one: everything before it is work you have already signed off.
    edLayout(); };
  const edSaveOffsets = () => { try { const m = {}; for (const f of ED.frames) { const hasRot = f.rot && f.rot.length; if (f.ox || f.oy || f.oz || hasRot) m[f.name] = hasRot ? [f.ox || 0, f.oy || 0, f.oz || 0, f.rot.slice()] : [f.ox, f.oy, f.oz]; } localStorage.setItem(ED.offKey || 'vb_edoffsets', JSON.stringify(m)); } catch (e) {} };   // AUTOSAVE on release → the CURRENT variant's offset namespace (ED.offKey); 4th element = the rotation step list
  const edFrameOffs = (frames) => frames.map((f, i) => ({ frame: i, name: f.name, ox: f.ox || 0, oy: f.oy || 0, oz: f.oz || 0, rot: (f.rot || []).slice() }));   // rot = ordered 90° steps ('y+'/'y-'/'p+'/'p-') so the export carries rotation too
  const edCopyOffsets = () => {                          // export BOTH lanes at once (user) — labelled by lane so I can bake each variant's positions
    // The label carries the ANIMATION as well as the model. Two lanes holding two animations of the SAME model
    // — which is exactly what one scene-graph .vox staged twice is — would otherwise both key on 'frog' and the
    // second would overwrite the first in this object, silently exporting one lane's offsets as both. The
    // suffixed form also matches the saved-offset namespace ('vb_edoffsets_frog_ribbet+hop'), so what comes out
    // of the clipboard names the same thing the bake table is called.
    const lbl = (nm, sq, dflt) => (nm || dflt) + (sq && sq !== nm ? '_' + sq : '');   // …but not when the animation is named after the model it is the only one of: 'ladybug', never 'ladybug_ladybug'
    const out = { [lbl(ED.name1, ED.seq1, 'left')]: edFrameOffs(ED.frames) };
    if (ED.frames2.length) out[lbl(ED.name2, ED.seq2, 'right')] = edFrameOffs(ED.frames2);
    const txt = JSON.stringify(out); try { navigator.clipboard.writeText(txt); } catch (e) {} return txt; };
  const edRotVox = (vox, sx, sy, q) => {               // rotate a frame's voxels q×90° about the vertical axis (non-destructive display rotation); dims swap on odd turns
    q = ((q % 4) + 4) % 4;
    let cur = vox, cx = sx, cy = sy;
    for (let r = 0; r < q; r++) { const out = []; for (const p of cur) { const x = p & 255, y = (p >> 8) & 255; out.push(y | ((cx - 1 - x) << 8) | (p & 0xffff0000)); } cur = out; const t = cx; cx = cy; cy = t; }
    return { vox: cur, sx: cx, sy: cy };
  };
  const edClearStamp = () => {                          // restore any grid-stamped model/ring voxels to plane/air WITHOUT re-stamping — used before a fresh .vox import lays out
    if (!ED.fcells.length && !ED.fcells2.length && !ED.ring.length) return;
    const cells = [];
    for (const ii of ED.fcells) { if (W[ii] !== 0) { W[ii] = 0; cells.push(ii); } }   // vacated cells → EMPTY (not ED.prev) so a tall-tree voxel the model overlapped is never revealed; ED.prev still drives edExit's rebuild
    ED.fcells = [];
    for (const ii of ED.fcells2) { if (W[ii] !== 0) { W[ii] = 0; cells.push(ii); } }   // preview lane too
    ED.fcells2 = [];
    for (const r of ED.ring) { const id = edPlaneId(r.x, r.z); if (W[r.ii] !== id) { W[r.ii] = id; cells.push(r.ii); } }
    ED.ring = [];
    if (cells.length) gpuPatch(cells, true, cells.length, false); };   // track=false: the editor stage is a frozen sky set, not terrain — the support resolver must never adjudicate it
  const edLayout = () => {                             // stamp the current frame of BOTH bunnies as real static objects (world voxels → full lighting): the EDITABLE bunny in the LEFT lane (with gizmos/ring), the PREVIEW bunny in the RIGHT lane. The animation swaps to the next static frame in place; no filmstrip.
    const cells = [];
    for (const ii of ED.fcells) { if (W[ii] !== 0) { W[ii] = 0; cells.push(ii); } }   // vacated model cells clear to EMPTY (not ED.prev) — the play space above the plane must stay empty, else a cell that overlapped a tall-tree voxel would restore & REVEAL that voxel (a "tree piece" left behind by the hopping bunny). ED.prev is untouched → edExit still rebuilds the real world.
    ED.fcells = [];
    for (const ii of ED.fcells2) { if (W[ii] !== 0) { W[ii] = 0; cells.push(ii); } }   // …and the preview lane's cells
    ED.fcells2 = [];
    for (const r of ED.ring) { const id = edPlaneId(r.x, r.z); if (W[r.ii] !== id) { W[r.ii] = id; cells.push(r.ii); } }
    ED.ring = [];
    // ── THE PREVIEW LANE STEPS ASIDE; THE EDITABLE ONE KEEPS THE MIDDLE ── both lanes used to move, ±24 about
    // the stage centre, which is right for COMPARING two variants of one model side by side (what it was written
    // for — two bunnies) and wrong the moment one of them is the subject and the other is a side exhibit.
    // edEnter frames the camera on the stage CENTRE, so an editable model pushed off it is the one thing you
    // cannot line the gizmos up against comfortably. The offset is one-sided now: lane 1 stays under the camera,
    // lane 2 alone moves out.
    const wrapIn = (v, lo, range) => lo + (((v - lo) % (range + 1) + (range + 1)) % (range + 1));   // fit a model start into [lo, lo+range] so the whole footprint stays in bounds after a forward-march shift
    const n = ED.frames.length;
    if (n) {                                           // ── EDITABLE bunny — LEFT lane ──
      const f = ED.frames[((ED.sel % n) + n) % n];
      const vsrc0 = (ED.blinkE === 1 && f.voxBlinkL) ? f.voxBlinkL : (ED.blinkE === 2 && f.voxBlinkR) ? f.voxBlinkR : (ED.blink && f.voxBlink) ? f.voxBlink : f.vox;   // eye-blink variant while the blink phase is lit (playing only) — blinkE 1/2 is the ONE-EYE-AT-A-TIME wink, and it falls through to the both-eyes variant for every model that has no per-side pair
      const rv = edRotVox(vsrc0, f.sx, f.sy, ED.paused ? 0 : (-ED.spin & 3));   // no whole-animation spin while editing/paused — pausing ALWAYS parks the model at SOUTH: that's the only heading you align (gizmo offsets are south-space; armOffset derives the rest)
      const bx = ED.x0 + ((ED.pw - rv.sx) >> 1), bz = ED.z0 + ((ED.pd - rv.sy) >> 1);   // CENTRE lane — where edEnter aimed the camera
      f.bx = bx; f.bz = bz;
      let ox = f.ox || 0, oy = f.oy || 0, oz = f.oz || 0;              // per-frame alignment offset (live gizmo edit)
      if (ED.arm && !ED.paused) { const e = armOffset(ED.spin, f.name, f.sx, f.sy, ED.bakes || ARMADILLO_BAKES); ox = e[0] || 0; oy = e[1] || 0; oz = e[2] || 0; }   // WALKING creature → South alignment AUTO-DERIVED onto the current heading (rigid rotation + parity fix, same as the world stamp). Paused = the live gizmo offset so you can adjust — always in SOUTH.
      // ── CONTINUOUS HOP ── while PLAYING, add the accumulated cycle offset (ED.hop*) so this bunny keeps marching forward; wrap it back inside the stage so it never hops off-screen. Editing/paused = no hop (base position, so the gizmos & ring line up).
      const xBase = ED.paused ? (bx + ox) : wrapIn(bx + ox + (ED.hopX || 0), ED.x0, Math.max(1, ED.pw - rv.sx));
      const zBase = ED.paused ? (bz + oz) : wrapIn(bz + oz + (ED.hopZ || 0), ED.z0, Math.max(1, ED.pd - rv.sy));
      const hopY = ED.paused ? 0 : (ED.hopY || 0);
      let hxlo = 1e9, hylo = 1e9, hzlo = 1e9, hxhi = -1e9, hyhi = -1e9, hzhi = -1e9;   // world AABB of the stamped voxels → the SOLID hitbox
      for (const p of rv.vox) {
        const x = xBase + (p & 255), y = ED.y + 1 + oy + hopY + ((p >> 16) & 255), z = zBase + ((p >> 8) & 255);
        if (y <= ED.y || y >= WY - 1 || x < ED.x0 || x >= ED.x0 + ED.pw || z < ED.z0 || z >= ED.z0 + ED.pd) continue;
        if (x < hxlo) hxlo = x; if (x > hxhi) hxhi = x; if (y < hylo) hylo = y; if (y > hyhi) hyhi = y; if (z < hzlo) hzlo = z; if (z > hzhi) hzhi = z;
        ED.fcells.push(edSet(x, y, z, p >>> 24, cells));
      }
      ED.box = hxhi >= hxlo ? { cx: (hxlo + hxhi + 1) / 2, cy: (hylo + hyhi + 1) / 2, cz: (hzlo + hzhi + 1) / 2,
        hx: (hxhi - hxlo + 1) / 2, hy: (hyhi - hylo + 1) / 2, hz: (hzhi - hzlo + 1) / 2 } : null;
      f.sxR = rv.sx; f.syR = rv.sy;                                     // rotated footprint (for the ring)
      const bxr = bx + ox, bzr = bz + oz;
      if (ED.paused) {                                 // amber ring + gizmos only while SELECTED (scrubbing) — on the EDITABLE bunny
        const rx0 = bxr - 2, rx1 = bxr + rv.sx + 1, rz0 = bzr - 2, rz1 = bzr + rv.sy + 1;   // ROTATED footprint (rv.*, not f.*) so the outline follows a whole-model spin (marcher heading) too, not just the base orientation (user)
        for (let x = rx0; x <= rx1; x++) for (let z = rz0; z <= rz1; z++) {
          if (x !== rx0 && x !== rx1 && z !== rz0 && z !== rz1) continue;
          if (x < ED.x0 || x >= ED.x0 + ED.pw || z < ED.z0 || z >= ED.z0 + ED.pd) continue;
          const ii = edSet(x, ED.y, z, ED_HLITE, cells);
          ED.ring.push({ ii, x, z });
        }
        if (ED.giz) edGizmos(f, bxr, bzr, cells);      // [e] move-gizmo
        if (ED.rgiz) edRotGizmos(f, bxr, bzr, cells);  // [r] rotate-gizmo
      } else { ED.gizBoxes = []; ED.rgizBoxes = []; }
    }
    const n2 = ED.frames2.length;
    if (n2) {                                          // ── PREVIEW model — SIDE lane (animates, no gizmos/ring) ──
      // ED.sel2, NOT ED.sel: the two lanes hold animations of DIFFERENT LENGTHS (tongue 24 frames against the
      // croak-and-leap's 31), and indexing the preview off the editable lane's frame number would play only as
      // many frames as the shorter one has. Its clock is in tick-support.js.
      const f2 = ED.frames2[((ED.sel2 % n2) + n2) % n2];
      // The preview blinks on the SAME phase as the editable model, per-side wink included — two frogs on one
      // stage, one of them with its eyes nailed open, reads as a bug.
      const vsrc2 = (ED.blinkE === 1 && f2.voxBlinkL) ? f2.voxBlinkL : (ED.blinkE === 2 && f2.voxBlinkR) ? f2.voxBlinkR : (ED.blink && f2.voxBlink) ? f2.voxBlink : f2.vox;
      const rv2 = edRotVox(vsrc2, f2.sx, f2.sy, ED.flyer2 ? (-ED.spin2 & 3) : 0);   // a FLYER faces where it is going; a stationary exhibit keeps the heading it was authored in
      // The up-stage offset exists to give a MARCHING model its runway in front of the viewer. A flyer does not
      // march, it orbits its own home, so it takes the lane's sideways offset and stands level with the middle.
      const bx2 = ED.x0 + ((ED.pw - rv2.sx) >> 1) + (ED.flyer2 ? ED_FLY_LANE : ED_LANE), bz2 = ED.z0 + ((ED.pd - rv2.sy) >> 1) + (ED.flyer2 ? 0 : ED_LANE_BACK);   // SIDE lane — across, and up-stage only if it travels
      const ox2 = f2.ox || 0, oy2 = f2.oy || 0, oz2 = f2.oz || 0;
      const xB2 = ED.paused ? (bx2 + ox2) : wrapIn(bx2 + ox2 + (ED.hop2X || 0), ED.x0, Math.max(1, ED.pw - rv2.sx));
      const zB2 = ED.paused ? (bz2 + oz2) : wrapIn(bz2 + oz2 + (ED.hop2Z || 0), ED.z0, Math.max(1, ED.pd - rv2.sy));
      const hopY2 = ED.paused ? 0 : (ED.hop2Y || 0);
      let ax = 1e9, ay = 1e9, az = 1e9, aX = -1e9, aY = -1e9, aZ = -1e9;
      for (const p of rv2.vox) {
        const x = xB2 + (p & 255), y = ED.y + 1 + oy2 + hopY2 + ((p >> 16) & 255), z = zB2 + ((p >> 8) & 255);
        if (y <= ED.y || y >= WY - 1 || x < ED.x0 || x >= ED.x0 + ED.pw || z < ED.z0 || z >= ED.z0 + ED.pd) continue;
        if (x < ax) ax = x; if (x > aX) aX = x; if (y < ay) ay = y; if (y > aY) aY = y; if (z < az) az = z; if (z > aZ) aZ = z;
        ED.fcells2.push(edSet(x, y, z, p >>> 24, cells));
      }
      ED.box2 = aX >= ax ? { cx: (ax + aX + 1) / 2, cy: (ay + aY + 1) / 2, cz: (az + aZ + 1) / 2,
        hx: (aX - ax + 1) / 2, hy: (aY - ay + 1) / 2, hz: (aZ - az + 1) / 2 } : null;
    } else ED.box2 = null;
    gpuPatch(cells, true, cells.length, false);        // track=false: the editor's frame-advance stamp on the frozen sky stage is not a terrain edit
    edHudUpd();
  };
  const edSwapBunnies = () => {                         // [b] swap the two bunnies between lanes → the OTHER one becomes editable (gizmos/scrub operate on ED.frames). Swap every per-lane field so the machinery follows.
    if (!ED.frames2.length) return;
    const t = ED.frames; ED.frames = ED.frames2; ED.frames2 = t;
    const tk = ED.offKey; ED.offKey = ED.off2; ED.off2 = tk;
    const tn = ED.name1; ED.name1 = ED.name2; ED.name2 = tn;
    const ts = ED.seq1; ED.seq1 = ED.seq2; ED.seq2 = ts;
    ED.mix = []; ED.mixT0 = 0;                          // the PLAYLIST does not survive a swap: what lands in the editable lane is the one cycle that was live, looping on its own. Aligning frames that swap out from under you is not something the editor should offer, and the swap is how you reach the side lane's frames at all.
    const hx = ED.hopX, hy = ED.hopY, hz = ED.hopZ; ED.hopX = ED.hop2X; ED.hopY = ED.hop2Y; ED.hopZ = ED.hop2Z; ED.hop2X = hx; ED.hop2Y = hy; ED.hop2Z = hz;
    ED.sel = 0; ED.sel2 = 0; ED.paused = false; ED.giz = false; ED.rgiz = false;   // fresh scrub/gizmo state for the newly-editable model — both lanes, since each carries its own frame index
    edLayout(); };
  const edSelStep = (d) => { const n = ED.frames.length; if (!n) return; ED.paused = true; ED.sel = ((ED.sel + d) % n + n) % n; edLayout(); };   // scrubbing pauses the animation
  const edMoveStep = (d, wrap) => { const n = ED.frames.length; if (n < 2) return; ED.paused = true; const s = ((ED.sel % n) + n) % n; let j = s + d;
    if (wrap) j = ((j % n) + n) % n; else if (j < 0 || j >= n) return;
    const t = ED.frames[s]; ED.frames[s] = ED.frames[j]; ED.frames[j] = t; ED.sel = j; edLayout(); };
  const edRotStep = (f, kind, dir) => {                // apply ONE 90° rotation to a frame's display vox + eye-blink variant + raw XYZI bytes + dims. PURE (no ED state, no repaint) — used LIVE by [r] and REPLAYED at load from the baked rot list.
    const osx = f.sx, osy = f.sy, osz = f.sz;
    const fn = kind === 'yaw'
      ? (dir > 0 ? (arr) => arr.map((p) => (((p >> 8) & 255)) | ((osx - 1 - (p & 255)) << 8) | (p & 0xffff0000))      // (x,y)→(y, sx-1-x)
                 : (arr) => arr.map((p) => ((osy - 1 - ((p >> 8) & 255))) | ((p & 255) << 8) | (p & 0xffff0000)))     // (x,y)→(sy-1-y, x)
      : (dir > 0 ? (arr) => arr.map((p) => (p & 0xff0000ff) | ((osz - 1 - ((p >> 16) & 255)) << 8) | (((p >> 8) & 255) << 16))   // (y,z)→(sz-1-z, y)
                 : (arr) => arr.map((p) => (p & 0xff0000ff) | (((p >> 16) & 255) << 8) | ((osy - 1 - ((p >> 8) & 255)) << 16)));  // (y,z)→(z, sy-1-y)
    f.vox = fn(f.vox);
    if (f.voxBlink) f.voxBlink = fn(f.voxBlink);       // keep the eye-blink variant in sync with the rotation
    if (f.raw) { const r = new Uint8Array(f.raw.length);
      if (kind === 'yaw') { for (let i = 0; i < f.raw.length; i += 4) { const x = f.raw[i], y = f.raw[i + 1]; r[i] = dir > 0 ? y : osy - 1 - y; r[i + 1] = dir > 0 ? osx - 1 - x : x; r[i + 2] = f.raw[i + 2]; r[i + 3] = f.raw[i + 3]; } }
      else { for (let i = 0; i < f.raw.length; i += 4) { const y = f.raw[i + 1], z = f.raw[i + 2]; r[i] = f.raw[i]; r[i + 1] = dir > 0 ? osz - 1 - z : z; r[i + 2] = dir > 0 ? y : osy - 1 - y; r[i + 3] = f.raw[i + 3]; } }
      f.raw = r; }
    if (kind === 'yaw') { const t = f.sx; f.sx = f.sy; f.sy = t; } else { const t = f.sy; f.sy = f.sz; f.sz = t; }   // width↔depth (yaw) / depth↔height (pitch)
  };
  const edApplyRot = (kind, dir) => {                  // [r] gizmo: rotate the selected frame 90° AND record the step so export/bake/save can replay it at load (rotation now lives in the offset data, not only the .vox)
    const n = ED.frames.length; if (!n) return; ED.paused = true;
    const f = ED.frames[((ED.sel % n) + n) % n];
    edRotStep(f, kind, dir);
    (f.rot = f.rot || []).push(kind[0] + (dir > 0 ? '+' : '-'));   // 'y+' 'y-' 'p+' 'p-' — the ordered step list; replaying it reproduces the exact orientation
    edSaveOffsets(); edLayout(); };                    // autosave (incl. rot) like the move-gizmo does
  const edRotate = () => edApplyRot('yaw', 1);         // legacy tap: one 90° yaw step
  const edSnapErr = [];                                // per-substitution max-channel error
  let edSnaps = 0;                                     // colours the FULL table substituted for imported/creature art — __vb.palAudit() reports it
  const edSnapCount = () => edSnaps;
  const edSnapErrs = () => edSnapErr.slice();          // getter for the same reason edSnapCount is one                   // a GETTER, not the number: the module wrapper returns its exports by value, so exporting the counter itself would hand every reader a frozen 0
  const edColCache = new Map();                        // runtime color → palette id; nearest-match once the 256-entry palette is full
  const edCol = (r, g, b) => { const key = (r << 16) | (g << 8) | b;
    let cid = edColCache.get(key);
    if (cid === undefined) {
      // REUSE AN EXACT MATCH FIRST. Imported art kept minting fresh entries for colours the palette already
      // held — 30 different shades sat on two, three, even four ids apiece, which is most of why the table
      // filled. addCol is deliberately left alone: ids like BROCK repeat ROCK's greys ON PURPOSE, so that
      // right-click pickup can flood a boulder without eating the terrain around it.
      for (let i = 1; i < palette.length && cid === undefined; i++) { const q = palette[i];
        if (q[0] === r && q[1] === g && q[2] === b) cid = i; }
      if (cid !== undefined) { /* found one already in the table */ }
      else if (PAL_TOL > 0 && (cid = edNearShareOK(r, g, b)) !== undefined) { /* ── TOLERANCE REUSE ── the same lever palShare got, on the path that actually fills the table. edCol mints creature colours LAZILY, the first time a species is stamped or ragdolled, and it had exact-match-or-mint only: a shade one unit off an existing one cost a whole id, and the ids ran out mid-session. Floored at DECOR_MIN and skipping palOwn for the reasons palNearShare documents. */ }
      else if (palette.length < 256) cid = addCol(r, g, b);
      // ── A SUBSTITUTE MUST NOT CARRY BEHAVIOUR (user 2026-08-18: "I got killed by a cactus in the cherry boime")
      // ── and that is exactly what this line used to do. A palette id is not a colour, it is a MATERIAL: cactusTab,
      // solidTab, foliaTab, the pickup sets and the support classes are all keyed by it. So when the table is full
      // and a creature colour is snapped to its nearest neighbour, the creature inherits whatever that neighbour
      // MEANS. Measured: the pink bird's plumage snapped onto the cactus FLOWER's pinks — ids 108/109/112, which
      // carry cactusTab — and every grid-stamped pink bird became a cactus. Walking into one in the cherry forest
      // reported "the cactus spines got you". Nothing about the colour was visibly wrong; the damage was.
      // edSubstOK is the same nearest-colour walk with the harmful materials removed from the candidate set, so a
      // substitution can still be the wrong SHADE — that is the documented price of a full table — but it can no
      // longer be the wrong THING. Preferred over freeing ids because it fixes the class rather than this instance:
      // the next creature added to a full palette would have hit it too.
      else { cid = edSubstOK(r, g, b); edSnaps++;
        const q = palette[cid]; edSnapErr.push(Math.max(Math.abs(q[0] - r), Math.abs(q[1] - g), Math.abs(q[2] - b))); }   // HOW WRONG the substitute is. This is the number the tolerance share is measured against: a reuse is bounded by PAL_TOL, a substitution is bounded by nothing   // ── COUNTED (2026-08-15) ── this path is where the palette ACTUALLY runs out: creature colours arrive lazily, the first time a species is stamped or ragdolled, so the table fills during PLAY and every colour past 256 is silently substituted. It reported nothing at all before, which is why the ceiling read as "exactly full, nothing turned away". __vb.palAudit().edSnaps                  // was a THIRD copy of the nearest-colour walk, and the only one that did not skip RESERVED ids — so a full palette could snap imported art onto a pinecone id and every stamp of it would right-click up as a pinecone. palNearest (assets/palette.js) is the one addCol and palShare use.
      edColCache.set(key, cid);
    }
    return cid; };
  // ── THE STAGE SHOWS THE ART, NOT THE NEAREST THING THE WORLD PALETTE CAN SPARE ──
  // edCol above is the right allocator for a creature that has to live in the world: it reuses, shares within
  // PAL_TOL, and once the 256-entry table is full it SUBSTITUTES the nearest legal colour. Measured on frog.vox's
  // hop: 12 of its 16 colours were substituted, off by as much as 73/255, and the frog's darkest green landed on
  // WATER_B — which is what the user saw ("looks like water voxels on the legs"). The material exclusion has been
  // widened to cover water (assets/palette.js), but that only stops the WRONG THING; it cannot stop the wrong
  // SHADE, because the table has no room and the greens nearest a frog belong to foliage, which is excluded.
  // So the editor stops asking the world palette for a colour it does not have, and BORROWS one instead. Entering
  // the editor already hides the world (edEnter zeroes occupancy; edExit rebuilds it) and already borrows the
  // world's VOXELS under a promise to put them back (ED.prev), so borrowing its palette entries under the same
  // promise costs nothing visible: while the stage is up, nothing wearing a borrowed id is on screen. The held
  // viewmodel is unaffected — item models carry their own raw .vox RGB and never index this table.
  // The id borrowed is the one whose colour is CLOSEST to the one asked for, so if a borrow ever has to be given
  // back mid-import the model degrades to what it looks like today rather than to noise.
  const edBorrow = new Map();                          // borrowed palette id → the [r, g, b] it held before the editor took it
  const edExactCache = new Map();                      // source colour → id for the CURRENT import; deliberately NOT edColCache, which outlives the editor and would hand a borrowed id to the world long after it was given back
  const edPalPinned = () => { const p2 = new Set([ED_WHITE, ED_GREY, ED_HLITE]);   // the stage's own colours are on screen the whole time — never borrow the floor out from under the model
    if (gizCol) for (const c of gizCol) p2.add(c);
    if (rgizCol) for (const c of rgizCol) p2.add(c);
    return p2; };
  const edPalRestore = () => { if (!edBorrow.size) { edExactCache.clear(); return 0; }
    const n = edBorrow.size;
    for (const [id, c] of edBorrow) palette[id] = c;
    edBorrow.clear(); edExactCache.clear(); palSync();
    return n; };
  const edBorrowN = () => edBorrow.size;               // a GETTER for the same reason edSnapCount is one
  const edColExact = (r, g, b) => {
    const key = (r << 16) | (g << 8) | b;
    const hit = edExactCache.get(key);
    if (hit !== undefined) return hit;
    for (let i = 1; i < palette.length; i++) { const c = palette[i];
      if (c && c[0] === r && c[1] === g && c[2] === b && !edBorrow.has(i)) { edExactCache.set(key, i); return i; } }   // the table already holds this exact colour → use it and borrow nothing (the frog's orange, its black and its red all land here)
    const pin = edPalPinned();
    let bd = 1e9, best = -1;
    for (let i = 1; i < palette.length; i++) { const c = palette[i];
      if (!c || edBorrow.has(i) || pin.has(i) || palOwn.has(i) || edMatBad(i)) continue;   // the same exclusions the substitute walk uses — a borrowed id keeps its MATERIAL flags, so it has to be an inert one
      const d = (c[0] - r) * (c[0] - r) + (c[1] - g) * (c[1] - g) + (c[2] - b) * (c[2] - b);
      if (d < bd) { bd = d; best = i; } }
    if (best < 0) { const sub = edCol(r, g, b); edExactCache.set(key, sub); return sub; }   // nothing left to borrow (an import with more colours than the table has inert ids) → the old substitute, so a huge file still loads
    edBorrow.set(best, palette[best]);
    palette[best] = [r, g, b];
    edExactCache.set(key, best);
    return best; };
  const edVoxSeqs = (pv) => {                          // the NAMED animations inside ONE .vox → [{ name, ids: [model index, …] }], each id list already in frame order
    // A .vox that holds several animations keeps them in its SCENE GRAPH, not in separate files. frog.vox is one
    // MAIN with 34 SIZE/XYZI pairs and an nTRN/nGRP/nSHP tree that says which of them are 'ribbet' (14 frames),
    // 'tongue' (24) and 'hop' (17). edParseVox below walks the SIZE/XYZI pairs alone — which is right for the
    // per-frame files every creature ships as, and wrong here: it hands back all three cycles concatenated in
    // file order, each model appearing once however many times its animation actually plays it. So read the graph.
    // Two things about the format decide the shape of this:
    //   * the FRAME LIST lives on the nSHP — one entry per frame, '_f' the frame index, repeats included, because
    //     'ribbet' genuinely plays model 1 twice. Sorting on '_f' rather than trusting file order is free.
    //   * the NAME lives on the nTRN ABOVE the group, and MagicaVoxel nests a second nTRN called 'frames' inside
    //     every animation. Taking the OUTERMOST name is what makes this return ribbet/tongue/hop instead of three
    //     sequences all called 'frames'.
    // Returns [] for a file with no scene graph (every single-model creature frame), which is what keeps this
    // invisible to the pose builders: they never pass a sequence name, so nothing below even calls it.
    const dvv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    const nodes = new Map();
    const rdStr = (o) => { const n = dvv.getInt32(o, true); let t = '';
      for (let i = 0; i < n; i++) t += String.fromCharCode(pv[o + 4 + i]);
      return [t, o + 4 + n]; };
    const rdDict = (o) => { const n = dvv.getInt32(o, true); o += 4; const d = {};
      for (let i = 0; i < n; i++) { const k = rdStr(o); const v = rdStr(k[1]); d[k[0]] = v[0]; o = v[1]; }
      return [d, o]; };
    const walk = (off, end) => { while (off + 12 <= end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = dvv.getUint32(off + 4, true), csz = dvv.getUint32(off + 8, true);
      if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      if (id === 'nTRN' || id === 'nGRP' || id === 'nSHP') {
        let o = off + 12; const nid = dvv.getInt32(o, true); o += 4;
        const at = rdDict(o); o = at[1];
        const rec = { t: id, name: at[0]._name || '' };
        if (id === 'nTRN') rec.child = dvv.getInt32(o, true);   // …then reserved / layer / numFrames and the per-frame transform dicts, none of which this needs: the chunk header already says where the next chunk starts
        else if (id === 'nGRP') { const nc = dvv.getInt32(o, true); o += 4; rec.kids = [];
          for (let i = 0; i < nc; i++) { rec.kids.push(dvv.getInt32(o, true)); o += 4; } }
        else { const nm = dvv.getInt32(o, true); o += 4; rec.models = [];
          for (let i = 0; i < nm; i++) { const mi = dvv.getInt32(o, true); o += 4; const md = rdDict(o); o = md[1];
            rec.models.push([mi, md[0]._f === undefined ? i : +md[0]._f]); } }
        nodes.set(nid, rec); }
      off += 12 + bsz + csz;
    } };
    try { walk(8, pv.length); } catch (e) { return []; }
    if (!nodes.size) return [];
    const gather = (nid, ids, seen) => { const r = nodes.get(nid); if (!r || seen.has(nid)) return; seen.add(nid);
      if (r.t === 'nTRN') gather(r.child, ids, seen);
      else if (r.t === 'nGRP') { for (const k of r.kids) gather(k, ids, seen); }
      else for (const m of r.models.slice().sort((a, b) => a[1] - b[1])) ids.push(m[0]); };
    const out = [];
    const scan = (nid, seen) => { const r = nodes.get(nid); if (!r || seen.has(nid)) return; seen.add(nid);
      if (r.t === 'nTRN' && r.name) { const ids = []; gather(nid, ids, new Set()); if (ids.length) out.push({ name: r.name, ids }); return; }   // OUTERMOST name wins → never descend into a named animation looking for more
      if (r.t === 'nTRN') scan(r.child, seen); else if (r.t === 'nGRP') for (const k of r.kids) scan(k, seen); };
    scan(nodes.has(0) ? 0 : nodes.keys().next().value, new Set());
    return out; };
  const edParseVox = (pv, name, seq, exact) => {              // ALL SIZE/XYZI pairs — a multi-model .vox is an animation, each model one frame — or, with `seq`, just the one NAMED animation out of a scene-graph file (see edVoxSeqs)
    const pdv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    const models = []; const ppal = new Uint8Array(1024); let hasPal = false;
    const walk = (off, end) => { while (off + 12 <= end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
      if (id === 'SIZE') models.push({ sx: pdv.getUint32(off + 12, true), sy: pdv.getUint32(off + 16, true), sz: pdv.getUint32(off + 20, true), raw: null });
      else if (id === 'XYZI') { const m = models.find((mm) => !mm.raw); if (m) { const n = pdv.getUint32(off + 12, true); m.raw = pv.subarray(off + 16, off + 16 + n * 4); } }
      else if (id === 'RGBA') { ppal.set(pv.subarray(off + 12, off + 12 + 1024)); hasPal = true; }
      else if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      off += 12 + bsz + csz;
    } };
    walk(8, pv.length);
    const out = [];
    let pick = null;
    if (seq) { const qs = edVoxSeqs(pv), q = qs.find((t) => t.name.toLowerCase() === String(seq).toLowerCase());
      if (q) pick = q.ids;                             // that animation's models, in ITS frame order, repeats and all
      else console.warn('[vb] editor: ' + name + ' has no animation named ' + seq + (qs.length ? ' — it holds ' + qs.map((t) => t.name).join(', ') : ' (no scene graph)') + '; loading every model'); }
    const order = pick || models.map((m, k) => k);     // no sequence asked for (every creature's per-frame files, every pose builder) → exactly the old behaviour, model order untouched
    order.forEach((mi, k) => { const m = models[mi]; if (!m || !m.raw) return;
      const cmap = new Map(), mvox = [];
      for (let i = 0; i < m.raw.length; i += 4) {
        const ci = m.raw[i + 3];
        let cid = cmap.get(ci);
        if (cid === undefined) { const cr = ppal[(ci - 1) * 4], cg = ppal[(ci - 1) * 4 + 1], cb = ppal[(ci - 1) * 4 + 2];
          cid = exact ? edColExact(cr, cg, cb) : edCol(cr, cg, cb); cmap.set(ci, cid); }   // exact = an EDITOR import, which BORROWS a palette entry rather than accepting the nearest colour the world can spare; every world pose builder leaves it unset and keeps edCol
        mvox.push(m.raw[i] | (m.raw[i + 1] << 8) | (m.raw[i + 2] << 16) | (cid << 24));   // model z-up → world y at stamp time
      }
      out.push({ sx: m.sx, sy: m.sy, sz: m.sz, vox: mvox, name: pick ? String(k).padStart(2, '0') + '.vox' : name + (models.length > 1 ? ' #' + (mi + 1) : ''),   // a named sequence numbers its frames 00.vox, 01.vox … — the naming every bake table, the saved-offset namespace and edExportSeq already speak, so alignment work on it saves and exports like a frame folder
        ox: 0, oy: 0, oz: 0,                              // per-frame gizmo offsets (voxels) — nudge a frame into alignment with [e] arrows; persisted + copied for baking (Task 1)
        raw: new Uint8Array(m.raw), pal: hasPal ? new Uint8Array(ppal) : null });   // byte-faithful copies — export rebuilds real .vox files from these, not from engine ids
    });
    return out;
  };
  const edExportSeq = () => {                          // one single-model .vox per frame, numbered in CURRENT sequence order (00.vox, 01.vox, … — the cardinal-flight naming)
    ED.frames.forEach((f, i) => {
      const n = f.raw.length / 4, hasPal = !!f.pal;
      const total = 20 + 24 + (16 + n * 4) + (hasPal ? 12 + 1024 : 0);
      const buf = new Uint8Array(total), dv2 = new DataView(buf.buffer);
      const wid = (off, s) => { for (let k = 0; k < 4; k++) buf[off + k] = s.charCodeAt(k); };
      wid(0, 'VOX '); dv2.setUint32(4, 150, true);
      wid(8, 'MAIN'); dv2.setUint32(12, 0, true); dv2.setUint32(16, total - 20, true);
      let o = 20;
      wid(o, 'SIZE'); dv2.setUint32(o + 4, 12, true); dv2.setUint32(o + 8, 0, true);
      dv2.setUint32(o + 12, f.sx, true); dv2.setUint32(o + 16, f.sy, true); dv2.setUint32(o + 20, f.sz, true); o += 24;
      wid(o, 'XYZI'); dv2.setUint32(o + 4, 4 + n * 4, true); dv2.setUint32(o + 8, 0, true);
      dv2.setUint32(o + 12, n, true); buf.set(f.raw, o + 16); o += 16 + n * 4;
      if (hasPal) { wid(o, 'RGBA'); dv2.setUint32(o + 4, 1024, true); dv2.setUint32(o + 8, 0, true); buf.set(f.pal, o + 12); }
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([buf], { type: 'application/octet-stream' }));
      a.download = String(i).padStart(2, '0') + '.vox';
      setTimeout(() => { a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 4000); }, i * 160);   // staggered — burst clicks can drop downloads
    });
    return ED.frames.length;
  };
  const ARMADILLO_BAKE = { '05.vox': [1, 0, 0], '06.vox': [1, 0, 0], '07.vox': [1, 0, 0] };   // heading 0 (SOUTH) body-centering — user's in-game alignment. STATIC per-frame alignment only; the walk marches nowhere (hop zeroed below).
  const ARMADILLO_BAKES = [ARMADILLO_BAKE, {}, {}, {}];   // [0]=SOUTH is the ONLY authored bake — E/N/W auto-derive in armOffset (rigid rotation + box-parity correction). The old manual N/W tables ("+1x/+1z on frames 01-03") were hand-fixes for the even-sx box-centre bias; the correction computes those exact values, so they're gone. (The supposed "leg-phase flip" was really box parity — frames 01-03 AND 05-07 have even sx; 05-07's nudge cancels against the rotated south bake.)
  const SKUNK_BAKES = [{ '01.vox': [0, 0, 1], '02.vox': [0, 0, 1], '03.vox': [0, 0, 1], '06.vox': [0, 0, 1], '07.vox': [0, 0, 1], '08.vox': [0, 0, 1] }, {}, {}, {}];   // SKUNK alignment (user editor export, SOUTH only): frames 01-03 + 06-08 nudged +1 in oz. Every other heading auto-derives — the user's old EAST export (−1x on exactly 02/07) is what the parity correction computes, verified against the .vox dims (sy even on 01/03/06/08, odd on 02/07).
  // Slots [1..3] are kept only so the bakes array stays shaped for ED.bakes; armOffset reads bakes[0] alone. AUTHOR ALIGNMENT ONCE, IN SOUTH — every heading derives from it.
  const armOffset = (spin, name, sx, sy, bakes = ARMADILLO_BAKES) => {   // heading alignment for a walking creature — FULLY AUTO-DERIVED from the SOUTH bake (the only one you author). E/N/W = the south offset rotated rigidly + a BOX-PARITY correction. The correction is the fix for the old per-heading nudging: edRotVox re-centres each rotated frame on its OWN box with floor division (s>>1), which is biased half a voxel on EVEN dims — rotation turns that bias into per-frame ±1 shifts. This closed form reproduced EVERY historical hand-tuned table exactly (armadillo N/W +1 on its even-sx frames, skunk E −1x on exactly its odd-sy frames 02/07), so manual per-heading bakes are gone.
    spin &= 3;
    const s = bakes[0][name] || [0, 0, 0];               // authored SOUTH offset (per-frame)
    let ox = s[0] || 0, oz = s[2] || 0; const oy = s[1] || 0;
    for (let r = 0; r < spin; r++) { const t = ox; ox = -oz; oz = t; }   // (ox,oz) → (−oz, ox) per 90° step: S(+1,0) → E(0,+1)
    const ex = 1 - (sx & 1), ey = 1 - (sy & 1);          // 1 when the frame's ORIGINAL footprint dim is even → the floor-centre bias flips under that rotation
    if (spin === 1) ox += ey; else if (spin === 2) { ox += ex; oz += ey; } else if (spin === 3) oz += ex;   // parity correction per heading (derived for q=(−spin)&3 edRotVox steps) → box-centre rotation ≡ true rigid rotation
    return [ox, oy, oz];
  };
  const buildArmPoses = () => {                          // build the GRID-STAMP poses ONCE: [frame][heading] → [dx, dy(height), dz, id], centred on the anchor + armOffset — IDENTICAL alignment to the editor stamp (user). Lazy: edParseVox/armOffset are ready by first render.
    if (ARMADILLO_POSES || !ARMADILLO_WALK.length) return;
    const frames = [];
    for (const fr of ARMADILLO_WALK) { try { frames.push(...edParseVox(fr.u8, fr.name)); } catch (e) {} }
    if (!frames.length) return;
    let footz = 255; for (const f of frames) for (const p of f.vox) { const z = (p >> 16) & 255; if (z < footz) footz = z; }   // lowest occupied model-z → feet sit on the ground
    ARMADILLO_FOOTZ = footz === 255 ? 0 : footz;
    const poses = [];
    for (const f of frames) { const perH = [];
      for (let h = 0; h < 4; h++) {                        // heading h → editor rotation q = (−h)&3 (same as edLayout's −ED.spin) + armOffset(h)
        const rv = edRotVox(f.vox, f.sx, f.sy, (-h) & 3);
        const e = armOffset(h, f.name, f.sx, f.sy), ox = e[0] || 0, oy = e[1] || 0, oz = e[2] || 0;
        const cx = rv.sx >> 1, cy = rv.sy >> 1, list = [];   // −cx/−cy centres the footprint on the anchor = editor's ((pw−sx)>>1) box-centre
        for (const p of rv.vox) { const id = p >>> 24; list.push([(p & 255) - cx + ox, ((p >> 16) & 255) + oy, ((p >> 8) & 255) - cy + oz, id]); CREATURE_IDS.add(id); CREA_FLAG[id] = 1; }
        perH.push(list); }
      poses.push(perH); }
    ARMADILLO_POSES = poses;
    palSync();                                             // upload the palette ids edParseVox allocated + the CREA_FLAG bits set above — the same contract every other pose builder keeps (see buffers.js: "every pose builder re-syncs after registering its ids"). Without it a beyond-420-voxel armadillo — which is where the mammals normally spawn — grid-stamped as a solid black silhouette on the wrong hit-flash path until some other builder happened to sync.
  };
  const stampArmadillo = (B, gsurf) => {                  // stamp the armadillo's current frame into W at its position — clear-then-restamp via stampApply (foliage restore), like the perched cardinal
    buildArmPoses(); if (!ARMADILLO_POSES) return;
    const n = ARMADILLO_POSES.length, afi = Math.floor((B.animClk || 0) * 24) % n, h = B.ah & 3;
    const gx = Math.round(B.x), gz = Math.round(B.z), gy = gsurf + 1 - ARMADILLO_FOOTZ;
    stampApply(B, ARMADILLO_POSES[afi][h], gx, gy, gz, gx + ',' + gy + ',' + gz + ',' + afi + ',' + h);
  };
  // ── FLAMINGO alignment is BAKED INTO THE ART, not held here (user 2026-08-18: "make sure the flamingo in
  // the cherry forest matches the one in the asset editor ... when I update the flamingo in the asset editor,
  // do the same for the real world") ── the exported offsets were applied to the .vox frames themselves with
  // tools/bake_offsets.py, so there is nothing left for this table to add.
  // WHY THE ART AND NOT A TABLE, WHEN EVERY OTHER CREATURE HERE USES A TABLE. The others are GRID-STAMPED, so
  // buildArmPoses reads their bake and the world stamps from the result. The flamingo is TRACE-INJECTED: its
  // world frames come from parseBunny reading the raw .vox straight into the item table, which never sees a
  // bake. A table could therefore only ever move the EDITOR's bird, which is exactly the mismatch being fixed.
  // Baked into the art, both paths read the same file and cannot disagree.
  // AN EMPTY OBJECT, NOT A DELETED LINE: the offsets are in the frames now, so re-adding them here would apply
  // them a second time and slide the bird back off centre by the amount that was just corrected.
  const FLAMINGO_BAKE = {};
  const PORCUPINE_BAKES = [{ '01.vox': [1, 0, 0], '02.vox': [1, 0, 0], '03.vox': [1, 1, 0] }, {}, {}, {}];   // PORCUPINE alignment (user editor re-export 2026-07-22, SOUTH only, now a 6-frame model 00-05): frames 01/02 +1x, frame 03 +1x +1y. E/N/W auto-derive via armOffset's parity correction.
  const buildPorcPoses = () => {                          // PORCUPINE grid-stamp poses [frame][heading] — IDENTICAL machinery to buildArmPoses (edParseVox, no blink), but from PORCUPINE_WALK + its own PORCUPINE_BAKES (user's 4th land mammal)
    if (PORCUPINE_POSES || !PORCUPINE_WALK.length) return;
    const frames = [];
    for (const fr of PORCUPINE_WALK) { try { frames.push(...edParseVox(fr.u8, fr.name)); } catch (e) {} }
    if (!frames.length) return;
    let footz = 255; for (const f of frames) for (const p of f.vox) { const z = (p >> 16) & 255; if (z < footz) footz = z; }   // lowest occupied model-z → feet sit on the ground
    PORCUPINE_FOOTZ = footz === 255 ? 0 : footz;
    const poses = [];
    for (const f of frames) { const perH = [];
      for (let h = 0; h < 4; h++) {                        // heading h → editor rotation q = (−h)&3 + armOffset(h, PORCUPINE_BAKES) — parity-corrected
        const rv = edRotVox(f.vox, f.sx, f.sy, (-h) & 3);
        const e = armOffset(h, f.name, f.sx, f.sy, PORCUPINE_BAKES), ox = e[0] || 0, oy = e[1] || 0, oz = e[2] || 0;
        const cx = rv.sx >> 1, cy = rv.sy >> 1, list = [];
        for (const p of rv.vox) { const id = p >>> 24; list.push([(p & 255) - cx + ox, ((p >> 16) & 255) + oy, ((p >> 8) & 255) - cy + oz, id]); CREATURE_IDS.add(id); CREA_FLAG[id] = 1; }
        perH.push(list); }
      poses.push(perH); }
    PORCUPINE_POSES = poses;
    palSync();                                             // upload any palette ids edParseVox allocated for the porcupine
  };
  const stampPorcupine = (B, gsurf) => {                  // stamp the porcupine's current walk frame into W — 12 fps base, 24 fps on flee (user: same fps mechanics as the skunk), from its eased frame-clock B.aframe
    buildPorcPoses(); if (!PORCUPINE_POSES) return;
    const n = PORCUPINE_POSES.length, afi = Math.floor(B.aframe || 0) % n, h = B.ah & 3;
    const gx = Math.round(B.x), gz = Math.round(B.z), gy = gsurf + 1 - PORCUPINE_FOOTZ;
    stampApply(B, PORCUPINE_POSES[afi][h], gx, gy, gz, gx + ',' + gy + ',' + gz + ',' + afi + ',' + h);
  };
  const buildSkunkPoses = () => {                          // SKUNK grid-stamp poses [frame][heading] — built through edBuildFrames (the SAME path the editor stage uses) so the world skunk inherits the editor's eye-BLINK variant + saved offsets, matching it 1:1 (user)
    if (SKUNK_POSES || !SKUNK_WALK.length) return;
    const frames = edBuildFrames(SKUNK_WALK, 'vb_edoffsets_skunk', SKUNK_BAKES[0], 'skunk');   // f.vox + f.voxBlink (eye recoloured to the nearest black) — identical to the editor's frames (armOffset below owns the per-heading alignment, so f.ox/rot are unused here)
    if (!frames.length) return;
    let footz = 255; for (const f of frames) for (const p of f.vox) { const z = (p >> 16) & 255; if (z < footz) footz = z; }   // lowest occupied model-z → feet sit on the ground
    SKUNK_FOOTZ = footz === 255 ? 0 : footz;
    const mk = (blink) => frames.map((f) => { const src = (blink && f.voxBlink) ? f.voxBlink : f.vox, perH = [];
      for (let h = 0; h < 4; h++) {                        // heading h → editor rotation q = (−h)&3 + armOffset(h, SKUNK_BAKES) — parity-corrected, same as the editor
        const rv = edRotVox(src, f.sx, f.sy, (-h) & 3);
        const e = armOffset(h, f.name, f.sx, f.sy, SKUNK_BAKES), ox = e[0] || 0, oy = e[1] || 0, oz = e[2] || 0;
        const cx = rv.sx >> 1, cy = rv.sy >> 1, list = [];
        for (const p of rv.vox) { const id = p >>> 24; list.push([(p & 255) - cx + ox, ((p >> 16) & 255) + oy, ((p >> 8) & 255) - cy + oz, id]); CREATURE_IDS.add(id); CREA_FLAG[id] = 1; }
        perH.push(list); }
      return perH; });
    SKUNK_POSES = mk(false); SKUNK_POSES_B = mk(true);      // normal + eye-blink pose sets (blink recolours the eye, geometry unchanged)
    palSync();                                             // upload any palette ids edBuildFrames allocated for the skunk (the skunk syncs itself so it never renders with a stale palette)
  };
  const stampSkunk = (B, gsurf, now) => {                  // stamp the skunk's current walk frame into W — 6 fps + eye-blink on the editor's cadence, so the world skunk matches the asset-editor skunk (user)
    buildSkunkPoses(); if (!SKUNK_POSES) return;
    const n = SKUNK_POSES.length, afi = Math.floor(B.aframe || 0) % n, h = B.ah & 3;   // SKUNK frame index from its eased frame-clock (B.aframe): 12 fps base, 24 fps while fleeing (user)
    const blink = (now % 3400) < 160;                      // SAME blink cadence as the editor stage (160 ms every 3.4 s)
    const poses = (blink && SKUNK_POSES_B) ? SKUNK_POSES_B : SKUNK_POSES;
    const gx = Math.round(B.x), gz = Math.round(B.z), gy = gsurf + 1 - SKUNK_FOOTZ;
    stampApply(B, poses[afi][h], gx, gy, gz, gx + ',' + gy + ',' + gz + ',' + afi + ',' + h + ',' + (blink ? 1 : 0));   // blink flag in the re-stamp key → the stamp refreshes when the eye opens/closes
  };
  const BUNNY_ROT_BAKE = { '03.vox': [0, 1, 0], '04.vox': [0, 2, 0], '05.vox': [0, 2, 0], '06.vox': [0, 2, 0, ['y-']], '07.vox': [0, 1, 0, ['y-']], '08.vox': [0, 0, 0, ['y-']], '09.vox': [0, 0, 0, ['y-']], '10.vox': [0, 0, 0, ['y-']] };   // BAKED rotate-bunny (user's copy-changes) — hop-bob (oy rise mid-cycle) + a 90° yaw (['y-']) held on the back half of the cycle (frames 6-10), so the bunny turns LEFT as it lands. 4th element = rotation step list replayed onto the .vox at load.
  const BUNNY_ROT_BAKE_R = Object.fromEntries(Object.entries(BUNNY_ROT_BAKE).map(([k, v]) => [k, v[3] ? [v[0], v[1], v[2], v[3].map((s) => s[0] + (s[1] === '-' ? '+' : '-'))] : v.slice()]));   // RIGHT turn = the LEFT rotation exactly, just flipped ('y-'→'y+') (user) — same bob/offsets, mirrored yaw
  const BUNNY_ROT_BOB = Object.fromEntries(Object.entries(BUNNY_ROT_BAKE).map(([k, v]) => [k, [v[0], v[1], v[2]]]));   // ONLY the oy hop-bob, NO baked yaw — the rotation runs through the .vox FRAMES themselves (user does the rotating, not the bake)
  const BUNNY_JUMP_BAKE = { '03.vox': [0, 1, -1], '04.vox': [0, 2, -2], '05.vox': [0, 2, -3], '06.vox': [0, 2, -4], '07.vox': [0, 1, -5], '08.vox': [0, 0, -6], '09.vox': [0, 0, -6], '10.vox': [0, 0, -6] };   // BAKED jump-bunny hop (user's copy-changes) — rises + travels −6 in z over the cycle; last-frame oz drives the continuous forward march
  let BUNNY_JUMP_POSES = null, BUNNY_ROT_POSES = null, BUNNY_FOOTZ = 0;   // GRID-STAMP poses [frame][heading] for jump + rotate models (built lazily) — the world bunny stamps into W like the editor (user)
  const buildBunnyPoses = () => {
    if ((BUNNY_JUMP_POSES && BUNNY_ROT_POSES) || !BUNNY_JUMP.length) return;
    const build = (rawFrames) => {
      const frames = [];
      for (const fr of rawFrames) { try { frames.push(...edParseVox(fr.u8, fr.name)); } catch (e) {} }   // RAW voxels (no edBuildFrames / no off[3] replay) — matches BUNNY_ITEM0's parseBunny; the turn is the HEADING, not baked into the voxels
      if (!frames.length) return null;
      for (const f of frames) for (const p of f.vox) { const z = (p >> 16) & 255; if (z < BUNNY_FOOTZ) BUNNY_FOOTZ = z; }
      return frames.map((f) => { const perH = [];
        for (let h = 0; h < 4; h++) { const rv = edRotVox(f.vox, f.sx, f.sy, (-h) & 3), cx = rv.sx >> 1, cy = rv.sy >> 1, list = [];   // box-centre; NO offset (bunny has no static armOffset — all bakes are motion/bob/rotation)
          for (const p of rv.vox) { const id = p >>> 24; list.push([(p & 255) - cx, (p >> 16) & 255, ((p >> 8) & 255) - cy, id]); CREATURE_IDS.add(id); CREA_FLAG[id] = 1; }
          perH.push(list); }
        return perH; });
    };
    BUNNY_FOOTZ = 255;
    BUNNY_JUMP_POSES = build(BUNNY_JUMP);
    BUNNY_ROT_POSES = build(BUNNY_ROTATE);   // LEFT rotate frames — used for BOTH rotate directions (like the emit reuses BUNNY_ITEM0 for bst 1 and 2)
    if (BUNNY_FOOTZ === 255) BUNNY_FOOTZ = 0;
    palSync();
  };
  const stampBunny = (B, gsurf) => {
    buildBunnyPoses(); if (!BUNNY_JUMP_POSES || !BUNNY_ROT_POSES) return;
    const NFR = 11, fi = Math.min(NFR - 1, Math.floor(B.bfclk || 0)), jump = (B.bst === 0);
    const poses = jump ? BUNNY_JUMP_POSES : BUNNY_ROT_POSES;
    const bake = jump ? BUNNY_JUMP_BAKE : (B.bst === 1 ? BUNNY_ROT_BAKE : BUNNY_ROT_BAKE_R);
    const off = bake[(fi < 10 ? '0' + fi : '' + fi) + '.vox'] || [0, 0, 0];
    const hf = ((!jump && off[3] && off[3].length) ? (B.bst === 1 ? B.bh + 1 : B.bh + 3) : B.bh) & 3;   // per-frame heading = the same mid-sequence yaw snap B.th does (frames 6-10 of a rotate face bh±1)
    const gx = Math.round(B.x), gz = Math.round(B.z), gy = gsurf + 1 - BUNNY_FOOTZ + Math.round(B.bOy || 0);   // feet on the surface + the baked hop-bob
    stampApply(B, poses[fi][hf], gx, gy, gz, gx + ',' + gy + ',' + gz + ',' + (jump ? 'J' : B.bst) + ',' + fi + ',' + hf);
  };
  const edBuildFrames = (list, offKey, bakeOff, name, exact) => {   // parse [{name, u8}] → frames with baked + saved offsets + the eye-blink variant. Shared by BOTH lanes. offKey namespaces the per-frame offsets so each bunny variant keeps its OWN positions though the frames share names 00-10.vox. bakeOff = committed default (undefined → cardinal default; object → that variant's bake; null → none). name = model id (gates the eye-blink colour)
    const frames = [];
    for (const f of list) { try { frames.push(...edParseVox(f.u8, f.name, f.seq, exact)); } catch (e) { console.warn('[vb] editor: bad .vox', f.name, e); } }   // f.seq (optional) = ONE named animation out of a scene-graph .vox; absent for every per-frame file, which is all the pose builders pass
    if (!frames.length) return frames;
    // ── TWO NAMED ANIMATIONS BECOME ONE (user 2026-08-22: "just bake the 2 frames together in one sequence") ──
    // edParseVox numbers the frames of a NAMED animation 00.vox upward, per animation, so concatenating two of
    // them out of the same file hands back 00-13 followed by 00-16: two frames called 00.vox, two called 01.vox
    // and so on. Every downstream lookup is by NAME — the bake table below, the saved-offset namespace,
    // edSaveOffsets, edExportSeq — so a colliding pair does not merely look odd, it makes the second half
    // uneditable and unbakeable, because both halves answer to the same key.
    // Renumber straight through, and do it HERE: before the bake is applied and before localStorage is read,
    // which is the only point at which the names are still nobody's business but this function's.
    // Guarded on more than one entry CARRYING A SEQ. A multi-FILE import (the per-frame .vox folders every
    // creature ships as) also arrives as a list of several, and there the names are the real filenames the
    // whole pipeline speaks — renumbering those would silently detach every pose builder from its bake.
    if (list.filter((f) => f.seq).length > 1) frames.forEach((f, i) => { f.name = String(i).padStart(2, '0') + '.vox'; });
    const bake = bakeOff === undefined ? { '04.vox': [0, 1, 0], '05.vox': [0, 1, 0] } : bakeOff;   // BAKED alignment applied even with no saved edits; cardinal default for manual imports, each bunny passes its own. A 4th element (array) = the baked ROTATION step list.
    if (bake) for (const f of frames) { const b = bake[f.name]; if (b) { f.ox = b[0]; f.oy = b[1]; f.oz = b[2]; if (Array.isArray(b[3])) f.rot = b[3].slice(); } }
    try { const m = JSON.parse(localStorage.getItem(offKey || 'vb_edoffsets') || '{}'); for (const f of frames) { const o = m[f.name]; if (o) { f.ox = o[0] | 0; f.oy = o[1] | 0; f.oz = o[2] | 0; if (Array.isArray(o[3])) f.rot = o[3].slice(); } } } catch (e) {}   // a live saved edit still overrides the bake (offsets AND rotation)
    for (const f of frames) if (f.rot && f.rot.length) for (const s of f.rot) edRotStep(f, s[0] === 'y' ? 'yaw' : 'pitch', s[1] === '+' ? 1 : -1);   // REPLAY the baked/saved rotation onto the loaded .vox — this is where 'rotation in the bake' happens (before the eye-blink variant is built, so it inherits the rotation)
    for (const f of frames) {                            // EYE BLINK: build a variant where each pitch-black eye voxel is recolored to its NEAREST body-colour voxel — red plumage for the cardinal, TAN for the bunny (user). Same cadence/mechanism as the other blinking life forms.
      const reds = [], body = [], tan = [], blacks = [], blackCols = [];
      // ── WHICH VOXEL IS THE EYE ── pitch-black, for every creature authored before the frog. The FROG's eye is
      // the RED voxel that stands proud of the side of its head (user 2026-08-21: "make the frogs red eyes
      // blink"): measured, each eye is a red voxel at the surface with a black one tucked directly behind it,
      // and that black is walled in by green on all five other sides — invisible. So the old detector was
      // faithfully recolouring a voxel nobody can see, which is exactly why the frog never appeared to blink.
      // The hidden black is excluded from the CANDIDATES too: it sits one voxel from the eye and would win the
      // nearest-colour search outright, shutting the eye to black — a hole rather than a closed lid. Skipping it
      // leaves the nearest green (1.41 away) to do the job.
      const EYE_RGB = name === 'frog' ? [221, 59, 39] : null;
      for (let i = 0; i < f.vox.length; i++) { const c = palette[f.vox[i] >>> 24]; if (!c) continue;
        const dark = c[0] < 30 && c[1] < 30 && c[2] < 30;
        if (EYE_RGB ? (c[0] === EYE_RGB[0] && c[1] === EYE_RGB[1] && c[2] === EYE_RGB[2]) : dark) { blacks.push(i); blackCols.push(f.vox[i]); }   // the eye — index (to recolour) + colour (so a black-furred model can blink INTO its own black)
        else if (EYE_RGB && dark) { /* the pupil behind a coloured eye: hidden, and never a lid colour */ }
        else { body.push(f.vox[i]);
          if (c[0] > 130 && c[0] > c[1] * 1.7 && c[0] > c[2] * 1.7) reds.push(f.vox[i]);   // cardinal plumage / the armadillo's saturated orange shell
          else if (c[0] >= c[1] && c[1] >= c[2] && c[0] - c[2] > 12) tan.push(f.vox[i]); } }   // warm & DESCENDING (r≥g≥b) = tan / light-brown; excludes pink (blue elevated → b>g) and greys
      const cands = name === 'skunk' ? (blackCols.length ? blackCols : body)   // SKUNK eyes blink to the nearest BLACK voxel — its black fur, not the tan fallback (user)
                  : (name === 'armadillo' || name === 'porcupine') ? (tan.length ? tan : body)   // armadillo + porcupine eyes blink to the nearest LIGHT-BROWN voxel — not a red shell nor a pink face voxel (user)
                  : (reds.length ? reds : body);           // cardinal → red plumage; bunny (no red) → nearest body voxel = its tan
      const shut = (idxs) => { const vb = f.vox.slice();   // close THESE eye voxels: each takes the colour of the nearest candidate voxel, so the lid reads as the skin around it
        for (const bi of idxs) { const p = f.vox[bi], ex = p & 255, ey = (p >> 8) & 255, ez = (p >> 16) & 255;
          let bd = 1e9; const tied = [];
          for (const r of cands) { const dx = (r & 255) - ex, dy = ((r >> 8) & 255) - ey, dz = ((r >> 16) & 255) - ez, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bd - 1e-6) { bd = d2; tied.length = 0; tied.push(r); } else if (d2 <= bd + 1e-6) tied.push(r); }
          // ── A TIE GOES TO THE TYPICAL COLOUR, NOT TO ARRAY ORDER ── measured on the frog: its eye has FOUR
          // candidates all exactly 1.41 away — three shades of head green and one cream from the chin below —
          // and taking whichever the voxel list happened to reach first shut the eye to CREAM, a pale dot where
          // a lid should be. So among the tied set take the MEDOID: the colour with the least total distance to
          // the others, i.e. the one most typical of the skin around that eye. Three greens outvote one cream.
          // Unique nearest candidates (the usual case, and every creature authored before the frog) have a tied
          // set of one and are untouched by this.
          let best = tied[0];
          if (tied.length > 1) { let bs = 1e18;
            for (const a of tied) { const ca = palette[a >>> 24]; let sum = 0;
              for (const b2 of tied) { const cb = palette[b2 >>> 24];
                sum += (ca[0] - cb[0]) * (ca[0] - cb[0]) + (ca[1] - cb[1]) * (ca[1] - cb[1]) + (ca[2] - cb[2]) * (ca[2] - cb[2]); }
              if (sum < bs) { bs = sum; best = a; } } }
          vb[bi] = (p & 0x00ffffff) | ((best >>> 24) << 24); }
        return vb; };
      if (blacks.length && cands.length) {
        f.voxBlink = shut(blacks);                       // BOTH eyes at once — what every creature before the frog does
        // ── ONE EYE AT A TIME (user 2026-08-21: "have one eye blink first, then .5 seconds later the other eye") ──
        // split the eye voxels laterally on the model's own centre line and build a variant per side, so the tick
        // can shut them independently. Model space, before edRotVox, so the two sides rotate with everything else
        // and a spun model still winks the eye the viewer expects. A model whose eyes do not fall on both sides of
        // the centre (one eye, or a face in profile) simply gets no per-side variants and keeps the both-at-once
        // blink above — which is why nothing else in the game changes.
        const half = f.sx / 2;
        const L = blacks.filter((i) => (f.vox[i] & 255) < half), R = blacks.filter((i) => (f.vox[i] & 255) >= half);
        if (L.length && R.length) { f.voxBlinkL = shut(L); f.voxBlinkR = shut(R); }
      }
    }
    return frames; };
  const edSeqOf = (list) => list.filter((f) => f.seq).map((f) => f.seq).join('+');   // 'ribbet+hop' for a concatenation, 'tongue' for one, '' for a plain multi-file import — the same string edLoadVox builds its offset namespace from
  const edImportBufs = (list, offKey, bakeOff, name) => {   // LEFT/editable lane — resets the scrub + repaints
    if (!ED.frames2.length) edPalRestore();            // a fresh single-object load starts from a clean table: give back what the last import borrowed. Skipped while the PREVIEW lane holds frames, whose stamped voxels are still wearing borrowed ids — those go back at exit.
    const frames = edBuildFrames(list, offKey, bakeOff, name, true);
    if (!frames.length) return 0;
    ED.offKey = offKey || 'vb_edoffsets'; ED.name1 = name || ''; ED.seq1 = edSeqOf(list);   // remembered so gizmo autosave writes back to the SAME variant's namespace, and so the export can tell two animations of one model apart
    ED.frames = frames; ED.sel = 0; ED.paused = false;
    palSync(); edLayout();
    return frames.length; };
  const edImportBufs2 = (list, offKey, bakeOff, name) => {   // RIGHT/preview lane — loads alongside, never touches the primary's scrub/selection
    const frames = edBuildFrames(list, offKey, bakeOff, name, true);
    ED.frames2 = frames; ED.off2 = offKey || ''; ED.name2 = name || ''; ED.seq2 = edSeqOf(list); ED.sel2 = 0;
    return frames.length; };
  const EDVOX = {};                                    // asset path → Promise<ArrayBuffer|null>: re-opening the editor re-uses the first fetch, and nothing is fetched at BOOT for a panel most sessions never open
  const edFetchVox = (path) => { if (!EDVOX[path]) EDVOX[path] = fetch(path).then((r) => r.ok ? r.arrayBuffer() : null).catch(() => null); return EDVOX[path]; };
  const edLoadVox = async (path, seq, nm, bake) => {   // stage an animation straight off the asset tree: edLoadVox('assets/life/frog.vox', 'hop') — or SEVERAL run together as one: edLoadVox('assets/life/frog.vox', ['ribbet', 'hop'])
    const ab = await edFetchVox(path);
    if (!ab || ab.byteLength < 8) { console.warn('[vb] editor: could not load ' + path); return 0; }
    const file = path.slice(path.lastIndexOf('/') + 1), id = nm || file.replace(/\.vox$/, '');
    edClearStamp(); ED.frames2 = []; ED.box2 = null; ED.seq2 = ''; ED.sel2 = 0; ED.flyer2 = false; ED.mix = []; ED.mixT0 = 0; ED.bun = null; ED.arm = null; ED.bunny = false;   // same clean single-object load the file picker does, and edClearStamp FIRST for the reason its comment gives
    const seqs = Array.isArray(seq) ? seq : [seq];      // one buffer, read once per animation named — edBuildFrames concatenates them in the order given and renumbers the result 00-30
    const u8 = new Uint8Array(ab);
    const n = edImportBufs(seqs.map((q) => ({ name: file, u8, seq: q })), 'vb_edoffsets_' + id + (seq ? '_' + seqs.join('+') : ''), bake || null, id);   // own offset namespace per animation, or per COMBINATION: aligning the frog's croak-and-leap can't disturb work saved against either cycle on its own. `|| null` and never undefined: edBuildFrames reads undefined as "no table given, use the cardinal/bunny default", which belongs to neither of these
    edHudUpd();
    return n; };
  const edMixPick = () => {                             // draw the editable lane's next cycle by weight and hand the lane to it
    const m = ED.mix; if (!m.length) return;
    let tot = 0;
    for (const e of m) tot += e.w;
    let r = Math.random() * tot, i = 0;
    for (; i < m.length - 1; i++) { r -= m[i].w; if (r < 0) break; }   // last entry takes whatever float error leaves over, so a draw can never fall off the end
    const e = m[i];
    ED.mixI = i; ED.frames = e.frames; ED.seq1 = e.seq; ED.offKey = e.offKey; ED.sel = 0;
    return i; };
  const edLoadMix = async (path, specs, nm) => {        // stage a WEIGHTED PLAYLIST of animations out of one .vox on the editable lane
    // One buffer, read once per animation named. Each entry keeps its OWN bake and its OWN offset namespace:
    // the cycles are picked and played whole, so their frames never share a numbering and must never share a
    // key — both 'ribbet' and 'hop' number themselves 00.vox upward, and one namespace would collide on every
    // name and make the second cycle unalignable.
    const ab = await edFetchVox(path);
    if (!ab || ab.byteLength < 8) { console.warn('[vb] editor: could not load ' + path); return 0; }
    const file = path.slice(path.lastIndexOf('/') + 1), id = nm || file.replace(/\.vox$/, '');
    edClearStamp(); ED.frames2 = []; ED.box2 = null; ED.seq2 = ''; ED.sel2 = 0; ED.flyer2 = false; ED.bun = null; ED.arm = null; ED.bunny = false;
    edPalRestore();
    const u8 = new Uint8Array(ab), mix = [];
    for (const sp of specs) {
      const offKey = 'vb_edoffsets_' + id + (sp.seq ? '_' + sp.seq : '');
      const frames = edBuildFrames([{ name: file, u8, seq: sp.seq }], offKey, sp.bake || null, id, true);
      if (frames.length) mix.push({ frames, seq: sp.seq || '', offKey, w: Math.max(0, +sp.w || 0) });
    }
    if (!mix.length) return 0;
    ED.mix = mix; ED.mixI = 0; ED.mixT0 = 0; ED.name1 = id; ED.paused = false;
    ED.hopX = ED.hopY = ED.hopZ = 0;
    edMixPick();
    palSync(); edLayout(); edHudUpd();
    return ED.frames.length; };
  const edLoadVox2 = async (path, seq, nm, bake, fly) => {   // …and the same into the SIDE lane, ALONGSIDE what is already staged: edLoadVox2('assets/life/frog.vox', ['ribbet', 'hop'])
    // Deliberately NOT edLoadVox with a lane argument: that one opens a CLEAN stage (it clears lane 2, the
    // creature AI and the stamped cells first), which is exactly what a fresh single-object load should do and
    // exactly what a second exhibit must not do. Nothing here touches lane 1's frames, scrub or selection.
    const ab = await edFetchVox(path);
    if (!ab || ab.byteLength < 8) { console.warn('[vb] editor: could not load ' + path); return 0; }
    const file = path.slice(path.lastIndexOf('/') + 1), id = nm || file.replace(/\.vox$/, '');
    ED.flyer2 = !!fly; ED.spin2 = 0; bfly.init = false;   // set BEFORE the import: edImportBufs2 ends in edLayout, which already needs to know whether this lane stands still or flies
    const seqs = Array.isArray(seq) ? seq : [seq], u8 = new Uint8Array(ab);
    const n = edImportBufs2(seqs.map((q) => ({ name: file, u8, seq: q })), 'vb_edoffsets_' + id + (seq ? '_' + seqs.join('+') : ''), bake || null, id);   // its OWN offset namespace, same shape as lane 1's: aligning what is on the side can't move what is in the middle, and this is the namespace that combination's saved edits already live in
    palSync(); edLayout(); edHudUpd();                 // edImportBufs2 does neither (it is the "load alongside, don't disturb the primary" call) — but a stage load is the last thing to happen, so the borrowed colours have to reach the GPU and the model has to be stamped
    return n; };
  const edSeqsAt = async (path) => { const ab = await edFetchVox(path);   // "what animations are in this file?" — the listing behind __vb.edSeqs
    return ab ? edVoxSeqs(new Uint8Array(ab)).map((q) => ({ name: q.name, frames: q.ids.length })) : []; };
  // ── WHAT THE STAGE OPENS ON (user 2026-08-21: "load in frog.vox into the asset editor … the jumping animation
  // … should be called hop within the file") ── ONE named model, named here, fetched on demand. NOT the ordered
  // fall-through chain that used to live inside edEnter: that asked "stage whatever creature happens to be
  // loaded", so removing its head only promoted the next link, and the same "remove X from the editor" request
  // came in five times before it was deleted on 2026-08-19 (the note in edEnter has the full list). This is one
  // line naming one file and one animation, so changing what the stage opens on is editing it, and going back to
  // an empty stage is setting it to null — neither can surface a creature nobody asked for.
  // ── THE THREE CYCLES, EACH BAKED ON ITS OWN (user 2026-08-22, every value pasted back from the editor's
  // export button) ── frog.vox holds 'ribbet' (14 frames), 'tongue' (24) and 'hop' (17) as separate cycles in
  // its scene graph, and the stage plays them as a WEIGHTED PLAYLIST rather than as one concatenation: see
  // ED_STAGE below. Three tables, not one, because each cycle is picked and played whole and so keeps its own
  // frame numbering and its own 'vb_edoffsets_frog_<seq>' namespace — alignment work on the tongue cannot move
  // the hop, and neither can be knocked out of step by the other's length.
  // EVERY TABLE BUT THE HOP'S BEGINS AND ENDS AT ZERO, which is the property that makes the mix work rather
  // than an accident of the alignment: the cycle offset is last-minus-first (main/tick-support.js), so a croak
  // or a tongue-flick leaves the frog exactly where it stood and only a LEAP moves it. Any cycle can therefore
  // follow any other with no step at the join.
  const FROG_RIBBET_BAKE = { '05.vox': [0, 0, -1], '06.vox': [0, 0, -1], '07.vox': [0, 0, -1] };
  // The tongue: one lean out and back over sixteen of its twenty-four frames.
  //   oz  0 0 0 0 -1 -2 -2 -3 -3 -3 -3 -3 -3 -3 -3 -3 -2 -2 -1 -1 0 0 0 0
  const FROG_TONGUE_BAKE = {
    '04.vox': [0, 0, -1], '05.vox': [0, 0, -2], '06.vox': [0, 0, -2], '07.vox': [0, 0, -3],
    '08.vox': [0, 0, -3], '09.vox': [0, 0, -3], '10.vox': [0, 0, -3], '11.vox': [0, 0, -3],
    '12.vox': [0, 0, -3], '13.vox': [0, 0, -3], '14.vox': [0, 0, -3], '15.vox': [0, 0, -3],
    '16.vox': [0, 0, -2], '17.vox': [0, 0, -2], '18.vox': [0, 0, -1], '19.vox': [0, 0, -1] };
  // The hop. The arc of the leap is NOT in frog.vox: every one of its 17 frames has its lowest voxel at model-z
  // 0, so stamped as authored the frog stretches and lands but never leaves the ground. Aligned by hand on the
  // stage rather than derived:
  //   oy  0 0 0 0 2 3 4 5 5 3 1 0 0 0 0 0 0     rises 5 voxels (50 cm) over frames 07-08
  //   oz  0 0 0 -1 -3 -4 -5 -6 -7 -8 -8 -8 -8 -9 -10 -10 -10     one metre forward across the leap
  // Frames 00-02 are the crouch and carry nothing, so they are absent: edBuildFrames applies a table entry only
  // where one exists and leaves the rest at zero. This is the ONE table whose last frame is not zero, which is
  // what makes the leap the only thing that moves the frog down the stage.
  const FROG_HOP_BAKE = { '03.vox': [0, 0, -1], '04.vox': [0, 2, -3], '05.vox': [0, 3, -4], '06.vox': [0, 4, -5],
    '07.vox': [0, 5, -6], '08.vox': [0, 5, -7], '09.vox': [0, 3, -8], '10.vox': [0, 1, -8], '11.vox': [0, 0, -8],
    '12.vox': [0, 0, -8], '13.vox': [0, 0, -9], '14.vox': [0, 0, -10], '15.vox': [0, 0, -10], '16.vox': [0, 0, -10] };
  // ── WHAT THE STAGE OPENS ON ── ONE named model per lane, named here, fetched on demand. NOT the ordered
  // fall-through chain that used to live inside edEnter: that asked "stage whatever creature happens to be
  // loaded", so removing its head only promoted the next link, and the same "remove X from the editor" request
  // came in five times before it was deleted on 2026-08-19 (the note in edEnter has the full list).
  // ── THE FROG'S MIX (user 2026-08-22: "jump = 50%, ribbet = 40%, tongue = 10%") ── `mix` is a weighted
  // playlist on the EDITABLE lane: at every cycle boundary the frog picks its next cycle by these weights and
  // plays it whole. The three `w` values ARE the percentages, and they are the only thing to edit to change the
  // mix — nothing else reads them, and they need not sum to 100 (the pick normalises).
  // The pick is RANDOM rather than a fixed rotation. 50/40/10 is 5:4:1, so a deterministic order would be a
  // visibly periodic ten-cycle loop; a weighted draw gives the same long-run split and lets the frog croak
  // twice running, which is what an animal does. By TIME the split lands at 51/37/12 rather than 50/40/10,
  // because the three cycles are different lengths (17, 14 and 24 frames plus a 600 ms hold) — the weights are
  // per OCCURRENCE, which is the reading that makes "50% of the time it jumps" true of what you watch.
  // WHY THE PLAYLIST AND NOT ONE CONCATENATED SEQUENCE, which is what this was before: a concatenation fixes
  // the ratio at one-of-each, and hitting 5:4:1 with it means repeating segments — 165 frames of hand-written
  // table for one loop, still periodic, and every repeat of the hop needs its own cumulative z. The playlist
  // carries that in the accumulator instead, and the ratio is three numbers.
  // The editable lane can carry it because a swap only ever happens while PLAYING: pausing to scrub or to drag
  // a gizmo freezes the live cycle, so frames never move under you mid-alignment.
  // ── WHAT THE STAGE OPENS ON ── the FROG on the editable lane, running its weighted mix, and the LADYBUG
  // flying loose beside it. Two different render paths on purpose, because the two jobs are different:
  //   * The frog is a LANE — grid-stamped into W, so it is clickable, scrubbable and its per-frame offsets can
  //     be dragged and exported. That is the alignment workstation, and it is where the top-centre frame
  //     counter reads. It hops the length of the platform and wraps at the border.
  //   * The ladybug is an EXHIBIT — trace-injected through the drop slots, never written into W, so it carries
  //     a float position and a free heading and flies the way the world's butterflies do. Nothing about it can
  //     be clicked; when it needs aligning it comes back to the lane (that is what the previous stage did).
  // `mix` weights are the percentages, and they are the only thing to edit to change the split.
  // `flip` says this model's head is at +y where the engine's convention is −y (main/tick-emit.js).
  // A `hold: true` on the exhibit pins it in place for frame-by-frame work — deliberately absent here, so it
  // wanders, bobs and lands (user 2026-08-22: "have the ladybug flying around like it was previously").
  const ED_STAGE = { path: 'assets/life/frog.vox', name: 'frog',
    mix: [{ seq: 'hop', bake: FROG_HOP_BAKE, w: 50 },
          { seq: 'ribbet', bake: FROG_RIBBET_BAKE, w: 40 },
          { seq: 'tongue', bake: FROG_TONGUE_BAKE, w: 10 }],
    exhibits: [
      { model: 'ladybug', kind: 'fly', at: [-26, 15, 6], r: 22, spd: 22, bob: 4, flip: true },
      // ── THE KOI ── swims the world's open-water fish steering: long lazy sweeps rather than the flyer's
      // flutter (a ±0.5 rad/s turn retargeted every 2-5 s, against ±2 every 0.4-1.2), slower, and a gentler
      // rise and fall. No `flip`: its 26 voxels assemble into a 5 x 10 x 4 body whose long axis is y and whose
      // tail tapers to a single voxel at the far end, so its head already lies down −y where the engine
      // expects it. It carries no swim cycle of its own — koi.vox is one assembled pose, not a strip.
      // `spd` is unread for a swimmer — FISH_CFG owns its speed, so the koi cruises and flees at exactly the
      // rates the lakes do. `r` still bounds where it may wander, since the stage is 242 across and a lake is not.
      { model: 'koi', kind: 'swim', at: [30, 13, 14], r: 26, bob: 3 },
      // ── FIVE FLIES IN A BUNCH (user 2026-08-22) ── `swarm` rather than `school`: each steers for itself
      // around a shared point, so they mill about near one another instead of flying in formation. A tight
      // orbit (7) is what keeps them a bunch; `jink` gives them the fast, hard little turns a fly makes rather
      // than a butterfly's drift, and `hover` stops them settling on the deck the ladybug lands on.
      // The fly needs no loader of its own — it already ships as a desert creature, frames and all.
      { model: 'fly', kind: 'fly', at: [4, 18, -4], r: 30, spd: 11, swarm: 5, hover: true } ] };   // for a swarm r/spd drive the CENTRE's drift; BEE_ORBIT_R/W/Y own the circling
  // ── TRACE-INJECTED EXHIBITS ── everything on the stage that MOVES FREELY. A lane is grid-stamped: its voxels
  // go into W, which pins it to integer positions and the four cardinal headings — that is what made the first
  // ladybug step along the grid and face only N/S/E/W. An exhibit is never stamped; it is staged into emitBuf
  // (main/tick-emit.js) exactly as a world creature is, so it carries a float position and a free heading.
  // The emit addresses a model by ITEM ID, so an exhibit's frames must be in the item table —
  // assets/held-items.js edStripItems loads them out of the scene-graph .vox files for exactly this.
  const edExItem = (name) => {                          // model name → { item0, n } in the item table, or null if its frames never loaded
    if (name === 'ladybug') return LBUG_NFRAMES ? { item0: LBUG_ITEM0, n: LBUG_NFRAMES } : null;
    if (name === 'koi') return KOI_NFRAMES ? { item0: KOI_ITEM0, n: KOI_NFRAMES } : null;
    const f = FISHES.find((q) => q.name === name);      // every world fish species is already a strip
    if (f) return { item0: f.item0, n: f.n };
    const g = DESERTS.find((q) => q.name === name);     // …and so is every desert creature, the fly among them
    return g ? { item0: g.item0, n: g.n } : null; };
  const edExStage = () => {                             // build ED.ex from ED_STAGE.exhibits, at the stage's own coordinates
    ED.ex = [];
    if (!ED_STAGE || !ED_STAGE.exhibits) return 0;
    const cx = ED.x0 + ED.pw / 2, cz = ED.z0 + ED.pd / 2;
    for (const sp of ED_STAGE.exhibits) {
      const it = edExItem(sp.model);
      if (!it) { console.warn('[vb] editor: no item strip for exhibit ' + sp.model + ' - skipped'); continue; }
      // A SCHOOL is one leader with the rest trailing it in formation; a SWARM is several that each steer for
      // themselves round the SAME point. Flies want the second — a knot of them jinking about near each other,
      // with no one of them leading — so `swarm` makes every copy independent where `school` makes followers.
      const nCopies = Math.max(1, (sp.school | 0) || (sp.swarm | 0));
      const isSwarm = !(sp.school | 0) && (sp.swarm | 0) > 1;
      for (let i = 0; i < nCopies; i++) {
        const at = sp.at || [0, 12, 0], t0 = Math.random() * 4;   // t0 desyncs the copies of a school; bph rides it so the bob starts at zero
        ED.ex.push({ model: sp.model, item0: it.item0, n: it.n, kind: sp.kind || 'fly', fps: sp.fps || 24,
          hx: cx + at[0], hy: ED.y + 1 + at[1], hz: cz + at[2],   // HOME — the point it orbits, never left behind. A swarm shares ONE home: the ring puts them apart, so jittering it too would only smear the circle
          r: sp.r || 28, spd: sp.spd || 20, bob: sp.bob === undefined ? 4 : sp.bob, aclk: 0, flee: false, fleeT: 0,
          flip: !!sp.flip,                              // the model's head is at +y rather than the usual −y (main/tick-emit.js)
          hold: !!sp.hold,                              // HOVER IN PLACE — see the note on the exhibit itself
          // gy = the vertical OFFSET that sets the model down on the plane. The stage plane's top face is
          // ED.y + 1 and a drop model is anchored at its centre, so a 2-high ladybug rests with its centre one
          // voxel above that. Derived from this exhibit's own hy so it stays right if `at` moves.
          gy: (ED.y + 2) - (ED.y + 1 + at[1]), ph: 'fly', next: 4 + Math.random() * 8,
          lead: (isSwarm || i === 0) ? -1 : ED.ex.length - i,   // followers point at the leader's index in ED.ex; a swarm has no leader at all
          // Spread the swarm's homes a little so they do not start stacked in one voxel, and give each its own
          // retarget cadence — identical clocks would have them all jink on the same frame, which reads as one
          // object rather than five insects.
          hover: !!sp.hover, orbit: isSwarm, ph9: Math.random() * 6.283,   // a swarm member never lands: it rides its own phase on the shared ring
          hub: isSwarm && i > 0 ? ED.ex.length - i : -1,   // which member owns the drifting centre (-1 = this one does)
          cx: 0, cy: 0, cz: 0, dth: Math.random() * 6.283,   // the ring centre as an offset from home (only the hub's is driven), and the heading it drifts along — separate from E.th, which the orbit owns
          rank: i, x: 0, y: 0, z: 0, th: Math.random() * 6.2831853, om: 0, omT: 0, tRe: 0, t: t0, bph: t0 });   // bph anchors the bob's phase; seeded to t0 so the FIRST cycle also starts at zero rather than mid-swing
      }
    }
    return ED.ex.length; };
  // ── AND HOW THEY MOVE ── the world's own steering, taken from main/tick-creatures.js rather than invented:
  // retarget the turn on a timer, ease the angular velocity toward that target, integrate the heading, advance
  // along it. That integrator is what gives a flyer its loose fluttery arc and a fish its long lazy sweep; the
  // only difference between the two kinds is how hard and how often they turn.
  //   FLY  (the kind-0 butterfly/dragonfly block): omT = ±2 rad/s, retargeted every 0.4-1.2 s.
  //   SWIM (the open-water fish branch):           omT = ±0.5 rad/s, retargeted every 2-5 s.
  // Both ease at 9/s, the shared glide constant every creature in the world turns on. SPEED is the one number
  // not copied: the world flies a butterfly at 56 vox/s, which crosses everything the stage camera can see in
  // about two seconds. The stage is a preview, so it runs at a pace you can watch.
  const edExStep = (dt) => {
    if (!ED.ex.length || ED.paused) return;
    const d9 = Math.min(0.1, dt || 0.016);              // a stalled tab must not teleport an exhibit across the stage on the catch-up frame
    for (const E of ED.ex) {
      E.t += d9;
      // ── HOVERING IN PLACE (user 2026-08-22: "can you have the ladybug fly in place temporarily? I need to
      // adjust the positionings of the frame") ── the clock above still runs, so the WINGS KEEP BEATING; only
      // the wander, the landing and the bob are held. Position and heading are pinned, which is the point: a
      // model that is drifting and turning cannot be judged frame against frame. The heading is a right angle
      // so it presents side-on to the camera edEnter sets up, where the wing sweep reads most clearly.
      if (E.hold) { E.x = 0; E.y = 0; E.z = 0; E.th = Math.PI / 2; E.om = 0; E.omT = 0; E.ph = 'fly'; E.aclk += E.fps * d9; continue; }   // the clock still runs: holding a model STILL is not holding it on one frame
      // ── A SWARM ORBITS, IT DOES NOT STEER (user 2026-08-22: "the bees already do this when they are chasing
      // the player, same mechanic just passive") ── this is the hive orbit out of main/tick-creatures.js, with
      // the exhibit's home standing in for the hive: each member holds its OWN phase on one ring, the vertical
      // term runs at 1.7× that phase offset per individual so they weave rather than rise together, and the
      // heading is tangent to the circle. Held KINEMATICALLY — no wander, no eased turn, no containment — which
      // is why it stays a bunch by construction instead of by a tether fighting an overshoot.
      // The three constants are the bees' own (sim/life/slots.js), read rather than copied: 1.9 rad/s across
      // 6.5 voxels is 12.4 vox/s of apparent circling, which is what makes it read as hovering rather than
      // flying somewhere.
      if (E.orbit) {
        // ── THE RING'S CENTRE DRIFTS (user 2026-08-22: "have the fly swarm move around more. it stays pretty
        // still") ── orbiting a FIXED point is a swarm spinning on the spot; the bees' chase version rides a
        // moving centre (the player), and this is the same thing with a wander in its place. ONE member owns
        // the drift and the rest read it, so the ring translates whole instead of five flies each wandering
        // off — which is what would pull the bunch apart.
        // The drift is the ordinary flyer wander, slowed: an eased turn on a retarget timer, steered back when
        // it passes E.r. Well under the orbit's own 12.4 vox/s, so the circling still reads as the motion and
        // the drift as the swarm going somewhere.
        if (E.hub < 0) {
          // A GENTLE turn target and a long dwell, deliberately: at ±1.1 rad/s against 11 vox/s the drift's own
          // turn radius is ~10 voxels, so it looped inside a 25-voxel patch and never used the room it had
          // (measured over 4434 samples: 25 x 25 of a 60-wide area). ±0.45 gives a ~24-voxel radius, so it makes
          // long runs across its range instead of circling one spot.
          // ── THE DRIFT STEERS ON ITS OWN HEADING (E.dth), NOT E.th ── E.th is the RENDER heading and the
          // orbit below overwrites it every frame with the tangent to the ring. Integrating the drift on it
          // meant steering on a heading spinning at BEE_ORBIT_W, so the centre traced a circle of spd/1.9 ≈ 5.8
          // voxels and no turn-rate or radius change could widen it — measured, 24 x 25 of travel whether the
          // turn target was ±1.1 or ±0.45, which is what gave the bug away.
          if (E.t > E.tRe) { E.omT = (Math.random() - 0.5) * 0.9; E.tRe = E.t + 1.6 + Math.random() * 2.4; }
          if (E.cx * E.cx + E.cz * E.cz > E.r * E.r) {
            const inT = Math.atan2(-E.cx, -E.cz), dth = Math.atan2(Math.sin(inT - E.dth), Math.cos(inT - E.dth));
            E.omT = Math.max(-4, Math.min(4, dth * 3));
          }
          E.om += (E.omT - E.om) * (1 - Math.exp(-6 * d9)); E.dth += E.om * d9;
          E.cx += Math.sin(E.dth) * E.spd * d9; E.cz += Math.cos(E.dth) * E.spd * d9;
          E.cy = Math.sin(E.t * 0.55) * 3;               // …and a slow rise and fall, so it does not swim one flat plane
        }
        const H9 = E.hub >= 0 ? ED.ex[E.hub] : E;        // every member reads the hub's centre, so the bunch travels together
        E.ph9 += BEE_ORBIT_W * d9;
        E.x = H9.cx + Math.sin(E.ph9) * BEE_ORBIT_R;
        E.z = H9.cz + Math.cos(E.ph9) * BEE_ORBIT_R;
        E.y = H9.cy + Math.sin(E.ph9 * 1.7 + E.rank) * BEE_ORBIT_Y;
        E.th = E.ph9 + 1.5708;
        // ── AND THE WINGS STILL BEAT ── this branch and the hold above both `continue` past the steering, and
        // the animation clock lived at the END of that steering path, so a swarm member advanced its orbit and
        // never its frame: five flies circling on frame 0 with their wings frozen (user 2026-08-22). Every path
        // out of this loop has to carry the clock; only a LANDED exhibit is meant to hold a frame, and that one
        // is pinned deliberately in main/tick-emit.js rather than by forgetting to tick.
        E.aclk += E.fps * d9;
        continue;
      }
      if (E.lead >= 0) continue;                        // followers are placed off the leader, below — they do not steer
      const fly = E.kind === 'fly';
      if (E.t > E.tRe) { E.omT = (Math.random() - 0.5) * (fly ? 4.0 : 1.0); E.tRe = E.t + (fly ? 0.4 + Math.random() * 0.8 : 2 + Math.random() * 3); }
      if (E.x * E.x + E.z * E.z > E.r * E.r) {          // outside its orbit → come about, hard enough to actually turn round
        const inTh = Math.atan2(-E.x, -E.z), dth = Math.atan2(Math.sin(inTh - E.th), Math.cos(inTh - E.th));
        E.omT = Math.max(-5, Math.min(5, dth * 3));
      }
      // ── LANDING (user 2026-08-22: "have the lady bug land on the platform periodically") ── four phases on
      // one clock: fly, come down, sit, climb away. A LANDED exhibit stops dead — no steering, no advance, no
      // bob — and main/tick-emit.js holds it on frame 00 while `ph` says 'land', so the wings are still rather
      // than mid-flap. The descent and the climb keep flying horizontally, so it banks down onto the deck and
      // lifts off again instead of dropping and rising on the spot.
      if (fly) {
        // ── STARTLED (user 2026-08-22: "do the same thing to the ladybug") ── the world flyers' own numbers,
        // read live from the same FLY_* constants main/tick-creatures.js uses, so the stage bug and the forest
        // butterflies cannot drift apart. Measured to the player's chest (P.y + 2), the same test they use.
        const dxF = (E.hx + E.x) - P.x, dyF = (E.hy + E.y) - P.y - 2, dzF = (E.hz + E.z) - P.z;
        if (dxF * dxF + dyF * dyF + dzF * dzF < FLY_THREAT_R * FLY_THREAT_R) E.fleeT = E.t + FLY_FLEE_HOLD;
        E.flee = E.t < (E.fleeT || 0);
        // A startled bug does not settle, and one already down gets off the deck: without this the speed would
        // double while it sat there, or it would calmly touch down with the player on top of it.
        if (E.flee && (E.ph === 'down' || E.ph === 'land')) E.ph = 'up';
        if (E.ph === 'fly' && !E.flee && !E.hover && E.t > E.next) E.ph = 'down';
        else if (E.ph === 'down' && Math.abs(E.y - E.gy) < 0.5) { E.ph = 'land'; E.y = E.gy; E.next = E.t + 3 + Math.random() * 4; }
        else if (E.ph === 'land' && E.t > E.next) E.ph = 'up';
        // ── AND THE BOB IS RE-PHASED ON TAKEOFF ── it is sin(t · rate) on a FREE-RUNNING clock, so resuming it
        // at whatever phase t had reached snapped the model from the ~0 it climbed to up to a full bob amplitude
        // in one frame — the upward glitch after takeoff (user 2026-08-22). Anchoring the phase here starts the
        // bob at zero and rising, which is continuous with the climb it just finished.
        else if (E.ph === 'up' && Math.abs(E.y) < 0.5) { E.ph = 'fly'; E.bph = E.t; E.next = E.t + 6 + Math.random() * 10; }
      }
      if (E.ph === 'land') continue;                    // sitting still: the heading it landed on is the heading it keeps
      // ── A SWIMMER RUNS ON THE WORLD'S OWN FISH NUMBERS ── read live out of FISH_CFG rather than copied into
      // constants here, so the stage and the lakes cannot drift apart: retune sim/life/fish.js (or
      // __vb.fishCfg.baseSpeed = … from the console) and this moves with it. What it takes:
      //   baseSpeed  cruise, and fleeMult × that when the player is inside threatR — EXACTLY double, which is
      //              the "twice as fast when the player is near" the world fish already do.
      //   fleeHold   the flee state lingers after the threat leaves, so it cannot flicker at the boundary.
      //   yawRate / fleeYawRate  the heading-change cap, sharper while fleeing because double speed needs
      //              harder banking — clamped rather than eased into, exactly as the creature tick clamps it.
      //   animFps    the tail beat, which scales with the speed: 24 at cruise, 48 fleeing.
      // The FLAP does not scale with the bolt, deliberately: the world butterflies' does not either (their
      // frame index rides a shared clock), and matching them is the whole point. The fish are the other way —
      // FISH_CFG says the tail beat scales, so below it does.
      let spdN = E.spd * (fly && E.flee ? FLY_FLEE_MULT : 1), fpsN = E.fps;
      if (!fly) {
        // The threat test is the creature tick's own, including its vertical term (main/tick-creatures.js
        // measures to P.y + 2, roughly the player's chest) — a stage swimmer flies well above head height, so a
        // flat distance would have it bolting from a player standing harmlessly underneath it. No predator scan:
        // that walks the duck band, and nothing lives on the stage but what ED_STAGE puts there.
        const C = FISH_CFG, dx9 = (E.hx + E.x) - P.x, dy9 = (E.hy + E.y) - P.y - 2, dz9 = (E.hz + E.z) - P.z;
        if (dx9 * dx9 + dy9 * dy9 + dz9 * dz9 < C.threatR * C.threatR) E.fleeT = E.t + C.fleeHold;
        E.flee = E.t < (E.fleeT || 0);
        const mul = E.flee ? C.fleeMult : 1;
        spdN = C.baseSpeed * mul; fpsN = C.animFps * mul;
        const yr = E.flee ? C.fleeYawRate : C.yawRate;
        if (E.om > yr) E.om = yr; else if (E.om < -yr) E.om = -yr;
      }
      E.om += (E.omT - E.om) * (1 - Math.exp(-9 * d9)); E.th += E.om * d9;
      E.x += Math.sin(E.th) * spdN * d9; E.z += Math.cos(E.th) * spdN * d9;
      // ── THE ANIMATION RIDES ITS OWN CLOCK, NOT t × fps ── the rate CHANGES the moment a fish flees, and
      // `floor(t · fps)` jumps the frame index when fps does. Accumulating frames instead means the beat just
      // gets quicker from where it already was, which is the same reason the world keeps B.animClk.
      E.aclk = (E.aclk || 0) + fpsN * d9;
      if (E.ph === 'down') E.y += (E.gy - E.y) * (1 - Math.exp(-2.2 * d9));        // settle onto the deck
      else if (E.ph === 'up') E.y += (0 - E.y) * (1 - Math.exp(-2.2 * d9));        // …and climb back to the cruising height, where the bob takes over again
      else E.y = Math.sin((E.t - (E.bph || 0)) * (fly ? 2.2 : 1.1)) * E.bob;
    }
    // ── THE SCHOOL ── every follower is placed off the LEADER rather than steering for itself, which is how the
    // world keeps its ducklings together: a fixed offset behind and to the side, rotated into the leader's
    // heading, so the formation turns with it instead of smearing on every corner. Each keeps its own bob phase.
    for (const E of ED.ex) {
      if (E.lead < 0) continue;
      const L = ED.ex[E.lead]; if (!L) continue;
      E.aclk = (E.aclk || 0) + (L.fps || 24) * d9;      // a follower beats on the leader's rate, so a school stays in step
      const back = 7 + ((E.rank - 1) >> 1) * 7, side = ((E.rank & 1) ? 5 : -5) * (1 + (((E.rank - 1) >> 1) * 0.35));
      const Hx = Math.sin(L.th), Hz = Math.cos(L.th);
      E.x = L.x - Hx * back + Hz * side; E.z = L.z - Hz * back - Hx * side;
      E.th = L.th; E.y = Math.sin((E.t - (E.bph || 0)) * 1.1 + E.rank) * E.bob;
    } };
  const edStage = () => { if (!ED_STAGE) return;
    edFetchVox(ED_STAGE.path).then(async () => { if (!ED.on || ED.frames.length) return;   // closed again, or a manual import landed while the fetch was in flight — never clobber what is on the stage
      const n = ED_STAGE.mix ? await edLoadMix(ED_STAGE.path, ED_STAGE.mix, ED_STAGE.name)
                             : await edLoadVox(ED_STAGE.path, ED_STAGE.seq, ED_STAGE.name, ED_STAGE.bake);
      const sd = ED_STAGE.side;
      // A `side` entry, if one is ever named again, is chained INSIDE the primary's load and re-tests the same
      // guard rather than running beside it: a manual import landing between the two would take the stage
      // (edImportBufs clears lane 2), and an unconditional second load would then park a model next to it.
      if (n && sd && ED.on && ED.frames.length) await edLoadVox2(sd.path || ED_STAGE.path, sd.seq, sd.name || ED_STAGE.name, sd.bake, sd.fly);
      if (ED.on) edExStage(); }); };                    // …and the free-moving exhibits, which need no fetch of their own: their frames were loaded into the item table at boot
  const edEnter = () => {
    if (ED.on) return;
    ED.on = true; ED.ret = { x: P.x, y: P.y, z: P.z, yaw: P.yaw, pitch: P.pitch, fly: P.fly };   // fly saved too — the editor turns it ON for the framing below, exit restores what the player had
    cmpVis();                                          // hide the top-centre compass while in the editor (user)
    petalClear();                                      // …and clear the leaves already falling: petalTick refuses to shed once ED.on, but the ones mid-flight would drift past the stage for another ten seconds (user 2026-08-21)
    // ── THE STAGE LOOKS FOR LOW GROUND (user 2026-08-18: the grove needs headroom) ── it used to plant itself
    // exactly at the player, and then had to sit above the tallest thing on its own footprint for brick purity.
    // Land next to a giant oak and the stage rides up with it, and since a model is stamped UPWARD from the
    // stage the room left under the world ceiling is whatever the terrain happened to leave: measured 46 voxels
    // against a grove 101 tall, i.e. the top two thirds simply clipped away.
    // So try a ring of candidate origins and take the one whose terrain is LOWEST. A coarse stride is enough to
    // choose between them — the fine scan below still decides the actual height — and a meadow twenty metres
    // away routinely sits 40+ voxels under a crown, which is the difference between a grove fitting and not.
    const stageTop = (sx0, sz0, step) => {
      let t = WL;
      for (let z = sz0; z < sz0 + ED.pd; z += step) for (let x = sx0; x < sx0 + ED.pw; x += step) {
        const gx = gwrap(x, WX), gz = gwrap(z, WZ), b2 = gx + gz * WX * WY;
        let y = Math.min(WY - 2, hmap[gx + gz * WX] + 118);
        while (y > WL && !W[b2 + y * WX]) y--;
        if (y > t) t = y;
      }
      return t;
    };
    const clampX = (v) => Math.max(rect.xlo + 4, Math.min(rect.xhi - 4 - ED.pw, v));
    const clampZ = (v) => Math.max(rect.zlo + 4, Math.min(rect.zhi - 4 - ED.pd, v));
    const px0 = Math.round(P.x / 10) * 10 - (ED.pw >> 1), pz0 = Math.round(P.z / 10) * 10 - (ED.pd >> 1);
    let bestX = clampX(px0), bestZ = clampZ(pz0), bestT = stageTop(bestX, bestZ, 8);
    // Three rings, not one. A single step lands in the same stand of trees you were standing in; the low ground
    // that actually buys headroom — a meadow, a shore, a river flat — is usually a few hundred voxels out. The
    // player is teleported onto the stage regardless, so distance costs nothing, and a coarse stride keeps all
    // 24 probes cheap enough to run on the button press.
    for (let ring = 1; ring <= 3; ring++) for (const [ox, oz] of [[0, -1], [0, 1], [-1, 0], [1, 0], [-1, -1], [1, -1], [-1, 1], [1, 1]]) {
      const cx = clampX(px0 + ox * ring * (ED.pw + 24)), cz = clampZ(pz0 + oz * ring * (ED.pd + 24));
      const t = stageTop(cx, cz, 8);
      if (t < bestT) { bestT = t; bestX = cx; bestZ = cz; }
    }
    ED.x0 = bestX; ED.z0 = bestZ;
    let topMax = WL;                                   // stage must sit above all content on its footprint (its bricks stay PURE — no terrain sharing an 8³ brick with the plane)
    for (let z = ED.z0; z < ED.z0 + ED.pd; z += 2) for (let x = ED.x0; x < ED.x0 + ED.pw; x += 2) {
      const gx = gwrap(x, WX), gz = gwrap(z, WZ), b2 = gx + gz * WX * WY;
      let y = Math.min(WY - 2, hmap[gx + gz * WX] + 118);
      while (y > WL && !W[b2 + y * WX]) y--;
      if (y > topMax) topMax = y;
    }
    // ── HEADROOM FOR WHAT IS ON THE STAGE (user 2026-08-18: the four birches were not showing) ── the stage
    // has to clear the tallest terrain on its own footprint (brick purity: an 8³ brick shared with terrain
    // would draw that terrain), but it was ALSO pinned within 80 of the ceiling, and a model is stamped upward
    // from it. Measured: WY 384, stage 336, so 46 voxels of room — and the birch grove is 81 tall, so both big
    // trees were cut off at the waist and it did not read as four trees at all.
    // Doubling the stage footprint made it worse rather than better: topMax now scans four times the area, so
    // it is far more likely to find a tall tree to clear.
    // WY - 160 as the floor buys up to ~158 on flat ground while the topMax term still lifts it clear of
    // anything underneath. It cannot promise a fixed amount — the terrain decides — which is why the grove is
    // also sized to fit the worst case rather than the best.
    ED.y = Math.min(WY - 24, Math.max((topMax + 15) & ~7, WY - 160));   // 8-aligned, clear of the ground below, as low as the ceiling allows
    unstampAllWorms();                                 // clear live worms out of W before the editor freezes the world (else edExit's rebuildBricks would resurrect them)
    bricks.fill(0); bricks2.fill(0); wbricks.fill(0);  // SEPARATE LEVEL: empty occupancy = the whole world vanishes from the tracer (W is untouched — exit rebuilds)
    const cells = [];
    for (let z = ED.z0; z < ED.z0 + ED.pd; z++) for (let x = ED.x0; x < ED.x0 + ED.pw; x++) edSet(x, ED.y, z, edPlaneId(x, z), cells);
    gpuPatch(cells, true, cells.length, false);        // track=false: entering the editor overwrites real world cells with the stage plane, and none of that is a terrain edit
    P.x = ED.x0 + ED.pw / 2; P.z = ED.z0 + ED.pd / 2; P.y = ED.y + 2; P.vy = 0; P.fly = true;   // fly ON so the framing below can't be pulled off by gravity while the world is frozen
    smoothEye = P.y + EYE; resetHist = 1;
    // ── THE STAGE OPENS ON THE SKUNK (user 2026-08-19: "remove the porcupine from the editor") ──
    // NO TREE AND NO PORCUPINE IS STAGED ANY MORE. The BIRCH, then the FIR, then the PORCUPINE walk cycle
    // each stood at the head of this fall-through chain in turn; all three are gone from HERE. The two trees
    // are also gone from the boot (nothing fetches birch.vox or fir_spruce.vox), but the PORCUPINE IS NOT:
    // it is one of the four land mammals and it still SPAWNS IN THE WORLD, so every porcupine symbol stays.
    // PORCUPINE_WALK is still fetched in assets/held-items.js, buildPorcPoses/stampPorcupine (just above)
    // still stamp the world porcupine, and PORCUPINE_BAKES still aligns it. Only the editor's USE of the
    // model is gone. Note buildPorcPoses reads edParseVox + PORCUPINE_BAKES directly — it never read the
    // 'vb_edoffsets_porcupine' localStorage namespace — so dropping that namespace with this branch cannot
    // move the world porcupine by a voxel. (game/assets/life/porcupine/*.vox and the decoration .vox files
    // all STAY on disk: VOXDEX is a build-time walk of game/assets and /spawn + /locate resolve through it.)
    // With the porcupine branch gone the chain falls through to its next live link — the SKUNK walk cycle,
    // which is what the stage opened on before the porcupine was staged.
    // ── THE SKUNK IS OFF THE STAGE TOO (user 2026-08-19: "remove the skunk from the asset editor") ── the
    // fourth removal from this chain today, after the birches, the fir and the porcupine. Only the STAGING
    // branch went: every SKUNK_* symbol stays, because the WORLD skunk needs them and — unlike the
    // porcupine, whose poses are built straight from edParseVox — buildSkunkPoses goes through
    // edBuildFrames and the 'vb_edoffsets_skunk' namespace, so deleting that namespace would move the
    // skunk that walks the forest. The chain now heads on the bunny.
    // ── THE STAGE OPENS EMPTY (user 2026-08-19: "now theres a bunny in the asset editor, remove it. why do
    // land mammals keep spawning in here? investigate and fix it") ── HERE IS WHY. This was an ordered
    // fall-through: "if nothing is staged, stage the next creature that happens to be loaded". Removing the
    // head of it only ever PROMOTED the next link, which is why the same request has now come in five times
    // — the four birches, then the fir, then the porcupine, then the skunk, and now the bunny. It was
    // whack-a-mole by construction, and the next removal would simply have surfaced the bluebird.
    // So the chain is gone rather than shortened again. The editor's own empty-stage message has said
    // "press ESC, then import a .vox to begin" the whole time (see edHudUpd at the top of this file), so an
    // empty stage is what it always claimed to do; the chain was the thing contradicting it.
    // Nothing about IMPORTING changed, and no creature's data was touched — every BUNNY_*/BLUEBIRD_* symbol
    // stays for the world, exactly as the skunk's and porcupine's did.
    ED.bun = null; ED.bunny = false; ED.arm = null;   // an empty stage animates nothing: clear the playback state the branches used to set
    // ── FRAME THE OBJECT ── stand the player BACK from the stage centre (where the model is stamped) with the FEET
    // ON THE FLOOR (P.y = ED.y + 1 = plane top), then pitch to aim the eye at the model's vertical centre so the object
    // stays framed. (The old framing floated the eye at the model's mid-height, which for any model shorter than the 18.5
    // eye height sank the 20-voxel person's feet below the plane — the "clipped through the floor" the user reported.)
    // ── …AND STAND FAR ENOUGH BACK TO SEE ALL OF IT (2026-08-19) ── `back` was a fixed 50, which frames a
    // bunny and cuts a tree off at the knees, so solve for the distance instead of picking one.
    // WHAT THE FIRST ATTEMPT GOT WRONG, because it is subtle and cost a user-visible bug: it solved for the
    // model's TOTAL angular span fitting the vertical FOV. That is only the right question if the camera axis
    // BISECTS the span — and the line below deliberately aims at the model's MID-HEIGHT instead, which for a
    // tall model is a very different direction (measured on the 116-voxel fir: axis 26.3°, bisector 18.8°).
    // The whole 7.5° of error lands on the bottom edge, so the tree's base and the stage under it were pushed
    // off-screen (measured ndcBot −1.127, i.e. 12.7% past the edge) while the crown sat comfortably inside.
    // So solve the constraint the code actually imposes, per edge, against the axis it actually uses. With the
    // eye EYE above the plane, the model H tall, and the axis at p = atan(A/d) where A = H/2 − EYE:
    //     bottom   p + atan(EYE/d)        ≤ V     →   tan(V)·d² − (H/2)·d + tan(V)·(−A·EYE)     ≥ 0
    //     top      atan((H−EYE)/d) − p    ≤ V     →   tan(V)·d² − (H/2)·d + tan(V)·((H−EYE)·A)  ≥ 0
    // Both are the same quadratic in d with a different constant, so `edge` takes the larger root of it; no
    // real root means that edge is never crossed at any distance, which is the normal case for anything
    // shorter than eye height. V is the VERTICAL HALF-angle: tanH = tan(FOV/2) in tick-camera.js, and
    // pre.js applies it as `up * (ndc.y * u.tanH)`, so vertical framing does not move with the aspect ratio.
    // FOV is read from its own declaration (ui/hud.js) rather than retyped, which is how the 72-vs-36 slip
    // happened the first time. MARGIN leaves air around the model rather than fitting it to the frame edge.
    // Floored at the old 50 so every small model is framed EXACTLY as before — this only ever backs off.
    { const bf = ED.frames[0]; const H = bf ? bf.sz : 12, midH = H * 0.5;
      const MARGIN = 1.22, tv = Math.tan(FOV / 2), A = midH - EYE;
      const edge = (c) => { const disc = midH * midH - 4 * tv * tv * c; return disc <= 0 ? 0 : (midH + Math.sqrt(disc)) / (2 * tv); };
      const fit = Math.max(edge(-A * EYE), edge((H - EYE) * A));   // the nearer the camera may stand with BOTH edges clear
      const cx = ED.x0 + ED.pw / 2, cz = ED.z0 + ED.pd / 2, back = Math.max(50, Math.round(fit * MARGIN));
      P.x = cx; P.z = cz - back;                        // back along −Z (yaw 0 faces +Z → the model)
      P.y = ED.y + 1;                                   // feet planted on the plane (NOT floating at eye-mid-height)
      P.yaw = 0; P.pitch = Math.atan2(midH - EYE, back);   // aim the eye at the model's vertical centre → the object sits framed in front of the camera
      P.vy = 0; smoothEye = P.y + EYE; resetHist = 1; }
    edBtnEl.classList.add('on'); edRowEl.classList.remove('hidden'); edHudEl.classList.remove('hidden');
    edHudUpd();
    edStage();                                         // …and load what the stage opens on (async; the guard inside it is what makes a manual import mid-fetch win)
  };
  const edExit = () => {
    if (!ED.on) return;
    ED.on = false;
    edPalRestore();                                    // hand the borrowed palette entries back BEFORE the world comes out of the void below — those ids are coloured for the world, not for the model that just left the stage
    const cells = [];
    for (const [ii, pv] of ED.prev) if (W[ii] !== pv) { W[ii] = pv; cells.push(ii); }
    ED.prev.clear(); ED.frames = []; ED.frames2 = []; ED.sel = -1; ED.sel2 = 0; ED.seq1 = ''; ED.seq2 = ''; ED.mix = []; ED.mixT0 = 0; ED.fcells = []; ED.fcells2 = []; ED.ring = []; ED.box = null; ED.box2 = null; ED.bun = null; ED.arm = null; bfly.init = false;   // both lanes + creature AI cleared → no stale editor hitbox lingers after exit
    gpuPatch(cells, true, cells.length, false);        // track=false: this RESTORES the world the editor borrowed — it is a rollback, not an edit
    rebuildBricks(0, WX, 0, WZ); uploadBricks();       // bring the whole world back from the void (occupancy was zeroed on enter)
    const r = ED.ret; if (r) { P.x = r.x; P.y = r.y; P.z = r.z; P.yaw = r.yaw; P.pitch = r.pitch; P.fly = !!r.fly; P.vy = 0; smoothEye = P.y + EYE; resetHist = 1; }
    edBtnEl.classList.remove('on'); edRowEl.classList.add('hidden'); edHudEl.classList.add('hidden');
    cmpVis();                                          // restore the compass (if locked + setting on) now the editor is closed
  };
  edBtnEl.addEventListener('click', (e) => { e.stopPropagation(); ED.on ? edExit() : edEnter(); });
  // ── THE EDITOR OPENS ON REFRESH (user 2026-08-22: "load me straight into the editor on refresh") ── this
  // was removed on 2026-07-22 and is back by request. It waits for the world to be ready rather than firing at
  // parse time: edEnter reads hmap and W to choose the stage's ground and height, and picks nonsense from a
  // world that has not generated — the stage lands at the world floor with the player inside it.
  // Deliberately NOT inside edEnter itself: opening the editor is otherwise always something the player did.
  // ── …BUT NOT IN A TEST SESSION ── entering the editor zeroes the brick occupancy, stamps a plane into W and
  // unstamps the worms, so a headless run that boots straight into it is measuring the STAGE and not the world.
  // Measured the moment this went in: tools/vbtest.py reported 736,804 pool-vs-inline voxel diffs and "gen
  // caught up: False" — not a worldgen regression (deepHash was bit-identical) but every terrain probe reading
  // the editor's floating platform. `?cdp` is the flag every test tool already passes, so tests boot the world
  // and players boot the editor; a test that wants the stage opens it the way they all already do, __vb.ed(true).
  const edBoot = () => { if (ED.on) return;
    if (typeof W === 'undefined' || !W || !hmap || !hmap.length || !rect) { setTimeout(edBoot, 120); return; }
    edEnter(); };
  if (!location.search.includes('cdp')) setTimeout(edBoot, 0);
  $('edCopy').addEventListener('click', (e) => { e.stopPropagation();   // copy the per-frame offsets → paste back to be baked into the code (replaces the .vox exporter)
    const btn2 = e.currentTarget; if (!ED.frames.length) { btn2.dataset.lbl = 'nothing to copy'; setTimeout(() => { btn2.dataset.lbl = 'export'; }, 1500); return; }   // icon button now — flash the hover LABEL + a green pulse instead of replacing the SVG with text
    edCopyOffsets(); btn2.classList.add('copied'); btn2.dataset.lbl = 'copied ✓ — paste it to me'; setTimeout(() => { btn2.classList.remove('copied'); btn2.dataset.lbl = 'export'; }, 2000); });
  edFileEl.addEventListener('click', (e) => e.stopPropagation());
  edFileEl.addEventListener('change', async () => {
    const list = [];
    for (const f of [...edFileEl.files].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true })))
      list.push({ name: f.name, u8: new Uint8Array(await f.arrayBuffer()) });
    if (list.length === 1) { const qs = edVoxSeqs(list[0].u8);   // ONE file holding several NAMED animations (frog.vox = ribbet/tongue/hop): stage the first rather than all three concatenated, and say what the others are called so the second one is reachable
      if (qs.length > 1) { list[0].seq = qs[0].name;
        console.log('[vb] editor: ' + list[0].name + ' holds ' + qs.length + ' animations — ' + qs.map((q) => q.name + ' (' + q.ids.length + ' frames)').join(', ') + '. Staged ' + qs[0].name + "; __vb.edLoad('assets/…/" + list[0].name + "', '" + (qs[1] ? qs[1].name : qs[0].name) + "') stages another."); } }
    if (list.length) { edClearStamp(); ED.frames2 = []; ED.box2 = null; ED.seq2 = ''; ED.sel2 = 0; ED.flyer2 = false; ED.mix = []; ED.mixT0 = 0; ED.bun = null; ED.arm = null; edImportBufs(list); ED.bunny = false; }   // a manual .vox import is a clean SINGLE-object edit — clear the second lane + creature AI + armadillo walk so only the imported model shows. edClearStamp runs FIRST, as its comment says: edImportBufs ends in edLayout, which stamps the new model, so clearing afterwards erased what had just been laid out and the import stayed invisible until the next repaint (up to 3.4 s).
    edFileEl.value = '';
  });

