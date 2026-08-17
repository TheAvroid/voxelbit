  // ── palette ── slot 0 = empty. Ground grain variants + remapped tree palette with per-tree tints.
  const palette = [[0, 0, 0]];
  const palOwn = new Set();                           // ids minted by an OWN-IDS model (parseVoxModel/parseVoxScene share=false). palShare must never hand one of these to anybody else — a model asks for its own ids precisely so a set built from them IDENTIFIES it, and sharing them silently destroys that. Measured: the pinecone's 7 ids were all re-issued to stick_1/stick_2, which made PICK_CONE a strict subset of PICK_STICK and every pinecone pick up as a stick (user).
  let palIdx = null;                                   // colour → first palette id, for palShare. Declared HERE, above addCol, because addCol now keeps it current and the named tables below call addCol long before palShare is defined — leaving it further down put it in the temporal dead zone.
  // ── THE 256 CEILING ── a voxel's id is 8 bits, so entry 256 does not exist: it wraps, and
  // it takes the voxel's SOLIDITY with it, not just its colour. palShare has guarded this
  // for a while; addCol did NOT, and addCol is the path every named table and all three
  // .json decor loaders use. The table currently ends at exactly 256 with zero headroom,
  // so the next shade anybody adds through addCol was a silent corruption at load time.
  // Now it snaps to the nearest existing colour instead and says so, loudly and once —
  // the wrong colour is a bug you can see, and a wrapped id is one you cannot.
  const palNearest = (r, g, b) => { let bd = 1e9, id = 1;   // closest colour already in the table, skipping RESERVED ids for the reason palShare skips them
    for (let i = 1; i < palette.length; i++) { const c = palette[i]; if (!c || palOwn.has(i)) continue;
      const d = (c[0] - r) * (c[0] - r) + (c[1] - g) * (c[1] - g) + (c[2] - b) * (c[2] - b);
      if (d < bd) { bd = d; id = i; } }
    return id; };
  const palMintLog = [];                               // ?palmint only — [id, r, g, b, caller stack]
  let palOver = 0;                                     // how many colours the ceiling turned away — __vb.palAudit() reports it
  const addCol = (r, g, b) => {
    if (palette.length >= 256) { const id = palNearest(r, g, b); palOver++;
      if (palOver === 1) console.error('[vb] PALETTE FULL (256/256) — colour', r, g, b, 'snapped to id', id,
        '- every colour added from here on is a SUBSTITUTE. Run __vb.palAudit() to see what is left to reclaim.');
      if (palIdx) { const k = (r << 16) | (g << 8) | b; if (!palIdx.has(k)) palIdx.set(k, id); }
      return id; }
    const id = (palette.push([r, g, b]), palette.length - 1);
    if (location.search.includes('palmint')) { try { throw new Error(); } catch (e) { palMintLog.push([id, r, g, b, String(e.stack || '').slice(0, 220)]); } }   // ?palmint - attribute every mint to its caller while chasing load-order races
    if (palIdx) { const k = (r << 16) | (g << 8) | b; if (!palIdx.has(k)) palIdx.set(k, id); }   // …so a colour added by a NON-share loader is still found by the next share lookup. palIdx used to be built once and never updated, which let share mode mint a second copy of a colour someone else had already added.
    return id; };
  const NEEDLE = [addCol(97, 74, 50), addCol(88, 67, 45), addCol(106, 82, 56), addCol(80, 61, 41)];      // 1..4 (shader far-field relies on these slots)
  const MOSS   = [addCol(74, 96, 48), addCol(66, 88, 44), addCol(82, 104, 54), addCol(58, 79, 40)];      // 5..8
  const DIRT   = [addCol(112, 90, 64), addCol(102, 82, 58), addCol(122, 98, 70)];
  const ROCK   = [addCol(124, 122, 116), addCol(106, 104, 100), addCol(90, 89, 86)];
  const ROCKX  = [addCol(138, 136, 130), addCol(99, 98, 94), addCol(74, 73, 70)];                        // partner shades — each formation layer is a blended two-tone swatch
  const BROCK  = [addCol(124, 122, 116), addCol(106, 104, 100), addCol(90, 89, 86)];                     // MEDIUM boulder — ROCK's exact colors on DEDICATED ids so right-click pickup can flood them without eating terrain
  // ── DESERT SHRUBS ── the shrubs' OWN palette: a 4-step GREEN ramp and 6 FLOWER shades.
  // THE SIX .vox FILES ARE HAND-AUTHORED (user 2026-08-16: "I made modifications and renamed the files").
  // They are no longer a bake, so tools/voxelize_desert_shrubs.py's old "PALN pinned to len(SHRUBC)" contract
  // is dead — there is nothing to keep in step with, and assets/bow.js resolves whatever the files contain
  // onto the ten ids below instead. Between them the six files use 14 distinct shades: an EIGHT-step green
  // ramp whose two ends are still literal cactus_1.vox body greens, and 6 flower shades resampled off the
  // cactus flower ramp (three of them exact cactus entries, the rest steps in between).
  // WHY 4 GREENS AND NOT 8. Honouring all 14 costs 12 MORE ids on a table with 16 free, and the green ramp does not
  // earn them: the whole 8-step ramp spans 23/255 in red and 16 in green, so consecutive shades differ by 3.
  // These four are four of the artist's OWN shades, chosen so every one of the eight is within 3/255 per
  // channel of the one it resolves to — inside PAL_TOL, the distance this module already treats as the same
  // colour everywhere else. The FLOWERS are not quantized at all: that ramp spans 69/255 in green alone, it is
  // the detail the user actually added, and it is a handful of voxels at the top of each plant where any
  // banding would show. Ten ids in all (2 already held, 8 new), worst error 3/255, and 7 slots still free
  // (measured 249/256 at boot — palAudit's len wobbles by one between boots, so read that as 7-8).
  // WHY NOT JUST WEAR THE CACTUS'S OWN IDS AND SPEND NOTHING AT ALL: an id carries material identity, not only
  // colour. material-tabs.js runs markSolid(CACTI) and cactusTab over every cactus id — FLOWERS INCLUDED — so
  // a shrub sharing one would get a 4 m saguaro's hitbox AND sting the player who brushed a knee-high bush.
  // Same colour, own ids — the arrangement BROCK already uses to repeat ROCK's exact greys.
  // The first two green ids are the RECLAIMED dead LOGC pair; the other eight are the first real palette spend
  // the shrubs have ever made. __vb.shrubIds() prints all ten and what every material table says about each.
  const SHRUBC = [addCol(83, 107, 36), addCol(90, 112, 39), addCol(99, 118, 43), addCol(106, 123, 46)];   // dark→light. ORDER is documentation only now — bow.js resolves by nearest colour, not by rank, so a re-authored file cannot silently shift every shade one step along the ramp
  const SHRUBF = [addCol(227, 61, 89), addCol(232, 81, 107), addCol(234, 91, 116), addCol(238, 110, 135), addCol(243, 130, 153), addCol(255, 203, 127)];   // the flower ramp, EXACT: five pinks dark→light and the cream highlight at the centre of the bloom
  // ── AND RESERVED, WHICH IS WHAT KEEPS THE CACTI OFF THEM ── quoting a colour the cacti also use is only
  // safe in ONE direction. These ids are minted here, long before assets/bow.js parses cactus_*.vox in SHARE
  // mode, so palShare's exact-match lookup would hand the cactus id 21 for its darkest green — and then
  // markSolid/cactusTab would come back through that id and make every shrub in the desert solid and spiky.
  // palOwn is the existing mechanism for exactly this: palShare treats an exact match on a reserved id as no
  // match and mints instead, so the cacti keep their own private ids and the shrubs keep theirs.
  // THE FLOWERS NEED THIS MORE THAN THE GREENS DO: (255,203,127) is the cactus bloom's cream and it is ALREADY
  // in the table twice (__vb.palAudit() lists 96/195 as an exact pair), and cactusTab covers a cactus flower
  // exactly as it covers a spine — so an unreserved match here is the one that would have made a bush sting.
  // ── ...AND A BROWN RAMP FOR THE LAST TWO SHRUBS (user 2026-08-17) ── files 5 and 6 are dead wood rather
  // than living bush. The .vox files are NOT edited for this: bow.js resolves every authored shade onto the
  // nearest SHRUB-OWNED id, so a brown painted into the file would simply snap back to the closest green.
  // The colour has to exist as an id first, and the loader has to be told which files may reach it.
  // Pine-wood brown taken lighter, as asked. It cannot be copied from the trunk as a literal: the bark
  // shades come out of the pine .vox and are then pulled toward their own mean by TRUNK_SMOOTH, so the
  // brown on screen is not a constant that exists anywhere to quote. Three steps, not four: the table had
  // four ids free, and a ramp that empties it is one that makes the NEXT colour anywhere in the game
  // silently nearest-match.
  const SHRUBB = [addCol(141, 110, 74), addCol(158, 126, 88), addCol(174, 142, 103)];   // dark -> light
  for (const i of SHRUBC) palOwn.add(i);
  for (const i of SHRUBF) palOwn.add(i);
  for (const i of SHRUBB) palOwn.add(i);
  const SAND   = [addCol(203, 183, 145), addCol(191, 171, 133), addCol(213, 193, 155)];                  // lake beaches + lakebed
  const DSAND  = [addCol(214, 188, 132), addCol(205, 178, 122), addCol(222, 197, 143), addCol(196, 169, 115)];   // ── DESERT SAND ── warmer and more saturated than the lake SAND above, and on DEDICATED ids for a reason that is not aesthetic: sandTab slows the player (beach sand pits), and sharing ids would make an entire biome wade. These are deliberately NOT in sandTab.
  const REDROCK = [addCol(193, 111, 72), addCol(171, 94, 60), addCol(147, 79, 52), addCol(209, 167, 127)];   // Colorado sandstone strata (last = cream band)
  const ORECOAL = [addCol(52, 52, 56), addCol(44, 44, 48)];                                                // minerals - seen in cave walls
  const OREIRON = [addCol(150, 106, 74), addCol(134, 94, 66)];
  const OREGOLD = [addCol(216, 174, 58), addCol(196, 156, 50)];
  const ORECRYS = [addCol(88, 196, 202), addCol(70, 176, 184)];

  // ── pine5.vox ── MagicaVoxel, Z-up (model z → world y)
  setLoad(12); await stage('loading pine5.vox…');
  let vox;
  try { vox = new Uint8Array(await (await fetch('assets/foilage/pine5.vox')).arrayBuffer()); } catch (e) { fail('failed to fetch assets/foilage/pine5.vox — serve over http (start.bat)'); }
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
  // ── SMALL ROCK (rock.vox) — right-click to pick up ── THREE NEUTRAL greys, matching what the model is
  // actually authored with. It was two WARM greys (122,120,114) and (103,101,97), and rock.vox paints five
  // pure neutrals (147/140/134/127/120, every one r=g=b): so the stone lost its shading to two tones AND
  // picked up a brown cast. Invisible on forest loam, obvious on pale sand once the same stone was scattered
  // in the desert (user 2026-08-16: "the colors on the rock.vox do not look right"). Three shades spanning
  // the authored range cost one net palette id and put the ramp back; the five source greys resolve onto them
  // with at most 7/255 of error, inside PAL_TOL.
  const PEBBLE  = [addCol(147, 147, 147), addCol(134, 134, 134), addCol(120, 120, 120)];
  const SNOW = [addCol(234, 238, 246), addCol(221, 227, 239)];                                           // fallen snow — strictly 1-voxel (10 cm) layers, walk-through decor
  const ED_WHITE = addCol(250, 250, 252), ED_GREY = addCol(202, 207, 216), ED_HLITE = addCol(255, 186, 64);   // asset-editor stage: white plane, 1 m gridline grey, amber selection ring (all marked solid below)
  const STICK_S = addCol(126, 95, 59), STICK_M = addCol(111, 83, 52);                                    // twig (pickable) / stick — pine-trunk browns
  const BLOOM   = [addCol(198, 62, 54), addCol(226, 192, 62), addCol(230, 228, 220), addCol(152, 94, 192), addCol(224, 122, 162), addCol(228, 142, 56)];   // flower heads
  const solidTab = new Uint8Array(256);                // per-id collision: terrain/trunks/logs solid; decor + FOLIAGE walk-through
  for (let i = 1; i < DECOR_MIN; i++) solidTab[i] = 1;
  for (const f of foliageIds) solidTab[f] = 0;
  // ── THE SHRUBS ARE SOLID NOW (user 2026-08-16: "give hitboxes to the shrubs, so the player cant just walk
  // through it") ── these two loops used to CLEAR solidTab for the shrub greens and flower shades, because the
  // ids are reclaimed and sit below DECOR_MIN where a blanket sweep marks everything solid. Letting that sweep
  // stand is the whole hitbox: `solid()` reads solidTab and nothing else. They keep decorTab, so they are still
  // soft to any tool and still choppable — only walking through them stops.
  const foliaTab = new Uint8Array(256);              // per-id CANOPY flag. Leaves collide with NOTHING (user): not the player, not rigid bodies, not creature navigation. What is left of this flag is rendering (see-through when the eye is inside a crown), the perched-bird SUPPORT test (which is placement, not collision — birds have to be able to sit in a canopy), and letting a stamp overwrite needles. Was: walk-through for the player, but the cinematic camera must still steer around it or it flies straight through pine crowns
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
  const cactusTab = new Uint8Array(256);               // per-id CACTUS flag — spines hurt on contact (user 2026-08-15), filled in material-tabs
  const pickOnlyTab = new Uint8Array(256);             // …and which need the PICK. Stone does: an axe bounces off a boulder, and the pick is no use against a tree (see the swing gate).
  const digOnlyTab = new Uint8Array(256);              // …and which need the SHOVEL. Ground does — the dirt and mossy grass the terrain is made of, and nothing else.
  const sandTab = new Uint8Array(256); for (const s of SAND) sandTab[s] = 1;   // per-id SAND flag — walking on beach/lakebed sand slows the player (sand pits)
  console.log('[vb] pine5.vox', MSX, MSY, MSZ, 'palette', palette.length, 'decor from', DECOR_MIN, 'foliage ids', foliageIds.length);

