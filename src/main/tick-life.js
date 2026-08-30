    // ── PROCEDURAL POPULATION ── the active butterfly count scales with the visible AREA (density ≈ 1 per (200 vox)²),
    // so the world stays populated at every render distance. Each is recycled to a fresh procedural cell when it leaves
    // the fog ring; color comes from a spatial hash of that cell so orange/red/blue are sprinkled across the world.
    const rdV = Math.max(140, renderDist);
    // ── ONE SPAWN RULE FOR ALL LIFE ── (user) every creature now follows the cardinal/worm mechanic. Two things fix the
    // "it appeared out of nowhere beside me" pop-in: (a) LIFE_IN — nothing is EVER placed closer than this, so life fades
    // in through the far fog and you walk up on it, instead of being re-placed 45 vox away in plain view; (b) hysteresis —
    // the spawn ring (LIFE_OUT) sits strictly inside the despawn ring (LIFE_KEEP), so a creature dropped at the rim can't
    // immediately recycle and thrash. LIFE_KEEP stays CAPPED because stationary life (cardinals) and slow life (worms)
    // clump behind a fast player when the ring never recycles.
    // DOUBLED 2026-07-18 (user). The hard ceiling is the GENERATED RECT, not the cap: rect reaches renderDist+96 and a
    // spawn outside it is rejected outright, so LIFE_KEEP can only reach a little past the view. At the default view
    // (368) that lands the inner ring at ~337 instead of ~202 — the most the built world allows. Raising the view
    // slider is what actually unlocks the full doubling: at a 1000 view the inner ring goes 286 -> 811.
    const LIFE_CAP = CARD_KEEP;                        // was 520 — doubled, so a long view distance is no longer clamped by the cap. NOT a literal any more: it and CARD_KEEP were two names for the world's edge and drifting them apart is what un-unifies the reach again (user 2026-08-17)
    const LIFE_KEEP = Math.min(rdV + 64, LIFE_CAP);    // DESPAWN radius (every kind) — slightly PAST the view, still safely inside the generated rect
    // ── AND IT IS SCALED BY HOW MUCH OF THE SPAWN RING IS ACTUALLY DESERT (user 2026-08-18: "life clustering up
    // where the border meets different biomes") ── this was the ONE population still asking for a flat count.
    // The mammals scale on bioFracAt, the flamingo on bioFracCherryAt and the porcupine on bioFracPineAt; the
    // desert species asked for six per species wherever the player stood. Stand ON the desert border and only a
    // sliver of the ring is legal sand, but the target is unchanged — so the placer retries until it has fitted
    // the WHOLE population into that sliver, and the result is the pile-up along the boundary in the user's
    // screenshot rather than a desert that simply thins out as it ends.
    // Same shape as the other three: 24 samples round a ring, count how many are DEEP desert, scale the target
    // by that fraction. So the population tracks the LEGAL AREA and the density stays what it is in open sand.
    // >= 0.85 is bioFracCherryAt's threshold and means the same thing here — a column the species would really
    // be admitted in, not one on the blend ramp where it is only half sand.
    // Radius: the middle of this band's own spawn ring. LIFE_OUT is 0.94 of LIFE_KEEP and the inner edge sits
    // a little below, so 0.86 is the mid-band, and LIFE_KEEP is already in scope on the line above.
    // FLOOR 0.12, not bioFracAt's 0.35: the point of the scale is to hold DENSITY constant, and a high floor is
    // exactly the over-population being fixed. Low but non-zero, so the sand does not go abruptly lifeless when
    // you stand on its edge — it thins.
    // DEEP DESERT IS BIT-IDENTICAL: all 24 samples pass there, the fraction is exactly 1, nDesert is exactly 6,
    // and the Bresenham split below still yields 4,5,4,5,4,5,4 = 31 to the last body.
    const bioFracDesertAt = (r) => {
      let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        if (desertM(P.x + Math.cos(a) * r, P.z + Math.sin(a) * r) >= 0.85) ok++; }
      return Math.max(0.12, ok / 24);
    };
    // ── THE WORLD GOES TO SLEEP (user 2026-08-19: "when it turns night, make all of the land animals
    // dissapear/bugs, etc. just have birds in the trees. no birds flying") ── one flag, read by every ground
    // and flying population below, so night is a single decision rather than nine of them drifting apart.
    // moonMode is the renderer's OWN night (main/tick-camera.js hands the world to the moon at sun elevation
    // -0.06), which is already in scope here and already gates the firefly/butterfly swap, so "night" means
    // the same thing to the creatures as it does to the sky.
    // WHAT STAYS: the perched songbirds (nCard) — they are the "birds in the trees" — the FISH, which are not
    // land animals, and the FIREFLIES, which only exist at night, are the night's own light source and have a
    // switch of their own in the L panel. Everything that walks, crawls, swims on the surface or flies goes.
    const NIGHT_QUIET = !!moonMode;
    // ── …AND IT ARRIVES AS A DUSK, NOT AS A DELETION (user 2026-08-20: "all of the life on the ground are
    // glitching, missing and dissapearing") ── NIGHT_QUIET is a BOOLEAN, and nine populations below read it:
    // mammals, worms, the whole desert bug band, the oak-only bee and grass snake, flamingos, porcupines,
    // dragonflies and ducks. So on the single frame the sun crosses -0.06 every one of them went to zero at
    // once — measured, 36 mammals + 20 worms + 31 sand bugs deleted between one frame and the next, wherever
    // they happened to be standing, and popped back the same way at dawn. The night itself is wanted (user
    // 2026-08-19: "when it turns night, make all of the land animals dissapear"); a whole meadow blinking out
    // in front of you is not, and that is what reads as "glitching".
    // NIGHT_K is that same decision as a RAMP: 1 in daylight, easing to 0 by the time the moon has the sky, so
    // the ground thins out over the last ~30 s of light — a couple of animals at a time, the same way the
    // population already thins across a biome border with bioFracAt, which has never read as a glitch.
    // Derived from tday rather than from `el` in tick-camera.js so it cannot drift out of step across the
    // fragment boundary, and it is the identical expression the songbird roost gate uses in sim/life/birds.js.
    // The window ENDS at moonMode's own -0.06: the ground is already empty when the sky flips, so the two
    // never disagree, and NIGHT_QUIET is left standing for the places that genuinely want a hard switch.
    const NIGHT_SUNEL = Math.sin(Math.sin(tday * Math.PI * 2 - Math.PI / 2) * 1.05);
    const NIGHT_T = Math.max(0, Math.min(1, (NIGHT_SUNEL + 0.06) / 0.16));
    const NIGHT_K = NIGHT_T * NIGHT_T * (3 - 2 * NIGHT_T);   // smoothstep: no kink at either end of the dusk
    const nightK = (v) => (NIGHT_K >= 1 ? v : Math.round(v * NIGHT_K));   // a count, thinned by how much light is left
    // ── …BUT THE WORLD MUST NOT GO DEAD (user 2026-08-20: "all the life is dissapearing and not showing") ──
    // MEASURED over a full cycle with the clock running: 536 creatures in daylight, 441 through the night, and
    // of those 441 every single one is a perched songbird. Night is sun elevation below -0.06, which is 0.48 of
    // a 20-minute cycle — so HALF of every session was an empty world, and it comes back in full at dawn, which
    // is why it never looked like a spawn leak and why no render-side fix ever touched it.
    // The 2026-08-19 request ("make all of the land animals dissapear/bugs, etc") is kept for the DAY species —
    // the bees, the desert bugs, the ducks, the flying songbirds all still go. What stays is a NOCTURNAL cast at
    // a third strength: the four land mammals (the skunk, porcupine and armadillo are nocturnal animals in the
    // first place) and the worms. So the night still reads as a different, quieter world rather than as a bug.
    // One number to tune, and 0 restores the old behaviour exactly.
    const NIGHT_NOCT = 0.34;
    const NIGHT_KN = Math.max(NIGHT_K, NIGHT_NOCT);
    const nightKN = (v) => (NIGHT_KN >= 1 ? v : Math.max(1, Math.round(v * NIGHT_KN)));   // …never rounds a surviving population down to nothing
    const nDesert = 6 * bioFracDesertAt(LIFE_KEEP * 0.86) * NIGHT_K;   // the BUGS (ant, gecko, scorpion, spider, fly, bee, snakes, desert mouse) — gone after dark
    // ── 25% RARER (user 2026-08-16) ── 6 per species x 7 = 42 was the old population; 0.75 of that is 31.5,
    // which no integer per-species count can express (5 gives 35, i.e. only 17% fewer; 4 gives 28, 33% fewer).
    // So the count is distributed Bresenham-style instead: the running total is 4.5 per species and each
    // species takes whatever crossing that total produces, giving 4,5,4,5,4,5,4 = 31. Deterministic, balanced,
    // and no species is arbitrarily favoured. Change DES_RARITY alone to retune.
    // ── 25% RARER (user 2026-08-16) ── 6 per species x 7 = 42; three-quarters is 31.5, which no integer
    // per-species count can express (5 gives 35, only 17% fewer; 4 gives 28, 33% fewer). So the count is
    // distributed Bresenham-style: the running total is 4.5 per species and each takes whatever crossing it
    // produces, giving 4,5,4,5,4,5,4 = 31. Deterministic, balanced, no species arbitrarily favoured.
    // NOTE this was briefly 6/7 on a measurement of 27 alive against a target of 31 — that gap was NOT slot
    // churn, it was the ant band spawning ZERO because its baked frames had been deleted. With the ant fixed,
    // alive == target, so the honest value is 0.75. Retune against __vb.lifeAll(), and check every species is
    // actually populated before concluding the target is unreachable.
    const DES_RARITY = 0.75;
    // ── AND THE BAND NOW HOLDS TWO BIOMES, SO THE DESERT COUNT SKIPS THE SPECIES THAT ARE NOT IN IT ──
    // (user 2026-08-17: the bee and the grass snake) The Bresenham above distributes 4.5 bodies per species
    // down the SPECIES INDEX, so simply appending two more names to DESERTS would have re-run the whole
    // distribution over nine slots and moved the desert's own populations. It walks the SAND species only:
    // an oak-only species takes 0, and the running index r advances only for the ones that are really out
    // there — so ant/cobra/desert_mouse/fly/gecko/scorpion/spider keep 4,5,4,5,4,5,4 = 31 to the last body,
    // bit-identical to before, and stay bit-identical however many oak species are appended after them.
    // Built as a small ARRAY once per frame rather than a closure that re-walks the list: nDesertOf is called
    // for every slot in the band on every frame, and DESERTS is loaded async so it cannot be hoisted out.
    const desCnt = []; { let r = 0;
      for (let i = 0; i < DES_N; i++) { const dn = (DESERTS[i] || {}).name;
        if (dn && DES_OAKONLY[dn]) { desCnt.push(0); continue; }
        desCnt.push(Math.floor((r + 1) * nDesert * DES_RARITY) - Math.floor(r * nDesert * DES_RARITY)); r++; } }
    const nDesertOf = (sp) => desCnt[sp] | 0;                                                                 // per SPECIES, out of DES_PER slots each — scattered across the desert; ZERO for an oak-only species
    // ── …AND WHAT AN OAK-ONLY SPECIES GETS INSTEAD ── its own entry in DES_OAKONLY, which IS the head-count
    // (see the table in sim/life/slots.js for why each of the two numbers is what it is). Read through a named
    // function rather than indexed at the call site so the desert count and the oak count are the same shape of
    // thing to every reader: nDesertOf(sp) + nOakOf(sp) is the species' whole population, and `active` in
    // tick-creatures is exactly that sum.
    const nOakOf = (sp) => nightK(DES_OAKONLY[(DESERTS[sp] || {}).name] | 0);   // the OAK-only bugs (bee, grass snake) are counted here and NOT by nDesert, so they need the night gate of their own — zeroing nDesert alone left them out foraging in the dark
    const LIFE_OUT = LIFE_KEEP * 0.94;                 // outermost SPAWN radius — the gap to LIFE_KEEP is the hysteresis band
    const LIFE_IN = Math.min(LIFE_KEEP * 0.78, LIFE_OUT - 24);   // innermost SPAWN radius — out past the fog, never in clear view
    const MAM_KEEP = CARD_KEEP;                        // LAND MAMMALS reach EXACTLY as far as the perched songbirds (user): the birds' FIXED 680 — not max(LIFE_KEEP,…), which let a big view slider push mammals past the birds and dilute the 56-head pool over an oversized disc (measured: mammals at 1020 vox, in-view rings near-empty)
    const MAM_OUT = MAM_KEEP * 0.94;                   // …and their outer SPAWN ring matches, with the same 6% hysteresis gap. Rect-clipped like the birds, so the effective reach lands wherever the generated world ends.
    // ══ EVEN DENSITY, NOT EVEN COUNT (user 2026-08-18: "there also seems to be a cluster of life when entering a
    // new biome ... life should be evenly spread across all the biomes no exceptions") ══
    //
    // THE BUG, MEASURED. Every population target below is a pure function of the view distance and nothing else —
    // no biome term has ever entered a count. The SPAWN GATE, meanwhile, refuses whole arcs of the annulus: sand
    // for forest life, blossom for everything except worms and butterflies. So the target count is constant while
    // the legal AREA is not, and the creatures pile into whatever is left. Measured on a ring at r=800:
    //     deep cherry  47% of the ring legal   deep oak 71%
    //     AT THE EDGE  48% legal, and 41 mammals inside 900 against deep oak's 49 over half again the area
    // i.e. ~23% more mammals per legal voxel at the border than in open forest, gathered into a crescent on the
    // legal side. That crescent is the "cluster when entering a new biome", and it is not a placement bug — the
    // placement is doing exactly what it was told. The COUNT was the bug.
    //
    // THE FIX. Sample the ring, measure the legal fraction, and scale the counts by it. Density then holds
    // constant everywhere and the crescent disappears, at the cost of genuinely fewer animals near a border —
    // which is the correct reading: half the land there belongs to a biome that does not want them.
    // Sampled at 24 angles on the mid-ring, once per frame: 24 mask pairs against the hundreds the spawn retries
    // already pay, and it is the same desertM/cherryM pair the gate itself uses, so the two cannot disagree
    // about what "legal" means. Floored at 0.35 so a player standing deep in a refused biome still sees SOME
    // life at the rim rather than an empty world, and clamped at 1 so nothing is ever scaled UP.
    // …and it is measured AT THE RADIUS THE POPULATION ITSELF USES. The first cut sampled one ring at
    // (LIFE_IN + LIFE_OUT)/2 and scaled the MAMMALS by it, and the mammals spawn on their own ring out at
    // MAM_OUT (639) — so the scalar and the count it scaled were measured in different places and the fix
    // overshot: the border came out 25% SPARSER than open forest instead of equal (measured 38 per legal
    // area against deep oak's 51). A ring is the right probe; it has to be the right ring.
    // …and its mirror: how much of the ring IS blossom. Same 24 samples, same reason.
    // …and the same question for PINE, which the porcupine needs. It is BIO_PINEF (oakM <= 0.15) and its
    // home-finder samples no mask at all, so wherever the player stands in oak the whole band is refused on
    // every retry. That was survivable while the oak line sat 420 voxels east of spawn; OAKOFF moved to 1220
    // and the cherry band doubled, so the player is now in oakM == 1 terrain essentially permanently and the
    // porcupine churned every frame for nothing. Same shape as the flamingo bug, same fix.
    const bioFracPineAt = (r) => {
      let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        if (oakM(P.x + Math.cos(a) * r, P.z + Math.sin(a) * r) <= 0.15) ok++; }
      return ok / 24;
    };
    const bioFracCherryAt = (r) => {
      let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        const x9 = P.x + Math.cos(a) * r;
        if (chNear(x9) && cherryM(x9, P.z + Math.sin(a) * r) >= 0.85) ok++; }   // chNear first, for the reason fillColumn asks it first: this runs every frame and the mask is ~7 vnoise a sample
      return ok / 24;
    };
    // ── AND THE SAME QUESTION FOR THE LIFE THAT IS ALLOWED IN THE BLOSSOM ── bioFracAt below excludes the
    // cherry band as well as the sand, because it sizes the MAMMALS and they are refused in the blossom. The
    // worms and the day butterflies are not: their legal area is everything but the desert. They were sized by
    // no fraction at all, which is why the deep desert asked for 22 worms and 16 flyers and held ZERO of each —
    // two bands permanently empty, re-trying every frame, and a `want` that lies to every tap that reads it.
    // Same shape, same ring, one term dropped.
    const bioFracLifeAt = (r) => {
      let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        if (desertM(P.x + Math.cos(a) * r, P.z + Math.sin(a) * r) <= 0.15) ok++; }
      const hereOK = desertM(P.x, P.z) <= 0.15;
      return hereOK ? Math.max(0.75, Math.min(1, ok / 24)) : Math.min(1, ok / 24);   // …and the same conditional floor, for the same reason
    };
    const bioFracAt = (r) => {
      let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        const x = P.x + Math.cos(a) * r, z = P.z + Math.sin(a) * r;
        if (desertM(x, z) <= 0.15 && (!chNear(x) || cherryM(x, z) <= 0.15)) ok++; }   // …and here too: away from the band the blossom test is a subtract and a compare
      // ── THE FLOOR IS WHAT STOPS A NARROW STRIP READING AS EMPTY (user 2026-08-20: "life in the pine forest
      // is dissapearing … it looks better in the oak forest now") ── this fraction holds DENSITY constant by
      // thinning the population to match the legal area, which is right when the illegal part is far away and
      // wrong when the creature is standing in a 2160-wide strip. The spawn ring is ~894 and a strip's half
      // width is 1080, so most of the ring is over the NEIGHBOURS: after the blossom moved to the oak band's
      // edge the pine strip ended up between cherry and desert, and this test excludes BOTH — measured, the
      // pine's mammal target halved to 8 against the oak's 14 while every band still read alive == want, i.e.
      // nothing was failing, there was simply less asked for. That reads as the forest emptying out.
      // 0.75, not 0.35: a strip hemmed in on both sides keeps three quarters of its animals instead of a
      // third. Density in the legal part rises, which is the trade — and it is the right way round, because
      // a biome you can walk through and see nothing in is the thing being reported.
      // …but ONLY WHERE THE CREATURE COULD ACTUALLY STAND. A blanket floor asks for a population in habitat
      // that refuses it: measured at 0.75 the blossom wanted 10 bunnies and held 0, and the deep desert wanted
      // 10 of each mammal and held 0 — bands permanently empty, burning their spawn retries every frame, and
      // __vb.lifeWhy() correctly calling it "placement is failing". That is the very defect this fraction
      // exists to prevent, so the floor is conditional on the CENTRE being legal ground: hemmed in on both
      // sides but standing somewhere valid, keep three quarters; standing in the sand or the blossom, take
      // the honest fraction, which goes to zero and asks for nothing.
      const hereOK = desertM(P.x, P.z) <= 0.15 && (!chNear(P.x) || cherryM(P.x, P.z) <= 0.15);
      return hereOK ? Math.max(0.75, Math.min(1, ok / 24)) : Math.min(1, ok / 24);
    };
    const nActD = Math.max(3, Math.min(16, Math.round(Math.PI * rdV * rdV / (200 * 200) / 2)));   // HALF the original density (user); this is the DENSITY term only — the flyer count is clamped to FLY_N (the slot ladder in sim/life/slots.js), never to a literal here
    const nAct = Math.round((moonMode ? Math.max(2, nActD >> 1) : Math.min(FLY_N, Math.round(nActD * 3.125))) * bioFracLifeAt((MAM_KEEP * 0.78 + MAM_OUT) * 0.5));   // …and the flyers too, for the same reason   // BUTTERFLIES doubled (user 2026-07-18): 1.25 -> 2.5, then +25% (user 2026-08-22): 2.5 -> 3.125 AND the clamp raised from a literal 16 to FLY_N (20, sim/life/slots.js). BOTH halves are needed and each is inert alone: the multiplier decides the count only at short view distances, the clamp decides it at every shipped one. Together they are exactly 1.25x the old number at every render distance   // FIREFLIES half as frequent (night) and NOT clamped by FLY_N, which is why widening the band left the night exactly as it was
    const nWorm = WORM_NFRAMES ? nightKN(Math.round(Math.max(11, Math.min(22, Math.round(nActD * 1.4))) * bioFracLifeAt((MAM_KEEP * 0.78 + MAM_OUT) * 0.5))) : 0;   // …scaled by the LEGAL area (see bioFracLifeAt): unscaled it asked the desert for 22 and got 0   // NOCTURNAL: worms stay out after dark, thinned (see NIGHT_NOCT)   // NOT scaled by bioFrac: a worm is admitted in the blossom as well as both forests, so its legal area is the whole world bar the sand — scaling it would thin the one population the cherry forest is supposed to have   // ground worms, day AND night — CUT 30% (user 2026-07-18): 32→22 cap, 16→11 floor
    // ── THE FOUR LAND MAMMALS, SCALED TO THE REACH (2026-08-17) ── the ceiling was a flat 7 and the count
    // landed on 6 at every shipped view, over a spawn disc of MAM_OUT = 0.94 * 680 = 639. MAM_KEEP is CARD_KEEP
    // now, so that disc is 977 and 6 head would read as 43% of the density this band has always had — the
    // failure the comment on MAM_KEEP records having been measured and reverted once already. x LIFE_DENS_K
    // takes it to 14 per species, which is the SAME animals per acre: the same-species mean nearest-neighbour
    // gap, sqrt(pi * MAM_OUT^2 / n), is 462 voxels before and 463 after — so MAM_APART / MAM_FLOOR / MAM_RELAX
    // are still sized for the spacing they were measured against and need no retuning.
    // The ceiling is now MAM_PER — the band's own width, the only bound that cannot be exceeded without
    // renumbering — rather than a hand-picked 7 that a bigger reach would silently sit on.
    const mamBase = Math.round(nActD * 0.405 * LIFE_DENS_K * bioFracAt((MAM_KEEP * 0.78 + MAM_OUT) * 0.5));
    // ── AND THE FLOOR OF 2 DOES NOT SURVIVE A ZERO FRACTION ── `Math.max(2, …)` ran AFTER the scale, so the
    // deep desert and the blossom — where every mammal is refused outright — still asked for a pair and held
    // none: the last two bands that could never fill. A zero legal area now means zero asked for, and the
    // floor still applies everywhere a mammal can actually stand, which is what it was written for.
    const nMam = mamBase <= 0 ? 0 : nightKN(Math.max(2, Math.min(MAM_PER, mamBase))) & ~1;   // NOCTURNAL: the skunk/porcupine/armadillo are night animals, so the four thin rather than vanish (see NIGHT_NOCT)   // & ~1 → EVEN count (user). ONE expression, because all four species have shared a formula since they were added and four copies of it is four chances to scale three of them.   // …and ONE bioFrac, on that same shared expression, so all four species thin together at a border instead of three of them doing it (floor 3 -> 2 because `& ~1` turns an odd floor into 2 anyway, and 3 would have re-inflated the scaled count)
    // ── THE FLAMINGO'S OWN COUNT ── not scaled by bioFracAt like the other four: they are refused IN the
    // blossom and thinned near it, and this one is the opposite — it lives ONLY there, so the legal-area scalar
    // would thin it exactly where it belongs. Sized off the same nMam expression so it reads at the four
    // mammals' density rather than needing a number of its own.
    // ── AND IT IS ZERO WHEN THERE IS NO BLOSSOM IN REACH (user 2026-08-18: "my game keeps freezing/crashing") ──
    // this is the load-bearing half. A flamingo is BIO_CHERRY and is admitted only at cherryM >= 0.85, but the
    // home-finder it borrows (findPorcHome) knows nothing about the biome and hands back forest points, which the
    // spawn gate then refuses. Stand anywhere but the blossom and all 24 slots run their full 12-retry loop EVERY
    // FRAME, forever, each retry paying a home-finder walk plus several mask samples at ~8 vnoise apiece — a
    // five-figure noise bill per frame that buys nothing. That is the freeze, and it is the exact failure the
    // comment at the annulus branch in tick-creatures.js already warns about for desert creatures routed through
    // a forest home-finder: "it would simply never spawn".
    // Measuring the ring costs 24 mask pairs once, against thousands of wasted retries, and it also means the
    // population fades in as the player approaches rather than popping at a threshold.
    const chFrac = FLAMINGO_ITEM0 ? bioFracCherryAt((MAM_KEEP * 0.78 + MAM_OUT) * 0.5) : 0;
    const nFlamingo = (chFrac > 0 && nMam > 0 && NIGHT_K > 0) ? Math.max(1, Math.round(nMam * chFrac / 2)) * 2 : 0;   // nMam > 0, not !NIGHT_QUIET: nMam now RAMPS to zero through dusk and this floor of 1 pair would otherwise strand two flamingos standing in the dark   // ── AND THE COUNT IS EVEN (user 2026-08-18: "spawn flamingos as a couple") ── rounded in PAIRS rather than clamped to a minimum of 2: an odd want left exactly one bird over, and the leftover is the most visible one precisely because it is the only one standing alone
    const nBunny = BUNNY_ITEM0 ? nMam : 0;             // ground BUNNIES (BUNNY_0..BUNNY_END) — kind 2, hop through the forest
    const nArmadillo = ARMADILLO_ITEM0 ? nMam : 0;     // ground ARMADILLOS (ARM_0..ARM_END) — kind 2, WALK the forest floor at ~9 vox/s
    const nSkunk = SKUNK_WALK.length ? nMam : 0;       // ground SKUNKS (SKUNK_0..SKUNK_END) — kind 2, same cardinal walk as the armadillo
    const pineFrac = PORCUPINE_WALK.length ? bioFracPineAt((MAM_KEEP * 0.78 + MAM_OUT) * 0.5) : 0;
    const nPorcupine = (pineFrac > 0 && nMam > 0) ? Math.max(2, Math.round(nMam * pineFrac)) : 0;   // …and the same for the porcupine's floor of 2 (see nFlamingo above)   // …zero when there is no pine in reach, so the band stops asking instead of burning its retries every frame
    const nDfly = (DFLY_NFRAMES && waterSpots.length) ? nightK(Math.min(3, 1 + (waterSpots.length >> 2))) : 0;   // DRAGONFLIES scale with how much water is in view and take the TOP of the flyer band, so with no water nearby the flock is 100% butterflies exactly as before. RATE HALVED 2026-07-20 (user): cap 6→3, per-spot growth >>1→>>2 — half as many at every water amount
    const nDuck = DUCK_ITEM0 ? nightK(Math.min(4, lakeSpots.length + 1)) : 0;   // MOTHER ducks (slots 16-19) — 1-2 families PER lake, at LEAST 1 in every lake (user): lakes+1 covers every detected lake and lets one get a 2nd family; capped by the 4 mom slots. lakeSpots is last frame's census.
    const nLily = 0;                                   // LIVE drifting lily pads DISABLED (user 2026-07-18) — only the STATIC stamped pads remain. Was `LILY_ITEM0 ? 12 : 0`.
    const nFish = (FISHES.length && waterSpots.length) ? Math.min(12, 3 + waterSpots.length * 3) : 0;   // HALVED AGAIN (user 2026-08-18): ceiling 24→12, base 6→3, per-spot 6→3 — another uniform half off the whole curve, same as the 2026-08-09 quarter. Nothing downstream needs a minimum: FISH_N 32 was never binding, and the species split is an index not a count.   // FISH scale with how much water is in view (lakes AND rivers). CUT 25% (user 2026-08-09): ceiling 32→24, base 8→6, per-spot 8→6 — a uniform quarter off the WHOLE curve, so the cut is the same at every water amount rather than only biting where the ceiling clamped. The per-pool density cap below (~1 fish per 3 census samples) is deliberately UNTOUCHED: it is the anti-cramming ceiling, not the population, and lowering it too would thin small ponds twice. Slot band FISH_0..FISH_END is 32 wide and the live count now stops short of it; species stays an even split (wk % FISHES.length). Was DOUBLED 2026-07-21: base 4→8, per-spot 4→8, ceiling 24→32
    // Count scales with the AREA the birds have to cover. The 60 cap was set when the spawn ring was ~331 vox; the
    // ring is now ~406, which is 1.5x the area, so the same 60 birds read as a thinner, patchier forest. This keeps
    // the ORIGINAL density over the larger disc (~90 at the default view) and is still inside the 180-slot pool.
    // ── AND THE SONGBIRDS SCALE WITH THE FOREST (soak 2026-08-18) ── this was the LAST population with no biome
    // term, and it was the most expensive one to leave that way: perched birds need TREES, there are none in the
    // desert, and want stayed 421 there against 70 alive. 351 slots x 12 retries is ~4,200 wasted placements per
    // frame, which measured as `life` costing 3.9x more in the desert than in the blossom (4.35 ms vs 0.90) —
    // the largest pure-waste CPU item in the game. Scaled by how much of the ring is forest at all, so the count
    // falls where the trees do instead of asking 421 times for a perch that cannot exist.
    const treeFrac = (() => { let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        if (desertM(P.x + Math.cos(a) * CARD_KEEP * 0.7, P.z + Math.sin(a) * CARD_KEEP * 0.7) <= 0.15) ok++; }
      return ok / 24; })();
    // ── AND THE BIRCH WOOD ASKS FOR TWICE AS MANY (user 2026-08-27) ── treeFrac only answers "is this ring
    // forest AT ALL", which is what the desert term needed and is deliberately blind to WHICH forest. The
    // birch multiplier rides on the same 24-point ring so the two agree by construction, and it RAMPS: at the
    // band edge, where half the ring is birch and half is pine, the count lands halfway rather than stepping.
    // CARD_BASE, never CARD_N, is the non-birch number — CARD_N is now just the pool's width (see slots.js),
    // and asking for the whole pool outside the birch is the 4,200-wasted-placements-a-frame mistake the note
    // above records, one biome over.
    const birchFrac = (() => { let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        if (birchM(P.x + Math.cos(a) * CARD_KEEP * 0.7, P.z + Math.sin(a) * CARD_KEEP * 0.7) > 0.5) ok++; }
      return ok / 24; })();
    // __vb.cardN(n) forces this count, so the perched band can be A/B'd at a fixed spot without a rebuild —
    // which is the only honest way to price it, because every boot re-randomises the biome layout under a
    // fixed coordinate (see the vbharness note in docs). -1 = follow the biome, the shipping behaviour.
    const nCard = CARD_NFRAMES ? (CARD_FORCE >= 0 ? Math.min(CARD_N, CARD_FORCE)
                  : Math.round(CARD_BASE * treeFrac * (1 + (CARD_BIRCH_K - 1) * birchFrac))) : 0;   // the FULL pool in birch, CARD_BASE in any other wood, and nothing in open sand

    LIFE_WANT.perched = nCard; LIFE_WANT.fish = nFish; LIFE_WANT.worm = nWorm; LIFE_WANT.flyer = nAct; LIFE_WANT.duck = nDuck;
    // flamingo is published too, or lifeBands().want has no key for it and the band cannot be checked
    // against its target from the tap. NOTE the comment is on its OWN line: putting it mid-line here is
    // what swallowed the three assignments after it, so want.armadillo/.skunk/.porcupine read 0 forever
    // while the spawner used the real values — a tap that says "asked for nothing" about a band that is
    // actually failing to place, which is the opposite diagnosis.
    LIFE_WANT.flamingo = nFlamingo; LIFE_WANT.bunny = nBunny;
    LIFE_WANT.armadillo = nArmadillo; LIFE_WANT.skunk = nSkunk; LIFE_WANT.porcupine = nPorcupine;
    // ── NOTHING LIVES ON THE ICE (user 2026-08-29: "remove all the life from the arctic") ── applied to the
    // WHOLE table in one place rather than as a term in each of the dozen counts above. Two reasons, and the
    // second is the one that decided it: every band would otherwise need its own arctic clause, and the ones
    // added later would silently not get one — which is exactly how treeAt ended up excluding itself from only
    // the two biomes that existed when it was written. A band added to LIFE_WANT after today inherits this for
    // free, because it multiplies whatever is in the table rather than naming its members.
    // A FRACTION, not a flag: the same 24-point ring the birch and pine tests above sample on, so the
    // population thins across the blend band instead of every animal vanishing on the iso-line. Standing deep
    // in the arctic it is 1 and the whole table goes to zero.
    const arcticFrac = (() => { let ok = 0;
      for (let i = 0; i < 24; i++) { const a = i * 0.2617994;
        if (arcticM(P.x + Math.cos(a) * CARD_KEEP * 0.7, P.z + Math.sin(a) * CARD_KEEP * 0.7) > 0.5) ok++; }
      return ok / 24; })();
    if (arcticFrac > 0) { const k9 = 1 - arcticFrac; for (const k in LIFE_WANT) LIFE_WANT[k] = Math.round(LIFE_WANT[k] * k9); }
    const ffLights = [];                               // glowing fireflies + live clash/death SPARKS this frame → the u.fflies point-light slots (window coords + intensity)
    const cshadList = [];                              // ground/water creatures this frame → the u.cshad sun-shadow boxes (window coords + half-extents) so moving lilies/ducks/worms cast shadows
    if ((DUCK_ITEM0 || DFLY_NFRAMES || FISHES.length) && !ED.on && now > lakeScanT) {   // ── LAKE + WATER CENSUS (every 2.5 s) ── grid-sample the view for wide-open-water spots, cluster them into lakes; fish home on the same census;
      lakeScanT = now + 2500; if (CPROF) cpEvt |= 1;                          //    mother ducks below TARGET these so every visible lake gets its own family (pool cap 2 families)
      lakeSpots.length = 0; waterSpots.length = 0;
      const R7 = LIFE_OUT;                             // lake census reaches the full spawn ring — at 0.8·rdV no lake was ever detected out where ducks now spawn, so they could never appear there
      for (let gx7 = -R7; gx7 <= R7; gx7 += 20) for (let gz7 = -R7; gz7 <= R7; gz7 += 20) {
        if (gx7 * gx7 + gz7 * gz7 > R7 * R7) continue;
        const sx8 = P.x + gx7, sz8 = P.z + gz7;
        if (sx8 <= rect.xlo + 4 || sx8 >= rect.xhi - 4 || sz8 <= rect.zlo + 4 || sz8 >= rect.zhi - 4) continue;
        if (!bfWater(sx8, sz8) || !bfSky(sx8, WL + 2, sz8)) continue;   // REAL open-sky water of any width — this is the dragonfly's pool (rivers included)
        { const cw = waterSpots.find((c) => (c.x - sx8) * (c.x - sx8) + (c.z - sz8) * (c.z - sz8) < 90 * 90);   // loose 90-vox merge: a river stays several spots along its length rather than collapsing to one
          if (cw) cw.n++; else waterSpots.push({ x: sx8, z: sz8, n: 1 }); }
        if (!bfOpenW(sx8, sz8)) continue;              // …and from here on, LAKES only (width test) — dry ravines/cave pools never seed a lake spot
        const cl7 = lakeSpots.find((c) => (c.x - sx8) * (c.x - sx8) + (c.z - sz8) * (c.z - sz8) < 260 * 260);   // MERGE a whole lake into ONE spot (was 110 → a big lake split into 3-4 spots → 4 families on it, user); 260 covers a typical lake, still separates distinct ones
        if (cl7) { cl7.n++; cl7.x += (sx8 - cl7.x) / cl7.n; cl7.z += (sz8 - cl7.z) / cl7.n; }   // centroid = lake identity; ax/az stays a KNOWN-GOOD water sample (the centroid of an L-shaped lake can be ashore)
        else lakeSpots.push({ x: sx8, z: sz8, ax: sx8, az: sz8, n: 1 });
      }
    }
    petalTick();                                   // ── CHERRY PETALS ── ambient, so it belongs on the same per-frame beat as the census rather than inside the creature loop; it is its own rate-limiter and costs six oakAt calls every 260 ms, nothing at all outside the blossom
    // ── AND THE PARTICLES CAN NEVER AGAIN STARVE THE WORLD (user 2026-08-20) ── compacting the band fixed the
    // saturated budget, but nothing STOPPED it: the pool is 56 slots and a busy scene (the petal shed was just
    // doubled) can legitimately have most of them alive at once, which would quietly walk the creature budget
    // back down to where it was and reproduce this whole bug. A spark is cosmetic and lives under a second; a
    // creature is the world. So the band takes at most PART_CAP slots and the rest of the pool simply is not
    // drawn this frame — imperceptible on a fleck, decisive for an animal. Creatures are guaranteed
    // 128 - 9 - PART_CAP - BIRD_SLOTS = 83 slots no matter what the weather is doing.
    const PART_CAP = 28;
    // ── A PARTICLE KEEPS THE SLOT IT WAS BORN INTO (user 2026-08-27: "I saw a birch tree falling leaf
    // flicker", "and sometimes it just dissapears") ── this used to RE-PACK the band every frame, walking
    // sparks3d in index order and handing out 9, 10, 11… to whatever was alive. Its own note in
    // ui/achievements.js says what that costs: "only the slot each one writes to moves". Two bugs fall out
    // of it, and the falling leaves get both because they are by far the longest-lived particle here
    // (a fall of 15-45 s against a splash droplet's 0.9 and a tear's 0.7):
    //   · FLICKER. Every birth or death at a LOWER index shifts every live particle above it down a slot.
    //     A drop slot IS the denoiser's identity for that pixel (lifeUid / lifeUidPrev), so a shift rejects
    //     the leaf's whole history and it re-converges from nothing. MEASURED in the birch wood: 1,388 slot
    //     changes over 95,571 live-leaf frames.
    //   · DISAPPEARING. The cap cuts at `sN < PART_CAP`, i.e. in INDEX ORDER, and the petal band is 24..56 —
    //     the highest indices there are. So the leaves are always the ones cut, and the petal band is 32
    //     slots against a PART_CAP of 28, so it overflows on its own with no other particle in the world.
    //     MEASURED: the cap was hit on 2,994 of 2,994 frames and 12.3% of live-leaf frames were never drawn.
    // Ownership is persistent instead: a live particle that already holds a slot keeps it, the dead release
    // theirs, and only genuinely NEW particles allocate — lowest free first, so the band still packs down.
    // A particle that finds the budget full at birth is marked -2 and stays undrawn for its whole life. That
    // is deliberate and it is the same rule the splash already follows ("skip this tear rather than cut a
    // live one short"): a leaf that never appears cannot be seen to be missing, while one that pops into
    // existence halfway down can. sparkBorn is what separates "still the same particle" from "the index was
    // reused", which is the only thing this needs to remember between frames.
    // LIVENESS IS COMPUTED ONCE, INTO partLive, AND EVERY PASS BELOW READS IT. A particle is not reaped on
    // the frame it expires — main/tick-emit.js nulls sparks3d[i] later in the same tick — so "sparks3d[i] is
    // not null" is NOT the same question as "is it alive", and an allocator that asked the first one handed
    // slots to the dead. That inflated the band the COMPOSITE walks per pixel (the trace skips it in one
    // jump; composite cannot, the particles are analytic) and cost 2.7 -> 5.3 ms on the gate.
    { let sN = 0;
      for (let i = 0; i < sparks3d.length; i++) { const sp = sparks3d[i];
        const live = !!sp && (now - sp.born) / 1000 <= sp.life;
        partLive[i] = live ? 1 : 0;
        if (!live) { sparkSlot[i] = -1; sparkBorn[i] = 0; continue; }
        if (sp.born !== sparkBorn[i]) { sparkBorn[i] = sp.born; sparkSlot[i] = -1; }   // the index was recycled — this is a NEW particle, so it must allocate rather than inherit
      }
      partUsed.fill(0);
      for (let i = 0; i < sparks3d.length; i++) { const s9 = sparkSlot[i]; if (partLive[i] && s9 >= 9) partUsed[s9 - 9] = 1; }
      for (let i = 0; i < sparks3d.length; i++) {
        if (!partLive[i] || sparkSlot[i] !== -1) continue;   // -2 = refused at birth and never reconsidered, so it cannot pop in mid-fall
        let got = -1;
        for (let q9 = 0; q9 < PART_CAP; q9++) if (!partUsed[q9]) { got = q9; break; }
        if (got < 0) { sparkSlot[i] = -2; continue; }
        partUsed[got] = 1; sparkSlot[i] = 9 + got;
      }
      for (let q9 = 0; q9 < PART_CAP; q9++) if (partUsed[q9]) sN = q9 + 1;   // HIGH-WATER, not a count: ownership can leave a hole until the next spawn fills it (lowest-free), and the creatures start above the last slot in use
      // ── THE EDITOR'S LIFE STARTS ABOVE THE ANALYTIC BAND ── render/wgsl/trace.js walks the drop slots and
      // steps straight over 5..24: "the 20 death-burst slots are analytic-only". That band is a CONSTANT in the
      // shader while this base is computed here, and the two only agree because the world always has particles:
      // sN saturates at PART_CAP 28, so live creatures land at 37 and clear it.
      // The asset editor has no weather and no sparks, so sN is 0 and the base would be 9 — every creature slot
      // would fall INSIDE the band the tracer skips. That is why a trace-injected model with a correct pose, a
      // valid item and a claimed slot rendered perfectly in the world and drew nothing at all on the stage: the
      // tracer never walked it. Found by writing the identical pose in both and comparing.
      // Floored at 25 rather than widening the shader's skip: that band is analytic by design and nothing else
      // wants it moved, the editor has 103 slots free either way, and a uniform is easier to keep honest than a
      // constant that lives in two languages.
      lifeSlotBase = ED.on ? Math.max(25, 9 + sN) : 9 + sN;
      // ── AND THE GAP IT OPENS MUST BE BLANKED ── the shader walks slots 0..dropN, and dropN is the live count,
      // so every slot BELOW the base is walked whether or not anything wrote it this frame. In the world there
      // is no gap: the base sits exactly at 9 + sN, one past the last spark. The editor's floor jumps it to 25
      // and leaves 9..24 written by nobody — holding whatever a previous frame left there, which the composite
      // then draws. That is the loose green voxels: stale leaf/petal poses from before the editor opened,
      // pinned in place because nothing updates them any more (user 2026-08-22).
      // Item id 0 is the empty-slot marker every other drop writer uses, so one store per orphaned slot clears it.
      // …and the HOLES the persistent ownership leaves inside the band get the same treatment, for the same
      // reason: the shader walks every slot below the base whether or not anything wrote it, so an unowned
      // slot would keep drawing whatever particle used to live there.
      for (let q9 = 0; q9 < sN; q9++) if (!partUsed[q9]) UF[dropOff(9 + q9) + 7] = 0;
      for (let s9 = 9 + sN; s9 < lifeSlotBase; s9++) UF[dropOff(s9) + 7] = 0; }
    let dropCursor = lifeSlotBase;             // COMPACTION: live creatures emit to consecutive slots ABOVE the particle band (5 + pool size, so 29 now that pollen took four); the count goes to the shader in pick2Y.w so its per-pixel loop covers only what exists. (5-24 = the 20 fixed death-burst slots: 4 sparks + 16 individual smoke voxels, user)
    // ── THE FLOCK NO LONGER TAKES THE WHOLE BUDGET (user 2026-08-05: ducklings, salmon and ducks all missing) ──
    // all eleven remaining songbirds used to be written unconditionally, ahead of every other creature, which
    // left 64 - 25 - 11 = 28 slots for the entire rest of the world's life to share. They are also the one kind
    // that is always overhead and always the same, so they were the cheapest thing to bound. Every bird still
    // FLIES — birdStep runs for all of them, so the flock and its hitboxes are unchanged — but only the nearest
    // BIRD_SLOTS are drawn, which hands six slots straight to the creatures the player was losing.
    if (!ED.on && FLYERS.length > 0 && !NIGHT_QUIET) {   // ── AND NOTHING FLIES AT NIGHT (user) ── the else below already deactivates every bird box, so this one clause both grounds the flock and stops it being drawn; the PERCHED songbirds are a separate population (nCard) and are untouched
      const tbF = now / 1000;
      const bOrd = [];
      for (let bi = 1; bi < BIRD_N; bi++) {
        if (birdRagTick(birds[bi])) { birdBoxes[bi].active = false; continue; }   // it is the rigid body now — not stepped, not drawn
        const ps = birdStep(birds[bi], bi, tbF, dt);   // stepped for ALL of them, drawn for some
        if (!ps) { birdBoxes[bi].active = false; continue; }   // ── NO LEGAL SKY ── birdStep returns null when the whole respawn ring is desert (see BIRD_IN there). Same treatment as a ragdolling bird: no pose, no hitbox, nothing drawn — NOT a pose at the world origin, which is what an un-inited bird would have been.
        birdPose(birds[bi], ps);                       // …and the pose it was drawn at, for the ragdoll
        ps.uid = bi;                                   // stable identity for the dynamic-life temporal reprojection
        birdHit(birdBoxes[bi], ps);                    // every cardinal is solid, same as the first
        bOrd.push({ ps, d: (ps.x - P.x) * (ps.x - P.x) + (ps.z - P.z) * (ps.z - P.z) });
      }
      bOrd.sort((a, b) => a.d - b.d);
      for (let i = 0; i < bOrd.length && i < BIRD_SLOTS && dropCursor < DROP_SLOTS; i++) {
        birdWrite(dropCursor, bOrd[i].ps, cam, right, up, fwd);
        dropCursor++;
      }
    } else for (let bi = 1; bi < BIRD_N; bi++) birdBoxes[bi].active = false;
    const fsX = tanH * aspect, fsY = tanH;             // frustum half-slopes + their plane normalisers — the sphere-vs-plane test each staged creature is ranked by
    const fnX = 1 / Math.sqrt(1 + fsX * fsX), fnY = 1 / Math.sqrt(1 + fsY * fsY);
    const dropBase = dropCursor;                       // first slot free after the flying songbirds — the wbf creatures (staged below) fill from here by distance
    let emitN = 0;                                     // wbf poses staged into emitBuf this frame; sorted + emitted after the loop
    let bunnyBoxN = 0; for (const bx9 of bunnyBoxes) bx9.active = false;   // rebuild the solid-bunny hitboxes each frame from the nearest bunnies
    let armBoxN = 0; for (const bx9 of armBoxes) bx9.active = false;   // …and the solid-armadillo hitboxes
    let skunkBoxN = 0; for (const bx9 of skunkBoxes) bx9.active = false;   // …and the solid-skunk hitboxes
    let porcBoxN = 0; for (const bx9 of porcBoxes) bx9.active = false;   // …and the solid-porcupine hitboxes
    let flamBoxN = 0; for (const bx9 of flamBoxes) bx9.active = false;   // …and the solid-flamingo hitboxes, rebuilt from the nearest each frame exactly as the other four are
