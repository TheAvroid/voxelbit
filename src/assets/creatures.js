  // ── cardinal ── an OFF-GRID animated flyer. Its 8 wing-flap frames (assets/life/cardinal/flight) are loaded into the held-item voxel
  // table below and DDA-raytraced as a drop-style model — arbitrary (sub-voxel, non-grid-snapped) rotation, depth-tested against the scene,
  // NEVER written into the world grid. Flight (a fixed high orbit near spawn) is driven in the tick loop. Model x→width, y→depth (beak at −y), z→height.
  let BIRD_PINK = -1;                                  // index into FLYERS of the derived pink bird, or -1 — set in assets/held-items.js, where load order is a fact rather than a guess
  const FLYERS = [];                                   // every loaded flight strip: { name, item0, n, glide }. Adding a species is a directory name — the flock,
  let BLUEF_ITEM0 = 0, BLUEF_NFRAMES = 0, BLUEF_GLIDE = 1;   // the even split, the rig and the render path all key off this table, so nothing is per-species
  let BIRD_ITEM0 = 0, BIRD_NFRAMES = 0, BIRD_GLIDE = 1;   // dit (1-based item id) of flap frame 0, frame count, and the frame held while GLIDING — FRAME 01 (user), not the auto-picked widest one, which landed on 00
  let BFLY_ITEM0 = 0, BFLY_NFRAMES = 0;                // butterfly flap frames (assets/life/butterfly/<color>) — orange flies over the editor stage; ALL colors fly the forest BY DAY
  let DFLY_ITEM0 = 0, DFLY_NFRAMES = 0;                // DRAGONFLY flap frames (assets/life/dragonfly) — one colour, 6 frames. Flies the butterfly's kind-0 code path verbatim; only its HOME differs (water, not meadow)
  let BFLY_YELLOW = -1;                                // index into BFLY_COLS of the SYNTHETIC yellow (built from the pink set at load) — the forests' pink stand-in
  let BFLY_HASHN = 0;                                  // how many colours the per-cell hash may pick from: the authored ones only, so the synthetic yellow is never rolled at random
  let BFLY_PINK = -1;                                  // index into BFLY_COLS of the 'pink' folder, or -1 if it never loaded — set in assets/held-items.js as the colours are parsed, because the index is a LOAD-ORDER fact and not a constant
  let BFLY_COLS = [];                                  // first item id of each loaded color's 8 frames, in folder order: orange, red, blue, lime, pink, purple. Colour is picked by a spatial hash of the creature's cell (B.col), so adding colours scatters them evenly and procedurally with no other change
  // ── EDITOR-STAGE EXHIBITS ── models whose frames live in ONE scene-graph .vox rather than a folder of
  // numbered files, loaded into the same item table so the stage can TRACE-INJECT them (assets/held-items.js
  // edStripItems). That is the whole reason they are here: an item id is what the emit addresses a model by,
  // and only an emitted model gets a sub-voxel position and a free heading. Grid-stamping cannot.
  let LBUG_ITEM0 = 0, LBUG_NFRAMES = 0;                // LADYBUG (assets/life/ladybug.vox) — flies the stage on the butterfly's own steering
  // ── THE NAMED ANIMATIONS INSIDE ONE .vox ── lives HERE, at the top level of an early fragment, because two
  // very different callers need it and neither may be the one that declares it: assets/held-items.js reads it
  // during the boot asset load, and ui/editor.js reads it when staging. It began in the editor, which the
  // loader could not see (the load runs long before that fragment is evaluated). Moving it into the loader
  // then hid it from the EDITOR — it landed inside that file's nested init function rather than at top level,
  // so it was function-scoped, and every editor stage failed with "edVoxSeqs is not defined" and an empty
  // stage. Top level of a file both can see is the only placement that satisfies both.
  // ── MOVED HERE FROM ui/editor.js (2026-08-22) ── it is a pure .vox SCENE-GRAPH reader that mints nothing
  // and touches no editor state, and it has to be declared BEFORE this loader rather than after it: the asset
  // load runs during boot, well before the editor fragment is evaluated, so calling it across the bundle threw
  // "Cannot access 'edVoxSeqs' before initialization" and the frog silently loaded zero frames (the catch
  // below swallowed it). Same class as the const-order black screen. The editor still calls it — it is later
  // in the bundle, so the binding is live by then.
