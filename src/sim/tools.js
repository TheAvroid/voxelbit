  // @module — the voxel-accurate melee ray, the stone tools’ reach/bite constants, and the hoe’s tilled earth
  // @exports AIM_FORGIVE, AIM_R, AXE_SCALE, CHOP_AIM, CHOP_RAD, DIG_SCALE, KNIFE_SCALE, PICK_SCALE, REACH_3D, REACH_H, SKUNK_ANIM_MUL, aimHitId, chopSwing, tillRevert, tilled, toolCanBreak, voxRay
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
  // ── WHAT THE LAST SWING DECIDED ── the aim pre-pass's verdict plus WHICH branch of the march spent the
  // swing, published so a test can ask instead of inferring. Inferring is what made "the axe hits foliage"
  // take three passes to pin down: a voxel diff shows leaves gone and cannot say whether that was the leaf
  // branch firing on a crosshair that was really on bark, or a wood-filtered sphere behaving correctly on a
  // crosshair that was really on a leaf. Read with __vb.chopAim().
  const CHOP_AIM = { path: 'none' };
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
      // ── AND A MOSS CAP ON STONE IS NOT AN OBSTRUCTION TO A PICK ── the third and last ray that had to learn
      // this (the aim pre-pass and the swing's own march are the other two). It matters because THIS one feeds
      // the BLOCKED sound and toolCanBreak, and the note under this function is explicit that a sound which
      // disagrees with the swing is worse than no sound. Without it a pick swinging clean through a birch cap
      // into the rock still reported the cap here — a digOnly id the pick cannot take — so the audio's answer
      // was "blocked" while the swing was carving stone. Measured before this: aimId 66 (the cap) in the birch
      // against 147 (the stone) in the pine, for the identical swing. Same test as the other two: mossTab with
      // stone DIRECTLY under it is a cap and not turf. NOT gated on the pick — see the note at the march.
      if (id && mossTab[id] && y > 1
          && pickOnlyTab[W[gwrap(x, WX) + (y - 1) * WX + gwrap(z, WZ) * WX * WY]]) return 0;
      if (id !== 0 && !foliaTab[id] && !floatTab[id] && !(isWater(id) && !solidTab[id])) return id;
      return 0;                                        // water, grass strands, twigs and needles are not obstructions — walk on to what is underneath
    }) || 0;
  };
  // ── CAN THE HELD TOOL TAKE THIS MATERIAL AT ALL ── hoisted to module scope (user 2026-08-28: block.mp4
  // when a tool cannot break something) so the AUDIO can ask the same question the SWING does. It has to be
  // the one rule and not a copy: a sound that disagrees with the swing is worse than no sound, because it
  // teaches the player the wrong thing about their tool. chopSwing's own toolTakes now delegates here.
  const toolTakesFor = (it, v) => {
    const knife = KNIFE_IT > 0 && it === KNIFE_IT, cut = it === 1 || knife;
    const pick = PICK_IT > 0 && it === PICK_IT, dig = SHOVEL_IT > 0 && it === SHOVEL_IT;
    return digOnlyTab[v] ? (dig || knife) : (pickOnlyTab[v] ? (pick || knife) : (axeOnlyTab[v] ? cut : true));
  };
  const toolCanBreak = (v) => v !== undefined && !!toolTakesFor(heldIt(), v);   // …and this is the audio's entry point: what is in my hand right now, against this id
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
    const D_DEEP = (window.__CDEEP !== undefined ? +window.__CDEEP : CHOP_DEEP) * base;   // the DECOR branch's own copy of the push — same number, its own name so an A/B can move it without touching the felled-body path
    const C_CUT = C_RAD;   // the CARVE sphere. Same as the reach today; kept as its own name because they are different questions — how far a swing may look for material, and how big a bite it takes out of it.   // the WOOD paths — a cutting edge OWNS wood, so this is its doubled sphere; C_DEEP is a depth offset, not a reach, and is untouched
    const C_BITE = knife ? Math.max(4, Math.round(PH.chopBite * KNIFE_BITE * base)) : Math.round(PH.chopBite * base);   // the WOOD chunk, at this tool's sphere   // the knife takes HALF the axe's chunk (user); the PICK and SHOVEL take proportionally MORE, because a wider sphere that still lifts 30 voxels is just a slower way to dig the same hole
    // A minimum bite bigger than the sphere can physically hold makes every swing miss (that is how the
    // knife looked broken before). ~2r^2 is a fair count of solid cells a half-filled sphere of radius r
    // can offer, so the requirement can never exceed what is reachable.
    const cap = Math.max(2, Math.floor(2.0 * C_RAD * C_RAD));
    // ── THE WOOD MINIMUM STAYS (tried and REVERTED, user 2026-08-20) ── it was briefly dropped to 2, the
    // floor phChopDecor is handed on every other material, so that wood would "have the same chunk mechanics
    // as the pick and shovel". It does not, and the user reverted it to this — ea97b7f's version — the same
    // day: the pick and the shovel chew into a solid mass, where an axe is supposed to cut a NOTCH, and taking
    // the first two voxels the ray meets makes every swing after the first shave the near fringe of the hole
    // it just made instead of biting deep. That is verbatim what CHOP_LAST's own comment records happening at
    // a floor of 4, and it is what the chunk coming off the tree looked wrong for.
    // So the axe still marches until it finds somewhere that can give a FULL piece, and CHOP_LAST's 15 is the
    // fallback for a trunk too notched to offer one. Do not re-propose the decor floor here.
    const C_MIN = Math.max(2, Math.min(Math.round(CHOP_MINBITE * kS), cap));
    const C_LAST = Math.max(2, Math.min(Math.round(CHOP_LAST * kS), cap));
    // ── THE HIVE'S OWN SPHERE AND BITE ── exactly what the decor branch below already hands a hive voxel
    // (soi/soiR both answer base * 0.5 for it, since a hive is nobody's own material), lifted out here so the
    // hollowed-hive continuation can use the SAME numbers from a cell that holds no id to derive them from.
    // Nothing is scaled: this is the identical sphere and the identical bite, moved, not widened — see the
    // note on the continuation for why the radius was never the lever.
    const H_RAD = CHOP_RAD * base * 0.5;
    // ── WHAT THE HELD TOOL CAN ACTUALLY TAKE ── one rule, used by the AIM march below and by the rigid-body
    // gate further down, which used to carry its own copy. Hoisted because the aim needs it: see the
    // see-through note at the leaf/wood test.
    const toolTakes = (v) => toolTakesFor(it, v);   // ONE rule, hoisted above this function so the bounce SOUND asks exactly what the swing asks — see toolTakesFor
    const H_BITE = knife ? Math.max(2, Math.round(PH.chopBite * KNIFE_BITE * base * 0.5)) : Math.max(2, Math.round(PH.chopBite * base * 0.5));   // no DIG_BITE term, unlike mBite below: a hive is axeOnlyTab, so the continuation is gated on `cut` and the shovel can never reach this   // axe 15, stone knife 4 — the two numbers the BEE_BREAK_F note is written against
    const okHive = (v) => !!HIVE_TAB[v];               // …and it takes HIVE and nothing else: the branch it hangs from is bark, one voxel above its top course and well inside the sphere
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
    // ── AND AN OAK CROWN IS NOT A PINE CROWN (user 2026-08-20: "im hitting the wood but its hitting the
    // foilage behind it") ── 6 was measured off a PINE, whose trunk stands inside a few voxels of needle, and
    // it is the whole reason the rule works there. An oak's bole sits in the MIDDLE of a broad canopy: sighted
    // from outside the crown at branch height the ray crosses 17 to 29 voxels of leaf before it reaches bark
    // (measured on an 86k oak at ten heights: 8, 9, 12, 17, 22, 24, 25, 27, 29, 29). Every one of those is
    // past 6, so the wood behind was never accepted, aimId stayed the LEAF, aimWood went false — and with the
    // aim on a leaf `okMat` falls to its permissive arm and the swing takes a sphere of FOLIAGE. The crosshair
    // is on the trunk and the axe eats the canopy, which is exactly the report.
    // Tool-dependent, because the number means "how far can the thing I am trying to hit hide": an AXE is for
    // wood, so it looks through a whole oak canopy for it. Everything else keeps the pine number — the knife
    // included, which is aimed at creatures and has no business reaching through a crown for a trunk.
    // ── AND 24 WAS STILL A GUESS, SO IT IS DERIVED NOW (user 2026-08-21: "when the axe is 'hitting' the wood,
    // it's still not voxel accurate. it's hitting the foliage behind the wood of the tree") ── the 2026-08-20
    // pass measured ten sight lines into an 86k oak and got 8, 9, 12, 17, 22, 24, 25, 27, 29, 29, then picked
    // 24 off that list. FOUR OF THE TEN ARE OVER IT. On those lines `t - leafT` fails the test, the pre-pass
    // falls to its `return 3` below, leafHit is 3 — and the whole swing is spent by phChopLeaves on a sphere
    // centred on the FIRST LEAF, whatever the crosshair was resting on. Crosshair on bark, axe eats canopy.
    // That is not a number that wants nudging again; it wants to stop being a number somebody chose. What it
    // has to cover is one CROWN, because that is the thing standing between the eye and the bole, and the bake
    // already carries that measurement: OKMARGIN (world/terrain.js) is the widest oak's half-footprint, kept
    // beside the models so it grows if the bake does. Twice it is the full width, i.e. the longest chord any
    // ray can take through the widest crown in the game — so a ray that entered a crown can always reach the
    // bole inside it, from any angle, on any oak.
    // BE HONEST ABOUT WHAT THIS COSTS. 120 is wider than the swing can reach at all (CHOP_REACH is 53
    // horizontal, 107 at the limit), so for an AXE the rule is now effectively "any wood on the ray wins".
    // The old note here argued against exactly that — "a player who really is aiming at leaves must still get
    // them" — and it is the price: with an axe in hand you can no longer cut a leaf that has a trunk behind it.
    // Leaves against open sky, an isolated bush, the outside of a canopy all still cut, and every OTHER tool
    // keeps the pine's 6 (the knife included), so the leaf-cutting the argument was protecting is reachable by
    // switching hands. Against that, the axe now does the one thing it is for wherever it is pointed, which is
    // what has been asked for twice.
    // The knife keeps 6: it is aimed at creatures and has no business reaching through a crown for a trunk.
    const LEAF_SEE_THROUGH = (window.__LST !== undefined) ? window.__LST : (axe ? OKMARGIN * 2 : 6);   // window.__LST overrides it live (same pattern as __UNIBR / __TFREEZE) so the pine number and the axe number can be A/B'd against the SAME tree — the world re-randomises on every reload, so that is the only way to compare them honestly                        // voxels of needles a trunk can hide behind — a pine crown is a few voxels of needle around the bole
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
      // ── AND WOOD IS NEVER PASS-THROUGH (user 2026-08-21: "still not voxel accurate on the axe, it's still
      // hitting the foliage when it should be hitting the wood. seems to be worse in the oak/cherry forests") ──
      // this line walked the aim ray straight through EVERY TRUNK IN THE GAME, and that is not hyperbole: there
      // are exactly four woodTab ids (49-52) and all four are floatTab, so `woodTab` is a strict subset of the
      // set this line skips. Everything below it that reads wood — the `!firstLeaf` stop, the whole leaf/trunk
      // see-through — was unreachable for a ray that had already met a leaf. aimId stayed the LEAF, aimWood went
      // false, and phChopLeaves took the entire swing. MEASURED with __vb.chopAim(): crosshair on an oak bole,
      // `{ aimId: 197 (a leaf), aimWood: false, leafHit: 3, path: 'leaves' }`.
      // WHY IT READS AS AN OAK/CHERRY PROBLEM. It is not one — it is every tree — but a PINE hides its bole in a
      // few voxels of needle and many sight lines reach bark with no leaf in front at all. Those lines fall to
      // `aimSky` (aimId 0 -> aimWood true) and work by accident. An oak bole sits in the middle of a broad crown,
      // so essentially every line meets a leaf first and there is no accident left to save it.
      // AND IT IS WHY RAISING LEAF_SEE_THROUGH DID NOTHING, twice: 6 -> 24 -> 120 all tuned a comparison that
      // could never be evaluated.
      // THE TAB CANNOT BE FIXED INSTEAD, and that is worth writing down. floatTab is set for twigs by walking
      // STICKV/STICKB's own voxels (assets/material-tabs.js) — and a twig is MADE OF BARK, the same palette ids
      // as the trunk it fell from, which is the id-sharing this codebase keeps re-learning. There is no id-level
      // difference between "a twig lying on the floor" and "the bole of an oak" to key on, so the distinction has
      // to be made HERE, where the question is "is this what the crosshair means".
      // WHAT IT COSTS: a twig on the ground is no longer walked past by the aim either, so an axe swing that
      // grazes one now stops on it. That is a fair trade — an axe swing at the forest floor does nothing today
      // (soil is digOnlyTab) — but it is the 2026-08-07 "pass-through is not an aim" rule shrinking by exactly
      // the wood ids, and if twigs ever start stealing swings this line is why.
      // ── A MOSS CAP ON STONE IS COVER, NOT A TARGET (user 2026-09-03: "you're not letting me hit through the moss
      // when hitting the rock. let me hit through the moss to hit the rock") ── the line below already walks the ray
      // through a cap, but only for a floatTab one, and the BIRCH forest's cap is not floatTab: mossCap lays it in
      // BIRCHMOSS, which is BIRCHGRASS's own ids, and assets/material-tabs.js marks those solid + decor + digOnly
      // because they are also the birch GROUND (the palette had no free ids when that cap was authored, so the two
      // share). To the ray it was therefore ordinary solid ground: it STOPPED on the cap, aimId became a digOnly id,
      // the pickStone guard below went false because a cap is not pickOnlyTab, and the swing was then filtered to
      // material a pick may not take — so a mossy birch boulder could not be mined at all. The pine forest was fine
      // throughout because ITS caps are GRASS, which is floatTab. Same root cause as the floating-cap bug fixed in
      // 0562996, and the second half of it.
      // ASKED PRECISELY, so it cannot swallow the ground it shares ids with: see through this voxel only when what
      // is DIRECTLY UNDER it is stone, which is what makes it a cap rather than turf, and only for the PICK, which
      // has no business taking either. Every other tool still aims at a cap exactly as it did, so the shovel can
      // still dig one.
      if (!inB && mossTab[lv] && pickOnlyTab[W[gwrap(lx, WX) + (ly - 1) * WX + gwrap(lz, WZ) * WX * WY]]) return 0;
      if (!inB && !woodTab[lv] && !foliaTab[lv] && (floatTab[lv] || (isWater(lv) && !solidTab[lv]))) return 0;   // …and FOLIAGE is never pass-through either: a BUSH's leaves carry floatTab where a tree's do not (measured: id 58 float true, ids 92/197 false), so any path that reaches this test with a leaf is one ordering change away from walking through the very thing the crosshair is on and taking the wood behind it (user 2026-08-22)   // ── PASS-THROUGH IS NOT AN AIM (user 2026-08-07) ── grass strands, blooms, twigs, cones and open water: the march's obstruction test and aimHitId both walk straight THROUGH these, but this pass stopped on them and set aimT to a STRAND's distance — which makes every `t >= aimT` gate below vacuous, since the ray passes that distance almost immediately. Ice keeps solidTab, so a frozen lake is still a thing you can aim at.
      if (!inB && snowTab[lv]) {                       // ── SNOW IS COVER, NOT A TARGET (user 2026-08-07: "the tool shouldn't register the snow, but the material under the snow") ── the same rule as LEAF vs TRUNK: remember the blanket, then keep walking for what it is sitting on. landSnowAt caps a stack at 3 layers, so the budget below always reaches the ground.
        if (!firstSnow) { firstSnow = true; snowT = t; if (!firstLeaf) { aimId = lv; aimBody = inB; aimT = t; } }   // …and the snow itself is only the FALLBACK aim, for a drift with nothing reachable under it
        return 0;
      }
      if (!firstLeaf) { if (!firstSnow || t - snowT <= SNOW_SEE_THROUGH) { aimId = lv; aimBody = inB; aimT = t; } return 2; }   // solid, and no leaf in front of it — the ordinary march decides
      // ── …AND A BEEHIVE IS THE OTHER THING THAT HIDES BEHIND LEAVES (user 2026-08-19: "have the bees also
      // target the player when the player hits the behive and breaks it") ── the see-through was WOOD only, and
      // a hive hangs from a BRANCH INSIDE an oak crown, so the ray meets a leaf a voxel or two before it on
      // almost every line. The leaf then won (the `return 3` below), the whole swing went to phChopLeaves, and
      // phChopLeaves returns TRUE — so the march that carries the hive's own branch was never reached and the
      // player's axe quietly cut needles instead. MEASURED with the axe aimed dead at the hive centre from 13
      // voxels, eight swings a world: in THREE worlds of six the hive lost not one voxel, and in two more it
      // stalled part-eaten at 28-31 of 54 against a break threshold of 27. So the hive never crossed
      // BEE_BREAK_F, hiveChopped was never even asked, nothing reached the ledger, and no bee answered — which
      // is the whole report. Swatting a bee works because nothing stands in front of a bee in the air; the
      // hive is the case where something always does. Same rule, same LEAF_SEE_THROUGH budget and same shape
      // as the trunk's, because it is the same question: what did the crosshair really mean.
      // ── …AND ONLY IF THE TOOL COULD TAKE IT (user 2026-08-19: "Im hitting a foilage voxel with a pick, but
      // its not breaking. instead its registering on the wood") ── the see-through was unconditional, so a leaf
      // in front of a trunk ALWAYS surrendered the aim to the wood, whatever was in the hand. With an axe that
      // is the point of the rule. With a PICK it is a dead swing: wood is axeOnlyTab, so the pick cannot cut
      // what it just aimed at, and the leaf it CAN break was thrown away to get there. The rule now only fires
      // when the tool can actually break the thing behind the leaf — otherwise the leaf really was the target,
      // which is the `return 3` below. Same question as before ("what did the crosshair mean"), asked with the
      // hand included.
      if ((woodTab[lv] || HIVE_TAB[lv]) && t - leafT <= LEAF_SEE_THROUGH && toolTakes(lv)) { aimId = lv; aimBody = inB; aimT = t; return 2; }   // TRUNK — or the HIVE hanging in the crown — behind the needles → that is what the player meant
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
                : ((v) => !digOnlyTab[v] && !pickOnlyTab[v] && !axeOnlyTab[v]))) : null));   // …and WOOD is owned too (user 2026-08-28: "the pick seems to break wood? it shouldnt be able to break wood") — see the okMat twin below, same leak, same fix
    // ── WHICH TOOL MAY CUT THIS BODY (user 2026-08-08: "when a rock topples over from cutting it in half, now
    // I cant keep taking chunks out of the rigid body… the same with the rigid mushroom… the tree still works
    // fine") ── the whole rigid-body branch was gated on `cut`, i.e. on holding a cutting edge. That is right
    // for WOOD and wrong for everything else, and it is exactly why a felled tree stayed choppable while a
    // toppled rock and a felled mushroom did not: a rock wants the PICK and a mushroom yields to anything, and
    // neither is `cut`, so the branch was skipped and the swing fell through to the world path — which finds
    // nothing, because a rigid body is not in W. The material's own rule is the one that should decide, and
    // that rule already exists a few lines below for the same materials while they are still in the ground.
    const bodyTool = toolTakes;   // the same rule the aim march uses — one copy, hoisted above (see toolTakes)
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
    CHOP_AIM.aimId = aimId; CHOP_AIM.aimBody = aimBody; CHOP_AIM.aimT = aimT; CHOP_AIM.aimWood = aimWood;
    CHOP_AIM.aimSky = aimSky; CHOP_AIM.leafHit = leafHit; CHOP_AIM.leafT = firstLeaf ? leafT : null;
    CHOP_AIM.gap = (firstLeaf && aimT !== Infinity) ? +(aimT - leafT).toFixed(2) : null;
    CHOP_AIM.lst = LEAF_SEE_THROUGH; CHOP_AIM.firstLeaf = firstLeaf ? firstLeaf.slice() : null; CHOP_AIM.path = 'none';
    CHOP_AIM.rockHit = false; CHOP_AIM.woodHit = false; CHOP_AIM.foliaHit = false;   // ── WHAT DID THIS SWING LAND ON? ── pick on stone, axe on wood, anything on foliage. NOT CHOP_AIM.leafHit, which is the aim pre-pass's see-through state and a NUMBER. cleared HERE, on the one line every
    // swing runs before it decides anything, so a swing that bites nothing cannot leave the last one's answer standing and hand the
    // sound to the wrong blow. ui/audio.js reads it (CHOP_AIM is exported and audio.js is 6 fragments below this one) to pick the
    // impact take: rock under a pick has its own recording, everything else keeps the generic break takes.
    // ── A PICK AIMED AT STONE IS NEVER A LEAF SWING (user 2026-09-02: "sometimes the leaf sound plays when I
    // hit the rock with a pick. prevent that from happening") ── this branch runs BEFORE the decor and body
    // arms below, so whenever foliage is in front of the real target it wins the swing outright and sets
    // foliaHit, and ui/audio.js reads that to choose the take. Rocks are exactly where this bites: the boulder
    // models carry MOSS on their crowns and field stone sits in grass, so the ray meets foliage on the way to
    // the stone and the swing is scored as leaves. The pick then works the rock anyway and you hear leaves.
    // The aim pre-pass already knows what the swing is FOR — aimId is the id it resolved — so this asks it
    // rather than re-deriving anything: a pick whose aim landed on pick-only material skips the leaf arm and
    // falls through to the decor/body branch that sets rockHit. Every other tool and target is untouched, so
    // an axe still shears a crown and a pick in bare foliage still cuts it.
    // ── AND THE MOSS RIDES THE CHUNK, IT IS NOT CUT (user 2026-09-02: "the moss isnt traveling with the rock
    // chunk … this is how it used to be") ── the mossy cap is the reason a pick-on-stone swing ever reached
    // this arm: the cap is the first thing the ray meets, so `firstLeaf` is a moss voxel and the leaf arm was
    // claiming the swing (and its sound) on what is plainly a rock. The guard is right and stays.
    // What was wrong was the first repair: it CARVED the moss here before falling through to the stone. That
    // answered "I cant hit the moss" and broke the thing the cap exists for. world/terrain.js (mossCap) spells
    // it out — the cap is laid in GRASS ids rather than MOSS ids precisely because GRASS is floatTab / SUP.DRAPE,
    // so "chop the rock and the drape lifts with it instead of hanging in the air". Cutting it here consumed
    // the voxels the drape rule was going to hand to the chunk, so the chunk flew to the player bare.
    // So a pick-on-stone swing does nothing at all in this arm now. It takes the rock, and the drape comes
    // with the rock, which is both what the user remembers and what mossCap was designed around.
    const pickStone = !!(pick && aimId && pickOnlyTab[aimId]);
    if (!pickStone
        && (leafHit === 3 || (leafHit === false && firstLeaf))          // the leaf wins: nothing behind it, or the ray ran out inside the crown
        && (CHOP_AIM.path = 'leaves') && (CHOP_AIM.foliaHit = true)
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
           phChopBody(x, y, z, C_RAD, C_MIN, C_BITE, okBody))) { CHOP_AIM.path = 'body'; CHOP_AIM.hitId = bAt; CHOP_AIM.rockHit = pick && !!pickOnlyTab[bAt]; CHOP_AIM.woodHit = axe && !!woodTab[bAt]; CHOP_AIM.foliaHit = !!leafSndTab[bAt]; return 1; }   // …a knocked-loose clump of leaves or fronds is still foliage   // …a felled LOG is wood under an axe, so bucking one sounds like chopping one   // …and a boulder CHUNK lying on the ground is still rock under a pick, so it gets the rock take too   // deeper bite first, same as the standing trunk   // a FELLED trunk lying in the way is choppable too — tested before treeShapeAt so the tool hits what is actually nearest
      const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      // ── AND THE SWING WALKS THROUGH A MOSS CAP TOO, NOT JUST THE AIM (user 2026-09-03: "hitting the rocks
      // through moss is still bugged … the pick doesnt hit through the moss. also sometimes when hitting a pine
      // forest moss, it doesnt pass through and only picks up the moss") ── the previous pass fixed the AIM ray
      // and stopped there, which was half the job: this march is a SECOND, independent ray, and the cap was still
      // the first thing it met. Both of the user's symptoms are that one fact, in the two bands:
      //   · PINE. Its cap is GRASS — decorTab and restricted to no tool at all — so the decor branch below
      //     matched on the FIRST voxel and the pick chopped the moss. "only picks up the moss".
      //   · BIRCH. Its cap is BIRCHMOSS, which is BIRCHGRASS's ids and therefore digOnly, so the same branch
      //     refused it (a pick is not a shovel) and the march fell through to the obstruction test and STOPPED.
      //     Nothing happened at all.
      // Skipping it here fixes both, and it is the same question the aim ray now asks, asked once more where the
      // swing actually spends itself: a mossTab voxel with STONE directly under it is a cap and not turf.
      // `return 0` is "keep walking" — the sphere is then centred on the stone.
      // ── AND THE CAP BELONGS TO THE ROCK, WHICHEVER TOOL IS SWUNG (user 2026-09-03: "can you have it when the
      // player hits the moss on the rocks, it pulls the stone with it") ── all three of these tests were gated on
      // the PICK at first, which fixed the pick and left the other half standing: with anything else in hand the
      // cap was still an ordinary decor voxel, so a bare hand or an axe stripped the moss off a boulder and left
      // the rock bare — the same "only picks up the moss", in a different tool. Ungated, a cap on stone is not a
      // target for anybody: every ray walks through it to the rock and the ROCK's own rule then answers. A pick
      // takes the stone and the cap rides the chunk out with it, which is the ask; a shovel or an axe is refused
      // exactly as it is on bare stone rather than quietly harvesting the moss. What is given up is digging a cap
      // off a rock and leaving the rock standing, which is the behaviour being removed on purpose.
      if (id && mossTab[id] && y > 1
          && pickOnlyTab[W[gwrap(x, WX) + (y - 1) * WX + gwrap(z, WZ) * WX * WY]]) return 0;
      // ROCK needs the pick, WOOD needs a cutting edge, everything else soft yields to whatever is in hand.
      // …and the carve is confined to that same material, so the sphere cannot spill into the next one.
      // ── …AND A BEEHIVE IS ITS OWN MATERIAL FOR THIS PURPOSE (2026-08-19) ── it is neither dig- nor pick-only,
      // so it fell through to the permissive third arm, which admits every other decorTab id in the sphere —
      // and the one thing always within 2.5 voxels of a hive is the oak BRANCH it hangs from (bark is decorTab
      // + woodTab + axeOnly). Nearest-first then spends part of the bite on bark, which is why a hive that
      // should have gone 54 -> 39 -> 24 was MEASURED stalling at 28-31: the two swings the threshold is sized
      // for never lifted 15 hive voxels each. This is the same rule the line above already states — the carve
      // is confined to the material the crosshair is on — applied to the one decor id it did not cover.
      // ── AND WOOD IS ITS OWN MATERIAL HERE TOO (user 2026-08-20: "remove all of the chunk mechanics
      // associated with the axe, and bring them in again matching the stone pick. make it wood instead. no
      // difference") ── the arm below is the literal mirror of the pick's: the pick confines its sphere to
      // pickOnlyTab, so the axe confines its sphere to woodTab. Without it a wood id fell through to the
      // permissive third arm, which admits every other decorTab id in range — and the thing always within ten
      // voxels of a trunk is its own canopy, so the bite would have been spent on leaves.
      const okMat = HIVE_TAB[id] ? okHive : (woodTab[id] ? isWood : (digOnlyTab[id] ? ((v) => !!digOnlyTab[v]) : (pickOnlyTab[id] ? ((v) => !!pickOnlyTab[v]) : ((v) => !digOnlyTab[v] && !pickOnlyTab[v] && !axeOnlyTab[v]))));
      // ── …AND WOOD IS OWNED TOO (user 2026-08-28: "the pick seems to break wood? it shouldnt be able to break wood") ── the arm above names the
      // two materials a swing may not spill into and forgot the third. That is not an oversight about hive ids: `id` here is a SOFT decoration
      // (grass, a fern, a flower, a mushroom — anything carrying decorTab and no tool tab), the swing gate has already let every tool through
      // because soft decor yields to whatever is in hand, and the sphere it then hands phChopDecor was confined to "not soil, not stone" — which
      // admits BARK. Grass grows at the foot of every trunk, so a pick aimed at the grass took the tree: MEASURED, one swing, path 'decor',
      // hand = pick, one wood voxel gone. The shovel and the bare hand had the identical hole. Wood is axeOnlyTab (assets/material-tabs.js), the
      // same table the swing gate reads two lines above, so naming it here is the same rule the comment already states — the carve is confined to
      // the material the crosshair is on — applied to the third owned material rather than only the first two.
      // IT ALSO SHARPENS THE SOFT SWING ITSELF: a bite that could spend itself on a neighbouring trunk is a bite the mushroom did not lose.
      // ── A MUSHROOM IS THE TOOL'S OWN MATERIAL (user 2026-09-02: "make the mushroom chunk match the same
      // size as the other tools. using a pick on the mushroom for example is much too small. make it match the
      // chunk of stone") ── a mushroom carries decorTab and no tool tab, so it was OFF-material for everything:
      // soi halved the bite and soiR halved the reach, and mushQ (removed below) quartered the bite again. With a pick
      // (base 1.0) that is bite 4 in a radius-2.5 sphere, against stone's bite 30 in a radius-10 one — an
      // EIGHTH of the chunk, which is the "much too small" being reported.
      // BOTH HALVES HAVE TO MOVE OR NEITHER DOES. The bite is a ceiling on voxels taken, not a promise: a
      // half-filled sphere of radius r only offers about 2r^2 cells, so radius 2.5 can supply ~12 and a bite of
      // 30 would simply never be reached. Raising the bite alone would have measured as no change at all.
      const mushOwn = !!mushTab[id];                  // …so it takes the own-material bite AND the own-material sphere, which together are exactly what stone gets
      const mS = mushOwn ? base : soi(id);            // this material's BITE factor (own = full, anything else = half) — unchanged by the SOI doubling
      const mR = mushOwn ? base * SOI_OWN : soiR(id); // …and its REACH, the doubled one on the tool's own material
      // the BITE follows the sphere it came out of — scaled from PH.chopBite, not from C_BITE, which
      // already carries the tool's own factor and would apply it a second time.
      // ── SUPERSEDED 2026-09-02 by the own-material block above, which removed mushQ entirely; kept because it
      // records what the quarter bite was FOR and the measurement behind it ── A MUSHROOM COMES APART IN FOUR,
      // NOT TWO (user 2026-08-22: "cut the red mushroom in 4s instead of
      // halves. when the player cuts it down") ── the number of pieces a decoration yields is just its voxel
      // count divided by the bite, so halving the bite on mushroom ids doubles the pieces and changes nothing
      // else: same sphere, same nearest-first order, same materials. Keyed on mushTab (assets/material-tabs.js),
      // which is the cluster's own id set, so ferns and logs beside it keep the bite they have always had.
      // The bite, not the sphere: a tool's own-material SOI is inert on ordinary geometry (measured twice).
      // MEASURED: halving alone was not enough. The bite went 15 -> 8 and the mushroom still came apart in two,
      // because a single cap is small enough that 15 and 8 both round to the same two bites — the piece count is
      // ceil(voxels / bite), not voxels / bite, so it only moves when the bite crosses a boundary. A QUARTER of
      // the original bite is what actually yields four (user 2026-08-22: "the red mushroom still breaks in 2
      // instead of 4"). Read the live number off __vb.chopAim().bite rather than re-deriving it.
      // ── AND A SOFT DECORATION IS CARVED WHERE THE CROSSHAIR IS, NOT BEHIND IT (user 2026-08-28: "improve the
      // voxel accuracy when hitting the mushroom in the pine forest? its really bad") ── the carve below is centred
      // D_DEEP along the ray, and that push is a WOOD AND STONE rule: it exists so a chunk out of a bole or a boulder
      // has depth instead of coming off as a flat surface cap (see the note at the call). It was applied to every
      // material because the three tools share this branch, and on soft decor it is the wrong shape of wrong:
      //   · the sphere is the OFF-MATERIAL one, radius CHOP_RAD * 0.5 = 2.5, so a 1.5 push moves the centre 60% of
      //     the way to its own rim — where on wood or stone the tool's own sphere is 10 and the same push is 15%.
      //   · the bite is small (a mushroom's is a QUARTER, 4 voxels), so the swing takes barely more than the centre
      //     cell and its nearest neighbours — which are all on the far side of the voxel the player pointed at.
      // MEASURED on one mushroom, five swings from a fixed crosshair: the carve centre landed one voxel behind and
      // one below the aimed voxel every time, the aimed voxel itself SURVIVED the first four swings, and what the
      // player got instead was a scatter of pits around the spot they hit. That is the whole report.
      // The chunk-depth argument does not apply here and neither does the fallback: with no push the two tries below
      // are the same sphere, so the second is skipped rather than gathering it twice.
      const mD = (woodTab[id] || pickOnlyTab[id] || digOnlyTab[id]) ? D_DEEP : 0;   // the push, for the materials it was written for
      // ── AND THE QUARTER BITE IS GONE WITH IT ── mushQ was 0.25 on mushTab ids, which is how "cut the red
      // mushroom in 4s instead of halves" (user 2026-08-22) was delivered: piece count is ceil(voxels / bite),
      // so quartering the bite quadrupled the pieces. THIS DELIBERATELY REVERSES THAT ASK — parity with stone
      // and a fixed four-way split are the same number pulling opposite ways, and the newer instruction wins.
      // A cap is small enough that a full bite takes it whole, so expect one chunk where there were four.
      const mBite = knife ? Math.max(2, Math.round(PH.chopBite * KNIFE_BITE * mS)) : Math.max(2, Math.round(PH.chopBite * mS * (dig ? DIG_BITE : 1)));
      // ── AND A BEEHIVE IS THE ONE PIECE OF DECOR THAT ANSWERS BACK (user 2026-08-17: "if the player breaks
      // open the beehive, have bees fly out of it, attacking the player") ── asked AFTER the carve, because the
      // question is "is the hive open NOW". hiveChopped (sim/life/slots.js) counts what is left of the box and
      // does nothing at all until it is past BEE_BREAK_F, so a chip is still just a chip. That is the whole of
      // the tool's involvement: it posts a fact about the world to a ledger and returns. It does not know what
      // a bee is, and the swing itself is untouched — same gate, same sphere, same bite, same return value.
      // ── AND LEAVES ARE NOT WHAT THE PLAYER MEANT (user 2026-08-20: "when the axe hits the wood, it should
      // take chunks of the wood, not leaves") ── this is the cost of routing wood through the decor branch.
      // That branch fires on the FIRST decor voxel the ray meets, and a crown is decor: aim at a trunk through
      // the needles and the march stopped on a leaf, so `id` was foliage, okMat fell to the permissive arm and
      // the swing took a sphere of LEAVES centred on the leaf. The old wood path never had this problem — it
      // carried `t >= aimTW`, i.e. do not cut before the thing the crosshair is actually on.
      // The aim pre-pass has already done the hard part: it looks THROUGH up to LEAF_SEE_THROUGH of foliage and
      // reports the trunk behind it, so aimWood is true and aimT is the distance to the WOOD. All that is
      // needed is to let the march walk past the leaves in front of it, which is what this does.
      // Only while the aim means wood. A crosshair genuinely resting on a leaf has aimWood false and the leaf
      // branch above it still takes the leaf, which is the "a leaf yields to ANYTHING" rule.
      if (aimWood && id && foliaTab[id] && !woodTab[id] && t < aimTW) { return 0; }   // `return 0` = keep walking. NOT `continue`: this body is a CALLBACK (see the note at the voxRay call above), so a continue here is a syntax error and the whole bundle stops parsing — a game that never boots at all, which is exactly what it did
      // ══ ONE CARVE FOR EVERY MATERIAL, WOOD INCLUDED ══ this line used to carry `!woodTab[id]`, which is what
      // sent a standing trunk to the tree path below and gave the axe a chunk rule of its own. It is gone. Wood
      // now takes the identical branch stone and soil take: the same phChopDecor, the same CHOP_RAD * mR sphere
      // centred on the marched voxel, the same mBite, the same confined okMat, and the same "take what is
      // there, refuse only when the sphere is empty". There is no longer an axe chunk mechanic to differ.
      // THE TREE STILL FALLS. phChopDecor only moves voxels, so the connectivity check that drops a severed
      // trunk is run straight after it — phTreeSettle (sim/chop-tree.js), the same block physChopAt runs, on
      // the shape this column belongs to. Latched into the march's own `S` so a swing pays treeShapeAt once.
      // ── AND THE SPHERE SITS INSIDE THE MATERIAL, NOT ON ITS SKIN (user 2026-08-21: "the chunk doesn't break off
      // the same as when the player is point blank. it takes off a larger flatter chunk vs a smaller spherical
      // chunk … it also is coming off the wrong place") ── this is a regression with a paper trail: the tree path
      // that used to carve standing wood took its bite CHOP_DEEP further along the ray for exactly this reason,
      // and its own note records the symptom in the user's words from 2026-08-05 — "centred on the surface
      // sample, most of it hung in open air and the chunk came out a flat cap". The 2026-08-20 pass deleted that
      // path to give wood the pick's mechanics and the decor branch never had the push, so the flat cap came back.
      // WHY IT READS AS A DISTANCE PROBLEM. It is really an ANGLE problem, and angle covaries with distance: up
      // close you look UP or DOWN at a bole, so the ray enters through the near face and drives on into solid
      // wood, and a sphere centred there is surrounded. Standing back the line goes flat, the ray clips the near
      // face tangentially, and half the sphere hangs in open air — so nearest-first spends the whole bite
      // spreading sideways across the bark instead of biting in. Same numbers, different shape, which is exactly
      // "larger and flatter" and "off the wrong place".
      // DEEPER FIRST, SURFACE AS FALLBACK — the identical two-try shape the felled-body branch above already
      // uses, and for the identical reason: if the pushed centre has run out the far side of a thin branch, the
      // surface point still gets its turn rather than the swing doing nothing.
      // ALL THREE TOOLS, because they share this branch and the user asked for parity — a chunk out of a boulder
      // wants to be a chunk for the same reason a chunk out of a trunk does. C_DEEP already carries the tool's
      // own scale (CHOP_DEEP * base), so nothing here singles the axe out.
      // window.__CDEEP overrides it live for A/B, the same pattern as __LST / __TFREEZE.
      if (id && decorTab[id] && (digOnlyTab[id] ? (dig || knife) : (pickOnlyTab[id] ? (pick || knife) : (axeOnlyTab[id] ? cut : true)))
          && (phChopDecor(x + vx * mD, y + vy * mD, z + vz * mD, CHOP_RAD * mR, mBite, okMat, woodTab[id] ? PH.chopCourse : 0)
              || (mD && phChopDecor(x, y, z, CHOP_RAD * mR, mBite, okMat, woodTab[id] ? PH.chopCourse : 0)))) {
        if (HIVE_TAB[id]) hiveChopped(x, y, z);
        // ── THE SETTLE BELONGS TO THE TREE THE BITE CAME OUT OF ── and that is not necessarily the one `S`
        // latched. S is taken at the FIRST column the march finds a tree in, which is deliberately open air
        // short of the bole (see the note below), and in a birch stand the crown the ray enters first is
        // routinely a different tree from the trunk it ends on: BKCELL is 44 against footprints up to 61, so
        // the crowns interleave. Flooding the latched tree then re-tests a tree nobody cut - it sheds, or comes
        // down - while the tree that just lost 30 voxels of bole is never asked and stays standing on nothing.
        // Re-resolving at the CARVE column costs the same one treeShapeAt a swing (this branch returns), and it
        // is the column the axe actually took material from, which is the only column the question is about.
        if (woodTab[id]) { const Sw = treeShapeAt(x, z); if (Sw) S = Sw; if (S) phTreeSettle(S); }   // …and only wood asks: nothing else can be holding a tree up
        CHOP_AIM.path = 'decor'; CHOP_AIM.hitId = id; CHOP_AIM.rockHit = pick && !!pickOnlyTab[id]; CHOP_AIM.woodHit = axe && !!woodTab[id]; CHOP_AIM.foliaHit = !!leafSndTab[id];   // canopy, flowers and ferns all sound like foliage - one table, filled in assets/material-tabs.js woodTab and foliaTab are disjoint (bark vs needle), so these cannot both be true   // pickOnlyTab is set beside decorTab on every rock and ore id (assets/material-tabs.js), so this is exactly 'the pick's own material', not a colour test
        CHOP_AIM.hitT = +t.toFixed(2); CHOP_AIM.rad = +(CHOP_RAD * mR).toFixed(2); CHOP_AIM.bite = mBite; CHOP_AIM.hit = [x, y, z]; CHOP_AIM.ctr = [+(x + vx * mD).toFixed(1), +(y + vy * mD).toFixed(1), +(z + vz * mD).toFixed(1)];   // ── THESE THREE WERE DEAD (2026-08-28) ── they sat at the tail of the `//` comment on the line above, so nothing ever assigned them and __vb.chopAim() never carried the numbers this note says a test must read. Restored, plus the impact voxel and the pushed sphere centre, which is what an ACCURACY claim ('the swing lands in the wrong place') is actually about   // the SPHERE and the BITE this swing actually used — 'chopping at a distance does not have the same soi' is a claim about these two numbers, so a test must be able to read them rather than infer them from a voxel diff
        return 1; }   // free gate, the id is already in hand
      // ── AND A HIVE THE CROSSHAIR HAS ALREADY PUNCHED THROUGH IS STILL A HIVE (user 2026-08-19: "fix the hive
      // stall") ── the branch above fires only when the MARCHED VOXEL is itself a hive voxel, and beehive.vox
      // is a HOLLOW SHELL: 54 voxels wrapped one deep around a 3x3x3 pocket of air. Two swings from a fixed
      // crosshair hole the near wall and then the far one, and from the third swing on the ray's own line runs
      // clean through the box without touching a single hive voxel. The branch stops being asked, and the 24
      // that remain — the rim around the tunnel — cannot be reached again from that stance at all. MEASURED as
      // a hard stall: swings 3, 4, 5 … removed nothing, for ever, while moving the mouse two voxels resumed it
      // instantly. That is not a reach problem and the radius is NOT the lever (raising a tool's own-material
      // sphere measured a null result twice, and this sphere is never even created); it is the GATE.
      // The tree path already answers exactly this question for a bole — "a hole punched clean through a trunk
      // is still aiming at the tree", see aimSky above — and it can, because treeShapeAt says which tree owns a
      // column whether or not any wood is left on the ray. hiveBoxAt is that same query for a hive, so this is
      // the decor twin of the S latch and not a new mechanism: if the ray is walking through the AIR INSIDE a
      // hive's own 5x5x5 box, the crosshair is on the hive, and the swing carves it. phChopDecor never required
      // its centre to be occupied — it gathers shells outward — so centring in the tunnel collects the rim.
      // Nothing else moves: same sphere, same bite, same hiveChopped, same BEE_BREAK_F. A chip is still a chip,
      // and the ambush this feature refused stays refused, because the ray has to pass THROUGH the hive's box
      // to get here — which is aiming at it, not clipping a crown on the way to a trunk.
      if (!id && cut && hiveBoxAt(x, y, z) && phChopDecor(x, y, z, H_RAD, H_BITE, okHive)) { hiveChopped(x, y, z); CHOP_AIM.path = 'hive'; CHOP_AIM.woodHit = true; return 1; }   // WOOD: the hive is the one carve that set no material at all, and it hangs on a BIRCH — so chopping one was a way to get the generic click off a tree (user 2026-08-26)   // `!id` only: an occupied cell is the branch above's business, whatever it holds   // hiveBoxAt memoises its oak scan per cell, so a whole march costs one 3x3 probe
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
        // ── THE TREE CARVE IS GONE (user 2026-08-20) ── physChopAt used to cut standing wood here, with its own
        // sphere centre, its own minimum and its own chunk. That was the axe chunk mechanic, and it is removed:
        // wood is carved by the decor branch above, which is the pick's. physChopAt itself stays — it is what
        // __vb.physChopFull drives for tests, and phTreeSettle was lifted out of it — but nothing in a swing
        // calls it any more. What is left of this block is the AIM: S latches the tree so the branch above can
        // settle it, and the return below stops a swing that is not this tree's from walking on underground.
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
      // ── AND THIS CARVE HAS TO SETTLE THE TREE TOO (user 2026-08-26: "the trunk gets cropped") ── it did
      // not, and neither did the fallback below, and between them they are where nearly every swing after
      // the first one goes. The decor branch above needs a SOLID voxel at the crosshair to fire; once the
      // first bite has opened the notch the ray passes through air where the wood was, so `id` is 0, that
      // branch is skipped, and the swing lands here instead. This path removes wood and returns, and
      // NOTHING then asks whether the tree is still standing.
      // MEASURED on four birches, one swing per 600 ms: swing 0 reports path 'decor' and every swing after
      // it reports path 'none' while `total` keeps falling ~4 voxels a swing. Orphans appear on swing 1 or
      // 5 - 7,443 of 7,601, then 15,390 of 15,575 - and never clear. The tree is cut clean through and
      // stays exactly where it is, which is the whole of "the trunk gets cropped": the axe eats the bole
      // and the tree above it never moves. Rare on a pine or an oak only because their trunks are thick
      // enough that the ray keeps finding fresh wood at the crosshair and the decor branch keeps firing.
      // ── AND THE PATH IS NAMED ── CHOP_AIM.path read 'none' for every one of those swings, which says a
      // swing did nothing when it had in fact just carved the tree. That is what made this take four
      // separate reproductions to find, so both arms are labelled now.
      if (cut && aimWood && t >= aimTW && phChopDecor(x, y, z, C_CUT, C_BITE, (v) => !!woodTab[v], PH.chopCourse)) {
        const Ss = treeShapeAt(x, z); if (Ss) phTreeSettle(Ss);
        CHOP_AIM.path = 'stump'; CHOP_AIM.hitId = id; CHOP_AIM.woodHit = axe; return 1; }   // the carve here is woodTab-filtered, so only the TOOL is still in question   // …and, like the tree path, it stands aside for a crosshair resting on a felled log
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
      // ── AND THIS PATH RE-RESOLVES TOO (user 2026-08-27: "when the player knocks down one birch tree, another
      // one falls?") ── it was handing physChopAt the LATCHED S, and physChopAt both carves in that shape's
      // frame and settles it (sim/chop-tree.js). S is taken at the first column the march finds a tree in,
      // which is deliberately open air short of the bole, and in a birch stand the crown the ray enters first
      // is routinely a different tree from the trunk it ends on — BKCELL 44 against footprints to 61, and 119
      // overlapping pairs measured in one stand. So the axe could carve and topple the NEIGHBOUR while the
      // tree under the crosshair was never touched. The decor branch (~535) and the stump branch (~620) were
      // both given this re-resolve when that bug was first found; this one, the last-resort bite, was missed.
      // Gated on the marched voxel actually BEING wood so the extra treeShapeAt runs where a carve is really
      // about to happen and not once per voxel down the whole ray — the same condition the branch below uses.
      if (aimWood && woodTab[W[gwrap(x2, WX) + y2 * WX + gwrap(z2, WZ) * WX * WY]]) {
        const Sl = treeShapeAt(x2, z2) || S;           // the column the bite comes out of; the latched shape only as a fallback, so reach is unchanged
        if (Sl && physChopAt(x2, y2, z2, C_RAD, Sl, C_LAST, C_BITE, isWood).hit) return true; }   // …still wood only, for the same reason the main march is
      if (cut && aimWood && woodTab[W[gwrap(x2, WX) + y2 * WX + gwrap(z2, WZ) * WX * WY]] && phChopDecor(x2, y2, z2, C_RAD, C_LAST, (v) => !!woodTab[v], PH.chopCourse)) {
        const Sf = treeShapeAt(x2, z2); if (Sf) phTreeSettle(Sf);   // …the same settle the stump path above now runs, and for the same reason
        CHOP_AIM.path = 'last'; CHOP_AIM.woodHit = axe; return true; }   // …and wood-only here too   // …and the last splinters of a stump   // same rule as the main pass: the crosshair has to be on wood
      return false;
    })) return true;
    return false;                                     // the swing never bit anything
  };
