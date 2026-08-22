  // ── …AND THE LILY PADS (user 2026-08-08: "have the lilly pads able to be broken with a tool") ── a pad is a
  // soft plant like a fern, so it yields to WHATEVER is in hand: no axeOnly/pickOnly/digOnly entry, which is
  // what makes the swing gate's (axeOnlyTab[id] ? cut : true) branch answer true for every tool. Two things
  // this deliberately does NOT need: the pads stay walk-through (solidTab untouched — a pad never had a hitbox
  // and breaking one should not give it one), and the REMAINDER of a bitten pad stays put on its own, because
  // supFlood terminates both floods on a FLUID neighbour and every pad voxel is stamped at WL+1 with the lake
  // directly beneath it. The GIANT pad's ids are folded in too: it is not stamped today, but it is the same
  // plant and its ids are its own (addCol above), so this can never leak onto anything else.
  const LILYIDS = [...new Set([...LILYV, ...(LILYPAD_GIGV ? [LILYPAD_GIGV] : [])].flatMap((m) => m.vox.map((p) => p >>> 24)))];
  for (const i of LILYIDS) decorTab[i] = 1;
  // ── THE GROUND LOG IS MADE OF THE PINE'S OWN BARK (user 2026-08-07: "make the log that's in the terrain
  // match the texture of the pine tree trunks") ── log.vox arrives with its own authored shades, minted as
  // their own palette entries, so a fallen log read as a different wood from the trunks around it. Every log
  // colour is repointed at the NEAREST bark shade the pine already owns. Reusing woodIds rather than adding
  // matched entries is deliberate twice over: the 256-table is full, so new entries would be silently
  // nearest-matched onto something else anyway; and the trunk shades have already been through the bark
  // smoothing above (TRUNK_SMOOTH), which is what gives a trunk its quiet grain — a log built from raw
  // authored browns would still read as a different material even at the same hue.
  if (LOGV && woodIds.length) {
    const logMap = new Map();
    const nearestBark = (id) => {
      if (logMap.has(id)) return logMap.get(id);
      const c = palette[id] || [0, 0, 0];
      let best = woodIds[0], bd = Infinity;
      for (const w of woodIds) { const q = palette[w]; if (!q) continue;
        const d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
        if (d < bd) { bd = d; best = w; } }
      logMap.set(id, best); return best;
    };
    LOGV.vox = LOGV.vox.map((p) => (p & 0xffffff) | (nearestBark(p >>> 24) << 24));
    console.log('[vb] log bark remap', [...logMap].map(([a, b]) => a + '->' + b).join(' '));
  }
  if (LOGV) for (const q of LOGV.vox) { decorTab[q >>> 24] = 1; axeOnlyTab[q >>> 24] = 1; }  // …and the ground LOGS (user), but those want the axe
  // ── WHAT THE RENDERER CALLS STONE (user 2026-08-16: "can you give the rock a reflection property from
  // the sun?" … "all of the rocks") ── the one table isRockV in PRE is generated from, and nothing else reads
  // it. A SEPARATE table on purpose: pickOnlyTab is the closest existing fit and it is wrong at both ends — it
  // carries COAL and IRON (ore is a vein IN stone, not a stone face, and gold/crystal already have their own
  // treatment) and it is missing BROCK and PEBBLE, which are the medium boulder and the hand stone — the two
  // rocks the player is most often looking straight at. Built here rather than in the shader because the two
  // model sets are 347k voxels between them and these loops already walk every one of them.
  // ── ONE KNOWN LEAK, MEASURED, AND LEFT IN ── palette ids are shared by TOLERANCE (PAL_TOL 6) and the
  // bird models load AFTER rocks26, so five of the twelve boulder shades — 145, 146, 148, 150, 151 — are also
  // worn by the PERCHED SONGBIRDS, which are the only life this game writes into the world grid (see the
  // stamp in sim/life/stamped.js) and therefore the only creature whose pixels reach isRockV at all. Every
  // other creature is trace-injected and TRACE zeroes its h.vox, so they are immune by construction.
  // Dropping the five is NOT the fix: they are 65.6% of a boulder's 347k voxels (149/150/151 alone are 71%
  // of that), so a boulder would glint in patches. Measured instead: hunt a perched bird that still holds a
  // colliding id, has clear sky overhead, and frame it down the MIRRORED sun — the one geometry where the
  // sheen exists — and 3 pixels of the frame change, at 12/255. That is the worst case, not the typical one.
  // The clean fix is the trick BROCK and the desert rocks already use, same colour on dedicated ids, applied
  // to the BIRD loader instead: it costs ~5 palette slots out of the handful still free, which is a real
  // spend for a 3-pixel artefact, so it is a decision to take deliberately rather than a bug to sweep up.
  const rockShTab = new Uint8Array(256);
  for (const i of [...ROCK, ...ROCKX, ...BROCK, ...PEBBLE]) rockShTab[i] = 1;   // terrain strata + their partner shades, the medium boulder, and the pickable field stone
  for (const r of ROCK26) for (const q of r.vox) { decorTab[q >>> 24] = 1; pickOnlyTab[q >>> 24] = 1; rockShTab[q >>> 24] = 1; }   // …and the 26 BOULDERS (user), which want the PICK. All 26 share one 12-shade palette, so this marks every rock in the world.   // …and the same 12 shades are what the sun sheen keys on, forest and desert alike: stampDrock stamps THESE models into the sand, so one line covers both biomes' boulders.
  for (const i of [...ROCK, ...ROCKX, ...BROCK, ...ORECOAL, ...OREIRON]) rockTopTab[i] = 1;   // ── WHAT COUNTS AS ROCK UNDERFOOT ── strata, the partner shades, BOULDERS (dedicated ids, so pickOnlyTab never sees them) and ore. Read by the land-mammal spawn test; a 1-voxel PEBBLE is deliberately not here — it is ground scatter, and excluding it over an 11-voxel footprint would reject most of the forest floor.
  for (const i of [...ROCK, ...ROCKX, ...ORECOAL, ...OREIRON]) { decorTab[i] = 1; pickOnlyTab[i] = 1; }   // …and COAL + IRON (user): ore belongs to the pick like the stone it sits in   // …the STONE STRATA under the soil (user) belong to the PICK, not the shovel: dig down with the shovel, then swap and keep going
  const woodTab = new Uint8Array(256);                 // ── WOOD ── the axe takes chunks out of anything made of wood voxels (user), including a stump the
  for (const i of woodIds) { woodTab[i] = 1; decorTab[i] = 1; axeOnlyTab[i] = 1; }   // felled tree left behind, which belongs to no tree shape any more
  for (const i of [...DIRT, ...MOSS, ...NEEDLE, ...SAND, ...DSAND]) { decorTab[i] = 1; digOnlyTab[i] = 1; }   // …and the SOIL (user): dirt, the mossy grass on top of it, the brown pine litter that covers most of the forest floor, and beach sand. NOT the stone strata underneath — the shovel stops at rock (user).   // …and the GROUND ITSELF (user), dug only with the SHOVEL: DIRT (the buried layers), MOSS (green surface), NEEDLE (the brown pine litter — most of the forest floor, and what reads as 'dirt' underfoot) and SAND (beaches, lakebed). GRASS (the strands) is separate walk-through decor and stays any-tool.
  // ── SURFACE SCATTER ── grass strands, flowers, twigs and pinecones: all of it needs something underneath,
  // which is what floatTab marks. floatTab is NOT a statement about whether a tool can break the thing — the
  // FLOWERS carry decorTab as well (see the block beside their solidTab line below), and the two tables answer
  // different questions: floatTab is "this rests on the surface and the eye/aim walks past it", decorTab is
  // "a swing may take a bite out of it". Grass, twigs and cones are still scatter-only, which is why they are
  // the set the hoe lifts and the flowers are the set that also breaks.
  // ── SNOW IS COVER, NOT A TARGET (user 2026-08-07: "the tool shouldn't register the snow, but the material
  // under the snow") ── its own flag, because a swing must SEE THROUGH the blanket the way it already sees a
  // trunk through needles. Deliberately NOT added to decorTab/digOnlyTab/pickOnlyTab: landSnowAt caps a stack
  // at 3 layers, so the see-through budget always reaches the ground and snow never needs to be a target at
  // all — and putting it in digOnlyTab would silently widen the shovel's okMat so every bite in dirt pulled
  // the blanket with it. Snow orphaned by a bite underneath is already handled: SUP.CLASS[SNOW] = DRAPE.
  // ── ICE THE PICK CAN BITE ── ice is not a material: freezing just flips solidTab[WATER_T], so the frozen
  // surface IS the lake's own water voxels made walkable. That means the pick can carve them like anything
  // else — but the hole has to be given back, because nothing here simulates flow and a pit chopped in a lake
  // would still be there in spring. Every water voxel a bite takes is logged and restored when the ice thaws.
  let iceCutI = new Int32Array(1 << 12), iceCutV = new Uint8Array(1 << 12), iceCutN = 0;
  const snowTab = new Uint8Array(256);
  snowTab[SNOW[0]] = 1; snowTab[SNOW[1]] = 1;
  const floatTab = new Uint8Array(256);
  // ── EVERYTHING THAT HANGS OFF A CROWN AND IS STRANDED WHEN THE CROWN LEAVES ── read by coneWake
  // (sim/chop.js), whose filter was `coneTab || snowTab` and therefore covered the two hangers that existed
  // when it was written. FRUIT arrived on 2026-08-17, months later, and was never added to it — so felling an
  // oak woke its cones and its canopy snow and left its apples and oranges hanging in the air, to be picked up
  // only incidentally, whenever some cleared cell happened to fall within 26 of one. That is the user's
  // 2026-08-22 report: "the oranges that were connected to it were just floating in the air. then after 5
  // seconds they fell." A named table rather than a longer `||` chain, so the NEXT thing that hangs off a
  // crown has one obvious place to register itself instead of a third condition to be forgotten from.
  const hangTab = new Uint8Array(256);
  const coneTab = new Uint8Array(256);                 // PINECONE ids — no hitbox for the PLAYER (see solid()); every other system still treats them normally
  for (const i of [...GRASS, ...FLOWERIDS, ...OAKMOSS]) floatTab[i] = 1;   // OAKMOSS rides with GRASS: same surface-scatter class, oak-canopy colour (see assets/palette.js)   // FLOWERIDS replaces BLOOM (user 2026-08-18): the flowers are an authored MODEL now, so the ids come off its own voxels — see assets/bow.js
  for (const m of STICKV.concat(STICKB)) for (const q of m.vox) floatTab[q >>> 24] = 1;            // twigs (user) — and the AUTHORED pink pair, whose browns are already flagged (they are the same ids) but whose four leaf pinks are its own and would otherwise miss the whole ground-scatter class
  // ── PINECONES ARE WALK-THROUGH FOR THE PLAYER (user 2026-08-05: "should be able to clip through them") ──
  // floatTab already called them surface scatter, but solidTab is set by the BLANKET `i < DECOR_MIN` sweep
  // above and cones come out of the pine's own palette, so they landed in that range and kept a hitbox: you
  // could stand on a cone and it blocked a step. solidTab is deliberately NOT cleared — cone ids are shared
  // with the pine's BARK ids, so that would take the hitbox off every trunk. The exemption lives in the
  // player's own solid() test instead; see coneTab there.
  if (CONEV) for (const q of CONEV.vox) { const ci = q >>> 24; floatTab[ci] = 1; coneTab[ci] = 1; }   // …and the pinecones lying beside them
  { const markSolid = (m) => { if (m) for (const p of m.vox) solidTab[p >>> 24] = 1; };     // logs + rocks + mushrooms are REAL obstacles; everything else stays walk-through decor
    markSolid(LOGV);
    markSolid(MUSHV);                                                                        // mushroom clusters collide (its ids are its own — addCol never dedupes)
    if (MUSHV) for (const p of MUSHV.vox) mushTab[p >>> 24] = 1;                             // …and they're BOUNCY: landing on one trampolines the player (progressively higher)
    markSolid(LILYPAD_GIGV);                                                                 // GIANT lilypads are solid — the player can stand on them (user: 'give it a hitbox')
    for (const r of ROCK26) markSolid(r);

    for (const r of DROCK) { markSolid(r); for (const p of r.vox) { decorTab[p >>> 24] = 1; pickOnlyTab[p >>> 24] = 1; rockShTab[p >>> 24] = 1; } }   // desert rocks are STONE: solid, and choppable only with the pick — the same pairing ROCK26 gets below, so an axe bounces off them
    for (const c of CACTI) markSolid(c);
    // ── AND CHOPPABLE BY ANYTHING, ARROWS INCLUDED (user 2026-08-15) ── decorTab is the admission ticket; the
    // three *OnlyTab tables are RESTRICTIONS on top of it. Listing the cacti here and in NOTHING else is exactly
    // what makes the swing gate's `(axeOnlyTab[id] ? cut : true)` branch answer true for every tool, the same
    // way the lily pads and ferns do it. The arrow needs no code at all: arrowChop already routes a non-wood
    // hit into phChopDecor and deliberately ignores axeOnlyTab, so decorTab alone is what it was waiting on.
    // markSolid STAYS — arrowBlocked tests solidTab, so without it a shaft would fly straight through the plant.
    for (const c of CACTI) for (const p of c.vox) decorTab[p >>> 24] = 1;
    for (const c of CACTI) for (const p of c.vox) cactusTab[p >>> 24] = 1;   // …and the SAME ids sting: one table, so a cactus can never be choppable-but-harmless
    // ── SHRUBS ── soft desert scrub, so NOT markSolid: you walk through them the way you walk through a fern.
    // decorTab and nothing else — no axeOnly / pickOnly / digOnly entry is exactly what makes the swing gate's
    // `(axeOnlyTab[id] ? cut : true)` branch answer true for every tool, arrows included, the same way the ferns
    // and the lily pads do it. Marked by SHRUBC + SHRUBF rather than by walking SHRUBV because the loader
    // resolves every shade in the .vox files onto exactly those ids, so it is the honest set either way and it
    // stays right when the artist re-authors the files with a different shade count — which is precisely what
    // happened. THE FLOWER IDS MUST BE HERE TOO: models 1-4 bloom, and marking only the greens would leave a
    // bush whose leaves chop away and whose flowers hang in the air. solidTab for all ten is cleared back in
    // palette.js, where the blanket below-DECOR_MIN sweep would otherwise have handed a knee-high bush the
    // hitbox a boulder gets. NOTE the shrubs quote the CACTUS's colours — same RGB, different ids, deliberately
    // (palette.js): markSolid(CACTI) and cactusTab two lines up are exactly what a shared id would have
    // dragged onto them, and that goes for the bloom's cream as much as for the body green.
    for (const i of SHRUBC) decorTab[i] = 1;
    for (const i of SHRUBF) decorTab[i] = 1; }                                                 // a 4 m saguaro is an obstacle, not scenery. Safe to mark by id because the cacti carry their OWN 16 ids (their .json palette goes through addCol, which never dedupes), so nothing else in the world wears them
  // ── THE OAKS ARE MADE OF THE TWO MATERIALS THE PINE ALREADY HAS ── and they have to say so HERE rather
  // than inherit anything, because the oaks' ids come out of a .json loader and therefore land ABOVE
  // DECOR_MIN, where the blanket `i < DECOR_MIN` sweep in palette.js never reaches. So both halves start life
  // as walk-through decor and the wood has to be given its solidity back explicitly.
  //   * BARK needs nothing at all, and that is the POINT of how it is loaded: assets/bow.js repoints the
  //     oaks' three bark shades onto the pine's own woodIds rather than minting new ones, so the loop a few
  //     lines up has already given every one of them woodTab + decorTab + axeOnlyTab, and the blanket
  //     below-DECOR_MIN sweep in palette.js has already given them solidity. It is re-asserted below anyway,
  //     as a no-op that documents the dependency — if the bark ever mints its own ids again, this line is
  //     what stops an oak trunk silently becoming soft scenery the generation orphan sweep may delete
  //     (ORPHAN_OK in sim/support-rules.js is derived as "not foliage and not wood").
  //   * LEAVES are canopy: foliaTab, and NOT solid — you walk through an oak crown exactly as you walk
  //     through a pine's needles. Pushing them into foliageIds as well is what carries that fact to the two
  //     consumers that build their own tables from it AFTER this file: the WGSL `isFol` in render/wgsl/dda.js
  //     (crown see-through when the eye clips into it) and the bird PERCH surface in main/tick-nav.js. It also
  //     reaches SUP.CLASS in sim/support-rules.js, which reads foliaTab and makes every leaf a DRAPE — the
  //     asymmetric-support rule that stops a crown ever being lifted as one piece with its trunk.
  //   * canopy SNOW follows for free: tick-snow keys on foliaTab, so an oak crown catches a storm.
  // Deliberately NOT in decorTab as a pair: bark is (with axeOnly), leaves are not — the same split the pine
  // has, where needles are scenery the axe passes through on its way to the trunk.
  for (const i of OAKBARK) { solidTab[i] = 1; woodTab[i] = 1; decorTab[i] = 1; axeOnlyTab[i] = 1; }
  for (const i of OAKLEAF) { solidTab[i] = 0; foliaTab[i] = 1; foliageIds.push(i); }
  // ── THE BLOSSOM IS CANOPY, EXACTLY AS THE GREEN LEAF IS ── the cherry forest stamps the SAME oak models with
  // the leaf ids swapped (assets/bow.js blosRemap), so every material question a crown answers — do I collide, do
  // I catch snow, does the primary ray see through me, am I axe-only, does the orphan sweep keep me — has to
  // answer identically or the two forests would behave differently while looking the same. Nothing is inherited
  // from OAKLEAF by being "a leaf": these flags are per-id and this line is the whole of the blossom's identity.
  // ── AND SO IS THE LIGHT GREEN VARIETY (user 2026-08-19) ── the same two lines the blossom gets, for the
  // same reason: assets/bow.js stamps HALF the oak forest with the leaf ids run through OAKLITER, and two of
  // that ramp's four steps are ids OAKLEAF already covered a line above. These are the other two, and without
  // this line a light crown would collide with the player, refuse snow, stop being see-through when you walked
  // into it, drop off the bird perch surface, and — worst — lose its exemption from the generation orphan
  // sweep (ORPHAN_OK in sim/support-rules.js is derived as "not foliage and not wood"), which deletes an
  // unsupported voxel: half the oaks in the world would generate with holes in them.
  for (const i of OAKLITE) { solidTab[i] = 0; foliaTab[i] = 1; foliageIds.push(i); }
  for (const i of BLOSLEAF) { solidTab[i] = 0; foliaTab[i] = 1; foliageIds.push(i); }
  for (const i of BLOSWHITE) { solidTab[i] = 0; foliaTab[i] = 1; foliageIds.push(i); }
  // The CHERRIES are foliage too, and deliberately the same class as the apples and oranges rather than the
  // canopy: foliaTab is what keeps a hanging fruit OUT of the generation orphan sweep (ORPHAN_OK is "not
  // foliage and not wood"), and it is what lets the aim ray stop on one instead of passing through the way it
  // passes through floatTab scatter. No hitbox, so you still walk through a crown.
  if (BLOSCHERRY) { solidTab[BLOSCHERRY] = 0; foliaTab[BLOSCHERRY] = 1; foliageIds.push(BLOSCHERRY); }   // the white variant is the same MATERIAL as the pink one — see the BLOSMAPW note in assets/bow.js
  // ── A BERRY AND A FRUIT ARE CANOPY, NOT SCATTER (user 2026-08-17) ── the three FRUITC ids get exactly the
  // treatment the oak LEAF above gets, and the reasoning is that a cherry on a bush and an apple in a crown
  // genuinely ARE part of the crown. Everything that follows from foliaTab is what a fruit wants, item by item:
  //   * NO HITBOX. solidTab stays 0, so you walk through a berry bush the way you walk through the leaves the
  //     berries replaced — which matters at 22% of every oak in the world, all of it at knee height.
  //   * DRAPE support (sim/support-rules.js reads foliaTab). A drape hangs off an anchored structure and never
  //     conducts anchoring onward, which is precisely a fruit's relationship to the branch above it.
  //   * NOT DELETABLE by the generation orphan sweep: ORPHAN_OK is derived as "not foliage and not wood", so
  //     foliaTab is the flag that keeps a hanging fruit out of it. floatTab — the obvious alternative, and what
  //     the BLOOM flower heads wear — would have been the opposite: ORPHAN_OK true, and the aim ray in
  //     sim/tools.js walks straight THROUGH a floatTab voxel, so an apple would not even be a thing you can
  //     point at.
  //   * CHOPPABLE BY ANY TOOL, for free and through the path that already exists: phChopLeaves takes whatever is
  //     inside its sphere with no tool gate at all, so knocking fruit out of a crown needs no new code and no
  //     decorTab entry. decorTab is deliberately NOT set — it would put a 1-voxel berry in front of the swing as
  //     its own target (the `decorTab[id] && !woodTab[id]` branch in tools.js) and confine that swing's okMat to
  //     the berry, which is not what someone chopping a bush meant.
  //   * canopy SNOW, crown see-through (`isFol` in render/wgsl/dda.js is generated from foliageIds) and the
  //     backlit-leaf transmission in composite.js all follow. The last is a bonus rather than a cost: a backlit
  //     cherry glowing red is what a backlit cherry does.
  for (const i of FRUITC) { solidTab[i] = 0; foliaTab[i] = 1; foliageIds.push(i); hangTab[i] = 1; }
  // ── AND THE FRUIT'S STALK IS CANOPY TOO, WHENEVER IT HAS AN ID OF ITS OWN ── FRUIT_STEM_ID (assets/bow.js)
  // is 0 today, so this loop does nothing and the stalk wears the oak leaf id exactly as it always has; the
  // line exists so that minting the brown the user asked for is a ONE-WORD change over there rather than a
  // change in two files that can be made in one. The treatment is deliberately IDENTICAL to the FRUITC line
  // above rather than to the beehive's: a stalk is woody in COLOUR, not in behaviour. It is a 2-voxel drape
  // hanging in a walk-through crown, so giving it woodTab/solidity to match its new colour would be the exact
  // trade the beehive comment below argues against — a hitbox in a crown you walk through, a chop ray that
  // reads it as the trunk behind the needles, and a STRUCTURE component whose only anchor is a DRAPE.
  if (FRUIT_STEM_ID) { solidTab[FRUIT_STEM_ID] = 0; foliaTab[FRUIT_STEM_ID] = 1; foliageIds.push(FRUIT_STEM_ID); hangTab[FRUIT_STEM_ID] = 1; }
  // ── AND A BEEHIVE IS A SOLID OBJECT YOU CUT OUT OF THE TREE ── solid + decorTab + axeOnly, which is the LOG's
  // pairing and not the leaves'. Three deliberate choices in that:
  //   * SOLID, because a 50 cm box in a crown you could walk through would read as a decal. It also gives it an
  //     arrow hitbox (sim/projectiles.js tests solidTab), which is what a hive should have.
  //   * decorTab + axeOnlyTab and nothing else, so `(axeOnlyTab[id] ? cut : true)` in the swing gate answers
  //     "you need an edge" — an axe or the knife cuts a hive out, a shovel and a pick do not.
  //   * NOT woodTab, even though a hive is the same KIND of target as a log. woodTab is what `aimWood` reads and
  //     aimWood hands the whole swing to the tree-felling path, while the decor branch that actually chops a
  //     hive is written as `decorTab[id] && !woodTab[id]`. Marking it wood would make a hive uncuttable AND make
  //     aiming at one carve the oak behind it. The price is ORPHAN_OK true, and that is safe HERE for two
  //     independent reasons, both read off sweepOrphans in world/terrain.js: the sweep runs only on a slab a
  //     GORGE intersects (and oakAt refuses nearCave), and it deletes a component only when EVERY voxel in it is
  //     ORPHAN_OK — a hive hangs off a BRANCH, so its component always contains bark and is spared. The runtime
  //     resolver is the one that really matters, and there the hive is STRUCTURE hanging on wood, which is
  //     exactly why OAK_BANCH below anchors it to bark rather than to leaves.
  for (const i of HIVEC) { solidTab[i] = 1; decorTab[i] = 1; axeOnlyTab[i] = 1; }
  solidTab[ED_WHITE] = solidTab[ED_GREY] = solidTab[ED_HLITE] = 1;                          // the editor stage is walkable floor
  // ── AND NOTHING A FLOWER WEARS IS SOLID ── stated explicitly, AFTER the editor line above, rather than left
  // to the fact that the flower now mints its own ids (palOwn on ED_*, see assets/palette.js). Belt and braces:
  // solidTab is a blanket `i < DECOR_MIN` range sweep, so any future colour a flower shares down into that
  // range would silently grow a hitbox again, and this is the line that says it must not. floatTab already
  // said "surface scatter"; floatTab is not what the player collides with.
  for (const i of FLOWERIDS) solidTab[i] = 0;
  // ── …AND EVERY FLOWER BREAKS, WITH WHATEVER IS IN HAND (user 2026-08-19: "make the flowers breakable with
  // any tool, or item. like the ferns for example") ── decorTab and nothing else, which is EXACTLY the fern's
  // pairing (the FERNIDS loop in assets/bow.js): decorTab is the admission ticket and the three *OnlyTab tables
  // are RESTRICTIONS on top of it, so leaving all three unset is what makes the swing gate's
  // `(axeOnlyTab[id] ? cut : true)` branch in sim/tools.js answer true for an axe, a knife, a pick, a shovel, a
  // held rock and an empty hand alike — the same sentence the lily pads, the cacti and the shrubs are already
  // written with.
  // WHAT WAS MISSING WAS THE TICKET, NOT A RESTRICTION, and that is worth stating because the obvious guess is
  // the other one: no *OnlyTab has ever named a flower id, so the tool gate always said yes. It never got that
  // far. phChopDecor (sim/support.js) rejects a candidate cell outright on `!decorTab[v]`, so the sphere came
  // back empty, the branch was false, and the swing walked on through the plant to whatever stood behind it.
  // One table, and a flower behaves as a fern does.
  // floatTab STAYS, and the two do not fight: floatTab is what makes the AIM ray walk past a flower, and the
  // decor branch in the main march reads the voxel the ray is CROSSING rather than the one the crosshair
  // settled on, so a flower is choppable without ever becoming a thing that can steal a crosshair from the
  // ground behind it. solidTab is untouched by the line above for the same reason the pads' is: breaking
  // something should not first give it a hitbox.
  // IT DOES NOT TAKE THE TURF WITH IT. okMat in sim/tools.js confines the sphere to `!digOnlyTab && !pickOnlyTab`
  // once it is aimed at a flower, and the moss/needle/dirt/sand a flower stands in is digOnlyTab — so the ground
  // under the plant is never a candidate, and neither is the GRASS beside it, which is floatTab but has no
  // decorTab of its own. And a stem cut out from under a head strands nothing: flowers are SUP.DRAPE (that is
  // floatTab's doing, sim/support-rules.js), so the remainder is adjudicated by supFlush and dropped as a chunk
  // exactly as any other severed drape is.
  // BOTH BIOME SETS, for free: FLOWERIDS is derived in assets/bow.js from FLOWERV's own voxels BEFORE the pink
  // variant is spliced out into FLOWERV_CH, with FLOWPURP pushed in after — so this one list is every id the
  // oak forest's variants and the blossom band's twin wear between them. Derived, never a literal id: decor ids
  // are not boot-stable.
  // THE GUARD IS NOT DECORATION. flowers.vox is read in SHARE mode, so a flower colour can RESOLVE onto an id a
  // tree already owns, and decorTab on a canopy or bark id would hand the pine's own crown to the decor branch
  // of the swing — the "aiming at the trunk gets the leaves" failure. assets/bow.js folds those ids off the
  // flower models before FLOWERIDS is built, so this test should never fire; it is here because it is the line
  // that has to hold if a re-authored file ever finds a green the fold cannot rehome.
  // ── ONE SHARE REMAINS, MEASURED, AND KEPT ── the stem's own green IS GRASS[1] (an exact palette match, not a
  // tolerance one), so this line also makes that one of the four grass shades choppable — measured at 3.3% of
  // columns, against 13% for all four. That is deliberate and it is the cheaper half of a trade with no third
  // option: the palette is FULL (256 used, 0 free, 0 reclaimable), so the stem cannot be given an id of its own,
  // and refusing this id instead would leave a green stub standing wherever a flower was broken. What it costs
  // is that a swing crossing one grass strand in four spends itself on the strand — the same thing a fern or a
  // shrub already does, on a twelfth of the columns grass actually covers. If the flowers ever get their own
  // stem id back, drop GRASS out of this and nothing else here changes.
  for (const i of FLOWERIDS) if (!foliaTab[i] && !woodTab[i]) decorTab[i] = 1;
  // ── WHERE THINGS HANG IN AN OAK ── PINE_ANCH's idea re-derived for eight crowns instead of one model, and
  // split in two because a fruit and a hive want different SURFACES:
  //   OAK_ANCH[k]   canopy LEAF with clear exterior air below — an apple or an orange hangs from it.
  //   OAK_BANCH[k]  a BRANCH (bark) with six clear cells below — a beehive hangs from it, and it has to be bark:
  //                 a hive is solid, so it is STRUCTURE to the support resolver, and the STRUCTURE flood may not
  //                 enter a DRAPE cell. Hung off leaves it would be a component with no path to the ground, and
  //                 the resolver would lift it the first time anything nearby was disturbed. Hung off bark it
  //                 reaches the trunk, and the trunk is buried below hmap. Measured over the bake, every model
  //                 offers 102-911 of these, so the constraint costs nothing in placement freedom.
  // "Clear air below" means EXTERIOR air (voxShellAir, assets/bow.js): these crowns are hollow shells, so the
  // naive empty-cell test would have hung most of the fruit inside the dome where nobody can see it.
  // CAPPED, and that is not a detail: world/gen-pool.js stringifies every registered table straight into each
  // gen worker's source, and the raw candidate lists run 471 to 39,053 entries per model. Angle-sorted first and
  // then sampled evenly, so the cap costs even coverage rather than one arc of the crown — the same trick
  // PINE_ANCH plays with cones, and 96 sectors is ten times the most fruit any tree carries.
  const OAK_ANCH = [], OAK_BANCH = [];
  { const AMAX = 96, BMAX = 32, EDGE = 3;
    const fol = new Uint8Array(256); for (const f of foliageIds) fol[f] = 1;   // built AFTER the FRUITC loop above, so a berry on a bush reads as canopy and can never be mistaken for a branch
    for (let k = 0; k < OAKV.length; k++) {
      const m = OAKV[k], sx = m.sx, sy = m.sy, sz = m.sz;
      const ext = voxShellAir(m).ext;
      const clear = (x, y, z, n) => { for (let d = 1; d <= n; d++) { const zz = z - d;
        if (zz < 0 || !ext[x + y * sx + zz * sx * sy]) return false; } return true; };
      // ── AND THE HIVE NEEDS ITS WHOLE BOX CLEAR, NOT A COLUMN (user 2026-08-18: "the beehive is clipping
      // through the tree") ── `clear` above tests a 1x1 column, which is right for a fruit (one voxel on a
      // string) and wrong for a beehive: HIVEV is 5x5x5, so an anchor whose column happened to be open still
      // put four of the hive's five courses through whatever branches and leaves stood beside it. Widening the
      // test to the model's real footprint is what stops it intersecting the crown at all.
      // EXTERIOR air, like the column test — these crowns are hollow shells, so an "empty" cell is very often
      // inside the dome, and a hive hung there is both buried and clipping.
      const hvx = (typeof HIVEV !== 'undefined' && HIVEV) ? (HIVEV.sx >> 1) : 2;
      const hvy = (typeof HIVEV !== 'undefined' && HIVEV) ? (HIVEV.sy >> 1) : 2;
      const clearBox = (x, y, z, n) => {
        for (let ddx = -hvx; ddx <= hvx; ddx++) for (let ddy = -hvy; ddy <= hvy; ddy++) {
          const xx = x + ddx, yy = y + ddy;
          if (xx < 0 || xx >= sx || yy < 0 || yy >= sy) return false;
          for (let d = 1; d <= n; d++) { const zz = z - d;
            if (zz < 0 || !ext[xx + yy * sx + zz * sx * sy]) return false; }
        }
        return true;
      };
      const A = [], B = [], BW = [];   // BW: branch anchors with room for the whole hive, see clearBox below
      for (const p of m.vox) {
        const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
        if (x < EDGE || x >= sx - EDGE || y < EDGE || y >= sy - EDGE || z < 6) continue;
        if (fol[p >>> 24]) { if (z >= (sz >> 2) && clear(x, y, z, 4)) A.push(x | (y << 8) | (z << 16)); }
        else if (clear(x, y, z, 6)) { B.push(x | (y << 8) | (z << 16)); if (clearBox(x, y, z, 5)) BW.push(x | (y << 8) | (z << 16)); }   // BW = the subset with the HIVE'S WHOLE BOX clear; B stays the old column test as the fallback
      }
      const ang = (q) => Math.atan2(((q >> 8) & 255) - sy * 0.5, (q & 255) - sx * 0.5);
      const trim = (a, n) => { a.sort((p, q) => ang(p) - ang(q));
        if (a.length <= n) return a;
        const out = []; for (let i = 0; i < n; i++) out.push(a[(((i + 0.5) / n) * a.length) | 0]);
        return out; };
      // ── PREFER A CLEAR BOX, BUT NEVER RETURN NOTHING (user 2026-08-18) ── requiring the hive's full 5x5x5 of
      // exterior air is what stops it clipping the crown, and on its own it is too strict: it emptied the list
      // for every model and hives vanished from the world entirely (measured — hiveDbg searched 400 cells and
      // found none). So the wide test SELECTS when it can and the old column test stands behind it. A crown
      // with no roomy branch still gets a hive, just the tucked-in one it had before, rather than none.
      OAK_ANCH.push(trim(A, AMAX)); OAK_BANCH.push(trim(BW.length ? BW : B, BMAX));
    }
    if (OAKV.length) console.log('[vb] oak anchors: fruit', OAK_ANCH.map((a) => a.length).join('/'),
      '| hive', OAK_BANCH.map((a) => a.length).join('/'), '(caps', AMAX, '/', BMAX + ')'); }
  const PINE_ANCH = [];                                // pinecone anchors: canopy foliage voxels with open air below, ≥2 in from the model edge (base rotation)
  { const fol = new Uint8Array(256); for (const f of foliageIds) fol[f] = 1;
    for (let z = 30; z < MSZ; z++) for (let y = 2; y < MSY - 2; y++) for (let x = 2; x < MSX - 2; x++) {
      const i = x + y * MSX + z * MSX * MSY;
      if (M[i] && fol[remap[M[i]]] && !M[i - MSX * MSY]) PINE_ANCH.push(x | (y << 8) | (z << 16));
    }
    PINE_ANCH.sort((a, b) => Math.atan2(((a >> 8) & 255) - MSY * 0.5, (a & 255) - MSX * 0.5) - Math.atan2(((b >> 8) & 255) - MSY * 0.5, (b & 255) - MSX * 0.5)); }   // angle-sorted around the trunk — stampTree slices it into sectors so cones ring the crown evenly
  // ── CONSOLE TAP: WHAT A FLOWER ID REALLY CARRIES ── the same service lilyIds() does for the pads, and it
  // exists for the same reason they needed one: flowers.vox is parsed in SHARE mode (PAL_TOL 6 — see
  // parseVoxVariants2 in assets/bow.js), so a flower colour may RESOLVE onto an id something else already owns,
  // and every table above is per-ID. Marking the flowers choppable is therefore only honest if nothing ELSE
  // wears one of those ids, and this is the read that settles it rather than asserting it: `own` true means the
  // id was minted and reserved for the flower so nothing can have shared onto it, and grass/oakmoss/shrub say
  // whether one of the other scatter sets is standing on the same slot. It lives here rather than in
  // main/debug-api.js because every one of these tables is declared in this fragment or the one above it.
  window.__vbFlowerMat = () => FLOWERIDS.map((i) => ({ id: i, col: palette[i], own: palOwn.has(i),
    n: FLOWERV.concat(FLOWERV_CH).reduce((a, m) => a + m.vox.reduce((b, q) => b + ((q >>> 24) === i ? 1 : 0), 0), 0),   // how many voxels of the six models wear it — a shared id nothing is painted with is not a leak worth paying for
    z: FLOWERV.concat(FLOWERV_CH).flatMap((m) => m.vox.filter((q) => (q >>> 24) === i).map((q) => (q >> 16) & 255)).sort((a, b) => a - b),   // …and how high up the plant, which is what says whether an id left out of decorTab would leave a stub or a hole
    petal: FLOWERHEAD.indexOf(i) >= 0, decor: !!decorTab[i], solid: !!solidTab[i], float: !!floatTab[i],
    folia: !!foliaTab[i], wood: !!woodTab[i], cone: !!coneTab[i], snow: !!snowTab[i], hang: !!hangTab[i],
    axe: !!axeOnlyTab[i], pick: !!pickOnlyTab[i], dig: !!digOnlyTab[i],
    grass: GRASS.indexOf(i) >= 0, oakmoss: OAKMOSS.indexOf(i) >= 0, shrub: SHRUBC.indexOf(i) >= 0 || SHRUBF.indexOf(i) >= 0 }));
  if (palette.length > 256) console.error('[vb] PALETTE OVERFLOW', palette.length, '— world ids are u8, decoration colors must be quantized harder');
  console.log('[vb] decorations: cone', !!CONEV, 'lily', LILYV.length, 'stick', STICKV.length, 'log', !!LOGV, 'rock', !!ROCKV, 'rocks26', ROCK26.length, 'ferns', FERN2V.length, 'shrubs', SHRUBV.length, 'anchors', PINE_ANCH.length, 'palette', palette.length);

