  // ── DYNAMIC-LIFE SLOT LEDGER ── who occupies each drop slot this frame, where its anchor is (world coords) and which
  // item/anim frame it shows. The frame-end lifeMot fill derives per-slot rigid motion deltas + history-rejection flags
  // from cur-vs-prev; the temporal/TAA passes reproject trace-injected creature pixels with them.
  const lifeUid = new Int32Array(DROP_SLOTS), lifeUidPrev = new Int32Array(DROP_SLOTS);
  const lifeAnc = new Float32Array(DROP_SLOTS * 3), lifeAncPrev = new Float32Array(DROP_SLOTS * 3);   // one xyz world anchor per drop slot
  const lifeItem = new Int32Array(DROP_SLOTS), lifeItemPrev = new Int32Array(DROP_SLOTS);
  const lifeAna = new Uint8Array(DROP_SLOTS);                  // analytic-only bit (fireflies, drops, sparks, empty slots)
  const lifeSlotSet = (slot, uid, x, y, z, item, analytic) => {
    lifeUid[slot] = uid; lifeItem[slot] = item; lifeAna[slot] = analytic ? 1 : 0;
    lifeAnc[slot * 3] = x; lifeAnc[slot * 3 + 1] = y; lifeAnc[slot * 3 + 2] = z; };
  const birdWrite = (slot, ps, cam, right, up, fwd) => {   // pose → one drop slot (camera-space anchor + the three model axes)
    const o2 = dropOff(slot);                          // the slot index is passed in, NOT derived from the offset: the two halves of the array are not contiguous
    lifeSlotSet(slot, 1000 + (ps.uid || 0), ps.x, ps.y, ps.z, Math.round(ps.item), false);   // flying birds are trace-injected life
    const rx = ps.x - cam[0], ry = ps.y - cam[1], rz = ps.z - cam[2];
    UF[o2] = rx * right[0] + ry * right[1] + rz * right[2]; UF[o2 + 1] = rx * up[0] + ry * up[1] + rz * up[2]; UF[o2 + 2] = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]; UF[o2 + 3] = BIRD_VS;
    UF[o2 + 4] = ps.Xw[0] * right[0] + ps.Xw[1] * right[1] + ps.Xw[2] * right[2]; UF[o2 + 5] = ps.Xw[0] * up[0] + ps.Xw[1] * up[1] + ps.Xw[2] * up[2]; UF[o2 + 6] = ps.Xw[0] * fwd[0] + ps.Xw[1] * fwd[1] + ps.Xw[2] * fwd[2]; UF[o2 + 7] = ps.item;
    UF[o2 + 8] = ps.Yw[0] * right[0] + ps.Yw[1] * right[1] + ps.Yw[2] * right[2]; UF[o2 + 9] = ps.Yw[0] * up[0] + ps.Yw[1] * up[1] + ps.Yw[2] * up[2]; UF[o2 + 10] = ps.Yw[0] * fwd[0] + ps.Yw[1] * fwd[1] + ps.Yw[2] * fwd[2]; UF[o2 + 11] = 0;
    UF[o2 + 12] = ps.Zw[0] * right[0] + ps.Zw[1] * right[1] + ps.Zw[2] * right[2]; UF[o2 + 13] = ps.Zw[0] * up[0] + ps.Zw[1] * up[1] + ps.Zw[2] * up[2]; UF[o2 + 14] = ps.Zw[0] * fwd[0] + ps.Zw[1] * fwd[1] + ps.Zw[2] * fwd[2]; UF[o2 + 15] = 0;
  };
  // ── A SHOT SKY BIRD DIES LIKE EVERYTHING ELSE (user 2026-08-09: "it just appears to disappear with the
  // sparks") ── it was the last creature without a ragdoll: it set the red flash and the clock, but nothing
  // ever converted it, so at the end of HURT_MS it stopped being drawn and the poof fired in empty air.
  // Two helpers because there are two flight paths — bird 0 rides a dedicated drop slot, the other eleven ride
  // the compacted band — and copying this into both is how they drift apart.
  //   birdRagTick: true means "this bird IS a rigid body now, do not step or draw it". Same rule the wbf loop
  //     applies with `if (B.rag) continue`, and for the same reason: drawing it again puts a second copy in
  //     the air where the corpse used to be.
  //   birdPose:    caches the pose the renderer just used, so creatureRagdoll's trace-injected branch can
  //     rebuild the bird's voxels in world space at the instant it dies — exactly what the wbf emit caches for
  //     ducks and fish. birdStep returns fresh Xw/Yw/Zw arrays per call, so holding the references is free and
  //     cannot go stale under the next bird.
  // ── SAME DEATH AS EVERY OTHER ANIMAL (user 2026-08-06) ── a sky bird used to blink out the instant the shaft
  // touched it: no red, no beat. Pool creatures set the hurt box and defer the poof by HURT_MS so the flash
  // plays; birds live in birds[], not wbf, so pendDeath (which indexes the pool) cannot carry them — they get
  // the same clock on their own field instead, and the box is re-published each frame so the red follows it.
  // Its own function so __vb.birdKill() can reach it: landing an arrow on a bird 140 voxels up is not a thing
  // a test can do reliably, and this is the only way to exercise the death at all.
  const birdShot = (B) => {
    if (!B || !B.init || B.dying) return false;
    B.dying = true; B.dieAt = performance.now() + HURT_MS; birdKills++;
    // ── GO RIGID (user 2026-08-09: "it just appears to disappear with the sparks") ── the sky bird was the one
    // creature left without a ragdoll. Everything needed was already here: creatureRagdoll's trace-injected
    // branch rebuilds a body from the item model at the pose the renderer last drew, and birdPose now caches
    // that pose per frame the way the wbf emit does for ducks and fish. Same call, same shatter, same
    // red-while-falling. If it cannot convert (physics off, model too big, no body slot) B.rag stays false and
    // the old death plays out untouched.
    if (creatureRagdoll(B)) B.rag = true;
    HURT.slot = -1; HURT.hold = false; HURT.t0 = performance.now();   // slot -1: no cSlot match, so the shader takes the AABB path — which is what a bird needs anyway
    if (B.rag) hurtBox(B);                             // the flash boxes the PIECES from here on, so it falls with them
    else { HURT.cx = B.x; HURT.cy = B.y; HURT.cz = B.z; HURT.hx = 3.2; HURT.hy = 3.2; HURT.hz = 3.2; }
    return true;
  };
  const birdRagTick = (BD) => {
    if (!BD || !BD.init || !BD.rag) return false;
    if (performance.now() >= BD.dieAt) {
      // The poof goes where the BODY is, not where the bird was flying half a second ago, and the pieces go
      // with it — the sparks are the death, so nothing is left behind to collect. reapDeaths does the same.
      spawnDeathBurst(BD.ragBody ? BD.ragBody.pos[0] : BD.x, BD.ragBody ? BD.ragBody.pos[1] : BD.y, BD.ragBody ? BD.ragBody.pos[2] : BD.z);
      for (const pb of (BD.ragParts || [])) { const pi = PH.bodies.indexOf(pb); if (pi >= 0) PH.bodies.splice(pi, 1); }
      BD.ragBody = null; BD.ragParts = null; BD.init = false; BD.dying = false; BD.rag = false;
    } else hurtBox(BD);                                // red on every piece, all the way down
    return true;
  };
  const birdPose = (BD, ps) => {
    if (!BD || !BD.init) return;
    BD.ragIt = ps.item; BD.ragA0 = ps.x; BD.ragA1 = ps.y; BD.ragA2 = ps.z;
    BD.ragX = ps.Xw; BD.ragY = ps.Yw; BD.ragZ = ps.Zw; BD.ragS = BIRD_VS;
    if (BD.dying) {                                    // dying but NOT rigid (physics off, or no body slot free): the old death, unchanged
      if (performance.now() >= BD.dieAt) { spawnDeathBurst(BD.x, BD.y, BD.z); BD.init = false; BD.dying = false; }
      else { HURT.cx = BD.x; HURT.cy = BD.y; HURT.cz = BD.z; }
    }
  };
  const birdHit = (box, ps) => {                       // SOLID hitbox: project the glide-frame extents onto the world axes for a snug, stable AABB
    const bdi = (itemsRef && itemsRef[ps.glideItem]) || { w: 14, d: 6, h: 10 };   // this bird's own glide frame — the two species are the same size, but keep it honest
    const ew2 = bdi.w * 0.5 * BIRD_VS, ed2 = bdi.d * 0.5 * BIRD_VS, eh2 = bdi.h * 0.5 * BIRD_VS;
    box.cx = ps.x; box.cy = ps.y; box.cz = ps.z;
    box.hx = ew2 * Math.abs(ps.Xw[0]) + ed2 * Math.abs(ps.Yw[0]) + eh2 * Math.abs(ps.Zw[0]);
    box.hy = ew2 * Math.abs(ps.Xw[1]) + ed2 * Math.abs(ps.Yw[1]) + eh2 * Math.abs(ps.Zw[1]);
    box.hz = ew2 * Math.abs(ps.Xw[2]) + ed2 * Math.abs(ps.Yw[2]) + eh2 * Math.abs(ps.Zw[2]);
    box.active = true;
  };
  const birdBoxes = Array.from({ length: 12 }, () => ({ active: false, cx: 0, cy: 0, cz: 0, hx: 8, hy: 8, hz: 8 }));   // one per flying songbird
  const birdBox = birdBoxes[0];   // SOLID cardinal hitbox (Task 2): the player can't fly/walk through it. World-axis half-extents, republished each frame from the oriented model
  const bunnyBoxes = Array.from({ length: 4 }, () => ({ active: false, cx: 0, cy: 0, cz: 0, hx: 3.5, hy: 5, hz: 3.5 }));   // SOLID world-bunny hitboxes (user) — the nearest few bunnies to the player, republished each frame; tested by boxFree
  const armBoxes = Array.from({ length: 4 }, () => ({ active: false, cx: 0, cy: 0, cz: 0, hx: 4, hy: 3, hz: 4 }));   // SOLID world-armadillo hitboxes — lower + flatter than the bunny; nearest few, republished each frame
  const skunkBoxes = Array.from({ length: 4 }, () => ({ active: false, cx: 0, cy: 0, cz: 0, hx: 4, hy: 4, hz: 4 }));   // SOLID world-skunk hitboxes — a touch taller than the armadillo (rounder body); nearest few, republished each frame
  const porcBoxes = Array.from({ length: 4 }, () => ({ active: false, cx: 0, cy: 0, cz: 0, hx: 4, hy: 4, hz: 4 }));   // SOLID world-porcupine hitboxes (user's 4th land mammal) — same extents as the skunk; nearest few, republished each frame
  const flamBoxes = Array.from({ length: 4 }, () => ({ active: false, cx: 0, cy: 0, cz: 0, hx: 4, hy: 4, hz: 4 }));   // …and the FLAMINGO's, or it is the one land-band creature the player walks straight through: sim/player.js tests exactly these arrays, so a band with no array is a body with no collision
  const bfly = { init: false, x: 0, z: 0, th: 0, om: 0, omT: 0, tRe: 0 };   // the editor butterfly's wander state — slot 4
  const cardSlainPerch = new Set(), CARD_SLAIN_CAP = 512;   // perches whose bird the player KILLED — see findPineCrown. Module scope on purpose: the kill handler and the placement search are ~4000 lines apart.
  const cardPerchKey = (tx, tz, bi) => tx + '|' + tz + '|' + bi;
  // ── REACH, AND THE ONE NUMBER THAT SIZES EVERY POPULATION BEHIND IT ── (user 2026-08-17: "unify the render
  // distance of all the life") CARD_KEEP is the DESPAWN radius for the perched songbirds, and main/tick-life.js
  // sets MAM_KEEP = CARD_KEEP so it is the four land mammals' reach too — those two were the short tier.
  // Everything else (worms, ducks, fish, butterflies, desert life) already kept at LIFE_KEEP =
  // min(renderDist + 64, LIFE_CAP), which is 1040 at the shipped 1000-voxel view, so this is now one number for
  // all life. It lives HERE rather than beside stampCardinal, where it was declared until today, because it is
  // what SIZES THE POOL below: CARD_N and the land-mammal counts are both derived from it, and
  // sim/life/stamped.js is concatenated UNDER this fragment.
  // 1040 IS THE WORLD'S EDGE, NOT A PREFERENCE: the generated rect reaches min(HALF, renderDist + 96) = 1024
  // here, and a spawn outside the rect is rejected outright because its hmap is stale. Nothing can see further
  // than this without a bigger window (the 3x-view-distance project).
  const CARD_KEEP = 1040;
  const CARD_KEEP_V0 = 680;                            // …and the reach every population below was TUNED at, kept as a NAME so the growth is arithmetic instead of a magic 2.34
  // ── AND A REACH DILUTES UNLESS THE COUNTS FOLLOW ── what the player reads as "how alive is this forest" is
  // population / AREA, so a population bounded by CARD_KEEP has to grow with the SQUARE of it or the same
  // animals are smeared over 2.34x the ground. That exact failure was measured and reverted once before — see
  // the comment on MAM_KEEP, "mammals at 1020 vox, in-view rings near-empty". Every count bounded by CARD_KEEP
  // multiplies by this and nothing else does: the kinds that were already at 1040 are untouched.
  // IT COSTS NOTHING IN DROP SLOTS, and that is a property of the arithmetic rather than luck. A land mammal
  // trace-injects only inside UNI_BIRD_R (= LIFE_DRAW, 420) and grid-stamps beyond it, and that radius did NOT
  // move; the traced head-count is therefore (near-field density) x pi x 420², and holding the density fixed is
  // exactly what holds the drop-slot demand fixed. Measured against the old numbers: 6 mammals over a 639-vox
  // spawn disc put 2.59 of them inside 420; 14 over a 977-vox disc puts 2.58. Same budget, more forest.
  const LIFE_DENS_K = (CARD_KEEP * CARD_KEEP) / (CARD_KEEP_V0 * CARD_KEEP_V0);   // = 2.3391
  // ── THE SLOT LADDER ── every band boundary in the game DERIVES from these widths. They used to be ~50
  // hard-coded integer literals scattered over eleven fragments (16, 20, 32, 64, 244, 276, 300, 324, 348, 372),
  // which meant widening any band silently renumbered every band above it — and a missed literal does not
  // throw, it mis-classifies: a bunny counted as a fish, a porcupine that never bleeds, a debug tap reading the
  // wrong animals. Widen a band by editing its _N here and every loop, spawn gate, census and tap moves with it.
  // MAM_END is kept as its own name because the mammal loops mean "end of the mammal band" while the full-pool
  // loops mean "end of the pool" (DES_END), and those two were the same number until the desert band landed.
  const FLY_N = 16, DUCK_N = 4, BABY_N = 12, WORM_N = 32, FISH_N = 32, MAM_PER = 24, DES_N = 9, DES_PER = 8;
  // ── PERCHED SONGBIRDS: THE ONE WIDTH THAT TRACKS THE REACH ── 180 was the pool at CARD_KEEP_V0 and it was
  // SATURATED there: the oak wood's measured 1.24e-4 birds/vox² offers 180 perches inside a 680 disc and the
  // census reached 179 of them. At 1040 the same wood offers 421, so the pool grows with the area and the
  // forest the player walks through has the density it always had, now all the way out.
  // WHAT THIS DOES AND DOES NOT BUY, because findPineCrown fills NEAREST-FIRST: a standing player was never
  // thinned by the radius change (the nearest 180 perches are the same 180 perches), so this is not a rescue —
  // it is REACH. A walking player was thinned, because a bird left behind holds its slot out to CARD_KEEP now
  // instead of 680, and the pool spent on that tail is pool not spent on the forest ahead.
  const CARD_N = Math.round(180 * LIFE_DENS_K);        // 421 — and it must stay UNDER the perch supply in the disc, or every unfilled slot re-runs buildCardCand at full cost each frame
  const FLY_0 = 0, FLY_END = FLY_0 + FLY_N;            //   0-15    butterflies / fireflies / dragonflies
  const DUCK_0 = FLY_END, DUCK_END = DUCK_0 + DUCK_N;  //  16-19    MOTHER ducks
  const BABY_0 = DUCK_END, BABY_END = BABY_0 + BABY_N; //  20-31    ducklings, 3 per mother
  const WORM_0 = BABY_END, WORM_END = WORM_0 + WORM_N; //  32-63    worms
  const CARD_0 = WORM_END, CARD_END = CARD_0 + CARD_N; //  64-484   perched songbirds — GRID-STAMPED, so they take no drop slot and are free to exceed the drop budget
  const FISH_0 = CARD_END, FISH_END = FISH_0 + FISH_N; // 485-516   fish (kind 6)
  const BUNNY_0 = FISH_END, BUNNY_END = BUNNY_0 + MAM_PER;   // 517-540
  const ARM_0 = BUNNY_END, ARM_END = ARM_0 + MAM_PER;        // 541-564
  const SKUNK_0 = ARM_END, SKUNK_END = SKUNK_0 + MAM_PER;    // 565-588
  const PORC_0 = SKUNK_END;                                  // 589-612
  // ── THE FLAMINGO (user 2026-08-18) ── a FIFTH band on the land-mammal run, and inside it rather than beside
  // it on purpose: it walks, it is killable and it leaves a carcass, which is the whole of what MAM_0..MAM_END
  // means to the code that loops over it (dropsMeat, the seat, the hurt box, the even-spread machinery). Being a
  // bird rather than a mammal changes nothing any of those loops ask. It is the cherry forest's only land life.
  const FLAM_0 = PORC_0 + MAM_PER, FLAM_END = FLAM_0 + MAM_PER;   // 613-636
  const MAM_0 = BUNNY_0, MAM_END = FLAM_END;   // the four land mammals are one contiguous run, MAM_0..MAM_END, and MAM_PER slots each — the per-species count is nBunny/nArmadillo/nSkunk/nPorcupine in tick-life.js and is far under the band width
  // ── THE 'DESERT' BAND (user 2026-08-15) ── DES_N species, DES_PER slots each, appended after the land
  // mammals. Appended rather than carved out of an existing band because every band is fully claimed.
  // WIDENED 7 -> 9 (user 2026-08-17) for the BEE and the GRASS SNAKE, and DES_N is the only number that
  // moved: DES_END, wbf's length, every `< DES_END` loop, every census and every debug tap derive from it,
  // which is the whole point of the ladder. 613-668 became 613-684.
  const DES_END = MAM_END + DES_N * DES_PER;           // 613-684
  // ── …AND THE BAND IS NO LONGER ONE BIOME ── the bee and the grass snake are OAK FOREST creatures. They
  // ride this band because it is the one that already carries scene-graph animation, per-species behaviour
  // tables and a habitat tag; nothing about it is intrinsically sand. Naming them HERE, next to DES_MEAT and
  // keyed the same way, is what keeps the two halves of the decision together: main/tick-life.js gives an
  // oak-only species ZERO of the desert head-count (so the sand's own 4,5,4,5,4,5,4 = 31 is untouched to the
  // last body) and main/tick-creatures.js gives it BIO_OAKF instead of BIO_SAND.
  // A species listed here is oak-only, and ITS VALUE IS ITS HEAD-COUNT — one table rather than a flag beside
  // a count, because the two can never be true of different species. Clamped to DES_PER at the single call
  // site (oakN in tick-creatures), so a value over 8 costs nothing and breaks nothing.
  //   grass_snake 5 = round(nDesert x DES_RARITY), i.e. exactly the per-species density the desert band's
  //     own seven were tuned to, over the same annulus, the same DES_APART floors and the same LIFE_KEEP.
  //   bee 8 = the species' whole band, because a SWARM needs numbers (user 2026-08-17: 'make bees swarm
  //     around it'). BEE_HIVE_N of them go to a hive when one is in reach and the rest forage, so at 5 the
  //     hive would have taken every bee in the world and the flower visiting would never have been seen.
  // The desert MOUSE is deliberately NOT here: it has two homes, which is a different thing and is expressed
  // by DES_OAK in tick-creatures giving it a SHARE of its own slots.
  const DES_OAKONLY = { bee: 8, grass_snake: 5 };
  // What tick-life ASKED each band for on the last frame. A plain mirror, written once per frame, because
  // nCard/nFish/nBunny… are consts inside tickBody and main/debug-api.js is concatenated ABOVE it — a tap
  // reading them directly is a ReferenceError that no static check catches and only the first call reveals.
  // Wanted vs alive is the pair that matters: a pool that never fills means placement is failing, not that
  // the count is wrong, and those two have opposite fixes.
  const LIFE_WANT = { perched: 0, fish: 0, worm: 0, flyer: 0, duck: 0, bunny: 0, armadillo: 0, skunk: 0, porcupine: 0 };
  // ── DESERT SPAWN SPACING ── Poisson-disc floors for the desert band, enforced in tick-creatures' spawn retry.
  // Sized off the annulus the creatures actually land in (~934k vox² at LIFE_KEEP 1040): 42 bodies means a mean
  // gap of ~105 vox, and 6-of-a-species means ~280, so these sit near the half-of-mean that dart-throwing can
  // still satisfy quickly. They are RELAXED per retry at the call site — never let a floor starve a slot.
  // ── AND THE SAME FOR THE FOUR LAND MAMMALS ── the forest band had ONE flat 70-voxel gate and no relaxation,
  // and MEASURED over six boots the whole population was jammed against it: pooled nearest-neighbour min 70.2,
  // p10 79.9, median 104.7 against a mean gap of ~163. A floor that the tenth percentile is sitting on is not a
  // spacing rule, it is the thing deciding every position. MAM_APART is what a try-0 placement must offer;
  // MAM_FLOOR is what the LAST try may accept and is exactly the old flat value, so the guarantee this band has
  // always had cannot get weaker and a cramped spot still fills. Cubic, like the desert's, because a linear
  // decay gives away half the gap by try 5. One floor rather than the desert's two: every mammal's nearest
  // neighbour is a different species already (same-species gaps run 200-280), so a same-species rule would
  // never bind.
  // MAM_RELAX = the try the decay reaches MAM_FLOOR on, out of the 12 the spawn loop runs. The last few
  // tries therefore ask for exactly the gate this band has always had, which is what keeps the count
  // whole: decaying across the full budget left a slot unplaced for a frame in 2 of 12 measured samples.
  const MAM_APART = 110, MAM_FLOOR = 70, MAM_RELAX = 7;
  // ── FLAMINGOS COME IN PAIRS (user 2026-08-18: "can you spawn flamingos as a couple") ── every other creature
  // reserves its home cell outright, one animal per cell, which is exactly what made a lone flamingo. So the
  // flamingo's cell takes TWO, and the second one is placed off the first rather than off the cell: a pair that
  // shares a cell but not a spot would stand 40 voxels apart and read as two lone birds, not a couple.
  const FLAM_PAIR = 2, FLAM_MATE_LO = 5, FLAM_MATE_HI = 9;
  // ── FISH SPACING (user 2026-08-18: "sometimes there will be way too many fish in a very small area") ──
  // the per-pool cap was never the leak; the GEOMETRY under it was. A spot stands for up to 25,400 vox² of water
  // but every fish it spawned landed inside a 24-vox box — 576 vox², so sixteen fish at 1 per 36 vox² when the
  // cap itself is written for 1 per 1200. Same shape as the desert's: a comfortable floor that relaxes cubically
  // rather than a hard one, so a cramped pond still fills instead of burning its retries.
  const FISH_APART = 28, FISH_FLOOR = 14, FISH_RELAX = 11;
  // The betta is the other half. It is EXEMPT from the floor by design (it shoals), joined the first betta within
  // 120 and broke unconditionally — one global anchor per pool, no size cap — so in the blossom band, where every
  // fish is picked betta, the entire population could stack into one ~18-vox disc.
  // JOIN is how far a newcomer will SWIM to a shoal; HUDDLE is how wide a shoal actually is (members sit 3-9 from
  // their anchor) and is what the cap is counted over. Keeping them separate matters: counting mates near the
  // CANDIDATE instead of near the ANCHOR measured shoals of 1-2 — the spawn disc is 45 wide, so a newcomer almost
  // never lands within one shoal-width of the shoal it should be joining, and the betta stopped schooling at all.
  const BETTA_JOIN = 120, BETTA_HUDDLE = 16, BETTA_SCHOOL = 6, BETTA_APART = 70;
  const DES_APART = 160;        // between two of the SAME species — the one the eye reads as "geckos in a litter"
  const DES_APART_ANY = 64;     // between any two desert creatures — stops a mixed pile-up in one patch of sand
  // ── THE BEE'S TWO ERRANDS (user 2026-08-17: bees 'going to flowers and sitting on them briefly', and
  // 'swarm around' a beehive) ── the bee flies the FLY's code path in every other respect; this is the whole
  // of what makes it a bee rather than a fly with stripes. Both errands are expressed as a GOAL BEARING fed
  // to navSteerAir's existing homeTh/leashOut seam — the same one the butterfly leash uses — so the planner
  // scores the errand against navReachAir/navFitsAir, the identical predicates the mover then applies.
  // THAT IS DELIBERATE AND IT IS THE FISH LESSON: sim/life/fish.js once had a planner and a mover that
  // disagreed about what was reachable and the fish swam at terrain forever. Nothing here ever writes B.th
  // or overrides a step. A bee that cannot get to its flower simply does not arrive, and BEE_GIVE_S ends it.
  const BEE_FLOWER_R = 72;      // how far from its HOME a bee will look for a flower. Under FLY_LEASH (84) on purpose: the flower is then always inside the disc the leash already holds the bee in, so the errand goal and the leash goal can never pull opposite ways and there is only ever one bearing to feed the fan.
  const BEE_LOOK_N = 512;       // dart throws per look. fillColumn plants a flower on ihash < 0.005 of eligible columns, i.e. 1 in 200, so 512 darts find one ~92% of the time and STOP AT THE FIRST HIT — the expected cost is ~200 hmap reads, not 512. A miss is free: the bee wanders and looks again at BEE_LOOK_S.
  const BEE_LOOK_S = 1.6;       // …and how long it waits between looks, plus the same again at random
  const BEE_SIT_R = 2.4;        // horizontal distance at which it stops approaching and settles onto the head
  const BEE_SIT_S = 2.0, BEE_SIT_J = 1.6;   // 'BRIEFLY': 2.0-3.6 s on the flower. Long enough to read as a visit at a glance across a clearing, short enough that a bee watched for ten seconds is seen to leave — the same reasoning ANT_WHIM records for its own visible-behaviour cadence. The wings keep flapping at the house 24 fps throughout: a settled bee still buzzes, and freezing the strip would read as a dead bee stuck to a petal.
  const BEE_GIVE_S = 10;        // …and the ARRIVE-BY deadline. A bee that has not reached its flower in this long abandons it and is barred from re-choosing that column for BEE_BAN_S. This is the anti-grind guarantee and it is a TIMER rather than a reachability test on purpose: a timer cannot disagree with the mover, and a second opinion about what is reachable is exactly what went wrong for the fish.
  const BEE_BAN_S = 20;         // how long a given-up flower stays banned for that bee
  const BEE_DOWN = 12;          // vox/s the bee eases down onto (and off) the bloom — a settle, never a snap
  const BEE_HIVE_R = 150;       // how far a bee will notice a hive. Under 1.5 x OKCELL (112) so the 3x3 oak-cell walk beeHiveNear does is guaranteed to cover the whole radius.
  const BEE_HIVE_N = 5;         // bees to a hive, taken from the LOW slots of the bee band so the split is a pure function of the slot number and needs no per-frame arbitration. The other 3 keep foraging, so a hive in view does not empty the meadow.
  const BEE_ORBIT_R = 6.5, BEE_ORBIT_W = 1.9, BEE_ORBIT_Y = 2.2;   // orbit radius, rad/s, and the vertical spread of the swarm. 1.9 rad/s at 6.5 vox is 12.4 vox/s — well under the bee's own 56, so the swarm reads as hovering rather than as a racetrack.
  const BEE_HIVE_S = 14, BEE_HIVE_J = 10;   // 14-24 s at the hive, then back to the flowers. Bounded so a hive cannot capture a bee for the whole session.
  const BEE_HIVE_GAP = 3;       // …and how long before it will go back to a hive again. WAS 8, and with the errand skip below (a bee with its hive in reach does not go to a flower) those 8 seconds were the whole of the time the swarm was not orbiting — MEASURED at 8, only 1-3 of the five were ever within 40 voxels of the hive at once; at 3 it is 4-5. Not zero, because the re-approach is the part that reads as bees coming and going rather than as a fixed ring of decoration.
  // Which palette ids are a flower HEAD. BLOOM is six shades and the bee has to recognise all six; a Set
  // lookup per sampled voxel would be the whole cost of the search, so it is a 256-entry table like every
  // other per-id rule in the game. Built HERE, at module scope: sim/life/slots.js is outside tickBody, and
  // the obvious home (assets/material-tabs.js, where floatTab already marks these ids) is another agent's
  // file today. BLOOM itself is assets/palette.js, fragment 16, far above this one.
  const BLOOM_TAB = new Uint8Array(256); for (const bq of FLOWERHEAD) BLOOM_TAB[bq] = 1;   // FLOWERHEAD, not every flower id: the model has a STEM and a bee belongs on the bloom. BLOOM was heads-only by construction; this restores that property explicitly (assets/bow.js)
  // ══════════════════════════════════════════════════════════════════════════════════════════════════
  // ══ THE BEE'S TWO ERRANDS ══ the flower finder and the HIVE SEAM. The tuning is the BEE_* block above; the
  // five-state machine that drives them is in main/tick-creatures.js, in the kind-0 flyer branch.
  // These live at MODULE scope rather than beside findFlyHome/findWormHome in main/tick-nav.js, where they
  // were written first, because that fragment is inside tickBody: everything declared there is a per-frame
  // local and main/debug-api.js - concatenated ABOVE it - cannot see one at all. __vb.beeDbg/bloomAt/bloomNear
  // would have thrown ReferenceError on their first call, and no static check in this repo catches that; the
  // typeof guard around nDesertOf in desBand() is the same wall, recorded.
  // ══════════════════════════════════════════════════════════════════════════════════════════════════
  // The surface height of a column. Character for character main/tick-nav.js's bfSurf, which is a per-frame
  // local for the reason above - the same split sim/nav.js records for its navBed/navWet mirrors. One line, so
  // a mirror is cheaper than moving bfSurf out from under everything that already closes over it.
  const beeSurf = (x, z) => Math.max(hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX], WL);
  // Is there a flower HEAD standing in this column, and at what y? -1 for no.
  // Flowers are two real voxels - world/terrain.js's fillColumn puts a GRASS stalk in the first air voxel above
  // the ground and a BLOOM head directly on top of it - so this is a VOXEL READ, not a re-derivation of the
  // planting rule. Re-deriving it (the ihash the planter rolls, which would be cheaper per column) is exactly
  // the wrong trade: it is a copy of another fragment's arithmetic that goes silently stale the day the
  // planting changes, and all the bee would do is quietly stop finding flowers with nothing to point at.
  // The window is +/-1 around the surface because BLOOM is in floatTab - surface scatter that never raises the
  // heightmap - plus a voxel of slack for snow cover and for ground the player has edited under it.
  const beeBloomAt = (x, z) => {
    const g = beeSurf(x, z); if (g <= WL + 0.5) return -1;
    const bx = gwrap(Math.floor(x), WX), bz = gwrap(Math.floor(z), WZ) * WX * WY;
    const y0 = Math.max(1, Math.floor(g) - 1), y1 = Math.min(WY - 2, Math.floor(g) + 2);
    for (let y = y0; y <= y1; y++) if (BLOOM_TAB[W[bx + y * WX + bz]]) return y;
    return -1;
  };
  // A flower near a bee's HOME, by dart throw. Deliberately not nearest-first and deliberately unreserved: a
  // bee is not competing for flowers the way a songbird competes for perches (there are ~80 blooms inside
  // BEE_FLOWER_R against 8 bees in the whole world), so the candidate list and occupancy set findPineCrown /
  // findFlyHome need would buy nothing and cost a per-frame rebuild. First hit wins and the search stops there.
  const findBeeFlower = (hx, hz) => {
    for (let t = 0; t < BEE_LOOK_N; t++) {
      const a = Math.random() * 6.283, d = Math.sqrt(Math.random()) * BEE_FLOWER_R;
      const fx = Math.floor(hx + Math.sin(a) * d) + 0.5, fz = Math.floor(hz + Math.cos(a) * d) + 0.5;
      if (fx <= rect.xlo + 4 || fx >= rect.xhi - 4 || fz <= rect.zlo + 4 || fz >= rect.zhi - 4) continue;   // outside the generated rect the hmap is stale window data - the same guard every spawn takes
      const fy = beeBloomAt(fx, fz); if (fy < 0) continue;
      return { x: fx, y: fy, z: fz };
    }
    return null;
  };
  // ═══════════════════════ THE HIVE SEAM - ONE LINE TO WIRE ═══════════════════════
  // The beehive is stamped into oak crowns by world/terrain.js, which this worktree does not own and has never
  // seen. Everything a bee DOES with a hive - approach, orbit, bob, leave, cool down - is finished code in
  // main/tick-creatures.js; the only thing missing is the query that says where one is, and it is this single
  // arrow function. Every other line of the swarm already runs and is exercised by __vb.beeDbg().
  //
  //   TO WIRE: replace the `null` on the next line with terrain.js's hive query, which is expected to be
  //   shaped exactly like oakAt - a pure deterministic function of an OAK CELL (OKCELL = 112, the grid
  //   buildCardCand walks in main/tick-nav.js) returning null or an object carrying the hive's world position:
  //
  //       const BEE_HIVE_Q = (cx, cz) => hiveAt(cx, cz);
  //
  //   beeHiveNear below normalises whatever comes back, so only the FIELD NAMES matter: it reads wx/wy/wz
  //   first and falls back to x/y/z, which between them cover both shapes already in this codebase (oakAt
  //   returns wx/wz, findPineCrown returns x/y/z). A missing height is not an error - the swarm is hung in the
  //   crown band instead. If the real query returns some third shape, this is the one place to map it.
  const BEE_HIVE_Q = (cx, cz) => hiveAt(cx, cz);   // <<<< WIRE HERE - nothing else about the bee needs touching
  // ...and the reach around it, on the oak grid. A 3x3 cell walk covers BEE_HIVE_R by construction (see the
  // constant). Nearest wins, so a bee between two hives commits to one instead of oscillating between them.
  const beeHiveNear = (x, z) => {
    const cx = Math.floor(x / OKCELL), cz = Math.floor(z / OKCELL);
    let best = null, bd = BEE_HIVE_R * BEE_HIVE_R;
    for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
      const h = BEE_HIVE_Q(cx + dx, cz + dz); if (!h) continue;
      const wx = h.wx !== undefined ? h.wx : h.x, wz = h.wz !== undefined ? h.wz : h.z, wy = h.wy !== undefined ? h.wy : h.y;
      if (wx === undefined || wz === undefined) continue;
      if (wx <= rect.xlo + 8 || wx >= rect.xhi - 8 || wz <= rect.zlo + 8 || wz >= rect.zhi - 8) continue;   // past the generated rect the crown is not built and the swarm would orbit nothing
      const ddx = wx - x, ddz = wz - z, d2 = ddx * ddx + ddz * ddz;
      if (d2 >= bd) continue;
      bd = d2; best = { x: wx, y: wy === undefined ? beeSurf(wx, wz) + 18 : wy, z: wz };
    }
    return best;
  };
  // ═══════════════════════ BREAKING ONE OPEN ═══════════════════════
  // (user 2026-08-17: "if the player breaks open the beehive, have bees fly out of it, attacking the player.")
  // Three separate questions, kept apart on purpose because each one has a different failure mode:
  //   WHEN is the hive broken   — hiveChopped, below. A THRESHOLD on what is left of it, not the first tap.
  //   WHO comes out             — main/tick-creatures.js. The hive's OWN bees, and nothing else.
  //   WHEN does it stop         — a rage clock, a give-up distance and a re-arm window; also over there.
  // What lives here is the part the other two have to agree on: a small LEDGER of hives that have just been
  // broken. sim/tools.js writes it at the moment of the swing (that fragment is 48 and this one is 51, so the
  // call resolves at CLICK time out of the one shared scope — never at module-evaluation time, which is the
  // only ordering that would have mattered) and the bee state machine reads it. Nothing in the tool decides
  // what a bee does and nothing in the bee reaches into the tool; the ledger is the whole of the seam.
  //
  // ── WHY A LEDGER AND NOT A DIRECT CALL ── three things fall out of it, and all three are real cases:
  //   * A bee that arrives LATE still finds the wreck. Recruiting at the instant of the swing would find only
  //     whoever was inside the radius on that one frame, and a hive's own foragers are often out.
  //   * An arrow, the editor or a felled branch can break a hive too, and none of them go through chopSwing.
  //     The orbit self-check in tick-creatures posts to this same ledger, so every path ends in one place.
  //   * A test does not have to land a swing: __vb.hiveBreak() posts a record and everything after it is real.
  //
  // Which palette ids ARE a hive. A 256-entry table for exactly the reason BLOOM_TAB above is one: it is asked
  // once per marched voxel inside the chop ray, and a two-element scan there would be the whole cost.
  const HIVE_TAB = new Uint8Array(256); for (const hq of HIVEC) HIVE_TAB[hq] = 1;
  // ── "BREAKS OPEN" IS A THRESHOLD, NOT A TAP ── the user's word is BREAKS, and a hive that has had one corner
  // chipped is not broken open. HIVEV is 54 voxels; an axe swing on it carves a CHOP_RAD * 0.5 = radius-2.5
  // sphere and lifts round(PH.chopBite * 0.5) = 15 of them (half, because a hive is deliberately not woodTab,
  // so it is never the axe's OWN material — see the note in assets/material-tabs.js). 54 -> 39 -> 24, so half
  // the hive is the SECOND swing: one hit chips it, the next one opens it. The stone knife works at a quarter
  // scale (radius 1.25, a 4-voxel bite) and needs seven, which is the right shape too — a worse tool is a
  // longer, louder mistake rather than a different rule.
  // A FRACTION rather than a voxel count, because the model is art: re-authoring beehive.vox to 80 voxels must
  // not silently move the threshold to "one swing".
  const BEE_BREAK_F = 0.5;
  // ── AND THE LEDGER ── at most this many wrecks are live at once. Four, because it is a ring for OVERLAPPING
  // breaks (two hives inside one bee's radius) and not a history: a fifth simultaneous break means the player
  // smashed five hives inside BEE_RAGE_WIN seconds, and the oldest of those has already recruited everything
  // that could hear it.
  const HIVE_BREAK = [], HIVE_BREAK_MAX = 4;
  // …and a hive is only ever broken ONCE. Keyed on the oak's trunk column, which is what identifies a hive
  // (hiveAt is a projection of oakAt on the OKCELL grid, so one cell is one tree is one hive). Capped and
  // cleared wholesale exactly like cardSlainPerch above — a set that grows unbounded across a long session is
  // the leak, and re-angering a hive the player broke an hour ago is not a behaviour anyone would notice.
  const HIVE_DONE = new Set(), HIVE_DONE_CAP = 512;
  // ── THE SWARM'S REACH, ITS TEMPER AND ITS PATIENCE ──
  const BEE_RAGE_R = BEE_HIVE_R;   // how far a break is heard. The SAME radius a bee notices a hive from, deliberately: the bees that can hear one break are exactly the bees that would have been living at it, so this is one number rather than two that can drift apart.
  // ── …AND A HIVE COMING APART IS HEARD TWICE AS FAR (user 2026-08-19: "if the player breaks the beehive,
  // have all the bees nearby attack him") ── the swat radius is BEE_RAGE_R above, and it is right for a swat:
  // one bee is hit, and the bees that could have seen it are the ones standing at the same flower patch. A hive
  // is a different event and the user's word for who answers is ALL, so the reach is the second number and the
  // eligibility rule is the first — both changed on the same day, and neither is a re-tune of the other.
  // 300 is exactly 2 x BEE_RAGE_R, and three independent readings land on it, which is why it is not a taste:
  //   * it covers the FORAGING population as it actually sits. The three non-swarm bees spawn on their own ring
  //     (0.10-0.26 x LIFE_KEEP = 104-270 at the shipped view) and drift FLY_LEASH inside it — MEASURED at a hive
  //     the player was standing on, the foragers sat 127, 190, 247, 323 and 347 voxels out across four samples.
  //     At 150 a break reached at most one of them and usually none; at 300 it reaches the ring.
  //   * it is under HALF the spacing between hives (~600), so a break at one hive can never call the swarm that
  //     lives at the next one. That is the bound that keeps this 'the bees nearby' and not 'every bee alive'.
  //   * BEE_RAGE_LEASH is measured from the WRECK to the PLAYER, not to the bee, so recruiting from 300 cannot
  //     be self-cancelling: a bee that starts 300 out is not over its own leash, it simply has 5.4 s of flying
  //     to do at its own 56 vox/s, inside the BEE_RAGE_S 18 it is angry for.
  const BEE_BREAK_R = BEE_RAGE_R * 2;
  const BEE_RAGE_WIN = 12;         // …and how long the wreck keeps calling. SHORTER than BEE_RAGE_S below, which is what stops a bee whose rage has just ended from re-entering off the very same record, forever.
  const BEE_RAGE_S = 18;           // seconds one bee stays angry. Long enough to be a chase across a clearing, and bounded for the reason BEE_HIVE_S is bounded: nothing may capture a bee for the rest of the session.
  const BEE_RAGE_LEASH = 220;      // ── THE GIVE-UP DISTANCE ── measured from the HIVE and not from the bee, because a swarm defends a PLACE. 22 m of running ends it, and the escape is a real choice: WALK is 46 vox/s against the bee's own 56, so walking away cannot work and SPRINTING (46 x 1.85 = 85) always can.
  const BEE_RAGE_GAP = BEE_RAGE_WIN;   // …and a bee that has just calmed down may not be recruited again until every record that could have called it is stale
  // ── …AND BEE_RAGE_Y WAS NOT THE BUG, WHICH HAD TO BE CHECKED FIRST ── the sting test is a horizontal
  // reach plus a BOX over the whole body, `B.y > P.y - 3 && B.y < P.y + HEIGHT + 3`, i.e. P.y-3 .. P.y+23.
  // Twelve sat squarely inside it with 11 voxels of margin either side, and the measurement agreed: over a
  // 17 s rage with 8 bees angry, `dy` read exactly 12 on every bee on every sample. The vertical servo was
  // already doing its job perfectly and every single miss was horizontal.
  // ── IT IS 16 ANYWAY, AND ONLY BECAUSE THE CLOSER CHANGED WHAT THE NUMBER MEANS ── 12 was authored as
  // "chest height, so it reads as a bee in your face rather than one circling overhead", and at the 20-90
  // voxels the bee actually used to sit at, 12 above the feet IS roughly the eyeline. Now that the closer
  // parks it on BEE_ATK_F × the sting reach — four voxels out — the same 12 is 6.5 voxels BELOW EYE (which
  // is 18.5), and the screenshot proves it: the camera had to pitch 77° down to see the swarm at all. A bee
  // mobbing you belongs in the forward view, not under your feet. 16 puts it 2.5 under the eyeline, ~32°
  // down at the ring — the lower half of the screen, at swatting distance. It is FREE: 16 ± BEE_ATK_YS is
  // 13.4-18.6 against a box that runs to +23, so the sting geometry keeps 4.4 voxels of headroom and the
  // spread is still contained by construction. Nothing about the sting test moved.
  const BEE_RAGE_Y = 16;           // how high up the player an enraged bee flies — see the two paragraphs above for why this is 16 and not the 12 it was authored at, and why the change cannot cost a sting.
  // ── THE CLOSE (user 2026-08-19: "when the bees are supposed to attack, they dont target the player, they
  // cause damage but only if the player just happens to be in the way") ── the TRIGGER was never the bug.
  // MEASURED on the same 17 s rage: pd ran 9 → 106 → 12 → 95 → 9 on one bee and 16 → 191 on another, five of
  // the eight never came inside the 6.5-voxel sting reach at all, and the swarm landed 4 stings in 17 s —
  // every one of them while CROSSING, which is the user's "only if the player just happens to be in the
  // way", stated as a number. That is a SAIL-PAST, and it has three causes, all in the pursuit:
  //   1. THE GOAL IS ONE SCORE TERM AMONG SIX, AND IT LOSES. sim/nav.js navSteerAir weighs the player
  //      bearing at NAV_W_HOME 0.95 against KEEP 0.55 + WANDER 0.70 = 1.25 that both favour whatever heading
  //      the bee already has. Do the arithmetic for a bee that has just overshot, with the player behind it:
  //      carrying straight on scores +0.55 +0.70 −0.95 = +0.30, turning round scores −0.55 −0.70 +0.95
  //      −0.30 = −0.60. The fan is STRUCTURALLY unable to turn a bee around — it re-acquires only when the
  //      reach and openness terms happen to disagree, which IS the 40-voxel, ~10 s orbit that was measured.
  //   2. NO ARRIVAL. A flyer's speed is a constant 56 vox/s and its yaw is clamped at 6.5 rad/s, so its
  //      minimum turn radius is 56/6.5 = 8.6 voxels before the eased integrator (really ~11). The sting
  //      reach is 5.0 + MAMFIT.bee.hd = 6.5. A bee at cruise cannot fly a circle small enough to STAY inside
  //      its own sting radius however perfect its bearing is; it has to slow down to close.
  //   3. THE FAN IS COARSE AND SLOW. Sixteen compass headings is 22.5° of bearing quantisation, decided at
  //      NAV_HZ = 12 Hz, which is 4.6 voxels of travel between decisions. Against a 6.5-voxel target that is
  //      a miss by construction.
  // So the rage gets its own CLOSER (main/tick-creatures.js, in the flyer's arbiter branch) and keeps the
  // fan as its ROUTER. Nothing about it writes B.th or moves the bee: it writes the same B.omT the fan
  // writes, through the same eased integrator and the same ±6.5 clamp, and a speed the same navBrakeAir
  // caps — so navReachAir and navFitsAir still have the last word and the fish's planner/mover split stays
  // unexpressible. It only takes the heading when the direct line is CLEAR to the goal; otherwise the fan
  // routes and "a bee that cannot get to you simply does not arrive" is unchanged.
  const BEE_ATK_F = 0.62;          // the ATTACK RING, as a fraction of the sting reach — derived from it rather than authored beside it, so the two can never drift apart. At MAMFIT.bee.hd 1.5 that is 6.5 × 0.62 = 4.0 voxels: comfortably inside the reach with room for the servo's own overshoot, and outside the player's own HW 2.6, so the bee is in your face rather than in your eye.
  const BEE_ATK_W = 1.15;          // rad/s the ring turns. Each bee chases ITS OWN point on that ring, so this is the swarm's apparent circling rate: 1.15 × 4.0 = 4.6 vox/s, well under the closing floor below, so the point is always catchable and the bee is never left chasing a target that outruns it.
  const BEE_ATK_YS = 2.6;          // …and the vertical spread about BEE_RAGE_Y, per bee, off the same B.beePh the hive orbit already spreads itself with. P.y + 12 ± 2.6 is 9.4-14.6, inside the sting box's own −3..+23 by construction, so the spread can never cost a sting. Its whole job is that eight bees converging do not end up in one plane.
  const BEE_CLOSE_K = 2.2;         // ── THE ARRIVAL ── vox/s of approach speed per voxel still to go. 25 out is 55 (full chase), 10 out is 22, at the ring it is the floor below. The turn radius scales with it: 10/6.5 = 1.5 voxels at the ring, so the bee can hold station ON you instead of flying a 17-voxel circle around you.
  const BEE_CLOSE_MIN = 10;        // …and the floor, so an arrived bee still has way on and still reads as a bee rather than a hovering decal. It is a FLOOR and never a ceiling: the top of the ramp is the flyer's own 56 and nothing here raises it, which is exactly what keeps SPRINTING (85 vox/s) an escape and BEE_RAGE_LEASH reachable.
  const BEE_ATK_TURN = 7.0;        // rage yaw gain (the fan's own is 4.2) — the closer has to win the last few voxels. Still a RATE fed to the same clamp and the same integrator, never a write to B.th.
  const BEE_ATK_TRAP = 0.25;       // …and it stands down while the mover is refusing steps. A closer that kept aiming at the player through a trunk would fight the escape probe, bank `trap` and mercy-recycle the bee; a quarter second of blocked steps hands the heading back to the fan, which is the half of this that knows how to go round.
  const BEE_CLOSER = !location.search.includes('nobeeclose');   // ?nobeeclose — rage ON, closer OFF: the bee falls back to the fan's goal bearing alone. Same shape and the same reason as sim/nav.js's own ?nobrake and ?noarb: this adds work to a per-frame path, so the A/B that isolates ITS cost has to exist in the shipped build rather than be inferred from two builds. Read once at module scope, never per frame.
  // ── THE STING, ON A FIVE-POINT BAR ── vitHurt converts at the door with max(1, ceil(amount / 4)), so on a
  // 5-point bar there is NO hit smaller than one fifth: 1, 2, 3 and 4 all cost exactly the same single point.
  // A bee therefore quotes the floor of the scale, 1, and the only lever left for "small" is the RATE — which
  // is why the cooldown below is SWARM-WIDE rather than per bee. Per bee it would be five independent clocks,
  // and at the cobra's own 1 s that is 5 points a second, i.e. the whole bar inside one: that is not a bee, it
  // is a landmine. One clock for the swarm makes it read the way the lava, quicksand and cactus timers in
  // main/tick-body.js already do — standing in a hazard costs you a point every so often — and the arithmetic
  // is then legible at a glance: 2.5 s a sting, so the full bar is 12.5 seconds of choosing to stand in it.
  const BEE_STING = 1, BEE_STING_CD = 2.5;
  let beeStingT = 0;               // …the one clock. Module scope, because main/tick-creatures.js runs inside tickBody where a `let` is a per-frame local and would reset the cooldown on every frame.
  // The nearest hive RECORD (not the normalised {x,y,z} beeHiveNear hands the swarm) within r. The record is
  // what a count and a break need — the box anchor, its size and its rotation — and it is what __vb.hiveDbg reads.
  const hiveNearest = (x, z, r) => {
    const cR = Math.ceil(r / OKCELL) + 1, cx = Math.floor(x / OKCELL), cz = Math.floor(z / OKCELL);
    let best = null, bd = r * r;
    for (let dz = -cR; dz <= cR; dz++) for (let dx = -cR; dx <= cR; dx++) {
      const h = BEE_HIVE_Q(cx + dx, cz + dz); if (!h) continue;
      const ex = h.wx - x, ez = h.wz - z, e2 = ex * ex + ez * ez;
      if (e2 >= bd) continue;
      bd = e2; best = h;
    }
    return best;
  };
  // ── AND WHICH HIVE THE SWARM BELONGS TO (user 2026-08-17: "angry bees dont come out of the beehives …
  // make the bees swarm around the beehive, still dont see them doing this") ── MEASURED before this, with
  // the player standing 8 voxels from a hive: all eight bees alive, every one of them 450-800 voxels away,
  // and the nearest hive to ANY of them 272. Breaking that hive recruited nobody — the ledger record came
  // back `called: 0`. So neither report is about the swarm behaviour, the ledger, the leash or the sting,
  // every one of which is exercised and correct; there was simply never a bee within BEE_RAGE_R of the
  // wreck, and orbiting was something that happened around hives nobody was standing at.
  // The cause was in the SPAWN, and it was two things at once:
  //   * it looked for a hive only inside the bee's own spawn RING (0.26 x LIFE_KEEP = 270 at the shipped
  //     view). Hives sit ~600 apart, so the hive the player is at is usually not in that disc at all —
  //     measured at one boot: four hives within 600 of the player, the nearest at 589, i.e. none.
  //   * and when one WAS in range it picked uniformly among every hive in the disc, so the swarm scattered
  //     one bee to a hive instead of five to the one the player can see.
  // Both are answered by asking a different question: not "is there a hive somewhere near this bee's spawn
  // ring" but "which hive is the PLAYER'S", answered once for the whole band and answered by DISTANCE. It
  // is POLLED rather than derived per body because hiveNearest over a ring-sized square is ~27x27 oakAt
  // calls — nothing every couple of seconds, real per bee per frame.
  const BEE_HOME_POLL = 2;     // seconds between polls. The answer only moves as the player walks, and a swarm that takes two seconds to notice it has arrived at a new hive is not something anyone can see.
  const BEE_STRAY_R = 200;     // …and how far a swarm bee may be from that hive before it is recycled back to it. Over FLY_LEASH (84) + BEE_FLOWER_R (72) with room to spare, so a bee merely out on an errand is never recycled mid-flight — only one whose hive is no longer the player's, which is what makes the swarm FOLLOW you from hive to hive instead of staying at the one it was born under.
  let beeHomeT = -1, beeHomeR = -1, beeHomeH = null;
  const beeHomeHive = (px, pz, tb, r) => {            // the nearest hive to the PLAYER, cached. `r` comes from the caller (LIFE_OUT) rather than being a constant here: a bee placed past LIFE_KEEP is recycled as "far" on its very next frame, which at a small view distance would be an every-frame churn loop.
    if (tb >= beeHomeT || r !== beeHomeR) { beeHomeT = tb + BEE_HOME_POLL; beeHomeR = r; beeHomeH = hiveNearest(px, pz, r); }
    return beeHomeH;
  };
  // The world box a hive occupies. stampModel anchors bottom-CENTRE and rotates about vertical, so the
  // footprint is (sy, sx) on an odd rotation and (sx, sy) on an even one — read off stampModel in
  // world/terrain.js rather than assumed, even though the shipped model is 5x5x5 and the two agree.
  const hiveBox = (h) => { const fw = (h.rot & 1) ? h.sy : h.sx, fd = (h.rot & 1) ? h.sx : h.sy;
    return { x0: h.bx - (fw >> 1), z0: h.bz - (fd >> 1), fw, fd, y0: h.by, fh: h.sz }; };
  // Which hive is this voxel part of? The 3x3 oak-cell walk is exhaustive, and that is arithmetic rather than a
  // hope: an oak candidate sits somewhere inside its own OKCELL (79) and its hive hangs at a crown anchor, so
  // it is within half the widest crown (118 across, i.e. 59) of the trunk. A hive voxel is therefore never more
  // than 79 + 59 = 138 voxels from its own cell's origin — under two cells — so the owning cell is always one
  // of the nine around the cell the voxel itself falls in.
  // ── …AND IT IS ASKED PER MARCHED VOXEL NOW, SO THE OAK SCAN IS MEMOISED (2026-08-19) ── sim/tools.js asks
  // this of every AIR cell a swing ray walks (see the hollowed-hive continuation there), and the bare form is
  // nine oakAt calls each time. The nine ANSWERS depend only on (cx, cz), and a hive is pure worldgen — a hash
  // on the oak grid, no state, and nothing a chop or an edit can move — so one cell's answer is valid for the
  // whole session. CHOP_REACH is a fraction of OKCELL (79), so a swing ray sits inside one cell for its entire
  // length and a whole march costs ONE scan. A single entry rather than a Map: the ray is a straight line and
  // there is one player, so a second cell only appears when the ray crosses a boundary, and re-scanning on
  // that crossing is cheaper than a map lookup on every voxel that does not.
  let hiveCellX = 0x7fffffff, hiveCellZ = 0x7fffffff, hiveCellA = null;
  const hiveCell = (cx, cz) => {
    if (cx !== hiveCellX || cz !== hiveCellZ) { hiveCellX = cx; hiveCellZ = cz; hiveCellA = null;
      for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
        const h = BEE_HIVE_Q(cx + dx, cz + dz); if (!h) continue;
        (hiveCellA || (hiveCellA = [])).push(h); } }
    return hiveCellA;
  };
  const hiveBoxAt = (x, y, z) => {
    const a = hiveCell(Math.floor(x / OKCELL), Math.floor(z / OKCELL)); if (!a) return null;
    for (let i = 0; i < a.length; i++) { const h = a[i], b = hiveBox(h);
      if (x < b.x0 || x >= b.x0 + b.fw || z < b.z0 || z >= b.z0 + b.fd || y < b.y0 || y >= b.y0 + b.fh) continue;
      return h;
    }
    return null;
  };
  // How much of it is still standing. A VOXEL COUNT over the box rather than a tally of what the tool took, for
  // the same reason beeBloomAt reads the world instead of re-deriving the planting roll: a hive can also be
  // shot, edited, or dropped whole when the branch it hangs from is felled, and a counter kept beside the axe
  // would know about none of that. 125 reads, paid only on a swing that actually bit a hive.
  const hiveLeft = (h) => {
    const b = hiveBox(h); let n = 0;
    for (let z = b.z0; z < b.z0 + b.fd; z++) { const zi = gwrap(z, WZ) * WX * WY;
      for (let x = b.x0; x < b.x0 + b.fw; x++) { const xi = gwrap(x, WX);
        for (let y = b.y0; y < b.y0 + b.fh; y++) { if (y < 1 || y >= WY) continue;
          if (HIVE_TAB[W[xi + y * WX + zi]]) n++; } } }
    return n;
  };
  const hiveFull = () => (HIVEV && HIVEV.vox) ? HIVEV.vox.length : 54;   // the model's own voxel count, read at CALL time — assets/bow.js assigns HIVEV during boot
  // Post a break. Returns 1 the first time a given hive is broken and 0 ever after, so a caller can read it as
  // "did this actually open the hive".
  const hiveBroke = (h) => {
    const k = h.tx + '|' + h.tz;
    if (HIVE_DONE.has(k)) return 0;
    if (HIVE_DONE.size >= HIVE_DONE_CAP) HIVE_DONE.clear();
    HIVE_DONE.add(k);
    HIVE_BREAK.push({ x: h.wx, y: h.wy, z: h.wz, t: performance.now() / 1000, n: 0, key: k, r: BEE_BREAK_R });   // no `swat` flag: a later swat must post its OWN record rather than merging into this one and dragging the wreck (and with it the leash anchor) off the hive
    while (HIVE_BREAK.length > HIVE_BREAK_MAX) HIVE_BREAK.shift();
    return 1;
  };
  // ── THE HOOK sim/tools.js CALLS ── one line at the decor branch of chopSwing, AFTER the bite has landed.
  // Deliberately asked after the carve: the question is "is the hive open now", and the swing that opened it is
  // the one that should let the bees out.
  const hiveChopped = (x, y, z) => {
    const h = hiveBoxAt(x, y, z); if (!h) return 0;
    if (hiveLeft(h) > hiveFull() * BEE_BREAK_F) return 0;
    return hiveBroke(h);
  };
  // ── …AND SWATTING ONE IS THE OTHER WAY TO START A FIGHT (user 2026-08-17: "if the player decides to hit
  // a bee, any surrounding bees will attack the player") ── the same LEDGER, deliberately, rather than a
  // second mechanism beside it: everything downstream of a record — the recruiting, the rage clock, the
  // leash measured from the place, the swarm-wide sting cooldown, the re-arm window, __vb.beeDbg's readout
  // — is already written and already exercised, and the only thing that differs between a smashed hive and
  // a swatted bee is WHERE the fight is and HOW FAR it is heard.
  // ── …AND THE SLOT SPLIT IS NO LONGER THE ELIGIBILITY RULE (user 2026-08-19: "if the player breaks the
  // beehive, have all the bees nearby attack him") ── a record used to carry `all`, and a hive break left it
  // unset so that only the five beeSwarm slots answered while the three foragers kept the meadow. The user has
  // now asked for the other rule on the hive too, so the flag is gone rather than being set on both kinds of
  // record: with nothing left that it distinguishes, keeping a field every writer sets to 1 is a rule nobody
  // is applying. EVERY live bee inside a record's own reach answers it, and the two events differ only in that
  // reach — BEE_RAGE_R for a swat, BEE_BREAK_R for a hive (see the justification at those constants).
  // The meadow is still guaranteed, by the two bounds that were always the real ones rather than by the slot
  // split: BEE_RAGE_S ends every bee's rage in 18 s, and BEE_BREAK_R is under half the hive spacing so a break
  // reaches this hive's neighbourhood and not the next hive's.
  //   * `r`    — how far this wreck is heard. Read by hiveRageAt; a missing one falls back to BEE_RAGE_R.
  //   * `swat` — set on a SWAT only, and it is a merge key, not an eligibility flag: two blows on one bee are
  //     one fight (BEE_SWAT_MERGE), but a swat landed next to a hive the player just smashed must NOT be
  //     allowed to fold into that hive's record, because merging moves the wreck — and the leash is anchored
  //     to the wreck, so a smashed hive would quietly re-anchor itself to wherever the last bee was hit.
  // sim/life/reactions.js calls this from hitCreature, above the wound/kill split, so it fires on EVERY blow
  // — wounding or fatal, axe or arrow or bare hand — exactly where the sparks, the sound and the bolt fire.
  const BEE_SWAT_MERGE = 40;   // …and two swings on the same bee are ONE fight. Without this a held left-click posts a record per blow and a four-deep ring of hive breaks is flushed by swatting one insect; refreshing the existing record also keeps the leash anchored where the fight actually is rather than where the first blow landed.
  const beeAngered = (x, y, z) => {
    const tb = performance.now() / 1000;
    for (let i = HIVE_BREAK.length - 1; i >= 0; i--) { const b = HIVE_BREAK[i];
      if (!b.swat) continue;   // SWAT records only — see the `swat` note above: folding a swat into a hive break would drag the wreck, and the leash with it, off the hive
      const ex = b.x - x, ez = b.z - z;
      if (ex * ex + ez * ez < BEE_SWAT_MERGE * BEE_SWAT_MERGE) { b.t = tb; b.x = x; b.y = y; b.z = z; return 0; }   // the same fight, moved
    }
    HIVE_BREAK.push({ x, y, z, t: tb, n: 0, key: '', swat: 1, r: BEE_RAGE_R });   // no HIVE_DONE key: a swat is not a hive coming apart, and must never mark one as spent
    while (HIVE_BREAK.length > HIVE_BREAK_MAX) HIVE_BREAK.shift();
    return 1;
  };
  // Is there a wreck near enough, and recent enough, for a bee at (x, z) to answer? Newest first, so two
  // overlapping breaks send a bee to the one that just happened. EVERY live bee is eligible for every record
  // (2026-08-19 — see the block above); the only thing that varies is the record's own reach, so this takes no
  // per-bee argument at all any more. The `|| BEE_RAGE_R` fallback is not defensive padding: __vb.hiveBreak and
  // any future poster that forgets the field then behave exactly as the swat path does rather than as a record
  // with a zero radius that nothing can ever hear.
  const hiveRageAt = (x, z, tb) => {
    for (let i = HIVE_BREAK.length - 1; i >= 0; i--) { const b = HIVE_BREAK[i];
      if (tb - b.t > BEE_RAGE_WIN) continue;
      const dx = b.x - x, dz = b.z - z, r = b.r || BEE_RAGE_R;
      if (dx * dx + dz * dz <= r * r) return b;
    }
    return null;
  };
  // ── A LIVE TAP ── window.__vbBee rather than a __vb.* method, for the reason sim/particles.js hands out
  // __vbPetal: main/debug-api.js is one shared fragment and this probes one state of one creature. It reads
  // the ALTITUDE SERVO and the CLOSER's own clearance from inside the rage (main/tick-creatures.js stashes
  // them there), which is the pair __vb.beeDbg cannot show and the pair that says WHY an enraged bee is not
  // arriving: `gAir` is the nav field's travel surface under the bee, `tgt` the height the servo is flying,
  // and `clr`/`want` how far the closer's direct line ran against how far it needed. A bee whose tgt sits at
  // gAir + 3 rather than near P.y + BEE_RAGE_Y is being held UP by the canopy it is standing on, not chasing.
  window.__vbBee = () => { const o = [];
    for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j];
      const sp = ((j - MAM_END) / DES_PER) | 0; if (((DESERTS[sp] || {}).name) !== 'bee') continue;
      if (!B || !B.init) continue;
      const r2 = (v) => (v === undefined ? null : +v.toFixed(1));
      o.push({ idx: (j - MAM_END) % DES_PER, m: B.beeM | 0, y: r2(B.y || 0),
        pd: r2(Math.sqrt((B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z))), dy: r2((B.y || 0) - P.y),
        gAir: r2(B.beeGA), tgt: r2(B.beeTgt), floor: B.beeGA === undefined ? null : r2(B.beeGA + 3),
        clr: r2(B.beeClr), want: r2(B.beeWant), spd: r2(B.beeRgSpd), trap: r2(B.trap || 0) }); }
    return { P: [Math.round(P.x), Math.round(P.y), Math.round(P.z)], rageY: BEE_RAGE_Y, bees: o }; };
  // ── ANT COLUMN ── spacing along the leader's path, and how often the leader drops a crumb. The gap is 6, not
  // the 3.2 the old steering aimed for: the baked ant model's box is 5 x 2 x 1, so at 3.2 the models would
  // interpenetrate even in a geometrically perfect line. The crumb step sets how faithfully a follower traces a
  // tight turn — finer is smoother and costs only array entries.
  const ANT_GAP = 6.0, ANT_CRUMB = 0.75;
  // How far ahead a walker checks for the biome line. 34 is a shade over two seconds of travel for the slowest
  // desert species and about half a second for a dashing gecko — far enough that the turn reads as a decision
  // rather than a bounce, close enough that it does not refuse ground it could legitimately have used.
  const BIO_LOOK = 34;
  // ── AND THE ANT WALKS ON THE COMPASS (user 2026-08-16: "just have it move forward. when it want to turn,
  // rotate the ant 90 degrees") ── the leader's heading is an INTEGER 0-3 into ANT_DIR, never an angle, so a
  // diagonal is not expressible; a turn is +/-1 on that integer and lands in one frame. This is the same
  // shape the armadillo/skunk/porcupine march already uses (see walkOK/B.ah in tick-creatures), which is why
  // it is written the same way here: cardinal table, a lookahead of ANT_LOOK voxels along a candidate
  // heading, and a COMMITTED turn side held for ANT_TURN_HOLD so a corner can never become a spin.
  const ANT_DIR = [[0, -1], [1, 0], [0, 1], [-1, 0]];
  const ANT_LOOK = 5;           // voxels of clear ground a heading must offer before the ant will walk it — the same 5 the marching mammals probe
  const ANT_TURN_HOLD = 0.35;   // seconds a turn is held before another may fire. Below ~0.2 a wedged ant reads as a twitch; this is one body-length of travel at 16 vox/s
  const ANT_WHIM = 1.5;         // an unblocked ant rolls for a turn every ANT_WHIM..2*ANT_WHIM seconds
  const ANT_WHIM_P = 0.35;      // …and turns on 35% of those rolls. Together that is a corner every ~6.4 s, or ~100 voxels of straight march at the band's 16 vox/s. MEASURED at 2.5/0.30 first and it was too sparse to be the behaviour the user asked for: one turn per 12.5 s is 200 voxels, so a player watching the column for ten seconds would usually never see the quarter turn at all.
  // ── WHICH DESERT-BAND KILLS LEAVE MEAT (user 2026-08-15) ── the four the user named. Ant, fly and spider
  // leave nothing, which is the same distinction they drew earlier asking for no drops off the bugs. Keyed by
  // NAME, not by slot number, so re-ordering DESERTS cannot silently re-assign it.
  // The two 2026-08-17 species split on exactly that line and neither is a guess about a new rule, only about
  // which side of the existing one they fall: the BEE is an insect like the fly and is absent, the GRASS
  // SNAKE is a 17-voxel snake beside the cobra's 19 and bleeds like it. Verify with __vb.meatSpecies().
  const DES_MEAT = { desert_mouse: 1, cobra: 1, scorpion: 1, gecko: 1, grass_snake: 1 };
  // WORLD creature pool. The LAYOUT is the ladder above — read the boundaries there, never off this line.
  //   FLY_0    flyers: butterflies by day, FIREFLIES after dark, dragonflies at the top of the band
  //   DUCK_0   mother ducks / BABY_0 ducklings, 3 per mother / WORM_0 worms (32 — DOUBLED AGAIN 2026-07-18)
  //   CARD_0   PERCHED SONGBIRDS — cardinal, blue bird and robin share the band (a third COLOUR was added when
  //            the robin joined, not a third share). Grid-stamped, so they use NO drop slot.
  //   FISH_0   fish (kind 6) — off-grid swimmers under the lake/river surface, 16 -> 32 slots 2026-07-20
  //   BUNNY_0  kind 2 ground hoppers reusing the worm machinery, model = BUNNY_ITEM0 (2026-07-21)
  //   ARM_0    kind 2 ground WALKERS, continuous cardinal march, model = ARMADILLO_ITEM0 (2026-07-21)
  //   SKUNK_0  kind 2 ground WALKERS, the armadillo's AI, poses from SKUNK_POSES (2026-07-21)
  //   PORC_0   kind 2 ground WALKERS, the armadillo's march, poses from PORCUPINE_POSES (2026-07-22)
  //   MAM_END  the DES_N desert-BAND species, DES_PER slots each, up to DES_END — seven on the sand plus the
  //            bee and the grass snake, which are oak-forest creatures riding the same machinery (DES_OAKONLY)
  const wbf = Array.from({ length: DES_END }, () => ({ init: false }));
  // ── DROP-SLOT PRIORITY ── six creature kinds share whatever is left of the drop slots once the 25 fixed ones
  // and the drawn flock have taken theirs (95 of 128 today). Rather than emit in wk order (which
  // starved whatever came last — fish, then worms), each wbf creature's 16-float pose is staged here with its distance²,
  // then after the loop the NEAREST are copied to the real slots. A fish right in front of you always renders; a far,
  // fog-bound butterfly is the one dropped. Allocated ONCE — never per frame.
  const EMIT_CAP = 216, emitBuf = new Float32Array(EMIT_CAP * 16), emitDp = new Float64Array(EMIT_CAP), emitIdx = new Int32Array(EMIT_CAP);
  const emitWho = new Int32Array(EMIT_CAP), emitAnc = new Float32Array(EMIT_CAP * 3), emitAna = new Uint8Array(EMIT_CAP);   // staged identity + world anchor + analytic-only flag → the dynamic-life slot ledger (filled in the nearest-first copy)
  // ── FAIR SHARE BETWEEN THE KINDS (user 2026-08-05: "dragonflies, salmon and ducks keep disappearing") ──
  // Every trace-injected creature competes for whatever is left of the 64 drop slots once the flying songbirds
  // have taken theirs: 64 - 25 - 11 = 28. The populations that contend for those 28 are far larger — 16
  // butterflies + 3 dragonflies + 4 mother ducks + 12 ducklings + 22 worms + 32 fish = up to 89. The surplus
  // used to be dropped by DISTANCE ALONE, which is not a fair rule when one kind clusters: MEASURED standing at
  // a lake, fish took 23 of the 28 slots and dragonflies, mother ducks and ducklings were drawn ZERO times out
  // of six samples, the nearest undrawn duck sitting 300 voxels away in plain view. In the forest the same
  // thing happened with butterflies and worms taking 28 of 28 and every fish and duck invisible.
  // So the allocator reserves the nearest LIFE_FLOOR of EVERY kind that has anything on screen before distance
  // is allowed to decide anything, and only then fills what is left nearest-first. A kind can still lose slots
  // to a closer crowd — that is the point of a budget — but it can no longer be shut out of the frame entirely.
  // FISH GET ONE BUCKET PER SPECIES, not one for "fish". There are five of them (bass, blue_gill, catfish,
  // minnow, salmon) sharing a 32-strong population, and to the player a salmon is as distinct a creature as a
  // duck is — so pooling them let distance alone decide which species you ever saw. MEASURED at a lake with the
  // kind-level share alone: bass 4.3 drawn, salmon 2.0; at a river bass 4.5, salmon 1.0. One guaranteed slot per
  // species costs 5 of the 28 and makes every species reliably present, which is the whole point of the floor.
  const LIFE_K_FLYER = 0, LIFE_K_DFLY = 1, LIFE_K_DUCK = 2, LIFE_K_BABY = 3, LIFE_K_WORM = 4, LIFE_K_OTHER = 5;
  const LIFE_K_FISH = 6, LIFE_FISH_MAX = 8, LIFE_K_BIRD = LIFE_K_FISH + LIFE_FISH_MAX, LIFE_KINDS = LIFE_K_BIRD + 1;   // LIFE_K_BIRD = the ?uni perched songbirds. Their own bucket, not LIFE_K_OTHER: pooled with the land mammals a forest full of birds would spend the whole share and the mammals would never be the ones drawn - the same reason ducklings are counted apart from their mothers.   // 6..13 = one bucket per loaded fish species
  const LIFE_FLOOR = 3, LIFE_FLOOR_FISH = 2;           // the general floor, and TWO slots per fish species — one was enough to prove a species was present but not enough to keep it there while it swam (user: "the salmon is still dissapearing")
  // ── A DUCK FAMILY IS ONE THING (user 2026-08-05: "the ducks are still disappearing") ── MEASURED over 50 s
  // beside a lake, the mothers never despawned at all: 4 active every frame, ZERO despawn events. What the
  // player was seeing was the draw budget — 2 of the 4 mothers on screen and 3 ducklings out of 12. A brood is
  // a mother plus three babies, so three duckling slots across four families means a mother paddling alone or
  // with one chick behind her, which reads exactly like ducks vanishing. There are only ever 4 mothers and 12
  // ducklings, so guaranteeing a whole family is cheap and bounded: every mother, and two complete broods.
  const LIFE_FLOOR_DUCK = 4, LIFE_FLOOR_BABY = 6;
  const lifeFloorOf = (k) => k === LIFE_K_BIRD ? LIFE_FLOOR : (k >= LIFE_K_FISH ? LIFE_FLOOR_FISH
    : (k === LIFE_K_DUCK ? LIFE_FLOOR_DUCK : (k === LIFE_K_BABY ? LIFE_FLOOR_BABY : LIFE_FLOOR)));   // the songbirds take the GENERAL floor - no special pleading for the kind that happens to be most numerous
  // ── AND THE FLOOR ONLY APPLIES WHERE YOU COULD ACTUALLY SEE IT ── a reserved slot spent on a speck is worse
  // than no reservation at all. Without this gate the fair share did exactly that: MEASURED, a mother duck at
  // 626 voxels and a fish at 928 held reserved slots while nearer creatures went undrawn. So the guarantee is
  // scoped to LIFE_DRAW, the one radius every kind is judged by, and anything beyond it competes for the
  // leftovers on distance alone — which means a far creature is still drawn when the scene is quiet, and never
  // at the cost of something in front of you. Creatures stay ALIVE out to LIFE_KEEP (~1040) as they always did;
  // this governs only which of them get one of the 28 slots.
  const LIFE_DRAW = 420, LIFE_DRAW2 = LIFE_DRAW * LIFE_DRAW;
  // ── AND ONE BIT SAYING "THIS BODY HAS NOWHERE ELSE TO BE DRAWN" (user 2026-08-20: "the land mammals and
  // insects are not rendering properly. they either dissapear or dont render at all") ── the four land mammals
  // are the only life with TWO render paths: grid-stamped into W when far, trace-injected into a drop slot when
  // near (uniTraced). Crossing inward they DROP the stamp and join this competition — and if they lose it they
  // are drawn by neither path. Measured walking with the budget saturated at 55/55: an armadillo invisible at
  // 291 voxels, repeatedly. Nothing else in the world can do that; a worm that loses is merely a worm that was
  // never stamped. The budget is not the bug — the ORDER is, so a body with no fallback is claimed before the
  // per-kind floors and a far worm gives up the slot instead. Costs nothing: the same number is still drawn.
  const emitMust = new Uint8Array(EMIT_CAP);
  const emitKnd = new Uint8Array(EMIT_CAP), emitTake = new Uint8Array(EMIT_CAP), emitKcnt = new Int32Array(LIFE_KINDS);
  const emitVis = new Uint8Array(EMIT_CAP);            // …and whether this creature is in the FRUSTUM at all — see the rank note at the emit
  // ── AND THE RANK IS VISIBILITY FIRST, DISTANCE SECOND ── the sort key used to be squared XZ distance and
  // nothing else, so a duck 60 voxels BEHIND your head outranked one 200 voxels in front of you. Creatures are
  // read only by TRACE's primary-hit block, COMPOSITE's analytic block and the VIS prepass — never by a
  // reflection, refraction, shadow or AO ray (ground shadows come from the separate 16-box u.cshad list) — so
  // one outside the frustum contributes NOTHING to the image and must never take a slot from one that does.
  // The radius is deliberately generous: over-including only keeps a creature in the visible group, whereas
  // under-including would demote something the player can actually see. LIFE_FRUST_HYST widens it again for a
  // creature that already holds a slot, so one straddling the frustum edge cannot flicker in and out of it.
  const LIFE_FRUST_R = 40, LIFE_FRUST_HYST = 40;
  let fishSchoolSeq = 0;                                // rolling id handed to each new fish school so same-school mates recognise each other (3-6 per school; ~28% of fish stay lone)
