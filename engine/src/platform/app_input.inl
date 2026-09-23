// app_input.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: keyboard and mouse events
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    bool onKeyEvent(const KeyboardEvent &e) override {
        if (e.type != KeyboardEvent::Type::KeyPressed) return false;
        // THE PICTURE IS ALREADY COLLAPSING. Swallow everything: the program is
        // a second from gone, and a key that opened a panel or teleported the
        // player now would only be drawn into the last few frames of a shot
        // nobody asked for.
        if (quitting_) return true;

        if (e.key == Input::Key::X) return true;  // held modifier for the wheel
        if (e.key == Input::Key::F) {
            player_.fly = !player_.fly;
            if (!player_.fly) player_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v2: %s\n", player_.fly ? "flying" : "walking");
            std::fflush(stdout);
            return true;
        }
        // -----------------------------------------------------------------
        // U -- THE ASSET EDITOR, AND U AGAIN TO COME BACK.
        //
        // A different PLACE, not a different mode of this one: on the stage the
        // wood is not in the acceleration structure at all, so what a ray finds
        // is the deck or the sky and nothing else. See World::setStage.
        //
        // THE WOOD IS LEFT EXACTLY AS IT WAS. Where you were standing, which
        // way you were looking, and every chunk that was resident -- all kept,
        // so U back is a rebuild and not a reload. That is what makes this
        // something you press to check a model rather than something you commit
        // to.
        //
        // FLYING, and it has to be: the player walks on the TERRAIN, and the
        // terrain function knows nothing about a deck floating at y 512 -- it
        // would answer with whatever hillside is at those coordinates and drop
        // you through the floor. Flight takes the ground out of the question.
        // -----------------------------------------------------------------
        // [I] IS THE ASSET EDITOR (user 2026-09-13: "Put the asset editor on
        // the i key"). U still works -- it is what every note in this file and
        // the help text already call it, and taking it away would invalidate
        // all of them to gain nothing. Two keys, one door.
        if ((e.key == Input::Key::U || e.key == Input::Key::I) && !consoleOpen_) {
            // -- THE TWO PLACES ARE EXCLUSIVE, AND THE WOOD IS BETWEEN THEM ---
            //
            // "Make sure that the esc menu and the asset editor are seperate
            // level. that they are not in the same world as the other levels."
            // They already were -- each REPLACES the world in the structure, and
            // they stand four kilometres apart -- but nothing stopped both being
            // OPEN at once, and the state that produced is worse than either: a
            // room drawn over a stage, with one saved wood position between them
            // that the second door to open would overwrite with the first
            // door's coordinates. Leaving for the wood first makes the save
            // correct by construction.
            if (pauseOpen_) setRoomOpen(false);
            // ...AND THE BUILDING IS A THIRD PLACE, held to the same rule. Two
            // levels open at once is the state this exclusivity was written to
            // prevent; the saved wood position is a single slot and whichever
            // door opened second would overwrite it with the first door's
            // coordinates. Leaving for the wood first makes the save correct by
            // construction, which is what the note above means.
            if (world_.levelOn()) leaveLevel();
            const bool on = !world_.staged();
            if (on) {
                woodPos_ = player_.pos;
                woodYaw_ = yaw_;
                woodPitch_ = pitch_;
                woodFly_ = player_.fly;
            }
            world_.setStage(on);
            if (on) {
                // Off the corner and looking at it, so the subject is in
                // front of you the moment you arrive rather than underfoot --
                // and so every handle on it can be dragged. See standOnDeck.
                standOnDeck();
                // ...and the subject, standing on the deck in front of you.
                stageSubject();
            } else {
                leaveStage();
            }
            player_.vy = 0.0f;  // no fall carried across the doorway
            pos_ = player_.eyePosition();
            tracer_.resetAccumulation();
            std::printf("v2: %s\n", on ? "asset editor" : "back to the wood");
            std::fflush(stdout);
            return true;
        }
        // -----------------------------------------------------------------
        // THE ASSET EDITOR'S OWN KEYS, and it gets first refusal on them.
        //
        // AFTER the door above, so [I] and [U] still let you out, and BEFORE
        // everything below, because that is what "the editor owns the keyboard
        // while it is up" means. It claims ten keys and passes every other
        // press straight through, so Y, O, ESC and the rest work on the deck
        // exactly as they do in the wood.
        //
        // THE ONE IT TAKES THAT SOMETHING ELSE WANTED IS [R]: it turns a frame
        // here rather than starting a recording. That is v1's binding and v1's
        // reason -- a key cannot mean two things at once in one mode -- and the
        // recorder gets it back the moment you step off the deck.
        // -----------------------------------------------------------------
        if (world_.staged() && !consoleOpen_ && !menuOpen_ && !waterPanelOpen_ &&
            edit_.key(e)) {
            std::fflush(stdout);
            return true;
        }
        if (e.key == Input::Key::Y) {
            setMenuOpen(!menuOpen_);
            return true;
        }
        // -- [L] -- THE WATER PANEL, BACK ON THE KEY IT WAS TAKEN OFF -------
        //
        // (user 2026-09-18: "also give me all the water settings again on the
        // l key.")
        //
        // Its fourth binding in five days -- [I], then [O], then [L], then
        // none, now [L] again -- so the rule this file adopted the last time
        // still stands and is worth restating: A KEY IS NAMED IN ONE PLACE,
        // HERE. Not in the panel title, not in a comment somewhere else. The
        // title said "[I]" for a day after the panel had moved to [O], and
        // that is what a second copy of a binding is always eventually doing.
        //
        // It was removed on the grounds that a press could open it by accident
        // mid-walk. That is still true and it is the user's call to make, not
        // mine; `--water-ui` remains for opening it at start-up.
        // -----------------------------------------------------------------
        if (e.key == Input::Key::L && !consoleOpen_) {
            setWaterPanelOpen(!waterPanelOpen_);
            return true;
        }
        // [O] -- THE BUILDING LEVEL, AND O AGAIN TO COME BACK.
        //
        // The asset deck's door, with the deck's own reasoning ("A different
        // PLACE, not a different mode of this one") and the same exclusivity:
        // the pause panel and the editor both close first, so only one place is
        // ever open and the saved wood position can only have been written by
        // whichever door is actually open. See the note over [U].
        //
        // NOT FLYING, and that is the difference from the deck. The editor has
        // to fly because the walk reads the TERRAIN and there is no terrain at
        // 640 m -- but this level brought its own ground with it, and a
        // building you float through is not a building. Solid::interior is
        // what makes the walk read the level's voxels instead; see collide.h.
        // -----------------------------------------------------------------
        if (e.key == Input::Key::O && !consoleOpen_) {
            if (pauseOpen_) setRoomOpen(false);
            if (world_.staged()) leaveStage();
            const bool on = !world_.levelOn();
            if (on) {
                woodPos_ = player_.pos;
                woodYaw_ = yaw_;
                woodPitch_ = pitch_;
                woodFly_ = player_.fly;
                if (!world_.setLevel(true)) {
                    std::fprintf(stderr, "v2: no level to travel to -- run "
                                         "tools/voxelize_arcade.py\n");
                    return true;
                }
                standInLevel();
            } else {
                leaveLevel();
            }
            player_.vy = 0.0f;   // no fall carried across the doorway
            pos_ = player_.eyePosition();
            tracer_.resetAccumulation();
            volfog_.invalidate();
            std::printf("v2: %s\n", on ? "arcade" : "back to the wood");
            std::fflush(stdout);
            return true;
        }
        // T OPENS THE CONSOLE, and only when it is shut -- while it is open the
        // key belongs to whatever is being typed, and ImGui has the keyboard.
        if (e.key == Input::Key::T && !consoleOpen_ && !menuOpen_) {
            setConsoleOpen(true);
            return true;
        }
        if (e.key == Input::Key::Escape) {
            // A PANEL FIRST, ALWAYS. ESC dismisses whatever is over the screen
            // before it starts down the ladder below -- otherwise closing a
            // console would also spend a rung of it, and the press that was
            // meant to put a panel away would be the press that gave the mouse
            // back as well.
            if (consoleOpen_) {
                setConsoleOpen(false);
                return true;
            }
            if (menuOpen_) {
                setMenuOpen(false);
                return true;
            }
            // ...THE WATER PANEL TOO, which it did not used to. That panel
            // hands the cursor back when it opens, so ESC over it fell past the
            // mouse rung and straight into the pause room -- leaving a panel up
            // over a room it has nothing to do with. It is a panel; ESC closes
            // panels.
            if (waterPanelOpen_) {
                setWaterPanelOpen(false);
                return true;
            }
            // -------------------------------------------------------------
            // ESC IS A LADDER OF THREE.
            //
            //     press 1   free the mouse          setCapture(false)
            //     press 2   the three buttons up    setRoomOpen(true)
            //     press 3   quit, CRT collapse      beginQuit()
            //
            // -- THE FIRST TWO WERE SWAPPED AND IT IS REVERTED ---------------
            //
            //    (user 2026-09-22: "have the first esc show the 3D ui menu,
            //     the second esc free the cursor", then "revert the recent esc
            //     ladder changes".)
            //
            //    The argument for menu-first was that a press which only hands
            //    the mouse back puts nothing on the screen, so it reads as a
            //    press that did nothing. That is true and it is not what the
            //    user wants: this order is the one that was asked for twice
            //    before (2026-09-14, 2026-09-20) and it is back.
            //
            // "Have 1 esc free the mouse, another esc to bring up the main menu
            // with the three balls, and one more esc to exit the game" (user
            // 2026-09-14). This is the shape the pause ROOM had, restored onto
            // the panel that replaced it -- the room is still gone; what came
            // back is the ladder.
            //
            // IT WAS A TOGGLE FOR A FEW HOURS, and the argument for that was
            // that a panel is not a place: three models standing in the wood
            // you never left have nowhere to travel to, so there was nothing
            // for the extra rungs to step through. That reasoning was about
            // the GEOMETRY and the ladder is about the KEYBOARD -- ESC is the
            // key that backs out of things, and each rung backs out of one more
            // than the last. Both readings are defensible; this one is the
            // user's, twice.
            //
            // THE PANEL IS TESTED ABOVE THE MOUSE RUNG, and it has to be:
            // setRoomOpen TAKES the cursor (the buttons are picked by the
            // crosshair), so a panel that fell through to the release below
            // would spend the third press handing the mouse back and never
            // reach the door.
            //
            // A PRESS THAT FREES NOTHING IS NOT SPENT. With the pointer already
            // loose -- a fresh launch nobody has clicked into, or a panel that
            // gave it away -- the first press goes straight to the buttons. The
            // rung only exists while there is something on it.
            //
            // THE RED BUTTON IS STILL THE OTHER WAY OUT, and it runs the same
            // beginQuit this third press does, so the two exits cannot differ.
            // ESC does not close the panel: that is what the green button is
            // for, and it is what makes this press an exit rather than a
            // second opinion.
            // -------------------------------------------------------------
            // -- ...AND THE LADDER IS COUNTED NOW, NOT INFERRED --------------
            //
            // (user 2026-09-20: "when I do double esc, it crashed my game.")
            //
            // IT DID NOT CRASH. It QUIT -- the long black freeze is beginQuit's
            // CRT collapse and the Falcor teardown behind it, doing exactly
            // what the red button does.
            //
            // TWO PRESSES REACHED IT, because the rungs were read off the STATE
            // rather than counted. With the pointer already loose -- a fresh
            // launch, or any panel that handed the cursor back -- the mouse
            // rung does not exist, so press 1 opened the panel and press 2 fell
            // straight through to the quit.
            //
            // THAT IS THE "a press that frees nothing is not spent" RULE, and
            // it is what is being reversed here. It is defensible on its own --
            // a rung with nothing on it is not a rung -- but it makes the
            // number of presses to EXIT depend on where the mouse happened to
            // be, and the ask it serves is a fixed one: "1 esc free the mouse,
            // another esc to bring up the main menu with the three balls, and
            // one more esc to exit the game". Three, from anywhere. Counting is
            // the only way that is true.
            //
            // The first press still frees the mouse when there is one to free;
            // when there is not, it is SPENT rather than skipped, and that is
            // the whole of the change. escRung_ resets wherever the ladder is
            // stepped off -- see setCapture and setRoomOpen.
            if (escRung_ == 0) {
                escRung_ = 1;
                if (looking_) setCapture(false);
                return true;
            }
            if (escRung_ == 1) {
                // AFTER setRoomOpen, NOT BEFORE. Opening the panel TAKES the
                // cursor (the buttons are picked by the crosshair), so it calls
                // setCapture(true) -- which is where the ladder is reset to 0.
                // Setting the rung first meant rung 2 wiped itself and the third
                // press started the ladder over at "free the mouse" instead of
                // quitting: "on the third esc it goes back to the first esc".
                setRoomOpen(true);
                escRung_ = 2;
                return true;
            }
            beginQuit();
            return true;
        }

        if (e.key == Input::Key::Minus) opt_.r.exposure = maxf(0.05f, opt_.r.exposure * 0.8f);
        if (e.key == Input::Key::Equal) opt_.r.exposure = minf(40.0f, opt_.r.exposure * 1.25f);
        if (e.key == Input::Key::LeftBracket) {
            opt_.r.maxDepth = maxi(1, opt_.r.maxDepth - 1);
            std::printf("v2: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        if (e.key == Input::Key::RightBracket) {
            opt_.r.maxDepth = mini(32, opt_.r.maxDepth + 1);
            std::printf("v2: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        // -- Q PUTS IT DOWN -------------------------------------------------
        //
        // ON THE KEY EVENT AND NOT ON THE POLLED STATE, unlike the swing: a
        // drop is one action per press, and polling would empty the whole kit
        // in three frames of holding the key.
        //
        // IT LEAVES FROM THE HAND. lastHeld_ is where the item actually was
        // last frame -- after the swing, the bob and the sway -- so the thing
        // that flies is the thing you were looking at, which is the JS engine's
        // own rule for this ("launch from the held item's true world spot ...
        // it FLIES out of the hand").
        if (e.key == Input::Key::Q && held_.ready() && held_.shown && held_.carrying() &&
            looking_ && !menuOpen_) {
            const Vec3 dir = forward();
            const Vec3 from = pos_ + camRight() * lastHeld_.cam.x + camUp() * lastHeld_.cam.y +
                              dir * lastHeld_.cam.z;
            const int sel = held_.selected();
            const Tool &t = held_.tool(sel);
            const int model = held_.model();
            if (held_.dropSelected() >= 0) {
                drops_.toss(sel, model, t.sx, t.sy, t.sz, from, dir);
                std::printf("v2: dropped %s\n", t.name);
                std::fflush(stdout);
                tracer_.resetAccumulation();
            }
            return true;
        }
        // -- G, THE WOOD AS IT WAS GENERATED -------------------------------
        //
        // (user 2026-09-17: "let me press q to refresh the game". Q already
        //  throws the held item out of your hand, so this is G and the throw
        //  is untouched.)
        //
        // Every pit filled in, every tilled bed turned back, every animal
        // re-scattered -- without the four minutes a relaunch costs. What it
        // does NOT do is re-read anything from disk: the terrain is compiled
        // in, so a changed constant still wants a build. This gives back the
        // world without the digging, which is what a refresh is for.
        //
        // THE LIFE IS DESPAWNED AND REPUBLISHED, not just despawned.
        // despawnAll clears the POPULATION; only a publish clears the BAND --
        // stageSubject learned that the hard way and its note says so. Miss
        // the publish and the slots keep drawing whatever was in them.
        //
        // BLOCKING, deliberately. A reload is a thing you ASKED for and then
        // watch; streaming it in over the next few seconds would look like the
        // wood dissolving rather than like a refresh.
        // -- [G] IS THE BIOME HOP NOW, AND CTRL+G IS THE REFRESH ----------
        //
        // (user 2026-09-19: "have it where the keybind g respawns the player to
        //  a new biome. if the player keeps pressing g to respawn, it cycles
        //  through the biomes.")
        //
        // THE KEY WAS TAKEN, and this is the split [R] already made for the
        // same reason: the ask names a key, the key does something else, so the
        // modifier decides which one this is. The refresh is a developer's
        // tool -- rebuild a constant, press it, see the wood without your
        // digging -- and it keeps every use it had, one chord further away.
        if (e.key == Input::Key::G && !consoleOpen_ && !menuOpen_) {
            if (e.hasModifier(Input::Modifier::Ctrl)) refreshWorld();
            // -- ...AND IN THE LEVEL IT RESETS THE MAP (user 2026-09-21) ---
            //
            // "when pressing g on the nuketown/fps mode. have it reset the
            //  map/level".
            //
            // ONE PROMISE, TWO PLACES. [G] has always meant "put it back the
            // way you found it" -- the wood drops its edits and re-scatters
            // its life. A biome cycle is meaningless on a slab 640 m above the
            // forest, so the key spent its whole time in here doing nothing
            // anybody wanted. See App::resetLevel.
            else if (world_.levelOn()) {
                respawnUsed_ = true;
                resetLevel();
            }
            else {
                // FOUND WITHOUT BEING TOLD, so the hint that would have taught
                // it ten seconds in never runs. See App::drawNotice.
                respawnUsed_ = true;
                respawnToNextBiome();
            }
            return true;
        }
        if (e.key == Input::Key::H && held_.ready()) {
            // An empty hand, and back again. The JS engine reaches the same
            // state by scrolling to an empty hotbar slot; there is no hotbar
            // here yet, so it is a key -- and it is worth having whatever
            // happens next, because comparing a shot with the tool and without
            // it is the first thing anyone does after adding one.
            held_.shown = !held_.shown;
            std::printf("v2: hand %s\n", held_.shown ? held_.name() : "empty");
            std::fflush(stdout);
        }
        // -- [R] RELOADS THE GUN, AND R IS ALREADY THE RECORDER --------------
        //
        // (user 2026-09-18: "reload with r.")
        //
        // THE KEY WAS TAKEN and the ask is explicit, so the tool in the hand
        // decides which R this is: with the rifle up it is a reload, and it is
        // the recorder every other moment of the game -- which is every moment
        // outside nuketown, because the gun exists nowhere else.
        //
        // CTRL+R IS ALWAYS THE RECORDER, so the one case the split would
        // otherwise cost -- recording a firefight -- is still one keypress.
        // Said here rather than in a help line nobody reads: a key that
        // silently changed meaning is the complaint this is trying not to
        // cause.
        if (e.key == Input::Key::R && !consoleOpen_ && !menuOpen_ &&
            !e.hasModifier(Input::Modifier::Ctrl) && holdingGun()) {
            const bool started = reloadGun();
            // ...AND SAY WHERE THE RECORDER WENT, ONCE. This keypress is the
            // exact moment the player believes recording is broken: they
            // pressed the record key and the gun reloaded. Answering it here
            // is the difference between a key that moved and a key that
            // vanished. Once per session -- the second time it would be a
            // nag, and by then it has been read or it never will be.
            if (!recHintSpent_) {
                recHintSpent_ = true;
                recHintAt_ = nowSeconds();
            }
            std::printf("v2: reload%s\n",
                        started ? "ing"
                                : (held_.reloading() ? " -- already reloading"
                                                     : " -- the magazine is full"));
            std::fflush(stdout);
            return true;
        }
        if (e.key == Input::Key::R) toggleRecording();
        // -- [F9] RECORDS, AND NO MODE CAN TAKE IT --------------------------
        //
        // (user 2026-09-21: "the recording function doesnt work in the fps
        //  mode. make it work please.")
        //
        // IT DID WORK -- on ctrl+R, which --rec-test confirms reaches the
        // recorder with the rifle up. That is the wrong answer to the report
        // anyway: a chord is not reachable with the left hand on WASD and the
        // right on the mouse, and a key you have to be TOLD about is a key
        // that does not work for the person who was not told.
        //
        // So the recorder gets one of its own. F9 is what a recorder is bound
        // to nearly everywhere, it is nowhere near the movement hand, and it
        // is free on both sides of the fence -- this engine uses F1 and F3,
        // Falcor's own SampleApp takes F2, F5 and F12. R and ctrl+R keep every
        // meaning they had; this only adds a way in that no level, no weapon
        // and no future binding can take away.
        if (e.key == Input::Key::F9 && !consoleOpen_) {
            toggleRecording();
            return true;
        }
        // [K] -- THE STACK BADGE'S FOUR NUMBERS. Free at the time of writing
        // and next to nothing else; see the panel for what it holds. The water
        // panel deliberately has NO key any more ("a key that keeps moving is
        // worse than no key"), and the difference is that this one was asked to
        // be reachable: "let me adjust the display number".
        if (e.key == Input::Key::K && !consoleOpen_) {
            stackPanelOpen_ = !stackPanelOpen_;
            // THE SIGHTS COME BACK DOWN WITH THE PANEL. The tick box that holds
            // them up is drawn on that card and nowhere else, so leaving it set
            // would weld the gun to the eye with no visible control to clear
            // it. See HeldItem::adsHold.
            if (!stackPanelOpen_) held_.adsHold = false;
            std::printf("v2: stack badge panel %s\n",
                        stackPanelOpen_ ? "open" : "closed");
        }
        // -- CTRL+C BAKES THE LAMPS ----------------------------------------
        //
        // (user 2026-09-17: "let me type ctrl + c to copy the new bulb
        // positions and ctrl + v to paste them into the code editor".)
        //
        // AS THE SOURCE LINES THEY LIVE ON, ready to paste between the braces
        // of World::kLevelBulbSeed. Not a config file and not a save: this
        // engine bakes by pasting -- the asset editor's [C] does it with a strip
        // table and the pose card does it with a HeldPose -- because a tuned
        // number that only exists in a running process dies with it.
        //
        // TO THE CONSOLE AS WELL, ALWAYS. assetedit.h's note is the reason:
        // OpenClipboard fails outright while another program holds it, and a
        // copy that silently did nothing is worse than no copy.
        if (e.key == Input::Key::C && e.hasModifier(Input::Modifier::Ctrl) && !consoleOpen_ &&
            world_.levelOn()) {
            std::string out;
            for (const Vec3 &b : world_.levelBulbs())
                out += fmt("            { %.2ff, %.2ff, %.2ff },\n", b.x, b.y, b.z);
            if (out.empty()) out = "            // (none placed)\n";
            std::printf("v2: %zu bulb(s) -- paste into World::levelBulbSeed()\n%s",
                        world_.levelBulbs().size(), out.c_str());
            std::fflush(stdout);
            ImGui::SetClipboardText(out.c_str());
            return true;
        }
        // -- NO SCREENSHOT KEY -------------------------------------------
        //
        // (user 2026-09-22: "remove the 6 keybind completely. no screenshots.")
        //
        // 6 AND P ARE BOTH GONE, and so is the readback behind them. It was put
        // on 6 an hour earlier, crashed the game on the first press, and could
        // not be reproduced here in four separate ways -- so what is removed is
        // a feature that was not working and had no diagnosis. --shot still
        // exists and is untouched: that is a command-line capture the test
        // harness drives, it quits on the next line, and it has never been the
        // thing that broke.
        // WHERE YOU ARE -- see showCoords_. Free of the console and the menu
        // for the reason every other bare key here is: a letter typed into the
        // console must reach the console.
        if (e.key == Input::Key::F3 && !consoleOpen_) showCoords_ = !showCoords_;
        if (e.key == Input::Key::F1) printHelp();
        std::fflush(stdout);
        return false;
    }

    // -----------------------------------------------------------------------
    bool onMouseEvent(const MouseEvent &e) override {
        if (quitting_) return true;   // see onKeyEvent
        // AN `if` WITH NO BODY BINDS TO THE NEXT STATEMENT, and the next
        // statement here is the whole wheel. There used to be a
        //
        //     if (e.type == MouseEvent::Type::ButtonDown) quitArmed_ = false;
        //
        // on this line. quitArmed_ went when ESC became a ladder of three, and
        // eight of the nine places that cleared it were statements of their own
        // and came out cleanly; this one was the tail of an `if`, and taking it
        // left the test behind. The wheel block below then ran only for an
        // event that was a ButtonDown AND a Wheel, which no event is -- so the
        // wheel did nothing at all, and there was no error and no warning,
        // because `if (a) if (b) {...}` is perfectly good C++.
        //
        // Reported as "I cant scroll to any other tool".
        if (e.type == MouseEvent::Type::Wheel) {
            // X IS A HELD MODIFIER, and it is POLLED rather than tracked from
            // key events: a stuck flag after an alt-tab that swallowed the key
            // release would silently turn every later scroll into a time change.
            // The input state reports the key's state now, and a window without
            // focus reports it released.
            if (getInputState().isKeyDown(Input::Key::X)) {
                clock_.nudgeSpeed(e.wheelDelta.y > 0.0f);
                char lbl[24];
                clock_.speedLabel(lbl, sizeof(lbl));
                std::printf("v2: day/night %s\n", lbl);
                std::fflush(stdout);
                return true;
            }
            // THE BARE WHEEL CHANGES TOOLS, which is what it does in the
            // engine this hand was ported from. It used to ZOOM, and every
            // stray scroll threw the accumulated film away and left the view at
            // some field of view nobody chose -- the note that said "the bare
            // wheel does nothing" was the fix for that, and the field of view
            // still belongs to the slider in the settings menu. This is not a
            // return to the zoom: it is the hotbar, and with nothing in the
            // hand it still does nothing.
            if (held_.ready() && !menuOpen_) {
                held_.cycle(e.wheelDelta.y > 0.0f ? 1 : -1);
                std::printf("v2: hand %s\n", held_.name());
                std::fflush(stdout);
            }
            return true;
        }

        // The mouse belongs to whichever panel is up.
        if (menuOpen_ || waterPanelOpen_) return false;

        // -- IN THE ROOM, A CLICK IS A BUTTON PRESS AND NOTHING ELSE --------
        //
        // Before the capture rule below, because in the room the FIRST click
        // has to work: there is nothing to look around at, and a pause menu
        // that ignores the first thing you do is a pause menu that feels
        // broken.
        if (pauseOpen_ && e.type == MouseEvent::Type::ButtonDown &&
            e.button == Input::MouseButton::Left) {
            // -- ...UNLESS THE POINTER IS NOT OURS YET, AND THAT EXCEPTION IS
            //    THE WHOLE OF THE DISCORD BUG (user 2026-09-14) -----------
            //
            // "if the player comes back from discord after clicking on it, it
            // cant click off the discord button."
            //
            // A LOOP, AND A TIGHT ONE. The blue button opens Discord;
            // releaseMouseOffFocus sees the window go to the back and hands the
            // pointer over, so `looking_` is false. Clicking the game window to
            // come back is a ButtonDown, this branch runs BEFORE the
            // click-to-capture rule below, and the crosshair has not moved --
            // it is still resting on the blue ball. So the click that was meant
            // to return to the game pressed Discord again, which took the focus
            // away again, for ever.
            //
            // The note above is still right about the ordinary case: ESC opens
            // the panel without touching the capture, so `looking_` is true and
            // the first click still works. What it did not cover is a first
            // click whose job is to get the mouse back, which is every click
            // that follows an alt-tab.
            if (!looking_) {
                setCapture(true);
                swingArmed_ = false;
                return true;
            }
            // -- THE CLICK STARTS THE PRESS; THE PRESS DOES THE THING -------
            //
            // "it presses the button down and then executes the action. make
            // the button press at the right timing when the player hits."
            //
            // So the action is not run here. What happens here is that the
            // button starts travelling, and tickButtons fires the action on the
            // frame it BOTTOMS OUT -- which is the moment a real button closes
            // its contact. Doing it on the click instead means the room shuts,
            // or the program exits, before the button has visibly moved: the
            // animation would exist but nobody would ever see it.
            //
            // ONE AT A TIME. A second click while a press is in flight is
            // ignored rather than queued -- two buttons going down together is
            // a thing a hand cannot do, and the second of them would be acting
            // on a room the first one had already left.
            const int b = buttonUnderCrosshair();
            if (b >= 0 && btnPend_ < 0) {
                btnHeld_ = b;
                btnPend_ = b;
                btnAt_ = btnClock_ + kBtnDownSec;
            }
            return true;
        }

        // -- ON THE DECK, A CLICK PICKS THE SUBJECT OR GRABS A HANDLE -------
        //
        // Before the capture rule below, exactly as the room's button press is
        // and for the same reason: the first click has to work. It is gated on
        // `looking_` all the same -- the ray is cast from the CROSSHAIR, and
        // with a loose cursor the crosshair is not where the pointer is, so a
        // click that had not taken the mouse yet would pick whatever happened
        // to be in the middle of the screen.
        if (world_.staged() && looking_ && e.button == Input::MouseButton::Left) {
            if (e.type == MouseEvent::Type::ButtonDown) {
                if (edit_.click(pos_, Camera::direction(yaw_, pitch_))) {
                    std::fflush(stdout);
                    return true;
                }
            } else if (e.type == MouseEvent::Type::ButtonUp && edit_.dragging()) {
                edit_.release();
                return true;
            }
        }
        if (e.type == MouseEvent::Type::ButtonDown && e.button == Input::MouseButton::Left) {
            // Click to capture, the way a game does it. ESC gives it back.
            //
            // THE CLICK THAT CAPTURES IS NOT A SWING. processInput polls the
            // button rather than latching it here (see the note there), so all
            // this has to do is disarm: the axe waits for the button to come up
            // once before it will swing. Swinging at the wood the instant a
            // window is clicked into focus is not what that click means.
            if (!looking_) {
                setCapture(true);
                swingArmed_ = false;
            }
            return true;
        }
        if (e.button == Input::MouseButton::Right) {
            // Hold-to-look, kept from the earlier engines so the habit carries
            // -- but ONLY as a way of taking the pointer in the first place.
            // Once it is ours the right button belongs to the hand: it is what
            // draws the bow, and a bow that let go of the mouse every time you
            // loosed an arrow would be unusable. The habit is untouched for
            // anyone who uses it, since it was always about grabbing the view
            // from a loose cursor.
            if (e.type == MouseEvent::Type::ButtonDown) {
                if (!looking_) {
                    holdLook_ = true;
                    setCapture(true);
                }
                // -- ...AND WITH SEEDS IN HAND IT PLANTS (user 2026-09-14) ---
                //
                // "have it where the player can right click tilled land with
                // seeds to place down seeds."
                //
                // AFTER THE CAPTURE, so the click that grabs a loose pointer is
                // not also a planting -- the same rule the left button's
                // click-to-capture follows, and for the same reason.
                //
                // BEFORE THE BOW, which is what the right button otherwise
                // means: plantSeed answers false unless seeds are in the hand
                // AND the crosshair is on turned earth, so the draw is
                // untouched by anything that is not both.
                // -- ...AND IT PICKS THE FRUIT ---------------------------
                //
                // (user 2026-09-17: "when the player right clicks on a apple or
                //  orange, the model appears in the right hand".)
                //
                // ON THIS BUTTON RATHER THAN ON THE SWING, which is where it
                // was and most of why it kept being reported as not working. A
                // pick is not a blow, and asking it as one made it answer at
                // the IMPACT FRAME of the left-click animation -- a quarter of
                // a second after the button went down, with the view already
                // carried by the swing, so the aim it judged was never the aim
                // the player took. It also hung a 0.9 m gate in front of every
                // axe stroke: an apple anywhere near the line was picked
                // instead of the trunk being chopped.
                //
                // BEFORE plantSeed AND BEFORE THE BOW, at no cost to either.
                // All three answer false unless their own thing is under the
                // crosshair, and a fruit, a patch of turned earth and a drawn
                // bow are three different things.
                else if (!pickFruit())
                    plantSeed();
            } else if (e.type == MouseEvent::Type::ButtonUp && holdLook_) {
                holdLook_ = false;
                setCapture(false);
            }
            return true;
        }
        return false;
    }

    #include "platform/app_window.inl"
    #include "world/app_ground.inl"
