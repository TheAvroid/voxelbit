// app_capture.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: mouse capture, look, and processInput
// -----------------------------------------------------------------------------
    void setWaterPanelOpen(bool on) {
        if (on == waterPanelOpen_) return;
        waterPanelOpen_ = on;
        if (on) {
            captureBeforeWater_ = looking_;
            if (looking_) setCapture(false);
            holdLook_ = false;
        } else if (captureBeforeWater_) {
            setCapture(true);
            captureBeforeWater_ = false;
        }
    }

    void setMenuOpen(bool on) {
        // Re-centre next time it is drawn. See menuPlaced_.
        if (on && !menuOpen_) menuPlaced_ = false;
        if (on == menuOpen_) return;
        if (on) {
            captureBeforeMenu_ = looking_;
            if (looking_) setCapture(false);
            holdLook_ = false;
        } else if (captureBeforeMenu_) {
            setCapture(true);
            captureBeforeMenu_ = false;
        }
        menuOpen_ = on;
    }

    // -----------------------------------------------------------------------
    // Mouse capture, by hand.
    //
    // The one piece of the old viewer the framework does not replace. Falcor's
    // Window keeps its GLFW handle private and exposes no cursor mode, so the
    // capture is done through Win32 on the HWND it does expose: hide the
    // cursor, and warp it back to the centre after every move so it can never
    // reach an edge. The delta is measured against that centre, which is also
    // why there is no first-move special case here -- the cursor is always
    // already where the last warp put it.
    // -----------------------------------------------------------------------
    void setCapture(bool on) {
        // BACK IN THE WORLD IS BACK AT THE BOTTOM OF THE LADDER. Without
        // this, clicking in after two presses would leave the NEXT ESC on
        // the quit rung -- one press from the CRT collapse, with nothing
        // on screen to say so.
        if (on) escRung_ = 0;
        // A background instance never takes the cursor, whatever it is asked:
        // it exists to be measured, not driven, and the person at the keyboard
        // is using another window.
        if (opt_.background) on = false;
        if (on == looking_) return;
        looking_ = on;

        if (!getWindow()) return;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        if (on) {
            while (::ShowCursor(FALSE) >= 0) {}
            ::SetCapture(hwnd);
            centreCursor();
        } else {
            ::ReleaseCapture();
            while (::ShowCursor(TRUE) < 0) {}
        }
    }

    // -----------------------------------------------------------------------
    // THE POINTER GOES BACK THE MOMENT THIS IS NOT THE WINDOW IN FRONT.
    //
    // Reported 2026-09-13: "when the user goes to discord and the discord
    // screen pops up, the game still has my mouse."
    //
    // Nothing in the capture noticed the window had gone to the back. `looking_`
    // stayed true, so every frame afterwards applyMouseLook WARPED THE CURSOR
    // back to the middle of this window -- across whatever the user was trying
    // to click in -- and the cursor stayed hidden, because ShowCursor's count is
    // per PROCESS and does not care which window has focus. Two separate ways of
    // holding a mouse that is not ours, and an alt-tab breaks neither.
    //
    // GetForegroundWindow, NOT GetActiveWindow: active is per thread and stays
    // set on our own window while another process is in front, which is exactly
    // the state being fixed. GA_ROOTOWNER keeps a dialog of our own as ours.
    //
    // THIS IS ALSO WHAT MAKES THE BLUE BUTTON WORK. The pause room opens Discord
    // with ShellExecute and then goes on rendering; the room is the one place in
    // this program that TAKES the cursor rather than handing it back, so without
    // this the button summoned a window the mouse could not reach.
    //
    // holdLook_ goes with it: right-button hold-to-look ends on a button-up that
    // a window without focus is never sent, and a flag left set there is a view
    // that keeps turning after the hand has gone.
    //
    // The way back in is unchanged and is the one a game uses -- click the
    // window. See the ButtonDown handler, which re-takes the cursor and disarms
    // the swing, so the click that returns is not also an axe blow.
    // -----------------------------------------------------------------------
    void releaseMouseOffFocus() {
        if (!looking_ || !getWindow()) return;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        const HWND fg = ::GetForegroundWindow();
        const bool ours = fg && (fg == hwnd || ::GetAncestor(fg, GA_ROOTOWNER) == hwnd);
        if (ours && !::IsIconic(hwnd)) return;
        setCapture(false);
        holdLook_ = false;
    }

    void centreCursor() {
        if (!getWindow()) return;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        RECT rc{};
        if (!::GetClientRect(hwnd, &rc)) return;
        POINT c{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
        ::ClientToScreen(hwnd, &c);
        ::SetCursorPos(c.x, c.y);
    }

    // The mouse look, polled rather than driven from the move event: warping
    // the cursor generates a move event of its own, and acting on those spins
    // the camera by exactly the amount the warp undid.
    // POLLED ONCE A FRAME, not driven by mouse events -- so dt here is the
    // frame time, and the drain at the bottom is frame-rate independent.
    //
    // It no longer returns early when the mouse has not moved: with weight on
    // the view there is still a turn to finish after the hand has stopped, and
    // an early return would freeze it mid-glide.
    // -----------------------------------------------------------------------
    // Mouse look. Pixels off the centre, times degrees per pixel, this frame.
    //
    // RAW, AND THAT IS THE FEATURE (user, 2026-09-06). What was here measured
    // the pointer's SPEED off the event clock, ran it through a gain curve, put
    // the result into a buffer of owed degrees, and drained a fixed fraction of
    // that buffer per frame. Every part of it was defensible on its own terms
    // and the sum of them was a camera that did not go where the hand put it:
    // the same wrist movement turned different amounts depending on how fast it
    // was made, and the turn carried on after the hand had stopped.
    //
    // WHAT IS LEFT IS THE WHOLE OF IT. Two multiplies. The pointer is warped
    // back to the centre of the client area every time it is read, so `rx`/`ry`
    // are the pixels moved since the last read -- there is no accumulator to
    // spend and nothing to tune but `sensitivity`.
    //
    // NOT SCALED BY THE FIELD OF VIEW, deliberately, and this is the one piece
    // of the old function worth keeping. A narrow field does make the same
    // wrist movement cover more of the frame -- that is what a narrow field IS,
    // and it is the reason a scope is harder to aim with than iron sights.
    // Compensating for it would defeat the one thing the setting is good for,
    // which is looking closely at something without also having to hold still.
    // -----------------------------------------------------------------------
    bool applyMouseLook() {
        if (!looking_) return false;

        float rx = 0.0f, ry = 0.0f;
        if (!getWindow()) return false;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        // -- A MINIMISED WINDOW HAS NO CENTRE TO MEASURE AGAINST -----------
        //
        // The look here is a DELTA FROM THE MIDDLE OF THE CLIENT AREA, and a
        // minimised window's client rect is 0 x 0. So the middle is (0, 0) in
        // client space, every cursor position on the desktop is hundreds of
        // pixels away from it, and the camera is turned by that difference on
        // every frame for as long as the window stays down.
        //
        // FOUND BY A CAPTURE THAT CAME BACK BLANK. --room --shot-ui renders
        // minimised (main.cpp minimises any automated capture, deliberately),
        // and the pause room is the one place in this program that TAKES the
        // cursor rather than handing it back -- so those two together spun the
        // camera to pitch -89 and photographed the floor. Three renders of a
        // blank white wall, and the room was never the thing that was wrong.
        //
        // IT IS NOT ONLY THE HARNESS. Anybody who minimises the game with the
        // mouse captured is in the same state, and gets their view thrown at
        // the floor while they are not looking at it.
        if (::IsIconic(hwnd)) return false;
        RECT rc{};
        POINT p{};
        if (::GetClientRect(hwnd, &rc) && ::GetCursorPos(&p)) {
            const int cx = (rc.right - rc.left) / 2, cy = (rc.bottom - rc.top) / 2;
            if (cx <= 0 || cy <= 0) return false;   // ...and a zero-size client, however caused
            ::ScreenToClient(hwnd, &p);
            rx = float(p.x - cx);
            ry = float(cy - p.y);
            if (rx != 0.0f || ry != 0.0f) centreCursor();
        }
        if (rx == 0.0f && ry == 0.0f) return false;

        // -- A GIZMO DRAG TAKES THE MOUSE OFF THE CAMERA --------------------
        //
        // Not a special case bolted on: it is what dragging a handle MEANS.
        // v1 returns out of its mousemove the same way, with the same one-line
        // reason ("suppress camera look while dragging"). Turning your head at
        // the same time as pulling an arrow would move the handle under the
        // pointer and make the drag chase itself.
        //
        // The delta this hands over is the raw pixel pair, +right and +UP --
        // `ry` is already measured that way just below, where v1's browser
        // movementY had to be negated.
        if (edit_.dragging()) {
            edit_.dragBy(rx, ry, yaw_, pitch_);
            return false;   // the view did not move, so nothing to re-accumulate
        }

        // Wrapped rather than left to grow: a long session spinning one way
        // otherwise walks yaw into the thousands, where a float's steps get
        // coarse enough to make the turn visibly notchy.
        yaw_ = fmodf(yaw_ + rx * opt_.sensitivity, 360.0f);
        if (yaw_ < 0.0f) yaw_ += 360.0f;
        pitch_ = clampf(pitch_ + ry * opt_.sensitivity, -89.0f, 89.0f);
        moving_ = true;
        return true;
    }

    // -----------------------------------------------------------------------
    // AN ARROW LEAVES THE BOW.
    //
    // The velocity is the JS engine's: ARROW_V is twice its thrown profile --
    // "a bow beats an arm, and the flatter arc is the point of it" -- and the
    // up-kick with it, both scaled by how far the bow was pulled. In its units
    // those are 480 and 18 voxels a second; here they are metres, which is the
    // same numbers over ten.
    //
    // WHERE IT STARTS IS THE BOW, and getting that wrong is what made the
    // arrow appear to vanish and be replaced (user 2026-09-07: "the arrow
    // disappears when its being fired").
    //
    // The nocked arrow is off to the right and down, wherever the hand holds
    // the bow. This used to spawn the shaft straight down the VIEW instead --
    // dead centre -- so on release the arrow you were looking at blinked out
    // and a different one appeared somewhere else. Two arrows, visibly.
    //
    // The JS engine's launchThrown has the answer and its note is the whole
    // idea: the viewmodel sits too close to the lens to spawn a full-size shaft
    // at, so take the bow's OWN sideways and vertical offset and carry it out
    // along the view to where an arrow can be drawn. The launch point then lies
    // on the RAY FROM THE EYE THROUGH THE BOW, which is the line the nocked
    // arrow is already on -- so the shaft leaves exactly where the arrow was,
    // just further down the same line, and the swap is invisible.
    //
    // ...and it must still go WHERE YOU AIMED. Leaving from a point to the side
    // of the eye means firing straight down the view sends the shaft along a
    // parallel line that never crosses the crosshair, so it is aimed at a
    // distant point ON the sight line and converges onto it within a few
    // metres, the way a real bow sight does.
    // -----------------------------------------------------------------------
    // Returns whether a shaft actually left, which is what the whoosh hangs
    // off -- see the call site.
    bool loose(float draw) {
        const float k = clampf(draw, 0.0f, 1.0f);
        if (k <= 0.0f) return false;
        const Vec3 dir = forward();
        // The bow's own place in the frame, carried out along the view.
        // `carry` is what puts the launch point on the eye-through-bow ray:
        // scale the lateral offsets by however much further out kArrowLaunchM
        // is than the bow itself. Guarded, because a pose with the item at the
        // eye would divide by nothing. NOT called k -- that is the draw, a few
        // lines up, and it is what the shot is worth.
        const Vec3 ho = lastHeld_.cam;
        const float carry = kArrowLaunchM / maxf(0.02f, ho.z);
        const Vec3 from =
            pos_ + camRight() * (ho.x * carry) + camUp() * (ho.y * carry) + dir * kArrowLaunchM;
        const Vec3 aim = pos_ + dir * kArrowAimM;
        Vec3 v = aim - from;
        const float l = sqrtf(maxf(1e-8f, lengthSq(v)));
        v = v * (kArrowSpeed * k / l);
        v.y += kArrowUp * k;
        arrows_.launch(from, v);
        if (true) {
            std::printf("v2: arrow away  draw %.2f  %.1f m/s\n", double(k),
                        double(kArrowSpeed * k));
            std::fflush(stdout);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    bool processInput(float dt) {
        const Falcor::InputState &in = getInputState();
        // BEFORE THE LOOK. applyMouseLook is what warps the cursor, so asking
        // afterwards would still spend one frame with the pointer in our fist.
        releaseMouseOffFocus();
        bool turned = applyMouseLook();

        // The arrows scrub the CLOCK, not the sun directly: with a cycle
        // running, a manual elevation would be overwritten on the next frame
        // and the control would look broken.
        // ...AND NOT ON THE EDITOR'S DECK, where the four of them reorder and
        // nudge frames instead. v1's rule, in its own words: "the asset editor
        // owns these two keys while it is up". A key cannot mean two things at
        // once in one mode, and the sky over the stage is empty anyway.
        if (!menuOpen_ && !world_.staged()) {
            const float scrub = 1.5f * dt;  // hours per second held
            if (in.isKeyDown(Input::Key::Left)) clock_.scrubHours(-scrub);
            if (in.isKeyDown(Input::Key::Right)) clock_.scrubHours(scrub);
            if (in.isKeyDown(Input::Key::Up)) clock_.scrubHours(scrub * 6.0f);
            if (in.isKeyDown(Input::Key::Down)) clock_.scrubHours(-scrub * 6.0f);
        }

        const bool sprint =
            in.isKeyDown(Input::Key::LeftShift) || in.isKeyDown(Input::Key::RightShift);
        const bool jump = in.isKeyDown(Input::Key::Space);
        // -- CROUCH IS CAPS LOCK, AND IT IS HELD --------------------------
        //
        // Straight off the JS engine's DEFBINDS, which has read
        // `crouch: 'CapsLock'` since 2026-08-05 -- it went C, then left Alt,
        // then here, and ui/keybinds.js still carries the migration that drags
        // saved bindings forward off the two dead keys.
        //
        // POLLED AND NOT TOGGLED, which is the whole reason this reads
        // isKeyDown rather than living in onKeyEvent beside F. Caps Lock
        // LATCHES A LIGHT ON THE KEYBOARD and nothing can stop it -- that
        // engine's input.js says so in as many words next to its
        // preventDefault: "preventDefault cannot stop CAPS LOCK toggling the OS
        // state". So the lamp will disagree with the crouch, and the only way
        // to keep the CROUCH honest is to read the physical key rather than
        // anything derived from it. Hold it down and you are down.
        const bool crouch = in.isKeyDown(Input::Key::CapsLock) && !menuOpen_;
        // Q HAS MOVED TO DROP (user 2026-09-07), which is where the JS engine
        // has always had it -- its DEFBINDS name KeyQ as `drop`. Control alone
        // descends in fly mode now; it was always the other half of that pair
        // and is the binding every other engine uses for it.
        // ...AND IT DESCENDS IN FLIGHT TOO, which is why that engine's BINDNAMES
        // calls the row "crouch / fly down" rather than "crouch". Control keeps
        // the job it was given on 2026-09-07; this is a second way down, not a
        // replacement for it.
        const bool down = in.isKeyDown(Input::Key::LeftControl) || crouch;

        // WASD in the horizontal plane only -- looking up must not walk you
        // into the sky. The forward vector is flattened and renormalised rather
        // than used directly, or a steep pitch would shorten every stride.
        const Vec3 f = forward();
        Vec3 flat(f.x, 0.0f, f.z);
        flat = (lengthSq(flat) > 1e-6f) ? normalize(flat) : Vec3(0.0f, 0.0f, -1.0f);
        const Vec3 r = normalize(cross(flat, Vec3(0, 1, 0)));

        Vec3 move(0.0f, 0.0f, 0.0f);
        // A scripted walk drives the player exactly as W would, so the motion
        // vectors, the head bob and the collision are all the real ones.
        if (opt_.shotWalk) move += flat;
        if (!menuOpen_) {
            if (in.isKeyDown(Input::Key::W)) move += flat;
            if (in.isKeyDown(Input::Key::S)) move -= flat;
            if (in.isKeyDown(Input::Key::A)) move -= r;
            if (in.isKeyDown(Input::Key::D)) move += r;
        }
        if (lengthSq(move) > 1e-6f) move = normalize(move);

        // -- AND A DEAD BODY ANSWERS TO WASD AND NOTHING ELSE ------------
        //
        // (user 2026-09-21: "the player can still control the player with the
        //  wasd keys. only the wasds should work breifly before being
        //  respawned".)
        //
        // GATED HERE RATHER THAN IN Player::update, which is the difference
        // between "the keys do nothing" and "the keys are not read". Sprint,
        // jump, the descent and the crouch all have a vertical or a speed in
        // them and the rise owns the vertical now -- see Player::dead -- so
        // they are dropped at the door. `move` survives untouched, which is
        // the whole of what was asked for.
        const bool dead = player_.dead;
        const Vec3 before = player_.eyePosition();
        player_.update(walkWorld(), move, dead ? false : sprint, dead ? false : jump,
                       dead ? false : down, dead ? false : crouch, dt);
        clampToStage();
        clampToLevel();
        tickButtons(dt);
        pos_ = player_.eyePosition();

        // -- THE TWO BARS -------------------------------------------------
        //
        // (user 2026-09-19: "import the damage/hunger mechanics from v1 into
        //  v2".)
        //
        // AFTER the move and AFTER the two clamps, because the drain is
        // charged off the REAL position delta -- being shoved, sliding or
        // swimming all count honestly, and a clamp that pushed the body back
        // is ground it did not cover. Unconditionally, never inside a movement
        // branch: v1's own note says its tick once sat inside `if (P.fly)` and
        // silently stopped the moment fly mode engaged.
        // THE FPS MAP HAS NO HUNGER -- see Vitals::hunger. Set every frame off
        // the world's own state rather than latched at the door, so stepping
        // out of the map puts it back without anybody having to remember.
        vitals_.hunger = !world_.levelOn();
        vitals_.tick(dt, player_.pos, player_.onGround, player_.fly, player_.swimming(),
                     sprint, player_.submerged());
        // -- ...AND THE SPINES, WHICH ARE v1's -----------------------------
        //
        // (user 2026-09-20: "have the cactus cause damage like in v1".)
        //
        // ONE POINT ON A COOLDOWN, AND THE FIRST ONE IS IMMEDIATE. v1's shape
        // exactly, including the part that is easy to get backwards: the timer
        // RESTS PRIMED rather than at zero, so brushing a saguaro bites on the
        // frame you touch it instead of costing nothing for the first 0.9 s.
        // Its own note argues that at length -- "this is a thing you brush
        // PAST, and it has to bite once" -- against the lava timer, which does
        // ramp and is deliberately not like this.
        //
        // NOT WHILE FLYING, which v1 also gates (`!P.fly`); Vitals::hurt
        // refuses everything in fly mode anyway, so this is belt and braces
        // and costs a compare.
        if (!player_.fly && touchingCactus()) {
            cactT_ += dt;
            if (cactT_ >= kCactusHurtSec) {
                cactT_ = 0.0f;
                vitals_.hurt(1, "the cactus spines got you");
            }
        } else {
            cactT_ = kCactusHurtSec;
        }
        drainVitals();

        // -- THE GRASS UNDERFOOT ------------------------------------------
        //
        // (user 2026-09-20: "can you add the footstep sounds from v1 into v2.
        //  its the grass footstep sound".)
        //
        // v1's arrangement exactly: ONE looping element whose gain says
        // whether you can hear it, rather than a sample fired per footfall.
        // Its own note is the argument -- "stopping dead on the frame the
        // player releases a key clicks, and starting dead drops you into the
        // middle of a footfall" -- and because the element keeps running while
        // faded out, starting to walk again rejoins the stride you left.
        //
        // MEASURED FROM THE GROUND COVERED, not from the keys: being shoved,
        // sliding down a bank or walking into a wall all read honestly, which
        // is the same rule the hunger drain uses two lines up.
        //
        // NOT ON SAND AND NOT IN THE AIR. It is the grass recording and the
        // desert has its own floor; `sandAt` is the question the life already
        // asks about the same ground, so the two cannot disagree about where
        // the sand starts.
        if (steps_.active()) {
            const Vec3 nowAt = player_.pos;
            const float dx = nowAt.x - stepLastX_, dz = nowAt.z - stepLastZ_;
            const float moved = std::sqrt(dx * dx + dz * dz);
            stepLastX_ = nowAt.x;
            stepLastZ_ = nowAt.z;
            const float speed = moved / maxf(dt, 1e-4f);
            const bool afoot = player_.onGround && !player_.fly && !player_.swimming() &&
                               speed > kStepMoveMs && moved < kVitTeleportM;
            const bool grass = !sandAt(nowAt.x, nowAt.z);
            // TWICE THE CADENCE WHEN SPRINTING -- v1's, and the seam of the
            // loop stays where it was because the cut is a property of the
            // audio rather than of the rate.
            steps_.setRate(speed > player_.walk * 1.35f ? 2.0f : 1.0f);
            steps_.update(dt, (afoot && grass) ? 1.0f : 0.0f);
        }

        // -- the swing -------------------------------------------------------
        //
        // POLLED, NOT LATCHED FROM THE EVENT, for the reason the X modifier on
        // the wheel is polled: a button-up swallowed by an alt-tab -- or by the
        // menu, which takes the mouse and returns before this file ever sees
        // the release -- would leave a flag set and the axe swinging by itself
        // for the rest of the session. The input state reports the button now,
        // and a window without focus reports it released.
        //
        // Holding it swings over and over. Each repeat re-arms the impact, so
        // the blow still lands 250 ms into whichever swing is running -- see
        // HeldItem::update.
        {
            const bool lmb = opt_.swingHold || in.isMouseButtonDown(Input::MouseButton::Left);
            if (!lmb) swingArmed_ = true;
            // -- A GUN DOES NOT SWING -------------------------------------
            //
            // (user 2026-09-17: "currently the gun hits when left clicking.
            // instead of that happening, have it shoot bullets".)
            //
            // THE TOOL DECIDES, NOT THE BUTTON. Everything else in the kit is
            // swung at what is in front of it, so the left button drives the
            // swing curve and the bite that HeldItem::update times off it. The
            // rifle takes the same button and spends it differently, and the
            // one thing that must not happen is BOTH -- a gun that also lands
            // an axe blow at 250 ms would carve whatever it is pointed at
            // twice, once from the swing and once from the round.
            // ONE DEFINITION, in app_actions.inl -- [R] asks the same question
            // and a second copy of it is a copy that drifts.
            //
            // EITHER GUN SUPPRESSES THE SWING; only the rifle pulls a trigger
            // below. See holdingGun for why those are two questions.
            const int gun = heldGun();
            const bool gunInHand = gun >= 0;
            // -- THE LAMP IN HAND EDITS THE MAP, IT DOES NOT SWING ----------
            //
            // (user 2026-09-17: "left click to remove the bulb and right click
            // to place a bulb".)
            //
            // ON THE EDGE, NOT WHILE HELD. The swing and the rifle both repeat
            // while the button is down, and both should: you keep chopping and
            // you keep firing. An EDIT must not -- holding the button for a
            // third of a second would hang a dozen lamps in a line, and each
            // one costs a full remesh of the level. `bulbArmed_` is the same
            // latch `swingArmed_` is, for the opposite reason.
            const bool bulbInHand = bulbTool_ >= 0 && held_.ready() &&
                                    held_.selected() == bulbTool_ && held_.carrying() &&
                                    world_.levelOn();
            if (bulbInHand && (looking_ || opt_.swingHold) && !menuOpen_) {
                const bool rmb = in.isMouseButtonDown(Input::MouseButton::Right);
                if (!lmb && !rmb) bulbArmed_ = true;
                if (bulbArmed_ && (lmb || rmb)) {
                    bulbArmed_ = false;
                    // WHERE THE PLAYER IS POINTING, through the one ray that
                    // decides what a blow lands on. A miss is a miss: reaching
                    // past the level's own geometry would hang a lamp in the
                    // sky.
                    const Swing sw = swingRay(wideWalkWorld(kArrowSolidsM), pos_, forward());
                    if (sw.hit) {
                        if (lmb) {
                            if (world_.removeLevelBulbNear(sw.point, kBulbPickM))
                                std::printf("v2: bulb removed -- %zu left\n",
                                            world_.levelBulbs().size());
                        } else if (world_.placeLevelBulb(sw.point)) {
                            std::printf("v2: bulb placed -- %zu now\n",
                                        world_.levelBulbs().size());
                        }
                        std::fflush(stdout);
                    }
                }
            } else {
                bulbArmed_ = true;
            }
            const bool swinging = !gunInHand && !bulbInHand &&
                (opt_.swingHold || (lmb && swingArmed_ && looking_ && !menuOpen_));
            // -- ...IT FIRES, AND IT KEEPS FIRING -------------------------
            //
            // AUTOMATIC, because it is an assault rifle and because the swing
            // it replaces repeats while the button is held -- a weapon that
            // needed a click per round would be the one thing in the kit that
            // behaves differently on the same button.
            //
            // GATED ON `looking_` like the swing and the draw: the pointer has
            // to be ours, or a click on the settings panel empties a magazine
            // into the wall behind it.
            // --swing-hold FIRES IT TOO, which is not a special case: that
            // flag means "the left button is down" for every other tool in the
            // kit, and it is the only way to photograph a thing that happens
            // while a mouse button is held. See its note beside opt_.swingHold.
            // EITHER GUN, AT ITS OWN RATE (user 2026-09-18: "left clicking the
            // pistol is not fireing bullets. fix that"). This used to name the
            // rifle, which is why the pistol held a model and nothing else --
            // see heldGun, which is the one place either of them is named now.
            if (gun >= 0 && lmb && (looking_ || opt_.swingHold) && !menuOpen_ &&
                simMs_ - lastShotMs_ >= gunIntervalOf(gun)) {
                // THE CLOCK IS STAMPED BY A ROUND LEAVING, not by the trigger
                // being pulled. fireRifle refuses while the gun is reloading,
                // and stamping anyway would hold the first shot of the fresh
                // magazine back by another whole interval -- for no reason the
                // player could see, on the one press that has been waited for.
                if (fireGun()) lastShotMs_ = simMs_;
            }
            // THE RIGHT BUTTON DRAWS, and only while the pointer is ours --
            // the same gate the swing has, and the JS engine's `locked`.
            // A SCRIPTED DRAW LETS GO ON A NAMED FRAME. --draw-hold alone pulls
            // and never fires, which photographs the draw; --shot-loose N is
            // what fires it, so a still of an arrow leaving is reproducible.
            const bool scripted =
                opt_.drawHold && (opt_.shotLoose < 0 || shotFrames_ < opt_.shotLoose);
            const bool drawing = scripted || (in.isMouseButtonDown(Input::MouseButton::Right) &&
                                              looking_ && !menuOpen_ && !holdLook_);
            // -- WHAT THE RIGHT BUTTON IS DOING, ON THE FRAME IT GOES DOWN ---
            //
            // (user 2026-09-20: "when right clicking the wheat in hand, the red
            //  voxel turned grey".)
            //
            // Right-click with wheat should do NOTHING -- the three claims on
            // this button are bow, ads and food, and wheat is none of them --
            // so reading the code says the colour cannot change and the screen
            // says it does. This prints which of the three the engine thinks it
            // has, and the palette table the hand is resolving against, so one
            // press settles which of those two is wrong. V2_HELD_PROBE=1.
            {
                const bool rmbNow = in.isMouseButtonDown(Input::MouseButton::Right);
                if (rmbNow && !rmbWas_ && held_.ready() && std::getenv("V2_HELD_PROBE")) {
                    const int selT = held_.selected();
                    const Tool &ht = held_.tool(selT);
                    std::printf("[held] RMB on '%s'  bow %d  ads %d  food %d  model %d\n",
                                ht.name, int(ht.bow), int(ht.ads), int(ht.food), held_.model());
                    std::fflush(stdout);
                }
                rmbWas_ = rmbNow;
            }
            // -- ...AND THE SAME BUTTON TAKES A BITE ----------------------
            //
            // (user 2026-09-17: "import the eating mechanics from v1 onto all
            //  of the food.")
            //
            // THE THREE CLAIMS ON THE RIGHT BUTTON ARE EXCLUSIVE and the tool
            // decides which one it is: `bow` draws, `ads` sights, `food` eats.
            // A tool is one of the three or none, which is why they are three
            // flags and not one enum with a value nobody set.
            //
            // HELD, NOT CLICKED. kEatMs is 900 ms of holding it down, and
            // letting go early loses the bite -- the model is visibly half
            // eaten while the button is down, so springing back whole is
            // exactly the feedback that says it did not count.
            const bool swallowed = held_.wantEat(drawing, simMs_);
            // THE CHEW GOES WITH THE BITE STARTING, not with it finishing --
            // see ToolSounds::eat. AFTER wantEat, because that is what raises
            // the edge; reading it first would play last frame's bite.
            if (held_.bitNow()) toolSfx_.eat();
            if (swallowed) {
                // ONE MOUTHFUL, PAID ON THE FRAME IT FINISHES. wantEat has
                // already taken it off the stack; this is the report.
                //
                // ...AND NOW IT IS WORTH SOMETHING. Until vitals.h this branch
                // printed a line and dropped the food on the floor -- there was
                // no bar for it to fill. One point of health and one of hunger,
                // v1's flat number for both; see Vitals::eat for why the
                // refusal is "nothing to gain" rather than "hunger is full".
                const bool used = vitals_.eat(1);
                std::printf("v2: ate one -- hp %d/%d, food %d/%d%s\n", vitals_.hp, kVitHpMax,
                            vitals_.food, kVitFoodMax, used ? "" : "  (nothing to gain)");
                std::fflush(stdout);
            }
            float draw = 0.0f;
            const bool released = held_.update(dt, swinging, player_.bobAmp, drawing, &draw);
            // -- THE MAGAZINE ARRIVES WHEN THE ANIMATION DOES -------------
            //
            // (user 2026-09-18: "the gun then reloads. there are animations for
            // the reload cycle.")
            //
            // IMMEDIATELY AFTER held_.update, which is the frame the clock this
            // asks about has just been advanced on. reloadDone() is an EDGE and
            // it consumes itself -- see its note -- so this refills once and
            // the badge pops once.
            //
            // NOT GATED ON THE GUN BEING IN HAND. It cannot be running unless
            // it was: scrolling off the rifle cancels the cycle outright
            // (HeldItem::cancelReload), which is the one thing that has to be
            // true for this line to be safe.
            // -- THE CYCLE, ONE LINE PER DRAWN FRAME ----------------------
            //
            // BEFORE reloadDone SO THE PHASE IS THE ONE THAT WAS ON SCREEN
            // this frame, and not the one the boundary has just moved to.
            // See App::lastReloadStrip_ for why an order needs a log at all.
            if (opt_.swingLog) {
                const int st = held_.reloadStrip();
                if (st != lastReloadStrip_) {
                    lastReloadStrip_ = st;
                    if (st >= 0) {
                        std::printf("v2: reload frame %02d  %s  (%d round(s) left)\n", st,
                                    held_.reloadPhaseName(), held_.reloadLeft());
                        std::fflush(stdout);
                    }
                }
            }
            if (held_.reloadDone()) {
                // WHICHEVER GUN IS IN THE HAND -- and it cannot be a different
                // one from the gun that started the cycle, because changing
                // hands cancels it (HeldItem::cancelReload).
                //
                // ONCE PER TURN OF THE STRIP, not once per reload: a revolver
                // gets a round per turn and the badge steps 1, 2, 3 as the
                // chambers fill, which is the animation being the mechanic
                // rather than a picture of one. A magazine gun's strip turns
                // once and loads the lot. See Tool::reloadRounds.
                // A ROUND IS IN THE GUN. Before the count, because this is
                // the same moment whichever gun it is and does not depend on
                // any of the bookkeeping below -- and once per ROUND, so the
                // revolver ticks six times and the rifle once. See
                // ToolSounds::gunLoaded for why it is the pickup's own voice.
                toolSfx_.gunLoaded();
                const int back = heldGun();
                if (back >= 0) {
                    const int per = held_.tool(back).reloadRounds;
                    const int now = per > 0 ? mini(gunMagOf(back), gunAmmoOf(back) + per)
                                            : gunMagOf(back);
                    setGunAmmo(back, now);
                    if (opt_.swingLog) {
                        // reloadLeft() RATHER THAN reloading(): the gun is still
                        // busy on the last round -- the cylinder has yet to swing
                        // shut -- but no more rounds are coming, and that is what
                        // this line is reporting. See HeldItem::reloadLeft.
                        std::printf("v2: %s loaded -- %d of %d%s\n", held_.tool(back).name, now,
                                    gunMagOf(back),
                                    held_.reloadLeft() > 0 ? "" : "  (reload done)");
                        std::fflush(stdout);
                    }
                }
            }
            // THE STRING STARTS CREAKING WITH THE PULL, and is cut the instant
            // it is let go -- whether or not a shaft left, so a half-draw never
            // rings on over the release. The JS engine's playBowStretch and
            // stopBowStretch, on the same two edges.
            if (held_.drewNow()) toolSfx_.draw();
            if (released) {
                toolSfx_.release();
                // AN ARROW IS AWAY. What it is worth is how far the bow was
                // pulled, which is what `draw` carries -- and the whoosh goes
                // with the SHAFT, not with the release: a bow that whooshed on
                // an empty loose would be lying about what happened, which is
                // that engine's own note on the line this comes from.
                if (loose(draw)) {
                    toolSfx_.loosed();
                    // ...AND THE FIRST ONE IS A DISCOVERY. Inside the same
                    // test as the whoosh, which is the point v1 makes about
                    // where this belongs: both are things that only happened
                    // if a shaft actually left the string.
                    unlockProjectile();
                }
            }
            // ...and the re-nock when the bow settles back to rest with a fresh
            // arrow on the string. Silent until sound/bow/reload.mp4 exists --
            // see ToolSounds::open.
            if (held_.nockedNow()) toolSfx_.nocked();
            if (held_.struck()) {
                // THE IMPACT FRAME. What a bite would be spent on; for now it
                // is the verdict and nothing else -- see the header of
                // render/helditem.h for why there is nothing to carve.
                lastSwing_ = swingRay(walkWorld(), pos_, forward());
                // -- ...AND THE THINGS THAT ARE NOT PART OF THE WORLD ANY MORE -
                //
                // "when a tree falls ... the player is unable to interact with
                // that felled object." swingRay walks the terrain and
                // `w.solids`, which is the list of things PLACED in the world,
                // and a felled tree left that list the moment it came down.
                // Nothing the tool asked could see it.
                //
                // Asked here rather than folded into swingRay because the ray
                // lives in render/helditem.h, which knows about a world you can
                // walk on and nothing about the debris band -- and because the
                // answer is compared on DISTANCE like any other: a log in front
                // of a rock wins, a rock in front of a log does not.
                lastDebris_ = DebrisHit{};
                {
                    const Vec3 eye = pos_, dir = forward();
                    DebrisHit dh;
                    if (world_.debrisRay(eye, dir, swingReachM(dir), &dh) &&
                        (!lastSwing_.hit || dh.t < lastSwing_.dist)) {
                        lastDebris_ = dh;
                        lastSwing_ = looseSwing(dh, eye, dir);
                    }
                }
                felled_ = false;   // per blow, not per fell -- see the swing log
                // -- ...AND A LIVING THING UNDER THE CROSSHAIR TAKES IT FIRST -
                //
                // (user 2026-09-14: "when hitting life, it turns an emmisive
                // red".)
                //
                // AHEAD OF EVERYTHING, which is v1's order -- its left click
                // calls tryKillCreature before the world bite. A rabbit
                // standing on grass is nearer than the grass, and a swing has
                // to spend itself on the first thing it meets or you dig a pit
                // through the animal you were aiming at.
                //
                // SPENDING THE SWING IS WHAT SILENCES THE REST. An emptied
                // lastSwing_ takes the tool knock, the amber sparks and the
                // whole bite chain with it -- the same idiom breakWheat and
                // tillGround use below, and the reason a blow on flesh throws
                // v1's RED embers instead of the rocks' amber ones rather than
                // both.
                // -- ...AND THE SPEND HAS TO SURVIVE THE RE-ASK BELOW -----
                //
                // (user 2026-09-15: "your still creating regular broken life
                // peices upon being killed ... the regular peices are getting
                // absorbed by the player".)
                //
                // EMPTYING lastSwing_ IS NOT ENOUGH ON ITS OWN. Forty lines
                // down, "A TOOL THAT WAS REFUSED ASKS AGAIN" re-runs the ray
                // against the MODELS whenever toolTakes says no -- and an
                // emptied swing is a refusal by definition (`if (!s.hit) return
                // false`). So every kill was followed by the tool re-aiming at
                // whatever stood behind the animal and carving it: a second set
                // of pieces, ordinary ones, which the player then absorbed.
                //
                // The flag says what happened rather than leaving it to be
                // inferred from an empty struct, which is the mistake above in
                // one word.
                const bool spentOnLife = strikeLife();
                if (spentOnLife) lastSwing_ = Swing{};
                // -- AND A CHUNK COMES OUT OF IT ------------------------------
                //
                // The impact frame is where the JS engine takes its bite, and
                // this is the same moment. What v2 cannot do is make the HOLE:
                // -- AND A CHUNK COMES OUT OF IT -----------------------------
                //
                // Ground and rock are the two that give. A trunk is an
                // instanced model rather than terrain, so it needs the private
                // copy the volume was kept for and is not wired here yet.
                // A TOOL TAKES ITS OWN MATERIAL AND NOTHING ELSE. The pick is
                // for stone, the axe is for wood, and swinging the wrong one
                // lands the blow and the sound but moves no voxels. Takes is
                // declared on the Tool (render/helditem.h) rather than guessed
                // from its name here, so a new tool states what it bites.
                //
                // BEDROCK IS NOT STONE FOR THIS PURPOSE. It is the floor of the
                // world -- see the note over mat::BEDROCK -- and a pick that
                // could take it out would open a hole into nothing.
                size_t dug = 0;
                // A TOOL THAT WAS REFUSED ASKS AGAIN, WITHOUT THE GROUND.
                //
                // swingRay reports the NEAREST thing under the crosshair, and
                // beside a boulder the terrain frequently is nearer -- stand
                // against a rock, aim a little down, and the ground march
                // answers first. The blow comes back as Ground on grass, the
                // pick refuses it because grass is not stone, and the rock you
                // were plainly aiming at goes untouched. Which rocks that
                // happens on depends on where you stand, not on the rock.
                //
                // So when the tool has been refused, the models are asked on
                // their own -- same ellipses, same reach, just without the
                // ground winning on distance. If one is there, that is what the
                // blow was for.
                // ...AND IT ASKS WHENEVER IT WOULD OTHERWISE DO NOTHING, not
                // only when the ground won. A swing can also come back as NO
                // hit at all -- the collider ellipse is measured over the
                // bottom two metres of a model, so a ray passing over a
                // boulder's shoulder misses it entirely while the stone is
                // plainly under the crosshair. Both cases end the same way,
                // with the blow doing nothing, so both ask the same question.
                // THE TEST IS "WAS IT REFUSED", SAID IN ONE WORD. It used to be
                // spelled out as "not a rock and not a trunk" plus the two
                // wants below, which came to the same thing for four kinds and
                // to the wrong thing for the fifth: an axe that had already
                // found a felled tree would ask again and be handed the
                // STANDING tree behind it, because Loose is not Trunk.
                if (!spentOnLife && !toolTakes(held_.takes(), lastSwing_)) {
                    const Takes tk = held_.takes();
                    const bool wantStone =
                        tk == Takes::Stone &&
                        !(lastSwing_.kind == Swing::Ground && isStoneMat(lastSwing_.material));
                    // ...AND AN AXE NOW WANTS A MUSHROOM AS WELL AS A TRUNK,
                    // so the model re-ask below cannot test for Trunk alone:
                    // aim down at a cap growing on a bank and the terrain wins
                    // on distance every time, which is the exact failure the
                    // note above describes for a pick beside a boulder.
                    const bool wantWood = tk == Takes::Wood;
                    if (wantStone || wantWood) {
                        const Vec3 eye = player_.eyePosition(), dir = forward();
                        Swing best;
                        const Swing ms = swingRayModels(walkWorld(), eye, dir);
                        if (ms.hit && ((wantStone && ms.kind == Swing::Rock) ||
                                       (wantWood && (ms.kind == Swing::Trunk ||
                                                     (ms.kind == Swing::Rock && ms.soft)))))
                            best = ms;
                        // ...AND THE LOOSE BODIES ON THE SAME TERMS, which is
                        // the case that matters most: a log is ON THE GROUND,
                        // so the terrain march wins on distance more often than
                        // not and the axe was refused for grass while the log
                        // was plainly under the crosshair.
                        DebrisHit dh;
                        if (world_.debrisRay(eye, dir, swingReachM(dir), &dh)) {
                            const Swing ls = looseSwing(dh, eye, dir);
                            if (toolTakes(tk, ls) && (!best.hit || ls.dist < best.dist)) {
                                best = ls;
                                lastDebris_ = dh;
                            }
                        }
                        if (best.hit) lastSwing_ = best;
                    }
                }
                // ...AND WHAT IT SOUNDED LIKE, decided on the swing that is
                // finally going to be acted on. The tool declares what it can
                // take (Takes, in render/helditem.h) and the blow decides the
                // rest, exactly as toolTakesFor and playToolHit split the job in
                // the engine this comes from.
                //
                // AFTER THE SECOND ASK, NOT BEFORE IT. It used to ring off the
                // first swingRay, which is a verdict the block above exists to
                // overturn -- so a tool that was refused by the ground and then
                // accepted by the rock behind it played the WRONG-TOOL knock
                // over a blow that carved. "A sound that disagrees with the
                // swing is worse than no sound, because it teaches the player
                // the wrong thing about their tool" -- and it is a felled log,
                // which is nearly always lying ON something, that made it
                // happen every time instead of occasionally.
                const Blow heard = toolSfx_.blow(held_.takes(), lastSwing_);
                // -- ...AND FOUR SPARKS OFF IT ---------------------------
                //
                // (user 2026-09-14: "everytime a tool hits something, play 4
                // sparks just like in v1. you can import the v1 spark
                // settings.")
                //
                // BESIDE THE SOUND AND FOR THE SAME REASON THE SOUND IS HERE:
                // this is the one line every landed swing passes through, after
                // the second ask has settled WHAT was struck. Fired on the
                // blow rather than on the bite, so a tool that rings off stone
                // it cannot cut still throws sparks -- which is what sparks
                // are, and v1 fires them above its own wound/kill split for
                // exactly that reason.
                if (lastSwing_.hit) particles_.toolSparks(lastSwing_.point, simMs_);
                // Hoisted out of the block below so the log can say WHICH of
                // these refused the blow -- see the NO BITE line.
                const bool stone =
                    lastSwing_.kind == Swing::Rock ||
                    (lastSwing_.kind == Swing::Ground && isStoneMat(lastSwing_.material));
                const bool wood = lastSwing_.kind == Swing::Trunk;
                // ...AND THE LOOSE GROUND, WHICH IS ONLY EVER TERRAIN. Stone
                // has two homes -- a boulder and a hillside -- and needs the
                // Rock arm above to cover both. Soil has one: nothing this
                // world places as a model is made of it, so there is no second
                // arm here and a shovel swung at a rock or a trunk simply
                // knocks. See isSoilMat.
                const bool soil =
                    lastSwing_.kind == Swing::Ground && isSoilMat(lastSwing_.material);
                // -- THE WHEAT BREAKS FIRST, AND FOR ANY TOOL -------------
                //
                // (user 2026-09-14: "when the player left clicks the wheat, the
                // wheat breaks, and the seeds and wheat drop.")
                //
                // AHEAD OF THE BITE CHAIN, because a blade is nearer than the
                // ground it grows on and the swing has to spend itself on the
                // first thing it meets. Without that, standing in a field and
                // swinging a shovel would dig a pit THROUGH the wheat, which is
                // the ground winning an argument it should not have been in.
                //
                // AND FOR ANY TOOL, which is why this is not a Takes. `left
                // clicks the wheat` is the whole condition -- straw does not
                // care whether you brought an axe -- and an empty hand works
                // too, since the swing is what lands, not the head on it.
                //
                // A MISS IS ORDINARY. mow() returns zero on a column with
                // nothing standing on it, and this then falls through to the
                // ordinary blow below exactly as if the wheat had not been
                // asked about. That is also what stops one plant paying out
                // twice: the second swing finds it already cut.
                // -- ...AND A HOE TURNS THE EARTH INSTEAD OF BREAKING IT ---
                //
                // BESIDE THE WHEAT AND FOR THE SAME REASON: both are swings
                // that spend themselves without taking a bite, so both have to
                // be settled before the chain below decides what came out of
                // the ground. The hoe is FIRST of the two -- a hoe swung at a
                // stand of wheat should turn the earth under it, not harvest
                // it, because that is the tool you chose.
                // -- THE WHEAT IS ASKED FIRST NOW, AND ONLY THE HOE CUTS IT -
                //
                // (user 2026-09-14: "make it where only the hoe can break the
                // wheat. if any other tool does it, play the antibreak sound.")
                //
                // THE ORDER IS THE OTHER WAY ROUND FROM YESTERDAY, and the note
                // that stood here argued for the old one: "a hoe swung at a
                // stand of wheat should turn the earth under it, not harvest
                // it, because that is the tool you chose." That was written
                // when ANY tool harvested, so the hoe needed protecting from
                // the wheat. Now the hoe is the only thing that harvests, and a
                // hoe swung at a stand of wheat is a player harvesting -- there
                // would be no other way to do it.
                //
                // Ground with nothing standing on it still tills, because
                // breakWheat answers false there and the chain falls through.
                // -- THE FRUIT IS NOT TAKEN HERE ANY MORE -----------------
                //
                // (user 2026-09-17: "when the player right clicks on a apple or
                //  orange, the model appears in the right hand".)
                //
                // pickFruit USED TO SIT AT THE FRONT OF THIS CHAIN, and it was
                // wrong twice over. It answered at the impact frame, a quarter
                // of a second after the click, judging an aim the swing itself
                // had already moved -- and it put its 0.9 m gate in front of
                // every axe stroke, so an apple near the line was picked
                // instead of the trunk being chopped. It is on the right button
                // now; see the mouse handler.
                if (breakWheat()) {
                    lastSwing_ = Swing{};   // the blow is spent
                } else if (tillGround()) {
                    lastSwing_ = Swing{};
                }
                if (lastSwing_.hit) {
                    const Takes t = held_.takes();
                    // ONE RULE, AND THE AUDIO ASKS THE SAME ONE. This was
                    // three comparisons written out here and three more written
                    // out in ToolSounds::blow -- see toolTakes in
                    // render/helditem.h, which is now the only place either of
                    // them asks. The three bools above survive as the LOG's
                    // explanation of a refusal, not as the decision.
                    if (toolTakes(t, lastSwing_)) {
                        // A BOULDER AND A HILLSIDE BREAK DIFFERENTLY. Terrain is
                        // a chunk to re-mesh; a rock is an INSTANCE that has to
                        // leave its shared model first. Same swing, same radius,
                        // two different edit paths -- see World::carveModel.
                        // BOTH ARMS BRACED, AND NOTHING BETWEEN THEM. This
                        // has now broken twice in the same way and both times
                        // the symptom was identical -- a rock that breaks and
                        // gives back no chunk, while the hillside behind it
                        // gets dug instead.
                        //
                        // The first time the else was unbraced and took only
                        // the assignment. The second time a felling test was
                        // added BETWEEN the two arms, which quietly re-bound
                        // the else to that test: every blow on a rock then ran
                        // dig() as well, which carved the terrain and
                        // overwrote the spoil with air on its way past.
                        //
                        // So the decision is one statement with two braced
                        // arms, and anything that wants to run afterwards runs
                        // AFTER it.
                        // A FELLED TREE IS A THIRD EDIT PATH, and it goes at
                        // the FRONT of the chain for the reason the note above
                        // gives: anything squeezed between the arms re-binds
                        // the else. A body has no chunk, no instance and no
                        // stump to leave -- what it has is its own voxels, and
                        // World::carveDebris takes the bite out of those and
                        // then asks whether what is left is still one thing.
                        // That question is the "break": see World::breakDebris.
                        if (lastSwing_.kind == Swing::Loose) {
                            dug = world_.carveDebris(physics_, lastDebris_, kDigRadiusVox, simMs_,
                                                     &spoilVol_, &spoilN_, &spoilAt_, &spoilYaw_)
                                      ? 1u
                                      : 0u;
                        } else if (lastSwing_.kind == Swing::Rock ||
                                   lastSwing_.kind == Swing::Trunk) {
                            // -- A BLOW ON THE HIVE IS A BLOW ON THE SWARM --
                            //
                            // (user 2026-09-16.) A hive is decor kind 5 and
                            // arrives here as a Swing::Rock like every other
                            // model, so modelKind is the only thing that can
                            // tell it from a boulder. Asked BEFORE the carve,
                            // because the carve can destroy the hive outright
                            // and then there is nothing left to identify.
                            if (lastSwing_.solid.modelKind == 5)
                                bees_.anger(lastSwing_.point, kBeeAngerM);
                            dug = world_.carveModel(lastSwing_.solid, lastSwing_.eye,
                                                    lastSwing_.dir, lastSwing_.reach,
                                                    kDigRadiusVox, &spoilVol_, &spoilN_,
                                                    &spoilAt_, &spoilYaw_)
                                      ? 1u
                                      : 0u;
                        } else {
                            spoilYaw_ = 0.0f;   // terrain is not turned
                            dug = world_.dig(lastSwing_.point, kDigRadiusVox, &spoilVol_,
                                             &spoilN_, &spoilAt_);
                        }
                        // ...AND DIGGING COSTS, THE WAY SWINGING DOES. v1's
                        // vitOnMine, a twentieth of a blow's charge -- see
                        // player/vitals.h. Every carve, not only the ones that
                        // took something: a swing at rock you cannot break is
                        // still work.
                        vitals_.onMine();

                        // ...AND IF THAT BLOW WAS THE ONE THAT CUT THROUGH, THE
                        // TREE COMES DOWN. Asked of the instance the carve just
                        // edited -- see World::fellTree, which decides by how
                        // much of the tree is no longer standing on anything
                        // rather than by counting blows.
                        // ...AND WHATEVER THAT LEFT STANDING ON NOTHING COMES
                        // DOWN. Asked after every carve on a model, rock or
                        // tree alike -- see World::fellTree, which decides by
                        // what is still connected to the model's bottom rather
                        // than by what kind of thing it is.
                        if (dug && physics_.available() &&
                            (lastSwing_.kind == Swing::Trunk || lastSwing_.kind == Swing::Rock))
                            felled_ = world_.fellTree(physics_, lastSwing_.solid, lastSwing_.dir,
                                                      simMs_);

                        // ...AND SO DOES WHATEVER WAS STANDING ON THE GROUND
                        // THAT JUST LEFT. The rule above is about one model's
                        // own voxels and seeds from its bottom row, which
                        // assumes there is ground under that row -- so digging
                        // the ground away instead of the model was the one way
                        // to leave a tree hanging that nothing ever asked
                        // about. See World::dropUndermined.
                        // THE GROUND AND ONLY THE GROUND. This was the else of
                        // the test above, which used to mean "anything that is
                        // not a model" and now would also mean a body -- and
                        // asking what a felled log has undermined is asking the
                        // terrain about a hole that is not in it.
                        else if (dug && physics_.available() &&
                                 lastSwing_.kind == Swing::Ground) {
                            world_.dropUndermined(physics_, lastSwing_.point, simMs_);
                            // ...AND THE SCATTER, WHICH THAT ONE CANNOT SEE.
                            //
                            // (user 2026-09-14: "flowers are still floating
                            // sometimes".) dropUndermined walks the COLLIDERS,
                            // and a flower has none -- it is drawn and walked
                            // through. Same question, second list. See
                            // World::dropScatterUndermined.
                            world_.dropScatterUndermined(
                                physics_, lastSwing_.point,
                                float(kDigRadiusVox) * VOXEL_M + 0.4f, simMs_);
                        }

                        // ...AND IT DOES NOT SIMPLY VANISH. The piece that came
                        // out becomes a rigid body: thrown a little back toward
                        // the person who swung, tumbling, and -- if it is small
                        // enough to carry -- collected a moment later. See
                        // World::spawnDebris and the absorb note beside it.
                        if (dug && physics_.available()) {
                            // IT POPS OUT WHERE IT WAS. It does not fly at you,
                            // and it is not hurled either -- v1 gives a chip a
                            // small shove ALONG the swing (away from the person
                            // who threw it) and lets gravity do the rest, and a
                            // separated piece gets no launch at all. What makes
                            // it come to you is the timer, not the throw: it
                            // tumbles where it fell for kAbsorbWaitMs and only
                            // then lifts. See World::updateDebris.
                            //
                            // NO THROW, NO SPIN, NO NUDGE CLEAR.
                            //
                            // ...AND IT IS STILL COLLECTED, mushroom or not
                            // (user 2026-09-14: "the chunks themselves obey the
                            // physics temporarily before getting absorbed from
                            // the player, just like it was before"). Nothing
                            // here marks the body scenery -- only the hanger
                            // spawn below does, because only the thing that
                            // came OFF the ground is the mushroom rather than a
                            // piece of it. See World::markScenery.
                            //
                            // It stops being part of the rock and starts being
                            // a body in the same instant and in the same place,
                            // and gravity is the only thing that touches it
                            // after that -- which is what v1 does. Every
                            // previous version of this line pushed the piece
                            // somewhere: along the swing (into the stone), back
                            // out of the face (it walked out along the cut), up
                            // (it floated at you). All of that was working
                            // around a collider that could not have the hole in
                            // it; the piece is stopped by the rock's own voxels
                            // now, so it needs no help getting clear.
                            const Vec3 at = spoilAt_;
                            const Vec3 kNoVel{0.0f, 0.0f, 0.0f};
                            const Vec3 kNoSpin{0.0f, 0.0f, 0.0f};
                            // THE ROCK IT CAME OUT OF, so the piece can be
                            // stopped by that rock's own voxels -- which have
                            // the hole in them. See World::updateDebris.
                            const Solid *srcRock =
                                (lastSwing_.kind == Swing::Rock ||
                                 lastSwing_.kind == Swing::Trunk)
                                    ? &lastSwing_.solid
                                    : nullptr;
                            // ...AND WHAT IT IS MADE OF GOES WITH IT, so the
                            // chip can be hit by the tool that cut it and not
                            // by the other one. Nothing in the spoil could say:
                            // the materials in it are the MODEL's palette ids,
                            // and a boulder's grey is not mat::ROCK. The swing
                            // knows, and this is the only moment it is asked.
                            // A MUSHROOM IS ASKED BEFORE THE KIND IS, because
                            // a cap arrives as Swing::Rock and the Rock arm
                            // would call it stone -- which would leave a chip
                            // of mushroom that only the pick could break up,
                            // off a cap that both tools had just cut.
                            const uint8_t spoilTakes =
                                lastSwing_.kind == Swing::Loose  ? lastDebris_.takes
                                : lastSwing_.soft                 ? uint8_t(kDebrisSoft)
                                : lastSwing_.kind == Swing::Trunk ? uint8_t(kDebrisWood)
                                : lastSwing_.kind == Swing::Rock  ? uint8_t(kDebrisStone)
                                : isSoilMat(lastSwing_.material)  ? uint8_t(kDebrisSoil)
                                                                  : uint8_t(kDebrisStone);
                            world_.spawnDebris(physics_, spoilVol_, spoilN_, at, kNoVel, kNoSpin,
                                               simMs_, spoilYaw_, srcRock, spoilTakes);
                        }
                        // ...AND THE GROUND THE BITE UNDERCUT COMES DOWN AS A
                        // BODY, not as a deletion. dropTerrainHangers finds
                        // the stone a blow cut loose from bedrock; before this
                        // it was carved away in place and simply vanished.
                        // A tree already did the right thing here, which is
                        // why this read as "only the terrain disappears".
                        {
                            const std::vector<uint8_t> *hv = nullptr;
                            int hn = 0;
                            Vec3 hat{0.0f, 0.0f, 0.0f};
                            float hyaw = 0.0f;
                            const Vec3 kStill{0.0f, 0.0f, 0.0f};
                            // AND THIS IS THE BODY THAT WAS FLYING. A model
                            // smaller than dropModelHangers' 39-voxel box has
                            // its whole severed half taken as hangers -- the
                            // wall never seeds, only the model's own floor does
                            // -- so cutting a mushroom's stem sends the entire
                            // cap down THIS path and never reaches fellTree.
                            // Called stone, it was a small chip of stone, and
                            // small chips are collected: half a second on the
                            // ground and then a curve into the player's hands.
                            if (world_.takeHangers(&hv, &hn, &hat, &hyaw)) {
                                const int hslot = world_.spawnDebris(
                                    physics_, *hv, hn, hat, kStill, kStill, simMs_, hyaw, nullptr,
                                    lastSwing_.soft ? uint8_t(kDebrisSoft)
                                    : isSoilMat(lastSwing_.material)
                                        ? uint8_t(kDebrisSoil)
                                        : uint8_t(kDebrisStone));
                                // ...AND A MUSHROOM THAT CAME OFF THE GROUND
                                // STAYS ON IT. This is the body that was
                                // flying: a model smaller than
                                // dropModelHangers' 39-voxel box never seeds
                                // from the box WALL, only from its own floor,
                                // so the whole severed cap arrives here in one
                                // piece -- and small enough to be collected.
                                //
                                // THE BITE IS NOT MARKED AND THIS IS, which is
                                // the whole distinction: what a blow knocks
                                // OUT is loot, what a blow cuts FREE is the
                                // mushroom.
                                //
                                // EVERY SOFT HANGER, NOT THE BIG ONES ONLY,
                                // and a size test is what that replaces: the
                                // bite is a radius-3 sphere, 113 voxels, and
                                // a small cap is about a hundred -- they
                                // overlap, so no threshold can tell a crumb
                                // from a cap. In practice there are no crumbs.
                                // Hangers appear only where a bite DISCONNECTS
                                // something, and a cap has been solid since it
                                // stopped being hollow, so the one place a
                                // mushroom comes apart is the stem. If a rim
                                // ever does nick off, it lies on the ground
                                // instead of being collected -- which is the
                                // safe way round to be wrong.
                                if (hslot >= 0 && lastSwing_.soft) world_.markScenery(hslot);
                            }
                        }
                    }
                }

                if (opt_.swingLog) {
                    static const char *kWhat[] = {"air", "ground", "trunk", "rock", "loose"};
                    static const char *kHeard[] = {"silent", "wood", "rock", "knock"};
                    // The FRAME, so a --swing-log run can be replayed one frame
                    // at a time with --shot-frame and the terrain compared
                    // across the blow. Correlating them by eye does not work:
                    // a swing lands about every thirty frames.
                    std::printf("v2: [f%d] swing -> %s", frameTick_,
                                kWhat[int(lastSwing_.kind)]);
                    // How many chunks that blow asked to be re-meshed. A disc
                    // that lands a slab at a time is this number against the
                    // adopt budget -- see World::remesh.
                    if (world_.lastRemesh)
                        std::printf("  [remesh %d chunks]", world_.lastRemesh);
                    if (lastSwing_.hit) std::printf("  %.2f m", lastSwing_.dist);
                    std::printf("  %s -> %s", held_.name(), kHeard[int(heard)]);
                    if (dug) std::printf("  DUG %zu chunk(s)", dug);
                    if (felled_) std::printf("  TIMBER");
                    // WHY NOTHING HAPPENED, when nothing happened. "the pick is
                    // not working 100% of the time" is three different failures
                    // wearing one face -- the tool refused the material, the
                    // carve moved no voxels, or the piece could not be spawned
                    // -- and they are told apart here rather than guessed at.
                    else if (lastSwing_.hit)
                        std::printf("  NO BITE: takes=%d stone=%d wood=%d soil=%d mat=%u",
                                    int(held_.takes()), int(stone), int(wood), int(soil),
                                    unsigned(lastSwing_.material));
                    if (dug) {
                        int solid = 0;
                        for (uint8_t v : spoilVol_)
                            if (v != mat::AIR) ++solid;
                        std::printf("  spoil=%d/%d loose=%d rockcol=%d/%d", solid, spoilN_,
                                    world_.looseCount(), physics_.convexMade(),
                                    physics_.convexTried());
                    }
                    std::printf("\n");
                    std::fflush(stdout);
                }
            }
        }

        // The BOB counts as movement. It shifts the eye every frame while
        // walking, so the accumulated samples describe a viewpoint that no
        // longer exists -- exactly as if the camera had been flown.
        const bool camMoved = lengthSq(pos_ - before) > 1e-10f;
        moving_ = moving_ || camMoved;
        // A MOVING TOOL COUNTS AS MOVEMENT, for exactly the reason the bob
        // does: the samples already in the film were drawn for a viewmodel that
        // is now somewhere else, and averaging them with the new ones smears
        // the axe rather than converging it. It is bounded -- animating() goes
        // false a moment after the swing ends and the hand settles -- so a
        // still player still gets a converged frame.
        return camMoved || turned || held_.animating() || arrows_.inFlight() > 0;
    }

    // -----------------------------------------------------------------------
    // Render one frame offline and write it.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Does the demodulation actually invert?
    //
    // The identity under test is the one Remodulate.cs.slang is written to
    // satisfy:
    //
    //     emission + albedo * (diffuse/albedo) + specAlbedo * (specular/specAlbedo)
    //         == the composited radiance the same path trace produced
    //
    // ONE SAMPLE, deliberately. The split channels hold this frame's sample
    // while gColor holds the accumulated MEAN, so the two are only comparable
    // when the mean is over a single sample. That is not a weaker test -- the
    // identity is per-sample, and averaging would only hide a violation.
    //
    // Pixels whose albedo fell under the epsilon floor are counted separately
    // rather than failed: there the division is deliberately not invertible,
    // and both sides come back black because the near-zero albedo multiplies
    // straight back in. Reporting them is how you tell "the floor did its job"
    // from "the split is broken".
    // -----------------------------------------------------------------------
    void checkDemodulation(Falcor::RenderContext *ctx) {
        std::printf("  check    demodulation round trip, 1 spp\n");
        tracer_.setDenoising(false);
        tracer_.setDemodulate(true);
        tracer_.resize(opt_.r.width, opt_.r.height, opt_.r.width, opt_.r.height);

        Camera cam;
        const float groundY = world_.terrain.heightM(opt_.camX, opt_.camZ);
        cam.origin = Vec3(opt_.camX, groundY + opt_.eye, opt_.camZ);
        cam.target = cam.origin + Camera::direction(opt_.yaw, opt_.pitch) * 50.0f;
        cam.fovDeg = opt_.fov;
        cam.aperture = opt_.aperture;
        cam.focusDist = opt_.focus > 0.0f ? opt_.focus : 40.0f;

        RenderSettings r = opt_.r;
        r.spp = 1;
        r.samplesPerFrame = 1;
        tracer_.renderSample(ctx, cam.gpu(tracer_.width(), tracer_.height()), r);
        tracer_.demodulate(ctx);
        tracer_.remodulatePassthrough(ctx);
        ctx->submit(true);

        const std::vector<uint8_t> aRaw =
            ctx->readTextureSubresource(tracer_.color().get(), 0);
        const std::vector<uint8_t> bRaw =
            ctx->readTextureSubresource(tracer_.remodulated().get(), 0);
        const std::vector<uint8_t> alRaw =
            ctx->readTextureSubresource(tracer_.demodAlbedo().get(), 0);
        const float *A = reinterpret_cast<const float *>(aRaw.data());
        const float *B = reinterpret_cast<const float *>(bRaw.data());

        const size_t n = size_t(opt_.r.width) * size_t(opt_.r.height);
        double sumAbs = 0.0, sumRef = 0.0, worst = 0.0;
        size_t worstAt = 0, floored = 0, compared = 0;
        // The albedo texture is RGBA16F, so it is read back as halves and the
        // epsilon test has to be made against the value the SHADER saw.
        const uint16_t *AL = reinterpret_cast<const uint16_t *>(alRaw.data());
        auto half2float = [](uint16_t h) -> float {
            const uint32_t sign = uint32_t(h >> 15) << 31;
            uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF;
            if (exp == 0) {
                if (man == 0) { const uint32_t b = sign; float f; std::memcpy(&f, &b, 4); return f; }
                exp = 1;
                while (!(man & 0x400)) { man <<= 1; --exp; }
                man &= 0x3FF;
            } else if (exp == 31) {
                const uint32_t b = sign | 0x7F800000u | (man << 13);
                float f; std::memcpy(&f, &b, 4); return f;
            }
            const uint32_t b = sign | ((exp + 112) << 23) | (man << 13);
            float f; std::memcpy(&f, &b, 4); return f;
        };

        for (size_t i = 0; i < n; ++i) {
            const float amin = minf(minf(half2float(AL[i * 4 + 0]), half2float(AL[i * 4 + 1])),
                                    half2float(AL[i * 4 + 2]));
            if (amin < 2e-3f) { ++floored; continue; }
            ++compared;
            for (int c = 0; c < 3; ++c) {
                const double a = A[i * 4 + c], b = B[i * 4 + c];
                const double d = fabs(a - b);
                sumAbs += d;
                sumRef += fabs(a);
                const double rel = d / (fabs(a) + 1e-4);
                if (rel > worst) { worst = rel; worstAt = i; }
            }
        }

        const double meanRel = sumRef > 0.0 ? sumAbs / sumRef : 0.0;
        std::printf("  compared %zu of %zu px (%zu under the albedo floor)\n", compared, n,
                    floored);
        std::printf("  mean rel %.6f %%   worst %.4f %% at px (%zu, %zu)\n", meanRel * 100.0,
                    worst * 100.0, worstAt % size_t(opt_.r.width),
                    worstAt / size_t(opt_.r.width));
        // Half precision on the split channels is the floor on what this can
        // reach: a 10-bit mantissa is about 0.1 % per channel, and the identity
        // sums three of them.
        const bool pass = meanRel < 0.005 && worst < 0.05;
        std::printf("  %s\n", pass ? "PASS -- the split is lossless"
                                    : "FAIL -- demodulation is not inverting");
    }

