  // @module — the asset editor: platform, gizmos, .vox parse, pose bakes
  // @exports BUNNY_JUMP_BAKE, BUNNY_ROT_BAKE, BUNNY_ROT_BAKE_R, edApplyRot, edCol, edCopyOffsets, edEnsureGizCols, edEnsureRgizCols, edEnter, edExit, edExportSeq, edHudUpd, edImportBufs, edLayout, edMoveStep, edOffset, edParseVox, edRotVox, edRotate, edSaveOffsets, edSelStep, edSwapBunnies, stampArmadillo, stampBunny, stampPorcupine, stampSkunk
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
  const edOffset = (axis, d) => { const n = ED.frames.length; if (!n) return; const f = ED.frames[((ED.sel % n) + n) % n];   // nudge the selected frame one voxel along an axis + restamp
    f.ox = (f.ox || 0) + (axis === 0 ? d : 0); f.oy = (f.oy || 0) + (axis === 1 ? d : 0); f.oz = (f.oz || 0) + (axis === 2 ? d : 0); edLayout(); };
  const edSaveOffsets = () => { try { const m = {}; for (const f of ED.frames) { const hasRot = f.rot && f.rot.length; if (f.ox || f.oy || f.oz || hasRot) m[f.name] = hasRot ? [f.ox || 0, f.oy || 0, f.oz || 0, f.rot.slice()] : [f.ox, f.oy, f.oz]; } localStorage.setItem(ED.offKey || 'vb_edoffsets', JSON.stringify(m)); } catch (e) {} };   // AUTOSAVE on release → the CURRENT variant's offset namespace (ED.offKey); 4th element = the rotation step list
  const edFrameOffs = (frames) => frames.map((f, i) => ({ frame: i, name: f.name, ox: f.ox || 0, oy: f.oy || 0, oz: f.oz || 0, rot: (f.rot || []).slice() }));   // rot = ordered 90° steps ('y+'/'y-'/'p+'/'p-') so the export carries rotation too
  const edCopyOffsets = () => {                          // export BOTH bunnies at once (user) — labelled by lane so I can bake each variant's positions
    const out = { [ED.name1 || 'left']: edFrameOffs(ED.frames) };
    if (ED.frames2.length) out[ED.name2 || 'right'] = edFrameOffs(ED.frames2);
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
    const LANE = ED.frames2.length ? 24 : 0;           // side-by-side lanes when a SECOND bunny is loaded; centred on the stage when the editable bunny is solo
    const wrapIn = (v, lo, range) => lo + (((v - lo) % (range + 1) + (range + 1)) % (range + 1));   // fit a model start into [lo, lo+range] so the whole footprint stays in bounds after a forward-march shift
    const n = ED.frames.length;
    if (n) {                                           // ── EDITABLE bunny — LEFT lane ──
      const f = ED.frames[((ED.sel % n) + n) % n];
      const vsrc0 = (ED.blink && f.voxBlink) ? f.voxBlink : f.vox;        // eye-blink variant while the blink phase is lit (playing only)
      const rv = edRotVox(vsrc0, f.sx, f.sy, ED.paused ? 0 : (-ED.spin & 3));   // no whole-animation spin while editing/paused — pausing ALWAYS parks the model at SOUTH: that's the only heading you align (gizmo offsets are south-space; armOffset derives the rest)
      const bx = ED.x0 + ((ED.pw - rv.sx) >> 1) - LANE, bz = ED.z0 + ((ED.pd - rv.sy) >> 1);   // LEFT lane
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
    if (n2) {                                          // ── PREVIEW bunny — RIGHT lane (animates, no gizmos/ring) ──
      const f2 = ED.frames2[((ED.sel % n2) + n2) % n2];
      const rv2 = edRotVox((ED.blink && f2.voxBlink) ? f2.voxBlink : f2.vox, f2.sx, f2.sy, 0);
      const bx2 = ED.x0 + ((ED.pw - rv2.sx) >> 1) + LANE, bz2 = ED.z0 + ((ED.pd - rv2.sy) >> 1);   // RIGHT lane
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
    const hx = ED.hopX, hy = ED.hopY, hz = ED.hopZ; ED.hopX = ED.hop2X; ED.hopY = ED.hop2Y; ED.hopZ = ED.hop2Z; ED.hop2X = hx; ED.hop2Y = hy; ED.hop2Z = hz;
    ED.sel = 0; ED.paused = false; ED.giz = false; ED.rgiz = false;   // fresh scrub/gizmo state for the newly-editable bunny
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
      else if (palette.length < 256) cid = addCol(r, g, b);
      else cid = palNearest(r, g, b);                  // was a THIRD copy of the nearest-colour walk, and the only one that did not skip RESERVED ids — so a full palette could snap imported art onto a pinecone id and every stamp of it would right-click up as a pinecone. palNearest (assets/palette.js) is the one addCol and palShare use.
      edColCache.set(key, cid);
    }
    return cid; };
  const edParseVox = (pv, name) => {                   // ALL SIZE/XYZI pairs — a multi-model .vox is an animation, each model one frame
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
    models.forEach((m, k) => { if (!m.raw) return;
      const cmap = new Map(), mvox = [];
      for (let i = 0; i < m.raw.length; i += 4) {
        const ci = m.raw[i + 3];
        let cid = cmap.get(ci);
        if (cid === undefined) { cid = edCol(ppal[(ci - 1) * 4], ppal[(ci - 1) * 4 + 1], ppal[(ci - 1) * 4 + 2]); cmap.set(ci, cid); }
        mvox.push(m.raw[i] | (m.raw[i + 1] << 8) | (m.raw[i + 2] << 16) | (cid << 24));   // model z-up → world y at stamp time
      }
      out.push({ sx: m.sx, sy: m.sy, sz: m.sz, vox: mvox, name: name + (models.length > 1 ? ' #' + (k + 1) : ''),
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
  const edBuildFrames = (list, offKey, bakeOff, name) => {   // parse [{name, u8}] → frames with baked + saved offsets + the eye-blink variant. Shared by BOTH lanes. offKey namespaces the per-frame offsets so each bunny variant keeps its OWN positions though the frames share names 00-10.vox. bakeOff = committed default (undefined → cardinal default; object → that variant's bake; null → none). name = model id (gates the eye-blink colour)
    const frames = [];
    for (const f of list) { try { frames.push(...edParseVox(f.u8, f.name)); } catch (e) { console.warn('[vb] editor: bad .vox', f.name, e); } }
    if (!frames.length) return frames;
    const bake = bakeOff === undefined ? { '04.vox': [0, 1, 0], '05.vox': [0, 1, 0] } : bakeOff;   // BAKED alignment applied even with no saved edits; cardinal default for manual imports, each bunny passes its own. A 4th element (array) = the baked ROTATION step list.
    if (bake) for (const f of frames) { const b = bake[f.name]; if (b) { f.ox = b[0]; f.oy = b[1]; f.oz = b[2]; if (Array.isArray(b[3])) f.rot = b[3].slice(); } }
    try { const m = JSON.parse(localStorage.getItem(offKey || 'vb_edoffsets') || '{}'); for (const f of frames) { const o = m[f.name]; if (o) { f.ox = o[0] | 0; f.oy = o[1] | 0; f.oz = o[2] | 0; if (Array.isArray(o[3])) f.rot = o[3].slice(); } } } catch (e) {}   // a live saved edit still overrides the bake (offsets AND rotation)
    for (const f of frames) if (f.rot && f.rot.length) for (const s of f.rot) edRotStep(f, s[0] === 'y' ? 'yaw' : 'pitch', s[1] === '+' ? 1 : -1);   // REPLAY the baked/saved rotation onto the loaded .vox — this is where 'rotation in the bake' happens (before the eye-blink variant is built, so it inherits the rotation)
    for (const f of frames) {                            // EYE BLINK: build a variant where each pitch-black eye voxel is recolored to its NEAREST body-colour voxel — red plumage for the cardinal, TAN for the bunny (user). Same cadence/mechanism as the other blinking life forms.
      const reds = [], body = [], tan = [], blacks = [], blackCols = [];
      for (let i = 0; i < f.vox.length; i++) { const c = palette[f.vox[i] >>> 24]; if (!c) continue;
        if (c[0] < 30 && c[1] < 30 && c[2] < 30) { blacks.push(i); blackCols.push(f.vox[i]); }   // the pitch-black eye — index (to recolour) + colour (so a black-furred model can blink INTO its own black)
        else { body.push(f.vox[i]);
          if (c[0] > 130 && c[0] > c[1] * 1.7 && c[0] > c[2] * 1.7) reds.push(f.vox[i]);   // cardinal plumage / the armadillo's saturated orange shell
          else if (c[0] >= c[1] && c[1] >= c[2] && c[0] - c[2] > 12) tan.push(f.vox[i]); } }   // warm & DESCENDING (r≥g≥b) = tan / light-brown; excludes pink (blue elevated → b>g) and greys
      const cands = name === 'skunk' ? (blackCols.length ? blackCols : body)   // SKUNK eyes blink to the nearest BLACK voxel — its black fur, not the tan fallback (user)
                  : (name === 'armadillo' || name === 'porcupine') ? (tan.length ? tan : body)   // armadillo + porcupine eyes blink to the nearest LIGHT-BROWN voxel — not a red shell nor a pink face voxel (user)
                  : (reds.length ? reds : body);           // cardinal → red plumage; bunny (no red) → nearest body voxel = its tan
      if (blacks.length && cands.length) { const vb = f.vox.slice();
        for (const bi of blacks) { const p = f.vox[bi], ex = p & 255, ey = (p >> 8) & 255, ez = (p >> 16) & 255;
          let best = cands[0], bd = 1e9; for (const r of cands) { const dx = (r & 255) - ex, dy = ((r >> 8) & 255) - ey, dz = ((r >> 16) & 255) - ez, d2 = dx * dx + dy * dy + dz * dz; if (d2 < bd) { bd = d2; best = r; } }
          vb[bi] = (p & 0x00ffffff) | ((best >>> 24) << 24); }
        f.voxBlink = vb; }
    }
    return frames; };
  const edImportBufs = (list, offKey, bakeOff, name) => {   // LEFT/editable lane — resets the scrub + repaints
    const frames = edBuildFrames(list, offKey, bakeOff, name);
    if (!frames.length) return 0;
    ED.offKey = offKey || 'vb_edoffsets'; ED.name1 = name || '';   // remembered so gizmo autosave writes back to the SAME variant's namespace
    ED.frames = frames; ED.sel = 0; ED.paused = false;
    palSync(); edLayout();
    return frames.length; };
  const edImportBufs2 = (list, offKey, bakeOff, name) => {   // RIGHT/preview lane — loads alongside, never touches the primary's scrub/selection
    const frames = edBuildFrames(list, offKey, bakeOff, name);
    ED.frames2 = frames; ED.off2 = offKey || ''; ED.name2 = name || '';
    return frames.length; };
  const edEnter = () => {
    if (ED.on) return;
    ED.on = true; ED.ret = { x: P.x, y: P.y, z: P.z, yaw: P.yaw, pitch: P.pitch, fly: P.fly };   // fly saved too — the editor turns it ON for the framing below, exit restores what the player had
    cmpVis();                                          // hide the top-centre compass while in the editor (user)
    ED.x0 = Math.max(rect.xlo + 4, Math.min(rect.xhi - 4 - ED.pw, Math.round(P.x / 10) * 10 - (ED.pw >> 1)));
    ED.z0 = Math.max(rect.zlo + 4, Math.min(rect.zhi - 4 - ED.pd, Math.round(P.z / 10) * 10 - (ED.pd >> 1)));
    let topMax = WL;                                   // stage must sit above all content on its footprint (its bricks stay PURE — no terrain sharing an 8³ brick with the plane)
    for (let z = ED.z0; z < ED.z0 + ED.pd; z += 2) for (let x = ED.x0; x < ED.x0 + ED.pw; x += 2) {
      const gx = gwrap(x, WX), gz = gwrap(z, WZ), b2 = gx + gz * WX * WY;
      let y = Math.min(WY - 2, hmap[gx + gz * WX] + 118);
      while (y > WL && !W[b2 + y * WX]) y--;
      if (y > topMax) topMax = y;
    }
    ED.y = Math.min(WY - 24, Math.max((topMax + 15) & ~7, WY - 80));   // 8-aligned with ≥8 clearance, high in the sky
    unstampAllWorms();                                 // clear live worms out of W before the editor freezes the world (else edExit's rebuildBricks would resurrect them)
    bricks.fill(0); bricks2.fill(0); wbricks.fill(0);  // SEPARATE LEVEL: empty occupancy = the whole world vanishes from the tracer (W is untouched — exit rebuilds)
    const cells = [];
    for (let z = ED.z0; z < ED.z0 + ED.pd; z++) for (let x = ED.x0; x < ED.x0 + ED.pw; x++) edSet(x, ED.y, z, edPlaneId(x, z), cells);
    gpuPatch(cells, true, cells.length, false);        // track=false: entering the editor overwrites real world cells with the stage plane, and none of that is a terrain edit
    P.x = ED.x0 + ED.pw / 2; P.z = ED.z0 + ED.pd / 2; P.y = ED.y + 2; P.vy = 0; P.fly = true;   // fly ON so the framing below can't be pulled off by gravity while the world is frozen
    smoothEye = P.y + EYE; resetHist = 1;
    if (!ED.frames.length && PORCUPINE_WALK.length) { edImportBufs(PORCUPINE_WALK, 'vb_edoffsets_porcupine', PORCUPINE_BAKES[0], 'porcupine'); ED.bun = null; ED.bunny = false; ED.arm = { hd: 0, px: 0, pz: 0, tRe: 0 }; ED.bakes = PORCUPINE_BAKES; }   // PORCUPINE walk cycle is now the ASSET-EDITOR object (user: swapped the skunk out of the editor — the skunk still spawns in the WORLD, it's just no longer what you edit). Align ONCE in SOUTH; headings auto-derive via armOffset (its own PORCUPINE_BAKES + 'vb_edoffsets_porcupine' namespace)
    else if (!ED.frames.length && SKUNK_WALK.length) { edImportBufs(SKUNK_WALK, 'vb_edoffsets_skunk', SKUNK_BAKES[0], 'skunk'); ED.bun = null; ED.bunny = false; ED.arm = { hd: 0, px: 0, pz: 0, tRe: 0 }; ED.bakes = SKUNK_BAKES; }   // FALLBACK: if the porcupine frames are missing, the skunk is still available in the editor
    else if (!ED.frames.length && BUNNY_JUMP.length) { edImportBufs(BUNNY_JUMP, 'vb_edoffsets_jump', BUNNY_JUMP_BAKE, 'jump'); ED.bunny = false; }   // jump frames only → just hop forward
    else if (!ED.frames.length && BUNNY_ROTATE.length) { edImportBufs(BUNNY_ROTATE, 'vb_edoffsets_rotate', BUNNY_ROT_BAKE, 'rotate'); ED.bunny = false; }   // rotate frames only
    else if (!ED.frames.length && BLUEBIRD_ROTATE.length) { edImportBufs(BLUEBIRD_ROTATE); ED.bunny = false; }   // …or the blue bird if both bunny sets are missing
    // ── FRAME THE OBJECT ── stand the player 5 voxels BACK from the stage centre (where the model is stamped) with the FEET
    // ON THE FLOOR (P.y = ED.y + 1 = plane top), then pitch DOWN to aim the eye at the model's vertical centre so the object
    // stays framed. (The old framing floated the eye at the model's mid-height, which for any model shorter than the 18.5
    // eye height sank the 20-voxel person's feet below the plane — the "clipped through the floor" the user reported.)
    { const bf = ED.frames[0]; const midH = bf ? (bf.sz * 0.5) : 6;
      const cx = ED.x0 + ED.pw / 2, cz = ED.z0 + ED.pd / 2, back = 50;
      P.x = cx; P.z = cz - back;                        // 50 voxels back along −Z (yaw 0 faces +Z → the model) — stands well clear of the object for a fuller view (user)
      P.y = ED.y + 1;                                   // feet planted on the plane (NOT floating at eye-mid-height)
      P.yaw = 0; P.pitch = Math.atan2(midH - EYE, back);   // aim the eye down at the model's vertical centre → the object sits framed in front of the camera
      P.vy = 0; smoothEye = P.y + EYE; resetHist = 1; }
    edBtnEl.classList.add('on'); edRowEl.classList.remove('hidden'); edHudEl.classList.remove('hidden');
    edHudUpd();
  };
  const edExit = () => {
    if (!ED.on) return;
    ED.on = false;
    const cells = [];
    for (const [ii, pv] of ED.prev) if (W[ii] !== pv) { W[ii] = pv; cells.push(ii); }
    ED.prev.clear(); ED.frames = []; ED.frames2 = []; ED.sel = -1; ED.fcells = []; ED.fcells2 = []; ED.ring = []; ED.box = null; ED.box2 = null; ED.bun = null; ED.arm = null; bfly.init = false;   // both lanes + creature AI cleared → no stale editor hitbox lingers after exit
    gpuPatch(cells, true, cells.length, false);        // track=false: this RESTORES the world the editor borrowed — it is a rollback, not an edit
    rebuildBricks(0, WX, 0, WZ); uploadBricks();       // bring the whole world back from the void (occupancy was zeroed on enter)
    const r = ED.ret; if (r) { P.x = r.x; P.y = r.y; P.z = r.z; P.yaw = r.yaw; P.pitch = r.pitch; P.fly = !!r.fly; P.vy = 0; smoothEye = P.y + EYE; resetHist = 1; }
    edBtnEl.classList.remove('on'); edRowEl.classList.add('hidden'); edHudEl.classList.add('hidden');
    cmpVis();                                          // restore the compass (if locked + setting on) now the editor is closed
  };
  edBtnEl.addEventListener('click', (e) => { e.stopPropagation(); ED.on ? edExit() : edEnter(); });
  // (boot straight into the pine forest — the asset editor no longer auto-opens on refresh; open it with the editor button. user 2026-07-22)
  $('edCopy').addEventListener('click', (e) => { e.stopPropagation();   // copy the per-frame offsets → paste back to be baked into the code (replaces the .vox exporter)
    const btn2 = e.currentTarget; if (!ED.frames.length) { btn2.dataset.lbl = 'nothing to copy'; setTimeout(() => { btn2.dataset.lbl = 'export'; }, 1500); return; }   // icon button now — flash the hover LABEL + a green pulse instead of replacing the SVG with text
    edCopyOffsets(); btn2.classList.add('copied'); btn2.dataset.lbl = 'copied ✓ — paste it to me'; setTimeout(() => { btn2.classList.remove('copied'); btn2.dataset.lbl = 'export'; }, 2000); });
  edFileEl.addEventListener('click', (e) => e.stopPropagation());
  edFileEl.addEventListener('change', async () => {
    const list = [];
    for (const f of [...edFileEl.files].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true })))
      list.push({ name: f.name, u8: new Uint8Array(await f.arrayBuffer()) });
    if (list.length) { edClearStamp(); ED.frames2 = []; ED.box2 = null; ED.bun = null; ED.arm = null; edImportBufs(list); ED.bunny = false; }   // a manual .vox import is a clean SINGLE-object edit — clear the second lane + creature AI + armadillo walk so only the imported model shows. edClearStamp runs FIRST, as its comment says: edImportBufs ends in edLayout, which stamps the new model, so clearing afterwards erased what had just been laid out and the import stayed invisible until the next repaint (up to 3.4 s).
    edFileEl.value = '';
  });

