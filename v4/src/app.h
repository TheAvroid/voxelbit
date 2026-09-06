// ---------------------------------------------------------------------------
// app.h -- walking around inside the render.
//
// The bargain is unchanged from every engine in this lineage, and it survives
// the move to Falcor for the same reason it survived the move to the GPU: it is
// about what the eye can use, not about what the renderer is.
//
//   MOVING   one sample per pixel and a short path. Noisy, but the frame rate
//            is high enough that the noise is moving too, and you can navigate
//            by it.
//   STILL    keep the accumulation, add a sample per frame, let the path run to
//            full length. The image converges under you while you stand still.
//
// CONTROLS follow the earlier engines deliberately, so the muscle memory
// carries between them. ESC always takes two presses -- the first gives the
// mouse back, the second quits -- because a rule that is only sometimes true is
// worse than one that is never true.
//
// WHAT THE FRAMEWORK REPLACED. v2 owned a GLFW window, a fixed-function OpenGL
// blit, a hand-rolled GDI text panel and a settings menu drawn as characters
// into a bitmap, with its own hit testing so a click could find a row. All of
// that was infrastructure for showing a number and offering a slider, and it is
// gone: Falcor's SampleApp brings the window, the swapchain and Dear ImGui, so
// onGuiRender below is the whole settings menu and the whole readout.
//
// The one thing the framework does NOT bring is mouse capture, so that is still
// done by hand -- see setCapture.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/SampleApp.h"
#include "Core/API/RenderContext.h"
#include "Utils/UI/Gui.h"
#include "Utils/UI/InputState.h"

#include <cstdio>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/defaults.h"
#include "gpu/tracer.h"
#include "gpu/world.h"
#include "render/camera.h"
#include "render/player.h"
#include "scene/daynight.h"

namespace v4 {

using Falcor::Fbo;
using Falcor::Gui;
namespace Input = Falcor::Input;  // a namespace, not a type
using Falcor::KeyboardEvent;
using Falcor::MouseEvent;
using Falcor::SampleApp;
using Falcor::SampleAppConfig;

// ---------------------------------------------------------------------------
// Everything the command line can set. Parsed in main.cpp.
// ---------------------------------------------------------------------------
struct Options {
    RenderSettings r;

    std::string out = "forest.png";
    bool outGiven = false;  // absent means: open a window instead
    bool writeHdr = false;
    bool stats = false;

    // Open minimised, never take the foreground, never take the mouse.
    //
    // For AUTOMATED launches -- benchmarks, screenshots, anything a script
    // starts while a person is using the machine. Without it every test run
    // yanks the cursor out of whatever window had it, which is intolerable if
    // you are playing your own copy at the time.
    bool background = false;

    float scale = defaults::kScale;  // traced resolution as a fraction of the window

    // DLSS RAY RECONSTRUCTION, on by default.
    //
    // It is not an optional polish step here, it is the answer to the thing
    // every engine in this lineage has been worst at: a path tracer that
    // accumulates looks superb standing still and like television static the
    // moment you walk, because walking throws the film away. Reconstruction
    // carries the history across the motion instead, so walking and standing
    // look the same -- which is the same goal `constantGrain` had, reached from
    // the other end and at a far better noise level.
    //
    // --no-dlss turns it off and hands the frame back to the accumulator, which
    // is also what happens automatically if the driver or GPU cannot do it.
    bool dlss = true;
    DlssQuality dlssQuality = DlssQuality::Quality;

    // Run the viewer for a fixed number of frames, write the result and quit.
    //
    // This exists because a TEMPORAL renderer cannot be checked any other way.
    // --out renders one frame offline, which is exactly the case Ray
    // Reconstruction does not apply to: it has no history on frame one, and
    // what it is being judged on is what it converges to over a couple of
    // seconds of walking. So the only honest screenshot of it is one taken from
    // the running viewer, after it has had frames to work with.
    //
    // It also walks, optionally, because the whole argument for reconstruction
    // is about what the picture looks like WHILE MOVING -- a still comparison
    // flatters the accumulator, which converges beautifully when nothing moves.
    std::string shotPath;
    int shotFrame = 240;
    // Hold W while the frames run. The point is to judge the picture WHILE
    // MOVING -- a still comparison flatters the accumulator, which converges
    // beautifully when nothing moves and is exactly not the case in question.
    bool shotWalk = false;
    // Simulated seconds per frame during a capture, INSTEAD of the wall clock.
    //
    // Without this a capture is not reproducible and two of them are not
    // comparable: the walk is driven by dt, so a run at 240 fps covers a
    // quarter of the ground a run at 60 fps does over the same frame count, and
    // an A/B between two renderers of different speeds ends up comparing two
    // different places in the wood. Pinning dt makes N frames mean the same
    // journey every time.
    float shotDt = 1.0f / 60.0f;
    int movingDepth = defaults::kMovingDepth;
    float speed = defaults::kSpeed;
    float eye = defaults::kEye;
    float timeOfDay = defaults::kTimeOfDay;
    float cycleSpeed = defaults::kCycleSpeed;

