  // ── VOXEL DESTRUCTION + RIGID BODIES ───────────────────────────────────────
  // Modelled on Teardown (Tuxedo Labs), whose approach is documented by its author:
  //   · detachment is a plain 6-CONNECTED FACE-ONLY flood fill, run per breakage event, not per frame
  //     ("it's just a flood fill, there's absolutely nothing special to it")
  //   · mass / centre of mass / inertia are BRUTE-FORCE SUMS over the voxels, recomputed on every
  //     break ("you just sum up all the voxels ... there is no hand tweaking")
  //   · there is deliberately NO structural-stress model — connectivity alone decides what falls
  //   · voxels collide as SPHERES, not cubes
  //   · contacts are pruned by classifying voxels edge/corner/face/interior; a shipping container goes
  //     from 1000+ candidate contacts to 4 ("that's kind of the key to making it run at this speed")
  //   · solver is sequential impulse / PGS at a FIXED 60 Hz with 8 iterations, and carries NO contact
  //     or pair state between frames — only body state persists
  //
  // A pine's world voxels are a PURE FUNCTION of its tree cell: stampTree writes
  //   A[mx + mz*R.sx + my*R.sx*R.sz]  ->  world (bx+mx, gy+my, bz+mz)
  // so local<->world is an exact affine map and no per-voxel ownership table is needed; treeAt() is the
  // deterministic reverse lookup. That is what makes an "object-local destructible shape" free here.
  const PH = {
    on: !location.search.includes('nophys'),
    chipMax: 120,                                    // orphaned components UNDER this many voxels are erased rather than dropped — see phSeparate in sim/chop.js. A severed pine is thousands, so the fell never trips it
    dt: 1 / 60,                                      // FIXED 60 Hz, like Teardown — the sim must not vary with render fps
    iters: 8,                                        // sequential-impulse iterations per step (Teardown ships 8)
    acc: 0, bodies: [], maxBodies: PHYS_MAX,   // 24 (user 2026-08-11, was 16) — MUST come from the uniform capacity: physB has room for exactly PHYS_MAX bodies and the emit loop clips to it, so a larger sim cap would simply never be drawn
    gravity: 200,                                    // matches the player's GRAVITY so falls read at the same scale
    linDamp: 0.05, angDamp: 0.18, restitution: 0.0, friction: 0.6,
    sleepLin: 1.2, sleepAng: 0.30, sleepFrames: 40,
    // ── HALVED AGAIN, AND THE CEILING RAISED TO ALLOW IT (user 2026-08-22: "cut the tree chunks in half
    // again" then "raise the uniform for the tree chunks") ── 6 x 2 x 2 = 24 pieces was EXACTLY PHYS_MAX, so a
    // single felled oak consumed the entire rigid-body budget: measured pieces 24 / bodies 24, and felling a
    // second tree then evicted a piece of the first, which DELETES it. PHYS_MAX is now 48 (render/buffers.js),
    // so 12 x 2 x 2 = 48 fits with the same headroom-of-one-tree the old numbers had.
    // ── ONE CHUNK SIZE FOR THE WHOLE GAME (user 2026-08-22: "all of the chunks across the game need to be a
    // similar size. the oak trunks are much bigger then the pine tree chunks") ── the split used to be a fixed
    // COUNT, so chunk size tracked tree size: MEASURED, a pine of 8,039 voxels and an oak of 86,365 both broke
    // into 42 pieces, median 143 against 1,592 — an eleven-fold difference. Driving the count from VOLUME makes
    // the chunk the constant and the count the variable, which is the way round the player actually perceives.
    // 700 is the pine's own largest piece today, so pines keep chunks they already had and the oak's come down
    // to match. An 86k oak wants ~123 pieces at that size, which is why PHYS_MAX went to 128.
    // ── THE MOST OF A TRUNK ONE SWING MAY TAKE ── see the cap in sim/chop-tree.js. A pine offers ~126 wood
    // voxels inside the axe's radius and an oak more, so a third of that is past chopBite and neither is
    // affected; a birch offers ~42. chopThin is the floor, so a twig still loses something to a swing.
    chopFrac: 0.6, chopThin: 4,
    // ── …AND THE SAME RULE ON THE PATH A SWING ACTUALLY TAKES ── chopFrac above caps the bite in
    // physChopAt, and physChopAt has not been in a swing since 2026-08-20, when wood was moved onto the
    // pick's carve (sim/tools.js). So the 2026-08-24 fix for "make the birch trees have the same tool
    // physics as everything else" went into code the player never reaches, and its own note — "nobody ever
    // saw them: the first swing already had the tree falling" — stayed true.
    // This is that cap for phChopDecor, and it is expressed in the unit the problem is actually in: COURSES
    // OF TRUNK PER BLOW. MEASURED, wood in the impact's own course inside the axe's sphere — birch 4-8,
    // pine 21-27, oak 33-41. A flat 30 is 0.8 of an oak course and 1.3 of a pine's, which is a notch; on a
    // birch it is FIVE COURSES, a 50 cm slice taken clean through a trunk 3 voxels thick. That is the whole
    // of "it stops rendering a portion of the trunk instead of the chunk mechanic": the piece removed is
    // trunk-shaped and sits exactly where the trunk was, so nothing reads as a chunk coming off.
    // The cap binds exactly when the median course is below chopBite / chopCourse, so 2.0 puts that boundary
    // at 15 voxels — and it is chosen to be INERT above, not tuned. MEASURED on trunks confirmed by an
    // unbroken 14-course run of wood in one column: pine 17-18, oak 18-22, birch 6-8. Every pine and oak
    // swing still takes its full 30, with room to spare as the notch deepens and the measure drifts down a
    // voxel or two; only something genuinely thinner than the axe's own bite can bind it. chopThin is the
    // floor, so a twig still loses something. Do not raise chopBite without re-checking this ratio: the two
    // numbers are one rule, and the boundary between them is the whole design.
    chopCourse: 2.0,
    fellChunkVox: 350, fellLandFrames: 3,   // halved again (user 2026-08-22: "we need smaller chunks to absorb by the player"). An 86k oak wants ~247 pieces at this size, which is why PHYS_MAX went to 256
    fellChunkMax: 600, fellStumpSlots: 8,   // ── THE COARSEST SPLIT STILL WORTH MAKING, AND THE SLOTS HELD BACK FOR THE STUMP ── phShatterTree used to refuse to break at all unless every piece could be exactly fellChunkVox, and a tier-7 oak is 86k voxels = 246 pieces against a PHYS_MAX of 256: measured 243 free in an EMPTY world, so the biggest oaks could never break, ever (user 2026-08-23: "I knocked over a big oak tree and it didnt even fall apart into chunks"). The piece count now takes whatever room there is and only REFUSES below fellChunkMax, so the size still cannot run away to the 10,027-voxel lumps that started this — it just no longer holds the whole tree hostage to the last three slots.
    // ── AND IT BREAKS ON A CLOCK, NOT ON A LANDING (user 2026-08-22: "have the tree turn into chunks after 10
    // seconds of becoming an rigid body") ── which also removes the last thing that needed the topple drive to
    // have finished, so the fall can be left to plain physics.
    // fellRoomTries: after this many ticks of failing to make room, a felled tree BREAKS COARSE rather
    // than staying whole (see phShatterTree). NB: every property below shares this ONE line — a `//` put
    // among them eats the rest of it, which is how fellHitVy..fellCalmAng went missing for a build and
    // sent every tree to the 25 s backstop instead of breaking on impact.
    fellChunkSpan: 22,
    fellBakeR: 120, fellBakeMs: 20000,
    fellBreakMs: 25000, fellStuckMs: 3000, fellRoomPerTry: 8, fellRoomTries: 10, fellHitVy: 8, fellHitFrac: 0.4, fellHitMs: 60, fellHitHoldMs: 400, fellTiltUp: 0.9, fellCalmLin: 0.8, fellCalmAng: 0.04, fellCalmMs: 500, fellMinMs: 2000,   // ── THE FELL BREAKS WHEN IT HITS THE GROUND (user 2026-08-23: "the tree is supposed to break up in chunks when it hits the ground", "make it more sensitive to when it hits the ground") ── MEASURED per-frame on two ~29k-voxel oaks: the fall is a clean vertical-speed arrest. Tree A peaked at 22.3 vox/s down and collapsed to 6.0 at 1.2 s; tree B peaked at 62.8 and collapsed to 2.2 at 0.6 s. AFTER that the body creeps and rolls for another 10-15 s at 2-3.5 vox/s, which is why waiting for it to go quiet broke the tree a quarter of a minute after the player watched it land. So the impact is the arrest, expressed as a FRACTION of the body's own peak fall speed rather than an absolute: fellHitFrac of peak, held fellHitMs, with at least one contact and only once it has really fallen (peak > fellHitVy - a trunk resting on its stump never arms it). Replayed against the recorded frames that fires at 1373 ms and 1196 ms, both within ~0.6 s of the visible landing, and stays disarmed through the free fall. fellHitMs was 200 and is now 60 (user 2026-08-23: "its at the moment the tree hits the ground or rolls on its side, that it falls apart"): the arrest is ONE FRAME wide - MEASURED on a landing pine, vy -40.51 -> -2.91 between consecutive frames with the body's y locking at 195.4 - and no frame of the fall itself comes near the threshold, so the dwell was buying nothing but delay. It is not zero only so a single glitched frame cannot fire it. fellTiltUp is what separates the two arrests a felled trunk makes: it seats on its own CUT FACE while still bolt upright (up = 1.00) and lands on the TERRAIN once it has leaned over, and MEASURED first ground contact happens at up = 0.41 on a tree that bounced and at -0.15 on one that did not - both far under 0.9, and both while the topple drive was still running. So the block is 'upright AND still toppling', not 'still toppling': a trunk that has tilted more than ~26 deg is past its stump and whatever stops it now is the ground. fellCalm* is the fallback for a tree that never falls fast enough to arm the impact test, and fellBreakMs is the backstop for one that never does either.
    // ── AND THE NEW PIECES MUST NOT REST ON EACH OTHER ── the `resting` latch in sim/solver.js turns gravity
    // OFF for a body with any contact and little speed. A shatter makes every piece at once, in contact with
    // its neighbours and all at rest, so the whole cluster held ITSELF up: a trunkless crown hanging in the
    // air over the debris that did fall (user 2026-08-22, screenshot). For this long after a break the latch
    // cannot apply, so gravity acts, the cluster separates, and each piece finds its own ground.
    fellSettleMs: 900,
    // ── ONE NUDGE, NOT A DRIVE ── a cut trunk is a column balanced on its own stump, and real physics leaves a
    // balanced column standing: MEASURED after the topple drive was removed, a severed pine sat at the same y
    // for 20 s and fell asleep after 752 ms — the floating pine trees. This is a single impulse in a RANDOM
    // direction at the moment of the cut, after which nothing steers it, so trees no longer all go the same way
    // (the complaint the drive was removed for) but they do go over.
    fellNudge: 3.2, fellSpin: 0.5,   // 14/4 -> 9/6: "cut the tree trunks in half again" (user 2026-08-22). With the two cross-cuts that is up to 24 pieces, which is maxBodies — phShatterTree's merge folds any it cannot seat into the first piece rather than dropping them
    abAll: new Float64Array([1e30, 1e30, 1e30, -1e30, -1e30, -1e30]),   // union of every live body's world AABB — phBodySolid's O(1) "nowhere near anything" reject
    sleepAngFree: 24,                                // at or under this many voxels, rest is judged on LINEAR motion alone — a cone is 13 and its spin is solver noise, not motion
    retireFar: 64,                                   // …and a SETTLED body up to this size is baked back too, once it is further than retireFarR away — see the sleep site. 64 covers a cone (13) and a needle tuft without touching anything the player would call a log.
    retireFarR: 48,                                  // 3x absorbR: far enough that nobody is mid-walk toward it
    retireMax: 6,                                    // a settled body this small is written back into the grid and its slot freed
    maxProbes: 512, maxContacts: 96, maxCCD: 12,     // maxCCD caps the adaptive substepping (see physStep).
    //   maxProbes was 160, but an 8-voxel bucketing of a 35x93x36 crown yields ~300 buckets — so barely
    //   half the body's surface cells carried a probe, and a narrow stump could pass clean between them.
    //   Probes only cost a grid lookup during contact GENERATION (once per substep), so 512 is cheap.
    treeLifeMs: 600000, chunkLifeMs: 600000, fellLifeMs: 300000,   // fellLifeMs: the pieces of a FELLED tree, 5 min (user 2026-08-22: "make the chunks of the tree dissapear after 5 minutes"). Its own number so the general chunk and the standing-trunk lifetimes are untouched         // ── LIFETIMES (user 2026-08-11) ── EVERYTHING the player made unstatic is deleted 10 min after it broke loose — a felled trunk and a chop chunk alike (was 5 min / 60 s). Two knobs so the trunk can still be retuned apart from the debris, but they are deliberately equal now.
    //                                                 Told apart by noAbsorb, which marks the toppling trunk and nothing else.
    // ── WIDER AND LESS FUSSY (user 2026-08-22: "the player is still not picking up every chunk. maybe make the
    // absorption more sensitive?") ── 16 was a reach for the odd chip; a felled tree now leaves dozens of pieces
    // spread over the whole footprint of the crown, and walking each one down individually is the chore that
    // produced the report. Paired with the stillness rule below being relaxed, since the chunks that got missed
    // were the ones still jostling against their neighbours when the player walked past.
    absorbAgeMs: 1500,                               // ── HOW LONG A CHUNK MUST EXIST BEFORE IT MAY BE COLLECTED ── was a flat 1500 hard-coded in the solver's absorb gate, which is why a piece had to LAND before you could take it; 38128ac made it 0 ("let the player pick up chunks instantly, the moment it turns into a chunk") and this puts the wait back (user 2026-09-02). The CONSTANT stays — the gate in sim/solver.js reads it rather than a literal, so this is the whole switch and there is no second place to keep in step. See that gate for why the velocity test beside it is left alone either way
    absorbR: 26,                                     // …and a body already AT REST on the ground is drawn in from this far (vox, user). 34 -> 26, back to what it was before 38128ac widened it: that widening was the other half of instant pickup ("instant pickup is worth little if you still have to be on top of it"), so it goes back with it rather than being left behind on its own. NB the long-standing claim here that this "matches AUTO_PICK_R" is not true and was not true at 26 either — AUTO_PICK_R is 16 (ui/audio.js), so a chunk has always been drawn in from further than a dropped item
    // ── THE FLIGHT IS HALF THE SPEED (user 2026-09-02: "slow the speed at which the chunks gets absorbed to
    // the player when the tool hits the rock … slow it down by 50%") ── absorbFly 420 -> 840. It is the flight's
    // DURATION in ms, not a rate: sim/solver.js reads k = (now - absorbT0) / absorbFly and smoothsteps it, so the
    // whole arc is parameterised on it and doubling the duration halves the speed everywhere along the curve —
    // the gentle leave and the fast arrival keep their proportions rather than one of them absorbing the change.
    // absorbMs is deliberately NOT touched: that is the WAIT before the chunk sets off, and slowing the flight
    // is not the same ask as making the player stand there longer before anything happens.
    // THEN 25% OF THE SPEED BACK (user 2026-09-02: "increase the speed of the chunk flight duration by 25%"):
    // 840 -> 672. Speed and duration are reciprocal here, so a 25% FASTER flight is a divide by 1.25, not a
    // subtraction of 25% — 840 * 0.75 would be 630 and a 33% speed-up. Net against the original 420: half the
    // speed, then a quarter of it back, leaving the flight 1.6x its original length.
    absorbMax: 2000, absorbMs: 450, absorbFly: 672,  // absorbMax: the ceiling on what may become a rigid BODY at all — a bigger separated component is dusted instead (see the flood-separate path). NOT an absorb limit; that is absorbSize below.    // absorbMs = the WAIT after breaking off before the chunk comes to you (halved from 900, user 2026-08-02);
    // ── RAISED TO COVER EVERY CHUNK THE GAME MAKES (user 2026-08-22: "make sure all chunks can be absorbed no
    // problem") ── 200 predates the felled-tree shatter, whose pieces are exactly fellChunkVox (350) each, so
    // every one of them was over the line and only got through on the fellLoot exemption. Anything the game
    // breaks off is now collectable on the ordinary rule, and the exemption stays as a backstop rather than as
    // the thing holding it up. Kept a little above 350 so a piece that swallows a short tail still fits.
    absorbSize: 600,                                 // above the chunker's natural SPREAD, not just its target: pieces aim at fellChunkVox (350) but a connected piece that swallows its last shell runs to ~460, and at 420 those few were refused while everything around them was collected                                 // ── TOO BIG TO CARRY (user) ── a chunk over this many voxels REFUSES to be absorbed: break it down first. Measured on a felled pine, one tree yields chunks of ~7, 12 and 139 voxels plus 800+ voxel trunk sections; 200 lets an armful through and turns the big sections into something you have to work on. Chopping one splits/shrinks it (phChopBody takes a ~30-voxel bite and can sever a long piece in two) until the parts drop under this and vacuum up normally.
    //                                                 absorbFly = the flight itself, left at 420 ms so the transition stays smooth rather than snapping in
    chopBite: 30,                                    // voxels an axe swing takes out — a FIXED count, so every chunk is the same size (user). 40 -> 30, a 25% reduction (user 2026-08-02)
    absorbY: -12,                                     // height the chunk is absorbed at, RELATIVE TO THE EYE — dropped 3 MORE voxels (user, -9 → -12), so it arrives lower still than the waist chest. (Briefly raised to +1 on 2026-08-02, reverted at user request.) One number to retune.                 // a felled pine is far more complex than Teardown's flat-bottomed container
    chopMinBody: 24, chopBodyBite: 4,                // a rigid body this big is choppable, and this many voxels is a real bite out of one (tiny debris is not a target)
    fellOrphans: 40,                                 // orphan voxels that mean a CROWN came loose (not just an axe chip) — the threshold that kills the birds perched in it
    fallSlow: 0.39,                                  // gravity multiplier for a FELLED TRUNK = 1/k^2 with k = 1.6 — the fall plays out ~60% slower (user). Chips/leaves/decor are unaffected.
    tipHoldMs: 380,                                  // after touchdown the top SITS on the base this long before it starts to go over (user: land on the base every time, then tilt)
    tipArmMs: 1600, tipBlockDepth: 2.2,              // a severed top DROPS onto the stump first and only then starts to tip (user). tipArmMs is the fallback if it never lands; tipBlockDepth pauses the drive while it is DEEPLY penetrating something, so it can never rotate THROUGH the stump. Must sit well above the ~1.3 vox a trunk normally rests at, or the guard fires constantly and the tree stalls half-tipped (measured).
    fellSpread: 1.5,                                 // rad of RANDOM spread either side of the notch direction when a trunk is severed (user: trees all dropped the same way)
    crashThrough: 200,                               // "this body is a TRUNK, not a chunk" — gates the topple drive and its speed ceiling (see phStep). It used to also exempt trunks from foliage contacts; leaves have no hitbox for anything now, so that part is gone.
    stuckMs: 2500, stuckNudge: 40,                   // …and if one hangs up on something SOLID, re-arm the topple every stuckMs with a downward shove
    tipSeed: 0.18,                                   // rad/s the topple STARTS at — a barely-perceptible lean, so the fall has somewhere to accelerate from (user: momentum)
    tipRate: 0.34,                                   // …the linear part of the ramp, reached about halfway over (see the topple drive in phStep)
    tipAccel: 1.3,                                   // …rising to tipRate + tipAccel as it goes over, so it accelerates past balance like a real tree (also /k)
    tipDone: 0.34,                                   // stop driving once the trunk's own up-axis has tilted past ~70 deg from vertical (cos 70 deg) — physics takes the landing from there
    tipMax: 1.0,                                     // rad/s ceiling on a felled trunk's rotation, drive or no drive — the top speed, not the average (user)
    tipMaxMs: 15000,   // …and the drive's hard stop stretches with it
    tipDamp: 0.6,                                    // per-SUBSTEP retention of any rotation that is not about the topple axis — solver noise, and the source of the shudder                                  // hard stop: never drive a topple longer than this, whatever it is caught on
    stats: { chops: 0, voxRemoved: 0, separations: 0, floods: 0, floodVox: 0, dustVox: 0,
             retired: 0, evicted: 0, sparks: 0, unstuck: 0, topples: 0, toppleMaxTilt: 0, ccd: 1, chunks: 0, reclaimed: 0, absorbed: 0, dropped: 0, birdsKilled: 0, bodyChops: 0, bodySplits: 0, decorFalls: 0, snowCarried: 0, coneCarried: 0, mossCarried: 0, lastFellDeg: 0, lastBodyChopMs: 0, lastFloodMs: 0, lastSepMs: 0, stepMs: 0, substeps: 0, contacts: 0, oakShapes: 0 },   // oakShapes = (model, rotation) oak shapes built this session; each is a 10-15 ms build, and a number that keeps climbing means the 3-entry LRU is thrashing
  };
  // ── THE OAKS FALL TOO (user 2026-08-17: "make the oak trees fall with the axe") ─────────────────────
  // Everything below treeShapeAt — the carve, the root flood, the separate, the topple — is written
  // against ONE interface: a shape S that answers "is local cell (mx,my,mz) still this tree's material,
  // and where does it sit in W". A pine satisfies it with MROT, a pre-rotated DENSE array palette.js builds
  // once at load. An oak cannot: OAKV is a SPARSE list of packed voxels per model, there are seven models at
  // four rotations, and the biggest is 86,365 voxels in a 114x112x114 box. Walking that list per query is out
  // of the question, and pre-rotating all 28 up front is 20+ MB of shapes almost none of which will ever be
  // chopped.
  // So the oak's shape is built ON DEMAND and cached: the same dense array a pine gets, minted the first time
  // the axe asks about that (model, rotation) and held in a 3-entry LRU. A build is ~86k scattered writes plus
  // one 6-connected labelling pass — call it 10-15 ms, once, on the first swing at a size of tree this session
  // has not chopped yet; every swing after that is a plain array read exactly as a pine's is.
  const OAK_SHP_MAX = 3;                               // cached (model, rotation) shapes — up to 3 MB each at the giant end. Sized for how the axe is used (one tree at a time, its neighbours the same size), not for how many shapes exist.
  const OAKSHP = new Map();                            // key = k*4 + rot -> the dense rotated shape. Map keeps insertion order, which is what makes the LRU a delete + re-set.
  const OAKID = (() => { const a = new Uint8Array(256); for (let i = 0; i < 256; i++) a[i] = i; return a; })();   // the oak's `remap`: bow.js already resolved OAKV's palette to WORLD ids (stampModel writes `p >>> 24` raw where stampTree writes remap[v]), so the model id IS the world id and phPresent's table is the identity
  const phNb26 = (() => { const a = []; for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) if (dx || dy || dz) a.push(dx, dy, dz); return a; })();   // 26 (dx,dy,dz) triples, flat — see the glue pass in oakShape and phComponent
  let OAK_MAXD = -1;                                   // widest model footprint in voxels; -1 until first asked
  const oakRad = () => {                               // …as a radius in OKCELLs, the oak twin of treeShapeAt's own `r`
    if (OAK_MAXD < 0) { let d = 0; for (const m of OAKV) d = Math.max(d, m.sx, m.sy); OAK_MAXD = d; }
    return Math.ceil(OAK_MAXD / OKCELL) + 1;
  };
  // ── ONE OAK'S SHAPE, ROTATED AND DENSE ── the rotation is stampModel's, copied literally rather than
  // re-derived, because a shape that disagrees with the stamp by one voxel makes phPresent answer 0 for the
  // whole tree and the axe finds nothing at all.
  //
  // ── AND THE GLUE, WHICH IS THE HALF THAT IS NOT OBVIOUS ── a pine is 8,440 voxels in ONE 6-connected piece
  // plus 31 stray specks, so the root flood can be 6-connected and nobody notices. An oak baked from a .glb
  // is not: MEASURED over the seven models, 6-connected they come apart into 48 / 140 / 73 / 454 / 1468 / 526
  // / 1556 pieces, and for the giant that is 233 clusters of 2+ voxels and 1,323 lone specks attached to the
  // main body by nothing but a DIAGONAL. Run the pine's 6-connected flood over that and every one of them is
  // an orphan on the FIRST swing: 232 rigid bodies fighting over 24 slots and 1,323 voxels erased as litter,
  // before the tree has even been notched. 26-connected, every model is exactly ONE piece — the same fact
  // sim/support-rules.js records for its drape cap, off the same asset.
  // The honest answer is a 26-connected flood and it costs 4.3x the neighbour probes (2.25M against 518k on
  // the giant) on EVERY swing. So connectivity is decided once, here, instead: label the 6-components, find
  // the main one, and flag both ends of every 26-link that joins a stray to something else. The flood then
  // walks 6-connected — the pine's own loop, untouched — and expands to 26 only at the flagged cells, which
  // MEASURED are 427 / 1287 / 508 / 3928 / 9769 / 4552 / 12505 of the seven models. On the giant that is 14%
  // of the pops, so 844k probes against 2.25M: same reachability on an intact tree at 37% of the cost.
  // A cut cannot resurrect a glue link the model did not have, and the trunk and the crown are both inside
  // the MAIN component, so no glue link spans the notch: sever the bole and the crown still comes away.
  // ── ONE SHAPE BUILDER, TWO BROADLEAVES ── the oak and the birch differ only in how a voxel is PACKED
  // (the birch carries a 9-bit z and a 3-bit palette INDEX, the oak an 8-bit z and the id itself) and in
  // which table turns that into a world id. Everything downstream - the 6-flood that finds the main body,
  // the 26-neighbour stray pass, the bole footprint - is identical, and it is delicate enough that two
  // copies would drift. So the decode is three arguments and the rest is shared verbatim.
  //   zMask   511 for a birch (models reach 241 courses), 255 for an oak
  //   idOf    the value stored in A. It must never be 0 for a real voxel - phPresent treats 0 as air -
  //           which is why the birch stores its index PLUS ONE and its remap table is shifted to match.
  //   woodOf  is that stored value bark? The bole footprint below is measured from it.
  //   boleZ0  which course the BOLE starts on, when the model knows better than a min-scan can. Left out,
  //           the footprint below is measured at the lowest WOOD voxel in the model — right for an oak,
  //           wrong for a birch whose lowest branch droops 20 courses below its own trunk (model 1 of 16):
  //           the scan then took that drooping tip for the bole, and since stampBirch seats the model on
  //           `- m.tbz` the tip is buried and refused, so the anchor scan found no bark in those columns at
  //           all and every one of those trees came up root -1 (MEASURED: 39 of 40 standing). tbz is the
  //           same number the stamp seats on, so passing it here is the two agreeing rather than a new rule.
  const phShapeBuild = (m, rot, zMask, idOf, woodOf, boleZ0) => {
    const sx = (rot & 1) ? m.sy : m.sx, sz = (rot & 1) ? m.sx : m.sy, h = m.sz;
    const A = new Uint8Array(sx * sz * h);
    const cells = new Int32Array(m.vox.length);        // the SPARSE index list — what makes the flood's seed pass O(voxels) instead of O(box), which at 114x112x116 is a 17x difference
    let n = 0;
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i], x = p & 255, y = (p >> 8) & 255, z = (p >> 16) & zMask;
      let rx, rz;                                      // …stampModel's rotation, verbatim
      if (rot === 0) { rx = x; rz = y; }
      else if (rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      const li = rx + rz * sx + z * sx * sz;
      if (!A[li]) cells[n++] = li;
      A[li] = idOf(p);
    }
    // ── THE LABEL BUFFER IS SCRATCH, NOT AN ALLOCATION (2026-08-19, the oak/cherry felling cost) ── it is
    // transient by its own admission: nothing outside this function ever reads it, and `shp` below does not
    // keep it. It was still a fresh Int32Array of the whole model BOX every time a shape was built, which on
    // the big oak is 101 x 100 x 116 = 1,171,600 entries — 4.7 MB allocated, zeroed and then handed to the GC,
    // per (model, rotation) pair. That is the cost the oak and cherry forests pay and the pine forest does
    // not: a pine has ONE model rotated at load, while an oak builds a shape on demand for each of 8 models
    // x 4 rotations, and the LRU below evicts, so the same shape is paid for again after enough trees.
    // Reused and cleared over the SPARSE cell list instead of refilled: only cells that are part of the tree
    // are ever written, so clearing 67k entries is exact where zeroing 1.17M was the same work as the flood
    // it was preparing for. Grown, never shrunk — the largest oak sizes it once for the session.
    if (!oakLabScr || oakLabScr.length < A.length) oakLabScr = new Int32Array(A.length);
    const lab = oakLabScr;                             // 6-component label per cell, 0 = none. Cleared over `cells` at the end of the stray pass below.
    const st = new Int32Array(n);
    let nc = 0, best = 0, bestN = 0;
    for (let i = 0; i < n; i++) {
      const s0 = cells[i]; if (lab[s0]) continue;
      const id2 = ++nc; let sp = 0, cnt = 0;
      lab[s0] = id2; st[sp++] = s0;
      while (sp > 0) {
        const c = st[--sp]; cnt++;
        const cx = c % sx, cz = ((c / sx) | 0) % sz, cy = (c / (sx * sz)) | 0;
        for (let d = 0; d < 6; d++) {
          const nx = cx + (d === 0 ? 1 : d === 1 ? -1 : 0);
          const ny = cy + (d === 2 ? 1 : d === 3 ? -1 : 0);
          const nz = cz + (d === 4 ? 1 : d === 5 ? -1 : 0);
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= h) continue;
          const nk = nx + nz * sx + ny * sx * sz;
          if (!A[nk] || lab[nk]) continue;
          lab[nk] = id2; st[sp++] = nk;
        }
      }
      if (cnt > bestN) { bestN = cnt; best = id2; }
    }
    let g = null;
    if (bestN < n) {                                   // there ARE strays — and only they and their partners need the 26 treatment, so this scan is over ~1,500 cells, not 86,000
      g = new Uint8Array(A.length);
      for (let i = 0; i < n; i++) {
        const c = cells[i]; if (lab[c] === best) continue;
        const cx = c % sx, cz = ((c / sx) | 0) % sz, cy = (c / (sx * sz)) | 0;
        for (let d = 0; d < 78; d += 3) {
          const nx = cx + phNb26[d], ny = cy + phNb26[d + 1], nz = cz + phNb26[d + 2];
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= h) continue;
          const nk = nx + nz * sx + ny * sx * sz;
          if (!A[nk] || lab[nk] === lab[c]) continue;  // the same 6-piece: the flood already crosses this edge on its own
          g[c] = 1; g[nk] = 1;                         // BOTH ends, because the flood can arrive from either side
        }
      }
    }
    for (let i = 0; i < n; i++) lab[cells[i]] = 0;      // …and hand the scratch back clean. Over the CELLS, not the box: those are the only entries the two passes above can have written

    // ── WHERE THE BOLE STANDS IN THIS SHAPE'S OWN FRAME ── the anchor band is measured at the TRUNK, and the
    // trunk is not at the middle of the model box: MEASURED, the bark centroid of the bottom course is off
    // the bbox centre by (-5.2, +6.0) on the 7 m oak and (+0.8, -2.9) on the 11.7 m one, because the box is
    // sized by the CROWN. Reading the ground at the planting column instead would be a metre out on the
    // models that lean, which on bumpy forest floor is the difference between an anchored tree and one that
    // orphans itself whole the first time it is touched. Rotated with everything else, so it is the bole's
    // real footprint whichever way the tree was placed.
    let wz0 = boleZ0 === undefined ? zMask : boleZ0;
    if (boleZ0 === undefined)
      for (let i = 0; i < m.vox.length; i++) { const p = m.vox[i]; if (!woodOf(p)) continue; const z = (p >> 16) & zMask; if (z < wz0) wz0 = z; }
    let tx0 = 1e9, tx1 = -1e9, tz0 = 1e9, tz1 = -1e9;
    for (let i = 0; i < m.vox.length; i++) {
      const p = m.vox[i], z = (p >> 16) & zMask;
      if (z > wz0 + 2 || !woodOf(p)) continue;   // the lowest three courses of BARK — the bole where it meets the ground
      const x = p & 255, y = (p >> 8) & 255;
      let rx, rz;
      if (rot === 0) { rx = x; rz = y; }
      else if (rot === 1) { rx = m.sy - 1 - y; rz = x; }
      else if (rot === 2) { rx = m.sx - 1 - x; rz = m.sy - 1 - y; }
      else { rx = y; rz = m.sx - 1 - x; }
      if (rx < tx0) tx0 = rx; if (rx > tx1) tx1 = rx;
      if (rz < tz0) tz0 = rz; if (rz > tz1) tz1 = rz;
    }
    if (tx1 < tx0) { tx0 = 0; tx1 = sx - 1; tz0 = 0; tz1 = sz - 1; }   // a model with no bark at all: fall back to the whole box rather than an empty scan
    return { A, sx, sz, h, g, cells: n === cells.length ? cells : cells.subarray(0, n), tx0, tx1, tz0, tz1 };
    PH.stats.oakShapes = (PH.stats.oakShapes | 0) + 1;
  };
  const oakShape = (k, rot) => {
    const key = k * 4 + rot;
    const hit = OAKSHP.get(key);
    if (hit) { OAKSHP.delete(key); OAKSHP.set(key, hit); return hit; }   // LRU touch — re-inserting moves it to the young end
    const shp = phShapeBuild(OAKV[k], rot, 255, (p) => p >>> 24, (p) => woodTab[p >>> 24]);
    OAKSHP.set(key, shp);
    while (OAKSHP.size > OAK_SHP_MAX) OAKSHP.delete(OAKSHP.keys().next().value);   // oldest out
    return shp;
  };
  // ── AND THE BIRCH, on its own cache for the same reason the oak has one ── the stored value is the palette
  // INDEX PLUS ONE (0 is air to phPresent), so BIRCHRM below is BIRCHIDS shifted by one to undo it.
  // BIRCHIDS shifted up by one, because the shape stores index+1 (phPresent reads 0 as air). Built lazily on
  // first use: assets/bow.js fills BIRCHIDS during the async birch load, long after this fragment runs, so a
  // table built at module scope here would capture an empty array and every birch voxel would read as absent.
  let BIRCHRM = null;
  const birchRemap = () => {
    if (BIRCHRM && BIRCHRM.length === BIRCHIDS.length + 1) return BIRCHRM;
    BIRCHRM = new Uint8Array(BIRCHIDS.length + 1);
    for (let i = 0; i < BIRCHIDS.length; i++) BIRCHRM[i + 1] = BIRCHIDS[i];
    return BIRCHRM;
  };
  const BIRCHSHP = new Map();
  const birchShape = (k, rot) => {
    const key = k * 4 + rot;
    const hit = BIRCHSHP.get(key);
    if (hit) { BIRCHSHP.delete(key); BIRCHSHP.set(key, hit); return hit; }
    const shp = phShapeBuild(BIRCHV[k], rot, 511, (p) => (p >>> 25) + 1, (p) => woodTab[BIRCHIDS[p >>> 25]], BIRCHV[k].tbz || 0);
    BIRCHSHP.set(key, shp);
    while (BIRCHSHP.size > OAK_SHP_MAX) BIRCHSHP.delete(BIRCHSHP.keys().next().value);
    return shp;
  };

  // ── HOW HIGH THE ROOT REACHES ── a pine goes into W through stampTree, which OVERWRITES: its sink 5-8
  // courses are real bark buried in the hill, and `my <= sink` is both the flood's anchor and the carve's
  // "never cut the roots out from under it" guard. stampOak stamps in MODE 1, which REFUSES every cell that
  // already holds terrain, so an oak has NO buried courses at all — `my <= sink` would seed a band that is
  // not there, the flood would find no anchor, and the tree would orphan itself whole on the first tap.
  //
  // The oak's anchor is therefore measured rather than assumed: walk up the bole's own footprint until the
  // first course of bark that actually made it into W, and take that plus OAK_ROOT. Measured, because how far
  // the ground rises across a trunk is exactly what the constant cannot know — stampOak seats on groundMin
  // over radius 4, so on bumpy floor the first standing course is several above the nominal seat, and it
  // moves with the terrain the player has dug as well.
  // The band is also the STUMP, since physChopAt refuses to cut it: a felled oak leaves ~30 cm of trunk,
  // which chopSwing's orphaned-wood path then takes down like any other stump.
  // ── AND ONLY BARK ANCHORS (see the seed pass in phFlood) ── the band is 3 courses, and on ground that rises
  // under the trunk it can reach course 8, which is exactly where the lowest leaves of the 11.7 m oak hang.
  // Let one of those be an anchor and the crown holds ITSELF up through its own skirt: notch the bole clean
  // through and the tree still stands. A leaf never held a tree up.
  const OAK_ROOT = 2;
  const oakShapeAt = (wx, wz) => {                     // which oak covers this world column — the exact mirror of the pine loop below
    if (!OAKV.length) return null;
    const r = oakRad();
    const c0x = Math.floor(wx / OKCELL), c0z = Math.floor(wz / OKCELL);
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
      const t = oakAt(c0x + dx, c0z + dz); if (!t) continue;
      const m = OAKV[t.k];
      const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;   // …anchored bottom-centre, stampModel's own arithmetic
      const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
      if (wx < bx || wx >= bx + fw || wz < bz || wz >= bz + fd) continue;
      const R = oakShape(t.k, t.rot);
      const S = { tr: t, R, bx, bz, gy: groundMin(t.wx, t.wz, 4) - t.sink,   // gy = stampOak's own seat, to the voxel
                  rm: OAKID, g: R.g, cells: R.cells, root: 0, oak: 1, hMax: Math.max(MSZ, R.h) };
      let lo = R.h;                                    // the lowest course of the bole still standing in W — bounded by the bole's own footprint, so ~60 probes and the inner loop shrinks as soon as one column answers
      for (let mz = R.tz0; mz <= R.tz1; mz++) for (let mx = R.tx0; mx <= R.tx1; mx++)
        for (let my = 0; my < lo; my++) { const v = phPresent(S, mx, my, mz); if (v && woodTab[v]) { lo = my; break; } }
      S.root = lo >= R.h ? -1 : lo + OAK_ROOT;         // -1 = no bark left at the base at all, i.e. nothing is holding this tree up any more: no seeds, and whatever still stands comes down on the next touch
      return S;
    }
    return null;
  };
  // ── WHICH BIRCH COVERS THIS COLUMN ── the exact mirror of oakShapeAt, and it differs in only two things.
  // The SEAT: stampBirch seats the BOLE on the ground, not the model's box, so gy carries the same
  // `- m.tbz` the stamp applies (world/terrain.js). Get that wrong and every carve is offset vertically by
  // however far up that model's trunk starts - up to 40 voxels.
  // The FLAG: `oak: 1` reads as "broadleaf" downstream, not "is an oak" — it lets an overlapping crown stand
  // in for our own in phPresent, and it gathers the ORPHANS 26-connected so a severed tree comes away as one
  // falling body instead of a shower of diagonal clusters. A birch needs both as much as an oak does: BKCELL
  // is 44 against crowns up to 86 wide, and stampBirch writes in mode 1, so neighbours really do own cells
  // inside our footprint. (It is NOT the flood's connectivity — phFlood is 6-connected for every species and
  // reaches diagonals only through the `g` glue links, so an axe notch severs a birch exactly as it does a
  // pine. Tried the other way on 2026-08-24 and it broke the fall: the crown parcelled into 25 bodies at the
  // cut instead of toppling whole.)
  // ── WHICH BIRCH REALLY OWNS A COLUMN ── birchShapeAt used to return the FIRST tree whose bounding BOX
  // covered the column, and on this grid that is close to a coin toss. BKCELL is 44, the footprints run 29 to
  // 61 wide and the crowns LEAN, so overlapping boxes are not the exception: MEASURED in one stand, 119 of
  // 9,316 tree pairs overlap, and 3.3% of birches resolved their OWN TRUNK column to a neighbour.
  // Everything downstream then works on the wrong tree, and the two halves of that are exactly the two
  // symptoms the user reported: a swing carves THIS trunk and phTreeSettle floods THAT one, so the tree that
  // was cut is never re-tested and stays standing with its base gone, while the neighbour is re-tested for a
  // cut it never took and sheds. "Knock one tree down and it knocks another down" and the floating birches are
  // one bug. MEASURED before this: a trunk severed clean through - 87 of the 139 voxels in its base column
  // gone, 20 courses of nothing left - reported orphans 0, detached 0, and the tree did not move.
  // The BOX is the wrong question, because a birch fills very little of its own. This is the right one: does
  // this tree's geometry actually stand in this column, and is any of it BARK? Bark beats leaf beats nothing,
  // and the bole distance breaks a tie - so the tree whose trunk is really here wins over a neighbour whose
  // canopy merely reaches across, and a column under two crowns goes to the nearer of them.
  // A 2D MASK, and per MODEL rather than per (model, rotation): a rotation is a transpose of the same mask, so
  // the QUERY is rotated into model space instead (the inverse of stampBirch's own four cases). One pass over
  // the model's 8-16k voxels, ~3 KB held for the session. Deliberately NOT the dense rotated shape: that cache
  // is three entries deep (OAK_SHP_MAX) precisely because each one is megabytes, and asking it about every
  // candidate would evict the tree being chopped to answer a question about its neighbour.
  const BKCOL = [];                                    // per-model column mask: bit 0 = the model stands in this column, bit 1 = some of it is bark
  const birchColMask = (k) => {
    let a = BKCOL[k]; if (a) return a;
    const m = BIRCHV[k]; if (!m || !BIRCHIDS.length) return null;   // BIRCHIDS is filled by the async .vox load - never cache a mask built before woodTab could answer
    a = new Uint8Array(m.sx * m.sy);
    for (let i = 0; i < m.vox.length; i++) { const p = m.vox[i];
      a[(p & 255) + ((p >> 8) & 255) * m.sx] |= woodTab[BIRCHIDS[p >>> 25]] ? 3 : 1; }
    BKCOL[k] = a; return a;
  };
  const birchColAt = (t, m, wx, wz) => {               // 0 = this tree is not in this column at all, 1 = leaf only, 2 = bark
    const a = birchColMask(t.k); if (!a) return 0;
    const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
    const rx = wx - (t.wx - (fw >> 1)), rz = wz - (t.wz - (fd >> 1));
    let x, y;                                          // ...stampBirch's rotation, inverted
    if (t.rot === 0) { x = rx; y = rz; }
    else if (t.rot === 1) { x = rz; y = m.sy - 1 - rx; }
    else if (t.rot === 2) { x = m.sx - 1 - rx; y = m.sy - 1 - rz; }
    else { x = m.sx - 1 - rz; y = rx; }
    if (x < 0 || x >= m.sx || y < 0 || y >= m.sy) return 0;
    const v = a[x + y * m.sx];
    return (v & 2) ? 2 : (v & 1) ? 1 : 0;
  };
  // ...and the sweep radius is MEASURED off the models, exactly as oakRad is, instead of the literal 160 that
  // stood here. 160 made it 11 x 11 = 121 cells; the widest birch footprint is 61, so a tree centred more than
  // three cells away cannot reach this column whatever it is, and 7 x 7 = 49 covers it. That matters now that
  // the loop can no longer stop at the first hit: the full sweep is cheaper than the old one was, hit or miss.
  let BK_MAXD = -1;
  const birchRad = () => {
    if (BK_MAXD < 0) { let d = 0; for (const m of BIRCHV) d = Math.max(d, m.sx, m.sy); BK_MAXD = d; }
    return Math.ceil(BK_MAXD / BKCELL) + 1;
  };
  const BIRCH_ROOT = 2;                                // the same allowance the oak gets: a bole may keep two courses of stump
  const birchShapeAt = (wx, wz) => {
    if (!BIRCHV.length) return null;
    const r = birchRad();                              // a crown can overhang several cells
    const c0x = Math.floor(wx / BKCELL), c0z = Math.floor(wz / BKCELL);
    let bt = null, bm = null, bsc = -1, bd = 0;        // the best claim on this column so far - see birchColAt above
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
      const t = birchAt(c0x + dx, c0z + dz); if (!t) continue;
      const m = BIRCHV[t.k]; if (!m) continue;
      const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
      const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
      if (wx < bx || wx >= bx + fw || wz < bz || wz >= bz + fd) continue;
      const sc = birchColAt(t, m, wx, wz);
      const tw0 = birchTrunkW(t, m), ex = wx - tw0.wx, ez = wz - tw0.wz, d = ex * ex + ez * ez;
      if (sc > bsc || (sc === bsc && d < bd)) { bt = t; bm = m; bsc = sc; bd = d; }
    }
    if (!bt) return null;
    // A score of 0 still answers. Every caller is asking "which tree owns this column", and chopSwing
    // deliberately latches a shape while the ray is still in the crown's airspace short of the bole, so
    // refusing here would take the axe's reach away. What has changed is only that a tree with real geometry
    // in this column can no longer lose to one that merely boxes it.
    const t = bt, m = bm;
    const fw = (t.rot & 1) ? m.sy : m.sx, fd = (t.rot & 1) ? m.sx : m.sy;
    const bx = t.wx - (fw >> 1), bz = t.wz - (fd >> 1);
    const R = birchShape(t.k, t.rot);
    const tw = birchTrunkW(t, m);
    const S = { tr: t, R, bx, bz, gy: groundMin(tw.wx, tw.wz, 4) - t.sink - (m.tbz || 0),
                rm: birchRemap(), g: R.g, cells: R.cells, root: 0, oak: 1, hMax: Math.max(MSZ, R.h) };
    let lo = R.h;                                      // the lowest course of bole still standing in W
    for (let mz = R.tz0; mz <= R.tz1; mz++) for (let mx = R.tx0; mx <= R.tx1; mx++)
      for (let my = 0; my < lo; my++) { const v = phPresent(S, mx, my, mz); if (v && woodTab[v]) { lo = my; break; } }
    S.root = lo >= R.h ? -1 : lo + BIRCH_ROOT;         // -1 = no bark left at the base at all
    return S;
  };
  const treeShapeAt = (wx, wz) => {                  // which pine covers this world column, and its exact local frame
    const c0x = Math.floor(wx / TCELL), c0z = Math.floor(wz / TCELL);
    const r = Math.ceil(Math.max(MSX, MSY) / TCELL) + 2;   // a crown can overhang several cells — +2 not +1 since scR can widen a crown past the model and a lean shifts it further
    // ── THE NEAREST TREE, NOT THE FIRST ONE THE SCAN REACHES ── crowns overlap, so several boxes cover the
    // same column and this used to return whichever the loop hit first. Measured: asking at a trunk returned
    // a NEIGHBOUR — root 6 against that tree's own sink of 3 — so a swing carved a tree the player was not
    // pointing at, which is the reported "voxels disappear out of the trunk". Scanning for the nearest trunk
    // axis costs one compare per candidate and no bake, and trees are TCELL apart so the nearest is the one
    // under the crosshair in every case that is not a genuine tie.
    let bestTr = null, bestD = Infinity;
    for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
      const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
      const ex = wx - tr.tx, ez = wz - tr.tz, d2 = ex * ex + ez * ez;
      if (d2 >= bestD) continue;
      bestTr = tr; bestD = d2;
    }
    if (bestTr) {
      // ── THE BAKED FRAME, NOT THE RAW MODEL ── a pine is scaled and leaned when it is stamped, so the
      // model's own dimensions describe a tree that is not there. pineFrame (world/terrain.js) replays the
      // STAMP into a local array, so R.sx/R.sz/R.A and bx/bz/hMax all describe what is actually standing.
      const R = pineFrame(bestTr), bx = R.bx, bz = R.bz;
      if (wx >= bx && wx < bx + R.sx && wz >= bz && wz < bz + R.sz)
        return { tr: bestTr, R, bx, bz, gy: R.gy, rm: remap, g: null, cells: null,
                 root: bestTr.sink, oak: 0, hMax: R.sy };   // …the same key set in the same order as the oak's, so both kinds of shape share one hidden class at every site that reads them
    }
    // ── AND OTHERWISE, AN OAK ── extending this rather than adding a second entry point is deliberate: every
    // caller of treeShapeAt (chopSwing, the arrow's carve in sim/projectiles.js, __vb.physChopFull) is asking
    // "which standing tree owns this column", and none of them wants to ask it twice. The two scans can never
    // both answer, because the biome masks are exclusive — treeAt refuses oakM > 0.5 and oakAt refuses
    // oakM < 0.5 — so in the pine forest this costs one bounded scan over empty cells.
    // …AND THEN THE BIRCH. Three scans, never two answers: treeAt refuses birchM > 0.5 and oakM > 0.5, oakAt
    // wants oakM >= 0.5, and birchAt wants the birch band - the masks are mutually exclusive by construction,
    // so outside a forest this costs one bounded walk over empty cells and inside one it stops at the first.
    return oakShapeAt(wx, wz) || birchShapeAt(wx, wz);
  };
  // 4 emissive embers at an impact point — the death poof's spark half, reused for a landed chop (user).
  // sparks3d is declared further down; this only ever runs from a swing, long after that.
  const spawnChopSparks = (wx, wy, wz) => {
    for (let i = 0; i < 4; i++) {
      const a = Math.random() * 6.283, sp = 14 + Math.random() * 22;
      sparks3d[i] = { x: wx, y: wy, z: wz, vx: Math.cos(a) * sp, vy: 12 + Math.random() * 22, vz: Math.sin(a) * sp,
        born: performance.now(), life: 0.4 + Math.random() * 0.25, ph: Math.random() * 6.283, smoke: false };
    }
    PH.stats.sparks++;
  };
  // ── A SLEEPING BODY MUST NOT OUTLIVE ITS SUPPORT (2026-08-08) ── phStep returns immediately for a sleeping
  // body, and NOTHING in the game re-examined one when the world changed: gpuPatch told the support queue and
  // the navfield and never PH.bodies. So a chunk that fell, landed and dozed off stayed exactly where it was
  // when the player carved away whatever it had been resting on — hanging in the air, permanently.
  //
  // That is the floater every audit in this file is blind to, and the reason the reports never matched the
  // measurements: `floatAudit` reads W, `whyFloating` marches W, and a rigid body is NOT IN W — that is the
  // whole point of the off-grid representation. Aimed at one of these, whyFloating answers "the view ray hit
  // nothing solid within 120 voxels", which is exactly what came back from the player's session.
  //
  // Cheap by construction: one call per gpuPatch BATCH (not per cell) against at most PH.maxBodies bodies,
  // and only when that batch actually cleared something. Waking is free — the solver re-tests contacts on the
  // next step and puts it straight back to sleep if it really was supported.
  // The box arrives in WINDOW coordinates (gpuPatch derives them from the flat index) and the bodies live in
  // WORLD space, so the x/z bounds are un-wrapped through supWorldX/supWorldZ themselves. y needs no conversion.
  // ── IT USED TO INLINE THEM AND GET IT WRONG ── the inlined form was `gx + (winOX - gwrap(winOX, WX))`, which
  // drops the modulo and so only agrees with supWorldX for columns at or right of the wrap point. With WX 768
  // and winOX 800 the wrap point is 32, so window column 10 is world column 1546 and this answered 778: outside
  // the window entirely, matching no body. The player stands at window column (wrap + HALF) mod WX, which is in
  // the broken half whenever the wrap point is past the middle — so digging under your own feet woke nothing,
  // and since this is the game's ONLY world-change wake path, a settled chunk whose support you removed hung
  // there for good. Waking a body that did not need it is free (the solver re-tests contacts and puts it
  // straight back to sleep); failing to wake one is permanent, so the seam case widens rather than guesses.
  const phWakeNear = (gx0, gx1, y0, y1, gz0, gz1) => {
    if (!PH.bodies.length) return;
    let x0 = supWorldX(gx0), x1 = supWorldX(gx1), z0 = supWorldZ(gz0), z1 = supWorldZ(gz1);
    if (x1 < x0) { x0 = winOX; x1 = winOX + WX - 1; }   // the batch straddles the window's wrap seam: in world space that is TWO intervals, so take the whole span rather than the empty one between them
    if (z1 < z0) { z0 = winOZ; z1 = winOZ + WZ - 1; }
    let woke = 0;
    for (let i = 0; i < PH.bodies.length; i++) {
      const b = PH.bodies[i];
      if (!b.sleeping) continue;
      const r = b.rMax + 2;                            // the body's own reach, plus a voxel of contact slop
      if (b.pos[0] + r < x0 || b.pos[0] - r > x1) continue;
      if (b.pos[1] + r < y0 || b.pos[1] - r > y1) continue;
      if (b.pos[2] + r < z0 || b.pos[2] - r > z1) continue;
      b.sleeping = false; b.sleepT = 0; woke++;
    }
    if (woke) PH.stats.woken = (PH.stats.woken | 0) + woke;
  };
  phWakeHook = phWakeNear;                             // gpuPatch sits above this block — see the hook's declaration
  const phWorldIdx = (S, mx, my, mz) => gwrap(S.bx + mx, WX) + (S.gy + my) * WX + gwrap(S.bz + mz, WZ) * WX * WY;
  // Kill every perched songbird in this pine (see the block comment above phFlushBirds' first use).
  // CARD_0..CARD_END is the perched-bird band; kind 5 is the perched-bird kind. Runs ONLY on the swing that
  // actually fells the tree.
  //
  // The bird DIES here rather than being recycled. Clearing init alone sent it back through the normal
  // population loop, which tries findPineCrown and then falls through to the GENERIC open-spot
  // placement when that misses — the "bird teleports to the ground" the user saw. This is the exact
  // triple tryKillCreature uses: the slot is dead for the session, so nothing re-places it anywhere.
  // Birds within `rad` of a single impact — used on EVERY landed chop, since carving foliage can pull
  // a perch out from under a bird without the felling gate ever firing.
  // ── A BIRD NEVER JUST VANISHES (user 2026-08-05: "hitting a bird in the tree, it just disappears instead
  // of going through the death sequence") ── this runs on EVERY landed chop, at the tool's own radius + 8, so
  // a swing that misses the bird and bites the branch beside it used to delete the bird outright: no flash,
  // no poof, nothing. It still dies — a perch cut out from under it is fatal — but it now dies the way
  // everything else does. birdDeath is assigned once the death machinery exists further down; until then the
  // old silent teardown stands in, which only matters during load when nothing is chopping anyway.
  let birdDeath = null;
  const phKillBird = (B) => {
    // ONCE. A ragdolled bird keeps B.init set for the half second its death plays out, and physChopAt runs
    // the near-flush AND (when the crown comes loose) the whole-tree flush in the same swing — so without
    // this the second pass killed the same bird again, which re-ran the teardown on a body it no longer had
    // and tore the death sequence back down to the silent vanish this exists to remove.
    if (B.rag || B.slain) return false;
    if (birdDeath) { birdDeath(B); return true; }
    if (B.sN) unstampWorm(B);                          // needles restored while the crown is still there, so they leave with it
    B.init = false; B.dieT = 0; B.slain = true;
    return true;
  };
  const phFlushBirdsNear = (wx, wy, wz, rad) => {
    let n = 0;
    for (let j = CARD_0; j < CARD_END; j++) {
      const B = wbf[j];
      if (!B || !B.init || (B.kind | 0) !== 5) continue;
      const dx = B.x - wx, dz = B.z - wz, dy = (B.perchFeet || 0) - wy;
      if (dx * dx + dz * dz + dy * dy > rad * rad) continue;
      if (phKillBird(B)) n++;
    }
    PH.stats.birdsKilled += n;
    return n;
  };
  const phFlushBirds = (S) => {
    let n = 0;
    for (let j = CARD_0; j < CARD_END; j++) {
      const B = wbf[j];
      if (!B || !B.init || (B.kind | 0) !== 5) continue;
      const mx = Math.round(B.x) - S.bx, mz = Math.round(B.z) - S.bz;
      if (mx < -2 || mx >= S.R.sx + 2 || mz < -2 || mz >= S.R.sz + 2) continue;   // perched in some other pine
      if (phKillBird(B)) n++;                          // …the same visible death (see phKillBird) — a whole crown coming down should not silently swallow the birds in it either
    }
    PH.stats.birdsKilled += n;
    return n;
  };
  // Returns the WORLD PALETTE ID when this local cell still belongs to the tree, else 0: a pine model voxel
  // not since replaced (by snow, a creature stamp or an earlier chop).
  // ── THE HUNG-PINECONE CLAUSE IS GONE ── it used to claim any PICK_CONE id sitting above sink+12 in a cell
  // where R.A is empty, so that a felled tree carried its cones with it. But PICK_CONE is a SUBSET of the
  // pine's own BARK ids, and trees are placed on a 45-voxel grid with jitter while a footprint is ~35 wide
  // — overlapping footprints are the NORMAL case, and treeShapeAt returns whichever pine it finds first. In
  // the overlap, tree B's bark sits exactly where tree A's R.A is empty, so this clause called it tree A's
  // material, found it unreachable from A's roots, and punched it out of tree B.
  // Hung cones need no special case now: a cone touches needles, the needles touch their branch, the branch
  // is anchored — so the DRAPE flood holds it as ordinary drape, and when the branch goes the cone goes with
  // the same component. That is the whole point of not making the two graphs symmetric.
  // ── S.rm IS THE MODEL-ID -> WORLD-ID TABLE ── `remap` for a pine, the identity for an oak, because
  // stampTree writes remap[v] and stampModel writes the packed id straight through. One property read; the
  // pine's answer is the same byte it always was.
  // ── AND AN OAK ACCEPTS ANY LEAF WHERE ITS OWN LEAF SHOULD BE ── OKCELL is 112 and the widest crown is 114,
  // so oak footprints OVERLAP, and stampModel mode 1 lets the later tree's leaves overwrite the earlier
  // tree's (leaf ids are minted past DECOR_MIN, i.e. soft decor; bark is not and is never overwritten). Held
  // to an exact id match, the earlier oak reads a hole through its own crown wherever a neighbour's canopy
  // crosses it — and a hole in a 6-connected crown is a spurious ORPHAN, i.e. clumps of leaves falling off a
  // tree on the first tap. Whichever oak is felled first takes the shared leaves with it and leaves a small
  // gap in the other's canopy, which is cosmetic and costs nothing: leaves have no hitbox and the drape
  // resolver re-adjudicates whatever was hanging on them. BARK stays an exact match, so a fell can never
  // punch the neighbouring tree's TRUNK out — the failure the pinecone clause above was deleted for.
  const phPresent = (S, mx, my, mz) => {
    const y = S.gy + my; if (y < 1 || y >= WY) return 0;
    const v = S.R.A[mx + mz * S.R.sx + my * S.R.sx * S.R.sz];
    if (!v) return 0;
    const wid = W[phWorldIdx(S, mx, my, mz)];
    if (wid === S.rm[v]) return wid;
    return (S.oak && foliaTab[v] && foliaTab[wid]) ? wid : 0;   // an overlapping oak's canopy standing in for our own
  };
  // ── 6-NEIGHBOUR CONNECTIVITY over ONE tree ── seeds are the buried courses (my <= sink): the root
  // anchor. Anything the flood cannot reach from the root is detached. Flat mark buffer sized to the
  // model box, allocated once and reused, so a chop allocates nothing.
  let phPres = null;                                 // per-cell "is this voxel still in the world", filled by the flood's seed pass — see the note there
  let oakLabScr = null;                              // see the note in the oak shape builder — reused label buffer, never read outside it
  let phMark = null, phStack = null;
  let phInC = null;                                  // per-cell "is this voxel in the body being built" — see phBuildBody. Same all-zero-on-entry discipline as phPres.
  // ── EVERY SHAPE CARRIES ITS OWN CEILING (2026-08-24, "half the trees float and half of them fall") ──
  // MSZ is pine5.vox's height, 116, and it was the my bound in the flood, the component walk, the carve, the
  // drape and the body re-split. A pine is exactly that tall and an oak is not taller, so for eight months it
  // read as "the model box" — but a birch reaches 241 courses, and MEASURED, the fraction of each model above
  // 116 predicts the orphan count on an UNTOUCHED tree to within 1.6 points (tree 2: 28.9% predicted / 29.1%
  // seen; tree 5: 84.5% / 85.1%). Everything above the ceiling was unreachable from the root, so it was never
  // seeded, never bodied and never erased: the trunk came away and the crown stayed in the sky. The two
  // shortest models drop most of their mass below 116 and fell convincingly, which is the "half" the user saw.
  // Math.max keeps the pine and the oak on the exact buffer sizes and bounds they have today — only a model
  // TALLER than a pine changes anything, and the models' own geometry is 100% root-connected (tools/birch_connect.py).
  const phFlood = (S) => {
    const sx = S.R.sx, sz = S.R.sz, hM = S.hMax || MSZ, nAll = sx * sz * hM;
    if (!phMark || phMark.length < nAll) { phMark = new Uint8Array(nAll); phStack = new Int32Array(nAll); }
    else phMark.fill(0, 0, nAll);
    if (!phPres || phPres.length < nAll) phPres = new Uint8Array(nAll);   // …and its partner. Never bulk-cleared: each flood erases exactly what it wrote (see the end of this function), so it is always all-zero on entry
    const li = (mx, my, mz) => mx + mz * sx + my * sx * sz;
    let sp = 0, reached = 0, total = 0;
    const t0 = performance.now();
    const root = S.root, g = S.g;                     // root: the anchored courses (see OAK_ROOT). g: the 26-glue flags, null for a pine.
    // ── THE SEED PASS WALKS THE MODEL, NOT THE BOX, WHEN IT CAN ── a pine's box is 35x36x116 = 146k cells and
    // scanning all of it costs nothing. An oak's is 114x112x116 = 1.48M for 86k voxels, so the same scan is
    // 17x the work for the same answer. S.cells is the shape's own sparse index list; the pine has none and
    // keeps its original triple loop, byte for byte.
    if (S.cells) {                                    // …the oak arm: sparse, and BARK ONLY in the anchor band (see OAK_ROOT)
      const C = S.cells;
      for (let i = 0; i < C.length; i++) {
        const k = C[i];
        const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
        const v = phPresent(S, mx, my, mz);
        phPres[k] = v ? 1 : 0;
        if (!v) continue;
        total++;
        if (my <= root && woodTab[v] && !phMark[k]) { phMark[k] = 1; phStack[sp++] = k; }
      }
    } else {
      for (let my = 0; my < hM; my++) for (let mz = 0; mz < sz; mz++) for (let mx = 0; mx < sx; mx++) {
        const k0 = li(mx, my, mz), v0 = phPresent(S, mx, my, mz);
        phPres[k0] = v0 ? 1 : 0;
        if (v0) { total++;
          if (my <= root && !phMark[k0]) { phMark[k0] = 1; phStack[sp++] = k0; } } }
    }
    while (sp > 0) {
      const k = phStack[--sp]; reached++;
      const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      for (let d = 0; d < 6; d++) {
        const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
        const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
        const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= hM) continue;
        const nk = li(nx, ny, nz);
        // ── THE SEED PASS ALREADY ASKED ── phPresent is not a lookup, it is a WORLD read: an A[] index, a
        // toroidal wrap on x and z, a W[] fetch and a remap compare. The pass above runs it over every cell
        // of the tree and then threw the answer away, so this loop asked again — six times per popped cell,
        // ~500k times on the giant oak, for a value that cannot have changed since (nothing writes W inside
        // a flood). Cached in phPres, it is a byte read.
        if (phMark[nk] || !phPres[nk]) continue;
        phMark[nk] = 1; phStack[sp++] = nk;
      }
      if (g && g[k]) {                                // ── THE DIAGONAL LINKS ── 12,505 flagged cells of 86,365 on the giant, so this runs on 14% of the pops and buys 26-connected reachability at 37% of a 26-connected flood (see oakShape)
        for (let d = 0; d < 78; d += 3) {
          const nx = mx + phNb26[d], ny = my + phNb26[d + 1], nz = mz + phNb26[d + 2];
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= hM) continue;
          const nk = li(nx, ny, nz);
          if (phMark[nk] || !phPres[nk]) continue;   // …the same cached answer the 6-neighbour loop above uses
          phMark[nk] = 1; phStack[sp++] = nk;
        }
      }
    }
    // ── HAND THE PRESENCE SCRATCH BACK CLEAN ── it is never bulk-cleared on entry, so every flood has to
    // erase exactly what it wrote. The oak wrote its sparse cell list; the pine wrote its whole box. Anything
    // left set would be read as "present" by the NEXT tree at the same index, and a stale 1 in a cell the next
    // tree does not own would let its flood walk through empty air.
    if (S.cells) { const C2 = S.cells; for (let i = 0; i < C2.length; i++) phPres[C2[i]] = 0; }
    else phPres.fill(0, 0, nAll);
    PH.stats.floods++; PH.stats.floodVox += total;
    PH.stats.lastFloodMs = +(performance.now() - t0).toFixed(2);
    return { total, reached, orphans: total - reached, sx, sz, li };
  };
  // ── …AND AN OAK'S ORPHANS ARE GATHERED 26-CONNECTED ── the flood decides WHAT came loose and this decides
  // how it is PARCELLED UP, and for an oak the two want different neighbourhoods. 6-connected, a severed oak
  // crown is not one piece: it is the main shell plus the 233 diagonal clusters oakShape glued back on, and
  // phSeparate would ask for a rigid-body slot for each of them. 26-connected it is one body, which is what a
  // falling tree is. Only ever reached when something really did detach, so the 4.3x neighbour cost is paid
  // once per felling rather than once per swing. The pine keeps its own loop verbatim.
  const phComponent = (S, f, start) => {             // one connected component of the ORPHANED cells
    const { sx, sz, li } = f; const hM = S.hMax || MSZ;
    const cells = [];
    const c26 = !!S.oak;
    let sp = 0; phStack[sp++] = start; phMark[start] = 2;
    while (sp > 0) {
      const k = phStack[--sp]; cells.push(k);
      const mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      if (c26) {
        for (let d = 0; d < 78; d += 3) {
          const nx = mx + phNb26[d], ny = my + phNb26[d + 1], nz = mz + phNb26[d + 2];
          if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= hM) continue;
          const nk = li(nx, ny, nz);
          if (phMark[nk] || !phPresent(S, nx, ny, nz)) continue;
          phMark[nk] = 2; phStack[sp++] = nk;
        }
        continue;
      }
      for (let d = 0; d < 6; d++) {
        const nx = mx + (d === 0 ? 1 : d === 1 ? -1 : 0);
        const ny = my + (d === 2 ? 1 : d === 3 ? -1 : 0);
        const nz = mz + (d === 4 ? 1 : d === 5 ? -1 : 0);
        if (nx < 0 || nx >= sx || nz < 0 || nz >= sz || ny < 0 || ny >= hM) continue;
        const nk = li(nx, ny, nz);
        if (phMark[nk] || !phPresent(S, nx, ny, nz)) continue;
        phMark[nk] = 2; phStack[sp++] = nk;
      }
    }
    return cells;
  };
  // ── BODY BUILD ── mass / COM / inertia straight from the voxels, plus Teardown's contact
  // classification: count filled 6-neighbours per voxel — 6 = interior (can never touch anything, never
  // probed), 5 = face, 4 = edge, <=3 = corner. Probes are taken CORNERS FIRST, which is what collapses a
  // flat-bottomed body from hundreds of candidate contacts to a handful.
  // Timed: the shatter calls this once PER CHUNK (~27 for a big oak) inside one frame, so it is the
  // term that decides whether a tree breaking up is a hitch. __vb.phys().stats.build* is the tap;
  // zero the counters, fell a tree, read ms/kvox.
  // ── AN ARROW GOES WITH THE TREE IT IS IN (user 2026-08-28: "when the player shoots an arrow at a tree and
  // the tree falls down, the arrow is still stuck in the air. make the arrow go with the tree") ── a stuck
  // shaft is a `drops` entry pinned to the WORLD point it struck (sim/projectiles.js), which is exactly right
  // for as long as what it struck IS world geometry. Felling ends that: phSeparate lifts the tree out of W into
  // a rigid body and the trunk swings away, and the shaft — still pinned to a point in W — hangs in clear air.
  // This is the same problem the animal path already solved ("make the arrow stick to the life as it moves",
  // user 2026-08-07) and it gets the same shape of answer: remember the impact in the thing's OWN frame and
  // replay it every frame. The one real difference is that a rigid body ROTATES ON ALL THREE AXES where a
  // creature only turns about world up, so the offset AND the attitude ride a full basis rather than one
  // heading — which is the whole reason a toppling trunk needs its own path and could not just set stDth.
  // HOOKED AT BUILD, and that is a deliberate choice twice over:
  //   · phBuildBody is the ONE funnel every body comes through — the fell, the shatter when it lands, the
  //     stump, a support collapse — so no other caller has to be taught that arrows exist.
  //   · a body is born with its voxels exactly where they stood in W, so "is this shaft inside this body" is
  //     only an honest question on this one frame. Asked every frame it would also catch a log ROLLING PAST an
  //     arrow stuck in the ground and snatch it off the floor.
  // A shaft riding a body that then breaks up re-attaches for free: every splitter splices the parent out
  // BEFORE building its children (see the note in sim/chop-tree.js), so the old ride already reads as dead by
  // the time the children ask, and the child holding the shaft claims it.
  // The probe is a small box rather than one cell because the shaft's own arrival CARVED where it landed
  // (arrowChop, ARROW_CHOP_RAD 3), so the voxel under the impact point is routinely the one it knocked out.
  const phRideBody = (b) => {
    if (!b || !b.gpu || !b.cpuGrid || !drops.length) return;
    const g = b.cpuGrid, bw = b.gpu.bw, bh = b.gpu.bh, bd = b.gpu.bd, cL = b.gpu.comL, R = 2;
    for (const dr of drops) {
      if (!dr || !dr.stick || !dr.hitDone || dr.gone) continue;   // only a shaft that has actually landed
      if (dr.stuckSlot !== undefined) continue;        // riding an animal — sim/particles.js's path owns it and must not be overridden
      if (dr.rideB && PH.bodies.indexOf(dr.rideB) >= 0) continue;   // already riding a body that is still alive
      const dx = dr.ex - b.pos[0], dy = dr.ey - b.pos[1], dz = dr.ez - b.pos[2];
      const ox = dx * b.ax[0] + dy * b.ax[1] + dz * b.ax[2];   // the impact point in the body's own frame — ax/ay/az are its local axes in WORLD space, so this is one projection each (the same transform phBodyIdAt uses)
      const oy = dx * b.ay[0] + dy * b.ay[1] + dz * b.ay[2];
      const oz = dx * b.az[0] + dy * b.az[1] + dz * b.az[2];
      const cx = ox + cL[0], cy = oy + cL[1], cz = oz + cL[2];   // …and into its voxel grid
      if (cx < -R || cy < -R || cz < -R || cx > bw + R || cy > bh + R || cz > bd + R) continue;
      let found = false;
      for (let qy = -R; qy <= R && !found; qy++) for (let qz = -R; qz <= R && !found; qz++) for (let qx = -R; qx <= R && !found; qx++) {
        const lx = Math.floor(cx) + qx, ly = Math.floor(cy) + qy, lz = Math.floor(cz) + qz;
        if (lx < 0 || ly < 0 || lz < 0 || lx >= bw || ly >= bh || lz >= bd) continue;
        if (g[lx + ly * bw + lz * bw * bh]) found = true;
      }
      if (!found) continue;
      dr.rideB = b; dr.rOx = ox; dr.rOy = oy; dr.rOz = oz; dr.rideMiss = 0;
      dr.rAx = [b.ax[0], b.ax[1], b.ax[2]];            // the body's attitude AT ATTACH — main/tick-support.js composes it with the current one into the delta the shaft turns by
      dr.rAy = [b.ay[0], b.ay[1], b.ay[2]];
      dr.rAz = [b.az[0], b.az[1], b.az[2]];
    }
  };
  const phBuildBody = (S, cells, f, idMap) => {
    const t0 = performance.now();
    const b = phBuildBody0(S, cells, f, idMap);
    phRideBody(b);                                     // …and any arrow standing in the voxels this body was just made of now rides it (see the note above)
    PH.stats.buildMs = (PH.stats.buildMs || 0) + (performance.now() - t0);
    PH.stats.buildN = (PH.stats.buildN | 0) + 1;
    PH.stats.buildVox = (PH.stats.buildVox | 0) + cells.length;
    return b;
  };
  const phBuildBody0 = (S, cells, f, idMap) => {
    const { sx, sz } = f, N = cells.length, hM = S.hMax || MSZ, nAll = sx * sz * hM;
    const lx = new Int16Array(N), ly = new Int16Array(N), lz = new Int16Array(N), id = new Uint8Array(N);
    let cxs = 0, cys = 0, czs = 0;
    let x0 = 1e9, y0 = 1e9, z0 = 1e9, x1 = -1e9, y1 = -1e9, z1 = -1e9;   // the TIGHT bbox, folded into this pass — consumed by the GPU shape below
    // MEMBERSHIP MARKER instead of `new Set(cells)`. Same index space as phMark/phPres and the same
    // discipline: always all-zero on entry, because the surface pass below erases exactly what this
    // wrote. A Set of a felled oak's 15.6k keys cost ~1 ms to build and turned the 6-neighbour test
    // into ~94k hash probes; a byte array makes both near-free. Worth doing carefully because the
    // shatter runs phBuildBody once PER CHUNK — a big oak breaks into ~27 of them in one frame.
    if (!phInC || phInC.length < nAll) phInC = new Uint8Array(nAll);
    for (let i = 0; i < N; i++) phInC[cells[i]] = 1;
    for (let i = 0; i < N; i++) {
      const k = cells[i], mx = k % sx, mz = ((k / sx) | 0) % sz, my = (k / (sx * sz)) | 0;
      lx[i] = mx; ly[i] = my; lz[i] = mz;
      id[i] = idMap ? idMap.get(k) : W[phWorldIdx(S, mx, my, mz)];   // LIVE voxel (covers pinecones, absent from R.A) unless the caller already carved it out
      cxs += mx + 0.5; cys += my + 0.5; czs += mz + 0.5;
      if (mx < x0) x0 = mx; if (mx > x1) x1 = mx;
      if (my < y0) y0 = my; if (my > y1) y1 = my;
      if (mz < z0) z0 = mz; if (mz > z1) z1 = mz;
    }
    const mass = N, com = [cxs / N, cys / N, czs / N];
    let Ixx = 0, Iyy = 0, Izz = 0;
    const cube = 1 / 6;                              // a solid 1x1x1 voxel about its own centre
    let rMax = 0;                                    // farthest voxel from the COM — sets the tip speed, which sets how finely this body must be stepped
    for (let i = 0; i < N; i++) {                    // inertia and rMax read the same r, so they share one pass
      const rx = lx[i] + 0.5 - com[0], ry = ly[i] + 0.5 - com[1], rz = lz[i] + 0.5 - com[2];
      const xx = rx * rx, yy = ry * ry, zz = rz * rz;
      Ixx += yy + zz + 2 * cube; Iyy += xx + zz + 2 * cube; Izz += xx + yy + 2 * cube;
      const d2 = xx + yy + zz; if (d2 > rMax) rMax = d2;
    }
    rMax = Math.sqrt(rMax);
    const b = { n: N, mass, com, id, lx, ly, lz, rMax, I: [Ixx, Iyy, Izz],
      src: phSrc,                                  // …readable via __vb.bodyMats()
      pos: [S.bx + com[0], S.gy + com[1], S.bz + com[2]],   // world position OF THE COM
      origin: [S.bx, S.gy, S.bz],                    // where the local frame sat in W — physValidate proves no duplicate remains
      vel: [0, 0, 0], omega: [0, 0, 0], q: [0, 0, 0, 1],
      sleeping: false, sleepT: 0, born: performance.now(), sx, sz, hMax: S.hMax || MSZ,   // …and the body inherits it, or phChopBody re-splits a felled birch against the pine's ceiling and shatters everything above it
      c26: S.oak ? 1 : 0,                            // ── THIS BODY'S OWN CONNECTIVITY ── an oak is one piece only 26-connected (see oakShape), so chopping a FELLED oak has to re-split it the same way or a single swing shatters the crown into 200 clumps. 0 for everything else, including the {bx,gy,bz} pseudo-shapes phSubBody and phBodyFromCells pass in; phChopBody carries it across to the pieces it makes.
      ax: [1, 0, 0], ay: [0, 1, 0], az: [0, 0, 1] };   // cached world axes — refreshed whenever the body moves, read by phBodySolid
    // SURFACE RANK. 0 = corner (the best probe) … 5 = face-locked; 6 = interior and skipped. Held in
    // parallel typed arrays rather than `rank.push([filled, i])`: a felled crown is nearly all surface,
    // so the pair form allocated ~10k two-element arrays per body and then handed them to a comparator
    // sort. `filled` is 0..5, so a 6-bucket counting sort is exact, allocation-free, and stable —
    // it emits the identical order the comparator sort did.
    const sIdx = new Int32Array(N), sFil = new Uint8Array(N);
    const fIdx = new Int32Array(N), fFil = new Uint8Array(N);   // foliage, held back in case the body turns out to be nothing else
    let sn = 0, fn = 0;
    const rowZ = sx, rowY = sx * sz;                 // phInC is indexed x + z*sx + y*sx*sz, so a neighbour is one add
    for (let i = 0; i < N; i++) {
      const mx = lx[i], my = ly[i], mz = lz[i], k = mx + mz * rowZ + my * rowY;
      let filled = 0;
      if (mx + 1 < sx && phInC[k + 1]) filled++;
      if (mx - 1 >= 0 && phInC[k - 1]) filled++;
      if (my + 1 < hM && phInC[k + rowY]) filled++;
      if (my - 1 >= 0 && phInC[k - rowY]) filled++;
      if (mz + 1 < sz && phInC[k + rowZ]) filled++;
      if (mz - 1 >= 0 && phInC[k - rowZ]) filled++;
      if (filled >= 6) continue;                     // INTERIOR — skipped entirely
      if (foliaTab[id[i]]) { fFil[fn] = filled; fIdx[fn++] = i; continue; }   // LEAVES HAVE NO HITBOX (user): needles never generate a contact, so a crown clips into the ground instead of standing the trunk up on it
      sFil[sn] = filled; sIdx[sn++] = i;
    }
    for (let i = 0; i < N; i++) phInC[cells[i]] = 0;   // erase exactly what we wrote — phInC must be all-zero for the next body
    const rFil = sn ? sFil : fFil, rIdx = sn ? sIdx : fIdx, rn = sn || fn;   // …the fallback is a body that is ALL leaves (a chunk chopped out of pure canopy), which would otherwise have no contacts at all and fall through the world
    const cnt = new Int32Array(7);                   // corners first
    for (let i = 0; i < rn; i++) cnt[rFil[i]]++;
    for (let d = 0, acc = 0; d < 6; d++) { const c = cnt[d]; cnt[d] = acc; acc += c; }
    const ord = new Int32Array(rn);
    for (let i = 0; i < rn; i++) ord[cnt[rFil[i]]++] = rIdx[i];
    b.surfN = rn;
    // SPATIAL STRATIFICATION. Corner-first alone samples only the extremities — for a felled trunk that
    // is the crown tips, and the flat CUT FACE got no probes at all, so the body sank straight through
    // the stump it should have been resting on. Bucket the surface into 8-voxel cells, take the
    // best-ranked (most corner-like) voxel from each, and only then fill any remaining slots with the
    // next-best overall. Keeps Teardown's corner preference AND guarantees every face is represented.
    const seenB = new Set(), pr = [], inPr = new Set();
    for (let i = 0; i < rn && pr.length < PH.maxProbes; i++) {
      const idx = ord[i];
      const bkey = (lx[idx] >> 3) | ((ly[idx] >> 3) << 8) | ((lz[idx] >> 3) << 16);
      if (seenB.has(bkey)) continue;
      seenB.add(bkey); pr.push(idx); inPr.add(idx);
    }
    for (let i = 0; i < rn && pr.length < PH.maxProbes; i++) {
      const idx = ord[i];
      if (!inPr.has(idx)) { pr.push(idx); inPr.add(idx); }   // was pr.indexOf(idx) — the same answer (the bucket pass only ever pushes distinct indices) at O(1). On a pine's ~2k surface cells the linear scan is invisible; a felled oak has 8,852 bark voxels against a 512-probe budget, which is 4.5M comparisons on the one swing that already costs the most.
    }
    b.probes = new Int32Array(pr);
    // ── GPU SHAPE ── a TIGHT bbox around the component (a felled crown is a fraction of the full model
    // box) uploaded as one dense id grid. The renderer DDAs this; W never sees it. The bbox itself was
    // folded into the decode pass at the top of this function.
    const bw = x1 - x0 + 1, bh = y1 - y0 + 1, bd = z1 - z0 + 1, cells2 = bw * bh * bd;
    phReclaim(cells2);                               // make room by retiring the oldest fallen debris (see phReclaim)
    if (bodyTop + cells2 > BODYCAP) { b.gpu = null; PH.stats.noGpu = (PH.stats.noGpu | 0) + 1; return b; }   // still no room (one body larger than the whole buffer) — simulates but is NOT DRAWN, which on screen is indistinguishable from the chunk vanishing. Counted so that case can be told apart from a body that was genuinely lost.
    const grid = new Uint32Array(cells2);
    for (let i = 0; i < N; i++) grid[(lx[i] - x0) + (ly[i] - y0) * bw + (lz[i] - z0) * bw * bh] = id[i];
    device.queue.writeBuffer(bodyBuf, bodyTop * 4, grid.buffer, 0, cells2 * 4);
    b.cpuGrid = grid;                              // CPU copy so player collision can query the body (see phBodySolid)
    b.gpu = { off: bodyTop, bw, bh, bd, cells: cells2, comL: [com[0] - x0, com[1] - y0, com[2] - z0] };
    bodyTop += cells2;
    phAabb(b);                                     // a body queried before its first physStep still needs a valid broad-phase box
    if (b.ab) { const A = PH.abAll; for (let k = 0; k < 3; k++) { if (b.ab[k] < A[k]) A[k] = b.ab[k]; if (b.ab[k + 3] > A[k + 3]) A[k + 3] = b.ab[k + 3]; } }
    return b;
  };
  // Evict the OLDEST bodies until `need` cells fit, compacting the shape buffer as we go. The user's
  // call: old fallen debris may disappear to make room for new. Compaction re-uploads the survivors from
  // their CPU grids, which is why every drawn body keeps one.
  // Free ONE body slot by dropping the oldest. Chunk spawning used to just `continue` when the slot
  // budget was full, so once a few felled trunks (which never retire) filled the list, the axe stopped
  // producing chunks entirely — the "chunks stop working after a couple of trees" report. Old fallen
  // debris giving way to new is the user's stated preference.
