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
  for (const r of ROCK26) for (const q of r.vox) { decorTab[q >>> 24] = 1; pickOnlyTab[q >>> 24] = 1; }   // …and the 26 BOULDERS (user), which want the PICK. All 26 share one 12-shade palette, so this marks every rock in the world.
  for (const i of [...ROCK, ...ROCKX, ...BROCK, ...ORECOAL, ...OREIRON]) rockTopTab[i] = 1;   // ── WHAT COUNTS AS ROCK UNDERFOOT ── strata, the partner shades, BOULDERS (dedicated ids, so pickOnlyTab never sees them) and ore. Read by the land-mammal spawn test; a 1-voxel PEBBLE is deliberately not here — it is ground scatter, and excluding it over an 11-voxel footprint would reject most of the forest floor.
  for (const i of [...ROCK, ...ROCKX, ...ORECOAL, ...OREIRON]) { decorTab[i] = 1; pickOnlyTab[i] = 1; }   // …and COAL + IRON (user): ore belongs to the pick like the stone it sits in   // …the STONE STRATA under the soil (user) belong to the PICK, not the shovel: dig down with the shovel, then swap and keep going
  const woodTab = new Uint8Array(256);                 // ── WOOD ── the axe takes chunks out of anything made of wood voxels (user), including a stump the
  for (const i of woodIds) { woodTab[i] = 1; decorTab[i] = 1; axeOnlyTab[i] = 1; }   // felled tree left behind, which belongs to no tree shape any more
  for (const i of [...DIRT, ...MOSS, ...NEEDLE, ...SAND]) { decorTab[i] = 1; digOnlyTab[i] = 1; }   // …and the SOIL (user): dirt, the mossy grass on top of it, the brown pine litter that covers most of the forest floor, and beach sand. NOT the stone strata underneath — the shovel stops at rock (user).   // …and the GROUND ITSELF (user), dug only with the SHOVEL: DIRT (the buried layers), MOSS (green surface), NEEDLE (the brown pine litter — most of the forest floor, and what reads as 'dirt' underfoot) and SAND (beaches, lakebed). GRASS (the strands) is separate walk-through decor and stays any-tool.
  // ── SURFACE SCATTER ── grass strands, flowers, twigs and pinecones. None of it is choppable decor, so
  // none of it is in decorTab — but all of it needs something underneath, which is what floatTab marks.
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
  const coneTab = new Uint8Array(256);                 // PINECONE ids — no hitbox for the PLAYER (see solid()); every other system still treats them normally
  for (const i of [...GRASS, ...BLOOM]) floatTab[i] = 1;
  for (const m of STICKV) for (const q of m.vox) floatTab[q >>> 24] = 1;            // twigs (user)
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
    for (const r of ROCK26) markSolid(r); }
  solidTab[ED_WHITE] = solidTab[ED_GREY] = solidTab[ED_HLITE] = 1;                          // the editor stage is walkable floor
  const PINE_ANCH = [];                                // pinecone anchors: canopy foliage voxels with open air below, ≥2 in from the model edge (base rotation)
  { const fol = new Uint8Array(256); for (const f of foliageIds) fol[f] = 1;
    for (let z = 30; z < MSZ; z++) for (let y = 2; y < MSY - 2; y++) for (let x = 2; x < MSX - 2; x++) {
      const i = x + y * MSX + z * MSX * MSY;
      if (M[i] && fol[remap[M[i]]] && !M[i - MSX * MSY]) PINE_ANCH.push(x | (y << 8) | (z << 16));
    }
    PINE_ANCH.sort((a, b) => Math.atan2(((a >> 8) & 255) - MSY * 0.5, (a & 255) - MSX * 0.5) - Math.atan2(((b >> 8) & 255) - MSY * 0.5, (b & 255) - MSX * 0.5)); }   // angle-sorted around the trunk — stampTree slices it into sectors so cones ring the crown evenly
  if (palette.length > 256) console.error('[vb] PALETTE OVERFLOW', palette.length, '— world ids are u8, decoration colors must be quantized harder');
  console.log('[vb] decorations: cone', !!CONEV, 'lily', LILYV.length, 'stick', STICKV.length, 'log', !!LOGV, 'rock', !!ROCKV, 'rocks26', ROCK26.length, 'ferns', FERN2V.length, 'anchors', PINE_ANCH.length, 'palette', palette.length);