    // Samples traced per DISPLAYED frame.
    //
    // The headroom knob, and it exists because the still image was never the
    // problem -- a still camera piles up samples until it converges, and a
    // moving one starts again from one sample every frame. Spending spare frame
    // time on more of them is the cheapest noise reduction available: it is the
    // same estimator, so noise falls as sqrt(n) with no blur, no history,
    // nothing to go wrong on a disocclusion.
    //
    // ONE by default, because this is a frame-rate decision and it is not mine
    // to make. The menu row moves it live with the fps counter above it.
    int samplesPerFrame = 1;

    // CONSTANT GRAIN: hold a still camera at exactly the noise a moving one
    // has, so the picture never settles.
    //
    // Asked for deliberately, and it is not a bug being papered over. Noise
    // that erupts the moment you walk and melts away the moment you stop is
    // noise you WATCH -- the change is what the eye catches, not the level.
    // Pinned flat it stops being an event and reads as film grain, and the
    // frame looks the same whatever the camera is doing.
    bool constantGrain = true;

    int view = 12;
    float treeDensity = 0.31f;
    float grass = 0.105f, flowers = 0.45f, rocks = 0.010f;
    int grassMin = 3, grassMax = 6;
    std::string pines = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decor = "C:/voxelbit/game/assets/decoration";

    float sunAz = defaults::kSunAz;
    float sunEl = defaults::kSunEl;
    float turbidity = 2.8f;

    float camX = -6.0f, camZ = 34.0f;
    float yaw = 205.0f, pitch = 7.0f;
    float fov = defaults::kFov;
    float aperture = 0.055f;
    float focus = 0.0f;
    // Metres to advance the camera per frame, offline only. This exists to make
    // the noise while WALKING measurable: comparing two hand-flown screenshots
    // compares two different views as much as two settings. With --walk the
    // camera path is identical in both runs.
    float walk = 0.0f;

    Options() {
        r.width = defaults::kWidth;
        r.height = defaults::kHeight;
        r.maxDepth = defaults::kDepth;
        r.exposure = defaults::kExposure;
    }
};

// Measured on an RTX 4070 at a maximised 3820x1990 window under the OptiX
// engine, and carried over unchanged: they encode a RELATIONSHIP, not a frame
// rate. The single most useful thing they say is that PIXELS COST AND BOUNCES
// ARE NEARLY FREE -- going from three bounces to eight costs a few percent,
// while halving the render scale nearly doubles the frame rate. Left to a
// command line, the instinct is always to cut bounces first, which gives up
// most of the lighting for almost none of the speed.
struct Preset {
    const char *name;
    float scale;
    int depth;
    const char *note;
};

inline const std::vector<Preset> &presets() {
    static const std::vector<Preset> kPresets = {
        {"Max FPS", 0.25f, 3, "noisy until it settles"},
        {"Fast", 0.35f, 8, "full lighting"},
        {"Balanced", 0.50f, 6, ""},
        {"Sharp", 0.70f, 8, ""},
        {"Native", 1.00f, 12, "every pixel traced"},
    };
    return kPresets;
}

// ---------------------------------------------------------------------------
class ForestApp : public SampleApp {
  public:
    ForestApp(const SampleAppConfig &config, const Options &opt) : SampleApp(config), opt_(opt) {}

