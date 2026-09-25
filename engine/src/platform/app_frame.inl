// app_frame.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: onFrameRender: the per-frame loop
// -----------------------------------------------------------------------------
    void onFrameRender(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) override {
        const auto hitchT0 = std::chrono::steady_clock::now();
        segBegin();   // see CpuSeg -- the frame split at its block boundaries
        world_.frameMark();   // SampleApp ends this device frame -- see World::endDeviceFrameIfDue
        hPhys_ = hLife_ = hPub_ = hDebris_ = 0.0;
        // APPLIED A SECOND TIME, ON THE FIRST FRAME ONLY. onLoad runs before
        // the swapchain is sized and before the window is shown, and anything
        // that happens in between -- Falcor's own sizing, a DPI change, the
        // shell placing the window -- lands after the first attempt and undoes
        // it. Re-applying once here is after all of that and still long before
        // a person could have moved the window themselves.
        if (!placedTwice_ && !opt_.background) {
            placedTwice_ = true;
            restoreWindowPlacement();
            // ...AND ONLY THEN FULLSCREEN, if this launch wants it (the
            // installed game -- see Options::fullscreen). After the last
            // placement, so nothing moves the window back out of it.
            if (opt_.fullscreen) applyFullscreen(true);
        }
        if (opt_.outGiven) return;

        // BEFORE ANYTHING IS DRAWN INTO IT. The target still holds the previous
        // frame at this point -- blitted, crosshaired and with the GUI composed
        // over it -- and that whole frame is what an interface has to be judged
        // on. Falcor does not clear it between frames, which is what makes this
        // possible at all.
        if (!opt_.shotUi.empty() && shotFrames_ >= opt_.shotFrame && target->getWidth() > 0) {
            // SYNCHRONOUSLY, and that last argument is the whole of it:
            // Falcor's captureToFile defaults to async = true, which hands the
            // PNG to the Threading pool and returns -- and the next line here
            // shuts the process down. The write was always a race and it is now
            // a certainty, because onShutdown ends in _Exit (see app_window.inl,
            // and the minute-long quit it was added for). The flag reported
            // "wrote ..." and left no file.
            target->getColorTexture(0)->captureToFile(0, 0, opt_.shotUi,
                                                      Falcor::Bitmap::FileFormat::PngFile,
                                                      Falcor::Bitmap::ExportFlags::None,
                                                      /*async=*/false);
            std::printf("v2: wrote %s at %ux%u (window, with the interface)\n",
                        opt_.shotUi.c_str(), target->getWidth(), target->getHeight());
            std::fflush(stdout);
            askShutdown(0);
            return;
        }

        // -- Reflex, and the frame it is pacing --------------------------
        //
        // THE MARKERS ARE NOT TELEMETRY. Reflex decides when to release the CPU
        // to start the next frame, and it decides it from these: how long
        // simulation took, when present happened, how far ahead of the GPU the
        // CPU is running. Without them Reflex is inert, and DLSS Frame
        // Generation -- which refuses to run without Reflex -- has nothing to
        // pace the generated frame against.
        //
        // newFrame() first, because everything below is attributed to the frame
        // index it hands out.
        // NO markPresentEnd ANYWHERE, deliberately, and matching v6.
        //
        // The present window is opened with markPresentStart and closed by
        // STREAMLINE, inside the interposer that owns the swapchain, when the
        // real Present happens. Closing it by hand from here reports a present
        // that ended before the generated frame was inserted -- which is the
        // one thing Reflex is pacing against.
        // DLSS-G'S OWN COUNTERS, read here and nowhere else: before this frame
        // starts and just after the previous present finished, which is the
        // only point they describe a completed frame.
        sl_.pollFrameGenState();
        // WHAT WAS ACTUALLY PRESENTED, counted here because nothing else can
        // see it. framesPresented() is DLSS-G's own count for the frame just
        // finished -- 1 means it presented only the rendered frame, 2 means it
        // inserted one of its own. Everything above that first one is a frame
        // the engine never drew.
        {
            const int presented = sl_.framesPresented();
            if (presented > 1) {
                generatedTotal_ += uint64_t(presented - 1);
                generatedThisSecond_ += presented - 1;
            }
            presentedTotal_ += uint64_t(presented > 0 ? presented : 1);
        }
        sl_.newFrame();
        sl_.markSimulationStart();

        const auto now = std::chrono::steady_clock::now();
        // Clamped so one slow frame cannot teleport the camera, but not so
        // tightly that movement stalls when the tracer is having a bad time.
        const float wallDt =
            minf(0.25f, float(std::chrono::duration<double>(now - lastTime_).count()));
        lastTime_ = now;
        // A scripted capture SIMULATES on a fixed step -- see the note on
        // shotDt -- but it is still measured on the wall clock. Letting the
        // frame rate be derived from the simulated step would make --stats
        // report 1/shotDt forever, which is the one number in that line nobody
        // could use.
        const float dt = opt_.shotPath.empty() ? wallDt : opt_.shotDt;

        // THE SCREEN SWITCHING OFF IS NOT PART OF THE SIMULATION, so it is
        // stepped here on the WALL clock rather than in processInput on the
        // scaled one -- a slowed or scripted game should still take exactly as
        // long to go dark.
        // AND NOTHING AFTER IT IF THE WINDOW IS GOING. See tickQuit: shutdown
        // tears the window down synchronously, and every line below this one
        // still talks to the device -- World::update drains compactions and
        // reads a query back, which is what the crash stack names.
        if (tickQuit(wallDt)) return;

        // -- THE WEATHER'S OWN CLOCK ------------------------------------
        //
        // ON THE WALL, NOT ON THE SIM. A flake's fall is a LOOK, and scrubbing
        // time with the arrow keys or pausing the day should not stop it --
        // the same reasoning the recorder's clock uses. See V6Params::snowTime.
        //
        // WRAPPED ON A WHOLE NUMBER OF CELLS. The fall is frac() of the clock
        // scaled by the cell, so any wrap that is not a multiple of the cell
        // time makes every flake in the sky jump at once. 4096 cells is about
        // half an hour at the fall speed and is exact in a float.
        snowClock_ = std::fmod(snowClock_ + double(wallDt), 4096.0 * 0.5);
        tracer_.snowTime = float(snowClock_);
        tracer_.snowStep = maxf(1.0f / 240.0f, wallDt);
        // ONE SWITCH FOR BOTH HALVES. The settings row sets VoxelTerrain::
        // snowOn, which is the blanket on the ground; this is the same answer
        // read for the sky, so it cannot be snowing with nothing settling or
        // lying with nothing falling.
        tracer_.snowFall = world_.terrain.snowOn ? 1.0f : 0.0f;

        // -- ...AND IT SETTLES ----------------------------------------
        //
        // (user 2026-09-20: "the snow should accumulate on the ground like in
        //  v1.")
        //
        // VoxelTerrain::snowLay is how much of the blanket has arrived, 0 to
        // 1. It walks up while it snows and back down when it stops, and
        // because it is TERRAIN -- it raises the column -- the world can only
        // be re-meshed when it actually changes by something the ground can
        // express. kSnowSteps of them over kSnowBuildSec.
        //
        // THE REBUILD IS THE COST AND IT IS WHY THIS IS STEPPED. Each step
        // restarts the mesher (the workers hold their own copy of the
        // generator) and streams the ring back in. Five of those over two
        // minutes is a blanket you watch arrive; sixty a second is not a
        // feature, it is a slideshow.
        {
            const float want = world_.terrain.snowOn ? 1.0f : 0.0f;
            const float rate = wallDt / kSnowBuildSec;
            snowLay_ = clampf(snowLay_ + (want > snowLay_ ? rate : -rate), 0.0f, 1.0f);
            // Quantised BEFORE the compare, so the test is "has the ground
            // changed" rather than "has the number changed".
            const int step = int(snowLay_ * float(kSnowSteps) + 0.001f);
            if (step != snowStep_) {
                snowStep_ = step;
                world_.terrain.snowLay = float(step) / float(kSnowSteps);
                // ONE CALL, AND IT DOES NOT ERASE ANYTHING -- see
                // World::retargetTerrain, and the crash that taught it.
                world_.retargetTerrain();
            }
        }

        clock_.advance(dt);
        // THE DECK DRIFTS ON THE DAY CYCLE CLOCK, NOT ON THE WALL CLOCK.
        //
        // dt * cycleSpeed is the amount of SIMULATED time this frame covered --
        // exactly the quantity DayNight::advance divides by DAY_SECONDS. So the
        // wind is whatever the sky is doing: at 1x it is identical to the wall
        // clock, at 32x the deck crosses the sky as fast as the sun does, and
        // pausing the cycle stops the weather with it instead of leaving clouds
        // sliding under a sun that has stopped.
        //
        // SIGNED, because cycleSpeed is. Running time backwards runs the wind
        // backwards too, which is the only reading of "the clouds follow the
        // day" that stays true at a negative speed.
        //
        // None of it touches the cache: wind is a lookup offset in the march,
        // so even 512x costs nothing and refills nothing.
        clouds_.advance(clock_.paused ? 0.0f : dt * clock_.cycleSpeed);
        applySun(false);

        // Stream the world around the camera. A changed ring means new geometry
        // in frame, so what has been accumulated for the old one no longer
        // describes it.
        //
        // TIMED SEPARATELY FROM THE FRAME, because this is the one part of a
        // frame whose cost has nothing to do with how hard the picture is: it
        // is paid in whichever frame a chunk happens to arrive in. Keeping the
        // two apart is what lets the report below say whether a slow frame was
        // the renderer or the streamer.
        const auto tu0 = std::chrono::steady_clock::now();
        // -- IN THE ROOM, THE STREAMER GOES ON WATCHING THE WOOD -----------
        //
        // The pause room is at -4096, four kilometres from anywhere anyone
        // plays. Following the CAMERA there would evict every chunk of the
        // wood and stream a ring of forest nobody can see -- and then do the
        // whole thing again backwards on the way out, so the green button would
        // hand you a grey hole to stand in while it refilled.
        //
        // So while the room is open the streamer is pointed at the place the
        // player LEFT. Everything else update() does -- draining compactions,
        // ageing the buffer pool, retiring meshes -- still runs, because it is
        // the same call.
        // THE WOOD WAS "WHERE YOU ARE, ALWAYS" FOR EXACTLY AS LONG AS THERE WAS
        // NOWHERE ELSE TO BE. This read pos_ unconditionally once the pause
        // room came down into the wood and left no second place to stand.
        //
        // THE BUILDING LEVEL IS A SECOND PLACE AGAIN -- at -4096, 640 m up --
        // and it revives the fault above word for word. rering() is not gated
        // on being somewhere else, so following the camera there evicts the
        // whole resident disc and streams a ring of forest at the bottom of the
        // sky that no ray can reach, and then does the entire thing backwards
        // when [O] brings you home.
        //
        // So the level points the streamer at woodPos_ -- where the player was
        // standing when they pressed the key. Same fix as the room's, same
        // reason, and it is what makes coming back one TLAS rebuild rather than
        // a re-stream of everything you were looking at a moment ago.
        const Vec3 streamAt = world_.levelOn() ? woodPos_ : pos_;
        segEnd(kSegPre);
        const auto streamT0 = std::chrono::steady_clock::now();
        {
            // Its GPU work too -- structure builds, compaction copies, the top
            // level -- which no scope measured, so a GPU spike on a frame a
            // chunk arrived in had nowhere to show up.
            FALCOR_PROFILE(ctx, "stream");
            if (world_.update(streamAt)) tracer_.resetAccumulation();
        }
        const double streamMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - streamT0)
                .count();
        segEnd(kSegStream);

        // ---- AND THE SPAWN IS CHECKED ONCE THERE IS A WORLD TO CHECK IT
        // AGAINST ------------------------------------------------------------
        //
        // nudgeOutOfSolids runs in onLoad, and onLoad happens BEFORE a single
        // chunk has been streamed -- world_.update, one line above this, is
        // what streams them. So the check that is supposed to keep the player
        // out of a boulder ran against a world with no boulders in it, found
        // nothing, and passed. That is the whole of "you spawned me inside a
        // rock": the test was right and it was asked too early.
        //
        // Asked again here, the first frame the chunk under the player exists.
        // placeOnGround runs unconditionally afterwards because the height is
        // wrong for the same reason the position was: groundInfo could not see
        // a rock that had not been streamed, so it put the player at terrain
        // level -- which, under a boulder, is inside it.
        // -- ...BUT NOT WHILE THE PLAYER IS SOMEWHERE THAT IS NOT THE WOOD --
        //
        // --room and --stage put the player in a place of their own, and this
        // ran one frame later and teleported them straight back out of it. It
        // is the whole of why `--room --shot-ui` photographed the forest from
        // two kilometres up: the room was built, it WAS in the structure, and
        // the camera had been moved back to the spawn point without anything
        // saying so. `--stage` had been doing the same thing for as long as it
        // has existed.
        //
        // POSTPONED, NOT SETTLED. spawnSettled_ stays false, so the check runs
        // on the first frame after they come back -- which is the first frame
        // the position it is checking is one they are actually standing at.
        if (!spawnSettled_ && !world_.staged() && !world_.levelOn() &&
            world_.chunkAt(player_.pos)) {
            spawnSettled_ = true;
            nudgeOutOfSolids();
            player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
            pos_ = player_.eyePosition();
            // ...AND --room OPENS HERE, on the first frame there is a settled
            // eye to hang it in front of. See the note over prewarmRoom.
            if (opt_.roomAtStart) setRoomOpen(true);
        }
        const float updateMs =
            float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tu0)
                      .count());
        if (opt_.profile) {
            frameMs_.push_back(wallDt * 1000.0f);
            streamMs_.push_back(updateMs);
        }
        // -- THE ARROW, IF THE PANEL ASKED FOR IT -------------------------
        //
        // HERE AND NOT IN THE PANEL, and beside the streamer for the same
        // reason the streamer is here: recomposing the bow's strip builds
        // fourteen bottom-level structures, and a structure build forces a
        // device drain and a blocking submit. This is the point in the frame
        // where that is legal -- world_.update above does exactly the same
        // thing whenever a chunk arrives. See the note on the rows in the
        // settings panel for what happened when it was done from there.
        if (arrowDirty_) {
            arrowDirty_ = false;
            const auto ta = std::chrono::steady_clock::now();
            if (held_.retuneArrow(world_, arrowWant_)) {
                std::printf("v2: arrow %+d %+d %+d voxels  (%.0f ms)\n", arrowWant_.across,
                            arrowWant_.along, arrowWant_.up,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - ta)
                                .count());
                std::fflush(stdout);
                // Different geometry, so every sample already in the film
                // describes a bow that is no longer there.
                tracer_.resetAccumulation();
            } else {
                // It did not take -- put the rows back on the value that is
                // actually in the hand, or they would go on showing a bow that
                // was never built.
                arrowWant_ = held_.arrow();
            }
        }

        if (processInput(dt)) tracer_.resetAccumulation();
        segEnd(kSegInput);

        // A SCRIPTED FLASH, on its named frame. It sets the flash DIRECTLY
        // rather than swinging: the kill path is covered by --kill-test, and
        // what is unproven here is the shader -- so this drives the one field
        // that shader reads and nothing else, which is what makes a difference
        // in the picture mean what it says.
        // -- KILL THE PLAYER ON A FRAME, so the death screen can be shot ---
        //
        // (user 2026-09-21: "the game over screen doesnt seem to be using the
        //  right pixel font. double check this and fix.")
        //
        // There was no way to SEE that screen without dying in a live session,
        // which is the reason its font went unnoticed. --die-frame N empties
        // the bar on frame N and --shot-ui a second later catches the curtain
        // at full fade. kGameOverHoldMs is the window it is up for.
        if (opt_.dieFrame >= 0 && shotFrames_ == opt_.dieFrame)
            vitals_.hurt(1000, "the test asked");
        if (opt_.hurtFrame >= 0 && shotFrames_ == opt_.hurtFrame) {
            int best = -1;
            float bestD = 1e30f;
            Vec3 bestAt{0, 0, 0};
            for (int q = 0; q < kFlyerInstances; ++q) {
                if (!lifeAtSlot(q).alive()) continue;
                Vec3 a{0, 0, 0};
                float r = 0.0f;
                if (!world_.flyerAt(q, &a, &r)) continue;
                const float dx = a.x - pos_.x, dy = a.y - pos_.y, dz = a.z - pos_.z;
                const float d = dx * dx + dy * dy + dz * dz;
                if (d >= bestD) continue;
                bestD = d;
                best = q;
                bestAt = a;
            }
            if (best >= 0) {
                // STAND OFF AND LOOK AT IT, so the animal is actually in the
                // frame -- a flash on something behind the camera proves as
                // little as no flash at all.
                const float dx = pos_.x - bestAt.x, dz = pos_.z - bestAt.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(bestAt.x + dx / d * 2.2f, bestAt.z + dz / d * 2.2f);
                publishLife();
                pos_ = player_.eyePosition();
                const Vec3 look = normalize(Vec3(bestAt.x - pos_.x, bestAt.y - pos_.y,
                                                 bestAt.z - pos_.z));
                yaw_ = atan2f(look.x, -look.z) * 180.0f / PI;
                pitch_ = asinf(look.y) * 180.0f / PI;
                world_.setFlyerHurt(best, opt_.hurtDim ? 0.0f : 1.0f);
                world_.flushFlyerInstances();
                std::printf("v2: hurt test -- %s in slot %d at (%.1f, %.1f, %.1f), %.1f m "
                            "off, flash %s\n",
                            lifeKindAt(best).name, best, bestAt.x, bestAt.y, bestAt.z,
                            std::sqrt(bestD), opt_.hurtDim ? "OFF" : "on");
            } else {
                std::printf("v2: hurt test -- no animal drawn anywhere\n");
            }
            std::fflush(stdout);
        }

        // A SCRIPTED BURST, on its named frame: four sparks and the smoke, two
        // metres in front of the camera and a little below the eye line, which
        // is where a blow lands. The one way to see whether an emissive
        // material is actually emitting.
        // THE DISCOVERY BANNER, on the same clock as the sparks above it.
        if (opt_.achFrame >= 0 && shotFrames_ == opt_.achFrame) unlockProjectile();
        if (opt_.sparkFrame >= 0 && shotFrames_ == opt_.sparkFrame) {
            const Vec3 sdir = forward();
            const Vec3 sat(pos_.x + sdir.x * 2.0f, pos_.y + sdir.y * 2.0f - 0.35f,
                           pos_.z + sdir.z * 2.0f);
            if (opt_.tearOnly) {
                // FOUR AT ONCE, spread across the slot band, so one frame shows
                // what a weeping duckling looks like: this pool has twice
                // shipped a particle that was the wrong SIZE and once the wrong
                // COLOUR, and all three were reported by eye rather than caught
                // by a number. See kTearEmit.
                for (int k = 0; k < kParticleTears; ++k)
                    particles_.tear(Vec3(sat.x + float(k) * 0.28f - 0.42f, sat.y, sat.z),
                                    simMs_);
            } else if (opt_.sparkOnly) {
                particles_.toolSparks(sat, simMs_);
            } else {
                particles_.deathBurst(sat, simMs_);
            }
            // ...AND HOW MANY OF THEM ARE LIGHTS, which is the only way to
            // tell "the sparks are not lighting anything" from "the sparks are
            // not being PUBLISHED as lights" -- two faults that look identical
            // in a dark frame. See publishSparkLights.
            std::printf("v2: spark test -- burst at (%.2f, %.2f, %.2f), %d live, %zu point "
                        "lights published, radiance %.2f %.2f %.2f, radius %.2f\n",
                        sat.x, sat.y, sat.z, particles_.live(), tracer_.bulbs.size(),
                        tracer_.bulbRadiance.x, tracer_.bulbRadiance.y, tracer_.bulbRadiance.z,
                        tracer_.bulbRadius);
            std::fflush(stdout);
        }

        // -- A SCRIPTED ROUND, AND WHAT IT TOOK OUT OF THE WALL --------------
        //
        // (user 2026-09-17: "I want you to make the bullet chunking mechanics
        // match the chunking mechanics of the arrow. same ones. the chunk
        // should fall out of the hole, or become a rigid body".)
        //
        // THROUGH fireRifle(), not beside it. The three things that were wrong
        // here -- the muzzle, the carve and the body -- were each wired in a
        // different function, so a test that launched its own round would have
        // passed while the trigger stayed broken.
        //
        // REPORTED SIX FRAMES LATER because the round has to fly. lastChipN_ is
        // cleared on the shot so the count is THIS round's, and lastChipSlot_
        // is the debris body: voxels removed with no slot is exactly the
        // "they just disappear" that was reported, and it is the one failure
        // a picture of the hole cannot tell from a success.
        // WHAT IS ALREADY LOOSE BEFORE A SHOT IS FIRED. The penetration audit
        // found a 2 x 2 x 25 body a third buried in the map and the sweep was
        // the obvious suspect -- but the sweep reports freeing four pieces of
        // seven voxels between them, so it is not the source. This says whether
        // the thing was there before anybody pulled a trigger.
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame - 1 && world_.levelOn()) {
            int b0 = 0, in0 = 0, w0 = 0;
            world_.levelPenetration(&b0, &in0, &w0);
            std::printf("v2: fire test -- BEFORE the shot: %d bodies, %d voxels inside, worst %d\n",
                        b0, in0, w0);
            std::fflush(stdout);
        }
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame) {
            lastChipN_ = 0;
            lastChipSlot_ = -1;
            fireGun();
            std::printf("v2: fire test -- round away from (%.2f, %.2f, %.2f)\n",
                        pos_.x, pos_.y, pos_.z);
            std::fflush(stdout);
        }

        // -- A SCRIPTED RELOAD, AND THE ONLY WAY TO PHOTOGRAPH ONE -----------
        //
        // (user 2026-09-18: "there are animations for the reload cycle".)
        //
        // NINE DRAWN FRAMES THAT ONLY PLAY ON AN EMPTY GUN. Every other way to
        // see them needs a hand on a mouse for twenty rounds and then a
        // screenshot timed to a 1800 ms window -- which is not a thing anybody
        // does twice, and is certainly not a thing a regression is caught by.
        //
        // IT EMPTIES THE MAGAZINE RATHER THAN CALLING THE ANIMATION. A full gun
        // refuses to reload, deliberately (see reloadRifle), so a test that
        // reached past that would be testing a path the game does not have.
        // This is the real empty-gun road: rounds at zero, and the same call
        // the trigger makes.
        //
        //     v2.exe --background --level --reload-frame 40 --shot-frame 70 \
        //            --shot out.png          # the middle of the cycle
        //
        // THE REPORT IS 150 FRAMES LATER, which is 2.5 s at this engine's dt
        // and comfortably past the 1.8 s cycle -- so --shot-frame has to be
        // past that to see it. It prints what the badge would be showing, which
        // is the whole of what a reload is for.
        if (opt_.reloadFrame >= 0 && shotFrames_ == opt_.reloadFrame) {
            // WHICHEVER GUN IS UP -- the rifle unless --scroll put the pistol
            // there, which is how one flag photographs both cycles.
            const int g = heldGun();
            setGunAmmo(g, 0);
            const bool started = reloadGun();
            std::printf("v2: reload test -- %s magazine emptied, cycle %s (%d reload frames)\n",
                        g >= 0 ? held_.tool(g).name : "no gun",
                        started ? "STARTED" : "REFUSED -- WRONG",
                        g >= 0 ? held_.tool(g).reloadFrames : -1);
            std::fflush(stdout);
        }
        // 220 FRAMES, WHICH IS 3.7 s. It was 150 when a reload was one turn of
        // 1800 ms, and 150 frames would report a revolver as still reloading --
        // which is the test lying about the feature it exists to check. The
        // pistol is 44 drawn frames of 45 ms now (four out, six per round x
        // six, four back in), so 1.98 s, and the margin is on purpose: this
        // number wants to outlast the art, not to track it.
        if (opt_.reloadFrame >= 0 && shotFrames_ == opt_.reloadFrame + 220) {
            const int g2 = heldGun();
            const int have = g2 >= 0 ? gunAmmoOf(g2) : -1, want = g2 >= 0 ? gunMagOf(g2) : 0;
            std::printf("v2: reload test -- %d/%d rounds, %s  %s\n", have, want,
                        held_.reloading() ? "STILL RELOADING" : "done",
                        (have == want && !held_.reloading())
                            ? "-- the magazine came back"
                            : "-- NOTHING CAME BACK, WRONG");
            std::fflush(stdout);
        }
        // -- ...AND THE SAME SHOT FROM ARM'S LENGTH ------------------------
        //
        // (user 2026-09-17: "the chunking mechanic from the bullet doesnt work
        //  point blank. fix this.")
        //
        // THE FIRST SHOT LANDS 24 m AWAY AND PROVES NOTHING ABOUT THIS. The
        // arming distance was a metre and a half, so every round fired closer
        // than that passed through whatever it was aimed at and armed on the
        // far side -- and the existing test could not see it, because it was
        // never fired at anything near. So this walks up to the hole the first
        // shot made and fires again from 0.6 m.
        //
        // THE DISTANCE IS THE REPORT. A chip is not enough on its own: a round
        // that flew past the wall and hit the next one along also chips. Where
        // the hole is relative to the muzzle is what says the round stopped
        // where it should have.
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame + 80 && lastChipN_ > 0) {
            // TWO THINGS THE FIRST ATTEMPT AT THIS GOT WRONG, both of which
            // made a working fix report as broken:
            //
            //   * A ROUND IS BORN AT THE MUZZLE, not at the eye, and the
            //     muzzle is the better part of a metre in front of it. Standing
            //     0.6 m off the wall put the muzzle THROUGH it, so the round
            //     started on the far side and hit the next wall along.
            //   * THE FIRST SHOT'S HOLE IS STILL THERE. Firing at the same spot
            //     sends the round through it. So this aims a little to one side
            //     of it, at wall that has not been cut yet.
            const Vec3 aim = forward();
            const Vec3 side = camRight();
            const Vec3 tgt{lastChipAt_.x + side.x * 0.6f, lastChipAt_.y + side.y * 0.6f,
                           lastChipAt_.z + side.z * 0.6f};
            pbFrom_ = Vec3{tgt.x - aim.x * kPointBlankM, tgt.y - aim.y * kPointBlankM,
                           tgt.z - aim.z * kPointBlankM};
            player_.pos = Vec3{pbFrom_.x, pbFrom_.y - player_.eye, pbFrom_.z};
            pos_ = player_.eyePosition();
            lastChipN_ = 0;
            lastChipSlot_ = -1;
            fireGun();
            std::printf("v2: fire test -- point blank, %.2f m from the wall\n",
                        double(kPointBlankM));
            std::fflush(stdout);
        }
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame + 95) {
            const float dx = lastChipAt_.x - pbFrom_.x, dy = lastChipAt_.y - pbFrom_.y,
                        dz = lastChipAt_.z - pbFrom_.z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            std::printf("v2: fire test -- point blank chipped %d voxels at %.2f m  %s\n",
                        lastChipN_, double(d),
                        lastChipN_ <= 0 ? "NOTHING CAME OUT -- WRONG"
                        : d > kPointBlankM + 0.6f
                            ? "THE ROUND FLEW PAST AND HIT SOMETHING ELSE -- WRONG"
                                        : "cut the wall in front of it");
            std::fflush(stdout);
        }

        // -- ...AND WHERE THAT BODY ACTUALLY WENT --------------------------
        //
        // (user 2026-09-17: "the chunks teleport to the top of the building
        //  where they proceed to endlessly fall forever.")
        //
        // THE COUNT AND THE SLOT CANNOT SEE THIS. "chip fell out as a body" was
        // already true while the body was being flung onto the roof once a
        // frame, because both of those are answered at the moment of the cut
        // and this fault is entirely about what happens afterwards. So the test
        // watches the piece for a second and reports the one number that tells
        // the three outcomes apart: how far it is from the hole it came out of.
        //
        //   near and slow   it fell out and landed        -- right
        //   far ABOVE       it was clamped onto the roof  -- the teleport
        //   far BELOW, fast it left the map               -- the endless fall
        // -- ...AND WHAT THE BODIES ARE DOING TO THE MAP -------------------
        //
        // (user 2026-09-17: "audit the physics system on nuketown. things are
        //  glitching when they become rigid bodies.")
        //
        // A BODY BORN PENETRATING IS THE GLITCH. The solver answers one with an
        // impulse proportional to the depth, so a piece a voxel inside a wall
        // twitches and one a metre inside is fired across the room -- and from
        // a frame you cannot tell either from a piece that is simply falling.
        // See World::debrisInLevel, and note debrisClip beside it CANNOT answer
        // this: it walks models, and the level has none.
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame + 160 && world_.levelOn()) {
            int bodies = 0, inside = 0, worst = 0, wslot = -1, wvox = 0;
            Vec3 wat{0, 0, 0};
            bool wfell = false;
            int wd[3] = {0, 0, 0};
            world_.levelPenetration(&bodies, &inside, &worst, &wslot, &wat, &wvox, &wfell, wd);
            std::printf("v2: fire test -- %d bodies, %d voxels inside the map, worst %d  %s\n",
                        bodies, inside, worst,
                        worst > 8 ? "A BODY IS BURIED IN THE MAP -- IT WILL BE THROWN"
                        : inside  ? "touching, which is what resting looks like"
                                  : "clear of it");
            if (worst > 0)
                std::printf("v2: fire test -- worst is slot %d at (%.2f, %.2f, %.2f), %d voxels%s\n",
                            wslot, wat.x, wat.y, wat.z, wvox, wfell ? ", scenery" : "");
            if (worst > 0) {
                Vec3 lin{0, 0, 0}, ang{0, 0, 0};
                world_.debrisVel(physics_, wslot, &lin, &ang);
                const float sp = sqrtf(lin.x * lin.x + lin.y * lin.y + lin.z * lin.z);
                // A BODY STILL MOVING IS NOT BURIED, IT IS FALLING. Without
                // this the audit cannot tell a piece wedged in a wall from one
                // that happens to be passing through the sample.
                std::printf("v2: fire test -- worst body is %dx%dx%d, %s (%.2f m/s)\n", wd[0],
                            wd[1], wd[2], (sp > 0.15f) ? "STILL MOVING" : "at rest",
                            double(sp));
            }
            std::fflush(stdout);
        }
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame + 150 && lastChipSlot_ >= 0) {
            Vec3 bp{0, 0, 0};
            float bq[4] = {0, 0, 0, 1};
            if (world_.debrisPose(lastChipSlot_, &bp, bq)) {
                const float dy = bp.y - lastChipAt_.y;
                const float drop = bp.y - chipWatchY_;
                std::printf("v2: fire test -- body at (%.2f, %.2f, %.2f), %+.2f m from the hole, "
                            "%+.2f m in the last 30 frames  %s\n",
                            bp.x, bp.y, bp.z, dy, drop,
                            dy > 1.0f      ? "CLAMPED UPWARD -- THE TELEPORT, WRONG"
                            : drop < -0.5f ? "STILL FALLING -- THE ENDLESS FALL, WRONG"
                                           : "came to rest below the hole");
                // ...AND THE TWO ANSWERS SIDE BY SIDE, which is the whole of
                // why this bug existed: the backstop used to ask for the top of
                // the column and now asks for the floor under the body. Printed
                // so the fix is demonstrated rather than assumed -- if these
                // two are equal at the hole, this test is not exercising the
                // case that was broken and the camera should be moved indoors.
                std::printf("v2: fire test -- floor under it %.2f, top of its column %.2f  "
                            "(the old backstop used the second)\n",
                            world_.levelFloorBelowM(bp.x, bp.y, bp.z),
                            world_.levelGroundM(bp.x, bp.z));
                std::fflush(stdout);
            }
        }
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame + 120 && lastChipSlot_ >= 0) {
            Vec3 bp{0, 0, 0};
            float bq[4] = {0, 0, 0, 1};
            if (world_.debrisPose(lastChipSlot_, &bp, bq)) chipWatchY_ = bp.y;
        }
        if (opt_.fireFrame >= 0 && shotFrames_ == opt_.fireFrame + 20) {
            std::printf("v2: fire test -- chipped %d voxels at (%.2f, %.2f, %.2f), body slot %d  %s\n",
                        lastChipN_, lastChipAt_.x, lastChipAt_.y, lastChipAt_.z, lastChipSlot_,
                        lastChipN_ <= 0   ? "NOTHING CAME OUT -- WRONG"
                        : lastChipSlot_ < 0 ? "CUT BUT NO BODY -- THE VOXELS JUST VANISHED, WRONG"
                                            : "chip fell out as a body");
            // ...AND WHAT THE CUT LEFT STANDING ON AIR. Zero is the ordinary
            // answer -- a hole in a wall severs nothing -- so this line is only
            // interesting when a shot cuts something free, and it is the only
            // way to see that a severed piece got a body rather than being left
            // hanging. See World::dropLevelHangers.
            if (world_.lastHangVox() > 0)
            {
                int hw = 0, hh = 0, hd = 0;
                world_.lastHangDims(&hw, &hh, &hd);
                std::printf("v2: fire test -- %d pieces cut loose by the hole, %d voxels, "
                            "biggest %d (%d x %d x %d), body slot %d  %s\n",
                            world_.lastHangPieces(), world_.lastHangVox(), world_.lastHangWorst(),
                            hw, hh, hd,
                            world_.lastHangSlot(),
                            world_.lastHangSlot() < 0
                                ? "SEVERED BUT NO BODY -- IT WOULD HAVE FLOATED, WRONG"
                                : (maxi(hw, maxi(hh, hd)) >= 4 * maxi(1, mini(hw, mini(hh, hd)))
                                       ? "A RIP -- long and thin, not a chip"
                                       : "came away as a body"));
            }
            std::fflush(stdout);
        }

        // A SCRIPTED [G], on its named frame -- the world again and the player
        // somewhere new in it. Reports both halves, because "it seems to just
        // reset the player" was a report about the two being confused: the
        // WORLD moving is invisible from inside it unless you also moved.
        if (opt_.refreshFrame >= 0 && shotFrames_ == opt_.refreshFrame) {
            refreshFrom_ = pos_;
            refreshWorld();
            const float dx = pos_.x - refreshFrom_.x, dz = pos_.z - refreshFrom_.z;
            std::printf("v2: refresh test -- moved %.1f m, (%.0f, %.0f) -> (%.0f, %.0f)  %s\n",
                        double(sqrtf(dx * dx + dz * dz)), double(refreshFrom_.x),
                        double(refreshFrom_.z), double(pos_.x), double(pos_.z),
                        (dx * dx + dz * dz) > 1.0f ? "respawned"
                                                   : "DID NOT MOVE -- the player was not respawned");
            std::fflush(stdout);
        }

        // ...AND, WITH V2_DROP_RETURN, THE WALK AWAY AND BACK that arms a throw
        // (see Item::leaveFirst): two seconds after the drop, stand 6 m off it;
        // a second later, 1.5 m off. It should come to you then and not before.
        if (opt_.dropFrame >= 0 && std::getenv("V2_DROP_RETURN")) {
            Vec3 at{0, 0, 0};
            bool flying = false;
            const int k = shotFrames_ - opt_.dropFrame;
            if ((k == 120 || k == 180) && drops_.newestDrop(&at, &flying)) {
                const float off = k == 120 ? 6.0f : 1.5f;
                std::printf("v2: drop-return frame +%d: the drop is %.2f m off; standing %.1f m "
                            "from it\n",
                            k,
                            double(sqrtf((at.x - player_.pos.x) * (at.x - player_.pos.x) +
                                         (at.z - player_.pos.z) * (at.z - player_.pos.z))),
                            double(off));
                teleportTo(at.x + off, at.z);
            }
        }

        // A SCRIPTED DROP, on its named frame. Runs the same three lines the
        // Q handler does; there is no key event on this path to reach them
        // through.
        if (opt_.dropFrame >= 0 && shotFrames_ == opt_.dropFrame && held_.ready() &&
            held_.carrying()) {
            const Vec3 ddir = forward();
            const Vec3 dfrom = pos_ + camRight() * lastHeld_.cam.x + camUp() * lastHeld_.cam.y +
                               ddir * lastHeld_.cam.z;
            const int dsel = held_.selected();
            // NOT dt -- that is the frame's own delta time, a few lines up.
            const Tool &dtool = held_.tool(dsel);
            const int dmodel = held_.model();
            if (held_.dropSelected() >= 0) {
                drops_.toss(dsel, dmodel, dtool.sx, dtool.sy, dtool.sz, dfrom, ddir);
                std::printf("v2: dropped %s  from (%.2f, %.2f, %.2f)\n", dtool.name, double(dfrom.x),
                            double(dfrom.y), double(dfrom.z));
                std::fflush(stdout);
            }
        }

        // -- WHAT IS ON THE GROUND ----------------------------------------
        //
        // Ticked with the shafts, and for the same reason: both are thrown
        // objects falling through terrain that streams. Walking over one picks
        // it up, which is what stops Q being a way to lose your axe -- the JS
        // engine's autoPickup, at its own radius.
        segEnd(kSegHooks);
        {
            // The night sky's clock -- see Tracer::skyTime. DayNight carries
            // whole days separately from the fraction, so this stays continuous
            // across midnight instead of snapping back at every wrap.
            tracer_.skyTime = float((double(clock_.days) + double(clock_.tday)) * DAY_SECONDS);

            // THE WATERLINE UNDER THE CAMERA, and it is per column because the
            // two woods do not share one -- see VoxelTerrain::waterAt, which
            // hands back kNoWater for the birch band. The shader needs it to
            // know whether a lit point is submerged (caustics) and whether the
            // eye itself began the frame under the surface.
            tracer_.waterY = world_.terrain.waterSurfaceAt(pos_.x, pos_.z);
            // ...and a WALL clock for the waves. Not the day clock: X plus
            // scroll runs that at up to forty times speed and backwards, and a
            // lake that reverses its chop when you scrub the sun is a bug.
            waveClock_ += dt;
            tracer_.waterTime = waveClock_;

            // THE SWELL AS GEOMETRY, staggered a chunk at a time -- see
            // World::tickWaves. The shader clock above still drives the
            // caustics; the surface itself is voxels now and moves by being
            // re-meshed, which is why this is rationed rather than done all at
            // once: the mesh is 0.04 ms a brick and the BLAS behind it is 1.05
            // ms a chunk.
            world_.tickWaves(dt);

            // -- IS IT ACTUALLY SIMULATING? ---------------------------------
            //
            // A wiring check with teeth, and the reason it exists is that every
            // cheap way of asking this question lies. PhysX prints a healthy
            // status line whether or not anything is stepped; a body that never
            // moves looks the same as one the scene never received; and a body
            // that falls for ever looks, for the first second, exactly like one
            // that is going to land. So the probe asks the question that has
            // only one right answer: drop boxes from a known height and see
            // whether they come to rest ON THE GROUND -- at their own half
            // extent above the terrain the renderer draws, and asleep.
            //
            // That single number exercises the whole chain: the scene exists,
            // step() runs it, the height field tile was built at the right
            // origin and the right scale, and the solver's ground and the
            // renderer's ground are the same surface.

            // THE EYE AS WELL AS THE FEET: the reach is measured from the
            // player, but a drop being absorbed converges on the CHEST, which
            // is a fixed drop below the eye rather than a height above the
            // ground. See kAbsorbEyeM.
            // THE LEVEL'S OWN FLOOR WHEN THE LEVEL IS OPEN -- see Drops::FloorF
            // and the item that went through the map. levelFloorBelowM scans
            // DOWN from the item, so a thing dropped on a first storey lands on
            // that storey rather than on the lawn under the building.
            drops_.update(dt, walkWorld(), player_.pos, player_.eyePosition(),
                          world_.levelOn()
                              ? Drops::FloorF([this](float x, float y, float z) {
                                    return world_.levelFloorBelowM(x, y, z);
                                })
                              : Drops::FloorF());
            // THE SOUND GOES WITH THE SNATCH, so it lands on the frame the item
            // leaves the ground rather than 360 ms later when the flight
            // arrives -- see ToolSounds::pickedUp for that engine's own note
            // about having got this the wrong way round first.
            //
            // ONE SOUND FOR A WHOLE PILE, and that is why snatchedNow stayed a
            // BOOL when the arrivals became a list. Several items leaving the
            // ground on one frame is one event to a listener; five copies of a
            // 60 ms sample starting on the same sample boundary is not five
            // pickups, it is a click.
            if (drops_.snatchedNow()) toolSfx_.pickedUp();
            // EVERY ARRIVAL, NOT THE FIRST. See Drops::arrivedThisTick -- a
            // pile converges together now, so more than one can land on a
            // frame, and taking only one would drop the rest out of the world.
            for (const int back : drops_.arrivedThisTick()) {
                // NAMED BEFORE THE GIVE, and it has to be: give() only changes
                // what is in the HAND when the hand is empty, so asking name()
                // afterwards reports whatever you are still holding. Walking
                // over a dropped axe while carrying a pick said "picked up
                // stone pick", which is the one thing that did not happen.
                const char *what = held_.tool(back).name;
                held_.give(back);
                // A FRUIT YOU PICKED ON PURPOSE GOES INTO THE HAND, not just
                // into the kit -- see pickFruit and Drops::grabFrom. Cleared
                // whichever way it goes, so a later walk-over pickup of the
                // same kind does not steal the hand.
                if (back == selectOnArrive_) {
                    held_.select(back);
                    // ...and the press that picked it is still spent, because
                    // the button may well still be down -- see spendEatPress.
                    held_.spendEatPress();
                    selectOnArrive_ = -1;
                }
                std::printf("v2: picked up %s\n", what);
                std::fflush(stdout);
            }
        }

        // Everything that has come off the static world: the solver, and the
        // pieces it is carrying. Before the arrows only because both want the
        // same frame's dt and this one also owns the clock they are timed on.
        segEnd(kSegDrops);
        stepLoose(dt);

        // The shafts in the air. AFTER processInput, so one loosed this frame
        // starts moving on the frame it left rather than the next.
        // -- A SHAFT NEEDS A WIDER WORLD THAN A PAIR OF FEET --------------
        //
        // See kArrowSolidsM. Only while something is actually in flight: the
        // gather walks every resident chunk's solids, and paying that on every
        // frame of a walk in the woods to serve an arrow nobody has loosed
        // would be the wrong trade entirely.
        // WHERE IT WAS AND WHERE IT IS GOING -- see Arrows::LifeF, and
        // LifeHits::along for what is done with the pair.
        const auto onLife = [this](const Vec3 &a, const Vec3 &b) { return arrowKill(a, b); };
        if (arrows_.inFlight() > 0) {
            const WalkWorld aw = wideWalkWorld(kArrowSolidsM);
            arrows_.update(dt, aw, onLife);
        } else {
            arrows_.update(dt, walkWorld(), onLife);
        }
        // ...and each one that stopped this tick lands with a thud, quieter the
        // further off it stuck. Drained here rather than inside the flight so
        // the sound is not fired from the 5 ms integration substep loop.
        for (const Vec3 &at : arrows_.landedThisTick())
            toolSfx_.arrowLanded(length(at - pos_));
        // ...AND EACH ONE TAKES A CHIP OUT OF WHAT IT HIT. Drained here for the
        // same reason the thud is: a carve re-meshes a chunk and must not run
        // from inside the 5 ms flight loop. See arrowChip.
        for (const Arrows::Impact &im : arrows_.impactsThisTick()) {
            // -- SPARKS OFF A SHAFT, THE SAME ONES A TOOL THROWS -----------
            //
            // (user 2026-09-17: "when the arrow makes contact with the
            //  environment, have sparks fly. the same sparks when a stone
            //  axe/pick hits something.")
            //
            // THE SAME CALL, not a copy of it. toolSparks is what the swing
            // throws -- one burst, one clash height, one colour -- and a second
            // entry point would be a second place for the ember to drift away
            // from the one the tools use.
            particles_.toolSparks(im.at, simMs_);
            arrowChip(im.at, im.dir);
        }

        // -- ...AND THEN IT GOES WITH WHAT IT LANDED IN --------------------
        //
        // (user 2026-09-20: "have the arrow go with whatever it landed on ...
        //  it should fall with the chunk for example, or go with the lifes
        //  peices. currently its stays in the air when shooting a fly".)
        //
        // AFTER BOTH OF THE ABOVE, AND THAT IS THE WHOLE ORDERING. A shaft
        // that killed something broke it into pieces inside arrows_.update
        // (arrowKill calls shatterFlyer), and a shaft that struck the world
        // knocked its chunk loose in the chip loop that just ran -- so this is
        // the first moment at which the thing it is standing in exists as a
        // body. Asking any earlier finds nothing and the shaft hangs in the
        // air, which is the report.
        //
        // THE SLACK IS THE CHIP'S OWN REACH. A shaft rests at the last point
        // OUTSIDE what it hit, so it is always a little proud of the surface;
        // half a metre covers that without letting it grab a piece it is
        // merely near.
        arrows_.attachRides([this](const Vec3 &p, double *gen, Vec3 *pos, float *q) {
            const int b = world_.bodyNear(p, 0.5f, gen);
            if (b >= 0) world_.bodyPose(b, *gen, pos, q);
            return b;
        });
        // ...and every shaft already riding one is carried by it. Before the
        // draw and after the solver, so what is published is where the piece
        // actually is this frame rather than where it was last.
        arrows_.rideStep([this](int b, double gen, Vec3 *pos, float *q) {
            return world_.bodyPose(b, gen, pos, q);
        });

        // -- ...AND THE SAME THREE THINGS FOR THE RIFLE'S ROUNDS -------------
        //
        // (user 2026-09-17: "also have the guns bullets take out a small chunk
        // of the environment much like the arrow does.")
        //
        // THE CHIP IS arrowChip, NOT A COPY OF IT. That function backs off
        // along the shaft and re-probes with swingRay, which is the ONE
        // classifier in this engine that decides rock-or-trunk-or-ground -- and
        // its own note says why a second opinion is the shape of every material
        // bug in this file. A bullet is a faster arrow; it should chip through
        // the same door.
        //
        // DRAINED HERE for the reason the arrow's is: a carve re-meshes a chunk
        // and must not run from inside the march loop.
        if (bullets_.inFlight() > 0) {
            const WalkWorld bw = wideWalkWorld(kBulletSolidsM);
            bullets_.update(dt, bw,
                            [this](const Vec3 &a, const Vec3 &b) { return arrowKill(a, b); });
        } else {
            // THE LAST ROUND'S IMPACT IS STILL IN THE LIST. See
            // Bullets::clearImpacts -- the shot that lands is the shot that
            // closes this gate behind itself, so without this the drain below
            // re-chips one hole every frame until another round is fired.
            bullets_.clearImpacts();
        }
        // -- ...AND NOTHING IN THE WORLD IS LEFT HANGING --------------------
        //
        // (user 2026-09-17: "create a mechanic layer for this ... then when a
        //  floating voxel is detected, turns into a rigid body.")
        //
        // ONE BOUNDED SLICE PER FRAME. It is doing a flood fill over a 9.6 m
        // region, which is far too much for one frame, so it does a few
        // thousand cells of it and comes back -- see stepFloatWatch. Called
        // unconditionally rather than behind a "did anything change" test:
        // what it is looking for is precisely the thing no single edit knows
        // it caused.
        if (physics_.available()) {
            const int made = world_.stepFloatWatch(physics_, simMs_);
            if (made && opt_.swingLog) {
                const auto fs = world_.floatStat();
                std::printf("v2: the float watch dropped %d bodies (%ld pieces, %ld voxels,"
                            " %ld queued)\n",
                            made, fs.piecesDropped, fs.voxelsDropped, fs.queued);
                std::fflush(stdout);
            }
        }

        // -- AND A SHAFT STANDING IN SOMETHING IS PICKED UP OFF THE WALL ----
        //
        // (user 2026-09-17: "just the arrow thats actually impacted into the
        //  terrain gets absorbed by the player. do not create another arrow.")
        //
        // NO DROP. The first cut spilled one at the impact, on the wheat's
        // terms -- and the wheat is CONSUMED when it pays out, so its drop IS
        // the plant. An arrow is still there, so a drop made two arrows out of
        // one: the real shaft in the wall and a second hovering beside it.
        //
        // WALKING UP TO IT IS THE WHOLE INTERACTION, which is what the rest of
        // the kit calls absorbing. kArrowAbsorbM is the radius a chip is
        // collected at, reused so that reaching for a thing is one distance in
        // this game rather than several.
        //
        // -- AND IT FLIES IN ON THE CHIP'S OWN CURVE ------------------------
        //
        // (user 2026-09-17: "when absorbing the arrow in the terrain, have to
        //  get absorbed by the player just like chunks from tools".)
        //
        // TWO CALLS FOR ONE COLLECT. takeStuckNear only takes the shaft off the
        // wall and starts it moving; stepGrabs walks the curve and reports the
        // frame it arrives, which is when the kit item and the sound are due.
        // Paying out at the START would be the old behaviour with an animation
        // bolted on -- you would have the arrow before it reached you.
        //
        // A LOOP RATHER THAN AN IF, because two shafts standing in the same
        // wall are collected together and each one is an arrow.
        if (arrowTool_ >= 0) {
            arrows_.takeStuckNear(pos_, kArrowAbsorbM);
            for (int n = arrows_.stepGrabs(pos_, dt); n > 0; --n) {
                held_.give(arrowTool_);
                toolSfx_.pickedUp();
            }
        }
        for (const Bullets::Impact &im : bullets_.impactsThisTick()) {
            if (opt_.swingLog) {
                std::printf("v2: round hit (%.2f %.2f %.2f)\n", im.at.x, im.at.y, im.at.z);
                std::fflush(stdout);
            }
            arrowChip(im.at, im.dir, kBulletChipVox);
        }

        // ...and the flock. It gathers its OWN colliders rather than taking the
        // six metres around the player that walkWorld carries: a butterfly is
        // up to eighty metres away and the trunks it has to miss are the ones
        // around IT. See Butterflies::decide for the clock that keeps that
        // affordable.
        // -- ...AND NONE OF IT HAPPENS IN THE PAUSE ROOM ------------------
        //
        // Every population below claims its sites from the lattice AROUND THE
        // PLAYER, and in the room the player is at (-4096, 2048, -4096). So a
        // tick in here gathered butterflies, songbirds, salmon, lily pads,
        // dragonflies and rabbits two kilometres up in the air over a hillside
        // four kilometres from the wood, and held their slots showing them --
        // which is the exact fault the editor's own branch below was written to
        // avoid, on the same band, for the same reason.
        //
        // -- AND NOTHING IS PAUSED WHILE THE BUTTONS ARE UP -----------------
        //
        // There was an `if (!pauseOpen_)` round everything below, and in the
        // room it was right: the player had LEFT, four kilometres up, and a
        // band that went on claiming sites round the camera gathered
        // butterflies, salmon, lily pads and rabbits over a hillside nobody
        // was standing on. The editor deck still has exactly that branch a
        // little further down, and still needs it.
        //
        // THE PANEL IS NOT A PLACE, so there is nothing to leave and nothing
        // to freeze. The player stands in the wood with three buttons hanging
        // in front of them and CAN STILL WALK AND LOOK -- which is what
        // settles it: a world where you move and nothing else does is not a
        // pause, it is a bug. Either the walk stops too or the wood keeps
        // living, and the wood keeping living is what "put the 3 balls in
        // front of the player IN GAME" describes.
        segEnd(kSegPhysics);
        hStart();
        lapBegin();
        flock_.update(dt, world_, player_.pos);
        lap(kLapButterflies);
        // -- PERCHES ARE LOOKED FOR FURTHER OUT THAN COLLISION IS ----------
        //
        // solids_ is gathered at six metres, which is the distance the PLAYER
        // can walk into something. A bird may sit in any tree you can see, so
        // it needs its own query at its own radius -- handing it the collision
        // list confined the whole population to the few trunks within arm.s
        // reach of the player, which is not a wood full of birds.
        //
        // NOT EVERY FRAME. This is a wide query and the answer barely changes
        // while you walk: trees do not move, and a bird only consults it when a
        // perch has to be filled. Twice a second is instant to a player and
        // costs a fraction of what the collision query costs -- the JS engine
        // makes the same call about its own perch check for the same reason.
        ++frameTick_;
        // GATHERED FOR TWO SYSTEMS NOW, so it is no longer conditional on the
        // birds being ready: the bunnies sense obstacles out of this same list
        // (see Bunnies::blocked), and a bunny whose sensor is empty is a bunny
        // that walks through boulders. 115 m covers the bunnies' own 105.
        // FOUR WIDE GATHERS SHARE THIS HALF-SECOND CLOCK AND NONE OF THEM
        // SHARE A FRAME -- see the three below, which carry the reasoning.
        // This one keeps tick 0 because it is the widest of the four: 115 m of
        // colliders is every chunk in a square a hundred metres across.
        if ((frameTick_ % 30) == 0)
            world_.collidersNear(player_.pos, kBirdKeepM, &perches_);
        lap(kLapPerchGather);
        birds_.update(dt, perches_, player_.pos);
        lap(kLapBirds);
        // WHAT LIVES ON THE WATER. Ticked beside the birds because it is the
        // same kind of thing -- a small population that follows the player --
        // and published in the same window, so the flyer band is written once.
        // THE PADS REACH AS FAR AS THE GROUND DOES -- see LakeLife::padReachM.
        // Taken from the world's own ring rather than hard-coded, so --view
        // moves both together and they cannot drift apart.
        // WHICH WOOD -- the betta is the cherry's own fish, see LakeLife::wood.
        // Set once rather than per frame would be enough, but this is where the
        // lake's other per-frame knob is and one place is easier to keep right.
        if (!lake_.wood) lake_.wood = [this](float x) { return world_.terrain.woodBit(x); };
        lake_.padReachM =
            float(maxi(1, world_.viewChunks)) * float(CHUNK_VOX) * VOXEL_M;
        lake_.update(dt, world_.terrain, player_.pos, forward());
        // ...AND ANY DUCKLING THAT WEPT THIS TICK GETS ITS DROPLET. The lake
        // queues world points rather than reaching into the particle pool --
        // see LakeLife::cryTick -- so this is the one line that joins them, and
        // it is the same shape as the arrow's landed/impact drains above.
        for (const Vec3 &t : lake_.tearsThisTick()) particles_.tear(t, simMs_);
        lake_.publish(world_);
        lap(kLapLake);
        // THE GROUND THE FLOCK FOLLOWS IS THE ONE EVERYTHING ELSE STANDS ON,
        // handed in rather than reached for -- see BirdFlock::update. The
        // generator's own height, not the walk's: a bird does not care about a
        // hole somebody dug, and asking the edit layer would cost a scan per
        // lookahead sample per bird per frame.
        flock2_.update(dt, player_.pos,
                       [this](float x, float z) {
                           return float(world_.terrain.heightVox(
                                            int(std::floor(x / VOXEL_M)),
                                            int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
                       },
                       // ...AND THE WOOD, which a bird thirty metres up was
                       // argued not to need -- see the note over its update.
                       &perches_);
        flock2_.publish(world_, kButterflySlots + kBirdSlots + kLakeSlots);
        lap(kLapFlock);
        // ...AND THE BUNNIES, on the generator's ground for the reason the
        // songbirds are: a rabbit does not care about a hole somebody dug, and
        // asking the edit layer would cost a scan per probe per animal.
        // ON THE DECK THE EDITOR OWNS THIS BAND AND THE POPULATION DOES NOT
        // TICK AT ALL. Not an optimisation: fill() claims sites from the
        // lattice around the PLAYER, and the player is four kilometres away at
        // y 512, so a tick here would spawn ten rabbits on whatever hillside
        // happens to lie under the stage and hold nine slots showing them.
        if (world_.staged()) {
            edit_.update(dt);
            edit_.publish(world_, kBunnySlot0);
        } else {
            const WalkWorld bgw = groundWorld();
            bunnies_.update(dt, player_.pos,
                            // ...AND THE SAME FLOOR THE CRITTERS TAKE, for
                            // the same reason -- see the block over
                            // critters_.update. The note that used to stand
                            // here said "a rabbit does not care about a hole
                            // somebody dug", and a rabbit standing in the air
                            // over one is the same sight the ants were
                            // reported for. These are ground animals inside
                            // the reach anybody digs in.
                            [&bgw](float x, float z) { return walkGroundM(bgw, x, z); },
                            // IS THIS COLUMN UNDER WATER -- the same three calls
                            // WaterField::rebuild makes, which is this engine's
                            // one definition of wet. A bunny and a lily pad
                            // therefore agree about where the lake is.
                            [this](float x, float z) { return wetColumnAt(x, z); }, perches_,
                            // WHICH WOOD, for the species that belong to one.
                            // The generator's own band weight, not a threshold:
                            // the fill is the only caller and it makes its own
                            // decision about where the seam is.
                            [this](float x) { return world_.terrain.woodBit(x); },
                            // ...AND NOT ON THE BEACH -- see Bunnies::blocked.
                            [this](float x, float z) { return sandAt(x, z); });
            // -- AND IF ONE OF THEM REACHED YOU ---------------------------
            //
            // (user 2026-09-19: "have the cobra attack the player instead of
            //  running away from the player. same thing for the scorpion
            //  too.")
            //
            // The marchers report and the app spends -- see
            // Bunnies::biteDamage. On the 20-point scale, which Vitals::hurt
            // converts at its own door, so the cobra's 5 is two points of a
            // five-point bar and the scorpion's 3 is one. v1's numbers.
            if (const int bit = bunnies_.biteDamage(); bit > 0)
                vitals_.hurt(bit, bit >= 5 ? "a cobra struck you" : "a scorpion stung you");
            bunnies_.publish(world_, kBunnySlot0);
            lap(kLapMammals);
            // ...AND THE MARCHERS, WHICH HAVE THEIR OWN RUN OF THE BAND. The
            // editor branch above deliberately does not publish them: the deck
            // is not the wood, and despawnAll() has already given every slot up.
            bunnies_.publishSkunks(world_, kMarchSlot0);
            // -- AND THE BEES, WHICH FOLLOW THE HIVES ----------------------
            //
            // The two lists are gathered on the birds' own half-second clock:
            // a hive is a placement in a crown and a flower is a placement on
            // the ground, and neither moves. kBeeHiveM decides how far a hive
            // may be and still have a swarm; the blooms are gathered a little
            // wider than a bee will fly, so a bee at the edge of its range
            // still has flowers to choose between.
            // -- ON THE SAME CLOCK, BUT NOT ON THE SAME FRAME ------------
            //
            // (user 2026-09-15: "just as im walking around the environment its
            // hitching".)
            //
            // ALL THREE OF THESE USED TO LAND TOGETHER. Each is a wide query --
            // decorNear walks every decor item of every chunk in a square, and
            // bankSpots re-derives a shoreline -- and putting them on one frame
            // adds their costs where it hurts most: twice a second, the frame
            // that runs them does three wide scans and the twenty-nine either
            // side do none. Measured over a walk, that frame ran 7 to 13 ms of
            // population work against a 4 ms median.
            //
            // THE PERIOD IS UNCHANGED AND SO IS EVERY ANSWER. A hive is a
            // placement in a crown and a shoreline does not move; all that
            // differs is WHICH frame of the thirty re-reads each one, so the
            // lists are still refreshed twice a second and the peak is a third
            // of what it was. The offsets are spread across the period rather
            // than merely made distinct -- 0, 10, 20 -- so no two can drift
            // back together.
            // TICK 0 IS ALREADY TAKEN, and by the biggest of them: the
            // perch gather a few hundred lines up runs collidersNear at 115 m
            // on exactly that frame. The first cut of this stagger moved the
            // blooms and the bank off each other and left the hives sitting on
            // top of it, which is why the population spike survived being
            // spread -- measured at 7 to 9 ms against a 4 ms median, on one
            // frame in thirty, twice a second, for ever.
            //
            // So all four are spread over the period: perches 0, hives 8,
            // blooms 16, bank 24.
            const int tick30 = frameTick_ % 30;
            if (tick30 == 8) world_.decorNear(5, player_.pos, kBeeHiveM, &hivesNear_);
            if (tick30 == 16)
                world_.decorNear(2, player_.pos, kBeeHiveM + kBeeFlowerM, &bloomsNear_);
            // ...AND THE BANK, for the frog. Same argument: a shoreline does
            // not move, and re-deriving it per frog per frame is the wide query
            // this engine pays once.
            if (tick30 == 24) lake_.bankSpots(uint32_t(frameTick_), 8, &banksNear_);
            lap(kLapDecorGather);
            bees_.update(dt, player_.pos, hivesNear_, bloomsNear_, &perches_);
            bees_.publish(world_, kBeeSlot0);
            lap(kLapBees);
            // -- AND THE FOUR SMALL ONES -----------------------------------
            //
            // The same three predicates the marchers take, because they are
            // asking the same three questions: how high is the ground, is it
            // under water, and which wood is this. The LOOK is handed in as
            // well so a critter is not born in the middle of your view -- see
            // BirthGate::mayAt.
            // -- ON THE FLOOR THE PLAYER WALKS ON, NOT THE GENERATOR'S ----
            //
            // (user 2026-09-20: "I saw ants walking well above the ground.")
            //
            // heightVox IS THE GENERATOR'S TOP AND CANNOT SEE AN EDIT. That
            // is the identical bug the player's own walk had -- "I created a
            // hole, then when I try to go inside the hole the player still
            // floats above it" -- and the identical fix: walkGroundM routes
            // through TerrainProbe, which is this engine's one implementation
            // of "generated, then edited". Dig a pit and an ant strolled
            // across it at the height the ground used to be.
            //
            // WHY THE CRITTERS AND NOT THE BIRDS. The cost argument the flock
            // and the rabbits carry is real but it is about SAMPLE COUNT: a
            // bird probes its lookahead every frame and is thirty metres up,
            // where a hole is invisible. An ant is two centimetres tall,
            // stands on the ground the player is looking at, and there are
            // six of them. And the probe is not a scan -- topVox returns
            // heightVox the moment EditStore::get comes back empty, which is
            // every column bar the handful anyone has hit.
            //
            // THE CLIP TEST COULD NOT HAVE CAUGHT THIS. Its agl column asks
            // the same heightVox the animal was positioned with, so it
            // reported 0.05 m for the ant while the ant hung in the air: a
            // measurement taken with the instrument under test.
            // HOISTED, and it has to be: see groundWorld. Two pointers, no
            // query, shared by every probe this tick.
            const WalkWorld gw = groundWorld();
            critters_.update(dt, player_.pos,
                             [&gw](float x, float z) { return walkGroundM(gw, x, z); },
                             [this](float x, float z) { return wetColumnAt(x, z); },
                             [this](float x) { return world_.terrain.woodBit(x); }, banksNear_,
                             perches_,
                             // AFTER DARK. v1 swaps its flyer band on the sun's
                             // own sign; this is the same test in degrees, a
                             // shade below the horizon so the fireflies come up
                             // as the light goes rather than the instant it
                             // crosses zero.
                             isNight(), forward(),
                             // ...AND THE LAKE'S OWN TOP, which the ground
                             // query cannot give -- see waterTopAt.
                             [this](float x, float z) { return waterTopAt(x, z); },
                             [this](float x, float z) { return sandAt(x, z); });
            critters_.publish(world_, kCritterSlot0);
            lap(kLapCritters);
            // The sparks are on the same clock as everything else in the band.
            // update() only retires what has run out -- a particle's position
            // is a closed form off its birth, so nothing here integrates.
            particles_.update(simMs_);
            particles_.publish(world_, simMs_);
            lap(kLapParticles);
            hLife_ += hStop();
            // ...AND THE RED, AFTER EVERY POPULATION HAS PUBLISHED. It is
            // written onto instances the populations have just re-placed, and a
            // slot that stopped being drawn keeps what it last held -- see
            // World::setFlyerHurt.
            lifeHits_.publish(world_, simMs_);
            // ...AND THE ONE MATERIAL IN THE WORLD THAT EMITS. Set every frame
            // rather than once, because it costs one compare and because a
            // firefly that failed to load must not leave a live material id
            // pointing at whatever took its palette entry instead.
            // -- ...AND IT IS OFF WHEN THE FIREFLY IS AN EMBER ------------
            //
            // Two mechanisms must not claim one material. If the firefly wears
            // the SPARK's voxel then V6Params::emitters already lights it, and
            // leaving glowMtl pointing at the same id would mean whichever the
            // shader tests first decides how bright every ember in the world is
            // -- the glow branch runs before the emitter loop, so a firefly's
            // 26 nits would be handed to every spark a blow throws.
            const bool fireflyIsEmber = critters_.sharesSpark();
            tracer_.glowMtl = (!fireflyIsEmber && critters_.glowMtl())
                                  ? uint32_t(critters_.glowMtl())
                                  : 0xFFFFFFFFu;
            tracer_.glowRadiance = Vec3(kGlowNits, kGlowNits * 0.85f, kGlowNits * 0.18f);
        }
        segEnd(kSegLife);

        // -- THE PENDANTS IN NUKETOWN'S ROOMS -------------------------------
        //
        // (user 2026-09-17: "put a lightbulb in the dark rooms".)
        //
        // EVERY FRAME, FOR THE FIREFLY'S REASON one line up, and for a second
        // one: the shader has a single point light, so which of the map's bulbs
        // is lighting the room is a function of where the eye is -- see
        // World::nearestBulb, which carries that argument.
        //
        // OFF EVERYWHERE BUT THE LEVEL. bulbMtl doubles as the on switch, so
        // clearing it here is what keeps the wood paying one compare -- and
        // what stops a material id from the level lighting up a pine.
        //
        // THE NUMBERS ARE THE PAUSE ROOM'S, unchanged: 15 cm of glass and a
        // warm 3000 K, because every other light in this engine is daylight and
        // a room lit by the same white as the sky reads as a void with walls.
        //
        // ALL OF THEM, EVERY FRAME, AND THE SHADER PICKS PER SHADING POINT.
        //
        // The first cut handed over the one bulb nearest the CAMERA, which is
        // a light that walks around with you: step out of a room and the room
        // goes dark behind you ("the lightbulb seems to turn off when the
        // player is away", user 2026-09-17). Publishing the whole set and
        // choosing per shaded vertex leaves every room lit by its own lamp for
        // as long as the level is open, and still costs one shadow ray -- see
        // kBulbSlots in Shared.slang.
        {
            if (world_.levelOn() && world_.levelBulbMtl() != mat::AIR &&
                !world_.levelBulbs().empty()) {
                tracer_.bulbs = world_.levelBulbs();
                if (tracer_.bulbs.size() > size_t(kBulbSlots))
                    tracer_.bulbs.resize(size_t(kBulbSlots));
                // The fallback the shader uses if the array is ever empty, and
                // what a one-lamp caller would set on its own.
                tracer_.bulbPos = tracer_.bulbs.front();
                tracer_.bulbRadius = 0.15f;
                tracer_.bulbRadiance = Vec3(1.00f, 0.86f, 0.66f) * 12.0f;
                tracer_.bulbMtl = uint32_t(world_.levelBulbMtl());
            } else {
                tracer_.bulbMtl = 0xFFFFFFFFu;   // kNoBulb
                tracer_.bulbs.clear();
            }
            // ...AND THE EMBERS JOIN THEM. See publishSparkLights.
            publishSparkLights();
        }

        // The bed follows the canopy. Fed the same dt as the walk and the day
        // cycle -- the shot clock when one is running -- so a scripted move
        // and a live one fade identically.
        ambience_.update(dt, forestGain());

        // Simulation is over: the camera is where it is going to be and the ring
        // has streamed. Everything after this is the GPU's frame.
        //
        // ...AND THE BREADCRUMB IS DROPPED HERE FOR THAT REASON: this is the
        // last point in the frame at which the player is where the frame will
        // draw them. Four plain stores -- see platform/crashlog.h -- so that a
        // fault anywhere in the GPU half, including on a driver thread we do
        // not own, still knows which frame and which place it died in.
        v2::crumbFrame(long(frameTick_), player_.pos.x, player_.pos.y, player_.pos.z, simMs_);
        segEnd(kSegAmbience);
        sl_.markSimulationEnd();


        uint32_t fw = target->getWidth(), fh = target->getHeight();
        // A minimised window reports nothing to draw into, and idling on that
        // is right for a person's game -- but a background instance is
        // minimised ON PURPOSE and exists to be measured, so it keeps drawing
        // at the size it was asked for instead of collapsing to the floor.
        if (opt_.background && (fw == 0 || fh == 0)) {
            fw = uint32_t(maxi(16, opt_.r.width));
            fh = uint32_t(maxi(16, opt_.r.height));
        }
        if (fw == 0 || fh == 0) return;

        // -- three sizes, and only one of them is the window ------------------
        //
        // THE GAME'S RESOLUTION IS NOT THE WINDOW'S. --scale, and the Resolution
        // row in the menu, set how big a frame the renderer PRODUCES; the
        // swapchain blit then stretches that up to whatever the window happens
        // to be. Nothing here resizes a window, and nothing about the world or
        // the integrator changes with it -- it is the one frame-rate control
        // that costs only sharpness.
        //
        // WITH RECONSTRUCTION there is a third size underneath that one. DLSS
        // owns the relationship between what is traced and what comes out: the
        // quality mode picks the input for a given output, and it is asked
        // rather than assumed because the optimal ratio for a mode is the
        // model's business and has changed between versions. What it is asked
        // ABOUT is the produced size above, NOT the window -- which is what lets
        // the slider go on meaning what it says with the denoiser running,
        // instead of being a control the quality mode overrules.
        //
        // WITHOUT IT the traced size and the produced size are one number, and
        // --scale is the plain fraction it always was.
        bool useDlss = opt_.dlss && dlss_.available();
        const int ow = maxi(16, int(float(fw) * opt_.scale));
        const int oh = maxi(16, int(float(fh) * opt_.scale));
        int rw = ow, rh = oh;
        if (useDlss) {
            uint2 rs{0, 0};
            if (dlss_.optimalRenderSize(uint2(uint32_t(ow), uint32_t(oh)), opt_.dlssQuality, &rs) &&
                rs.x && rs.y) {
                rw = int(rs.x);
                rh = int(rs.y);
            } else {
                useDlss = false;
            }
        }
        // Before resize(), because the hint is read when the FEATURE is
        // created and a feature already built ignores it.
        dlss_.preset = opt_.rrPreset;
        tracer_.setDenoising(useDlss);
        tracer_.resize(rw, rh, ow, oh);


        RenderSettings cfg = opt_.r;
        // Shorter paths while moving are a frame-rate trade, but they also
        // change the ESTIMATOR -- fewer bounces is a different amount of noise
        // and a different amount of light. With constant grain that would leave
        // a visible step between standing and walking even with the sample
        // counts matched, so the path length is pinned too. Bounces are nearly
        // free here, so this costs very little of what the trade was buying.
        if (moving_ && !opt_.constantGrain)
            cfg.maxDepth = mini(opt_.r.maxDepth, opt_.movingDepth);

        // HOW MUCH THE FILM REMEMBERS, which is the same thing as how much
        // grain it keeps: a rolling average of N samples never converges, so N
        // IS the noise floor.
        const float rate = fabsf(clock_.cycleSpeed);
        if (opt_.constantGrain) {
            // NO CAP, because a cap is the wrong tool and was subtly wrong.
            //
            // Capping the film at N samples does not give N samples: once full
            // it becomes an exponential moving average with weight N, and an
            // EMA's variance is sigma^2/(2N+1) rather than sigma^2/N. So a
            // standing camera came out about sqrt(2) QUIETER than a walking
            // one. The film is instead thrown away every displayed frame below,
            // so still and moving are both the mean of the same N fresh samples
            // and the noise is identical rather than merely close.
            cfg.maxAccum = 0u;
        } else if (clock_.paused || rate == 0.0f) {
            cfg.maxAccum = 0u;  // nothing moving: converge without limit
        } else {
            // Otherwise the window is sized in TIME rather than in samples, so
            // it does not change meaning with the frame rate. An N-sample
            // rolling mean lags by about N/2 frames, and the sun moves 0.15
            // deg/s of azimuth at cycleSpeed 1, so a couple of seconds is a
            // fraction of a degree and invisible on soft shadows.
            const float n = 2.0f * maxf(30.0f, fps_) * 1.2f / maxf(1.0f, rate);
            cfg.maxAccum = uint32_t(clampf(n, 32.0f, 8192.0f));
        }
        liveMaxAccum_ = cfg.maxAccum;
        liveDepth_ = cfg.maxDepth;

#if V2_HAS_NRCSDK
        // -- NVIDIA'S NEURAL CACHE: the frame opens here ---------------------
        //
        // Configure() first, and it costs nothing unless the frame size or the
        // SNAPPED scene box has actually moved -- see nrcsdk.h for why the box
        // is a kilometre wide and why it is snapped rather than centred.
        //
        // BeginFrame has to precede the path tracing passes: it is what clears
        // the record counters the tracer then appends to.
        if (nrcSdk_.available() && nrcSdk_.enabled) {
            // The same position the camera is built from, twenty lines below.
            const Vec3 cp = pos_;
            if (nrcSdk_.configure(uint32_t(tracer_.width()), uint32_t(tracer_.height()), cp,
                                  uint32_t(maxi(2, opt_.r.maxDepth))))
                nrcSdk_.beginFrame(ctx, 1.0f);
        }
#endif

        // THE CINEMA RIG MAY HAVE THE CAMERA -- see ui/app_cinema.inl. After
        // life has published this frame's boxes, so it follows where the animal
        // IS; and into locals, so pos_ and the player's own look stay the
        // player's for everything else that reads them.
        Vec3 camPos = pos_, camDir = forward();
        tickCinema(dt, &camPos, &camDir);
        Camera cam;
        cam.origin = camPos;
        cam.target = camPos + camDir * 50.0f;
        cam.fovDeg = fov_;
        // A PINHOLE, AND IT WENT BACK TO BEING ONE ON PURPOSE. The viewer did
        // briefly drive the thin lens from a "Depth of field" slider, and the
        // lens itself was correct -- but Ray Reconstruction assumes every sample
        // in a pixel comes from a single point, and an open lens is exactly what
        // breaks that. The result was not shallow focus, it was a smear the
        // denoiser could not resolve, which is what the note at the top of
        // render/camera.h warned about before it was tried.
        //
        // The lens is still driven by --aperture and --focus, which are OFFLINE
        // and accumulate with no denoiser in the way. That is where it works.
        cam.aperture = 0.0f;
        cam.focusDist = 40.0f;
        const V6Camera gcam = cam.gpu(tracer_.width(), tracer_.height());



        // -- the tool in the hand, for the tracer to hit ---------------------
        //
        // HERE AND NOT IN processInput, because the pose is expressed in the
        // CAMERA'S frame and referenced to its field of view -- it cannot be
        // built until the camera for this frame exists. The bob is read off the
        // player so the head and the hand ride one stride.
        //
        // renderOffline sets it separately, in its own sample loop, for the
        // reason it has to set the fog and the sky separately: it runs none of
        // the per-frame systems, so anything hung off this tick is absent from
        // every --out image.
        segEnd(kSegSetup);
        {
            const HeldXform hx = held_.xform(gcam, player_.bobPhase, player_.bobAmp);
            // KEPT FOR THE NEXT FRAME'S RELEASE. loose() runs in processInput,
            // before the hand is placed, so what it can read is where the bow
            // was last frame -- which is what the JS engine reads too, off its
            // own prevCam. A frame of lag on a launch point that moves with the
            // bob is not something an eye can see.
            lastHeld_ = hx;
            // AFTER THE POSE, because it reads it. The badge is rewritten every
            // frame rather than on a change: the hand bobs and sways, so the
            // point it hangs off moves whether or not the COUNT does.
            setStackBadge();
            // CINEMA HIDES THE TOOL HERE, AT THE DRAW, and nowhere else -- see
            // toggleCinema for why it no longer touches held_.shown.
            const bool handDrawn = hx.show && !cinema_;
            world_.setHeldInstance(held_.model(), hx.m, hx.tx, hx.ty, hx.tz, handDrawn);
            // -- AND NOTHING IS RESET HERE. READ THIS BEFORE ADDING IT BACK --
            //
            // (user 2026-09-20: "when pulling the bow back or switching weapons
            //  there seems to be noise on the entire screen.")
            //
            // A `resetHistoryNextFrame()` used to sit on this line, on the
            // frame the held MODEL changed, put there to chase the steak's
            // residue. It is gone, and the reason is that it was never a local
            // fix: that flag is the same one a resize, a teleport and a quality
            // change set, and it throws away RAY RECONSTRUCTION'S WHOLE
            // HISTORY -- every pixel of the screen, not the tool's. What comes
            // back is one un-denoised frame of the entire wood, which is the
            // noise that was reported.
            //
            // AND IT FIRED CONSTANTLY, because a held model changes far more
            // often than a held OBJECT does: the bow steps through three models
            // as it is drawn and more as it settles, a gun steps through its
            // reload strip, a food steps through its bite frames. So the two
            // actions in the report are exactly the two that stepped a strip.
            //
            // THE TOOL'S OWN PIXELS ARE ALREADY HANDLED, and by the right
            // mechanism: V6Params::heldPrevValid drops to 0 when the held
            // OBJECT changes, which invalidates the motion vector for the item
            // and for nothing else. Its note says why the same must not be done
            // per model -- the frames of a strip share an origin and a pose by
            // construction, so their motion vector stays good, and invalidating
            // it would throw one away on every frame of a draw.
            //
            // If the residue comes back, it belongs there or in the guide
            // buffers, per pixel. It does not belong on a flag that means "the
            // world the history describes no longer exists".
            // ...and the tracer is told the same pose, because it is the one
            // thing in this scene whose motion vector cannot be worked out from
            // where it is in the world -- see V6Params::heldPrev0. It keeps its
            // own previous copy and steps it with prevCam_, which is the only
            // way the two can be guaranteed to describe the same frame.
            // ...AND WHICH TOOL IT IS, so a change of hands does not carry the
            // last one's motion vector onto this one. See heldPrevValid.
            // drawnTool(), NOT selected(). This is the identity the motion
            // vector is invalidated on (V6Params::heldPrevValid), so it has to
            // name the geometry that is on screen -- during a swap the hand
            // holds the new tool and the old one is still falling out of frame,
            // and telling the tracer the new one's name there would throw away
            // a perfectly good vector for every frame of the drop.
            tracer_.setHeldXform(hx.m, hx.tx, hx.ty, hx.tz, handDrawn, held_.drawnTool());
            hStart();
            arrows_.publish(world_);
            bullets_.publish(world_);
            drops_.publish(world_);
            // ...AND THE TWO THAT ARE PUBLISHED HERE RATHER THAN BESIDE THEIR
            // OWN TICK. They were gated on the pause room along with the tick
            // -- a publish without a tick writes the population back into the
            // band at the position it last held in the wood, undoing the
            // hideFlyerBand the room did on the way in. Both the room and that
            // hide are gone; the wood ticks while the buttons are up, so these
            // two publish with it.
            flock_.publish(world_);
            birds_.publish(world_);
            hPub_ += hStop();
            world_.refitTlas();
        }
        segEnd(kSegPublish);

        // Constant grain: start from nothing EVERY frame, not just when the
        // camera moves. Moving already did this -- it is what made a walking
        // frame noisy -- so doing it always is what makes the two identical.
        // -- V2_ABLATE -- see kAblFirst -------------------------------------
        //
        // BOOKED BEFORE THIS FRAME'S WORK: getGpuTime is the PREVIOUS frame's,
        // so it goes to the phase that frame ran in. Every switch is restored
        // and then this phase's one thing is turned off, so no phase can leak
        // into the next.
        if (opt_.profile && ablateOn()) {
            static const bool fog0 = volfog_.enabled, cloud0 = clouds_.enabled,
                              atmo0 = atmo_.enabled;
            Falcor::Profiler *ap = getDevice()->getProfiler();
            if (ap && !ablCam_) {
                for (Falcor::Profiler::Event *e : ap->getEvents()) {
                    if (!e) continue;
                    const std::string nm = e->getName();
                    const auto ends = [&nm](const std::string &tail) {
                        return nm.size() >= tail.size() &&
                               nm.compare(nm.size() - tail.size(), tail.size(), tail) == 0;
                    };
                    if (ends("/camera")) ablCam_ = e;
                    else if (ends("/fog")) ablFogEv_ = e;
                    else if (ends("/onFrameRender")) ablRoot_ = e;
                }
            }
            const int f = ablFrame_++;
            const int prev = f - 1;
            if (prev >= kAblFirst && ablCam_) {
                const int pp = (prev - kAblFirst) / kAblLen;
                if (pp < kAblCount && (prev - kAblFirst) % kAblLen >= kAblSkip) {
                    ablSum_[pp] += ablCam_->getGpuTime();
                    ablFog_[pp] += ablFogEv_ ? ablFogEv_->getGpuTime() : 0.0f;
                    ablAll_[pp] += ablRoot_ ? ablRoot_->getGpuTime() : 0.0f;
                    ++ablN_[pp];
                }
            }
            volfog_.enabled = fog0;
            clouds_.enabled = cloud0;
            atmo_.enabled = atmo0;
            const int phase = f < kAblFirst ? -1 : (f - kAblFirst) / kAblLen;
            if (phase != ablPrevPhase_ && phase >= 0 && phase < kAblCount) {
                std::printf("  ablate   phase %d: %s\n", phase, kAblName[phase]);
                std::fflush(stdout);
            }
            ablPrevPhase_ = phase;
            switch (phase) {
                case kAblNoFog: volfog_.enabled = false; break;
                case kAblNoClouds: clouds_.enabled = false; break;
                case kAblNoSky: cfg.skyRays = 0; break;
                case kAblDepth1: cfg.maxDepth = 1; break;
                case kAblDepth3: cfg.maxDepth = mini(cfg.maxDepth, 3); break;
                case kAblNoAtmo: atmo_.enabled = false; break;
                default: break;
            }
        }

        const int spf = maxi(1, opt_.samplesPerFrame);
        cfg.samplesPerFrame = spf;
        // GPU TIMESTAMPS, and only under --profile. The whole point is to be
        // able to say "the denoiser is two thirds of the frame" as a
        // measurement rather than as the difference between two runs with
        // different settings, which is a comparison of two different frames.
        Falcor::Profiler *prof = opt_.profile ? getDevice()->getProfiler() : nullptr;
        bool reconstructed = false;

        // -- the sky, before anything that reads IT --------------------------
        //
        // Ahead of the probes as well as the trace: both read skyDome(), and a
        // table rebuilt after the probes had already sampled it would light the
        // indirect fill from the PREVIOUS sun position. Almost always a no-op --
        // see the movement test in Atmosphere::update.
        if (atmo_.enabled && atmo_.available()) {
            FALCOR_PROFILE(ctx, "atmosphere");
            atmo_.update(ctx, world_.sky.gpu(), pos_.y,
                         Sky::SUN_IRRADIANCE * world_.sky.sunScale);
        }

        // -- the probes, before anything that reads them ---------------------
        //
        // THE VOLUME FOLLOWS THE PLAYER, snapped to whole probe spacings inside
        // setOrigin. An unsnapped origin would slide the grid a fraction of a
        // cell every frame, and since a probe's irradiance is a moving average
        // over about thirty frames, sliding it means every probe is permanently
        // averaging light from somewhere it no longer is. The symptom is
        // indirect light that smears along behind you as you walk.
        //
        // Traced BEFORE the camera sample, so the atlas the tracer reads
        // already holds this frame's rays rather than last frame's.
        tickFireLive();   // V2_FIRE_LIVE only; app_tests_interaction.inl
        if (opt_.r.giMode == 1 && ddgi_.available()) {
            FALCOR_PROFILE(ctx, "probes");
            ddgi_.setOrigin(pos_);
            tracer_.traceProbes(ctx);
        }

        // Fill the next band of the cloud cache. Does nothing once the volume
        // is complete, which is the normal state after the first few frames --
        // the deck is a periodic TILE and has no evolution clock, so there is
        // never anything to refill for either movement or time.
        if (clouds_.available() && !clouds_.filled()) {
            FALCOR_PROFILE(ctx, "cloudfill");
            clouds_.update(ctx);
        }

        if (useDlss) {
            // ONE FRAME'S WORTH OF SAMPLES, AND NO FILM. The reconstruction IS
            // the history, so accumulating underneath it would be two temporal
            // filters fighting: the film would hand DLSS an image whose age
            // varies with how long you stood still, while the motion vectors
            // beside it describe this frame only.
            //
            // spf still means what it says -- the samples land in the
            // accumulator and their mean is what gets handed over -- so raising
            // it lowers the input noise rather than the output lag.
            cfg.maxAccum = 0u;
            tracer_.resetAccumulation();
            {
                FALCOR_PROFILE(ctx, "trace");
                for (int i = 0; i < spf; ++i) tracer_.renderSample(ctx, gcam, cfg);
            }
            {
                FALCOR_PROFILE(ctx, "reconstruct");
                reconstructed = tracer_.reconstruct(ctx, dlss_);
            }
        } else {
            // Constant grain: start from nothing EVERY frame, not just when the
            // camera moves. Moving already did this -- it is what made a walking
            // frame noisy -- so doing it always is what makes the two identical.
            if (opt_.constantGrain) tracer_.resetAccumulation();
            FALCOR_PROFILE(ctx, "trace");
            for (int i = 0; i < spf; ++i) tracer_.renderSample(ctx, gcam, cfg);
        }
        {
            FALCOR_PROFILE(ctx, "tonemap");
            // -- the fog volume, under a scope of its OWN ---------------------
            //
            // It used to sit unlabelled inside "tonemap", which made its cost
            // read as tone mapping in every profile ever taken of this engine.
            // That was survivable when it was two dispatches over a 160x90x64
            // froxel grid; it is not now. The injection pass fires TWO RAYS PER
            // CELL over two 128x48x128 cascades -- about 3.1 million rays a
            // frame, in the same order as the camera paths themselves.
            //
            // NOTE WHAT THIS SCOPE STILL DOES NOT CATCH: the fog MARCH runs
            // inside the tracer's own shader, so it is counted under "trace".
            // The fog therefore costs time in two scopes and owns neither
            // outright, which is worth remembering before reading a number here
            // as the price of the fog.
            //
            // RUNS AFTER THE TRACE, DELIBERATELY. The trace above samples LAST
            // frame's volume -- a one-frame lag that a world-anchored volume can
            // afford, because a cell describes the same air whichever frame you
            // ask. (The froxel version could not: its cells were rebuilt from
            // the camera every frame, which is why the comment that used to sit
            // here claimed the fog was lit first. It was not, and with the grid
            // gone the claim is not even the right thing to want.)
            {
                FALCOR_PROFILE(ctx, "fog");
                tracer_.renderVolFog(ctx, gcam, opt_.r.fogDensity, opt_.r.fogHeight, moving_);
            }
            // dt, and it is the SHOT clock rather than the wall clock when one
            // is running -- the same dt the player and the day cycle step by.
            // An adaptation driven by wall time inside a --shot-walk sequence
            // would settle at a rate that depended on how fast the machine
            // happened to render, which is exactly what those flags exist to
            // take out of the picture.
#if V2_HAS_NRCSDK
            // -- ...and NVIDIA'S closes here ------------------------------------
            //
            // QueryAndTrain runs the network over the records the tracer wrote:
            // it predicts radiance at the query points, propagates it back along
            // the stored paths, and fits the network to the result. Resolve then
            // folds the predictions into the image, since none of that can happen
            // inline in the path loop.
            //
            // EndFrame goes LAST and takes the queue rather than the command list,
            // because it is the submission it waits on, not the recording.
            //
            // AND ALL OF IT RUNS BEFORE tracer_.resolve, which is v2's tone map.
            // That pass READS color_; a cache resolve that writes color_ after it
            // has run is writing into a texture nothing will read again before the
            // next trace overwrites it from the accumulator. It cost an afternoon:
            // every SDK debug view -- DirectCacheView, QueryIndex, the lot --
            // came out pixel-identical to the plain render, which reads exactly
            // like a cache that has learnt nothing.
            if (nrcSdk_.configured() && nrcSdk_.enabled) {
                nrcSdk_.queryAndTrain(ctx);
                // v2's OWN resolve, not the library's: the built-in one adds into
                // the output texture, and v2's output texture is a running mean.
                // See the note at the top of NrcSdkResolve.cs.slang.
                if ((opt_.nrcSdkDebug > 0 && opt_.nrcSdkDebug != 99) || opt_.nrcSdkBuiltin)
                    nrcSdk_.resolve(ctx, tracer_.color().get());
                else
                    tracer_.runNrcSdkResolve(ctx);
                // Printed occasionally: a loss that never moves off zero means the
                // library is receiving no training records at all, which looks
                // exactly like a cache that is working badly.
                if ((nrcSdkLogTick_++ % 120u) == 0u)
                {
                    uint32_t nq = 0, nt = 0;
                    nrcSdk_.readCounters(nq, nt);
                    std::printf("  nrc recs query %-8u training %-8u loss %.6f\n",
                                nq, nt, nrcSdk_.trainingLoss());
                }

                // -- SUBMIT, AND ONLY THEN END THE FRAME ---------------------
                //
                // EndFrame takes the command QUEUE, not the command list, and the
                // guide is explicit that it goes after the list has been
                // submitted: it is what the library waits on to know its own work
                // has run. Falcor owns submission and does it when onFrameRender
                // returns, so left alone this called EndFrame on a queue that had
                // never been given the frame's work -- and the symptom was not an
                // error but a training loss of exactly zero, for ever.
                //
                // ddgi.h already flushes mid-frame for the same reason: an SDK
                // that reaches past the abstraction needs the abstraction to have
                // caught up first. false, not true -- the queue has to have the
                // work, but nothing here needs to block on it finishing.
                ctx->submit(false);
                nrcSdk_.endFrame();
            }
#endif

            {
                FALCOR_PROFILE(ctx, "resolve");
                tracer_.resolve(ctx, cfg, reconstructed, dt);
            }

            // -- teach the cache what this frame found -----------------------
            //
            // AFTER the trace, because the records it fits were written by it,
            // and before the next one, so nothing accumulates across frames.
            //
            // The count is CALCULATED, not read back. The tracer selects one
            // pixel in trainEvery, so the host already knows how many records
            // exist; asking the GPU would mean a stall to learn a number that
            // was never in doubt.
            if (nrc_.shouldTrain()) {
                FALCOR_PROFILE(ctx, "nrc");
                const uint32_t traced = uint32_t(tracer_.width()) * uint32_t(tracer_.height());
                const uint32_t n = traced / uint32_t(maxi(1, nrc_.trainEvery));
                const Vec3 sd = world_.sky.sunDir();
                nrc_.trainBatch(ctx, mini(n, kNrcMaxSamples),
                                Falcor::float3(sd.x, sd.y, sd.z));
            }
        }


        // Guarded because a background instance may have nothing on screen to
        // blit to: it goes on tracing at the size it was asked for, and simply
        // does not present.
        //
        // THE SOURCE RECTANGLE is the part of the display texture the tone map
        // actually filled, which is not always all of it: the texture is
        // allocated at the produced size, and a frame that asked for
        // reconstruction and did not get it tone maps the smaller TRACED image
        // into that same surface. Blitting the whole thing would stretch the
        // fallback frame with a band of stale pixels down two of its edges.
        // -- everything DLSS-G needs, in the order it needs it ---------------
        //
        // THIS IS THE HALF THAT WAS MISSING. v2 asked for frame generation and
        // set the Reflex markers, and stopped there -- so DLSS-G was switched on
        // with no depth, no motion vectors and no matrices, and had nothing to
        // interpolate between. It reported itself available the whole time.
        segEnd(kSegRecord);
        sl_.markRenderSubmitStart();
        // ...OR ASKED FOR: the settings dropdown can turn it on from Off, and
        // the block below is where that request is applied (see slFgAsked_).
        if (sl_.frameGeneration() != FrameGen::Off ||
            (opt_.frameGen != FrameGen::Off && sl_.hasFrameGeneration())) {
            const Falcor::uint2 renderDim{uint32_t(tracer_.width()), uint32_t(tracer_.height())};
            const Falcor::uint2 outDim{uint32_t(tracer_.displayWidth()),
                                       uint32_t(tracer_.displayHeight())};
            // Tag first: it records the motion-vector extent the constants are
            // then scaled against.
            sl_.tagResources(ctx, tracer_.display().get(), tracer_.guideDepth().get(),
                             tracer_.guideMotion().get(), renderDim, outDim);
            // SAME SIGN FLIP AS RAY RECONSTRUCTION. DLSS defines jitter the way
            // a rasteriser applies it; this tracer builds the ray for
            // pixel + jitter, which moves the image the other way. Getting it
            // wrong does not look broken -- it just never quite resolves.
            const Vec2 j = tracer_.lastJitter();
            sl_.setFrameConstants(&gcam.pos.x, &gcam.u.x, &gcam.v.x, &gcam.w.x, gcam.halfW,
                                  gcam.halfH, -j.x, -j.y, slReset_);

            // RE-DECLARED ONLY WHEN THE SIZE CHANGES. Streamline warns
            // "Repeated slDLSSGSetOptions() call for the frame N -- a redundant
            // call or a race condition with Present()", and it means it: this
            // call races the present thread, so issuing it every frame is not
            // merely wasteful.
            //
            // ...OR WHEN THE PLAYER PICKS ANOTHER MODE in the settings. Applied
            // here, on the frame, rather than from the dropdown: this is the
            // point in the frame the call was already known to be safe at.
            // slFgAsked_ records the REQUEST, not the result, so a mode the
            // driver refuses is asked for once and not every frame.
            if (slFgDim_.x != renderDim.x || slFgDim_.y != renderDim.y ||
                slFgOut_.x != outDim.x || slFgOut_.y != outDim.y ||
                slFgAsked_ != opt_.frameGen) {
                slFgDim_ = renderDim;
                slFgOut_ = outDim;
                slFgAsked_ = opt_.frameGen;
                sl_.setFrameGeneration(opt_.frameGen, renderDim, outDim);
            }
            slReset_ = false;
        }

        if (target->getWidth() > 0 && target->getHeight() > 0) {
            FALCOR_PROFILE(ctx, "blit+hud");
            ctx->blit(tracer_.display()->getSRV(), target->getRenderTargetView(0),
                      Falcor::uint4(0, 0, uint32_t(tracer_.displayWidth()),
                                    uint32_t(tracer_.displayHeight())));
            // NOT IN CINEMA -- "all ui dissapears". The game-over curtain stays:
            // it is not interface, it is the game saying you died.
            if (!cinema_) {
                drawCrosshair(ctx, target);
                // AFTER the crosshair, because a dying player should not be
                // squinting past a mark, and the curtain is meant to cover it.
                drawVitals(ctx, target);
            }
            drawGameOver(ctx, target);
        }
        segEnd(kSegPresent);

        // -- the recorder ----------------------------------------------------
        //
        // FROM THE DISPLAY TEXTURE, NOT FROM THE WINDOW, and that is what keeps
        // the interface out of the recording. Everything drawn after this point
        // -- the crosshair, the fps readout, the settings panel, the REC badge
        // itself -- goes into `target`, which the recorder never reads. The
        // WebGPU game solved the same problem by making its banner a DOM
        // element; here the clean feed already exists and is simply the one
        // that gets recorded.
        //
        // It is also the tone-mapped frame at the PRODUCED resolution, so a
        // take is unaffected by the window being resized or by the blit.
        {
            const double nowSec = std::chrono::duration<double>(now.time_since_epoch()).count();
            recorder_.tick(ctx, tracer_.display(), tracer_.displayWidth(),
                           tracer_.displayHeight(), nowSec);
            vb::Take take;
            if (recorder_.poll(take)) onTakeSaved(take, nowSec);
        }

        // -- the scripted take ------------------------------------------------
        //
        // STOPPED ON THE RECORDER'S OWN CLOCK, not on a frame count and not on
        // the wall clock. elapsed() is the slot clock, so "--rec 5" is five
        // seconds of VIDEO however many frames the engine managed to render
        // underneath it -- which is the only definition that makes two runs on
        // two machines comparable.
        if (opt_.recSeconds > 0.0f) {
            if (!recStarted_ && tracer_.displayWidth() > 0) {
                recStarted_ = true;
                toggleRecording();
            } else if (recorder_.recording() &&
                       recorder_.elapsed() >= double(opt_.recSeconds)) {
                recorder_.stop();
            } else if (recStarted_ && !recorder_.busy() && !recorder_.recording()) {
                askShutdown(0);
                return;
            }
        }

        moving_ = false;  // cleared only once the frame it applied to is drawn
        drainInfoQueue();
        segEnd(kSegTail);
        ++segFrames_;

        // -- the scripted capture, if one was asked for -----------------------
        // --profile shares the frame counter, so a run can be measured with or
        // without a png falling out of it.
        // -- ...AND THE FRAME IS BOOKED (see HitchFrame) -------------------
        // -- A LONG FRAME WHILE SOMETHING IS COMING APART, NAMED ----------
        //
        // (user 2026-09-22: "its still freezing as well before it turns into
        //  chunks", after three rounds of headless measurement found nothing
        //  freeze-sized.)
        //
        // NOTHING HEADLESS CAN SEE THIS. --fell-test measures the drain at
        // 0.84 ms a frame over 55 frames and the solver at one step, and both
        // of those are already too thin to feel -- so whatever is being
        // reported happens on the render path, which that test does not run.
        // The frame loop is the only place standing on it.
        //
        // ALWAYS ON, not behind --hitch. A player who fells a tree and feels a
        // freeze is not going to be running with a diagnostic flag, and this is
        // the one event where a frame over 30 ms has a single obvious suspect
        // list. Rate limited to six so a bad session prints six lines, not six
        // hundred, and gated on shatterBusy so an ordinary streaming hitch --
        // which is a different problem with its own instrument -- says nothing.
        const double frameMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hitchT0)
                .count();
        if (frameMs > kShatterLoudMs && world_.shatterBusy() && shatterLoudLeft_ > 0) {
            --shatterLoudLeft_;
            const World::Profile lp = world_.profile();
            std::printf("v2: FRAME %.1f ms while a body was coming apart  "
                        "(stream %.1f  drain %.1f  blas %.1f  tlas %.1f  phys %.1f  life %.1f  "
                        "publish %.1f)\n",
                        frameMs, streamMs, lp.drainMs - shatterLoudWas_.drainMs,
                        lp.blasMs - shatterLoudWas_.blasMs, lp.tlasMs - shatterLoudWas_.tlasMs,
                        hPhys_, hLife_, hPub_);
            std::fflush(stdout);
        }
        shatterLoudWas_ = world_.profile();

        if (opt_.hitch) {
            const World::Profile wp = world_.profile();
            HitchFrame h;
            h.total = float(frameMs);
            h.stream = float(streamMs);
            h.blas = float(wp.blasMs - hitchWas_.blasMs);
            h.tlas = float(wp.tlasMs - hitchWas_.tlasMs);
            h.pool = float(wp.poolMs - hitchWas_.poolMs);
            h.rering = float(wp.reringMs - hitchWas_.reringMs);
            h.drain = float(wp.drainMs - hitchWas_.drainMs);
            h.take = float(wp.takeMs - hitchWas_.takeMs);
            h.cread = float(wp.compactReadMs - hitchWas_.compactReadMs);
            // Clamped: the shatter report zeroes these clocks when it prints.
            h.bAcq = float(maxf(0.0f, float(wp.bAcq - hitchWas_.bAcq)));
            h.bUpd = float(maxf(0.0f, float(wp.bUpd - hitchWas_.bUpd)));
            h.bPre = float(maxf(0.0f, float(wp.bPre - hitchWas_.bPre)));
            h.bRes = float(maxf(0.0f, float(wp.bRes - hitchWas_.bRes)));
            h.bCre = float(maxf(0.0f, float(wp.bCre - hitchWas_.bCre)));
            h.adopted = int(wp.adopted - hitchWas_.adopted);
            h.phys = float(hPhys_);
            h.life = float(hLife_);
            h.pub = float(hPub_);
            hitchWas_ = wp;
            hitch_.push_back(h);
        }
        tickFellLive(ctx, frameMs, streamMs);   // --fell-live only; see app_tests_interaction.inl
        tickEditLive(ctx);                      // V2_EDIT_LIVE only; same file
        tickCinemaTest(ctx);                    // V2_CINEMA_TEST only; ui/app_cinema.inl
        if (!opt_.shotPath.empty() || opt_.profile || !opt_.shotUi.empty()) {
            ++shotFrames_;
            // --shot-ui does its own capture at the TOP of a frame, so it must
            // not be shut down from here before it gets there.
            if (shotFrames_ >= opt_.shotFrame && opt_.shotUi.empty()) {
                // -- WHAT WAS STANDING THERE WHEN THE SHUTTER WENT ---------
                //
                // (user 2026-09-20, the fifth "skunks everywhere": the census
                //  says the populations are even and the player says they are
                //  not, so the untested link is the one between the two --
                //  whether what the SIM holds is what the SCREEN shows.)
                //
                // A capture is the player's own instrument and until now it
                // was the one thing with no numbers attached. This prints the
                // live roster the frame the shot is taken, with the yaw that
                // would centre each animal -- so a follow-up run at the same
                // seed and the same position, changing only --yaw, is aimed
                // at a named individual rather than swept blind. The position
                // is what makes that work: the fill is a function of where
                // the PLAYER is, so turning the camera does not move anybody.
                if (std::getenv("V2_SHOT_LIFE") != nullptr) {
                    std::vector<LifeAt> who;
                    critters_.livePoints(&who);
                    bees_.livePoints(&who);
                    flock_.livePoints(&who);
                    bunnies_.livePoints(&who);
                    flock2_.livePoints(&who);
                    birds_.livePoints(&who);
                    lake_.livePoints(&who);
                    const Vec3 eye = player_.pos;
                    std::printf("\n  -- live at the shutter, from (%.1f, %.1f) --\n", eye.x,
                                eye.z);
                    std::printf("  %-11s %8s %6s   %s\n", "species", "away", "yaw", "at");
                    for (const LifeAt &a : who) {
                        const float dx = a.p.x - eye.x, dz = a.p.z - eye.z;
                        const float d = std::sqrt(dx * dx + dz * dz);
                        if (d > 120.0f) continue;
                        // THE ENGINE'S OWN CONVENTION, COPIED FROM THE ONE
                        // PLACE THAT ALREADY COMPUTES IT -- the spawn's "facing
                        // it" line in app_spawn.inl, which is `atan2(wx, -wz)`.
                        // THE MINUS ON z IS THE WHOLE THING: yaw 0 looks down
                        // -z, not +z. Written as atan2(dx, dz) the bearing
                        // comes out reflected, which aims the camera at the
                        // mirror image of the animal -- 70 degrees off for the
                        // first one tried here -- and an empty frame reads
                        // exactly like a model that is never drawn. A bearing
                        // is only ever as good as the convention it is printed
                        // in; take it from the code that already turns.
                        const float yaw = std::atan2(dx, -dz) * 57.2957795f;
                        std::printf("  %-11s %7.1fm %6.0f   (%.0f, %.0f)\n", a.what, d, yaw,
                                    a.p.x, a.p.z);
                    }
                    std::printf("\n");
                }
                if (!opt_.shotPath.empty() && tracer_.writePng(ctx, opt_.shotPath))
                    // The SIZE is printed because a capture comes out at the
                    // size the game was set to produce, not at the window's --
                    // --scale is a resolution, and the resolution of a
                    // reference shot is the one thing that must never have to
                    // be guessed at from the file.
                    std::printf("v2: wrote %s at %dx%d after %d frames (%s)\n",
                                opt_.shotPath.c_str(), tracer_.displayWidth(),
                                tracer_.displayHeight(), shotFrames_,
                                tracer_.denoising() ? "reconstructed" : "accumulated");
                else if (!opt_.shotPath.empty())
                    std::fprintf(stderr, "v2: could not write %s\n", opt_.shotPath.c_str());
                if (opt_.profile) printProfile();
                if (opt_.hitch) printHitch();
                // The march writes its own picture, so it saves its own file
                // beside the traced one -- the two are the same camera and the
                // same frame, which is what makes them comparable.
                // A SCRIPTED DROP SAYS WHERE IT ENDED UP, not just where it was
                // thrown from. The toss print above is half a measurement: what
                // the floor under a dropped item is worth cannot be read off a
                // screenshot, because the grass stands taller than the gap.
                if (birds_.ready()) {
                    std::printf("v2: flyer band %d slots, %d models\n",
                                world_.flyerBandSlots(), world_.flyerModelCount());
                    std::printf("v2: %d songbirds perched\n", birds_.count());
                    Vec3 bp;
                    for (int k = 0; k < 12 && birds_.nth(k, &bp); ++k) {
                        const Vec3 to = bp - pos_;
                        const float len = maxf(0.001f, length(to));
                        // NO LINE-OF-SIGHT TEST HERE, and the one that was is
                        // worth recording as a warning. swingRay looked like
                        // the right instrument -- it marches the terrain and
                        // the solids and knows nothing about flyers -- but it
                        // is the TOOL swing, and it stops at the tool.s reach.
                        // Every bird beyond a few metres therefore came back
                        // "nothing in the way", which is not a measurement of
                        // anything. It read as twelve clear sight lines and was
                        // twelve rays that never got there.
                        std::printf("v2:   bird %d at (%.2f, %.2f, %.2f)  %.1f m  "
                                    "yaw %+.1f pitch %+.1f\n",
                                    k, double(bp.x), double(bp.y), double(bp.z), double(len),
                                    double(atan2f(to.x, -to.z) * 180.0f / PI),
                                    double(asinf(to.y / len) * 180.0f / PI));
                    }
                }
                if (opt_.dropFrame >= 0) {
                    float clear = 0.0f;
                    Vec3 at{0, 0, 0};
                    for (int i = 0; i < kDropSlots; ++i)
                        if (drops_.clearance(i, &clear) && drops_.position(i, &at))
                            std::printf("v2: drop %d clears %.3f m (%.1f voxels), %.2f m from you "
                                        "(reach %.1f)\n",
                                        i, double(clear), double(clear / VOXEL_M),
                                        double(sqrtf((at.x - player_.pos.x) * (at.x - player_.pos.x) +
                                                     (at.z - player_.pos.z) * (at.z - player_.pos.z))),
                                        double(kPickupM));
                }
                std::fflush(stdout);
                askShutdown(0);
            }
        }

        // WHETHER FRAME GENERATION IS ACTUALLY GENERATING, which is not the
        // same question as whether it was switched on. DLSS-G declines quietly
        // -- a resolution it dislikes, a missing tag, Reflex not running -- and
        // reports itself available throughout. framesPresented() is its own
        // count, and the only honest way to tell "on" from "on and working".
        //
        // Printed on the --stats cadence rather than per frame, because it is a
        // number to watch rather than to read.
        if (opt_.stats && opt_.frameGen != FrameGen::Off && fpsAccum_ == 0.0) {
            std::printf("  fg       %s -- %.0f rendered/s -> %.0f shown/s; %llu generated total; %s\n",
                        frameGenName(opt_.frameGen), fps_, fps_ + genFps_,
                        (unsigned long long)generatedTotal_, sl_.frameGenStatus().c_str());
            std::fflush(stdout);
        }

        // The frame rate the menu reports. Falcor tracks one of its own, but it
        // is smoothed over a different window and the number beside a setting
        // has to be the number that setting moved.
        // PRESENT IS NOT IN THIS FUNCTION, which is why these two are not
        // adjacent. SampleApp presents AFTER onFrameRender returns, so a
        // start/end pair written here in sequence would bracket nothing at all
        // and hand Reflex a present that took zero time -- which is worse than
        // no marker, because it is a confident wrong answer.
        //
        // So the pair straddles the boundary: end the PREVIOUS frame's present
        // at the top of this one (see newFrame above), and open this frame's
        // present here, as the last thing before returning into Falcor.
        // The present window Reflex paces against. DLSS-G inserts its generated
        // frame inside it, on the proxy swapchain.
        sl_.markRenderSubmitEnd();
        sl_.markPresentStart();

        fpsAccum_ += wallDt;
        ++fpsFrames_;
        if (fpsAccum_ >= 0.5) {
            fps_ = float(fpsFrames_ / maxf(1e-4f, float(fpsAccum_)));
            genFps_ = float(generatedThisSecond_ / maxf(1e-4f, float(fpsAccum_)));
            generatedThisSecond_ = 0;
            fpsAccum_ = 0.0;
            fpsFrames_ = 0;
            clock_.clock(clockText_, sizeof(clockText_));
            if (opt_.stats) {
                std::printf("  %.1f fps  %dx%d -> %dx%d  %s  %d bounces  %s %.1f m/s"
                            "  cam (%.1f %.1f %.1f)\n",
                            fps_, tracer_.width(), tracer_.height(),
                            tracer_.outWidth(), tracer_.outHeight(),
                            tracer_.denoising() ? dlssQualityName(opt_.dlssQuality) : "accumulate", liveDepth_,
                            player_.fly ? "fly" : (player_.onGround ? "ground" : "air"),
                            player_.speed(), pos_.x, pos_.y, pos_.z);
                // Redirected to a file, stdout is fully buffered -- so without
                // this a run that is killed rather than quit loses every line
                // it ever printed, which is precisely the automated case
                // --stats exists for.
                std::fflush(stdout);
            }
        }
    }

    #include "ui/app_gui.inl"
