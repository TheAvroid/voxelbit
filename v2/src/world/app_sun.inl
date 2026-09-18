// app_sun.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: sun, sky, night, and screen recording
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Push the clock's sun into the sky, and say whether it moved enough to
    // matter.
    //
    // THE THRESHOLD IS THE POINT. At 1x the sun sweeps 0.3 degrees a second, so
    // every single frame moves it a little and a naive "did it change?" would
    // throw the accumulation away sixty times a second and never let a still
    // frame converge at all.
    // -----------------------------------------------------------------------
    // THE SUN'S COLOUR FOLLOWS WHICHEVER SKY IS ON, through the same air the
    // dome is integrated through. Negative hands sky.h back to its own
    // Kasten-Young fit, so the Preetham path stays bit-for-bit what it was.
    //
    // Its own function because there are TWO places that place the sun -- the
    // day/night clock, and --sun-az/--sun-el on the offline path -- and the
    // offline one deliberately never runs applySun. Returns whether the value
    // changed, so the caller only pays for a rebuild when it has to.
    bool syncSunToSky() {
        const Vec3 want = (atmo_.enabled && atmo_.available())
                              ? atmo_.sunTransmittance(world_.sky.sunDir())
                              : Vec3(-1.0f, -1.0f, -1.0f);
        const Vec3 had = world_.sky.sunTransOverride;
        if (fabsf(want.x - had.x) + fabsf(want.y - had.y) + fabsf(want.z - had.z) < 1e-6f)
            return false;
        world_.sky.sunTransOverride = want;
        return true;
    }

    // -----------------------------------------------------------------------
    // IS THE SUN DOWN? -- the fireflies' whole existence, and ONE reading of it.
    //
    // OFF THE SKY AND NOT OFF THE CLOCK, which is the difference between a test
    // that is right on both paths and one that is right on the interactive one.
    // The clock drives the sun while the viewer is running; an OFFLINE render
    // takes its sun from --sun-az/--sun-el and leaves the clock at its default
    // (see the note over the --water-flags branch in onLoad -- "the clock is a
    // VIEWER concept"). So `--out --time 23` asked for midnight and the report
    // said "daylight -- none expected", which is the report being asked the
    // wrong question.
    //
    // world_.sky is what actually lit the frame, both ways round. To render
    // fireflies offline, name the sun: --sun-el -10.
    //
    // A SHADE BELOW THE HORIZON rather than at it, so they come up as the light
    // goes rather than the instant it crosses zero. v1 swaps its flyer band on
    // the sun vector's own sign at -0.06, which is this angle.
    bool isNight() const { return world_.sky.elevationDeg() < -3.4f; }

    bool applySun(bool force) {
        const float el = clock_.elevationDeg();
        const float az = clock_.azimuthDeg();
        // THE PHASE IS PART OF THE "HAS ANYTHING CHANGED" TEST. It runs on its
        // own clock, hundreds of times slower than the sun.s elevation, so
        // leaving it out meant the moon only ever changed face when the sun
        // happened to move enough -- and never at all on a paused night.
        // Scaled up because a phase delta is tiny in absolute terms.
        const float ph = opt_.moonPhaseGiven
                             ? opt_.moonPhase
                             : (clock_.days + clock_.tday) / Sky::MOON_PERIOD_DAYS;
        const float moved =
            fabsf(el - sunEl_) + fabsf(az - sunAz_) + fabsf(ph - moonPh_) * 720.0f;
        if (!force && moved < 0.02f) return false;
        sunEl_ = el;
        sunAz_ = az;
        moonPh_ = ph;
        world_.sky.setMoonPhase(ph * Sky::MOON_PERIOD_DAYS);
        world_.sky.setSun(az, el);
        // AFTER setSun, because it needs the direction setSun just computed --
        // and then setSun again, because the colour it produces is baked by the
        // fit rather than read per frame. Two rebuilds only on the frames the
        // sun actually moved, which applySun has already established.
        if (syncSunToSky()) world_.sky.setSun(az, el);
        return true;
    }

    // -----------------------------------------------------------------------
    // The night level, fanned out to the two lights it is a master over.
    //
    // BOTH OF THEM HAVE TO BE PUSHED, and neither pushes itself on a paused
    // night -- which is exactly the state somebody tuning this is in. The
    // moon's key reaches the GPU only through setSun, and the floor only
    // through a sky-view rebuild that is skipped on every frame the sun has not
    // moved. Without these two lines a paused midnight would take the new value
    // and go on showing the old picture until something else moved the sun.
    void applyNightLevel() {
        world_.sky.moonKeyScale = moonKeyBase_ * nightLevel_;
        atmo_.nightFloor = nightFloorBase_ * nightLevel_;
        applySun(true);
        atmo_.invalidate();
        invalidate();
    }

    // -----------------------------------------------------------------------
    // Opening the menu hands the mouse back, and closing it takes it again if
    // it had it. A menu you can see but not point at is worse than no menu.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHERE A TAKE OR A SCREENSHOT GOES: the recordings folder, not the
    // working directory.
    //
    // v2.bat deliberately runs the exe from the repo root so that output lands
    // "next to the launcher where it can be found", and for one screenshot that
    // was right. It stops being right the moment the recorder exists: a
    // afternoon of takes and shots buries the repo root in v2_take_004.mp4 and
    // has to be swept up by hand. recordings/ already existed for exactly this
    // sort of thing.
    //
    // AND IT NEVER OVERWRITES. The counters start at zero every run, so before
    // this a second session quietly wrote over the first one's v2_take_000.mp4
    // -- which mattered little when the file was in your face at the repo root
    // and matters a great deal once takes accumulate somewhere tidy. The index
    // walks forward until it finds a name nobody is using.
    //
    // Falling back to the working directory if the folder cannot be made:
    // losing a recording because a directory was read-only would be a worse
    // failure than putting it in the wrong place.
    // -----------------------------------------------------------------------
    static std::string outputPath(const char *fmt, int *counter) {
        std::error_code ec;
        const bool dir = std::filesystem::exists("recordings", ec) ||
                         std::filesystem::create_directories("recordings", ec);
        char name[64];
        for (int guard = 0; guard < 10000; ++guard) {
            std::snprintf(name, sizeof(name), fmt, *counter);
            ++*counter;
            std::string path = dir ? (std::string("recordings/") + name) : std::string(name);
            if (!std::filesystem::exists(path, ec)) return path;
        }
        return dir ? (std::string("recordings/") + name) : std::string(name);
    }

    // mm:ss for the REC badge. Not the editor's timecode helper: that one
    // carries tenths, which on a badge that is already pulsing is a digit
    // flickering in the corner of the eye for no information at all.
    static std::string clockLabel(double seconds) {
        const int t = int(seconds < 0.0 ? 0.0 : seconds);
        char b[16];
        std::snprintf(b, sizeof(b), "%02d:%02d", t / 60, t % 60);
        return b;
    }

    // -----------------------------------------------------------------------
    // R. One key, two states: the second press stops, and the file is written.
    // -----------------------------------------------------------------------
    void toggleRecording() {
        if (recorder_.recording()) {
            recorder_.stop();
            std::printf("v2: recording stopped -- encoding\n");
            std::fflush(stdout);
            return;
        }
        // Still writing the last one. Refuse rather than queue: two sink
        // writers and two encoder threads for one hardware encoder is a way to
        // make both takes worse.
        if (recorder_.busy()) {
            std::printf("v2: still finishing the last take\n");
            std::fflush(stdout);
            return;
        }

        if (tracer_.displayWidth() <= 0) return;

        const std::string take = outputPath("v2_take_%03d.mp4", &takeIndex_);
        const char *name = take.c_str();
        const double nowSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (!recorder_.start(name, tracer_.displayWidth(), tracer_.displayHeight(),
                             opt_.recMaxWidth, opt_.recFps, 1, nowSec)) {
            std::fprintf(stderr, "v2: could not start recording\n");
            return;
        }
        std::printf("v2: recording to %s -- %dx%d @ %d fps (R again to stop)\n", name,
                    recorder_.captureWidth(), recorder_.captureHeight(), opt_.recFps);
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // A take that has finished encoding.
    //
    // THE FILE IS THE WHOLE PRODUCT. It is already written, already named on
    // stdout by the recorder, and sitting next to v2.bat where anything else
    // can pick it up -- so there is nothing for an editor to be the gateway
    // to. Stopping a take costs one keystroke and takes nothing away: no
    // panel to dismiss, no cursor handed back and forth, no camera parked
    // while a modal window is up. All that is left is to say it happened.
    // -----------------------------------------------------------------------
    void onTakeSaved(const vb::Take &take, double nowSec) {
        savedTake_ = take;
        savedAt_ = nowSec;
    }

    // -----------------------------------------------------------------------
    // THE CONSOLE, AND WHAT /locate DOES.
    //
    // The biomes are bands running north-south and repeating forever, so every
    // one of them is somewhere specific and "take me there" is a well-posed
    // request: walk east from the pine band's centre and you reach the birch
    // band's. /locate finds the NEAREST band of the named kind rather than a
    // fixed coordinate, so it is a short hop from wherever you are standing
    // instead of a trip back to the origin.
    //
    // ADDING A BIOME IS ONE ROW IN THIS TABLE. That is the point of writing it
    // as a table at all -- the parser, the completion in the error message and
    // the teleport all read it, so a new wood cannot be half-registered.
    // -----------------------------------------------------------------------
    struct BiomeName { const char *name; const char *alias; Biome biome; };
