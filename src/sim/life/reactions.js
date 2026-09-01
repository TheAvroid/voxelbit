  // @module - what a creature does when it is hit: hurt, spook, ragdoll, death and the meat it drops
  // @exports HURT, HURT_MS, RAG_UP, creatureRagdoll, dropMeat, dropsMeat, hitCreature, hurtBox, reapDeaths, spooked, tryKillCreature, hitsNeeded
  // ── STRUCK = SPOOKED (user 2026-08-09) ── every family that has a flee state already enters it on
  // PROXIMITY and runs its animation at double rate there: the bunny 24→48 fps (B.bflee), the land mammals
  // 12→24 (B.aflee), the fish ×fleeMult with its animation scaled to match (B.fleeT). Being SHOT did none of
  // that — an arrow could land on a rabbit and it would carry on ambling at walking pace. One window, set on
  // the blow in hitCreature and ORed into all three tests, so a hit reads the way proximity already does.
  // 5 s, not the fish's 1.2 s fleeHold: that number exists to stop flicker at the edge of the threat radius,
  // where the animal is deciding moment to moment. A wound is not a decision, and an animal that shrugs it off
  // in a second reads as not having noticed.
  // ── DOES THIS KILL LEAVE A CARCASS? ── the land mammals always have, and the DESERT band now does for the
  // five species listed in DES_MEAT; ant, fly, spider and now the BEE leave nothing, the same line the user
  // drew asking for no drops off the bugs — the bee needed no change here at all, only the confirmation that
  // an unlisted species falls out of this predicate FALSE (`__vb.meatSpecies().bee`). The GRASS SNAKE is
  // listed: it is a snake beside the cobra, not a bug. A named predicate rather than an inline test because it is the only way to check the
  // rule without a kill: `__vb.meatFor(slot)` calls exactly this, so the test and the game cannot disagree.
  // Keyed by NAME so re-ordering DESERTS cannot silently re-assign which creature bleeds.
  const dropsMeat = (j) => {
    if (j >= MAM_0 && j < MAM_END) return true;
    if (j < MAM_END || j >= DES_END) return false;
    const d = DESERTS[((j - MAM_END) / DES_PER) | 0];
    return !!(d && DES_MEAT[d.name]);
  };
  const HIT_SPOOK_MS = 5000;
  const spooked = (B) => performance.now() < (B.spookT || 0);
  const HURT_MS = 500;                                 // ONE blink, half a second (user). Comfortably longer than TAA's ~8-frame colour blend, which is what made an earlier 83 ms flash almost invisible on screen.
  const HURT = { t0: -1e9, hold: false, slot: -1, cx: 0, cy: 0, cz: 0, hx: 0, hy: 0, hz: 0 };   // world coords — rebased to the window at publish time, so a world shift mid-blink cannot drag the box off the animal
  const hurtBox = (B) => {                             // the animal's CURRENT stamped bounds — re-read every frame, because it is still walking while it flashes
    const q = B && B.sB && B.sN ? B.sB : null;
    // sB holds voxel INDICES, and voxel v fills [v, v+1) — so the animal's geometric span is [q0, q3+1] and its
    // true centre is half a voxel PAST the index midpoint. Taking the midpoint of the indices as the centre put
    // the box half a voxel low/behind on every axis: its +x/+y/+z faces landed exactly on the animal's own
    // surface (a knife-edge dHit == 1.0) while a whole voxel of slack went to waste underneath it, and the
    // shader's fade — which measures against hurtH-1 — then started INSIDE the animal on those three faces.
    // Measured on a live armadillo that left its back at 0.62 red against 0.98 on its belly.
    // ── A RAGDOLL CARRIES ITS OWN FLASH ── once the animal is a rigid body it is no longer where the AI
    // thinks it is: it is falling. Box it by the body's live centre and radius so the red stays ON it all
    // the way down, which is what "red and rigid at the same time" needs.
    // …and once it has BURST, the flash has to cover every piece, not just the biggest: box their union, which
    // grows with them as they fly apart, so the whole spray reads red for the half-second it is alive.
    if (B && B.ragParts && B.ragParts.length) {
      let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9, live = 0;
      for (const pb of B.ragParts) {
        if (!pb || PH.bodies.indexOf(pb) < 0) continue;  // a piece already absorbed or reclaimed drops out of the box
        const r = (pb.rMax || 3) + 0.6; live++;
        if (pb.pos[0] - r < x0) x0 = pb.pos[0] - r; if (pb.pos[0] + r > x1) x1 = pb.pos[0] + r;
        if (pb.pos[1] - r < y0) y0 = pb.pos[1] - r; if (pb.pos[1] + r > y1) y1 = pb.pos[1] + r;
        if (pb.pos[2] - r < z0) z0 = pb.pos[2] - r; if (pb.pos[2] + r > z1) z1 = pb.pos[2] + r;
      }
      if (live) { HURT.cx = (x0 + x1) * 0.5; HURT.cy = (y0 + y1) * 0.5; HURT.cz = (z0 + z1) * 0.5;
                  HURT.hx = (x1 - x0) * 0.5; HURT.hy = (y1 - y0) * 0.5; HURT.hz = (z1 - z0) * 0.5; return; }
    }
    if (B && B.ragBody) { const rb = B.ragBody, r = (rb.rMax || 4) + 0.6;
      HURT.cx = rb.pos[0]; HURT.cy = rb.pos[1]; HURT.cz = rb.pos[2];
      HURT.hx = r; HURT.hy = r; HURT.hz = r; return; }
    if (q) { HURT.cx = (q[0] + q[3] + 1) * 0.5; HURT.cy = (q[1] + q[4] + 1) * 0.5; HURT.cz = (q[2] + q[5] + 1) * 0.5;
             // The TRUE half-extent and NOTHING MORE (user: "the land mammals cast a red square on the
             // terrain"). The old +1 voxel of slack on every side is what stained the ground around them —
             // with the centre above corrected the animal is fully covered without it. The 0.02 is float
             // safety so a surface point sitting exactly on the boundary can't fall out of the box.
             HURT.hx = (q[3] - q[0] + 1) * 0.5 + 0.02; HURT.hy = (q[4] - q[1] + 1) * 0.5 + 0.02; HURT.hz = (q[5] - q[2] + 1) * 0.5 + 0.02; }
    else if (B) { HURT.cx = B.x; HURT.cy = B.y + 3; HURT.cz = B.z; HURT.hx = 5; HURT.hy = 5; HURT.hz = 5; }   // between stamps: a generous box beats no feedback
  };
  const tryKillCreature = () => { const b0 = aimedCreature(); if (b0 >= 0) hitCreature(b0); };   // LEFT-CLICK SWING KILL (user): whatever is under the crosshair takes the hit
  // ══ RAGDOLL (user 2026-08-05) ══ the KILLING blow turns the animal into a real rigid body — the same kind
  // a chopped trunk becomes — so it goes stiff and tumbles to the ground while it flashes red, and the yellow
  // poof then fires where it came to rest. ONLY the fatal hit does this; wounding hits are untouched.
  //
  // Two sources of voxels, because the game draws life two different ways:
  //   · GRID-STAMPED (mammals, perched birds) are already world voxels — take them and hand the world back
  //     what was underneath (unstampWorm), so the ragdoll never leaves the animal's silhouette punched into
  //     the pine it was sitting in.
  //   · TRACE-INJECTED (ducks, fish, flyers, butterflies) have no world voxels at all. Their ragdoll is built
  //     from the ITEM MODEL at the pose the renderer last drew, which the emit caches on the creature.
  //     Model colours are raw RGB, so each one is resolved through edCol — exact palette match first, then
  //     nearest once the 256 entries are full. That is the same route imported art already takes.
  const RAG_UP = [0, 1, 0];                             // the level frame's Z axis — shared, so caching a pose allocates nothing
  // ── A KILLED ANIMAL COMES APART (user 2026-08-07: "have it break apart, like they are chunks … only on the
  // hit that kills the life") ── the corpse is one rigid body by the time the red flash has run out, so this
  // partitions it into spatially coherent pieces and throws each clear. It fires at the SAME instant as the
  // death poof, so the sequence reads: red flash on the intact animal → it bursts → sparks and smoke. Nothing
  // about a non-fatal hit changes: those still spawn the red `spawnHitSparks` blood and leave the body whole.
  // Pieces are marked `rag` and given NO absorbAt: they are a corpse coming apart, not loot, so they fall and
  // tumble for the half-second the red flash lasts and are then removed with the death poof (see reapDeaths).
  // The 2×2×2 split of the body's own local bbox is what makes them read as broken parts rather than confetti:
  // each piece is a contiguous quadrant of the animal.
  const phShatter = (b) => {
    if (!b || !b.n) return 0;
    const sx = b.sx, sz = b.sz, key = (mx, my, mz) => mx + mz * sx + my * sx * sz;
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9;
    for (let i = 0; i < b.n; i++) {
      if (b.lx[i] < x0) x0 = b.lx[i]; if (b.lx[i] > x1) x1 = b.lx[i];
      if (b.ly[i] < y0) y0 = b.ly[i]; if (b.ly[i] > y1) y1 = b.ly[i];
      if (b.lz[i] < z0) z0 = b.lz[i]; if (b.lz[i] > z1) z1 = b.lz[i];
    }
    // ── DO NOT SPLIT AN AXIS TOO THIN TO SPLIT (user 2026-08-31: the death is "not breaking apart in
    // peices") ── the 2x2x2 quadrant cut below was written for the animal it was tested on and is
    // unconditional, so it also applies to everything SMALL. Measured on the creatures that actually live in
    // this forest: their models run 38-66 voxels, and eight quadrants of that is pieces of 2 to 12 voxels -
    // specks, scattering in half a second. The break-up is genuinely happening and cannot be seen, which is
    // exactly the report.
    // An axis is only worth cutting if there is something on both sides of the cut. Below SPLIT_MIN voxels of
    // extent the plane is pushed outside the model instead, so that axis contributes nothing and the body
    // comes apart into 2 or 4 chunky pieces rather than 8 crumbs. A big animal still has extent on all three
    // and still gets its eight, so nothing about the case this was tuned for changes.
    const SPLIT_MIN = 5;
    const mx2 = (x1 - x0 + 1) >= SPLIT_MIN ? (x0 + x1) * 0.5 : Infinity,
          my2 = (y1 - y0 + 1) >= SPLIT_MIN ? (y0 + y1) * 0.5 : Infinity,
          mz2 = (z1 - z0 + 1) >= SPLIT_MIN ? (z0 + z1) * 0.5 : Infinity;
    const idMap = new Map(), bucket = new Map();
    for (let i = 0; i < b.n; i++) {
      const kk = key(b.lx[i], b.ly[i], b.lz[i]); idMap.set(kk, b.id[i]);
      const q = (b.lx[i] > mx2 ? 1 : 0) | (b.ly[i] > my2 ? 2 : 0) | (b.lz[i] > mz2 ? 4 : 0);
      let a = bucket.get(q); if (!a) { a = []; bucket.set(q, a); }
      a.push(kk);
    }
    const bi = PH.bodies.indexOf(b); if (bi >= 0) PH.bodies.splice(bi, 1);   // out of the list BEFORE rebuilding — phBuildBody's reclaim may splice it too
    let made = 0; const out = [];
    for (const cells of bucket.values()) {
      if (cells.length < 2) continue;                    // a lone voxel is litter, not a piece
      if (PH.bodies.length >= PH.maxBodies && !phMakeRoom()) break;
      const nb = phSubBody(b, cells, idMap);
      const dx = nb.pos[0] - b.pos[0], dz = nb.pos[2] - b.pos[2];
      const d = Math.max(0.6, Math.hypot(dx, dz));
      nb.vel[0] += (dx / d) * 7 + (Math.random() - 0.5) * 4;   // outward from the animal's own centre, so the pieces open up rather than all going one way
      nb.vel[1] += 6 + Math.random() * 4;
      nb.vel[2] += (dz / d) * 7 + (Math.random() - 0.5) * 4;
      nb.omega[0] = (Math.random() - 0.5) * 7; nb.omega[1] = (Math.random() - 0.5) * 7; nb.omega[2] = (Math.random() - 0.5) * 7;
      nb.rag = true;                                     // ── A CORPSE IS NOT LOOT (user 2026-08-07: "the player seems to absorb the life when it breaks into chunks") ── no absorbAt, so nothing schedules them into the player, and `rag` also keeps the next swing from mincing them. They live exactly as long as the red does; reapDeaths removes them with the poof.
      nb.sleeping = false; nb.sleepT = 0;
      PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(nb); out.push(nb); made++; PH.stats.chunks++;
    }
    PH.stats.shattered = (PH.stats.shattered | 0) + made;
    if (!out.length) { PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(b); return out; }   // no room for even one piece — put the corpse back rather than deleting the animal outright
    return out;
  };
  const ragCellsBuf = [];
  const creatureRagdoll = (B) => {
    if (!PH.on) return null;
    ragCellsBuf.length = 0;
    if (B.sN && B.sW) {                                 // grid-stamped: lift its own world voxels
      for (let i = 0; i < B.sN; i++) { const v = W[B.sCells[i]]; if (!v) continue;
        ragCellsBuf.push([B.sW[i * 3], B.sW[i * 3 + 1], B.sW[i * 3 + 2], v]); }
      unstampWorm(B);                                   // …and put back the needles/ground it was standing in
    } else if (B.ragIt && itemsRef) {                   // trace-injected: rebuild from the model at its last drawn pose
      const it = itemsRef[(B.ragIt | 0) - 1];
      if (!it || !it.cells) return null;
      const ax = B.ragX, ay = B.ragY, az = B.ragZ, s = B.ragS || 1;
      // ── THE CORPSE APPEARS WHERE THE ANIMAL WAS (user 2026-08-05: "it teleports to a new location where it
      // proceeds to fall like a rigid body") ── ragA0/1/2 is the RENDER anchor, and the renderer centres a model
      // on its anchor (the same half-extents the hitboxes are built from: bdi.w * 0.5 ...). This rebuild walked
      // the model out from its CORNER instead, so every trace-injected ragdoll was born offset by half its own
      // body along all three axes. MEASURED on a mother duck: the body appeared 5.4 voxels to the side and 4.4
      // voxels ABOVE her and fell from there, which is exactly the teleport-then-drop that was reported.
      // Subtracting half the extent builds the corpse ABOUT the anchor instead of hanging off one corner of it.
      // This is not duck-specific: it fixes fish, butterflies and every other off-grid creature's death too.
      const hw = it.w * 0.5, hd = it.d * 0.5, hh = it.h * 0.5;
      for (let z = 0; z < it.h; z++) for (let y = 0; y < it.d; y++) for (let x = 0; x < it.w; x++) {
        const c = it.cells[x + y * it.w + z * it.w * it.d]; if (!c) continue;
        const mx = (x + 0.5 - hw) * s, my = (y + 0.5 - hd) * s, mz = (z + 0.5 - hh) * s;
        ragCellsBuf.push([Math.round(B.ragA0 + ax[0] * mx + ay[0] * my + az[0] * mz),
                          Math.round(B.ragA1 + ax[1] * mx + ay[1] * my + az[1] * mz),
                          Math.round(B.ragA2 + ax[2] * mx + ay[2] * my + az[2] * mz),
                          edCol(c[0], c[1], c[2])]);
      }
    } else return null;
    if (ragCellsBuf.length < 2) return null;
    if (ragCellsBuf.length > PH.absorbMax) return null;  // too big to be a body at all — leave the old death alone
    if (PH.bodies.length >= PH.maxBodies && !phMakeRoom()) return null;
    const fb = phBodyFromVoxels(ragCellsBuf);
    if (!fb) return null;
    // THE BLOW carries it: away from the player, a little upward, and tumbling. Small — this is a body going
    // limp and dropping, not a chunk being thrown.
    const dxK = fb.pos[0] - P.x, dzK = fb.pos[2] - P.z, dK = Math.max(1e-3, Math.hypot(dxK, dzK));
    fb.vel[0] += (dxK / dK) * 9; fb.vel[1] += 7; fb.vel[2] += (dzK / dK) * 9;
    fb.omega[0] = (Math.random() - 0.5) * 5; fb.omega[1] = (Math.random() - 0.5) * 5; fb.omega[2] = (Math.random() - 0.5) * 5;
    fb.sleeping = false; fb.sleepT = 0;
    fb.rag = true;
    PHSRC[phSrc] = (PHSRC[phSrc] || 0) + 1; PH.bodies.push(fb); PH.stats.chunks++;
    PH.stats.ragdolls = (PH.stats.ragdolls | 0) + 1;
    // ── AND IT COMES APART NOW, NOT WHEN THE FLASH ENDS (user 2026-08-07: "the chunks should play AS the red
    // animation plays … the chunks should get the red emissive voxels") ── shattering at reap meant the animal
    // stayed whole for the entire half-second of red and only burst once the red was over, so the two never
    // shared a frame. Splitting here puts the pieces in the air while HURT is still running, and hurtBox boxes
    // their UNION every frame, so each piece is inside the flash and glows with it as it flies.
    B.ragParts = phShatter(fb);
    B.ragBody = B.ragParts[0] || fb;                    // the largest piece — every existing ragBody consumer still has a body to read
    return B.ragBody;
  };
  // ── HOW MANY BLOWS THIS WEAPON NEEDS ── the one place the answer lives, so the hit path and the debug tap
  // cannot disagree. The KNIFE is read off heldIt() rather than off `token`, because it has no token of its
  // own: arrows and spears announce themselves because they arrive DETACHED from the player, while a knife
  // blow comes through the same hand-tool path as an axe or a bare fist and the only thing that tells them
  // apart is what is in the hand.
  // `slot` is optional and only the ARROW branch reads it: a flamingo takes two where everything else takes one.
  const hitsNeeded = (token, slot) => (token !== undefined && String(token).startsWith('arrow'))
    ? ((slot !== undefined && slot >= FLAM_0 && slot < FLAM_END) ? FLAM_ARROW_HITS : ARROW_HITS_TO_KILL)
    : ((KNIFE_IT && heldIt() === KNIFE_IT) ? KNIFE_HITS_TO_KILL : HITS_TO_KILL);   // the STONE KNIFE takes TWO (user 2026-08-19, was three): it is a cutting edge, so it sits with the arrow between the axe's one blow and every blunt hand tool's three

  const hitCreature = (best, token) => {                // …and this is the hit itself, on a KNOWN slot — callable from a test without having to aim first
    const B = wbf[best];
    if (!B || !B.init) return;
    const tk = token === undefined ? swingStart : token;   // an ARROW has no swing behind it, so it brings its own token for the one-hit-per-blow guard (user)
    // ── HOW MANY HITS ── the AXE kills outright; everything else takes THREE (user). One rule for every
    // life form and every tool — pick, shovel, knife, bow, a held rock, empty hands all wear it down.
    // …except a BUTTERFLY, which dies to anything in one hit (user): slots 0..15 are the butterfly/moth
    // band, and a creature that size surviving three blows from a shovel reads as absurd.
    if (B.lastSwing === tk) { return; }                 // this same swing already landed on this creature — one swing, one hit
    B.lastSwing = tk;
    const hs9 = hitSpot(B);
    vitOnAttack();                                      // a landed blow costs exhaustion, like Minecraft's 0.1
    spawnHitSparks(hs9[0], hs9[1], hs9[2]);             // SPARKS ON EVERY BLOW (user) — the same embers a shaft already threw, now on any hit, wounding or killing. Fired here, before the wound/kill split, so hits one, two and three all show it.
    playLifeHit(hs9[0], hs9[1], hs9[2]);                // …and the SAME rule for the sound (user 2026-08-08): every blow that lands on a living thing is heard, whatever swung it. Here, above the wound/kill split and below the one-hit-per-swing guard, so it is exactly once per blow — hits one, two and three included, and a held swing cannot machine-gun it.
    B.spookT = performance.now() + HIT_SPOOK_MS;        // …and it BOLTS (user 2026-08-09) — same place and same rule as the two above, so every blow spooks it, arrow or axe or bare hand, wounding or fatal
    B.thrX = P.x; B.thrZ = P.z;                         // …away from the PLAYER: the fish flee angles off a remembered threat position, and the one that just shot it is the threat
    // ── AND HITTING A BEE BRINGS THE REST (user 2026-08-17) ── posted HERE, beside the spook and above the
    // wound/kill split, for the reason every line around it is here: this is the one place every blow in the
    // game funnels through, so the swarm answers an arrow, a spear, an axe or a bare hand without any of them
    // needing to know about bees. It fires whether the blow wounds or kills — a bee swatted out of the air
    // still had witnesses. beeAngered (sim/life/slots.js) merges repeat blows into one fight.
    if (best >= MAM_END && best < DES_END && ((DESERTS[((best - MAM_END) / DES_PER) | 0] || {}).name) === 'bee')
      beeAngered(B.x, B.y || 0, B.z);
    const need = hitsNeeded(token, best);                  // …and it is a NAMED function so main/debug-api.js's hitsOn() can report the same number this line acts on: the tap printed the bare HITS_TO_KILL, which was true when three was the only answer and became a lie the moment the arrow got its own   // an ARROW is a weapon, not a hand tool: TWO and the animal is down (user)
    // …and the SPEAR kills outright, thrust or thrown — the axe's privilege, for the one tool that is a
    // weapon first (user). Everything else wears the animal down.
    const shaft = token !== undefined && (String(token).startsWith('spear') || String(token).startsWith('arrow'));
    // …and a BIRD drops to a single shaft, arrow or spear (user): kind 5 is the perched songbirds.
    const oneBlow = heldIt() === 1 || !!(SPEAR_IT && heldIt() === SPEAR_IT) || (token !== undefined && String(token).startsWith('spear'))
      || (shaft && (B.kind | 0) === 5);
    // …and the WORM band goes the same way (user): something that size surviving three
    // blows reads as absurd for the same reason the butterflies were exempted.
    // ── AND EVERY BUG IN THE DESERT BAND, THE BEE INCLUDED (user 2026-08-21: "make bees 1 hit like the other
    // insects. make all insects one hit") ── the FLY_0 band the rule started with holds only the butterflies,
    // moths, fireflies and dragonflies, so the ant, the housefly, the bee, the scorpion and the spider were still
    // riding the desert band's default three and the bee read as the exception to a rule the player had already
    // been taught. They are the same size class as everything the two lines above exempt. The list is DES_FRAIL
    // (sim/life/slots.js) — by NAME, because the band's bugs are not contiguous; see the note there.
    const desFrail = best >= MAM_END && best < DES_END && !!DES_FRAIL[((DESERTS[((best - MAM_END) / DES_PER) | 0] || {}).name)];
    const frail = best < FLY_END || (best >= WORM_0 && best < WORM_END) || desFrail;
    if (!B.dying && !oneBlow && !frail) {
      B.hits = (B.hits | 0) + 1;
      B.hurtAt = performance.now();                    // …and WHEN, which is what keeps the population controller from retiring it mid-fight (see the grace in main/tick-creatures.js)
      if (B.hits < need) {
        B.hurt = 1;
        B.hopT0 = performance.now();                    // the BOUNCE lands on every hit — it is what shows hits two and three registering
        B.blinked = true;                               // EVERY hit flashes (user): gating this to the first one made hits two and three look like misses.
        HURT.slot = best; HURT.hold = false; hurtBox(B);  // One flash per HIT, not two — the B.lastSwing guard above already allows a single hit per swing.
        HURT.t0 = performance.now();
        return;                                         // wounded, not dead
      }
    }
    // EVERY life form flashes red first (user). The poof and the slot teardown wait out the blink,
    // so the hit reads on the animal itself rather than the animal simply vanishing into smoke.
    if (B.dying) return;                                // already flashing its way out — a second click must not double-kill or restart the blink
    B.dying = true;
    if (dropsMeat(best)) B.mammal = true;   // …and BELOW the mammal band's end. This was open-ended because MAM_0.. WAS the whole band and the pool stopped at MAM_END; the desert creatures now live above it and were inheriting the land mammals' meat drop (user: 'the bugs shouldnt drop anything').                   // remember WHAT it was: by the time it dies the slot is only a bag of stale numbers
    const blink = true;                                 // the KILLING blow flashes too (user) — it used to be skipped whenever an earlier hit had already flashed,
    B.blinked = true;                                   // which left the third and fatal swing with no feedback of its own at all.
    // ── GO RIGID (user 2026-08-05) ── ONLY here, on the blow that kills: the animal becomes a rigid body and
    // starts falling this instant, so it is red AND a ragdoll together. Wounding hits never reach this line.
    // If the conversion cannot happen (physics off, model too big, no body slot) the old death plays out
    // exactly as before — B.rag stays false and the creature keeps rendering through its flash.
    if (creatureRagdoll(B)) B.rag = true;
    HURT.slot = best; HURT.hold = false; hurtBox(B); HURT.t0 = performance.now();
    B.hopT0 = B.rag ? undefined : performance.now();    // BOUNCE (user) — every hit, wounding or killing. A ragdoll does its own falling; the scripted hop would fight it.
    pendDeath.push({ slot: best, at: performance.now() + (blink ? HURT_MS : 0), born: B.born });   // …and with no flash to wait out, it dies immediately   // `born` is the slot's IDENTITY, so the reap can tell an emptied slot from a refilled one
  };
  const dropMeat = (B) => {                             // RAW MEAT where a land mammal fell (user): lands as an ordinary drop, so it hovers, spins and can be picked up like anything else
    const lx = Math.round(B.x), lz = Math.round(B.z);
    // ── A FISH LEAVES ITS MEAT ON THE SURFACE (user 2026-08-19: "when the player kills a fish, have the raw
    // meat hover above the water, where the fish was killed") ── every other kill drops at hmap, the GROUND
    // height of the column. For a fish that is the SEABED, so the meat would sink out of sight under however
    // many voxels of water it was swimming in, and the one drop the player cannot walk to is the one they have
    // to swim down for. WL is the global waterline (world/window.js), so this floats it where the fish died.
    // The x/z are the fish's own, untouched: "where the fish was killed" is the whole point, and only the
    // height moves. Drops already hover and spin above their y, so the meat bobs on the surface for free.
    const onWater = B.kind === 6;
    drops.push({ x: lx, y: onWater ? WL : hmap[gwrap(lx, WX) + gwrap(lz, WZ) * WX], z: lz, it: MEAT_IT, ph: Math.random() * 6.28,
      born: performance.now(), T: 0, q0: [0, 0, 0, 1] });
    if (drops.length > 8) drops.shift();   // ── 8 ITEM DROPS, NOT 4 (user 2026-08-20: "have the max number of floating hand held items on the field 8 instead of 4") ── drops.shift() DELETES the oldest, so a fifth drop did not just stop being drawn, it stopped existing. The render band moved with it: see the band map at dropCursor in main/tick-life.js
  };
  // …and now that the ragdoll and the poof both exist, a bird cut out of its perch gets the SAME death as one
  // struck directly: it goes rigid, drops out of the tree, and the yellow sparks fire where it lands. No red
  // flash — nothing hit the bird itself, its branch went — so this is a fall, not a wound.
  birdDeath = (B) => {
    const gone = creatureRagdoll(B);                    // lifts its stamped voxels and hands the needles back
    if (!gone) { if (B.sN) unstampWorm(B); spawnDeathBurst(B.x, B.perchFeet || B.y, B.z); }   // no body to be had (physics off, no slot) — at least poof where it sat
    else B.rag = true;
    B.dieT = 0; B.slain = true;
    if (gone) pendDeath.push({ slot: wbf.indexOf(B), at: performance.now() + HURT_MS, born: B.born });   // …the poof follows the fall, exactly as a struck animal's does
    else B.init = false;
  };
  const pendDeath = [];                                 // creatures mid-blink: {slot, at} — they die for real when the flash finishes
  const reapDeaths = (nowMs) => {                        // …and this is where that happens, one blink later
    for (let i = pendDeath.length - 1; i >= 0; i--) {
      if (nowMs < pendDeath[i].at) continue;
      const entD = pendDeath[i], slotD = entD.slot;
      const B = wbf[slotD];
      pendDeath.splice(i, 1);
      if (!B) continue;
      // ── A SLOT RETIRED DURING THE FLASH IS STILL A KILL (user 2026-08-27: "when I killed a frog, a new one
      // spawned in its place") ── `!B.init` used to skip this whole teardown, and the one piece of it that
      // must happen regardless is B.slain: without it the slot is merely EMPTY, so the population controller
      // fills it again with the same species, in the same place, and the animal the player just killed looks
      // like it came back. Half a second is long enough for one of the walker escapes in tick-creatures to
      // retire a body that cannot move, which is how a dying creature ends up here already deactivated.
      // Identity is B.born, the idiom the stuck-drop tracker already uses (main/tick-support.js:74): a slot
      // that has since been REFILLED holds a different animal and must be left alone, but an emptied one is
      // still the one that died. Nothing else runs for it — no poof, no meat — it is already off the field.
      if (entD.born !== undefined && B.born !== entD.born) continue;
      if (!B.init) { B.slain = true; B.dieT = 0; B.dying = false; continue; }
      if (MEAT_IT && pendDeath.length >= 0 && (B.mammal || B.kind === 6)) dropMeat(B);   // LAND MAMMALS leave RAW MEAT behind (user) — and FISH do too (user 2026-08-19), floated to the waterline by dropMeat rather than left on the seabed — armed at the hit, since B.x/B.y are about to stop being maintained
      // …the poof goes where the animal ACTUALLY IS. A ragdoll has been falling for the whole flash, so B.x/B.y
      // is where it was standing half a second ago; its body knows where it landed (user: it dies with the sparks).
      // The body goes WITH the poof: the sparks are the death, and leaving the corpse behind would both outlive
      // the moment and hand the player a dead animal to vacuum up on the ordinary chunk-absorb path.
      if (B.ragBody) { spawnDeathBurst(B.ragBody.pos[0], B.ragBody.pos[1], B.ragBody.pos[2]);
        // …and the pieces go WITH the poof (user 2026-08-07). They have been tumbling since the hit, red for as
        // long as HURT ran; the sparks are the end of the animal, so nothing of it is left behind to collect.
        for (const pb of (B.ragParts || [])) { const pi = PH.bodies.indexOf(pb); if (pi >= 0) PH.bodies.splice(pi, 1); }
        B.ragBody = null; B.ragParts = null; }
      else spawnDeathBurst(B.x, (B.kind | 0) === 5 ? (B.perchFeet || 0) : B.y, B.z);   // poof at the creature (a PERCHED BIRD's height lives in perchFeet — B.y is stale for kind 5)
      if (B.sN) unstampWorm(B);                           // clear a grid-stamped creature's world voxels (mammals + perched birds)
      if ((B.kind | 0) === 5 && B.tx !== undefined && cardSlainPerch.size < CARD_SLAIN_CAP) cardSlainPerch.add(cardPerchKey(B.tx, B.tz, B.bi));   // ── PERCH CLEARED ── so the next free slot cannot land another bird on the branch this one just died on (user)
      B.init = false; B.dieT = 0; B.slain = true; B.dying = false;         // SLAIN (user: "when I kill something, it respawns somewhere else — prevent this"): the slot is dead for the rest of the session — the population loop skips it, so nothing re-places it elsewhere
      if (slotD >= DUCK_0 && slotD < DUCK_END) startCrying(slotD);   // …a MOTHER duck: the brood she leaves behind cries (user). Armed HERE, not at the hit, so a wounded-but-alive mother never sets them off.
    }
  };
  document.addEventListener('mousedown', (e) => {
    if (!locked) return;
    // ── RIGHT CLICK ALSO PUTS THE HELD ITEM DOWN (user 2026-08-20) ── tested AFTER the pickup, so aiming at something
    // grabbable still grabs: a flower in the hand must not stop you picking up the rock in front of you. It
    // leaves eatHold FALSE when it plants, which is what makes planting one-per-click — holding the button
    // would otherwise carpet the ground through the same auto-repeat that eats a stack of apples.
    if (e.button === 2) { if (!mouse2) { bowT0 = performance.now(); if (BOW_IT && heldIt() === BOW_IT) playBowStretch(); } mouse2 = true; tryPickup();
      if (!grabAnim && tryPlaceItem()) { eatHold = false; } else { eatHold = !grabAnim; if (eatHold) tryEat(); } }   // …and with food in hand and nothing to grab, you EAT it (user)   // …and the string starts creaking under the pull (user)   // …the bow starts DRAWING here (user), and the right button stays HELD as the throw wind-up
    if (e.button === 0 && ED.on) {                     // ASSET EDITOR: left-click the stamped cardinal to SELECT it (pause) / click again to resume; then , . scrub the frames
      const n = ED.frames.length;
      if (n) {                                         // AABB slab test against the model's bounding box — forgiving for a small SPARSE model (an exact-voxel ray slips through the gaps)
        const cp2 = Math.cos(P.pitch), sp2 = Math.sin(P.pitch), dd = [Math.sin(P.yaw) * cp2, sp2, Math.cos(P.yaw) * cp2];
        const eo = [P.x, smoothEye, P.z];
        if (ED.giz && ED.paused && ED.gizBoxes.length) {   // grab a gizmo arrow first — the nearest one the crosshair passes through starts a drag
          let bestA = -1, bestT = 1e9;
          for (const g of ED.gizBoxes) { let tmin = 0.1, tmax = 300, ok = true;
            for (let a = 0; a < 3; a++) { const iv = 1 / (Math.abs(dd[a]) < 1e-9 ? 1e-9 : dd[a]);
              let ta = (g.min[a] - eo[a]) * iv, tb = (g.max[a] - eo[a]) * iv; if (ta > tb) { const s = ta; ta = tb; tb = s; }
              tmin = Math.max(tmin, ta); tmax = Math.min(tmax, tb); if (tmin > tmax) { ok = false; break; } }
            if (ok && tmin < bestT) { bestT = tmin; bestA = g.axis; } }
          if (bestA >= 0) { ED.dragAxis = bestA; ED.dragAcc = 0; return; }   // dragging this axis now — mousemove nudges the frame
        }
        if (ED.rgiz && ED.paused && ED.rgizBoxes.length) {   // grab a rotation RING — the nearest ring-slab the crosshair passes through starts a rotate-drag
          let bestK = null, bestT = 1e9;
          for (const g of ED.rgizBoxes) { let tmin = 0.1, tmax = 300, ok = true;
            for (let a = 0; a < 3; a++) { const iv = 1 / (Math.abs(dd[a]) < 1e-9 ? 1e-9 : dd[a]);
              let ta = (g.min[a] - eo[a]) * iv, tb = (g.max[a] - eo[a]) * iv; if (ta > tb) { const s = ta; ta = tb; tb = s; }
              tmin = Math.max(tmin, ta); tmax = Math.min(tmax, tb); if (tmin > tmax) { ok = false; break; } }
            if (ok && tmin < bestT) { bestT = tmin; bestK = g.kind; } }
          if (bestK) { ED.dragRing = bestK === 'yaw' ? 0 : 1; ED.dragRAcc = 0; return; }   // dragging this ring — mousemove rotates in 90° steps, both directions
        }
        // ── PICK THE BOX THAT WAS ACTUALLY STAMPED ── this used to rebuild the model's extent from f.bx/f.bz plus
        // the frame's own offsets, and that reproduces where the model is only while it is standing still at its
        // base position. It misses two things edLayout applies: the marching hop (ED.hopX/Y/Z, added while
        // PLAYING and wrapped back onto the stage), and the rotated footprint (f.sxR/f.syR — sx/sy are the
        // unrotated dims). Both were invisible for as long as every import had zero offsets, because then the
        // cycle offset last-minus-first is zero and the model never marches. Baking the frog's hop (see
        // FROG_BAKE) made it -10 in z, the frog started hopping across the stage, and the pick box stayed
        // behind at the base position: clicking the frog stopped selecting it (user 2026-08-21).
        // edLayout already computes the true extent of the voxels it wrote, publishes it as ED.box, and the
        // frame loop hands that same box to birdBox as the player's collision volume. So ask IT rather than
        // deriving a second answer that can disagree — one box, one source, and it cannot drift again.
        const f = ED.frames[((ED.sel % n) + n) % n], EB = ED.box;
        const ox = f.ox || 0, oy = f.oy || 0, oz = f.oz || 0;
        const bmin = EB ? [EB.cx - EB.hx, EB.cy - EB.hy, EB.cz - EB.hz] : [f.bx + ox, ED.y + 1 + oy, f.bz + oz];
        const bmax = EB ? [EB.cx + EB.hx, EB.cy + EB.hy, EB.cz + EB.hz] : [f.bx + ox + f.sx, ED.y + 1 + oy + f.sz, f.bz + oz + f.sy];   // the fallback is the old derivation, for the one case ED.box is null: a model clipped entirely off the stage, which stamps nothing to measure
        let tmin = 0.1, tmax = 300;
        for (let a = 0; a < 3; a++) { const iv = 1 / (Math.abs(dd[a]) < 1e-9 ? 1e-9 : dd[a]);
          let ta = (bmin[a] - eo[a]) * iv, tb = (bmax[a] - eo[a]) * iv; if (ta > tb) { const s = ta; ta = tb; tb = s; }
          tmin = Math.max(tmin, ta); tmax = Math.min(tmax, tb); }
        if (tmin <= tmax) { ED.paused = !ED.paused; edLayout(); }   // the view ray passes through the model → toggle select
      }
      if (!dead && performance.now() - swingStart >= 570) { swingStart = performance.now(); }   // SWING the held item in the editor too (user)   // no whoosh on the swing any more (user 2026-08-07): the tool is heard when it BREAKS something, not when it is waved — reached on any click that didn't grab a gizmo (those return above); select-toggle only fires on this mousedown, the swing then auto-repeats via the tick loop while held
      mouse0 = true;
      return;
    }
    if (e.button === 0 && !dead && mouse2 && ROCK_IT > 0 && heldIt() === ROCK_IT) {
      dropHeld(true);                                  // ── THROW ── right held + left click, rock in hand (user). Consumes the rock and sends it flying flat.
      playSwish();
      return;                                          // no swing this click: the hand is empty now
    }
    if (e.button === 0 && !dead) {
      // ── THE BENCH OWNS SHIFT+CLICK WHILE IT IS OPEN (user 2026-08-20, re-asking for shift+click to be what
      // crafts) ── the `!CRAFT.open` is why the gesture actually commits. The comment below used to claim the
      // clash and the bench were "mutually exclusive by construction"; they are not. dualRocks() is n >= 2, not
      // n === 2, so a player holding the 3 rocks an axe costs with a stick in another slot satisfies BOTH — and
      // this line, being first, took every shift+click and clashed the rocks together instead of making the tool
      // the player was looking at. Nothing about the knife changes: with the bench closed this is still the first
      // test, so two rocks and a shift+click are still a knife.
      if (e.shiftKey && !CRAFT.open && dualRocks() && (slots[selSlot].n === 2 || canAdd(KNIFE_IT)) && performance.now() - clashT0 > 700) { clashT0 = performance.now(); clashSparked = false; return; }
      // ── …AND THE SAME GESTURE OPENS THE STONE AGE BENCH FOR A ROCK + A STICK (user 2026-08-19) ── tested AFTER
      // the clash, so two rocks still make a knife and nothing about that path changes. Same 700 ms re-arm, and
      // the same `return` — the click is the gesture, so it must not also swing the tool.
      // WITH THE BENCH CLOSED the clash still wins a tie, and that is deliberate: a knife is the recipe for two
      // rocks and the bench is the recipe for a rock and a stick, so selecting the STICK is what asks for the
      // bench. (This IS a tie the old comment said could not happen — three rocks in the selected slot and a
      // stick in another satisfies both — so it is a rule, not an accident.)
      // ── SHIFT + CLICK IS BOTH HALVES OF THE BENCH (user 2026-08-19: "instead of enter to craft something its
      // shift + left click", and again 2026-08-20) ── the same gesture opens it and commits it, which is why
      // this is tested BEFORE the open below: while the bench is up, a shift+click can only mean "make this
      // one". Enter still works (ui/input.js keeps it) — a second way in costs nothing.
      if (e.shiftKey && CRAFT.open) { craftConfirm(); return; }
      if (e.shiftKey && !CRAFT.open && performance.now() - clashT0 > 700 && craftOpen()) { clashT0 = performance.now(); return; }   // clash instead of swing — only when the knife will have somewhere to go (exactly-2 rocks frees this slot)
      // ── AND AN UNSHIFTED CLICK WHILE THE BENCH IS OPEN DOES NOTHING (user 2026-08-20: "the player can still
      // hit objects while theres a crafted item in the middle of the screen") ── the bench is a MODAL chooser:
      // both hands are drawn into it, the tool they were holding is hidden from CRAFT.lit, and the preview
      // hovers on the crosshair. A swing from there had no hand to come from and no tool to swing, yet it still
      // chopped whatever the crosshair was resting on — the player was breaking blocks and killing animals
      // through the menu. Swallowed rather than passed through: `mouse0` is deliberately NOT set either, so
      // holding the button down through the whole gesture cannot auto-repeat a swing the moment it closes.
      if (CRAFT.open) return;
      mouse0 = true;
      if (performance.now() - swingStart >= 570) { swingStart = performance.now(); pendKillT = swingStart + 250; }   // …and no whoosh here either (user 2026-08-07) — see playToolHit   // a click is IGNORED until the current swing's full 570 ms animation finishes — no mid-swing restart (user; applies to every held item). Holding still auto-repeats via the tick loop. The creature-hit is ARMED here but registers 250 ms in, when the axe visually lands (user).
    }
  });
  document.addEventListener('mouseup', (e) => { if (e.button === 2) { if (mouse2) { bowRel = performance.now();   // …and releasing LOOSES it, running 03→06 (user)
      if (BOW_IT && heldIt() === BOW_IT) stopBowStretch();   // the pull is over — a half-draw must not leave the creak ringing over the release
      if (BOW_IT && heldIt() === BOW_IT && (bowRel - bowT0) > BOW_DRAW_MS * 0.5) { bowLoosed = true; if (shootArrow()) playSwish(); }   // …and the shaft leaves with the whoosh (user) — only when one actually leaves: an empty quiver releasing the string with a whoosh and no arrow is the one way this could lie about what happened   // …the ARROW LEAVES the bow (user): the bare strip takes over and a projectile flies
      else if (SPEAR_IT && heldIt() === SPEAR_IT && (bowRel - bowT0) > 90) { if (throwSpear()) playSwish(); }   // …and the SPEAR flies out of the raised hand (user)
      }
    mouse2 = false; eatHold = false; }
    if (e.button === 0) { mouse0 = false;
    if (ED.dragAxis >= 0) { ED.dragAxis = -1; edSaveOffsets(); edHudUpd(); }   // let go of a gizmo arrow → AUTOSAVE the new offset
    if (ED.dragRing >= 0) { ED.dragRing = -1; edHudUpd(); } } });   // let go of a rotation ring

