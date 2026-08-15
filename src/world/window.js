  // ── ENDLESS WORLD ── voxel = 10 cm. A 768×160×768 window (94 MB u8) slides with the player over an
  // infinite deterministic world. Storage is TOROIDAL: shifting the window regenerates + uploads only the
  // 8-voxel strip that wrapped — never a full-buffer move. All generation is a pure function of WORLD
  // coordinates, so strips re-materialise seamlessly and revisited terrain is always identical.
  const WX = WXZ, WY = WYpick, WZ = WXZ;              // deep world: +128 voxels of stone below the surface for TRUE gorge depth
  const LIFT = WY >= 384 ? 128 : 0;                    // terrain floats this far above bedrock
  const BX = WX >> 3, BY = WY >> 3, BZ = WZ >> 3;     // 8³ brick occupancy for empty-space skipping
  const HALF = WX >> 1;
  const RD_FIXED = Math.min(1000, HALF - 24);           // ── VIEW DISTANCE ── pinned at 100 m (1000 vox), no slider (user). Clamped to the window: if the adapter caps the window at 768 this falls back to what fits rather than reaching past it.
  const W = new Uint8Array(WX * WY * WZ);             // CPU copy — collision + build (toroidal)
  const W32 = new Uint32Array(W.buffer);
  const hmap = new Int16Array(WX * WZ);               // terrain top per column (toroidal)
  const stopY = new Int16Array(WX * WZ);              // ── scanTop COLUMN CACHE ── the snowfall sweep asks for the topmost LANDING surface of every column in a
  const stopS = new Uint16Array(WX * WZ);             // 120-radius disc EVERY frame (~5,000 calls) and the answer almost never changes; profiling put scanTop at 27%
  let stopF = 1;                                      // of ALL js time during a storm — the single biggest cost in the engine while it snows. stopS is a frame stamp
  let STOP_CACHE = 1;                                  // A/B the scanTop column cache: a STALE column would land flakes at a surface that is no longer there
  const STOP_TTL = 30;                                // (0 = invalid): gpuPatch — the one funnel every runtime mutation of W goes through — clears the touched column,
                                                      // and the TTL is a self-healing backstop, so a writer that ever bypasses the funnel (chunk generation streams
                                                      // terrain straight into W) costs half a second of staleness instead of a wrong landing height forever.
  const bricks = new Uint32Array((BX * BY * BZ + 31) >> 5);
  const wbricks = new Uint32Array((BX * BY * BZ + 31) >> 5);   // "occupied brick contains ONLY water" — skipW rays (underwater camera, refraction, reflection) stride whole 8³ bricks through the water body instead of fine-stepping every voxel; LOSSLESS (nothing in an all-water brick is hittable when water is skipped)
  const B2X = BX >> 2, B2Y = BY >> 2, B2Z = BZ >> 2;  // L2 occupancy: 32-voxel SUPER-bricks — long rays leap 32 voxels through open air
  const bricks2 = new Uint32Array((B2X * B2Y * B2Z + 31) >> 5);
  const touched = new Uint8Array(BX * BZ);             // per 8×8 column tile: has this memory EVER been written? virgin tiles skip air-clearing (~40% of a column)
  let lgtPaint = () => {};                             // panel repaint — held here so __vb.lgt() from the console keeps the buttons honest instead of silently disagreeing with the image
  const LGT_ALL = 0xffffff;                            // 24 lighting/shading terms, all enabled = the normal image (see the top-right panel / LG() in the shader). Bits 18-23 are the WATER group (user 2026-08-05). Stays exact in the f32 uniform: integers are exact to 2^24.
  const LGT_WATER = 0xfc0000;                          // bits 18-23 — the WATER group, and the only terms the panel exposes (user 2026-08-05: "I only want buttons that change the water")
  const LGT2_ALL = 0x0;                                // ── SECOND TERM MASK (u.lgt.z) ── EMPTY. lgt.x is full at 24 bits (an f32 is exact only to 2^24, so a 25th bit there would round), so this is where a 25th term would go. Three groups have lived and died here on 2026-08-09: the water soft glisten (bit 0), the tier-1 LOOK set (bits 1-6) and the tier-2 set (bits 0-3). LG2() stays wired for whatever is next.
  // ══ WATER BAKE (user 2026-08-05) ══ THE defaults for every water control. Tune with the top-right panel,
  // hit `copy` on its bake row, and paste the line it gives you OVER this one — that is the whole workflow.
  // A player who has never touched the panel gets exactly what is written here; `reset` in the panel puts a
  // live session back to it. `reflection` is the Fresnel mirror weight (1 = physical Schlick), the rest are
  // on/off. Anything the player HAS changed is remembered in localStorage and wins until they hit reset.
  const WATER_BAKE = { reflect: 1, refract: 1, foam: 1, ice: 1, pixelGlisten: 1, waves: 0, reflection: 0.45 };
  const WBIT = { reflect: 18, refract: 19, foam: 20, ice: 21, pixelGlisten: 22, waves: 23 };   // …their bits in u.lgt.x
  const wBakeMask = () => { let m = LGT_ALL & ~LGT_WATER; for (const k in WBIT) if (WATER_BAKE[k]) m |= (1 << WBIT[k]); return m; };
  const wBakeMask2 = () => 0;                          // nothing lives in the second mask (see LGT2_ALL)
  const wBakeRefl = () => { const v = +WATER_BAKE.reflection; return (isFinite(v) && v >= 0 && v <= 2) ? v : 1; };
  // Everything OUTSIDE the water group is FORCED ON at load. The panel used to carry all 24 terms, so a
  // saved mask can have sun shadow / AO / fog / TAA switched off from an earlier bisection — and with those
  // rows gone there would be no way left to switch them back. Only the water bits are restored from storage.
  let lgtMask = (() => { try { const v = localStorage.getItem('vb_lgt');
    return v === null ? wBakeMask() : (((parseInt(v, 10) & LGT_WATER) | (LGT_ALL & ~LGT_WATER)) & LGT_ALL); } catch (e) { return wBakeMask(); } })();
  let lgtMask2 = 0;                                    // …so this is 0 and stays 0. Kept as a variable because the frame still writes it to u.lgt.z (UF[UF_LGT + 2]) and __vb.lgt2() still reads it.
  // ── WATER REFLECTION STRENGTH (user 2026-08-05) ── multiplies the Fresnel mirror/transmission split.
  // 1 = physical (pure Schlick, what it has always been), 0 = no mirror at all, 2 = twice as reflective.
  let wReflK = (() => { try { const v = parseFloat(localStorage.getItem('vb_wrefl')); return (isFinite(v) && v >= 0 && v <= 2) ? v : wBakeRefl(); } catch (e) { return wBakeRefl(); } })();
  const REACT_FADE = 450;                              // ms for the reactive mask to fade out after the last body motion. Long enough that a trunk jittering in and out of sleep on its contacts reads as one continuous settle rather than a strobe.
  let reactT0 = -1e9;                                  // when a rigid body was last in motion (see physC.y / the reactive mask)
  let winOX = 0, winOZ = 0;                            // world coord of the window corner (multiples of 8)
  const rect = { xlo: 0, xhi: 0, zlo: 0, zhi: 0 };     // the fully-GENERATED world rectangle (8-aligned) — only terrain inside it is ever traced
  const gwrap = (v, n) => ((v % n) + n) % n;
  let SPWX = 0, SPWZ = 0;                              // world spawn — placeholder; RANDOMISED on every refresh at boot (user 2026-07-20), see the spawn block below
  let SPYAW = -1.517; const SPPITCH = -0.044;          // spawn CAMERA facing baked with the position (same T export); the lake-side spawn below re-aims the yaw at the water

  // deterministic integer hash — the shader ports this bit-for-bit for the far-field terrain
  const ihash = (x, z) => { let h = (Math.imul(x, 374761393) + Math.imul(z, 668265263)) | 0; h = Math.imul(h ^ (h >>> 13), 1274126177); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
  const ROCKSTEP = Math.imul(57, 374761393);           // Δ of the inlined rock-shade hash per +1 y (x advances by 57)
  const sstep = (t) => t * t * (3 - 2 * t);
  const vnoise = (x, z) => { const ix = Math.floor(x), iz = Math.floor(z), fx = sstep(x - ix), fz = sstep(z - iz);
    return (ihash(ix, iz) * (1 - fx) + ihash(ix + 1, iz) * fx) * (1 - fz) + (ihash(ix, iz + 1) * (1 - fx) + ihash(ix + 1, iz + 1) * fx) * fz; };
  const fbm = (x, z) => vnoise(x, z) * 0.55 + vnoise(x * 2.13 + 11.7, z * 2.13 + 5.3) * 0.27 + vnoise(x * 4.41 + 41.2, z * 4.41 + 23.8) * 0.18;
  const vnoise3 = (x, y, z) => {                       // 3D value noise — trilinear smoothstep over the ihash lattice. COHERENT (unlike a raw per-voxel ihash), so cave walls get organic bulges instead of grit
    const ix = Math.floor(x), iy = Math.floor(y), iz = Math.floor(z);
    const fx = sstep(x - ix), fy = sstep(y - iy), fz = sstep(z - iz);
    const h3 = (a, b, c) => ihash(a + Math.imul(c, 92837111), b + Math.imul(c, 689287499));   // fold the 3rd axis into the 2D hash — distinct large primes keep the lattice planes decorrelated
    const c00 = h3(ix, iy, iz) * (1 - fx) + h3(ix + 1, iy, iz) * fx, c10 = h3(ix, iy + 1, iz) * (1 - fx) + h3(ix + 1, iy + 1, iz) * fx;
    const c01 = h3(ix, iy, iz + 1) * (1 - fx) + h3(ix + 1, iy, iz + 1) * fx, c11 = h3(ix, iy + 1, iz + 1) * (1 - fx) + h3(ix + 1, iy + 1, iz + 1) * fx;
    return (c00 * (1 - fy) + c10 * fy) * (1 - fz) + (c01 * (1 - fy) + c11 * fy) * fz;
  };
  const HMAX = Math.min(105 + LIFT, WY - 122);         // terrain ceiling
  const WL = 24 + LIFT;                                // GLOBAL water level — water is simply terrain below this line
  const baseH = (x, z) => {
    const b = 8 + LIFT + 88 * fbm(x * 0.008, z * 0.008);
    const shoreK = Math.min(1, Math.abs(b - WL) / 12);   // fine detail fades out near the waterline — smooth, beach-like entries into water
    return Math.min(HMAX, Math.max(4 + LIFT, Math.round(b + 9 * fbm(x * 0.04 + 7.3, z * 0.04 + 2.1) * (0.2 + 0.8 * shoreK))));
  };
  const basinM = (x, z) => {                           // huge, rare low-frequency basins pull the land under the waterline (threshold halved — lakes are rarer)
    const b = vnoise(x * 0.0016 + 313.7, z * 0.0016 + 157.3);
    if (b >= 0.065) return 0;
    return sstep(Math.min(1, (0.065 - b) / 0.06));
  };
  const H = (x, z) => {
    let h = baseH(x, z);
    const bm = basinM(x, z);
    const m = bm * Math.max(0, Math.min(1, (66 + LIFT - h) / 20));   // basins only form in low country
    if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(x * 13 + 7, z * 17 + 3) - 0.5) * 0.8);   // gently dithered — no terrace banding
    const rs = riverS(x, z);
    const bn = fbm(x * 0.05 + 13.7, z * 0.05 + 4.2);   // bed/beach relief — lakebeds and sand flats are no longer billiard-flat
    if (rs > 0.02) h = Math.min(h, Math.round(h * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(x * 19 + 5, z * 23 + 9) - 0.5) * 0.8));   // noisy bed + gently dithered banks
    if (h <= WL && h >= WL - 5 && bm <= 0.25 && rs <= 0.04) h = WL + 1 + Math.max(0, Math.round((bn - 0.55) * 5));   // beach flats get 0-2 voxel dune relief
    return h;
  };
  const RIVCELL = 768, RIVINF = 6200;                  // WATERSHEDS — one candidate per ~77 m cell, rare roll; each hit is a whole dendritic system (influence radius must cover the longest possible chain)
  const rivCache = new Map();
  function riverAt(cx, cz) {                           // builds a WATERSHED: 1-3 tributaries join a main stem at confluences, the stem widens downstream
    const key = cx * 100003 + cz;                      // into a BIG reservoir lake, and ~half the reservoirs spill an OUTLET river that ends in a smaller tail lake.
    let R = rivCache.get(key);                         // R = { segs: [{sx,sz,dxr,dzr,len,wb,seed,t0,t1}], lakes: [{x,z,r,seed}], bbox }
    if (R !== undefined) return R;
    R = null;
    if (ihash(cx * 83 + 19, cz * 89 + 7) <= 0.035) {   // rarer than the old isolated segments — water stays scarce, but every occurrence is a connected system
      const hx = cx * RIVCELL + 100 + ihash(cx * 3 + 61, cz * 7 + 23) * (RIVCELL - 200);   // headwater of the main stem
      const hz = cz * RIVCELL + 100 + ihash(cx * 9 + 47, cz * 5 + 83) * (RIVCELL - 200);
      const ang = ihash(cx + 15, cz + 92) * Math.PI;
      const dxr = Math.cos(ang), dzr = Math.sin(ang);
      const Lm = 1800 + ihash(cx * 11 + 6, cz * 13 + 31) * 800;    // main stem 180-260 m
      const wbM = 58 + ihash(cx * 17 + 8, cz * 19 + 2) * 42;
      const seed = cx * 571 + cz * 769;
      const mx = hx + dxr * Lm, mz = hz + dzr * Lm;
      const segs = [{ sx: hx, sz: hz, dxr, dzr, len: Lm, wb: wbM, seed, t0: 0.6, t1: 1.15 }];   // the stem WIDENS downstream like a real river
      const lakes = [{ x: mx, z: mz, r: 200 + ihash(cx * 31 + 9, cz * 37 + 5) * 100, seed }];   // the reservoir it feeds
      const nT = 1 + ((ihash(cx * 7 + 44, cz * 3 + 18) * 2.99) | 0);
      for (let i = 0; i < nT; i++) {                   // TRIBUTARIES — branch back-and-out from a confluence on the stem, narrowing toward their heads
        const f = 0.25 + ihash(cx * 13 + i * 17, cz * 11 + i * 23) * 0.55;
        const jx = hx + dxr * (Lm * f), jz = hz + dzr * (Lm * f);
        const side = ihash(cx * 5 + i * 31, cz * 29 + i * 7) < 0.5 ? 1 : -1;
        const ta = ang + Math.PI + side * (0.4 + ihash(cx * 19 + i * 3, cz * 41 + i * 13) * 0.55);
        const tl = 600 + ihash(cx * 23 + i * 29, cz * 17 + i * 37) * 800;
        const tdx = Math.cos(ta), tdz = Math.sin(ta);
        segs.push({ sx: jx, sz: jz, dxr: tdx, dzr: tdz, len: tl, wb: wbM * 0.55, seed: seed + 97 * (i + 1), t0: 1.0, t1: 0.55 });
        if (ihash(cx * 37 + i * 5, cz * 43 + i * 11) < 0.4)        // some tributaries rise from a small HEADWATER POND
          lakes.push({ x: jx + tdx * tl, z: jz + tdz * tl, r: 55 + ihash(cx * 47 + i * 7, cz * 53 + i * 3) * 45, seed: seed + 31 * (i + 1) });
      }
      if (ihash(cx * 61 + 13, cz * 59 + 27) < 0.55) {  // OUTLET — the reservoir FEEDS a downstream river that ends in a smaller tail lake
        const oa = ang + (ihash(cx * 67 + 5, cz * 71 + 9) - 0.5) * 1.0;
        const odx = Math.cos(oa), odz = Math.sin(oa);
        const ol = 900 + ihash(cx * 73 + 21, cz * 79 + 15) * 700;
        const osx = mx + odx * (lakes[0].r * 0.7), osz = mz + odz * (lakes[0].r * 0.7);   // starts inside the reservoir rim - seamless junction
        segs.push({ sx: osx, sz: osz, dxr: odx, dzr: odz, len: ol, wb: wbM * 0.8, seed: seed + 501, t0: 1.0, t1: 0.9 });
        lakes.push({ x: osx + odx * ol, z: osz + odz * ol, r: 120 + ihash(cx * 89 + 3, cz * 97 + 7) * 60, seed: seed + 733 });
      }
      let x0 = 1e9, x1 = -1e9, z0 = 1e9, z1 = -1e9;    // bbox over every segment + lake (+ meander & wobble margin)
      for (const sg of segs) { const ex = sg.sx + sg.dxr * sg.len, ez = sg.sz + sg.dzr * sg.len; const pad = sg.wb * 1.4 * Math.max(sg.t0, sg.t1) + 60;
        x0 = Math.min(x0, sg.sx - pad, ex - pad); x1 = Math.max(x1, sg.sx + pad, ex + pad);
        z0 = Math.min(z0, sg.sz - pad, ez - pad); z1 = Math.max(z1, sg.sz + pad, ez + pad); }
      for (const L of lakes) { const pad = L.r * 1.18 + 24;
        x0 = Math.min(x0, L.x - pad); x1 = Math.max(x1, L.x + pad);
        z0 = Math.min(z0, L.z - pad); z1 = Math.max(z1, L.z + pad); }
      R = { segs, lakes, x0, x1, z0, z1 };
    }
    rivCache.set(key, R);
    return R;
  }
  const rivEval = (R, x, z) => {                       // watershed strength at this column: max over channel segments + lakes
    if (x < R.x0 || x > R.x1 || z < R.z0 || z > R.z1) return 0;   // bbox fast reject
    let best = 0;
    for (const sg of R.segs) {
      const tRaw = (x - sg.sx) * sg.dxr + (z - sg.sz) * sg.dzr;
      const t = Math.max(0, Math.min(sg.len, tRaw));
      const off = Math.sin(t * 0.015 + sg.seed) * 30 + Math.sin(t * 0.04 + sg.seed * 1.7) * 10;   // broad meanders
      const pd = (x - (sg.sx + sg.dxr * t)) * (-sg.dzr) + (z - (sg.sz + sg.dzr * t)) * sg.dxr;
      const w = sg.wb * 1.4 * (sg.t0 + (sg.t1 - sg.t0) * (t / sg.len));   // width taper: stems widen downstream, tributaries narrow to their heads
      const over = tRaw < 0 ? -tRaw : (tRaw > sg.len ? tRaw - sg.len : 0);   // rounded end caps - no strip past the endpoints (the old straight-cutoff bug)
      const d = Math.hypot(Math.abs(pd - off), over);
      if (d < w) { const v = sstep(1 - d / w); if (v > best) best = v; }
    }
    for (const L of R.lakes) {                         // lakes: wobbled organic shorelines, SATURATED strength across the body (the H carve only makes water above rs=0.75)
      const dl = Math.hypot(x - L.x, z - L.z);
      if (dl < L.r * 1.15) {
        const al = Math.atan2(z - L.z, x - L.x);
        const wr = L.r * (1 + 0.10 * Math.sin(al * 3 + L.seed) + 0.05 * Math.sin(al * 7 + L.seed * 2.3));
        if (dl < wr) { const v = sstep(Math.min(1, (1 - dl / wr) * 3)); if (v > best) best = v; }
      }
    }
    return best;
  };
  let rivScope = null;                                 // bulk-gen fast path: the rivers relevant to a region, gathered ONCE — not a 49-cell scan per column
  function gatherRivers(x0, x1, z0, z1) {
    const list = [];
    for (let jz = Math.floor((z0 - RIVINF) / RIVCELL); jz <= Math.floor((z1 + RIVINF) / RIVCELL); jz++)
      for (let jx = Math.floor((x0 - RIVINF) / RIVCELL); jx <= Math.floor((x1 + RIVINF) / RIVCELL); jx++) {
        const R = riverAt(jx, jz); if (R) list.push(R);
      }
    return { x0, x1, z0, z1, list };
  }
  function riverS(x, z) {                              // channel strength 0..1 at this column
    let best = 0;
    if (rivScope && x >= rivScope.x0 && x < rivScope.x1 && z >= rivScope.z0 && z < rivScope.z1) {
      for (const R of rivScope.list) { const v = rivEval(R, x, z); if (v > best) best = v; }
      return best;
    }
    for (let jz = Math.floor((z - RIVINF) / RIVCELL); jz <= Math.floor((z + RIVINF) / RIVCELL); jz++)
      for (let jx = Math.floor((x - RIVINF) / RIVCELL); jx <= Math.floor((x + RIVINF) / RIVCELL); jx++) {
        const R = riverAt(jx, jz); if (!R) continue;
        const v = rivEval(R, x, z); if (v > best) best = v;
      }
    return best;
  }
  // ── WHERE A BOULDER SITS ── stampBoulder probed groundMin at a radius CAPPED AT 10 while a rocks26 model is
  // up to 74 wide, so on any slope the far lobes were seated off ground the probe never looked at and hung in
  // the air. Measured 2026-08-07: up to 13% of a rock's underside overhanging, drops of 8-9 voxels. This probes
  // the model's real half-footprint and the diagonals too, because a 5-sample cross misses the corners of a
  // blob this wide. Still only 9 H() calls, and only mid/big rocks pay them.
  const rockSeatY = (m, x, z) => { const r = Math.max(2, Math.max(m.sx, m.sy) >> 1), d = (r * 0.7071) | 0;
    return Math.min(H(x, z), H(x - r, z), H(x + r, z), H(x, z - r), H(x, z + r),
                    H(x - d, z - d), H(x + d, z - d), H(x - d, z + d), H(x + d, z + d)); };
  const groundMin = (x, z, r) => Math.min(H(x, z), H(x - r, z), H(x + r, z), H(x, z - r), H(x, z + r));   // lowest ground under a footprint — nothing floats on slopes


