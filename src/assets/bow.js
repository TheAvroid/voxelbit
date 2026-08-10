  // ── ARROW ORIENTATION ── quarter turns about x / y / z, applied to the ARROW ALONE and driven live from
  // the panel in the top right (user). This is the placement baked from that panel, so a cleared profile
  // starts where the tuning left off rather than back at the raw file orientation.
  const ARROW_ROT0 = [0, 2, 2];                        // user bake 2026-08-04
  let ARROW_ROT = (() => { try { const v = JSON.parse(localStorage.getItem('vb_arrowRot') || 'null');
    return (Array.isArray(v) && v.length === 3) ? v.map((n) => (n | 0) & 3) : ARROW_ROT0.slice(); } catch (e) { return ARROW_ROT0.slice(); } })();
  const ARROW_POS_R = 4;                               // how far the position sliders reach, in voxels — the strip's grid is padded for EXACTLY this, so sliding never resizes it
  const ARROW_POS_DEF = () => [[1, 1, 0], [1, 2, 0], [1, 3, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]];   // ONE OFFSET PER DRAW FRAME (user bake 2026-08-04): across one voxel, and stepping further out as the string draws back
  const arrowPosClamp = (v) => [0, 1, 2].map((k) => Math.max(-ARROW_POS_R, Math.min(ARROW_POS_R, (v && v[k]) | 0)));
  let ARROW_POS = (() => { try { const v = JSON.parse(localStorage.getItem('vb_arrowPos') || 'null');
    if (Array.isArray(v) && typeof v[0] === 'number') { const p = arrowPosClamp(v); return ARROW_POS_DEF().map(() => p.slice()); }   // the earlier single offset applied to every frame — carry it across rather than dropping it
    if (Array.isArray(v) && Array.isArray(v[0])) return ARROW_POS_DEF().map((q, i) => arrowPosClamp(v[i] || q));
  } catch (e) {} return ARROW_POS_DEF(); })();
  let bowLock = -1;                                    // which draw frame the held bow is pinned to (-1 = play it live) — , and . step through them
  let arwSync = () => {};                              // repaint the arrow panel onto whichever frame is pinned (assigned with the panel)
  let bowRefit = null;                                 // set once the item table exists: re-cuts the bow strip in place for a new ARROW_ROT / ARROW_POS
  const arrowRotQ = (d, r) => { let x = d[0], y = d[1], z = d[2];   // right-handed quarter turns in the .vox frame: x across, y along the shaft, z up
    for (let i = 0; i < r[0]; i++) { const t = y; y = -z; z = t; }
    for (let i = 0; i < r[1]; i++) { const t = z; z = -x; x = t; }
    for (let i = 0; i < r[2]; i++) { const t = x; x = -y; y = t; }
    return [x, y, z]; };
  // ── THE BOW STRIP ── every frame of the bow, composited with the arrow where the file places it, plus
  // the same frames WITHOUT the arrow so it can leave on release. Both share one grid: the two strips are
  // interchangeable frame for frame, so swapping between them cannot shift the bow by even a voxel.
  const fetchBowStrip = async (url) => {
    try {
      const sc = parseVoxScene(new Uint8Array(await (await fetch(url)).arrayBuffer()), true);
      if (!sc.shapes.length) return null;
      let bow = sc.shapes[0], arrow = null;            // the ANIMATION is the bow; a lone extra shape is the arrow
      for (const sh of sc.shapes) if (sh.ids.length > bow.ids.length) bow = sh;
      for (const sh of sc.shapes) if (sh !== bow && sh.ids.length) { arrow = sh; break; }
      // the bow's own depth per frame — the draw IS this number growing, so it drives both alignments
      const depth = bow.ids.map((mi) => sc.models[mi].sy);
      const d0 = depth[0];
      const place = (sh, mi, sft) => { const m = sc.models[mi], out = []; if (!m || !m.raw) return out;   // a model's voxels in world coords, with a per-frame shift
        const o = sc.org(sh, mi);
        for (let i = 0; i < m.raw.length; i += 4)
          out.push({ x: o[0] + m.raw[i] + sft[0], y: o[1] + m.raw[i + 1] + sft[1], z: o[2] + m.raw[i + 2] + sft[2], c: sc.colId(m.raw[i + 3]) });
        return out; };
      const boxOf = (pts) => { const a = [1e9, 1e9, 1e9], b = [-1e9, -1e9, -1e9];
        for (const v of pts) { const p = [v.x, v.y, v.z];
          for (let k = 0; k < 3; k++) { if (p[k] < a[k]) a[k] = p[k]; if (p[k] > b[k]) b[k] = p[k]; } }
        return [a, b]; };
      // the arrow turned about its OWN box centre. Doubled coordinates keep a quarter turn exact whichever
      // way the box's parity falls — rounding only ever splits a genuine half voxel.
      const spin = (pts, rot) => { const [a, b] = boxOf(pts), c2 = [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
        return pts.map((v) => { const d = arrowRotQ([2 * v.x - c2[0], 2 * v.y - c2[1], 2 * v.z - c2[2]], rot);
          return { x: Math.round((d[0] + c2[0]) / 2), y: Math.round((d[1] + c2[1]) / 2), z: Math.round((d[2] + c2[2]) / 2), c: v.c, a: true }; }); };
      const raw = [];                                // per frame: the bow's cells + the arrow's UNTURNED cells, spun on demand
      for (let fi = 0; fi < bow.ids.length; fi++) {
        // ── FRONT FIXED ── MagicaVoxel CENTRES each frame on the shared node translation, so a 7-deep frame
        // reaches 2 further forward than the 3-deep rest pose: the face of the bow creeps away from you as it
        // draws. Cancel exactly that half-depth growth and the far edge sits still (user):
        //   far edge = t + ceil(d/2) - 1 + shift   →   constant when shift = ceil(d0/2) - ceil(d/2)
        // The old shift (d0 - d) was the STRING's travel, not the front's — DOUBLE the correction needed, and
        // in the wrong place: it dragged the whole bow backwards with the string instead of holding its face.
        const bv = place(bow, bow.ids[fi], [0, Math.ceil(d0 / 2) - Math.ceil(depth[fi] / 2), 0]).map((v) => ({ x: v.x, y: v.y, z: v.z, c: v.c, a: false }));
        const av = [];                                 // …and the ARROW rides the near edge, drawn back with the string — (d0 - d) IS that travel — a notch above the bow's centre line (user)
        if (arrow) for (const ai of arrow.ids) for (const v of place(arrow, ai, [0, d0 - depth[fi], 1])) av.push(v);
        raw.push({ bv, av });
      }
      if (!raw.length) return null;
      // THE GRID IS FIXED. Its dimensions are compiled into the shader (ITEMD) and the held pose is measured
      // from its centre, so turning the arrow must not disturb either: size the grid once for the arrow's
      // WHOLE range of orientations, and keep it centred exactly where the unturned strip's box already was.
      let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
      const grow = (pts, a0, b0) => { const [a, b] = boxOf(pts);
        for (let k = 0; k < 3; k++) { if (a[k] < a0[k]) a0[k] = a[k]; if (b[k] > b0[k]) b0[k] = b[k]; } };
      for (const r of raw) { grow(r.bv, lo, hi); if (r.av.length) grow(spin(r.av, ARROW_ROT0), lo, hi); }
      const c2 = [lo[0] + hi[0], lo[1] + hi[1], lo[2] + hi[2]];   // TWICE the strip's centre — every baked held pose is measured against this point
      const nlo = lo.slice(), nhi = hi.slice();
      for (let rx = 0; rx < 4; rx++) for (let ry = 0; ry < 4; ry++) for (let rz = 0; rz < 4; rz++)
        for (const r of raw) if (r.av.length) grow(spin(r.av, [rx, ry, rz]), nlo, nhi);
      for (let k = 0; k < 3; k++) { nlo[k] -= ARROW_POS_R; nhi[k] += ARROW_POS_R;   // …and room for the sliders' whole travel, so dragging one cannot resize the grid either
        const h = Math.max(c2[k] - 2 * nlo[k], 2 * nhi[k] - c2[k]);                 // doubled half-extent, so the centre cannot drift by half a voxel
        lo[k] = Math.floor((c2[k] - h) / 2); hi[k] = Math.ceil((c2[k] + h) / 2); }
      const sx = hi[0] - lo[0] + 1, sy = hi[1] - lo[1] + 1, sz = hi[2] - lo[2] + 1;   // ONE grid for every frame, both strips AND every arrow orientation + offset
      const build = (rot, pos) => {
        const mk = (f, keepArrow) => ({ sx, sy, sz, vox: f.filter((v) => keepArrow || !v.a)
          .map((v) => (v.x - lo[0]) | ((v.y - lo[1]) << 8) | ((v.z - lo[2]) << 16) | (v.c << 24)) });
        const cells = raw.map((r, fi) => { const p = arrowPosClamp((pos && pos[fi]) || [0, 0, 0]);   // clamped per FRAME: a stray value must never push voxels out of the grid
          return r.bv.concat(r.av.length
            ? spin(r.av, rot).map((v) => ({ x: v.x + p[0], y: v.y + p[1], z: v.z + p[2], c: v.c, a: true })) : []); });
        return { withArrow: cells.map((f) => mk(f, true)), bowOnly: cells.map((f) => mk(f, false)), hasArrow: !!arrow };
      };
      const out = build(ARROW_ROT, ARROW_POS);
      out.rebuild = build; out.dims = [sx, sy, sz];
      return out;
    } catch (e) { console.warn('[vb] bow strip', url, e); return null; }
  };
  const fetchVoxStrip = async (url) => { try { return parseVoxAll(new Uint8Array(await (await fetch(url)).arrayBuffer()), true); } catch (e) { console.warn('[vb] vox strip', url, e); return []; } };   // …EVERY frame in a multi-model .vox (the bow)
  const fetchVoxShared = async (url) => { try { return parseVoxModel(new Uint8Array(await (await fetch(url)).arrayBuffer()), true); } catch (e) { console.warn('[vb] vox', url, e); return null; } };   // …in SHARE mode: an item-only model that must not cost palette entries (the bow strip)
  // SHARE by default (2026-08-05). Every decoration used to mint a fresh palette entry for colours the table
  // already held — the pine browns and foliage greens landed two and three times over, 40 duplicate ids in all,
  // and the table hit its 256 ceiling before the land mammals loaded. Past the ceiling edCol silently
  // NEAREST-MATCHES, which is why the porcupine wore a mushroom's olive and the armadillo's shell gradient
  // scattered. Reusing an identical colour is invisible; what it is NOT safe for is a model whose OWN ids gate a
  // behaviour — PICK_CONE is built from the pinecone's ids, so a shared id there would let one cone pickup flood
  // everything else wearing that colour. Pass own = true to keep dedicated entries, as the pinecone does.
  const fetchVoxModel = async (url, own) => { try { return parseVoxModel(new Uint8Array(await (await fetch(url)).arrayBuffer()), !own); } catch (e) { console.warn('[vb]', url, 'missing — that decoration is skipped', e); return null; } };
  const [CONEV, lilyS, lilyM, lilyL, stk1, stk2, LOGV, ROCKV, PICKV, SHOVV, ARROWV, BOWSTRIP, MEATV, HOEV, SPEARV] = await Promise.all([        // fetched IN PARALLEL — decoration models no longer block one another on boot
    fetchVoxModel('assets/decoration/pinecone.vox', true),                                  // hangs under pine branches — OWN ids: PICK_CONE is built from them, so they must not be shared with anything else
    fetchVoxModel('assets/decoration/lillypad_small.vox'),                                  // floats on lakes + rivers
    fetchVoxModel('assets/decoration/lillypad_medium.vox'),
    fetchVoxModel('assets/decoration/lillypad_large.vox'),
    fetchVoxModel('assets/decoration/stick_1.vox'),                                         // ground scatter, pickable
    fetchVoxModel('assets/decoration/stick_2.vox'),
    fetchVoxModel('assets/decoration/log.vox'),                                             // rare fallen log, solid
    fetchVoxModel('assets/decoration/rock.vox'),                                            // the small stone, pickable
    fetchVoxShared('assets/stone_tools/stone_pick.vox'),                                     // …and the STONE PICK, the rock-breaking counterpart to the axe (user)
    fetchVoxShared('assets/stone_tools/stone_shovel.vox'),
    fetchVoxShared('assets/stone_tools/bow_arrow/arrow.vox'),                                // …and the ARROW that lies on the bow (user)                                   // …and the STONE SHOVEL, which digs ground and nothing else (user)
    fetchBowStrip('assets/stone_tools/bow_arrow/bow/base.vox'),                              // …the BOW: ONE multi-model file whose models are its DRAW FRAMES (user), parsed in
                                                                                            // SHARE mode so the frames' shared colours cost no palette entries
    fetchVoxShared('assets/food/meat/meat.vox'),                                        // …and RAW MEAT, which a killed land mammal leaves behind (user)
    fetchVoxShared('assets/stone_tools/stone_hoe.vox'),                                 // …and the STONE HOE and STONE SPEAR, carried like the rest of the kit (user)
    fetchVoxShared('assets/stone_tools/stone_spear.vox')]);
  const CONEVL = CONEV ? { sx: CONEV.sx, sy: CONEV.sz, sz: CONEV.sy,                        // the same cone tipped 90° onto its side — fallen cones lie on the forest floor
    vox: CONEV.vox.map((p) => (p & 255) | (((p >> 16) & 255) << 8) | ((CONEV.sy - 1 - ((p >> 8) & 255)) << 16) | (p & 0xff000000)) } : null;
  const LILYV = [lilyS, lilyM, lilyL].filter(Boolean);
  const STICKV = [stk1, stk2].filter(Boolean);
  const ROCKVU = ROCKV ? { sx: ROCKV.sx, sy: ROCKV.sz, sz: ROCKV.sy,                        // the same stone tipped 90° onto its edge — half the field stones stand upright
    vox: ROCKV.vox.map((p) => (p & 255) | (((p >> 16) & 255) << 8) | ((ROCKV.sy - 1 - ((p >> 8) & 255)) << 16) | (p & 0xff000000)) } : null;
  let ROCK26 = [], R26S = [], R26M = [], R26B = [];                                         // 26 pre-voxelized rocks from rocks.glb (see scratch voxelize_rocks.py) in 3 rarity tiers
  try {
    const rj = await (await fetch('assets/decoration/rocks26.json')).json();
    const rids = rj.pal.map((c) => addCol(c[0], c[1], c[2]));                               // 12 shared quantized shades for all 26 rocks
    ROCK26 = rj.rocks.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (rids[p >>> 24] << 24)) }));
    rj.rocks.forEach((r, i) => { (r.grp === 'big' ? R26B : (r.grp === 'mid' ? R26M : R26S)).push(i); });
  } catch (e) { console.warn('[vb] rocks26.json missing — GLB rocks skipped', e); }
  let FERN2V = [];                                                                          // the big fern plant from fern.glb (see voxelize_fern2.py), walk-through decor — the ferns_grass clumps were REMOVED 2026-07-16
  try {
    const f2 = await (await fetch('assets/decoration/fern2.json')).json();
    const f2ids = f2.pal.map((c) => addCol(c[0], c[1], c[2]));
    FERN2V = f2.ferns.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (f2ids[p >>> 24] << 24)) }));
  } catch (e) { console.warn('[vb] fern2.json missing — fern decor skipped', e); }
  let MUSHV = null;                                                                          // RARE pine-forest mushroom cluster (mushroom.json from mushrooms.glb, palette-reduced to ≤24 shades — the editor's full-colour mushrooms.vox would blow the shared 256 palette)
  try {
    const mj = await (await fetch('assets/decoration/mushroom.json')).json();
    const mids = mj.pal.map((c) => addCol(c[0], c[1], c[2]));                                // its own quantized shades → global ids
    MUSHV = { sx: mj.sx, sy: mj.sy, sz: mj.sz, vox: mj.vox.map((p) => (p & 0xffffff) | (mids[p >>> 24] << 24)) };
    // ── FILL THE SHELL ── see the block comment convention above: hollow until the axe cut into it (user).
    {
      const SX = MUSHV.sx, SY = MUSHV.sy, SZ = MUSHV.sz, NC = SX * SY * SZ;
      const at = (x, y, z) => x + y * SX + z * SX * SY;
      const cell = new Int32Array(NC).fill(-1);        // -1 empty, else the palette id occupying it
      for (const q of MUSHV.vox) cell[at(q & 255, (q >> 8) & 255, (q >> 16) & 255)] = q >>> 24;
      const OUT = 1, IN = 2;
      const mark = new Uint8Array(NC), st = new Int32Array(NC);
      let sp = 0;
      for (let z = 0; z < SZ; z++) for (let y = 0; y < SY; y++) for (let x = 0; x < SX; x++) {
        if (x && x < SX - 1 && y && y < SY - 1 && z && z < SZ - 1) continue;   // boundary shell only
        const k = at(x, y, z); if (cell[k] >= 0 || mark[k]) continue;
        mark[k] = OUT; st[sp++] = k;
      }
      while (sp > 0) {                                 // 1. reachable-from-outside empty space
        const k = st[--sp], x = k % SX, y = ((k / SX) | 0) % SY, z = (k / (SX * SY)) | 0;
        for (let d = 0; d < 6; d++) {
          const nx = x + (d === 0 ? 1 : d === 1 ? -1 : 0);
          const ny = y + (d === 2 ? 1 : d === 3 ? -1 : 0);
          const nz = z + (d === 4 ? 1 : d === 5 ? -1 : 0);
          if (nx < 0 || nx >= SX || ny < 0 || ny >= SY || nz < 0 || nz >= SZ) continue;
          const nk = at(nx, ny, nz);
          if (cell[nk] >= 0 || mark[nk]) continue;
          mark[nk] = OUT; st[sp++] = nk;
        }
      }
      let cav = 0;
      for (let k = 0; k < NC; k++) if (cell[k] < 0 && !mark[k]) { mark[k] = IN; cav++; }
      if (cav) {
        sp = 0;                                        // 2. grow the shell's colours inward
        for (let k = 0; k < NC; k++) if (cell[k] >= 0) st[sp++] = k;
        let head = 0;
        const q2 = Array.from(st.subarray(0, sp));
        while (head < q2.length) {
          const k = q2[head++], x = k % SX, y = ((k / SX) | 0) % SY, z = (k / (SX * SY)) | 0;
          for (let d = 0; d < 6; d++) {
            const nx = x + (d === 0 ? 1 : d === 1 ? -1 : 0);
            const ny = y + (d === 2 ? 1 : d === 3 ? -1 : 0);
            const nz = z + (d === 4 ? 1 : d === 5 ? -1 : 0);
            if (nx < 0 || nx >= SX || ny < 0 || ny >= SY || nz < 0 || nz >= SZ) continue;
            const nk = at(nx, ny, nz);
            if (mark[nk] !== IN || cell[nk] >= 0) continue;
            cell[nk] = cell[k];                        // nearest surface voxel's colour
            q2.push(nk);
          }
        }
        const filled = [];
        for (let k = 0; k < NC; k++) if (cell[k] >= 0) {
          const x = k % SX, y = ((k / SX) | 0) % SY, z = (k / (SX * SY)) | 0;
          filled.push(x | (y << 8) | (z << 16) | (cell[k] << 24));
        }
        console.log('[vb] mushroom: shell', MUSHV.vox.length, 'vox +', cav, 'interior filled =', filled.length);
        MUSHV.vox = filled;
      }
    }
    // ── ONE BODY PER MUSHROOM, EVERY BASE ON THE GROUND ── mushroom.vox is authored as three separate
    // mushrooms (see tools/voxelize_mushroom.py, which flattens them into this one cluster). Split the
    // cluster into connected components: each is one mushroom, each drops to z = 0 so it starts flush,
    // and each is kept as its own sub-model so stampMush can sit it on ITS OWN terrain height. A single
    // stamp put the whole cluster on one ground plane, which on any slope left the uphill mushrooms
    // buried and the downhill ones perched in the air (user: "the base of every mushroom must touch the
    // ground"). Component-based rather than cap-colour based — a cap colour is shared between mushrooms;
    // being a separate body is what actually tells them apart. Model z is world HEIGHT (MagicaVoxel is z-up).
    {
      const key = (x, y, z) => x | (y << 8) | (z << 16);
      const occ = new Map();
      for (let i = 0; i < MUSHV.vox.length; i++) { const p = MUSHV.vox[i]; occ.set(p & 0xffffff, i); }
      const comp = new Int32Array(MUSHV.vox.length).fill(-1);
      const sizes = [];
      for (let i = 0; i < MUSHV.vox.length; i++) {
        if (comp[i] >= 0) continue;
        const id = sizes.length; let n = 0; const stack = [i];
        comp[i] = id;
        while (stack.length) {
          const j = stack.pop(); n++;
          const p = MUSHV.vox[j], x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
          const nb = [key(x + 1, y, z), key(x - 1, y, z), key(x, y + 1, z), key(x, y - 1, z), key(x, y, z + 1), key(x, y, z - 1)];
          for (const nk of nb) { const m = occ.get(nk); if (m !== undefined && comp[m] < 0) { comp[m] = id; stack.push(m); } }
        }
        sizes.push(n);
      }
      // Drop EVERY body until it RESTS ON z = 0 — no exemption for the biggest. A body that keeps an authored
      // height is a mushroom left hanging, which is the whole complaint. Shifting by the body's own lowest
      // voxel plants it exactly touching and leaves an already-grounded one alone.
      const minZ = new Array(sizes.length).fill(255);
      for (let i = 0; i < MUSHV.vox.length; i++) { const z = (MUSHV.vox[i] >> 16) & 255; if (z < minZ[comp[i]]) minZ[comp[i]] = z; }
      for (let i = 0; i < MUSHV.vox.length; i++) {
        if (!minZ[comp[i]]) continue;                            // already at z=0
        const p = MUSHV.vox[i], z = (p >> 16) & 255;
        MUSHV.vox[i] = (p & ~0xff0000) | ((z - minZ[comp[i]]) << 16);
      }
      // Each body becomes its OWN sub-model, but declared in the CLUSTER's frame (same sx/sy, voxels left at
      // their cluster coordinates). stampModel rotates off sx/sy, so every body then rotates through exactly
      // the arithmetic the merged model used — no box-parity correction to get wrong (see the armOffset saga).
      // The only thing that differs per body is the height it is stamped at. cx/cy is the body's own centre,
      // which is all stampMush needs to ask the terrain how high the ground is under THAT mushroom.
      const bx0 = new Array(sizes.length).fill(255), bx1 = new Array(sizes.length).fill(0);
      const by0 = new Array(sizes.length).fill(255), by1 = new Array(sizes.length).fill(0);
      const bz1 = new Array(sizes.length).fill(0), bvox = [];
      for (let c = 0; c < sizes.length; c++) bvox.push([]);
      for (let i = 0; i < MUSHV.vox.length; i++) {
        const p = MUSHV.vox[i], c = comp[i], x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
        if (x < bx0[c]) bx0[c] = x; if (x > bx1[c]) bx1[c] = x;
        if (y < by0[c]) by0[c] = y; if (y > by1[c]) by1[c] = y;
        if (z > bz1[c]) bz1[c] = z;
        bvox[c].push(p);
      }
      MUSHV.bodies = [];
      for (let c = 0; c < sizes.length; c++) MUSHV.bodies.push({ sx: MUSHV.sx, sy: MUSHV.sy, sz: bz1[c] + 1,
        cx: bx0[c] + ((bx1[c] - bx0[c] + 1) >> 1), cy: by0[c] + ((by1[c] - by0[c] + 1) >> 1),
        // …and the radius groundMin should probe: the body's OWN half-footprint, the same rule stampBoulder
        // uses. A fixed radius samples a cross that misses a wide body's corners, so a mushroom straddling a
        // 2-voxel step still ended up hanging over the low side (measured: 3 voxels of air under its base).
        br: Math.max(1, Math.min(10, Math.max(bx1[c] - bx0[c] + 1, by1[c] - by0[c] + 1) >> 1)), vox: bvox[c] });
      console.log('[vb] mushroom cluster:', sizes.length, 'mushrooms —',
        MUSHV.bodies.map((b, c) => sizes[c] + 'vox h' + b.sz + (minZ[c] ? ' (dropped ' + minZ[c] + ')' : '')).join(', '));
    }
  } catch (e) { console.warn('[vb] mushroom.json missing — forest mushrooms skipped', e); }
  let LILYPAD_GIGV = null;                                                                   // GIGANTIC lilypad on lakes (lillypad_gigantic.json, palette-reduced + laid flat) — solid, 1-2 per lake
  try {
    const gj = await (await fetch('assets/decoration/lillypad_gigantic.json')).json();
    const lilyCols = [...new Set(LILYV.flatMap((m) => m.vox.map((p) => p >>> 24)))].map((id) => palette[id]);   // the REGULAR lilypads' mint-green swatches (user: 'same shade as the other lillypads')
    const gcache = new Map();                                                                // snap each giant-pad shade to the nearest regular-lily colour, addCol'd to its OWN id so markSolid never makes the small pads solid too
    const gids = gj.pal.map((c) => { let bd = 1e18, m = c;
      for (const q of lilyCols) { const d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]); if (d < bd) { bd = d; m = q; } }
      const k = (m[0] << 16) | (m[1] << 8) | m[2]; if (!gcache.has(k)) gcache.set(k, addCol(m[0], m[1], m[2])); return gcache.get(k); });
    LILYPAD_GIGV = { sx: gj.sx, sy: gj.sy, sz: gj.sz, vox: gj.vox.map((p) => (p & 0xffffff) | (gids[p >>> 24] << 24)) };
  } catch (e) { console.warn('[vb] lillypad_gigantic.json missing — giant pads skipped', e); }
  const FERNIDS = [...new Set(FERN2V.flatMap((m) => m.vox.map((p) => p >>> 24)))];          // soft-decor id list for the pick-passthru + snow-bury sets
  if (MUSHV) for (const q of MUSHV.vox) decorTab[q >>> 24] = 1;                             // ── CHOPPABLE DECOR ── mushrooms…
  for (const i of FERNIDS) decorTab[i] = 1;                                                 // …and ferns (user): any tool takes chunks out of both
