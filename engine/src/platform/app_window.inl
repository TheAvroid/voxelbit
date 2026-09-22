// app_window.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: window placement, shutdown, resize
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHERE THE WINDOW OPENS.
    //
    // Falcor opens wherever Windows feels like putting it, which for anyone
    // using FancyZones means dragging it back into the same zone every single
    // launch. So the placement is remembered: written on the way out, restored
    // on the way in, and CENTRED on the primary monitor the first time, when
    // there is nothing to remember.
    //
    // VALIDATED AGAINST THE MONITORS THAT EXIST NOW, not just read back. A
    // saved position is a promise about a display layout, and unplugging a
    // second monitor breaks it -- restoring blind puts the window on a desktop
    // nobody can see, with no way to drag it back. MonitorFromRect with
    // MONITOR_DEFAULTTONULL answers whether the rectangle still lands on real
    // glass, and if it does not the window is centred instead.
    //
    // SWP_NOACTIVATE, because moving a window must never be the thing that
    // takes focus off whatever the person is actually doing -- and --background
    // exists precisely so v2 can be launched without stealing it.
    std::string windowStateFile() const {
        const char *base = std::getenv("LOCALAPPDATA");
        if (!base || !*base) return std::string();
        // THE SEPARATOR NEEDS A DOUBLED BACKSLASH, and for a long time it did
        // not have one. A single backslash followed by a v is the VERTICAL TAB
        // escape, 0x0B -- so this built a path with a control character where
        // the separator should be, and fopen refused it every time. Silently,
        // because neither the save nor the load has anywhere to report to.
        //
        // The effect was that window placement NEVER PERSISTED. Every launch
        // failed to read a file that had never been written, fell through to
        // the centred default, and looked like it was working -- because
        // centring a window is a reasonable-looking answer.
        return std::string(base) + "\\voxelbit-window.txt";
    }

    // -----------------------------------------------------------------------
    // THE VISIBLE EDGES, NOT THE WINDOW RECT. This is the whole reason the
    // window came back slightly low.
    //
    // GetWindowRect returns a rectangle that includes the invisible resize
    // border and drop shadow Windows keeps around a window -- several pixels
    // wider than anything drawn, and asymmetric: nothing at the top, a few
    // pixels at the sides and bottom. FancyZones snaps a window by its VISIBLE
    // frame, which is DWMWA_EXTENDED_FRAME_BOUNDS. Save one and restore the
    // other and the window lands offset by exactly that margin every time.
    //
    // So both halves work in visible coordinates: the saved rectangle is what
    // you can see, and the restore converts back through this margin to the
    // rectangle SetWindowPos wants.
    struct FrameMargin {
        long l = 0, t = 0, r = 0, b = 0;
    };

    static FrameMargin frameMargin(HWND hwnd) {
        FrameMargin m;
        RECT wr{}, fr{};
        if (!::GetWindowRect(hwnd, &wr)) return m;
        if (::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)) != S_OK)
            return m;
        m.l = fr.left - wr.left;
        m.t = fr.top - wr.top;
        m.r = wr.right - fr.right;
        m.b = wr.bottom - fr.bottom;
        return m;
    }

    static bool visibleFrame(HWND hwnd, RECT *out) {
        if (::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out, sizeof(RECT)) == S_OK)
            return true;
        return ::GetWindowRect(hwnd, out) != 0;
    }

    // How far above centre a window with no remembered placement opens, in
    // pixels. See the note in restoreWindowPlacement for why it is not applied to
    // a restored one.
    static constexpr long kLaunchNudgeUpPx = 20;

    void restoreWindowPlacement() {
        // NO WINDOW, NO WINDOW WORK. --out renders offline and there is no
        // window to ask; without this the null deref took the whole flag out.
        if (!getWindow()) return;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        if (!hwnd) return;

        RECT want{};
        bool have = false;
        const std::string path = windowStateFile();
        if (!path.empty()) {
            if (FILE *f = std::fopen(path.c_str(), "rb")) {
                // VERSIONED, because the numbers used to mean the window rect
                // and now mean the visible frame. Reading an old file as a new
                // one would reintroduce the very offset this fixes, so anything
                // without the marker is ignored and the window is centred once.
                int ver = 0;
                long l = 0, t = 0, w = 0, h = 0;
                if (std::fscanf(f, "v%d %ld %ld %ld %ld", &ver, &l, &t, &w, &h) == 5 && ver == 2 &&
                    w > 120 && h > 120) {
                    want = RECT{l, t, l + w, t + h};
                    have = ::MonitorFromRect(&want, MONITOR_DEFAULTTONULL) != nullptr;
                }
                std::fclose(f);
            }
        }

        if (!have) {
            RECT cur{};
            if (!visibleFrame(hwnd, &cur)) return;
            const long w = cur.right - cur.left, h = cur.bottom - cur.top;
            const long sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
            want = RECT{(sw - w) / 2, (sh - h) / 2, 0, 0};
            want.right = want.left + w;
            want.bottom = want.top + h;

            // TEN PIXELS ABOVE DEAD CENTRE.
            //
            // ONLY ON THE CENTRED DEFAULT, and that restriction is the whole
            // of the design. This function runs on EVERY launch and
            // saveWindowPlacement writes the result back on every shutdown, so
            // a nudge applied to the restored rect would be saved, restored,
            // and nudged again -- the window would walk ten pixels up the
            // screen per run until it went off the top. Applied here it moves
            // a window that has no remembered position, once, and the moment
            // the window is moved by hand that placement is what comes back.
            want.top -= kLaunchNudgeUpPx;
            want.bottom -= kLaunchNudgeUpPx;
        }

        const FrameMargin m = frameMargin(hwnd);
        ::SetWindowPos(hwnd, nullptr, want.left - m.l, want.top - m.t,
                       (want.right - want.left) + m.l + m.r,
                       (want.bottom - want.top) + m.t + m.b, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void saveWindowPlacement() {
        const std::string path = windowStateFile();
        if (path.empty()) return;
        if (!getWindow()) return;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        if (!hwnd) return;
        // A minimised or maximised window reports a position that is not the
        // one to come back to -- --background runs minimised for its whole
        // life, and saving that would move the window to the corner of the
        // world on the next ordinary launch.
        if (::IsIconic(hwnd) || ::IsZoomed(hwnd)) return;
        RECT fr{};
        if (!visibleFrame(hwnd, &fr)) return;
        if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "v2 %ld %ld %ld %ld\n", fr.left, fr.top, fr.right - fr.left,
                         fr.bottom - fr.top);
            std::fclose(f);
        }
    }

    void onShutdown() override {
        const auto t0 = std::chrono::steady_clock::now();
        // THE WORKER OUTLIVES NOTHING. It holds `this` and writes into a member
        // grid, so letting the process tear down around a running one is a use
        // after free with a thread attached to it.

        // A take still finalising owns a thread and a sink writer. Abandoning
        // it drops the file rather than waiting on an encoder while the device
        // is being torn down underneath it.
        recorder_.abandon();
        // THE TRAINED NETWORK, IF ANYONE ASKED FOR IT. Written here rather
        // than on a timer because a run is the unit of training: whatever the
        // cache learnt walking around is what gets kept.
        if (!opt_.nrcSave.empty() && nrc_.available()) {
            if (nrc_.saveWeights(opt_.nrcSave))
                std::printf("v2: wrote %s (%u batches trained)\n",
                            opt_.nrcSave.c_str(), nrc_.batches());
            else
                std::printf("v2: could not write %s\n", opt_.nrcSave.c_str());
            std::fflush(stdout);
        }
        // Before the window goes: an audio device held open past it is the
        // one kind of leak you can hear. ORDERED -- every source voice is made
        // from the engine and must be destroyed before it, so the two voices
        // go first and the device last.
        ambience_.stop();
        toolSfx_.close();
        audio_.close();
        saveWindowPlacement();

        // -- AND THEN LEAVE, RATHER THAN UNWIND -----------------------------
        //
        // (user 2026-09-20: "the game seems to freeze on the black screen when
        //  the user presses esc 3 times. it does eventually shuts down but it
        //  takes a minute.")
        //
        // EVERYTHING THAT OUTLIVES THE PROCESS HAS ALREADY BEEN WRITTEN by the
        // lines above: the take is abandoned, the NRC weights are saved, the
        // audio device is closed and the window placement is on disk. What is
        // left to destroy is GPU-side and process-local -- roughly 700 chunk
        // structures, kDebrisInstances bodies, a 420 MB triangle pool, ~690
        // material tables and a PhysX scene -- and every one of those releases
        // goes through Falcor's deferred-destruction fence. That is the minute.
        //
        // NONE OF IT SURVIVES THE PROCESS. Windows reclaims the address space,
        // the driver reclaims the device, and the file handles are already
        // flushed. Unwinding it politely buys nothing a player can see and
        // costs the whole of that wait.
        //
        // _Exit AND NOT exit(): no static destructors, which is the point --
        // those are what reach back into the device. stdout is flushed by hand
        // first, because _Exit does not.
        //
        // V2_SLOW_EXIT keeps the old path for anyone chasing a leak or a
        // validation-layer complaint, which is the one job the unwind is
        // actually good for.
        // -- ...AND IT HAS TO LEAVE WITH A ZERO -----------------------------
        //
        // (user 2026-09-20: "you're also not closing out the terminal when the
        //  game closes out.")
        //
        // std::_Exit(0) HERE EXITED WITH 0xC0000409 -- STATUS_STACK_BUFFER_
        // OVERRUN, the code __fastfail raises -- every single time, measured.
        // v2.bat holds the window open on a non-zero code on purpose ("only
        // hold the window open on failure"), so the quit that was made fast
        // started leaving a console sitting in the taskbar with "Press any key
        // to continue" in it. The same run with V2_SLOW_EXIT=1 exits 0.
        //
        // So the way out is timed and reported rather than guessed at: the
        // number below is what says whether a fast exit is still worth having.
        const auto now = std::chrono::steady_clock::now();
        const double teardownMs = std::chrono::duration<double, std::milli>(now - t0).count();
        // TWO NUMBERS, BECAUSE THEY HAVE DIFFERENT CAUSES. `waited` is
        // everything between the engine deciding to quit and this function
        // being reached -- Falcor's message loop noticing, the last frame
        // finishing, the window going -- and it is the black screen. `teardown`
        // is only what this function itself does.
        const double waitedMs =
            quitAt_.time_since_epoch().count() == 0
                ? 0.0
                : std::chrono::duration<double, std::milli>(now - quitAt_).count();
        std::printf("v2: shutdown -- %.0f ms waiting to be called, %.0f ms in here\n",
                    waitedMs, teardownMs);
        std::fflush(stdout);
        std::fflush(stderr);
        if (!std::getenv("V2_SLOW_EXIT")) {
            // TerminateProcess RATHER THAN _Exit, and the code is stated: this
            // is the same "do not unwind" decision, asked of the OS directly
            // instead of through a CRT path that was demonstrably not giving us
            // the code we passed it.
            ::TerminateProcess(::GetCurrentProcess(), 0);
        }
    }

    void onResize(uint32_t, uint32_t) override { tracer_.resetAccumulation(); }

  private:
    Options opt_;
    // Summits and lakes found in whatever window is loaded -- see world/poi.h.
    PoiIndex poi_;
    World world_;
    Tracer tracer_;
    Dlss dlss_;
    Player player_;
    // HEALTH AND HUNGER -- v1's two bars, see player/vitals.h. Beside the
    // player because every one of its inputs is the player's: where the body
    // is, whether it is in the air, whether it is in a lake.
    Vitals vitals_;
    // WHERE A DEAD PLAYER GETS UP. Taken on the first frame the body has a
    // position rather than from the spawn code, because the spawn roll mutates
    // opt_.camX/camZ as it steps clear of cover and there is no other record of
    // where the run began.
    Vec3 vitHome_{0.0f, 0.0f, 0.0f};
    bool vitHomeSet_ = false;
    // What is in the hand, and the state of the button that swings it. The
    // button is POLLED into a flag rather than read from the input state at
    // use, because a swing is armed on a press and repeats while it is held --
    // two different questions, and only the event knows the first one.
    HeldItem held_;
    // What the bow looses. A fixed pool of instances the world reserves -- see
    // render/arrows.h for why it is fixed.
    Arrows arrows_;
    Drops drops_;
    Butterflies flock_;
    // False until the left button has been seen UP once -- see onMouseEvent.
    // HOW BIG A BITE, in voxels of radius. Six is 60 cm at VOXEL_M -- a
    // pick-sized hole rather than a crater, and small enough that the voxel
    // pass touches roughly a dozen columns out of a chunk's 65 536.
    static constexpr int kDigRadiusVox = 3;
    // HOW WIDE A SWING CUTS WHEAT. Bigger than the dig radius on purpose: a
    // bite is a tool head meeting stone and this is a swing going THROUGH a
    // stand of straw, so it takes the tuft you aimed at rather than a
    // thumbnail of it. 0.5 m, which is about what one plant occupies.
    // WHAT A STRAY TALL BLADE COUNTS AS, when the hit is not inside any tuft.
    // tuftAt is what makes a blade tall in the first place, so this should
    // never fire -- it is the answer for the case where that stops being true.
    static constexpr float kLooseWheatM = 0.5f;
    // v1's TILL_R, which is 5 voxels -- a seed bed a metre across per swing.
    static constexpr float kTillRadiusM = 0.5f;
    // HOW FAR A SEED BED MAY BE FROM WATER AND STILL TAKE. Four metres, which
    // is a bed you can see the lake from -- far enough that a shoreline is
    // farmable along its length, near enough that it is a reason to farm THERE.
    static constexpr float kSeedWaterM = 4.0f;
    bool swingArmed_ = false;
    // The last pose the menu's copy row printed, kept so the row can show it
    // back rather than the player having to find the console.
    std::string poseCopied_;
    // ...and the sighted pose's own bake -- see the AIM DOWN SIGHTS card. Kept
    // apart from poseCopied_ so that copying one does not blank the other's
    // readout while both cards are on screen.
    std::string adsCopied_;
    Swing lastSwing_;
    // WHICH LOOSE BODY, AND WHICH OF ITS VOXELS, when lastSwing_ is a Loose
    // one. The slot alone would not do: the carve wants the struck VOXEL, and
    // re-deriving that from the hit point is a rounding away from the cell in
    // front or the one behind. Meaningless unless lastSwing_.kind is Loose.
    DebrisHit lastDebris_;
    vb::AudioDevice audio_;
    vb::Ambience ambience_;
    // THE GRASS UNDERFOOT -- v1's sound/footsteps/grass.mp4, the same looping
    // element faded by gain. See the footstep block in app_capture.inl.
    vb::Ambience steps_;
    float stepLastX_ = 0.0f, stepLastZ_ = 0.0f;
    ToolSounds toolSfx_;
    // What the settings panel has asked the arrow to be, and whether the frame
    // still has to act on it. The rows edit this rather than the tool's own
    // offset, so dragging stays responsive while the rebuild happens a frame
    // later -- see the note on the rows.
    HeldXform lastHeld_;  // where the hand was last frame -- see loose()
    ArrowOffset arrowWant_;
    bool arrowDirty_ = false;
    Falcor::ref<Falcor::FullScreenPass> crosshair_;
    // THE TWO BARS AND THE END OF THE RUN -- see shaders/Vitals.ps.slang and
    // shaders/GameOver.ps.slang. Both draw on the WINDOW after the blit, the
    // same position the crosshair takes and for the same three reasons.
    Falcor::ref<Falcor::FullScreenPass> vitalsPass_;
    Falcor::ref<Falcor::FullScreenPass> gameOver_;
    // Seeded per HIT, never per frame: a per-frame hash re-rolls every block
    // sixty times a second and sizzles instead of fading.
    int vitSeed_ = 1;
    int vitLastHp_ = kVitHpMax;
    // WHEN THE RUN ENDED, or a sentinel. The screen fades in over
    // kGameOverFadeMs and the body gets up at kGameOverHoldMs.
    double deathAtMs_ = -1.0;
    std::string deathWhy_;
    DayNight clock_;  // owns the sun; sunAz_/sunEl_ are its output
    bool placedTwice_ = false;
    std::vector<Solid> solids_;
    // ...and the wide gather's own, so the two cannot tread on each
    // other. See wideWalkWorld.
    std::vector<Solid> wideSolids_;  // decor near the player, regathered each tick

    Vec3 pos_{0, 2, 0};  // the EYE, derived from the player every frame
    float yaw_ = 0.0f, pitch_ = 0.0f, fov_ = 50.0f;
    float sunAz_ = 38.0f, sunEl_ = 24.0f;
    // Last phase uploaded, so applySun can tell when the moon has moved on.
    float moonPh_ = -1.0f;
    // HOW DARK THE NIGHT IS -- the menu row, --night-brightness -- and the two
    // levels it multiplies, both captured in onLoad once the flags that name
    // them have been read. See applyNightLevel.
    float nightLevel_ = defaults::kNightBrightness;
    float moonKeyBase_ = 1.0f;
    float nightFloorBase_ = 0.0f;

    bool looking_ = false;   // cursor captured, mouse turns the camera
    bool holdLook_ = false;  // ...because the right button is held
    bool moving_ = false;
    bool menuOpen_ = false;
    // -- WHERE YOU ARE, ON F3 --------------------------------------------
    //
    // (user 2026-09-19: "give me a toggle for coords. x, y, and z coords.")
    //
    // OFF BY DEFAULT, and not baked -- it is a readout you turn on to answer a
    // question and off again, not a preference you set once. F3 toggles it.
    //
    // F3 because that is where two decades of voxel games have put it, and
    // because the settings checkbox is the discoverable half -- a toggle with
    // no key is a trip to the menu every time, and a key with no checkbox is a
    // feature nobody finds.
    // OFF unless --coords asked for it. The flag exists so this can be SEEN
    // without a keystroke: every other setting in this engine can be reached
    // from the command line, and a readout that only appears after someone
    // presses F3 is one no headless shot can check.
    // WHICH BIOME [G] TAKES YOU TO NEXT -- an index into biomeNames(), or -1
    // for "none yet", which makes the FIRST press land in the band after the
    // one you are standing in rather than in pine every time. See
    // respawnToNextBiome.
    int respawnBiome_ = -1;
    // How many times it has been pressed -- the salt that moves the drop, so a
    // lap of the cycle comes back to the same WOOD and not the same clearing.
    uint32_t respawnHops_ = 0;
    // ...and how far along z one press may move you. Big enough that the next
    // lap is a different piece of forest, small enough that the band hop is
    // still the thing you notice.
    static constexpr float kRespawnRoamM = 450.0f;
    // How many dry destinations [G] will look for before it settles for the
    // ring step in teleportTo. Twenty is far more than a wooded band needs and
    // costs twenty pure terrain queries, which is nothing beside the hop
    // itself -- see respawnToNextBiome.
    static constexpr int kRespawnDryTries = 20;
    // -- THE PINNED ARRIVALS (user 2026-09-21) --------------------------
    //
    // "save the position -2135 231 -2077 for the pine spawnpoint. have this
    //  be the only spawnpoint when pressing g to cycle through biomes", then
    //  "make -3339 212 -314 the spawnpoint for the birch forest".
    //
    // A TABLE FROM THE SECOND ONE ON. The first was written as a pair of
    // constants and an `if`; a second would have made that two ifs, and the
    // fourth would have made it a bug. A biome with no row here keeps the
    // roam, which is what the oak, cherry and desert still want -- a second
    // visit to those is a different piece of them, on purpose.
    //
    // ONLY X AND Z. teleportTo stands the body on whatever ground it finds,
    // which is the only height that stays right across an edit, a dig or a
    // different DEM -- the y in each ask is where the ground happened to be
    // on the day it was read.
    //
    // MEASURED BEFORE THEY WERE PINNED: woodWeights gives 1.000 for the named
    // band and 0.000 for every other at each, so none sits near a seam where
    // it could drop you in the neighbouring wood.
    //
    // -- ...AND A BAND MAY HOLD MORE THAN ONE (user 2026-09-21) -----------
    //
    // "add in 1942 120 2505 into the pine forest rotation. so the pine forest
    //  should have 2 locations now that it should cycle through. facing
    //  northwest."
    //
    // SO THIS IS A LIST, NOT A LOOKUP. Rows are matched in the order they are
    // written and a band walks its own rows ONE PER VISIT, so the pine cycles
    // its clearings in turn and is back at the first on its fourth lap. A band
    // with one row still lands in the same place every time and a band with
    // none still rolls -- both unchanged. Adding a fourth is one line; nothing
    // counts these but the modulo in respawnToNextBiome.
    static constexpr float kKeepFacing = -1000.0f;   // "however you were facing"
    struct PinnedSpawn {
        Biome biome;
        float x, z;
        // A COMPASS BEARING IN DEGREES -- 0 N, 90 E, 180 S, 270 W -- which is
        // the convention the compass strip and /locate already use; see the
        // "WHICH WAY IS NORTH" note in app_gui.inl. kKeepFacing leaves the
        // camera pointing wherever it was, which is what [G] did for every
        // row before this one and is why it is the default.
        float yaw = kKeepFacing;
    };
    static constexpr PinnedSpawn kPinnedSpawns[] = {
        // Re-measured 2026-09-21 ("at the coords -2126 233 -2077 spawn the
        // player facing east") -- the same clearing as the original
        // -2135 231 -2077, nine metres east, now with a bearing.
        {Biome::Pine, -2126.0f, -2077.0f, 90.0f},   // 90 = E
        {Biome::Pine, 1942.0f, 2505.0f, 315.0f},   // 315 = NW
        {Biome::Pine, 2056.0f, 224.0f, 225.0f},    // 225 = SW  (user 2026-09-21)
        {Biome::Birch, -3339.0f, -314.0f},
    };
    // HOW MANY TIMES [G] HAS LANDED IN EACH BAND -- the cycle index into that
    // band's pinned rows, sized off the last enumerator.
    //
    // NOT respawnHops_, WHICH COUNTS EVERY PRESS. A band comes round once per
    // lap of the biome cycle, so with an EVEN number of reachable biomes the
    // press count is the same parity at every visit and a two-row band would
    // show one of its addresses forever. Five biomes hide that; --pine, which
    // makes the cycle one long, does not.
    uint32_t pinnedVisit_[size_t(Biome::Desert) + 1] = {};
    // -- THE CACTUS SPINES ------------------------------------------------
    //
    // v1's CACT_CD and CACT_MARGIN, carried over rather than re-picked: 0.9 s
    // between points and 0.4 m of reach past the body, which is what makes the
    // collision clamp's last millimetre still count as touching it.
    static constexpr float kCactusHurtSec = 0.9f;
    static constexpr float kCactusReachM = 0.4f;
    float cactT_ = kCactusHurtSec;   // rests PRIMED -- see the note at its use
    bool rmbWas_ = false;   // the right button last frame -- see the probe in processInput

    // Is the body against a cactus? The desert's scatter is kind 7 and the
    // Solid already carries its kind, so this asks the collision list the
    // player is already standing in rather than the voxel grid v1 had to scan.
    bool touchingCactus() const {
        const float feet = player_.pos.y;
        const float head = feet + kBodyHeightM;
        for (const Solid &s : solids_) {
            if (s.modelKind != 7) continue;
            if (solidBoxOverlap(s, player_.pos.x, feet, player_.pos.z, head,
                                player_.halfWidth + kCactusReachM, VOXEL_M))
                return true;
        }
        return false;
    }
    bool showCoords_ = false;
    // The water panel, its own capture memory, and one bool per term. All on:
    // the panel subtracts, it does not build the water up from nothing.
    bool waterPanelOpen_ = false;
    // [K] -- the stack badge's four numbers. See the panel.
    bool stackPanelOpen_ = false;
    bool stackPanelForce_ = true;
    // -- THE BADGE'S POP -- see setStackBadge -------------------------------
    //
    // What the number said last frame and which tool was saying it. BOTH, or
    // scrolling from a stack of two to a stack of two pops for a change that
    // did not happen -- and worse, scrolling from x5 to x2 pops as though you
    // had just spent three.
    // How many voxels the last arrow chip lifted -- for --shaft-test, which is
    // the only way to see a number a shaft picks for itself.
    int lastChipN_ = 0;
    // Where the fire test's body was 30 frames before it is judged -- see the
    // report above, which needs a RATE to tell a resting piece from a falling
    // one and has no velocity of its own to read.
    float chipWatchY_ = 0.0f;
    // Where the point-blank shot was fired from -- see the fire test.
    Vec3 pbFrom_{0.0f, 0.0f, 0.0f};
    Vec3 refreshFrom_{0.0f, 0.0f, 0.0f};   // where the refresh test started
    // How far off the wall the point-blank shot is taken. Well inside the
    // metre and a half the arming used to swallow, and far enough out that the
    // muzzle -- which is what the round is actually born at -- is still on this
    // side of it.
    static constexpr float kPointBlankM = 1.6f;
    int lastChipSlot_ = -1;
    Vec3 lastChipAt_{0.0f, 0.0f, 0.0f};
    int stackPopN_ = -1;
    int stackPopTool_ = -1;
    double stackPopT0_ = -1e9;
    // The last bake, kept so the panel can say it took -- see the bake button.
    std::string stackBaked_;
    bool captureBeforeWater_ = false;
    bool captureBeforeRoom_ = false;
    // -- THE PAUSE PANEL, AND WHERE IT IS STANDING ------------------------
    //
    // Pinned once, when the key is pressed -- see setRoomOpen. Everything that
    // draws, picks or labels a button reads the pose from here, so there is one
    // description of where the panel is rather than three.
    bool pauseOpen_ = false;
    Vec3 panelAt_{0.0f, 0.0f, 0.0f};
    // THE HEADING THE PANEL WAS OPENED WITH, kept so it can follow the player
    // without turning with them -- see repositionPanel.
    Vec3 panelFlat_{0.0f, 0.0f, -1.0f};
    Vec3 panelRight_{1.0f, 0.0f, 0.0f};
    Vec3 panelUp_{0.0f, 1.0f, 0.0f};
    Vec3 panelInto_{0.0f, 0.0f, -1.0f};
    // HOW FAR IN FRONT OF THE EYE. Far enough that three buttons 1.3 m apart
    // are all comfortably in view at once, near enough that they read as
    // something you could reach out and press.
    static constexpr float kPanelReachM = 2.6f;
    // -- THE THREE BUTTONS. press is 0 out and 1 fully down; held is the one a
    //    finger is on, or -1; pend is the one whose action has not fired yet.
    static constexpr float kBtnDownSec = 0.08f, kBtnUpSec = 0.16f;
    float btnPress_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // The green button has been pressed and the wood is not ready yet.
    float btnClock_ = 0.0f, btnAt_ = 0.0f;
    int btnHeld_ = -1, btnPend_ = -1;
    // WHEN THE ENGINE ASKED TO GO. Everything between this and onShutdown is
    // black screen the player is waiting through, and until it was measured it
    // was assumed to be nothing -- see the note in onShutdown.
    std::chrono::steady_clock::time_point quitAt_{};
    void askShutdown(int code) {
        if (quitAt_.time_since_epoch().count() == 0)
            quitAt_ = std::chrono::steady_clock::now();
        shutdown(code);
    }
    // -- THE RED BUTTON'S AFTERMATH. quitT_ is the collapse, 0 to 1.
    //
    // 1.15 s ONCE, AND IT WAS THE WHOLE WAIT (user 2026-09-20: "can you speed
    // up the time in which the game shuts down ... reduce the time of the black
    // screen on shutdown"). Measured before changing it: everything AFTER the
    // collapse -- Falcor noticing the window should close, the last frame
    // finishing, onShutdown, the exit -- is `171 ms waiting to be called, 0 ms
    // in here`, and that 171 ms is one HEADLESS frame at 3820x1990. In a real
    // session a frame is a sixtieth of a second, so the process side of the
    // quit is already about as fast as it can be and the collapse was all of
    // the rest of it.
    //
    // A TUBE DOES NOT TAKE A SECOND TO GO OUT either, so this is nearer the
    // thing it is imitating as well as nearer what was asked for. Still long
    // enough to read as a collapse rather than a cut.
    //
    // -- ...AND THEN HALF OF THAT WAS TOO MUCH -----------------------
    //
    // (user 2026-09-20: "slow down the ending tv close screen by 50%. or
    //  just restore it to what it was. it was the complete black screen that
    //  I want to be gone quicker, and you succeeded at that.")
    //
    // WHICH IS THE WHOLE LESSON OF THE NOTE ABOVE, READ BACKWARDS. The 1.15
    // was cut because the measurement showed the collapse was all of the
    // wait -- true, and it does not follow that the collapse was the part
    // worth cutting. The black screen AFTER it is dead time; the collapse is
    // the thing the player is watching. Those got shortened together because
    // one number controlled both, and only one of them should have been.
    //
    // 0.9 IS HALF SPEED, which is what "slow down by 50%" says literally --
    // the animation takes twice as long. It stops short of the 1.15 the
    // other half of the sentence offers, because the black screen that
    // followed it is genuinely gone now (TerminateProcess, see shutdown) and
    // the two were only ever judged together.
    static constexpr float kQuitFadeSec = 0.9f;
    bool quitting_ = false;
    float quitT_ = 0.0f;
    // ALL NINE ON -- see kWFDefault in Shared.slang. App::onLoad overwrites
    // every one of them from opt_.waterFlags, so this initialiser and that
    // default cannot drift apart in practice; it is written out here so reading
    // the member says the same thing.
    bool waterTerm_[10] = {true, true, true, true, true, true, true, true, true, true};
    // ---- the console (T) ---------------------------------------------------
    // A command line, the way the browser engine has one. It exists for
    // /locate: the biomes are bands now (see birchWeight in
    // scene/voxelworld.h), so "the birch forest" is somewhere you can be sent.
    // Seconds of wall time since launch, for the wave field only.
    float waveClock_ = 0.0f;
    bool consoleOpen_ = false;
    bool consoleFocus_ = false;          // grab the caret on the frame it opens
    bool consoleCapture_ = false;        // was the mouse captured before it opened
    char consoleBuf_[160] = {0};
    std::string consoleMsg_;             // the last reply, drawn after the box shuts
    double consoleMsgUntil_ = 0.0;       // steady-clock seconds; 0 means nothing to draw
    static constexpr double kConsoleMsgHold = 5.0;  // how long the reply stays up
    static constexpr float kConsoleMsgFade = 1.0f;  // ...of which the last second fades
    // False until the panel has been centred for this opening; see onGuiRender.
    bool menuPlaced_ = false;
    // Where the player last dragged the settings panel, in framebuffer pixels.
    // Negative x means "never opened", which is the only state that centres.
    ImVec2 menuPos_ = ImVec2(-1.0f, -1.0f);
    bool captureBeforeMenu_ = false;
    bool shotRequested_ = false;
    int shotIndex_ = 0;
    int shotFrames_ = 0;

    // -- the recorder -----------------------------------------------------
    vb::Recorder recorder_;
    int takeIndex_ = 0;
    // HOW MANY TIMES THE RECORDER HAS BEEN ASKED, which is not how many takes
    // there are: it refuses without a display, and it refuses while the last
    // one is still encoding. --rec-test reads it, because "the key never
    // arrived" and "the key arrived and the recorder declined" look identical
    // from outside and only one of them is a binding bug.
    int recAsks_ = 0;
    // The one-off "f9 records" line, and whether it has been spent. See the
    // reload branch in onKeyEvent for why this is raised there of all places.
    double recHintAt_ = -1.0;
    bool recHintSpent_ = false;
    bool recStarted_ = false;  // --rec has fired; see the scripted take
    // The take that finished most recently, and when, for the on-screen
    // acknowledgement in onGuiRender. Cleared once it has faded.
    vb::Take savedTake_;
    double savedAt_ = 0.0;
    static constexpr double kSavedNotice = 6.0;  // seconds the notice lives

    // One entry per displayed frame, in milliseconds. Kept whole rather than
    // reduced online because the interesting statistics are the tail ones, and
    // a running mean and variance cannot answer for a tail.
    std::vector<float> frameMs_, streamMs_;

    float fps_ = 0.0f;
    Streamline sl_;
    // What DLSS-G has actually put on screen. Counted rather than inferred --
    // see the note where they are accumulated.
    uint64_t generatedTotal_ = 0;
    uint64_t presentedTotal_ = 0;
    int generatedThisSecond_ = 0;
    float genFps_ = 0.0f;

    // Streamline is told to reset its history on the first frame and after
    // anything that invalidates reprojection.
    bool slReset_ = true;
    // The sizes frame generation was last DECLARED with, so the options call is
    // made only when they actually change -- see the note where it is issued.
    uint2 slFgDim_{0, 0};
    uint2 slFgOut_{0, 0};
    Neural neural_;
    Nrc nrc_;
    VolFog volfog_;
    Clouds clouds_;
    Atmosphere atmo_;
    Ddgi ddgi_;
    Sharc sharc_;
    Restir restir_;
