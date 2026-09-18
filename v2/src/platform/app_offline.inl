// app_offline.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: the headless render path and its probes
// -----------------------------------------------------------------------------
    void renderOffline(Falcor::RenderContext *ctx) {
        // Offline never denoises -- Ray Reconstruction is temporal and there is
        // nothing temporal about one frame -- so traced and shown are the same
        // size and the accumulator does the work.
        tracer_.setDenoising(false);
        tracer_.resize(opt_.r.width, opt_.r.height, opt_.r.width, opt_.r.height);

        Camera cam;
        const float groundY = world_.terrain.heightM(opt_.camX, opt_.camZ);
        cam.origin = Vec3(opt_.camX, groundY + opt_.eye, opt_.camZ);
        cam.target = cam.origin + Camera::direction(opt_.yaw, opt_.pitch) * 50.0f;
        cam.fovDeg = opt_.fov;
        cam.aperture = opt_.aperture;
        cam.focusDist = opt_.focus > 0.0f ? opt_.focus : 40.0f;
        std::printf("  camera   (%.1f, %.1f, %.1f) fov %.0f focus %.1f m\n", cam.origin.x,
                    cam.origin.y, cam.origin.z, cam.fovDeg, cam.focusDist);

        const Vec3 walkDir = normalize(cam.target - cam.origin);

        // -- AND THE FLOCK, which nothing else here would place --------------
        //
        // renderOffline runs none of the per-frame systems, which is why the
        // fog grid, the cloud cache and the sky table all have to be built by
        // hand below. The butterflies are the same case: without this, the one
        // picture of this wood that gets kept is the only one with nothing
        // flying through it.
        //
        // A SECOND OF THEM, not one tick. The slots fill on the first, but the
        // fade each butterfly materialises through is 0.7 s and the altitude
        // servo needs about as long to lift one off its spawn height onto the
        // glide line -- so a single tick would render a flock of small ones
        // sitting slightly too low.
        //
        // A --walk render moves the camera per sample and the flock does not
        // follow it; over the few metres a walk covers that only means the
        // butterflies are placed for the start of it.
        if (flock_.ready()) {
            // TEN SECONDS, NOT ONE. The songbirds' own warmup below says why at
            // length: a system with a settling time reports its spawn state if
            // you only tick it once. The butterflies have two -- the flap
            // phases spread over a second, and the PAIRING takes longer than
            // that because a pair needs a partner in range, both of them free,
            // and a cooldown to have expired. One second reported 4 of 24
            // chasing where the steady state is 6.
            for (int i = 0; i < 600; ++i) flock_.update(1.0f / 60.0f, world_, cam.origin);
            flock_.publish(world_);
            // The birds settle the same way: the offline path runs no frames,
            // so a population that fills itself over time has to be given the
            // time here or the render shows an empty wood.
            {
                std::vector<Solid> perches;
                world_.collidersNear(cam.origin, kBirdKeepM, &perches);
                for (int i = 0; i < 60; ++i) birds_.update(1.0f / 60.0f, perches, cam.origin);
                birds_.publish(world_);
            }
            // ...AND THE LAKE, for the reason the flock above is ticked: an
            // --out picture of a lake with nothing living on it is a picture of
            // a different lake. A second of it, so the pads have drifted off
            // their spawn and the fish are not all pointing the same way.
            {
                for (int i = 0; i < 600; ++i) lake_.update(1.0f / 60.0f, world_.terrain, cam.origin);
                lake_.publish(world_);
                const auto groundAt = [this](float x, float z) {
                    return float(world_.terrain.heightVox(int(std::floor(x / VOXEL_M)),
                                                          int(std::floor(z / VOXEL_M))) + 1) *
                           VOXEL_M;
                };
                // TWENTY SECONDS, NOT ONE. The butterflies need a second because
                // their fade is 0.7 s; the flock needs far longer for a
                // different reason -- every bird is BORN ON A RING at 0.78-0.94
                // of the keep radius, so one tick of it is nine birds sitting
                // at eighty metres and nothing in the middle. Twenty seconds at
                // 5.5 m/s is a hundred metres of flight, which is the ring
                // crossed: by then they are distributed the way they are in
                // play rather than the way they are spawned.
                // THE GATHER MOVED UP HERE FROM ABOVE THE BUNNIES, because
                // the flock reads it now too and the bunnies used to be the
                // first population in this block that did. A warmup handed an
                // empty list is the "900 ticks of a blind animal" the note
                // below is about, and it fails silently -- the birds fly, the
                // render looks right, and nothing has been checked.
                world_.collidersNear(cam.origin, kBirdKeepM, &perches_);
                for (int i = 0; i < 1200; ++i)
                    flock2_.update(1.0f / 60.0f, cam.origin, groundAt, &perches_);
                flock2_.publish(world_, kButterflySlots + kBirdSlots + kLakeSlots);
                // ...AND THE BUNNIES, warmed the same way and for the same
                // reason: a population with a settling time reports its SPAWN
                // state if you only tick it once, and a bunny spends most of a
                // minute sitting -- one tick and every one of them is still in
                // the pose it was born in.
                // ...and the offline path gathers the same list, or the warmup
                // would run 900 ticks of a blind animal and report where a blind
                // animal ended up. Gathered above the flock now -- same point,
                // same radius, one call.
                for (int i = 0; i < 900; ++i)
                    bunnies_.update(1.0f / 60.0f, cam.origin, groundAt,
                                    [this](float x, float z) { return wetColumnAt(x, z); },
                                    perches_,
                                    [this](float x) { return world_.terrain.woodBit(x); },
                                    [this](float x, float z) { return sandAt(x, z); });
                bunnies_.publish(world_, kBunnySlot0);
                bunnies_.publishSkunks(world_, kMarchSlot0);
                // ...AND THE BEES, off the same two lists the live path
                // gathers. Ticked for long enough that the errands have
                // run: a bee sits on a flower for three seconds and
                // orbits for twenty, so a warmup shorter than that
                // reports the state every one of them was born in.
                world_.decorNear(5, cam.origin, kBeeHiveM, &hivesNear_);
                world_.decorNear(2, cam.origin, kBeeHiveM + kBeeFlowerM, &bloomsNear_);
                for (int i = 0; i < 1800; ++i)
                    bees_.update(1.0f / 60.0f, cam.origin, hivesNear_, bloomsNear_, &perches_);
                bees_.publish(world_, kBeeSlot0);
                {
                    Vec3 bat{0, 0, 0};
                    float bd = 0.0f;
                    long mt[5] = {0, 0, 0, 0, 0};
                    bees_.modeShare(mt);
                    const double tot = double(mt[0] + mt[1] + mt[2] + mt[3] + mt[4]) + 1e-9;
                    if (bees_.nearest(cam.origin, &bat, &bd)) {
                        std::printf("  bee      %d bees at %zu hives (%zu blooms in range), "
                                    "nearest %.0f m at (%.0f, %.0f, %.0f)\n",
                                    bees_.living(), hivesNear_.size(), bloomsNear_.size(),
                                    double(bd), double(bat.x), double(bat.y),
                                    double(bat.z));
                        // A SNAPSHOT OF A STATE MACHINE REPORTS NOTHING --
                        // see Bees::modeShare. This is bee-ticks over the
                        // whole warmup, which is the only way to see
                        // whether the flower errand ever runs at all.
                        std::printf("  bee      time spent: %.0f%% wandering, %.0f%% flying "
                                    "to a flower, %.0f%% sitting on one, %.0f%% flying "
                                    "home, %.0f%% orbiting\n",
                                    100.0 * mt[0] / tot, 100.0 * mt[1] / tot,
                                    100.0 * mt[2] / tot, 100.0 * mt[3] / tot,
                                    100.0 * mt[4] / tot);
                    } else {
                        std::printf("  bee      none -- %zu hives and %zu blooms in "
                                    "range\n",
                                    hivesNear_.size(), bloomsNear_.size());
                    }
                }
                // -- AND THE FOUR SMALL ONES ---------------------------
                //
                // WARMED ON THEIR OWN CLOCK, like the bees above and for the
                // same reason: a ladybug's cruise-descend-sit is tens of
                // seconds long and a frog picks a new cycle roughly every
                // second, so a single tick reports the state every one of them
                // was BORN in rather than the state play sees.
                {
                    const auto cgr = [this](float x, float z) {
                        return float(world_.terrain.heightVox(
                                         int(std::floor(x / VOXEL_M)),
                                         int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
                    };
                    lake_.bankSpots(7u, 8, &banksNear_);
                    for (int i = 0; i < 1800; ++i)
                        critters_.update(1.0f / 60.0f, cam.origin, cgr,
                                         [this](float x, float z) { return wetColumnAt(x, z); },
                                         [this](float x) { return world_.terrain.woodBit(x); },
                                         banksNear_, perches_, isNight(),
                                         Vec3(0.0f, 0.0f, 0.0f),
                                         [this](float x, float z) {
                                             return waterTopAt(x, z);
                                         },
                                         [this](float x, float z) { return sandAt(x, z); });
                    critters_.publish(world_, kCritterSlot0);
                    float gap = 0.0f, worst = 0.0f;
                    const int pairs = critters_.antSpacing(&gap, &worst);
                    // A HEAD-COUNT PROVES THE COLUMN EXISTS; ONLY THE SPACING
                    // PROVES IT IS A COLUMN. Six ants in a heap and six ants in
                    // a line report the same number of ants.
                    std::printf("  ant      %d walking, %d links at %.2f m mean "
                                "(worst %.2f, want %.2f)\n",
                                critters_.livingAnts(), pairs, double(gap), double(worst),
                                double(kAntGap));
                    long lt[3] = {0, 0, 0};
                    critters_.lbugPhaseShare(lt);
                    const double lsum = double(lt[0] + lt[1] + lt[2]) + 1e-9;
                    // ...AND HOW FAR APART THEY ARE. "You have ladybugs flying
                    // together in a pack" was a missing line in the claim --
                    // six slots filling on one frame all took the same
                    // lowest-hash cell. A head-count could never have shown
                    // it; nearest-neighbour spacing is what does.
                    float lmin = 0.0f, lmean = 0.0f;
                    critters_.bugSpread(&lmin, &lmean);
                    std::printf("  ladybug  %d flying, %d down now, nearest two %.1f m "
                                "apart (mean %.1f) -- time spent: %.0f%% "
                                "cruising, %.0f%% descending, %.0f%% landed\n",
                                critters_.livingBugs(), critters_.landedBugs(),
                                double(lmin), double(lmean),
                                100.0 * lt[0] / lsum, 100.0 * lt[1] / lsum, 100.0 * lt[2] / lsum);
                    long ft[4] = {0, 0, 0, 0};
                    critters_.frogCycleShare(ft);
                    const double fsum = double(ft[0] + ft[1] + ft[2] + ft[3]) + 1e-9;
                    // THE WANT IS NOT kFrogMix. Those are SELECTION weights and
                    // these are TICKS, and the four cycles are different lengths
                    // (17/14/24/17) -- so 40/40/10/10 of PICKS is 41/34/15/10 of
                    // TIME. Comparing against the raw mix would read as a fault on
                    // every run of a machine that is working correctly.
                    std::printf("  frog     %d on the bank -- time spent: %.0f%% hopping, "
                                "%.0f%% ribbeting, %.0f%% tongue, %.0f%% turning "
                                "(want 41/34/15/10 -- the 40/40/10/10 mix weighted "
                                "by cycle length)\n",
                                critters_.livingFrogs(), 100.0 * ft[0] / fsum,
                                100.0 * ft[1] / fsum, 100.0 * ft[2] / fsum, 100.0 * ft[3] / fsum);
                    float ymin = 0.0f, ymean = 0.0f;
                    critters_.flySpread(&ymin, &ymean);
                    // WHERE the nearest one is, so a camera can be aimed at a
                    // wing. Half a metre of insect at thirty metres is a
                    // pixel, and a render nobody can find the subject in
                    // proves nothing about how the subject looks.
                    Vec3 yat{0, 0, 0};
                    float yd = 0.0f;
                    if (critters_.nearestFly(cam.origin, &yat, &yd))
                        std::printf("  fly      nearest %.0f m at (%.1f, %.1f, %.1f)\n",
                                    double(yd), double(yat.x), double(yat.y), double(yat.z));
                    std::printf("  fly      %d in the air, bunches of %d, closest two %.2f m "
                                "apart (mean %.2f, body is 0.30)\n",
                                critters_.livingFlies(), kHouseflyPerBunch, double(ymin),
                                double(ymean));
                    float fmin = 0.0f, fmean = 0.0f;
                    critters_.fireflySpread(&fmin, &fmean);
                    std::printf("  firefly  %d alight, nearest two %.1f m apart "
                                "(mean %.1f) (%s)\n",
                                critters_.livingFireflies(), double(fmin), double(fmean),
                                isNight() ? "after dark"
                                                             : "daylight -- none expected");
                }
                Vec3 uat{0, 0, 0};
                float ud = 0.0f;
                if (bunnies_.nearest(cam.origin, &uat, &ud))
                    std::printf("  bunny    %d on the ground, nearest %.0f m at "
                                "(%.0f, %.0f, %.0f)\n",
                                bunnies_.living(), double(ud), double(uat.x), double(uat.y),
                                double(uat.z));
                // THE SECOND LAND MAMMAL, REPORTED SEPARATELY. There is no
                // way to tell "no skunks placed" from "a skunk ninety
                // metres away and four pixels across" out of a picture,
                // which is the whole reason every population here prints a
                // nearest.
                for (int mk = 0; mk < kMarchKinds; ++mk) {
                    Vec3 kat{0, 0, 0};
                    float kd = 0.0f;
                    if (bunnies_.nearestSkunk(mk, cam.origin, &kat, &kd))
                        std::printf("  %-8s %d marching, nearest %.0f m at "
                                    "(%.0f, %.0f, %.0f)\n",
                                    kMarchSpec[mk].name, bunnies_.skunksLiving(mk),
                                    double(kd), double(kat.x), double(kat.y),
                                    double(kat.z));
                    else
                        std::printf("  %-8s none placed (%s)\n", kMarchSpec[mk].name,
                                    woodsName(kMarchSpec[mk].woods));
                }
                // -- DOES THE HOP IN PLACE MATCH THE HOP FORWARD? ------------
                //
                // A TURN IS A HOP THAT GOES NOWHERE, and the wood draws both
                // from the same eleven frames, so the body has to leave the
                // ground by the same amount in both or one animal is jumping
                // two different ways. That is a difference between two STATES,
                // which is why no still frame catches it -- each one on its own
                // looks right.
                //
                // Sampled over six hundred more ticks rather than read once: at
                // any instant most of the population is sitting, and a snapshot
                // of a state nobody is in reports nothing at all.
                {
                    float peak[3] = {0, 0, 0};
                    long long seen[3] = {0, 0, 0};
                    for (int f = 0; f < 600; ++f) {
                        // THE SAME FIVE PREDICATES THE WOOD RUNS ON. A report
                        // that measures an animal with a different set of rules
                        // is measuring a different animal -- the birch gate and
                        // the beach gate both change where a marcher may go.
                        bunnies_.update(1.0f / 60.0f, cam.origin, groundAt,
                                        [this](float x, float z) { return wetColumnAt(x, z); },
                                        perches_,
                                        [this](float x) { return world_.terrain.woodBit(x); },
                                        [this](float x, float z) { return sandAt(x, z); });
                        for (int k = 0; k < bunnies_.slots(); ++k) {
                            Bunnies::Probe pr;
                            if (!bunnies_.probe(k, &pr)) continue;
                            ++seen[pr.state];
                            if (pr.lift > peak[pr.state]) peak[pr.state] = pr.lift;
                        }
                    }
                    std::printf("  bunny    arc peak: sit %.3f m, turn %.3f m, hop %.3f m"
                                "   (%lld / %lld / %lld samples)\n",
                                double(peak[0]), double(peak[1]), double(peak[2]), seen[0],
                                seen[1], seen[2]);
                    if (seen[1] && seen[2] && fabsf(peak[1] - peak[2]) > 0.005f)
                        std::printf("    ^ THE TURN AND THE HOP DO NOT MATCH\n");
                }
                Vec3 bat{0, 0, 0};
                float bd = 0.0f;
                if (flock2_.nearest(cam.origin, &bat, &bd))
                    std::printf("  flock    %d songbirds in the air, %d of them chasing, "
                                "nearest %.0f m at (%.0f, %.0f, %.0f)\n",
                                flock2_.flying(), flock2_.chasing(), double(bd), double(bat.x),
                                double(bat.y), double(bat.z));
                if (lake_.living()) {
                    float llo = 0, lmean = 0, lhi = 0;
                    lake_.spread(cam.origin, &llo, &lmean, &lhi);
                    char sch[128];
                    const int ns = lake_.schools(sch, sizeof(sch), 0);
                    char smin[128];
                    const int nm = lake_.schools(smin, sizeof(smin), 3);
                    std::printf("  lake     %d living on the water, %.0f / %.0f / %.0f m "
                                "nearest / mean / farthest\n",
                                lake_.living(), double(llo), double(lmean), double(lhi));
                    int nf[8] = {};
                    int npd = 0, nfl = 0;
                    lake_.census(nf, &npd, &nfl);
                    std::printf("  lake     %d salmon, %d bass, %d koi, %d minnows, %d catfish, "
                                "%d blue gill, %d lily pads, %d dragonflies, "
                                "%d ducks + %d ducklings\n",
                                nf[0], nf[1], nf[2], nf[3], nf[4], nf[5], npd, nfl,
                                lake_.ducksLiving(true), lake_.ducksLiving(false));
                    std::printf("  school   salmon: %d school%s (%s) | minnows: %d school%s "
                                "(%s)   %.2f m off station on average\n",
                                ns, ns == 1 ? "" : "s", sch, nm, nm == 1 ? "" : "s", smin,
                                double(lake_.stationErr()));
                    // A DUCK DOES NOT SWIM THROUGH A LILY PAD, and the only
                    // way to say that is to count it -- see
                    // LakeLife::duckOffPads. The NEAR count is what makes
                    // the clash count mean anything: zero clashes over a run
                    // where nothing ever met a leaf is not a result.
                    long dnear = 0, dclash = 0;
                    float dworst = 0.0f, dgap = 0.0f;
                    lake_.duckPads(&dnear, &dclash, &dworst, &dgap);
                    std::printf("  ducks    closest a duck ever came to a lily pad %.2f m; "
                                "%ld duck-ticks within half a metre of one, %ld of them "
                                "INSIDE it (deepest %.3f m)%s\n",
                                double(dgap), dnear, dclash, double(dworst),
                                dclash ? "  <-- CLIPPING" : "");
                }
            }
            world_.refitTlas();
            Vec3 at{0, 0, 0};
            float d = 0.0f, lo = 0.0f, hi = 0.0f;
            flock_.band(&lo, &hi);
            if (flock_.nearest(cam.origin, &at, &d))
                std::printf("  flock    %d butterflies, %d of them chasing, %.1f to %.1f m "
                            "up, nearest %.1f m at (%.1f, %.1f, %.1f)\n",
                            flock_.flying(), flock_.chasing(), double(lo), double(hi), double(d),
                            double(at.x), double(at.y), double(at.z));
            else
                std::printf("  flock    %d butterflies\n", flock_.flying());
        }

        // -- LIGHT THE FOG GRID, WHICH OFFLINE NEVER DID --------------------
        //
        // A fog-enabled --out render came out SOLID BLACK, and had done since
        // the froxel grid replaced the analytic fog. Nothing here ever wrote
        // the grid, and the tracer reads its alpha as TRANSMITTANCE: an
        // untouched grid reads zero, zero transmittance multiplies the whole
        // frame away, and the only offline render that ever looked right was
        // one with --fog 0. The interactive path was never affected, because it
        // lights the grid every frame before the trace.
        //
        // ONCE IS ENOUGH FOR A STILL CAMERA. With no history the injection pass
        // takes this frame whole rather than blending, and a camera that is not
        // moving has nothing left to converge. A --walk render moves per
        // sample, so it relights inside the loop below.

        // FILL THE CLOUD CACHE TO COMPLETION FIRST. The interactive path fills
        // a band a frame and lets the deck arrive over the first few frames;
        // an offline render has no "next frame" to finish in, and a march
        // against a part-written volume would put clouds over half the sky and
        // nothing over the other half.
        while (clouds_.available() && !clouds_.filled()) clouds_.update(ctx);

        // AND BUILD THE SKY, for exactly the reason the fog note above gives.
        // The interactive path rebuilds the sky-view table on any frame the sun
        // has moved; offline there is no such frame, so without this the table
        // would never be built at all, Atmosphere::active() would stay false,
        // and --atmosphere would quietly render the Preetham sky instead. A
        // silent fallback rather than the fog's black frame, which is worse:
        // the render would look plausible and be the wrong model.
        if (atmo_.enabled && atmo_.available()) {
            if (syncSunToSky()) world_.sky.setSun(opt_.sunAz, opt_.sunEl);
            atmo_.update(ctx, world_.sky.gpu(), cam.origin.y,
                         Sky::SUN_IRRADIANCE * world_.sky.sunScale);
        }
        const bool fogPerStep = opt_.walk > 0.0f;
        if (!fogPerStep) {
            tracer_.renderVolFog(ctx, cam.gpu(tracer_.width(), tracer_.height()),
                                 opt_.r.fogDensity, opt_.r.fogHeight, false);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < opt_.r.spp; ++s) {
            Camera c = cam;
            if (opt_.walk > 0.0f) {
                const Vec3 step = walkDir * (opt_.walk * float(s));
                c.origin = cam.origin + step;
                c.target = cam.target + step;
                // The film is thrown away every frame, exactly as it is when
                // the camera actually moves. This IS the thing being measured:
                // a still image converges, a moving one is back to one sample.
                tracer_.resetAccumulation();
            }
            if (fogPerStep) {
                tracer_.renderVolFog(ctx, c.gpu(tracer_.width(), tracer_.height()),
                                     opt_.r.fogDensity, opt_.r.fogHeight, true);
            }
            // THE TOOL IS IN AN OFFLINE RENDER TOO, and it has to be set here
            // for the reason the fog and the sky above are: renderOffline runs
            // none of the per-frame systems, so anything hung off the
            // interactive tick is simply absent from every --out image. At
            // REST, though -- update() is never called here, so the swing clock
            // never advances and the sway never starts, which is exactly what a
            // still frame accumulating a thousand samples wants.
            //
            // UNVERIFIED, and honestly so: --out segfaults before it writes,
            // and it does so on the commit before this file gained a viewmodel
            // as well -- measured, 2026-09-06, by stashing every change here and
            // rebuilding. So this line is written to be right rather than
            // observed to be, and whoever fixes that crash should look at the
            // tool in the first image it produces.
            {
                const HeldXform hx =
                    held_.xform(c.gpu(tracer_.width(), tracer_.height()), 0.0f, 0.0f);
                world_.setHeldInstance(held_.model(), hx.m, hx.tx, hx.ty, hx.tz, hx.show);
                arrows_.publish(world_);
            bullets_.publish(world_);
                world_.refitTlas();
            }
            tracer_.renderSample(ctx, c.gpu(tracer_.width(), tracer_.height()), opt_.r);
            if ((s % 16) == 15 || s + 1 == opt_.r.spp) {
                ctx->submit(true);
                std::printf("\r  render   %5.1f%%  (%.1f s)", 100.0 * (s + 1) / opt_.r.spp,
                            secondsSince(t0));
                std::fflush(stdout);
            }
        }
        const double sec = secondsSince(t0);
        std::printf("\r  render   %.2f s -- %.1f Mpaths/s                    \n", sec,
                    double(opt_.r.width) * opt_.r.height * opt_.r.spp / sec / 1e6);

        tracer_.resolve(ctx, opt_.r, false);
        if (!tracer_.writePng(ctx, opt_.out)) {
            std::fprintf(stderr, "v2: could not write %s\n", opt_.out.c_str());
            return;
        }
        std::printf("  wrote    %s\n", opt_.out.c_str());

        if (opt_.writeHdr) {
            const std::string p = opt_.out.substr(0, opt_.out.find_last_of('.')) + ".pfm";
            if (tracer_.writePfm(ctx, p)) std::printf("  wrote    %s\n", p.c_str());
        }
    }

    // -----------------------------------------------------------------------
    // Where you wake up.
    //
    // The world is endless and a pure function of the seed, so there is no
    // reason to start in the same clearing every time -- and a wood you have
    // already learnt the shape of is a wood you stop looking at. The SEED is
    // what varies, not the world: same --seed, same trees, different corner of
    // them. It is printed at startup, so a spot worth finding again can be.
    //
    // A RANDOM POINT IS NOT A PLACE ANYONE WOULD STAND, which is most of the
    // work here. The candidate has to be out of the water with a margin, on
    // ground shallow enough that the terrain would grow trees on it rather than
    // leave it as scree, and inside a stand rather than out on a bald ridge --
    // the same density field the trees are planted from, read at the same
    // threshold, so "where the wood is" needs no second answer.
    //
    // All three tests are pure functions of position, so this runs BEFORE a
    // single chunk exists and costs a few hundred evaluations of the height
    // field. What it cannot see is the trees themselves, which are placed per
    // chunk -- that is what nudgeOutOfSolids is for, afterwards.
    // -----------------------------------------------------------------------
    // groundShade()'s hash, on the host, so --ground-stats can report the
    // scatter the device will actually draw.
    static uint32_t hashVoxel3(int x, int y, int z) {
        uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 1103515245u +
                     uint32_t(z) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    // Mirrors skyPerez() in Sky.slang exactly, on the host, so --ground-stats
    // can report what a patch of ground actually receives.
    Vec3 domeRadiance(const V6Sky &g, Vec3 d) const {
        const Vec3 dn = normalize(d);
        const float cosTheta = maxf(dn.y, 0.01f);
        const float cosGamma =
            clampf(dn.x * g.sunDir.x + dn.y * g.sunDir.y + dn.z * g.sunDir.z, -1.0f, 1.0f);
        const float gamma = acosf(cosGamma);
        auto perez = [&](float a, float b, float c, float dd, float e) {
            const float ct = maxf(cosTheta, 0.01f);
            return (1.0f + a * expf(b / ct)) * (1.0f + c * expf(dd * gamma) + e * cosGamma * cosGamma);
        };
        const float vx = g.zenith.x * perez(g.A.x, g.B.x, g.C.x, g.D.x, g.E.x) / maxf(1e-6f, g.normF.x);
        const float vy = g.zenith.y * perez(g.A.y, g.B.y, g.C.y, g.D.y, g.E.y) / maxf(1e-6f, g.normF.y);
        const float vz = g.zenith.z * perez(g.A.z, g.B.z, g.C.z, g.D.z, g.E.z) / maxf(1e-6f, g.normF.z);
        const float Y = maxf(0.0f, vx), x = vy, y = maxf(1e-4f, vz);
        const float X = (x / y) * Y, Z = ((1.0f - x - y) / y) * Y;
        Vec3 rgb(3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
                 -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
                 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z);
        rgb = Vec3(maxf(rgb.x, 0.0f), maxf(rgb.y, 0.0f), maxf(rgb.z, 0.0f));
        return rgb * g.skyScale;
    }

    void skyStats() {
        const V6Sky g = world_.sky.gpu();
        const Vec3 sun(g.sunDir.x, g.sunDir.y, g.sunDir.z);
        const Vec3 sunRad(g.sunRadiance.x, g.sunRadiance.y, g.sunRadiance.z);
        const float omega = 6.2831853f * (1.0f - SUN_COS_THETA_MAX);
        const Vec3 sunIrr = sunRad * (omega * maxf(0.0f, sun.y));

        Vec3 skyIrr(0.0f, 0.0f, 0.0f);
        const int N = 16384;
        for (int i = 0; i < N; ++i) {
            const float u1 = hashUnit(7771u, uint32_t(i));
            const float u2 = hashUnit(9973u, uint32_t(i));
            const float ct = sqrtf(1.0f - u1), st = sqrtf(u1);
            const float ph = 6.2831853f * u2;
            skyIrr = skyIrr + domeRadiance(g, Vec3(st * cosf(ph), ct, st * sinf(ph)));
        }
        skyIrr = skyIrr * (3.14159265f / float(N));

        auto lum = [](Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
        std::printf("\nsun elevation %.1f deg   turbidity %.1f\n",
                    asinf(maxf(0.0f, sun.y)) * 57.29578f, world_.sky.turbidity);
        std::printf("  sun irradiance, flat patch  %10.3f %10.3f %10.3f   lum %8.3f\n",
                    sunIrr.x, sunIrr.y, sunIrr.z, lum(sunIrr));
        std::printf("  sky irradiance, flat patch  %10.3f %10.3f %10.3f   lum %8.3f\n",
                    skyIrr.x, skyIrr.y, skyIrr.z, lum(skyIrr));
        std::printf("  sun : sky = %.1f : 1   -- a fully shadowed patch keeps %.1f%% of the light\n",
                    lum(sunIrr) / maxf(1e-9f, lum(skyIrr)),
                    100.0f * lum(skyIrr) / maxf(1e-9f, lum(sunIrr) + lum(skyIrr)));
        std::printf("  for reference, a real clear midday sky is about 6:1 (14%%)\n");
        std::fflush(stdout);
    }

    // A CROSS-SECTION OF WHAT THE FEET WOULD FIND, straight through the middle
    // of the nearest boulder.
    //
    // This exists because the bug it checks for is invisible from a screenshot.
    // The old collider gave every point inside a rock's footprint the SAME
    // height -- the model's highest voxel -- so the profile below would come
    // back as a flat plateau with vertical walls, while the rock on screen is a
    // dome. Anything that follows the stone rises and falls across it.
    void collideProbe() {
        // A far wider net than the walk uses -- collidersNear takes 6 m, which
        // is right for a body and no use at all for finding a rock to measure.
        // The camera's spot, not player_.pos -- the body is placed on the
        // ground later in onLoad, so at this point it is still at the origin
        // and the ring is nowhere near it.
        const Vec3 probeAt(opt_.camX, 0.0f, opt_.camZ);
        world_.collidersNear(probeAt, 120.0f, &solids_);
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = solids_.data();
        w.solidCount = int(solids_.size());
        std::printf("collide probe: %d colliders within 120 m\n", w.solidCount);
        int nStand = 0, nBouncy = 0, nField = 0;
        for (int i = 0; i < w.solidCount; ++i) {
            if (w.solids[i].standable) ++nStand;
            if (w.solids[i].bouncy) ++nBouncy;
            if (w.solids[i].col) ++nField;
        }
        std::printf("  %d standable, %d bouncy (mushrooms), %d with heightfields\n",
                    nStand, nBouncy, nField);
        const Solid *best = nullptr;
        float bestD = 1e30f;
        for (int i = 0; i < w.solidCount; ++i) {
            const Solid &s = w.solids[i];
            if (!s.standable || !s.col) continue;
            const float dx = s.cx - probeAt.x, dz = s.cz - probeAt.z;
            const float d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; best = &s; }
        }
        if (!best) {
            std::printf("collide probe: no standable heightfield solid in the ring\n");
            return;
        }
        std::printf("\ncollide probe -- ground height across the nearest rock\n");
        std::printf("  centre (%.2f, %.2f)  half extents %.2f x %.2f  model top %.2f m\n",
                    best->cx, best->cz, best->hx, best->hz, best->top);
        const float span = best->hx * 1.6f;
        std::printf("  x offset :");
        for (int i = -10; i <= 10; ++i) std::printf(" %5.1f", float(i) / 10.0f * span);
        std::printf("\n  height   :");
        for (int i = -10; i <= 10; ++i) {
            const float x = best->cx + float(i) / 10.0f * span;
            std::printf(" %5.2f", player_.groundHeight(w, x, best->cz));
        }
        std::printf("\n");
        // The same line asking the model directly, so a difference between the
        // two rows is the body's own width rounding the profile off rather than
        // the collider disagreeing with the geometry.
        std::printf("  column   :");
        for (int i = -10; i <= 10; ++i) {
            const float x = best->cx + float(i) / 10.0f * span;
            float y = 0.0f;
            if (solidColumnTop(*best, x, best->cz, VOXEL_M, &y)) std::printf(" %5.2f", y);
            else std::printf("     -");
        }
        std::printf("\n");
    }

    void groundStats() {
        const VoxelTerrain &t = world_.terrain;
        std::printf("\nground make-up, 200 m squares, %% of columns\n");
        std::printf("   region        grass   soil   litter   rock   sand\n");
        double gAll = 0, sAll = 0, lAll = 0, n = 0;
        for (int ry = -2; ry <= 2; ++ry)
            for (int rx = -2; rx <= 2; ++rx) {
                const float ox = float(rx) * 900.0f, oz = float(ry) * 900.0f;
                int cnt[5] = {0, 0, 0, 0, 0};
                int total = 0;
                TerrainMemo memo;
                for (int j = 0; j < 200; ++j)
                    for (int i = 0; i < 200; ++i) {
                        const int ci = int((ox + float(i)) / VOXEL_M);
                        const int cj = int((oz + float(j)) / VOXEL_M);
                        const int h = t.heightVox(ci, cj, memo);
                        const uint8_t m = t.topMaterial(ci, cj, h, memo);
                        ++total;
                        if (isGrass(m)) ++cnt[0];
                        else if (isSoil(m)) ++cnt[1];
                        else if (isLitter(m)) ++cnt[2];
                        else if (m == mat::ROCK) ++cnt[3];
                        else ++cnt[4];
                    }
                const double f = 100.0 / double(total);
                std::printf("  %6.0f,%6.0f   %5.1f  %5.1f   %5.1f  %5.1f  %5.1f\n", ox, oz,
                            cnt[0] * f, cnt[1] * f, cnt[2] * f, cnt[3] * f, cnt[4] * f);
                gAll += cnt[0] * f;
                sAll += cnt[1] * f;
                lAll += cnt[2] * f;
                n += 1.0;
            }
        std::printf("  mean          %5.1f  %5.1f   %5.1f\n", gAll / n, sAll / n, lAll / n);

        // WHICH SHADE a voxel takes is decided on the DEVICE now, so counting
        // what the mesher emitted would only ever report one slot per family.
        // The hash it uses is reproduced here instead, over a block of real
        // voxels, which is the thing worth checking: a scatter that is not flat
        // is a floor with a favourite colour.
        int gs[16] = {0}, ss[16] = {0};
        TerrainMemo memo;
        for (int j = 0; j < 700; ++j)
            for (int i = 0; i < 700; ++i) {
                const int ci = int(float(i) * 3.0f / VOXEL_M);
                const int cj = int(float(j) * 3.0f / VOXEL_M);
                const int h = t.heightVox(ci, cj, memo);
                const uint8_t m = t.topMaterial(ci, cj, h, memo);
                const uint32_t shade = hashVoxel3(ci, h, cj);
                if (isGrass(m)) ++gs[shade % mat::GRASS_COUNT];
                else if (isSoil(m)) ++ss[shade % mat::SOIL_COUNT];
            }
        std::printf("  grass shades ");
        for (int k = 0; k < mat::GRASS_COUNT; ++k) std::printf(" %6d", gs[k]);
        std::printf("\n  soil shades  ");
        for (int k = 0; k < mat::SOIL_COUNT; ++k) std::printf(" %6d", ss[k]);
        std::printf("\n");
        std::fflush(stdout);
    }

    #include "world/app_spawn.inl"
