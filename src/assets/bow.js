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
  const parseBowStrip = (b, url) => {
    try {
      const sc = parseVoxScene(b, true, true);   // the BOW is held too — exact colours
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
  const parseVoxShared = (b, url) => { try { return b ? parseVoxModel(b, true, true) : null; } catch (e) { console.warn('[vb] vox', url, e); return null; } };   // noTol: every model on this path is a HELD item (the stone kit, the arrow, the meat) — exact colours only, see palShare   // …in SHARE mode: an item-only model that must not cost palette entries (the bow strip)
  // SHARE by default (2026-08-05). Every decoration used to mint a fresh palette entry for colours the table
  // already held — the pine browns and foliage greens landed two and three times over, 40 duplicate ids in all,
  // and the table hit its 256 ceiling before the land mammals loaded. Past the ceiling edCol silently
  // NEAREST-MATCHES, which is why the porcupine wore a mushroom's olive and the armadillo's shell gradient
  // scattered. Reusing an identical colour is invisible; what it is NOT safe for is a model whose OWN ids gate a
  // behaviour — PICK_CONE is built from the pinecone's ids, so a shared id there would let one cone pickup flood
  // everything else wearing that colour. Pass own = true to keep dedicated entries, as the pinecone does.
  const parseVoxVariants2 = (b, url) => { try { return b ? parseVoxVariants(b, true) : null; } catch (e) { console.warn('[vb]', url, 'is unreadable — that variant set is skipped', e); return null; } };   // share=true: meadow flowers are scenery, so the PAL_TOL reuse applies (they are not held)
  const parseVoxDecor = (b, url, own) => { try { return b ? parseVoxModel(b, !own) : null; } catch (e) { console.warn('[vb]', url, 'is unreadable — that decoration is skipped', e); return null; } };
  const fetchBytes = async (url) => { try { return new Uint8Array(await (await fetch(url)).arrayBuffer()); } catch (e) { console.warn('[vb]', url, 'missing — that decoration is skipped', e); return null; } };
  // ── WHAT THE ARTIST PAINTED, CELL BY CELL, AND NOTHING ELSE ── a .vox read that MINTS NOTHING: no palette
  // id is added, shared or reserved by calling this, which is the same promise voxColsUsed (assets/models.js)
  // makes and for the same reason — sometimes a loader has to know the authored colours BEFORE it decides what
  // ids they should get. voxColsUsed answers "which colours"; this answers "which colour is in which CELL",
  // which is what the fruit loader below needs to tell an apple's brown stalk from its green leaf after the
  // bake pooled the two into one slot. Key is x | y<<8 | z<<16, the same packing every model here uses.
  const voxCellCols = (b) => { const out = new Map(); if (!b || b.length < 16) return out;
    try {
      const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
      let vox = null; const pal = new Uint8Array(1024);
      const walk = (off, end) => { while (off + 12 <= end) {
        const id = String.fromCharCode(b[off], b[off + 1], b[off + 2], b[off + 3]);
        const sz = dv.getUint32(off + 4, true), csz = dv.getUint32(off + 8, true);
        if (id === 'XYZI' && !vox) { const n = dv.getUint32(off + 12, true); vox = b.subarray(off + 16, off + 16 + n * 4); }
        else if (id === 'RGBA') pal.set(b.subarray(off + 12, off + 12 + 1024));
        else if (id === 'MAIN') { walk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
        off += 12 + sz + csz;
      } };
      walk(8, b.length);
      if (vox) for (let i = 0; i < vox.length; i += 4) { const ci = vox[i + 3]; if (!ci) continue;
        out.set(vox[i] | (vox[i + 1] << 8) | (vox[i + 2] << 16), [pal[(ci - 1) * 4], pal[(ci - 1) * 4 + 1], pal[(ci - 1) * 4 + 2]]); }
    } catch (e) { console.warn('[vb] voxCellCols: unreadable .vox — the caller falls back to its default', e); }
    return out; };
  // ── PARALLEL FETCH, ORDERED PARSE ── the requests still overlap, which is the whole point of the
  // Promise.all this replaces. What changed is WHERE the palette ids get minted: parsing used to happen
  // inside each promise, so a model's colours were added the moment ITS response landed, and id assignment
  // became a race on network timing. Measured: two identical boots disagreed on 26 palette entries in the
  // 189+ range. Ids a boot cannot reproduce cannot be audited, compared or reclaimed — and every A/B of a
  // palette change is noise until this is ordered. So: fetch all the bytes at once, parse them in ARRAY order.
  const DECOR_LOAD = [
    ['assets/decoration/pinecone.vox', 'own'],                                              // hangs under pine branches — OWN ids: PICK_CONE is built from them, so they must not be shared with anything else
    ['assets/decoration/lillypad_small.vox', 'decor'],                                      // floats on lakes + rivers
    ['assets/decoration/lillypad_medium.vox', 'decor'],
    ['assets/decoration/lillypad_large.vox', 'decor'],
    ['assets/decoration/stick_1.vox', 'held'],                                              // ground scatter, and PICKABLE - so 'held' for the same reason rock.vox is: the player carries it, and the shared palette path rounds its authored browns onto whatever decoration colour is within PAL_TOL
    ['assets/decoration/stick_2.vox', 'held'],
    ['assets/decoration/log.vox', 'decor'],                                                 // rare fallen log, solid
    ['assets/decoration/rock.vox', 'held'],                                                 // the small stone. Ground scatter AND the thing the player dual-wields, which is why it is 'held' and not 'decor': it is parsed like a decoration in every other respect but takes the EXACT-COLOUR path. Its five authored greys (147/140/134/127/120) sit inside PAL_TOL of one another, so on the shared path 140 and 127 both collapsed onto 134 and twelve of its twenty voxels came out the same shade - a five-step ramp rendered as three. Measured with __vb.itemCols(2), which reported three colours where the .vox has five. The noTol comment in models.js already states the rule this was missing: the tolerance is right for scenery at 20 m and wrong for the thing in their hand.
    ['assets/stone_tools/stone_pick.vox', 'item'],                                          // …and the STONE PICK, the rock-breaking counterpart to the axe (user)
    ['assets/stone_tools/stone_shovel.vox', 'item'],                                        // …and the STONE SHOVEL, which digs ground and nothing else (user)
    ['assets/stone_tools/bow_arrow/arrow.vox', 'item'],                                     // …and the ARROW that lies on the bow (user)
    ['assets/stone_tools/bow_arrow/bow/base.vox', 'bow'],                                   // …the BOW: ONE multi-model file whose models are its DRAW FRAMES (user), parsed in SHARE mode so the frames' shared colours cost no palette entries
    ['assets/food/meat/steak/base.vox', 'item'],                                            // …and RAW MEAT, which a killed land mammal leaves behind (user). MOVED into steak/ (user 2026-08-19): the folder now holds the source AND the 21 carved eat frames tools/eat_frames.py writes beside it, the same base.vox + numbered-frames layout every animated model in assets/life uses. The frames are the readable copy — the strip the game actually shows is still carved at load by eatStrip
    ['assets/stone_tools/stone_hoe.vox', 'item'],                                           // …and the STONE HOE and STONE SPEAR, carried like the rest of the kit (user)
    ['assets/stone_tools/stone_spear.vox', 'item'],
  // ── THE CHERRY FOREST'S AUTHORED TWIGS (user 2026-08-18: "reload the sticks ... using the sticks labeled
  // pink_stick_1 and pink_stick_2") ── these REPLACE a derived recolour that used to live in this file. Authored
  // art beats a hue rotation, and it costs almost nothing here: the two files reuse stick_1/stick_2's browns
  // EXACTLY (verified byte-for-byte on the palettes), so 'held' — which is share-plus-exact-match-only — reuses
  // every brown id already minted and mints only the four pinks of the leaf.
  // APPENDED AT THE END of DECOR_LOAD deliberately: the destructure below is positional, so an entry inserted
  // beside its relatives renames every model after it. The same lesson the held-item table taught the same day.
    ['assets/decoration/pink_stick_1.vox', 'held'],
    ['assets/decoration/pink_stick_2.vox', 'held'],
  // ── THE MEADOW FLOWERS (user 2026-08-18: "replace all the flowers in the current game with flowers.vox") ──
  // ONE multi-model file whose models are VARIANTS, not frames — five different flowers, so 'variants' rather
  // than 'decor'. 'decor' would have read only the first of the five (parseVoxModel takes the first SIZE/XYZI
  // pair) and silently planted the same flower everywhere.
  // Appended at the END for the reason the pink twigs above are: the destructure below is POSITIONAL.
    ['assets/decoration/flowers.vox', 'variants'],
];
  const decorBytes = await Promise.all(DECOR_LOAD.map((d) => fetchBytes(d[0])));
  const [CONEV, lilyS, lilyM, lilyL, stk1, stk2, LOGV, ROCKV, PICKV, SHOVV, ARROWV, BOWSTRIP, MEATV, HOEV, SPEARV, pstk1, pstk2, FLOWERV0] =
    DECOR_LOAD.map((d, i) => d[1] === 'bow' ? parseBowStrip(decorBytes[i], d[0])
      : d[1] === 'variants' ? parseVoxVariants2(decorBytes[i], d[0])                 // a VARIANT SET — see the flowers entry above
      : (d[1] === 'item' || d[1] === 'held') ? parseVoxShared(decorBytes[i], d[0])   // 'held' is 'decor' plus the exact-colour exemption - see rock.vox above
      : parseVoxDecor(decorBytes[i], d[0], d[1] === 'own'));
  const CONEVL = CONEV ? { sx: CONEV.sx, sy: CONEV.sz, sz: CONEV.sy,                        // the same cone tipped 90° onto its side — fallen cones lie on the forest floor
    vox: CONEV.vox.map((p) => (p & 255) | (((p >> 16) & 255) << 8) | ((CONEV.sy - 1 - ((p >> 8) & 255)) << 16) | (p & 0xff000000)) } : null;
  const LILYV = [lilyS, lilyM, lilyL].filter(Boolean);
  // ── THE FLOWER SET, AND THE TWO ID SETS THE REST OF THE GAME ASKS ABOUT ── derived from the models' OWN
  // voxels, never from a literal id. That is the standing rule for decor and it is not stylistic: the palette
  // is not boot-stable (assets are parsed in array order now, but edCol still mints during play), so an id
  // written down here would be a different colour on another load.
  //   FLOWERIDS  — every id the variants use. This is the MATERIAL set, and it inherits exactly what BLOOM's
  //                six ids used to carry: floatTab (surface scatter — no hitbox, the aim ray passes through,
  //                and ORPHAN_OK so the generation sweep may clear a stranded one) plus the snow pass-through.
  //   FLOWERHEAD — the PETAL ids only, i.e. everything that is not stem or leaf green. This is what the bees
  //                forage on (sim/life/slots.js BLOOM_TAB): a bee belongs on the bloom, not halfway down the
  //                stalk, and the old single-voxel flower had no stem to land on by mistake. Green is ASKED OF
  //                THE PALETTE rather than hard-coded, the same test the pink twigs' leaf recolour uses.
  const FLOWERV = (FLOWERV0 || []).filter((m) => m && m.vox.length);
  // ── AND NO FLOWER MAY WEAR A PINE'S NEEDLE ── parseVoxVariants2 reads this file in SHARE mode (PAL_TOL 6),
  // which is right for scenery and has one consequence nothing downstream can undo: an id is a MATERIAL, so a
  // flower colour that resolves onto a colour the CANOPY already owns hands the pine every table the flower is
  // later given. Measured: the stem's upper green (74,114,51) landed on a pine needle id carrying 13.4% of the
  // crown voxels above ground in a 193x193 patch of pine forest, and it was already costing the tree two things
  // nobody asked for — floatTab (assets/material-tabs.js marks every FLOWERIDS id surface scatter) and the snow
  // pass-through in ui/settings.js, so a flake fell THROUGH one needle in seven instead of settling on it. Making
  // the flowers choppable would have added a third and much worse one: decorTab on a needle puts the pine's own
  // crown in front of the swing as decor, which is the "aiming at the trunk gets the leaves" complaint the whole
  // leaf-vs-trunk rule in sim/tools.js exists to answer.
  // FOLDED, NOT EXCLUDED. Leaving the id out of the flower's tables instead would leave one stem voxel per plant
  // that no tool can take — and this is the stem, at z=1 of an 8-tall model, so every broken flower would keep a
  // green stub. Re-pointing it at the flower's OTHER stem green costs one voxel a shift of 15/255 in green on a
  // 2-voxel stalk and hands the plant a single flat stem colour, which is what the model reads as anyway.
  // DERIVED BOTH WAYS, because the palette is not boot-stable: the ids to fold are "whatever this file resolved
  // onto a canopy or bark id" (foliageIds/woodIds are already populated by assets/palette.js from pine5.vox), and
  // the host is the flower's most-used GREEN that no tree claims — the same green test FLOWERHEAD is built on.
  // If a re-author ever leaves no unclaimed green at all this refuses rather than guesses, and the decorTab line
  // in material-tabs.js has the matching belt-and-braces so a canopy id can never reach it either way.
  {
    const isGrn = (i) => { const c = palette[i]; return !!c && c[1] > c[0] && c[1] >= c[2]; };
    const claimed = (i) => foliageIds.indexOf(i) >= 0 || woodIds.indexOf(i) >= 0;
    const cnt = new Map();
    for (const m of FLOWERV) for (const q of m.vox) { const i = q >>> 24; cnt.set(i, (cnt.get(i) || 0) + 1); }
    const bad = [...cnt.keys()].filter(claimed);
    let host = -1, hn = -1;
    for (const [i, n] of cnt) if (isGrn(i) && !claimed(i) && n > hn) { hn = n; host = i; }
    if (bad.length && host >= 0) {
      const bs = new Set(bad);
      for (let k = 0; k < FLOWERV.length; k++) { const m = FLOWERV[k];
        FLOWERV[k] = { sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox.map((q) => (bs.has(q >>> 24) ? (q & 0xffffff) | (host << 24) : q)) }; }
      console.log('[vb] flowers: tree-claimed ids', bad.join('/'), '-> own green', host, palette[host]);
    } else if (bad.length) console.warn('[vb] flowers wear tree ids', bad.join('/'), 'and have no green of their own to fold onto — those voxels stay out of decorTab');
  }
  const FLOWERIDS = [], FLOWERHEAD = [];
  { const seen = new Set();
    for (const m of FLOWERV) for (const q of m.vox) { const fid = q >>> 24;
      if (seen.has(fid)) continue; seen.add(fid); FLOWERIDS.push(fid);
      const fc = palette[fid]; if (fc && !(fc[1] > fc[0] && fc[1] >= fc[2])) FLOWERHEAD.push(fid); } }
  // ── THE PINK VARIANT IS THE BLOSSOM BAND'S, AND ONLY THE BLOSSOM BAND'S (user 2026-08-18: "I actually just
  // made a pink flower in the flower.vox folder. use that instead") ── it is authored, not derived: an earlier
  // cut recoloured the white flower's three near-whites through a minted pink ramp, and authored art beats a
  // recolour every time. It also costs nothing, because the file was already being loaded.
  // SPLIT BY MEASUREMENT, NOT BY INDEX. It is model 0 today and adding it pushed every other variant down one —
  // which is precisely why nothing here may count on position. The pink flower is "the variant whose petals are
  // pink-hued", so re-exporting the file in any order, or adding a seventh flower, cannot silently plant pink
  // through the oak forest or strand the blossom band with a yellow one.
  // Hue 330..360 with a saturation floor separates it from the RED flower at hue 0, which is the only other
  // warm variant; the amber centre and green stems every model shares fall outside the band.
  const flowerIsPink = (id) => {
    const c = palette[id]; if (!c) return false;
    const mx = Math.max(c[0], c[1], c[2]), mn = Math.min(c[0], c[1], c[2]);
    if (mx < 150 || mx === 0 || (mx - mn) / mx < 0.15) return false;   // too dark, or too grey to have a meaningful hue
    const d = mx - mn;
    const h = mx === c[0] ? (((60 * (c[1] - c[2])) / d) + 360) % 360
            : mx === c[1] ? (60 * (c[2] - c[0])) / d + 120
                          : (60 * (c[0] - c[1])) / d + 240;
    return h >= 330 && h < 360;
  };
  const FLOWERV_CH = [];
  {
    const pinkN = FLOWERV.map((m) => m.vox.reduce((n, q) => n + (flowerIsPink(q >>> 24) ? 1 : 0), 0));
    const best = pinkN.indexOf(Math.max(...pinkN));
    if (best >= 0 && pinkN[best] > 0) {
      FLOWERV_CH.push(FLOWERV[best]);
      FLOWERV.splice(best, 1);                         // …and OUT of the general set, so the oak forest never plants it
    }
  }
  // ── AND ITS LIGHTER TWIN (user 2026-08-19: "basically just make half the current pink flowers a lighter pink
  // like the lighter tree") ── the blossom band ships two cherry varieties, and until now its flower had one
  // colour, so a pale tree stood over dark pink flowers. This is the SAME arrangement the crowns already use:
  // one authored model, its petal ids remapped onto the other variety's ramp (blosRemap, a few hundred lines
  // below), and it costs ZERO palette entries — BLOSWHITE is already minted, already palOwn-reserved, and is
  // literally "the lighter tree" the user is asking these flowers to match.
  // BY LUMINANCE RANK, not by nearest colour, and for the reason the crown map records: the two ramps barely
  // overlap, so a nearest-colour match would collapse every pink onto BLOSWHITE's darkest step and the flower
  // would lose its shading. Rank spreads the petals across the whole ramp however many shades either has.
  // ONLY the petals move. flowerIsPink is the same hue test that found this model in the first place, so the
  // amber centre and the green stem every flower shares fall outside it and pass through untouched — a light
  // flower is the pink one with pale petals, not a bleached plant.
  // The 50/50 needs no code: flowerAt draws `k` over the set's length on a COARSE patch cell, so two entries
  // give patches of dark pink and patches of light in the same drifts every other flower colour already forms.
  if (FLOWERV_CH.length === 1 && typeof BLOSWHITE !== 'undefined' && BLOSWHITE.length) {
    const src9 = FLOWERV_CH[0];
    const flum = (i) => { const c = palette[i]; return c ? c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114 : 0; };   // local: bow.js's own lum0 is declared later, inside the oak loader
    const pinks9 = [...new Set(src9.vox.map((q) => q >>> 24))].filter(flowerIsPink).sort((a, b) => flum(a) - flum(b));
    if (pinks9.length) {
      const m9 = new Map();
      // ONE PETAL SHADE IS THE CASE THAT ACTUALLY HAPPENS: the authored flower paints all its petals in a
      // single pink, so the rank spread below is the general path and this is the one that runs. It lands on
      // BLOSWHITE.length - 2, NOT the top of the ramp — the top step is (247,235,238), which is so close to
      // white that the flower stopped reading as pink at all, and the user asked for a lighter PINK. One step
      // down is the exact colour the light cherry's own falling petal already uses (PETALW_IT, taken from the
      // middle of this ramp), so a pale flower and the petal drifting onto it are the same shade by construction.
      pinks9.forEach((id, i) => { const t = pinks9.length < 2 ? Math.max(0, BLOSWHITE.length - 2)
        : Math.round(i * (BLOSWHITE.length - 1) / (pinks9.length - 1));
        m9.set(id, BLOSWHITE[t]); });
      FLOWERV_CH.push({ sx: src9.sx, sy: src9.sy, sz: src9.sz,
        vox: src9.vox.map((q) => (m9.has(q >>> 24) ? (q & 0xffffff) | (m9.get(q >>> 24) << 24) : q)) });
      console.log('[vb] cherry flowers:', FLOWERV_CH.length, 'varieties —', pinks9.length, 'petal shades remapped onto BLOSWHITE');
    }
  }
  // ── THE LAVENDER SPIKE GETS A RAMP (user 2026-08-18: "create 3 more shades for the lavender flower ... 3 more
  // purple shades I mean") ── the model paints all 17 of its petal voxels in ONE purple, which at 3x3x8 is the
  // largest flat block of colour in the set. FLOWPURP adds three shades around the authored one and the voxel's
  // own position picks between them, the same dither the blossom crowns use and for the same reason: the art
  // supplies one colour, so a per-id remap could never show four.
  // FOUND BY HUE, not by index — it is model 3 today and was model 2 before the pink flower was added, which is
  // exactly the drift this avoids. 250..300 is violet, clear of the blue flower below it and the pink above.
  if (FLOWPURP.length) {
    const hueOf = (c) => { const mx = Math.max(c[0], c[1], c[2]), mn = Math.min(c[0], c[1], c[2]), d = mx - mn;
      if (!d || mx < 60) return -1;
      return mx === c[0] ? (((60 * (c[1] - c[2])) / d) + 360) % 360
           : mx === c[1] ? (60 * (c[2] - c[0])) / d + 120 : (60 * (c[0] - c[1])) / d + 240; };
    const isViolet = (id) => { const c = palette[id]; if (!c) return false; const h = hueOf(c); return h >= 250 && h < 300; };
    let vi = -1, vn = 0;
    FLOWERV.forEach((m, i) => { let n = 0; for (const q of m.vox) if (isViolet(q >>> 24)) n++; if (n > vn) { vn = n; vi = i; } });
    if (vi >= 0 && vn > 0) {
      const m = FLOWERV[vi];
      const vlum = (i) => { const c = palette[i]; return c ? c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114 : 0; };   // local: bow.js's own lum0 is declared several hundred lines BELOW this, so using it here is a temporal dead zone and a black screen at boot
      const ramp = [...new Set(m.vox.map((q) => q >>> 24).filter(isViolet).concat(FLOWPURP))]
        .sort((a, b) => vlum(a) - vlum(b));   // dark -> light, the authored shade taking its place among the three
      FLOWERV[vi] = { sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox.map((q) => {
        if (!isViolet(q >>> 24)) return q;                      // stems and the blue tip are left alone
        const x = q & 255, y = (q >> 8) & 255, z = (q >> 16) & 255;
        return (q & 0xffffff) | (ramp[((x * 131 + y * 61 + z * 17) & 1023) % ramp.length] << 24);
      }) };
      for (const i of FLOWPURP) { if (!FLOWERIDS.includes(i)) FLOWERIDS.push(i); if (!FLOWERHEAD.includes(i)) FLOWERHEAD.push(i); }
    }
  }
  console.log('[vb] flowers.vox', FLOWERV.length, 'variants,', FLOWERIDS.length, 'ids of which', FLOWERHEAD.length, 'are petal (the rest are stem/leaf) +', FLOWERV_CH.length, 'pink twin for the blossom band');
  // ── AND THEIR BLOSSOM TWINS (user 2026-08-18: "change the green leaf on the stick to a pink leaf") ──
  // built at LOAD from the models already parsed, the same derivation berryBush does on the oak bush a few
  // hundred lines below, and for the same reason: nothing here re-shapes a twig, it re-colours the four leaf
  // voxels of one and the seven of the other. Baking two more .vox files would ship two more models to express
  // a recolour that is a pure function of the models on disk.
  // The leaf is found by ASKING THE PALETTE which of the twig's ids are green, not by hard-coding indices:
  // stick_1 and stick_2 are 'held' models, so their ids come out of palShare in whatever order the parse ran,
  // and an index written here would rot the first time an unrelated decoration minted before them.
  const STICKV = [stk1, stk2].filter(Boolean);
  let STICKBIRCH = [];                                 // the same two twigs in BIRCH colours — filled after the birch load below, because it needs the bark ids that load MINTS
  const STICKB = [pstk1, pstk2].filter(Boolean);       // the AUTHORED pink twigs, index-parallel to STICKV so world/terrain.js picks a pair rather than a model. Empty if the two files are missing, and stickAt falls back to the green pair
  // The leaf ids the pink twigs actually carry, read off the models rather than assumed: the pickup has to remove
  // them (see twigLeafCells in sim/projectiles.js) and they are NOT the canopy's BLOSLEAF — the user authored a
  // ramp of their own. Told apart from the twig browns by channel order: a brown is r>g>b, these pinks are r>b>g.
  const TWIGPINK = [...new Set(STICKB.flatMap((m) => m.vox.map((q) => q >>> 24))
    .filter((id) => { const c = palette[id]; return c && c[2] > c[1]; }))];
  const ROCKVU = ROCKV ? { sx: ROCKV.sx, sy: ROCKV.sz, sz: ROCKV.sy,                        // the same stone tipped 90° onto its edge — half the field stones stand upright
    vox: ROCKV.vox.map((p) => (p & 255) | (((p >> 16) & 255) << 8) | ((ROCKV.sy - 1 - ((p >> 8) & 255)) << 16) | (p & 0xff000000)) } : null;
  let ROCK26 = [], ROCK26D = [], R26S = [], R26M = [], R26B = [];
  const R26DMAP = new Array(256).fill(0);   // rock palette id -> its Colorado-sandstone twin; the workers rebuild ROCK26D from this                                         // 26 pre-voxelized rocks from rocks.glb (see scratch voxelize_rocks.py) in 3 rarity tiers
  try {
    const rj = await (await fetch('assets/decoration/rocks26.json')).json();
    const rids = rj.pal.map((c) => addCol(c[0], c[1], c[2]));                               // 12 shared quantized shades for all 26 rocks
    ROCK26 = rj.rocks.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (rids[p >>> 24] << 24)) }));
    rj.rocks.forEach((r, i) => { (r.grp === 'big' ? R26B : (r.grp === 'mid' ? R26M : R26S)).push(i); });
    // ── THE DESERT COPY, IN COLORADO SANDSTONE ── the same 26 models with their ids remapped onto REDROCK.
    // A separate array is required, not a recolour in place: ROCK26 is also what the FOREST boulders stamp,
    // so remapping it would turn every boulder in the pine woods red too.
    // REDROCK was declared in palette.js and used by NOTHING — four already-minted ids labelled "Colorado
    // sandstone strata", which is exactly this. So the desert rocks cost ZERO new palette entries, and since
    // nothing else wears those ids, giving them decorTab/pickOnly cannot leak onto another material.
    // Mapped by RELATIVE lightness: the two ranges do not overlap, so a nearest-colour match would collapse
    // all twelve rock shades onto one sandstone shade and flatten the rock (measured when the desert rocks
    // wore sand). Ranking within each palette keeps the model's own shading.
    // ── …AND IT IS ONLY BUILT WHILE R26D_ON (2026-08-17) ── REDROCK is an EMPTY array when the switch in
    // assets/palette.js is off, and an empty `red` would index red[-1] = undefined, write `undefined << 24`
    // (id 0) into every ROCK26D voxel and fill R26DMAP with undefined. Nothing reads either one today, so that
    // would be invisible rather than harmful — which is exactly the kind of quiet wrong state to refuse.
    if (REDROCK.length)
    { const lum = (c) => c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114;
      const red = REDROCK.slice().sort((a, b) => lum(palette[a]) - lum(palette[b]));
      const rl = rj.pal.map(lum), lo = Math.min(...rl), hi = Math.max(...rl);
      const map = rj.pal.map((c) => { const t = hi > lo ? (lum(c) - lo) / (hi - lo) : 0.5;
        return red[Math.min(red.length - 1, Math.floor(t * red.length))]; });
      ROCK26D = rj.rocks.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (map[p >>> 24] << 24)) }));
      // …and a 256-entry id->id lookup so the GEN WORKERS can rebuild ROCK26D from ROCK26 instead of being
      // handed a second copy of it. gen-pool.js stringifies every registered table straight into each worker's
      // source, and these models are 347k voxels: shipping ROCK26D as a table doubled that into every worker
      // and the boot stopped completing. A 256-number array costs nothing.
      for (let i = 0; i < rids.length; i++) R26DMAP[rids[i]] = map[i]; }
  } catch (e) { console.warn('[vb] rocks26.json missing — GLB rocks skipped', e); }
  let CACTI = [];                                                                           // 9 desert cacti, one .vox per variant (tools/voxelize_cacti.py bakes these from cactus.glb)
  try {
    if (location.search.includes('nocacti')) throw new Error('?nocacti');   // A/B switch: an empty CACTI disables the scatter, the solid marking and the stamp in one go
    const CACN = 9;
    const cbytes = await Promise.all(Array.from({ length: CACN }, (_, i) =>                 // fetched together, PARSED IN ORDER — parsing is what mints palette ids, and doing it as each response lands makes id assignment a race (see the DECOR_LOAD note above)
      fetchBytes('assets/foilage/cactus/cactus_' + (i + 1) + '.vox')));
    // SHARE + noTol. Every file carries the SAME baked 16-shade palette, so the first one mints those 16 and
    // the other eight hit them as exact matches and cost nothing — 16 ids for all nine plants, which is what
    // the old cacti.json loader produced. noTol matters: a TOLERANCE reuse could hand a cactus an id some
    // other decoration already owns, and markSolid(CACTI) would then make that decoration solid too. Measured
    // before switching: none of the 16 shades exists anywhere else in the palette, so the set stays private.
    CACTI = cbytes.map((b, i) => b ? parseVoxModel(b, true, true) : null).filter(Boolean);
    if (CACTI.length !== CACN) console.warn('[vb] cacti: only ' + CACTI.length + ' of ' + CACN + ' .vox loaded');
  } catch (e) { console.warn('[vb] cactus .vox missing — cacti skipped', e); CACTI = []; }
  // ── DESERT SHRUBS ── six HAND-AUTHORED scrub plants, 0.6-1.3 m, at the CACTI'S OWN 10 cm voxel, which is
  // what makes a 1.3 m bush sit believably beside a 4.4 m saguaro. THE .vox FILES ARE THE ASSET — fetched
  // here directly, exactly the way the cacti above are. Named 1.vox…6.vox (user 2026-08-16: "I made
  // modifications and renamed the files"); they were shrub_1…shrub_14 and this loader still asked for those,
  // which is the entire reason no shrub had been rendering. tools/voxelize_desert_shrubs.py wrote the OLD
  // names and is now a dead end for this folder — re-running it would bury the user's art under 14 bakes.
  // ── NOT THE CACTUS'S IDS, EVEN WHERE THE COLOUR IS THE CACTUS'S ── material-tabs.js runs markSolid(CACTI)
  // and cactusTab over every cactus id, FLOWER IDS INCLUDED, so one shared id would give a knee-high bush a
  // saguaro's hitbox and make brushing past it STING. So this is neither of the two loaders around it.
  // Not share mode — palShare would find those very ids on an exact colour match, and PAL_TOL would find them
  // on a near one. Not plain addCol — that mints, on a table with 7 entries left. It is parseVoxModel's
  // colMap, PRE-BUILT below over the shrubs' own reserved ids (SHRUBC + SHRUBF, palette.js). Every colour in
  // every file resolves out of that map, so nothing is minted and nothing is shared: cost ZERO further ids.
  // ── AND IT IS BUILT BY READING THE FILES, NOT BY PINNING THEM ── the old contract was that the bake emitted
  // exactly len(SHRUBC) shades and this map was keyed on those literal RGBs, so a hand-edited file with one
  // extra shade missed the map and fell through to addCol on a full table. These files ARE hand-edited, and
  // they carry 14 shades against 10 ids. voxColsUsed reads what each file actually references first, then each
  // shade is resolved to the NEAREST shrub-owned colour. Nearest is safe here in the way it is not for the
  // desert rocks: the candidate set is the shrubs' own ten, drawn from these very files, so the ranges overlap
  // instead of sitting apart — the greens land on greens and the flowers on flowers with room to spare
  // (the nearest green/flower pair is 142 apart in RGB, against 3 between adjacent greens). The worst error is logged every boot; the shrubs are
  // the only thing that can ever wear these ids, so a wrong resolve here is a wrong colour and never a
  // wrong material.
  let SHRUBV = [];
  try {
    const SHRN = 6;
    const sbytes = await Promise.all(Array.from({ length: SHRN }, (_, i) =>                // fetched together, parsed in order — the same reason the cacti are, see the DECOR_LOAD note above
      fetchBytes('assets/foilage/desert_shrub/' + (i + 1) + '.vox')));
    const sok = sbytes.filter(Boolean);
    // ── TWO MAPS, NOT ONE (user 2026-08-17: make shrubs 5 and 6 brown) ── the resolve is nearest-colour onto
    // the shrubs' OWN ids, so which ids a file is ALLOWED to reach is the only thing that decides its colour.
    // Files 1-4 may reach the greens, files 5-6 the browns, and both may reach the flowers. The .vox files are
    // untouched: repainting one brown would have resolved it straight back to the nearest green, because green
    // was the only body colour on offer. Worst-error tracking is per map for the same reason it existed before
    // - it is the alarm that says a file has drifted away from the ramp it is being held against.
    const SHRUB_BROWN0 = 4;                            // 0-based: files 5.vox and 6.vox
    const smapFor = (ids) => {
      const m = new Map(); let worst = 0, wcol = null;
      for (const b of sok) for (const c of voxColsUsed(b)) {
        const ck = (c[0] << 16) | (c[1] << 8) | c[2];
        if (m.has(ck)) continue;
        let bd = 1e9, bi = ids[0];
        for (const q of ids) { const p = palette[q], d = (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]);
          if (d < bd) { bd = d; bi = q; } }
        const bp = palette[bi], e = Math.max(Math.abs(bp[0] - c[0]), Math.abs(bp[1] - c[1]), Math.abs(bp[2] - c[2]));
        if (e > worst) { worst = e; wcol = c; }
        m.set(ck, bi);
      }
      return { m, worst, wcol };
    };
    const sgreen = smapFor(SHRUBC.concat(SHRUBF)), sbrown = smapFor(SHRUBB.concat(SHRUBF));
    const sworst = sgreen.worst, sworstCol = sgreen.wcol;
    // Mapped over sbytes, not sok, so the brown cut-off counts FILES and not survivors: with sok a single
    // missing .vox would shift every later file one place and quietly recolour a green shrub.
    SHRUBV = sbytes.map((b, i) => (b ? parseVoxModel(b, false, false, (i >= SHRUB_BROWN0 ? sbrown : sgreen).m) : null)).filter(Boolean);
    if (SHRUBV.length !== SHRN) console.warn('[vb] shrubs: only ' + SHRUBV.length + ' of ' + SHRN + ' .vox loaded');
    console.log('[vb] shrubs:', SHRUBV.length, 'models,', sgreen.m.size, 'authored shades ->', SHRUBC.length + ' green /', SHRUBB.length + ' brown (files ' + (SHRUB_BROWN0 + 1) + '-' + SHRN + ') +', SHRUBF.length + ' flower, worst error green', sgreen.worst + '/255', 'brown', sbrown.worst + '/255');
    if (sworst > PAL_TOL) console.warn('[vb] shrubs: a shade is ' + sworst + '/255 from the nearest shrub id (' + sworstCol + ') — past PAL_TOL, so the .vox has moved away from SHRUBC/SHRUBF in palette.js');
  } catch (e) { console.warn('[vb] desert_shrub .vox missing — shrub decor skipped', e); SHRUBV = []; }
  let DROCK = [], DROCKS = [], DROCKM = [], DROCKB = [];                                                  // desert rocks from desert_rocks.glb (tools/voxelize_desert_rocks.py), small + mid tiers
  try {
    // REMOVED from the desert (user 2026-08-15). Skipped at the LOADER, not just at the scatter, so the four
    // sand-coloured ids are never minted and the palette keeps them — the table is at its ceiling. ?rocks
    // brings the whole thing back (the bake, the tiers, the tabs and the stamp pass are all still here).
    if (!location.search.includes('rocks')) throw new Error('desert rocks disabled — add ?rocks to restore');
    const dj = await (await fetch('assets/decoration/desert_rocks.json')).json();
    // ── THE ROCKS WEAR THE DESERT FLOOR'S OWN COLOURS (user 2026-08-15) ── but on DEDICATED ids, not by
    // reusing DSAND's. An id carries material identity as well as colour: DSAND is digOnlyTab (shovel) and the
    // rocks are pickOnlyTab, and one id cannot be both — sharing would make the whole desert floor pick-only.
    // So these are deliberate duplicates of the sand RGBs, exactly the trick BROCK uses to repeat ROCK's greys
    // on its own ids. Costs 4 slots instead of the 10 the rocks' own quantized palette wanted.
    const sandRGB = DSAND.map((i) => palette[i]);
    const sandIds = sandRGB.map((c) => addCol(c[0], c[1], c[2]));
    const lum = (c) => c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114;
    const sandL = sandRGB.map(lum);
    // Map by RELATIVE lightness, not absolute. The two ranges do not overlap — every rock shade is darker than
    // every sand shade — so a nearest-luminance match collapsed all ten onto the single darkest sand and left
    // the rocks flat and 3 minted slots unused. Ranking within each palette keeps the rock's own shading.
    const order = [...sandIds].map((id, i) => i).sort((a, b) => sandL[a] - sandL[b]);         // sand shades, darkest first
    const rl = dj.pal.map(lum), rlo = Math.min(...rl), rhi = Math.max(...rl);
    const dids = dj.pal.map((c) => {
      const t = rhi > rlo ? (lum(c) - rlo) / (rhi - rlo) : 0.5;                               // 0 = this rock's darkest shade, 1 = its lightest
      return sandIds[order[Math.min(order.length - 1, Math.floor(t * order.length))]];
    });
    DROCK = dj.rocks.map((r) => ({ sx: r.sx, sy: r.sy, sz: r.sz, vox: r.vox.map((p) => (p & 0xffffff) | (dids[p >>> 24] << 24)) }));
    dj.rocks.forEach((r, i) => { (r.grp === 'big' ? DROCKB : (r.grp === 'mid' ? DROCKM : DROCKS)).push(i); });   // three tiers off the .glb's own mesh names
  } catch (e) { console.warn('[vb] desert_rocks.json missing — desert rocks skipped', e); DROCK = []; }
  // ── THE OAK TREES (user 2026-08-17: "voxelize the oak_trees.glb file") ── seven broadleaf trees, 2.4 m to
  // 11.7 m, baked from source/glb/oak_trees.glb by tools/voxelize_oaks.py at the SAME 10 cm voxel as every
  // other asset in this game. The .json IS the asset: editing the .glb changes nothing on its own, exactly as
  // it does not for the cacti and the rocks — re-run the tool.
  // ORDER MATTERS AND IS THE TOOL'S: `trees` is emitted sorted by height, so the bake's [0] is the 2.4 m bush and
  // its [6] is the 11.7 m oak. world/terrain.js picks its size tiers by index off that — but note the BERRY block
  // further down splices the bush into two, so the LIVE OAKV is eight models and every index above the bush tier
  // is one higher than the bake's. oakAt's size ladder is written against the live array, not against this line.
  // ── SEVEN SHADES, AND ONLY FOUR OF THEM COST ANYTHING ── the three bark shades are repointed onto the
  // pine's existing bark ids and the four leaf shades are minted as the oaks' OWN and reserved in palOwn.
  // Both halves of that are explained where they happen, inside the try below.
  // ── THE DITHER THAT LETS A FOUR-GREEN CROWN SHOW EIGHT PINKS ── the oak art carries four distinct leaf
  // greens, so a straight id -> id remap can only ever put four colours on a crown; the other four shades of an
  // eight-step ramp were minted and never written. Each green instead names a BAND of the ramp and the voxel's
  // own position picks within it, so the whole ramp appears and the crown gains a fine mottle rather than four
  // flat plateaus — which is what blossom actually looks like.
  // DETERMINISTIC AND POSITION-KEYED, which both matter: the gen workers rebuild these sets from OAKV
  // themselves (world/gen-pool.js), so main thread and worker must agree voxel for voxel or gtest splits. The
  // hash reads only x/y/z out of the packed voxel, so it cannot drift.
  // A FUNCTION DECLARATION, not a const arrow: it is stringified into every gen worker, and the worker's
  // OAKBLOSV line runs at startup — a const would be in its temporal dead zone there.
  function blosRemap(m, ramp, rank, fruit) {
    const top = ramp.length - 1;
    let rmax = 0;
    for (let i = 0; i < 256; i++) if (rank[i] > rmax) rmax = rank[i];   // how many leaf greens the art really has; never assume four
    return { sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox.map((p) => {
      const r = rank[p >>> 24];
      if (r < 0) return p;                             // bark, and every id a crown carries that is not leaf, passes straight through
      const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
      // ── THE CHERRIES ── a leaf voxel becomes fruit instead of blossom, on its own hash so the scatter does
      // not correlate with the shade dither below (sharing one would put every cherry on the same tone).
      // 3/2048 is ~0.15%: on the giant crown's ~77,000 leaf voxels that is a hundred or so, which reads as a
      // fruiting tree, and on the bush tiers only a handful. Tested first at 1% and the crowns went red.
      // Keyed on POSITION like everything else here, so a tree keeps its cherries across regeneration and the
      // gen workers place them identically — the gtest invariant this whole function lives under.
      if (fruit && (((x * 191 + y * 37 + z * 101) & 2047) < 3)) return (p & 0xffffff) | (fruit << 24);
      const base = rmax ? Math.round(r * top / rmax) : 0;
      const j = (((x * 73 + y * 151 + z * 199) & 1023) % 3) - 1;   // -1, 0 or +1 — one step either side of the band centre
      const q = base + j;
      return (p & 0xffffff) | (ramp[q < 0 ? 0 : (q > top ? top : q)] << 24);
    }) };
  }
  // ── THE BIRCH DECODER ── base64 delta-varint -> Int32Array of packed voxels, x | z<<8 | y<<16 | c<<25.
  // NOTE THE SHIFTS ARE NOT THE OAK'S: a birch reaches 264 voxels tall and the oak layout gives height only
  // eight bits, so height has NINE here and the colour seven. The colour stays a 0..6 INDEX into BIRCHIDS
  // rather than becoming a palette id the way the oak repack does — seven bits cannot hold an 8-bit id, and
  // the stamp maps it through BIRCHIDS anyway. Registered for the gen workers in world/gen-pool.js.
  // ── WHERE THE TRUNK ACTUALLY IS ── the centroid of the WOOD in the model's bottom few layers, in model
  // coordinates. stampBirch centres a model on its BOUNDING BOX, and on these trees the box centre is not
  // the trunk: measured across the 26, the bole sits up to 83 voxels (8.3 m) away from it, because a crown
  // leans. Seating the tree on the ground under the BOX therefore samples the wrong column, and on any slope
  // the tree ends up hanging in the air on the downhill side - which is the "birch floating off the ground"
  // the user spotted. Sampling under the TRUNK is the fix, and this is the number that makes it possible.
  const birchTrunkC = (vox, sx, sy, sz) => {
    // THE DENSEST WOOD COLUMN, NOT THE CENTROID. A mean over the base layers averages across whatever wood
    // touches the ground - roots, a low branch, the bole - and lands BETWEEN them, in empty space: measured on
    // model 19, the centroid picked a column holding two stray bark voxels 22 above the ground while the bole
    // stood elsewhere, so the tree was seated to a column that has no trunk in it. The column with the MOST
    // wood in the lower third is the bole by construction, and it cannot be empty.
    const cnt = new Map();
    const lim = Math.max(4, sz / 3) | 0;
    for (let i = 0; i < vox.length; i++) { const q = vox[i];
      if (((q >> 16) & 511) < lim && (q >>> 25) < BIRCHNB) {
        const k = (q & 255) | ((q >> 8) & 255) << 8;
        cnt.set(k, (cnt.get(k) || 0) + 1);
      } }
    let best = -1, bk = -1;
    for (const [k, v] of cnt) if (v > best) { best = v; bk = k; }
    if (bk < 0) return { tcx: sx >> 1, tcy: sy >> 1, tbz: 0 };
    // ── AND HOW FAR UP THAT COLUMN THE BOLE ACTUALLY STARTS ── the model is trimmed to the extent of ALL its
    // voxels, LEAVES INCLUDED, so a low branch hanging near the ground sets z = 0 and the trunk can begin well
    // above it: measured on model 13, the bole's lowest wood sits at z = 28. Seating the model's BOX on the
    // ground then leaves the trunk starting 28 voxels up, hanging in the air over its own leaves - which is
    // the birch the user spotted floating. stampBirch subtracts this, which buries the low foliage instead;
    // mode 1 refuses those cells against the terrain, so the buried leaves simply do not stamp.
    const tcx = bk & 255, tcy = (bk >> 8) & 255;
    let tbz = 511;
    for (let i = 0; i < vox.length; i++) { const q = vox[i];
      if ((q & 255) === tcx && ((q >> 8) & 255) === tcy && (q >>> 25) < BIRCHNB) {
        const z = (q >> 16) & 511; if (z < tbz) tbz = z; } }
    return { tcx, tcy, tbz: tbz === 511 ? 0 : tbz };
  };
  const birchDec = (t) => {
    const b = atob(t.vox);
    const out = new Int32Array(t.n);
    let v = 0, sh = 0, acc = 0, k = 0;
    for (let i = 0; i < b.length; i++) { const c = b.charCodeAt(i);
      v |= (c & 127) << sh;
      if (c & 128) { sh += 7; } else { acc += v; out[k++] = acc; v = 0; sh = 0; } }
    return Object.assign({ sx: t.sx, sy: t.sy, sz: t.sz, wood: t.wood, vox: out }, birchTrunkC(out, t.sx, t.sy, t.sz));
  };
  // …and the inverse, because the models now arrive as .vox files and the gen workers still cannot be handed
  // 1.2M voxels of JavaScript source (world/gen-pool.js stringifies every registered table into the pool's
  // shared source, and a second copy of the OAKS alone once stopped a boot). So the main thread re-encodes what
  // it read into the same compact stream the .json used to carry, and each worker decodes its own copy.
  const birchEnc = (arr) => {
    const out = new Uint8Array(arr.length * 3);        // 3 bytes/voxel is a safe ceiling: the deltas measured 1-2
    let k = 0, prev = 0;
    for (let i = 0; i < arr.length; i++) {
      let d = arr[i] - prev; prev = arr[i];
      while (d >= 128) { out[k++] = (d & 127) | 128; d >>>= 7; }
      out[k++] = d;
    }
    let s = '';                                        // chunked: String.fromCharCode.apply blows the stack past ~100k args
    for (let i = 0; i < k; i += 8192) s += String.fromCharCode.apply(null, out.subarray(i, Math.min(k, i + 8192)));
    return btoa(s);
  };
  // ── WHICH TREE GETS PLANTED, AND WHY IT IS NOT A UNIFORM DRAW (user 2026-08-23: "the tops of the trees are
  // still cut off", three times) ── the tops are NOT cut. Audited: 723,536 model voxels wanted, 721,170
  // stamped, zero above WY, zero lost from any crown. What is actually happening is that the SOURCE SET is
  // half spindly. Measured over the twenty-six, voxels per layer runs 51 to 517, and the thin ones are thin in
  // the worst way - birch_18_2m carries its first leaf at 57% of its height, birch_25_4m at 70%. Those are
  // forest-interior trees that self-pruned, and they are perfectly real; a stand made mostly of them is a
  // field of bare poles with a tuft on each, which is exactly what "cut off" describes.
  // So the draw is WEIGHTED BY CROWN DENSITY rather than uniform: a tree appears in this table once per 60
  // voxels-per-layer, capped at 8, so the fullest are eight times likelier than the barest and the barest still
  // appear. It stays SORTED BY HEIGHT because birchAt's fits-under-the-sky guard walks a prefix of it.
  const birchPick = (v) => {
    // TWO MEASUREMENTS PER MODEL, both taken from the model itself so a re-baked or hand-edited .vox is judged
    // on what it actually contains:
    //   dens  - voxels per layer. How much tree there is per unit of height.
    //   bare  - how far up the FIRST leaf sits, as a fraction of total height.
    const stat = v.map((m) => {
      let lo = 1e9;
      for (let i = 0; i < m.vox.length; i++) { const q = m.vox[i];
        if ((q >>> 25) >= BIRCHNB) { const z = (q >> 16) & 511; if (z < lo) lo = z; } }
      return { dens: m.vox.length / Math.max(1, m.sz), bare: (lo === 1e9 ? 1 : lo / Math.max(1, m.sz)) };
    });
    // ── THE SPINDLY MODELS ARE NOT PLANTED (user 2026-08-23, after asking five times why "the tops of the
    // trees are cut off") ── they are not cut. Audited: 723,536 model voxels wanted, 721,170 stamped, zero
    // above the ceiling, zero lost from any crown; and a marker ladder proves the renderer draws to y=483.
    // What is actually there is the SOURCE SET. Half of these are forest-interior birches that self-pruned:
    // measured, voxels per layer runs 51 to 517, and birch_18_2m carries its first leaf at 57% of its height,
    // birch_25_4m at 70%, birch_26_4m at 62%. Rendered on their own they read exactly as the user describes -
    // a bare pole with a scattered tuft near the top, i.e. a tree whose top has been cut off.
    // Weighting the draw toward the fuller ones was not enough, because the thin ones still appeared. So they
    // are refused outright: dens >= 150 AND bare <= 0.5. Fourteen of the twenty-six pass, spanning 12.1 m to
    // 26 m, so the range of sizes survives.
    // ALL 26 .vox FILES STAY ON DISK AND STAY EDITABLE - this decides only what the WORLD plants. Fatten a
    // rejected tree's crown in MagicaVoxel and it starts being planted again, with no code change: the test is
    // on the art, not on a list of names.
    // ── THE GATE IS `bare` ALONE, AND dens IS ONLY A WEIGHT (2026-08-24) ── it used to gate on BOTH, with
    // `dens >= 150` typed against the 26-model set that existed when it was written: 14 passed and the forest
    // had variety. The set is hand-pruned now, the densest trees went with it, and the SAME absolute number
    // then admitted 4 of 16 — a whole biome built from four repeating trees, which is what "spread the birch
    // trees around the birch forest" is not. An absolute threshold over a set someone edits by hand is a
    // slow-acting bug; it does not fail when you change the number, it fails later when the set moves under it.
    // Nothing is lost by dropping it, because dens is ALREADY the draw weight below — a sparse tree that is
    // otherwise fine now appears rarely instead of never, which is what "weighted by fullness" should mean.
    // `bare` stays a hard gate because it is the one the original complaint was about: a first leaf above half
    // the tree's height reads as a bare pole with a tuft on it, i.e. "the tops of the trees are cut off".
    // ── EVERY MODEL IN THE FOLDER IS PLANTED (user 2026-08-24: "why cant you put all 16 models in the birch
    // forest? go ahead and implement all 16") ── the `bare <= 0.5` gate is gone with the dens one. It was
    // added when the set was the raw 26-model bake and half of them were forest-interior birches that had
    // self-pruned: first leaf at 57-70% of height, which rendered as a bare pole with a tuft on top and read
    // as "the tops of the trees are cut off". That gate was doing the job a CURATED folder does, and the
    // folder is curated now — hand-pruned from 26 to 16 and hand-edited since. Refusing the owner's own files
    // is the tool second-guessing the author, so it does not.
    // The FULLNESS WEIGHTING below is what remains, and it is enough: a sparse tree still appears, just less
    // often than a full one. If a bare-looking birch turns up again, the fix is to edit or delete that .vox —
    // which is now a real fix, because the files are what the game loads.
    const use = v.map((m, i) => i);
    console.log('[vb] birch: planting all', use.length, 'models, weighted by fullness');
    // …and the weight is RELATIVE to this set's own median, for the same reason the gate stopped being
    // absolute: it has to keep meaning "fuller than average here" however many trees are in the folder.
    const med = use.map((i) => stat[i].dens).sort((a, b) => a - b)[use.length >> 1] || 1;
    const out = [];
    for (const i of use) {
      const w = Math.max(1, Math.min(6, Math.round(3 * stat[i].dens / med)));
      for (let k = 0; k < w; k++) out.push(i);
    }
    return out;
  };
  // ── HOW MANY OF THE PACKED COLOUR INDICES ARE BARK ── every birch voxel carries its colour as a 0-based
  // index into the model palette (bark first, then the four leaf greens), so "is this wood or leaf" is the
  // test `idx < BIRCHNB`. It was written as a literal 3 in three places and the count then CHANGED to 4
  // (tools/birch_bark_white.py), which broke all three silently and in different directions: birchPick read
  // the fourth BARK shade as the lowest leaf and mis-measured how bare a model is, and birchTrunkC stopped
  // counting that shade as wood — the very test the tree-seating fix depends on. The loader assigns this from
  // the file, and world/gen-pool.js ships it to the workers, which rebuild BIRCHV through birchDec and would
  // otherwise disagree with the main thread about which voxels are trunk.
  let BIRCHNB = 3;
  let BIRCHV = [], BIRCHENC = [], BIRCHBARK = [], BIRCHIDS = [], BIRCHPICK = [], BIRCH_BANCH = [], BIRCH_ANCH = [];
  // -- WHERE A BEEHIVE HANGS IN A BIRCH (user 2026-08-24: "attach beehives to some of the birch trees") --
  // Registered as a FUNCTION and re-run per gen worker off that worker's own BIRCHV, exactly as birchPick is
  // (world/gen-pool.js) and for the same reason: shipping the table would mean snapshotting it at whatever
  // moment the pool assembled its source, and the birch models arrive from an async load. When it WAS shipped
  // as a table the workers got an empty list, so a worker-generated region grew the tree and not its hive,
  // and gtest reported 28 voxels of main-thread hive standing over worker air. Derived on both sides, the two
  // cannot disagree.
  // Declared here, above the async birch load that calls it, because anything below that load is unreachable
  // from inside it - the same temporal dead zone that already caught HIVEV and voxShellAir.
  // ── WHERE A LEAF LETS GO OF A BIRCH (user 2026-08-27: "have leaves fall in the birch forest like the other
  // trees") ── PINE_ANCH's rule and OAK_ANCH's, third time: a CANOPY voxel with clear cells directly beneath
  // it, so the leaf is visible from the frame it is born and does not appear out of the middle of the crown.
  // The three differences from birchBanch above are all the same difference — a leaf is not a beehive:
  //   * it wants LEAF voxels, so the bark test is inverted (index >= BIRCHBARK.length, the same index into
  //     BIRCHIDS that block uses, and for its reason: foliaTab is still empty this early in the load);
  //   * four clear cells below, not the hive's whole 5x5x5 box — one leaf needs somewhere to fall, not a
  //     place to sit, so the strict clearance would throw away most of the crown for nothing;
  //   * and a much higher cap, because this list is sampled once per shed rather than once per tree: 96 is
  //     PINE_ANCH's own number and it is what makes the fall ring the crown instead of pouring off one side.
  // Angle-sorted with the packed coordinate breaking ties, exactly as birchBanch is, so the list is a pure
  // function of the SET of voxels and a gen worker rebuilding BIRCHV from the varint cannot disagree with the
  // main thread about it.
  const birchLanch = (BV) => {
    // ── EDGE 1 -> 3, DROP 4 -> 7 (user 2026-08-27: "I see falling leaves off the birch tree flicker or
    // dissapear") ── a leaf born inside a canopy voxel is invisible until it falls clear of it, which is the
    // blink. Some of that is unavoidable in any wood, because crowns overlap and a cell this model calls
    // empty can hold a NEIGHBOUR's foliage: MEASURED at 18% of births in the oak and 17% in the pine. The
    // birch was 32%, nearly double, and the two reasons are both here. EDGE 1 took anchors hard against the
    // model boundary, which is exactly where a neighbouring crown is most likely to be standing; the oak's
    // own anchor list uses 3 and birchBanch beside this uses 2. And four clear cells is the shallowest drop
    // that can be called clear at all, so any overlap at all put a leaf straight back into foliage.
    const OUT = [], LMAX = 96, EDGE = 3, DROP = 7, NBK = BIRCHBARK.length;
    for (let k = 0; k < BV.length; k++) {
      const m = BV[k], sx = m.sx, sy = m.sy, sz = m.sz;
      const occ = new Uint8Array(sx * sy * sz);
      for (const q of m.vox) occ[(q & 255) + ((q >> 8) & 255) * sx + ((q >> 16) & 511) * sx * sy] = 1;
      const L = [];
      for (const q of m.vox) {
        const x = q & 255, y = (q >> 8) & 255, z = (q >> 16) & 511;
        if ((q >>> 25) < NBK) continue;                // bark — a leaf falls off a leaf
        if (x < EDGE || x >= sx - EDGE || y < EDGE || y >= sy - EDGE) continue;
        if (z < DROP + 1) continue;                    // nothing to fall through
        let clear = true;
        for (let d = 1; d <= DROP && clear; d++) if (occ[x + y * sx + (z - d) * sx * sy]) clear = false;
        if (clear) L.push(x | (y << 8) | (z << 16));
      }
      const ang = (u) => Math.atan2(((u >> 8) & 255) - sy * 0.5, (u & 255) - sx * 0.5);
      L.sort((u, v) => (ang(u) - ang(v)) || (u - v));
      const out = [];
      if (L.length <= LMAX) { for (const u of L) out.push(u); }
      else for (let i = 0; i < LMAX; i++) out.push(L[Math.floor(i * L.length / LMAX)]);   // EVEN sample of the angle order, so the cap costs coverage evenly instead of one arc
      OUT.push(out);
    }
    return OUT;
  };
  const birchBanch = (BV) => {
    const OUT = [];
    // -- WHERE A BEEHIVE HANGS IN A BIRCH (user 2026-08-24: "attach beehives to some of the birch trees") --
    // OAK_BANCH's rule, re-derived for these crowns: a BARK voxel with the hive's whole box of EXTERIOR air
    // below it. Bark and not leaf for the reason recorded at OAK_BANCH in assets/material-tabs.js - a hive is
    // solid, so it is STRUCTURE to the support resolver and the structure flood may not enter a drape cell;
    // hung off leaves it would have no path to the ground and be lifted the first time anything nearby moved.
    // Built HERE and not beside OAK_BANCH because BIRCHV is filled by this async load, long after
    // material-tabs.js has run - a table built there would capture an empty list and every birch would offer
    // no anchor at all. The leaf test is the index into BIRCHIDS rather than foliaTab for the same reason:
    // material-tabs.js has not run yet, so the tab is still empty here. Indices below BIRCHBARK.length are the
    // minted bark ids by construction (see the BIRCHIDS build above).
    // Angle-sorted and capped like the oak's, so the cap costs even coverage rather than one arc of the crown,
    // and so gen-pool.js can stringify it into every worker without shipping tens of thousands of entries.
      const BMAX = 24, EDGE = 2, NBK = BIRCHBARK.length;
      // HIVEV's OWN DIMENSIONS, WRITTEN OUT (2026-08-24) -- reading HIVEV here throws
      // "Cannot access 'HIVEV' before initialization": it is declared further down the load order, and this
      // block runs inside the async birch load, which resolves before that declaration is evaluated. The
      // whole birch load is inside one try/catch, so the throw did not crash anything visibly - it just left
      // BIRCHV empty and the entire forest gone, reported only by window.__birchErr. A typeof guard is no
      // help either: typeof on a let in its temporal dead zone throws exactly the same way.
      // The hive is 5x5x5; if that model is ever resized these three follow it.
      const hvx = 2, hvy = 2, hvz = 5;
      for (let k = 0; k < BV.length; k++) {
        const m = BV[k], sx = m.sx, sy = m.sy, sz = m.sz;
        // OCCUPANCY, NOT AN EXTERIOR-AIR FLOOD (2026-08-24) -- OAK_BANCH floods the box inward first because
        // those crowns are baked from a .glb as HOLLOW SHELLS: half the empty space inside an oak dome is a
        // sealed cavity, so "is empty" does not mean "is outside" and a hive hung there is simply buried.
        // A birch is not that model - it is scattered branches and leaf clusters with no enclosed volume - so
        // plain emptiness IS exteriority here, and asking the simpler question keeps this block free of
        // voxShellAir, which is declared further down and unreachable from inside this async load (the same
        // temporal dead zone that hid HIVEV, and it fails the same silent way: an empty BIRCHV and no forest).
        const occ = new Uint8Array(sx * sy * sz);
        for (const q of m.vox) occ[(q & 255) + ((q >> 8) & 255) * sx + ((q >> 16) & 511) * sx * sy] = 1;
        const clearBox = (x, y, z, r) => {
          for (let dx = -r; dx <= r; dx++) for (let dy = -r; dy <= r; dy++) {
            const xx = x + dx, yy = y + dy;
            if (xx < 0 || xx >= sx || yy < 0 || yy >= sy) return false;
            for (let d = 1; d <= hvz; d++) { const zz = z - d;
              if (zz < 0 || occ[xx + yy * sx + zz * sx * sy]) return false; }
          }
          return true;
        };
        // -- WIDEST CLEARANCE THAT ANY BRANCH ON THIS MODEL CAN OFFER -- OAK_BANCH's rule, and for its reason:
        // demanding the hive's whole 5x5 footprint is what stops it clipping through the crown, and on its own
        // it is too strict to be the only test. MEASURED here, requiring the full box left models 0, 2 and 4
        // with no anchor at all, so three of the five birches in play could never carry a hive and the rate
        // came out 4.4% against the 10% asked for. So the wide test SELECTS when the model can afford it and
        // the narrower ones stand behind it: a crown with no roomy branch still gets a hive, just a more
        // tucked-in one. Ordered widest-first and the first non-empty tier wins, per model.
        let B = [];
        for (const rad of [hvx, 1, 0]) {
          for (const q of m.vox) {
            const x = q & 255, y = (q >> 8) & 255, z = (q >> 16) & 511;
            if ((q >>> 25) >= NBK) continue;            // a LEAF - see the support argument above
            if (x < EDGE || x >= sx - EDGE || y < EDGE || y >= sy - EDGE) continue;
            if (z < (sz >> 2) || z > sz - 6) continue;  // low enough to be seen from the ground, clear of the very top
            if (clearBox(x, y, z, rad)) B.push(x | (y << 8) | (z << 16));
          }
          if (B.length) break;
        }
        const ang = (u) => Math.atan2(((u >> 8) & 255) - sy * 0.5, (u & 255) - sx * 0.5);
        // A TOTAL ORDER, NOT JUST AN ANGLE (2026-08-24) -- a worker rebuilds BIRCHV from the delta-varint, so its
        // voxel list can arrive in a different ORDER than the main thread's for the same model. Sorting on the
        // angle alone leaves ties to be broken by that order, the even sample below then picks different
        // anchors on each side, and the two disagree about where the hive hangs: gtest reported 25 voxels of
        // main-thread hive standing over worker air. Breaking ties on the packed coordinate makes the list a
        // pure function of the SET of voxels, which is the thing both sides really share.
        B.sort((u, v) => (ang(u) - ang(v)) || (u - v));
        const out = [];
        if (B.length <= BMAX) { for (const u of B) out.push(u); }
        else for (let i = 0; i < BMAX; i++) out.push(B[(((i + 0.5) / BMAX) * B.length) | 0]);
        OUT.push(out);
      }
    
    return OUT;
  };
  let OAKV = [], OAKBARK = [], OAKLEAF = [], OAKBLOSV = [], OAKWHITV = [], OAKLITER = [], OAKLITEV = [], BLOSRANK = new Int8Array(256).fill(-1);   // OAKLITER: the LIGHT green oak variety's 4-step ramp — the sorted leaf ids' top two, then the two OAKLITE mints. OAKLITEV: OAKV with every leaf run through it   // BLOSRANK: oak leaf id -> 0..3 by luminance, -1 for everything else. The RAMP is an argument, so one table serves both varieties   // BLOSMAP: oak leaf id -> its cherry-blossom twin; identity for everything else. The workers rebuild the pink crowns from it rather than being handed a second 218k-voxel model set
  try {
    if (location.search.includes('nooaks')) throw new Error('?nooaks');   // A/B switch: an empty OAKV disables the scatter, the material marking and the stamp in one go, the way ?nocacti does
    const oj = await (await fetch('assets/decoration/oak_trees.json')).json();
    // ── BARK COSTS NOTHING: IT IS THE PINE'S OWN ── measured, ?nooaks reports 250/256 with SIX ids free,
    // and the first cut of this asked for eight. The two it could not have were taken from the asset
    // editor's swatches in complete silence (edSubs 0 -> 2, with palAudit still reporting over=0/snaps=0),
    // which is the exact failure mode the ceiling notes in assets/palette.js warn about. So the three bark
    // shades are repointed onto woodIds instead of minted — the same move log.vox already makes a few lines
    // down in material-tabs.js, and for the same two reasons: the table has no room, and an oak trunk WANTS
    // to be the same material as every other trunk in the game (solid, woodTab, axe-only), which reusing the
    // ids grants for free rather than by remembering to mark it.
    // ── MAPPED BY RELATIVE LIGHTNESS, NOT BY NEAREST COLOUR ── the two ranges barely overlap: the oak bake
    // spans luma 74..107 and the pine's smoothed bark 89..104, so a nearest-colour match collapses all three
    // oak shades onto the pine's darkest and the trunk loses its shading. This is the same rank-map the
    // desert rocks use onto REDROCK, and for the identical reason — see ROCK26D above.
    const lum0 = (c) => c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114;
    const bark = woodIds.slice().sort((a, b) => lum0(palette[a]) - lum0(palette[b]));
    const obk = oj.pal.slice(0, oj.nbark).slice().sort((a, b) => lum0(a) - lum0(b));
    const bmap = new Map();                            // oak bark shade -> the pine bark id at the same rank
    obk.forEach((c, i) => bmap.set(oj.pal.indexOf(c), bark[Math.min(bark.length - 1,
      Math.floor(((i + 0.5) / obk.length) * bark.length))]));
    // …and only the LEAVES mint, because the leaf colour is the one thing about this biome that nothing else
    // in the table already carries. RESERVED in palOwn for the reason the pinecone's ids are: these ids are
    // about to be told they are canopy — walk-through, DRAPE support, snow-bearing — and a later tolerance
    // share would hand that to whatever model happened to ask for a nearby green. The palOwn add is guarded
    // on the mint really happening: on a full table addCol returns somebody ELSE's nearest id, and reserving
    // that would quietly steal it.
    const oids = oj.pal.map((c, i) => { if (i < oj.nbark) return bmap.get(i);
      const n0 = palette.length; const id = addCol(c[0], c[1], c[2]);
      if (palette.length > n0) palOwn.add(id); return id; });
    OAKBARK = [...new Set(oids.slice(0, oj.nbark))];                                         // pal[0:nbark] is bark, pal[nbark:] is leaf — see the voxelizer header
    OAKLEAF = oids.slice(oj.nbark);
    OAKV = oj.trees.map((t) => ({ sx: t.sx, sy: t.sy, sz: t.sz, vox: t.vox.map((p) => (p & 0xffffff) | (oids[p >>> 24] << 24)) }));
    // ── THE CHERRY FOREST'S PINK CROWN IS AN ID MAP, NOT A SECOND MODEL SET (user 2026-08-18) ── 256 numbers,
    // of which only the leaf entries are not the identity. The gen workers rebuild OAKBLOSV from OAKV through
    // it (see world/gen-pool.js), which is exactly the R26DMAP pattern a few hundred lines above and exists for
    // exactly the same measured reason: gen-pool stringifies every registered table into EACH worker's source,
    // OAKV is 218,367 voxels, and shipping a second copy is what stopped the boot completing when ROCK26D tried
    // it. A 256-number array costs nothing and the bark, which is 3 of the 7 ids, is shared rather than copied.
    // Paired BY RANK, dark→dark, not by nearest colour: the two ramps barely overlap (green luma 74..107,
    // blossom 128..205), so a nearest-colour match would collapse all four greens onto the darkest blossom and
    // the crown would lose its shading — the identical mistake the bark map documents avoiding just above.
    // FOUR GREENS ONTO THREE BLOSSOMS: the two middle greens share the mid pink. They are the pair a crown uses
    // interchangeably for the same interior shade (105,143,51 and 107,141,77 differ by 26 in one channel and
    // nothing in luma), so collapsing THEM is what a three-shade ramp should collapse.
    // ── ONE RANK TABLE, NOT ONE MAP PER VARIETY ── BLOSMAP/BLOSMAPW were id -> id, which caps the crown at as
    // many colours as the SOURCE has: the oak art carries exactly four leaf greens, so an eight-shade ramp
    // showed four shades and the other four were never written. What varies per variety is the target ramp, and
    // what the source actually supplies is a RANK — how dark this green is relative to the other three. So the
    // table is rank-only and the ramp is an argument (see blosRemap below).
    // -1 means "not a leaf": bark, and any id a crown does not carry, passes through untouched.
    BLOSRANK = new Int8Array(256).fill(-1);
    if (OAKLEAF.length) {
      const lo = OAKLEAF.slice().sort((a, b) => lum0(palette[a]) - lum0(palette[b]));   // dark -> light, the same order every blossom ramp is authored in
      lo.forEach((id, i) => { BLOSRANK[id] = i; });
      // ── AND THE SECOND GREEN VARIETY'S RAMP, ASSEMBLED HERE FOR THE SAME REASON THE RANK TABLE IS ── this is
      // the one place in the build that holds the leaf ids in luminance order, and OAKLITER is defined in terms
      // of that order rather than of particular ids: "the two lightest greens the art already has, then the two
      // lighter ones assets/palette.js minted above them". Re-authoring oak_trees.glb with different greens
      // therefore moves this ramp with it instead of silently pointing at the wrong shade.
      // TWO OF THE FOUR STEPS ARE THE DARK VARIETY'S OWN IDS, deliberately — see the OAKLITE note in
      // assets/palette.js. They are already palOwn, already foliaTab and already in foliageIds, so sharing them
      // between the two varieties is free in slots AND free in material: both varieties are oak canopy and every
      // question the game asks about a leaf has to get the same answer from either.
      OAKLITER = lo.slice(2).concat(OAKLITE);
    }
    console.log('[vb] oaks:', OAKV.length, 'trees,', OAKV.reduce((a, m) => a + m.vox.length, 0), 'voxels,',
      OAKBARK.length, 'bark ids (BORROWED from the pine) +', OAKLEAF.length, 'leaf ids (minted), widest', Math.max(...OAKV.map((m) => Math.max(m.sx, m.sy))), 'tallest', Math.max(...OAKV.map((m) => m.sz)));
  } catch (e) { console.warn('[vb] oak_trees.json missing — oaks skipped', e); OAKV = []; OAKBARK = []; OAKLEAF = []; OAKBLOSV = []; OAKWHITV = []; OAKLITER = []; OAKLITEV = []; }
  // ══ THE BIRCH FOREST'S TREES (user 2026-08-23) ══ 26 models, 12.1 m to 26.4 m, from
  // tools/voxelize_birch_forest.py. Three things about this asset are NOT like the oak's, and each one is
  // forced by the fact that a birch is four times the size of an oak.
  //
  // 1. THE BARK IS MINTED, WHERE THE OAK'S IS BORROWED. The oak note above repoints its three bark shades onto
  //    the pine's woodIds and calls it free — and it IS free, for a brown tree. A birch trunk is WHITE, and on
  //    the pine's browns it reads as a dead pine rather than a birch; the whiteness with the dark lash marks is
  //    the single strongest species cue there is. So these three mint. The table has four ids free (measured,
  //    __vb.palAudit) and the birch spends three of them, which is exactly why the LEAVES could not also mint —
  //    see 2. They are marked wood/solid/axe-only explicitly in assets/material-tabs.js, which is the property
  //    borrowing woodIds would have granted for nothing; minting means remembering to say it.
  // 2. THE LEAVES ARE THE OAK'S OWN IDS, not a second green ramp (user: "make the foilage in the birch forest a
  //    lighter green. matching the leaves of the lighter green oak tree"). The bake already repointed the
  //    sampled greens onto oak_trees.json's ramp, so pal[nbark:] IS OAKLEAF colour for colour; taking OAKLEAF's
  //    ids rather than minting twins costs zero slots and — more to the point — makes every question the game
  //    asks about a leaf (foliaTab, DRAPE support, snow catch, see-through primary ray) answer identically for
  //    the two forests, which is what "matching" has to mean for anything except the colour.
  // 3. THE MODELS ARE NOT SHIPPED TO THE WORKERS. BIRCHV is 1,194,089 voxels against OAKV's 218,367, and
  //    world/gen-pool.js JSON.stringifies every registered table into the pool's shared source — the note there
  //    records that a SECOND copy of the oaks alone stopped a boot completing. Stringified, this would be ~12 MB
  //    of JavaScript that up to 16 workers each parse. So what is registered is BIRCHENC, the base64
  //    delta-varint the .json already ships in (2.29 MB of string, cheap to parse), and each worker rebuilds
  //    BIRCHV from it through birchDec — the same "rebuilt here, not shipped" move the pink crowns make.
  try {
    // ── THE BIRCHES LOAD FROM THE .vox FOLDER, NOT FROM A BAKED .json (user 2026-08-23: "If I edit the assets
    // in the .vox folder, I want it to effect the assets in the game as well") ── which is a deliberate break
    // from how every other decoration in this game works. The rule elsewhere is that a .vox is an AUTHORING
    // original and the .json is what ships, so editing the .vox does nothing until a tool regenerates the
    // .json. Here the .vox files ARE the shipped asset: edit one in MagicaVoxel, reload, and the forest
    // changes. tools/voxelize_birch_forest.py writes them; nothing else stands between the folder and the game.
    //
    // WHICH FILES: whatever VOXDEX says is in the folder. VOXDEX is bundle.py's build-time walk of
    // game/assets (see assets/vox-index.js), so DROPPING A NEW TREE IN is enough - it needs no edit here and
    // no list to keep in step. It loads before this fragment, which is why this can read it.
    //
    // THE PALETTE COMES OUT OF THE FILES TOO, so recolouring in the editor carries as well. Every tree is
    // written with the same table — bark shades first, then the four leaf greens — and the FIRST FILE READ
    // DECIDES IT for the whole set, so every tree in the folder must share one palette. NB (how many of the
    // leading entries are bark) is derived from that table below rather than typed, because the count has
    // already changed once: the bake writes 3 and tools/birch_bark_white.py rewrites it to 4.
    // ?nobirch — A/B switch, exactly as ?nooaks is. (The band was emptied outright for a while on 2026-08-23
    // and put back the same day: "spread the birch trees around the birch forest again". Nothing had to be
    // rebuilt to restore it, because turning the wood off was only ever a refusal to LOAD — birchAt opens with
    // `if (!BIRCHV.length) return null`, which switches placement off on the main thread and in the gen workers
    // together, since they are handed this same array rather than deciding for themselves.)
    // WHOLE-FLAG MATCH, not includes(): '?cdp&nobirchbirds' CONTAINS 'nobirch', so an includes() test here
    // silently killed the whole wood whenever the bird-only A/B flag was used — and an A/B that removes the
    // thing you are trying to hold constant reports a beautiful, meaningless result. It cost one wrong
    // conclusion (the trees' cost attributed to the birds). Same trap the ?igpu note in core/gpu.js records.
    if (/[?&]nobirch/.test(location.search)) throw new Error('?nobirch');
    if (!OAKLEAF.length) throw new Error('no OAKLEAF to share — oaks must load first');   // the leaves borrow the oak's ramp; with no oaks there is nothing to borrow and minting four greens would not fit
    // VOXDEX is a STRING at runtime, not the array the source literal looks like: bundle.py joins the folder
    // entries with ';'. ui/console.js reads it the same way — VOXDEX.split(';') — and that is the contract.
    const bdir = VOXDEX.split(';').find((e) => e.startsWith('foilage/birch_trees:'));
    const bnames = bdir ? bdir.slice(bdir.indexOf(':') + 1).split(',').filter(Boolean) : [];
    if (!bnames.length) throw new Error('no .vox in game/assets/foilage/birch_trees');
    // A MagicaVoxel 150 reader, small enough to keep here: SIZE/XYZI/RGBA out of the chunk tree. Packs into
    // the same x | z<<8 | y<<16 | index<<25 the stamper reads, with the colour left as a 0..6 INDEX because
    // seven bits cannot hold a palette id (see stampBirch).
    // A MagicaVoxel 150 reader. It has to handle a MULTI-PART file, because a .vox coordinate is one byte and
    // the birches are now scaled past 256: tools/voxelize_birch_forest.py writes a tall tree as a STACK of
    // parts plus an nTRN/nGRP/nSHP scene graph. The z offset of each part is read out of its nTRN translation
    // rather than assumed from the order, so moving a part in MagicaVoxel moves it in the world too.
    const readVox = (buf) => {
      const d = new DataView(buf), u8 = new Uint8Array(buf);
      let i = 8, rgba = null;                          // skip 'VOX ' + version
      const sizes = [], models = [], shp = new Map(), trn = [];
      const rdStr = (o) => { const n = d.getUint32(o, true); return [String.fromCharCode.apply(null, u8.subarray(o + 4, o + 4 + n)), o + 4 + n]; };
      const rdDict = (o) => { const n = d.getUint32(o, true); o += 4; const m = {};
        for (let k = 0; k < n; k++) { let a, b; [a, o] = rdStr(o); [b, o] = rdStr(o); m[a] = b; } return [m, o]; };
      while (i < buf.byteLength - 12) {
        const id = String.fromCharCode(u8[i], u8[i + 1], u8[i + 2], u8[i + 3]);
        const cs = d.getUint32(i + 4, true); i += 12;
        if (id === 'MAIN') continue;                   // its content is empty; its CHILDREN follow inline
        if (id === 'SIZE') sizes.push([d.getUint32(i, true), d.getUint32(i + 4, true), d.getUint32(i + 8, true)]);
        else if (id === 'XYZI') { const n = d.getUint32(i, true), a = new Int32Array(n);
          for (let k = 0; k < n; k++) { const o = i + 4 + k * 4;
            a[k] = u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | ((u8[o + 3] - 1) << 25); }
          models.push(a); }
        else if (id === 'RGBA') rgba = u8.subarray(i, i + 1024);
        else if (id === 'nSHP') { let o = i; const nid = d.getInt32(o, true); o += 4;
          let dict; [dict, o] = rdDict(o); const nm = d.getUint32(o, true); o += 4;
          shp.set(nid, d.getInt32(o, true)); }        // first model id is enough: the writer emits one per shape
        else if (id === 'nTRN') { let o = i; const nid = d.getInt32(o, true); o += 4;
          let dict; [dict, o] = rdDict(o); const child = d.getInt32(o, true); o += 16;   // child, reserved, layer, nframes
          let fr; [fr, o] = rdDict(o);
          const tz = fr._t ? (parseInt(fr._t.split(' ')[2], 10) || 0) : 0;
          trn.push([child, tz]); }
        i += cs;
      }
      if (!models.length) return null;
      if (models.length === 1) { const v = models[0]; v.sort();
        return { sx: sizes[0][0], sy: sizes[0][1], sz: sizes[0][2], vox: v, rgba }; }
      // STACK THE PARTS. Each nTRN names a child nSHP, which names a model; its _t z is that part's CENTRE in
      // tree-local space, so the part's base is centre - size/2. Rebased to zero so the tree starts at z 0.
      const base = new Array(models.length).fill(null);
      for (const [child, tz] of trn) { const mi = shp.get(child);
        if (mi !== undefined && mi < models.length) base[mi] = tz - (sizes[mi][2] >> 1); }
      for (let k = 0; k < models.length; k++) if (base[k] === null) base[k] = 0;   // no scene graph: fall back to sequential stacking
      let lo = Math.min.apply(null, base);
      const out = [];
      let sx = 0, sy = 0, top = 0;
      for (let k = 0; k < models.length; k++) {
        const off = base[k] - lo;
        sx = Math.max(sx, sizes[k][0]); sy = Math.max(sy, sizes[k][1]);
        top = Math.max(top, off + sizes[k][2]);
        const a = models[k];
        for (let j = 0; j < a.length; j++) { const q = a[j];
          out.push((q & 0x1ffff) | ((((q >> 16) & 511) + off) << 16) | ((q >>> 25) << 25)); }
      }
      const v = Int32Array.from(out); v.sort();
      return { sx, sy, sz: top, vox: v, rgba };
    };
    const loaded = [];
    for (const nm of bnames) {
      try {
        const ab = await (await fetch('assets/foilage/birch_trees/' + nm + '.vox')).arrayBuffer();
        const m = readVox(ab); if (m) { m.name = nm; loaded.push(m); }
      } catch (e2) { console.warn('[vb] birch: could not read', nm, e2); }
    }
    if (!loaded.length) throw new Error('no birch .vox parsed');
    loaded.sort((a2, b2) => a2.sz - b2.sz);            // SHORT TO TALL: birchAt's height guard walks a PREFIX of this
    const pal0 = loaded.find((m) => m.rgba);
    if (!pal0) throw new Error('no RGBA chunk in any birch .vox');
    // NB 3 -> 4 (user 2026-08-23: "make it multiple shades of white and multiple shades of dark grey"):
    // pal[0:4] bark, pal[4:8] leaf. tools/birch_bark_white.py writes that table and shifts the four leaves up
    // by one to make room, so this and the tool must move together — read the wrong NB and the leaves come
    // back off by one and a crown mints a bark id. FOUR IS A HARD CEILING, not a preference: each bark shade
    // costs one palette id and they cannot be shared (palOwn below reserves them because they are about to be
    // called wood), and with the birches unloaded the palette stands at 252/256.
    // …and NB is DERIVED FROM THE FILE, not typed here. It was a literal 3, then a literal 4, and a literal is
    // a standing trap: the baker writes the bark ramp and tools/birch_bark_white.py rewrites it to a different
    // LENGTH, so a bake that is not followed by the tool (or a tool run that is not followed by a re-read)
    // leaves this number pointing one entry off — and being off by one here does not throw, it silently reads
    // the first leaf as bark, mints a green id as WOOD, and hands the crown a bark shade. The bark is the run
    // of leading entries that are not green, using the same green test the tools use, so 3 shades and 4 both
    // load correctly and the file is the single source of truth.
    const isGreen = (c) => c[1] > c[0] + 12 && c[1] > c[2] + 12;
    const bpal0 = [];
    for (let i = 0; i < 12; i++) bpal0.push([pal0.rgba[i * 4], pal0.rgba[i * 4 + 1], pal0.rgba[i * 4 + 2]]);
    let NB = 0;
    while (NB < bpal0.length && !isGreen(bpal0[NB]) && (bpal0[NB][0] || bpal0[NB][1] || bpal0[NB][2])) NB++;
    const bpal = bpal0.slice(0, NB + 4);
    if (NB < 1 || NB > 6) throw new Error('birch palette: ' + NB + ' bark shades, expected 1-6');
    BIRCHNB = NB;                                      // BEFORE birchDec/birchTrunkC/birchPick run below — they all read it
    // Bark MINTS (a white trunk cannot borrow the pine's browns); leaves take OAKLEAF's ids, matched BY RANK so
    // the darkest bake green lands on the darkest oak id however the two arrays happen to be ordered.
    const lum1 = (c) => c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114;
    const leafByLum = OAKLEAF.slice().sort((a2, b2) => lum1(palette[a2]) - lum1(palette[b2]));
    const bleaf = bpal.slice(NB).map((c, i) => [lum1(c), i]).sort((a2, b2) => a2[0] - b2[0]);
    const leafId = new Array(bpal.length - NB);
    bleaf.forEach(([, i], r) => { leafId[i] = leafByLum[Math.min(leafByLum.length - 1, r)]; });
    BIRCHIDS = bpal.map((c, i) => {
      if (i >= NB) return leafId[i - NB];
      const n0 = palette.length, id = addCol(c[0], c[1], c[2]);
      if (palette.length > n0) palOwn.add(id);         // RESERVED for the reason the oak leaves are: these are about to be told they are wood, and a tolerance share would hand that to somebody else's model
      return id;
    });
    BIRCHBARK = [...new Set(BIRCHIDS.slice(0, NB))];
    // ── THE TWIGS ON THE GROUND ARE BIRCH TWIGS TOO (user: "the twigs on the ground, make them birch twigs.
    // so they should be white with some dark grey in it + light green leaves") ── a RECOLOUR of stick_1/stick_2
    // rather than new art, which is what the cherry forest does the other way round: it has AUTHORED pink twigs
    // (STICKB) because a hue rotation could not make a convincing blossom. Here the target ramps already exist
    // and are exactly right - the birch's own minted bark, and the oak leaf greens the birch canopy wears - so
    // remapping beats drawing. Identical geometry, so the seating, the pickup flood and the float table all
    // behave as they do for the brown twig.
    // BY RANK, not by index: each ramp is sorted dark-to-light and the model's own ids are too, so the twig's
    // darkest wood lands on the darkest bark and its lightest on the lightest however either table is ordered.
    // Wood and leaf are told apart by foliaTab, which is the same question the renderer asks.
    if (STICKV.length && BIRCHBARK.length && OAKLEAF.length) {
      const lum9 = (i) => { const c = palette[i]; return c ? c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114 : 0; };
      const bark9 = BIRCHBARK.slice().sort((a, b) => lum9(a) - lum9(b));
      const leaf9 = OAKLEAF.slice().sort((a, b) => lum9(a) - lum9(b));
      const pick9 = (ramp, k, n9) => ramp[n9 < 2 ? ramp.length - 1 : Math.round(k * (ramp.length - 1) / (n9 - 1))];
      STICKBIRCH = STICKV.map((m) => {
        const ids9 = [...new Set(m.vox.map((q) => q >>> 24))];
        // GREENNESS, not foliaTab: assets/material-tabs.js runs AFTER this fragment (src/manifest.txt), so the
        // tab is still empty here and every id would classify as wood — which is exactly what happened, and it
        // turned the twig's leaves into bark. The colour itself cannot be out of order.
        const green9 = (i) => { const c = palette[i]; return !!c && c[1] > c[0] + 12 && c[1] > c[2] + 12; };
        const wood9 = ids9.filter((i) => !green9(i)).sort((a, b) => lum9(a) - lum9(b));
        const lv9 = ids9.filter((i) => green9(i)).sort((a, b) => lum9(a) - lum9(b));
        const map9 = new Map();
        // ── 80% WHITE, 20% DARK GREY (user) ── and the split is by VOXEL COUNT, not by id count. Spreading the
        // model's wood ids evenly across the four bark shades sounds equivalent and is not: the twig's ids do
        // not carry equal numbers of voxels, so an even spread over ids put roughly half the WOOD on the two
        // greys. Ranked dark-to-light and split at the 20th percentile of voxels, the darkest fifth of the
        // twig takes the greys and the rest takes the whites, which is what the eye actually counts.
        const cnt9 = new Map();
        for (const q of m.vox) cnt9.set(q >>> 24, (cnt9.get(q >>> 24) || 0) + 1);
        const woodN = wood9.reduce((a, id) => a + (cnt9.get(id) || 0), 0);
        const greys9 = bark9.slice(0, Math.max(1, bark9.length >> 1));       // the dark half of the birch ramp
        const whites9 = bark9.slice(Math.max(1, bark9.length >> 1));         // …and the light half
        // The boundary is the id whose CUMULATIVE share lands closest to 20%, not the first id to cross it. An
        // id is atomic - every voxel wearing it takes one colour - so "first past the post" overshoots by that
        // id's whole weight: measured, it gave 30% grey against the 20% asked for. Choosing the nearest
        // boundary instead splits the error rather than always rounding it up.
        let cum9 = 0; const acc9 = wood9.map((id) => { cum9 += cnt9.get(id) || 0; return cum9 / Math.max(1, woodN); });
        let cut9 = 0, bestD = 1e9;
        for (let i = 0; i < acc9.length; i++) { const d = Math.abs(acc9[i] - 0.20); if (d < bestD) { bestD = d; cut9 = i + 1; } }
        wood9.forEach((id, i) => {                                          // wood9 is already sorted DARK -> light
          map9.set(id, i < cut9 ? greys9[Math.min(greys9.length - 1, Math.round(i / Math.max(1, cut9 - 1) * (greys9.length - 1)))]
                                : whites9[Math.min(whites9.length - 1, Math.round((i - cut9) / Math.max(1, wood9.length - cut9 - 1) * (whites9.length - 1)))]);
        });
        lv9.forEach((id, k) => map9.set(id, pick9(leaf9, k, lv9.length)));   // the leaves were already right - light green, on the oak ramp the canopy wears
        return { sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox.map((q) => (q & 0xffffff) | ((map9.get(q >>> 24) || (q >>> 24)) << 24)) };
      });
      console.log('[vb] birch twigs:', STICKBIRCH.length, 'models recoloured onto', bark9.length, 'bark +', leaf9.length, 'leaf ids');
    }
    BIRCHV = loaded.map((m) => Object.assign({ sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox }, birchTrunkC(m.vox, m.sx, m.sy, m.sz)));
    // …and the gen workers still cannot be handed 1.2M voxels of source, so they get the same delta-varint the
    // .json used to ship and rebuild from it. Encoding here costs one pass at boot; see world/gen-pool.js.
    BIRCHENC = BIRCHV.map((m) => ({ sx: m.sx, sy: m.sy, sz: m.sz, n: m.vox.length, vox: birchEnc(m.vox) }));
    BIRCHPICK = birchPick(BIRCHV);                     // the density-weighted draw — see birchPick
    BIRCH_BANCH = birchBanch(BIRCHV);
    BIRCH_ANCH = birchLanch(BIRCHV);
    console.log('[vb] birch leaf anchors:', BIRCH_ANCH.map((a) => a.length).join('/'));
    console.log('[vb] birch hive anchors:', BIRCH_BANCH.map((a) => a.length).join('/'));
    console.log('[vb] birches:', BIRCHV.length, 'trees from .vox,', BIRCHV.reduce((a2, m) => a2 + m.vox.length, 0),
      'voxels,', BIRCHBARK.length, 'bark ids (MINTED) +', BIRCHIDS.length - NB, 'leaf ids (shared with the oak), tallest',
      Math.max(...BIRCHV.map((m) => m.sz)));
  } catch (e) { window.__birchErr = String(e && e.stack || e); console.warn('[vb] birch .vox folder unreadable — birches skipped', e); BIRCHV = []; BIRCHENC = []; BIRCHBARK = []; BIRCHIDS = []; BIRCHPICK = []; BIRCH_BANCH = []; }
  // ── ONE DENSE OCCUPANCY + EXTERIOR-AIR FLOOD FOR A SPARSE MODEL ── the oak crowns baked out of a .glb are
  // HOLLOW SHELLS: measured over all seven, every single leaf voxel has an empty 6-neighbour (2468 of 2468 on
  // the bush, 77513 of 77513 on the giant), so "has air beside it" does not mean "is on the OUTSIDE" — half of
  // that air is the sealed cavity under the dome. A berry hung on an inside face, or an apple dropped into the
  // hollow, is simply invisible. So the air is flooded from the bounding box inward first, and everything below
  // asks about EXTERIOR air specifically. Both consumers (the berry scatter here and OAK_ANCH / OAK_BANCH in
  // material-tabs.js) need exactly this, which is why it is one helper and not two loops.
  // Transient: the caller drops both arrays. The giant is 114x112x114 = 1.46 MB of Uint8, and the whole set of
  // nine models is ~3.5 M cells walked ONCE at load.
  const voxShellAir = (m) => {
    const sx = m.sx, sy = m.sy, sz = m.sz, n = sx * sy * sz;
    const occ = new Uint8Array(n), ext = new Uint8Array(n), st = new Int32Array(n);
    for (const p of m.vox) occ[(p & 255) + ((p >> 8) & 255) * sx + ((p >> 16) & 255) * sx * sy] = 1;
    let sp = 0;
    for (let z = 0; z < sz; z++) for (let y = 0; y < sy; y++) for (let x = 0; x < sx; x++) {
      if (x && x < sx - 1 && y && y < sy - 1 && z && z < sz - 1) continue;   // seed from the box FACES only
      const i = x + y * sx + z * sx * sy; if (occ[i] || ext[i]) continue;
      ext[i] = 1; st[sp++] = i;
    }
    while (sp > 0) { const i = st[--sp];
      const x = i % sx, y = ((i / sx) | 0) % sy, z = (i / (sx * sy)) | 0;
      for (let d = 0; d < 6; d++) {
        const nx = x + (d === 0 ? 1 : d === 1 ? -1 : 0), ny = y + (d === 2 ? 1 : d === 3 ? -1 : 0), nz = z + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || ny < 0 || ny >= sy || nz < 0 || nz >= sz) continue;
        const j = nx + ny * sx + nz * sx * sy; if (occ[j] || ext[j]) continue;
        ext[j] = 1; st[sp++] = j;
      } }
    return { occ, ext };
  };
  // ── THE BUSH TIER IS A BERRY BUSH NOW, AND THE PLAIN ONE STOPS EXISTING (user 2026-08-17: "in oak_1 … one has
  // single red voxels scattered around it, and the other one has single blue voxels … so there shouldn't be one
  // without any berries") ── OAKV[0] is the 2.4 m underbrush, 22% of every oak in the world. It becomes TWO
  // models, a cherry bush and a blueberry bush, and world/terrain.js splits that tier between them 50/50, so the
  // berryless bush is not rarer — it is gone.
  //
  // WHY THIS IS A LOAD-TIME DERIVATION AND NOT A RE-BAKE. Every other decoration in this game is baked by a tool
  // and the .json IS the asset, and that is still true of the TREE: nothing here re-shapes oak_1, it re-COLOURS
  // 48 of its 2468 leaf voxels. Baking that would mean shipping two more copies of a 3427-voxel model to express
  // a recolour that is a pure function of the model already on disk, and it would put the berry count and the
  // scatter behind a re-run of tools/voxelize_oaks.py (which needs source/glb/oak_trees.glb) instead of in front
  // of the reader. game/assets/decoration/oak_trees.json is untouched by this whole feature.
  //
  // A BERRY REPLACES A LEAF, IT DOES NOT ADD A VOXEL, and that is what makes it free of every support question in
  // the game: the cell was already the crown's, so the berry is 26-connected to the crown by construction, it is
  // inside the model that sim/physics.js oakShape() builds, and it therefore falls with the bush when the bush is
  // felled. The voxel COUNT is identical, so nothing downstream that sizes off the bake shifts.
  // Candidates are OUTER leaves only (see voxShellAir), angle-sorted about the trunk and picked one per angular
  // sector — the same even-ring trick PINE_ANCH uses for cones, so berries wrap the bush instead of clumping on
  // one face. The two variants take different phases off the same list, so a cherry bush and a blueberry bush
  // standing side by side are not the same bush twice.
  const OKBERRY = 48;                                  // berries per bush: 1 in every ~51 leaves, ~15 of them facing you at once on a 3.4 m bush. The one number to move if the bushes read bare or gaudy.
  if (OAKV.length && OAKLEAF.length) {
    const m = OAKV[0], sx = m.sx, sy = m.sy;
    const fol = new Uint8Array(256); for (const i of OAKLEAF) fol[i] = 1;
    const ext = voxShellAir(m).ext;
    const cand = [];                                   // outer-surface leaf voxels, as INDICES into m.vox
    for (let i = 0; i < m.vox.length; i++) { const p = m.vox[i];
      if (!fol[p >>> 24]) continue;
      const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
      let out = false;
      for (let d = 0; d < 6 && !out; d++) {
        const nx = x + (d === 0 ? 1 : d === 1 ? -1 : 0), ny = y + (d === 2 ? 1 : d === 3 ? -1 : 0), nz = z + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || ny < 0 || ny >= sy || nz < 0 || nz >= m.sz) continue;
        if (ext[nx + ny * sx + nz * sx * sy]) out = true;
      }
      if (out) cand.push(i);
    }
    cand.sort((a, b) => Math.atan2(((m.vox[a] >> 8) & 255) - sy * 0.5, (m.vox[a] & 255) - sx * 0.5)
                      - Math.atan2(((m.vox[b] >> 8) & 255) - sy * 0.5, (m.vox[b] & 255) - sx * 0.5));
    const berryBush = (col, salt) => {                  // one recoloured copy of the bush
      const vox = m.vox.slice(), n = Math.min(OKBERRY, cand.length);
      for (let k = 0; k < n; k++) {
        const j = cand[(((k + 0.15 + ihash(k * 29 + salt, k * 31 + salt) * 0.7) / n) * cand.length) | 0];
        vox[j] = (vox[j] & 0xffffff) | (col << 24);
      }
      return { sx: m.sx, sy: m.sy, sz: m.sz, vox };
    };
    OAKV.splice(0, 1, berryBush(FRUITC[0], 11), berryBush(FRUITC[1], 97));   // [0] cherry, [1] blueberry — every index above the bush tier shifts up one, and world/terrain.js's size ladder is written against that
    // ── AND THE PINK SET IS BUILT HERE, AFTER THE SPLICE, BECAUSE THE SPLICE CHANGES OAKV'S LENGTH ──
    // it replaces one model with two, so every index above the bush tier shifts up by one. Built before this
    // line, OAKBLOSV had 7 entries against OAKV's 8 and `OAKBLOSV[t.k]` was a DIFFERENT TREE from `OAKV[t.k]`.
    // The gen workers never had the bug — they derive their copy from the OAKV that gen-pool registers, which
    // is this final one — so the two paths stamped different geometry and __vb.gtest reported 21,640 voxel
    // diffs: the pool's pink crown against empty air where the main thread had put a smaller model.
    // If a later splice ever touches OAKV again, this line has to stay below it.
    OAKBLOSV = OAKV.map((m) => blosRemap(m, BLOSLEAF, BLOSRANK, BLOSCHERRY));
    OAKWHITV = OAKV.map((m) => blosRemap(m, BLOSWHITE, BLOSRANK, BLOSCHERRY));   // the white variety fruits too — it is the same species, only the blossom differs   // built HERE, after berryBush has spliced OAKV — building it earlier is what once made OAKBLOSV[t.k] a different tree from OAKV[t.k] (gtest 21640)
    // ── AND THE LIGHT GREEN OAK, WHICH IS THE SAME MOVE IN A THIRD COLOUR (user 2026-08-19) ── same models,
    // same rank table, a different ramp. It MUST be built here with the other two and not a line earlier: the
    // splice above turned one bush into two, so an OAKLITEV assembled before it would be 7 entries against
    // OAKV's 8 and OAKLITEV[t.k] would be a DIFFERENT TREE from OAKV[t.k] — the gtest 21640 failure the pink
    // set already documents, which the gen workers never reproduce because they derive from the final OAKV.
    // NO FRUIT ARGUMENT (0 rather than BLOSCHERRY): the cherries are a blossom-only scatter, and a green oak
    // that carries fruit carries the apple and orange MODELS that stampOak hangs off OAK_ANCH. Those are
    // untouched here and stay correct for both varieties — the fruit's leaf blade wears the lightest oak leaf
    // id, which is a member of BOTH ramps, so it reads as part of whichever crown it is hanging in.
    // The BERRIES on the two bush tiers pass through for the same reason the bark does: BLOSRANK is -1 for
    // every id that is not a leaf, so a cherry or blueberry bush in the light variety keeps its berries.
    OAKLITEV = OAKLITER.length ? OAKV.map((m) => blosRemap(m, OAKLITER, BLOSRANK, 0)) : [];
    console.log('[vb] oak bushes: cherry + blueberry variants,', Math.min(OKBERRY, cand.length), 'berries each from',
      cand.length, 'outer leaves — OAKV is', OAKV.length, 'models now');
  }
  // ── APPLES AND ORANGES (user 2026-08-17: "pick trees at random and place apples and oranges in the trees") ──
  // tools/voxelize_fruit.py bakes both out of the art (apple/00.vox, and the ORANGE node inside the 117-model
  // culinary pack that is orange.vox) and emits SLOTS rather than colours: 0 = flesh, 1 = stem and leaf. The ids
  // are decided here, and neither of them is a mint this file makes:
  //   * the flesh takes its own FRUITC entry, matched by colour to what the tool baked — so if the art is
  //     re-authored and the tool's mean moves, this picks the nearer of the two rather than silently painting
  //     an apple orange, and a mismatch is visible in the console line below;
  //   * the LEAF WEARS AN OAK LEAF ID, the lightest of the four. That is not thrift, it is the correct
  //     answer: the voxels are a leaf, and they are hanging in a crown made of exactly that leaf, so they get
  //     foliaTab, DRAPE support and canopy see-through for nothing and cannot be told apart from the tree.
  //   * the STEM IS WOODY AND WANTS A BROWN (user 2026-08-17: "the apple doesnt seem to have a brown stem …
  //     the stem is currently green like the leaf on it"). The user is right, and the ART AGREES WITH THEM:
  //     apple/00.vox paints the stalk at (0,1,4) and (1,1,3) in (143,95,74), a real brown, and the leaf blade
  //     at (2,1,4)/(2,1,3)/(3,1,3) in three greens. What lost it is the BAKE, not this loader — fruit.json
  //     carries a BOOLEAN high byte (0 = flesh, 1 = everything else) and one pooled `pal[nbody]` colour, whose
  //     voxel-weighted mean is (171,178,100): three greens outvoting two browns, i.e. an olive.
  //     Once collapsed the split cannot be recovered from fruit.json by GEOMETRY — measured, the five cells are
  //     one 26-connected blob and the brown (1,1,3) is FACE-adjacent to the green (2,1,3), so no connectivity
  //     or adjacency rule separates them. It CAN be recovered from the art, which the game now ships and loads
  //     anyway for the eating animation, so that is what happens below: every non-flesh cell is looked up in
  //     apple/00.vox and classified by its authored hue (r > g = woody stalk, otherwise leaf blade).
  //     The ORANGE is read off orange.vox by the same rule (user 2026-08-17: "do not use the apple model at
  //     all for the orange model") — it happens to answer identically today, because its three non-flesh
  //     cells are three greens and it has no stalk at all, but it is now ITS OWN art saying so. A cell the
  //     art does not know falls back to LEAF, so a re-authored fruit cannot come out wrong, only unimproved.
  //   * …AND THE BROWN ITSELF IS NOT MINTED HERE. FRUIT_STEM_ID is 0, so the stalk falls back to the leaf id:
  //     there is no id in the table that is both BROWN and CANOPY, and the palette is at 255/256 with one slot
  //     left, which is not this file's to spend. Every reuse was checked and every one leaks. STICK_S is in
  //     PICK_STICK, so a stalk would right-click up as a TWIG — and it would do it through the very flood the
  //     fruit pickup in sim/projectiles.js walks. The oaks' own OAKBARK is solid + woodTab: that puts a hitbox
  //     in a walk-through crown, lets the chop ray read a 2-voxel stalk as "the trunk behind the needles" and
  //     hand the swing to the tree-felling path, and makes the stem STRUCTURE hanging off a DRAPE anchor — the
  //     component-with-no-path-to-the-ground that the beehive comment in material-tabs.js documents, which the
  //     resolver lifts on the first disturbance. STICK_M is the one already-minted brown that nothing wears,
  //     but it is not in palOwn and it sits in the middle of a 29-colour pine-brown cluster (both sticks, the
  //     log, both pines and every stone-tool haft are within PAL_TOL 8 of it), so it is very likely already
  //     borrowed by palShare: marking it foliaTab could hand canopy identity to a twig, and reserving it would
  //     displace its borrower onto the last slot or a snap. Set FRUIT_STEM_ID to a minted brown and the split
  //     below goes live — material-tabs.js already marks it canopy whenever it is non-zero.
  //     The HELD apple and its eating animation are raw-RGB art and are unaffected either way: their stalk is
  //     the artist's own brown TODAY, at no palette cost, which is where an apple is actually big on screen.
  const FRUIT_STEM_ID = FRUIT_STEM;                    // the brown minted in assets/palette.js — see the note there for why nothing existing could be reused
  let FRUITV = [];
  let FRUIT_LEAF_ID = 0;    // …and the CANOPY LEAF id the fruit's own blade ended up wearing, hoisted to module scope so the PICKUP can read it (sim/projectiles.js). It is chosen inside the try below by nearest colour, and a const in there is block-scoped — which is exactly why the pickup could not see it and left the leaf hanging in the tree.
  try {
    const fj = await (await fetch('assets/decoration/fruit.json')).json();
    const near = (c, ids) => { let bd = 1e9, best = ids[0];
      for (const i of ids) { const q = palette[i]; if (!q) continue;
        const d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
        if (d < bd) { bd = d; best = i; } }
      return best; };
    const leafId = near(fj.pal[fj.nbody], OAKLEAF.length ? OAKLEAF : [FRUITC[0]]);
    FRUIT_LEAF_ID = leafId;
    const stemId = FRUIT_STEM_ID || leafId;
    // ── EACH FRUIT IS READ OFF ITS OWN ART (user 2026-08-17: "do not use the apple model at all for the
    // orange model") ── this looked the non-flesh cells of BOTH fruit up in apple/00.vox, on the argument
    // that the two are baked on one grid and the orange's three crown cells ARE the apple's three leaf
    // cells. That is true of the art as it stands and wrong as a rule: it is the apple deciding what an
    // orange's stem is, so an orange re-authored with a brown stalk would quietly wear the apple's answer
    // instead of its own. One map per fruit, keyed by the name tools/voxelize_fruit.py wrote, and the
    // fallback is unchanged — a cell the art does not know is a leaf, which is what shipped before.
    const ART = { apple: voxCellCols(await fetchBytes('assets/food/apple/00.vox')),
                  orange: voxCellCols(await fetchBytes('assets/food/orange.vox')) };   // cell -> the colour the ARTIST painted: the only place the stem/leaf split still exists once the bake has pooled it
    const woody = (art, p) => { const c = art && art.get(p & 0xffffff); return !!c && c[0] > c[1]; };   // brown stalk vs green blade, read off the authored colour. No art (or no file) = leaf
    let nStem = 0;
    FRUITV = fj.fruit.map((f, i) => { const body = near(fj.pal[i], [FRUITC[0], FRUITC[2]]), art = ART[f.name];
      return { name: f.name, sx: f.sx, sy: f.sy, sz: f.sz,
               vox: f.vox.map((p) => { const id = (p >>> 24) ? (woody(art, p) ? (nStem++, stemId) : leafId) : body;
                 return (p & 0xffffff) | (id << 24); }) }; });
    console.log('[vb] fruit:', FRUITV.map((f, i) => f.name + ' ' + f.sx + 'x' + f.sy + 'x' + f.sz + ' ' + f.vox.length + 'vox flesh id ' +
      near(fj.pal[i], [FRUITC[0], FRUITC[2]])).join(', '), '— leaf on oak leaf id', leafId, '| stalk', nStem, 'vox on id', stemId,
      stemId === leafId ? '(NO BROWN MINTED — the stalk still reads as leaf; see FRUIT_STEM_ID)' : '(brown)');
  } catch (e) { console.warn('[vb] fruit.json missing — no fruit in the oaks', e); FRUITV = []; }
  // ── AND THE BEEHIVE (user 2026-08-17: "implement the beehive.vox on some of the oak trees as well") ── one
  // 5x5x5 model, 54 voxels, and NO new palette entry: every shade it is authored with is resolved onto the two
  // HIVEC ids by nearest colour before the parse, and handing parseVoxModel a complete colMap is what stops it
  // reaching addCol at all. That matters more than it sounds on a table this full — the own-ids path would have
  // minted eight, and on a full table addCol SUBSTITUTES rather than fails. Reading the colours first with
  // voxColsUsed (which mints nothing, by design) is the only way to decide a mapping before the parser does.
  let HIVEV = null;
  { const hb = await fetchBytes('assets/decoration/beehive.vox');
    if (hb) try {
      const cmap = new Map();
      for (const c of voxColsUsed(hb)) { let bd = 1e9, id = HIVEC[0];
        for (const h of HIVEC) { const q = palette[h];
          const d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
          if (d < bd) { bd = d; id = h; } }
        cmap.set((c[0] << 16) | (c[1] << 8) | c[2], id); }
      HIVEV = parseVoxModel(hb, false, false, cmap);
      console.log('[vb] beehive:', HIVEV.sx + 'x' + HIVEV.sy + 'x' + HIVEV.sz, HIVEV.vox.length, 'vox,', cmap.size, 'authored shades ->', HIVEC.length, 'ids');
    } catch (e) { console.warn('[vb] beehive.vox is unreadable — no hives', e); HIVEV = null; }
  }
  let FERN2V = [];                                                                        // the big fern plant from fern.glb (see voxelize_fern2.py), walk-through decor — the ferns_grass clumps were REMOVED 2026-07-16
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
  // The fetch itself is behind LGIG_ON (assets/palette.js), not just the stamp: the 14 ids this model mints are
  // the point of the switch, and they are minted by the loader below whether or not anything ever draws it.
  try {
    if (!LGIG_ON) throw new Error('gigantic lilypads are off (LGIG_ON) — their 14 palette ids are reclaimed');
    const gj = await (await fetch('assets/decoration/lillypad_gigantic.json')).json();
    const lilyCols = [...new Set(LILYV.flatMap((m) => m.vox.map((p) => p >>> 24)))].map((id) => palette[id]);   // the REGULAR lilypads' mint-green swatches (user: 'same shade as the other lillypads')
    const gcache = new Map();                                                                // snap each giant-pad shade to the nearest regular-lily colour, addCol'd to its OWN id so markSolid never makes the small pads solid too
    const gids = gj.pal.map((c) => { let bd = 1e18, m = c;
      for (const q of lilyCols) { const d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]); if (d < bd) { bd = d; m = q; } }
      const k = (m[0] << 16) | (m[1] << 8) | m[2]; if (!gcache.has(k)) gcache.set(k, addCol(m[0], m[1], m[2])); return gcache.get(k); });
    LILYPAD_GIGV = { sx: gj.sx, sy: gj.sy, sz: gj.sz, vox: gj.vox.map((p) => (p & 0xffffff) | (gids[p >>> 24] << 24)) };
  } catch (e) { if (LGIG_ON) console.warn('[vb] lillypad_gigantic.json missing — giant pads skipped', e); }   // silent when the feature is simply OFF: the throw above is the switch, not a failure, and a warn every boot saying a present file is missing is worse than no warn at all
  const FERNIDS = [...new Set(FERN2V.flatMap((m) => m.vox.map((p) => p >>> 24)))];          // soft-decor id list for the pick-passthru + snow-bury sets
  if (MUSHV) for (const q of MUSHV.vox) decorTab[q >>> 24] = 1;                             // ── CHOPPABLE DECOR ── mushrooms…
  for (const i of FERNIDS) decorTab[i] = 1;                                                 // …and ferns (user): any tool takes chunks out of both