    // -----------------------------------------------------------------------
    void onLoad(Falcor::RenderContext *ctx) override {
        world_.seed = opt_.r.seed;
        world_.terrain.grassDensity = clampf(opt_.grass, 0.0f, 1.0f);
        world_.flowerDensity = clampf(opt_.flowers, 0.0f, 1.0f);
        world_.rockDensity = clampf(opt_.rocks, 0.0f, 1.0f);
        world_.treeDensity = clampf(opt_.treeDensity, 0.0f, 1.0f);
        world_.pineDir = opt_.pines;
        world_.decorDir = opt_.decor;
        world_.viewChunks = mini(15, maxi(1, opt_.view));
        world_.terrain.grassMinRows = maxi(1, opt_.grassMin);
        world_.terrain.grassMaxRows = maxi(opt_.grassMin, opt_.grassMax);
        // A stream of its own, so re-seeding the wood does not also reshuffle
        // every blade of grass in it -- the two are independent things to want
        // varied.
        world_.terrain.strandSeed = opt_.r.seed + 991u;
        world_.sky.turbidity = opt_.turbidity;
        world_.sky.setSun(opt_.sunAz, opt_.sunEl);

        auto t0 = std::chrono::steady_clock::now();
        if (!world_.build(getDevice(), ctx)) {
            shutdown(1);
            return;
        }
        std::printf("  models   %.2f s -- %d pines, %d rocks, %d flowers, %d materials\n",
                    secondsSince(t0), world_.loadedPines, world_.loadedRocks, world_.loadedFlowers,
                    world_.palette.used());
        std::printf("           %.2f M unique tris in %d models at %.0f cm voxels\n",
                    world_.uniqueTris / 1e6,
                    world_.loadedPines + world_.loadedRocks + world_.loadedFlowers,
                    VOXEL_M * 100.0f);

        // Fill the ring before the first frame, so nobody sees a hole in the
        // ground while the workers catch up.
        t0 = std::chrono::steady_clock::now();
        const float gy = world_.terrain.heightM(opt_.camX, opt_.camZ);
        world_.primeBlocking(Vec3(opt_.camX, gy + opt_.eye, opt_.camZ));
        std::printf(
            "  world    %.2f s -- %zu chunks of %.1f m resident, %.1f M tris, %zu instances\n",
            secondsSince(t0), world_.chunkCount(), CHUNK_M, world_.residentTris() / 1e6,
            world_.instanceCount());
        std::printf("           %.0f ms of that was structure building, %.0f MB of tri pool\n",
                    world_.buildMs(), double(world_.poolBytes()) / (1024.0 * 1024.0));

        tracer_.init(getDevice(), &world_);

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
        tracer_.setQuality(opt_.dlssQuality);

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
        if (opt_.outGiven) {
            renderOffline(ctx);
            shutdown(0);
            return;
        }

        player_.walk = opt_.speed;
        player_.eye = opt_.eye;
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

        printHelp();
        lastTime_ = std::chrono::steady_clock::now();
    }

    // -----------------------------------------------------------------------
    void onFrameRender(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) override {
        if (opt_.outGiven) return;

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

        clock_.advance(dt);
        applySun(false);

        // Stream the world around the camera. A changed ring means new geometry
        // in frame, so what has been accumulated for the old one no longer
        // describes it.
        if (world_.update(pos_)) tracer_.resetAccumulation();
        if (processInput(dt)) tracer_.resetAccumulation();

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

        // -- how many pixels get traced, and what gets shown ------------------
        //
        // WITH RECONSTRUCTION the two are different numbers and DLSS owns the
        // relationship: the quality mode picks the input size for a given
        // output, and it is asked rather than assumed because the optimal ratio
        // for a mode is the model's business and has changed between versions.
        //
        // WITHOUT IT they are the same number and --scale is a plain fraction,
        // with the swapchain blit doing the stretching. That is the old
        // behaviour, kept intact so the two can be compared honestly.
        bool useDlss = opt_.dlss && dlss_.available();
        int rw = maxi(16, int(float(fw) * opt_.scale));
        int rh = maxi(16, int(float(fh) * opt_.scale));
        if (useDlss) {
            uint2 rs{0, 0};
            if (dlss_.optimalRenderSize(uint2(fw, fh), opt_.dlssQuality, &rs) && rs.x && rs.y) {
                rw = int(rs.x);
                rh = int(rs.y);
            } else {
                useDlss = false;
            }
        }
        tracer_.setDenoising(useDlss);
        tracer_.resize(rw, rh, useDlss ? int(fw) : rw, useDlss ? int(fh) : rh);

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

        Camera cam;
        cam.origin = pos_;
        cam.target = pos_ + forward() * 50.0f;
        cam.fovDeg = fov_;
        cam.aperture = 0.0f;  // a pinhole here; depth of field is for stills
        cam.focusDist = 40.0f;
        const V4Camera gcam = cam.gpu(tracer_.width(), tracer_.height());

        // Constant grain: start from nothing EVERY frame, not just when the
        // camera moves. Moving already did this -- it is what made a walking
        // frame noisy -- so doing it always is what makes the two identical.
        const int spf = maxi(1, opt_.samplesPerFrame);
        bool reconstructed = false;
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
            for (int i = 0; i < spf; ++i) tracer_.renderSample(ctx, gcam, cfg);
            reconstructed = tracer_.reconstruct(ctx, dlss_);
        } else {
            // Constant grain: start from nothing EVERY frame, not just when the
            // camera moves. Moving already did this -- it is what made a walking
            // frame noisy -- so doing it always is what makes the two identical.
            if (opt_.constantGrain) tracer_.resetAccumulation();
            for (int i = 0; i < spf; ++i) tracer_.renderSample(ctx, gcam, cfg);
        }
        tracer_.resolve(ctx, cfg, reconstructed);

