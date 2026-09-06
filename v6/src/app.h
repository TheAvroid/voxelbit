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
#include "Core/API/BlendState.h"
#include "Core/API/RenderContext.h"
#include "Core/Pass/FullScreenPass.h"
#include "Utils/UI/Gui.h"
#include "Utils/Timing/Profiler.h"
#include <imgui.h>
#include "Utils/UI/InputState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
#include "gpu/streamline.h"
#include "gpu/volfog.h"
#include "gpu/tracer.h"
#include "gpu/world.h"
#include "render/camera.h"
#include "render/player.h"
#include "scene/daynight.h"

namespace v6 {

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

    // PHASE B -- separate the lighting from the texture before denoising.
    //
    // Off by default, and that is not laziness. The default route is DLSS Ray
    // Reconstruction, which is handed the COMPOSITED colour plus its albedo and
    // specular guides and demodulates INTERNALLY -- it is trained on exactly
    // that input. Feeding it radiance the engine has already divided would
    // demodulate the frame twice and is the "never double-process a buffer"
    // rule applied one layer up. So this comes on for the routes that genuinely
    // need pre-demodulated input: NRD, and the Super Resolution path.
    bool demodulate = false;

    // PHASE A, the air. The froxel grid is the ONLY fog -- there is no analytic
    // fallback behind it, so volFog = false means no fog at all.
    bool volFog = true;
    float fogAnisotropy = 0.7f;
    float fogAmbient = 0.60f;
    float fogFar = 400.0f;

    // PHASE A, indirect. The irradiance probes are ON by default -- they are
    // the largest single change v6 makes to what the frame looks like, and the
    // fallback if the hardware refuses them is exactly v4's renderer.
    bool ddgi = true;
    // Print which phases actually came up on this machine, then exit. The one
    // command worth running first on a machine nobody has tried before.
    bool pipelineReport = false;
    // How much of the probe update to run. 0 none, 1 upload, 2 + trace,
    // 3 + blend, 4 + relocate, 5 everything. For bisecting a GPU fault.
    int ddgiStage = 5;

    // PHASE D, after the upscale: DLSS Frame Generation. Off by default --
    // it changes how the frame is PACED rather than how it looks, and it is
    // the one feature here that needs a patched slang-gfx to exist at all.
    // ON BY DEFAULT AT 2x. Every path that cannot actually deliver it turns
    // it back off below with a line saying why, so defaulting it on cannot
    // leave the engine claiming something it is not doing -- which was the
    // failure mode that made this feature so expensive to get working in
    // the first place. 3x/4x stay opt-in: they need a 50-series card.
    FrameGen frameGen = FrameGen::On2x;

    // PHASE C, auxiliary: ambient occlusion, and a way to look at it.
    bool ao = false;
    int aoView = 0;   // 0 picture, 1 raw AO, 2 denoised AO
    // How many probe passes an offline render runs before the film starts.
    // Adjustable because the debug layer makes 300 of them take minutes.
    int ddgiWarm = 300;
    bool ddgiBounce = true;
    float ddgiHysteresis = 0.97f;

    // Prove the split is lossless, then exit.
    //
    // Traces exactly one sample, splits it, divides the textures out, multiplies
    // them straight back with NOTHING denoising in between, and compares the
    // result against the composited image the same trace produced. The two
    // should agree to floating-point rounding on every pixel. If they do not,
    // the demodulation is losing or inventing light, and every denoiser
    // downstream of it is being fed a lie.
    bool checkDemod = false;

    // WHERE THE TIME ACTUALLY GOES, printed once at exit.
    //
    // A mean frame rate is the wrong instrument for the thing this engine is
    // worst at. Streaming a chunk in costs the MAIN thread a structure build
    // and a top-level rebuild, and that lands in one frame rather than being
    // spread over the second it took to earn -- so a run can average 90 fps and
    // still stutter every time you cross a boundary. An average hides that by
    // construction; a p99 and a worst case do not.
    //
    // It also splits the main thread's streaming cost into its three parts,
    // because "chunk loading is slow" is not actionable and "the structure
    // build waits 14 ms on the device" is.
    bool profile = false;

    // Open minimised, never take the foreground, never take the mouse.
    //
    // For AUTOMATED launches -- benchmarks, screenshots, anything a script
    // starts while a person is using the machine. Without it every test run
    // yanks the cursor out of whatever window had it, which is intolerable if
    // you are playing your own copy at the time.
    bool background = false;

    // THE GAME'S RESOLUTION, as a fraction of the window.
    //
    // Not a window size, and deliberately not one: the window is whatever the
    // person dragged it to, and this is how many pixels get MADE inside it
    // before the swapchain stretches the result back up to fill it. Dropping it
    // is the cheapest frame rate in the engine and the only one that gives up
    // nothing but sharpness -- no geometry leaves the ring, no bounce is
    // surrendered, no sample is skipped.
    //
    // With Ray Reconstruction on this is what DLSS is asked to PRODUCE, and the
    // quality mode picks what gets traced into it, so the two stack rather than
    // one of them silently overruling the other.
    float scale = defaults::kScale;

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

    // A PHOTOGRAPH OF THE WINDOW, not of the render.
    //
    // --shot writes what the tracer produced, which is the right thing for
    // judging a picture and the wrong thing for judging an interface: the
    // crosshair is inverted into the WINDOW's pixels after the blit, and the
    // menu is drawn by the GUI after the frame is handed back. Neither is in
    // the render at all.
    //
    // This captures the target framebuffer instead, at the top of a frame --
    // which is where it still holds the previous frame, fully composited. It is
    // how the interface gets checked without opening a window at somebody.
    std::string shotUi;
    // Open the settings panel at startup, so a scripted capture can see it.
    bool menuAtStart = false;
    bool groundStats = false;

    int shotFrame = 240;
    // Hold W while the frames run. The point is to judge the picture WHILE
    // MOVING -- a still comparison flatters the accumulator, which converges
    // beautifully when nothing moves and is exactly not the case in question.
    bool shotWalk = false;
    // Start in fly mode.
    //
    // Added for the profiler, and it is not a convenience. A scripted walk is
    // subject to collision, so pointing it at a trunk parks the camera against
    // the bark and reports 40 m/s while the position never changes -- which is
    // exactly what the first --profile run did, and it measured a stationary
    // camera in a fully resident ring. Flying passes through, so the ring
    // genuinely moves and the streamer is genuinely exercised.
    bool startFly = false;
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

    // HOW FAR THE VIEW TURNS FOR A PIXEL OF MOUSE, in degrees.
    //
    // Degrees per pixel rather than a multiplier of something, because that is
    // what the number physically is and it is the only form in which two
    // machines can be compared. A multiplier is a multiplier of whatever this
    // engine happened to be hardcoded to, which was 0.12 and was chosen by
    // nobody in particular.
    float sensitivity = defaults::kSensitivity;
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
    float treeDensity = 0.2325f;
    float grass = 0.105f, flowers = 0.45f, rocks = 0.010f;
    int grassMin = 3, grassMax = 6;
    std::string pines = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decor = "C:/voxelbit/game/assets/decoration";

    float sunAz = defaults::kSunAz;
    float sunEl = defaults::kSunEl;
    float turbidity = 2.8f;