#if V2_HAS_NRCSDK
    NrcSdk nrcSdk_;
    uint32_t nrcSdkLogTick_ = 0;
#endif
    Cuda cuda_;
    Clusters clusters_;
    Physics physics_;
    Birds birds_;
    // What lives on and in the water -- see render/lake.h. Its own system
    // rather than a branch of the flock: a fish, a pad and a dragonfly share a
    // WATER FIELD and nothing else, where a butterfly shares the meadow's.
    LakeLife lake_;
    // ...and the songbirds in the sky, which are not the perched ones in a
    // different state -- see render/birdflock.h.
    BirdFlock flock2_;
    Bunnies bunnies_;
    Bees bees_;
    Critters critters_;
    // v1's sparks and its death smoke. See render/particles.h.
    Particles particles_;
    // ...and what a blow does to a living thing. See render/lifehit.h.
    LifeHits lifeHits_;
    // Whether the last blow on a living thing KILLED it, as against merely
    // landing. strikeLife returns "did the swing spend itself", which is true
    // for a wound too -- and a test that took that for a kill measured a corpse
    // that was never made.
    bool lastKilled_ = false;
    // How many pieces the last kill actually threw. For --kill-test: the
    // shatter can be refused (no debris slot, a model loaded without its
    // voxels) and a corpse that simply vanishes looks the same as one that
    // came apart unless somebody counts.
    int lastPieces_ = 0;
    // WHERE THE HIVES AND THE BLOOMS ARE, gathered off World::decorNear twice a
    // second rather than per bee per frame -- the same clock and the same
    // reasoning as the perched birds' tree list, and for the same kind of
    // thing: a hive and a flower do not move.
    std::vector<Vec3> banksNear_;
    std::vector<Vec3> hivesNear_, bloomsNear_;
    // THE TOOLS ON THE DECK. Attached to the population rather than owning a
    // second copy of the strips: it edits a BAKE, and the bake is read by the
    // rabbits in the wood -- see render/assetedit.h.
    AssetEdit edit_;

    // Where the wood was when U was pressed -- see the handler.
    Vec3 woodPos_{0, 0, 0};
    float woodYaw_ = 0.0f, woodPitch_ = 0.0f;
    bool woodFly_ = false;
    std::vector<Solid> perches_;  // trees a songbird may sit in -- see the note at its update
    // Where seeds have to come back out of the ground this frame -- see the
    // till revert. Cleared every tick; empty on all but a handful of them.
    std::vector<Vec3> seedsBack_;
    // The two kit slots a broken wheat plant pays into, or -1 if the art did
    // not load. See the kit block and breakWheat.
    int wheatTool_ = -1, seedsTool_ = -1, steakTool_ = -1;
    // WHICH RUNG OF THE ESC LADDER THE NEXT PRESS IS. 0 = free the
    // mouse, 1 = the three balls, 2+ = quit. Counted rather than read
    // off the state, so the presses-to-exit do not depend on where the
    // pointer happened to be -- see the handler in app_input.inl and
    // what inferring it cost ("when I do double esc, it crashed my
    // game"). Reset wherever the ladder is stepped off.
    int escRung_ = 0;
    // ...AND THE TWO FRUIT, which are kit slots on exactly the wheat's terms:
    // loaded at start-up, stowed, and given by picking one in the world.
    int appleTool_ = -1, orangeTool_ = -1;
    // ...AND THE SHAFT YOU WALK OVER. Stowed like the rest until one is
    // recovered -- see the arrow impact.
    int arrowTool_ = -1;
    // The kit slot a deliberate pick is flying in, so the arrival can put it in
    // the hand rather than only in the wheel -- see pickFruit.
    int selectOnArrive_ = -1;
    // ...AND THE ONE THE LEVEL HANDS OVER, or -1 if the art did not load. Not
    // one of the three above: those are paid out by the world and this is paid
    // out by the door on [O]. See the kit block, standInLevel and leaveLevel.
    int rifleTool_ = -1;
    // ...AND THE PISTOL, ONE SLOT ABOVE IT IN THE WHEEL (user 2026-09-18:
    // "when the player scrolls up it selects it"). Handed over and taken back
    // by the same door, for the same reason -- see standInLevel.
    int pistolTool_ = -1;
    // The lamp you place them with -- see the kit block. Immediately after the
    // rifle in the wheel, which is the whole of "scroll up from the rifle".
    int bulbTool_ = -1;
    // EDGE-TRIGGERED, unlike the swing -- see the bulb block. Cleared while no
    // button is down and consumed by the press, so one click is one lamp.
    bool bulbArmed_ = true;
    // How near a click has to land to take a lamp down. 1.5 m: a bulb is five
    // voxels of glass on a ceiling and asking somebody to hit it exactly is
    // asking them to be precise about something they can barely see.
    static constexpr float kBulbPickM = 1.5f;
    // WHEN THE LAST ROUND WENT OFF, on the sim clock -- the shot clock under
    // --shot-walk, so a scripted burst paces identically to a live one.
    double lastShotMs_ = -1.0e9;
    // -- WHAT IS IN THE MAGAZINE -------------------------------------------
    //
    // (user 2026-09-18: "put a number next to the assault rifle just like the
    // stacked number on hand held items. this is to count the guns ammo. have
    // it start with 20 bullets.")
    //
    // TWENTY, AND IT IS THE USER'S NUMBER -- not a real magazine's 30. Said
    // here as a constant because three places need it and none of them should
    // own it: fireRifle spends it, the reload refills to it, and the badge asks
    // whether it is full.
    //
    // NOT A Tool FIELD, WHICH IS WHERE IT NEARLY WENT. HeldItem::Tool carries
    // `stack`, and an ammo count looks exactly like one more of those -- but
    // `stack` is how many of a thing you are carrying and is saved and restored
    // by snapshotKit as part of the wheel. Rounds in a gun are not a count of
    // guns. Keeping it here also keeps HeldItem free of the idea: that file
    // owns the reload's CLOCK and knows nothing about what it is reloading.
    static constexpr int kRifleMag = 20;
    int rifleAmmo_ = kRifleMag;
    // -- ...AND THE PISTOL'S SIX -------------------------------------------
    //
    // (user 2026-09-18: "there should only be 6 bullets fired until the pistol
    // has to reload.")
    //
    // A SECOND NUMBER RATHER THAN A SHARED ONE, which is the whole of what
    // makes the two guns different to hold: same trigger, same reload key, same
    // badge, and a magazine a third the size. Kept beside the rifle's for the
    // reason that one is not on the Tool -- see above.
    static constexpr int kPistolMag = 6;
    int pistolAmmo_ = kPistolMag;
    // -- THE LAST RELOAD FRAME --swing-log REPORTED ------------------------
    //
    // (user 2026-09-18: "the reload needs to stay open as it cycles through
    // the bullets.")
    //
    // A CYCLE IS ONLY WRONG IN ITS ORDER, and an order is not something a
    // screenshot can hold: the complaint above is the strip's first frames
    // coming back between rounds, which is four frames out of forty-four and
    // is over in 180 ms. So the log prints the strip frame each time it
    // CHANGES -- once per drawn frame rather than once per engine frame -- and
    // the whole cycle is then one readable column. -2 so the first one prints.
    int lastReloadStrip_ = -2;
    // The wheel as the wood left it -- see standInLevel. Empty while in the
    // wood, which is also what says "there is nothing to put back".
    std::vector<std::pair<bool, int>> woodKit_;
    Bullets bullets_;
    // A PLAIN FRAME COUNTER. It paces the perch query above and salts the
    // chunk hashes; it is not the tracer's tick, which is a sampler seed.
    uint32_t frameTick_ = 0;
    double fpsAccum_ = 0.0;
    int fpsFrames_ = 0;
    uint32_t liveMaxAccum_ = 0;
    int liveDepth_ = 0;
    char clockText_[16] = {0};
    std::string bakeStatus_;
    std::chrono::steady_clock::time_point lastTime_ = std::chrono::steady_clock::now();