        // Guarded because a background instance may have nothing on screen to
        // blit to: it goes on tracing at the size it was asked for, and simply
        // does not present.
        if (target->getWidth() > 0 && target->getHeight() > 0)
            ctx->blit(tracer_.display()->getSRV(), target->getRenderTargetView(0));

        if (shotRequested_) {
            shotRequested_ = false;
            char name[64];
            std::snprintf(name, sizeof(name), "v4_shot_%03d.png", shotIndex_++);
            if (tracer_.writePng(ctx, name))
                std::printf("v4: wrote %s\n", name);
            else
                std::fprintf(stderr, "v4: could not write %s\n", name);
            std::fflush(stdout);
        }

        moving_ = false;  // cleared only once the frame it applied to is drawn

        // -- the scripted capture, if one was asked for -----------------------
        if (!opt_.shotPath.empty()) {
            ++shotFrames_;
            if (shotFrames_ >= opt_.shotFrame) {
                if (tracer_.writePng(ctx, opt_.shotPath))
                    std::printf("v4: wrote %s after %d frames (%s)\n",
                                opt_.shotPath.c_str(), shotFrames_,
                                tracer_.denoising() ? "reconstructed" : "accumulated");
                else
                    std::fprintf(stderr, "v4: could not write %s\n", opt_.shotPath.c_str());
                std::fflush(stdout);
                shutdown(0);
            }
        }

