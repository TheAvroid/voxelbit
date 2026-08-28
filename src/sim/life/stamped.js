  // ── GRID-STAMPED CREATURES (Tasks 3 + 6) ── worms AND ducks are stamped into the world voxel grid so they pick up the SAME static SVGF lighting as terrain, instead of the analytic off-grid drop shading.
  // Worms animate their crawl (re-stamping the current frame) while moving + freeze on a pause; ducks are a single rigid pose floating at a fixed waterline. A creature re-stamps only when its cell / facing / frame changes, so a still one fully converges to crisp static lighting.
  const CREATURE_IDS = new Set();                      // palette ids the grid-stamped creatures write. NOT A NAVIGATION CONCEPT — it used to be the one bfObst/bfObstW consulted, and because the 256-entry palette is FULL and shared it aliased 84 of 256 ids, so a sixth of the world was a wall to the player and invisible to every animal; the nav probes now read solidTab (the player's own truth) and exclude stamps per-CELL via stampedIdx. This set survives only as the render/animation registry it always was — see CREA_FLAG, which is written beside it. (The worm/duck moving-life stamp experiment that once lived here was dead code and is REMOVED: those are TRACE-INJECTED rigid models now — see the DYNAMIC LIFE block in TRACE — with no grid writes at all.)
  // grid indices touched by PERCHED-BIRD (un)stamps this frame → one light gpuPatch at the loop's end.
  // POOLED: this used to be a plain Array push-ed thousands of times per frame and truncated with .length = 0,
  // which churns its backing store every frame. A grown-once Int32Array + count allocates nothing in steady state.
  const buriedTmp = [], snowCells = [], snowMeltCells = [];   // pooled scratch for the snow settle scan + this frame's snow voxel edits; snowMeltCells is the THAW's own list, uploaded with no support seed (a melted flake orphans nothing — see the melt block)
  const snowStopCols = [];                             // (column, newTop) pairs a landing computed EXACTLY — replayed after gpuPatch blanket-clears the scanTop cache. Pooled like the two above; see the restore in tick-snow.
  let wormPatch = new Int32Array(1 << 15), wormPatchN = 0;
  const wormPatchPush = (ii) => {
    if (wormPatchN >= wormPatch.length) { const g = new Int32Array(wormPatch.length * 2); g.set(wormPatch); wormPatch = g; }
    wormPatch[wormPatchN++] = ii;
  };
  // Per-creature stamp buffers, grown in place and REUSED. The old code built two fresh JS Arrays per re-stamp;
  // at 24 fps across ~180 perched birds + mammals that was the single largest allocator in the profile (~40% of
  // all per-frame garbage). B.sN is now the "is stamped" truth — B.sCells stays allocated between stamps.
  // sW: the WORLD coordinate of each landed cell, 3 per cell. sCells holds the flat WINDOW index, and the
  // window is toroidal — you cannot recover a world position from it. The RAGDOLL needs real world coords
  // (a rigid body is positioned in world space), so they are recorded as the stamp is laid down.
  const stampAlloc = (B, n) => { if (!B.sCells || B.sCells.length < n) { B.sCells = new Int32Array(n + 64); B.sPrev = new Uint8Array(n + 64); B.sW = new Int32Array((n + 64) * 3); } };
  // ── WHICH WORLD CELLS ARE A CREATURE RIGHT NOW ── an exact index of every voxel currently occupied by a
  // grid-stamped animal. cellStamped() cannot answer this: it only scans creatures within 20 voxels of the
  // PLAYER, which is right for "what am I standing on" and useless for a carve happening 50 voxels away.
  // The severed-voxel sweep needs the truth anywhere, or it treats a perched bird as detached terrain and
  // lifts the animal out of the tree as debris (user 2026-08-05: "chunks seem to break off of them").
  // ── OPEN-ADDRESSED, NOT A JS Set ── this is the hottest small structure in the game and it is hit from
  // both directions: nav asks has() about ~1M cells per field rebuild, and the perched songbirds CHURN it,
  // ~2,900 deletes + 2,900 adds every frame as they re-stamp (a re-stamp is an un-stamp then a stamp, and
  // each touches every cell of the bird). MEASURED in one session at 842 birds by no-op'ing just these two
  // calls: stampApply 1106 -> 209 ms over 10 s, i.e. the Set was 81% of it, 0.69 ms a frame, more than
  // every other thing the band costs put together.
  // Linear probing over one Int32Array. Slots hold key+1 so 0 can mean EMPTY (cell index 0 is a real cell),
  // and -1 is a TOMBSTONE. Same has/add/delete/size/clear surface the Set had, and nothing iterates it.
  // 1<<17 slots = 512 KB, against ~24k live entries: load stays near 0.18, so has() is one probe in the
  // common case. Tombstones are swept by a rehash when live+dead passes 60%, which at this churn is about
  // every 19 frames and costs one pass over the table.
  const stampedIdx = (() => {
    let cap = 1 << 17, mask = cap - 1, keys = new Int32Array(cap), n = 0, tomb = 0;
    const slotOf = (k) => {                            // >=0 : the slot holding k. <0 : ~slot to insert it into
      let i = (Math.imul(k, 0x9E3779B1) >>> 15) & mask, t = -1;
      for (;;) {
        const v = keys[i];
        if (v === 0) return t >= 0 ? ~t : ~i;          // hit EMPTY — the probe chain ends, so k is absent
        if (v === -1) { if (t < 0) t = i; }            // remember the first tombstone, but keep scanning for k
        else if (v === k + 1) return i;
        i = (i + 1) & mask;
      }
    };
    const rehash = (nc) => { const old = keys; cap = nc; mask = cap - 1; keys = new Int32Array(cap); tomb = 0;
      for (let i = 0; i < old.length; i++) { const v = old[i]; if (v > 0) keys[~slotOf(v - 1)] = v; } };
    return {
      has: (k) => slotOf(k) >= 0,
      add(k) { const s = slotOf(k); if (s >= 0) return; const i = ~s;
        if (keys[i] === -1) tomb--; keys[i] = k + 1; n++;
        if ((n + tomb) * 5 > cap * 3) rehash(n * 5 > cap * 2 ? cap * 2 : cap); },   // grow only if the LIVE set is what filled it; otherwise just sweep the tombstones
      delete(k) { const s = slotOf(k); if (s < 0) return false; keys[s] = -1; n--; tomb++; return true; },
      clear() { keys.fill(0); n = 0; tomb = 0; },
      get size() { return n; },
    };
  })();
  const unstampWorm = (B) => { if (!B.sN) return; const nc = B.sCells, pv = B.sPrev;   // (used ONLY by the perched songbirds — the sole grid-stamped life)
    for (let i = 0; i < B.sN; i++) { const ii = nc[i]; stampedIdx.delete(ii);
      if (W[ii] !== 0) { W[ii] = pv[i]; wormPatchPush(ii); }   // restore the overwritten foliage, not 0 — else the bird leaves a hole in the canopy
      // ── AND WAKE WHATEVER CAME TO REST ON THE BIRD (2026-08-08) ── the stamp patch is uploaded with
      // track=false, deliberately: a CONDUIT is not terrain and re-adjudicating the world every time a
      // perched bird shifts its feet would be ruinous. But that also means the un-stamp asks NOTHING, and a
      // cell whose previous id was AIR goes back to air with anything that settled on top of it still there.
      // Measured: a storm lays snow on a perched bird's back (scanTop reads W and a stamp is solid in W), the
      // bird leaves, and the snow hangs in the sky with nothing under it — no chopping involved, which is why
      // it read as "occasionally floating". Only air-restored cells can strand anything, and only when
      // something is actually sitting there, so this queues a handful of cells per departure, not a sweep.
      // The cell ABOVE is what gets seeded rather than the vacated one: supPush's cleared-cell branch would
      // mark the whole column carved and cost every later anchor query a scan, for the same answer.
      // ── AND IT ASKS ONLY ABOUT SNOW ── this is a HOT path: stampApply re-stamps on every pose change, i.e.
      // at the 24 fps animation rate, across ~180 stamped creatures of ~150 voxels each. Waking on "anything
      // above" therefore offered the resolver something like 400k cells/second — most of them a needle above a
      // bird standing in a crown, which was never resting on the bird at all. Snow is the ONLY thing in the
      // game that comes to rest on top of a creature, so snowTab is both the exact question and a plain array
      // read. With the landSnowAt refusal above it should now never fire; it is what makes that guarantee
      // hold for a blanket that predates it, and it costs one indexed load per air cell.
      if (!pv[i]) { const up = ii + WX; if (up < W.length && snowTab[W[up]]) supPush(up); }
    }
    B.sN = 0; B.sKey = null; };
  const stampApply = (B, pose, gx, gy, gz, key) => {   // shared: re-stamp only on change; only into EMPTY AIR so un-stamp is a safe →0
    if (B.sKey === key && B.sN) return;
    unstampWorm(B);
    // A perched bird sits INSIDE the pine crown, so much of its body lands on needles. Writing only into empty air
    // dropped every one of those voxels and left birds with holes punched through them (42% of them, worst cases
    // losing a third of the model). Foliage is now overwritten and the PREVIOUS id is remembered per cell, so the
    // un-stamp restores the needles instead of leaving a bird-shaped hole in the tree. Anything else — trunks, rocks,
    // another bird's stamp — still blocks, so nothing structural is ever eaten.
    stampAlloc(B, pose.length);
    const nc = B.sCells, pv = B.sPrev; let k = 0;
    let sb0 = 1e9, sb1 = 1e9, sb2 = 1e9, sb3 = -1e9, sb4 = -1e9, sb5 = -1e9;
    // ── THE TOROIDAL WRAP IS PER-STAMP, NOT PER-CELL ── gwrap is ((v % n) + n) % n, i.e. TWO modulos, and
    // this ran it twice for every voxel of every re-stamp: ~97 birds a frame x 28 cells x 4 modulos. The
    // creature's anchor is a WORLD coordinate (±200k, so the wrap is real work), but the pose offsets are
    // tiny — a songbird's whole stamp is a 6x6x3 box — so wrapping the ANCHOR once and then nudging by the
    // offset with a compare is exactly equivalent for any |offset| < WX. Same index, same cells.
    const bx9 = gwrap(gx, WX), bz9 = gwrap(gz, WZ), WXY9 = WX * WY;
    for (let q = 0; q < pose.length; q++) { const p = pose[q]; const wy = gy + p[1]; if (wy < 1 || wy >= WY - 1) continue;
      let ax9 = bx9 + p[0]; if (ax9 < 0) ax9 += WX; else if (ax9 >= WX) ax9 -= WX;
      let az9 = bz9 + p[2]; if (az9 < 0) az9 += WZ; else if (az9 >= WZ) az9 -= WZ;
      const ii = ax9 + wy * WX + az9 * WXY9;
      const cur = W[ii];
      if (cur !== 0 && !foliaTab[cur]) continue;   // trunks, rocks and other stamps still block; a voxel lost inside the branch is hidden by it anyway
      const wx = gx + p[0], wz = gz + p[2];             // WORLD bounds of what actually landed — the knife's hit flash boxes exactly this and nothing around it
      B.sW[k * 3] = wx; B.sW[k * 3 + 1] = wy; B.sW[k * 3 + 2] = wz;   // …and the world coord itself, for the ragdoll (see stampAlloc)
      pv[k] = cur; W[ii] = p[3]; nc[k] = ii; k++; wormPatchPush(ii); stampedIdx.add(ii);
      if (wx < sb0) sb0 = wx; if (wx > sb3) sb3 = wx;
      if (wy < sb1) sb1 = wy; if (wy > sb4) sb4 = wy;
      if (wz < sb2) sb2 = wz; if (wz > sb5) sb5 = wz;
    }
    B.sN = k; B.sKey = key; B.sWant = pose.length;
    if (k) { if (!B.sB) B.sB = new Float32Array(6);
      B.sB[0] = sb0; B.sB[1] = sb1; B.sB[2] = sb2; B.sB[3] = sb3; B.sB[4] = sb4; B.sB[5] = sb5; }
  };
  const stampOrient = (th) => { const Hx = Math.sin(th), Hz = Math.cos(th); return Math.abs(Hz) >= Math.abs(Hx) ? (Hz >= 0 ? 0 : 2) : (Hx >= 0 ? 1 : 3); };   // nearest cardinal
  // ── GRID-STAMPED PERCHED CARDINAL (matches the editor exactly) ── built from the SAME rotate .vox via edParseVox + edRotVox + the same baked offsets, so its grid lighting/rotation/alignment are identical to the asset-editor cardinal (user)
  let CARD_POSES = null, BLUE_POSES = null, ROBIN_POSES = null;
  const CARD_OFF = { '04.vox': [0, 1, 0], '05.vox': [0, 1, 0] };   // baked per-frame alignment (user's copy-changes) — same table the editor uses
  const buildBirdPoses = (rotate) => {                  // [frame][orient0..3] pose lists from raw rotate .vox bytes — cardinal OR its BLUE reskin (identical geometry, so same offsets/FOOTZ)
    const frames = [];
    for (const f of rotate) { try { frames.push(...edParseVox(f.u8, f.name)); } catch (e) {} }
    if (!frames.length) return null;
    const poses = frames.map((f) => { const o = CARD_OFF[f.name] || [0, 0, 0], orients = [];
      for (let q = 0; q < 4; q++) { const rv = edRotVox(f.vox, f.sx, f.sy, q), cx = rv.sx >> 1, cy = rv.sy >> 1, list = [];
        for (const p of rv.vox) { const id = p >>> 24; list.push([(p & 255) - cx, ((p >> 16) & 255) + o[1], ((p >> 8) & 255) - cy, id]); CREATURE_IDS.add(id); CREA_FLAG[id] = 1; }   // dx centred, dy = model-z (height) + oy, dz centred (ox/oz are 0)
        orients.push(list); }
      return orients; });
    palSync();                                          // edParseVox appended the bird's colors via edCol
    return poses;
  };
  const ensureCardPoses = () => { if (!CARD_POSES && CARDINAL_ROTATE.length) CARD_POSES = buildBirdPoses(CARDINAL_ROTATE); return CARD_POSES; };
  const ensureBluePoses = () => { if (!BLUE_POSES && BLUEBIRD_ROTATE.length) BLUE_POSES = buildBirdPoses(BLUEBIRD_ROTATE); return BLUE_POSES; };   // blue bird = the cardinal reskinned, scattered the SAME way (user)
  const ensureRobinPoses = () => { if (!ROBIN_POSES && ROBIN_ROTATE.length) ROBIN_POSES = buildBirdPoses(ROBIN_ROTATE); return ROBIN_POSES; };   // robin = the same bird recoloured again — third colour in the same rotation
  let PINK_POSES = null;
  const ensurePinkPoses = () => { if (!PINK_POSES && PINKBIRD_ROTATE.length) PINK_POSES = buildBirdPoses(PINKBIRD_ROTATE); return PINK_POSES; };   // …and the cherry forest's own, built the same way from the same 11-frame layout
  const ensureBirdPoses = (c) => (c === 3 ? ensurePinkPoses() : (c === 2 ? ensureRobinPoses() : (c === 1 ? ensureBluePoses() : null))) || ensureCardPoses();   // any missing reskin quietly falls back to the red cardinal
  // DOUBLED (user 2026-07-18): 340 -> 680. The pool is the real limit before the radius is — at this density 680
  // would hold ~300 birds against 180 slots, so the activation frontier lands near 530 vox rather than 680, and the
  // generated rect (renderDist + 96) caps it from there. Declared at module scope because stampCardinal below needs it.
  const birdColour = (tx, tz, bi) => {              // 0 cardinal / 1 blue bird / 2 robin — a pure function of the TREE (pine or oak), so a census can sample it without spawning anything
    // LOCALLY EVEN (user). The old 40-vox `(x + 2z) % 3` colouring was even world-wide but not near you: a
    // diagonal 3-colouring lays out long same-colour BANDS, so any one view skewed (64/49/67 of 180 measured).
    // Index the pine's own candidate CELL instead — exactly one pine per cell — and colour it from a 3×3 LATIN
    // SQUARE, which contains each colour exactly three times. Every 3×3 patch of cells is therefore even by
    // construction, and rotating/flipping each patch independently stops the tiling reading as one motif.
    // Balance the BIRDS, not the cells: a 3x3 Latin square over cells still skewed, because a pine carries
    // 0-3 birds, so an even spread of cells is not an even spread of birds. Instead enumerate every bird in
    // this pine's 3x3 patch of candidate cells in a fixed order and deal the three colours round-robin, which
    // makes each patch exactly even however the birds are distributed across its pines. The per-patch rotation
    // and stride (both coprime to 3, so balance is preserved) stop it reading as one repeating sequence.
    // ── ONE ROUND-ROBIN, TWO FORESTS (user 2026-08-17: "make the song birds perched in the oak trees") ── every
    // word of the argument above is about a 3x3 patch of CANDIDATE CELLS and the birds inside it, and none of it
    // is about pines, so the whole of it is lifted out here and handed the grid it should walk. Running an OAK
    // through the pine grid would have looked like it worked and been meaningless: TCELL is 45 against OKCELL's
    // 112, so a 3x3 pine patch is 135 voxels across where one oak cell alone is 112, and treeAt returns null
    // everywhere in this biome — every oak would have scored seq = 0 and been coloured by bi alone, i.e. the
    // per-tree sequence with no neighbourhood balancing at all, which is exactly the skew this replaced.
    const oak = oakM(tx, tz) >= 0.5;                   // >= 0.5 is the SAME line oakAt and treeAt split on, so a tree is always asked about on the grid that actually produced it
    // -- ...AND A THIRD GRID FOR THE BIRCH -- the argument above is about a 3x3 patch of CANDIDATE CELLS, and the
    // cell size is the whole of what a biome changes. Running a birch through the PINE grid, which is what this
    // did before, looks like it works and is meaningless: treeAt returns null across the whole band, so every
    // birch scored seq = 0 and was coloured by bi alone - the per-tree sequence with no neighbourhood balancing
    // at all, which is exactly the skew the round-robin exists to remove.
    const bir = !oak && birchM(tx, tz) >= 0.5;         // the same halfway line birchAt itself splits on
    const bc = oak ? birdColourOn(tx, tz, bi, OKCELL, oakAt, (t) => birdsOnOak(t.wx, t.wz, t.k))
                   : bir ? birdColourOn(tx, tz, bi, BKCELL, birchAt, (t) => birdsOnBirch(t.wx, t.wz, t.k))
                   : birdColourOn(tx, tz, bi, TCELL, treeAt, (t) => birdsOnPine(t.tx, t.tz));
    // ── AND THE BLOSSOM IS ONE SPECIES, NOT A QUARTER OF FOUR (user 2026-08-18) ── the round-robin above balances
    // three colours across a 3x3 patch, and the cherry forest wants none of that: it wants EVERY perched bird to
    // be the pink one. So the balancing still runs (it decides nothing here) and its answer is overridden, rather
    // than the pink bird being dealt into the rotation — dealing it in would have put pink birds in the oak wood
    // and cardinals in the blossom, which is the opposite of what was asked for both ways round.
    // Asked of the TREE's own position, the same coordinate the whole function is a pure function of, so a census
    // can still sample it without spawning anything.
    // ── AND THE TEST IS THE TREE'S OWN, 0.5, NOT 0.15 (user 2026-08-18: "Pink birds are in oak trees ... seems to
    // only be in neighboring oak trees next to the cherry trees") ── which is exactly what a 0.15 here produced.
    // `blos` in world/terrain.js decides whether a tree is a BLOSSOM tree at cherryM > 0.5, so every tree in the
    // 0.15..0.5 blend is a green oak — and this line was putting pink birds in all of them. One species, two
    // thresholds, and the gap between them was visible from the ground. The bird now keys on the same number the
    // TREE does, so a pink bird sits in a pink crown and nowhere else, by construction rather than by tuning.
    if (PINKBIRD_ROTATE.length && cherryM(tx, tz) > 0.5) return 3;
    return (bc === 1 && !BLUEBIRD_ROTATE.length) || (bc === 2 && !ROBIN_ROTATE.length) ? 0 : bc;   // a missing reskin gives its share back to the cardinal
  };
  const birdColourOn = (tx, tz, bi, cell, at, count) => {   // the patch walk itself — grid size, tree accessor and per-tree bird count are the only things a biome changes
    const cx = Math.floor(tx / cell), cz = Math.floor(tz / cell);
    const sx = Math.floor(cx / 3), sz = Math.floor(cz / 3);
    const off = (ihash(sx * 37 + 11, sz * 53 + 7) * 3) | 0;
    const stride = ihash(sx * 71 + 5, sz * 29 + 13) < 0.5 ? 1 : 2;
    let seq = 0, mine = -1;
    for (let jz = 0; jz < 3; jz++) for (let jx = 0; jx < 3; jx++) {
      const ccx = sx * 3 + jx, ccz = sz * 3 + jz;
      if (ccx === cx && ccz === cz) { mine = seq + bi; }
      const tr = at(ccx, ccz); if (!tr) continue;
      seq += count(tr);
    }
    return (((mine < 0 ? bi : mine) * stride + off) % 3 + 3) % 3;
  };
  const birdsOnPine = (tx, tz) => {                   // 0-3, a deterministic property of the tree. HALVED (user 2026-07-18):
    const h = ihash(tx * 0x9E37 + 13, tz * 0x85EB + 7);   // mean 0.83 -> 0.41 birds per pine (was .42/.80/.95), then 0.41 -> 0.63
    return h < 0.55 ? 0 : (h < 0.86 ? 1 : (h < 0.96 ? 2 : 3));   // …+50% when the ROBIN joined, so the third colour is ADDED on top instead of thinning the other two (user). The population is perch-limited, not slot-limited: nCard alone could not lift it past ~130.
  };
  // ── HOW MANY BIRDS AN OAK CARRIES ── birdsOnPine's table cannot simply be reused, because it is a constant
  // that only reads as one because every pine in the game IS ONE MODEL. oakAt deals SEVEN sizes, from a 2.4 m
  // bush to an 11.4 m giant whose crown box is 114 voxels across against pine5's 35 — a flat 0-3 would put as
  // many birds in a shrub as in a tree ten times its size. So tie the count to the CROWN FOOTPRINT at the
  // pine's own birds-per-area-of-canopy, and the number falls out of the bake: a size tier can be added or
  // re-weighted in world/terrain.js and this cannot go stale behind it.
  //   The 0.63 is birdsOnPine's OWN expectation, read straight off its own thresholds:
  //   0.31*1 + 0.10*2 + 0.04*3 = 0.63 birds over pine5.vox's 35 x 36 crown box. Change that table and change
  //   this number with it. Against the bake it deals k1 0.97, k2 0.94, k3 1.83, k4 2.62, k5 5.05, k6 6.38.
  // MEASURED CONSEQUENCE, AND IT IS DELIBERATE: an oak wood's canopy covers ~28% of the ground (OKCELL 112 at
  // 78% occupancy over a mean 4481-voxel crown box) against the pine wood's ~45% (TCELL 45 at 72% over 1260),
  // so this lands the oak forest at 55% of the pine forest's BIRD density: 1.99 birds per oak, 1.24e-4 per
  // vox², which is ~109 birds inside the rect-clipped ~530 frontier against the pines' 198 — i.e. ~109 of the
  // 180 slots filled rather than all 180. (At a CARD_KEEP of 680 it reaches 179 — which is exactly the census CARD_N is now sized off; see the ladder in slots.js. So the oak population
  // is REACH-limited, not tree-limited: raise the view distance and it saturates the pool exactly as the pines
  // do.) Matching the pine density head-on at the default view would need ~10
  // birds in a single giant oak, which is the roost CARD_PER_TREE was introduced to refuse ("unlimited clumped
  // them and the spread measured worse"). A more open wood holding fewer birds is the honest answer; if the
  // biome wants to read denser, the lever is OKCELL in terrain.js, not more birds per tree.
  const CARD_OAK_CAP = 6;                             // ceiling per oak: 2x the pines' 3, on a crown 3.3x as wide, so even a maxed giant is SPARSER per branch than a pine
  const birdsOnOak = (wx, wz, k) => {
    const m = OAKV[k]; if (!m || !MSX) return 0;      // ?nooaks / a failed bake, or pine5.vox never parsed (MSX 0 would divide by zero)
    // ── AND NOTHING PERCHES IN THE BLOSSOM (user 2026-08-18: "dont spawn any songbirds in the trees") ──
    // asked HERE, at the per-tree count, rather than at the spawn gate, because the perched birds are the one
    // life path that never reaches it: buildCardCand walks the tree grids directly. Zero birds on a tree means
    // buildCardCand offers no candidate there, findPineCrown returns null, and the slot simply stays unplaced
    // — the same shape as standing in the desert. The count is also what __vb.birdCensus reports, so a census
    // taken in the cherry forest tells the truth for free.
    // ── AND THE BLOSSOM GETS ITS BIRDS BACK (user 2026-08-18, reversing the same day's refusal) ── this returned
    // 0 here while the cherry forest had no bird of its own. It has one now, so the tree keeps its normal count
    // and birdColour below decides the SPECIES instead. Nothing about the count is biome-specific any more.
    if (PINKBIRD_ROTATE.length === 0 && cherryM(wx, wz) > 0.5) return 0;   // 0.5, matching the tree: at 0.15 a wide ring of ORDINARY green oaks around the blossom lost its birds too   // …except with no pink art loaded, in which case the blossom would fill with cardinals — better empty than wrong
    // ── AND NOTHING PERCHES IN THE BUSH TIER ── OAKV[0] is 21 voxels tall, and stampOak sinks it 1-3, so its
    // crown top lands about 18 voxels up: the player's own eye line (sim/player.js EYE 18.5, HEIGHT 20). A
    // perched bird is not decor — it is a SOLID grid stamp — so a bird there is a body you walk face-first
    // into on open forest floor, and at 22% of all oaks it would be a lot of them. It would not read as
    // "perched in a tree" either; it reads as a bird standing in the undergrowth. Asked as a question about
    // the MODEL against the PLAYER rather than written as `k === 0`, so re-baking the tool with a different
    // smallest tree cannot leave this test pointing at the wrong index.
    if (m.sz - 3 < HEIGHT) return 0;
    const mean = 0.63 * (m.sx * m.sy) / (MSX * MSY);
    return Math.min(CARD_OAK_CAP, Math.round(mean * (0.5 + ihash(wx * 0x7F4A + 29, wz * 0x6C08 + 17))));   // its OWN salt, not birdsOnPine's: two scatters keyed alike rank the world alike (see the mammals' one-key-per-species finding). The 0.5..1.5 roll preserves the mean exactly and still leaves the small tiers sometimes empty.
  };
  // -- AND HOW MANY A BIRCH CARRIES -- the same expectation the oak uses, against the same pine reference, so a
  // crown of a given footprint carries the same number of birds whichever forest it grew in (user 2026-08-24:
  // "the life in the birch forest should match the life in the oak forest"). The birch forest arrives at its
  // population from the other direction: BKCELL is 44 against OKCELL's 112, so there are far more trees and
  // each carries about one bird, where an oak wood is a few big crowns carrying several each.
  // No cherry test and no bush-tier test - neither the blossom nor a 21-voxel shrub exists in this band - but
  // the height guard stays, because it is a statement about the PLAYER's eye line, not about the model list.
  // Its own salt, like every other scatter in the game: two keyed alike rank the world alike.
  const birdsOnBirch = (wx, wz, k) => {
    const m = BIRCHV[k]; if (!m || !MSX) return 0;
    if (m.sz - 3 < HEIGHT) return 0;
    const mean = 0.63 * (m.sx * m.sy) / (MSX * MSY);
    return Math.min(CARD_OAK_CAP, Math.round(mean * (0.5 + ihash(wx * 0x51E3 + 41, wz * 0x2C9F + 13))));
  };
  let uniBirdN = 0, uniBirdWant = 0;                   // how many perched songbirds reached a drop slot last frame, and how many asked
  const uniBirds = [];                                 // ?uni: perched songbirds staged as [wx, wy, wz, item, th, poolIdx, dp2] and injected into drop slots after the fair-share emit
  const birdItem0 = (c) => (c === 3 ? (PINKB_ITEM0 || CARD_ITEM0) : (c === 2 ? (ROBIN_ITEM0 || CARD_ITEM0) : (c === 1 ? (BLUEB_ITEM0 || CARD_ITEM0) : CARD_ITEM0)));   // …and the PINK arm, or a blossom bird stamps pink beyond the trace radius and draws RED inside it   // a missing reskin quietly falls back to the red cardinal, exactly as ensureBirdPoses does on the grid path
  const uniCor = (n) => n / 2 - (n >> 1);              // 0 for an even model dimension, 0.5 for an odd one: the grid stamp centres on (n >> 1) whole voxels while the emit centres on n / 2, and this is the whole difference between them
  const uniBird = (B, wk, fiC, q, gx, gy, gz) => {     // the same anchor + frame the grid stamp would have used, expressed for the trace path (derivation in the patch header)
    const it0 = birdItem0(B.bird | 0); if (!it0 || !itemsRef) return;
    const dm = itemsRef[it0 - 1 + fiC]; if (!dm) return;
    const oy = (CARD_OFF[(fiC < 10 ? '0' : '') + fiC + '.vox'] || [0, 0, 0])[1];   // the same baked per-frame lift buildBirdPoses applies
    const dx9 = B.x - P.x, dz9 = B.z - P.z;
    const t9 = (window.__UNITH0 === undefined ? 2 : window.__UNITH0) * (Math.PI / 2) + q * (window.__UNISGN === undefined ? 1 : window.__UNISGN) * (Math.PI / 2);
    uniBirds.push([gx + ((q & 1) ? uniCor(dm.d) : uniCor(dm.w)), gy + oy + dm.h * 0.5, gz + ((q & 1) ? uniCor(dm.w) : uniCor(dm.d)),
                   it0 + fiC, t9, wk, dx9 * dx9 + dz9 * dz9, (window.__UNIMIR === undefined ? -1 : window.__UNIMIR)]);   // dims SWAP on odd q because edRotVox swaps them; th and the -1 handedness are derived, not fitted
  };
  // == THE PERCHED-SONGBIRD DISTANCE HYBRID (?uni) == 180 perched songbirds competing for the 128 drop
  // slots is what starved everything else: measured under ?uni with every bird traced, flyers fell to
  // 4 of 13, worms 3 of 9 and fish 6 of 32. Beyond UNI_BIRD_R a bird is grid-stamped and takes no drop
  // slot at all; inside it, it trace-injects like every other creature. This is a DISTANCE hybrid, not a
  // return to two conventions - every bird you can meaningfully see is traced.
  //
  // Why grid-stamping is allowed here when it was rejected for the ducks and worms: what jittered there
  // was the BODY MOVING THROUGH THE WORLD, snapping voxel by voxel. A perched bird's anchor never moves.
  // It does still animate - stampCardinal steps frame and 4-way turn at 24 fps out to CARD_KEEP (680) -
  // so a far bird re-stamps exactly as often as it does in the build we ship TODAY, which makes the
  // hybrid strictly cheaper on the CPU than today rather than free. Raising DROP_SLOTS instead was
  // rejected: it moves the same cost onto the trace loop, for every pixel.
  // Now also the MAMMAL radius (user 2026-08-06): 17 traced mammals ate the whole 17-slot headroom and left FISH at 24/32. Beyond this radius a mammal grid-stamps,
  // which is exactly what ships TODAY, so a distant mammal is no worse than it is now and a near one is strictly better. Near/far, not traced/not-traced.
  const UNI_BIRD_R = LIFE_DRAW, UNI_BIRD_HYST = 24;   // = 420, and NOT a tuned number: LIFE_DRAW is the radius past which the allocator draws nothing at all, so tracing beyond it spends a slot on a creature that cannot be seen. Measured sweep at this spawn: r=120 traced 0 mammals (they spawn out near the bird keep radius), r=400 traced 8 with every other kind still at full, r=1000 traced 18 and cost FISH 9 draws.   // trace radius in voxels, and the band a bird must re-cross to switch back - without it one sitting exactly on the boundary flips path as the player breathes
  const uniTraced = (B) => {                    // window.__UNIBR overrides the radius live (0 = all grid, 1e9 = all trace) so the boundary can be swept in a single boot
    const r9 = (window.__UNIBR === undefined ? UNI_BIRD_R : window.__UNIBR), rr9 = B.uniTr ? r9 + UNI_BIRD_HYST : r9;
    const dx9 = B.x - P.x, dz9 = B.z - P.z, t9 = dx9 * dx9 + dz9 * dz9 < rr9 * rr9;
    B.uniTr = t9 ? 1 : 0; return t9;
  };
  // ── CARD_KEEP MOVED (2026-08-17) ── the perched songbirds' despawn radius, and via MAM_KEEP the land
  // mammals' too, now lives in sim/life/slots.js beside the slot ladder, because it is what SIZES that ladder:
  // CARD_N = round(180 * LIFE_DENS_K) and the four mammal counts are all derived from it. The reasoning for
  // the value, and for why a reach change forces a population change, is in the comment there.
  const LAST_PICK = { body: 0, leaf: 0, it: 0 };      // what the last twig pickup actually removed, for the test tap in main/debug-api.js
  const CARD_SEP = 8;                                  // minimum gap between two perched birds, in voxels — must exceed the model footprint or their grid stamps collide
  // ── HOW FAR A PERCHED BIRD ANIMATES ── deliberately the FULL keep radius: the user rejected a short one
  // outright ("it doesn't look like all the birds are animated"), and the last time it was a separate number it
  // was 90 voxels. It is a NAME rather than CARD_KEEP inlined because it is the one lever that would pay for
  // the CARD_N growth outright if the frame time ever asks for it: re-stamping is what a perched bird costs,
  // it is paid only by birds that animate, and dropping this to CARD_KEEP_V0 would hold the re-stamp budget at
  // exactly today's while the population still fills the new disc — the birds beyond 680 did not exist
  // yesterday, so making them static decor regresses nothing that ships. NOT taken without the user's say-so:
  // it is a visual change, and this is the exact complaint they have already made once.
  const CARD_ANIM_R = CARD_KEEP;
  const CARD_REST_MS = 25;                                 // the rest held on EVERY perched-bird frame, on top of the authored 24 fps (see stampCardinal)
  const stampCardinal = (B, now, wk9) => {                   // stamp the current rotate frame at the perch — 24 fps + 600 ms hold, EXACTLY the editor's timing (user chose grid @ 24 fps to match the editor; the brief grid-stamp AO shimmer during each ~0.46 s rotation burst is the accepted trade-off)
    const cp = ensureBirdPoses(B.bird | 0); if (!cp) return;   // perched birds come in THREE colours (B.bird: 0 red cardinal, 1 blue bird, 2 robin)
    // ── AND NOTHING HOLDS ON THE LAST FRAME ANY MORE (user 2026-08-27: "the perched song birds are not
    // playing correctly") ── the header above still says this is "EXACTLY the editor's timing", and it was
    // when it was written. The stage changed underneath it on 2026-08-22 ("when I drag in a animation
    // sequence, have it play the frames continously. right now its plays till the end pauses, then plays the
    // frames again") and its pauseMs went to 0; this copy of the number never followed. So the bird ran its
    // cycle and then froze on its last frame for 600 ms, over and over — six tenths of every second and a bit
    // spent as a statue, which is what a bird that is not playing correctly looks like.
    // Same divergence the frog's rest turned out to be, and the same fix: take the stage's number.
    // ── A SMALL REST BETWEEN FRAMES (user 2026-08-27: "add a small rest in between frames of the song bird")
    // ── the eleven frames ARE a quarter turn, so back to back at 24 fps the bird came round every 1.83 s: a
    // continuous spin with no beat in it. The rest CANNOT go at the cycle boundary — that is pauseMs, and a
    // hold there froze the bird on frame 10 before each hand-off, which is the "not playing correctly" report
    // the note above records. So it goes on EVERY frame equally: each pose is held frameMs + CARD_REST_MS, the
    // hand-off frame is held no longer than any other, and the rotation still runs on without a stutter — it
    // just steps rather than sweeps. The frames themselves are still the authored 24 fps; the rest is dead
    // time appended to each, which is what "a rest between frames" is. 25 ms makes a pose 1/15 s and a full
    // revolution 2.93 s.
    const n = cp.length, frameMs = 1000 / 24, stepMs = frameMs + CARD_REST_MS, cyc = n * stepMs;
    const near = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z) < CARD_ANIM_R * CARD_ANIM_R;   // EVERY active bird animates (user: 'it doesn't look like all the birds are animated'). This was 90 while the population was double; halving it bought back the re-stamp budget.
    const t = (now + B.phase) % cyc;
    const fiC = (near && !window.__CARDPIN) ? Math.min(n - 1, Math.floor(t / stepMs)) : 0;   // every frame gets the same hold — see CARD_REST_MS
    // ── THE 4-WAY TURN IS A FACING, NOT A PIROUETTE (user 2026-08-27: "Im seeing perched cardinals flicker
    // ... its dissapearing and reapearing quickly") ── this was a function of TIME, so it advanced one quarter
    // turn at every cycle boundary: MEASURED on a perched bird, 19 spins in 20 seconds, one a second, for ever.
    // A 28-voxel bird that flips its whole silhouette inside a single frame, once a second, is exactly what
    // reads as vanishing and coming back — and it is the only large instantaneous change in its rendering.
    // Everything else about the bird measured clean: no respawns, no re-perching, never unstamped, never
    // gutted, stamp voxels steady at 25-31 across 2,400 frames, and the GPU patch flushes every frame.
    // The spin came from the editor's turntable, where showing a model from four sides is the whole point of
    // a preview. On a perch it is not behaviour, so it becomes what it should have been out here: a fixed
    // FACING, taken from the perch itself so the flock still faces four different ways and each bird keeps
    // one direction for as long as it sits there. A bird that re-perches elsewhere gets a new facing.
    // ── THE TURN IS THE ANIMATION, AND q IS WHAT MAKES IT ENDLESS (user 2026-08-27: "the birds are supposed
    // to play frames 0-10. then where the last frame ended, the 00 frame plants itself in the same position.
    // thus creating an endless rotation") ── so this was never a spin bolted onto a perched bird: the eleven
    // frames ARE a quarter turn, and advancing q by one at each cycle boundary re-seats frame 00 at the
    // orientation frame 10 finished in. Played back to back that is one continuous rotation, which is the
    // whole effect. I took it for a once-a-second snap and replaced it first with a fixed facing and then with
    // an occasional glance; both broke the thing it was doing. Restored to the original expression.
    // It matters that pauseMs is 0 above: with the old 600 ms hold the bird froze on frame 10 before each
    // hand-off, so the rotation stuttered once a cycle instead of running on.
    const q = (near && !window.__CARDPIN) ? ((-(Math.floor((now + B.phase) / cyc) % 4)) & 3) : 0;        // GRID-ALIGNED 4-way spin = the editor's edRotVox(−spin)
    const gx = Math.round(B.x), gz = Math.round(B.z), gy = Math.round(B.perchFeet) - CARD_FOOTZ;   // feet (model z = CARD_FOOTZ) land on the crown needle
    // ── IS THE PERCH STILL THERE? ── the crown it was assigned can be chopped or felled between the
    // perch being chosen and this stamp; without this the bird stamps into thin air and hangs there.
    //
    // TWO THINGS THIS HAS TO GET RIGHT, both learned the hard way (life CPU 0.47 -> 13.1 ms):
    //   · the cell under the feet is frequently part of the bird's OWN stamp — the model extends below
    //     perchFeet by CARD_FOOTZ — and a bird voxel is neither foliage nor solid. Reading W naively
    //     failed for most birds and recycled the whole population EVERY FRAME. Consult sPrev, which
    //     remembers what the stamp covered, so the test sees the crown and not the bird.
    //   · it does not need to run at frame rate. A perch disappears when a tree is chopped, which is
    //     rare; twice a second is instant to a player and costs nothing.
    if (now > (B.perchChk || 0)) {
      B.perchChk = now + 400 + Math.random() * 300;
      // ONE cell under the feet was too strict — a bird does not always sit squarely on a needle, and
      // every false negative is a full recycle: it teleports to another pine and its animation restarts
      // (user). Ask what this check is actually for instead: is there ANY crown left underneath? A
      // felled tree removes all of it; an off-by-one perch does not.
      const pf0 = Math.round(B.perchFeet);
      const nc = B.sCells, pv = B.sPrev, sn = B.sN | 0;
      let support = false;
      for (let dy = 1; dy <= 3 && !support; dy++) {
        const py = pf0 - dy; if (py < 1) break;
        for (let dz = -1; dz <= 1 && !support; dz++) for (let dx = -1; dx <= 1; dx++) {
          const ii = gwrap(gx + dx, WX) + py * WX + gwrap(gz + dz, WZ) * WX * WY;
          let u = W[ii];
          if (sn) { for (let q = 0; q < sn; q++) if (nc[q] === ii) { u = pv[q]; break; } }   // see past this bird's OWN stamp to what the crown had there
          if (u && (foliaTab[u] || solidTab[u] === 1)) { support = true; break; }
        }
      }
      if (!support) {
        if (B.sN) unstampWorm(B);
        B.init = false;                                // nothing left underneath at all → back to the population loop
        return;
      }
    }
    if (LIFE_UNI && UNI_BIRDS && uniTraced(B)) { if (B.sN) unstampWorm(B); uniBird(B, wk9, fiC, q, gx, gy, gz); return; }   // ?uni: same frame, same 4-way turn, same anchor - injected instead of stamped. Forked HERE, below the perch-support check, so both paths recycle a bird whose crown was felled on exactly the same rule. The unstamp is what makes the hybrid safe to cross INWARD: without it the bird would be drawn twice, once out of W and once out of its slot.
    stampApply(B, cp[fiC][q], gx, gy, gz, gx + ',' + gy + ',' + gz + ',' + fiC + ',' + q);
  };
  const unstampAllWorms = () => { for (let j = DUCK_0; j < DES_END; j++) { const B = wbf[j]; if (B && B.sN) {   // ALL grid-stamped creatures: perched songbirds (CARD_0..CARD_END) + BUNNIES/ARMADILLOS/SKUNKS/PORCUPINES (MAM_0..MAM_END). Must cover the mammals too, or their stamps get unstamped one-by-one AFTER the editor's bricks.fill(0) and re-patch as floating chunks (user bug)
    for (let i = 0; i < B.sN; i++) { const ii = B.sCells[i]; stampedIdx.delete(ii); if (W[ii] !== 0) W[ii] = B.sPrev[i]; }   // restore, same reason as unstampWorm
    B.sN = 0; B.sKey = null; } } };   // hard clear (editor freeze) — caller re-uploads occupancy
  let lakeScanT = 0; const lakeSpots = [];             // detected LAKES in view (clusters of wide-open-water samples) — each gets its own duck family
  const waterSpots = [];                              // detected WATER of any width — lakes AND rivers. Dragonflies home here; ducks still use lakeSpots (a river is too narrow for a family)
  const BIRD_SPD = 55, BIRD_ALT = 140, BIRD_FLAP = 24, BIRD_VS = 1.0;   // ground speed (vox/s, constant); min height above (smoothed) local GROUND — must beat the tallest pine (116 vox) + bob or it clips foliage; wing-flap fps (24); voxel scale — 1.0 = each model voxel is exactly one 10 cm world voxel (same resolution as the trees)

  const startGrab = (it, wx, wy, wz, aPh, lev) => {    // EVERY pickup flies to the hand — never an instant swap; a 2nd rock flies to the LEFT hand
    if (lev) playPickUp();                             // ── THE SNATCH, NOT THE LANDING (user 2026-08-08) ── this fired from the two ARRIVAL branches at first, which put it a full GRAB_MS (measured: 365 ms) after the grab, and the pickup read as late. It belongs HERE: the item leaves the air on this frame, the flight is just it travelling to the hand. Only the two drop grabs pass lev — a rock in a wall or a worm in the dirt is not levitating.
    grabAnim = { t0: performance.now(), it, x: wx, y: wy, z: wz, aPh: aPh !== undefined ? aPh : performance.now() * 0.0012,
      // ── WHICH HAND IS THIS FLYING TO (user 2026-08-19: "when the player picked up a rock while holding a
      // stick in the right hand, it goes inside the player and then goes to the left hand") ── this used to
      // mean ONE thing: a second ROCK, for the dual-wield clash. Every other pickup took the right hand's
      // hand-full path, which draws the item as a world ghost flying to [P.x, eye + absorbY, P.z] — the
      // player's own chest. That was right while the off-hand could only ever hold a second rock, and became
      // wrong the moment the stone-age bench let it hold the OTHER half of a craft pair: the rock flew into
      // the player, was granted, and only then appeared out in the left hand, which is the jump reported.
      // The test now matches what tick-camera will actually SHOW in that hand (its craftOther), so the flight
      // ends where the item ends up:
      // ── AND IT IS ONE PREDICATE NOW, NOT A TABLE OF PAIRS (user 2026-08-20: "the dual wield is still
      // happening, even though the player is not pressing e for dual wield") ── the four cases spelled out here
      // were written when the off hand filled ITSELF: a second rock always, and the other half of a craft pair
      // as soon as one existed. Both of those are gone — the split is opt-in on shift+E and the craft pair only
      // shows while the bench gesture is running — so this table sent rocks and sticks flying to a hand that
      // was going to be empty when they arrived. offHandWants (ui/achievements.js) is the same question the
      // renderer answers, asked once, so the flight can no longer disagree with the hand.
      left: offHandWants(it) };
  };
  // == PUT A HELD ITEM DOWN (user 2026-08-20: "have the player be able to place the flowers back into the
  // terrain by place right click", then "apply the flower put down logic to all hand held items in the game.
  // the items become static in the terrain") == the mirror of the pickup, built out of the SAME pieces the
  // generator stamps decorations with, so a put-down object is ordinary world geometry:
  //   * the MODEL is PLACE_MODEL[item] (assets/held-items.js) - the source .vox, carrying palette ids. An item
  //     assembled from frames rather than from one model has no entry and simply cannot be put down.
  //   * the ROTATION comes off the placement column with flowerAt's own salt, so two things put down side by
  //     side do not stand in identical poses.
  //   * the write is stampModel's mode 1 (empty cells and soft decor), transcribed rather than called, because
  //     stampModel writes W and hands nothing back and a live edit needs the touched cells for gpuPatch. The
  //     rotation cases below are its four, verbatim.
  const PLACED = [], PLACED_IDX = new Map(), PLACED_MAX = 512;   // the ledger - see placedAt below
  // -- WHY THE SEAT IS A MAXIMUM (user 2026-08-20: "sometimes planting a flower on the first try doesnt work.
  // have it apply on the first try consistently") -- it used to seat on the column the ray happened to hit and
  // then refuse outright if any of the model's cells landed in solid ground. On flat ground that is always
  // fine; on a one-voxel step - which is most of this terrain - one column of a 3x3 footprint is a voxel
  // higher than the rest, that cell comes back solid, and the whole placement is refused. That is exactly
  // "sometimes the first try does nothing", and re-aiming a few centimetres is what made it work.
  // Seating on the HIGHEST free cell across the WHOLE footprint means no cell can be inside terrain, so the
  // refusal below is unreachable on ordinary ground and the first click lands every time. It is the same rule
  // the land mammals are seated by, and for the same reason.
  const freeTop = (gx, gz, from) => { let y = from; while (y < WY && W[gwrap(gx, WX) + y * WX + gwrap(gz, WZ) * WX * WY]) y++; return y; };
  function tryPlaceItem() {
    if (dead || grabAnim) return false;
    const it = heldIt(); if (!it) return false;
    const m = (typeof PLACE_MODEL !== 'undefined') ? PLACE_MODEL[it] : null;
    if (!m || !m.vox || !m.vox.length) return false;   // nothing to stamp - the item falls through to the eat/draw path untouched
    const cp2 = Math.cos(P.pitch), sp2 = Math.sin(P.pitch);
    const d = [Math.sin(P.yaw) * cp2, sp2, Math.cos(P.yaw) * cp2];
    let hx = 0, hy = -1, hz = 0;
    // ── THE SAME REACH A TOOL BREAKS AT (user 2026-08-20: "make the reach of placing hand held objects down
    // the same as using tools to break things") ── it was a flat 30, picked when this only planted flowers.
    // chopSwing marches min(REACH_3D, REACH_H / |cos pitch|), and so does the aim thud and the kill test, all
    // three off the same two constants "so they can never drift apart" (sim/tools.js). Putting a thing down is
    // the same arm at the same distance, so it reads off those constants too rather than carrying a fourth
    // number. The BOTH-CAPS form matters: a flat 3D radius lets you reach much further along the ground when
    // looking down than standing up, which is exactly the asymmetry REACH_H exists to remove.
    const cpP = Math.cos(P.pitch);
    const PLACE_REACH = Math.min(REACH_3D, REACH_H / Math.max(0.15, Math.abs(cpP)));
    for (let t = 0.6; t < PLACE_REACH; t += 0.3) {
      const x = Math.floor(P.x + d[0] * t), y = Math.floor(smoothEye + d[1] * t), z = Math.floor(P.z + d[2] * t);
      if (y < 1 || y >= WY) return false;
      const v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      if (!v || PASSTHRU.has(v)) continue;             // grass and ferns are what a plant grows THROUGH, so they are not the ground
      hx = x; hy = y; hz = z; break;
    }
    if (hy < 0) return false;                          // nothing under the crosshair within reach
    const rot = (ihash(hx * 61 + 31, hz * 67 + 43) * 3.99) | 0;
    const fw = (rot & 1) ? m.sy : m.sx, fd = (rot & 1) ? m.sx : m.sy;
    const bx = hx - (fw >> 1), bz = hz - (fd >> 1);
    // …ON THE COLUMN YOU AIMED AT, starting at the solid voxel the ray stopped on so a gap lower down the
    // column (beside a fallen log, under an overhang) can never be mistaken for the surface.
    // NOT the max over the footprint, which the first cut used: on rolling ground the highest of nine columns
    // is routinely a voxel or two above the one under the crosshair, so the object floated over the spot it was
    // aimed at — and, measured, ended up ABOVE the pick ray's own path through that column, which is why a
    // put-down flower could not then be picked back up. Seating where the player pointed is what makes the two
    // agree.
    const gy = freeTop(hx, hz, hy);
    if (gy < 1 || gy >= WY - m.sz) return false;
    if (gy <= WL + 1) return false;                    // not in the water - the scatter refuses beaches and shallows for the same reason (see flowerAt)
    const cells = [], ids = [];
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i];
      const x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & 255;
      let rx, rz;
      if (rot === 0) { rx = x; rz = y; }
      else if (rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      const ay = gy + z; if (ay < 1 || ay >= WY) return false;
      const ii = gwrap(bx + rx, WX) + ay * WX + gwrap(bz + rz, WZ) * WX * WY;
      const cur = W[ii];
      if (cur !== 0 && cur < DECOR_MIN) continue;      // ── SKIPPED, NOT REFUSED (user 2026-08-20: "sometimes planting a flower on the first try doesnt work") ── this is stampModel's mode-1 rule and mode 1 SKIPS: it may grow through grass, never through stone. Refusing the whole placement because one voxel of a 3x3 footprint landed in a one-voxel step is what made the first click do nothing on any uneven ground, and re-aiming a few centimetres is what appeared to fix it. The generator clips its own decorations against terrain exactly like this, everywhere, all the time
      cells.push(ii); ids.push(p >>> 24);
    }
    if (!cells.length) return false;                   // every voxel of it was blocked — there is genuinely nowhere to put this down
    for (let i = 0; i < cells.length; i++) W[cells[i]] = ids[i];
    gpuPatch(cells);
    // -- AND IT IS REMEMBERED, SO IT CAN BE PICKED BACK UP (user 2026-08-20: "when the player puts down the
    // flower after having picked it up, it cant be picked up again") -- the pickup identifies things by
    // PALETTE ID, and a put-down axe wears ids no PICK_ set claims, while a put-down flower stands where
    // flowerAt has never planted one so the variant lookup came back empty. A LEDGER answers both exactly:
    // these cells, this item. That is cheaper and more honest than reverse-engineering the object from its ids.
    // Bounded, and self-healing: entries are checked against the world before being honoured (see placedAt), so
    // one whose ground was dug out or whose region re-streamed is dropped rather than handing over an item that
    // is no longer standing there.
    const ent = { it, cells, ids, x: hx, y: gy, z: hz };
    PLACED.push(ent); for (const ii of cells) PLACED_IDX.set(ii, ent);
    while (PLACED.length > PLACED_MAX) { const old = PLACED.shift(); for (const ii of old.cells) if (PLACED_IDX.get(ii) === old) PLACED_IDX.delete(ii); }
    const sl = slots[selSlot];                         // and it costs one out of the stack, the exact inverse of the pickup
    if (sl) { sl.n -= 1; if (sl.n <= 0) slots[selSlot] = null; slotTidy(); }
    return true;
  }
  // Is this voxel part of something the player put down, and is that thing still all there? Returns the ledger
  // entry or null, dropping any entry the world has since disagreed with.
  function placedAt(ii) {
    const e = PLACED_IDX.get(ii); if (!e) return null;
    for (let i = 0; i < e.cells.length; i++) if (W[e.cells[i]] !== e.ids[i]) {   // dug out, overwritten, or re-streamed - it is not this object any more
      placedForget(e);
      return null;
    }
    return e;
  }
  function placedForget(e) { const k = PLACED.indexOf(e); if (k >= 0) PLACED.splice(k, 1);
    for (const c of e.cells) if (PLACED_IDX.get(c) === e) PLACED_IDX.delete(c); }
  function placedTake(e) {                             // lift the whole object back out of the world
    for (const c of e.cells) W[c] = 0;
    gpuPatch(e.cells);
    placedForget(e);
  }
  // -- WHICH FLOWER WAS THIS? -- flowerAt is the SAME function that decided what to plant here, so asking it
  // again is not a guess: it returns the variant index the stamp used, and the blossom band's pink twin is a
  // different set with its own item run. The 3x3 sweep is because a flower model is 3 voxels across, so the
  // petal the ray hit may be one cell off the stem the generator seeded on - and FLWCELL is 8, so a footprint
  // can straddle two cells. Nearest match wins; 0 means the ray hit a petal no GENERATED flower claims, which
  // is the ordinary case for one the player planted - the ledger above answers those.
  function flowerItemAt(x, z) {
    if (typeof flowerAt !== 'function' || !FLOWER_IT0) return 0;
    for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
      const f = flowerAt(Math.floor((x + dx) / FLWCELL), Math.floor((z + dz) / FLWCELL));
      if (!f || Math.abs(f.wx - x) > 1 || Math.abs(f.wz - z) > 1) continue;
      const base = f.ch ? FLOWER_CH_IT0 : FLOWER_IT0;
      return base ? base + f.k : 0;
    }
    return 0;
  }
  function tryPickup() {                               // march the view ray; first pickable id wins, first solid stops it
    if (grabAnim) return;                              // one flight at a time — a second grab mid-flight would overwrite (and lose) the first item
    const cp2 = Math.cos(P.pitch), sp2 = Math.sin(P.pitch);
    const d = [Math.sin(P.yaw) * cp2, sp2, Math.cos(P.yaw) * cp2];
    for (let i = 0; i < drops.length; i++) {           // DROPPED items grab first — generous ray-sphere test
      const dr = drops[i];
      if (dr.T && (performance.now() - dr.born) / 1000 < dr.T) continue;        // still mid-toss — let it land first
      if (!canAdd(dr.it)) continue;                    // no slot free for it
      const ox = dr.x + 0.5 - P.x, oy = dr.y + dropAnchor(dr) - smoothEye, oz = dr.z + 0.5 - P.z;
      const tq = ox * d[0] + oy * d[1] + oz * d[2];
      if (tq < 0 || tq > 55) continue;
      const qx = ox - d[0] * tq, qy = oy - d[1] * tq, qz = oz - d[2] * tq;
      if (qx * qx + qy * qy + qz * qz < 42) {
        startGrab(dr.it, dr.x + 0.5, dr.y + dropAnchor(dr), dr.z + 0.5, dr.spin === undefined ? performance.now() * 0.0012 + dr.ph : dr.spin, dropLevitating(dr));   // …from its CURRENT height: a settled drop is not where the hover pose was
        drops.splice(i, 1);
        return;
      }
    }
    if (WORM_NFRAMES && canAdd(WORM_ITEM0)) {          // LIVE WORMS — same generous ray-sphere grab as drops; the caught worm keeps wriggling in the hand
      for (let wi = WORM_0; wi < WORM_END; wi++) {     // worm pool slots (WORM_N = 32 — DOUBLED AGAIN 2026-07-18)
        const B = wbf[wi];
        if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const ox = B.x - P.x, oy = B.y - smoothEye, oz = B.z - P.z;
        const tq = ox * d[0] + oy * d[1] + oz * d[2];
        if (tq < 0 || tq > 50) continue;
        const qx = ox - d[0] * tq, qy = oy - d[1] * tq, qz = oz - d[2] * tq;
        if (qx * qx + qy * qy + qz * qz < 24) {
          startGrab(WORM_ITEM0, B.x, B.y, B.z);
          unstampWorm(B);                              // clear its grid stamp (flushed at the next creature-loop end)
          B.init = false;                              // the creature slot frees — a fresh worm respawns elsewhere in the ring later
          return;
        }
      }
    }
    for (let t = 0.6; t < 45; t += 0.3) {
      const x = Math.floor(P.x + d[0] * t), y = Math.floor(smoothEye + d[1] * t), z = Math.floor(P.z + d[2] * t);
      if (y < 0 || y >= WY) return;
      const ii9 = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
      const v = W[ii9];
      if (!v || PASSTHRU.has(v)) continue;
      // ── SOMETHING THE PLAYER PUT DOWN COMES BACK UP AS ITSELF ── tested FIRST, and before any palette-id
      // branch, because the ledger is exact where the id sets are only a guess: a put-down flower stands where
      // no generated one does, and a put-down axe wears ids no PICK_ set claims at all.
      { const pe = placedAt(ii9);
        if (pe) { if (canAdd(pe.it)) { placedTake(pe); startGrab(pe.it, x + 0.5, y + 0.5, z + 0.5); } return; } }
      if (PICK_ROCK.has(v) && canAdd(2)) { const c = floodRemove(x, y, z, PICK_ROCK, 40); if (c.length) { startGrab(2, x + 0.5, y + 0.5, z + 0.5); gpuPatch(c); } }   // cap fits the rounded dome (~23 voxels at rr 2.3)
      else if (PICK_BOULDER.has(v) && canAdd(2)) {     // MEDIUM boulder → one rock; it raised hmap when stamped, so re-derive the freed columns or drops/toss hover on phantom ground
        const c = floodRemove(x, y, z, PICK_BOULDER, 300);   // sized for the biggest medium boulder (rr≈4 ≈ 200 voxels) — a too-small cap leaves a half-eaten rock
        if (c.length) { startGrab(2, x + 0.5, y + 0.5, z + 0.5); gpuPatch(c);
          const cols = new Set(); for (const ii of c) cols.add(ii % WX + ((ii / (WX * WY)) | 0) * WX);
          for (const ci of cols) { let hy = hmap[ci]; while (hy > 1 && !W[(ci % WX) + (hy - 1) * WX + ((ci / WX) | 0) * WX * WY]) hy--; hmap[ci] = hy; } } }
      else if (PICK_TWIG.has(v)) {                   // ── STICK **or** PINECONE ── measured: PICK_CONE is [79-85] and PICK_STICK is [51,71,79-85,96-105], so EVERY cone id is also a
        // stick id. The models' own .vox ids are disjoint (cone 9-16, sticks 17-20/241-248); they collide because the 256-entry world palette is FULL and remaps by nearest colour, so the
        // cone's browns land on the sticks'. This branch used to be two, stick first, which made a pinecone pick up as a stick EVERY time — the cone branch was unreachable (user).
        // Ordering cannot fix a subset, so identify the WHOLE component instead: flood once over the union, then ask whether it contains any id a pinecone can never have.
        // The real fix is giving pinecone.vox its own palette entries (the loader comment above says it is supposed to have them); ~50 duplicate slots are reclaimable. Until then, this is exact.
        const sc = floodScan(x, y, z, PICK_TWIG, TWIG_CAP);   // one flood, read-only — nothing is destroyed until we know what it is AND that there is room for it
        // ── TOO BIG TO BE A TWIG (user 2026-08-20) ── a FALLEN LOG wears the same four pine browns a stick
        // does (see TWIG_CAP in sim/projectiles.js), so without this the flood claimed a 372-voxel log, took
        // the cap's worth out of it and handed over a twig. TWIG_MAX is the biggest stick model there is, so
        // anything past two of them is not sticks: leave it alone entirely rather than gouge it.
        if (sc.cells.length > TWIG_MAX * 2) return;
        if (sc.cells.length) {
          let isCone = true; for (const q3 of sc.kinds) if (!PICK_CONE.has(q3)) { isCone = false; break; }   // any stick-exclusive id present → it is a stick
          // ── A BLOSSOM TWIG STAYS PINK IN THE HAND ── decided by WHERE it was picked up rather than by the ids
          // the flood found, because the flood deliberately does not contain the leaf: BLOSLEAF is the canopy's
          // id set and putting it in PICK_STICK would make every pink crown right-click up as a twig (the
          // pinecone/stick collision recorded at palOwn). The biome is the honest question anyway — a twig lying
          // in the cherry forest fell off a cherry tree.
          const pit = isCone ? 4 : (STICK_BLOS_IT && cherryM(x, z) > 0.5 ? STICK_BLOS_IT : 3);   // 0.5 — the same number stickAt stamps on (world/terrain.js). At 0.15 a visibly GREEN twig in the outer blend picked up as a pink one
          // …and the LEAF comes away with it. sc.cells is the browns only — the blossom leaf is deliberately not a
          // pickup trigger (see PICK_STICK in sim/projectiles.js) — so without this the twig vanished and its pink
          // leaf stayed hanging in the air. twigLeafCells is bounded by the component's own box and capped at the
          // model's own leaf count, so it can never run out into the crown a twig happens to be lying under.
          const leaf9 = twigLeafCells(sc.cells), all9 = sc.cells.concat(leaf9);
          LAST_PICK.body = sc.cells.length; LAST_PICK.leaf = leaf9.length; LAST_PICK.it = pit;   // __vb.lastPick — the only way to SEE this working: a twig always lies under a crown, so counting blossom voxels in a box around it counts the canopy too
          if (canAdd(pit)) { for (const ii of all9) W[ii] = 0; startGrab(pit, x + 0.5, y + 0.5, z + 0.5); gpuPatch(all9); }
        }
      }
      else if (FRUIT_IDS.has(v)) { const fr = fruitAt(x, y, z);   // ── AN APPLE OR AN ORANGE, OFF THE BRANCH (user 2026-08-17) ── fruitAt (sim/projectiles.js) owns the whole verdict: which species, which cells, and whether this is one fruit rather than a cherry or a fused pair
        if (fr && canAdd(fr.it)) { for (const ii of fr.cells) W[ii] = 0; startGrab(fr.it, x + 0.5, y + 0.5, z + 0.5); gpuPatch(fr.cells); } }   // the FLESH comes away and the stalk stays on the tree — see the note in projectiles.js
      else if (PICK_FLOWER.has(v)) { const fit = flowerItemAt(x, z);   // ── A MEADOW FLOWER (user 2026-08-20) ── the bloom comes away and its two grass-green voxels stay, exactly as a fruit's stalk does (see PICK_FLOWER in sim/projectiles.js)
        if (fit && canAdd(fit)) { const c = floodRemove(x, y, z, PICK_FLOWER, FLOWER_CAP); if (c.length) { startGrab(fit, x + 0.5, y + 0.5, z + 0.5); gpuPatch(c); } } }
      return;
    }
  }
  function pickAim() {                                 // READ-ONLY mirror of tryPickup: is the view ray resting on something grabbable right now? (drives the crosshair □)
    if (grabAnim) return false;
    const cp2 = Math.cos(P.pitch), sp2 = Math.sin(P.pitch);
    const d = [Math.sin(P.yaw) * cp2, sp2, Math.cos(P.yaw) * cp2];
    for (let i = 0; i < drops.length; i++) {           // dropped items
      const dr = drops[i];
      if (dr.T && (performance.now() - dr.born) / 1000 < dr.T) continue;
      if (!canAdd(dr.it)) continue;
      const ox = dr.x + 0.5 - P.x, oy = dr.y + dropAnchor(dr) - smoothEye, oz = dr.z + 0.5 - P.z;
      const tq = ox * d[0] + oy * d[1] + oz * d[2];
      if (tq < 0 || tq > 55) continue;
      const qx = ox - d[0] * tq, qy = oy - d[1] * tq, qz = oz - d[2] * tq;
      if (qx * qx + qy * qy + qz * qz < 42) return true;
    }
    if (WORM_NFRAMES && canAdd(WORM_ITEM0)) {          // live worms
      for (let wi = WORM_0; wi < WORM_END; wi++) {
        const B = wbf[wi];
        if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const ox = B.x - P.x, oy = B.y - smoothEye, oz = B.z - P.z;
        const tq = ox * d[0] + oy * d[1] + oz * d[2];
        if (tq < 0 || tq > 50) continue;
        const qx = ox - d[0] * tq, qy = oy - d[1] * tq, qz = oz - d[2] * tq;
        if (qx * qx + qy * qy + qz * qz < 24) return true;
      }
    }
    for (let t = 0.6; t < 45; t += 0.3) {              // voxel rocks / sticks / cones — first pickable wins, first solid stops
      const x = Math.floor(P.x + d[0] * t), y = Math.floor(smoothEye + d[1] * t), z = Math.floor(P.z + d[2] * t);
      if (y < 0 || y >= WY) return false;
      const ii9 = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
      const v = W[ii9];
      if (!v || PASSTHRU.has(v)) continue;
      { const pe = placedAt(ii9); if (pe) return canAdd(pe.it); }   // …and the crosshair squares up on it too, through the same verdict
      if ((PICK_ROCK.has(v) || PICK_BOULDER.has(v)) && canAdd(2)) return true;
      if (PICK_STICK.has(v) && canAdd(3)) return true;
      if (PICK_CONE.has(v) && canAdd(4)) return true;
      if (FRUIT_IDS.has(v)) { const fr = fruitAt(x, y, z); return !!fr && canAdd(fr.it); }   // …and a fruit, through the SAME verdict the click uses, so the square never lights on a cherry or on a fused pair the pick would refuse
      if (PICK_FLOWER.has(v)) { const fi = flowerItemAt(x, z); return !!fi && canAdd(fi); }   // …and a flower, through the SAME verdict too: the crosshair must not square up on a petal flowerAt no longer claims
      return false;                                    // solid but not pickable → the ray is blocked
    }
    return false;
  }
  // where a drop's anchor actually is right now — hovering and bobbing at first, resting on the ground
  // once it has settled. Shared by both pickups and the aim tests so none of them grabs at a stale height.
  const dropAnchor = (dr) => {
    const nw = performance.now();
    const rK = Math.min(1, Math.max(0, (nw - dr.born - DROP_REST_MS) / DROP_REST_EASE));
    const rE = rK * rK * (3 - 2 * rK);
    const restY = dropRestY(dr.it);                     // …the SAME resting extent the pose uses, or a standing arrow is drawn at one height and grabbed at another
    return 9.0 + (restY - 9.0) * rE + Math.sin(nw * 0.002 + dr.ph) * 1.3 * (1 - rE);
  };
  // Is this drop still LEVITATING? Exactly dropAnchor's rK < 1, spelled out against the same two constants
  // rather than re-derived: the hover ends when the settle ease completes, and that is the one moment the
  // item stops being something you pick out of the air. Drives the pickup sound (see playPickUp).
  const dropLevitating = (dr) => performance.now() - dr.born < DROP_REST_MS + DROP_REST_EASE;
