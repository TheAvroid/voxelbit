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
  const bfly = { init: false, x: 0, z: 0, th: 0, om: 0, omT: 0, tRe: 0 };   // the editor butterfly's wander state — slot 4
  const cardSlainPerch = new Set(), CARD_SLAIN_CAP = 512;   // perches whose bird the player KILLED — see findPineCrown. Module scope on purpose: the kill handler and the placement search are ~4000 lines apart.
  const cardPerchKey = (tx, tz, bi) => tx + '|' + tz + '|' + bi;
  const wbf = Array.from({ length: 372 }, () => ({ init: false }));   // WORLD creature pool: 0-15 flyers, 16-19 MOM ducks, 20-31 ducklings (3/mom), 32-63 worms (32 — DOUBLED AGAIN 2026-07-18), 64-243 PERCHED SONGBIRDS (180 slots — 120 -> 180 when the ROBIN joined, so it ADDS a third colour instead of eating the cardinal/blue-bird share — grid-stamped, so they use NO drop slot; free to exceed the drop budget), 244-275 FISH (kind 6 — off-grid swimmers under the lake/river surface; 16 -> 32 slots, DOUBLED 2026-07-20 at user request), 276-299 BUNNIES (24 — kind 2 ground hoppers reusing the worm machinery, model = BUNNY_ITEM0, added 2026-07-21 at user request), 300-323 ARMADILLOS (24 — kind 2 ground WALKERS, continuous cardinal march, model = ARMADILLO_ITEM0, added 2026-07-21 at user request), 324-347 SKUNKS (24 — kind 2 ground WALKERS, same cardinal-march AI as the armadillo, GRID-STAMPED via SKUNK_POSES, added 2026-07-21 at user request), 348-371 PORCUPINES (24 — kind 2 ground WALKERS, armadillo-style constant march, GRID-STAMPED via PORCUPINE_POSES, re-added 2026-07-22 at user request as a 4th land mammal alongside the skunk)
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
