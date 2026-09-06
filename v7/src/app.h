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
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include "core/defaults.h"
#include "gpu/neural.h"
#include "gpu/streamline.h"
#include "gpu/nrc.h"
#include "gpu/atmosphere.h"
#include "gpu/volfog.h"
#include "gpu/cuda.h"
#include "gpu/clusters.h"
#include "physics/physics.h"
#include "gpu/tracer.h"
#include "gpu/world.h"
#include "render/camera.h"
#include "render/player.h"
#include "render/recorder.h"
#include "scene/daynight.h"

namespace v7 {

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
    // BALANCED, and it is no longer selectable in the menu -- so this is not a
    // starting point, it is the mode. Quality upscales from more pixels than a
    // canopy needs at this frame rate; Balanced is where the samples the saving
    // buys are worth more than the resolution they cost. --dlss <mode> still
    // names all five, for an offline render or a benchmark.
    DlssQuality dlssQuality = DlssQuality::Balanced;

    // proven thing in the engine, it only exists on a device with cooperative
    // vectors, and a renderer whose default configuration depends on a network
    // that might be diverging is not a renderer anyone can debug.
    // DLSS 3 FRAME GENERATION, ON AT 2x BY DEFAULT.
    //
    // It was off, on the grounds that it needs the gfx.dll interposer patch and
    // an RTX 40-series card, so the safe default was the one that works
    // everywhere. That argument is weaker than it looked: every one of those
    // conditions is CHECKED at startup, and when any of them fails the engine
    // says so and runs without it. Defaulting to off meant the feature was
    // present, working, and silent unless you knew the flag.
    //
    // 2x rather than higher because 3x and 4x are DLSS 4 multi-frame generation
    // and need a 50-series; on Ada they are clamped to 2x anyway, so asking for
    // more would be asking for something that cannot happen.
    //
    // --fg off turns it back off.
    FrameGen frameGen = FrameGen::On2x;

    // RESTIR GI. Off by default -- it changes how indirect light is estimated,
    // and a renderer whose default estimator is the newest thing in the tree is
    // one nobody can get a reference image out of.
    // Issue a real cluster build at startup. Off by default -- see the note at
    // the call site.
    bool clusterTest = false;



    bool nrc = false;


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

    // -- the built-in recorder, on R -------------------------------------
    //
    // A FIXED CAPTURE RATE, NOT THE DISPLAY'S. The WebGPU game derived its
    // rate from the refresh rate, because it was sampling the compositor and
    // had no choice; this recorder owns its own timeline and can simply state
    // one. 60 is stated: it is what the footage will be watched at, it is
    // below what this engine sustains, and a rate that is not a property of
    // whichever monitor the window happened to be on is a rate two takes can
    // be cut together at.
    // The sun glare and lens flare, 0 = off. Given rather than defaulted so the
    // slider and a bake stay the source of truth when the flag is absent.
    float flare = 1.0f;
    bool flareGiven = false;
    int recFps = 60;
    // A SCRIPTED TAKE. Record this many seconds from startup, then exit --
    // exactly what --shot is for a still, and for the same reason: the only
    // honest way to check a recorder is to look at the file it produced, and
    // reaching that file through a window a person has to press R in is not
    // something a script or a headless run can do. Pairs with --shot-walk,
    // which is the case worth capturing: a still take tells you nothing about
    // frame pacing.
    float recSeconds = 0.0f;
    // The recording is never wider than this. Hardware H.264 stops at 4096 and
    // the encoder is the one thing in the pipeline that cannot be told to try
    // harder, so a 5K window records at 3840 rather than failing to record.
    int recMaxWidth = 3840;

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
    float fogAniso = 0.7f;
    bool fogAnisoGiven = false;
    int pineconesPerTree = 14;
    bool collideProbe = false;
    float fogAmbient = 0.60f;
    bool fogAmbientGiven = false;

    // How much sky light reaches air the up-ray finds under canopy. 1.0 is the
    // old unshadowed term and brings the sun blur back with it.
    float fogSkyUnder = 0.65f;
    bool fogSkyUnderGiven = false;

    // The cloud deck. Given-flags because the defaults live in gpu/clouds.h
    // beside the reasoning for them.
    float cloudCut = 0.45f;
    bool cloudCutGiven = false;
    float cloudVar = 0.26f;
    bool cloudVarGiven = false;
    float cloudSun = 2.2f;
    bool cloudSunGiven = false;
    float cloudMoonKey = 16.0f;
    bool cloudMoonKeyGiven = false;
    // The Hillaire sky. Defaulted from defaults.h like every other start-up
    // setting, so --no-atmosphere is the override rather than --atmosphere
    // being the opt-in.
    bool atmosphere = defaults::kAtmosphere;

    // Airglow and starlight, which the scattering model has no term for -- at 0
    // a deep night is physically honest and visually useless. Only reachable
    // here now that the sky group has left the menu; the default lives on
    // Atmosphere itself, so an unset flag changes nothing.
    float nightFloor = 0.0f;
    bool nightFloorGiven = false;

    // -- the sampler's pattern, and the two passes before the curve ---------
    //
    // GATED, AND OFF BY DEFAULT. All three change what a given seed puts on the
    // screen -- the mask by moving where the error lands, the other two by
    // moving the whole picture's level -- so defaulting them on would silently
    // invalidate every reference image this engine has ever been compared
    // against. Each is one flag away.
    bool blueNoise = defaults::kBlueNoise;
    bool autoExposure = defaults::kAutoExposure;
    float bloom = defaults::kBloom;
    float bloomThreshold = 1.0f;
    bool bloomThresholdGiven = false;
    float expKey = 0.14f;
    bool expKeyGiven = false;
    float moonScale = 1.0f;
    bool moonScaleGiven = false;
    // Pins the phase, 0 full .. 0.5 new. Without it the phase runs off the day
    // counter, which an offline render has no way to advance.
    float moonPhase = 0.0f;
    bool moonPhaseGiven = false;
    float moonKey = 1.0f;
    bool moonKeyGiven = false;

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
        // Before anything slow, so the window is where it belongs while the
        // wood is still loading rather than jumping there afterwards.
        if (!opt_.background) restoreWindowPlacement();

        world_.seed = opt_.r.seed;
        world_.terrain.grassDensity = clampf(opt_.grass, 0.0f, 1.0f);
        world_.flowerDensity = clampf(opt_.flowers, 0.0f, 1.0f);
        world_.rockDensity = clampf(opt_.rocks, 0.0f, 1.0f);
        world_.pineconesPerTree = maxi(0, opt_.pineconesPerTree);
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
        // THE SCALES GO ON BEFORE THE BUILD, not after. setSun below is what
        // rebuilds the sky, and anything applied to these after it has run is a
        // value the GPU never sees on the offline path -- which never rebuilds
        // again because it never starts the clock.
        if (opt_.moonScaleGiven) world_.sky.moonScale = opt_.moonScale;
        if (opt_.moonKeyGiven) world_.sky.moonKeyScale = opt_.moonKey;

