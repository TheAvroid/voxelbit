  // ── decoration models ── ONE sparse format everywhere: { sx (width, x), sy (depth, y), sz (height, z),
  // vox: [x | y<<8 | z<<16 | palId<<24, …] }. MagicaVoxel is z-up so model z maps straight to world y.
  // Each model gets its OWN palette ids (deduped within the model only) — solid, pickable and walk-through
  // classes can never collide on a shared id. All ids land above DECOR_MIN → walk-through unless marked solid.
  setLoad(22); await stage('loading decorations…');
  // colour → existing palette id, for SHARE mode below. Built once on first use, so it sees every
  // colour the world registered before any item model is parsed.
  const palShare = (r, g, b) => {
    if (!palIdx) { palIdx = new Map();
      for (let i = 1; i < palette.length; i++) { const c = palette[i]; if (!c) continue;
        const k = (c[0] << 16) | (c[1] << 8) | c[2]; if (!palIdx.has(k)) palIdx.set(k, i); } }
    const k = (r << 16) | (g << 8) | b;
    let id = palIdx.get(k);
    if (id !== undefined && palOwn.has(id)) id = undefined;   // an exact colour match on a RESERVED id is not a match — fall through and mint (or snap to) something this model may legitimately share
    if (id === undefined) {
      if (palette.length < 256) { id = addCol(r, g, b); }
      else {                                           // FULL: nearest existing colour, never a 257th entry — an id past 255 wraps and takes a voxel's SOLIDITY with it. palNearest (assets/palette.js) is the same walk addCol's ceiling uses, and it skips reserved ids for the same reason: a full palette must not re-create the collision palOwn just refused.
        id = palNearest(r, g, b);
        console.warn('[vb] palette full — item colour', r, g, b, 'snapped to id', id);
      }
      palIdx.set(k, id);
    }
    return id;
  };
  // the whole SCENE: models plus the transform each shape node places them at. A multi-model .vox is an
  // animation, and its nTRN/nSHP nodes are where the author's relative placement lives.
  const parseVoxScene = (pv, share) => {
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
      c = share ? palShare(cr, cg, cb) : addCol(cr, cg, cb); cmap.set(ci, c); } return c; };
    for (const sh of shapes) sh.t = trn.get(sh.node) || [0, 0, 0];
    // world-space origin of a model under a shape: MagicaVoxel translates the model's CENTRE
    const org = (sh, mi) => { const m = models[mi];
      return [sh.t[0] - (m.sx >> 1), sh.t[1] - (m.sy >> 1), sh.t[2] - (m.sz >> 1)]; };
    return { models, shapes, org, colId };
  };
  const parseVoxAll = (pv, share) => {
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
          if (share) { cid = palShare(cr, cg, cb); } else { cid = addCol(cr, cg, cb); palOwn.add(cid); }   // own ids are RESERVED — see palOwn
        cmap.set(ci, cid); }
        mvox.push(m.raw[i] | (m.raw[i + 1] << 8) | (m.raw[i + 2] << 16) | (cid << 24));
      }
      out.push({ sx: m.sx, sy: m.sy, sz: m.sz, vox: mvox });
    }
    return out;
  };
  const parseVoxModel = (pv, share) => {                 // share: reuse an existing palette id for an exact colour match — ITEM-ONLY models (see the bow strip)
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
        if (share) { cid = palShare(cr, cg, cb); } else { cid = addCol(cr, cg, cb); palOwn.add(cid); }   // own ids are RESERVED — see palOwn
        cmap.set(ci, cid); }
      mvox.push(pvox[i] | (pvox[i + 1] << 8) | (pvox[i + 2] << 16) | (cid << 24));   // cid<<24 may flip the sign bit — every reader uses bitwise ops, so that's fine
    }
    return { sx, sy, sz, vox: mvox };
  };
