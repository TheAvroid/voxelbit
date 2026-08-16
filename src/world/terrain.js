  // @module - worldgen: heights, rivers, gorges and every stamped decoration - the source the gen worker is built from
  // @exports BCELL, CACCELL, CAVE_CELL, CAVE_FLOOR_MAX, CAVE_MARGIN, CAVE_WMAX, DRCELL, F2CELL, LGCELL, LGIGCELL, LILYCELL, MUCELL, OCELL, PCCELL, SCELL, SHCELL, SHRUB_ON, SPVIEW_D, SPVIEW_W, TCELL, TMARGIN, boulderAt, cactusAt, caveAt, caveHitsBox, drockAt, fern2At, fillColumn, genRegion, genRegionGen, lilyAt, lilyGigAt, logAt, mushAt, nearCave, oreAt, pconeAt, rebuildBricks, rebuildBricks2, rockRowSpan, shrubAt, stampBoulder, stampCactus, stampCave, stampCellsGen, stampDrock, stampFern2, stampLily, stampLilyGig, stampLog, stampModel, stampMush, stampOre, stampPcone, stampShrub, stampStick, stampTree, stickAt, sweepOrphans, treeAt, treesInRegion
  // ── deterministic world-coordinate generation ──────────────────────────────
  function fillColumn(wx, wz, fresh, h0, hxm, hxp, hzm, hzp, mossV) {   // terrain + lakes + twigs + grass; heights + moss fbm arrive precomputed from the row sweep
    const gx = gwrap(wx, WX), gz = gwrap(wz, WZ);
    const tI = (gx >> 3) + (gz >> 3) * BX;
    if (!touched[tI]) { fresh = true; touched[tI] = 1; }   // virgin memory is all zeros — no need to write the sky
    let h = h0;
    const lake = h <= WL - 1 ||                        // ≥ 2 voxels deep, OR a 1-deep rim column joined to real water — no isolated dither puddles
      (h === WL && (hxm < WL || hxp < WL || hzm < WL || hzp < WL));
    if (!lake && h <= WL) h = WL + 1;                  // dry land NEVER sits below the water plane — beach tops meet the surface FLUSH
    hmap[gx + gz * WX] = h;                            // hmap = the GROUND (lakebeds included — you walk on them, underwater)
    const mossy = !lake && mossV > 0.52;               // 0.56 → 0.52 ≈ +30% moss coverage
    const dm = desertM(wx, wz);                        // biome weight for this column: 0 = pine forest, 1 = open desert
    const base = gx + gz * WX * WY;
    let surfMoss = false;                              // grass only grows where a MOSS surface voxel actually landed
    const yTop = fresh ? (lake ? WL + 1 : h) : WY;     // fresh columns skip writing the empty sky — ~40% faster world builds
    // (the ROCK core — the hottest loop in worldgen, ~180 voxels/column — moved to rockRowSpan: writing it per COLUMN
    // strode WX bytes per voxel, a fresh cache line every write; the row-major pass writes contiguous x-runs instead.
    // rockTop was min(h-3, yTop), but yTop ≥ h in every branch, so h-3 exactly — rockRowSpan reproduces it bit-identically.)
    // ── SOIL ── 15 voxels deep under EVERY column (user), cut out of the rock core rockRowSpan already
    // wrote. This used to be 2 voxels everywhere plus a deep layer on grass only, which is exactly the
    // uneven depth the user saw: deep under the meadows, shallow on the beach and the forest floor.
    for (let y = Math.max(0, h - 16); y < Math.min(h - 1, yTop); y++) W[base + y * WX] = DIRT[(ihash(wx, y * 131 + wz) * 3) | 0];
    if (h - 1 >= 0 && h - 1 < yTop) {                  // the SURFACE voxel
      const sh = ihash(wx * 3 + 1, wz * 3 + 7);        // hoisted — was hashed up to twice
      const shore = h <= WL + 6;                       // any waterline — lakes AND rivers
      let c;
      if (dm > 0 && ihash(wx * 5 + 17, wz * 7 + 29) < dm) c = DSAND[(sh * 4) | 0];                       // ── DESERT ── dithered against the mask itself, so the sand thins out into the forest floor across the whole rim instead of ending on a line. Same trick as the sand-to-forest blend below, driven by the biome weight rather than by depth.
      else if (shore && h <= WL + 2) c = SAND[(sh * 3) | 0];                                             // waterline beach + sandy lakebed
      else if (shore && ihash(wx * 7 + 5, wz * 11 + 3) > (h - WL - 2) / 4.5) c = SAND[(sh * 3) | 0];     // dithered sand-to-forest blend
      // ── NO FOREST FLOOR ON THE DESERT SIDE, BUT ONLY NEAR WATER ── the DESERT branch above is dithered
      // against the mask, so a fraction (1 - dm) of columns miss it by design; inland those misses are the
      // gradient, which is the whole point. Near a WATERLINE they were landing on moss instead, and clustered
      // into the green patch on the lake's desert shore. This catches exactly that case. An earlier version
      // of this fix dropped the `shore` test and sent EVERY dm > 0.5 column to sand — which cured the lake and
      // replaced the gradient with a hard edge running down the biome line (user: "can you blend this biome
      // transtion line better"). Keeping `shore` is what lets the dither go on doing its job on dry land.
      else if (shore && dm > 0.5) c = SAND[(sh * 3) | 0];
      else { c = (mossy ? MOSS : NEEDLE)[(sh * 4) | 0]; surfMoss = mossy; }
      W[base + (h - 1) * WX] = c;
    }
    if (lake) for (let y = h; y <= Math.min(WL, yTop - 1); y++) W[base + y * WX] = y === WL ? WATER_T : WATER_B;
    if (!fresh) { let ia = base + (lake ? WL + 1 : h) * WX; for (let y = lake ? WL + 1 : h; y < WY; y++) { W[ia] = 0; ia += WX; } }
    if (surfMoss && h + 4 < WY) {                      // GRASS STRANDS: moss patches ONLY, 1–4 voxels tall, moss-matched colors
      if (ihash(wx * 3 + 41, wz * 3 + 87) < 0.06) {
        const gh = 1 + ((ihash(wx * 5 + 3, wz * 7 + 9) * 4) | 0);
        const gc = GRASS[(ihash(wx + 13, wz * 13) * 4) | 0];
        for (let k = 0; k < gh; k++) { const ii = base + (h + k) * WX; if (W[ii]) break; W[ii] = gc; }
      }
    }
    if (!lake && h > WL + 4 && h + 3 < WY && dm < 0.5) {   // FLOWERS: everywhere except beaches/water — and not in the desert (grass needs no gate: it only grows where a MOSS surface landed, which the sand branch never sets)
      if (ihash(wx * 9 + 71, wz * 5 + 29) < 0.005) {   // uniform spread — no clustering
        const s0 = base + h * WX;
        if (!W[s0] && !W[s0 + WX]) {
          W[s0] = GRASS[(ihash(wx + 13, wz * 13) * 4) | 0];
          W[s0 + WX] = BLOOM[(ihash(wx * 17 + 3, wz * 23 + 11) * 6) | 0];
        }
      }
    }
  }
  function stampModel(m, rot, wx, wy, wz, x0, x1, z0, z1, mode) {   // sparse decoration model stamp, rotated 0-3 about vertical, anchored bottom-center,
    const fw = (rot & 1) ? m.sy : m.sx, fd = (rot & 1) ? m.sx : m.sy;   // clipped to the region. mode: 0 = empty cells only; 1 = empty + soft decor;
    const bx = wx - (fw >> 1), bz = wz - (fd >> 1);                     // 2 = OVERWRITE + raise hmap (solid rocks); 3 = empty/water (seafloor pebbles);
    for (let i = 0; i < m.vox.length; i++) {                            // 4 = empty cells whose column floats on WATER_T (lilypads)
      const p = m.vox[i];
      const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
      let rx, rz;
      if (rot === 0) { rx = x; rz = y; }
      else if (rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      const ax = bx + rx, az = bz + rz;
      if (ax < x0 || ax >= x1 || az < z0 || az >= z1) continue;
      const ay = wy + z; if (ay < 1 || ay >= WY) continue;
      const gx = gwrap(ax, WX), gz = gwrap(az, WZ);
      const ii = gx + ay * WX + gz * WX * WY;
      const cur = W[ii];
      if (mode === 0 && cur !== 0) continue;
      if (mode === 1 && cur !== 0 && cur < DECOR_MIN) continue;
      if (mode === 3 && cur !== 0 && cur !== WATER_T && cur !== WATER_B) continue;
      if (mode === 4 && (cur !== 0 || W[gx + (wy - 1) * WX + gz * WX * WY] !== WATER_T)) continue;
      W[ii] = p >>> 24;
      if (mode === 2 && ay >= hmap[gx + gz * WX]) hmap[gx + gz * WX] = ay + 1;   // …and see supAnchored: raising hmap over a stamped body is exactly why that column's hmap can no longer be trusted as a proof of solid ground
    }
  }
  const BCELL = 34;                                    // one rock candidate per 3.4 m cell (2× the 48-cell density, 4× the original)
  function boulderAt(cx, cz) {                         // FOUR tiers, all real voxelized models now: rock.vox stone / small / mid / BIG rocks26 —
    if (ihash(cx * 29 + 7, cz * 31 + 3) > 0.5) return null;   // the much larger rocks are much rarer
    const bx = Math.round(cx * BCELL + 4 + ihash(cx * 3 + 40, cz * 7 + 90) * (BCELL - 8));
    const bz = Math.round(cz * BCELL + 4 + ihash(cx * 13 + 6, cz * 5 + 44) * (BCELL - 8));
    // ── THE HAND STONE IS ALLOWED IN THE DESERT, THE BOULDERS ARE NOT (user 2026-08-16: "scatter the rock.vox
    // around the desert") ── this used to reject all four tiers on one line. It now reads the tier FIRST and
    // lets only tier 0 through, which is rock.vox itself: the small pickable field stone. The three boulder
    // tiers stay excluded, because "remove the rocks from the desert" was about those, and the desert has its
    // own boulder pass (drockAt) with its own stock of rocks26 models and its own rate.
    const inDesert = desertM(bx, bz) > 0.5;
    if (nearCave(bx, bz)) return null;                 // rocks DO grow underwater — scattered seafloor boulders
    const sr = ihash(cx * 17 + 9, cz * 19 + 2);
    if (sr >= 0.275 && sr < 0.55) return null;         // the dropped half of the old 55% stone share — field stone rate halved
    const size = sr < 0.275 ? 0 : (sr < 0.93 ? 1 : (sr < 0.985 ? 2 : 3));   // stone 27.5% / small 38% / mid 5.5% / big 1.5%
    if (inDesert && size !== 0) return null;           // boulders: forest only. Tier 0 falls through and scatters on the sand.
    // ── AND HALF AS MANY OF THEM ON THE SAND (user 2026-08-16: "decrease the rock.vox by 50%") ── gated on
    // inDesert, because this tier is ALSO the pine forest's field stone and the user is looking at the desert.
    // A separate salt so it thins independently of the tier roll above rather than re-cutting the same order.
    if (inDesert && ihash(cx * 53 + 31, cz * 59 + 13) > 0.375) return null;   // another -25% (user 2026-08-16): 0.5 -> 0.375, so a quarter of the original desert hand stones remain
    const rot = (ihash(cx * 23 + 2, cz * 7 + 11) * 3.99) | 0;
    // Boulders stamp AFTER the cave carve, off the PRISTINE height, so one overhanging the mouth pit hangs in the
    // air. The old guard tested the rock's CENTRE against a flat 52, but the pit measures 42-52 across and the widest
    // model is 74 — a big rock centred just outside 52 still had its near edge ~37 voxels inside the hole. Grow the
    // exclusion by the rock's own half-width so the whole footprint clears the pit, and only the big ones pay for it.
    // ── NO ROCK GROWS THROUGH A PINE (user 2026-08-07: "dont spawn rocks into trees. the tree clips into
    // rocks") ── this rejection existed but only mid and BIG rocks paid it, and a "small" rocks26 model is
    // 778-1869 voxels — easily wide enough to swallow a bole. Field stones are small but still stamp solid
    // stone through a trunk's base. One helper now, used by every tier, with the margin scaled to the model
    // actually being placed rather than to its tier.
    const treeClash = (mw, mh, pad) => {
      const halfw = (Math.max(mw, mh) >> 1) + pad;
      for (let cz2 = Math.floor((bz - halfw) / TCELL); cz2 <= Math.floor((bz + halfw) / TCELL); cz2++)
        for (let cx2 = Math.floor((bx - halfw) / TCELL); cx2 <= Math.floor((bx + halfw) / TCELL); cx2++) {
          const t = treeAt(cx2, cz2); if (!t) continue;
          const dx = bx - t.tx, dz = bz - t.tz;
          if (dx * dx + dz * dz < halfw * halfw) return true;
        }
      return false;
    };
    if (size === 0) return (ROCKV && !treeClash(ROCKV.sx, ROCKV.sy, 3))
      ? { bx, bz, size, rot, mi: 0, up: ihash(cx * 43 + 12, cz * 47 + 21) < 0.5 ? 1 : 0 } : null;   // half stand upright
    const list = size === 1 ? R26S : (size === 2 ? R26M : R26B);
    if (!list.length) return null;
    const mi = list[(ihash(cx * 37 + 5, cz * 41 + 8) * list.length) | 0];
    const m = ROCK26[mi];
    if (treeClash(m.sx, m.sy, 4)) return null;         // …and every rocks26 tier, not just mid/big
    if (size >= 2) {                                   // mid/big rocks are TREE-sized — reject candidates that would swallow a pine (same probe ferns use)
      if (treeClash(m.sx, m.sy, 8)) return null;       // …and mid/big keep their WIDER berth on top of the shared one
      const sink = 1 + ((m.sz * 0.12) | 0) + ((ihash(bx * 3 + 1, bz * 5 + 2) * 3) | 0);   // EXACT copy of stampBoulder's base math —
      const gy = rockSeatY(m, bx, bz) - sink;       // the poke-through test must see the same gy the stamp will use
      const rr3 = Math.round((Math.max(m.sx, m.sy) >> 1) * 0.7);                          // ground rising THROUGH the rock top = the "stone cube" tower with a dirt cap — reject the site
      if (H(bx, bz) > gy + m.sz - 3) return null;
      for (let a = 0; a < 8; a++) {
        if (H(bx + Math.round(Math.cos(a * 0.785) * rr3), bz + Math.round(Math.sin(a * 0.785) * rr3)) > gy + m.sz - 3) return null;
      }
    }
    return { bx, bz, size, rot, mi };
  }
  // GENERATION-TIME ORPHANS. gpuPatch is the funnel for every RUNTIME mutation, which is why the support
  // resolver never hears about anything the GENERATOR carves. Measured 2026-08-07: 8 of 8 gorge sites carried
  // small detached stone fragments (2-10 voxels) hanging in the walls, permanent, because nothing ever asks.
  // This is the generator's own supPush: after a slab is built, anything not 26-connected to bedrock goes.
  // Runs in the WORKER on its private slab, so the main thread (already the generation bottleneck) pays nothing.
  // Conservative in three ways: only slabs a GORGE touches are swept (the only generator that carves through
  // solid strata); only components made ENTIRELY of hard terrain are deleted, so a canopy whose needles read as
  // detached in the model is never touched; and a component reaching the slab wall is left alone, because it
  // may well continue into the neighbouring slab.
  const ORPH_SCRATCH = { mark: null, stk: null };   // reused: the tile queue calls this every frame
  function sweepOrphans(x0, x1, z0, z1) {
    let near = false;
    const c0x = Math.floor((x0 - CAVE_MARGIN) / CAVE_CELL), c1x = Math.floor((x1 + CAVE_MARGIN) / CAVE_CELL);
    const c0z = Math.floor((z0 - CAVE_MARGIN) / CAVE_CELL), c1z = Math.floor((z1 + CAVE_MARGIN) / CAVE_CELL);
    for (let cz = c0z; cz <= c1z && !near; cz++) for (let cx = c0x; cx <= c1x; cx++) {   // ── THE GATE IS THE WHOLE COST OF THIS FUNCTION ──
      const c = caveAt(cx, cz); if (c && caveHitsBox(c, x0, x1, z0, z1)) { near = true; break; }   // ...so it has to be EXACT, not merely conservative
    }
    if (!near) return null;                          // null = no gorge in this slab, distinct from 'swept and found nothing'
    const sx = x1 - x0, sz = z1 - z0, n = sx * WY * sz;
    const li = (ix, iy, iz) => ix + iy * sx + iz * sx * WY;
    const wi = (ix, iy, iz) => gwrap(x0 + ix, WX) + iy * WX + gwrap(z0 + iz, WZ) * WX * WY;   // gwrap makes this correct in BOTH contexts: identity-minus-offset in the worker slab, toroidal on the main-thread window
    if (!ORPH_SCRATCH.mark || ORPH_SCRATCH.mark.length < n) { ORPH_SCRATCH.mark = new Uint8Array(n); ORPH_SCRATCH.stk = new Int32Array(n); }
    const mark = ORPH_SCRATCH.mark, stk = ORPH_SCRATCH.stk;
    mark.fill(0, 0, n);
    let sp = 0;
    for (let iz = 0; iz < sz; iz++) for (let iy = 0; iy < WY; iy++) for (let ix = 0; ix < sx; ix++) {
      const v = W[wi(ix, iy, iz)]; if (!v) continue;
      const k = li(ix, iy, iz);
      if (iy <= 1 || v === WATER_T || v === WATER_B) { mark[k] = 2; stk[sp++] = k; }
      else mark[k] = 1;
    }
    while (sp > 0) {
      const k = stk[--sp];
      const ix = k % sx, iy = ((k / sx) | 0) % WY, iz = (k / (sx * WY)) | 0;
      for (let d = 0; d < 27; d++) {
        const ax = ix + (d % 3) - 1, ay = iy + (((d / 3) | 0) % 3) - 1, az = iz + ((d / 9) | 0) - 1;
        if (ax < 0 || ay < 0 || az < 0 || ax >= sx || ay >= WY || az >= sz) continue;
        const q = li(ax, ay, az); if (mark[q] === 1) { mark[q] = 2; stk[sp++] = q; }
      }
    }
    let cut = 0; const seeds = [], gone = [];
    for (let k0 = 0; k0 < n; k0++) {
      if (mark[k0] !== 1) continue;
      let sp2 = 0, wall = false, hard = true;
      const comp = [];
      stk[sp2++] = k0; mark[k0] = 3;
      while (sp2 > 0) {
        const k = stk[--sp2]; comp.push(k);
        const ix = k % sx, iy = ((k / sx) | 0) % WY, iz = (k / (sx * WY)) | 0;
        if (ix === 0 || iz === 0 || ix === sx - 1 || iz === sz - 1) wall = true;
        if (!ORPHAN_OK[W[wi(ix, iy, iz)]]) hard = false;
        for (let d = 0; d < 27; d++) {
          const ax = ix + (d % 3) - 1, ay = iy + (((d / 3) | 0) % 3) - 1, az = iz + ((d / 9) | 0) - 1;
          if (ax < 0 || ay < 0 || az < 0 || ax >= sx || ay >= WY || az >= sz) continue;
          const q = li(ax, ay, az); if (mark[q] === 1) { mark[q] = 3; stk[sp2++] = q; }
        }
      }
      if (!hard) continue;
      // A component that reaches the slab wall may continue into the neighbouring slab, so the worker must
      // not judge it. Hand ONE representative voxel to the main thread, where the support resolver sees the
      // stitched world and adjudicates it on its own per-frame budget - the same answer, just later.
      if (wall) { const k = comp[0], ix = k % sx, iy = ((k / sx) | 0) % WY, iz = (k / (sx * WY)) | 0;
        if (seeds.length < 512) seeds.push(x0 + ix, iy, z0 + iz); continue; }
      for (const k of comp) { const ix = k % sx, iy = ((k / sx) | 0) % WY, iz = (k / (sx * WY)) | 0;
        W[wi(ix, iy, iz)] = 0; if (gone.length < 60000) gone.push(x0 + ix, iy, z0 + iz); }
      cut += comp.length;
    }
    return { cut, seeds, gone };
  }
  function stampBoulder(b, x0, x1, z0, z1) {
    if (b.size === 0) { stampModel(b.up ? ROCKVU : ROCKV, b.rot, b.bx, H(b.bx, b.bz) - 1, b.bz, x0, x1, z0, z1, 3); return; }   // small stone: SUNK 1 vox into the ground (user — its base row lands in solid, mode-3 skips it, so it never floats), walk-through decor, pickable, sits on the seafloor too; half stand on edge
    const m = ROCK26[b.mi];
    const sink = 1 + ((m.sz * 0.12) | 0) + ((ihash(b.bx * 3 + 1, b.bz * 5 + 2) * 3) | 0);   // bigger rocks sit deeper — no floating edges on bumpy ground
    const gy = rockSeatY(m, b.bx, b.bz) - sink;
    stampModel(m, b.rot, b.bx, gy, b.bz, x0, x1, z0, z1, 2);   // overwrite + hmap raise — a REAL rock you collide with
  }
  const CAVE_CELL = 640, CAVE_MARGIN = 900;            // RAVINES -> CAVES: sparser, VAST gorges (1/4 the old frequency)
  const CAVE_WMAX = 52;                                // the half-extent stampCave carves around its axis. Shared with caveHitsBox: if these two ever disagree the orphan sweep starts skipping real gorges.
  const CAVE_FLOOR_MAX = 6;                            // the largest value stampCave's floorY can take (2 + round(fbm*4), fbm in 0..1) — and floorY is EXACTLY what it drops a carved column's hmap to, so hmap <= this is the signature of a gorge tile. Read by both copies of the brick sky-cap (rebuildBricks here, the pool worker's own scan in gen-pool.js); if floorY's formula changes, this must too.
  const caveCache = new Map();
  function caveAt(cx, cz) {
    const key = cx * 100003 + cz;
    let c = caveCache.get(key);
    if (c !== undefined) return c;
    c = null;
    const myRoll = ihash(cx * 91 + 31, cz * 97 + 57);                // base roll 0.22 → ~9.9% EFFECTIVE after the adjacency cull below (user wants ~10% effective WITH no adjacency; 0.22 was tuned by simulation), then the dryness test culls any whose path touches water/basins
    let win = myRoll <= 0.22;
    for (let dz = -1; dz <= 1 && win; dz++) for (let dx = -1; dx <= 1; dx++) {   // NO TWO RAVINES ADJACENT (user): of the 8-neighbour cells that ALSO rolled, only the SMALLEST roll survives → a ravine never touches another. Uses the raw roll only (not caveAt) so it stays non-recursive; ties break on cell index.
      if (!dx && !dz) continue;
      const nr = ihash((cx + dx) * 91 + 31, (cz + dz) * 97 + 57);
      if (nr <= 0.22 && (nr < myRoll || (nr === myRoll && (cx + dx) * 100003 + (cz + dz) < cx * 100003 + cz))) { win = false; break; }
    }
    if (win) {
      const sx = Math.round(cx * CAVE_CELL + 60 + ihash(cx * 3 + 12, cz * 7 + 44) * (CAVE_CELL - 120));
      const sz = Math.round(cz * CAVE_CELL + 60 + ihash(cx * 9 + 71, cz * 5 + 13) * (CAVE_CELL - 120));
      const ang = ihash(cx + 21, cz + 63) * 6.283, len = 450 + ihash(cx * 23, cz * 29) * 350;
      const wb = 18 + ihash(cx * 27 + 4, cz * 31 + 9) * 16;
      const nx2 = -Math.sin(ang), nz2 = Math.cos(ang);
      let dry = true;                                  // the WHOLE path must be dry — centre AND both rims, so a gorge never runs beside a river either
      for (let q = 0; q <= 8 && dry; q++) {
        const px2 = sx + Math.cos(ang) * (len * q / 8), pz2 = sz + Math.sin(ang) * (len * q / 8);
        for (let s = -1; s <= 1; s++) {
          const lx = Math.round(px2 + nx2 * s * (wb * 1.4 + 34)), lz = Math.round(pz2 + nz2 * s * (wb * 1.4 + 34));
          if (H(lx, lz) <= WL + 4 || basinM(lx, lz) > 0.1 || desertM(lx, lz) > 0) { dry = false; break; }   // …and NOT INTO THE DESERT (user). Tested here, on the same 9-point path walk with the same rim offsets, because the test has to reject the WHOLE gorge: gating the carve instead would leave a canyon that stops dead at the sand line. desertM > 0 (not > 0.5) so a gorge cannot even reach the blend band.
        }
      }
      if (dry) c = { sx, sz, ang, len, wb, dep: 280, seed: cx * 733 + cz * 911 };
    }
    caveCache.set(key, c);
    return c;
  }
  function stampCave(c, x0, x1, z0, z1) {
    const dxr = Math.cos(c.ang), dzr = Math.sin(c.ang);
    const nx = -dzr, nz = dxr;
    const ex2 = c.sx + dxr * c.len, ez2 = c.sz + dzr * c.len;
    const WMAX = CAVE_WMAX;                             // vast gorge: pointy ends, near-vertical walls, noisy bedrock floor
    const bx0 = Math.max(x0, Math.floor(Math.min(c.sx, ex2) - WMAX)), bx1 = Math.min(x1, Math.ceil(Math.max(c.sx, ex2) + WMAX) + 1);
    const bz0 = Math.max(z0, Math.floor(Math.min(c.sz, ez2) - WMAX)), bz1 = Math.min(z1, Math.ceil(Math.max(c.sz, ez2) + WMAX) + 1);
    for (let z = bz0; z < bz1; z++) for (let x = bx0; x < bx1; x++) {
      const t = Math.max(0, Math.min(c.len, (x - c.sx) * dxr + (z - c.sz) * dzr));
      const k = Math.sin(Math.PI * t / c.len);
      const off = Math.sin(t * 0.02 + c.seed) * 14 + Math.sin(t * 0.053 + c.seed * 1.7) * 5;   // long lazy snake
      const pd = (x - (c.sx + dxr * t)) * nx + (z - (c.sz + dzr * t)) * nz;
      const wEff = c.wb * Math.pow(k, 1.5) + Math.sin(t * 0.05 + c.seed * 2.3) * 1.5;          // pow taper → the gorge ENDS IN A POINT
      if (wEff < 0.8) continue;
      const d = Math.abs(pd - off);
      if (d > wEff + 4.5) continue;                    // generous cull — the per-band wall jag decides the real edge
      const surfY = H(x, z);
      if (surfY <= WL + 6 || basinM(x, z) > 0.15) continue;         // belt-and-braces column guard near any water
      const floorY = 2 + Math.round(fbm(x * 0.05 + 5.1, z * 0.05 + 9.7) * 4);   // noisy bedrock floor
      const gx = gwrap(x, WX), gz = gwrap(z, WZ);
      let carvedFloor = false;
      for (let y = Math.max(1, floorY); y <= Math.min(WY - 1, surfY + 6); y++) {   // JAGGED walls: the wall line wanders per 4-voxel band
        const band = y >> 2;
        const wallJ = (ihash(x * 3 + band * 151, z * 7 - band * 57) - 0.5) * 4.2
                    + Math.sin(y * 0.11 + x * 0.05 + z * 0.07) * 1.4;
        if (d > wEff + wallJ) continue;
        const ii = gx + y * WX + gz * WX * WY;
        if (W[ii] !== WATER_T && W[ii] !== WATER_B) { W[ii] = 0; if (y <= floorY + 1) carvedFloor = true; }
      }
      const ci = gx + gz * WX;
      if (d < wEff - 3) {
        if (hmap[ci] > floorY) hmap[ci] = floorY;
        const lt = fbm(x * 0.06 + 21.3, z * 0.06 + 8.7);            // LAVA pool — smooth red/orange/yellow patches that blend
        const top = lt < 0.40 ? LAVA_R : (lt < 0.62 ? LAVA_T : LAVA_Y);
        W[gx + Math.max(1, floorY) * WX + gz * WX * WY] = LAVA_B;
        W[gx + (Math.max(1, floorY) + 1) * WX + gz * WX * WY] = top;
      } else if (carvedFloor && hmap[ci] > floorY) hmap[ci] = floorY;
    }
    for (let t = 4; t < c.len; t += 5) {                // IRON + COAL nodules protruding from the gorge walls
      const k = Math.sin(Math.PI * t / c.len);
      const wEff = c.wb * Math.pow(k, 1.5);
      if (wEff < 3) continue;
      const off = Math.sin(t * 0.02 + c.seed) * 14 + Math.sin(t * 0.053 + c.seed * 1.7) * 5;
      for (let side = -1; side <= 1; side += 2) {
        const r0 = ihash(c.seed + t * 3, side * 11 + 5);
        if (r0 > 0.55) continue;
        const wx2 = Math.round(c.sx + dxr * t + nx * (off + side * (wEff - 1)));
        const wz2 = Math.round(c.sz + dzr * t + nz * (off + side * (wEff - 1)));
        if (wx2 < x0 - 4 || wx2 >= x1 + 4 || wz2 < z0 - 4 || wz2 >= z1 + 4) continue;   // pure reach cull — the per-voxel clip below decides, so region seams can't drop a nodule
        const sy = H(wx2, wz2);
        const oy = Math.max(8, Math.round(sy - 6 - r0 * (c.dep > sy ? sy - 12 : c.dep - 8)));
        const ORE = ihash(c.seed + t, side) < 0.5 ? OREIRON : ORECOAL;
        const br = 1.2 + ihash(t, c.seed + side) * 1.1;
        const BR = Math.ceil(br);
        for (let dz2 = -BR; dz2 <= BR; dz2++) for (let dy2 = -BR; dy2 <= BR; dy2++) for (let dx2 = -BR; dx2 <= BR; dx2++) {
          if (dx2 * dx2 + dy2 * dy2 + dz2 * dz2 > br * br) continue;
          const yy = oy + dy2; if (yy < 5 || yy >= WY) continue;
          const xx = wx2 + dx2, zz = wz2 + dz2;
          if (xx < x0 || xx >= x1 || zz < z0 || zz >= z1) continue;
          const jj = gwrap(xx, WX) + yy * WX + gwrap(zz, WZ) * WX * WY;
          if (W[jj] !== WATER_T && W[jj] !== WATER_B) W[jj] = ORE[(ihash(xx + yy, zz - yy) * 2) | 0];
        }
      }
    }
  }
  // Would stampCave write ANYTHING inside this box? Its carve loop and its ore-nodule loop are both bounded by
  // the segment AABB grown by CAVE_WMAX, so an AABB miss means the gorge cannot have touched the box. Used to
  // gate the orphan sweep, whose old gate asked the far weaker question 'is a gorge CELL within CAVE_MARGIN'
  // -- a 900-voxel skirt around a 640-voxel cell grid, which ~90% of slabs passed. See sweepOrphans.
  function caveHitsBox(c, x0, x1, z0, z1) {
    const ex = c.sx + Math.cos(c.ang) * c.len, ez = c.sz + Math.sin(c.ang) * c.len;
    return Math.min(c.sx, ex) - CAVE_WMAX < x1 && Math.max(c.sx, ex) + CAVE_WMAX + 1 > x0
        && Math.min(c.sz, ez) - CAVE_WMAX < z1 && Math.max(c.sz, ez) + CAVE_WMAX + 1 > z0;
  }
  function nearCave(x, z, r = 56) {                    // surface decor keeps off the ravine line (r widens for big features like domes)
    for (let cz = Math.floor((z - CAVE_MARGIN) / CAVE_CELL); cz <= Math.floor((z + CAVE_MARGIN) / CAVE_CELL); cz++)
      for (let cx = Math.floor((x - CAVE_MARGIN) / CAVE_CELL); cx <= Math.floor((x + CAVE_MARGIN) / CAVE_CELL); cx++) {
        const c = caveAt(cx, cz); if (!c) continue;
        const dxr = Math.cos(c.ang), dzr = Math.sin(c.ang);
        const t = Math.max(0, Math.min(c.len, (x - c.sx) * dxr + (z - c.sz) * dzr));
        const ddx = x - (c.sx + dxr * t), ddz = z - (c.sz + dzr * t);
        if (ddx * ddx + ddz * ddz < r * r) return true;
      }
    return false;
  }
  const OCELL = 40;                                    // MINERAL veins: small blobs in the deep stone, type by depth
  function oreAt(cx, cz) {
    if (ihash(cx * 101 + 9, cz * 103 + 27) > 0.55) return null;
    const x = Math.round(cx * OCELL + 4 + ihash(cx * 7 + 2, cz * 3 + 6) * (OCELL - 8));
    const z = Math.round(cz * OCELL + 4 + ihash(cx * 5 + 9, cz * 11 + 1) * (OCELL - 8));
    const df = ihash(cx * 13 + 4, cz * 17 + 8);
    const tr = ihash(cx * 19 + 6, cz * 23 + 2);
    const type = tr < 0.5 ? 0 : 1;                     // ONLY coal (0) + iron (1) — gold + crystal REMOVED (user); ~50/50, clustered by the rr blob below
    return { x, z, df, type, rr: 1.8 + ihash(cx + 3, cz + 5) * 1.7 };   // bigger blobs → clearer clusters in the cave walls
  }
  function stampOre(o, x0, x1, z0, z1) {
    const y0 = Math.round(8 + o.df * Math.max(4, H(o.x, o.z) - 20));
    const ORES = [ORECOAL, OREIRON, OREGOLD, ORECRYS];
    const R = Math.ceil(o.rr);
    for (let dz = -R; dz <= R; dz++) { const z = o.z + dz; if (z < z0 || z >= z1) continue;
      for (let dx = -R; dx <= R; dx++) { const x = o.x + dx; if (x < x0 || x >= x1) continue;
        for (let dy = -R; dy <= R; dy++) {
          if (dx * dx + dy * dy + dz * dz > o.rr * o.rr) continue;
          const y = y0 + dy; if (y < 5 || y >= WY) continue;
          const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
          const v = W[ii];
          if (v === ROCK[0] || v === ROCK[1] || v === ROCK[2]) W[ii] = ORES[o.type][(ihash(x + y, z - y) * 2) | 0];
        } } }
  }
  const F2CELL = 30;                                   // FERNS: the single ~1.1 m fern plant from fern.glb (the ferns_grass clumps were removed)
  function fern2At(cx, cz) {
    if (!FERN2V.length) return null;
    if (ihash(cx * 79 + 7, cz * 83 + 31) > 0.07) return null;   // halved 2026-07-16 (was 0.14)
    const wx = Math.round(cx * F2CELL + 3 + ihash(cx * 5 + 27, cz * 3 + 41) * (F2CELL - 6));
    const wz = Math.round(cz * F2CELL + 3 + ihash(cx * 11 + 33, cz * 7 + 15) * (F2CELL - 6));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── ferns are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (H(wx, wz) <= WL + 4) return null;              // no ferns in water or on beaches
    if (nearCave(wx, wz)) return null;
    for (let cz2 = Math.floor((wz - 14) / TCELL); cz2 <= Math.floor((wz + 14) / TCELL); cz2++)   // keep clear of pine trunks — the plant is 23 wide
      for (let cx2 = Math.floor((wx - 14) / TCELL); cx2 <= Math.floor((wx + 14) / TCELL); cx2++) {
        const t = treeAt(cx2, cz2); if (!t) continue;
        const dx = wx - t.tx, dz = wz - t.tz;
        if (dx * dx + dz * dz < 14 * 14) return null;
      }
    for (let bz2 = Math.floor((wz - 50) / BCELL); bz2 <= Math.floor((wz + 50) / BCELL); bz2++)   // KEEP CLEAR OF ROCKS — ferns clipped into boulders (user); boulderAt is deterministic so probe the same candidates it places
      for (let bx2 = Math.floor((wx - 50) / BCELL); bx2 <= Math.floor((wx + 50) / BCELL); bx2++) {
        const b = boulderAt(bx2, bz2); if (!b) continue;
        const rm = b.size === 0 ? (ROCKV ? ROCKV : null) : ROCK26[b.mi];
        const rhalf = rm ? (Math.max(rm.sx, rm.sy) >> 1) : 4;
        const clr = rhalf + 12;                        // rock half-width + fern half-width (23 wide → 11.5) + a small gap → footprints never touch
        const dx = wx - b.bx, dz = wz - b.bz;
        if (dx * dx + dz * dz < clr * clr) return null;
      }
    return { wx, wz, mi: (ihash(cx * 23 + 8, cz * 29 + 19) * FERN2V.length) | 0, rot: (ihash(cx * 2 + 9, cz * 2 + 5) * 3.99) | 0 };
  }
  function stampFern2(f, x0, x1, z0, z1) {
    stampModel(FERN2V[f.mi], f.rot, f.wx, H(f.wx, f.wz), f.wz, x0, x1, z0, z1, 1);
  }
  // ── CACTI ── the desert's only scatter. One candidate per 9.6 m cell; the mask test is >= 0.85 rather
  // than the > 0.5 the forest litter uses, because that gate is a REJECT (keep pines out of the sand) and this
  // one is an ADMIT: a cactus standing in the half-and-half rim would be a cactus in the treeline. Holding it
  // to the open desert keeps the border reading as a border.
  const CACCELL = 96;
  function cactusAt(cx, cz) {
    if (!CACTI.length) return null;
    if (ihash(cx * 53 + 11, cz * 59 + 23) > 0.24) return null;   // -20% (user 2026-08-15), was 0.30
    const wx = Math.round(cx * CACCELL + 8 + ihash(cx * 7 + 31, cz * 3 + 17) * (CACCELL - 16));
    const wz = Math.round(cz * CACCELL + 8 + ihash(cx * 13 + 5, cz * 11 + 29) * (CACCELL - 16));
    if (desertM(wx, wz) < 0.85) return null;
    if (H(wx, wz) <= WL + 4) return null;              // never in water, never on a beach
    if (nearCave(wx, wz)) return null;
    const dxs = wx - SPWX, dzs = wz - SPWZ;
    if (dxs * dxs + dzs * dzs < 26 * 26) return null;  // the spawn clearing, same radius the pines respect
    return { wx, wz, mi: (ihash(cx * 3 + 7, cz * 5 + 2) * CACTI.length) | 0, rot: (ihash(cx + 61, cz + 37) * 3.99) | 0 };
  }
  function stampCactus(c, x0, x1, z0, z1) {
    stampModel(CACTI[c.mi], c.rot, c.wx, H(c.wx, c.wz) - 3, c.wz, x0, x1, z0, z1, 1);   // THREE voxels into the sand (user 2026-08-16). One was not enough: H is the column's own height, so on a dune's slope the far side of a wide base still hung in the air
  }
  // ── DESERT ROCKS ── the desert's second scatter. Two tiers off the .glb's own naming: the small ones
  // (1.4-3.3 m) carry the field, the mid ones (5-11 m) are rare landmarks. Same ADMIT test as the cacti
  // (>= 0.85) so none of them stand in the treeline, and the same spawn clearing the pines respect.
  const DRCELL = 112;
  function drockAt(cx, cz) {
    if (!ROCK26.length) return null;
    const roll = ihash(cx * 41 + 17, cz * 47 + 29);
    // -25% again (user 2026-08-16), 0.225 -> 0.169; it was 0.30 before the first cut on 2026-08-15. ihash is
    // uniform on 0..1, so the threshold IS the acceptance rate and the arithmetic is the verification — a
    // 400-voxel in-game scan cannot resolve a quarter either way, which is a lesson the shrubs already taught.
    // 0.251 (user 2026-08-16: "increase the rate of the runic and small rocks by 25%"). The tier cuts below
    // move with it so that ONLY the small/runic pool grows: big and mid keep the same ABSOLUTE count they had
    // at 0.211, and the extra acceptance all lands in the small pool. Was 0.169, and 0.225 before that.
    if (roll > 0.251) return null;
    const wx = Math.round(cx * DRCELL + 40 + ihash(cx * 5 + 13, cz * 7 + 41) * (DRCELL - 80));
    const wz = Math.round(cz * DRCELL + 40 + ihash(cx * 11 + 23, cz * 3 + 7) * (DRCELL - 80));
    if (desertM(wx, wz) < 0.85) return null;           // ADMIT test, not a reject: a rock in the blend band is a rock in the treeline
    if (H(wx, wz) <= WL + 4) return null;
    if (nearCave(wx, wz)) return null;
    const dxs = wx - SPWX, dzs = wz - SPWZ;
    if (dxs * dxs + dzs * dzs < 26 * 26) return null;  // the spawn clearing the pines respect
    // Same three tiers rocks26 ships, weighted so the desert reads as scattered stone with the odd landmark
    // rather than a boulder field: big 4%, mid 20%, the small/runic pool the rest.
    const t = ihash(cx * 19 + 7, cz * 23 + 5);
    // big 3.36% / mid 16.81% / small+runic the rest — rebalanced from 4%/20%/76% when the accept rate went
    // 0.211 -> 0.251, chosen so 0.251 x 0.0336 and 0.251 x 0.1681 reproduce the old 0.211 x 0.04 and x 0.20.
    const pool = (t < 0.0336 && R26B.length) ? R26B : ((t < 0.2017 && R26M.length) ? R26M : (R26S.length ? R26S : R26M));
    if (!pool.length) return null;
    const mi = pool[(ihash(cx * 3 + 29, cz * 5 + 61) * pool.length) | 0];
    // ── NEVER ON TOP OF A CACTUS (user 2026-08-15) ── the rock pass runs AFTER the cactus pass and stamps in
    // mode 2, which OVERWRITES, so an overlap does not interleave the two models: it buries the plant and the
    // player sees a rock where a saguaro was. Cleared by the two half-extents summed, the same shape fern2At
    // uses to keep the big fern off pine trunks. cactusAt is deterministic, so asking it here is free of any
    // ordering assumption — it does not matter which pass has already run.
    const rm = ROCK26[mi];
    if (rm) { const clr = Math.max(rm.sx, rm.sy) * 0.5 + 14;   // 14 = the widest cactus half-extent (25 across)
      for (let cz2 = Math.floor((wz - clr) / CACCELL); cz2 <= Math.floor((wz + clr) / CACCELL); cz2++)
        for (let cx2 = Math.floor((wx - clr) / CACCELL); cx2 <= Math.floor((wx + clr) / CACCELL); cx2++) {
          const c9 = cactusAt(cx2, cz2); if (!c9) continue;
          const dx9 = wx - c9.wx, dz9 = wz - c9.wz;
          if (dx9 * dx9 + dz9 * dz9 < clr * clr) return null;
        } }
    return { wx, wz, mi, rot: (ihash(cx + 71, cz + 13) * 3.99) | 0 };
  }
  function stampDrock(r, x0, x1, z0, z1) {
    stampModel(ROCK26[r.mi], r.rot, r.wx, H(r.wx, r.wz) - 5, r.wz, x0, x1, z0, z1, 2);   // FIVE voxels in (user 2026-08-16) — a boulder is wider than a cactus, so it needs to bury more of itself before its underside is flush on sloping sand   // mode 2 = OVERWRITE + raise hmap, the mode the forest boulders use: you walk over a rock, not through it
  }
  // ── DESERT SHRUBS ── the desert's third scatter: 6 GREEN scrub plants, 0.6-1.3 m, four of them FLOWERING,
  // walk-through decor. (user 2026-08-15: "the shrubs are not showing up. implement them on the desert";
  // re-baked green on the cactus's palette 2026-08-16; replaced by the user's own hand-authored 1.vox…6.vox
  // the same day — "reload the shrubs in, I made modifications and renamed the files". The scatter itself is
  // unchanged by that: it asks SHRUBV for a count and a footprint and never for a name or a shade.)
  // DESERT ONLY, on the SAME >= 0.85 ADMIT test the cacti and the desert rocks use — not the > 0.5 REJECT the
  // pine-forest litter passes use. The difference matters at the border: an admit test keeps every desert plant
  // out of the dithered treeline, so the biome boundary still reads as a boundary. In the pine forest this
  // returns null on its first line and shrubs are exactly zero.
  // DENSITY: one candidate per 4.8 m cell, 28% kept -> one shrub per ~82 m² before the clearance rejects below,
  // about one every 9 m. Deliberately several times denser than the cacti (one per 384 m²) and the desert rocks
  // (one per 557 m²), because scrub is what a desert floor is mostly made of — present in every view, nowhere
  // a carpet. Measure the real figure with __vb.shrubCount(x, z).
  const SHCELL = 48;
  const SHRUB_ON = true;                               // desert shrubs: ON. They were switched off 2026-08-16 ("remove the brown shrubs from the desert") and back on the same day once they were re-baked GREEN on the cactus ramp — the colour was the objection, not the plants. One named switch, both ways. Note this flag was ALREADY true while no shrub existed anywhere in the world: the loader was asking for the old shrub_N.vox names, SHRUBV came back empty, and shrubAt returns null on its first line — so an empty desert is a LOADER symptom first and a flag symptom second.
  function shrubAt(cx, cz) {
    if (!SHRUBV.length) return null;
    // 0.21, down from 0.28 (user 2026-08-16: "decrease the amount of shrubs by 25%"). ihash is uniform on
    // 0..1, so the threshold IS the acceptance rate and 0.28 * 0.75 = 0.21 is exactly a quarter fewer
    // candidate cells. The downstream rejections (cactus/rock clearance, water, the spawn clearing) scale
    // with it, so the planted count falls by the same quarter rather than by some other amount.
    if (ihash(cx * 67 + 23, cz * 71 + 11) > 0.158) return null;   // -25% again (user 2026-08-16): 0.21 -> 0.158, from 0.28 originally
    const wx = Math.round(cx * SHCELL + 4 + ihash(cx * 7 + 19, cz * 5 + 37) * (SHCELL - 8));
    const wz = Math.round(cz * SHCELL + 4 + ihash(cx * 13 + 31, cz * 11 + 3) * (SHCELL - 8));
    if (desertM(wx, wz) < 0.85) return null;
    if (H(wx, wz) <= WL + 4) return null;              // never in the water, and not on the beach ring either
    if (nearCave(wx, wz)) return null;
    const dxs = wx - SPWX, dzs = wz - SPWZ;
    if (dxs * dxs + dzs * dzs < 26 * 26) return null;  // the spawn clearing, same radius the pines respect
    const mi = (ihash(cx * 3 + 11, cz * 5 + 43) * SHRUBV.length) | 0;
    const m = SHRUBV[mi], half = Math.max(m.sx, m.sy) >> 1;   // this plant's OWN half-footprint — the set runs 5 to 16 wide, so one fixed radius would either float the big ones or over-space the small ones. Read off the MODEL, so a re-authored set changes the spacing and the seating with it and nothing here needs touching
    // ── NEVER INSIDE A CACTUS OR A DESERT ROCK ── both of those passes run AFTER this one in genRegionGen, and
    // the rocks stamp in mode 2 (OVERWRITE), so an overlap does not interleave two models: it buries the bush,
    // or leaves scrub growing out of a saguaro's ribs. cactusAt and drockAt are DETERMINISTIC, so probing the
    // candidates they will place is free of any ordering assumption — it does not matter which pass has run.
    // Same shape drockAt already uses to keep itself off the cacti: the two half-extents summed.
    { const clr = half + 14;                           // 14 = the widest cactus half-extent (25 across)
      for (let cz2 = Math.floor((wz - clr) / CACCELL); cz2 <= Math.floor((wz + clr) / CACCELL); cz2++)
        for (let cx2 = Math.floor((wx - clr) / CACCELL); cx2 <= Math.floor((wx + clr) / CACCELL); cx2++) {
          const c9 = cactusAt(cx2, cz2); if (!c9) continue;
          const dx9 = wx - c9.wx, dz9 = wz - c9.wz;
          if (dx9 * dx9 + dz9 * dz9 < clr * clr) return null;
        } }
    { const clr = half + 37;                           // 37 = the widest rocks26 model's half-extent (74 across)
      for (let bz2 = Math.floor((wz - clr) / DRCELL); bz2 <= Math.floor((wz + clr) / DRCELL); bz2++)
        for (let bx2 = Math.floor((wx - clr) / DRCELL); bx2 <= Math.floor((wx + clr) / DRCELL); bx2++) {
          const r9 = drockAt(bx2, bz2); if (!r9) continue;
          const rm = ROCK26[r9.mi], rc = half + (rm ? (Math.max(rm.sx, rm.sy) >> 1) : 8) + 2;   // that rock's OWN half-extent, so a 1.4 m stone does not clear a 4 m hole around itself
          const dx9 = wx - r9.wx, dz9 = wz - r9.wz;
          if (dx9 * dx9 + dz9 * dz9 < rc * rc) return null;
        } }
    return { wx, wz, mi, half, rot: (ihash(cx + 53, cz + 29) * 3.99) | 0 };
  }
  function stampShrub(r, x0, x1, z0, z1) {
    stampModel(SHRUBV[r.mi], r.rot, r.wx, groundMin(r.wx, r.wz, r.half) - 1, r.wz, x0, x1, z0, z1, 1);   // groundMin, not H: seated on the LOWEST ground under its OWN footprint, so a bush on dune relief sinks into the slope instead of standing on one corner with daylight under the rest. The extra -1 is the sink stampCactus takes for the same reason — groundMin samples five points, not the whole footprint, so a dip between them can still leave a gap. mode 1 keeps terrain winning every contested cell, so the buried courses simply do not draw.
  }
  const MUCELL = 52;                                   // MUSHROOMS: a rare cluster, ONLY in the pine forest (a pine must be within crown reach), one candidate per 5.2 m cell
  function mushAt(cx, cz) {
    if (!MUSHV) return null;
    if (ihash(cx * 89 + 17, cz * 97 + 5) > 0.053) return null;   // rare — ~5.3% of candidate cells before the pine-forest + rock-clearance gates below (cut by a third from 0.08, user)
    const wx = Math.round(cx * MUCELL + 5 + ihash(cx * 5 + 23, cz * 3 + 9) * (MUCELL - 10));
    const wz = Math.round(cz * MUCELL + 5 + ihash(cx * 11 + 7, cz * 7 + 19) * (MUCELL - 10));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── mushrooms are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (H(wx, wz) <= WL + 4) return null;              // no mushrooms in water or on beaches
    if (nearCave(wx, wz)) return null;
    let nearPine = false;                              // PINE-FOREST GATE: a pine within ~46 vox (crown reach) but ≥16 away so the 23-wide cluster never swallows a trunk
    for (let cz2 = Math.floor((wz - 46) / TCELL); cz2 <= Math.floor((wz + 46) / TCELL); cz2++)
      for (let cx2 = Math.floor((wx - 46) / TCELL); cx2 <= Math.floor((wx + 46) / TCELL); cx2++) {
        const t = treeAt(cx2, cz2); if (!t) continue;
        const dx = wx - t.tx, dz = wz - t.tz, d2 = dx * dx + dz * dz;
        if (d2 < 16 * 16) return null;                 // too close to a trunk — reject the whole site
        if (d2 < 46 * 46) nearPine = true;
      }
    if (!nearPine) return null;                        // no pine nearby → not the forest → no mushrooms
    for (let bz2 = Math.floor((wz - 60) / BCELL); bz2 <= Math.floor((wz + 60) / BCELL); bz2++)   // KEEP CLEAR OF ROCKS — both are decor-range ids so a mode-1 stamp would overwrite/interpenetrate the boulder (visible clipping); boulderAt is deterministic so probe the same candidates it places
      for (let bx2 = Math.floor((wx - 60) / BCELL); bx2 <= Math.floor((wx + 60) / BCELL); bx2++) {
        const b = boulderAt(bx2, bz2); if (!b) continue;
        const rm = b.size === 0 ? (ROCKV ? ROCKV : null) : ROCK26[b.mi];
        const rhalf = rm ? (Math.max(rm.sx, rm.sy) >> 1) : 4;
        const clr = rhalf + 16;                        // rock half-width + mushroom half-width (27 wide → 13.5) + a small gap → footprints never touch
        const dx = wx - b.bx, dz = wz - b.bz;
        if (dx * dx + dz * dz < clr * clr) return null;
      }
    return { wx, wz, rot: (ihash(cx * 2 + 13, cz * 2 + 7) * 3.99) | 0 };
  }
  function stampMush(m, x0, x1, z0, z1) {
    // EVERY MUSHROOM ON ITS OWN GROUND (user). One stamp put all three on a single plane taken at the
    // cluster's centre, so across a 23×28 footprint on sloping forest floor the uphill ones sank and the
    // downhill ones hung in the air. Each body is stamped separately at the ground under itself instead.
    if (!MUSHV.bodies) { stampModel(MUSHV, m.rot, m.wx, groundMin(m.wx, m.wz, 3), m.wz, x0, x1, z0, z1, 1); return; }
    const sx = MUSHV.sx, sy = MUSHV.sy;
    const fw = (m.rot & 1) ? sy : sx, fd = (m.rot & 1) ? sx : sy;   // the cluster's rotated footprint — same anchor stampModel uses
    const ax = m.wx - (fw >> 1), az = m.wz - (fd >> 1);
    for (let i = 0; i < MUSHV.bodies.length; i++) {
      const b = MUSHV.bodies[i];
      let rx, rz;                                      // this body's centre, carried through the SAME rotation stampModel applies
      if (m.rot === 0) { rx = b.cx; rz = b.cy; }
      else if (m.rot === 1) { rx = sy - 1 - b.cy; rz = b.cx; }
      else if (m.rot === 2) { rx = sx - 1 - b.cx; rz = sy - 1 - b.cy; }
      else { rx = b.cy; rz = sx - 1 - b.cx; }
      stampModel(b, m.rot, m.wx, groundMin(ax + rx, az + rz, b.br), m.wz, x0, x1, z0, z1, 1);   // …only the HEIGHT differs per body; b carries cluster coords + cluster sx/sy, so the rotation is identical
    }
  }
  const PCCELL = 26;                                   // GROUND PINECONES: fallen cones (pinecone.vox on its side), pickable, scattered on the forest floor
  function pconeAt(cx, cz) {
    if (!CONEVL) return null;
    if (ihash(cx * 71 + 13, cz * 73 + 29) > 0.22) return null;
    const wx = Math.round(cx * PCCELL + 3 + ihash(cx * 7 + 9, cz * 5 + 3) * (PCCELL - 6));
    const wz = Math.round(cz * PCCELL + 3 + ihash(cx * 3 + 17, cz * 11 + 8) * (PCCELL - 6));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── pinecones are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (H(wx, wz) <= WL + 4) return null;              // cones stay off beaches and lakebeds
    if (nearCave(wx, wz)) return null;
    return { wx, wz, rot: (ihash(cx + 19, cz + 23) * 3.99) | 0 };
  }
  function stampPcone(p, x0, x1, z0, z1) {
    stampModel(CONEVL, p.rot, p.wx, groundMin(p.wx, p.wz, 2), p.wz, x0, x1, z0, z1, 1);   // lying on its side, hugging the ground like the sticks
  }
  const SCELL = 14;                                    // STICKS: stick_1 / stick_2 .vox models, pickable, scattered on the forest floor
  function stickAt(cx, cz) {
    if (!STICKV.length) return null;
    if (ihash(cx * 61 + 7, cz * 67 + 19) > 0.42) return null;
    const wx = Math.round(cx * SCELL + 2 + ihash(cx * 3 + 8, cz * 5 + 4) * (SCELL - 4));
    const wz = Math.round(cz * SCELL + 2 + ihash(cx * 7 + 2, cz * 9 + 6) * (SCELL - 4));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── twigs are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (H(wx, wz) <= WL + 4) return null;              // sticks stay off the beach
    if (nearCave(wx, wz)) return null;
    return { wx, wz, m: ihash(cx * 11 + 3, cz * 13 + 5) < 0.5 ? 0 : STICKV.length - 1, rot: (ihash(cx + 4, cz + 7) * 3.99) | 0 };
  }
  function stampStick(s, x0, x1, z0, z1) {
    stampModel(STICKV[s.m], s.rot, s.wx, groundMin(s.wx, s.wz, 2), s.wz, x0, x1, z0, z1, 1);
  }
  const LGCELL = 96;                                   // FALLEN LOG (log.vox): one candidate per 9.6 m cell, 14% kept — rare, solid, walkable
  function logAt(cx, cz) {
    if (!LOGV) return null;
    if (ihash(cx * 71 + 13, cz * 73 + 29) > 0.14) return null;
    const wx = Math.round(cx * LGCELL + 6 + ihash(cx * 3 + 9, cz * 7 + 1) * (LGCELL - 12));
    const wz = Math.round(cz * LGCELL + 6 + ihash(cx * 11 + 4, cz * 13 + 2) * (LGCELL - 12));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── fallen logs are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (H(wx, wz) <= WL + 4) return null;
    if (nearCave(wx, wz)) return null;
    return { wx, wz, rot: (ihash(cx * 5 + 21, cz * 3 + 33) * 3.99) | 0 };
  }
  function stampLog(l, x0, x1, z0, z1) {
    stampModel(LOGV, l.rot, l.wx, groundMin(l.wx, l.wz, 6) - 1, l.wz, x0, x1, z0, z1, 1);   // sunk one voxel so it hugs the ground line
  }
  const LILYCELL = 18;                                 // LILYPADS: small/medium/large .vox pads floating on lakes and rivers
  function lilyAt(cx, cz) {                             // STATIC lilies RE-ENABLED for testing (user 2026-07-18) — stamped into the world grid (full static-voxel shading) alongside the LIVE drifting pads, so their render can be compared
    if (!LILYV.length) return null;
    if (ihash(cx * 83 + 3, cz * 89 + 17) > 0.057) return null;   // halved twice 2026-07-16 (0.34 → 0.17 → 0.085), then CUT BY 1/3 (0.085 → 0.057, user 2026-07-18)
    const wx = Math.round(cx * LILYCELL + 3 + ihash(cx * 7 + 6, cz * 3 + 14) * (LILYCELL - 6));
    const wz = Math.round(cz * LILYCELL + 3 + ihash(cx * 5 + 12, cz * 11 + 7) * (LILYCELL - 6));
    if (H(wx, wz) > WL - 1) return null;               // ≥ 2 voxels of water under the pad — real lakes/rivers, never the beach film
    const sr = ihash(cx * 19 + 8, cz * 23 + 4);
    return { wx, wz, size: Math.min(LILYV.length - 1, sr < 0.55 ? 0 : (sr < 0.88 ? 1 : 2)), rot: (ihash(cx + 31, cz + 42) * 3.99) | 0 };
  }
  function stampLily(f, x0, x1, z0, z1) {
    stampModel(LILYV[f.size], f.rot, f.wx, WL + 1, f.wz, x0, x1, z0, z1, 4);   // rests ON the water surface; mode 4 clips every column to open water
  }
  const LGIGCELL = 128;                                // GIGANTIC LILYPADS: 1-2 per lake — one candidate per 12.8 m cell, only on WIDE open lake water
  function lilyGigAt(cx, cz) {
    if (!LILYPAD_GIGV) return null;
    if (ihash(cx * 53 + 11, cz * 59 + 7) > 0.1375) return null;   // HALVED AGAIN (user 2026-07-18): 0.55 -> 0.275 -> 0.1375
    const wx = Math.round(cx * LGIGCELL + 22 + ihash(cx * 3 + 5, cz * 7 + 9) * (LGIGCELL - 44));
    const wz = Math.round(cz * LGIGCELL + 22 + ihash(cx * 11 + 3, cz * 5 + 13) * (LGIGCELL - 44));
    if (H(wx, wz) > WL - 2) return null;               // ≥ 3 voxels of water under the centre
    for (let k = 0; k < 8; k++) { const a = k * 0.7854, dx = Math.round(Math.cos(a) * 24), dz = Math.round(Math.sin(a) * 24);
      if (H(wx + dx, wz + dz) > WL - 1) return null; } // a 24-vox-radius ring must ALL be water → a real LAKE (not a river/puddle) → naturally ~1-2 pads per lake
    return { wx, wz, rot: (ihash(cx * 2 + 1, cz * 2 + 3) * 3.99) | 0 };
  }
  function stampLilyGig(g, x0, x1, z0, z1) {
    stampModel(LILYPAD_GIGV, g.rot, g.wx, WL + 1, g.wz, x0, x1, z0, z1, 4);   // floats on the lake surface; mode 4 clips each column to open water so it fits the lake outline
  }
  const TCELL = 45, TMARGIN = 24;                      // one pine candidate per 4.5 m cell (≈2× the old 64-cell density)
  const SPVIEW_D = 96, SPVIEW_W = 13;                  // spawn sight-line: 9.6 m ahead, 1.3 m either side of the view axis
  function treeAt(cx, cz) {
    if (ihash(cx * 7 + 13, cz * 11 + 5) > 0.72) return null;
    const wx = Math.round(cx * TCELL + 6 + ihash(cx * 3 + 1, cz * 5 + 2) * (TCELL - 12));
    const wz = Math.round(cz * TCELL + 6 + ihash(cx * 9 + 4, cz * 3 + 8) * (TCELL - 12));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── pines are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    const dxs = wx - SPWX, dzs = wz - SPWZ;
    if (dxs * dxs + dzs * dzs < 26 * 26) return null;  // spawn clearing
    // ── AND A CLEAR LINE OF SIGHT DOWN THE SPAWN HEADING (user 2026-08-16: "try not to have the player spawn
    // looking in front of a pine tree") ── the 26-voxel clearing above only guarantees elbow room; a pine just
    // outside it, dead ahead, still fills the screen the instant you load. This rejects trunks inside a narrow
    // CORRIDOR along SPYAW rather than widening the circle, which would clear trees to the sides and behind
    // where nobody is looking. Derived from SPYAW rather than assuming +X, so re-baking the spawn heading
    // moves the corridor with it. Short and narrow on purpose: long enough to open the view to the sand, tight
    // enough that it reads as a gap between trees and not as a felled lane.
    const fwdX = Math.sin(SPYAW), fwdZ = Math.cos(SPYAW);
    const along = dxs * fwdX + dzs * fwdZ;
    if (along > 0 && along < SPVIEW_D && Math.abs(dxs * fwdZ - dzs * fwdX) < SPVIEW_W) return null;
    if (H(wx, wz) <= WL + 4) return null;    // no pines in water or on beaches
    if (nearCave(wx, wz)) return null;
    return { tx: wx, tz: wz, rot: (ihash(cx + 101, cz + 55) * 3.99) | 0, sink: 5 + ((ihash(cx * 13, cz * 17) * 4) | 0) };   // sink 5-8 (was 1-7) — every trunk base voxel is buried, no floating trees on bumpy ground
  }
  function stampTree(tr, x0, x1, z0, z1) {             // exact rotated-array copy, clipped to a world region
    const R = MROT[tr.rot];
    const gy = groundMin(tr.tx, tr.tz, 2) - tr.sink;              // random sink (1–7) varies how deep each pine sits
    const bx = tr.tx - (R.sx >> 1), bz = tr.tz - (R.sz >> 1);
    const xa = Math.max(0, x0 - bx), xb = Math.min(R.sx - 1, x1 - 1 - bx);
    const za = Math.max(0, z0 - bz), zb = Math.min(R.sz - 1, z1 - 1 - bz);
    if (xa > xb || za > zb) return;
    for (let my = 0; my < MSZ; my++) {
      const y = gy + my; if (y < 1) continue; if (y >= WY) break;
      const yrow = y * WX, moff = my * R.sx * R.sz;
      for (let mz = za; mz <= zb; mz++) for (let mx = xa; mx <= xb; mx++) {
        const v = R.A[mx + mz * R.sx + moff]; if (!v) continue;
        W[gwrap(bx + mx, WX) + yrow + gwrap(bz + mz, WZ) * WX * WY] = remap[v];
      }
    }
    if (CONEV && PINE_ANCH.length) {                   // PINECONES — 6-12 per pine (2× the old 3-6), hung UNDER canopy anchors (foliage with open air below),
      const n = 6 + ((ihash(tr.tx * 7 + 5, tr.tz * 9 + 2) * 7) | 0);   // rotated with the tree so every region stamps them identically
      const used = new Set();                          // one cone per column — never stacked on top of each other
      for (let k = 0; k < n; k++) {                    // PINE_ANCH is angle-sorted: pick k-th from the k-th angular sector — cones ring the crown evenly
        const a = PINE_ANCH[(((k + 0.15 + ihash(tr.tx * 13 + k * 29, tr.tz * 17 + k * 31) * 0.7) / n) * PINE_ANCH.length) | 0];
        const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
        let rx, rz;
        if (tr.rot === 0) { rx = ax; rz = ay; }
        else if (tr.rot === 1) { rx = MSY - 1 - ay; rz = ax; }
        else if (tr.rot === 2) { rx = MSX - 1 - ax; rz = MSY - 1 - ay; }
        else { rx = ay; rz = MSX - 1 - ax; }
        const ck = rx | (rz << 8); if (used.has(ck)) continue; used.add(ck);
        stampModel(CONEV, (ax + az + k) & 3, bx + rx, gy + az - CONEV.sz, bz + rz, x0, x1, z0, z1, 0);   // empty cells only — never eats foliage
      }
    }
  }
  const treesInRegion = (x0, x1, z0, z1) => {
    const out = [];
    for (let cz = Math.floor((z0 - TMARGIN) / TCELL); cz <= Math.floor((z1 + TMARGIN) / TCELL); cz++)
      for (let cx = Math.floor((x0 - TMARGIN) / TCELL); cx <= Math.floor((x1 + TMARGIN) / TCELL); cx++) {
        const t = treeAt(cx, cz); if (t) out.push(t);
      }
    return out;
  };
  function* stampCellsGen(x0, x1, z0, z1, cell, margin, atFn, stampFn) {   // one yield per stamped feature — cave/dome-heavy bands can't spike a frame
    for (let cz = Math.floor((z0 - margin) / cell); cz <= Math.floor((z1 + margin - 1) / cell); cz++)
      for (let cx = Math.floor((x0 - margin) / cell); cx <= Math.floor((x1 + margin - 1) / cell); cx++) {
        const c = atFn(cx, cz); if (c) { stampFn(c, x0, x1, z0, z1); yield; }
      }
  }
  function rockRowSpan(x0, x1, wz, tops) {             // ── ROW-MAJOR ROCK CORE ── writes one wz-row's rock for every column at once, y-major with CONTIGUOUS x-runs.
    // The old per-column loop strode WX bytes per voxel (every write its own cache line, 44% of all gen CPU); this is the
    // same voxel set with the same incremental hash — imul is linear mod 2³², so re-anchoring per (x0, y) is bit-identical.
    const len = x1 - x0;
    let gxA = rockRowSpan.gx; if (!gxA || gxA.length < len) gxA = rockRowSpan.gx = new Int32Array(Math.max(2048, len));
    let maxR = 0; for (let i = 0; i < len; i++) { if (tops[i] > maxR) maxR = tops[i]; gxA[i] = gwrap(x0 + i, WX); }
    if (maxR <= 0) return;
    const zb = gwrap(wz, WZ) * WX * WY;
    for (let y = 0; y < maxR; y++) {
      let acc = (Math.imul(x0, 374761393) + Math.imul(wz, 668265263) + Math.imul(y, ROCKSTEP)) | 0;
      const yb = zb + y * WX;
      for (let i = 0; i < len; i++) {
        if (y < tops[i]) { const hh = Math.imul(acc ^ (acc >>> 13), 1274126177);
          W[yb + gxA[i]] = ROCK[((((hh ^ (hh >>> 16)) >>> 0) / 4294967296) * 3) | 0]; }
        acc = (acc + 374761393) | 0;                   // advancing x by 1 adds K1 — identical to imul(x0+i, K1) mod 2³²
      }
    }
  }
  function* genRegionGen(x0, x1, z0, z1, fresh) {      // rolling sweep with row/column-cached noise — sweeps along the region's LONG axis so the cache always pays
    rivScope = gatherRivers(x0 - 300, x1 + 300, z0 - 300, z1 + 300);   // covers the region plus every stamp margin
    if (x1 - x0 >= z1 - z0) {
      const pre = takeRows(x0, x1, z0, z1, false);     // worker-precomputed heights + moss, if the prefetch landed
      const wpad = (x1 - x0) + 2;
      let hM = new Int16Array(wpad), hC = new Int16Array(wpad), hP = new Int16Array(wpad);
      const mossRow = new Float64Array(x1 - x0);
      const fillH = pre ? ((wz, out) => { const r = wz - (z0 - 1); out.set(pre.hs.subarray(r * wpad, (r + 1) * wpad)); })
                        : ((wz, out) => { const f = makeHRow(wz); for (let i = 0; i < wpad; i++) out[i] = f(x0 - 1 + i); });
      let tops = rockRowSpan.tops; if (!tops || tops.length < x1 - x0) tops = rockRowSpan.tops = new Int16Array(Math.max(2048, x1 - x0));
      fillH(z0 - 1, hM); fillH(z0, hC);
      for (let wz = z0; wz < z1; wz++) {
        fillH(wz + 1, hP);
        if (pre) mossRow.set(pre.ms.subarray((wz - z0) * (x1 - x0), (wz - z0 + 1) * (x1 - x0)));
        else { const mf = makeMossRow(wz); for (let i = 0; i < x1 - x0; i++) mossRow[i] = mf(x0 + i); }
        for (let i = 0; i < x1 - x0; i++) { const h0 = hC[i + 1];   // the rock ceiling = fillColumn's EXACT lake/beach-clamped h, minus 3
          const lk = h0 <= WL - 1 || (h0 === WL && (hC[i] < WL || hC[i + 2] < WL || hM[i + 1] < WL || hP[i + 1] < WL));
          tops[i] = ((!lk && h0 <= WL) ? WL + 1 : h0) - 3; }
        rockRowSpan(x0, x1, wz, tops);                 // the row's whole rock core in contiguous x-runs
        yield;
        for (let wx = x0; wx < x1; wx++) {
          const i = wx - x0;
          fillColumn(wx, wz, fresh, hC[i + 1], hC[i], hC[i + 2], hM[i + 1], hP[i + 1], mossRow[i]);
          if ((i & 511) === 511) { yield; }            // fine-grained slices — a 2048-long row never blows the frame budget
        }
        const hT = hM; hM = hC; hC = hP; hP = hT;
        yield;
      }
    } else {                                           // TRANSPOSED: tall-thin region (x-direction band) — sweep columns along z
      const pre = takeRows(x0, x1, z0, z1, true);
      const wpad = (z1 - z0) + 2, cols = (x1 - x0) + 2;
      let hAll;                                        // ALL columns' heights materialized once — the rock pass runs row-major across the band (contiguous x-runs), the sweep reads its slices
      if (pre) hAll = pre.hs;
      else { hAll = new Int16Array(cols * wpad);
        for (let c = 0; c < cols; c++) { const f = makeHCol(x0 - 1 + c); for (let i = 0; i < wpad; i++) hAll[c * wpad + i] = f(z0 - 1 + i); } }
      { let tops = rockRowSpan.tops; if (!tops || tops.length < x1 - x0) tops = rockRowSpan.tops = new Int16Array(Math.max(2048, x1 - x0));
        for (let wz = z0; wz < z1; wz++) { const r = wz - (z0 - 1);
          for (let i = 0; i < x1 - x0; i++) { const c = (i + 1) * wpad + r, h0 = hAll[c];
            const lk = h0 <= WL - 1 || (h0 === WL && (hAll[c - wpad] < WL || hAll[c + wpad] < WL || hAll[c - 1] < WL || hAll[c + 1] < WL));
            tops[i] = ((!lk && h0 <= WL) ? WL + 1 : h0) - 3; }
          rockRowSpan(x0, x1, wz, tops);
          if (((wz - z0) & 63) === 63) { yield; }
        } }
      let hM = new Int16Array(wpad), hC = new Int16Array(wpad), hP = new Int16Array(wpad);
      const mossCol = new Float64Array(z1 - z0);
      const fillH = (wx, out) => { const c = wx - (x0 - 1); out.set(hAll.subarray(c * wpad, (c + 1) * wpad)); };   // pre or inline, the heights are in hAll now
      fillH(x0 - 1, hM); fillH(x0, hC);
      for (let wx = x0; wx < x1; wx++) {
        fillH(wx + 1, hP);
        if (pre) mossCol.set(pre.ms.subarray((wx - x0) * (z1 - z0), (wx - x0 + 1) * (z1 - z0)));
        else { const mf = makeMossCol(wx); for (let i = 0; i < z1 - z0; i++) mossCol[i] = mf(z0 + i); }
        for (let wz = z0; wz < z1; wz++) {
          const i = wz - z0;
          fillColumn(wx, wz, fresh, hC[i + 1], hM[i + 1], hP[i + 1], hC[i], hC[i + 2], mossCol[i]);
          if ((i & 511) === 511) { yield; }            // fine-grained slices — a 2048-long column never blows the frame budget
        }
        const hT = hM; hM = hC; hC = hP; hP = hT;
        yield;
      }
    }
    yield* stampCellsGen(x0, x1, z0, z1, OCELL, 6, oreAt, stampOre);
    yield* stampCellsGen(x0, x1, z0, z1, CAVE_CELL, CAVE_MARGIN, caveAt, stampCave);
    yield* stampCellsGen(x0, x1, z0, z1, BCELL, 44, boulderAt, stampBoulder);   // margin covers the widest rocks26 model (74 wide)
    yield* stampCellsGen(x0, x1, z0, z1, F2CELL, 16, fern2At, stampFern2);   // the fern.glb plant (23 wide)
    yield* stampCellsGen(x0, x1, z0, z1, CACCELL, 16, cactusAt, stampCactus);
    // ── ONE NAMED SWITCH, SHRUB_ON, AND IT IS DELIBERATELY LOUD ── the first time these were removed the
    // loader was gutted and this call deleted, so when the user later asked for shrubs back it read as a BUG
    // and cost an agent a full run to rediscover they had simply been turned off. The second removal (the
    // brown ones) went through the flag instead, and turning them green again was a one-word change here.
    if (SHRUB_ON) yield* stampCellsGen(x0, x1, z0, z1, SHCELL, 10, shrubAt, stampShrub);   // desert scrub. Margin 10 covers the widest model's half-footprint (still 16 across in the hand-authored set -> 8), so a bush centred just outside the region still stamps the half of it that falls inside. RE-CHECK THIS WHENEVER THE .vox CHANGE: a model wider than 20 silently loses its outer courses at every region seam, and the seam is exactly where nobody looks.
    // AFTER the cacti and BEFORE the rocks, and that order is not arbitrary. A shrub's ids are RECLAIMED LOW
    // ids (see SHRUBC/SHRUBF), so they are below DECOR_MIN — which means stampModel's mode-1 test reads a shrub voxel
    // as hard terrain and REFUSES to write over it, exactly as it would for stone. Stamped first, a bush would
    // therefore punch holes in a saguaro that lands on it. Going second it cannot, and the rocks stamp in mode
    // 2 (OVERWRITE) so they win either way. shrubAt already refuses to sit inside either, so this is belt and
    // braces rather than the mechanism — but it is the cheap half of the pair.
    yield* stampCellsGen(x0, x1, z0, z1, DRCELL, 44, drockAt, stampDrock);   // the 26 rocks26 models, in the DESERT, in their own stock grey (user 2026-08-15 — the sandstone recolour is reverted). Margin 44 covers the widest of them (74 voxels), the same figure boulderAt uses for the same models in the forest
    yield* stampCellsGen(x0, x1, z0, z1, MUCELL, 18, mushAt, stampMush);      // rare pine-forest mushroom cluster (23×27 footprint)
    yield* stampCellsGen(x0, x1, z0, z1, PCCELL, 8, pconeAt, stampPcone);
    yield* stampCellsGen(x0, x1, z0, z1, SCELL, 8, stickAt, stampStick);
    yield* stampCellsGen(x0, x1, z0, z1, LILYCELL, 8, lilyAt, stampLily);
    // (GIGANTIC lake pads REMOVED at user request 2026-07-20 — the lilyGigAt/stampLilyGig pair is left in place, and still
    //  crosses to the gen workers, so re-enabling is a one-line restore of this pass. The small drifting pads are untouched.)
    yield* stampCellsGen(x0, x1, z0, z1, LGCELL, 16, logAt, stampLog);
    for (const t of treesInRegion(x0, x1, z0, z1)) { stampTree(t, x0, x1, z0, z1); yield; }   // one tree per slice — the old all-at-once pass spiked 10–25 ms per band
  }
  function genRegion(x0, x1, z0, z1, fresh) { const g = genRegionGen(x0, x1, z0, z1, fresh); for (let r = g.next(); !r.done; r = g.next()) {} }
  function rebuildBricks(gx0, gx1, gz0, gz1) {         // grid coords, 8-aligned
    for (let bz = gz0 >> 3; bz < gz1 >> 3; bz++) for (let bx = gx0 >> 3; bx < gx1 >> 3; bx++) {
      let maxH = 0, cav = 0;                           // nothing exists above terrain + the tallest pine — sky bricks clear without a single voxel read
      for (let z = bz * 8; z < bz * 8 + 8; z++) for (let x = bx * 8; x < bx * 8 + 8; x++) { const hv = hmap[x + z * WX]; if (hv > maxH) maxH = hv; if (hv <= CAVE_FLOOR_MAX) cav = 1; }
      if (cav && maxH < HMAX) maxH = HMAX;             // ── GORGE TILE ── stampCave drops a carved column's hmap to the gorge FLOOR while its per-4-voxel-band wall jag deliberately leaves wall standing all the way back up to the pristine surface, so hmap is no longer an upper bound on that column's contents. The cap force-CLEARS every brick above it and the DDA reads an unset brick as air, so intact stone went invisible (it still collided, chopped and anchored support). H() is clamped to HMAX, so that is the honest ceiling for a column whose real height the carve erased. Must stay identical to the copy in gen-pool.js — __vb.gtest diffs the two.
      const byCap = Math.min(BY, ((maxH + 122) >> 3) + 1);
      for (let by = 0; by < BY; by++) {
        let occ = 0;
        if (by < byCap) {
          scan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {
            const rw = (y * WX + z * WX * WY + bx * 8) >> 2;         // whole u32 words — 4 voxels per test
            if (W32[rw] | W32[rw + 1]) { occ = 1; break scan; }
          }
        }
        const b = bx + by * BX + bz * BX * BY;
        if (occ) bricks[b >> 5] |= 1 << (b & 31); else bricks[b >> 5] &= ~(1 << (b & 31));
        let wonly = 0;                                 // ── WATER-ONLY bit ── every nonzero voxel is WATER_T/WATER_B → skipW rays stride the whole brick (only bricks at/below the waterline can qualify; the byte scan early-outs on the first real solid, so land bricks pay ~1 byte)
        if (occ && by * 8 <= WL) {
          wonly = 1;
          wscan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {
            const r0 = y * WX + z * WX * WY + bx * 8;
            for (let x = 0; x < 8; x++) { const v = W[r0 + x]; if (v !== 0 && v !== WATER_T && v !== WATER_B) { wonly = 0; break wscan; } }
          }
        }
        if (wonly) wbricks[b >> 5] |= 1 << (b & 31); else wbricks[b >> 5] &= ~(1 << (b & 31));
      }
    }
    rebuildBricks2(gx0, gx1, gz0, gz1);
  }
  function rebuildBricks2(gx0, gx1, gz0, gz1) {        // L2-only rebuild (from the L1 bricks) — the pooled band path merges worker-computed L1 bits, then only this remains
    for (let cz = gz0 >> 5; cz <= (gz1 - 1) >> 5; cz++) for (let cy = 0; cy < B2Y; cy++) for (let cx = gx0 >> 5; cx <= (gx1 - 1) >> 5; cx++) {
      let occ = 0;                                     // L2 = OR of the 4³ covered bricks
      scan2: for (let bz = cz * 4; bz < cz * 4 + 4; bz++) for (let by = cy * 4; by < cy * 4 + 4; by++) for (let bx = cx * 4; bx < cx * 4 + 4; bx++) {
        const b = bx + by * BX + bz * BX * BY;
        if ((bricks[b >> 5] >>> (b & 31)) & 1) { occ = 1; break scan2; }
      }
      const c = cx + cy * B2X + cz * B2X * B2Y;
      if (occ) bricks2[c >> 5] |= 1 << (c & 31); else bricks2[c >> 5] &= ~(1 << (c & 31));
    }
  }