        // THE PHASE HAS TO BE SET HERE TOO. This is the branch the OFFLINE path
        // takes -- it never starts the clock, so applySun (where the phase
        // normally comes from) never runs and the moon would be stuck full in
        // every --out render however --moon-phase was set.
        world_.sky.setMoonPhase(opt_.moonPhase * Sky::MOON_PERIOD_DAYS);
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
        std::printf("  models   %.2f s -- %d pines, %d rocks, %d flowers, %d mushrooms, "
                    "%d materials\n",
                    secondsSince(t0), world_.loadedPines, world_.loadedRocks,
                    world_.loadedFlowers, world_.loadedMushrooms, world_.palette.used());
        std::printf("           %.2f M unique tris in %d models at %.0f cm voxels\n",
                    world_.uniqueTris / 1e6,
                    world_.loadedPines + world_.loadedRocks + world_.loadedFlowers +
                        world_.loadedMushrooms,
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

        // AFTER the ring is resident, because a collider only exists once the
        // chunk that owns it has been adopted.
        if (opt_.collideProbe) { collideProbe(); shutdown(0); return; }
        std::printf("           %zu pines, %zu rocks, %zu flowers, %zu mushrooms,"
                    " %zu pinecones standing in them\n",
                    world_.decorCount(0), world_.decorCount(1), world_.decorCount(2),
                    world_.decorCount(3), world_.decorCount(4));
        std::printf("           %.0f ms of that was structure building, %.0f MB of tri pool\n",
                    world_.buildMs(), double(world_.poolBytes()) / (1024.0 * 1024.0));

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

        tracer_.init(getDevice(), &world_, neural_.available());
        makeCrosshair();

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
        if (opt_.moonScaleGiven) world_.sky.moonScale = opt_.moonScale;
        if (opt_.moonKeyGiven) world_.sky.moonKeyScale = opt_.moonKey;
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
            if (nrc_.init(getDevice(), kNrcMaxSamples))
                { std::printf("  cache    neural radiance cache ready (%u weights)\n",
                              NrcLayout::kElems);
                  tracer_.setNrc(&nrc_); }
            else
                std::printf("  cache    unavailable: %s\n", nrc_.status().c_str());
            std::fflush(stdout);
        }

