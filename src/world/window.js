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
  const LGT2_ALL = 0x1;                                // ── SECOND TERM MASK (u.lgt.z) ── lgt.x is full at 24 bits (an f32 is exact only to 2^24, so a 25th bit there would round), so this is where a 25th term goes. Three groups have lived and died here on 2026-08-09: the water soft glisten (bit 0), the tier-1 LOOK set (bits 1-6) and the tier-2 set (bits 0-3). BIT 0 IS NOW THE SUN SHEEN ON STONE (user 2026-08-16) — __vb.lgt2(0) turns it off and __vb.lgt2(1) back on, which is the A/B this effect is judged with; the 31 bits above it are still free.
  // ══ WATER BAKE (user 2026-08-05) ══ THE defaults for every water control. Tune with the top-right panel,
  // hit `copy` on its bake row, and paste the line it gives you OVER this one — that is the whole workflow.
  // A player who has never touched the panel gets exactly what is written here; `reset` in the panel puts a
  // live session back to it. `reflection` is the Fresnel mirror weight (1 = physical Schlick), the rest are
  // on/off. Anything the player HAS changed is remembered in localStorage and wins until they hit reset.
  // ── WHY THIS LIVES HERE AND NOT IN ui/settings.js WITH THE REST OF THE WEATHER ── render/wgsl/trace.js is
  // manifest line 29 and ui/settings.js is line 60, and the shader interpolates RAIN_ON straight into its
  // source. A const read before its own declaration is the "stuck on uploading world" black screen this
  // codebase has hit before (see the notes in world/build.js), and the linter cannot see inside a template
  // literal to catch it. world/window.js is line 14, above every consumer.
  // ── RAIN: OFF (user 2026-08-17: "disable the rain from the oak forest") ── ONE named switch, the way
  // SHRUB_ON is one, because the rain is four separate pieces in four files and turning it off by deleting
  // any one of them leaves the other three describing a feature that no longer exists:
  //   * render/wgsl/trace.js — the rain DDA march, and the cull that keeps SNOW off the oak forest
  //   * main/tick-snow.js    — the accumulation gate that stops a blanket forming under the rain
  //   * main/tick-camera.js  — the storm sky's rainK, which is what darkens the cloud and dims the sun
  //   * main/tick-body.js    — the freeze gate that stops rain skinning the oak lakes with ice
  // With this false the oak forest goes back to SNOWING like the rest of the world: the snow cull stops, the
  // blanket forms again, the ice returns, and the sky keeps its ordinary storm look. That is deliberately the
  // full revert rather than "no drops": disabling only the march would leave the oak forest the one biome with
  // no weather at all, and the sky still going dark for a rain that never falls.
  // The rain code is all still here and none of it is deleted. Flip this to true to bring it back.
  const RAIN_ON = false;
  // ── AND NO SNOW EITHER (user 2026-08-17: "turn off the snow in the oak forest") ── a SEPARATE switch from
  // RAIN_ON, because the two were entangled and the pair of requests is not one request. The rain work culled
  // snow over the oak forest as a side effect of putting rain there; switching the rain off handed the snow
  // back, which is what "turn off the snow" is now undoing. Two flags, four states, and all four are coherent:
  //     RAIN_ON  OAK_SNOW   the oak forest gets
  //     false    false      NOTHING - the current setting: no drops, no flakes, no blanket, no ice
  //     false    true       snow, exactly like the pine forest and the desert rim
  //     true     false      rain, and no snow under it (what the rain feature shipped as)
  //     true     true       both at once - allowed, and it would look wrong; nothing enforces it
  // The three things this drives are the flake CULL in the shader (a flake is a GPU lattice voxel, so the CPU
  // cannot reach it), the ACCUMULATION gate in tick-snow, and the FREEZE gate in tick-body - because a lake
  // skinning over with ice in a biome where no snow is falling is the same wrongness as rain freezing one.
  const OAK_SNOW = false;
  const WATER_BAKE = { reflect: 1, refract: 1, foam: 1, ice: 1, pixelGlisten: 1, waves: 0, reflection: 0.45 };
  const WBIT = { reflect: 18, refract: 19, foam: 20, ice: 21, pixelGlisten: 22, waves: 23 };   // …their bits in u.lgt.x
  const wBakeMask = () => { let m = LGT_ALL & ~LGT_WATER; for (const k in WBIT) if (WATER_BAKE[k]) m |= (1 << WBIT[k]); return m; };
  const wBakeMask2 = () => LGT2_ALL;                   // the second mask has no per-term panel rows, so its bake IS its default — `reset` puts the rock sheen back on (see LGT2_ALL)
  const wBakeRefl = () => { const v = +WATER_BAKE.reflection; return (isFinite(v) && v >= 0 && v <= 2) ? v : 1; };
  // Everything OUTSIDE the water group is FORCED ON at load. The panel used to carry all 24 terms, so a
  // saved mask can have sun shadow / AO / fog / TAA switched off from an earlier bisection — and with those
  // rows gone there would be no way left to switch them back. Only the water bits are restored from storage.
  let lgtMask = (() => { try { const v = localStorage.getItem('vb_lgt');
    return v === null ? wBakeMask() : (((parseInt(v, 10) & LGT_WATER) | (LGT_ALL & ~LGT_WATER)) & LGT_ALL); } catch (e) { return wBakeMask(); } })();
  let lgtMask2 = LGT2_ALL;                             // …and this starts at the bake. Deliberately NOT restored from localStorage the way lgtMask is: every bit in here is a whole-scene look term, and a player who bisected one off in an old session must not be stuck with it (the same argument that forces every non-water bit of lgtMask on at load).
  // ── WATER REFLECTION STRENGTH (user 2026-08-05) ── multiplies the Fresnel mirror/transmission split.
  // 1 = physical (pure Schlick, what it has always been), 0 = no mirror at all, 2 = twice as reflective.
  let wReflK = (() => { try { const v = parseFloat(localStorage.getItem('vb_wrefl')); return (isFinite(v) && v >= 0 && v <= 2) ? v : wBakeRefl(); } catch (e) { return wBakeRefl(); } })();
  const REACT_FADE = 450;                              // ms for the reactive mask to fade out after the last body motion. Long enough that a trunk jittering in and out of sleep on its contacts reads as one continuous settle rather than a strobe.
  let reactT0 = -1e9;                                  // when a rigid body was last in motion (see physC.y / the reactive mask)
  let winOX = 0, winOZ = 0;                            // world coord of the window corner (multiples of 8)
  const rect = { xlo: 0, xhi: 0, zlo: 0, zhi: 0 };     // the fully-GENERATED world rectangle (8-aligned) — only terrain inside it is ever traced
  const gwrap = (v, n) => ((v % n) + n) % n;
  // ── HOW FAR THE PLAYER STARTS FROM THE BAND ANCHOR ── the biome bands are anchored to SPWX, so the player
  // is placed at SPWX + SPOX instead of moving SPWX itself (see sim/player.js). Declared HERE because the
  // STREAMING WINDOW has to be centred on the player rather than on the anchor: HALF is at most 1024 and SPOX
  // is 2160, so a window built around the anchor does not contain the player at all — which is the world
  // loading in around you after you have already spawned (user 2026-08-22).
  let SPOX = 0;
  let SPWX = 0, SPWZ = 0;                              // world spawn — placeholder; RANDOMISED on every refresh at boot (user 2026-07-20), see the spawn block below
  let SPYAW = 1.5708; const SPPITCH = -0.044;   // ── FACING EAST, AT THE BLOSSOM, WITH THE PINE TREELINE AT YOUR BACK (user 2026-08-21: "spin the player
  // around facing the cherry forest") ── heading is (sin(yaw), cos(yaw)), so +pi/2 looks down +x, and +x is where the pink is: the blossom's centre is
  // SPWX - CHOFF and CHOFF is NEGATIVE (see BAND_MIRROR), so the band sits at spawn +2960 with its near edge 1880 ahead, the whole oak strip away. The dawn
  // sun rises in +x too, which is the entire point of BAND_MIRROR (user 2026-08-20: "running into the sun while running in the direction of the cherry
  // forest") ─ that pairing is restored by this turn, and it was the one thing the previous revision gave up.
  // THE SPAWN SLIDE STAYS, AND THAT IS THE WHOLE POINT OF THEM BEING TWO KNOBS. Only the heading moved here: the player still stands 280 from the oak/pine
  // line (see the slide note above OAKOFF), so "at the edge of the pine forest, but in the oak forest" is still true to the voxel ─ the treeline is simply
  // 28 m BEHIND the start pose instead of in front of it, and one turn on the spot brings it back. Anyone re-reading this later: do NOT "fix" the slide to
  // match the heading. The position was asked for on its own and is unrelated to which way the camera points.
  // NOTHING ELSE NEEDS TO MOVE for the re-aim: both spawn sight-line corridors ─ treeAt's and oakAt's ─ build their forward vector from SPYAW rather than
  // assuming +x, precisely so re-aiming the camera drags the cleared lane with it, and the corridor now opens the view back down the oak strip. Check that
  // is still true if either is ever rewritten.
  // Was, for one revision: ── FACING WEST, AT THE PINE TREELINE (user 2026-08-21: "facing the direction of the pine forest") ─ same 280 stance, turned round.
  // And before that: ── FACING EAST, DOWN THE OAK STRIP AT THE BLOSSOM (and into the sunrise — see BAND_MIRROR) (user 2026-08-20: "facing the cherry forest direction") ── -pi/2 is -x, and the cherry band's centre is SPWX - CHOFF, i.e. due WEST of spawn, so this already pointed at it; the 2026-08-20 band slide kept it that way deliberately (see the map above). Was: ── FACING WEST, INTO THE ENDLESS OAK FOREST (user
  // 2026-08-17) ── heading is (sin(yaw), cos(yaw)), so -pi/2 looks down -x: away from the pine treeline
  // 42 m behind you and out over oak wood that runs to the horizon and never ends. It was +pi/2 for two
  // earlier reasons that have both expired - it aimed at the desert when the sand was 80 voxels east
  // (2026-08-15), then at the pine treeline once spawn moved into the oaks. Turning round is deliberate:
  // the first thing you see should be the biome you are standing in, not the edge of it.
  // NOTHING ELSE NEEDS TO MOVE. Both spawn sight-line corridors - treeAt's and oakAt's - derive their
  // forward vector from SPYAW rather than assuming +x, precisely so re-aiming the camera drags the
  // cleared lane with it. Check that is still true if either is ever rewritten.
  // (was: FACING EAST, DOWN THE BIOME GRADIENT.)

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
    return Math.min(HMAX, Math.max(4 + LIFT, Math.round(oakRoll(b + 9 * fbm(x * 0.04 + 7.3, z * 0.04 + 2.1) * (0.2 + 0.8 * shoreK), x, z))));   // ── ROUNDED OAK HILLS ── the forest expression is untouched; oakRoll (below, with oakM) either hands it straight back or replaces it with the oak forest's own rounded field. makeHRow and makeHCol wrap their own copies of this same expression in the same call
  };
  const basinM = (x, z) => {                           // huge, rare low-frequency basins pull the land under the waterline (threshold halved — lakes are rarer)
    const b = vnoise(x * 0.0016 + 313.7, z * 0.0016 + 157.3);
    if (b >= 0.065) return 0;
    return sstep(Math.min(1, (0.065 - b) / 0.06));
  };
  // ── THE DESERT ── the EASTERNMOST of the world's three bands (oak forest | pine forest | desert; see
  // oakM below for the other border). Anchored to SPWX/SPWZ rather than fixed coordinates because spawn is
  // re-randomised on every refresh (see build.js) - a fixed centre would land in a different place
  // relative to the player every session, or nowhere near them at all.
  // It is no longer centred on the spawn and the player no longer starts in it: as of 2026-08-17 they start
  // in the oak forest, two bands west, and the sand is a walk rather than a view.
  // The rim is a distance field wobbled by low-frequency noise, never a circle, and it BLENDS over DESB
  // rather than switching: H is continuous noise, so a hard mask would cut a cliff along the border. That
  // is the same reason the sand-to-forest surface transition below dithers instead of snapping.
  // ── EVERY BIOME STRIP IS THE SAME WIDTH, AND THAT WIDTH IS 2160 (user 2026-08-19: "make all of the biomes
  // the exact same size. no exceptions. double the size of the bands") ── even by ARITHMETIC, not by eye, and
  // measured the way the 2026-08-18 pass measured it: between each mask's own MIDPOINT, the 0.5 iso-line,
  // which is where every gate in the game splits.
  //
  // WHAT "ALL OF THEM, NO EXCEPTIONS" HAS TO MEAN. Walking east the player crosses SIX strips per period, not
  // four, because two of the four biomes are cut in half by a third. Pine sits on BOTH sides of the desert
  // (deliberately — see the meander note under OAKOFF) and oak sits on BOTH sides of the blossom (that is what
  // "a band with oak either side of it" means — see cherryM). A biome is a thing you WALK THROUGH, so the STRIP
  // is the unit and there are six of them. The other reading — four equal biome TOTALS — would hand the player
  // 2160 of desert against 1080 of pine and call them equal, which is the exact complaint the 2026-08-18 pass
  // was answering ("the oak forest band is very tiny between the cherry forest and the pine forest").
  //
  // AND THE 2026-08-18 PASS ONLY EVENED FOUR OF THE SIX. It counted "oak strip = OAKOFF - 540 = 1080" and never
  // looked WEST of the blossom, where the leftover oak was 625; and the second pine strip came out 1075 against
  // the first's 1080. Both were accidents of BIOP being reached by ADDITION rather than set by multiplication.
  //     W    = 2160 = 2 * 1080, the doubling asked for.
  //     BIOP = 6 * W = 12960, by construction — an inconsistent BIOP is precisely what makes the cycle stop
  //            being seamless, and the old 7100 was 6 * 1183.3.
  // The one strip that does NOT move is the CHERRY forest: it was doubled to 2160 on 2026-08-18 and is already
  // the target, so CHOFF/CHHALF/CHB/CHW are untouched and spawn's position inside the blossom is bit-identical.
  // Every other strip grows to meet it.
  // (That is the 2026-08-19 table, and it is HISTORY twice over: BAND_MIRROR flipped every centre on 2026-08-20 and
  // "one biome per band" deleted the second oak strip on the same day, so the period is FIVE strips, not six, and
  // it runs the other way round. THE CURRENT MAP, as signed distances from spawn along +x, spawn at 0:
  //     pine    -6760 .. -4600   2160   (the wrap: the oak band's west edge less one BIOP)
  //     desert  -4600 .. -2440   2160   = 2 * DESH,  outer edge <- -DESOFF
  //     pine    -2440 ..  -280   2160
  //     oak      -280 .. +1880   2160   <- the oak/pine line is -OAKOFF, i.e. 280 voxels WEST of spawn, and SPYAW faces it
  //     cherry  +1880 .. +4040   2160   = 2 * (CHHALF + CHB),  centre -CHOFF   <- and the oak BAND's far edge, -OAKWOFF
  // BIOP = 5 * 2160 = 10800. Every line in it is a signed distance, so a WEST band reads negative; the constants
  // themselves are the unmirrored magnitudes and BAND_MIRROR carries the sign — see OAKC/DESC/CHOFF.)
  //
  // ── SPAWN SLID 600 EAST INSIDE THE OAK STRIP (user 2026-08-20: "spawn the player in the oak forest but near
  // the pine forest, facing the cherry forest direction") ── every offset above moved by the SAME 600, so the
  // arrangement is translated and not reshaped: every strip is the 2160 it was, and the equal-strip invariant
  // the note below is written against still holds to the voxel. Only where SPAWN sits inside it changed —
  // from the middle of the east oak strip to 620 from the pine line, which is inside pure oak (OAKB is 450, so
  // the blend reaches only 225 either side) with the pines in plain view. (620 was still too far back — 2026-08-21
  // repeats this exact move with 340 and lands on 280; see the slide note above OAKOFF.) CHOFF moves the OPPOSITE way because
  // it is measured WESTWARD: the blossom's centre is SPWX - CHOFF, so +600 there is -600 in signed distance,
  // which keeps the cherry band where it was relative to the pines and leaves it dead ahead of the start yaw.
  //
  // THE OAK *BAND* IS 6480 WIDE AND MUST NOT BE 2160. cherryM is a SUB-REGION of oakM, not a disjoint band (the
  // long note under cherryM says why, and it is load-bearing in both directions), so oakM has to CONTAIN the
  // blossom: the band is oak-strip + cherry + oak-strip = 3 * W. What the player WALKS THROUGH either side of
  // the pink is 2160 of pure oak, and that is the number this rule is about. The two half-infinite ends the
  // 2026-08-18 note had to apologise for are gone outright now that both oak strips are finite and equal.
  // AND OAKC IS NOW EXACTLY THE BLOSSOM'S OWN CENTRE — both are SPWX - 940. That is not a coincidence to be
  // tidied away later: two EQUAL oak strips means the two bands are CONCENTRIC, which is the cheapest possible
  // statement of the containment invariant. Nothing can bring their edges together except the residual meander,
  // and that is bounded at 235.7 voxels against 1599.3 of clearance (see the note under OAKWOFF).
  // == THE BANDS CYCLE, ENDLESSLY (user 2026-08-18: "do the biomes cycle over and over again? I'm going
  // through seemingly endless oak forest. make sure the landscapes keep cycling, endlessly") ==
  // Every mask in this file was a HALF-PLANE - desertM is "east of a line", oakM is "west of a line" - so the
  // world had two ends: walk far enough east and the desert never stops, far enough west and the oak forest
  // never stops. The second of those is what the player walked into.
  // The fix is one coordinate rather than four masks: each band is measured on a distance that WRAPS every
  // BIOP voxels, so the arrangement that already exists repeats. Two things had to change for that to be
  // seamless. oakM gained a WEST edge (a half-plane has no far side to meet the wrap with), and desertM became
  // a BAND like the cherry forest's, which is what lets one band cover both the east end of a period and the
  // west end of the next.
  // AND PINE SITS ON BOTH SIDES OF THE SAND, which is not decoration: the long note under OAKOFF works out
  // that two meanders can converge until the oak forest touches the sand with no pine between them, and it is
  // the reason the wobbles are part-shared in the first place. Both pine seams are now the identical 2160 gap,
  // so both inherit the identical guarantee — 1710 of pure pine nominal, >= 1357.2 worst case, against the 272
  // the 1075-wide strip used to promise. Doubling the strips turned the tightest seams in the world into the
  // loosest ones.
  // …and the CYCLE loses that strip with it: FIVE strips of 2160, oak, cherry, pine, desert, pine.
  // BIOP must be the sum of the strips that actually exist or the pattern stops tiling and the bands drift
  // apart at the wrap — it is derived by construction, never measured off the pieces.
  // The desert is deliberately NOT moved: its centre is already 3860 west of spawn, which inside a 10800
  // period is the same column as 6940 EAST, i.e. exactly the fourth strip. The pine that remains on both
  // sides of the sand stays on purpose — it is what keeps the desert from ever bordering the oak, which
  // sim/nav.js's BIO_SANDLINE reasoning depends on.
  const BIOP = 10800;                                  // one full cycle: SIX strips of 2160 — oak, cherry, oak, pine, desert, pine. 6 * W by construction, never a sum of measured pieces
  const pwrap = (d) => d - Math.floor(d / BIOP + 0.5) * BIOP;   // signed distance into [-BIOP/2, BIOP/2). floor(x + 0.5) rather than Math.round because the WGSL port must agree with this bit for bit, and WGSL's round() breaks ties to EVEN where Math.round breaks them upward
  const DESOFF = 1080, DESB = 450, DESW = 1000;        // how far the pine/desert line sits EAST of spawn; blend width; boundary meander (voxels, 10 cm each). 2300 -> 4460 = OAKOFF + W, so the pine strip between the oak line and the sand is one full 2160-wide strip like every other strip in the period. DESB is deliberately NOT doubled with it — a blend is a TREELINE, not a biome, and widening it would drag life's 0.15/0.85 admit ends (main/tick-creatures.js) and the weather contrast curve along with it
  // History, because the number has moved three times and each move had a different reason: 500 -> 300 (user
  // 2026-08-15) because 50 m of dense pine hid the thing the spawn camera was aimed at; then 300 -> 80; then
  // 80 -> 1500 below, which abandons "the sand is visible from spawn" outright rather than tuning it, because
  // spawn is no longer in the pine forest at all.
  // 80 -> 1500 (user 2026-08-17, "spawn the player here"): spawn moved into the NEW oak forest, and the three
  // bands run oak / pine / desert west to east, so the desert can no longer sit 8 m from the player - the whole
  // pine forest is now between the two. DESOFF is what buys the pine band its width; see OAKOFF below for the
  // arithmetic that keeps oak and desert from ever touching. The cost is that the sand is no longer visible
  // from spawn (150 m against a 100 m view distance) - it is a two-minute walk east instead of a glance.
  // 80 -> 1500 -> 2300 (even bands, 2026-08-18) -> 4460 (equal strips, 2026-08-19). The reason is the same one
  // each time it has moved since spawn left the pine forest: DESOFF is not a view-distance knob any more, it is
  // whatever the strip arithmetic at the top of the file says the pine forest is owed. The sand is now 446 m
  // east of spawn, and the pine between them is a real walk rather than a screen's worth of trees.
  const DESY = WL + 7;                                 // the flat: high enough that no basin or river bed can flood it
  // ── DUNE RELIEF, ON A 1-10 SCALE (user 2026-08-15) ── the rest of the terrain is NOT on a scale: its
  // amplitudes are raw voxel counts buried in the noise expressions (the forest is `88 * fbm` for the base plus
  // `9 * fbm` for detail, i.e. about +-44 voxels). This is the one knob that is, and the unit is plain: DESREL
  // is the PEAK-TO-PEAK height of the dunes in voxels, so 1 is a billiard table, 10 is +-5 voxels of roll. It
  // was effectively 4 before this. Putting the FOREST on the same 1-10 scale would need a different unit —
  // +-44 vs +-3 is two orders of magnitude, so one linear scale leaves the desert no usable resolution.
  const DESREL = 24;                                   // FIXED at 24 (user 2026-08-15). The ?desrel override and the L knob that drove it are both gone — this is the shipped relief, not a tuning dial.
  // ── DUNES (user 2026-08-15: "hilly, like with dunes") ── a SECOND, much longer-wavelength field on top of
  // the fine relief. DESREL alone could never make dunes however high it went: its wavelength is ~83 voxels
  // (8 m), so raising it just made the ripples taller and read as squiggly contour noise. Dunes need distance
  // between crests, so this runs at 0.0022 (~450 voxels, 45 m) with a half-amplitude second octave for shape.
  // It is deliberately POSITIVE-ONLY (fbm, not fbm-0.5): dunes are relief piled ON a plain, and keeping the
  // term above zero means the sand floor never drops toward the waterline - which is half of the no-water
  // guarantee below. ?desdune=N overrides live, same as ?desrel.
  const DESDUNE = (() => { const m = /[?&]desdune=(\d+(?:\.\d+)?)/.exec(location.search); return m ? +m[1] : 26; })();
  const duneH = (x, z) => {
    const a = fbm(x * 0.0022 + 61.3, z * 0.0022 + 17.9);
    const b = fbm(x * 0.0051 + 12.7, z * 0.0051 + 73.1);
    // ── ROUNDED, NOT NOISY (user 2026-08-15: "more curvy... more round") ── two changes, both about SHAPE
    // rather than height. The fine octave drops 0.28 -> 0.16 because it was the part adding wobble to a crest
    // instead of adding a crest. Then the combined field is smoothstepped TWICE: sstep flattens a signal near
    // 0 and 1 and steepens it through the middle, so applying it to the dune height domes the tops, flattens
    // the interdune floors, and rounds the shoulders between them. fbm on its own is linear-ish through its
    // range, which is exactly what reads as lumpy noise rather than as dunes.
    const n = a * 0.84 + b * 0.16;
    return sstep(sstep(n)) * DESDUNE;                  // 0 .. DESDUNE, never negative
  };
  // ── THE DESERT ── a HALF-AND-HALF split, not an island: one wandering boundary line with open sand on the
  // east side of it and pine forest on the west, both running to the horizon. (The first pass made the desert a
  // disc, which meant the forest encircled it — not what was asked for.)
  // The line is anchored to SPWX/SPWZ rather than to fixed coordinates because spawn is re-randomised on every
  // refresh (see build.js) - a fixed boundary would land somewhere different relative to the player each session,
  // or nowhere near them at all. DESOFF puts the player 50 m inside the sand, and the spawn camera faces west, so
  // the treeline is straight ahead at boot with the desert running away behind.
  // It BLENDS across DESB rather than switching: H is continuous noise, so a hard edge would cut a cliff along
  // the whole border. Same reason the surface colour dithers on the mask weight instead of snapping.
  // ── THE BORDERS SWEEP IN S-CURVES, NOT WIGGLES (user 2026-08-19: "have the transition between biomes have a
  // more random shape vs just being a straight line. maybe an s like pattern?") ── the meander was already
  // here and it was already two octaves; what made it read as a ruled line is that BOTH dials were wrong for
  // the size the bands have since become. The amplitudes were set when a strip was half its present width
  // (the equal-strip pass doubled them), so +-160 voxels against a 2160-wide strip is a 7% waver; and the
  // primary octave ran at 0.0011, a ~900-voxel feature, so what little swing there was reversed before the
  // eye could read it as a curve. An S is a LONG wavelength at a LARGE amplitude, so both moved together:
  // the wavelength roughly doubles and the amplitude goes up ~1.75x, which is one sweep across ~1800 voxels
  // of northing — about how far you can see down a border — instead of two half-swings inside it.
  // THE CEILING IS THE PINE STRIP, and the sum that matters is NOT the one it looks like. Pine is the narrowest
  // band at 2160 and the only one squeezed from both sides, so a first pass held (desert swing + oak swing)
  // under 2160 and that capped the amplitudes at about half what the effect needs. It is the wrong sum:
  // oakWob CARRIES 0.6 of desWob, so writing the two edges out and subtracting,
  //     pine width = (DESOFF - OAKOFF) + 0.4 * dDesWob - dOwnOctave
  // and 60% of the desert's swing cancels because BOTH edges make it together — which is the entire point of
  // the carried term (see the note under desWob about why it is not laziness). The real worst case is
  // 0.4 * 2 * DESW * 0.675 + 2 * OAKW * 0.5 = 540 + 540 = 1080, leaving 1080 voxels of pine at the very worst
  // crossing rather than the ~0 the naive sum predicts. Cherry is checked the same way and CHREACH comes to
  // 2250 against a band that has 3240 either side of its centre, so it still cannot escape the oak forest.
  // Every derived bound below (OAKWMAX, CHREACH, the four short-circuits) reads off these constants rather
  // than a typed number, so they all moved with them — which is exactly why this was safe to change at all.
  // ── THE FOUR MEANDER FREQUENCIES, NAMED, BECAUSE THE SHADER NEEDS THEM TOO (user 2026-08-21: "the snow on
  // the ground needs to match the snow in the sky in terms of how far it's going … the pine forest falling snow
  // is too far out in the oak forest") ── render/wgsl/trace.js carries a port of all three wobbles, because a
  // falling flake is a GPU lattice voxel the CPU cannot reach, and every one of its four frequencies had drifted
  // to ~1.9x the numbers below:
  //     desWob 1   0.00060 -> 0.0011      oakWob   0.0014 -> 0.0027
  //     desWob 2   0.0022  -> 0.0043      chWob    0.0010 -> 0.0019
  // The CPU decides what SETTLES and the GPU decides what FALLS, so that is two borders wandering at different
  // wavelengths over the same world: at one z the flakes overshoot deep into the oak, at another they stop short
  // of the blossom — which is the report, both halves of it, and it is why the ground reads correct (the blanket,
  // the trees and the terrain all share the CPU line) while the sky does not.
  // NAMED AND INTERPOLATED so the class of bug is gone rather than the instance: trace.js now writes
  // ${WOB_OAK} instead of a typed literal, and a copy that cannot be typed cannot drift.
  const WOB_DES1 = 0.00060, WOB_DES2 = 0.0022, WOB_OAK = 0.0014, WOB_CH = 0.0010;   // four SCALARS rather than one object: the gen-pool/gen-worker registries carry scalars, and a shared table is a new shape for them to learn
  const desWob = (z) => (vnoise(z * WOB_DES1 + 27.9, 83.1) - 0.5) * DESW
                      + (vnoise(z * WOB_DES2 + 11.2, 51.7) - 0.5) * DESW * 0.35;   // two octaves: the long one is the S, the short one keeps it from reading as a drawn curve
  // ── THE OAK FOREST (user 2026-08-17) ── the world's THIRD band, and the one the player now starts in.
  // West to east the world reads OAK | CHERRY | OAK | PINE | DESERT | PINE (see the strip table at the top of
  // the file): the same shape as the desert border, one wandering north-south line with a blended rim, because
  // that is the only arrangement in which every biome is reachable on foot and none of them encircles another.
  // OAKOFF is measured EAST of spawn and is deliberately POSITIVE — spawn sits OAKOFF voxels inside the oak
  // side of the line, which is what makes "spawn in the oak forest" true by construction on every refresh
  // rather than a nudge applied afterwards. It is no longer a sight-line number: at 420 the pine treeline was
  // 42 m ahead and visible from spawn, but the strip arithmetic sets it now, so the pines are a walk away.
  //
  // ── WHY THE WOBBLE IS PART-SHARED WITH THE DESERT'S, AND WHY THAT IS NOT LAZINESS ── two independent
  // meanders of DESW's amplitude can swing 432 voxels each against spawn, so with fully separate noise the
  // two borders could close to within 216 voxels and the oak forest would touch the sand somewhere out along
  // z, with no pine between them. Carrying 0.6 of the desert's own wobble makes the two lines broadly
  // parallel and leaves only the 0.4 residual free to converge; adding OAKW of independent noise on top keeps
  // them from reading as ruled tramlines. Worst-case convergence is then 0.4*432 + 180 = 352.8 voxels against
  // a DESOFF - OAKOFF gap of 2160, so the two 450-wide blend bands can never overlap and at least 1357.2
  // voxels (135.7 m) of pure pine survives anywhere in the world — typically 1710. (At the old 1080 gap those
  // two numbers were 277 and 630. The seam did not get safer by being tuned; it got safer because the strip
  // doubled and the meander did not.) The SECOND pine strip, between the sand and the next period's oak, is
  // the identical arithmetic on an identical 2160 gap — see the note under DESH.
  // ══ SPAWN SLID 340 WEST AGAIN, ONTO THE PINE TREELINE (user 2026-08-21: "spawn the player at the edge of the pine forest, but in the oak forest …
  // basically just spawn the player further back towards the pine forest") ══
  // The 2026-08-20 pass put spawn 620 from the oak/pine line and called that "the pines in plain view"; the user has now called 620 too far back, so it is
  // 280. Same move as that pass and made the same way: FOUR offsets move by the SAME 340 so the arrangement is TRANSLATED and not reshaped ─ every strip is
  // still exactly 2160 wide, BIOP is still 5 * 2160, and the only thing that changed is where the player stands inside the pattern.
  //     OAKOFF   620 -> 280      the oak/pine line, i.e. how far the treeline is from the start pose
  //     OAKWOFF -3700 -> -4040   the oak band's far side, so the band keeps its 4320 (= 2 strips: oak + cherry)
  //     CHOFF   -2620 -> -2960   the blossom rides along, so it stays the oak band's outer strip exactly
  //     DESOFF   2780 -> 2440    the pine/desert line rides along, so the pine strip keeps its 2160
  // AND 280 IS NOT AN ARBITRARY 'NEAR'. OAKB is 450, so oakM reaches a hard 1 at OAKB/2 = 225 from the line: 280 leaves 55 voxels of cushion, which is what
  // keeps "spawn is in the oak forest by construction" literally true (oakM(SPWX, SPWZ) === 1, not 0.98) while putting the treeline 28 m ahead. It is exact
  // rather than approximate because oakM pins its meander at the spawn's own z ─ oakWob(z) - oakWob(SPWZ) cancels at SPWZ ─ and pwrap(SPWX - (SPWX + OAKC))
  // does not depend on SPWX, so build.js's eastward nudge off water cannot erode the cushion.
  // WHAT THE PLAYER SEES: pines are gated `oakM > 0.5 -> reject` (terrain.js treeAt) and oaks on `oakM < 0.5 -> reject` (oakAt), so the canopies split on the
  // mask MIDPOINT and the treeline is a line, 280 voxels dead ahead of the start yaw. Behind the player: 1880 of oak, then the blossom.
  // AND IT CANNOT BE DONE BY NUDGING SPWX ─ the note under CHOFF records a first attempt that tried; the bands are anchored to SPWX and simply come along.
  // ── AND THE WHOLE ARRANGEMENT SLID 1360 EAST, SO SPAWN IS IN THE PINE FOREST (user 2026-08-21: "spawn me in
  // the pine forest by default") ── the same translation move the 2026-08-20 (600) and 2026-08-21 (340) slides
  // made, for the same reason and by the same rule: every offset moves by the SAME amount, so the arrangement is
  // TRANSLATED and never reshaped. Every strip is still 2160 wide, BIOP is untouched, and the equal-strip
  // invariant holds to the voxel — only where spawn sits inside the pattern changed. Walking SPWX still cannot
  // work (see the note in world/build.js): the bands are anchored to SPWX itself, so moving the player moves the
  // forest with them. THE CURRENT MAP, signed distances from spawn along +x, spawn at 0:
  //     desert  -3240 .. -1080
  //     pine    -1080 .. +1080   <- spawn is DEAD CENTRE, 1080 from either edge
  //     oak     +1080 .. +3240
  //     cherry  +3240 .. +5400
  // 1360 is not a taste number: it is the old pine strip's own half-width (the strip ran -2440..-280, centre
  // -1360), so sliding by exactly that lands spawn on the centre line. 1080 of clearance against a blend that
  // reaches OAKB/2 = 225 either side of a line means the meander (bounded at 235.7 — see the note under
  // OAKWOFF) cannot put spawn anywhere but pure pine: worst case is 1080 - 235.7 - 225 = 619.3 of pure pine
  // still between spawn and the nearest edge. Measured after the change with __vb.om/cm/dm at spawn: 0/0/0.
  // SPYAW is UNCHANGED at +pi/2 (east): the player now looks down 1080 of pine to the oak line rather than
  // standing on it, so the note above SPYAW that says "at the blossom with the pine treeline at your back"
  // describes the arrangement this slide replaced.
  const OAKOFF = -1080, OAKB = 450, OAKW = 540;         // where the oak/pine line sits (the line is at -OAKOFF, so a NEGATIVE value puts it EAST of spawn — 1080 east, since the 2026-08-21 pine slide above); blend width; the INDEPENDENT half of the meander. 1220 -> 2300 = the blossom's east midpoint (spawn+140) plus one full strip W, so the PURE oak between the pink and the pines is 2160 like everything else. Spawn is still 2300 WEST of this line, so oakM(SPWX,SPWZ) is still exactly 1 and "spawn is in the oak forest by construction" still holds — the cherry band simply sits inside it
  const oakWob = (z) => desWob(z) * 0.6 + (vnoise(z * WOB_OAK + 143.7, 61.3) - 0.5) * OAKW;
  // ── THE WEST EDGE, AND WHY IT SITS WHERE IT DOES ── it is the OTHER oak strip, and as of 2026-08-19 it is
  // set the same way the east one is: the blossom's west midpoint (spawn - 2020) less one full strip W, i.e.
  // -4180. It used to be set by the CONTAINMENT BOUND instead — the least-negative west edge that still keeps
  // "every column with cherryM > 0 also has oakM === 1" true — and that is exactly why the west oak strip came
  // out 625 wide against the east one's 1080. The containment bound is still CHECKED, it just no longer decides
  // the number:
  //   the band has to CONTAIN the blossom band, because that invariant is load-bearing (see the long note under
  //   cherryM: the whole cherry forest is oak TERRAIN, and it inherits oakH's valley floors and oakBank's
  //   beaches by being inside this mask). The blossom reaches CHOFF + CHHALF + CHB = 2120 west of spawn; the
  //   two borders meander independently by up to 235.7 voxels against each other (chWob carries 0.6 of oakWob,
  //   so the free part is 0.4 of oakWob's 439.2 swing plus one CHW octave); and this mask needs a further
  //   OAKB/2 = 225 to reach a hard 1. 2120 + 235.7 + 225 = 2580.7, so the west edge may sit no closer in than
  //   -2580.7. It sits at -4180: 1599.3 voxels of margin, against the 64 the tight version had.
  //   THE EAST SIDE OF THE SAME TEST, which the old note never wrote down: the blossom reaches
  //   CHHALF + CHB - CHOFF = 240 east of spawn, so 240 + 235.7 + 225 = 700.7 against OAKOFF's 2300 — the same
  //   1599.3. It is the same number on both sides BY CONSTRUCTION, because the two bands are now concentric.
  // ── ONE OAK STRIP, NOT TWO (user 2026-08-20: "remove number 3. only have one biome in each band endlessly,
  // not 2") ── the oak BAND was three strips wide, oak-strip + cherry + oak-strip, because cherryM is a
  // sub-region of oakM and the two were made CONCENTRIC so the blossom sat in the middle of the wood. Walking
  // out of spawn that reads as oak, cherry, oak, pine, desert: the same biome twice before the next one.
  // The band is two strips now, and the blossom is pushed to its FAR edge rather than its centre, so the
  // crossing is oak, cherry, pine, desert with nothing repeated.
  // ONE NUMBER DOES IT, and it is this one. OAKC and OAKH are derived — OAKC = (OAKWOFF+OAKOFF)/2 and
  // OAKH = (OAKOFF-OAKWOFF)/2 — so moving this edge alone gives centre +1540 and half-width 2160 (= W), and
  // the blossom's own centre (SPWX - CHOFF, i.e. +2620 mirrored, half-width CHHALF+CHB = 1080) lands on
  // 1540..3700, which IS the band's outer strip exactly. The oak strip is 1540 wide either side of spawn and
  // spawn keeps its 620 to the pine line, so the spawn work is untouched.
  // (Those three numbers are pre-2026-08-21: the 340 slide above makes them centre +1880, blossom 1880..4040, and
  // spawn 280 from the pine line. The RELATION each states — outer strip, equal halves — is what the slide preserves,
  // and it preserves it exactly, because all four offsets moved together.)
  const OAKWOFF = -5400;                               // the WEST boundary, as a signed distance from spawn: the blossom's west midpoint less one strip W, carried 1360 east with everything else by the pine slide (was -4040). OAKC/OAKH are derived from this and OAKOFF, and OAKFAR/OAKNEAR/OAKWFAR/OAKWNEAR from those, so the whole oak geometry follows these two numbers and nothing else has to move
  // ── THE BANDS ARE MIRRORED ABOUT SPAWN (user 2026-08-20: "can you flip the map? I want to be running into
  // the sun while running in the direction of the cherry forest. I want the sun to stay east though") ──
  // the SUN is untouched: tick-camera derives it from tday alone, and at dawn (tday 0.25) its vector is
  // [1, 0, 0], i.e. it rises in +x. So "run into the sun toward the blossom" means the blossom has to sit at
  // +x, and it sat at -x. Flipping the WORLD rather than the sky is what keeps sunrise where it was.
  // Done on the three band CENTRES, not by mirroring x inside each mask, because the masks have copies the
  // masks themselves cannot reach: render/wgsl/trace.js bakes `SPWX + OAKC` and `SPWX - CHOFF` into the shader
  // at bundle time, and world/gen-worker.js is handed DESC/DESOFF as plain consts. Negating the centre makes
  // every one of those copies flip together, which is the same "all three copies of H" rule the desert biome
  // is written under. Half-widths (OAKH, DESH, CHHALF) are distances and stay exactly as they were, so no
  // strip changes size and spawn keeps its position inside the oak — only which way round the world runs.
  const BAND_MIRROR = -1;                              // +1 restores the original arrangement (blossom west, pines east)
  const OAKC = BAND_MIRROR * ((OAKWOFF + OAKOFF) / 2), OAKH = (OAKOFF - OAKWOFF) / 2;   // -940 and 3240: centre + half-width. OAKC ± OAKH reproduce OAKWOFF and OAKOFF exactly, and OAKC is now EXACTLY the blossom's own centre (SPWX - CHOFF) — that equality IS "the two oak strips are the same width"
  const oakM = (x, z) => {                             // 1 = deep oak forest, 0 = pine forest either side of it — a BAND now, not a half-plane, so the forest ends in both directions and the cycle can close
    const c = SPWX + OAKC + oakWob(z) - oakWob(SPWZ);   // pinned at the spawn's own z, for the reason desertM pins its own: otherwise how far the player starts from the border is a per-session lottery
    const t = 0.5 + (OAKH - Math.abs(pwrap(x - c))) / OAKB;   // a DISTANCE from the centre line, the same shape cherryM has always had
    return t >= 1 ? 1 : t <= 0 ? 0 : sstep(t);
  };
  // ══ THE CHERRY FOREST (user 2026-08-18) ══ a fourth band, WEST of the oak forest, and deliberately carved out
  // of oak's own region rather than given a height field of its own.
  //
  // WHY WEST, AND WHY THAT IS THE WHOLE DESIGN. oakRoll short-circuits: `if (dx <= OAKNEAR) return oakH(x, z)`,
  // so every column more than 244 voxels west of spawn is UNCONDITIONALLY oak terrain — the mask is never even
  // evaluated out there. A cherry band placed in that region therefore inherits oakH's broad valley floors,
  // oakBank's shallow banks and beaches, and with them the thing the user asked for by name: WATER. OAKY sits 4
  // under WL precisely so those flat floors can flood, and that is a measured number (see the note above it).
  // The alternative — a fourth base field — would have meant a fourth term in all THREE copies of H, which is
  // the one edit in this file that cannot be made safely by inspection. This way H is not touched at all, and
  // htest/gtest stay clean by construction rather than by testing.
  //
  // SO cherryM IS A SUB-REGION OF oakM, NOT A DISJOINT BAND. Every column with cherryM > 0 also has oakM === 1.
  // That is deliberate and it is load-bearing in both directions: the cherry forest inherits every oak REFUSAL
  // for free (no ferns, no pinecones, no mushrooms, no pines — all already gated on oakM > 0.5), and in exchange
  // every oak ADMISSION has to be re-gated to keep cherry out (the oaks themselves, the fruit, the oak sticks).
  // Read CHOFF as "how far west of the oak border the blossom starts" — it is NOT measured from spawn like the
  // other two, because it hangs off a border that already moves with the spawn.
  // ── AND IT IS A FINITE BAND, BECAUSE THE PLAYER SPAWNS IN IT (user 2026-08-18) ── the first cut made cherry
  // west-infinite like oak and desert, and that cannot work once spawn has to be inside it: oakM is 1 for every
  // column west of its line, so a second west-infinite mask either sits entirely inside oak (and spawn, which is
  // 2300 WEST of oak's east line, is outside it) or swallows oak entirely. A band solves both at once — oak is what
  // lies on BOTH sides of it, which is exactly "neighbour the oak forest", and spawn sits inside by construction
  // the way it sits inside oak by construction.
  //
  // THE THREE NUMBERS ARE CONSTRAINED, NOT CHOSEN. Spawn must be in the pure band, so CHHALF >= CHOFF. The band's
  // east edge is CHB east of that, and it must stay clear of oak's own line at OAKOFF or the blossom runs into
  // the pine treeline — which is why CHB is 200 and not the 450 the other two borders use. Worst-case meander is
  // ±235.7 (see chWob), and with OAKOFF at 2300 the clearance is 1599.3, so that constraint is slack now; it was
  // 160 when the oak line sat at 1220, and CHB is still 200 because widening a blend changes what the border
  // LOOKS like, and nobody asked for that.
  // ── SPAWN SITS OFF CENTRE, AT THE EAST EDGE (user 2026-08-18: "make the spawn moved off center to get the full
  // bands walk") ── the band was centred ON spawn, which meant the player only ever had half of it in front of
  // them: 540 of blossom whichever way they walked, against a full 1080 of oak beyond it. That is what read as a
  // thin cherry band even though the arithmetic said they matched. The centre now sits CHOFF west of spawn, so
  // spawn lands 140 inside the EAST edge and the whole 940 of blossom is ahead — and ahead is the right word,
  // because SPYAW faces WEST into the forest (see the spawn heading above), so the full band is in the view the
  // player is given rather than behind them.
  // OAKWOFF, OAKOFF and DESOFF are set FROM this band rather than the other way round (see the strip table at the
  // top of the file): every strip in the period is W = 2160 measured between mask midpoints, and the blossom is
  // the one that already was, so it is the ruler.
  //     cherry  spawn-2020 .. spawn+140   = 2160
  //     oak     spawn+140  .. spawn+2300  = 2160
  //     pine    spawn+2300 .. spawn+4460  = 2160
  // CHOFF is 940 rather than 980 (which would put spawn exactly on the pure band's edge) so cherryM(SPWX,SPWZ)
  // is 1 with margin instead of landing on the 1.0 boundary.
  // ── AND SPAWN MOVED OUT OF THE BLOSSOM (user 2026-08-19: "have the player spawn in the oak forest on
  // refresh") ── these three offsets are all measured from SPWX, which is what makes the arrangement follow the
  // spawn point around the world; so where the player STANDS in the pattern is set here and nowhere else, and
  // in particular it cannot be fixed by nudging SPWX in world/build.js (a first attempt did exactly that and
  // landed in cherry on 5 boots out of 5 — the band simply came along).
  // The blossom slides one HALF-STRIP west, 940 -> 2020, and oak's two lines move the same 1080 the other way
  // so every strip keeps its 2160: the blossom's east midpoint goes from spawn+140 to spawn-940, and spawn
  // therefore sits 940 east of the pink and 1220 west of the pines, i.e. inside the oak strip with room on
  // both sides. BIOP is unchanged, the six strips are unchanged, and only the PHASE of the pattern moved.
  const CHOFF = BAND_MIRROR * 4320, CHHALF = 980, CHB = 200, CHW = 260;   // ── UNCHANGED BY THE EQUAL-STRIP PASS (user 2026-08-19: "make all of the biomes the exact same size... double the size of the bands") ── this band was doubled on 2026-08-18 (1080 -> 2160 measured between mask midpoints, 2 * (CHHALF + CHB/2)) and 2160 is precisely the width every OTHER strip has now grown to, so nothing here moves: spawn still sits 140 inside the EAST edge with the whole 2020 of blossom ahead in the facing direction, bit for bit   // band centre, west of spawn; half-width of PURE blossom; blend width; its own meander   // CHOFF is MIRRORED (see BAND_MIRROR): the blossom's centre is SPWX - CHOFF, so a negative CHOFF puts it EAST of spawn, in the sunrise. Magnitude unchanged.
  const chWob = (z) => oakWob(z) * 0.6 + (vnoise(z * WOB_CH + 211.3, 97.7) - 0.5) * CHW;   // carries 0.6 of the OAK meander for the reason oakWob carries 0.6 of the desert's: two free meanders converge and let bands touch. Its own half is small because this band has the least room of the three
  const cherryM = (x, z) => {                          // 1 = inside the blossom band, 0 = the oak forest either side of it
    const b = SPWX - CHOFF + chWob(z) - chWob(SPWZ);   // pinned at the spawn's own z, so the wobble cancels there and spawn's position in the band is not a per-session lottery
    const t = (CHHALF + CHB - Math.abs(pwrap(x - b))) / CHB;  // …and it is a DISTANCE from the centre line, not a side of it — that one change is what makes it a band. pwrap is what makes it RECUR: without it the band exists once and the world either side of it does not repeat
    return t >= 1 ? 1 : t <= 0 ? 0 : sstep(t);
  };
  // ── AND desWob IS IN THIS SUM (audit 2026-08-18) ── chWob = oakWob*0.6 + noise*CHW, and oakWob is itself
  // desWob*0.6 + noise*OAKW, so chWob carries desWob*0.36. The first version of this counted only the OAKW and
  // CHW terms and therefore UNDER-covered by ~155 voxels each side — meaning chNear could answer "not blossom"
  // for a column that cherryM would have called blossom, which is the one answer a cheap bound may never give.
  // |desWob| <= DESW * 0.675 (the two-octave sum, see desWob), so the carried term is 0.36 * that.
  // ── THE WEATHER GETS ITS OWN, MUCH WIDER BLEND (user 2026-08-19: "the snow just appears instantly when going
  // into the cherry biome. make the transition smooth") ── and it has to be a SEPARATE number, because the
  // obvious fix is the one thing this file forbids: the note above records that CHB is 200 rather than the 450
  // the other two borders use precisely so the blossom does not run into the pine treeline, so widening it to
  // soften the snow would move the forest to fix the weather.
  // Same centre, same CHHALF, ONLY the ramp is longer: the mask still reaches 1 at exactly CHHALF, so where it
  // snows is unchanged and only how fast it stops snowing moves. That is the whole of the report — the blanket
  // drew a clean white line across the ground at the band edge instead of thinning into it.
  // 600 IS THREE TIMES CHB, and the fade the player actually sees is longer than that ratio suggests: wSharp
  // clips the mask to a window, and cherryM is smoothstepped, so the 0.20..0.80 window used to cover only ~86
  // voxels of the 200-wide ramp. On a 600-wide ramp through the widened window below it is ~450 voxels, which
  // is several seconds of walking rather than a line you cross.
  // ── …BUT 600 PUT THE SNOW LINE TOO FAR OUT (user 2026-08-20: "can you bring the snow closer into the cherry
  // forest … the snow should not be so far out in its neighboring biome") ── cherryW reaches 1 at CHHALF and
  // ramps to 0 over CHBW OUTSIDE it, so this number IS the snowless buffer the blossom pushes into the pine:
  // at 600, with the widened wSharp window, the fade the player walks through measured ~450 voxels and the
  // last flake fell 45 m short of the treeline. Halved to 300 — still a ~225-voxel ramp, so it thins into the
  // band rather than drawing the white line the 2026-08-19 report was about, but the snow now reaches the
  // blossom instead of stopping a field away from it.
  // It stays a SEPARATE number from CHB for the reason above: CHB is 200 so the blossom does not run into the
  // pine treeline, and widening THAT to move the snow would move the forest.
  const CHBW = 300;
  const cherryW = (x, z) => {                        // the blossom band as the WEATHER sees it — cherryM's twin with a longer ramp; the trees keep cherryM
    const b = SPWX - CHOFF + chWob(z) - chWob(SPWZ);
    const t = (CHHALF + CHBW - Math.abs(pwrap(x - b))) / CHBW;
    return t >= 1 ? 1 : t <= 0 ? 0 : sstep(t);
  };
  const CHREACH = CHHALF + CHB + 2 * (DESW * 0.675 * 0.36 + OAKW * 0.5 * 0.6 + CHW * 0.5);
  // ── AND THE CHEAP BOUND THAT MUST BE ASKED FIRST (user 2026-08-18: the game froze on boot) ── cherryM costs
  // ~7 vnoise, and fillColumn called it for EVERY COLUMN with om > 0, which is the whole infinite oak forest.
  // A 768x768 window is 590,000 columns, so a boot paid millions of extra noise evaluations in the hottest loop
  // in worldgen. This is the same shape OAKFAR/OAKNEAR give oakRoll: one subtract and a compare that answers
  // "this column cannot be in the band" without evaluating the band. chWob's swing is already inside CHREACH,
  // so the bound is exact rather than conservative — a column it rejects has cherryM 0 by construction.
  const chNear = (x) => Math.abs(pwrap(x - (SPWX - CHOFF))) <= CHREACH;   // …on the wrapped distance, or this cheap bound would answer 'not blossom' for every band but the first and cull the cherry forest out of every later cycle
  // ── AND THE WEATHER ASKS A DIFFERENT QUESTION THAN THE WORLDGEN DOES (user 2026-08-18: "in the cherry biome
  // make it snow like the pine forest") ── cherryM is a SUB-REGION of oakM (see the long note above), which is
  // exactly what lets the blossom band inherit every oak REFUSAL for free. Weather is the one place that
  // inheritance is wrong. The oak forest is deliberately the biome with no falling weather at all — OAK_SNOW
  // false means snow is culled there and RAIN_ON false means nothing replaced it — and because cherry sits
  // inside oakM the blossom band was silently getting that same nothing while the pines were in a storm.
  // This is the oak mask with the blossom band cut back out of it. Read it as "does this column get OAK
  // weather", as against oakM's "is this column oak". It is a SEPARATE NAME rather than a change to oakM
  // because the two questions genuinely differ: the ferns, cones, mushrooms, pines and rounded hills all still
  // want the plain mask, and folding cherry into oakM would hand the blossom band pine WORLDGEN as well.
  // chNear FIRST, for the reason it is asked first everywhere else in this file: this runs once per SETTLING
  // COLUMN in the snow path and cherryM is ~7 vnoise. Outside the band's exact bound the answer is oakM
  // unchanged, for the cost of one subtract and one compare.
  // ── AND THE WEATHER BORDER IS SHARPER THAN THE BIOME BORDER (user 2026-08-18: "can you keep the snow more in
  // the relative biomes territory?") ── the masks cross-fade over their whole blend band, 450 voxels for the
  // desert and 200 for the blossom, because that is right for TERRAIN: canopies, ground cover and height all
  // want to interleave across a treeline rather than switch on a line. Weather inherited that ramp and it is
  // too generous — snow was still falling a couple of hundred voxels into country that does not get snow,
  // which is what reads as it belonging to no biome in particular.
  // This is a contrast curve, not a hard edge: the fade still exists, it just happens over the middle 30% of
  // the ramp instead of all of it, so a storm still crosses a treeline rather than stopping at a wall.
  // IT MUST BE APPLIED IDENTICALLY ON BOTH SIDES. The blanket is decided on the CPU (landSnowAt) and the
  // falling flakes on the GPU (the trace march), so a curve on one and not the other is snow settling under a
  // clear sky — see wSharpG in render/wgsl/trace.js, which is this function ported bit for bit.
  // ── AND THE FAR END COMES BACK IN (user 2026-08-20: "can you bring the snow closer into the biome at the
  // transition between biomes?") ── 0.94 -> 0.70 on the UPPER bound only. m is the mask of the biome that does
  // NOT get this weather, so the upper bound is how far past the treeline snow was still settling: at 0.94 the
  // blanket ran ~156 voxels into the desert (smoothstep, 450-voxel blend), which is what reads as belonging to
  // no biome. 0.70 is ~67, less than half. The LOWER bound is untouched on purpose — moving it would pull snow
  // off the snowy biome's own ground, which is the opposite of the request.
  // AND THE FADE IS STILL LONG. The ramp is 224 voxels of the desert blend and ~298 of the blossom's (which
  // gets the 600-wide cherryW), so this does NOT undo 2026-08-18/19 ("the snowing appears instantly when
  // transitioning biomes"): what changes is WHERE the fade sits, not how long it is. It now happens mostly on
  // the snowy side and is finished by the time you are properly into the other biome, instead of straddling
  // the border evenly.
  const wSharp = (m) => (m <= 0.06 ? 0 : m >= 0.70 ? 1 : (m - 0.06) / 0.64);   // 0.20..0.80 -> 0.06..0.94 -> 0.06..0.70 AND the second smoothstep is gone (user 2026-08-19, the same complaint a second time): cherryM is already smoothstepped, so squaring the S made the middle of the ramp far steeper than either end and that steep middle IS the line the player sees. Linear here leaves exactly one S in the chain   // 0.35..0.65 -> 0.20..0.80 (user 2026-08-18: "the snowing appears instantly when transitioning biomes") — the first cut squeezed the fade into 25 voxels, which at walking pace is under a second and reads as a switch. 0.60 of the ramp is ~75 voxels: still far tighter than the 125 the raw mask gives, so the snow stays in its own biome, but you now walk INTO it
  const oakWeather = (x, z) => { const om = oakM(x, z); return (om > 0 && chNear(x)) ? om * (1 - cherryW(x, z)) : om; };   // cherryW, NOT cherryM: the weather border feathers over CHBW while the treeline keeps CHB (see the block above)
  const PETAL_ON = !location.search.includes('nopetal');   // ?nopetal — A/B the fallen-petal scatter. A CONST, not a location read inside stampOak: that function is serialised into the gen workers, whose Blob location carries no query string, so the flag would be true in the pool and false inline and gtest would (correctly) call it a mismatch.   // CHREACH = 1503.52: no column further than this from the band centre can be cherry at all — the cheap bound a scatter gate takes before paying for 8 vnoise. It is DERIVED from CHHALF/CHB/CHW/OAKW/DESW, none of which the equal-strip pass touched, so it did not move either
  // ── ROUNDED HILLS (user 2026-08-17: "make the terrain have much more 'rounded' hills", with a photograph of
  // Tuscan downland — broad domed crests, long clean sweeps, no fine-grained bumpiness anywhere) ── OAK FOREST
  // ONLY. The pine forest and the desert come back out of oakRoll as the identical double, bit for bit.
  //
  // The forest base cannot be rounded by damping it, and that is the whole design. It is 88 * fbm at 0.008
  // plus 9 * fbm at 0.04, and fbm's own third octave runs at 0.035 — so ~18 voxels of the 88 arrive on a
  // 28-voxel wavelength, and the 9-voxel detail octave arrives on a 25-voxel one. THAT is the bumpiness in
  // the photograph's terms: relief whose wavelength is shorter than the player is tall. Scaling those terms
  // down leaves the same crinkle, quieter — the same mistake duneH's comment records from the dune pass,
  // where raising amplitude alone made the sand squigglier rather than rounder.
  // So the oak forest does not damp the forest base, it REPLACES it, exactly the way the desert flat replaces
  // it: its own field, at its own wavelength, blended over the same rim. Two octaves at 0.0024 and 0.0057 —
  // 417 and 175 voxels, i.e. hills 42 m across with a 17 m swell for shape, against the forest's 12 m — and
  // then the same double smoothstep duneH uses, for the same reason: sstep flattens a signal near 0 and 1 and
  // steepens it through the middle, so crests broaden into domes, valley floors go flat, and the shoulder
  // between them is the rounded part. Measured against the pine base over 600 random columns, on the field
  // itself before Math.round: median slope 0.41 -> 0.17, 95th percentile 0.87 -> 0.42, and mean |curvature|
  // 0.097 -> 0.008, a 12x smoother surface. On the ROUNDED heights the same change reads as terrace width:
  // the mean run of constant height along a walk goes 3.4 -> 8.7 voxels, so the ground steps half as often
  // and half as high.
  // Height DISTRIBUTION is deliberately left near where it was — mean 56.1 -> 59.8, peak 87 -> 98 against an
  // HMAX of 105 — so the two forests meet at essentially the same elevation and the rim blend has nothing to
  // ramp. Measured over the 66k columns of the blend band itself: 83% of neighbouring columns are dead level,
  // 17% step one voxel and 0.01% step two, and NOTHING steps further — against 28% one-voxel steps and an
  // 8-voxel worst case in the untouched pine forest, so the border is smoother than the biome it joins.
  // Relief is BIGGER, not higher: sd 12.5 -> 17.7, which is what "large hills" means when the mean cannot move.
  const OAKY = 20 + LIFT, OAKHILL = 78;                // the valley floor, and crest-above-floor
  // OAKY sits 4 under WL on purpose, and the number was measured rather than picked. Above WL + 7 the oak
  // forest has no still water in it at all — no lake, no shoreline, nothing for the ducks and the lilies —
  // because the double smoothstep gives it broad FLAT valley floors and they would all be dry. Far below it,
  // those same flat floors turn into the `shore` band (h <= WL + 6) and come up SAND: at 17 the oak forest
  // was 4.2% sand surface against the pine forest's 1.0%, which is a green biome with a pale patch in every
  // valley. 20 costs 2.5% and keeps the low ground a basin can still flood.
  // ── THE TWO CHEAP BOUNDS ── oakRoll runs on EVERY column in the world, and oakM is 6 vnoise (24 hashes)
  // where the rest of a column's height is about 10. These are the x-range outside which the answer is known
  // without asking, and they are derived from the constants rather than typed, so a change to the border
  // arithmetic carries them with it. |desWob| <= DESW * (0.5 + 0.35 * 0.5) and oakWob carries 0.6 of that
  // plus its own +-OAKW/2, so |oakWob(z) - oakWob(SPWZ)| <= 2 * OAKWMAX; the rim is OAKB/2 either side of
  // the line. East of OAKFAR the mask is exactly 0, west of OAKNEAR it is exactly 1 — so the pine forest and
  // the desert pay one subtraction for this feature and the deep oak forest skips the mask too.
  const OAKWMAX = DESW * 0.675 * 0.6 + OAKW * 0.5;     // 675.0 — the ceiling on |oakWob|
  // ── THESE FOUR MUST BE READ OFF THE MIRRORED CENTRE, NOT OFF THE RAW OFFSETS (user 2026-08-20, terrain
  // seam after the flip) ── they were `OAKOFF ± …` and `OAKWOFF ± …`, i.e. the band's edges as WRITTEN, which
  // stopped agreeing with oakM the moment BAND_MIRROR negated OAKC. The mask said oak on one side of the world
  // and these short-circuits said pine, so oakRoll and the beach handed whole regions the wrong terrain and
  // the two treatments met in a hard plane — the grey slabs cutting across the hills.
  // `OAKC ± OAKH` IS `OAKWOFF .. OAKOFF` when BAND_MIRROR is +1 (OAKC = (OAKWOFF+OAKOFF)/2 and
  // OAKH = (OAKOFF-OAKWOFF)/2 by construction), so this is bit-identical unmirrored and correct mirrored.
  // The comment above still describes the shape: conservative in both directions, because a wrong answer here
  // is the wrong TERRAIN and not merely a slower one.
  const OAKFAR = OAKC + OAKH + 2 * OAKWMAX + OAKB * 0.5;    // 2964.2 east of spawn: no column past this can be in the oak forest at all
  const OAKNEAR = OAKC + OAKH - 2 * OAKWMAX - OAKB * 0.5;   // 1635.8: no column east of this is deep oak, mask exactly 1 below it
  // ── AND THE SAME PAIR ON THE WEST EDGE ── these are pure short-circuits, not the mask: they must be
  // CONSERVATIVE in both directions or a column gets the wrong terrain rather than merely a slower answer.
  // Same construction as the two above, mirrored about the band's other boundary.
  const OAKWFAR = OAKC - OAKH - 2 * OAKWMAX - OAKB * 0.5;  // no column west of this can be in the oak forest at all
  const OAKWNEAR = OAKC - OAKH + 2 * OAKWMAX + OAKB * 0.5; // every column east of this (and west of OAKNEAR) is DEEP oak
  const oakH = (x, z) => {                             // the oak forest's OWN base height — long wavelength, double-smoothstepped, positive-only like duneH
    const a = fbm(x * 0.0024 + 91.7, z * 0.0024 + 33.1);
    const b = fbm(x * 0.0057 + 47.3, z * 0.0057 + 8.9);
    return OAKY + OAKHILL * sstep(sstep(a * 0.82 + b * 0.18));   // OAKY .. OAKY + OAKHILL, never negative
  };
  // ── ONE HELPER, CALLED FROM ALL THREE COPIES OF H ── H(), makeHRow and makeHCol each carry the same height
  // expression and have to agree BIT FOR BIT or the bulk fill and the placement queries disagree about where
  // the ground is (__vb.gtest is what measures it). So this is a scalar function that takes the height the
  // forest expression just produced and hands back the height the biome wants, and each of the three wraps
  // its own existing expression in it verbatim. The three cannot drift, because there is only one expression.
  const oakRoll = (h, x, z) => {
    const dx = pwrap(x - SPWX);                        // WRAPPED, like the mask it is short-circuiting for: on a raw distance these two tests answer for the first period only and every later oak forest would come out as pine
    if (dx >= OAKFAR || dx <= OAKWFAR) return h;       // pine forest and desert — the identical double back out, so their terrain is unchanged to the last bit
    if (dx <= OAKNEAR && dx >= OAKWNEAR) return oakH(x, z);   // deep oak — mask is exactly 1, and h * 0 + oakH * 1 is oakH
    const om = oakM(x, z);
    return om <= 0 ? h : h * (1 - om) + oakH(x, z) * om;   // the rim: the same lerp shape the desert flat uses, over the same 450 voxels, so the two forests meet on a slope rather than on a step
  };
  // ── SHALLOW BANKS (user 2026-08-17: "make the rivers in the oak forest flatter against the terrain instead
  // of how the terrain just drops off steeply to the water") ── OAK FOREST ONLY, and a direct consequence of the
  // rounded hills above.
  //
  // THE DIAGNOSIS. The river carve in H is a LERP between the land and a bed, weighted by the channel strength
  // rs: `h * (1 - rs) + (WL - 2 - 26 * rs) * rs`. The vertical distance it has to travel is (h - bed); the
  // horizontal distance it has to travel it in is the channel's own influence half-width w, ~50-160 voxels. So
  // the bank slope is about (h - WL) / (0.6 * w) and it scales with how high the surrounding land is. In the
  // pine forest, land near water sits ~35 voxels over WL and that reads fine. oakH REPLACED the oak base with a
  // field running OAKY .. OAKY + OAKHILL, so oak land near water sits ~42 over WL and reaches +74 - the same
  // lerp, over the same few tens of voxels, is then a cliff. Measured on a hand transcription of H over two
  // river patches deep in the oak forest (68k land samples within 120 voxels of water, |grad h| on a 10-voxel
  // baseline): the ground climbs from +11 above WL at 0-20 voxels out to its full +42 by 40-60 voxels and then
  // plateaus, p90 slope 1.30, max 3.49, 15.4% of the bank steeper than 45 degrees - against an AMBIENT oak
  // slope of median 0.21 / p90 0.45. Banks 2.9x steeper than the biome they sit in, which is exactly the
  // "drops off steeply" the user is looking at. The same measurement says basinM lakes are ALREADY fine (p90
  // 0.74, 1.6x ambient), so the basin pass is left alone; the big reservoirs riverAt builds are part of
  // rivEval, so they get the skirt along with the channels.
  //
  // THE FIX is a CEILING, not another blend: the oak forest may not stand higher than a cone that rises out of
  // the water's edge. Math.min is what makes that safe - it can only ever LOWER ground, so it cannot fill a
  // lake, cannot raise a bed, and does nothing where the land is already low. 93.4% of oak columns are
  // untouched; the ones that move drop a median of 12 voxels.
  // OAKBANKY is the shelf the cone starts from, at the edge of the channel's influence, and the carve does its
  // own lerp from there down to the bed. 22 is what keeps that last stretch gentle without widening the water
  // much: the waterline is wherever the lerp crosses WL, so a lower shelf is a wider river - at 22 the water
  // area grows 8.0%, at 18 it is 14%, and at 34 the bank is visibly steeper again for 4.7%.
  // OAKBRISE + OAKBANKY - WL = 86 is deliberately above HMAX - WL = 81: the cone tops out over the highest
  // ground the generator can make, so at the rim of the skirt the min ALWAYS releases and the skirt joins the
  // hillside with no crease. OAKBANKR is then the only shape knob, and the pair is chosen so the ramp's
  // steepest point, OAKBRISE * 1.5 / OAKBANKR = 0.34, stays under the oak forest's own p90 slope - the bank is
  // never steeper than the hills it interrupts, which is what "flat against the terrain" has to mean here.
  const OAKBANKR = 280, OAKBANKY = WL + 22, OAKBRISE = 64;   // skirt reach in voxels; the old shelf height; the cone's rise
  // ── AND A FLAT BEACH INSIDE THE SKIRT (user 2026-08-17: "the sand that forms the outline around the water,
  // it needs to be flatter") ── the skirt above already made the BANK shallow, but it started its cone at
  // WL + 22, which is 16 voxels above the top of the sand band. fillColumn paints sand on `h <= WL + 6`, so
  // the sand was only ever the thin strip where the river carve's own lerp happened to dip under that line:
  // a narrow, tilted rind around the water rather than a shore. Holding the cap at OAKBEACHY for the first
  // OAKBEACH voxels puts the whole ring inside the sand band and dead level, and the cone then starts from
  // the beach instead of from the waterline.
  // OAKBEACHY = WL + 4 sits in the MIDDLE of the sand band, not at its top: fillColumn dithers sand against
  // (h - WL - 2) / 4.5, so at +4 about 56% of the ring comes up sand and the rest oak green - a beach that
  // grades into the wood rather than a painted band with an edge. At WL + 6 it would be ~11% sand, at WL + 1
  // it would be solid sand with a hard rim.
  // It can still only ever LOWER ground (the `c >= h` test below), so it cannot fill a lake, raise a bed, or
  // put a beach anywhere the land was already lower than the shelf.
  // ── SHORTER SHORE, SAME FLATNESS (user 2026-08-17: "theres too much sand next to water. make the shore
  // of the water shorter. still keep the flatness of the sand") ── 60 -> 24. The two properties are set by
  // two DIFFERENT numbers, which is what makes this a one-constant change: OAKBEACHY is the shelf HEIGHT
  // and is what makes the shore flat and sandy, OAKBEACH is only how far the shelf runs before the cone
  // starts. Measured at 60 the sand ring came out 52 voxels wide against the pine forest's 18 - a beach
  // you could lose a lake in. 24 lands it near the pine's width while keeping the median slope at ~0.06
  // against pine's 0.182, which is the whole point: as much sand as a shore should have, as flat as a
  // beach should be.
  // The cone is unaffected, and if anything gentler: it now runs from 24 to OAKBANKR instead of from 60,
  // so the same 82-voxel rise is spread over 256 voxels rather than 220.
  const OAKBEACH = 24, OAKBEACHY = WL + 4;             // flat shore width in voxels (2.4 m), and its height
  const oakBank = (h, x, z) => {                       // ONE shared scalar helper again, for the reason oakRoll is one: three copies of H and no room for drift
    const dx = pwrap(x - SPWX);                        // wrapped, for the reason oakRoll's is
    if (dx >= OAKFAR || dx <= OAKWFAR || h <= OAKBEACHY) return h;      // pine forest, desert, and any oak ground already at or under the BEACH - one subtraction and a compare, before the river scan
    const d = bankDist(x, z);
    if (d >= OAKBANKR) return h;                       // no water within the skirt
    // ONE continuous profile from the beach to the hilltop: flat for OAKBEACH, then the same sstep cone to
    // the same top (OAKBANKY + OAKBRISE, which is deliberately above HMAX - WL so the cap always releases
    // into the hillside with no crease). Two separate pieces would step 18 voxels where they met, which is
    // exactly the cliff this whole helper exists to remove.
    const c = OAKBEACHY + (OAKBANKY + OAKBRISE - OAKBEACHY)
              * sstep(Math.max(0, d - OAKBEACH) / (OAKBANKR - OAKBEACH));
    if (c >= h) return h;
    if (dx > OAKNEAR || dx < OAKWNEAR) { const om = oakM(x, z); return om <= 0 ? h : h * (1 - om) + c * om; }   // the rim: faded in on the same mask oakRoll uses, so a river crossing the biome border changes width gradually instead of stepping
    return c;
  };
  // 1080 gives the sand 2160 voxels — one strip W, the same width every other strip in the period has (see the
  // table at the top of the file) — and DESC = DESOFF + DESH puts DESC - DESH exactly on DESOFF, so the
  // pine/desert line is wherever DESOFF says it is and the two constants can never disagree about it.
  // NOT "run the sand up to where the next period's oak begins" — that was the first version of this line and
  // it measured 2155 wide, because the gap from DESOFF to BIOP + OAKWOFF is desert AND the pine strip that has
  // to sit between the sand and the next oak forest. It ate that strip whole: sampled at 200-voxel steps the
  // band sequence came back desert(2200) → pine(200) → oak, i.e. sand running straight into the trees, which
  // is the exact adjacency the meander notes under OAKOFF exist to prevent. That trap is the reason DESH is
  // derived from the strip width (DESH = W/2) and never from BIOP: the leftover between the sand's east
  // midpoint and the next period's oak is not slack to be absorbed, it is the SECOND PINE STRIP.
  const DESH = 1080, DESC = BAND_MIRROR * (DESOFF + DESH);             // half-width to the mask MIDPOINT, and the band centre: DESC ± DESH = 4460 and 6620
  const desertM = (x, z) => {                          // 0 = pine forest, 1 = open desert — a BAND now (see BIOP), with pine on BOTH sides of the sand
    // The wobble is subtracted AT THE SPAWN'S OWN z. Without that it swings +-216 voxels either way, so how
    // far the player starts from the sand was a lottery: measured 380 voxels one session where the constant
    // of the day (80) implied 305, and a big enough swing would have spawned them PAST the boundary, in the
    // desert. That specific hazard is gone now DESOFF is 1500, but the reason to pin it is not - oakM pins
    // its own wobble the same way, and there the offset is OAKOFF (2300) against the same +-216.
    // Pinning it costs nothing — the border still meanders exactly as before, it just passes DESOFF from
    // spawn at the spawn's own latitude, so "how far to the desert" is the same every session.
    const c = SPWX + DESC + desWob(z) - desWob(SPWZ);
    const t = 0.5 + (DESH - Math.abs(pwrap(x - c))) / DESB;   // a band, not a half-plane: one band centred at DESC covers the sand at the east end of a period AND the sand at the west end of the next, because the distance wraps
    return t >= 1 ? 1 : t <= 0 ? 0 : sstep(t);
  };
  const H = (x, z) => {
    let h = baseH(x, z);
    const bm = basinM(x, z);
    const m = bm * Math.max(0, Math.min(1, (66 + LIFT - h) / 20));   // basins only form in low country
    if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(x * 13 + 7, z * 17 + 3) - 0.5) * 0.8);   // gently dithered — no terrace banding
    const rs = riverS(x, z);
    const bn = fbm(x * 0.05 + 13.7, z * 0.05 + 4.2);   // bed/beach relief — lakebeds and sand flats are no longer billiard-flat
    h = Math.round(oakBank(h, x, z));                  // ── SHALLOW OAK BANKS ── BEFORE the carve, so the lerp below starts from the shelf instead of from a hilltop. h is already an integer here, so Math.round is the identity outside the oak forest and the pine/desert heights stay bit-exact; see oakBank
    if (rs > 0.02) h = Math.min(h, Math.round(h * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(x * 19 + 5, z * 23 + 9) - 0.5) * 0.8));   // noisy bed + gently dithered banks
    if (h <= WL && h >= WL - 5 && bm <= 0.25 && rs <= 0.04) h = WL + 1 + Math.max(0, Math.round((bn - 0.55) * 5));   // beach flats get 0-2 voxel dune relief
    // ── THE DESERT FLAT DOES NOT FILL IN LAKES (user 2026-08-16, screenshot: a forest lake bordering the
    // desert was sliced off along a dead-straight diagonal) ── the WL+2 lift below exists so the desert never
    // sits below sea level, and it was unconditional: every column past dm 0.5 was shoved above the water,
    // INCLUDING the bed of a lake straddling the line. So the water ended exactly on the dm=0.5 iso-line,
    // which at lake scale is a straight edge, and the shore dither on the far side left a dark fringe along
    // the cut. bm/rs are the same two predicates the beach-flat line already uses to mean "this column
    // belongs to a water body". A biome decides what the shore is MADE OF, never where the water ENDS.
    const dm = desertM(x, z); if (dm > 0) { h = Math.round(h * (1 - dm) + (DESY + duneH(x, z) + (fbm(x * 0.012 + 5.1, z * 0.012 + 9.3) - 0.5) * DESREL) * dm); if (dm > 0.5 && bm <= 0.25 && rs <= 0.04) h = Math.max(h, WL + 2); }   // ── DESERT FLAT ── LAST on purpose: it runs after the basin and river passes so the sand overrides a lake bed or a channel instead of being carved by one. Relief is DESREL voxels peak-to-peak (see the scale above) against the forest's +-44.
    return h;
  };
  const RIVCELL = 768, RIVINF = 6200;                  // WATERSHEDS — one candidate per ~77 m cell, rare roll; each hit is a whole dendritic system (influence radius must cover the longest possible chain)
  const rivCache = new Map();
  function riverAt(cx, cz) {                           // builds a WATERSHED: 1-3 tributaries join a main stem at confluences, the stem widens downstream
    const key = cx * 100003 + cz;                      // into a BIG reservoir lake, and ~half the reservoirs spill an OUTLET river that ends in a smaller tail lake. HALF of all watersheds are instead a LONE LAKE with no channels at all (2026-08-19) — see `lone` below.
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
      // ── HALF OF THEM ARE JUST A LAKE (user 2026-08-19: "have it where theres a 50% chance that a lake spawns
      // without a river attached to it") ── a lone reservoir with no stem, no tributaries and no outlet. The
      // RESERVOIR is kept and everything that feeds or drains it is dropped, rather than the other way round,
      // because the lake is the thing the request is about and it is also what the rest of the game hangs off:
      // ducks, lilies, fish and the shoreline spawns all read lakes, not channels.
      // Its position still comes off the stem's own direction and length, so a world seed places the water in
      // the same place whether or not the river is there — only the channels differ.
      // The bbox below is taken over segs AND lakes, so an empty segs list still yields a correct box.
      const lone = ihash(cx * 97 + 41, cz * 103 + 17) < 0.5;
      const segs = lone ? [] : [{ sx: hx, sz: hz, dxr, dzr, len: Lm, wb: wbM, seed, t0: 0.6, t1: 1.15 }];   // the stem WIDENS downstream like a real river
      const lakes = [{ x: mx, z: mz, r: 200 + ihash(cx * 31 + 9, cz * 37 + 5) * 100, seed }];   // the reservoir it feeds
      const nT = lone ? 0 : 1 + ((ihash(cx * 7 + 44, cz * 3 + 18) * 2.99) | 0);   // a lone lake has no tributaries, and therefore no headwater ponds either
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
      if (!lone && ihash(cx * 61 + 13, cz * 59 + 27) < 0.55) {  // OUTLET — the reservoir FEEDS a downstream river that ends in a smaller tail lake
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
  // ── THE SAME GEOMETRY, READ AS A DISTANCE ── rivEval's strength is zero the moment a column is further from
  // the centreline than the channel's own half-width w, and the whole point of the oak forest's shore skirt is
  // to reach FURTHER than that, so it cannot be derived from rs however it is reshaped. This is rivEval's
  // segment/lake loop line for line - same meander, same downstream taper, same rounded end caps, same wobbled
  // lake rim - returning `distance beyond the water's edge` in voxels instead of a 0..1 strength. Negative
  // inside a water body. rivEval itself is untouched, so rs stays bit-identical everywhere; this is a second,
  // wider read of the geometry rather than a change to the first one.
  const bankEval = (R, x, z, best) => {
    if (best <= 0) return best;                        // already inside a channel or a lake: the cone is at its floor and no other watershed can lower it
    if (x < R.x0 - OAKBANKR || x > R.x1 + OAKBANKR || z < R.z0 - OAKBANKR || z > R.z1 + OAKBANKR) return best;   // rivEval's bbox reject, grown by the skirt
    for (const sg of R.segs) {
      const tRaw = (x - sg.sx) * sg.dxr + (z - sg.sz) * sg.dzr;
      const t = Math.max(0, Math.min(sg.len, tRaw));
      const off = Math.sin(t * 0.015 + sg.seed) * 30 + Math.sin(t * 0.04 + sg.seed * 1.7) * 10;
      const pd = (x - (sg.sx + sg.dxr * t)) * (-sg.dzr) + (z - (sg.sz + sg.dzr * t)) * sg.dxr;
      const w = sg.wb * 1.4 * (sg.t0 + (sg.t1 - sg.t0) * (t / sg.len));
      const over = tRaw < 0 ? -tRaw : (tRaw > sg.len ? tRaw - sg.len : 0);
      const d = Math.hypot(Math.abs(pd - off), over) - w;
      if (d < best) best = d;
    }
    for (const L of R.lakes) {
      const dl = Math.hypot(x - L.x, z - L.z);
      if (dl < L.r * 1.15 + OAKBANKR) {
        const al = Math.atan2(z - L.z, x - L.x);
        const wr = L.r * (1 + 0.10 * Math.sin(al * 3 + L.seed) + 0.05 * Math.sin(al * 7 + L.seed * 2.3));
        if (dl - wr < best) best = dl - wr;
      }
    }
    return best;
  };
  function bankDist(x, z) {                            // …taken over every watershed in range: riverS's own walk, with the same rivScope fast path
    let best = OAKBANKR;                               // OAKBANKR means "no water within the skirt", which is all oakBank needs to bail out
    if (rivScope && x >= rivScope.x0 && x < rivScope.x1 && z >= rivScope.z0 && z < rivScope.z1) {
      for (const R of rivScope.list) best = bankEval(R, x, z, best);
      return best;
    }
    for (let jz = Math.floor((z - RIVINF) / RIVCELL); jz <= Math.floor((z + RIVINF) / RIVCELL); jz++)
      for (let jx = Math.floor((x - RIVINF) / RIVCELL); jx <= Math.floor((x + RIVINF) / RIVCELL); jx++) {
        const R = riverAt(jx, jz); if (!R) continue;
        best = bankEval(R, x, z, best);
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
  const groundMin = (x, z, r) => Math.min(H(x, z), H(x - r, z), H(x + r, z), H(x, z - r), H(x, z + r),
    H(x - r, z - r), H(x + r, z - r), H(x - r, z + r), H(x + r, z + r));   // lowest ground under a footprint — nothing floats on slopes.
  // ── THE DIAGONALS ARE NOT OPTIONAL (user 2026-08-23: "oak trees are levitating") ── this sampled a PLUS of
  // five columns, so a dip on a diagonal was invisible and whatever seated here was left standing over it.
  // MEASURED over 1400 columns at r=4 against the true minimum of the whole 9x9: the five-point cross reads
  // 0.22 voxels high on average and up to a full voxel, and the four corners bring the mean error to 0. That
  // is exactly the trunk-base histogram it produced — 78 boles sitting flat on the ground, 5 hanging one
  // voxel over it and 1 hanging two. Nine H() calls instead of five, once per stamped object.


