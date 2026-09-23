// app_ground.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: small helpers, loose debris, the ground patch, walkWorld
// -----------------------------------------------------------------------------
    static double secondsSince(std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    // The same clock the recorder is driven on, read from the GUI pass, which
    // is not handed the frame's `now`.
    static double nowSeconds() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    template <typename... A>
    static std::string fmt(const char *f, A... a) {
        char b[256];
        std::snprintf(b, sizeof(b), f, a...);
        return std::string(b);
    }

    void invalidate() { tracer_.resetAccumulation(); }

    Vec3 forward() const { return Camera::direction(yaw_, pitch_); }
    // THE CAMERA'S OTHER TWO AXES, built exactly as Camera::gpu builds them --
    // w forward, u = w x up, v = u x w. A pose is expressed against these
    // three, so anything that reads a pose back out (the bow's launch point)
    // has to use the same ones or it lands somewhere else.
    Vec3 camRight() const { return normalize(cross(forward(), Vec3(0.0f, 1.0f, 0.0f))); }
    Vec3 camUp() const { return cross(camRight(), forward()); }

    // The world as the player sees it: the terrain, plus the trees and rocks
    // close enough to walk into.
    //
    // Six metres of reach for a body a quarter of a metre wide, because the
    // gather happens ONCE a tick and the player then moves within it. Anything
    // it misses is something the next tick will pick up long before it is
    // reached at 17 m/s.
    // -----------------------------------------------------------------------
    // THE LOOSE WORLD: the solver, the ground it rests on, and the pieces.
    //
    // THE GROUND FOLLOWS THE PLAYER AND IS REBUILT ONLY WHEN THEY LEAVE IT.
    // PhysX needs something to land on, and v2's terrain is a height field by
    // construction -- one height per column, from a pure function -- which is
    // exactly PxHeightFieldGeometry's own primitive. Handing it the resident
    // wood as a mesh would be 91 million triangles and a BVH to cook; handing
    // it a patch is a memcpy of int16s.
    //
    // The margin is what stops it being rebuilt every step at the boundary, and
    // what guarantees nothing falls off an edge before the next patch exists.
    // -----------------------------------------------------------------------
    void stepLoose(float dt) {
        simMs_ += double(dt) * 1000.0;
        // ...AND THE TURNED EARTH GROWS BACK OVER. v1'''s tillRevert, on the
        // same clock the rest of the loose world runs on -- it is the same kind
        // of thing: a change to the static world with a lifetime on it. Costs
        // one compare while nothing is tilled, which is nearly always.
        // -- ...AND ANY SEED THAT WAS IN IT COMES BACK OUT ------------------
        //
        // (user 2026-09-14: "when the tilled grass turns back to dirt/grass,
        // have the planted seeds pop out of the ground and start levitating".)
        //
        // A DROP, WHICH IS WHAT LEVITATING ALREADY IS IN THIS ENGINE. Drops
        // hover for kDropRestSec and are absorbed by walking over them, so a
        // seed that did not take is a seed you can pick up and try somewhere
        // wetter -- which is the whole of what the ask wants and needed no new
        // kind of object.
        //
        // World SAYS WHERE AND app.h SPILLS, because the drop pool is not the
        // world's to reach into. Same split as the spoil, one file over.
        seedsBack_.clear();
        world_.tillRevert(simMs_ * 0.001, &seedsBack_);
        if (!seedsBack_.empty() && seedsTool_ >= 0) {
            const Tool &st = held_.tool(seedsTool_);
            if (!st.models.empty())
                for (size_t k = 0; k < seedsBack_.size(); ++k)
                    drops_.spill(seedsTool_, st.models[0], st.sx, st.sy, st.sz, seedsBack_[k],
                                 float(k) * 1.7f);
        }
        if (physics_.available()) {
            hStart();
            // REBUILT WHEN THE GROUND MOVES, not only when the player does.
            // A dig changes the shape of the floor under everything that is
            // falling, and the patch is the only copy of it the solver has.
            maybeRebuildGroundPatch();
            physics_.step(dt);
            hPhys_ += hStop();
            // Gathered ONCE for the whole band rather than per body: walkWorld
            // runs collidersNear, and asking it per chip per frame would be the
            // expensive part of a feature that is otherwise nearly free.
            // THE ROCKS ARE NOT HULLS ANY MORE. What a loose piece falls
            // against is built where the piece is, off the damaged voxels, and
            // has the hole in it -- see World::buildWindow. A hull of the whole
            // boulder standing beside that would put back exactly the shape
            // that buried every previous version of the chunk.
            // Gathered ONCE for the whole band: walkWorld runs collidersNear,
            // and asking it per chip per frame would be the expensive part of
            // a feature that is otherwise nearly free.
            hStart();
            const WalkWorld ww = walkWorld();
            world_.updateDebris(
                physics_, player_.eyePosition(), simMs_,
                [&](float x, float z) {
                    // Terrain alone: the one floor nothing may ever be under.
                    // ...AND IT HAS THE HOLES IN IT, for the same reason the
                    // walk does now -- a chip that falls into a pit must not be
                    // shoved back out of it by a floor the generator remembers
                    // and the world no longer has.
                    // ...AND THE LEVEL IS NOT HANDLED HERE ANY MORE.
                    // It used to return levelGroundM(x, z) -- the top of the
                    // column -- which indoors is the ROOF and teleported every
                    // chip onto it. That question cannot be answered from
                    // (x, z) alone, so updateDebris asks the level itself now,
                    // downward from the body. See World::levelFloorBelowM.
                    return walkGroundM(ww, x, z);
                });
            world_.flushDebrisInstances();
            hDebris_ += hStop();   // --fell-live reads it
            // -- WHATEVER A FELLED TREE WAS CARRYING, FALLING -----------------
            //
            // See World::dropHangers. A fruit becomes a pickable fruit while the
            // drops have a free slot -- the same spill a shot-down one makes --
            // and a falling body after that, so a laden oak does not push the
            // player's own dropped kit out of the drops' eight slots.
            {
                Vec3 fa{0.0f, 0.0f, 0.0f};
                int fk = 0, nth = 0, asBodies = 0;
                while (world_.takeFallenFruit(&fa, &fk)) {
                    const int slot = (fk == 1) ? orangeTool_ : appleTool_;
                    const bool room = drops_.count() < kDropSlots;
                    if (slot >= 0 && room && !held_.tool(slot).models.empty()) {
                        const Tool &t = held_.tool(slot);
                        drops_.spill(slot, t.models[0], t.sx, t.sy, t.sz, fa,
                                     float(nth) * 2.39996f);   // the golden angle apart
                    } else {
                        world_.spawnFruitBody(physics_, fa, fk, simMs_);
                        ++asBodies;
                    }
                    ++nth;
                }
                if (nth > 0) {
                    std::printf("  fell     %d fruit came down with the tree: %d to pick up, "
                                "%d as bodies (the drops were full)\n",
                                nth, nth - asBodies, asBodies);
                    std::fflush(stdout);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // THE BOULDERS NEARBY, AS COLLISION.
    //
    // A chip must not pass through the stone it came off, and the only way to
    // mean that is to put the stone in the solver. A model's colTop is already
    // a height field -- the top of every voxel column, and the very surface the
    // player is collided against -- so it goes in as one, shared across every
    // placement of the same model.
    //
    // ONLY WHAT IS NEAR, and diffed rather than rebuilt: a static actor costs
    // nothing to leave alone and something to create, and the set changes only
    // when the player walks. Keyed by chunk and decor slot, which is what makes
    // a DAMAGED boulder -- carrying its own colTop -- replace its own actor
    // rather than keeping the pristine shape.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // THE ONE PLACE THAT DECIDES WHETHER THE SOLVER'S FLOOR NEEDS REBUILDING.
    //
    // It was written out at SIX call sites as
    // `takeGroundDirty() || !groundCovers(...)`, and that || SHORT-CIRCUITS --
    // so on any frame with an edit in it the coverage question was never put.
    // Which is the question that decides whether the patch may stay where it
    // is, and therefore whether the re-sample can be a partial one. Six copies
    // of a decision is also five places for the next change to miss.
    void maybeRebuildGroundPatch() {
        if (!physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
            rebuildGroundPatch(true);        // the player has walked off it
        else if (world_.groundDirty())
            rebuildGroundPatch(false);       // it moved under him; keep the origin
    }

    // -----------------------------------------------------------------------
    // `recentre` FALSE KEEPS THE PATCH WHERE IT IS AND ONLY RE-SAMPLES.
    //
    // (user 2026-09-17: "keep pursuing the hoe glitch".)
    //
    // THE ORIGIN FOLLOWED THE PLAYER AT VOXEL GRANULARITY, so it moved on every
    // step -- and a partial re-sample is only valid over a patch at the SAME
    // origin, so the first attempt at this measured 35 rebuilds and 35 of them
    // FULL. The dirty box was right and could never be used.
    //
    // It did not need to move. The patch is 64 m across and the caller has just
    // checked groundCovers: if the player is still well inside it, the patch is
    // in a perfectly good place and recentring is work nobody asked for. So a
    // rebuild triggered by an EDIT keeps the origin and re-samples the bite
    // alone, and only one triggered by the player reaching the margin moves it
    // -- which is the rebuild that was always going to be full anyway.
    void rebuildGroundPatch(bool recentre = true) {
        const int n = kGroundPatchCols;
        const int step = kGroundPatchStep;
        const bool keep = !recentre && groundHave_ &&
                          groundPatch_.size() == size_t(n) * size_t(n);
        const int i0 = keep ? groundI0_
                            : int(std::floor(player_.pos.x / VOXEL_M)) - (n / 2) * step;
        const int j0 = keep ? groundJ0_
                            : int(std::floor(player_.pos.z / VOXEL_M)) - (n / 2) * step;
        groundPatch_.resize(size_t(n) * size_t(n));
        // THE MEMO IS THE WHOLE COST HERE. heightVox is several octaves of
        // noise; asked cold, 16 384 columns is a chunk's worth of meshing on
        // the frame the player crosses the boundary. The memo is what the
        // chunk mesher uses for the same reason -- i on the inside, so a
        // lattice cell is reused across a run of columns.
        // ...AND IT HAS THE HOLES IN IT. See World::groundPatch: sampled
        // from heightVox alone this floor had a lid over every pit the player
        // had dug, and anything born under that lid was thrown out of it.
        // -- ONLY THE PART THAT MOVED, WHEN THE PATCH HAS NOT ---------------
        //
        // (user 2026-09-17: "keep pursuing the hoe glitch".)
        //
        // A hoe bite changes seven cells by seven and this re-sampled 25,600,
        // because the dirty FLAG could not say where. World::markGroundDirty
        // records the box now, so the common case -- standing and tilling,
        // where the patch origin does not move at all -- re-samples the bite
        // and leaves the rest of groundPatch_ alone.
        //
        // THE ORIGIN IS THE WHOLE PRECONDITION. A partial sample is only valid
        // OVER the previous one, so the moment the player walks far enough to
        // move i0/j0 the array means something else and the box is worthless:
        // that run has to be a full one. Same when nothing is pending, which is
        // the groundCovers path that gets here without an edit at all.
        int bi0 = 0, bj0 = 0, bi1 = 0, bj1 = 0;
        const bool box = world_.groundDirtyBox(&bi0, &bj0, &bi1, &bj1);
        const bool partial = box && groundHave_ && i0 == groundI0_ && j0 == groundJ0_;
        const auto gt0 = std::chrono::steady_clock::now();
        if (partial)
            world_.groundPatch(groundPatch_.data(), n, i0, j0, step, bi0, bj0, bi1, bj1);
        else
            world_.groundPatch(groundPatch_.data(), n, i0, j0, step);
        groundI0_ = i0;
        groundJ0_ = j0;
        groundHave_ = true;
        const auto gt1 = std::chrono::steady_clock::now();
        world_.takeGroundDirty();
        physics_.setGroundPatch(groundPatch_.data(), n, i0, j0, VOXEL_M, step);
        if (opt_.hitch) {
            const auto gt2 = std::chrono::steady_clock::now();
            std::printf("  [ground] %s sample %.2f ms   physx %.2f ms\n",
                        partial ? "part" : "FULL",
                        std::chrono::duration<double, std::milli>(gt1 - gt0).count(),
                        std::chrono::duration<double, std::milli>(gt2 - gt1).count());
            std::fflush(stdout);
        }
    }

    // SIXTY-FOUR METRES OF GROUND, for the same 25,600 samples the sixteen
    // metres used to cost -- 160 a side, as before, one every four voxels.
    //
    // The width is set by the longest thing that can fall on it. A felled pine
    // is twenty-six metres, so its crown lands well outside a sixteen-metre
    // patch and there was nothing under it: the tree went through the world.
    // Nothing walks on this height field -- the player's ground is answered on
    // the CPU against the voxel columns -- so trading resolution for reach
    // costs a chip a few centimetres of accuracy on a slope and buys a tree a
    // floor to land on.
    //
    // And it is CHEAPER in practice than what it replaces: four times the width
    // means the player leaves it far less often, and leaving it is the only
    // thing that rebuilds it.
    static constexpr int kGroundPatchCols = 160;
    static constexpr int kGroundPatchStep = 4;
    static constexpr float kGroundMarginM = 4.0f;
    std::vector<int16_t> groundPatch_;
    // Where groundPatch_ was last sampled from, so a partial re-sample can tell
    // whether the array it is amending still means the same thing. See
    // rebuildGroundPatch.
    int groundI0_ = 0, groundJ0_ = 0;
    bool groundHave_ = false;
    // WHAT THE LAST BLOW TOOK OUT, kept as a member so a swing does not
    // allocate: dig and carveModel fill it with the voxels actually removed,
    // and the piece that flies at you is meshed from exactly those.
    std::vector<uint8_t> spoilVol_;
    int spoilN_ = 0;
    // Turned once per blow so two chips never tumble the same way.
    uint32_t swingSalt_ = 0;
    // Where the last blow actually bit, in world metres.
    Vec3 spoilAt_{0, 0, 0};
    // ...and the quarter turn of whatever it was cut out of.
    float spoilYaw_ = 0.0f;
    // Whether the last blow was the one that put a tree on the ground. Read by
    // the swing log and nothing else.
    bool felled_ = false;
    // Whether the spawn has been checked against a world that actually exists.
    // See the note beside it in the frame loop.
    bool spawnSettled_ = false;
    double simMs_ = 0.0;
    // The falling snow's own clock, in seconds on the WALL -- see the block
    // that advances it, and V6Params::snowTime.
    double snowClock_ = 0.0;
    // How much of the blanket has settled, 0 to 1, and which STEP of it the
    // world was last meshed for -- see the accumulation block in onFrameRender.
    float snowLay_ = 0.0f;
    int snowStep_ = 0;
    // Over how long a full blanket arrives, and in how many re-meshes.
    static constexpr float kSnowBuildSec = 100.0f;
    static constexpr int kSnowSteps = 5;

    // -----------------------------------------------------------------------
    // THE SAME WORLD, WITH THE COLLIDERS GATHERED FURTHER OUT.
    //
    // walkWorld's six metres is right for a body: it is what you can walk into
    // this frame. Anything that TRAVELS needs its own reach, and the list is
    // the only thing that changes -- the terrain, the edits and the level are
    // the same world however far you are looking.
    //
    // ITS OWN VECTOR, so a wide gather cannot leave the walk's list holding
    // half the wood for the rest of the frame.
    WalkWorld wideWalkWorld(float reachM) {
        world_.collidersNear(player_.pos, reachM, &wideSolids_);
        if (world_.levelOn()) {
            Solid lv;
            if (world_.levelSolid(&lv)) wideSolids_.push_back(lv);
        }
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = wideSolids_.data();
        w.solidCount = int(wideSolids_.size());
        return w;
    }

    // -- THE GROUND ONLY, WITH NO COLLIDER QUERY ----------------------
    //
    // walkWorld below re-runs collidersNear on every call -- a spatial query
    // that refills solids_ -- because the body it is built for has to know
    // about trunks and boulders as well as the floor.
    //
    // THE LIFE'S GROUND FUNCTION ASKS ONE QUESTION, "how high is the ground
    // here", and walkGroundM answers it out of terrain and edits alone. Built
    // from walkWorld it would have run a collider gather PER PROBE PER ANIMAL
    // PER FRAME and rewritten solids_ under everything else holding it -- a
    // hitch and an aliasing bug bought for a number that does not use the
    // field. So: the two pointers the probe reads, and nothing else.
    WalkWorld groundWorld() {
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        return w;
    }

    WalkWorld walkWorld() {
        world_.collidersNear(player_.pos, 6.0f, &solids_);
        // THE LEVEL IS NOT IN A CHUNK, so collidersNear cannot find it: that
        // walk goes through the resident chunk map and this building is four
        // kilometres from the nearest chunk and 640 m over it. One solid,
        // appended by hand, which is the whole of the level's physics -- see
        // World::levelSolid and Solid::interior.
        if (world_.levelOn()) {
            Solid lv;
            if (world_.levelSolid(&lv)) solids_.push_back(lv);
        }
        WalkWorld w;
        w.terrain = &world_.terrain;
        // ...AND WHERE THE HOLES ARE. The ground is the generator plus the
        // edits, and a swing that asked only the first could not see a pit it
        // had dug a moment ago -- see TerrainProbe.
        w.edits = &world_.editStore();
        w.solids = solids_.data();
        w.solidCount = int(solids_.size());
        return w;
    }

    #include "world/app_sun.inl"
    #include "ui/app_locate.inl"
    #include "platform/app_capture.inl"
    #include "platform/app_tests_terrain.inl"
    #include "player/app_actions.inl"
    #include "platform/app_tests_interaction.inl"
    #include "platform/app_offline.inl"