    float camX = -6.0f, camZ = 34.0f;
    // Whether the two above were ASKED for. A named camera is a named camera:
    // it pins the offline render, it pins a scripted capture, and it turns the
    // random spawn off. Without this the flags would be silently overwritten by
    // a spawn the user did not ask for.
    bool camGiven = false;
    // 0 means "somewhere new", which is the default for the viewer. Any other
    // value returns to the same place -- the seed is printed on every launch so
    // a spot worth finding again can be.
    uint32_t spawnSeed = 0;
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
        r.shadowLift = defaults::kShadowLift;
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
// THE LOOK IS v2's, and the reasoning behind it came with it.
//
// v2 drew its panels by hand -- GDI text into a bitmap, uploaded as a texture,
// hit-tested against its own character grid. Falcor brings Dear ImGui, so none
// of that machinery survives the port. The DESIGN did, because none of it was
// about the machinery:
//
//   AMBER for a title, CYAN for the value you are changing, near-white for a
//   label you can act on, grey for one you can only read. Four roles, four
//   colours, and nothing decorative.
//
//   CONSOLAS, because a column of numbers that does not line up is a column of
//   numbers you have to read one at a time. Falcor registers the same face v2
//   asked GDI for; it only has to be switched on.
//
//   ROUNDED IN PROPORTION TO THE TEXT, clamped at both ends -- below about ten
//   pixels the arc is indistinguishable from a square corner, and much above
//   twenty it stops reading as a rounded rectangle and starts reading as a
//   lozenge.
//
//   THE PANEL DIMS WHAT IS BEHIND IT, so an oversized one is not harmless: it
//   dims a band of the render for no reason. Sized from its content.
// ---------------------------------------------------------------------------
namespace ui {

// v2's, verbatim.
inline ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, a);
}
inline ImVec4 kTitle() { return rgb(255, 214, 120); }  // amber
inline ImVec4 kText() { return rgb(226, 232, 240); }
inline ImVec4 kDim() { return rgb(150, 158, 170); }
inline ImVec4 kHot() { return rgb(126, 220, 255); }  // cyan: the live value
inline ImVec4 kNote() { return rgb(128, 140, 155); }
inline ImVec4 kBar() { return rgb(38, 52, 70); }
// 214 of 255, as v2 composed its panel.
inline ImVec4 kPanel() { return rgb(16, 20, 27, 214.0f / 255.0f); }

}  // namespace ui

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

        // SOMEWHERE NEW EACH TIME, but only in the viewer. --out renders one
        // frame from a named camera and the whole point of it is that the same
        // flags give the same picture, so it keeps the camera it was given.
        if (!opt_.outGiven && !opt_.camGiven) chooseSpawn();
        if (opt_.groundStats) { groundStats(); skyStats(); shutdown(0); return; }

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
        std::printf("           %zu pines, %zu rocks, %zu flowers standing in them\n",
                    world_.decorCount(0), world_.decorCount(1), world_.decorCount(2));
        std::printf("           %.0f ms of that was structure building, %.0f MB of tri pool\n",
                    world_.buildMs(), double(world_.poolBytes()) / (1024.0 * 1024.0));

        tracer_.init(getDevice(), &world_);
        makeCrosshair();

        // -- PHASE C, auxiliary: NRD ----------------------------------------
        //
        // Only brought up when AO is actually wanted. It is a denoiser for one
        // signal; creating it when nothing produces that signal would allocate
        // a dozen textures to filter a buffer nobody writes.
        if (opt_.ao || opt_.aoView != 0) {
            if (nrd_.init(getDevice())) {
                std::printf("  nrd      %s -- REBLUR diffuse occlusion\n",
                            nrd_.status().c_str());
            } else {
                std::printf("  nrd      unavailable: %s\n"
                            "           ambient occlusion will not be denoised\n",
                            nrd_.status().c_str());
            }
            tracer_.setAmbientOcclusion(true);
            tracer_.setDebugView(opt_.aoView);
            std::fflush(stdout);
        }

        // -- Streamline: ask what this machine can actually do ---------------
        //
        // Never fatal. If the interposer is not underneath slang-gfx -- because
        // gfx.dll was not patched, or was rebuilt over by a Falcor update --
        // this reports it and everything downstream uses the direct-NGX path.
        if (sl_.init(getDevice())) {
            std::printf("  sl       streamline ready\n%s", sl_.featureReport().c_str());
            // Printed whenever frame generation is asked for, because this is
            // the only line that distinguishes "the swapchain is a proxy" from
            // "everything reports fine and nothing is generated".
            if (opt_.frameGen != FrameGen::Off)
                std::printf("%s", Streamline::moduleReport().c_str());
            if (opt_.frameGen != FrameGen::Off) {
                if (sl_.hasFrameGeneration()) {
                    // Sizes are not known until the first resize, so this is
                    // the requested output and a matching render size; the
                    // frame loop re-applies it once the tracer is sized.
                    const Falcor::uint2 out{uint32_t(opt_.r.width), uint32_t(opt_.r.height)};
                    if (sl_.setFrameGeneration(opt_.frameGen, out, out))
                        std::printf("  sl       frame generation %s\n",
                                    frameGenName(opt_.frameGen));
                    else
                        std::printf("  sl       frame generation refused: %s\n",
                                    sl_.status().c_str());
                } else if (sl_.gpuSupportsFrameGeneration()) {
                    std::printf("  sl       frame generation asked for, but the swapchain is\n"
                                "           not a Streamline proxy -- nothing would be\n"
                                "           generated, so it is left off\n");
                    opt_.frameGen = FrameGen::Off;
                } else {
                    std::printf("  sl       frame generation unavailable on this GPU\n");
                    opt_.frameGen = FrameGen::Off;
                }
            }
        } else {
            std::printf("  sl       streamline unavailable: %s\n", sl_.status().c_str());
            // Without Streamline there is no frame generation, so do not
            // leave the option reading 2x -- the HUD would then report a
            // multiplier that nothing is applying.
            opt_.frameGen = FrameGen::Off;
        }
        std::fflush(stdout);

        // -- PHASE A: the irradiance probes ---------------------------------
        //
        // Allowed to fail for any of half a dozen reasons -- no SDK in the
        // build, Shader Model below 6.6, no wave intrinsics, a missing .cso,
        // a volume the SDK refuses -- and every one of them lands in the same
        // place: the probes are off, the message says why, and the path tracer
        // goes back to bouncing for its indirect light exactly as v4 does.

        // The fog grid, and it is now the ONLY fog: the analytic haze is gone
        // rather than left behind as a fallback, so that two implementations
        // cannot disagree in edge cases. Its two passes run over 160x90x64
        // cells rather than the screen, so the cost does not grow with the
        // output resolution -- which is most of the argument for froxels.
        if (volfog_.init(getDevice())) {
            tracer_.setVolFog(&volfog_);
            // The command line moves these before anything has drawn; the
            // menu moves them afterwards. Both write the same fields.
            volfog_.enabled = opt_.volFog;
            volfog_.anisotropy = opt_.fogAnisotropy;
            volfog_.ambient = opt_.fogAmbient;
            volfog_.farD = opt_.fogFar;
            std::printf("  fog      volumetric, %s\n", volfog_.status().c_str());
        } else {
            std::printf("  fog      unavailable: %s\n", volfog_.status().c_str());
        }

        if (opt_.ddgi) {
            ddgi_.setHysteresis(opt_.ddgiHysteresis);
            if (ddgi_.init(getDevice(), Falcor::getRuntimeDirectory() / "shaders" / "v6")) {
                std::printf("  ddgi     %d probes x %d rays, %.0f m spacing -- %.0fk rays/frame\n",
                            ddgi_.numProbes(), ddgi_.raysPerProbe(), double(kProbeSpacing),
                            double(ddgi_.numProbes()) * ddgi_.raysPerProbe() / 1000.0);
                tracer_.bindProbes(ddgi_);
            } else {
                std::printf("  ddgi     unavailable: %s\n"
                            "           indirect light falls back to path tracing\n",
                            ddgi_.status().c_str());
                opt_.ddgi = false;
            }
            std::fflush(stdout);
        }

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
        if (opt_.pipelineReport) {
            reportPipeline();
            shutdown(0);
            return;
        }
        if (opt_.checkDemod) {
            checkDemodulation(ctx);
            shutdown(0);
            return;
        }
        if (opt_.outGiven) {
            tracer_.setDemodulate(opt_.demodulate);
            tracer_.setAmbientOcclusion(opt_.ao || opt_.aoView != 0);
            tracer_.setDebugView(opt_.aoView);
            renderOffline(ctx);
            shutdown(0);
            return;
        }

        nudgeOutOfSolids();

        player_.walk = opt_.speed;
        player_.fly = opt_.startFly;
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

        // The profiler is off by default and costs a pair of timestamps per
        // scope when it is on, so it is turned on only for a measured run.
        if (opt_.profile && getDevice()->getProfiler()) getDevice()->getProfiler()->setEnabled(true);

        if (opt_.menuAtStart) setMenuOpen(true);

        printHelp();
        lastTime_ = std::chrono::steady_clock::now();
    }

    // -----------------------------------------------------------------------
    void onFrameRender(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) override {
        if (opt_.outGiven) return;

        // BEFORE ANYTHING IS DRAWN INTO IT. The target still holds the previous
        // frame at this point -- blitted, crosshaired and with the GUI composed
        // over it -- and that whole frame is what an interface has to be judged
        // on. Falcor does not clear it between frames, which is what makes this
        // possible at all.
        if (!opt_.shotUi.empty() && shotFrames_ >= opt_.shotFrame && target->getWidth() > 0) {
            target->getColorTexture(0)->captureToFile(0, 0, opt_.shotUi);
            std::printf("v6: wrote %s at %ux%u (window, with the interface)\n",
                        opt_.shotUi.c_str(), target->getWidth(), target->getHeight());
            std::fflush(stdout);
            shutdown(0);
            return;
        }

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
        //
        // TIMED SEPARATELY FROM THE FRAME, because this is the one part of a
        // frame whose cost has nothing to do with how hard the picture is: it
        // is paid in whichever frame a chunk happens to arrive in. Keeping the
        // two apart is what lets the report below say whether a slow frame was
        // the renderer or the streamer.
        const auto tu0 = std::chrono::steady_clock::now();
        if (world_.update(pos_)) tracer_.resetAccumulation();
        const float updateMs =
            float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tu0)
                      .count());
        if (opt_.profile) {
            frameMs_.push_back(wallDt * 1000.0f);
            streamMs_.push_back(updateMs);
        }
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

        Camera cam;
        cam.origin = pos_;
        cam.target = pos_ + forward() * 50.0f;
        cam.fovDeg = fov_;
        cam.aperture = 0.0f;  // a pinhole here; depth of field is for stills
        cam.focusDist = 40.0f;
        const V6Camera gcam = cam.gpu(tracer_.width(), tracer_.height());

        // -- Streamline: this frame begins ----------------------------------
        //
        // The token identifies the frame to every later call, so it has to come
        // first. The simulation markers bracket the CPU work Reflex paces
        // against; with frame generation on, the pacer is what decides when the
        // generated frame is shown, so markers that do not bracket the real
        // work produce a higher frame rate that feels worse than none.
        // Read DLSS-G's counters here, before this frame starts and just
        // after the previous present finished -- see pollFrameGenState().
        sl_.pollFrameGenState();
        sl_.newFrame();
        sl_.markSimulationStart();

        // -- PHASE A: refresh the probes ------------------------------------
        //
        // BEFORE the camera trace, because the camera path reads the atlas this
        // produces. Reading last frame's would be a frame of lag on all
        // indirect light -- invisible while standing still and a visible smear
        // behind you as you walk.
        //
        // The order inside is fixed and explained in ddgi.h: move the grid,
        // upload the constants (which also picks this frame's random ray
        // rotation), trace the probe rays, then let the SDK blend, relocate and
        // classify.
        cfg.useDdgi = opt_.ddgi && ddgi_.available();
        cfg.ddgiBounce = opt_.ddgiBounce;
        if (cfg.useDdgi) {
            FALCOR_PROFILE(ctx, "ddgi");
            ddgi_.setOrigin(pos_);
            if (opt_.ddgiStage >= 1) ddgi_.uploadConstants(ctx);
            tracer_.setDdgiConsts(ddgi_.consts());
            if (opt_.ddgiStage >= 2) tracer_.traceProbes(ctx, ddgi_, world_.sky.gpu(), cfg);
            if (opt_.ddgiStage >= 3) ddgi_.updateProbes(ctx, opt_.ddgiStage - 1);
        }

        // THE FOG IS LIT BEFORE THE FRAME IS TRACED, because the trace samples
        // it. Both passes are over the 160x90x64 grid rather than the screen, so
        // this is a fixed cost that does not grow with resolution.
        {
            FALCOR_PROFILE(ctx, "fog volume");
            tracer_.renderVolFog(ctx, gcam, opt_.r.fogDensity, opt_.r.fogHeight,
                                 tracer_.samples());
        }

        // Constant grain: start from nothing EVERY frame, not just when the
        // camera moves. Moving already did this -- it is what made a walking
        // frame noisy -- so doing it always is what makes the two identical.
        const int spf = maxi(1, opt_.samplesPerFrame);
        cfg.samplesPerFrame = spf;
        // GPU TIMESTAMPS, and only under --profile. The whole point is to be
        // able to say "the denoiser is two thirds of the frame" as a
        // measurement rather than as the difference between two runs with
        // different settings, which is a comparison of two different frames.
        Falcor::Profiler *prof = opt_.profile ? getDevice()->getProfiler() : nullptr;
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
            tracer_.resolve(ctx, cfg, reconstructed);
        }

        // -- PHASE C, auxiliary: denoise the ambient occlusion --------------
        //
        // AFTER the trace (which wrote the raw signal) and BEFORE the tone map
        // reads it. The result is consumed by the NEXT frame's probe lookup --
        // see the note on gAoPrev in Trace.cs.slang -- so a frame of latency is
        // built into the design rather than an accident of ordering.
        if (opt_.ao || opt_.aoView != 0) {
            FALCOR_PROFILE(ctx, "nrd");
            if (nrd_.resize(uint32_t(tracer_.width()), uint32_t(tracer_.height()))) {
                float viewToClip[16], worldToView[16];
                buildCameraMatrices(gcam, viewToClip, worldToView);
                tracer_.denoiseAo(ctx, nrd_, cfg, viewToClip, worldToView, slReset_);
            }
        }

        // -- Streamline: hand over this frame's camera and buffers ----------
        //
        // AFTER the tone map and BEFORE the crosshair, deliberately. What is
        // tagged is the finished image with no UI on it; tagging the window
        // instead would let the generated frames interpolate the crosshair and
        // the settings menu, which smears them across the screen whenever the
        // camera moves.
        sl_.markSimulationEnd();
        sl_.markRenderSubmitStart();
        if (sl_.frameGeneration() != FrameGen::Off) {
            FALCOR_PROFILE(ctx, "streamline");
            const Falcor::uint2 renderDim{uint32_t(tracer_.width()),
                                          uint32_t(tracer_.height())};
            const Falcor::uint2 outDim{uint32_t(tracer_.displayWidth()),
                                       uint32_t(tracer_.displayHeight())};
            // Tag first: it is what records the motion-vector extent the
            // constants are then scaled against.
            sl_.tagResources(ctx, tracer_.display().get(), tracer_.guideDepth().get(),
                             tracer_.guideMotion().get(), renderDim, outDim);
            // SAME SIGN FLIP AS RAY RECONSTRUCTION. DLSS defines jitter the way
            // a rasteriser applies it; this tracer builds the ray for
            // pixel + jitter, which moves the image the other way. See the note
            // in Tracer::reconstruct() -- getting it wrong here does not look
            // broken, it just never quite resolves.
            const Vec2 j = tracer_.lastJitter();
            sl_.setFrameConstants(&gcam.pos.x, &gcam.u.x, &gcam.v.x, &gcam.w.x,
                                  gcam.halfW, gcam.halfH, -j.x, -j.y, slReset_);

            // The sizes at startup were a guess -- the tracer had not been
            // sized yet, and under DLSS the traced size is chosen by the SDK.
            // Re-declare them the first time the real ones are known, and again
            // whenever they change.
            // RE-DECLARED ONLY WHEN THE SIZE ACTUALLY CHANGES, and the guard is
            // updated even when nothing is re-declared. Streamline warns
            // "Repeated slDLSSGSetOptions() call for the frame N -- a redundant
            // call or a race condition with Present()", and it means it: this
            // call races the present thread, so issuing it once a frame is not
            // merely wasteful.
            if (slFgDim_.x != renderDim.x || slFgDim_.y != renderDim.y ||
                slFgOut_.x != outDim.x || slFgOut_.y != outDim.y) {
                slFgDim_ = renderDim;
                slFgOut_ = outDim;
                sl_.setFrameGeneration(opt_.frameGen, renderDim, outDim);
            }
            slReset_ = false;
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
        if (target->getWidth() > 0 && target->getHeight() > 0) {
            ctx->blit(tracer_.display()->getSRV(), target->getRenderTargetView(0),
                      Falcor::uint4(0, 0, uint32_t(tracer_.displayWidth()),
                                    uint32_t(tracer_.displayHeight())));
            drawCrosshair(ctx, target);
        }

        // The present window Reflex paces against. DLSS-G inserts its
        // generated frame inside it, on the proxy swapchain.
        sl_.markRenderSubmitEnd();
        sl_.markPresentStart();
        slPresentPending_ = true;

        if (shotRequested_) {
            shotRequested_ = false;
            char name[64];
            std::snprintf(name, sizeof(name), "v6_shot_%03d.png", shotIndex_++);
            if (tracer_.writePng(ctx, name))
                std::printf("v6: wrote %s at %dx%d\n", name, tracer_.displayWidth(),
                            tracer_.displayHeight());
            else
                std::fprintf(stderr, "v6: could not write %s\n", name);
            std::fflush(stdout);
        }

        moving_ = false;  // cleared only once the frame it applied to is drawn

        // -- the scripted capture, if one was asked for -----------------------
        // --profile shares the frame counter, so a run can be measured with or
        // without a png falling out of it.
        if (!opt_.shotPath.empty() || opt_.profile || !opt_.shotUi.empty()) {
            ++shotFrames_;
            // --shot-ui does its own capture at the TOP of a frame, so it must
            // not be shut down from here before it gets there.
            if (shotFrames_ >= opt_.shotFrame && opt_.shotUi.empty()) {
                if (!opt_.shotPath.empty() && tracer_.writePng(ctx, opt_.shotPath))
                    // The SIZE is printed because a capture comes out at the
                    // size the game was set to produce, not at the window's --
                    // --scale is a resolution, and the resolution of a
                    // reference shot is the one thing that must never have to
                    // be guessed at from the file.
                    std::printf("v6: wrote %s at %dx%d after %d frames (%s)\n",
                                opt_.shotPath.c_str(), tracer_.displayWidth(),
                                tracer_.displayHeight(), shotFrames_,
                                tracer_.denoising() ? "reconstructed" : "accumulated");
                else if (!opt_.shotPath.empty())
                    std::fprintf(stderr, "v6: could not write %s\n", opt_.shotPath.c_str());
                if (opt_.profile) printProfile();
                // WHETHER FRAME GENERATION ACTUALLY GENERATED ANYTHING, asked
                // of the SDK rather than assumed from the frame rate. DLSS-G
                // falls back to presenting only real frames whenever it cannot
                // produce a good one -- a resolution it dislikes, a missing
                // tag, Reflex not running -- and it does so silently. This is
                // the only honest way to tell "on" from "on and working".
                if (opt_.frameGen != FrameGen::Off) {
                    std::printf("v6: frame generation %s -- %d frames presented per rendered frame\n",
                                frameGenName(opt_.frameGen), sl_.framesPresented());
                    std::printf("v6: frame generation status: %s\n",
                                sl_.frameGenStatus().c_str());
                }
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
        // THE READOUT IS ONE NUMBER.
        //
        // It used to carry the resolutions, the denoiser mode, the clock, the
        // chunk and triangle counts and a line of key hints -- which is a
        // developer's console pinned over a wood you are trying to look at.
        // Everything it said is still available and better placed: the sizes
        // and the mode are rows in the settings menu that owns them, the clock
        // is in the sky, the counts are in --stats, and the keys are on F1.
        // THE READOUT STAYS TOP-LEFT while the menu is centred, and v2 gives
        // the reason: this number is read WHILE MOVING, and a number that
        // follows the crosshair around is a number in the way.
        const float fbW = float(getTargetFbo()->getWidth());
        const float fbH = float(getTargetFbo()->getHeight());

        // NO TITLE BAR, NO MOVE, NO RESIZE GRIP on either panel. v2 drew its
        // own title and put the panel where it belonged; ImGui's chrome on top
        // of that is a second title over the first and a drag handle for a
        // window that is not meant to be dragged.
        const Gui::WindowFlags kBare = Gui::WindowFlags::AutoResize | Gui::WindowFlags::NoResize;

        {
            styleV2 style(pGui, 0.60f, fbH);  // a lighter veil than the menu's
            Gui::Window hud(pGui, "v6hud", {0, 0}, {12, 12}, kBare);
            ImGui::SetWindowFontScale(style.scale);
            // Set every frame rather than on first use: ImGui remembers window
            // positions in an ini file between runs, so "where I asked for it"
            // and "where it appears" are otherwise two different things.
            ImGui::SetWindowPos(ImVec2(12.0f, 12.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(235, 240, 248));
            ImGui::TextUnformatted(fpsLabel().c_str());
            ImGui::PopStyleColor();
        }

        if (!menuOpen_) return;

        styleV2 style(pGui, 1.0f, fbH);
        Gui::Window w(pGui, "settings##v6", menuOpen_, {0, 0}, {0, 0}, kBare);
        ImGui::SetWindowFontScale(style.scale);

        // WIDTH IN CHARACTERS, HEIGHT FROM THE CONTENT -- v2's rule exactly.
        // Seventy columns is the longest line the panel can hold, and fixing
        // the width is not only tidiness: right-aligning anything inside a
        // window that is auto-sizing to its own content is a feedback loop, and
        // the window grows a little wider every frame.
        const float cw = ImGui::CalcTextSize("0").x;
        const float panelW = 70.0f * cw + 32.0f;
        ImGui::SetWindowSize(ImVec2(panelW, 0.0f));

        // CENTRED, which is v2's decision and its reasoning: the middle of the
        // window is where the eyes already are. It used to sit under the fps
        // readout in the top-left corner, which is the one part of the screen
        // you are not looking at while flying.
        const ImVec2 sz = ImGui::GetWindowSize();
        ImGui::SetWindowPos(ImVec2(floorf(maxf(0.0f, (fbW - sz.x) * 0.5f)),
                                   floorf(maxf(0.0f, (fbH - sz.y) * 0.5f))));

        // The title line: the name on the left, and on the right the number
        // that everything below is a trade against. One line, both ends.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTitle());
        ImGui::TextUnformatted("v6  settings");
        {
            const std::string f = fpsLabel();
            ImGui::SameLine();
            ImGui::SetCursorPosX(panelW - ImGui::CalcTextSize(f.c_str()).x -
                                 ImGui::GetStyle().WindowPadding.x);
            ImGui::TextUnformatted(f.c_str());
        }
        ImGui::PopStyleColor();

        // What the frame is actually made of, in the colour of things you can
        // only read. v2 put the same block here for the same reason: the rows
        // below are a trade, and a trade needs a price on screen.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted(
            fmt("%d bounces   film %s", liveDepth_,
                liveMaxAccum_ ? fmt("%u samples", liveMaxAccum_).c_str() : "unlimited")
                .c_str());
        ImGui::TextUnformatted(fmt("%d x %d traced -> %d x %d shown in %d x %d",
                                   tracer_.width(), tracer_.height(), tracer_.outWidth(),
                                   tracer_.outHeight(), int(getTargetFbo()->getWidth()),
                                   int(getTargetFbo()->getHeight()))
                                   .c_str());
        ImGui::TextUnformatted(fmt("%zu chunks   %.1f M tris   %zu instances",
                                   world_.chunkCount(), world_.residentTris() / 1e6,
                                   world_.instanceCount())
                                   .c_str());
        ImGui::PopStyleColor();
        w.separator();

        // THE PRESETS ARE MEASURED, and each one says what it cost. v6 had
        // been carrying the notes in the table and throwing them away at the
        // point of use, which left five buttons whose only difference was a
        // name -- exactly the thing the command line was bad at, reproduced in
        // a menu.
        for (size_t i = 0; i < presets().size(); ++i) {
            const Preset &p = presets()[i];
            const bool active = fabsf(opt_.scale - p.scale) < 0.005f;
            ImGui::PushStyleColor(ImGuiCol_Text, active ? ui::kHot() : ui::kText());
            if (w.button(p.name)) {
                opt_.scale = p.scale;
                opt_.r.maxDepth = p.depth;
                invalidate();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine(140.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            ImGui::TextUnformatted(fmt("%d%%  %d bounces%s%s", int(p.scale * 100.0f + 0.5f),
                                       p.depth, *p.note ? "   " : "", p.note)
                                       .c_str());
            ImGui::PopStyleColor();
        }
        w.separator();

        // ---- the game's resolution ------------------------------------------
        //
        // NOT the window's, and the line underneath says so in pixels so there
        // is no need to take the label's word for it. A percentage rather than a
        // list of sizes because the window is resizable and any fixed list would
        // be the wrong SHAPE for most of the shapes a window can take -- a
        // fraction is always the right aspect ratio, and the resolution it comes
        // to is printed anyway.
        //
        // This row used to be hidden whenever Ray Reconstruction was on, on the
        // grounds that the quality mode already owned the traced size and a
        // second control over one number is a control that sometimes does
        // nothing. That was true, and it left the setting missing in exactly the
        // configuration it ships in. It now sizes what the renderer PRODUCES
        // while the quality mode sizes what is traced into that -- two different
        // numbers, both live, neither of them ever inert.
        int pct = int(opt_.scale * 100.0f + 0.5f);
        if (w.slider("Resolution", pct, 10, 200, false, "%d%%")) {
            // TEN PERCENT NOTCHES. Partly mechanical: every distinct value
            // reallocates the film and its guides and recreates the DLSS
            // feature -- and that last one submits and WAITS on the device --
            // so a slider free to stop anywhere stutters for as long as it is
            // being dragged. And partly because a five percent step is a choice
            // being offered that nobody can actually judge; ten is a step you
            // can see the frame rate answer.
            pct = ((pct + 5) / 10) * 10;
            opt_.scale = clampf(float(pct) / 100.0f, 0.10f, 2.0f);
            invalidate();
        }
        w.text(fmt("   %d x %d  in a %d x %d window", tracer_.outWidth(), tracer_.outHeight(),
                   int(getTargetFbo()->getWidth()), int(getTargetFbo()->getHeight())));
        if (tracer_.denoising())
            w.text(fmt("   %d x %d traced into it by DLSS %s", tracer_.width(), tracer_.height(),
                       dlssQualityName(opt_.dlssQuality)));
        w.separator();

        // ---- PHASE A: the irradiance probes ---------------------------------
        //
        // Switchable while you watch, which is the only way to see what it
        // actually does: the difference is almost entirely in the SHADOWS, and
        // a still comparison of two screenshots hides it far better than
        // flicking the box does.
        if (ddgi_.available()) {
            if (w.checkbox("DDGI irradiance probes", opt_.ddgi)) {
                invalidate();
                tracer_.resetHistory();
            }
            // The fog grid. Its two passes run over 160x90x64 cells rather than the
        // screen, so what it costs does not grow with the output resolution --
        // which is most of the argument for froxels in the first place.
        if (volfog_.init(getDevice())) {
            tracer_.setVolFog(&volfog_);
            // The command line moves these before anything has drawn; the menu
            // moves them afterwards. Both write the same fields.
            volfog_.enabled = opt_.volFog;
            volfog_.anisotropy = opt_.fogAnisotropy;
            volfog_.ambient = opt_.fogAmbient;
            volfog_.farD = opt_.fogFar;
            std::printf("  fog      volumetric, %s\n", volfog_.status().c_str());
        } else {
            std::printf("  fog      unavailable: %s\n", volfog_.status().c_str());
        }

        if (opt_.ddgi) {
                w.text(fmt("   %d probes x %d rays = %.0fk rays/frame",
                           ddgi_.numProbes(), ddgi_.raysPerProbe(),
                           double(ddgi_.numProbes()) * ddgi_.raysPerProbe() / 1000.0));
                // The bounce depth is the one number worth exposing: 0 would
                // let the probes supply the FIRST bounce and throw away the
                // contact shading that makes a voxel look like a voxel.
                if (w.slider("Cache from bounce", opt_.r.ddgiDepth, 1, 4)) invalidate();
                if (w.slider("Indirect strength", opt_.r.ddgiIntensity, 0.0f, 3.0f))
                    invalidate();
            }
        } else {
            w.text(fmt("DDGI unavailable: %s", ddgi_.status().c_str()));
        }
        w.separator();

        // ---- PHASE A, the air: volumetric fog -------------------------------
        if (volfog_.available()) {
            // Unchecking this leaves NO fog at all, not the old analytic model
            // -- that one is gone. It is here to measure what the two passes
            // cost, and to see the wood without any air in it.
            if (w.checkbox("Volumetric fog (all fog)", volfog_.enabled)) {
                volfog_.invalidate();
                invalidate();
            }
            if (volfog_.enabled) {
                if (w.slider("  density", opt_.r.fogDensity, 0.0f, 0.02f, false, "%.4f /m")) {
                    volfog_.invalidate();
                    invalidate();
                }
                if (w.slider("  height falloff", opt_.r.fogHeight, 2.0f, 200.0f, false, "%.0f m")) {
                    volfog_.invalidate();
                    invalidate();
                }
                // Forward scattering. This is the knob that decides whether the
                // air near the sun GLOWS or whether the whole volume simply
                // lifts: at 0 the phase is a sphere and the fog is milk, and by
                // 0.9 nearly all the scattered light carries on in the direction
                // it was already going, which is what makes a beam a beam.
                if (w.slider("  forward scatter", volfog_.anisotropy, 0.0f, 0.95f, false,
                             "%.2f g")) {
                    volfog_.invalidate();
                    invalidate();
                }
                // The one honestly fudged number in the system -- the sky dome
                // is not traced per froxel, see the note in VolFogInject. Drop
                // it to zero and shadowed air goes black, which is a stronger
                // effect than it sounds and worth seeing once.
                if (w.slider("  sky fill", volfog_.ambient, 0.0f, 1.0f, false, "%.2f")) {
                    volfog_.invalidate();
                    invalidate();
                }
                // How far the 64 slices are stretched. Short and the haze stops
                // dead at a visible wall; long and every slice is spent on air
                // too distant to resolve, so the beams in the first ten metres
                // coarsen.
                if (w.slider("  grid reaches", volfog_.farD, 50.0f, 1200.0f, false, "%.0f m")) {
                    volfog_.invalidate();
                    invalidate();
                }
                w.text("  160x90x64 froxels, one shadow ray each");
            }
        } else {
            w.text(fmt("Volumetric fog unavailable: %s", volfog_.status().c_str()));
        }
        w.separator();

        // ---- PHASE B: demodulation ------------------------------------------
        if (w.checkbox("Demodulate albedo before denoising", opt_.demodulate)) {
            tracer_.setDemodulate(opt_.demodulate);
            invalidate();
        }
        w.text("   splits lighting from texture. Ray Reconstruction does this");
        w.text("   internally, so leaving it off is correct while RR is on.");
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

        // ---- PHASE D: frame generation --------------------------------------
        //
        // This one changes the frame RATE rather than the frame, so the reading
        // beside it is the SDK's own count of what the swapchain actually
        // presented -- not a guess from the frame timer. DLSS-G silently falls
        // back to real frames whenever it cannot generate a good one, and this
        // is the only way to see that happen.
        if (sl_.hasFrameGeneration()) {
            // ONLY THE MODES THIS CARD CAN ACTUALLY DO. 3x and 4x are DLSS 4
            // multi-frame generation and need an RTX 50-series; on Ada, DLSS-G
            // silently clamps them to 2x, so offering all four would give three
            // settings that behave identically and no clue as to why.
            const int maxGen = sl_.maxGeneratedFrames();
            Falcor::Gui::DropdownList kFg = {
                {uint32_t(FrameGen::Off), "Off"},
                {uint32_t(FrameGen::On2x), "2x"},
            };
            if (maxGen >= 2) kFg.push_back({uint32_t(FrameGen::On3x), "3x"});
            if (maxGen >= 3) kFg.push_back({uint32_t(FrameGen::On4x), "4x"});
            uint32_t g = uint32_t(opt_.frameGen);
            if (w.dropdown("Frame generation", kFg, g)) {
                opt_.frameGen = FrameGen(g);
                const Falcor::uint2 ren{uint32_t(tracer_.width()), uint32_t(tracer_.height())};
                const Falcor::uint2 out{uint32_t(tracer_.outWidth()),
                                        uint32_t(tracer_.outHeight())};
                sl_.setFrameGeneration(opt_.frameGen, ren, out);
                slReset_ = true;
            }
            if (opt_.frameGen != FrameGen::Off)
                w.text(fmt("   %s", sl_.frameGenStatus().c_str()));
            // The single precondition that decides whether any of the above can
            // work, stated rather than left to be inferred. DLSS-G wraps the
            // swap chain from a hook on its creation, so it must have been given
            // the device BEFORE Falcor built one. If this line ever reads "late",
            // frame generation is inert no matter what everything else reports.
            w.text(fmt("   device handed to Streamline: %s",
                       sl_.earlyDeviceHandover() ? "at creation (correct)"
                                                 : "late -- DLSS-G missed the swapchain"));
            if (maxGen < 3)
                w.text(fmt("   this GPU generates up to %dx (3x/4x need RTX 50-series)",
                           maxGen + 1));
        } else if (sl_.gpuSupportsFrameGeneration()) {
            // The distinction is worth spelling out: the card can do it, this
            // BUILD cannot, and the reason is a specific missing piece rather
            // than a limitation of the hardware.
            w.text("Frame generation: unavailable in this build");
            w.text("   the GPU supports it, but the swapchain is not a Streamline");
            w.text("   proxy -- see patch_gfx_interposer.py. Known to crash the");
            w.text("   viewer when applied, so it is off by default.");
        } else if (sl_.available()) {
            w.text("Frame generation: not supported on this GPU");
        } else {
            w.text(fmt("Frame generation: %s", sl_.status().c_str()));
        }
        w.separator();

        if (w.slider("Bounces", opt_.r.maxDepth, 1, 32)) invalidate();
        if (w.slider("Bounces (moving)", opt_.movingDepth, 1, 32)) invalidate();
        // Exposure changes no sample already drawn, so it deliberately does NOT
        // throw the accumulation away. Nor does the toe -- both are the curve
        // between the film and the screen, not the film.
        w.slider("Exposure", opt_.r.exposure, 0.05f, 40.0f);
        w.slider("Shadow lift", opt_.r.shadowLift, 0.03f, 0.20f, false, "%.3f toe");
        w.slider("Walk speed", player_.walk, 0.2f, 200.0f);
        // No invalidate: it changes nothing that has already been traced, only
        // how far the next mouse movement will turn the view -- exactly like
        // exposure and walk speed above it.
        w.slider("Sensitivity", opt_.sensitivity, 0.02f, 0.50f, false, "%.3f deg/px");
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
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted(bakeStatus_.empty()
                                   ? "writes src/core/defaults.h; then rebuild.bat, in v6/"
                                   : bakeStatus_.c_str());
        w.separator();
        // v2 closed with the keys, because a panel that has to be discovered
        // twice is a panel nobody finds the second thing in.
        ImGui::TextUnformatted("Y or ESC  close        F1  controls, in the console");
        ImGui::PopStyleColor();
    }

    // -----------------------------------------------------------------------
    bool onKeyEvent(const KeyboardEvent &e) override {
        if (e.type != KeyboardEvent::Type::KeyPressed) return false;

        if (e.key == Input::Key::X) return true;  // held modifier for the wheel
        if (e.key == Input::Key::F) {
            player_.fly = !player_.fly;
            if (!player_.fly) player_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v6: %s\n", player_.fly ? "flying" : "walking");
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
                std::printf("v6: press ESC again to quit\n");
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
            std::printf("v6: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        if (e.key == Input::Key::RightBracket) {
            opt_.r.maxDepth = mini(32, opt_.r.maxDepth + 1);
            std::printf("v6: bounces = %d\n", opt_.r.maxDepth);
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
                std::printf("v6: day/night %s\n", lbl);
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
    VolFog volfog_;
    Ddgi ddgi_;
    Streamline sl_;
    NrdAo nrd_;
    // Tells Streamline the history is worthless -- a resize, a teleport, a
    // quality change. NOT ordinary camera motion, which is what the motion
    // vectors are for; resetting on that would throw the history away every
    // frame you walked.
    bool slReset_ = true;
    bool slPresentPending_ = false;
    float lastJitterX_ = 0.0f, lastJitterY_ = 0.0f;
    Falcor::uint2 slFgDim_{0, 0}, slFgOut_{0, 0};
    Player player_;
    Falcor::ref<Falcor::FullScreenPass> crosshair_;
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

    // One entry per displayed frame, in milliseconds. Kept whole rather than
    // reduced online because the interesting statistics are the tail ones, and
    // a running mean and variance cannot answer for a tail.
    std::vector<float> frameMs_, streamMs_;

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
        // NOT scaled by the field of view, deliberately. Zooming in with the
        // wheel does make the same wrist movement cover more of the frame --
        // that is what zooming IS, and it is the reason a scope is harder to
        // aim with than iron sights. Compensating for it would make the zoom
        // useless for the one thing it is good at, which is looking closely at
        // something without also having to hold still.
        const float dx = float(p.x - cx) * opt_.sensitivity;
        const float dy = float(cy - p.y) * opt_.sensitivity;
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
    // -----------------------------------------------------------------------
    // WHICH PHASES ACTUALLY CAME UP ON THIS MACHINE.
    //
    // Every phase in v6 is allowed to refuse, and a refusal is a one-line
    // message during a startup nobody reads. This prints the whole ledger at
    // once, so "is frame generation actually on?" is a question with an answer
    // rather than a guess from the frame rate.
    // -----------------------------------------------------------------------
    // ---------------------------------------------------------------------
    // v6's camera as a pair of matrices.
    //
    // The engine has none: its camera is a basis and two half-extents, built to
    // construct a ray rather than transform a vertex. NRD and Streamline both
    // want matrices, so they are assembled here, row-major row-vector
    // (clip = world * V * P), and each consumer converts if it must.
    //
    // The projection MUST match ndcDepth() in Trace.cs.slang -- same near, same
    // far, same A and B -- because the depth buffer these describe was written
    // by that function.
    // ---------------------------------------------------------------------------
    static void buildCameraMatrices(const V6Camera &c, float *viewToClip, float *worldToView) {
        const float n = 0.05f, f = 8000.0f;
        const float A = f / (f - n), B = -n * f / (f - n);

        const float dotU = c.pos.x * c.u.x + c.pos.y * c.u.y + c.pos.z * c.u.z;
        const float dotV = c.pos.x * c.v.x + c.pos.y * c.v.y + c.pos.z * c.v.z;
        const float dotW = c.pos.x * c.w.x + c.pos.y * c.w.y + c.pos.z * c.w.z;

        const float wv[16] = {c.u.x, c.v.x, c.w.x, 0.0f, c.u.y, c.v.y, c.w.y, 0.0f,
                              c.u.z, c.v.z, c.w.z, 0.0f, -dotU, -dotV, -dotW, 1.0f};
        const float vc[16] = {1.0f / c.halfW, 0.0f,          0.0f, 0.0f,
                              0.0f,           1.0f / c.halfH, 0.0f, 0.0f,
                              0.0f,           0.0f,          A,    1.0f,
                              0.0f,           0.0f,          B,    0.0f};
        for (int i = 0; i < 16; ++i) {
            worldToView[i] = wv[i];
            viewToClip[i] = vc[i];
        }
    }

    // ---------------------------------------------------------------------
    // The frame rate, and what it means with frame generation on.
    //
    // fps_ counts frames THIS ENGINE DREW. Frame generation does not change
    // that number -- it lowers it slightly, because generating a frame costs
    // GPU time -- while doubling the number the display actually receives. So a
    // working 2x reads as FEWER frames per second by this counter, which is the
    // single most misleading thing about a frame generator and the reason this
    // shows both.
    //
    // The multiplier is the SDK's own count of frames presented per rendered
    // frame, not an assumption from the mode: DLSS-G drops back to presenting
    // only real frames whenever it cannot generate a good one, and that shows
    // up here as the presented figure quietly collapsing back onto the drawn
    // one.
    // ---------------------------------------------------------------------
    std::string fpsLabel() const {
        if (opt_.frameGen == FrameGen::Off || !sl_.hasFrameGeneration())
            return fmt("%.0f fps", fps_);
        const int mult = sl_.framesPresented();
        // NOT "none generated". numFramesActuallyPresented has to be read in
        // step with the present thread -- Streamline says so every time it is
        // asked from anywhere else -- and this counter runs on the render
        // thread, so a 1 here means "could not measure", not "did not happen".
        // Claiming the stronger thing sent a real investigation down the wrong
        // road for hours; the honest reading is that the engine cannot tell.
        if (mult <= 1)
            return fmt("%.0f fps drawn, generated frames not measurable here", fps_);
        return fmt("%.0f fps drawn -> %.0f presented (%dx)", fps_, fps_ * float(mult), mult);
    }

    void reportPipeline() {
        std::printf("\nv6 pipeline\n");
        std::printf("  A shading      DDGI irradiance probes  : %s\n",
                    ddgi_.available() ? "on" : ddgi_.status().c_str());
        if (ddgi_.available())
            std::printf("                 %d probes x %d rays = %.0fk rays/frame, %.0f m spacing\n",
                        ddgi_.numProbes(), ddgi_.raysPerProbe(),
                        double(ddgi_.numProbes()) * ddgi_.raysPerProbe() / 1000.0,
                        double(kProbeSpacing));
        std::printf("  A air          volumetric fog          : %s\n",
                    volfog_.active() ? "on (160x90x64 froxels, one shadow ray each)"
                                     : (volfog_.available() ? "off (no fog at all)"
                                                            : volfog_.status().c_str()));
        std::printf("  B demodulate   albedo out of lighting  : %s\n",
                    opt_.demodulate ? "on" : "off (Ray Reconstruction demodulates internally)");
        std::printf("  C reconstruct  DLSS Ray Reconstruction : %s\n",
                    dlss_.available() ? "on" : dlss_.status().c_str());
        std::printf("  D upscale      DLSS Super Resolution   : %s\n",
                    sl_.hasSuperResolution() ? "available (folded into Ray Reconstruction)"
                                             : "via NGX with Ray Reconstruction");
        std::printf("    frame generation                      : %s\n",
                    sl_.hasFrameGeneration()
                        ? (opt_.frameGen == FrameGen::Off ? "available, off"
                                                          : frameGenName(opt_.frameGen))
                        : (sl_.available() ? "not supported on this GPU"
                                           : sl_.status().c_str()));
        std::printf("  C aux          NRD ambient occlusion   : %s\n",
                    (opt_.ao || opt_.aoView) ? nrd_.status().c_str() : "off");
        std::printf("    reflex (frame generation needs it)    : %s\n",
                    sl_.hasReflex() ? "on" : "no");
        if (!sl_.featureReport().empty())
            std::printf("\n  streamline features\n%s", sl_.featureReport().c_str());
        std::printf("\n  probe shape    %d rays, %d/%d irradiance texels, %d/%d distance texels\n",
                    kProbeNumRays, kProbeIrrTexels, kProbeIrrInterior,
                    kProbeDistTexels, kProbeDistInterior);
        std::fflush(stdout);
    }

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
        // Same reason as the offline loop below: no onFrameRender here, so
        // nothing else would fill the fog grid.
        tracer_.renderVolFog(ctx, cam.gpu(tracer_.width(), tracer_.height()), r.fogDensity,
                             r.fogHeight, 0u);
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

        // -- PHASE A: converge the probes before the film starts ------------
        //
        // AN OFFLINE RENDER DOES NOT RUN THE FRAME LOOP, so nothing has ever
        // updated the probes and the atlas is black. Sampling it would not
        // merely look wrong, it would look DARK in a specific and misleading
        // way: the direct light would be right and every shadow would be a
        // hole, which reads as a bug in the tracer rather than an empty cache.
        //
        // So the probes are run on their own first. The hysteresis is 0.97, so
        // a probe is an exponential average with a time constant near thirty
        // iterations; a few hundred is comfortably converged, and because each
        // pass feeds the previous atlas back in, the multi-bounce term builds
        // up over the same loop. It costs about a second and it happens once.
        if (opt_.ddgi && ddgi_.available()) {
            opt_.r.ddgiBounce = opt_.ddgiBounce;
            const int warm = maxi(0, opt_.ddgiWarm);
            std::printf("  probes   converging %d passes ...", warm);
            std::fflush(stdout);
            const auto pt0 = std::chrono::steady_clock::now();
            ddgi_.setOrigin(cam.origin);
            for (int i = 0; i < warm; ++i) {
                if (opt_.ddgiStage >= 1) ddgi_.uploadConstants(ctx);
                tracer_.setDdgiConsts(ddgi_.consts());
                if (opt_.ddgiStage >= 2)
                    tracer_.traceProbes(ctx, ddgi_, world_.sky.gpu(), opt_.r);
                if (opt_.ddgiStage >= 3) ddgi_.updateProbes(ctx, opt_.ddgiStage - 1);
                // Submitted every pass while bisecting, so a fault is attributed
                // to the pass that caused it rather than to a batch of thirty.
                if (opt_.ddgiStage < 5) ctx->submit(true);
            }
            ctx->submit(true);
            std::printf("\r  probes   %d passes in %.2f s          \n", warm,
                        secondsSince(pt0));
            std::fflush(stdout);
        }

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
            // THE OFFLINE RENDERER HAS TO LIGHT THE FOG ITSELF. It does not go
            // through onFrameRender, so the froxel grid would otherwise never be
            // filled -- and since the tracer samples that grid unconditionally,
            // an empty one reads as transmittance zero and the frame comes out
            // black rather than merely unfogged.
            //
            // Inside the loop because --walk moves the camera between samples,
            // and the grid is built in view space.
            tracer_.renderVolFog(ctx, c.gpu(tracer_.width(), tracer_.height()),
                                 opt_.r.fogDensity, opt_.r.fogHeight, uint32_t(s));
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
            std::fprintf(stderr, "v6: could not write %s\n", opt_.out.c_str());
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

    void chooseSpawn() {
        uint32_t seed = opt_.spawnSeed;
        if (seed == 0) {
            const uint64_t t =
                uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
            // Mixed rather than truncated: the low bits of a steady clock move
            // in lockstep with its resolution, and two launches a millisecond
            // apart should not land next to each other.
            seed = uint32_t(t * 0x9E3779B97F4A7C15ull >> 32) | 1u;
        }

        const VoxelTerrain &t = world_.terrain;
        const float wl = t.waterLevel;
        float bestX = opt_.camX, bestZ = opt_.camZ;
        bool found = false;

        for (uint32_t i = 0; i < 512 && !found; ++i) {
            // A disc, sampled with a square root so the points are spread over
            // the AREA rather than piled up near the middle.
            const float r = 200.0f + 2800.0f * sqrtf(hashUnit(seed + 1u, i));
            const float a = hashUnit(seed + 2u, i) * 6.2831853f;
            const float x = r * cosf(a), z = r * sinf(a);

            const float h = t.heightM(x, z);
            // WELL CLEAR OF THE SHORE, not merely out of the water. topMaterial
            // paints a sand band for the first 3.4 m above the waterline, so a
            // spawn a metre up is a spawn on a beach -- which is the one part
            // of this world with no trees in it and the last place to open a
            // forest in.
            if (h < wl + 5.0f) continue;

            const int ci = int(floorf(x / VOXEL_M)), cj = int(floorf(z / VOXEL_M));
            const int slope = maxi(absi(t.heightVox(ci + 1, cj) - t.heightVox(ci - 1, cj)),
                                   absi(t.heightVox(ci, cj + 1) - t.heightVox(ci, cj - 1)));
            if (slope >= VoxelTerrain::kTreeSlope) continue;  // scree, not ground

            // The stand-density gate the trees themselves are planted through.
            // Above 0.34 there is a wood; above about 0.62 it is a thicket, and
            // waking up in one is waking up in a wall of trunks.
            const float dens = t.standDensity(x, z);
            if (dens < 0.34f || dens > 0.62f) continue;

            bestX = x;
            bestZ = z;
            found = true;
        }

        opt_.camX = bestX;
        opt_.camZ = bestZ;
        std::printf("  spawn    %.1f, %.1f  (--spawn %u to come back here)\n", bestX, bestZ,
                    unsigned(seed));
        std::fflush(stdout);
    }

    // A spawn inside a trunk is a spawn you cannot walk out of: a tree is a
    // wall at every height, so the collision resolver has nowhere to push you.
    // The terrain test above cannot see trees -- they are placed per chunk and
    // no chunk existed yet -- so this runs once the ring is resident and steps
    // outward until the body fits.
    void nudgeOutOfSolids() {
        // NOT named 'near'. windows.h, which this file includes for the mouse
        // capture, still defines near and far as empty macros from the segmented
        // memory era, and the error it produces names neither of them.
        std::vector<Solid> nearby;
        world_.collidersNear(player_.pos, 8.0f, &nearby);
        auto blocked = [&](float x, float z) {
            for (const Solid &s : nearby)
                if (!s.standable && s.hx > 0.0f && s.hz > 0.0f &&
                    touches(s, x, z, player_.halfWidth))
                    return true;
            return false;
        };
        if (!blocked(player_.pos.x, player_.pos.z)) return;

        // Outward in rings rather than in a random walk, so the nudge is the
        // SHORTEST one that works and the spot stays the spot that was chosen.
        for (int ring = 1; ring <= 16; ++ring) {
            const float rad = 0.5f * float(ring);
            for (int step = 0; step < 16; ++step) {
                const float a = 6.2831853f * float(step) / 16.0f;
                const float x = player_.pos.x + rad * cosf(a);
                const float z = player_.pos.z + rad * sinf(a);
                if (blocked(x, z)) continue;
                opt_.camX = x;
                opt_.camZ = z;
                player_.placeOnGround(walkWorld(), x, z);
                pos_ = player_.eyePosition();
                std::printf("  spawn    stepped %.1f m clear of a trunk\n", rad);
                std::fflush(stdout);
                return;
            }
        }
    }

    // -----------------------------------------------------------------------
    // v2's look, pushed for the life of one panel and popped after it.
    //
    // A guard object rather than a pair of calls, because ImGui's style stack
    // is strictly balanced and an early return between a push and its pop
    // corrupts every panel drawn afterwards -- silently, and not necessarily
    // this frame.
    // -----------------------------------------------------------------------
    // v2's rule for how big the text is: proportional to the window, and
    // stopped at both ends -- below twelve pixels Consolas stops being legible
    // over a moving render, and above twenty-four a settings panel starts
    // reading as a poster.
    static float v2FontPx(float fbH) { return clampf(fbH / 64.0f, 12.0f, 24.0f); }

    struct styleV2 {
        Gui *gui;
        // What the windows have to pass to SetWindowFontScale to land on
        // v2FontPx. Falcor loads its fonts at fourteen points times whatever
        // the display scaling is, so the number is not knowable up front.
        float scale = 1.0f;

        styleV2(Gui *g, float veil, float fbH) : gui(g) {
            // The same fixed-pitch face v2 asked GDI for. Falcor registers it
            // at startup; it only has to be switched on.
            gui->setActiveFont("monospace");
            const float target = v2FontPx(fbH);
            scale = target / maxf(1.0f, ImGui::GetFontSize());

            ImGuiStyle &st = ImGui::GetStyle();
            saved_ = st;
            // In proportion to the text, clamped at both ends -- see the note
            // above the palette.
            const float r = clampf(target, 10.0f, 22.0f);
            st.WindowRounding = r;
            st.FrameRounding = r * 0.4f;
            st.GrabRounding = r * 0.4f;
            // A THIN MARKER, not a pill. ImGui centres a slider's value inside
            // its trough and draws the grab wherever the value sits, so a wide
            // grab spends much of its travel parked on top of the number it is
            // there to set -- which is the one thing on the row that has to be
            // readable while you drag it.
            st.GrabMinSize = 8.0f;
            st.WindowPadding = ImVec2(16.0f, 14.0f);
            st.ItemSpacing = ImVec2(8.0f, 6.0f);
            st.WindowBorderSize = 0.0f;

            ImVec4 panel = ui::kPanel();
            panel.w *= veil;
            ImGui::PushStyleColor(ImGuiCol_WindowBg, panel);
            ImGui::PushStyleColor(ImGuiCol_TitleBg, panel);
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, panel);
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, panel);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kText());
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui::kDim());
            // The selection bar, which in v2 was the highlight behind the row
            // the cursor was on. Here it is the trough and fill of the sliders,
            // which are the rows.
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_Header, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_Separator, ui::rgb(58, 70, 88));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::kPanel());
            colors_ = 19;
        }
        ~styleV2() {
            ImGui::PopStyleColor(colors_);
            ImGui::GetStyle() = saved_;
            gui->setActiveFont("");
        }
        ImGuiStyle saved_;
        int colors_ = 0;
    };

    // -----------------------------------------------------------------------
    // The crosshair.
    //
    // WHITE THROUGH A BLEND OF OneMinusDstColor, which is `1 * (1 - dst)` --
    // the frame under the mark, inverted. v2 reached the same pixels through
    // glLogicOp(GL_XOR) against white, because on eight-bit channels XOR with
    // white is a bitwise NOT; the fixed-function pipeline had no difference
    // blend and glBlendEquation needed an extension loader it did not have.
    // Here the ordinary blender does it, and the shader never has to read the
    // target it is writing to. See shaders/Crosshair.ps.slang.
    // -----------------------------------------------------------------------
    void makeCrosshair() {
        crosshair_ = Falcor::FullScreenPass::create(getDevice(), "v6/shaders/Crosshair.ps.slang");
        Falcor::BlendState::Desc bd;
        bd.setRtBlend(0, true).setRtParams(0, Falcor::BlendState::BlendOp::Add,
                                           Falcor::BlendState::BlendOp::Add,
                                           Falcor::BlendState::BlendFunc::OneMinusDstColor,
                                           Falcor::BlendState::BlendFunc::Zero,
                                           // Alpha is left exactly as it was: the
                                           // swapchain's is not ours to invert.
                                           Falcor::BlendState::BlendFunc::Zero,
                                           Falcor::BlendState::BlendFunc::One);
        crosshair_->getState()->setBlendState(Falcor::BlendState::create(bd));
    }

    void drawCrosshair(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) {
        // The settings panel owns the middle of the screen, and while it is up
        // the mouse is a cursor -- a mark that does not follow it is in the way.
        if (!crosshair_ || menuOpen_) return;

        const float w = float(target->getWidth()), h = float(target->getHeight());
        // v2's sizes, and v2's whole-number scaling rule with them: 32 px across
        // and 4 px thick at 1080p, DOUBLING at 2160p rather than growing
        // continuously, so the bars stay hard-edged instead of landing half on a
        // pixel and going grey.
        const float scale = maxf(1.0f, floorf(h / 1080.0f));

        auto var = crosshair_->getRootVar();
        var["gCrossCB"]["gSize"] = float2(w, h);
        var["gCrossCB"]["gArm"] = 16.0f * scale;
        var["gCrossCB"]["gHalf"] = 2.0f * scale;
        crosshair_->execute(ctx, target);
    }

    // -----------------------------------------------------------------------
    // The profile report.
    //
    // PERCENTILES, NOT AN AVERAGE. What a person feels walking through this
    // wood is the worst frame in the last second, not the mean of the last
    // hundred -- and the two move independently, because the streamer's cost
    // arrives in bursts while the renderer's is flat. So the tail is reported
    // in full, and every frame that cost more than twice the median is counted
    // as a hitch and attributed: streaming, or the renderer.
    // -----------------------------------------------------------------------
    void printProfile() {
        if (frameMs_.empty()) return;
        // The first frames of a run are the prime finishing and the first
        // structure builds landing, and they are not what the engine does when
        // it is running. Dropped so a steady-state number is a steady-state
        // number.
        const size_t warm = mini(size_t(30), frameMs_.size() / 4);
        std::vector<float> f(frameMs_.begin() + warm, frameMs_.end());
        std::vector<float> u(streamMs_.begin() + warm, streamMs_.end());
        const size_t n = f.size();
        if (n == 0) return;

        double sum = 0.0, streamSum = 0.0;
        for (size_t i = 0; i < n; ++i) { sum += f[i]; streamSum += u[i]; }

        std::vector<float> sorted = f;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double q) { return sorted[mini(n - 1, size_t(q * double(n)))]; };
        const float med = pct(0.50);

        size_t hitches = 0, streamHitches = 0;
        float worstStream = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (f[i] > 2.0f * med) {
                ++hitches;
                // Attributed to the streamer when the streamer accounts for
                // most of the overshoot -- which is a claim the numbers can
                // support, unlike "it felt like loading".
                if (u[i] > 0.5f * (f[i] - med)) ++streamHitches;
            }
            worstStream = maxf(worstStream, u[i]);
        }

        // The device-side breakdown, if the profiler collected one.
        if (Falcor::Profiler *prof = getDevice()->getProfiler()) {
            std::string line;
            for (Falcor::Profiler::Event *e : prof->getEvents()) {
                if (!e) continue;
                const std::string path = e->getName();
                // Falcor names nested events by their path; the leaf is enough.
                const size_t slash = path.find_last_of('/');
                const std::string leaf =
                    slash == std::string::npos ? path : path.substr(slash + 1);
                if (leaf != "trace" && leaf != "reconstruct" && leaf != "tonemap" &&
                    leaf != "fog volume" && leaf != "fog apply" && leaf != "ddgi")
                    continue;
                line += fmt("   %s %.2f ms", leaf.c_str(), e->getGpuTimeAverage());
            }
            if (!line.empty()) std::printf("  gpu      %s\n", line.c_str());
        }

        const World::Profile w = world_.profile();
        std::printf(
            "\nv6 profile -- %zu frames at %dx%d -> %dx%d, %s\n"
            "  frame     mean %.2f ms (%.0f fps)   median %.2f   p95 %.2f   p99 %.2f   max %.2f\n"
            "  hitches   %zu over 2x median (%.2f%%), %zu of them streaming\n"
            "  stream    %.3f ms/frame average, worst frame %.2f ms\n",
            n, tracer_.width(), tracer_.height(), tracer_.outWidth(), tracer_.outHeight(),
            tracer_.denoising() ? dlssQualityName(opt_.dlssQuality) : "accumulate",
            sum / double(n), 1000.0 * double(n) / maxf(1e-6f, float(sum)), med, pct(0.95),
            pct(0.99), sorted[n - 1], hitches, 100.0 * double(hitches) / double(n), streamHitches,
            streamSum / double(n), worstStream);
        std::printf(
            "  chunks    %zu meshed on workers, %zu adopted on the main thread\n"
            "  mesh      %.1f ms/chunk (off-thread)\n"
            "  blas      %.2f ms/batch over %zu batches   %.1f ms total (MAIN THREAD)\n"
            "  pool      %.2f ms/chunk   %.1f ms total    (MAIN THREAD)\n"
            "  tlas      %.2f ms/rebuild over %zu rebuilds  %.1f ms total (MAIN THREAD)\n",
            w.meshed, w.adopted, w.meshed ? w.meshMs / double(w.meshed) : 0.0,
            w.blasCalls ? w.blasMs / double(w.blasCalls) : 0.0, w.blasCalls, w.blasMs,
            w.adopted ? w.poolMs / double(w.adopted) : 0.0, w.poolMs,
            w.tlasCalls ? w.tlasMs / double(w.tlasCalls) : 0.0, w.tlasCalls, w.tlasMs);
        std::printf("  drain     %.1f ms over %zu drains (%zu forced)   take %.1f   rering %.1f\n",
                    w.drainMs, w.drains, w.forcedDrains, w.takeMs, w.reringMs);
        std::printf("  compact   %.0f MB built -> %.0f MB kept (%.0f%%), %zu still pending\n"
                    "  pools     %.0f MB in %zu recycled buffers\n",
                    w.uncompactedMb, w.compactedMb,
                    w.uncompactedMb > 0.0 ? 100.0 * w.compactedMb / w.uncompactedMb : 0.0,
                    w.pendingCompactions, w.poolMb, w.poolBuffers);
        std::fflush(stdout);
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
    // The path comes from V6_SOURCE_DIR, baked in by the build, rather than
    // being derived from the working directory -- the launcher runs the exe
    // from C:\voxelbit, so anything relative would land in the wrong tree and
    // report success while writing nothing anyone would ever compile.
    // -----------------------------------------------------------------------
    std::string bakeDefaults() {
#ifndef V6_SOURCE_DIR
        return "bake unavailable: built without V6_SOURCE_DIR";
#else
        // V6_SOURCE_DIR is "<engine>/src", so the engine root -- and the
        // rebuild script the user is about to be told to run -- is one level up.
        // Derived rather than hardcoded so a copy of this tree elsewhere still
        // reports its own path.
        const std::string srcDir = V6_SOURCE_DIR;
        const std::string root =
            srcDir.size() > 4 ? srcDir.substr(0, srcDir.size() - 4) : srcDir;
        const std::string path = srcDir + "/core/defaults.h";
        char clockText[16];
        clock_.clock(clockText, sizeof(clockText));
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return "could not write " + path;

        std::fprintf(f,
            "// ---------------------------------------------------------------------------\n"
            "// defaults.h -- the settings v6 starts with.\n"
            "//\n"
            "// GENERATED FILE. Everything below is rewritten wholesale by \"Bake as\n"
            "// default\" in the in-viewer settings menu (Y), so hand edits survive only\n"
            "// until the next bake -- but hand edits are perfectly fine, the format is just\n"
            "// constants and the file is checked in.\n"
            "//\n"
            "// The point of it is that the settings menu and the command line stop being\n"
            "// separate universes: fly around, tune the picture until it looks right, bake,\n"
            "// rebuild, and the thing you tuned is what v6 opens with.\n"
            "// ---------------------------------------------------------------------------\n"
            "#pragma once\n"
            "\n"
            "namespace v6 {\n"
            "namespace defaults {\n"
            "\n"
            "constexpr float kScale = %.2ff;\n"
            "constexpr int kDepth = %d;\n"
            "constexpr int kMovingDepth = %d;\n"
            "constexpr float kExposure = %.2ff;\n"
            "constexpr float kShadowLift = %.3ff;\n"
            "constexpr float kSpeed = %.1ff;\n"
            "constexpr float kSensitivity = %.3ff;\n"
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
            "}  // namespace v6\n",
            opt_.scale, opt_.r.maxDepth, opt_.movingDepth, opt_.r.exposure, opt_.r.shadowLift,
            player_.walk,
            opt_.sensitivity, player_.eye, fov_, sunAz_, sunEl_,
            int(getTargetFbo()->getWidth()),
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

}  // namespace v6
