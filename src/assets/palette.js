  // ── palette ── slot 0 = empty. Ground grain variants + remapped tree palette with per-tree tints.
  const palette = [[0, 0, 0]];
  const palOwn = new Set();                           // ids minted by an OWN-IDS model (parseVoxModel/parseVoxScene share=false). palShare must never hand one of these to anybody else — a model asks for its own ids precisely so a set built from them IDENTIFIES it, and sharing them silently destroys that. Measured: the pinecone's 7 ids were all re-issued to stick_1/stick_2, which made PICK_CONE a strict subset of PICK_STICK and every pinecone pick up as a stick (user).
  let palIdx = null;                                   // colour → first palette id, for palShare. Declared HERE, above addCol, because addCol now keeps it current and the named tables below call addCol long before palShare is defined — leaving it further down put it in the temporal dead zone.
  const addCol = (r, g, b) => { const id = (palette.push([r, g, b]), palette.length - 1);
    if (palIdx) { const k = (r << 16) | (g << 8) | b; if (!palIdx.has(k)) palIdx.set(k, id); }   // …so a colour added by a NON-share loader is still found by the next share lookup. palIdx used to be built once and never updated, which let share mode mint a second copy of a colour someone else had already added.
    return id; };
  const NEEDLE = [addCol(97, 74, 50), addCol(88, 67, 45), addCol(106, 82, 56), addCol(80, 61, 41)];      // 1..4 (shader far-field relies on these slots)
  const MOSS   = [addCol(74, 96, 48), addCol(66, 88, 44), addCol(82, 104, 54), addCol(58, 79, 40)];      // 5..8
  const DIRT   = [addCol(112, 90, 64), addCol(102, 82, 58), addCol(122, 98, 70)];
  const ROCK   = [addCol(124, 122, 116), addCol(106, 104, 100), addCol(90, 89, 86)];
  const ROCKX  = [addCol(138, 136, 130), addCol(99, 98, 94), addCol(74, 73, 70)];                        // partner shades — each formation layer is a blended two-tone swatch
  const BROCK  = [addCol(124, 122, 116), addCol(106, 104, 100), addCol(90, 89, 86)];                     // MEDIUM boulder — ROCK's exact colors on DEDICATED ids so right-click pickup can flood them without eating terrain
  const LOGC   = [addCol(121, 91, 57), addCol(105, 78, 49)];                                             // fallen LOG — pine-trunk browns, solid
  const SAND   = [addCol(203, 183, 145), addCol(191, 171, 133), addCol(213, 193, 155)];                  // lake beaches + lakebed
  const REDROCK = [addCol(193, 111, 72), addCol(171, 94, 60), addCol(147, 79, 52), addCol(209, 167, 127)];   // Colorado sandstone strata (last = cream band)
  const ORECOAL = [addCol(52, 52, 56), addCol(44, 44, 48)];                                                // minerals - seen in cave walls
  const OREIRON = [addCol(150, 106, 74), addCol(134, 94, 66)];
  const OREGOLD = [addCol(216, 174, 58), addCol(196, 156, 50)];
  const ORECRYS = [addCol(88, 196, 202), addCol(70, 176, 184)];

  // ── pine5.vox ── MagicaVoxel, Z-up (model z → world y)
  setLoad(12); await stage('loading pine5.vox…');
  let vox;
  try { vox = new Uint8Array(await (await fetch('assets/tree/pine5.vox')).arrayBuffer()); } catch (e) { fail('failed to fetch assets/tree/pine5.vox — serve over http (start.bat)'); }
  const dv = new DataView(vox.buffer);
  let MSX = 0, MSY = 0, MSZ = 0, voxels = null; const vpal = new Uint8Array(1024);
  { const walk = (off, end) => { while (off < end) {
      const id = String.fromCharCode(vox[off], vox[off + 1], vox[off + 2], vox[off + 3]);
      const sz = dv.getUint32(off + 4, true), csz = dv.getUint32(off + 8, true);
      if (id === 'SIZE' && !MSX) { MSX = dv.getUint32(off + 12, true); MSY = dv.getUint32(off + 16, true); MSZ = dv.getUint32(off + 20, true); }
      else if (id === 'XYZI' && !voxels) { const n = dv.getUint32(off + 12, true); voxels = vox.subarray(off + 16, off + 16 + n * 4); }
      else if (id === 'RGBA') { vpal.set(vox.subarray(off + 12, off + 12 + 1024)); }
      else if (id === 'MAIN') { walk(off + 12 + sz, off + 12 + sz + csz); off += 12 + sz + csz; continue; }
      off += 12 + sz + csz;
    } };
    walk(8, vox.length);
  }
  if (!voxels) fail('pine5.vox: no model found');
  const M = new Uint8Array(MSX * MSY * MSZ);
  for (let i = 0; i < voxels.length; i += 4) M[voxels[i] + voxels[i + 1] * MSX + voxels[i + 2] * MSX * MSY] = voxels[i + 3];
  const remap = new Uint8Array(256);
  const foliageIds = [];                               // green tree voxels get NO hitbox — you can walk through the canopy
  const woodIds = [];                                  // …and the BARK/branch ids: everything in the pine that is not foliage. The axe cuts any of it (user), tree or stump.
  // ── BARK SMOOTHING (user) ── the authored trunk shades sit far apart, so neighbouring voxels read as
  // hard-edged blotches rather than one piece of wood. Pull every BARK shade toward the trunk's own
  // weighted mean, which compresses the contrast while keeping each shade's relative order and hue —
  // the grain is still there, just quieter. The asset is untouched; this is one number to tune, and
  // ?bark=N (0 = as authored, 1 = flat) overrides it live for A/B without an edit.
  const TRUNK_SMOOTH = (() => { const m = /[?&]bark=([0-9.]+)/.exec(location.search);
    return m ? Math.max(0, Math.min(1, parseFloat(m[1]))) : 0.55; })();
  { const used = new Set(), cnt = new Map();
    for (let i = 0; i < M.length; i++) if (M[i]) { used.add(M[i]); cnt.set(M[i], (cnt.get(M[i]) || 0) + 1); }
    const isFol = (r, g, b) => g > r && g >= b;
    // weighted mean of the bark, so the shade the trunk is mostly made of anchors the result
    let sr = 0, sg = 0, sb = 0, sw = 0;
    for (const ci of used) { const r = vpal[(ci - 1) * 4], g = vpal[(ci - 1) * 4 + 1], b = vpal[(ci - 1) * 4 + 2];
      if (isFol(r, g, b)) continue; const w = cnt.get(ci); sr += r * w; sg += g * w; sb += b * w; sw += w; }
    const mr = sw ? sr / sw : 0, mg = sw ? sg / sw : 0, mb = sw ? sb / sw : 0;
    const k = TRUNK_SMOOTH, lum = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;
    let lo = 255, hi = 0, lo2 = 255, hi2 = 0;
    for (const ci of used) { const r = vpal[(ci - 1) * 4], g = vpal[(ci - 1) * 4 + 1], b = vpal[(ci - 1) * 4 + 2];
      if (isFol(r, g, b)) { remap[ci] = addCol(r, g, b); foliageIds.push(remap[ci]); continue; }
      const l0 = lum(r, g, b); if (l0 < lo) lo = l0; if (l0 > hi) hi = l0;
      const nr = Math.round(r + (mr - r) * k), ng = Math.round(g + (mg - g) * k), nb = Math.round(b + (mb - b) * k);
      const l1 = lum(nr, ng, nb); if (l1 < lo2) lo2 = l1; if (l1 > hi2) hi2 = l1;
      remap[ci] = addCol(nr, ng, nb); woodIds.push(remap[ci]); }
    console.log('[vb] bark smoothing', k, '- luminance spread', (hi - lo).toFixed(1), '->', (hi2 - lo2).toFixed(1)); }
  const MROT = [];                                     // the 4 exact 90° rotations, precomputed — variation without resampling artifacts
  for (let k = 0; k < 4; k++) {
    const sx = (k & 1) ? MSY : MSX, sz = (k & 1) ? MSX : MSY;
    const A = new Uint8Array(sx * sz * MSZ);
    for (let z = 0; z < MSZ; z++) for (let y = 0; y < MSY; y++) for (let x = 0; x < MSX; x++) {
      const v = M[x + y * MSX + z * MSX * MSY]; if (!v) continue;
      let rx, rz;
      if (k === 0) { rx = x; rz = y; }
      else if (k === 1) { rx = MSY - 1 - y; rz = x; }
      else if (k === 2) { rx = MSX - 1 - x; rz = MSY - 1 - y; }
      else { rx = y; rz = MSX - 1 - x; }
      A[rx + rz * sx + z * sx * sz] = v;
    }
    MROT.push({ A, sx, sz });
  }
  // walk-through DECOR (grass, ferns) lives at the TOP of the palette — solid() ignores ids >= DECOR_MIN, rays still hit them
  const DECOR_MIN = palette.length;
  const GRASS = [addCol(83, 108, 54), addCol(74, 99, 49), addCol(92, 117, 61), addCol(65, 88, 45)];      // = the MOSS palette, only ~12% brighter — strands blend into their patch
  const WATER_T = addCol(58, 128, 154), WATER_B = addCol(43, 106, 134);
  const isWater = (v) => v === WATER_T || v === WATER_B;   // the lake surface / body ids — a swing walks straight through these (see chopSwing)
  const LAVA_T = addCol(255, 122, 22), LAVA_B = addCol(198, 44, 6), LAVA_R = addCol(226, 58, 10), LAVA_Y = addCol(255, 206, 46);   // gorge-floor lava — blended red/orange/yellow, emissive, deadly                                  // pond surface / body, close shades (walk-through — you swim)
  const PEBBLE  = [addCol(122, 120, 114), addCol(103, 101, 97)];                                         // SMALL rock — right-click to pick up
  const SNOW = [addCol(234, 238, 246), addCol(221, 227, 239)];                                           // fallen snow — strictly 1-voxel (10 cm) layers, walk-through decor
  const ED_WHITE = addCol(250, 250, 252), ED_GREY = addCol(202, 207, 216), ED_HLITE = addCol(255, 186, 64);   // asset-editor stage: white plane, 1 m gridline grey, amber selection ring (all marked solid below)
  const STICK_S = addCol(126, 95, 59), STICK_M = addCol(111, 83, 52);                                    // twig (pickable) / stick — pine-trunk browns
  const BLOOM   = [addCol(198, 62, 54), addCol(226, 192, 62), addCol(230, 228, 220), addCol(152, 94, 192), addCol(224, 122, 162), addCol(228, 142, 56)];   // flower heads
  const solidTab = new Uint8Array(256);                // per-id collision: terrain/trunks/logs solid; decor + FOLIAGE walk-through
  for (let i = 1; i < DECOR_MIN; i++) solidTab[i] = 1;
  for (const f of foliageIds) solidTab[f] = 0;
  const foliaTab = new Uint8Array(256);                // per-id CANOPY flag. Leaves collide with NOTHING (user): not the player, not rigid bodies, not creature navigation. What is left of this flag is rendering (see-through when the eye is inside a crown), the perched-bird SUPPORT test (which is placement, not collision — birds have to be able to sit in a canopy), and letting a stamp overwrite needles. Was: walk-through for the player, but the cinematic camera must still steer around it or it flies straight through pine crowns
  for (const f of foliageIds) foliaTab[f] = 1;
  const mushTab = new Uint8Array(256);                 // per-id BOUNCY flag — mushroom voxels trampoline the player (filled once MUSHV loads)
  const rockTopTab = new Uint8Array(256);              // per-id ROCK flag for the WORLDGEN stone — see the fill below
  // ── IS THIS SURFACE STONE? ── the id table alone is not enough. Worldgen strata and ore have known ids, but
  // the stamped rock DECOR (boulders, field formations) ships as .json and its palette entries are assigned by
  // nearest-colour match into an already-full 256 table, so they land on ids nothing declares — measured, the
  // rock a mammal actually stands on came back as id 149, which no rock list contains. Its COLOUR is
  // unmistakable though, and it is the same signature the original spawn test used: r≈g≈b, mid-bright. Dirt
  // (97,74,50) and grass (58,79,40) fail the r-g gap, sand (203,183,145) fails it and the brightness ceiling,
  // and snow is far too bright — so the two tests together cover generated stone and stamped stone alike.
  const rockCol = (c) => !!c && Math.abs(c[0] - c[1]) < 18 && Math.abs(c[1] - c[2]) < 18 && c[0] > 60 && c[0] < 200;
  const isRockSurf = (v) => !!v && (!!rockTopTab[v] || rockCol(palette[v]));
  const decorTab = new Uint8Array(256);                // per-id CHOPPABLE-DECOR flag — mushrooms, ferns and ground logs yield chunks (see phChopDecor). Deliberately NOT mushTab: choppable and bouncy are different properties, and reusing it would make every fern a trampoline.
  const axeOnlyTab = new Uint8Array(256);              // …and of those, which need a CUTTING tool (axe/knife) rather than any held tool. Wood does; mushrooms and ferns do not.
  const pickOnlyTab = new Uint8Array(256);             // …and which need the PICK. Stone does: an axe bounces off a boulder, and the pick is no use against a tree (see the swing gate).
  const digOnlyTab = new Uint8Array(256);              // …and which need the SHOVEL. Ground does — the dirt and mossy grass the terrain is made of, and nothing else.
  const sandTab = new Uint8Array(256); for (const s of SAND) sandTab[s] = 1;   // per-id SAND flag — walking on beach/lakebed sand slows the player (sand pits)
  console.log('[vb] pine5.vox', MSX, MSY, MSZ, 'palette', palette.length, 'decor from', DECOR_MIN, 'foliage ids', foliageIds.length);

