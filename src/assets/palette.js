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
  // ── …AND THE ONE A CREATURE COLOUR MAY BE SNAPPED TO ── palNearest above is right for scenery, which is
  // allowed to land on another piece of scenery's id. A CREATURE is not: a grid-stamped body wearing a cactus id
  // stings, wearing a pinecone id picks up as a pinecone, wearing a foliage id stops colliding. So this is the
  // same walk with the materials that carry BEHAVIOUR struck out of the candidate set. The tables are declared in
  // assets/material-tabs.js, below this fragment, so they are read through a late-bound getter rather than closed
  // over — a direct reference here is the const-before-declaration black screen this codebase is prone to.
  // Falls back to palNearest if every candidate is excluded, because a wrong shade beats no id at all.
  // The material exclusion is shared by BOTH creature paths, because both of them hand a creature somebody
  // else's id: the tolerance REUSE in edCol (which is what actually did it — measured, the pink bird's
  // (243,133,158) is 5/255 from the cactus flower's (243,130,153), inside PAL_TOL, so it was handed that id
  // outright and edSubs never even incremented) and the nearest-colour SUBSTITUTION below it. Reserved-id
  // skipping was already right in both; behaviour skipping was missing from both.
  const edMatBad = (i) => (typeof cactusTab !== 'undefined' && cactusTab[i]) ||
                          (typeof coneTab !== 'undefined' && coneTab[i]) ||
                          (typeof foliaTab !== 'undefined' && foliaTab[i]);
  const edNearShareOK = (r, g, b) => {                 // tolerance reuse, minus the ids that MEAN something
    let bd = 1e9, best;
    for (let i = DECOR_MIN; i < palette.length; i++) { const c = palette[i];
      if (!c || palOwn.has(i) || edMatBad(i)) continue;
      const d = Math.max(Math.abs(c[0] - r), Math.abs(c[1] - g), Math.abs(c[2] - b));
      if (d <= PAL_TOL && d < bd) { bd = d; best = i; } }
    return best; };
  const edSubstOK = (r, g, b) => { let bd = 1e9, id = -1;
    for (let i = 1; i < palette.length; i++) { const c = palette[i]; if (!c || palOwn.has(i) || edMatBad(i)) continue;
      const d = (c[0] - r) * (c[0] - r) + (c[1] - g) * (c[1] - g) + (c[2] - b) * (c[2] - b);
      if (d < bd) { bd = d; id = i; } }
    return id < 0 ? palNearest(r, g, b) : id; };
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
  // ── COLORADO SANDSTONE: FOUR IDS FOR A THING THAT IS NOT IN THE GAME (reclaimed 2026-08-17, the BEEHIVE) ──
  // this ramp has exactly one consumer in the whole build: the ROCK26D block in assets/bow.js, which recolours
  // the 26 boulders into sandstone twins for the desert. Nothing stamps ROCK26D. The user reverted the desert
  // rocks to stock grey, and world/gen-pool.js says so in as many words — its ROCK26D line is commented out with
  // "the desert rocks went back to stock grey, so nothing in the worker references it". So these four are back in
  // precisely the state palette.js's own comment described before the experiment: "declared and used by NOTHING".
  // Four dead ids on a table with two free is what funds the hive and half the fruit. ONE boolean brings both the
  // colours and the sandstone rocks back together, which is the whole reason this is a switch and not a deletion —
  // bow.js guards its ROCK26D build on REDROCK.length, so flipping this is the entire restore.
  // ── THE GIANT LILYPAD, AND WHY ITS FOURTEEN IDS ARE WORTH MORE THAN IT IS ── same shape as the switch below
  // and the same argument, only larger. lillypad_gigantic.json carries a 14-entry palette and every one of them
  // is minted through addCol, which never dedupes — and the pass that stamps it was removed from genRegionGen.
  // debug-api.js says so in its own words at the lilyGigAt tap: "the GIANT pads no longer stamp, so a candidate
  // site must come back with an empty footprint". So the model is loaded, marked solid, folded into LILYIDS, and
  // never written into the world: fourteen ids on a table that had NOTHING free, spent on a plant nobody can see.
  // Flipping this back on restores the model AND its colours together — bow.js skips the whole fetch when it is
  // off and every consumer is already null-guarded (markSolid, LILYIDS, lilyGigAt), which is what makes a switch
  // honest here rather than a deletion.
  const LGIG_ON = false;                               // gigantic lilypads — off, and their 14 palette ids with them
  const R26D_ON = false;                               // sandstone desert boulders (ROCK26D) — off, and the 4 REDROCK ids with them
  const REDROCK = R26D_ON ? [addCol(193, 111, 72), addCol(171, 94, 60), addCol(147, 79, 52), addCol(209, 167, 127)] : [];   // Colorado sandstone strata (last = cream band)
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
    const k = TRUNK_SMOOTH, WOOD_DEDUP = 4, lum = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;   // WOOD_DEDUP: max channel delta at which two SMOOTHED bark shades are the same colour — see the loop below
    let lo = 255, hi = 0, lo2 = 255, hi2 = 0;
    for (const ci of used) { const r = vpal[(ci - 1) * 4], g = vpal[(ci - 1) * 4 + 1], b = vpal[(ci - 1) * 4 + 2];
      if (isFol(r, g, b)) { remap[ci] = addCol(r, g, b); foliageIds.push(remap[ci]); continue; }
      const l0 = lum(r, g, b); if (l0 < lo) lo = l0; if (l0 > hi) hi = l0;
      const nr = Math.round(r + (mr - r) * k), ng = Math.round(g + (mg - g) * k), nb = Math.round(b + (mb - b) * k);
      const l1 = lum(nr, ng, nb); if (l1 < lo2) lo2 = l1; if (l1 > hi2) hi2 = l1;
      // ── THE SMOOTHING COLLAPSES SHADES, SO MINT THE COLLAPSED ONES ONCE (2026-08-18) ── pulling every bark
      // colour k of the way to the trunk's weighted mean is a CONTRACTION: shades that are distinct in
      // pine5.vox come out within a unit or two of each other, and minting one id per SOURCE colour then spent
      // several of a 256-entry table on colours nothing can tell apart. Measured before this line existed: of
      // eight wood ids, three sat within 3/255 of another one. remap[] is exactly the mechanism for two source
      // colours to share one game id, so reuse instead of minting.
      // WHY NOT palNearShare/PAL_TOL: that is a judgement about how close is close enough for UNRELATED art,
      // and it is floored at DECOR_MIN, which the bark sits below. This is a much narrower claim — the
      // smoothing has already made these the same colour — so the bound is 3, not PAL_TOL's 8, and it only
      // ever matches against this tree's own wood.
      let wid = -1;
      for (const q of woodIds) { const c = palette[q];
        if (Math.max(Math.abs(c[0] - nr), Math.abs(c[1] - ng), Math.abs(c[2] - nb)) <= WOOD_DEDUP) { wid = q; break; } }
      if (wid >= 0) { remap[ci] = wid; continue; }      // …and a source colour that lands on an existing shade just points at it
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
  // ── GORGE LAVA, COLLAPSED TO ONE ID (2026-08-18) ── it was four shades, and all four are UNREACHABLE in the
  // shipped world: caveAt refuses a gorge in the desert, in the oak forest AND in the pine forest (see the three
  // clauses in world/terrain.js), so no gorge generates anywhere and stampCave's lava lines never run. Every
  // other mention of these names is a GUARD — "is this voxel lava?" in player death, the support rules, the
  // snow skip, the console ban list, the trace shader — and a guard against a voxel that never exists behaves
  // identically whether it names one id or four. So three of the four are handed back to pay for the cherry
  // forest's blossom, on a table that is otherwise at 256/256 with nothing free.
  // RESTORING THEM IS THIS ONE LINE. If gorges ever come back, put the three addCol calls back and the lava
  // renders in its original four-tone blend again; nothing else has to change, because nothing else ever
  // distinguished them except the blend in stampCave.
  const LAVA_T = addCol(255, 122, 22), LAVA_B = LAVA_T, LAVA_R = LAVA_T, LAVA_Y = LAVA_T;   // gorge-floor lava — emissive, deadly, and currently ungenerated                                  // pond surface / body, close shades (walk-through — you swim)
  // ── THE CHERRY FOREST'S BLOSSOM (user 2026-08-18) ── the pink the oak canopy wears in the cherry biome. THREE
  // shades against the green ramp's four, and that is the whole cost of this biome's colour: the three ids come
  // from the lava collapse above, so nothing anywhere else in the world moved a single value to pay for them.
  // Three well-separated blossoms read better on a crown than four crowded ones would — the green ramp spans
  // luma 74..107 in four steps because it is describing LEAVES in shadow, and a blossom canopy is a brighter,
  // flatter thing that bands if the steps are too close. Dark → light, the same order OAKLEAF is sorted in, so
  // the id map in assets/bow.js can pair them off by rank without knowing anything about either ramp.
  // These are reserved in palOwn below: a canopy id carries "is foliage", and a model tolerance-sharing onto
  // one at 8/255 would silently inherit snow-catching, DRAPE and the see-through primary ray.
  // ── THE PALE PETAL IS GONE (user 2026-08-18: "remove the lightest most whitest voxel color from the pink
  // cherry trees") ── the ramp was deep rose -> mid pink -> (246,182,205), and that last one is nearly white:
  // it was the canopy's highlight and it read as blown-out against the white variety standing next to it.
  // The ramp was rebuilt to eight shades afterwards (see just below), so nothing was lost by dropping it — the
  // light end simply stops short of white now instead of reaching it.
  // PINK ONLY. BLOSWHITE below is untouched: the white trees are supposed to be white, and their palest step is
  // a different colour (248,238,241) that this does not go near.
  // It also hands back a palette slot, which is why the table has room again — see the WOOD_DEDUP note.
  // ── EIGHT SHADES (user 2026-08-18: "throw much more shades of pink into the cherry trees ... try 8 shades") ──
  // The two it replaces are both still here, at index 3 and 6, so nothing about the crown's existing colour
  // moves; the ramp is filled in AROUND them. It runs deep rose -> mid pink and STOPS THERE: the pale petal was
  // removed earlier the same day for reading as blown-out white, so the light end deliberately reaches only
  // (236,148,178), which is still plainly pink beside the white variety rather than competing with it.
  // Eight shades only pay off because of the DITHER in assets/bow.js: the oak crown art carries four distinct
  // leaf greens, so a per-id remap can never show more than four colours however long this list is.
  // ── AND THE WHOLE RAMP MOVED FOUR STEPS LIGHTER (user 2026-08-18: "make the darker cherry trees 2 shades
  // lighter", then "4 shades lighter") ── the dark end has climbed from the original (150,62,96) to
  // (208,110,143), four of its own steps. The pink variety is the dark one standing next to the white, so
  // lightening it is what closes the gap between the two varieties.
  // THE RAMP COMPRESSES RATHER THAN SLIDING, and that is deliberate. Sliding all eight steps up by four would
  // have carried the LIGHT end past (246,182,205) — the exact shade removed earlier today for reading as blown
  // out — so the top is pinned where two steps had already put it and the dark end walks up to meet it. The
  // tree reads four shades lighter; no white is reintroduced. If it wants to go lighter again, the honest move
  // is to revisit that cap explicitly rather than to let the top drift through it.
  // THE LIGHT END STILL STOPS SHORT OF WHITE. The new top is (248,168,195), against the (246,182,205) that was
  // removed earlier today for reading as blown out — 14 more saturation on the weakest channel, which is the
  // difference between "light pink" and "white with a tint". Do not push this further without checking that.
  const BLOSLEAF = [addCol(208, 110, 143), addCol(214, 118, 151), addCol(220, 126, 159), addCol(226, 134, 167),
                    addCol(232, 142, 175), addCol(238, 150, 183), addCol(243, 159, 189), addCol(248, 168, 195)];
  // ── AND THE WHITE VARIANT (user 2026-08-18: "make half the cherry trees have white petals instead of pink") ──
  // Same structure as BLOSLEAF and read the same way: a leaf green's RANK picks a band of this ramp and the
  // voxel's own position picks within it (assets/bow.js blosRemap), so the length here is independent of how
  // many greens the oak art happens to carry. A warm neutral with a
  // faint pink cast rather than a pure grey-white — a real white cherry is cream, and a neutral ramp beside the
  // rose one reads as a lighting bug rather than a second variety.
  // KEPT OFF THE SNOW: the palest step is 248,238,241 and settled snow is 250,250,252, which is 11 apart on
  // green and blue. They are both palOwn so neither can be handed to the other by tolerance, but the point here
  // is VISUAL — a white canopy that matches its own snow cap exactly loses the cap, and it snows in this biome
  // now (see oakWeather in world/window.js). 11/255 is enough to read the cap and little enough to stay white.
  // THE THREE IDS THESE COST were freed by the bark dedup above (WOOD_DEDUP): the table was at 256/256 and the
  // smoothing had been minting one id per SOURCE bark colour for shades it had already collapsed. So this is
  // paid for out of genuine waste, not out of somebody else's ramp.
  // ── AND SIX FOR THE WHITE VARIETY (user 2026-08-18: "more shades of white/pink for the lighter cherry
  // trees") ── the original three are kept at index 1, 3 and 5 and the ramp is filled in around them, so the
  // white trees keep the colour they already had and simply gain the steps between.
  // The light end is (247,235,238), not the (248,238,241) it replaces: settled snow is (250,250,252) and
  // PAL_TOL is now 12, so the old value sat exactly on that tolerance. These are palOwn and minted through
  // addCol, so no sharing could actually have taken them — but a white canopy that matches its own snow cap to
  // within a rounding error loses the cap, and it snows in this biome. 15/255 is enough to keep them apart.
  const BLOSWHITE = [addCol(186, 160, 170), addCol(200, 176, 184), addCol(214, 193, 201),
                     addCol(230, 212, 218), addCol(239, 225, 230), addCol(247, 235, 238)];
  for (const i of BLOSLEAF) palOwn.add(i);
  for (const i of BLOSWHITE) palOwn.add(i);
  // ── AND THE PALE VARIETY'S FALLEN PETALS (user 2026-08-19: "can you make the petals (single voxels) under the
  // light cherry tree also match the light cherry tree?") ── the ground scatter under a blossom tree wore
  // TWIGPINK whatever colour the crown above it was, so a white cherry shed PINK petals onto the grass.
  // THESE ARE BLOSWHITE'S TOP FOUR COLOURS EXACTLY, ON THEIR OWN IDS, and the duplication is the point — it is
  // the same rule OAKMOSS is built on. BLOSWHITE is CANOPY material: foliaTab, which carries the see-through
  // primary ray, the snow catch and DRAPE support. A petal lying in the grass that you can see through, that
  // catches snow and that the support resolver treats as hanging is not a petal. Its own ids get the ground
  // scatter's class instead (mossTab, beside TWIGPINK), so only the colour is shared.
  // FOUR SHADES, not the full six: the two darkest BLOSWHITE steps are the crown's shadow side and read as
  // grey on open ground, where a fallen petal is lit from above.
  // Paid for by the six ids the delta-1 share in assets/models.js reclaimed — the table was at 256/256.
  const TWIGWHITE = [addCol(214, 193, 201), addCol(230, 212, 218), addCol(239, 225, 230), addCol(247, 235, 238)];
  for (const i of TWIGWHITE) palOwn.add(i);   // reserved: the scatter keys on these, and a tolerance reuse would sprinkle somebody else's model through the blossom
  // ── AND THE FRUIT (user 2026-08-18: "scatter this shade: 840c0c on the cherry trees ... single voxels that
  // will act like cherries on the tree") ── the authored colour exactly, (132,12,12).
  // IT MINTS ITS OWN ID EVEN THOUGH THAT COLOUR IS ALREADY IN THE TABLE. addCol pushes unconditionally, so this
  // is a deliberate duplicate and palAudit will report it as one — the same arrangement BROCK has against ROCK,
  // and for the same reason: an id is a MATERIAL. The existing (132,12,12) belongs to the baked desert set, and
  // borrowing it would hand every cherry that creature's material and every one of those creatures a fruit's.
  // A duplicate colour on its own id costs one slot and keeps both meanings intact; sharing costs nothing and
  // corrupts both. There are slots now (see PAL_TOL in assets/models.js), which is what makes this affordable.
  // ── THREE MORE PURPLES FOR THE LAVENDER (user 2026-08-18: "create 3 more shades for the lavender flower ...
  // 3 more purple shades I mean") ── the model paints its whole 17-voxel spike in ONE purple, so it reads as a
  // solid block of colour. These three plus the authored one make a four-step ramp, dithered onto the spike by
  // voxel position (assets/bow.js) exactly as the blossom crowns are — same reason, same mechanism.
  // palOwn: the recolour is keyed on these, so a tolerance reuse handing one to another model would sprinkle
  // that model's material through the lavender.
  // ── TIGHTER, AND DARKER (user 2026-08-18: "make the lavender have much less contrast between its purples.
  // make it more on the darker purple side") ── the first cut ran (120,70,158) to (198,152,226), which put the
  // pale end 78 red-units above the dark end and read as a two-tone flower rather than a shaded one. All three
  // now sit BELOW the model's own authored (152,94,192) and 12 units apart, so the four-step ramp spans 112 to
  // 152 instead of 120 to 198 — the contrast drops by roughly half and the whole flower sits deeper.
  const FLOWPURP = [addCol(112, 66, 150), addCol(124, 76, 162), addCol(136, 86, 174)];
  for (const i of FLOWPURP) palOwn.add(i);
  // (The blossom band's pink meadow flower briefly minted its own three-pink ramp here, derived from the white
  // variant. The user authored a real pink flower into flowers.vox instead, so the derivation and its three ids
  // are gone — authored art beats a recolour, and it costs nothing extra because the file was already loading.)
  // ── OAK-FOREST MOSS (user 2026-08-18: "make the moss on the rocks in the oak forest match the leaf color of
  // the oak trees") ── the rock cap was laid in GRASS, which is the MOSS ramp ~12% brighter and reads much
  // duller than an oak canopy: measured, the brightest oak leaf is 50/255 from its nearest GRASS shade, and the
  // mid two are 24-26 off. So the pine forest's cap is right and the oak forest's was not.
  // THESE ARE THE OAK LEAF COLOURS EXACTLY, ON THEIR OWN IDS. Reusing OAKLEAF's ids would have been free and is
  // wrong: those are CANOPY material — foliaTab, which carries the see-through primary ray, the snow catch and
  // DRAPE support. Moss on a boulder that you can see through is not moss. Their own ids get floatTab instead,
  // the same surface-scatter class GRASS has, so the cap keeps every property the pine one has and only the
  // colour changes. Duplicate colours on separate ids, deliberately, exactly as BROCK repeats ROCK's greys.
  // THREE, NOT FOUR. The darkest oak leaf (82,115,47) is already within 7/255 of a GRASS shade, so it would buy
  // nothing; the three that matter are the ones a canopy actually reads as. The table has 5 free.
  const OAKMOSS = [addCol(105, 143, 51), addCol(107, 141, 77), addCol(134, 167, 89)];
  for (const i of OAKMOSS) palOwn.add(i);   // reserved: mossCap keys on these, and a tolerance reuse would scatter somebody else's model over the boulders
  // ── THE SECOND OAK, AND IT COSTS TWO IDS (user 2026-08-19: "make the oak trees have 2 shades of green. a
  // lighter green and a darker green. similar to whats done with the cherry trees") ── the cherry forest ships
  // two VARIETIES of one tree by remapping the oak's leaf ids onto another ramp (assets/bow.js blosRemap), and
  // that is what this is: half the oaks keep the green they have and half wear a lighter one. The DARK variety
  // is free — it is the existing OAKLEAF, stamped raw and byte-for-byte unchanged — so the only spend is the
  // light one's ramp, and even that is mostly reuse.
  //
  // TWO IDS FOR A FOUR-STEP RAMP. The light ramp is [oak leaf #2, oak leaf #3, these two] (assembled as
  // OAKLITER in assets/bow.js, which is where the luminance-sorted leaf ids exist). It CLIMBS OFF the existing
  // ramp rather than standing beside it: the light variety's two darkest steps are the dark variety's two
  // lightest, and the two below them are simply dropped. That is not thrift dressed up — it is what "the same
  // tree, lighter" means, and it is why the pair reads as one species in two shades rather than two unrelated
  // greens.
  //
  // WHY THAT IS ENOUGH TO SEE, given the two ramps overlap. The art's four greens are NOT evenly used: the
  // darkest (82,115,47) is 28-72% of every crown (measured over all seven models), so a crown's colour is
  // decided almost entirely by where rank 0 lands. Dropping it IS the effect. Simulated over the real models
  // with the real dither, the crown mean moves (103,138,60) -> (139,171,93): luma 119 -> 153, and 36/255 on the
  // widest channel.
  //
  // THE HUE STAYS ON THE OAK'S OWN LINE and is pushed a few degrees YELLOWER rather than paler, for a reason
  // that is not aesthetic: ids 112-115 are the LILY PAD's four greens, (155,182,117) up to (193,220,166), and
  // the pale continuation of the oak ramp lands inside PAL_TOL of them (measured: 8/255 on the first step).
  // These two sit 17 and 21 away instead. palOwn already makes a share impossible in both directions — this is
  // the belt to that brace, and it also keeps a sunlit crown from reading as the colour of pondweed.
  //
  // RESERVED, for exactly the reason OAKLEAF and both blossom ramps are: assets/material-tabs.js is about to
  // tell these ids they are CANOPY — walk-through, DRAPE support, snow-catching, see-through when the eye is
  // inside them, and a bird perch — and a later tolerance share would hand all of that to whatever model next
  // asked for a bright green.
  const OAKLITE = [addCol(160, 192, 100), addCol(186, 216, 124)];   // the two steps the LIGHT oak variety adds ABOVE the existing leaf ramp — dark -> light
  for (const i of OAKLITE) palOwn.add(i);
  const BLOSCHERRY = addCol(132, 12, 12);
  palOwn.add(BLOSCHERRY);   // reserved: the scatter is keyed on this id, so a tolerance reuse handing it to a model would sprinkle that model through every crown   // reserved HERE and not up with SHRUBF's: this const is declared 100 lines below that one, and reading it there is the const-before-declaration black screen (a bare `for` in a module body is not hoisted past a TDZ)
  // ── SMALL ROCK (rock.vox) — right-click to pick up ── THREE NEUTRAL greys, matching what the model is
  // actually authored with. It was two WARM greys (122,120,114) and (103,101,97), and rock.vox paints five
  // pure neutrals (147/140/134/127/120, every one r=g=b): so the stone lost its shading to two tones AND
  // picked up a brown cast. Invisible on forest loam, obvious on pale sand once the same stone was scattered
  // in the desert (user 2026-08-16: "the colors on the rock.vox do not look right"). Three shades spanning
  // the authored range cost one net palette id and put the ramp back; the five source greys resolve onto them
  // with at most 7/255 of error, inside PAL_TOL.
  const PEBBLE  = [addCol(147, 147, 147), addCol(134, 134, 134), addCol(120, 120, 120)];
  // ── THE FRUIT STALK (user 2026-08-17: "the apple doesnt seem to have a brown stem") ── the LAST free
  // slot of the 256, spent deliberately. The artist did paint a brown — apple/00.vox has the stalk at
  // (143,95,74) — but the bake pools the stalk and the leaf into one colour whose mean is a green, so it
  // arrived olive. assets/bow.js recovers the split by hue from the art the game already loads; this is
  // the id that split needs to land on, and there was no reuse that did not leak: STICK_S is PICK_STICK
  // (the stalk would right-click up as a twig), the bark ids are woodTab + solid (a hitbox in a
  // walk-through crown, and STRUCTURE hanging off DRAPE), and STICK_M has 29 authored colours inside
  // PAL_TOL of it, so it is very likely already borrowed by something that would inherit canopy.
  // (143,95,74) is the artist's own value, not a guess. RESERVED in palOwn for the reason the fruit
  // flesh ids are: a later tolerance share would hand canopy identity to whatever asked for a brown.
  const FRUIT_STEM = addCol(143, 95, 74);
  palOwn.add(FRUIT_STEM);
  const SNOW = [addCol(234, 238, 246), addCol(221, 227, 239)];                                           // fallen snow — strictly 1-voxel (10 cm) layers, walk-through decor
  // ── RAIN, AND WHY IT IS EXACTLY ONE ID ── the oak forest gets rain where the pine forest gets snow, and a
  // falling raindrop is a traced voxel in the same lattice the flakes use (see the RAIN block in TRACE), so it
  // needs a palette entry the way a flake needs SNOW[0]. THE TABLE HAS THREE SLOTS LEFT — measured, and that
  // budget is known here: one shade is spent, not a ramp. Snow can afford two because a BLANKET is a surface
  // and a surface with one colour bands; falling rain is a scatter of ~2.45% occupied cells at 5x the fall
  // speed, never adjacent to itself for more than a frame, so a second shade would buy nothing anybody could
  // see. It is also never written into W — nothing lands, nothing accumulates — so unlike SNOW it needs no
  // entry in snowTab, support-rules or any material table; it exists only as a colour the tracer looks up.
  // Deliberately sits ABOVE DECOR_MIN so the blanket solid() sweep cannot mark it solid.
  // ── …AND IT IS ONLY SPENT WHILE IT RAINS (2026-08-17, the FRUIT) ── RAIN_ON has been false since the user
  // turned the weather off, and every path that could put this colour on screen is compiled out behind it: the
  // whole rain march in TRACE is inside `if (${RAIN_ON ? 'oakNear' : 'false'})`, so `fIsRain` is a constant
  // false, `h.vox = select(SNV, RNV, fIsRain)` is always SNV, and the one other reader (`h.vox == RNV`) sits
  // inside `if (flakeHit)` where h.vox can only be SNV or RNV. Nothing renders it, and it is never written into
  // W, so the id was a slot the table was holding for a feature that is switched off — which is exactly the slot
  // the berries and the fruit needed. 0 rather than a near colour ON PURPOSE: aliasing it onto SNOW[1] would make
  // that `h.vox == RNV` test fire for real fallen snow and quietly relight the blanket. Flip RAIN_ON back and the
  // mint comes back with it, so this is a reclaim rather than a removal.
  const RAIN = RAIN_ON ? addCol(150, 196, 236) : 0;                                                      // falling rain — one light blue, lit almost entirely by the sky term (see the scatter floor in TRACE)
  const ED_WHITE = addCol(250, 250, 252), ED_GREY = addCol(202, 207, 216), ED_HLITE = addCol(255, 186, 64);
  // ── RESERVED (user 2026-08-18: "I think the flowers have hitboxes") ── and that is exactly what happened.
  // material-tabs.js makes these three SOLID because the editor stage is walkable floor, and they sit below
  // DECOR_MIN. flowers.vox authors an amber centre at (255,186,64) and a petal at (250,250,252) — the same two
  // colours, exactly — so palShare's EXACT-match path (which, unlike the tolerance path, is NOT floored at
  // DECOR_MIN) handed the flower the editor's ids and every flower with a centre got a hitbox. Measured: 2 of
  // the 14 flower ids were solid, and the amber is shared by four of the five variants.
  // palOwn is the fix rather than clearing solidTab, because the editor genuinely needs its floor: an exact
  // match on a RESERVED id is not a match, so the flower mints its own and both meanings survive.
  for (const i of [ED_WHITE, ED_GREY, ED_HLITE]) palOwn.add(i);   // asset-editor stage: white plane, 1 m gridline grey, amber selection ring (all marked solid below)
  const STICK_S = addCol(126, 95, 59), STICK_M = addCol(111, 83, 52);                                    // twig (pickable) / stick — pine-trunk browns
  // ── BLOOM IS GONE, AND ITS SIX SLOTS PAY FOR flowers.vox (user 2026-08-18: "replace all the flowers in the
  // current game with flowers.vox") ── it was six single-voxel flower HEADS, sown one per column beside a grass
  // stem in fillColumn. The authored file is five whole plants, so the six ids had no remaining reader: nothing
  // in the game wants "the colour of a flower head" as a constant any more, it wants the model's own ids.
  // Everything the six carried is now derived from those (assets/bow.js FLOWERIDS/FLOWERHEAD): floatTab and the
  // snow pass-through from FLOWERIDS, and the bees' forage test from FLOWERHEAD, which is the petals ONLY —
  // BLOOM had no stem to confuse a bee with, and the model does.
  // Deleting them here is what makes the swap affordable: the table is at its ceiling, the five variants want
  // 15 colours, and PAL_TOL absorbs the ones that already exist. Do NOT re-add a BLOOM ramp to "keep the old
  // flowers working" — there is nothing left to work, and the six slots are spent.
  // ── FRUIT AND BERRIES: THREE IDS, AND THAT IS THE WHOLE BUDGET (user 2026-08-17: berry bushes, and apples
  // and oranges in the oaks) ── the ask was four things wearing 22 authored colours between them (an 11-colour
  // apple, a 9-colour orange, a red berry and a blue one) and the table had TWO slots. It fits because two
  // separate reuses do the work that minting would have:
  //   * ONE RED SERVES THE CHERRY AND THE APPLE. [0] is the apple's own flesh, quantized to the voxel-weighted
  //     mean of its seven authored shades by tools/voxelize_fruit.py — so the apple is not "close to" anything,
  //     it IS its own average — and a cherry is the same fruit red at 1 voxel. Two fruit, one slot.
  //   * THE STEM AND LEAF OF BOTH FRUIT WEAR AN OAK LEAF ID (assets/bow.js), which costs nothing and is not a
  //     compromise: the thing is a leaf, hanging in a crown made of exactly that leaf.
  // WHY NOT BLOOM[0]/BLOOM[5], WHICH ARE ALREADY A RED AND AN ORANGE. Because an id is a MATERIAL, and both
  // directions of that swap are wrong. Taking their ids would give a fruit floatTab — which is ground scatter:
  // the aim ray walks straight THROUGH a floatTab voxel (sim/tools.js), so an apple would be unaimable, and
  // ORPHAN_OK in sim/support-rules.js is derived as "not foliage and not wood", which makes every one of them
  // deletable by the generation orphan sweep. Going the other way and marking those ids foliaTab to fix that
  // would hand every flower head in the game a canopy's see-through, snow-catching, bird-perchable identity.
  // Same colour, own ids — the arrangement BROCK, DSAND and the desert shrubs all already use.
  // The blue is this game's own: nothing in the table is a berry blue, and the two things that come closest are
  // the WATER pair, which SUP.CLASS marks FLUID and isWater() reads as the lake.
  // These sit above DECOR_MIN, so the blanket `i < DECOR_MIN` solidity sweep below cannot reach them and a
  // berry never gets a hitbox; material-tabs.js then says what they positively ARE. __vbOak.ids() (world/gen-pool.js)
  // prints all three with every material table's verdict, and __vb.palAudit() over/snaps must both still read 0.
  const FRUITC = [addCol(209, 75, 70), addCol(86, 110, 192), addCol(244, 152, 61)];                      // 0 = cherry + apple flesh, 1 = blueberry, 2 = orange flesh (0 and 2 are tools/voxelize_fruit.py's own means — keep them in step with fruit.json's `pal`)
  // ── THE BEEHIVE, IN TWO SHADES OF ONE HONEY YELLOW (user 2026-08-17: "implement the beehive.vox on some of
  // the oak trees") ── beehive.vox paints 54 voxels in an EIGHT-step ramp that never leaves one hue: red is 255
  // on every step and the whole ramp moves 12/255 in green and 52 in blue. Two ids is not a sacrifice of that,
  // it is what the model is actually made of — the darkest shade alone is 24 of the 54 voxels and it draws the
  // two horizontal BANDS that make the shape read as a hive rather than a crate, and the other seven are a soft
  // face gradient across the panels between them. So: the band exactly as authored, and one weighted mean for
  // the panels. Six of the eight shades land within 15/255 and the two extremes (a 1-voxel highlight each) within
  // 22 — bounded, unlike the palNearest substitution a third mint would have caused. assets/bow.js resolves
  // whatever the file contains onto these two by nearest colour, so re-authoring the hive cannot mint anything.
  // OWN IDS, not BLOOM's yellow or OREGOLD's: a hive is SOLID and axe-choppable (see material-tabs.js), and
  // hanging either of those flags on a flower head or on gold ore is exactly the leak DSAND and BROCK exist to
  // avoid. Above DECOR_MIN like everything else here, so material-tabs.js grants the solidity explicitly.
  const HIVEC = [addCol(255, 231, 97), addCol(255, 238, 127)];                                           // 0 = the two banding courses (the model's own darkest, exact), 1 = the panel gradient's weighted mean
  // ── AND ALL FIVE ARE RESERVED, WHICH IS THE HALF THAT IS EASY TO FORGET ── these are minted HERE, in
  // palette.js, and assets/creatures.js and assets/held-items.js parse in SHARE mode several fragments LATER at
  // PAL_TOL 8. Without palOwn, the first creature whose red lands within 8/255 of the apple's (209,75,70) — a
  // cardinal is exactly that colour — would be handed the FRUIT id, and material-tabs.js has already told that
  // id it is CANOPY: walk-through, DRAPE, snow-catching, see-through when the eye is near it. That is the
  // pinecone/stick failure in a new costume, and palOwn is the mechanism that already exists for it: palShare
  // treats an exact match on a reserved id as no match, and palNearShare skips reserved ids outright. The HIVE
  // pair needs it just as much in the other direction — its ids carry SOLID, and a bird that borrowed one would
  // grow a hitbox. Nothing here loses anything by it: assets/bow.js assigns all five BY HAND, never through
  // palShare, which is exactly the arrangement the oaks' own leaf ids use.
  for (const i of FRUITC) palOwn.add(i);
  for (const i of HIVEC) palOwn.add(i);
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