const edVoxSeqs = (pv) => {                          // the NAMED animations inside ONE .vox → [{ name, ids: [model index, …] }], each id list already in frame order
  // A .vox that holds several animations keeps them in its SCENE GRAPH, not in separate files. frog.vox is one
  // MAIN with 34 SIZE/XYZI pairs and an nTRN/nGRP/nSHP tree that says which of them are 'ribbet' (14 frames),
  // 'tongue' (24) and 'hop' (17). edParseVox below walks the SIZE/XYZI pairs alone — which is right for the
  // per-frame files every creature ships as, and wrong here: it hands back all three cycles concatenated in
  // file order, each model appearing once however many times its animation actually plays it. So read the graph.
  // Two things about the format decide the shape of this:
  //   * the FRAME LIST lives on the nSHP — one entry per frame, '_f' the frame index, repeats included, because
  //     'ribbet' genuinely plays model 1 twice. Sorting on '_f' rather than trusting file order is free.
  //   * the NAME lives on the nTRN ABOVE the group, and MagicaVoxel nests a second nTRN called 'frames' inside
  //     every animation. Taking the OUTERMOST name is what makes this return ribbet/tongue/hop instead of three
  //     sequences all called 'frames'.
  // Returns [] for a file with no scene graph (every single-model creature frame), which is what keeps this
  // invisible to the pose builders: they never pass a sequence name, so nothing below even calls it.
  const dvv = new DataView(pv.buffer, pv.byteOffset, pv.byteLength);
  const nodes = new Map();
  const rdStr = (o) => { const n = dvv.getInt32(o, true); let t = '';
    for (let i = 0; i < n; i++) t += String.fromCharCode(pv[o + 4 + i]);
    return [t, o + 4 + n]; };
  const rdDict = (o) => { const n = dvv.getInt32(o, true); o += 4; const d = {};
    for (let i = 0; i < n; i++) { const k = rdStr(o); const v = rdStr(k[1]); d[k[0]] = v[0]; o = v[1]; }
    return [d, o]; };
  const walk = (off, end) => { while (off + 12 <= end) {
    const id = String.fromCharCode(pv[off], pv[off + 1], pv[off + 2], pv[off + 3]);
    const bsz = dvv.getUint32(off + 4, true), csz = dvv.getUint32(off + 8, true);
    if (id === 'MAIN') { walk(off + 12 + bsz, off + 12 + bsz + csz); off += 12 + bsz + csz; continue; }
    if (id === 'nTRN' || id === 'nGRP' || id === 'nSHP') {
      let o = off + 12; const nid = dvv.getInt32(o, true); o += 4;
      const at = rdDict(o); o = at[1];
      const rec = { t: id, name: at[0]._name || '' };
      if (id === 'nTRN') rec.child = dvv.getInt32(o, true);   // …then reserved / layer / numFrames and the per-frame transform dicts, none of which this needs: the chunk header already says where the next chunk starts
      else if (id === 'nGRP') { const nc = dvv.getInt32(o, true); o += 4; rec.kids = [];
        for (let i = 0; i < nc; i++) { rec.kids.push(dvv.getInt32(o, true)); o += 4; } }
      else { const nm = dvv.getInt32(o, true); o += 4; rec.models = [];
        for (let i = 0; i < nm; i++) { const mi = dvv.getInt32(o, true); o += 4; const md = rdDict(o); o = md[1];
          rec.models.push([mi, md[0]._f === undefined ? i : +md[0]._f]); } }
      nodes.set(nid, rec); }
    off += 12 + bsz + csz;
  } };
  try { walk(8, pv.length); } catch (e) { return []; }
  if (!nodes.size) return [];
  const gather = (nid, ids, seen) => { const r = nodes.get(nid); if (!r || seen.has(nid)) return; seen.add(nid);
    if (r.t === 'nTRN') gather(r.child, ids, seen);
    else if (r.t === 'nGRP') { for (const k of r.kids) gather(k, ids, seen); }
    else for (const m of r.models.slice().sort((a, b) => a[1] - b[1])) ids.push(m[0]); };
  const out = [];
  const scan = (nid, seen) => { const r = nodes.get(nid); if (!r || seen.has(nid)) return; seen.add(nid);
    if (r.t === 'nTRN' && r.name) { const ids = []; gather(nid, ids, new Set()); if (ids.length) out.push({ name: r.name, ids }); return; }   // OUTERMOST name wins → never descend into a named animation looking for more
    if (r.t === 'nTRN') scan(r.child, seen); else if (r.t === 'nGRP') for (const k of r.kids) scan(k, seen); };
  scan(nodes.has(0) ? 0 : nodes.keys().next().value, new Set());
  return out; };
  let FROG_ITEM0 = 0, FROG_NFRAMES = 0;                // the frog's THREE cycles, loaded back to back so one item0 + one length addresses all of them
  let FROG_CYC = [];                                   // [{ off, n, move }] per cycle in strip order — hop MOVES the frog, ribbet and tongue play in place                // FROG, the 'hop' cycle out of assets/life/frog.vox (the file also holds 'ribbet' and 'tongue')
  let KOI_ITEM0 = 0, KOI_NFRAMES = 0;                  // KOI (assets/life/koi.vox) — swims the stage like the world's fish
  let FFLY_ITEM0 = 0, FFLY_NFRAMES = 0;                // firefly frames (assets/life/firefly) — replaces the butterflies AT NIGHT; the yellow abdomen voxel glows via the drops dYv.w lane
  let WORM_ITEM0 = 0, WORM_NFRAMES = 0;                // inchworm crawl frames (assets/life/worm) — a GROUND crawler, active day and night (pool slots 24+)
  let BETTA_FSP = -1;                                  // index into FISHES of the betta, or -1 if its frames never loaded. A LOAD-ORDER fact, captured in assets/held-items.js, never written as a literal
  const FISHES = [];                                   // every loaded FISH strip: { name, item0, n } (assets/life/<species>/00..NN.vox — salmon today, minnow etc. join automatically
  const BLINK_HAS = new Set();                        // item0 bases whose strip carries BLINK variants at +NFRAMES (assets/held-items.js eyeBlink).
                                                     // A creature NOT in here must never take the offset: it would read past its own strip into the next one's frames.
  const DESERTS = [];                                  // every loaded DESERT creature: { name, item0, n }. Frames are baked flat by tools/bake_desert_life.py from the scene-graph .vox — see the loader in held-items.js
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
  let STICK_BLOS_IT = 0;                               // the cherry forest's twig, held: stick_1 with a pink leaf. 0 when the blossom models never built
  let PETAL_IT = 0, PETALW_IT = 0, PETALG_IT = 0, PETALGL_IT = 0, PETALN_IT = 0, WORM_EAT0 = 0;   // the four FALLING LEAF voxels — pink off a pink tree, cream off a white one, GREEN off a plain oak (user 2026-08-19: "also apply this to the regular oak trees. green leaves should fall")
  let SPARK_IT = 0, HITRED_IT = 0, HITGOLD_IT = 0, KNIFE_IT = 0, ROCK_IT = 0, STICK_IT = 0, CONE_IT = 0, SMOKE_IT = 0, PICK_IT = 0, SHOVEL_IT = 0, BOW_IT = 0, BOW_NOCK = 0, BOW_FRAMES = 0, ARROW_IT = 0, MEAT_IT = 0, HOE_IT = 0, SPEAR_IT = 0, HEART_IT = 0;   // BOW_NOCK = the same strip WITHOUT the arrow, shown once it is loosed   // item ids of the clash spark, stone knife, held field-stone, held stick, held pinecone, DEATH SMOKE (the natural ones get AO+grain)
  let BLUEB_ITEM0 = 0, ROBIN_ITEM0 = 0, PINKB_ITEM0 = 0;               // the two songbird RESKINS in the item table (trace path). Identical geometry to the cardinal, so they reuse CARD_NFRAMES / CARD_OFF / CARD_FOOTZ untouched.
  let CARD_ITEM0 = 0, CARD_NFRAMES = 0, CARD_H = 0, CARD_FOOTZ = 1e9;   // cardinal ROTATE frames (assets/life/cardinal/rotate/00-10, base.vox ignored) → item table; ONE animated model STANDING on the asset-editor stage. CARD_H/CARD_FOOTZ plant the feet on the plane.
  let CARDINAL_ROTATE = [];                            // + the same frames' RAW bytes, auto-imported as the editable filmstrip on the editor stage (,/. select each frame) while the standing model previews the animation
  let BLUEBIRD_ROTATE = [];                            // BLUE BIRD = the cardinal model reskinned (assets/life/blue_bird/rotate/00-10, base.vox ignored) — same 11-frame rotate filmstrip, shown on the editor stage
  let PINKBIRD_ROTATE = [];                            // PINK BIRD = the cherry forest's own perched songbird (assets/life/pink_bird/rotate/00-10, base.vox ignored — split from it by tools/split_vox_frames.py). Same 11-frame layout as the cardinal, so it reuses CARD_OFF/CARD_FOOTZ untouched
  let ROBIN_ROTATE = [];                               // ROBIN = the SAME bird recoloured again (assets/life/robin/rotate/00-10, base.vox ignored) — same layout as the cardinal and blue bird
  let BUNNY_ROTATE = [];                               // ROTATE bunny frames (assets/life/bunny/rotate/00-10) — the RIGHT-lane editor object (jumps + rotates in place)
  let BUNNY_JUMP = [];                                 // JUMP bunny frames (assets/life/bunny/jump/00-10) — the LEFT-lane editor object (jumps FORWARD). Both show on the stage together (user).
  let BUNNY_ROTATE_RIGHT = [];                         // RIGHT-rotating bunny frames (assets/life/bunny/rotate/right/00-10) — authored right-turn .vox (rotation is IN the voxel data), user
  let ARMADILLO_WALK = [];                             // ARMADILLO walk frames (assets/life/armadillo/walk/00-07) — world creature; still loaded for the pine-forest armadillo (no longer the asset-editor default)
  let SKUNK_WALK = [];                             // SKUNK walk frames (assets/life/skunk/00-09) — the asset-editor object AGAIN (user 2026-08-19 removed the porcupine from the editor; the porcupine is still a WORLD creature)
  let ARMADILLO_ITEM0 = 0, ARMADILLO_NFRAMES = 0;      // (legacy trace path — the world armadillo now GRID-STAMPS instead, see ARMADILLO_POSES)
  let ARMADILLO_POSES = null, ARMADILLO_FOOTZ = 0;     // GRID-STAMP poses [frame][heading] (built lazily) — the world armadillo stamps into W exactly like the asset editor (identical box-centre + armOffset), so its alignment matches 1:1 (user)
  let FLAMINGO_ITEM0 = 0, FLAMINGO_NFRAMES = 0;        // FLAMINGO walk frames in the ITEM table (trace path) — parsed from raw .vox RGB like the other walkers, so it costs the 256-entry world palette nothing
  const FLAMINGO_WALK = [];                            // raw bytes of assets/life/flamingo/00..09.vox, split from flamingo.vox by tools/split_vox_frames.py
  // ── NO TREE IS STAGED IN THE ASSET EDITOR (user 2026-08-19: "remove the pine tree from the asset editor") ──
  // a BIRCH_VOX and then a FIR_VOX buffer lived here, each holding one decoration .vox's RAW BYTES for the
  // editor stage. Both are gone, along with the fetches in assets/held-items.js that filled them; the stage
  // falls through to the skunk (see ui/editor.js edEnter) — the PORCUPINE branch that headed the chain was
  // removed on 2026-08-19 too, but only from the EDITOR: the porcupine is still a world land mammal and every
  // PORCUPINE_* symbol below is still read by the world. game/assets/decoration/birch.vox and
  // fir_spruce.vox are deliberately KEPT on disk so `/spawn birch` and `/spawn fir_spruce` still resolve
  // through VOXDEX, which is a build-time walk of that folder.
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