        // The frame rate the menu reports. Falcor tracks one of its own, but it
        // is smoothed over a different window and the number beside a setting
        // has to be the number that setting moved.
        fpsAccum_ += wallDt;
        ++fpsFrames_;
        if (fpsAccum_ >= 0.5) {
            fps_ = float(fpsFrames_ / maxf(1e-4f, float(fpsAccum_)));
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

    // -----------------------------------------------------------------------
    // The readout, and the settings menu on Y.
    //
    // Every knob here is also a launch flag, and that is deliberate: the flags
    // were unusable as a way to FIND a setting, because choosing between them
    // meant knowing the answer already. Quitting, editing a command line and
    // reloading a scene of tens of millions of triangles to try one number is
    // not a way to learn what the number does. In here the change lands on the
    // next frame and the frame rate two lines above it reacts, so the trade is
    // visible while you make it.
    //
    // Rows that would need the world rebuilt -- the seed, the ring radius, the
    // densities -- are deliberately absent. A menu that silently does nothing
    // is worse than one that does not offer the control.
    // -----------------------------------------------------------------------
    void onGuiRender(Gui *pGui) override {
        if (opt_.outGiven) return;
        {
            Gui::Window hud(pGui, "v4", {330, 92}, {10, 10});
            hud.text(fmt(" %.0f fps    %dx%d -> %dx%d    %s    %s", fps_, tracer_.width(),
                         tracer_.height(), tracer_.outWidth(), tracer_.outHeight(),
                         tracer_.denoising()
                             ? fmt("DLSS RR %s", dlssQualityName(opt_.dlssQuality)).c_str()
                             : fmt("%u spp accumulated", tracer_.samples()).c_str(),
                         clockText_));
            hud.text(fmt(" %zu chunks   %.1f M tris   %zu instances", world_.chunkCount(),
                         world_.residentTris() / 1e6, world_.instanceCount()));
            hud.text(" Y for settings, F1 for the controls in the console");
        }

        if (!menuOpen_) return;
        Gui::Window w(pGui, "settings", menuOpen_, {380, 430}, {10, 112});

        w.text(fmt("%.0f fps   %d bounces   film %s", fps_, liveDepth_,
                   liveMaxAccum_ ? fmt("%u samples", liveMaxAccum_).c_str() : "unlimited"));
        w.separator();

        for (size_t i = 0; i < presets().size(); ++i) {
            const Preset &p = presets()[i];
            if (w.button(p.name, i > 0)) {
                opt_.scale = p.scale;
                opt_.r.maxDepth = p.depth;
                invalidate();
            }
            (void)p.note;
        }
        w.separator();

        // ---- the denoiser ---------------------------------------------------
        if (dlss_.available()) {
            if (w.checkbox("DLSS Ray Reconstruction", opt_.dlss)) {
                tracer_.resetHistory();
                invalidate();
            }
            if (opt_.dlss) {
                static const Falcor::Gui::DropdownList kModes = {
                    {uint32_t(DlssQuality::UltraPerformance), "Ultra performance"},
                    {uint32_t(DlssQuality::Performance), "Performance"},
                    {uint32_t(DlssQuality::Balanced), "Balanced"},
                    {uint32_t(DlssQuality::Quality), "Quality"},
                    {uint32_t(DlssQuality::Dlaa), "DLAA (no upscale)"},
                };
                uint32_t m = uint32_t(opt_.dlssQuality);
                if (w.dropdown("Mode", kModes, m)) {
                    opt_.dlssQuality = DlssQuality(m);
                    tracer_.setQuality(opt_.dlssQuality);
                }
            }
        } else {
            w.text(fmt("DLSS unavailable: %s", dlss_.status().c_str()));
        }
        w.separator();

        // Only the accumulating path is sized by --scale; with reconstruction
        // running the input size belongs to the quality mode above, and a
        // second control over the same number would be a control that
        // sometimes does nothing.
        if (!tracer_.denoising() && w.slider("Render scale", opt_.scale, 0.15f, 2.0f)) invalidate();
        if (w.slider("Bounces", opt_.r.maxDepth, 1, 32)) invalidate();
        if (w.slider("Bounces (moving)", opt_.movingDepth, 1, 32)) invalidate();
        // Exposure changes no sample already drawn, so it deliberately does NOT
        // throw the accumulation away.
        w.slider("Exposure", opt_.r.exposure, 0.05f, 40.0f);
        w.slider("Walk speed", player_.walk, 0.2f, 200.0f);
        if (w.slider("Field of view", fov_, 10.0f, 100.0f)) invalidate();
        if (w.slider("Samples / frame", opt_.samplesPerFrame, 1, 64)) invalidate();
        if (w.slider("Sun samples", opt_.r.shadowRays, 1, 16)) invalidate();
        if (w.checkbox("Constant grain", opt_.constantGrain)) invalidate();
        w.separator();

        float hours = clock_.tday * 24.0f;
        if (w.slider("Time of day", hours, 0.0f, 24.0f)) {
            clock_.tday = clampf(hours / 24.0f, 0.0f, 0.99999f);
            invalidate();
        }
        char speed[24];
        clock_.speedLabel(speed, sizeof(speed));
        w.text(fmt("Cycle speed  %s   (X + wheel, or:)", speed));
        if (w.button("slower")) clock_.nudgeSpeed(false);
        if (w.button("faster", true)) clock_.nudgeSpeed(true);
        if (w.button(clock_.paused ? "resume" : "pause", true)) clock_.paused = !clock_.paused;
        w.separator();

        // A bake writes SOURCE, not a config file, deliberately. A config read
        // at startup would be one more thing that can be stale, missing, or
        // disagree with the flags; a header means the defaults are visible in
        // the diff, travel with the branch, and cost nothing at runtime.
        if (w.button("Bake as default")) bakeStatus_ = bakeDefaults();
        w.text(bakeStatus_.empty() ? "writes src/core/defaults.h; then rebuild.bat, in v4/"
                                   : bakeStatus_.c_str());
    }

    // -----------------------------------------------------------------------
    bool onKeyEvent(const KeyboardEvent &e) override {
        if (e.type != KeyboardEvent::Type::KeyPressed) return false;

        if (e.key == Input::Key::X) return true;  // held modifier for the wheel
        if (e.key == Input::Key::F) {
            player_.fly = !player_.fly;
            if (!player_.fly) player_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v4: %s\n", player_.fly ? "flying" : "walking");
            std::fflush(stdout);
            quitArmed_ = false;
            return true;
        }
        if (e.key == Input::Key::Y) {
            setMenuOpen(!menuOpen_);
            quitArmed_ = false;
            return true;
        }
        if (e.key == Input::Key::Escape) {
            // ESC closes the menu before it starts arming the quit -- otherwise
            // dismissing a panel would leave the window one press from closing.
            if (menuOpen_) {
                setMenuOpen(false);
                return true;
            }
            // First press hands the mouse back, second press quits. Whether the
            // cursor was captured or not, it always takes two.
            if (looking_) setCapture(false);
            if (quitArmed_) {
                shutdown(0);
            } else {
                quitArmed_ = true;
                std::printf("v4: press ESC again to quit\n");
                std::fflush(stdout);
            }
            return true;
        }

        // Any other key means the session is still in use, so disarm. Otherwise
        // an ESC pressed minutes ago is still primed to close the window.
        quitArmed_ = false;

        if (e.key == Input::Key::Minus) opt_.r.exposure = maxf(0.05f, opt_.r.exposure * 0.8f);
        if (e.key == Input::Key::Equal) opt_.r.exposure = minf(40.0f, opt_.r.exposure * 1.25f);
        if (e.key == Input::Key::LeftBracket) {
            opt_.r.maxDepth = maxi(1, opt_.r.maxDepth - 1);
            std::printf("v4: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        if (e.key == Input::Key::RightBracket) {
            opt_.r.maxDepth = mini(32, opt_.r.maxDepth + 1);
            std::printf("v4: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        if (e.key == Input::Key::P) shotRequested_ = true;
        if (e.key == Input::Key::F1) printHelp();
        std::fflush(stdout);
        return false;
    }

    // -----------------------------------------------------------------------
    bool onMouseEvent(const MouseEvent &e) override {
        if (e.type == MouseEvent::Type::ButtonDown) quitArmed_ = false;

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
                std::printf("v4: day/night %s\n", lbl);
                std::fflush(stdout);
                return true;
            }
            const float f = clampf(fov_ - e.wheelDelta.y * 2.0f, 10.0f, 100.0f);
            if (f != fov_) {
                fov_ = f;
                tracer_.resetAccumulation();
            }
            return true;
        }

        if (menuOpen_) return false;  // the mouse belongs to the menu while it is up

        if (e.type == MouseEvent::Type::ButtonDown && e.button == Input::MouseButton::Left) {
            // Click to capture, the way a game does it. ESC gives it back.
            if (!looking_) setCapture(true);
            return true;
        }
        if (e.button == Input::MouseButton::Right) {
            // Hold-to-look, kept from the earlier engines so the habit carries.
            if (e.type == MouseEvent::Type::ButtonDown) {
                holdLook_ = true;
                if (!looking_) setCapture(true);
            } else if (e.type == MouseEvent::Type::ButtonUp && holdLook_) {
                holdLook_ = false;
                setCapture(false);
            }
            return true;
        }
        return false;
    }

    void onResize(uint32_t, uint32_t) override { tracer_.resetAccumulation(); }

  private:
    Options opt_;
    World world_;
    Tracer tracer_;
    Dlss dlss_;
    Player player_;
    DayNight clock_;  // owns the sun; sunAz_/sunEl_ are its output
    std::vector<Solid> solids_;  // decor near the player, regathered each tick

    Vec3 pos_{0, 2, 0};  // the EYE, derived from the player every frame
    float yaw_ = 0.0f, pitch_ = 0.0f, fov_ = 50.0f;
    float sunAz_ = 38.0f, sunEl_ = 24.0f;

    bool looking_ = false;   // cursor captured, mouse turns the camera
    bool holdLook_ = false;  // ...because the right button is held
    bool quitArmed_ = false;
    bool moving_ = false;
    bool menuOpen_ = false;
    bool captureBeforeMenu_ = false;
    bool shotRequested_ = false;
    int shotIndex_ = 0;
    int shotFrames_ = 0;

    float fps_ = 0.0f;
    double fpsAccum_ = 0.0;
    int fpsFrames_ = 0;
    uint32_t liveMaxAccum_ = 0;
    int liveDepth_ = 0;
    char clockText_[16] = {0};
    std::string bakeStatus_;
    std::chrono::steady_clock::time_point lastTime_ = std::chrono::steady_clock::now();

    static double secondsSince(std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    template <typename... A>
    static std::string fmt(const char *f, A... a) {
        char b[256];
        std::snprintf(b, sizeof(b), f, a...);
        return std::string(b);
    }

    void invalidate() { tracer_.resetAccumulation(); }

    Vec3 forward() const { return Camera::direction(yaw_, pitch_); }

    // The world as the player sees it: the terrain, plus the trees and rocks
    // close enough to walk into.
    //
    // Six metres of reach for a body a quarter of a metre wide, because the
    // gather happens ONCE a tick and the player then moves within it. Anything
    // it misses is something the next tick will pick up long before it is
    // reached at 17 m/s.
    WalkWorld walkWorld() {
        world_.collidersNear(player_.pos, 6.0f, &solids_);
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.solids = solids_.data();
        w.solidCount = int(solids_.size());
        return w;
    }

    // -----------------------------------------------------------------------
    // Push the clock's sun into the sky, and say whether it moved enough to
    // matter.
    //
    // THE THRESHOLD IS THE POINT. At 1x the sun sweeps 0.3 degrees a second, so
    // every single frame moves it a little and a naive "did it change?" would
    // throw the accumulation away sixty times a second and never let a still
    // frame converge at all.
    // -----------------------------------------------------------------------
    bool applySun(bool force) {
        const float el = clock_.elevationDeg();
        const float az = clock_.azimuthDeg();
        const float moved = fabsf(el - sunEl_) + fabsf(az - sunAz_);
        if (!force && moved < 0.02f) return false;
        sunEl_ = el;
        sunAz_ = az;
        world_.sky.setSun(az, el);
        return true;
    }

    // -----------------------------------------------------------------------
    // Opening the menu hands the mouse back, and closing it takes it again if
    // it had it. A menu you can see but not point at is worse than no menu.
    // -----------------------------------------------------------------------
    void setMenuOpen(bool on) {
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
        // A background instance never takes the cursor, whatever it is asked:
        // it exists to be measured, not driven, and the person at the keyboard
        // is using another window.
        if (opt_.background) on = false;
        if (on == looking_) return;
        looking_ = on;

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

    void centreCursor() {
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
    bool applyMouseLook() {
        if (!looking_) return false;
        HWND hwnd = (HWND)getWindow()->getApiHandle();
        RECT rc{};
        POINT p{};
        if (!::GetClientRect(hwnd, &rc) || !::GetCursorPos(&p)) return false;
        ::ScreenToClient(hwnd, &p);

        const int cx = (rc.right - rc.left) / 2, cy = (rc.bottom - rc.top) / 2;
        const float dx = float(p.x - cx) * 0.12f;
        const float dy = float(cy - p.y) * 0.12f;
        centreCursor();
        if (dx == 0.0f && dy == 0.0f) return false;

        // Wrapped rather than left to grow: a long session spinning one way
        // otherwise walks yaw into the thousands, where a float's steps get
        // coarse enough to make the turn visibly notchy.
        yaw_ = fmodf(yaw_ + dx, 360.0f);
        if (yaw_ < 0.0f) yaw_ += 360.0f;
        pitch_ = clampf(pitch_ + dy, -89.0f, 89.0f);
        moving_ = true;
        return true;
    }

    // -----------------------------------------------------------------------
    bool processInput(float dt) {
        const Falcor::InputState &in = getInputState();
        bool turned = applyMouseLook();

        // The arrows scrub the CLOCK, not the sun directly: with a cycle
        // running, a manual elevation would be overwritten on the next frame
        // and the control would look broken.
        if (!menuOpen_) {
            const float scrub = 1.5f * dt;  // hours per second held
            if (in.isKeyDown(Input::Key::Left)) clock_.scrubHours(-scrub);
            if (in.isKeyDown(Input::Key::Right)) clock_.scrubHours(scrub);
            if (in.isKeyDown(Input::Key::Up)) clock_.scrubHours(scrub * 6.0f);
            if (in.isKeyDown(Input::Key::Down)) clock_.scrubHours(-scrub * 6.0f);
        }

        const bool sprint =
            in.isKeyDown(Input::Key::LeftShift) || in.isKeyDown(Input::Key::RightShift);
        const bool jump = in.isKeyDown(Input::Key::Space);
        const bool down = in.isKeyDown(Input::Key::LeftControl) || in.isKeyDown(Input::Key::Q);

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

        const Vec3 before = player_.eyePosition();
        player_.update(walkWorld(), move, sprint, jump, down, dt);
        pos_ = player_.eyePosition();

        // The BOB counts as movement. It shifts the eye every frame while
        // walking, so the accumulated samples describe a viewpoint that no
        // longer exists -- exactly as if the camera had been flown.
        const bool camMoved = lengthSq(pos_ - before) > 1e-10f;
        moving_ = moving_ || camMoved;
        return camMoved || turned;
    }

    // -----------------------------------------------------------------------
    // Render one frame offline and write it.
    // -----------------------------------------------------------------------
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
            std::fprintf(stderr, "v4: could not write %s\n", opt_.out.c_str());
            return;
        }
        std::printf("  wrote    %s\n", opt_.out.c_str());

        if (opt_.writeHdr) {
            const std::string p = opt_.out.substr(0, opt_.out.find_last_of('.')) + ".pfm";
            if (tracer_.writePfm(ctx, p)) std::printf("  wrote    %s\n", p.c_str());
        }
    }

    void printHelp() const {
        std::printf(
            "\ncontrols:\n"
            "  click the window first  -- it needs focus, the console steals it on launch\n"
            "\n"
            "  left click            capture the mouse and look freely\n"
            "  right-drag            look around without capturing\n"
            "  W A S D               walk (hold shift to sprint)\n"
            "  space                 jump\n"
            "  F                     toggle fly mode\n"
            "  scroll                zoom (field of view)\n"
            "  arrow keys            scrub time (up/down = fast)\n"
            "  X + scroll wheel      day/night speed -- scroll down past 0.25x to REWIND\n"
            "  Y                     SETTINGS MENU\n"
            "  - / =                 exposure down / up\n"
            "  [ / ]                 bounces down / up\n"
            "  P                     screenshot            F1   this help\n"
            "  ESC                   release the mouse; ESC again quits\n\n");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // Write the live settings back out as src/core/defaults.h.
    //
    // The path comes from V4_SOURCE_DIR, baked in by the build, rather than
    // being derived from the working directory -- the launcher runs the exe
    // from C:\voxelbit, so anything relative would land in the wrong tree and
    // report success while writing nothing anyone would ever compile.
    // -----------------------------------------------------------------------
    std::string bakeDefaults() {
#ifndef V4_SOURCE_DIR
        return "bake unavailable: built without V4_SOURCE_DIR";
#else
        // V4_SOURCE_DIR is "<engine>/src", so the engine root -- and the
        // rebuild script the user is about to be told to run -- is one level up.
        // Derived rather than hardcoded so a copy of this tree elsewhere still
        // reports its own path.
        const std::string srcDir = V4_SOURCE_DIR;
        const std::string root =
            srcDir.size() > 4 ? srcDir.substr(0, srcDir.size() - 4) : srcDir;
        const std::string path = srcDir + "/core/defaults.h";
        char clockText[16];
        clock_.clock(clockText, sizeof(clockText));
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return "could not write " + path;

        std::fprintf(f,
            "// ---------------------------------------------------------------------------\n"
            "// defaults.h -- the settings v4 starts with.\n"
            "//\n"
            "// GENERATED FILE. Everything below is rewritten wholesale by \"Bake as\n"
            "// default\" in the in-viewer settings menu (Y), so hand edits survive only\n"
            "// until the next bake -- but hand edits are perfectly fine, the format is just\n"
            "// constants and the file is checked in.\n"
            "//\n"
            "// The point of it is that the settings menu and the command line stop being\n"
            "// separate universes: fly around, tune the picture until it looks right, bake,\n"
            "// rebuild, and the thing you tuned is what v4 opens with.\n"
            "// ---------------------------------------------------------------------------\n"
            "#pragma once\n"
            "\n"
            "namespace v4 {\n"
            "namespace defaults {\n"
            "\n"
            "constexpr float kScale = %.2ff;\n"
            "constexpr int kDepth = %d;\n"
            "constexpr int kMovingDepth = %d;\n"
            "constexpr float kExposure = %.2ff;\n"
            "constexpr float kSpeed = %.1ff;\n"
            "constexpr float kEye = %.2ff;\n"
            "constexpr float kFov = %.1ff;\n"
            "constexpr float kSunAz = %.1ff;\n"
            "constexpr float kSunEl = %.1ff;\n"
            "constexpr int kWidth = %d;\n"
            "constexpr int kHeight = %d;\n"
            "constexpr int kTrees = %d;\n"
            "constexpr float kTimeOfDay = %.4ff;  // %s\n"
            "constexpr float kCycleSpeed = %.2ff;\n"
            "\n"
            "}  // namespace defaults\n"
            "}  // namespace v4\n",
            opt_.scale, opt_.r.maxDepth, opt_.movingDepth, opt_.r.exposure, player_.walk,
            player_.eye, fov_, sunAz_, sunEl_, int(getTargetFbo()->getWidth()),
            int(getTargetFbo()->getHeight()), defaults::kTrees, clock_.tday, clockText,
            clock_.cycleSpeed);
        std::fclose(f);
        // The FULL PATH, not just the file name. "run rebuild.bat" is only
        // useful if you already know which of the engine trees it lives in,
        // and the exe is launched from the repo root rather than from beside
        // its own source -- so the obvious place to look is the wrong one.
        return "baked. now run " + root + "/rebuild.bat";
#endif
    }
};

}  // namespace v4
