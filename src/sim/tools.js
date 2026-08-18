  // @module — the voxel-accurate melee ray, the stone tools’ reach/bite constants, and the hoe’s tilled earth
  // @exports AIM_FORGIVE, AIM_R, AXE_SCALE, CHOP_RAD, DIG_SCALE, KNIFE_SCALE, PICK_SCALE, REACH_3D, REACH_H, SKUNK_ANIM_MUL, aimHitId, chopSwing, tillRevert, tilled, voxRay
  // ── AXE SWING -> TRUNK ── melee reach along the view ray, trunk voxels only. Returns true when the
  // swing bit wood so the caller can spend the swing on the tree rather than also registering a kill.
  const AIM_FORGIVE = 1.6;                            // voxels of slack around a creature's own radius in the kill test. Small and ABSOLUTE, so it forgives a shaky hand without ever growing into the fixed 35-degree cone this replaced.
  const AIM_R = [2.2, 2.2, 3.0, 3.0, 5.0, 3.0, 3.0];  // fallback target radius by B.kind (flyer, bird, worm/mammal, duck, lily, perched, fish) — used only when the creature is not grid-stamped, in which case its real stamped bounds are exact
  const REACH_H = 53, REACH_3D = 107, REACH_DOT = 0.82;   // THE melee reach — shared by the stone tools (chopSwing) and the kill test (aimedCreature) so the two can never drift apart (user). +5 voxels 2026-08-02.
  // ── VOXEL-ACCURATE AIM (user 2026-08-05) ── ONE traversal for every tool and every probe in the game.
  // What it replaces was point sampling: `for (t += 0.5) { x = Math.round(P.x + vx*t) … }`, which is wrong
  // twice over. Math.ROUND snaps to the NEAREST integer, but voxel v occupies [v, v+1) — so every lookup sat
  // half a voxel off on all three axes and could read the neighbour of the one under the crosshair. And a fixed
  // 0.5 step SAMPLES the ray rather than walking it: it can skip a voxel entirely, or land twice in one and
  // never touch the next. Aiming at a trunk and cutting the leaf beside it is exactly what those two produce.
  // This is the standard grid traversal (Amanatides & Woo): it visits every voxel the ray truly passes through,
  // in order, with no gaps and no repeats, and hands back the exact entry distance. `hit` returns truthy to
  // stop, and that value is returned, so a caller can act on whatever it found.
  const voxRay = (ox, oy, oz, dx, dy, dz, maxT, hit) => {
    let x = Math.floor(ox), y = Math.floor(oy), z = Math.floor(oz);
    const sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    const ax = Math.abs(dx) < 1e-9 ? Infinity : 1 / Math.abs(dx);   // distance travelled per whole voxel crossed, per axis
    const ay = Math.abs(dy) < 1e-9 ? Infinity : 1 / Math.abs(dy);
    const az = Math.abs(dz) < 1e-9 ? Infinity : 1 / Math.abs(dz);
    let tx = ax === Infinity ? Infinity : (dx > 0 ? x + 1 - ox : ox - x) * ax;   // …to the FIRST boundary on each axis
    let ty = ay === Infinity ? Infinity : (dy > 0 ? y + 1 - oy : oy - y) * ay;
    let tz = az === Infinity ? Infinity : (dz > 0 ? z + 1 - oz : oz - z) * az;
    let t = 0;
    for (let guard = 0; guard < 4096; guard++) {
      const r = hit(x, y, z, t);                       // the voxel the ray is inside right now, entered at distance t
      if (r) return r;
      if (tx <= ty && tx <= tz) { t = tx; tx += ax; x += sx; }
      else if (ty <= tz) { t = ty; ty += ay; y += sy; }
      else { t = tz; tz += az; z += sz; }
      if (t > maxT) return false;
    }
    return false;
  };
  const AXE_SCALE = 1.0;                              // THE reference sphere — every tool shares it (user)
  const PICK_SCALE = 1.0, DIG_SCALE = 1.0;            // the pick and the shovel now reach exactly as far as the axe (user)
  const KNIFE_SCALE = 0.5;                            // …except the KNIFE, which is smaller (user). Off its own material it works at half of this again. stone in chunks, wood a bite at a time
  const KNIFE_BITE = 0.5;                             // the stone knife BREAKS ANYTHING (user) and takes half the chunk doing it. Its OWN material is wood, so it reaches as far as the axe there. Unl of it.
  // ── THE SHOVEL LIFTS DOUBLE (user 2026-08-05) ── this is the lever SOI_OWN is not: the sphere only ever
  // decides WHICH voxels are candidates, and the near shells already hold far more soil than one bite, so
  // widening it changed nothing (measured null twice). The BITE is how much a swing actually takes.
  const DIG_BITE = 2;
  //                                                    A QUARTER of the axe's radius (halved twice, user); the chunk it yields is half. 1.0/1.0 would make it identical to the axe.
  const SKUNK_ANIM_MUL = 0.5;                          // the SKUNK's walk rate as a fraction of the porcupine's 12/24 (user 2026-08-06, re-asked 2026-08-07) → 6 fps walking, 12 fleeing. One constant, read by the world marcher AND the editor preview, so the two can never drift.
  const CHOP_RAD = 5, CHOP_DEEP = 1.5;   // RAD 5 so a full PH.chopBite is reachable from one impact point; CHOP_DEEP pushes the carve sphere into the wood so the chunk has depth instead of being a surface cap
  // ── DEEP WAS 3 (user 2026-08-07: "the axe hits the tree trunk from the backside") ── that 3 was authored
  // when the impact point could not be trusted: the carve fired from wherever treeShapeAt first answered,
  // which is open air well short of the tree, so most of the push was spent just REACHING the wood. Now that
  // the impact point is the voxel the crosshair rests on, the same 3 drives the sphere clean past the middle
  // of a bole — measured, the notch centre landed 2.25 voxels past the aimed face on a trunk 3.5-4 thick, so
  // the chunk came out of the FAR side. At 1.5 it lands 1.29 in: still buried enough that the bite is a ball
  // rather than the flat surface cap this offset exists to prevent, but in the near half where the axe struck.
  const CHOP_MINBITE = 30, CHOP_BODYBITE = 30;        // = PH.chopBite: the swing keeps marching until it finds somewhere that can give a FULL piece
  const CHOP_LAST = 15;                               // last-resort bite for an almost-severed trunk — half a piece, never the 2-3 voxel specks the old `1` produced   // a fallen trunk takes the SAME minimum bite as a standing one: at 4 the ray settled for the fringe of the hole it had just made and every swing after the first shaved 3-4 voxels   // a swing takes at least this many voxels, or it keeps looking
  // ── TILLING ── the hoe turns the top of the soil over: the surface voxel comes away and what is left is
  // TILLED EARTH, a lighter brown, across a disc around where you struck. Nothing can be planted in it yet,
  // so the ground REMEMBERS what it was and goes back to that after a while (user).
  const TILL_R = 5, TILL_MS = 45000;
  // TILL_ID lives in sim/hands.js, not here: this fragment is a module, and a module can only export a const
  // snapshot of a name, so a `let` minted in here would read as 0 everywhere outside (see the note there).
  const tilled = [];                                   // {t, ii, prevTop, jj, prevBelow, hi} — everything needed to put it back exactly
  const tillSet = new Set();                           // the voxels that ARE tilled earth, by index — the one-layer rule tests this, not the colour
  const hoeTill = () => {
    if (ED.on || dead) return false;
    const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch);
    const vx = Math.sin(P.yaw) * cp, vy = sp, vz = Math.cos(P.yaw) * cp;
    const reach = Math.min(REACH_3D, REACH_H / Math.max(0.15, Math.abs(cp)));   // the same reach a swing gets (CHOP_REACH itself is local to chopSwing)
    let cx = 0, cz = 0, found = false;                 // where the swing lands on the ground
    voxRay(P.x, smoothEye, P.z, vx, vy, vz, reach, (x, y, z) => {
      if (y < 1 || y >= WY) return false;
      if (!solid(x, y, z)) return false;
      cx = x; cz = z; found = true; return true;       // the FIRST voxel the ray truly enters, not the nearest-rounded sample
    });
    if (!found) return false;
    // TILLED EARTH: a DARKER brown than the first pass (user) — turned soil, not dust. One palette entry, and
    // only if the hoe is ever actually used — but the table is HARD CAPPED at 256 and
    // an id past that wraps to 0, which wrote AIR into the ground instead of tilled earth. If there is no
    // room left, take the nearest colour already in the table rather than corrupting the world.
    if (!TILL_ID) {
      const fresh = palette.length < 255;              // a BRAND-NEW palette entry, or a shade already owned by something else?
      if (fresh) TILL_ID = addCol(150, 116, 76);
      else {                                           // the table is FULL — pick the closest shade already in it, by hand:
        let bd = 1e9;                                  // edCol's own match came back near-white, which is not tilled earth
        for (let i = 0; i < palette.length && i < 255; i++) { const c = palette[i];
          const d = (c[0] - 150) ** 2 + (c[1] - 116) ** 2 + (c[2] - 76) ** 2;
          if (d < bd) { bd = d; TILL_ID = i + 1; } }
      }
      // ── THE MATERIAL TABLES AND THE SUPPORT CLASS ARE BOTH FOR A NEW ENTRY ONLY ── this is the one palette
      // id in the game minted at RUNTIME, so the sweeps that fill solidTab/digOnlyTab/SUP.CLASS beside the
      // material tables cannot see it and it has to write its own. But ONLY when the entry is genuinely NEW:
      // with a full table TILL_ID is a shade already owned by something else, and every write here is by
      // PALETTE ID, so it re-describes that material everywhere in the world for the rest of the session.
      // SUP.CLASS was already guarded; the two table writes were not, and digOnlyTab is the more expensive
      // mistake of the two because it is tested BEFORE pickOnlyTab in every tool gate — the nearest source
      // literal to (150,116,76) is OREIRON[0], so one hoe swing could turn iron ore shovel/knife-only and
      // make it un-mineable with the pick. palSync goes with them: nothing was added to the palette either.
      // (The support class barely matters even when it is written — hoeTill puts tilled earth at h-2 and
      // lowers hmap to h-1, so the cell is inside the static ground column and supAnchored answers yes on
      // the O(1) test whatever its class.)
      if (fresh) { solidTab[TILL_ID] = 1; digOnlyTab[TILL_ID] = 1; SUP.CLASS[TILL_ID] = SUP.STRUCTURE; palSync(); }
      console.log('[vb] tilled earth id', TILL_ID, 'palette', palette.length, TILL_ID > 255 ? 'OVERFLOW' : '');
    }
    const cells = [];
    for (let dz = -TILL_R; dz <= TILL_R; dz++) for (let dx = -TILL_R; dx <= TILL_R; dx++) {
      if (dx * dx + dz * dz > TILL_R * TILL_R) continue;
      const x = cx + dx, z = cz + dz, gx = gwrap(x, WX), gz = gwrap(z, WZ);
      const hi = gx + gz * WX, h = hmap[hi];           // hmap is the first AIR voxel, so the surface is h-1
      if (h < 3 || h >= WY - 1) continue;
      const ii = gx + (h - 1) * WX + gz * WX * WY, jj = gx + (h - 2) * WX + gz * WX * WY;
      const top = W[ii];
      if (tillSet.has(ii)) continue;                 // ── ONE LAYER ONLY (user) ── already turned over: the hoe cannot dig itself deeper
      if (!top || !digOnlyTab[top] || SAND.indexOf(top) >= 0) continue;   // only SOIL turns over — and SAND is not soil (user 2026-08-07): it is in digOnlyTab because the SHOVEL moves it, which is a different question from whether a hoe can make a seed bed out of a beach
      // ── AND THE STRANDS ON TOP GO WITH IT (user) ── grass tufts, flowers, twigs and cones stand ON the
      // surface voxel that just came away and carry no support of their own — that is exactly what floatTab
      // marks, and the chop path already erases the same set when it digs the ground out from under them.
      // Without this the tufts hung in mid-air over bare turned soil. The whole column is lifted and
      // REMEMBERED, so it grows back with the ground beneath it when the till reverts.
      const str = [];
      for (let y = h; y < Math.min(WY, h + 10); y++) {
        const si = gx + y * WX + gz * WX * WY, sv = W[si];
        if (!sv) break;                              // clear sky
        // ── AND THE SNOW SIMPLY GOES (user 2026-08-07: "when the player tills the land, the snow floats") ──
        // the blanket sits at h, the first air voxel, so it was the FIRST thing this loop met and `!floatTab`
        // broke out of it immediately: the surface voxel underneath was then lifted and the snow was left in
        // mid-air. Snow is weather, not planting, so unlike the strands it is NOT pushed into `str` — the till
        // revert must not lay a blanket back down on ground that has since thawed.
        if (snowTab[sv]) { W[si] = 0; cells.push(si); continue; }
        if (!floatTab[sv]) break;                    // a log/rock/trunk — real geometry, and not ours to move
        str.push(si, sv); W[si] = 0; cells.push(si);
      }
      tilled.push({ t: performance.now(), ii, prevTop: top, jj, prevBelow: W[jj], hi, str });
      tillSet.add(jj);                               // …the NEW surface is tilled earth, and stays off limits until it grows back
      W[ii] = 0; cells.push(ii);
      W[jj] = TILL_ID; cells.push(jj);
      hmap[hi] = h - 1;
    }
    if (!cells.length) return false;
    gpuPatch(cells, false);
    return true;
  };
  const tillRevert = (nowMs) => {                      // …and it grows back over, since nothing was ever planted in it (user)
    if (!tilled.length) return;
    const cells = [];
    for (let i = tilled.length - 1; i >= 0; i--) {
      const q = tilled[i];
      if (nowMs - q.t < TILL_MS) continue;
      tilled.splice(i, 1);
      if (W[q.ii] || W[q.jj] !== TILL_ID) { tillSet.delete(q.jj); continue; }   // something was built or dug here since — leave it alone
      W[q.ii] = q.prevTop; cells.push(q.ii);
      W[q.jj] = q.prevBelow; cells.push(q.jj);
      if (q.str) for (let k = 0; k < q.str.length; k += 2) {   // …and the grass the hoe turned under grows back with it (user)
        const si = q.str[k];
        if (W[si]) continue;                         // something moved in since — a snow layer, a dropped chunk: leave it be
        W[si] = q.str[k + 1]; cells.push(si);
      }
      tillSet.delete(q.jj);
      hmap[q.hi] = hmap[q.hi] + 1;
    }
    if (cells.length) gpuPatch(cells, false);
  };
  // ── WHAT THE SWING RAN INTO (user 2026-08-07) ── chopSwing answers one question, "did I bite something",
  // and a NO from it covers two swings that should not sound alike: one through open air, and one that hit
  // material this tool cannot break. Only the second thuds. This is a separate probe rather than a value
  // threaded out of chopSwing because that function's aim state is delicate — the leaf-vs-trunk see-through
  // rule, the felled-log gate — and its `false` is reachable from four different places, one of which
  // (an axe on stone inside a pine's footprint) never even reaches the obstruction test. Cost is one grid
  // walk of at most ~107 voxels, and only on a swing that already found nothing, at most once per 570 ms.
  // The obstruction test below is COPIED from the main march's `return 2` line on purpose: whatever that
  // walks through — pass-through water, surface scatter, needles — the swing did not run into either, so
  // the two can never disagree about what the crosshair is resting on.
  const aimHitId = () => {
    const cp = Math.cos(P.pitch), vx = Math.sin(P.yaw) * cp, vy = Math.sin(P.pitch), vz = Math.cos(P.yaw) * cp;
    const reach = Math.min(REACH_3D, REACH_H / Math.max(0.15, Math.abs(cp)));   // the same both-caps reach chopSwing marches, so the thud stops exactly where the swing stops
    return voxRay(P.x, smoothEye, P.z, vx, vy, vz, reach, (x, y, z) => {
      if (y < 1 || y >= WY) return 0;
      const b9 = phBodyIdAt(x, y, z);                  // a felled trunk is one off-grid rigid body and is NOT in W, but it is very much something to bounce off
      if (b9) return b9;
      const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      if (id !== 0 && !foliaTab[id] && !floatTab[id] && !(isWater(id) && !solidTab[id])) return id;
      return 0;                                        // water, grass strands, twigs and needles are not obstructions — walk on to what is underneath
    }) || 0;
  };
  const chopSwing = () => {
    if (!PH.on || ED.on || dead) return false;
    if (HOE_IT && heldIt() === HOE_IT) return hoeTill();   // the HOE does not chop — it tills (user)
    // ── WHAT IS IN YOUR HAND ── the axe and the stone knife both CUT; anything else (or nothing) can
    // still knock chunks out of soft decor. The knife works the same way at HALF the sphere (user), and
    // the bite scales with it so the piece stays a full ball rather than a sliver of a big one.
    const it = heldIt(), axe = it === 1, knife = KNIFE_IT > 0 && it === KNIFE_IT, cut = axe || knife;
    const pick = PICK_IT > 0 && it === PICK_IT;      // the PICK breaks stone and nothing else — note it is deliberately NOT part of `cut`, which is what gates every wood path below
    const dig = SHOVEL_IT > 0 && it === SHOVEL_IT;   // …and the SHOVEL moves ground and nothing else, on the same principle
    // ── HOW FAR THIS TOOL REACHES, PER MATERIAL (user) ── its own material at full sphere, anything else
    // at half, and WOOD always at the axe's sphere whatever is in hand. `base` is this tool's own.
    const base = knife ? KNIFE_SCALE : (dig ? DIG_SCALE : (pick ? PICK_SCALE : AXE_SCALE));
    const ownTab = dig ? digOnlyTab : (pick ? pickOnlyTab : woodTab);   // a cutting edge owns WOOD; the shovel soil, the pick stone and ore
    // ── SOI DOUBLED (user 2026-08-05) ── every tool reaches TWICE as far into its OWN material. Only that
    // number moves: off-material reach stays at half the base sphere, and every BITE stays exactly what it was,
    // which is why the bite keeps its own factor (soi) while the reach uses soiR. Doubling one function for
    // both would have doubled how much each swing lifts as well.
    // The SHOVEL gets TWICE that again (user 2026-08-05) — a radius-20 sphere in soil where every other tool
    // gets 10. NOTE what this does and does not change: the sphere only ever hands phChopDecor a wider set of
    // CANDIDATES, and the swing still lifts `mBite` of them. On ordinary ground the near shells already hold
    // far more soil than one bite, so the wider sphere changes nothing there — it tells only where soil is
    // scarce within 10 but not within 20 (a patch smaller than a bite, the far lip of a crater). Making a dig
    // visibly faster is the BITE's job, not this one's.
    const SOI_OWN = dig ? 4 : 2;
    const isWood = (v) => !!woodTab[v];               // the tree paths below cut BARK AND BRANCH, never needles (see the physChopAt calls)
    const soi = (id) => (ownTab && ownTab[id]) ? base : base * 0.5;     // material factor for the BITE — unchanged
    const soiR = (id) => (ownTab && ownTab[id]) ? base * SOI_OWN : base * 0.5;   // …and the REACH, doubled on the tool's own material
    const kS = base;
    const C_RAD = CHOP_RAD * base * SOI_OWN, C_DEEP = CHOP_DEEP * base;
    const C_CUT = C_RAD;   // the CARVE sphere. Same as the reach today; kept as its own name because they are different questions — how far a swing may look for material, and how big a bite it takes out of it.   // the WOOD paths — a cutting edge OWNS wood, so this is its doubled sphere; C_DEEP is a depth offset, not a reach, and is untouched
    const C_BITE = knife ? Math.max(4, Math.round(PH.chopBite * KNIFE_BITE * base)) : Math.round(PH.chopBite * base);   // the WOOD chunk, at this tool's sphere   // the knife takes HALF the axe's chunk (user); the PICK and SHOVEL take proportionally MORE, because a wider sphere that still lifts 30 voxels is just a slower way to dig the same hole
    // A minimum bite bigger than the sphere can physically hold makes every swing miss (that is how the
    // knife looked broken before). ~2r^2 is a fair count of solid cells a half-filled sphere of radius r
    // can offer, so the requirement can never exceed what is reachable.
    const cap = Math.max(2, Math.floor(2.0 * C_RAD * C_RAD));
    const C_MIN = Math.max(2, Math.min(Math.round(CHOP_MINBITE * kS), cap));
    const C_LAST = Math.max(2, Math.min(Math.round(CHOP_LAST * kS), cap));
    const cp = Math.cos(P.pitch), vx = Math.sin(P.yaw) * cp, vy = Math.sin(P.pitch), vz = Math.cos(P.yaw) * cp;
    // the ray is unit, so t IS the 3D distance and t*cp the horizontal one — cap by both, exactly as
    // aimedCreature does, so the axe reaches precisely as far as a swing that would kill
    const CHOP_REACH = Math.min(REACH_3D, REACH_H / Math.max(0.15, Math.abs(cp)));
    phSetFallDir(vx, vz);                              // fell it AWAY from the player, like a real notch
    // ── LEAVES ── whatever the crosshair is actually resting on wins, and a leaf yields to ANYTHING
    // (user). Decided BEFORE the march because the wood path finds its tree with a sparse probe that
    // fires every 4 voxels through open air — aiming at foliage, the swing was spent on the trunk
    // behind it before the ray ever reached the leaf.
    // ── LEAF vs TRUNK ── "first thing the ray touches wins" is not what the player means. A pine's trunk
    // stands INSIDE its crown, so a ray aimed dead at the trunk still clips a needle a voxel or two before it
    // — and now that the walk is exact rather than sampled every half voxel, it clips EVERY one of them
    // instead of skipping most. That is why aiming at a trunk started cutting leaves (user). So: when the ray
    // first meets foliage, keep walking a short way. If WOOD is close behind it, the crosshair was on the
    // trunk and the leaf was only in the way — hand the swing to the wood path. The leaf wins only when
    // nothing solid is hiding behind it, which is exactly when the player really is pointing at foliage.
    const LEAF_SEE_THROUGH = 6;                        // voxels of needles a trunk can hide behind — a pine crown is a few voxels of needle around the bole
    // ── AND THE SAME TEST HAS TO SEE A FELLED TREE (user 2026-08-07) ── it read W and only W, and a tree
    // on the ground is NOT in W: the whole log, needles and bark alike, is one off-grid rigid body. So on a
    // fallen pine this pass found nothing at all, aimId stayed 0, and the march below was left to decide by
    // proximity — which on a crown wrapped round a bole means needles. Asking phBodyIdAt where W is empty
    // makes the leaf-vs-trunk rule work identically standing or fallen, which is what the player expects.
    const SNOW_SEE_THROUGH = 4;                        // voxels of SNOW the ground can hide behind — landSnowAt caps a stack at 3, +1 for a diagonal crossing
    let firstSnow = false, snowT = 0;                  // the first snow voxel the aim ray met, and where
    let firstLeaf = null, leafT = 0, aimId = 0, aimBody = false, aimT = Infinity;   // aimId: the material the crosshair is REALLY resting on, after the see-through rule — aimBody: whether that voxel belongs to an off-grid rigid body (a felled tree) rather than to W
    const leafHit = voxRay(P.x, smoothEye, P.z, vx, vy, vz, CHOP_REACH, (lx, ly, lz, t) => {
      if (ly < 1 || ly >= WY) return 0;
      let lv = W[gwrap(lx, WX) + ly * WX + gwrap(lz, WZ) * WX * WY];
      let inB = false;
      if (!lv) { lv = phBodyIdAt(lx, ly, lz); inB = !!lv; }   // W is air here — but a felled trunk or its crown may be
      if (!lv) return 0;                               // still in the air — keep looking for the first thing the crosshair meets
      if (foliaTab[lv]) {                              // a leaf: remember it, then keep looking for wood just behind
        if (!firstLeaf) { firstLeaf = [lx, ly, lz]; leafT = t; aimId = lv; aimBody = inB; aimT = t; }
        return 0;
      }
      if (!inB && (floatTab[lv] || (isWater(lv) && !solidTab[lv]))) return 0;   // ── PASS-THROUGH IS NOT AN AIM (user 2026-08-07) ── grass strands, blooms, twigs, cones and open water: the march's obstruction test and aimHitId both walk straight THROUGH these, but this pass stopped on them and set aimT to a STRAND's distance — which makes every `t >= aimT` gate below vacuous, since the ray passes that distance almost immediately. Ice keeps solidTab, so a frozen lake is still a thing you can aim at.
      if (!inB && snowTab[lv]) {                       // ── SNOW IS COVER, NOT A TARGET (user 2026-08-07: "the tool shouldn't register the snow, but the material under the snow") ── the same rule as LEAF vs TRUNK: remember the blanket, then keep walking for what it is sitting on. landSnowAt caps a stack at 3 layers, so the budget below always reaches the ground.
        if (!firstSnow) { firstSnow = true; snowT = t; if (!firstLeaf) { aimId = lv; aimBody = inB; aimT = t; } }   // …and the snow itself is only the FALLBACK aim, for a drift with nothing reachable under it
        return 0;
      }
      if (!firstLeaf) { if (!firstSnow || t - snowT <= SNOW_SEE_THROUGH) { aimId = lv; aimBody = inB; aimT = t; } return 2; }   // solid, and no leaf in front of it — the ordinary march decides
      if (woodTab[lv] && t - leafT <= LEAF_SEE_THROUGH) { aimId = lv; aimBody = inB; aimT = t; return 2; }   // TRUNK behind the needles → the player meant the trunk
      return 3;                                        // something else solid behind the leaf → the leaf really was the target (aimId stays the leaf)
    });
    // ── THE SWING IS CONFINED TO WHAT THE CROSSHAIR IS ON ── wood keeps to wood, needles to needles. Only
    // those two: anything else (stone, soil, a chunk of something) keeps the old unfiltered behaviour, so
    // nothing outside the tree paths changes. This is the off-grid twin of the `isWood` filter physChopAt
    // already gets, and of okMat on the W path.
    // ── AND A BODY IS CONFINED TO ITS AIMED MATERIAL THE WAY THE WORLD IS ── wood and foliage had their own
    // filters; everything else fell through to null, i.e. the sphere took whatever was nearest. Mirrors okMat
    // below so a bite out of a toppled rock cannot spill into the moss riding on it.
    const okBody = woodTab[aimId] ? ((v) => !!woodTab[v])
      : (foliaTab[aimId] ? ((v) => !!foliaTab[v])
      : (aimId ? (digOnlyTab[aimId] ? ((v) => !!digOnlyTab[v])
                : (pickOnlyTab[aimId] ? ((v) => !!pickOnlyTab[v])
                : ((v) => !digOnlyTab[v] && !pickOnlyTab[v]))) : null));
    // ── WHICH TOOL MAY CUT THIS BODY (user 2026-08-08: "when a rock topples over from cutting it in half, now
    // I cant keep taking chunks out of the rigid body… the same with the rigid mushroom… the tree still works
    // fine") ── the whole rigid-body branch was gated on `cut`, i.e. on holding a cutting edge. That is right
    // for WOOD and wrong for everything else, and it is exactly why a felled tree stayed choppable while a
    // toppled rock and a felled mushroom did not: a rock wants the PICK and a mushroom yields to anything, and
    // neither is `cut`, so the branch was skipped and the swing fell through to the world path — which finds
    // nothing, because a rigid body is not in W. The material's own rule is the one that should decide, and
    // that rule already exists a few lines below for the same materials while they are still in the ground.
    const bodyTool = (v) => digOnlyTab[v] ? (dig || knife) : (pickOnlyTab[v] ? (pick || knife) : (axeOnlyTab[v] ? cut : true));
    // ── THE ONE GATE (user 2026-08-07: "I'm aiming and clicking with the axe on the dirt, the axe registers the
    // nearby tree") ── the crosshair is resting on GRID wood. treeShapeAt is a bare XZ bounding-box test over a
    // pine's ~35-voxel footprint, so S latches for ANY ground column within ~17 voxels of the bole; physChopAt
    // then gathers every wood voxel within radius 10 of the impact point. With no material gate, a swing aimed
    // at dirt inside that footprint reached sideways and took the trunk — measured at 69% of ground aims.
    // Every path that carves a standing tree or a stump now asks this and nothing else.
    // ── AND A HOLE PUNCHED CLEAN THROUGH A TRUNK IS STILL AIMING AT THE TREE (user 2026-08-07) ── once the
    // notch pierces the bole the crosshair looks straight through it at open sky, so aimId is 0, aimWood went
    // false, and the axe simply stopped working: measured, 196 of 220 swings at a holed trunk did nothing at
    // all while 27 voxels of wood still stood around the hole. A tree left standing with a hole through its
    // base is exactly what "floating from the base where it was broken" looks like. `aimId === 0` means the ray
    // reached its full length without meeting anything solid — it cannot be confused with the dirt case that
    // the gate exists for, where aimId is the dirt — and the tree paths still require S, so only a column
    // genuinely inside a pine's footprint can be cut this way.
    const aimSky = aimId === 0;
    const aimWood = !aimBody && (!!woodTab[aimId] || aimSky);
    const aimTW = aimSky ? 0 : aimT;                   // nothing was hit, so there is no "past the crosshair" to enforce
    if ((leafHit === 3 || (leafHit === false && firstLeaf))          // the leaf wins: nothing behind it, or the ray ran out inside the crown
        && phChopLeaves(firstLeaf[0], firstLeaf[1], firstLeaf[2], CHOP_RAD * base * 0.5, Math.max(2, Math.round(C_BITE * 0.5)))) return true;   // FOLIAGE is nobody's own material: half the tool's sphere (user)   // a FALLEN crown is not in W, so this misses and the march below takes it with okBody instead
    let S = null;
    // ── VOXEL-ACCURATE (user 2026-08-05) ── walk the exact voxels the crosshair ray passes through. The
    // body below is unchanged; only its exits are, because a callback cannot continue/return the outer
    // function: 0 = keep walking, 1 = the swing bit something, 2 = a real obstruction stops it.
    let nStep = 0;
    const mainHit = voxRay(P.x, smoothEye, P.z, vx, vy, vz, CHOP_REACH, (x, y, z, t) => {
      nStep++;
      if (y < 1 || y >= WY) return 0;
      // ── THE SPHERE HAS TO BE CENTRED ON THE BODY, NOT IN FRONT OF IT (user 2026-08-07) ── this fired on
      // EVERY marched voxel, air included, so a radius-10 sphere started grabbing at the log from ten voxels
      // short of it: centred out in open air, the nearest voxels it could reach were whatever stuck out
      // furthest — the crown. Requiring the marched voxel to be INSIDE the body makes the impact point the
      // voxel the crosshair is genuinely on. A notched log still works: the ray walks through the hole the
      // last swing left and centres on the next body voxel behind it.
      // …and NOT `!S` any more (user 2026-08-07): treeShapeAt is a COLUMN test that answers the moment the
      // ray enters a standing pine's FOOTPRINT, tens of voxels of open air before the bole. It latches S for
      // the rest of the march, and every line below it returns before this one can run again — so a log
      // felled inside a forest (which is every log) had its swings quietly handed to whichever standing tree
      // happened to overhang it. Measured: 11 of 14 swings aimed at a fallen bole took no body bite at all,
      // and small `treeChunk` pieces appeared off a tree nobody was pointing at. The bAt gate makes this test
      // exact on its own, so it no longer needs S to stand aside for it.
      const bAt = phBodyIdAt(x, y, z);
      if (bAt && bodyTool(bAt) && (!okBody || okBody(bAt)) &&
          (phChopBody(x + vx * C_DEEP, y + vy * C_DEEP, z + vz * C_DEEP, C_RAD, C_MIN, C_BITE, okBody) ||
           phChopBody(x, y, z, C_RAD, C_MIN, C_BITE, okBody))) return 1;   // deeper bite first, same as the standing trunk   // a FELLED trunk lying in the way is choppable too — tested before treeShapeAt so the tool hits what is actually nearest
      const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      // ROCK needs the pick, WOOD needs a cutting edge, everything else soft yields to whatever is in hand.
      // …and the carve is confined to that same material, so the sphere cannot spill into the next one.
      const okMat = digOnlyTab[id] ? ((v) => !!digOnlyTab[v]) : (pickOnlyTab[id] ? ((v) => !!pickOnlyTab[v]) : ((v) => !digOnlyTab[v] && !pickOnlyTab[v]));
      const mS = soi(id);                            // this material's BITE factor (own = full, anything else = half) — unchanged by the SOI doubling
      const mR = soiR(id);                           // …and its REACH, the doubled one on the tool's own material
      // the BITE follows the sphere it came out of — scaled from PH.chopBite, not from C_BITE, which
      // already carries the tool's own factor and would apply it a second time.
      const mBite = knife ? Math.max(2, Math.round(PH.chopBite * KNIFE_BITE * mS)) : Math.max(2, Math.round(PH.chopBite * mS * (dig ? DIG_BITE : 1)));
      // ── AND A BEEHIVE IS THE ONE PIECE OF DECOR THAT ANSWERS BACK (user 2026-08-17: "if the player breaks
      // open the beehive, have bees fly out of it, attacking the player") ── asked AFTER the carve, because the
      // question is "is the hive open NOW". hiveChopped (sim/life/slots.js) counts what is left of the box and
      // does nothing at all until it is past BEE_BREAK_F, so a chip is still just a chip. That is the whole of
      // the tool's involvement: it posts a fact about the world to a ledger and returns. It does not know what
      // a bee is, and the swing itself is untouched — same gate, same sphere, same bite, same return value.
      if (id && decorTab[id] && !woodTab[id] && (digOnlyTab[id] ? (dig || knife) : (pickOnlyTab[id] ? (pick || knife) : (axeOnlyTab[id] ? cut : true))) && phChopDecor(x, y, z, CHOP_RAD * mR, mBite, okMat)) { if (HIVE_TAB[id]) hiveChopped(x, y, z); return 1; }   // …but NOT wood: a standing trunk belongs to the tree path below, which is what fells it   // free gate, the id is already in hand
      // Find the pine ONCE, then keep cutting along the rest of the ray. The trigger is any non-air voxel
      // PLUS a sparse probe every 4 voxels: after the first swing punches a hole the ray sees only air
      // where the trunk was, and a solid-only trigger could never find the tree again — the axe would
      // land exactly one bite and then stop.
      if (cut && !S && (id !== 0 || (nStep & 3) === 0)) S = treeShapeAt(x, z);   // WOOD wants a cutting edge — the axe or the knife
      if (S) {
        // physChopAt cuts everything within CHOP_RAD of the point, so marching through the trunk's
        // airspace WIDENS the notch each swing instead of needing an exact voxel hit. The bite is taken
        // CHOP_DEEP further along the ray so the sphere sits INSIDE the wood: centred on the surface
        // sample, most of it hung in open air and the chunk came out a flat cap (user: "the chunks are
        // often flat pieces"). If the deeper point has run out the far side, the surface point still gets
        // its turn.
        // ── WOOD ONLY (user 2026-08-05: "aiming at the trunk gets the leaves") ── treeShapeAt is a COLUMN
        // test: it answers the moment the ray enters the pine's footprint, which is the whole crown, so the
        // first carve point is usually open air 15 voxels short of the bole. An unfiltered sphere then took
        // the nearest 30 tree voxels it could reach from there — needles, every time, since the crown is
        // nothing but. Filtered to wood, the canopy holds no bite at all and the march walks on to the
        // trunk the player was actually pointing at. Aiming AT foliage still cuts foliage: that is decided
        // before this march and goes to phChopLeaves.
        // ── BUT NOT OVER A FELLED LOG (user 2026-08-07) ── treeShapeAt answers on the whole FOOTPRINT, so S
        // latches while the ray is still in open air 15 voxels short of the bole, and this radius-10 carve
        // then fires from there — outranking everything the ray has not reached yet. A log always comes to
        // rest inside some standing pine's footprint, so swings aimed squarely at a fallen bole were being
        // spent on whichever tree overhung it (measured: 8 of 14). When the crosshair is resting on the
        // off-grid body, this path stands aside and the body path above takes the swing. Aim at a STANDING
        // trunk and nothing here changes — which is deliberate: gating on distance instead moved the carve
        // onto the bole and felled a pine in 2 swings instead of ~9, and that is a balance call, not a fix.
        if (aimWood && t >= aimTW && (physChopAt(x + vx * C_DEEP, y + vy * C_DEEP, z + vz * C_DEEP, C_CUT, S, C_MIN, C_BITE, isWood).hit ||
                                      physChopAt(x, y, z, C_CUT, S, C_MIN, C_BITE, isWood).hit)) return 1;   // whatever that branch was holding up now drops through supFlush, fed by physChopAt's own gpuPatch
        if (aimWood) return 0;   // ── AND ONLY A TREE SWING OWNS THE VOXEL ── this swallowed EVERY marched voxel once S latched, which made the obstruction test below unreachable anywhere inside a pine's footprint: dirt could never stop the swing, so the ray walked on UNDERGROUND firing a radius-10 wood grab from each voxel for the rest of its reach. A swing that does not belong to the tree now falls through to the stump and obstruction tests exactly as it would outside the footprint.
      }
      // ── ORPHANED WOOD ── a STUMP is wood that belongs to no tree: treeShapeAt stops answering for that
      // column the moment the trunk topples, so S is null and nothing above cuts it. Worse, a stump is
      // SHORT, so the swing that means to hit it usually samples the dirt in FRONT of it and stops there.
      // Hence no test on the sampled voxel: whatever the ray landed on, if wood is within the axe's own
      // sphere it gets cut. ok() confines the carve to wood, so it can never eat the ground it stands in.
      // ── BUT ONLY WHEN THE CROSSHAIR IS ON WOOD (user 2026-08-07) ── unconditional, this is a radius-10
      // grab for wood fired from EVERY voxel of the ray, and it outranks everything the march has not
      // reached yet: aiming at a felled crown, the swing was landing on the STUMP ten voxels off the line
      // of sight (measured: chip src decorBite, 7 wood, 0 needles, while the crosshair sat on needles).
      // The reason it had no test on the sampled voxel no longer holds either — the march samples every
      // voxel exactly now, not every half voxel, so a swing meant for a stump lands ON the stump and
      // aimId is its bark. Anything else the player is pointing at keeps its own swing.
      if (cut && aimWood && t >= aimTW && phChopDecor(x, y, z, C_CUT, C_BITE, (v) => !!woodTab[v])) return 1;   // …and, like the tree path, it stands aside for a crosshair resting on a felled log
      // ── WATER IS NOT AN OBSTRUCTION (user 2026-08-05: "let me use tools underwater") ── a water voxel is a
      // non-zero id like any other, so this test used to STOP the swing dead at the surface: the pick bounced
      // off the lake instead of the rock beneath it, and nothing submerged could be worked at all. A FROZEN
      // lake is a different matter — solidTab[WATER_T] flips to 1 as the ice sets in, and ice SHOULD stop a
      // swing — so the pass-through asks solidTab rather than trusting the id.
      if (id !== 0 && !foliaTab[id] && !floatTab[id] && !(snowTab[id] && !snowTab[aimId]) && !(isWater(id) && !solidTab[id])) return 2;   // …and SNOW is not one: a blanket over rock must not eat the swing (user 2026-08-07). It stops the swing only when the pre-pass already decided the crosshair is genuinely resting on it (see SNOW_SEE_THROUGH).   // solid and no pine here — a real obstruction stops the swing.
      return 0;                                        // water, surface scatter and foliage are not obstructions — walk on to what is underneath (user)
    });
    if (mainHit === 1) return true;
    if (mainHit === 2) return false;
    // Nothing along the ray held a full bite (a heavily notched trunk). Take whatever is there rather
    // than letting the swing do nothing at all.
    if (voxRay(P.x, smoothEye, P.z, vx, vy, vz, CHOP_REACH, (x2, y2, z2) => {
      if (y2 < 1 || y2 >= WY) return false;
      if (aimBody && phBodyIdAt(x2, y2, z2) && bodyTool(phBodyIdAt(x2, y2, z2)) && phChopBody(x2, y2, z2, C_RAD, C_LAST, C_BITE, okBody)) return true;   // the crosshair must be ON a body and the sampled voxel INSIDE one — the same exactness the main march gets from its bAt gate. Ungated, this took a 15-voxel bite out of any felled log within radius 10 of ANY voxel on a 107-long ray.   // …and a reduced bite off a fallen trunk still counts   // still the aimed material only: the whole point of the safety net is a notched TRUNK, and letting it fall back to needles is the bug it would be papering over
      if (aimWood && S && physChopAt(x2, y2, z2, C_RAD, S, C_LAST, C_BITE, isWood).hit) return true;   // …still wood only, for the same reason the main march is
      if (cut && aimWood && woodTab[W[gwrap(x2, WX) + y2 * WX + gwrap(z2, WZ) * WX * WY]] && phChopDecor(x2, y2, z2, C_RAD, C_LAST, (v) => !!woodTab[v])) return true;   // …and the last splinters of a stump   // same rule as the main pass: the crosshair has to be on wood
      return false;
    })) return true;
    return false;                                     // the swing never bit anything
  };
