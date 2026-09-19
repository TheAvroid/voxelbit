// app_tests_interaction.inl
//
// Lifted out of app.h, which was 17,043 lines with ~16,100 of them inside a
// single class. This file is #included INSIDE the body of ForestApp, at exactly
// the point the code used to sit, so the preprocessor sees the same text in the
// same order -- member declaration order, layout and initialisation order are
// all unchanged. It is not a standalone header and has no include guard.
//
// Contents: hoe, palette, float sweep, rip, audit, food, wheat, clip, locate and fell tests
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === LOCATE TEST === -- what /locate can find, and where it puts you.
    //
    // WHAT THIS IS FOR. /locate <animal> is nineteen rows of a table wired to
    // six populations, and a row wired to the WRONG ONE still compiles, still
    // teleports, and still prints a confident reply. The six fish are the
    // sharpest case: they share one vector and are told apart by an integer,
    // so `catfish` pointing at species 5 would quietly take you to a blue gill
    // for ever. Nothing about that is visible from a screenshot -- the two are
    // fish-shaped and both are in the water.
    //
    // SO THE SURVEY IS THE TEST. Every row is asked, once, from one spawn:
    // what it found, how far off, and -- for the fish -- whether the six
    // answers are six DIFFERENT animals, checked against the lake's own
    // census. A row that finds nothing is not a failure by itself (half the
    // table is gated to one wood) but it must find nothing for the reason the
    // table says, which is the `wood` column printed beside it.
    //
    // AND THEN ONE ARRIVAL OF EACH KIND, because the survey never moves: the
    // land case proves standNear's ordinary answer is the stand-off on dry
    // ground outside a trunk, and the water case proves the same loop walks
    // out of a lake instead of dropping the player on the bed.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === CLIP TEST === -- is any animal inside a tree or a rock?
    //
    // WHAT THIS IS FOR. "the flys were caught flying inside a big rock." The
    // fix for that is spread over five files and nine populations, and every
    // one of those populations is a loop that runs a few hundred times a
    // second in a world nobody is looking at. A rule of that shape is not
    // verified by a screenshot -- you would have to be standing next to the
    // one boulder that has a swarm in it, on the frame it is there.
    //
    // SO THE CHECK IS THE ENGINE'S OWN QUESTION ASKED BACK. solidsTouch is
    // what the animals are now steered by; this walks every live creature
    // every frame and asks it, which means a population that is exempted by
    // accident fails here rather than in a report months later. The list comes
    // from livePoints, which every population had to grow -- nine structs in
    // seven files with nothing in common but a position.
    //
    // WHAT IS ALLOWED TO BE INSIDE SOMETHING. One thing: a perched songbird,
    // which is sitting on a branch in a crown. It is counted and printed
    // rather than skipped -- see LifeAt::inTree.
    //
    // IT MUST BE RUN IN BOTH WOODS. Half the table is gated to one of them
    // (--pine pins the other), and a bee, a frog, a mouse and a snake do not
    // exist at all in the pine band.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === HOE TEST === -- does a swing turn the earth, and does it grow back?
    //
    // WHAT THIS IS FOR. v1's hoeTill is five rules in one function and four of
    // them are refusals -- only soil, never sand, one layer only, and put it
    // all back after forty-five seconds. A refusal that does not fire looks
    // exactly like a refusal that does until you are standing on a tilled
    // beach, and the one that puts it back cannot be watched at all without
    // waiting three quarters of a minute in front of the right square metre.
    //
    // THE REVERT IS TESTED BY MOVING THE CLOCK, not by waiting: World::till
    // stamps each column with the time it was turned and tillRevert takes a
    // time, so handing it one three minutes later is the same arithmetic the
    // engine does and needs no frames at all.
    // -----------------------------------------------------------------------
    void runHoeTest() {
        std::printf("\n=== HOE TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();

        int hoe = -1;
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Earth) hoe = i;
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood, hoe in slot %d\n", pos_.x,
                    pos_.y, pos_.z, world_.terrain.woodName(pos_.x), hoe);
        if (hoe < 0) {
            std::printf("  FAIL -- the hoe did not load.\n");
            return;
        }
        held_.select(hoe);

        // -- WHETHER A WHOLE BITE IS TURNABLE, ASKED IN ONE PLACE ----------
        //
        // ONE COLUMN IS NOT THE BITE. A till is a disc of kTillRadiusM and a
        // shore is a metre of soil against a metre of sand, so a centre that
        // passes on its own can sit in a bite that is mostly refused -- and
        // the test then measures the column under the crosshair, which may be
        // one of the refused ones. That reported "surface 4 -> 4" and "DUG
        // ITSELF DEEPER" on a hoe that had done exactly the right thing.
        //
        // IT IS A LAMBDA BECAUSE TWO PLACES NEED IT and they were not the
        // same place: the site search asks it of the column it walks to, and
        // the swing asks it of the column the RAY LANDS ON, which is a metre
        // further on and was never checked at all.
        // The column the search approves, for the aim below to point at.
        Vec3 site{0.0f, 0.0f, 0.0f};
        const auto discTurnable = [&](int i, int j) {
            TerrainMemo dm;
            const int rv = int(kTillRadiusM / VOXEL_M) + 1;
            for (int dz = -rv; dz <= rv; ++dz)
                for (int dx = -rv; dx <= rv; ++dx) {
                    if (dx * dx + dz * dz > rv * rv) continue;
                    const int qi = i + dx, qj = j + dz;
                    const int qh = world_.terrain.heightVox(qi, qj, dm);
                    const uint8_t qt = world_.terrain.topMaterial(qi, qj, qh, dm);
                    if (isSand(qt) || !isSoilMat(qt)) return false;
                }
            return true;
        };

        // -- STAND ON GRASS, because half the rule is about the grass ---
        //
        // v1 lifts the strands off a column it turns and lays them back on
        // the revert; v2 gets both for free from the mesher's own rule (an
        // edited column grows nothing), which is exactly the kind of thing
        // that is true until it is not. A pine floor is bare in patches, so
        // a spawn picked at random tests it about half the time -- this
        // walks to a column that definitely has blades on it.
        {
            // -- A GRASSY SPOT, AND BESIDE WATER IF THERE IS ANY --------
            //
            // TWO PASSES, AND THE SECOND ONE IS WHY. The planting rule has
            // two halves -- a bed keeps if water is in reach and grows back
            // if it is not -- and a test that walks to the nearest grass
            // lands four metres from a lake about never. Three seeds in a
            // row all reported the dry case, which is the half that would
            // still pass on an engine that ignored water altogether.
            //
            // So: look for grass NEAR WATER first, and settle for plain
            // grass only if the wood has no shore in reach. The report says
            // which it got, and the verdict asserts whichever that was.
            TerrainMemo gm;
            bool got = false, nearWater = false;
            // -- GO TO THE SHORE FIRST, IF THE WORLD HAS ONE ------------
            //
            // A RING SEARCH FROM THE SPAWN IS THE WRONG TOOL. Sixty metres
            // of it found no water on four seeds running, because the
            // nearest lake was a hundred and seventy metres off -- and
            // widening the ring is quadratic in something that was already
            // the slowest part of this test.
            //
            // nearestWater spirals six kilometres and is what /locate uses
            // for exactly this. Move there, stream it, and let the two
            // passes below pick a grassy column off the shore.
            {
                float wx = 0.0f, wz = 0.0f;
                if (nearestWater(&wx, &wz)) {
                    for (int w = 0; w < 300; ++w) {
                        world_.update(Vec3(wx, pos_.y, wz));
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    player_.placeOnGround(walkWorld(), wx, wz);
                    pos_ = player_.eyePosition();
                    std::printf("  walked to the nearest shore (%.0f, %.0f)\n",
                                double(wx), double(wz));
                }
            }
            const int a0 = int(std::floor(pos_.x / VOXEL_M));
            const int b0 = int(std::floor(pos_.z / VOXEL_M));
            for (int pass = 0; pass < 3 && !got; ++pass)
                for (int r = 2; r < 600 && !got; ++r)
                    for (int d = -r; d <= r && !got; ++d) {
                        const int cand[4][2] = {{a0 + d, b0 - r}, {a0 + d, b0 + r},
                                                {a0 - r, b0 + d}, {a0 + r, b0 + d}};
                        for (int k = 0; k < 4 && !got; ++k) {
                            const int i = cand[k][0], j = cand[k][1];
                            const int h = world_.terrain.heightVox(i, j, gm);
                            const uint8_t t = world_.terrain.topMaterial(i, j, h, gm);
                            // -- THE TWO PASSES WANT DIFFERENT COLUMNS ---
                            //
                            // A SHORE IS SAND AND SAND GROWS NOTHING, so
                            // 'grass within four metres of water' is a
                            // column this world mostly does not have --
                            // which is why demanding both found neither.
                            //
                            // So each pass asks for the thing its half of
                            // the test needs: pass 0 wants WATER in reach
                            // (the planting rule) and does not care about
                            // blades; pass 1 wants BLADES (the strand
                            // removal) and does not care about water. The
                            // verdict then asserts whichever it got --
                            // between the two runs everything is covered,
                            // and neither run pretends to cover the other.
                            if (isSand(t)) continue;   // the hoe refuses it anyway
                            // -- THREE PASSES, THREE THINGS TO PROVE ----
                            //
                            //   0  water in reach   -- the planting rule
                            //   1  grass AND a flower standing in the bed
                            //      -- the strand removal AND the scatter
                            //   2  grass alone      -- the fallback
                            //
                            // PASS 1 EXISTS BECAUSE PASS 2 PASSED WITHOUT
                            // PROVING ANYTHING. 'scatter standing 0 -> 0'
                            // is a green tick on a disc that never had a
                            // flower in it, which is exactly the shape of
                            // the bug being tested for -- reported twice,
                            // and the second time the code was wrong in a
                            // way no test here could see.
                            if (pass >= 1 && world_.terrain.strandRows(i, j, t, gm) <= 0)
                                continue;
                            // -- AND THE WHOLE DISC HAS TO BE TURNABLE ---
                            //
                            // ONE COLUMN IS NOT THE BITE. A till is a disc
                            // of kTillRadiusM and a shore is a metre of
                            // soil against a metre of sand, so a centre
                            // that passes on its own can sit in a bite that
                            // is mostly refused -- 35 columns of 81 -- and
                            // the test then measures the column under the
                            // crosshair, which may be one of the refused
                            // ones. That reported 'surface 4 -> 4' and
                            // 'DUG ITSELF DEEPER' on a hoe that had done
                            // exactly the right thing.
                            if (!discTurnable(i, j)) continue;
                            const Vec3 c((float(i) + 0.5f) * VOXEL_M, 0.0f,
                                         (float(j) + 0.5f) * VOXEL_M);
                            // The first pass demands a shore; the second
                            // takes anything. 2 m rather than the rule's 4,
                            // so the swing lands inside it with room.
                            const bool wet = waterWithin(c, 2.0f);
                            if (pass == 0 && !wet) continue;
                            // ASKED OF THE RESIDENT WORLD, which the ring
                            // around a streamed spawn is. A column the
                            // streamer has not reached answers zero and is
                            // simply not chosen by this pass.
                            if (pass == 1 &&
                                world_.scatterShownNear(c, kTillRadiusM) <= 0)
                                continue;
                            // STREAM IT FIRST -- the walk and the mow both
                            // read resident chunks, and a shore six hundred
                            // voxels off is outside the spawn's ring.
                            for (int w = 0; w < 200; ++w) {
                                world_.update(Vec3(c.x, pos_.y, c.z));
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            // STAND BACK FROM IT AND REMEMBER IT. Standing back
                            // is what a player does; remembering it is what stops
                            // the ray landing on a shore nobody checked.
                            player_.placeOnGround(walkWorld(), c.x, c.z - 1.2f);
                            pos_ = player_.eyePosition();
                            site = c;
                            got = true;
                            nearWater = wet;
                        }
                    }
            std::printf("  moved to grass: %s%s\n", got ? "yes" : "none found",
                        (got && nearWater) ? " (beside water)" : "");
        }
        // LOOK DOWN AND FORWARD, which is how anybody tills.
        pitch_ = -55.0f;
        yaw_ = 180.0f;

        TerrainMemo memo;
        const auto matAt = [&](int i, int j, int y) {
            TerrainProbe pr(&world_.terrain, &world_.editStore());
            return pr.material(i, j, y);
        };

        // THE COLUMN THE SWING LANDS ON, not the one under your boots.
        // The first cut of this measured the player's own column and read
        // "surface 13 -> 13" while eighty-one columns a metre in front had
        // plainly turned. A hoe is swung at the ground AHEAD -- that is
        // what a ray aimed sixty degrees down means -- so the test has to
        // look where the tool lands and not where the feet are.
        // -- AND THE RAY IS AIMED AT THE BITE THAT WAS CHOSEN -------------
        //
        // IT WAS NOT LANDING ON IT. The search approves a column, the player
        // stands back from it and looks down at a FIXED ANGLE -- and where
        // that ray comes down depends on the slope and on the eye height. One
        // run landed 1.1 m from the column it had just approved, on sand, and
        // reported the hoe broken; sweeping the pitch to find a landing was
        // the same guess with more tries.
        //
        // SO IT IS POINTED AT THE COLUMN, which is what "swing at that spot"
        // means and what a player does with a mouse. Everything downstream
        // still reads lastSwing_, so the tool is exercised exactly as it is in
        // the game -- only the aiming stops being a coincidence.
        const auto aimAtSite = [&]() {
            const Vec3 t((float(int(std::floor(site.x / VOXEL_M))) + 0.5f) * VOXEL_M,
                         walkGroundM(walkWorld(), site.x, site.z) - 0.05f,
                         (float(int(std::floor(site.z / VOXEL_M))) + 0.5f) * VOXEL_M);
            Vec3 d{t.x - pos_.x, t.y - pos_.y, t.z - pos_.z};
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len > 1e-4f) { d.x /= len; d.y /= len; d.z /= len; }
            // KEPT IN SYNC so anything that reads the camera rather than the
            // swing -- the held tool, the log line -- agrees with the ray.
            yaw_ = atan2f(d.x, -d.z) * 180.0f / PI;
            pitch_ = asinf(d.y) * 180.0f / PI;
            lastSwing_ = swingRay(walkWorld(), pos_, d);
        };
        aimAtSite();
        std::printf("  swing hit %s, material %u\n",
                    lastSwing_.hit ? (lastSwing_.kind == Swing::Ground ? "ground" : "a model")
                                   : "NOTHING",
                    unsigned(lastSwing_.material));
        if (!lastSwing_.hit) {
            std::printf("  FAIL -- the swing found no ground to aim at.\n");
            return;
        }
        if (!discTurnable(int(std::floor(lastSwing_.point.x / VOXEL_M)),
                          int(std::floor(lastSwing_.point.z / VOXEL_M)))) {
            std::printf("  the ray landed on ground the hoe refuses -- "
                        "this run proved nothing\n");
            return;
        }
        const int i0 = int(std::floor(lastSwing_.point.x / VOXEL_M));
        const int j0 = int(std::floor(lastSwing_.point.z / VOXEL_M));
        const int h0 = world_.terrain.heightVox(i0, j0, memo);
        const uint8_t top0 = matAt(i0, j0, h0);
        // -- COUNTED OVER THE WHOLE DISC, NOT AT THE LANDING COLUMN ----
        //
        // Blades are a per-column hash and the pine floor is patchy, so a
        // single column is bare about as often as not -- two runs of this
        // in a row reported "0 blade rows" from a spot the player had been
        // walked to BECAUSE it had grass on it, because the swing lands a
        // column further on than the feet. The disc is what the hoe turns,
        // so the disc is what to count.
        const auto discGrass = [&]() {
            const int rv = int(kTillRadiusM / VOXEL_M) + 1;
            int n = 0;
            for (int dz = -rv; dz <= rv; ++dz)
                for (int dx = -rv; dx <= rv; ++dx)
                    if (dx * dx + dz * dz <= rv * rv) n += bladeRowsAt(i0 + dx, j0 + dz);
            return n;
        };
        const int grass0 = discGrass();
        // WHAT IS STANDING ON THIS BED. Flowers, caps and cones -- the
        // scatter that has no support of its own. Reported twice as not
        // going away, so it is counted rather than looked at.
        const int scatter0 = world_.scatterShownNear(lastSwing_.point, kTillRadiusM);
        std::printf("  it lands on (%d, %d): surface %u at y %d, %d blade voxels in the disc\n",
                    i0, j0, unsigned(top0), h0, grass0);

        const bool turned = tillGround();
        const uint8_t topA = matAt(i0, j0, h0);
        const uint8_t belowA = matAt(i0, j0, h0 - 1);
        const int grassA = discGrass();
        const int scatterA = world_.scatterShownNear(lastSwing_.point, kTillRadiusM);
        std::printf("\n  -- the swing --\n");
        std::printf("  tilled            %s, %zu columns still turned\n",
                    turned ? "yes" : "NO", world_.tilledCount());
        std::printf("  surface voxel     %u -> %u (air is %u)\n", unsigned(top0), unsigned(topA),
                    unsigned(mat::AIR));
        std::printf("  the one below     -> %u (tilled is %u)\n", unsigned(belowA),
                    unsigned(mat::TILLED));
        std::printf("  blade voxels      %d -> %d\n", grass0, grassA);
        std::printf("  scatter standing  %d -> %d\n", scatter0, scatterA);

        // ---- one layer only -------------------------------------------------
        //
        // ASKED OF THE COLUMN, NOT OF THE COUNT. A second swing legitimately
        // turns MORE columns -- the ground under the crosshair just dropped a
        // voxel, so the ray lands slightly differently and catches fresh rim
        // that the first disc missed. v1 does the same and its `tillSet` is
        // per CELL for exactly this reason. What must not happen is this
        // column going down a second voxel.
        lastSwing_ = swingRay(walkWorld(), pos_, forward());
        tillGround();
        const uint8_t topB = matAt(i0, j0, h0);
        const uint8_t belowB = matAt(i0, j0, h0 - 1);
        const bool deeper = !(topB == mat::AIR && belowB == mat::TILLED);
        std::printf("  swung again       %s (surface %u, below %u)\n",
                    deeper ? "DUG ITSELF DEEPER -- WRONG" : "one layer only, correct",
                    unsigned(topB), unsigned(belowB));

        // ---- ...AND A SEED STOPS IT GROWING BACK, IF THERE IS WATER --------
        //
        // (user 2026-09-14: "if there is no water present, the tilled land
        // goes back to dirt/grass like it does currently.")
        //
        // BOTH HALVES, ON THE SAME BED. A test that only planted where
        // there is water would pass on an engine that ignored water
        // entirely, which is the more likely mistake of the two -- there is
        // nothing to write to make a seed take, only something to write to
        // make it NOT take.
        const bool wetHere = waterWithin(lastSwing_.point, kSeedWaterM);
        const size_t bed = world_.plantAt(lastSwing_.point, kTillRadiusM, wetHere);
        // WHAT A SOWN BED LOOKS LIKE, COUNTED RATHER THAN SAMPLED. Three
        // seed voxels lie somewhere ON the bed -- not at a spot this test can
        // name, and deliberately not over the column under the crosshair -- so
        // the disc is swept for them. The band is generous in y because the
        // till lowers a column by one and the ground is not flat.
        int seedVox = 0;
        {
            const int rr = maxi(1, int(std::ceil(kTillRadiusM / VOXEL_M)));
            for (int dj = -rr; dj <= rr; ++dj)
                for (int di = -rr; di <= rr; ++di) {
                    if (di * di + dj * dj > rr * rr) continue;
                    // WIDE ENOUGH FOR A SLOPE. Each seed sits at ITS OWN
                    // column's turned voxel, and a bed of eighty-one columns on
                    // a hillside spans more than the six voxels this allowed --
                    // so a run would count two of three and report the planting
                    // broken when it was the window that was too narrow.
                    for (int y = h0 - 8; y <= h0 + 5; ++y)
                        if (isSeed(matAt(i0 + di, j0 + dj, y))) ++seedVox;
                }
        }
        // AND THE BED IS STILL A BED. This is the whole of the report -- the
        // seed used to be written OVER the tilled voxel, so planting read as
        // the till coming undone.
        const uint8_t sown = matAt(i0, j0, h0 - 1);
        std::printf("\n  -- planting --\n");
        std::printf("  water within %.0f m  %s\n", double(kSeedWaterM),
                    wetHere ? "yes" : "no");
        std::printf("  bed found         %zu columns\n", bed);
        // A SOWN BED HAS TO LOOK SOWN -- the whole of the report was that
        // planting changed nothing you could see.
        std::printf("  seed voxels       %d  %s\n", seedVox,
                    seedVox == 3 ? "three, correct" : "SHOULD BE THREE -- WRONG");
        std::printf("  the bed still     %u (tilled is %u, seed is %u)\n",
                    unsigned(sown), unsigned(mat::TILLED), unsigned(mat::SEED_0));

        // ---- and it grows back ----------------------------------------------
        std::vector<Vec3> back;
        world_.tillRevert(simMs_ * 0.001 + 300.0, &back);
        const uint8_t topR = matAt(i0, j0, h0);
        // A KEPT BED IS AIR *OR A SEED LYING IN IT*. The till empties the
        // surface voxel and a planting puts three seeds back into it, so on a
        // bed that kept, the column under the crosshair reads a seed about
        // a third of the time -- which this called "GREW BACK" on a bed that
        // had plainly done nothing of the kind.
        const bool keptTop = topR == mat::AIR || isSeed(topR);
        const int grassR = discGrass();
        const int scatterR = world_.scatterShownNear(lastSwing_.point, kTillRadiusM);
        std::printf("\n  -- five minutes later --\n");
        std::printf("  surface voxel     %u -> %u (was %u before the hoe)\n", unsigned(topA),
                    unsigned(topR), unsigned(top0));
        std::printf("  blade voxels      %d -> %d (was %d)\n", grassA, grassR,
                    grass0);
        std::printf("  scatter standing  %d -> %d (was %d)\n", scatterA, scatterR,
                    scatter0);
        std::printf("  still turned      %zu\n", world_.tilledCount());
        std::printf("  seeds handed back %zu\n", back.size());
        std::printf("  the bed %s\n",
                    wetHere ? (keptTop ? "KEPT -- planted beside water, correct"
                                                : "GREW BACK -- a planted bed must not")
                            : (topR == top0 ? "grew back -- no water, correct"
                                            : "KEPT -- it has no water, WRONG"));

        // grass0 > 0 IS PART OF THE PASS. Without it a run that happened to
        // land on bare dirt reports a green tick for a rule it never exercised,
        // which is the failure this whole file is written against.
        // THE REVERT'S EXPECTATION FLIPS ON THE WATER. A bed beside a lake is
        // SUPPOSED not to grow back once a seed is in it, so asserting the
        // restoration unconditionally would fail the very case the feature
        // exists for. Which case this spawn is came out of waterWithin, so the
        // test asserts whichever one it got rather than demanding one.
        // tilledCount() GOES TO ZERO EITHER WAY, and asserting otherwise was
        // this test contradicting its own engine: tillRevert DROPS a planted
        // record from the list without undoing it -- it has nothing left to do,
        // and leaving it would walk it again on every frame for the session.
        // So what says a planted bed kept is the GROUND, not the bookkeeping.
        const bool revertRight = wetHere ? keptTop
                                         : (topR == top0 && grassR == grass0 &&
                                            world_.tilledCount() == 0);
        // ---- ...AND THE WET CASE, FORCED --------------------------------
        //
        // THE HARNESS COULD NOT GET TO A WET ONE ON ITS OWN. A shore in
        // this world is SAND, sand grows nothing and the hoe refuses it, so
        // the nearest column a till can even touch is metres inland -- four
        // seeds and a walk to the nearest lake all came back dry. Waiting
        // for a world that happens to have turnable soil within four metres
        // of water is a test that passes by not running.
        //
        // SO THE WATER ANSWER IS SUPPLIED RATHER THAN FOUND. plantAt takes
        // `keep` as an argument precisely because the caller decides it,
        // and handing it true here exercises the half that can actually go
        // wrong: whether a planted bed survives its own revert.
        //
        // WHAT THIS DOES NOT TEST is waterWithin -- whether the engine
        // AGREES there is water. That is a ring search over wetColumnAt,
        // the same predicate the frogs and the lake are placed from, and it
        // is exercised everywhere else. The join between the two is the one
        // line plantSeed writes, and it is not covered here.
        // ...AND ON A WET RUN THE BED ABOVE ALREADY PROVED IT. The forced
        // block below cannot run there -- the ground is still turned, so a
        // second till finds nothing to turn and never gets as far as planting.
        bool keptWhenPlanted = wetHere;
        {
            aimAtSite();
            if (tillGround()) {
                const size_t n2 = world_.plantAt(lastSwing_.point, kTillRadiusM, true);
                const size_t before2 = world_.tilledCount();
                world_.tillRevert(simMs_ * 0.001 + 600.0);
                // A PLANTED RECORD IS DROPPED FROM THE LIST WITHOUT BEING
                // UNDONE, so the count going to zero is expected -- what is
                // being asked is whether the GROUND came back.
                const uint8_t topP = matAt(i0, j0, h0);
                keptWhenPlanted = n2 > 0 && (topP == mat::AIR || isSeed(topP));
                std::printf("\n  -- planted, then five more minutes --\n");
                std::printf("  bed planted       %zu columns of %zu\n", n2, before2);
                std::printf("  surface voxel     %u (air is %u, a seed is %u)\n",
                            unsigned(topP), unsigned(mat::AIR), unsigned(mat::SEED_0));
                std::printf("  the planted bed   %s\n",
                            keptWhenPlanted ? "KEPT, correct"
                                            : "GREW BACK -- a seed must stop the revert");
            }
        }

        // ---- ...AND THE FLOWERS, ON A BED THAT ACTUALLY HAS SOME --------
        //
        // (user 2026-09-14, twice: "the flowers are still not dissapering
        // when being tilled under".)
        //
        // ASKED WHERE THE FLOWERS ARE, NOT WHERE THE SWING WENT. Three
        // runs of the sequence above reported 'scatter standing 0 -> 0',
        // which is a green tick on a disc that never had a flower in it --
        // the same vacuous pass the grass count had before it was made to
        // go looking. Walking the player somewhere floral fought the
        // streamer; this does not move at all, it scans the chunks that are
        // ALREADY resident for a spot with scatter on it and tills that.
        //
        // NOT THROUGH A SWING, deliberately. The swing is covered above;
        // what is unproven here is hideScatterOn, and reaching it through a
        // ray means a second thing that can fail to find anything.
        bool flowersWent = false, flowersBack = false, flowersStayedDown = false;
        {
            Vec3 spot{0, 0, 0};
            int had = 0;
            for (int r = 1; r < 240 && !had; ++r)
                for (int d = -r; d <= r && !had; ++d) {
                    const float o[4][2] = {{float(d), float(-r)}, {float(d), float(r)},
                                           {float(-r), float(d)}, {float(r), float(d)}};
                    for (int k = 0; k < 4 && !had; ++k) {
                        const Vec3 c(pos_.x + o[k][0] * VOXEL_M, 0.0f,
                                     pos_.z + o[k][1] * VOXEL_M);
                        const int m = world_.scatterShownNear(c, kTillRadiusM);
                        if (!m) continue;
                        // ...AND ON GROUND THE HOE WILL ACTUALLY TURN, or
                        // the till does nothing and the flowers stay for a
                        // reason that is not the one being tested.
                        TerrainMemo fm;
                        const int fi = int(std::floor(c.x / VOXEL_M));
                        const int fj = int(std::floor(c.z / VOXEL_M));
                        const int fh = world_.terrain.heightVox(fi, fj, fm);
                        const uint8_t ft = world_.terrain.topMaterial(fi, fj, fh, fm);
                        if (isSand(ft) || !isSoilMat(ft)) continue;
                        spot = Vec3(c.x, (float(fh) + 0.5f) * VOXEL_M, c.z);
                        had = m;
                    }
                }
            std::printf("\n  -- the scatter --\n");
            if (!had) {
                std::printf("  no flowers, caps or cones on turnable ground in reach -- this run proved nothing\n");
            } else {
                const size_t n3 = world_.till(spot, kTillRadiusM, simMs_ * 0.001 + 1000.0);
                const int after3 = world_.scatterShownNear(spot, kTillRadiusM);
                // -- ...AND STILL DOWN AFTER THE CHUNK COMES BACK ----------
                //
                // THE COUNT ABOVE IS NOT THE TEST. It reads the masks in the
                // same breath as the till, and this bug lives entirely in what
                // happens NEXT: a till edits the ground, editing the ground
                // re-meshes the chunk, and adoptMany rebuilds decorDesc from
                // the scatter with every mask back on. Twice this test passed
                // on an engine where the flowers came back a frame later --
                // which is exactly what was being reported, and the only thing
                // the test could not see.
                //
                // SO THE MESHER IS LET ANSWER. update() takes what the workers
                // have finished and adopts it; sixty passes is far more than
                // the handful of chunks one bite touches needs.
                for (int k = 0; k < 60; ++k) world_.update(player_.pos);
                const int adopted3 = world_.scatterShownNear(spot, kTillRadiusM);
                world_.tillRevert(simMs_ * 0.001 + 2000.0);
                const int back3 = world_.scatterShownNear(spot, kTillRadiusM);
                flowersWent = n3 > 0 && after3 == 0;
                flowersStayedDown = adopted3 == 0;
                flowersBack = back3 == had;
                std::printf("  standing on the bed  %d\n", had);
                std::printf("  after the till       %d  %s\n", after3,
                            flowersWent ? "gone, correct"
                                        : "STILL STANDING OVER TURNED EARTH -- WRONG");
                std::printf("  after the re-mesh    %d  %s\n", adopted3,
                            flowersStayedDown
                                ? "still down, correct"
                                : "STOOD BACK UP ON ADOPT -- WRONG (this is the bug)");
                std::printf("  after it grew back   %d  %s\n", back3,
                            flowersBack ? "back, correct" : "DID NOT COME BACK -- WRONG");
            }
        }

        // grass0 > 0 IS ASSERTED ONLY ON THE DRY RUN, which is the one that
        // went looking for blades. The wet run stood on a shore to test the
        // planting rule and a shore has no grass on it -- insisting on both
        // would fail the run that is testing the thing it was sent to test.
        // ONE SEED BACK PER PLANTING, and only on the run whose bed actually
        // reverted -- a kept bed still has the seed in it.
        const bool seedBack = wetHere ? (back.empty()) : (back.size() == 1);
        const bool pass = turned && topA == mat::AIR && belowA == mat::TILLED && !deeper &&
                          grassA == 0 && bed > 0 && revertRight && keptWhenPlanted &&
                          seedVox == 3 && sown == mat::TILLED && seedBack &&
                          (wetHere || grass0 > 0) &&
                          // NOTHING LEFT STANDING ON TURNED EARTH, and it all
                          // comes back with the ground -- unless the bed kept,
                          // in which case it is still turned and they stay down.
                          scatterA == 0 && (wetHere ? true : scatterR == scatter0) &&
                          flowersWent && flowersStayedDown && flowersBack;
        std::printf("\n  %s\n", pass ? "PASS -- it turned the earth, refused to dig itself "
                                       "deeper, and grew back."
                                     : "FAIL -- see the lines above.");
    }

    // -----------------------------------------------------------------------
    // === WHEAT TEST === -- does a swing break a plant, and does it pay out?
    //
    // WHAT THIS IS FOR. The feature is five pieces in five files -- a second
    // ray, an edit that removes nothing solid, a mesher rule that was already
    // there, two kit slots and the drop pool -- and every one of them fails
    // QUIETLY. A ray that never meets a blade, a mow that takes no cells, a
    // probe that goes on reporting the plant it just cut, a drop spilled into a
    // slot nothing can pick up: all four look identical from outside, which is
    // a swing that does nothing.
    //
    // So this walks the whole chain with no window: find a stand of wheat, aim
    // at it, swing, and check each link by what it left behind.
    //
    // IT AIMS THE REAL CAMERA AND CALLS THE REAL HANDLER. breakWheat reads
    // forward() and eyePosition(), so pointing the player at the plant is the
    // only honest way to exercise the ray -- calling mow() directly would test
    // everything except the part most likely to be wrong.
    // -----------------------------------------------------------------------
    // -- THE PALETTE, AS SOMETHING YOU CAN OPEN IN MAGICAVOXEL -------------
    //
    // WHERE THIS IS CALLED FROM IS THE WHOLE MEASUREMENT. The table is served
    // first-come and it is built by the load order -- the terrain ramps, the
    // pines, the birches, the rocks and the decor, the held kit's reservation,
    // then the flyer band -- so a plate taken halfway through that sequence is
    // a plate of a palette that never renders. This runs at the BOTTOM of
    // onLoad, after every loader and after --stage and --level have had their
    // turn, which is the only point at which the table is the thing the game
    // actually runs on.
    //
    // WHAT IT COSTS: nothing but the boot. No window (pass --background), no
    // frame, no chunk streaming beyond what World::build already did.
    //
    // ADD --stage TO INCLUDE THE ASSET DECK and --level for the building.
    // Those two places register colours of their own and a plain run does not
    // open either, so the plain run is the WOOD's table -- which is the one
    // nearly every asset is authored against.
    void runPaletteVox() {
        std::printf("\n=== PALETTE PLATE ===\n");
        const Palette &pal = world_.palette;
        std::vector<PlateCell> terrain, model;
        // Indexed BY ID so the ramps below can be gathered by name; the band is
        // dense and small, and an id-indexed table is what the mat:: constants
        // are addresses into.
        std::vector<PlateCell> terrainCell(static_cast<size_t>(mat::TREE_BASE));
        int foliage = 0, exact = 0;
        for (int id = 1; id < pal.used(); ++id) {
            PlateCell c;
            c.id = id;
            c.foliage = pal.isFoliage(uint8_t(id));
            c.exact = pal.authoredExact(uint8_t(id));
            std::array<uint8_t, 3> src{};
            if (pal.authoredColor(uint8_t(id), &src)) {
                // A MODEL ENTRY, AND THE PLATE SHOWS WHAT THE ART CARRIED.
                // Not what it renders: a needle is lifted 1.7x on the way in,
                // so the albedo is a green no .vox file holds and nothing
                // would ever match against. See Palette::authoredColor.
                c.rgb = src;
                model.push_back(c);
                foliage += c.foliage ? 1 : 0;
                exact += c.exact ? 1 : 0;
            } else {
                // THE TERRAIN BAND. Nothing authored these -- the grass and
                // soil ramps are derived from the trees, the stone band from
                // the boulders, the wheat from the grass -- so the stored
                // albedo IS the honest colour, and it is not lifted.
                const Vec3 a = pal[uint8_t(id)].albedo;
                c.rgb = {uint8_t(Palette::srgbByte(a.x)), uint8_t(Palette::srgbByte(a.y)),
                         uint8_t(Palette::srgbByte(a.z))};
                terrainCell[size_t(id)] = c;
            }
        }
        // -- THE TERRAIN BAND MOVES AS RAMPS, NOT AS CELLS ------------------
        //
        // Every one of these ranges is a GRADIENT and its id order is the
        // gradient's order -- six greens sampled across the pines' foliage,
        // four shades of one soil, the six stones setStoneBand spreads by
        // luminance, ten wheat shades tan to brown. Sorting the cells by colour
        // would put six greens in six different places and there would be no
        // ramp left to read.
        //
        // So the RAMP is the unit: each block keeps its own order, and
        // orderBlocksByHue decides where the block goes. Declared off the mat::
        // constants rather than as literals so the layout cannot drift from the
        // table it describes; anything in the band that no range claims becomes
        // a block of one and finds its own hue neighbours (mat::MOSS ends up
        // beside the grass, mat::DIRT beside the soils).
        const struct {
            uint8_t base, count;
        } kRamps[] = {
            {mat::GRASS_0, mat::GRASS_COUNT},   {mat::BGRASS_0, mat::BGRASS_COUNT},
            {mat::SOIL_0, mat::SOIL_COUNT},     {mat::LITTER_0, mat::LITTER_COUNT},
            {mat::STONE_0, mat::STONE_COUNT},   {mat::SAND_0, mat::SAND_COUNT},
            {mat::WHEAT_0, mat::WHEAT_COUNT},   {mat::BWHEAT_0, mat::BWHEAT_COUNT},
            {mat::SEED_0, mat::SEED_COUNT},
        };
        std::vector<std::vector<PlateCell>> blocks;
        std::vector<uint8_t> owner(size_t(mat::TREE_BASE), 0u);
        for (const auto &r : kRamps) {
            std::vector<PlateCell> block;
            for (int k = 0; k < int(r.count); ++k) {
                const int id = int(r.base) + k;
                if (id < 1 || id >= int(terrainCell.size())) continue;
                owner[size_t(id)] = 1u;
                if (terrainCell[size_t(id)].id) block.push_back(terrainCell[size_t(id)]);
            }
            if (!block.empty()) blocks.push_back(block);
        }
        for (int id = 1; id < int(mat::TREE_BASE); ++id)
            if (!owner[size_t(id)] && terrainCell[size_t(id)].id)
                blocks.push_back({terrainCell[size_t(id)]});
        terrain = orderBlocksByHue(blocks);
        std::printf("  %d terrain entries (ids 1..%d -- ramps, and NOT authorable: "
                    "nearestModelColor starts at %d)\n",
                    int(terrain.size()), int(mat::TREE_BASE) - 1, int(mat::TREE_BASE));
        std::printf("  %d model entries -- %d classified FOLIAGE (lifted, translucent), "
                    "%d minted EXACT (the held kit)\n",
                    int(model.size()), foliage, exact);
        if (!writePalettePlate(opt_.paletteVoxOut, terrain, model, pal.used(),
                               pal.overflowedColors()))
            std::fprintf(stderr, "v2: --palette-vox: could not write %s\n",
                         opt_.paletteVoxOut.c_str());
    }

    // -----------------------------------------------------------------------
    // TAKE AN APPLE OUT OF A TREE AND EAT IT, WITH NO WINDOW.
    //
    // (user 2026-09-17: "import the apple/oranges pick up mechanic" and
    //  "import the eating mechanics from v1 onto all of the food".)
    //
    // FOUR THINGS, AND EACH ONE FAILS SILENTLY ON ITS OWN. The models can load
    // and the kit slot still be -1 (the whole feature was dead exactly that way
    // once -- an empty last bite frame). The pick can find nothing in reach. A
    // fruit can be taken and grow straight back on the next re-mesh, which is
    // the trap hiddenScatter_ exists for and which was reported three times
    // against the flowers. And the bite can run without ever consuming.
    //
    // THE RE-MESH IS THE ONE WORTH SPELLING OUT. `adoptMany` rebuilds every
    // decor instance from the scatter with its mask back on, so a test that
    // counts the fruit in the same breath as the pick reads the one frame in
    // which it is right -- see the note over hideScatterOn. So this lets the
    // mesher answer before believing anything.
    // -----------------------------------------------------------------------
    // WHAT IS STANDING ON NOTHING IN THE MAP -- see World::auditLevelFloaters.
    // -----------------------------------------------------------------------
    // EVERY FLOATING THING IN THE WORLD, WITH NOTHING EXCUSED.
    //
    // (user 2026-09-17: "I need you to audit the game for floating objects ...
    //  we have attempted this multiple times and it is not working".)
    //
    // WHY --float-test WAS NOT ENOUGH, which is most of why this kept being
    // reported as unfixed while the test said PASS:
    //
    //   * ITS MODEL ARM EXCUSES ITSELF. "the tree did not come down in 60
    //     blows -- its crown is not counted". That branch was added for a good
    //     reason (a standing tree is not a floating canopy) and it fires on the
    //     exact case that matters: a trunk cut through while fellTree declines
    //     to take it. On the shipped build it fired at two spawns out of three.
    //   * ITS GROUND ARM DIGS INSIDE THE SWEEP IT IS TESTING. Thirty blows in a
    //     +-6 voxel cluster is 1.2 m across; dropTerrainHangers floods a 2.7 m
    //     box. Every voxel it disturbs is inside the box that judges it, so it
    //     cannot fail however broken the sweep is.
    //   * NOTHING ASKS TWICE. Every sweep in this engine runs in the frame of
    //     the blow and never again. A piece a box called "held from outside"
    //     stays called that after the thing holding it is cut, because the
    //     later blow floods a box that no longer contains the earlier piece.
    //
    // So this digs BIGGER THAN THE SWEEP, chops until the tree gives or two
    // hundred blows are spent, and reports COMPONENTS rather than a total --
    // see LooseReport, and the boulder that is 95% loose untouched. It changes
    // nothing. It is the measurement the fix has to be argued from.
    // -----------------------------------------------------------------------
    bool shovelBad_ = false;

    void runFloatSweep() {
        std::printf("\n=== FLOAT SWEEP ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), player_.pos.x, player_.pos.z);
        pos_ = player_.eyePosition();
        std::printf("  spawn (%.0f, %.0f, %.0f)\n", player_.pos.x, player_.pos.y, player_.pos.z);

        std::vector<Solid> around;
        world_.collidersNear(player_.pos, 120.0f, &around);

        long worstChunky = 0;
        // ---- 1. A TREE AND A ROCK, CHOPPED UNTIL THEY GIVE ------------------
        for (int kind = 0; kind <= 1; ++kind) {
            for (const Solid &s0 : around) {
                if (s0.modelKind != kind || !s0.vol || s0.hx <= 0.0f) continue;
                const Solid so = s0;
                World::LooseReport born, was;
                if (!world_.looseReportNow(so, &born, &was)) continue;
                std::printf("\n  --- %s at (%.0f, %.0f) ---\n", kind ? "a rock" : "a tree", so.cx,
                            so.cz);
                std::printf("    as drawn : %ld loose voxels in %d pieces, largest %ld, %ld of"
                            " them 27+\n",
                            born.voxels, born.pieces, born.largest, born.chunky);
                // AT THE TRUNK, FROM BESIDE IT, AND SWEPT ACROSS THE CUT.
                //
                // runFellTest's aim, copied because it is the only one in this
                // file that reliably cuts a tree through, and both halves of it
                // matter. NOT THE MIDDLE OF ITS BOX: a birch is a trunk with
                // the crown leaning off it, so the box centre can be five
                // metres from the wood -- the first cut of this audit aimed
                // there and landed 9 blows out of 200, which measured its own
                // aim and nothing else. And SWEPT, because every blow from one
                // point along one ray eats a tunnel, and a tunnel severs
                // nothing.
                //
                // STAND WHERE THE TREE IS, too. The ground patch follows the
                // player and carveModel reaches 5 m.
                player_.placeOnGround(walkWorld(), so.cx, so.cz);
                double bx = 0.0, bz = 0.0;
                long nb = 0;
                for (int mz = 0; mz < int(so.msz); ++mz)
                    for (int mx = 0; mx < int(so.msx); ++mx)
                        if (solidVoxel(so, mx, 0, mz)) {
                            bx += double(mx) + 0.5;
                            bz += double(mz) + 0.5;
                            ++nb;
                        }
                if (!nb) {
                    std::printf("    it has nothing at its base -- skipped\n");
                    break;
                }
                float wx = 0.0f, wz = 0.0f;
                solidWorldSpace(so, float(bx / double(nb)) * VOXEL_M,
                                float(bz / double(nb)) * VOXEL_M, &wx, &wz);
                const float cutY = so.baseY + 1.2f;
                bool camedown = false;
                int blows = 0, landed = 0;
                for (; blows < 200 && !camedown; ++blows) {
                    const float wob = (float(blows % 11) - 5.0f) * 0.12f;
                    const Vec3 eye{wx + 3.0f, cutY + (float(blows % 3) - 1.0f) * 0.1f, wz + wob};
                    const Vec3 dir{-1.0f, 0.0f, 0.0f};
                    if (!world_.carveModel(so, eye, dir, 5.0f, kDigRadiusVox)) continue;
                    ++landed;
                    camedown = world_.fellTree(physics_, so, dir, simMs_);
                }
                World::LooseReport now;
                world_.looseReportNow(so, nullptr, &now);
                // HOW MUCH WOOD THE BLOWS ACTUALLY MOVED, and how much is left
                // across the cut. A tree that will not fall is either one the
                // aim keeps missing or one whose trunk is wider than a bite can
                // clear at a single height -- and those want opposite fixes, so
                // the count has to say which.
                {
                    int tot = 0, atCut = 0, cutRow = 0;
                    world_.modelSolidProfile(so, so.baseY + 1.2f, &tot, &atCut, &cutRow);
                    std::printf("    wood left: %d voxels in the model, %d of them across row"
                                " %d (the cut height)\n",
                                tot, atCut, cutRow);
                }
                std::printf("    trunk at (%.1f, %.1f), %d blows (%d landed), %s\n", wx, wz,
                            blows, landed,
                            camedown ? "it came down" : "IT IS STILL STANDING");
                std::printf("    now      : %ld loose voxels in %d pieces, largest %ld, %ld of"
                            " them 27+\n",
                            now.voxels, now.pieces, now.largest, now.chunky);
                // THE DELTA IS THE VERDICT. What was loose when the model was
                // drawn is how the .vox was drawn and nobody has ever pointed
                // at it; what the blows ADDED is the rule being broken.
                const long dv = now.voxels - born.voxels;
                const long dc = long(now.chunky) - long(born.chunky);
                // THE SEVER TEST'S OWN VIEW, beside the mesher's. If these
                // two disagree -- fine says the top is off, coarse says it is
                // attached -- that is the grain gap, and it is why a tree can
                // look cut through and refuse to fall. See World::coarseLoose.
                {
                    int cc = 0, cb = 0;
                    float cf = 0.0f;
                    if (world_.coarseLoose(so, &cc, &cb, &cf))
                        std::printf("    sever test (COARSE): %d loose cells, %d at birth,"
                                    " %.1f%% of the model loose%s\n",
                                    cc, cb, double(cf) * 100.0,
                                    (!camedown && now.voxels - born.voxels > 200)
                                        ? "   <-- FINE SAYS SEVERED, COARSE SAYS ATTACHED"
                                        : "");
                }
                std::printf("    THE BLOWS LEFT: %+ld voxels hanging, %+ld pieces of 27+\n", dv,
                            dc);
                if (dc > worstChunky) worstChunky = dc;
                break;   // one of each kind is enough; the flood is the slow part
            }
        }

        // ---- 2. THE GROUND, DUG WIDER THAN THE SWEEP THAT JUDGES IT ---------
        //
        // AN ISLAND, NOT A TRENCH, AND THE FIRST CUT OF THIS GOT IT WRONG.
        //
        // A line of blows undercutting the crust along x severs NOTHING: the
        // crust above it is still joined to the world along z on both sides.
        // It measured 0 and the 0 was true and meaningless.
        //
        // What actually leaves terrain in the air is a lid: hollow a room out
        // at depth, then cut a RING through the crust around that room, and the
        // disc in the middle is joined to nothing. That is what a player does
        // with a pick and it is the case every local box misses -- the ring is
        // 8 m across and kHangBoxVox sees 2.7 m, so no single blow can ever
        // contain both the cut and the piece it frees.
        // ---- 2a. ORDINARY SHOVELLING, AND WHAT THE WATCH DOES TO IT ------
        //
        // (user 2026-09-17: "when shoveling the ground causes glitches in the
        //  terrain ... whenever Im hitting the meshed terrain, it glitches
        //  out.")
        //
        // THE WATCH RUNS ON EVERY dig NOW, so the first question is whether
        // ordinary digging makes it remove anything at all. It should not: a
        // shovel takes bites out of a hillside and a hillside is one connected
        // mass, so there is nothing to cut loose. Anything it takes here is
        // terrain disappearing for no reason the player can see, which is
        // exactly what "glitches out" describes.
        //
        // CONSERVATION IS THE OTHER HALF. Whatever it does take must come back
        // as bodies, voxel for voxel -- see the vanishing bug it re-introduced
        // once already. Removed-minus-spawned is the number that says so.
        {
            std::printf("\n  --- ordinary shovelling ---\n");
            TerrainMemo sm;
            const int si = int(std::floor(player_.pos.x / VOXEL_M));
            const int sj = int(std::floor((player_.pos.z + 4.0f) / VOXEL_M));
            const int sh = world_.terrain.heightVox(si, sj, sm);
            const int SR = 70, sn = SR * 2 + 1;
            const int ai = si - SR, aj = sj - SR, ay = sh - SR;
            auto countSolid = [&]() {
                long n = 0;
                TerrainMemo m;
                for (int c = 0; c < sn; ++c)
                    for (int a = 0; a < sn; ++a)
                        for (int b = 0; b < sn; ++b)
                            if (world_.terrainSolidAt(ai + a, aj + c, ay + b, m)) ++n;
                return n;
            };
            const long before = countSolid();
            // A HUNDRED BITES OVER FIVE METRES, at and just under the surface,
            // which is what digging a hole with a shovel looks like.
            uint32_t rs = 7771u;
            auto rnd = [&]() {
                rs ^= rs << 13;
                rs ^= rs >> 17;
                rs ^= rs << 5;
                return rs;
            };
            int bites = 0;
            for (int k = 0; k < 100; ++k) {
                const int px = si + int(rnd() % 51u) - 25;
                const int pz = sj + int(rnd() % 51u) - 25;
                const int hh = world_.terrain.heightVox(px, pz, sm);
                const int py = hh - int(rnd() % 12u);
                world_.dig(Vec3{(float(px) + 0.5f) * VOXEL_M, (float(py) + 0.5f) * VOXEL_M,
                                (float(pz) + 0.5f) * VOXEL_M},
                           kDigRadiusVox);
                ++bites;
            }
            const long dug = countSolid();
            const auto s0 = world_.floatStat();
            // TIMED, because "glitches out" has meant a STALL every previous
            // time this user has said it -- see the hoe's ground patch, which
            // was 25 ms landing on the frame of every swing. The watch runs on
            // every dig now, so what it costs while somebody is digging is the
            // question, and the WORST step is the one that is felt.
            int spins = 0;
            double wNs = 0.0, wWorst = 0.0;
            for (; spins < 40000; ++spins) {
                const auto t0 = std::chrono::steady_clock::now();
                world_.stepFloatWatch(physics_, simMs_);
                const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count());
                wNs += ns;
                if (ns > wWorst) wWorst = ns;
                simMs_ += 1000.0 / 60.0;
                if (!world_.floatStat().queued) break;
            }
            const long after = countSolid();
            const auto s1 = world_.floatStat();
            const long watchTook = dug - after;
            const long watchGave = s1.voxelsDropped - s0.voxelsDropped;
            std::printf("    %d bites: the shovel took %ld voxels\n", bites, before - dug);
            std::printf("    the watch: %ld jobs, %ld pieces, %ld bodies, %ld refused,"
                        " %ld too big\n",
                        s1.jobsDone - s0.jobsDone, s1.piecesDropped - s0.piecesDropped,
                        s1.bodies - s0.bodies, s1.refused - s0.refused,
                        s1.tooBig - s0.tooBig);
            std::printf("    the watch removed %ld voxels and made %ld of them into bodies  %s\n",
                        watchTook, watchGave,
                        watchTook == watchGave
                            ? (watchTook == 0 ? "nothing taken, correct" : "conserved")
                            : "MISMATCH -- TERRAIN VANISHED, WRONG");
            std::printf("    it spent %d frames on that: %.3f ms a frame, worst %.3f ms\n",
                        spins, spins ? wNs / double(spins) / 1e6 : 0.0, wWorst / 1e6);
            if (watchTook != watchGave) shovelBad_ = true;
            world_.clearDebris(physics_);
        }

        std::printf("\n  --- the ground: a hollowed room with its lid cut free ---\n");
        TerrainMemo tm;
        const int ci0 = int(std::floor(player_.pos.x / VOXEL_M));
        const int cj0 = int(std::floor((player_.pos.z + 6.0f) / VOXEL_M));
        const int h0 = world_.terrain.heightVox(ci0, cj0, tm);
        int blows = 0;
        const int kRoomVox = 40;   // 4 m radius -- the room, and the lid it leaves
        auto digAt = [&](int wi, int wj, int wy) {
            world_.dig(Vec3{(float(wi) + 0.5f) * VOXEL_M, (float(wy) + 0.5f) * VOXEL_M,
                            (float(wj) + 0.5f) * VOXEL_M},
                       kDigRadiusVox);
            ++blows;
        };
        // THE DEPTHS ARE CHOSEN AROUND kDigRadiusVox AND THE FIRST CUT WAS
        // NOT. A blow is a sphere of radius 3, so digging at hh-3 takes
        // everything from hh-6 up to hh -- the lid included. The first version
        // of this hollowed from hh-3 and then wondered why there was nothing
        // left to float. The room starts at hh-9 (reaching up to hh-6) so rows
        // hh-5..hh survive as a 60 cm lid, and the ring is cut at hh-1 and hh-4
        // so together they take hh+2..hh-7 -- through the lid and into the room.
        // 1. HOLLOW THE ROOM
        for (int dz = -kRoomVox; dz <= kRoomVox; dz += 3)
            for (int dx = -kRoomVox; dx <= kRoomVox; dx += 3) {
                if (dx * dx + dz * dz > kRoomVox * kRoomVox) continue;
                const int wi = ci0 + dx, wj = cj0 + dz;
                const int hh = world_.terrain.heightVox(wi, wj, tm);
                for (int d = 9; d <= 18; d += 4) digAt(wi, wj, hh - d);
            }
        // 2. ...AND CUT THE RING, so the lid is joined to the world by nothing
        for (int a = 0; a < 720; ++a) {
            const float th = float(a) * 0.0087266f;   // half-degree steps: no gaps
            const int wi = ci0 + int(std::lround(std::cos(th) * float(kRoomVox)));
            const int wj = cj0 + int(std::lround(std::sin(th) * float(kRoomVox)));
            const int hh = world_.terrain.heightVox(wi, wj, tm);
            digAt(wi, wj, hh - 1);
            digAt(wi, wj, hh - 4);
        }
        // ...and flood a region far bigger than any bite box. TWICE: once the
        // instant the cut is made, and once after the float watch has had its
        // turn. The pair is the whole point of this audit -- the first number
        // is what the per-blow sweeps left behind, the second is what survives
        // the layer that was built to catch it.
        const int R = 60, n2 = R * 2 + 1;
        const int i0 = ci0 - R, j0 = cj0 - R, y0 = h0 - R;
        std::vector<uint8_t> sol(size_t(n2) * size_t(n2) * size_t(n2), 0), seen(sol.size(), 0);
        auto ix = [&](int a, int b, int c) {
            return size_t(a) + size_t(c) * size_t(n2) + size_t(b) * size_t(n2) * size_t(n2);
        };
        long gv = 0, gp = 0, gLargest = 0, gChunky = 0;
        auto census = [&](const char *label) {
            std::fill(sol.begin(), sol.end(), 0);
            std::fill(seen.begin(), seen.end(), 0);
            long nSolid = 0;
            for (int c = 0; c < n2; ++c)
                for (int a = 0; a < n2; ++a)
                    for (int b = 0; b < n2; ++b)
                        if (world_.terrainSolidAt(i0 + a, j0 + c, y0 + b, tm)) {
                            sol[ix(a, b, c)] = 1;
                            ++nSolid;
                        }
            std::vector<int> st;
            for (int c = 0; c < n2; ++c)
                for (int a = 0; a < n2; ++a)
                    if (sol[ix(a, 0, c)]) {
                        seen[ix(a, 0, c)] = 1;
                        st.push_back(int(ix(a, 0, c)));
                    }
            static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            auto flood = [&](long *cnt) {
                while (!st.empty()) {
                    const int q0 = st.back();
                    st.pop_back();
                    if (cnt) ++*cnt;
                    const int a = q0 % n2, c = (q0 / n2) % n2, b = q0 / (n2 * n2);
                    for (const int *o : off) {
                        const int x = a + o[0], y = b + o[1], z = c + o[2];
                        if (x < 0 || y < 0 || z < 0 || x >= n2 || y >= n2 || z >= n2) continue;
                        const size_t q = ix(x, y, z);
                        if (!sol[q] || seen[q]) continue;
                        seen[q] = 1;
                        st.push_back(int(q));
                    }
                }
            };
            long nReached = 0;
            flood(&nReached);
            // INTERIOR ONLY -- a voxel on this box's own wall may be held from
            // just outside it, and counting it would be measuring the ruler.
            gv = gp = gLargest = gChunky = 0;
            for (int b = 1; b < n2 - 1; ++b)
                for (int c = 1; c < n2 - 1; ++c)
                    for (int a = 1; a < n2 - 1; ++a) {
                        const size_t q = ix(a, b, c);
                        if (!sol[q] || seen[q]) continue;
                        seen[q] = 1;
                        st.push_back(int(q));
                        long n = 0;
                        flood(&n);
                        gv += n;
                        ++gp;
                        if (n > gLargest) gLargest = n;
                        if (n >= kFloatPieceVox) ++gChunky;
                    }
            // SAY WHAT THE FLOOD SAW. A zero is worth nothing without it: two
            // versions of this reported "nothing hanging" because the dig had
            // REMOVED the lid rather than freed it, and the verdict alone
            // cannot tell those apart.
            std::printf("    %-18s %ld solid, %ld reachable -- HANGING %ld voxels in %ld"
                        " pieces, largest %ld, %ld of them 27+\n",
                        label, nSolid, nReached, gv, gp, gLargest, gChunky);
        };
        std::printf("    %d blows, room %d voxels across, flood %d voxels across\n", blows,
                    kRoomVox * 2, n2);
        census("straight after:");

        // ---- ...AND NOW LET THE WATCH HAVE ITS TURN -----------------------
        //
        // What the game does over the following second, driven here as fast as
        // it will go. stepFloatWatch is budgeted per frame by design, so this
        // spins it until its queue is empty rather than guessing a frame count
        // -- and caps the spin, because a watch that never drains is itself a
        // finding and must not hang the test.
        int spins = 0, madeTotal = 0;
        // TIMED, because this runs inside every frame of the real game and a
        // hitch on a pick swing is a live complaint with its own harness --
        // see --hitch. The budget constants exist to keep the WORST step small,
        // so the worst is what is reported, not the mean.
        double watchNs = 0.0, worstNs = 0.0;
        for (; spins < 20000; ++spins) {
            const auto t0 = std::chrono::steady_clock::now();
            madeTotal += world_.stepFloatWatch(physics_, simMs_);
            const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - t0)
                                         .count());
            watchNs += ns;
            if (ns > worstNs) worstNs = ns;
            simMs_ += 1000.0 / 60.0;
            if (!world_.floatStat().queued) break;
        }
        const auto fs = world_.floatStat();
        std::printf("    the watch: %d steps, %ld jobs, %ld pieces -> %ld bodies, %ld voxels,"
                    " %ld deferred, %ld refused, %ld too big\n",
                    spins, fs.jobsDone, fs.piecesDropped, fs.bodies, fs.voxelsDropped,
                    fs.deferred, fs.refused, fs.tooBig);
        std::printf("    the watch costs %.3f ms a frame on average, worst step %.3f ms\n",
                    spins ? watchNs / double(spins) / 1e6 : 0.0, worstNs / 1e6);
        std::printf("    of which: %.1f ms making bodies, %.1f ms filling the cubes\n",
                    fs.spawnMs, fs.fillMs);
        census("after the watch:");

        std::printf("\n=== VERDICT ===\n");
        std::printf("  models: %+ld pieces of 27+ voxels left hanging by the blows\n", worstChunky);
        std::printf("  ground: %ld pieces (%ld of 27+) still hanging after the watch ran\n", gp,
                    gChunky);
        std::printf("  shovel: %s\n",
                    shovelBad_ ? "THE WATCH ATE TERRAIN NOBODY CUT LOOSE -- WRONG"
                               : "ordinary digging leaves the watch with nothing to do");
        std::printf("  %s\n", (worstChunky > 0 || gChunky > 0 || shovelBad_)
                                  ? "FAIL -- there is floating geometry nothing will ever drop."
                                  : "PASS -- nothing of any size is left in the air.");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // DOES A ROUND IN A WALL TAKE A STRIP OUT OF IT?
    //
    // (user 2026-09-17, THREE times: "the bullet impact chunks are still
    //  causing a vertical rip".)
    //
    // WHY THIS EXISTS RATHER THAN --fire-frame. That flag fires one round from
    // the camera on a named frame of the REAL LOOP, which means the level has
    // to render -- eight million triangles a frame -- and headlessly it takes
    // upwards of nine minutes to reach the shot. One shot per run, one bearing
    // per run, and the thing being hunted needs dozens of both.
    //
    // The carve and the sweep do not need a renderer. --float-audit already
    // loads the level and walks its grid with no frame loop at all; this does
    // the same and calls carveLevelToBody directly, hundreds of times, at
    // points picked off the map's own geometry.
    //
    // AND IT MEASURES THE SHAPE, NOT THE COUNT. "Breaking things vertically"
    // is a statement about DIMENSIONS: 2 x 2 x 25 is a rip, 5 x 4 x 5 is the
    // chip the shot is supposed to leave. A voxel total cannot tell them apart
    // and every report before this one was a voxel total.
    // -----------------------------------------------------------------------
    void runRipTest() {
        std::printf("\n=== RIP TEST ===\n");
        if (!world_.levelOn()) {
            std::printf("  the level is not open -- pass --level\n");
            return;
        }
        // SHOTS ALONG THE INSIDE OF THE MAP, at head height and a little above
        // and below it, on a lattice. Not aimed: the point is coverage, and a
        // round that meets nothing costs one early return.
        int fired = 0, cut = 0, rips = 0, worstLong = 0;
        int worstDim[3] = {0, 0, 0};
        double worstAt[3] = {0, 0, 0};
        long long totalVox = 0;
        const auto t0 = std::chrono::steady_clock::now();
        // A METRE LATTICE. The first cut of this fired 38 rounds and that is
        // not enough to believe a zero from: the map is 49 x 98 m and most of
        // a coarse lattice lands in open air.
        for (int gz = 2; gz < 96; gz += 2)
            for (int gx = 2; gx < 48; gx += 2)
                for (int gy = 1; gy <= 7; gy += 1) {
                    const Vec3 lo = World::levelOrigin();
                    const Vec3 at{lo.x + float(gx) * 1.0f, lo.y + float(gy) * 1.0f,
                                  lo.z + float(gz) * 1.0f};
                    // Only shoot where there is something to shoot AT, so the
                    // lattice does not spend its budget on open air.
                    if (!world_.levelSolidAtM(at)) continue;
                    ++fired;
                    world_.clearHangReport();
                    int nOut = 0;
                    Vec3 spoilAt{0, 0, 0};
                    world_.carveLevelToBody(physics_, at, kBulletChipVox, simMs_, kArrowAbsorbM,
                                            &nOut, &spoilAt);
                    if (world_.lastHangVox() <= 0) continue;
                    ++cut;
                    totalVox += world_.lastHangVox();
                    int w = 0, h = 0, d = 0;
                    world_.lastHangDims(&w, &h, &d);
                    // A RIP IS LONG AND THIN. Four times its own thickness is
                    // the line: a 2 x 2 x 25 rod is twelve, a chip is one.
                    const int lng = maxi(w, maxi(h, d));
                    const int thin = maxi(1, mini(w, mini(h, d)));
                    if (lng >= 4 * thin && lng >= 8) {
                        ++rips;
                        if (lng > worstLong) {
                            worstLong = lng;
                            worstDim[0] = w;
                            worstDim[1] = h;
                            worstDim[2] = d;
                            worstAt[0] = at.x;
                            worstAt[1] = at.y;
                            worstAt[2] = at.z;
                        }
                    }
                    // THE POOL HAS TO BE DRAINED or the later shots cannot
                    // spawn and the test measures kDebrisInstances.
                    world_.clearDebris(physics_);
                }
        std::printf("  %d rounds into the map, %d of them cut something loose (%lld voxels)\n",
                    fired, cut, totalVox);
        std::printf("  %d of those were RIPS -- long and thin rather than a chip\n", rips);
        if (rips)
            std::printf("  worst %d x %d x %d voxels at (%.1f, %.1f, %.1f)\n", worstDim[0],
                        worstDim[1], worstDim[2], worstAt[0], worstAt[1], worstAt[2]);
        std::printf("  (%.0f ms)\n", std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - t0)
                                         .count());
        std::printf("\n  %s\n", rips == 0
                                    ? "PASS -- every hole left a chip, not a strip."
                                    : "FAIL -- rounds are tearing strips out of the walls.");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // --pole-test: BREAK EVERY POST IN THE MAP AND WATCH IT COME DOWN.
    //
    // (user 2026-09-18: "on the nuketown map, the light pole is not being\n    //  subject to gravity when cut from the static terrain".)
    //
    // THE COMPANION TO --rip-test, AND THE TWO ARE A PAIR. That one says a
    // round leaves a chip rather than tearing a strip out of a wall; this one
    // says a post that is genuinely cut through comes away. Tightening either
    // guard until the other fails is exactly the loop this fault has been in,
    // so neither is allowed to be the only test that is run.
    //
    // A SINGLE ROUND DOES NOT SEVER A POST -- a bullet bite is 7 voxels and a
    // light pole is 5 across -- so this cuts with a radius that goes through.
    // Anything less tests whether the pole was hit, not whether it falls.
    // -----------------------------------------------------------------------
    void runPoleTest() {
        std::printf("\n=== POLE TEST ===\n");
        if (!world_.levelOn()) {
            std::printf("  the level is not open -- pass --level\n");
            return;
        }
        std::vector<Vec3> posts;
        world_.findLevelPosts(20, &posts);
        std::printf("  %zu free-standing posts at least 2.0 m tall\n", posts.size());
        if (posts.empty()) {
            std::printf("\n  FAIL -- found no posts to cut, so this proves nothing.\n");
            std::fflush(stdout);
            return;
        }
        // Wide enough to go through a 5-voxel shaft; see the note above.
        const int kCutVox = 3;
        // -- THE VERDICT IS WHAT IS LEFT HANGING, NOT WHAT CAME DOWN -------
        //
        // The first cut of this test asked "did the sweep drop something" and
        // reported 214 of 223 posts as failures -- because most columns the
        // finder calls a post are WALLS, a 0.3 m bite does not sever a wall,
        // and dropping nothing is the correct answer there. It was measuring
        // the finder, not the sweep.
        //
        // levelHangingAbove asks the rule's own question instead: after the
        // cut, is anything above it still in the grid and unable to reach the
        // foundation? A post that fell answers 0 because it is gone; a wall
        // answers 0 because it still stands on the ground. Only a hanger
        // answers non-zero, which is exactly the thing the rule forbids.
        int severed = 0, hanging = 0, untouched = 0, worst = 0;
        // What the simulation phase below counts. See the note there.
        int settled = 0, stillFalling = 0, sunk = 0, fellThrough = 0, lost = 0;
        // Only pieces this big are followed -- a speck in a bullet hole
        // says nothing about a pole.
        const int kBigPieceVox = 100;
        // Four seconds at 60 Hz: a 3.4 m post has 0.2 m to fall and is long
        // since still, and what is not still by then is not going to be.
        const int kSettleFrames = 240;
        // Slower than this is resting contact, not a fall.
        const float kStillM = 0.25f;
        double worstAt[3] = {0, 0, 0};

        // =================================================================
        // ARM ZERO: THE SAME CUT, WITH THE BODY POOL FULL.
        //
        // (user 2026-09-18: "on nuketown, the pole just deleted itself,
        //  instead of being subject to physics.")
        //
        // EVERY OTHER ARM OF EVERY OTHER TEST DRAINS THE POOL, and that is
        // exactly what hid this. runRipTest says why in as many words -- "the
        // pool has to be drained or the later shots cannot spawn and the test
        // measures kDebrisInstances" -- and the sweep below does the same
        // between posts. It is the right call for measuring the SWEEP, and it
        // means neither test can ever see the state the game is actually in:
        // a firefight does not drain anything. Every round leaves a chip that
        // nobody collects and that lives kLevelChipLifeMs, so sixty-four rounds
        // -- thirteen seconds of trigger at kBulletIntervalMs -- and the pool
        // is full for the next minute and a half.
        //
        // WHAT THAT USED TO DO: the sweep cut the pole out of the grid, asked
        // spawnDebris for a slot, was told no, and the pole ceased to exist.
        //
        // THE PROOF IS THE PAIR OF NUMBERS. The free count is read immediately
        // before the cut and it is ZERO -- spawnPiece takes the first slot that
        // is not live, so with none free there is no body it could have
        // returned by accident. A body existing after that means room was MADE
        // for it. See World::makeRoomForBodies.
        int poolFree = -1, poolVox = 0, poolBody = 0;
        bool poolTried = false;
        {
            std::printf("\n  -- arm 0: the same cut with the pool full, which is what a "
                        "firefight leaves --\n");
            world_.clearDebris(physics_);
            const Vec3 org = World::levelOrigin();
            int fired = 0;
            for (int gz = 2; gz < 96 && fired < 4 * kDebrisInstances; gz += 1)
                for (int gx = 2; gx < 48 && fired < 4 * kDebrisInstances; gx += 1)
                    for (int gy = 1; gy <= 3; ++gy) {
                        const Vec3 at{org.x + float(gx) * 1.0f, org.y + float(gy) * 1.0f,
                                      org.z + float(gz) * 1.0f};
                        if (!world_.levelSolidAtM(at)) continue;
                        ++fired;
                        world_.carveLevelToBody(physics_, at, kBulletChipVox, simMs_,
                                                kArrowAbsorbM);
                    }
            std::printf("     %d rounds into the map -- %d of %d body slots free\n", fired,
                        world_.debrisFree(), kDebrisInstances);
            // AND THOSE ROUNDS ARE REAL DAMAGE. The lattice walks the whole map
            // at chest height, so a few of them land on a post and cut it
            // before the sweep below ever reaches it -- which is why this run
            // reports a handful more "the bite never severed" than a run
            // without this arm. That is the arm being faithful, not the sweep
            // regressing: the posts it does sever are the same ones, at the
            // same sizes.
            // The first post this bite actually severs. Most of what the finder
            // calls a post is a wall, and a wall the bite does not cut through
            // proves nothing either way -- which is the same reason the sweep
            // below counts `untouched` separately.
            for (const Vec3 &foot : posts) {
                if (poolTried) break;
                const Vec3 at{foot.x, foot.y + 0.2f, foot.z};
                const int freeBefore = world_.debrisFree();
                world_.clearHangReport();
                world_.carveLevelToBody(physics_, at, kCutVox, simMs_, kArrowAbsorbM);
                const int dropped = world_.lastHangVox();
                if (dropped < kBigPieceVox) continue;
                poolTried = true;
                poolFree = freeBefore;
                poolVox = dropped;
                for (int i = 0; i < kDebrisInstances; ++i) {
                    Vec3 p{0, 0, 0};
                    if (!world_.debrisPose(i, &p, nullptr)) continue;
                    const float dx = p.x - at.x, dz = p.z - at.z;
                    if (dx * dx + dz * dz > 25.0f) continue;
                    const int v = world_.debrisVoxels(i);
                    if (v >= kBigPieceVox && v > poolBody) poolBody = v;
                }
                std::printf("     (%8.1f,%7.1f,%8.1f)  %4d voxels cut loose with %d slots "
                            "free -- %s\n",
                            double(at.x), double(at.y), double(at.z), dropped, freeBefore,
                            poolBody ? "a body took" : "CUT BUT NO BODY -- THE VOXELS VANISHED");
            }
            if (!poolTried)
                std::printf("     no post was severed, so this arm proves nothing\n");
            world_.clearDebris(physics_);
        }

        for (const Vec3 &foot : posts) {
            // 20 cm above the foot, which is where a swing or a burst lands
            // and is clear of the ground run under it.
            const Vec3 at{foot.x, foot.y + 0.2f, foot.z};
            world_.clearHangReport();
            int nOut = 0;
            Vec3 spoilAt{0, 0, 0};
            world_.carveLevelToBody(physics_, at, kCutVox, simMs_, kArrowAbsorbM, &nOut, &spoilAt);
            const int dropped = world_.lastHangVox();
            const int left = world_.levelHangingAbove(Vec3{at.x, at.y + 0.3f, at.z}, 20000);
            if (left > 0) {
                ++hanging;
                if (left > worst) {
                    worst = left;
                    worstAt[0] = double(at.x);
                    worstAt[1] = double(at.y);
                    worstAt[2] = double(at.z);
                }
                std::printf("  (%8.1f,%7.1f,%8.1f)  %5d voxels LEFT HANGING\n", double(at.x),
                            double(at.y), double(at.z), left);
            } else if (dropped > 0) {
                ++severed;
                // -- ...AND THEN WATCH IT LAND -----------------------------
                //
                // (user 2026-09-18: "when things become rigid bodies on
                //  nuketown, they fall endlessly and glitch out on the floor".)
                //
                // FREEING THE POST IS HALF THE RULE. "Nothing floats" is not
                // satisfied by a body that leaves the grid and then falls
                // through the map for ever -- that is the same voxel in a
                // different wrong place. Only the posts big enough to be worth
                // simulating are followed, because a one-voxel speck resting in
                // a bullet hole tells nothing about a 3.4 m pole.
                if (dropped >= kBigPieceVox) {
                    // The body the sweep just made: the fattest live slot near
                    // the cut. carveLevelToBody returns the CHIP's slot, not
                    // the hanger's, so it has to be found rather than asked for.
                    int slot = -1, best = 0;
                    for (int i = 0; i < kDebrisInstances; ++i) {
                        Vec3 p{0, 0, 0};
                        if (!world_.debrisPose(i, &p, nullptr)) continue;
                        const float dx = p.x - at.x, dz = p.z - at.z;
                        if (dx * dx + dz * dz > 25.0f) continue;
                        const int v = world_.debrisVoxels(i);
                        if (v > best) { best = v; slot = i; }
                    }
                    // NO SLOT IS NOT "NOTHING TO MEASURE" -- it is the
                    // failure. The voxels have already left the grid by the
                    // time this runs, so a piece with no body is a piece that
                    // was deleted, which is the one thing the RULE over
                    // kMinBodyVoxels forbids. This used to fall through the
                    // `if` below in silence and the test printed PASS.
                    if (slot < 0) {
                        ++lost;
                        std::printf("  (%8.1f,%7.1f,%8.1f)  %4d vox  CUT BUT NO BODY -- THE"
                                    " VOXELS JUST VANISHED\n",
                                    double(at.x), double(at.y), double(at.z), dropped);
                    }
                    if (slot >= 0) {
                        Vec3 p0{0, 0, 0};
                        world_.debrisPose(slot, &p0, nullptr);
                        const float dt2 = 1.0f / 60.0f;
                        for (int f = 0; f < kSettleFrames; ++f) {
                            physics_.step(dt2);
                            simMs_ += double(dt2) * 1000.0;
                            world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                                [&](float x, float z) {
                                                    return player_.surfaceAt(walkWorld(), x, z);
                                                });
                        }
                        Vec3 p1{0, 0, 0};
                        const bool alive = world_.debrisPose(slot, &p1, nullptr);
                        Vec3 lin{0, 0, 0}, ang{0, 0, 0};
                        physics_.velocityOf(world_.debrisBody(slot), &lin, &ang);
                        // -- MEASURED AT THE BODY'S FEET, NOT ITS ORIGIN --
                        //
                        // The first version of this compared the ORIGIN to the
                        // floor and reported every upright post as resting
                        // "+2.2 m over floor", which is true and says nothing:
                        // a 3.4 m post standing on its end has its origin 1.7 m
                        // up, and one lying down has it at 0.25 m. An origin
                        // height cannot tell those apart, and "did it land" is
                        // exactly the question it has to answer.
                        //
                        // The BOUNDS bottom is the same number whatever the
                        // post is doing: on the floor it is the floor.
                        Vec3 bl{0, 0, 0}, bh{0, 0, 0};
                        const bool haveB = physics_.boundsOf(world_.debrisBody(slot), &bl, &bh);
                        const float floorY = world_.levelFloorBelowM(p1.x, p0.y, p1.z);
                        const float above = (haveB ? bl.y : p1.y) - floorY;
                        const char *verdict = "settled";
                        if (!alive) { verdict = "GONE"; ++lost; }
                        else if (p1.y < World::levelOrigin().y - 5.0f) {
                            verdict = "FELL OUT OF THE MAP"; ++fellThrough;
                        } else if (lin.y < -kStillM) {
                            verdict = "STILL FALLING"; ++stillFalling;
                        } else if (above < -0.25f) {
                            verdict = "SUNK INTO THE FLOOR"; ++sunk;
                        } else {
                            ++settled;
                        }
                        std::printf("  (%8.1f,%7.1f,%8.1f)  %4d vox  y %7.2f -> %8.2f  "
                                    "vy %7.2f  feet %+.2f m vs floor  %s\n",
                                    double(at.x), double(at.y), double(at.z), dropped,
                                    double(p0.y), double(p1.y), double(lin.y), double(above),
                                    verdict);
                    }
                }
            } else {
                ++untouched;
            }
            // The pool has to be drained or the later posts cannot spawn and
            // the test measures kDebrisInstances -- same trap as runRipTest.
            world_.clearDebris(physics_);
        }
        // HOW MANY WERE REAL SEVERS IS PART OF THE RESULT. A run where the bite
        // never cut anything through would report zero hangers and mean
        // nothing; saying so is what makes the pass readable.
        std::printf("\n  %d posts cut: %d came down, %d left hanging, %d the bite never severed\n",
                    severed + hanging + untouched, severed, hanging, untouched);
        std::printf("  of the big ones: %d settled, %d STILL FALLING, %d SUNK, %d FELL OUT,"
                    " %d vanished\n", settled, stillFalling, sunk, fellThrough, lost);
        if (poolTried)
            std::printf("  with the pool full: %d voxels cut loose with %d slots free, "
                        "biggest body near it %d voxels  %s\n",
                        poolVox, poolFree, poolBody,
                        poolBody ? "-- it came down" : "-- IT VANISHED");
        if (hanging)
            std::printf("  worst: %d voxels at (%.1f, %.1f, %.1f)\n", worst, worstAt[0], worstAt[1],
                        worstAt[2]);
        if (!severed && !hanging)
            std::printf("\n  FAIL -- the bite severed nothing at all, so nothing was tested.\n");
        else
            std::printf("\n  %s\n",
                        hanging != 0
                            ? "FAIL -- a cut post is still hanging in the sky."
                            : (lost || (poolTried && !poolBody))
                                  ? "FAIL -- a post was cut out of the map and no body took."
                                  : (stillFalling || sunk || fellThrough)
                                  ? "FAIL -- a post came down and then fell through the map."
                                  : "PASS -- every post cut through came down and landed on it.");
        std::fflush(stdout);
    }

    void runFloatAudit() {
        std::printf("\n=== LEVEL FLOAT AUDIT ===\n");
        if (!world_.levelOn()) {
            std::printf("  the level is not open -- pass --level\n");
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const World::FloatAudit a = world_.auditLevelFloaters();
        std::printf("  %lld solid voxels: %lld held up by the foundation, %lld standing on "
                    "nothing\n",
                    a.solid, a.grounded, a.floating);
        std::printf("  %d floating pieces, largest %d voxels, tallest %d voxels (%.1f m)\n",
                    a.pieces, a.largest, a.tallest, double(a.tallest) * VOXEL_M);
        std::printf("  the biggest one sits at row %d with %s below it\n", a.largestBaseY,
                    a.largestGapY < 0 ? "nothing at all"
                    : a.largestGapY == 1
                        ? "solid ONE ROW under it -- it is resting on something the flood could not step across"
                        : "solid further down");
        if (a.largestGapY > 1)
            std::printf("           (%d rows, %.2f m of air)\n", a.largestGapY,
                        double(a.largestGapY) * VOXEL_M);
        std::printf("  %.2f%% of the map is unsupported  (%.0f ms)\n",
                    a.solid ? 100.0 * double(a.floating) / double(a.solid) : 0.0,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count());
        std::fflush(stdout);
    }

    void runFoodTest() {
        std::printf("\n=== FOOD TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("  kit: apple slot %d, orange slot %d, steak slot %d\n", appleTool_,
                    orangeTool_, steakTool_);
        if (appleTool_ < 0 || orangeTool_ < 0 || steakTool_ < 0) {
            std::printf("  FAIL -- a food did not reach the kit.\n");
            return;
        }
        // THE LOWEST ONE WITHIN A WALK, not the first one adopted. Most of a
        // crop hangs well above kFruitReachM and an arbitrary fruit is a true
        // report about the wrong fruit -- this is the one a player could
        // actually reach, which is what the hand pick is for. The high ones are
        // shootFruit's business.
        Vec3 f{0.0f, 0.0f, 0.0f};
        if (!world_.lowestFruitNear(player_.pos, 90.0f, &f)) {
            std::printf("  FAIL -- no fruit in the ring to pick.\n");
            return;
        }
        // Stand under it and look up at it. The aim is the exact bearing from
        // the eye to the fruit, so this tests the REACH rather than the aim.
        player_.placeOnGround(walkWorld(), f.x, f.z);
        pos_ = player_.eyePosition();
        const Vec3 d{f.x - pos_.x, f.y - pos_.y, f.z - pos_.z};
        const float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        yaw_ = atan2f(d.x, -d.z) * 180.0f / PI;
        pitch_ = asinf(clampf(d.y / maxf(dl, 1e-4f), -1.0f, 1.0f)) * 180.0f / PI;
        std::printf("  fruit at (%.1f, %.1f, %.1f), eye (%.1f, %.1f, %.1f), %.2f m away "
                    "(reach %.2f)\n",
                    f.x, f.y, f.z, pos_.x, pos_.y, pos_.z, dl, double(kFruitReachM));

        // ---- 1. the hand pick ---------------------------------------------------
        //
        // THE KIT IS WHAT IS CHECKED, not the drop field: a pick puts the fruit
        // in the hand now rather than on the floor. See pickFruit.
        const bool took = pickFruit();
        // IT FLIES IN NOW, so the hand is not set on the frame of the pick --
        // see Drops::grabFrom. kGrabSec is 360 ms; this drives the drops until
        // it lands, which is what the game does over the following third of a
        // second. Without it the test reads the hand before the fruit is in it
        // and reports the flight as a failure to pick.
        // NAMED fr, not f: the fruit position above is already `f` in this
        // scope and /WX turns the shadow into an error.
        for (int fr = 0; fr < 120 && selectOnArrive_ >= 0; ++fr) {
            drops_.update(1.0f / 60.0f, walkWorld(), player_.pos, player_.eyePosition());
            for (const int back : drops_.arrivedThisTick()) {
                held_.give(back);
                if (back == selectOnArrive_) {
                    held_.select(back);
                    held_.spendEatPress();
                    selectOnArrive_ = -1;
                }
            }
        }
        const int sel = held_.selected();
        std::printf("  pick: %s, hand now slot %d, carried %s\n",
                    took ? "took one" : "NOTHING IN REACH", sel,
                    held_.carrying() ? "yes" : "NO");
        if (took && sel != appleTool_ && sel != orangeTool_) {
            std::printf("  FAIL -- it was picked but it did not reach the hand.\n");
            return;
        }
        if (!took) {
            std::printf("  FAIL -- nothing was picked.\n");
            return;
        }

        // ---- 2. ...and it stays picked ------------------------------------
        //
        // The whole point of hiddenScatter_. Let the mesher answer first, or
        // this reads the one frame in which the mask is right.
        for (int i = 0; i < 120; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const bool again = pickFruit();
        std::printf("  after the re-mesh: %s\n",
                    again ? "IT GREW BACK -- WRONG" : "still gone, correct");

        // ---- 3. ...and the ones out of reach are shot down ------------------
        //
        // A SEPARATE ARM WITH ITS OWN WAY OF BEING WRONG. shootFruit probes
        // BACKWARDS down the shaft from where the round landed, because a fruit
        // is walkThrough and the impact is reported on the wood behind it -- an
        // easy thing to get the sign of and impossible to see in a frame.
        // Driven directly here rather than by firing: the rifle only exists
        // inside the level and aiming a bow at a 3 cm target headlessly tests
        // the aim, not the pickup.
        Vec3 f2{0.0f, 0.0f, 0.0f};
        bool shot = false;
        if (world_.lowestFruitNear(player_.pos, 90.0f, &f2)) {
            const Vec3 up{0.0f, 1.0f, 0.0f};
            // Where a round fired from below would have stopped: just past the
            // fruit, which is where the branch it hangs from is.
            const Vec3 hit{f2.x, f2.y + 0.3f, f2.z};
            const int d0 = drops_.count();
            shot = shootFruit(hit, up);
            std::printf("  shot: %s, drops %d -> %d\n", shot ? "knocked one down" : "MISSED",
                        d0, drops_.count());
        }

        // ---- 4. the bite ---------------------------------------------------
        held_.give(appleTool_);
        held_.select(appleTool_);
        const int stack0 = held_.tool(appleTool_).stack;
        const int m0 = held_.model();
        double t = simMs_;
        // ONE PRESS AND THEN LET GO -- which is the fix being tested (user
        // 2026-09-17: "the user should only have to press right click once, not
        // hold it down"). Holding it would pass either way; releasing on the
        // very next step is what fails if the bite still needs the button.
        // LET GO FIRST. The pick a few lines up spent this press on purpose --
        // see HeldItem::spendEatPress -- so a test that presses without ever
        // releasing is asking to eat with the same click that picked, which is
        // the bug being fixed. A player's finger comes up; so does this.
        held_.wantEat(false, t);
        held_.wantEat(true, t);           // ...and now the press
        const bool bit = held_.bitNow();  // ...and the cue the chew plays on
        int mMid = -1;
        bool ate = false;
        for (int i = 1; i <= 40 && !ate; ++i) {
            t += double(kEatMs) / 20.0;   // half a frame of the strip per step
            ate = held_.wantEat(false, t);   // released, and it still finishes
            if (i == 10) mMid = held_.model();
        }
        const int stack1 = held_.tool(appleTool_).stack;
        std::printf("  bite: pressed once and released, model %d -> %d (mid-bite), ate %s, "
                    "stack %d -> %d, chew %s\n", m0, mMid,
                    ate ? "yes" : "NO", stack0, stack1, bit ? "cued" : "NOT CUED");

        // ---- 5. PICKING WITH A FRUIT ALREADY IN HAND DOES NOT EAT IT -------
        //
        // (user: "if Im holding an apple/orange in hand, then right click to
        //  pick up another one from a tree, it eats the fruit.")
        //
        // WHY THIS NEEDS ITS OWN ARM RATHER THAN A CHECK BOLTED ONTO ARM 1.
        // Arm 1 picks with an EMPTY hand, and an empty hand cannot reproduce
        // this: wantEat falls straight out on holdingFood(), so the press does
        // no harm and the arrival spends it 360 ms later. The bug needs food in
        // the hand ON THE PRESS FRAME. Arm 1 also drives the drops to arrival
        // first, and the arrival path spends the press itself -- so anything
        // measured after that loop passes whatever pickFruit did.
        //
        // SO: food in hand, pick, and poll the bite in the SAME step with the
        // button still down -- which is the order the game runs them in, the
        // event handler and then processInput. No drops_.update between them.
        held_.give(appleTool_);
        held_.select(appleTool_);      // select cancels any bite -- a clean start
        const bool hadFood = held_.holdingFood();
        Vec3 f3{0.0f, 0.0f, 0.0f};
        bool armed = false, noBiteOnPick = false, tookSecond = false;
        if (hadFood && world_.lowestFruitNear(player_.pos, 90.0f, &f3)) {
            // STAND UNDER THIS ONE TOO. Arm 1 took the lowest fruit and arm 3
            // shot another down, so the next one up can easily be past
            // kFruitReachM from where arm 1 left the feet -- and an arm that
            // quietly does not run is the thing this whole test exists to
            // avoid. Same placeOnGround the first pick uses.
            player_.placeOnGround(walkWorld(), f3.x, f3.z);
            pos_ = player_.eyePosition();
            const Vec3 d3{f3.x - pos_.x, f3.y - pos_.y, f3.z - pos_.z};
            const float dl3 = sqrtf(d3.x * d3.x + d3.y * d3.y + d3.z * d3.z);
            yaw_ = atan2f(d3.x, -d3.z) * 180.0f / PI;
            pitch_ = asinf(clampf(d3.y / maxf(dl3, 1e-4f), -1.0f, 1.0f)) * 180.0f / PI;
            armed = dl3 <= kFruitReachM;
            if (armed) {
                tookSecond = pickFruit();          // the ButtonDown half
                held_.wantEat(true, simMs_);       // ...and processInput, same frame
                noBiteOnPick = !held_.eating();
                held_.bitNow();                    // swallow the cue either way
            }
        }
        std::printf("  second pick with food in hand: %s, bite %s\n",
                    !armed ? "NO FRUIT IN REACH -- not exercised"
                           : tookSecond ? "took one" : "NOTHING PICKED",
                    !armed ? "n/a" : noBiteOnPick ? "not opened -- correct"
                                                  : "OPENED -- it ate the one in hand");

        const bool pass = took && !again && shot && ate && bit && mMid != m0 && mMid >= 0 &&
                          stack1 < stack0 && (!armed || (tookSecond && noBiteOnPick));
        std::printf("\n  %s\n", pass ? "PASS" : "FAIL");
        std::fflush(stdout);
    }

    void runWheatTest() {
        std::printf("\n=== WHEAT TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood\n", pos_.x, pos_.y, pos_.z,
                    world_.terrain.woodName(pos_.x));
        std::printf("  kit: wheat slot %d, seeds slot %d\n", wheatTool_, seedsTool_);
        if (wheatTool_ < 0 || seedsTool_ < 0) {
            std::printf("  FAIL -- the drop models did not load.\n");
            return;
        }

        // ---- find a stand of wheat within arm's reach ----------------------
        //
        // WHEAT IS NOT EVERYWHERE. It is the tall band of the blade ramp, so
        // most columns have none -- this walks outward from the player and
        // takes the first one that has some, then stands next to it.
        // A PERIMETER WALK, NOT A FILLED SQUARE. The first cut of this scanned
        // every cell of every ring and was therefore cubic in the radius, which
        // is why it only reached nine metres -- and nine metres of pine wood
        // very often has no wheat in it at all. Walking the ring itself is
        // linear per ring, so forty metres costs less than nine did.
        TerrainMemo memo;
        int wi = 0, wj = 0, wy = 0;
        bool found = false;
        const int p0i = int(std::floor(pos_.x / VOXEL_M));
        const int p0j = int(std::floor(pos_.z / VOXEL_M));
        const auto tryCell = [&](int i, int j) {
            if (found) return;
            const int h = world_.terrain.heightVox(i, j, memo);
            const uint8_t top = world_.terrain.topMaterial(i, j, h, memo);
            const int sr = world_.terrain.strandRows(i, j, top);
            if (sr <= 0) return;
            if (!isWheat(world_.terrain.bladeMaterial(i, j, sr))) return;
            int lo = 0, hi = 0;
            VoxelTerrain::bladeSpan(h, sr, &lo, &hi);
            wi = i;
            wj = j;
            wy = (lo + hi) / 2;
            found = true;
        };
        for (int r = 1; r < 400 && !found; ++r) {
            for (int d = -r; d <= r && !found; ++d) {
                tryCell(p0i + d, p0j - r);
                tryCell(p0i + d, p0j + r);
                tryCell(p0i - r, p0j + d);
                tryCell(p0i + r, p0j + d);
            }
        }
        if (!found) {
            std::printf("  NO WHEAT WITHIN 40 m -- this run proved nothing. Try another "
                        "--spawn.\n");
            return;
        }
        // STREAM THE GROUND UNDER IT before standing there: the walk below and
        // the mow both read the edit layer through resident chunks, and a stand
        // of wheat forty metres off may be outside the ring the spawn built.
        for (int i = 0; i < 200; ++i) {
            world_.update(Vec3((float(wi) + 0.5f) * VOXEL_M, pos_.y,
                               (float(wj) + 0.5f) * VOXEL_M));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const Vec3 stalk((float(wi) + 0.5f) * VOXEL_M, (float(wy) + 0.5f) * VOXEL_M,
                         (float(wj) + 0.5f) * VOXEL_M);
        std::printf("  wheat at (%.1f, %.1f, %.1f), %.1f m off\n", stalk.x, stalk.y, stalk.z,
                    std::hypot(stalk.x - pos_.x, stalk.z - pos_.z));

        // ---- stand next to it and look at it -------------------------------
        player_.placeOnGround(walkWorld(), stalk.x + 0.9f, stalk.z + 0.9f);
        pos_ = player_.eyePosition();
        // DEGREES, AND -Z IS FORWARD. Camera::direction takes degrees and
        // builds (cos p sin y, sin p, -cos p cos y) -- so a yaw taken as
        // atan2(x, z) points the wrong way round the compass and a pitch in
        // radians is a rounding error away from level. Either one aims the
        // swing at the sky, which reads as "breakWheat found nothing" with
        // the wheat plainly standing in front of it.
        const Vec3 to = stalk - pos_;
        yaw_ = std::atan2(to.x, -to.z) * 57.29578f;
        pitch_ = std::atan2(to.y, std::hypot(to.x, to.z)) * 57.29578f;
        const Vec3 aim = forward();
        std::printf("  standing at (%.1f, %.1f, %.1f), aim (%.2f, %.2f, %.2f)\n", pos_.x,
                    pos_.y, pos_.z, aim.x, aim.y, aim.z);
        {
            const BladeHit pr = bladeRay(walkWorld(), pos_, aim, swingReachM(aim));
            std::printf("  blade ray         %s %s at %.2f m\n",
                        pr.hit ? "hit" : "MISSED",
                        pr.hit ? (isWheat(pr.material) ? "wheat" : "green grass") : "",
                        double(pr.dist));
        }

        const int before = bladeRowsAt(wi, wj);
        const int drops0 = drops_.count();

        // ---- FIRST, THE WRONG TOOL ------------------------------------------
        //
        // (user 2026-09-14: "make it where only the hoe can break the wheat.
        // if any other tool does it, play the antibreak sound.")
        //
        // TESTED BEFORE THE HOE IS EVEN PICKED UP, because the failure this
        // guards against is not that the axe does nothing -- it is that the
        // axe HARVESTS, which is what it did until now and which a test that
        // only swings the right tool would never see.
        //
        // THE SOUND IS NOT ASSERTED and cannot usefully be: there is no audio
        // device on this path. What is asserted is that the blow was SPENT --
        // breakWheat returning true is what stops the chain digging a crater in
        // the dirt behind the plant -- and that nothing was cut and nothing paid.
        int axeSlot = -1;
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Wood) axeSlot = i;
        bool axeSpent = false, axeQuiet = true;
        if (axeSlot >= 0) {
            held_.select(axeSlot);
            axeSpent = breakWheat();
            axeQuiet = (bladeRowsAt(wi, wj) == before) && (drops_.count() == drops0);
            std::printf("\n  -- the axe --\n");
            std::printf("  blow spent        %s\n",
                        axeSpent ? "yes -- it knocks and stops there"
                                 : "NO -- it would fall through and dig");
            std::printf("  took nothing      %s\n",
                        axeQuiet ? "correct" : "IT HARVESTED -- WRONG");
        }

        // ---- ...AND NOW THE HOE ---------------------------------------------
        int hoeSlot = -1;
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Earth) hoeSlot = i;
        std::printf("  hoe in slot %d\n", hoeSlot);
        if (hoeSlot < 0) {
            std::printf("  FAIL -- no hoe in the kit, so nothing can harvest.\n");
            return;
        }
        held_.select(hoeSlot);

        // ---- the swing ------------------------------------------------------
        const bool broke = breakWheat();
        const int after = bladeRowsAt(wi, wj);
        const int drops1 = drops_.count();
        std::printf("\n  -- the swing --\n");
        std::printf("  breakWheat        %s\n", broke ? "yes" : "NO -- it found nothing");
        std::printf("  blade rows here   %d -> %d\n", before, after);
        std::printf("  drops in flight   %d -> %d\n", drops0, drops1);

        // ---- and a second swing must pay NOTHING ---------------------------
        //
        // The half of this that is easy to get wrong: the mesher stops drawing
        // a cut column but the PROBE has its own copy of the rule, and if the
        // two disagree the plant is gone from the screen and still there to
        // swing at. That is a field that pays out for ever.
        const bool again = breakWheat();
        std::printf("  swung again       %s\n",
                    again ? "PAID OUT TWICE -- WRONG" : "nothing left, correct");

        // ---- ...AND NOWHERE ELSE IN THE SAME PATCH EITHER ------------
        //
        // THIS IS THE REPORT, and re-swinging at the same column does not
        // test it: "have one patch of wheat drop one seed and one wheat.
        // not multiple per patch." A tuft is a metre or so across and the
        // old bite was half that, so the way to farm one plant for ever was
        // to aim a step to the left. So this walks the tuft's own radius
        // and swings at every remaining stalk it can find in it.
        float trad = 0.0f, twant = 0.0f, tsx = stalk.x, tsz = stalk.z;
        const bool inTuft =
            world_.terrain.tuftAt(stalk.x, stalk.z, &trad, &twant, &tsx, &tsz);
        int extra = 0, stalksLeft = 0;
        if (inTuft) {
            const int rv = int(trad / VOXEL_M) + 1;
            const int c0 = int(std::floor(tsx / VOXEL_M)), d0 = int(std::floor(tsz / VOXEL_M));
            for (int dz = -rv; dz <= rv; ++dz)
                for (int dx = -rv; dx <= rv; ++dx) {
                    if (dx * dx + dz * dz > rv * rv) continue;
                    if (bladeRowsAt(c0 + dx, d0 + dz) <= 0) continue;
                    ++stalksLeft;
                    player_.placeOnGround(walkWorld(), (float(c0 + dx) + 0.5f) * VOXEL_M + 0.9f,
                                          (float(d0 + dz) + 0.5f) * VOXEL_M + 0.9f);
                    pos_ = player_.eyePosition();
                    const Vec3 t2(((float(c0 + dx) + 0.5f) * VOXEL_M) - pos_.x, 0.0f,
                                  ((float(d0 + dz) + 0.5f) * VOXEL_M) - pos_.z);
                    yaw_ = std::atan2(t2.x, -t2.z) * 57.29578f;
                    pitch_ = -35.0f;
                    held_.select(hoeSlot);   // the only thing that can pay out
                    if (breakWheat()) ++extra;
                }
        }
        std::printf("  tuft %.1f m across: %d stalks still standing in it, %d more payouts\n",
                    double(trad * 2.0f), stalksLeft, extra);
        // A STALK LEFT IS A FAILURE EVEN IF IT PAYS NOTHING. It was reported as
        // "one strand of wheat was left" -- the player sees the plant, not the
        // ledger, and a patch that is cut except for one stem reads as a bug
        // whether or not swinging at it again would hand out a second seed.
        if (stalksLeft) std::printf("      ^ A CUT PATCH MUST BE EMPTY -- WRONG\n");

        // ---- walk over them -------------------------------------------------
        std::printf("\n  -- absorbing --\n");
        int got = 0;
        float firstAt = -1.0f, lastAt = -1.0f;
        for (int f = 0; f < 60 * 12 && got < drops1 - drops0; ++f) {
            drops_.update(1.0f / 60.0f, walkWorld(), player_.pos, player_.eyePosition());
            // EVERY ARRIVAL ON THE FRAME, not the first -- see
            // Drops::arrivedThisTick. The old loop took one per tick, which
            // was a complete answer while only one item could be in flight
            // and is a silent leak now that a pile converges together.
            for (const int back : drops_.arrivedThisTick()) {
                const float at = float(f) / 60.0f;
                if (firstAt < 0.0f) firstAt = at;
                lastAt = at;
                std::printf("  picked up %s (slot %d) after %.2f s\n",
                            held_.tool(back).name, back, double(at));
                held_.give(back);
                ++got;
            }
        }
        // -- AND HOW FAR APART THEY LANDED, which is the whole of the ask
        //    (user 2026-09-14: "absorb multiple object at once, instead of
        //    one at a time in a line") -------------------------------------
        //
        // ONE FLIGHT IS kGrabSec, so two items absorbed one after the other
        // land a whole flight apart and two absorbed together land on the
        // same frame or within a step of it. The gap is the measurement; the
        // count never changed and never would have.
        std::printf("  first at %.2f s, last at %.2f s -- %.2f s apart\n",
                    double(firstAt), double(lastAt), double(lastAt - firstAt));
        std::printf("  carrying wheat    %s\n", held_.tool(wheatTool_).carried ? "yes" : "NO");
        std::printf("  carrying seeds    %s\n", held_.tool(seedsTool_).carried ? "yes" : "NO");

        // TOGETHER MEANS TOGETHER: the two drops leave the plant on the same
        // frame and are the same distance from the player, so if they are
        // absorbed concurrently they arrive within a few frames of each other.
        // A whole kGrabSec apart is the old one-at-a-time behaviour wearing a
        // passing count.
        const bool together = (lastAt - firstAt) < 0.5f * kGrabSec;
        const bool pass = broke && after == 0 && before > 0 && (drops1 - drops0) == 2 &&
                          !again && extra == 0 && stalksLeft == 0 && axeSpent && axeQuiet &&
                          together && got == 2 &&
                          held_.tool(wheatTool_).carried && held_.tool(seedsTool_).carried;
        std::printf("\n  %s\n", pass ? "PASS -- the wheat broke, paid one of each, and both "
                                        "were absorbed."
                                      : "FAIL -- see the lines above.");
    }

    // How many blade voxels are still standing on this column, as the PROBE
    // sees it -- which is the same answer the mesher draws. See mow().
    int bladeRowsAt(int i, int j) {
        TerrainProbe probe(&world_.terrain, &world_.editStore());
        TerrainMemo memo;
        const int h = world_.terrain.heightVox(i, j, memo);
        const uint8_t top = world_.terrain.topMaterial(i, j, h, memo);
        const int sr = world_.terrain.strandRows(i, j, top);
        if (sr <= 0) return 0;
        int lo = 0, hi = 0;
        VoxelTerrain::bladeSpan(h, sr, &lo, &hi);
        int n = 0;
        for (int y = lo; y <= hi; ++y) n += isBlade(probe.material(i, j, y)) ? 1 : 0;
        return n;
    }

    void runClipTest() {
        std::printf("\n=== CLIP TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        const bool birch = world_.terrain.birchAt(pos_.x);
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood, %04.1fh, sun %.1f deg (%s)\n",
                    pos_.x, pos_.y, pos_.z, birch ? "birch" : "pine",
                    double(opt_.timeOfDay * 24.0f), double(world_.sky.elevationDeg()),
                    isNight() ? "night" : "day");

        // THE WOOD AROUND THE SPAWN, gathered once and wide. This is the same
        // list the animals themselves are given (see the perch gather in
        // update) -- if the check gathered its own, the two could disagree and
        // a pass would mean nothing.
        world_.collidersNear(pos_, kBirdKeepM, &perches_);
        int rocks = 0;
        for (const Solid &sd : perches_) rocks += sd.standable ? 1 : 0;
        std::printf("  %d solids in reach -- %d standable (rocks), %d not (trunks)\n",
                    int(perches_.size()), rocks, int(perches_.size()) - rocks);

        // ---- settle, then watch --------------------------------------------
        //
        // A POPULATION REPORTS ITS SPAWN STATE IF YOU ONLY TICK IT ONCE, which
        // is the lesson the bunny report and the bee report each had to learn
        // separately. Thirty seconds to fill and settle, then sixty watched --
        // long enough for a fly bunch to cross a clearing at 1 m/s and for
        // every bee to run two full errands.
        warmLife(pos_, 30);

        // -- WHAT THE THREE NUMBERS MEAN, because "inside" alone said too
        //    little to act on.
        //
        //    touch   the body's rim met a model -- a wing clipping a corner
        //    centre  the animal's own position is in the solid, which is the
        //            report ("caught flying INSIDE a big rock") and is the one
        //            that matters
        //    deep    how far it would have to rise to get out, walked upward
        //            in tenths -- a centimetre is a surface, a metre is a
        //            creature living in the stone
        //
        // The first cut of this printed a fourth number that was nonsense: the
        // radius at which solidsTouch stopped firing, walked outward to six
        // metres. That probes the RIM of a circle, so it reports whether there
        // is a model within six metres of the animal, which there usually is in
        // a wood. It read 6.00 m for every single hit, which is what a metric
        // that is measuring nothing looks like.
        struct Worst {
            int touch = 0, centre = 0, wet = 0;
            float deep = 0.0f;
            Vec3 at{0, 0, 0};
            bool rock = false;
        };
        std::map<std::string, Worst> bad;
        std::map<std::string, long> seen;
        // -- HOW FAR EACH SPECIES ACTUALLY WENT -----------------------------
        //
        // THE OTHER HALF OF THE ANSWER, and without it a pass means nothing.
        // Every fix in this change is a REFUSAL -- do not take that step, do
        // not hold that line -- and the way a refusal fails is by refusing
        // everything. A wood where no animal is inside a rock because no
        // animal has moved since it was born passes the check above perfectly.
        //
        // Matched by INDEX and only while the count is steady, which is as much
        // identity as livePoints has: a slot that dies renumbers the rest. Any
        // jump over two metres in a frame is a birth or a recycle rather than a
        // step, and is dropped.
        std::map<std::string, double> went;
        std::map<std::string, long> wentN;
        // -- AND HOW HIGH IT FLIES, which is the other thing a report of
        //    "the flys are too low" or "the ducks sit too deep" needs a
        //    number for. Height above the local ground, averaged.
        std::map<std::string, double> agl;
        std::vector<LifeAt> prev;
        std::vector<LifeAt> who;
        long samples = 0, inTree = 0;

        const auto groundAt = [this](float x, float z) {
            return float(world_.terrain.heightVox(int(std::floor(x / VOXEL_M)),
                                                  int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
        };
        // -- THE BROAD PHASE, WHICH THE ANIMALS DO NOT NEED AND THIS DOES --
        //
        // A creature is handed a list gathered at 2.8 m and this one is
        // gathered at 115, because the check has to see every animal at once
        // and they are spread over the whole streaming ring. Walking two
        // thousand models per creature per frame through the voxel test is a
        // minute of arithmetic to answer a question about a metre of space.
        //
        // REJECTED ON THE MODEL'S OWN EXTENT, not on the collider's. The
        // ellipse is measured over the first two metres of a model and a domed
        // boulder is WIDER higher up (see measureCollider), so a reject built
        // from hx/hz would quietly stop testing the overhangs -- which are
        // exactly the shapes this is here to catch.
        const auto insideAny = [this](const Vec3 &p, float r, const Solid **which) {
            for (const Solid &sd : perches_) {
                if (p.y > sd.top) continue;
                float reach = (sd.hx > sd.hz ? sd.hx : sd.hz);
                if (sd.msx > 0 && sd.msz > 0) {
                    const float ex = float(sd.msx) * VOXEL_M, ez = float(sd.msz) * VOXEL_M;
                    const float diag = std::sqrt(ex * ex + ez * ez) * 0.5f;
                    if (diag > reach) reach = diag;
                }
                reach += r + 0.5f;
                const float dx = p.x - sd.cx, dz = p.z - sd.cz;
                if (dx * dx + dz * dz > reach * reach) continue;
                if (solidsTouch(&sd, 1, p.x, p.y, p.z, r, VOXEL_M)) {
                    if (which) *which = &sd;
                    return true;
                }
            }
            return false;
        };

        const float dt = 1.0f / 60.0f;
        for (int frame = 0; frame < 30 * 60; ++frame) {
            birds_.update(dt, perches_, pos_);
            lake_.update(dt, world_.terrain, pos_);
            flock2_.update(dt, pos_, groundAt, &perches_);
            bunnies_.update(dt, pos_, groundAt,
                            [this](float x, float z) { return wetColumnAt(x, z); }, perches_,
                            [this](float x) { return world_.terrain.woodBit(x); },
                            // ...AND NOT ON THE BEACH -- see Bunnies::blocked.
                            [this](float x, float z) { return sandAt(x, z); });
            bees_.update(dt, pos_, hivesNear_, bloomsNear_, &perches_);
            flock_.update(dt, world_, pos_);
            lake_.bankSpots(uint32_t(frame), 8, &banksNear_);
            critters_.update(dt, pos_, groundAt,
                             [this](float x, float z) { return wetColumnAt(x, z); },
                             [this](float x) { return world_.terrain.woodBit(x); }, banksNear_,
                             perches_, isNight(), Vec3(0.0f, 0.0f, 0.0f),
                             [this](float x, float z) { return waterTopAt(x, z); },
                             [this](float x, float z) { return sandAt(x, z); });

            who.clear();
            critters_.livePoints(&who);
            bees_.livePoints(&who);
            flock_.livePoints(&who);
            bunnies_.livePoints(&who);
            flock2_.livePoints(&who);
            birds_.livePoints(&who);
            lake_.livePoints(&who);

            if (prev.size() == who.size())
                for (size_t q = 0; q < who.size(); ++q) {
                    if (prev[q].what != who[q].what) continue;
                    const float dx = who[q].p.x - prev[q].p.x, dz = who[q].p.z - prev[q].p.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 > 4.0f) continue;
                    went[who[q].what] += double(std::sqrt(d2));
                    ++wentN[who[q].what];
                }
            prev = who;

            for (const LifeAt &a : who) {
                ++samples;
                ++seen[a.what];
                agl[a.what] += double(a.p.y - groundAt(a.p.x, a.p.z));
                // -- IS IT UNDER THE LAKE ---------------------------------
                //
                // The second invariant, and it exists because of a report the
                // first one could never have caught: "a lady bug was caught
                // swimming in water". Nothing was inside a solid -- a lake is
                // not a solid -- and the animal was a foot under the surface
                // holding exactly the altitude it had been told to hold.
                if (!a.inWater) {
                    const float top = waterTopAt(a.p.x, a.p.z);
                    if (a.p.y < top) ++bad[a.what].wet;
                }
                if (a.inTree) { ++inTree; continue; }
                const Solid *hit = nullptr;
                if (!insideAny(a.p, a.r, &hit)) continue;
                Worst &w = bad[a.what];
                ++w.touch;
                // -- THE FIRST ONE OF EACH KIND, IN FULL ---------------------
                //
                // A count says a rule is broken and says nothing about which
                // rule. One line naming the model, its footprint, its base and
                // its top against the creature's own position is the whole
                // diagnosis, and the first offence is as good as any: these
                // failures come in runs of one animal over many frames, so the
                // hundredth is the same animal as the first.
                if (w.touch == 1 && hit) {
                    std::printf("    first %-10s at (%.1f, %.1f, %.1f) r %.2f -- %s "
                                "centre (%.1f, %.1f) h (%.1f, %.1f) base %.1f top %.1f "
                                "model %dx%d vol %s\n",
                                a.what, a.p.x, a.p.y, a.p.z, double(a.r),
                                hit->standable ? "ROCK" : "tree", hit->cx, hit->cz, hit->hx,
                                hit->hz, hit->baseY, hit->top, int(hit->msx), int(hit->msz),
                                hit->vol ? "yes" : "NO");
                }
                if (!insideAny(a.p, 0.0f, nullptr)) continue;   // a graze, not a burial
                ++w.centre;
                // HOW FAR IN, which is the number that says whether this is a
                // wing clipping a corner or a swarm living in a boulder. Walked
                // outward rather than solved: the shape is a voxel grid and has
                // no inside-distance to ask for.
                // STRAIGHT UP UNTIL IT IS OUT, which is a distance the shape
                // can actually answer: every model in this world has air over
                // it eventually, and the climb crosses exactly the stone that
                // is on top of the animal.
                float d = 0.0f;
                for (float up = 0.1f; up <= 8.0f; up += 0.1f) {
                    if (!insideAny(Vec3(a.p.x, a.p.y + up, a.p.z), 0.0f, nullptr)) break;
                    d = up;
                }
                if (d >= w.deep) {
                    w.deep = d;
                    w.at = a.p;
                    w.rock = hit && hit->standable;
                }
            }
        }

        // ---- the report ----------------------------------------------------
        std::printf("\n  -- 30 s watched, %ld creature-frames --\n", samples);
        std::printf("  %-11s %9s %6s %6s %6s %7s %7s   %s\n", "species", "frames", "touch",
                    "centre", "wet", "m/s", "agl", "deepest");
        long hitTotal = 0;
        for (const auto &kv : seen) {
            const auto it = bad.find(kv.first);
            const long wn = wentN.count(kv.first) ? wentN[kv.first] : 0;
            const double mps = wn ? went[kv.first] / (double(wn) * double(dt)) : 0.0;
            const double up = kv.second ? agl[kv.first] / double(kv.second) : 0.0;
            if (it == bad.end()) {
                std::printf("  %-11s %9ld %6d %6d %6d %7.2f %7.2f   -\n", kv.first.c_str(),
                            kv.second, 0, 0, 0, mps, up);
                continue;
            }
            hitTotal += it->second.centre + it->second.wet;
            std::printf("  %-11s %9ld %6d %6d %6d %7.2f %7.2f   %.1f m under a %s at "
                        "(%.0f, %.0f, %.0f)\n",
                        kv.first.c_str(), kv.second, it->second.touch, it->second.centre,
                        it->second.wet, mps, up, double(it->second.deep),
                        it->second.rock ? "rock" : "tree", it->second.at.x, it->second.at.y,
                        it->second.at.z);
        }
        std::printf("\n  %ld perched-songbird frames in a crown (expected, not counted)\n",
                    inTree);
        if (perches_.empty())
            std::printf("  NO SOLIDS IN REACH -- this run proved nothing. Move the spawn.\n");
        else if (hitTotal == 0)
            std::printf("  PASS -- nothing was ever inside a tree or a rock, or under the "
                        "lake.\n");
        else
            std::printf("  FAIL -- %ld creature-frames with the animal ITSELF in a solid or "
                        "under water.\n", hitTotal);
    }

    void runLocateTest() {
        std::printf("\n=== LOCATE TEST ===\n");
        // The spawn first -- runFellTest's own note: this runs before the
        // player has been placed, so streaming round pos would stream the
        // origin and survey a wood nobody is standing in.
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        // THE TERRAIN'S OWN WOOD BIT, NOT "IS IT BIRCH". This read `birchAt`,
        // so an OAK spawn reported itself as pine and every row was then judged
        // against the wrong wood -- which is what hid four bad gates in the
        // table for as long as it did. See LifeName::woods.
        const uint8_t here = world_.terrain.woodBit(pos_.x);
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood\n", pos_.x, pos_.y, pos_.z,
                    world_.terrain.woodName(pos_.x));

        warmLife(pos_, 30);

        // ---- the survey ---------------------------------------------------
        std::printf("\n  -- what /locate finds from here --\n");
        // "lives" IS A SET AND NEEDS THE ROOM TO SAY SO -- bunnies.h's
        // woodsName prints "birch and oak", which is the whole point of the
        // column and does not fit in six characters.
        std::printf("  %-11s %-13s %8s   %s\n", "name", "lives", "distance", "at");
        int found = 0, expectedMisses = 0, wrong = 0;
        for (const LifeName &ln : lifeNames()) {
            Vec3 at{0, 0, 0};
            const char *lives = ln.water ? "water" : woodsName(ln.woods);
            if (nearestLife(ln.life, &at)) {
                const float d = std::hypot(at.x - pos_.x, at.z - pos_.z);
                std::printf("  %-11s %-13s %6.0f m   (%.0f, %.0f, %.0f)\n", ln.name, lives, d,
                            at.x, at.y, at.z);
                ++found;
                // A row that finds an animal standing in a wood it does not
                // list means the gate in the table disagrees with the gate in
                // the engine -- and it caught four of them the day it learned
                // to ask with the terrain's own bits. See LifeName::woods.
                //
                // ASKED AT THE ANIMAL, NOT AT THE PLAYER, and that is the
                // whole difference between a report and a false one. A band is
                // 800 m and the streaming disc is 300, so standing anywhere
                // near a boundary means the ring spans TWO woods: from a pine
                // spawn nine metres off the birch line this flagged the mouse,
                // the grass snake and a beehive, and all three were correctly
                // in the BIRCH a few dozen metres away. The player's wood is
                // the right question for a MISS, below, and the wrong one for
                // a find.
                if (!ln.water && !(ln.woods & world_.terrain.woodBit(at.x))) {
                    std::printf("      ^ TABLE WRONG: one is standing in the %s wood, which "
                                "its row says it does not live in\n",
                                world_.terrain.woodName(at.x));
                    ++wrong;
                }
                continue;
            }
            // Nothing found. Expected when the row is gated elsewhere -- by
            // wood, by water, or by the CLOCK.
            const bool asleep = ln.night && !isNight();
            bool elsewhere = ln.water ? true : !(ln.woods & here);
            // WHY IT IS EXCUSED, IN ITS OWN WORDS. Every exemption below used
            // to print "not this wood", which is true of the first one and a
            // lie about the rest: a bee is excused because there is no HIVE,
            // a songbird because there is no TREE tall enough, a crop because
            // it is sparse. A line that gives the wrong reason for a real
            // miss is worse than no line -- it sends the next reader to the
            // band table to fix something that is not there.
            const char *why = "(not this wood -- /locate will travel)";
            // -- A BEE IS GATED ON A LANDMARK, NOT ON A WOOD ---------------
            //
            // kBeeHiveM is 40 m: a bee exists because a HIVE is within forty
            // metres of the player, and hives hang in one birch in a hundred.
            // So "no bee here" is the ordinary state of most of the birch wood
            // and finding one is luck -- `--birch` passed this row and the
            // banded world failed it from a spawn two hundred metres away,
            // which is the same world and the same code.
            //
            // THE SAME SHAPE AS THE FROG, which the table already flags with
            // `water`: the row is gated on something being NEAR, and /locate
            // travels to it on a miss. warmLife has already gathered the hives
            // this query would use, so the honest test is whether there was
            // one to find at all.
            if (ln.life == Life::Bee && hivesNear_.empty()) {
                elsewhere = true;
                why = "(no hive within 40 m -- /locate will travel)";
            }
            // -- AND A HIVE IS THE SPARSE LANDMARK THE BEE IS GATED ON -------
            //
            // The bee beside it has had this excuse since the day it was
            // written and the hive itself never did, because until now the row
            // said birch-only and every oak spawn excused it as "not this
            // wood". Widening it to kWoodBroad -- which is what hangHive
            // actually does -- took that excuse away and left the row with
            // none at all, so `--oak` reported WRONG for a hive being where
            // hives usually are not. hangHive bears on a small fraction of
            // trees; a 260 m disc holding none is ordinary.
            //
            // AN OPEN QUESTION IT IS DELIBERATELY NOT HIDING: the banded world
            // finds hives in the oak (one at 300 m, measured) and this `--oak`
            // run found none anywhere. That may be hangHive's `treeIndex <
            // birchBase` gate behaving differently when only the oak models
            // are loaded. The reason string says so rather than printing a
            // reassuring "-".
            if (ln.life == Life::Hive) {
                elsewhere = true;
                why = "(none in range -- hives are sparse; if a PINNED wood never has "
                      "any, look at hangHive's birchBase gate)";
            }
            // -- AND A SONGBIRD IS GATED ON A TREE TALL ENOUGH TO SIT IN -----
            //
            // Exactly the same shape, and the roaming spawn is what finally
            // showed it: the picker now opens in whatever wood it rolled
            // rather than always the one clearing, and a thin stretch of oak
            // reported "NONE, AND IT SHOULD BE HERE" on a world with nothing
            // wrong with it. `all woods` says where a songbird MAY live, not
            // that any given hundred metres of it holds a perch.
            //
            // MEASURED THE WAY THE BIRDS MEASURE IT, off the same list they
            // are handed: a trunk (not standable), at least kBirdMinTreeM
            // tall, inside kBirdPlaceM -- birds.h's own three tests. A count
            // of nearby SOLIDS would not do; most of them are rocks and
            // saplings, and a bird cannot perch on either.
            if (ln.life == Life::Songbird) {
                int perchable = 0;
                for (const Solid &s : perches_) {
                    if (s.standable) continue;
                    if (s.top - s.baseY < kBirdMinTreeM) continue;
                    if (std::hypot(s.cx - pos_.x, s.cz - pos_.z) > kBirdPlaceM) continue;
                    ++perchable;
                }
                if (perchable == 0) {
                    elsewhere = true;
                    why = "(no tree tall enough within 42 m -- /locate will travel)";
                }
            }
            // -- AND A CROP IS SPARSE, WHICH IS NOT THE SAME AS MISSING ------
            //
            // hangFruit bears on 15% of the oaks tall enough to carry one and
            // gives each of those ONE species on a 50/50 roll, so a stretch of
            // oak with apples in it and no orange inside 260 m is a wood, not
            // a defect -- measured, the nearest apple was 222 m out and there
            // was no orange at all. "The oak holds a crop" is a real claim and
            // it is still checked, but per-WOOD rather than per-fruit, down in
            // "the crop", which is the block that can ask it properly.
            if (ln.fruit) {
                elsewhere = true;
                why = "(none of this one in range -- the crop is checked below)";
            }
            // -- ...OR NOT THERE YET, WHICH IS NOT THE SAME AS NOT THERE -----
            //
            // A POPULATION FILLS IN OVER TIME and some of them are slower than
            // others: a bee exists because a HIVE does (see World::decorNear),
            // hives hang in one birch in a hundred, and in the BANDED world
            // only a third of the ring is birch -- so the nearest hive can be
            // hundreds of metres out and the swarm has not been born by the
            // time this survey runs. Measured: --birch finds a bee at 35 m and
            // the same wood inside the bands finds none, while the flyer band
            // at the end of this very test reports ten bees drawn.
            //
            // So a miss gets a second chance with more time on the clock before
            // it is called WRONG. A test that reports a failure on a healthy
            // world is a test that stops being read.
            //
            // A CROP IS NOT A POPULATION, so it gets no second chance and
            // wants none. warmLife ticks the flocks and the marchers and
            // streams nothing; a fruit is decor that was either meshed with
            // its chunk or does not exist, so another sixty seconds of
            // simulation cannot change the answer -- it can only spend a
            // minute of the run arriving at the same one.
            bool late = false;
            if (!asleep && !elsewhere && !ln.fruit) {
                warmLife(pos_, 60);
                Vec3 at2{0, 0, 0};
                late = nearestLife(ln.life, &at2);
                if (late) {
                    const float d2 = std::hypot(at2.x - pos_.x, at2.z - pos_.z);
                    std::printf("  %-11s %-13s %6.0f m   (%.0f, %.0f, %.0f)  -- after another "
                                "60 ticks\n",
                                ln.name, lives, d2, at2.x, at2.y, at2.z);
                    ++found;
                    continue;
                }
            }
            std::printf("  %-11s %-13s %8s   %s\n", ln.name, lives, "-",
                        asleep      ? "(after dark only -- try --time 23)"
                        : elsewhere ? why
                                    : "NONE, AND IT SHOULD BE HERE");
            if (asleep || elsewhere) ++expectedMisses;
            else ++wrong;
        }
        std::printf("  %d found, %d away in another wood, %d WRONG\n", found, expectedMisses,
                    wrong);

        // ---- the six fish are six different fish ---------------------------
        //
        // The census counts by the species field; the table reaches them by
        // the same integer. If a row has the wrong one, two names return the
        // same animal and a name with fish in the lake returns nothing.
        {
            int byFish[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int pads = 0, flies = 0;
            lake_.census(byFish, &pads, &flies);
            std::printf("\n  -- the lake's own census, against the table --\n");
            static const char *kFish[6] = {"salmon", "bass", "koi", "minnow", "catfish",
                                           "bluegill"};
            for (int s = 0; s < 6; ++s) {
                Vec3 at{0, 0, 0};
                float d = 0.0f;
                const bool got = lake_.nearestFish(pos_, s, &at, &d);
                std::printf("  species %d  %-9s %2d alive   /locate %-9s %s\n", s, kFish[s],
                            byFish[s], kFish[s],
                            got ? "finds one" : (byFish[s] ? "FINDS NOTHING -- WRONG" : "none"));
                if (!got && byFish[s]) ++wrong;
            }
            std::printf("  %d lily pads, %d dragonflies, %d ducks\n", pads, flies,
                        lake_.ducksLiving(true));
        }


        // ---- the THREE WOODS, which are places rather than animals --------
        //
        // (user 2026-09-16: "give me a /locate oak".)
        //
        // THIS HALF EXISTED AND WAS NEVER CHECKED, and it had just broken.
        // nearestBandX had the period written out as `2.0f * kBandW` -- right
        // for two woods, silently wrong for three -- so /locate pine would have
        // walked to a multiple of 1600 m while the pine band repeats every
        // 2400. You would have arrived among the wrong trees with a confident
        // reply saying otherwise, which is the same failure the animal half of
        // this test exists to catch.
        //
        // ASKED OF THE WORLD IT LANDS IN, not of the number it returns: teleport,
        // then ask the terrain which wood is actually underfoot.
        std::printf("\n  -- the woods --\n");
        {
            const Vec3 was = pos_;
            // A PINNED WORLD HAS ONE WOOD AND THAT IS NOT A FAILURE. --pine,
            // --birch, --oak and --acadia set `forced`, and there is no band to
            // walk to -- so this arm asks the two questions that world can
            // actually answer rather than the three a banded one can.
            //
            // THE WOOD IT IS PINNED TO MUST STILL TELEPORT (user 2026-09-18:
            // "make sure /locate birch still works teleporting"). It is the
            // ordinary state of the Acadia island, not a debugging flag, and a
            // player with ten kilometres of forest around them asking to be
            // taken to it is asking for something possible. The OTHER two are
            // the refusal, and that is still the right answer for them.
            if (world_.terrain.forced) {
                const char *pinned = world_.terrain.woodName(pos_.x);
                for (const BiomeName &bn : biomeNames()) {
                    const Vec3 from = pos_;
                    const std::string reply = runCommand(std::string("/locate ") + bn.name);
                    const bool mine = std::string(pinned) == bn.name;
                    const bool moved = std::fabs(pos_.x - from.x) > 0.5f ||
                                       std::fabs(pos_.z - from.z) > 0.5f;
                    // The refusal names the flag that would put that wood in
                    // reach -- see the reply itself for why "pinned" is not the
                    // word to look for any more.
                    const bool says = reply.find("--all-woods") != std::string::npos;
                    const bool right = mine ? (moved && !says) : (says && !moved);
                    std::printf("  /locate %-6s in a %s world -> %-42s %s\n", bn.name, pinned,
                                reply.c_str(),
                                right ? (mine ? "teleported" : "says so, which is right")
                                      : (mine ? "DID NOT TELEPORT -- WRONG"
                                              : "SHOULD HAVE REFUSED -- WRONG"));
                    if (!right) ++wrong;
                    // AND IT HAS TO LAND ON THE DATA. A band period is 2400 m
                    // and a DEM window is finite, so a jump can end up outside
                    // it, on the flat ground held at the border sample -- a
                    // legal teleport onto a featureless plain.
                    if (mine && moved)
                        std::printf("      landed at (%.0f, %.0f, %.0f), the %s wood\n", pos_.x,
                                    pos_.y, pos_.z, world_.terrain.woodName(pos_.x));
                }
            } else
            for (const BiomeName &bn : biomeNames()) {
                const std::string reply = runCommand(std::string("/locate ") + bn.name);
                const char *landed = world_.terrain.woodName(pos_.x);
                const bool right = std::string(landed) == bn.name;
                std::printf("  /locate %-6s -> %8.0f  lands in the %-5s wood  %s\n", bn.name,
                            double(pos_.x), landed,
                            right ? "correct" : "THE WRONG WOOD -- WRONG");
                if (!right) ++wrong;
            }
            // ...and its ALIAS reaches the same row, which is the other way a
            // table like this rots: a name that answers and an alias that does
            // not look identical until someone types the alias.
            for (const BiomeName &bn : biomeNames()) {
                const std::string reply = runCommand(std::string("/locate ") + bn.alias);
                const bool known = reply.find("nothing called") == std::string::npos;
                if (!known) {
                    std::printf("  alias '%s' is not recognised -- WRONG\n", bn.alias);
                    ++wrong;
                }
            }
            teleportTo(was.x, was.z);
        }

        // ---- THE PLACES, AND WHETHER /locate CAN REACH THEM ----------------
        //
        // The summits and lakes come out of the loaded .vbdem/.vbcov, so there
        // is no table to check them against -- poi_test does that offline,
        // against the data. What only the engine can answer is the part the
        // player actually meets: does every name the menu prints resolve, does
        // Tab complete it, and does the arrival put you somewhere you can
        // stand. A lake's entry is a point ON the water, so that last one is
        // not rhetorical: before this, /locate granby stood the player on the
        // bed of a five-kilometre reservoir.
        {
            std::printf("\n  -- the places in the data --\n");
            if (!poi_.ok()) {
                std::printf("  none -- this world has no elevation data loaded\n");
            } else {
                int unreachable = 0;
                for (const PoiIndex::Poi &pl : poi_.all())
                    if (!poi_.find(pl.name)) {
                        std::printf("  '%s' is printed but cannot be reached -- WRONG\n",
                                    pl.name.c_str());
                        ++unreachable;
                    }
                wrong += unreachable;
                std::printf("  %d places, %d named, %d unreachable\n",
                            int(poi_.all().size()), poi_.namedCount(), unreachable);

                // TAB, WITHOUT A KEYBOARD. completions() is what the callback
                // calls, so exercising it here covers everything about the
                // feature except ImGui delivering the keystroke.
                struct Probe { const char *typed; bool wantHit; };
                const Probe kProbe[] = {
                    {"/loc", true}, {"loc", true}, {"/LOC", true},
                    {"/locate ", true}, {"/locate wat", true}, {"/locate zzz", false},
                };
                for (const Probe &pr : kProbe) {
                    const std::vector<std::string> hits = completions(pr.typed);
                    const bool got = !hits.empty();
                    std::printf("  tab after \"%-12s\" -> %2d match%s%s%s\n", pr.typed,
                                int(hits.size()), hits.size() == 1 ? "" : "es",
                                got ? ", best '" : "", got ? (hits[0] + "'").c_str() : "");
                    if (got != pr.wantHit) {
                        std::printf("      ^ WRONG -- expected %s\n",
                                    pr.wantHit ? "a match" : "nothing");
                        ++wrong;
                    }
                }
                // The first place's name must be offered once its stem is typed.
                {
                    const std::string nm = poi_.all()[0].name;
                    const std::vector<std::string> hits =
                        completions("/locate " + nm.substr(0, 2));
                    bool offered = false;
                    for (const std::string &hnm : hits) offered |= (hnm == nm);
                    std::printf("  tab after \"/locate %s\" offers '%s': %s\n",
                                nm.substr(0, 2).c_str(), nm.c_str(),
                                offered ? "yes" : "NO -- WRONG");
                    if (!offered) ++wrong;
                }

                // THE ARRIVAL, on the two that are hardest to get right: the
                // biggest lake, whose shore is further than standNear's old
                // 400 m reach, and the highest summit.
                const PoiIndex::Poi *big = nullptr, *high = nullptr;
                for (const PoiIndex::Poi &pl : poi_.all()) {
                    if (pl.lake && (!big || pl.size > big->size)) big = &pl;
                    if (!pl.lake && (!high || pl.m > high->m)) high = &pl;
                }
                const Vec3 wasAt = pos_;
                for (int k = 0; k < 2; ++k) {
                    const PoiIndex::Poi *pl = k ? high : big;
                    if (!pl) continue;
                    const float tx = pl->x, tz = pl->z;
                    const std::string nm = pl->name;
                    const std::string reply = runCommand("/locate " + nm);
                    // Streamed before it is asked about, for runLocateTest's
                    // own reason: collidersNear only sees RESIDENT chunks.
                    for (int i = 0; i < 200; ++i) {
                        world_.update(player_.pos);
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    const bool wet = wetColumnAt(pos_.x, pos_.z);
                    const float off = std::hypot(tx - pos_.x, tz - pos_.z);
                    std::printf("  /locate %-10s -> \"%s\"\n", nm.c_str(), reply.c_str());
                    std::printf("      stood (%.0f, %.0f, %.0f)  %.0f m from the "
                                "%s  in water: %s\n",
                                pos_.x, pos_.y, pos_.z, off, k ? "summit" : "lake's centre",
                                wet ? "YES -- FAIL" : "no");
                    if (wet) ++wrong;
                }
                teleportTo(wasAt.x, wasAt.z);
            }
        }

        // ---- and where it puts you -----------------------------------------
        std::printf("\n  -- the arrival --\n");
        for (int pass = 0; pass < 2; ++pass) {
            // Pass 0 takes the first LAND row that is here, pass 1 the first
            // WATER row: the two halves of standNear, and the water half is
            // the one that has a lake bed to avoid.
            const LifeName *pick = nullptr;
            Vec3 at{0, 0, 0};
            for (const LifeName &ln : lifeNames()) {
                if (ln.water != (pass == 1)) continue;
                if (!nearestLife(ln.life, &at)) continue;
                pick = &ln;
                break;
            }
            if (!pick) {
                std::printf("  no %s life in range to try -- /locate would travel\n",
                            pass ? "water" : "land");
                continue;
            }
            const Vec3 target = at;
            const std::string reply = runCommand(std::string("/locate ") + pick->name);
            // THE GROUND YOU LANDED ON HAS TO BE STREAMED BEFORE IT CAN BE
            // ASKED ABOUT. collidersNear only sees RESIDENT chunks, so a trunk
            // check run on the frame of the teleport reads an empty list and
            // reports "no trunk" whatever is standing there -- a check that
            // always passes, which is worse than no check. The terrain and
            // water questions are generated rather than resident and do not
            // care, but they may as well wait with it.
            for (int i = 0; i < 200; ++i) {
                world_.update(player_.pos);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const float off = std::hypot(target.x - pos_.x, target.z - pos_.z);
            const bool wet = wetColumnAt(pos_.x, pos_.z);
            const bool tree = trunkAt(pos_.x, pos_.z);
            std::printf("  /locate %-10s -> \"%s\"\n", pick->name, reply.c_str());
            // "from" RATHER THAN "asked", because the stand-off is where the
            // ring STARTS and not what the answer has to be: a fish five
            // metres out from the bank is reached from the bank, which is
            // 2.0 m asked and 5.0 m arrived at and nothing wrong. Only the
            // land rows should land on their own number.
            std::printf("      stood (%.0f, %.0f)  %.1f m off (ring from %.1f)  "
                        "in water: %s  in a trunk: %s\n",
                        pos_.x, pos_.z, off, pick->stand, wet ? "YES -- FAIL" : "no",
                        tree ? "YES -- FAIL" : "no");
            // Facing it. The whole point of the aim is that the animal is on
            // the crosshair, and a yaw that is 180 degrees out looks exactly
            // like a teleport that went nowhere.
            const Vec3 f = forward();
            const Vec3 to = normalize(Vec3(target.x - pos_.x, target.y - pos_.y,
                                           target.z - pos_.z));
            const float dot = f.x * to.x + f.y * to.y + f.z * to.z;
            std::printf("      facing it: %s (cos %.3f)\n", dot > 0.999f ? "yes" : "NO -- FAIL",
                        dot);
            if (wet || tree || dot <= 0.999f) ++wrong;
            // Put the world back under the player before the second pass --
            // the survey above moved them, and the next row is chosen from
            // populations that were filled somewhere else.
            //
            // -- AND THIS TEST CAN RUN THE CARD OUT OF MEMORY ----------------
            //
            // At some spawns it dies here, in World::update, and takes the run
            // with it SILENTLY -- the log stops mid-section with no FAIL,
            // which reads as a timeout:
            //
            //   (Fatal) GFX call '...createCommandBuffer(...)' failed with
            //   error -2005270523 (DXGI_ERROR_DEVICE_REMOVED)
            //     5# v2::World::update            world.h:9790
            //     6# v2::ForestApp::runLocateTest app_tests_interaction.inl
            //
            // NOT AN UNPACED SUBMIT LOOP, which is what it looks like and what
            // it was first "fixed" as: adding the 2 ms every sibling loop here
            // carries changed nothing at all, the crash simply moved to the
            // next streaming loop along. It is VRAM. The card reports 6.0 of
            // 11.0 GB in use before the test starts (see the `gpu` line) and
            // this test teleports across the world a dozen times, each jump
            // streaming a fresh ring while the old chunks are still resident.
            //
            // It surfaced the day the spawn started roaming -- see
            // chooseSpawn's stage one -- because the old spawn was always the
            // same clearing and never reached the configurations that tip it
            // over. The spawn is not the bug; it is the thing that found it.
            // UNFIXED, and it wants someone with the eviction path in hand.
            for (int i = 0; i < 120; ++i) world_.update(player_.pos);
        }

        // ---- THE FROG, WHICH IS GATED BY A WOOD *AND* BY WATER -------------
        //
        // (user 2026-09-14: "I dont see any frogs even when doing /locate
        // frog.")
        //
        // ITS ROW USED TO SAY `water = false`, so a miss fell through to the
        // birch BAND CENTRE -- dry birch wood, where fillFrogs cannot place one
        // because it places from a bank and from nothing else. The command ran,
        // reported success, moved you, and took you to the one kind of place
        // the animal does not live. Every time. So the check is not "does
        // /locate answer" but "does where it puts you have frogs in it a moment
        // later", which is the only thing the player was ever asking.
        {
            const LifeName *fr = nullptr;
            for (const LifeName &ln : lifeNames())
                if (std::string(ln.name) == "frog") fr = &ln;
            std::printf("\n  -- /locate frog, the one row gated by a wood AND by water --\n");
            if (!fr) {
                std::printf("  NO FROG ROW -- WRONG\n");
                ++wrong;
            } else {
                const std::string said = locateLife(*fr);
                std::printf("  said: %s\n", said.c_str());
                pos_ = player_.eyePosition();
                const bool birchThere = world_.terrain.birchAt(pos_.x);
                // HOW FAR THE WATER IS FROM WHERE IT PUT YOU. A frog stands
                // within kFrogShoreM of a bank, so an arrival that is not
                // beside one is an arrival with no frogs in its future.
                float best = 1e9f;
                for (float r = 0.0f; r <= 40.0f; r += 1.0f) {
                    const int steps = (r < 1.0f) ? 1 : maxi(8, int(2.0f * PI * r));
                    for (int k = 0; k < steps; ++k) {
                        const float a = float(k) / float(steps) * 2.0f * PI;
                        if (!wetColumnAt(pos_.x + cosf(a) * r, pos_.z + sinf(a) * r)) continue;
                        best = r;
                        r = 41.0f;
                        break;
                    }
                }
                std::printf("  arrived (%.0f, %.0f) -- the %s wood, water %.0f m away\n", pos_.x,
                            pos_.z, birchThere ? "birch" : "PINE", double(best));
                // ...AND THEN GIVE IT THE MOMENT THE REPLY PROMISES.
                warmLife(pos_, 20);
                Vec3 at{0, 0, 0};
                const bool got = nearestLife(Life::Frog, &at);
                // ...UNLESS THE WOOD IT NEEDS DOES NOT EXIST HERE. The frog
                // is gated to birch and oak, and a world pinned to pine -- which
                // every DEM world is -- has neither, so /locate cannot travel to
                // one and no amount of waiting will produce a frog. That is the
                // world being what it was asked to be, not a defect, and
                // counting it as one made this whole run print FAIL.
                const bool canExist = !world_.terrain.forced || birchThere;
                std::printf("  frogs after 20 s  %s\n",
                            got ? "yes -- and that is the whole point"
                                : canExist ? "NONE, AND THERE SHOULD BE -- WRONG"
                                           : "none -- this world is pinned to a wood "
                                             "the frog does not live in");
                if (canExist && (!got || !birchThere || best > 20.0f)) ++wrong;
            }
        }

        // ---- ...AND IS ANY OF IT ACTUALLY ON SCREEN ------------------------
        //
        // (user 2026-09-14, three times: "I dont see the frog on the field.")
        //
        // EVERY CHECK IN THIS FILE UNTIL NOW READ THE HOST. livePoints, the
        // census, /locate, the speeds -- all of them ask the population where
        // it thinks it is, and all of them were perfectly happy about a frog
        // that has never once been drawn. The thing none of them touched is the
        // PUBLISH: setFlyerInstance drops a slot past the end of a population's
        // run in silence, and the critters' run was two short before the frog
        // was even added.
        //
        // So this walks the band itself. It is the only test here that would
        // have caught it, and it is four lines.
        {
            // PUBLISHED FIRST, AND THAT IS NOT A DETAIL. warmLife TICKS the
            // populations and never publishes them -- it settles behaviour, it
            // does not draw -- so asking the band straight after it reports
            // every run empty, which is a test that fails on everything and
            // therefore says nothing. The first cut of this did exactly that and
            // read "0 drawn" for the bunnies too.
            critters_.publish(world_, kCritterSlot0);
            bunnies_.publish(world_, kBunnySlot0);
            bunnies_.publishSkunks(world_, kMarchSlot0);
            bees_.publish(world_, kBeeSlot0);
            std::printf("\n  -- published into the flyer band --\n");
            const int runs[][3] = {
                {kCritterSlot0, kCritterSlots, 0},
                {kBunnySlot0, kBunnySlots, 1},
                {kMarchSlot0, kMarchSlots, 2},
                {kBeeSlot0, kBeeSlots, 3},
            };
            static const char *kRun[4] = {"critters", "bunnies", "marchers", "bees"};
            for (const auto &r : runs) {
                int on = 0;
                for (int k = 0; k < r[1]; ++k) on += world_.flyerShown(r[0] + k) ? 1 : 0;
                std::printf("  %-9s slots %3d..%-3d  %d drawn\n", kRun[r[2]], r[0],
                            r[0] + r[1] - 1, on);
            }
            // THE FROG IS THE LAST RUN INSIDE THE CRITTERS' RUN, which is why it
            // was the one that fell off. Counted on its own, by the same
            // arithmetic Critters::publish walks.
            const int frog0 = kCritterSlot0 + kFireflyCount + kAntCount + kHouseflyCount +
                              kLbugCount;
            int frogsDrawn = 0;
            for (int k = 0; k < kFrogCount; ++k)
                frogsDrawn += world_.flyerShown(frog0 + k) ? 1 : 0;
            Vec3 fat{0, 0, 0};
            const int frogsAlive = nearestLife(Life::Frog, &fat) ? 1 : 0;
            std::printf("  frog run  slots %3d..%-3d  %d drawn, %s alive on the host\n", frog0,
                        frog0 + kFrogCount - 1, frogsDrawn, frogsAlive ? "some" : "none");
            if (frogsAlive && !frogsDrawn) {
                std::printf("      ^ ALIVE AND NOT DRAWN -- the band is too short\n");
                ++wrong;
            }
        }

        // -- ...AND THE ARCADE'S MAPS, WHICH ARE THE OTHER HALF OF /locate ---
        //
        // (user 2026-09-18: "I want to be able to type /locate (map name) for
        // example.")
        //
        // WHAT THIS CATCHES is everything between the voxelizer and the
        // console: a .maps sidecar that does not match the .vox beside it, a
        // rectangle the spawn search cannot find open ground inside, and a
        // map seated so that its arrival lands in the foundation rather than
        // on it. None of those is visible in a render -- the map looks
        // perfect and the command puts you inside a wall.
        //
        // IT DOES NOT ENTER THE LEVEL. enterLevelAt swaps the held kit and
        // rebuilds the TLAS, which is not what a survey should do; the
        // question here is only whether the arrival POINT is sound, and that
        // is a pure function of the asset.
        {
            const auto &maps = world_.levelMaps();
            std::printf("\n  -- what /locate finds in the arcade --\n");
            if (maps.empty()) {
                std::printf("  no maps -- the level asset has no .maps sidecar beside it; "
                            "run tools/voxelize_arcade.py\n");
                ++wrong;
            }
            const float baseY = World::levelOrigin().y;
            for (const World::LevelMap &m : maps) {
                const Vec3 s = world_.levelMapSpawn(m);
                const float yaw = world_.levelMapYaw(m);
                const int vx = int((s.x - World::levelOrigin().x) / VOXEL_M);
                const int vz = int((s.z - World::levelOrigin().z) / VOXEL_M);
                const bool in = vx >= m.x0 && vx < m.x1 && vz >= m.z0 && vz < m.z1;
                // ON the floor, not IN it. levelMapSpawn's fallback returns the
                // grid's own base, which is half a metre under the concrete --
                // the exact bug levelSpawn was rewritten for once already.
                const bool stood = s.y > baseY + 0.05f;
                std::printf("  %-11s %4d x %-4d voxels   spawn (%.0f, %.1f, %.0f) yaw %.0f  %s\n",
                            m.name.c_str(), m.x1 - m.x0, m.z1 - m.z0, s.x, s.y, s.z, yaw,
                            (in && stood) ? "ok" : "BAD");
                if (!in) {
                    std::printf("      ^ the arrival is OUTSIDE this map's own rectangle\n");
                    ++wrong;
                }
                if (!stood) {
                    std::printf("      ^ the arrival is at the base of the grid -- it found no "
                                "open ground and fell through to the fallback\n");
                    ++wrong;
                }
            }
        }

        // ---- THE CROP, WHICH IS FOUND THE SAME WAY AND IS NOT ALIVE --------
        //
        // (user 2026-09-19: "give me a /locate apple command along with the
        // orange too" -- "teleports me to the nearest apple".)
        //
        // THE ONE THING THE REPLY CANNOT SAY IS WHICH FRUIT IT TOOK YOU TO,
        // and that is what this is for. Kind 6 is BOTH fruits and the species
        // is DecorAt::index, so a query that dropped the index would answer
        // "the nearest fruit" to both names: the console would print "orange
        // -- 412, -80, 4.0 m off", the teleport would be real, the arrival
        // would be four metres from something -- and it would be an apple.
        // Nothing in the reply, the distance or the crosshair is wrong in that
        // world; only the fruit is.
        //
        // SO THE TWO NAMES MUST NOT ANSWER WITH THE SAME TREE. hangFruit gives
        // a crown ONE species on its own salt ("an apple tree is an apple
        // tree"), so two hits at one coordinate cannot happen while the filter
        // works and cannot fail to happen once it stops.
        //
        // IT PUTS ITS OWN SPAWN BACK before each arrival, because being stood
        // next to the apple moves which orange is nearest.
        //
        // AND IT RUNS LAST, WHICH IS NOT TIDINESS. Fruit is oak-only, so from
        // anywhere else this block's travel arm is a 1.2 km round trip to the
        // oak band -- and a teleport is instant while a population is not.
        // Run in the middle, it left all THREE of the world's frogs an oak
        // band away with no slot free to place one near the player, and the
        // frog check below reported "NONE, AND THERE SHOULD BE -- WRONG"
        // standing one metre from water in the right wood, on a world where
        // nothing was broken. Putting the populations back costs twenty
        // simulated seconds of every class in the game and pushed the whole
        // test past its timeout; running last costs nothing and cannot be
        // forgotten the way a restore can.
        //
        // IT THEREFORE ASKS THE WORLD WHERE IT IS rather than reading `here`,
        // which is the wood at the ORIGINAL spawn and several sections stale
        // by the time this runs.
        {
            const Vec3 spawn = pos_;
            const uint8_t cropWood = world_.terrain.woodBit(spawn.x);
            static const char *kFruit[2] = {"apple", "orange"};
            Vec3 fat[2] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
            bool got[2] = {false, false};
            std::printf("\n  -- the crop, in the %s wood --\n",
                        world_.terrain.woodName(pos_.x));
            for (int i = 0; i < 2; ++i) {
                got[i] = world_.nearestFruit(i, spawn, 260.0f, &fat[i]);
                if (!got[i]) {
                    // NO OAK IN RANGE IS THE ORDINARY ANSWER out here, and it
                    // is the same answer the survey above already counted:
                    // /locate travels to the oak band on it.
                    //
                    // AND ONE SPECIES MISSING IS ORDINARY EVEN IN THE OAK.
                    // This counted a miss as WRONG per fruit, which is a
                    // stricter claim than the world makes: hangFruit gives a
                    // whole crown ONE species on a 50/50 roll and only bears
                    // on 15% of the oaks tall enough, so a thin stretch of
                    // wood holding apples and no oranges inside 260 m is a
                    // wood, not a defect. Measured -- the nearest apple was
                    // 222 m out and there was no orange at all, and nothing
                    // was broken. The verdict moved to "neither", below.
                    std::printf("  %-6s  none within 260 m\n", kFruit[i]);
                    continue;
                }
                std::printf("  %-6s  (%.0f, %.1f, %.0f)  %.0f m out, %.1f m off the ground\n",
                            kFruit[i], fat[i].x, fat[i].y, fat[i].z,
                            std::hypot(fat[i].x - spawn.x, fat[i].z - spawn.z),
                            fat[i].y - world_.terrain.heightM(fat[i].x, fat[i].z));
            }
            // THE OAK WOOD HAS TO HOLD A CROP, but it is "either fruit" that
            // says so -- see the note above on why per-fruit was too strict.
            if ((cropWood & kWoodOak) && !got[0] && !got[1]) {
                std::printf("  NO CROP AT ALL IN THE OAK WOOD -- WRONG\n");
                ++wrong;
            }
            if (got[0] && got[1]) {
                const float gap = std::hypot(fat[0].x - fat[1].x, fat[0].z - fat[1].z);
                std::printf("  the two are %.1f m apart  %s\n", gap,
                            gap > 0.5f
                                ? "-- two different trees, which is the index filter working"
                                : "-- ONE FRUIT ANSWERING TO BOTH NAMES, WRONG");
                if (gap <= 0.5f) ++wrong;
            }
            bool walked = false;
            for (int i = 0; i < 2; ++i) {
                teleportTo(spawn.x, spawn.z);
                const LifeName *row = nullptr;
                for (const LifeName &ln : lifeNames())
                    if (std::string(ln.name) == kFruit[i]) row = &ln;
                if (!row) {
                    std::printf("  NO %s ROW -- WRONG\n", kFruit[i]);
                    ++wrong;
                    continue;
                }
                const std::string said = runCommand(std::string("/locate ") + kFruit[i]);
                pos_ = player_.eyePosition();
                // -- THE TRAVEL ARM, WHICH IS THE ONE A PLAYER MEETS FIRST ---
                //
                // Fruit is oak-only and the default world spawns you wherever
                // the spawn picker likes, so "/locate apple" typed in the pine
                // is the ORDINARY case and its whole job is the trip. The
                // reply promises an oak wood with a crop in it; this checks
                // both halves rather than the sentence.
                //
                // THE RING HAS TO BE STREAMED BEFORE IT CAN BE ASKED. Decor
                // lives on meshed chunks, so nearestFruit on the frame of the
                // teleport reads an empty world and reports "no crop" whatever
                // is standing there -- the same trap the arrival check below
                // this block already carries for trunks.
                if (!got[i]) {
                    // ONE TRIP, NOT TWO. Both names take the identical route --
                    // the same nearestWoodX, the same oak band, the same
                    // "give it a moment" -- and the only thing that differs is
                    // the index, which the two checks above already pin down.
                    // Walking it twice costs two full re-rings across 2.4 km of
                    // world and pushed this test past ten minutes, which is
                    // long enough that it stops being run.
                    if (walked) {
                        std::printf("  /locate %-6s -> %s\n      (the same trip the %s just "
                                    "made -- not walked twice)\n",
                                    kFruit[i], said.c_str(), kFruit[1 - i]);
                        continue;
                    }
                    walked = true;
                    if (world_.terrain.forced) {
                        std::printf("  /locate %-6s -> %s\n      (pinned to one wood -- there "
                                    "is nowhere to travel, and saying so is the answer)\n",
                                    kFruit[i], said.c_str());
                        continue;
                    }
                    const bool inOak = (world_.terrain.woodBit(pos_.x) & kWoodOak) != 0;
                    // -- IT ASKS THE TERRAIN, NOT THE CHUNKS ---------------
                    //
                    // This used to stream the destination (120 world_.update
                    // passes) and then check a crop was really there. That is
                    // the honest question and it is the one thing in this test
                    // the card will not survive: a fresh wood streamed in at
                    // the end of a run that has already teleported a dozen
                    // times takes the DEVICE out inside World::update --
                    //
                    //   (Fatal) DXGI_ERROR_DEVICE_REMOVED
                    //
                    // -- and the run dies with no FAIL and no verdict, which
                    // costs far more than the check is worth. Confirmed to be
                    // nothing to do with the spawn: it fires with the picker
                    // skipped (--cam-x/--cam-z), and evicting first
                    // (reloadWorld) does not save it either, because the peak
                    // is the NEW wood rather than the old residency.
                    //
                    // So this arm checks what it can ask for free -- the reply
                    // routed you into the oak -- and the CROP ITSELF is proved
                    // by the other arm, which runs whenever the spawn is
                    // already in the oak wood and checks the fruit, the
                    // species filter and the arrival. Between them the claim
                    // is covered; it is only covering it in ONE run that the
                    // hardware refuses.
                    std::printf("  /locate %-6s -> %s\n", kFruit[i], said.c_str());
                    std::printf("      landed in the %s wood   %s\n",
                                world_.terrain.woodName(pos_.x),
                                inOak ? "travelled (run --oak to see the crop checked)"
                                      : "WRONG");
                    if (!inOak) ++wrong;
                    continue;
                }
                const float d = std::hypot(fat[i].x - pos_.x, fat[i].z - pos_.z);
                // ON THE CROSSHAIR, which is half of what the command means: a
                // fruit hangs several metres up, so an arrival that faces the
                // right coordinate on the ground still has the player looking
                // at a trunk. `dot` is against the FULL 3D bearing for that
                // reason -- lookAt sets the pitch as well as the yaw.
                // (`closeEnough`, not `near`: windows.h defines `near` to
                // nothing, and app.h carries that note three times.)
                const Vec3 f = forward();
                const Vec3 want{fat[i].x - pos_.x, fat[i].y - pos_.y, fat[i].z - pos_.z};
                const float wl = maxf(0.01f, length(want));
                const float dot = (want.x * f.x + want.y * f.y + want.z * f.z) / wl;
                // THE STAND-OFF IS A FLOOR, NOT A TARGET. standNear walks
                // outward from the fruit taking the first dry column, and
                // teleportTo's findClear then steps the body out of whatever
                // trunk it landed in -- so the arrival is at least `stand` and
                // a few metres more is the ring doing its job, not a miss.
                const bool closeEnough = d >= row->stand - 1.5f && d < row->stand + 6.0f;
                std::printf("  /locate %-6s -> %s\n", kFruit[i], said.c_str());
                std::printf("      stood %.1f m off (asked %.1f), crosshair %.3f   %s\n", d,
                            row->stand, dot,
                            (closeEnough && dot > 0.99f) ? "on it" : "WRONG");
                if (!closeEnough || dot <= 0.99f) ++wrong;
            }
            // Nothing runs after this, so the populations are left wherever
            // the trip put them on purpose -- see the note at the top of the
            // block. The player still goes home: a test that ends somewhere
            // other than it started is one you cannot read the last section of.
            teleportTo(spawn.x, spawn.z);
        }

        std::printf("\n  %s\n", wrong ? "FAIL" : "PASS -- every row is wired to its own animal, "
                                                 "and every map has somewhere to arrive.");
    }

    void runFellTest() {
        std::printf("\n=== FELL TEST ===\n");
        // WHERE THE SPAWN IS, BEFORE ANYTHING ELSE. This runs before the
        // player has been put anywhere, so pos is still the origin -- and
        // streaming the world round the origin finds a wood nobody is standing
        // in. The first run of this printed "no tree within 80 m" for exactly
        // that reason.
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);

        // The world has to exist before anything can be felled in it, and the
        // chunks are meshed on worker threads -- so this gives them time rather
        // than spinning on a queue they have not filled yet.
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        std::printf("  spawn (%.1f, %.1f, %.1f)\n", player_.pos.x, player_.pos.y,
                    player_.pos.z);

        // NOT NAMED 'near'. windows.h still defines near and far as empty macros
        // from the segmented-memory era, so `std::vector<Solid> near;` compiles
        // as `std::vector<Solid> ;` and the errors name neither of them. This
        // file already carries the same note twice; here is the third time.
        std::vector<Solid> around;
        world_.collidersNear(player_.pos, 120.0f, &around);
        const Solid *tree = nullptr;
        float best = 1e30f;
        for (const Solid &s : around) {
            if (s.modelKind != 0 || !s.vol || s.hx <= 0.0f) continue;
            const float dx = s.cx - player_.pos.x, dz = s.cz - player_.pos.z;
            if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; tree = &s; }
        }
        if (!tree) {
            std::printf("  no tree within 80 m of the spawn -- try another --spawn\n");
            return;
        }
        const Solid so = *tree;
        std::printf("  tree at (%.1f, %.1f, %.1f)  model %d  %d x %d voxels\n", so.tx, so.baseY,
                    so.tz, int(so.modelIndex), int(so.msx), int(so.msz));
        // STAND WHERE THE TREE IS. The ground patch follows the player, and a
        // player who has just chopped a tree down is next to it -- so the test
        // has to be too, or it measures a fall over ground that was never
        // streamed into the scene.
        player_.placeOnGround(walkWorld(), so.cx, so.cz);

        // ---- cut it through, aiming at the TRUNK from beside it ------------
        //
        // NOT THE MIDDLE OF ITS BOX. A birch is a trunk with the crown leaning
        // off it, so the box centre can be five metres from the wood -- the
        // same trap the placement fell into. The trunk is where the model is
        // solid at its own row zero, which is its underside.
        double bx = 0.0, bz = 0.0;
        long nb = 0;
        for (int mz = 0; mz < int(so.msz); ++mz)
            for (int mx = 0; mx < int(so.msx); ++mx)
                if (solidVoxel(so, mx, 0, mz)) {
                    bx += double(mx) + 0.5;
                    bz += double(mz) + 0.5;
                    ++nb;
                }
        if (!nb) {
            std::printf("  the model has nothing at its base\n");
            return;
        }
        float wx = 0.0f, wz = 0.0f;
        solidWorldSpace(so, float(bx / double(nb)) * VOXEL_M, float(bz / double(nb)) * VOXEL_M,
                        &wx, &wz);
        std::printf("  trunk at (%.1f, %.1f)\n", wx, wz);
        // SWEPT ACROSS THE TRUNK, not drilled into it. Every blow from the
        // same point along the same ray eats a TUNNEL through the wood, and a
        // tunnel severs nothing -- forty of those left the tree standing. A
        // player's aim wanders across the cut, so this does too.
        const float cutY = so.baseY + 1.2f;
        int blows = 0;
        bool down = false;
        for (; blows < 60 && !down; ++blows) {
            const float off = (float(blows % 11) - 5.0f) * 0.12f;
            const Vec3 eye{wx + 3.0f, cutY + (float(blows % 3) - 1.0f) * 0.1f, wz + off};
            const Vec3 dir{-1.0f, 0.0f, 0.0f};
            if (!world_.carveModel(so, eye, dir, 5.0f, kDigRadiusVox, &spoilVol_, &spoilN_,
                                   &spoilAt_, &spoilYaw_))
                continue;
            down = world_.fellTree(physics_, so, dir, simMs_);
        }
        if (!down) {
            std::printf("  %d blows and it never came down\n", blows);
            return;
        }
        std::printf("  felled after %d blows\n\n", blows);
        std::printf("  %6s %9s %9s %9s %9s %9s %9s %9s\n", "ms", "x", "y", "z", "pitch",
                    "fall m/s", "spin r/s", "in ground");

        // ---- and then watch it -------------------------------------------
        //
        // THE GROUND HAS TO BE IN THE SCENE. The frame loop builds the height
        // field patch when the player leaves the last one; nothing here does,
        // so without this the tree falls through a world with no floor in it --
        // which is a fault in the test rather than in the game, and the first
        // run of this spent its whole trace proving it.
        const float dt = 1.0f / 60.0f;
        for (int f = 0; f < 900; ++f) {
            maybeRebuildGroundPatch();
            physics_.step(dt);
            simMs_ += double(dt) * 1000.0;
            world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                [&](float x, float z) { return player_.surfaceAt(walkWorld(), x, z); });
            if ((f % 30) != 0) continue;
            for (int i = 0; i < 512; ++i) {
                Vec3 p{0, 0, 0}, lin{0, 0, 0}, ang{0, 0, 0};
                float q[4] = {0, 0, 0, 1};
                if (!world_.debrisPose(i, &p, q)) continue;
                world_.debrisVel(physics_, i, &lin, &ang);
                // How far off upright the model's own +Y has been tipped.
                const float uy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
                const float pitch = acosf(uy < -1.0f ? -1.0f : (uy > 1.0f ? 1.0f : uy)) *
                                    57.29578f;
                // HOW MUCH OF THE DRAWN TREE IS UNDER THE GROUND RIGHT NOW.
                // Per voxel, per column -- see World::debrisSink. A body that
                // sweeps through the hillside on its way over and comes out
                // clean is a different fault from one that settles buried, and
                // without this column they look identical in a trace.
                const WalkWorld wwT = walkWorld();
                const World::Sink sk = world_.debrisSink(
                    i, [&](float x, float z) { return walkGroundM(wwT, x, z); });
                std::printf("  %6.0f %9.2f %9.2f %9.2f %8.1fd %9.2f %9.2f  %5.1f%% %.1fm\n",
                            double(f) * dt * 1000.0, p.x, p.y, p.z, pitch, -lin.y,
                            sqrtf(ang.x * ang.x + ang.y * ang.y + ang.z * ang.z),
                            100.0 * double(sk.under) / double(maxi(1, sk.solid)), double(sk.worst));
                break;
            }
        }
        // ...AND WHETHER IT ENDED UP INSIDE ANYTHING. The reported bug was a
        // tree clipping into a rock as it felled, and this is that question
        // asked of the collider the solver was actually given.
        int clipped = 0, boxes = 0;
        for (int i = 0; i < 512; ++i) {
            Vec3 p{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            clipped = world_.debrisClip(i);
            boxes = world_.debrisWindowBoxes();
            break;
        }
        std::printf("\n  collider boxes inside a rock or a trunk at rest: %d"
                    "   (static window %d boxes)\n",
                    clipped, boxes);

        // -- ...AND HOW MUCH OF THE TREE IS INSIDE THE HILLSIDE --------------
        //
        // The line above asks the SOLVER's shape about ROCKS. This asks the
        // DRAWN tree about the GROUND, which is the question "the tree clips
        // through the terrain" is actually about -- and the one nothing here
        // has ever asked. See World::debrisSink for why it is per voxel and
        // per column rather than a bounding box against a corner height.
        {
            const WalkWorld wwS = walkWorld();
            for (int i = 0; i < kDebrisInstances; ++i) {
                Vec3 p{0, 0, 0};
                float q[4] = {0, 0, 0, 1};
                if (!world_.debrisPose(i, &p, q)) continue;
                const World::Sink sk = world_.debrisSink(
                    i, [&](float x, float z) { return walkGroundM(wwS, x, z); });
                if (!sk.solid) continue;
                std::printf("  slot %d: %d of %d voxels are under the ground (%.1f%%), "
                            "deepest %.2f m at (%.1f, %.1f, %.1f)\n",
                            i, sk.under, sk.solid,
                            100.0 * double(sk.under) / double(maxi(1, sk.solid)), double(sk.worst),
                            double(sk.worstAt[0]), double(sk.worstAt[1]), double(sk.worstAt[2]));
            }
        }

        // ---- AND THE ONE NUMBER THAT SAYS "IT LANDED" ---------------------
        //
        // The trajectory above reports the body's ORIGIN, which for a felled
        // tree is the model's base corner -- metres from the wood once the
        // thing is lying down, and below the ground by construction because the
        // collider starts at the CUT. Reading it as a height is what made a
        // floating tree and a buried one look the same in this trace, twice.
        //
        // So: the SHAPES' own bounds against the ground under them. A collider
        // barely taller than it is wide is a tree that never got a trunk -- the
        // birch failure, where an 11 m tree was given an 0.8 m stub, could not
        // topple, and left the mesh hanging in the air.
        for (int i = 0; i < 512; ++i) {
            Vec3 p{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            // CONTINUE, NOT BREAK. debrisPose answers for a slot whose actor
            // has already been swept, and breaking there printed nothing at all
            // -- which reads as "the test did not run" rather than "that slot
            // was stale", and cost a rebuild to tell apart.
            if (!world_.debrisBounds(physics_, i, &lo, &hi)) continue;
            const WalkWorld ww = walkWorld();
            float g = walkGroundM(ww, lo.x, lo.z);
            g = maxf(g, walkGroundM(ww, hi.x, lo.z));
            g = maxf(g, walkGroundM(ww, lo.x, hi.z));
            g = maxf(g, walkGroundM(ww, hi.x, hi.z));
            g = maxf(g, walkGroundM(ww, (lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f));
            std::printf("\n  collider   %.1f x %.1f x %.1f m   longest side %.1f m\n",
                        double(hi.x - lo.x), double(hi.y - lo.y), double(hi.z - lo.z),
                        double(maxf(hi.x - lo.x, maxf(hi.y - lo.y, hi.z - lo.z))));
            std::printf("  at rest    underside %.2f m, ground %.2f m  -->  %+.2f m %s\n",
                        double(lo.y), double(g), double(lo.y - g),
                        (lo.y - g > 0.5f) ? "FLOATING" : "resting");
            break;
        }
        std::printf("\n  (pitch 0 = still standing, 90 = flat on the ground)\n");

        // -------------------------------------------------------------------
        // ...AND NOW CHOP THE LOG THAT IS LYING THERE.
        //
        // "the player should be able to hit a fellen tree with an axe for
        // example and it break even when felled on the ground."
        //
        // Two separate things to prove, and the first one is the whole of the
        // reported bug: can the swing SEE it. Before World::debrisRay the
        // answer was no and could not be anything else -- a felled tree is not
        // in w.solids, which is the only list the swing walked.
        //
        // The second is the break. Nothing counts blows: the bite is carved out
        // of the body's own voxels and the body is re-made as however many
        // six-connected pieces that left, so "it broke" is the loose count
        // going up on the blow that reached the far side of the trunk.
        // -------------------------------------------------------------------
        std::printf("\n=== CHOP TEST -- the axe against the log on the ground ===\n");
        int log = -1;
        Vec3 lo{0, 0, 0}, hi{0, 0, 0};
        for (int i = 0; i < kDebrisInstances; ++i) {
            Vec3 p{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            if (!world_.debrisBounds(physics_, i, &lo, &hi)) continue;
            log = i;
            break;
        }
        if (log < 0) {
            std::printf("  nothing loose left to chop\n");
            return;
        }

        // STAND BESIDE THE WOOD, NOT BESIDE THE BOX.
        //
        // A felled tree is a long thin thing lying at an angle inside a big
        // box, so the middle of its bounds is a point in the air next to the
        // log about as often as it is the log -- and the swing reaches 5.3 m,
        // which the far side of that box is not within. Same trap runFellTest
        // hit aiming at a standing birch, same answer: aim at where the model
        // is SOLID. See World::debrisAim.
        Vec3 wood{0, 0, 0};
        if (!world_.debrisAim(log, &wood, /*butt=*/true)) {
            std::printf("  the body has no voxels to aim at\n");
            return;
        }
        // ACROSS THE TRUNK, NOT ALONG IT. The collider's longest horizontal
        // side is the log's own axis, and the cut plane is perpendicular to it.
        const bool alongX = (hi.x - lo.x) >= (hi.z - lo.z);
        const Vec3 mid = wood;
        const Vec3 dir = alongX ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 eye0 = alongX ? Vec3{mid.x, mid.y, mid.z - 2.5f}
                                 : Vec3{mid.x - 2.5f, mid.y, mid.z};
        std::printf("  log slot %d   collider %.1f x %.1f x %.1f m   axis %c   %d voxels\n", log,
                    double(hi.x - lo.x), double(hi.y - lo.y), double(hi.z - lo.z),
                    alongX ? 'X' : 'Z', world_.debrisVoxels(log));
        std::printf("  wood at (%.1f, %.1f, %.1f) -- the THICK end -- swinging from 2.5 m %s\n",
                    wood.x, wood.y, wood.z, alongX ? "-Z" : "-X");

        // ---- 1. CAN THE SWING SEE IT AT ALL --------------------------------
        {
            DebrisHit dh;
            const bool saw = world_.debrisRay(eye0, dir, swingReachM(dir), &dh);
            std::printf("  the swing ray %s\n",
                        saw ? "FINDS the log" : "FINDS NOTHING -- this is the reported bug");
            if (saw)
                std::printf("    slot %d at %.2f m   voxel (%d, %d, %d)   material %u   %s\n",
                            dh.slot, double(dh.t), dh.vox[0], dh.vox[1], dh.vox[2],
                            unsigned(dh.mat),
                            dh.takes == kDebrisWood   ? "wood -- an axe takes it"
                            : dh.takes == kDebrisSoft ? "mushroom -- either tool takes it"
                                                      : "stone");
            else
                return;
        }

        // ---- 2. AND DOES IT BREAK ------------------------------------------
        //
        // ONE CUT PLANE, AND THE BLOW GOES WHEREVER THERE IS STILL WOOD IN IT.
        //
        // A FIXED SWEEP DOES NOT SEVER ANYTHING, which cost a run to learn: a
        // band of seventeen heights cut seventeen tunnels through the trunk, the
        // rays then passed clean through the holes they had made, and the log
        // sat there at 63,388 voxels for another three hundred and sixty blows.
        // The wood that was holding it together was ABOVE AND BELOW the band --
        // branches, and on a pine a great deal of needle -- and nothing was ever
        // aimed at it.
        //
        // So each blow scans the cut plane for whatever is still there and hits
        // that. A player does this without thinking about it; a test has to be
        // told. When the scan comes back empty the plane is clear, which is the
        // same thing as the log being in two pieces.
        const int before = world_.looseCount();
        const int vox0 = world_.debrisVoxels(log);
        int cuts = 0, landed = 0, broke = -1;
        double carveMs = 0.0;
        const float dt2 = 1.0f / 60.0f;
        for (; cuts < 600 && broke < 0; ++cuts) {
            const float slide = (float(cuts % 5) - 2.0f) * 0.05f;
            DebrisHit dh;
            bool found = false;
            for (int step = 0; step < 61 && !found; ++step) {
                // Outwards from the middle of the trunk, so the kerf is worked
                // from the wood the aim found rather than from the top down.
                const int k = (step + 1) / 2;
                const float rise = ((step & 1) ? -1.0f : 1.0f) * float(k) * 0.1f;
                Vec3 eye = eye0;
                eye.y = mid.y + rise;
                if (alongX)
                    eye.x = mid.x + slide;
                else
                    eye.z = mid.z + slide;
                found = world_.debrisRay(eye, dir, swingReachM(dir), &dh);
            }
            if (found) {
                // WHAT A BLOW ON A LOG COSTS. The bite re-meshes the body's
                // whole volume and rebuilds its structure, which is the same
                // work felling one does -- once per swing, so half a second
                // apart, but it is on the frame and worth a number.
                const auto t0 = std::chrono::steady_clock::now();
                const bool bit = world_.carveDebris(physics_, dh, kDigRadiusVox, simMs_,
                                                    &spoilVol_, &spoilN_, &spoilAt_, &spoilYaw_);
                carveMs += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
                if (bit) ++landed;
            }
            // A few frames between blows, the way a swing is half a second
            // apart -- so a piece that has come away has somewhere to fall.
            for (int f = 0; f < 6; ++f) {
                physics_.step(dt2);
                simMs_ += double(dt2) * 1000.0;
                world_.updateDebris(physics_, player_.eyePosition(), simMs_, [&](float x, float z) {
                    return player_.surfaceAt(walkWorld(), x, z);
                });
            }
            if (world_.looseCount() > before) broke = cuts + 1;
            if (((cuts + 1) % 150) == 0)
                std::printf("    %3d blows: %d landed, %d voxels left\n", cuts + 1, landed,
                            world_.debrisVoxels(log));
        }
        std::printf("  %d blows, %d of them carved wood\n", cuts, landed);
        std::printf("  bodies %d -> %d\n", before, world_.looseCount());
        if (landed)
            std::printf("  %.2f ms per blow -- the bite, the re-mesh and the break together\n",
                        carveMs / double(landed));
        if (broke > 0)
            std::printf("  PASS -- the log came apart on blow %d\n", broke);
        else if (landed > 0)
            std::printf("  the axe bit %d times but the log held (voxels %d -> %d)\n", landed,
                        vox0, world_.debrisVoxels(log));
        else
            std::printf("  FAIL -- no blow landed on it\n");
        if (broke <= 0) return;

        // ---- AND WHAT THE PIECES DO AFTERWARDS -----------------------------
        //
        // A break that leaves two bodies in the debris band is only half of it.
        // Each piece has to be a REAL body: its own collider, resting on the
        // ground rather than hanging where the log used to be (the rule is that
        // nothing floats), and hittable again -- because "chop it in half and
        // then chop the halves" is the next thing anybody does.
        for (int f = 0; f < 240; ++f) {
            physics_.step(dt2);
            simMs_ += double(dt2) * 1000.0;
            world_.updateDebris(physics_, player_.eyePosition(), simMs_, [&](float x, float z) {
                return player_.surfaceAt(walkWorld(), x, z);
            });
        }
        std::printf("\n  four seconds later:\n");
        const WalkWorld ww2 = walkWorld();
        for (int i = 0; i < kDebrisInstances; ++i) {
            Vec3 p{0, 0, 0}, bl{0, 0, 0}, bh{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            if (!world_.debrisBounds(physics_, i, &bl, &bh)) continue;
            float g = walkGroundM(ww2, bl.x, bl.z);
            g = maxf(g, walkGroundM(ww2, bh.x, bl.z));
            g = maxf(g, walkGroundM(ww2, bl.x, bh.z));
            g = maxf(g, walkGroundM(ww2, bh.x, bh.z));
            // ...and can it be hit again. Asked from right beside the piece,
            // which is where the player is standing by now.
            Vec3 aim{0, 0, 0};
            bool again = false;
            if (world_.debrisAim(i, &aim)) {
                DebrisHit dh2;
                const Vec3 e2{aim.x - 2.0f, aim.y, aim.z};
                again = world_.debrisRay(e2, Vec3{1.0f, 0.0f, 0.0f}, swingReachM(Vec3{1, 0, 0}),
                                         &dh2);
            }
            // ...AND WHETHER SOMETHING IS HOLDING IT UP. "Above the ground" is
            // not "in the air": a branch cut off a trunk that is still standing
            // on its end can quite properly come to rest against the next tree,
            // and the static window round the body is exactly the boxes of the
            // wood near it. So the piece is asked what is UNDER it and how fast
            // it is moving, and only a piece with nothing under it and nothing
            // happening to it is floating.
            Vec3 lin{0, 0, 0}, ang{0, 0, 0};
            world_.debrisVel(physics_, i, &lin, &ang);
            const float speed = sqrtf(lin.x * lin.x + lin.y * lin.y + lin.z * lin.z);
            float under = 0.0f;
            const int what = physics_.typeBelow(Vec3{(bl.x + bh.x) * 0.5f, bl.y - 0.05f,
                                                     (bl.z + bh.z) * 0.5f},
                                                40.0f, &under);
            const bool held = (what >= 0 && under < 1.0f) || speed > 0.05f;
            std::printf("    slot %2d  %6d voxels  %4.1f x %4.1f x %4.1f m  underside %+.2f m of "
                        "ground  %-8s  %.2f m/s  under: %s\n",
                        i, world_.debrisVoxels(i), double(bh.x - bl.x), double(bh.y - bl.y),
                        double(bh.z - bl.z), double(bl.y - g),
                        (bl.y - g > 0.5f && !held) ? "FLOATING" : "resting", double(speed),
                        what < 0 ? "nothing within 40 m"
                                 : (what == 5 ? "the ground" : "a static box"));
            if (!again) std::printf("      CANNOT BE HIT\n");
        }
    }

