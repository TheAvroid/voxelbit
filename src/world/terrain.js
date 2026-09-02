  // @module - worldgen: heights, rivers, gorges and every stamped decoration - the source the gen worker is built from
  // @exports BKCELL, BKMARGIN, BK_BOLE, BK_LEAN, BK_SPAWN, BKHIVE, birchAt, birchTrunkW, stampBirch, BCELL, CACCELL, CAVE_CELL, CAVE_FLOOR_MAX, CAVE_MARGIN, CAVE_WMAX, DRCELL, F2CELL, FLWCELL, FLWPATCH, LGCELL, LGIGCELL, LILYCELL, MUCELL, flowerAt, mossCap, stampFlower, OCELL, OKCELL, OKFRUIT, OKHIVE, OKMARGIN, OKVIEW_W, PCCELL, SCELL, SHCELL, SHRUB_ON, SPVIEW_D, SPVIEW_W, TCELL, TMARGIN, boulderAt, cactusAt, caveAt, caveHitsBox, drockAt, fern2At, fillColumn, genRegion, genRegionGen, hiveAt, lilyAt, lilyGigAt, logAt, mushAt, nearCave, oakAt, oreAt, pconeAt, rebuildBricks, rebuildBricks2, rockRowSpan, shrubAt, stampBoulder, stampCactus, stampCave, stampCellsGen, stampDrock, stampFern2, stampLily, stampLilyGig, stampLog, stampModel, stampMush, stampOak, stampOre, stampPcone, stampShrub, stampStick, stampTree, stickAt, sweepOrphans, treeAt, treesInRegion
  // ── deterministic world-coordinate generation ──────────────────────────────
  function fillColumn(wx, wz, fresh, h0, hxm, hxp, hzm, hzp, mossV) {   // terrain + lakes + twigs + grass; heights + moss fbm arrive precomputed from the row sweep
    const gx = gwrap(wx, WX), gz = gwrap(wz, WZ);
    const tI = (gx >> 3) + (gz >> 3) * BX;
    if (!touched[tI]) { fresh = true; touched[tI] = 1; }   // virgin memory is all zeros — no need to write the sky
    let h = h0;
    const lake = h <= WL - 1 ||                        // ≥ 2 voxels deep, OR a 1-deep rim column joined to real water — no isolated dither puddles
      (h === WL && (hxm < WL || hxp < WL || hzm < WL || hzp < WL));
    // ── …BUT ON SNOW A FLUSH SHELF IS AN ARTIFACT (user 2026-08-29) ── this clamp pins every column that fell
    // at or under the waterline to ONE altitude, and a dead-level plateau is invisible in a forest (grass,
    // litter and moss break it up) and glaring on bare snow: a perfectly smooth white shelf with a hard drawn
    // edge, which is the shape in the screenshot. Measured: 596 perfectly flat samples in one arctic view, all
    // at exactly WL + 1.
    // The arctic gets the same coherent relief its own ground carries, applied UPWARD only — max(0, ...) is
    // what keeps the guarantee this line exists for, which is that dry land never sits below the water plane.
    // Only columns the clamp actually fires on pay for the mask lookup, which is a thin rim around water.
    if (!lake && h <= WL) h = WL + 1 + (arcticM(wx, wz) > ARCT_BARE
      ? Math.max(0, Math.round((fbm(wx * 0.09 + 3.1, wz * 0.09 + 8.7) - 0.42) * 5)) : 0);   // dry land NEVER sits below the water plane — beach tops meet the surface FLUSH
    hmap[gx + gz * WX] = h;                            // hmap = the GROUND (lakebeds included — you walk on them, underwater)
    const mossy = !lake && mossV > 0.52;               // 0.56 → 0.52 ≈ +30% moss coverage
    const dm = desertM(wx, wz);                        // biome weight for this column: 0 = pine forest, 1 = open desert
    const om = oakM(wx, wz);                           // …and the other way: 1 = oak forest (west of the pines), 0 = pine forest. The two masks can never both be non-zero — see the gap arithmetic at OAKOFF
    const bm = birchM(wx, wz);                         // …and the BIRCH band, which sits between the pine and the sand and wears the OAK's terrain (world/window.js)
    const am = arcticM(wx, wz);                        // …and the ARCTIC, which unlike the others replaces the ground COMPLETELY: no soil shows through snow
    const cm = (om > 0 && chNear(wx)) ? cherryM(wx, wz) : 0;   // chNear FIRST: this line runs once per COLUMN, and cherryM is ~7 vnoise — see the bound's note in world/window.js. The om test alone was not a cheap-out at all, because om > 0 is the whole infinite oak forest           // …and the blossom, which is a SUB-REGION of the oak mask (world/window.js), so `om > 0` is an exact cheap-out rather than an approximation: outside the oak forest cherryM is 0 by construction and the pine forest and the desert pay nothing at all for this biome existing
    const base = gx + gz * WX * WY;
    let surfMoss = false;                              // grass only grows where a MOSS surface voxel actually landed
    let surfBirch = false;                             // …and in the birch forest the strands match ITS floor, not the pine's
    const yTop = fresh ? (lake ? WL + 1 : h) : WY;     // fresh columns skip writing the empty sky — ~40% faster world builds
    // (the ROCK core — the hottest loop in worldgen, ~180 voxels/column — moved to rockRowSpan: writing it per COLUMN
    // strode WX bytes per voxel, a fresh cache line every write; the row-major pass writes contiguous x-runs instead.
    // rockTop was min(h-3, yTop), but yTop ≥ h in every branch, so h-3 exactly — rockRowSpan reproduces it bit-identically.)
    // ── SOIL ── 15 voxels deep under EVERY column (user), cut out of the rock core rockRowSpan already
    // wrote. This used to be 2 voxels everywhere plus a deep layer on grass only, which is exactly the
    // uneven depth the user saw: deep under the meadows, shallow on the beach and the forest floor.
    // ── THE ARCTIC HAS NO SOIL (user 2026-08-29: "I can see dirt exposed when the terrain drops off to
    // water. make all the terrain in the arctic snow") ── this layer is what a cliff face, a river bank or a
    // dug hole EXPOSES, and it was brown dirt under every white surface: the top voxel was snow and the
    // fifteen under it were soil, so the moment the ground dropped away you saw the seam. Snow all the way
    // down through the soil band instead. Not dithered against the mask — an exposed face has to be one
    // material or the seam simply becomes a speckled seam.
    const asn = arctSnow(wx, wz);                      // the SNOW mask, not the biome mask — it reaches out to the treeline and its edge is 2-D noise (world/window.js)
    const aSoil = asn > 0.5;
    // ── AND THE BIRCH FOREST HAS NO SOIL EITHER (user 2026-09-02: "make sure there is not dirt in the terrain
    // but grass instead") ── the SAME fix the arctic took, for the same reason and with the same shape: this
    // layer is what a slope down to the water, a cliff or a dug hole EXPOSES, and it was brown under a green
    // surface, so the moment the ground fell away you saw the seam. Its own light green all the way down
    // instead. NOT dithered against the mask — an exposed face has to be ONE material or the seam just
    // becomes a speckled seam, which is the note the arctic arm already makes.
    const bSoil = bm > 0.5;
    for (let y = Math.max(0, h - 16); y < Math.min(h - 1, yTop); y++) W[base + y * WX] = aSoil ? ASNOW[(ihash(wx, y * 131 + wz) * 4) | 0] : bSoil ? BIRCHMOSS[(ihash(wx, y * 131 + wz) * 4) | 0] : DIRT[(ihash(wx, y * 131 + wz) * 3) | 0];
    if (h - 1 >= 0 && h - 1 < yTop) {                  // the SURFACE voxel
      const sh = ihash(wx * 3 + 1, wz * 3 + 7);        // hoisted — was hashed up to twice
      const shore = h <= WL + 6;                       // the band sand may occupy AT ALL — solid to WL+4, then two levels of soft edge above it
      let c;
      // ── THE ARCTIC SURFACE ── FIRST, because it is the one biome whose ground is not soil: snow covers the
      // shore, the flats and the hills alike, so it has to win over the beach and sand-blend arms below rather
      // than be overridden by them.
      // ── THE ARCTIC IS SNOW, AND ONLY SNOW ── one material for the whole biome (user 2026-08-30: "remove
      // everything except the terrain and water", and 2026-08-29: "I can see dirt exposed when the terrain
      // drops off to water. make all the terrain in the arctic snow"). The first build had two more arms here
      // — glacier ICE where the ice field beat the land, and bare ROCK on slopes of 4 voxels or steeper, so
      // cliffs read as stone. Both are gone with the fields that drove them; a slope in this biome is a snow
      // slope. Dithered against the mask, exactly as the desert and oak arms below are, so the white thins out
      // across the whole rim instead of stopping on the iso-line.
      if (asn > 0 && ihash(wx * 5 + 17, wz * 7 + 29) < asn) c = ASNOW[(sh * 4) | 0];
      else if (dm > 0 && ihash(wx * 5 + 17, wz * 7 + 29) < dm) c = DSAND[(sh * 4) | 0];                       // ── DESERT ── dithered against the mask itself, so the sand thins out into the forest floor across the whole rim instead of ending on a line. Same trick as the sand-to-forest blend below, driven by the biome weight rather than by depth.
      // ── THE SAND IS SOLID SAND, AND THE BLEND SITS ABOVE IT (user 2026-09-01: "remove the dirt and grass
      // in the sand", with a screenshot of a beach speckled black) ── the dither used to start at WL+2, so
      // every MISS inside the beach fell through to the last arm of this chain and painted needle litter or
      // moss. On a wide beach that is most of its area, which is the speckle. The band is unchanged in
      // spirit — sand still fades into the forest — but the fade now happens ABOVE the sand rather than
      // through it, so nothing inside the beach can miss.
      // ── AND FIVE LEVELS OF IT (user: "make the sand have more steps.. 3-5") ── a step is one voxel of
      // height, so the COUNT of steps you can see is the number of levels the sand spans: WL..WL+4 is five,
      // and the terraces are wide enough now (see PINE_BOWL in world/window.js) that they read as separate
      // treads rather than one slope. WL..WL+1 was two, and that is why the beach looked like a single step.
      else if (shore && h <= WL + 4) c = SAND[(sh * 3) | 0];
      else if (shore && ihash(wx * 7 + 5, wz * 11 + 3) < 1 - sstep(Math.max(0, Math.min(1, (h - WL - 4) / 2)))) c = SAND[(sh * 3) | 0];   // the soft edge, now WL+5..WL+6 — sand thinning into the forest floor ABOVE the beach proper
      // ── AND THE BLEND IS SMOOTHSTEPPED, NOT LINEAR (user 2026-09-01: "make the dirt/grass terrain have
      // smooth transition into the sand terrain") ── the old ramp was linear in depth, so sand probability
      // fell at a constant rate and both ENDS of the blend were corners: a hard edge where the solid beach
      // stopped and another where the dither ran out. sstep is flat at 0 and 1 and steep only in the middle,
      // so the sand now thins out of the beach and into the forest floor with no visible line at either end —
      // which matters more here than it did, because halving the band above left half the room to do it in.
      // ── NO FOREST FLOOR ON THE DESERT SIDE, BUT ONLY NEAR WATER ── the DESERT branch above is dithered
      // against the mask, so a fraction (1 - dm) of columns miss it by design; inland those misses are the
      // gradient, which is the whole point. Near a WATERLINE they were landing on moss instead, and clustered
      // into the green patch on the lake's desert shore. This catches exactly that case. An earlier version
      // of this fix dropped the `shore` test and sent EVERY dm > 0.5 column to sand — which cured the lake and
      // replaced the gradient with a hard edge running down the biome line (user: "can you blend this biome
      // transtion line better"). Keeping `shore` is what lets the dither go on doing its job on dry land.
      else if (shore && dm > 0.5) c = SAND[(sh * 3) | 0];
      // ── THE OAK FOREST FLOOR IS GREEN (user 2026-08-17) ── and it is MOSS, the shade the pine forest
      // already grows in patches, rather than a new ramp. Two reasons, and the second is the one that decided
      // it: a fresh 4-shade green would cost four more palette ids on a table with about twelve free and the
      // oak models themselves have just spent eight; and MOSS is already wired into every table a ground
      // surface has to be in — digOnlyTab (the shovel, and only the shovel, digs ground), decorTab, and the
      // snow/support classes. What separates the two biomes is COVERAGE, not hue: the pine floor is brown
      // needle litter with green patches (mossV > 0.52), the oak floor is green everywhere. Standing on the
      // border the change is unmistakable, and it cost nothing.
      // DITHERED against the mask itself, exactly as the desert branch above is, so the green thins out into
      // the litter across the whole rim instead of ending on a line.
      // ── THE BIRCH FLOOR IS THE LIGHTER GREEN ── ahead of the oak arm because the two masks are disjoint and
      // this one is cheaper to reject. Dithered against its own mask exactly as the oak and desert arms are, so
      // the green thins into the pine litter across the whole rim instead of ending on a line.
      else if (asn > 0.5) { c = ASNOW[(sh * 4) | 0]; }   // ── a dithered MISS inside the arctic is still arctic ── the branch above only fires on `ihash < am`, so the remainder used to fall through to the pine floor and grow GRASS on a glacier. Snow without setting surfMoss: white ground, no strands.
      else if (bm > 0 && ihash(wx * 11 + 23, wz * 13 + 41) < bm) { c = BIRCHMOSS[(sh * 4) | 0]; surfMoss = true; surfBirch = true; }
      else if (om > 0 && ihash(wx * 11 + 23, wz * 13 + 41) < om) { c = MOSS[(sh * 4) | 0]; surfMoss = true; }
      else { c = (mossy ? MOSS : NEEDLE)[(sh * 4) | 0]; surfMoss = mossy; }
      W[base + (h - 1) * WX] = c;
    }
    if (lake) for (let y = h; y <= Math.min(WL, yTop - 1); y++) W[base + y * WX] = y === WL ? WATER_T : WATER_B;
    // ── SNOW CAPS ON ARCTIC WATER ── the surface voxel becomes packed snow instead of water, so a cap sits
    // FLUSH with the lake rather than as a lip on top of it, and the water body underneath is untouched (the
    // WATER_B column below still fills, so depth, refraction and the fish all still see a lake). Coherent
    // noise, so these read as floes rather than as speckle, and gated on the SNOW mask so the caps stop where
    // the snow does instead of at the band's edge. Rivers get them on the same line — a river column is a lake
    // column by the time it reaches here.
    if (!fresh) { let ia = base + (lake ? WL + 1 : h) * WX; for (let y = lake ? WL + 1 : h; y < WY; y++) { W[ia] = 0; ia += WX; } }
    // ── SNOW CAPS ON ARCTIC WATER ── floes of packed snow on lakes and rivers. The surface water voxel becomes
    // snow and the cap is then built UPWARD from it, so a floe has thickness rather than being a painted-on
    // sheet (user 2026-08-30: "give the snow caps more depth. they look just flat. make them have height").
    // The water body underneath is untouched — the WATER_B column still fills, so depth, refraction and the
    // fish all still see a lake.
    // ── IT MUST RUN AFTER THE !fresh CLEAR ABOVE, AND THAT IS THE WHOLE BUG THE FIRST VERSION HAD ── that loop
    // wipes a lake column from WL + 1 to the world ceiling. A one-voxel cap written at WL survived it by
    // sitting exactly one below the floor, which is why the flat version worked; every voxel of height added
    // above it would have been erased on any column that was not freshly generated. Written here instead, the
    // cap is placed after the clear and nothing takes it away.
    // ── THICKNESS COMES FROM THE FLOE FIELD ITSELF ── height rises with how far the noise clears the
    // threshold, so a floe is thickest at its middle and tapers to one voxel at its rim. That is a dome rather
    // than a slab, and it means the same field decides both where a floe is and how deep it is, so the two can
    // never disagree about its edge.
    if (lake && asn > 0.5 && WL <= yTop - 1) {
      const fl = fbm(wx * ARCT_FLOEF + 71.3, wz * ARCT_FLOEF + 12.9);
      const flT = ARCT_FLOE - (riverS(wx, wz) > 0.02 ? ARCT_FLOE_RIV : 0);   // a RIVER column ices over more readily than a lake — see ARCT_FLOE_RIV. 0.02 is the same rs the H carve treats as "a river is here"
      if (fl > flT) {
        // ── A TRUE HEMISPHERE, NOT JUST A CONCAVE RAMP ── t is how far in from the floe's rim this column
        // is (0 at the edge, 1 at the middle), so the radius from the centre is (1 - t) and a round cap is
        // sqrt(1 - (1-t)^2) — the circle. The previous sqrt(t) was concave and read as domed, but it is not
        // actually a sphere's profile and it rose too fast right at the rim, which is the part that reads as
        // a wall rather than a curve. Same field, same edge, no extra sampling.
        const ft = Math.min(1, (fl - flT) / ARCT_FLOESPAN);
        const capH = 1 + Math.round(ARCT_FLOEH * Math.sqrt(Math.max(0, ft * (2 - ft))));   // measured from THIS column's own threshold, so river ice is as deep as lake ice rather than uniformly thicker
        for (let y = WL; y < Math.min(WY - 1, WL + capH); y++) W[base + y * WX] = ASNOW[(ihash(wx * 7 + 3 + y, wz * 11 + 5) * 4) | 0];
      }
    }
    // ── THE ARCTIC GROWS NOTHING AT ALL, NOT EVEN SNOW (user 2026-08-30: "you seemed to have placed singular
    // voxels on top of the terrain? can you remove it to make the terrain more smooth") ── a SASTRUGI pass sat
    // here: one snow voxel on 3% of arctic columns, borrowed from the grass strands below, whose job was to
    // break up the contour rings that bare quantised ground shows (the long note is in world/window.js). It
    // worked on the rings and it lost on its own terms — a snow nub is a full white cube throwing a hard blue
    // shadow onto white, so scattered single voxels read as debris on ground that is supposed to be smooth.
    // THE RINGS COME BACK WITH IT, and that is the accepted trade, not an oversight. If they are ever worth
    // attacking again, do it somewhere other than the height field and other than here: window.js records four
    // attempts (±1.28 dither, ±0.45 sub-voxel, ±1.5 drifts, and the ground ramp alone) and why each failed.
    if (surfMoss && h + 4 < WY) {                      // GRASS STRANDS: moss patches ONLY, 1–4 voxels tall, moss-matched colors
      if (ihash(wx * 3 + 41, wz * 3 + 87) < 0.06) {
        const gh = 1 + ((ihash(wx * 5 + 3, wz * 7 + 9) * 4) | 0);
        const gc = (surfBirch ? BIRCHSTRAND : GRASS)[(ihash(wx + 13, wz * 13) * 4) | 0];   // ── STRANDS ARE A SCATTER RAMP, AND THE BIRCH HAS ITS OWN ── they used to take BIRCHMOSS in the birch band, and that ramp is the solid GROUND colour now (see material-tabs.js): sharing ids would have made every blade of grass a hitbox, where the pine forest's strands are walk-through. GRASS is >= DECOR_MIN and in floatTab, which is exactly the surface-scatter class a strand wants. The floor keeps its lighter colour; only the blades on top of it stay the standard green   // the birch forest's strands are its own lighter green — the whole point of the request
        for (let k = 0; k < gh; k++) { const ii = base + (h + k) * WX; if (W[ii]) break; W[ii] = gc; }
      }
    }
    // ── NO FLOWERS IN THE BLOSSOM (user 2026-08-18: "remove all of the flowers") ── the cherry forest's ground
    // colour is fallen petals instead, and those are dropped by the trees themselves in stampOak rather than
    // sown here, because "near trees" is the whole point of them. A meadow flower and a fallen petal in the
    // same square metre would read as neither.
    // ── FALLEN PETALS (user 2026-08-18: "scatter pink voxels on the ground near trees") ── placed HERE, in the
    // per-column pass, and not scattered from stampOak the way the first cut did it. That version cost a
    // MEASURED 2.65-SECOND FREEZE the first time the cherry forest generated, and the reason is material class,
    // not count: it wrote the canopy's BLOSLEAF ids, which are foliaTab, so every petal was an isolated
    // ORPHAN_OK:false voxel and the generation orphan sweep had to flood from each one to prove it was anchored.
    // Thousands per region. With ?nopetal the same walk peaked at 50 ms.
    // The flowers have always done this job cheaply, so the petals now do it the same way: one voxel, laid in
    // fillColumn, wearing an id that is FLOAT-tab ground scatter rather than canopy. TWIGPINK is the authored
    // pink_stick leaf ramp, already marked floatTab in assets/material-tabs.js, so this costs no palette id and
    // lands the petals in the right material class by construction.
    // "Near trees" is satisfied by the biome rather than by a proximity probe: the blossom band is 78% tree
    // cells, so uniform-within-cherry IS under the canopy, and a per-column proximity walk is exactly the cost
    // this rewrite exists to remove.
    // ── AND THEY FALL UNDER THE TREES, NOT OVER THE WHOLE BAND (user 2026-08-18: "only have the single dropped
    // pink petals underneath the trees. not scattered around everywhere") ── the first cut leaned on the biome
    // instead of on a proximity test, reasoning that the blossom band is 78% tree CELLS so uniform-within-cherry
    // is effectively under the canopy. Cells are not canopy: a cell is 79 voxels and a crown is a good deal
    // narrower, so the open ground between trees got the same litter as the ground beneath them, and it read as
    // pink confetti over a meadow rather than as something the trees had dropped.
    // The test is the crown's own FOOTPRINT, from the same oakAt the stamp uses, so a petal lands exactly where
    // a crown will be. A 3x3 cell walk covers it: OKCELL is 79 and the widest crown's half-footprint is inside
    // OKMARGIN 60, so no tree outside those nine cells can reach this column.
    // COST: the walk is behind the rate roll, so it runs on ~1.5% of blossom columns rather than all of them —
    // fillColumn is the hottest loop in worldgen and this is the same shape as the chNear guard above it.
    // 0.004 -> 0.015 because the eligible area shrank by roughly that factor; the litter under a crown stays as
    // dense as it was, and the meadow between crowns is now clean.
    const petalUnderTree = () => {
      const cx9 = Math.floor(wx / OKCELL), cz9 = Math.floor(wz / OKCELL);
      for (let a = -1; a <= 1; a++) for (let b = -1; b <= 1; b++) {
        const t9 = oakAt(cx9 + a, cz9 + b);
        if (!t9 || !t9.blos) continue;                 // only a BLOSSOM crown drops blossom
        const m9 = OAKV[t9.k]; if (!m9) continue;
        const fw = (t9.rot & 1) ? m9.sy : m9.sx, fd = (t9.rot & 1) ? m9.sx : m9.sy;
        if (Math.abs(wx - t9.wx) <= fw * 0.42 && Math.abs(wz - t9.wz) <= fd * 0.42) return t9;   // the TREE, not just true: the scatter now has to know which VARIETY shed on this column (user 2026-08-19)   // 0.42 of the footprint, not 0.5: the bounding box includes the crown's sparse outermost voxels, and litter at the very rim reads as scatter again
      }
      return null;
    };
    if (PETAL_ON && !lake && h > WL + 2 && h + 2 < WY && cm > 0.5 && typeof TWIGPINK !== 'undefined' && TWIGPINK.length
        && ihash(wx * 7 + 53, wz * 11 + 17) < 0.015) {   // 0.02 -> 0.004, just under the FLOWERS' own 0.005. The rate is the whole cost: every scattered voxel is ORPHAN_OK ground litter, so the generation orphan sweep floods from each one to prove it is anchored, and at 2% of columns that was ~12,000 floods per region and a 1.3 s hitch the first time the blossom generated
      const t9p = petalUnderTree();
      // ── THE PETAL MATCHES THE CROWN IT FELL OFF ── `wht` is the same flag stampOak reads to decide which
      // ramp the canopy itself wears, so the ground and the tree above it cannot disagree. TWIGWHITE falls
      // back to TWIGPINK if the pale ids were never minted (a full palette), which keeps this a colour
      // question rather than a reason for the scatter to vanish.
      const ramp9 = (t9p && t9p.wht && typeof TWIGWHITE !== 'undefined' && TWIGWHITE.length) ? TWIGWHITE : TWIGPINK;
      const s9 = base + h * WX;
      if (t9p && !W[s9]) W[s9] = ramp9[(ihash(wx * 19 + 5, wz * 23 + 9) * ramp9.length) | 0];
    }
    // ── THE FLOWERS USED TO BE SOWN HERE, ONE VOXEL AT A TIME ── a grass stem at h and a BLOOM head at h+1, at
    // 0.005 per column. They are an authored five-variant MODEL now (user 2026-08-18: "replace all the flowers
    // in the current game with flowers.vox"), and a model cannot be stamped from fillColumn: stampModel needs
    // the region bounds this per-column function does not have. So they moved to a cell pass beside the
    // mushrooms and the sticks — see FLWCELL / flowerAt / stampFlower below. The biome gates travelled with
    // them unchanged (no beaches, no water, not the desert, not the blossom band).
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
    if (ihash(cx * 29 + 7, cz * 31 + 3) > 0.375) return null;   // the much larger rocks are much rarer   // -25% (user 2026-09-01: "reduce the rocks by 25%") 0.5 -> 0.375: ihash is uniform on 0..1, so the threshold IS the acceptance rate and the arithmetic is its own verification
    const bx = Math.round(cx * BCELL + 4 + ihash(cx * 3 + 40, cz * 7 + 90) * (BCELL - 8));
    const bz = Math.round(cz * BCELL + 4 + ihash(cx * 13 + 6, cz * 5 + 44) * (BCELL - 8));
    // ── AND NONE OF THE FOUR TIERS IN THE ARCTIC (user 2026-08-29: "remove the rocks and twigs") ── unlike the
    // desert, which keeps tier 0 because the hand stone reads as scattered field stone on sand, ALL of them go
    // here: these models carry MOSS on their crowns, so every boulder on the snow was a green cap on a white
    // field. The bare rock the arctic does want is the nunatak, and that comes from the SURFACE arm in
    // fillColumn where a steep face exposes stone — terrain, not a scattered model.
    if (arcticM(bx, bz) > ARCT_BARE) return null;   // dithered, not cut — see treeAt
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
    // ── AND A QUARTER FEWER IN THE OAK FOREST (user 2026-08-17: "reduce the rocks by 25%. (oak forest)") ──
    // the desert cut two lines above is the pattern: a biome rate cut is its own roll on its own salt, so it
    // thins the field independently instead of re-cutting the tier order and shifting which tier a surviving
    // cell draws. It applies to ALL FOUR tiers because "the rocks" in an oak wood means every rock in it —
    // unlike the desert, where the same user request was about the boulders only and tier 0 had its own line.
    // ── THE CUT WAS TAKEN TWICE (user 2026-08-17: "reduce the rocks by 25%", then "make the rocks -25% less
    // rare again") ── 0.75 kept three candidates in four; 0.5625 is that same quarter taken off what was
    // left, so the oak forest now carries 0.75 * 0.75 = 56.25% of the rock density the pine forest does.
    // ONE threshold on ONE salt rather than two chained rolls: chaining would be the same field thinned
    // twice, which is identical in expectation but re-draws which particular rocks survive, and the user is
    // looking at a world they have already walked around in.
    // The mask is tested AFTER the salt, not before it, purely for cost: oakM is 6 vnoise where this whole
    // function is otherwise a handful of hashes, and boulderAt runs on every 34-voxel cell in the world plus
    // ~16 more probes per mushroom site. Same rejections, a quarter of the mask evaluations.
    // ── AND THE HAND STONE GETS A QUARTER BACK (user 2026-08-20: "increase the amount of hand held rocks in
    // the oak/cherry forest by 25%") ── 0.5625 -> 0.703125 for TIER 0 ONLY, which is rock.vox: the small
    // pickable field stone, and the only tier crafting can actually use. The three boulder tiers keep 0.5625,
    // because the two cuts above were about landmarks and nothing has asked for more of those.
    // THE SAME SALT AND THE SAME DRAW, with only the threshold moved. Raising a threshold on an existing roll
    // is a SUPERSET: every stone that stood there yesterday still stands there and new ones appear between
    // them. Rolling a second, independent draw for the increase would have re-decided the whole field and
    // moved rocks the player has already walked past, which is the thing this function's other notes keep
    // guarding against.
    // The blossom band needs no test of its own — it sits well inside oakM > 0.5, so "oak/cherry forest" is
    // exactly what this line already selects (the same reasoning flowerAt gives for `inCh`).
    if (ihash(cx * 61 + 43, cz * 67 + 29) > (size === 0 ? 0.703125 : 0.5625) && oakM(bx, bz) > 0.5) return null;
    // ── AND THE BIG ONES HALVED AGAIN ON TOP (user 2026-08-17: "reduce the large rocks in half") ──
    // ANSWERING THE QUESTION THAT CAME WITH IT: yes, the cut above already thins the large rocks, because
    // it is a flat rate on every tier — after it the oak forest carries 56.25% of the pine forest's big
    // rocks just as it carries 56.25% of its pebbles. This line is a SECOND, tier-specific cut on top, so
    // the two biggest tiers end at 28.1% of the pine forest's while the field stone and the small rocks
    // stay at 56.25%: the oak wood keeps its scatter and loses its landmarks.
    // size 2 and 3 are the mid and BIG rocks26 tiers (5.5% and 1.5% of candidates); its own salt again, so
    // it thins that population rather than re-drawing which tier a survivor belongs to.
    if (size >= 2 && ihash(cx * 71 + 17, cz * 73 + 41) > 0.5 && oakM(bx, bz) > 0.5) return null;
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
      // ── AND THE OAKS, WHICH THIS NEVER ASKED ABOUT (user 2026-08-18: "dont have trees and rocks collide") ──
      // the loop above probes treeAt, which is PINES, and treeAt returns null for every column where
      // oakM > 0.5. So in the oak forest and the blossom band inside it the clash test was not merely too
      // tight, it was INERT: it walked its cells, found nothing, and passed every candidate. Rocks stamp in
      // mode 2 (OVERWRITE, and 23 passes before the oaks), while stampOak writes in mode 1 and refuses cells
      // that already hold hard stone — so the rock was placed first and the crown was then carved away around
      // it. That is a boulder punched clean through a cherry canopy, which is exactly what the user photographed.
      // THE BERTH IS THE CROWN'S, NOT THE TRUNK'S. An oak's crown reaches 57 voxels from its centre at the
      // widest tier while a big rock is 37 — so the separation this needs is ~94, against the 45 the pine test
      // gives. Each tree is measured with its OWN model rather than a constant, the way stickAt does it, or
      // every rock would be pushed away from a sapling by a giant's radius.
      // ROCK-REFUSES-TREE is the right direction and not an arbitrary choice: rocks are placed first, so they
      // are the ones that can still yield, and oakAt is already walked per frame by buildCardCand — putting the
      // cost in the rarer pass is what keeps it off the frame budget.
      if (OAKV.length) {
        const reach = halfw + 57;                      // 57 = the widest crown's half-footprint; the per-tree test below is what actually decides
        for (let cz2 = Math.floor((bz - reach) / OKCELL); cz2 <= Math.floor((bz + reach) / OKCELL); cz2++)
          for (let cx2 = Math.floor((bx - reach) / OKCELL); cx2 <= Math.floor((bx + reach) / OKCELL); cx2++) {
            const t9 = oakAt(cx2, cz2); if (!t9) continue;
            const m9 = OAKV[t9.k]; if (!m9) continue;
            const need = halfw + (Math.max(m9.sx, m9.sy) >> 1);
            const dx9 = bx - t9.wx, dz9 = bz - t9.wz;
            if (dx9 * dx9 + dz9 * dz9 < need * need) return true;
          }
      }
      // ── AND THE BIRCHES, WHICH IS THE SAME OVERSIGHT ONE BIOME LATER (user 2026-08-26: "sometimes birch
      // trees spawn inside rocks") ── the two probes above are PINES and OAKS, and treeAt returns null for
      // every column where birchM > 0.5 (see its own guard). So in the birch band this test was inert in
      // exactly the way the oak note above describes: it walked its cells, found nothing, and passed every
      // candidate, and the rock — stamped in mode 2, OVERWRITE, and long before the trees — was punched
      // straight through the bole. birchAt opens with a hash that rejects 76% of cells and a band test right
      // after, so a rock anywhere else pays four cheap ops per cell for this.
      // THE BERTH IS THE BOLE'S, NOT THE CROWN'S, and that is the one way this differs from the oak arm above.
      // A rock standing under a birch canopy is fine and the stamp already handles it (mode 1 refuses cells
      // holding hard stone), so pushing rocks out by a 264-voxel model's half-footprint would strip the birch
      // forest of its boulders to fix something that was never wrong. What the report is about is stone
      // INSIDE the trunk, so the trunk is what gets the clearance.
      // AND IT MUST BE THE TRUNK'S OWN POSITION: on these models the bole sits up to 83 voxels from the
      // bounding-box centre (assets/bow.js birchTrunkC — a crown leans), which is why a test at t.wx/t.wz
      // would miss the very case being reported. Same helper the stamp and the perched-bird walk use, so
      // there is one answer in the game to "where does this birch actually stand". The cell walk is widened
      // by that same 83 or a tree whose box centre is outside the reach could still have its bole inside it.
      if (BIRCHV.length) {
        const reach = halfw + BK_BOLE + BK_LEAN;
        for (let cz2 = Math.floor((bz - reach) / BKCELL); cz2 <= Math.floor((bz + reach) / BKCELL); cz2++)
          for (let cx2 = Math.floor((bx - reach) / BKCELL); cx2 <= Math.floor((bx + reach) / BKCELL); cx2++) {
            const t9 = birchAt(cx2, cz2); if (!t9) continue;
            const m9 = BIRCHV[t9.k]; if (!m9) continue;
            const tw9 = birchTrunkW(t9, m9);           // the BOLE, not the box centre
            const need = halfw + BK_BOLE;
            const dx9 = bx - tw9.wx, dz9 = bz - tw9.wz;
            if (dx9 * dx9 + dz9 * dz9 < need * need) return true;
          }
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
    mossCap(m, b, gy, x0, x1, z0, z1);                 // …and a mossy cap on the sky-facing faces (see below)
  }
  // ── MOSS ON THE FOREST ROCKS (user 2026-08-18: "add moss to the rocks in the pine forest. the 26 rocks.
  // also do it the rocks in the oak forest") ── laid ON TOP of the rock as its own voxel rather than by
  // recolouring the rock's own, and that distinction is the whole design.
  //
  // WHY NOT REPAINT THE ROCK'S TOP VOXELS. A palette id is a MATERIAL. The rocks26 ids carry decorTab +
  // pickOnlyTab + rockShTab; the MOSS ids carry decorTab + digOnlyTab. digOnlyTab is tested BEFORE pickOnlyTab
  // in every tool gate (sim/tools.js), so repainting would make a PICK bounce off the green voxels and a
  // SHOVEL bounce off the grey ones — and worse, the okMat flood that decides a bite splits by material, so a
  // pick bite starting on stone would carve the rock out from under its own cap and leave a floating green
  // shell. The rock would also lose rockShTab (its sun sheen) and rockTopTab (the land-mammal seat test).
  //
  // WHY GRASS AND NOT MOSS. GRASS is already floatTab — surface scatter, SUP.DRAPE, walk-through, cuttable by
  // any tool, and already in projectiles.js's PASSTHRU — which is exactly what a cap wants: chop the rock and
  // the drape lifts with it instead of hanging in the air. MOSS is digOnly ground material and would bring the
  // tool-gate split back in through the side door. It also costs ZERO palette ids, and the table has none to
  // give (PAL_TOL is already at 12 to free the last few).
  //
  // NO BIOME TEST IS NEEDED, and that is not an oversight: boulderAt returns null for every rocks26 tier in
  // the desert (see `inDesert && size !== 0` above), so stampBoulder can only ever reach this in the pine and
  // oak forests — which is precisely the two the user asked for.
  //
  // The rotation math is stampModel's, verbatim, because the cap has to land on the ROTATED voxel: deriving it
  // any other way would put moss beside the rock at three of the four headings.
  function mossCap(m, b, gy, x0, x1, z0, z1) {
    if (!GRASS.length) return;
    // ── THE OAK FOREST'S MOSS IS THE OAK'S OWN LEAF COLOUR (user 2026-08-18) ── GRASS is the MOSS ramp and
    // reads far duller than the canopy above it (the brightest oak leaf is 50/255 from its nearest GRASS
    // shade). OAKMOSS is those leaf colours on float-material ids — see assets/palette.js for why they are not
    // simply OAKLEAF's own ids.
    // ONE mask sample per ROCK, not per voxel: this decides a whole boulder's cap, and the loop below runs over
    // every voxel of a model that can reach 56k of them.
    // ── AND THE BLOSSOM BAND'S IS PINK (user 2026-08-18: "make the moss in the cherry forest, pink on the
    // rocks ... just reuse pink slots") ── TWIGPINK, and it costs NOTHING: it is the authored pink stick-leaf
    // ramp, and material-tabs.js already flags every stick voxel id floatTab, which is the exact surface-scatter
    // class GRASS and OAKMOSS use. So it is the one pink in the table that is already the right MATERIAL — the
    // canopy pinks (BLOSLEAF/BLOSWHITE) are foliaTab and would make the cap see-through, the same reason
    // OAKMOSS could not simply borrow OAKLEAF's ids. The table is at 0 free, so reusing was the only option
    // anyway, and this one happens to be correct rather than merely affordable.
    // CHERRY IS TESTED FIRST because the blossom band is a SUB-REGION of the oak mask — asking oakM first would
    // hand every blossom rock the oak's green.
    const cap = (TWIGPINK.length && chNear(b.bx) && cherryM(b.bx, b.bz) > 0.5) ? TWIGPINK
              : (BIRCHMOSS.length && birchM(b.bx, b.bz) > 0.5) ? BIRCHMOSS   // ── AND THE BIRCH FOREST'S ROCK MOSS IS THE SAME LIGHT GREEN ── BIRCHMOSS now, not OAKMOSS: that ramp was reclaimed to DEADC when its biome went, so a rock in the birch wood was wearing a dead colour. OAKMOSS is exactly the oak's brightest leaf colours on float-material ids, so the rule the oak forest follows (moss matches the leaves over it) lands on the identical ramp here, for zero new ids
              : (OAKMOSS.length && oakM(b.bx, b.bz) > 0.5) ? OAKMOSS : GRASS;
    const fw = (b.rot & 1) ? m.sy : m.sx, fd = (b.rot & 1) ? m.sx : m.sy;
    const bx = b.bx - (fw >> 1), bz = b.bz - (fd >> 1);
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i];
      const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
      let rx, rz;
      if (b.rot === 0) { rx = x; rz = y; }
      else if (b.rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (b.rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      const ax = bx + rx, az = bz + rz;
      if (ax < x0 || ax >= x1 || az < z0 || az >= z1) continue;
      const ay = gy + z + 1; if (ay < 1 || ay >= WY) continue;   // the cell ABOVE this rock voxel
      const gx = gwrap(ax, WX), gz = gwrap(az, WZ);
      const ii = gx + ay * WX + gz * WX * WY;
      if (W[ii] !== 0) continue;                       // sky-facing only: anything occupied above means this is an interior or side voxel, not the top
      if (ihash(ax * 7 + 13, az * 11 + 29) >= 0.55) continue;   // patchy, not a lawn — 55% of the exposed top, on its own salt so it does not correlate with the rock's own tier roll
      W[ii] = cap[(ihash(ax * 3 + 1, az * 3 + 7) * cap.length) | 0];
    }
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
          if (H(lx, lz) <= WL + 4 || basinM(lx, lz) > 0.1) { dry = false; break; }
          // ── WHICH BIOMES REFUSE A GORGE ── one clause per band, tested on the same 9-point path walk with
          // the same rim offsets as the water tests above, because a biome has to reject the WHOLE gorge:
          // gating the carve instead would leave a canyon that stops dead on the biome line. Each is `> 0`
          // rather than `> 0.5` so a gorge cannot even reach a blend band.
          // READ THE THIRD CLAUSE BEFORE CHANGING ANYTHING: the world has exactly three bands, the two masks
          // are never both non-zero, and "neither of them" IS the pine forest — so with all three listed, NO
          // GORGE GENERATES ANYWHERE IN THE WORLD. That is what was asked for (user 2026-08-17: "remove the
          // ravines from the pine forest and the oak forest", the desert having already refused them on
          // 2026-08-16), and it is deliberately written as three refusals rather than as a deleted system:
          // caveAt/stampCave/nearCave/caveHitsBox and the orphan sweep are all intact, so handing a biome its
          // ravines back is deleting its one clause here, and a fourth band would start with them enabled.
          const dmL = desertM(lx, lz), omL = oakM(lx, lz);
          if (dmL > 0                                  // ── NOT IN THE DESERT (user 2026-08-16) ──
            || omL > 0                                 // ── NOR IN THE OAK FOREST (user 2026-08-17) ──
            || (dmL <= 0 && omL <= 0)) { dry = false; break; }   // ── NOR IN THE PINE FOREST (user 2026-08-17) ── every column that is neither of the above is pine, so this clause is the pine forest and it is unconditional
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
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    if (!FERN2V.length) return null;
    if (ihash(cx * 79 + 7, cz * 83 + 31) > 0.07) return null;   // halved 2026-07-16 (was 0.14)
    const wx = Math.round(cx * F2CELL + 3 + ihash(cx * 5 + 27, cz * 3 + 41) * (F2CELL - 6));
    const wz = Math.round(cz * F2CELL + 3 + ihash(cx * 11 + 33, cz * 7 + 15) * (F2CELL - 6));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── ferns are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (oakM(wx, wz) > 0.5) return null;      // ── NOR IN THE OAK FOREST (user 2026-08-17: "remove the ferns from the oak forest") ── the same halfway gate on the other border, so the fern field thins out across the rim instead of ending on the iso-line. DELIBERATE AND USER-DIRECTED, exactly like the mushroom gate a few passes down: it looks identical to the accidental biome exclusion that gate replaced, so do not "fix" it back.
    if (birchM(wx, wz) > 0.5) return null;    // ── NOR IN THE BIRCH FOREST (user 2026-08-24: "remove the ferns from the birch forest") ── the third border, on the same halfway test as the two above, so the fern field thins out across the rim instead of ending on the iso-line. USER-DIRECTED, like the oak gate above it: it reads exactly like an accidental biome exclusion and is not one.
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
    const dxs = wx - (SPWX + SPOX), dzs = wz - SPWZ;
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
    const dxs = wx - (SPWX + SPOX), dzs = wz - SPWZ;
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
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    if (!SHRUBV.length) return null;
    // 0.21, down from 0.28 (user 2026-08-16: "decrease the amount of shrubs by 25%"). ihash is uniform on
    // 0..1, so the threshold IS the acceptance rate and 0.28 * 0.75 = 0.21 is exactly a quarter fewer
    // candidate cells. The downstream rejections (cactus/rock clearance, water, the spawn clearing) scale
    // with it, so the planted count falls by the same quarter rather than by some other amount.
    if (ihash(cx * 67 + 23, cz * 71 + 11) > 0.158) return null;   // -25% again (user 2026-08-16): 0.21 -> 0.158, from 0.28 originally
    const wx = Math.round(cx * SHCELL + 4 + ihash(cx * 7 + 19, cz * 5 + 37) * (SHCELL - 8));
    const wz = Math.round(cz * SHCELL + 4 + ihash(cx * 13 + 31, cz * 11 + 3) * (SHCELL - 8));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (desertM(wx, wz) < 0.85) return null;
    if (H(wx, wz) <= WL + 4) return null;              // never in the water, and not on the beach ring either
    if (nearCave(wx, wz)) return null;
    const dxs = wx - (SPWX + SPOX), dzs = wz - SPWZ;
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
  // ── MEADOW FLOWERS (user 2026-08-18: "replace all the flowers in the current game with flowers.vox. make the
  // flowers 2x as rare") ── a cell pass, because a MODEL needs region bounds; the single-voxel version this
  // replaces lived in fillColumn. Same biome gates it had there, restated on the candidate's own column.
  //
  // THE RATE IS THE OLD ONE, HALVED, AND THAT IS ARITHMETIC RATHER THAN TASTE. The old test was one flower per
  // column at p = 0.005, i.e. 1 per 200 columns. FLWCELL 8 gives one candidate per 64 columns, so matching the
  // old density would need p = 0.005 * 64 = 0.32; half of that is 0.16, which is 1 flower per 400 columns.
  // FLWCELL is 8 and not larger because the cell also sets the MINIMUM SPACING: a 5x5 variant in a 4-wide cell
  // would overlap its neighbour, and much larger than 8 turns a uniform meadow into visible clumps-and-gaps.
  const FLWCELL = 8;
  // How many flower cells across one single-species PATCH is. 12 cells = 96 voxels, which at the rate above
  // holds ~11 flowers — enough to read as "a patch of roses" rather than as two that happen to match. The patch
  // edges are straight, and that is invisible at this density: nothing draws the boundary, ~11 scattered plants
  // do, so what the eye gets is a drift of one colour into another rather than a tile.
  const FLWPATCH = 12;
  function flowerAt(cx, cz) {
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    if (!FLOWERV || !FLOWERV.length) return null;
    if (ihash(cx * 37 + 11, cz * 41 + 29) > 0.08) return null;   // see the rate note above — 0.08 of cells, a QUARTER of the old per-column density
    const wx = Math.round(cx * FLWCELL + 1 + ihash(cx * 7 + 13, cz * 5 + 3) * (FLWCELL - 2));
    const wz = Math.round(cz * FLWCELL + 1 + ihash(cx * 3 + 19, cz * 11 + 23) * (FLWCELL - 2));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (desertM(wx, wz) > 0.5) return null;            // the old gate: dm < 0.5
    // ── THE BLOSSOM BAND GETS ITS OWN FLOWER RATHER THAN NONE (user 2026-08-18: "have them follow the same
    // flower mechanics as the oak forest flowers ... this is all taking place in the cherry forest") ── it used
    // to be refused here, on the argument that a meadow flower and a fallen petal in the same square metre read
    // as neither. The answer turned out to be a flower that belongs: the white variant recoloured pink
    // (assets/bow.js FLOWERV_CH). Everything else about the scatter is identical — same cell, same rate, same
    // patch machinery — so the band gets the same drifts the oak forest does, in its own colour.
    const inCh = chNear(wx) && cherryM(wx, wz) > 0.5;
    if (inCh && !FLOWERV_CH.length) return null;       // the derivation failed (no white variant found) — refuse rather than plant an oak-forest flower in the blossom
    const h = H(wx, wz);
    if (h <= WL + 4 || h + 3 >= WY) return null;       // no flowers in water, on beaches, or against the sky ceiling
    if (nearCave(wx, wz)) return null;
    // ── THE VARIETY COMES FROM A COARSE PATCH CELL, NOT THIS ONE (user 2026-08-18: "make the flowers stick with
    // their respective colors ... patches of roses or patches of yellow flowers") ── drawing k on the flower's
    // OWN cell gives every neighbour an independent variety, which is the confetti this replaces. Keying it on
    // a cell FLWPATCH times coarser makes every flower inside one patch the same plant, and the patch is where
    // the variety changes. Its own salt, so a patch's colour is not tied to which cells inside it happen to
    // carry a flower.
    // Math.floor, NOT `/ FLWPATCH | 0`: cell coordinates go negative (the world is centred far from 0) and a
    // bitwise truncation rounds toward zero, which would mirror the patch grid about the origin and put a seam
    // through it. floor is uniform across the sign change.
    // ── AND HALF AGAIN IN THE PINE FOREST (user 2026-08-18: "reduce the amount of flowers in the pine forest
    // in half", then "decrease the flowers in the pine forest by 25%") ── a SECOND, independent draw rather than a
    // separate rate constant, so the two densities stay in a stated ratio: everywhere else keeps the rate at the
    // top of this function and the pine forest gets 0.375 of it. Its own salt, or it would correlate with the
    // placement draw and thin precisely the cells that were already sparse.
    //
    // ── THIS BLOCK SPENT A DAY IN treeAt (fixed 2026-08-19: user "double the density of the pine forest? it
    // seems more sparse now?") ── word for word, comment and all, it was written into the PINE spawn instead of
    // the flower one. So the flowers the user asked to halve were never touched, and the pine forest was quietly
    // cut to 0.375 of its trees — which is exactly the sparseness they then reported. The two functions are
    // shaped alike (cell hash, jittered wx/wz, a run of biome refusals, a return), which is how it went unseen;
    // the load-bearing difference is that treeAt returns a TREE. Anything of the form "less X in the pine
    // forest" belongs beside X's own rate, and X here is the flower.
    //
    // oakM < 0.5 is the pine forest by the same halfway line every other pine/oak split here uses, and the
    // desert has already been refused by the mask test above, so this cannot catch it too. The blossom band
    // sits well inside oakM > 0.5, so it keeps the full rate and `inCh` needs no test of its own.
    // ihash FIRST, oakM second: `&&` is left-to-right and both are pure, so the order cannot change the answer,
    // but it decides how often the expensive one runs. oakM is ~7 vnoise; this way the 0.625 the cheap draw
    // rejects never pay for it, and flower cells are FLWCELL = 8 apart — dense enough for that to matter.
    // ── THE BIRCH FOREST KEEPS THE OAK'S FLOWER RATE (user: "copy how the flowers work in the oak forest
    // inside of the birch forest") ── this line is the oak's denser meadow: everywhere ELSE only 37.5% of
    // candidates survive, so the pine forest gets a scattering and the oak gets nearly three times as many.
    // The birch band read as "not oak" and was thinned with the pines - measured, oak placed on 6.99% of cells
    // against birch's 2.26%, and 6.99 x 0.375 = 2.62 is exactly that thinning rather than anything else going
    // wrong. Taking the max is the same move oakRoll/oakBank make for the ground itself: the band is an oak
    // forest that grows birches, so anything keyed on "is this the oak forest" has to say yes here too.
    if (ihash(cx * 71 + 5, cz * 79 + 13) > 0.375 && Math.max(oakM(wx, wz), birchM(wx, wz)) < 0.5) return null;
    const px = Math.floor(cx / FLWPATCH), pz = Math.floor(cz / FLWPATCH);
    const set = inCh ? FLOWERV_CH : FLOWERV;           // the blossom band draws from its own one-variant set; everywhere else from the five
    return { wx, wz, ch: inCh ? 1 : 0, k: (ihash(px * 53 + 7, pz * 59 + 17) * (set.length - 0.01)) | 0,
             rot: (ihash(cx * 61 + 31, cz * 67 + 43) * 3.99) | 0 };   // rotation stays per-FLOWER: a patch of one species should not be a patch of one pose
  }
  function stampFlower(m, x0, x1, z0, z1) {
    // mode 1 = empty cells + soft decor, the same mode the mushrooms and the oaks use: a flower grows THROUGH
    // the grass already standing in its column instead of leaving a bare square around itself.
    // ── SEATED ON ITS OWN COLUMN, NOT ON A MINIMUM OVER ITS NEIGHBOURS (user 2026-08-18: "sometimes the rose
    // seems to be one voxel too low in the ground. both of its green voxels need to be showing") ── it was
    // groundMin(wx, wz, 2), which takes the LOWEST of five columns probed at +/-2. Every flower's occupied
    // footprint is 3x3 and its stem is the CENTRE column alone, so those probes sample ground the model never
    // touches: on any slope with a one-voxel fall within two voxels the seat dropped by one, the z=0 green
    // landed inside the surface voxel, and stampModel's mode-1 gate (`cur !== 0 && cur < DECOR_MIN`) refused
    // it. One green showing instead of two, and only on slopes — hence "sometimes".
    // H(wx, wz) is the first EMPTY y of the stem's own column, so z=0 lands ON the surface and z=1 above it.
    // It is also the same function flowerAt already gates the candidate on, so placement and seat now agree
    // rather than being decided by two different measurements.
    stampModel((m.ch ? FLOWERV_CH : FLOWERV)[m.k], m.rot, m.wx, H(m.wx, m.wz), m.wz, x0, x1, z0, z1, 1);
  }
  const MUCELL = 52;                                   // MUSHROOMS: a rare cluster, ONLY in the pine forest (a pine must be within crown reach), one candidate per 5.2 m cell
  function mushAt(cx, cz) {
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    if (!MUSHV) return null;
    if (ihash(cx * 89 + 17, cz * 97 + 5) > 0.0398) return null;   // rare — ~4.0% of candidate cells before the pine-forest + rock-clearance gates below (user 2026-09-01: "decrease the spawnrate of the mushroom by 25%", 0.053 -> 0.0398; and cut by a third from 0.08 before that, user)
    const wx = Math.round(cx * MUCELL + 5 + ihash(cx * 5 + 23, cz * 3 + 9) * (MUCELL - 10));
    const wz = Math.round(cz * MUCELL + 5 + ihash(cx * 11 + 7, cz * 7 + 19) * (MUCELL - 10));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── mushrooms are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    // ── NOR IN THE OAK FOREST, AND THIS IS DELIBERATE — DO NOT "FIX" IT (user 2026-08-17: "remove the red
    // mushroom for the oak forest") ── mushrooms were taught about oaks EARLIER THE SAME DAY, by adding an
    // oakAt proximity loop beside the pine one below so that `nearPine` meant "am I in a forest" rather than
    // "is there a pine here". The user then asked for the opposite, so that loop is gone and mushrooms are
    // pine-forest litter again. It looks exactly like the bug it was written to fix — a gate that rejects
    // every candidate in a whole biome — so it is stated twice: this explicit mask test, which is the one
    // that carries the user's decision, and the pine-only `nearPine` loop below, which would achieve it on
    // its own (treeAt returns null everywhere oakM > 0.5, so no candidate in the oak forest can find a pine).
    // The explicit test is the load-bearing one: it survives anyone later relaxing the tree gate.
    // Same halfway point and same reason as the desert line above — the litter thins across the rim.
    if (oakM(wx, wz) > 0.5) return null;
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
    if (!nearPine) return null;                        // no pine within crown reach → not pine-forest floor → no mushrooms
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
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    if (!CONEVL) return null;
    if (ihash(cx * 71 + 13, cz * 73 + 29) > 0.22) return null;
    const wx = Math.round(cx * PCCELL + 3 + ihash(cx * 7 + 9, cz * 5 + 3) * (PCCELL - 6));
    const wz = Math.round(cz * PCCELL + 3 + ihash(cx * 3 + 17, cz * 11 + 8) * (PCCELL - 6));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (oakM(wx, wz) > 0.5) return null;      // ── NOR IN THE OAK FOREST ── a cone on the ground is a PINE cone: it is the one piece of forest litter that names the tree it fell from, and there are no pines here to have dropped it. (The cones hung IN a crown need no gate — stampTree hangs those, and stampTree no longer runs in this biome.)
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── pinecones are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (birchM(wx, wz) > 0.5) return null;    // ── NOR IN THE BIRCH FOREST (user 2026-08-24: "there seems to be pine cones in the birch. remove them as well") ── the same reason as the oak gate above: a cone on the ground is a PINE cone and no pine dropped it here. birchAt stamps the birches, not stampTree, so nothing hangs cones in these crowns either.
    if (H(wx, wz) <= WL + 4) return null;              // cones stay off beaches and lakebeds
    if (nearCave(wx, wz)) return null;
    return { wx, wz, rot: (ihash(cx + 19, cz + 23) * 3.99) | 0 };
  }
  function stampPcone(p, x0, x1, z0, z1) {
    stampModel(CONEVL, p.rot, p.wx, groundMin(p.wx, p.wz, 2), p.wz, x0, x1, z0, z1, 1);   // lying on its side, hugging the ground like the sticks
  }
  const SCELL = 14;                                    // STICKS: stick_1 / stick_2 .vox models, pickable, scattered on the forest floor
  function stickAt(cx, cz) {
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    if (!STICKV.length) return null;
    if (ihash(cx * 61 + 7, cz * 67 + 19) > 0.42) return null;
    const wx = Math.round(cx * SCELL + 2 + ihash(cx * 3 + 8, cz * 5 + 4) * (SCELL - 4));
    const wz = Math.round(cz * SCELL + 2 + ihash(cx * 7 + 2, cz * 9 + 6) * (SCELL - 4));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── twigs are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    // ── IN THE OAK FOREST, ONLY UNDER A TREE (user 2026-08-17: "only have stick near oak trees", and
    // explicitly "keep the pine forest the same") ── a twig is something a tree DROPPED, and the oak wood has
    // wide clearings between its crowns where a stick lying in open meadow reads as litter rather than as
    // deadfall. So here the twig has to be under a canopy. The PINE forest keeps its blanket scatter exactly:
    // this whole test is behind the oak mask, so `oakM <= 0.5` skips it and nothing about the pines changes.
    // 64 voxels is the widest oak's half-footprint (114 across) plus a little, so "near" means "under or just
    // beyond the crown", not "within sight of".
    if (oakM(wx, wz) > 0.5) {
      let nearOak = false;
      for (let cz2 = Math.floor((wz - 64) / OKCELL); cz2 <= Math.floor((wz + 64) / OKCELL) && !nearOak; cz2++)
        for (let cx2 = Math.floor((wx - 64) / OKCELL); cx2 <= Math.floor((wx + 64) / OKCELL); cx2++) {
          const t2 = oakAt(cx2, cz2); if (!t2) continue;
          const rr = (Math.max(OAKV[t2.k].sx, OAKV[t2.k].sy) >> 1) + 8;   // that tree's OWN crown radius, so a bush drops twigs in a small ring and a giant in a wide one
          const dx2 = wx - t2.wx, dz2 = wz - t2.wz;
          if (dx2 * dx2 + dz2 * dz2 < rr * rr) { nearOak = true; break; }
        }
      if (!nearOak) return null;
    }
    if (H(wx, wz) <= WL + 4) return null;              // sticks stay off the beach
    if (nearCave(wx, wz)) return null;
    return { wx, wz, m: ihash(cx * 11 + 3, cz * 13 + 5) < 0.5 ? 0 : STICKV.length - 1, b: chNear(wx) && cherryM(wx, wz) > 0.5, bk: birchM(wx, wz) > 0.5, rot: (ihash(cx + 4, cz + 7) * 3.99) | 0 };   // b = this twig fell off a cherry tree, so its leaf is pink
  }
  function stampStick(s, x0, x1, z0, z1) {
    // STICKB is the same two twigs with the leaf recoloured (assets/bow.js) — identical geometry, so the
    // seating, the pickup flood and the float table all behave the same. s.b is decided in stickAt, where the
    // biome is already being sampled, rather than here, so the stamp stays a pure function of the candidate.
    // birch first: the two masks are disjoint (a whole pine strip sits between the bands), so the order
    // between them cannot matter, but it keeps the cheapest test in front.
    stampModel((s.bk && STICKBIRCH.length ? STICKBIRCH : (s.b && STICKB.length ? STICKB : STICKV))[s.m], s.rot, s.wx, groundMin(s.wx, s.wz, 2), s.wz, x0, x1, z0, z1, 1);
  }
  const LGCELL = 96;                                   // FALLEN LOG (log.vox): one candidate per 9.6 m cell, 14% kept — rare, solid, walkable
  function logAt(cx, cz) {
    // ── NOTHING GROWS ON THE ICE ── the arctic is the one biome with no ground cover at all: the reference
    // photographs are snow, ice and bare rock, and a twig or a toadstool on a glacier reads as a bug rather
    // than as sparse planting. Gated at the cell centre, the same > 0.5 halfway test treeAt uses, so the
    // litter thins out across the blend band instead of stopping on the iso-line.
    // ── THE FALLEN LOGS ARE GONE (user 2026-09-01: "remove the logs on the field") ── every one of them,
    // everywhere, not one more biome carved off the front: the line below already read "remove the logs in
    // the birch forest", and the desert and the arctic had taken their own bites before that, so what was
    // left was the pine forest and this removes that too.
    // Wiped as an early return rather than by deleting logAt/stampLog, which is how birchM and arcticM were
    // wiped in world/window.js and for the same reason: stampCellsGen names both functions, so do both gen
    // worker registries and the debug taps, and a rename sweep across four files is a different change from
    // this one — it would bury a one-line decision in a diff nobody can read. Everything below is dead and
    // kept only so the placement rule is still legible if these ever come back.
    return null;
    if (!LOGV) return null;
    if (ihash(cx * 71 + 13, cz * 73 + 29) > 0.14) return null;
    const wx = Math.round(cx * LGCELL + 6 + ihash(cx * 3 + 9, cz * 7 + 1) * (LGCELL - 12));
    const wz = Math.round(cz * LGCELL + 6 + ihash(cx * 11 + 4, cz * 13 + 2) * (LGCELL - 12));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── fallen logs are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (birchM(wx, wz) > 0.5) return null;   // ── NOR IN THE BIRCH FOREST (user: "remove the logs in the birch forest") ── the same > 0.5 halfway test the desert line above uses, and the same one treeAt takes
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
    if (ihash(cx * 83 + 3, cz * 89 + 17) > 0.0428) return null;   // -25% (user 2026-09-01: "decrease lillypads by 25%") 0.057 -> 0.0428   // halved twice 2026-07-16 (0.34 → 0.17 → 0.085), then CUT BY 1/3 (0.085 → 0.057, user 2026-07-18)
    const wx = Math.round(cx * LILYCELL + 3 + ihash(cx * 7 + 6, cz * 3 + 14) * (LILYCELL - 6));
    const wz = Math.round(cz * LILYCELL + 3 + ihash(cx * 5 + 12, cz * 11 + 7) * (LILYCELL - 6));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (H(wx, wz) > WL - 1) return null;               // ≥ 2 voxels of water under the pad — real lakes/rivers, never the beach film
    const sr = ihash(cx * 19 + 8, cz * 23 + 4);
    return { wx, wz, size: Math.min(LILYV.length - 1, sr < 0.55 ? 0 : (sr < 0.88 ? 1 : 2)), rot: (ihash(cx + 31, cz + 42) * 3.99) | 0 };
  }
  function stampLily(f, x0, x1, z0, z1) {
    stampModel(LILYV[f.size], f.rot, f.wx, WL + 1, f.wz, x0, x1, z0, z1, 4);   // rests ON the water surface; mode 4 clips every column to open water
  }
  const LGIGCELL = 128;                                // GIGANTIC LILYPADS: 1-2 per lake — one candidate per 12.8 m cell, only on WIDE open lake water
  function lilyGigAt(cx, cz) {
    // ── NO LILY PADS ON A FROZEN LAKE (user 2026-08-29) ── the arctic has water like anywhere else, and
    // the pads were floating on it. Dithered like every other arctic gate so the pads thin out toward the
    // border rather than ending on a line.
    if (!LILYPAD_GIGV) return null;
    if (ihash(cx * 53 + 11, cz * 59 + 7) > 0.1375) return null;   // HALVED AGAIN (user 2026-07-18): 0.55 -> 0.275 -> 0.1375
    const wx = Math.round(cx * LGIGCELL + 22 + ihash(cx * 3 + 5, cz * 7 + 9) * (LGIGCELL - 44));
    const wz = Math.round(cz * LGIGCELL + 22 + ihash(cx * 11 + 3, cz * 5 + 13) * (LGIGCELL - 44));
    // ── TESTED AT THE OBJECT, NOT AT ITS CELL (user 2026-08-29: "the life is still rendering in the
    // arctic. investigate this deeply") ── this gate used to read arcticM at `cx * CELL`, the cell's own
    // CORNER, while the thing it is gating is placed at a jittered point somewhere INSIDE that cell. The
    // cells here run tens to hundreds of voxels, so a corner outside the band admits an object that lands
    // well inside it — which is exactly how a mushroom, and rock and soil from the log models, ended up on
    // open snow at mask 1.0. treeAt and boulderAt never had the bug because they always tested the trunk's
    // own position; these nine were written from the cell index and inherited it. Measured before the fix:
    // ~25 stray voxels in 40401 arctic columns, all of them past the 0.15 line.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    if (H(wx, wz) > WL - 2) return null;               // ≥ 3 voxels of water under the centre
    for (let k = 0; k < 8; k++) { const a = k * 0.7854, dx = Math.round(Math.cos(a) * 24), dz = Math.round(Math.sin(a) * 24);
      if (H(wx + dx, wz + dz) > WL - 1) return null; } // a 24-vox-radius ring must ALL be water → a real LAKE (not a river/puddle) → naturally ~1-2 pads per lake
    return { wx, wz, rot: (ihash(cx * 2 + 1, cz * 2 + 3) * 3.99) | 0 };
  }
  function stampLilyGig(g, x0, x1, z0, z1) {
    stampModel(LILYPAD_GIGV, g.rot, g.wx, WL + 1, g.wz, x0, x1, z0, z1, 4);   // floats on the lake surface; mode 4 clips each column to open water so it fits the lake outline
  }
  // -- THE OAKS (user 2026-08-17: "spread the trees around the oak forest biome") -- the oak forest's only
  // tree pass, and the exact counterpart of treeAt/stampTree below: one candidate per cell, jittered inside
  // it, gated on the biome mask, kept out of water and gorges and off the spawn.
  //
  // WHY 112 AND NOT THE PINES' 45. An oak is not a pine-shaped object. The widest of these crowns is 118
  // voxels across against pine5's 35, and the biggest is 86,080 voxels against the pine's 8,440 - ten times
  // the model at three times the width. Cell size is therefore doing two jobs at once, and they happen to
  // want the same number: 11.2 m spacing is what a closed broadleaf canopy actually looks like (crowns
  // touching, trunks not), and it is also what keeps the STAMP COST in the same league as the pine pass it
  // sits beside. Against the size mix below that is ~1.8 voxels written per world column, where the pine
  // pass writes ~3.0. Halve this cell and the generator writes four times as much oak per column, which is
  // the one way this feature could quietly cost the whole game its frame budget.
  //
  // THE SIZE MIX IS WEIGHTED, NOT UNIFORM, and it is weighted DOWN. OAKV is height-sorted by the voxelizer
  // (0 = the 2.4 m bush, 6 = the 11.7 m oak), and a flat roll over seven models would make the two giants
  // 29% of every tree - 76k voxels a throw. The ramp below spends its picks on the cheap end while still
  // putting a full-grown oak in most views: an even split by CANOPY LAYER (underbrush / young / mature /
  // giant) rather than by model, which is also how a real oak wood is stocked.
  // ── DENSITY DOUBLED (user 2026-08-17) ── and it is the CELL that moved, not the pass rate: candidates
  // per unit area go as 1/OKCELL^2, so 112 -> 79 is 112^2/79^2 = 2.01x, where raising the 0.78 roll could
  // only ever have bought 28% before it saturated. 7.9 m spacing against crowns up to 11.4 m wide means
  // the canopy now genuinely closes - which is the point - and stampOak's mode 1 simply lets neighbouring
  // crowns interleave rather than fight.
  // THE COST IS LINEAR IN THIS AND IT IS THE ONE THING TO WATCH: the pass writes ~2x the voxels per world
  // column it did (about 3.6 against the pine pass's 3.0), all of it in the gen workers.
  const OKCELL = 79, OKMARGIN = 60;                    // one oak candidate per 7.9 m cell; margin covers the widest crown's half-footprint (118 across -> 59), so a tree centred just outside a region still stamps the half of it that falls inside. RE-CHECK THIS IF THE BAKE GETS BIGGER - a model wider than 2*OKMARGIN silently loses its outer courses at every region seam, and the seam is exactly where nobody looks.
  // ── DOUBLED, 0.10 -> 0.20 (user 2026-08-17: "I don't see fruit trees very often") ── and the reason it
  // read as rarer than 10% is worth writing down, because the two numbers are not the same thing:
  // this is the share of ELIGIBLE oaks, and the two BUSH tiers are excluded (a berry bush already carries
  // fruit), so 22% of every oak you walk past can never qualify. 0.10 of the remaining 78% was 7.8% of all
  // oaks — about 1 in 13 — which is what the eye was reporting. Measured in-game before this change: 7 of
  // 124 oaks, 5.6%.
  // 0.10 -> 0.20 read as TOO MANY (user, same session), so it settles at 0.15: 11.7% of all oaks,
  // about 1 in 9 — half again as common as it started and a third down from the doubling.
  // Kept as a share of the ELIGIBLE trees rather than
  // re-based onto all oaks, so the bush exclusion stays visible in the number instead of being buried.
  const OKFRUIT = 0.15;                                // share of oak TREES (the two bush tiers excluded — they are the berry bushes) that carry fruit
  const OKHIVE = 0.06;                                 // …and of the mature+giant layers that carry a beehive: 6% of the 52% of oaks that reach those layers = 3.1% of all oaks
  const OKVIEW_W = 22;                                 // spawn sight-line half-width for an oak, against the pine's 13: a crown carries 59 voxels of half-footprint, so a trunk cleared at the pine's tolerance still hangs its canopy over the whole view
  function oakAt(cx, cz) {
    if (!OAKV.length) return null;                     // ?nooaks, or the .json never loaded - one test disables the whole pass
    if (ihash(cx * 41 + 19, cz * 37 + 7) > 0.78) return null;
    const wx = Math.round(cx * OKCELL + 10 + ihash(cx * 5 + 3, cz * 7 + 11) * (OKCELL - 20));
    const wz = Math.round(cz * OKCELL + 10 + ihash(cx * 11 + 9, cz * 13 + 5) * (OKCELL - 20));
    if (oakM(wx, wz) < 0.5) return null;               // -- OAK FOREST ONLY -- the mirror of the pines' own test at the same halfway point, so the two canopies thin into each other across the rim instead of both ending on one line
    // -- THE SPAWN CLEARING, WIDENED FOR A BROADLEAF -- the same two tests treeAt runs (a circle, then a
    // corridor down SPYAW), with both radii scaled to the tree. 40 voxels rather than 26: the player now
    // starts INSIDE this biome rather than beside it, so this is the clearing they actually stand in, and an
    // oak trunk 2.6 m away puts bark across a third of the screen. Derived from SPYAW rather than assuming
    // +X, so re-baking the spawn heading moves the corridor with it.
    const dxs = wx - (SPWX + SPOX), dzs = wz - SPWZ;
    if (dxs * dxs + dzs * dzs < 40 * 40) return null;
    const fwdX = Math.sin(SPYAW), fwdZ = Math.cos(SPYAW);
    const along = dxs * fwdX + dzs * fwdZ;
    if (along > 0 && along < SPVIEW_D && Math.abs(dxs * fwdZ - dzs * fwdX) < OKVIEW_W) return null;
    if (H(wx, wz) <= WL + 4) return null;              // no oaks in water or on a beach - the same line the pines use
    if (nearCave(wx, wz)) return null;
    const sr = ihash(cx * 17 + 23, cz * 19 + 31);
    // ── THE BUSH TIER IS TWO MODELS NOW, AND EVERY INDEX ABOVE IT MOVED UP ONE ── assets/bow.js splices the
    // 2.4 m bush into a CHERRY bush and a BLUEBERRY bush and splices both into OAKV in its place, so the bake's
    // seven models are eight and the plain berryless bush does not exist anywhere in the world any more (user
    // 2026-08-17: "there shouldn't be one without any berries"). The four canopy layers and their weights are
    // unchanged - only the indices they name. 50/50 on its OWN salt rather than reusing sr, because sr has
    // already been spent deciding the layer and a second read of it would correlate the two.
    const k = sr < 0.22 ? (ihash(cx * 23 + 5, cz * 29 + 3) < 0.5 ? 0 : 1)         // 22% underbrush - the 2.4 m bush, half cherry and half blueberry
            : sr < 0.48 ? 2 + ((ihash(cx * 3 + 71, cz * 5 + 13) * 1.99) | 0)      // 26% young, ~5 m
            : sr < 0.80 ? 4 + ((ihash(cx * 7 + 29, cz * 3 + 61) * 1.99) | 0)      // 32% mature, 7-9 m
            : 6 + ((ihash(cx * 13 + 47, cz * 11 + 19) * 1.99) | 0);               // 20% giant, 11 m
    // ── THE CHERRY FOREST'S TREES ARE THESE TREES (user 2026-08-18: "use the same oak trees, except make the
    // leaves pink") ── so this is a FLAG on the oak, not a second scatter pass. The cherry band sits inside
    // oakM by construction (see cherryM in world/window.js), which means oakAt already runs there and already
    // returns the right trees at the right density; all that changes is which model array stampOak reads and
    // whether fruit is hung. A separate pass would have had to duplicate the size ladder, the spawn clearing,
    // the anchors and the stick/rock proximity probes, and would have broken the "the two tree passes never
    // meet" claim that lets oakAt and treeAt be ordered freely.
    const blos = chNear(wx) && cherryM(wx, wz) > 0.5;   // chNear first: oakAt is NOT worldgen-only — buildCardCand (main/tick-nav.js) walks a 29x29 block of it EVERY FRAME, so an unguarded mask here was ~5,000 vnoise a frame spent answering "no"
    // …and the BUSH TIERS ARE SKIPPED IN BLOSSOM (user: "dont spawn apples or oranges in the trees"). Tiers 0
    // and 1 are not plain underbrush — they are the CHERRY and BLUEBERRY bushes, 22% of every oak in the world,
    // and they carry berries by construction rather than by the fruit roll below. Turning off the roll alone
    // would still have left a fifth of the cherry forest as fruiting shrubs. Re-rolled into the young tier on
    // its own salt so the tier mix stays a partition and the blossom wood is not simply 22% emptier.
    const k2 = blos && k < 2 ? 2 + ((ihash(cx * 31 + 13, cz * 17 + 7) * 1.99) | 0) : k;
    // ── A QUARTER OF THE BLOSSOM IS WHITE (user 2026-08-18: "make half the cherry trees have white petals
    // instead of pink", then "make the white cherry trees at 25% instead of 50%") ── a SECOND flag rather than a tri-state on `blos`, because everything else in the game that asks
    // about these trees is asking "is this a cherry tree" and must keep getting yes for both varieties:
    // cherryOak() refuses the songbird perches on them, the fruit roll below is gated on !t.blos, and the bush
    // tiers are re-rolled above. Only the STAMP cares which variety it is.
    // Its own salt, and one no other decision here uses: sharing k2's or rot's would tie a tree's colour to its
    // size or its facing, and a forest where every big tree is white is not a mix, it is a pattern.
    // Keyed on the CELL (cx, cz) like every other per-tree draw, so a tree keeps its colour across regeneration
    // and across the worker/main-thread split — the same reason rot is.
    const wht = blos && ihash(cx * 43 + 91, cz * 29 + 67) < 0.25;   // 0.5 -> 0.25 (user 2026-08-18): a QUARTER white, so pink still reads as the forest's colour and white as the variety in it
    // ── AND THE GREEN OAKS COME IN TWO SHADES (user 2026-08-19: "make the oak trees have 2 shades of green. a
    // lighter green and a darker green. similar to whats done with the cherry trees") ── the cherry parallel is
    // exact and deliberate: this is the same arrangement `wht` is, one flag deciding which of two remapped model
    // sets stampOak reads, with the geometry, the anchors, the footprint, the fruit and the hive untouched. So
    // everything that probes an oak — the rock clash, the stick proximity, the bird perches, the fell — cannot
    // tell the varieties apart, which is the property that makes this a colour change and not a second tree.
    // Light is a VARIETY inside a dark-green forest exactly as white is one inside a pink forest, so dark stays
    // the wood's colour and light is the thing you notice in it. It shipped at 50/50 on the reading that the
    // user had asked for the oaks to HAVE two shades — neither the exception — and the follow-ups settled it
    // the other way, first at a quarter and then at a quarter OF WHAT.
    // A QUARTER OF ALL OAKS, WHICH IS NOT 0.25 HERE (user 2026-08-19: "I want 25% of all oak trees to be
    // lighter", revising the same day's "make the light tree oak variants at 25% of the total oak trees", which
    // shipped as a quarter of the population this draw can reach). `!blos` has already spent the blossom band
    // BEFORE the roll, so a light oak can only ever come out of the GREEN oaks — a bare 0.25 buys a quarter of a
    // sub-population, and measured that way it was 16.46% of the world's oaks. The compensation is exact and it
    // is one division:  p = 0.25 / (1 - blossomShare).
    // THE BLOSSOM SHARE IS MEASURED, and re-measured rather than inherited, because it is a property of the
    // biome strips and those move — all six are 2160 wide today (world/window.js, BIOP = 12960) and the number
    // moved with them, from 34.05% to 33.262% over 1,358,483 oakAt candidates. So p = 0.25 / 0.66738 = 0.3746;
    // 0.379, the value the earlier strip widths implied, would land at 25.29% of all oaks.
    // SAMPLE IN WHOLE BIOP PERIODS — that is the trap in measuring it at all. The strips repeat every BIOP in x,
    // so any window narrower than one period reads a single PHASE of the arrangement: a first pass over
    // half-period windows read 32.0% with a 30.4-34.3% spread between anchors, which is noise about the wrong
    // centre. Eight blocks six periods wide, spread from z -880,000 to +1,140,000, read 33.05-33.51%. The same
    // pass reads white at 25.04% of blossom, which is this line's control, and light at 24.97% of green, which
    // says the salt does not correlate with oakAt's own acceptance rolls. Re-cut the bands and this constant is
    // wrong: re-measure and re-divide it, do not nudge it.
    // THE TWO FIGURES ARE FAR APART AND BOTH ARE WORTH KNOWING: at 0.3746 the light variety is a quarter of ALL
    // oaks and ~37.5% of the GREEN ones, so the oak forest proper reads as better than a third light while the
    // world reads as a quarter. That is the request, and it is what it costs for the blossom band to be a third
    // of every oak rather than the thin strip it looks like on a map.
    // NOT IN THE BLOSSOM: `!blos` keeps the cherry band on exactly the trees it had. A blossom oak's leaf ids
    // are already remapped to pink or white, and a second remap on top of that is meaningless — the light green
    // is a variety of the GREEN oak, the way white is a variety of the pink one.
    // Its own salt, and one nothing else in this function uses: sharing k2's would tie a tree's colour to its
    // size, rot's to its facing, and the fruit roll's would make every light oak an apple tree. Keyed on the
    // CELL like every other per-tree draw, so a tree keeps its shade across regeneration and across the
    // worker/main-thread split.
    const lite = !blos && OAKLITEV.length > 0 && ihash(cx * 97 + 13, cz * 101 + 59) < 0.3746;   // 0.5 -> 0.25 -> 0.3746 (user 2026-08-19): a quarter of ALL oaks wear the LIGHT ramp (assets/bow.js OAKLITER), which over a population that is 33.262% blossom is 0.25 / (1 - 0.33262) of the GREEN ones — see the arithmetic above. Not a taste value: change the blossom share and this must be divided again
    const t = { wx, wz, k: Math.min(OAKV.length - 1, k2), blos, wht, lite, rot: (ihash(cx + 137, cz + 89) * 3.99) | 0,
                sink: 1 + ((ihash(cx * 19, cz * 23) * 3) | 0) };   // sink 1-3, and it means something DIFFERENT here than it does for a pine: stampOak writes in mode 1, so every course below the local ground is refused rather than punched into the hill. The sink only decides how many base courses are hidden.
    // ── FRUIT (user 2026-08-17: "pick trees at random and place apples and oranges in the trees ... make 10% of
    // oak trees have some fruit it in") ── one SPECIES per tree, because an apple tree is an apple tree, and a
    // count that comes off the crown's own footprint the way birdsOnOak does rather than a constant: 3 on a young
    // oak, 9 on a giant. THE BUSH TIERS ARE EXCLUDED and that is the point of them - a berry bush already carries
    // fruit, and hanging a 30 cm apple in a 2.4 m shrub reads as litter. So the 10% is 10% of the oak TREES.
    if (!t.blos && t.k >= 2 && FRUITV.length && OAK_ANCH[t.k] && OAK_ANCH[t.k].length &&
        ihash(cx * 53 + 7, cz * 59 + 13) < OKFRUIT) {   // …and NO FRUIT IN THE BLOSSOM (user 2026-08-18). t.fn stays undefined and stampOak's whole fruit block is already guarded on it, so this one term is the entire switch — and it also keeps the fruit's LEAF out, which wears an OAKLEAF id and would have hung green in a pink crown
      const fm = OAKV[t.k];
      t.fk = ihash(cx * 61 + 17, cz * 43 + 29) < 0.5 ? 0 : 1;                     // 0 = apple, 1 = orange (FRUITV's own order, which tools/voxelize_fruit.py fixes)
      // ── DOUBLED (user 2026-08-17: "double the rate of the apples in the bigger trees. As in a single tree
      // should carry more apples") ── the x2 wraps the WHOLE expression rather than being folded into the
      // divisor, so the size ramp keeps its shape and every tier simply doubles: 6 / 6 / 8 / 10 / 16 / 18
      // against the old 3 / 3 / 4 / 5 / 8 / 9. Folding it into the /2000 instead would have flattened the
      // ramp's low end (a young oak would have gained as much as a giant), and the request is explicitly
      // about the BIGGER trees carrying more.
      // The cap moves 10 -> 20 with it, or the two giants would both clip to 10 and the doubling would
      // land on every tier EXCEPT the ones it was asked for. OAK_ANCH is capped at 96 anchors per model,
      // so 18 fruit still draws from a pool five times its size and cannot run short.
      t.fn = Math.min(20, 2 * (3 + (((fm.sx * fm.sy) / 2000) | 0)));
    }
    // ── AND A BEEHIVE, WHICH IS A LANDMARK RATHER THAN SCATTER (user 2026-08-17) ── rarer than the fruit and
    // restricted to the two biggest canopy layers: a 50 cm box needs a crown big enough to hold it without
    // becoming the tree. 6% of the mature-and-giant layers is 3.1% of all oaks, which at OKCELL 79 and the 78%
    // pass rate is about one hive per 51 m square - roughly two inside the default draw radius, so it is
    // findable without being furniture. The Y IS RESOLVED HERE, not at stamp time, because hiveAt() is the
    // query the bee swarm reads and it has to answer with a world position; groundMin is only paid on the 3%
    // that actually carry one. The hive hangs from a BRANCH (OAK_BANCH, bark), never from leaves - see the
    // support argument in assets/material-tabs.js.
    // ── BEEHIVES DO HANG IN THE BLOSSOM (user 2026-08-18, reversing the same day's removal) ── they were briefly
    // gated off here on the argument that the bees are BIO_OAKF and refused at the spawn gate, so the nest would
    // be an empty one. The user wants the nest regardless, so the gate is gone rather than left as a dead `true`.
    // If you want bees in it too, the lever is the BIO_ANY exclusion in main/tick-creatures.js, not this line.
    if (t.k >= 4 && HIVEV && OAK_BANCH[t.k] && OAK_BANCH[t.k].length &&
        ihash(cx * 71 + 29, cz * 67 + 41) < OKHIVE) {
      const m = OAKV[t.k], B = OAK_BANCH[t.k];
      const a = B[((ihash(cx * 83 + 11, cz * 79 + 37) * (B.length - 0.01)) | 0)];
      const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
      let rx, rz;                                                                 // stampModel's rotation, verbatim - see the cone block in stampTree
      if (t.rot === 0) { rx = ax; rz = ay; }
      else if (t.rot === 1) { rx = m.sy - 1 - ay; rz = ax; }
      else if (t.rot === 2) { rx = m.sx - 1 - ax; rz = m.sy - 1 - ay; }
      else { rx = ay; rz = m.sx - 1 - ax; }
      const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
      const hx = t.wx - (fw >> 1) + rx, hz = t.wz - (fd >> 1) + rz;
      const hy = groundMin(t.wx, t.wz, 4) - t.sink + az - HIVEV.sz;               // …so the hive's TOP course lands one below the branch it hangs from: the model's top-centre voxel (2,2,4) is occupied and stampModel anchors bottom-CENTRE, so that voxel is face-adjacent to the bark whatever the rotation
      t.hv = { wx: hx, wy: hy + (HIVEV.sz >> 1), wz: hz,                          // CENTRE of the box - the point a swarm orbits
               bx: hx, by: hy, bz: hz,                                            // …and the stampModel anchor (bottom-CENTRE column) it is actually written at
               sx: HIVEV.sx, sy: HIVEV.sy, sz: HIVEV.sz,
               tx: t.wx, tz: t.wz, k: t.k, rot: t.rot };
    }
    return t;
  }
  // ── WHERE ARE THE BEEHIVES ── the pure query the BEES read, shaped exactly like oakAt: same OKCELL grid, same
  // (cx, cz) cell coordinates, hash-driven, no state and no cache, safe to call from the main thread, a gen
  // worker or a test. It is a projection of oakAt rather than a second scatter, because a hive is a property OF
  // an oak: two independent rolls could put a hive where no tree stands. Scan it the way stampCellsGen scans
  // oakAt - Math.floor((w - OKMARGIN) / OKCELL) to Math.floor((w + OKMARGIN) / OKCELL) - and every hive in a
  // region comes back. Returns null, or { wx, wy, wz (the box's CENTRE), bx, by, bz (its stamp anchor),
  // sx, sy, sz (the model box), tx, tz (the oak's trunk column), k (size tier), rot }.
  // ── NO PERCHED SONGBIRDS IN THE BLOSSOM (user 2026-08-18) ── the perch candidates come from the two TREE
  // grids, not from the life spawn gate: buildCardCand (main/tick-nav.js) walks oakAt and treeAt and hangs birds
  // on whatever it finds, and it is the one life path that never reaches the biome test in tick-creatures.js.
  // So the refusal has to live where the perches are counted. cherryOak answers "is this oak a blossom one",
  // and the bird count for such a tree is zero — which starves findPineCrown of candidates and leaves the slot
  // unplaced, exactly as standing in the desert already does.
  // A separate helper rather than a term inside oakAt: oakAt must keep returning the tree (the rocks, the
  // sticks and the fell all probe it), and only the BIRD question changes.
  function cherryOak(t) { return !!(t && t.blos); }
  // ── ...AND THE BIRCH BAND'S HIVES ANSWER THE SAME QUERY (user 2026-08-24) ── the bee swarm walks a 3x3 of
  // OAK cells (BEE_HIVE_Q in sim/life/slots.js), and a birch hive lives on the 44-voxel grid, so one oak cell
  // covers a few birch cells and they have to be enumerated rather than indexed. Guarded on the mask first:
  // outside the birch band that test is one cheap call and the walk never runs, so the oak forest pays
  // nothing for this.
  function hiveAt(cx, cz) {
    const t = oakAt(cx, cz); if (t && t.hv) return t.hv;
    const mx = cx * OKCELL + (OKCELL >> 1), mz = cz * OKCELL + (OKCELL >> 1);
    if (birchM(mx, mz) < 0.5) return null;
    const b0x = Math.floor(cx * OKCELL / BKCELL), b1x = Math.floor(((cx + 1) * OKCELL - 1) / BKCELL);
    const b0z = Math.floor(cz * OKCELL / BKCELL), b1z = Math.floor(((cz + 1) * OKCELL - 1) / BKCELL);
    for (let bz = b0z; bz <= b1z; bz++) for (let bx = b0x; bx <= b1x; bx++) {
      const b = birchAt(bx, bz); if (b && b.hv) return b.hv;
    }
    return null;
  }
  function stampOak(t, x0, x1, z0, z1) {
    const gy = groundMin(t.wx, t.wz, 4) - t.sink;
    // OAKBLOSV is the same models with every leaf voxel run through the pink ramp, and OAKWHITV through the
    // white one (assets/bow.js blosRemap; the gen workers rebuild their own copies). A leaf's GREEN picks a band
    // of the ramp and the voxel's own position picks within it, so a crown wears the whole ramp rather than one
    // shade per source green. Geometry, anchors and footprint are untouched — so everything that probes an oak
    // (rock clash, stick proximity, the bird perches, the fell) is unaffected.
    // Each falls back to the next set down if its own failed to build, so a missing derived set degrades to pink
    // and then to green rather than to a crash — OAKV[t.k] is always valid because every set maps OAKV in place.
    // …and OAKLITEV is the GREEN forest's second shade, built through the same blosRemap off OAKLITER (user
    // 2026-08-19). The `t.blos` arm is untouched, so the cherry forest stamps exactly the models it stamped
    // before: a blossom oak never reaches the light-green arm, because oakAt refuses `lite` when `blos` is set.
    const oakSet = t.blos ? (t.wht && OAKWHITV.length ? OAKWHITV : (OAKBLOSV.length ? OAKBLOSV : OAKV))
                          : (t.lite && OAKLITEV.length ? OAKLITEV : OAKV);
    stampModel(oakSet[t.k], t.rot, t.wx, gy, t.wz, x0, x1, z0, z1, 1);   // mode 1 = empty cells + soft decor: the crown grows through the ferns and grass instead of leaving a hole where one would have been, and the buried trunk courses are clipped by the terrain instead of carving it. groundMin over a 4-voxel radius seats the tree on the LOW side of a slope, so no oak stands on a stalk.
    if (t.fn) {                                        // FRUIT — hung UNDER canopy anchors exactly as stampTree hangs its pinecones, and rotated with the tree so every region stamps them identically
      const m = OAKV[t.k], A = OAK_ANCH[t.k], F = FRUITV[t.fk];
      const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
      const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
      const used = new Set();                          // one fruit per column — never stacked
      for (let j = 0; j < t.fn; j++) {                 // OAK_ANCH is angle-sorted: the j-th fruit comes out of the j-th angular sector, so a tree's crop rings the crown instead of clumping
        const a = A[(((j + 0.15 + ihash(t.wx * 13 + j * 29, t.wz * 17 + j * 31) * 0.7) / t.fn) * A.length) | 0];
        const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
        let rx, rz;
        if (t.rot === 0) { rx = ax; rz = ay; }
        else if (t.rot === 1) { rx = m.sy - 1 - ay; rz = ax; }
        else if (t.rot === 2) { rx = m.sx - 1 - ax; rz = m.sy - 1 - ay; }
        else { rx = ay; rz = m.sx - 1 - ax; }
        const ck = rx | (rz << 8); if (used.has(ck)) continue; used.add(ck);
        // Mode 1, not the cone's mode 0: a fruit is a 3-voxel ball and mode 0 would let one leaf in the way take
        // a bite out of it. It cannot eat BARK either way — stampModel's mode-1 test is `cur < DECOR_MIN`, and
        // the oaks' bark ids are the pine's, which are below that line.
        // The model's top-centre voxel is (2,1,4) on both fruit, so with wy = gy + az - F.sz it lands within one
        // column of the anchor leaf at every rotation — face-adjacent at rot 0/1 and diagonal at 2/3, and the
        // drape flood in sim/support-rules.js is 26-connected precisely so a diagonal counts as attached.
        stampModel(F, (ax + az + j) & 3, bx + rx, gy + az - F.sz, bz + rz, x0, x1, z0, z1, 1);
      }
    }
    if (t.hv) stampModel(HIVEV, t.rot, t.hv.bx, t.hv.by, t.hv.bz, x0, x1, z0, z1, 1);   // the BEEHIVE, at the world anchor oakAt already resolved
  }
  // ── PINE DENSITY (user 2026-08-19: "double the density of the pine forest? it seems more sparse now?") ──
  // THE DOUBLING IS THE BUG FIX ABOVE, NOT THIS NUMBER. The sparseness was real and it was one day old: a
  // "reduce the flowers in the pine forest" edit had been written into treeAt instead of flowerAt (see the note
  // there), so every pine was drawn against a 0.375 gate meant for meadow flowers. Removing it takes a
  // 1024 × 1024 patch of pine forest from 117 trees back to 284 — 2.43×, which is the doubling that was asked
  // for, and is also exactly the forest that shipped before that edit. TCELL is therefore UNCHANGED at 45.
  //
  // If a future ask really is "denser than it has ever been", this is the dial, and the arithmetic is not the
  // obvious one: TCELL is a cell EDGE while density is per unit AREA, so doubling divides it by √2, not by 2.
  // 45 → 32 is 45² / 32² = 1.98× on top of the 2.43× above; it was built and measured (573 trees, 4.90×) and
  // costs only +0.2 ms a frame, because by then the canopy is closed and the extra trunks are hidden behind the
  // ones in front. It is a WALL: the floor, and every flower, mushroom, fern and rock on it, stops being
  // visible. 45 → 22 would be 4× again and is never the right reading of "double".
  //
  // TMARGIN is NOT a function of TCELL and did not move. A candidate is jittered 6 voxels inside its own cell,
  // so the widest thing a cell can stamp overhangs the cell by the same amount at any cell size: pine5.vox is
  // 35 × 36 × 116 → a half-footprint of 18, less the 6 inset = 12, and the cones hung off the crown's outer
  // anchors add ~4. 24 clears both. (Compare OKMARGIN = 60 for the oaks, whose widest crown is 114 across —
  // a model wider than 2 × margin silently loses its outer courses at every region seam, and that is the
  // failure this number exists to stay ahead of.)
  //
  // Every OTHER reader of TCELL — the boulder, fern, mushroom and treesInRegion scans here, plus physics.js,
  // tick-nav.js, tick-creatures.js and debug-api.js — walks `Math.floor((w ± R) / TCELL)` with R in WORLD
  // voxels. A candidate lives inside its own cell, so a cell wholly outside [w - R, w + R] cannot hold one
  // within R: those scans are exact at any cell size. None of them needed touching; they simply walk more cells.
  const TCELL = 45, TMARGIN = 34;                      // one pine candidate per 4.5 m cell
  // ── TMARGIN IS THE OVERHANG ALLOWANCE, AND THE NINE PINES ARE WIDER ── it bounds how far a stamp may
  // reach outside the cell that owns it, so a region sweep knows how far to look for trees that overlap it.
  // pine5.vox was 35 across (17 from centre) and 24 covered it; the widest of the nine is 67 (34 from
  // centre). Left at 24 the sweep misses trees whose crowns reach into the region, and they stamp as
  // half-trees clipped on the region boundary.
  const SPVIEW_D = 96, SPVIEW_W = 13;                  // spawn sight-line: 9.6 m ahead, 1.3 m either side of the view axis
  const BKCELL = 44, BKMARGIN = 128;
  // The bole's own half-width plus a gap, and how far a bole can sit from its model's bounding-box centre.
  // Both are read by the birch arm of boulderAt's treeClash — see the note there for why the berth is the
  // trunk's and not the crown's. 83 is assets/bow.js birchTrunkC's worst case across the loaded set.
  const BK_BOLE = 7, BK_LEAN = 83;
  const BK_SPAWN = 44;                                 // the spawn clearing measured at the BOLE — a little wider than the 40 the box test uses, so the trunk's own width cannot creep back in
  const BKHIVE = 0.01;                                 // one birch cell in a HUNDRED carries a beehive (user 2026-08-24: 0.10 -> 0.05 -> 0.02; 2026-08-26: "make birch 1%"). A landmark rather than furniture, which is the same reasoning OKHIVE's 3.1% of oaks follows                    // one birch candidate per 4.4 m cell; margin covers the widest crown's half-footprint
  function birchAt(cx, cz) {
    if (!BIRCHV.length) return null;                   // ?nobirch, or the .json never loaded - one test disables the whole pass, as ?nooaks does
    if (ihash(cx * 53 + 29, cz * 47 + 11) > 0.76) return null;         // ~24% of cells carry a tree
    const wx = Math.round(cx * BKCELL + 12 + ihash(cx * 7 + 5, cz * 9 + 13) * (BKCELL - 24));
    const wz = Math.round(cz * BKCELL + 12 + ihash(cx * 13 + 7, cz * 11 + 3) * (BKCELL - 24));
    if (birchM(wx, wz) < 0.5) return null;             // BIRCH BAND ONLY - the mirror of the pines' test at the same halfway point
    // ── AND NOT INTO THE ARCTIC (user 2026-08-30: "the birch trees are too far into the arctic. push them
    // back") ── this was the ONE tree pass that never got the arctic gate every other placer here carries,
    // and the band arithmetic is why it mattered: birch and arctic share a boundary, birch plants at
    // birchM >= 0.5, and the arctic's blend is 900 wide against birch's 450 — so at the shared line the
    // arctic mask is already 0.5 and climbing while birch is still planting. Birches stood a third of the
    // way into the snow. ARCT_BARE, the same line every other tree stops at.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    // ── SPWX + SPOX, NOT SPWX (user 2026-08-28: "prevent the player from spawning in a tree") ── every
    // clearing in this file used to measure from SPWX alone, and SPWX is the BAND ANCHOR, not the player.
    // The player stands at SPWX + SPOX and SPOX is -2160 (world/build.js), so all seven clearings were being
    // carved 2160 voxels east of anybody — outside the 2048-wide streaming window entirely, i.e. a hole in
    // terrain that is never generated, protecting nobody. Measured before the fix, over 8 fresh worlds: the
    // nearest birch trunk came in at 4.1 voxels twice, against the 40 this line asks for.
    // Every other clearing in this file carries the same correction for the same reason; the split between
    // the two numbers is explained at SPOX in world/window.js.
    const dxs = wx - (SPWX + SPOX), dzs = wz - SPWZ;   // the spawn clearing and the sight-line corridor, at the oak's broadleaf radii: the player now spawns IN this biome
    if (dxs * dxs + dzs * dzs < 40 * 40) return null;
    const fwdX = Math.sin(SPYAW), fwdZ = Math.cos(SPYAW);
    const along = dxs * fwdX + dzs * fwdZ;
    if (along > 0 && along < SPVIEW_D && Math.abs(dxs * fwdZ - dzs * fwdX) < OKVIEW_W) return null;
    if (H(wx, wz) <= WL + 4) return null;              // no birches in water or on a beach
    if (nearCave(wx, wz)) return null;
    // -- THE TREE MUST FIT UNDER THE SKY, and this is the guard core/gpu.js promises when it explains why the
    // rung ladder puts window width ahead of world height. The world is 504 tall for these trees, but on a
    // machine that only binds 384 - or simply on high ground - a 264-voxel birch would have its crown clipped
    // by the ay >= WY test in the stamp, silently, and a decapitated tree looks like a bug rather than a
    // fallback. BIRCHV is sorted short-to-tall by the bake, so the fitting models are a PREFIX and the pick is
    // one walk down from the top. If not even the smallest fits, this column grows nothing.
    // (the previous build refused a birch whose bole landed inside a boulder, via rockClashCell. That helper
    // is part of the rock-clash work and is not in this build, so the test is not made here. A birch and a
    // rock can therefore share a cell; the stamp writes in mode 1 and refuses cells that already hold hard
    // stone, so the result is a tree growing past a rock rather than through it.)
    const avail = Math.min(CANOPY, WY - 2 - H(wx, wz));   // CANOPY as well as the ceiling: a model taller than the brick sky-cap would be stamped and then never drawn (world/window.js)
    if (BIRCHV[0].sz > avail) return null;
    // BIRCHPICK, not BIRCHV: the same models, repeated in proportion to how full their crowns are, so the
    // spindly half of the set stops dominating a stand. Sorted by height like BIRCHV, so the fits-under-the-
    // sky walk is still a prefix. See birchPick in assets/bow.js.
    let top = BIRCHPICK.length - 1;
    while (top > 0 && BIRCHV[BIRCHPICK[top]].sz > avail) top--;
    const k = BIRCHPICK[(ihash(cx * 17 + 3, cz * 19 + 7) * (top + 0.999)) | 0];
    const t = { wx, wz, k, rot: (ihash(cx + 211, cz + 97) * 3.99) | 0,
                sink: 1 + ((ihash(cx * 23, cz * 29) * 3) | 0) };
    // ── AND THE SPAWN CLEARING IS MEASURED AT THE BOLE (user 2026-08-26: "never spawn players in trees") ──
    // the test near the top of this function compares SPAWN against wx/wz, which is the model's BOUNDING-BOX
    // CENTRE. On these models the bole is up to BK_LEAN (83) from that centre, because the crowns lean, so a
    // birch whose box passed the 40-voxel clearing could still put its TRUNK on top of the player. MEASURED
    // on a fresh world: three bark voxels inside the player's own collision box at spawn, nearest trunk 15
    // voxels away. Same lesson the boulder arm records — anything that means "where this birch stands" has to
    // go through birchTrunkW, never through the cell. It is re-tested HERE rather than moved because the bole
    // is not known until the model is picked, and the cheap box test above still rejects most cells first.
    { const m8 = BIRCHV[k];
      if (m8) { const tw8 = birchTrunkW(t, m8);
        const sdx = tw8.wx - (SPWX + SPOX), sdz = tw8.wz - SPWZ;
        if (sdx * sdx + sdz * sdz < BK_SPAWN * BK_SPAWN) return null; } }   // sink 1-3, and it means what it means for an oak: the stamp writes in mode 1, so courses below local ground are refused rather than punched into the hill
    // -- AND A BEEHIVE ON ONE BIRCH IN A HUNDRED (user 2026-08-24: "attach beehives to some of the birch trees.
    // make it somewhat rare. say 10%?", walked down to 5% and then 2% the same day, and to 1% on 2026-08-26)
    // -- oakAt's hive block, line for line, against BIRCH_BANCH and this band's own grid. The Y IS RESOLVED
    // HERE and not at stamp time for oakAt's reason: hiveAt() is the query the bee swarm reads and it has to
    // answer with a world position, and groundMin is then only paid on the hundredth of cells that carry one.
    // BKHIVE is a SMALLER number than the oak's OKHIVE but sits on a denser grid, and the two land in the same
    // place on the ground: OKHIVE is 6% of the two biggest oak layers, i.e. 3.1% of oaks on a 79-voxel cell at
    // a 78% pass rate = one hive per 51 m square, where this is 1% of birches on a 44-voxel cell at a 76% pass
    // rate = one per 50 m square. Ground density is the number to compare, never the raw percentage — the two
    // bands' cells differ by 3.2x in area. The seat matches stampBirch exactly - groundMin at the TRUNK, less
    // sink, less m.tbz - because a hive resolved against the box centre would hang at the wrong height on
    // every model that leans.
    const HB = BIRCH_BANCH[k];
    if (HIVEV && HB && HB.length && ihash(cx * 61 + 43, cz * 59 + 17) < BKHIVE) {
      const m = BIRCHV[k];
      const a = HB[((ihash(cx * 89 + 7, cz * 97 + 31) * (HB.length - 0.01)) | 0)];
      const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
      let rx, rz;                                                                 // stampBirch's rotation, verbatim
      if (t.rot === 0) { rx = ax; rz = ay; }
      else if (t.rot === 1) { rx = m.sy - 1 - ay; rz = ax; }
      else if (t.rot === 2) { rx = m.sx - 1 - ax; rz = m.sy - 1 - ay; }
      else { rx = ay; rz = m.sx - 1 - ax; }
      const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
      const hx = t.wx - (fw >> 1) + rx, hz = t.wz - (fd >> 1) + rz;
      const tw = birchTrunkW(t, m);
      const hy = groundMin(tw.wx, tw.wz, 4) - t.sink - m.tbz + az - HIVEV.sz;     // the same seat stampBirch uses, so the hive's TOP course lands one below the branch it hangs from
      t.hv = { wx: hx, wy: hy + (HIVEV.sz >> 1), wz: hz,                          // CENTRE of the box - the point a swarm orbits
               bx: hx, by: hy, bz: hz,                                            // ...and the stampModel anchor (bottom-CENTRE column) it is actually written at
               sx: HIVEV.sx, sy: HIVEV.sy, sz: HIVEV.sz,
               tx: t.wx, tz: t.wz, k: t.k, rot: t.rot };
    }
    return t;
  }
  // stampModel cannot be reused, and the reason is the packing: a birch reaches 264 voxels and the shared
  // stamper reads height out of eight bits. Here it is nine (x | depth<<8 | height<<16 | colour<<25), and the
  // top field is a 0..6 INDEX into BIRCHIDS rather than a palette id, because seven bits cannot hold an 8-bit
  // id. Everything else - the rotation, the clip, the mode-1 test - is stampModel's, line for line.
  // ── WHERE A PLANTED BIRCH'S TRUNK STANDS, IN THE WORLD ── the model is centred on its BOUNDING BOX but the
  // bole is not at that centre (up to 83 voxels off — assets/bow.js birchTrunkC), so anything that wants "the
  // tree's position" in the sense a player would mean it has to rotate the trunk column out of the model and
  // add it to the box corner. Two callers now: the stamp, which seats the bole on the ground, and the perched
  // songbird walk in main/tick-nav.js, which must scan the crown around the TRUNK or its bole-exclusion ring
  // lands in open air on one side and inside the wood on the other. One helper, because when these two
  // disagreed the trees hung in the sky.
  function birchTrunkW(t, m) {
    const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
    const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
    if (t.rot === 0) return { wx: bx + m.tcx, wz: bz + m.tcy };
    if (t.rot === 1) return { wx: bx + m.sy - 1 - m.tcy, wz: bz + m.tcx };
    if (t.rot === 2) return { wx: bx + m.sx - 1 - m.tcx, wz: bz + m.sy - 1 - m.tcy };
    return { wx: bx + m.tcy, wz: bz + m.sx - 1 - m.tcx };
  }
  function stampBirch(t, x0, x1, z0, z1) {
    const m = BIRCHV[t.k];
    const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
    const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
    // ── SEAT ON THE GROUND UNDER THE TRUNK, NOT UNDER THE BOUNDING BOX ── this used to read
    // groundMin(t.wx, t.wz, 4), i.e. the column at the box CENTRE. On these models the bole is up to 83
    // voxels from that centre (assets/bow.js birchTrunkC), because a crown leans, so on a slope the tree was
    // seated to a column 8 m away from where it actually stands - and hung in the air on the downhill side.
    // The trunk centroid is rotated exactly as the voxels are below, so it tracks the tree's own facing.
    const tw = birchTrunkW(t, m), tcx = tw.wx - bx, tcz = tw.wz - bz;
    const gy = groundMin(tw.wx, tw.wz, 4) - t.sink - m.tbz;   // seat the BOLE on the ground, not the model's box — m.tbz is how far up the trunk starts (assets/bow.js). Low side of a slope, as the oaks do, so no birch stands on a stalk
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i];
      const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 511;
      let rx, rz;
      if (t.rot === 0) { rx = x; rz = y; }
      else if (t.rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (t.rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      const ax = bx + rx, az = bz + rz;
      if (ax < x0 || ax >= x1 || az < z0 || az >= z1) continue;
      const ay = gy + z; if (ay < 1 || ay >= WY) continue;
      const gx = gwrap(ax, WX), gz = gwrap(az, WZ);
      const ii = gx + ay * WX + gz * WX * WY;
      const cur = W[ii];
      if (cur !== 0 && cur < DECOR_MIN) continue;      // mode 1: grow through ferns and grass, never through terrain
      W[ii] = BIRCHIDS[p >>> 25];
    }
    if (t.hv) stampModel(HIVEV, t.rot, t.hv.bx, t.hv.by, t.hv.bz, x0, x1, z0, z1, 1);   // the BEEHIVE, at the world anchor birchAt already resolved — mode 1, like the tree it hangs in
  }
  function treeAt(cx, cz) {
    if (ihash(cx * 7 + 13, cz * 11 + 5) > 0.72) return null;
    const wx = Math.round(cx * TCELL + 6 + ihash(cx * 3 + 1, cz * 5 + 2) * (TCELL - 12));
    const wz = Math.round(cz * TCELL + 6 + ihash(cx * 9 + 4, cz * 3 + 8) * (TCELL - 12));
    if (desertM(wx, wz) > 0.5) return null;   // ── NOT IN THE DESERT ── pines are pine-forest litter. Gated on the same mask the height and the surface colour use, at the halfway point, so the forest floor thins out across the rim rather than stopping dead at a line.
    if (oakM(wx, wz) > 0.5) return null;     // ── NOR IN THE OAK FOREST (user 2026-08-17) ── the same test at the other border. Without it the new biome is a pine wood with oaks in it rather than a biome, and the two canopies interpenetrate right across the 450-voxel blend band. > 0.5 rather than > 0 deliberately: the pines thin out through the rim instead of stopping on the iso-line, which is what makes the two forests read as meeting rather than as abutting.
    // ── NOR IN THE BIRCH FOREST ── the THIRD border, and the same > 0.5 halfway test the desert and oak
    // lines above use. Without it the new band is a pine forest wearing oak ground: treeAt only ever
    // excluded itself from the two biomes that existed when it was written, and a band that is neither
    // desert nor oak reads to it as ordinary pine country.
    if (birchM(wx, wz) > 0.5) return null;
    // ── NOR IN THE ARCTIC (user 2026-08-29) ── the FOURTH border, and the fourth time this list has had to
    // grow with the world. The pattern is worth naming: treeAt places pine wherever it is not told otherwise,
    // so pine is the DEFAULT and every new biome has to exclude itself here or it opens as a pine forest
    // wearing someone else's ground. Same > 0.5 halfway test as the other three, so the pines thin out across
    // the blend band rather than stopping on the iso-line.
    // DITHERED against the mask rather than cut at 0.5 (user 2026-08-29: "make the transition … smoother. it
    // currently looks like a straight snow line"). A halfway test stops the trees dead on an iso-line while the
    // snow underneath fades in across the whole rim, and the eye reads the tree edge, not the ground: hence a
    // straight line. Rejecting with PROBABILITY equal to the mask thins the wood out across the blend instead,
    // which is the same trick the desert sand and the oak floor already use on their own rims.
    if (arcticM(wx, wz) > ARCT_BARE) return null;
    const dxs = wx - (SPWX + SPOX), dzs = wz - SPWZ;
    if (dxs * dxs + dzs * dzs < 26 * 26) return null;  // spawn clearing
    // ── AND A CLEAR LINE OF SIGHT DOWN THE SPAWN HEADING (user 2026-08-16: "try not to have the player spawn
    // looking in front of a pine tree") ── the 26-voxel clearing above only guarantees elbow room; a pine just
    // outside it, dead ahead, still fills the screen the instant you load. This rejects trunks inside a narrow
    // CORRIDOR along SPYAW rather than widening the circle, which would clear trees to the sides and behind
    // where nobody is looking. Derived from SPYAW rather than assuming +X, so re-baking the spawn heading
    // moves the corridor with it. Short and narrow on purpose: long enough to open the view out of the trees, tight
    // enough that it reads as a gap between trees and not as a felled lane.
    const fwdX = Math.sin(SPYAW), fwdZ = Math.cos(SPYAW);
    const along = dxs * fwdX + dzs * fwdZ;
    if (along > 0 && along < SPVIEW_D && Math.abs(dxs * fwdZ - dzs * fwdX) < SPVIEW_W) return null;
    if (H(wx, wz) <= WL + 4) return null;    // no pines in water or on beaches
    if (nearCave(wx, wz)) return null;
    // ── NINE PINES, NOT ONE (user 2026-09-01: "you can transfer the assets you need … like the 9 pine trees") ──
    // `ti` indexes MROT9 (assets/palette.js), which carries one rotation set per model. It gets its OWN hash
    // rather than reusing rot's: keyed off the same value, every tree of a given species would also share a
    // pose, and a stand would read as one asset repeated rather than nine.
    return { tx: wx, tz: wz, ti: (ihash(cx * 31 + 17, cz * 37 + 29) * (MROT9.length - 0.01)) | 0,
             rot: (ihash(cx + 101, cz + 55) * 3.99) | 0, sink: 5 + ((ihash(cx * 13, cz * 17) * 4) | 0) };   // sink 5-8 (was 1-7) — every trunk base voxel is buried, no floating trees on bumpy ground
  }
  function stampTree(tr, x0, x1, z0, z1) {             // exact rotated-array copy, clipped to a world region
    const R = MROT9[tr.ti | 0][tr.rot];   // …the tree's OWN model. `| 0` so a shape built before ti existed still resolves to model 0 rather than undefined
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
    const ANCH = PINE_ANCH9[tr.ti | 0], AMX = MROT9[tr.ti | 0][0].sx, AMY = MROT9[tr.ti | 0][0].sz;
    // ── THIS TREE'S OWN ANCHORS, NOT MODEL 0's (user: "the pine cones are floating in the air") ── an anchor
    // is a canopy voxel with open air beneath it, so PINE_ANCH9 holds one list PER MODEL — the nine pines have
    // nine different crowns. This used PINE_ANCH, the legacy alias for PINE_ANCH9[0], and rotated by the global
    // MSX/MSY, which are model 0's dimensions. Every tree therefore hung its cones on pine #1's anchor cells,
    // which on the other eight models are empty sky.
    if (CONEV && ANCH.length) {                   // PINECONES — 6-12 per pine (2× the old 3-6), hung UNDER canopy anchors (foliage with open air below),
      const n = 6 + ((ihash(tr.tx * 7 + 5, tr.tz * 9 + 2) * 7) | 0);   // rotated with the tree so every region stamps them identically
      const used = new Set();                          // one cone per column — never stacked on top of each other
      for (let k = 0; k < n; k++) {                    // PINE_ANCH is angle-sorted: pick k-th from the k-th angular sector — cones ring the crown evenly
        const a = ANCH[(((k + 0.15 + ihash(tr.tx * 13 + k * 29, tr.tz * 17 + k * 31) * 0.7) / n) * ANCH.length) | 0];
        const ax = a & 255, ay = (a >> 8) & 255, az = (a >> 16) & 255;
        let rx, rz;
        if (tr.rot === 0) { rx = ax; rz = ay; }
        else if (tr.rot === 1) { rx = AMY - 1 - ay; rz = ax; }
        else if (tr.rot === 2) { rx = AMX - 1 - ax; rz = AMY - 1 - ay; }
        else { rx = ay; rz = AMX - 1 - ax; }
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
    yield* stampCellsGen(x0, x1, z0, z1, MUCELL, 18, mushAt, stampMush);
    yield* stampCellsGen(x0, x1, z0, z1, FLWCELL, 6, flowerAt, stampFlower);   // meadow flowers — margin 6 covers the widest variant (5) rotated, plus a voxel      // rare pine-forest mushroom cluster (23×27 footprint)
    yield* stampCellsGen(x0, x1, z0, z1, PCCELL, 8, pconeAt, stampPcone);
    yield* stampCellsGen(x0, x1, z0, z1, SCELL, 8, stickAt, stampStick);
    yield* stampCellsGen(x0, x1, z0, z1, LILYCELL, 8, lilyAt, stampLily);
    // (GIGANTIC lake pads REMOVED at user request 2026-07-20 — the lilyGigAt/stampLilyGig pair is left in place, and still
    //  crosses to the gen workers, so re-enabling is a one-line restore of this pass. The small drifting pads are untouched.)
    yield* stampCellsGen(x0, x1, z0, z1, LGCELL, 16, logAt, stampLog);
    yield* stampCellsGen(x0, x1, z0, z1, BKCELL, BKMARGIN, birchAt, stampBirch);   // ── THE BIRCHES ── beside the oaks and before the pines, for the same reason the oaks sit here: the ground scatter has to be down first (a tree refuses a cell a rock already took), and the pines come last
    yield* stampCellsGen(x0, x1, z0, z1, OKCELL, OKMARGIN, oakAt, stampOak);   // -- THE OAKS -- last of the stamped decor and immediately before the pines, which is where a tree belongs: every ground scatter above has already been laid, and mode 1 lets a crown grow through the ferns and grass rather than leaving a hole where one would have been. The two tree passes never meet - their biome gates are exclusive - so their order relative to each other is free.
    for (const t of treesInRegion(x0, x1, z0, z1)) { stampTree(t, x0, x1, z0, z1); yield; }   // one tree per slice — the old all-at-once pass spiked 10–25 ms per band
  }
  function genRegion(x0, x1, z0, z1, fresh) { const g = genRegionGen(x0, x1, z0, z1, fresh); for (let r = g.next(); !r.done; r = g.next()) {} }
  function rebuildBricks(gx0, gx1, gz0, gz1) {         // grid coords, 8-aligned
    for (let bz = gz0 >> 3; bz < gz1 >> 3; bz++) for (let bx = gx0 >> 3; bx < gx1 >> 3; bx++) {
      let maxH = 0, cav = 0;                           // nothing exists above terrain + the tallest pine — sky bricks clear without a single voxel read
      for (let z = bz * 8; z < bz * 8 + 8; z++) for (let x = bx * 8; x < bx * 8 + 8; x++) { const hv = hmap[x + z * WX]; if (hv > maxH) maxH = hv; if (hv <= CAVE_FLOOR_MAX) cav = 1; }
      if (cav && maxH < HMAX) maxH = HMAX;             // ── GORGE TILE ── stampCave drops a carved column's hmap to the gorge FLOOR while its per-4-voxel-band wall jag deliberately leaves wall standing all the way back up to the pristine surface, so hmap is no longer an upper bound on that column's contents. The cap force-CLEARS every brick above it and the DDA reads an unset brick as air, so intact stone went invisible (it still collided, chopped and anchored support). H() is clamped to HMAX, so that is the honest ceiling for a column whose real height the carve erased. Must stay identical to the copy in gen-pool.js — __vb.gtest diffs the two.
      const byCap = Math.min(BY, ((maxH + CANOPY) >> 3) + 1);   // CANOPY, not a literal — world/window.js explains why a number here goes INVISIBLE rather than wrong
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
        if (poolTouchHook) poolTouchHook(b, -1);      // the inline (non-pooled) region rebuild — the fallback path when the gen pool is unavailable or a region is generated on the main thread. Same reason as the slab merge in gen-pool.js: generation never goes through gpuPatch, so it must queue its own bricks.
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
