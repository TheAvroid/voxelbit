// app_actions.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: planting, firing, killing, tilling, picking, breaking
// -----------------------------------------------------------------------------
    void publishLife() {
        flock_.publish(world_);
        birds_.publish(world_);
        lake_.publish(world_);
        flock2_.publish(world_, kButterflySlots + kBirdSlots + kLakeSlots);
        bunnies_.publish(world_, kBunnySlot0);
        bunnies_.publishSkunks(world_, kMarchSlot0);
        bees_.publish(world_, kBeeSlot0);
        critters_.publish(world_, kCritterSlot0);
        world_.flushFlyerInstances();
    }

    void warmLife(const Vec3 &at, int seconds) {
        const auto groundAt = [this](float x, float z) {
            return float(world_.terrain.heightVox(int(std::floor(x / VOXEL_M)),
                                                  int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
        };
        world_.collidersNear(at, kBirdKeepM, &perches_);
        world_.decorNear(5, at, kBeeHiveM, &hivesNear_);
        world_.decorNear(2, at, kBeeHiveM + kBeeFlowerM, &bloomsNear_);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < seconds * 60; ++i) {
            flock_.update(dt, world_, at);
            birds_.update(dt, perches_, at);
            lake_.update(dt, world_.terrain, at);
            flock2_.update(dt, at, groundAt, &perches_);
            bunnies_.update(dt, at, groundAt,
                            [this](float x, float z) { return wetColumnAt(x, z); }, perches_,
                            [this](float x) { return world_.terrain.woodBit(x); },
                            // ...AND NOT ON THE BEACH -- see Bunnies::blocked.
                            [this](float x, float z) { return sandAt(x, z); });
            bees_.update(dt, at, hivesNear_, bloomsNear_, &perches_);
            lake_.bankSpots(uint32_t(frameTick_), 8, &banksNear_);
            critters_.update(dt, at, groundAt,
                             [this](float x, float z) { return wetColumnAt(x, z); },
                             [this](float x) { return world_.terrain.woodBit(x); }, banksNear_,
                             perches_, isNight(), Vec3(0.0f, 0.0f, 0.0f),
                             [this](float x, float z) { return waterTopAt(x, z); });
        }
    }

    // -----------------------------------------------------------------------
    // PUT A SEED IN THE GROUND UNDER THE CROSSHAIR.
    //
    // (user 2026-09-14: "have it where the player can right click tilled land
    // with seeds to place down seeds. however if there is no water present, the
    // tilled land goes back to dirt/grass like it does currently.")
    //
    // WHAT PLANTING ACTUALLY IS HERE: the till's revert is CANCELLED. Turned
    // earth grows back over after kTillSec "since nothing was ever planted in
    // it" -- v1's own words for its own rule -- so the whole of "something has
    // been planted in it" is that the clock stops. Nothing new is drawn, no
    // second kind of voxel exists, and there is no crop system behind this.
    //
    // AND ONLY IF THERE IS WATER. That is the second half of the ask and it is
    // what makes a seed bed a PLACE rather than a thing you do anywhere: a bed
    // out of reach of water is turned earth with a seed in it, and turned earth
    // grows back. So a dry planting costs you the seed and the bed reverts on
    // its own clock, exactly as it does now.
    //
    // THE SEED IS SPENT EITHER WAY, which is deliberate. Refusing to plant
    // without water would need the rule explained in the moment -- there is no
    // UI here to explain it in -- and "I planted it and it did not take" is a
    // thing a player can see happen and learn from.
    // -----------------------------------------------------------------------
    bool plantSeed() {
        if (seedsTool_ < 0 || !held_.ready()) return false;
        if (held_.selected() != seedsTool_) return false;
        if (!held_.tool(seedsTool_).carried) return false;

        const Vec3 eye = player_.eyePosition(), dir = forward();
        const Swing aim = swingRay(walkWorld(), eye, dir);
        // TILLED ONLY, NOT SEEDED. A bed that already has a seed in it is not
        // a bed to put a second one in -- and saying so here is what stops a
        // held stack draining into one square metre of dirt.
        if (!aim.hit || aim.kind != Swing::Ground || aim.material != mat::TILLED) return false;

        const bool wet = waterWithin(aim.point, kSeedWaterM);
        const size_t held = world_.plantAt(aim.point, kTillRadiusM, wet);
        if (!held) return false;

        // ONE SEED PER PLANTING, taken through the same door a drop leaves by:
        // dropSelected is what decrements a stack and gives the slot up when
        // the last one goes. See HeldItem::take.
        held_.dropSelected();
        std::printf("v2: planted at (%.1f, %.1f, %.1f) -- %s, %zu columns\n", aim.point.x,
                    aim.point.y, aim.point.z,
                    wet ? "water in reach, the bed keeps" : "NO WATER, it will grow back over",
                    held);
        std::fflush(stdout);
        return true;
    }

    // IS THERE WATER WITHIN REACH OF THIS BED. A ring search rather than a
    // square, and it stops at the first wet column -- the answer is a bool and
    // the nearest one is not more true than any other.
    bool waterWithin(const Vec3 &at, float reachM) {
        for (float r = 0.0f; r <= reachM; r += 0.5f) {
            const int steps = (r < 0.5f) ? 1 : maxi(8, int(2.0f * PI * r / 0.5f));
            for (int k = 0; k < steps; ++k) {
                const float a = float(k) / float(steps) * 2.0f * PI;
                if (wetColumnAt(at.x + cosf(a) * r, at.z + sinf(a) * r)) return true;
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // A SHAFT FINDS SOMETHING ALIVE -- v1's one-shot bow.
    //
    // The same consequences a killing blow has, in the same order, and reached
    // through the same two calls -- so an arrow cannot drift away from what an
    // axe does. What it does NOT share is the reach test: a melee swing asks
    // what is under the crosshair within 5.3 m, and an arrow is simply AT the
    // point it has flown to.
    // -----------------------------------------------------------------------
    bool arrowKill(const Vec3 &p) {
        if (!world_.flyersLoaded()) return false;
        const int slot = lifeHits_.at(world_, p);
        if (slot < 0) return false;
        const LifeKind kind = lifeKindAt(slot);
        if (!kind.alive()) return false;
        // ONE SHOT, WHATEVER IT IS. v1's arrow count is 1 for everything.
        const LifeHits::Blow b = lifeHits_.strike(world_, slot, kind, /*oneBlow=*/true, simMs_);
        if (!b.landed) return false;
        particles_.hitSparks(b.at, simMs_, /*red=*/true);
        if (!b.killed) return true;
        lastPieces_ = world_.shatterFlyer(physics_, slot, p, simMs_);
        particles_.deathBurst(b.at, simMs_);
        if (kind.meat) dropMeatAt(b.at);
        killLifeAt(slot);
        lifeHits_.clear(slot);
        std::printf("v2: an arrow killed the %s -- %d pieces%s\n", kind.name, lastPieces_,
                    kind.meat ? ", left a steak" : "");
        std::fflush(stdout);
        return true;
    }

    // -----------------------------------------------------------------------
    // A SHAFT TAKES A CHIP OUT OF WHAT IT HIT -- v1's arrowChop.
    //
    // (user 2026-09-15: "have arrow take out tiny peices of material. v1 did
    // this. the peice should be as big as the knife chunk as seen in v1".)
    //
    // HOW BIG IS "THE KNIFE CHUNK". v1 measures a bite as a COUNT of voxels
    // lifted out of a sphere, and the knife's is
    //
    //     max(4, round(chopBite * KNIFE_BITE * KNIFE_SCALE))
    //         = max(4, round(30 * 0.5 * 0.5)) = 8 voxels
    //
    // (v1's own arrow takes ARROW_CHOP_BITE = 10, so the user's "as big as the
    // knife chunk" is asking for slightly less than v1's arrow, not for a
    // different order of thing.)
    //
    // THIS ENGINE HAS NO BITE COUNT -- a carve here is the whole sphere -- so
    // the size is set by the RADIUS, and the radius had to be MEASURED rather
    // than worked out. The arithmetic says a radius-1 sphere is the 7 cells
    // within one of the centre, which looked like the answer; the engine says
    //
    //     radius 1   3 voxels     radius 2   5 voxels     kDigRadiusVox 3   113
    //
    // because the sphere is centred on the voxel the shaft went INTO, which is
    // at the surface -- so a good half of it is the air the arrow flew through,
    // and how much of the rest is stone depends on how square-on it struck.
    // Radius 2 is what actually lands nearest v1's eight.
    //
    // AGAINST THE SWING'S 113, which is the ratio that matters: an axe takes
    // twenty times this out of the same boulder. A shaft chips; it does not
    // dig, and there is no radius here that could be mistaken for one.
    //
    // NOTHING ALIVE IS CARVED, which is v1's rule stated the same way ("a
    // creature in the way cancels the chop outright") and here it is free --
    // Arrows only records an impact for a shaft that struck the WORLD, because
    // one that struck an animal took the kill branch and never reached this.
    // -----------------------------------------------------------------------
    static constexpr int kArrowChipVox = 2;
    // -- ...AND A BULLET'S HOLE IS ONE VOXEL SMALLER --------------------
    //
    // (user 2026-09-17: "I want you decrease the sphere of the bullet impact
    //  by 1 voxel.")
    //
    // ITS OWN CONSTANT, DERIVED FROM THE ARROW'S rather than written as a 1.
    // The two impacts are the same mechanism through the same function -- see
    // arrowChip's chipVox -- and the only thing that differs is how big the
    // bite is, so saying that in the constant keeps the relationship visible:
    // move the arrow and the round follows it down.
    //
    // A RADIUS, SO THE SPHERE LOSES A VOXEL IN EVERY DIRECTION. carveLevel
    // keeps `dx*dx + dy*dy + dz*dz <= r*r`, so 2 -> 1 takes the hole from 33
    // voxels to 7 and its width from five voxels across to three.
    static constexpr int kBulletChipVox = kArrowChipVox - 1;

    // -----------------------------------------------------------------------
    // ONE ROUND, FROM THE MUZZLE, STRAIGHT DOWN THE AIM.
    //
    // (user 2026-09-17: "create a 1 voxel bullet when it shoots ... the bullet
    // should appear at the tip of the gun and travel straight.")
    //
    // WHERE IT COMES FROM AND WHERE IT GOES ARE TWO DIFFERENT ANSWERS, and
    // that is deliberate rather than sloppy. The round is BORN at the barrel,
    // because a tracer that starts in the middle of your face is the thing the
    // ask is about -- but it travels along the AIM, not along the barrel's own
    // axis, because the barrel is 70 cm to the right of the eye and sways with
    // the walk. Firing down the barrel's axis would put the shots wherever the
    // bob had left the gun that frame, which is not a weapon anybody can use.
    // Every FPS ever made does exactly this and for exactly this reason.
    //
    // THE MUZZLE IS ASKED OF THE HAND, not reconstructed here -- see
    // HeldItem::muzzle, which finds the far end of the model through the pose
    // that is actually on screen this frame, recoil included. If the hand
    // cannot answer (no camera yet, tool hidden) the round leaves from the eye,
    // which is invisible rather than wrong.
    // -----------------------------------------------------------------------
    // IS THE GUN THE THING IN THE HAND RIGHT NOW.
    //
    // Four conditions and every one of them is load-bearing: the art loaded,
    // the kit is built, the wheel is pointed at it, and it has not been stowed
    // at the level's door. Written once here because three callers want it --
    // the trigger, the swing it suppresses, and [R] -- and three copies of a
    // four-term condition is how one of them ends up a term short.
    // -----------------------------------------------------------------------
    bool rifleInHand() const {
        return rifleTool_ >= 0 && held_.ready() && held_.selected() == rifleTool_ &&
               held_.carrying();
    }

    // -----------------------------------------------------------------------
    // IS THERE A GUN IN THE HAND AT ALL -- either of them.
    //
    // (user 2026-09-18: "import the pistol asset into the fps mode. put it in
    // the inventory".)
    //
    // NOT THE SAME QUESTION AS rifleInHand, AND CONFLATING THEM WOULD FIRE THE
    // WRONG WEAPON. What the rifle's slot decides is what the TRIGGER does --
    // it spends the rifle's magazine, kicks the rifle's recoil and draws the
    // rifle's ammo count. What THIS decides is only what the left button must
    // NOT do, which is swing.
    //
    // A GUN DOES NOT SWING, and that is the whole of why this exists. Every
    // other tool in the kit spends the left button on the swing curve and the
    // bite HeldItem::update times off it, so a pistol that was not named here
    // would be swung at the map like an axe and would carve nuketown with its
    // grip. The rifle has had that exclusion since it learned to fire; the
    // pistol needs it from the moment it is holdable, which is now.
    // -----------------------------------------------------------------------
    bool pistolInHand() const {
        return pistolTool_ >= 0 && held_.ready() && held_.selected() == pistolTool_ &&
               held_.carrying();
    }
    // -----------------------------------------------------------------------
    // WHICH GUN IS IN THE HAND, as a kit slot, or -1 for none.
    //
    // (user 2026-09-18: "left clicking the pistol is not fireing bullets. fix
    // that.")
    //
    // THE TRIGGER USED TO ASK rifleInHand() AND THAT IS WHY. Everything below
    // this line -- the magazine, the cyclic rate, the reload, the badge -- was
    // written against one gun and named after it, so the pistol arrived holding
    // a model and nothing else. It is one question now and the four small
    // lookups under it are the only places either gun is named.
    //
    // NOT AN ENUM AND NOT A TABLE. There are two guns; a `Guns[]` indexed by a
    // second id would have to be kept in step with the kit slots that already
    // identify them, and a slot IS the identity here -- it is what the wheel,
    // the badge, standInLevel and snapshotKit all key on.
    // -----------------------------------------------------------------------
    int heldGun() const {
        if (rifleInHand()) return rifleTool_;
        if (pistolInHand()) return pistolTool_;
        return -1;
    }
    bool holdingGun() const { return heldGun() >= 0; }
    bool isGun(int tool) const {
        return tool >= 0 && (tool == rifleTool_ || tool == pistolTool_);
    }
    int gunMagOf(int tool) const { return tool == pistolTool_ ? kPistolMag : kRifleMag; }
    int gunAmmoOf(int tool) const { return tool == pistolTool_ ? pistolAmmo_ : rifleAmmo_; }
    void setGunAmmo(int tool, int n) {
        if (tool == pistolTool_) pistolAmmo_ = n;
        else if (tool == rifleTool_) rifleAmmo_ = n;
    }
    // HOW OFTEN IT MAY GO OFF. See kPistolIntervalMs for why they differ.
    double gunIntervalOf(int tool) const {
        return tool == pistolTool_ ? kPistolIntervalMs : kBulletIntervalMs;
    }

    // -----------------------------------------------------------------------
    // PUT A FRESH MAGAZINE IN. Returns true if a cycle actually started.
    //
    // (user 2026-09-18: "so also implement a reload function while you are at
    // it. reload with r.")
    //
    // THE ROUNDS ARRIVE AT THE END, NOT HERE -- see the reloadDone() poll in
    // onFrameRender. A magazine that filled the moment the key was pressed
    // would let a player tap [R] between two shots and never run dry, which is
    // the animation being decorative rather than being the cost.
    //
    // A FULL GUN DOES NOTHING. Not a no-op for tidiness: without it, [R] on a
    // full magazine is 1800 ms of standing there with the gun unable to fire,
    // and the player's own keypress is the last thing they would suspect.
    // -----------------------------------------------------------------------
    bool reloadGun() {
        const int tool = heldGun();
        if (tool < 0) return false;
        if (gunAmmoOf(tool) >= gunMagOf(tool) || held_.reloading()) return false;
        // -- HOW MANY TURNS OF THE STRIP THIS IS ---------------------------
        //
        // (user 2026-09-18: "it needs to play multiple times depending on how
        // many shots have been powered. its a standard revolver with 6
        // rounds.")
        //
        // ONE PER EMPTY CHAMBER for a gun whose strip loads rounds
        // (Tool::reloadRounds), and exactly one for a gun whose strip is a
        // magazine change. That is the only place the two kinds differ: the
        // clock, the frames, the key and the badge are all the same code.
        //
        // COUNTED HERE, BEFORE THE FIRST TURN, so the animation and the
        // magazine cannot disagree -- and so that reloading with two rounds
        // left is two turns rather than six.
        const int per = held_.tool(tool).reloadRounds;
        const int missing = gunMagOf(tool) - gunAmmoOf(tool);
        const int cycles = per > 0 ? (missing + per - 1) / per : 1;
        if (!held_.startReload(cycles)) return false;
        if (opt_.swingLog) {
            std::printf("v2: %s reload started -- %d of %d rounds left, %d turn(s)\n",
                        held_.tool(tool).name, gunAmmoOf(tool), gunMagOf(tool), cycles);
            std::fflush(stdout);
        }
        return true;
    }

    // Returns true if a round actually left the barrel -- false on an empty or
    // busy gun, which is what the trigger and the scripted burst both need to
    // know before they report a shot that never happened.
    bool fireGun() {
        // WHICH GUN, ASKED ONCE AT THE TOP. Everything below spends THIS
        // slot's magazine, at this slot's rate -- see heldGun.
        const int tool = heldGun();
        if (tool < 0) return false;
        // -- NOTHING COMES OUT OF A GUN THAT IS BEING RELOADED ---------------
        //
        // FIRST, BEFORE THE AMMO TEST, because the magazine is still empty for
        // the whole of the cycle -- the rounds arrive at the end. Test the
        // count first and an empty gun re-arms its own reload on every trigger
        // frame, restarting the animation five times a second and never
        // finishing it. (startReload refuses a second one, so the visible
        // symptom would have been a gun that simply never reloaded.)
        if (held_.reloading()) return false;
        // -- ...AND AN EMPTY ONE RELOADS INSTEAD OF FIRING ------------------
        //
        // (user 2026-09-18: "the number goes down until 0, where the gun then
        // reloads".)
        //
        // TWO ROADS TO THE SAME CALL, and they are not the same moment. This is
        // the trigger being pulled on an empty gun; the one at the bottom of
        // this function is the shot that EMPTIED it, which starts the cycle
        // straight away rather than making the player pull once more to
        // discover there is nothing left.
        if (gunAmmoOf(tool) <= 0) {
            reloadGun();
            return false;
        }
        setGunAmmo(tool, gunAmmoOf(tool) - 1);
        const Vec3 aim = forward();
        Vec3 from = pos_;
        // The same camera the frame is about to be drawn with, built the way
        // onFrameRender builds it -- the muzzle is a point in the VIEW's frame,
        // so anything else here would put the round somewhere the player is not
        // looking.
        Camera cam;
        cam.origin = pos_;
        cam.target = pos_ + aim * 50.0f;
        cam.fovDeg = fov_;
        cam.aperture = 0.0f;
        cam.focusDist = 40.0f;
        const V6Camera gcam = cam.gpu(tracer_.width(), tracer_.height());
        Vec3 tip{0, 0, 0};
        const bool gotTip = held_.muzzle(gcam, player_.bobPhase, player_.bobAmp, &tip);
        if (gotTip) from = tip;
        // -- AND IT FLIES TO WHERE YOU ARE POINTING, NOT PARALLEL TO IT ------
        //
        // (user 2026-09-17: "make sure the bullets are coming directily out of
        // the tip of the gun".)
        //
        // The round is BORN at the barrel and used to travel along the view's
        // own forward vector -- which is 70 cm to the left of it and 35 cm up.
        // Two parallel lines: the tracer left the muzzle correctly and then ran
        // beside the crosshair for ever, which from behind the gun reads as a
        // round that came out of somewhere else.
        //
        // So it is aimed at a point ON the view axis, kBulletConvergeM away.
        // The line now starts at the barrel and passes through the crosshair,
        // which is what a sighted-in weapon does and what the eye expects.
        const Vec3 converge = pos_ + aim * kBulletConvergeM;
        Vec3 dir = converge - from;
        if (lengthSq(dir) < 1e-6f) dir = aim;
        bullets_.launch(from, dir);
        held_.kick();
        // THE BANG GOES WITH THE ROUND, on the frame it leaves the barrel and
        // not with the click -- everything above this line can still refuse the
        // shot (dry, reloading, inside the repeat interval), and a gun that
        // cracked on a trigger pull that fired nothing would be the one way
        // this could lie about what happened. See ToolSounds::gunFired.
        toolSfx_.gunFired();
        // ONE LINE PER ROUND UNDER --swing-log, exactly as the arrow's flight
        // reports under the same flag. A tracer at 120 m/s is gone in a frame
        // or two, so this is the only way to see that the muzzle is where it
        // should be without photographing it.
        if (opt_.swingLog) {
            std::printf("v2: round from (%.2f %.2f %.2f) eye (%.2f %.2f %.2f) %s  %d/%d left\n",
                        from.x, from.y, from.z, pos_.x, pos_.y, pos_.z,
                        gotTip ? "MUZZLE" : "NO MUZZLE -- fell back to the eye",
                        gunAmmoOf(tool), gunMagOf(tool));
            std::fflush(stdout);
        }
        // -- ...AND THE VIEW CLIMBS A LITTLE --------------------------------
        //
        // A gun that kicks the model and not the camera reads as a toy. This is
        // deliberately small -- kRifleClimbDeg is a third of a degree, so a
        // ten-round burst walks the aim up about three and a half -- and it is
        // NOT decayed back down on purpose: pulling the muzzle back onto the
        // target is the player's job, which is the whole of what recoil means
        // in a game. Clamped through the same limiter the mouse goes through so
        // sustained fire cannot flip the camera over backwards.
        pitch_ = clampf(pitch_ + kRifleClimbDeg, -89.0f, 89.0f);
        // -- ...AND THE ROUND THAT EMPTIED IT STARTS THE NEXT MAGAZINE ------
        //
        // (user 2026-09-18: "the number goes down until 0, where the gun then
        // reloads".)
        //
        // AT THE BOTTOM, AFTER THE SHOT HAS BEEN FIRED AND REPORTED. The last
        // round is a round like any other -- it leaves the barrel, chips what
        // it hits and climbs the view -- and only then is the gun empty.
        if (gunAmmoOf(tool) <= 0) reloadGun();
        return true;
    }

    // `chipVox` is the bite's radius in voxels. It is a PARAMETER and not
    // kArrowChipVox because a bullet's hole is smaller than an arrow's -- see
    // kBulletChipVox -- and everything else about the two impacts is the same,
    // which is the whole reason a rifle round comes through the arrow's door.
    // -----------------------------------------------------------------------
    // EVERY BURNING FLECK IN THE WORLD, AS A LIGHT.
    //
    // (user 2026-09-17: "can you make the spark voxel emit volumetric light
    //  like the lightbulbs? this would effect the gun bullet and the spark
    //  animation. but they use the same voxel anyway.")
    //
    // AND THEY DO USE THE SAME VOXEL, which is why this is one function rather
    // than two: a rifle tracer IS a spark (Bullets::useModel takes the spark
    // model, so a round costs no model, no material and no palette entry), and
    // the sparks a blow throws are the same material again. One list covers
    // both because there was only ever one thing.
    //
    // WHAT A SPARK WAS BEFORE THIS: an emissive material and nothing else. A
    // voxel wearing it draws at its own radiance -- and only on a camera ray
    // (`depth == 0` in Trace.cs.slang) -- so it was a bright fleck on the
    // screen that lit neither the room it was in nor the air it was flying
    // through. Both halves of that are what the ask names.
    //
    // THE SAME LIST THE LAMPS ARE IN, and that is the whole design. V6Params::
    // bulbs is the engine's point-light array; sampleBulbSplit already picks
    // the nearest entry per shading point and spends ONE shadow ray on it, so
    // an ember costs nothing per pixel that a pendant did not already cost.
    // The fog march reads the same array for the volumetric half.
    //
    // A GAIN PER LIGHT, because an ember is not a 60 W bulb. bulbs[].w was
    // already being written as a constant 1.0f, so it was free to become this
    // and a lamp's behaviour is unchanged by construction.
    //
    // THE COLOUR IS SHARED AND IN THE LEVEL IT IS THE LAMP'S. There is one
    // bulbRadiance for the whole array, so where a map has pendants the sparks
    // borrow their warm white instead of the ember's amber. That is a real
    // approximation and it is the right one to make: a muzzle flash reading as
    // warm white in a lit room is not something anybody can see, and the
    // alternative is a second light array with a second shadow ray for four
    // voxels that live 0.4 s. In the WOOD there are no pendants, so the array
    // is all sparks and it gets the ember's own colour.
    //
    // NEAREST FIRST, AND THE LAMPS KEEP THEIR PLACES. kBulbSlots is twelve and
    // a map can want more pendants than that already, so the embers take a few
    // slots off the END and only when there are embers to put there -- a room
    // does not lose a lamp because nobody is shooting.
    // -----------------------------------------------------------------------
    static constexpr int kSparkLightSlots = 3;
    // An ember against a lamp, as a fraction of bulbRadiance. A spark is a
    // 10 cm fleck and a pendant lights a room; a fifth is enough to throw a
    // visible pool on the wall beside a burst without the flash reading as a
    // second ceiling light.
    static constexpr float kSparkLightGain = 0.20f;

    void publishSparkLights() {
        tracer_.bulbGain.assign(tracer_.bulbs.size(), 1.0f);
        if (!particles_.ready() && bullets_.inFlight() <= 0 && !critters_.sharesSpark()) return;

        // The nearest few, by distance from the eye -- a spark across the wood
        // lights nothing anybody is looking at, and the slots are scarce.
        struct Near { float d2; Vec3 p; };
        Near best[kSparkLightSlots];
        int have = 0;
        const Vec3 eye = pos_;
        auto offer = [&](const Vec3 &p) {
            const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            // Past this an ember contributes less than a thousandth of what it
            // does at arm's length, and the fog march rejects it anyway.
            if (d2 > 576.0f) return;
            int at = have;
            if (have < kSparkLightSlots) ++have;
            else if (d2 >= best[kSparkLightSlots - 1].d2) return;
            else at = kSparkLightSlots - 1;
            while (at > 0 && best[at - 1].d2 > d2) { best[at] = best[at - 1]; --at; }
            best[at] = Near{d2, p};
        };
        // THE EMBERS. Smoke is skipped: it wears its own material and is a puff
        // of grey, not a light -- see the note over kSmokeEmit.
        Vec3 sp{0.0f, 0.0f, 0.0f};
        bool smoke = false;
        for (int i = 0; i < kParticleSlots; ++i)
            if (particles_.at(i, &sp, &smoke, simMs_) && !smoke) offer(sp);
        // ...AND THE ROUNDS IN FLIGHT, which are the same voxel travelling.
        for (int i = 0; i < kBulletSlots; ++i)
            if (bullets_.at(i, &sp)) offer(sp);
        // ...AND THE FIREFLIES, once they wear that voxel too (user 2026-09-17:
        // "have the lightning bug at night share the same lit voxel as the
        // spark voxel"). Sharing the material makes one DRAW the same; sharing
        // this list is what makes it LIGHT the same, which is the half a
        // material cannot carry.
        if (critters_.sharesSpark())
            for (int i = 0; i < critters_.fireflySlots(); ++i)
                if (critters_.fireflyAt(i, &sp)) offer(sp);
        if (have <= 0) return;

        // Room at the end of the array, taken from the lamps only now that
        // there is something to put there.
        const size_t keep =
            size_t(maxi(0, int(kBulbSlots) - have));
        if (tracer_.bulbs.size() > keep) tracer_.bulbs.resize(keep);
        tracer_.bulbGain.assign(tracer_.bulbs.size(), 1.0f);
        for (int i = 0; i < have; ++i) {
            tracer_.bulbs.push_back(best[i].p);
            tracer_.bulbGain.push_back(kSparkLightGain);
        }
        // IN THE WOOD THE ARRAY IS ALL EMBER, so it gets the ember's own
        // colour and its own switch. bulbMtl stays kNoBulb -- there is no glass
        // out here and the material test must not fire on a pine -- which is
        // exactly why sampleBulbSplit is gated on the LIST rather than on the
        // material now.
        if (tracer_.bulbMtl == 0xFFFFFFFFu) {
            tracer_.bulbRadiance = Vec3(kSparkEmit[0], kSparkEmit[1], kSparkEmit[2]);
            tracer_.bulbRadius = 0.05f;   // a 10 cm fleck, so half of one
            tracer_.bulbPos = tracer_.bulbs.front();
        }
    }

    // A SHOT FRUIT COMES OFF THE BRANCH.
    //
    // (user 2026-09-17: "import the apple/oranges pick up mechanic".)
    //
    // THE PICK ALONE IS HALF A MECHANIC. kFruitReachM is an arm and a stretch,
    // and collectPerches puts anchors above a third of a crown's height -- so
    // the fruit on a young oak is reachable and the fruit on a giant one is
    // eight metres up. Without this, most of the crop is scenery.
    //
    // ASKED AT THE IMPACT, NOT ALONG THE FLIGHT. A fruit is walkThrough, so a
    // round passes through it and only reports where it finally hit -- which in
    // a crown is the wood a hand's width behind the apple, because an anchor is
    // a cell with wood directly above it. So a short probe backwards down the
    // shaft from the impact finds the fruit that was in the way.
    //
    // IT DROPS WHERE IT HUNG, with a bearing off the fruit -- the same payout
    // the hand pick makes, because it is the same event with a longer arm.
    bool shootFruit(const Vec3 &at, const Vec3 &dir) {
        if (appleTool_ < 0 && orangeTool_ < 0) return false;
        Vec3 fat{0.0f, 0.0f, 0.0f};
        const Vec3 from{at.x - dir.x * kFruitShotBackM, at.y - dir.y * kFruitShotBackM,
                        at.z - dir.z * kFruitShotBackM};
        const int kind = world_.takeFruitAlong(from, dir, kFruitShotBackM * 2.0f, &fat);
        if (kind < 0) return false;
        const int slot = (kind == 1) ? orangeTool_ : appleTool_;
        if (slot < 0) return false;
        const Tool &t = held_.tool(slot);
        if (t.models.empty()) return false;
        drops_.spill(slot, t.models[0], t.sx, t.sy, t.sz, fat, std::atan2(fat.x, fat.z));
        std::printf("v2: %s shot down at (%.1f, %.1f, %.1f)\n", t.name, fat.x, fat.y, fat.z);
        std::fflush(stdout);
        return true;
    }

    // How far back down the shaft a round looks for the fruit it just went
    // through. A fruit is 0.4 m and the anchor puts wood right behind it, so
    // most of a metre covers the gap at any angle of entry.
    static constexpr float kFruitShotBackM = 0.8f;

    // -----------------------------------------------------------------------
    // [G] -- THE WORLD AGAIN, AND YOU SOMEWHERE NEW IN IT.
    //
    // A NAMED FUNCTION RATHER THAN A KEY HANDLER'S BODY, so --refresh-frame
    // can drive it. A fix for a reported bug that nothing can run is a fix
    // nobody has seen work.
    // -----------------------------------------------------------------------
    void refreshWorld() {
            const auto t0 = std::chrono::steady_clock::now();
            bunnies_.despawnAll();
            bunnies_.publish(world_, kBunnySlot0);
            bunnies_.publishSkunks(world_, kMarchSlot0);
            bees_.despawnAll();
            bees_.publish(world_, kBeeSlot0);
            critters_.despawnAll();
            critters_.publish(world_, kCritterSlot0);
            drops_.clearAll();
            world_.reloadWorld();
            // -- ...AND THE PLAYER IS PUT BACK TOO -----------------------
            //
            // (user 2026-09-17: "when pressing g is seems to just reset the
            //  player. I want the world reset and the player respawned.")
            //
            // IT WAS DOING THE OPPOSITE OF WHAT IT LOOKED LIKE. The world WAS
            // being rebuilt -- edits dropped, life re-scattered -- and the
            // player was left standing exactly where they were, which from
            // inside the game is indistinguishable from nothing having
            // happened to the world and something having happened to you.
            //
            // chooseSpawn IS THE SAME FUNCTION THE GAME OPENS WITH, so a
            // refresh lands you the way a fresh start does: a glade, open to
            // the sun, clear of the shore. It writes opt_.camX/camZ, which is
            // what placeOnGround then reads -- the identical two lines onLoad
            // runs, in the identical order.
            //
            // BEFORE primeBlocking, because priming builds the ring around a
            // position and that position has just changed. Priming the old
            // spot and then teleporting is how you arrive somewhere with no
            // ground under you.
            if (!opt_.camGiven) chooseSpawn();
            world_.primeBlocking(Vec3(opt_.camX, pos_.y, opt_.camZ));
            player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
            pos_ = player_.eyePosition();
            nudgeOutOfSolids();
            if (physics_.available()) rebuildGroundPatch();
            // The denoiser's history describes a world that no longer exists,
            // which is exactly what Tracer::resetHistory is for -- see its
            // note, and note it lists a teleport as the other case.
            tracer_.resetHistory();
            tracer_.resetAccumulation();
            std::printf("v2: world reloaded in %.0f ms -- edits dropped, life re-scattered, "
                        "respawned at (%.0f, %.0f)\n",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count(),
                        double(pos_.x), double(pos_.z));
            std::fflush(stdout);
    }

    void arrowChip(const Vec3 &at, const Vec3 &dir, int chipVox = kArrowChipVox) {
        // THE FRUIT IN FRONT OF THE WOOD COMES FIRST. A round that knocked an
        // apple loose has spent itself on the apple; chipping the branch behind
        // it as well would pay out twice for one shot.
        if (shootFruit(at, dir)) return;
        // -- ...AND SO DOES A LAMP ------------------------------------------
        //
        // (user 2026-09-17: "when the player shoots the lightbulb, it goes out.
        //  this is what should happen.")
        //
        // THE MACHINERY WAS ALREADY HERE AND NOTHING CALLED IT. World::
        // removeLevelBulbNear takes a pendant out of the light list and
        // re-meshes the map without it, which is exactly "it goes out" -- and
        // its only caller was the left button with the LAMP in hand, a placing
        // tool. A round did nothing at all.
        //
        // SAME REASON AS THE FRUIT for being before the carve: the glass is in
        // front of the ceiling it hangs from, and a shot that broke a bulb
        // should not also chip the slab behind it.
        //
        // kBulbPickM IS THE PICK'S OWN RADIUS, reused deliberately. A bulb you
        // can shoot out and a bulb you can pick up should be the same bulb --
        // two radii would be two answers to one question about the same object.
        if (world_.levelOn() &&
            world_.takeLevelBulbNear(&physics_, at, kBulbPickM, simMs_, /*shatter=*/true)) {
            std::printf("v2: bulb shot out -- %zu left\n", world_.levelBulbs().size());
            std::fflush(stdout);
            return;
        }
        // -- THE LEVEL IS CARVED THROUGH ITS OWN DOOR -----------------------
        //
        // (user 2026-09-17, three times: "the bullets are not making impact on
        // the terrain".)
        //
        // AND THIS IS WHERE IT WAS FAILING. Everything below classifies the hit
        // with swingRay and then hands it to carveDebris, carveModel or dig --
        // and the level is none of those three. It is not a chunk, so `dig` has
        // no terrain to cut; it is not a decor instance, so `carveModel` sees
        // `decorSlot < 0` and returns false on its first line. A round landed,
        // the impact was reported, this function ran, and nothing happened --
        // silently, three times.
        //
        // World::carveLevel is the fourth case and the level's own: it cuts
        // both grids, re-meshes only the blocks that changed, and fixes the
        // column tops so a hole in a floor is a hole you can fall through.
        if (world_.levelOn()) {
            // ONE CALL, AND THAT IS THE POINT. The carve and the body used to
            // be two statements here and the second one was deleted by a
            // concurrent edit on 2026-09-17, which put the map straight back to
            // holes that nothing comes out of. See the RULE block over
            // kMinBodyVoxels in gpu/world.h: they are fused now, so there is no
            // spawn line left to lose.
            int nOut = 0;
            Vec3 spoilAt{0.0f, 0.0f, 0.0f};
            lastChipSlot_ = world_.carveLevelToBody(physics_, at, chipVox, simMs_, kArrowAbsorbM,
                                                    &nOut, &spoilAt);
            lastChipAt_ = spoilAt;
            lastChipN_ = nOut;
            return;
        }
        // -- WHAT IT WAS, ASKED THE WAY A SWING ASKS ------------------------
        //
        // Backing off along the shaft and re-probing, rather than inventing a
        // second classifier. swingRay is what decides rock-or-trunk-or-ground
        // for every blow in the game, and a chip that classified its own
        // material would be a second opinion to keep in step with the first --
        // which is the shape of every material bug in this file.
        //
        // 0.4 m BACK because that is comfortably more than the 24 cm substep
        // the impact point was found on, so the ray starts outside the surface
        // whatever angle the shaft came in at; and swingRay's reach is metres,
        // so a target 40 cm ahead is never out of range.
        // -- NOTHING IS CHIPPED OUT FROM UNDER WATER ------------------------
        //
        // (user 2026-09-18: "shooting an arrow at the water caused a black
        // square to appear in the water.")
        //
        // WATER IS NOT SOLID TO A SHAFT. insideWorld tests walkTopVox, which is
        // the walkable top and deliberately ignores water, so an arrow loosed
        // at a lake flies straight through the surface and reports its impact
        // on the BED. Everything below then treats that exactly like a hit on
        // dry ground: swingRay classifies it, `dig` cuts a chip-sized hole, and
        // the edit layer's AIR wins over the water the mesher would otherwise
        // fill the column with. The result is a cubic void inside the lake,
        // which is the black square -- not a shading bug at all, a hole.
        //
        // THE GUARD IS ON THE IMPACT POINT, NOT ON THE CLASSIFIER, because by
        // the time swingRay has answered "ground" the fact that there were five
        // metres of water over it has already been thrown away.
        //
        // IT STILL LANDS AND IT STILL SPARKS -- the caller has already done
        // both. What it does not do is excavate. A splash belongs here
        // eventually; a hole never did.
        if (wetColumnAt(at.x, at.z) && at.y < waterTopAt(at.x, at.z)) return;
        const Vec3 from = at - dir * 0.4f;
        const Swing sw = swingRay(wideWalkWorld(kArrowSolidsM), from, dir);
        if (!sw.hit) return;

        // The swing's own three arms, at the chip's radius. Braced exactly as
        // they are there, and for the reason written over them: an else that
        // takes only the assignment, or a test squeezed between the arms, has
        // broken that chain twice.
        std::vector<uint8_t> vol;
        int n = 0;
        Vec3 spoilAt{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        bool dug = false;
        if (sw.kind == Swing::Loose) {
            dug = world_.carveDebris(physics_, lastDebris_, chipVox, simMs_, &vol, &n,
                                     &spoilAt, &yaw);
        } else if (sw.kind == Swing::Rock || sw.kind == Swing::Trunk) {
            dug = world_.carveModel(sw.solid, sw.eye, sw.dir, sw.reach, chipVox, &vol, &n,
                                    &spoilAt, &yaw);
        } else {
            yaw = 0.0f;   // terrain is not turned
            dug = world_.dig(sw.point, chipVox, &vol, &n, &spoilAt);
        }
        // -- ...AND IF THAT CUT IT THROUGH, THE TREE COMES DOWN -------------
        //
        // (user 2026-09-18: "an arrow should be able to fell trees but its not.
        // im splitting a tree in half with an arrow and the tree is not
        // falling.")
        //
        // THE SWING HAS ALWAYS ASKED AND THE SHAFT NEVER DID. app_capture.inl
        // calls World::fellTree after every carve on a Trunk or a Rock, and
        // dropUndermined after one on Ground. arrowChip carved through exactly
        // the same three doors and then asked nothing -- so a shaft could take
        // the last voxel of a trunk out and leave the tree standing on the
        // air where its stem used to be. The rifle inherits the fix, because
        // its rounds come through this same function.
        //
        // BEFORE THE `n <= 0` RETURN, deliberately. fellTree is about what is
        // still CONNECTED, not about what came off: a blow that severs a trunk
        // while producing no loose spoil is precisely the blow that fells, and
        // returning early on an empty chip would skip the one case that
        // matters. The swing guards on `dug` alone for the same reason.
        if (dug && physics_.available()) {
            if (sw.kind == Swing::Trunk || sw.kind == Swing::Rock)
                felled_ = world_.fellTree(physics_, sw.solid, sw.dir, simMs_);
            else if (sw.kind == Swing::Ground)
                world_.dropUndermined(physics_, sw.point, simMs_);
        }
        if (!dug || n <= 0) return;

        // ...AND THE CHIP IS A BODY, exactly as a swing's is: dropped where it
        // came from with no throw and no spin, stopped by the voxels it was cut
        // out of, then collected off the ground. See the long note over the
        // swing's own spawnDebris for why nothing pushes it anywhere.
        const Vec3 kNoVel{0.0f, 0.0f, 0.0f};
        const Solid *srcRock =
            (sw.kind == Swing::Rock || sw.kind == Swing::Trunk) ? &sw.solid : nullptr;
        const uint8_t takesAs = sw.kind == Swing::Loose  ? lastDebris_.takes
                                : sw.soft                ? uint8_t(kDebrisSoft)
                                : sw.kind == Swing::Trunk ? uint8_t(kDebrisWood)
                                : sw.kind == Swing::Rock  ? uint8_t(kDebrisStone)
                                : isSoilMat(sw.material)  ? uint8_t(kDebrisSoil)
                                                          : uint8_t(kDebrisStone);
        // ...AND IT LIES THERE UNTIL YOU WALK UP TO IT. The last argument is
        // the whole of that: see kArrowAbsorbM, and v1's note on why a piece
        // knocked off from across the clearing must be no easier to collect
        // than one you cut standing over it.
        lastChipSlot_ = world_.spawnDebris(physics_, vol, n, spoilAt, kNoVel, kNoVel, simMs_,
                                           yaw, srcRock, takesAs, kArrowAbsorbM);
        lastChipAt_ = spoilAt;
        lastChipN_ = n;   // --kill-test reads these two; nothing else does
    }

    // -----------------------------------------------------------------------
    // A CARCASS LEAVES ONE STEAK -- v1's dropMeat.
    //
    // (user 2026-09-14: "then lastly, have life drop a raw steak".)
    //
    // AN ORDINARY DROP, which is v1's own choice and its own words: "lands as
    // an ordinary drop, so it hovers, spins and can be picked up like anything
    // else". Nothing about meat needs a second kind of object -- the wheat and
    // the seeds already come off the world this way.
    //
    // AT THE ANIMAL, which for a fish means ON THE WATER. v1 special-cases that
    // and says why: every other kill drops at the ground height of the column,
    // and for a fish that is the SEABED, so the meat would sink out of sight
    // under however many voxels it was swimming in -- "the one drop the player
    // cannot walk to is the one they have to swim down for". Drops::spill takes
    // a world point and the body it came off was AT the surface it died at, so
    // handing it the animal's own position is that rule for free.
    // -----------------------------------------------------------------------
    void dropMeatAt(const Vec3 &at) {
        if (steakTool_ < 0) return;
        const Tool &st = held_.tool(steakTool_);
        if (st.models.empty()) return;
        // -- ...AND A FISH LEAVES ITS MEAT ON THE SURFACE -------------------
        //
        // (user 2026-09-15: "when killing a fish have the raw steak float above
        // the water. just like in v1".)
        //
        // v1 SPECIAL-CASES THIS AND SAYS WHY: every other kill drops at the
        // ground height of the column, and for a fish that is the SEABED -- so
        // the meat sinks out of sight under however many voxels it was swimming
        // in, and "the one drop the player cannot walk to is the one they have
        // to swim down for".
        //
        // BY THE WATER RATHER THAN BY THE SPECIES, which is the same rule said
        // in terms this engine can check anywhere: if there is water over the
        // point the animal died at, the meat belongs on top of it. That covers
        // the fish, and it also covers a duck shot over a lake and a frog on a
        // bank, none of which had to be named.
        // MOVING THE LAUNCH WAS NOT ENOUGH, and that is the whole of why the
        // first cut of this still put the steak on the seabed. A spill FLIES,
        // and Drops ended every arc at the terrain height -- so lifting the
        // start to the waterline just meant the meat was released at the
        // surface and fell through it. The floor is the half that matters; see
        // Drops::Item::floorY.
        Vec3 p = at;
        const float top = waterTopAt(at.x, at.z);
        float floorY = -1e9f;
        if (top > at.y) {
            p.y = top;
            floorY = top;
        }
        drops_.spill(steakTool_, st.models[0], st.sx, st.sy, st.sz, p, 0.0f, floorY);
    }

    // -----------------------------------------------------------------------
    // WHAT IS IN THIS BAND SLOT, WITH THE ONE THING THE TABLE CANNOT KNOW.
    //
    // render/lifehit.h answers from the band's LAYOUT, which is exact for every
    // population but one: the six marchers share a run and which one a slot
    // holds is a property of the marcher, not of the slot. A worm and a skunk
    // are neighbours in it, and one of them leaves a carcass.
    // -----------------------------------------------------------------------
    LifeKind lifeKindAt(int slot) const {
        LifeKind k = lifeAtSlot(slot);
        if (!k.alive() || slot < kMarchSlot0 || slot >= kMarchSlot0 + kMarchSlots) return k;
        switch (bunnies_.marchKindAt(slot - kMarchSlot0)) {
            case kMarchSkunk:      return {"skunk", true, false};
            case kMarchArmadillo:  return {"armadillo", true, false};
            case kMarchPorcupine:  return {"porcupine", true, false};
            case kMarchMouse:      return {"mouse", true, false};
            // v1's WORM band: one hit, and nothing left behind.
            case kMarchWorm:       return {"worm", false, true};
            case kMarchSnake:      return {"snake", true, false};
            default:               return {};
        }
    }

    // -----------------------------------------------------------------------
    // ...AND WHICH POPULATION OWNS IT.
    //
    // The same switch shape as nearestLife, and it exists for the same reason:
    // the one thing a band slot cannot tell you is which container the animal
    // standing in it came out of. Six classes, one line each.
    // -----------------------------------------------------------------------
    bool killLifeAt(int slot) {
        if (slot < 0 || slot >= kFlyerInstances) return false;
        int s = slot;
        if (s < kButterflySlots) return flock_.killSlot(s);
        s -= kButterflySlots;
        if (s < kBirdSlots) return birds_.killSlot(s);
        s -= kBirdSlots;
        if (s < kLakeSlots) return lake_.killSlot(s);
        s -= kLakeSlots;
        if (s < kFlockSlots) return flock2_.killSlot(s);
        s -= kFlockSlots;
        if (s < kBunnySlots) return bunnies_.killSlot(s);
        s -= kBunnySlots;
        if (s < kMarchSlots) return bunnies_.killMarcher(s);
        s -= kMarchSlots;
        if (s < kBeeSlots) return bees_.killSlot(s);
        s -= kBeeSlots;
        if (s < kButtonSlots) return false;   // a button is not alive
        s -= kButtonSlots;
        if (s < kCritterSlots) return critters_.killSlot(s);
        return false;   // the particles
    }

    // -----------------------------------------------------------------------
    // A BLOW ON A LIVING THING -- v1's hitCreature, with v2's consequences.
    //
    // Returns whether the swing landed on an animal at all, which is what
    // spends it. Everything below the split is what v1 does in the same order:
    // the sparks and the flash on EVERY blow, wounding or fatal, and the
    // carcass, the poof and the meat only on the one that kills.
    //
    // THE AXE KILLS OUTRIGHT, which is v1's rule for its own axe: "the axe is
    // the killing tool and ignores this entirely". Everything else -- a pick, a
    // shovel, a hoe, an empty hand -- wears the animal down over three, and the
    // frail species die to anything in one.
    // -----------------------------------------------------------------------
    bool strikeLife() {
        if (!world_.flyersLoaded()) return false;
        const int slot = lifeHits_.aim(world_, pos_, forward());
        if (slot < 0) return false;
        const LifeKind kind = lifeKindAt(slot);
        if (!kind.alive()) return false;
        const bool axe = held_.ready() && held_.carrying() && held_.takes() == Takes::Wood;
        const LifeHits::Blow b = lifeHits_.strike(world_, slot, kind, axe, simMs_);
        lastKilled_ = b.killed;
        if (!b.landed) return false;

        // ---- every blow: the red embers ----------------------------------
        //
        // v1 fires these above its own wound/kill split and says why: "SPARKS
        // ON EVERY BLOW -- the same embers a shaft already threw, now on any
        // hit, wounding or killing. Fired here, before the wound/kill split, so
        // hits one, two and three all show it."
        particles_.hitSparks(b.at, simMs_, /*red=*/true);
        // -- AND THE SWARM ANSWERS -------------------------------------
        //
        // (user 2026-09-16: "when attacking a bee or the beehive, have all of
        // the bees near start attacking the player".)
        //
        // ON THE BLOW, NOT ON THE KILL, which is the user's own word --
        // "attacking". A bee dies to one hit so for the bee the two are the
        // same instant; for the HIVE they are not, and putting the trigger here
        // means either one brings them. It also means a swing that MISSES the
        // kill still provokes, which a kill-only trigger would lose in silence.
        if (kind.name && std::strcmp(kind.name, "bee") == 0) {
            const int woke = bees_.anger(b.at, kBeeAngerM);
            if (woke > 1)
                std::printf("v2: the swarm is up -- %d bees\n", woke);
        }
        if (!b.killed) {
            std::printf("v2: hit the %s -- %d of %d\n", kind.name, b.hits, b.needed);
            std::fflush(stdout);
            return true;
        }

        // ---- ...and the one that kills -----------------------------------
        //
        // IN v1'S ORDER, WHICH MATTERS FOR THE FIRST TWO. The corpse comes
        // apart AT THE HIT, not when the flash ends: "shattering at reap meant
        // the animal stayed whole for the entire half-second of red and only
        // burst once the red was over, so the two never shared a frame." The
        // pieces are in the air while the red is still running.
        const int pieces = world_.shatterFlyer(physics_, slot, pos_, simMs_);
        lastPieces_ = pieces;
        particles_.deathBurst(b.at, simMs_);
        if (kind.meat) dropMeatAt(b.at);
        killLifeAt(slot);
        lifeHits_.clear(slot);
        std::printf("v2: killed the %s -- %d pieces%s\n", kind.name, pieces,
                    kind.meat ? ", left a steak" : "");
        std::fflush(stdout);
        return true;
    }

    // -----------------------------------------------------------------------
    // TURN THE EARTH UNDER THE CROSSHAIR -- v1'S hoeTill, AS A SWING.
    //
    // True when a column actually turned, which is the caller's signal that the
    // blow is spent. False is ordinary: a hoe swung at rock, at a beach, at the
    // sky, or at ground it has already been over does nothing at all and says
    // so, and the swing then falls through to the wheat and to the ordinary
    // bite chain exactly as if this had not been asked.
    //
    // THE GROUND, NOT A MODEL. v1 marches its own ray here rather than reusing
    // the chop; this reuses `lastSwing_`, which has already resolved the trunks
    // and the boulders against the terrain on distance -- so a hoe swung at a
    // rock standing in a field turns nothing, which is right, instead of
    // tilling the dirt behind it.
    // -----------------------------------------------------------------------
    bool tillGround() {
        if (held_.takes() != Takes::Earth) return false;
        if (!lastSwing_.hit || lastSwing_.kind != Swing::Ground) return false;
        const size_t n = world_.till(lastSwing_.point, kTillRadiusM, simMs_ * 0.001);
        if (!n) return false;
        if (opt_.swingLog) {
            std::printf("v2: [f%d] tilled %zu columns at (%.1f, %.1f, %.1f), %zu still turned\n",
                        frameTick_, n, lastSwing_.point.x, lastSwing_.point.y,
                        lastSwing_.point.z, world_.tilledCount());
            std::fflush(stdout);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // CUT THE WHEAT UNDER THE CROSSHAIR, AND DROP WHAT IT WAS MADE OF.
    //
    // True when a plant actually broke -- which is the caller's signal that the
    // swing is spent and the ground behind it must be left alone.
    //
    // THE BLADE RAY, NOT lastSwing_.material. A blade is not solid (see
    // bladeRay), so the ordinary swing goes straight through a field and lands
    // on the dirt -- its material is the SOIL, every time, and asking it about
    // wheat would never be true. The second march is what finds the plant, and
    // it is bounded by however far the first one got, so wheat on the far side
    // of a boulder is not cut through the boulder.
    //
    // WHEAT ONLY, not every blade. isBlade covers the green ramps too and
    // mowing the lawn is not what was asked for; isWheat is the straw, in
    // either wood.
    //
    // ONE OF EACH, AND THEY LAND APART. The two drops leave on opposite
    // bearings from the stalk -- "one a piece", and two items spilling onto the
    // same square read as one.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // TAKE THE APPLE YOU ARE LOOKING AT.
    //
    // (user 2026-09-17: "import the apple/oranges pick up mechanic. so they
    //  need to be a handheld now.")
    //
    // THE BROWSER ENGINE'S MECHANIC, ON v2's BUTTON. There it is the right
    // button, because that is its generic "pick up the small thing you are
    // looking at" -- the same one that lifts a cone, a stick or a field stone.
    // v2 has no such verb: the right button is already the bow's draw, the
    // rifle's sights and now a bite, and all three are exclusive claims on it.
    // The LEFT button is v2's "do the thing in front of you", the wheat is
    // already picked with it, and a plant does not care what you brought -- so
    // an apple is taken the same way.
    //
    // IT PAYS OUT AS A DROP, not straight into the hand. That is the wheat's
    // arrangement and it is the right one: the flight, the sound, the walk-over
    // pickup and the stacking are all addressed by a kit index and already
    // exist, and an apple that appeared in your fist would be the only thing in
    // the game that does.
    //
    // REACH, NOT RANGE. kFruitReachM is what an arm plus a stretch covers. Most
    // of a crown's crop hangs well above that -- collectPerches puts anchors
    // above a third of the model's height -- so the low fruit is what you can
    // take by hand, which is also true of an actual orchard.
    // -----------------------------------------------------------------------
    // 4.5 -> 8.0 (user 2026-09-17: "have the apples and oranges be able to be
    // picked up"). The mechanic worked and almost nothing was in range of it:
    // collectPerches puts anchors above a third of a crown's height, so on a
    // young oak the crop hangs at 3.7 m and on a giant at eight. Measured, the
    // first --food-test aimed at a fruit 6.13 m up and correctly reported
    // NOTHING IN REACH -- a true answer about a fruit nobody could ever have
    // taken by hand.
    //
    // IT IS A PICK, NOT AN ARM. The browser engine's is a look-at pick at
    // interaction range and never claimed to be a reach; calling this one a
    // reach is what made 4.5 m sound defensible. Eight metres covers the crop
    // on everything but the giants, and those are what shootFruit is for.
    static constexpr float kFruitReachM = 8.0f;

    bool pickFruit() {
        if (appleTool_ < 0 && orangeTool_ < 0) return false;
        Vec3 at{0.0f, 0.0f, 0.0f};
        float missed = -1.0f;
        const int kind = world_.takeFruitAlong(pos_, forward(), kFruitReachM, &at, &missed);
        if (kind < 0) {
            // WHY IT MISSED, WHEN THERE WAS SOMETHING TO MISS. "picking fruit
            // is not working" was reported twice, and from in front of the
            // screen a pick that finds nothing and a pick that is never called
            // look the same. This says which: silence means no fruit was in
            // range at all, a number means the aim was off by that much.
            if (missed >= 0.0f) {
                std::printf("v2: no fruit taken -- nearest was %.2f m off the aim (gate %.2f)\n",
                            double(missed), double(kFruitAimM));
                std::fflush(stdout);
            }
            return false;
        }
        // WHICH FRUIT, BY THE ORDER loadFruit REGISTERED THEM -- apple then
        // orange, which is fruit.json's own order and the order the two kit
        // slots are added in. Both halves read the same list, so they cannot
        // drift apart without the file changing under them.
        const int slot = (kind == 1) ? orangeTool_ : appleTool_;
        if (slot < 0) return false;
        const Tool &t = held_.tool(slot);
        if (t.models.empty()) return false;
        // -- STRAIGHT INTO THE HAND, NOT ONTO THE GROUND ------------------
        //
        // (user 2026-09-17: "when the player right clicks on a apple or orange,
        //  the model appears in the right hand".)
        //
        // THE DROP WAS THE WRONG HALF OF THE MECHANIC. Spilling made the pick
        // identical to the wheat: the fruit flew out, landed, and was absorbed
        // a second later by walking over it. That is right for something you
        // CUT off a plant and leave lying, and wrong for something you PICK --
        // it is already in your hand, and putting it on the floor first is a
        // detour the player then has to chase.
        //
        // give() AND THEN select(), because give only takes the hand when the
        // hand is empty -- see its note, which is the rule that stops walking
        // over a pick from swapping out the axe you are mid-swing with. A pick
        // is deliberate and aimed, so it is one of the few things allowed to
        // override that.
        // -- IT FLIES IN; IT DOES NOT TELEPORT ----------------------------
        //
        // (user 2026-09-17: "have the item smoothly go to the players hand
        //  position".)
        //
        // See Drops::grabFrom. The kit slot is handed over when it ARRIVES, by
        // the same loop that pays out a dropped axe you walked over -- so this
        // function no longer gives anything, it only starts the journey.
        //
        // AND IT IS SELECTED ON ARRIVAL, which give() alone will not do: give
        // only takes the hand when the hand is empty, and a pick is deliberate.
        // The slot is remembered rather than acted on, because the hand must
        // not change until the fruit is actually in it.
        if (!drops_.grabFrom(slot, t.models[0], t.sx, t.sy, t.sz, at)) {
            // The drop band is full. Fall back to the old instant hand-over
            // rather than losing the fruit -- the flight is the polish, the
            // fruit is the mechanic.
            held_.give(slot);
            held_.select(slot);
            held_.spendEatPress();
            std::printf("v2: %s picked (no drop slot -- straight to hand)\n", t.name);
            std::fflush(stdout);
            return true;
        }
        selectOnArrive_ = slot;
        std::printf("v2: %s picked at (%.1f, %.1f, %.1f) -- in hand\n", t.name, at.x, at.y, at.z);
        std::fflush(stdout);
        return true;
    }

    bool breakWheat() {
        const Vec3 eye = player_.eyePosition(), dir = forward();
        const float reach = lastSwing_.hit ? lastSwing_.dist : swingReachM(dir);
        const BladeHit bh = bladeRay(walkWorld(), eye, dir, reach);
        if (!bh.hit || !isWheat(bh.material)) return false;

        // -- ONLY THE HOE, AND EVERYTHING ELSE KNOCKS ---------------------
        //
        // AND IT STILL SPENDS THE BLOW. Returning false here would let the
        // chain fall through to the bite, so an axe swung at a stand of wheat
        // would knock AND dig a hole in the dirt behind it -- two answers to
        // one swing, and the second one is a crater the player did not ask for.
        // The plant is what the crosshair was on; it is what the swing meets.
        //
        // ASKED AFTER THE RAY, not before it, so a tool that cannot harvest
        // still has to be AIMED at wheat to be told so. A knock on every swing
        // anywhere would be worse than silence.
        if (held_.takes() != Takes::Earth) {
            toolSfx_.knock();
            return true;
        }
        if (wheatTool_ < 0 && seedsTool_ < 0) return false;

        // -- ONE PATCH, ONE PAYOUT (user 2026-09-14: "have one patch of wheat
        //    drop one seed and one wheat. not multiple per patch.") ----------
        //
        // A TUFT IS THE PATCH, and it is the only grouping this world has --
        // see VoxelTerrain::tuftAt. Blades are a per-column hash with no
        // grouping at all, so the first cut of this mowed a fixed 0.5 m bite,
        // which is SMALLER THAN A TUFT: every swing found more of the same
        // plant standing and paid out again.
        //
        // THE FIX IS NOT A TALLY, IT IS CUTTING THE WHOLE PLANT. Take the tuft
        // at its own radius and there is nothing left to swing at -- the
        // "already cut" test in mow() then does the bookkeeping for free, and
        // it stays correct across a save, a chunk unload, or two players.
        float rad = 0.0f, want = 0.0f, sx = bh.point.x, sz = bh.point.z;
        if (!world_.terrain.tuftAt(bh.point.x, bh.point.z, &rad, &want, &sx, &sz))
            rad = kLooseWheatM;   // a stray tall blade outside any tuft
        if (!world_.mow(Vec3(sx, bh.point.y, sz), rad)) return false;

        // A BEARING OFF THE PLANT, not off the player: two swings at the same
        // tuft from two sides should not throw the drops the same way, and the
        // stalk's own position is the only thing in this that is about the
        // plant rather than about you.
        const float base = std::atan2(bh.point.x, bh.point.z);
        int n = 0;
        for (int k = 0; k < 2; ++k) {
            const int slot = k ? seedsTool_ : wheatTool_;
            if (slot < 0) continue;
            const Tool &t = held_.tool(slot);
            if (t.models.empty()) continue;
            drops_.spill(slot, t.models[0], t.sx, t.sy, t.sz,
                         Vec3(bh.point.x, bh.point.y + 0.15f, bh.point.z),
                         base + (k ? 2.2f : -2.2f));
            ++n;
        }
        if (n) {
            std::printf("v2: wheat broken at (%.1f, %.1f, %.1f) -- %d dropped\n", bh.point.x,
                        bh.point.y, bh.point.z, n);
            std::fflush(stdout);
        }
        return true;
    }

    // Is a body standing here inside a trunk? The arrival check -- gathered at
    // the SPOT rather than at the player, for the reason teleportTo carries.
    bool trunkAt(float x, float z) {
        std::vector<Solid> around;
        world_.collidersNear(Vec3(x, player_.pos.y, z), 8.0f, &around);
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = around.data();
        w.solidCount = int(around.size());
        return player_.blocked(w, x, z);
    }

