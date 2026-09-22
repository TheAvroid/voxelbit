// app_tests_terrain.inl
//
// Lifted out of app.h, which was 17,043 lines with ~16,100 of them inside a
// single class. This file is #included INSIDE the body of ForestApp, at exactly
// the point the code used to sit, so the preprocessor sees the same text in the
// same order -- member declaration order, layout and initialisation order are
// all unchanged. It is not a standalone header and has no include guard.
//
// Contents: dig, shaft, ladybug, duck, kill, soil and float tests
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHAT A FELLED TREE ACTUALLY DOES, PRINTED, WITH NO WINDOW.
    //
    // This exists because the felling has now been "fixed" three times by
    // reasoning about it and has been wrong three times. The renderer cannot be
    // opened to look at it -- and should not be -- so the body is asked
    // directly: fell a tree, step the solver, and print where it is and what it
    // is doing, frame by frame.
    //
    // The three failures it is meant to tell apart, which look alike in a
    // sentence and not at all in a table:
    //
    //   IT DOES NOT MOVE          -- the pitch never leaves zero. Something is
    //                                holding it: a collider it cannot topple
    //                                off, or a body that never woke.
    //   IT BOBS                   -- y oscillates while the pitch stays put.
    //                                That is depenetration fighting gravity.
    //   IT FALLS AND KEEPS GOING  -- y runs away downward. Nothing under it.
    //
    // A fall that works looks like neither: the pitch runs from 0 to about 90
    // degrees over a second or two, y drops once and settles, and the speeds go
    // to nothing.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // DIG THE GROUND OUT FROM UNDER SOMETHING AND WATCH WHETHER IT FALLS.
    //
    // float_probe measures the GAP this leaves and proves the hole in the rule;
    // it cannot run the engine, so it cannot show the rule being obeyed. This
    // does: the real terrain, the real placements, the real dig, and
    // World::dropUndermined asked exactly where the swing path asks it.
    //
    // The pass condition is the user\'s rule, unedited -- nothing that has lost
    // the ground under it is still standing there.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === DIG TEST === -- what each tool takes, and what the ground is made of.
    //
    // WITH NO WINDOW, for the reason --fell-test and --float-test have none:
    // the alternative is putting the game on screen and swinging at a hillside
    // by hand, which proves one spot on one run and cannot be repeated after a
    // change. This asks the real swing ray, the real toolTakes and the real
    // World::dig, at a column picked from the terrain rather than chosen.
    //
    // THREE THINGS ARE BEING PROVED, and they are the three that were asked for:
    //
    //   1. THE PROFILE. A column is turf, then a few voxels of soil, then a
    //      hundred voxels of stone, then bedrock. Printed as the materials
    //      actually found down the column, so the depths are read off the world
    //      rather than off the constants that made it.
    //
    //   2. THE TOOLS DISAGREE, AND ABOUT THE RIGHT THING. The shovel takes the
    //      soil band and refuses the stone under it; the pick does the reverse;
    //      and neither of them takes bedrock. That is one table, and it is the
    //      whole of what makes a shovel a different tool from a pick.
    //
    //   3. A PIT DOES NOT PAY OUT TWICE. Bite the same spot repeatedly and the
    //      swing must follow the hole DOWN -- if it cannot see the hole it
    //      stops on the surface that used to be there and hands back a chunk of
    //      ground that is no longer under it. Every bite is printed with the
    //      row it landed on, so a swing that cannot see the hole shows as a row
    //      that will not fall.
    // -----------------------------------------------------------------------
    void runDigTest() {
        std::printf("\n=== DIG TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        std::printf("  spawn (%.1f, %.1f, %.1f)\n", player_.pos.x, player_.pos.y, player_.pos.z);

        // ---- 1. WHAT A COLUMN IS MADE OF ---------------------------------
        //
        // Read through TerrainProbe, which is what the swing reads through too,
        // so a disagreement between this table and the tools below would be one
        // bug rather than two separate ideas of the world.
        const int ci = int(std::floor(player_.pos.x / VOXEL_M));
        const int cj = int(std::floor(player_.pos.z / VOXEL_M));
        TerrainProbe probe(&world_.terrain, &world_.editStore());
        const int h = world_.terrain.heightVox(ci, cj);
        std::printf("\n  --- the column at the spawn, surface row %d ---\n", h);
        uint8_t was = 255;
        int runFrom = 0, soilFloor = h, stoneFloor = h;
        for (int d = 0; d <= 140; ++d) {
            const uint8_t m = probe.material(ci, cj, h - d);
            if (isSoilMat(m)) soilFloor = h - d;
            if (isStoneMat(m)) stoneFloor = h - d;
            if (m == was) continue;
            if (was != 255)
                std::printf("    %3d..%3d below surface   %s (id %u)\n", runFrom, d - 1,
                            matFamily(was), unsigned(was));
            was = m;
            runFrom = d;
        }
        std::printf("    %3d..    below surface   %s (id %u)\n", runFrom, matFamily(was),
                    unsigned(was));
        std::printf("    soil reaches %d voxels down, stone %d voxels down\n", h - soilFloor,
                    h - stoneFloor);

        // ---- 2. WHICH TOOL TAKES WHICH BAND ------------------------------
        //
        // The material comes from the probe and the verdict from toolTakes,
        // which is the same function the blow and the audio both ask.
        std::printf("\n  --- what each tool takes, by depth ---\n");
        std::printf("    %-6s %-10s %-6s %-6s %-6s\n", "depth", "material", "axe", "pick",
                    "shovel");
        const int kProbeDepths[] = {0, 1, 4, 10, 50, 99, 101, 120};
        for (const int d : kProbeDepths) {
            Swing s;
            s.hit = true;
            s.kind = Swing::Ground;
            s.material = probe.material(ci, cj, h - d);
            std::printf("    %-6d %-10s %-6s %-6s %-6s\n", d, matFamily(s.material),
                        toolTakes(Takes::Wood, s) ? "yes" : ".",
                        toolTakes(Takes::Stone, s) ? "yes" : ".",
                        toolTakes(Takes::Soil, s) ? "yes" : ".");
        }

        // ---- 3. A PIT, DUG THROUGH BOTH BANDS ----------------------------
        //
        // Straight down, from an eye that stays where a player's would. THE EYE
        // DOES NOT FOLLOW THE HOLE DOWN, and that is the point: every bite
        // after the first has to find the bottom of the pit through the edit
        // layer, which is exactly what the swing ray could not do before.
        //
        // AND IT CHANGES TOOLS THE WAY A PLAYER WOULD, because a pit through
        // this ground is two jobs: the shovel takes the turf and the soil under
        // it, then the soil runs out and only the pick will go on. Digging with
        // one tool would stop at the band it cannot take and prove nothing
        // about the ground below it -- which is most of the ground.
        std::printf("\n  --- twenty swings at one spot, right tool each time ---\n");
        const Vec3 eye{(float(ci) + 0.5f) * VOXEL_M, float(h + 16) * VOXEL_M,
                       (float(cj) + 0.5f) * VOXEL_M};
        const Vec3 down{0.0f, -1.0f, 0.0f};
        // NOT h + 1. The first bite legitimately lands on the row above the
        // surface -- a ray stopping on a top face crosses the boundary exactly,
        // and the floor of that is the voxel above -- so seeding this with the
        // surface row counts the opening swing as a swing that went nowhere.
        // The question only means anything from the SECOND bite on.
        int lastRow = 0;
        bool haveRow = false;
        int stuck = 0, bites = 0;
        for (int b = 0; b < 20; ++b) {
            const Swing s = swingRay(walkWorld(), eye, down);
            if (!s.hit) {
                std::printf("    %2d  the ray found no ground within reach\n", b);
                break;
            }
            const int row = int(std::floor(s.point.y / VOXEL_M));
            const char *tool = toolTakes(Takes::Soil, s)    ? "shovel"
                               : toolTakes(Takes::Stone, s) ? "pick"
                                                            : nullptr;
            if (!tool) {
                std::printf("    %2d  row %4d  %-10s NOTHING IN THE KIT TAKES IT"
                            " -- the floor of the world\n",
                            b, row, matFamily(s.material));
                break;
            }
            if (haveRow && row >= lastRow) ++stuck;
            lastRow = row;
            haveRow = true;
            ++bites;
            std::vector<uint8_t> spoil;
            int n = 0;
            Vec3 at{0, 0, 0};
            world_.dig(s.point, kDigRadiusVox, &spoil, &n, &at);
            int solid = 0;
            for (uint8_t v : spoil)
                if (v != mat::AIR) ++solid;
            std::printf("    %2d  row %4d  %-10s %-6s bit out %3d voxels\n", b, row,
                        matFamily(s.material), tool, solid);
        }
        std::printf("\n  %d bites, %d of them no deeper than the bite before\n", bites, stuck);
        std::printf("  %s\n", stuck == 0
                                  ? "PASS -- every bite came out of ground that was still there"
                                  : "FAIL -- a swing could not see the hole below it");

        // ---- 4. AND NONE OF IT IS DRAWN UNTIL IT IS SEEN -----------------
        //
        // ONLY RENDER WHAT THE PLAYER CAN SEE, measured rather than asserted.
        //
        // A hundred voxels of stone under every column is a hundred times the
        // MATTER, and the whole question is whether it is a hundred times the
        // geometry. It is not, and it cannot be: the heightmap path emits one
        // top quad per column plus the drop to each neighbour, and the voxel
        // path a dug column falls back to emits a face only where the voxel
        // next door is AIR. A voxel with six solid neighbours has no face to
        // give, so buried stone costs nothing to draw however deep it goes.
        //
        // This meshes the chunk the player is standing in -- the real mesher,
        // through the real edit layer -- and prints the ground it contains
        // against the triangles that ground turned into. The first number
        // counts every solid voxel down to bedrock; the second is what the
        // renderer is actually handed.
        //
        // THEN AGAIN, WITH THE PIT IN IT. A hole adds the faces that bound it
        // and nothing else, so the difference is a pit's worth of wall -- not a
        // column's worth of depth.
        std::printf("\n  --- what the mesher emits for this chunk ---\n");
        const int cx = EditStore::floorDiv(ci, CHUNK_VOX);
        const int cz = EditStore::floorDiv(cj, CHUNK_VOX);
        ChunkScratch scratch;
        const std::shared_ptr<const ChunkEdits> ce = world_.editStore().get(cx, cz);
        const size_t trisNow = world_.terrain.meshChunk(cx, cz, scratch, ce.get()).triCount();
        const size_t trisPristine = world_.terrain.meshChunk(cx, cz, scratch, nullptr).triCount();

        // Every solid voxel in the chunk, surface down to the bedrock the
        // profile above found. Counted from the generator, because the point of
        // the comparison is how much ground there IS.
        TerrainMemo cmemo;
        long long solidVox = 0;
        for (int j = 0; j < CHUNK_VOX; ++j)
            for (int i = 0; i < CHUNK_VOX; ++i) {
                const int hh = world_.terrain.heightVox(cx * CHUNK_VOX + i, cz * CHUNK_VOX + j,
                                                        cmemo);
                solidVox += hh;   // rows 0..hh are ground; below kBedrockVox it is bedrock
            }
        std::printf("    %lld solid voxels of ground in the chunk\n", solidVox);
        std::printf("    %zu triangles pristine, %zu with the pit in it (+%lld)\n", trisPristine,
                    trisNow, (long long)trisNow - (long long)trisPristine);
        const double perCol = double(trisPristine) / double(size_t(CHUNK_VOX) * CHUNK_VOX);
        std::printf("    one triangle per %.0f voxels of ground, %.2f triangles per COLUMN\n",
                    trisPristine ? double(solidVox) / double(trisPristine) : 0.0, perCol);
        // THE TEST IS WHICH NUMBER IT SCALES WITH. A mesher that drew what is
        // THERE would emit triangles by the voxel, and this would be in the
        // hundreds. Drawing only what can be SEEN makes it a property of the
        // surface instead: a top quad, plus however many bands the drop to a
        // neighbour has to be split into -- a handful, and flat in the depth of
        // the stone underneath. Eight is a generous ceiling on a handful; this
        // terrain measures about three.
        std::printf("    %s\n", perCol < 8.0
                                    ? "PASS -- triangles scale with the SURFACE, not the depth,"
                                      " so buried stone is free until it is cut into"
                                    : "FAIL -- the buried stone is reaching the renderer");

        // ---- ...AND CAN THE PLAYER GET INTO THE HOLE -----------------------
        //
        // THE ONE THING THIS TEST NEVER ASKED. Everything above proves the
        // world was dug: the bites go deeper, the mesher emits the pit. None of
        // it touches whether the BODY agrees, and for as long as this test
        // existed it did not -- reported as "I created a hole, then when I try
        // to go inside the hole the player still floats above it. its like the
        // missing terrain is missing from the renderer but still there in
        // memory".
        //
        // It was the other way round. The terrain really was gone; Player's
        // ground query read heightVox, which is the GENERATOR's top and cannot
        // see an edit, so the body stood on a floor nothing was drawing.
        //
        // So this asks the walk directly, at the bottom of the shaft the twenty
        // swings above just cut. It is the acceptance test for that bug and it
        // is cheap: two calls, no window, no physics.
        {
            const WalkWorld ww = walkWorld();
            // THE COLUMN THE SWINGS WENT DOWN, not the spawn. The shaft is cut
            // straight down through (ci, cj) from sixteen voxels above it --
            // asking at player_.pos measures a piece of ground nobody touched,
            // which is a test that passes when the bug is present.
            const float sx = (float(ci) + 0.5f) * VOXEL_M;
            const float sz = (float(cj) + 0.5f) * VOXEL_M;
            const float gen = float(world_.terrain.heightVox(ci, cj) + 1) * VOXEL_M;
            std::printf("\n  --- and whether the player can get into it ---\n");
            std::printf("    generated surface   %.2f m\n", double(gen));

            // TWO DIFFERENT QUESTIONS, and only the first is the bug.
            //
            // THE COLUMN is what the walk reads per sample: it must follow the
            // shaft down, and if it does not, nothing else can.
            const float col = walkGroundM(ww, sx, sz);
            std::printf("    the column says     %.2f m   (%+.2f m)  %s\n", double(col),
                        double(col - gen),
                        (col < gen - 0.5f) ? "PASS -- the walk reads the edit layer"
                                           : "FAIL -- it is still reading the generator");

            // THE BODY is groundInfo, which samples the four corners of a 52 cm
            // footprint and takes the HIGHEST. Against the 60 cm shaft the
            // twenty swings cut, standing on the rim is the RIGHT answer -- you
            // cannot fall into a hole narrower than you are. So the body's half
            // of this is asked of a pit it can actually get into: four more
            // bites around the first, which is what digging down looks like.
            // A 3x3 OF BITES AT 4 VOXELS' SPACING. A plus is not enough and
            // the diagonals are why: groundInfo samples the four CORNERS of the
            // footprint, at (+-0.26, +-0.26), and a bite of radius 0.3 m
            // centred on a plus arm at (+-0.5, 0) misses those by 5 cm. The
            // first version of this dug a plus and the body stood on four
            // untouched diagonal columns, which looks exactly like the bug.
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const Vec3 e2{(float(ci + dx * 4) + 0.5f) * VOXEL_M,
                                  float(h + 16) * VOXEL_M,
                                  (float(cj + dz * 4) + 0.5f) * VOXEL_M};
                    for (int k = 0; k < 8; ++k) {
                        const Swing s3 = swingRay(walkWorld(), e2, down);
                        if (!s3.hit) break;
                        world_.dig(s3.point, kDigRadiusVox);
                    }
                }
            const float body = player_.surfaceAt(walkWorld(), sx, sz);
            std::printf("    the body stands at  %.2f m   (%+.2f m)  %s\n", double(body),
                        double(body - gen),
                        (body < gen - 0.5f)
                            ? "PASS -- it can get into a pit its own width"
                            : "FAIL -- standing on ground that has been dug away");
        }
    }

    // The family a material id belongs to, for the tables above. Named rather
    // than numbered because a ramp is six ids that mean one thing, and "13" in
    // a test report tells nobody the shovel is standing in soil.
    static const char *matFamily(uint8_t m) {
        if (m == mat::AIR) return "air";
        if (m == mat::BEDROCK) return "bedrock";
        if (m == mat::ROCK) return "stone";
        if (isGrass(m)) return "grass";
        if (isSoil(m)) return "soil";
        if (isLitter(m)) return "litter";
        if (m == mat::SAND) return "sand";
        if (m == mat::SILT) return "silt";
        if (m == mat::DIRT) return "dirt";
        return "other";
    }

    // -----------------------------------------------------------------------
    // DIG STRAIGHT DOWN AND WATCH WHERE THE SPOIL GOES.
    //
    // (user 2026-09-14, twice: "digging up dirt seems to teleport the chunks to
    // the surface" / "still, when diging deep tin the ground, dirt voxels
    // teleport to the surface instantly this is wrong".)
    //
    // WRITTEN BECAUSE TWO FIXES WERE GUESSED AND SHIPPED. Both were reasoned
    // from the code and neither was measured, and the report came back
    // unchanged both times. A chip that is thrown out of a shaft is a NUMBER --
    // its height against the shaft floor it was born in -- and nothing in this
    // engine was asking for that number.
    //
    // IT RUNS THE LIVE FLOOR, NOT THE GENERATOR'S. --float-test hands
    // updateDebris a terrainAt built from heightVox alone, which is the world
    // as it was before anybody dug; under that backstop a chip in a shaft is
    // below the ground BY DEFINITION and is clamped to the surface every
    // frame. That would have shown this "bug" on a perfectly good engine and
    // hidden it on a broken one. The lambda here is the one the frame loop
    // uses -- walkGroundM, which has the holes in it.
    // -----------------------------------------------------------------------
    void runShaftTest() {
        std::printf("\n=== SHAFT TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        const float x = player_.pos.x, z = player_.pos.z;
        TerrainMemo memo;
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        const float surf = float(world_.terrainTopAt(ci, cj, memo) + 1) * VOXEL_M;
        std::printf("  standing at (%.1f, %.1f)  surface %.2f m\n", x, z, surf);

        // ---- a shaft, one bite every 15 cm ---------------------------------
        //
        // DEEPER THAN THE DIRT IS. The report is about DIRT, and the crust is
        // eleven voxels now -- a shaft that bottoms out on stone in the first
        // half metre is not the thing being tested.
        const float depthM = 4.0f;
        long blows = 0;
        for (float y = surf - 0.05f; y > surf - depthM; y -= 0.15f)
            if (world_.dig(Vec3(x, y, z), kDigRadiusVox, &spoilVol_, &spoilN_, &spoilAt_))
                ++blows;
        const WalkWorld ww0 = walkWorld();
        const float floorNow = walkGroundM(ww0, x, z);
        std::printf("  %ld bites, floor now %.2f m -- %.2f m down\n", blows, floorNow,
                    surf - floorNow);
        // ---- ...AND WHERE THE PLAYER STANDS WHILE DIGGING IT ---------------
        //
        // HALF THE QUESTION, AND IT WAS NOT BEING ASKED. Spoil flies to the
        // EYE, so "where does the dirt go" is "where is the player" -- and a
        // body has a radius while a bite is three voxels across. If the walk
        // keeps you standing on the rim of your own shaft, every chip you cut
        // four metres down travels four metres up through solid ground to
        // reach you, which is exactly what was reported.
        player_.placeOnGround(walkWorld(), x, z);
        std::printf("  the player stands at %.2f m -- %s\n", player_.pos.y,
                    player_.pos.y < floorNow + 0.5f ? "in the shaft"
                                                    : "ON THE RIM, over their own hole");
        if (surf - floorNow < 1.0f) {
            std::printf("  the shaft is under a metre deep; nothing here is worth "
                        "measuring\n");
            return;
        }

        // ---- one more bite, and this one keeps its spoil -------------------
        const float birthY = floorNow + 0.10f;
        if (!world_.dig(Vec3(x, birthY, z), kDigRadiusVox, &spoilVol_, &spoilN_, &spoilAt_)) {
            std::printf("  the last bite took nothing -- bedrock?\n");
            return;
        }
        const Vec3 still{0.0f, 0.0f, 0.0f};
        const int slot =
            world_.spawnDebris(physics_, spoilVol_, spoilN_, spoilAt_, still, still, simMs_,
                               0.0f, nullptr, uint8_t(kDebrisSoil));
        if (slot < 0) {
            std::printf("  no free debris slot\n");
            return;
        }
        rebuildGroundPatch();
        Vec3 p0{0, 0, 0};
        float q0[4] = {0, 0, 0, 1};
        if (!world_.debrisPose(slot, &p0, q0)) {
            std::printf("  the chip has no pose\n");
            return;
        }
        std::printf("\n  the chip is born at %.2f m, %.2f m under the surface\n", p0.y,
                    surf - p0.y);
        std::printf("  frame    chip y   vs floor   vs surface   backstop  state\n");

        // ---- and now watch it, on the frame loop's own terms ---------------
        const float dt = 1.0f / 60.0f;
        float highest = p0.y;
        // ...AND WHERE IT WAS WHEN THE COLLECT TOOK IT, which is the only
        // number here that says anything about the PHYSICS. Everything after
        // that moment is a scripted lerp to the eye and proves nothing.
        bool wasFlying = false;
        float atAbsorb = p0.y;
        Vec3 p = p0;
        for (int f = 1; f <= 180; ++f) {
            maybeRebuildGroundPatch();
            physics_.step(dt);
            simMs_ += double(dt) * 1000.0;
            {
                const WalkWorld ww = walkWorld();
                world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                    [&](float ax, float az) { return walkGroundM(ww, ax, az); });
            }
            float qq[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(slot, &p, qq)) {
                std::printf("  the chip is gone at frame %d\n", f);
                break;
            }
            if (p.y > highest) highest = p.y;
            // EVERY EARLY FRAME, AND THE FLOOR THE BACKSTOP WOULD USE.
            //
            // The two ways a chip leaves a hole look nothing alike frame by
            // frame and identical every thirty: updateDebris' clampAbove is a
            // TELEPORT -- one frame, any distance -- and a heightfield the
            // solver thinks the body is inside is a shove, fast but continuous.
            // The backstop column is printed beside it because that is the
            // number clampAbove compares against: if it reads the shaft floor,
            // the clamp is innocent and the patch is not.
            const bool flying = world_.debrisAbsorbing(slot);
            if (flying && !wasFlying) {
                wasFlying = true;
                atAbsorb = p.y;
            }
            if (f <= 40 || f % 30 == 0) {
                const WalkWorld wq = walkWorld();
                std::printf("  %5d  %7.2f  %+8.2f  %+10.2f  %8.2f  %s\n", f, p.y,
                            p.y - floorNow, p.y - surf, walkGroundM(wq, p.x, p.z),
                            flying ? "collected" : "falling");
            }
        }

        // ---- the verdict ---------------------------------------------------
        //
        // TWO THINGS, AND THE FIRST IS THE REPORT. A chip born four metres down
        // must never be seen at the surface -- not at the end, at any point, so
        // the highest it ever reached is what is tested. The second is that it
        // is still down there when the dust settles, which rules out a chip
        // that is ejected and falls back in.
        // MEASURED AT THE MOMENT THE COLLECT TOOK IT, not at the end. A chip
        // that is picked up has left the simulation: it is a kinematic lerp to
        // the eye from there on, it passes through the ground by design, and
        // judging the physics by where that lerp ends would fail every engine.
        const bool stayedDown = atAbsorb < surf - 0.5f;
        const bool restedLow = atAbsorb > floorNow - 1.0f;
        std::printf("\n  the solver left it at %.2f m (%+.2f m against the surface)  %s\n",
                    atAbsorb, atAbsorb - surf,
                    stayedDown ? "stayed in the shaft" : "CAME OUT -- THIS IS THE BUG");
        std::printf("  the highest it ever reached, the collect included, was %.2f m\n",
                    highest);
        std::printf("  %s\n",
                    (stayedDown && restedLow)
                        ? "PASS -- the spoil stayed in the hole it came out of."
                        : "FAIL -- the spoil was thrown to the surface.");
    }

    // -----------------------------------------------------------------------
    // KILL ONE OF EVERYTHING AND CHECK WHAT IT LEFT BEHIND.
    //
    // (user 2026-09-14, the five-part ask that this whole feature is.)
    //
    // FIVE THINGS HAPPEN WHEN AN ANIMAL DIES and four of them are invisible to
    // any test that only looks at whether it is gone: the flash, the pieces,
    // the sparks, the smoke and the steak. So this walks the /locate table,
    // teleports to each species in turn, aims at it, and swings until it is
    // down -- reporting every one of the five against what v1's rules say that
    // species should do.
    //
    // THE MEAT RULE IS THE POINT OF THE SPECIES LOOP. "there are unique animals
    // that dont drop anything, like the song birds and worms" -- so a test that
    // kills one rabbit proves the half that is easy. The column that matters is
    // the one where a songbird and a worm leave NOTHING and the frog beside
    // them leaves a steak.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // --duck-test: A MOTHER DIES AND HER BROOD CARRIES ON.
    //
    // (user 2026-09-15: "the babies should not dissapeare when the mother dies,
    // but stay on the field, and wander in random directions without their
    // mother ... the babies should cry".)
    //
    // THE FAILURE THIS EXISTS TO CATCH IS SILENT AND WAS ALREADY THERE. The
    // ducklings were not being deleted by anything that mentions ducklings:
    // they were retired by the population's ordinary distance test, which asks
    // how far the MOTHER is -- and a killed mother is a zeroed struct sitting
    // at the world origin, which is far from everywhere. Nothing logs that, and
    // at a glance a brood quietly fading out is indistinguishable from a brood
    // that swam off. So this asks the three questions separately: are they
    // still there, did they move, and did they weep.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // --lbug-test: DOES THE LADYBUG DOUBLE ITS RATE NEAR THE PLAYER?
    //
    // (user 2026-09-15: "if the player is near, the ladybug moves at twice the
    // rate, double in both fps but also movement speed".)
    //
    // NO TELEPORTING, AND NOTHING FORCED. The player stands still and the
    // ladybugs wander in and out of their own near band on their own, so one
    // run yields both populations -- and every sample is the real step, not a
    // rate arithmetic I could get right in the test and wrong in the engine.
    //
    // ONLY CRUISING FRAMES COUNT. A ladybug sitting on a stone has a speed of
    // zero whatever the distance, and averaging those in would drag both
    // buckets toward zero and hide the effect in the noise.
    //
    // THE ANIMATION IS MEASURED THE SAME WAY, off the frame counter, because
    // the user asked for BOTH and a change to one is not evidence about the
    // other -- the two are driven from one scaled dt in stepBugs and this is
    // what says so.
    // -----------------------------------------------------------------------
    void runLbugTest() {
        std::printf("\n=== LADYBUG TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        warmLife(player_.pos, 3);
        publishLife();

        const int lbN = Critters::lbugCount();
        const float dt = 1.0f / 60.0f;

        // -- THE DISTANCE IS SET, NOT WAITED FOR -------------------------
        //
        // The first cut of this stood still and sampled whatever distance the
        // ladybugs happened to wander to. Over a full minute that gave 5487
        // samples beyond ten metres and ONE inside five: they are scattered on
        // a 22 m lattice and have no reason to come to you, so the near bucket
        // -- the entire point of the test -- never filled.
        //
        // So the player is PUT at a chosen distance from one ladybug, every
        // frame, and the same insect is measured at both. Nothing about the
        // ladybug is touched; it does not flee, it does not seek, and the only
        // thing the player's position feeds is the rate ramp under test.
        auto measure = [&](float standOff, double *speed, double *fps, int *samples) {
            *speed = 0.0;
            *fps = 0.0;
            *samples = 0;
            Vec3 was{0, 0, 0};
            float wasF = 0.0f;
            bool had = false;
            for (int f = 0; f < 1800; ++f) {   // thirty seconds at each distance
                bool live = false, cruising = false;
                Vec3 at{0, 0, 0};
                float fr = 0.0f;
                if (critters_.lbugProbe(0, &live, &cruising, &at, &fr) && live) {
                    player_.pos.x = at.x + standOff;
                    player_.pos.z = at.z;
                    pos_ = player_.eyePosition();
                }
                world_.update(player_.pos);
                // ONE FRAME OF CRITTERS, NOT ONE SECOND. This called
                // warmLife(pos, 1) -- which advances a WHOLE SECOND of
                // population time per call -- once per loop iteration, so two
                // consecutive "frames" were a second apart and the ladybug's
                // apparent speed came out at 36 m/s against a real 2.2. The
                // same argument list the frame itself passes, so what is
                // measured is the shipping step and not a simplified one.
                critters_.update(dt, player_.pos,
                                 [this](float x, float z) {
                                     return float(world_.terrain.heightVox(
                                                      int(std::floor(x / VOXEL_M)),
                                                      int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
                                 },
                                 [this](float x, float z) { return wetColumnAt(x, z); },
                                 [this](float x) { return world_.terrain.woodBit(x); },
                                 banksNear_, perches_, isNight(), forward(),
                                 [this](float x, float z) { return waterTopAt(x, z); },
                                 [this](float x, float z) { return sandAt(x, z); });
                simMs_ += double(dt) * 1000.0;
                if (!critters_.lbugProbe(0, &live, &cruising, &at, &fr) || !live || !cruising) {
                    had = false;
                    continue;
                }
                if (had) {
                    const float mx = at.x - was.x, mz = at.z - was.z;
                    // THE WINGBEAT WRAPS at the strip length, so a negative
                    // difference is a wrap rather than the wings running
                    // backwards, and the sample is dropped rather than counted
                    // as a large step the wrong way.
                    const float dfps = (fr - wasF) / dt;
                    if (dfps > 0.0f) {
                        *speed += double(std::sqrt(mx * mx + mz * mz) / dt);
                        *fps += double(dfps);
                        ++(*samples);
                    }
                }
                was = at;
                wasF = fr;
                had = true;
            }
            if (*samples > 0) {
                *speed /= double(*samples);
                *fps /= double(*samples);
            }
        };

        double farS = 0.0, farF = 0.0, nearS = 0.0, nearF = 0.0;
        int farN = 0, nearN = 0;
        // FAR FIRST, so the near pass is not measuring a ladybug that is still
        // carrying speed from being crowded.
        measure(kLbugFarM + 4.0f, &farS, &farF, &farN);
        measure(kLbugNearM - 2.0f, &nearS, &nearF, &nearN);

        if (nearN < 60 || farN < 60) {
            std::printf("  not enough cruising frames (%d near, %d far) -- no verdict\n", nearN,
                        farN);
            return;
        }
        std::printf("  cruising frames: %d at %.0f m, %d at %.0f m\n", nearN,
                    double(kLbugNearM - 2.0f), farN, double(kLbugFarM + 4.0f));
        const double sr = farS > 1e-6 ? nearS / farS : 0.0;
        const double fr2 = farF > 1e-6 ? nearF / farF : 0.0;
        std::printf("  speed      near %.2f m/s   far %.2f m/s   ratio %.2f  %s\n", nearS, farS,
                    sr, (sr > 1.7 && sr < 2.3) ? "doubled, correct" : "NOT DOUBLED -- WRONG");
        std::printf("  wingbeat   near %.1f fps   far %.1f fps   ratio %.2f  %s\n", nearF, farF,
                    fr2, (fr2 > 1.7 && fr2 < 2.3) ? "doubled, correct" : "NOT DOUBLED -- WRONG");
        const bool ok = sr > 1.7 && sr < 2.3 && fr2 > 1.7 && fr2 < 2.3;
        std::printf("\n  %s\n",
                    ok ? "PASS -- near the player it moves and flaps at twice the rate."
                       : "FAIL -- see the line above.");
        std::fflush(stdout);
    }

    // A SPAWN WITH A LAKE IN IT, OR THERE IS NOTHING TO TEST. Ducks follow
    // water, so this test is only as good as where --spawn drops you -- and
    // when the oak band was inserted the tiling moved, which put the long-used
    // --spawn 4242 somewhere with no lake in reach and made this print "no duck
    // in this wood". That is the spawn, not the ducks: 1, 7, 99 and 2026 all
    // pass. If this says there is no duck, try another seed before believing it.
    void runDuckTest() {
        std::printf("\n=== DUCK TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();

        // THE POPULATION HAS TO HAVE RUN BEFORE IT CAN BE ASKED. runKillTest
        // wrote the note on this and this test walked straight into it anyway:
        // "the first version primed the CHUNKS and then asked where the nearest
        // bunny was, which is a question about a population that had never
        // ticked". Asked cold, every lake in range answers "no ducks".
        warmLife(player_.pos, 3);
        publishLife();
        Vec3 at{0, 0, 0};
        if (!nearestLife(Life::Duck, &at)) {
            std::printf("  no duck in this wood -- nothing to test\n");
            return;
        }
        teleportToLife(at, 6.0f);
        for (int w = 0; w < 240; ++w) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        warmLife(player_.pos, 3);
        publishLife();

        const int mom = lake_.firstFamily();
        if (mom < 0) {
            std::printf("  a duck, but no complete family within reach\n");
            return;
        }
        bool live = false, orphan = false, crying = false;
        std::printf("  a family: mother %d, three ducklings\n", mom);

        // == ONE OF THE BABIES DIES, AND THE LINE CLOSES UP ==================
        //
        // (user 2026-09-20: "if you shoot one of the baby ducks behind the
        //  mother duck, the rest of the baby ducks lose there tracking.")
        //
        // THE ONE IN FRONT, because that is the one that strands the others: a
        // duckling heels behind the sibling ahead, so killing the LAST of three
        // would prove nothing. Killed through killLifeAt, the same real path
        // the mother's death below uses.
        Vec3 sibWas[kBabyPerDuck];
        for (int b = 0; b < kBabyPerDuck; ++b)
            lake_.duckProbe(LakeLife::babyIndex(mom, b), &live, &orphan, &crying, &sibWas[b]);
        killLifeAt(kButterflySlots + kBirdSlots +
                   LakeLife::duckLocalSlot(LakeLife::babyIndex(mom, 0)));
        publishLife();
        for (int f = 0; f < 300; ++f) {
            const float dt = 1.0f / 60.0f;
            world_.update(player_.pos);
            lake_.update(dt, world_.terrain, player_.pos, forward());
            simMs_ += double(dt) * 1000.0;
        }
        {
            int followed = 0, frozen = 0, alive = 0;
            float worstToMom = 0.0f;
            Vec3 momAt{0, 0, 0};
            bool ml = false, mo = false, mc = false;
            lake_.duckProbe(mom, &ml, &mo, &mc, &momAt);
            for (int b = 1; b < kBabyPerDuck; ++b) {
                Vec3 now{0, 0, 0};
                if (!lake_.duckProbe(LakeLife::babyIndex(mom, b), &live, &orphan, &crying, &now))
                    continue;
                if (!live) continue;
                ++alive;
                const float moved = sqrtf((now.x - sibWas[b].x) * (now.x - sibWas[b].x) +
                                          (now.z - sibWas[b].z) * (now.z - sibWas[b].z));
                // FROZEN IS THE FAULT, and it is not subtle: the old code gave
                // up entirely when the leader slot was dead, so a stranded
                // duckling did not drift -- it did not move at all.
                if (moved < 0.05f) ++frozen; else ++followed;
                const float toMom = sqrtf((now.x - momAt.x) * (now.x - momAt.x) +
                                          (now.z - momAt.z) * (now.z - momAt.z));
                if (toMom > worstToMom) worstToMom = toMom;
            }
            std::printf("  -- the one in front is shot --\n");
            std::printf("  the other two        %d alive, %d still swimming, %d frozen   %s\n",
                        alive, followed, frozen,
                        (alive > 0 && frozen == 0) ? "correct"
                                                   : "THEY LOST THE LINE -- WRONG");
            std::printf("  furthest from mother %.2f m   %s\n", double(worstToMom),
                        worstToMom < kBabyLostM ? "still in the line"
                                                : "ADRIFT -- WRONG");
        }

        // WHERE THE SURVIVORS WERE, so "they wandered" can be a measurement
        // rather than an impression.
        // WHICH SLOTS, NOT HOW MANY. The population REFILLS: the slot whose
        // duckling was just shot is recycled, and a fresh one is born into it
        // -- so a count taken here and compared with a count taken later read
        // "3 of 2", and the replacement's birth showed up as one duckling
        // wandering three and a half kilometres. Only the slots that were alive
        // at this moment are the brood this phase is about.
        Vec3 was[kBabyPerDuck];
        bool wasLive[kBabyPerDuck] = {};
        int before = 0;
        for (int b = 0; b < kBabyPerDuck; ++b) {
            lake_.duckProbe(LakeLife::babyIndex(mom, b), &live, &orphan, &crying, &was[b]);
            wasLive[b] = live;
            if (live) ++before;
        }

        // ...AND SHE IS KILLED THROUGH THE REAL PATH, not by clearing a slot.
        // killLifeAt is what a blow reaches, and the orphaning is armed inside
        // LakeLife::killSlot underneath it -- so a test that zeroed the struct
        // itself would be testing the test.
        const int band = kButterflySlots + kBirdSlots + LakeLife::duckLocalSlot(mom);
        killLifeAt(band);
        publishLife();

        int sawTears = 0;
        for (int f = 0; f < 300; ++f) {   // five seconds: the 0.9 s wait, then 3 s of weeping
            const float dt = 1.0f / 60.0f;
            world_.update(player_.pos);
            lake_.update(dt, world_.terrain, player_.pos, forward());
            sawTears += int(lake_.tearsThisTick().size());
            simMs_ += double(dt) * 1000.0;
        }

        int stillHere = 0, wandered = 0, wept = 0;
        float moved = 0.0f;
        for (int b = 0; b < kBabyPerDuck; ++b) {
            Vec3 now{0, 0, 0};
            if (!wasLive[b]) continue;   // a slot refilled since -- not this brood
            if (!lake_.duckProbe(LakeLife::babyIndex(mom, b), &live, &orphan, &crying, &now))
                continue;
            if (!live) continue;
            ++stillHere;
            if (orphan) ++wept;
            const float d = sqrtf((now.x - was[b].x) * (now.x - was[b].x) +
                                  (now.z - was[b].z) * (now.z - was[b].z));
            if (d > moved) moved = d;
            if (d > 0.5f) ++wandered;
        }
        // AGAINST WHAT WAS STILL ALIVE, not against three: one of them was
        // shot a moment ago on purpose, and comparing to three would fail this
        // on correct behaviour.
        std::printf("  -- and then the mother --\n");
        std::printf("  still on the field   %d of %d   %s\n", stillHere, before,
                    stillHere == before ? "correct"
                                        : "THE BROOD VANISHED WITH HER -- WRONG");
        std::printf("  marked orphaned      %d of %d   %s\n", wept, before,
                    wept == stillHere && stillHere > 0 ? "correct"
                                                       : "STILL FOLLOWING A DEAD MOTHER -- WRONG");
        std::printf("  wandered off         %d of %d, furthest %.2f m   %s\n", wandered, before,
                    double(moved),
                    wandered > 0 ? "correct"
                                 : "THEY FROZE WHERE SHE DIED -- WRONG");
        std::printf("  tears shed           %d   %s\n", sawTears,
                    sawTears > 0 ? "correct" : "NOBODY CRIED -- WRONG");
        const bool ok = stillHere == kBabyPerDuck && wept == stillHere && wandered > 0 &&
                        sawTears > 0;
        std::printf("\n  %s\n", ok ? "PASS -- the brood outlived her, wandered, and wept."
                                     : "FAIL -- see the line above.");
        std::fflush(stdout);
    }

    void runKillTest() {
        std::printf("\n=== KILL TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        // AN AXE, so one blow is one kill and the test is about the death
        // rather than about counting to three. The three-blow path is checked
        // on its own below.
        // THE SAME TRAVEL THE HOE TEST MAKES, and for the same reason -- the
        // spawn can land in the sand now, and there is nothing there to hunt.
        // See the note over runHoeTest's copy.
        // -- ...UNLESS THE DESERT IS THE WHOLE WORLD (user 2026-09-21) -----
        //
        // THIS TEST COULD NOT SEE THE GECKO, AND THAT IS WHY IT WENT UNNOTICED.
        // The travel below exists because a spawn in the sand of a MIXED world
        // has almost nothing to hunt -- true, and the right call there. But it
        // ran unconditionally, so `--desert --kill-test` walked out of the one
        // world where gecko, cobra and scorpion exist and then reported
        // "SKIPPED": the three desert species have never been exercised by it.
        //
        // Exactly the hole the clip test had, found the same way -- by a
        // species being reported broken that the suite called fine.
        //
        // A FORCED WORLD IS THE SIGNAL, not the ground underfoot. `forced` says
        // the player asked for this band and there is nowhere else to go; the
        // old inner check asked "is it still sand after travelling", which is
        // the same question with the answer already decided.
        if (world_.terrain.desertAt(pos_.x) && !world_.terrain.forced) {
            std::printf("  spawned in the sand -- travelling to the oak: %s\n",
                        runCommand("/locate oak").c_str());
        }
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Wood) held_.select(i);
        // AND THE POPULATIONS HAVE TO HAVE RUN. The first version of this
        // primed the CHUNKS and then asked where the nearest bunny was, which
        // is a question about a population that had never ticked: every one of
        // the twenty-four answered "none", the loop skipped them all in
        // silence, and the report was a header with nothing under it.
        warmLife(player_.pos, 3);
        publishLife();
        std::printf("  the %s wood, steak slot %d\n",
                    world_.terrain.woodName(pos_.x), steakTool_);

        std::printf("\n  %-10s  %-5s  %-6s  %-6s  %-5s  %s\n", "species", "hits", "pieces",
                    "sparks", "smoke", "meat");
        int tried = 0, killed = 0, wrong = 0;
        for (const LifeName &ln : lifeNames()) {
            // THE HIVE AND THE LILY PAD ARE NOT ALIVE. One is decor in a crown
            // and the other is a leaf; neither answers the band's life table.
            //
            // AND NEITHER IS THE CROP, which is the third time this exception
            // has had to be made and the first one that cannot be forgotten:
            // the apple and the orange are flagged on the row rather than
            // named here, so a fourth fruit joins the skip by existing. An
            // apple is decor too -- it has no flyer slot, so shooting at it
            // would report "not drawn" about something that was never in the
            // band to begin with.
            if (ln.life == Life::Hive || ln.life == Life::LilyPad || ln.fruit) continue;
            Vec3 at{0, 0, 0};
            // NOT SILENT. A species this world has none of is an ordinary
            // outcome -- the frog is birch-only, the armadillo pine-only -- but
            // it is not the same outcome as a kill that went wrong, and a run
            // that skipped everything in silence is what this line is for.
            if (!nearestLife(ln.life, &at)) {
                std::printf("  %-10s  none in this wood\n", ln.name);
                continue;
            }
            teleportToLife(at, ln.stand);
            // STREAM THE GROUND, THEN LET THE POPULATION FIND IT. Both are
            // needed and in that order: an animal is placed against terrain
            // that has to exist first, and a population that has not ticked
            // since the jump still has its members back where you came from.
            for (int w = 0; w < 120; ++w) {
                world_.update(player_.pos);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            warmLife(player_.pos, 2);
            publishLife();
            // ...and find it again, because it has been walking while the
            // chunks streamed in.
            if (!nearestLife(ln.life, &at)) {
                std::printf("  %-10s  gone by the time the chunks landed\n", ln.name);
                continue;
            }
            // -- ...AND THEN WALK UP TO IT --------------------------------
            //
            // teleportToLife stands you at the species' OWN distance, which is
            // a number chosen so the animal does not bolt before you have seen
            // it (see the `stand` column) -- five metres for a bunny and ten
            // for a mouse. The melee reach is 5.3 m, so half the table was out
            // of range before the first swing and reported "not drawn" for a
            // reason that had nothing to do with the kill path.
            //
            // NO WARM AFTER THIS, deliberately: the animal is where the line
            // above found it, and ticking the populations again would move it
            // while this closes the distance. The publish is so the BAND knows
            // where the player is standing -- see flyerAt.
            {
                const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(at.x + dx / d * 1.6f, at.z + dz / d * 1.6f);
                publishLife();
            }
            // AIM AT IT. Everything downstream reads forward(), so this is the
            // whole of "look at the animal" -- the same trick --hoe-test uses
            // to stop its ray landing somewhere it never approved.
            pos_ = player_.eyePosition();
            const Vec3 d = normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
            yaw_ = atan2f(d.x, -d.z) * 180.0f / PI;
            pitch_ = asinf(d.y) * 180.0f / PI;
            const int slot = lifeHits_.aim(world_, pos_, forward());
            if (slot < 0) {
                // WHY, rather than that it happened. The two reasons are not
                // the same bug: an animal out of REACH is this test standing in
                // the wrong place, and one not DRAWN is a population that
                // published nothing into the band -- which would be a real
                // fault in the thing being tested.
                const float dxq = at.x - pos_.x, dyq = at.y - pos_.y, dzq = at.z - pos_.z;
                int drawn = 0;
                for (int q = 0; q < kFlyerInstances; ++q)
                    if (lifeAtSlot(q).alive() && world_.flyerAt(q, nullptr, nullptr)) ++drawn;
                std::printf("  %-10s  no aim: it is %.1f m off (%.1f m up), %d animals drawn\n",
                            ln.name, std::sqrt(dxq * dxq + dzq * dzq), dyq, drawn);
                continue;
            }
            const LifeKind kind = lifeKindAt(slot);
            ++tried;
            // AN EMPTY FIELD BEFORE EACH ONE -- see Drops::clearAll and
            // World::clearDebris. Both pools are small and a corpse fills a
            // good part of one, so without this the later species in the table
            // report a shatter and a steak that the pools simply had no room
            // for -- which reads exactly like the feature not working.
            drops_.clearAll();
            world_.clearDebris(physics_);
            const int drops0 = drops_.count();
            int hits = 0;
            bool died = false;
            for (int k = 0; k < 6 && !died; ++k) {
                ++hits;
                if (!strikeLife()) break;
                died = !lifeHits_.flashing(slot) || lifeHits_.dying(slot);
                // A NEW SWING EACH TIME. One swing lands one hit -- v1 guards
                // that with a token; here the guard is that this test is the
                // only thing swinging.
            }
            const int sparks = particles_.live();
            int smoke = 0;
            for (int q = 0; q < kParticleSlots; ++q) {
                Vec3 pp{0, 0, 0};
                bool sm = false;
                if (particles_.at(q, &pp, &sm, simMs_) && sm) ++smoke;
            }
            const int got = drops_.count() - drops0;
            const bool meatRight = kind.meat ? (got > 0) : (got == 0);
            // -- ...AND A FISH'S STEAK HAS TO BE ON THE LAKE ----------------
            //
            // (user 2026-09-15, twice: "have the raw steak float above the
            // water ... have the steak appear above the water floating, not in
            // it".)
            //
            // THE FIRST FIX WAS INVISIBLE TO EVERY TEST THERE WAS, which is why
            // it shipped wrong: `got > 0` is true whether the meat is bobbing
            // on the surface or lying on the seabed four metres under it. The
            // steak was being RELEASED at the waterline and then flying on down
            // to the terrain, because the arc's terminator had never heard of
            // water -- so the only question worth asking is this one, and the
            // drop has to be settled before it is asked.
            char meatWhere[48] = {0};
            if (kind.meat && got > 0) {
                Vec3 dp{0, 0, 0};
                bool flying = true;
                for (int f = 0; f < 240 && drops_.newestDrop(&dp, &flying) && flying; ++f)
                    drops_.update(1.0f / 60.0f, walkWorld(), player_.pos, pos_);
                if (drops_.newestDrop(&dp, &flying)) {
                    const float wtop = waterTopAt(dp.x, dp.z);
                    if (wtop > -1e8f && wtop > at.y - 1.0f)
                        std::snprintf(meatWhere, sizeof(meatWhere), "  %+.2f m vs the water %s",
                                      double(dp.y - wtop),
                                      dp.y >= wtop ? "-- afloat" : "-- SUNK, WRONG");
                }
            }
            if (died) ++killed;
            if (!died || !meatRight) ++wrong;
            std::printf("  %-10s  %-5d  %-6d  %-6d  %-5d  %s%s\n", kind.name ? kind.name : ln.name,
                        hits, lastPieces_, sparks - smoke, smoke,
                        kind.meat ? (got > 0 ? "steak" : "NONE -- WRONG")
                                  : (got == 0 ? "none, correct" : "A STEAK -- WRONG"),
                        died ? (meatWhere[0] ? meatWhere : "") : "   IT DID NOT DIE");
            particles_.update(simMs_ + 4000.0);   // clear the air before the next one
        }
        // ---- ...AND A SHAFT STOPS AT A BOULDER --------------------------
        //
        // (user 2026-09-15: "arrows are clipping through big rocks. dont let
        // arrows go through anything.")
        //
        // FIRED FROM WELL BACK, which is the whole point: inside the walk's own
        // six metres the arrow always did stop, so a test that stood close
        // would have passed against the bug. This stands 25 m off and asks
        // whether the shaft is still travelling when it is past the rock.
        bool rockTried = false, rockStopped = false;
        {
            std::vector<Solid> around;
            world_.collidersNear(player_.pos, 120.0f, &around);
            const Solid *big = nullptr;
            float bestR = 0.0f;
            for (const Solid &q : around) {
                if (q.modelKind != 1 || !q.vol) continue;   // rocks only
                const float r = 0.5f * (float(q.msx) + float(q.msz)) * VOXEL_M;
                if (r > bestR) { bestR = r; big = &q; }
            }
            if (big && bestR > 0.8f) {
                const Solid rock = *big;
                rockTried = true;
                lastChipN_ = 0;   // so the report is THIS shot's, not an older one's
                // Stand off along +x and aim at the rock's middle.
                // THE UPPER HALF, NOT THE MIDDLE. A flat shot at the centre
                // of a boulder 25 m away is a shot that spends 25 m falling
                // under kArrowG, and on ground that rises even slightly it
                // buries itself at the archer's own feet -- measured: the chip
                // came out 0.5 m from the player, which then absorbed exactly as
                // the rule says it should and read as the rule being broken.
                // LEVEL WITH THE ARCHER'S EYE, clamped into the rock. Aiming
                // at a fixed fraction of the boulder's height aims DOWNWARD
                // whenever the player is standing above it -- which on this
                // terrain is most of the time -- and a downward shot from six
                // metres buries itself in the ground in front of them. Measured
                // at 80% of the rock's height: still cut 2.3 m from the archer.
                //
                // A LEVEL SHOT HAS NO GROUND TO HIT. The eye is 1.6 m over the
                // feet, the boulder is metres tall, and the clamp only matters
                // for a rock shorter than the archer -- which bestR > 0.8 has
                // already excluded.
                const float rockTop = rock.baseY + float(rock.vsy) * VOXEL_M;
                const float ry = maxf(rock.baseY + 0.25f * float(rock.vsy) * VOXEL_M,
                                      minf(pos_.y, rockTop - 0.3f));
                // -- SIX METRES CLEAR OF THE FACE, NOT 25 FROM THE CENTRE --
                //
                // A fixed 25 m was a fixed distance from a boulder whose RADIUS
                // varies from one to eight metres, so how far the shaft actually
                // had to fly changed with whichever rock the world happened to
                // offer -- and on any ground that rises between, it buried
                // itself at the archer's feet instead. Measured twice, at a
                // cut 0.4 m away.
                //
                // What the two rules under test need is only that the target is
                // struck and that the chip lands FAR BEYOND the 1.6 m absorb
                // reach. Six metres of clear air proves both and gives gravity
                // almost nothing to work with: kArrowSpeed covers it in an
                // eighth of a second.
                const float standOff = bestR + 6.0f;
                // -- EIGHT BEARINGS, AND THE FIRST WITH CLEAR AIR WINS --------
                //
                // ONE BEARING IS A COIN TOSS IN A WOOD. The shot was fired from
                // +x every time, so whatever stood between the archer and the
                // stone on that one line decided the test: measured, the shaft
                // stopped 2.4 m out and the chip it cut was then inside the
                // player's own absorb reach -- which is the rule WORKING, and it
                // read as the rule failing. The oaks made it likelier, one being
                // seventeen metres across, but a birch or a second boulder was
                // always just as capable of standing in the way.
                //
                // So the archer walks round the rock. A chip that lands well
                // clear of them is a shot that actually reached the stone, and
                // that is the only thing the two rules below need.
                const WalkWorld aw = wideWalkWorld(kArrowSolidsM);
                for (int bear = 0; bear < 8; ++bear) {
                    const float ang = float(bear) * 0.7853982f;
                    teleportTo(rock.cx + sinf(ang) * standOff, rock.cz + cosf(ang) * standOff);
                    pos_ = player_.eyePosition();
                    lastChipN_ = 0;
                    // So a bearing that cuts nothing at all reads as SHORT
                    // rather than inheriting the last one's cut.
                    lastChipAt_ = pos_;
                    const Vec3 aim =
                        normalize(Vec3(rock.cx - pos_.x, ry - pos_.y, rock.cz - pos_.z));
                    arrows_.launch(pos_, Vec3(aim.x * kArrowSpeed, aim.y * kArrowSpeed,
                                              aim.z * kArrowSpeed));
                    for (int f = 0; f < 120 && arrows_.inFlight() > 0; ++f) {
                        arrows_.update(1.0f / 60.0f, aw, nullptr);
                        // THE CHIP IS DRAINED HERE. This loop drives the flight
                        // itself rather than going through the frame, so it has
                        // to do the frame's job: impactsThisTick is cleared on
                        // the NEXT update, so a drain outside this loop would
                        // find nothing however well the carve worked. Which is
                        // exactly what it reported -- 0 voxels off a boulder it
                        // had demonstrably just stopped dead in.
                        for (const Arrows::Impact &im : arrows_.impactsThisTick())
                            arrowChip(im.at, im.dir);
                    }
                    const float cdx = lastChipAt_.x - pos_.x, cdz = lastChipAt_.z - pos_.z;
                    if (lastChipN_ > 0 && cdx * cdx + cdz * cdz > 25.0f) break;
                }
                // WHERE IT CAME TO REST, against the rock's own centre. A shaft
                // that stopped is within a metre or two of the stone; one that
                // went through is metres PAST it, on the far side.
                float stoppedAt = 1e9f;
                for (const Vec3 &q : arrows_.landedThisTick())
                    stoppedAt = minf(stoppedAt, q.x - rock.cx);
                rockStopped = arrows_.inFlight() == 0;
                std::printf("\n  -- a shaft at a %.1f m boulder, from %.1f m --\n",
                            double(bestR * 2.0f), double(standOff));
                std::printf("  still flying after 2 s: %s\n",
                            arrows_.inFlight() ? "YES -- IT WENT THROUGH" : "no, it stopped");
                // ...AND WHAT IT TOOK OUT OF IT (user 2026-09-15: "have arrow
                // take out tiny peices of material ... as big as the knife
                // chunk as seen in v1"). v1's knife lifts 8 voxels; a radius-1
                // sphere is the 7 cells within one of the centre, which is the
                // nearest this engine can cut -- see kArrowChipVox. An AXE
                // takes 113 from the same boulder, so the pair of numbers is
                // what says "chips, does not dig".
                std::printf("  it chipped out %d voxels  %s\n", lastChipN_,
                            lastChipN_ <= 0 ? "NOTHING CAME OUT -- WRONG"
                            : lastChipN_ > 24 ? "THAT IS A DIG, NOT A CHIP -- WRONG"
                                              : "a chip, about the knife's own");
                // -- ...AND IT DOES NOT COME TO YOU FROM OVER THERE ---------
                //
                // (user 2026-09-15: "dont have the chipped chunk caused by the
                // arrow get absorbed by the player. only when the player is
                // close enough to absorbe the chunk".)
                //
                // THE PLAYER HAS NOT MOVED and is still the 25 m back they
                // fired from, so a chip that arrives is a chip that crossed the
                // clearing. Three seconds is well past kAbsorbWaitMs and past
                // the whole kAbsorbFlyMs curve, so if it were coming it would
                // have arrived.
                // THE CHIP ITSELF, NOT THE POOL. The first cut of this
                // counted looseCount() before and after, which is every loose
                // body in the world -- and by this point the run has killed
                // fifteen animals and left their pieces lying about. Two of
                // them were collected during the three seconds, the count fell,
                // and the test reported that the arrow's chip had flown across
                // the clearing when the chip had not moved at all. A pool total
                // cannot answer a question about one body in it.
                const int chipSlot = lastChipSlot_;
                {
                    const WalkWorld cw = walkWorld();
                    for (int f = 0; f < 180; ++f) {
                        physics_.step(1.0f / 60.0f);
                        simMs_ += 1000.0 / 60.0;
                        world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                            [&](float ax, float az) {
                                                return walkGroundM(cw, ax, az);
                                            });
                    }
                }
                // -- FOUND BY WHERE IT IS, NOT BY WHICH SLOT IT TOOK -----
                //
                // The slot is not an identity. kDebrisInstances is 64 and this
                // test kills eighteen animals before it fires the arrow, so the
                // pool has wrapped several times over by now and spawnDebris
                // hands out the OLDEST slot once it is full. Asked by index, the
                // chip read as "gone" while it was lying exactly where it fell
                // -- a failure of the question, not of the engine.
                //
                // So the pool is scanned for a body still near where the chip
                // was cut out. A chip that has not moved is within a metre of
                // that point; one that came to the player is twenty-five metres
                // away and is not.
                Vec3 cp{0, 0, 0};
                float cq[4] = {0, 0, 0, 1};
                bool stillThere = false, coming = false;
                for (int b = 0; b < kDebrisInstances; ++b) {
                    Vec3 bp{0, 0, 0};
                    float bq[4] = {0, 0, 0, 1};
                    if (!world_.debrisPose(b, &bp, bq)) continue;
                    const float dx = bp.x - lastChipAt_.x, dz = bp.z - lastChipAt_.z;
                    if (dx * dx + dz * dz > 4.0f) continue;   // two metres of the cut
                    stillThere = true;
                    cp = bp;
                    coming = world_.debrisAbsorbing(b);
                    break;
                }
                (void)chipSlot;
                // -- A SHOT THAT FELL SHORT PROVES NOTHING ---------------
                //
                // The rule under test is "a chip cut across the clearing does
                // not come to you". If the shaft buried itself at the archer's
                // feet then the chip was never across the clearing, and it
                // absorbing is the rule WORKING rather than failing. Measured
                // once as a flat WRONG before this guard existed, on a shot cut
                // 0.5 m from the player.
                const float cutAway =
                    std::sqrt((lastChipAt_.x - pos_.x) * (lastChipAt_.x - pos_.x) +
                              (lastChipAt_.z - pos_.z) * (lastChipAt_.z - pos_.z));
                const float away =
                    stillThere ? std::sqrt((cp.x - pos_.x) * (cp.x - pos_.x) +
                                           (cp.z - pos_.z) * (cp.z - pos_.z))
                               : 0.0f;
                if (cutAway < 5.0f) {
                    std::printf("  the shaft fell short -- it cut %.1f m from the archer, so "
                                "there is no reach rule to test here\n", double(cutAway));
                } else {
                    std::printf("  after 3 s, standing off: %s  %s\n",
                                !stillThere ? "the chip is gone"
                                : coming    ? "the chip is on its way to you"
                                            : "the chip is still lying there",
                                (stillThere && !coming && away > 5.0f)
                                    ? "correct, it waits to be walked up to"
                                    : "IT CAME TO THE PLAYER -- WRONG");
                }
                if (stillThere && cutAway >= 5.0f)
                    std::printf("  ...and it is %.1f m off, where it was knocked loose\n",
                                double(away));
            }
        }
        if (!rockTried) std::printf("\n  no boulder in reach to shoot at\n");

        // ---- ...AND A SHAFT KILLS IN ONE --------------------------------
        //
        // (user 2026-09-15: "have the arrow able to kill life in one shot, just
        // like in v1".) Launched rather than shot from the bow: what is being
        // checked is the SHAFT meeting an animal, and the draw, the loose and
        // the kit slot are a different path with their own test.
        bool arrowKilled = false, arrowTried = false;
        Vec3 arrowStop{1e9f, 1e9f, 1e9f}, arrowAt{0, 0, 0};
        // Where the shaft was when it first took hold of a piece, and where it
        // is now. The distance between them is the whole measurement.
        Vec3 arrowRide0{0, 0, 0}, arrowRideNow{0, 0, 0};
        int arrowRode = 0;
        // "IT NEVER TOOK HOLD" AND "IT TOOK HOLD AND THE PIECE THEN WENT" are
        // different answers and the count alone cannot tell them apart -- one
        // is a broken ride, the other is the ride working and v1's rule
        // retiring the shaft with the piece.
        bool arrowRodeEver = false;
        int arrowRideLoose = 0;
        {
            Vec3 at{0, 0, 0};
            if (arrows_.ready() &&
                (nearestLife(Life::Bunny, &at) || nearestLife(Life::Skunk, &at) ||
                 nearestLife(Life::Duck, &at))) {
                const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(at.x + dx / d * 6.0f, at.z + dz / d * 6.0f);
                publishLife();
                pos_ = player_.eyePosition();
                const int slot = lifeHits_.at(world_, at);
                if (slot >= 0) {
                    arrowTried = true;
                    // STRAIGHT AT IT, at the bow's own speed.
                    Vec3 dir = normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
                    arrows_.launch(pos_, Vec3(dir.x * 40.0f, dir.y * 40.0f, dir.z * 40.0f));
                    // THE POPULATION, NOT THE BAND. killLifeAt retires the
                    // member, but its instance keeps whatever the last publish
                    // left in it -- so "still drawn" is not "still alive", and
                    // asking the band here reports a survivor every time.
                    const int loose0 = world_.looseCount();
                    for (int f = 0; f < 30 && !arrowKilled; ++f) {
                        arrows_.update(1.0f / 60.0f, walkWorld(),
                                       [this](const Vec3 &qa, const Vec3 &qb) { return arrowKill(qa, qb); });
                        // WHERE IT STOPPED, IF IT STOPPED SHORT -- captured in
                        // the loop because landedThisTick is cleared by the
                        // next update. A survivor is two different faults and
                        // this is the line that tells them apart: a shaft that
                        // buried itself in a rise between the archer and the
                        // animal is the TEST standing in the wrong place, and
                        // one that reached the animal and did nothing is the
                        // kill rule. Without it the terrain moving under this
                        // check reads exactly like the rule breaking, which it
                        // did on 2026-09-17 when the puddle fill landed.
                        for (const Vec3 &q : arrows_.landedThisTick()) arrowStop = q;
                        // THE SAME TWO CALLS THE FRAME MAKES, in the same
                        // order -- see the note beside them in app_frame.inl.
                        // Without these the test exercises the kill and not the
                        // thing the kill is for.
                        arrows_.attachRides(
                            [this](const Vec3 &pp, double *g, Vec3 *bp, float *bq) {
                                const int bi = world_.bodyNear(pp, 0.5f, g);
                                if (bi >= 0) world_.bodyPose(bi, *g, bp, bq);
                                return bi;
                            });
                        arrows_.rideStep([this](int bi, double g, Vec3 *bp, float *bq) {
                            return world_.bodyPose(bi, g, bp, bq);
                        });
                        // ...and the pieces have to actually FALL, or a shaft
                        // riding one is carried nowhere and proves nothing.
                        stepLoose(1.0f / 60.0f);
                        if (arrowRode == 0 && arrows_.riding(&arrowRide0) > 0)
                            arrowRideNow = arrowRide0;
                        arrowRode = arrows_.riding(&arrowRideNow);
                        if (arrowRode > 0) arrowRodeEver = true;
                        publishLife();
                        if (!world_.flyerAt(slot, nullptr, nullptr) ||
                            world_.looseCount() > loose0)
                            arrowKilled = true;
                    }
                    // -- ...AND THEN LET THE PIECES FALL ------------------
                    //
                    // The loop above STOPS ON THE KILL, so on its own it
                    // measures the carry over zero frames and reports 0.00 m
                    // whatever the ride does -- which is a broken measurement
                    // and not a broken feature. These are the frames in which
                    // the body actually drops.
                    for (int f = 0; f < 90 && arrowRode > 0; ++f) {
                        arrows_.rideStep([this](int bi, double g, Vec3 *bp, float *bq) {
                            return world_.bodyPose(bi, g, bp, bq);
                        });
                        stepLoose(1.0f / 60.0f);
                        arrowRode = arrows_.riding(&arrowRideNow);
                        arrowRideLoose = world_.looseCount();
                    }
                    arrowAt = at;
                }
            }
        }
        std::printf("\n  -- a shaft, which v1 says kills outright --\n");
        if (!arrowTried)
            std::printf("  nothing in reach to shoot -- not exercised\n");
        else if (arrowKilled)
            std::printf("  one arrow: it went down, correct\n");
        else if (arrowStop.x < 1e8f)
            // THE SHAFT NEVER GOT THERE. Not a verdict on the kill rule.
            std::printf("  one arrow: it stopped %.1f m short of the animal -- the shot was "
                        "blocked, not refused\n",
                        double(std::sqrt((arrowStop.x - arrowAt.x) * (arrowStop.x - arrowAt.x) +
                                         (arrowStop.y - arrowAt.y) * (arrowStop.y - arrowAt.y) +
                                         (arrowStop.z - arrowAt.z) * (arrowStop.z - arrowAt.z))));
        else
            std::printf("  one arrow: IT SURVIVED -- WRONG\n");
        // -- ...AND DID THE SHAFT GO WITH IT -------------------------------
        if (arrowTried) {
            if (!arrowRodeEver)
                std::printf("  the shaft: rode nothing -- left standing where the animal "
                            "was\n");
            else if (arrowRode <= 0)
                std::printf("  the shaft: took hold, then went with the piece (%d loose "
                            "left) -- v1's rule\n",
                            arrowRideLoose);
            else
                std::printf("  the shaft: riding %d piece(s), carried %.2f m from where it "
                            "struck\n",
                            arrowRode,
                            double(std::sqrt((arrowRideNow.x - arrowRide0.x) *
                                                 (arrowRideNow.x - arrowRide0.x) +
                                             (arrowRideNow.y - arrowRide0.y) *
                                                 (arrowRideNow.y - arrowRide0.y) +
                                             (arrowRideNow.z - arrowRide0.z) *
                                                 (arrowRideNow.z - arrowRide0.z))));
        }

        // ---- ...AND THE THREE-BLOW RULE, WHICH THE AXE HIDES -------------
        //
        // Everything above swings an AXE, because that is how a test kills one
        // of twenty-four species without spending a minute on each -- and it
        // means every line of it reports "1 hit", which proves nothing about
        // v1's HITS_TO_KILL. So this does one more kill with a PICK: three
        // blows, flashing after each, and dead on the third and not before.
        int need = 0, gotHits = 0;
        bool needRight = false;
        {
            for (int i = 0; i < held_.count(); ++i)
                if (held_.tool(i).takes == Takes::Stone) held_.select(i);
            Vec3 at{0, 0, 0};
            // A BUNNY: not frail, so it is the case the rule is about. Any of
            // the marchers would do; the bunny is simply the most common.
            if (nearestLife(Life::Bunny, &at)) {
                teleportToLife(at, 5.0f);
                for (int w = 0; w < 120; ++w) {
                    world_.update(player_.pos);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                warmLife(player_.pos, 2);
                publishLife();
                if (nearestLife(Life::Bunny, &at)) {
                    const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                    const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                    teleportTo(at.x + dx / d * 1.6f, at.z + dz / d * 1.6f);
                    publishLife();
                    pos_ = player_.eyePosition();
                    const Vec3 look =
                        normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
                    yaw_ = atan2f(look.x, -look.z) * 180.0f / PI;
                    pitch_ = asinf(look.y) * 180.0f / PI;
                    const int slot = lifeHits_.aim(world_, pos_, forward());
                    if (slot >= 0) {
                        need = kHitsToKill;
                        for (int k = 0; k < 5; ++k) {
                            if (!strikeLife()) break;
                            ++gotHits;
                            if (lifeHits_.dying(slot)) break;
                            // ...AND IT IS STILL ALIVE AND STILL FLASHING,
                            // which is the other half of "every hit flashes".
                            if (!lifeHits_.flashing(slot)) break;
                        }
                        needRight = gotHits == need;
                    }
                }
            }
        }
        // ---- ...AND THE CORPSE DOES NOT LIE THERE -----------------------
        //
        // (user 2026-09-15: "it should stay red while it breaks into peices,
        // then it should dissapear completely as it drops the raw steak".)
        //
        // THE PIECES GO WHEN THE RED DOES. They are spawned at the killing blow
        // and carry the flash for kHurtMs; after that they are a heap of grey
        // lumps where an animal was, and v1 removes them with the poof. Checked
        // by running the clock past the flash and counting what is left.
        {
            // THE AXE, EXPLICITLY. This block sits after the one that selects
            // the PICK to count v1's three blows, so it inherited it -- and a
            // pick's first swing is a WOUND. The strike still returned true
            // (it landed), lastPieces_ still held 7 from the kill above, and
            // the check reported a corpse that was never made. A test that
            // reads a member set by somebody else is a test of nothing.
            for (int i = 0; i < held_.count(); ++i)
                if (held_.tool(i).takes == Takes::Wood) held_.select(i);
            world_.clearDebris(physics_);
            Vec3 at{0, 0, 0};
            int before = 0, after = 0;
            bool did = false;
            // The deepest any piece got below the ground under it, over the
            // whole fall. A corpse that lands is a small negative or a few
            // centimetres; one that goes through the floor is metres.
            float sank = -1e9f;
            // ...AND HOW HIGH ANY OF THEM GOT ABOVE WHERE IT WAS BORN. A piece
            // thrown clear rises a few tens of centimetres; one that started
            // inside the ground and was depenetrated leaves like a rocket, and
            // that is a different number entirely.
            float rose = 0.0f;
            float bornY = 0.0f;
            int redN = 0, plainN = 0;
            // A BUNNY OR A MARCHER -- something big enough that its octants
            // hold more than one voxel. A fly would pass this test by having no
            // corpse to leave.
            if (nearestLife(Life::Bunny, &at) || nearestLife(Life::Skunk, &at) ||
                nearestLife(Life::Mouse, &at)) {
                const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(at.x + dx / d * 1.6f, at.z + dz / d * 1.6f);
                publishLife();
                pos_ = player_.eyePosition();
                const Vec3 look = normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
                yaw_ = atan2f(look.x, -look.z) * 180.0f / PI;
                pitch_ = asinf(look.y) * 180.0f / PI;
                // ...AND IT HAS TO HAVE ACTUALLY DIED. The axe kills in one,
                // but saying so here is what stops this drifting again.
                const int slotC = lifeHits_.aim(world_, pos_, forward());
                (void)slotC;
                if (slotC >= 0 && strikeLife() && lastKilled_) {
                    // THE SHATTER'S OWN COUNT, not the pool's. looseCount can
                    // read zero for an animal too small to make a piece -- an
                    // ant's octants are one voxel each and spawnDebris refuses
                    // those -- and "0 pieces before, 0 after" is a green tick on
                    // a kill that never had a corpse. Same vacuous shape the
                    // flower check had before it was made to go looking.
                    before = lastPieces_;
                    bornY = at.y;
                    world_.debrisKinds(&redN, &plainN);
                    std::printf("  (shatter said %d, the pool holds %d)\n", lastPieces_,
                                world_.looseCount());
                    // Past the flash, on the frame loop's own terms.
                    const float dt = 1.0f / 60.0f;
                    // -- ...AND HOW FAR UNDER THE GROUND ANY OF THEM GOT --
                    //
                    // (user 2026-09-15: "when the life dies and turns red, it
                    // clips straight through the ground".)
                    //
                    // The count going to zero is not enough on its own: a piece
                    // that falls through the world and is then retired reads
                    // exactly like one that landed and was cleared. So every
                    // frame, every live piece is measured against the ground of
                    // its own column and the worst is kept.
                    // LONG ENOUGH FOR THE CAP, not just for the usual case.
                    // A piece that comes to rest ON something -- a rock, a log
                    // -- never meets the terrain height its landing is measured
                    // against, and goes on the 2.5 s backstop instead. At 1.5 s
                    // the test saw that one still lying there and called the
                    // whole rule broken.
                    for (int f = 0; f < 200; ++f) {
                        // THE FLOOR THE SOLVER STANDS ON, MAINTAINED. The frame
                        // loop does this every tick and this test did not: it
                        // teleports to the animal and then steps physics
                        // against a patch still centred where it came from, so
                        // there was no collision floor under the corpse at all
                        // and every piece fell through a world that was not
                        // there. The engine was never asked the question.
                        maybeRebuildGroundPatch();
                        physics_.step(dt);
                        simMs_ += double(dt) * 1000.0;
                        const WalkWorld ww = walkWorld();
                        world_.updateDebris(
                            physics_, player_.eyePosition(), simMs_,
                            [&](float ax, float az) { return walkGroundM(ww, ax, az); });
                        for (int b = 0; b < kDebrisInstances; ++b) {
                            Vec3 bp{0, 0, 0};
                            float bq[4] = {0, 0, 0, 1};
                            if (!world_.debrisPose(b, &bp, bq)) continue;
                            const float g = walkGroundM(ww, bp.x, bp.z);
                            if (g - bp.y > sank) sank = g - bp.y;
                            if (bp.y - bornY > rose) rose = bp.y - bornY;
                        }
                    }
                    after = world_.looseCount();
                    did = true;
                }
            }
            std::printf("\n  -- the corpse, half a second later --\n");
            if (!did)
                std::printf("  nothing in reach to kill -- not exercised\n");
            else if (before == 0)
                std::printf("  it broke into no pieces at all -- nothing to watch vanish\n");
            else
                std::printf("  pieces at the blow %d, after the red ran out %d  %s\n", before,
                            after,
                            after == 0 ? "gone, correct" : "STILL LYING THERE -- WRONG");
                // 0.2 m, NOT 0.5. A chip is a few voxels: resting ON the
                // ground puts its centre ABOVE the surface, so anything
                // meaningfully positive here is buried. Half a metre passed a
                // corpse that was 0.42 m inside a hillside and called it
                // correct, which is a threshold chosen to make a test go green.
                std::printf("  deepest under the ground %.2f m  %s\n", double(sank),
                            sank < 0.2f ? "rested on it, correct"
                                        : "IT SANK INTO THE GROUND -- WRONG");
                // 2.5 m, not 2.0: the pop was raised to 3.8-5.1 m/s on
                // 2026-09-15 ("have the life pop up more upon death"), which is
                // a rise of up to 1.32 m by itself, and a piece thrown off a
                // bank starts higher than the kill point. The number this is
                // really watching for is the depenetration rocket, and that
                // cleared SEVEN metres.
                std::printf("  highest above the kill   %.2f m  %s\n", double(rose),
                            rose < 2.5f ? "thrown, not fired"
                                        : "A PIECE WAS LAUNCHED -- WRONG");
                // "THERE SHOULD ONLY BE THE BROKEN UP RED PEICES." Counted
                // apart, because a corpse piece and a chip look the same in a
                // total: red ones are scenery and carry hurtT0, and anything
                // else in the pool at a kill is a piece of debris the player
                // can walk up and absorb.
                std::printf("  the pool holds %d red, %d plain  %s\n", redN, plainN,
                            plainN == 0 ? "only the corpse, correct"
                                        : "THERE ARE ORDINARY PIECES TOO -- WRONG");
        }

        std::printf("\n  -- and with a pick, which does not kill outright --\n");
        if (!need)
            std::printf("  no bunny in reach; the three-blow rule was not exercised\n");
        else
            std::printf("  a bunny took %d blows, v1 says %d  %s\n", gotHits, need,
                        needRight ? "correct" : "WRONG");

        std::printf("\n  %d species tried, %d died, %d wrong\n", tried, killed, wrong);
        std::printf("  %s\n", (tried > 0 && wrong == 0 && (!need || needRight))
                                   ? "PASS -- every blow landed, every carcass was right."
                                   : "FAIL -- see the lines above.");
    }


    // -----------------------------------------------------------------------
    // WHAT A BITE OF DIRT DOES THAT A BITE OF STONE DOES NOT.
    //
    // (user 2026-09-14: "digging dirt voxels is very glitchy. just match the
    // pick to stone mechanics, but for shovel to dirt.")
    //
    // THE TWO TOOLS ALREADY SHARE EVERY LINE OF THE EDIT. toolTakes lets the
    // pick at stone and the shovel at soil, and both arrive at the same
    // World::dig with the same radius -- so "match the pick's mechanics" cannot
    // be done by copying a path, because it is already the same path. Whatever
    // differs is in what the bite MEETS, and that is a measurement.
    //
    // Twelve bites into each, reported side by side: how much came out, how
    // many chunks had to be re-meshed, and -- the one that is not symmetric --
    // how many loose BODIES the bite cut free of the world.
    // -----------------------------------------------------------------------
    void runSoilTest() {
        std::printf("\n=== SOIL TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        const float x = player_.pos.x, z = player_.pos.z;
        TerrainMemo memo;
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        const int h = world_.terrainTopAt(ci, cj, memo);
        const auto matAt = [&](int i, int j, int y) {
            TerrainProbe pr(&world_.terrain, &world_.editStore());
            return pr.material(i, j, y);
        };
        std::printf("  standing at (%.0f, %.0f), surface voxel %d\n", x, z, h);
        // WHERE THE CRUST ENDS, which is the whole geometry of the question: a
        // shovel works in a layer a few voxels thick over stone, and a pick
        // works inside a solid that goes down for ever.
        int crust = 0;
        for (int y = h; y > h - 40; --y) {
            const uint8_t m = matAt(ci, cj, y);
            if (isStoneMat(m) || m == mat::BEDROCK) break;
            ++crust;
        }
        std::printf("  the crust here is %d voxels; a bite is %d across\n", crust,
                    kDigRadiusVox * 2 + 1);

        std::printf("\n  %-8s  %-6s  %-7s  %-7s  %s\n", "bite", "spoil", "chunks", "bodies",
                    "what it cut loose");
        for (int pass = 0; pass < 2; ++pass) {
            const bool soil = pass == 0;
            std::printf("  -- %s --\n", soil ? "SHOVEL into the dirt, from the surface down"
                                              : "PICK into the stone, well below it");
            // A FRESH COLUMN for each pass, so the second is not digging in the
            // first one's hole.
            const float bx = x + (soil ? 6.0f : -6.0f), bz = z;
            const int bi = int(std::floor(bx / VOXEL_M)), bj = int(std::floor(bz / VOXEL_M));
            TerrainMemo m2;
            const int top = world_.terrainTopAt(bi, bj, m2);
            int y = soil ? top : (top - crust - 8);
            world_.clearDebris(physics_);
            for (int k = 0; k < 12; ++k, y -= 2) {
                const int before = world_.looseCount();
                spoilN_ = 0;
                const size_t chunks =
                    world_.dig(Vec3(bx, (float(y) + 0.5f) * VOXEL_M, bz), kDigRadiusVox,
                               &spoilVol_, &spoilN_, &spoilAt_);
                int vox = 0;
                for (uint8_t v : spoilVol_)
                    if (v != mat::AIR) ++vox;
                const int bodies = world_.looseCount() - before;
                // ...and how big the biggest of them is, because one chip is
                // ordinary and a slab of hillside coming away is not.
                int biggest = 0;
                for (int b = 0; b < kDebrisInstances; ++b) {
                    const int n = world_.debrisVoxels(b);
                    if (n > biggest) biggest = n;
                }
                std::printf("  %-8d  %-6d  %-7zu  %-7d  %s\n", k + 1, vox, chunks, bodies,
                            bodies > 0 ? (biggest > 200 ? "A SLAB -- this is the glitch"
                                                        : "a chip")
                                       : "nothing");
            }
        }
        // ---- ...AND WHAT THE COLLISION FLOOR DID AROUND THE HOLE ---------
        //
        // THE OTHER HALF OF THE QUESTION, and the half a shovel shows and a
        // pick cannot. groundPatch dilates a dug sample onto its eight
        // neighbours so a narrow shaft has a floor a body can rest on rather
        // than a funnel it slides out of -- see the note there. The samples are
        // 0.4 m apart, so that widens the solver's crater to about 1.2 m, and
        // everything in this world that DIGS AT THE SURFACE leaves its spoil
        // lying inside that radius. A chip resting on ground that is plainly
        // still there, sinking into a hole the eye cannot see, is exactly what
        // "digging dirt is very glitchy" would look like.
        //
        // So: a chip is put down on UNDUG ground beside the pit and watched.
        {
            world_.clearDebris(physics_);
            const float bx = x + 6.0f, bz = z;
            TerrainMemo m3;
            const int bi = int(std::floor(bx / VOXEL_M)), bj = int(std::floor(bz / VOXEL_M));
            const float lip = float(world_.terrainTopAt(bi, bj, m3) + 1) * VOXEL_M;
            std::printf("\n  -- a chip set down beside the pit --\n");
            std::printf("  %-8s  %-9s  %-9s  %s\n", "away", "ground", "rested at", "verdict");
            for (int k = 0; k < 4; ++k) {
                const float away = 0.4f + float(k) * 0.4f;
                const float cx = bx + away, cz = bz;
                TerrainMemo m4;
                const int qi = int(std::floor(cx / VOXEL_M)), qj = int(std::floor(cz / VOXEL_M));
                const float g = float(world_.terrainTopAt(qi, qj, m4) + 1) * VOXEL_M;
                // A LUMP OF THE GROUND IT IS LYING ON, born just above it.
                spoilN_ = 3;
                spoilVol_.assign(27, mat::DIRT);
                const Vec3 still{0.0f, 0.0f, 0.0f};
                const int slot = world_.spawnDebris(physics_, spoilVol_, spoilN_,
                                                    Vec3(cx, g + 0.12f, cz), still, still, simMs_,
                                                    0.0f, nullptr, uint8_t(kDebrisSoil));
                if (slot < 0) continue;
                rebuildGroundPatch();
                const float dt = 1.0f / 60.0f;
                Vec3 pp{0, 0, 0};
                float qq[4] = {0, 0, 0, 1};
                // WHERE THE SOLVER LEFT IT, not where it ended up. Half a
                // second after it is born every small body is COLLECTED -- a
                // kinematic lerp to the player's eye, through anything in the
                // way -- so reading the last frame measures the pickup. The
                // shaft test learned this the same way. See debrisAbsorbing.
                float restY = pp.y;
                for (int f = 0; f < 90; ++f) {
                    maybeRebuildGroundPatch();
                    physics_.step(dt);
                    simMs_ += double(dt) * 1000.0;
                    const WalkWorld ww = walkWorld();
                    world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                        [&](float ax, float az) { return walkGroundM(ww, ax, az); });
                    if (!world_.debrisPose(slot, &pp, qq)) break;
                    if (world_.debrisAbsorbing(slot)) break;
                    restY = pp.y;
                }
                pp.y = restY;
                const float sank = g - pp.y;
                std::printf("  %-8.1f  %-9.2f  %-9.2f  %s\n", double(away), double(g),
                            double(pp.y),
                            sank > 0.25f ? "SANK INTO GROUND THAT IS STILL THERE -- WRONG"
                                         : "rested on the surface");
                world_.clearDebris(physics_);
            }
            (void)lip;
        }

        // ---- ...AND WHETHER A FLOWER OVER THE HOLE CAME DOWN WITH IT -----
        //
        // (user 2026-09-14: "flowers are still floating sometimes. investigate
        // and fix this.")
        //
        // THE SAME SWING, A DIFFERENT LIST. dropUndermined has been dropping
        // trees and rocks and caps since it was written, and it walks the
        // COLLIDERS -- so a flower, which is drawn and walked through and has
        // no collider at all, was never on any list the question was asked of.
        // This digs the ground out from under one and asks whether it is still
        // being drawn.
        {
            Vec3 spot{0, 0, 0};
            int had = 0;
            for (int r = 1; r < 240 && !had; ++r)
                for (int d = -r; d <= r && !had; ++d) {
                    const float o[4][2] = {{float(d), float(-r)}, {float(d), float(r)},
                                           {float(-r), float(d)}, {float(r), float(d)}};
                    for (int k = 0; k < 4 && !had; ++k) {
                        const Vec3 c(x + o[k][0] * VOXEL_M, 0.0f, z + o[k][1] * VOXEL_M);
                        const int m = world_.scatterShownNear(c, 0.25f);
                        if (!m) continue;
                        TerrainMemo fm;
                        const int fi = int(std::floor(c.x / VOXEL_M));
                        const int fj = int(std::floor(c.z / VOXEL_M));
                        spot = Vec3(c.x, float(world_.terrainTopAt(fi, fj, fm)) * VOXEL_M, c.z);
                        had = m;
                    }
                }
            std::printf("\n  -- a flower over a fresh hole --\n");
            if (!had) {
                std::printf("  no scatter in reach -- this run proved nothing\n");
            } else {
                world_.clearDebris(physics_);
                world_.undermineLog = true;
                const int loose0 = world_.looseCount();
                // STRAIGHT DOWN THROUGH THE COLUMN IT STANDS ON. One bite
                // takes the surface and the next three take what is under it,
                // so there is nothing left within the three voxels of slack
                // dropScatterUndermined allows for a slope.
                // -- WIDE ENOUGH TO ACTUALLY UNDERMINE WHAT IS COUNTED ----
                //
                // kDigRadiusVox is 3 voxels -- 0.3 m -- and the check below
                // counts anything within 0.25 m. Those two do not fit together:
                // a decor item 0.23 m away by its MID can stand on a voxel
                // column 0.32 m from the dig centre once both are quantised,
                // and a radius-3 sphere never touches it. Traced exactly that
                // way -- a mushroom held by material 13 one voxel down, on all
                // four bites, because its column (10762, 27258) is dx 1, dz 3
                // from the hole and 1 + 9 > 9.
                //
                // So the hole is dug wider than the radius that is counted,
                // rather than the test asking the engine to drop something it
                // was never undermining. Five voxels is 0.5 m against a 0.25 m
                // count, which leaves the whole counted neighbourhood inside
                // the bite with room for the quantisation.
                const int undermineR = kDigRadiusVox + 2;
                for (int k = 0; k < 4; ++k) {
                    const Vec3 at2(spot.x, spot.y - float(k) * 3.0f * VOXEL_M, spot.z);
                    world_.dig(at2, undermineR, &spoilVol_, &spoilN_, &spoilAt_);
                    world_.dropScatterUndermined(physics_, at2,
                                                 float(undermineR) * VOXEL_M + 0.4f, simMs_);
                }
                const int after = world_.scatterShownNear(spot, 0.25f);
                std::printf("  standing before  %d\n", had);
                std::printf("  still drawn      %d  %s\n", after,
                            after == 0 ? "it came down, correct"
                                       : "STILL IN THE AIR OVER A HOLE -- WRONG");
                std::printf("  bodies it left   %d\n", world_.looseCount() - loose0);
                world_.undermineLog = false;
            }
        }

        std::printf("\n  Read the BODIES column. A bite that cuts the world loose spawns one;\n");
        std::printf("  a bite fully inside a solid spawns none.\n");
    }

    void runFloatTest() {
        std::printf("\n=== UNDERMINE TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        std::printf("  spawn (%.1f, %.1f, %.1f)\n", player_.pos.x, player_.pos.y,
                    player_.pos.z);

        // Not named `near`; see the note in runFellTest.
        std::vector<Solid> around;
        world_.collidersNear(player_.pos, 120.0f, &around);
        int tried = 0, fell = 0, stood = 0;
        for (int kind = 0; kind <= 1; ++kind) {
            const Solid *pick = nullptr;
            float best = 1e30f;
            for (const Solid &s2 : around) {
                if (s2.modelKind != kind || !s2.vol || s2.hx <= 0.0f) continue;
                const float dx = s2.cx - player_.pos.x, dz = s2.cz - player_.pos.z;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; pick = &s2; }
            }
            if (!pick) {
                std::printf("  no %s within reach of the spawn\n", kind ? "rock" : "tree");
                continue;
            }
            const Solid so = *pick;
            ++tried;
            std::printf("\n  %s at (%.1f, %.1f, %.1f)  model %d  %d x %d voxels\n",
                        kind ? "rock" : "tree", so.tx, so.baseY, so.tz, int(so.modelIndex),
                        int(so.msx), int(so.msz));
            // The patch follows the player, and something about to fall needs a
            // floor -- the same lesson runFellTest paid for.
            player_.placeOnGround(walkWorld(), so.cx, so.cz);
            maybeRebuildGroundPatch();

            // ---- dig out every column it is standing on --------------------
            //
            // A metre down, with the real brush, one bite per column of the
            // model\'s own row zero. That is what a player levelling a spot does
            // and it is what left forty trees hanging in float_probe.
            long blows = 0;
            bool down = false;
            TerrainMemo digMemo;
            for (int mz = 0; mz < int(so.msz) && !down; ++mz)
                for (int mx = 0; mx < int(so.msx) && !down; ++mx) {
                    if (!solidVoxel(so, mx, 0, mz)) continue;
                    float wx = 0.0f, wz = 0.0f;
                    solidWorldSpace(so, (float(mx) + 0.5f) * VOXEL_M, (float(mz) + 0.5f) * VOXEL_M,
                                    &wx, &wz);
                    // FROM THE SURFACE DOWN, not from the model's base down.
                    // A placement is SUNK into the terrain, so the ground
                    // beside its row zero stands well above that row -- digging
                    // from the base leaves the shoulder in place and the model
                    // is still standing on it. The first run of this spent 128
                    // blows and moved nothing for exactly that reason.
                    const int ci = int(std::floor(wx / VOXEL_M));
                    const int cj = int(std::floor(wz / VOXEL_M));
                    const float topY =
                        float(world_.terrainTopAt(ci, cj, digMemo) + 1) * VOXEL_M;
                    for (float y = topY; y > so.baseY - 0.4f && !down; y -= 0.25f) {
                        const Vec3 p{wx, y, wz};
                        if (!world_.dig(p, kDigRadiusVox)) continue;
                        ++blows;
                        down = world_.dropUndermined(physics_, p, simMs_);
                    }
                }
            if (!down) {
                ++stood;
                std::printf("  FAIL -- %ld blows took the ground away and it is still there\n",
                            blows);
                continue;
            }
            ++fell;
            std::printf("  came down after %ld blows\n", blows);

            // ...and then it is a body like any other. Watch it settle.
            const float dt = 1.0f / 60.0f;
            for (int f = 0; f < 300; ++f) {
                if (!physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                    rebuildGroundPatch();
                physics_.step(dt);
                simMs_ += double(dt) * 1000.0;
                world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                    [&](float x, float z) {
                                        return float(world_.terrain.heightVox(
                                                         int(std::floor(x / VOXEL_M)),
                                                         int(std::floor(z / VOXEL_M))) +
                                                     1) *
                                               VOXEL_M;
                                    });
                if ((f % 60) != 0 && f != 299) continue;
                for (int i = 0; i < 512; ++i) {
                    Vec3 p{0, 0, 0}, lin{0, 0, 0}, ang{0, 0, 0};
                    float q[4] = {0, 0, 0, 1};
                    if (!world_.debrisPose(i, &p, q)) continue;
                    world_.debrisVel(physics_, i, &lin, &ang);
                    const float uy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
                    const float pitch =
                        acosf(uy < -1.0f ? -1.0f : (uy > 1.0f ? 1.0f : uy)) * 57.29578f;
                    std::printf("    %5.0f ms   y %7.2f   pitch %5.1fd   fall %5.2f m/s\n",
                                double(f) * dt * 1000.0, p.y, pitch, -lin.y);
                    break;
                }
            }
        }
        // ---- ...AND WHAT A BLOW ON A MODEL LEAVES BEHIND -----------------
        //
        // Asked of the ENGINE: real swings through World::carveModel, then a
        // per-voxel flood of the instance that took them, against the same
        // model as it was drawn. Six voxels a blow before dropModelHangers.
        std::printf("\n  --- a blow on a model ---\n");
        long modelBlows = 0, modelLeft = 0;
        for (int kind = 0; kind <= 1; ++kind) {
            for (const Solid &s2 : around) {
                if (s2.modelKind != kind || !s2.vol || s2.hx <= 0.0f) continue;
                const Solid so2 = s2;
                // ---------------------------------------------------------
                // FROM ONE SIDE, SWEEPING ACROSS THE CUT. Not round and round.
                //
                // This used to walk a full circle -- 45 degrees per blow, a
                // different face of the trunk every time -- and a tree cut like
                // that is never cut THROUGH. fellTree therefore never fired,
                // the loop never broke, and every blow counted the whole canopy
                // as "standing on nothing": a flat 31,450 voxels, which is the
                // number the note below this loop already warns about. The test
                // was measuring its own aim.
                //
                // runFellTest solves the same problem the same way and says
                // why: "every blow from the same point along the same ray eats
                // a TUNNEL through the wood, and a tunnel severs nothing", so a
                // player's aim wanders across the cut and this does too. One
                // side, one direction, a hand's width of wander.
                //
                // A ROCK IS UNAFFECTED. It is not severable, fellTree declines
                // it, and what this measures there -- chips left hanging in the
                // stone -- never depended on the angle.
                // ---------------------------------------------------------
                // SIXTY, WHICH IS runFellTest's BUDGET AND FOR ITS REASON.
                // 24 was enough for most trees and not for all of them, and a
                // tree that has been cut through but not felled reports its
                // whole canopy as hanging -- so the run-to-run variance in
                // which tree is nearest the spawn showed up as this test
                // passing and failing on alternate runs with nothing changed.
                // Measured: 0 voxels at a pinned spawn, 58,708 at a random one,
                // same binary.
                const float ang0 = 0.7853982f;   // one side, and it stays that side
                bool camedown = false;
                long kindLeft = 0;
                for (int b = 0; b < 60; ++b) {
                    const float wob = (float(b % 11) - 5.0f) * 0.12f;
                    const Vec3 eye{so2.cx + std::cos(ang0) * (so2.hx + 3.0f) - std::sin(ang0) * wob,
                                   so2.baseY + 1.2f + float(b % 3) * 0.1f,
                                   so2.cz + std::sin(ang0) * (so2.hz + 3.0f) + std::cos(ang0) * wob};
                    const Vec3 dir{-std::cos(ang0), 0.0f, -std::sin(ang0)};
                    if (!world_.carveModel(so2, eye, dir, 12.0f, kDigRadiusVox)) continue;
                    ++modelBlows;
                    // THE WHOLE SWING PATH, not half of it. carveModel alone
                    // leaves a severed limb sitting in the model, because it is
                    // fellTree that hands a piece that big to the solver -- and
                    // a test that skips it measures 31,450 voxels of its own
                    // omission, which is what the first run of this did.
                    if (world_.fellTree(physics_, so2, dir, simMs_)) { camedown = true; break; }
                    // INTO A LOCAL, and committed below only if this model
                    // actually came down -- see the note after the loop.
                    const long l = world_.looseVoxelsNow(so2);
                    if (l > kindLeft) kindLeft = l;
                }
                // ...AND IF IT NEVER CAME DOWN, SAY SO RATHER THAN COUNTING IT.
                // A tree still standing has its crown connected through its own
                // trunk; a tree that has been severed and refused a body has
                // the whole crown loose, and the number that produces is the
                // canopy's size, not a floating-geometry bug. Reporting it as
                // one is how this test cried wolf.
                if (kind == 0 && !camedown)
                    std::printf("  (the tree did not come down in 60 blows -- its crown is not"
                                " counted; see the note in this loop)\n");
                else if (kindLeft > modelLeft)
                    modelLeft = kindLeft;
                break;   // one of each kind is enough; the flood is the slow part
            }
        }
        std::printf("  %ld blows on a tree and a rock: worst %ld voxels left standing"
                    " on nothing\n",
                    modelBlows, modelLeft);
        // ---- WHAT A NON-ZERO HERE MEANS, AND WHAT IT DOES NOT -------------
        //
        // THE TWO FLOODS RUN AT DIFFERENT GRAIN, and that is the whole of it.
        // fellTree decides what has come away using COARSE cells
        // (kSeverCell); this counts FINE voxels. A coarse cell is solid if
        // any voxel in it is, so the coarse flood leaks across a one-voxel gap
        // the fine one stops at -- and a blow can therefore disconnect tens of
        // thousands of fine voxels while the sever test correctly reports that
        // nothing structural has come off.
        //
        // So this number is real -- those voxels ARE standing on nothing, and
        // nothing-floats says they should fall -- but it is a KNOWN GAP in the
        // grain of the sever test, not a regression in whatever was last
        // changed. It surfaces on some spawns and not others because it needs
        // a blow that cuts a fine bridge without cutting a coarse one.
        //
        // Closing it means running the sever flood at voxel grain, or coarse
        // first and fine inside the cells the coarse pass calls contested. It
        // is not a one-line fix and it has never been attempted.
        if (modelLeft > 0)
            std::printf("  (coarse sever vs fine flood -- see the note here before blaming\n"
                        "   whatever changed last)\n");

        // ---- ...AND THE GROUND ITSELF ------------------------------------
        //
        // Asked of the ENGINE, through the same terrainSolidAt every other
        // consumer uses, rather than of a reimplementation of it. Dig the way a
        // player digs, then flood the ground around the hole from what is
        // provably standing on bedrock and count what the flood cannot reach.
        std::printf("\n  --- the ground ---\n");
        world_.collidersNear(player_.pos, 120.0f, &around);   // the set moved on
        long sites = 0, siteFloat = 0, voxFloat = 0, worstSite = 0;
        double digNs = 0.0;
        long digN = 0;
        uint32_t rs = 20260909u;
        auto rnd = [&]() {
            rs ^= rs << 13;
            rs ^= rs >> 17;
            rs ^= rs << 5;
            return rs;
        };
        TerrainMemo tmemo;
        for (int site = 0; site < 40; ++site) {
            const float sx = player_.pos.x + float(int(rnd() % 400u)) - 200.0f;
            const float sz = player_.pos.z + float(int(rnd() % 400u)) - 200.0f;
            const int ci = int(std::floor(sx / VOXEL_M));
            const int cj = int(std::floor(sz / VOXEL_M));
            const int h = world_.terrain.heightVox(ci, cj, tmemo);
            // Thirty blows, clustered, exactly as float_probe's sweep is --
            // and TIMED, because dropTerrainHangers runs inside every one of
            // them and hitching on a pick swing is a live complaint.
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 30; ++b) {
                const int px = ci + int(rnd() % 13u) - 6;
                const int pz = cj + int(rnd() % 13u) - 6;
                const int py = h + int(rnd() % 13u) - 9;
                world_.dig(Vec3{(float(px) + 0.5f) * VOXEL_M, (float(py) + 0.5f) * VOXEL_M,
                                (float(pz) + 0.5f) * VOXEL_M},
                           kDigRadiusVox);
            }
            digNs += double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
            digN += 30;
            ++sites;
            // ...and now count what is left standing on nothing.
            const int R = 24, n2 = R * 2 + 1;
            const int i0 = ci - R, j0 = cj - R, y0 = h - R - 6;
            std::vector<uint8_t> sol(size_t(n2) * n2 * n2, 0), seen(size_t(n2) * n2 * n2, 0);
            auto ix = [&](int a, int bb, int c) {
                return size_t(a) + size_t(c) * size_t(n2) + size_t(bb) * size_t(n2) * size_t(n2);
            };
            long solid = 0;
            for (int c = 0; c < n2; ++c)
                for (int a = 0; a < n2; ++a)
                    for (int bb = 0; bb < n2; ++bb)
                        if (world_.terrainSolidAt(i0 + a, j0 + c, y0 + bb, tmemo)) {
                            sol[ix(a, bb, c)] = 1;
                            ++solid;
                        }
            std::vector<int> st;
            for (int c = 0; c < n2; ++c)
                for (int a = 0; a < n2; ++a)
                    if (sol[ix(a, 0, c)]) {
                        seen[ix(a, 0, c)] = 1;
                        st.push_back(int(ix(a, 0, c)));
                    }
            long reached = long(st.size());
            static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            while (!st.empty()) {
                const int p = st.back();
                st.pop_back();
                const int a = p % n2, c = (p / n2) % n2, bb = p / (n2 * n2);
                for (const int *o : off) {
                    const int x2 = a + o[0], y2 = bb + o[1], z2 = c + o[2];
                    if (x2 < 0 || y2 < 0 || z2 < 0 || x2 >= n2 || y2 >= n2 || z2 >= n2) continue;
                    const size_t q = ix(x2, y2, z2);
                    if (!sol[q] || seen[q]) continue;
                    seen[q] = 1;
                    ++reached;
                    st.push_back(int(q));
                }
            }
            // COUNTED IN THE INTERIOR ONLY. A voxel on this box's own wall may
            // be held up by stone just outside it, which the flood cannot see,
            // so counting it would be measuring the ruler rather than the wood.
            long f2 = 0;
            for (int bb = 1; bb < n2 - 1; ++bb)
                for (int c = 1; c < n2 - 1; ++c)
                    for (int a = 1; a < n2 - 1; ++a)
                        if (sol[ix(a, bb, c)] && !seen[ix(a, bb, c)]) ++f2;
            (void)solid;
            (void)reached;
            if (f2 > 0) {
                ++siteFloat;
                voxFloat += f2;
                if (f2 > worstSite) worstSite = f2;
            }
        }
        std::printf("  %.3f ms per blow, dig and the hanger sweep together (%ld blows)\n", 
                    digN ? digNs / double(digN) / 1e6 : 0.0, digN);
        std::printf("  %ld dig sites, 30 blows each: %ld left ground hanging, %ld voxels,"
                    " worst %ld\n",
                    sites, siteFloat, voxFloat, worstSite);

        std::printf("\n=== VERDICT ===\n");
        std::printf("  %d undermined, %d came down, %d left standing on nothing\n", tried, fell,
                    stood);
        std::printf("  models: %ld voxels left hanging over %ld blows\n", modelLeft,
                    modelBlows);
        std::printf("  ground: %ld voxels of terrain standing on nothing\n", voxFloat);
        std::printf("  %s\n", (stood || voxFloat || modelLeft > 0)
                                     ? "FAIL"
                                     : "PASS -- nothing is left in the air.");
    }

    // -----------------------------------------------------------------------
    // TICK EVERY POPULATION AT A POINT, the way one frame of the wood does.
    //
    // WHY A WARMUP EXISTS AT ALL is written out at length in the offline
    // report, which does the same thing for the same reason: a population that
    // FILLS ITSELF OVER TIME reports its spawn state if you tick it once, and
    // most of these have a settling time measured in tens of seconds -- the
    // flying flock is born on a ring at 0.78-0.94 of 105 m and needs to cross
    // it before it is distributed the way play sees it.
    //
    // ONE LOOP RATHER THAN SIX, which is the one thing this does differently:
    // the report warms each population separately with its own count because
    // it is making a PICTURE and each system has its own settling time. A test
    // of where the animals ARE wants the state a running frame produces, and
    // that is all six advancing on one clock.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // EVERY POPULATION ONTO THE BAND, IN ONE CALL.
    //
    // The frame loop publishes them where each is ticked, which is six places;
    // a headless test ticks them all in one loop (warmLife) and then has to do
    // the same. It exists because --kill-test asks the BAND what is under the
    // crosshair -- see render/lifehit.h -- and an animal that has not been
    // published is, as far as that question is concerned, not there.
    // -----------------------------------------------------------------------
