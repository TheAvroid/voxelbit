  // ── ONE ANCHORED-SUPPORT RULE ──────────────────────────────────────────────────────────────────────
  // Every runtime mutation of W already funnels through gpuPatch, so THAT is the choke point: the cells
  // that actually changed drive one resolver instead of four boxes centred on the impact point. What
  // replaced four disagreeing "support" definitions (buried-model-course, box-floor, any-box-face,
  // any-non-loose-voxel-within-26-hops) is a single question — CAN THIS CELL REACH THE STATIC GROUND —
  // answered in O(1) by hmap, plus two floods that deliberately do NOT conduct anchoring to each other.
  //
  // THE ASYMMETRY IS THE WHOLE DESIGN. A previous attempt let one flood traverse wood AND needles
  // interchangeably; that made a crown and its trunk the same component, so a crown was lifted whole and
  // stranded every branch tip, cone and neighbouring tree's needles resting against it. MEASURED, that
  // took the floater count from 86 to 1007. Here:
  //   · the STRUCTURE flood may not ENTER a DRAPE cell — a crown can never join a wood component, so it
  //     can never be lifted as one, and that failure is structurally impossible rather than tuned away;
  //   · the DRAPE flood may not enter a STRUCTURE cell — it may only TERMINATE on one that is ITSELF
  //     anchored, which closes phFallLoose's worst hole (any occupied non-loose voxel used to count as
  //     support, including one that was itself floating).
  // DRAPE consumes anchoredness; it never conducts it. That also retires FALL_HOPS: the hop limit only
  // ever existed because a symmetric graph merged the canopy into one blob, and it was a property of this
  // one pine asset that would silently mis-tune for any other.
  //
  // CONDUIT is the third state creature grid stamps have always needed. stampApply writes only where the
  // cell is empty or foliage, so a stamped cell was NEVER structure — treating it as empty in the
  // STRUCTURE flood is exactly right and punches no hole. In the DRAPE flood it is traversable but never
  // liftable and never an anchor: a perched cardinal conducts support across its own body (no false
  // orphan behind it) and grants none (a clump hanging off a bird is not held by the bird).
  //
  // 26-CONNECTED IN BOTH. Pine needles attach to branches diagonally; a 6-connected test calls attached
  // clumps unsupported and drops them, which strands their neighbours. 26's failure mode is "a thing
  // touching at a corner reads anchored" -> one floater left. 6's is "real geometry deleted" -> and THAT
  // manufactures floaters. The chosen failure mode, everywhere in here, is: leave a rare floater, never
  // wrongly drop real geometry. A false negative is inert and gets another attempt the moment anything
  // within 26 cells of it is disturbed; a false positive is irreversible and strands everything that was
  // resting on what it took.
  const SUP = {
    IGNORE: 0, FLUID: 1, STRUCTURE: 2, DRAPE: 3, CONDUIT: 4,
    CLASS: new Uint8Array(256),                        // palette id -> class. CONDUIT is not in here: it is the stampedIdx lookup, which is per-CELL, not per-id.
    retry: [],                                         // seeds whose flood ran out of budget — re-queued so 'undecided' can never mean 'forgotten'
    cap: 1 << 20,                                    // ── STRUCTURE flood ceiling ── 2000 first (a cap hit was READ AS ANCHORED, so anything bigger hung in the air forever), then 32768 — which still could not decide the case it exists for: a big rocks26 formation is 56k voxels, so cutting one loose hit the cap on every try and floated permanently. That is the rock in the user's screenshot. This clears any mass a player can sever. Terrain never pays it: the walk exits on its FIRST anchored cell, so only a genuinely detached component is ever walked to the end.
    // ── 3000 -> 1<<17 (2026-08-17, THE OAKS) ── this is the DRAPE half of the pair above, and it was
    // sized for a PINE canopy, which is many small tufts: pine5.vox's needles come apart into clumps
    // of a few hundred and 3000 was never close to binding. An oak crown baked from a .glb is not that
    // shape at all - it is ONE 26-connected shell, measured at 2,468 / 5,874 / 1,372 / 17,314 / 26,478 /
    // 60,303 / 77,505 leaves for the seven models, so FIVE of the seven exceeded the cap with their
    // whole crown in a single component.
    // That is not a lost verdict, it is a queue that cannot drain: a capped flood returns undecided,
    // supFlush re-queues the seed, and next frame it walks 3000 cells and gives up again - forever, for
    // every disturbed leaf. It is the same failure SUP.cap was raised to 1<<20 for when a 56k rocks26
    // formation could not be adjudicated, and it wants the same answer.
    // THE WALK IS NOT THE COST IT LOOKS LIKE. A DRAPE flood TERMINATES on the first anchored STRUCTURE
    // cell it touches, and every leaf on a standing tree is a few hops from its own branch, so the full
    // 77k walk only ever happens for a crown that really is severed - which is exactly the case that has
    // to be decided rather than deferred.
    drapeCap: 1 << 17,
    msBudget: 2.0,                                     // ms/frame, checked BETWEEN components — never mid-flood, so a component is always resolved as a whole
    maxPasses: 4,                                      // cascade rounds per frame: a lift appends its vacated cells to the same queue
    qMax: 400000,
    q: [], qh: 0, qs: new Set(),                       // the dirty queue: flat indices, deduped, PERSISTENT UP TO qMax — a cell the budget could not reach this frame is still there next frame, but a cell that arrives when qMax are already pending is DROPPED and never re-seeded. That is a real hole and the one place in this system where a question is lost rather than deferred; see the overflow branch in supPush, which counts every one and records the first few.
    res: new Set(),                                    // resolved THIS pass: cleared between cascade passes so a lift is honestly re-adjudicated, never within one so a 2000-cell anchored blob is walked once, not once per seed
    ancS: new Set(), flS: new Set(),                   // per-pass memo of the component verdict, both ways (see supHeld) — a standing pine is walked once, not once per needle
    busy: new Set(),                                   // components currently on the walk stack: they may not be used as anyone else's anchor, which is what breaks cross-class cycles
    blocked: 0, blockedNow: 0, on: true,
    guessed: false,                                    // raised when supHeld answered ANCHORED off a NESTED CAP HIT — a walk that ran out of budget rather than one that found an anchor. supFlush clears it per SEED and re-seeds that seed when it is set, which is the same treatment a top-level undecided flood already gets, so erring anchored stays a delay instead of becoming permanent. Deliberately NOT raised by the recursion-depth guard beside it: that one fires structurally on every pine and re-seeding it builds a queue backlog (measured — see the note there).
    stats: { queued: 0, carried: 0, passes: 0, ms: 0, msMax: 0, structFloods: 0, drapeFloods: 0,
             capHits: 0, depthHits: 0, lifted: 0, liftedVox: 0, erasedSmall: 0, erasedBig: 0, blockedFrames: 0,
             overflow: 0, reclaims: 0, hmapLower: 0, tooBig: 0, liftedDrape: 0, liftedStruct: 0, guessRedo: 0 },
    log: [],                                           // capped ring of what was lifted, for __vb.support()
    refused: [],                                       // capped ring of components the resolver DECLINED to drop, with where and why. The tooBig branch below leaves real geometry suspended on purpose (erasing it is worse), so when a player reports a floating chunk this is the record that says whether that is what happened.
  };
  { const C = SUP.CLASS;                               // default STRUCTURE: an id that never appears in W is never classified, and a NEW one is far likelier to be terrain than drape
    C.fill(SUP.STRUCTURE); C[0] = SUP.IGNORE;
    for (let i = 1; i < 256; i++) if (foliaTab[i] || floatTab[i]) C[i] = SUP.DRAPE;   // needles, grass, blooms, twigs, cones
    C[SNOW[0]] = SUP.DRAPE; C[SNOW[1]] = SUP.DRAPE;
    for (const v of [WATER_T, WATER_B, LAVA_T, LAVA_B, LAVA_R, LAVA_Y]) C[v] = SUP.FLUID; }   // LAST, so fluid can never be overwritten. Water is FLUID whether or not the ice flag has flipped solidTab[WATER_T]: a lake reaches the ground by construction, and a rule that walked it would flood the whole body and never find an anchor.
  // ── THE MOSS CAP COMES AWAY WITH THE ROCK (user 2026-08-19: "when breaking the rocks with moss on top of it,
  // the moss doesnt go with the chunk that was broken off. this was already fixed with the snow landing on the
  // rocks. fix it for the moss.") ── and the fix is the SAME fix, not a second one: mossCap (world/terrain.js)
  // lays the cap as its OWN voxel in the cell ABOVE each sky-facing rock voxel — deliberately, so the stone
  // keeps pickOnlyTab and its sun sheen while the green keeps the float-material class. That makes a moss cap
  // structurally identical to a SNOW cap, and the two places that already hand a bitten or lifted mass its
  // blanket (the cap scan in phChopDecor, the addS walk in supDrop) both key on snowTab. Moss was simply not in
  // the set that travels, so it stayed behind for the resolver to pick up as its own little drape component —
  // which is exactly what "the moss doesnt go with the chunk" looks like.
  //
  // WHY A TAB OF ITS OWN RATHER THAN THE floatTab CLASS IT ALREADY WEARS. floatTab carries the PINECONE ids,
  // and cone ids ARE the pine's bark ids (see the coneTab note in assets/material-tabs.js) — as are the twig
  // browns. Both carry paths walk STRAIGHT UP from a taken voxel until the material changes, so keyed on
  // floatTab a shovel bite at the foot of a pine would walk the whole TRUNK into the chunk. Nor snowTab: snow
  // has melt and thaw queues, the landSnowAt 3-layer cap and the supPush landed-flake skip hanging off it, and
  // moss wants none of that. One flag, read by the same two loops.
  //
  // THE THREE RAMPS BELOW ARE EXACTLY WHAT mossCap CAN WRITE, and none is shared with anything structural:
  // GRASS is minted through addCol, which never dedupes, so it is distinct from the MOSS ground ramp it was
  // sampled from; OAKMOSS is palOwn-reserved for this and nothing else; TWIGPINK is filtered off the pink
  // twigs' own voxels by channel order (r>b>g), so the twig BROWNS — which are bark ids — are not in it.
  // GRASS also dresses the loose 1-4 voxel STRANDS on the forest floor, and that is a feature rather than a
  // leak: a strand is one column resting on the soil, so a shovel bite under one left it hanging over its own
  // hole — the same bug, and it now rides the chunk for the same reason.
  const mossTab = new Uint8Array(256);
  for (const i of [...GRASS, ...OAKMOSS, ...TWIGPINK, ...TWIGWHITE]) mossTab[i] = 1;   // TWIGWHITE rides with TWIGPINK: same scatter, same class, only the crown above it differs
  // Which ids may a generation-time orphan sweep DELETE. Derived, not hand-listed: everything except foliage
  // and wood. A canopy can read as detached in its own model and a trunk is the tree, so neither is ever
  // deleted blind; that leaves terrain — stone, strata, dirt, moss, sand, ore — which is what gorge carving
  // orphans. Built here so the worker gets the SAME array (serialised below) instead of a second guess.
  const ORPHAN_OK = new Uint8Array(256);
  for (let i = 1; i < 256; i++) ORPHAN_OK[i] = (!foliaTab[i] && !woodTab[i] && SUP.CLASS[i] !== SUP.FLUID) ? 1 : 0;
  // ── THE ANCHOR ── O(1), no walk to bedrock. hmap is the first AIR voxel of the STATIC ground column and
  // it deliberately excludes trees, foliage, mushrooms, snow and creature stamps (stampTree writes W
  // directly and never touches it; stampModel raises it only for mode 2). That exclusion is what makes it
  // an anchor oracle rather than a height field. A standing pine is anchored for free: stampTree bases the
  // model at groundMin - sink with sink in [5,8], so 5-8 trunk courses sit BELOW hmap — the same courses
  // phFlood seeds on, with no special case. y<=1 is true bedrock: rockRowSpan fills every column from 0 and
  // stampCave carves from max(1, floorY), so y0/y1 are never carved by anything in worldgen.
  // The oracle is sound only while hmap is never STALE-HIGH, which is why the choke point below only ever
  // LOWERS it. Stale-low is harmless (terrain is contiguous, so the flood finds an anchored neighbour in a
  // few hops); stale-high would wrongly anchor.
  // ── …AND STALE-HIGH IS EXACTLY WHAT HAPPENS (user 2026-08-07: "there should be no floaters at all") ──
  // two ways, both of which this oracle used to answer ANCHORED without proof:
  //   * stampModel mode 2 RAISES hmap over the stamped body, and that is how every BOULDER is placed. So a
  //     rock's own voxels all sit below the raised hmap and read as terrain. Mine a slot through a formation
  //     and the part above it is still "below hmap" — declared attached, forever. That is the floating rock.
  //   * carving a void INSIDE a column never lowers hmap (the choke point only lowers it when the TOP comes
  //     away), so everything above a horizontal cut is still below hmap. Cut clean through a hill and the top
  //     is genuinely detached and still reads anchored.
  // The shortcut is only ever sound while the column beneath the voxel is UNBROKEN, so that is now what it
  // checks — but only for columns something has actually removed a voxel from, which is a vanishing fraction
  // of the world, so the hot path stays O(1). Returning false is not "it is floating": it means "I cannot
  // shortcut", and the flood then finds the real answer through a neighbouring intact column. That keeps the
  // failure direction the same as everywhere else in here — never wrongly drop, only decline to guess.
  // Per-column: is this column's hmap still a PROOF of solid ground all the way down? Cleared by anything
  // that empties a voxel here, and — since 2026-08-08 — by stampModel mode 2, which raises hmap OVER a
  // stamped body and so leaves the column describing something it is not standing on.
  const supCarved = new Uint8Array(WX * WZ);
  let supColMemo = new Map();                          // per-pass cache of the downward scan, so a flood over one boulder pays it once per column
  // ── WINDOW COLUMN -> WORLD COLUMN ── the window is a WX-wide torus over the world columns [winOX, winOX+WX),
  // so a window index must be un-wrapped before it can be compared with anything that lives in WORLD space (a
  // rigid body, the player). Declared HERE, above every consumer, rather than beside the resolver several
  // thousand lines down: phWakeNear used to inline "the same arithmetic" exactly because these were below it,
  // and the inlined copy dropped the modulo — wrong for every column left of the wrap point, which is half the
  // window and includes the player's own feet whenever the seam sits in the near half (see phWakeNear).
  const supWorldX = (gx) => winOX + (((gx - gwrap(winOX, WX)) % WX) + WX) % WX;
  const supWorldZ = (gz) => winOZ + (((gz - gwrap(winOZ, WZ)) % WZ) + WZ) % WZ;
  // ── WAKE THE RIGID BODIES WHEN THE WORLD MOVES UNDER THEM ── assigned once the physics block exists, several
  // thousand lines below; gpuPatch sits above it and a const read before its declaration is the "stuck on
  // uploading world" failure in this file, so it goes through a hook the same way birdDeath does.
  let phWakeHook = null;
  const supAnchored = (ii) => {
    const gy = ((ii / WX) | 0) % WY;
    if (gy <= 1) return true;
    if (SUP.CLASS[W[ii]] === SUP.FLUID) return true;
    const gx = ii % WX, gz = (ii / (WX * WY)) | 0, col = gx + gz * WX;
    if (gy >= hmap[col]) return false;                 // above the static surface — never trivially anchored, exactly as before
    // ── THE "UNTOUCHED COLUMN" SHORTCUT IS GONE (2026-08-08) ── it answered ANCHORED for any column nothing
    // had been emptied from, on the premise that hmap then describes solid ground all the way down. It does
    // not: stampModel mode 2 RAISES hmap over the body it stamps — that is how every boulder, rock formation,
    // mushroom cluster and fallen log is placed — so a model's own voxels sit "below the surface" in columns
    // nobody ever carved. All of them were therefore unconditionally anchored, and severing such a thing left
    // the part whose columns the player had not personally carved hanging forever: never proven attached,
    // never asked again, never lifted. MEASURED: a 460-voxel mass with 9 voxels of clear air beneath it,
    // verdict "supAnchored". That is the floater that applies to EVERYTHING, because everything scenic is
    // stamped — which is exactly what the reports said and what a dozen fixes aimed at the resolver missed.
    // Marking the column at stamp time cannot work: stampModel is also serialised into the generation WORKERS,
    // which have no supCarved (a main-thread array), so it throws at boot. The scan below answers the same
    // question honestly instead, memoised per column per pass.
    // The LOWEST gap in the column is the one number that answers every height at once: a voxel is standing
    // on solid ground iff no gap lies strictly below it. Cached per pass, so a flood over one boulder pays
    // the walk once per column rather than once per voxel.
    let lo = supColMemo.get(col);
    if (lo === undefined) {
      lo = 0;
      const top = Math.min(WY - 1, hmap[col]);
      for (let y = 1; y < top; y++) if (!W[gx + y * WX + gz * WX * WY]) { lo = y; break; }
      supColMemo.set(col, lo);
    }
    return lo === 0 || lo >= gy;                       // no gap at all, or every gap is at/above this voxel → genuinely standing on the column
  };
  // Declared HERE, beside SUP rather than beside the resolver, because gpuPatch calls it and gpuPatch sits
  // several thousand lines above the physics block: a const referenced before its own declaration has run
  // is the "stuck on uploading world" failure in this file, and there is no reason to court it.
  // Set for the duration of ONE gpuPatch whose cells provably orphan nothing — the snow thaw, and only it.
  // See the melt block: both melts clear the TOP voxel of a snow column, and under this game's own drape
  // rules there is nothing that can be resting on such a cell. Without it a thaw parks ~94,000 unanswerable
  // questions in front of every real one.
  let supMute = false;
  const supPush = (ii) => {
    if (supMute) return;
    // Every mutation of W funnels through here, so this is the one place that can notice a column has gained
    // a void. Set BEFORE the dedupe return, or the second touch of an already-queued cell would miss it.
    if (!W[ii]) {
      supCarved[(ii % WX) + (((ii / (WX * WY)) | 0) * WX)] = 1;
      // ── A CLEARED VOXEL CAN ONLY ORPHAN SOMETHING THAT TOUCHED IT ── with nothing at all in the 26
      // neighbourhood there is no question to ask, and the walk would find an empty component anyway. That
      // early-out matters enormously during a storm: clearing snow used to queue a flood per cell and the
      // backlog was measured at 368,000 entries against a 2 ms/frame budget.
      //
      // ── BUT THE PROBE HAS TO BE ALL 26, NOT THE LAYER ABOVE (2026-08-08) ── it read only the 3x3 directly
      // OVERHEAD, on the reasoning that a cleared voxel can only orphan what was RESTING on it. That is
      // gravity's intuition and it is not this graph's rule: support here is 26-connected REACHABILITY to an
      // anchor, so a cell's path to the ground can run sideways or downwards through the cleared voxel just as
      // easily as a load can sit on top of it. Cut the one link holding a spur that reaches out horizontally —
      // an arch, a cantilever, a branch of a rock formation, a ledge — and with open sky overhead the removal
      // queued NOTHING. Not "answered wrongly": never asked, so no later pass would ever revisit it and the
      // spur hung there permanently. MEASURED with a built test case: severing the single load-bearing voxel
      // of a lateral arm queued 0 cells. This is the floater that survived a dozen fixes, because every one of
      // them was aimed at the resolver's verdict and the resolver was never consulted.
      // Cost is 26 array reads instead of 9 on cleared cells only, and the two big sources of cleared cells —
      // a thaw and a landing — are already handled above and below this by exact rules of their own.
      const uy = ((ii / WX) | 0) % WY;
      const ux = ii % WX, uz = (ii / (WX * WY)) | 0;
      var anyNb = false;
      for (let ay = -1; ay <= 1 && !anyNb; ay++) {
        const ny = uy + ay; if (ny < 1 || ny >= WY) continue;
        for (let az = -1; az <= 1 && !anyNb; az++) for (let ax = -1; ax <= 1; ax++) {
          if (!ax && !ay && !az) continue;
          if (W[gwrap(ux + ax, WX) + ny * WX + gwrap(uz + az, WZ) * WX * WY]) { anyNb = true; break; }
        }
      }
      if (!anyNb) return;
    }
    // ── A LANDED FLAKE IS ANCHORED BY CONSTRUCTION ── landSnowAt only ever lays snow on a surface that already
    // read as solid from below, so queueing it asks the resolver a question with a known answer. It was doing that
    // hundreds of times a frame during a storm (supFlood + supFlush measured 1.3 s of every 4 s of js). This skips
    // the ADD only: the moment anything underneath is REMOVED that removal funnels through here itself, and the
    // flood picks the drape up from below exactly as before, so it opens no route to a floater.
    // ── A LANDED FLAKE IS ANCHORED BY CONSTRUCTION, WHATEVER IT LANDED ON ── this used to require STRUCTURE
    // underneath, so every flake settling on other snow or on a crown's needles still queued a flood. With
    // canopy snow that is most of them, and the backlog reached 322,000 entries against a 2 ms/frame budget —
    // which is what left real floaters sitting in the air for minutes after it snowed. landSnowAt only ever
    // places snow on a surface that already read solid from below, so anything non-empty under it is support.
    // The moment that support is REMOVED, the removal funnels through here itself and the flood picks the
    // drape up from below exactly as before, so this opens no route to a floater.
    if (snowTab[W[ii]] && W[ii - WX]) { SUP.stats.snowSkip = (SUP.stats.snowSkip || 0) + 1; return; }
    if (SUP.qs.has(ii)) return;
    // ── THE ONE PLACE A QUESTION IS THROWN AWAY ── qMax is a real bound (the dedupe Set, not the array, is what
    // costs), so a seed arriving on a full queue cannot be kept and nothing re-seeds it later: whatever that cell
    // was holding up stays unasked until something else within 26 of it is disturbed. It is not persistent, and
    // the declaration above no longer claims it is. Counted always, and the first few are recorded with WHERE, so
    // a reported floater can be matched against a drop instead of being hunted in the resolver's verdict logic.
    if (SUP.q.length - SUP.qh >= SUP.qMax) { SUP.stats.overflow++;
      if (SUP.stats.overflow <= 8 && SUP.refused.length < 32) SUP.refused.push({ n: 0, id: W[ii] || 0, x: supWorldX(ii % WX), y: ((ii / WX) | 0) % WY, z: supWorldZ((ii / (WX * WY)) | 0), why: 'queueOverflow', cap: SUP.qMax });   // capped at 8 so a storm cannot crowd the tooBig refusals out of the ring
      return; }
    SUP.qs.add(ii); SUP.q.push(ii); SUP.stats.queued++;
  };

