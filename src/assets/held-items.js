  // ── HELD ITEMS ── everything carried renders through ONE system: TRUE 3D voxel grids (never flattened,
  // the old engine's buildHeldFrames approach) DDA-raytraced in camera space. Item ids in u.pickX.w:
  // 0 = empty hand, 1 = stone axe (.vox), 2 = pebble rock, 3 = twig. Add future pickups to `items`.
  let pickWGSL = 'const ITEMN : i32 = 0; const PICKSTEPS : i32 = 1; const TRA_LO : i32 = 536870912; const TRA_HI : i32 = -1; const TRA2_LO : i32 = 536870912; const TRA2_HI : i32 = -1;\n    const ITEMD : array<vec4<i32>, 1> = array<vec4<i32>, 1>(vec4<i32>(0));\n    @group(0) @binding(13) var<storage, read> ITEMMAP : array<vec4<f32>>;';
  let itemHalfH = null;                              // per-item half height in voxels — a settled drop rests its BOTTOM on the ground, so it needs its own size
  let itemMapF32 = new Float32Array(4);                // ITEMMAP lives in a STORAGE BUFFER (binding 13) — as a var<private> constant array it silently broke past ~2.6k entries (the butterfly frames pushed it over; FXC-side limit, no compile error)
  // ── THE TWO FRUIT, AND THE STRIP THEY ARE EATEN THROUGH ── each is a RUN of FOOD_EAT_N consecutive item ids
  // exactly as the bow's draw is: APPLE_IT + f is frame f, and APPLE_IT itself (frame 0) is the whole fruit, so
  // the thing that sits in a hotbar slot and the first frame of the animation are the SAME id and cannot drift
  // apart. Declared out here rather than inside the items block for the reason TILL_ID is declared in
  // sim/hands.js: the block below assigns them, and every reader (sim/vitals.js's food table, ui/audio.js's
  // bite, main/tick-camera.js's frame pick, ui/hud.js's pose) is in a later fragment that needs the binding
  // itself, not a copy of the 0 it started as.
  let APPLE_IT = 0, ORANGE_IT = 0, FOOD_EAT_N = 0;
  {
    const items = [];                                  // {w, d, h, cells: [[r,g,b] | null]} — x = width, y = depth, z = height
    // ── PARALLEL PREFETCH ── kick EVERY creature/tool .vox fetch off at once so the network runs concurrently (was ~50 serial round-trips, one await per frame — the slow part of boot).
    const AB = {};                                     // url → Promise<ArrayBuffer|null>; the parse loops below just `await AB[url]` (already in flight) and still append in deterministic item-id order.
    const pf = (u) => { AB[u] = fetch(u).then((r) => r.ok ? r.arrayBuffer() : null).catch(() => null); return u; };
    const abuf = async (u) => new Uint8Array((AB[u] ? await AB[u] : await (await fetch(u).catch(() => ({ arrayBuffer: () => null }))).arrayBuffer()) || new ArrayBuffer(0));
    pf('assets/stone_tools/stone_axe.vox'); pf('assets/stone_tools/stone_knife.vox'); pf('assets/single.vox');
    for (let f = 0; f < 8; f++) pf('assets/life/cardinal/flight/0' + f + '.vox');
    for (const c of ['orange', 'red', 'blue', 'lime', 'pink', 'purple']) for (let f = 0; f < 8; f++) pf('assets/life/butterfly/' + c + '/0' + f + '.vox');   // prefetch must list the SAME colours as the loader below or the new ones stall on a cold fetch
    for (let f = 0; f < 6; f++) pf('assets/life/dragonfly/0' + f + '.vox');   // dragonfly ships 6 frames (base.vox is the source art, not a frame — skipped)
    for (let f = 0; f < 13; f++) pf('assets/food/apple/' + String(f).padStart(2, '0') + '.vox');   // the apple EATING strip — 13 on disk today; the loader still walks until one is missing, so this number only decides how many arrive warm
    for (let f = 0; f < 4; f++) pf('assets/life/firefly/0' + f + '.vox');
    for (let f = 0; f < 12; f++) pf('assets/life/worm/' + String(f).padStart(2, '0') + '.vox');
    for (const sp of ['salmon', 'minnow']) for (let f = 0; f < 12; f++) pf('assets/life/' + sp + '/' + String(f).padStart(2, '0') + '.vox');   // fish swim strips (base.vox is source art, skipped); species without frames simply don't join
    for (let f = 0; f < 11; f++) pf('assets/life/cardinal/rotate/' + String(f).padStart(2, '0') + '.vox');
    pf('assets/life/duck/base.vox'); pf('assets/life/duck/baby.vox');
    // ── THE DESERT SET, BY MANIFEST ── this used to prefetch a blind 20 frames per species. Only 58 of those
    // 140 files exist, so every boot spent 82 round-trips collecting 404s — and they were also the bulk of the
    // 97 "expected" console errors that made every test's console check unreadable. tools/bake_desert_life.py
    // now writes desert_frames.json beside the frames it bakes, so the count cannot drift from the files.
    // The manifest is ONE extra request and it is awaited, because the prefetch below needs its numbers; if it
    // is missing we fall straight back to the old blind 20 and nothing breaks.
    // ── THE DESERT BAND'S LOAD ORDER, IN ONE PLACE ── it used to be written out twice, once for the prefetch
    // and once for the loader forty lines down, with a comment on each saying they must agree. They are the
    // same list now, because the failure mode of them disagreeing is silent: a species missing from the
    // prefetch still loads, one cold serial round-trip per frame, and only shows up as a slower boot.
    // ORDER IS LOAD ORDER IS SLOT-BAND ORDER (species index = position in DESERTS), so this is APPEND-ONLY —
    // it must stay in step with NAMES in tools/bake_desert_life.py. The last two are not desert creatures at
    // all: the bee and the grass snake live in the OAK FOREST and only ride this band's machinery.
    const DES_LOAD = ['ant', 'cobra', 'desert_mouse', 'fly', 'gecko', 'scorpion', 'spider', 'bee', 'grass_snake'];
    let desFrames = null;
    try { desFrames = await (await fetch('assets/life/desert_frames.json')).json(); } catch (e) { desFrames = null; }
    for (const sp of DES_LOAD)
      for (let f = 0, nf = (desFrames && desFrames[sp]) || 20; f < nf; f++) pf('assets/life/' + sp + '/' + String(f).padStart(2, '0') + '.vox');   // the desert set — the prefetch must list the same names the loader walks or every frame stalls on a cold fetch
    try {
      const pv = await abuf('assets/stone_tools/stone_axe.vox');
      const pdv = new DataView(pv.buffer);
      let psx = 0, psy = 0, psz = 0, pvox = null; const ppal = new Uint8Array(1024);
      const pwalk = (off, end) => { while (off < end) {
        const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
        const sz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
        if (id === 'SIZE' && !psx) { psx = pdv.getUint32(off + 12, true); psy = pdv.getUint32(off + 16, true); psz = pdv.getUint32(off + 20, true); }
        else if (id === 'XYZI' && !pvox) { const n = pdv.getUint32(off + 12, true); pvox = pv.subarray(off + 16, off + 16 + n * 4); }
        else if (id === 'RGBA') ppal.set(pv.subarray(off + 12, off + 12 + 1024));
        else if (id === 'MAIN') { pwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
        off += 12 + sz + csz;
      } };
      pwalk(8, pv.length);
      const cells3 = new Array(psx * psy * psz).fill(null);           // FULL 3D grid — model x → width, model y → depth, model z → height; every layer kept
      for (let i = 0; i < pvox.length; i += 4) {
        const ci = pvox[i + 3];
        cells3[pvox[i] + pvox[i + 1] * psx + pvox[i + 2] * psx * psy] = [ppal[(ci - 1) * 4], ppal[(ci - 1) * 4 + 1], ppal[(ci - 1) * 4 + 2]];
      }
      items.push({ w: psx, d: psy, h: psz, cells: cells3 });
      // ── PIN THE HELD TOOL'S COLOURS INTO THE PALETTE, EXACTLY ── this model is stored as RAW RGB cells and
      // never goes through palShare, so the noTol exemption that keeps the rest of the stone kit exact cannot
      // reach it: its colours only enter the table later, through edCol, which DOES apply the tolerance. At
      // PAL_TOL 8 that put one axe shade 7/255 off. Minting them here, before any loader can tolerance-match
      // them, means edCol's exact lookup finds them and the tool the player stares at stays byte-accurate.
      // Only colours the table does not already hold cost anything — usually one or two.
      { const seen = new Set();
        for (const c3 of cells3) { if (!c3) continue;
          const k3 = (c3[0] << 16) | (c3[1] << 8) | c3[2]; if (seen.has(k3)) continue; seen.add(k3);
          let have = false;
          for (let i3 = 1; i3 < palette.length && !have; i3++) { const q3 = palette[i3];
            if (q3 && q3[0] === c3[0] && q3[1] === c3[1] && q3[2] === c3[2]) have = true; }
          if (!have && palette.length < 256) addCol(c3[0], c3[1], c3[2]);
        } }
      console.log('[vb] stone_axe.vox', psx, psy, psz, 'voxels', pvox.length / 4);
    } catch (e) { console.warn('[vb] stone_axe.vox missing — held tool disabled', e); items.push({ w: 0, d: 0, h: 0, cells: [] }); }
    const modelToItem = (m) => { const cells = new Array(m.sx * m.sy * m.sz).fill(null);   // sparse decoration model → dense item grid (held/drop DDA wants the raw RGB)
      for (const p of m.vox) cells[(p & 255) + ((p >> 8) & 255) * m.sx + ((p >> 16) & 255) * m.sx * m.sy] = palette[p >>> 24];
      return { w: m.sx, d: m.sy, h: m.sz, cells }; };
    ROCK_IT = items.length + 1;                                       // held field-stone id (1-based) — used to bake AO + gate its stony grain so it reads like a static world rock
    if (ROCKV) items.push(modelToItem(ROCKV));                        // ROCK — the exact field stone (rock.vox), held as picked up
    else { const dk = [103, 101, 97], lt = [122, 120, 114];           // fallback: the old 3×3 pebble slab
      const cells = new Array(3 * 3 * 2).fill(null);
      for (let y = 0; y < 3; y++) for (let x = 0; x < 3; x++) cells[x + y * 3] = dk;
      cells[1 + 1 * 3 + 9] = lt;
      items.push({ w: 3, d: 3, h: 2, cells }); }
    STICK_IT = items.length + 1;                                      // held stick id — AO + grain so it reads like a static twig
    if (STICKV.length) items.push(modelToItem(STICKV[0]));            // STICK — stick_1.vox, held as picked up
    else { const ca = [126, 95, 59], cb = [111, 83, 52];              // fallback: the old 7-voxel twig
      const cells = new Array(7 * 1 * 2).fill(null);
      for (let x = 0; x < 7; x++) { if (x < 5) cells[x] = (x & 1) ? cb : ca; else cells[x + 7] = (x & 1) ? cb : ca; }
      items.push({ w: 7, d: 1, h: 2, cells }); }
    CONE_IT = items.length + 1;                                       // held pinecone id — AO + grain so it reads like a static cone
    items.push(CONEV ? modelToItem(CONEV) : { w: 0, d: 0, h: 0, cells: [] });   // PINECONE (item 4) — pickable ground/tree cones; keep the slot even if the .vox is missing so ids stay stable
    SPARK_IT = items.length + 1;                                      // CLASH SPARK — one bright 10 cm voxel, rendered through the drops path (never in inventory)
    items.push({ w: 1, d: 1, h: 1, cells: [[255, 208, 112]] });
    HITRED_IT = items.length + 1;                                     // ── HIT VOXEL (user 2026-08-05) ── striking a life form throws RED voxels rather than the rocks' amber embers. Identical particle in every other way — same arc, spin and lifetime — so the DEATH poof can keep the original sparks.
    items.push({ w: 1, d: 1, h: 1, cells: [[222, 38, 30]] });
    SMOKE_IT = items.length + 1;                                      // DEATH SMOKE — one white voxel (like a snowflake is one voxel). Each smoke SLOT is an INDIVIDUAL voxel at its own continuous position + snowflake spin (user: "move off the grid like the snowflakes"), rendered TRANSLUCENT (20%) through the drops path (never in inventory)
    items.push({ w: 1, d: 1, h: 1, cells: [[255, 255, 255]] });
    FOAM_IT = items.length + 1;                                       // ── SPLASH DROPLET (user 2026-08-05) ── the same particle as a spark, in FOAM: thrown when a fish or the player breaks the surface either way
    items.push({ w: 1, d: 1, h: 1, cells: [FOAM_SRGB.slice()] });
    KNIFE_IT = items.length + 1;                                      // STONE KNIFE — born when two rocks are clashed together
    try {
      const kv = await abuf('assets/stone_tools/stone_knife.vox');
      const kdv = new DataView(kv.buffer);
      let ksx = 0, ksy = 0, ksz = 0, kvox = null; const kpal = new Uint8Array(1024);
      const kwalk = (off, end) => { while (off < end) {
        const id = String.fromCharCode(kv[off], kv[off + 1], kv[off + 2], kv[off + 3]);
        const sz = kdv.getUint32(off + 4, true), csz = kdv.getUint32(off + 8, true);
        if (id === 'SIZE' && !ksx) { ksx = kdv.getUint32(off + 12, true); ksy = kdv.getUint32(off + 16, true); ksz = kdv.getUint32(off + 20, true); }
        else if (id === 'XYZI' && !kvox) { const n = kdv.getUint32(off + 12, true); kvox = kv.subarray(off + 16, off + 16 + n * 4); }
        else if (id === 'RGBA') kpal.set(kv.subarray(off + 12, off + 12 + 1024));
        else if (id === 'MAIN') { kwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
        off += 12 + sz + csz;
      } };
      kwalk(8, kv.length);
      const kcells = new Array(ksx * ksy * ksz).fill(null);            // raw .vox palette, full 3D grid — same treatment as the axe
      for (let i = 0; i < kvox.length; i += 4) { const ci = kvox[i + 3]; kcells[kvox[i] + kvox[i + 1] * ksx + kvox[i + 2] * ksx * ksy] = [kpal[(ci - 1) * 4], kpal[(ci - 1) * 4 + 1], kpal[(ci - 1) * 4 + 2]]; }
      items.push({ w: ksx, d: ksy, h: ksz, cells: kcells });
      console.log('[vb] stone_knife.vox', ksx, ksy, ksz, 'voxels', kvox.length / 4);
    } catch (e) { console.warn('[vb] stone_knife.vox missing — clash still sparks, no knife', e); items.push({ w: 0, d: 0, h: 0, cells: [] }); }
    // STONE PICK (user) — the rock tool. APPENDED here, after every fixed id: 1-4 are hard-coded in
    // PICK_DEFS/ITEM_NAMES, so slotting it in among them would silently hand the twig the pinecone's pose.
    if (PICKV) { PICK_IT = items.length + 1; items.push(modelToItem(PICKV)); console.log('[vb] stone_pick.vox item', PICK_IT, PICKV.sx, PICKV.sy, PICKV.sz); }
    if (ARROWV) { ARROW_IT = items.length + 1; items.push(modelToItem(ARROWV)); console.log('[vb] arrow item', ARROW_IT); }
    if (SHOVV) { SHOVEL_IT = items.length + 1; items.push(modelToItem(SHOVV)); console.log('[vb] stone_shovel.vox item', SHOVEL_IT, SHOVV.sx, SHOVV.sy, SHOVV.sz); }
    if (BOWSTRIP && BOWSTRIP.withArrow && BOWSTRIP.withArrow.length) {
      // TWO consecutive runs of the same length: BOW_IT + f is frame f WITH the arrow on it, BOW_NOCK + f
      // is the identical frame without it. Loosing the arrow is a swap between the two (user).
      BOW_IT = items.length + 1;
      for (const bv of BOWSTRIP.withArrow) items.push(modelToItem(bv));
      BOW_NOCK = items.length + 1;
      for (const bv of BOWSTRIP.bowOnly) items.push(modelToItem(bv));
      BOW_FRAMES = BOWSTRIP.withArrow.length;
      console.log('[vb] bow', BOW_IT, 'nocked strip + bare strip at', BOW_NOCK, 'frames', BOW_FRAMES, 'arrow in file:', BOWSTRIP.hasArrow, 'palette', palette.length);
    }
    if (false) { const strip = [];
      void strip; }                                  // (superseded: the scene builder above already puts every frame in one shared grid)
    // ══ EATEN DOWN TO A REMNANT ══ one carved strip, and EVERY food takes it: the two fruit below, and the
    // steak (user 2026-08-17: "can you do the same animation to the meat like you did for the apple/orange").
    // The whole model is frame 0 — which is why it doubles as the held item, exactly as apple/00.vox did — and
    // each frame after it is missing a little more of the artist's own voxels.
    // HOW THE CARVE WORKS. A bite point at the top corner of the model's own box, every voxel ordered by
    // distance from it, and frame f missing the nearest f/(N-1) of the ones that go. The model is eaten INWARD
    // FROM ONE SIDE down to a remnant rather than dissolving evenly: on the apple that means the blade goes
    // with the first mouthful while the brown stalk, at the far corner from the bite, survives to the end, so
    // what is left still reads as a core; on the steak it is a flat slab nibbled in from one corner.
    // It costs ZERO palette entries either way — every frame is a copy of cells the model already resolved.
    // EAT_N is the frame count and the ONE number ui/audio.js's EAT_FPS clock walks, so every food is eaten on
    // the same beat. The BOX is per model and defaults to the model's own: the held DDA centres a model on its
    // box, so the box is what decides where a thing hangs in your hand, and changing it would move an item
    // whose pose is already baked. Only the fruit pass one in (see FRUIT_BOX).
    const EAT_N = 13, EAT_KEEP = 0.28;                 // …and how much is still in your hand on the LAST frame. A remnant rather than an empty grid: a thing that vanishes a frame early reads as a dropped item
    FOOD_EAT_N = EAT_N;                                // the strip length every edible shares — set here, not inside the fruit block, so a missing apple cannot silently take the meat's animation with it
    const eatStrip = (m, box) => {                     // one whole model -> EAT_N frames of it being eaten
      const cl = [];                                   // its voxels, in one list, so the bite order is a sort rather than a rule repeated per frame
      for (let z = 0; z < m.h; z++) for (let y = 0; y < m.d; y++) for (let x = 0; x < m.w; x++) {
        const c = m.cells[x + y * m.w + z * m.w * m.d]; if (c) cl.push({ x, y, z, c });
      }
      const bx = m.w - 1, by = (m.d - 1) / 2, bz = m.h - 1;
      for (const q of cl) q.d2 = (q.x - bx) * (q.x - bx) + (q.y - by) * (q.y - by) + (q.z - bz) * (q.z - bz);
      cl.sort((a, b) => a.d2 - b.d2 || a.z - b.z || a.x - b.x || a.y - b.y);   // ties broken on the coordinates so the strip is identical every boot — a sort that is not total is a thing eaten differently each time
      const gone = Math.max(0, cl.length - Math.max(3, Math.round(cl.length * EAT_KEEP)));
      const bb = box || [m.w, m.d, m.h];
      const W = Math.max(bb[0], m.w), D = Math.max(bb[1], m.d), H = Math.max(bb[2], m.h);
      const out = [];
      for (let f = 0; f < EAT_N; f++) {
        const eaten = EAT_N < 2 ? 0 : Math.round(gone * f / (EAT_N - 1));
        const cells = new Array(W * D * H).fill(null);
        for (let i = eaten; i < cl.length; i++) { const q = cl[i]; cells[q.x + q.y * W + q.z * W * D] = q.c.slice(); }
        out.push({ w: W, d: D, h: H, cells });
      }
      return out;
    };
    if (MEATV) { const ms = eatStrip(modelToItem(MEATV));   // …its OWN box, so the steak hangs exactly where its baked pose already puts it
      MEAT_IT = items.length + 1; for (const it of ms) items.push(it);
      console.log('[vb] raw_meat items', MEAT_IT, '..', MEAT_IT + FOOD_EAT_N - 1, MEATV.sx, MEATV.sy, MEATV.sz,
        ms[0].cells.filter(Boolean).length + '->' + ms[ms.length - 1].cells.filter(Boolean).length, 'vox'); }   // …appended after the pick, same reason: ids 1-4 are hard-coded elsewhere
    // …and the HOE and SPEAR (user), appended last of the tools so no existing id moves. Poses are stored
    // by NAME now, so the table can grow safely, but leaving the bow strip's run where it is costs nothing.
    if (HOEV) { HOE_IT = items.length + 1; items.push(modelToItem(HOEV)); console.log('[vb] stone_hoe.vox item', HOE_IT, HOEV.sx, HOEV.sy, HOEV.sz); }
    if (SPEARV) { SPEAR_IT = items.length + 1; items.push(modelToItem(SPEARV)); console.log('[vb] stone_spear.vox item', SPEAR_IT, SPEARV.sx, SPEARV.sy, SPEARV.sz); }
    // ── THE APPLE AND THE ORANGE, AND THE STRIP THEY ARE EATEN THROUGH (user 2026-08-17: "have the player able
    // to right click an apple from a tree and pick it up … then the player can right click to eat it … play the
    // apple eating animation as there should already be one") ── the animation DID already exist and nothing in
    // the game referenced it: assets/food/apple/00.vox … 12.vox, thirteen frames of an apple being eaten down to
    // a core while its leaf tumbles away. Frame 00 is the whole fruit, which is why it doubles as the held model
    // — one id for "an apple in your hand" and "frame zero of eating one" means they can never disagree.
    //
    // PARSED RAW, LIKE THE AXE AND THE KNIFE, NOT THROUGH modelToItem. Two things follow from that and both are
    // the point. First, it costs ZERO palette entries: an item is a grid of raw RGB in ITEMMAP and never asks
    // for an id at all, so all eleven authored shades arrive exactly as painted on a table with one slot left.
    // Second — and this is the user's other note today — the artist's BROWN STALK (143,95,74) survives here even
    // though the world fruit's stalk is still stuck on the oak leaf id: fruit.json pooled stem and leaf into one
    // colour and the palette has no brown that is also canopy (see the argument in assets/bow.js). So the apple
    // you hold has the brown stem the user asked for, today, at no cost.
    //
    // THERE IS ALSO A game/assets/food/apple/eat.b64.js — DELIBERATELY UNUSED. It is a base64 copy of these same
    // thirteen files as a classic script, and it is one of TWENTY-ONE .b64.js files in the asset tree (chickens,
    // tropical fish, a fly) that NOTHING in src/ loads or has ever loaded. The game is served, not opened over
    // file://, and index.html is a single `type="module"`, so a classic script tag would be a second loading
    // mechanism for one asset. The .vox files are fetched exactly as every other frame strip is.
    //
    // THE ORANGE HAS ART OF ITS OWN and now uses it — assets/food/orange.vox, parsed by the same rawItem
    // below and carved into its own eat strip. It used to be the apple's thirteen frames with the flesh
    // re-tinted, on the argument that both fruit are authored on one 4x3x5 grid and share their crown cells
    // voxel for voxel; the user's answer to that is the whole of the change (see the block at the orange
    // itself). Nothing about the APPLE moved: it is the same thirteen files, parsed the same way, padded to
    // the same box.
    { const rawItem = (b) => {                         // one .vox -> {w,d,h,cells} of raw RGB. Mints nothing; a missing or unreadable file answers null, which is how the walk below finds the end of the strip
        if (!b || b.length < 16) return null;
        try {
          const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
          let sx = 0, sy = 0, sz = 0, vox = null; const pal = new Uint8Array(1024);
          const walk = (off, end) => { while (off + 12 <= end) {
            const id = String.fromCharCode(b[off], b[off + 1], b[off + 2], b[off + 3]);
            const sz9 = dv.getUint32(off + 4, true), csz = dv.getUint32(off + 8, true);
            if (id === 'SIZE' && !sx) { sx = dv.getUint32(off + 12, true); sy = dv.getUint32(off + 16, true); sz = dv.getUint32(off + 20, true); }
            else if (id === 'XYZI' && !vox) { const n = dv.getUint32(off + 12, true); vox = b.subarray(off + 16, off + 16 + n * 4); }
            else if (id === 'RGBA') pal.set(b.subarray(off + 12, off + 12 + 1024));
            else if (id === 'MAIN') { walk(off + 12 + sz9, off + 12 + sz9 + csz); off += 12 + sz9 + csz; continue; }
            off += 12 + sz9 + csz;
          } };
          walk(8, b.length);
          if (!sx || !vox || !vox.length) return null;
          if (sx * sy * sz > 1 << 20) return null;     // the walk that finds the end of the strip runs one COLD fetch past the last frame, and a 404 answers with the dev server's HTML rather than nothing — so a stray "SIZE" in a body that is not a .vox must not be allowed to size an allocation. Every real frame here is under 200 cells
          const cells = new Array(sx * sy * sz).fill(null);
          for (let i = 0; i < vox.length; i += 4) { const ci = vox[i + 3];
            cells[vox[i] + vox[i + 1] * sx + vox[i + 2] * sx * sy] = [pal[(ci - 1) * 4], pal[(ci - 1) * 4 + 1], pal[(ci - 1) * 4 + 2]]; }
          return { w: sx, d: sy, h: sz, cells };
        } catch (e) { return null; }                   // a 404 body (the dev server answers with HTML) parses to nothing and ends the walk, which is the intended reading
      };
      // ── ONE STRIP BUILDER, AND BOTH FRUIT GO THROUGH IT (user 2026-08-17: "have the apple follow the
      // same eating animation as the orange") ── the apple used to be thirteen AUTHORED frames
      // (apple/00..12.vox: a bite, then the leaf tumbling away from a core) and the orange was carved out of
      // its one model, so the two fruit were eaten in visibly different ways. Now there is one rule and both
      // take it: the WHOLE fruit is the artist's single model, and the strip is carved out of that model's
      // own voxels. apple/01..12.vox are no longer read by anything — they are left in the asset tree rather
      // than deleted, like the eat.b64.js beside them, so the authored take is still there to go back to.
      //
      // HOW THE CARVE WORKS. A bite point at the top corner of the model's own box, every voxel ordered by
      // distance from it, and frame f missing the nearest f/(N-1) of the ones that go. The fruit is eaten
      // INWARD FROM ONE SIDE down to a remnant rather than dissolving evenly, the crown goes with the first
      // mouthful, and on the apple the brown stalk — which sits at the far corner from the bite — survives
      // to the last frame, so the thing left in your hand still reads as a core.
      // Every voxel in it is the artist's, at the colour they painted, and the whole strip costs ZERO
      // palette entries: an item is raw RGB in ITEMMAP and never asks for an id (see rawItem above).
      //
      // THE TWO NUMBERS THAT ARE NOT THE ART'S:
      //   FRUIT_BOX is the grid every frame is padded into, and the ONE thing the fruit do not take from
      //     eatStrip's default. The held DDA centres a model on its box
      //     (`+ vec3(hw, hd, hh)` in COMPOSITE), so the box is what decides WHERE the fruit hangs in your
      //     hand — and 7x3x6 is the envelope the apple's authored strip needed when its leaf tumbled clear
      //     of the fruit, which is the grid the held poses in ui/hud.js were baked against. Both models are
      //     4x3x5 with the ball at the origin, so it would be tidier to shrink it — and that would slide
      //     both fruit a voxel and a half across the hand for a change nobody asked for. It is WIDENED,
      //     never clipped, if either model is ever re-authored bigger.
      const FRUIT_BOX = [7, 3, 6];
      const apb = rawItem(await abuf('assets/food/apple/00.vox')), orb = rawItem(await abuf('assets/food/orange.vox'));
      if (apb && orb) {
        const ap = eatStrip(apb, FRUIT_BOX), or9 = eatStrip(orb, FRUIT_BOX);
        APPLE_IT = items.length + 1; for (const it of ap) items.push(it);
        ORANGE_IT = items.length + 1; for (const it of or9) items.push(it);
        const nvox = (st) => st[0].cells.filter(Boolean).length + '->' + st[st.length - 1].cells.filter(Boolean).length;
        console.log('[vb] food: apple items', APPLE_IT, '..', APPLE_IT + FOOD_EAT_N - 1, '| orange', ORANGE_IT, '..', ORANGE_IT + FOOD_EAT_N - 1,
          '—', FOOD_EAT_N, 'carved eat frames on', ap[0].w + 'x' + ap[0].d + 'x' + ap[0].h,
          '| apple.vox', apb.w + 'x' + apb.d + 'x' + apb.h, nvox(ap), 'vox | orange.vox', orb.w + 'x' + orb.d + 'x' + orb.h, nvox(or9), 'vox (0 palette ids)');
      } else console.warn('[vb] assets/food/apple/00.vox or orange.vox missing — no apple or orange item, and no eating animation');
    }
    // ── THE HEART (user 2026-08-15: "theres a file called single.vox. use this for the hearts") ── the file is
    // exactly what its name says: a 1x1x1 model holding ONE voxel, colour 255/67/67. Five of them float in front
    // of the eye as the health readout (see the heart block in COMPOSITE); this is only where the model becomes
    // an item id. Parsed from the raw .vox the way the axe and the knife are, NOT through modelToItem, for one
    // reason: modelToItem resolves colours through `palette`, and palShare is allowed to hand back a shade up to
    // PAL_TOL off. That trade is right for scenery and wrong for a health colour, which has to be the same red
    // every session and can never drift toward some neighbouring decor brown. Registered here, ahead of the
    // creature strips, so it stays outside the bakeAO loop below (creatureStart marks that boundary) — a lone
    // voxel has no neighbours to occlude it, so the bake would be a no-op, but the heart is not a creature.
    try {
      const hv = await abuf('assets/single.vox');
      const hdv = new DataView(hv.buffer);
      let hsx = 0, hsy = 0, hsz = 0, hvox = null; const hpal = new Uint8Array(1024);
      const hwalk = (off, end) => { while (off < end) {
        const id = String.fromCharCode(hv[off], hv[off + 1], hv[off + 2], hv[off + 3]);
        const sz = hdv.getUint32(off + 4, true), csz = hdv.getUint32(off + 8, true);
        if (id === 'SIZE' && !hsx) { hsx = hdv.getUint32(off + 12, true); hsy = hdv.getUint32(off + 16, true); hsz = hdv.getUint32(off + 20, true); }
        else if (id === 'XYZI' && !hvox) { const n = hdv.getUint32(off + 12, true); hvox = hv.subarray(off + 16, off + 16 + n * 4); }
        else if (id === 'RGBA') hpal.set(hv.subarray(off + 12, off + 12 + 1024));
        else if (id === 'MAIN') { hwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
        off += 12 + sz + csz;
      } };
      hwalk(8, hv.length);
      if (hsx && hvox && hvox.length) {
        const hcells = new Array(hsx * hsy * hsz).fill(null);
        for (let i = 0; i < hvox.length; i += 4) { const ci = hvox[i + 3]; hcells[hvox[i] + hvox[i + 1] * hsx + hvox[i + 2] * hsx * hsy] = [hpal[(ci - 1) * 4], hpal[(ci - 1) * 4 + 1], hpal[(ci - 1) * 4 + 2]]; }
        HEART_IT = items.length + 1; items.push({ w: hsx, d: hsy, h: hsz, cells: hcells });
        console.log('[vb] single.vox heart item', HEART_IT, hsx, hsy, hsz, 'voxels', hvox.length / 4);
      }
    } catch (e) { console.warn('[vb] single.vox missing — the floating hearts are disabled', e); }   // HEART_IT stays 0 and the composite block never runs; nothing else in the game depends on it
    // ONE loader for every flying songbird strip — the cardinal and the blue bird ship identical 7-frame packs,
    // so the parse, the widest-frame scan and the eye-blink variants are all shared rather than copy-pasted.
    const loadFlight = async (dir, label) => {
      const it0 = items.length + 1;                                  // dit is 1-based; frame 0 follows the twig
      let bloaded = 0;
      for (let f = 0; f < 16; f++) {                   // every CONSECUTIVE frame that exists — the old fixed count of 8 threw on the first gap and disabled the bird (the pack ships 7)
        let bv = null;
        try { bv = await abuf('assets/life/' + dir + '/flight/' + String(f).padStart(2, '0') + '.vox'); } catch (e2) { bv = null; }
        if (!bv) { if (f === 0) throw new Error('flight frame 00 missing'); break; }
        const bdv = new DataView(bv.buffer);
        let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
        const bwalk = (off, end) => { while (off < end) {
          const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
          const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
          if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
          else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
          else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
          else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
          off += 12 + sz + csz;
        } };
        bwalk(8, bv.length);
        if (!bvox) { if (f === 0) throw new Error('flight frame 00 has no voxels'); break; }   // a 404 page parses to no XYZI — treat it as the end of the strip, not a fatal error
        const cells = new Array(bsx * bsy * bsz).fill(null);         // model x → width, y → depth, z → height (matches ITEMD axes)
        for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
        items.push({ w: bsx, d: bsy, h: bsz, cells });
        bloaded++;
      }
      const nf = bloaded;
      const gl = Math.min(1, nf - 1);   // hold frame 01 while gliding (user); clamped so a one-frame strip still resolves
      for (let f = 0; f < nf; f++) {         // BLINK variants (items it0+N .. +2N-1): the black EYE voxel (directly above the pink cheek) turns the plumage red — shown for a beat every few seconds
        const src = items[it0 - 1 + f], n = src.w * src.d;
        const cells = src.cells.slice();
        let pink = -1;
        for (let i = 0; i < cells.length; i++) { const c = cells[i]; if (c && c[0] > 225 && c[1] > 140 && c[1] < 195 && c[2] > 160 && c[2] < 210) { pink = i; break; } }
        if (pink >= 0 && pink + n < cells.length) {
          const eye = cells[pink + n];                 // one layer up in model height
          if (eye && eye[0] < 60 && eye[1] < 60 && eye[2] < 60) {
            const px = pink % src.w, py = ((pink / src.w) | 0) % src.d, pz = (pink / n) | 0;
            let red = null, bd = 1e9;                  // the red plumage voxel NEAREST the pink cheek
            for (let i = 0; i < cells.length; i++) { const c = cells[i]; if (!c || c[0] < 140 || c[0] < c[1] * 1.8 || c[0] < c[2] * 1.8) continue;
              const dx = i % src.w - px, dy = ((i / src.w) | 0) % src.d - py, dz = (i / n | 0) - pz, d2 = dx * dx + dy * dy + dz * dz;
              if (d2 < bd) { bd = d2; red = c; } }
            if (red) cells[pink + n] = red;
          }
        }
        items.push({ w: src.w, d: src.d, h: src.h, cells });
      }
      console.log('[vb] ' + label + ' flight', nf, 'frames + blink -> items', it0, '..', it0 + 2 * nf - 1);
      console.log('[vb] ' + label + ' flight', nf, 'frames -> items', it0, '.. glide pose', gl);
      return { item0: it0, n: nf, glide: gl };
    };
    // ── BIRDS ARE BACK IN THE SKY (user 2026-08-17: "put song birds in the sky, just like the pine forest") ──
    // and the history matters, because the request was framed as an oak-forest gap and it was not one. The same
    // user emptied this list on 2026-08-15 ("NO BIRDS IN THE SKY"), at the SOURCE rather than by zeroing BIRD_N,
    // and it was emptied for EVERY biome: measured before restoring, all 9 flock slots read init:false standing
    // in the pine forest as well as the oak. So there was nothing biome-specific to fix and nothing to copy
    // from the pine forest — the flock was simply switched off world-wide, and putting the three species back
    // restores it world-wide too. It is NOT gated to the oak forest: birds.js has no biome test at all, its
    // respawn ring just follows the player, and adding a first-ever biome gate to make a restore look local
    // would be a bigger and stranger change than the restore itself.
    // WHAT IT COSTS, since this is the thing the 2026-08-05 note bounded deliberately: the flock takes bird 0's
    // dedicated drop slot 4 plus BIRD_SLOTS (8) of the compacted creature slots back off the ground creatures
    // that were given them. If ducks/worms/fish start disappearing again, this line is why.
    // Emptying it again is how you turn them off; PERCHED songbirds are a different system (uniBirds,
    // stamped.js) and are unaffected either way.
    for (const sp of ['cardinal', 'blue_bird', 'robin']) {   // every songbird that ships a flight/ strip joins the flock automatically
      try { const r0 = await loadFlight(sp, sp); FLYERS.push({ name: sp, item0: r0.item0, n: r0.n, glide: r0.glide }); }
      catch (e) { console.warn('[vb] ' + sp + ' flight frames missing - species skipped', e); }
    }
    if (FLYERS.length) { BIRD_ITEM0 = FLYERS[0].item0; BIRD_NFRAMES = FLYERS[0].n; BIRD_GLIDE = FLYERS[0].glide; }   // legacy handles = the first species
    if (FLYERS.length > 1) { BLUEF_ITEM0 = FLYERS[1].item0; BLUEF_NFRAMES = FLYERS[1].n; BLUEF_GLIDE = FLYERS[1].glide; }
    console.log('[vb] flying songbirds:', FLYERS.map((f) => f.name + '(' + f.n + 'f @' + f.item0 + ')').join(', ') || 'none');
    // ── TRANSPARENT WINGS ON THE BUTTERFLY + DRAGONFLY (user 2026-08-16: "like the fly") ── the same
    // per-voxel ALPHA lane the desert fly rides (ITEMMAP.w — see the note in the desert loader below and the
    // readers in TRACE, creaSec and the composite). Two different numbers because the two wings are not the
    // same object: a dragonfly's are bare membrane over 6 pure-white voxels beside a 5-voxel body, so they take
    // the fly's own 0.5 and read as glass; a butterfly's wings ARE the animal — 6 of its 10 voxels and the only
    // thing that carries its colour — and at 0.5 it washes out over a bright meadow into a coloured ghost with
    // no silhouette left. 0.72 is thin enough that the ground moves visibly through the wing while the colour
    // still reads as that butterfly's colour at flying distance.
    const BFLY_WING_A = 0.72, DFLY_WING_A = 0.5;
    if (!location.search.includes('nobfly'))                          // BUTTERFLY flap frames → item table, same treatment as the cardinal (raw .vox palette, full 3D grids)
      for (const cname of ['orange', 'red', 'blue', 'lime', 'pink', 'purple']) {   // one 8-frame set per user-authored color subfolder; a missing color just skips
        try {
          const loaded = [];
          for (let f = 0; f < 8; f++) {
            const bv = await abuf('assets/life/butterfly/' + cname + '/0' + f + '.vox');
            const bdv = new DataView(bv.buffer);
            let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
            const bwalk = (off, end) => { while (off < end) {
              const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
              const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
              if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
              else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
              else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
              else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
              off += 12 + sz + csz;
            } };
            bwalk(8, bv.length);
            if (!bvox) throw new Error('butterfly ' + cname + ' frame 0' + f + ' has no voxels');
            const cells = new Array(bsx * bsy * bsz).fill(null);
            for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
            // ── THE BUTTERFLY'S WINGS ARE SOLID AGAIN (user 2026-08-16: "in the pine forest the butterflys
            // wings seem to be transparent, revert that change, they should be solid") ── the tagging that
            // marked every non-dark-grey cell with alpha 0.72 is removed. A butterfly's wings ARE the animal,
            // 6 of its 10 voxels and the only thing carrying its colour, so translucency read as a ghost over
            // the forest rather than as a wing. The FLY and the DRAGONFLY keep theirs: bare membrane over a
            // small dark body is the case the effect was built for, and neither was complained about.
            loaded.push({ w: bsx, d: bsy, h: bsz, cells });
          }
          BFLY_COLS.push(items.length + 1);                           // all 8 frames parsed OK — commit the whole color at once (no orphan half-sets)
          for (const it of loaded) items.push(it);
        } catch (e) { console.warn('[vb] butterfly color', cname, 'missing - skipped', e); }
      }
    if (BFLY_COLS.length) { BFLY_ITEM0 = BFLY_COLS[0]; BFLY_NFRAMES = 8;
      console.log('[vb] butterfly 8 frames x', BFLY_COLS.length, 'colors -> items', BFLY_ITEM0, '..', items.length); }
    try {                                                             // ── DRAGONFLY ── one 6-frame strip, parsed exactly like a butterfly colour (raw .vox palette, full 3D grid).
      const dloaded = [];                                             // base.vox is the source art and is deliberately NOT loaded, same as the songbird flight packs.
      for (let f = 0; f < 6; f++) {
        const bv = await abuf('assets/life/dragonfly/0' + f + '.vox');
        const bdv = new DataView(bv.buffer);
        let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
        const bwalk = (off, end) => { while (off < end) {
          const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
          const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
          if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
          else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
          else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
          else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
          off += 12 + sz + csz;
        } };
        bwalk(8, bv.length);
        if (!bvox) throw new Error('dragonfly frame 0' + f + ' has no voxels');
        const cells = new Array(bsx * bsy * bsz).fill(null);
        for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
        // ── THE DRAGONFLY IS THE FLY'S OWN CASE ── 6 PURE WHITE voxels (three per side) against a five-voxel
        // blue abdomen gradient and two black eyes: no other voxel in any of the six frames is even close to
        // white, so the fly's exact colour test transfers unchanged and survives a palette re-index.
        if (!location.search.includes('opaquewings')) for (const c9 of cells) { if (c9 && c9[0] > 250 && c9[1] > 250 && c9[2] > 250) c9.push(DFLY_WING_A); }
        dloaded.push({ w: bsx, d: bsy, h: bsz, cells });
      }
      DFLY_ITEM0 = items.length + 1;                                  // commit all 6 at once — no orphan half-set
      for (const it of dloaded) items.push(it);
      DFLY_NFRAMES = dloaded.length;
      console.log('[vb] dragonfly', DFLY_NFRAMES, 'frames -> items', DFLY_ITEM0, '..', items.length);
    } catch (e) { console.warn('[vb] dragonfly frames missing - skipped', e); DFLY_NFRAMES = 0; }
    try {                                                             // FIREFLY wing frames — same treatment; 3×3×3, 4 voxels (dark body, YELLOW abdomen, 2 white wings)
      if (location.search.includes('nobfly')) throw new Error('disabled by ?nobfly flag');
      const loaded = [];
      for (let f = 0; f < 4; f++) {
        const bv = await abuf('assets/life/firefly/0' + f + '.vox');
        const bdv = new DataView(bv.buffer);
        let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
        const bwalk = (off, end) => { while (off < end) {
          const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
          const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
          if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
          else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
          else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
          else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
          off += 12 + sz + csz;
        } };
        bwalk(8, bv.length);
        if (!bvox) throw new Error('firefly frame 0' + f + ' has no voxels');
        const cells = new Array(bsx * bsy * bsz).fill(null);
        for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
        loaded.push({ w: bsx, d: bsy, h: bsz, cells });
      }
      FFLY_ITEM0 = items.length + 1;
      for (const it of loaded) items.push(it);
      FFLY_NFRAMES = 4;
      console.log('[vb] firefly', FFLY_NFRAMES, 'frames -> items', FFLY_ITEM0, '..', items.length);
    } catch (e) { console.warn('[vb] firefly frames missing - nights stay dark', e); FFLY_NFRAMES = 0; }
    try {                                                             // WORM crawl frames — a 6-voxel pink inchworm, 12-frame crawl cycle, long axis = model y (head at −y, like the cardinal's beak)
      if (location.search.includes('nobfly')) throw new Error('disabled by ?nobfly flag');
      const loaded = [];
      for (let f = 0; f < 12; f++) {
        const bv = await abuf('assets/life/worm/' + String(f).padStart(2, '0') + '.vox');
        const bdv = new DataView(bv.buffer);
        let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
        const bwalk = (off, end) => { while (off < end) {
          const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
          const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
          if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
          else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
          else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
          else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
          off += 12 + sz + csz;
        } };
        bwalk(8, bv.length);
        if (!bvox) throw new Error('worm frame ' + f + ' has no voxels');
        const cells = new Array(bsx * bsy * bsz).fill(null);
        for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
        loaded.push({ w: bsx, d: bsy, h: bsz, cells });
      }
      WORM_ITEM0 = items.length + 1;
      for (const it of loaded) items.push(it);
      WORM_NFRAMES = 12;
      console.log('[vb] worm', WORM_NFRAMES, 'frames -> items', WORM_ITEM0, '..', items.length);
    } catch (e) { console.warn('[vb] worm frames missing - forest floor stays still', e); WORM_NFRAMES = 0; }
    for (const sp of ['salmon', 'minnow', 'bass', 'blue_gill', 'catfish']) {   // ── FISH ── one loader for every species: numbered swim frames straight in the species dir (like the worm), long axis = model y, head at −y
      try {
        if (location.search.includes('nobfly')) throw new Error('disabled by ?nobfly flag');
        const loaded = [];
        for (let f = 0; f < 16; f++) {                 // every CONSECUTIVE frame that exists — a species with no 00.vox just doesn't join the school
          const bv = await abuf('assets/life/' + sp + '/' + String(f).padStart(2, '0') + '.vox');
          if (!bv.length) break;
          const bdv = new DataView(bv.buffer);
          let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
          const bwalk = (off, end) => { while (off < end) {
            const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
            const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
            if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
            else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
            else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
            else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
            off += 12 + sz + csz;
          } };
          bwalk(8, bv.length);
          if (!bvox) break;                            // a 404 page parses to no XYZI — end of the strip, not fatal
          const cells = new Array(bsx * bsy * bsz).fill(null);
          for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
          loaded.push({ w: bsx, d: bsy, h: bsz, cells });
        }
        if (!loaded.length) throw new Error('no frames');
        const it0 = items.length + 1;
        for (const it of loaded) items.push(it);
        FISHES.push({ name: sp, item0: it0, n: loaded.length, half: Math.max(2, loaded[0].d * 0.5) });   // half the model's LONG axis (model y) — the AI's body probes scale to the real species, so a short minnow isn't navigated (or reported) as a 10-voxel salmon
        console.log('[vb] fish ' + sp, loaded.length, 'frames -> items', it0, '..', items.length);
      } catch (e) { if (!String(e).includes('no frames')) console.warn('[vb] fish ' + sp + ' skipped', e); }
    }
    // ── THE DESERT BAND ── the same loader shape as the fish above, and deliberately so. These are authored as
    // SCENE-GRAPH animations (cobra is 19 keyframed segments, scorpion 6 parts, and so on), which nothing here
    // could read; tools/bake_desert_life.py composites each animation frame into a flat numbered .vox offline,
    // so by the time the game sees them they are ordinary frame strips and need no new parsing.
    // Cells are raw sRGB like every other creature, so these cost ZERO of the 256-entry world palette — which
    // matters: the band holds ~110 distinct authored colours and the table has almost nothing left. NOTHING in
    // this loop touches palShare or edCol: a cell is `[r, g, b]` straight off the .vox RGBA chunk and goes into
    // ITEMMAP as floats, so ADDING A SPECIES HERE CANNOT MINT A PALETTE ID. The grass snake's 21 authored
    // greens are the proof — they would not have fit otherwise.
    // ── AND 'DESERT' IS NOW THE BAND, NOT THE BIOME ── the bee and the grass snake are OAK FOREST creatures
    // (user 2026-08-17) that ride this band because it is the one that already does scene-graph animation,
    // per-species behaviour tables and a biome tag. Which biome a species lives in is decided by DES_OAKONLY
    // in sim/life/slots.js, never by membership of this list.
    for (const sp of DES_LOAD) {
      try {
        const loaded = [];
        for (let f = 0; f < 96; f++) {                 // gecko is 67 frames (its tongue keyframes run to _f 66)
          const bv = await abuf('assets/life/' + sp + '/' + String(f).padStart(2, '0') + '.vox');
          if (!bv.length) break;
          const bdv = new DataView(bv.buffer);
          let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
          const bwalk = (off, end) => { while (off < end) {
            const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
            const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
            if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
            else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
            else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
            else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
            off += 12 + sz + csz;
          } };
          bwalk(8, bv.length);
          if (!bvox) break;
          const cells = new Array(bsx * bsy * bsz).fill(null);
          for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
          // ── THE FLY'S WINGS ARE HALF TRANSPARENT (user 2026-08-15) ── a cell may carry a FOURTH
          // number, its ALPHA, which rides into ITEMMAP.w — a lane that was a 1/0 occupancy flag and is now
          // the real coverage (see the readers in TRACE, creaSec and the composite). Keyed on the COLOUR, not
          // on a palette index: the fly is one dark body voxel (23,23,23) between two PURE WHITE wings, so
          // white is unambiguous here and survives the artist re-indexing the authored .vox palette.
          // ?opaquewings puts the wings back to solid for an A/B: it drops the alpha at the SOURCE, so the
          // measured id range below comes back empty and every downstream test folds to its old behaviour.
          // ── AND THE BEE RIDES THE SAME LANE, ON THE SAME KEY ── it is the fly's model with a stripe: two PURE
          // WHITE wing voxels either side of a three-voxel black/yellow/black body, flapping on the fly's own
          // z 9-8-7-8 cycle. So the colour key needs no widening — the body's (23,23,23) and (252,215,5) are
          // nowhere near 250 on all three channels and stay solid — and it takes the fly's 0.5 rather than the
          // butterfly's 0.72 for the fly's reason: the wings are bare membrane beside a body that carries all
          // of the animal's colour, so there is no silhouette to lose.
          if ((sp === 'fly' || sp === 'bee') && !location.search.includes('opaquewings')) for (const c9 of cells) { if (c9 && c9[0] > 250 && c9[1] > 250 && c9[2] > 250) c9.push(0.5); }
          loaded.push({ w: bsx, d: bsy, h: bsz, cells });
        }
        if (!loaded.length) throw new Error('no frames');
        const it0 = items.length + 1;
        for (const it of loaded) items.push(it);
        DESERTS.push({ name: sp, item0: it0, n: loaded.length });
        // mamFitOf takes (itemsTable, ONE-BASED id) — passing the item object alone made it index loaded[0][-1],
        // come back undefined and return null for all seven, silently. The guard mirrors the land-mammal call
        // site: a null must never overwrite, or the seat falls back to a constant that fits nothing.
        const fitD = mamFitOf(items, it0); if (fitD) MAMFIT[sp] = fitD;
        console.log('[vb] desert ' + sp, loaded.length, 'frames -> items', it0, '..', items.length);
      } catch (e) { if (!String(e).includes('no frames')) console.warn('[vb] desert ' + sp + ' skipped', e); }
    }
    console.log('[vb] desert creatures:', DESERTS.map((f) => f.name + '(' + f.n + 'f @' + f.item0 + ')').join(', ') || 'none');
    console.log('[vb] fish species:', FISHES.map((f) => f.name + '(' + f.n + 'f @' + f.item0 + ')').join(', ') || 'none');
    try {                                                             // DUCK — one static model (5×8×8): orange feet z0, brown body, white ring, green head; floats on lakes
      if (location.search.includes('nobfly')) throw new Error('disabled by ?nobfly flag');
      const bv = await abuf('assets/life/duck/base.vox');
      const bdv = new DataView(bv.buffer);
      let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
      const bwalk = (off, end) => { while (off < end) {
        const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
        const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
        if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
        else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
        else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
        else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
        off += 12 + sz + csz;
      } };
      bwalk(8, bv.length);
      if (!bvox) throw new Error('duck has no voxels');
      const cells = new Array(bsx * bsy * bsz).fill(null);
      for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
      DUCK_ITEM0 = items.length + 1;
      items.push({ w: bsx, d: bsy, h: bsz, cells });
      { let bestG = -1; for (const c of cells) { if (!c) continue; const [r, g, b] = c;   // HEAD GREEN = the brightest strongly-green voxel (for the eye blink)
        if (g > r + 18 && g > b + 18 && g > bestG) { bestG = g; DUCK_GREEN = [Math.pow(r / 255, 2.2), Math.pow(g / 255, 2.2), Math.pow(b / 255, 2.2)]; } } }
      console.log('[vb] duck -> item', DUCK_ITEM0, 'head green', DUCK_GREEN.map((v) => Math.round(Math.pow(v, 1 / 2.2) * 255)));
      try {                                                           // BABY duck — same parse; missing baby just means mothers swim alone
        const v2 = await abuf('assets/life/duck/baby.vox');
        const d2v = new DataView(v2.buffer);
        let cx2 = 0, cy2 = 0, cz2 = 0, cvox = null; const cpal = new Uint8Array(1024);
        const cwalk = (off, end) => { while (off < end) {
          const id = String.fromCharCode(v2[off], v2[off + 1], v2[off + 2], v2[off + 3]);
          const sz = d2v.getUint32(off + 4, true), csz = d2v.getUint32(off + 8, true);
          if (id === 'SIZE' && !cx2) { cx2 = d2v.getUint32(off + 12, true); cy2 = d2v.getUint32(off + 16, true); cz2 = d2v.getUint32(off + 20, true); }
          else if (id === 'XYZI' && !cvox) { const n = d2v.getUint32(off + 12, true); cvox = v2.subarray(off + 16, off + 16 + n * 4); }
          else if (id === 'RGBA') cpal.set(v2.subarray(off + 12, off + 12 + 1024));
          else if (id === 'MAIN') { cwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
          off += 12 + sz + csz;
        } };
        cwalk(8, v2.length);
        if (!cvox) throw new Error('baby duck has no voxels');
        const cc = new Array(cx2 * cy2 * cz2).fill(null);
        for (let i = 0; i < cvox.length; i += 4) { const ci = cvox[i + 3]; cc[cvox[i] + cvox[i + 1] * cx2 + cvox[i + 2] * cx2 * cy2] = [cpal[(ci - 1) * 4], cpal[(ci - 1) * 4 + 1], cpal[(ci - 1) * 4 + 2]]; }
        DUCKB_ITEM0 = items.length + 1;
        items.push({ w: cx2, d: cy2, h: cz2, cells: cc });
        // ── WHERE THE EYES ARE (user 2026-08-05) ── an orphaned duckling CRIES, and the tears have to come
        // out of the eyes rather than the middle of the model. The eye is the BLACK voxel — the same one the
        // blink flashes green, found by the same test the shader uses (linear < 0.02 ≈ sRGB < 42), so the two
        // can never disagree about which voxel is an eye. Only the upper half is searched: a duckling's
        // feet are black too, and tears from the feet would be a different effect entirely.
        // ── STORED RELATIVE TO THE MODEL'S CENTRE (user 2026-08-05: "not coming out of the right area") ──
        // and that is the whole fix. The drop-slot tracer puts the emit ANCHOR at the middle of the box:
        // its ray origin is (camera−anchor)/scale + (w/2, d/2, h/2), so a model cell m sits at
        //   anchor + (Xw·(m.x−w/2) + Yw·(m.y−d/2) + Zw·(m.z−h/2)) · scale.
        // The emit was rotating the RAW cell centre, i.e. missing that half-extent, so every tear appeared
        // a half-model up-and-across from the duckling instead of at its eye. baby.vox is 3x6x6 with its
        // one black voxel at (1,2,4), so the error was (1.5, 3, 3) cells — bigger than the duckling.
        DUCKB_EYES = [];
        for (let z = cz2 >> 1; z < cz2; z++) for (let y = 0; y < cy2; y++) for (let x = 0; x < cx2; x++) {
          const c = cc[x + y * cx2 + z * cx2 * cy2];
          if (c && c[0] < 42 && c[1] < 42 && c[2] < 42)
            DUCKB_EYES.push([x + 0.5 - cx2 * 0.5, y + 0.5 - cy2 * 0.5, z + 0.5 - cz2 * 0.5]);
        }
        console.log('[vb] baby duck -> item', DUCKB_ITEM0, 'eye voxels', DUCKB_EYES.length);
      } catch (e) { console.warn('[vb] baby duck missing - mothers swim alone', e); DUCKB_ITEM0 = 0; }
    } catch (e) { console.warn('[vb] duck missing - lakes stay empty', e); DUCK_ITEM0 = 0; }
    if (LILYV.length) {                                               // LILY PADS → item table: the same lake decor models, now free-floating entities
      LILY_ITEM0 = items.length + 1;
      for (const m of LILYV) { items.push(modelToItem(m)); LILY_SZ.push(m.sz); }
      console.log('[vb] lily pads x', LILYV.length, '-> items', LILY_ITEM0, '..', items.length);
    }
    try {                                                             // CARDINAL ROTATE frames → item table (raw .vox palette); shown as ONE spinning model on the asset-editor stage (base.vox ignored)
      const loaded = [];
      for (let f = 0; f < 11; f++) {
        const bv = await abuf('assets/life/cardinal/rotate/' + String(f).padStart(2, '0') + '.vox');
        const bdv = new DataView(bv.buffer, bv.byteOffset, bv.byteLength);
        let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
        const bwalk = (off, end) => { while (off < end) {
          const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
          const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
          if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
          else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
          else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
          else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
          off += 12 + sz + csz;
        } };
        bwalk(8, bv.length);
        if (!bvox) throw new Error('cardinal rotate frame ' + f + ' has no voxels');
        const cells = new Array(bsx * bsy * bsz).fill(null);
        for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
        loaded.push({ w: bsx, d: bsy, h: bsz, cells });
        CARDINAL_ROTATE.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv });   // keep raw bytes for the editor filmstrip
      }
      CARD_ITEM0 = items.length + 1;
      for (const it of loaded) items.push(it);
      CARD_NFRAMES = loaded.length;
      for (const it of loaded) { CARD_H = Math.max(CARD_H, it.h);   // model height + the LOWEST occupied voxel row → so the editor can plant the feet on the platform (not float)
        for (let z = 0; z < it.h; z++) { let any = false; for (let i = z * it.w * it.d; i < (z + 1) * it.w * it.d; i++) if (it.cells[i]) { any = true; break; } if (any) { CARD_FOOTZ = Math.min(CARD_FOOTZ, z); break; } } }
      console.log('[vb] cardinal rotate', CARD_NFRAMES, 'frames -> items', CARD_ITEM0, '..', items.length, 'h', CARD_H, 'footZ', CARD_FOOTZ);
    } catch (e) { console.warn('[vb] cardinal rotate frames missing - editor bird disabled', e); CARD_NFRAMES = 0; }
    try {                                                             // BLUE BIRD rotate frames (raw bytes) → editor filmstrip, EXACTLY like the cardinal (same 11 frames, base.vox ignored)
      for (let f = 0; f < 11; f++) { const bv = await abuf('assets/life/blue_bird/rotate/' + String(f).padStart(2, '0') + '.vox'); BLUEBIRD_ROTATE.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }
      console.log('[vb] blue_bird rotate', BLUEBIRD_ROTATE.length, 'frames (editor filmstrip)');
    } catch (e) { console.warn('[vb] blue_bird rotate frames missing', e); BLUEBIRD_ROTATE = []; }
    try {                                                             // ROBIN rotate frames — identical geometry to the cardinal, so it reuses CARD_OFF/CARD_FOOTZ untouched
      for (let f = 0; f < 11; f++) { const bv = await abuf('assets/life/robin/rotate/' + String(f).padStart(2, '0') + '.vox'); ROBIN_ROTATE.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }
      console.log('[vb] robin rotate', ROBIN_ROTATE.length, 'frames (3rd perched songbird)');
    } catch (e) { console.warn('[vb] robin rotate frames missing', e); ROBIN_ROTATE = []; }
    try {                                                             // BUNNY frames (raw bytes) → the asset-editor filmstrips. Folders: jump/, rotate/left/, rotate/right/ (00-10 each). A missing file 404s to an HTML page, so REQUIRE the 'VOX ' magic before pushing — else the loop swallows 404 pages as garbage frames.
      const isVox = (u8) => u8.length > 4 && u8[0] === 0x56 && u8[1] === 0x4f && u8[2] === 0x58 && u8[3] === 0x20;   // 'VOX '
      for (let f = 0; f < 16; f++) { const bv = await abuf('assets/life/bunny/rotate/left/' + String(f).padStart(2, '0') + '.vox'); if (!isVox(bv)) break; BUNNY_ROTATE.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }
      for (let f = 0; f < 16; f++) { const bv = await abuf('assets/life/bunny/jump/' + String(f).padStart(2, '0') + '.vox'); if (!isVox(bv)) break; BUNNY_JUMP.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }
      for (let f = 0; f < 16; f++) { const bv = await abuf('assets/life/bunny/rotate/right/' + String(f).padStart(2, '0') + '.vox'); if (!isVox(bv)) break; BUNNY_ROTATE_RIGHT.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }   // authored RIGHT-rotate frames
      for (let f = 0; f < 16; f++) { const bv = await abuf('assets/life/armadillo/walk/' + String(f).padStart(2, '0') + '.vox'); if (!isVox(bv)) break; ARMADILLO_WALK.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }   // ARMADILLO walk cycle (world creature)
      for (let f = 0; f < 16; f++) { const bv = await abuf('assets/life/skunk/' + String(f).padStart(2, '0') + '.vox'); if (!isVox(bv)) break; SKUNK_WALK.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }   // SKUNK walk cycle — the asset-editor object (user)
      for (let f = 0; f < 16; f++) { const bv = await abuf('assets/life/porcupine/' + String(f).padStart(2, '0') + '.vox'); if (!isVox(bv)) break; PORCUPINE_WALK.push({ name: String(f).padStart(2, '0') + '.vox', u8: bv }); }   // PORCUPINE walk cycle — WORLD-only 4th land mammal (user re-added)
      console.log('[vb] bunny', BUNNY_ROTATE.length, 'rotate +', BUNNY_JUMP.length, 'jump frames (asset-editor objects)');
      // …and load the SAME frames into the ITEM table so the editor TRACE-INJECTS the playing bunny through the dynamic-life
      // path (full SVGF + the creature grain/AO treatment) — i.e. it renders IDENTICALLY to the ducks/fish/birds (user).
      const parseBunny = (frames) => { const out = [];
        for (const fr of frames) { const bv = fr.u8, bdv = new DataView(bv.buffer, bv.byteOffset, bv.byteLength);
          let bsx = 0, bsy = 0, bsz = 0, bvox = null; const bpal = new Uint8Array(1024);
          const bwalk = (off, end) => { while (off + 12 <= end) {
            const id = String.fromCharCode(bv[off], bv[off + 1], bv[off + 2], bv[off + 3]);
            const sz = bdv.getUint32(off + 4, true), csz = bdv.getUint32(off + 8, true);
            if (id === 'SIZE' && !bsx) { bsx = bdv.getUint32(off + 12, true); bsy = bdv.getUint32(off + 16, true); bsz = bdv.getUint32(off + 20, true); }
            else if (id === 'XYZI' && !bvox) { const n = bdv.getUint32(off + 12, true); bvox = bv.subarray(off + 16, off + 16 + n * 4); }
            else if (id === 'RGBA') bpal.set(bv.subarray(off + 12, off + 12 + 1024));
            else if (id === 'MAIN') { bwalk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
            off += 12 + sz + csz;
          } };
          bwalk(8, bv.length);
          if (!bvox) continue;
          const cells = new Array(bsx * bsy * bsz).fill(null);
          for (let i = 0; i < bvox.length; i += 4) { const ci = bvox[i + 3]; cells[bvox[i] + bvox[i + 1] * bsx + bvox[i + 2] * bsx * bsy] = [bpal[(ci - 1) * 4], bpal[(ci - 1) * 4 + 1], bpal[(ci - 1) * 4 + 2]]; }
          out.push({ w: bsx, d: bsy, h: bsz, cells });
        }
        return out; };
      const bloaded = parseBunny(BUNNY_ROTATE);           // ROTATE frames (turn animation) → BUNNY_ITEM0
      if (bloaded.length) { BUNNY_ITEM0 = items.length + 1; for (const it of bloaded) items.push(it); BUNNY_NFRAMES = bloaded.length; }
      const bjloaded = parseBunny(BUNNY_JUMP);             // JUMP frames (hop animation) → BUNNY_JUMP_ITEM0
      if (bjloaded.length) { BUNNY_JUMP_ITEM0 = items.length + 1; for (const it of bjloaded) items.push(it); BUNNY_JUMP_NFRAMES = bjloaded.length; }
      const aloaded = parseBunny(ARMADILLO_WALK);          // 8 WALK frames → ARMADILLO_ITEM0 (world armadillo trace-injects these; no blink items, like the bunny)
      if (aloaded.length) { ARMADILLO_ITEM0 = items.length + 1; for (const it of aloaded) items.push(it); ARMADILLO_NFRAMES = aloaded.length; }
      const blloaded = parseBunny(BLUEBIRD_ROTATE);        // BLUE BIRD rotate frames -> BLUEB_ITEM0
      if (blloaded.length === CARD_NFRAMES) { BLUEB_ITEM0 = items.length + 1; for (const it of blloaded) items.push(it); }   // frame-count parity is required, not cosmetic: the perch clock indexes both tables with the SAME frame number
      const rbloaded = parseBunny(ROBIN_ROTATE);           // ROBIN rotate frames -> ROBIN_ITEM0
      if (rbloaded.length === CARD_NFRAMES) { ROBIN_ITEM0 = items.length + 1; for (const it of rbloaded) items.push(it); }
      console.log('[vb] songbird reskins -> items: blue', BLUEB_ITEM0, 'robin', ROBIN_ITEM0, 'of', CARD_NFRAMES, 'frames');
      const skloaded = parseBunny(SKUNK_WALK);             // SKUNK walk frames -> SKUNK_ITEM0 (same call the armadillo uses; raw .vox RGB, so this does NOT touch the 256-entry world palette)
      if (skloaded.length) { SKUNK_ITEM0 = items.length + 1; for (const it of skloaded) items.push(it); SKUNK_NFRAMES = skloaded.length; }
      const ploaded = parseBunny(PORCUPINE_WALK);          // PORCUPINE walk frames -> PORCUPINE_ITEM0
      if (ploaded.length) { PORCUPINE_ITEM0 = items.length + 1; for (const it of ploaded) items.push(it); PORCUPINE_NFRAMES = ploaded.length; }
      for (const [k9, i9] of [['arm', ARMADILLO_ITEM0], ['skunk', SKUNK_ITEM0], ['porc', PORCUPINE_ITEM0], ['bunny', BUNNY_ITEM0]]) {
        const f9 = i9 ? mamFitOf(items, i9) : null; if (f9) MAMFIT[k9] = f9; }   // read off frame 0: every frame of a walk cycle shares the model box
      console.log('[vb] mammal fit', JSON.stringify(MAMFIT));
      console.log('[vb] skunk -> items', SKUNK_ITEM0, '(' + SKUNK_NFRAMES + '); porcupine', PORCUPINE_ITEM0, '(' + PORCUPINE_NFRAMES + ')');
      console.log('[vb] bunny -> items: rotate', BUNNY_ITEM0, '(' + BUNNY_NFRAMES + '), jump', BUNNY_JUMP_ITEM0, '(' + BUNNY_JUMP_NFRAMES + '); armadillo', ARMADILLO_ITEM0, '(' + ARMADILLO_NFRAMES + ')');
    } catch (e) { console.warn('[vb] bunny frames missing', e); BUNNY_ROTATE = []; }
    const bakeAO = (it) => {                                          // per-voxel SELF-AMBIENT-OCCLUSION → darker in creases/undersides, the depth cue static world voxels get from irr.g (user: creatures 'don't render like static objects')
      const w = it.w, d = it.d, h = it.h, cells = it.cells, out = cells.slice();
      const sol = (x, y, z) => x >= 0 && x < w && y >= 0 && y < d && z >= 0 && z < h && !!cells[x + y * w + z * w * d];
      for (let z = 0; z < h; z++) for (let y = 0; y < d; y++) for (let x = 0; x < w; x++) {
        const c = cells[x + y * w + z * w * d]; if (!c) continue;
        let occ = 0;                                                  // count filled neighbours in the UPPER hemisphere + same level (17 cells) — these block ambient sky; the support voxels BELOW don't darken a lit top
        for (let dz = 0; dz <= 1; dz++) for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { if ((dx || dy || dz) && sol(x + dx, y + dy, z + dz)) occ++; }
        const ao = 1 - 0.5 * (occ / 17);                              // exposed tip ~1.0, tucked crease ~0.7
        out[x + y * w + z * w * d] = c.length > 3 ? [c[0] * ao, c[1] * ao, c[2] * ao, c[3]] : [c[0] * ao, c[1] * ao, c[2] * ao];   // …carrying a per-voxel ALPHA straight through: occlusion darkens a wing, it must never make it solid
      }
      it.cells = out; };
    const creatureStart = Math.min(...[BFLY_ITEM0, FFLY_ITEM0, WORM_ITEM0, DUCK_ITEM0, DUCKB_ITEM0, LILY_ITEM0, CARD_ITEM0, BUNNY_ITEM0, ARMADILLO_ITEM0, SKUNK_ITEM0, PORCUPINE_ITEM0, BLUEB_ITEM0, ROBIN_ITEM0].filter((x) => x > 0).concat([1e9]));   // creatures only — NOT the hand tools (axe/rock/knife keep their authored gradients, no grain/AO)
    const HELD_ITEMS = new Set([1, ROCK_IT, STICK_IT, CONE_IT, KNIFE_IT, PICK_IT, SHOVEL_IT, BOW_IT, MEAT_IT, HOE_IT, SPEAR_IT].filter(Boolean));
    for (let i = creatureStart - 1; i < items.length; i++) {
      if (!items[i] || !items[i].cells || !items[i].cells.length) continue;
      // ── STATIC LIGHTING ON WHAT YOU CARRY (user) ── held and dropped items now get the SAME baked
      // occlusion every other model does, so a tool has form in the hand — its head reads as separate from
      // its haft, and the recesses hold shadow — instead of sitting flat under whatever the world lights it
      // with. ?flatheld goes back to the traced-only look for an A/B.
      if (location.search.includes('flatheld') && HELD_ITEMS.has(i + 1)) continue;
      if (/[?&]uni\b/.test(location.search) && !HELD_ITEMS.has(i + 1)) continue;   // ?uni: real AO rays replace the bake for creatures (see the note above); held tools keep theirs. MATCHED AS A WHOLE FLAG, not a substring: includes('uni') also fired on ?nouni (buffers.js LIFE_UNI), so booting the pre-unified A/B silently dropped bakeAO from every creature item too — two changes at once, which is the exact comparison ?nouni exists to make valid.
      bakeAO(items[i]);
    }
    itemsRef = items;                                  // publish the finished table to the tick loop (cardinal hitbox size + worm stamp poses)
    const dims = [], flat = [], itemOff = [];          // itemOff = where each item's block starts in ITEMMAP, in vec4s — the bow strip is re-written there live
    for (const it of items) {
      itemOff.push(flat.length >> 2);
      dims.push(`vec4<i32>(${it.w}, ${it.d}, ${it.h}, ${flat.length >> 2})`);
      for (const c of it.cells) { if (c) flat.push(Math.pow(c[0] / 255, 2.2), Math.pow(c[1] / 255, 2.2), Math.pow(c[2] / 255, 2.2), c.length > 3 ? c[3] : 1.0); else flat.push(0, 0, 0, 0); }   // .w is ALPHA now, not occupancy: 1 = solid, 0 = empty, anything between = translucent (the fly's wings)
    }
    if (!flat.length) flat.push(0, 0, 0, 0);
    itemHalfH = items.map((m) => (m.h || 9) * 0.5);   // …and each item's half height, for the resting pose of a dropped item (see the drop block)
    itemMapF32 = new Float32Array(flat);               // → storage buffer at binding 13 (created with the other GPU buffers)
    const maxSteps = Math.max(...items.map(it => it.w + it.d + it.h + 3));
    // ── WHICH ITEMS HOLD A TRANSLUCENT VOXEL ── the composite has to walk a trace-injected creature's model a
    // SECOND time to draw the voxels TRACE deliberately looked straight through, and doing that for every
    // creature would spend a whole extra DDA per pixel on animals with nothing translucent about them. So the
    // ids that actually carry a sub-1.0 alpha are measured once here and the shaders test them by RANGE.
    // Nothing translucent → LO > HI → the test is dead and the second walk is never entered.
    //
    // TWO ranges, not one, and that is the whole reason this is not a plain min/max any more: the butterflies
    // and the dragonfly load back to back near the HEAD of the item table (48 + 6 consecutive ids) while the
    // desert fly lands far down it, past the fireflies, worms, songbirds, ducks, lilies, cardinals, the four
    // land mammals and every fish. One min..max span would drag all of those through the second DDA per pixel
    // for nothing — a whole wasted model walk on animals with no translucent voxel in them to find. So the
    // RUNS are measured: run 1 is the leading block, run 2 spans everything after the first gap (exactly the
    // fly today; a third block added later WIDENS range 2 rather than being silently dropped).
    const traIds = [];
    for (let i = 0; i < items.length; i++) { const it = items[i]; if (!it || !it.cells) continue;
      for (const c of it.cells) { if (c && c.length > 3 && c[3] < 0.999) { traIds.push(i + 1); break; } } }
    let traLo = 536870912, traHi = -1, tra2Lo = 536870912, tra2Hi = -1;
    for (const id9 of traIds) {
      if (traHi < 0) { traLo = id9; traHi = id9; }                    // the first translucent id opens run 1
      else if (tra2Hi < 0 && id9 === traHi + 1) { traHi = id9; }      // still contiguous with run 1 - extend it
      else { if (tra2Hi < 0) tra2Lo = id9; tra2Hi = id9; }            // past the first gap - run 2, widening
    }
    console.log('[vb] translucent item ids', traHi > 0 ? traLo + '..' + traHi : 'none', tra2Hi > 0 ? '+ ' + tra2Lo + '..' + tra2Hi : '');
    // ── LIVE ARROW TURN ── re-cut the two bow runs for a new orientation and overwrite ONLY their colours.
    // fetchBowStrip fixed the grid across every orientation, so the dimensions baked into ITEMD above stay
    // true and nothing has to be recompiled — the change is one buffer write over a contiguous range.
    bowRefit = (rot, pos) => {
      if (!BOW_IT || !BOW_NOCK || !BOWSTRIP || !BOWSTRIP.rebuild) return false;
      const ns = BOWSTRIP.rebuild(rot, pos);
      for (const [id0, strip] of [[BOW_IT, ns.withArrow], [BOW_NOCK, ns.bowOnly]])
        for (let f = 0; f < strip.length; f++) {
          const i = id0 - 1 + f, it = modelToItem(strip[f]);
          items[i] = it;
          let o = itemOff[i] * 4;
          for (const c of it.cells) {
            if (c) { itemMapF32[o] = Math.pow(c[0] / 255, 2.2); itemMapF32[o + 1] = Math.pow(c[1] / 255, 2.2); itemMapF32[o + 2] = Math.pow(c[2] / 255, 2.2); itemMapF32[o + 3] = 1; }
            else { itemMapF32[o] = 0; itemMapF32[o + 1] = 0; itemMapF32[o + 2] = 0; itemMapF32[o + 3] = 0; }
            o += 4;
          }
        }
      const last = BOW_NOCK - 1 + BOW_FRAMES - 1;      // the two runs are consecutive, so one contiguous write covers both
      const from = itemOff[BOW_IT - 1] * 4, to = itemOff[last] * 4 + items[last].cells.length * 4;
      device.queue.writeBuffer(itemMapBuf, from * 4, itemMapF32, from, to - from);
      return true;
    };
    pickWGSL = `const ITEMN : i32 = ${items.length}; const PICKSTEPS : i32 = ${maxSteps}; const TRA_LO : i32 = ${traLo}; const TRA_HI : i32 = ${traHi}; const TRA2_LO : i32 = ${tra2Lo}; const TRA2_HI : i32 = ${tra2Hi};
    const ITEMD : array<vec4<i32>, ${items.length}> = array<vec4<i32>, ${items.length}>(${dims.join(', ')});
    @group(0) @binding(13) var<storage, read> ITEMMAP : array<vec4<f32>>;`;
  }

