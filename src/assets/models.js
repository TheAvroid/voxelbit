  // ── decoration models ── ONE sparse format everywhere: { sx (width, x), sy (depth, y), sz (height, z),
  // vox: [x | y<<8 | z<<16 | palId<<24, …] }. MagicaVoxel is z-up so model z maps straight to world y.
  // Each model gets its OWN palette ids (deduped within the model only) — solid, pickable and walk-through
  // classes can never collide on a shared id. All ids land above DECOR_MIN → walk-through unless marked solid.
  setLoad(22); await stage('loading decorations…');
  // colour → existing palette id, for SHARE mode below. Built once on first use, so it sees every
  // colour the world registered before any item model is parsed.
  // ── TOLERANCE SHARE ── how near is "the same colour". A max-channel delta of 2/255 is under a
  // percent and invisible on textured voxel art; the point is to stop minting a fresh slot for a
  // shade the table already holds. Two rules make it safe, and both matter:
  //   * DECOR_MIN floor — a loader must never be handed a SOLID id it did not ask for. solidTab is a
  //     RANGE test (i < DECOR_MIN), so sharing downward would silently make a model's voxels collide.
  //     The exact-match path above predates this and is left exactly as it was; only the new
  //     tolerance path is restricted, so nothing that works today changes.
  //   * palOwn skipped — for the reason it is skipped everywhere else: an own-ids model's set of ids
  //     IDENTIFIES it (the pinecone/stick pickup bug).
  // RAISED 6 -> 8 (2026-08-15, after the desert content landed). Same argument as below, one notch further and
  // for the same reason: at 6 the table was full again and SIX colours were falling through to palNearest with
  // NO bound, worst 15/255. At 8 nothing is substituted at all, 62 colours are reused instead of minted, 12
  // slots stay free, and no colour anywhere is more than 8/255 from what the artist authored. Worst case 15 -> 8.
  // ?paltol=6 puts it back.
  // WHY 6 AND NOT 2 (measured 2026-08-15). This is not a quality sacrifice, it is the opposite, and the
  // numbers are the argument. At tol=0 the table saturates and 19 colours fall through to palNearest, which
  // is bounded by NOTHING — worst error 15/255 at boot alone, and far worse in play, where a porcupine brown
  // once came back as a mushroom's olive. At tol=6 nothing is substituted at all (edSubs 0), 51 colours are
  // reused instead of minted, and NO colour anywhere is more than 6/255 from what the artist authored. Worst
  // case goes 15 -> 6 while 32 slots come free. Terrain is untouched at any tolerance: this only decides which
  // id a NEW model colour gets, never what an existing id looks like, and it is floored at DECOR_MIN.
  // ?paltol=N overrides for A/B; 0 restores the old exact-only behaviour.
  // ── 8 -> 12 (user 2026-08-18: "condense any palette colors that are either similar in shade or very similar
  // in shade. whatever slots you can gain make them black slots") ── measured over four boots of this build:
  //     paltol   palette  free   worst error   silently substituted
  //        8       255      1        8/255            0
  //       10       244     12        9/255            0
  //       12       237     19       11/255            0
  //       14       234     22       11/255            0
  // 12 is the knee: 18 slots for 3/255 of extra bounded error, and 14 buys only 3 more for nothing. Note the
  // last column stays ZERO throughout, which is the whole safety argument and is worth restating — a tolerance
  // reuse is BOUNDED by PAL_TOL, while the palNearest substitution it prevents is bounded by nothing at all
  // (15/255 at boot historically, and far worse in play: the green-porcupine bug). Raising this trades a
  // slightly larger bounded error for headroom, never for an unbounded one.
  // HELD ITEMS ARE UNAFFECTED: everything on the 'item'/'held' path passes noTol and still gets exact colours
  // or its own id, which is the fix the stone tools needed on 2026-08-15 and this must not undo.
  const PAL_TOL = (() => { const m = /[?&]paltol=(\d+)/.exec(location.search); return m ? +m[1] : 12; })();
  let palTolHits = 0;                                  // slots this saved — __vb.palAudit() reports it
  const palTolErr = {};                                // max-channel delta -> how many colours landed there
  const palTolIdx = new Map();                         // colour -> the id a TOLERANCE reuse gave it. Deliberately not palIdx: see the note at the call site
  let palSnaps = 0;                                    // colours the FULL table turned away and substituted — the overflow depth
  const palNearShare = (r, g, b) => {
    let bd = 1e9, best = -1;
    for (let i = DECOR_MIN; i < palette.length; i++) {
      const c = palette[i]; if (!c || palOwn.has(i)) continue;
      const d = Math.max(Math.abs(c[0] - r), Math.abs(c[1] - g), Math.abs(c[2] - b));
      if (d <= PAL_TOL && d < bd) { bd = d; best = i; }
    }
    if (best < 0) return undefined;
    palTolHits++; palTolErr[bd] = (palTolErr[bd] || 0) + 1;   // the error histogram is the whole quality argument: every reuse is bounded by PAL_TOL, while the palNearest substitution it REPLACES had no bound at all                                      // counts SLOTS saved, not queries — the caller caches the hit into palIdx below, so the same colour never walks this loop twice
    return best;
  };
  // ── noTol: SHARE, BUT ONLY ON AN EXACT MATCH ── the tolerance below is right for scenery a player sees at
  // 20 m and wrong for the thing in their hand. Measured 2026-08-15: at PAL_TOL 6, seven of the stone kit's 19
  // authored colours were reused off by up to 6/255 (the user's "slightly off"). A held tool fills a third of
  // the screen and is stared at constantly, so it mints its own id instead. Costs a handful of slots out of the
  // headroom the tolerance itself freed - that is what the headroom is FOR.
  // Nearest id within ONE unit on every channel, or undefined. Reserved ids (palOwn) and ids whose material
  // MEANS something (edMatBad) are never offered — the same two exclusions the tolerance share uses.
  const palNear1 = (r, g, b) => {
    for (let i = 1; i < palette.length; i++) { const c = palette[i];
      if (!c || palOwn.has(i)) continue;
      if (Math.abs(c[0] - r) <= 1 && Math.abs(c[1] - g) <= 1 && Math.abs(c[2] - b) <= 1) return i; }
    return undefined;
  };
  const palShare = (r, g, b, noTol) => {
    if (!palIdx) { palIdx = new Map();
      for (let i = 1; i < palette.length; i++) { const c = palette[i]; if (!c) continue;
        const k = (c[0] << 16) | (c[1] << 8) | c[2]; if (!palIdx.has(k)) palIdx.set(k, i); } }
    const k = (r << 16) | (g << 8) | b;
    let id = palIdx.get(k);
    if (id !== undefined && palOwn.has(id)) id = undefined;   // an exact colour match on a RESERVED id is not a match — fall through and mint (or snap to) something this model may legitimately share
    if (id === undefined && PAL_TOL > 0 && !noTol) {   // ── TOLERANCE SHARE ── exact match was the ONLY way to reuse a slot, so a model colour one unit off a colour the table already held minted a whole new id. Measured: that is where the 256 ceiling actually went — the accidental near-duplicates are all up in the loader-minted decor range, not in the hand-authored ramps.
      // The hit is cached in palTolIdx and NOT in palIdx, and that separation is the whole point. palIdx means
      // "this exact colour is at this id". Writing a tolerance substitute into it made it lie, and the lie then
      // defeated the noTol exemption below: the stone kit asked for its exact colour, hit the poisoned entry on
      // the EXACT path, and got the substitute anyway. Measured as still 7/19 tools wrong after the exemption
      // was added. Two maps, two meanings.
      id = palTolIdx.get(k);
      if (id === undefined) { id = palNearShare(r, g, b); if (id !== undefined) palTolIdx.set(k, id); }
    }
    // ── A noTol MODEL MAY STILL SHARE AN INVISIBLE NEIGHBOUR (2026-08-19, to free the two ids the light cherry
    // scatter needs) ── noTol exists because PAL_TOL is 6 and snapping a held tool's ramp within 6 collapsed
    // its shades (the stone kit bug the block above records). It does NOT need to mean "exact or mint": the
    // audit found the table holding 109,78,50 AND 109,79,51, and 115,83,54 AND 114,83,53 — two pairs one unit
    // apart on one or two channels, minted separately only because stick_1 and stick_2 are different files
    // parsed as 'held'. One unit of 255 is below anything a screen or an eye resolves, so sharing at delta 1
    // cannot flatten a ramp the way 6 did — a ramp with a 1-unit step has no step. It returns exactly 2 ids
    // on this table, which is what the light-cherry ground petals and twigs are minted from.
    // palOwn and edMatBad are still respected, via palNear1 — a reserved id means something and is never a
    // substitute, whatever the distance.
    if (id === undefined && noTol) { const n1 = palNear1(r, g, b); if (n1 !== undefined) id = n1; }   // NO length gate: the pairs this frees mint at ids ~127-139, long before any threshold worth setting, and a 1-unit share cannot flatten a ramp at any table size
    if (id === undefined) {
      if (palette.length < 256) { id = addCol(r, g, b); }
      else {                                           // FULL: nearest existing colour, never a 257th entry — an id past 255 wraps and takes a voxel's SOLIDITY with it. palNearest (assets/palette.js) is the same walk addCol's ceiling uses, and it skips reserved ids for the same reason: a full palette must not re-create the collision palOwn just refused.
        id = palNearest(r, g, b);
        palSnaps++;                                    // ── HOW OVERSUBSCRIBED ── this path was a bare console.warn and did NOT feed palOver, so the table has been running past its ceiling silently: every colour here is a SUBSTITUTE the player is already looking at. Counting it is what tells you how many slots a new material actually has to find.
        if (palSnaps <= 3) console.warn('[vb] palette full — item colour', r, g, b, 'snapped to id', id);
      }
      palIdx.set(k, id);
    }
    return id;
  };
  // the whole SCENE: models plus the transform each shape node places them at. A multi-model .vox is an
  // animation, and its nTRN/nSHP nodes are where the author's relative placement lives.
  const parseVoxScene = (pv, share, noTol) => {
    const pdv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    const models = [], shapes = [], trn = new Map(); const ppal = new Uint8Array(1024);
    const rdStr = (o) => { const n2 = pdv.getUint32(o, true); let t = ''; for (let i = 0; i < n2; i++) t += String.fromCharCode(pv[o + 4 + i]); return [t, o + 4 + n2]; };
    const rdDict = (o) => { let n2 = pdv.getUint32(o, true); o += 4; const d = {};
      for (let i = 0; i < n2; i++) { const a = rdStr(o); const b2 = rdStr(a[1]); d[a[0]] = b2[0]; o = b2[1]; } return [d, o]; };
    const walk = (off, end) => { while (off + 12 <= end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
      let o = off + 12;
      if (id === 'SIZE') models.push({ sx: pdv.getUint32(o, true), sy: pdv.getUint32(o + 4, true), sz: pdv.getUint32(o + 8, true), raw: null });
      else if (id === 'XYZI') { const m = models.find((mm) => !mm.raw); if (m) { const cnt = pdv.getUint32(o, true); m.raw = pv.subarray(o + 4, o + 4 + cnt * 4); } }
      else if (id === 'RGBA') ppal.set(pv.subarray(o, o + 1024));
      else if (id === 'nTRN') { const nid = pdv.getInt32(o, true); o += 4; const a = rdDict(o); o = a[1];
        const child = pdv.getInt32(o, true); o += 16; const nfr = pdv.getInt32(o - 4, true);
        let t = [0, 0, 0];
        for (let f = 0; f < nfr; f++) { const d = rdDict(o); o = d[1];
          if (d[0]._t) { const q = d[0]._t.split(' ').map(Number); if (q.length === 3) t = q; } }
        trn.set(child, t); }
      else if (id === 'nSHP') { const nid = pdv.getInt32(o, true); o += 4; const a = rdDict(o); o = a[1];
        const nm = pdv.getInt32(o, true); o += 4; const ids = [];
        for (let i = 0; i < nm; i++) { ids.push(pdv.getInt32(o, true)); o += 4; const d = rdDict(o); o = d[1]; }
        shapes.push({ node: nid, ids }); }
      else if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      off += 12 + bsz + csz;
    } };
    walk(8, pv.length);
    const cmap = new Map();
    const colId = (ci) => { let c = cmap.get(ci); if (c === undefined) {
      const cr = ppal[(ci - 1) * 4], cg = ppal[(ci - 1) * 4 + 1], cb = ppal[(ci - 1) * 4 + 2];
      c = share ? palShare(cr, cg, cb, noTol) : addCol(cr, cg, cb); cmap.set(ci, c); } return c; };
    for (const sh of shapes) sh.t = trn.get(sh.node) || [0, 0, 0];
    // world-space origin of a model under a shape: MagicaVoxel translates the model's CENTRE
    const org = (sh, mi) => { const m = models[mi];
      return [sh.t[0] - (m.sx >> 1), sh.t[1] - (m.sy >> 1), sh.t[2] - (m.sz >> 1)]; };
    return { models, shapes, org, colId };
  };
  const parseVoxAll = (pv, share, noTol) => {
    const pdv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    const models = []; const ppal = new Uint8Array(1024);
    const walk = (off, end) => { while (off + 12 <= end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
      if (id === 'SIZE') models.push({ sx: pdv.getUint32(off + 12, true), sy: pdv.getUint32(off + 16, true), sz: pdv.getUint32(off + 20, true), raw: null });
      else if (id === 'XYZI') { const m = models.find((mm) => !mm.raw); if (m) { const cnt = pdv.getUint32(off + 12, true); m.raw = pv.subarray(off + 16, off + 16 + cnt * 4); } }
      else if (id === 'RGBA') ppal.set(pv.subarray(off + 12, off + 12 + 1024));
      else if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      off += 12 + bsz + csz;
    } };
    walk(8, pv.length);
    const out = [];
    for (const m of models) {
      if (!m.raw) continue;
      const cmap = new Map(), mvox = [];
      for (let i = 0; i < m.raw.length; i += 4) {
        const ci = m.raw[i + 3];
        let cid = cmap.get(ci);
        if (cid === undefined) { const cr = ppal[(ci - 1) * 4], cg = ppal[(ci - 1) * 4 + 1], cb = ppal[(ci - 1) * 4 + 2];
          if (share) { cid = palShare(cr, cg, cb, noTol); } else { cid = addCol(cr, cg, cb); palOwn.add(cid); }   // own ids are RESERVED — see palOwn
        cmap.set(ci, cid); }
        mvox.push(m.raw[i] | (m.raw[i + 1] << 8) | (m.raw[i + 2] << 16) | (cid << 24));
      }
      out.push({ sx: m.sx, sy: m.sy, sz: m.sz, vox: mvox });
    }
    return out;
  };
  const parseVoxModel = (pv, share, noTol, colMap) => {  // share: reuse an existing palette id for an exact colour match — ITEM-ONLY models (see the bow strip). noTol: exact matches only, no tolerance reuse (held items)
    const pdv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    let sx = 0, sy = 0, sz = 0, pvox = null; const ppal = new Uint8Array(1024);
    const walk = (off, end) => { while (off < end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
      if (id === 'SIZE' && !sx) { sx = pdv.getUint32(off + 12, true); sy = pdv.getUint32(off + 16, true); sz = pdv.getUint32(off + 20, true); }
      else if (id === 'XYZI' && !pvox) { const n = pdv.getUint32(off + 12, true); pvox = pv.subarray(off + 16, off + 16 + n * 4); }
      else if (id === 'RGBA') ppal.set(pv.subarray(off + 12, off + 12 + 1024));
      else if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      off += 12 + bsz + csz;
    } };
    walk(8, pv.length);
    if (!pvox) throw new Error('no XYZI chunk');
    const cmap = new Map(), mvox = [];
    for (let i = 0; i < pvox.length; i += 4) {
      const ci = pvox[i + 3];
      let cid = cmap.get(ci);
      if (cid === undefined) { const cr = ppal[(ci - 1) * 4], cg = ppal[(ci - 1) * 4 + 1], cb = ppal[(ci - 1) * 4 + 2];
        // ── colMap: ONE SET OF OWN IDS ACROSS SEVERAL FILES ── cmap above only dedupes within a model, and
        // own-ids deliberately never share, so N files carrying the same authored palette minted N copies of
        // it (measured: two shrub .vox took 10 ids for 5 colours). Pass the same Map to each parse and the
        // first file's ids are reused by the rest. It is filled on the way through, so the caller does not
        // have to know the colours in advance.
        const ck = (cr << 16) | (cg << 8) | cb;
        if (colMap && colMap.has(ck)) { cid = colMap.get(ck); }
        else if (share) { cid = palShare(cr, cg, cb, noTol); }
        else { cid = addCol(cr, cg, cb); palOwn.add(cid); }   // own ids are RESERVED — see palOwn
        if (colMap && !colMap.has(ck)) colMap.set(ck, cid);
        cmap.set(ci, cid); }
      mvox.push(pvox[i] | (pvox[i + 1] << 8) | (pvox[i + 2] << 16) | (cid << 24));   // cid<<24 may flip the sign bit — every reader uses bitwise ops, so that's fine
    }
    return { sx, sy, sz, vox: mvox };
  };
  // ── EVERY MODEL IN A MULTI-MODEL .vox, AS SEPARATE DECOR VARIANTS ── parseVoxModel above deliberately reads
  // only the FIRST SIZE/XYZI pair (the `!sx` / `!pvox` guards), because every other caller in this tree has one
  // model per file. A VARIANT SET is the other shape and it needs its own function rather than a flag, because
  // the return type differs: flowers.vox is five different flowers, not five frames of one, so each model
  // becomes its own entry and the ground scatter picks between them.
  // NOT tools/split_vox_frames.py's job either — that splits FRAMES to disk, and its own guard refuses a file
  // whose models differ more than 4x in volume, which this one does (27 to 125) precisely because the variants
  // are different plants rather than poses of one.
  // ONE colMap ACROSS ALL THE MODELS, for the reason parseVoxModel's colMap note gives: the variants overlap
  // heavily — the same stem green, the same white — and parsing them independently would mint that green once
  // per model. Shared, the whole set costs what one model's palette costs plus what the others add.
  // maxN: mint colours for only the FIRST N models. A hand-authored .vox often carries an animation rig
  // alongside the model you want - penguin.vox is two penguins and twelve keyframed body parts - and every
  // model this walks MINTS PALETTE IDS. On a table at 256/256 that is not a waste, it is a corruption: addCol
  // stops growing and starts SNAPPING to the nearest existing colour, so a part's shade quietly becomes some
  // other decoration's id and the two share material flags from then on. Measured on penguin.vox: 16 colours
  // substituted with all fourteen models parsed, 0 with the leading few.
  const parseVoxVariants = (pv, share, noTol, maxN) => {
    const pdv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    const sizes = [], raws = []; const ppal = new Uint8Array(1024);
    const walk = (off, end) => { while (off < end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
      if (id === 'SIZE') sizes.push([pdv.getUint32(off + 12, true), pdv.getUint32(off + 16, true), pdv.getUint32(off + 20, true)]);
      else if (id === 'XYZI') { const n = pdv.getUint32(off + 12, true); raws.push(pv.subarray(off + 16, off + 16 + n * 4)); }
      else if (id === 'RGBA') ppal.set(pv.subarray(off + 12, off + 12 + 1024));
      else if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      off += 12 + bsz + csz;
    } };
    walk(8, pv.length);
    if (!raws.length) throw new Error('no XYZI chunk');
    const colMap = new Map(), out = [];
    const nM = maxN ? Math.min(maxN, raws.length) : raws.length;
    for (let m = 0; m < nM; m++) {
      const raw = raws[m], sz = sizes[m] || sizes[0], cmap = new Map(), mvox = [];
      for (let i = 0; i < raw.length; i += 4) {
        const ci = raw[i + 3];
        let cid = cmap.get(ci);
        if (cid === undefined) {
          const cr = ppal[(ci - 1) * 4], cg = ppal[(ci - 1) * 4 + 1], cb = ppal[(ci - 1) * 4 + 2];
          const ck = (cr << 16) | (cg << 8) | cb;
          if (colMap.has(ck)) cid = colMap.get(ck);
          else { if (share) cid = palShare(cr, cg, cb, noTol); else { cid = addCol(cr, cg, cb); palOwn.add(cid); }
            colMap.set(ck, cid); }
          cmap.set(ci, cid);
        }
        mvox.push(raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16) | (cid << 24));
      }
      out.push({ sx: sz[0], sy: sz[1], sz: sz[2], vox: mvox });
    }
    return out;
  };
  // ── WHAT COLOURS DOES THIS .vox ACTUALLY USE ── the distinct RGBs its voxels reference, and NOTHING ELSE:
  // no palette id is minted, shared or reserved by looking. That is the whole point. parseVoxModel decides an
  // id the moment it meets a colour, so a caller that wants to choose the mapping itself — quantize a ramp,
  // refuse a stray shade, spend a fixed budget — has to see every colour in every file BEFORE the first parse.
  // Pre-reading is also what lets a hand-authored asset change shade count without the loader minting on a
  // full table (the desert shrubs, assets/bow.js). A .vox palette holds 256 entries whatever the model uses,
  // so the XYZI indices are read, not the RGBA chunk: an unused swatch left in the file is not a colour.
  const voxColsUsed = (pv) => {
    const pdv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
    let pvox = null; const ppal = new Uint8Array(1024);
    const walk = (off, end) => { while (off < end) {
      const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
      const bsz = pdv.getUint32(off + 4, true), csz = pdv.getUint32(off + 8, true);
      if (id === 'XYZI' && !pvox) { const n = pdv.getUint32(off + 12, true); pvox = pv.subarray(off + 16, off + 16 + n * 4); }
      else if (id === 'RGBA') ppal.set(pv.subarray(off + 12, off + 12 + 1024));
      else if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
      off += 12 + bsz + csz;
    } };
    walk(8, pv.length);
    const seen = new Uint8Array(256), out = [];
    if (pvox) for (let i = 3; i < pvox.length; i += 4) { const ci = pvox[i];
      if (seen[ci]) continue; seen[ci] = 1;
      out.push([ppal[(ci - 1) * 4], ppal[(ci - 1) * 4 + 1], ppal[(ci - 1) * 4 + 2]]);   // MagicaVoxel indices are 1-based and the RGBA chunk is not, exactly as parseVoxModel reads it
    }
    return out;
  };
