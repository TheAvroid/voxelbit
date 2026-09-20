// app_load.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: onLoad: the whole world/device/asset bring-up
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // EVERY MODEL THE PALETTE MUST RESERVE FOR BEFORE THE WOOD IS SCATTERED.
    //
    // A function rather than the braced list it used to be, and the pistol is
    // why: its colours are not all in one file. See the note at the call site
    // for which paths are here and on what argument, and HeldItem::prewarmColors
    // for what the reservation is worth.
    //
    // THE PISTOL'S RELOAD FRAMES ARE IN IT, which no other entry needs. Frames
    // 04 to 07 are the magazine out of the grip and they carry two shades --
    // the follower and the base plate -- that the gun at rest does not have
    // anywhere on it, and a colour first asked for at kit-build time is asked
    // for after the whole world has had its turn at a 255-entry table. The
    // rifle has no such problem: its nine reload frames are strict subsets of
    // the rest pose, measured at "worst colour shift 0/255" on all ten models.
    // See [[v2-palette-is-full]].
    std::vector<std::string> kitPrewarm() const {
        std::vector<std::string> out = {opt_.axe,   opt_.pick,  opt_.shovel, opt_.arrow,
                                        opt_.steak, opt_.seeds, opt_.rifle,  opt_.pistol};
        for (const std::string &p : HeldItem::stripFiles(opt_.pistolReload)) out.push_back(p);
        return out;
    }

    // -----------------------------------------------------------------------
    void onLoad(Falcor::RenderContext *ctx) override {
        // Before anything slow, so the window is where it belongs while the
        // wood is still loading rather than jumping there afterwards.
        if (!opt_.background) restoreWindowPlacement();

        world_.seed = opt_.r.seed;
        // THE BIOME IS SET BEFORE ANYTHING READS THE TERRAIN, and that
        // ordering is load-bearing: loadPines() asks terrain.birch() to decide
        // which species to load, and the chunk mesher is handed a COPY of the
        // terrain when it starts its workers. Set it late and half the engine
        // has already been told it is a pine wood.
        world_.terrain.forced = opt_.birch || opt_.pineOnly || opt_.oakOnly ||
                                opt_.cherryOnly || opt_.desertOnly;
        world_.terrain.biome = opt_.birch         ? Biome::Birch
                               : opt_.oakOnly     ? Biome::Oak
                               : opt_.cherryOnly  ? Biome::Cherry
                               : opt_.desertOnly  ? Biome::Desert
                                                  : Biome::Pine;
        showCoords_ = opt_.coords;
        world_.terrain.grassDensity = clampf(opt_.grass, 0.0f, 1.0f);
        world_.flowerDensity = clampf(opt_.flowers, 0.0f, 1.0f);
        world_.rockDensity = clampf(opt_.rocks, 0.0f, 1.0f);
        world_.pineconesPerTree = maxi(0, opt_.pineconesPerTree);
        world_.treeDensity = clampf(opt_.treeDensity, 0.0f, 1.0f);
        world_.oakDensity = clampf(opt_.oakDensity, 0.0f, 1.0f);
        world_.pineDir = opt_.pines;
        world_.decorDir = opt_.decor;
        // THE CEILING IS A MEMORY CEILING, NOT A DESIGN ONE. It was 15,
        // inherited from v7 with no note saying why, and it made `--view 24`
        // silently mean 15 -- so raising the view distance appeared to cost
        // nothing because nothing had changed.
        //
        // What it costs, per `rering`'s disc and the TriPool sizing in build():
        //
        //   R   radius    chunks   TriPool
        //   12  307 m     441      310 MB
        //   15  384 m     709      477 MB
        //   24  614 m     1793     1191 MB
        //
        // The pool is allocated whole at build() and the BLAS memory that
        // scales beside it is the larger term, so this is the one setting that
        // can exhaust a 12 GB card on its own. The spill is silent and reads
        // as LOWER local VRAM, not higher -- see the memory note.
        world_.viewChunks = mini(24, maxi(1, opt_.view));
        world_.terrain.grassMinRows = maxi(1, opt_.grassMin);
        world_.terrain.grassMaxRows = maxi(opt_.grassMin, opt_.grassMax);
        // A stream of its own, so re-seeding the wood does not also reshuffle
        // every blade of grass in it -- the two are independent things to want
        // varied.
        world_.terrain.strandSeed = opt_.r.seed + 991u;

        // THE DEM GOES ON BEFORE THE BUILD, for the same reason the scales do:
        // build() generates chunks, and a chunk generated against the invented
        // landform does not come back when the height field changes under it.
        if (!opt_.demPath.empty()) {
            if (world_.terrain.loadDem(opt_.demPath, opt_.demBaseM,
                                       opt_.demScale, opt_.demExag)) {
                world_.terrain.demDetailM = opt_.demDetail;
                world_.terrain.demRoughM = opt_.demRough;
                // The patch goes on AFTER the DEM, because it is rebased onto
                // whatever the DEM says locally -- see world/inset.h.
                if (!opt_.insetPath.empty()) {
                    if (world_.terrain.inset.load(opt_.insetPath, opt_.insetX,
                                                  opt_.insetZ, opt_.demScale))
                        printf("[inset] %s  %dx%d at %.3f world m, %.0f m across,"
                               " centred (%.0f, %.0f)\n",
                               opt_.insetPath.c_str(), world_.terrain.inset.w(),
                               world_.terrain.inset.h(),
                               world_.terrain.inset.stepWorldM(),
                               world_.terrain.inset.spanWorldM(),
                               opt_.insetX, opt_.insetZ);
                    else
                        printf("[inset] FAILED %s -- %s\n", opt_.insetPath.c_str(),
                               world_.terrain.inset.err());
                }
                world_.terrain.stemDiv = opt_.stemDiv;
                const DemField &d = world_.terrain.dem();
                printf("[dem] %s  %dx%d  %.1f..%.1f m asl  relief %.1f m\n"
                       "      1 world m = %.1f real m, vert x%.2f  ->  %.2f x %.2f km world, %.0f m of relief\n",
                       opt_.demPath.c_str(), d.w(), d.h(), d.minM(), d.maxM(),
                       d.reliefM(),
                       d.shrink(), d.exag(),
                       d.spanX() / 1000.0f, d.spanZ() / 1000.0f,
                       d.aslToWorld(d.maxM()) - d.aslToWorld(d.minM()));
                // THE DEFAULT SPAWN IS QUOTED IN REAL METRES FROM THE WINDOW
                // CENTRE, so it names a PLACE in Colorado rather than a world
                // coordinate. Shrinking the dataset moves every world
                // coordinate but not the place, so it is converted here. Miss
                // this and --dem-scale 6 throws the default spawn six times
                // further out than the window is wide, onto the flat held-edge
                // ground outside the data, which looks like the DEM failed to
                // load. An explicit --cam-x/--cam-z is left alone: that one is
                // a world coordinate because the user typed it as one.
                if (!opt_.camGiven && d.shrink() != 1.0f) {
                    opt_.camX /= d.shrink();
                    opt_.camZ /= d.shrink();
                }
                // AFTER THE DEM, ALWAYS -- loadCover is handed the terrain's
                // shrink, so a cover loaded first would be indexed at 1:1 and
                // put every tree in the wrong place.
                // BEFORE loadCover, because the ground ramp below is only
                // published into the palette when the imagery is trusted for
                // the ground -- see VoxelTerrain::coverGround.
                world_.terrain.coverGround = opt_.coverGround;
                if (!opt_.coverPath.empty()) {
                    if (world_.terrain.loadCover(opt_.coverPath)) {
                        const CoverField &cv = world_.terrain.cover();
                        // BEFORE World::build, ALWAYS. A colour minted after it
                        // renders flat 170-grey until the table is uploaded
                        // again, and that failure looks like the ramp was never
                        // set at all.
                        // ...AND ONLY IF THE GROUND IS THE PICTURE'S. The ramp is
                        // built from the window's bare-ground pixels, so under
                        // --cover-water it would paint mat::GROUND_0..9 with
                        // colours nothing is ever going to ask for -- and on a
                        // window the classifier misreads, those ten entries are
                        // the lavender the rock came out as.
                        if (opt_.coverGround)
                            world_.palette.setGroundBand(cv.ramp(), CoverField::kRamp);
                        printf("[cover] %s  %dx%d  ground ramp #%02x%02x%02x .. #%02x%02x%02x\n",
                               opt_.coverPath.c_str(), cv.w(), cv.h(),
                               cv.ramp()[0], cv.ramp()[1], cv.ramp()[2],
                               cv.ramp()[27], cv.ramp()[28], cv.ramp()[29]);
                    } else {
                        printf("[cover] not loaded: %s\n",
                               world_.terrain.cover().err());
                    }
                }
                // ------------------------------------ AFTER BOTH, NOT AFTER ONE
                // This has now been in the wrong place twice. It started in
                // the DEM block, which runs BEFORE the cover loads: the lake
                // pass saw no water and the index came out as twelve summits
                // and nothing else. Moving it into the cover block fixed the
                // lakes and broke the peaks -- a window loaded with no
                // --cover then got no places at all, when a summit needs only
                // the heights. It belongs after BOTH, taking whatever each
                // one has. Same ordering trap as the cover load itself: that
                // one needs the shrink, this one needs the water.
                poi_.build(world_.terrain.dem(), world_.terrain.cover());
                printf("[poi] %d places in the data (%d named)\n",
                       int(poi_.all().size()), poi_.namedCount());
            } else {
                printf("[dem] FAILED to load %s: %s\n", opt_.demPath.c_str(),
                       world_.terrain.dem().err());
            }
        }
        world_.sky.turbidity = opt_.turbidity;
        // THE SCALES GO ON BEFORE THE BUILD, not after. setSun below is what
        // rebuilds the sky, and anything applied to these after it has run is a
        // value the GPU never sees on the offline path -- which never rebuilds
        // again because it never starts the clock.
        if (opt_.moonScaleGiven) world_.sky.moonScale = opt_.moonScale;
        if (opt_.moonKeyGiven) world_.sky.moonKeyScale = opt_.moonKey;

        // AND THE NIGHT LEVEL ON TOP OF THEM, here for the reason the comment
        // above gives: the offline path never rebuilds the sky, so a scale
        // applied after setSun is a scale the GPU never sees. What is captured
        // first is the BASE -- whatever the command line just said -- so
        // --moon-key goes on meaning "the moon at 1.0x" and this stays a master
        // over it rather than a second opinion about it.
        nightLevel_ = maxf(0.0f, opt_.nightBrightness);
        moonKeyBase_ = world_.sky.moonKeyScale;
        world_.sky.moonKeyScale = moonKeyBase_ * nightLevel_;

        // THE PHASE HAS TO BE SET HERE TOO. This is the branch the OFFLINE path
        // takes -- it never starts the clock, so applySun (where the phase
        // normally comes from) never runs and the moon would be stuck full in
        // every --out render however --moon-phase was set.
        world_.sky.setMoonPhase(opt_.moonPhase * Sky::MOON_PERIOD_DAYS);
        world_.sky.setSun(opt_.sunAz, opt_.sunEl);

        // SOMEWHERE NEW EACH TIME, but only in the viewer. --out renders one
        // frame from a named camera and the whole point of it is that the same
        // flags give the same picture, so it keeps the camera it was given.
        if ((!opt_.outGiven || opt_.spawnPick) && !opt_.camGiven) chooseSpawn();
        if (opt_.groundStats) { groundStats(); skyStats(); shutdown(0); return; }

        // BEFORE THE MODELS LOAD, because the load is the only moment their
        // voxels exist and it throws them away when it is done.

        auto t0 = std::chrono::steady_clock::now();
        if (!world_.build(getDevice(), ctx)) {
            shutdown(1);
            return;
        }
        std::printf("  models   %.2f s -- %d pines, %d rocks, %d flowers, %d mushrooms, "
                    "%d materials\n",
                    secondsSince(t0), world_.loadedPines, world_.loadedRocks,
                    world_.loadedFlowers, world_.loadedMushrooms, world_.palette.used());
        std::printf("           %.2f M unique tris in %d models at %.0f cm voxels\n",
                    world_.uniqueTris / 1e6,
                    world_.loadedPines + world_.loadedRocks + world_.loadedFlowers +
                        world_.loadedMushrooms,
                    VOXEL_M * 100.0f);
        world_.reportVolumes();

        // Fill the ring before the first frame, so nobody sees a hole in the
        // ground while the workers catch up.
        t0 = std::chrono::steady_clock::now();
        const float gy = world_.terrain.heightM(opt_.camX, opt_.camZ);
        world_.primeBlocking(Vec3(opt_.camX, gy + opt_.eye, opt_.camZ));
        std::printf(
            "  world    %.2f s -- %zu chunks of %.1f m resident, %.1f M tris, %zu instances\n",
            secondsSince(t0), world_.chunkCount(), CHUNK_M, world_.residentTris() / 1e6,
            world_.instanceCount());

        // AFTER the ring is resident, because a collider only exists once the
        // chunk that owns it has been adopted.
        if (opt_.collideProbe) { collideProbe(); shutdown(0); return; }
        // The hive count is only interesting in the birch wood, and printing
        // "0 hives" in the pine one would read as a fault rather than as a
        // species that does not have them.
        // TREES, not pines -- the ring can hold both species at once now, and
        // near a seam it usually does.
        // ...and the FRUIT. It matters only in the oak wood, for the reason the
        // hive count matters only in the birch one -- but it is the one number
        // that says whether the orchard pass ran at all, so it prints beside the
        // rest rather than behind a biome test.
        std::printf("           %zu trees, %zu rocks, %zu flowers, %zu mushrooms,"
                    " %zu pinecones, %zu beehives, %zu fruit\n",
                    world_.decorCount(0), world_.decorCount(1), world_.decorCount(2),
                    world_.decorCount(3), world_.decorCount(4), world_.decorCount(5),
                    world_.decorCount(6));
        // ...AND WHERE ONE OF THEM IS. A crop hangs inside a crown and is
        // 40 cm across, so "did the orchard run" is answered by the count and
        // "does it look right" only by standing under one.
        if (world_.decorCount(6)) {
            const Vec3 f = world_.firstFruit();
            std::printf("           first fruit at %.0f, %.0f, %.0f\n",
                        double(f.x), double(f.y), double(f.z));
        }
        // THE RING'S CENTRE, not pos_ -- the player is placed further down and
        // pos_ is still the origin here, which printed "0, 0" from wherever you
        // actually were. opt_ is what the world was built around.
        std::printf("           the %s wood at %.0f, %.0f%s\n",
                    world_.terrain.woodName(opt_.camX), opt_.camX,
                    opt_.camZ, world_.terrain.forced ? " (pinned)" : " -- T, /locate");
        std::printf("           %.0f ms of that was structure building, %.0f of %.0f MB of tri pool used\n",
                    world_.buildMs(), double(world_.poolUsedBytes()) / (1024.0 * 1024.0),
                    double(world_.poolBytes()) / (1024.0 * 1024.0));

        // -- Streamline: Frame Generation and Reflex -------------------------
        //
        // preInit already ran in main(), before the device existed -- that is
        // the half that has to be early. This half needs the device, so it is
        // here, and it is what asks the driver FEATURE BY FEATURE what this
        // adapter can actually do. Anything it refuses is simply not offered in
        // the menu; a 20-series card upscales but cannot generate frames, and
        // one binary has to be correct on both.
        if (sl_.init(getDevice())) {
            // One line naming each feature, because "Streamline is up" is not
            // the useful fact -- WHICH of these this adapter actually admitted
            // to is. Frame generation is 40-series and up and needs Reflex;
            // super resolution goes back to the 20-series.
            std::printf("  stream   super res %s, frame gen %s, reflex %s\n",
                        sl_.hasSuperResolution() ? "yes" : "no",
                        sl_.hasFrameGeneration() ? "yes" : "no",
                        sl_.hasReflex() ? "yes" : "no");
            // THE PER-FEATURE ANSWERS, which the summary above cannot carry.
            // Streamline distinguishes "this GPU cannot" from "this driver
            // cannot" from "this SDK ships no plugin for it", and only the
            // numeric refusal tells them apart -- which matters the moment you
            // ask whether something like frame warp is reachable at all.
            std::fputs(sl_.featureReport().c_str(), stdout);
        } else if (!opt_.outGiven) {
            std::printf("  stream   unavailable: %s\n", sl_.status().c_str());
        }
        std::fflush(stdout);

        // RTX NEURAL SHADING, ASKED BEFORE THE TRACER IS BUILT.
        //
        // The order here is load-bearing, and it was not at first. The radiance
        // cache lives INSIDE the trace shader, and cooperative vectors do not
        // exist on a D3D12 target below Shader Model 6.10. A tracer compiled
        // without knowing the answer does not quietly lose the cache -- the
        // whole shader fails to compile and the engine dies at startup on an
        // error that mentions nothing about neural anything. So the capability
        // is established first and the tracer is told. See gpu/neural.h.
        if (neural_.init(getDevice()))
            std::printf("  neural   %s\n", neural_.status().c_str());

        else
            std::printf("  neural   unavailable: %s\n", neural_.status().c_str());

        std::fflush(stdout);

        sharc_.voxelKey = !opt_.sharcHashGrid;
        sharc_.statsOn = opt_.sharcStats;
        if (opt_.sharcStale > 0) sharc_.staleFrames = uint32_t(opt_.sharcStale);
        sharc_.setCapacity(uint32_t(maxi(0, opt_.sharcEntries)));
#if V2_HAS_NRCSDK
        // NVIDIA'S CACHE, AND IT HAS TO COME BEFORE tracer_.init.
        //
        // Whether this is on decides which PROGRAMS the tracer compiles: the
        // NRC buffers only exist in a build that defined V2_NRCSDK. Bring it up
        // afterwards and the host binds five resources the shader never
        // declared -- "No member named 'gNrcQueryPathInfo' found", at the first
        // dispatch rather than at startup.
        if (opt_.nrcSdk) {
            nrcSdk_.enabled = true;
            nrcSdk_.debugMode = opt_.nrcSdkDebug;
            nrcSdk_.maxRadiance = opt_.nrcSdkRadiance;
            if (nrcSdk_.init(getDevice()))
                std::printf("  nrc sdk  %s\n", nrcSdk_.status().c_str());
            else
                std::printf("  nrc sdk  unavailable: %s\n", nrcSdk_.status().c_str());
            std::fflush(stdout);
            if (nrcSdk_.available()) tracer_.setNrcSdk(&nrcSdk_);
            tracer_.nrcSdkResolveDebug = (opt_.nrcSdkDebug == 99);
        }
#endif

        tracer_.init(getDevice(), &world_, neural_.available(), !opt_.sharcHashGrid,
                     !opt_.nrcFreqEncoding);
        makeCrosshair();
        makeVitalsPasses();   // the two bars and the death curtain

        // A denoiser that takes the program down when a driver is old is worse
        // than no denoiser, so this is allowed to fail and say so. Everything
        // below carries on with the accumulator.
        if (opt_.dlss && !opt_.outGiven) {
            if (dlss_.init(getDevice(), Falcor::getRuntimeDirectory())) {
                std::printf("  dlss     ray reconstruction ready, %s\n",
                            dlssQualityName(opt_.dlssQuality));
            } else {
                std::printf("  dlss     unavailable: %s\n"
                            "           falling back to accumulation\n",
                            dlss_.status().c_str());
                opt_.dlss = false;
            }
            std::fflush(stdout);
        }

        // WHICH STREAMLINE MODULES ACTUALLY LOADED. The interposer reports
        // itself available long before its plugins are in memory, so this is
        // the difference between "Streamline is up" and "the thing that
        // generates frames is up".
        if (opt_.stats)
            std::printf("%s", Streamline::moduleReport().c_str());

        // FRAME GENERATION HAS TO BE PRIMED HERE, and leaving it out is why the
        // tagging never ran: the per-frame block is guarded on
        // frameGeneration() != Off, and nothing else ever moved it off Off, so
        // the guard was false forever and DLSS-G reported "nothing tagged yet"
        // while cheerfully claiming to be available.
        //
        // The sizes are a guess at this point -- the tracer has not been sized,
        // and under DLSS the traced size is the SDK's choice -- so the frame
        // loop re-applies them the first time the real ones are known.
        if (opt_.frameGen != FrameGen::Off) {
            if (sl_.hasFrameGeneration()) {
                const uint2 out{uint32_t(opt_.r.width), uint32_t(opt_.r.height)};
                if (sl_.setFrameGeneration(opt_.frameGen, out, out))
                    std::printf("  sl       frame generation %s\n",
                                frameGenName(opt_.frameGen));
                else
                    std::printf("  sl       frame generation refused: %s\n",
                                sl_.status().c_str());
            } else {
                std::printf("  sl       frame generation asked for but unavailable\n");
            }
            std::fflush(stdout);
        }

        // RTX MEGA GEOMETRY, asked of the driver rather than guessed from the
        // adapter name.
        // THE SELF TEST IS OPT-IN, and that is not caution for its own sake:
        // it currently REMOVES THE DEVICE. The capability query is sound and
        // costs nothing, so it still runs; actually issuing a cluster build is
        // behind --cluster-test until the descriptor bug is found. An engine
        // that cannot start is worse than one without clusters.
        clusters_.setBisect(opt_.clusterTest);
        if (clusters_.init(getDevice()) && (!opt_.clusterTest || clusters_.selfTest(ctx)))
            std::printf("  clusters %s%s\n", clusters_.status().c_str(),
                        opt_.clusterTest ? " -- self test passed" : " (build path untested)");
        else
            std::printf("  clusters unavailable: %s\n", clusters_.status().c_str());
        // -- WHAT CARD THIS IS, AND WHETHER IT HAS THE ROOM ------------------
        //
        // (user 2026-09-17: "when booting up the game, can you detect for the
        //  gpu that the player has if you dont already do this already.")
        //
        // MOSTLY, ALREADY, AND IN MORE DETAIL THAN A NAME. main.cpp prints the
        // adapter and its API before this report starts, and the eight lines
        // above it are the real answer to "what can this machine do": DLSS
        // super resolution, frame generation and Reflex, SM 6.10 and
        // cooperative vectors for the neural path, cluster operations, the
        // PhysX solver, CUDA interop and RTXGI. Each says available or says WHY
        // not, which is the part a name cannot tell you.
        //
        // WHAT WAS MISSING IS THE MEMORY, and on this engine that is the number
        // that decides whether it runs. v2 holds ~7 GB of VRAM with the world
        // resident -- 114 MB of model volumes, a 67 M triangle ring and its
        // structures -- so a card's capacity is not a detail, and Falcor's
        // Device::Info carries only a name. DXGI has it, and the native handle
        // is already reached for two lines up in clusters.h for NVAPI.
        //
        // REPORTED, NOT ENFORCED. The minimum spec is a 4070 and this does not
        // refuse to start on anything -- a number in the log is what makes a
        // report from a machine nobody here owns readable.
        {
            ID3D12Device *d3d = getDevice()->getNativeHandle().as<ID3D12Device *>();
            IDXGIFactory4 *fac = nullptr;
            IDXGIAdapter3 *ad3 = nullptr;
            DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
            bool got = false;
            if (d3d && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&fac))) {
                IDXGIAdapter1 *ad1 = nullptr;
                const LUID luid = d3d->GetAdapterLuid();
                if (SUCCEEDED(fac->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1),
                                                     (void **)&ad1)) && ad1) {
                    if (SUCCEEDED(ad1->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&ad3)) &&
                        ad3) {
                        got = SUCCEEDED(ad3->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm));
                        ad3->Release();
                    }
                    ad1->Release();
                }
                fac->Release();
            }
            if (got)
                std::printf("  gpu      %s -- %.1f GB of video memory, %.1f GB in use\n",
                            getDevice()->getInfo().adapterName.c_str(),
                            double(vm.Budget) / (1024.0 * 1024.0 * 1024.0),
                            double(vm.CurrentUsage) / (1024.0 * 1024.0 * 1024.0));
            else
                std::printf("  gpu      %s -- video memory unreported by DXGI\n",
                            getDevice()->getInfo().adapterName.c_str());
        }
        if (opt_.clusterTest)
            std::printf("           bisect: %s\n", clusters_.bisectNote().c_str());
            std::printf("           build:  %s\n", clusters_.buildNote().c_str());
        std::fflush(stdout);

        // PHYSX 5. Beside collide.h rather than instead of it -- see
        // physics/physics.h for why the hand-written collision stays.
        if (physics_.init())
            std::printf("  physx    %s\n", physics_.status().c_str());
        else
            std::printf("  physx    unavailable: %s\n", physics_.status().c_str());
        std::fflush(stdout);

        // CUDA, and whether it really shares memory with the renderer.
        if (cuda_.init(getDevice()))
            std::printf("  cuda     %s\n", cuda_.status().c_str());
        else
            std::printf("  cuda     unavailable: %s\n", cuda_.status().c_str());
        std::fflush(stdout);

        // VOLUMETRIC FOG, replacing the analytic height fog entirely.
        if (opt_.fogAnisoGiven) volfog_.anisotropy = opt_.fogAniso;
        if (opt_.fogAmbientGiven) volfog_.ambient = opt_.fogAmbient;
        if (opt_.fogSkyUnderGiven) volfog_.skyShadow = opt_.fogSkyUnder;
        if (opt_.flareGiven) tracer_.flare = opt_.flare;
        tracer_.blueNoise = opt_.blueNoise;
        tracer_.post().autoExposure = opt_.autoExposure;
        tracer_.post().bloom = opt_.bloom;
        if (opt_.bloomThresholdGiven) tracer_.post().bloomThreshold = opt_.bloomThreshold;
        if (opt_.expKeyGiven) tracer_.post().expKey = opt_.expKey;
        if (volfog_.init(getDevice())) {
            tracer_.setVolFog(&volfog_);
            std::printf("  fog      volumetric, %s\n", volfog_.status().c_str());
        } else {
            std::printf("  fog      unavailable: %s\n", volfog_.status().c_str());
        }

        // THE CLOUD DECK. Sky.slang is Preetham -- a closed-form clear-sky
        // model with no volume in it anywhere -- so clouds cannot live in the
        // sky function and need a medium of their own. Ported from the WebGPU
        // game in src/render/wgsl/cloudgen.js; see Clouds.slang.
        if (opt_.cloudCutGiven) clouds_.cut = opt_.cloudCut;
        if (opt_.cloudVarGiven) clouds_.regVar = opt_.cloudVar;
        if (opt_.cloudSunGiven) clouds_.sunStrength = opt_.cloudSun;
        if (opt_.cloudMoonKeyGiven) clouds_.moonStrength = opt_.cloudMoonKey;
        // THE MOON SCALES ARE NOT RE-APPLIED HERE, and they used to be. Setting
        // the same value twice from the same flag cannot be seen, so the second
        // copy survived as a no-op -- but it stops being one the moment
        // anything sits between the two, and the night level now does. Left in
        // place it would quietly undo the multiplier on the viewer path while
        // leaving it standing on the offline one, which is the worst of both.
        if (clouds_.init(getDevice())) {
            tracer_.setClouds(&clouds_);
            std::printf("  clouds   %s\n", clouds_.status().c_str());
        } else {
            std::printf("  clouds   unavailable: %s\n", clouds_.status().c_str());
        }
        std::fflush(stdout);


        // THE ATMOSPHERE, and the tracer is told about it either way. Its
        // textures exist even when its shaders did not compile -- see the note
        // on the ordering in Atmosphere::init -- precisely so that the binding
        // in tracer.h can be unconditional and cannot itself be what breaks.
        const bool atmoOk = atmo_.init(getDevice());
        atmo_.enabled = opt_.atmosphere && atmoOk;
        if (opt_.nightFloorGiven) atmo_.nightFloor = opt_.nightFloor;
        // The floor's base, captured after its flag for the same reason the
        // moon's was: --night-floor names the level at 1.0x and the night level
        // scales what it names. No invalidate needed, unlike the menu row that
        // writes these same two fields -- nothing has been built from it yet,
        // the sky-view table is baked on the first update.
        nightFloorBase_ = atmo_.nightFloor;
        atmo_.nightFloor = nightFloorBase_ * nightLevel_;
        tracer_.setAtmosphere(&atmo_);
        std::printf("  sky      %s\n",
                    !atmoOk ? atmo_.status().c_str()
                            : (atmo_.enabled
                                   ? "Hillaire scattering -- Rayleigh, Mie, ozone"
                                   : "Preetham fit (--no-atmosphere); Hillaire tables ready"));
        std::fflush(stdout);

        // THE RADIANCE CACHE, only where the hardware can actually run it.
        // Cooperative vectors are the whole mechanism, so this follows neural_
        // exactly -- there is no fallback path and pretending otherwise would
        // just move the failure later.
        if (neural_.available()) {
            nrc_.enabled = opt_.nrc;
            nrc_.training = !opt_.nrcFrozen;
            if (opt_.nrcLr > 0.0f) nrc_.learningRate = opt_.nrcLr;
            if (nrc_.init(getDevice(), kNrcMaxSamples, !opt_.nrcFreqEncoding))
                { std::printf("  cache    neural radiance cache ready (%u weights, %s)\n",
                              NrcLayout::kElems,
                              opt_.nrcFreqEncoding ? "frequency encoding"
                                                   : "voxel features");
                  if (!opt_.nrcLoad.empty()) {
                      std::string why;
                      if (nrc_.loadWeights(opt_.nrcLoad, &why))
                          std::printf("  cache    loaded %s -- %u batches already trained\n",
                                      opt_.nrcLoad.c_str(), nrc_.batches());
                      else
                          std::printf("  cache    could NOT load %s: %s\n",
                                      opt_.nrcLoad.c_str(), why.c_str());
                  }
                  tracer_.setNrc(&nrc_); }
            else
                std::printf("  cache    unavailable: %s\n", nrc_.status().c_str());
            std::fflush(stdout);
        }

        // RESAMPLED INDIRECT. The reservoirs and both resampling passes have
        // been in the tree since Phase C, and until now nothing constructed
        // one: tracer_.setRestir() was never called, so restir_ stayed null,
        // restirMode was always 0 and the passes never ran. Off unless asked
        // for, because it is a different noise character rather than a strict
        // improvement -- see --restir.
        restir_.enabled = opt_.restir;
        restir_.world = !opt_.noRestirWorld;
        restir_.worldMaxM = opt_.restirWorldM;
        if (restir_.init(getDevice())) {
            std::printf("  restir   %s%s\n", opt_.restir ? "on" : "available (--restir)",
                        restir_.world ? ", world-space reservoirs" : "");
            tracer_.setRestir(&restir_);
        } else {
            std::printf("  restir   unavailable: %s\n", restir_.status().c_str());
        }
        std::fflush(stdout);

        // THE IRRADIANCE PROBES. On by default wherever they can run, because
        // what they fix is not a nicety: under a canopy the indirect term is
        // most of the light there is, and a path tracer finds it only by
        // surviving roulette long enough to bounce its way back out to the sky.
        //
        // D3D12 only -- RTXGI's D3D12 backend is what v2 links -- so on Vulkan
        // this reports why and the cache mode drops to none.
        if (ddgi_.init(getDevice(), Falcor::getRuntimeDirectory() / "shaders" / "v2")) {
            tracer_.setDdgi(&ddgi_);
            std::printf("  probes   DDGI, %d probes x %d rays a frame\n", ddgi_.numProbes(),
                        ddgi_.raysPerProbe());
        } else {
            std::printf("  probes   unavailable: %s\n", ddgi_.status().c_str());
            // FALL THROUGH TO SHaRC RATHER THAN TO NOTHING. The probes are
            // D3D12-only, and the Vulkan path is exactly the one that has
            // already lost DLSS, Streamline and Ray Reconstruction -- so it is
            // the path that can least afford to lose the indirect fill as well.
            if (opt_.r.giMode == 1) opt_.r.giMode = 2;
        }
        std::fflush(stdout);

        // THE HASH CACHE. Shader-only and backend-agnostic, so it comes up
        // wherever its headers were staged. It is the fallback above, and it can
        // also be asked for outright with --gi 2 on D3D12, where the two are a
        // real choice: probes interpolate and never have holes, the hash map is
        // exact where it has samples and empty where it does not.
        if (sharc_.init(getDevice())) {
            tracer_.setSharc(&sharc_);
            std::printf("  hash gi  SHaRC, %s\n", sharc_.status().c_str());
        } else {
            std::printf("  hash gi  unavailable: %s\n", sharc_.status().c_str());
            if (opt_.r.giMode == 2) opt_.r.giMode = 0;
        }
        std::fflush(stdout);

        tracer_.setQuality(opt_.dlssQuality);

        // -- THE KIT CLAIMS ITS COLOURS HERE, NOT WHERE IT LOADS -------------
        //
        // "the bow is broken, missing voxels" (user 2026-09-14). The palette
        // read 255 of 255 -- FULL on a plain start and the fourteen refused
        // calls were the bow's fourteen frames, each one distinct colour short.
        // The held kit loads LAST of everything in this function and colours are
        // served first-come, so it is the kit that goes without.
        //
        // THE SAME MOVE prewarmRoom MAKES, and for the same reason, three
        // hundred lines earlier than it because the queue in front of the tools
        // is now the whole flyer band. HeldItem::prewarmColors carries the
        // measurement and the argument; it takes colours only, so the models
        // themselves still load below where the pool and the structures exist.
        //
        // HERE and not above: deriveGroundFromTrees has run, and the note on
        // the flyer band below explains what registering ahead of it does.
        if (opt_.axeOn) {
            // EVERYTHING THE KIT HOLDS, and the four that were missing from
            // this list are why it had to grow: the hoe, the wheat, the seeds
            // and the STEAK all load in the same block as the axe, a few
            // hundred lines below, by which time the flyer band has taken the
            // colours. Measured the moment the steak was added -- "PALETTE
            // FULL, 2 of its colours came back AIR", which is precisely the
            // silent failure this reservation exists to prevent and which the
            // bow had already been through once.
            const int took = HeldItem::prewarmColors(
                world_,
                // THE SEEDS ARE IN THIS LIST AND THE WHEAT IS NOT, which is
                // not an oversight. What a late loader needs is not to be
                // BIG, it is to be UNSHAREABLE: the hoe is stone greys and the
                // wheat is tans, and both snap onto colours the wood already
                // owns for nothing. The seeds are three vivid greens that
                // nothing in a pine forest is within sixteen of, so they mint
                // three new entries or they go without -- measured, one of the
                // three came back AIR and the model lost a voxel.
                // ...AND THE RIFLE IS IN IT TOO, on the seeds' argument rather
                // than the hoe's. Its eleven shades are seven near-blacks
                // (43..64), three light greys and one red, registered EXACTLY
                // like the rest of the kit -- so none of them snaps onto a
                // colour the wood already owns, and all eleven mint or all
                // eleven go without. A gun that loads minutes after start-up
                // on [O] would be asking last; this is what stops that.
                // ...AND THE PISTOL, ON THE SEEDS' ARGUMENT AGAIN -- ten of its
                // thirteen shades are new, a seven-step grey ramp and three
                // browns that nothing in a pine forest is near, so they are
                // minted early or they are not minted at all. THE RELOAD
                // FRAMES ARE IN THE LIST TOO, which no other entry needs: four
                // of them carry two shades the rest pose does not have, and a
                // colour first asked for by frame 04 is a colour asked for
                // after the whole world has had its turn.
                kitPrewarm(),
                opt_.bow);
            std::printf("  held     reserved %d palette entries for the kit, %d of %d now used\n",
                        took, world_.palette.used(), int(mat::COUNT));
            std::fflush(stdout);
        }


        // -- and what is in the air (render/butterflies.h) -------------------
        //
        // AFTER the world, and it has to be: the models go into the same
        // palette, the same triangle pool and the same structures every rock
        // does, and none of those exist until the world has built them. It is
        // also after deriveGroundFromTrees for a subtler reason -- that samples
        // the entries the TREES minted to decide what a hillside is made of,
        // and a butterfly registered ahead of it would be a candidate for soil.
        //
        // AND BEFORE THE OFFLINE BRANCH, which is where this differs from the
        // axe below it. A viewmodel has no business in a landscape render and
        // is deliberately never loaded for one; a butterfly is part of the
        // wood, and an --out picture of this place without them would be a
        // picture of a different place. renderOffline ticks the flock itself,
        // because it runs none of the per-frame systems that would otherwise.
        if (opt_.butterflies > 0) {
            flock_.wanted = mini(opt_.butterflies, kButterflySlots);
            flock_.init(world_, opt_.butterflyDir);
            // The songbirds share the band -- see kButterflySlots -- so they
            // are loaded here, beside the flock, and for the same reason: the
            // flyer models have to exist before anything is built.
            birds_.init(world_, opt_.birdDir);
            // ...and the lake, which shares the band for the same reason and
            // has to be registered in the same window. See render/lake.h.
            lake_.load(world_, opt_.birdDir, opt_.decor);
            flock2_.load(world_, opt_.birdDir);
            // ...and the bunnies, in the same window and for the same reason:
            // every flyer-band model has to be registered before anything is
            // built. opt_.birdDir is assets/life, which is where they live.
            bunnies_.load(world_, opt_.birdDir + "/bunny");
            bees_.load(world_, opt_.birdDir);
            // ...AND THE ANT, THE FLY, THE LADYBUG AND THE FROG. After the
            // bees for the reason everything here is ordered: the palette is
            // served first-come (see World::prewarmRoom), and these four are
            // the smallest claim in the program -- between them they ask for
            // about thirty entries, most of which the nearest-match snap
            // resolves onto colours the wood already owns.
            // -- THE PARTICLES GO FIRST NOW, AND THE FIREFLY FOLLOWS THEM --
            //
            // (user 2026-09-17: "have the lightning bug at night share the same
            //  lit voxel as the spark voxel.")
            //
            // THE ORDER IS THE WHOLE OF HOW THAT IS POSSIBLE. The spark's
            // colour is a PRIVATE TINT chosen at runtime -- `one()` walks
            // candidates and then sweeps until it finds a colour nothing else
            // is near -- so it is not a constant anybody can write down, and
            // the firefly can only repaint onto it AFTER it exists.
            //
            // The old order was the other way round and its reason still holds
            // and is still served: the particles want private materials, and a
            // private material is one with nothing near it, so they should ask
            // last. They still do, of everything that competes with them --
            // the firefly no longer claims a lamp of its own, so there is
            // nothing left here for them to lose a colour to. It costs one
            // fewer palette entry than before.
            if (opt_.smokeIor > 0.0f) particles_.setSmokeIor(opt_.smokeIor);
            particles_.load(world_, &tracer_);
            critters_.load(world_, opt_.birdDir, particles_.ready() ? particles_.sparkRgb()
                                                                    : nullptr,
                           particles_.sparkMtl());
            // ...AND THE RIFLE'S ROUNDS ARE THE SPARK'S VOXEL. Borrowed rather
            // than loaded -- see Bullets::useModel and the header's note on why
            // a tracer round wants exactly the spark's private emissive
            // material and nothing of its own.
            bullets_.useModel(particles_.sparkModel());
            // ...and the override, if one was given. After load(), which is
            // what registers the material the emitter is keyed on.
            if (opt_.sparkR >= 0.0f)
                particles_.setSparkEmit(tracer_, Vec3(opt_.sparkR, opt_.sparkG, opt_.sparkB));

            // -- AND WHETHER THE PALETTE SURVIVED ALL OF THAT ---------------
            //
            // The table has mat::COUNT entries and forModelColor returns AIR
            // when it is full, so a model registered past the ceiling simply
            // stops being drawn with no error anywhere. Palette::overflowed()
            // counted exactly that for weeks with nothing calling it, and the
            // first anybody knew was "the tools are broke".
            //
            // HERE, NOT AFTER THE TOOLS, WHICH IS WHERE IT USED TO SIT. That
            // was right while the held kit was the last thing to register a
            // colour; it PREWARMS at the front of the queue now (see the note
            // over HeldItem::prewarmColors), and this band -- butterflies,
            // birds, six fish, ducks, the flock, the rabbits, four marchers and
            // the bees -- is what loads last. A report that stops counting
            // before the last loader is a report that cannot see the overflow
            // it exists for, and that is exactly what it did: "the newly
            // imported life is missing voxels" with a clean-looking log.
            //
            // Printed unconditionally, and on the OFFLINE path too. It used to
            // sit inside the held-model branch, which --out deliberately does
            // not take, so a headless render could not see the number at all.
            // -- AND NOW THE ARCADE'S OWN TABLE, WHICH HAS TO BE LAST --------
            //
            // (user 2026-09-18: "cant you give me seperate tables? one palete
            // table for the sandbox world and one for the arcade with the fps
            // maps".)
            //
            // AFTER EVERY LOADER IN THE PROGRAM, and that is not tidiness. The
            // arcade allocates its own 255 entries TOP-DOWN from 254, and the
            // one thing it may not reuse is an id that carries BEHAVIOUR in
            // the wood, because `h.mtl` is a raw uint8 and none of the tests
            // that read it can be told which table is in force. Three sets
            // qualify and all three are recorded through World::noteHeldMtl:
            //
            //   * the HELD KIT -- you carry it through [O] (prewarmColors).
            //   * the PARTICLE materials -- spark, ember red and smoke are in
            //     V6Params::emitters, and an arcade colour that lands on one
            //     of those ids GLOWS. That is not a guess; see the note in
            //     Particles::load for the cliff face it lit up.
            //   * the firefly's glow, for the same reason.
            //
            // The first of those is known at line 631 and the other two are
            // not known until here, which is what decides the position of this
            // call. Being last costs the arcade nothing -- it is not competing
            // with anybody for entries any more, which is the whole point of
            // it having a table.
            world_.prepareLevelPalette();

            {
                const int used = world_.palette.used();
                const int lost = world_.palette.overflowedColors();
                const int calls = world_.palette.overflowed();
                std::printf("  palette  %d of %d entries used%s\n", used, int(mat::COUNT),
                            lost ? "  -- FULL" : "");
                if (lost)
                    std::fprintf(stderr,
                                 "v2: PALETTE FULL -- %d distinct colour(s) could not be "
                                                                         "registered and render as AIR, refused %d time(s) across all "
                                                                         "models. Models are served in load order, so what you cannot "
                                                                         "see is whatever loaded last.\n",
                                 lost, calls);
            }
            edit_.attach(&bunnies_);

            std::fflush(stdout);
        }

        // THE OFFLINE PATH NEVER STARTS THE CLOCK, and the order here is the
        // whole reason why.
        //
        // --sun-az and --sun-el place the sun directly. The day/night clock
        // places it too, from a time of day -- and its azimuth is
        // `azimuthBase + (tday - 0.5) * 180`, so at any hour but noon the two
        // disagree. Running applySun() before this branch overwrote the sun the
        // flags asked for with the sun the default hour implies: 18.4 degrees
        // of azimuth away, which moved every shadow in the frame and made a
        // matched comparison against the OptiX engine impossible.
        //
        // The clock is a VIEWER concept. Offline renders one frame at one sun,
        // named on the command line and reproducible from it.
        if (opt_.checkDemod) {
            checkDemodulation(ctx);
            shutdown(0);
            return;
        }
        if (opt_.fellTest) {
            runFellTest();
            shutdown(0);
            return;
        }
        if (opt_.floatTest) {
            runFloatTest();
            shutdown(0);
            return;
        }
        if (opt_.digTest) {
            runDigTest();
            shutdown(0);
            return;
        }
        if (opt_.clipTest) {
            // The clock, for the same reason --locate-test sets it: the
            // fireflies do not exist before dark, so a check run at noon
            // silently skips a population.
            clock_.tday = opt_.timeOfDay;
            clock_.cycleSpeed = opt_.cycleSpeed;
            clock_.azimuthBase = opt_.sunAz;
            // ...AND THEN PUT IT IN THE SKY, WHICH IS THE HALF THAT WAS
            // MISSING. Setting clock_.tday is not setting the sun: applySun is
            // what carries the clock into world_.sky, it lives two hundred
            // lines BELOW this branch (deliberately -- see the note over the
            // offline path), and isNight() asks the SKY.
            //
            // So every headless diagnostic has run in daylight whatever --time
            // said, and --locate-test has been reporting "firefly -- NONE, AND
            // IT SHOULD BE HERE" at eleven at night ever since the fireflies
            // were added. The clock line above was added to fix exactly that
            // and fixed half of it; the report it was meant to silence went on
            // printing, and was read as a known quirk.
            applySun(true);
            runClipTest();
            shutdown(0);
            return;
        }
        if (opt_.biteTest) {
            // The same clock/sun pair every headless diagnostic needs -- see
            // the note above, which is about the half of it that was missing
            // for months.
            clock_.tday = opt_.timeOfDay;
            clock_.cycleSpeed = opt_.cycleSpeed;
            clock_.azimuthBase = opt_.sunAz;
            applySun(true);
            runBiteTest();
            shutdown(0);
            return;
        }
        if (opt_.locateTest) {
            // -- THE CLOCK FIRST, AND IT WAS NOT --------------------------
            //
            // clock_.tday is set eighty lines BELOW this, with the rest of the
            // viewer's state, so every headless diagnostic ran at the default
            // hour whatever --time said. It did not matter until a population
            // existed that only comes out after dark: --locate-test --time 23
            // surveyed a wood at noon and reported "firefly - NONE, AND IT
            // SHOULD BE HERE", which is the survey being asked the wrong
            // question rather than the firefly being missing.
            //
            // The same trap as --water-flags a few lines up, and the same rule
            // settles it: anything the command line sets about the WORLD
            // belongs above the diagnostics that measure the world.
            clock_.tday = opt_.timeOfDay;
            clock_.cycleSpeed = opt_.cycleSpeed;
            clock_.azimuthBase = opt_.sunAz;
            applySun(true);   // see the note in the --clip-test branch above
            runLocateTest();
            shutdown(0);
            return;
        }

        // -- THE WATER TERMS, BEFORE ANYTHING CAN RENDER ---------------------
        //
        // ABOVE THE OFFLINE RETURN, and that placement is the whole point.
        // These two lines used to sit two hundred lines further down, past the
        // `if (opt_.outGiven) { ...; shutdown(0); return; }` below -- so
        // --water-flags reached the interactive path and NOT the offline one.
        // Measured: `--out --water-flags 0` against `--water-flags 511` differed
        // by exactly zero pixels. Every scripted A/B of a water term taken that
        // way was comparing an image with itself and reporting no change, which
        // is indistinguishable from a term that does nothing.
        //
        // The same trap as the waterY note in the block below, one flag later.
        // Anything the command line sets for the RENDERER belongs above that
        // return; only what it sets for the player belongs after it.
        //
        // The panel is seeded from the same number so --water-flags and the
        // checkboxes cannot disagree about what is on.
        for (int b = 0; b < 10; ++b) waterTerm_[b] = (opt_.waterFlags >> b) & 1u;
        tracer_.waterFlags = opt_.waterFlags;
        // -- AND THE WAVE HEIGHT, WHICH ONLY THE OFFLINE PATH WAS SETTING ----
        //
        // (user 2026-09-18: "you turned off the waves. turn them back on.")
        //
        // `tracer_.waterWaveGain = opt_.waves` existed, twenty lines down,
        // inside the `if (opt_.outGiven)` block -- so --waves reached the
        // --out renders and NOTHING ELSE. An interactive session never
        // assigned it at all and ran on tracer.h's initialiser, which is 0.
        // The water in the game has been flat since the term was added, and no
        // value of --waves could have changed that.
        //
        // This is the third thing to be caught by exactly the note above it:
        // anything the command line sets for the RENDERER belongs above the
        // offline return, or it only ever reaches half the program. ONCE, not
        // per frame -- the [L] panel's slider writes the same field, and a
        // per-frame assignment would overwrite it between the drag and the
        // next draw, which looks like a slider that does not work.
        tracer_.waterWaveGain = opt_.waves;

        if (opt_.outGiven) {
            tracer_.setDemodulate(opt_.demodulate);
            // THE OFFLINE PATH SETS THE WATER ITSELF, because it never runs
            // onFrameRender -- see the note over renderOffline. Without this
            // every --out render had waterY at its "no water anywhere" default
            // and waterTime at zero: a submerged camera got no absorption and
            // the waves stood still. Every verification render taken before
            // this was quietly lying about both.
            tracer_.waterY = world_.terrain.waterSurfaceAt(pos_.x, pos_.z);
            tracer_.waterTime = 0.0f;
            renderOffline(ctx);
            shutdown(0);
            return;
        }

        nudgeOutOfSolids();

        player_.walk = opt_.speed;
        player_.fly = opt_.startFly;
        player_.eye = opt_.eye;
        held_.sway = opt_.handSway;
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        yaw_ = opt_.yaw;
        pitch_ = opt_.pitch;
        fov_ = opt_.fov;
        sunAz_ = opt_.sunAz;
        sunEl_ = opt_.sunEl;
        clock_.tday = opt_.timeOfDay;
        clock_.cycleSpeed = opt_.cycleSpeed;
        clock_.azimuthBase = opt_.sunAz;
        applySun(true);

        // The profiler is off by default and costs a pair of timestamps per
        // scope when it is on, so it is turned on only for a measured run.
        if (opt_.profile && getDevice()->getProfiler()) getDevice()->getProfiler()->setEnabled(true);

        if (opt_.menuAtStart) setMenuOpen(true);
        if (opt_.waterPanelAtStart) setWaterPanelOpen(true);
        // -- THE PAUSE BUTTONS ARE LOADED NOW, WHETHER OR NOT ANYONE OPENS
        // THEM ------------------------------------------------------------
        //
        // They used to be loaded on the first ESC, which made the pause menu
        // the last thing in the program to ask the palette for a colour -- and
        // the palette runs out. See World::prewarmRoom for the measurement and
        // for what the user actually saw. HERE rather than three lines later
        // because the held tools below are the next in the queue and they were
        // the LAST thing to break this way.
        world_.prewarmRoom();
        // --room IS NOT OPENED HERE. The panel is pinned in front of the EYE
        // when it opens, and the eye does not reach its final place until the
        // chunk under the player has streamed and placeOnGround has run -- one
        // frame later, in onFrameRender. Opened here it would hang at whatever
        // height the player was at before the ground was found, which is a
        // photograph of a bug rather than of the menu. See the settle block.
        // THE EDITOR, FROM THE COMMAND LINE. The same path U takes, so a shot
        // of the stage is a shot of the thing the key opens rather than of a
        // second arrangement that could drift from it.
        if (opt_.stageAtStart) {
            world_.setStage(true);
            standOnDeck();
            stageSubject();
            if (opt_.gizmoAtStart) edit_.pick(opt_.gizmoAtStart);
        }
        // --level IS NOT DISPATCHED HERE, and it used to be. See the block
        // below the held kit: standInLevel puts a rifle in the player's hands
        // now, and the kit those hands reach into does not exist this early.

        // The recorder compiles its conversion shader here rather than on the
        // first R: a first take that spent 300 ms in the shader compiler would
        // start by recording a hitch.
        recorder_.init(getDevice());

        // The bed is decoded and started HERE, silently, and then only its
        // volume ever moves -- render/audio.h says why it is not started on
        // entering a wood instead.
        //
        // NOT UNDER --out AND NOT UNDER --background. An offline render has
        // no listener and would only be a five-second decode added to every
        // frame job; and a --background instance is one you are meant to be
        // able to forget is running, which a forest singing out of a
        // minimised window rather defeats.
        // ONE DEVICE, AND THE BED AND THE TOOLS SHARE IT. See vb::AudioDevice
        // for why two mastering voices would be two entries in the system
        // mixer for one game.
        //
        // The gate is unchanged and now covers both: no audio under --out,
        // which has no listener and would only add a decode to every frame
        // job, and none under --background, which is an instance you are meant
        // to be able to forget is running. A scripted --swing-hold run in the
        // background would otherwise chop away audibly in a minimised window.
        if (opt_.soundOn && !opt_.outGiven && !opt_.background && audio_.open()) {
            ambience_.open(audio_, opt_.sound, opt_.ambience);
            toolSfx_.open(audio_, opt_.soundDir);
            toolSfx_.setGain(opt_.sfx);
            // ...AND THE RECORDER GETS THE SAME MIX THE PLAYER HEARS. The tap
            // sits on the mastering voice, so what R keeps is the game's own
            // output after every gain -- and nothing from any other program.
            recorder_.useAudio(audio_.ring(), [this](bool on) { audio_.armTap(on); });
        }

        // -- the axe ---------------------------------------------------------
        //
        // AFTER world_.build, and it has to be: the model goes into the same
        // palette, the same triangle pool and the same acceleration structures
        // every rock does, and none of those exist until the world has built
        // them. World::loadHeldModel does all of it and re-uploads the material
        // table for the handful of entries the tool adds.
        if (opt_.axeOn) {
            // THE STARTING KIT, IN HOTBAR ORDER. The JS engine's giveStartKit
            // for 2026-08-31 is "spawn me with an axe and a pick", axe first --
            // and first is what the hand opens with. The wheel cycles them.
            //
            // THE PICK STARTS ON THE AXE'S BAKE, which is that engine's own
            // note on the row: same haft, same swing, so the pose that was
            // tuned for one is the right place to begin the other. Tune it live
            // and use the menu's copy row to bring the numbers back here.
            //
            // AND EACH DECLARES WHAT IT TAKES. That is the only thing left in
            // v2 that tells an axe from a pick -- nothing can be carved here,
            // so the material a tool is for survives purely as what it SOUNDS
            // like against wood and against stone. See Takes in
            // render/helditem.h, and toolsound.h for the rule it feeds.
            //
            // THE OFFSETS ARE THE OLD ONES CARRIED OUT TO REAL SCALE. Each is
            // the world point the old pose actually put the tool at, times the
            // factor its voxels grew by -- 100 mm over the 11.0 mm a tool voxel
            // used to measure. An object N times the size at N times the
            // distance projects to the same place, so the framing that was
            // tuned by eye survives the change of units exactly; what changed
            // is that the axe is now a 90 cm axe rather than a 10 cm one.
            held_.add(world_, "stone axe", opt_.axe,
                      HeldPose{8.799f, -0.910f, 8.730f, 0.040f, -1.420f, 1.580f, 1.000f},
                      Takes::Wood);
            held_.add(world_, "stone pick", opt_.pick,
                      HeldPose{8.512f, -0.910f, 8.730f, 0.040f, -1.420f, 1.580f, 1.003f},
                      Takes::Stone);
            // ...AND THE SHOVEL, THIRD (user 2026-09-10). Same haft, same
            // swing, so it starts on the PICK's bake for the reason the pick
            // starts on the axe's -- the pose that was tuned for one is the
            // right place to begin the next. Tune it live and use the menu's
            // copy row to bring the numbers back here.
            //
            // THIRD RATHER THAN LAST, and neither end was free: slot zero is
            // what the world opens with and that is the axe's by the
            // giveStartKit rule, and the last slot is the empty hand, which was
            // asked for as the thing the wheel reaches after everything else.
            // Between the pick and the bow is where the hand tools already are.
            //
            // AND IT TAKES SOIL, which is the whole of what makes it a shovel:
            // the loose ground gives to it and stone does not, exactly as wood
            // gives to the axe. See Takes and isSoilMat.
            //
            // ...EXCEPT FOR THE ROLL, AND THE MODEL IS WHY. The axe and the
            // pick are authored 5x1x9 and 7x1x9 -- long up the Z axis, one
            // voxel THIN IN Y, so the head lies in the model's XZ plane. The
            // shovel is 1x3x10: long up Z like the others, but thin in X, with
            // its blade in the YZ plane. It is the same tool turned a quarter
            // turn in the file it was drawn in.
            //
            // Roll is the innermost rotation -- R = Rx(pitch).Ry(yaw).Rz(roll),
            // so Rz acts on the model's own axes before anything else -- which
            // makes it exactly the angle that undoes that. 1.580 - pi/2 puts
            // the shovel's wide axis where the axe's wide axis already sits,
            // so the blade faces across the frame instead of edge-on to it.
            //
            // THIS IS A STARTING POINT AND NOT A FINISHED ANSWER, like every
            // pose on this row: a viewmodel is judged by eye. Tune it live in
            // the settings menu (Y) and use the copy row to bring the numbers
            // back here.
            held_.add(world_, "stone shovel", opt_.shovel,
                      // ...AND HALF A TURN ON THE ROLL (user 2026-09-13: "flip the
                      // shovel 180 degrees vertically so that the stone is facing
                      // upwards like the other tools"). Roll is the innermost
                      // rotation and turns the model about its own long axis, so
                      // pi is exactly a vertical flip -- the blade that hung under
                      // the haft now sits on top of it, where the axe head and the
                      // pick head already are. 0.009 + pi.
                      // ...AND HALF A TURN ON THE PITCH (user 2026-09-13: "flip the
                      // shovel 180 degrees vertically so that the stone is facing
                      // upwards like the other tools"). -1.420 + pi = 1.722.
                      //
                      // THE PITCH, NOT THE ROLL, and the difference is worth a
                      // line because the roll was the obvious guess and it is
                      // wrong. Roll is the innermost rotation, so it turns the
                      // model about its OWN axis -- which spins a shovel on the
                      // spot and leaves the blade exactly where it was; measured,
                      // it changed 2% of the frame. Pitch is the outermost, so it
                      // is the one that acts in the view's own frame, and half a
                      // turn there is what "flip it vertically" means to somebody
                      // looking at the screen.
                      HeldPose{8.512f, -0.910f, 8.730f, 0.040f, 1.722f, 0.009f, 1.003f},
                      Takes::Soil);
            // ...AND THE HOE, FOURTH (user 2026-09-14: "import the hoe from
            // v1"). Same haft as the shovel and the same flip, so it starts on
            // the SHOVEL's bake for the reason the shovel started on the
            // pick's: these three are one shape of tool and a pose tuned for
            // one of them frames the others. Its head is a blade set across the
            // haft rather than along it, which the quarter turn on the roll is
            // for.
            // ...AND HALF A TURN ON THE PITCH (user 2026-09-14: "flip the hoe
            // vertically 180 degrees so that the stone is facing upwards like
            // the rest of the tools"). 1.722 - pi = -1.420, which is the axe's
            // and the pick's own pitch -- so the hoe was the odd one out by
            // exactly the flip the SHOVEL had needed, inherited along with the
            // shovel's bake it started from.
            //
            // THE PITCH, NOT THE ROLL, and the shovel's note is why: roll is
            // the innermost rotation and spins the model about its own long
            // axis, which turns a hoe on the spot and leaves the blade where it
            // was. Pitch is the outermost, so it acts in the VIEW's frame, and
            // half a turn there is what "flip it vertically" means to somebody
            // looking at the screen.
            // ...AND THREE QUARTERS OF A TURN ON THE ROLL, arrived at in two
            // asks: a quarter (2026-09-14, "change the hoes roll by 90
            // degrees") to 1.580, then a half on top of that ("now rotate the
            // hoe roll by 180 degrees") to 4.722.
            //
            // BOTH WRITTEN AS ONE NUMBER rather than as 1.580 + kPi, because a
            // pose row is a BAKE -- seven literals somebody read off the panel
            // and pasted -- and an expression in the middle of one is a value
            // you cannot copy back out. See the copy-pose row in the settings
            // menu, which hands over exactly this form.
            //
            // The head ends up under the haft rather than over it, which is not
            // where the axe and the pick carry theirs. That is the ask; a hoe
            // is swung with the blade turned down.
            held_.add(world_, "stone hoe", opt_.hoe,
                      HeldPose{8.512f, -0.910f, 8.730f, 0.040f, -1.420f, 4.722f, 1.003f},
                      Takes::Earth);
            // THE BOW'S OWN BAKE, from the JS engine's PICK_DEFS for
            // 2026-08-04. It is not the tool family's pose: a bow is held
            // upright across the hand, further out and turned a quarter turn
            // (pitch 1.57) so the limbs stand across the frame rather than
            // along it, and its art is much longer than a hand tool, which is
            // what the larger scale is for.
            // THE BOW'S OWN, carried out by ITS factor -- 100 mm over the
            // 14.6 mm its voxels used to measure, which is not the tools'
            // 9.09 because its scale was not theirs. That difference is the
            // whole bug being fixed here: two hand items on two different
            // voxel sizes. They are on one now, and each keeps the framing it
            // was tuned to.
            held_.addBow(world_, "bow", opt_.bow,
                         HeldPose{12.000f, -1.320f, 6.990f, 0.010f, 1.570f, -0.060f, 1.593f});
            // ...AND A FOURTH SLOT WITH NOTHING IN IT (user 2026-09-07). LAST
            // rather than first, so the wheel reaches it after the bow and the
            // game still opens with the axe in hand -- putting it at slot zero
            // would have changed what you start holding, which was not asked
            // for.
            // -- WHAT THE WHEAT PAYS OUT, AS TWO KIT SLOTS -----------------
            //
            // (user 2026-09-14: "have them drop out of the tall grass (wheat).
            // one a piece ... then the player can absorb the items.")
            //
            // THEY ARE CARRIED ITEMS, NOT A COUNTER. Everything an absorbed
            // drop needs already exists for the axe -- the walk-over pickup,
            // the flight into the chest, the sound, the hand -- and all of it
            // is addressed by a KIT INDEX. Giving wheat a tally of its own
            // would mean a second inventory with none of that, and a pickup
            // path that forks on which of the two kinds of thing you walked
            // over. So a stalk of wheat is a thing you hold, like a pick.
            //
            // AND THEY START OUT OF THE WHEEL. `carried` is false until one is
            // picked up, which is the same state a dropped axe is in -- see
            // HeldItem::take. So the kit still opens on the axe and still
            // scrolls axe / pick / shovel / bow / hand until you have cut some.
            //
            // They TAKE nothing: swinging a stalk of wheat at a rock knocks and
            // moves no voxels, which is what Takes::Nothing already means.
            // ITS OWN POSE, GIVEN RATHER THAN DERIVED (user 2026-09-14). The
            // first number is how far out from the eye the item sits and the
            // stalk wants 11.570 against the tools' 8.799 -- wheat is a long
            // thin thing held up, not a haft gripped at the middle.
            wheatTool_ = held_.count();
            // ...AND IT FOLDS ITS FOUR CREAMS INTO ONE -- see kRampMergeTol.
            if (!held_.add(world_, "wheat", opt_.wheat,
                           HeldPose{11.570f, -0.910f, 8.730f, 0.040f, -1.420f, 1.580f, 1.000f},
                           Takes::Nothing, HeldItem::kRampMergeTol))
                wheatTool_ = -1;
            seedsTool_ = held_.count();
            if (!held_.add(world_, "seeds", opt_.seeds,
                           HeldPose{8.799f, -0.910f, 8.730f, 0.040f, -1.420f, 1.580f, 1.000f}))
                seedsTool_ = -1;
            // ...AND THE STEAK, on the same terms as the other two: a thing
            // you hold, stowed until a kill puts one in your hands. Its pose is
            // the seeds' -- a slab held out in front -- rather than the wheat's
            // long-thin one, and it is a starting point for a bake rather than
            // a tuned number.
            steakTool_ = held_.count();
            // -- ...AND ITS COLOURS EXACTLY, LIKE THE REST OF THE KIT ----
            //
            // (user 2026-09-15: "fix the raw steaks color pallette".)
            //
            // IT WAS ALLOWED TO SHARE FOR ONE DAY and sharing is what was wrong
            // with it. The meat is nine colours in two smooth ramps -- five
            // reds twelve apart and four pinks -- and a snap at kModelMatch
            // merges every neighbouring pair: MEASURED, nine authored colours
            // collapse to FIVE. The ramp becomes bands, and worse, the merge is
            // against the WHOLE table rather than within the model, so a
            // steak's red can land on a colour the wood already owns.
            //
            // That was done because the table was full. It is not any more --
            // the seed ramp and the particles settled it at 243 of 255 -- so
            // the steak takes its nine, like the axe takes its fourteen, and
            // the start-up line prints the total so the day it stops fitting is
            // a number rather than a surprise.
            //
            // THE POSE IS THE USER'S BAKE (2026-09-15), not a guess.
            // -- ...AND IT IS FOOD, SO IT IS A BITE STRIP -------------------
            //
            // (user 2026-09-17: "import the eating mechanics from v1 onto all
            //  of the food.")
            //
            // `addFood` rather than `add`, which is the whole change: the steak
            // becomes kEatFrames models of itself being eaten instead of one,
            // and frame 0 is the steak, so nothing about how it looks in the
            // hand moves. Its merge tolerance rides along -- see the note above
            // on why the meat's nine colours must not be snapped.
            if (!held_.addFood(world_, "steak", opt_.steak,
                               // Re-baked 2026-09-19 off the [K] card.
                               HeldPose{8.799f, -1.624f, 8.730f, 0.977f, 0.575f, 1.885f, 1.000f},
                               HeldItem::kSteakMergeTol))
                steakTool_ = -1;
            // -- THE APPLE AND THE ORANGE, WHICH ARE THE SAME TWO MODELS -----
            //
            // (user 2026-09-17: "import the apple/oranges pick up mechanic. so
            //  they need to be a handheld now.")
            //
            // THE FILES ARE THE ONES HANGING IN THE TREES. The browser engine
            // makes the same choice and says why: frame 0 of eating an apple
            // IS an apple, so one model for "in your hand" and "in the crown"
            // means the two can never disagree about what an apple looks like.
            // They are 4x3x5 and 3x3x5 -- a 40 cm fruit at the kit's 1 voxel =
            // 10 cm, which is the same scale the wheat and the seeds are on.
            //
            // STOWED, LIKE THE WHEAT AND THE STEAK. Loaded at start-up so the
            // colours are served first-come (a model that waits for a pickup
            // renders as AIR -- see the note over the rifle), and out of the
            // wheel until one is actually picked.
            //
            // THE POSE IS THE SEEDS', which is the kit's "a small thing held
            // out in front" bake rather than the wheat's long-thin one. A
            // starting point for a bake, said as one.
            // -- THE ARROW, AS A THING YOU CAN PICK BACK UP -------------
            //
            // Its own kit slot on the wheat's terms. The model is already
            // loaded for the bow's strip, but a STRIP FRAME IS NOT A TOOL and
            // cannot be carried -- so this is a second registration of the same
            // file, which costs no palette because held colours are served to
            // an EXACT key the first one already took.
            arrowTool_ = held_.count();
            if (!held_.add(world_, "arrow", opt_.arrow,
                           HeldPose{8.799f, -0.910f, 8.730f, 0.040f, -1.420f, 1.580f, 1.000f},
                           Takes::Nothing))
                arrowTool_ = -1;
            // THE USER'S BAKE, off the [K] card. It drops the fruit 67 cm in
            // the hand's own units from the seeds' starting pose -- the note
            // above called that pose "a starting point for a bake, said as
            // one", and this is the bake. ONE POSE FOR BOTH: an apple and an
            // orange are 4x3x5 and 3x3x5 of the same kit scale, so a pose that
            // sits one correctly sits the other (user: "apply these new
            // positions for the apple as well since they are the same shape").
            //
            // RE-BAKED 2026-09-19: roll 1.580 -> 1.231, the user's own card
            // again ("apple { 8.799, -1.577, 8.730, 0.040, -1.420, 1.231,
            // 1.000 }. apply this to the orange too"). A fifth of a radian
            // about the shaft and nothing else moves -- the fruit turns in the
            // hand, it does not change where the hand is.
            appleTool_ = held_.count();
            if (!held_.addFood(world_, "apple", opt_.apple,
                               HeldPose{8.799f, -1.577f, 8.730f, 0.040f, -1.420f, 1.231f, 1.000f}))
                appleTool_ = -1;
            orangeTool_ = held_.count();
            if (!held_.addFood(world_, "orange", opt_.orange,
                               HeldPose{8.799f, -1.577f, 8.730f, 0.040f, -1.420f, 1.231f, 1.000f}))
                orangeTool_ = -1;
            // -- THE ASSAULT RIFLE, ON THE WHEAT'S TERMS AND FOR A NEW REASON -
            //
            // (user 2026-09-17: "spawn the player in with an assault rifle when
            // spawning into the o. dont let the player have the gun in the
            // regular sandbox yet.")
            //
            // THE MECHANISM IS ALREADY HERE. A kit slot that is loaded but not
            // in the wheel is exactly what the wheat, the seeds and the steak
            // are, and `stow` is the whole of how they do it -- so the gun is a
            // fourth one of those and needs nothing new. What is new is WHO
            // takes it out of the wheel again: those three are given by picking
            // one up in the world, and this one is given by a DOOR. See
            // standInLevel and leaveLevel.
            //
            // LOADED AT START-UP EVEN THOUGH IT IS NOT CARRIED, which is the
            // whole point of putting it here and not behind the keypress. The
            // note over opt_.rifle has the argument: colours are served
            // first-come, [O] happens minutes later, and a model that asks then
            // is a model that renders as air. This is the same reasoning that
            // moved the pause room to the front and that broke the stone tools
            // when it was got wrong.
            //
            // -- THE POSE, DERIVED FROM THE TRANSFORM RATHER THAN GUESSED ----
            //
            // A GUN POINTS WHERE YOU ARE LOOKING, and nothing else in this kit
            // does, so none of the bakes above is a starting point. The first
            // cut borrowed the axe's row with the roll taken off and put a 1.1 m
            // rifle STANDING ON END half a metre from the eye -- a wall of dark
            // blocks filling a third of the frame, which is not a thing you
            // recognise as a mistuned pose.
            //
            // So it is worked out from HeldItem::instanceFor instead. That
            // function maps the pose's three axes onto the model's, and the
            // mapping is not the identity -- the model's HEIGHT takes `az` and
            // its DEPTH takes `ay`, which is the swap scene/vox.h's header
            // warns about. Written out at yaw 0:
            //
            //     ax = ( cr,  sr.cp,  sr.sp )   -> the model's WIDTH  (sx = 3)
            //     az = (  0,   -sp,     cp   )  -> the model's HEIGHT (sy = 4)
            //     ay = (-sr,  cr.cp,  cr.sp )   -> the model's DEPTH  (sz = 11)
            //
            // and camera space is x right, y up, z forward. This gun is long
            // down its DEPTH, so what is wanted is ay = +-(0, 0, 1) with az
            // still (0, 1, 0). Pitch = -pi/2 gives sp = -1, cp = 0 and settles
            // the second of those on its own -- az = (0, 1, 0) with no roll
            // term in it at all, so the gun is upright whatever the roll does.
            // What is left is
            //
            //     ay = (-sr, 0, -cr)        the barrel
            //     ax = ( cr, -sr,  0)       the width
            //
            // -- AND THE ROLL IS THEREFORE A 180 DEGREE TURN, NOT A TWIST -----
            //
            // (user 2026-09-17: "flip the assault rifle around 180 degrees. the
            // point that is facing the player is now facing away from the
            // player.")
            //
            // With the pitch above, roll only ever takes sr = 0, and the two
            // ends of it are a pair:
            //
            //     roll = pi   ->  ay = (0, 0,  1)   barrel AWAY, ax = (-1,0,0)
            //     roll = 0    ->  ay = (0, 0, -1)   barrel TOWARD YOU, ax = (1,0,0)
            //
            // Both flip together, az does not move, and the determinant is
            // unchanged -- so this is a rotation about the VERTICAL axis and
            // not a mirror, which is exactly the half turn that was asked for.
            // It shipped at pi and the muzzle was pointing back at the player;
            // it is 0 now.
            //
            // Worth saying because it is the one rotation here that is NOT
            // guessable from the number: a roll of zero reads as "no roll", and
            // on this pose it is the half turn.
            //
            // THE OFFSET IS THE CENTRE OF THE BOX, in world voxels from the eye
            // -- so the 11-voxel barrel runs from 0.35 m to 1.45 m in front of
            // you, clear of the near plane at one end and not poking into the
            // scenery at the other. Right and down by a hand's width, which is
            // where a viewmodel sits.
            //
            // IT IS STILL A BAKE WAITING TO HAPPEN. Tune it live and use the
            // settings menu's copy-pose row to bring the numbers back here,
            // exactly as the hoe and the steak were baked.
            //
            // AND ITS RAMP SURVIVES AT kGunMergeTol -- see mergeTolFor, which
            // carries the measurement: eight entries of eleven, worst voxel
            // 3/255 out, against the five-entry fold that was reported as "the
            // guns color pallete is off". The tolerance passed here MUST match
            // what mergeTolFor answers for this path or the reservation claims
            // entries the load never asks for.
            //
            // THE POSE IS THE USER'S BAKE (2026-09-17), off the live panel.
            //
            // -- ...AND WHERE IT GOES WHEN THE RIGHT BUTTON IS DOWN ---------
            //
            // Centred on x, up to just under the eye line, and a little closer
            // in. The rotations are the hip pose's untouched -- a gun that is
            // already pointing where you are looking does not need to turn to
            // be aimed, it only needs to come up -- so the ease has nothing to
            // do but travel, and no angle in it can wrap the wrong way round.
            //
            // y = -1.6 rather than 0: the model is 4 voxels tall and the pose
            // names the CENTRE of its box, so this puts the top of the receiver
            // about level with the eye and the barrel just under the crosshair,
            // which is where a sight picture sits.
            // THE USER'S BAKE (2026-09-17), read off the [K] sights card.
            // -- BOTH POSES SIT ONE VOXEL LOWER THAN THE BAKE DID ----------
            //
            // The user's two bakes are y = -3.500 (hip) and y = -2.600
            // (sights), read off the [K] cards on 2026-09-17, and what is
            // written below is each of them MINUS ONE.
            //
            // THE MODEL GREW A MAGAZINE WELL UNDER IT. The rifle is now a
            // strip -- the rest pose plus the nine reload frames -- and a
            // strip's frames share one box (HeldItem::addGun). The tallest
            // frame is 6 voxels where the rest pose is 4, so the shared box is
            // two rows deeper than the gun, and those rows go UNDERNEATH it
            // because that is where the frames line up (see HeldItem::fitStrip,
            // which measures it rather than assuming it).
            //
            // A pose names the CENTRE of the box and the corner is derived from
            // it -- centre less half the box down each axis -- so a box two
            // rows taller puts its centre one row lower relative to the gun,
            // and the gun would ride one voxel (10 cm at scale 1) UP the screen
            // for nothing. Subtracting one from y is exactly that, cancelled.
            //
            // So these are still the user's numbers. If the strip is ever
            // re-authored so the tallest frame is not 6, this arithmetic moves
            // with it -- the rest pose alone cannot tell you what the box is.
            static const HeldPose kRifleAds{0.000f, -3.600f, 8.000f,
                                            0.000f, -1.571f, 0.000f, 1.000f};
            rifleTool_ = held_.count();
            if (!held_.addGun(world_, "assault rifle", opt_.rifle, opt_.rifleReload,
                              HeldPose{7.248f, -4.500f, 9.135f, 0.000f, -1.571f, 0.000f, 1.000f},
                              HeldItem::kGunMergeTol, &kRifleAds))
                rifleTool_ = -1;
            // -- THE PISTOL, ONE SLOT ABOVE THE RIFLE -----------------------
            //
            // (user 2026-09-18: "import the pistol asset into the fps mode. put
            // it in the inventory, when the player scrolls up it selects it.")
            //
            // ORDER IS THE ASK, AGAIN. The wheel is cycle(+1) on scroll up, so
            // "scroll up and it selects the pistol" means the very next slot
            // after the rifle and nothing else will do. That sentence used to
            // belong to the bulb below, which has not been handed to the player
            // in the level since "remove the lightbulb from the hand on the fps
            // map" -- so nothing is between these two that is ever in the wheel
            // at the same time as the gun.
            //
            // THE POSE IS A FIRST BAKE AND IT IS DERIVED, not eye-tuned: the
            // rifle's own bake, moved by the difference between the two models.
            // The pistol's shared box is 5 x 5 x 8 against the rifle's 3 x 6 x
            // 11, and the three terms follow from that --
            //
            //   x  the gun body sits at the HIGH-x end of its box (the magazine
            //      comes in from the low side during the reload -- see
            //      fitStrip), so the box centre is a voxel left of the body:
            //      7.248 - 1 = 6.25.
            //   y  the rifle's body sits at the TOP of its box, one voxel above
            //      its centre; the pistol fills its box, so it needs that voxel
            //      back: -4.5 + 1 = -3.5.
            //   z  THE RIFLE'S OWN DEPTH, 9.135, AND THAT IS THE ANSWER TO "IS
            //      IT THE SAME VOXEL SIZE" (user 2026-09-18: "can you make sure
            //      the pistol is the same voxel size as the assault rifle").
            //
            //      IT ALWAYS WAS, in the only sense the pose can express it:
            //      `scale` is 1.000 on both, and HeldItem::xform builds the
            //      instance from `pose.scale * VOXEL_M`, so one model voxel is
            //      one world voxel -- 10 cm -- for both guns and for every
            //      other thing in the kit.
            //
            //      WHAT WAS DIFFERENT WAS THE DISTANCE, and on screen that is
            //      indistinguishable from a scale. The first cut put the pistol
            //      at 7.600 so its grip sat where the rifle's stock does; 15 cm
            //      nearer the eye is 20% more angle per voxel, so the same-sized
            //      voxels drew bigger. Held at the rifle's own depth they
            //      measure the same, which is what was asked. The pistol is
            //      SHORTER, so its muzzle now stops 30 cm short of where the
            //      rifle's does -- that is the gun being a different gun rather
            //      than a different size.
            //
            // Tune it live on [K] and use the copy-pose row, exactly as the
            // rifle's and the hoe's were baked.
            // SHIPPED AT THE USER'S OWN BAKE (2026-09-18, off the [K] card):
            // {-1.000f, -2.933f, 8.000f}. Both terms moved off the derived
            // number -- the sight sits LEFT of the pistol's centreline where
            // the rifle's is on it, and a third of a voxel lower.
            static const HeldPose kPistolAds{-1.000f, -2.933f, 8.000f,
                                             0.000f, -1.571f, 0.000f, 1.000f};
            pistolTool_ = held_.count();
            // -- ...AND IT LOADS LIKE A REVOLVER ---------------------------
            //
            // (user 2026-09-18: "its a standard revolver with 6 rounds".)
            //
            // The last two arguments are the whole of it: ONE TURN of the strip
            // takes kPistolReloadMs and loads ONE round, so the gun plays it
            // once per empty chamber. The rifle takes the defaults -- 1800 ms,
            // and 0 meaning "this turn loads the magazine".
            if (!held_.addGun(world_, "pistol", opt_.pistol, opt_.pistolReload,
                              HeldPose{6.250f, -3.500f, 9.135f, 0.000f, -1.571f, 0.000f, 1.000f},
                              HeldItem::kGunMergeTol, &kPistolAds, kPistolReloadMs,
                              /*reloadRounds=*/1))
                pistolTool_ = -1;
            // -- THE BULB, RIGHT AFTER THE RIFLE ---------------------------
            //
            // ORDER WAS THE ASK HERE TOO, AND IT HAS BEEN SUPERSEDED. "put a
            // bulb in my hand when I scroll up from the assault rifle" made
            // this the very next slot after the gun; the lamp was then taken
            // out of the wheel in the level altogether ("remove the lightbulb
            // from the hand on the fps map"), and the slot above the rifle is
            // the PISTOL now. The two never share a wheel, so neither sentence
            // is broken by the other.
            //
            // A LEVEL TOOL, like the rifle: stowed at start-up, handed over by
            // the door, taken back on the way out. It edits nothing in the wood
            // and there is nothing in the wood for it to edit.
            bulbTool_ = held_.count();
            if (!held_.add(world_, "lightbulb", opt_.bulb,
                           HeldPose{7.248f, -3.500f, 9.135f, 0.000f, -1.420f, 0.000f, 1.000f},
                           Takes::Nothing))
                bulbTool_ = -1;
            if (wheatTool_ >= 0) held_.stow(wheatTool_);
            if (seedsTool_ >= 0) held_.stow(seedsTool_);
            if (steakTool_ >= 0) held_.stow(steakTool_);
            // ...AND THE FRUIT, for the wheat's reason: loaded so the colours
            // are served, out of the wheel until one is picked.
            if (arrowTool_ >= 0) held_.stow(arrowTool_);
            if (appleTool_ >= 0) held_.stow(appleTool_);
            if (orangeTool_ >= 0) held_.stow(orangeTool_);
            // ...AND THE GUN IS STOWED WITH THEM, which is what keeps it out of
            // the sandbox. Nothing in the wood can give it back -- there is no
            // pickup for it and no recipe -- so the only route to `carried` is
            // the level door.
            if (rifleTool_ >= 0) held_.stow(rifleTool_);
            // ...AND THE PISTOL WITH IT, for the same reason and by the same
            // door: it is the level's, and nothing in the wood can hand it over.
            if (pistolTool_ >= 0) held_.stow(pistolTool_);
            if (bulbTool_ >= 0) held_.stow(bulbTool_);

            held_.addEmpty("empty hand");
            held_.select(opt_.tool);
            // AFTER THE WHOLE KIT, because it matches on NAME and needs every
            // slot to have one. See stackBakes.
            applyStackBakes();

            // -- AND WHETHER THE PALETTE SURVIVED ALL OF THAT ---------------
            //
            // AFTER THE TOOLS, WHICH IS THE WHOLE POINT OF WHERE IT SITS. The
            // table has mat::COUNT entries and forModelColor returns **AIR**
            // when it is full, so a model registered past the ceiling simply
            // stops being drawn with no error anywhere.
            //
            // Palette::overflowed() has counted exactly this since the table
            // was written and NOTHING HAS EVER CALLED IT. The first anybody
            // knew was "the tools are broke" -- every stone head in the game
            // had quietly become air, because the held items are the LAST thing
            // to ask for colours and there were not enough left.
            //
            // Printed unconditionally, not only on overflow: a warning at the
            // cliff edge tells you when it is already too late, and a number
            // that creeps up tells you which model was the one that could not
            // fit. 251 of 255 with the tools still to come is the reading that
            // would have caught this before it shipped.
            // AND IT LEADS WITH THE DISTINCT COUNT, not the call count. This
            // line used to say "14 colours could not be registered" and then,
            // after the held kit was moved to the front of the queue, "240" --
            // which reads as twenty times worse and is the SAME ONE COLOUR,
            // re-asked by every model in the flyer band. See
            // Palette::overflowedColors. The call count is kept beside it
            // because it does say something real: how widely that colour is
            // wanted, and therefore how many models show the hole.
            arrows_.init(world_, opt_.arrow);
            arrows_.log = opt_.swingLog;
            world_.carveLog = opt_.swingLog;
            // WHERE THE NOCKED ARROW STARTS. Asked for rather than applied:
            // the request is served on the first frame, beside the streamer,
            // which is the one place in this engine a structure may be built --
            // see the note there, and the one on the settings rows.
            arrowWant_ = opt_.arrowNudge;
            arrowDirty_ = (opt_.arrowNudge.across || opt_.arrowNudge.along ||
                           opt_.arrowNudge.up);
        }


        // -- NUKETOWN, FROM THE COMMAND LINE -- the same path [O] takes ------
        //
        // AND IT RUNS DOWN HERE FOR THE WHEAT CHECK'S REASON, which is written
        // out below and was paid for once already. standInLevel gives the
        // player the assault rifle, and the rifle is a KIT SLOT -- so dispatched
        // two hundred lines up, beside --stage where this used to live,
        // `rifleTool_` is still -1 and the gun is silently not handed over. The
        // key never had this problem because a keypress cannot happen before
        // start-up has finished; the FLAG can, and --level is the only way to
        // photograph this level, so the flag being a faithful copy of the key
        // is the whole of what makes it useful.
        //
        // It cost a render to find and the symptom named nothing: the map came
        // up correctly, the log said the gun had loaded with all of its
        // colours, and the player's hands were empty.
        //
        // AND IT SAVES THE WOOD FIRST, which the key does and this would
        // otherwise skip: woodPos_ starts at the origin, so arriving here by
        // flag and then pressing O to leave would have dropped the player at
        // (0, 0, 0) -- underground, four kilometres from the spawn. There is
        // nowhere better to go back to than wherever the wood had put them by
        // now, which is exactly what the key would have recorded.
        if (opt_.levelAtStart) {
            woodPos_ = player_.pos;
            woodYaw_ = yaw_;
            woodPitch_ = pitch_;
            woodFly_ = player_.fly;
            if (world_.setLevel(true)) standInLevel();
            else std::fprintf(stderr, "v2: --level: no nuketown.vox to travel to\n");
            // -- ...AND THEN TURN THE WHEEL, IF ASKED ----------------------
            //
            // (user 2026-09-18: "when the player scrolls up it selects it".)
            //
            // THE ONLY WAY TO PHOTOGRAPH A SCROLL. The wheel is driven by a
            // mouse event, so what is in the hand after one turn of it cannot
            // be reached from the command line at all -- and "one above the
            // rifle" is a claim about slot ORDER that a picture is the only
            // honest test of. --tool cannot do this: it is applied when the kit
            // is built and standInLevel selects the rifle after it.
            //
            // THROUGH cycle() ITSELF, not through select(), which is the point:
            // cycle skips whatever is not being carried, and that skipping is
            // exactly what makes the pistol rather than the stowed lamp the
            // thing one turn up from the gun.
            for (int s = 0; s < opt_.scroll; ++s) held_.cycle(1);
            if (opt_.scroll > 0) {
                std::printf("v2: scrolled up %d -- holding %s\n", opt_.scroll, held_.name());
                std::fflush(stdout);
            }
        }

        // -- AND THE WHEAT CHECK, WHICH RUNS DOWN HERE AND NOT UP THERE ----
        //
        // Every other headless diagnostic dispatches two hundred lines above
        // this, before the world has a kit -- and that is right for them: they
        // measure terrain and populations, and returning early is what keeps
        // them off the render path. This one swings a tool at a plant and pays
        // into two KIT SLOTS, so it cannot run until those exist.
        //
        // IT COST A BUILD TO FIND, and the symptom named nothing: the test
        // printed "wheat slot -1, seeds slot -1" and no loader error anywhere,
        // because the loader had not been called yet. The same trap as the
        // clock in --clip-test, one file further on.
        if (opt_.hoeTest) {
            clock_.tday = opt_.timeOfDay;
            clock_.cycleSpeed = opt_.cycleSpeed;
            clock_.azimuthBase = opt_.sunAz;
            applySun(true);
            runHoeTest();
            shutdown(0);
            return;
        }
        if (opt_.soilTest) {
            runSoilTest();
            shutdown(0);
            return;
        }
        if (opt_.duckTest) {
            clock_.tday = opt_.timeOfDay;
            applySun(true);
            runDuckTest();
            shutdown(0);
            return;
        }
        if (opt_.lbugTest) {
            clock_.tday = opt_.timeOfDay;
            applySun(true);
            runLbugTest();
            shutdown(0);
            return;
        }
        if (opt_.killTest) {
            clock_.tday = opt_.timeOfDay;
            applySun(true);
            runKillTest();
            shutdown(0);
            return;
        }
        if (opt_.shaftTest) {
            runShaftTest();
            shutdown(0);
            return;
        }
        if (opt_.foodTest) {
            runFoodTest();
            shutdown(0);
            return;
        }
        if (opt_.floatSweep) {
            runFloatSweep();
            shutdown(0);
            return;
        }
        if (opt_.ripTest) {
            runRipTest();
            shutdown(0);
            return;
        }
        if (opt_.poleTest) {
            runPoleTest();
            shutdown(0);
            return;
        }
        if (opt_.floatAudit) {
            runFloatAudit();
            shutdown(0);
            return;
        }
        if (opt_.wheatTest) {
            clock_.tday = opt_.timeOfDay;
            clock_.cycleSpeed = opt_.cycleSpeed;
            clock_.azimuthBase = opt_.sunAz;
            applySun(true);
            runWheatTest();
            shutdown(0);
            return;
        }
        // LAST OF THE HEADLESS RUNS, and it has to be: everything above this
        // line can still mint a colour, and a plate that stops counting before
        // the last loader is the same mistake the palette report made for
        // weeks. See runPaletteVox.
        if (opt_.paletteVox) {
            runPaletteVox();
            shutdown(0);
            return;
        }

        printHelp();
        lastTime_ = std::chrono::steady_clock::now();
    }

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // --hitch: WHAT THE MAIN THREAD SPENT, FRAME BY FRAME.
    //
    // (user 2026-09-15: "can you investigate hitching in the game ... just as im
    // walking around the environment its hitching".)
    //
    // A TOTAL CANNOT FIND A HITCH. --stats already reports where streaming
    // time goes and it has been reporting it happily all along, because a
    // hitch is not an amount of work -- it is an amount of work landing on ONE
    // frame. 400 ms of TLAS spread over a thousand frames is invisible; the
    // same 400 ms in eight frames is eight stutters, and the two print the
    // same number.
    //
    // So this records every frame separately and reports the DISTRIBUTION,
    // with the streaming profile differenced per frame so the worst ones can
    // say what they were doing. Kept to a POD in a flat vector: the recorder
    // must not be the thing it is measuring.
    struct HitchFrame {
        float total = 0.0f;    // the whole of onFrameRender's main-thread work
        float stream = 0.0f;   // World::update -- rering, take, adopt, tlas
        float blas = 0.0f, tlas = 0.0f, pool = 0.0f, rering = 0.0f;
        float drain = 0.0f, take = 0.0f;   // the compaction drain, and the queue pop
        float phys = 0.0f;     // the ground patch and the solver
        float life = 0.0f;     // every population's update
        float pub = 0.0f;      // ...and every population's publish
        int adopted = 0;
    };
    std::vector<HitchFrame> hitch_;
    World::Profile hitchWas_{};
    // Three running totals for the frame in progress. Plain doubles and a
    // stack clock: the recorder has to cost less than the thing it is looking
    // for, and the thing it is looking for is a millisecond.
    double hPhys_ = 0.0, hLife_ = 0.0, hPub_ = 0.0;
    std::chrono::steady_clock::time_point hMark_;
    void hStart() { hMark_ = std::chrono::steady_clock::now(); }
    double hStop() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hMark_)
            .count();
    }