        // THE IRRADIANCE PROBES. On by default wherever they can run, because
        // what they fix is not a nicety: under a canopy the indirect term is
        // most of the light there is, and a path tracer finds it only by
        // surviving roulette long enough to bounce its way back out to the sky.
        //
        // D3D12 only -- RTXGI's D3D12 backend is what v7 links -- so on Vulkan
        // this reports why and the cache mode drops to none.
        if (ddgi_.init(getDevice(), Falcor::getRuntimeDirectory() / "shaders" / "v7")) {
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
        if (opt_.outGiven) {
            tracer_.setDemodulate(opt_.demodulate);
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

        // The recorder compiles its conversion shader here rather than on the
        // first R: a first take that spent 300 ms in the shader compiler would
        // start by recording a hitch.
        recorder_.init(getDevice());

        printHelp();
        lastTime_ = std::chrono::steady_clock::now();
    }

    // -----------------------------------------------------------------------
    void onFrameRender(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) override {
        // APPLIED A SECOND TIME, ON THE FIRST FRAME ONLY. onLoad runs before
        // the swapchain is sized and before the window is shown, and anything
        // that happens in between -- Falcor's own sizing, a DPI change, the
        // shell placing the window -- lands after the first attempt and undoes
        // it. Re-applying once here is after all of that and still long before
        // a person could have moved the window themselves.
        if (!placedTwice_ && !opt_.background) {
            placedTwice_ = true;
            restoreWindowPlacement();
        }
        if (opt_.outGiven) return;

        // BEFORE ANYTHING IS DRAWN INTO IT. The target still holds the previous
        // frame at this point -- blitted, crosshaired and with the GUI composed
        // over it -- and that whole frame is what an interface has to be judged
        // on. Falcor does not clear it between frames, which is what makes this
        // possible at all.
        if (!opt_.shotUi.empty() && shotFrames_ >= opt_.shotFrame && target->getWidth() > 0) {
            target->getColorTexture(0)->captureToFile(0, 0, opt_.shotUi);
            std::printf("v7: wrote %s at %ux%u (window, with the interface)\n",
                        opt_.shotUi.c_str(), target->getWidth(), target->getHeight());
            std::fflush(stdout);
            shutdown(0);
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
        if (world_.update(pos_)) tracer_.resetAccumulation();
        const float updateMs =
            float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tu0)
                      .count());
        if (opt_.profile) {
            frameMs_.push_back(wallDt * 1000.0f);
            streamMs_.push_back(updateMs);
        }
        if (processInput(dt)) tracer_.resetAccumulation();

        // Simulation is over: the camera is where it is going to be and the ring
        // has streamed. Everything after this is the GPU's frame.
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
            tracer_.resolve(ctx, cfg, reconstructed, dt);

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
                const uint32_t traced = uint32_t(tracer_.width()) * uint32_t(tracer_.height());
                const uint32_t n = traced / uint32_t(maxi(1, nrc_.trainEvery));
                nrc_.trainBatch(ctx, mini(n, kNrcMaxSamples));
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
        // THIS IS THE HALF THAT WAS MISSING. v7 asked for frame generation and
        // set the Reflex markers, and stopped there -- so DLSS-G was switched on
        // with no depth, no motion vectors and no matrices, and had nothing to
        // interpolate between. It reported itself available the whole time.
        sl_.markRenderSubmitStart();
        if (sl_.frameGeneration() != FrameGen::Off) {
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
            if (slFgDim_.x != renderDim.x || slFgDim_.y != renderDim.y ||
                slFgOut_.x != outDim.x || slFgOut_.y != outDim.y) {
                slFgDim_ = renderDim;
                slFgOut_ = outDim;
                sl_.setFrameGeneration(opt_.frameGen, renderDim, outDim);
            }
            slReset_ = false;
        }

        if (target->getWidth() > 0 && target->getHeight() > 0) {
            ctx->blit(tracer_.display()->getSRV(), target->getRenderTargetView(0),
                      Falcor::uint4(0, 0, uint32_t(tracer_.displayWidth()),
                                    uint32_t(tracer_.displayHeight())));
            drawCrosshair(ctx, target);
        }

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
                shutdown(0);
                return;
            }
        }

        if (shotRequested_) {
            shotRequested_ = false;
            const std::string shot = outputPath("v7_shot_%03d.png", &shotIndex_);
            const char *name = shot.c_str();
            if (tracer_.writePng(ctx, name))
                std::printf("v7: wrote %s at %dx%d\n", name, tracer_.displayWidth(),
                            tracer_.displayHeight());
            else
                std::fprintf(stderr, "v7: could not write %s\n", name);
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
                    std::printf("v7: wrote %s at %dx%d after %d frames (%s)\n",
                                opt_.shotPath.c_str(), tracer_.displayWidth(),
                                tracer_.displayHeight(), shotFrames_,
                                tracer_.denoising() ? "reconstructed" : "accumulated");
                else if (!opt_.shotPath.empty())
                    std::fprintf(stderr, "v7: could not write %s\n", opt_.shotPath.c_str());
                if (opt_.profile) printProfile();
                std::fflush(stdout);
                shutdown(0);
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
            Gui::Window hud(pGui, "v7hud", {0, 0}, {12, 12}, kBare);
            ImGui::SetWindowFontScale(style.scale);
            // Set every frame rather than on first use: ImGui remembers window
            // positions in an ini file between runs, so "where I asked for it"
            // and "where it appears" are otherwise two different things.
            ImGui::SetWindowPos(ImVec2(12.0f, 12.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(235, 240, 248));
            // WHAT IS ON THE SCREEN, which is not what this counter naturally
            // measures. fpsFrames_ increments once per onFrameRender, so it
            // counts RENDERED frames; DLSS-G inserts its frames at the
            // swapchain, downstream of that function, and they never pass
            // through it. Reporting it raw made switching frame generation on
            // look like it COST frame rate -- the engine does render fewer real
            // frames, because DLSS-G takes GPU time and paces submission, while
            // more frames reach the screen.
            //
            // So the readout is rendered plus generated: one number, and the
            // one a person is actually looking at. With frame generation off
            // genFps_ is zero and this is exactly what it always was. The
            // rendered figure is still in the settings menu, where the two are
            // worth telling apart.
            ImGui::TextUnformatted(fmt("%.0f fps", fps_ + genFps_).c_str());
            ImGui::PopStyleColor();

            // THE BADGE IS DRAWN INTO THE WINDOW, NOT INTO THE FRAME, so it can
            // never end up in the recording -- see the note at the capture site
            // in onFrameRender. It pulses because a recorder that is running is
            // the one piece of state where "I did not notice it was still on"
            // is expensive.
            if (recorder_.recording()) {
                const float pulse = 0.55f + 0.45f * std::sin(float(ImGui::GetTime()) * 4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, pulse));
                ImGui::TextUnformatted(
                    fmt("REC  %s", clockLabel(recorder_.elapsed()).c_str()).c_str());
                ImGui::PopStyleColor();
                // The costs, only once they are non-zero. A held frame is the
                // recorder covering a hitch in the GAME; a dropped one is the
                // encoder falling behind the recorder. They are different
                // problems and the readout does not merge them.
                if (recorder_.heldFrames() || recorder_.droppedFrames()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(200, 160, 90));
                    ImGui::TextUnformatted(fmt("  %lld held  %lld dropped",
                                               (long long)recorder_.heldFrames(),
                                               (long long)recorder_.droppedFrames())
                                               .c_str());
                    ImGui::PopStyleColor();
                }
            } else if (recorder_.busy()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(255, 214, 120));
                ImGui::TextUnformatted("encoding...");
                ImGui::PopStyleColor();
            } else if (savedTake_.valid()) {
                // WHERE THE PERSON WHO PRESSED R IS ACTUALLY LOOKING. The take
                // is written and the recorder names it on stdout, but stdout is
                // behind the window -- and with nothing opening any more, a
                // silent stop is indistinguishable from a stop that failed.
                //
                // It FADES rather than waiting to be dismissed. An
                // acknowledgement is not a dialog: the thing wanted after a
                // take is the wood back, not another key to press.
                const double age = nowSeconds() - savedAt_;
                if (age >= kSavedNotice) {
                    savedTake_ = vb::Take{};
                } else {
                    const double f = (kSavedNotice - age) / 1.2;
                    const float a = float(f > 1.0 ? 1.0 : f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(150, 220, 160, a));
                    ImGui::TextUnformatted(fmt("saved  %s", savedTake_.path.c_str()).c_str());
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(150, 158, 170, a));
                    ImGui::TextUnformatted(fmt("  %lld frames  %s",
                                               (long long)savedTake_.frames,
                                               clockLabel(savedTake_.seconds()).c_str())
                                               .c_str());
                    ImGui::PopStyleColor();
                }
            }
        }

        if (!menuOpen_) return;

        styleV2 style(pGui, 1.0f, fbH);
        // ITS OWN FLAGS, and both differences from the readout matter.
        //
        // AllowMove, because without it Falcor passes ImGuiWindowFlags_NoMove
        // and the panel cannot be dragged no matter what the position code
        // does. That was half of why it sat still; the other half was the
        // re-centring below.
        //
        // NOT AutoResize, because the panel has outgrown the screen. Auto-sized
        // windows do not scroll -- they simply grow, and the rows past the
        // bottom edge become unreachable. Giving it an explicit height capped
        // to the window turns the overflow into a scrollbar.
        const Gui::WindowFlags kPanel = Gui::WindowFlags::AllowMove | Gui::WindowFlags::NoResize;
        Gui::Window w(pGui, "settings##v7", menuOpen_, {0, 0}, {0, 0}, kPanel);
        ImGui::SetWindowFontScale(style.scale);

        // WIDTH IN CHARACTERS, HEIGHT FROM THE CONTENT -- v2's rule exactly.
        // Seventy columns is the longest line the panel can hold, and fixing
        // the width is not only tidiness: right-aligning anything inside a
        // window that is auto-sizing to its own content is a feedback loop, and
        // the window grows a little wider every frame.
        const float cw = ImGui::CalcTextSize("0").x;
        // THE PANEL GREW WITH THE SLIDERS. Seventy columns was the width of the
        // longest LINE OF TEXT, which was the right rule while the sliders were
        // short. Three times the slider needs somewhere to put it, so the panel
        // is sized from the widget instead: the trough, the gap, and the label
        // beside it.
        const float sliderW = 3.0f * 12.0f * cw;  // was about twelve columns
        const float panelW = sliderW + 26.0f * cw + 32.0f;
        // Eighty-five percent of the window, so there is always visibly
        // something behind the panel -- a settings screen that covers the thing
        // being adjusted is a settings screen you have to close to judge.
        const float panelH = floorf(fbH * 0.85f);
        ImGui::SetWindowSize(ImVec2(panelW, panelH));

        // CENTRED ON OPEN, THEN YOURS. v2 centred it every frame, and its
        // reasoning for the position was right -- the middle of the screen is
        // where the eyes already are, unlike the top-left corner it used to sit
        // in. But setting the position EVERY frame is also what made the panel
        // immovable: ImGui moves a dragged window and the next frame put it
        // straight back, so it did not so much refuse to move as twitch.
        //
        // So the placement happens once, when the menu opens, and after that
        // the window is ImGui's to drag.
        //
        // AND WHERE IT OPENS IS WHERE IT WAS LEFT. It used to re-centre on
        // every opening, on the argument that a panel dragged somewhere
        // awkward is never lost. That is true and it is still the wrong
        // default: someone who moves a panel out of the way of the thing they
        // are tuning means it, and having it jump back to the middle of the
        // screen every time undoes the move as fast as they can make it.
        // Centring is now only what happens the FIRST time, before there is a
        // remembered place to prefer.
        //
        // Clamped to the framebuffer on the way back out, because the window
        // can be resized between one opening and the next -- a panel remembered
        // at x = 3000 on a wide monitor must not be off the edge of a narrow
        // one, which would leave it genuinely lost with no way to drag it back.
        if (!menuPlaced_) {
            if (menuPos_.x < 0.0f) {  // never opened: the middle of the screen
                menuPos_ = ImVec2(floorf(maxf(0.0f, (fbW - panelW) * 0.5f)),
                                  floorf(maxf(0.0f, (fbH - panelH) * 0.5f)));
            }
            const float mx = maxf(0.0f, fbW - panelW);
            const float my = maxf(0.0f, fbH - 40.0f);  // keep the title grabbable
            ImGui::SetWindowPos(ImVec2(clampf(menuPos_.x, 0.0f, mx),
                                       clampf(menuPos_.y, 0.0f, my)));
            menuPlaced_ = true;
        } else {
            // Read back every frame rather than on close: ImGui does not tell
            // us when a drag ends, and the menu can be shut by ESC, by Y or by
            // its own close box, so there is no one place a "save on close"
            // hook could live without one of the three missing it.
            menuPos_ = ImGui::GetWindowPos();
        }

        // EVERY WIDGET IN THE PANEL, one width. ImGui's default is a fraction of
        // the window, which would have made the sliders grow with the panel and
        // the panel grow with the sliders -- so it is stated once, explicitly,
        // and popped at the end of the function.
        // Falcor draws slider troughs at a width of its own, so asking ImGui is
        // not enough -- see Gui::setSliderWidth, added to v7's fork for this.
        Gui::setSliderWidth(sliderW);
        ImGui::PushItemWidth(sliderW);

        // The title line: the name on the left, and on the right the number
        // that everything below is a trade against. One line, both ends.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTitle());
        ImGui::TextUnformatted("v7  settings");
        {
            const std::string f = fmt("%.0f fps", fps_ + genFps_);
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

        // ── NO PRESETS AND NO RESOLUTION ROW ──────────────────────────────
        //
        // Removed at the user's asking, along with the five preset buttons
        // (Max FPS / Fast / Balanced / Sharp / Native) that set the same two
        // numbers between them. The engine renders at 100 % of the window and
        // that is now simply what it does -- defaults::kScale is 1.00 and
        // nothing in the menu moves it.
        //
        // --scale still exists and still works, because an offline render or
        // a benchmark has a real reason to ask for a different size and no
        // menu to ask it through. What is gone is the in-game control, not
        // the capability behind it.

        // ---- the denoiser ---------------------------------------------------
        //
        // NO ON/OFF ROW. Ray Reconstruction is always on -- at one sample a
        // pixel it is not an enhancement, it is the thing that makes the image
        // an image, and offering to switch it off is offering to break the
        // renderer. --no-dlss still exists for a reference render, which is the
        // only context where the accumulating film is the right answer.
        //
        // THREE MODES, NOT FIVE. Ultra performance and Performance are not
        // offered: below Balanced, Ray Reconstruction is upscaling from so few
        // pixels that a conifer canopy -- thin, high-frequency geometry with
        // bright sky behind it -- comes back as mush that no amount of
        // denoising recovers. The ENUM still has them and --dlss names them, so
        // a benchmark keeps a capability the in-game menu does not offer; the
        // same split --scale got.
        if (dlss_.available() && opt_.dlss) {
            Falcor::Gui::DropdownList modes = {
                {uint32_t(DlssQuality::Balanced), "Balanced"},
                {uint32_t(DlssQuality::Quality), "Quality"},
                {uint32_t(DlssQuality::Dlaa), "DLAA (no upscale)"},
            };

            // AND IF WE ARE IN A MODE THE LIST DOES NOT OFFER, SHOW IT ANYWAY.
            // This is not tidiness. Falcor's addDropdown scans the list for the
            // live value and leaves its index at -1 when it is not found, then
            // unconditionally does `values[curItem]` (Gui.cpp:668) -- so a value
            // outside the list is not a blank combo, it is an out-of-bounds read
            // on a std::vector. Reachable from the command line today with
            // `--dlss performance`.
            const uint32_t live = uint32_t(opt_.dlssQuality);
            bool listed = false;
            for (const auto &e : modes) listed = listed || e.value == live;
            if (!listed)
                modes.insert(modes.begin(), {live, dlssQualityName(opt_.dlssQuality)});

            uint32_t m = live;
            if (w.dropdown("Mode", modes, m)) {
                opt_.dlssQuality = DlssQuality(m);
                tracer_.setQuality(opt_.dlssQuality);
            }
        } else if (dlss_.available()) {
            // --no-dlss: the film is accumulating instead, and a reconstruction
            // mode is not a thing that has a meaning here.
            w.text("Ray Reconstruction off (--no-dlss) -- accumulating");
        } else {
            w.text(fmt("DLSS unavailable: %s", dlss_.status().c_str()));
        }

        // FRAME GENERATION IS NOT IN THIS MENU. It is on at 2x and stays there;
        // --fg 2x|3x|4x|off still works and the capability probes in
        // streamline.h still run, so a card that cannot do it still declines
        // quietly. Its diagnostics -- what DLSS-G says about itself, the
        // generated-frame counts, whether the device reached Streamline before
        // the swapchain -- are printed at start-up and under --stats, which is
        // where a diagnostic belongs.
        w.separator();


        // =====================================================================
        // GLOBAL ILLUMINATION
        // =====================================================================
        //
        // THERE IS NO "ENABLE GI" CHECKBOX AND THERE CANNOT BE. v7 is a path
        // tracer: every pixel is a random walk that keeps bouncing until it
        // dies, and the light it gathers on the way IS the indirect light.
        // Turning that off does not give a scene lit some other way, it gives a
        // scene lit by the sun and nothing else -- so the setting that would
        // switch GI off is "Bounces = 1", and it is a slider rather than a
        // checkbox because 2 and 3 and 8 are all useful answers.
        //
        // What everything under this heading decides is HOW the indirect light
        // is gathered and how much is spent gathering it. The three blocks are
        // in the order they matter: how long the paths are, then two different
        // ways of not having to trace so many of them.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted("GLOBAL ILLUMINATION -- path traced, always on");
        ImGui::PopStyleColor();

        // Path length. Two of them, because a moving camera cannot accumulate
        // and so cannot afford the same walk a still one can -- the shorter
        // number is what you actually see while walking.
        //
        // BOUNCES ARE NEARLY FREE AND PIXELS ARE NOT. Going 3 -> 8 costs a few
        // percent; the profile counters have said so on every run. The instinct
        // when the frame rate sags is always to cut bounces first, and it gives
        // up the interreflection between the trunks -- the thing that makes the
        // wood look lit rather than painted -- to buy almost nothing.
        if (w.slider("Bounces", opt_.r.maxDepth, 1, 32)) invalidate();
        if (w.slider("Bounces (moving)", opt_.movingDepth, 1, 32)) invalidate();

        // RUSSIAN ROULETTE, and it is not a minor knob. It decides how long
        // paths actually live: at 1 most of them die after two or three
        // bounces, which is why raising "Bounces" alone changes so little --
        // and why the radiance cache only pays once this is pushed out. The
        // estimator stays unbiased either way; what changes is where the
        // samples get spent.
        if (w.slider("Roulette starts at", opt_.r.rrStart, 1, 32)) invalidate();

        // Next-event estimation: how many sun samples each vertex takes, and
        // how deep down the path it keeps taking them. Depth 0 is direct light
        // only, and every step past that is another bounce whose own lighting
        // is resolved rather than left to chance -- which is most of what
        // separates a bright wood from a noisy one.
        if (w.slider("Sun samples", opt_.r.shadowRays, 1, 16)) invalidate();
        if (w.slider("Sun sample depth", opt_.r.shadowRayDepth, 0, 8)) invalidate();

        // THE SAME THING FOR THE SKY, and under a canopy it matters more than
        // the sun does -- the sun is occluded by definition in a shadow, so the
        // dome is the whole of the light. At 0 the dome is left to be found by
        // a bounce that happens to escape the needles, which is what made the
        // shadows read as black.
        if (w.slider("Sky samples", opt_.r.skyRays, 0, 16)) invalidate();
        if (w.slider("Sky sample depth", opt_.r.skyRayDepth, 0, 8)) invalidate();

        // Paths per pixel per frame. The only honest way to buy less noise, and
        // the only one that costs exactly what it looks like it costs.
        if (w.slider("Samples / frame", opt_.samplesPerFrame, 1, 64)) invalidate();

        // The firefly ceiling. A single specular path through a gap in the
        // canopy can return thousands of times the mean, and one such sample
        // is a white speck that accumulation takes a very long time to average
        // out. Clamping is a bias, deliberately taken.
        if (w.slider("Firefly clamp", opt_.r.clampIndirect, 1.0f, 200.0f)) invalidate();

        // THE SAME NUMBER OF SAMPLES, ARRANGED DIFFERENTLY. This does not
        // reduce variance -- it moves it up the spatial frequencies, where the
        // eye and Ray Reconstruction both discard far more of it. Costs one
        // texture fetch on the shallow dimensions and nothing else.
        //
        // Invalidates, because the film already holds samples drawn the other
        // way and mixing the two would converge to the same image through a
        // visibly worse middle.
        if (w.checkbox("Blue-noise sampling", tracer_.blueNoise)) invalidate();
        w.separator();

        // ---- the neural radiance cache --------------------------------------
        //
        // The batch count is the honest indicator and it is why it is on screen:
        // "on" tells you what was asked for, and the number climbing tells you
        // the network is actually being fed. A cache that is enabled but whose
        // count is stuck is a cache that is doing nothing, and without this row
        // that looks exactly like one that is working.
        if (nrc_.available()) {
            if (w.checkbox("Neural radiance cache", opt_.nrc)) {
                nrc_.enabled = opt_.nrc;
                invalidate();
            }
            if (opt_.nrc) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted(
                    fmt("   %s   %u batches", nrc_.warm() ? "predicting" : "warming up",
                        nrc_.batches())
                        .c_str());
                ImGui::PopStyleColor();
                w.checkbox("  keep learning", nrc_.training);
                if (w.slider("  from bounce", nrc_.queryDepth, 1, 6)) invalidate();
                w.slider("  train 1 px in", nrc_.trainEvery, 8, 512);
                w.slider("  learning rate", nrc_.learningRate, 0.0005f, 0.05f, false, "%.4f");
                // fp16 training does diverge, and when it does every query is a
                // NaN that the film will happily accumulate. There is no
                // recovering a poisoned network, so the only cure is on offer.
                if (w.button("  retrain from scratch")) nrc_.reset();
            }
        } else if (neural_.available()) {
            w.text(fmt("Neural radiance cache: %s", nrc_.status().c_str()));
        }
        w.separator();

        // ---- the indirect cache ---------------------------------------------
        //
        // THE ONE CONTROL THAT ACTUALLY MOVES THE SHADOWS. Under a canopy the
        // sun is occluded by definition, so nearly all the light in a shadow is
        // indirect -- and a path tracer only finds it by surviving roulette
        // long enough to bounce back out to the sky, which at foliage albedo
        // 0.19 most paths do not. A cache remembers it instead.
        //
        // ONE RADIO GROUP, NOT TWO CHECKBOXES, because the two caches answer
        // the same question in the same units and the tracer adds whichever it
        // is handed. Both on would count the indirect light twice.
        {
            const bool haveD = ddgi_.available();
            const bool haveS = sharc_.available();
            if (haveD || haveS) {
                int mode = opt_.r.giMode;
                bool changed = false;
                changed |= ImGui::RadioButton("Indirect cache: off", &mode, 0);
                if (haveD) {
                    ImGui::SameLine();
                    changed |= ImGui::RadioButton("probes", &mode, 1);
                }
                if (haveS) {
                    ImGui::SameLine();
                    changed |= ImGui::RadioButton("hash", &mode, 2);
                }
                if (changed && mode != opt_.r.giMode) {
                    opt_.r.giMode = mode;
                    invalidate();
                }

                if (opt_.r.giMode != 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                    ImGui::TextUnformatted(
                        fmt("   %s", opt_.r.giMode == 1 ? "DDGI irradiance probes, D3D12"
                                                        : "SHaRC hash grid, either backend")
                            .c_str());
                    ImGui::PopStyleColor();

                    // WHERE THE CACHE TAKES OVER, and 2 is measured rather than
                    // preferred: against a 32-bounce reference of the same
                    // frame, 1 comes out 22 % too bright in the darkest quarter
                    // -- a six-metre probe grid standing in for light one bounce
                    // from the eye -- and 2 lands within about 2 %.
                    if (w.slider("  from bounce", opt_.r.giDepth, 1, 6)) invalidate();
                    // A cheat, and labelled as one. The transport is right at 1;
                    // this is for when you want the wood lighter than it is.
                    if (w.slider("  strength", opt_.r.giStrength, 0.0f, 3.0f)) invalidate();
                }
            } else {
                w.text(fmt("Indirect cache: %s", ddgi_.status().c_str()));
            }
        }
        w.separator();

        // ---- what the hardware is doing under all of this --------------------
        //
        // Read-only where there is nothing to decide. Clusters and cooperative
        // vectors are capabilities of this device and this backend, not
        // preferences, and a checkbox for something the driver has already
        // refused is a checkbox that lies.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted(fmt("clusters   %s", clusters_.available() ? "available"
                                                                          : "not on this backend")
                                   .c_str());
        ImGui::TextUnformatted(
            fmt("neural     %s", neural_.available() ? "cooperative vectors" : "unavailable")
                .c_str());
        ImGui::TextUnformatted(fmt("cuda       %s", cuda_.available() ? "shared with the renderer"
                                                                     : "unavailable")
                                   .c_str());
        ImGui::PopStyleColor();
        w.separator();

        // Exposure changes no sample already drawn, so it deliberately does NOT
        // throw the accumulation away. Nor does the toe -- both are the curve
        // between the film and the screen, not the film.
        w.slider("Exposure", opt_.r.exposure, 0.05f, 40.0f);
        // THE KNOB FOR "SHADOWS ARE TOO DARK", and the range matters. This is
        // the ACES toe: the curve's slope near black is b/0.14, so the 0.14
        // default is a slope of exactly 1.0 -- linear, no crush. Below that the
        // curve is eating shadow detail; above it is deliberately lifting.
        //
        // It used to stop at 0.20 and that was too low to be the answer to
        // anything. 0.35 reaches a slope of 2.5, and the shoulder does not move
        // with it: b appears only in the linear term, so highlights roll off
        // identically at every setting on this slider.
        w.slider("Shadow lift", opt_.r.shadowLift, 0.03f, 0.35f, false, "%.3f toe");

        // THE ONE THAT ONLY TOUCHES THE DARK. The toe above is a parameter of
        // the tone curve and so acts on the whole frame; this falls to exactly
        // zero at the reach below it, which is why the midtones do not move.
        // Reach for this one when the undersides of the canopy are too dark and
        // the rest of the frame is right, which is the usual case in a wood.
        w.slider("Deep shadow lift", opt_.r.deepLift, 0.0f, 0.15f, false, "%.3f");
        // How far the corners fall off. 0 is off, which is where it starts --
        // the tone map runs it last, after the flare, so it darkens the
        // finished image rather than having the ghosts scatter back over it.
        // THE SUN GLARE HAD NO CONTROL AT ALL, which is how "I can see the sun
        // through the tree" ended up with no way to answer it from inside the
        // game. It is a look, and every other look in this menu is a slider.
        // 0 removes the glare and the ghosts entirely and costs nothing else --
        // the sun disc itself is drawn by the sky, not by this.
        w.slider("Sun glare", tracer_.flare, 0.0f, 2.0f, false, "%.2f");
        w.slider("Vignette", tracer_.vignette, 0.0f, 1.0f, false, "%.2f");

        // AUTO-EXPOSURE, BLOOM AND THE DEEP-LIFT'S REACH ARE NOT IN THIS MENU,
        // deliberately. All three are reachable from the command line
        // (--auto-exposure, --exposure-key, --bloom, --bloom-threshold,
        // --deep-range) and the first three bake, so nothing about them is
        // gone -- they are simply not worth the rows they cost here.
        w.slider("Walk speed", player_.walk, 0.2f, 200.0f);
        // No invalidate: it changes nothing that has already been traced, only
        // how far the next mouse movement will turn the view -- exactly like
        // exposure and walk speed above it.
        w.slider("Sensitivity", opt_.sensitivity, 0.02f, 0.50f, false, "%.3f deg/px");
        if (w.slider("Field of view", fov_, 10.0f, 100.0f)) invalidate();
        w.separator();

        // ---- the air, and the lens ------------------------------------------
        //
        // The density cap is five times what the analytic fog offered. That fog
        // washed out at 0.02 because its in-scatter was the unshadowed sky and
        // more of it only meant more grey; the froxel grid is shadowed, so more
        // density means deeper beams rather than flatter ones, and the range is
        // finally worth having.
        if (w.slider("Fog density", opt_.r.fogDensity, 0.0f, 0.10f, false, "%.4f /m"))
            invalidate();
        if (w.slider("Fog height", opt_.r.fogHeight, 1.0f, 200.0f, false, "%.0f m"))
            invalidate();

        if (volfog_.available()) {
            // Unchecking this leaves NO fog at all, not the old analytic
            // model -- that one is gone. It is here to measure what the two
            // passes cost, and to see the wood without any air in it.
            if (w.checkbox("Volumetric fog (all fog)", volfog_.enabled)) {
                volfog_.invalidate();
                invalidate();
            }
            if (volfog_.enabled) {
                // Forward scattering. This is the knob that decides whether the
                // air near the sun GLOWS or whether the whole volume simply
                // lifts: at 0 the phase is a sphere and the fog is milk, and by
                // 0.9 nearly all the scattered light goes on in the direction it
                // was already travelling, which is what makes a beam a beam.
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

                // How much sky light reaches air the up-ray found under
                // canopy. 1.00 is the old unshadowed behaviour and brings the
                // sun blur back with it; 0 puts black holes under the trees.
                if (w.slider("  sky under canopy", volfog_.skyShadow, 0.0f, 1.0f, false,
                             "%.2f")) {
                    volfog_.invalidate();
                    invalidate();
                }

                // How far the 64 slices are stretched. Short and the haze stops
                // dead at a visible wall; long and every slice is spent on air
                // too distant to resolve, so the beams in the first ten metres
                // coarsen. 400 m is about where the wood stops being legible.
                if (w.slider("  march reaches", volfog_.farD, 50.0f, 1200.0f, false, "%.0f m")) {
                    volfog_.invalidate();
                    invalidate();
                }
                w.slider("  settle (still)", volfog_.settleStill, 0.02f, 1.0f, false, "%.2f");
                w.slider("  settle (moving)", volfog_.settleMoving, 0.05f, 1.0f, false, "%.2f");
                w.text("  160x90x64 froxels, one shadow ray each");
            }
        }
        // ---- WHICH SKY IS NOT A QUESTION THIS MENU ASKS ---------------------
        //
        // Atmospheric scattering is ON and stays on. The Preetham fit it
        // replaced survives only as a fallback for a machine where the LUT
        // shaders will not compile, and as --no-atmosphere for anyone comparing
        // against an image taken before 2026-09-06. Neither is worth a row, and
        // the wrong answer to it silently freezes every sunset.
        //
        // The night floor came out with it and is now --night-floor, because a
        // setting whose only access was a sub-row of a checkbox that no longer
        // exists is a setting nobody can reach. It stands in for airglow and
        // starlight: the model knows about sunlight and nothing else, so at 0 a
        // deep night is honestly -- and uselessly -- black.
        //
        // AND NOTE THE TURBIDITY ROW BELOW. It is a PREETHAM parameter, and the
        // scattering path carries its own fixed aerosol profile and ignores it,
        // so with the atmosphere always on that slider moves nothing anybody
        // can see. It is left alone because --no-atmosphere still reads it.
        if (w.slider("Sky turbidity", opt_.turbidity, 1.8f, 8.0f)) {
            applySun(true);
            invalidate();
        }
        // NO APERTURE OR FOCUS ROW HERE, deliberately. Depth of field is not
        // something this game does: the lens model still exists for the
        // OFFLINE paths (--aperture / --focus, and the thin lens in
        // Trace.cs.slang they drive), because a still frame is where it earns
        // its keep, but the viewer is a pinhole and has no control for it.
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
                                   ? "writes src/core/defaults.h; then rebuild.bat, in v7/"
                                   : bakeStatus_.c_str());
        w.separator();
        // v2 closed with the keys, because a panel that has to be discovered
        // twice is a panel nobody finds the second thing in.
        ImGui::TextUnformatted("Y or ESC  close        F1  controls, in the console");
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();
    }

    // -----------------------------------------------------------------------
    bool onKeyEvent(const KeyboardEvent &e) override {
        if (e.type != KeyboardEvent::Type::KeyPressed) return false;

        if (e.key == Input::Key::X) return true;  // held modifier for the wheel
        if (e.key == Input::Key::F) {
            player_.fly = !player_.fly;
            if (!player_.fly) player_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v7: %s\n", player_.fly ? "flying" : "walking");
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
                std::printf("v7: press ESC again to quit\n");
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
            std::printf("v7: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        if (e.key == Input::Key::RightBracket) {
            opt_.r.maxDepth = mini(32, opt_.r.maxDepth + 1);
            std::printf("v7: bounces = %d\n", opt_.r.maxDepth);
            tracer_.resetAccumulation();
        }
        if (e.key == Input::Key::R) toggleRecording();
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
                std::printf("v7: day/night %s\n", lbl);
                std::fflush(stdout);
                return true;
            }
            // THE BARE WHEEL DOES NOTHING. It used to zoom, and every stray
            // scroll threw the accumulated film away and left the view at some
            // field of view nobody chose. The slider in the settings menu is
            // the deliberate way to set it, which is the only way it wants
            // setting.
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
    // exists precisely so v7 can be launched without stealing it.
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
        return std::string(base) + "\\voxelbit-v7-window.txt";
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
        // A take still finalising owns a thread and a sink writer. Abandoning
        // it drops the file rather than waiting on an encoder while the device
        // is being torn down underneath it.
        recorder_.abandon();
        saveWindowPlacement();
    }

    void onResize(uint32_t, uint32_t) override { tracer_.resetAccumulation(); }

  private:
    Options opt_;
    World world_;
    Tracer tracer_;
    Dlss dlss_;
    Player player_;
    Falcor::ref<Falcor::FullScreenPass> crosshair_;
    DayNight clock_;  // owns the sun; sunAz_/sunEl_ are its output
    bool placedTwice_ = false;
    std::vector<Solid> solids_;  // decor near the player, regathered each tick

    Vec3 pos_{0, 2, 0};  // the EYE, derived from the player every frame
    float yaw_ = 0.0f, pitch_ = 0.0f, fov_ = 50.0f;
    float sunAz_ = 38.0f, sunEl_ = 24.0f;
    // Last phase uploaded, so applySun can tell when the moon has moved on.
    float moonPh_ = -1.0f;

    bool looking_ = false;   // cursor captured, mouse turns the camera
    bool holdLook_ = false;  // ...because the right button is held
    bool quitArmed_ = false;
    bool moving_ = false;
    bool menuOpen_ = false;
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
    Cuda cuda_;
    Clusters clusters_;
    Physics physics_;
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

    // The same clock the recorder is driven on, read from the GUI pass, which
    // is not handed the frame's `now`.
    static double nowSeconds() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
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
    // Opening the menu hands the mouse back, and closing it takes it again if
    // it had it. A menu you can see but not point at is worse than no menu.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHERE A TAKE OR A SCREENSHOT GOES: the recordings folder, not the
    // working directory.
    //
    // v7.bat deliberately runs the exe from the repo root so that output lands
    // "next to the launcher where it can be found", and for one screenshot that
    // was right. It stops being right the moment the recorder exists: a
    // afternoon of takes and shots buries the repo root in v7_take_004.mp4 and
    // has to be swept up by hand. recordings/ already existed for exactly this
    // sort of thing.
    //
    // AND IT NEVER OVERWRITES. The counters start at zero every run, so before
    // this a second session quietly wrote over the first one's v7_take_000.mp4
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
            std::printf("v7: recording stopped -- encoding\n");
            std::fflush(stdout);
            return;
        }
        // Still writing the last one. Refuse rather than queue: two sink
        // writers and two encoder threads for one hardware encoder is a way to
        // make both takes worse.
        if (recorder_.busy()) {
            std::printf("v7: still finishing the last take\n");
            std::fflush(stdout);
            return;
        }

        if (tracer_.displayWidth() <= 0) return;

        const std::string take = outputPath("v7_take_%03d.mp4", &takeIndex_);
        const char *name = take.c_str();
        const double nowSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (!recorder_.start(name, tracer_.displayWidth(), tracer_.displayHeight(),
                             opt_.recMaxWidth, opt_.recFps, 1, nowSec)) {
            std::fprintf(stderr, "v7: could not start recording\n");
            return;
        }
        std::printf("v7: recording to %s -- %dx%d @ %d fps (R again to stop)\n", name,
                    recorder_.captureWidth(), recorder_.captureHeight(), opt_.recFps);
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // A take that has finished encoding.
    //
    // THE FILE IS THE WHOLE PRODUCT. It is already written, already named on
    // stdout by the recorder, and sitting next to v7.bat where anything else
    // can pick it up -- so there is nothing for an editor to be the gateway
    // to. Stopping a take costs one keystroke and takes nothing away: no
    // panel to dismiss, no cursor handed back and forth, no camera parked
    // while a modal window is up. All that is left is to say it happened.
    // -----------------------------------------------------------------------
    void onTakeSaved(const vb::Take &take, double nowSec) {
        savedTake_ = take;
        savedAt_ = nowSec;
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
        // NOT scaled by the field of view, deliberately. A narrow field does
        // make the same wrist movement cover more of the frame -- that is what
        // a narrow field IS, and it is the reason a scope is harder to aim with
        // than iron sights. Compensating for it would defeat the one thing the
        // setting is good for, which is looking closely at something without
        // also having to hold still.
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
            std::fprintf(stderr, "v7: could not write %s\n", opt_.out.c_str());
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
            // SIXTEEN, UP FROM EIGHT. The original was deliberately thin, and
            // the reasoning still holds -- ImGui centres a slider's value in its
            // trough and draws the grab wherever the value sits, so a wide grab
            // spends part of its travel parked on the number it is there to set.
            //
            // What changed is the trough. The sliders are three times longer
            // now, so the grab covers a third of the fraction of the row it used
            // to, and the number is legible past it again. A thicker handle on a
            // longer bar is easier to catch with the mouse and no harder to read
            // around, which was the only thing the thin one was buying.
            st.GrabMinSize = 16.0f;
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
        crosshair_ = Falcor::FullScreenPass::create(getDevice(), "v7/shaders/Crosshair.ps.slang");
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
                // "probes" and "fog" were being COLLECTED and then dropped here, so
                // every profile this engine has ever printed was silent about the
                // DDGI probe trace and about the fog volume.
                if (leaf != "probes" && leaf != "trace" && leaf != "fog" &&
                    leaf != "reconstruct" && leaf != "tonemap")
                    continue;
                line += fmt("   %s %.2f ms", leaf.c_str(), e->getGpuTimeAverage());
            }
            if (!line.empty()) std::printf("  gpu      %s\n", line.c_str());
        }

        const World::Profile w = world_.profile();
        std::printf(
            "\nv7 profile -- %zu frames at %dx%d -> %dx%d, %s\n"
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
            "  arrow keys            scrub time (up/down = fast)\n"
            "  X + scroll wheel      day/night speed -- scroll down past 0.25x to REWIND\n"
            "  Y                     SETTINGS MENU\n"
            "  R                     RECORD -- press again to stop and save\n"
            "  - / =                 exposure down / up\n"
            "  [ / ]                 bounces down / up\n"
            "  P                     screenshot            F1   this help\n"
            "  ESC                   release the mouse; ESC again quits\n\n");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // Write the live settings back out as src/core/defaults.h.
    //
    // The path comes from V7_SOURCE_DIR, baked in by the build, rather than
    // being derived from the working directory -- the launcher runs the exe
    // from C:\voxelbit, so anything relative would land in the wrong tree and
    // report success while writing nothing anyone would ever compile.
    // -----------------------------------------------------------------------
    std::string bakeDefaults() {
#ifndef V7_SOURCE_DIR
        return "bake unavailable: built without V7_SOURCE_DIR";
#else
        // V7_SOURCE_DIR is "<engine>/src", so the engine root -- and the
        // rebuild script the user is about to be told to run -- is one level up.
        // Derived rather than hardcoded so a copy of this tree elsewhere still
        // reports its own path.
        const std::string srcDir = V7_SOURCE_DIR;
        const std::string root =
            srcDir.size() > 4 ? srcDir.substr(0, srcDir.size() - 4) : srcDir;
        const std::string path = srcDir + "/core/defaults.h";
        char clockText[16];
        clock_.clock(clockText, sizeof(clockText));
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return "could not write " + path;

        std::fprintf(f,
            "// ---------------------------------------------------------------------------\n"
            "// defaults.h -- the settings v7 starts with.\n"
            "//\n"
            "// GENERATED FILE. Everything below is rewritten wholesale by \"Bake as\n"
            "// default\" in the in-viewer settings menu (Y), so hand edits survive only\n"
            "// until the next bake -- but hand edits are perfectly fine, the format is just\n"
            "// constants and the file is checked in.\n"
            "//\n"
            "// The point of it is that the settings menu and the command line stop being\n"
            "// separate universes: fly around, tune the picture until it looks right, bake,\n"
            "// rebuild, and the thing you tuned is what v7 opens with.\n"
            "// ---------------------------------------------------------------------------\n"
            "#pragma once\n"
            "\n"
            "namespace v7 {\n"
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
            "constexpr bool kAtmosphere = %s;\n"
            "constexpr bool kBlueNoise = %s;\n"
            "constexpr bool kAutoExposure = %s;\n"
            "constexpr float kBloom = %.2ff;\n"
            "\n"
            "}  // namespace defaults\n"
            "}  // namespace v7\n",
            opt_.scale, opt_.r.maxDepth, opt_.movingDepth, opt_.r.exposure, opt_.r.shadowLift,
            player_.walk,
            opt_.sensitivity, player_.eye, fov_,
            // THE BASE, NOT THE DERIVED AZIMUTH. kSunAz is loaded straight
            // into clock_.azimuthBase, but this used to bake sunAz_, which is
            // what azimuthDeg() computed for the CURRENT hour -- so every bake
            // folded that hour's offset into the base and the sun walked east
            // a little further each time. Baking at 23:50 on 2026-09-06 moved
            // it from 6.9 to 95.7 in one go.
            clock_.azimuthBase, sunEl_,
            int(getTargetFbo()->getWidth()),
            int(getTargetFbo()->getHeight()), defaults::kTrees, clock_.tday, clockText,
            clock_.cycleSpeed, atmo_.enabled ? "true" : "false",
            // BAKED FROM THE LIVE OBJECTS, not from opt_. The menu writes
            // straight to tracer_ and post(), so opt_ still holds whatever the
            // command line said at start-up -- baking that would quietly
            // discard the thing just tuned, which is the one job this has.
            tracer_.blueNoise ? "true" : "false",
            tracer_.post().autoExposure ? "true" : "false", tracer_.post().bloom);
        std::fclose(f);
        // The FULL PATH, not just the file name. "run rebuild.bat" is only
        // useful if you already know which of the engine trees it lives in,
        // and the exe is launched from the repo root rather than from beside
        // its own source -- so the obvious place to look is the wrong one.
        return "baked. now run " + root + "/rebuild.bat";
#endif
    }
};

}  // namespace v7
