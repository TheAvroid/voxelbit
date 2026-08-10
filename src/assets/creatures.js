  // ── cardinal ── an OFF-GRID animated flyer. Its 8 wing-flap frames (assets/life/cardinal/flight) are loaded into the held-item voxel
  // table below and DDA-raytraced as a drop-style model — arbitrary (sub-voxel, non-grid-snapped) rotation, depth-tested against the scene,
  // NEVER written into the world grid. Flight (a fixed high orbit near spawn) is driven in the tick loop. Model x→width, y→depth (beak at −y), z→height.
  const FLYERS = [];                                   // every loaded flight strip: { name, item0, n, glide }. Adding a species is a directory name — the flock,
  let BLUEF_ITEM0 = 0, BLUEF_NFRAMES = 0, BLUEF_GLIDE = 1;   // the even split, the rig and the render path all key off this table, so nothing is per-species
  let BIRD_ITEM0 = 0, BIRD_NFRAMES = 0, BIRD_GLIDE = 1;   // dit (1-based item id) of flap frame 0, frame count, and the frame held while GLIDING — FRAME 01 (user), not the auto-picked widest one, which landed on 00
  let BFLY_ITEM0 = 0, BFLY_NFRAMES = 0;                // butterfly flap frames (assets/life/butterfly/<color>) — orange flies over the editor stage; ALL colors fly the forest BY DAY
  let DFLY_ITEM0 = 0, DFLY_NFRAMES = 0;                // DRAGONFLY flap frames (assets/life/dragonfly) — one colour, 6 frames. Flies the butterfly's kind-0 code path verbatim; only its HOME differs (water, not meadow)
  let BFLY_COLS = [];                                  // first item id of each loaded color's 8 frames, in folder order: orange, red, blue, lime, pink, purple. Colour is picked by a spatial hash of the creature's cell (B.col), so adding colours scatters them evenly and procedurally with no other change
  let FFLY_ITEM0 = 0, FFLY_NFRAMES = 0;                // firefly frames (assets/life/firefly) — replaces the butterflies AT NIGHT; the yellow abdomen voxel glows via the drops dYv.w lane
  let WORM_ITEM0 = 0, WORM_NFRAMES = 0;                // inchworm crawl frames (assets/life/worm) — a GROUND crawler, active day and night (pool slots 24+)
  const FISHES = [];                                   // every loaded FISH strip: { name, item0, n } (assets/life/<species>/00..NN.vox — salmon today, minnow etc. join automatically
                                                       // when their numbered frames ship). Fish are kind-6 wbf creatures: OFF-GRID swimmers under the water surface of lakes AND rivers,
                                                       // burst-glide pacing (dart → coast), free y-axis rotation with pitch/bank, and a fast bolt away from a too-close player.
  let DUCK_ITEM0 = 0;                                  // duck (assets/life/duck/base.vox) — floats on lakes with its orange feet below the waterline (pool slots 16-19 = moms)
  let DUCK_GREEN = [0.03, 0.22, 0.07];                 // the mallard HEAD green (linear) — extracted at load; the black eye voxel blinks to THIS (user)
  let DUCKB_ITEM0 = 0;                                 // duckling (assets/life/duck/baby.vox, 3×7×5) — 1-3 follow each mom in a line (pool slots 18-23, 3 reserved per mom)
  let DUCKB_EYES = [];                                 // …its EYE voxels in model space — where an orphan's tears come from (see the scan at load)
  let LILY_ITEM0 = 0, LILY_SZ = [];                    // lily pads (kind 4) drift and rotate OFF-grid on the water; static worldgen stamping retired
  // ── FOAM, ONE COLOUR ── the shoreline foam the water shader draws AND the splash droplets thrown when
  // something breaks the surface (user 2026-08-05). Linear, because that is what the shader works in; the
  // item builder wants sRGB bytes, so it converts rather than carrying a second literal that could drift.
  const FOAM_RGB = [0.82, 0.87, 0.90];
  const FOAM_SRGB = FOAM_RGB.map((v) => Math.round(Math.pow(v, 1 / 2.2) * 255));
  let FOAM_IT = 0;                                     // …its item id (one 10 cm voxel, the splash particle)
  let SPARK_IT = 0, HITRED_IT = 0, KNIFE_IT = 0, ROCK_IT = 0, STICK_IT = 0, CONE_IT = 0, SMOKE_IT = 0, PICK_IT = 0, SHOVEL_IT = 0, BOW_IT = 0, BOW_NOCK = 0, BOW_FRAMES = 0, ARROW_IT = 0, MEAT_IT = 0, HOE_IT = 0, SPEAR_IT = 0;   // BOW_NOCK = the same strip WITHOUT the arrow, shown once it is loosed   // item ids of the clash spark, stone knife, held field-stone, held stick, held pinecone, DEATH SMOKE (the natural ones get AO+grain)
  let BLUEB_ITEM0 = 0, ROBIN_ITEM0 = 0;               // the two songbird RESKINS in the item table (trace path). Identical geometry to the cardinal, so they reuse CARD_NFRAMES / CARD_OFF / CARD_FOOTZ untouched.
  let CARD_ITEM0 = 0, CARD_NFRAMES = 0, CARD_H = 0, CARD_FOOTZ = 1e9;   // cardinal ROTATE frames (assets/life/cardinal/rotate/00-10, base.vox ignored) → item table; ONE animated model STANDING on the asset-editor stage. CARD_H/CARD_FOOTZ plant the feet on the plane.
  let CARDINAL_ROTATE = [];                            // + the same frames' RAW bytes, auto-imported as the editable filmstrip on the editor stage (,/. select each frame) while the standing model previews the animation
  let BLUEBIRD_ROTATE = [];                            // BLUE BIRD = the cardinal model reskinned (assets/life/blue_bird/rotate/00-10, base.vox ignored) — same 11-frame rotate filmstrip, shown on the editor stage
  let ROBIN_ROTATE = [];                               // ROBIN = the SAME bird recoloured again (assets/life/robin/rotate/00-10, base.vox ignored) — same layout as the cardinal and blue bird
  let BUNNY_ROTATE = [];                               // ROTATE bunny frames (assets/life/bunny/rotate/00-10) — the RIGHT-lane editor object (jumps + rotates in place)
  let BUNNY_JUMP = [];                                 // JUMP bunny frames (assets/life/bunny/jump/00-10) — the LEFT-lane editor object (jumps FORWARD). Both show on the stage together (user).
  let BUNNY_ROTATE_RIGHT = [];                         // RIGHT-rotating bunny frames (assets/life/bunny/rotate/right/00-10) — authored right-turn .vox (rotation is IN the voxel data), user
  let ARMADILLO_WALK = [];                             // ARMADILLO walk frames (assets/life/armadillo/walk/00-07) — world creature; still loaded for the pine-forest armadillo (no longer the asset-editor default)
  let SKUNK_WALK = [];                             // SKUNK walk frames (assets/life/skunk/00-09) — the asset-editor object (user: replaced the porcupine in the editor)
  let ARMADILLO_ITEM0 = 0, ARMADILLO_NFRAMES = 0;      // (legacy trace path — the world armadillo now GRID-STAMPS instead, see ARMADILLO_POSES)
  let ARMADILLO_POSES = null, ARMADILLO_FOOTZ = 0;     // GRID-STAMP poses [frame][heading] (built lazily) — the world armadillo stamps into W exactly like the asset editor (identical box-centre + armOffset), so its alignment matches 1:1 (user)
  let SKUNK_ITEM0 = 0, SKUNK_NFRAMES = 0;              // SKUNK walk frames in the ITEM table (trace path). Without this the emit's item selector fell through to WORM_ITEM0 and a trace-injected skunk drew a worm.
  let SKUNK_POSES = null, SKUNK_POSES_B = null, SKUNK_FOOTZ = 0;     // GRID-STAMP poses [frame][heading] for the world skunk — normal + eye-BLINK variant (SKUNK_POSES_B), built lazily via edBuildFrames so the world skunk matches the asset-editor skunk 1:1 (alignment, blink, all) (user)
  let PORCUPINE_WALK = [];                             // PORCUPINE walk frames (assets/life/porcupine/00-…) — WORLD-only 4th land mammal (user re-added it alongside the skunk; the editor object stays the skunk). Behaves like the armadillo: constant cardinal march, 24 fps, grid-stamped, no blink.
  let PORCUPINE_ITEM0 = 0, PORCUPINE_NFRAMES = 0;      // and the PORCUPINE's, same reason (it had no entry either)
  let PORCUPINE_POSES = null, PORCUPINE_FOOTZ = 0;     // GRID-STAMP poses [frame][heading] for the world porcupine (built lazily from PORCUPINE_WALK + PORCUPINE_BAKES) — same machinery as the armadillo, parity-corrected headings
  // ── HOW A LAND MAMMAL SITS ON THE GROUND ── measured from the MODEL, never hand-tuned per band. The
  // renderer centres a model on its anchor, so the underside of its lowest OCCUPIED layer is anchor − h/2 + z0:
  // seat = h/2 − z0 is therefore exactly the lift that puts its feet on the surface. Hand-tuned constants got
  // this wrong the moment a second animal joined: 2.5 is right for the armadillo (5 tall) and was reused
  // verbatim for the skunk and the porcupine, which are 8 — so both walked 1.50 voxels UNDER the ground
  // (measured, median, on flat terrain). Grid-stamped that never showed, because the stamp path takes the
  // ground as an argument and OVERWRITES the terrain voxels it lands in; trace-injected, the terrain and the
  // model coexist and you see straight through the animal into the dirt (user 2026-08-07).
  // hw/hd are the occupied footprint half-extents — across the heading and along it — for the ground scan.
  let MAMFIT = {};                                     // {item0: {seat, hw, hd}} — filled once, after the frames load
  const mamFitOf = (items9, it0) => { const it = items9[(it0 | 0) - 1]; if (!it || !it.cells) return null;
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9;
    for (let z = 0; z < it.h; z++) for (let y = 0; y < it.d; y++) for (let x = 0; x < it.w; x++) {
      if (!it.cells[x + y * it.w + z * it.w * it.d]) continue;
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; if (z < z0) z0 = z; }
    if (x1 < x0) return null;
    return { seat: it.h * 0.5 - z0, hw: (x1 - x0 + 1) * 0.5, hd: (y1 - y0 + 1) * 0.5 }; };
  let BUNNY_ITEM0 = 0;                                 // …and the same frames in the ITEM table (BUNNY_ITEM0+frame) so the editor TRACE-INJECTS the playing bunny — identical render path to every other life form (grain + baked AO + SVGF)
  let BUNNY_NFRAMES = 0;                               // frame count of the item-table bunny ROTATE frames — the WORLD bunnies play these while turning
  let BUNNY_JUMP_ITEM0 = 0, BUNNY_JUMP_NFRAMES = 0;    // …and the JUMP frames in the item table — world bunnies play these while HOPPING forward (editor-style grid behavior)
  let itemsRef = null;                                 // module-scope handle to the loader's `items` table (dims + baked cells) — the tick loop reads it for the cardinal hitbox + worm grid-stamp

