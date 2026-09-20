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
            // ...AND THE CHERRY, 2026-09-19. Its band was APPENDED, so it is
            // the far side of the pine from the oak -- which is the long walk
            // of the four and the reason /locate is worth having for it.
            {"cherry", "cherry_forest", Biome::Cherry},
            // ...AND THE SAND, 2026-09-19. The only entry in this table that
            // is not a forest, and the furthest walk of the five.
            {"desert", "sand", Biome::Desert},
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
    //   woods  WHERE IT LIVES WHEN IT IS NOT HERE, as a SET of kWoodPine /
    //          kWoodBirch / kWoodOak -- VoxelTerrain's own bits, and the same
    //          value MarchSpec::woods carries. Only ever read on a miss, and
    //          then it is the difference between "there are none" -- which is
    //          wrong, there are plenty, one band over -- and taking the player
    //          to them.
    //
    //          IT WAS AN INDEX AND THE ENGINE HAS ALWAYS USED A SET. -1 / 0 / 1
    //          could not say "birch AND oak", which is exactly what the mouse,
    //          the grass snake and the frog are (kWoodBroad), so those three
    //          were written down as birch-only and the housefly, which is
    //          kWoodPine|kWoodOak, as pine-only. Nothing reported it: the
    //          readers asked `birchAt(x)`, so in a PINE wood an oak-dweller and
    //          a pine-dweller answered the same. --locate-test in `--oak`
    //          printed three TABLE WRONG lines the moment the check learned
    //          about the third wood, and all three were the column, not the
    //          engine. One vocabulary now, so they cannot drift again.
    //
    //   water  IT NEEDS A LAKE RATHER THAN A WOOD. Same job as `woods` for the
    //          nine populations in render/lake.h: with none in range the
    //          fallback is the nearest shore, which is where they will be.
    // -----------------------------------------------------------------------
    enum class Life {
        Bunny, Skunk, Armadillo, Porcupine, Mouse, Worm, Snake,
        // The desert's three and the blossom's one. The desert trio landed on
        // 2026-09-19 without rows here, so /locate could not reach three of the
        // engine's nineteen animals; the flamingo would have been the fourth.
        Gecko, Cobra, Scorpion, Flamingo,
        Ant, Fly, Ladybug, Frog, Firefly,
        Bee, Hive, Butterfly, Songbird, Flock,
        Salmon, Bass, Koi, Minnow, Catfish, Bluegill, Duck, Dragonfly, LilyPad,
        // -- AND TWO THINGS THAT ARE NOT ALIVE -------------------------------
        //
        // (user 2026-09-19: "give me a /locate apple command along with the
        // orange too ... teleports me to the nearest apple".)
        //
        // A CROP BELONGS IN THIS TABLE AND A LAKE DOES NOT, and the line
        // between them is the one the header above already draws: a biome is a
        // PLACE, which is a coordinate this engine can compute from nothing,
        // and everything in here is a THING, which exists only where it is
        // right now and can only be found by asking the world. An apple is the
        // second kind -- it hangs in one crown in a wood of them, at 15% of the
        // oaks, and no arithmetic says which. So it gets a row, the whole
        // machinery behind a row (the parser, Tab, both error messages, the
        // arrival and --locate-test) comes with it, and the one thing it needs
        // that no animal did is the `fruit` flag below.
        Apple, Orange,
    };

    struct LifeName {
        const char *name;
        const char *alias;   // "" for none
        Life life;
        float stand;
        // A SET OF kWood* BITS, never an index -- see the column note above.
        // kWoodAll is "anywhere", and it is what every row that used to say -1
        // means.
        uint8_t woods;
        bool water;
        // ONLY AFTER DARK. The survey below judged a row by its wood and its
        // water and knew nothing about the clock, so the firefly -- which the
        // engine will not spawn until the sun is 3.4 degrees under -- came back
        // "NONE, AND IT SHOULD BE HERE" on every daytime run and failed the
        // whole test. A test that is always red is a test nobody reads, and it
        // would have hidden a real row breaking behind it.
        bool night = false;
        // NOT AN ANIMAL. Two readers need to know: the menu, which would
        // otherwise list an apple under "life", and --kill-test, which walks
        // this table shooting everything in it and has already had to make
        // this exception by hand twice (the hive and the lily pad, named in
        // its own code). A flag on the row is the version of that which a new
        // crop cannot forget to join.
        bool fruit = false;
    };

    static const std::vector<LifeName> &lifeNames() {
        // THE WOOD COLUMN IS COPIED FROM THE ENGINE, ROW BY ROW -- kMarchSpec's
        // `woods` for the six marchers, the `claim` call for each critter, the
        // birchBase gate in hangHive and the oakBase one in hangFruit. Four of
        // them did not match what was written here before; see the column note.
        //   name         alias          kind             stand  woods         water
        static const std::vector<LifeName> t = {
            // -- kWoodGreen, NOT kWoodAll, SINCE THE BLOSSOM GOT A ROSTER --
            //
            // (user 2026-09-19: "remove all life that isnt pink in the cherry
            //  forest".) These rows said "anywhere" and meant "in any wood",
            //  which is the same drift kWoodAll took over the desert -- see
            //  kWoodGreen. The survey below judges a find against this column,
            //  so a row left saying anywhere would report every one of them
            //  MISSING from the cherry band rather than correctly absent.
            {"bunny",     "rabbit",    Life::Bunny,      5.0f, kWoodGreen, false},
            {"skunk",     "",          Life::Skunk,      5.0f, kWoodGreen, false},
            {"armadillo", "",          Life::Armadillo,  5.0f, kWoodPine, false},
            {"porcupine", "",          Life::Porcupine,  5.0f, kWoodPine, false},
            // BIRCH *AND* OAK -- kMarchSpec says kWoodBroad for both of these,
            // and this column said birch. In the oak wood /locate mouse found
            // a mouse and the survey called the find a table error, which is
            // the right complaint aimed at the wrong half.
            {"mouse",     "",          Life::Mouse,     10.0f,
             uint8_t(kWoodBroadGreen | kWoodDesert),                       false},
            // EVERY FOREST, AND NOT THE SAND. kMarchSpec gives the worm
            // kWoodForest; this row said kWoodAll, so standing in the desert
            // the survey reported "NONE, AND IT SHOULD BE HERE" about an
            // animal that is correctly absent.
            {"worm",      "",          Life::Worm,       4.0f, kWoodForest, false},
            {"snake",     "grass_snake",Life::Snake,     5.0f, kWoodBroadGreen, false},
            {"firefly",   "fireflies", Life::Firefly,    4.0f, kWoodGreen, false, true},
            {"ant",       "ants",      Life::Ant,        4.0f, kWoodGreen, false},
            // ...AND THE HOUSEFLY IS THE PAIR NOTHING ELSE IS: pine and oak,
            // straight off its claim() in critters.h.
            {"fly",       "flies",     Life::Fly,        4.0f,
             uint8_t(kWoodPine | kWoodOak),                                false},
            {"ladybug",   "ladybird",  Life::Ladybug,    4.0f, kWoodGreen, false},
            // THE BLOSSOM'S OWN, and the only row in this table that lives in
            // one band and no other. See kMarchFlamingo.
            {"flamingo",  "",          Life::Flamingo,   6.0f, kWoodCherry, false},
            // ...AND THE SAND'S THREE, which landed without rows -- see the
            // enum. Their masks come straight off kMarchSpec.
            {"gecko",     "",          Life::Gecko,      5.0f, kWoodDesert, false},
            {"cobra",     "",          Life::Cobra,      6.0f, kWoodDesert, false},
            {"scorpion",  "",          Life::Scorpion,   4.0f, kWoodDesert, false},
            // -- A WATERSIDE ROW, AND THAT IS THE FIX (user 2026-09-14: "I
            //    dont see any frogs even when doing /locate frog") -----------
            //
            // It said `water = false`, so a miss sent you to the BIRCH BAND
            // CENTRE -- dry birch wood, where a frog can never spawn, because
            // fillFrogs places from a bank and nothing else. The command
            // worked perfectly and took you to the one kind of place the
            // animal does not live, every time, for ever.
            //
            // `water = true` with a WOOD is what it actually is: a frog wants a
            // broadleaf SHORE. nearestWater takes the wood set now -- the frog
            // is the only row that needs one, since a lake is the whole
            // condition for everything else that swims.
            //
            // kWoodBroad, NOT BIRCH. fillFrogs tests `& kWoodBroad`, so there
            // are frogs on every oak bank in the world and this column said
            // there were none -- a miss in the oak wood walked the player to
            // the birch band, past the water they were standing next to.
            {"frog",      "",          Life::Frog,       4.0f, kWoodBroad,true},
            // -- A HIVE HANGS IN AN OAK TOO, WHICH NOBODY MEANT ---------------
            //
            // hangHive's only wood test is `if (treeIndex < birchBase) return;`
            // and its comment says why: "a pine carries no hive". That was a
            // complete test when the models ran pines then birches. The oaks
            // were appended AFTER the birches -- pines are [0, birchBase),
            // birches [birchBase, oakBase), oaks [oakBase, size) -- so every
            // oak is above the line and inherited the hives silently.
            //
            // THE TABLE DESCRIBES THE ENGINE, so both rows are kWoodBroad. The
            // survey found a hive 300 m into the oak wood and called the row
            // wrong, which it was. Whether the oak SHOULD bear hives is a
            // separate question for whoever owns hangHive; answering it here
            // by writing birch in this column would only make /locate lie
            // about where the hives it can see are.
            {"bee",       "bees",      Life::Bee,        4.0f, kWoodBroad,false},
            {"hive",      "beehive",   Life::Hive,       5.0f, kWoodBroad,false},
            // -- THE FLYERS, AND WHICH SKIES ARE THEIRS ------------------
            //
            // All three said "everywhere" and all three had stopped being
            // everywhere: the butterflies and the flock were shut out of the
            // SAND on 2026-09-19, and the flock out of the BLOSSOM on the
            // same day. A row that outlives its gate does not fail quietly --
            // it fails the whole run, and it accuses the engine of the bug.
            //
            // The butterfly keeps every forest because the cherry band has its
            // own: v1's pink one, dealt at the birth over blossom. The perched
            // songbird likewise -- the pink bird is a cherry bird.
            {"butterfly", "",          Life::Butterfly,  4.0f, kWoodForest, false},
            {"songbird",  "bird",      Life::Songbird,   5.0f, kWoodForest, false},
            // "songbird" is the one PERCHED in a crown (render/birds.h) and
            // "flock" is the one FLYING (render/birdflock.h) -- two
            // populations, two files, and the same animal to look at.
            // ...AND THE FLYING FLOCK IS THE ONE THAT LOSES THE BLOSSOM TOO:
            // v1's rule is that pink belongs to the cherry and nothing else
            // belongs there, so a blue bird crossing it is recycled.
            {"flock",     "flyingbird",Life::Flock,      8.0f, kWoodGreen, false},
            // EVERY LAKE, NOT THE PINE'S. Nothing in render/lake.h asks the
            // terrain which wood it is in -- a fish exists because there is
            // water -- and all three woods have been wet since the lakes
            // landed. These rows said pine, which was harmless while it only
            // ever chose a fallback shore in a two-wood world and is a walk to
            // the wrong band now.
            {"salmon",    "",          Life::Salmon,     2.0f, kWoodAll,  true},
            {"bass",      "",          Life::Bass,       2.0f, kWoodAll,  true},
            {"koi",       "",          Life::Koi,        2.0f, kWoodAll,  true},
            {"minnow",    "",          Life::Minnow,     2.0f, kWoodAll,  true},
            {"catfish",   "",          Life::Catfish,    2.0f, kWoodAll,  true},
            {"bluegill",  "blue_gill", Life::Bluegill,   2.0f, kWoodAll,  true},
            {"duck",      "",          Life::Duck,       2.0f, kWoodAll,  true},
            {"dragonfly", "",          Life::Dragonfly,  2.0f, kWoodAll,  true},
            {"lilypad",   "lily",      Life::LilyPad,    2.0f, kWoodAll,  true},
            // -- THE CROP, WHICH IS AN OAK ROW --------------------------------
            //
            // hangFruit is the only thing that places kind 6 and it refuses
            // anything that is not an oak above kFruitMinTreeVox, so `wood = 2`
            // is not a preference -- it is where a fruit can exist at all. The
            // pine and the birch have none and never will, which makes a miss
            // out there the ORDINARY answer and the oak band the honest place
            // to be sent.
            //
            // 4 m OFF, AND THE REASON IS THE OPPOSITE OF AN ANIMAL'S. Nothing
            // here bolts -- a fruit is decor, and the whole column exists
            // because a flee sphere does. It is the TREE that sets this one:
            // arriving on the fruit's own column puts the player inside the
            // trunk it hangs off, and findClear then shoves them out to a spot
            // nobody chose. Four metres stands them clear of the bole, well
            // inside kFruitReachM (8 m) for anything on a young oak, and
            // looking up into the crown -- which is where the crop is.
            {"apple",     "apples",    Life::Apple,      4.0f, kWoodOak,  false, false, true},
            {"orange",    "oranges",   Life::Orange,     4.0f, kWoodOak,  false, false, true},
        };
        return t;
    }

    // ONE BIT -> ITS BAND, AND ITS NAME. Both take a single kWood* bit, never
    // a set: a set is a question with up to three answers and nearestWoodX is
    // what turns it into one.
    static Biome woodBiome(uint8_t bit) {
        return (bit & kWoodBirch)    ? Biome::Birch
               : (bit & kWoodOak)    ? Biome::Oak
               : (bit & kWoodCherry) ? Biome::Cherry
               : (bit & kWoodDesert) ? Biome::Desert
                                     : Biome::Pine;
    }
    static const char *woodWord(uint8_t bit) {
        return (bit & kWoodBirch)    ? "birch"
               : (bit & kWoodOak)    ? "oak"
               : (bit & kWoodCherry) ? "cherry"
               : (bit & kWoodDesert) ? "desert"
                                     : "pine";
    }

    // -----------------------------------------------------------------------
    // THE NEAREST OF THE WOODS A ROW LIVES IN.
    //
    // A ROW'S `woods` IS A SET, so "take me to where it lives" has up to three
    // right answers and the useful one is whichever band is closest -- a mouse
    // is birch AND oak, and walking a player past the oak band to reach the
    // birch one is a longer trip to the same animal. Returns which bit won, so
    // the reply can name the wood it actually picked.
    //
    // kWoodAll NEVER REACHES HERE: a row that lives everywhere and found
    // nothing has nowhere better to be sent, and locateLife says so instead.
    // -----------------------------------------------------------------------
    bool nearestWoodX(uint8_t woods, float *outX, uint8_t *outBit) const {
        bool any = false;
        float bestX = 0.0f, best = 1e30f;
        for (int i = 0; i < 3; ++i) {
            const uint8_t bit = uint8_t(1u << i);   // pine, birch, oak
            if (!(woods & bit)) continue;
            const float x = nearestBandX(woodBiome(bit));
            const float d = fabsf(x - pos_.x);
            if (any && d >= best) continue;
            best = d;
            bestX = x;
            any = true;
            if (outBit) *outBit = bit;
        }
        if (any && outX) *outX = bestX;
        return any;
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
            case Life::Gecko:     return bunnies_.nearestSkunk(kMarchGecko, pos_, at, &d);
            case Life::Cobra:     return bunnies_.nearestSkunk(kMarchCobra, pos_, at, &d);
            case Life::Scorpion:  return bunnies_.nearestSkunk(kMarchScorpion, pos_, at, &d);
            case Life::Flamingo:  return bunnies_.nearestSkunk(kMarchFlamingo, pos_, at, &d);
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
            // THE SAME REACH THE HIVE ASKS FOR, and for the same reason: decor
            // exists as far out as the chunk holding it is resident, and a
            // command that could only find a fruit already inside pick range
            // would not be worth typing. The scan is over resident chunks only
            // -- a one-off cost on one keystroke.
            case Life::Apple:     return world_.nearestFruit(0, pos_, 260.0f, at);
            case Life::Orange:    return world_.nearestFruit(1, pos_, 260.0f, at);
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
    // `woods` is a SET of kWood* bits, kWoodAll for any -- the frog is the only
    // row that narrows it, and it has to: sending it to the nearest lake full
    // stop lands it on a PINE shore, where fillFrogs gates it out and it will
    // never appear. See its row in lifeNames.
    bool nearestWater(float *outX, float *outZ, uint8_t woods = kWoodAll) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        auto wetAt = [&](float x, float z) {
            // THE TERRAIN'S OWN BIT, not `birchAt` against an index. The frog
            // is kWoodBroad and this used to ask "is it birch", so every oak
            // bank in the world -- half the water a frog can live beside --
            // was refused. See LifeName::woods.
            if (woods != kWoodAll && !(t.woodBit(x) & woods)) return false;
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
    // -----------------------------------------------------------------------
    // [G] -- RESPAWN INTO THE NEXT BIOME, AND KEEP PRESSING TO CYCLE.
    //
    // (user 2026-09-19: "have it where the keybind g respawns the player to a
    //  new biome. if the player keeps pressing g to respawn, it cycles through
    //  the biomes.")
    //
    // THIS IS /locate <wood> WITH A CURSOR ON IT. That command is the tested
    // path to a band -- nearestBandX picks the nearest repeat of the tiling
    // rather than an absolute coordinate, teleportTo gathers the trees round
    // the DESTINATION before it stands you up, and --locate-test walks all five
    // every run. Writing a second way to arrive in a wood would be a second
    // thing to keep true.
    //
    // THE FIRST PRESS STARTS FROM WHERE YOU ARE. The cursor is -1 until then,
    // and it is seeded from the band under your feet -- so press one takes you
    // OUT of the wood you are standing in, which is what "respawn me somewhere
    // new" means. Seeding it with 0 would make the first press a no-op for
    // anyone who happened to be in the pine.
    //
    // AND THE DROP MOVES EVERY TIME. A pure band hop keeps z, so the fourth
    // lap of a five-band cycle would put you on the exact spot of the first --
    // five places, for ever. Each press walks z on by a hashed stride, so a
    // lap comes back to the same WOOD and not to the same clearing.
    //
    // A PINNED WORLD HAS ONE BIOME AND SAYS SO. Cycling a list of one is a key
    // that looks broken, so there the press is still a respawn -- a new drop in
    // the only wood there is -- and the reply names it.
    // -----------------------------------------------------------------------
    void respawnToNextBiome() {
        const std::vector<BiomeName> &all = biomeNames();
        // WHICH OF THEM THIS WORLD ACTUALLY HAS.
        std::vector<int> reachable;
        for (size_t i = 0; i < all.size(); ++i)
            if (!world_.terrain.forced || world_.terrain.biome == all[i].biome)
                reachable.push_back(int(i));
        if (reachable.empty()) return;   // cannot happen: a forced world is in the table

        if (respawnBiome_ < 0) {
            // SEEDED FROM UNDERFOOT, so the first press leaves this wood.
            const Biome here = woodBiome(world_.terrain.woodBit(pos_.x));
            for (size_t k = 0; k < reachable.size(); ++k)
                if (all[size_t(reachable[k])].biome == here) respawnBiome_ = int(k);
            if (respawnBiome_ < 0) respawnBiome_ = 0;
        }
        respawnBiome_ = (respawnBiome_ + 1) % int(reachable.size());
        const BiomeName &bn = all[size_t(reachable[size_t(respawnBiome_)])];

        // ...AND A DIFFERENT CLEARING EACH TIME. Salted on the press count so
        // two players pressing G the same number of times land together, which
        // is the same determinism every other placement in this engine has.
        ++respawnHops_;
        const float stride =
            (hashUnit(0x9E37u + respawnHops_ * 2654435761u, 0u) - 0.5f) * 2.0f * kRespawnRoamM;

        float tx = nearestBandX(bn.biome), tz = pos_.z + stride;
        bool stand = false;
        if (world_.terrain.forced) stand = nearestStand(&tx, &tz);
        if (world_.terrain.usingDem() && !stand) {
            const float half = maxf(0.0f, 0.5f * world_.terrain.dem().spanX() - 200.0f);
            const float halfZ = maxf(0.0f, 0.5f * world_.terrain.dem().spanZ() - 200.0f);
            tx = clampf(tx, -half, half);
            tz = clampf(tz, -halfZ, halfZ);
        }
        teleportTo(tx, tz);
        std::printf("v2: respawned in the %s wood at %.0f, %.0f%s\n",
                    world_.terrain.woodName(pos_.x), double(pos_.x), double(pos_.z),
                    reachable.size() == 1 ? "  (the only wood in this world)" : "");
        std::fflush(stdout);
        // ON SCREEN as well as on the console: the same fading line every
        // /locate reply uses, because this IS a /locate reply -- the player
        // pressed a key instead of typing it.
        consoleMsg_ = std::string("respawned -- the ") + bn.name +
                      (reachable.size() == 1 ? " wood, the only one in this world" : " wood");
        consoleMsgUntil_ = nowSeconds() + kConsoleMsgHold;
    }

    float nearestBandX(Biome b) const {
        const float period = VoxelTerrain::bandCount() * VoxelTerrain::kBandW;
        const float c = VoxelTerrain::bandCentre(b);
        const float k = floorf((pos_.x - c) / period + 0.5f);
        return c + k * period;
    }

    // -----------------------------------------------------------------------
    // WHERE THE TREES ACTUALLY ARE -- for a world that is all one wood.
    //
    // (user 2026-09-18: "/locate birch does not work. make it work.")
    //
    // A BAND CENTRE IS A MEANINGLESS COORDINATE IN A PINNED WORLD, and that is
    // the whole of what was wrong. --acadia pins the island to birch, so
    // nearestBandX answers with a number that is birch, like every other number
    // in that world: the player was teleported ninety metres inside an
    // identical wood, the reply said "birch forest", and nothing they could see
    // had changed. Refusing (what it did before) and hopping (what it did
    // after) are the same experience from the keyboard -- the command does
    // nothing.
    //
    // SO IT GOES TO A STAND. The one thing "take me to the birch forest" can
    // honestly mean where everything is birch is take me to where the birch is
    // THICK -- out of whatever clearing, bare ridge or shore the player is
    // standing in and under a canopy. standDensity is the field that plants the
    // trees (see the gate in chunks.h), so it is the same question the wood
    // itself is answered with, and it costs no chunk.
    //
    // FAR ENOUGH TO BE FELT. kStandMinM is 150 m -- half a view disc, so the
    // trees around you when you arrive are not the ones you left. A closer
    // "best" spot is worse than a slightly thinner one further out, which is
    // why distance gates the candidates rather than scoring them.
    //
    // ON THE DATA, NOT IN THE LAKE, AND WHERE A TREE IS ALLOWED. The same three
    // conditions the spawn picker uses, for the same reasons -- see chooseSpawn
    // -- and with the DEM window clamp the band answer needed anyway.
    // -----------------------------------------------------------------------
    static constexpr float kStandMinM = 150.0f;
    static constexpr float kStandMaxM = 420.0f;
    bool nearestStand(float *outX, float *outZ) const {
        if (!outX || !outZ) return false;
        const VoxelTerrain &t = world_.terrain;
        float bestX = 0.0f, bestZ = 0.0f, best = -1.0f;
        const float half = t.usingDem()
                               ? maxf(0.0f, 0.5f * t.dem().spanX() - 200.0f)
                               : 1.0e9f;
        for (uint32_t i = 0; i < 512; ++i) {
            const float r =
                kStandMinM + (kStandMaxM - kStandMinM) * sqrtf(hashUnit(0x51ed270bu, i));
            const float a = hashUnit(0x9e3779b9u, i) * 6.2831853f;
            const float x = clampf(pos_.x + r * cosf(a), -half, half);
            const float z = clampf(pos_.z + r * sinf(a), -half, half);
            if (std::hypot(x - pos_.x, z - pos_.z) < kStandMinM) continue;
            if (!t.coverAllowsTree(x, z)) continue;
            const float h = t.heightM(x, z);
            const float w = t.waterAt(x);
            if (w > -1.0e8f && h < w + 2.0f) continue;
            const float d = t.standDensity(x, z);
            if (d > best) {
                best = d;
                bestX = x;
                bestZ = z;
            }
        }
        if (best < 0.0f) return false;
        *outX = bestX;
        *outZ = bestZ;
        return true;
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
        // -- AT THE DESTINATION'S ALTITUDE, NOT THIS ONE ------------------
        //
        // (user 2026-09-19: "when pressing g, the player spawned inside a
        //  rock".)
        //
        // THE RADIUS WAS RIGHT AND THE CENTRE WAS NOT. This asked for solids
        // round (x, THE PLAYER'S CURRENT y, z) -- and [G] is a jump across the
        // world, so that y belongs to the wood being left. Teleporting from a
        // ridge to a valley floor put the query a hundred metres above the
        // ground being landed on, collidersNear returned nothing, and
        // findClear then spiralled through a boulder it could not see.
        //
        // The ground at the destination is one heightVox away and is what the
        // query is actually about.
        //
        // 12 m RATHER THAN 8, because the desert's boulders are the biggest
        // solids in the world and a stone is kept by its CENTRE: at 8 the
        // gather could drop a three-metre rock whose middle sat just outside
        // it while its flank still covered the spot being stood on.
        {
            TerrainMemo tmem;
            const int gi = int(std::floor(x / VOXEL_M)), gj = int(std::floor(z / VOXEL_M));
            const float gy = float(world_.terrain.heightVox(gi, gj, tmem) + 1) * VOXEL_M;
            world_.collidersNear(Vec3(x, gy, z), 12.0f, &solids_);
        }
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
    // 1.5 m STEPS, AND THE REACH IS THE CALLER'S. Finer than nearestWater's 6 m
    // because this one is placing a body rather than finding a lake. The reach
    // defaults to 400 m, which is a lake's half-width as seen from an animal
    // swimming in it -- past that there is no shore to reach and the caller is
    // better off being told.
    //
    // IT IS A PARAMETER BECAUSE /locate <lake> BROKE IT. That one starts from
    // the middle of the whole water body rather than from a fish somewhere in
    // it, and this window holds a five-kilometre reservoir: every bearing was
    // still water at 400 m, the sweep failed, and the fallback stood the player
    // at the centre -- where placeOnGround does exactly what it says and puts
    // them on the BED, underwater, in the dark. A cap that is right for one
    // caller is not a constant.
    // -----------------------------------------------------------------------
    bool standNear(float tx, float tz, float stand, float *outX, float *outZ,
                   float reach = 400.0f) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        const float a0 = atan2f(pos_.x - tx, pos_.z - tz);
        for (float r = maxf(1.5f, stand); r <= reach; r += 1.5f) {
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
    //
    // `fruit` PICKS WHICH HALF, because an apple listed under "life" is a lie
    // the size of the word. The wrap counts what it PRINTS rather than where
    // the row sits in the table -- counting the index instead leaves a hole
    // wherever the filter skipped one, and the crop is the tail of the table,
    // so every line of the life list would have been short by two.
    static std::string lifeList(const char *indent, size_t perLine, bool fruit = false) {
        std::string m;
        size_t n = 0;
        for (const LifeName &ln : lifeNames()) {
            if (ln.fruit != fruit) continue;
            if (n % perLine == 0) m += (n ? "\n" : "");
            m += (n % perLine == 0) ? indent : "  ";
            m += ln.name;
            ++n;
        }
        return m;
    }

    // What /locate knows, for the empty line and for a name it does not have.
    // Three lines, because one would be a hundred and forty characters.
    std::string locateMenu(const std::string &lead) const {
        // The places come first: they are the ones that move you kilometres.
        std::string m = lead + "\n  places  water";
        for (size_t i = 0; i < biomeNames().size(); ++i)
            m += "  " + std::string(biomeNames()[i].name);
        m += poi_.menu();
        // ...AND THE MAPS ARE THEIR OWN ROW, not another place. They are not in
        // this world at all -- naming one walks you through [O] into the arcade
        // -- and a list that puts "canyon" beside a summit is a list that says
        // those are the same kind of trip.
        if (!world_.levelMaps().empty()) {
            m += "\n  arcade ";
            for (const World::LevelMap &lm : world_.levelMaps()) m += "  " + lm.name;
        }
        // ...AND THE CROP IS ITS OWN ROW, for the reason the arcade line above
        // is: a list is a claim about what kind of thing each name is, and
        // "apple" sitting between "bee" and "duck" says an apple walks around.
        return m + "\n  life\n" + lifeList("    ", 7) + "\n  fruit\n" +
               lifeList("    ", 7, true);
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
        // WATER FIRST, because it is the stronger condition of the two. A lake
        // is a place inside a wood rather than a wood to choose between, so
        // sending a salmon-hunter to a band centre would leave them standing in
        // dry trees having been told they were on their way to a fish.
        if (ln.water) {
            float wx = 0.0f, wz = 0.0f;
            if (!nearestWater(&wx, &wz, ln.woods))
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

        if (ln.woods != kWoodAll) {
            // -- ASKED AS A SET, NOT AS "IS IT BIRCH" ------------------------
            //
            // This was `birchAt(x) == (ln.wood == 1)`, a pair of booleans over
            // a gate the engine has always kept as a SET of three bits. It
            // answered correctly for a pine row and a birch row in a two-wood
            // world and for nothing else: an OAK row would have found agreement
            // standing in the PINE -- neither of them is birch -- and replied
            // "walk on a little", which is the answer for being in the right
            // wood, from two bands away. See the note on LifeName::woods.
            const uint8_t here = world_.terrain.woodBit(pos_.x);
            // ALREADY IN A WOOD IT LIVES IN AND STILL NOTHING. Not a place
            // problem, so do not teleport: either the models failed to load
            // (the loaders say so on stdout) or this stretch of the wood
            // simply drew none -- a hive is on 5% of the birches and a crop on
            // 15% of the oaks big enough to carry one.
            if (here & ln.woods)
                return std::string("no ") + ln.name + " in range -- walk on a little";
            // WHICH OF ITS WOODS, ASKED ONCE. A row can name more than one and
            // the nearest is the only one worth either travelling to or
            // naming in the refusal below.
            float tx = pos_.x;
            uint8_t want = kWoodPine;
            if (!nearestWoodX(ln.woods, &tx, &want))
                return std::string("no ") + ln.name + " anywhere -- its row lists no wood";
            if (world_.terrain.forced)
                // -- AND IT NAMES A FLAG THAT EXISTS -----------------------
                //
                // The biome branch carries the paragraph about why: "restart
                // without it" is an instruction to remove something that may
                // never have been on the command line, and it also named
                // --birch / --pine only, so an oak row asked in a birch world
                // was told to restart with one of two flags, neither of which
                // would have helped.
                return std::string("no ") + ln.name + " here -- this world is " +
                       world_.terrain.woodName(pos_.x) +
                       " everywhere. restart with --all-woods for the three bands, or --" +
                       woodWord(want) + " for a world of it";
            teleportTo(tx, pos_.z);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "no %s here -- the %s wood, %.0f, %.0f. "
                                            "give it a moment",
                          ln.name, woodWord(want), tx, pos_.z);
            return std::string(buf);
        }

        // Every wood, so there is nowhere better to be sent: this one did not
        // load. Every loader prints its own reason at startup.
        return std::string("no ") + ln.name + " anywhere -- check the console output for the "
                           "loader's warning";
    }

    #include "ui/app_console.inl"
    #include "ui/app_room.inl"
