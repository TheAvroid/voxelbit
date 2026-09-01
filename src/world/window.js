  // ── ENDLESS WORLD ── voxel = 10 cm. A 768×160×768 window (94 MB u8) slides with the player over an
  // infinite deterministic world. Storage is TOROIDAL: shifting the window regenerates + uploads only the
  // 8-voxel strip that wrapped — never a full-buffer move. All generation is a pure function of WORLD
  // coordinates, so strips re-materialise seamlessly and revisited terrain is always identical.
  const WX = WXZ, WY = WYpick, WZ = WXZ;              // deep world: +128 voxels of stone below the surface for TRUE gorge depth
  // ── LIFT PAYS FOR THE MOUNTAINS (user 2026-08-31: "double the height elevation of the pine forest") ──
  // 128 was the float over bedrock the CAVE systems needed, and all three of those were deleted 2026-08-05,
  // so most of it has been dead space under the water ever since. The world's vertical budget is WY and
  // nothing else: what sits below the waterline cannot be seen, and what is spent there is not available
  // above it. Dropping the float to 48 moves the whole world down 80 voxels - WL goes with it, because WL
  // is defined as 24 + LIFT - and hands those 80 to the sky, where the hills are.
  // 48 and not 0: a lake bed still has to get 52 under the waterline (PINE_BASE) without hitting bedrock.
  const LIFT = WY >= 384 ? 48 : 0;                     // terrain floats this far above bedrock
  const BX = WX >> 3, BY = WY >> 3, BZ = WZ >> 3;     // 8³ brick occupancy for empty-space skipping
  const HALF = WX >> 1;
  // ══ TWO WINDOWS ══ the CPU's W is what physics, nav, support, chopping and every edit read, and it is
  // DENSE: at 2048 it is already 1.5 GB of RAM, so it cannot grow — 4096 would be 6.4 GB and nothing binds
  // that. The GPU's world is the PAGED BRICK POOL in render/buffers.js, which costs ~13% of dense for the
  // same volume, so IT can. GMUL is how much wider the GPU window is; the CPU window sits concentrically
  // inside it, so the near field is W-backed exactly as before and the ring beyond it is render-only — no
  // collision, no life, no editing out there, and none of those want it.
  // View distance is bounded by the GPU window (GHALF) rather than by HALF, which is the whole point.
  // GMUL RIDES THE ADAPTER, IT IS NOT A CONSTANT. The pool is what makes a wider window affordable, but
  // affordable is not free: the far ring is ~3x the near window's area, so GMUL 2 asks for roughly 700 MB of
  // GPU storage against 190 MB at GMUL 1. This game ships to players' own machines (see the v1 plan), so a
  // machine that cannot bind that gets GMUL 1 and the view it can afford, exactly the way WXZ already walks
  // the 2048/1536/1280/1024/768 ladder. GPOOL_CAP is the budget the ladder is measured against.
  const GMUL = GMUL_PICK;
  const GWX = WX * GMUL, GWY = WY, GWZ = WZ * GMUL;
  const GBX = GWX >> 3, GBY = GWY >> 3, GBZ = GWZ >> 3;
  const GHALF = GWX >> 1;
  const GPAD = (GWX - WX) >> 1;                        // voxels of far ring on each side; 32-aligned because WX is
  const gwOX = () => winOX - GPAD;                     // the GPU window origin, derived so the two can never drift apart
  const gwOZ = () => winOZ - GPAD;
  let poolTouchHook = null;                            // set by render/buffers.js once the pool exists; terrain.js and gen-pool.js run before it and must call through this
  let RD_DBG = 0;                                      // dev: __vb.setRD() view-distance override, 0 = off. Lives HERE, not in the video-editor module: a module exports a const SNAPSHOT, so a write inside one is invisible to every other fragment (lint check catches it).
  const RD_FIXED = Math.min(1000 * GMUL, GHALF - 24);   // ── VIEW DISTANCE ── 100 m per GMUL step, no slider (user). Clamped to the GPU window: an adapter that caps the window small falls back to what fits rather than reaching past it.
  // ── THE WORLD GRID IS SHARED WITH THE GENERATION POOL WHEN THE PAGE IS CROSS-ORIGIN ISOLATED ──
  // world/gen-pool.js already runs the ENTIRE generator on workers; what it could not do is put the result
  // where it belongs. A worker's slab had to be TRANSFERRED back and the main thread memcpy'd it into this
  // array, and that memcpy is the single biggest main-thread cost while the player moves: MEASURED over 20 s
  // of sprinting, 1,900 ms in blitSlab against 6 ms actually spent waiting on the pool. The pool was never
  // the bottleneck — the hand-off was.
  // A SharedArrayBuffer lets the workers write here directly, so that copy happens on their core instead of
  // this one. It needs cross-origin isolation (game/.htaccess + tools/serve-nocache.py send the two headers),
  // and it is a graceful upgrade, not a requirement: without isolation SharedArrayBuffer is undefined, this
  // falls back to a plain Uint8Array, and gen-pool keeps the transfer path. Both produce a bit-identical
  // world — deepHash is the acceptance test, not frame time.
  // 1,536 MB allocates in ~1 ms here; the size is the same either way, so this costs no memory.
  const W_SHARED = typeof SharedArrayBuffer !== 'undefined' && self.crossOriginIsolated === true
    && !location.search.includes('nosab');   // ?nosab forces the transfer path, so the NON-isolated fallback stays testable on an isolated host
  const W = new Uint8Array(W_SHARED ? new SharedArrayBuffer(WX * WY * WZ) : WX * WY * WZ);   // CPU copy — collision + build (toroidal)
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
  const LGT2_ALL = 0x3f;                                // ── SECOND TERM MASK (u.lgt.z) ── lgt.x is full at 24 bits (an f32 is exact only to 2^24, so a 25th bit there would round), so this is where a 25th term goes. Three groups have lived and died here on 2026-08-09: the water soft glisten (bit 0), the tier-1 LOOK set (bits 1-6) and the tier-2 set (bits 0-3). BIT 0 IS NOW THE SUN SHEEN ON STONE (user 2026-08-16) — __vb.lgt2(0) turns it off and __vb.lgt2(1) back on, which is the A/B this effect is judged with; BIT 1 IS THE GRASS/SUBPIXEL CROSS-FACE SPATIAL FALLBACK (see SPATIAL in render/wgsl/denoise.js) — __vb.lgt2(1) off / __vb.lgt2(3) on, and it is a LIVE toggle rather than a URL flag on purpose: the world seed differs between loads, so a cross-load A/B compares two different forests and is worthless here. BITS 2-5 ARE THE REST OF THE WATER GROUP (user 2026-08-30: "every single water setting") — 2 caustics, 3 the underwater look, 4 splash/wake ripples, 5 shoreline surf; see WBIT2/LGT2_WATER below and the [Y] panel. Bit 2's last tenant before them was the SVGF HISTORY FIX (user 2026-08-29, "address the noise issue … the noise is worse on the grass") — a silhouette pixel that fails the temporal depth test borrows a neighbour's converged history instead of rendering one raw ray. It lived on [Y] for exactly as long as it took the user to look at the A/B and was BAKED IN the same day, which is why the bit was spare for the water rows to take; see HISTORY FIX in render/wgsl/denoise.js. Before it the bit held SURFACE RINGS — the ring a splash leaves and the wake behind a swimmer or a duck — which was BAKED IN on 2026-08-29 and now runs unconditionally; see ripHF in render/wgsl/pre.js. A RIVER/LAKE wave character sat here briefly the same day and was reverted (the wave sum sampled in each water body's own frame; the generator's riverAt already carries the flow direction and width, so it is buildable again if it ever comes back). Before those the bit held, and lost, WHITECAPS on the crests (removed on request), the per-pixel waterline and SNELL'S WINDOW (both reverted on sight — do not rebuild either), and the ported cloud deck. Earlier still, a per-pixel sun accumulation window and variance-driven spatial filtering, both measured and REVERTED — the sun window is arithmetically a no-op at 1x, and the variance radius measured -5.4% residual noise, inside the run-to-run scatter. The 25 bits above bit 5 are still free.
  const LGT2_WATER = 0x3c;                             // bits 2-5 of the SECOND mask — the four water terms that overflowed lgt.x (caustics, underwater, ripples, shore surf). The only bits of lgt.z the panel exposes and the only ones restored from storage, exactly as LGT_WATER is for lgt.x.
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
  // ── EVERY WATER TERM ON ITS OWN SWITCH (user 2026-08-30: "give me every single water setting to toggle
  // off and on") ── the first six live in u.lgt.x bits 18-23 and are the group the panel has always carried;
  // the last four are NEW and live in u.lgt.z (LG2), because lgt.x is FULL at 24 bits (an f32 is exact only
  // to 2^24, so a 25th bit there would round). They were all previously baked in with no way to switch them:
  //   caustics   — the webs on the refracted bed, on a submerged creature, on anything you swim past, and the
  //                god-ray shafts in the underwater march. Four call sites, one button, so it is all or none.
  //   underwater — the whole submerged look: Beer-Lambert absorption over the in-water path plus the marched
  //                single scatter. Off, swimming looks like standing in air.
  //   ripples    — the splash rings and the wakes behind a swimmer or a duck (ripHF; both the in-march sample
  //                and the far-water fallback).
  //   shoreSurf  — the churned foam band where water meets land, AND the voxel lift that stands it proud.
  // The bake below is what a fresh player gets; the panel's `reset` returns to exactly this.
  const WATER_BAKE = { reflect: 1, refract: 1, foam: 1, ice: 1, pixelGlisten: 1, waves: 0, reflection: 0.45,
                       caustics: 1, underwater: 1, ripples: 1, shoreSurf: 1 };
  const WBIT = { reflect: 18, refract: 19, foam: 20, ice: 21, pixelGlisten: 22, waves: 23 };   // …their bits in u.lgt.x
  const WBIT2 = { caustics: 2, underwater: 3, ripples: 4, shoreSurf: 5 };                      // …and these four in u.lgt.z (LG2)
  const wBakeMask = () => { let m = LGT_ALL & ~LGT_WATER; for (const k in WBIT) if (WATER_BAKE[k]) m |= (1 << WBIT[k]); return m; };
  const wBakeMask2 = () => { let m = LGT2_ALL & ~LGT2_WATER; for (const k in WBIT2) if (WATER_BAKE[k]) m |= (1 << WBIT2[k]); return m; };   // …and the non-water bits of the second mask are forced on, so `reset` also puts the rock sheen and the cross-face fallback back
  const wBakeRefl = () => { const v = +WATER_BAKE.reflection; return (isFinite(v) && v >= 0 && v <= 2) ? v : 1; };
  // Everything OUTSIDE the water group is FORCED ON at load. The panel used to carry all 24 terms, so a
  // saved mask can have sun shadow / AO / fog / TAA switched off from an earlier bisection — and with those
  // rows gone there would be no way left to switch them back. Only the water bits are restored from storage.
  let lgtMask = (() => { try { const v = localStorage.getItem('vb_lgt');
    return v === null ? wBakeMask() : (((parseInt(v, 10) & LGT_WATER) | (LGT_ALL & ~LGT_WATER)) & LGT_ALL); } catch (e) { return wBakeMask(); } })();
  // ── AO RAY REACH ── how far the ambient-occlusion ray marches, in voxels. Lives out here in the shared scope
  // rather than beside its writer in tick-emit, which bundle.py wraps in its own module IIFE. Rides in the spare
  // physC.z uniform lane so it can be swept LIVE with __vb.aoReach(n): rebooting between A/B configs re-rolls the
  // creature population, and since every trace ray walks every rigid body, that moved the trace pass by more than
  // the effect being measured (two identical runs read 2.22 and 1.51 ms).
  let AO_REACH = 24;
  // ── THE SECOND MASK IS PERSISTED THE SAME WAY, AND ONLY ITS WATER BITS ARE ── identical rule to lgtMask
  // above: bits 2-5 (the four new water terms) come back from storage, everything else is forced on at load.
  let lgtMask2 = (() => { try { const v = localStorage.getItem('vb_lgt2');
    return v === null ? wBakeMask2() : (((parseInt(v, 10) & LGT2_WATER) | (LGT2_ALL & ~LGT2_WATER)) & LGT2_ALL); } catch (e) { return wBakeMask2(); } })();   // …and it starts at the bake when there is nothing stored. It used to be deliberately NOT restored, because every bit in here was a whole-scene look term and a player who bisected one off in an old session must not be stuck with it. Bits 2-5 are WATER now, so they follow lgtMask's rule instead: the water group survives a reload, the whole-scene bits (0 rock sheen, 1 cross-face fallback) are still forced on at load.
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
  // AND EVERY SPAWN CLEARING KEYS OFF THE SUM, NOT THE ANCHOR (world/terrain.js): the trees, rocks, shrubs
  // and cacti that refuse to stamp near the player all measure from SPWX + SPOX. Measuring from SPWX put
  // the clearing 2160 voxels away — beyond the streaming window, so it protected nothing and the player
  // spawned in the crowns. SPOX is registered in gen-pool.js's consts for the same reason SPWX is: the
  // gen workers evaluate those refusals themselves, and a const missing from that list is silently 0.
  let SPOX = 0;
  // ── AND THE Y THE SPAWN SEARCH CHOSE, WHEN IT CHOSE ONE (2026-08-30) ── -1 means "nothing to say, use hmap",
  // which is every biome but the arctic. There it is the standing y of the glacier summit the search settled
  // on, and sim/player.js needs it told rather than derived: hmap is the seabed under the ice, and scanning W
  // downward for the first solid voxel instead finds whatever is standing on the summit — worldgen stamps the
  // penguin colonies before the player is placed, so that scan spawned the player on a penguin's head, nine
  // voxels up, and dropped them the moment the bird walked out from under them. arctIceTop is exact here (it
  // is what the colony siting itself is built on) and was verified against the stamped column on four boots.
  let SPY = -1;
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
  // ── WHY THERE IS NO SUB-VOXEL DITHER HERE, THOUGH THE BASIN AND RIVER CARVES BOTH HAVE ONE ──
  // Tried twice on 2026-08-31 against the contour-ring report ('the birch terrain has a weird design'), and
  // reverted both times. It does not work on THIS field and the numbers say why. Measured over a 300x300
  // patch, share of columns standing on a 1-voxel riser, and the longest unbroken flat run:
  //     no dither        oak  8.3% risers, longest flat run 126     birch 49.0%, 150
  //     ihash +-0.45     birch 65.7%
  //     vnoise +-0.95    oak 73.2%                                   birch 72.8%
  //     vnoise +-0.375   oak 70.0%, longest run 49                   birch 44.0%, 32
  // The dither does what it claims - the 126-voxel terraces break into 49 - but the cost is that a column is
  // on a riser 70% of the time instead of 8%. That trades a contour line for an all-over stipple, which is a
  // different artefact and a worse one. The ground MATERIAL is identical on risers and flats (ids 9/10/11 at
  // 32% each on both), so what draws a contour is the shading on the riser, and there is no amount of height
  // noise that gives both few risers and short ones.
  // AND THE 'TOO FLAT' READING THAT PROMPTED A RELIEF PASS WAS A SAMPLING ERROR. 8.3% risers came from ONE
  // patch. Over 2000 scattered probes the oak field stands a column on a riser 59.7% of the time at a mean
  // slope of 1.156 - it was never flat, and the relief that was briefly added on the strength of that number
  // has been taken back out. terrain.js already carried the answer above the grass-strand block: the rings are
  // a KNOWN accepted trade with four prior attempts recorded, and it says not to attack them in the height
  // field. This is the fifth and sixth.
  // What IS new is the mechanism, measured at a pinned spawn: a ground-cover blade sits on 99.7% of FLAT
  // columns and only 63.8% of RISER columns. Risers follow contours, so a third of every contour is bare
  // ground. That is the thing to attack, and it is in the cover scatter, not here.
  const LIFE_OFF = 0;   // ── LIFE IS BACK (user 2026-08-31: "add the rest of the life from the original pine forest") ──                                  // ── NO LIFE (user 2026-08-31: "dont add any life yet") ── the whole creature
                                                       // system is intact and gated at one line in main/tick-creatures.js. Set to 0 to restore it.
  // …and the ceiling is now the TREE RESERVE alone. `130 + LIFT` was the other half of this min() and it
  // moved with LIFT, so lowering the float would have pulled the mountain tops down by exactly the 80
  // voxels it was meant to free - the ceiling has to be measured from the TOP of the world, not from a
  // baseline that just dropped.
  const HMAX = WY - 158;         // terrain ceiling — and the 151 is the PINE RESERVE, see below
  // ── AND THE SECOND TERM IS THE TREE, WHICH IS WHAT MAKES THIS A BUDGET AND NOT A DIAL ── 122 was never an
  // arbitrary reserve: it is pine5.vox's own 116 voxels plus a little, because a trunk planted at HMAX has to
  // fit under WY or its crown is cut off by the window's ceiling. That coupling was invisible until the pine
  // was rescaled and 15.3% of the biome started refusing trees to avoid flat-topped ones (measured with
  // __vb.treeDensity: 377 trees in 2485 cells above 190, against a healthy 0.67 per cell).
  // So terrain height and tree height spend ONE budget, WY, and the user asked for mountains WITH pine trees
  // on them - which settles it, because a treeline is mountains WITHOUT them. At 1.25x the pine is 145 and
  // the reserve is 151, so the ceiling lands at 233 and every column in the biome can carry a whole tree.
  // 1.5x would have forced this to 204 and cost more relief than the taller tree bought.
  // ── RAISED 105 -> 130 FOR THE PINE MOUNTAINS (user 2026-08-31: "make the terrain height elevation vary
  // greatly. I want to almost see mountains with pine trees on it") ── and it cost nothing, because the
  // OTHER term in this min was never binding: WY - 122 is 262 and the ceiling was pinned at 233 by the
  // literal, so 29 voxels of vertical the window already paid for were simply unreachable. 130 takes 25 of
  // them and leaves 4 spare. Nothing else moves: oak crests at 226 and arctH is built on oak's field, so
  // every existing biome is under the OLD ceiling and cannot notice a higher one. Only pineH reaches up.
  // Worth stating what this does NOT buy. The world is on a 10 cm grid, so 258 - WL is 106 voxels = 10.6 m
  // of relief above water, and a pine tree is 11.6 m before the 1.5x. These are big HILLS. Real mountains
  // need WY itself to grow, and WY is the brick grid's height: GBY 48 -> more, which multiplies the page
  // pool and lands on the RAM budget this ships under. That is a deliberate call, not an oversight.
  const WL = 24 + LIFT;                                // GLOBAL water level — water is simply terrain below this line
  const baseH = (x, z) => {
    const b = 8 + LIFT + 88 * fbm(x * 0.008, z * 0.008);
    const shoreK = Math.min(1, Math.abs(b - WL) / 12);   // fine detail fades out near the waterline — smooth, beach-like entries into water
    return Math.min(HMAX, Math.max(4 + LIFT, Math.round(oakRoll(b + 9 * fbm(x * 0.04 + 7.3, z * 0.04 + 2.1) * (0.2 + 0.8 * shoreK), x, z))));   // ── ROUNDED OAK HILLS ── the forest expression is untouched; oakRoll (below, with oakM) either hands it straight back or replaces it with the oak forest's own rounded field. makeHRow and makeHCol wrap their own copies of this same expression in the same call
  };
  // ── THE LAKE THRESHOLD, AND WHY IT IS A CONSTANT NOW ── this number is INLINED IN THREE PLACES: here, and
  // again in makeHRow and makeHCol (world/gen-noise.js), which carry their own copy of the height expression
  // and decompose the same noise into row/column form. All three must agree bit for bit or the bulk fill and
  // the placement queries disagree about where the ground is — __vb.gtest() is what measures it. Naming it
  // does not remove the duplication (the row/col forms cannot call this), but it does mean a change here is
  // visibly a change to a shared constant rather than to a magic number.
  const BASIN_T = 0.030;                               // base: how much of the low-frequency basin field drops under the waterline
  // ── AND THE ARCTIC GETS TWICE THE WATER (user 2026-08-30: "double the rate of water in the arctic") ──
  // added on top of the base rather than replacing it, and scaled by the biome mask so the extra lakes fade in
  // with the snow instead of appearing along the band's edge. TUNED BY MEASUREMENT, not by arithmetic: the
  // covered area is the noise field's cumulative distribution below the threshold, and that is not linear in
  // the threshold, so "double the number" would not have doubled the water.
  const BASIN_ARCT = 0.330;                            // 0.038 -> 0.048 -> 0.105 -> 0.193 -> 0.330: doubled on request four times, each step MEASURED rather than scaled — the wet fraction is the basin noise's CDF below the threshold and is nowhere near linear in it (user 2026-08-30: "double the water surface area in the arctic")
  const basinT = (x, z) => BASIN_T + BASIN_ARCT * arcticM(x, z);
  // ── AND HOW HIGH A BASIN MAY FORM, WHICH IS WHAT MAKES A POCKET BIG ── the carve is gated on low ground, so
  // with a fixed ceiling a basin is clipped wherever the land rises through it and a lake comes out as a
  // scatter of small pockets in the valley floors (user 2026-08-30: "the water in the arctic they are small
  // pockets. make the pockets much much bigger"). Raising the ceiling in the arctic lets one basin flood a
  // whole bowl instead of only its lowest corner, so the pockets JOIN UP rather than merely multiply — which
  // is a different lever from the threshold above, and the one that actually changes their size.
  // Like the threshold, this line is INLINED IN THREE PLACES (H, makeHRow, makeHCol) and is a shared helper
  // for that reason: the arithmetic exists once so the three copies cannot drift.
  const BASIN_LOW = 55;                                // ceiling, over LIFT, under which a basin may carve at all
  const BASIN_ARCTLIFT = 34;                           // …and how much higher the arctic's may reach
  const basinLow = (h, x, z) => Math.max(0, Math.min(1, (BASIN_LOW + LIFT + BASIN_ARCTLIFT * arcticM(x, z) - h) / 20));
  const basinM = (x, z) => {                           // huge, rare low-frequency basins pull the land under the waterline
    const b = vnoise(x * 0.0016 + 313.7, z * 0.0016 + 157.3);
    const t = basinT(x, z);
    if (b >= t) return 0;
    return sstep(Math.min(1, (t - b) / 0.06));
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
  const BIOP = 30240;   // DOUBLED 2026-08-30 (user: "double the bands of all the biomes") - one uniform scale of the whole layout, so every offset below doubles with it and the strip sequence is unchanged.   // SEVEN strips now (7 * W): the ARCTIC is a new 2160 strip between the spawn pine and the birch (user 2026-08-29), so the period grows by exactly one strip again and BIRCHOFF/DESOFF each slide one strip west to open the gap. SPAWN DOES NOT MOVE - it sits in the pine strip at -1080..+1080 and the arctic takes the slot birch used to hold, which is what keeps every spawn guarantee in this file true. Was SIX strips: the BIRCH band is a new 2160 strip between the spawn pine and the desert, so the period grows by exactly one strip (6 * W by construction, never a sum of measured pieces)                                  // one full cycle: SIX strips of 2160 — oak, cherry, oak, pine, desert, pine. 6 * W by construction, never a sum of measured pieces
  const pwrap = (d) => d - Math.floor(d / BIOP + 0.5) * BIOP;   // signed distance into [-BIOP/2, BIOP/2). floor(x + 0.5) rather than Math.round because the WGSL port must agree with this bit for bit, and WGSL's round() breaks ties to EVEN where Math.round breaks them upward
  const DESOFF = 10800, DESB = 900, DESW = 2000;   // 1080 -> 3240 -> 5400: one strip further west each time a band is inserted inboard of it - first the BIRCH, then the ARCTIC (see BIOP)        // how far the pine/desert line sits EAST of spawn; blend width; boundary meander (voxels, 10 cm each). 2300 -> 4460 = OAKOFF + W, so the pine strip between the oak line and the sand is one full 2160-wide strip like every other strip in the period. DESB is deliberately NOT doubled with it — a blend is a TREELINE, not a biome, and widening it would drag life's 0.15/0.85 admit ends (main/tick-creatures.js) and the weather contrast curve along with it
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
  const OAKOFF = -2160, OAKB = 900, OAKW = 1080;         // where the oak/pine line sits (the line is at -OAKOFF, so a NEGATIVE value puts it EAST of spawn — 1080 east, since the 2026-08-21 pine slide above); blend width; the INDEPENDENT half of the meander. 1220 -> 2300 = the blossom's east midpoint (spawn+140) plus one full strip W, so the PURE oak between the pink and the pines is 2160 like everything else. Spawn is still 2300 WEST of this line, so oakM(SPWX,SPWZ) is still exactly 1 and "spawn is in the oak forest by construction" still holds — the cherry band simply sits inside it
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
  const OAKWOFF = -10800;                               // the WEST boundary, as a signed distance from spawn: the blossom's west midpoint less one strip W, carried 1360 east with everything else by the pine slide (was -4040). OAKC/OAKH are derived from this and OAKOFF, and OAKFAR/OAKNEAR/OAKWFAR/OAKWNEAR from those, so the whole oak geometry follows these two numbers and nothing else has to move
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
  const oakM = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
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
  const CHOFF = BAND_MIRROR * 8640, CHHALF = 1710, CHB = 900, CHW = 520;
  // ── CHHALF/CHB RESHAPED SO THE BLOSSOM'S OUTER EDGE *IS* THE OAK BAND'S (user 2026-08-31: "I see a thin
  // slice of oak forest between the pine forest and the cherry forest. dont let this happen") ── measured
  // before the change, walking east off the pink: cherry fell to 0 at one x and oak kept going for a further
  // 0-450 voxels (median 140, over 30 latitudes), and every one of those columns plants an OAK. That strip
  // is the slice, and it had two causes, both of them the two bands being specified independently:
  //   * the BLEND widths differed. Both masks cross 0.5 on the same nominal line (oak's east edge and the
  //     blossom's are both SPWX + 10800), but oak faded out over OAKB = 900 and cherry over CHB = 400, so
  //     oak's tail simply outlived cherry's by 250 voxels. CHB is OAKB now, and CHHALF drops by the same
  //     450 the wider ramp adds, so the 0.5 line does NOT move: 1710 + 900/2 = 2160 = the old 1960 + 400/2.
  //   * the MEANDERS differed, which is the half that actually mattered. cherryM rode chWob and oak rides
  //     oakWob, and chWob is 0.6*oakWob + its own octave, so the two edges wander apart by up to +-800
  //     voxels. No amount of blend tuning fixes that: matching the widths alone would just make the strip
  //     as often CHERRY spilling onto pine as oak. So cherryM below now rides oakWob outright.
  // Together those make the invariant the file has always claimed - "cherryM is a SUB-REGION of oakM" -
  // true by CONSTRUCTION on the outer edge rather than by two independent fields happening to agree.
  const CHWHALF = 1960;                                // …and the WEATHER band keeps the OLD half-width. cherryW's ramp was tuned against it (see the CHBW note below: 600 put the snow line 45 m short of the treeline and it was halved to 300), so folding the mask reshape into the weather would move a snow line that has already been placed by hand twice.   // ── UNCHANGED BY THE EQUAL-STRIP PASS (user 2026-08-19: "make all of the biomes the exact same size... double the size of the bands") ── this band was doubled on 2026-08-18 (1080 -> 2160 measured between mask midpoints, 2 * (CHHALF + CHB/2)) and 2160 is precisely the width every OTHER strip has now grown to, so nothing here moves: spawn still sits 140 inside the EAST edge with the whole 2020 of blossom ahead in the facing direction, bit for bit   // band centre, west of spawn; half-width of PURE blossom; blend width; its own meander   // CHOFF is MIRRORED (see BAND_MIRROR): the blossom's centre is SPWX - CHOFF, so a negative CHOFF puts it EAST of spawn, in the sunrise. Magnitude unchanged.
  const chWob = (z) => oakWob(z) * 0.6 + (vnoise(z * WOB_CH + 211.3, 97.7) - 0.5) * CHW;   // carries 0.6 of the OAK meander for the reason oakWob carries 0.6 of the desert's: two free meanders converge and let bands touch. Its own half is small because this band has the least room of the three
  const cherryM = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
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
  const cherryW = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
  const CHREACH = Math.max(CHHALF + CHB, CHWHALF + CHBW) + 2 * (DESW * 0.675 * 0.6 + OAKW * 0.5);   // …on OAKWOB's swing now, not chWob's, because both cherry masks ride oakWob — and over the WIDER of the two ramps, since chNear gates cherryW as well as cherryM. A cheap bound may under-cover by nothing.
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
  const oakWeather = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;   // cherryW, NOT cherryM: the weather border feathers over CHBW while the treeline keeps CHB (see the block above)
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
  // ── THE OAK FIELD'S SHAPE, SPLIT OUT SO THE BIRCH CAN RIDE IT WITHOUT COPYING IT ── oakH is the oak forest,
  // the birch forest and the arctic's base all at once, so "give the birch its own lakes" cannot just be a
  // second term on it. Naming the 0..1 curve lets birchH subtract from the SAME field instead of restating
  // its two fbm calls, which is the copy that goes stale the first time either frequency is touched.
  // oakH itself is unchanged to the bit: same expression, same order of operations.
  // ── FREQUENCY IS THE RELIEF LEVER, NOT AMPLITUDE (user 2026-08-31: "restore relief") ── the contour rings
  // are what a nearly level surface looks like once it is quantised to whole voxels: measured, the oak field
  // stood a column on a 1-voxel riser only 8.3% of the time, and each of those risers then ran unbroken for
  // up to 126 voxels. That is a contour line by construction, and no height dither fixes it (see the note
  // above HMAX for the four attempts and their numbers).
  // Amplitude cannot be the answer here: OAKY + OAKHILL is 226 against an HMAX of 233, so there are seven
  // voxels of headroom in the whole biome. Frequency costs nothing from that budget - the same range drawn
  // over a shorter wavelength is the same hills, closer together, with every slope proportionally steeper.
  // 0.0024 -> 0.0031 is a 1.3x. 1.75x was tried first and overshot hard - oak came out at 66% risers and a
  // mean slope of 1.59, i.e. mountainside, because this field runs through sstep(sstep(...)) and the double
  // smoothstep multiplies whatever gradient it is fed by up to 2.25 before it reaches the ground.
  const oakFieldM = (x, z) => {
    const a = fbm(x * 0.0024 + 91.7, z * 0.0024 + 33.1);
    const b = fbm(x * 0.0057 + 47.3, z * 0.0057 + 8.9);
    return sstep(sstep(a * 0.82 + b * 0.18));
  };
  const oakH = (x, z) => OAKY + OAKHILL * oakFieldM(x, z);   // OAKY .. OAKY + OAKHILL, never negative
  // ── AND THE BIRCH FOREST GETS THE PINE'S LAKES (user 2026-08-31: "give the birch forest the same amount of
  // water as you did the pine forest") ── the same shape pineH uses: the low end of the field pulled under the
  // waterline by (1 - m) squared, so the drop is confined to the bottom of the range and the rim passes
  // through H's beach window on its way out, which is what gives a lake a beach instead of a drowned edge.
  // Scaled off OAKHILL the way PINE_LAKE is off PINEHILL, because this field's range is the smaller of the two.
  const BIRCH_LAKE = 22;
  // …and the same waterline shelf pineH carries, for the same reason: without it the field's own gradient at
  // the shore terraces every four voxels, and the birch has lakes now so it has the same shore to get right.
  const birchH = (x, z) => { const m = oakFieldM(x, z);
    const sh = m <= PINE_SHA ? 0 : m >= PINE_SHB ? 1 : sstep((m - PINE_SHA) / (PINE_SHB - PINE_SHA));
    return OAKY + OAKHILL * m - BIRCH_LAKE * (1 - m) * (1 - m) - PINE_SHELF * sh; };
  // ══ THE PINE FOREST'S OWN FIELD (user 2026-08-31: "give the pine forest the same terrain generation as the
  // oak forest but leave the styling unique to the pine forest ... make the terrain round and hilly like the
  // oak forest. however ... make the terrain height elevation vary greatly") ══
  // Same SHAPE as oakH — long wavelength, double-smoothstepped, positive-only — because "round and hilly like
  // the oak forest" is a statement about the field, and this is that field. Three things differ, and all three
  // are about RELIEF rather than style:
  //   * the range is 118 voxels against oak's 78, and it starts 8 under the waterline rather than 4 over it, so
  //     pine valleys flood into inlets and pine crests stand ~106 over the water instead of ~74;
  //   * the outer curve is SQUARED. oakH's double-sstep spreads its range evenly, which gives a landscape that
  //     is high nearly everywhere - a plateau, not mountains. Squaring biases the mass to the low end, so most
  //     of the biome stays low rolling ground and only a minority climbs. A peak reads as a peak because the
  //     land around it does not;
  //   * a third, short octave rides on top, faded in by the same curve, so the high ground gets ridges and
  //     broken faces while the valley floors stay as smooth as oak's.
  // STYLING IS NOT TOUCHED HERE and cannot be: what a column is MADE of (grass, dirt, needles, the pine's own
  // litter and moss) is decided in world/terrain.js off the biome masks, not off the height. This changes only
  // where the ground is.
  // ── AND THE FIELD'S WAVELENGTH IS WHAT SETS THE TERRACE WIDTH (user 2026-08-31: "create more surface area
  // between the upper steps") ── a terrace is one voxel of rise, so its width is 1 / (dh/dm * dm/dx). Seven
  // attempts went at dh/dm - the bank cone, two height remaps, the m-space shelf - and none of them moved the
  // measured 4 voxels, for a reason the coverage numbers finally made obvious: bankDist reaches 5 of 39 pine
  // shorelines, so on 34 of them there is no cone to tune and the terrace is the RAW field. At the waterline
  // that field runs dh/dm ~88 against dm/dx ~0.005, which is a gradient of 0.4 - a step every two to four
  // voxels, exactly what was measured.
  // dm/dx is the untouched half of that product, and it is just the noise frequency. Halving both octaves
  // doubles every wavelength: the same hills and the same lakes, drawn twice as large, with every slope in
  // the biome half as steep. It costs nothing per column - the same two fbm calls - and unlike a remap it
  // genuinely moves land horizontally, which is the only thing that widens a terrace.
  const PINE_LAKE = 15;                                // how far under PINEY the lowest ground is pulled — see the note under pineH
  const PINE_SHELF = 0, PINE_SHA = 0.35, PINE_SHB = 0.55;   // a flat shelf in the m->h curve, its amplitude and the m range it spans
  // ── AND A SHELF AT THE WATERLINE, BUILT INTO THE FIELD (user 2026-08-31: "fix the bank steps") ── measured
  // first: of 30 pine shorelines, oakBank's cone reached FOUR. bankDist walks WATERSHED geometry, and 26 of
  // those shores are this field's own lakes - rs 0, basin 0 to the skirt - so they never had a bank at all
  // and kept the raw gradient, one terrace every four voxels from WL+3 up.
  // The cone is the right shape and the wrong place to add it: it needs a DISTANCE to the water, and this
  // water has no geometry to measure a distance from. But `m` already IS one - it falls monotonically from
  // the ridges into the lakes - so the shore can be shaped in the m -> h curve instead, which is the same
  // move cherryM made when it was carved out of oakM rather than given a field of its own.
  // Subtracting a smoothstep centred on the m where h crosses WL flattens dh/dm exactly there: the curve
  // holds near the waterline over a wider span of m, which is a wider span of GROUND, which is wider
  // terraces. A local remap of h could never do this (it was tried and reverted): pinning both ends of a
  // height band forces it to expand wherever it compresses. Moving the curve in m has no such constraint.
  // It costs the peaks PINE_SHELF voxels, which is why PINEHILL goes up by the same amount below - the
  // shelf is a step DOWN that never comes back, so the summits have to be given it back explicitly.
  const PINEY = LIFT + 20, PINEHILL = 85;              // valley floor = OAKY exactly, crest 85 above it — 233, exactly HMAX, so no summit is flattened by the clamp
  // ── PINEY WAS LIFT + 16 AND THAT DROWNED THE SHORELINE (user 2026-08-31: "the pine forest has patches of
  // missing trees", and "get rid of the sinking effect of the sand") ── both reports are the same voxel.
  // H lifts near-water ground onto a beach with `h <= WL && h >= WL - 2`, a window of [147, 152]. A valley
  // floor at 144 falls UNDER that window, so it was never lifted: the sand sat 8 voxels below the water,
  // which is the sinking - and treeAt's `H <= WL + 4` test then refused every one of those columns, which
  // is the patches. Measured with __vb.treeDensity: 0 trees in 825 cells below 150, against a healthy 0.67
  // per cell from 160 up. LIFT + 20 is OAKY to the voxel, so the pine valley floor now lands INSIDE the
  // beach window exactly as the oak forest's always has, and gets lifted to WL + 1..3 like everything else.
  // Water in the pine forest now comes from where it comes from everywhere else - rivers and basins carved
  // through this field - rather than from the field itself starting under the sea.
  // PINEHILL drops 118 -> 108 to keep the crest at 256: the floor went up 4, so the range gives back 10 and
  // the top of the biome does not move into HMAX's clamp, which would flatten the summits.
  const pineH = (x, z) => {
    const a = fbm(x * 0.00165 + 61.3, z * 0.00165 + 77.9);   // the massifs — see the WAVELENGTH note under PINEY
    const b = fbm(x * 0.00345 + 25.1, z * 0.00345 + 13.7);   // and the shoulders on them
    const m = sstep(sstep(a * 0.78 + b * 0.22));
    const k = m * m;                                   // …squared: broad low country, and the top of the range reached rarely
    const sh = m <= PINE_SHA ? 0 : m >= PINE_SHB ? 1 : sstep((m - PINE_SHA) / (PINE_SHB - PINE_SHA));
    return PINEY + PINEHILL * k - PINE_LAKE * (1 - m) * (1 - m) - PINE_SHELF * sh
         + 9 * (fbm(x * 0.028 + 3.7, z * 0.028 + 9.1) - 0.5) * k * k;   // ridge detail — faded on k SQUARED, see below
  // ── AND THE RIDGE DETAIL IS FADED ON k*k, NOT k (user 2026-08-31, "fix the bank steps") ── this rides at
  // wavelength ~36 voxels, so it is the one term in the field that varies fast enough to make a terrace on
  // its own. Faded on k it still carried +-1.9 voxels at the shoreline (k = 0.21 where h crosses WL), which
  // is a local gradient of ~0.2 - the same order as the whole rest of the field put together, and enough to
  // step the beach every four voxels no matter how flat the underlying curve is made. That is why the bank
  // cone, the waterline shelf and the m-space shelf all measured no change in turn: each one flattened a
  // slope that was not the one doing the stepping.
  // k*k puts it at +-0.4 there and leaves it untouched on the ridges it was added for.
  };
  // ── AND THE LOW END IS PULLED UNDER THE WATER (user 2026-08-31: "increase the surface area of the water in
  // the pine forest") ── the mirror of the squaring above: k = m*m lifts the top of the range rarely, and
  // this drops the BOTTOM of it by the same shape reversed, so the lowest ground in the biome goes to
  // 148 - 34 = 114, which is 38 under the waterline. That depth is the point. The first attempt at pine
  // water was a flat field floor 8 under WL, and it produced the "sinking sand" report rather than lakes:
  // too shallow to read as water and too low for H's beach rule, which only lifts ground in [WL-5, WL].
  // A basin that goes properly deep has a RIM, and the rim passes through that window on its way out, so
  // these come with beaches on them the way the oak forest's lakes do.
  // Done here rather than by raising BASIN_T because pineH is already the pine forest's own field and is
  // evaluated nowhere else: the oak forest, the birch and the arctic keep exactly the water they had, and
  // it costs no new noise call - m is already computed two lines up.
  // ══ THE ARCTIC'S OWN GROUND ══ and it is the BIRCH FOREST'S, exactly (user 2026-08-30: "make the terrain
  // generation match the birch forest"). The birch band wears oakH — see the note under BIRCHOFF — so the
  // arctic calls oakH too, and the three bands roll on one shared height field. There is no arctic landform
  // code, which is the point: nothing can drift between them because there is only one field.
  //
  // THIS REPLACES A MUCH LARGER FIRST BUILD, and what it dropped is worth recording so it is not rebuilt by
  // reflex. That version stacked three fields — oakH's hills, a RIDGED mountain field (1 - |2f - 1|, gated and
  // cubed so ranges occurred in places), and a glacier surface the ground was lifted UP to with a max(), so
  // valleys flooded with ice and left the peaks proud as nunataks. It worked, and it is gone because the ask
  // changed to "match the birch forest", which no amount of mountain is.
  //
  // ── AND THE CONTOUR RINGS ARE FIXED IN COLOUR, NOT IN GEOMETRY ── worth the whole of this note, because
  // three height-based attempts failed before the answer turned out to be somewhere else entirely.
  // A smooth height field quantised to whole voxels steps along its own iso-heights, and every step is a
  // 1-voxel lip running the length of one. Under the birch that is invisible; on bare snow birch's own rounded
  // hills came out as a topographic map in white, concentric rings following every dome. Three tries at
  // deforming the terrain to hide it, all measured on screen:
  //   * ±1.28 voxels, half of it WHITE noise → a field of loose blocks. Past half a voxel a dither stops
  //     nudging columns already on a rounding boundary and starts moving arbitrary ones, so neighbours
  //     disagree by a whole voxel at random.
  //   * ±0.45, the textbook sub-voxel dither → the rings survived, plus single-voxel speckle from the white
  //     half. It cannot work here for a geometric reason: these hills are long-wavelength and shallow, so one
  //     step spans a wide band of ground, and moving the edge of a line tens of voxels wide by half a voxel
  //     is not a visible change.
  //   * ±1.5 coherent drifts at a ~25-voxel wavelength → regular parallel corrugations, a ploughed field.
  //     Coherent noise at one wavelength IS a wave, and a wave over rolling ground reads as ridges.
  // THE STEP WAS NEVER THE PROBLEM — every biome has it. What the forests have and bare snow did not is
  // GROUND COLOUR VARIATION: the pine floor picks per column from a 4-entry ramp spanning ~13% in luminance
  // (NEEDLE), and that competing variation is what stops the eye joining the step edges into a line. Snow had
  // a 2-entry ramp spanning 6%, so there was nothing to compete. Widening the ramp (assets/palette.js) fixes
  // it where it actually lives and leaves the terrain alone — which is also what was asked for: the arctic's
  // ground is the birch forest's ground, to the voxel, with no arctic height code at all.
  // ── ARCTIC SNOWFALL, PARKED ── the arctic needs NO new snow code: tick-snow refuses the blanket by biome and
  // the arctic is not in that refusal, so it would settle here on exactly the pine forest's mechanics. Which
  // means switching it OFF is what takes a line. Flip this to true and the biome storms like the pine and the
  // cherry do, with the same sweep, blanket and thaw.
  // ── WHERE THE ICE STOPS BEING HOSPITABLE ── one number, and everything the arctic refuses to grow reads it:
  // trees, boulders, twigs, flowers, mushrooms, ferns, shrubs, cones, logs and lily pads. LOW on purpose
  // (user 2026-08-29: "now your putting pine trees in the arctic, dont do that"). A dithered gate was tried
  // first, to soften the border, and it does soften it — but softening a gate means SOME of it survives at
  // every mask value, so pines stood on open snow deep into the band. The border is smooth because the SNOW
  // fades in over 900 voxels while the planting stops at 0.15: the treeline and the snowline are in DIFFERENT
  // places. That is what a real treeline looks like, and one line doing both jobs is what read as drawn.
  // ══ AND IT IS 0.72 NOW, DITHERED, BECAUSE THE ARCTIC IS WATER (user 2026-08-30: "improve the transitions
  // from the arctic to the neighboring biomes") ══ 0.15 was written when the arctic was LAND, and it had to be
  // low: the ask it came from was "now your putting pine trees in the arctic, dont do that", and on an ice
  // sheet any surviving tree is a tree standing on a glacier. The note below records that a dithered gate was
  // tried FIRST and rejected for exactly that — softening a gate means some of it survives at every mask
  // value, so pines stood on open snow deep into the band.
  // NEITHER OBJECTION SURVIVES THE ARCTIC BECOMING SEA. There is no ice sheet to stand on any more; past the
  // waterline there is only water, and every planter in terrain.js already refuses a wet column on its own
  // (`H(wx, wz) <= WL + 4` in treeAt, and the equivalent in each of the others). So "deep in the band" is not
  // somewhere a tree can reach whatever this number says.
  // WHAT 0.15 LEFT BEHIND WAS THE REAL PROBLEM, and it is what the transition ask is about: planting stopped
  // at am 0.15 while the shoreline sits at roughly am 0.75 (the rim lerps ~47 voxels from forest ground down
  // to the bed, and crosses WL about three quarters of the way), so between the last tree and the water lay
  // 500-plus voxels of bare white ground with nothing on it at all — a blank plain, wearing the contour rings
  // that bare quantised snow always shows. The eye read arctic sea, then emptiness, then a wall of forest.
  // 0.72 puts the last stragglers just short of the water, and the DITHER is what makes it a treeline rather
  // than a second line further out: refusal probability rises linearly from 0 at the forest to 1 at
  // ARCT_BARE, so the wood thins over the whole rim and ends where the sea begins. The ceiling is explicit
  // (`am >= ARCT_BARE`) rather than left to the dither, which is the piece the 2026-08-29 attempt lacked.
  // GROUND COVER IS DELIBERATELY NOT ON THIS PATH — twigs, cones, toadstools, ferns, flowers, boulders and
  // logs keep the hard 0.15 cut through `arcticM(...) > ARCT_GROUND`. A thinning wood over clean snow is a
  // treeline; a thinning wood over snow with mushrooms in it is the "reads as a bug" case the ground-cover
  // note already argues, and it is still right.
  const ARCT_BARE = 0.72;                              // where the LAST tree stands — dithered in through arctBare()
  const ARCT_GROUND = 0.15;                            // …and where ground cover stops dead, which is what ARCT_BARE used to be for everything
  const arctBare = (x, z, salt) => {                   // true = refuse to plant here. salt keeps species from thinning in the same places
    const am = arcticM(x, z);
    if (am <= 0) return false;
    return am >= ARCT_BARE || ihash(x * 3 + salt, z * 5 + salt * 7) < am / ARCT_BARE;
  };
  const ARCTIC_SNOW = false;
  // ══ THE ARCTIC IS OPEN SEA (user 2026-08-30: "remove all of the regular terrain of the arctic and just have
  // water and snow caps. so it is essentially making the water 100% of the surface area with snow caps in it") ══
  // arctH used to be `oakH(x, z)` — the birch forest's ground, to the voxel. It is now a SEABED that never
  // reaches the water plane, so every column in the band comes out as lake and the only thing standing above
  // the surface anywhere in the biome is a snow cap.
  //
  // ── AND THE BED SITS JUST UNDER THE SURFACE, WHICH IS THE OTHER HALF OF THE SAME REQUEST (user 2026-08-30:
  // "put the terrain just below the water in the arctic. This way I can see how refraction would look across
  // all of the surface area") ── refraction in COMPOSITE traces a bent ray to the bed and absorbs what comes
  // back per channel over the path (Beer-Lambert, WATER_SIG). Over a deep lake almost nothing returns and the
  // surface reads as flat colour; the effect is only VISIBLE where there is a bed within reach. At 8 voxels
  // roughly 15% of the red and 60% of the blue survive the round trip, at 16 it is 2% and 35% — so this range
  // spans "sandy shallow" to "deep blue" and the whole band shows the effect instead of a fringe near the shore.
  // DELIBERATELY NEVER SHALLOWER THAN WL - 8: the beach-flat line in all three copies of H fires on
  // `h <= WL && h >= WL - 2` and would LIFT a bed inside that window straight back out of the water, one column
  // at a time, as a field of white islands. WL - 6 and below is out of its reach; this leaves two more voxels
  // of headroom on the rounding.
  // == DEEPER, AND THE DEPTH IS NOW THE FEATURE (user 2026-08-30: "its very shallow. can you increase the
  // depth of the water but keep the look of the refraction of the terrain? I also like how different areas
  // have different colors of refraction because of the shallowness of the floor") ==
  //
  // THOSE TWO ASKS PULL AGAINST EACH OTHER ON A MEAN AND AGREE ON A RANGE. Refraction is Beer-Lambert: what
  // comes back off the bed is exp(-sigT * path) per channel, sigT = (0.24, 0.092, 0.042). Push the whole bed
  // down and the return dies everywhere and the sea goes flat blue - the very effect the shallow bed was put
  // there to show. So the MEAN goes down (12 -> 19, a 58% deeper sea) and the RELIEF goes up much harder
  // (4 -> 11), which widens the band from 8-16 voxels to 8-30. That is the point: at 8 voxels 71% of the blue
  // and 15% of the red survive and the bed reads as bright sand; at 30 it is 28% blue and 0.07% red and the
  // same bed reads as deep ocean. The colour of a patch of sea IS its depth, and there is now nearly three
  // times as much of that range to look at.
  // 30 IS INSIDE THE REFRACTION REACH, DELIBERATELY. COMPOSITE caps the refracted ray at 34 voxels, so a bed
  // below that returns nothing at all and the water would go opaque rather than deep - the range stops just
  // inside it.
  // TWO OCTAVES, NOT ONE. One long wavelength gives smooth basins and one colour per bay; the second, at
  // nearly four times the frequency and a quarter of the amplitude, puts shelves and channels INSIDE a bay so
  // the colour breaks up at swimming scale as well as at map scale. The pair is what "different areas have
  // different colors" asks for.
  // AND THE SHALLOWEST IT MAY EVER BE IS STILL WL - 8: the beach-flat line in all three copies of H fires on
  // h <= WL && h >= WL - 2 and would lift a bed inside that window back out of the water as white islands.
  // DEEPER AGAIN (user 2026-08-30: "make the floor terrain deeper under the arctic water"), 19 -> 27 mean and
  // 11 -> 15 of relief, so the bed now runs WL-12 (bright shallow) to WL-42 (deep ocean).
  // THE REFRACTION RAY HAD TO GROW WITH IT OR THIS WOULD HAVE UNDONE ITSELF. COMPOSITE capped the refracted
  // ray at 34 voxels, on the argument that Beer-Lambert leaves under 3% past that - true of the RED channel
  // and wrong about blue, which still returns 24% at 34 and 17% at 42. With a bed at 42 and a ray that stops
  // at 34, the deep half of the sea would have gone flat in-scatter blue with no floor in it at all, which is
  // exactly the "no terrain under the water" state this whole thread has been undoing. The cap is 48 now.
  // That is not free but it is close to it: the ray is gated on fres < 0.80 and only runs at all where a
  // refracted path exists, and the water pass has repeatedly measured inside the noise floor.
  const ARCT_SEA = WL - 27;                            // mean seabed, twenty-seven voxels (2.7 m) under the surface
  const ARCT_SEAREL = 15;                              // relief either side of it: WL-42 (deep blue) to WL-12 (bright shallow)
  const ARCT_SEAF = 0.003;                             // the BASIN octave - ~330 voxels, so a whole bay shares a colour
  const ARCT_SEAF2 = 0.011;                            // ...and the SHELF octave at ~90 voxels, which breaks that bay up from close in
  const ARCT_SEAMIX = 0.72;                            // how much of the relief the basin octave owns; the shelf gets the rest
  // ── AND THE SUM IS SHAPED, BECAUSE fbm WILL NOT USE THE RANGE IT IS GIVEN ── the first cut simply scaled
  // two octaves by ARCT_SEAREL and measured 14-24 voxels against a design of 8-30: fbm clusters hard around
  // its own middle, so a nominal +-1 term spends almost all its time inside +-0.25 and the sea came out one
  // colour after all. Raising ARCT_SEAREL to compensate does not fix it, it just moves the same narrow band
  // deeper and makes the rare extremes absurd.
  // ARCT_SEAPOW is a tail-fattener: sign(u) * |u|^0.35 leaves 0 at 0 and 1 at 1 - so the deepest and
  // shallowest water are exactly where they were designed to be - and lifts everything in between. |u| = 0.23,
  // which is where the measured p90 sat, becomes 0.60. The 8-30 range is now actually inhabited rather than
  // merely permitted, and 8-30 is the whole point: it spans bright sand to deep ocean in Beer-Lambert terms.
  const ARCT_SEAPOW = 0.35;                            // <1 pushes the distribution out toward both ends; 1 would be the raw fbm this replaces
  const arctSeaH = (x, z) => {
    const u = (fbm(x * ARCT_SEAF + 91.7, z * ARCT_SEAF + 33.1) - 0.5) * 2 * ARCT_SEAMIX
            + (fbm(x * ARCT_SEAF2 + 11.3, z * ARCT_SEAF2 + 57.9) - 0.5) * 2 * (1 - ARCT_SEAMIX);
    const w = (u < 0 ? -1 : 1) * Math.pow(Math.min(1, Math.abs(u)), ARCT_SEAPOW);
    return ARCT_SEA + Math.round(w * ARCT_SEAREL);
  };
  const arctH = (x, z) => arctSeaH(x, z);              // …and the band's "ground" IS that bed — see above
  // ── ONE HELPER, CALLED FROM ALL THREE COPIES OF H ── H(), makeHRow and makeHCol each carry the same height
  // expression and have to agree BIT FOR BIT or the bulk fill and the placement queries disagree about where
  // the ground is (__vb.gtest is what measures it). So this is a scalar function that takes the height the
  // forest expression just produced and hands back the height the biome wants, and each of the three wraps
  // its own existing expression in it verbatim. The three cannot drift, because there is only one expression.
  // ── THE BIRCH FOREST ── a band of OAK TERRAIN between the pine forest and the desert. There is no birchH
  // and H is not touched at all: oakRoll and oakBank are the two shared scalar helpers all three copies of H
  // call, so widening THEM to cover this band gives the birch forest oak ground without the three-copy edit
  // that the cherry forest's note calls the one edit in this file that cannot be made safely by inspection.
  //
  // IT TAKES THE DESERT'S OLD SLOT, exactly. BIRCHC/BIRCHH are the numbers DESC/DESH had before the sand moved
  // west, so the strip lands where a strip already fitted and nothing else has to be re-derived.
  //
  // AND IT SHARES THE DESERT'S MEANDER, which is not laziness. The long note under OAKOFF works out that two
  // INDEPENDENT wobbles can converge until two biomes touch with no pine between them; the existing masks
  // defend against it by part-sharing (oakWob = 0.6 * desWob + its own octave). Using desWob RAW here makes the
  // birch/desert seam exactly parallel - a constant 2160, convergence impossible - and leaves the birch/oak
  // seam with the same guarantee the desert/oak seam already had, since |desWob - oakWob| = |0.4*desWob - own|
  // <= 270 + 270 = 540 against 2160 of nominal gap less 450 of blend, i.e. >= 1170 of pure pine worst case.
  // ── HOW FAR ABOVE THE GROUND ANYTHING CAN REACH ── and it is not a hint. rebuildBricks force-CLEARS every
  // brick row above `hmap + CANOPY`; a voxel above it gets no brick bit, the DDA reads an unset brick as air,
  // and the voxel is INVISIBLE WHILE STILL SOLID. The symptom is unmistakable and cost five rounds the first
  // time: "the tops of the trees are cut off, but I can walk on the canopy". Everything that reads W - the
  // audits, collision, chopping - says the world is perfect, because it IS; only the renderer disagrees.
  // It was a literal 122 in TWO places and 118 in a third, all meaning "the tallest PINE", written when pines
  // were the tallest thing there was. The birches are 168. So it is one named constant now, read by
  // terrain.js rebuildBricks, the worker's copy of it in gen-pool.js (a string - __vb.gtest diffs the two and
  // they must stay identical), and the editor's stage carve.
  // 192 clears the tallest model with 24 to spare, and it is a COST: those rows are scanned per tile.
  // RAISE IT BEFORE BAKING ANYTHING TALLER.
  const CANOPY = 240;   // 192 -> 240 when the arctic glaciers went to 176 voxels over a seabed 42 below the waterline (see ARCT_FLOEH): 218 of reach above hmap, and this is the ceiling that decides whether a voxel is drawn at all.
  // ══ THE ARCTIC (user 2026-08-29) ══ a seventh strip, BETWEEN the spawn pine and the birch forest.
  // Placed on the WEST side of the spawn pine rather than the east for one reason: spawn sits in that pine
  // strip, and every spawn guarantee in this file (the view down the strip, the distance to the treeline,
  // SPYAW facing the oak) is written against it. Inserting inboard of spawn would have moved the player onto
  // an ice sheet on the first frame. So the arctic takes the slot the birch held and the birch, the desert and
  // the wrap all slide one strip west - which is exactly what the birch itself did to the desert in 2026-08-24.
  // It borders PINE on its east and BIRCH on its west, which is where the user asked for it.
  // The meander is desWob, SHARED with the birch it touches: two adjacent bands on a shared wobble keep a
  // parallel seam, and the long note under OAKOFF is about what happens when two independent meanders are
  // free to converge on each other.
  // ARCTB is DOUBLE the 450 every other band uses (user 2026-08-29: "make the transition … smoother. it
  // currently looks like a straight snow line going across"). The note under DESB argues a blend is a TREELINE
  // and should not be widened — and that is right for two forests, which meet as canopy against canopy. Snow
  // against forest is a GROUND change, and a ground change reads as a drawn line at any width the eye can take
  // in at once. 900 voxels is 90 m of thinning, which is about the distance the fog starts softening anyway.
  const ARCTOFF = 2160, ARCTB = 1800, ARCTH = 2160;     // inner edge from spawn; blend width; half-width to the mask midpoint
  const ARCTC = BAND_MIRROR * (ARCTOFF + ARCTH);       // -2160: the band centre, mirrored like DESC/BIRCHC/OAKC/CHOFF
  const arcticM = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
  // ══ WHERE THE SNOW LIES, WHICH IS NOT WHERE THE BIOME IS ══════════════════════════════════════════════
  // (user 2026-08-30: "make the edge of the arctic meet up much closer to the pine forest. but then also make
  // the transition much smoother instead of a straight line. dont do anything to the tree placements.")
  // Those three asks pull against each other on ONE mask, and that is why there are now two.
  // arcticM keeps its job: the terrain blend and, through ARCT_BARE, which columns refuse to plant. It is
  // untouched, so every tree in the world stands exactly where it stood — which is the third ask, and it is
  // the reason none of this widens the band or moves its centre.
  // The SNOW moves instead. It used to be dithered straight against arcticM, so at the treeline (am 0.15)
  // only about 15% of columns were white: trees stopped, and then there was a long stretch of bare pine-forest
  // floor before the snow properly began. That gap is what read as the arctic not meeting the pine forest.
  // This ramp is full by am 0.16 — just past where planting stops — so the white now runs right up to the last
  // trees, and it falls to nothing by am 0. Same band, same trees, snow much further out.
  // ── AND THE EDGE IS NOISE, NOT A LINE ── the meander arcticM inherits (desWob) is a function of z ALONE, so
  // the boundary is one smooth curve running north-south: at walking scale that is a straight line, which is
  // exactly what was reported. The two octaves below are functions of x AND z, and they are added to the mask
  // BEFORE the ramp rather than to the position, so the iso-line wanders in both axes at once — snow reaches
  // into the forest in fingers and the odd bare patch survives inside the white. ±0.135 against a 0.16 ramp is
  // deliberately most of its width: anything less and the ramp still reads as a band with a wiggle on it.
  const ARCT_SNOW0 = 0.50;                             // mask level at which snow BEGINS — the band's own edge
  const ARCT_SNOWR = 0.30;                             // …and the mask width it takes to reach full white
  // ── THE RAMP NOW STARTS AT THE BORDER INSTEAD OF AT MASK ZERO (user 2026-08-31: "remove the snow from the
  // pine forest. I mean the snow caused by the arctic … remove the arctic mountains that are near the
  // transition of the pine forest. let them be further out") ── it used to be (am + n) / 0.16 with no floor,
  // so snow began wherever arcticM was non-zero at all. arcticM fades over ARCTB = 1800 and is non-zero 900
  // voxels OUTSIDE the band, and 0.16 saturates almost immediately, which put FULL white about 470 voxels
  // into whichever forest was next door. The birch got its own guard earlier today; this is the general
  // form of the same fault, and it fixes the pine side with it.
  // arcticM is 0.5 exactly ON the nominal edge, so ARCT_SNOW0 = 0.50 makes the border the place snow starts
  // rather than the place it has already finished. The ±0.09 of noise below still wanders that line ~160
  // voxels in both axes, which is what keeps it from reading as a drawn curve.
  // AND IT MOVES THE GLACIERS WITH IT, which is the second half of the request: arctGB gates the bergs on
  // asn > 0.5, so pushing the snow ramp inward pushes every mountain that rides on it inward too. They now
  // begin around mask 0.65 - well inside the band - instead of at its outer fringe, so what the player
  // meets at the transition is forest, then water, then flat snow, and the ice only rises further in.
  const ARCT_SNOWN = 0.18;                             // peak-to-peak noise added to the mask, in mask units
  // ══ AND THE GLACIERS FADE OUT WITH IT, RATHER THAN STOPPING ON ITS GATE ══ the ice block in fillColumn is
  // entered on `asn > 0.5`, a hard test, so a mountain 176 voxels tall stood at FULL height right up to the
  // column where the snow mask crossed a half and then simply was not there. The biome's ground fades across
  // the whole rim while its largest feature ended on a line, which is the "cut off looking" edge (user
  // 2026-08-30: "smooth out the transitions between the biomes … blend the glaciers in nicely").
  // Same answer as the dome roughness in arctCliff: multiply, do not cut. The envelope is scaled by how far
  // PAST the gate the mask has got, so bergs come in low and thicken inland instead of appearing full-size.
  // Smoothstepped so the ramp has no visible start or end of its own.
  // It scales the KEEL too, in the cap block — the underside is the same solid seen through the water, and a
  // full-depth keel under a tenth-height crown is the same discontinuity moved below the surface.
  // ── HOW HIGH LAND MAY STAND IN THE ARCTIC ── the ceiling in oakBank is WL + (1 - am) * this, so ordinary
  // ground keeps its full relief at the band's edge and nothing can stand proud of the water at its core.
  // 60 is the neighbouring biomes' own scale (oakH lifts land ~42 voxels over the water near a shore), so at
  // am 0 the rule is inert by construction and only starts biting once the arctic mask is genuinely present.
  const ARCT_STAND = 60;
  const ARCT_GBLEND = 0.42;                            // …how much of the mask above the gate a berg takes to reach full height. 0.42 puts the ramp across most of the rim, which is where the eye reads the edge
  const arctGB = (asn) => { const t = (asn - 0.5) / ARCT_GBLEND; return t <= 0 ? 0 : t >= 1 ? 1 : sstep(t); };
  // ── AND IT STOPS AT THE BIRCH (user 2026-08-31: "take the snow out of the birch forest, the snow caused from
  // trying to blend the arctic with the birch forest") ── the two bands SHARE an edge: birch runs to -6480 and
  // the arctic starts there. But arcticM fades over ARCTB = 1800, so it is non-zero 900 voxels INTO the birch,
  // and arctSnow saturates at am 0.16 - which puts FULL snow about 470 voxels inside a forest that should have
  // none. The blend was doing its job; the snow ramp is simply much shorter than the terrain ramp it rides on.
  // Faded on birchM rather than cut at the border, because a hard height or mask test here draws a straight
  // white line - the exact complaint the ARCT_SNOWR note further up was written to answer. Behind the same
  // wrapped-distance cheap-out oakRoll and oakBank use, so the pine forest and the desert pay two compares.
  const arctSnow = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
  // ── SNOW CAPS ON THE WATER (user 2026-08-30: "inside the lakes and rivers can you create snow caps") ──
  // written AT the waterline in place of the surface water voxel, not above it, so a cap is flush with the
  // lake rather than a lip standing proud of it. Coherent, so they come in floes rather than as speckle; the
  // threshold is what sets coverage.
  const ARCT_FLOE = 0.55;                              // …and the noise threshold a column must clear to carry one
  // ── A RIVER CARRIES TWICE THE ICE OF A LAKE (user 2026-08-30: "make the rivers half as sparse with snow
  // caps") ── the same floe field, read at a lower threshold on any column the river carve touched, so a
  // channel ices over while the lakes keep the coverage they have. Half as SPARSE, not twice as covered:
  // it is the gaps that halve, which is what the phrase asks for and what stops a river simply becoming a
  // solid white strip.
  const ARCT_FLOE_RIV = 0.13;                          // how much lower that threshold sits on a river column
  // DOUBLED AGAIN 2026-08-30 (user: "double the size of the big snow glaciers ... this should make them look
  // more like mountains"). 17.6 m of ice standing out of the sea, on a footprint doubled with it - the
  // crevasse and fracture wavelengths below are halved in the same pass, or a mountain would wear a hill's
  // texture and read as a scale model of one.
  // THIS IS WHAT FORCED CANOPY UP. rebuildBricks force-clears every brick row above hmap + CANOPY, and a
  // voxel above that line gets no brick bit, so the DDA reads it as air and it is INVISIBLE WHILE STILL
  // SOLID - the "tops of the trees are cut off but I can walk on the canopy" failure the CANOPY note records.
  // The arctic seabed sits as deep as WL - 42, so a 176-voxel glacier reaches 218 above its own hmap and 192
  // was no longer enough.
  const ARCT_FLOEH = 176;                                // extra voxels of thickness at a floe's thickest, over the 1 it always has. 3 -> 8 -> 14 -> 28 (user 2026-08-30, three times: "give even more depth to the polar ice caps. they are still flat", then "make them taller and round") — at 4 the caps still read as a sheet from standing height, because 40 cm of ice is under a knee. 9 voxels is most of a metre and throws a shadow you can see across the lake.
  // ── AND A STEP, NOT A SPAN ── the first cut spread the thickness over the WHOLE remaining range of the
  // noise (fl - ARCT_FLOE) / (1 - ARCT_FLOE), which is the obvious normalisation and gave 70% of floes a
  // single voxel: fbm clusters hard just above any threshold, so almost nothing reached the upper part of
  // that range and the caps still read as flat. Measured off the thickness histogram instead — 0.035 puts
  // its steps near the quartiles of fl among the columns that actually carry a floe, so all four
  // thicknesses get used.
  // ── AND THE PROFILE IS A DOME, NOT A ZIGGURAT (user 2026-08-30: "make them taller and round") ── height was
  // a LINEAR step: one more voxel per fixed increment of noise, which stacks a floe as concentric plateaus and
  // reads as a terraced cake. Taking the SQUARE ROOT of the normalised noise instead makes the cap climb fast
  // at its rim and flatten across its top, which is the profile of a dome and is what rounds it. Same field,
  // same edge, no extra sampling — only the mapping from noise to height changed.
  // A SPAN, not a step, because sqrt needs the whole range normalised to 0..1 before it means anything.
  // ── AND THE SPAN WIDENS WITH THE HEIGHT, OR THE DOME GROWS A FLAT TOP ── a quarter of cap columns were
  // SATURATING at full height, and a saturated column is by definition part of a plateau: the profile stops
  // being a curve exactly where the eye looks hardest, at the middle. Widening the span keeps most of the
  // field below the ceiling so the crown stays curved (user 2026-08-30: "make sure they are even rounder").
  const ARCT_FLOESPAN = 0.30;                          // noise above the threshold at which a cap reaches full height
  // ══ THE GLACIER PROFILE (user 2026-08-30: "round out the bottoms of the glaciers", "also round out the tops
  // as well") ══ one curve, used for the crown above the water and for the KEEL below it, so a berg is one
  // round solid rather than a dome sitting on a flat disc.
  //
  // WHAT WAS WRONG WITH THE OLD ONE. It was `sqrt(t * (2 - t))`, the exact profile of a HEMISPHERE — a sphere
  // cut at its equator. That is geometrically round and it fails at both ends the user is pointing at, for the
  // same reason: at the rim (t = 0) a hemisphere's surface is VERTICAL, so the ice came out of the water as a
  // wall; and it is already at 0.87 of full height by t = 0.5 and 0.99 by t = 0.85, so most of a cap's area sat
  // within half a voxel of its own ceiling and quantised into one flat disc. A vertical bottom and a flat top.
  //
  // WHAT THIS IS. A spherical cap of a sphere whose CENTRE IS BELOW THE WATER — the water plane cuts it above
  // the equator, not through it. Radius 1, centre ARCT_DOMEC under the surface, so the footprint radius is
  // sqrt(1 - c^2) and the peak stands (1 - c) proud. The surface then meets the water at a finite angle
  // (rounded bottom) and climbs most of its height in the inner half of the cap (rounded top): 0.28 at t = 0.2
  // and 0.88 at t = 0.8, against the hemisphere's 0.60 and 0.98. Same field, same edge, no extra sampling —
  // only the mapping from noise to height changed, which is the third time that has been the answer here.
  const ARCT_DOMEC = 0.6;                              // how far under the water the sphere's centre sits, in radii. 0 would be the old hemisphere; higher is rounder and lower
  const ARCT_DOMEK = 1 - ARCT_DOMEC * ARCT_DOMEC;      // = the footprint radius squared, precomputed — the profile needs it every column
  const arctDome = (t) => { const r = 1 - t; return (Math.sqrt(Math.max(0, 1 - ARCT_DOMEK * r * r)) - ARCT_DOMEC) / (1 - ARCT_DOMEC); };
  // == AND THEY ARE GLACIERS NOW, NOT DOMES (user 2026-08-30: "double the size of the ice caps. instead of
  // roundness. make them jagged glaciers. cliffs.") == arctDome above still supplies the ENVELOPE - how much
  // ice this column is entitled to, from nothing at the rim to full height at the middle - because the field
  // that decides where a floe IS has to keep deciding where it ends. Everything between the envelope and the
  // voxel changes:
  //   * FRACTURE. A coherent short-wavelength term is added to the height BEFORE the terracing. On its own
  //     that would only be a bumpy dome; what it actually does is move where each terrace edge falls, so the
  //     cliff lines wander instead of following the envelope's iso-contours as concentric rings.
  //   * TERRACES. The result is snapped to whole ARCT_STEP blocks. A quantised height field IS a cliff: every
  //     step is a vertical face ARCT_STEP voxels tall with a flat bench on top, which is what a calving
  //     glacier front looks like and is the exact opposite of the smooth spherical cap it replaces.
  // The two together are what separates this from the terracing the arctic GROUND was criticised for: there
  // the steps were one voxel and unavoidable, here they are nine and deliberate.
  // DOUBLE SIZE is both axes: ARCT_FLOEH 44 -> 88 for the height, and ARCT_FLOEF halved (0.0105 -> 0.00525)
  // for the footprint. Coverage is set by the threshold, not the frequency, so it barely moves.
  // ══ SERACS, NOT STAIRS (user 2026-08-30: "you made the glaciers have steps instead of jagged edges. try
  // again. look to the internet on the appearance of jagged glaciers") ══
  //
  // I LOOKED IT UP, AND I HAD THE MECHANISM BACKWARDS. A serac - the ice pinnacle that makes a glacier look
  // jagged - is not a step in a slope. It is what is LEFT OVER where two crevasses cross: the block of ice
  // between the intersecting cracks is partly cut free of the mass and stands as a tower or a pillar, "an
  // impenetrable system of pillars and pinnacles", several metres to over thirty tall. The jaggedness of an
  // icefall is CARVED IN by vertical cracks, it is not built up as horizontal benches.
  // So the previous version could not have worked however it was tuned. Quantising a smooth dome to 9-voxel
  // benches produces exactly one thing - terraces - and terraces are the shape of a quarry or a rice paddy,
  // where every edge is horizontal and every face is the same height. Real serac fields have almost no
  // horizontal lines in them at all.
  //
  // WHAT THIS DOES INSTEAD. The envelope (arctDome + the two fracture octaves) still says how much ice a
  // column is entitled to. Then CREVASSES are subtracted from it:
  //   * RIDGED NOISE puts a crack where a noise field crosses its own midline. arctRidge(v) peaks at 1 exactly
  //     on the v = 0.5 contour and falls away either side, so thresholding it high gives a NARROW, winding,
  //     continuous line - which is what a crevasse is, and what no amount of ordinary fbm will give you.
  //   * TWO FAMILIES at different scales, so they cross. That is the whole point and it is straight out of the
  //     geology: where two crevasses intersect, the ice between them is isolated, and THAT is the tower.
  //     One family alone would only groove the glacier into parallel ribs.
  //   * THE CUT IS DEEP AND MOSTLY PROPORTIONAL, so a crevasse through tall ice is a canyon and one through
  //     thin ice cuts clean through to the water - which leaves open leads between the towers, exactly as a
  //     real icefall calves into a fjord. arctCliff returning 0 is allowed and means "no ice in this column".
  // And the terracing is GONE. ARCT_STEP is 1: the only quantisation left is the voxel grid itself, and on a
  // near-vertical serac wall its contours are one voxel apart, which reads as texture rather than as stairs.
  const ARCT_STEP = 1;                                 // no terracing - the voxel grid is the only quantisation, and the crevasses do the shaping
  const ARCT_JAG = 30;                                 // peak-to-peak FINE fracture, in voxels - roughens the ice surface between cracks
  const ARCT_JAGF = 0.0275;                             // its wavelength, ~18 voxels
  const ARCT_JAG2 = 68;                                // peak-to-peak BLOCK fracture, so whole sectors of one glacier stand at different heights
  const ARCT_JAGF2 = 0.007;                            // its wavelength, ~70 voxels
  const ARCT_CREVF1 = 0.0095;                           // crevasse family ONE - lines about 53 voxels apart
  const ARCT_CREVF2 = 0.0135;                           // ...and family TWO at a different scale, so the two cross instead of running parallel
  const ARCT_CREVT = 0.90;                             // how close to the midline counts as crack: higher = narrower slots and bigger towers
  const ARCT_CREVW = 0.10;                             // the ramp from rim to full depth - keeps a crevasse wall steep without making it a single hard column
  const ARCT_CREVD = 28;                               // fixed part of the cut, in voxels - what carves thin ice clean through to the water
  const ARCT_CREVK = 0.78;                             // ...and the part proportional to the local height, which is what makes a canyon out of tall ice
  const arctRidge = (v) => 1 - Math.abs(v + v - 1);    // 1 exactly on the field's midline, 0 at its extremes - a CRACK, not a blob
  const arctCliff = (t, x, z, g) => {                  // the glacier's height above the water at this column. g = the biome blend (arctGB), 0 at the snow mask's own gate and 1 well inside
    // ══ THE ROUGHNESS IS SCALED BY THE DOME IT ROUGHENS ══ the two jag octaves used to be ADDED to the
    // envelope independently of it, and a term that does not vanish where the envelope does is not roughness —
    // it is geometry of its own. At the floe's rim arctDome goes to 0 while the jag is still worth up to +49,
    // so ice stood in open water with nothing beneath it: isolated white pillars, a few voxels thick and dozens
    // tall, out on the sea and along the treeline. Measured before the fix, flying the arctic: 60 columns
    // standing 18-78 voxels above everything five voxels away, the tallest reaching y=329 — against a design
    // ceiling of WL + ARCT_FLOEH. They read as rendering errors, which is exactly what they were reported as.
    // Multiplying instead of adding ties the crest's variation to the thickness of the ice under it: full
    // roughness where the berg is thick, none at all where it has run out, and no column can outlive its dome.
    const d0 = arctDome(t) * (g === undefined ? 1 : g);
    const env = d0 * (ARCT_FLOEH
      + (fbm(x * ARCT_JAGF + 313.7, z * ARCT_JAGF + 77.1) - 0.5) * ARCT_JAG
      + (fbm(x * ARCT_JAGF2 + 51.9, z * ARCT_JAGF2 + 143.3) - 0.5) * ARCT_JAG2);
    if (env <= 0) return 0;
    const cr = Math.max(arctRidge(fbm(x * ARCT_CREVF1 + 19.3, z * ARCT_CREVF1 + 202.7)),
                        arctRidge(fbm(x * ARCT_CREVF2 + 401.1, z * ARCT_CREVF2 + 8.9)));
    let h = env;
    if (cr > ARCT_CREVT) h -= (ARCT_CREVD + ARCT_CREVK * env) * Math.min(1, (cr - ARCT_CREVT) / ARCT_CREVW);
    return Math.max(0, Math.round(h / ARCT_STEP) * ARCT_STEP);
  };
  // ── AND THE BOTTOM IS A REAL KEEL, NOT A CUT ── a cap used to start at the waterline and stack upward, so
  // its underside was a flat disc floating at WL with open water beneath it. Now that the arctic is all sea
  // (see arctH) that underside is the part you look straight through the surface AT, so it gets the same dome
  // mirrored downward. 0.40 rather than an iceberg's real 7/8: the bed is only ~12 voxels down, and a keel any
  // deeper than this simply grounds into it on every cap big enough to matter, which hides the very curve the
  // request is about. Big bergs still ground, and that reads correctly as shelf ice sitting on the shallows.
  // 0.40 -> 0.22 WHEN THE GLACIERS DOUBLED. The keel is a fraction of the CROWN, so doubling ARCT_FLOEH to 88
  // took it to 35 voxels — deeper than most of the sea — and every glacier grounded on the bed, which buries
  // the one part of them the deeper water was supposed to make visible. 0.22 is ~19 voxels: the big ones still
  // ground in the shallows, and in the 19-30 range they float clear and you can see the mass under them.
  const ARCT_KEELK = 0.22;                             // keel depth as a fraction of the crown's height
  // == FLAT ICE PATCHES (user 2026-08-30: "add ice patches into the water. not caps, but flat ice patches.
  // make it 25% of the total surface area of the water") == ONE voxel, written at WL in place of the surface
  // water, on its OWN noise field - so an ice patch is a sheet flush with the sea and a snow cap is a glacier
  // standing out of it, and neither can be mistaken for the other at any distance.
  // ITS OWN FIELD, not a second threshold on the floe field: keyed to the floes, the ice would only ever
  // appear as a fringe around the glaciers, which is a rim rather than a scatter across the water. The
  // frequency sits between the floes' and the bed's, so a sheet is bigger than a wave and smaller than a bay.
  // THE THRESHOLD IS MEASURED, NOT DERIVED. fbm is not uniform - it clusters hard around 0.5 - so the value
  // that yields 25% coverage cannot be read off the number 0.75. This one was tuned against a census of the
  // actual arctic water surface, which is also the only way to state the coverage honestly.
  // ── PENGUIN SCATTER ── how often a cell rolls one, and how deep into the band they start. PENG_INNER is
  // well past ARCT_BARE (0.72, the last tree): the rim is snowy forest and a penguin under a pine reads as a
  // bug. Past 0.8 the surface is sea, ice sheet or glacier, which is the only place one belongs.
  // ── AND THEY STAND IN COLONIES, NOT ALONE (user 2026-08-30: "make penguins huddle together vs just
  // scattered everywhere. have different groups of penguins") ── one roll per cell used to mean one bird per
  // cell, evenly spread, which is the one thing penguins never do. The cell is much bigger now and a hit
  // places a whole HUDDLE: the cell decides where a colony is, the colony decides how many birds and how
  // tightly they stand, and the ice test then thins it wherever the floe runs out from under it.
  // THE SPREAD OF SIZES IS THE POINT of "different groups" - a colony rolls anywhere from a handful to a
  // couple of dozen, so the biome has pairs, family groups and proper rookeries in it rather than one
  // repeated clump.
  const PENG_RATE = 0.7;                               // fraction of PENGCELL cells that hold a colony
  const PENG_INNER = 0.8;                              // arctic mask a column must clear before a penguin will stand there
  // ── AND THEY STAND ON THE GLACIERS (user 2026-08-30: "put the penguins ontop of the glaciers, not the
  // ice") ── PENG_MINTOP is what separates a glacier bench from a snow cap: the caps are at most ARCT_CAPH + 1
  // voxels proud, so anything standing higher than that is ice the crevasse carve left behind.
  // THE HUDDLE HAD TO TIGHTEN TO FIT ONE. A bench between two crevasses is 20-30 voxels across, which is
  // narrower than the colonies were laid out for on open sheets — spread wider, most of a colony fell into a
  // cleft and was thinned away one bird at a time, which is the same failure that moved them off the glaciers
  // in the first place. Smaller colonies, packed tighter, is what makes them fit.
  const PENG_MINTOP = 18;                              // voxels above the waterline a column must stand for a colony to site on it — clear of ARCT_CAPMAX, so a disc is never mistaken for a glacier bench
  const PENG_MIN = 3, PENG_MAX = 16;                   // birds per colony, before the ice test thins it
  const PENG_SPACE = 2.9;                              // huddle tightness: the disc radius grows as PENG_SPACE * sqrt(n), so density is constant and only the SIZE varies. Tight enough that a colony fits on one floe rather than being thinned in half by the water around it
  const PENG_JIT = 2.2;                                // voxels of scatter on each bird, so a huddle is not a visible lattice
  // ── AND SOME OF THEM HAVE A CHICK (user 2026-08-30) ── rolled PER ADULT rather than as a share of the
  // colony, so a chick always has a parent standing next to it, which is the whole picture the ask is about.
  // Not every adult: a rookery where every single bird has a chick reads as a pattern rather than a colony.
  // ══ WHAT THE SURFACE OF AN ARCTIC COLUMN IS, WITHOUT LOOKING AT IT ══ the y a bird stands at here, or -1
  // for open water. It re-derives exactly what the cap/ice block in terrain.js writes, from the same three
  // fields, and that is the point: the first version READ W instead, and reading W from a decoration stamp is
  // a determinism bug even when it looks like it works.
  // WHY. stampCellsGen visits a cell from every band whose margin reaches it, so a colony straddling a band
  // boundary is stamped from both. A member's column is fully generated in the band that CONTAINS it and is
  // whatever the toroidal window last held in the other — so the same bird was placed by one band and refused
  // by the other, depending on which order the world happened to stream in. It also cost most of the
  // population: measured 25 penguin probes across 1800x1800 where the roll rate asks for roughly a hundred
  // and fifty birds. Every scatter in this file that needs to know about the ground asks H(); this asks the
  // three fields that decide the ice, for the same reason and with the same guarantee.
  const arctIceTop = (x, z) => {
    const asn = arctSnow(x, z);
    if (asn <= 0.5) return -1;                         // the snow mask gates the caps and the sheets alike (see the cap block in terrain.js)
    if (H(x, z) > WL - 1) return -1;                   // not a lake column at all - the rim, where a penguin has no business
    const fl = fbm(x * ARCT_FLOEF + 71.3, z * ARCT_FLOEF + 12.9);
    const flT = ARCT_FLOE - (riverS(x, z) > 0.02 ? ARCT_FLOE_RIV : 0);
    if (fl > flT) {                                    // a glacier stands here IF the crevasses left any of it
      const c = arctCliff(Math.min(1, (fl - flT) / ARCT_FLOESPAN), x, z, arctGB(asn));   // the SAME blend the stamp uses — this function exists to agree with it exactly
      if (c > 0) return WL + c;                        // the cap fills up to WL + capH - 1, so its top face is at WL + c - 1 and a bird stands one above that
    }
    // ── AND THE SMALL SNOW CAPS, WHOSE HEIGHT HAS TO BE MIRRORED EXACTLY ── this is the same arithmetic the
    // cap stamp in terrain.js runs, for the reason the whole function exists: a bird standing on one has to
    // stand on its CROWN, and the crown is not at a fixed height any more.
    const ic = fbm(x * ARCT_ICEF + 137.1, z * ARCT_ICEF + 211.7)
             + (fbm(x * ARCT_CAPIRRF + 71.9, z * ARCT_CAPIRRF + 133.7) - 0.5) * ARCT_CAPIRR
             + (fbm(x * ARCT_CAPIRRF2 + 24.3, z * ARCT_CAPIRRF2 + 96.1) - 0.5) * ARCT_CAPIRR2;
    if (ic > ARCT_ICE) return WL + ARCT_CAPMIN + Math.round((ARCT_CAPMAX - ARCT_CAPMIN) * fbm(x * ARCT_CAPHF + 401.3, z * ARCT_CAPHF + 55.1));
    return -1;                                         // open water
  };
  const PENG_CHICK = 0.45;                             // chance an adult has a chick beside it
  const PENG_CHICKR = 4;                               // how far to its side the chick stands, in voxels - just clear of the parent's own footprint
  // == SMALL SNOW CAPS, SPRINKLED (user 2026-08-30: "remove the ice from the water in the arctic. create
  // small snow caps sprinkled in the water. make it take up 25% of the surface area of the water") ==
  // This replaces the FLAT ICE SHEETS that were here, and with them goes the whole AICE palette ramp and the
  // ICEF face-id path in the shaders: a sheet needed its own colour and its own ice shading to be seen at all
  // against white glaciers, and a MOUND does not - it has a silhouette. Three palette ids came back, which on
  // a table that was sitting at exactly 256/256 is worth having.
  // SMALL, and that is the whole difference from a glacier: ~22-voxel wavelength against the floe field's
  // ~190, and a handful of voxels of height against 88. They read as bergy bits between the bergs.
  // SAME FIELD AS THE SHEETS USED, at three times the frequency: it is independent of the floe field on
  // purpose, because keyed to it the caps would only ever ring the glaciers rather than scatter across open
  // water. The threshold is MEASURED against a census of the actual water surface, not derived - fbm clusters
  // hard around its middle, so the value giving 25% cannot be read off the number 0.75.
  // DOUBLED IN SIZE, HALVED IN COVERAGE (user 2026-08-30: "double the size of the small snow caps but reduce
  // the surface area by 50%"). Both axes double — a ~44-voxel disc 8-14 thick keeps the same 1:4 proportion
  // that made it read as a disc at 22 and 4-7, so it is the same object at twice the scale rather than a
  // different, chunkier one. The two supporting fields double their wavelength with it (ARCT_CAPHF for the
  // per-cap thickness, ARCT_CAPIRRF for the frayed rim), or a bigger disc would wear a smaller disc's edge.
  // LARGE CAPS ONLY (user 2026-08-30: "remove all the tiny ones and keep the larger ones ... I want large
  // snow caps"). The wavelength halves AGAIN, to ~90 voxels, so what the field produces is a handful of broad
  // floes rather than a scatter of lumps; the threshold then keeps only the cores of those, which is what
  // "remove the tiny ones" asks for — a higher cut on fbm removes small blobs entirely before it shrinks big
  // ones, because a small blob is one that barely cleared the old cut anywhere.
  // ── DOUBLED AGAIN, AND THE SMALL ONES CULLED (user 2026-08-30: "remove the smaller snow caps. double the
  // size of the caps") ── size is the FIELD'S WAVELENGTH, so doubling a cap means halving the frequency: the
  // blobs come out twice as wide for four times the area. On its own that would keep the same coverage and
  // simply scale everything, including the marginal fragments around each blob's rim, so the threshold goes up
  // with it — a blob whose peak no longer clears it disappears entirely, which is what culls the small ones
  // rather than shrinking them.
  const ARCT_ICEF = 0.00775;                           // the snow-cap field's wavelength, ~180 voxels — twice the previous 90
  // ── HALF AS MANY, AND FLAT (user 2026-08-30: "decrease the snowcaps by 50%. also make them more flat by
  // design. make them 4-7 voxels tall. like snow discs, but make it irregular, we dont want a perfect disc") ──
  // 0.612 measured 27.7% of the water surface; this is the threshold for half of that.
  // FLAT MEANS A CONSTANT HEIGHT ACROSS ONE CAP, not a dome flattened. The height comes from a LOW-frequency
  // field, so it barely changes across a single ~22-voxel cap but differs between caps: every disc is level,
  // and no two are the same thickness. A per-column roll would have given a lumpy crust instead.
  // AND THE EDGE IS RAGGED because a threshold on smooth fbm is a smooth curve — which at this size reads as
  // a stamped circle. A short-wavelength term added to the FIELD (not to the height) moves the iso-line in and
  // out by a few voxels, so the rim breaks up while the top stays flat.
  const ARCT_ICE = 0.745;                              // …raised with the wavelength: it is what removes a blob outright instead of making every blob smaller
  const ARCT_CAPMIN = 3, ARCT_CAPMAX = 4;              // a disc's thickness in voxels, inclusive — thin sheets now, so a big one still reads as floating ice rather than as a plateau
  const ARCT_CAPHF = 0.00275;                          // the height field's wavelength, ~360 voxels — doubled with the caps, so it is still several of them wide and one cap is still level
  // ── AND THE EDGE IS PROPERLY TORN, NOT WAVY (user: "the current edges are straight, make edges jagged") ──
  // one gentle octave only bent the outline; a big cap needs the outline broken at more than one scale or it
  // still reads as a stamped shape with a wobble. The COARSE term (~40 voxels) cuts bays and headlands into
  // the floe, the FINE one (~7 voxels) chews the resulting edge into single-voxel teeth. Both are added to
  // the FIELD rather than to the height, which is what moves the iso-line instead of lumping the top.
  const ARCT_CAPIRR = 0.16;                            // coarse displacement of the threshold — bays and headlands
  const ARCT_CAPIRRF = 0.013;                          // …at ~80 voxels, doubled with the caps: bays and headlands in proportion to the disc, not a smaller disc's edge on a bigger one
  const ARCT_CAPIRR2 = 0.075;                          // …and a fine one for the teeth
  const ARCT_CAPIRRF2 = 0.145;                         // …at ~7 voxels                              // ...and the threshold a column must clear. TUNED AGAINST A CENSUS, not derived: 0.545 measured 39.3% of the water surface, because fbm clusters hard around its middle and the value giving 25% is nowhere near the 0.75 the fraction suggests
  // ── FLOE SIZE ── the wavelength of the field that decides where a cap is, so it sets how BIG one is. Halved
  // (0.021 -> 0.0105) to double each floe's footprint on request; coverage is set by the threshold above and is
  // very nearly unchanged by this, which is what makes size and coverage separable levers.
  const ARCT_FLOEF = 0.002625;
  const ARCTWMAX = DESW * 0.675;                       // the band's absolute reach, for the same cheap-out shape birch uses
  const ARCTFAR = ARCTC + ARCTH + 2 * ARCTWMAX + ARCTB * 0.5;    // no column east of this can be arctic at all
  const ARCTWFAR = ARCTC - ARCTH - 2 * ARCTWMAX - ARCTB * 0.5;   // …nor west of this
  const BIRCHOFF = 6480, BIRCHB = 900, BIRCHH = 2160;   // inner edge east of spawn; blend width; half-width to the mask midpoint   // 1080 -> 3240 (2026-08-29): one strip further out, because the ARCTIC now occupies the strip the birch used to. Its centre BIRCHC follows automatically, and so do BIRCHFAR/BIRCHWFAR, so the oakRoll cheap-out moves with it
  const BIRCHC = BAND_MIRROR * (BIRCHOFF + BIRCHH);     // -2160: the band centre, mirrored like DESC/OAKC/CHOFF
  const birchM = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
  // The band's absolute reach, for the cheap-out in oakRoll/oakBank. |desWob| <= DESW * 0.675, and the rim is
  // BIRCHB/2 either side of the line. NOTE there is deliberately no "deep birch" short-circuit to match oak's
  // OAKNEAR/OAKWNEAR: 2 * BIRCHWMAX + BIRCHB/2 = 1575 is WIDER than BIRCHH = 1080, so no column is guaranteed
  // to be mask-exactly-1 and such a test could never fire. Only the outside-the-band cheap-out is real.
  const BIRCHWMAX = DESW * 0.675;
  const BIRCHFAR = BIRCHC + BIRCHH + 2 * BIRCHWMAX + BIRCHB * 0.5;    // no column east of this can be birch at all
  const BIRCHWFAR = BIRCHC - BIRCHH - 2 * BIRCHWMAX - BIRCHB * 0.5;   // …nor west of this
  const oakRoll = (h, x, z) => {
    const dx = pwrap(x - SPWX);                        // WRAPPED, like the mask it is short-circuiting for: on a raw distance these two tests answer for the first period only and every later oak forest would come out as pine
    // ── THE BIRCH AND THE ARCTIC COMPOSE; THEY DO NOT SHORT-CIRCUIT EACH OTHER ──
    // This used to be two arms that each evaluated a mask and RETURNED, on a stated assumption: "the two bands
    // cannot overlap: birch reaches at most BIRCHFAR east and oak at most OAKWFAR west, and a whole pine strip
    // sits between them." That is true of birch and OAK. It was never true of birch and the ARCTIC, which was
    // added later and SHARES AN EDGE with the birch - and the birch arm is checked first, so wherever birchM
    // was still above zero it answered for the column and arcticM was never consulted at all.
    // Measured at the worst case: two columns two voxels apart, arcticM 0.843 and 0.844, baseH 222 against 132.
    // On one side the birch arm returned pine terrain; on the other birchM had reached exactly 0, the column
    // fell through, and the arctic lerp pulled it 90 voxels down onto the sea bed. A vertical wall along the
    // birchM = 0 iso-line, which is the "terrain looks like it is just shifting up" report.
    // Sequential lerps are continuous in BOTH masks: each one vanishes as its own mask goes to zero, so no
    // column can change which rule owns it. The cheap-outs stay - they are what keeps the pine forest and the
    // desert paying two compares - and the deep cases still skip the fields they do not need.
    const bm = (dx < BIRCHFAR && dx > BIRCHWFAR) ? birchM(x, z) : 0;
    const am = (dx < ARCTFAR && dx > ARCTWFAR) ? arcticM(x, z) : 0;
    if (am >= 1) return arctH(x, z);                   // the arctic core is the bed, whatever else claims the column
    if (bm > 0 || am > 0) {
      let hh;
      if (bm >= 1) hh = birchH(x, z);                  // deep birch: the oak field with its own lakes cut into it, and pineH is not needed
      else if (bm > 0) hh = pineH(x, z) * (1 - bm) + birchH(x, z) * bm;
      else hh = pineH(x, z);
      return am > 0 ? hh * (1 - am) + arctH(x, z) * am : hh;
    }
    if (dx >= OAKFAR || dx <= OAKWFAR) return pineH(x, z);   // ── THE PINE FOREST, AND THE DESERT'S BASE ── this used to hand `h` (baseH) straight back, which is what made pine the default terrain. It is a field of its own now. The desert comes through here too and that is deliberate: its own flat runs LAST in H and lerps over this, so deep sand is untouched to the bit and only the rim changes — mountains grading down into dune, which is what a desert edge should look like.
    if (dx <= OAKNEAR && dx >= OAKWNEAR) return oakH(x, z);   // deep oak — mask is exactly 1, and h * 0 + oakH * 1 is oakH
    const om = oakM(x, z);
    return om <= 0 ? pineH(x, z) : pineH(x, z) * (1 - om) + oakH(x, z) * om;   // the rim: the same lerp shape the desert flat uses, over the same 450 voxels, so the two forests meet on a slope rather than on a step
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
  const OAKBANKR = 340;                                // skirt reach in voxels. OAKBANKY/OAKBRISE are GONE: they set the cone's fixed top, and the cone lerps to the column's own height now — see the note at the cone.
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
  const SANDDEEP = 8;                                  // how far UNDER the waterline sand still reaches — see `shore` in world/terrain.js
  const SANDR = 44;                                   // how far a beach looks for open water before it is allowed to be sand — see `shore` in world/terrain.js
  const OAKBEACH = 10, OAKBEACHY = WL + 2;
  // ── OAKBEACHY SITS INSIDE THE SAND, NOT ON TOP OF IT (user 2026-08-31: "I dont want to see dirt/grass in
  // the middle of the sand") ── the cone holds its flat at this height, so this is the level most of a beach
  // ACTUALLY sits at. At WL+4 it was landing one voxel above the solid sand band: measured by height, WL-1
  // through WL+3 come out 95-100% sand (ids 34/35/36) and WL+4 is 60% forest floor (9/10/11), so the cone was
  // parking the widest part of every beach on the one level that is not sand - which is the dirt and grass in
  // the middle of it. WL+2 is inside the band with a level of margin either side.
             // flat shore width in voxels (9.6 m), and its height
  // ── ONE PROFILE, FLATTER, FOR EVERY BIOME (user 2026-08-31: "smooth out the sand banks in the cherry forest
  // as well … actually all banks should be smoothed out") ── this was briefly a pine-only pair of constants and
  // that was the wrong shape for the problem: nothing about a shoreline wants to know which forest it is in,
  // and the cherry report is just the same bank seen from the other side of a border. So the skirt widens for
  // everyone instead - reach 280 -> 460, flat sand 24 -> 60 - and the whole biome branch is gone with it.
  // Mean gradient over the cone is now 82/400 = 0.205 against the old 82/256 = 0.320, a bank a little under
  // two thirds as steep, with two and a half times as much flat sand in front of it.
  // OAKBRISE is UNTOUCHED at 64 and that is deliberate: the note above it explains that the top has to clear
  // HMAX - WL = 81 or the cone stops under the hillside and creases, which is exactly what the pine-only
  // version got wrong by lowering it to 46 in the name of flatness. Length is the knob; height is not.
  // ── AND THE PINE FOREST'S OWN, WIDER AND SHALLOWER (user 2026-08-31: "make the transition from the dirt to
  // sand flatter in the pine forest") ── same cone, two numbers changed: twice the flat sand before it starts
  // to climb, and a top at WL + 46 instead of oak's WL + 86. Those give a mean gradient of 42/232 = 0.18
  // against oak's 82/256 = 0.32, so the apron is a little over half as steep and reaches the same distance.
  // The oak forest keeps its own profile untouched: this was asked for in the pine, and the oak's shore is
  // the one the whole helper was tuned against in the first place.
  // against oak's 0.320 - flatter than what was asked for last time, and with no step at either end.
  const oakBank = (h, x, z) => {                       // ONE shared scalar helper again, for the reason oakRoll is one: three copies of H and no room for drift
    const dx = pwrap(x - SPWX);                        // wrapped, for the reason oakRoll's is
    // ── THE ARCTIC SEABED IS RE-ASSERTED HERE, AFTER THE BASINS ── arctH already put the bed under the water
    // back in baseH, but H's pass order runs the BASIN carve between baseH and this helper, and basinLow reads
    // the arctic explicitly (BASIN_ARCTLIFT) so basins very much do form here. A basin drags a column down
    // toward LIFT - 40, which is sixty voxels under the surface: Beer-Lambert kills the return from that depth
    // completely, and the result would be exactly the flat unlit water the "put the terrain just below the
    // water" request is asking to get rid of, in patches, across the biome.
    // Re-stating the bed is the one place it can be done once and be true in all three copies of H, and it is
    // the same trick oakRoll uses for the band terrain. Everything that runs AFTER this can only LOWER the
    // column (the river carve is a Math.min) or is out of reach (the beach flat needs h >= WL - 5), so the bed
    // this line writes is the bed the world gets.
    // …and it is a FLOOR, not a second lerp. Re-lerping on the same mask that oakRoll already lerped on
    // squares it — the effective weight becomes 1 - (1 - am)^2, which reaches 1 far too early and drags the
    // whole shoreline out into the forest, steepening the one transition this band is judged on. Firing only
    // where the column is BELOW the bed leaves the shore slope exactly as oakRoll drew it and still catches
    // every basin, which is the only thing that can have put a column down there.
    if (dx < ARCTFAR && dx > ARCTWFAR) {
      const am = arcticM(x, z);
      // ══ THE FLOOR STAYS A FLOOR — a second lerp here is a KNOWN, DOCUMENTED FAILURE (the paragraph above
      // names it, and it was re-introduced and reverted on 2026-08-30). oakRoll has ALREADY lerped this column
      // toward the arctic on `am`; lerping again on the same mask squares it to 1 - (1 - am)^2, which reaches 1
      // far too early and marches the shoreline out into the forest. Do not "make the bed authoritative" here.
      // ══ WHAT THE FLOOR CANNOT DO is stop a hill that is ABOVE the bed from standing in open sea, and that is
      // a real defect: rock 66 voxels proud of the water at am 0.84, snow-capped, reading as an iceberg made of
      // ROCK[0] grey. So the second rule is a CEILING ON HEIGHT ABOVE THE WATERLINE, and it is a different thing
      // from a lerp toward the bed in the one way that matters: it only touches ground that is HIGH. The shore
      // slope lives within a few voxels of WL, stays under the cap everywhere, and is left exactly as oakRoll
      // drew it — so the mask is not effectively squared where the transition is actually judged.
      // The excess above the cap is COMPRESSED rather than clipped, or every affected hill would come out with
      // a flat top at exactly the cap — a plateau, which is the artefact this is trying to remove.
      if (am >= 1) return arctSeaH(x, z);              // the core of the band IS the bed
      if (am > 0) {
        const sb = arctSeaH(x, z);
        if (h < sb) return h * (1 - am) + sb * am;     // …the floor, unchanged: basins and anything under the bed
        const cap = WL + (1 - am) * ARCT_STAND;
        if (h > cap) h = Math.max(sb, cap + (h - cap) * (1 - am));   // …and FALL THROUGH, see below
        // ── THIS USED TO `return` AND THAT IS A 54-VOXEL CLIFF (user 2026-08-31: "the terrain … looks like it
        // is just shifting up") ── returning here skips the whole bank skirt below, so `h > cap` was not a
        // height cap at all, it was a switch between two completely different treatments of the column.
        // Measured at the worst case in the birch forest, two voxels apart with EVERY mask constant and no
        // river or basin: H 159 against H 211. Below the cap the bank cone pulled a 213-voxel hill down to
        // 159; one voxel above it, the same hill returned 213 untouched. A vertical wall along the cap's
        // iso-line, and the reason it shows up in the BIRCH forest of all places is that am is 0.016 there -
        // the arctic mask's far tail, 900 voxels outside its own band, where cap sits at 211 and ordinary
        // birch hills cross it. Nothing about that column is arctic except this rule reaching it.
        // Assigning instead of returning makes the cap COMPOSE with the skirt rather than replace it, which
        // is what it was always described as doing: 'a CEILING ON HEIGHT ABOVE THE WATERLINE'. A ceiling is
        // not a branch.
      }
    }
    // THE BIRCH FOREST GETS THE SHALLOW BANKS TOO, because it got the rounded hills that make them necessary:
    // the whole reason this helper exists is that oakH lifts land near water ~42 voxels over WL and the river
    // lerp then reads as a cliff. A band with oak terrain and pine banks would have exactly that cliff.
    // ── AND THE ARCTIC, for exactly the reason stated above (user 2026-08-29: "the terrain raising
    // unnaturally") ── arctH is built on oakH's field, so arctic land near water stands the same ~42 voxels
    // over WL, and the river lerp turns that into the same cliff — a grey stone wall along the bank, which is
    // what the screenshot shows. It had oak terrain and pine banks, the exact combination this note warns
    // about. Adding it here is one term, and it is the same term the birch needed for the same reason.
    // …and the ARCTIC arm that used to be the second half of this expression is GONE, because the early return
    // above answers for every arctic column — which is true now that it returns unconditionally, and was NOT
    // true while it was guarded by `h < sb`: a column with land above the bed fell straight through to here. The bank cone exists to stop land standing 42 voxels over
    // the water and dropping to it as a cliff; the arctic has no land left to do that with.
    // ── AND THE PINE FOREST GETS THEM TOO (user 2026-08-31: "can you flatten the sand that leads to the
    // water in the pine forest. similar to how the oak forest does it") ── it used to back out here with the
    // desert, and that was right while pine WAS baseH: baseH's own shoreK term already fades its fine detail
    // out near the waterline, so pine met water gently and needed no skirt. pineH does not have that term and
    // is 118 voxels tall, so pine now meets water exactly the way oak did before this helper existed - as a
    // cliff. The two asks are the same ask: the biome that got oak's hills needs oak's banks, which is the
    // note the birch and the arctic arms below are both already written against.
    const bm = (dx < BIRCHFAR && dx > BIRCHWFAR) ? birchM(x, z) : 0;
    const pine = bm <= 0 && (dx >= OAKFAR || dx <= OAKWFAR);   // …no named band owns this column: the pine forest, and the desert's base under its flat
    if (h <= OAKBEACHY) return h;                      // …and any ground already at or under the BEACH, in either forest
    let d = bankDist(x, z);
    // ── AND A DISTANCE TO THE WATER THE SKIRT CANNOT SEE (user 2026-08-31: "the transition needs to be much
    // smoother") ── bankDist walks WATERSHED geometry only, so lakes cut by a biome's own height field are
    // invisible to it. Measured: the cone ran on 5 of 33 pine shorelines. The other 28 kept the raw field
    // gradient, which terraces every four voxels, and nothing that reshapes the HEIGHT can fix that - a remap
    // with both ends pinned expands wherever it compresses (tried twice, reverted twice). Widening a terrace
    // means moving land horizontally, and that needs a horizontal measure.
    // For a smooth field, the distance to its own WL contour is (h - WL) / |grad h| to first order, and the
    // gradient costs two extra samples because the centre value is the h we were handed. Gated hard: only where
    // the watershed walk found nothing AND the column is low enough that the cone could reach it at all, so the
    // whole inland world pays nothing. It is an ESTIMATE and only ever used to make the cone reach further, so
    // being wrong makes a shore gentler than it needed to be, never a cliff.
    if (d >= OAKBANKR && h - WL < 92) {
      const g = 4, fld = bm > 0 ? birchH : (dx >= OAKFAR || dx <= OAKWFAR ? pineH : oakH);
      const gx = (fld(x + g, z) - h) / g, gz = (fld(x, z + g) - h) / g;
      const gr = Math.sqrt(gx * gx + gz * gz);
      if (gr > 0.02) { const df = (h - WL) / gr; if (df >= 0 && df < d) d = df; }
    }
    if (d >= OAKBANKR) return h;                       // no water within the skirt
    // ── THE CONE RISES TO THE COLUMN'S OWN HEIGHT, NOT TO A FIXED TOP (user 2026-08-31: "you have one flat
    // step of sand, then much smaller steps of sand that lead to the dirt terrain … all water banks should have
    // a flat but also smooth transition") ── it used to climb to OAKBANKY + OAKBRISE, a constant 86 over the
    // waterline, chosen to clear HMAX - WL = 81 so the min would always release into the hillside. That works,
    // but it spends the SAME 82 voxels of rise beside a low lake as beside a mountain, and 82 over the reach is
    // a gradient of ~0.2 - one terrace every five voxels. That is the stepping: a wide flat beach, then the
    // sstep accelerates and the terraces narrow to three or four voxels on the way up to the grass.
    // Lerping to `h` instead makes the rise exactly as tall as the land actually is. Beside ordinary lake
    // country (h ~190) that is 34 voxels over the same reach - a gradient of 0.09, terraces two to three times
    // wider - and it only steepens where the ground genuinely does. It also RETIRES the clear-HMAX rule rather
    // than restating it: c reaches h precisely at the rim by construction, at any terrain height, so there is
    // no top left to set too low. That rule cost a crease once already this session.
    const c = OAKBEACHY + (h - OAKBEACHY)
              * sstep(Math.max(0, d - OAKBEACH) / (OAKBANKR - OAKBEACH));
    if (c >= h) return h;
    if (bm > 0) return bm >= 1 ? c : h * (1 - bm) + c * bm;   // the birch band, faded on its own mask for the same reason the oak rim is
    if (pine) return c;                                // the pine forest: the cone at full strength, exactly as deep oak gets it
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
  const DESH = 2160, DESC = BAND_MIRROR * (DESOFF + DESH);             // half-width to the mask MIDPOINT, and the band centre: DESC ± DESH = 4460 and 6620
  const desertM = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
  // ══ THE BIOME BORDER RIVERS (user 2026-08-31: "separate the different biomes with rivers and water") ══
  // Every band mask in this file is the SAME shape — a centre line c(z) that wobbles with z, and a mask that
  // reads exactly 0.5 where |pwrap(x - c)| equals the band's half-width. So "the border between two biomes" is
  // not something that has to be searched for: it IS that iso-line, in closed form, for all five bands at
  // once. bioEdge below is bankEval's trick applied to the band geometry instead of the channel geometry — a
  // second, different READ of numbers that already exist — so where the rivers run can never drift away from
  // where the biomes actually change. Move a band and its river moves with it, for free.
  //
  // AND IT GOES INTO riverS, NOT INTO H. There are THREE copies of H (this one, and makeHRow/makeHCol in
  // world/gen-noise.js, which __vb.htest() asserts are identical), and adding a term to all three by hand is
  // the one edit in this file that cannot be made safely by inspection. All three already carve on `rs`, so a
  // term folded into riverS reaches every one of them by construction and htest/gtest stay clean without being
  // tested — the same reason the cherry forest was carved out of oakM rather than given a height field of its
  // own. It also means the border rivers inherit the ENTIRE water stack for nothing: the bed carve, the beach
  // dither, the water fill, shore surf, fish, lily pads and the river ambience bed all key on rs, and not one
  // of them had to learn a new shape.
  const BIORW = 122;                                    // channel half-width in voxels, before the per-z variation below.
  // TRIPLED from 26 (user 2026-08-31: "triple the width of the rivers that separate the biomes"). The number
  // that actually had to triple is the WET width, which is not this one: the H carve only floods above
  // rs ~0.65, and with BIORSAT at 1.10 that lands at d = 0.468 * bwW, so the water is 0.94 * bwW across.
  // 26 measured 30-44 voxels in-game, and 122 gives ~114 - three times the water, which is what a player
  // actually sees. It is 122 rather than 78 because BIORSAT came down in the same pass (see below): a
  // gentler ramp reaches the flood line sooner, so the channel has to be wider to end up equally wide.
  const BIORSAT = 1.10;                                // …and the saturation over the middle of the channel.
  // ── THIS IS WHAT MADE THE BANKS DROP OFF (user 2026-08-31: "the edges of the river just seem to drop
  // off. make the banks of the river smoother as it goes into the water to the ocean floor") ── it was
  // 2.2, borrowed from the trick rivEval's LAKES use to stay wet bank to bank. On a lake that is right;
  // on a river cross-section it is not, because saturating rs at 1 across the inner 55% makes the bed a
  // flat PLATEAU, and every voxel of the ~61 the bank has to climb then has to happen in the thin taper
  // that is left. Measured in-game at BIORW 26: 124 at the centre and 150 only 14 voxels out - and 150 is
  // still UNDER the waterline at 152, so the drop-off the user saw was the submerged half of it, a wall
  // going straight down to the bed with no shelf at all.
  // At 1.10 the ramp is very nearly linear: full depth is held only over the inner ~9% (an 11-voxel
  // thalweg, so the deepest part is still a channel and not a single column) and rs then falls smoothly
  // all the way out. Depth now ramps 124 -> 131 -> 141 -> 151 over 0/30/45/57 voxels, so the bed shelves
  // up to the shore at about 1:2 instead of dropping off it, and the valley term below carries the same
  // curve on above the waterline. The whole section is one continuous slope now, wet part included.
  let bpZ = null, bpD = 0, bpO = 0, bpC = 0;
  const bioPin = () => {                               // the wobbles are PINNED at the spawn's own z, exactly as desertM/oakM/cherryM pin theirs, so the borders meander identically. Lazy and keyed on SPWZ, NOT precomputed: SPWX/SPWZ are `let`s baked by sim/player.js, which loads AFTER this file, so evaluating them at load time is a TDZ throw — and they are re-randomised per world, so the key has to be checked rather than assumed.
    if (bpZ === SPWZ) return;
    bpZ = SPWZ; bpD = desWob(SPWZ); bpO = oakWob(SPWZ); bpC = bpO;   // the cherry band rides oakWob now (see CHHALF), so the border river on its edge has to ride it too or the water and the blossom part company
  };
  let bwZ = null, bwD = 0, bwO = 0, bwC = 0, bwM = 0, bwW = 0;
  const bioWobZ = (z) => {                             // ── 1-ENTRY MEMO, KEYED ON z ── every term here is a function of z ALONE, and the row path (makeHRow) walks a whole 2048-wide row at one z, so this is ~9 vnoise per ROW rather than per column. The column path (makeHCol) misses every time and pays them, which is what it already does for the masks themselves.
    if (z === bwZ) return;
    bwZ = z; bwD = desWob(z); bwO = oakWob(z); bwC = bwO;
    bwM = (vnoise(z * 0.0029 + 61.3, 137.9) - 0.5) * 96 + (vnoise(z * 0.0104 + 19.7, 173.1) - 0.5) * 30;   // the CHANNEL'S own meander, on top of the band's: the border already wobbles, but a river that follows it exactly reads as a drawn line. This lets the water cross and re-cross the line it marks, which is what a real border river does.
    bwW = BIORW * (0.72 + 0.56 * vnoise(z * 0.0017 + 88.1, 41.3));   // …and it narrows and widens along its length rather than running at one gauge
  };
  // Distance in voxels to the nearest band EDGE, over all five bands. Each line is that band's own mask
  // arithmetic read backwards: |pwrap(x - c)| is the distance from the centre line, so subtracting the
  // half-width and taking |.| is the distance to the edge. cherryM is the one that states its half-width
  // differently — its t crosses 0.5 at CHHALF + CHB/2, not at a bare constant — so that is written out here.
  const bioEdge = (x, z) => {
    bioPin(); bioWobZ(z);
    const xm = x - bwM;                                // the meander shifts the RIVER, never the band: the masks are not touched, so no biome moves and no tree, spawn or life band shifts with it
    const dw = bwD - bpD, ow = bwO - bpO, cw = bwC - bpC;
    let b = 1e9, d;
    d = Math.abs(Math.abs(pwrap(xm - (SPWX + DESC   + dw))) - DESH);              if (d < b) b = d;
    d = Math.abs(Math.abs(pwrap(xm - (SPWX + ARCTC  + dw))) - ARCTH);             if (d < b) b = d;
    d = Math.abs(Math.abs(pwrap(xm - (SPWX + BIRCHC + dw))) - BIRCHH);            if (d < b) b = d;
    d = Math.abs(Math.abs(pwrap(xm - (SPWX + OAKC   + ow))) - OAKH);              if (d < b) b = d;
    d = Math.abs(Math.abs(pwrap(xm - (SPWX - CHOFF  + cw))) - (CHHALF + CHB * 0.5));  if (d < b) b = d;
    return b;
  };
  const BIORIV_ON = location.search.includes('noriv') ? 0 : 1;   // ?noriv — the whole feature on one switch, and a URL flag rather than a live toggle for two reasons: the height field is BAKED into W, so only a world rebuild could apply a flip anyway, and a plain const is carried into both gen workers by the ordinary consts registry. A `let` would have to be hand-declared in each preamble AND kept in step across three threads to mean anything.
  // ── AND THE CHANNEL SITS IN A VALLEY, OR IT IS A CANAL ── the first version was the core term alone, and
  // in-game it read as a trench with vertical walls: BIORSAT saturates the inner ~54% of the channel at full
  // strength, so the whole climb out happened in the ~12 voxels of taper that were left. Measured across the
  // first build, at the waterline WL=152: 124 at the centre and back up to 190 only 60 voxels away, most of it
  // in the last twelve. A river cuts a VALLEY and then runs along the bottom of it, so there are two terms
  // here, not one: the core is the wet channel and is unchanged, and the valley is a much wider, much gentler
  // cone the core sits inside. rs is already a soft carve at low strength — the H lerp pulls h only part of
  // the way to the bed — so a valley is just the same expression evaluated further out.
  const BIORVALL = 2.0;                                // the valley reaches this many channel-widths from the line
  // ── AND IT CAME DOWN WHEN BIORW TRIPLED, WHICH IS NOT A HEDGE AGAINST THE ASK ── this is expressed in
  // CHANNEL widths, so leaving it at 4.5 would have tripled the valley too: a 900-voxel-wide depression
  // either side of every border, with banks three times flatter than the ones that were just tuned to look
  // right. The bank has the same job it always had - climb the ~60 voxels from the bed back to open land -
  // and that vertical did not change when the river got wider, so the bank should keep its slope and only
  // start further out. It ran 99 voxels before (wet edge 19 -> valley 117 at BIORW 26); 2.0 puts the valley
  // at 156 against a wet edge of 57, which is the same 99. The river widens; the shoreline does not flatten.
  const BIORVK = 0.72;                                 // …and this is its strength at the centre, well under the core's, so the core still owns the middle and this only shapes the shoulders
  const bioRivS = () => 0;   // WIPED 2026-08-31 (user: "completely wipe the terrain generation and restart"). One biome now - the pine forest - so this mask is identically zero and every branch it gated is dead. Kept as a zero rather than deleted because terrain.js, the worker registries and debug-api all still name it.;
    // ══ THE PINE FOREST, AND IT IS THE WHOLE WORLD ═══════════════════════════════════════════════
  // Every band, mask and blend that used to stand between here and the waterline is gone (user
  // 2026-08-31: "completely wipe the terrain generation and restart completely ... begin by
  // implementing the pine forest"). There is ONE field, it is the pine forest's, and it is
  // evaluated everywhere - no biome selector runs in the height path at all any more.
  // ROUND, and that is what the double smoothstep buys: one pass leaves the midband of the noise
  // linear, and a linear midband is what makes a voxel hill read as a slab with a corner on it.
  // Twice pushes the field toward its own ends, so hilltops dome over and valley floors flatten
  // out, and it widens the height histogram at the same time - which is the "a lot of terrain
  // height variation" half of the ask.
  const PINE_LOW    = 6;                               // hard floor: a lake bed may not reach the world's own bottom
  const PINE_BASE   = WL - 40;                             // the deepest ground, 52 under WL. Depth is the point - a shallow
                                                       // field floor reads as wet sand rather than as a lake (recorded
                                                       // against the first pine water build, which produced exactly that).
  // ── THE SHALLOWS ARE THE PROBLEM, NOT THE DEPTH (user 2026-08-31: "theres alot of areas with very
  // shallow water. can you deepen these very shallow areas") ── the field already reaches 40 under the
  // waterline, but almost nothing USES that: the distribution puts most wet columns a voxel or two under,
  // so wide stretches read as a wet floor with the bed showing through rather than as water.
  // Lowering PINE_BASE does not fix it - that moves the FLOOR, and the floor was never what these columns
  // were sitting on. What is needed is to bend the shallow end of the depth curve down, which is what this
  // does: depth d becomes d + DEEP_ADD * (d / DEEP_SPAN), capped at a constant +DEEP_ADD once past the span.
  // d = 0 maps to d = 0, so the WATERLINE ITSELF DOES NOT MOVE and the shore is untouched - no step, no
  // band, and the beach that meets it is the same beach. A 3-deep flat becomes 7, an 8-deep becomes 19.
  // Dry land is not touched at all: the whole term is behind h < WL.
  // ── AND A THRESHOLD, WHICH IS WHAT KILLS THE PUDDLES (user 2026-08-31: "prevent small puddles of
  // water") ── censused over five far-apart regions, the small bodies and the shallow ones are the SAME
  // bodies: 18 and 36 vox^2 at 0 deep, 396 and 594 at 2 deep, against real lakes at 5, 7, 12 and 14. So
  // depth alone separates them, and depth is the one thing a per-column height function can test - H
  // cannot look at its neighbours without recursing into itself.
  // A plain "dry anything under N deep" would put a lip at every shore, which is the opposite of what was
  // asked for two turns ago. Instead the ramp is moved: raw depth up to DEEP_MIN is not water at all (the
  // existing `!lake && h <= WL` clamp in world/terrain.js lifts it to WL+1 as dry ground), and past that
  // the depth is measured FROM DEEP_MIN and multiplied. At the boundary the new depth is 0, so the water's
  // edge is still flush with the land - continuous, no step - and one raw level further in it is already
  // several voxels deep, which is what makes what remains read as a lake rather than a wet patch.
  const DEEP_MIN = 1, DEEP_K = 4.0, DEEP_CAP = 46;
  const deepen = (h) => { if (h >= WL) return h; const d = WL - h;
    if (d <= DEEP_MIN) return WL;                      // too shallow to be water - terrain.js dries it to WL+1
    return Math.round(WL - Math.min(DEEP_CAP, (d - DEEP_MIN) * DEEP_K)); };
  const PINE_WET    = 1.00;                            // ── AND THE WATER IS PUT BACK (user approved 32%) ── doubling the
                                                       // relief moved the whole field UP relative to a waterline that did not move
                                                       // with it, and the lakes drained: 32.3% -> 12.7%. This exponent pulls the
                                                       // distribution back down toward its own floor WITHOUT touching either end,
                                                       // so the peaks still reach HMAX and the valleys still flood. Measured over
                                                       // 111k columns: 1.0 -> 12.7%, 1.3 -> 27.8%, 1.6 -> 41.4%.
  const PINE_RELIEF = HMAX - PINE_BASE;                             // ...up to HMAX, so a ridge stands ~74 over the water carrying a whole 152-voxel tree.
  const pineField = (x, z) => {
    // ── RAISED, NOT STEEPENED (user 2026-08-31: "dont make the terrain steeper per say, just raise the
    // elevation... correct?") ── yes, and the first cut got it wrong: it multiplied the AMPLITUDE and left
    // the wavelengths alone, and a slope is amplitude over wavelength, so every gradient in the world went
    // up with it. The visible cost was the shoreline - the ground crossed the whole beach window in a voxel
    // or two and the grass came down to the water. Each octave's wavelength is now stretched by the SAME
    // 1.635 the relief grew by, so the hills are twice as tall and no steeper than they were: bigger
    // country, same walk up it.
    const a = fbm(x * 0.00098 + 61.3, z * 0.00098 + 77.9);   // the massifs - one hill every ~1000 voxels
    const b = fbm(x * 0.00257 + 25.1, z * 0.00257 + 13.7);   // the shoulders on them
    // ── AND THE ROLL DOES NOT MOVE (user 2026-08-31: "I still want the terrain to be hilly like it is") ──
    // stretching THIS octave with the other two is what would flatten the world into smooth swells: it is
    // the one that puts a hill in front of you rather than a horizon. It keeps its original wavelength AND
    // its original amplitude - weight 0.095 of a 206 relief is 19.6 voxels, against 0.15 of the old 126,
    // which is 18.9 - so the ground underfoot rolls exactly as it did. Only the two BIG octaves grew, and
    // they grew in both dimensions at once, which is what raises the country without tilting it.
    const c = fbm(x * 0.011 + 3.7, z * 0.011 + 9.1);       // the roll that makes a hill a hill - unchanged
    return Math.pow(sstep(sstep(a * 0.585 + b * 0.320 + c * 0.095)), PINE_WET);
  };
  const H = (x, z) => {
    let h = Math.min(HMAX, Math.max(PINE_LOW, Math.round(PINE_BASE + PINE_RELIEF * pineField(x, z))));
    h = deepen(h);                                     // bend the shallow end of the depth curve down — see DEEP_SPAN
    const bm = basinM(x, z);                           // the LAKES: broad low-frequency bowls pressed into the field
    const m = bm * basinLow(h, x, z);
    if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(x * 13 + 7, z * 17 + 3) - 0.5) * 0.8);
    const rs = riverS(x, z);                           // and the RIVERS: the watershed network, widened - see RIVWIDE
    const bn = fbm(x * 0.05 + 13.7, z * 0.05 + 4.2);   // bed/beach relief, so a bed is not billiard-flat
    if (rs > 0.02) h = Math.min(h, Math.round((h - Math.max(0, h - (WL + RIVLAND)) * rs) * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(x * 19 + 5, z * 23 + 9) - 0.5) * 0.8));
    // ── AND NOW THE SHORELINE IS NOT TOUCHED AT ALL (user 2026-08-31: "remove that band and smooth it
    // out", then "and I dont mean make the sand beds flatter either like done in the past") ──
    // Both halves of that rule out every version this line has ever had. It has always been an ARTIFICIAL
    // edit to the height at the waterline, and it came in exactly two shapes:
    //   * a TRANSLATION - [WL-2,WL] up by 3, later [WL-6,WL] up by 7. Not continuous at the top of its own
    //     window, so the lifted ground stood proud of the unlifted column beside it. THAT IS THE BAND, and
    //     it got taller every time the window was widened to make the cascade wider.
    //   * a COMPRESSION - sixteen raw levels squeezed onto nine. Continuous, so no band, but squeezing IS
    //     flattening, which is the thing the second half of the report rules out.
    // There is no third shape: any remap that pulls drowned ground into the air either steps at its edge
    // or flattens what it moves. So the edit is gone. The shore is now whatever the field already does -
    // and after the steepness fix that is a gentle natural grade - and the BEACH is made by the sand
    // MATERIAL following it (see `shore` in world/terrain.js) instead of by moving ground. The steps in it
    // are the terrain's own, which is why they neither band nor flatten.
    // It costs water AREA, and that is the honest trade: ground in [WL-6, WL] used to be lifted into dry
    // beach and now simply stays wet, so the lakes get bigger rather than the beaches.

    return h;
  };;
  // ── A RIVER'S WIDTH MUST NOT DEPEND ON HOW HIGH THE LAND BESIDE IT IS (user 2026-08-31: "the river between
  // the pine and oak has areas where the river is very thin. prevent thin rivers from forming") ── the carve
  // is a lerp toward the bed weighted by rs, so the land term h * (1 - rs) puts the SURROUNDING HEIGHT into
  // the channel's depth. On low ground rs 0.65 is enough to flood; beside a 233-voxel hill the same rs comes
  // out above the waterline and only the very middle of the channel goes under. Same river, same rs field,
  // half the width - and the pine mountains are exactly what turned that from a latent bug into a visible
  // one. Capping the land term at WL + 44 decouples them: a channel floods to the same width whether it
  // crosses a valley floor or a shoulder. BIT-EXACT below the cap, which is all ground under 196 - so every
  // river in the world outside the high country is untouched - and it can only ever LOWER a column, because
  // the whole expression is already inside a Math.min against h.
  // ── AND THE CAP FADES IN WITH rs, WHICH THE FIRST VERSION DID NOT (user 2026-08-31: "getting flat walls in
  // the pine forest: I also saw them in the desert") ── written as a bare Math.min(h, WL + RIVLAND) the cap
  // was at FULL strength the instant rs cleared the 0.02 carve threshold, so a 250-voxel hill with the
  // faintest trace of river over it evaluated to 196 and the outer Math.min took it: a 55-voxel vertical
  // face along the rs = 0.02 iso-line, in every biome tall enough to have one. That is the flat wall, and
  // the desert had it too because a dune crest is just as high.
  // `h - max(0, h - cap) * rs` is the same cap reached continuously: identity at rs 0, the full cap at
  // rs 1, and nothing to step over in between. The width it was added to buy is unaffected - what widens a
  // channel is the cap at HIGH rs, which is exactly where this still applies it in full.
  const RIVWIDE = 3.00;                                 // ── WIDE RIVERS (user 2026-08-31: "add in lakes and wide rivers
                                                       // throughout the terrain") ── the channel half-width multiplier. Was 1.4, which is
                                                       // the width the seven-band world wanted when a river was a BORDER between biomes.
                                                       // Here a river is a feature of the forest itself and is meant to be crossed by
                                                       // swimming, not by stepping, so it carries most of the 25% water target with the lakes.
  const RIVLAND = 44;                                  // …how far over the waterline the land may push a channel's bed
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
      // ── ONE STEM, NO TRIBUTARIES (user 2026-08-31: "dont have the river split into thinner rivers, just
      // keep the one big river") ── this gave every watershed 1-3 tributaries feeding the main stem, and a
      // tributary is BY CONSTRUCTION thinner than what it feeds: those are the thin slivers in the report.
      // Widening them was not the answer either, because a system whose branches are as wide as its trunk
      // does not read as a river at all. So the stem keeps its full width and the branches are gone. The
      // headwater ponds go with them - they were the tributaries' sources, and a pond feeding nothing is
      // just a puddle. The reservoir the stem feeds is untouched.
      const nT = 0;   // was: lone ? 0 : 1 + ((ihash(cx * 7 + 44, cz * 3 + 18) * 2.99) | 0)
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
    // ── ARCTIC RIVERS ARE TWICE AS WIDE (user 2026-08-30) ── applied to the channel half-width, so the banks
    // move apart and the whole cross-section scales; the H carve downstream reads rs and deepens with it, so a
    // wider arctic river is a bigger river rather than a shallow smear. Hoisted OUT of the segment loop: this
    // costs one arcticM (two vnoise) per column instead of one per segment, and rivEval is height-path code.
    const aw = 1 + arcticM(x, z);
    for (const sg of R.segs) {
      const tRaw = (x - sg.sx) * sg.dxr + (z - sg.sz) * sg.dzr;
      const t = Math.max(0, Math.min(sg.len, tRaw));
      const off = Math.sin(t * 0.015 + sg.seed) * 30 + Math.sin(t * 0.04 + sg.seed * 1.7) * 10;   // broad meanders
      const pd = (x - (sg.sx + sg.dxr * t)) * (-sg.dzr) + (z - (sg.sz + sg.dzr * t)) * sg.dxr;
      const w = sg.wb * RIVWIDE * aw * (sg.t0 + (sg.t1 - sg.t0) * (t / sg.len));   // width taper: stems widen downstream, tributaries narrow to their heads; aw doubles it across the arctic
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
  // ── AND GAMEPLAY QUERIES GET A SCOPE TOO, WITHOUT HAVING TO DECLARE ONE ── rivScope is the bulk-gen fast
  // path: gather the watersheds for a region once, then every column in it walks that short list. Everything
  // OUTSIDE a bulk sweep — H() for a creature's ground, a perch, the player's own feet — fell through to the
  // scan below, and that scan is RIVINF/RIVCELL cells each way: 6200/768 rounds to 17, so 17 x 17 = 289
  // riverAt calls PER COLUMN. Only 3.5% of cells carry a watershed, so ~279 of those return null and are
  // pure overhead. MEASURED while running: riverAt 12.3% of main-thread self time, with riverS 2.8%,
  // vnoise 2.6% and bankDist 2.1% behind it — nearly a fifth of the frame's CPU between them.
  // So the scope builds itself, keyed on the RIVCELL cell the query lands in. The list gathered is the UNION
  // over the whole cell (gatherRivers' own range arithmetic), which is a strict SUPERSET of what any single
  // point in that cell would have scanned — floor is monotonic, so every point's range sits inside it.
  // Evaluating a superset is safe for exactly the reason rivScope is already safe, and it is the same
  // invariant rather than a new one: riverS takes a MAX of rivEval and a watershed beyond its influence
  // radius contributes 0, bankDist takes a MIN of bankEval and a further watershed can only be further away.
  // The bulk path is untouched — when rivScope is set it still wins, because it is broader still.
  const rivNear = new Map(), RIVNEAR_CAP = 4096;       // ~9 cells cover a loaded window; the cap is only a bound on a session that wanders the whole planet
  function riversNear(x, z) {
    const cx = Math.floor(x / RIVCELL), cz = Math.floor(z / RIVCELL), key = cx * 100003 + cz;
    let L = rivNear.get(key);
    if (L === undefined) {
      if (rivNear.size >= RIVNEAR_CAP) rivNear.clear();   // wholesale, not LRU: the working set is a handful of cells, so a clear costs one rebuild of those
      L = gatherRivers(cx * RIVCELL, (cx + 1) * RIVCELL - 1, cz * RIVCELL, (cz + 1) * RIVCELL - 1).list;
      rivNear.set(key, L);
    }
    return L;
  }
  function riverS(x, z) {                              // channel strength 0..1 at this column
    let best = bioRivS(x, z);                          // ── THE BIOME BORDER RIVER ── seeded here rather than added anywhere downstream, so it is folded into the SAME max the watersheds are and every one of rs's consumers gets it without knowing it exists. A watershed crossing a border simply wins where it is the deeper of the two.
    if (rivScope && x >= rivScope.x0 && x < rivScope.x1 && z >= rivScope.z0 && z < rivScope.z1) {
      for (const R of rivScope.list) { const v = rivEval(R, x, z); if (v > best) best = v; }
      return best;
    }
    const LS = riversNear(x, z);                       // the self-managing scope — 289 riverAt calls become one Map.get
    for (let i = 0; i < LS.length; i++) { const v = rivEval(LS[i], x, z); if (v > best) best = v; }
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
    const LB = riversNear(x, z);                       // …and the same scope serves the bank walk
    for (let i = 0; i < LB.length; i++) best = bankEval(LB[i], x, z, best);
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


