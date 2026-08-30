    for (let wk = 0; wk < DES_END; wk++) {
      const B = wbf[wk];
      if (B.slain) continue;
      // ── A BODY WITH NO REAL POSITION IS NOT A BODY (user 2026-08-27: "only the mother duck is showing. I
      // dont see the babies") ── NaN loses every comparison it takes part in, so a slot that has picked one up
      // reads as NEAR to the surplus grace, INSIDE the recycle radius and CLOSE ENOUGH to its mother to the
      // rejoin test: not one of them can retire it, because each is a `>` or a `<` and NaN fails them all.
      // Meanwhile it still emits, at NaN, which draws nothing at all — so the slot is spent on an invisible
      // creature for ever. MEASURED on the ducklings at a lake: NINE of twelve held init and a drop slot for
      // 600 straight frames with x = NaN, mothers long retired, which is a brood you cannot see beside a
      // mother you can. Caught HERE, at the top, before any test downstream can be fooled by it.
      // X AND Z ONLY. B.y is NOT part of the test and must not be: a PERCHED songbird keeps its height in
      // perchFeet and its B.y is deliberately stale (see the poof at reapDeaths, which reads perchFeet for
      // exactly this reason), so including y here retired all 421 of them every frame and set the whole perch
      // search running again — MEASURED at 35-46 ms of life tick across every biome, a 28 fps world, for a
      // guard that was meant to cost nothing. The plane position is the thing every body genuinely has and
      // the thing every distance test is built on, which is all this needs to protect.
      if (B.init && !(isFinite(B.x) && isFinite(B.z))) { if (B.sCells) unstampWorm(B); B.init = false; B.dieT = 0; continue; }
      // ── EVERY CREATURE BLINKS (user 2026-08-19: "implement a universal blinking feature") ── one clock for
      // all of them, on the state the duck already used, so no creature can end up with two blink timers
      // disagreeing. Read only by the creatures whose strip actually carries lid variants (BLINK_HAS, at the
      // emit below); the flag costs nothing on the rest, and a strip that gains variants later starts blinking
      // with no further wiring. The beat and the gap are the duck's, tuned by hand: 150 ms shut, 2.2-4.8 s
      // open, re-rolled every time so a herd never falls into step.
      // AT THE TOP OF THE LOOP ON PURPOSE. It was first placed down beside the emit, and 125 of 546 creatures
      // froze mid-blink with a deadline already in the past — everything that takes one of the many `continue`
      // paths between here and there (grid-stamped mammals, culled slots, the recycle branch) simply stopped
      // advancing its clock. A clock has to tick on every frame the creature exists, not only on the frames it
      // happens to be drawn.
      if (B.init && (!B.blinkT || now > B.blinkT)) { B.blink = !B.blink; B.blinkT = now + (B.blink ? 150 : 1100 + Math.random() * 1300); }   // gap HALVED (user 2026-08-19: "double the rate of the blinking") — 2.2-4.8 s -> 1.1-2.4 s. The 150 ms shut is untouched: doubling that too would read as a slow wink rather than a blink                           // KILLED BY THE PLAYER (user): a slain slot never recycles or re-places — the creature is gone for the session (kill already cleared its stamp; init=false keeps it out of every emit/census)
      if (B.rag) continue;                             // RAGDOLLED: the rigid body IS the animal now. Steering, animating or emitting it again would draw a second copy standing where it used to be while the real one falls.
      const desSlot = wk >= MAM_END, desSp = desSlot ? ((wk - MAM_END) / DES_PER) | 0 : 0;
      const DES_FPS = { scorpion: 12 };                   // ── PER-SPECIES ANIMATION RATE ── unlisted species keep the 24 fps house rule
      const DES_SPD = { desert_mouse: 32, gecko: 32 };    // ── PER-SPECIES TRAVEL SPEED ── doubled from the shared 16 (user). Purely spatial: animClk accumulates raw dt and never reads speed, so the animation rate is unchanged
      // ── WHO BOLTS WHEN YOU GET CLOSE (user 2026-08-15) ── doubles its travel speed inside DES_DASH_R. Applied
      // to the SPEED, not to the animation rate, so the gecko keeps its 24 fps gait and simply covers ground
      // faster — the same split the mouse's speed doubling used.
      // ── WHO CHANGES PACE WHEN THE PLAYER IS CLOSE ── x2 inside DES_DASH_R, whichever way they are pointing.
      // The gecko and the mouse spend it RUNNING (the flee block steers them away); the cobra and the scorpion
      // spend it CHARGING, because DES_HUNT already steers them at the player and the flee block excludes any
      // hunter. So one table drives two opposite behaviours and neither species needs a special case.
      // ── …AND THE GRASS SNAKE TAKES THE COBRA'S NUMBER ON THE OTHER SIDE OF IT (user 2026-08-17) ── it is the
      // cobra's model, animation cadence and travel speed, and the ONE deliberate difference is that it is
      // absent from DES_HUNT. Because run8 below is `DES_DASH && !DES_HUNT`, the identical 2 that makes a
      // cobra CHARGE makes a grass snake BOLT — same table, same constant, opposite behaviour, no special
      // case. That is the whole of 'harmless': it never enters the bite block at all, so there is no reach,
      // no cooldown and no damage number to tune.
      const WD_WIN = 4.0, WD_MIN = 3.0, WD_GIVE = 3;   // ── NO-PROGRESS WATCHDOG ── window in seconds, the net displacement that counts as progress inside it, and how many stalled windows before a creature is recycled rather than re-headed. See the block that reads them, above the recycle test
      const DES_DASH = { gecko: 2, desert_mouse: 2, cobra: 2, scorpion: 2, grass_snake: 2 }, DES_DASH_R = 70;
      // ── THE TWO ADMIT ENDS OF THE BIOME GATE ── desertM at or past BIO_DESERT is open sand, at or under
      // BIO_FOREST is closed forest, and the span between them is a treeline nothing spawns in. DESB is 450
      // voxels of blend, so 0.15/0.85 puts each side's nearest spawn about 100 voxels - 10 m - off the
      // boundary's centre line, and leaves roughly 20 m of empty ground between the two populations.
      const BIO_DESERT = 0.85, BIO_FOREST = 0.15;
      const DES_HUNT = { cobra: 1, scorpion: 1 };         // ── WHO HUNTS THE PLAYER ── (user 2026-08-15)                   // ── PER-SPECIES ANIMATION RATE ── the scorpion reads slow at the house 24 (user 2026-08-15); everything unlisted stays 24
      // ── WHICH COLUMN THIS ANT IS IN ── a species' DES_PER slots are split in two: the desert's own
      // population first, then the FOREST one (DES_OAK, narrowed to the birch band by DES_BIRCHF —
      // sim/life/slots.js). Each run is its own column with its own leader and its own breadcrumb trail, so
      // the base is the first slot of the run this ant belongs to. Without it every birch ant is a follower
      // of slot 0, which is a DESERT ant: they would queue up behind a leader a biome away and be placed on
      // a trail recorded in the sand.
      const antBase = (sp9, k9) => { const ix9 = (k9 - MAM_END) % DES_PER, d9 = nDesertOf(sp9); return ix9 < d9 ? 0 : d9; };
      const antLead = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'ant' && ((wk - MAM_END) % DES_PER) === antBase(desSp, wk);   // ── THE ANT COLUMN'S HEAD ── slot 0 of the ant band is the only ant that decides anything: it marches on the compass (its own branch in the steering chain below), and every other ant is PLACED on the path it recorded. Keyed on the NAME like desFly, so re-ordering the load list cannot promote some other animal to leader.
      // ── WHO IN THE BAND FLIES (user 2026-08-15, + the BEE 2026-08-17) ── a table for the same reason
      // DES_HUNT and DES_MEAT are tables: keyed on the NAME, not the index, so re-ordering the load list
      // cannot silently turn some other animal into a flyer. A member is kind 0 — the butterfly's whole code
      // path, its speed, its altitude servo and its arbiter branch — rather than the band's default kind 2.
      const DES_FLYER = { fly: 1, bee: 1, ladybug: 1 };   // the LADYBUG flies (user 2026-08-22) — it takes the fly's whole air behaviour, minus the bunching, which keys on the name 'fly' below
      const desFly = desSlot && DESERTS[desSp] && !!DES_FLYER[DESERTS[desSp].name];
      const desLbug = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'ladybug';
      const desFrog = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'frog';   // the one creature that HOPS: see the leap block below   // it flies like the others (DES_FLYER) but it is the only one that LANDS — see the state machine below
      const desBee = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'bee';   // …and the ONE member with errands. Everything else about it is the fly.
      const DES_FLY_UP = 16;                             // …and rides this much higher than a butterfly's glide line   // ── DESERT CREATURES ── appended after the mammals, DES_PER slots per species, species index off the slot the same way the fish take B.fsp
      const flamSlot = wk >= FLAM_0 && wk < FLAM_END, porcSlot = wk >= PORC_0 && wk < PORC_0 + MAM_PER, skunkSlot = wk >= SKUNK_0 && wk < SKUNK_END, armSlot = wk >= ARM_0 && wk < ARM_END, bunnySlot = wk >= BUNNY_0 && wk < BUNNY_END, fishSlot = wk >= FISH_0 && wk < FISH_END, wormSlot = wk >= WORM_0 && wk < WORM_END, duckSlot = wk >= DUCK_0 && wk < BABY_END, lilySlot = wk >= CARD_0 && wk < CARD_END;
      // ── A DESERT SPECIES THAT ALSO LIVES IN THE OAK FOREST (user 2026-08-17: "implement the mouse like done
      // in the desert inside of the oak forest") ── keyed on the NAME, like desFly and antLead, so re-ordering
      // the load list cannot silently move a habitat onto some other animal. The value is how many of the
      // species' OWN DES_PER slots go to the oak population, and it is CLAMPED to what its desert head-count
      // leaves spare: the mouse is species 2, nDesertOf gives it 4 of its 8, so 4 are free and the oak forest
      // takes them. Three consequences, and all three are the reason it is done this way rather than by adding
      // slots: the desert's own mouse count is not touched, no other species' band is touched, and DES_END is
      // unchanged so every `< DES_END` loop in the game (particles, projectiles, the debug taps) still covers
      // exactly the pool it always did. If DES_RARITY is ever raised the clamp gives the slots BACK to the
      // desert rather than overrunning the band — the oak population shrinks, nothing breaks.
      const desIx = desSlot ? (wk - MAM_END) % DES_PER : 0;
      // …and an OAK-ONLY species (DES_OAKONLY, sim/life/slots.js) takes its WHOLE population from this term,
      // because nDesertOf gave it zero. One expression, not a second branch: for the mouse both operands are
      // character for character what they were, so its arithmetic is untouched.
      // ── THE MOUSE'S OAK HALF NEEDED THE NIGHT GATE TOO (user 2026-08-20) ── nOakOf carries the dusk ramp
      // for the bee and the grass snake, and its own comment in tick-life.js records exactly why an oak-only
      // species cannot rely on nDesert going to zero. The desert MOUSE reaches this line by the OTHER arm —
      // DES_OAK, a plain constant 4 — and that arm was never gated on anything. Measured at dusk: every other
      // ground creature had thinned to nothing and the desert band still held 4, which is these mice, out
      // foraging in the dark on their own. Same nightK, so both arms of the same expression now go quiet
      // together instead of one of them staying up all night.
      const oakN = desSlot && DESERTS[desSp] ? Math.min(DES_PER - nDesertOf(desSp), DES_OAKONLY[DESERTS[desSp].name] ? nOakOf(desSp) : nightK(DES_OAK[DESERTS[desSp].name] | 0)) : 0;
      const oakSlot = oakN > 0 && desIx >= nDesertOf(desSp) && desIx < nDesertOf(desSp) + oakN;   // the TAIL of the species' band is the oak population — a pure function of the slot number, so there is no per-body state to go stale across a recycle
      // ── AND THE ONE TAG THE FIVE BIOME GATES ALL READ ── see bioHomeOK in sim/nav.js. BIO_ANY is what every
      // creature in the game was before today, and its arithmetic there is the old test unchanged. The two new
      // values are the whole of this change's biome logic: the oak mouse, and the PORCUPINE, which the user
      // removed from the oak forest on the same day (2026-08-17) and which is therefore the one forest species
      // that no longer means "either forest". The other three land mammals are deliberately still BIO_ANY.
      const bioMe = oakSlot ? (DES_BIRCHF[(DESERTS[desSp] || {}).name] ? BIO_BIRCH : DES_ANYFOREST[(DESERTS[desSp] || {}).name] ? BIO_ANY : BIO_OAKF) : (desSlot ? BIO_SAND : (flamSlot ? BIO_CHERRY : (porcSlot ? BIO_PINEF : BIO_ANY)));   // DES_ANYFOREST widens an oak-only species to BOTH forests (sim/life/slots.js)   // the FLAMINGO is the cherry forest's own, and the only creature that takes BIO_CHERRY   // (the pink bird takes BIO_CHERRY at its own spawn site — it is not in this pool)
      const isBaby = wk >= BABY_0 && wk < BABY_END, sib = isBaby ? (wk - BABY_0) % 3 : 0;
      const mom5 = isBaby ? wbf[DUCK_0 + (((wk - BABY_0) / 3) | 0)] : null;   // the first three ducklings belong to the first mother, the next three to the second, and so on (3 each)
      // ── ORPHANED (user 2026-08-05: "killing the mom kills all the baby ducks — each duck has to be killed")
      // A brood used to be defined out of existence the instant its mother's slot went inactive, so one hit on
      // the mom silently deleted three ducklings that were never touched. Only a SLAIN mother makes orphans:
      // she is gone for the session, so the deactivation is permanent and the brood must stand on its own.
      // A merely RECYCLED mother (walked out of range, window recentred) still takes her babies with her —
      // that is population churn far from the player, not a kill, and the pair must recycle together.
      const orphan = isBaby && !!mom5.slain;
      const wantK = desFly ? 0 : desSlot ? 2 : (bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot) ? 2 : (fishSlot ? 6 : (lilySlot ? 5 : (wormSlot ? 2 : (duckSlot ? 3 : (moonMode ? 1 : 0)))));   // BUNNIES + ARMADILLOS + SKUNKS + PORCUPINES are kind 2 (worm machinery) — the slot band, not the kind, tells them apart at emit/AI
      const isDfly = !desSlot && wantK === 0 && nDfly > 0 && waterSpots.length > 0 && wk >= nAct - nDfly;   // !desSlot: a desert slot number is far above nAct, so this would call the fly a dragonfly and then refuse to place it anywhere but water   // DRAGONFLIES take the TOP of the active flyer band — butterflies only give up slots when there is water in view   // day = butterflies, night = FIREFLIES; 16-19 MOM DUCKS, 20-31 ducklings, 32-63 WORMS (32), CARD_0..CARD_END PERCHED SONGBIRDS, FISH_0..FISH_END FISH
      const active = desSlot ? (desSp < DESERTS.length && desIx < nDesertOf(desSp) + oakN)   // …+ oakN: the species' oak-forest population lives in the slots its desert count leaves spare, so admitting it is one term on the same test rather than a second band
        : (flamSlot ? (wk - FLAM_0 < nFlamingo)
        : (porcSlot ? (wk - PORC_0 < nPorcupine)
        : (skunkSlot ? (wk - SKUNK_0 < nSkunk)
        : (armSlot ? (wk - ARM_0 < nArmadillo)
        : (bunnySlot ? (wk - BUNNY_0 < nBunny)
        : (fishSlot ? (wk - FISH_0 < nFish)
        : (lilySlot ? (wk - CARD_0 < nCard)
        : (wormSlot ? (wk - WORM_0 < nWorm || (B.init && B.rel))
        : (isBaby ? (!!DUCKB_ITEM0 && (orphan ? B.init : (mom5.init && isFinite(mom5.x) && isFinite(mom5.z) && sib < (mom5.nBab || 0))))   // …and she must have a REAL position: a duckling is placed by arithmetic on hers, so a mother who is init without one hands the whole brood NaN — and a NaN body cannot be retired by any distance test, because every one of them is a comparison and NaN loses them all. Asked HERE, in `active`, rather than only at the placement: refusing there instead sends the brood down the generic 12-try water search every single frame, which it can never satisfy inland — MEASURED at 56 ms of life tick, a 25 fps world   // a duckling exists only while its mother does, up to her brood size — unless she was KILLED, in which case it carries on alone until it is killed too
        : (duckSlot ? (wk - DUCK_0 < nDuck) : (wk < nAct)))))))))));   // B.rel = a worm the player RELEASED with Q — lives beyond the population cap until it recycles  [three extra ) close the porcSlot + skunkSlot + armSlot branches]
      // ── A CREATURE THE PLAYER IS FIGHTING IS NOT SURPLUS (user 2026-08-20: "I was hitting an armadillo and it
      // dissapeared") ── `active` is the population controller's verdict, and it is a COUNT: when the target
      // drops, whichever slots fall outside it are retired on the spot. Two ordinary things drop it out from
      // under a fight — nMam goes to 0 the moment moonMode flips, so every land mammal is deleted at moonrise,
      // and the count also thins as the player walks and the biome fraction under the ring changes. Either way
      // the animal you are three hits into simply stops existing, with no death, no poof and no meat.
      // Six seconds of grace from the last landed hit. It is deliberately keyed on being HURT rather than on
      // distance: a creature the player has committed to is worth a slot whatever the target says, and one
      // that gets away clean is back to being surplus a few seconds later. It cannot leak a slot — hurtAt is
      // only ever set by a hit, and reapDeaths retires the animal on the killing blow long before this expires.
      const fighting = B.hurtAt !== undefined && (now - B.hurtAt) < 6000;
      // ── …AND NEITHER IS ONE YOU ARE LOOKING AT (user 2026-08-20: "the life is STILL dissapearing" — "a skunk
      // specifically at that particular moment") ── the grace above only covers a creature being HIT. The
      // ordinary case is worse and needs no fight at all: `active` is `slot index < target`, so the moment the
      // target falls the HIGHEST-NUMBERED bodies are retired, and which ones those are has nothing to do with
      // where they are standing. Measured walking in broad daylight, deep in the oak forest with the night
      // ramp nowhere near it: nMam stepped 14 -> 12 in a single frame — and nBunny, nArmadillo and nSkunk are
      // all nMam, so that one step deleted about six animals at once, in the open, with no death and no poof.
      // The cause is the count itself: it scales on bioFracAt, a 24-point ring sample, so walking flips one
      // sample across the treeline ~900 vox away and the whole population quantises down a notch.
      // The target is right; retiring an animal in the player's face to reach it is not. A surplus body is now
      // spared while it is NEAR, and goes the instant it is beyond SURP_NEAR — at which range a pop is a few
      // pixels on the horizon and nobody has ever reported one. SURP_GRACE bounds it so this can only ever
      // DELAY a retirement, never veto it: the night still empties on schedule, a few seconds late at worst,
      // and the population cannot drift above its target for longer than that. Costs one distance test on a
      // body the loop was about to discard anyway.
      const SURP_NEAR2 = 110 * 110, SURP_GRACE = 12000;
      if (active) B.surpT = undefined; else if (B.init && B.surpT === undefined) B.surpT = now;
      const spared = !active && B.init && (now - (B.surpT || now)) < SURP_GRACE
        && ((B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z)) < SURP_NEAR2;
      const surp9 = !active && !fighting && !spared;
      if (ED.on || dead || (flamSlot ? !FLAMINGO_ITEM0 : (porcSlot ? !PORCUPINE_WALK.length : (skunkSlot ? !SKUNK_WALK.length : (armSlot ? !ARMADILLO_ITEM0 : (bunnySlot ? !BUNNY_ITEM0 : (wantK === 6 ? !FISHES.length : (wantK === 5 ? !CARD_NFRAMES : (wantK === 2 ? !WORM_NFRAMES : (wantK === 3 ? !DUCK_ITEM0 : (wantK === 1 ? !FFLY_NFRAMES : !BFLY_COLS.length))))))))))) { if (B.sCells) unstampWorm(B); B.init = false; B.dieT = 0; continue; }   // an inactive/hidden grid-stamped creature (worm/duck/skunk/porcupine) must clear its stamp
      // ── AND A SURPLUS ANIMAL SHRINKS AWAY, IT DOES NOT VANISH (user 2026-08-26: "I just saw a bunny
      // dissapear") ── the population target is a moving number: bioFracAt is a 24-point ring sample, so
      // walking flips one sample across a treeline and the whole count quantises down a notch. The `spared`
      // rule above already stops that deleting an animal in your face, but only for SURP_GRACE, and only
      // inside SURP_NEAR — which is 110 voxels, and at 10 cm per voxel that is ELEVEN METRES, not the
      // horizon its note assumes. A bunny at 11 m is a whole bunny. So the surplus arm no longer clears
      // init at all: it starts the SAME 0.7 s shrink the dusk/dawn line below uses, and the timer there
      // finishes the job. Nothing else changes — the editor, death and a missing asset still take effect on
      // the frame they happen, because those are not the population drifting, they are the world stopping.
      // surpDie marks whose fade this is, so an animal the count hands back gets a reprieve while a
      // dusk/dawn fade (which is a kind CHANGE, not a surplus) is left alone to finish.
      if (surp9) { if (B.init && !B.dieT) { B.dieT = now; B.surpDie = 1; } }
      else if (B.surpDie) { B.surpDie = 0; B.dieT = 0; }
      const tb3 = now / 1000;
      if (B.init && (B.kind | 0) !== wantK && !B.dieT) B.dieT = now;   // dusk/dawn — the wrong creature shrinks away over 0.7 s, the right one fades in at a fresh spot
      // ── FLYER COMPOSITION DRIFT ── isDfly is read ONLY at spawn, so the butterfly/dragonfly split freezes into the band
      // the moment slots stop recycling. Constant mercy-recycle churn used to re-roll it by accident; now that the arbiter
      // keeps flyers alive indefinitely, a flock that filled before the lake census saw water stays 100% butterfly forever
      // (measured: 0 dragonfly-minutes over 240 s on all three seeds). Re-check against the LIVE split and retire the
      // mismatch through the SAME 0.7 s fade dusk/dawn uses — never a conversion in place, so a butterfly never pops into
      // a dragonfly mid-flight and the replacement re-places under the over-water rule. The 3 s dwell outlasts one 2.5 s
      // census tick, so a waterSpots count wobbling across a >>2 boundary cannot churn the band.
      if (wantK === 0 && B.init && !B.dieT) { if (!!B.dfly === isDfly) B.dfMis = 0; else if (!B.dfMis) B.dfMis = now; else if (now - B.dfMis > 3000) { B.dfMis = 0; B.dieT = now; } }
      if (B.dieT && now - B.dieT > 700) { B.dieT = 0; if (B.sN) unstampWorm(B); B.init = false; }   // …and the STAMP goes with it: deactivating a grid-stamped creature without unstamping leaves its voxels in W and its cells in stampedIdx, permanently exempt from every support test
      // ── AN EMPTY SLOT NOBODY WANTS IS NOT TICKED AT ALL (2026-08-27) ── everything below this line steers,
      // seats, stamps or recycles A CREATURE, and a slot that is both uninitialised and surplus holds none.
      // It still ran the whole body, and the expensive part is not the arithmetic: an empty slot has no kind,
      // so the kind chain falls through to the FLYER arm and calls navSteerAir — a 16-candidate fan, each
      // candidate a navReachAir DDA — on a creature that does not exist.
      // MEASURED, oak forest, same 421 live birds either way: the arbiter ran 136 fan ticks per frame with no
      // idle card slots and 597 with 421 of them, i.e. ~1.05 wasted fans per idle slot per frame, and
      // navSteerAir + navFitsAir were 55% of the whole profile. Frame 6.9 -> 14.5 ms, 146 -> 65 fps.
      // Confirmed against a third point rather than assumed: the birch wood, where most of the pool IS wanted,
      // sat in between at 210 idle slots and 374 ticks. The count of idle slots predicts it, not the biome.
      // This is PRE-EXISTING — it is what an empty desert already did to all 421 card slots, and what the
      // treeFrac note above is really describing — but the birch multiplier makes a standing surplus the
      // normal state of every non-birch wood, so it stops being a corner and starts being the common path.
      // `!B.dieT` keeps a creature that is mid-shrink: the 0.7 s fade above still has to finish and clear init.
      if (surp9 && !B.init && !B.dieT) continue;
      // Perched birds recycle at CARD_KEEP, and the CARD_N pool is sized in slots.js so that it runs out at about that same radius.
      // Because the frontier sits far beyond anything you can pick out, a bird activating there is invisible — which
      // is why this no longer needs the "only spawn behind the player" rule the random placement depended on.
      const rdK = wantK === 5 ? CARD_KEEP : ((bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot) ? MAM_KEEP : LIFE_KEEP);   // land mammals persist to the bird keep radius (user: "spawn as far as the perched birds"); worms/ducks/fish keep LIFE_KEEP
      const dp2 = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z);
      const hd2 = ((B.kind | 0) === 0 || (B.kind | 0) === 2 || (B.kind | 0) === 6) && B.hcx !== undefined ? (B.hx - P.x) * (B.hx - P.x) + (B.hz - P.z) * (B.hz - P.z) : dp2;
      // ── AND ANYTHING THAT WANDERS ONTO THE ICE IS RECYCLED (user 2026-08-29, third time of asking) ── the
      // population damping in tick-life zeroes every band's WANT while the player is in the arctic, and that
      // is genuinely working: censused 0 wanted, 0 in every band, 0 birds, 0 drops. What it cannot do is stop
      // a creature that spawned in the pine forest from WALKING IN, because want governs placement near the
      // player, not migration afterwards. That is what was still being seen: a handful of transient stamped
      // voxels, gone by the next scan because the animal had moved on.
      // Recycled on the SAME 0.5 midline the bee's own containment uses above, and for the same stated reason
      // — not the 0.15 planting line, because clipping an animal off where the trees stop would read as an
      // invisible wall well before the snow starts.
      const far = hd2 > rdK * rdK || arcticM(B.x, B.z) > 0.5;   // butterflies (and fish) judge by their HOME, everything else by where it is
      // ── AND AN OAK-FOREST FLYER IS RECYCLED ONCE IT IS PROPERLY OUT OF THE OAK (user 2026-08-17) ── every
      // other biome containment in this loop is `(B.kind|0) === 2`: the step rule, the turn-away and the
      // planner's reach clip are all a WALKER's. A flyer has none of them, and the bee is the first creature
      // that is both airborne and tied to one biome, so without this it would spawn in deep oak and then
      // wander the pine forest at 56 vox/s with nothing to stop it — it has no home cell of its own from the
      // annulus branch, so not even the butterfly leash applied (it does now; see the hcx write at the spawn).
      // TWO THRESHOLDS, WHICH IS THE SHAPE sim/life/birds.js ARRIVED AT for exactly this question and for
      // exactly this reason: a bird 'is the one creature that SHOULD be able to cross a treeline — clipping
      // it off at the halfway line would read as an invisible wall in open sky'. So the bee is ADMITTED only
      // in deep oak (the spawn gate's oakM > BIO_DESERT, 0.85) and RECYCLED only once it is past the midline
      // (bioHomeOK's own BIO_OAKLINE, 0.5) — BIRD_IN/BIRD_OUT with the numbers this band already had. It is
      // bioHomeOK itself rather than a second mask test, so there is still exactly one answer in the game to
      // 'is this creature at home here'. Scoped to the oak FLYER: a walker is already contained, and a
      // walker that HAS been stranded is deliberately left free to walk home instead of being deleted.
      const beeOut = oakSlot && desFly && B.init && !bioHomeOK(bioMe, B.x, B.z, desBee);   // desBee is the SAME term `cherryLife` admits the bee on, passed to the containment test so the two halves cannot disagree — when they did, every blossom bee was recycled every frame
      // ── …AND A SWARM BEE FOLLOWS THE HIVES (user 2026-08-17) ── placing the five swarm slots at the
      // player's nearest hive is only half an answer on its own: FLY_LEASH pins a bee within 84 voxels of
      // wherever it was born and `far` does not fire until LIFE_KEEP (1040), so walking 600 voxels to the
      // next hive would leave the whole swarm behind at the old one and the new hive standing empty — the
      // exact thing the report is about, one hive along. So a swarm bee whose hive is no longer the
      // player's recycles, and the placement below puts it at the new one. Same shape as beeOut above: a
      // containment test on the ONE thing this slot is for, judged against the very query the placement
      // reads, so the two can never disagree about where a swarm belongs.
      const beeHome9 = (oakSlot && desBee && desIx < BEE_HIVE_N) ? beeHomeHive(P.x, P.z, tb3, LIFE_OUT) : null;
      const beeStray = !!beeHome9 && B.init && (B.x - beeHome9.wx) * (B.x - beeHome9.wx) + (B.z - beeHome9.wz) * (B.z - beeHome9.wz) > BEE_STRAY_R * BEE_STRAY_R;
      // ── THE NO-PROGRESS WATCHDOG (user 2026-08-19: "snake getting stuck on trees ... make sure that no life
      // gets stuck on objects endlessly") ── B.trap, which every escape below is written against, counts only
      // steps the mover REFUSES. That misses the failure the user is describing: a creature that oscillates
      // against a trunk has its step ACCEPTED each frame and then bounced back, so it travels nowhere while
      // trap stays at zero and nothing ever fires. MEASURED over ~40 s in the oak forest, max trap by band:
      // flyer 0.03, bunny 0.05, skunk 0.13, armadillo 0.24, DESERT (the snake's band) 0.26 — none of them
      // within an order of magnitude of the 12 s escape, which is exactly what a stall that trap cannot see
      // looks like. So this watches DISPLACEMENT instead, which cannot be fooled by motion that goes nowhere.
      // GENTLE FIRST, and that ordering is the whole design: one stalled window re-seats the heading and lifts
      // trap just past the 0.35/0.5 thresholds the existing steering escapes already watch for, so the creature
      // gets to solve it with the machinery that is already there. Only after WD_GIVE consecutive stalled
      // windows does it hand over to the recycle below, and 3 x 4 s lands on the same 12 s the trap escape uses.
      // NOT WORMS (wantK 2) AND NOT PERCHED BIRDS (wantK 5): a worm legitimately wanders a 3-4 voxel patch for
      // ten seconds at a time (measured: 24.8 voxels of path inside a 3.4 voxel box) and a cardinal is supposed
      // to sit still, so both would be permanent false positives.
      if (B.init && wantK !== 5 && wantK !== 2) {
        if (B.wdT === undefined) { B.wdT = tb3; B.wdX = B.x; B.wdZ = B.z; B.wdN = 0; }
        else if (tb3 - B.wdT > WD_WIN) {
          const wdx = B.x - B.wdX, wdz = B.z - B.wdZ;
          if (wdx * wdx + wdz * wdz < WD_MIN * WD_MIN) {
            B.wdN = (B.wdN || 0) + 1;
            B.om = 0; B.omT = 0; B.tRe = 0;            // drop any pending turn, then face somewhere new outright
            B.h = (Math.random() * 4) | 0; B.ah = B.h; B.th = Math.random() * 6.2831853;
            B.trap = Math.max(B.trap || 0, 1.0);       // …and above the 0.35/0.5 the steering escapes watch
            if (B.wdN >= WD_GIVE) B.trap = 99;         // still nowhere after WD_GIVE windows: let the recycle take it
          } else B.wdN = 0;
          B.wdT = tb3; B.wdX = B.x; B.wdZ = B.z;
        }
      }
      // ── A SLOT THE POPULATION CONTROLLER DOES NOT WANT IS NEVER RE-FILLED (2026-08-27) ── this block asks
      // only "does this slot need a spot", never "is this slot supposed to exist", so a SURPLUS slot went
      // round in a permanent loop: surp9 starts the 0.7 s shrink, the timer clears B.init, `!B.init` sends it
      // straight back in here, findPineCrown hands it a fresh perch, and surp9 marks it surplus again. It
      // could not drain and it could not stop. MEASURED after walking a birch wood into a pine one: 318
      // surplus songbirds still alive 90 s later, each carrying a dieT only ~400 ms old — not one population
      // decaying but 318 slots re-placing themselves twice a second, findPineCrown and all.
      // It was invisible before because no band ever held a large STANDING surplus in ordinary country: every
      // wood asked for the whole songbird pool, so `wk - CARD_0 < nCard` was true for the entire band, and the
      // desert case (nCard 0) is the one place it could bite. The birch multiplier makes a surplus the normal
      // state of every non-birch wood, which is what surfaced it.
      // surp9, not !active, so the two reprieves above still hold: a creature being HIT and one standing near
      // enough to see are both re-placed exactly as before. An orphan duckling is handled by the same test —
      // its `active` IS B.init, meaning an existing orphan lives and a new one is never made, which is what
      // skipping the placement gives it.
      if (!surp9 && (!B.init || far || beeOut || beeStray || (B.trap > 12 && (B.kind < 3 || B.kind === 6 || !bfWater(B.x, B.z))) ||   // trap > 12 s = escape truly failed — mercy recycle; a duck/lily wedged ON real water NEVER teleports (the unstick frees it), but a FISH sealed in a rock pocket the escape can't solve does (respawns in open water)
          B.x <= rect.xlo + 4 || B.x >= rect.xhi - 4 || B.z <= rect.zlo + 4 || B.z >= rect.zhi - 4 ||   // the window recentred/shrank under it — this ground is stale garbage, leave it
          (isBaby && !orphan && (B.x - mom5.x) * (B.x - mom5.x) + (B.z - mom5.z) * (B.z - mom5.z) > 40 * 40) ||   // a duckling stranded from its (recycled) mother rejoins her — an ORPHAN has nowhere to rejoin, and must not be teleported to where its mother died
          ((B.kind === 3 || B.kind === 4 || B.kind === 6) && !isBaby && ((frame + wk) & 63) === 0 && !bfWater(B.x, B.z)) ||   // recycle ONLY a DUCK/LILY/FISH genuinely OFF real water (dry ravine/land after a recenter) — NOT a perched cardinal (kind 5, never on water) and NOT one merely near a shore
          (B.trap > 4 && (dp2 > 0.7 * 0.7 * rdK * rdK || (B.kind < 3 && bfObst(B.x, B.y, B.z)))))) {   // stuck 4 s: recycle if far in the fog OR body-in-solid (kinds <3 only — a duck's body dips into the WATER voxel in wave troughs, which is not 'stuck')
        if (B.rel) B.rel = false;                      // a released worm that leaves/loses its spot rejoins the normal population rules
        let placed = false;
        if (wantK === 5) {                             // PERCHED CARDINAL: find a nearby pine crown to sit on (its own search — not the generic open-spot one)
          const pc = findPineCrown(wk);
          if (pc) { B.tx = pc.tx; B.tz = pc.tz; B.bi = pc.bi; B.x = pc.x; B.z = pc.z; B.perchFeet = pc.y + 1; placed = true; }   // feet rest ON TOP of the crown needle
        }
        // …and she must HAVE a position to hatch behind: every line below is arithmetic on mom5.x/th, so a
        // mother who has not been placed (or has just been retired) hands the whole brood NaN and the guard at
        // the top of the loop is then the only thing that can clear them. Falling through leaves the duckling
        // to the generic open-water spot, which is exactly what an orphan already does.
        if (isBaby && !orphan && isFinite(mom5.x) && isFinite(mom5.z) && isFinite(mom5.th)) {                       // ducklings hatch right behind their mother, in line order (an orphan has none — it takes the generic open-water spot below, like a lone adult)
          let hx6 = mom5.x - Math.sin(mom5.th) * (4 + sib * 3.5) + (Math.random() - 0.5) * 2;
          let hz6 = mom5.z - Math.cos(mom5.th) * (4 + sib * 3.5) + (Math.random() - 0.5) * 2;
          if (!bfWater(hx6, hz6) || !bfSky(hx6, WL + 2, hz6)) { hx6 = mom5.x; hz6 = mom5.z; }   // the trail spot is ashore/dry-ravine/inside the bank (mom hugging the shore) — hatch AT mom instead; she's on valid water by construction
          B.x = hx6; B.z = hz6;
          B.y = WL + 2.5; placed = true;
        }
        const tooClose = (j0, j1, minD) => {           // EVEN DISTRIBUTION — reject spawns bunched against a live same-kind neighbour
          for (let j = j0; j < j1; j++) { const O = wbf[j];
            if (O !== B && O.init && (O.x - sx7) * (O.x - sx7) + (O.z - sz7) * (O.z - sz7) < minD * minD) return true; }
          return false; };
        let sx7 = 0, sz7 = 0, hcx, hcz, mateJoin = false;   // mateJoin: this try is placing a flamingo BESIDE its partner, which is the one case the land-mammal spacing floor must not veto
        // ── fspWant IS DECLARED OUT HERE, NOT IN THE RETRY LOOP (user 2026-08-18: the game froze/crashed) ──
        // it was `const fspWant` INSIDE the loop below and read after the loop closed, so the moment any fish
        // slot placed, tickBody threw ReferenceError. tick() CATCHES that, which is why every gate said the game
        // was fine: no uncaught exception for the harness to see, lint clean, and a screenshot of a perfectly
        // rendered forest — on an instance whose loop was dead. The frame aborts before rendering, so the canvas
        // holds its last good picture and nothing advances; past 3600 caught errors tick.js stops requeuing rAF
        // and it is unrecoverable. It read as "intermittent" only because nFish is 0 with no water in the ring:
        // away from water the game is perfect, and walking to a lake kills it instantly.
        let fspWant = -1;                                // which species this fish will be; -1 for anything that is not a fish
        for (let tries = 0; tries < 12 && !placed && wantK !== 5; tries++) {
          mateJoin = false;                              // per TRY, not per slot: a rejected mate placement must not exempt the next try's ordinary one
          let sx, sz, beeAtHive = false;               // …set by the hive placement below and read by the desert band's spacing floors: a SWARM is the one thing in that band that is meant to be a cluster
          if (wantK === 3 && lakeSpots.length) {       // MOTHER DUCKS TARGET LAKES: an EMPTY lake first, then one still under the cap — never a lake already holding DUCK_PER_LAKE families (user 2026-08-27: "no more then 2 duck families can spawn in a lake at a time")
            // The old rule was one family per lake with a FALLBACK to the whole spot list, and the fallback is
            // what put a third and a fourth family on one lake: once every lake held a mom, `free` emptied and
            // each remaining mom was handed lakeSpots with nothing counting how many were already there.
            // Counting instead of filtering gives both halves at once — the preference for an empty lake AND a
            // real ceiling — so a crowded lake now leaves a mom unplaced rather than taking her.
            // ── AND THE COUNT IS PER LAKE, NOT PER SPOT ── main/tick-life.js clusters its water samples around
            // a DRIFTING centroid at 260, so a lake wider than that comes back as two or three spots. That is
            // the same split that put four families on one lake when the merge was 110, and a per-spot cap
            // would let it straight back in on a big reservoir. So the spots are unioned into water BODIES
            // first (anything within DUCK_LAKE_LINK chains, transitively) and the cap is charged to the body.
            // Consequence worth knowing before it reads as a bug: on a lake that spans two spots the want
            // (lakes + 1) asks for three families and this grants two, so lifeWhy shows duckMom 3/4 there. The
            // surplus mom is deliberate — it is what puts a SECOND family on a single-lake scene — and the
            // break below makes her cheap: one pass over a handful of spots, not twelve retries.
            const par7 = [];                             // union-find over the census spots; tiny (a handful) and only ever walked while a duck slot is actually looking for a home
            for (let i7 = 0; i7 < lakeSpots.length; i7++) par7.push(i7);
            const find7 = (a7) => { while (par7[a7] !== a7) { par7[a7] = par7[par7[a7]]; a7 = par7[a7]; } return a7; };
            for (let i7 = 0; i7 < lakeSpots.length; i7++) for (let k7 = i7 + 1; k7 < lakeSpots.length; k7++) {
              const A7 = lakeSpots[i7], C7 = lakeSpots[k7];
              if ((A7.x - C7.x) * (A7.x - C7.x) + (A7.z - C7.z) * (A7.z - C7.z) < DUCK_LAKE_LINK * DUCK_LAKE_LINK) {
                const ra7 = find7(i7), rb7 = find7(k7); if (ra7 !== rb7) par7[rb7] = ra7; } }
            const occ7 = new Int32Array(lakeSpots.length);   // families per body, indexed by its ROOT spot
            for (let m = DUCK_0; m < DUCK_END; m++) { const O = wbf[m]; if (!(O && O !== B && O.init)) continue;
              let best7 = -1, bd7 = DUCK_LAKE_LINK * DUCK_LAKE_LINK;   // she belongs to the NEAREST spot, not to every spot in range: two small lakes side by side must not both count her
              for (let i7 = 0; i7 < lakeSpots.length; i7++) { const L = lakeSpots[i7];
                const d7 = (O.x - L.x) * (O.x - L.x) + (O.z - L.z) * (O.z - L.z); if (d7 < bd7) { bd7 = d7; best7 = i7; } }
              if (best7 >= 0) occ7[find7(best7)]++; }
            const free = [], room = [];
            for (let i7 = 0; i7 < lakeSpots.length; i7++) { const n7 = occ7[find7(i7)];
              if (n7 === 0) free.push(lakeSpots[i7]); else if (n7 < DUCK_PER_LAKE) room.push(lakeSpots[i7]); }
            const pool7 = free.length ? free : room;
            if (!pool7.length) break;                    // every lake in view is at the cap — BREAK, not continue: all 12 retries would ask the same question of the same spots and get the same answer, and the fish branch below already treats "nowhere to put it" this way
            const L7 = pool7[tries % pool7.length];
            sx = L7.ax + (Math.random() - 0.5) * 18; sz = L7.az + (Math.random() - 0.5) * 18;
          } else if (desSlot && DESERTS[desSp] && DES_WATER[DESERTS[desSp].name]) {
            // ── A WATERSIDE SPECIES (user 2026-08-22: the frog "around water, similar to the dragonflies in
            // that way") ── the same waterSpots home the dragonfly takes, walked by `tries` so one pool cannot
            // monopolise every slot. The difference is the RADIUS and what happens at the edge: a dragonfly is
            // placed OVER the water and rejected if it drifts ashore, and a frog is the exact opposite — it
            // wants the bank, so it is placed in a ring around the spot and the wet ones are rejected below.
            if (!waterSpots.length) continue;          // no water in the window → no frog, the same deal the dragonfly gets. Falling through to the inland annulus would put frogs in dry forest, which is the thing being asked against
            const rW = DES_WATER[DESERTS[desSp].name];
            const L6 = waterSpots[(wk + tries) % waterSpots.length];
            const aW = Math.random() * 6.2831853, rr6 = rW * (0.45 + 0.55 * Math.sqrt(Math.random()));
            sx = L6.x + Math.cos(aW) * rr6; sz = L6.z + Math.sin(aW) * rr6;
          } else if (wantK === 0 && isDfly) {          // DRAGONFLY: same kind-0 creature, but its home is WATER (user: only over lakes and rivers).
            const L8 = waterSpots[(wk + tries) % waterSpots.length];   // walk the spots as tries advance so one crowded pool can't monopolise every slot
            sx = L8.x + (Math.random() - 0.5) * 30; sz = L8.z + (Math.random() - 0.5) * 30;
            hcx = Math.floor(sx / FLY_CELL); hcz = Math.floor(sz / FLY_CELL);   // give it a home cell so the butterfly LEASH applies and it stays over its water
          } else if (wantK === 6) {                    // FISH: home is a water spot (lakes AND rivers), body UNDER the surface. Capacity SCALES with the pool's SIZE so a small pond isn't crammed (user)
            if (!waterSpots.length) break;             // the census can empty waterSpots THIS frame, after nFish was computed from last frame's — a `% 0` here made `L9.x` throw (this was a permanent game-freeze before the tick wrapper)
            let L9 = null;
            for (let k = 0; k < waterSpots.length; k++) { const cand = waterSpots[(wk + tries + k) % waterSpots.length];   // walk the spots so a full pool yields to a hungrier one
              const cap = Math.max(1, Math.min(16, Math.round(cand.n / 3)));   // UNIFORM density: ~1 fish per 3 census samples (~1200 vox² of water). The old 1-per-sample slope CRAMMED ponds (a 9-sample pond held 9 fish) while big-lake spots were clamped at 16 regardless of area — density inverted (user)
              let near = 0; for (let f = FISH_0; f < FISH_END; f++) { const F = wbf[f]; if (F && F.init && (F.kind | 0) === 6 && F.hx !== undefined && (F.hx - cand.x) * (F.hx - cand.x) + (F.hz - cand.z) * (F.hz - cand.z) < 45 * 45) near++; }   // count by HOME, not wander position — spots are pairwise >90 apart so 45-vox home-discs are DISJOINT; the old 90-vox position count tallied one lake fish against ALL its spots and starved big lakes (user)
              if (near < cap) { L9 = cand; break; } }
            if (!L9) break;                            // every pool already holds its size-capped share → add no more fish
            const r9 = 45 * Math.sqrt(Math.random()), a9 = Math.random() * 6.2831853;   // a DISC of the near-test's own radius, not a box: sqrt() makes it uniform by AREA (a raw radius crowds the middle), and staying inside 45 keeps the fish in the home-disc the cap counted it against — a ±45 SQUARE would reach 63 into the neighbouring spot's and corrupt both counts
            sx = L9.x + Math.cos(a9) * r9; sz = L9.z + Math.sin(a9) * r9;   // off-water landings are rejected by the bfWater gate below and simply retried, so a ragged shoreline costs retries, not fish
            hcx = Math.floor(sx / FLY_CELL); hcz = Math.floor(sz / FLY_CELL);   // a home cell → the leash keeps it in its pool, the recycle judges by home like a butterfly's
          } else if (!desSlot && (wantK === 0 || wantK === 2)) {   // desSlot deliberately falls through to the ANNULUS below: the home-finders (findWormHome/findSkunkHome/…) pick fro
            // ── A REFUSED HOME IS NOT WORTH RE-ASKING SIXTY TIMES A SECOND ── the finder below tries up to 12
            // cells internally, and the LIFE_IN floor added under it rejects whatever it returns when the only
            // free cells are the ones near the player. Without this that whole search ran every frame for every
            // slot that could not be filled: MEASURED at +9% frame average and +43% p99 before the cooldown.
            // 250 ms is four attempts a second, which fills a slot the moment you walk far enough for a legal
            // cell to exist and is invisible against a spawn ring 800 voxels out.
            // …from procedural grids that do no biome test, so a desert creature routed through one is placed in the forest and then rejected by the gate on every retry — it would simply never spawn. BUTTERFLY / WORM: take a procedural home rather than a random point on the ring.
            if (B.homeCd && now < B.homeCd) continue;
            const h = wantK === 0 ? findFlyHome(wk) : (flamSlot ? findFlamHome() : (porcSlot ? findPorcHome() : (skunkSlot ? findSkunkHome() : (armSlot ? findArmHome() : (bunnySlot ? findBunnyHome() : findWormHome(wk)))))); if (!h) break;
            // ── AND NOT WHERE YOU CAN WATCH IT ARRIVE (user 2026-08-26: "bees for examaple just pop into view")
            // ── the ANNULUS below floors every other spawn at LIFE_IN, but the home-finders never looked at
            // the player at all: they pick a free cell off their own grid, so a bunny's home could be the cell
            // you are standing next to. MEASURED over 880 frames of walking: walkers arriving as close as 76
            // voxels, 12 of them inside 200. Same floor the annulus uses, so there is one answer in the game
            // to "how close may life appear", and the same `continue` the trunk test below uses — the finder
            // is asked up to 12 times a frame and most tries are rejected downstream anyway, so a refusal
            // costs a retry and nothing else. Life already placed nearby is untouched; this is only ARRIVAL.
            if ((h.x - P.x) * (h.x - P.x) + (h.z - P.z) * (h.z - P.z) < LIFE_IN * LIFE_IN) { B.homeCd = now + 250; continue; }
          if (wantK === 2) {                           // a home inside a trunk is unreachable, and the leash below would grind the worm against it forever
            const tc = treeAt(Math.floor(h.x / TCELL), Math.floor(h.z / TCELL));
            if (tc && (tc.tx - h.x) * (tc.tx - h.x) + (tc.tz - h.z) * (tc.tz - h.z) < 14 * 14) continue;
          }
            sx = h.x; sz = h.z; hcx = h.cx; hcz = h.cz;
          // ── FLAMINGOS STAND AS A COUPLE (user 2026-08-18: "can you spawn flamingos as a couple") ── decided
          // HERE and not in the home finder, on the LIVE census: the finder is asked for a home up to 12 times
          // per slot per frame and most of those tries are rejected downstream, so a counter kept there counts
          // attempts rather than birds, and a cell retired by two rejected tries takes its partner slot with it.
          // A bird whose cell holds fewer than FLAM_PAIR is looking for a partner; this one goes beside it and
          // ADOPTS ITS CELL, so the census then reads the cell as full and the next bird opens a new one.
          if (flamSlot) {
            const cnt = new Map();
            for (let j2 = FLAM_0; j2 < FLAM_END; j2++) { const O = wbf[j2];
              if (!O || O === B || !O.init || O.hcx === undefined) continue;
              const kF = O.hcx + ',' + O.hcz; cnt.set(kF, (cnt.get(kF) || 0) + 1); }
            let lone = null;
            for (let j2 = FLAM_0; j2 < FLAM_END; j2++) { const O = wbf[j2];
              if (!O || O === B || !O.init || O.hcx === undefined || O.hx === undefined) continue;
              if ((cnt.get(O.hcx + ',' + O.hcz) || 0) < FLAM_PAIR) { lone = O; break; } }
            if (lone) {                                // ihash on the CELL, not Math.random: a couple whose spacing re-rolled on every respawn would not be the same couple
              const aM = ihash(lone.hcx * 311 + 13, lone.hcz * 313 + 17) * 6.2831853;
              const rM = FLAM_MATE_LO + ihash(lone.hcx * 317 + 19, lone.hcz * 331 + 23) * (FLAM_MATE_HI - FLAM_MATE_LO);
              const mx = lone.hx + Math.cos(aM) * rM, mz = lone.hz + Math.sin(aM) * rM;
              if (mx > rect.xlo + 8 && mx < rect.xhi - 8 && mz > rect.zlo + 8 && mz < rect.zhi - 8) {
                sx = mx; sz = mz; hcx = lone.hcx; hcz = lone.hcz; mateJoin = true;
              }
            }
          }
          } else {
            const a5 = Math.random() * 6.283;
            // ── THE DESERT BAND GETS A MUCH WIDER RING (user 2026-08-16: life "all seem to spawn near each
            // other, then vast areas where nothing spawns") ── measured before changing anything: with the
            // shared LIFE_IN (0.78 x LIFE_KEEP ~ 811) the ring is only ~31% of the disc it sits in and NOTHING
            // is ever placed inside 811, so 61% of bodies sat beyond 770 voxels and the ground within 110 of
            // the player held 1.3% of them. That reads exactly as a distant band with empty desert in front.
            // Halving the inner radius quadruples the usable area and lets the same 31 bodies cover it.
            // Pop-in is the reason the floor exists, and it is why this is scoped to the DESERT band alone:
            // these are 1-3 voxel animals, sub-pixel at 400+ voxels, where a bunny or a duck is not.
            // ── AND NOT THE LAND MAMMALS ── a `mamB` arm sat here giving bunnies/armadillos/skunks/porcupines
            // a 0.55 inner floor, on the same argument. It was INERT: every one of the four is kind 2 with
            // desSlot false, so the branch above routes all of them into findBunnyHome/findArmHome/
            // findSkunkHome/findPorcHome and nothing in the band can ever reach this draw. Measured before
            // removing it — over six boots the mammals' distance distribution (p25 323 / median 437 / p75 543
            // / max 638) is a dead match for an area-uniform disc of radius MAM_OUT = 639, which is what the
            // home finders produce and is nothing like this annulus. Their spread is decided by the scatter
            // key in those finders, so that is where the even-spread work belongs and where it now is.
            // ── A 3-VOXEL BEE ON THE HORIZON IS NOT A BEE (user 2026-08-17: "I dont see bees at all") ──
            // MEASURED before this: 8 bees alive, om 1, home true — correct in every way except that they
            // were 750 and 1026 voxels out. The desert annulus starts at 0.40 * LIFE_KEEP = 416 and runs to
            // 978, which is right for a 4 m saguaro's scorpion and useless for something 3 voxels across:
            // at 75 m a bee is a sub-pixel speck, so the population was entirely real and entirely unseen.
            // The FLYING SONGBIRDS already solved exactly this — birds.js rides a deliberately tighter ring
            // (0.24..0.50 of keep) so they arrive well inside the view and cross it, with a comment saying
            // the wide ring 'puts a flyer on the horizon where it is a single pixel'. Same reasoning, same
            // numbers, and the bee is smaller than a bird so it takes the tighter end.
            // It is gated on the OAK flyers alone: the desert species keep their annulus exactly, so the
            // sand is untouched. FLY_LEASH (84) then holds a bee near wherever it arrived.
            // ── THE BEE'S RING IS THE BEE'S (user 2026-08-22: "when the player spawns, all the life clusters on
            // spawn ... this has been a very persistent problem. The life needs to be evenly spread out") ──
            // this read desFly, which is the DES_FLYER table, so it covered the fly as well as the bee and then
            // the ladybug the moment that was added to fly. The ring is TIGHT on purpose — LIFE_KEEP * 0.10 to
            // 0.26 — because it exists to put bees at the hive the player is standing under (beeHomeHive just
            // below). Every other oak flyer inherited it and spawned in a 104-270 disc around the player while
            // every other band used 416-978. MEASURED at spawn: the desert band's median distance was 162 with
            // a max of 264, against 679-881 medians and ~1000 maxima for every other band. desBee is
            // name-exact, so the bee keeps its hive ring and nothing else borrows it.
            const beeRing = desBee && oakSlot;
            // The bee band moved OUT, 0.10-0.26 -> 0.26-0.42 (user 2026-08-26: "bees for examaple just pop into
            // view"). This is the fallback the ring takes when no hive is in reach, and at 0.10 it started 104
            // voxels away — ten metres, measured arriving as close as 181. It stays a TIGHT band on purpose,
            // because that is what keeps a swarm regional rather than scattered across the whole disc; it is
            // simply a band you cannot watch fill. The hive path above has its own, closer floor for the same
            // reason in reverse: there the point is to fill the hive you are walking up to.
            const inR = beeRing ? LIFE_KEEP * 0.26 : (desSlot ? Math.min(LIFE_IN, LIFE_KEEP * 0.40) : LIFE_IN);
            const outR = beeRing ? LIFE_KEEP * 0.42 : LIFE_OUT;
            const d5 = Math.sqrt(inR * inR + Math.random() * (outR * outR - inR * inR));   // worms, flyers and fallback ducks all share the one AREA-uniform annulus now (was three different inner floors: 40 / 50 / 50)
            sx = P.x + Math.sin(a5) * d5; sz = P.z + Math.cos(a5) * d5;
            // ── A SWARMER IS BORN AT ITS HIVE (user 2026-08-17: "it doesnt look like the bees are swarming
            // the hive") ── the swarm behaviour was finished and the query was wired, and it still never
            // fired, because the two were never introduced. beeHiveNear searches a 3x3 OAK-CELL walk around
            // the BEE (about +-79 voxels) while hives sit roughly 690 apart, and a bee is placed at a random
            // bearing from the PLAYER — so the odds of one landing inside its own search disc are a few
            // percent, and every bee simply found flowers instead. Waiting for a bee to wander onto a hive
            // does not work either: FLY_LEASH pins it within 84 voxels of wherever it spawned.
            // So the hive is chosen FIRST and the bee is placed on it. Same trick the ducks use with the
            // lake census - a creature that needs a rare feature is spawned AT the feature, not dropped at
            // random in the hope of finding one.
            // ── …AND THE SWARM SLOTS ARE PLACED AT THE PLAYER'S OWN HIVE (user 2026-08-17: "make the bees
            // swarm around the beehive, still dont see them doing this") ── this searched the SPAWN RING and
            // picked a hive out of it at random, which is why it almost never fired and why, on the rare
            // boot where it did, it sent one bee each to five different hives. beeHomeHive (sim/life/
            // slots.js, where the measurement behind this is recorded) answers the question that actually
            // matters — which hive is nearest the PLAYER, out to the whole spawn disc — so all five swarm
            // slots land at one hive and it is the hive you are standing under. The other three keep the
            // ring and forage, so a hiveless stretch of forest still has bees in it.
            // Placed just OFF the hive, not on it: inside BEE_ORBIT_R the approach state has nothing to do
            // and the bee would pop straight to orbiting. A few voxels out and it flies in, which is the
            // part worth watching — and it is a little further out than it was (14-32 against 10-24) now
            // that the player is routinely standing at the hive this places around.
            if (beeRing && desIx < BEE_HIVE_N) { const hv9 = beeHomeHive(P.x, P.z, tb3, LIFE_OUT);
              // FARTHEST POINT ON THE RING, NOT A RANDOM ONE. The ring is only 14-32 across, so which side of
              // the hive the bee lands on is the whole difference between appearing behind it and appearing in
              // front of you. Take the best of BEE_POP_TRIES and refuse the placement outright if even that is
              // inside BEE_POP_MIN — the slot simply waits, and the hive fills the moment you are not on top
              // of it. Refusing costs nothing: this runs every tick, and an empty hive you are standing in is
              // less wrong than a bee blinking into existence at arm's length.
              if (hv9) { let bd9 = -1, bx9 = 0, bz9 = 0;
                for (let t9 = 0; t9 < BEE_POP_TRIES; t9++) {
                  const ja = Math.random() * 6.2832, jr = 14 + Math.random() * 18;
                  const cx9 = hv9.wx + Math.cos(ja) * jr, cz9 = hv9.wz + Math.sin(ja) * jr;
                  const dq9 = (cx9 - P.x) * (cx9 - P.x) + (cz9 - P.z) * (cz9 - P.z);
                  if (dq9 > bd9) { bd9 = dq9; bx9 = cx9; bz9 = cz9; }
                }
                if (bd9 < BEE_POP_MIN * BEE_POP_MIN) continue;
                sx = bx9; sz = bz9; beeAtHive = true; }
            }
          }
          const antHeel = desSlot && DESERTS[desSp] && DESERTS[desSp].name === 'ant' && ((wk - MAM_END) % DES_PER) > 0;
          if (antHeel) {
            const ld = wbf[wk - 1];                    // hatch behind the ant ahead, in line order — the duckling spawn, minus the water tests
            if (!ld || !ld.init) continue;             // …and NEVER anywhere else. Falling through to the annulus put followers 130-600 voxels from
            sx = ld.x - Math.sin(ld.th) * 3.2 + (Math.random() - 0.5) * 1.2;   // their leader, and at 16 vox/s they never closed it before being recycled.
            sz = ld.z - Math.cos(ld.th) * 3.2 + (Math.random() - 0.5) * 1.2;   // Waiting a frame for the leader is free — the retry loop comes straight back.
          }
          // ── BIOME GATE, BOTH WAYS, AND NOW BOTH ENDS ARE ADMIT TESTS ── placed on the ONE annulus every
          // land, flying and worm spawn funnels through, after both the home-finder and the fallback branch have
          // chosen a point, so no species can route around it. The desert band has always admitted at 0.85 rather
          // than 0.5 so none of its creatures stands in the dithered treeline. The forest was the plain INVERSE
          // of that test, which admitted it anywhere under 0.85 - through the entire blend band and right up
          // against the sand (user 2026-08-17: "the pine forest life are spawning too far out in the transition
          // between the desert and the pine forest"). It gets its own admit end now, so the treeline reads as a
          // border rather than as a mixing zone.
          // ── …AND THE OAK BORDER GETS THE SAME PAIR OF ADMIT ENDS (user 2026-08-17) ── OAKB is 450 like DESB,
          // so the identical 0.15/0.85 buys the identical ~20 m of empty treeline between the oak forest's
          // own population and the pine forest's. Two species read it, in opposite directions: the desert
          // MOUSE, which now also lives in the oak forest and must be admitted only in DEEP oak, and the
          // PORCUPINE, which the user removed from the oak forest and must therefore be admitted only OUTSIDE
          // it — the plain 'not sand' test every other forest species still uses would have let it straight in.
          // The oak test is second and reached only by those two, so no other band pays an oakM sample.
          const dmS = desertM(sx, sz);
          if (bioMe === BIO_SAND ? dmS < BIO_DESERT : dmS > BIO_FOREST) continue;   // an OAK mouse takes the forest end of this, like every other forest creature: it is 1080+ voxels of pine away from the sand and must never be admitted near it
          if (bioMe === BIO_BIRCH && birchM(sx, sz) < BIO_DESERT) continue;   // the birch band's own admit line, at the same 0.85 every closed-forest home uses
          if (bioMe === BIO_OAKF || bioMe === BIO_PINEF) {   // …and BOTH broadleaf bands count as "oak" here, the admit half of bioHomeOK's widened home (sim/nav.js)
            const omS = bioMe === BIO_OAKF ? Math.max(oakM(sx, sz), birchM(sx, sz)) : Math.max(oakM(sx, sz), birchM(sx, sz));
            if (bioMe === BIO_OAKF ? omS < BIO_DESERT : omS > BIO_FOREST) continue; }
          // ── THE BLOSSOM ADMITS TWO KINDS AND REFUSES THE REST (user 2026-08-18) ── the other half of the
          // containment in sim/nav.js, and the half that decides what is BORN here rather than what may walk
          // here. cherryM is only sampled when the point could possibly be in the band (it is a sub-region of
          // the oak mask), so the pine forest and the desert pay nothing for this test existing.
          //   * BIO_CHERRY — the pink bird — must be DEEP inside, at the same 0.85 the desert band uses, so it
          //     is never placed standing in the dithered edge where the blossom is half oak.
          //   * worms and butterflies (BIO_ANY, wantK 0 and 2) are admitted anywhere in it.
          //   * everything else is refused, which is the whole of "remove all the life except for the worm".
          // Written as an explicit kind test rather than "not a mammal": the slot bands already say what a slot
          // is, and naming the two that stay is a list that a future creature cannot silently join.
          if (bioMe === BIO_CHERRY) { if (!chNear(sx) || cherryM(sx, sz) < BIO_DESERT) continue; }   // chNear first: this runs up to 24 slots x 12 tries a frame
          else if (oakM(sx, sz) > 0) {                 // cheap-out: outside the oak mask the blossom cannot be here
            const cmS = cherryM(sx, sz);
            // The admit list is by SLOT BAND, not by kind: the four land mammals run on the WORM machinery and
            // are kind 2 exactly as a worm is (see wantK above), so `wantK === 2` would have walked every bunny,
            // armadillo, skunk and porcupine straight into the blossom. wormSlot is the band itself and cannot
            // be confused. Fireflies fall out for free — they are kind 1 at night, not kind 0.
            // …and FISH. The betta is the blossom's own species and it was NOT on this list, while line ~404
            // simultaneously removed it from the ordinary round-robin — so it was refused in cherry water by
            // this gate 170 lines before the species branch ran, and excluded from every other lake by that
            // branch. It could not spawn anywhere in the game. One admit list, and it has to name everything
            // that lives here: worms, day butterflies, and the betta.
            // ── AND THE BEES (user 2026-08-18: "have bees in the cherry forest") ── this is the BIO_ANY exclusion
            // the hive-placement note in world/terrain.js points at. The hives already hang in the blossom; the
            // bees were the half that was still refused here, so the nests were empty ones.
            // It rides `oakSlot && desBee` rather than a bioMe test because the bee is a BIO_OAKF creature and
            // the blossom band is INSIDE oakM — every other gate on the way here already passes it (the
            // BIO_OAKF branch above, and bioHomeOK, both read oakM, which is 1 in the band). This single line
            // was the whole refusal.
            // The errands work in the band without any further change: beeHomeHive anchors on the hive nearest
            // the PLAYER, and the forage test is FLOWERHEAD, which now carries the blossom band's own pink
            // flower (assets/bow.js). So a cherry-forest bee has both a nest and something to visit.
            const cherryLife = (bioMe === BIO_ANY && ((wantK === 0 && !isDfly) || wormSlot || (wantK === 6 && BETTA_FSP >= 0)))
                            || (oakSlot && desBee);   // day butterflies and worms; dragonflies need water and are not wanted here
            if (cmS > BIO_FOREST && !cherryLife) continue;
          }
          if (sx <= rect.xlo + 4 || sx >= rect.xhi - 4 || sz <= rect.zlo + 4 || sz >= rect.zhi - 4) continue;   // NEVER spawn outside the GENERATED rect — hmap there is garbage (the snow-landing guard; embedded 'cave' creatures after recenters)
          sx7 = sx; sz7 = sz;
          // ── …AND AN OAK FLYER TAKES A HOME CELL HERE (user 2026-08-17) ── the desert band falls through to
          // this annulus deliberately (see the note on the home-finder branch above), and the annulus is the
          // one spawn path that never set hcx — so FLY_LEASH, which is gated on `B.hcx !== undefined`, has
          // never applied to a desert flyer. That is fine for the FLY, whose desert is 2000 voxels across;
          // it is not fine for a bee, which is admitted at oakM > 0.85 and would otherwise be free to leave
          // the wood entirely at 56 vox/s. Same one line the DRAGONFLY uses to stay over its water, and it
          // buys the same second thing there: the recycle then judges by the HOME rather than by wherever
          // the insect has flown to. BEE_FLOWER_R is deliberately under FLY_LEASH so the errands stay inside
          // the disc this pins, and the two goals can never pull opposite ways.
          if (oakSlot && desFly) { hcx = Math.floor(sx / FLY_CELL); hcz = Math.floor(sz / FLY_CELL); }
          const gS = bfSurf(sx, sz);
          if (wantK === 2 && gS <= WL + 0.5) continue; // worms spawn on LAND only
          // ── DESERT LIFE SPREADS OUT (user 2026-08-15: "there shouldnt be geckos right next to each other") ──
          // The desert band was the ONE band with no spacing floor: every other kind has had a tooClose gate for
          // ages (mammals 70, ducks 200, fireflies 40, fish 14) and desSlot was simply never wired into any of
          // them, so its 42 creatures were placed by uniform random darts. Uniform random is not even — it
          // CLUMPS (Poisson), which is exactly the pair of touching geckos. There is no shortage of room: the
          // annulus is ~934k vox², so the mean gap is already ~105 vox overall and ~280 within a species. The
          // floors below are about a HALF of those means, which is the usual Poisson-disc working point.
          // TWO floors, because one cannot do both jobs: the same-species one is what the user actually sees
          // (six geckos reading as a litter), the cross-species one just stops a pile-up of mixed bugs.
          // ── …AND THEY DO NOT APPLY TO A SWARM (2026-08-17) ── DES_APART is 160 and never relaxes below
          // half of it, while the hive jitter above is 14-32, so the SECOND bee placed at a hive was
          // rejected here on every one of its twelve tries and ended up at a different hive entirely. That
          // is the whole reason a hive never held more than one bee. The floor exists so six geckos do not
          // read as a litter; five bees round a hive is the behaviour that was asked for, so the one
          // placement that is deliberately a cluster is exempt — the ant heel already takes the identical
          // exemption for the identical reason, and nothing else about the band changes.
          if (desSlot && !antHeel && !beeAtHive) {
            // RELAXED BY TRY, so a floor can never STARVE a slot. Near the biome edge only a slice of the
            // annulus passes the desertM gate, and a hard floor there would leave slots permanently uninit —
            // i.e. fewer creatures, a worse bug than the one being fixed. Try 0 demands the full gap and the
            // last try demands nothing, so a cramped spot still fills, just less evenly.
            // CUBIC, and it now bottoms out at HALF rather than at zero. Cubic because a linear decay had
            // already given away half the gap by try 5. The floor under it is the newer part: letting spread
            // reach 0 meant the last try placed with NO spacing at all, which is how a pair ends up touching.
            // With the desert thinned to ~31 bodies in a ~934k vox² annulus even the full 160 is easy to
            // satisfy, so half of it is never a starvation risk — measured, the population still fills.
            const spread = 1 - 0.5 * Math.pow(tries / 11, 3);
            const sp0 = MAM_END + desSp * DES_PER;
            // ── A WATERSIDE SPECIES IS A CLUSTER ON PURPOSE, LIKE THE HIVE BEE ── DES_WATER confines the frog
            // to a 14-vox ring round a pond, which is smaller than either floor above, so the pair of them
            // would reject every frog after the first exactly as they once left every hive holding one bee.
            // Both floors have to yield, not just the same-species one: the frogs sit inside MAM_END..DES_END,
            // so DES_APART_ANY counts them against each other too. DES_WATER_APART (sim/life/slots.js) is a
            // real floor rather than the bee's outright exemption, so two frogs still never share a bank.
            // `spread` deliberately does NOT apply to the waterside floor: see DES_WATER_APART in
            // sim/life/slots.js — the ring is small enough that the last try's half-gap re-admits the very pair
            // the floor exists to prevent. A shore that cannot hold them apart holds fewer of them instead.
            const wApart = DESERTS[desSp] && DES_WATER[DESERTS[desSp].name] ? DES_WATER_APART : 0;
            if (tooClose(sp0, sp0 + DES_PER, wApart || DES_APART * spread)) continue;
            if (tooClose(MAM_END, DES_END, wApart || DES_APART_ANY * spread)) continue;
          }
          if (desSlot && wantK === 2 && !navSand(sx, sz)) continue;   // ── NO DESERT WALKER STARTS ON A ROCK OR A CACTUS (user 2026-08-16) ── the same rule its every step is judged by, asked once at placement. Without it a body placed on stone spends its whole life in the egress path, which is the one branch that moves without asking. The forest's own version of this test (isRockSurf over the MAMFIT footprint, below) is deliberately left where it is: it answers a different question, about worldgen strata, for a band this change does not touch.
          // CROSS-SPECIES floor: never spawn a land mammal within MAM_FLOOR of ANY live mammal (bunny/
          // armadillo/skunk/porcupine) — breaks up the multi-species KNOTS (user: skunk 24 vox from a bunny
          // etc.). 70 was count-starving under NEAREST-FIRST (even 24 left skunk 2 short — rejects had nowhere
          // else to go), but the HASH-ORDER scatter retries land anywhere in the disc, so counts hold.
          // RELAXED BY TRY (user 2026-08-16: "make sure that the land mammals are evenly spread out across the
          // pine forest"). The flat 70 was not spacing the band, it was DEFINING it: measured over six boots
          // the pooled nearest-neighbour distribution sat at min 70.2 / p10 79.9 / median 104.7, i.e. the tenth
          // percentile was standing on the gate. Asking for MAM_APART on the first try and decaying to the old
          // 70 by try MAM_RELAX gives the placement room to find a real gap while leaving the worst case
          // exactly where it has always been — so this cannot place anything closer than the flat rule did.
          // It reaches the floor at MAM_RELAX rather than at the last try for a measured reason: decaying
          // across the whole budget spent every try on a gate stricter than the old one, and a slot that used
          // to fill could exhaust all twelve (measured 23 of 24 alive in 2 of 12 samples — transient, the slot
          // retries next frame, but real). The last few tries now run at exactly the historical gate, so the
          // band cannot do worse than it always has.
          if (bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot) {
            const mamGap = MAM_APART - (MAM_APART - MAM_FLOOR) * Math.pow(Math.min(1, tries / MAM_RELAX), 3);
            if (!mateJoin && tooClose(MAM_0, MAM_END, mamGap)) continue;   // …except a flamingo joining its partner, which is DELIBERATELY inside the floor: 5-9 voxels is the whole point of a couple, and this floor is what silently prevented one
          }
          // ── NO LAND MAMMAL STARTS ON STONE (user 2026-08-07) ── this used to sample ONE column, at the spawn
          // point, and reject only when it found GREY three voxels deep. Two ways through it: a boulder or a
          // shallow outcrop is not three deep, and an animal is not a column — a skunk is 11 voxels long, so its
          // centre could sit on grass with half its body over rock. Now it is the model's own footprint (MAMFIT,
          // the same extents the ground servo seats on) against a real id table rather than a colour guess, which
          // also picks up BOULDERS: those live on dedicated palette ids and the grey test only caught them by
          // accident of shade.
          if (bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot) {
            const fitS = bunnySlot ? MAMFIT.bunny : (armSlot ? MAMFIT.arm : (skunkSlot ? MAMFIT.skunk : (flamSlot ? MAMFIT.flam : MAMFIT.porc)));
            const rS = fitS ? Math.max(fitS.hw, fitS.hd) : 3;   // heading is not chosen yet, so take the longer half-extent both ways — conservative by construction
            let onRock = false;
            for (let ux = -1; ux <= 1 && !onRock; ux++) for (let uz = -1; uz <= 1; uz++) {
              const qx = sx + ux * rS, qz = sz + uz * rS, qg = bfSurf(qx, qz);
              if (qg <= WL + 0.5) continue;                // underwater column — the land tests elsewhere own that
              // ── SCAN THE COLUMN, DO NOT TRUST hmap ── bfSurf is the HEIGHTMAP, and a stamped boulder sits ON
              // TOP of it: hmap never saw the stamp, so `gS - 1` reads the dirt underneath and every boulder in
              // the world was invisible to this test. Measured: 18,000 land columns, not one rock found through
              // hmap. Walk down from a little above the heightmap instead and take the first solid voxel — the
              // same thing the player sees underfoot.
              const bx9 = gwrap(Math.floor(qx), WX), bz9 = gwrap(Math.floor(qz), WZ) * WX * WY;
              for (let y9 = Math.min(WY - 1, Math.floor(qg) + 8); y9 >= Math.max(1, Math.floor(qg) - 2); y9--) {
                const v9 = W[bx9 + y9 * WX + bz9]; if (!v9 || solidTab[v9] !== 1) continue;
                if (isRockSurf(v9)) onRock = true;
                break;                                     // first solid from above IS the surface — whatever it is, stop
              }
              if (onRock) break;
            }
            if (onRock) { PH.stats.mamRockRej = (PH.stats.mamRockRej | 0) + 1; continue; }
            PH.stats.mamRockOK = (PH.stats.mamRockOK | 0) + 1;
          }
          if (wantK === 0 && isDfly && !bfWater(sx, sz)) continue;    // the jitter may have thrown it ashore — a dragonfly only ever starts over real water
          if (desSlot && DESERTS[desSp] && DES_WATER[DESERTS[desSp].name] && bfWater(sx, sz)) continue;   // …and the mirror of it: a frog starts ON THE BANK, never in the lake
          // ── …AND ON FLAT GROUND (user 2026-08-27: "try to spawn frogs on flat terrain ... flat terrain
          // around water") ── the shore ring alone will happily put a frog on the side of a bank, and a frog
          // placed on a slope spends its life leaping up or down one: that is where the landing snap and the
          // clipping below are worst, so the cheapest half of both fixes is to not start it there. Four
          // samples a stride out, and the spread between them is the slope. RELAXED BY TRY, the same shape as
          // the spacing floors above and for the same reason — a demand that cannot be met must not empty the
          // pond. Try 0 wants a voxel of level ground, the last try takes whatever the shore offers.
          if (desSlot && DESERTS[desSp] && DES_WATER[DESERTS[desSp].name]) {
            const g0 = navWalkStand(sx, sz);
            if (g0 === undefined) continue;
            let lo0 = g0, hi0 = g0, okF = true;
            for (const [ox, oz] of [[4, 0], [-4, 0], [0, 4], [0, -4]]) {
              const gq = navWalkStand(sx + ox, sz + oz);
              if (gq === undefined) { okF = false; break; }
              if (gq < lo0) lo0 = gq; if (gq > hi0) hi0 = gq; }
            if (!okF) continue;
            if (hi0 - lo0 > 1 + (tries >> 2)) continue;   // ONE voxel of slope for the first four tries, then a voxel more every four. Opening it a voxel per TRY was no constraint at all by the end of twelve — measured, a frog still landed on a 3-voxel slope — and the point of relaxing is to fill a cramped shore, not to give the rule up
          }
          if (wantK === 0 && !isDfly && bfWater(sx, sz)) continue;    // …and a BUTTERFLY never starts over water (user: limit butterflies over lakes); it may still drift out over one
          if ((wantK === 3 || wantK === 4) && (!bfSky(sx, WL + 2, sz) || !bfOpenW(sx, sz))) continue;   // ducks + lilies spawn on OPEN-SKY, WIDE REAL-WATER only (bfOpenW now tests actual water voxels) — cave pools AND dry gorge/ravine floors are out
          // ── BETTAS SCHOOL (user 2026-08-18: "make the betta fish swim in schools") ── the 14-voxel floor is an
          // ANTI-clustering rule, and schooling is its opposite, so the betta is exempt from it and pulled TOWARD
          // its own kind instead. Every other species keeps the floor: one salmon per 14 voxels is what stops a
          // lake reading as a shoal of the wrong fish.
          // The species has to be decided HERE, before the water test, because the schooling exemption depends on
          // it — B.fsp itself is not written until the placement succeeds, ~20 lines below, so reading it here
          // read the PREVIOUS occupant of the slot. It is a pure function of (wk, sx, sz), so computing it early
          // costs nothing and the assignment below uses this same value rather than recomputing it.
          fspWant = wantK !== 6 ? -1
            : ((BETTA_FSP >= 0 && chNear(sx) && cherryM(sx, sz) > BIO_FOREST) ? BETTA_FSP
              : (wk % Math.max(1, FISHES.length - (BETTA_FSP >= 0 ? 1 : 0))));
          const schooling = BETTA_FSP >= 0 && fspWant === BETTA_FSP;
          // ── THE FLOOR RELAXES INSTEAD OF STANDING STILL ── a flat 14 was low enough that a pool could still read
          // as a knot, but raising it flat would have left small ponds empty. Cubic like the desert's: the first
          // retries insist on 28, and only a spot that keeps failing settles back toward the old 14.
          const fishApart = FISH_APART - (FISH_APART - FISH_FLOOR) * Math.min(1, Math.pow(tries / FISH_RELAX, 3));
          if (wantK === 6 && (!bfWater(sx, sz) || WL - bfBed(sx, sz) < 4 || (!schooling && tooClose(FISH_0, FISH_END, fishApart)))) continue;   // FISH need REAL water DEEP enough for a body below the surface (4+ vox)
          if (schooling) {                             // …and gather on a betta already in this pool, the way a swarm bee gathers at its hive rather than waiting to drift there
            let anchor = null, bestD = 1e9;            // the old loop broke on the FIRST betta it saw, so it could neither measure that shoal nor look past a full one — one anchor served the whole pool forever
            for (let f = FISH_0; f < FISH_END; f++) { const F = wbf[f];
              if (!F || F === B || !F.init || (F.kind | 0) !== 6 || F.fsp !== BETTA_FSP) continue;
              const d9 = (F.x - sx) * (F.x - sx) + (F.z - sz) * (F.z - sz);
              if (d9 >= BETTA_JOIN * BETTA_JOIN || d9 >= bestD) continue;
              let mates = 0;                           // …measured around the ANCHOR, which is where a shoal actually is
              for (let g = FISH_0; g < FISH_END; g++) { const G = wbf[g];
                if (!G || G === B || !G.init || (G.kind | 0) !== 6 || G.fsp !== BETTA_FSP) continue;
                if ((G.x - F.x) * (G.x - F.x) + (G.z - F.z) * (G.z - F.z) < BETTA_HUDDLE * BETTA_HUDDLE) mates++; }
              if (mates < BETTA_SCHOOL) { anchor = F; bestD = d9; } }             // a full shoal is simply skipped, and the search carries on to the next one
            if (!anchor && tooClose(FISH_0, FISH_END, BETTA_APART)) continue;      // no shoal to join → this fish OPENS one, and it has to open well clear of every other fish. Spacing has to act HERE, at the shoal, because the members are deliberately exempt from it
            if (anchor) {
              const a8 = Math.random() * 6.2831853, r8 = 3 + Math.random() * 6;   // 3-9 voxels: one shoal to look at, far enough apart that the bodies do not interpenetrate
              const nx = anchor.x + Math.cos(a8) * r8, nz = anchor.z + Math.sin(a8) * r8;
              if (bfWater(nx, nz) && WL - bfBed(nx, nz) >= 4) { sx = nx; sz = nz; sx7 = nx; sz7 = nz; }   // …but never out of the water it has to swim in. The HOME moves with the body (sx7/sz7 were frozen at the census point ~110 lines up): without that the leash spends the rest of the session towing every member back to a home the shoal never was, and the shoal quietly dissolves
            }
          }   // FISH need REAL water DEEP enough for a body below the surface (≥4 vox) — rivers qualify, shore shelves don't; ≥14 apart so a pool holds a loose school, not a knot
          if (wantK === 1 && tooClose(FLY_0, FLY_END, 40)) continue;   // FIREFLIES still spread by proximity. BUTTERFLIES must NOT: their homes are already 128 vox apart by the cell grid, and testing against other butterflies' WANDERING positions made placement depend on where they happened to be — which is exactly what stopped the set being reproducible.             // FLYERS spread ≥40 vox apart at spawn → even distribution, never clustered (user)
          // (worms no longer use tooClose: their homes are already 160 vox apart by the cell grid, and testing against other worms' CRAWLING positions made placement depend on where they had got to)          // worms spread WIDE — never bunched (user, doubled with the population)
          if (wantK === 3 && tooClose(DUCK_0, DUCK_END, 200)) continue;         // mother ducks spawn ≥200 vox apart → at most ~2 families fit on a typical lake (user: 'only 1-2 per lake')
          if (wantK === 4 && tooClose(CARD_0, CARD_END, 12)) continue;   // lilies scatter — and the band is CARD_0..CARD_END (lilySlot), not the 40-55 this read: the three literals on these lines were the last hard-coded band edges left after the ladder refactor, and this one had been WRONG since the lily slots were repurposed for the perched songbirds. Inert either way — nLily is 0, so no creature has had wantK 4 since — which is exactly why it survived: a stale band literal does not throw, it silently addresses the wrong animals.
          const sy = wantK === 6 ? (bfBed(sx, sz) + WL) * 0.5 : (wantK === 2 ? gS + ((desSlot && DESERTS[desSp] && MAMFIT[DESERTS[desSp].name]) ? MAMFIT[DESERTS[desSp].name].seat : 2) : (wantK === 3 ? WL + 4 : (wantK === 4 ? WL + 1.4 : (desFly ? DES_FLY_UP : 0) + bfGlide(sx, sz) + (wantK ? 9 : 14))));   // fish hang at MID-DEPTH; ducks/lilies float; duck FEET sit inside the water voxel layer
          if (wantK === 6 || ((wantK === 2 || !bfRoofed(sx, sy, sz)) && !bfObst(sx, sy, sz))) {   // a fish spot was already fully validated (real deep water) — bfObst would see the WATER voxels as solid and reject every one; worms live happily UNDER the canopy — the roof test starved dense-forest placement ('worms only near spawn')
            B.x = sx; B.z = sz; B.y = sy; placed = true;
            const cx = Math.floor(sx / 64), cz = Math.floor(sz / 64);   // color = spatial hash of the 64-vox cell → world-anchored variety, all 3 colors present
            B.col = ((Math.imul(cx, 374761393) ^ Math.imul(cz, 668265263)) >>> 0) % Math.max(1, BFLY_HASHN || BFLY_COLS.length);   // HASHN, not length: the synthetic yellow sits past the authored colours and must never be rolled at random — it stands in for pink, below
            // ── AND THE BLOSSOM GETS ONE COLOUR (user 2026-08-18: "have butterflies but only pink butterflies") ──
            // the spatial hash above is what scatters the six colours evenly across the world; here it is simply
            // overridden. Scoped to the flyer kinds and guarded on BFLY_PINK >= 0 so a missing pink/ folder
            // degrades to the normal scatter rather than to butterflies with no colour at all. B.col is also the
            // LILY size index (the lily path is inert today, nLily = 0, but the field is genuinely shared), which
            // is the other reason this is not written as a blanket assignment.
            if (BFLY_PINK >= 0 && wantK === 0 && !isDfly && cherryM(sx, sz) > BIO_FOREST) B.col = BFLY_PINK;
            // ── AND PINK IS THE CHERRY BAND'S ALONE (user 2026-08-22: "make the pink butterfly a yellow one
            // instead. remove the pink butterflies in the pine and oak forests. leave the cherry forest
            // butterflies alone") ── the line above FORCES pink inside the band; outside it the hash could
            // still roll pink on its own, which is where the forest pinks came from. Substituting rather than
            // re-rolling keeps the per-cell colour map stable: a cell that was pink is now yellow, and every
            // other cell keeps the colour it had. Runs after the cherry line, so the band is untouched.
            else if (BFLY_YELLOW >= 0 && wantK === 0 && !isDfly && B.col === BFLY_PINK) B.col = BFLY_YELLOW;
          }
        }
        if (!placed) { if (B.sN) unstampWorm(B); B.init = false; continue; }     // no open spot this frame — stay hidden, retry next
        if (wantK === 0 || wantK === 2 || wantK === 6) { B.hx = sx7; B.hz = sz7; B.hcx = hcx; B.hcz = hcz; } else { B.hcx = undefined; }
        B.dfly = (wantK === 0 && isDfly); B.dfMis = 0;              // the strip + frame count key off this; everything else about the creature is butterfly
        if (wantK === 6) { B.spd = FISH_CFG.baseSpeed * 0.8; B.spdT = FISH_CFG.baseSpeed; B.dT = 0.3 + Math.random() * 0.5; B.dRe = 0; B.animClk = Math.random() * 40; B.cx = B.x; B.cy = B.y; B.cz = B.z; B.cth = B.th; B.vyS = 0;   // FISH: ease up into the configured cruise + desynced tail-beat; cx/cy/cz = last body-clear pose (terrain hitbox revert)   // …and B.cth. Three of the four clear-pose fields were written here and the heading was not, so a fish that clipped terrain before its first clear frame reverted to the PREVIOUS occupant's heading — and since fishBodyAt is heading-dependent, the revert could then keep failing at the same spot
          // ── SPECIES BY SLOT, EXCEPT THE BETTA (user 2026-08-18: "put the pink betta fish in the water of the
          // cherry forest") ── the split is by SLOT for the reason the songbirds' is: an exact even share that
          // cannot drift as fish recycle. The betta comes OUT of that rotation and is handed to cherry water
          // alone, the same shape the pink bird takes: the round-robin still runs over the ORDINARY species and
          // the biome overrides it, so no betta appears in the pine forest and no salmon in the blossom.
          // bettaIx is read from the LOADED order, never written as a literal, because a species whose frames
          // fail to load shifts every index after it.
          B.fsp = fspWant >= 0 ? fspWant : 0;          // …the same value the schooling test used, so the two can never disagree about what this fish is
          B.fhalf = (FISHES[B.fsp] || {}).half || 5;              // body half-length for every navigation probe — per species, not a salmon-sized guess
          { const sn = (FISHES[B.fsp] || {}).name;                // per-species LEAP frequency multiplier from the config (unlisted species get a modest default)
            B.jumpMul = FISH_CFG.jump.species[sn] !== undefined ? FISH_CFG.jump.species[sn] : 0.25;
            B.schools = FISH_CFG.schoolSpecies.indexOf(sn) >= 0; }   // …and whether this species shoals at all
          B.jumpV = undefined; B.jumpArm = undefined; B.fleeT = 0; B.spookT = 0;   // a recycled slot must never inherit a live arc, a stale flee window or the last occupant's wound-panic
          B.jumpRe = tb3 + (3 + Math.random() * 14) / Math.max(0.05, B.jumpMul);
          // ── SCHOOL ── join a same-species school that still has room (spawning right beside it), else start a fresh school of 3-6, else swim alone (~28%)
          B.school = -1; B.schoolCap = 0;
          for (let m = FISH_0; m < FISH_END; m++) { const O = wbf[m]; if (O === B || !O.init || (O.kind | 0) !== 6 || O.school < 0 || O.fsp !== B.fsp) continue;
            if ((O.x - B.hx) * (O.x - B.hx) + (O.z - B.hz) * (O.z - B.hz) > 60 * 60) continue;   // LOCAL schools only — joining used to TELEPORT the spawn beside a school in a DIFFERENT pool, bypassing that pool's cap (this piled extra fish into small ponds, and a pond-locked fish could never swim back to its real home)
            let cnt = 0; for (let q = FISH_0; q < FISH_END; q++) { const Q = wbf[q]; if (Q.init && Q.school === O.school) cnt++; }
            if (cnt < O.schoolCap && bfWater(O.x, O.z) && WL - bfBed(O.x, O.z) >= 3) { B.school = O.school; B.schoolCap = O.schoolCap; B.x = O.x + (Math.random() - 0.5) * 9; B.z = O.z + (Math.random() - 0.5) * 9; break; } }
          if (B.school < 0 && B.schools && Math.random() > 0.18) { B.school = (fishSchoolSeq = (fishSchoolSeq + 1) & 0x3fffffff); B.schoolCap = 4 + (Math.random() * 5 | 0); } }   // 4-8 per school; only the SCHOOLING species form them (user: salmon + minnow for now), and they do so more often now that there are twice as many fish
        B.th = isBaby ? mom5.th : Math.random() * 6.283;
        // ── AND THE WATCHDOG / FILTER STATE (audit 2026-08-18) ── everything below is seeded lazily under an
        // `=== undefined` test or only ever `+=`'d, so a recycled slot inherited the PREVIOUS occupant's value
        // and measured a brand-new body against it. The fish stall watchdog was the one that could be seen: a
        // fresh fish skipped its baseline seed, took the `tb3 - B.stallT > 1.8` branch on its very first frame,
        // compared its position to where the last occupant had been standing, and got teleported 40 voxels or
        // dropped outright — a fish that flickers in and vanishes, with nothing logged. The rest are quieter
        // (a duck riding high until its waterline filter re-converges, a fish bolting from a threat the last
        // occupant saw, a frame of the wrong gait), but they are all the same defect and this is the one list
        // that is meant to prevent it. `B.blinked` was already here; `B.blink`/`B.blinkT` were not, which is
        // what suggested the omission was accidental rather than considered.
        B.stallT = undefined; B.stallX = undefined; B.stallZ = undefined; B.noMove = 0;   // fish stall watchdog
        B.wvM = undefined; B.thrX = undefined; B.thrZ = undefined; B.chase = 0;           // duck waterline filter, fish threat memory, heel-distance gait
        B.blink = false; B.blinkT = 0;                                                    // …beside B.blinked, which was already reset
        // ── FLIES COME IN A BUNCH, NOT ONE BY ONE (user 2026-08-22: "I see individual flies? can remove the
        // single flies and add the bunch of flies?") ── the desert scatter gives every slot its own spot with
        // DES_APART between them, which is right for a scorpion and wrong for a fly: it reads as a lone insect
        // hanging in the air. So a fly ADOPTS a live bunch-mate's anchor when that bunch has room, and only
        // opens a new one when none has — the flamingo's pairing rule, with a count of FLY_BUNCH instead of 2.
        // The anchor is a POINT, kept on the creature rather than in a cell table, because the bunch has no
        // home cell of its own: it is wherever the first fly of that bunch happened to be placed.
        if (desFly && DESERTS[desSp] && DESERTS[desSp].name === 'fly') {
          const lo9 = MAM_END + desSp * DES_PER, hi9 = lo9 + DES_PER;
          let host = null, hostN = 0;
          for (let j9 = lo9; j9 < hi9; j9++) { const O = wbf[j9];
            if (!O || O === B || !O.init || !O.bn) continue;                             // !O.bn, not O.bnx: the anchor is a shared object now and the old field name silently matched nobody, so every fly opened its own bunch
            let n9 = 0;
            for (let k9 = lo9; k9 < hi9; k9++) { const Q = wbf[k9];
              if (Q && Q.init && Q.bn === O.bn) n9++; }                              // identity, not coordinates: the anchor MOVES now, so comparing its x/z stopped
            if (n9 < FLY_BUNCH && (!host || n9 < hostN)) { host = O; hostN = n9; }   // the emptiest bunch with room, so they fill evenly rather than piling into the first
          }
          // The anchor is ONE SHARED OBJECT rather than a copy per fly. A copy is what made the drift awkward:
          // every member would have to integrate the same wander and they would diverge on rounding alone.
          // Shared, the bunch has exactly one position and one heading, and `st` lets whichever member ticks
          // first that frame own the step — without it N flies would advance it N times and the bunch would
          // travel at N x FLY_BN_SPD, faster the more of them there are.
          if (host) B.bn = host.bn;
          else B.bn = { x: B.x, y: B.y, z: B.z, ox: B.x, oz: B.z, th: Math.random() * 6.2831853, om: 0, omT: 0, tRe: 0, st: -1 };
          B.bnPh = Math.random() * 6.2831853;          // its own place on the ring, so the bunch is a bunch and not one fly with copies
        } else B.bn = undefined;
        B.om = 0; B.omT = 0; B.turnAcc = 0; B.tRe = 0; B.trap = 0; B.born = now; B.init = true; B.hurt = 0; B.hits = 0; B.dying = false; B.blinked = false; B.hopT0 = undefined; B.lastSwing = undefined; B.spookT = 0; B.trail = undefined; B.beeM = undefined; B.fgP = undefined;   // …and B.fgP undefined re-anchors the FROG, for exactly the reason given for the bunny below: its position is ASSIGNED from B.fgX/B.fgZ every frame, so a recycled slot that kept the last occupant's anchor OVERWRITES wherever the spawner just placed it. MEASURED with a sentinel anchor 900 voxels out: the re-placed frog arrived at the sentinel, not at the bank it was given, on the first frame and stayed there   // …and B.beeM undefined re-seeds the BEE's errand machine from scratch: a recycled slot that kept beeM 2 would go on pinning itself to the previous occupant's flower hundreds of voxels away, every frame, exactly the way a stale B.bh snapped a re-placed bunny back to its old cell (see the note below). It is in THIS list and not beside the bee code for the same reason every other field here is: one place to look for what a fresh occupant must not inherit.   // fresh occupant — never inherits the last one's knife wound, pending death, spent flash, panic, wound-up yaw or (ant leader) RECORDED PATH: a stale trail is a line of followers snapped back to where the previous occupant walked (turnAcc: a recycled duck inheriting most of a circle reads its very first bank as an over-wound spin and unwinds the long way round for nothing)
        // bh/ah undefined → a (re)spawned bunny/armadillo reinits its cardinal state machine from the fresh heading.
        // THIS LINE WAS DEAD: it used to sit after a `//` on the line above, so the trailing comment ate it. The
        // bunny is the one creature whose position is ASSIGNED rather than integrated (B.x = B.bpx + bake offset),
        // so a stale bh meant a stale bpx/bpz base cell: the frame after it was re-placed near the player it
        // snapped straight back to its old spot hundreds of voxels away, went out of range again, was re-placed
        // again… every single frame. That is the bunny "glitching" and being impossible to hit. The armadillo and
        // the other marchers survived it because they integrate (B.x += …), so a stale heading is harmless.
        B.bh = undefined; B.ah = undefined;
        B.kind = wantK; B.dieT = 0; B.glow = false; B.glowT = 0; B.gRef = bfSurf(B.x, B.z);
        if (duckSlot && !isBaby) B.nBab = 3;           // every mother leads exactly 3 ducklings (user)
        if (lilySlot) {                                // PERCHED BIRD init (lily slots repurposed): a random animation phase so they're DESYNCED, + red cardinals / BLUE BIRDS (the reskin) — both grid-stamped by stampCardinal
          B.phase = Math.random() * 100000;
          // INTERMIX THE SONGBIRDS (user: "you're clustering the colored ones with each other"). An independent
          // 50/50 flip per bird is what produced the clumps — random runs of one colour are normal in a coin flip
          // and read as deliberate grouping. Instead take the colour that is UNDER-represented among the perched
          // birds ALREADY nearby, so every neighbourhood self-balances; ties still flip a coin so it never
          // degenerates into a rigid checkerboard. Population stays ~50/50 globally.
          // COLOUR COMES FROM THE PINE, not from a coin flip at spawn: a DIAGONAL 3-colouring over 40-vox blocks
          // (cardinal / blue bird / robin), with ~30% of trees kicked to one of the other two so it never reads as a
          // literal grid. Deterministic per tree, so the same pine is always the same colour and the mix cannot drift
          // or clump as birds recycle. The 3-colouring guarantees ADJACENT blocks never share a colour, which is what
          // keeps the songbirds intermixed now that there are three of them.
          B.bird = birdColour(B.tx, B.tz, B.bi);
        }
      }
      const fadeIn = Math.min(1, (now - B.born) / 700);   // grow in over 0.7 s so a recycled creature materialises smoothly instead of popping into view
      const fadeOut = B.dieT ? Math.max(0, 1 - (now - B.dieT) / 700) : 1;   // …and shrinks away at the day/night handover
      const bScale = 0.12 + 0.88 * fadeIn * fadeIn * (3 - 2 * fadeIn) * fadeOut;
      if (B.kind === 5) { stampCardinal(B, now, wk); continue; }   // ── PERCHED CARDINAL ── GRID-STAMPED (aligned to grid, identical grid lighting + rotation + offsets to the asset-editor cardinal); stationary, so no shimmer beyond the editor's
      let Hx2 = Math.sin(B.th), Hz2 = Math.cos(B.th);
      const duckArb = NAVARB && (B.kind | 0) === 3;
      const mamArb = NAVARB && (B.kind | 0) === 2 && (armSlot || skunkSlot || porcSlot || flamSlot);   // BUNNY DELIBERATELY EXCLUDED (measured 2026-08-06): on seed 909091 it lost 12-16% of its distance and speed on the arbiter, and an order-swapped rerun reproduced it, so it follows the BUILD and not run position. It is the only one of the four that HOPS 5-6 voxels instead of walking, so a single refused station costs it a whole hop where a walker just takes a shorter step. The other three gained (armadillo path +6%, hard-stops -33 to -40%; skunk and porcupine hard-stops -67 to -96%) and keep the arbiter. Re-including the bunny needs the hop to be planned as one committed arc rather than per-station.   // ── LAND MAMMALS, ON THE ARBITER ── they share B.kind 2 with the worm and drive their own march, so the worm's wiring is not theirs; this flag is the whole of the band's opt-in and ?noarb turns it off without touching one character of the marcher's own arithmetic.
      const walkFree = (tx, tz) => mamArb ? navWalkFree(tx, tz, NAV_MCLR)   // the RELAXED gate the boxed-in and watchdog escapes are chosen with AND advanced under: land + body-clear, step limit ignored. It was hand-rolled identically in three places (two marcher escapes and the bunny's), and is now ONE function at a scope BOTH mammal controllers can see, because a marcher picking an exit under one rule and advancing under another is the original 'stuck on terrain' bug.
        : (bfSurf(tx, tz) > WL + 0.5 && !bfObstW(tx, bfSurf(tx, tz) + 2, tz) && !bfObstW(tx, bfSurf(tx, tz) + 4, tz));
      const duckFit = (px, pz) => bfWater(px, pz) && bfSky(px, WL + 2, pz);   // ── THE DUCK'S ONE ANSWER ── real water under open sky, which is what every one of its consumers was already reaching for; they just each asked a different subset. It is now literally ONE function behind the planner's lookahead, the reach fan, the blocked-escape probe, stepOK and the head buffer. It is deliberately NOT the field's swim band: gating on nvD >= 3 was built and measured, and it refused 3.6%% of a mother duck's steps on her own lake (322 stop events per creature-minute against 0.8, and 6%% off her distance) for a band that had zero stuck-seconds to begin with. The arbiter's job in this band is to remove the disagreement, not to add a rule.
      // ══ THE BEE'S ERRANDS (user 2026-08-17) ══ 'going to flowers and sitting on them briefly', and 'make bees
      // swarm around' a hive. One five-state machine, run every frame because it is TIMERS; the steering it asks
      // for is a single goal BEARING handed to navSteerAir's existing homeTh/leashOut seam a hundred lines down,
      // and the two HOLD states pin the pose after every servo has run, the way the ant followers are placed.
      // Nothing here writes B.th, refuses a step or second-guesses navFitsAir. That is the point: sim/life/fish.js
      // was broken for a long time because its planner and its mover each had an opinion about what was
      // reachable and they disagreed, so the fish swam into terrain and stayed there. The bee cannot express
      // that bug — an unreachable flower is not refused, it is simply never arrived at, and BEE_GIVE_S ends the
      // attempt and BANS that column for BEE_BAN_S so the next look picks a different one.
      //   B.beeM  0 wandering · 1 flying to a flower · 2 sitting on it · 3 flying to a hive · 4 orbiting it
      //           5 ENRAGED — its hive has just been broken open and it is on the player (2026-08-17)
      // ══ …AND THE FIFTH STATE IS AN ADDITION, NOT A REPLACEMENT ══ (user 2026-08-17: "if the player breaks
      // open the beehive, have bees fly out of it, attacking the player.") Everything above it is untouched to
      // the character: 5 is one more arm of the same if/else chain, one more clause at the steering seam, one
      // more branch in the altitude servo, and it is NOT one of the two HOLD states down at the pose pin — so
      // foraging, flower-sitting and hive-orbiting all still run exactly the code they ran yesterday.
      if (desBee && B.init && !B.dying && !B.slain) {
        if (B.beeM === undefined) { B.beeM = 0; B.beeRe = tb3 + Math.random() * BEE_LOOK_S; B.beeHRe = 0; B.beeBanT = 0; B.beePh = Math.random() * 6.283; B.beeRgRe = 0; B.beeHck = 0; B.beeStings = 0; }   // …and the three RAGE fields seed from here for the same reason every other one does: the recycle list up in the spawn block clears B.beeM alone, and this branch is what a fresh occupant re-derives the rest of the errand machine from. A kept beeRgRe would bar a new bee from a hive break it never saw.
        const beeSwarm = desIx < BEE_HIVE_N;           // WHICH bees go to a hive is a pure function of the slot number — no per-frame arbitration, no scan of the other bees, and the split cannot drift as slots recycle. The rest keep foraging, so a hive in view never empties the meadow.
        // ── WHERE THE ANGRY BEES COME FROM, AND WHY THERE ARE AT MOST FIVE ── nothing is spawned and no slot is
        // stolen: the attackers are the hive's OWN bees, recruited off the break ledger. The cap is not a new
        // constant, it is beeSwarm — the SAME slot-number split that already decides who lives at a hive and who
        // forages. Three consequences, and each is the reason it is written this way rather than as a scan:
        //   * The meadow is guaranteed. The three high slots are never recruited under any circumstance, so
        //     however many hives the player smashes there are always bees out visiting flowers.
        //   * The five are the right five. They are precisely the bees that go to hives, so on any hive the
        //     player can actually find, the bees that come out of it were the bees that were living in it.
        //   * There is no arbitration and nothing to go stale across a slot recycle, exactly as BEE_HIVE_N says.
        // Entry sits BEFORE the state dispatch on purpose: a bee mid-orbit of the hive that just came apart has
        // to drop what it is doing, and so does one sitting on a flower fifty voxels away. It can interrupt any
        // state, which is what makes it read as the hive erupting rather than as five bees finishing an errand.
        // ── …AND THE CAP IS GONE (user 2026-08-19: "if the player breaks the beehive, have all the bees
        // nearby attack him") ── the five paragraphs above are the record of why a hive break used to recruit
        // only beeSwarm, and every one of them is still TRUE — they are just no longer what the user wants.
        // ALL is the word, so the ledger asks one question of every bee now: are you inside this wreck's own
        // reach (BEE_BREAK_R for a hive, BEE_RAGE_R for a swat — sim/life/slots.js), and is it still calling.
        // beeSwarm is therefore no longer passed in and no longer means "who may be angry"; it goes back to
        // meaning only what its own comment says — which bees LIVE at a hive — and is still read by the orbit
        // bound below. The three guarantees the cap used to provide are now carried by the two clocks that
        // always really carried them: BEE_RAGE_S ends every rage, and BEE_BREAK_R is under half the hive
        // spacing, so a smashed hive calls its own neighbourhood and never the next hive's swarm.
        if (B.beeM !== 5 && tb3 > (B.beeRgRe || 0)) {
          const rg = hiveRageAt(B.x, B.z, tb3);
          if (rg) { B.beeM = 5; B.beeRgX = rg.x; B.beeRgZ = rg.z; B.beeT = tb3 + BEE_RAGE_S; rg.n++; }
        }
        // ── AND THE HIVE IS WATCHED WHATEVER THE BEE IS DOING (user 2026-08-19: "have the bees also target
        // the player when the player hits the behive and breaks it") ── the axe posts to the ledger itself,
        // but only from the one branch of chopSwing whose crosshair id IS a hive voxel, and that branch is
        // reached far less often than a swing lands. MEASURED at a hive 13 voxels away, aiming dead at it:
        // five consecutive swings returned `hit` with the hive's own voxel count frozen — the ray met the
        // crown standing in front of the hive first, so the swing was spent on foliage and tools.js never
        // asked. An ARROW never asks at all: sim/projectiles.js reaches phChopDecor by its own path with no
        // hook on it. Both are SILENT BREAKS, and a hive that comes apart in silence is exactly the report:
        // the player breaks it and no bee ever answers, while swatting one always works because
        // sim/life/reactions.js fires on EVERY blow rather than on one privileged branch of one weapon.
        // So the world is ASKED on a clock instead of the weapon being trusted to report — the same answer
        // beeBloomAt gives for a flower that may no longer be there, and the same guarantee the swat has.
        // This lived inside the ORBIT branch before, which made it a watch on hives that happened to have a
        // bee in state 4 at that instant; a bee out at a flower, inbound, or already enraged watched nothing.
        // Scoped to the SWARM slots — the bees that live at a hive — and to the PLAYER's own hive, which is
        // the one being chopped and is already cached at BEE_HOME_POLL, so it adds no search. One 125-cell
        // count per second per swarm bee: exactly what the orbit branch paid, and hiveBroke is idempotent, so
        // this and the swing racing to the same hive still post one record between them.
        if (beeSwarm && tb3 > (B.beeHck || 0)) {
          B.beeHck = tb3 + 1;
          const hw9 = beeHomeHive(P.x, P.z, tb3, LIFE_OUT);
          if (hw9 && hiveLeft(hw9) <= hiveFull() * BEE_BREAK_F) hiveBroke(hw9);
        }
        if (B.beeM === 1) {                            // ── FLYING TO A FLOWER ──
          const ddx = B.beeTx - B.x, ddz = B.beeTz - B.z;
          if (beeBloomAt(B.beeTx, B.beeTz) !== B.beeTy) { B.beeM = 0; B.beeRe = tb3 + BEE_LOOK_S; }   // the bloom is GONE (chopped, buried by snow, edited away) — re-checked every frame so a bee can never settle onto a memory of a flower
          else if (ddx * ddx + ddz * ddz < BEE_SIT_R * BEE_SIT_R) { B.beeM = 2; B.beeT = tb3 + BEE_SIT_S + Math.random() * BEE_SIT_J; }
          else if (tb3 > B.beeT) { B.beeM = 0; B.beeBanX = B.beeTx; B.beeBanZ = B.beeTz; B.beeBanT = tb3 + BEE_BAN_S; B.beeRe = tb3 + BEE_LOOK_S; }   // GAVE UP — and bans this exact column, or the very next look re-picks the flower it just failed to reach and the bee grinds forever on a lane the fan cannot solve
        } else if (B.beeM === 2) {                     // ── SITTING ── the pose is pinned below; here only the clock and the vanishing-flower check
          if (tb3 > B.beeT || beeBloomAt(B.beeTx, B.beeTz) !== B.beeTy) { B.beeM = 0; B.beeRe = tb3 + BEE_LOOK_S + Math.random() * BEE_LOOK_S;
          // ── AND IT LEAVES CARRYING POLLEN (user 2026-08-18) ── this is the ONE 2->0 transition, i.e. the exact
          // moment a bee stops sitting on a bloom, so it is where the trail is armed. A DEADLINE rather than a
          // one-shot emit, mirroring the duckling's B.cryTo: the grains have to keep coming as the bee flies
          // off, and the only place the model's world frame exists is the emit loop, so that is where they are
          // actually spawned (see the pollen block there).
          B.polTo = now + POL_MS; B.polNext = now; }
        } else if (B.beeM === 3) {                     // ── FLYING TO A HIVE ──
          const hdx = B.beeTx - B.x, hdz = B.beeTz - B.z, har = BEE_ORBIT_R * 1.6;
          if (hdx * hdx + hdz * hdz < har * har) { B.beeM = 4; B.beeT = tb3 + BEE_HIVE_S + Math.random() * BEE_HIVE_J; }
          else if (tb3 > B.beeT) { B.beeM = 0; B.beeHRe = tb3 + BEE_HIVE_GAP; B.beeRe = tb3 + BEE_LOOK_S; }
        } else if (B.beeM === 4) {                     // ── ORBITING ──
          // ── A SWARM BEE NEVER LEAVES (user 2026-08-18: "have the bees swarm much much closer to the beehive.
          // it looks like theyre swarming the tree") ── and the radius was never the problem. MEASURED at a hive:
          // three of the five swarm bees sat at td 5.9-6.5, exactly BEE_ORBIT_R, while the other two cycled
          // between 25 and 104 voxels. Those two were not foraging — they were in state 3, FLYING TO A HIVE,
          // over and over: this timeout dropped them out of the orbit, BEE_HIVE_GAP held them off, and the
          // return trip is what read as bees roaming the whole crown. A crown is ~114 voxels across, which is
          // exactly the scale of the wandering.
          // The bound is right for the OTHER three. It exists so "a hive can never capture a bee for the whole
          // session", which is a statement about the foraging population — the meadow must not empty. The five
          // swarm slots are the ones that exist to be AT the hive (see beeSwarm above, and the placement that
          // puts them there), so for them the bound is the bug and not the safeguard.
          // beeSwarm is a pure function of the slot number, so this cannot drift as slots recycle.
          if (tb3 > B.beeT && !beeSwarm) { B.beeM = 0; B.beeHRe = tb3 + BEE_HIVE_GAP; B.beeRe = tb3 + BEE_LOOK_S; }
        } else if (B.beeM === 5) {                     // ── ENRAGED ──
          // ── HOW THE RAGE ENDS, THREE WAYS, AND ONE OF THEM ALWAYS FIRES ── an attacker that chases forever
          // is a bug, and the bee already has the precedent for both halves of the answer: BEE_GIVE_S ends an
          // errand on a clock, FLY_LEASH ends a drift on a distance.
          //   * the CLOCK. BEE_RAGE_S seconds and it is over, whatever is happening — the same bound
          //     BEE_HIVE_S puts on the orbit, for the same reason: nothing captures a bee for a session.
          //   * the LEASH, measured from the HIVE and not from the bee, because a swarm defends a place. Get
          //     BEE_RAGE_LEASH from the wreck and they lose interest. This is the one the PLAYER controls.
          //   * DEATH. A dead player is not a target, and the swarm stands down rather than hovering over the
          //     game-over screen.
          // On the way out it re-arms rather than dropping straight back to idle: BEE_RAGE_GAP is exactly
          // BEE_RAGE_WIN, so by the time the bee could be recruited again every record that called it is
          // stale. Without that a bee that gave up on the leash would re-enter on the very next frame, exit
          // again, and flicker between the two for the life of the record. It also takes the ordinary hive
          // cooldown, so a bee that has just been driven off does not immediately fly back to the wreck.
          const rgx = P.x - B.beeRgX, rgz = P.z - B.beeRgZ;
          if (tb3 > B.beeT || dead || rgx * rgx + rgz * rgz > BEE_RAGE_LEASH * BEE_RAGE_LEASH) {
            B.beeM = 0; B.beeRgRe = tb3 + BEE_RAGE_GAP; B.beeHRe = tb3 + BEE_HIVE_GAP; B.beeRe = tb3 + BEE_LOOK_S;
          } else {
            // ── THE STING, ON THE COBRA'S MACHINERY ── same reach expression (the animal's own MAMFIT
            // footprint plus the player's half width and a little), same !dead && !P.fly gate, same one call
            // to vitHurt. Two deliberate differences, and both are about what a BEE is:
            //   * the vertical test is a BOX over the player's whole 20-voxel body, not the cobra's
            //     centre-height band. A cobra is on the ground with the player, so |P.y - B.y| is a fair
            //     question; a bee is in the AIR, and asking that of a flyer either excludes it entirely (the
            //     bug that kept the cobra from ever biting) or has to be widened until it means nothing.
            //   * the cooldown is the SWARM'S, not this bee's. See BEE_STING_CD — five private clocks on a
            //     five-point bar is the whole bar inside a second.
            const fB5 = MAMFIT.bee, sR5 = 5.0 + (fB5 ? fB5.hd : 2);
            const sdx = P.x - B.x, sdz = P.z - B.z;
            if (!P.fly && sdx * sdx + sdz * sdz < sR5 * sR5 && B.y > P.y - 3 && B.y < P.y + HEIGHT + 3 && tb3 > beeStingT) {
              beeStingT = tb3 + BEE_STING_CD;
              B.beeStings = (B.beeStings || 0) + 1;
              vitHurt(BEE_STING, 'the bees stung you');
            }
          }
        } else if (tb3 > B.beeRe) {                    // ── IDLE ── look for something to do. The hive outranks the flowers for the bees assigned to it: a bee at the hive is not out foraging.
          B.beeRe = tb3 + BEE_LOOK_S + Math.random() * BEE_LOOK_S;
          // ── …AND "OUTRANKS" NOW MEANS IT ALSO OUTRANKS THE COOLDOWN (user 2026-08-17: "still dont see
          // them doing this") ── the hive was only consulted while BEE_HIVE_GAP had expired, so during the
          // gap the very same look fell straight through to findBeeFlower and sent a swarm bee off on a
          // 10-second errand up to BEE_FLOWER_R away. MEASURED with the placement fixed and this branch
          // unchanged: the five swarm bees were at the hive, but only 1-3 of them were inside 40 voxels of
          // it at any moment — the rest were out at flowers, which is exactly "I still dont see them
          // swarming". So the hive is asked FIRST and unconditionally: a bee with one in reach either
          // flies to it or waits out its cooldown near it, and never leaves for a flower. It is one
          // beeHiveNear call, not two — `hvN` answers both questions.
          const hvN = beeSwarm ? beeHiveNear(B.x, B.z) : null;
          if (hvN && tb3 > B.beeHRe) { B.beeM = 3; B.beeTx = hvN.x; B.beeTy = hvN.y; B.beeTz = hvN.z; B.beeT = tb3 + BEE_GIVE_S; }
          else if (!hvN) {                             // no hive in reach — forage, which is the whole life of the three non-swarm slots and of any swarm bee whose hive has gone out of range
            const fl = findBeeFlower(B.hx !== undefined ? B.hx : B.x, B.hz !== undefined ? B.hz : B.z);
            if (fl && !(tb3 < B.beeBanT && fl.x === B.beeBanX && fl.z === B.beeBanZ)) { B.beeM = 1; B.beeTx = fl.x; B.beeTy = fl.y; B.beeTz = fl.z; B.beeT = tb3 + BEE_GIVE_S; }
          }
        }
      }
      // A bee on an errand drops out of the fly's high cruise lane: DES_FLY_UP holds a desert fly 16 voxels above
      // every other flyer's glide line, and a flower head stands ONE voxel off the ground. This is the only reason
      // the three altitude sites below take a variable instead of the constant — for the fly, and for a bee that
      // is merely wandering or circling a hive, it IS the constant.
      // …and an ENRAGED bee drops out of it for the same reason a foraging one does, only harder: DES_FLY_UP
      // floors a flyer at gAir + 22, and the player's box tops out at P.y + 20, so a bee left in the cruise
      // lane would hover permanently above the head of the person it is attacking and could never once be
      // inside the sting box. Zero, not a smaller number: the rage servo below aims it at P.y + BEE_RAGE_Y and
      // gAir + 6 is the floor it must not fight.
      // ── A LANDING LADYBUG IS EXEMPT FROM THE FLYER FLOOR ── MEASURED without this: all six sat permanently
      // in 'down' and never reached the deck. The floor CLIMBS at 34 voxels/s while the landing ease pulls at
      // 2.2 * (ground - y), so the two settle into an equilibrium ~20 voxels up and the arrival test
      // (|y - ground| < 0.5) can never fire. The floor is right for a flyer that is flying and simply does not
      // apply to one that is deliberately setting down, so it is suppressed for exactly those two states —
      // 'up' keeps it, which is what lifts the ladybug back into the lane when it takes off again.
      const lbDown = B.lbPh === 'down' || B.lbPh === 'land';
      // ── AND THE LADYBUG FLIES THE BUTTERFLY'S LANE, NOT THE FLY'S (user 2026-08-22: "the ladybug seems to
      // have a different pathfinding then the butterfly. [I want] the ladybug on the exact same path finding as
      // the butterfly") ── it needs desFly to FLY at all (that flag is what makes a desert-band species airborne
      // rather than a walker), but desFly also carries DES_FLY_UP, which exists to lift a housefly 16 voxels
      // above every other flyer's glide line. Zero here puts it on exactly the butterfly's altitude, and the
      // horizontal wander is already the shared kind-0 arbiter, so the two now steer identically.
      const flyUp = (desFly && !desLbug) ? ((desBee && (B.beeM === 1 || B.beeM === 2 || B.beeM === 5)) ? 0 : DES_FLY_UP) : 0;
      if (B.kind === 4) {                              // LILY PAD: slow drift on the water + constant free rotation; movement heading (mth) is independent of the visual spin
        if (tb3 > B.tRe) { B.mth += (Math.random() - 0.5) * 1.2; B.tRe = tb3 + 3 + Math.random() * 4; }
        if (!bfWater(B.x + Math.sin(B.mth) * 5, B.z + Math.cos(B.mth) * 5)) B.mth += 2.6 * dt;   // shore/dry ahead — curl the drift away
      } else if (B.kind === 2 && (armSlot || skunkSlot || porcSlot || flamSlot)) {   // ARMADILLO + SKUNK + PORCUPINE + FLAMINGO (user):   // ── THE FLAMINGO JOINS THE WALKERS (user 2026-08-18: "its the walking animation. it should play through all the frames") ── it had the slot band, the biome, the emit strip and the ground seat, but NOT this branch, so it fell through to the WORM machinery and ran worm logic. The frozen animation was the visible half of that: B.aframe is advanced INSIDE here, so it never moved. Every other place these three are named as a group now names it too — the keep radius, the spacing, the navmesh arbiter, the speed read, mamSlot and the stamp handedness — because a walker that is a walker in six places and a worm in the seventh is the shape of this whole bug. a continuous cardinal WALK on the forest floor — marches ~9 vox/s, turns only 90° (never diagonal), follows the terrain, breaks out of dead-ends (never spins). Editor behaviour, world terrain. (Same AI; the slot band picks the stamp model — porcupine is armadillo-like, skunk doubles on flee.)
        const DIRa = [[0, -1], [1, 0], [0, 1], [-1, 0]];
        const cardTh = (h) => Math.atan2(DIRa[h & 3][0], DIRa[h & 3][1]);
        const gcM = mamArb ? navWalkStand(B.x, B.z) : 0;   // the marcher's OWN travel surface, from the field — ONE number shared by its lookahead, its brake, its step test and its y servo, so all four agree on where the ground is
        const walkOK = (h) => { if (mamArb) return navReachWalk(B.x, B.z, cardTh(h), NAV_MSTA, gcM, NAV_MUP, NAV_MDN, NAV_MCLR) >= NAV_MSTA;   // ── THE WHOLE FIVE VOXELS, NOT THE POINT AT FIVE ── while the marcher walks a straight line the point probe sweeps the corridor for it, but the instant it TURNS, the new heading's first voxels have never been sampled at all: the point at 5 can be clear over a rock at 1, so the turn is approved and the marcher then walks into the rock and grinds. The DDA is the same one the brake and the step test use, so the three cannot disagree.
          const tx = B.x + DIRa[h & 3][0] * 5, tz = B.z + DIRa[h & 3][1] * 5, gA = bfSurf(tx, tz), cur = bfSurf(B.x, B.z);   // 5 vox ahead must be climbable LAND, clear over the body
          return gA > WL + 0.5 && (gA - cur) <= 3 && (cur - gA) <= 4 && !bfObstW(tx, gA + 2, tz) && !bfObstW(tx, gA + 4, tz); };   // STEP UP ≤3 blocks (raised from 2 — the ≤2-over-5-vox gate stalled every marcher on ordinary pine-forest slopes, user; matches the bunny's ±3 hopOK) + drop ≤4 so it doesn't get boxed at a ledge
        if (B.ah === undefined) { B.ah = ((Math.round(B.th / (Math.PI / 2)) % 4) + 4) % 4; B.aWhim = 0; B.aesc = 0; B.aTurnT = 0; }
        const adx = B.x - P.x, adz = B.z - P.z, ady = P.y - B.y;
        B.aflee = spooked(B) || (adx * adx + ady * ady + adz * adz) < (B.aflee ? 46 * 46 : 30 * 30);   // SPHERE flee — flying overhead won't spook it, but a hit does, at any range
        if (!walkOK(B.ah)) {                            // ── BLOCKED AHEAD ── stop, turn 90° in a COMMITTED direction (can't oscillate), spaced by a cooldown so it never spins
          if (tb3 > B.aTurnT) {
            if (walkOK(B.ah + 1) || walkOK(B.ah + 2) || walkOK(B.ah + 3)) {
              if (!B.aesc) B.aesc = walkOK(B.ah + 1) ? 1 : (walkOK(B.ah + 3) ? 3 : 1);
              B.ah = (B.ah + B.aesc) & 3; B.aTurnT = tb3 + 0.28;
            } else {                                     // BOXED IN — break out over a RELAXED step (down a ledge / over a gap), or respawn if truly islanded
              B.aesc = 0; let best = -1, bgv = -1e9;
              for (let h = 0; h < 4; h++) { const tx = B.x + DIRa[h][0] * 5, tz = B.z + DIRa[h][1] * 5, gA = mamArb ? navWalkStand(tx, tz) : bfSurf(tx, tz);
                if (walkFree(tx, tz) && gA > bgv) { bgv = gA; best = h; } }
              if (best >= 0) { B.ah = best; B.aTurnT = tb3 + 0.28; B.aRelax = 1.2; } else { if (B.sN) unstampWorm(B); B.init = false; continue; }   // arm a RELAXED-ADVANCE window: the picked heading fails walkOK by construction, so without this the creature faced its escape route but the walkOK-gated march never moved — the actual "stuck on terrain" bug (user). The wedge EXIT must unstamp, or the creature's voxels stay in W and in stampedIdx for good.
            }
          }
        } else {                                         // ── CLEAR AHEAD ── walk; occasionally re-head (flee away from the player / random whim)
          B.aesc = 0;
          if (B.aflee) { let best = B.ah, bd = -1e9; for (let h = 0; h < 4; h++) if (walkOK(h)) { const d = DIRa[h][0] * adx + DIRa[h][1] * adz; if (d > bd) { bd = d; best = h; } } if (best !== B.ah && tb3 > B.aTurnT) { B.ah = best; B.aTurnT = tb3 + 0.28; } }
          else if (B.hx !== undefined && (B.x - B.hx) * (B.x - B.hx) + (B.z - B.hz) * (B.z - B.hz) > 60 * 60) {   // LEASH: marched too far from its RESERVED home cell → head back (same 60-vox leash as the bunny), so armadillos/skunks stay evenly spread and never drift together into a bunch (user)
            if (tb3 > B.aTurnT) { const hx = B.hx - B.x, hz = B.hz - B.z; let best = B.ah, bd = -1e9;
              for (let h = 0; h < 4; h++) if (walkOK(h)) { const d = DIRa[h][0] * hx + DIRa[h][1] * hz; if (d > bd) { bd = d; best = h; } }
              if (best !== B.ah) { B.ah = best; B.aTurnT = tb3 + 0.28; } } }
          else if (tb3 > B.aWhim) { B.aWhim = tb3 + 1 + Math.random() * 0.6; if (Math.random() < 0.25) { const t = Math.random() < 0.5 ? 1 : 3; if (walkOK(B.ah + t)) { B.ah = (B.ah + t) & 3; B.aTurnT = tb3 + 0.28; } } }   // each ~1-1.6 s step: 75% keep walking forward / 25% rotate 90° (50/50 left/right) — user
        }
        // …AND THE MOVEMENT WITH IT. The flamingo fell into the bare `9` here — the only one of these four with
        // no flee pace at all, so being hit changed its legs and not its ground speed, which is the half of the
        // report that mattered. Doubled to 18, exactly as the porcupine's 9 -> 18 and the skunk's 24 -> 48 do,
        // so the three now share one rule rather than the flamingo being the exception nobody wrote down.
        // Eased on the same ramp below, so a hit accelerates it rather than teleporting it to full speed.
        const spdTgt = skunkSlot ? (B.aflee ? 48 : 24) : (porcSlot ? (B.aflee ? 18 : 9) : (flamSlot ? (B.aflee ? 36 : 9) : 9));
        // ── AND THE FLAMINGO'S FLEE IS SET AGAINST WHAT IT ACHIEVES, NOT WHAT IT IS ASKED FOR (user 2026-08-20:
        // "when the flamingo enters 48 fps mode, it doesnt appear to move faster") ── the 18 above was already
        // double the 9 base and the animation rate is on the SAME flag and the SAME dt*6 ramp, so nothing was out
        // of step. MEASURED though, a flamingo walks 4.5 vox/s and flees at 9.0: exactly 2x, and exactly HALF of
        // both targets, because navBrake2 gives up about half the march threading between blossom trunks. A bird
        // 'fleeing' at 9 is moving at its own quoted walking pace, which is why the 48 fps legs read as frantic
        // over a body that ambles. 36 is what makes the ACHIEVED flee 18 — twice the 9 it is supposed to walk at.
        // The porcupine keeps 18: it is not in the blossom, it does not pay that toll, and it already bolts.   // SKUNK: 24→48 on flee. PORCUPINE: 9→18 — DOUBLE the pace when the player is near (user). ARMADILLO (else): constant 9. All eased via B.aspd below.
        B.aspd = (B.aspd === undefined) ? spdTgt : B.aspd + (spdTgt - B.aspd) * Math.min(1, dt * 6);   // ease the pace toward the target (bunny-style ramp) so the speed-up/-down isn't an instant snap
        B.th = cardTh(B.ah);                             // HOISTED from below the advance — nothing between the two positions ever read B.th, so the non-arbiter path is unchanged; the brake needs the heading the marcher is ACTUALLY walking this frame, not last frame's
        if (walkOK(B.ah)) {
          if (mamArb) { navMoveK[7]++;                   // row 7 is the LAND MAMMAL row: this band shares B.kind 2 with the worm, and one row for both would let a landing in either hide inside the other's total
            let mvM = B.aspd * dt;
            if (NAVBRK) mvM = navBrake2(B, (th7, L7) => navReachWalk(B.x, B.z, th7, L7, gcM, NAV_MUP, NAV_MDN, NAV_MCLR), mvM, dt, NAV_MLOOK, NAV_MBCLR, NAV_MBRK2, 7);   // ── THE BRAKE ── the lookahead above already refuses a lane under 5 voxels, so the cap only ever ramps between 5 and 8 and the 9 vox/s armadillo never feels it at all; what it removes is the fleeing skunk arriving at a turn at 48 vox/s and hard-stopping for a frame
            const nxM = B.x + DIRa[B.ah][0] * mvM, nzM = B.z + DIRa[B.ah][1] * mvM;
            if (navWalkOK(nxM, nzM, gcM, NAV_MUP, NAV_MDN, NAV_MCLR)) { B.x = nxM; B.z = nzM; }   // THE SAME answer the lookahead and the brake scored with, asked of the actual step
            else { navRejK[7]++;
              if (!navWalkOK(B.x, B.z, gcM, NAV_MUP, NAV_MDN, NAV_MCLR)) { B.x = nxM; B.z = nzM; navEgrK[7]++; } }   // ── EGRESS ── the marcher is already OUTSIDE the travelable set (a tree fell on it, it streamed in beside rock). Refusing the step would pin it there until the watchdog respawns it; it walks out along its heading instead. Displacement, never a teleport.
            B.aRelax = 0; }
          else { B.x += DIRa[B.ah][0] * B.aspd * dt; B.z += DIRa[B.ah][1] * B.aspd * dt; B.aRelax = 0; }   // advance ONLY when clear → never walks into a rock/water (clear again → drop any relaxed window). CHARACTER FOR CHARACTER the original expression: `DIRa[..] * mvM` is not `DIRa[..] * B.aspd * dt`, float multiply does not reassociate, and routing the control build through the arbiter's variable would move its step by an ULP a frame and re-roll the whole soak.
        }
        else if ((B.aRelax || 0) > 0) { B.aRelax -= dt;   // BOXED-IN ESCAPE (timed): advance under the SAME relaxed gate the escape was picked with — land + body-clear, step limit ignored — so it actually climbs/drops out of the pocket instead of facing the exit forever
          const tx = B.x + DIRa[B.ah][0] * 5, tz = B.z + DIRa[B.ah][1] * 5;
          if (walkFree(tx, tz)) { B.x += DIRa[B.ah][0] * B.aspd * dt; B.z += DIRa[B.ah][1] * B.aspd * dt; } }
        if (B.aAncX === undefined || (B.x - B.aAncX) * (B.x - B.aAncX) + (B.z - B.aAncZ) * (B.z - B.aAncZ) > 2.5 * 2.5) { B.aAncX = B.x; B.aAncZ = B.z; B.aStk = 0; B.aStkN = 0; }   // ── STEEP-TERRAIN STUCK WATCHDOG (user) ── judge by NET DISPLACEMENT, not blockage: a marcher can pass every walkOK test yet oscillate in place at a slope base (leash pulls uphill → blocked → contour turn → pulled back…). Real walking resets the anchor constantly; only genuine pinning accumulates.
        else if ((B.aStk = (B.aStk || 0) + dt) > 3 && !(B.aRelax > 0)) {
          B.aStk = 0; B.aStkN = (B.aStkN || 0) + 1;
          let best = -1, bgv = -1e9;                     // force the RELAXED escape (step limit ignored — same gate as boxed-in) toward the most climbable neighbour
          for (let h = 0; h < 4; h++) { const tx = B.x + DIRa[h][0] * 5, tz = B.z + DIRa[h][1] * 5, gA = mamArb ? navWalkStand(tx, tz) : bfSurf(tx, tz);
            if (walkFree(tx, tz) && gA > bgv) { bgv = gA; best = h; } }
          if (best < 0 || B.aStkN >= 3) { if (B.sN) unstampWorm(B); B.init = false; continue; }   // three failed escapes (or nowhere to go) = truly wedged → respawn on fresh ground
          B.ah = best; B.aTurnT = tb3 + 0.28; B.aRelax = 1.4;
        }
        B.animClk = (B.animClk || 0) + dt;   // armadillo walk clock (24 fps, read by stampArmadillo)
        // ── A STRUCK FLAMINGO BOLTS (user 2026-08-19: "have the flamingo double in speed. not only in fps
        // (should be 48) but also in movement") ── 48 is the user's own number and it is DOUBLE the 24 fps house
        // rate rather than double this bird's own 12: a wading flamingo is deliberately half-rate, and doubling
        // that to 24 was the old flee and is what reads as too slow to be a bolt.
        if (skunkSlot || porcSlot || flamSlot) { const fps = (B.aflee ? (flamSlot ? 48 : 24) : 12) * (skunkSlot ? SKUNK_ANIM_MUL : 1); B.afps = (B.afps === undefined) ? fps : B.afps + (fps - B.afps) * Math.min(1, dt * 6); B.aframe = ((B.aframe || 0) + dt * B.afps) % Math.max(1, skunkSlot ? SKUNK_WALK.length : (porcSlot ? PORCUPINE_WALK.length : FLAMINGO_NFRAMES)); }   // ── AND THE FLAMINGO (user 2026-08-18: "its not playing through its frames") ── fi3 was already reading B.aframe for it, but nothing ADVANCED that clock, so it read a permanent 0 and the bird stood in frame 0. The modulus is its own frame count, and Math.max(1, ...) keeps a species whose art failed to load from dividing by zero   // SKUNK + PORCUPINE (user): ease 12↔24 fps (DOUBLE the walk cycle when fleeing, bunny-style) on a frame-position clock so switching rate never jumps a frame; read by stampSkunk/stampPorcupine   // ── SKUNK AT HALF SPEED (user 2026-08-06) ── 6 fps walking, 12 fleeing; it shared this line with the porcupine so the rate is SPLIT, not halved for both. The 24 fps house rule still holds everywhere else.
        B.om = 0; B.omT = 0; B.bspd = 0;                 // kill the shared glide/steering — the march above is the motion. Alignment lives in the GRID-STAMP poses (armOffset baked in), not a render offset — the armadillo grid-stamps again (user).
      } else if (B.kind === 2 && bunnySlot) {          // BUNNY (user): its MOTION IS THE BAKED ANIMATION — I apply no glide or turn of my own. The jump bake carries the −6 forward march; the rotate bake carries the 90° yaw. I only choose which sequence to play + follow the terrain. Cardinal grid, editor-identical.
        const DIRb = [[0, -1], [1, 0], [0, 1], [-1, 0]];
        const cardTh = (h) => Math.atan2(DIRb[h & 3][0], DIRb[h & 3][1]);
        const rot2b = (ox, oz, h) => { let x = ox, z = oz; for (let k = 0; k < (h & 3); k++) { const nx = -z, nz = x; x = nx; z = nz; } return [x, z]; };
        const pad2b = (n) => (n < 10 ? '0' + n : '' + n) + '.vox';
        const hopOK = (h) => { const dx = DIRb[h & 3][0], dz = DIRb[h & 3][1];   // check ALONG the whole −6 hop path (2,4,6 ahead), not just the landing — a trunk/rock the lunge would pass THROUGH must block it too (user: stuck against trees/terrain)
          for (let dd = 2; dd <= 6; dd += 2) { const tx = B.bpx + dx * dd, tz = B.bpz + dz * dd;
            if (mamArb) { if (!navWalkOK(tx, tz, B.bg, 3, 3, NAV_MCLR)) return false; continue; }   // the SAME three stations, asked of the field. Not the reach DDA the marchers use: a hop is ONE ballistic commitment from B.bg, so every station is judged against the TAKE-OFF ground (±3), never carried forward — carrying it would approve a 9-voxel climb the baked animation cannot make.
            const gA = navWalkStand(tx, tz);          // …and the SAME surface off the arbiter (user 2026-08-07): the hop is planned against whatever number the body will be SEATED on, or the bunny commits to an arc measured on the raw heightmap and lands on a nav-field surface up to 3 voxels elsewhere — the planner/mover split this project keeps rediscovering
            if (!(gA > WL + 0.5 && Math.abs(gA - B.bg) <= 3 && !bfObstW(tx, gA + 2, tz) && !bfObstW(tx, gA + 4, tz))) return false; }
          return true; };
        if (B.bh === undefined) { B.bh = ((Math.round(B.th / (Math.PI / 2)) % 4) + 4) % 4; B.bst = 0; B.bfclk = 0; B.bpx = B.x; B.bpz = B.z; B.bg = navWalkStand(B.x, B.z); B.bWhim = 0; B.bfps = 24; B.besc = 0; }   // bpx/bpz = base cell; bst: 0 jump | 1 rotate-left | 2 rotate-right; besc = committed escape-turn direction
        const NFR = 11;                                 // frames per baked sequence (00-10)
        const adx = B.bpx - P.x, adz = B.bpz - P.z, ady = P.y - (B.bg !== undefined ? B.bg : B.y);   // ady = vertical gap → the flee radius is now a SPHERE, not a ground circle: flying high overhead no longer spooks it (user)
        B.bflee = spooked(B) || (adx * adx + ady * ady + adz * adz) < (B.bflee ? 55 * 55 : 40 * 40);   // …or it has just been hit, which spooks it wherever the player is standing
        B.bfps += ((B.bflee ? 48 : 24) - B.bfps) * Math.min(1, dt * 6);   // ease the PLAY SPEED (flee = 2×) — the only thing I vary; the animation does the moving
        B.bfclk += dt * B.bfps;
        if (B.bfclk >= NFR) { B.bfclk -= NFR;           // a baked sequence just finished → commit its motion, pick the next
          if (B.bst === 0) { const [dx, dz] = rot2b(0, -6, B.bh); B.bpx += dx; B.bpz += dz; B.bg = navWalkStand(B.bpx, B.bpz); if (mamArb) navMoveK[7]++; }   // jump baked −6 forward → advance the base cell. The bunny's ground anchor comes from the SAME plane its hop gate reads, or the gate would measure ±3 from a surface the creature is not standing on.
          else B.bh = (B.bst === 1 ? B.bh + 1 : B.bh + 3) & 3;   // rotate baked a 90° yaw → commit the new cardinal
          if (B.bAncX === undefined || (B.bpx - B.bAncX) * (B.bpx - B.bAncX) + (B.bpz - B.bAncZ) * (B.bpz - B.bAncZ) > 5 * 5) { B.bAncX = B.bpx; B.bAncZ = B.bpz; B.bStkT = tb3; B.bStkN = 0; }   // ── STEEP-TERRAIN STUCK WATCHDOG (user) ── same net-displacement rule as the marchers: hopping around normally moves the base cell and resets the anchor; only real pinning ages the clock
          const bForce = tb3 - (B.bStkT === undefined ? tb3 : B.bStkT) > 4;
          if (bForce) { B.bStkT = tb3; B.bStkN = (B.bStkN || 0) + 1; }
          if (bForce || !hopOK(B.bh)) {                  // ── OBSTACLE AHEAD (user) ── (or the watchdog fired — then skip the polite turn and break out)
            if (!bForce && (hopOK(B.bh + 1) || hopOK(B.bh + 2) || hopOK(B.bh + 3))) {   // a clean cardinal EXISTS → commit ONE turn toward it, persisted so the escape can't oscillate (clears a corner in one turn, a dead-end in two)
              if (!B.besc) B.besc = hopOK(B.bh + 1) ? 1 : (hopOK(B.bh + 3) ? 2 : 1);
              B.bst = B.besc;
            } else {                                     // BOXED IN — no clean hop in ANY direction. THIS is what made it turn forever; break out instead of spinning (user: "gets stuck and rotates endlessly")
              B.besc = 0;
              let best = -1, bgv = -1e9;                 // the most climbable LAND neighbour, IGNORING the ±3 step limit → hop DOWN a ledge / over a small gap to escape the pocket
              for (let h = 0; h < 4; h++) { const tx = B.bpx + DIRb[h][0] * 6, tz = B.bpz + DIRb[h][1] * 6, gA = mamArb ? navWalkStand(tx, tz) : bfSurf(tx, tz);
                if (walkFree(tx, tz) && gA > bgv) { bgv = gA; best = h; } }   // the THIRD hand-rolled copy of the relaxed gate, now the shared walkFree — this one used to read the escape under a rule of its own while the two marcher escapes read another
              if (best >= 0 && (B.bStkN || 0) < 3) { B.bh = best; B.bst = 0; }   // snap to face the escape + HOP out (the vertical snap is worth breaking the spin)
              else if (mamArb && (B.bStkN || 0) < 3 && !navWalkOK(B.bpx, B.bpz, B.bg, 3, 3, NAV_MCLR)) { B.bst = 0; navEgrK[7]++; }   // ── EGRESS ── nothing around it is free AND its own cell is outside the travelable set: it is boxed in by terrain that arrived after it did. Hop along the heading rather than respawn — displacement, never a teleport, which is the whole difference between the arbiter's escape and the mercy recycle.
              else { if (B.sN) unstampWorm(B); B.init = false; continue; }   // truly islanded (only water/void within reach) OR three watchdog escapes went nowhere → respawn on fresh land
            }
          } else {                                       // ── CLEAR AHEAD ── escape done; resume normal behaviour
            B.besc = 0;
            if (B.bflee) { let best = B.bh, bd = -1e9; for (let h = 0; h < 4; h++) { const d = DIRb[h][0] * adx + DIRb[h][1] * adz; if (d > bd) { bd = d; best = h; } } const diff = (best - B.bh + 4) & 3; B.bst = diff === 0 ? 0 : diff === 3 ? 2 : 1; }   // face away from the player
            else if (B.hx !== undefined && (B.bpx - B.hx) * (B.bpx - B.hx) + (B.bpz - B.hz) * (B.bpz - B.hz) > 60 * 60) {   // LEASH: wandered too far from its RESERVED home cell → head back, so bunnies stay evenly spread and don't bunch up (user)
              const hx = B.hx - B.bpx, hz = B.hz - B.bpz; let best = B.bh, bd = -1e9; for (let h = 0; h < 4; h++) { const d = DIRb[h][0] * hx + DIRb[h][1] * hz; if (d > bd) { bd = d; best = h; } }
              const diff = (best - B.bh + 4) & 3; B.bst = diff === 0 ? 0 : diff === 3 ? 2 : 1; }
            else if (tb3 > B.bWhim) { B.bWhim = tb3 + 1.6 + Math.random() * 3; B.bst = Math.random() < 0.75 ? 0 : (Math.random() < 0.5 ? 1 : 2); }   // editor odds: 75% hop, 25% turn
            else B.bst = 0;
          }
        }
        const fi = Math.min(NFR - 1, Math.floor(B.bfclk));
        const bake = B.bst === 0 ? BUNNY_JUMP_BAKE : (B.bst === 1 ? BUNNY_ROT_BAKE : BUNNY_ROT_BAKE_R);
        const off = bake[pad2b(fi)] || [0, 0, 0];
        const [lx, lz] = rot2b(off[0] || 0, off[2] || 0, B.bh);   // THIS FRAME's baked lunge, rotated into the heading (rotate bakes have oz=0 → in place)
        B.x = B.bpx + lx; B.z = B.bpz + lz; B.bOy = off[1] || 0;   // baked bob (oy) is lifted in the emit
        B.th = (B.bst !== 0 && off[3] && off[3].length) ? cardTh(B.bst === 1 ? B.bh + 1 : B.bh + 3) : cardTh(B.bh);   // the baked 90° yaw sits on frames 6-10 → the facing SNAPS there, exactly like the editor (no manual interp)
        B.om = 0; B.omT = 0; B.bspd = 0; B.animClk = B.bfclk / 24;   // kill the shared glide/steering; animClk (=frames/24) feeds the emit's frame index
      } else if (antLead) {                          // ── THE ANT MARCHES ON THE COMPASS (user 2026-08-16: "just have it move forward. when it want to turn, rotate the ant 90 degrees") ──
        // The leader's whole controller, and it REPLACES the worm arbiter's scored heading fan for this one
        // slot rather than post-processing it. Rounding the fan's output was tried on paper and cannot work:
        // the fan re-rolls a wander of ±0.7 rad ABOUT THE CURRENT HEADING, which is narrower than the 0.785
        // a quarter turn needs, so a quantised fan marches dead straight forever — and widening it to reach
        // the boundary puts the ant back to flickering across it. An INTEGER heading is the honest way to say
        // "cardinal": B.ah indexes ANT_DIR, a turn is ±1 on it, and a diagonal is not expressible.
        // The MOVER below is deliberately untouched — it still translates along B.th and still gates the step
        // on navLandOK — so the ant keeps the band's brake, step rule and egress. Only the choosing changed.
        B.animClk = (B.animClk || 0) + dt;
        if (B.ah === undefined) { B.ah = ((Math.round(B.th / (Math.PI / 2)) % 4) + 4) % 4; B.aTurnT = 0; B.aesc = 0; B.aWhim = tb3 + ANT_WHIM; }   // the spawn heading, snapped to the nearest cardinal once
        const antOK = (h) => { const tx = B.x + ANT_DIR[h & 3][0] * ANT_LOOK, tz = B.z + ANT_DIR[h & 3][1] * ANT_LOOK, gA = bfSurf(tx, tz);
          return gA > WL + 0.5 && Math.abs(gA - bfSurf(B.x, B.z)) <= 2 && !bfObstW(tx, gA + 2, tz) && !bfObstW(tx, gA + 3, tz); };   // the WORM'S OWN step rule, asked ANT_LOOK voxels down a cardinal — so a heading the ant picks is one the mover will accept
        const antTurn = (d) => {                       // ── THE QUARTER TURN ── instant (one frame), and it pins a crumb AT THE CORNER first…
          if (B.trail && B.trail.length) B.trail.unshift({ x: B.x, z: B.z, th: B.th });   // …so the recorded path stays a polyline of exactly axis-aligned segments: every follower then turns on the same spot the leader did instead of cutting the corner across one 0.75-voxel diagonal chord
          B.ah = (B.ah + d) & 3; B.aTurnT = tb3 + ANT_TURN_HOLD;
        };
        if (!antOK(B.ah) || B.antBlk) {                // BLOCKED — either the lookahead refuses the lane, or the mover refused the actual step last frame (the backstop: the two predicates are close but not identical, and an ant that cannot move must never just grind)
          if (tb3 > B.aTurnT) {                        // commit to ONE side and hold it for ANT_TURN_HOLD, exactly as the marching mammals do, so a corner can never become a spin
            if (!B.aesc) B.aesc = antOK(B.ah + 1) ? 1 : (antOK(B.ah + 3) ? 3 : 1);
            antTurn(B.aesc);
          }
        } else {
          B.aesc = 0;                                  // clear again — the next block gets a fresh choice of side
          if (tb3 > B.aWhim) { B.aWhim = tb3 + ANT_WHIM + Math.random() * ANT_WHIM;
            if (Math.random() < ANT_WHIM_P) { const t = Math.random() < 0.5 ? 1 : 3; if (antOK(B.ah + t)) antTurn(t); } }   // …and an unblocked ant still turns now and then, 50/50 left or right, but only onto a lane it can actually walk
        }
        B.antBlk = 0;
        B.th = Math.atan2(ANT_DIR[B.ah][0], ANT_DIR[B.ah][1]);   // the ONE writer of B.th for this ant, and always exactly a cardinal — so the mover's step, the render yaw and every crumb it records are axis-aligned by construction
        B.om = 0; B.omT = 0;                           // kill the eased turn integrator: it is the continuous controller this branch exists to replace, and any non-zero om would drift B.th back off the compass one frame later
      } else if (B.kind === 2 && NAVARB) {              // ── WORM, ON THE ARBITER ── the crawl had FOUR writers of B.th: a random wander, a keep-apart push, a two-probe obstacle turn, and the home leash further down. Three of them used bfSurf — the HEIGHTMAP — while the mover's step rule used it at a different distance, so the planner could commit to a heading the mover then refused every frame, which is the whole of the 6 s/creature-min the band was losing. One scored fan at 12 Hz on ONE predicate replaces all four.
        B.animClk = (B.animClk || 0) + dt;
        if (tb3 > B.tRe) { B.navWander = B.th + (Math.random() - 0.5) * 1.4; B.tRe = tb3 + 1.5 + Math.random() * 2.5; }   // the WANDER survives as a score term instead of a direct omT write. Same two RNG draws at the same cadence as the line it replaces, so a seeded A/B still lines up.
        if (B.navRe === undefined) B.navRe = tb3 + (wk & 31) * 0.0026;   // stagger the sense tick across the band so 32 worms never think on the same frame
        if (tb3 >= B.navRe) {
          B.navRe = tb3 + NAV_HZ;
          let gx8 = 0, gz8 = 0;                          // ── ONE GOAL VECTOR ── the keep-apart push and the home leash, resolved into a single direction BEFORE the fan. They used to be two separate writers, one of them steering the worm straight into the trunk the other was trying to avoid.
          for (let j = WORM_0; j < WORM_END; j++) { const O = wbf[j]; if (O === B || !O.init || (O.kind | 0) !== 2) continue;
            const dxw = B.x - O.x, dzw = B.z - O.z, d2w = dxw * dxw + dzw * dzw;
            if (d2w < 26 * 26 && d2w > 0.01) { const il = 1 / Math.sqrt(d2w); gx8 += dxw * il * (26 - Math.sqrt(d2w)) / 26; gz8 += dzw * il * (26 - Math.sqrt(d2w)) / 26; } }   // unit vectors, weighted by closeness — the old raw sum let one very near worm swamp the leash entirely
          if (B.hcx !== undefined && (B.trap || 0) <= 0.35) { const lx = B.x - B.hx, lz = B.z - B.hz, l2 = lx * lx + lz * lz;   // STUCK (trap > 0.35): drop the pull toward home so the fan can steer AROUND the trunk instead of into it — the same exemption the direct leash carried
            if (l2 > WORM_LEASH * WORM_LEASH) { const il = 1.6 / Math.sqrt(l2); gx8 -= lx * il; gz8 -= lz * il; } }
          // ── AND THE FLEE / HUNT BEARING IS A GOAL, NOT AN OVERRIDE (user 2026-08-17: life "is also getting
          // stuck on objects. cactus for example") ── it used to be written straight into B.omT further down,
          // AFTER this fan, with no terrain in it at all: inside DES_DASH_R a gecko was pointed dead away from
          // the player, and where a cactus trunk stood on that bearing the brake clamped its step to exactly
          // zero and held it there — a few centimetres of travel over 2.3 s in the user's clip. That is the same
          // defect the keep-apart push and the home leash had before they were folded in above, so it is folded
          // in the same way: one more contributor to the ONE goal vector. Same bearing and the same two radii
          // the overrides carried, but the fan must now satisfy "away from the player" AND "down an open lane"
          // at once, which is what sends the animal AROUND the trunk instead of into it.
          if (desSlot && DESERTS[desSp]) {
            const hunt8 = !!DES_HUNT[DESERTS[desSp].name], run8 = !!DES_DASH[DESERTS[desSp].name] && !hunt8;   // ant, spider and every unlisted species are in neither table, so their goal vector stays empty and their plan is bit-identical
            const px8 = hunt8 ? P.x - B.x : B.x - P.x, pz8 = hunt8 ? P.z - B.z : B.z - P.z;
            const pd8 = px8 * px8 + pz8 * pz8, pr8 = hunt8 ? 90 : DES_DASH_R;
            if ((hunt8 || run8) && pd8 < pr8 * pr8 && pd8 > 0.01) { const il8 = 1 / Math.sqrt(pd8); gx8 += px8 * il8; gz8 += pz8 * il8; } }   // a UNIT vector like the keep-apart terms: navSteer2 reads only the DIRECTION, and no other term writes this vector for the desert band, so the length is free
          const gOn = gx8 * gx8 + gz8 * gz8 > 0.01;
          const gc8 = navGroundAt(B.x, B.z);
          // Only a walker standing on its OWN ground clips against the line — one already stranded the wrong side
          // must be free to walk home, and the near-edge window keeps the extra mask samples off the 90-odd
          // percent of the band that is nowhere near a boundary. The window is far wider than a plan tick of
          // travel, so a walker is always inside it well before the line is within reach.
          // Both halves come off the body's HOME TAG now rather than off desSlot: the same two samples and the
          // same two comparisons for every creature that was here before, and the only band that reads a
          // different line is the desert mouse's oak-forest population (bioHomeOK / bioNearEdge, sim/nav.js).
          const bioOn8 = bioHomeOK(bioMe, B.x, B.z) && bioNearEdge(bioMe, B.x, B.z);
          navSteer2(B, navLandOK(B.x, B.z, gc8, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot),
            (th7, L7) => { const r7 = navReachLand(B.x, B.z, th7, L7, gc8, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot);
              return bioOn8 ? navBioClip(B.x, B.z, th7, r7, bioMe) : r7; },
            NAV_WREACH, 2 * NAV_WN, gOn ? Math.atan2(gx8, gz8) : 0, gOn, 2.6, 2.4);   // the same ±2.6 clamp and 2.4 gain the keep-apart push used, so the crawl's turn character is unchanged
        }
      } else if (B.kind === 2) {                       // WORM: smooth continuous meandering crawl (the random pause was removed — user)
        B.animClk = (B.animClk || 0) + dt;
        // ── ANT LINES (user 2026-08-15) ── the duckling follow-chain, reused whole. Slot 0 of a species is the
        // leader and 1..DES_PER-1 heel behind it, each steering for a point just behind the one AHEAD rather
        // than all at the leader — a chain, not a star — and falling back up the chain when a link dies. This
        // works here only because two things do NOT apply to the desert band: the keep-apart repulsion below
        // scans the WORM band so ants never shove each other out of line, and the home leash is gated on
        // B.hcx, which the annulus spawn never sets for them. Either would have fought the formation.
        const wormOK = (th6, dist) => { const ax = B.x + Math.sin(th6) * dist, az = B.z + Math.cos(th6) * dist, gA = bfSurf(ax, az);
          return gA > WL + 0.5 && Math.abs(gA - bfSurf(B.x, B.z)) <= 2 && !bfObstW(ax, gA + 2, az) && !bfObstW(ax, gA + 3, az); };   // ≤2 matches the STEP rule; +3 probe = body TOP clearance (bfObstW passes small ground clutter)
        if (tb3 > B.tRe) { B.omT = (Math.random() - 0.5) * 1.4; B.tRe = tb3 + 1.5 + Math.random() * 2.5; }
        let wrx = 0, wrz = 0;                           // SPREAD OUT — steer away from any nearby worm so they never bunch up (user); placement is already ≥60 apart, this stops runtime clumping
        for (let j = WORM_0; j < WORM_END; j++) { const O = wbf[j]; if (O === B || !O.init || (O.kind | 0) !== 2) continue;
          const dxw = B.x - O.x, dzw = B.z - O.z, d2w = dxw * dxw + dzw * dzw;
          if (d2w < 26 * 26 && d2w > 0.01) { const w = 26 - Math.sqrt(d2w); wrx += dxw * w; wrz += dzw * w; } }
        if (wrx * wrx + wrz * wrz > 1) {                // a worm is within ~2.6 m — bias the heading toward the away direction
          let dthw = Math.atan2(wrx, wrz) - B.th; dthw = Math.atan2(Math.sin(dthw), Math.cos(dthw));
          B.omT = Math.max(-2.6, Math.min(2.6, dthw * 2.4));
        }
        if (!wormOK(B.th, 5) || !wormOK(B.th, 2.5)) {  // unclimbable ahead (rock face / shoreline / cliff) — commit to a firm turn toward the open side (obstacle avoidance wins over the spread bias)
          const pF = wormOK(B.th + 1.1, 5), nF = wormOK(B.th - 1.1, 5);
          B.omT = pF && !nF ? 2.6 : (nF && !pF ? -2.6 : (B.om >= 0 ? 2.6 : -2.6));
        }
      } else if (B.kind === 3) {                       // DUCK: paddles its lake — veer away from the shore, gentle drifting turns
        // ── AND IT LEAVES A WAKE ── the same rings the player and every splash push (ripAdd, world/window.js),
        // emitted per RIP_STEP voxels TRAVELLED. Keyed on the distance moved since the last one rather than on
        // the paddle speed, so it does not care where in this branch the move actually happens, and a duck
        // sitting still leaves nothing. Ducks only: they are the one creature that swims ON the surface —
        // a fish is under it, and a ring on the water from something two metres down would be wrong.
        if (B.init) {
          if (B.wkx === undefined) { B.wkx = B.x; B.wkz = B.z; }
          const dkx = B.x - B.wkx, dkz = B.z - B.wkz;
          const dkS = RIP_STEP * 2.2;              // …a LONGER stride than the player's: a duck is a fraction of the size and leaves a correspondingly shorter tail, and with a whole brood on one lake the shared ring list is what pays for a stride that is too short
          if (dkx * dkx + dkz * dkz >= dkS * dkS) { B.wkx = B.x; B.wkz = B.z; ripAdd(B.x, B.z, isBaby ? 0.22 : 0.38); }
        }
        if (isBaby && !orphan) {                       // DUCKLING: steer for a spot just behind its leader — mom for the first, the sibling ahead for the rest (a follow line)
          const lead = (sib === 0 || !wbf[wk - 1].init) ? mom5 : wbf[wk - 1];
          const tx = lead.x - Math.sin(lead.th) * 4.5, tz = lead.z - Math.cos(lead.th) * 4.5;
          let dth = Math.atan2(tx - B.x, tz - B.z) - B.th; dth = Math.atan2(Math.sin(dth), Math.cos(dth));
          B.omT = Math.max(-2.8, Math.min(2.8, dth * 2.2));
          B.chase = Math.hypot(tx - B.x, tz - B.z);    // distance to heel — drives the paddle speed
        } else {
          if (orphan) B.chase = 4;                     // …an ORPHAN has no line to hold, so it paddles the lake on its own: the edge-avoidance wander below, at the steady 7 the speed table gives for a chase over 3.5 (never the 10 of a duckling scrambling to catch up)
          const duckOK = (th6, dist) => duckFit(B.x + Math.sin(th6) * dist, B.z + Math.cos(th6) * dist);   // …and the lookahead is now literally the move rule: ONE function, so the planner cannot pick a lane the mover refuses
          let rx = 0, rz = 0;                          // PROACTIVE EDGE AVOIDANCE (user: 'keep the ducks further away from the terrain'): a stronger, WIDER repulsion so the duck banks off well before it can touch a shore/rock
          for (let k = 0; k < 8; k++) { const a = k * 0.785398, sa = Math.sin(a), ca = Math.cos(a);
            for (let d = 4; d <= 15; d += 3) { if (!bfWater(B.x + sa * d, B.z + ca * d)) { rx -= sa * (17 - d); rz -= ca * (17 - d); break; } } }   // sees banks out to 15 vox; closer edge → stronger push. This one stays on the bare water probe on purpose: it is a SOFT term that only bends the heading, stepOK re-asks the full question anyway, and a 33-voxel sky scan inside an 8×4 loop is ~1.7M voxel reads a second for nothing.
          if (rx * rx + rz * rz > 0.02) {              // a bank is within ~15 vox — ease the heading toward the resultant OPEN-water direction
            let dth = Math.atan2(rx, rz) - B.th; dth = Math.atan2(Math.sin(dth), Math.cos(dth));
            // If it has already wound up most of a turn, prefer unwinding the long way round to the
            // same heading rather than completing the loop.
            if (Math.abs(B.turnAcc || 0) > 4.0 && Math.sign(dth) === Math.sign(B.turnAcc)) dth -= Math.sign(dth) * 6.283185;
            B.omT = Math.max(-2.8, Math.min(2.8, dth * 2.8));
          } else if (tb3 > B.tRe) { B.omT = (Math.random() - 0.5) * 1.0; B.tRe = tb3 + 2 + Math.random() * 3; }   // open water — gentle wander
          // signed yaw carried over the last several seconds: ~±6.3 means a full circle one way
          B.turnAcc = (B.turnAcc || 0) * Math.exp(-0.48193 * dt) + (B.om || 0) * dt;   // PER-SECOND leak (0.48193 = −60·ln 0.992), IDENTICAL at 60 fps and unchanged in character. The old 0.992 was applied once per FRAME against a per-second input, so the steady value was 125·om·dt — ~5.8 at 60 fps but only 1.2-2.4 at the 185-400 fps this ships at, which left BOTH thresholds below (4.0 and 2.5) permanently dead: a mother duck circling a small bay never got the unwind-the-long-way-round correction or the reverse-direction penalty and orbited forever, on a fast machine only.
          if (!duckOK(B.th, 13) || !duckOK(B.th, 7)) {  // imminent bank ahead — turn down the LONGEST open lane (backs up the soft repulsion; earlier lookahead so it never reaches the shore)
            const duckReach = (th6) => { for (let d = 5; d <= 26; d += 3) if (!duckOK(th6, d)) return d; return 26; };
            const spin = Math.abs(B.turnAcc) > 2.5 ? Math.sign(B.turnAcc) : 0;   // already part-way round? penalise going further that way
            let bo = 0, bs = -1e9;
            for (const off of [0.6, -0.6, 1.2, -1.2, 1.9, -1.9, 2.7, -2.7]) {
              const sc = duckReach(B.th + off) - Math.abs(off) * 2.2 - (Math.sign(off) === spin ? 9 : 0);
              if (sc > bs) { bs = sc; bo = off; }
            }
            B.omT = Math.max(-3.4, Math.min(3.4, bo * 2.4));
          }
        }
      } else if (B.kind === 6) {                       // ── FISH 2.0 ── config-driven swimmer (FISH_CFG): SWIM ⇄ FLEE states in the water, dead-straight in the AIR.
        const FC = FISH_CFG, CRZ = FC.baseSpeed;
        if (freezeK < 0.4) B.animClk = (B.animClk || 0) + dt * ((B.spd || CRZ) / CRZ) * (FC.animFps / 24);   // tail-beat LOCKED to swim speed on the SIM clock (the frame index floors animClk·24): baseSpeed → animFps, double speed → EXACTLY 2×animFps. Render fps never enters the equation. FROZEN in the ice (freezeK≥0.4) → hold the tail frame too, not just movement (user)
        const ay = Math.floor(B.y);
        const fishOK = (th6, dist) => fishFits(B, th6, dist);      // BODY-aware + deep-enough water (see fishFits) — identical to what stepOK will accept
        const reachOf = (th6, maxD) => fishReach(B, th6, maxD);    // how far the open water runs along a heading — the compass for smooth channel-following
        if (B.jumpV === undefined) {                   // all steering is water-state only; an airborne fish holds its validated launch line
          // ── THREAT SCAN ── the player plus every configured predator creature (FC.predatorKinds — ducks by default);
          // the NEAREST one inside threatR (re)starts the flee window, and fleeHold keeps the state from flickering at the rim.
          let td2 = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z) + (B.y - P.y - 2) * (B.y - P.y - 2), tx9 = P.x, tz9 = P.z;
          if (FC.predatorKinds.length) for (let m9 = DUCK_0; m9 < BABY_END; m9++) { const O = wbf[m9];   // ducks live in DUCK_0..BABY_END; widen the scan if other kinds ever join the list
            if (!O || !O.init || FC.predatorKinds.indexOf(O.kind | 0) < 0) continue;
            const q2 = (B.x - O.x) * (B.x - O.x) + (B.z - O.z) * (B.z - O.z) + (B.y - O.y) * (B.y - O.y);
            if (q2 < td2) { td2 = q2; tx9 = O.x; tz9 = O.z; } }
          if (td2 < FC.threatR * FC.threatR) { B.fleeT = tb3 + FC.fleeHold; B.thrX = tx9; B.thrZ = tz9; }
          if (tb3 < (B.fleeT || 0) || spooked(B)) {    // ── FLEE ── EXACTLY fleeMult × base speed, angling away along the CLEAREST open water (never bolt into a bank)   // …or struck: hitCreature sets the spook window AND thrX/thrZ, so a shot fish bolts off the player exactly as it bolts off a duck
            if (tb3 > (B.flRe || 0)) {                 // the escape-heading FAN re-plans at 8 Hz, not per render frame — at 700 fps the every-frame fan was most of the fish AI cost for zero behavioural gain (the om ease bridges 125 ms invisibly)
              B.flRe = tb3 + 0.125;
              const away = Math.atan2(B.x - (B.thrX !== undefined ? B.thrX : P.x), B.z - (B.thrZ !== undefined ? B.thrZ : P.z));
              let bo = 0, bs = -1e9;
              for (const off of [0, 0.7, -0.7, 1.6, -1.6]) { const sc = reachOf(away + off, 30) - Math.abs(off) * 6; if (sc > bs) { bs = sc; bo = off; } }   // 5 headings (was 9) — plenty to pick an open escape lane
              B.flTh = away + bo;
            }
            const fth = B.flTh !== undefined ? B.flTh : B.th;
            B.omT = Math.max(-FC.fleeYawRate, Math.min(FC.fleeYawRate, Math.atan2(Math.sin(fth - B.th), Math.cos(fth - B.th)) * 5));
            B.spdT = CRZ * FC.fleeMult; B.dT = 0.6 + Math.random() * 0.3;
            B.fleeing = true;
          } else {                                     // ── CRUISE ── the LOCAL NAVIGATION PROBE (in the sense block below) is now the compass: a whisker fan
            B.fleeing = false;                         // scanning the water every tick, so the cruise here only sets the long-range INTENT (drift straight, held by
            // navTh) plus school cohesion. The probe then bends that intent onto the freest lane, which is what keeps the
            // fish clear of banks and rocks proactively instead of only reacting once one is in its face (user).
            let want = B.navTh !== undefined ? B.navTh : B.th;
            if (B.school >= 0) {                       // schooling BLENDS with the channel: mostly stick with the group, but bend toward open water so the school never piles into a bank
              let cx = 0, cz = 0, hx = 0, hz = 0, sxx = 0, szz = 0, n = 0;
              for (let m = FISH_0; m < FISH_END; m++) { const O = wbf[m]; if (O === B || !O.init || O.school !== B.school) continue;
                const dxs = O.x - B.x, dzs = O.z - B.z, d2 = dxs * dxs + dzs * dzs; if (d2 > 42 * 42) continue;
                cx += O.x; cz += O.z; hx += Math.sin(O.th); hz += Math.cos(O.th); n++;
                if (d2 < 7 * 7 && d2 > 0.01) { const w = 7 - Math.sqrt(d2); sxx -= dxs * w; szz -= dzs * w; } }
              if (n > 0) { const gx = (cx / n - B.x) + sxx * 1.6, gz = (cz / n - B.z) + szz * 1.6;
                const sth = (gx * gx + gz * gz > 1) ? Math.atan2(gx, gz) : Math.atan2(hx, hz);
                want = Math.atan2(0.68 * Math.sin(sth) + 0.32 * Math.sin(want), 0.68 * Math.cos(sth) + 0.32 * Math.cos(want)); } }
            B.navGoal = want;                          // the intent handed to the whisker probe — it steers toward the freest lane NEAR this, not blindly at it
            B.spdT = CRZ;                              // NORMAL swimming runs at the configured base speed, exactly — the flee state is the sole 2× (user)
          }
          B.spd = (B.spd || CRZ) + ((B.spdT || CRZ) - (B.spd || CRZ)) * (1 - Math.exp(-((B.spdT || 0) > (B.spd || CRZ) ? 6 : 1.4) * dt));   // asymmetric ease: the kick to double is FAST, the bleed-off slow — dart, then coast
          if (tb3 > (B.dRe || 0)) { B.dT = 0.25 + Math.random() * 0.6; B.dRe = tb3 + 2 + Math.random() * 4; }   // depth wander — a new hold-depth every few seconds
          // ── SENSE AT ~14 Hz, ACT EVERY FRAME ── the voxel probing re-evaluates on a ~70 ms cadence (a fish covers a
          // voxel or two between checks — nothing sneaks inside the multi-body-length whiskers), while the cached RESULT
          // (nav heading, push vector, backstop, speed cap) applies every frame so motion stays perfectly smooth. At
          // uncapped render rates the per-frame probing WAS the fish AI cost.
          if (tb3 > (B.senseRe || 0)) { B.senseRe = tb3 + 0.07;
            // ── LOCAL NAVIGATION PROBE ── the "sensors constantly scanning the water" (user). A whisker fan sweeps the
            // front arc around the current intent; each whisker measures how far the BODY can travel down it (fishReach)
            // AND whether both flanks a few body-lengths ahead are open water (so it stays CENTRED in a channel, off the
            // banks). The fish continuously banks toward the freest, best-centred lane — it curves around a shore, rock
            // or river bend from many voxels out instead of only reacting once the wall is in its face. probeMax scales
            // with speed, so a fast fish looks correspondingly further ahead.
            const goal = B.navGoal !== undefined ? B.navGoal : B.th;
            const probeMax = Math.max(20, B.spd * 1.1);
            const flankD = Math.max(5, (B.fhalf || 5) + 2);
            const flankClear = (th, side) => { const qx = B.x + Math.sin(th + side) * flankD, qz = B.z + Math.cos(th + side) * flankD;
              return (bfWater(qx, qz) && WL - bfBed(qx, qz) >= 3 && !solid(Math.floor(qx), ay, Math.floor(qz))) ? 1 : 0; };
            let bestTh = goal, best = -1e9, bestReach = 0;
            for (const off of [0, 0.3, -0.3, 0.62, -0.62, 1.0, -1.0, 1.5, -1.5, 2.2, -2.2]) {
              const th = goal + off;
              const reach = fishReach(B, th, probeMax);
              if (reach < 3) continue;                  // the body can't even start down this whisker — never a candidate
              const flank = flankClear(th, 1.05) + flankClear(th, -1.05);   // 0..2 — lanes with water on both sides win, centring the fish
              const turn = Math.abs(off) + Math.abs(Math.atan2(Math.sin(th - B.th), Math.cos(th - B.th))) * 0.5;   // prefer the goal AND the current heading → smooth, no dithering
              const sc = Math.min(reach, probeMax) + flank * 6 - turn * FC.turnCost;
              if (sc > best) { best = sc; bestTh = th; bestReach = reach; }
            }
            if (best <= -1e9) { bestTh = B.th + (B.om >= 0 ? 3.14159 : -3.14159); bestReach = 0; }   // every whisker blocked → about-face
            B.navTh = bestTh; B.navReach = bestReach;

            let rx = 0, rz = 0;                         // ── BANK REPULSION ── a safety NET for a body that has already drifted a nose/tail into terrain even though the centre is in water: sum a push off any shore/rock within 9 vox and slide toward open water. With the probe steering proactively this rarely fires, but it is what physically peels a fish off an edge steering alone cannot.
            for (let k = 0; k < 8; k++) { const sa = Math.sin(k * 0.7854), ca = Math.cos(k * 0.7854);
              for (let d = 3.5; d <= 9; d += 2.75) { const qx = B.x + sa * d, qz = B.z + ca * d;
                if (WL - bfBed(qx, qz) < 3 || solid(Math.floor(qx), ay, Math.floor(qz)) || solid(Math.floor(qx), ay + 1, Math.floor(qz))) { rx -= sa * (10 - d); rz -= ca * (10 - d); break; } } }
            B.repX = rx; B.repZ = rz;
            const look = Math.max(FC.lookMin, B.spd * 0.62);   // ── REACTIVE WALL BACKSTOP ── last-ditch: terrain inside the immediate lookahead → hard override + slow. The probe should prevent this from ever tripping, so it is now purely a fail-safe.
            B.bkOn = !fishOK(B.th, look) || !fishOK(B.th, 5);
            if (B.bkOn && tb3 > (B.bkRe || 0)) {
              B.bkRe = tb3 + 0.1;
              let bo = 0, bs = -1e9;
              for (const off of [0, 0.5, -0.5, 1.1, -1.1, 2.0, -2.0]) { const sc = reachOf(B.th + off, look) - Math.abs(off) * 3.5 - (off * B.om < 0 ? 4 : 0); if (sc > bs) { bs = sc; bo = off; } }
              // ── THE ABOUT-FACE IS THE LAST RESORT, NOT THE SECOND (user 2026-08-07) ── `bs` is scored with
              // reachOf capped at `look`, so in any confined water it sits under 4 routinely and this used to spin
              // the fish through 180° on a short-range opinion while the long-range probe still had a perfectly good
              // lane. Ask the probe first: only when IT has nothing either (navReach under a body length) is the water
              // genuinely a dead end. Otherwise take the scan's own offset and let it add urgency to the probe's turn.
              const probeStuck = (B.navReach === undefined ? 0 : B.navReach) < 4;
              B.bkTh = (bs < 4 && probeStuck ? (B.om >= 0 ? 3.14159 : -3.14159) : bo); B.bkTight = bs < look * 0.5;
            }
          }
          if (!B.fleeing) {                             // ── STEER toward the probe's chosen lane every frame (cached navTh) ── the eased om below smooths the ~70 ms cadence into a continuous glide
            let dth = Math.atan2(Math.sin((B.navTh !== undefined ? B.navTh : B.th) - B.th), Math.cos((B.navTh !== undefined ? B.navTh : B.th) - B.th));
            B.omT = Math.max(-FC.yawRate, Math.min(FC.yawRate, dth * 2.2));
            const nr = B.navReach !== undefined ? B.navReach : 30;   // PROACTIVE SLOWDOWN: the open lane is short → ease off well before the backstop so tight water is taken at a controlled pace
            if (nr < 16) B.spd = Math.min(B.spd, Math.max(9, nr));
          }
          { const rx = B.repX || 0, rz = B.repZ || 0;   // apply the CACHED repulsion each frame (dt-scaled → frame-rate independent slide)
            if (rx * rx + rz * rz > 0.3) { const rl = Math.hypot(rx, rz), nrx = rx / rl, nrz = rz / rl;
              if (bfWater(B.x + nrx * 2, B.z + nrz * 2) && WL - bfBed(B.x + nrx * 2, B.z + nrz * 2) >= 3) { B.x += nrx * Math.min(7, rl) * dt * 1.9; B.z += nrz * Math.min(7, rl) * dt * 1.9; }   // slide toward open water, but only if that step is itself clear (never shove into the opposite bank)
              const dr = Math.atan2(Math.sin(Math.atan2(nrx, nrz) - B.th), Math.cos(Math.atan2(nrx, nrz) - B.th));
              B.omT = Math.max(-5, Math.min(5, B.omT + dr * 2.2)); } }
          if (B.bkOn) {                                 // ── THE BACKSTOP ADDS URGENCY; IT NO LONGER REPLACES THE PLAN (user 2026-08-07) ──
            // It used to overwrite omT outright. But `look` is max(12, spd·0.62) ≈ 13.6 voxels, and in a river a point
            // 13.6 ahead is land most of the time — measured, this "fail-safe" was tripping on 79% of frames. So four
            // frames in five the fish threw away the whisker probe's 24-voxel, flank-centred lane and steered instead on
            // a 7-way scan capped at 13.6 that about-faces whenever nothing scores above 4. That is the ping-pong
            // between banks, and it is why steering felt reactive however good the probe got. Adding to the probe's turn
            // keeps the long-range lane authoritative while a genuine wall still dominates: an about-face is ±π here, so
            // it swamps the sum by construction, while a mild bkTh only nudges.
            B.omT = Math.max(-6.5, Math.min(6.5, B.omT + (B.bkTh || 0) * 2.5));
            B.spd = Math.min(B.spd, B.bkTight ? 8 : 16);     // tight gap → crawl; comfortable gap → shed the dart but keep way on
          }
        } else { B.omT = 0; B.fleeing = false; }       // AIRBORNE: no steering — the splash-down was validated for the launch line and must stay where it was aimed
      } else if (NAVARB) {                             // ── FLYER, ON THE ARBITER ── ONE scored candidate fan at 12 Hz, and ONE feasibility answer (navFitsAir) shared by this planner, the mover below, the vertical step and the escape — so "the planner chose a move the mover refuses" is not expressible. It replaces two POINT probes at 7 and 13 voxels: at 56 vox/s against an 11.2-vox turn radius, 13 vox is 0.23 s of warning, and a trunk at 10 voxels was invisible to the planner while the mover advanced 0.93 vox a frame straight into it.
        if (tb3 > B.tRe) { B.navWander = B.th + (Math.random() - 0.5) * 2.2; B.tRe = tb3 + 0.4 + Math.random() * 0.8; }   // the WANDER survives as a score term instead of a direct omT write. Same two RNG draws at the same cadence as the line it replaces, so a seeded A/B still lines up.
        if (B.navRe === undefined) B.navRe = tb3 + (wk & 15) * 0.0052;   // stagger the sense tick across the band so 16 flyers never think on the same frame
        if (tb3 >= B.navRe) {
          B.navRe = tb3 + NAV_HZ;
          let homeTh = 0, leashOut = false;
          if (B.hcx !== undefined) { const lx = B.x - B.hx, lz = B.z - B.hz;   // the LEASH becomes a score term too — the block below used to bend B.th directly, which is a second writer steering against the avoidance
            if (lx * lx + lz * lz > FLY_LEASH * FLY_LEASH) { leashOut = true; homeTh = Math.atan2(-lx, -lz); } }
          // …and a BEE ON AN ERRAND steers by exactly the same term, overriding the leash for as long as it lasts.
          // It can only OVERRIDE and never fight it, because BEE_FLOWER_R < FLY_LEASH puts every flower the bee
          // can choose inside the disc the leash holds it in, so the two bearings agree by construction. Fed as a
          // SCORE, not as a heading: the fan still has to find a lane that is open (navReachAir) and a pose the
          // mover will accept (navFitsAir), so a trunk between the bee and its flower routes it around rather
          // than into — which is the whole reason the errand is expressed here and not as a write to B.th.
          // ── AND AN ENRAGED BEE IS THE SAME TERM WITH THE PLAYER AS THE GOAL ── one more clause, not a new
          // mechanism, and it is the ONLY reason the chase can leave FLY_LEASH: leashOut REPLACES the home
          // bearing with this one, so the disc the bee is normally held in stops pulling for as long as the
          // rage lasts and the give-up distance is what bounds it instead. Still a SCORE and never a write to
          // B.th, so navReachAir/navFitsAir keep the last word — a bee whose player is behind a trunk routes
          // around it, and a bee that cannot get to you simply does not arrive, which is the same guarantee
          // the flower errand has and the same one sim/life/fish.js was fixed to give.
          if (desBee && (B.beeM === 1 || B.beeM === 3 || B.beeM === 5)) { leashOut = true;
            const gx9 = B.beeM === 5 ? P.x : B.beeTx, gz9 = B.beeM === 5 ? P.z : B.beeTz;
            homeTh = Math.atan2(gx9 - B.x, gz9 - B.z); }
          navSteerAir(B, homeTh, leashOut);
        }
        // ══ THE CLOSER (user 2026-08-19: "when the bees are supposed to attack, they dont target the
        // player, they cause damage but only if the player just happens to be in the way") ══ the fan above
        // is the ROUTER and stays the router; this is the last twenty voxels of the chase, and it is the
        // whole of the fix. The three reasons a goal BEARING cannot land a sting are written out on the
        // BEE_ATK_* block in sim/life/slots.js; the short of it is that the fan's own KEEP + WANDER terms
        // outvote the goal term for a bee that has overshot, that 56 vox/s against a 6.5 rad/s yaw clamp is
        // an 8.6-voxel minimum turn radius round a 6.5-voxel target, and that 22.5° at 12 Hz is 4.6 voxels
        // of travel per decision. So the closer runs EVERY FRAME instead of on the sense tick, and aims at
        // an EXACT bearing instead of one of sixteen.
        // WHAT IT IS NOT: it is not a second mover. It writes the same B.omT the fan writes, through the
        // same ease and the same ±6.5 clamp, and a speed the same navBrakeAir caps and the same navFitsAir
        // vetoes. It takes the heading ONLY when the direct line is clear all the way to its goal, so a bee
        // with a trunk in the way is still routed by the fan and "a bee that cannot get to you simply does
        // not arrive" is the guarantee it always was — the fish lesson, kept.
        if (desBee && B.beeM === 5 && BEE_CLOSER) {
          B.beePh += BEE_ATK_W * dt;                 // ── THE SWARM STAYS A SWARM ── each bee closes on its OWN point of a ring round the player, not on the player, so eight converging bees arrive on eight different bearings instead of stacking into one dot. beePh is already seeded per slot from Math.random and is free-running, so this needs no new field and cannot go stale across a recycle (the seed line clears B.beeM and re-derives it).
          const rA5 = (5.0 + (MAMFIT.bee ? MAMFIT.bee.hd : 2)) * BEE_ATK_F;   // …and the ring is the STING REACH scaled — CHARACTER FOR CHARACTER the expression the sting test reads, so the geometry the bee flies and the geometry that scores a hit cannot drift when the model's footprint changes
          const axR = P.x + Math.sin(B.beePh) * rA5, azR = P.z + Math.cos(B.beePh) * rA5;
          const dxR = axR - B.x, dzR = azR - B.z, dR = Math.sqrt(dxR * dxR + dzR * dzR);
          B.beeRgSpd = Math.max(BEE_CLOSE_MIN, Math.min(56, dR * BEE_CLOSE_K));   // ARRIVAL, capped at the flyer's OWN cruise. This ramp can only ever SLOW a bee — that is what keeps sprinting (85) an escape and the leash reachable, and it is the half that lets the bee hold a 1.5-voxel turn radius at the ring instead of an 8.6-voxel one.
          if (dR > 0.05 && !(B.trap > BEE_ATK_TRAP)) {  // …and it stands down while the mover is refusing steps, or it would fight the escape probe below and bank trap until the mercy recycle
            const thR = Math.atan2(dxR, dzR);
            const wantR = dR < NAV_LOOK ? dR : NAV_LOOK;
            B.beeClr = navReachAir(B.x, B.y, B.z, thR, wantR); B.beeWant = wantR;   // stashed for window.__vbBee — the value is computed anyway, so this is two writes and no extra probe
            if (B.beeClr >= wantR - 0.01)   // THE SAME predicate the fan scores with and the mover applies — clear to the goal (or to the planner's own horizon) or the fan keeps the heading and routes
              B.omT = Math.max(-6.5, Math.min(6.5, navAng(thR - B.th) * BEE_ATK_TURN));
          }
        }
      } else {
        if (tb3 > B.tRe) { B.omT = (Math.random() - 0.5) * 4.0; B.tRe = tb3 + 0.4 + Math.random() * 0.8; }
        const la5 = 13;
        if (bfObst(B.x + Hx2 * la5, B.y, B.z + Hz2 * la5) || bfObst(B.x + Hx2 * 7, B.y + 1, B.z + Hz2 * 7)) {   // flyers see obstacles EARLY so the EASED turn has room — no last-moment snap
          const pFree = !bfObst(B.x + Math.sin(B.th + 1.0) * la5, B.y, B.z + Math.cos(B.th + 1.0) * la5);
          const nFree = !bfObst(B.x + Math.sin(B.th - 1.0) * la5, B.y, B.z + Math.cos(B.th - 1.0) * la5);
          B.omT = pFree && !nFree ? 5.0 : (nFree && !pFree ? -5.0
            : (!pFree && !nFree ? (!bfObst(B.x + Math.sin(B.th + 2.4) * 9, B.y, B.z + Math.cos(B.th + 2.4) * 9) ? 6.4 : -6.4)   // fully cornered (dense canopy pocket) — swing back toward the way it came
            : (B.om >= 0 ? 5.0 : -5.0)));              // steer toward the open side; the ease below smooths the heading — no discontinuous snap
        }
      }
      const iceLock = (B.kind === 3 || B.kind === 6) && freezeK >= 0.4;   // DUCK on the surface / FISH under it → locked in the ice when the water turns to ice: no paddling/swimming, turning or depth-wander. 0.4 = where the waves flatten + the ice look is set in (user)
      if (iceLock) { B.om = 0; B.omT = 0; B.trap = 0; }   // hold the heading + clear any pending turn/stuck state so it resumes cleanly on thaw
      else { B.om += (B.omT - B.om) * (1 - Math.exp(-9 * dt)); B.th += B.om * dt; }
      // ── THE LILY'S TWO UNWRITTEN FIELDS (audit 2026-08-18) ── B.spin is assigned NOWHERE in src, and B.mth is
      // only ever +='d, never seeded at spawn: `undefined * dt` is NaN, and it lands straight in the heading and
      // then the position. Unreachable today — nLily is 0 and the lily band was repurposed for the perched
      // songbirds — so this is a trap armed for whoever re-enables kind 4, not a live bug. Defaulted rather than
      // deleted because the drift itself is still wanted if the pads come back; a NaN position is not.
      if (B.kind === 4) { B.th += (B.spin || 0) * dt; const mth9 = B.mth || 0; Hx2 = Math.sin(mth9); Hz2 = Math.cos(mth9); }   // lily: spin the MODEL (th) freely while drifting along mth
      else { Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th); }
      // ── A FLYER BOLTS WHEN THE PLAYER IS ON IT (user 2026-08-22: "do the same thing to the butterfly.
      // double its speed when the player gets near") ── the same shape the fish have, and their numbers where
      // they transfer: FLY_FLEE_MULT is exactly 2, and FLY_FLEE_HOLD keeps the state from flickering at the rim
      // the way FISH_CFG.fleeHold does. The RADIUS is the flyers' own — a butterfly at 56 vox/s crosses the
      // fish's 56-voxel sphere in a second, so sharing it would leave every butterfly permanently spooked.
      // Distance is measured to the player's chest, the same P.y + 2 the fish threat scan uses: a butterfly
      // lives above head height and a flat test would trigger on someone walking underneath it.
      // ── AND IT SITS HERE, NOT IN THE FLYER BRANCH ── the flyers have TWO steering paths, `else if (NAVARB)`
      // and the fallback `else` below it, and NAVARB is on, so a check written into the fallback never runs at
      // all: measured, B.fleeT stayed undefined and the speed never moved off 56. This line is past the whole
      // by-kind chain, on the way to the speed every creature is about to be given, so neither path can miss it.
      // SPEED ONLY — the heading is untouched. That is the "it darts off" the request asked for rather than a
      // fish's escape-heading fan, and a startled butterfly bolting along its own wander reads correctly.
      if ((B.kind | 0) === 0) {
        const td0 = (B.x - P.x) * (B.x - P.x) + (B.y - P.y - 2) * (B.y - P.y - 2) + (B.z - P.z) * (B.z - P.z);
        if (td0 < FLY_THREAT_R * FLY_THREAT_R) B.fleeT = tb3 + FLY_FLEE_HOLD;
      }
      const spd5 = iceLock ? 0 : (B.kind === 6 ? (B.spd || 6) : (B.kind === 4 ? 1.1 : (B.kind === 3 ? (isBaby ? (B.chase > 9 ? 10 : (B.chase > 3.5 ? 7 : 1.5)) : 7)   // fish ride their live burst-glide speed; lilies drift; ducklings hustle when behind, dawdle at heel
        : (B.kind === 2 ? ((bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot) ? (B.bspd || 0) : (desSlot && DESERTS[desSp] ? ((B.chase > 6 ? 34 : (B.chase > 3.6 ? 22 : (B.chase > 0 ? 13 : 0))) || DES_SPD[DESERTS[desSp].name] || 16) * ((DES_DASH[DESERTS[desSp].name] && ((P.x - B.x) * (P.x - B.x) + (P.z - B.z) * (P.z - B.z)) < DES_DASH_R * DES_DASH_R) ? DES_DASH[DESERTS[desSp].name] : 1) : 16)) : (B.kind === 1 ? 26 : 56 * (tb3 < (B.fleeT || 0) ? FLY_FLEE_MULT : 1))))));   // bunny/armadillo/skunk/porcupine drive their OWN motion (bspd 0 → no shared glide); worm 1.6 m/s (continuous), firefly 2.6, butterfly 5.6  [extra ) closes the iceLock ternary]
      const mamSlot = bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot;   // the LAND MAMMALS share B.kind 2 with the worm but drive their own march; the worm's arbiter wiring is theirs alone
      // ── DESERT STEERING OVERRIDE ── runs AFTER both kind-2 branches have chosen a heading, because the
      // desert creatures take the NAV-ARBITER branch (wormArb below is true for them) and anything written
      // into the plain worm branch never executes for them at all. Setting B.omT here wins either way.
      // ── TURN AWAY FROM THE BIOME EDGE (user 2026-08-16: life "gets stuck on the invisible wall between
      // biomes") ── for the bands that are NOT on the arbiter. The mammals, the bunny and the ant leader each
      // drive their own walk and never reach navSteer2, so the reach clamp above cannot help them; without this
      // they walk into the line and the step rule holds them there. Measured: this block first went in INSIDE
      // the desert gate below, which meant every pine-forest mammal — the whole population on the other side of
      // the line — had no avoidance whatsoever, and the residual stall was mostly them.
      if ((B.kind | 0) === 2 && B.init && (mamSlot || bunnySlot || antLead)) {
        // …and it is the HOME TAG that decides what 'the other side' means, not desSlot. That matters for
        // exactly one creature in this block: the PORCUPINE, which is no longer allowed in the oak forest
        // and so has an oak line to turn away from as well as a sand one. The bunny, armadillo and skunk
        // are BIO_ANY and their three samples are the identical desertM comparisons they always were.
        if (!bioHomeOK(bioMe, B.x + Hx2 * BIO_LOOK, B.z + Hz2 * BIO_LOOK)) {
          const lx9 = Math.sin(B.th + 1.2), lz9 = Math.cos(B.th + 1.2);
          const rx9 = Math.sin(B.th - 1.2), rz9 = Math.cos(B.th - 1.2);
          const lOK9 = bioHomeOK(bioMe, B.x + lx9 * BIO_LOOK, B.z + lz9 * BIO_LOOK);
          const rOK9 = bioHomeOK(bioMe, B.x + rx9 * BIO_LOOK, B.z + rz9 * BIO_LOOK);
          const turn9 = lOK9 ? 1.2 : (rOK9 ? -1.2 : 3.14159);
          B.th += Math.max(-2.4 * dt, Math.min(2.4 * dt, turn9));
          Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
          B.mth = B.th;
        }
      }
      if (desSlot && (B.kind | 0) === 2) {
        const desLine = DESERTS[desSp] && DESERTS[desSp].name === 'ant';
        const desIdx = (wk - MAM_END) % DES_PER;
        // ── ANT FOLLOWERS ARE NO LONGER STEERED AT ALL ── they are PLACED on the leader's own path, after the
        // move, further down this loop. Steering them could not work and the measurements say why: a follower
        // aimed at a point behind the leader's INSTANTANEOUS heading, and its turn-rate cap gives it a minimum
        // turn radius of 4.6-12.1 voxels against a 3.2 voxel spacing. Since each ant's own jittery heading was
        // the reference for the next one, the error amplified down the chain — a string instability. Measured
        // gaps ran 6.8 / 17.4 / 25.5 / 29.4 / 30.9 against an intended 3.2, they overtook each other (slot
        // order matched travel order in only 20% of frames) and passed straight through one another.
        if (desLine && desIdx > 0) B.chase = 0;
        // ── THE HUNTERS ── cobra and scorpion steer AT the player instead of wandering. The mammals' flee
        // maths inverted: they maximise the dot with the away vector, this maximises it with the toward one.
        // ── THE PREY RUNS THE OTHER WAY (user 2026-08-16: "if the mouse or gecko come near the player, have
        // them run in the opposite direction. keep the 2x speed") ── the mirror image of the hunt block below.
        // Same radius as DES_DASH, so the animal starts sprinting and starts fleeing on the same step rather
        // than bolting in whatever direction it happened to be pointing. The speed itself is untouched —
        // DES_DASH still doubles it — this only decides WHERE the doubled speed is spent.
        // ── BOTH BEARINGS NOW GO IN THROUGH THE PLANNER ── the B.omT write that used to sit in each of these
        // two blocks is gone; it ran every frame AFTER the fan and threw the fan's answer away, which is the
        // whole of the cactus stall. What is left here is the WANDER target, which is the one thing the fan
        // cannot derive for itself: pointing it along the run keeps the wander term (0.70) pulling the same way
        // as the goal term (0.95) instead of dragging the sprint off toward a stale random bearing.
        // ── AND THE RE-ROLL IS DEFERRED, NOT KILLED ── this was `tb3 + 1e9`: one pass within DES_DASH_R froze
        // B.navWander for the rest of the creature's life, since nothing outside a slot recycle ever wrote tRe
        // again, so long after the player had gone the animal was still steering at one fixed compass bearing.
        // Re-armed 0.5 s ahead every frame the chase is live, it is suppressed for exactly as long as the run
        // lasts and the ordinary 1.5-4 s re-roll resumes half a second after it ends.
        if (desSlot && DESERTS[desSp] && DES_DASH[DESERTS[desSp].name] && !DES_HUNT[DESERTS[desSp].name]) {
          const fdx = B.x - P.x, fdz = B.z - P.z, fd2 = fdx * fdx + fdz * fdz;
          if (fd2 < DES_DASH_R * DES_DASH_R && fd2 > 0.01) {
            B.navWander = Math.atan2(fdx, fdz);
            B.tRe = tb3 + 0.5;
          }
        }
        if (desSlot && DESERTS[desSp] && DES_HUNT[DESERTS[desSp].name]) {
          const hdx = P.x - B.x, hdz = P.z - B.z, hd2 = hdx * hdx + hdz * hdz;
          if (hd2 < 90 * 90) {
            B.navWander = Math.atan2(hdx, hdz);        // the charge bearing, same as the flee above: the goal term steers, the wander term stops fighting it
            B.tRe = tb3 + 0.5;
            // CONTACT. This used to call die() outright — the comment here noted that the moment a health
            // counter existed this is where it would be decremented instead, and that is now what happens.
            // A cobra bites harder than a scorpion, and both have a cooldown so standing in one cannot drain
            // the whole bar in a single second.
            // ── 6.5, NOT 2.2, AND THE OLD NUMBER WAS UNREACHABLE ── hd2 is CENTRE to CENTRE, and the player's
            // own half-width is HW = 2.6, so a snake pressed against the player is already 2.6+ away before
            // its own body is counted: the 2.2 test could never be true and the cobra never bit at all. The
            // scorpion only ever landed the one hit it got while closing head-on. 6.5 is the player's half
            // width plus the attacker's, plus a little, so a creature CIRCLING at contact range keeps testing
            // true and keeps biting on the cooldown — which is what the user asked for. Exactly the same
            // mistake the cactus contact test made; see cactusHurtAt, where the fix was a box sweep.
            // ── THE REACH IS THE ANIMAL'S OWN SIZE, NOT A CONSTANT ── measured: at contact the SCORPION sits
            // 4.7 away with its centre 3.7 above the player's feet, but the COBRA sits 12.2 away with its
            // centre 8.4 up. It is a 19-segment model, so its bulk holds it further out and its seat lifts
            // its centre far higher — a single pair of constants cannot cover both, and every constant tried
            // so far (2.2, then 6.5) simply excluded the cobra, which is why it never bit ONCE while the
            // scorpion stung fine. Both bounds now scale with the creature's own MAMFIT footprint and seat.
            const fB = DESERTS[desSp] ? MAMFIT[DESERTS[desSp].name] : null;
            const bReach = 5.0 + (fB ? fB.hd : 2), bRise = 4.0 + (fB ? fB.seat : 2);
            if (!dead && !P.fly && hd2 < bReach * bReach && Math.abs(P.y - B.y) < bRise && tb3 > (B.bitT || 0)) {
              B.bitT = tb3 + 1.0;
              const cobra = DESERTS[desSp].name === 'cobra';
              vitHurt(cobra ? 5 : 3, cobra ? 'a cobra struck you' : 'a scorpion stung you');
            }
          }
        }
      }
      const wormArb = NAVARB && (B.kind | 0) === 2 && !mamSlot;
      const gcW = wormArb ? navGroundAt(B.x, B.z) : 0;   // the worm's OWN travel surface, from the field — one number shared by its brake, its step test and its y servo, so all three agree on where the ground is
      // ── AND AN ENRAGED BEE FLIES ITS ARRIVAL RAMP INSTEAD OF THE FLAT CRUISE ── spd5 above is the BAND's
      // speed and stays exactly that; this is one creature in one state substituting its own. For every
      // other body in the pool spdB5 IS spd5, the identical double, so `spdB5 * dt` is bit-for-bit the
      // expression that was here and no band's trajectory moves by an ULP. The ramp is a cap, never a
      // boost — its top is spd5's own 56 — which is what leaves SPRINTING an escape (see BEE_CLOSE_MIN).
      const spdB5 = (desBee && B.beeM === 5 && B.beeRgSpd !== undefined) ? B.beeRgSpd : spd5;
      let mv5 = spdB5 * dt, nx5, nz5, wBrk0 = false;   // the frame's step LENGTH as its own variable, so the flyer brake has something to cap (wBrk0: the worm brake clamped it to exactly zero — see below)
      if (NAVBRK && (B.kind | 0) === 6 && B.jumpV === undefined) {   // ── FISH, ON THE BRAKE ── airborne is exempt: a salmon's leap was validated at launch and must fly its arc at full speed.
        mv5 = navBrake2(B, (th7, L7) => fishReach(B, th7, L7), mv5, dt, NAV_FLOOK, NAV_FBCLR, NAV_FBRK2, 6);
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      if (NAVBRK && duckArb && !iceLock) {            // ── DUCK, ON THE BRAKE ── the paddle translates along B.th, which lags the planned heading while the eased turn integrator catches up. The head buffer already refuses a step with under 7 voxels of water ahead, so without a cap the duck's only way to honour it is a hard STOP; the ramp turns that into an arrival that is already slow.
        mv5 = navBrake2(B, (th7, L7) => navReach2(duckFit, B.x, B.z, th7, L7), mv5, dt, NAV_DLOOK, NAV_DBCLR, NAV_DBRK2);
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      else if (NAVBRK && wormArb) {                   // ── WORM, ON THE BRAKE ── the mover translates along B.th, which lags the planned heading while the eased turn integrator catches up; capping the step by the reach of the lane actually being crawled means the step can no longer END past it
        mv5 = navBrake2(B, (th7, L7) => navReachLand(B.x, B.z, th7, L7, gcW, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot), mv5, dt, NAV_WLOOK, NAV_WBCLR, NAV_WBRK2);
        if (B.navClear < NAV_WLOOK && B.navRe - tb3 > 0.034) B.navRe = tb3 + 0.034;   // a braking worm re-plans at 30 Hz instead of 12 — a slowed creature must not creep at the obstacle for the rest of an 83 ms tick
        // ── A BRAKE-TO-ZERO IS A BLOCKED FRAME, AND IT USED TO BE INVISIBLE ── at reach ≤ NAV_WBCLR the curve's
        // sqrt term is 0 and the geometric cap holds it there, so mv5 is EXACTLY 0 and nx5/nz5 come out equal to
        // B.x/B.z. The step test below is then asked about the creature's own cell, which is legal sand by
        // construction, so it passed and the accepted-move line cleared trap/stuck/noMove every single frame:
        // the animal was pinned a voxel off a cactus with every stall counter reading zero, the blocked branch
        // never ran, and the 12 s mercy recycle could never fire. This flag changes no motion whatsoever — mv5
        // is already 0 — it only lets the counters see what the brake is doing, so the recycle is a real
        // backstop again if a heading the fan cannot solve ever gets through.
        wBrk0 = B.navClear <= NAV_WBCLR;
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      else if (NAVBRK && B.kind < 2) { mv5 = navBrakeAir(B, mv5, dt);
        if (B.navClear < NAV_LOOK && B.navRe - tb3 > 0.034) B.navRe = tb3 + 0.034;   // a BRAKING flyer re-plans at 30 Hz instead of 12. The rejection used to force that re-plan (B.navRe = 0 below) and closing the rejection would otherwise have taken the re-plan away with it, leaving a slowed creature to creep at the obstacle for the remainder of an 83 ms tick.
        nx5 = B.x + Hx2 * mv5; nz5 = B.z + Hz2 * mv5; }
      else { nx5 = B.x + Hx2 * spd5 * dt; nz5 = B.z + Hz2 * spd5 * dt; }   // EVERY OTHER BAND KEEPS THE ORIGINAL EXPRESSION, CHARACTER FOR CHARACTER. `Hx2 * mv5` is not `Hx2 * spd5 * dt`: float multiply does not reassociate, and routing a worm through the flyer's variable moved its step by 1 ULP a frame — enough to send a 240 s soak down a different trajectory and make a chaotic re-roll look like a regression in a band this change does not touch. ?nobrake is now bit-exact for kinds 2-6.
      let stepOK;
      if (B.kind === 2 && NAVARB && !mamSlot) {        // ── WORM, THE SAME ANSWER THE FAN SCORED WITH ── so "the planner chose a move the mover refuses" is not expressible
        navMoveK[2]++; stepOK = navLandOK(nx5, nz5, gcW, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot);   // …and for the DESERT band the same predicate also refuses a rock or a cactus outright, so it walks around one instead of up it (user 2026-08-16)
        if (!stepOK) { navRejK[2]++; B.navRe = 0;       // a rejection can only mean the world moved under the worm between sense ticks, so it forces an immediate re-plan
          if (!navLandOK(B.x, B.z, gcW, NAV_WUP, NAV_WDN, NAV_WCLR, desSlot)) { stepOK = true; navEgrK[2]++; } }   // ── EGRESS ── the worm is already OUTSIDE the travelable set (a tree landed on it, it streamed in beside rock). Refusing the step would pin it there until the mercy recycle; it crawls out along its heading instead. Displacement, never a teleport.
      } else if (B.kind === 2) {                        // STEP-AWARE crawl: accept the move if the destination surface is land, within a 2-voxel step, and clear at ITS height AND above the body
        const gN = bfSurf(nx5, nz5);
        stepOK = gN > WL + 0.5 && Math.abs(gN - bfSurf(B.x, B.z)) <= 2 && !bfObstW(nx5, gN + 2, nz5) && !bfObstW(nx5, gN + 3, nz5);
      } else if (B.kind === 6) {                       // ── FISH TERRAIN HITBOX (user) ── the WHOLE BODY (a ~10-vox model) must clear solid, not just the centre point — otherwise the long nose pokes into a wall the centre hasn't reached. Sample nose→tail across the body height; any point in rock/ground/shore rejects the move. NO player hitbox — the fish still passes freely through you.
        stepOK = B.jumpV !== undefined ||              // AIRBORNE: mid-arc + splash-down were validated at launch — the water tests below would halt the arc in mid-air over a pad or bank lip
          fishFitsAt(B, nx5, nz5, B.th);               // …otherwise THE one answer: real, deep-enough water AND the whole body fitting there, so the planner can only ever be told to go where it can actually go
      } else if (duckArb) {                            // ── DUCK, THE SAME ANSWER THE PLANNER SCORED WITH ──
        navMoveK[3]++;
        stepOK = duckFit(nx5, nz5) && (bfOpenW(nx5, nz5) || !bfOpenW(B.x, B.z));   // bfOpenW stays MOVER-ONLY and stays out of the shared answer on purpose: it is the no-gorge rule, and because a duck already inside a narrow arm is always allowed to move, it can refuse a step but can never pin one — the planner has open lanes by construction wherever this term bites.
        if (stepOK) { const hl = isBaby ? 5.0 : 7.0; stepOK = duckFit(nx5 + Hx2 * hl, nz5 + Hz2 * hl); }   // DUCK HEAD BUFFER (user: 'keep further away, still running into it'): require water WELL ahead (7 vox mom / 5 baby) so it stops with a clear margin, never touching the bank
        if (!stepOK) { navRejK[3]++;
          if (!duckFit(B.x, B.z)) { stepOK = true; navEgrK[3]++; } }   // ── EGRESS ── the duck is already outside the travelable set (the lake iced over and thawed under it, the window recentred). Refusing the step would pin it; it paddles out along its heading instead.
      } else if (B.kind >= 3) { stepOK = bfWater(nx5, nz5) && bfSky(nx5, WL + 2, nz5)   // ducks + lilies only ever move onto OPEN-SKY REAL water — never onto a dry ravine floor or under a cave roof…
          && (bfOpenW(nx5, nz5) || !bfOpenW(B.x, B.z));  // …and never INTO a narrow gorge/ravine arm (escape moves out of one are always allowed so nothing gets pinned)
        if (B.kind === 3 && stepOK) { const hl = isBaby ? 5.0 : 7.0; stepOK = bfWater(nx5 + Hx2 * hl, nz5 + Hz2 * hl); }   // DUCK HEAD BUFFER (user: 'keep further away, still running into it'): require water WELL ahead (7 vox mom / 5 baby) so it stops with a clear margin, never touching the bank
      }
      else if (NAVARB) { navMoveN++; stepOK = navFitsAir(nx5, B.y, nz5);
        if (!stepOK) { navRejects++; B.navRe = 0;      // THE SAME predicate the fan above scored with. A rejection can only mean the world moved under the creature between sense ticks, so it forces an immediate re-plan: the planner and the mover cannot disagree for longer than one frame.
          if (!navFitsAir(B.x, B.y, B.z)) { stepOK = true; navEgressN++; }   // ── EGRESS ── the creature is already OUTSIDE the travelable set (the world was edited under it, a tree landed on it, it streamed in beside rock). Refusing the step would pin it there until the mercy recycle; it walks out along the escape heading at its OWN speed instead. Displacement, never a teleport.
          else if (!(nvF[nvIdx(nx5, nz5)] & NVF_BUILT)) navRejUnb++;   // the destination cell is not vouched for, so navFitsAir fell back to a POINT probe there while the DDA sampled a DIFFERENT point of the same 2×2 — sub-cell disagreement that no lookahead measured in cells can see
          else if (navReachAir(B.x, B.y, B.z, B.th, mv5 + 0.05) > mv5) navRejSub++;   // the DDA calls the whole step clear and the endpoint probe refuses it: navFitsAir is cell-uniform only BELOW the NV_CCAP band ceiling, above which it point-probes too
          else navRejGeom++; } }   // the lane really did end inside this step — the cap was off, or the field changed under the creature between the DDA and the move
      else stepOK = !bfObst(nx5, B.y, nz5) && !bfObst(nx5, B.y + 2, nz5);
      if (((((B.kind | 0) === 0) && !NAVARB) || ((B.kind | 0) === 2 && !wormArb) || (B.kind | 0) === 6) && B.hcx !== undefined && B.jumpV === undefined) {   // LEASH: a worm/fish drifts around its home rather than away from it (never mid-LEAP — bending the heading in the air would curve the arc off its validated splash-down). The BUTTERFLY is off this path when the arbiter is live: its leash is a scored term up in navSteerAir, not a direct write to B.th behind the planner's back.
        const lx = B.x - B.hx, lz = B.z - B.hz; let l2 = lx * lx + lz * lz;
        const leash = (B.kind | 0) === 2 ? WORM_LEASH : ((B.kind | 0) === 6 ? 52 : FLY_LEASH);   // a fish patrols ~5 m around its pool spot — enough for river runs, never leaves its water body
        if ((B.kind | 0) === 2 && (B.trap || 0) > 0.35) l2 = 0;   // STUCK: stop pulling toward home so the normal avoidance can steer it around the trunk instead of into it
        if (l2 > leash * leash) {              // ease the heading back toward home — a hard clamp would make it skid along an invisible wall
          let want = Math.atan2(lx, lz) + Math.PI, d7 = want - B.th;
          while (d7 > Math.PI) d7 -= 6.283; while (d7 < -Math.PI) d7 += 6.283;
          B.th += d7 * Math.min(1, dt * 1.6);
          Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
        }
      }
      // ── NEITHER BIOME'S LIFE WALKS INTO THE OTHER'S (user 2026-08-16) ── the SPAWN gate further up already
      // admits desert species only where desertM > 0.85 and everything else only outside it, but that is a
      // one-time test: nothing stopped a bunny strolling east into the sand or a scorpion into the pines.
      // This refuses the STEP itself, at the midline rather than at the spawn threshold, so a creature turns
      // back at the boundary instead of being trapped inside the narrower band it was born in. Only walkers
      // are gated (kind 2): fish and ducks live in water that crosses the border, and flyers are not the
      // complaint. A creature that somehow starts on the wrong side is NOT frozen — the test only refuses a
      // step that would move it further in, so it can always walk home.
      // 0.85, the SAME threshold the spawn gate admits on — not 0.5. Measured at 0.5, forest walkers showed
      // up on the "desert" side in 15% of samples, and they were not trespassing: forest life legally spawns
      // anywhere up to dm 0.85, so a midline test called a large legal band wrong and the creatures were
      // simply standing where they were born. Matching the spawn threshold makes the two agree. (That
      // reasoning is now recorded on BIO_SANDLINE in sim/nav.js, along with why the OAK line does not
      // inherit it and sits on the honest midline instead.)
      // The 'walk home' escape is unchanged and is now the shape it always meant: refuse the step only when
      // the destination is foreign AND the creature is currently standing somewhere it belongs. The old
      // two-boolean form said exactly that and no more — with one line in the world, 'not my side' and 'the
      // other creature's side' were the same bit; with two, they are not, so it is written out literally.
      if (stepOK && (B.kind | 0) === 2 && !bioHomeOK(bioMe, nx5, nz5) && bioHomeOK(bioMe, B.x, B.z)) stepOK = false;
      if (stepOK) { B.x = nx5; B.z = nz5;
        if (wBrk0) { B.trap = (B.trap || 0) + dt; B.noMove = (B.noMove || 0) + dt; }   // ACCEPTED, but the brake made it a zero-length step: count it exactly as the refused branch below does, or a stall against a cactus reports as a clean walk forever
        else { B.trap = Math.max(0, (B.trap || 0) - dt * 3); B.stuck = 0; B.noMove = 0; } }
      // ── THE ANT COLUMN ── a breadcrumb snake. The LEADER walks normally and drops a crumb every ANT_CRUMB
      // voxels; each follower is then placed at a fixed ARC LENGTH back along that recorded path, facing along
      // it. Following the leader's PATH rather than its current heading is the whole fix — the path is what a
      // real column walks, and it cannot oscillate because it is history, not a moving target.
      // Placed kinematically (B.x/B.z/B.th written directly) rather than steered, which also puts the ants out
      // of the nav arbiter's reach — the arbiter used to rewrite their heading 12 times a second.
      // ── A DEAD ANT STOPS BEING DRIVEN (user 2026-08-16: killing one leaves it "static into the terrain") ──
      // the ant is the ONE creature placed kinematically: the leader writes B.x/B.z/B.th directly and each
      // follower is set on the leader's breadcrumb path every frame, bypassing the mover entirely. Every other
      // species dies by becoming a rigid body that falls, but this block kept writing the corpse's position
      // back on top of the ragdoll, pinning it where it stood. Excluding a dying or slain body hands it to the
      // same death path the rest of the life uses. A dead LEADER also stops laying crumbs, so the followers
      // fall back to the head of the line rather than trailing a body that no longer moves.
      if (desSlot && (B.kind | 0) === 2 && !B.dying && !B.slain && DESERTS[desSp] && DESERTS[desSp].name === 'ant') {
        // RELATIVE to this ant's own run (see antBase): 0 is the leader of THIS column, and every use below —
        // the leader lookup wbf[wk - aIdx] and the arc length aIdx * ANT_GAP — is correct unchanged.
        const aIdx = ((wk - MAM_END) % DES_PER) - antBase(desSp, wk);
        if (aIdx === 0) {
          if (!B.trail) B.trail = [{ x: B.x, z: B.z, th: B.th }];
          const t0 = B.trail[0];
          const md = Math.hypot(B.x - t0.x, B.z - t0.z);
          if (md >= ANT_CRUMB) {
            B.trail.unshift({ x: B.x, z: B.z, th: B.th });   // the heading rides ALONG WITH the crumb: it is the cardinal the leader walked to reach this point, so a follower can wear it verbatim instead of re-deriving it from the chord
            // enough crumbs to cover the whole column with room to spare, and no more: an unbounded trail on a
            // creature that lives for minutes is a slow leak.
            const need = Math.ceil((DES_PER * ANT_GAP) / ANT_CRUMB) + 4;
            if (B.trail.length > need) B.trail.length = need;
          }
        } else {
          const lead = wbf[wk - aIdx];
          if (lead && lead.init && lead.trail && lead.trail.length > 1) {
            // ── THE PATH STARTS AT THE LEADER'S LIVE POSITION, NOT AT ITS LAST CRUMB ── this walked from
            // tr[0], and a crumb is only pushed once the leader has moved a whole ANT_CRUMB (0.75) from the
            // previous one. So the head of the measured path JUMPED forward 0.75 at a time while the leader
            // itself glided, and every follower inherited that stutter: the leader read as moving smoothly
            // off-grid and the line behind it as stepping from cell to cell (user 2026-08-16). Prepending the
            // leader's live x/z makes the arc length continuous, and costs one array entry per frame.
            let want = aIdx * ANT_GAP, k = 0, acc = 0;
            const tr = [{ x: lead.x, z: lead.z, th: lead.th }, ...lead.trail];
            while (k < tr.length - 1) {
              const seg = Math.hypot(tr[k + 1].x - tr[k].x, tr[k + 1].z - tr[k].z);
              if (acc + seg >= want) break;
              acc += seg; k++;
            }
            if (k < tr.length - 1) {
              const seg = Math.max(1e-4, Math.hypot(tr[k + 1].x - tr[k].x, tr[k + 1].z - tr[k].z));
              const f = Math.max(0, Math.min(1, (want - acc) / seg));
              B.x = tr[k].x + (tr[k + 1].x - tr[k].x) * f;
              B.z = tr[k].z + (tr[k + 1].z - tr[k].z) * f;
              B.th = tr[k].th !== undefined ? tr[k].th : Math.atan2(tr[k].x - tr[k + 1].x, tr[k].z - tr[k + 1].z);   // face the way the leader WENT — the crumb's OWN recorded heading, not the chord between two crumbs. They agree to the last bit on a straight run, but the chord across a corner crumb is a diagonal, and reading it would have been the one place a follower ever pointed off the compass.
              B.trap = 0; B.stuck = 0; B.noMove = 0;   // it is riding a path the leader already proved walkable
            }
          }
        }
      }
      if (!stepOK) {                                 // ── THE STEP WAS REFUSED ── written as its own test, NOT as an `else`: the ant-column block above was inserted between `if (stepOK)` and this branch, which silently re-bound the `else` to the ANT test — so every non-ant creature in the pool ran the blocked path on every frame, escape probes and all, however well its step had gone.
        B.trap = (B.trap || 0) + dt;                   // blocked (cornered) — count it
        if (antLead) B.antBlk = 1;                     // …and the ant's compass reads this next frame as "turn": its lookahead and the mover's gate are close but not the same predicate, so this is what guarantees a leader can never grind at a wall the lookahead called clear
        // ── HARD ANTI-STALL (user: "fish are still getting stuck on rocks and terrain") ── `trap` is NOT a reliable
        // stuck signal for a fish: the escape below clears it whenever it finds *somewhere* to nudge toward, so a fish
        // wedged where its 10-voxel body fits NO heading jiggles on the spot with trap pinned at 0 and never reaches
        // the 12 s mercy-recycle. Measured: one fish stalled 55 s of a 90 s soak. noMove counts REAL rejected motion
        // and nothing resets it except an accepted move, so it always fires.
        B.noMove = (B.noMove || 0) + dt;
        if (B.kind === 6) {                            // ── FISH ESCAPE ── body-aware, and it can go OVER an obstruction as well as around it (real fish rise over a boulder rather than grind its face).
          let bestA = -1, bestScore = -1e9;
          for (let k9 = 0; k9 < 16; k9++) { const th9 = k9 * 0.3927;
            if (!fishFits(B, th9, 3.5)) continue;      // the first real step must fit the WHOLE BODY — the old centre-column probe passed gaps the fish could never enter, which is how it stayed pinned
            let reach = 3.5;
            for (let d9 = 6; d9 <= 20; d9 += 3) { if (fishFits(B, th9, d9)) reach = d9; else break; }
            const turn = Math.abs(Math.atan2(Math.sin(th9 - B.th), Math.cos(th9 - B.th)));
            const sc = reach - turn * 1.6;             // widest water wins, with a mild preference for not spinning right around
            if (sc > bestScore) { bestScore = sc; bestA = th9; } }
          if (bestA >= 0) {                            // peel off toward that water at a real pace and EASE the heading over (a hard snap read as a glitchy instant spin)
            const mv = Math.max(spd5, 11) * dt;
            B.x += Math.sin(bestA) * mv; B.z += Math.cos(bestA) * mv;
            const dthr = Math.atan2(Math.sin(bestA - B.th), Math.cos(bestA - B.th));
            B.omT = Math.max(-4.5, Math.min(4.5, dthr * 5.5)); B.trap = 0; B.stuck = 0;
          } else {                                     // NOTHING fits at this depth → CLIMB toward the surface (dT drives the depth target) and come about; next frame re-probes at the new height
            B.dT = Math.min(1, (B.dT !== undefined ? B.dT : 0.5) + 1.8 * dt);
            B.dRe = tb3 + 1.5;                         // hold the raised target briefly so the depth wander can't immediately undo the climb
            B.omT = (B.om >= 0 ? 3.6 : -3.6);
          }
        } else if (B.kind >= 3) {                      // WATER creature blocked at a bank/rock (user: 'still getting stuck against shore/rocks'): don't just TURN — TURNING alone left it grazing corners.
          let bestA = -1, bestReach = -1;              // Actively STEP toward the clearest open water EVERY blocked frame so it physically peels off the bank and can never pin.
          const ay9 = Math.floor(B.y), fishClear = (px9, pz9) => B.kind !== 6 || (WL - bfBed(px9, pz9) >= 3 && !solid(Math.floor(px9), ay9 - 1, Math.floor(pz9)) && !solid(Math.floor(px9), ay9, Math.floor(pz9)) && !solid(Math.floor(px9), ay9 + 1, Math.floor(pz9)) && !solid(Math.floor(px9), ay9 + 2, Math.floor(pz9)));   // for a fish, "clear" also means deep enough AND no rock across the body at the swim depth
          for (let k9 = 0; k9 < 16; k9++) { const th9 = k9 * 0.3927;
            const sx9 = B.x + Math.sin(th9) * 2.5, sz9 = B.z + Math.cos(th9) * 2.5;   // probe a touch further so the fish peels FULLY off the rock, not just onto its edge
            if (!duckFit(sx9, sz9) || !fishClear(sx9, sz9)) continue;   // this heading's immediate step must land on water the MOVER would accept — the escape used to probe a looser rule than stepOK, so it could peel a duck toward a spot the very next step refused
            let reach = 2.5;
            for (let d9 = 4; d9 <= 16; d9 += 2.0) { const rx9 = B.x + Math.sin(th9) * d9, rz9 = B.z + Math.cos(th9) * d9; if (bfWater(rx9, rz9) && fishClear(rx9, rz9)) reach = d9; else break; }   // the RANKING term stays on the bare water probe: the first-step GATE above it is the shared answer, this only orders the candidates, and leaving it alone keeps ?noarb bit-exact against the pre-arbiter build so the control is provably the same code
            if (reach > bestReach) { bestReach = reach; bestA = th9; }   // widest contiguous open water wins → steer toward the OPEN lake, away from the bank/boulder
          }
          if (bestReach > 0) {                         // MOVE toward that water at normal paddle speed + face it + clear trap → never pins, never mercy-recycles
            const mv = spd5 * dt;
            B.x += Math.sin(bestA) * mv; B.z += Math.cos(bestA) * mv;   // physically peel off toward the open water
            const dthr = Math.atan2(Math.sin(bestA - B.th), Math.cos(bestA - B.th));   // EASE the heading toward the escape instead of a HARD snap (the snap read as a glitchy instant model spin — user: 'glitching out') — the om ease smooths it over a few frames
            B.omT = Math.max(-3.4, Math.min(3.4, dthr * 4.5)); B.trap = 0; B.stuck = 0;
          }
        }
      }
      const gLoc = bfSurf(B.x, B.z);                   // the actual ground right here
      const gAir = (NAVARB && B.kind < 2 && nvOn) ? Math.max(gLoc, nvTopAir(nvIdx(B.x, B.z))) : gLoc;   // ── FLYERS TAKE THEIR GROUND FROM THE FIELD ── bfSurf is ONE column of hmap clamped to sea level; the field's travel surface is the MAX over the 2×2 and counts decor and rock the heightmap never saw. Leaving the altitude servo on bfSurf floored butterflies BELOW the surface the predicate measures clearance from, so every frame the mover refused a move the planner had just approved — the exact planner/mover split the arbiter exists to close, reintroduced on the vertical axis.
      const yPrev5 = B.y;                            // total vertical motion this frame is budget-capped below — branches must never STACK into a visible jump
      let yBudD = 0;                                 // …and a SEATED creature may raise that budget to its own pace (see the desert servo below); 0 leaves the shared 30/34 exactly as it was
      if (B.kind === 2) {                              // WORM / BUNNY: rides the terrain SMOOTHLY — target blends the ground AHEAD (starts the ramp before a step) and eases at a gentle rate
        // ── ONE SEAT FOR ALL FOUR LAND MAMMALS (user 2026-08-07: "do they all behave the same way?") ── they did
        // not. The ground SOURCE was tied to arbiter membership: `mamArb ? navWalkStand : bfSurf`, and the bunny is
        // deliberately off the arbiter (a hop is one committed arc, see mamArb), so it alone read the raw heightmap
        // while the other three read the nav field. Measured: those two disagree at the same column on 20-34% of
        // frames, by up to 3 voxels — bfSurf is blind to the rock and decor stamped on top of it, which is precisely
        // what a body gets seated inside. Which planner an animal uses and which surface its feet rest on are
        // different questions; only the second one belongs here, and it is now the same answer for every mammal.
        const fitM = desSlot ? (DESERTS[desSp] ? MAMFIT[DESERTS[desSp].name] : null) : bunnySlot ? MAMFIT.bunny : (armSlot ? MAMFIT.arm : (skunkSlot ? MAMFIT.skunk : (porcSlot ? MAMFIT.porc : (flamSlot ? MAMFIT.flam : null))));   // ── AND THE FLAMINGO (user 2026-08-18: "the legs are completely in the ground") ── the comment on this very line predicted it: a walker with no branch here falls to the worm default of yoff 2, and the flamingo is 17 voxels tall on long legs, so it stood buried to the body. mamFitOf measures the seat off frame 0 of its own art, so nothing here is hand-tuned   // …and the desert band, or its measured seat is dead data: without a branch here every one of them falls to the worm default (yoff = 2), which is 1.5 voxels of air under a 1-voxel ant and buries the lower half of a 9-voxel cobra
        const gW = wormArb ? gcW : (fitM ? navWalkStand(B.x, B.z) : gLoc);
        // ── THE FORWARD LOOK IS A SEAT, SO IT READS THE SEAT'S SURFACE ── it used to be keyed on arbiter
        // membership rather than on having a footprint at all, which sent the desert band (wormArb) to
        // navGroundAt: the RAW 2x2 cell top, with no clutter subtracted and none of navWalkStand's step-up
        // sanity. A saguaro three voxels ahead was therefore a legal answer to "how high is my floor", and the
        // creature rose 20 voxels up its side without ever stepping onto it. Anything with a MAMFIT now takes
        // this from the same plane mamSeatG does — and, in the desert, under the same sand rule.
        const gFwd = fitM ? mamStandAt(B.x + Hx2 * 3, B.z + Hz2 * 3, desSlot)
          : (wormArb ? navGroundAt(B.x + Hx2 * 3, B.z + Hz2 * 3) : bfSurf(B.x + Hx2 * 3, B.z + Hz2 * 3));
        const yoff = fitM ? fitM.seat : 2, yflr = fitM ? fitM.seat : 1.6;   // the lift is the MODEL's own half-height above its lowest occupied layer (see MAMFIT), so a new animal can never inherit another's by accident
        // ── A BODY IS NOT A COLUMN (user 2026-08-07: "it appears to clip through the terrain") ── mamSeatG scans
        // the model's own occupied footprint, oriented by heading, and reduces with MAX; the forward look folds
        // into the same max, which keeps the reason it existed (start the ramp before the step) without a blend
        // that lands between two ground heights. The WORM keeps its 2:1 blend below: it is 3 voxels long, so its
        // body really is a column.
        let gBody = gW;
        if (fitM) { gBody = mamSeatG(B, fitM, desSlot); if (gFwd > gBody) gBody = gFwd; }
        const gT7 = (fitM ? gBody : (gW * 2 + gFwd) / 3) + yoff;   // the WORM keeps its blend: it is 3 voxels long, so its body really is a column
        // ── THE SERVO IS PACED IN VOXELS TRAVELLED, NOT SECONDS (user 2026-08-07: "still not making contact when
        // going DOWN steeper terrain") ── 12 vox/s and tau = 1/7 s were authored against a 9 vox/s armadillo. The
        // skunk walks at 24 and flees at 48, so on anything steeper than 12/24 = 1:2 the ground drops away faster
        // than this line is ALLOWED to follow it, and the body floats off at (spd*slope - 12) vox/s for the whole
        // descent — 12 vox/s on a 1:1, more than its own 8-voxel height across a 20-voxel hillside — then takes as
        // long again to settle at the bottom. Uphill never showed it: yoff === yflr for a mammal, so gT7 IS the hard
        // floor one line below, every upward correction is done instantly by that clamp, and the Math.min branch
        // here is unreachable. Scaling both terms by the animal's own pace makes the smoothing a constant ~1.3
        // voxels of TRAVEL for every mammal instead of 3.4 for a walking skunk and 6.9 for a fleeing one.
        // aspd is 9 for the armadillo/porcupine and undefined for the bunny and the worm, so kV7 is exactly 1.0
        // there and every product is bit-identical: x * 1.0 === x, and (7 * 1.0) * dt is the same double as 7 * dt.
        // The DESERT band never had a pace to scale by: B.aspd is written only by the land mammals' own marcher, so
        // every one of the seven fell to the 9 vox/s default and rode the armadillo's servo at 32 vox/s (64 when a
        // gecko bolts inside DES_DASH_R). That is the SAME defect this line was written to fix for the skunk, and it
        // is why the float got worse the faster the animal moved. Their pace is spd5, the shared glide they are
        // actually advanced by. Bit-identical everywhere else: a mammal short-circuits on B.aspd, and the bunny and
        // the worm are not desSlot, so `0 || 9` is the same 9 the old expression produced.
        const kV7 = fitM ? Math.max(1, (B.aspd || (desSlot ? spd5 : 0) || 9) / 9) : 1;
        // ── AND THE GLOBAL BUDGET IS PART OF THE SERVO ── the 30/34 vox/s clamp below is applied to every creature
        // AFTER these branches, so raising the servo's own rate alone changed almost nothing on a steep descent: a
        // gecko bolting at 64 vox/s down a 1:2 slope needs 32 vox/s of drop and the clamp only allowed 30, which is
        // where the last of the float lived (measured: 10 voxels of air, held for ~0.5 s at a time). The CLIMB side is
        // the same defect pointing the other way — it undoes the hard body floor one line below, which is why the
        // dashing gecko was also SINKING into steep ascents. The raised budget is 1.33x the animal's own travel speed,
        // so the vertical rate can never outrun the horizontal one and a teleport stays unreachable. Scoped to the
        // desert band: the fleeing skunk at 48 vox/s clips this same ceiling more mildly, and it is a shipped,
        // signed-off animal — leaving yBudD at 0 keeps every other creature's arithmetic character for character.
        if (desSlot) yBudD = 12 * kV7;
        B.y += Math.max(-12 * kV7 * dt, Math.min(12 * kV7 * dt, (gT7 - B.y) * (1 - Math.exp(-7 * kV7 * dt)) + Math.sign(gT7 - B.y) * 2 * kV7 * dt));
        if (B.y < gBody + yflr) B.y = gBody + yflr;    // hard body floor — the ease could lag uphill and sink it INTO the slope ('clips through the ground')
        if (bfObst(B.x, B.y, B.z)) B.y = gBody + yoff; // body ended up inside something (streamed terrain/step edge) — re-seat on the local surface instead of hiding+vanishing
      } else if (B.kind === 6) {                       // FISH: hold a WANDERING depth between the bed and the underside of the surface; LEAPS are real ballistics
        const FC9 = FISH_CFG, bed9 = bfBed(B.x, B.z);
        const lo9 = bed9 + 1.6, hi9 = Math.max(WL - 1.4, lo9 + 0.2);
        if (B.jumpV !== undefined) {                   // ── AIRBORNE ── pure ballistics; the depth ease/clamp is bypassed and the global vertical budget exempts the arc
          B.y += B.jumpV * dt; B.jumpV -= FC9.jump.gravity * dt;
          if (B.jumpV < 0 && B.y <= hi9) {             // ── REENTRY ── falling back through the surface → swimming again, cleanly: hold-depth resumes, the terrain
            B.y = hi9; B.jumpV = undefined;            // hitbox below re-validates the whole body, and the per-species cooldown keeps leaps occasional
            if (B.jumpSpl) spawnSplash(B.x, B.z, 1.15); if (B.jumpSpl) PH.stats.leapDown = (PH.stats.leapDown|0)+1;   // …and it goes back in with a splash (user) — a shade bigger than the launch, it is coming down with the arc behind it. Whatever the LAUNCH decided: a leap must not splash at one end only.
            B.jumpRe = tb3 + (FC9.jump.cooldownMin + Math.random() * (FC9.jump.cooldownMax - FC9.jump.cooldownMin)) / Math.max(0.05, B.jumpMul || 1);
            B.spdT = FC9.baseSpeed; B.dT = 0.35 + Math.random() * 0.3;   // glide off the splash back into a mid-depth cruise
          }
        } else {
          if ((snowOn || freezeK >= 0.02) && B.jumpArm !== undefined) { B.jumpArm = undefined; B.jumpRe = tb3 + 5; }   // snow began mid-run-up → abandon the leap (a fish must never breach a snowing/frozen lake — user)
          const tgt9 = lo9 + (hi9 - lo9) * (B.dT !== undefined ? B.dT : 0.5);
          if (!iceLock) B.y += Math.max(-7 * dt, Math.min(7 * dt, (tgt9 - B.y) * (1 - Math.exp(-2.2 * dt))));   // FROZEN fish holds its depth — no wander (user)
          B.y = Math.max(lo9, Math.min(hi9, B.y));     // hard clamp — the body NEVER breaks the surface and never grinds the bed
          // ── LEAP, TWO-PHASE ── ARM a run-up to the surface first (waiting for the depth wander to leave the fish
          // shallow practically never fired), then BREACH only after the WHOLE ARC validates. Every dial lives in
          // FISH_CFG.jump: the cooldown window ÷ species multiplier = frequency, vMin/vMax = height, forward = distance.
          if ((B.jumpMul || 0) > 0 && !snowOn && freezeK < 0.02 && B.jumpArm === undefined && tb3 > (B.jumpRe || 0) && hi9 - lo9 > FC9.jump.minDepth
              && bfSky(B.x, WL + 2, B.z) && fishFits(B, B.th, 10)) {
            B.jumpArm = tb3 + FC9.jump.armS;           // ARM: climb for up to armS seconds…
            B.dT = 1; B.dRe = tb3 + FC9.jump.armS + 1; // …holding the shallow depth target so the wander can't pull it back down mid-ascent
          }
          if (B.jumpArm !== undefined) {
            if (B.y > hi9 - 1.6 && bfSky(B.x, WL + 2, B.z)) {   // reached the surface — now prove the ARC itself before committing:
              const jv = FC9.jump.vMin + Math.random() * (FC9.jump.vMax - FC9.jump.vMin);
              const hang = 2 * jv / FC9.jump.gravity;  // total air time → splash-down = launch + heading · forward · hang
              const lx9 = B.x + Math.sin(B.th) * FC9.jump.forward * hang, lz9 = B.z + Math.cos(B.th) * FC9.jump.forward * hang;
              const mx9 = (B.x + lx9) * 0.5, mz9 = (B.z + lz9) * 0.5;
              if (bfWater(lx9, lz9) && WL - bfBed(lx9, lz9) >= 3 && bfSky(lx9, WL + 2, lz9) &&   // splash-down = REAL deep water under open sky — never land, never a shelf…
                  bfWater(mx9, mz9) && bfSky(mx9, WL + 2, mz9) && fishFits(B, B.th, 8)) {        // …and the mid-arc + immediate exit are clear too
                B.jumpV = jv; B.spd = FC9.jump.forward; B.spdT = FC9.jump.forward;   // launch, carrying the configured forward speed through the whole arc
                B.om = 0; B.omT = 0; B.jumpArm = undefined;                          // dead-straight in the air — the validated splash-down must stay where it was aimed
                B.jumpSpl = lifeIsDrawn(wk);       // decided HERE, for the whole arc — see lifeIsDrawn
                if (B.jumpSpl) spawnSplash(B.x, B.z, 1); if (B.jumpSpl) PH.stats.leapUp = (PH.stats.leapUp|0)+1;   // …and it breaks the surface with a splash (user), if it is near enough to be seen doing it
              } else { B.jumpArm = undefined; B.jumpRe = tb3 + 4 + Math.random() * 6; B.dT = 0.3 + Math.random() * 0.4; }   // the arc lands badly (bank/shallow/roof) — stand down, sink back, try elsewhere
            } else if (tb3 > B.jumpArm) { B.jumpArm = undefined; B.jumpRe = tb3 + 6 + Math.random() * 12; }   // never reached the surface (roofed/penned in) — stand down
          }
        }
        if (NAVARB && B.jumpV === undefined && Math.floor(B.y) !== Math.floor(yPrev5)
            && !fishBodyAt(B.x, B.z, B.th, Math.floor(B.y), B.fhalf) && fishBodyAt(B.x, B.z, B.th, Math.floor(yPrev5), B.fhalf)) { B.y = yPrev5; navVetK[6]++; }   // ── VERTICAL, SAME PREDICATE ── stepOK validates the body at the PRE-step y; the depth ease then moved y with no body test at all, so a move approved at the old height could clip at the new one. The hitbox below reverted the pose and the fish "moved" every frame while sitting still (measured: 60 s at spd 8 with noMove reading 0). Vetoing the y step makes that oscillation unreachable instead of catching it afterwards. A LEAP is exempt — its arc is real ballistics.
        B.vyS = (B.vyS || 0) + ((B.y - yPrev5) / Math.max(dt, 1e-4) - (B.vyS || 0)) * (1 - Math.exp(-6 * dt));   // low-passed climb/dive — drives the SUBTLE nose pitch in the render frame below
      } else if (B.kind >= 3) {                        // DUCK / LILY: ride the CONTINUOUS swell (JS mirror of the TRACE wave field) — smooth by construction, no quantized ratcheting
        let wvMax = 0;
        if (freezeK < 0.4) {                           // waves are gated exactly like the renderer — a frozen lake is flat
          const wvAt = (wx, wz) => gerstHJS(Math.floor(wx), Math.floor(wz), tb3);   // the GERSTNER mirror — bit-matched to the shader's gerstH, so floats ride EXACTLY the drawn surface
                                                       // MAX over the body FOOTPRINT — the waves are per-column, and a crest under an edge column must lift the whole float
          const fr9 = B.kind === 4 ? 4 : 2.5;          // lily pads are WIDE and 1 voxel thin — the footprint must reach the pad edge or an edge-column crest swallows the whole pad
          wvMax = Math.max(wvAt(B.x, B.z), wvAt(B.x + fr9, B.z), wvAt(B.x - fr9, B.z), wvAt(B.x, B.z + fr9), wvAt(B.x, B.z - fr9));
          if (B.kind === 4) { const fd9 = 2.8; wvMax = Math.max(wvMax, wvAt(B.x + fd9, B.z + fd9), wvAt(B.x - fd9, B.z + fd9), wvAt(B.x + fd9, B.z - fd9), wvAt(B.x - fd9, B.z - fd9)); }
          wvMax += 0.5;                                // +0.5 = the renderer's floor(wv + 0.5) worst case — the float stays AT or ABOVE the drawn surface at all times
        }
        const base9 = B.kind === 4 ? WL + 1.6 + 0.5 * (LILY_SZ[B.col % Math.max(1, LILY_SZ.length)] || 1)   // lily rests ON the surface with clearance — a 1-voxel-thin pad has NO sacrificial bottom row
          : (isBaby ? WL + 1.9 : WL + 3.4);            // ducks: ONE VOXEL LOWER (user 2026-08-05) — they sat too high on the surface; the bottom row now dips properly   // ducks: ride high — only (part of) the BOTTOM voxel row ever dips
        // ── HALF THE SWAY (user 2026-08-05: "reduce the ducks up and down sway by 50%") ── damping wvMax
        // itself would drop the duck at every crest, because that number carries the renderer's +0.5 floor
        // that keeps the body AT or ABOVE the drawn surface. So the ride is damped about its OWN slow mean
        // instead: the average waterline is exactly what it was, only the excursion either side of it halves.
        // The mean is per-float and self-correcting, so it follows the duck across water of any roughness.
        B.wvM = B.wvM === undefined ? wvMax : B.wvM + (wvMax - B.wvM) * (1 - Math.exp(-0.08 * dt));  // ~12 s low-pass. It has to be well SLOWER than the swell itself (GWOM puts the four waves at 3-4 s periods) or the mean tracks the very rise and fall it exists to average out — at 2 s the damping came out 43%%, not 50%%
        const wvUse = B.kind === 3 ? B.wvM + (wvMax - B.wvM) * DUCK_SWAY : wvMax;   // lilies keep the full ride — a pad sits flat ON the surface and reads wrong if it lags the swell
        const tgt9 = base9 + wvUse;
        const ek9 = B.kind === 4 ? 14 : 5, ec9 = B.kind === 4 ? 10 : 7;   // lily tracks TIGHT (lag ~0.2 vox — it has no draft to hide lag in); ducks keep the lazy bob-along ease
        B.y += Math.max(-ec9 * dt, Math.min(ec9 * dt, (tgt9 - B.y) * (1 - Math.exp(-ek9 * dt))));   // ONE symmetric ease — the swell moves ≤ ~2.5 vox/s, so this tracks it closely and SMOOTHLY
      } else if (B.trap > 0.5 && !NAVARB) {            // STUCK: dive for the open understory when there's air below, else flutter up if the sky above is clear. RETIRED under the arbiter: it is a third writer of y that answers to a counter rather than to the field, and it parks a roofed flyer at ground+8 UNDER the canopy it is trying to leave.
        if (!bfObst(B.x, B.y - 2, B.z) && B.y - 2 > gLoc + 6) B.y -= 34 * dt;
        else if (!bfRoofed(B.x, B.y, B.z)) B.y += 40 * dt;
      } else if (desBee && B.beeM === 5) {             // ── ENRAGED: HOLD THE PLAYER'S OWN HEIGHT ──
        // The cruise line is a property of the TERRAIN (ground memory + 12), which is the right answer for
        // anything wandering and the wrong one for something aimed at a person: on a slope, on a rock, on a
        // roof, the player's feet and the local ground are different numbers, and the sting box is drawn round
        // the player. So the rage flies the PLAYER instead — the same eased, rate-capped, budget-capped write
        // the cruise branch makes, only against a different target. gAir + 3 is the floor so it cannot chase
        // you into the dirt, and B.gRef is still maintained here so the ordinary servo resumes from a live
        // ground memory the instant the rage ends rather than snapping off a stale one.
        // ── …AND THE SWARM DOES NOT ARRIVE IN ONE PLANE ── the closer already spreads the bees round the
        // player horizontally; without this they would all do it at exactly P.y + BEE_RAGE_Y and read as a
        // ring cut out of card. Same per-bee B.beePh, same shape as the hive orbit's own vertical term, and
        // it is free: ±BEE_ATK_YS about 12 is 9.4-14.6, which the sting box (P.y−3 .. P.y+HEIGHT+3) contains
        // with 11 voxels to spare, so no bee can ever be spread OUT of a sting it would otherwise land.
        B.gRef = Math.max(gAir, (B.gRef || gAir) - 9 * dt);
        const tgtR5 = Math.max(gAir + 3, P.y + BEE_RAGE_Y + Math.sin(B.beePh * 1.7 + wk) * BEE_ATK_YS);
        B.beeGA = gAir; B.beeTgt = tgtR5;              // read by window.__vbBee (sim/life/slots.js) — mode 5 only, so an ordinary bee pays nothing
        if (!lbDown) B.y += Math.max(-26 * dt, Math.min(30 * dt, (tgtR5 - B.y) * (1 - Math.exp(-4 * dt))));   // a LANDING ladybug is exempt: this servo pulls toward the cruise altitude at 4/s against the landing ease's 2.2/s, so the two just met at an equilibrium ~20 voxels up and the arrival test never fired (MEASURED: stuck at 211.84 against a ground of 191, which is exactly gAir + 6 + DES_FLY_UP)
      } else {
        B.gRef = Math.max(gAir, (B.gRef || gAir) - 9 * dt);   // GROUND MEMORY: rises instantly with terrain, sinks slowly — stays HIGH crossing gorges (no diving in) but settles to the
        const cruise = B.gRef + (B.kind ? 8 : 12);            // local ground on flats/slopes, so the cruise line sits in the open UNDERSTORY below the canopy ('caught on trees' fix —
        let stepY = (cruise - B.y) * (1 - Math.exp(-4 * dt)); // the old terrain-max stencil pinned cruise AT canopy height on any slope)
        stepY = Math.max(-26 * dt, Math.min(30 * dt, stepY)); // RATE-CAPPED — a reference jump must never become a visible teleport
        if (!lbDown && !(stepY > 0 && (bfObst(B.x, B.y + 3, B.z) || bfObst(B.x, B.y + 6, B.z)))) B.y += stepY;   // …and a LANDING ladybug is exempt from this one too: it is a FOURTH altitude authority (ground-memory cruise, gRef + 8), separate from the tgtR5 servo and the two floors, and with only those three suppressed a descent settled at exactly gRef + 7 and timed out every time (MEASURED: 795 'down' samples, 0 landings)   // never ease UP into foliage overhead (two probes — gappy pine crowns fooled one)
        if (!lbDown && B.y < B.gRef + 7 + flyUp) B.y = Math.min(B.gRef + 7 + flyUp, B.y + 34 * dt);   // floors are APPROACHED at climb speed, never snapped   // floors are APPROACHED at climb speed, never snapped
      }
      if (!lbDown && B.kind < 2 && B.y < gAir + 6 + flyUp) B.y = Math.min(gAir + 6 + flyUp, B.y + 34 * dt);   // the fly's floor rides DES_FLY_UP above every other flyer's   // absolute local-ground floor (FLYERS only — a worm lives at ground+2, a duck at the waterline); gAir so the floor and the feasibility predicate agree on where the ground is
      if (NAVARB && B.kind < 2 && B.y !== yPrev5 && !navFitsAir(B.x, B.y, B.z) && navFitsAir(B.x, yPrev5, B.z)) { B.y = yPrev5; navVetoY++; }   // ── VERTICAL, SAME PREDICATE ── the altitude servo is the flyer's other motion axis and it used to write y with no feasibility test at all. It is not a teleport, so it is not rewritten here; it is VETOED by navFitsAir, so both axes now answer to one predicate.
      if (B.jumpV === undefined) { const yDn9 = yBudD > 30 ? yBudD : 30, yUp9 = yBudD > 34 ? yBudD : 34;
        B.y = Math.max(yPrev5 - yDn9 * dt, Math.min(yPrev5 + yUp9 * dt, B.y)); }   // GLOBAL vertical budget: whatever the branches above did, the frame's total climb/descent stays at flutter speed — teleports impossible. A LEAPING salmon is exempt: its arc is real ballistics and this cap would flatten the rise and make the fall float.
      // ══ A BEE HOLDING STATION ══ the two states in which the bee's pose is PLACED rather than integrated, and
      // it happens here, after every servo and every veto, for the reason the ant followers are placed here: a
      // second controller arguing with the mover is the bug this codebase keeps re-learning, so there is no
      // argument — the mover runs untouched and its answer is simply overwritten for these two states. The stall
      // counters are cleared with it, or a bee that is deliberately stationary would bank 12 s of `trap` and
      // mercy-recycle itself mid-visit.
      // BOTH POSES ARE navFitsAir-LEGAL BY CONSTRUCTION, which is why neither needs the veto: the predicate
      // vouches for y >= nvY + 2, a bloom's HEAD voxel is the second voxel above nvY (fillColumn puts a stalk in
      // the first air voxel and the head on top of it), and the bee sits on top of THAT — so its lowest voxel is
      // already a voxel clear of the band's floor. The orbit hangs in the crown band, higher still.
      // ── AND THE BUNCH IS HELD, NOT STEERED ── the same kinematic circle the bee's hive orbit uses two blocks
      // down, for the same reason: a swarm that STEERS toward a point overshoots it and comes apart, while one
      // that rides a phase on a ring is a bunch by construction. Each fly has its own phase and the vertical
      // term runs at 1.7x it, so they weave past each other instead of rising together.
      // The mover above ran and its answer is overwritten, which is what the bee block's own note argues for:
      // no second mover, just two states whose position is decided rather than integrated.
      if (desFly && B.bn) {
        const A9 = B.bn;
        if (A9.st !== tb3) {                           // ONE step per frame for the whole bunch, whoever gets here first
          A9.st = tb3;
          if (tb3 > A9.tRe) { A9.omT = (Math.random() - 0.5) * FLY_BN_TURN * 2; A9.tRe = tb3 + 1.6 + Math.random() * 2.4; }
          const dx9 = A9.x - A9.ox, dz9 = A9.z - A9.oz;
          if (dx9 * dx9 + dz9 * dz9 > FLY_BN_R * FLY_BN_R) {   // outside its roam: steer for home on the SHORT way round, rather than snapping or bouncing
            let e9 = Math.atan2(-dx9, -dz9) - A9.th;
            e9 = Math.atan2(Math.sin(e9), Math.cos(e9));
            A9.omT = Math.max(-2, Math.min(2, e9 * 2));
          }
          A9.om += (A9.omT - A9.om) * (1 - Math.exp(-6 * dt));
          A9.th += A9.om * dt;
          A9.x += Math.sin(A9.th) * FLY_BN_SPD * dt;
          A9.z += Math.cos(A9.th) * FLY_BN_SPD * dt;
          // ── AND AT THE FLY'S OWN CRUISE LANE, NOT THE BUTTERFLY'S (user 2026-08-22: "the flies seem to be
          // very low to the ground, bring them up where around the butterflies are") ── this rode bfSurf + 6,
          // which is the BASE flyer floor with DES_FLY_UP left off, so the bunch sat 16 voxels below the lane
          // that constant exists to put a fly in. The kinematic ring below overwrites B.y outright, so the
          // real floor sites (the `gAir + 6 + flyUp` lines above) never get a say for a bunched fly — the
          // anchor has to carry the whole altitude rule itself. gAir, not bare ground: with NAVARB on, a
          // flyer's ground is the nav field's top-of-air, so a bunch drifting under a crown clears it instead
          // of threading through the canopy.
          const gr9 = bfSurf(A9.x, A9.z);
          const ga9 = (NAVARB && nvOn) ? Math.max(gr9, nvTopAir(nvIdx(A9.x, A9.z))) : gr9;
          const g9 = ga9 + 6 + DES_FLY_UP;
          A9.y += (g9 - A9.y) * (1 - Math.exp(-1.5 * dt));
        }
        B.bnPh += BEE_ORBIT_W * dt;
        B.x = A9.x + Math.sin(B.bnPh) * BEE_ORBIT_R;
        B.z = A9.z + Math.cos(B.bnPh) * BEE_ORBIT_R;
        B.y = A9.y + Math.sin(B.bnPh * 1.7 + wk) * BEE_ORBIT_Y;
        // ── THE RING IS KINEMATIC, SO IT HAS TO CHECK WHAT IT FLIES INTO (user 2026-08-22: "flys seem to be
        // bugged completely. stuck in trees") ── the anchor clears the canopy at ITS OWN column (nvTopAir
        // above), but a ring member sits BEE_ORBIT_R away, where the crown can be taller — and because the
        // position is assigned rather than integrated, nothing else in the tick ever gets to veto it. MEASURED:
        // 1 of 5 flies sitting inside a leaf (id 195). Lift it out along +y, bounded, which is the one
        // direction guaranteed to leave a crown.
        for (let s9 = 0; s9 < 14 && bfObst(B.x, B.y, B.z); s9++) B.y += 1;
        B.th = B.bnPh + 1.5708;                        // face along the ring, the one place a bunched fly's heading is set
        Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
        B.om = 0; B.omT = 0; B.trap = 0; B.noMove = 0;
      }
      // ── THE WORLD LADYBUG LANDS, LIKE THE EDITOR ONE (user 2026-08-22: "im not 100% if the ladybug even
      // lands? the real world ladybug should match the asset editor ladybug") ── it was registered as a
      // DES_FLYER, which gave it the fly's whole air behaviour and nothing else, so it never came down. Same
      // four states and the same timings as the exhibit in ui/editor.js: it cruises 6-16 s, settles, sits
      // 3-7 s holding frame 00, then climbs back. Ground is gAir, not bare terrain, so it sets down on top of
      // whatever it is over rather than sinking into a crown. A frightened ladybug abandons a landing and
      // climbs, which is the exhibit's rule too — B.fleeT is the flyer flee the butterfly and fly share.
      // ── THE WORLD LADYBUG LANDS, LIKE THE EDITOR ONE (user 2026-08-22) ── and it does it WITHOUT ever
      // flying into a tree. A descent has to suspend the three altitude authorities that hold a flyer above the
      // canopy — the cruise servo and the two floors above — because they are what makes it a flyer; there is
      // no way to reach the ground with them running. Suspending them blindly is what wedged it in crowns.
      // So the column is PROVEN CLEAR before the descent is committed: a straight-down probe from the body to
      // the surface, no higher than LBUG_DROP_MAX, and if anything is in the way it simply keeps flying and
      // asks again in a couple of seconds. Once committed there is still a deadline (LBUG_DROP_MAX_S) and an
      // abort if something moves under it, so a descent can never become a permanent snag.
      // Everything else about its flight is the butterfly's, untouched: flyUp is 0 for it and the horizontal
      // wander is the shared kind-0 arbiter (user, three times: "the exact same flight mechanics as the butterfly").
      if (desLbug) {
        if (!B.lbPh) { B.lbPh = 'fly'; B.lbNext = tb3 + 6 + Math.random() * 10; }
        const flee9 = tb3 < (B.fleeT || 0);
        const cruise9 = gLoc + LBUG_CRUISE;
        if (B.lbPh === 'fly') {
          if (!flee9 && tb3 > B.lbNext) {
            // ── AND IT LANDS ON OPEN GROUND, NOT ON A TREETOP ── bfSurf is the top of the COLUMN, which under a
            // pine is the canopy: MEASURED, every one of 135 landings sat 14-31 voxels above the terrain, i.e.
            // on the crown. That reads as "it never lands" from the ground (user, twice) and it is also what
            // put it among the branches. Requiring the column's surface to be within a few voxels of the
            // TERRAIN confines landings to clearings — visible, and with nothing to snag on on the way down.
            const terr9 = H(Math.round(B.x), Math.round(B.z));
            let clear9 = (B.y - gLoc) < LBUG_DROP_MAX && (gLoc - terr9) < 3;
            for (let y9 = Math.floor(B.y); clear9 && y9 > gLoc + 1; y9--) if (bfObst(B.x, y9, B.z)) clear9 = false;
            // Pin the COLUMN at the moment of commit and come straight down it. Letting it keep drifting while
            // it descended is why it arrived high: the target height was computed for the column it committed
            // over, and by touchdown it was somewhere else entirely — MEASURED at 13-25 voxels above the ground
            // under it. Straight down also keeps the clear-column probe above honest, since that probe tested
            // exactly this column and no other.
            if (clear9) { B.lbPh = 'down'; B.lbGy = gLoc + 1; B.lbX = B.x; B.lbZ = B.z; B.lbT = tb3 + LBUG_DROP_MAX_S; }
            else B.lbNext = tb3 + 2 + Math.random() * 3;   // canopy overhead — keep flying, ask again shortly
          }
        } else if (B.lbPh === 'down') {
          if (flee9 || tb3 > B.lbT || (B.y - 1.5 > B.lbGy && bfObst(B.x, B.y - 1.5, B.z))) B.lbPh = 'up';   // startled, took too long, or something moved in UNDER it — the height test matters: without it the probe reads the very ground it is landing on as an obstruction and every descent aborted a metre short (MEASURED: 866 'down' samples, 0 landings)
          else if (B.y - B.lbGy < 0.5) { B.lbPh = 'land'; B.y = B.lbGy; B.lbNext = tb3 + 3 + Math.random() * 4; }   // lbX/lbZ were already pinned at commit
        } else if (B.lbPh === 'land') {
          if (flee9 || tb3 > B.lbNext) B.lbPh = 'up';
        } else if (B.lbPh === 'up' && B.y > cruise9) { B.lbPh = 'fly'; B.lbNext = tb3 + 6 + Math.random() * 10; }
        if (B.lbPh === 'down') { B.x = B.lbX; B.z = B.lbZ; B.y += (B.lbGy - B.y) * (1 - Math.exp(-2.2 * dt)); B.om = 0; B.omT = 0; }
        else if (B.lbPh === 'land') { B.y = B.lbGy; B.x = B.lbX; B.z = B.lbZ; B.om = 0; B.omT = 0; B.trap = 0; Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th); }   // sitting still: the heading it landed on is the heading it keeps
        else if (B.lbPh === 'up') B.y += (cruise9 - B.y) * (1 - Math.exp(-2.2 * dt));
      }
      // ── THE FROG HOPS (user 2026-08-22: "pursue implementing the frog in") ── it is a kind-2 ground creature
      // and the walker above has already slid it along, which is exactly what a frog must not do. The leap is
      // PRESCRIBED, not integrated: FROG_OFF (sim/life/slots.js) holds all three cycles' per-frame offsets as
      // ui/editor.js aligned them by hand against the stage, [up, forward], and playing the hop carries it one
      // metre forward and five voxels up over 17 frames. Between leaps it sits perfectly still, which is what
      // makes the hop read as a hop — a frog that drifts between jumps just looks like it is skating.
      // The frame is driven from here too (B.fgF), because the shared kind-2 frame clock would run the cycle
      // continuously and the animation would not line up with the travel it is supposed to be causing.
      // ── THE FROG IS THE EDITOR'S FROG (user 2026-08-22: "it should be an exact copy") ── same file, same
      // three cycles, same weights: hop 50 / ribbet 40 / tongue 10 (FROG_MIX). Only the HOP carries it
      // anywhere; ribbet and tongue play where it sits, which is what stops it looking like it is permanently
      // mid-jump. Structured like the BUNNY, which the user pointed at: position is ASSIGNED from an anchor
      // plus the baked per-frame offset rather than integrated, and the ground comes from navWalkStand — the
      // walkable standing height. bfSurf is the top of the COLUMN, which under a crown is the canopy, and
      // using it is what had the frog clipping through terrain.
      // `!B.dying && !B.slain` is the same guard the ant and the bee blocks carry, and the frog needs it for a
      // sharper reason than tidiness: this block ASSIGNS B.x/B.y/B.z from the anchor every frame, so a frog on
      // its killing blow was being snapped back on top of its own ragdoll and could never move again. A body
      // that cannot move is one the kind-2 walker escapes below eventually call wedged or islanded — and a
      // frog sits on a BANK, with water taking half of everything within reach, which is the shape that test
      // reads as islanded. That path retires the slot (B.init = false) with no death, and if it lands inside
      // the 500 ms death flash then reapDeaths finds the slot already empty, skips the teardown and never sets
      // B.slain — leaving the slot free for the population to refill. Hence "I killed a frog and a new one
      // spawned in its place" (user 2026-08-27). The reap is hardened too (sim/life/reactions.js); this is the
      // half that stops a dying frog wandering into that code at all.
      if (desFrog && FROG_CYC.length && !B.dying && !B.slain) {
        const fitF = DESERTS[desSp] ? MAMFIT[DESERTS[desSp].name] : null;
        // ── THE SEAT HAS TO BE PER-FRAME, BECAUSE THE BOX IS (user 2026-08-27: "the space positions are not
        // correct with the animations … it should match the asset editor frog but its not") ── the trace centres
        // a creature's model box on its anchor using THAT FRAME's own dimensions (the `+ vec3(ew2, ed2, eh2)` in
        // render/wgsl/trace.js), while mamFitOf measures exactly ONE frame — item0, the frog at rest, 5 tall.
        // Every other animal in the band is a constant box frame to frame (measured: all ten), so a single seat
        // has always been right for them and this has never bitten. The frog's frames run 5 to 10 tall, so the
        // constant sat the box centre still while the box grew around it and the feet went (h - 5) / 2 UNDER the
        // ground: 2.5 voxels at the top of the leap, half a voxel through the ribbet and the tongue's peak.
        // The asset editor never shows this because it anchors the other way — it stamps the model's z = 0 plane
        // on a FIXED floor (`ED.y + 1 + oy + model_z`, ui/editor.js) and lets the box grow upward from there. The
        // seat that reproduces that is exactly h / 2, and mamFitOf's -z0 term drops out on purpose: a frame
        // authored to leave the floor should leave it, because that lift IS the animation.
        // Frame 0 is arithmetically unchanged (h 5, z0 0 -> 2.5, the number the constant already carried), so the
        // resting pose does not move a voxel and only the frames that actually grow are corrected.
        // Horizontal needs no equivalent: the editor centres its lane the same way the trace does
        // (`bx = ED.x0 + ((ED.pw - rv.sx) >> 1)`), so the box centre there is independent of the frame size too.
        const frogIt = (fr9) => (itemsRef && itemsRef[FROG_ITEM0 + (fr9 | 0) - 1]) || null;
        const frogSeat = (fr9) => { const it9 = frogIt(fr9); return it9 ? it9.h * 0.5 : (fitF ? fitF.seat : 2); };
        // ── AND THE HALF VOXEL THE EDITOR ROUNDS AWAY (user 2026-08-27: "when the frog ribbets, it goes
        // backwards when it should be staying in place. same thing with the tongue") ── the stage centres a
        // frame in its lane with `(ED.pd - rv.sy) >> 1`, an INTEGER floor, while the trace centres the box on
        // an exact half. So a frame whose DEPTH is odd draws half a voxel further back here than it does on the
        // stage, and the frog's depth changes parity mid-cycle: the ribbet puffs 7 -> 8 and the tongue runs
        // 7, 8, 10, 11, 12, 13. Worked through, the editor's rear sits at a flat 2.0 for every frame of both
        // cycles and the world's alternates 2.5 / 2.0 — the editor is perfectly still and the world flickers.
        // That is also exactly why the leap was fine and these two were not: half a voxel is nothing against
        // ten voxels of travel, and it is the ONLY motion in a cycle that is supposed to hold position.
        // Returned as a RENDER offset rather than folded into B.x: the anchor is re-latched from B.x at every
        // cycle end, so baking half a voxel into the position would accumulate a real drift, one per cycle.
        // ── AND IT IS PER-FRAME, WHICH IS THE THING THAT HOLDS THE MODEL STILL ── I made this a constant once,
        // reasoning that a toggling correction must be the judder. That was backwards, and the numbers say so
        // plainly. The half voxel does not CAUSE the toggle, it CANCELS one: the trace centres the box at
        // anchor - d/2, so a frame whose depth is odd already sits half a voxel off its neighbours, both in
        // where the body lands and in where the model's voxel boundaries fall. Adding 0.5 on exactly those
        // frames makes anchor - d/2 keep a constant fractional part, so the body holds position AND the voxel
        // grid keeps one phase. Holding the 0.5 constant instead leaves the depth parity exposed on both.
        // MEASURED, body offset through the 24-frame tongue: the stage holds a flat -1.9 the whole way; with
        // this per-frame it is -1.9 flat too; with it held constant it oscillates -1.9 / -2.4 six times, which
        // is the "wrong positions again" report (user 2026-08-27) and is a judder rather than a placement.
        const frogPar = (fr9) => { const it9 = frogIt(fr9); return (it9 && (it9.d & 1)) ? 0.5 : 0; };
        // ── A BODY IS NOT A COLUMN (user 2026-08-27: "the frogs feet are clipping into the ground") ── the
        // frog seated on navWalkStand at its CENTRE, one point, while it is nine voxels across and seven deep.
        // Wherever the ground under an edge of that footprint stands higher than the middle, that edge is
        // buried — which is the feet, because the feet are the outermost thing it has. Every land mammal in
        // the game already answers this the other way: mamSeatG takes the MAX ground under the whole
        // footprint, oriented by the heading, and its own note records the identical report ("it appears to
        // clip through the terrain"). Using the mammals' function rather than a frog-shaped copy of it means
        // there is still one answer in the game to "how high does this body stand".
        // frogGAt is the same reduction at an arbitrary point, for testing where a leap will LAND: seating on
        // the footprint but choosing the landing on a point probe would put the frog down and then lift it.
        // ── …BUT THE MAX ALONE IS THE OTHER DITCH ── mamSeatG is written for a body big enough that standing
        // on the highest thing under it reads as standing on the ground. A frog is nine voxels by seven, and
        // on ordinary forest floor the max across that span runs 2-3 above the middle: MEASURED with the bare
        // mammal rule, the feet had solid directly beneath them in only 22% of resting frames and the frog
        // hovered a voxel or two over every dip it stood in. So the seat is the CENTRE's ground, lifted only
        // as far as an edge really demands and never more than a voxel: max(centre, edgeMax - 1). That bounds
        // the float at one voxel and the bury at one voxel, which for a body this size is the closest either
        // can get to right — where the ground under it varies by more than that, no single height is correct.
        const frogGAt = (px, pz) => {
          const hx = Math.sin(B.fgTh || 0), hz = Math.cos(B.fgTh || 0);
          const hd9 = fitF ? fitF.hd : 3, hw9 = fitF ? fitF.hw : 2;
          const c9g = navWalkStand(px, pz);
          let g = -1e9;
          for (let u = -1; u <= 1; u++) for (let v = -1; v <= 1; v++) {
            const q = navWalkStand(px + hx * hd9 * u + hz * hw9 * v, pz + hz * hd9 * u - hx * hw9 * v);
            if (q !== undefined && q > g) g = q; }
          if (g < -1e8) return c9g;
          return c9g === undefined ? g : Math.max(c9g, g - 1); };
        const seatF = frogSeat(0);                     // the RESTING seat — frame 0, which is what the sit, the settle and the landing-clearance probe below all ask about
        // ── ON THE GRID FROM THE FIRST FRAME (user 2026-08-27: "aligned to the grid and not allowed to rotate
        // off the grid much like the bunny ... only rotates at 90 degree angles") ── the bunny's own two lines:
        // a heading INDEX rather than an angle (its B.bh) and an INTEGER base cell (its B.bpx/B.bpz). The frog
        // already turned in exact quarter turns, but it inherited whatever bearing the spawner left in B.th,
        // so its lattice was square to nothing and its anchor sat between voxels. Snapping both here is the
        // whole of it: every hop after this is a whole number of voxels along a cardinal, for ever.
        if (B.fgP === undefined) { B.fgP = -1; B.fgC = -1; B.fgF = 0;
          B.fgH = ((Math.round((B.th || 0) / FROG_TURN) % 4) + 4) % 4;   // nearest cardinal, the bunny's own expression
          B.fgTh = B.fgH * FROG_TURN;
          B.fgX = Math.round(B.x); B.fgZ = Math.round(B.z); B.x = B.fgX; B.z = B.fgZ;
          B.fgHX = B.fgX; B.fgHZ = B.fgZ; B.fgTurn = 0; B.fgG = frogGAt(B.fgX, B.fgZ);
          B.fgRest = tb3 + FROG_REST_MIN + Math.random() * (FROG_REST_MAX - FROG_REST_MIN); }
        const fdir = FROG_DIR[B.fgH | 0] || FROG_DIR[0];   // [sin, cos] of this heading, EXACTLY — never trig of the angle
        // ── THE FROG KEEPS ITS OWN HEADING (user 2026-08-27: "dont rotate the frog yourself. keep it
        // straight") ── B.th is rewritten every frame by the shared walker steering ~600 lines above, which is
        // what was quietly swinging the frog about between and during its cycles. B.fgTh is the frog's own
        // heading and the ONLY thing allowed to move it is the turning hop below, in 90 degree steps. Pinned
        // here at the top of the block so every branch under it — the sit, the leap, the emit's half-voxel —
        // reads one settled direction rather than whatever the steering happened to leave behind.
        B.th = B.fgTh;   // fgHX/fgHZ is the BANK it was spawned on, and the only thing that keeps it there — see the leash in the leap below
        if (B.fgP < 0) {                               // ── SITTING ── pinned on its anchor, holding frame 0
          B.x = B.fgX; B.z = B.fgZ; B.y = B.fgG + seatF; B.fgF = 0;
          B.fgPar = frogPar(0);                        // the RESTING frame — the sit holds frame 0
          B.om = 0; B.omT = 0; B.trap = 0;
          if (tb3 > B.fgRest && !(tb3 < (B.fleeT || 0))) {
            let w9 = Math.random() * FROG_MIX_SUM, c9 = 0;   // the WHOLE table (sim/life/slots.js) — adding the entries by hand here is what kept the fourth cycle from ever being drawn
            while (c9 < FROG_MIX.length - 1 && w9 > FROG_MIX[c9]) { w9 -= FROG_MIX[c9]; c9++; }
            if (c9 >= FROG_CYC.length) c9 = 0;
            if (FROG_CYC[c9].move) {                   // a LEAP has to have somewhere to land: no water, no cliff, nothing in the way
              // ── THE LEAP GOES STRAIGHT AHEAD, OR NOT AT ALL (user 2026-08-27: "dont rotate the frog
              // yourself. keep it straight") ── this used to try six headings, five of them fully random, and
              // take the first it could land on. That IS the rotating the user asked to stop: it re-aimed the
              // animal on most leaps, and the turn happened between two frames with no animation behind it.
              // A hop now tests exactly ONE direction, the one the frog already faces; a turning hop tests
              // where its own 90 degrees will actually put it. Refusing is the honest outcome when the way
              // ahead is blocked — the frog sits, and the next draw is a fresh roll of the mix. That is what
              // the rotate cycle is FOR, and it is why it needs no random search to fall back on.
              // the CYCLE'S OWN table, not the hop's: a turn now carries no forward column at all, so far and
              // fw8 are both 0 and the test below asks about the square the frog is already standing on —
              // which is exactly right, and is what makes a turn the one move that is always available.
              // where a cycle of kind `cc` would put the frog down, for a given turn. A turning hop leaves on
              // the OLD heading and finishes on the new one, so its landing is the two legs end to end; the
              // straight hop is one leg. The LEASH lives in here too, and only ever on a straight hop — see
              // the note at FROG_LEASH: refusing a turn is what strands a frog rather than what holds it.
              const landOK = (cc, tn) => {
                const TT = FROG_OFF[FROG_CYC[cc].name] || FROG_OFF.hop;
                const far = -TT[TT.length - 1][1], fw8 = -TT[FROG_TURN_FRAME][1];
                const d2 = FROG_DIR[(((B.fgH | 0) + (tn > 0 ? 1 : 3)) % 4)];   // where the quarter turn would point
                const ax = B.x + fdir[0] * (tn ? fw8 : far) + (tn ? d2[0] * (far - fw8) : 0);
                const az = B.z + fdir[1] * (tn ? fw8 : far) + (tn ? d2[1] * (far - fw8) : 0);
                const gg = frogGAt(ax, az);         // the footprint's ground, the same reduction the seat uses
                if (bfWater(ax, az) || gg === undefined || Math.abs(gg - B.fgG) > 6 || bfObst(ax, gg + seatF + 1, az)) return null;
                B.fgG1try = gg;                         // the ground the leap is aimed AT — see the arc below
                if (!tn && B.fgHX !== undefined) {
                  const d0 = Math.hypot(B.x - B.fgHX, B.z - B.fgHZ), d1 = Math.hypot(ax - B.fgHX, az - B.fgHZ);
                  if (d0 > FROG_LEASH && d1 > d0) return null;
                }
                return [ax, az]; };
              const tryCycle = (cc) => {                // → the turn it settles on, or null if there is nowhere to put it
                const tn = FROG_CYC[cc].turn ? (Math.random() < 0.5 ? -FROG_TURN : FROG_TURN) : 0;
                if (landOK(cc, tn)) return tn;
                if (tn && landOK(cc, -tn)) return -tn;   // the coin picks first; the other way is the fallback, so 50/50 holds wherever both are clear
                return null; };
              let turn9 = tryCycle(c9), ok9 = turn9 !== null;
              // ── A HOP WITH NOWHERE TO GO BECOMES A TURN ── refusing outright costs 0.6 s of sitting, and with
              // the heading pinned a frog facing the water refuses EVERY forward hop, so those pauses stack and
              // the only way out is the one draw in ten that happens to be a turn. MEASURED with the rest
              // removed: one hop landed in 33 cycles, still 43% idle, and the mix collapsed onto the two cycles
              // that cannot refuse. Turning is the honest answer to "not that way" and is what the turn cycle
              // is for, so a blocked forward hop is re-offered as one before the frog gives up on the beat.
              if (!ok9 && !FROG_CYC[c9].turn) {
                for (let r9 = 0; r9 < FROG_CYC.length; r9++) if (FROG_CYC[r9].turn) {
                  const tn = tryCycle(r9);
                  if (tn !== null) { c9 = r9; turn9 = tn; ok9 = true; }
                  break; } }
              if (ok9) B.fgTurn = turn9;
              else { B.fgRest = tb3 + 0.6; c9 = -1; }   // boxed in every way — sit, and try again shortly
            }
            if (c9 >= 0) { B.fgC = c9; B.fgP = 0; B.fgX = Math.round(B.x); B.fgZ = Math.round(B.z); B.fgH0 = B.fgH | 0; B.fgTh0 = B.fgTh; B.fgG1 = B.fgG1try !== undefined ? B.fgG1try : B.fgG; }   // the anchor is re-latched as an INTEGER cell, the bunny's B.bpx   // fgG1 = the ground under the LANDING, which is what the arc is flown between (see below)   // fgTh0 = the heading this cycle STARTED on; the pivot is written off it every frame rather than added once, so it cannot drift
          }
        } else {                                       // ── PLAYING A CYCLE ── frame and position both read off it
          const C9 = FROG_CYC[B.fgC] || FROG_CYC[0];
          B.fgP += FROG_HOP_FPS * dt;
          const i9 = Math.max(0, Math.min(C9.n - 1, Math.floor(B.fgP)));
          B.fgF = C9.off + i9;
          // EVERY cycle gets its own table, not just the hop: ribbet and tongue lean the body and come back,
          // and drawing them without their offsets is what put the frog's alignment out. One expression for all
          // three — the hop is simply the one whose table does not return to zero.
          const T9 = FROG_OFF[C9.name], o9 = (T9 && T9[Math.min(T9.length - 1, i9)]) || FROG_ZERO;
          const fwd9 = -o9[1], far9 = -T9[T9.length - 1][1];   // far9 = the whole cycle's travel, so fwd9/far9 is how far along the leap this frame is
          // ── THE 90 DEGREES GOES IN AT THE TOP OF THE LEAP ── written FROM fgTh0 each frame rather than
          // added once, so it is idempotent: a frame that runs twice, or a cycle re-entered mid-flight, ends
          // on the same heading instead of turning again. Before the pivot frame the frog is still on its old
          // heading and after it the new one, and the travel is the two legs added end to end. Without that
          // split the whole displacement would swing onto the new heading in a single frame and the frog would
          // jump sideways by however far it had already leapt.
          if (C9.turn && B.fgTh0 !== undefined) {
            const past9 = i9 >= FROG_TURN_FRAME, step9 = (B.fgTurn || 0) > 0 ? 1 : 3;
            B.fgH = past9 ? (((B.fgH0 | 0) + step9) % 4) : (B.fgH0 | 0);   // a quarter turn is ONE STEP of the index — it can never land between cardinals
            B.fgTh = B.fgH * FROG_TURN; B.th = B.fgTh;
            const dA = FROG_DIR[B.fgH0 | 0], dB = FROG_DIR[((B.fgH0 | 0) + step9) % 4];
            const fw8 = -(FROG_OFF[C9.name] || FROG_OFF.hop)[FROG_TURN_FRAME][1];   // this cycle's own table — 0 for the in-place turn, so both legs collapse and the frog pivots on its anchor
            const legA = Math.min(fwd9, fw8), legB = Math.max(0, fwd9 - fw8);
            B.x = B.fgX + dA[0] * legA + dB[0] * legB;
            B.z = B.fgZ + dA[1] * legA + dB[1] * legB;
          } else {
            B.x = B.fgX + fdir[0] * fwd9; B.z = B.fgZ + fdir[1] * fwd9; }   // integer anchor + integer offset along a cardinal = the frog never leaves the lattice
          // ── THE ARC IS FLOWN BETWEEN THE TWO GROUNDS, NOT OVER THE TAKE-OFF ONE (user 2026-08-27: "when the
          // frog lands, it glitches out on the landing" / "sometimes when the frog jumps it completely clips
          // through the land ... look into the bunny") ── the bunny is the right model and the difference is
          // one line of its jump: it advances its base cell and re-derives B.bg at the DESTINATION the moment
          // the hop starts, so its arc rides where it is going. The frog held the take-off ground for the whole
          // leap and only re-derived at the end, which is both symptoms at once — hopping downhill it landed
          // high and snapped down a voxel or three on the last frame, and hopping uphill it flew straight
          // through the rising bank. Interpolating between the two by how far the leap has carried it keeps
          // the clean parabola (the baseline just tilts) and there is nothing left to snap: the last frame
          // already IS the landing ground.
          // …and a floor under it, because a tilt cannot answer a HUMP. navWalkStand under the frog's own
          // feet is the same plane every one of its tests uses, so a leap can graze a rise rather than pass
          // through it. One extra probe per frame per frog, and there are two of them.
          const gA9 = B.fgG, gB9 = (B.fgG1 === undefined ? B.fgG : B.fgG1);
          const fr9 = far9 > 0 ? Math.min(1, Math.max(0, fwd9 / far9)) : 0;
          const gN9 = frogGAt(B.x, B.z);            // the footprint again, not the centre column — see frogGAt above
          let base9 = gA9 + (gB9 - gA9) * fr9;
          // ── …AND ONLY WHILE IT IS ACTUALLY TRAVELLING (user 2026-08-27: "when the frog sticks out the tongue
          // or ribbets, it raises the frog up by 1 voxel? doesnt happen everytime just sometimes") ── the floor
          // is there to stop a LEAP passing through a rise, and it re-asks the ground wherever the frog has got
          // to. A ribbet and a tongue do not go anywhere: every one of their frames carries up = 0 and they only
          // LEAN, the body tipping forward over feet that never leave their own square. Re-sampling under the
          // leaned body reads the ground one voxel ahead, and where that happens to be a voxel higher the floor
          // lifts the whole animal — which is the report, and it is intermittent because it depends entirely on
          // what is in front of it. MEASURED: 1 ribbet in 20 rose, by exactly 1 voxel, on a cycle whose own
          // table says its base must not move at all. `far9 > 0` is the honest test for "this cycle travels":
          // it is the cycle's own total displacement, 10 for a hop and 0 for everything that holds position.
          if (far9 > 0 && gN9 !== undefined && gN9 > base9 + o9[0]) base9 = gN9 - o9[0];   // never below the ground under it
          B.y = base9 + frogSeat(B.fgF) + o9[0];       // …and THIS one is the frame being drawn, so the seat tracks the box it is seating (see frogSeat above)
          B.fgPar = frogPar(B.fgF);                    // the frame actually being drawn — see frogPar above for why this must not be a constant
          B.om = 0; B.omT = 0; B.trap = 0;
          if (B.fgP >= C9.n) {                         // finished — commit where it ended up and settle
            B.fgP = -1; B.fgX = Math.round(B.x); B.fgZ = Math.round(B.z); B.x = B.fgX; B.z = B.fgZ; B.fgF = 0;
            const gN = frogGAt(B.x, B.z); if (gN !== undefined && gN > -1e8) B.fgG = gN;   // re-derive at the NEW anchor, exactly as the bunny does after a baked jump — and on the footprint, so the settle agrees with the seat
            B.fgG1 = B.fgG;                             // the arc has already landed ON this ground, so there is nothing left for the settle to move
            B.y = B.fgG + seatF;
            B.fgRest = tb3 + FROG_REST_MIN + Math.random() * (FROG_REST_MAX - FROG_REST_MIN);
          }
        }
        Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
      }
      if (desBee && (B.beeM === 2 || B.beeM === 4)) {
        if (B.beeM === 2) {                            // SITTING: pinned over the bloom's own column, easing down onto the head. BEE_DOWN rather than a snap so the last of the approach reads as a landing, and the same ease lifts it off when the clock runs out.
          B.x = B.beeTx; B.z = B.beeTz;
          const sy = B.beeTy + 1 + ((MAMFIT.bee && MAMFIT.bee.seat) || 1.5);   // head voxel top, plus the model's own measured seat — the same mamFitOf number the walkers stand on, so the bee rests ON the flower instead of hovering at a hand-picked offset that would be wrong the moment the art changes
          B.y += Math.max(-BEE_DOWN * dt, Math.min(BEE_DOWN * dt, sy - B.y));
        } else {                                       // ORBITING: a real circle, held kinematically. BEE_ORBIT_W x BEE_ORBIT_R is 12 vox/s against the bee's own 56, so it reads as hovering; the vertical term spreads the swarm through the crown rather than stacking it in one plane.
          B.beePh += BEE_ORBIT_W * dt;
          B.x = B.beeTx + Math.sin(B.beePh) * BEE_ORBIT_R;
          B.z = B.beeTz + Math.cos(B.beePh) * BEE_ORBIT_R;
          B.y = B.beeTy + Math.sin(B.beePh * 1.7 + wk) * BEE_ORBIT_Y;
          B.th = B.beePh + 1.5708;                     // face along the orbit — the ONE place the bee's heading is assigned, and only while it is not being steered at all
          Hx2 = Math.sin(B.th); Hz2 = Math.cos(B.th);
        }
        B.om = 0; B.omT = 0; B.trap = 0; B.noMove = 0;
      }
      if (B.kind === 6) {                              // ── FISH TERRAIN HITBOX ── a hard guarantee: never render with the body in terrain. If any part of the long body overlaps solid, RESOLVE it — push the centre out to the nearest clear spot (works WITH the repulsion, not against it); only if there's no clear spot within reach does it fall back to the last clear pose. This is the "hitbox for terrain, not for the player" the user asked for.
        const bodyClear6 = (cx6, cz6) => fishBodyAt(cx6, cz6, B.th, Math.floor(B.y), B.fhalf);   // same single body test as the planner and stepOK
        if (bodyClear6(B.x, B.z)) { if (B.jumpV === undefined) { B.cx = B.x; B.cy = B.y; B.cz = B.z; B.cth = B.th; } }   // clear → remember this pose AND ITS HEADING (never an AIRBORNE one: a later revert would teleport the fish back into the sky)
        else {                                                             // clipping → move the centre to the NEAREST genuinely clear spot
          let fixed = false;                                               // Expanding-ring search on the real body test. The old version derived a push NORMAL from 8 probes at r=4 and one
          for (let s6 = 1; s6 <= 10 && !fixed; s6++) {                     // height, so anything those probes missed (a nose buried 5+ vox out, rock above/below that height) produced no
            for (let k6 = 0; k6 < 16; k6++) { const a6 = k6 * 0.3927;      // push at all and it fell straight through to the revert.
              const qx = B.x + Math.sin(a6) * s6, qz = B.z + Math.cos(a6) * s6;
              if (fishWaterOK(qx, qz, Math.floor(B.y)) && bodyClear6(qx, qz)) {   // the SAME water answer stepOK uses — a ring that offered a spot the mover would refuse just re-clipped the fish on the next frame
                B.x = qx; B.z = qz; B.cx = qx; B.cy = B.y; B.cz = qz; B.cth = B.th; fixed = true; break; } } }
          if (!fixed) for (let u6 = 1; u6 <= 6 && !fixed; u6++) {          // pinned horizontally → RISE out of it (a fish wedged beside a boulder has open water straight up)
            const qy = Math.min(WL - 1.4, B.y + u6);
            if (fishBodyAt(B.x, B.z, B.th, Math.floor(qy), B.fhalf)) { B.y = qy; B.cx = B.x; B.cy = qy; B.cz = B.z; B.cth = B.th; fixed = true; } }
          if (!fixed && B.cx !== undefined) { B.x = B.cx; B.y = B.cy; B.z = B.cz; if (B.cth !== undefined) B.th = B.cth;   // last-resort revert — RESTORE THE HEADING TOO: the body test depends on it, so a pose remembered at
            B.trap = (B.trap || 0) + dt;                                   // one heading can clip at another, and reverting position alone left a fish embedded for ~10 s at a time (measured).
            B.noMove = (B.noMove || 0) + dt;           // …and this counts as NOT MOVING. stepOK validates the body at the PRE-step y, then the depth ease shifts y and this
          }                                            // resolve rejects at the new y and restores the same pose — the move "succeeded" every frame while the fish sat
        }                                              // frozen (measured: 60 s at spd 8 with noMove stuck at 0). Both stall routes must feed the same counter.
      }
      // ── STALL WATCHDOG ── purely OBSERVATIONAL, and it is what finally kills the "stuck on rocks" report. Counting
      // rejected moves was not enough: the post-move hitbox often "fixes" a clipping fish by nudging it, the next frame
      // re-clips and nudges it back, and it oscillates on the spot with every internal counter reading healthy. So judge
      // the only thing that matters — did it actually TRAVEL — and require the terrain backstop to be engaged so a fish
      // merely circling in open water is never touched.
      let stallHit = false;
      if (B.kind === 6 && B.jumpV === undefined) {
        if (B.stallT === undefined) { B.stallT = tb3; B.stallX = B.x; B.stallZ = B.z; }
        else if (tb3 - B.stallT > 1.8) {
          if (Math.hypot(B.x - B.stallX, B.z - B.stallZ) < 7 && B.bkOn) stallHit = true;   // < 3.9 vox/s of real progress while grinding terrain
          B.stallT = tb3; B.stallX = B.x; B.stallZ = B.z;
        }
      }
      if (B.kind === 6 && ((B.noMove || 0) > 3 || stallHit) && B.jumpV === undefined) {
        // Sweep outward for somewhere the body genuinely fits and relocate; if the whole neighbourhood
        // is unusable, drop the slot so the spawner re-places it in open water. Either way no fish parks in the scenery.
        let done = false;
        for (let rr = 6; rr <= 40 && !done; rr += 4) {
          for (let k8 = 0; k8 < 12; k8++) { const a8 = k8 * 0.5236 + B.th;
            const qx = B.x + Math.sin(a8) * rr, qz = B.z + Math.cos(a8) * rr;
            if (!bfWater(qx, qz) || WL - bfBed(qx, qz) < 5) continue;
            const qy = Math.max(bfBed(qx, qz) + 2.2, Math.min(WL - 2, (bfBed(qx, qz) + WL) * 0.5));   // re-seat at MID depth — the stalls all began in shallow shelf water
            if (!fishBodyAt(qx, qz, a8, Math.floor(qy), B.fhalf)) continue;
            B.x = qx; B.z = qz; B.y = qy; B.th = a8; B.om = 0; B.omT = 0; B.dT = 0.5;
            B.cx = qx; B.cy = qy; B.cz = qz; B.cth = a8; B.hx = qx; B.hz = qz;   // re-home too, or the leash drags it straight back into the pocket
            B.noMove = 0; B.trap = 0; B.bkOn = false; B.senseRe = 0;
            B.stallT = tb3; B.stallX = qx; B.stallZ = qz; done = true; break; }
        }
        if (!done) { B.init = false; B.noMove = 0; B.stallT = undefined; }   // nowhere within 4 m works — recycle rather than leave a fish parked in the scenery
      }
      if (B.kind < 2 && (bfObst(B.x, B.y, B.z) || bfRoofed(B.x, B.y, B.z))) {   // under a roof (cave/overhang) — steer OUT horizontally (eased, never climb into it); count toward a recycle. Worms/ducks don't care about roofs.
        if (!NAVARB) B.omT = (B.om >= 0 ? 5.0 : -5.0);   // …but NOT while the arbiter is live: an unconditional omT write here is exactly the "fifth uncoordinated controller" the arbiter exists to remove, and roofedness is already one of its scored terms (navRoofAir, asked of the same predicate)
        B.trap = (B.trap || 0) + dt;
      }
      // ── …BUT NOT THE FROG (user 2026-08-27: "the frog seems to be flickering?") ── this hide exists for a
      // creature that has been SHOVED into geometry, and it drops it for exactly as long as that is true, which
      // for anything moving is a frame or two: a one-frame disappearance, i.e. a flicker. The frog is the one
      // body here whose position is prescribed rather than integrated — B.y rides its ANCHOR's ground (fgG) while
      // a lean carries x/z forward, so over rising ground the body POINT dips into the hillside while the model
      // itself is still sitting on the surface in plain view. MEASURED on a bank: ~1% of frames for one of three
      // frogs, and in an earlier test it took the frog off screen entirely mid-tongue. Its anchor and its landing
      // spot are both validated before it ever moves, so there is nothing here for this to rescue — and being
      // drawn a voxel into a slope is a far smaller artefact than winking out of existence.
      if (B.kind < 3 && !desFrog && bfObst(B.x, B.y, B.z)) continue;   // body-in-solid hide (skip emission) — LAND/AIR creatures only: a duck/lily bobbing into the water voxel layer is normal
      const bobA = (iceLock || B.lbPh === 'land') ? 0 : (B.kind === 2 || B.kind === 6 ? 0 : (B.kind === 4 ? 0.12 : (B.kind === 3 ? (isBaby ? 0.18 : 0.25) : (B.kind === 1 ? 1.1 : 2.2))));   // …and a LANDED LADYBUG is dead still for the same reason a frozen duck is: the flight bob is a RENDER term (below), not a change to B.y, so pinning the body in the tick left it visibly hovering on the spot (user 2026-08-22: "when the lady bug lands, it hovers. it should stay still")   // frozen duck → dead still (no bob); else worms/fish don't bob, water floats keep the cosmetic bob SMALL — the wave riding is the real motion, a deep bob dip re-sank the bottom rows
      const bobF = B.kind === 4 ? 1.1 : (B.kind === 3 ? (isBaby ? 2.3 : 1.6) : (B.kind === 1 ? 2.6 : 6.8));
      const hop3 = hurtHop(B);   // BOUNCE (user): a short arc up and back down over the flash window
      const px3 = B.x + (armSlot ? (B.aRoX || 0) : (desFrog ? Math.sin(B.th) * (B.fgPar || 0) : 0)), pz3 = B.z + (armSlot ? (B.aRoZ || 0) : (desFrog ? Math.cos(B.th) * (B.fgPar || 0) : 0)), py3 = hop3 + B.y + (bunnySlot ? (B.bOy || 0) : (armSlot ? (B.aRoY || 0) : Math.sin(tb3 * bobF + wk * 1.9) * bobA));   // ARMADILLO: shift by its per-heading/per-frame alignment offset so it centres like the editor (user). BUNNY: the lift is the BAKED oy. Worm bob stays 0.
      if (B.kind >= 2 && B.kind !== 6 && freezeK < 0.4) {   // GROUND/WATER creatures cast a sun shadow (lily/duck/worm) — flyers (0/1) skip, FISH skip (submerged: the surface owns the light there); frozen lakes are flat & shadow-free like the water
        const hxz = B.kind === 4 ? 5.0 : (B.kind === 3 ? (isBaby ? 1.8 : 3.0) : 3.0);
        const hy = B.kind === 4 ? 1.3 : (B.kind === 3 ? (isBaby ? 1.5 : 2.5) : 1.2);   // lily box a touch taller so the pad's shadow projects clear of the pad (not tucked underneath)
        cshadList.push([px3 - winOX, py3, pz3 - winOZ, hxz, hy, dp2]);
      }
      // ── A MODEL AUTHORED THE OTHER WAY ROUND (user 2026-08-22: "the lady bug is flying backwards") ── the
      // basis below is built on the convention that model −y is the head end, and every creature baked by
      // tools/bake_desert_life.py honours it. The ladybug does not: it came in through edStripItems as a
      // scene-graph .vox authored for the asset editor, where its head is the narrow all-dark end at +y —
      // established there by inspecting the model rather than by eye, after two wrong guesses off screenshots
      // (x is the WINGSPAN; the dark band down the middle is the elytra split). So it is a flat 180°, and it
      // is applied to the RENDER yaw only: the creature's own steering, spacing and flee logic all keep
      // working in B.th, exactly as the editor exhibit's `flip` does in main/tick-emit.js.
      const thR9 = (desSlot && DES_BACKWARDS[(DESERTS[desSp] || {}).name]) ? B.th + Math.PI : B.th;   // desSlot FIRST: desSp is 0 for every non-desert body, so without it this asks about DESERTS[0] on behalf of the whole world
      const HxR = Math.sin(thR9), HzR = Math.cos(thR9);   // RENDER yaw — for lilies this spins freely, independent of drift
      const mamMir = (bunnySlot || armSlot || skunkSlot || porcSlot || flamSlot) ? -1 : 1;   // HANDEDNESS: the grid stamp maps model (x,y,z) to world (+x,+z,+y) - determinant -1, a REFLECTION - while the emit basis below is right-handed, so the two paths draw one .vox as mirror images. The four land mammals are grid-stamped, so the emit branch below never runs for them and has never been corrected; -1 is that correction, scoped to them so nothing already on the trace path moves.
      let Xw = [HzR * mamMir, 0, -HxR * mamMir], Yw = [-HxR, 0, -HzR], Zw = null;   // level frame — Zw = up, right-handed (Yw×Zw = Xw); model −y (the head end) leads
      if (B.kind === 6 && B.jumpV === undefined) {     // ── FISH ORIENTATION ── upright always: free yaw, a SUBTLE nose pitch from the low-passed climb/dive, and ROLL
        // IMPOSSIBLE BY CONSTRUCTION — the lateral axis Xw stays exactly horizontal and Zw completes the right-handed frame
        // from it, so no path through this code can bank or flip the body. AIRBORNE leaps stay dead level (user).
        const p9 = Math.max(-FISH_CFG.pitchMax, Math.min(FISH_CFG.pitchMax, Math.atan2((B.vyS || 0) * FISH_CFG.pitchGain, Math.max(6, B.spd || FISH_CFG.baseSpeed))));
        if (p9 * p9 > 1e-6) { const cp9 = Math.cos(p9), sp9 = Math.sin(p9);
          Yw = [-HxR * cp9, -sp9, -HzR * cp9]; Zw = [-HxR * sp9, cp9, -HzR * sp9]; }   // head dir = −Yw tips up when climbing; Zw = Xw×Yw stays roll-free
      }
      // ── CRYING (user 2026-08-05) ── an orphaned duckling weeps for 3 s, one tear at a time, alternating
      // eyes. Done HERE because this is where the model's world frame exists: an eye is a model-space cell,
      // and (px3,py3,pz3) + Xw/Yw/Zw + bScale is exactly the basis the emit hands the tracer, so a tear
      // leaves the eye the renderer actually draws — at any heading, and riding the same wave bob.
      if (isBaby && B.cryTo && DUCKB_EYES.length) {
        if (now > B.cryTo) B.cryTo = 0;
        else if (now >= B.cryNext) {
          B.cryNext = now + CRY_GAP;
          B.cryEye++;                                  // a free-running counter now, NOT wrapped to the eye list: baby.vox
          const e = DUCKB_EYES[B.cryEye % DUCKB_EYES.length];   // is one voxel wide at the head, so it has ONE black voxel and `% length` pinned this to 0 forever
          // …and the droplet wells out of the SIDE of that voxel, alternating cheeks — half a cell along the
          // model's own X puts it exactly on the eye's outer face, so it is visible the instant it is born
          // instead of spending its first frames buried inside the head.
          const ex = e[0] + ((B.cryEye & 1) ? 0.5 : -0.5), zW = Zw || [0, 1, 0];
          spawnTear(px3 + (Xw[0] * ex + Yw[0] * e[1] + zW[0] * e[2]) * bScale,
                    py3 + (Xw[1] * ex + Yw[1] * e[1] + zW[1] * e[2]) * bScale,
                    pz3 + (Xw[2] * ex + Yw[2] * e[1] + zW[2] * e[2]) * bScale);
        }
      }
      // ── THE POLLEN TRAIL (user 2026-08-18) ── the duckling's tear block above, re-aimed at the bee's tail.
      // It lives HERE for the same reason that one does: this is the only place the model's world frame
      // (Xw/Yw/Zw, px3/py3/pz3, bScale) exists, so it is the only place a body-relative point can be turned
      // into a world position.
      // THE TAIL NEEDS NO TABLE. The bee is 3x3x3 with its abdomen at cell (1,2,1), which is [0, +1, 0] from
      // the box centre in the same convention DUCKB_EYES uses — and Yw is MINUS the heading, so +y is already
      // behind the bee at every heading. 1.6 rather than 1.0 pushes the grain just clear of the body so it is
      // visible the instant it is born instead of spending its first frames inside the abdomen, which is the
      // half-cell trick the tears use on the eye.
      // ── ONLY FROM A BEE YOU CAN ACTUALLY SEE (user 2026-08-18: "small artifacts that are appearing and
      // disappearing everywhere") ── that was this effect, and the fault was mine: 8 bees emitting every 110 ms
      // into a 4-slot band means bandSlot refuses most grains, so what landed was a scatter of unrelated white
      // specks strung across the whole draw distance with no visible bee attached to any of them. A particle
      // whose SOURCE is off-screen or too small to resolve does not read as pollen, it reads as a glitch.
      // 64 voxels is about where the bee itself is still a recognisable shape rather than two pixels, so inside
      // it the trail has something to belong to. It also cuts the band's load by roughly the ratio of that disc
      // to the spawn disc, which is what stops the grains being dropped at random in the first place.
      if (desBee && B.polTo && spawnPollen && dp2 < 64 * 64) {
        if (now > B.polTo) B.polTo = 0;
        else if (now >= (B.polNext || 0)) {
          B.polNext = now + POL_GAP;
          const zW = Zw || [0, 1, 0], e = [0, 1.6, 0];
          spawnPollen(px3 + (Xw[0] * e[0] + Yw[0] * e[1] + zW[0] * e[2]) * bScale,
                      py3 + (Xw[1] * e[0] + Yw[1] * e[1] + zW[1] * e[2]) * bScale,
                      pz3 + (Xw[2] * e[0] + Yw[2] * e[1] + zW[2] * e[2]) * bScale);
        }
      }
      // ── A DASHING ANIMAL ANIMATES AS FAST AS IT MOVES (user 2026-08-16) ── the same DES_DASH factor and the
      // same DES_DASH_R that double the travel speed now double the frame rate, so legs keep pace with ground
      // covered instead of a sprinting gecko sliding on a walk cycle. It multiplies each species' OWN rate
      // rather than setting a flat 48: everything runs at 24 and doubles to 48, but the scorpion runs at 12
      // and doubles to 24, which keeps the slower gait the user asked for.
      const desRate = (desSlot && DESERTS[desSp])
        ? (DES_FPS[DESERTS[desSp].name] || 24) * ((DES_DASH[DESERTS[desSp].name] && ((P.x - B.x) * (P.x - B.x) + (P.z - B.z) * (P.z - B.z)) < DES_DASH_R * DES_DASH_R) ? DES_DASH[DESERTS[desSp].name] : 1)
        : 24;
      const nfr = desSlot ? (DESERTS[desSp] ? DESERTS[desSp].n : 1) : B.kind === 6 ? FISHES[B.fsp || 0].n : (B.kind >= 3 ? 1 : (B.kind === 2 ? (bunnySlot ? (B.bst ? BUNNY_NFRAMES : BUNNY_JUMP_NFRAMES) : (armSlot ? ARMADILLO_NFRAMES : (skunkSlot ? SKUNK_NFRAMES : (porcSlot ? PORCUPINE_NFRAMES : (flamSlot ? FLAMINGO_NFRAMES : WORM_NFRAMES))))) : (B.kind === 1 ? FFLY_NFRAMES : (B.dfly ? DFLY_NFRAMES : BFLY_NFRAMES))));
      // ── THE SKUNK AND PORCUPINE RUN OFF THE SAME CLOCK IN BOTH RENDER PATHS (user 2026-08-07: "cut the
      // skunk's animation speed in half") ── it already had been, once, and it never showed: the GRID-STAMPED
      // path reads B.aframe, which carries the eased 12↔24 fps rate and the skunk's own ×0.5 on top, while
      // this emit read animClk at a flat 24. So the authored rate only ever applied beyond the trace radius,
      // and the animal the player was actually looking at ran at 24 fps — and jumped 4× the moment it crossed
      // the boundary. Same clock now, so the rate is what the marcher set, near and far alike.
      // ── THE FLAMINGO IS ON THE WALKERS' CLOCK TOO (user 2026-08-18: "the flamingo turns into a skunk briefly") ──
      // and that symptom was this line and `nfr` above, together. With no flamingo case, nfr fell through to
      // WORM_NFRAMES — MORE frames than the flamingo's ten — so FLAMINGO_ITEM0 + fi3 walked off the end of its
      // own strip and into whatever was loaded next in the item table. The flamingo is parsed immediately before
      // the skunk, so "next" was the skunk, frame for frame. An item-table overrun reads as one creature briefly
      // becoming another, and it is only ever a frame-count mismatch.
      // ── THE FROG'S OWN FRAME, AND IT HAS TO COME BEFORE THE KIND-2 ARM (user 2026-08-27: "the animations are
      // fine, its just the movement while the animations are playing are completely wrong … when the hop
      // animation plays, the frog stays in the same position") ── the arm below it said exactly this and could
      // never run: a frog is kind 2, so `(B.kind === 2 || B.kind === 6)` matched first and B.fgF never reached
      // the renderer. What was drawn was animClk, free-running the WHOLE 55-frame strip — hop, ribbet and
      // tongue back to back on a loop at 24 fps — while the sim moved the body on whatever cycle IT had picked.
      // The two were decoupled, which is the whole bug: MEASURED, animClk was showing hop frames 5, 9, 13 while
      // fgP was -1 (parked), so the frog visibly leapt and did not move an inch, and it flicked its tongue
      // mid-ribbet. Ordering is the fix; the arm's own reasoning was right all along.
      // It also has to sit ahead of the arm for a second reason: frogSeat above seats the frame the sim thinks
      // it is on, so with the two decoupled the seat was being computed for a frame that was never drawn.
      const fi3 = (B.kind === 2 && (skunkSlot || porcSlot || flamSlot)) ? (Math.floor(B.aframe || 0) % nfr)
        : (desFrog && B.fgF !== undefined) ? B.fgF    // the FROG's frame is driven by its leap (above), not by the shared clock — the arc and the animation are the same seventeen frames
        : (B.kind === 2 || B.kind === 6) ? (Math.floor((B.animClk || 0) * desRate) % nfr)   // …and the desert rate applies HERE, which is the branch a kind-2 creature actually takes — putting it only on the line below meant the scorpion silently stayed at 24   // WORM/FISH: the frame runs off the creature's OWN clock — the worm's freezes with its pauses, the fish's scales with its swim speed
        : B.lbPh === 'land' ? 0                          // LANDED LADYBUG: frame 00 is the wings-SHUT pose, and holding it is what makes a landing read as a landing rather than a hover at ground level (the editor exhibit does the same in main/tick-emit.js)
        : Math.floor((tb3 + wk * 0.37) * desRate) % nfr;   // per-species rate for the desert set; everything else keeps the 24 fps house rule                  // 24 fps cycle, desynced per creature (duck/lily are single static models)
      let glow = 0;
      if (B.kind === 1) {                              // GLOW (fireflies only): random dark spell, then the yellow abdomen holds BRIGHT for a full 2 s
        if (!B.glowT || now > B.glowT) { B.glow = !B.glow; B.glowT = now + (B.glow ? 2000 : 1500 + Math.random() * 3500); }
        glow = B.glow ? 2.8 : 0;
        if (glow > 0) ffLights.push([B.x - winOX, py3, B.z - winOZ, glow * fadeIn * fadeOut, dp2]);   // this one casts LIGHT — window coords for the tracer
      }
      if (B.kind === 3) {                              // DUCK EYE BLINK (user): the black eye voxel flashes green ~every 2.5-5 s for a beat; glow lane carries 0/1 to the shader
        glow = B.blink ? 1 : 0;                       // …the duck's is a GREEN flash in the shader rather than a baked lid frame, so it reads the same clock a different way
      }
      const rx = px3 - cam[0], ry = py3 - cam[1], rz = pz3 - cam[2];
      if (bunnySlot && dp2 < 30 * 30 && bunnyBoxN < bunnyBoxes.length) { const bx9 = bunnyBoxes[bunnyBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // SOLID (user): publish a hitbox for each near bunny so the player can't run through it
      if (LIFE_UNI && B.sN && uniTraced(B)) unstampWorm(B);   // ── CROSSING THE TRACE RADIUS ── a mammal that was grid-stamped and is now traced MUST drop its stamp, or the old voxels stay welded into the world and read as an animal frozen in the terrain (user 2026-08-06). The songbird path already did this; the mammal branches below fell straight through to the emit without it.
      if (bunnySlot && (!LIFE_UNI || !uniTraced(B))) { stampBunny(B, Math.round(mamSeatG(B, MAMFIT.bunny) + hurtHop(B))); continue; }   // …seated by the SAME footprint scan the traced path uses (user 2026-08-07)   // …including the bounce (user)   // BUNNY is GRID-STAMPED into W (editor-identical, user) — stamp instead of trace-emit, and SKIP the emit below (hitbox already published above)
      if (armSlot && dp2 < 30 * 30 && armBoxN < armBoxes.length) { const bx9 = armBoxes[armBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and each near armadillo
      if (armSlot && (!LIFE_UNI || !uniTraced(B))) { stampArmadillo(B, Math.round(mamSeatG(B, MAMFIT.arm) + hurtHop(B))); continue; }   // …same seat   // …including the bounce (user)   // ARMADILLO is GRID-STAMPED into W again (editor-identical alignment, user) — stamp instead of trace-emit, skip the emit below
      if (skunkSlot && dp2 < 30 * 30 && skunkBoxN < skunkBoxes.length) { const bx9 = skunkBoxes[skunkBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and each near skunk
      if (skunkSlot && (!LIFE_UNI || !uniTraced(B))) { stampSkunk(B, Math.round(mamSeatG(B, MAMFIT.skunk) + hurtHop(B)), now); continue; }   // …same seat   // …including the bounce (user)   // SKUNK is GRID-STAMPED into W (editor-identical alignment + blink, user) — stamp instead of trace-emit, skip the emit below
      if (porcSlot && dp2 < 30 * 30 && porcBoxN < porcBoxes.length) { const bx9 = porcBoxes[porcBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and each near porcupine
      if (flamSlot && dp2 < 30 * 30 && flamBoxN < flamBoxes.length) { const bx9 = flamBoxes[flamBoxN++]; bx9.active = true; bx9.cx = B.x; bx9.cy = B.y; bx9.cz = B.z; }   // …and the flamingo publishes one too, or sim/player.js has an array with nothing ever in it
      if (porcSlot && (!LIFE_UNI || !uniTraced(B))) {   // ── AND THE PORCUPINE'S NOSE HACK IS GONE (user 2026-08-07) ──
        // it used to take the stamp height at the CENTRE and then add half a voxel while ASCENDING, because the
        // model's nose reaches ~3 voxels ahead and buried itself climbing. That was a one-animal patch for the
        // general fault: a seat sampled at one column under a body that is not a column. mamSeatG scans the whole
        // footprint — the nose included — so the lift it was approximating now falls out of the measurement, in
        // every direction rather than only uphill, and for all four animals rather than this one.
        stampPorcupine(B, Math.round(mamSeatG(B, MAMFIT.porc) + hurtHop(B))); continue; }   // …including the bounce (user)   // PORCUPINE is GRID-STAMPED into W (armadillo-style, user's 4th land mammal) — stamp instead of trace-emit, skip the emit below
      if (emitN >= EMIT_CAP) continue;                 // stage the pose (never emit directly) — the nearest are chosen after the loop
      const o4 = emitN * 16;
      emitBuf[o4] = rx * right[0] + ry * right[1] + rz * right[2]; emitBuf[o4 + 1] = rx * up[0] + ry * up[1] + rz * up[2]; emitBuf[o4 + 2] = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]; emitBuf[o4 + 3] = bScale;
      const _it0 = (desSlot ? (DESERTS[desSp] ? DESERTS[desSp].item0 : 0) : B.kind === 6 ? FISHES[B.fsp || 0].item0 : (B.kind === 4 ? LILY_ITEM0 + (B.col % Math.max(1, LILY_SZ.length)) : (B.kind === 3 ? (isBaby ? DUCKB_ITEM0 : DUCK_ITEM0) : (B.kind === 2 ? (bunnySlot ? (B.bst ? BUNNY_ITEM0 : BUNNY_JUMP_ITEM0) : (armSlot ? ARMADILLO_ITEM0 : (skunkSlot ? SKUNK_ITEM0 : (porcSlot ? PORCUPINE_ITEM0 : (flamSlot ? FLAMINGO_ITEM0 : WORM_ITEM0))))) : (B.kind === 1 ? FFLY_ITEM0 : (B.dfly ? DFLY_ITEM0 : BFLY_COLS[B.col]))))));
      // ── AND THE BLINK IS ONE OFFSET ── the variants sit immediately after a creature's own frames, so
      // +nfr selects the lid. Gated on BLINK_HAS because a strip WITHOUT variants would read straight past
      // its own end into the next creature's frames — a visible corruption, not a missing blink.
      emitBuf[o4 + 7] = _it0 + fi3 + ((B.blink && BLINK_HAS.has(_it0)) ? nfr : 0);
      emitBuf[o4 + 11] = glow;
      // ── THE POSE THE RENDERER USED ── cached so the RAGDOLL can rebuild this creature's voxels in world
      // space at the instant it dies. Nothing extra is allocated: Xw/Yw/Zw are this frame's own arrays, and
      // the anchor is three numbers written over three numbers.
      B.ragIt = emitBuf[o4 + 7]; B.ragA0 = px3; B.ragA1 = py3; B.ragA2 = pz3;
      B.ragX = Xw; B.ragY = Yw; B.ragZ = Zw || RAG_UP; B.ragS = bScale;
      if (Zw) {                                        // FULL 3D frame (fish): every axis has a y component — the level-frame fast path below assumes Zw = world-up
        emitBuf[o4 + 4] = Xw[0] * right[0] + Xw[1] * right[1] + Xw[2] * right[2]; emitBuf[o4 + 5] = Xw[0] * up[0] + Xw[1] * up[1] + Xw[2] * up[2]; emitBuf[o4 + 6] = Xw[0] * fwd[0] + Xw[1] * fwd[1] + Xw[2] * fwd[2];
        emitBuf[o4 + 8] = Yw[0] * right[0] + Yw[1] * right[1] + Yw[2] * right[2]; emitBuf[o4 + 9] = Yw[0] * up[0] + Yw[1] * up[1] + Yw[2] * up[2]; emitBuf[o4 + 10] = Yw[0] * fwd[0] + Yw[1] * fwd[1] + Yw[2] * fwd[2];
        emitBuf[o4 + 12] = Zw[0] * right[0] + Zw[1] * right[1] + Zw[2] * right[2]; emitBuf[o4 + 13] = Zw[0] * up[0] + Zw[1] * up[1] + Zw[2] * up[2]; emitBuf[o4 + 14] = Zw[0] * fwd[0] + Zw[1] * fwd[1] + Zw[2] * fwd[2]; emitBuf[o4 + 15] = 0;
      } else {                                         // LEVEL frame (everything else) — Xw[1] = Yw[1] = 0 and Zw = [0,1,0], so the dots collapse
        emitBuf[o4 + 4] = Xw[0] * right[0] + Xw[2] * right[2]; emitBuf[o4 + 5] = Xw[0] * up[0] + Xw[2] * up[2]; emitBuf[o4 + 6] = Xw[0] * fwd[0] + Xw[2] * fwd[2];
        emitBuf[o4 + 8] = Yw[0] * right[0] + Yw[2] * right[2]; emitBuf[o4 + 9] = Yw[0] * up[0] + Yw[2] * up[2]; emitBuf[o4 + 10] = Yw[0] * fwd[0] + Yw[2] * fwd[2];
        emitBuf[o4 + 12] = right[1]; emitBuf[o4 + 13] = up[1]; emitBuf[o4 + 14] = fwd[1]; emitBuf[o4 + 15] = 0;
      }
      emitWho[emitN] = wk; emitAna[emitN] = B.kind === 1 ? 1 : 0;   // FIREFLIES stay analytic (emissive abdomen + translucent wings need the composite path)
      // which KIND is contending for a slot — the fair-share allocator's only input besides distance. Ducklings
      // are counted apart from their mothers on purpose: a brood is a dozen bodies against her one, so pooled
      // they would spend the whole duck allowance on babies and the mother would never be the one drawn.
      emitKnd[emitN] = desBee ? LIFE_K_BEE : (fishSlot ? (LIFE_K_FISH + Math.min(LIFE_FISH_MAX - 1, B.fsp | 0)) : (wormSlot ? LIFE_K_WORM : (isBaby ? LIFE_K_BABY : (duckSlot ? LIFE_K_DUCK
        : (wk < FLY_END ? (B.dfly ? LIFE_K_DFLY : LIFE_K_FLYER) : LIFE_K_OTHER)))));   // wk < FLY_END is the flyer band: butterflies by day, fireflies by night, dragonflies at the top of it
      emitMust[emitN] = (bunnySlot || armSlot || skunkSlot || porcSlot) ? 1 : 0;   // a mammal only reaches the emit by SKIPPING its grid stamp (the branches above `continue` when they stamp), so reaching here means slot-or-nothing — see emitMust in sim/life/slots.js
      emitAnc[emitN * 3] = px3; emitAnc[emitN * 3 + 1] = py3; emitAnc[emitN * 3 + 2] = pz3;
      { const cx8 = emitBuf[o4], cy8 = emitBuf[o4 + 1], cz8 = emitBuf[o4 + 2];   // the camera-space anchor written above
        const r8 = LIFE_FRUST_R + (lifeIsDrawn(wk) ? LIFE_FRUST_HYST : 0);
        emitVis[emitN] = (cz8 + r8 > 0 && (Math.abs(cx8) - fsX * cz8) * fnX <= r8 && (Math.abs(cy8) - fsY * cz8) * fnY <= r8) ? 1 : 0; }
      emitDp[emitN] = dp2; emitN++;
    }
    if (CPROF) cpMark(5);
