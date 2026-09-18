// app_locate.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: biome/life tables, nearest-thing queries, teleport, /locate
// -----------------------------------------------------------------------------
    static const std::vector<BiomeName> &biomeNames() {
        static const std::vector<BiomeName> t = {
            {"pine", "pine_forest", Biome::Pine},
            {"birch", "birch_forest", Biome::Birch},
            // v1's wood, imported 2026-09-16. Its band sits between the other
            // two -- see VoxelTerrain's band note -- so /locate oak from the
            // birch is the shorter walk of the two.
            {"oak", "oak_forest", Biome::Oak},
        };
        return t;
    }

    // -----------------------------------------------------------------------
    // THE LIFE /locate CAN TAKE YOU TO.
    //
    // "/locate skunk. this teleports the player to the nearest life."
    // (user 2026-09-14.) The biome table above takes you to a PLACE, which is
    // a coordinate this engine can compute from nothing; this one takes you to
    // an ANIMAL, which exists only where it is standing right now. So every
    // row below is answered by asking that population, never by arithmetic.
    //
    // ADDING AN ANIMAL IS ONE ROW, for the reason the biome table gives: the
    // parser, the list in both error messages and the teleport all read this,
    // so a creature cannot be half-registered. The one thing a row cannot
    // carry is WHICH population to ask -- six classes with six containers --
    // and that is the switch in nearestLife, which the compiler checks is
    // exhaustive.
    //
    // WHAT EACH COLUMN IS FOR:
    //
    //   stand  HOW FAR OFF YOU ARRIVE, and it is not decoration. Every land
    //          mammal has a flee sphere (MarchSpec::fleeInM) and landing
    //          inside it means the animal you asked to be taken to bolts
    //          before you have seen it do anything else. Each one is set
    //          outside its own hysteresis: the mouse's 8.6 m flee-out is why
    //          it is the odd number in the column. Anything that swims gets
    //          the minimum, because standNear walks out to the shore on its
    //          own and starting further out only skips usable ground.
    //
    //   wood   WHERE IT LIVES WHEN IT IS NOT HERE: -1 either, 0 pine, 1 birch,
    //          from MarchSpec::wood and the hive pass in scene/chunks.h. This
    //          is only ever read when nothing was found, and then it is the
    //          difference between "there are none" -- which is wrong, there
    //          are plenty, one band over -- and taking the player to them.
    //
    //   water  IT NEEDS A LAKE RATHER THAN A WOOD. Same job as `wood` for the
    //          nine populations in render/lake.h: with none in range the
    //          fallback is the nearest shore, which is where they will be.
    // -----------------------------------------------------------------------
    enum class Life {
        Bunny, Skunk, Armadillo, Porcupine, Mouse, Worm, Snake,
        Ant, Fly, Ladybug, Frog, Firefly,
        Bee, Hive, Butterfly, Songbird, Flock,
        Salmon, Bass, Koi, Minnow, Catfish, Bluegill, Duck, Dragonfly, LilyPad,
    };

    struct LifeName {
        const char *name;
        const char *alias;   // "" for none
        Life life;
        float stand;
        int wood;            // -1 either, 0 pine, 1 birch
        bool water;
        // ONLY AFTER DARK. The survey below judged a row by its wood and its
        // water and knew nothing about the clock, so the firefly -- which the
        // engine will not spawn until the sun is 3.4 degrees under -- came back
        // "NONE, AND IT SHOULD BE HERE" on every daytime run and failed the
        // whole test. A test that is always red is a test nobody reads, and it
        // would have hidden a real row breaking behind it.
        bool night = false;
    };

    static const std::vector<LifeName> &lifeNames() {
        //                name         alias          kind             stand wood water
        static const std::vector<LifeName> t = {
            {"bunny",     "rabbit",    Life::Bunny,      5.0f, -1, false},
            {"skunk",     "",          Life::Skunk,      5.0f, -1, false},
            {"armadillo", "",          Life::Armadillo,  5.0f,  0, false},
            {"porcupine", "",          Life::Porcupine,  5.0f,  0, false},
            {"mouse",     "",          Life::Mouse,     10.0f,  1, false},
            {"worm",      "",          Life::Worm,       4.0f, -1, false},
            {"snake",     "grass_snake",Life::Snake,     5.0f,  1, false},
            {"firefly",   "fireflies", Life::Firefly,    4.0f, -1, false, true},
            {"ant",       "ants",      Life::Ant,        4.0f, -1, false},
            {"fly",       "flies",     Life::Fly,        4.0f,  0, false},
            {"ladybug",   "ladybird",  Life::Ladybug,    4.0f, -1, false},
            // -- A WATERSIDE ROW, AND THAT IS THE FIX (user 2026-09-14: "I
            //    dont see any frogs even when doing /locate frog") -----------
            //
            // It said `water = false`, so a miss sent you to the BIRCH BAND
            // CENTRE -- dry birch wood, where a frog can never spawn, because
            // fillFrogs places from a bank and nothing else. The command
            // worked perfectly and took you to the one kind of place the
            // animal does not live, every time, for ever.
            //
            // `water = true` with `wood = 1` is what it actually is: a frog
            // wants a birch SHORE. nearestWater takes the wood now -- it is
            // the first row that has ever needed it, since every other swimmer
            // is a pine row and a lake was the whole condition.
            {"frog",      "",          Life::Frog,       4.0f,  1, true},
            {"bee",       "bees",      Life::Bee,        4.0f,  1, false},
            {"hive",      "beehive",   Life::Hive,       5.0f,  1, false},
            {"butterfly", "",          Life::Butterfly,  4.0f, -1, false},
            {"songbird",  "bird",      Life::Songbird,   5.0f, -1, false},
            // "songbird" is the one PERCHED in a crown (render/birds.h) and
            // "flock" is the one FLYING (render/birdflock.h) -- two
            // populations, two files, and the same animal to look at.
            {"flock",     "flyingbird",Life::Flock,      8.0f, -1, false},
            {"salmon",    "",          Life::Salmon,     2.0f,  0, true},
            {"bass",      "",          Life::Bass,       2.0f,  0, true},
            {"koi",       "",          Life::Koi,        2.0f,  0, true},
            {"minnow",    "",          Life::Minnow,     2.0f,  0, true},
            {"catfish",   "",          Life::Catfish,    2.0f,  0, true},
            {"bluegill",  "blue_gill", Life::Bluegill,   2.0f,  0, true},
            {"duck",      "",          Life::Duck,       2.0f,  0, true},
            {"dragonfly", "",          Life::Dragonfly,  2.0f,  0, true},
            {"lilypad",   "lily",      Life::LilyPad,    2.0f,  0, true},
        };
        return t;
    }

    // -----------------------------------------------------------------------
    // WHERE THE NEAREST ONE OF A KIND IS, or false if none is live.
    //
    // FALSE IS THE ORDINARY ANSWER, NOT AN ERROR. A population only exists
    // inside the streaming ring -- see the fill in each of these files -- so
    // asking for an armadillo from the birch wood finds nothing because there
    // IS nothing, and the caller's job is then to say where they are instead.
    // -----------------------------------------------------------------------
    bool nearestLife(Life k, Vec3 *at) const {
        float d = 0.0f;
        switch (k) {
            case Life::Bunny:     return bunnies_.nearest(pos_, at, &d);
            // nearestSkunk answers for any of the four marchers -- the name is
            // bunnies.h's own shorthand for the family, as publishSkunks is.
            case Life::Skunk:     return bunnies_.nearestSkunk(kMarchSkunk, pos_, at, &d);
            case Life::Armadillo: return bunnies_.nearestSkunk(kMarchArmadillo, pos_, at, &d);
            case Life::Porcupine: return bunnies_.nearestSkunk(kMarchPorcupine, pos_, at, &d);
            case Life::Mouse:     return bunnies_.nearestSkunk(kMarchMouse, pos_, at, &d);
            case Life::Worm:      return bunnies_.nearestSkunk(kMarchWorm, pos_, at, &d);
            case Life::Snake:     return bunnies_.nearestSkunk(kMarchSnake, pos_, at, &d);
            case Life::Firefly:   return critters_.nearestFirefly(pos_, at, &d);
            case Life::Ant:       return critters_.nearestAnt(pos_, at, &d);
            case Life::Fly:       return critters_.nearestFly(pos_, at, &d);
            case Life::Ladybug:   return critters_.nearestBug(pos_, at, &d);
            case Life::Frog:      return critters_.nearestFrog(pos_, at, &d);
            case Life::Bee:       return bees_.nearest(pos_, at, &d);
            case Life::Hive:      return nearestHive(at);
            case Life::Butterfly: return flock_.nearest(pos_, at, &d);
            case Life::Songbird:  return birds_.nearest(pos_, at, &d);
            case Life::Flock:     return flock2_.nearest(pos_, at, &d);
            case Life::Salmon:    return lake_.nearestFish(pos_, 0, at, &d);
            case Life::Bass:      return lake_.nearestFish(pos_, 1, at, &d);
            case Life::Koi:       return lake_.nearestFish(pos_, 2, at, &d);
            case Life::Minnow:    return lake_.nearestFish(pos_, 3, at, &d);
            case Life::Catfish:   return lake_.nearestFish(pos_, 4, at, &d);
            case Life::Bluegill:  return lake_.nearestFish(pos_, 5, at, &d);
            case Life::Duck:      return lake_.nearestDuck(pos_, at, &d);
            case Life::Dragonfly: return lake_.nearestDfly(pos_, at, &d);
            case Life::LilyPad:   return lake_.nearestPad(pos_, at, &d);
        }
        return false;
    }

    // A HIVE IS NOT A POPULATION. It is decor hanging in a birch crown, so
    // there is no class holding a list of them -- World::decorNear is the
    // register, and it is the same query the bees themselves are handed.
    //
    // ASKED FRESH AND WIDE RATHER THAN READING hivesNear_. That list is
    // gathered to kBeeHiveM (40 m) because that is how far a bee cares; a
    // command that can only find a hive already well within earshot of one is
    // not worth having. The reach here is the loaded ring's own, and the scan
    // is over resident chunks only, which is a one-off cost on one keystroke.
    //
    // THE POSITION IS A CORNER, which decorNear says in its own comment and is
    // worth repeating at the one caller that AIMS at the result: the hive is
    // about a metre across, so the crosshair lands on its edge rather than its
    // middle. Close enough to be looking at it, and the bees orbiting it are
    // what the eye finds anyway.
    bool nearestHive(Vec3 *at) const {
        std::vector<Vec3> hives;
        world_.decorNear(5, pos_, 260.0f, &hives);
        float best = 1e30f;
        for (const Vec3 &h : hives) {
            const float dx = h.x - pos_.x, dz = h.z - pos_.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = h;
        }
        return best < 1e29f;
    }

    void setConsoleOpen(bool on) {
        if (on == consoleOpen_) return;
        if (on) {
            consoleCapture_ = looking_;
            if (looking_) setCapture(false);
            holdLook_ = false;
            consoleBuf_[0] = 0;
            consoleFocus_ = true;
            // A FRESH BOX. The reply lives in the fading line now, so carrying
            // the last one back into the window would stack a stale answer
            // above a prompt that has not been typed into yet.
            consoleMsg_.clear();
            consoleMsgUntil_ = 0.0;
        } else if (consoleCapture_) {
            setCapture(true);
            consoleCapture_ = false;
        }
        consoleOpen_ = on;
    }

    // The centre of the nearest band of `b` to the player, in world x. The
    // bands repeat with period 2 * kBandW, so this is the band centre plus
    // whichever whole period lands closest.
    // -----------------------------------------------------------------------
    // THE NEAREST SHORE, for /locate water.
    //
    // TWO SEARCHES, AND THE SECOND ONE IS THE POINT. Finding a wet column is
    // easy; arriving ON it drops you on the LAKE BED, underwater, which is not
    // what "take me to water" means. So the wet column is only an anchor, and
    // the spot returned is the nearest DRY column to it -- a shore, looking at
    // the water rather than standing in it.
    //
    // RINGS RATHER THAN A GRID because the answer wanted is the nearest one,
    // and a ring search returns it in the order it wants. 6 m steps out to
    // 6 km: lakes are basin-gated and sparse (0.97% of the world is wet, and
    // the nearest one to the origin is several hundred metres away), so a
    // coarse ring is what keeps this from being a second of stalling.
    // -----------------------------------------------------------------------
    // `wood` is -1 for any, 0 pine, 1 birch -- the frog is the only row that
    // passes anything but -1, and it has to: sending it to the nearest lake
    // full stop lands it on a PINE shore, where it is gated out and will never
    // appear. See its row in lifeNames.
    bool nearestWater(float *outX, float *outZ, int wood = -1) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        auto wetAt = [&](float x, float z) {
            if (wood >= 0 && (t.birchAt(x) ? 1 : 0) != wood) return false;
            const int vi = int(floorf(x / VOXEL_M)), vj = int(floorf(z / VOXEL_M));
            int wy = 0;
            return t.lakeColumn(vi, vj, memo, &wy);
        };
        float wx = 0.0f, wz = 0.0f;
        bool found = false;
        for (float r = 0.0f; r <= 6000.0f && !found; r += 6.0f) {
            const int steps = (r < 1.0f) ? 1 : maxi(8, int(2.0f * PI * r / 6.0f));
            for (int k = 0; k < steps; ++k) {
                const float a = float(k) / float(steps) * 2.0f * PI;
                const float x = pos_.x + cosf(a) * r, z = pos_.z + sinf(a) * r;
                if (!wetAt(x, z)) continue;
                wx = x; wz = z; found = true; break;
            }
        }
        if (!found) return false;
        // Back out to dry land -- the nearest column that is not in the lake.
        for (float r = 2.0f; r <= 200.0f; r += 2.0f) {
            const int steps = maxi(8, int(2.0f * PI * r / 2.0f));
            for (int k = 0; k < steps; ++k) {
                const float a = float(k) / float(steps) * 2.0f * PI;
                const float x = wx + cosf(a) * r, z = wz + sinf(a) * r;
                if (wetAt(x, z)) continue;
                *outX = x; *outZ = z; return true;
            }
        }
        *outX = wx; *outZ = wz;  // a lake with no shore within 200 m: stand in it
        return true;
    }

    // THE PERIOD IS THE WORLD'S, NOT A COPY OF IT. This read
    // `2.0f * kBandW` -- correct while there were two woods, and silently wrong
    // the moment a third was inserted: /locate pine would have walked you to a
    // multiple of 1600 m when the pine band now repeats every 2400, which lands
    // in whichever wood happens to be there. Nothing would have reported it;
    // you would simply have arrived among the wrong trees.
    //
    // Derived from bandCount() so a fourth wood cannot reintroduce it.
    float nearestBandX(Biome b) const {
        const float period = VoxelTerrain::bandCount() * VoxelTerrain::kBandW;
        const float c = VoxelTerrain::bandCentre(b);
        const float k = floorf((pos_.x - c) / period + 0.5f);
        return c + k * period;
    }

    void teleportTo(float x, float z) {
        // -- THE TREES ARE GATHERED ROUND THE DESTINATION ---------------------
        //
        // NOT round where you are standing, which is what walkWorld() does and
        // is why this is not simply that call. placeOnGround runs findClear,
        // whose whole job is to step a body out of a trunk it would otherwise
        // be planted inside -- and findClear can only see the solids in the
        // WalkWorld it is handed. walkWorld() gathers a six-metre bubble round
        // player_.pos, so after a jump of a hundred metres that list describes
        // the wood you have just LEFT: the check ran, found nothing in the way,
        // and passed every spot in the new one.
        //
        // It cost nothing to miss while /locate went to a BAND CENTRE, which is
        // a coordinate with no particular tree at it. /locate skunk arrives
        // five metres from an animal a few dozen times a session, and a pine
        // wood puts a trunk under about one spot in sixteen.
        //
        // 8 m, because findClear spirals out to 6 and a solid is kept by its
        // centre -- a trunk whose middle is just outside the gather is still
        // one the body can be inside.
        world_.collidersNear(Vec3(x, player_.pos.y, z), 8.0f, &solids_);
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = solids_.data();
        w.solidCount = int(solids_.size());
        player_.placeOnGround(w, x, z);
        pos_ = player_.eyePosition();
        // EVERYTHING TEMPORAL HAS TO BE TOLD. The film, the fog's history and
        // the reconstruction all carry state about somewhere else entirely, and
        // blending out of it drags the old wood across the new one for a
        // second. The streamer re-rings itself from the new position on its own.
        moving_ = true;
        tracer_.resetAccumulation();
        volfog_.invalidate();
    }

    // Point the camera at a world position. The asset deck's own two lines --
    // see stageSubject, which now calls this -- pulled out because /locate
    // needs exactly the same thing: being set down beside an animal and left
    // facing the other way is not being taken to it.
    void lookAt(const Vec3 &p) {
        const Vec3 aim = p - pos_;
        const float len = maxf(0.01f, length(aim));
        yaw_ = atan2f(aim.x, -aim.z) * 180.0f / PI;
        pitch_ = asinf(clampf(aim.y / len, -1.0f, 1.0f)) * 180.0f / PI;
    }

    // -----------------------------------------------------------------------
    // WHERE YOU STAND TO LOOK AT SOMETHING -- WHICH IS NOT WHERE IT IS.
    //
    // ARRIVING ON THE ANIMAL IS WRONG TWICE OVER. On land it puts the player
    // inside a body and inside its flee sphere, so the thing you asked for
    // bolts on the frame you appear. On water it puts you on the LAKE BED,
    // underwater, which is the trap /locate water already carries a paragraph
    // about -- and every fish, duck, dragonfly and lily pad in the table is on
    // water.
    //
    // SO IT IS ONE RING SEARCH OUTWARD FROM THE CREATURE, starting at that
    // row's own stand-off and taking the first column that is not wet. ON LAND
    // THAT IS THE STAND-OFF ITSELF, because the ground beside a skunk is dry;
    // OVER WATER THE SAME LOOP WALKS OUT TO THE SHORE on its own. Two cases,
    // no branch -- and the water half is nearestWater's second pass, which is
    // the same answer arrived at by the same means.
    //
    // THE SWEEP STARTS ON THE PLAYER'S OWN SIDE. Every point on a ring is the
    // same distance from the animal, so with no preference the arrival side is
    // wherever k == 0 lands -- and being sent round to the far shore of a lake
    // to look at a fish is a long walk for nothing. Opening the sweep at the
    // bearing back toward the player and widening it alternately each way
    // makes the accepted spot the nearest acceptable one to where you were.
    //
    // 1.5 m STEPS, 400 m OUT. Finer than nearestWater's 6 m because this one
    // is placing a body rather than finding a lake, and the cap is a lake's
    // half-width: past that there is no shore to reach and the caller is
    // better off being told.
    // -----------------------------------------------------------------------
    bool standNear(float tx, float tz, float stand, float *outX, float *outZ) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        const float a0 = atan2f(pos_.x - tx, pos_.z - tz);
        for (float r = maxf(1.5f, stand); r <= 400.0f; r += 1.5f) {
            const int steps = maxi(8, int(2.0f * PI * r / 1.5f));
            for (int k = 0; k < steps; ++k) {
                // 0, +1, -1, +2, -2 ... out from the player's bearing.
                const int half = (k + 1) / 2;
                const float turn = float(((k & 1) ? half : -half)) / float(steps) * 2.0f * PI;
                const float a = a0 + turn;
                const float x = tx + sinf(a) * r, z = tz + cosf(a) * r;
                const int vi = int(floorf(x / VOXEL_M)), vj = int(floorf(z / VOXEL_M));
                int wy = 0;
                if (t.lakeColumn(vi, vj, memo, &wy)) continue;
                *outX = x;
                *outZ = z;
                return true;
            }
        }
        return false;
    }

    // Put the player beside `at` and face them at it. Returns how far off the
    // arrival ended up being, which is what the reply quotes -- a fish reached
    // from forty metres of shore and one reached from four are both "the
    // nearest salmon", and the number is the only thing that says which.
    float teleportToLife(const Vec3 &at, float stand) {
        float sx = at.x, sz = at.z;
        // A creature with no dry column within 400 m: stand where it is. The
        // player floats over water rather than falling through it, so this is
        // survivable, and it cannot happen on any lake this world generates.
        if (!standNear(at.x, at.z, stand, &sx, &sz)) { sx = at.x; sz = at.z; }
        teleportTo(sx, sz);
        lookAt(at);
        const float dx = at.x - pos_.x, dz = at.z - pos_.z;
        return sqrtf(dx * dx + dz * dz);
    }

    // The names in the table, wrapped -- the console's reply window is one
    // TextUnformatted and will not wrap for itself, and nineteen animals on
    // one line runs off the side of the screen.
    //
    // BUILT FROM THE TABLE, so the help can never list a creature /locate does
    // not know or miss one it does.
    static std::string lifeList(const char *indent, size_t perLine) {
        std::string m;
        for (size_t i = 0; i < lifeNames().size(); ++i) {
            if (i % perLine == 0) m += (i ? "\n" : "");
            m += (i % perLine == 0) ? indent : "  ";
            m += lifeNames()[i].name;
        }
        return m;
    }

    // What /locate knows, for the empty line and for a name it does not have.
    // Three lines, because one would be a hundred and forty characters.
    std::string locateMenu(const std::string &lead) const {
        std::string m = lead + "\n  places  water";
        for (size_t i = 0; i < biomeNames().size(); ++i)
            m += "  " + std::string(biomeNames()[i].name);
        return m + "\n  life\n" + lifeList("    ", 7);
    }

    // -----------------------------------------------------------------------
    // /locate <animal> -- TAKE ME TO THE NEAREST ONE.
    //
    // THE ANSWER WHEN THERE IS NONE IS STILL A TELEPORT. A population exists
    // only inside the streaming ring, and half this table is gated to one wood
    // or to water (see MarchSpec::wood, the hive pass in scene/chunks.h, and
    // every fill in render/lake.h) -- so "/locate armadillo" typed in the
    // birch wood finds nothing, and there is nothing wrong. Replying "none
    // found" would be true and useless: there are six of them, one band over.
    // So a miss falls through to the PLACE the creature lives, which is a
    // coordinate this engine can always compute, and the population fills in
    // around the player over the next second -- BirthGate waives its floor for
    // exactly this case, a jump of more than 40 m in one tick. See
    // [[v2-spawn-contract]].
    //
    // WHICH IS WHY THE REPLY SAYS WHICH OF THE TWO HAPPENED. "skunk -- 412,
    // -80, 5.0 m off" is an animal on your screen; "no armadillo here -- the
    // pine wood" is a promise about the next few seconds, and reading one as
    // the other is the report "the command does nothing".
    // -----------------------------------------------------------------------
    std::string locateLife(const LifeName &ln) {
        // NOT FROM THE DECK, because it despawns every population on the way
        // in (see stageSubject) and does not run their updates -- there is
        // nothing to find and the answer would always be the fallback, which
        // would then teleport the player off a deck they opened on purpose.
        //
        // AND NOT WITH THE PAUSE BUTTONS UP, for a different reason now that
        // the wood keeps living behind them: the panel is PINNED where it was
        // opened (see setRoomOpen), so a teleport would leave it standing in a
        // wood the player is no longer in.
        if (world_.staged() || pauseOpen_)
            return std::string("not from in here -- back to the wood first");

        Vec3 at{0.0f, 0.0f, 0.0f};
        if (nearestLife(ln.life, &at)) {
            const float d = teleportToLife(at, ln.stand);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%s -- %.0f, %.0f, %.0f m off", ln.name, pos_.x,
                          pos_.z, d);
            return std::string(buf);
        }

        // ---- none in range: go to where they live ---------------------------
        //
        // WATER FIRST, because it is the stronger condition of the two. Every
        // row that swims is also a pine row, and a lake is a place inside that
        // wood rather than a second wood to choose between -- so sending a
        // salmon-hunter to the band centre would leave them standing in dry
        // pines having been told they were on their way to a fish.
        if (ln.water) {
            float wx = 0.0f, wz = 0.0f;
            if (!nearestWater(&wx, &wz, ln.wood))
                return std::string("no ") + ln.name +
                       " and no water it would live beside within 6 km -- lakes sit in "
                       "basins, and this one has to be in the right wood";
            teleportTo(wx, wz);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "no %s in range -- the nearest shore, %.0f, %.0f. "
                                            "give it a moment",
                          ln.name, wx, wz);
            return std::string(buf);
        }

        if (ln.wood >= 0) {
            const bool inBirch = world_.terrain.birchAt(pos_.x);
            const bool wantBirch = (ln.wood == 1);
            // ALREADY IN THE RIGHT WOOD AND STILL NOTHING. Not a place
            // problem, so do not teleport: either the models failed to load
            // (the loaders say so on stdout) or, for a hive, this stretch of
            // birch simply drew none -- they are on 5% of the trees.
            if (inBirch == wantBirch)
                return std::string("no ") + ln.name + " in range -- walk on a little";
            if (world_.terrain.forced)
                return std::string("no ") + ln.name + " here, and the world is pinned to one "
                                   "wood (--birch / --pine) -- restart without it";
            const float tx = nearestBandX(wantBirch ? Biome::Birch : Biome::Pine);
            teleportTo(tx, pos_.z);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "no %s here -- the %s wood, %.0f, %.0f. "
                                            "give it a moment",
                          ln.name, wantBirch ? "birch" : "pine", tx, pos_.z);
            return std::string(buf);
        }

        // Either wood, so there is nowhere better to be sent: this one did not
        // load. Every loader prints its own reason at startup.
        return std::string("no ") + ln.name + " anywhere -- check the console output for the "
                           "loader's warning";
    }

    #include "ui/app_console.inl"
    #include "ui/app_room.inl"
