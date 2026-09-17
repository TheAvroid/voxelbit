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
// carries between them. ESC IS A LADDER OF THREE: the first press gives the
// mouse back, the second steps into the pause room, the third switches the
// picture off and leaves. Each rung is a smaller commitment than the one above
// it, and every one of them is reachable from the one before -- see the note at
// the key itself.
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

#include <chrono>
#include <thread>
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
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
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
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include "core/defaults.h"
#include "gpu/neural.h"
#include "gpu/streamline.h"
#include "gpu/nrc.h"
#include "gpu/nrcsdk.h"
#include "gpu/atmosphere.h"
#include "gpu/volfog.h"
#include "gpu/cuda.h"
#include "gpu/clusters.h"
#include "physics/physics.h"
#include "gpu/tracer.h"
#include "gpu/world.h"
#include "render/audio.h"
#include "render/camera.h"
#include "render/arrows.h"
#include "render/bees.h"
#include "render/critters.h"
#include "render/particles.h"
#include "render/lifehit.h"
#include "render/birds.h"
#include "render/butterflies.h"
#include "render/drops.h"
#include "render/helditem.h"
#include "render/holotext.h"
#include "render/birdflock.h"
#include "render/bunnies.h"
#include "render/assetedit.h"
#include "render/lake.h"
#include "render/toolsound.h"
#include "render/player.h"
#include "render/recorder.h"
#include "scene/daynight.h"
#include "scene/palplate.h"

namespace v2 {

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
    // --hitch: record every frame's main-thread cost. See printHitch.
    bool hitch = false;

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
    // WHICH Ray Reconstruction MODEL, as the NGX preset hint. Pinned rather
    // than left to the driver -- see the note in Dlss::resize for the
    // measurement. 0 hands the choice back to whatever the driver ships.
    int rrPreset = kDlssPresetDefault;

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

    // THE OLD SHaRC KEY, KEPT FOR COMPARISON ONLY. v2 files cache entries under
    // an exact voxel face; this puts the SDK's distance-quantised hash grid
    // back. It is a startup flag rather than a menu toggle because the key is a
    // shader define -- the two builds of Trace.cs.slang cannot both be live.
    // See the long note at the top of the voxel-key section in Sharc.slang.
    bool sharcHashGrid = false;
    // Count what the cache is actually doing, and how long it remembers.
    bool sharcStats = false;
    int sharcStale = 0;   // 0 leaves gpu/sharc.h's own default alone
    int sharcEntries = 0; // cache entries; 0 leaves the default 2^21

    // ReSTIR GI. Off by default: it trades a path tracer's white noise for
    // correlated, lower-variance noise, which is better under a denoiser and
    // worse in a still accumulating frame, so it is asked for rather than
    // assumed.
    bool restir = false;
    // The NRC's OLD frequency encoding, for comparison against the voxel-native
    // one. See the encoding note in shaders/Nrc.slang.
    bool nrcFreqEncoding = false;
    // Stop training: the network answers from the weights it was given.
    bool nrcFrozen = false;
    // NVIDIA's Neural Radiance Cache (RTXGI 2.x) instead of v2's own. Runs on
    // D3D12, where the denoiser is -- see gpu/nrcsdk.h.
    bool nrcSdk = false;
    // An NrcResolveMode to visualise instead of rendering. 11 = DirectCacheView.
    int nrcSdkDebug = 0;
    float nrcSdkRadiance = 1.0f;
    // Use the SDK's own Resolve instead of v2's custom one.
    bool nrcSdkBuiltin = false;
    // 0 leaves gpu/nrc.h's own default alone.
    float nrcLr = 0.0f;
    // Weights in and out. The whole point of the voxel encoding is that these
    // are worth keeping between runs and between worlds.
    std::string nrcLoad, nrcSave;
    // ...and the world-space reservoirs inside it, on whenever ReSTIR is, with
    // this to take them away for comparison. See shaders/RestirWorld.slang.
    bool noRestirWorld = false;
    // How much confidence a world reservoir may bring into a disoccluded
    // pixel -- restir.h worldMaxM. Exposed to sweep it.
    int restirWorldM = 16;


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
    // Opens the water panel on the first frame, so --shot-ui can photograph it
    // with no window and no keystroke. Same trick menuAtStart is for.
    bool waterPanelAtStart = false;
    bool roomAtStart = false;   // --room: put the pause buttons up once spawned
    // Everything but the world reflection -- see kWFDefault in Shared.slang,
    // which is where this number is explained. Kept as a literal because
    // app.h does not include the shader header.
    uint32_t waterFlags = 0x1FFu;  // see kWF* in Shared.slang
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
    // PIN THE WHOLE WORLD TO ONE WOOD. Without either of these the biomes are
    // bands you walk between and /locate takes you to one; with one of them the
    // world is that wood everywhere, which is what a reproducible screenshot or
    // a profile run wants. See Biome and birchWeight in scene/voxelworld.h.
    bool birch = false;
    bool pineOnly = false;
    // --oak: pin the world to the oak wood, as --pine and --birch do for
    // theirs. Worth more here than for the other two: the band tiling
    // moved when the oak was inserted, so a coordinate is no longer a
    // reliable way to name a wood and this is.
    bool oakOnly = false;
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
    float treeDensity = 0.3210f;
    float grass = 0.105f, flowers = 0.45f, rocks = 0.010f;
    int grassMin = 3, grassMax = 6;
    std::string pines = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decor = "C:/voxelbit/game/assets/decoration";

    // ---- the face every letter in the engine is drawn in -----------------
    //
    // THE GAME'S OWN PIXEL FONT, out of the same asset tree as the models
    // above and for the same reason. v1 drew its readout, its hints and its
    // title in this face (`font-family: px3`) while the engine drew everything
    // in Consolas, and two halves of one game do not get to disagree about
    // what text looks like.
    //
    // "off" -- or nothing -- gives the framework's Consolas back. A pixel font
    // is a strong look and this is the way out of it that does not need a
    // rebuild; a path that cannot be opened takes the same way out rather than
    // taking the interface down with it.
    std::string font = "C:/voxelbit/game/3x3-pixel.otf";

    // ---- the wood's ambience (render/audio.h) --------------------------
    //
    // IN THE GAME'S SOUND FOLDER, not one of v2's own, for the same reason
    // `pines` and `decor` point into its asset tree: there is one set of
    // assets for this project and two engines that read them. v2 owns no
    // assets at all, and this is not the feature to start it owning some.
    //
    // `ambience` is a MASTER gain, not a level -- what actually reaches the
    // voice is this times the canopy closure at the listener's feet. 1.0 is
    // the bed at the level it was baked (peak -3 dBFS, mean -23.6), which is
    // a background at a normal system volume rather than a foreground.
    //
    // A QUARTER OF THE BAKED LEVEL. This was 0.75 on the argument that the
    // birds sat too far forward at unity; a quarter is the same argument
    // carried to where it actually lands. It is a LINEAR AMPLITUDE, so 0.25
    // is -12 dB rather than "a quarter as loud" -- roughly half the perceived
    // loudness of the old default, which is the difference between a bed you
    // notice and one you only miss when it stops.
    //
    // The compressor in render/dynamics.h is upstream of this and unaffected:
    // it decides the SHAPE of the bed, this decides how much of it you get.
    // --ambience 1.0 still restores the baked level.
    //
    // FROM THE BAKE NOW, like every other setting with a row in the menu.
    // The quarter used to be written here and nowhere else, so the one
    // control that changes it -- the Volume slider -- was the only one in the
    // Y menu whose value a bake silently threw away: tune the wood down, bake,
    // rebuild, and it came back at whatever this line said. defaults.h is
    // where a tuned setting belongs, and the volume is a tuned setting.
    std::string sound = "C:/voxelbit/game/sound/bird_ambience.mp3";
    float ambience = defaults::kAmbience;
    bool soundOn = true;
    // ---- what the tools sound like (render/toolsound.h) ------------------
    //
    // The DIRECTORY, not a file: it holds a dozen cues in three subfolders and
    // naming them one by one on a command line would be absurd. In the game's
    // asset tree with the models and the bed, for the same reason they are.
    //
    // `sfx` is the JS engine's SFX BUS -- a second slider under the master, for
    // the sounds the world makes. One number here where it has four, because
    // v2 has two things that make sound rather than five.
    std::string soundDir = "C:/voxelbit/game/sound";
    float sfx = 1.0f;

    // ---- how much the hand moves (render/helditem.h) --------------------
    //
    // A GAIN over the stride and the breath. The constants in helditem.h are
    // the 1.00 look; this is how much of it you get, and it is 2.00.
    //
    // COMMAND LINE AND BAKE, with no row in the Y menu. It had one for a few
    // minutes on 2026-09-08 and the answer it produced was "2.00, and take the
    // slider away" -- which is the same shape kBloom and kAutoExposure have,
    // and the menu is shorter for every setting that has stopped being a
    // question.
    float handSway = defaults::kHandSway;

    // ---- what is in the player's hand (render/helditem.h) ---------------
    //
    // IN THE GAME'S ASSET TREE, for the same reason `pines`, `decor` and the
    // ambience bed are: there is one set of assets for this project and two
    // engines that read them, and v2 owns none of its own.
    //
    // A path rather than a flag, so a second tool is a command line away
    // without anything here changing. --no-axe opens with an empty hand.
    std::string axe = "C:/voxelbit/game/assets/stone_tools/stone_axe.vox";
    std::string pick = "C:/voxelbit/game/assets/stone_tools/stone_pick.vox";
    std::string shovel = "C:/voxelbit/game/assets/stone_tools/stone_shovel.vox";
    std::string hoe = "C:/voxelbit/game/assets/stone_tools/stone_hoe.vox";
    std::string bow = "C:/voxelbit/game/assets/stone_tools/bow_arrow/bow/base.vox";
    std::string arrow = "C:/voxelbit/game/assets/stone_tools/bow_arrow/arrow.vox";
    bool axeOn = true;

    // ---- what is flying through it (render/butterflies.h) ----------------
    //
    // In the game's asset tree with everything else, and a COUNT rather than a
    // switch: nought is the empty wood, and the ceiling is kFlyerInstances --
    // how many slots the acceleration structure reserves. Twenty-four over the
    // eighty-metre disc the flock lives in works out at one butterfly per
    // twenty-odd metres of wood, which is a few in view at a time in the open
    // and one at a time under a canopy.
    std::string butterflyDir = "C:/voxelbit/game/assets/life/butterfly";
    // The three species' folders sit under this one -- see Birds::init.
    std::string birdDir = "C:/voxelbit/game/assets/life";
    // TWELVE, HALVED FROM 24 (user 2026-09-14: "reduce the butterflies in
    // half"). The band still reserves 64 -- a reservation is not a population,
    // and shrinking it would cost a structure rebuild to change your mind. This
    // is the number flock_.wanted is set from at load; --butterflies overrides
    // it, and 0 switches them off entirely.
    int butterflies = 12;

    // WHERE THE NOCKED ARROW STARTS, in whole voxels -- the same three numbers
    // the settings panel edits, and applied through the same path, so a value
    // tuned by hand can be pinned on a command line and photographed without
    // being baked into kArrowPos first.
    //
    // NOT `arrow`, which is a few lines up and is the arrow MODEL's path. Two
    // fields called the same thing in one struct is the sort of collision that
    // compiles the day the second one is a different type.
    ArrowOffset arrowNudge;
    // Which tool the hand opens with, as an index into the kit. Exists for the
    // same reason --swing-hold does: a tool you can only reach by scrolling
    // cannot be photographed, measured or regression tested.
    int tool = 0;
    // Print what every swing ran into. Off by default -- it is a line per blow
    // and the blows repeat while the button is held -- but it is the only way
    // to see the reach and the aim without a bite to look at.
    bool swingLog = false;
    // Fell a tree headlessly and print the body's trajectory -- see runFellTest.
    bool fellTest = false;
    // Dig the ground out from under a tree with no window, and report whether
    // it came down -- see runFloatTest.
    bool floatTest = false;
    // ...and the shovel's, which needs no window either. See runDigTest.
    bool digTest = false;
    // Survey every row of the /locate life table and check where it lands you
    // -- see runLocateTest. No window either; its whole output is stdout.
    bool locateTest = false;
    bool clipTest = false;
    bool wheatTest = false;
    bool hoeTest = false;
    bool shaftTest = false;
    bool killTest = false;
    // --duck-test: does a brood outlive its mother? See runDuckTest.
    bool duckTest = false;
    // --lbug-test: does the ladybug double its rate near the player?
    bool lbugTest = false;
    bool soilTest = false;
    // -- WRITE THE LIVE PALETTE OUT AS A MAGICAVOXEL PLATE, THEN EXIT -------
    //
    // The table is built at boot and has never existed as a file, so authoring
    // a new asset has meant guessing at what colours -- and therefore what
    // MATERIALS -- are already in it. See scene/palplate.h for the whole of
    // why, and runPaletteVox for where in the load order it is read.
    bool paletteVox = false;
    // AND NOT OVER game/assets/palette.vox. That file is v1's table, written
    // by tools/palette_vox.py out of a browser and regenerated by
    // tools/hooks/pre-commit whenever a v1 asset moves. The two engines hold
    // DIFFERENT tables that happen to be read the same way, and overwriting
    // one with the other would be silently wrong in both directions.
    std::string paletteVoxOut = "C:/voxelbit/game/assets/palette_v2.vox";
    // WHAT A BROKEN WHEAT PLANT PAYS OUT. Files rather than ids, like every
    // other model this engine loads, so an artist can swap them without a
    // build -- see --axe and its siblings.
    std::string wheat = "C:/voxelbit/game/assets/decoration/wheat.vox";
    std::string seeds = "C:/voxelbit/game/assets/decoration/seeds.vox";
    // ...AND THE RAW MEAT A KILL LEAVES BEHIND (user 2026-09-14: "have
    // life drop a raw steak"). v1's own file, at v1's own path -- its
    // note calls it "RAW MEAT, which a killed land mammal leaves behind".
    // base.vox is the source; the numbered frames beside it are that
    // engine's eat animation, which v2 has nothing to do with.
    std::string steak = "C:/voxelbit/game/assets/food/meat/steak/base.vox";
    // Hold the swing from the first frame, exactly as --shot-walk holds W. It
    // exists for the same reason that one does: an animation you can only see
    // by holding a mouse button cannot be photographed, measured or regression
    // tested, and picking a --shot-frame off the 570 ms curve is how you look
    // at one phase of it. 34 frames is a whole swing at the default 1/60 dt.
    bool swingHold = false;
    // Hold the DRAW from the first frame, as --swing-hold holds the swing and
    // --shot-walk holds W. Releasing it is what looses an arrow, so a scripted
    // shot draws for --shot-frame frames and never fires; use --shot-loose N to
    // let go on frame N.
    bool drawHold = false;
    // DROP THE HELD ITEM ON A NAMED FRAME, the same idea as --shot-loose above
    // it and for the same reason: a still of an item leaving the hand, or of
    // one lying in the wood, has to be reproducible. Negative never drops.
    int dropFrame = -1;
    // ...AND A SCRIPTED BURST, for the same reason and on the same clock:
    // a shader change cannot be type-checked and an emissive material is
    // exactly the kind that fails SILENTLY -- see the private-material
    // note in render/particles.h. --spark-frame N --shot out.png renders
    // one with the sparks in the air.
    int sparkFrame = -1;
    // -- THE SPARK'S EMISSION, ON THE COMMAND LINE -----------------------
    //
    // A COLOUR THAT HAS TO SURVIVE A TONE CURVE CANNOT BE CHOSEN ON PAPER.
    // The first value looked like a warm amber written down and rendered as
    // pure white, because acesFilmic is per channel and every channel was past
    // its shoulder -- see kSparkEmit. Finding the ratio that comes out orange
    // is a sweep, and a sweep that costs a four-minute BUILD per guess is a
    // sweep nobody finishes. Negative means "use the compiled value".
    float sparkR = -1.0f, sparkG = -1.0f, sparkB = -1.0f;
    // ...and the smoke's, for the same sweep. Negative keeps the compiled one.
    float smokeIor = -1.0f;
    // THE FOUR SPARKS ALONE, for --spark-frame. The smoke is white and opaque
    // now, so a render of the whole burst has no way to tell a spark pixel from
    // a smoke one -- not by brightness and not by hue once the spark is pale.
    // Measuring a colour against a picture that contains two white things is
    // how the last sweep came back saying a lighter yellow was darker.
    bool sparkOnly = false;
    // --tear-only: the death burst frame throws TEARS instead. See above.
    bool tearOnly = false;
    // ...AND THE SAME FOR THE HIT FLASH, which is the other half of this
    // feature that only a picture can check: V6Instance::hurt is a new field
    // and a new branch in the shader, and Slang compiles at runtime, so the
    // C++ build has never seen either. --hurt-frame N walks up to the nearest
    // animal, aims at it and lights it; run it against -1 and diff.
    int hurtFrame = -1;
    // ...AND THE OTHER HALF OF THE A/B. The walk-up and the aim happen either
    // way; only the flash itself is switched, so the two pictures are of the
    // same animal from the same place and every pixel that differs differs
    // because of the one field being tested. Without this the 'off' run stood
    // somewhere else entirely and the diff was the whole frame.
    bool hurtDim = false;
    // Open straight into the asset editor -- what U does, without the key.
    bool stageAtStart = false;
    // ...and straight into the building level, which is what O does. Same
    // reason --stage exists: a shot of the level has to be a shot of the place
    // the key opens, not of a second arrangement that can drift from it -- and
    // it is the only way to check the level headlessly, since a key press is
    // not something --shot-frame can make.
    bool levelAtStart = false;
    // ...AND WITH THE SUBJECT ALREADY PICKED, which is what the mouse does.
    // 0 none, 1 the move arrows, 2 the rotation rings. It exists for the same
    // reason --stage does, and the note there gives it: a shot of the deck has
    // to be a shot of the thing the key opens. The handles cannot be reached
    // from a script any other way -- selecting is a click, and a click needs a
    // hand -- so without this the one part of the editor that is pure geometry
    // is the one part no capture can check.
    int gizmoAtStart = 0;
    int shotLoose = -1;

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

    // HOW DARK THE NIGHT IS, as one number, and it is a MULTIPLIER over the two
    // things that light a wood after dark rather than a third light of its own:
    // the moon's key (sky.h moonKeyScale) and the airglow floor above it.
    //
    // BOTH, because either alone is a control that lies. The moon is worth tens
    // of times the floor while it is up and near nothing when it is new, so a
    // slider that moved only one of them would do nothing at all on half the
    // nights of the month. 1.0 is the night this engine has always rendered.
    float nightBrightness = defaults::kNightBrightness;

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


    // THERE IS NO POINTER ACCELERATION AND NO VIEW WEIGHT (user, 2026-09-06:
    // "remove mouse accel, remove mouse weight from the game"). Both existed,
    // both were sliders, and both are gone rather than defaulted to zero: a
    // setting whose only correct value is off is a row in a menu that can only
    // make the game worse, and the code behind them -- a speed estimate from
    // the event clock, a buffer of owed degrees and an exponential drain --
    // could not be left in place without still being the thing that turns the
    // camera. `sensitivity` is now the whole of the mouse. See applyMouseLook.
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
// #ffd76a, AND IT IS ONE GOLD RATHER THAN TWO. Lifted from the JS engine's
// style-console.css, where the same value dresses the copyright watermark, the
// menu button hover and the button labels -- and its note says why it is not a
// fresh number: "two golds a few hex apart read as a mistake rather than a
// choice". The frame-rate readout and the watermark share it here for exactly
// that reason. The alpha is the caller's; the watermark asks for 0.64, which
// is that engine's opacity for it, and the readout takes it whole.
inline ImVec4 kGold(float a = 1.0f) { return rgb(255, 215, 106, a); }
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
        // THE BIOME IS SET BEFORE ANYTHING READS THE TERRAIN, and that
        // ordering is load-bearing: loadPines() asks terrain.birch() to decide
        // which species to load, and the chunk mesher is handed a COPY of the
        // terrain when it starts its workers. Set it late and half the engine
        // has already been told it is a pine wood.
        world_.terrain.forced = opt_.birch || opt_.pineOnly || opt_.oakOnly;
        world_.terrain.biome = opt_.birch    ? Biome::Birch
                               : opt_.oakOnly ? Biome::Oak
                                              : Biome::Pine;
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
        if (!opt_.outGiven && !opt_.camGiven) chooseSpawn();
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
        std::printf("           %zu trees, %zu rocks, %zu flowers, %zu mushrooms,"
                    " %zu pinecones, %zu beehives\n",
                    world_.decorCount(0), world_.decorCount(1), world_.decorCount(2),
                    world_.decorCount(3), world_.decorCount(4), world_.decorCount(5));
        // THE RING'S CENTRE, not pos_ -- the player is placed further down and
        // pos_ is still the origin here, which printed "0, 0" from wherever you
        // actually were. opt_ is what the world was built around.
        std::printf("           the %s wood at %.0f, %.0f%s\n",
                    world_.terrain.woodName(opt_.camX), opt_.camX,
                    opt_.camZ, world_.terrain.forced ? " (pinned)" : " -- T, /locate");
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
                {opt_.axe, opt_.pick, opt_.shovel, opt_.arrow, opt_.steak, opt_.seeds},
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
            critters_.load(world_, opt_.birdDir);
            // ...AND THE PARTICLES, LAST OF THE FOUR AND DELIBERATELY SO. They
            // want three PRIVATE materials and a private material is one with
            // nothing near it, so asking after everything else has registered
            // is asking the question in its hardest form -- which is the only
            // form whose answer stays true. See the note over Particles::load.
            if (opt_.smokeIor > 0.0f) particles_.setSmokeIor(opt_.smokeIor);
            particles_.load(world_, &tracer_);
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
        for (int b = 0; b < 9; ++b) waterTerm_[b] = (opt_.waterFlags >> b) & 1u;
        tracer_.waterFlags = opt_.waterFlags;

        if (opt_.outGiven) {
            tracer_.setDemodulate(opt_.demodulate);
            // THE OFFLINE PATH SETS THE WATER ITSELF, because it never runs
            // onFrameRender -- see the note over renderOffline. Without this
            // every --out render had waterY at its "no water anywhere" default
            // and waterTime at zero: a submerged camera got no absorption and
            // the waves stood still. Every verification render taken before
            // this was quietly lying about both.
            tracer_.waterY = world_.terrain.waterAt(pos_.x);
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
        // THE BUILDING, FROM THE COMMAND LINE -- the same path [O] takes, for
        // the reason the line above it does.
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
            else std::fprintf(stderr, "v2: --level: no building.vox to travel to\n");
        }

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
                         HeldPose{12.000f, -1.320f, 6.990f, 0.010f, 1.570f, -0.060f, 1.234f});
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
            if (!held_.add(world_, "steak", opt_.steak,
                           HeldPose{8.799f, -1.624f, 8.730f, 0.050f, 0.680f, 1.951f, 1.000f},
                           Takes::Nothing, HeldItem::kSteakMergeTol))
                steakTool_ = -1;
            if (wheatTool_ >= 0) held_.stow(wheatTool_);
            if (seedsTool_ >= 0) held_.stow(seedsTool_);
            if (steakTool_ >= 0) held_.stow(steakTool_);

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
            // WHERE THE NOCKED ARROW STARTS. Asked for rather than applied:
            // the request is served on the first frame, beside the streamer,
            // which is the one place in this engine a structure may be built --
            // see the note there, and the one on the settings rows.
            arrowWant_ = opt_.arrowNudge;
            arrowDirty_ = (opt_.arrowNudge.across || opt_.arrowNudge.along ||
                           opt_.arrowNudge.up);
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

    void onFrameRender(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) override {
        const auto hitchT0 = std::chrono::steady_clock::now();
        hPhys_ = hLife_ = hPub_ = 0.0;
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
            std::printf("v2: wrote %s at %ux%u (window, with the interface)\n",
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

        // THE SCREEN SWITCHING OFF IS NOT PART OF THE SIMULATION, so it is
        // stepped here on the WALL clock rather than in processInput on the
        // scaled one -- a slowed or scripted game should still take exactly as
        // long to go dark.
        tickQuit(wallDt);

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
        const auto streamT0 = std::chrono::steady_clock::now();
        if (world_.update(streamAt)) tracer_.resetAccumulation();
        const double streamMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - streamT0)
                .count();

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

        // A SCRIPTED FLASH, on its named frame. It sets the flash DIRECTLY
        // rather than swinging: the kill path is covered by --kill-test, and
        // what is unproven here is the shader -- so this drives the one field
        // that shader reads and nothing else, which is what makes a difference
        // in the picture mean what it says.
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
            std::printf("v2: spark test -- burst at (%.2f, %.2f, %.2f), %d live\n", sat.x, sat.y,
                        sat.z, particles_.live());
            std::fflush(stdout);
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
            tracer_.waterY = world_.terrain.waterAt(pos_.x);
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
            drops_.update(dt, walkWorld(), player_.pos, player_.eyePosition());
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
                std::printf("v2: picked up %s\n", what);
                std::fflush(stdout);
            }
        }

        // Everything that has come off the static world: the solver, and the
        // pieces it is carrying. Before the arrows only because both want the
        // same frame's dt and this one also owns the clock they are timed on.
        stepLoose(dt);

        // The shafts in the air. AFTER processInput, so one loosed this frame
        // starts moving on the frame it left rather than the next.
        // -- A SHAFT NEEDS A WIDER WORLD THAN A PAIR OF FEET --------------
        //
        // See kArrowSolidsM. Only while something is actually in flight: the
        // gather walks every resident chunk's solids, and paying that on every
        // frame of a walk in the woods to serve an arrow nobody has loosed
        // would be the wrong trade entirely.
        if (arrows_.inFlight() > 0) {
            const WalkWorld aw = wideWalkWorld(kArrowSolidsM);
            arrows_.update(dt, aw, [this](const Vec3 &p) { return arrowKill(p); });
        } else {
            arrows_.update(dt, walkWorld(), [this](const Vec3 &p) { return arrowKill(p); });
        }
        // ...and each one that stopped this tick lands with a thud, quieter the
        // further off it stuck. Drained here rather than inside the flight so
        // the sound is not fired from the 5 ms integration substep loop.
        for (const Vec3 &at : arrows_.landedThisTick())
            toolSfx_.arrowLanded(length(at - pos_));
        // ...AND EACH ONE TAKES A CHIP OUT OF WHAT IT HIT. Drained here for the
        // same reason the thud is: a carve re-meshes a chunk and must not run
        // from inside the 5 ms flight loop. See arrowChip.
        for (const Arrows::Impact &im : arrows_.impactsThisTick()) arrowChip(im.at, im.dir);

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
        hStart();
        flock_.update(dt, world_, player_.pos);
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
        birds_.update(dt, perches_, player_.pos);
        // WHAT LIVES ON THE WATER. Ticked beside the birds because it is the
        // same kind of thing -- a small population that follows the player --
        // and published in the same window, so the flyer band is written once.
        lake_.update(dt, world_.terrain, player_.pos, forward());
        // ...AND ANY DUCKLING THAT WEPT THIS TICK GETS ITS DROPLET. The lake
        // queues world points rather than reaching into the particle pool --
        // see LakeLife::cryTick -- so this is the one line that joins them, and
        // it is the same shape as the arrow's landed/impact drains above.
        for (const Vec3 &t : lake_.tearsThisTick()) particles_.tear(t, simMs_);
        lake_.publish(world_);
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
            bunnies_.update(dt, player_.pos,
                            [this](float x, float z) {
                                return float(world_.terrain.heightVox(
                                                 int(std::floor(x / VOXEL_M)),
                                                 int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
                            },
                            // IS THIS COLUMN UNDER WATER -- the same three calls
                            // WaterField::rebuild makes, which is this engine's
                            // one definition of wet. A bunny and a lily pad
                            // therefore agree about where the lake is.
                            [this](float x, float z) { return wetColumnAt(x, z); }, perches_,
                            // WHICH WOOD, for the species that belong to one.
                            // The generator's own band weight, not a threshold:
                            // the fill is the only caller and it makes its own
                            // decision about where the seam is.
                            [this](float x) { return world_.terrain.birchMix(x); },
                            // ...AND NOT ON THE BEACH -- see Bunnies::blocked.
                            [this](float x, float z) { return sandAt(x, z); });
            bunnies_.publish(world_, kBunnySlot0);
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
            bees_.update(dt, player_.pos, hivesNear_, bloomsNear_, &perches_);
            bees_.publish(world_, kBeeSlot0);
            // -- AND THE FOUR SMALL ONES -----------------------------------
            //
            // The same three predicates the marchers take, because they are
            // asking the same three questions: how high is the ground, is it
            // under water, and which wood is this. The LOOK is handed in as
            // well so a critter is not born in the middle of your view -- see
            // BirthGate::mayAt.
            critters_.update(dt, player_.pos,
                             [this](float x, float z) {
                                 return float(world_.terrain.heightVox(
                                                  int(std::floor(x / VOXEL_M)),
                                                  int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
                             },
                             [this](float x, float z) { return wetColumnAt(x, z); },
                             [this](float x) { return world_.terrain.birchMix(x); }, banksNear_,
                             perches_,
                             // AFTER DARK. v1 swaps its flyer band on the sun's
                             // own sign; this is the same test in degrees, a
                             // shade below the horizon so the fireflies come up
                             // as the light goes rather than the instant it
                             // crosses zero.
                             isNight(), forward(),
                             // ...AND THE LAKE'S OWN TOP, which the ground
                             // query cannot give -- see waterTopAt.
                             [this](float x, float z) { return waterTopAt(x, z); });
            critters_.publish(world_, kCritterSlot0);
            // The sparks are on the same clock as everything else in the band.
            // update() only retires what has run out -- a particle's position
            // is a closed form off its birth, so nothing here integrates.
            particles_.update(simMs_);
            particles_.publish(world_, simMs_);
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
            tracer_.glowMtl = critters_.glowMtl() ? uint32_t(critters_.glowMtl()) : 0xFFFFFFFFu;
            tracer_.glowRadiance = Vec3(kGlowNits, kGlowNits * 0.85f, kGlowNits * 0.18f);
        }

        // The bed follows the canopy. Fed the same dt as the walk and the day
        // cycle -- the shot clock when one is running -- so a scripted move
        // and a live one fade identically.
        ambience_.update(dt, forestGain());

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

        Camera cam;
        cam.origin = pos_;
        cam.target = pos_ + forward() * 50.0f;
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
            world_.setHeldInstance(held_.model(), hx.m, hx.tx, hx.ty, hx.tz, hx.show);
            // ...and the tracer is told the same pose, because it is the one
            // thing in this scene whose motion vector cannot be worked out from
            // where it is in the world -- see V6Params::heldPrev0. It keeps its
            // own previous copy and steps it with prevCam_, which is the only
            // way the two can be guaranteed to describe the same frame.
            // ...AND WHICH TOOL IT IS, so a change of hands does not carry the
            // last one's motion vector onto this one. See heldPrevValid.
            tracer_.setHeldXform(hx.m, hx.tx, hx.ty, hx.tz, hx.show, held_.selected());
            hStart();
            arrows_.publish(world_);
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
            const std::string shot = outputPath("v2_shot_%03d.png", &shotIndex_);
            const char *name = shot.c_str();
            if (tracer_.writePng(ctx, name))
                std::printf("v2: wrote %s at %dx%d\n", name, tracer_.displayWidth(),
                            tracer_.displayHeight());
            else
                std::fprintf(stderr, "v2: could not write %s\n", name);
            std::fflush(stdout);
        }

        moving_ = false;  // cleared only once the frame it applied to is drawn

        // -- the scripted capture, if one was asked for -----------------------
        // --profile shares the frame counter, so a run can be measured with or
        // without a png falling out of it.
        // -- ...AND THE FRAME IS BOOKED (see HitchFrame) -------------------
        if (opt_.hitch) {
            const World::Profile wp = world_.profile();
            HitchFrame h;
            h.total = float(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - hitchT0)
                                .count());
            h.stream = float(streamMs);
            h.blas = float(wp.blasMs - hitchWas_.blasMs);
            h.tlas = float(wp.tlasMs - hitchWas_.tlasMs);
            h.pool = float(wp.poolMs - hitchWas_.poolMs);
            h.rering = float(wp.reringMs - hitchWas_.reringMs);
            h.drain = float(wp.drainMs - hitchWas_.drainMs);
            h.take = float(wp.takeMs - hitchWas_.takeMs);
            h.adopted = int(wp.adopted - hitchWas_.adopted);
            h.phys = float(hPhys_);
            h.life = float(hLife_);
            h.pub = float(hPub_);
            hitchWas_ = wp;
            hitch_.push_back(h);
        }
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
                    for (int i = 0; i < kDropSlots; ++i)
                        if (drops_.clearance(i, &clear))
                            std::printf("v2: drop %d clears %.3f m (%.1f voxels)\n", i,
                                        double(clear), double(clear / VOXEL_M));
                }
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

        // THE FACE FIRST, AND NOTHING ELSE ON THE FRAME THAT BAKES IT. The
        // window is what decides the size, so this cannot be done at load
        // time; see bakePx3 for why the frame is then given up.
        if (bakePx3(pGui, fbH)) return;

        // NO TITLE BAR, NO MOVE, NO RESIZE GRIP on either panel. v2 drew its
        // own title and put the panel where it belonged; ImGui's chrome on top
        // of that is a second title over the first and a drag handle for a
        // window that is not meant to be dragged.
        const Gui::WindowFlags kBare = Gui::WindowFlags::AutoResize | Gui::WindowFlags::NoResize;

        // ---- the frame rate ------------------------------------------------
        //
        // NO BOX. The veil is zero, so the window paints nothing and what is on
        // the screen is the number and the number only -- which is what a
        // readout meant to be GLANCED at should be. The panel was buying one
        // thing, contrast, and the shadow below buys it back for two pixels
        // instead of a rectangle: gold over snow or a bright sky is gold you
        // cannot read.
        //
        // TOP LEFT, which is where it began and where it is again (user
        // 2026-09-07). The corner it sits in is the only thing that has moved:
        // it is still the bare number, still gold, still with no panel behind
        // it. Left-aligned it needs no measuring to place -- the corner is the
        // corner -- but the width is still measured, because the recorder's
        // panel has to be told where the readout ends so the two do not stack
        // on top of each other. See `below`.
        // -------------------------------------------------------------------
        // THE WATER PANEL -- `--water-ui`, top right.
        //
        // NO KEY. It was [I], then [O], then [L], and none of them stuck; it is
        // a start-up flag now (user 2026-09-14: "you remove the water panel
        // from l"). See onKeyEvent for the whole of that history.
        //
        // One row per term the water actually does, each a live uniform bit
        // (kWF* in Shared.slang) rather than a rebuild. v1 keeps the same row
        // of switches in its WATER_BAKE and its note is the reason this exists:
        // a panel that cannot speak for every site "is a lie", so every one of
        // these turns off the WHOLE term wherever it is evaluated.
        //
        // TOP RIGHT, AND SET EVERY FRAME. ImGui remembers window positions in
        // an ini between runs, so asking once is not the same as asking.
        // -------------------------------------------------------------------
        if (waterPanelOpen_) {
            styleV2 style(pGui, px3_, 1.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
            Gui::Window ww(pGui, "water##v2", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            // NO KEY IN THE TITLE, because it no longer has one -- this said
            // "[I]" for a day after the panel had moved to [O], which is the
            // argument for not naming a binding in a label at all.
            ww.text("WATER");
            ww.separator();
            static const char *kWaterRows[9] = {
                "sun glint",        // kWFGlint
                "caustics",         // kWFCaustic
                "world reflection", // kWFReflect
                "absorption",       // kWFAbsorb
                "in-scatter",       // kWFScatter
                "sunlight to bed",  // kWFSunPath
                "voxel swell",      // kWFSwell
                "surface ripple",   // kWFRipple
                "shore foam",       // kWFFoam
            };
            uint32_t wf = 0;
            for (int b = 0; b < 9; ++b) {
                ww.checkbox(kWaterRows[b], waterTerm_[b]);
                if (waterTerm_[b]) wf |= (1u << b);
            }
            tracer_.waterFlags = wf;
            ww.separator();
            if (ww.button("all on")) {
                for (int b = 0; b < 9; ++b) waterTerm_[b] = true;
            }
            const ImVec2 wsz = ImGui::GetWindowSize();
            ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - wsz.x - 12.0f), 12.0f));
        }

        // -------------------------------------------------------------------
        // THE STACK BADGE'S FOUR NUMBERS, LIVE.
        //
        // (user 2026-09-14: "let me adjust the display number just like how we
        // were able to in v1.")
        //
        // v1 tunes this from a card with exactly four sliders -- SB_K is
        // ['x', 'y', 'size', 'tilt'] -- and these are the same four. Built on
        // the water panel rather than on the settings menu because it is the
        // same KIND of thing: a handful of numbers you move while looking at
        // what they do, not a preference you set once.
        //
        // IT SHOWS THE COUNT IT IS TUNING and forces one if the hand is empty
        // of a stack, because four sliders that move nothing you can see are
        // four sliders nobody can use. v1 does the same -- "empty hand -> tune
        // the axe, the same fallback the pose card takes".
        //
        // NOT PER ITEM, which v1's is. Its badge is a screen-space blit whose
        // right place depends on how the model happens to project; this one
        // hangs off the model's own measured half-width, so the same four
        // numbers frame every tool in the kit. If one item ever needs its own,
        // the shape of it is v1's sbCfgs table.
        // -------------------------------------------------------------------
        if (stackPanelOpen_) {
            styleV2 style(pGui, px3_, 1.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
            Gui::Window sw(pGui, "stack##v2", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            sw.text("STACK COUNT");
            sw.separator();
            const int sel = held_.ready() ? held_.selected() : -1;
            sw.text(sel >= 0 ? held_.tool(sel).name : "nothing in hand");
            sw.checkbox("show a count while this is open", stackPanelForce_);
            sw.separator();
            // BOUND TO WHAT IS IN THE HAND, resolved every frame -- v1's own
            // rule for its card ("resolved from the hand each time the panel
            // refreshes, exactly as pkIt is"). Scroll to another tool and the
            // sliders are that tool's.
            StackCfg &cfg = stackFor(sel >= 0 ? sel : 0);
            sw.slider("size", cfg.cell, 0.003f, 0.05f, false, "%.4f m");
            sw.slider("across", cfg.across, -0.4f, 0.6f, false, "%.3f m");
            sw.slider("up", cfg.up, -0.3f, 0.4f, false, "%.3f m");
            sw.slider("tilt", cfg.tilt, -1.6f, 1.6f, false, "%.3f rad");
            sw.separator();
            if (sw.button("defaults")) cfg = StackCfg{};
            // -- THE BAKE (user 2026-09-14: "let me bake the stack count number
            //    text into here") ---------------------------------------------
            //
            // WHAT A BAKE IS HERE: the four numbers as the four SOURCE LINES
            // they live on, ready to paste over the declarations in
            // setStackBadge. Not a config file and not a save -- this engine
            // bakes by pasting, which is what the asset editor's [C] does with
            // its strip table and what the settings menu's "copy pose" does
            // with a HeldPose. A tuned number that only exists in a running
            // process is a number that dies with it.
            //
            // TO THE CONSOLE AS WELL, ALWAYS. assetedit.h's note is the reason:
            // OpenClipboard fails outright when another program is holding it,
            // and a copy that silently did nothing is worse than no copy. The
            // console is the copy that cannot fail.
            if (sw.button("bake")) {
                // THE ROW, AS THE LINE THAT DECLARES IT. The table is keyed by
                // the tool's NAME rather than by its slot, because a slot is
                // where a thing happens to sit in the wheel this build and a
                // name is what it is -- the same reason v1 keys sbCfgs by
                // ITEM_NAMES and not by index.
                stackBaked_ = fmt("        { \"%s\", { %.4ff, %.3ff, %.3ff, %.3ff } },",
                                  sel >= 0 ? held_.tool(sel).name : "?", double(cfg.cell),
                                  double(cfg.across), double(cfg.up), double(cfg.tilt));
                std::printf("v2: stack badge bake -- paste into kStackBakes:\n%s\n",
                            stackBaked_.c_str());
                std::fflush(stdout);
                ImGui::SetClipboardText(stackBaked_.c_str());
            }
            sw.text(stackBaked_.empty() ? "tune it, then bake" : "copied -- paste into app.h");
            char line[160];
            std::snprintf(line, sizeof(line), "%.4f  %.3f  %.3f  %.3f", double(cfg.cell),
                          double(cfg.across), double(cfg.up), double(cfg.tilt));
            sw.text(line);
            const ImVec2 ssz = ImGui::GetWindowSize();
            ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - ssz.x - 12.0f),
                                       maxf(0.0f, fbH - ssz.y - 12.0f)));
        }

        // -------------------------------------------------------------------
        // THE ASSET EDITOR'S READOUT -- top right, only on the deck.
        //
        // NOT OPTIONAL AND NOT A TOGGLE, unlike every other panel in this file.
        // Which frame is selected, what it plays, how far it has been nudged
        // and which axis the arrows are pointing at are not settings you turn
        // on to check: they are the tool's ENTIRE state, and a nudge whose
        // effect you cannot read is a nudge you have to count in your head.
        // v1's #edHud is up for the same reason and says the same first line.
        //
        // The key list is under it, out of AssetEdit::help, so the panel and F1
        // cannot describe two different sets of bindings.
        // -------------------------------------------------------------------
        if (edit_.on() && !waterPanelOpen_) {
            std::vector<std::string> rows;
            edit_.hudLines(&rows);
            styleV2 style(pGui, px3_, 1.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
            Gui::Window ew(pGui, "asset editor##v2", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            ew.text("ASSET EDITOR  [I]");
            ew.separator();
            for (const std::string &r : rows) ew.text(r.c_str());
            ew.separator();
            int nh = 0;
            const char *const *hr = AssetEdit::help(&nh);
            for (int i = 0; i < nh; ++i) ew.text(hr[i]);
            const ImVec2 esz = ImGui::GetWindowSize();
            ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - esz.x - 12.0f), 12.0f));
        }

        float below = 12.0f;
        {
            const float inset = 12.0f;  // the HUD's, so the two corners agree
            // Room for the shadow, and no more: a window's draw list is clipped
            // to its own rectangle, and at zero padding a two-pixel offset
            // loses its bottom-right corner.
            const float pad = 3.0f;
            styleV2 style(pGui, px3_, 0.0f, fbH);  // veil 0: no panel, no box
            ImGui::GetStyle().WindowPadding = ImVec2(pad, pad);
            Gui::Window fpsWin(pGui, "v2fps", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
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
            //
            // AND IT IS ONLY THE NUMBER. The unit was worth its width while the
            // readout sat in a panel with other lines to be told apart from;
            // alone in a corner in gold there is nothing else it could be
            // counting.
            const std::string f = fmt("%.0f", fps_ + genFps_);
            const ImVec2 sz = ImGui::CalcTextSize(f.c_str());
            // Set every frame rather than on first use: ImGui remembers window
            // positions in an ini file between runs, so "where I asked for it"
            // and "where it appears" are otherwise two different things.
            ImGui::SetWindowPos(ImVec2(inset - pad, inset - pad));
            // WHERE THE NEXT THING IN THIS CORNER MAY START. The recorder's
            // panel shares the corner now, and a REC badge drawn over the frame
            // rate is two readouts and neither legible.
            below = inset + sz.y + 8.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 2.0f, at.y + 2.0f),
                                                IM_COL32(0, 0, 0, 150), f.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold());
            ImGui::TextUnformatted(f.c_str());
            ImGui::PopStyleColor();
        }

        // ---- what the recorder has to say, top left ------------------------
        //
        // IT IS DRAWN INTO THE WINDOW, NOT INTO THE FRAME, so it can never end
        // up in the recording -- see the note at the capture site in
        // onFrameRender. The badge pulses because a recorder that is running is
        // the one piece of state where "I did not notice it was still on" is
        // expensive.
        //
        // THE PANEL OPENS ONLY WHEN THERE IS A LINE FOR IT, which the fps
        // readout used to guarantee and no longer does. An empty AutoResize
        // window is not nothing on the screen: it is a small rounded rectangle
        // in the corner with nothing inside it.
        //
        // So the saved notice is retired HERE, before the test, rather than
        // inside the window where it used to be -- expiring mid-draw would
        // leave exactly that empty rectangle for a frame.
        if (savedTake_.valid() && nowSeconds() - savedAt_ >= kSavedNotice)
            savedTake_ = vb::Take{};
        if (recorder_.recording() || recorder_.busy() || savedTake_.valid()) {
            styleV2 style(pGui, px3_, 0.60f, fbH);  // a lighter veil than the menu's
            Gui::Window hud(pGui, "v2hud", {0, 0}, {12, 12}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            // UNDER THE FRAME RATE, not on top of it -- see `below`.
            ImGui::SetWindowPos(ImVec2(12.0f, below));
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
            } else {
                // WHERE THE PERSON WHO PRESSED R IS ACTUALLY LOOKING. The take
                // is written and the recorder names it on stdout, but stdout is
                // behind the window -- and with nothing opening any more, a
                // silent stop is indistinguishable from a stop that failed.
                //
                // It FADES rather than waiting to be dismissed. An
                // acknowledgement is not a dialog: the thing wanted after a
                // take is the wood back, not another key to press.
                const double f = (kSavedNotice - (nowSeconds() - savedAt_)) / 1.2;
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

        // ---- the copyright, bottom centre, always --------------------------
        //
        // The JS engine's #copyr watermark, brought over as it is written
        // there: the same wording, the same gold, the same 0.64 opacity, the
        // same ten pixels off the bottom edge, centred. Its own note says why
        // it sits outside every overlay over there -- "it has to survive on the
        // loading screen, in play and on the esc menu alike" -- and the same
        // holds here, which is why it is drawn unconditionally rather than
        // beside the readout that comes and goes.
        //
        // THE (c) IS A LETTER AND A RING, and that is not a flourish: the 3x3
        // pixel face has no copyright glyph, so that engine builds the mark out
        // of a px3 "c" with a circle drawn round it. Anything else -- a Unicode
        // (c) from a fallback face, or the two characters -- is a different
        // typeface in the middle of a word. Same trick here, with the circle on
        // the window's own draw list.
        //
        // NOT IN THE RECORDING, like every other thing drawn here: the capture
        // happens in onFrameRender, upstream of the whole interface. That
        // matches the engine it comes from, whose watermark is a DOM element
        // over a canvas the recorder never sees.
        {
            // Padding, and it is NOT what keeps the ring off the window's
            // clip edge -- see the PushClipRectFullScreen below. Raising this
            // was the first fix tried and it cannot work, for a reason worth
            // writing down: ImGui insets InnerClipRect by HALF the window
            // padding but places the CURSOR at the full padding, so every
            // pixel added here moves the text one pixel further from the left
            // edge and the clip only half a pixel to meet it. The margin grows
            // at half a pixel per pixel spent, and the ring -- which starts to
            // the LEFT of the text origin -- keeps losing the race.
            const float pad = 11.0f;
            styleV2 style(pGui, px3_, 0.0f, fbH);  // no panel behind it either
            ImGui::GetStyle().WindowPadding = ImVec2(pad, pad);
            Gui::Window cop(pGui, "v2copy", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);

            // A SPACE AFTER THE c, so the ring has somewhere to be. That engine
            // gets the room from a flex box; here the gap is the space glyph.
            static const char *kMark = "c";
            static const char *kRest = " 2026 voxelbit - all rights reserved";
            const ImVec2 ms = ImGui::CalcTextSize(kMark);
            const ImVec2 rs = ImGui::CalcTextSize(kRest);
            const float wide = ms.x + rs.x;
            ImGui::SetWindowPos(ImVec2((fbW - wide) * 0.5f - pad, fbH - rs.y - 10.0f - pad));

            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            // -- OUT FROM UNDER THE WINDOW'S CLIP RECTANGLE -----------------
            //
            // Everything here is drawn by hand at absolute positions, and the
            // ring reaches further left than the cursor this window sized
            // itself around -- so an auto-resized window's rectangle is simply
            // the wrong shape to clip it by, and it was shaving the left of
            // the circle. The stroke read one pixel there against two on the
            // right, which is what a half-clipped stroke looks like and why an
            // angular PRESENCE test scored it a clean hundred per cent: enough
            // of it survived to be found, and the wrong half was measured.
            //
            // The overlay is screen furniture at a fixed corner, so it is
            // pinned to the screen and clipped by nothing else. Popped below.
            dl->PushClipRectFullScreen();
            // The shadow first, the whole line at once -- #copyr carries one
            // too, and without it gold on a bright sky is unreadable.
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 150), kMark);
            dl->AddText(ImVec2(at.x + ms.x + 1.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 150), kRest);

            const ImU32 gold = ImGui::GetColorU32(ui::kGold(0.64f));
            dl->AddText(at, gold, kMark);
            dl->AddText(ImVec2(at.x + ms.x, at.y), gold, kRest);
            // -- THE RING, ROUND THE INK AND NOT ROUND THE LINE BOX --------
            //
            // CalcTextSize answers with the ADVANCE and the LINE HEIGHT, which
            // is a box the letter merely sits somewhere inside: for a lowercase
            // c that box is most of a line tall and the ink is a short bar
            // across the middle of it. Centring on that box put the circle high
            // and sized it to the leading, which is why it read as a letter
            // next to a circle rather than as a copyright mark.
            //
            // The glyph's own extent is the right thing to ask for, and ImGui
            // will give it: FindGlyph returns the ink box in the font's base
            // units, so scaling by the ratio of the current size to that base
            // puts it in pixels. The ring is then concentric with the letter by
            // construction, at any font size and after any SetWindowFontScale.
            //
            // HALF #copyr's 0.14 em OF THICKNESS, AND A PIXEL MORE RADIUS.
            //
            // The border width that is right in CSS is not right here, and the
            // reason is the size: at this text size 0.14 em is a stroke almost
            // as wide as the hole it encloses, so the ring filled in and the
            // gold c vanished ON TOP of gold. What was left to read was the c's
            // SHADOW -- a dark notch in a solid disc, sitting down and right of
            // centre because a shadow is offset by definition. The letter was
            // centred the whole time; there was nothing to see it against.
            //
            // So the stroke is halved and the circle grows by a pixel, which
            // puts daylight back between the ink and the ring. That is what the
            // CSS is really buying at ITS size, and this is the same look
            // arrived at through this font's numbers rather than through that
            // one's.
            {
                const ImFont *fnt = ImGui::GetFont();
                const ImFontGlyph *gl = fnt ? fnt->FindGlyph((ImWchar)'c') : nullptr;
                const float em = ImGui::GetFontSize();
                float cx = at.x + ms.x * 0.5f, cy = at.y + ms.y * 0.5f, rad = ms.x * 0.62f;
                if (gl && fnt->FontSize > 0.0f) {
                    const float k = em / fnt->FontSize;
                    const float x0 = at.x + gl->X0 * k, x1 = at.x + gl->X1 * k;
                    const float y0 = at.y + gl->Y0 * k, y1 = at.y + gl->Y1 * k;
                    cx = (x0 + x1) * 0.5f;
                    // THE GLYPH BOX IS THE CENTRE. There WAS a pixel of
                    // correction here, on the strength of a measurement that
                    // said the c sat low in the ring -- and the measurement was
                    // of the wrong thing. It was taken while the stroke was
                    // 0.14 em, thick enough that the gold c disappeared into
                    // gold and the only mark left to find was the c's SHADOW,
                    // which is offset +1,+2 because that is what a shadow is.
                    // So a shadow was measured and the ring was moved down to
                    // meet it.
                    //
                    // With the stroke halved the letter is visible and can be
                    // measured directly: ink rows 1963-1974 about a ring
                    // spanning 1959-1980, which is the box centre exactly. The
                    // correction is removed rather than re-tuned -- the metrics
                    // were right the whole time.
                    cy = (y0 + y1) * 0.5f;
                    // TWO PIXELS MORE AIR (user 2026-09-07), on top of the one
                    // the thinner stroke bought back. em * 0.13 is the gap
                    // #copyr gets from its box-sizing; the constant beside it
                    // is this face's, which is small enough that a proportional
                    // term alone cannot buy a whole pixel.
                    rad = maxf(x1 - x0, y1 - y0) * 0.5f + em * 0.13f + 3.0f;
                }
                const float ring = maxf(1.0f, em * 0.07f);
                // -- THE DARK IS AN OUTLINE, NOT A DROP SHADOW ---------------
                //
                // It was a drop shadow at the text's own +1,+2, and that ate
                // the left of the circle. A GLYPH is a solid block, so an
                // offset shadow only ever peeks out from behind it; a thin RING
                // is a one-pixel stroke, and an offset of one pixel lands the
                // dark arc squarely underneath it. The anti-aliased gold then
                // blends with black and the stroke muddies to nothing.
                //
                // Measured on a 16x crop before and after: coverage round the
                // left of the ring was 47-50% against 75-96% everywhere else,
                // and only the left -- because the y offset of two clears the
                // stroke while the x offset of one is about the width of it.
                //
                // So the dark is drawn CONCENTRIC and two pixels wider instead.
                // The gold covers the middle of it and dark shows on both
                // edges, which is what an outline is: the same contrast against
                // bright ground, and no side of the circle can be eaten because
                // nothing is offset into it.
                dl->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0, 0, 0, 150), 0, ring + 2.0f);
                dl->AddCircle(ImVec2(cx, cy), rad, gold, 0, ring);
            }
            dl->PopClipRect();
            // The window has to be told how much it is holding: everything
            // above went straight to the draw list, which ImGui does not
            // measure. Without this the box is empty and collapses.
            ImGui::Dummy(ImVec2(wide, rs.y));
        }

        // ---- the console --------------------------------------------------
        // Drawn before the settings panel and independently of it: T and Y are
        // separate surfaces and either may be up without the other.
        if (consoleOpen_) {
            const float cw = fbW > 0 ? float(fbW) : 1280.0f;
            const float ch = fbH > 0 ? float(fbH) : 720.0f;
            const float margin = 12.0f;  // the HUD's inset, so the two line up
            const float boxW = minf(cw - margin * 2.0f, 720.0f);
            // BOTTOM LEFT, ANCHORED BY ITS BOTTOM EDGE. The pivot is what
            // makes that work: this window is AlwaysAutoResize, so it grows
            // downward by a line the moment a command prints a reply, and
            // positioning its top-left would push the prompt off the bottom of
            // the screen. Pivot (0,1) pins the BOTTOM-left corner instead, so
            // the reply opens upward and the caret never moves.
            ImGui::SetNextWindowPos(ImVec2(margin, ch - margin), ImGuiCond_Always,
                                    ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(boxW, 0.0f), ImGuiCond_Always);
            ImGui::Begin("##v2console", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings);
        // PUSHED BY HAND HERE, because the console is the one surface that is
        // not a Gui::Window inside a styleV2 -- it is a bare ImGui window with
        // its own End() below, and the pop has to happen before that End.
        if (px3_) ImGui::PushFont(px3_);
            // The caret has to be taken on the frame the box appears, or the
            // first keystroke is eaten deciding what is focused.
            if (consoleFocus_) {
                ImGui::SetKeyboardFocusHere();
                consoleFocus_ = false;
            }
            // NOTHING IS DRAWN ABOVE THE PROMPT ANY MORE. The reply used to
            // sit here, stacked over the input the way a game console reads,
            // but Enter now shuts the box before it could be read -- so the
            // reply moved to the fading line at the foot of this branch, which
            // takes over this exact corner once the window is gone.
            ImGui::TextUnformatted(">");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            bool submitted = false;
            if (ImGui::InputText("##cmd", consoleBuf_, sizeof(consoleBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                consoleMsg_ = runCommand(std::string(consoleBuf_));
                consoleBuf_[0] = 0;
                // CLOSES ON ENTER. A command is meant to be one keystroke to
                // open, the line, and done -- back in the wood looking at what
                // it did rather than reading past a box. The reply is not lost
                // with the window: it is handed to the fading line below, which
                // draws in exactly this corner for a few seconds afterwards.
                consoleMsgUntil_ = nowSeconds() + kConsoleMsgHold;
                submitted = true;
            }

            // ---- ESC CLOSES IT, AND IT HAS TO BE ASKED HERE ----------------
            //
            // Not in onKeyEvent, which never sees the key. Falcor dispatches
            // keyboard as
            //
            //     if (mShowUI && mpGui->onKeyboardEvent(e)) return;   // eaten
            //     ... onKeyEvent(e);                                  // skipped
            //
            // and Gui::onKeyboardEvent returns io.WantCaptureKeyboard, which is
            // TRUE for every key while a text field is active. So the whole app
            // is deaf while you are typing -- by design, or typing "f" would
            // toggle fly mode.
            //
            // ImGui's own InputText does handle Escape, but only by reverting
            // the edit and dropping focus. That left the box open and unfocused
            // and took a second Escape to actually shut, which is what this is
            // fixing.
            //
            // IsKeyPressed rather than reading io.KeysDown: it is edge
            // triggered, so holding Escape does not close this and then arm the
            // quit on the next frame. ImGuiKey_Escape resolves because Falcor
            // populates io.KeyMap (Gui.cpp), which is worth knowing -- most of
            // its KeysDown indices are Falcor's own key codes, not ImGui's.
            const bool escaped = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (px3_) ImGui::PopFont();
            ImGui::End();
            // Closed AFTER End(), because setConsoleOpen may hand the mouse
            // back and the window still has to be finished either way. Escape
            // throws the line away and takes the old reply with it; Enter has
            // already run the line and wants the reply left up.
            if (escaped) {
                consoleMsgUntil_ = 0.0;
                setConsoleOpen(false);
            } else if (submitted) {
                setConsoleOpen(false);
            }
        } else if (!consoleMsg_.empty() && nowSeconds() < consoleMsgUntil_) {
            // ---- WHAT THE COMMAND SAID, AFTER THE BOX HAS GONE -------------
            //
            // Same corner, same font, no prompt and no input, so the reply
            // reads as the tail of the line you just typed rather than as a
            // second surface. NoInputs because the mouse is back on the camera
            // the instant Enter lands -- a window sitting there taking clicks
            // would steal the look for as long as it was up.
            const float cw = fbW > 0 ? float(fbW) : 1280.0f;
            const float ch = fbH > 0 ? float(fbH) : 720.0f;
            const float margin = 12.0f;
            const float boxW = minf(cw - margin * 2.0f, 720.0f);
            // Solid first and only fading over the last kConsoleMsgFade
            // seconds, so a reply you are still reading does not dim under you.
            const float left = float(consoleMsgUntil_ - nowSeconds());
            const float alpha = left >= kConsoleMsgFade ? 1.0f : left / kConsoleMsgFade;
            ImGui::SetNextWindowPos(ImVec2(margin, ch - margin), ImGuiCond_Always,
                                    ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(boxW, 0.0f), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::Begin("##v2consolemsg", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing);
            if (px3_) ImGui::PushFont(px3_);
            ImGui::TextUnformatted(consoleMsg_.c_str());
            if (px3_) ImGui::PopFont();
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (!menuOpen_) return;

        styleV2 style(pGui, px3_, 1.0f, fbH);
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
        Gui::Window w(pGui, "settings##v2", menuOpen_, {0, 0}, {0, 0}, kPanel);
        px3Font face(px3_);
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
        // not enough -- see Gui::setSliderWidth, added to v2's fork for this.
        Gui::setSliderWidth(sliderW);
        ImGui::PushItemWidth(sliderW);

        // The title line: the name on the left, and on the right the number
        // that everything below is a trade against. One line, both ends.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTitle());
        ImGui::TextUnformatted("v2  settings");
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
        if (flock_.ready())
            ImGui::TextUnformatted(fmt("%d butterflies   %d colours", flock_.flying(),
                                       flock_.colourCount())
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
        // RANGE RAISED WITH THE DEFAULT. 2.0 was the ceiling and is now where
        // the slider starts, which would have made it a knob that only turns
        // down. 4.0 keeps as much headroom above the default as there is below.
        w.slider("Sun glare", tracer_.flare, 0.0f, 4.0f, false, "%.2f");
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
        // AND NOTHING ELSE ABOUT THE MOUSE. There were two more rows here --
        // acceleration and weight -- and they are gone at the user's word; the
        // note in Options says why they were removed rather than zeroed.
        // Sensitivity is now the whole of it, which is what a raw mouse means.
        if (w.slider("Field of view", fov_, 10.0f, 100.0f)) invalidate();
        // No invalidate here either, and for a stronger reason than the two
        // above: this one changes nothing the renderer can even see. Hidden
        // rather than greyed when there is no voice -- under --no-sound or on
        // a machine with no endpoint, a slider that does nothing is worse
        // than no slider.
        // CALLED VOLUME, BECAUSE THAT IS WHAT SOMEBODY LOOKS FOR. It was
        // "Ambience", which is accurate -- the bed is the only sound the engine
        // makes -- and accurate is not the same as findable.
        //
        // AND A REASON WHEN IT IS MISSING, which reverses the old rule here.
        // Hiding a dead slider is right; hiding it without explanation sends
        // somebody hunting through a menu for a row that was never going to be
        // drawn. A line of text is not a control that does nothing, it is the
        // answer to the question the missing control provokes.
        //
        // THE DEFAULT IS THE MIDDLE OF THE TRACK. This ran to 2.0, and the
        // bed is a background: everything anybody would actually choose
        // lived in the first eighth of the travel, with the whole right-hand
        // half reserved for twice the level the asset was baked at. So the
        // top is now twice the default instead of eight times it, which puts
        // the handle you start with in the centre and spends the travel on
        // the range the ear is actually being asked about.
        //
        // The command line is unchanged and still reaches the baked level:
        // --ambience 1.0, or 2.0, is a number rather than a drag.
        if (ambience_.active()) {
            float amb = ambience_.masterGain();
            // The floor is what stops a bake at zero from welding the control
            // shut: a slider whose top is its bottom can never be dragged back
            // up, and the volume is the one setting somebody is most likely to
            // take all the way down before baking.
            const float top = maxf(0.05f, defaults::kAmbience * 2.0f);
            if (w.slider("Volume", amb, 0.0f, top, false, "%.2f"))
                ambience_.setMasterGain(amb);
        }
        // ITS OWN SLIDER, which is the JS engine's split: the bed and the
        // things the world does are two buses there, because a wood that is too
        // loud and an axe that is too loud are different complaints.
        if (toolSfx_.ready()) {
            float s = toolSfx_.gain();
            if (w.slider("Tools", s, 0.0f, 2.0f, false, "%.2f")) toolSfx_.setGain(s);
        }
        if (!ambience_.active()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            ImGui::TextUnformatted(opt_.background
                                       ? "Volume: no audio under --background"
                                       : "Volume: no audio (--no-sound, or no endpoint)");
            ImGui::PopStyleColor();
        }

        // ---- THE THING IN YOUR HAND ---------------------------------------
        //
        // SEVEN SLIDERS, AND THEY ARE THE POINT OF THIS SECTION. The pose came
        // over from the JS engine's held-item panel, and it came over because
        // that panel existed: nobody arrives at { 0.91, -0.10, 0.96, 0.04,
        // -1.42, 1.58 } by reasoning about it. A viewmodel is judged by eye and
        // adjusted by hand, and the two conversions between that engine and
        // this one (see render/helditem.h) mean the bake is a starting point
        // here rather than a finished answer.
        //
        // THERE IS NO LIGHTING ROW, and that is the feature. The tool is traced
        // at the primary vertex off the world's own materials, so how bright it
        // is has exactly one answer and it is the same answer the wood gets --
        // there is nothing here to tune, and nothing that can drift out of step
        // with the frame behind it. An earlier cut of this composited a
        // separately lit axe over the finished image and needed three rows to
        // make it agree with the picture; it never quite did.
        //
        // THESE SEVEN DO INVALIDATE, through animating(): moving the tool makes
        // every sample already in the film describe a tool that is somewhere
        // else.
        //
        // The values are the JS engine's units, voxels and radians, so a row
        // read here can be pasted straight back into that engine's PICK_DEFS
        // and vice versa. See the note on HeldPose.
        if (held_.ready()) {
            w.separator();
            w.checkbox(fmt("%s in hand  (H)", held_.name()).c_str(), held_.shown);
            if (held_.shown) {
                if (held_.count() > 1)
                    w.text(fmt("  %d of %d -- the wheel changes tools", held_.selected() + 1,
                               held_.count()));
                // THE RANGES ARE IN WORLD VOXELS, like the pose itself. They
                // were the old millimetre-scale pose's -- plus or minus two,
                // when the axe now opens at 7.27 -- so "right" was pinned at
                // its own maximum from the first frame and could only ever
                // move the tool left. Thirty voxels is three metres, which is
                // further than a hand reaches in any direction; forty forward
                // is four, well past arm's length.
                w.slider("  right", held_.pose().x, -30.0f, 30.0f, false, "%.3f");
                w.slider("  up", held_.pose().y, -30.0f, 30.0f, false, "%.3f");
                w.slider("  forward", held_.pose().z, 1.0f, 40.0f, false, "%.3f");
                w.slider("  yaw", held_.pose().yaw, -PI, PI, false, "%.3f");
                w.slider("  pitch", held_.pose().pitch, -PI, PI, false, "%.3f");
                w.slider("  roll", held_.pose().roll, -PI, PI, false, "%.3f");
                // ONE IS EXACT: one model voxel per 10 cm world voxel. The
                // slider is still here because a viewmodel is judged by eye,
                // but anything other than 1.000 is a hand item that no longer
                // matches the grid the world is built on.
                w.slider("  size", held_.pose().scale, 0.25f, 2.0f, false, "%.3f");

                // ---- WHERE THE ARROW SITS ON THE STRING ------------------
                //
                // IN WHOLE VOXELS, because the arrow is voxels stamped into the
                // bow's own grid and there is nowhere for half a voxel to land
                // (see ArrowOffset in render/bow.h). One step is one voxel is
                // ten centimetres, which is the unit the rest of this world is
                // measured in and the unit kArrowPos is written in.
                //
                // THE AXES ARE THE .VOX FILE'S, not the screen's, so what is
                // read here can be pasted back into kArrowPos without being
                // converted: across the bow, along the shaft -- which is the
                // way the string draws -- and up.
                //
                // EACH STEP RECOMPOSES THE WHOLE STRIP, fourteen frames of it,
                // and that is the other reason this steps in whole voxels
                // rather than sliding.
                //
                // THE ROWS ONLY ASK. THEY DO NOT REBUILD, and the first cut of
                // this did -- it called retuneArrow straight from here and took
                // the game down with it. Rebuilding a structure means
                // World::buildBlas, which forces a compaction drain and a
                // BLOCKING submit, and the note over drainCompactions says what
                // is wrong with that in as many words: the forced path "is
                // never taken while a frame is being displayed". This function
                // runs inside the frame, after the trace has been recorded, so
                // fourteen blocking submits land in the middle of a command
                // list that is still being written.
                //
                // So the slider writes a WANT and the frame acts on it, next to
                // world_.update, where every other structure this engine builds
                // is built. One frame of lag on a tuning control, and no
                // GPU work issued from the interface at all.
                if (held_.holdingBow()) {
                    bool moved = false;
                    moved |= w.slider("  arrow across", arrowWant_.across, -12, 12);
                    moved |= w.slider("  arrow along", arrowWant_.along, -12, 12);
                    moved |= w.slider("  arrow up", arrowWant_.up, -12, 12);
                    if (moved) arrowDirty_ = true;
                }
                // COPY, NOT SAVE. The bake writes defaults.h and this pose is
                // not in it -- deliberately, because a viewmodel pose belongs
                // beside the model it poses rather than in a file of renderer
                // settings. So the row hands over the literal to paste into
                // HeldPose, which is exactly what the JS engine's own panel
                // does with the same seven numbers.
                if (w.button("copy pose")) {
                    poseCopied_ = fmt("{ %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff }",
                                      held_.pose().x, held_.pose().y, held_.pose().z, held_.pose().yaw,
                                      held_.pose().pitch, held_.pose().roll, held_.pose().scale);
                    poseCopied_ = std::string(held_.name()) + "  " + poseCopied_;
                    // ...and the arrow with it, in the form kArrowPos wants: a
                    // pose row that is copied without the offset that was tuned
                    // beside it is half an answer.
                    if (held_.holdingBow())
                        poseCopied_ += fmt("   arrow +{ %d, %d, %d } voxels on every frame",
                                           held_.arrow().across, held_.arrow().along,
                                           held_.arrow().up);
                    std::printf("v2: held pose %s\n", poseCopied_.c_str());
                    std::fflush(stdout);
                    ImGui::SetClipboardText(poseCopied_.c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted(poseCopied_.empty()
                                           ? "  left mouse swings; hold it to keep swinging"
                                           : poseCopied_.c_str());
                ImGui::PopStyleColor();
            }
        }
        w.separator();

        // ---- the air, and the lens ------------------------------------------
        //
        // THE CAP IS THE TOP OF THE USEFUL BAND, NOT THE TOP OF WHAT THE PASS
        // WILL DRAW. The froxel grid goes on working far past this -- the cap
        // used to be 0.10, five times what the analytic fog could offer before it
        // washed out to grey -- but nothing up there is a look anyone reaches for,
        // and a trough that wide is not adjustable. The default is 0.0022, so 0.10
        // spent its first 2% on every value worth having and the rest on soup.
        // 0.0040 puts the default a little past halfway and makes the whole travel
        // mean something.
        //
        // Five decimals rather than four for the same reason: %.4f reads out in
        // steps of 0.0001, which over this range is forty of them end to end.
        if (w.slider("Fog density", opt_.r.fogDensity, 0.0f, 0.0040f, false, "%.5f /m"))
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
        // THE NIGHT FLOOR IS BACK, as half of the row below rather than as a
        // row of its own. It left with the sky group and lived on --night-floor
        // alone, which put the answer to "the night is too dark" behind a
        // relaunch. It stands in for airglow and starlight: the model knows
        // about sunlight and nothing else, so at 0 a deep night is honestly --
        // and uselessly -- black.
        //
        // AND NOTE THE TURBIDITY ROW BELOW. It is a PREETHAM parameter, and the
        // scattering path carries its own fixed aerosol profile and ignores it,
        // so with the atmosphere always on that slider moves nothing anybody
        // can see. It is left alone because --no-atmosphere still reads it.
        // ONE ROW FOR THE WHOLE NIGHT. It drives the moon's key light and the
        // airglow floor together -- Options::nightBrightness says why it has to
        // be both -- so 0.5x is a night half as bright at every phase of the
        // moon, rather than only on the ones where the term it happened to move
        // was the one doing the lighting. 0 is the physically honest black; 3
        // leaves the wood readable at midnight, which is usually what a
        // screenshot at that hour actually wants.
        //
        // LEFT OF 1.0 IS DARKER, which is the direction this gets reached for,
        // and the row is still named for brightness: every other slider in this
        // menu moves right for more of what it names, and one that ran
        // backwards would be wrong more often than it was clever.
        if (w.slider("Night brightness", nightLevel_, 0.0f, 3.0f, false, "%.2fx"))
            applyNightLevel();
        if (w.slider("Sky turbidity", opt_.turbidity, 1.8f, 8.0f)) {
            applySun(true);
            invalidate();
        }
        // NO DEPTH-OF-FIELD ROW HERE, and this time the reason is measured
        // rather than assumed. There was a slider, and the lens it drove was
        // correct -- the result still looked wrong. Ray Reconstruction wants
        // every sample in a pixel to share an origin, and a lens is precisely
        // the thing that stops them doing so; what came out was not shallow
        // focus but a smear the denoiser could not resolve. render/camera.h
        // said as much before any of it was tried.
        //
        // --aperture and --focus still drive the same lens OFFLINE, where the
        // film accumulates and there is no denoiser in the way. That is where
        // it earns its keep, and it is the only place it ever did.
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
                                   ? "writes src/core/defaults.h; then rebuild.bat, in v2/"
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
        // -- THE WATER PANEL HAS NO KEY ANY MORE (user 2026-09-14: "you remove
        // -- the water panel from l") ---------------------------------------
        //
        // It had three in two days. It was built on [I] on 2026-09-13; I was
        // rebound to ESC that same day so it took O; O was asked for by name
        // the next day for the building level, so it took L; and L is now gone
        // too. A key that keeps moving is worse than no key -- every note that
        // names one goes stale the moment it moves, and this one had already
        // outlived two.
        //
        // IT IS NOT DELETED. `--water-ui` opens it at start-up and everything
        // below setWaterPanelOpen is untouched, so the eight live terms are
        // still there for the session you actually want to tune water in. What
        // has gone is the press that could open it by accident mid-walk.
        //
        // If it ever wants a key back, this is the spot -- and J, M and Z are
        // what is free. See World::levelOn for what took O.
        // -----------------------------------------------------------------
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
                                         "tools/voxelize_building.py\n");
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
            std::printf("v2: %s\n", on ? "the building" : "back to the wood");
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
            if (pauseOpen_) {
                beginQuit();
                return true;
            }
            if (looking_) {
                setCapture(false);
                return true;
            }
            setRoomOpen(true);
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
        if (e.key == Input::Key::R) toggleRecording();
        // [K] -- THE STACK BADGE'S FOUR NUMBERS. Free at the time of writing
        // and next to nothing else; see the panel for what it holds. The water
        // panel deliberately has NO key any more ("a key that keeps moving is
        // worse than no key"), and the difference is that this one was asked to
        // be reachable: "let me adjust the display number".
        if (e.key == Input::Key::K && !consoleOpen_) {
            stackPanelOpen_ = !stackPanelOpen_;
            std::printf("v2: stack badge panel %s\n",
                        stackPanelOpen_ ? "open" : "closed");
        }
        if (e.key == Input::Key::P) shotRequested_ = true;
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
                else
                    plantSeed();
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
        return std::string(base) + "\\voxelbit-v2-window.txt";
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
    }

    void onResize(uint32_t, uint32_t) override { tracer_.resetAccumulation(); }

  private:
    Options opt_;
    World world_;
    Tracer tracer_;
    Dlss dlss_;
    Player player_;
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
    Swing lastSwing_;
    // WHICH LOOSE BODY, AND WHICH OF ITS VOXELS, when lastSwing_ is a Loose
    // one. The slot alone would not do: the carve wants the struck VOXEL, and
    // re-deriving that from the hit point is a rounding away from the cell in
    // front or the one behind. Meaningless unless lastSwing_.kind is Loose.
    DebrisHit lastDebris_;
    vb::AudioDevice audio_;
    vb::Ambience ambience_;
    ToolSounds toolSfx_;
    // What the settings panel has asked the arrow to be, and whether the frame
    // still has to act on it. The rows edit this rather than the tool's own
    // offset, so dragging stays responsive while the rebuild happens a frame
    // later -- see the note on the rows.
    HeldXform lastHeld_;  // where the hand was last frame -- see loose()
    ArrowOffset arrowWant_;
    bool arrowDirty_ = false;
    Falcor::ref<Falcor::FullScreenPass> crosshair_;
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
    float btnPress_[3] = {0.0f, 0.0f, 0.0f};
    // The green button has been pressed and the wood is not ready yet.
    float btnClock_ = 0.0f, btnAt_ = 0.0f;
    int btnHeld_ = -1, btnPend_ = -1;
    // -- THE RED BUTTON'S AFTERMATH. quitT_ is the collapse, 0 to 1.
    static constexpr float kQuitFadeSec = 1.15f;
    bool quitting_ = false;
    float quitT_ = 0.0f;
    // ALL NINE ON -- see kWFDefault in Shared.slang. App::onLoad overwrites
    // every one of them from opt_.waterFlags, so this initialiser and that
    // default cannot drift apart in practice; it is written out here so reading
    // the member says the same thing.
    bool waterTerm_[9] = {true, true, true, true, true, true, true, true, true};
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
    // THE CAMERA'S OTHER TWO AXES, built exactly as Camera::gpu builds them --
    // w forward, u = w x up, v = u x w. A pose is expressed against these
    // three, so anything that reads a pose back out (the bow's launch point)
    // has to use the same ones or it lands somewhere else.
    Vec3 camRight() const { return normalize(cross(forward(), Vec3(0.0f, 1.0f, 0.0f))); }
    Vec3 camUp() const { return cross(camRight(), forward()); }

    // The world as the player sees it: the terrain, plus the trees and rocks
    // close enough to walk into.
    //
    // Six metres of reach for a body a quarter of a metre wide, because the
    // gather happens ONCE a tick and the player then moves within it. Anything
    // it misses is something the next tick will pick up long before it is
    // reached at 17 m/s.
    // -----------------------------------------------------------------------
    // THE LOOSE WORLD: the solver, the ground it rests on, and the pieces.
    //
    // THE GROUND FOLLOWS THE PLAYER AND IS REBUILT ONLY WHEN THEY LEAVE IT.
    // PhysX needs something to land on, and v2's terrain is a height field by
    // construction -- one height per column, from a pure function -- which is
    // exactly PxHeightFieldGeometry's own primitive. Handing it the resident
    // wood as a mesh would be 91 million triangles and a BVH to cook; handing
    // it a patch is a memcpy of int16s.
    //
    // The margin is what stops it being rebuilt every step at the boundary, and
    // what guarantees nothing falls off an edge before the next patch exists.
    // -----------------------------------------------------------------------
    void stepLoose(float dt) {
        simMs_ += double(dt) * 1000.0;
        // ...AND THE TURNED EARTH GROWS BACK OVER. v1'''s tillRevert, on the
        // same clock the rest of the loose world runs on -- it is the same kind
        // of thing: a change to the static world with a lifetime on it. Costs
        // one compare while nothing is tilled, which is nearly always.
        // -- ...AND ANY SEED THAT WAS IN IT COMES BACK OUT ------------------
        //
        // (user 2026-09-14: "when the tilled grass turns back to dirt/grass,
        // have the planted seeds pop out of the ground and start levitating".)
        //
        // A DROP, WHICH IS WHAT LEVITATING ALREADY IS IN THIS ENGINE. Drops
        // hover for kDropRestSec and are absorbed by walking over them, so a
        // seed that did not take is a seed you can pick up and try somewhere
        // wetter -- which is the whole of what the ask wants and needed no new
        // kind of object.
        //
        // World SAYS WHERE AND app.h SPILLS, because the drop pool is not the
        // world's to reach into. Same split as the spoil, one file over.
        seedsBack_.clear();
        world_.tillRevert(simMs_ * 0.001, &seedsBack_);
        if (!seedsBack_.empty() && seedsTool_ >= 0) {
            const Tool &st = held_.tool(seedsTool_);
            if (!st.models.empty())
                for (size_t k = 0; k < seedsBack_.size(); ++k)
                    drops_.spill(seedsTool_, st.models[0], st.sx, st.sy, st.sz, seedsBack_[k],
                                 float(k) * 1.7f);
        }
        if (physics_.available()) {
            hStart();
            // REBUILT WHEN THE GROUND MOVES, not only when the player does.
            // A dig changes the shape of the floor under everything that is
            // falling, and the patch is the only copy of it the solver has.
            if (world_.takeGroundDirty() ||
                !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                rebuildGroundPatch();
            physics_.step(dt);
            hPhys_ += hStop();
            // Gathered ONCE for the whole band rather than per body: walkWorld
            // runs collidersNear, and asking it per chip per frame would be the
            // expensive part of a feature that is otherwise nearly free.
            // THE ROCKS ARE NOT HULLS ANY MORE. What a loose piece falls
            // against is built where the piece is, off the damaged voxels, and
            // has the hole in it -- see World::buildWindow. A hull of the whole
            // boulder standing beside that would put back exactly the shape
            // that buried every previous version of the chunk.
            // Gathered ONCE for the whole band: walkWorld runs collidersNear,
            // and asking it per chip per frame would be the expensive part of
            // a feature that is otherwise nearly free.
            const WalkWorld ww = walkWorld();
            world_.updateDebris(
                physics_, player_.eyePosition(), simMs_,
                [&](float x, float z) {
                    // Terrain alone: the one floor nothing may ever be under.
                    // ...AND IT HAS THE HOLES IN IT, for the same reason the
                    // walk does now -- a chip that falls into a pit must not be
                    // shoved back out of it by a floor the generator remembers
                    // and the world no longer has.
                    return walkGroundM(ww, x, z);
                });
            world_.flushDebrisInstances();
        }
    }

    // -----------------------------------------------------------------------
    // THE BOULDERS NEARBY, AS COLLISION.
    //
    // A chip must not pass through the stone it came off, and the only way to
    // mean that is to put the stone in the solver. A model's colTop is already
    // a height field -- the top of every voxel column, and the very surface the
    // player is collided against -- so it goes in as one, shared across every
    // placement of the same model.
    //
    // ONLY WHAT IS NEAR, and diffed rather than rebuilt: a static actor costs
    // nothing to leave alone and something to create, and the set changes only
    // when the player walks. Keyed by chunk and decor slot, which is what makes
    // a DAMAGED boulder -- carrying its own colTop -- replace its own actor
    // rather than keeping the pristine shape.
    // -----------------------------------------------------------------------

    void rebuildGroundPatch() {
        const int n = kGroundPatchCols;
        const int step = kGroundPatchStep;
        const int i0 = int(std::floor(player_.pos.x / VOXEL_M)) - (n / 2) * step;
        const int j0 = int(std::floor(player_.pos.z / VOXEL_M)) - (n / 2) * step;
        groundPatch_.resize(size_t(n) * size_t(n));
        // THE MEMO IS THE WHOLE COST HERE. heightVox is several octaves of
        // noise; asked cold, 16 384 columns is a chunk's worth of meshing on
        // the frame the player crosses the boundary. The memo is what the
        // chunk mesher uses for the same reason -- i on the inside, so a
        // lattice cell is reused across a run of columns.
        // ...AND IT HAS THE HOLES IN IT. See World::groundPatch: sampled
        // from heightVox alone this floor had a lid over every pit the player
        // had dug, and anything born under that lid was thrown out of it.
        const auto gt0 = std::chrono::steady_clock::now();
        world_.groundPatch(groundPatch_.data(), n, i0, j0, step);
        const auto gt1 = std::chrono::steady_clock::now();
        world_.takeGroundDirty();
        physics_.setGroundPatch(groundPatch_.data(), n, i0, j0, VOXEL_M, step);
        if (opt_.hitch) {
            const auto gt2 = std::chrono::steady_clock::now();
            std::printf("  [ground] sample %.2f ms   physx %.2f ms\n",
                        std::chrono::duration<double, std::milli>(gt1 - gt0).count(),
                        std::chrono::duration<double, std::milli>(gt2 - gt1).count());
            std::fflush(stdout);
        }
    }

    // SIXTY-FOUR METRES OF GROUND, for the same 25,600 samples the sixteen
    // metres used to cost -- 160 a side, as before, one every four voxels.
    //
    // The width is set by the longest thing that can fall on it. A felled pine
    // is twenty-six metres, so its crown lands well outside a sixteen-metre
    // patch and there was nothing under it: the tree went through the world.
    // Nothing walks on this height field -- the player's ground is answered on
    // the CPU against the voxel columns -- so trading resolution for reach
    // costs a chip a few centimetres of accuracy on a slope and buys a tree a
    // floor to land on.
    //
    // And it is CHEAPER in practice than what it replaces: four times the width
    // means the player leaves it far less often, and leaving it is the only
    // thing that rebuilds it.
    static constexpr int kGroundPatchCols = 160;
    static constexpr int kGroundPatchStep = 4;
    static constexpr float kGroundMarginM = 4.0f;
    std::vector<int16_t> groundPatch_;
    // WHAT THE LAST BLOW TOOK OUT, kept as a member so a swing does not
    // allocate: dig and carveModel fill it with the voxels actually removed,
    // and the piece that flies at you is meshed from exactly those.
    std::vector<uint8_t> spoilVol_;
    int spoilN_ = 0;
    // Turned once per blow so two chips never tumble the same way.
    uint32_t swingSalt_ = 0;
    // Where the last blow actually bit, in world metres.
    Vec3 spoilAt_{0, 0, 0};
    // ...and the quarter turn of whatever it was cut out of.
    float spoilYaw_ = 0.0f;
    // Whether the last blow was the one that put a tree on the ground. Read by
    // the swing log and nothing else.
    bool felled_ = false;
    // Whether the spawn has been checked against a world that actually exists.
    // See the note beside it in the frame loop.
    bool spawnSettled_ = false;
    double simMs_ = 0.0;

    // -----------------------------------------------------------------------
    // THE SAME WORLD, WITH THE COLLIDERS GATHERED FURTHER OUT.
    //
    // walkWorld's six metres is right for a body: it is what you can walk into
    // this frame. Anything that TRAVELS needs its own reach, and the list is
    // the only thing that changes -- the terrain, the edits and the level are
    // the same world however far you are looking.
    //
    // ITS OWN VECTOR, so a wide gather cannot leave the walk's list holding
    // half the wood for the rest of the frame.
    WalkWorld wideWalkWorld(float reachM) {
        world_.collidersNear(player_.pos, reachM, &wideSolids_);
        if (world_.levelOn()) {
            Solid lv;
            if (world_.levelSolid(&lv)) wideSolids_.push_back(lv);
        }
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = wideSolids_.data();
        w.solidCount = int(wideSolids_.size());
        return w;
    }

    WalkWorld walkWorld() {
        world_.collidersNear(player_.pos, 6.0f, &solids_);
        // THE LEVEL IS NOT IN A CHUNK, so collidersNear cannot find it: that
        // walk goes through the resident chunk map and this building is four
        // kilometres from the nearest chunk and 640 m over it. One solid,
        // appended by hand, which is the whole of the level's physics -- see
        // World::levelSolid and Solid::interior.
        if (world_.levelOn()) {
            Solid lv;
            if (world_.levelSolid(&lv)) solids_.push_back(lv);
        }
        WalkWorld w;
        w.terrain = &world_.terrain;
        // ...AND WHERE THE HOLES ARE. The ground is the generator plus the
        // edits, and a swing that asked only the first could not see a pit it
        // had dug a moment ago -- see TerrainProbe.
        w.edits = &world_.editStore();
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
    static const std::vector<BiomeName> &biomeNames() {
        static const std::vector<BiomeName> t = {
            {"pine", "pine_forest", Biome::Pine},
            {"birch", "birch_forest", Biome::Birch},
            // v1's wood, imported 2026-09-16. Its band sits between the other
            // two -- see VoxelTerrain's band note -- so /locate oak from the
            // birch is the shorter walk of the two.
            {"oak", "oak_forest", Biome::Oak},
        };
        return t;
    }

    // -----------------------------------------------------------------------
    // THE LIFE /locate CAN TAKE YOU TO.
    //
    // "/locate skunk. this teleports the player to the nearest life."
    // (user 2026-09-14.) The biome table above takes you to a PLACE, which is
    // a coordinate this engine can compute from nothing; this one takes you to
    // an ANIMAL, which exists only where it is standing right now. So every
    // row below is answered by asking that population, never by arithmetic.
    //
    // ADDING AN ANIMAL IS ONE ROW, for the reason the biome table gives: the
    // parser, the list in both error messages and the teleport all read this,
    // so a creature cannot be half-registered. The one thing a row cannot
    // carry is WHICH population to ask -- six classes with six containers --
    // and that is the switch in nearestLife, which the compiler checks is
    // exhaustive.
    //
    // WHAT EACH COLUMN IS FOR:
    //
    //   stand  HOW FAR OFF YOU ARRIVE, and it is not decoration. Every land
    //          mammal has a flee sphere (MarchSpec::fleeInM) and landing
    //          inside it means the animal you asked to be taken to bolts
    //          before you have seen it do anything else. Each one is set
    //          outside its own hysteresis: the mouse's 8.6 m flee-out is why
    //          it is the odd number in the column. Anything that swims gets
    //          the minimum, because standNear walks out to the shore on its
    //          own and starting further out only skips usable ground.
    //
    //   wood   WHERE IT LIVES WHEN IT IS NOT HERE: -1 either, 0 pine, 1 birch,
    //          from MarchSpec::wood and the hive pass in scene/chunks.h. This
    //          is only ever read when nothing was found, and then it is the
    //          difference between "there are none" -- which is wrong, there
    //          are plenty, one band over -- and taking the player to them.
    //
    //   water  IT NEEDS A LAKE RATHER THAN A WOOD. Same job as `wood` for the
    //          nine populations in render/lake.h: with none in range the
    //          fallback is the nearest shore, which is where they will be.
    // -----------------------------------------------------------------------
    enum class Life {
        Bunny, Skunk, Armadillo, Porcupine, Mouse, Worm, Snake,
        Ant, Fly, Ladybug, Frog, Firefly,
        Bee, Hive, Butterfly, Songbird, Flock,
        Salmon, Bass, Koi, Minnow, Catfish, Bluegill, Duck, Dragonfly, LilyPad,
    };

    struct LifeName {
        const char *name;
        const char *alias;   // "" for none
        Life life;
        float stand;
        int wood;            // -1 either, 0 pine, 1 birch
        bool water;
        // ONLY AFTER DARK. The survey below judged a row by its wood and its
        // water and knew nothing about the clock, so the firefly -- which the
        // engine will not spawn until the sun is 3.4 degrees under -- came back
        // "NONE, AND IT SHOULD BE HERE" on every daytime run and failed the
        // whole test. A test that is always red is a test nobody reads, and it
        // would have hidden a real row breaking behind it.
        bool night = false;
    };

    static const std::vector<LifeName> &lifeNames() {
        //                name         alias          kind             stand wood water
        static const std::vector<LifeName> t = {
            {"bunny",     "rabbit",    Life::Bunny,      5.0f, -1, false},
            {"skunk",     "",          Life::Skunk,      5.0f, -1, false},
            {"armadillo", "",          Life::Armadillo,  5.0f,  0, false},
            {"porcupine", "",          Life::Porcupine,  5.0f,  0, false},
            {"mouse",     "",          Life::Mouse,     10.0f,  1, false},
            {"worm",      "",          Life::Worm,       4.0f, -1, false},
            {"snake",     "grass_snake",Life::Snake,     5.0f,  1, false},
            {"firefly",   "fireflies", Life::Firefly,    4.0f, -1, false, true},
            {"ant",       "ants",      Life::Ant,        4.0f, -1, false},
            {"fly",       "flies",     Life::Fly,        4.0f,  0, false},
            {"ladybug",   "ladybird",  Life::Ladybug,    4.0f, -1, false},
            // -- A WATERSIDE ROW, AND THAT IS THE FIX (user 2026-09-14: "I
            //    dont see any frogs even when doing /locate frog") -----------
            //
            // It said `water = false`, so a miss sent you to the BIRCH BAND
            // CENTRE -- dry birch wood, where a frog can never spawn, because
            // fillFrogs places from a bank and nothing else. The command
            // worked perfectly and took you to the one kind of place the
            // animal does not live, every time, for ever.
            //
            // `water = true` with `wood = 1` is what it actually is: a frog
            // wants a birch SHORE. nearestWater takes the wood now -- it is
            // the first row that has ever needed it, since every other swimmer
            // is a pine row and a lake was the whole condition.
            {"frog",      "",          Life::Frog,       4.0f,  1, true},
            {"bee",       "bees",      Life::Bee,        4.0f,  1, false},
            {"hive",      "beehive",   Life::Hive,       5.0f,  1, false},
            {"butterfly", "",          Life::Butterfly,  4.0f, -1, false},
            {"songbird",  "bird",      Life::Songbird,   5.0f, -1, false},
            // "songbird" is the one PERCHED in a crown (render/birds.h) and
            // "flock" is the one FLYING (render/birdflock.h) -- two
            // populations, two files, and the same animal to look at.
            {"flock",     "flyingbird",Life::Flock,      8.0f, -1, false},
            {"salmon",    "",          Life::Salmon,     2.0f,  0, true},
            {"bass",      "",          Life::Bass,       2.0f,  0, true},
            {"koi",       "",          Life::Koi,        2.0f,  0, true},
            {"minnow",    "",          Life::Minnow,     2.0f,  0, true},
            {"catfish",   "",          Life::Catfish,    2.0f,  0, true},
            {"bluegill",  "blue_gill", Life::Bluegill,   2.0f,  0, true},
            {"duck",      "",          Life::Duck,       2.0f,  0, true},
            {"dragonfly", "",          Life::Dragonfly,  2.0f,  0, true},
            {"lilypad",   "lily",      Life::LilyPad,    2.0f,  0, true},
        };
        return t;
    }

    // -----------------------------------------------------------------------
    // WHERE THE NEAREST ONE OF A KIND IS, or false if none is live.
    //
    // FALSE IS THE ORDINARY ANSWER, NOT AN ERROR. A population only exists
    // inside the streaming ring -- see the fill in each of these files -- so
    // asking for an armadillo from the birch wood finds nothing because there
    // IS nothing, and the caller's job is then to say where they are instead.
    // -----------------------------------------------------------------------
    bool nearestLife(Life k, Vec3 *at) const {
        float d = 0.0f;
        switch (k) {
            case Life::Bunny:     return bunnies_.nearest(pos_, at, &d);
            // nearestSkunk answers for any of the four marchers -- the name is
            // bunnies.h's own shorthand for the family, as publishSkunks is.
            case Life::Skunk:     return bunnies_.nearestSkunk(kMarchSkunk, pos_, at, &d);
            case Life::Armadillo: return bunnies_.nearestSkunk(kMarchArmadillo, pos_, at, &d);
            case Life::Porcupine: return bunnies_.nearestSkunk(kMarchPorcupine, pos_, at, &d);
            case Life::Mouse:     return bunnies_.nearestSkunk(kMarchMouse, pos_, at, &d);
            case Life::Worm:      return bunnies_.nearestSkunk(kMarchWorm, pos_, at, &d);
            case Life::Snake:     return bunnies_.nearestSkunk(kMarchSnake, pos_, at, &d);
            case Life::Firefly:   return critters_.nearestFirefly(pos_, at, &d);
            case Life::Ant:       return critters_.nearestAnt(pos_, at, &d);
            case Life::Fly:       return critters_.nearestFly(pos_, at, &d);
            case Life::Ladybug:   return critters_.nearestBug(pos_, at, &d);
            case Life::Frog:      return critters_.nearestFrog(pos_, at, &d);
            case Life::Bee:       return bees_.nearest(pos_, at, &d);
            case Life::Hive:      return nearestHive(at);
            case Life::Butterfly: return flock_.nearest(pos_, at, &d);
            case Life::Songbird:  return birds_.nearest(pos_, at, &d);
            case Life::Flock:     return flock2_.nearest(pos_, at, &d);
            case Life::Salmon:    return lake_.nearestFish(pos_, 0, at, &d);
            case Life::Bass:      return lake_.nearestFish(pos_, 1, at, &d);
            case Life::Koi:       return lake_.nearestFish(pos_, 2, at, &d);
            case Life::Minnow:    return lake_.nearestFish(pos_, 3, at, &d);
            case Life::Catfish:   return lake_.nearestFish(pos_, 4, at, &d);
            case Life::Bluegill:  return lake_.nearestFish(pos_, 5, at, &d);
            case Life::Duck:      return lake_.nearestDuck(pos_, at, &d);
            case Life::Dragonfly: return lake_.nearestDfly(pos_, at, &d);
            case Life::LilyPad:   return lake_.nearestPad(pos_, at, &d);
        }
        return false;
    }

    // A HIVE IS NOT A POPULATION. It is decor hanging in a birch crown, so
    // there is no class holding a list of them -- World::decorNear is the
    // register, and it is the same query the bees themselves are handed.
    //
    // ASKED FRESH AND WIDE RATHER THAN READING hivesNear_. That list is
    // gathered to kBeeHiveM (40 m) because that is how far a bee cares; a
    // command that can only find a hive already well within earshot of one is
    // not worth having. The reach here is the loaded ring's own, and the scan
    // is over resident chunks only, which is a one-off cost on one keystroke.
    //
    // THE POSITION IS A CORNER, which decorNear says in its own comment and is
    // worth repeating at the one caller that AIMS at the result: the hive is
    // about a metre across, so the crosshair lands on its edge rather than its
    // middle. Close enough to be looking at it, and the bees orbiting it are
    // what the eye finds anyway.
    bool nearestHive(Vec3 *at) const {
        std::vector<Vec3> hives;
        world_.decorNear(5, pos_, 260.0f, &hives);
        float best = 1e30f;
        for (const Vec3 &h : hives) {
            const float dx = h.x - pos_.x, dz = h.z - pos_.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = h;
        }
        return best < 1e29f;
    }

    void setConsoleOpen(bool on) {
        if (on == consoleOpen_) return;
        if (on) {
            consoleCapture_ = looking_;
            if (looking_) setCapture(false);
            holdLook_ = false;
            consoleBuf_[0] = 0;
            consoleFocus_ = true;
            // A FRESH BOX. The reply lives in the fading line now, so carrying
            // the last one back into the window would stack a stale answer
            // above a prompt that has not been typed into yet.
            consoleMsg_.clear();
            consoleMsgUntil_ = 0.0;
        } else if (consoleCapture_) {
            setCapture(true);
            consoleCapture_ = false;
        }
        consoleOpen_ = on;
    }

    // The centre of the nearest band of `b` to the player, in world x. The
    // bands repeat with period 2 * kBandW, so this is the band centre plus
    // whichever whole period lands closest.
    // -----------------------------------------------------------------------
    // THE NEAREST SHORE, for /locate water.
    //
    // TWO SEARCHES, AND THE SECOND ONE IS THE POINT. Finding a wet column is
    // easy; arriving ON it drops you on the LAKE BED, underwater, which is not
    // what "take me to water" means. So the wet column is only an anchor, and
    // the spot returned is the nearest DRY column to it -- a shore, looking at
    // the water rather than standing in it.
    //
    // RINGS RATHER THAN A GRID because the answer wanted is the nearest one,
    // and a ring search returns it in the order it wants. 6 m steps out to
    // 6 km: lakes are basin-gated and sparse (0.97% of the world is wet, and
    // the nearest one to the origin is several hundred metres away), so a
    // coarse ring is what keeps this from being a second of stalling.
    // -----------------------------------------------------------------------
    // `wood` is -1 for any, 0 pine, 1 birch -- the frog is the only row that
    // passes anything but -1, and it has to: sending it to the nearest lake
    // full stop lands it on a PINE shore, where it is gated out and will never
    // appear. See its row in lifeNames.
    bool nearestWater(float *outX, float *outZ, int wood = -1) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        auto wetAt = [&](float x, float z) {
            if (wood >= 0 && (t.birchAt(x) ? 1 : 0) != wood) return false;
            const int vi = int(floorf(x / VOXEL_M)), vj = int(floorf(z / VOXEL_M));
            int wy = 0;
            return t.lakeColumn(vi, vj, memo, &wy);
        };
        float wx = 0.0f, wz = 0.0f;
        bool found = false;
        for (float r = 0.0f; r <= 6000.0f && !found; r += 6.0f) {
            const int steps = (r < 1.0f) ? 1 : maxi(8, int(2.0f * PI * r / 6.0f));
            for (int k = 0; k < steps; ++k) {
                const float a = float(k) / float(steps) * 2.0f * PI;
                const float x = pos_.x + cosf(a) * r, z = pos_.z + sinf(a) * r;
                if (!wetAt(x, z)) continue;
                wx = x; wz = z; found = true; break;
            }
        }
        if (!found) return false;
        // Back out to dry land -- the nearest column that is not in the lake.
        for (float r = 2.0f; r <= 200.0f; r += 2.0f) {
            const int steps = maxi(8, int(2.0f * PI * r / 2.0f));
            for (int k = 0; k < steps; ++k) {
                const float a = float(k) / float(steps) * 2.0f * PI;
                const float x = wx + cosf(a) * r, z = wz + sinf(a) * r;
                if (wetAt(x, z)) continue;
                *outX = x; *outZ = z; return true;
            }
        }
        *outX = wx; *outZ = wz;  // a lake with no shore within 200 m: stand in it
        return true;
    }

    // THE PERIOD IS THE WORLD'S, NOT A COPY OF IT. This read
    // `2.0f * kBandW` -- correct while there were two woods, and silently wrong
    // the moment a third was inserted: /locate pine would have walked you to a
    // multiple of 1600 m when the pine band now repeats every 2400, which lands
    // in whichever wood happens to be there. Nothing would have reported it;
    // you would simply have arrived among the wrong trees.
    //
    // Derived from bandCount() so a fourth wood cannot reintroduce it.
    float nearestBandX(Biome b) const {
        const float period = VoxelTerrain::bandCount() * VoxelTerrain::kBandW;
        const float c = VoxelTerrain::bandCentre(b);
        const float k = floorf((pos_.x - c) / period + 0.5f);
        return c + k * period;
    }

    void teleportTo(float x, float z) {
        // -- THE TREES ARE GATHERED ROUND THE DESTINATION ---------------------
        //
        // NOT round where you are standing, which is what walkWorld() does and
        // is why this is not simply that call. placeOnGround runs findClear,
        // whose whole job is to step a body out of a trunk it would otherwise
        // be planted inside -- and findClear can only see the solids in the
        // WalkWorld it is handed. walkWorld() gathers a six-metre bubble round
        // player_.pos, so after a jump of a hundred metres that list describes
        // the wood you have just LEFT: the check ran, found nothing in the way,
        // and passed every spot in the new one.
        //
        // It cost nothing to miss while /locate went to a BAND CENTRE, which is
        // a coordinate with no particular tree at it. /locate skunk arrives
        // five metres from an animal a few dozen times a session, and a pine
        // wood puts a trunk under about one spot in sixteen.
        //
        // 8 m, because findClear spirals out to 6 and a solid is kept by its
        // centre -- a trunk whose middle is just outside the gather is still
        // one the body can be inside.
        world_.collidersNear(Vec3(x, player_.pos.y, z), 8.0f, &solids_);
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = solids_.data();
        w.solidCount = int(solids_.size());
        player_.placeOnGround(w, x, z);
        pos_ = player_.eyePosition();
        // EVERYTHING TEMPORAL HAS TO BE TOLD. The film, the fog's history and
        // the reconstruction all carry state about somewhere else entirely, and
        // blending out of it drags the old wood across the new one for a
        // second. The streamer re-rings itself from the new position on its own.
        moving_ = true;
        tracer_.resetAccumulation();
        volfog_.invalidate();
    }

    // Point the camera at a world position. The asset deck's own two lines --
    // see stageSubject, which now calls this -- pulled out because /locate
    // needs exactly the same thing: being set down beside an animal and left
    // facing the other way is not being taken to it.
    void lookAt(const Vec3 &p) {
        const Vec3 aim = p - pos_;
        const float len = maxf(0.01f, length(aim));
        yaw_ = atan2f(aim.x, -aim.z) * 180.0f / PI;
        pitch_ = asinf(clampf(aim.y / len, -1.0f, 1.0f)) * 180.0f / PI;
    }

    // -----------------------------------------------------------------------
    // WHERE YOU STAND TO LOOK AT SOMETHING -- WHICH IS NOT WHERE IT IS.
    //
    // ARRIVING ON THE ANIMAL IS WRONG TWICE OVER. On land it puts the player
    // inside a body and inside its flee sphere, so the thing you asked for
    // bolts on the frame you appear. On water it puts you on the LAKE BED,
    // underwater, which is the trap /locate water already carries a paragraph
    // about -- and every fish, duck, dragonfly and lily pad in the table is on
    // water.
    //
    // SO IT IS ONE RING SEARCH OUTWARD FROM THE CREATURE, starting at that
    // row's own stand-off and taking the first column that is not wet. ON LAND
    // THAT IS THE STAND-OFF ITSELF, because the ground beside a skunk is dry;
    // OVER WATER THE SAME LOOP WALKS OUT TO THE SHORE on its own. Two cases,
    // no branch -- and the water half is nearestWater's second pass, which is
    // the same answer arrived at by the same means.
    //
    // THE SWEEP STARTS ON THE PLAYER'S OWN SIDE. Every point on a ring is the
    // same distance from the animal, so with no preference the arrival side is
    // wherever k == 0 lands -- and being sent round to the far shore of a lake
    // to look at a fish is a long walk for nothing. Opening the sweep at the
    // bearing back toward the player and widening it alternately each way
    // makes the accepted spot the nearest acceptable one to where you were.
    //
    // 1.5 m STEPS, 400 m OUT. Finer than nearestWater's 6 m because this one
    // is placing a body rather than finding a lake, and the cap is a lake's
    // half-width: past that there is no shore to reach and the caller is
    // better off being told.
    // -----------------------------------------------------------------------
    bool standNear(float tx, float tz, float stand, float *outX, float *outZ) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        const float a0 = atan2f(pos_.x - tx, pos_.z - tz);
        for (float r = maxf(1.5f, stand); r <= 400.0f; r += 1.5f) {
            const int steps = maxi(8, int(2.0f * PI * r / 1.5f));
            for (int k = 0; k < steps; ++k) {
                // 0, +1, -1, +2, -2 ... out from the player's bearing.
                const int half = (k + 1) / 2;
                const float turn = float(((k & 1) ? half : -half)) / float(steps) * 2.0f * PI;
                const float a = a0 + turn;
                const float x = tx + sinf(a) * r, z = tz + cosf(a) * r;
                const int vi = int(floorf(x / VOXEL_M)), vj = int(floorf(z / VOXEL_M));
                int wy = 0;
                if (t.lakeColumn(vi, vj, memo, &wy)) continue;
                *outX = x;
                *outZ = z;
                return true;
            }
        }
        return false;
    }

    // Put the player beside `at` and face them at it. Returns how far off the
    // arrival ended up being, which is what the reply quotes -- a fish reached
    // from forty metres of shore and one reached from four are both "the
    // nearest salmon", and the number is the only thing that says which.
    float teleportToLife(const Vec3 &at, float stand) {
        float sx = at.x, sz = at.z;
        // A creature with no dry column within 400 m: stand where it is. The
        // player floats over water rather than falling through it, so this is
        // survivable, and it cannot happen on any lake this world generates.
        if (!standNear(at.x, at.z, stand, &sx, &sz)) { sx = at.x; sz = at.z; }
        teleportTo(sx, sz);
        lookAt(at);
        const float dx = at.x - pos_.x, dz = at.z - pos_.z;
        return sqrtf(dx * dx + dz * dz);
    }

    // The names in the table, wrapped -- the console's reply window is one
    // TextUnformatted and will not wrap for itself, and nineteen animals on
    // one line runs off the side of the screen.
    //
    // BUILT FROM THE TABLE, so the help can never list a creature /locate does
    // not know or miss one it does.
    static std::string lifeList(const char *indent, size_t perLine) {
        std::string m;
        for (size_t i = 0; i < lifeNames().size(); ++i) {
            if (i % perLine == 0) m += (i ? "\n" : "");
            m += (i % perLine == 0) ? indent : "  ";
            m += lifeNames()[i].name;
        }
        return m;
    }

    // What /locate knows, for the empty line and for a name it does not have.
    // Three lines, because one would be a hundred and forty characters.
    std::string locateMenu(const std::string &lead) const {
        std::string m = lead + "\n  places  water";
        for (size_t i = 0; i < biomeNames().size(); ++i)
            m += "  " + std::string(biomeNames()[i].name);
        return m + "\n  life\n" + lifeList("    ", 7);
    }

    // -----------------------------------------------------------------------
    // /locate <animal> -- TAKE ME TO THE NEAREST ONE.
    //
    // THE ANSWER WHEN THERE IS NONE IS STILL A TELEPORT. A population exists
    // only inside the streaming ring, and half this table is gated to one wood
    // or to water (see MarchSpec::wood, the hive pass in scene/chunks.h, and
    // every fill in render/lake.h) -- so "/locate armadillo" typed in the
    // birch wood finds nothing, and there is nothing wrong. Replying "none
    // found" would be true and useless: there are six of them, one band over.
    // So a miss falls through to the PLACE the creature lives, which is a
    // coordinate this engine can always compute, and the population fills in
    // around the player over the next second -- BirthGate waives its floor for
    // exactly this case, a jump of more than 40 m in one tick. See
    // [[v2-spawn-contract]].
    //
    // WHICH IS WHY THE REPLY SAYS WHICH OF THE TWO HAPPENED. "skunk -- 412,
    // -80, 5.0 m off" is an animal on your screen; "no armadillo here -- the
    // pine wood" is a promise about the next few seconds, and reading one as
    // the other is the report "the command does nothing".
    // -----------------------------------------------------------------------
    std::string locateLife(const LifeName &ln) {
        // NOT FROM THE DECK, because it despawns every population on the way
        // in (see stageSubject) and does not run their updates -- there is
        // nothing to find and the answer would always be the fallback, which
        // would then teleport the player off a deck they opened on purpose.
        //
        // AND NOT WITH THE PAUSE BUTTONS UP, for a different reason now that
        // the wood keeps living behind them: the panel is PINNED where it was
        // opened (see setRoomOpen), so a teleport would leave it standing in a
        // wood the player is no longer in.
        if (world_.staged() || pauseOpen_)
            return std::string("not from in here -- back to the wood first");

        Vec3 at{0.0f, 0.0f, 0.0f};
        if (nearestLife(ln.life, &at)) {
            const float d = teleportToLife(at, ln.stand);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%s -- %.0f, %.0f, %.0f m off", ln.name, pos_.x,
                          pos_.z, d);
            return std::string(buf);
        }

        // ---- none in range: go to where they live ---------------------------
        //
        // WATER FIRST, because it is the stronger condition of the two. Every
        // row that swims is also a pine row, and a lake is a place inside that
        // wood rather than a second wood to choose between -- so sending a
        // salmon-hunter to the band centre would leave them standing in dry
        // pines having been told they were on their way to a fish.
        if (ln.water) {
            float wx = 0.0f, wz = 0.0f;
            if (!nearestWater(&wx, &wz, ln.wood))
                return std::string("no ") + ln.name +
                       " and no water it would live beside within 6 km -- lakes sit in "
                       "basins, and this one has to be in the right wood";
            teleportTo(wx, wz);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "no %s in range -- the nearest shore, %.0f, %.0f. "
                                            "give it a moment",
                          ln.name, wx, wz);
            return std::string(buf);
        }

        if (ln.wood >= 0) {
            const bool inBirch = world_.terrain.birchAt(pos_.x);
            const bool wantBirch = (ln.wood == 1);
            // ALREADY IN THE RIGHT WOOD AND STILL NOTHING. Not a place
            // problem, so do not teleport: either the models failed to load
            // (the loaders say so on stdout) or, for a hive, this stretch of
            // birch simply drew none -- they are on 5% of the trees.
            if (inBirch == wantBirch)
                return std::string("no ") + ln.name + " in range -- walk on a little";
            if (world_.terrain.forced)
                return std::string("no ") + ln.name + " here, and the world is pinned to one "
                                   "wood (--birch / --pine) -- restart without it";
            const float tx = nearestBandX(wantBirch ? Biome::Birch : Biome::Pine);
            teleportTo(tx, pos_.z);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "no %s here -- the %s wood, %.0f, %.0f. "
                                            "give it a moment",
                          ln.name, wantBirch ? "birch" : "pine", tx, pos_.z);
            return std::string(buf);
        }

        // Either wood, so there is nowhere better to be sent: this one did not
        // load. Every loader prints its own reason at startup.
        return std::string("no ") + ln.name + " anywhere -- check the console output for the "
                           "loader's warning";
    }

    // Returns the reply to show. Never throws; an unknown command is a message,
    // not a failure.
    std::string runCommand(std::string line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '/')) line.erase(line.begin());
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) return std::string();

        std::string verb = line, arg;
        const size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            verb = line.substr(0, sp);
            arg = line.substr(sp + 1);
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        }
        for (char &c : verb) c = char(tolower((unsigned char)c));
        for (char &c : arg) c = char(tolower((unsigned char)c));

        if (verb == "locate") {
            if (arg.empty()) return locateMenu("locate what?");
            // WATER IS NOT A BIOME, so it is not a row in that table -- it is
            // a feature of the landform inside one. Handled before the band
            // loop, and it works in a pinned world too, unlike the bands.
            if (arg == "water" || arg == "lake") {
                float wx = 0.0f, wz = 0.0f;
                if (!nearestWater(&wx, &wz))
                    // NOT "the birch wood has none at all" any more: that was
                    // true when birchWater was kNoWater and has not been since
                    // the lakes landed. All three woods are wet now -- 5.9% of
                    // the pine, 7.6% of the birch, 7.5% of the oak -- so a miss
                    // here means you are between basins, not in a dry wood.
                    return std::string("no water within 6 km -- lakes sit in basins, "
                                       "and you are between them; walk on or try "
                                       "/locate <wood>");
                teleportTo(wx, wz);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "the shore -- %.0f, %.0f", wx, wz);
                return std::string(buf);
            }
            for (const BiomeName &bn : biomeNames()) {
                if (arg != bn.name && arg != bn.alias) continue;
                // --birch and --pine pin the world to one wood, so there is no
                // other band to travel to. Say so rather than teleporting to a
                // place that is the same as this one.
                if (world_.terrain.forced) {
                    return std::string("the world is pinned to one wood "
                                       "(--birch / --pine / --oak) -- restart without it to "
                                       "walk between them");
                }
                const float tx = nearestBandX(bn.biome);
                teleportTo(tx, pos_.z);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s forest -- %.0f, %.0f", bn.name, tx, pos_.z);
                return std::string(buf);
            }
            // ---- ...AND THE LIFE ------------------------------------------
            for (const LifeName &ln : lifeNames())
                if (arg == ln.name || arg == ln.alias) return locateLife(ln);
            return locateMenu("nothing called '" + arg + "'.");
        }
        if (verb == "where") {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%.0f, %.0f, %.0f -- the %s wood", pos_.x, pos_.y,
                          pos_.z,
                          world_.terrain.woodName(pos_.x));
            return std::string(buf);
        }
        if (verb == "help")
            return std::string("/locate <biome|water>   /where   "
                               "ENTER runs and closes   ESC cancels\n"
                               "/locate <animal> takes you to the nearest one:\n") +
                   lifeList("  ", 7);
        return std::string("unknown command '" + verb + "' -- try /help");
    }

    // THE SAME TRADE THE MENU MAKES: while a panel is up the mouse belongs to
    // it, and the look is handed back on close only if it was ours to begin
    // with. Kept separate from setMenuOpen so the two panels cannot fight over
    // who restores the capture.
    // -----------------------------------------------------------------------
    // INTO THE ROOM AND BACK OUT, which is the editor deck's own trip -- see
    // the U key. The wood's position, heading and flight are remembered on the
    // way in and put back on the way out, because a pause that moves you is not
    // a pause.
    //
    // THE MOUSE IS TAKEN, NOT HANDED BACK, which is the opposite of what every
    // other panel in this file does and is the right call here. The buttons are
    // picked by the CROSSHAIR -- see buttonUnderCrosshair -- and the outer two
    // sit twenty degrees off centre, so a player who cannot turn their head can
    // only ever press the green one. A settings panel is a page you point at; a
    // room is a place you look around in.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE WORDS OVER THE BUTTONS.
    //
    // "quit", "back" and "discord", standing in the air a hand's width above
    // the button each one names -- see V2Holo in Shared.slang for what they are
    // and render/holotext.h for the face they are written in.
    //
    // IN THE ROOM, NOT ON THE SCREEN. This is the whole point of them and it is
    // worth saying next to the code: an ImGui label over each button would have
    // been four lines and would have been WRONG, because the pause room is a
    // place the player is standing in and text pasted on the glass is not in it.
    // The plane these are drawn on has world coordinates, so it has parallax
    // against the wall behind it, the bulb's own glare passes in front of it,
    // and it reprojects for Ray Reconstruction like every other surface. v1 made
    // the same call about the stack badge beside the held tool -- its note reads
    // "drawn INTO the image (user: not HTML in the corner)" -- and could only
    // fake the perspective with a tilt and a shear, because a raymarcher that
    // shades one hit has nowhere to put a second surface. A path tracer does.
    //
    // THE COLOUR HAD TO BE MEASURED, AND THE FIRST TWO GUESSES WERE THE SAME
    // MISTAKE. In the room the walls were white at 226 lit to a mean of 180, so
    // a label had to be BRIGHTER THAN THE WALL to read at all -- paint of any
    // colour on a white wall under one lamp comes out a grey. Both early cuts
    // just turned the number up, and both rendered as WHITE WORDS WITH A
    // COLOURED FRINGE.
    //
    // What was wrong is not the level, it is which channels got it. kBtnRgb is
    // an sRGB colour, so the way to make a radiance out of it is to LINEARISE it
    // -- and sRGB is a steep curve down low, so (214, 58, 58) is not
    // (1.00, 0.27, 0.27) of light but (1.00, 0.06, 0.06). Scaling the encoded
    // numbers had been handing the off-channels four times too much, and up on
    // the tone map's shoulder the top channel is compressed hard while those
    // off-channels are not, so the three of them converge and the hue is
    // squeezed out. That is the whole reason the level below is applied to a
    // LINEARISED colour and normalised on its brightest channel.
    //
    // -- AND THE LEVEL ITSELF WENT UP WHEN THE ROOM CAME DOWN ---------------
    //
    // 2.0 was measured against a wall. The panel stands in the open now and the
    // thing behind a word is usually SKY, which the tone map puts far higher
    // than any interior -- so the same ink that beat a wall lost to a cloud.
    // Measured off --room --spawn 7, ink luma against the luma right beside it:
    //
    //     gain   quit          back          discord       saturation
    //     2.0    +4            +30           +63           0.23 / 0.17 / 0.26
    //     4.0    +17           +34           +85           0.18 / 0.13 / 0.19
    //     8.0    +31           +27           +65           0.12 / 0.06 / 0.17
    //     16.0   +41           +38           +104          0.07 / 0.05 / 0.09
    //
    // GREEN IS THE ONE THAT DECIDES IT. A saturated green is the highest-luma
    // hue there is -- 0.7152 of the luma weight is in that channel -- so "back"
    // was rendering at 205 against a sky at 212 and simply was not there. It is
    // also the first word to go white as the level rises, and by 8.0 it has
    // (saturation 0.06, which is a grey).
    //
    // 4.0 IS WHERE BOTH HOLD: every word clears its background, and all three
    // still read as their button's colour rather than as white. Past that the
    // contrast column barely moves and the saturation column falls off a cliff,
    // which is the same failure the paragraph above describes arriving by a
    // different road.
    static constexpr float kLabelNits = 4.0f;
    //
    // THE BUTTON'S OWN COLOUR, out of World::kBtnRgb, so a label can never name
    // the wrong button.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // HOW MANY OF THE THING IN YOUR HAND, WRITTEN BESIDE IT.
    //
    // (user 2026-09-14: "import the stacked item numbers next to the hand held
    // object. it should be setup similar to the current main menu floating
    // text.")
    //
    // THE SAME GLYPHS THE ROOM'S LABELS USE, which is what that sentence asks
    // for and is also the cheap answer: V2Holo already draws world-space text
    // through the tracer, so this is one slot and one call. v1 does it the
    // other way -- a badge blitted at the held model's projected corner, with
    // its own lane in the uniform buffer and its own glyph table in the blit
    // shader -- and none of that machinery has to exist here.
    //
    // WHERE IT GOES. The held item is a VIEWMODEL: it lives in camera space and
    // has no world position of its own. HeldXform::cam is its offset along the
    // camera's three axes, which is exactly what the Q drop already converts to
    // a world point, so the badge is that point pushed out along camRight.
    //
    // ...AND IT FACES THE CAMERA BECAUSE ITS PAGE IS THE CAMERA'S. right and up
    // are camRight/camUp, so the number is square to the eye at every angle --
    // a label on the panel is on a wall and this one is not.
    //
    // HIDDEN BELOW TWO. "x1" next to an axe is noise, and it is the state
    // almost everything in the kit is in almost always.
    // -----------------------------------------------------------------------
    // -- ...AND ALL FOUR OF THEM ARE LIVE (user 2026-09-14: "let me adjust the
    //    display number just like how we were able to in v1") ---------------
    //
    // v1 tunes this badge from a panel with exactly four sliders -- SB_K is
    // ['x', 'y', 'size', 'tilt'] -- and these are the same four in world terms:
    // across the view, up it, the glyph cell, and a roll about the view axis.
    //
    // NOT `static constexpr` ANY MORE, and that is the whole change: a number
    // you can only edit by rebuilding is not one you can judge, because the
    // thing you are judging is how it sits next to a model that is bobbing.
    // They start where they were tuned, so a session that never opens the panel
    // sees what a session before it did.
    //
    // HALF THE SIZE IT WAS (same message): 0.022 -> 0.011. v1's default `size`
    // is 1 against a badge the blit draws at a fixed pixel scale, so there is
    // no number to carry over -- this one was chosen by eye against a 0.9 m
    // tool and was chosen too big.
    // -- AND THEY ARE PER ITEM NOW (user 2026-09-14) -----------------------
    //
    // The note that stood here said this was deliberately NOT per item: "this
    // one hangs off the model's own measured half-width, so the same four
    // numbers frame every tool in the kit. If one item ever needs its own, the
    // shape of it is v1's sbCfgs table."
    //
    // TWO DID, IN THE SAME MESSAGE. The seed and the wheat came back with
    // different numbers -- the seed up 0.205 and the wheat up 0.313 and ACROSS
    // -0.145, on the other side of the model. The half-width is a box, and a
    // stalk held upright and a handful of seed are not the same shape inside
    // one; no single offset frames both.
    //
    // A ROW PER TOOL, defaulted to the base. That is v1's sbCfgs exactly, and
    // its own fallback is the same: an id with nothing of its own reads the
    // base rather than minting an entry from a render loop.
    struct StackCfg {
        float cell = 0.011f;    // glyph cell, in metres
        float across = 0.100f;  // clear of the model's right edge
        float up = 0.060f;      // ...and up from its middle
        float tilt = 0.000f;    // a roll about the view axis, radians
    };
    std::vector<StackCfg> stackCfg_;

    // -----------------------------------------------------------------------
    // THE BAKED ROWS -- what the panel's tuning looks like once it has landed.
    //
    // (user 2026-09-14, giving both directly: "seed position: ... stackUpM_ =
    // 0.205f ..." and "wheat position: ... stackAcrossM_ = -0.145f, stackUpM_ =
    // 0.313f".)
    //
    // KEYED BY NAME, NOT BY SLOT. A slot is where a thing happens to sit in the
    // wheel in this build -- the wheat moved from 4 to 5 when the hoe was added
    // -- and a bake keyed by that would silently start framing the bow. v1
    // keys sbCfgs by ITEM_NAMES for the same reason.
    //
    // ANYTHING NOT LISTED TAKES StackCfg's OWN DEFAULTS, which is every tool:
    // a tool is never held in a stack of two, so its row would never be seen.
    // Only things you can gather need one.
    struct StackBake {
        const char *name;
        StackCfg cfg;
    };
    static const std::vector<StackBake> &stackBakes() {
        static const std::vector<StackBake> t = {
            //          cell     across      up      tilt
            {"seeds", {0.0110f, 0.100f, 0.205f, 0.000f}},
            {"wheat", {0.0110f, -0.145f, 0.313f, 0.000f}},
            {"steak", {0.0110f, 0.056f, 0.260f, 0.000f}},
        };
        return t;
    }

    // Applied once, after the kit is built -- the only moment every tool has a
    // name AND a slot. A name in the table that is not in the kit is ignored
    // rather than an error: the table outlives any one build of the kit.
    void applyStackBakes() {
        stackCfg_.assign(size_t(held_.count()), StackCfg{});
        for (const StackBake &b : stackBakes())
            for (int i = 0; i < held_.count(); ++i)
                if (std::string(held_.tool(i).name) == b.name) stackCfg_[size_t(i)] = b.cfg;
    }

    // The row for a tool, minted on first use. Never called from the render
    // loop with an id the kit does not have -- see stackOf below, which is.
    StackCfg &stackFor(int tool) {
        if (stackCfg_.size() < size_t(held_.count())) stackCfg_.resize(size_t(held_.count()));
        return stackCfg_[size_t(tool)];
    }
    // ...and the read-only side, which IS called from the render loop.
    const StackCfg &stackOf(int tool) const {
        static const StackCfg kBase;
        return (tool >= 0 && size_t(tool) < stackCfg_.size()) ? stackCfg_[size_t(tool)] : kBase;
    }
    // NOT a slider: how bright a label is was settled for the room's own
    // words and there is no reason for this one to differ. See kLabelNits.
    static constexpr float kStackNits = 3.0f;
    // How long the badge's pop runs and how far it swells at the top of it.
    // A quarter of a second is about as long as a flourish can be before it
    // starts reading as the number being wrong; 45% is enough to catch the eye
    // at the edge of vision, which is where the hand is.
    static constexpr float kStackPopSec = 0.25f;
    static constexpr float kStackPopSwell = 0.45f;

    void setStackBadge() {
        V2Holo &h = tracer_.holo[kHoloHand];
        h = V2Holo{};
        // NOTHING IN THE ROOM. The panel owns the screen there and a number
        // hanging in front of it is the hand intruding on a menu.
        if (pauseOpen_ || menuOpen_ || !held_.ready() || !held_.shown) return;
        const int sel = held_.selected();
        if (sel < 0) return;
        const Tool &t = held_.tool(sel);
        // FORCED WHILE THE PANEL IS OPEN, so there is something to aim the
        // sliders at -- almost everything in the kit sits at one.
        const int n = (stackPanelOpen_ && stackPanelForce_) ? maxi(2, t.stack) : t.stack;
        // -- THE POP IS ARMED ABOVE THE BADGE'S OWN CUT-OFF -----------------
        //
        // (user 2026-09-15: "have the stack 'pop up' as well. a smooth pop up
        // animation".)
        //
        // A BADGE ONLY EXISTS FROM TWO UP, and that is exactly why this cannot
        // live below the return. Tracked there, the count 1 is never seen --
        // so picking up the SECOND of something, which is the moment the number
        // first appears and the most worth announcing, would arrive as "a tool
        // I have no previous count for" and pass in silence.
        //
        // NOT ON THE FIRST SIGHT OF A TOOL, though: selecting a stack you were
        // already carrying is not a change to it, and popping there made every
        // scroll of the kit twitch. So the tool has to match as well as the
        // count having moved.
        if (n != stackPopN_ || sel != stackPopTool_) {
            if (n != stackPopN_ && sel == stackPopTool_) stackPopT0_ = simMs_;
            stackPopN_ = n;
            stackPopTool_ = sel;
        }
        if (!t.carried || n < 2) return;

        char word[8];
        std::snprintf(word, sizeof(word), "x%d", n);
        // THE HAND'S OWN WORLD POINT -- the same three terms the Q drop uses.
        const Vec3 rt = camRight(), up = camUp(), fw = forward();
        const Vec3 hand =
            pos_ + rt * lastHeld_.cam.x + up * lastHeld_.cam.y + fw * lastHeld_.cam.z;
        // Half the model's own width, so a long item does not wear its number
        // over itself. The box is in the tool's voxels; VOXEL_M turns it into
        // metres, and the pose scale is already in lastHeld_.
        const StackCfg &cfg = stackOf(sel);
        // -- ...AND IT POPS WHEN IT CHANGES ---------------------------------
        //
        // (user 2026-09-15: "have the stack 'pop up' as well. a smooth pop up
        // animation".)
        //
        // A NUMBER THAT CHANGES SILENTLY IS A NUMBER YOU MISS. Picking up the
        // sixth of something rewrites one glyph in the corner of the hand and
        // nothing about the frame says it happened -- the same complaint the
        // gold at ten answered, one step earlier.
        //
        // ARMED ON THE CHANGE, NOT ON THE PICKUP, so every route to the count
        // is covered by construction: a walk-over absorb, a break that pays
        // out, a craft, a bite taken out of a stack of meat. There is no list
        // of ways to gain an item to keep in step with, because this watches
        // the number itself.
        //
        // sin(pi*u) IS THE CURVE: it leaves 1.0, reaches the full swell at the
        // halfway point and comes back to 1.0, so the badge never steps -- and
        // its slope at u=0 is steep, which is what separates a POP from a
        // throb. Scaling the CELL is the whole of it: holoSetWord derives the
        // word's origin from the cell, so the number grows about its own middle
        // rather than sliding off the hand.
        float cellM = cfg.cell;
        {
            const float u = float((simMs_ - stackPopT0_) * 0.001) / kStackPopSec;
            if (u >= 0.0f && u < 1.0f) cellM *= 1.0f + kStackPopSwell * sinf(PI * u);
        }
        const float halfW = 0.5f * float(t.sx) * VOXEL_M + cfg.across;
        const Vec3 at = hand + rt * halfW + up * cfg.up;
        // THE TILT IS A ROLL OF THE PAGE, not of the glyphs: holoSetWord takes
        // the two axes the word is laid out on, so turning both of them in
        // their own plane turns the whole number. v1's `tilt` is the same
        // quantity -- its default is -0.26, a slight lean, and this starts at
        // zero because a world-space label has the model's own perspective on
        // it already.
        const float c = cosf(cfg.tilt), sn = sinf(cfg.tilt);
        const Vec3 rt2 = rt * c + up * sn;
        const Vec3 up2 = up * c - rt * sn;
        // -- GOLD AT THE CAP (user 2026-09-15: "have 10x be the max stacked
        //    item number just like in v1. make the text turn gold when its
        //    maxed at 10") ------------------------------------------------
        //
        // The cap was already v1's ten -- HeldItem::kStackMax, and HeldItem::take
        // clamps to it -- so the only thing missing was being TOLD. A number
        // that stops climbing and says nothing reads as a bug in the counter.
        //
        // The same warm gold the sparks settled on, at the label's own
        // brightness: this is a colour, not a second light.
        const bool full = n >= HeldItem::kStackMax;
        const float3 tint = full ? float3(kStackNits * 1.00f, kStackNits * 0.78f,
                                          kStackNits * 0.22f)
                                 : float3(kStackNits, kStackNits, kStackNits);
        holoSetWord(h, word, float3(at.x, at.y, at.z), float3(rt2.x, rt2.y, rt2.z),
                    float3(up2.x, up2.y, up2.z), cellM, tint);
    }

    void setRoomLabels() {
        for (int b = 0; b < 3; ++b) setRoomLabel(b, World::buttonLabel(b));
    }

    // ONE WORD OVER ONE BUTTON, in that button's own colour. Split out of
    // setRoomLabels so the green one can say something other than "back" while
    // it waits -- see leaveRoom.
    void setRoomLabel(int b, const char *word) {
        if (b < 0 || b >= 3) return;
        // Normalised on its brightest channel and then scaled: the three
        // colours were chosen as surfaces and have quite different lumas, so
        // scaling them as they stand would make "back" half the brightness of
        // "quit" for no reason anybody could see.
        float lin[3], m = 0.0f;
        for (int k = 0; k < 3; ++k) {
            lin[k] = srgbToLinearF(float(World::kBtnRgb[b][k]) / 255.0f);
            m = maxf(m, lin[k]);
        }
        const float s = (m > 0.0f) ? (kLabelNits / m) : 0.0f;
        const float3 tint(lin[0] * s, lin[1] * s, lin[2] * s);
        // ALL THREE OFF THE PANEL, so a word cannot end up facing a way its
        // button is not. The page is read along the panel and stands up it.
        const Vec3 mid = panelLabelMid(b);
        const Vec3 rt = panelRight_;
        const Vec3 up = panelUp_;
        holoSetWord(tracer_.holo[b], word, float3(mid.x, mid.y, mid.z),
                    float3(rt.x, rt.y, rt.z), float3(up.x, up.y, up.z),
                    World::kBtnLabelCellM, tint);
    }

    // Back to the wood from the asset deck: the position, heading and flight
    // saved on the way in. Extracted because the pause room has to be able to
    // do it too -- see the note at the editor's key.
    // -----------------------------------------------------------------------
    // WHAT STANDS ON THE DECK.
    //
    // THE PORCUPINE HAS THE MIDDLE (user 2026-09-14: "put the porcupine on the
    // asset editor in the middle and remove the bunny from it"), and it is not
    // standing there to be looked at -- the editor's tools operate on it.
    // WHICH animal that is lives in one table, kEditSubjects in
    // render/assetedit.h; this function only clears the deck and lets the
    // editor publish onto it.
    //
    // THE BUNNY IS GONE FROM IT, and the despawn below is most of how: the
    // wild rabbits were already cleared here, and with the editor no longer
    // publishing one either, there is no rabbit left on the deck at all. Its
    // STRIPS are still compiled and still played -- the rabbits in the wood hop
    // out of them -- they are simply not a subject this deck offers now.
    //
    // THE CARDINAL STEPPED ASIDE RATHER THAN OUT when the deck first grew a
    // subject; it has since been removed outright (2026-09-13). See the body.
    //
    // AND EVERY ANIMAL LETS GO OF ITS SLOTS. The editor draws through
    // kBunnySlot0, so the ten rabbits that were following you round the pines
    // have to be cleared or nine keep standing wherever they were -- four
    // kilometres away and, on this deck's coordinates, under the floor.
    //
    // THE MARCHERS NOW MATTER TWICE OVER. Their band was already cleared for
    // that reason; it also has to be, because the porcupine the editor stands
    // in the middle is drawn from the SAME STRIP the wild ones walk on. Leave
    // them live and the deck holds two porcupines out of one animation, only
    // one of which is posed by the bake you are editing.
    // update() stops ticking them while the stage is up, so nothing refills.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHERE YOU ARRIVE ON THE DECK, AND WHY IT IS NOT STRAIGHT ON.
    //
    // It used to be three metres down +Z with yaw 0 -- square to the subject,
    // for the sound reason that yaw 0 looks down -Z here (Camera::direction) so
    // standing on the other side put it behind the camera. That reason is
    // untouched; what changed is that there are HANDLES on the deck now, and a
    // square-on camera is the one angle from which a three-axis gizmo does not
    // work.
    //
    // A TRANSLATE ARROW IS DRAGGED ALONG ITS SCREEN DIRECTION, so an axis
    // pointing straight at the eye has no screen direction and cannot be
    // pulled at all. At yaw 0 that is the Z arrow -- and Z is the axis a bound
    // travels along, so the handle you reach for first was the dead one. The
    // pitch ring was edge-on for the same reason, a violet bar rather than a
    // ring. Both were photographed before this changed.
    //
    // A THREE-QUARTER VIEW IS THE STANDARD ANSWER and every 3D tool opens on
    // one. Off the corner, all three axes have a screen direction and both
    // rings read as rings.
    //
    // THE AIM IS COMPUTED, NOT TYPED. The eye is two metres over the body it is
    // carried by and the subject is a rabbit on the floor, so a hand-picked
    // pitch is a guess that has to be re-guessed the moment anything moves. Ask
    // for the angles that point at the subject and they are right by
    // construction -- which is also what makes the eye height below the only
    // number here worth choosing by eye.
    // -----------------------------------------------------------------------
    void standOnDeck() {
        const Vec3 c = World::stageCentre();
        // A COUPLE OF METRES OFF EACH OF THE TWO NEAR CORNERS, which is what
        // gives the three-quarter view a gizmo needs: at yaw 0 the Z arrow
        // points straight at the eye and cannot be dragged at all.
        const float back = 2.35f;
        // -- ON ITS FEET, NOT HOVERING --------------------------------------
        //
        // "let the player stand on the asset editor platform." It used to
        // arrive FLYING, and not as a choice: the walk collider reads the
        // terrain, the terrain under the deck is 640 m down, and a walking
        // body would simply have fallen off the world. Flight was the only
        // thing holding anybody up. See clampToStage, which is the floor --
        // and the fence -- that makes standing possible.
        //
        // THE EYE HEIGHT IS NO LONGER A COMPOSITION. It was 1.30 m, measured
        // off a render because the deck's own horizon ran through the rabbit at
        // 0.95; it is now the player's own 2.00 m, because a person standing on
        // a platform is exactly what was asked for. The subject sits lower in
        // the frame than it did, which is what looking down at something on the
        // floor looks like -- and the aim below still points at the middle of
        // it rather than at the deck.
        player_.pos = Vec3(c.x - back, c.y, c.z + back);
        player_.fly = false;
        player_.onGround = true;
        player_.vy = 0.0f;
        pos_ = player_.eyePosition();
        // The middle of the rabbit rather than its feet, so it sits on the
        // crosshair instead of under it.
        lookAt(Vec3(c.x, c.y + 0.25f, c.z));
    }

    void stageSubject() {
        const Vec3 c = World::stageCentre();
        bunnies_.despawnAll();
        bunnies_.publish(world_, kBunnySlot0);        // every slot, written empty
        // ...AND THE MARCHERS AND THE BEES TOO, or the deck keeps whatever the
        // band was holding when you pressed the key. despawnAll clears the
        // POPULATION; only a publish clears the BAND, and the live path stops
        // writing these runs the moment the stage is up.
        bunnies_.publishSkunks(world_, kMarchSlot0);
        bees_.despawnAll();
        bees_.publish(world_, kBeeSlot0);
        critters_.despawnAll();
        critters_.publish(world_, kCritterSlot0);
        edit_.enter(world_, c);
        edit_.publish(world_, kBunnySlot0);
        // -- AND NOTHING ELSE STANDS ON IT ------------------------------
        //
        // The cardinal is gone (user 2026-09-13: "remove the cardinal from the
        // asset editor"). assetedit.h's own note already explains why it was
        // the odd one out: it "was a cardinal that you could look at and
        // nothing else. That is a VIEWER" -- and once the deck grew real tools
        // the bird was a second subject competing with whatever you actually
        // came here to look at.
        //
        // unstage() still runs on the way out, so a cardinal placed by an older
        // build cannot be left standing on an empty deck.
    }

    void leaveStage() {
        edit_.leave();
        birds_.unstage();
        world_.setStage(false);
        player_.pos = woodPos_;
        player_.fly = woodFly_;
        yaw_ = woodYaw_;
        pitch_ = woodPitch_;
        player_.vy = 0.0f;
        pos_ = player_.eyePosition();
    }

    // -----------------------------------------------------------------------
    // ARRIVING AT THE BUILDING -- on the slab, on foot, facing it.
    //
    // standOnDeck's sibling, and the differences are the interesting part. The
    // deck has to be given its floor height as a constant because it IS a
    // constant; this level's ground is its own voxels, so the spawn asks the
    // asset's heightfield where the top of that column is (World::levelSpawn)
    // and a re-voxelised building moves the player with it rather than leaving
    // them buried in a slab that got thicker.
    //
    // ON FOOT rather than flying, which the deck cannot manage -- see the note
    // on the [O] handler, and Solid::interior for what makes the walk work in
    // a place the terrain function has never heard of.
    // -----------------------------------------------------------------------
    void standInLevel() {
        player_.pos = world_.levelSpawn();
        player_.fly = false;
        player_.onGround = true;
        player_.vy = 0.0f;
        yaw_ = world_.levelSpawnYaw();
        // The wood's own default rather than dead level -- Options::pitch is 7
        // and a body standing still looks very slightly up, not at its feet.
        pitch_ = 7.0f;
        pos_ = player_.eyePosition();
    }

    void leaveLevel() {
        world_.setLevel(false);
        player_.pos = woodPos_;
        player_.fly = woodFly_;
        yaw_ = woodYaw_;
        pitch_ = woodPitch_;
        player_.vy = 0.0f;
        player_.onGround = false;   // the wood's ground decides, not this
        pos_ = player_.eyePosition();
    }

    // -----------------------------------------------------------------------
    // THE FENCE ROUND THE LEVEL -- clampToStage's sibling, and it is here for
    // exactly the reason that one is.
    //
    // The walk asks the TERRAIN what is under the body, and what the terrain
    // says is under this slab is six hundred and forty metres of nothing. On
    // the platform the level's own voxels answer first and the terrain never
    // wins; step OFF the platform and they stop answering, and the body falls
    // the whole way into a wood it was supposed to have left -- streaming the
    // forest in behind it, which is the one thing a separate world is for
    // avoiding.
    //
    // So the slab's footprint is the edge of the world while you are here. The
    // asset IS the platform -- the .glb's biggest mesh by far is the slab, and
    // every wall stands inside it -- so clamping to the asset's own bounds is
    // clamping to the concrete, with no second description of where it is.
    // -----------------------------------------------------------------------
    void clampToLevel() {
        if (!world_.levelOn()) return;
        const float r = 0.32f;   // shoulders -- clampToStage's number
        Vec3 p = player_.pos;
        p.x = clampf(p.x, world_.levelMinX() + r, world_.levelMaxX() - r);
        p.z = clampf(p.z, world_.levelMinZ() + r, world_.levelMaxZ() - r);
        // ...AND A FLOOR UNDER THE WHOLE OF IT. The slab's underside is the
        // bottom of this world; nothing below it is anywhere.
        const float floorY = World::levelOrigin().y;
        if (p.y <= floorY) {
            p.y = floorY;
            player_.vy = 0.0f;
            player_.onGround = true;
        }
        player_.pos = p;
    }

    // -----------------------------------------------------------------------
    // THE PAUSE PANEL: THREE BUTTONS, IN THE WOOD, IN FRONT OF YOU.
    //
    // "Remove the esc room from the sky. Instead, put the 3 balls in front of
    // the player IN GAME, along with the floating text above the balls just
    // like how it was in the esc room. When the user hits escape, the 3 balls
    // appear in a line horizontally. Pressing esc again removes them."
    // (user 2026-09-14.)
    //
    // WHAT WENT. A sealed white box at (-4096, 2048, -4096) that REPLACED the
    // world in the structure, a teleport in and out of it, the saved wood
    // position that trip needed, a point light invented because a closed box
    // has no sun in it, and a six-plane collider because the walk cannot see a
    // room the terrain does not contain. All of it was in service of one idea
    // -- that a pause menu you can see the wood through is a HUD rather than a
    // place -- and the answer to that is simply that this one IS in the wood.
    //
    // THE PANEL IS PINNED WHERE YOU WERE LOOKING, ONCE. Not carried on the
    // camera: a panel that follows your head is a HUD again, and the whole
    // point of these being models in the world is that you can step round them,
    // that the sun lights them, and that a tree can stand between you and the
    // discord button. Walk away and they stay where you left them until you
    // press the key again.
    //
    // THE MOUSE IS TAKEN, exactly as the room took it, and for the same reason:
    // the buttons are picked by the CROSSHAIR -- see buttonUnderCrosshair -- and
    // the outer two sit well off centre, so a player who cannot turn their head
    // can only ever press the middle one.
    // -----------------------------------------------------------------------
    void setRoomOpen(bool on) {
        if (on == pauseOpen_) return;
        pauseOpen_ = on;
        // Nothing half-pressed carries across -- see tickButtons.
        btnPress_[0] = btnPress_[1] = btnPress_[2] = 0.0f;
        btnHeld_ = -1;
        btnPend_ = -1;
        if (!on) {
            tracer_.clearHolos();
            if (!captureBeforeRoom_ && looking_) setCapture(false);
            return;
        }

        // -- WHERE IT STANDS -------------------------------------------------
        //
        // A panel's width in front of the eye, level with it, and square to the
        // way you are facing. The heading is taken FLAT: pitch is deliberately
        // dropped, so looking at your feet when you press the key does not bury
        // the buttons in the ground or hang them over your head.
        const Vec3 f = forward();
        const float fl = sqrtf(f.x * f.x + f.z * f.z);
        const Vec3 flat = (fl > 1e-3f) ? Vec3(f.x / fl, 0.0f, f.z / fl) : Vec3(0.0f, 0.0f, -1.0f);
        panelUp_ = Vec3(0.0f, 1.0f, 0.0f);
        // -- forward x up, AND THE ORDER OF THE THREE IS WHY -------------
        //
        // It was up x forward, and that is the player's LEFT: facing -Z, which
        // is where the camera points at a yaw of zero, cross((0,1,0),(0,0,-1))
        // is (-1,0,0). The row came out MIRRORED -- discord, back, quit from
        // left to right, where the room's wall had always read quit, back,
        // discord -- and it is only visible because button 0 and button 2 are
        // different colours; a symmetric panel would have hidden it.
        //
        // forward x up gives (1,0,0) for the same facing, which is +X, which is
        // the player's right. Button 0 is then on the left where it was.
        panelRight_ = normalize(cross(flat, panelUp_));
        // The face looks BACK at you, so the press travels away from you.
        panelInto_ = flat;
        panelAt_ = player_.eyePosition() + flat * kPanelReachM;

        captureBeforeRoom_ = looking_;
        if (!looking_) setCapture(true);
        setRoomLabels();
    }

    // WHERE BUTTON b STANDS, and the one place that says so. The picker, the
    // publisher and the words above them all read this: three descriptions of
    // one object is the drift World::buttonAt was written to prevent, and the
    // panel moving into the world does not make that less true.
    Vec3 panelButtonAt(int b) const {
        return panelAt_ + panelRight_ * (float(b - 1) * World::kBtnSpacingM);
    }
    // ...and where its word hangs: a hand's width above the button it names.
    Vec3 panelLabelMid(int b) const {
        return panelButtonAt(b) + panelUp_ * World::kBtnLabelRiseM;
    }


    // -----------------------------------------------------------------------
    // WHICH BUTTON THE PLAYER IS LOOKING AT, or -1.
    //
    // A ray against three discs on a known plane, in host code -- there is no
    // reason to ask the acceleration structure about three circles whose
    // centres this file already knows, and doing it here means the highlight
    // and the click cannot disagree about which one is under the cursor.
    // -----------------------------------------------------------------------
    int buttonUnderCrosshair() const {
        if (!pauseOpen_) return -1;
        const Vec3 o = pos_;
        const Vec3 d = Camera::direction(yaw_, pitch_);
        const float r = World::buttonRadiusM();
        // A RAY AGAINST THREE SPHERES. It was a ray against three discs on the
        // wall plane, which was right while the buttons were painted on it --
        // now they are balls standing proud of it, and a plane test would let
        // you press one by looking at the wall BESIDE it from far enough to the
        // side. The nearest root wins, so a ball in front of another cannot be
        // pressed through.
        int best = -1;
        float bestT = 1e9f;
        for (int b = 0; b < 3; ++b) {
            const Vec3 c = panelButtonAt(b);
            const float ox = o.x - c.x, oy = o.y - c.y, oz = o.z - c.z;
            const float half = ox * d.x + oy * d.y + oz * d.z;
            const float cq = ox * ox + oy * oy + oz * oz - r * r;
            const float disc = half * half - cq;
            if (disc < 0.0f) continue;
            const float sq = sqrtf(disc);
            float t = -half - sq;
            if (t <= 0.0f) t = -half + sq;      // standing inside it
            if (t <= 0.0f || t >= bestT) continue;
            bestT = t;
            best = b;
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // THE ROOM'S PHYSICS, WHICH IS SIX PLANES AND THREE CYLINDERS.
    //
    // The walk collider reads the TERRAIN -- see walkWorld -- and the pause
    // room is not terrain: it is its own bottom-level structure at -4096, 2048,
    // where there is no chunk and never will be. So the collider finds nothing
    // under the player and they fall, for ever, out of the bottom of a sealed
    // box. Flight hid it; walking is what was asked for.
    //
    // A BOX IS TWO CORNERS, and that is nearly the whole of it. Applied AFTER
    // the player has moved, so the ordinary walk -- momentum, the head bob, the
    // crouch, the jump -- all still happen, and this only says where they stop.
    //
    // THE BUTTONS ARE SOLID TOO. They are balls standing two thirds proud of
    // the wall at chest height, so without this you can walk into the middle of
    // one and press it from inside, looking at the back of its far side.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // IS THE COLUMN AT (x, z) UNDER WATER?
    //
    // The three calls WaterField::rebuild makes, in the same order and with the
    // same meaning -- this is the engine's single definition of "wet", and every
    // creature that needs to know asks it the same way. A second definition is
    // how a bunny and a lily pad end up disagreeing about where a lake is.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // ...AND HOW HIGH THAT WATER STANDS, for anything that has to fly OVER it.
    //
    // wetColumnAt answers "is this a lake", which is all a walker or a spawn
    // site needs. A flyer needs the number: `ground` is the top of the SOLID
    // column, which in a lake is the BED, and a creature holding a metre over
    // that is a metre above the bottom of the lake. See Critters::flyFloor and
    // the ladybug that was reported swimming.
    //
    // Deliberately NOT -infinity on a dry column: the caller takes a max
    // against the ground and a real sentinel keeps that honest without needing
    // a second flag.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // IS THE GROUND HERE A BEACH?
    //
    // The surface material, asked the way the mesher asks it -- topMaterial of
    // the column's own height -- so this cannot disagree with what is drawn.
    // Both the single sand id and the four grains the device spreads it over,
    // because either can be the surface of a shore. See Bunnies::blocked.
    // -----------------------------------------------------------------------
    bool sandAt(float x, float z) {
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        TerrainMemo memo;
        const int h = world_.terrain.heightVox(ci, cj, memo);
        return isSand(world_.terrain.topMaterial(ci, cj, h, memo));
    }

    float waterTopAt(float x, float z) {
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        TerrainMemo memo;
        const int line =
            world_.terrain.lakeLineAt(world_.terrain.wx(ci), world_.terrain.wx(cj), memo);
        if (line == VoxelTerrain::kNoWaterVox) return -1.0e30f;
        const int h = world_.terrain.heightVox(ci, cj, memo);
        if (!world_.terrain.wetColumn(ci, cj, h, line, memo)) return -1.0e30f;
        return float(line + 1) * VOXEL_M;
    }

    bool wetColumnAt(float x, float z) {
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        TerrainMemo memo;
        const int line =
            world_.terrain.lakeLineAt(world_.terrain.wx(ci), world_.terrain.wx(cj), memo);
        if (line == VoxelTerrain::kNoWaterVox) return false;
        const int h = world_.terrain.heightVox(ci, cj, memo);
        return world_.terrain.wetColumn(ci, cj, h, line, memo);
    }

    // -----------------------------------------------------------------------
    // A LOOSE BODY, AS A SWING.
    //
    // The one place a DebrisHit becomes something the tools, the audio and the
    // log can all read -- so a felled tree answers every question they ask in
    // exactly the words a standing one does. What it is MADE of comes out of
    // the body (World::DebrisTakes) rather than out of its material id: a
    // boulder's grey is a palette entry and not mat::ROCK, so the material
    // could never have told anybody which tool to use.
    // -----------------------------------------------------------------------
    static Swing looseSwing(const DebrisHit &h, const Vec3 &eye, const Vec3 &dir) {
        Swing s;
        s.hit = true;
        s.kind = Swing::Loose;
        s.debris = h.slot;
        s.takesAs = h.takes == kDebrisWood   ? Takes::Wood
                    : h.takes == kDebrisSoil ? Takes::Soil
                                             : Takes::Stone;
        // ...AND WHETHER IT IS A MUSHROOM, which Takes above cannot say: a cap
        // answers to the axe AND the pick, and that enum holds one tool's worth
        // of material. Swing::soft is the same flag a STANDING cap arrives with
        // (swingRayModels, off Solid::bouncy), so toolTakes needs one rule for
        // both and the fallen half of a mushroom cannot end up cuttable by a
        // different tool than the standing half. See kDebrisSoft.
        s.soft = h.takes == kDebrisSoft;
        s.dist = h.t;
        s.point = h.point;
        s.material = h.mat;
        s.eye = eye;
        s.dir = dir;
        s.reach = swingReachM(dir);
        return s;
    }

    // -----------------------------------------------------------------------
    // THE DECK IS A FLOOR AND A FENCE.
    //
    // clampToRoom, asked of the other place that is not the world. The room is
    // a box and this is a plate, so it is the same function with the walls and
    // the ceiling taken out -- and one thing put in that the room gets for
    // free.
    //
    // THE FENCE, WHICH THE ROOM GETS FROM ITS WALLS. The deck has none, and it
    // has nothing underneath it either: one step in any direction is six
    // hundred metres of clear air and no ground at the bottom of it. So the rim
    // stops you. An eight metre deck is as far as walking round a model ever
    // goes, and flight is still there and still unclamped upwards for the view
    // from above.
    //
    // IT RUNS WHILE FLYING TOO. A body that can be flown through its own floor
    // is a body that can be left under the deck when flight goes off, which is
    // the one way back into the fall this exists to prevent.
    // -----------------------------------------------------------------------
    void clampToStage() {
        if (!world_.staged()) return;
        const float r = 0.32f;   // shoulders -- clampToRoom's number
        Vec3 p = player_.pos;
        p.x = clampf(p.x, World::stageMinX() + r, World::stageMaxX() - r);
        p.z = clampf(p.z, World::stageMinZ() + r, World::stageMaxZ() - r);
        const float floorY = World::stageFloorY();
        if (p.y <= floorY) {
            p.y = floorY;
            player_.vy = 0.0f;
            player_.onGround = true;
        }
        player_.pos = p;
    }

    // -- AND NOTHING CLAMPS THE PLAYER ANY MORE ---------------------------
    //
    // clampToRoom was here: six planes and three cylinders, the pause room's
    // whole physics, written because the walk collider reads the TERRAIN and
    // the room was a separate structure four kilometres from the nearest chunk
    // -- so without it the player fell out of the bottom of a sealed box.
    //
    // The panel stands in the wood now, on ground the walk already understands,
    // so there is nothing left to clamp to. YOU CAN WALK THROUGH THE BUTTONS,
    // deliberately: they are 50 cm of light at eye height in the middle of a
    // path, and a pause menu that can shove you off a ledge or wedge you
    // against a trunk would be a worse thing than one you can step through.

    // -----------------------------------------------------------------------
    // THE BUTTONS' OWN CLOCK.
    //
    // A LINEAR RAMP, NOT AN EXPONENTIAL EASE. Everything else that moves in
    // this engine is eased, because everything else is an animal or a camera
    // and those do not start instantly. A button is a piece of plastic under a
    // finger: it goes down at the speed the finger pushes it, and -- more to
    // the point -- an exponential never actually ARRIVES, so there would be no
    // frame that is the bottom of the travel to hang the action on.
    //
    // DOWN FAST AND BACK SLOWER. 80 ms down is about as long as a real switch
    // takes and short enough that the click still feels like the cause; the
    // spring back is twice that because nobody is pushing it any more.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE CRT POWER-OFF, AND THEN THE DOOR.
    //
    // ON THE WALL CLOCK, NOT THE SHOT CLOCK. Everything else in this frame is
    // fed the scaled dt so a scripted camera move and a live one agree; this
    // wants the real one, because it is a thing happening to the SCREEN rather
    // than to the world, and a paused or slowed game should still take exactly
    // as long to switch off.
    //
    // THE EXIT IS ONE FRAME LATE ON PURPOSE. shutdown is called when the ramp
    // has already been at 1 for a frame, so the fully black frame is presented
    // before the window goes -- otherwise the last thing on screen is the
    // second-to-last frame of the collapse, which is a bright dot.
    // -----------------------------------------------------------------------
    void tickQuit(float wallDt) {
        if (!quitting_) return;
        const bool wasDone = quitT_ >= 1.0f;
        quitT_ = minf(1.0f, quitT_ + wallDt / kQuitFadeSec);
        tracer_.crt = quitT_;
        if (wasDone) shutdown(0);
    }

    void tickButtons(float dt) {
        btnClock_ += dt;
        for (int b = 0; b < 3; ++b) {
            const float want = (btnHeld_ == b) ? 1.0f : 0.0f;
            const float step = dt / ((btnHeld_ == b) ? kBtnDownSec : kBtnUpSec);
            btnPress_[b] += clampf(want - btnPress_[b], -step, step);
        }

        // CONTACT. The order matters: the button is put at the bottom of its
        // travel and PUBLISHED there before the action runs, so the last frame
        // the player sees -- which for the red one is the last frame there is
        // -- has the button fully down in it.
        if (btnPend_ >= 0 && btnClock_ >= btnAt_) {
            const int b = btnPend_;
            btnPend_ = -1;
            btnHeld_ = -1;              // ...and it springs back from here
            btnPress_[b] = 1.0f;
            publishPanel(true);
            pressButton(b);
            return;
        }
        publishPanel(pauseOpen_);
    }

    // The three of them, where the panel put them. One call, so the press ramp
    // and the panel's pose cannot be handed over from two different places.
    void publishPanel(bool show) {
        const Vec3 at[3] = {panelButtonAt(0), panelButtonAt(1), panelButtonAt(2)};
        world_.publishButtons(show, btnPress_, at, panelRight_, panelUp_, panelInto_);
    }

    // 0 red = quit, 1 green = back to the wood, 2 blue = the Discord server.
    // -----------------------------------------------------------------------
    // SWITCH THE TUBE OFF, AND LET tickQuit CLOSE THE DOOR.
    //
    // Not shutdown(0). The window would be gone on the same frame the press
    // landed, and the last thing anybody saw would be a button halfway down --
    // which is the same complaint the press animation was added to fix, one
    // level further in.
    //
    // ONE FUNCTION BECAUSE THERE ARE TWO WAYS OUT AND THEY MUST NOT DIFFER:
    // the red button on the wall, and the third ESC. A quit that collapsed the
    // picture when pressed with the mouse and blinked out when pressed with the
    // keyboard would read as a bug in whichever one you found second.
    //
    // IDEMPOTENT. ESC held down, or pressed again while the picture is going,
    // must not restart the collapse from full brightness.
    // -----------------------------------------------------------------------
    void beginQuit() {
        if (quitting_) return;
        quitT_ = 0.0f;
        quitting_ = true;
    }

    void pressButton(int b) {
        if (b == 0) {
            beginQuit();
        } else if (b == 1) {
            leaveRoom();
        } else if (b == 2) {
            openDiscord();
        }
    }

    // -----------------------------------------------------------------------
    // THE GREEN BUTTON PUTS THE PANEL AWAY, and that is the whole of it.
    //
    // IT USED TO BE A JOURNEY. The room stood four kilometres off the wood, so
    // "back" was a teleport, and a teleport is only safe once the chunks it
    // lands in exist. The press therefore armed a WAIT -- the button held at
    // the bottom of its travel, the word over it changed to "loading",
    // tickRoomExit letting the player out on the first frame chunkAt(woodPos_)
    // was true with nothing missing and nothing in flight, and a 15 s giveup so
    // a stalled streamer could not be a room with no door. That was written
    // for a real fault: ESC in the first seconds of a launch, or out of the
    // asset deck, used to drop the player into an empty grey hole.
    //
    // THERE IS NOWHERE TO COME BACK FROM NOW. The panel hangs in the wood the
    // player never left, on chunks they were standing in a moment ago, so the
    // wait would wait on nothing -- and worse than nothing: woodPos_ is now
    // only ever the ASSET DECK's saved spot, so a panel opened by somebody who
    // has not visited the deck would test the ORIGIN, find no chunk there, and
    // sit on "loading" for the full fifteen seconds before letting go.
    //
    // THE DECK'S DOOR IS UNCHANGED and still teleports on the frame it is
    // pressed -- leaveStage never had this wait, it borrowed the room's by
    // being reachable through it. If an empty ring is ever seen stepping off
    // the deck, the wait belongs THERE, keyed on the position leaveStage is
    // about to restore, and the shape of it is in this note.
    // -----------------------------------------------------------------------
    void leaveRoom() { setRoomOpen(false); }

    // THE ONE PLACE THE LINK LIVES. docs/discord-integration.md is where it
    // came from; the browser is the shell's business, not ours.
    void openDiscord() const {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", "https://discord.gg/AtW5fWZtSG", nullptr, nullptr,
                      SW_SHOWNORMAL);
#endif
        std::printf("  room     opening https://discord.gg/AtW5fWZtSG\n");
        std::fflush(stdout);
    }

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

        const Vec3 before = player_.eyePosition();
        player_.update(walkWorld(), move, sprint, jump, down, crouch, dt);
        clampToStage();
        clampToLevel();
        tickButtons(dt);
        pos_ = player_.eyePosition();

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
            const bool swinging =
                opt_.swingHold || (lmb && swingArmed_ && looking_ && !menuOpen_);
            // THE RIGHT BUTTON DRAWS, and only while the pointer is ours --
            // the same gate the swing has, and the JS engine's `locked`.
            // A SCRIPTED DRAW LETS GO ON A NAMED FRAME. --draw-hold alone pulls
            // and never fires, which photographs the draw; --shot-loose N is
            // what fires it, so a still of an arrow leaving is reproducible.
            const bool scripted =
                opt_.drawHold && (opt_.shotLoose < 0 || shotFrames_ < opt_.shotLoose);
            const bool drawing = scripted || (in.isMouseButtonDown(Input::MouseButton::Right) &&
                                              looking_ && !menuOpen_ && !holdLook_);
            float draw = 0.0f;
            const bool released = held_.update(dt, swinging, player_.bobAmp, drawing, &draw);
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
                if (loose(draw)) toolSfx_.loosed();
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
                    std::printf("v2: swing -> %s", kWhat[int(lastSwing_.kind)]);
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

    // -----------------------------------------------------------------------
    // WHAT A FELLED TREE ACTUALLY DOES, PRINTED, WITH NO WINDOW.
    //
    // This exists because the felling has now been "fixed" three times by
    // reasoning about it and has been wrong three times. The renderer cannot be
    // opened to look at it -- and should not be -- so the body is asked
    // directly: fell a tree, step the solver, and print where it is and what it
    // is doing, frame by frame.
    //
    // The three failures it is meant to tell apart, which look alike in a
    // sentence and not at all in a table:
    //
    //   IT DOES NOT MOVE          -- the pitch never leaves zero. Something is
    //                                holding it: a collider it cannot topple
    //                                off, or a body that never woke.
    //   IT BOBS                   -- y oscillates while the pitch stays put.
    //                                That is depenetration fighting gravity.
    //   IT FALLS AND KEEPS GOING  -- y runs away downward. Nothing under it.
    //
    // A fall that works looks like neither: the pitch runs from 0 to about 90
    // degrees over a second or two, y drops once and settles, and the speeds go
    // to nothing.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // DIG THE GROUND OUT FROM UNDER SOMETHING AND WATCH WHETHER IT FALLS.
    //
    // float_probe measures the GAP this leaves and proves the hole in the rule;
    // it cannot run the engine, so it cannot show the rule being obeyed. This
    // does: the real terrain, the real placements, the real dig, and
    // World::dropUndermined asked exactly where the swing path asks it.
    //
    // The pass condition is the user\'s rule, unedited -- nothing that has lost
    // the ground under it is still standing there.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === DIG TEST === -- what each tool takes, and what the ground is made of.
    //
    // WITH NO WINDOW, for the reason --fell-test and --float-test have none:
    // the alternative is putting the game on screen and swinging at a hillside
    // by hand, which proves one spot on one run and cannot be repeated after a
    // change. This asks the real swing ray, the real toolTakes and the real
    // World::dig, at a column picked from the terrain rather than chosen.
    //
    // THREE THINGS ARE BEING PROVED, and they are the three that were asked for:
    //
    //   1. THE PROFILE. A column is turf, then a few voxels of soil, then a
    //      hundred voxels of stone, then bedrock. Printed as the materials
    //      actually found down the column, so the depths are read off the world
    //      rather than off the constants that made it.
    //
    //   2. THE TOOLS DISAGREE, AND ABOUT THE RIGHT THING. The shovel takes the
    //      soil band and refuses the stone under it; the pick does the reverse;
    //      and neither of them takes bedrock. That is one table, and it is the
    //      whole of what makes a shovel a different tool from a pick.
    //
    //   3. A PIT DOES NOT PAY OUT TWICE. Bite the same spot repeatedly and the
    //      swing must follow the hole DOWN -- if it cannot see the hole it
    //      stops on the surface that used to be there and hands back a chunk of
    //      ground that is no longer under it. Every bite is printed with the
    //      row it landed on, so a swing that cannot see the hole shows as a row
    //      that will not fall.
    // -----------------------------------------------------------------------
    void runDigTest() {
        std::printf("\n=== DIG TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        std::printf("  spawn (%.1f, %.1f, %.1f)\n", player_.pos.x, player_.pos.y, player_.pos.z);

        // ---- 1. WHAT A COLUMN IS MADE OF ---------------------------------
        //
        // Read through TerrainProbe, which is what the swing reads through too,
        // so a disagreement between this table and the tools below would be one
        // bug rather than two separate ideas of the world.
        const int ci = int(std::floor(player_.pos.x / VOXEL_M));
        const int cj = int(std::floor(player_.pos.z / VOXEL_M));
        TerrainProbe probe(&world_.terrain, &world_.editStore());
        const int h = world_.terrain.heightVox(ci, cj);
        std::printf("\n  --- the column at the spawn, surface row %d ---\n", h);
        uint8_t was = 255;
        int runFrom = 0, soilFloor = h, stoneFloor = h;
        for (int d = 0; d <= 140; ++d) {
            const uint8_t m = probe.material(ci, cj, h - d);
            if (isSoilMat(m)) soilFloor = h - d;
            if (isStoneMat(m)) stoneFloor = h - d;
            if (m == was) continue;
            if (was != 255)
                std::printf("    %3d..%3d below surface   %s (id %u)\n", runFrom, d - 1,
                            matFamily(was), unsigned(was));
            was = m;
            runFrom = d;
        }
        std::printf("    %3d..    below surface   %s (id %u)\n", runFrom, matFamily(was),
                    unsigned(was));
        std::printf("    soil reaches %d voxels down, stone %d voxels down\n", h - soilFloor,
                    h - stoneFloor);

        // ---- 2. WHICH TOOL TAKES WHICH BAND ------------------------------
        //
        // The material comes from the probe and the verdict from toolTakes,
        // which is the same function the blow and the audio both ask.
        std::printf("\n  --- what each tool takes, by depth ---\n");
        std::printf("    %-6s %-10s %-6s %-6s %-6s\n", "depth", "material", "axe", "pick",
                    "shovel");
        const int kProbeDepths[] = {0, 1, 4, 10, 50, 99, 101, 120};
        for (const int d : kProbeDepths) {
            Swing s;
            s.hit = true;
            s.kind = Swing::Ground;
            s.material = probe.material(ci, cj, h - d);
            std::printf("    %-6d %-10s %-6s %-6s %-6s\n", d, matFamily(s.material),
                        toolTakes(Takes::Wood, s) ? "yes" : ".",
                        toolTakes(Takes::Stone, s) ? "yes" : ".",
                        toolTakes(Takes::Soil, s) ? "yes" : ".");
        }

        // ---- 3. A PIT, DUG THROUGH BOTH BANDS ----------------------------
        //
        // Straight down, from an eye that stays where a player's would. THE EYE
        // DOES NOT FOLLOW THE HOLE DOWN, and that is the point: every bite
        // after the first has to find the bottom of the pit through the edit
        // layer, which is exactly what the swing ray could not do before.
        //
        // AND IT CHANGES TOOLS THE WAY A PLAYER WOULD, because a pit through
        // this ground is two jobs: the shovel takes the turf and the soil under
        // it, then the soil runs out and only the pick will go on. Digging with
        // one tool would stop at the band it cannot take and prove nothing
        // about the ground below it -- which is most of the ground.
        std::printf("\n  --- twenty swings at one spot, right tool each time ---\n");
        const Vec3 eye{(float(ci) + 0.5f) * VOXEL_M, float(h + 16) * VOXEL_M,
                       (float(cj) + 0.5f) * VOXEL_M};
        const Vec3 down{0.0f, -1.0f, 0.0f};
        // NOT h + 1. The first bite legitimately lands on the row above the
        // surface -- a ray stopping on a top face crosses the boundary exactly,
        // and the floor of that is the voxel above -- so seeding this with the
        // surface row counts the opening swing as a swing that went nowhere.
        // The question only means anything from the SECOND bite on.
        int lastRow = 0;
        bool haveRow = false;
        int stuck = 0, bites = 0;
        for (int b = 0; b < 20; ++b) {
            const Swing s = swingRay(walkWorld(), eye, down);
            if (!s.hit) {
                std::printf("    %2d  the ray found no ground within reach\n", b);
                break;
            }
            const int row = int(std::floor(s.point.y / VOXEL_M));
            const char *tool = toolTakes(Takes::Soil, s)    ? "shovel"
                               : toolTakes(Takes::Stone, s) ? "pick"
                                                            : nullptr;
            if (!tool) {
                std::printf("    %2d  row %4d  %-10s NOTHING IN THE KIT TAKES IT"
                            " -- the floor of the world\n",
                            b, row, matFamily(s.material));
                break;
            }
            if (haveRow && row >= lastRow) ++stuck;
            lastRow = row;
            haveRow = true;
            ++bites;
            std::vector<uint8_t> spoil;
            int n = 0;
            Vec3 at{0, 0, 0};
            world_.dig(s.point, kDigRadiusVox, &spoil, &n, &at);
            int solid = 0;
            for (uint8_t v : spoil)
                if (v != mat::AIR) ++solid;
            std::printf("    %2d  row %4d  %-10s %-6s bit out %3d voxels\n", b, row,
                        matFamily(s.material), tool, solid);
        }
        std::printf("\n  %d bites, %d of them no deeper than the bite before\n", bites, stuck);
        std::printf("  %s\n", stuck == 0
                                  ? "PASS -- every bite came out of ground that was still there"
                                  : "FAIL -- a swing could not see the hole below it");

        // ---- 4. AND NONE OF IT IS DRAWN UNTIL IT IS SEEN -----------------
        //
        // ONLY RENDER WHAT THE PLAYER CAN SEE, measured rather than asserted.
        //
        // A hundred voxels of stone under every column is a hundred times the
        // MATTER, and the whole question is whether it is a hundred times the
        // geometry. It is not, and it cannot be: the heightmap path emits one
        // top quad per column plus the drop to each neighbour, and the voxel
        // path a dug column falls back to emits a face only where the voxel
        // next door is AIR. A voxel with six solid neighbours has no face to
        // give, so buried stone costs nothing to draw however deep it goes.
        //
        // This meshes the chunk the player is standing in -- the real mesher,
        // through the real edit layer -- and prints the ground it contains
        // against the triangles that ground turned into. The first number
        // counts every solid voxel down to bedrock; the second is what the
        // renderer is actually handed.
        //
        // THEN AGAIN, WITH THE PIT IN IT. A hole adds the faces that bound it
        // and nothing else, so the difference is a pit's worth of wall -- not a
        // column's worth of depth.
        std::printf("\n  --- what the mesher emits for this chunk ---\n");
        const int cx = EditStore::floorDiv(ci, CHUNK_VOX);
        const int cz = EditStore::floorDiv(cj, CHUNK_VOX);
        ChunkScratch scratch;
        const std::shared_ptr<const ChunkEdits> ce = world_.editStore().get(cx, cz);
        const size_t trisNow = world_.terrain.meshChunk(cx, cz, scratch, ce.get()).triCount();
        const size_t trisPristine = world_.terrain.meshChunk(cx, cz, scratch, nullptr).triCount();

        // Every solid voxel in the chunk, surface down to the bedrock the
        // profile above found. Counted from the generator, because the point of
        // the comparison is how much ground there IS.
        TerrainMemo cmemo;
        long long solidVox = 0;
        for (int j = 0; j < CHUNK_VOX; ++j)
            for (int i = 0; i < CHUNK_VOX; ++i) {
                const int hh = world_.terrain.heightVox(cx * CHUNK_VOX + i, cz * CHUNK_VOX + j,
                                                        cmemo);
                solidVox += hh;   // rows 0..hh are ground; below kBedrockVox it is bedrock
            }
        std::printf("    %lld solid voxels of ground in the chunk\n", solidVox);
        std::printf("    %zu triangles pristine, %zu with the pit in it (+%lld)\n", trisPristine,
                    trisNow, (long long)trisNow - (long long)trisPristine);
        const double perCol = double(trisPristine) / double(size_t(CHUNK_VOX) * CHUNK_VOX);
        std::printf("    one triangle per %.0f voxels of ground, %.2f triangles per COLUMN\n",
                    trisPristine ? double(solidVox) / double(trisPristine) : 0.0, perCol);
        // THE TEST IS WHICH NUMBER IT SCALES WITH. A mesher that drew what is
        // THERE would emit triangles by the voxel, and this would be in the
        // hundreds. Drawing only what can be SEEN makes it a property of the
        // surface instead: a top quad, plus however many bands the drop to a
        // neighbour has to be split into -- a handful, and flat in the depth of
        // the stone underneath. Eight is a generous ceiling on a handful; this
        // terrain measures about three.
        std::printf("    %s\n", perCol < 8.0
                                    ? "PASS -- triangles scale with the SURFACE, not the depth,"
                                      " so buried stone is free until it is cut into"
                                    : "FAIL -- the buried stone is reaching the renderer");

        // ---- ...AND CAN THE PLAYER GET INTO THE HOLE -----------------------
        //
        // THE ONE THING THIS TEST NEVER ASKED. Everything above proves the
        // world was dug: the bites go deeper, the mesher emits the pit. None of
        // it touches whether the BODY agrees, and for as long as this test
        // existed it did not -- reported as "I created a hole, then when I try
        // to go inside the hole the player still floats above it. its like the
        // missing terrain is missing from the renderer but still there in
        // memory".
        //
        // It was the other way round. The terrain really was gone; Player's
        // ground query read heightVox, which is the GENERATOR's top and cannot
        // see an edit, so the body stood on a floor nothing was drawing.
        //
        // So this asks the walk directly, at the bottom of the shaft the twenty
        // swings above just cut. It is the acceptance test for that bug and it
        // is cheap: two calls, no window, no physics.
        {
            const WalkWorld ww = walkWorld();
            // THE COLUMN THE SWINGS WENT DOWN, not the spawn. The shaft is cut
            // straight down through (ci, cj) from sixteen voxels above it --
            // asking at player_.pos measures a piece of ground nobody touched,
            // which is a test that passes when the bug is present.
            const float sx = (float(ci) + 0.5f) * VOXEL_M;
            const float sz = (float(cj) + 0.5f) * VOXEL_M;
            const float gen = float(world_.terrain.heightVox(ci, cj) + 1) * VOXEL_M;
            std::printf("\n  --- and whether the player can get into it ---\n");
            std::printf("    generated surface   %.2f m\n", double(gen));

            // TWO DIFFERENT QUESTIONS, and only the first is the bug.
            //
            // THE COLUMN is what the walk reads per sample: it must follow the
            // shaft down, and if it does not, nothing else can.
            const float col = walkGroundM(ww, sx, sz);
            std::printf("    the column says     %.2f m   (%+.2f m)  %s\n", double(col),
                        double(col - gen),
                        (col < gen - 0.5f) ? "PASS -- the walk reads the edit layer"
                                           : "FAIL -- it is still reading the generator");

            // THE BODY is groundInfo, which samples the four corners of a 52 cm
            // footprint and takes the HIGHEST. Against the 60 cm shaft the
            // twenty swings cut, standing on the rim is the RIGHT answer -- you
            // cannot fall into a hole narrower than you are. So the body's half
            // of this is asked of a pit it can actually get into: four more
            // bites around the first, which is what digging down looks like.
            // A 3x3 OF BITES AT 4 VOXELS' SPACING. A plus is not enough and
            // the diagonals are why: groundInfo samples the four CORNERS of the
            // footprint, at (+-0.26, +-0.26), and a bite of radius 0.3 m
            // centred on a plus arm at (+-0.5, 0) misses those by 5 cm. The
            // first version of this dug a plus and the body stood on four
            // untouched diagonal columns, which looks exactly like the bug.
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const Vec3 e2{(float(ci + dx * 4) + 0.5f) * VOXEL_M,
                                  float(h + 16) * VOXEL_M,
                                  (float(cj + dz * 4) + 0.5f) * VOXEL_M};
                    for (int k = 0; k < 8; ++k) {
                        const Swing s3 = swingRay(walkWorld(), e2, down);
                        if (!s3.hit) break;
                        world_.dig(s3.point, kDigRadiusVox);
                    }
                }
            const float body = player_.surfaceAt(walkWorld(), sx, sz);
            std::printf("    the body stands at  %.2f m   (%+.2f m)  %s\n", double(body),
                        double(body - gen),
                        (body < gen - 0.5f)
                            ? "PASS -- it can get into a pit its own width"
                            : "FAIL -- standing on ground that has been dug away");
        }
    }

    // The family a material id belongs to, for the tables above. Named rather
    // than numbered because a ramp is six ids that mean one thing, and "13" in
    // a test report tells nobody the shovel is standing in soil.
    static const char *matFamily(uint8_t m) {
        if (m == mat::AIR) return "air";
        if (m == mat::BEDROCK) return "bedrock";
        if (m == mat::ROCK) return "stone";
        if (isGrass(m)) return "grass";
        if (isSoil(m)) return "soil";
        if (isLitter(m)) return "litter";
        if (m == mat::SAND) return "sand";
        if (m == mat::SILT) return "silt";
        if (m == mat::DIRT) return "dirt";
        return "other";
    }

    // -----------------------------------------------------------------------
    // DIG STRAIGHT DOWN AND WATCH WHERE THE SPOIL GOES.
    //
    // (user 2026-09-14, twice: "digging up dirt seems to teleport the chunks to
    // the surface" / "still, when diging deep tin the ground, dirt voxels
    // teleport to the surface instantly this is wrong".)
    //
    // WRITTEN BECAUSE TWO FIXES WERE GUESSED AND SHIPPED. Both were reasoned
    // from the code and neither was measured, and the report came back
    // unchanged both times. A chip that is thrown out of a shaft is a NUMBER --
    // its height against the shaft floor it was born in -- and nothing in this
    // engine was asking for that number.
    //
    // IT RUNS THE LIVE FLOOR, NOT THE GENERATOR'S. --float-test hands
    // updateDebris a terrainAt built from heightVox alone, which is the world
    // as it was before anybody dug; under that backstop a chip in a shaft is
    // below the ground BY DEFINITION and is clamped to the surface every
    // frame. That would have shown this "bug" on a perfectly good engine and
    // hidden it on a broken one. The lambda here is the one the frame loop
    // uses -- walkGroundM, which has the holes in it.
    // -----------------------------------------------------------------------
    void runShaftTest() {
        std::printf("\n=== SHAFT TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        const float x = player_.pos.x, z = player_.pos.z;
        TerrainMemo memo;
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        const float surf = float(world_.terrainTopAt(ci, cj, memo) + 1) * VOXEL_M;
        std::printf("  standing at (%.1f, %.1f)  surface %.2f m\n", x, z, surf);

        // ---- a shaft, one bite every 15 cm ---------------------------------
        //
        // DEEPER THAN THE DIRT IS. The report is about DIRT, and the crust is
        // eleven voxels now -- a shaft that bottoms out on stone in the first
        // half metre is not the thing being tested.
        const float depthM = 4.0f;
        long blows = 0;
        for (float y = surf - 0.05f; y > surf - depthM; y -= 0.15f)
            if (world_.dig(Vec3(x, y, z), kDigRadiusVox, &spoilVol_, &spoilN_, &spoilAt_))
                ++blows;
        const WalkWorld ww0 = walkWorld();
        const float floorNow = walkGroundM(ww0, x, z);
        std::printf("  %ld bites, floor now %.2f m -- %.2f m down\n", blows, floorNow,
                    surf - floorNow);
        // ---- ...AND WHERE THE PLAYER STANDS WHILE DIGGING IT ---------------
        //
        // HALF THE QUESTION, AND IT WAS NOT BEING ASKED. Spoil flies to the
        // EYE, so "where does the dirt go" is "where is the player" -- and a
        // body has a radius while a bite is three voxels across. If the walk
        // keeps you standing on the rim of your own shaft, every chip you cut
        // four metres down travels four metres up through solid ground to
        // reach you, which is exactly what was reported.
        player_.placeOnGround(walkWorld(), x, z);
        std::printf("  the player stands at %.2f m -- %s\n", player_.pos.y,
                    player_.pos.y < floorNow + 0.5f ? "in the shaft"
                                                    : "ON THE RIM, over their own hole");
        if (surf - floorNow < 1.0f) {
            std::printf("  the shaft is under a metre deep; nothing here is worth "
                        "measuring\n");
            return;
        }

        // ---- one more bite, and this one keeps its spoil -------------------
        const float birthY = floorNow + 0.10f;
        if (!world_.dig(Vec3(x, birthY, z), kDigRadiusVox, &spoilVol_, &spoilN_, &spoilAt_)) {
            std::printf("  the last bite took nothing -- bedrock?\n");
            return;
        }
        const Vec3 still{0.0f, 0.0f, 0.0f};
        const int slot =
            world_.spawnDebris(physics_, spoilVol_, spoilN_, spoilAt_, still, still, simMs_,
                               0.0f, nullptr, uint8_t(kDebrisSoil));
        if (slot < 0) {
            std::printf("  no free debris slot\n");
            return;
        }
        rebuildGroundPatch();
        Vec3 p0{0, 0, 0};
        float q0[4] = {0, 0, 0, 1};
        if (!world_.debrisPose(slot, &p0, q0)) {
            std::printf("  the chip has no pose\n");
            return;
        }
        std::printf("\n  the chip is born at %.2f m, %.2f m under the surface\n", p0.y,
                    surf - p0.y);
        std::printf("  frame    chip y   vs floor   vs surface   backstop  state\n");

        // ---- and now watch it, on the frame loop's own terms ---------------
        const float dt = 1.0f / 60.0f;
        float highest = p0.y;
        // ...AND WHERE IT WAS WHEN THE COLLECT TOOK IT, which is the only
        // number here that says anything about the PHYSICS. Everything after
        // that moment is a scripted lerp to the eye and proves nothing.
        bool wasFlying = false;
        float atAbsorb = p0.y;
        Vec3 p = p0;
        for (int f = 1; f <= 180; ++f) {
            if (world_.takeGroundDirty() ||
                !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                rebuildGroundPatch();
            physics_.step(dt);
            simMs_ += double(dt) * 1000.0;
            {
                const WalkWorld ww = walkWorld();
                world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                    [&](float ax, float az) { return walkGroundM(ww, ax, az); });
            }
            float qq[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(slot, &p, qq)) {
                std::printf("  the chip is gone at frame %d\n", f);
                break;
            }
            if (p.y > highest) highest = p.y;
            // EVERY EARLY FRAME, AND THE FLOOR THE BACKSTOP WOULD USE.
            //
            // The two ways a chip leaves a hole look nothing alike frame by
            // frame and identical every thirty: updateDebris' clampAbove is a
            // TELEPORT -- one frame, any distance -- and a heightfield the
            // solver thinks the body is inside is a shove, fast but continuous.
            // The backstop column is printed beside it because that is the
            // number clampAbove compares against: if it reads the shaft floor,
            // the clamp is innocent and the patch is not.
            const bool flying = world_.debrisAbsorbing(slot);
            if (flying && !wasFlying) {
                wasFlying = true;
                atAbsorb = p.y;
            }
            if (f <= 40 || f % 30 == 0) {
                const WalkWorld wq = walkWorld();
                std::printf("  %5d  %7.2f  %+8.2f  %+10.2f  %8.2f  %s\n", f, p.y,
                            p.y - floorNow, p.y - surf, walkGroundM(wq, p.x, p.z),
                            flying ? "collected" : "falling");
            }
        }

        // ---- the verdict ---------------------------------------------------
        //
        // TWO THINGS, AND THE FIRST IS THE REPORT. A chip born four metres down
        // must never be seen at the surface -- not at the end, at any point, so
        // the highest it ever reached is what is tested. The second is that it
        // is still down there when the dust settles, which rules out a chip
        // that is ejected and falls back in.
        // MEASURED AT THE MOMENT THE COLLECT TOOK IT, not at the end. A chip
        // that is picked up has left the simulation: it is a kinematic lerp to
        // the eye from there on, it passes through the ground by design, and
        // judging the physics by where that lerp ends would fail every engine.
        const bool stayedDown = atAbsorb < surf - 0.5f;
        const bool restedLow = atAbsorb > floorNow - 1.0f;
        std::printf("\n  the solver left it at %.2f m (%+.2f m against the surface)  %s\n",
                    atAbsorb, atAbsorb - surf,
                    stayedDown ? "stayed in the shaft" : "CAME OUT -- THIS IS THE BUG");
        std::printf("  the highest it ever reached, the collect included, was %.2f m\n",
                    highest);
        std::printf("  %s\n",
                    (stayedDown && restedLow)
                        ? "PASS -- the spoil stayed in the hole it came out of."
                        : "FAIL -- the spoil was thrown to the surface.");
    }

    // -----------------------------------------------------------------------
    // KILL ONE OF EVERYTHING AND CHECK WHAT IT LEFT BEHIND.
    //
    // (user 2026-09-14, the five-part ask that this whole feature is.)
    //
    // FIVE THINGS HAPPEN WHEN AN ANIMAL DIES and four of them are invisible to
    // any test that only looks at whether it is gone: the flash, the pieces,
    // the sparks, the smoke and the steak. So this walks the /locate table,
    // teleports to each species in turn, aims at it, and swings until it is
    // down -- reporting every one of the five against what v1's rules say that
    // species should do.
    //
    // THE MEAT RULE IS THE POINT OF THE SPECIES LOOP. "there are unique animals
    // that dont drop anything, like the song birds and worms" -- so a test that
    // kills one rabbit proves the half that is easy. The column that matters is
    // the one where a songbird and a worm leave NOTHING and the frog beside
    // them leaves a steak.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // --duck-test: A MOTHER DIES AND HER BROOD CARRIES ON.
    //
    // (user 2026-09-15: "the babies should not dissapeare when the mother dies,
    // but stay on the field, and wander in random directions without their
    // mother ... the babies should cry".)
    //
    // THE FAILURE THIS EXISTS TO CATCH IS SILENT AND WAS ALREADY THERE. The
    // ducklings were not being deleted by anything that mentions ducklings:
    // they were retired by the population's ordinary distance test, which asks
    // how far the MOTHER is -- and a killed mother is a zeroed struct sitting
    // at the world origin, which is far from everywhere. Nothing logs that, and
    // at a glance a brood quietly fading out is indistinguishable from a brood
    // that swam off. So this asks the three questions separately: are they
    // still there, did they move, and did they weep.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // --lbug-test: DOES THE LADYBUG DOUBLE ITS RATE NEAR THE PLAYER?
    //
    // (user 2026-09-15: "if the player is near, the ladybug moves at twice the
    // rate, double in both fps but also movement speed".)
    //
    // NO TELEPORTING, AND NOTHING FORCED. The player stands still and the
    // ladybugs wander in and out of their own near band on their own, so one
    // run yields both populations -- and every sample is the real step, not a
    // rate arithmetic I could get right in the test and wrong in the engine.
    //
    // ONLY CRUISING FRAMES COUNT. A ladybug sitting on a stone has a speed of
    // zero whatever the distance, and averaging those in would drag both
    // buckets toward zero and hide the effect in the noise.
    //
    // THE ANIMATION IS MEASURED THE SAME WAY, off the frame counter, because
    // the user asked for BOTH and a change to one is not evidence about the
    // other -- the two are driven from one scaled dt in stepBugs and this is
    // what says so.
    // -----------------------------------------------------------------------
    void runLbugTest() {
        std::printf("\n=== LADYBUG TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        warmLife(player_.pos, 3);
        publishLife();

        const int lbN = Critters::lbugCount();
        const float dt = 1.0f / 60.0f;

        // -- THE DISTANCE IS SET, NOT WAITED FOR -------------------------
        //
        // The first cut of this stood still and sampled whatever distance the
        // ladybugs happened to wander to. Over a full minute that gave 5487
        // samples beyond ten metres and ONE inside five: they are scattered on
        // a 22 m lattice and have no reason to come to you, so the near bucket
        // -- the entire point of the test -- never filled.
        //
        // So the player is PUT at a chosen distance from one ladybug, every
        // frame, and the same insect is measured at both. Nothing about the
        // ladybug is touched; it does not flee, it does not seek, and the only
        // thing the player's position feeds is the rate ramp under test.
        auto measure = [&](float standOff, double *speed, double *fps, int *samples) {
            *speed = 0.0;
            *fps = 0.0;
            *samples = 0;
            Vec3 was{0, 0, 0};
            float wasF = 0.0f;
            bool had = false;
            for (int f = 0; f < 1800; ++f) {   // thirty seconds at each distance
                bool live = false, cruising = false;
                Vec3 at{0, 0, 0};
                float fr = 0.0f;
                if (critters_.lbugProbe(0, &live, &cruising, &at, &fr) && live) {
                    player_.pos.x = at.x + standOff;
                    player_.pos.z = at.z;
                    pos_ = player_.eyePosition();
                }
                world_.update(player_.pos);
                // ONE FRAME OF CRITTERS, NOT ONE SECOND. This called
                // warmLife(pos, 1) -- which advances a WHOLE SECOND of
                // population time per call -- once per loop iteration, so two
                // consecutive "frames" were a second apart and the ladybug's
                // apparent speed came out at 36 m/s against a real 2.2. The
                // same argument list the frame itself passes, so what is
                // measured is the shipping step and not a simplified one.
                critters_.update(dt, player_.pos,
                                 [this](float x, float z) {
                                     return float(world_.terrain.heightVox(
                                                      int(std::floor(x / VOXEL_M)),
                                                      int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
                                 },
                                 [this](float x, float z) { return wetColumnAt(x, z); },
                                 [this](float x) { return world_.terrain.birchMix(x); },
                                 banksNear_, perches_, isNight(), forward(),
                                 [this](float x, float z) { return waterTopAt(x, z); });
                simMs_ += double(dt) * 1000.0;
                if (!critters_.lbugProbe(0, &live, &cruising, &at, &fr) || !live || !cruising) {
                    had = false;
                    continue;
                }
                if (had) {
                    const float mx = at.x - was.x, mz = at.z - was.z;
                    // THE WINGBEAT WRAPS at the strip length, so a negative
                    // difference is a wrap rather than the wings running
                    // backwards, and the sample is dropped rather than counted
                    // as a large step the wrong way.
                    const float dfps = (fr - wasF) / dt;
                    if (dfps > 0.0f) {
                        *speed += double(std::sqrt(mx * mx + mz * mz) / dt);
                        *fps += double(dfps);
                        ++(*samples);
                    }
                }
                was = at;
                wasF = fr;
                had = true;
            }
            if (*samples > 0) {
                *speed /= double(*samples);
                *fps /= double(*samples);
            }
        };

        double farS = 0.0, farF = 0.0, nearS = 0.0, nearF = 0.0;
        int farN = 0, nearN = 0;
        // FAR FIRST, so the near pass is not measuring a ladybug that is still
        // carrying speed from being crowded.
        measure(kLbugFarM + 4.0f, &farS, &farF, &farN);
        measure(kLbugNearM - 2.0f, &nearS, &nearF, &nearN);

        if (nearN < 60 || farN < 60) {
            std::printf("  not enough cruising frames (%d near, %d far) -- no verdict\n", nearN,
                        farN);
            return;
        }
        std::printf("  cruising frames: %d at %.0f m, %d at %.0f m\n", nearN,
                    double(kLbugNearM - 2.0f), farN, double(kLbugFarM + 4.0f));
        const double sr = farS > 1e-6 ? nearS / farS : 0.0;
        const double fr2 = farF > 1e-6 ? nearF / farF : 0.0;
        std::printf("  speed      near %.2f m/s   far %.2f m/s   ratio %.2f  %s\n", nearS, farS,
                    sr, (sr > 1.7 && sr < 2.3) ? "doubled, correct" : "NOT DOUBLED -- WRONG");
        std::printf("  wingbeat   near %.1f fps   far %.1f fps   ratio %.2f  %s\n", nearF, farF,
                    fr2, (fr2 > 1.7 && fr2 < 2.3) ? "doubled, correct" : "NOT DOUBLED -- WRONG");
        const bool ok = sr > 1.7 && sr < 2.3 && fr2 > 1.7 && fr2 < 2.3;
        std::printf("\n  %s\n",
                    ok ? "PASS -- near the player it moves and flaps at twice the rate."
                       : "FAIL -- see the line above.");
        std::fflush(stdout);
    }

    // A SPAWN WITH A LAKE IN IT, OR THERE IS NOTHING TO TEST. Ducks follow
    // water, so this test is only as good as where --spawn drops you -- and
    // when the oak band was inserted the tiling moved, which put the long-used
    // --spawn 4242 somewhere with no lake in reach and made this print "no duck
    // in this wood". That is the spawn, not the ducks: 1, 7, 99 and 2026 all
    // pass. If this says there is no duck, try another seed before believing it.
    void runDuckTest() {
        std::printf("\n=== DUCK TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();

        // THE POPULATION HAS TO HAVE RUN BEFORE IT CAN BE ASKED. runKillTest
        // wrote the note on this and this test walked straight into it anyway:
        // "the first version primed the CHUNKS and then asked where the nearest
        // bunny was, which is a question about a population that had never
        // ticked". Asked cold, every lake in range answers "no ducks".
        warmLife(player_.pos, 3);
        publishLife();
        Vec3 at{0, 0, 0};
        if (!nearestLife(Life::Duck, &at)) {
            std::printf("  no duck in this wood -- nothing to test\n");
            return;
        }
        teleportToLife(at, 6.0f);
        for (int w = 0; w < 240; ++w) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        warmLife(player_.pos, 3);
        publishLife();

        const int mom = lake_.firstFamily();
        if (mom < 0) {
            std::printf("  a duck, but no complete family within reach\n");
            return;
        }
        // WHERE THE THREE OF THEM WERE, so "they wandered" can be a measurement
        // rather than an impression.
        Vec3 was[kBabyPerDuck];
        bool live = false, orphan = false, crying = false;
        for (int b = 0; b < kBabyPerDuck; ++b)
            lake_.duckProbe(LakeLife::babyIndex(mom, b), &live, &orphan, &crying, &was[b]);
        std::printf("  a family: mother %d, three ducklings\n", mom);

        // ...AND SHE IS KILLED THROUGH THE REAL PATH, not by clearing a slot.
        // killLifeAt is what a blow reaches, and the orphaning is armed inside
        // LakeLife::killSlot underneath it -- so a test that zeroed the struct
        // itself would be testing the test.
        const int band = kButterflySlots + kBirdSlots + LakeLife::duckLocalSlot(mom);
        killLifeAt(band);
        publishLife();

        int sawTears = 0;
        for (int f = 0; f < 300; ++f) {   // five seconds: the 0.9 s wait, then 3 s of weeping
            const float dt = 1.0f / 60.0f;
            world_.update(player_.pos);
            lake_.update(dt, world_.terrain, player_.pos, forward());
            sawTears += int(lake_.tearsThisTick().size());
            simMs_ += double(dt) * 1000.0;
        }

        int stillHere = 0, wandered = 0, wept = 0;
        float moved = 0.0f;
        for (int b = 0; b < kBabyPerDuck; ++b) {
            Vec3 now{0, 0, 0};
            if (!lake_.duckProbe(LakeLife::babyIndex(mom, b), &live, &orphan, &crying, &now))
                continue;
            if (!live) continue;
            ++stillHere;
            if (orphan) ++wept;
            const float d = sqrtf((now.x - was[b].x) * (now.x - was[b].x) +
                                  (now.z - was[b].z) * (now.z - was[b].z));
            if (d > moved) moved = d;
            if (d > 0.5f) ++wandered;
        }
        std::printf("  still on the field   %d of 3   %s\n", stillHere,
                    stillHere == kBabyPerDuck ? "correct"
                                              : "THE BROOD VANISHED WITH HER -- WRONG");
        std::printf("  marked orphaned      %d of 3   %s\n", wept,
                    wept == stillHere && stillHere > 0 ? "correct"
                                                       : "STILL FOLLOWING A DEAD MOTHER -- WRONG");
        std::printf("  wandered off         %d of 3, furthest %.2f m   %s\n", wandered, double(moved),
                    wandered > 0 ? "correct"
                                 : "THEY FROZE WHERE SHE DIED -- WRONG");
        std::printf("  tears shed           %d   %s\n", sawTears,
                    sawTears > 0 ? "correct" : "NOBODY CRIED -- WRONG");
        const bool ok = stillHere == kBabyPerDuck && wept == stillHere && wandered > 0 &&
                        sawTears > 0;
        std::printf("\n  %s\n", ok ? "PASS -- the brood outlived her, wandered, and wept."
                                     : "FAIL -- see the line above.");
        std::fflush(stdout);
    }

    void runKillTest() {
        std::printf("\n=== KILL TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        // AN AXE, so one blow is one kill and the test is about the death
        // rather than about counting to three. The three-blow path is checked
        // on its own below.
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Wood) held_.select(i);
        // AND THE POPULATIONS HAVE TO HAVE RUN. The first version of this
        // primed the CHUNKS and then asked where the nearest bunny was, which
        // is a question about a population that had never ticked: every one of
        // the twenty-four answered "none", the loop skipped them all in
        // silence, and the report was a header with nothing under it.
        warmLife(player_.pos, 3);
        publishLife();
        std::printf("  the %s wood, steak slot %d\n",
                    world_.terrain.woodName(pos_.x), steakTool_);

        std::printf("\n  %-10s  %-5s  %-6s  %-6s  %-5s  %s\n", "species", "hits", "pieces",
                    "sparks", "smoke", "meat");
        int tried = 0, killed = 0, wrong = 0;
        for (const LifeName &ln : lifeNames()) {
            // THE HIVE AND THE LILY PAD ARE NOT ALIVE. One is decor in a crown
            // and the other is a leaf; neither answers the band's life table.
            if (ln.life == Life::Hive || ln.life == Life::LilyPad) continue;
            Vec3 at{0, 0, 0};
            // NOT SILENT. A species this world has none of is an ordinary
            // outcome -- the frog is birch-only, the armadillo pine-only -- but
            // it is not the same outcome as a kill that went wrong, and a run
            // that skipped everything in silence is what this line is for.
            if (!nearestLife(ln.life, &at)) {
                std::printf("  %-10s  none in this wood\n", ln.name);
                continue;
            }
            teleportToLife(at, ln.stand);
            // STREAM THE GROUND, THEN LET THE POPULATION FIND IT. Both are
            // needed and in that order: an animal is placed against terrain
            // that has to exist first, and a population that has not ticked
            // since the jump still has its members back where you came from.
            for (int w = 0; w < 120; ++w) {
                world_.update(player_.pos);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            warmLife(player_.pos, 2);
            publishLife();
            // ...and find it again, because it has been walking while the
            // chunks streamed in.
            if (!nearestLife(ln.life, &at)) {
                std::printf("  %-10s  gone by the time the chunks landed\n", ln.name);
                continue;
            }
            // -- ...AND THEN WALK UP TO IT --------------------------------
            //
            // teleportToLife stands you at the species' OWN distance, which is
            // a number chosen so the animal does not bolt before you have seen
            // it (see the `stand` column) -- five metres for a bunny and ten
            // for a mouse. The melee reach is 5.3 m, so half the table was out
            // of range before the first swing and reported "not drawn" for a
            // reason that had nothing to do with the kill path.
            //
            // NO WARM AFTER THIS, deliberately: the animal is where the line
            // above found it, and ticking the populations again would move it
            // while this closes the distance. The publish is so the BAND knows
            // where the player is standing -- see flyerAt.
            {
                const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(at.x + dx / d * 1.6f, at.z + dz / d * 1.6f);
                publishLife();
            }
            // AIM AT IT. Everything downstream reads forward(), so this is the
            // whole of "look at the animal" -- the same trick --hoe-test uses
            // to stop its ray landing somewhere it never approved.
            pos_ = player_.eyePosition();
            const Vec3 d = normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
            yaw_ = atan2f(d.x, -d.z) * 180.0f / PI;
            pitch_ = asinf(d.y) * 180.0f / PI;
            const int slot = lifeHits_.aim(world_, pos_, forward());
            if (slot < 0) {
                // WHY, rather than that it happened. The two reasons are not
                // the same bug: an animal out of REACH is this test standing in
                // the wrong place, and one not DRAWN is a population that
                // published nothing into the band -- which would be a real
                // fault in the thing being tested.
                const float dxq = at.x - pos_.x, dyq = at.y - pos_.y, dzq = at.z - pos_.z;
                int drawn = 0;
                for (int q = 0; q < kFlyerInstances; ++q)
                    if (lifeAtSlot(q).alive() && world_.flyerAt(q, nullptr, nullptr)) ++drawn;
                std::printf("  %-10s  no aim: it is %.1f m off (%.1f m up), %d animals drawn\n",
                            ln.name, std::sqrt(dxq * dxq + dzq * dzq), dyq, drawn);
                continue;
            }
            const LifeKind kind = lifeKindAt(slot);
            ++tried;
            // AN EMPTY FIELD BEFORE EACH ONE -- see Drops::clearAll and
            // World::clearDebris. Both pools are small and a corpse fills a
            // good part of one, so without this the later species in the table
            // report a shatter and a steak that the pools simply had no room
            // for -- which reads exactly like the feature not working.
            drops_.clearAll();
            world_.clearDebris(physics_);
            const int drops0 = drops_.count();
            int hits = 0;
            bool died = false;
            for (int k = 0; k < 6 && !died; ++k) {
                ++hits;
                if (!strikeLife()) break;
                died = !lifeHits_.flashing(slot) || lifeHits_.dying(slot);
                // A NEW SWING EACH TIME. One swing lands one hit -- v1 guards
                // that with a token; here the guard is that this test is the
                // only thing swinging.
            }
            const int sparks = particles_.live();
            int smoke = 0;
            for (int q = 0; q < kParticleSlots; ++q) {
                Vec3 pp{0, 0, 0};
                bool sm = false;
                if (particles_.at(q, &pp, &sm, simMs_) && sm) ++smoke;
            }
            const int got = drops_.count() - drops0;
            const bool meatRight = kind.meat ? (got > 0) : (got == 0);
            // -- ...AND A FISH'S STEAK HAS TO BE ON THE LAKE ----------------
            //
            // (user 2026-09-15, twice: "have the raw steak float above the
            // water ... have the steak appear above the water floating, not in
            // it".)
            //
            // THE FIRST FIX WAS INVISIBLE TO EVERY TEST THERE WAS, which is why
            // it shipped wrong: `got > 0` is true whether the meat is bobbing
            // on the surface or lying on the seabed four metres under it. The
            // steak was being RELEASED at the waterline and then flying on down
            // to the terrain, because the arc's terminator had never heard of
            // water -- so the only question worth asking is this one, and the
            // drop has to be settled before it is asked.
            char meatWhere[48] = {0};
            if (kind.meat && got > 0) {
                Vec3 dp{0, 0, 0};
                bool flying = true;
                for (int f = 0; f < 240 && drops_.newestDrop(&dp, &flying) && flying; ++f)
                    drops_.update(1.0f / 60.0f, walkWorld(), player_.pos, pos_);
                if (drops_.newestDrop(&dp, &flying)) {
                    const float wtop = waterTopAt(dp.x, dp.z);
                    if (wtop > -1e8f && wtop > at.y - 1.0f)
                        std::snprintf(meatWhere, sizeof(meatWhere), "  %+.2f m vs the water %s",
                                      double(dp.y - wtop),
                                      dp.y >= wtop ? "-- afloat" : "-- SUNK, WRONG");
                }
            }
            if (died) ++killed;
            if (!died || !meatRight) ++wrong;
            std::printf("  %-10s  %-5d  %-6d  %-6d  %-5d  %s%s\n", kind.name ? kind.name : ln.name,
                        hits, lastPieces_, sparks - smoke, smoke,
                        kind.meat ? (got > 0 ? "steak" : "NONE -- WRONG")
                                  : (got == 0 ? "none, correct" : "A STEAK -- WRONG"),
                        died ? (meatWhere[0] ? meatWhere : "") : "   IT DID NOT DIE");
            particles_.update(simMs_ + 4000.0);   // clear the air before the next one
        }
        // ---- ...AND A SHAFT STOPS AT A BOULDER --------------------------
        //
        // (user 2026-09-15: "arrows are clipping through big rocks. dont let
        // arrows go through anything.")
        //
        // FIRED FROM WELL BACK, which is the whole point: inside the walk's own
        // six metres the arrow always did stop, so a test that stood close
        // would have passed against the bug. This stands 25 m off and asks
        // whether the shaft is still travelling when it is past the rock.
        bool rockTried = false, rockStopped = false;
        {
            std::vector<Solid> around;
            world_.collidersNear(player_.pos, 120.0f, &around);
            const Solid *big = nullptr;
            float bestR = 0.0f;
            for (const Solid &q : around) {
                if (q.modelKind != 1 || !q.vol) continue;   // rocks only
                const float r = 0.5f * (float(q.msx) + float(q.msz)) * VOXEL_M;
                if (r > bestR) { bestR = r; big = &q; }
            }
            if (big && bestR > 0.8f) {
                const Solid rock = *big;
                rockTried = true;
                lastChipN_ = 0;   // so the report is THIS shot's, not an older one's
                // Stand off along +x and aim at the rock's middle.
                // THE UPPER HALF, NOT THE MIDDLE. A flat shot at the centre
                // of a boulder 25 m away is a shot that spends 25 m falling
                // under kArrowG, and on ground that rises even slightly it
                // buries itself at the archer's own feet -- measured: the chip
                // came out 0.5 m from the player, which then absorbed exactly as
                // the rule says it should and read as the rule being broken.
                // LEVEL WITH THE ARCHER'S EYE, clamped into the rock. Aiming
                // at a fixed fraction of the boulder's height aims DOWNWARD
                // whenever the player is standing above it -- which on this
                // terrain is most of the time -- and a downward shot from six
                // metres buries itself in the ground in front of them. Measured
                // at 80% of the rock's height: still cut 2.3 m from the archer.
                //
                // A LEVEL SHOT HAS NO GROUND TO HIT. The eye is 1.6 m over the
                // feet, the boulder is metres tall, and the clamp only matters
                // for a rock shorter than the archer -- which bestR > 0.8 has
                // already excluded.
                const float rockTop = rock.baseY + float(rock.vsy) * VOXEL_M;
                const float ry = maxf(rock.baseY + 0.25f * float(rock.vsy) * VOXEL_M,
                                      minf(pos_.y, rockTop - 0.3f));
                // -- SIX METRES CLEAR OF THE FACE, NOT 25 FROM THE CENTRE --
                //
                // A fixed 25 m was a fixed distance from a boulder whose RADIUS
                // varies from one to eight metres, so how far the shaft actually
                // had to fly changed with whichever rock the world happened to
                // offer -- and on any ground that rises between, it buried
                // itself at the archer's feet instead. Measured twice, at a
                // cut 0.4 m away.
                //
                // What the two rules under test need is only that the target is
                // struck and that the chip lands FAR BEYOND the 1.6 m absorb
                // reach. Six metres of clear air proves both and gives gravity
                // almost nothing to work with: kArrowSpeed covers it in an
                // eighth of a second.
                const float standOff = bestR + 6.0f;
                // -- EIGHT BEARINGS, AND THE FIRST WITH CLEAR AIR WINS --------
                //
                // ONE BEARING IS A COIN TOSS IN A WOOD. The shot was fired from
                // +x every time, so whatever stood between the archer and the
                // stone on that one line decided the test: measured, the shaft
                // stopped 2.4 m out and the chip it cut was then inside the
                // player's own absorb reach -- which is the rule WORKING, and it
                // read as the rule failing. The oaks made it likelier, one being
                // seventeen metres across, but a birch or a second boulder was
                // always just as capable of standing in the way.
                //
                // So the archer walks round the rock. A chip that lands well
                // clear of them is a shot that actually reached the stone, and
                // that is the only thing the two rules below need.
                const WalkWorld aw = wideWalkWorld(kArrowSolidsM);
                for (int bear = 0; bear < 8; ++bear) {
                    const float ang = float(bear) * 0.7853982f;
                    teleportTo(rock.cx + sinf(ang) * standOff, rock.cz + cosf(ang) * standOff);
                    pos_ = player_.eyePosition();
                    lastChipN_ = 0;
                    // So a bearing that cuts nothing at all reads as SHORT
                    // rather than inheriting the last one's cut.
                    lastChipAt_ = pos_;
                    const Vec3 aim =
                        normalize(Vec3(rock.cx - pos_.x, ry - pos_.y, rock.cz - pos_.z));
                    arrows_.launch(pos_, Vec3(aim.x * kArrowSpeed, aim.y * kArrowSpeed,
                                              aim.z * kArrowSpeed));
                    for (int f = 0; f < 120 && arrows_.inFlight() > 0; ++f) {
                        arrows_.update(1.0f / 60.0f, aw, nullptr);
                        // THE CHIP IS DRAINED HERE. This loop drives the flight
                        // itself rather than going through the frame, so it has
                        // to do the frame's job: impactsThisTick is cleared on
                        // the NEXT update, so a drain outside this loop would
                        // find nothing however well the carve worked. Which is
                        // exactly what it reported -- 0 voxels off a boulder it
                        // had demonstrably just stopped dead in.
                        for (const Arrows::Impact &im : arrows_.impactsThisTick())
                            arrowChip(im.at, im.dir);
                    }
                    const float cdx = lastChipAt_.x - pos_.x, cdz = lastChipAt_.z - pos_.z;
                    if (lastChipN_ > 0 && cdx * cdx + cdz * cdz > 25.0f) break;
                }
                // WHERE IT CAME TO REST, against the rock's own centre. A shaft
                // that stopped is within a metre or two of the stone; one that
                // went through is metres PAST it, on the far side.
                float stoppedAt = 1e9f;
                for (const Vec3 &q : arrows_.landedThisTick())
                    stoppedAt = minf(stoppedAt, q.x - rock.cx);
                rockStopped = arrows_.inFlight() == 0;
                std::printf("\n  -- a shaft at a %.1f m boulder, from %.1f m --\n",
                            double(bestR * 2.0f), double(standOff));
                std::printf("  still flying after 2 s: %s\n",
                            arrows_.inFlight() ? "YES -- IT WENT THROUGH" : "no, it stopped");
                // ...AND WHAT IT TOOK OUT OF IT (user 2026-09-15: "have arrow
                // take out tiny peices of material ... as big as the knife
                // chunk as seen in v1"). v1's knife lifts 8 voxels; a radius-1
                // sphere is the 7 cells within one of the centre, which is the
                // nearest this engine can cut -- see kArrowChipVox. An AXE
                // takes 113 from the same boulder, so the pair of numbers is
                // what says "chips, does not dig".
                std::printf("  it chipped out %d voxels  %s\n", lastChipN_,
                            lastChipN_ <= 0 ? "NOTHING CAME OUT -- WRONG"
                            : lastChipN_ > 24 ? "THAT IS A DIG, NOT A CHIP -- WRONG"
                                              : "a chip, about the knife's own");
                // -- ...AND IT DOES NOT COME TO YOU FROM OVER THERE ---------
                //
                // (user 2026-09-15: "dont have the chipped chunk caused by the
                // arrow get absorbed by the player. only when the player is
                // close enough to absorbe the chunk".)
                //
                // THE PLAYER HAS NOT MOVED and is still the 25 m back they
                // fired from, so a chip that arrives is a chip that crossed the
                // clearing. Three seconds is well past kAbsorbWaitMs and past
                // the whole kAbsorbFlyMs curve, so if it were coming it would
                // have arrived.
                // THE CHIP ITSELF, NOT THE POOL. The first cut of this
                // counted looseCount() before and after, which is every loose
                // body in the world -- and by this point the run has killed
                // fifteen animals and left their pieces lying about. Two of
                // them were collected during the three seconds, the count fell,
                // and the test reported that the arrow's chip had flown across
                // the clearing when the chip had not moved at all. A pool total
                // cannot answer a question about one body in it.
                const int chipSlot = lastChipSlot_;
                {
                    const WalkWorld cw = walkWorld();
                    for (int f = 0; f < 180; ++f) {
                        physics_.step(1.0f / 60.0f);
                        simMs_ += 1000.0 / 60.0;
                        world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                            [&](float ax, float az) {
                                                return walkGroundM(cw, ax, az);
                                            });
                    }
                }
                // -- FOUND BY WHERE IT IS, NOT BY WHICH SLOT IT TOOK -----
                //
                // The slot is not an identity. kDebrisInstances is 64 and this
                // test kills eighteen animals before it fires the arrow, so the
                // pool has wrapped several times over by now and spawnDebris
                // hands out the OLDEST slot once it is full. Asked by index, the
                // chip read as "gone" while it was lying exactly where it fell
                // -- a failure of the question, not of the engine.
                //
                // So the pool is scanned for a body still near where the chip
                // was cut out. A chip that has not moved is within a metre of
                // that point; one that came to the player is twenty-five metres
                // away and is not.
                Vec3 cp{0, 0, 0};
                float cq[4] = {0, 0, 0, 1};
                bool stillThere = false, coming = false;
                for (int b = 0; b < kDebrisInstances; ++b) {
                    Vec3 bp{0, 0, 0};
                    float bq[4] = {0, 0, 0, 1};
                    if (!world_.debrisPose(b, &bp, bq)) continue;
                    const float dx = bp.x - lastChipAt_.x, dz = bp.z - lastChipAt_.z;
                    if (dx * dx + dz * dz > 4.0f) continue;   // two metres of the cut
                    stillThere = true;
                    cp = bp;
                    coming = world_.debrisAbsorbing(b);
                    break;
                }
                (void)chipSlot;
                // -- A SHOT THAT FELL SHORT PROVES NOTHING ---------------
                //
                // The rule under test is "a chip cut across the clearing does
                // not come to you". If the shaft buried itself at the archer's
                // feet then the chip was never across the clearing, and it
                // absorbing is the rule WORKING rather than failing. Measured
                // once as a flat WRONG before this guard existed, on a shot cut
                // 0.5 m from the player.
                const float cutAway =
                    std::sqrt((lastChipAt_.x - pos_.x) * (lastChipAt_.x - pos_.x) +
                              (lastChipAt_.z - pos_.z) * (lastChipAt_.z - pos_.z));
                const float away =
                    stillThere ? std::sqrt((cp.x - pos_.x) * (cp.x - pos_.x) +
                                           (cp.z - pos_.z) * (cp.z - pos_.z))
                               : 0.0f;
                if (cutAway < 5.0f) {
                    std::printf("  the shaft fell short -- it cut %.1f m from the archer, so "
                                "there is no reach rule to test here\n", double(cutAway));
                } else {
                    std::printf("  after 3 s, standing off: %s  %s\n",
                                !stillThere ? "the chip is gone"
                                : coming    ? "the chip is on its way to you"
                                            : "the chip is still lying there",
                                (stillThere && !coming && away > 5.0f)
                                    ? "correct, it waits to be walked up to"
                                    : "IT CAME TO THE PLAYER -- WRONG");
                }
                if (stillThere && cutAway >= 5.0f)
                    std::printf("  ...and it is %.1f m off, where it was knocked loose\n",
                                double(away));
            }
        }
        if (!rockTried) std::printf("\n  no boulder in reach to shoot at\n");

        // ---- ...AND A SHAFT KILLS IN ONE --------------------------------
        //
        // (user 2026-09-15: "have the arrow able to kill life in one shot, just
        // like in v1".) Launched rather than shot from the bow: what is being
        // checked is the SHAFT meeting an animal, and the draw, the loose and
        // the kit slot are a different path with their own test.
        bool arrowKilled = false, arrowTried = false;
        {
            Vec3 at{0, 0, 0};
            if (arrows_.ready() &&
                (nearestLife(Life::Bunny, &at) || nearestLife(Life::Skunk, &at) ||
                 nearestLife(Life::Duck, &at))) {
                const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(at.x + dx / d * 6.0f, at.z + dz / d * 6.0f);
                publishLife();
                pos_ = player_.eyePosition();
                const int slot = lifeHits_.at(world_, at);
                if (slot >= 0) {
                    arrowTried = true;
                    // STRAIGHT AT IT, at the bow's own speed.
                    Vec3 dir = normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
                    arrows_.launch(pos_, Vec3(dir.x * 40.0f, dir.y * 40.0f, dir.z * 40.0f));
                    // THE POPULATION, NOT THE BAND. killLifeAt retires the
                    // member, but its instance keeps whatever the last publish
                    // left in it -- so "still drawn" is not "still alive", and
                    // asking the band here reports a survivor every time.
                    const int loose0 = world_.looseCount();
                    for (int f = 0; f < 30 && !arrowKilled; ++f) {
                        arrows_.update(1.0f / 60.0f, walkWorld(),
                                       [this](const Vec3 &q) { return arrowKill(q); });
                        publishLife();
                        if (!world_.flyerAt(slot, nullptr, nullptr) ||
                            world_.looseCount() > loose0)
                            arrowKilled = true;
                    }
                }
            }
        }
        std::printf("\n  -- a shaft, which v1 says kills outright --\n");
        if (!arrowTried)
            std::printf("  nothing in reach to shoot -- not exercised\n");
        else
            std::printf("  one arrow: %s\n",
                        arrowKilled ? "it went down, correct" : "IT SURVIVED -- WRONG");

        // ---- ...AND THE THREE-BLOW RULE, WHICH THE AXE HIDES -------------
        //
        // Everything above swings an AXE, because that is how a test kills one
        // of twenty-four species without spending a minute on each -- and it
        // means every line of it reports "1 hit", which proves nothing about
        // v1's HITS_TO_KILL. So this does one more kill with a PICK: three
        // blows, flashing after each, and dead on the third and not before.
        int need = 0, gotHits = 0;
        bool needRight = false;
        {
            for (int i = 0; i < held_.count(); ++i)
                if (held_.tool(i).takes == Takes::Stone) held_.select(i);
            Vec3 at{0, 0, 0};
            // A BUNNY: not frail, so it is the case the rule is about. Any of
            // the marchers would do; the bunny is simply the most common.
            if (nearestLife(Life::Bunny, &at)) {
                teleportToLife(at, 5.0f);
                for (int w = 0; w < 120; ++w) {
                    world_.update(player_.pos);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                warmLife(player_.pos, 2);
                publishLife();
                if (nearestLife(Life::Bunny, &at)) {
                    const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                    const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                    teleportTo(at.x + dx / d * 1.6f, at.z + dz / d * 1.6f);
                    publishLife();
                    pos_ = player_.eyePosition();
                    const Vec3 look =
                        normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
                    yaw_ = atan2f(look.x, -look.z) * 180.0f / PI;
                    pitch_ = asinf(look.y) * 180.0f / PI;
                    const int slot = lifeHits_.aim(world_, pos_, forward());
                    if (slot >= 0) {
                        need = kHitsToKill;
                        for (int k = 0; k < 5; ++k) {
                            if (!strikeLife()) break;
                            ++gotHits;
                            if (lifeHits_.dying(slot)) break;
                            // ...AND IT IS STILL ALIVE AND STILL FLASHING,
                            // which is the other half of "every hit flashes".
                            if (!lifeHits_.flashing(slot)) break;
                        }
                        needRight = gotHits == need;
                    }
                }
            }
        }
        // ---- ...AND THE CORPSE DOES NOT LIE THERE -----------------------
        //
        // (user 2026-09-15: "it should stay red while it breaks into peices,
        // then it should dissapear completely as it drops the raw steak".)
        //
        // THE PIECES GO WHEN THE RED DOES. They are spawned at the killing blow
        // and carry the flash for kHurtMs; after that they are a heap of grey
        // lumps where an animal was, and v1 removes them with the poof. Checked
        // by running the clock past the flash and counting what is left.
        {
            // THE AXE, EXPLICITLY. This block sits after the one that selects
            // the PICK to count v1's three blows, so it inherited it -- and a
            // pick's first swing is a WOUND. The strike still returned true
            // (it landed), lastPieces_ still held 7 from the kill above, and
            // the check reported a corpse that was never made. A test that
            // reads a member set by somebody else is a test of nothing.
            for (int i = 0; i < held_.count(); ++i)
                if (held_.tool(i).takes == Takes::Wood) held_.select(i);
            world_.clearDebris(physics_);
            Vec3 at{0, 0, 0};
            int before = 0, after = 0;
            bool did = false;
            // The deepest any piece got below the ground under it, over the
            // whole fall. A corpse that lands is a small negative or a few
            // centimetres; one that goes through the floor is metres.
            float sank = -1e9f;
            // ...AND HOW HIGH ANY OF THEM GOT ABOVE WHERE IT WAS BORN. A piece
            // thrown clear rises a few tens of centimetres; one that started
            // inside the ground and was depenetrated leaves like a rocket, and
            // that is a different number entirely.
            float rose = 0.0f;
            float bornY = 0.0f;
            int redN = 0, plainN = 0;
            // A BUNNY OR A MARCHER -- something big enough that its octants
            // hold more than one voxel. A fly would pass this test by having no
            // corpse to leave.
            if (nearestLife(Life::Bunny, &at) || nearestLife(Life::Skunk, &at) ||
                nearestLife(Life::Mouse, &at)) {
                const float dx = player_.pos.x - at.x, dz = player_.pos.z - at.z;
                const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
                teleportTo(at.x + dx / d * 1.6f, at.z + dz / d * 1.6f);
                publishLife();
                pos_ = player_.eyePosition();
                const Vec3 look = normalize(Vec3(at.x - pos_.x, at.y - pos_.y, at.z - pos_.z));
                yaw_ = atan2f(look.x, -look.z) * 180.0f / PI;
                pitch_ = asinf(look.y) * 180.0f / PI;
                // ...AND IT HAS TO HAVE ACTUALLY DIED. The axe kills in one,
                // but saying so here is what stops this drifting again.
                const int slotC = lifeHits_.aim(world_, pos_, forward());
                (void)slotC;
                if (slotC >= 0 && strikeLife() && lastKilled_) {
                    // THE SHATTER'S OWN COUNT, not the pool's. looseCount can
                    // read zero for an animal too small to make a piece -- an
                    // ant's octants are one voxel each and spawnDebris refuses
                    // those -- and "0 pieces before, 0 after" is a green tick on
                    // a kill that never had a corpse. Same vacuous shape the
                    // flower check had before it was made to go looking.
                    before = lastPieces_;
                    bornY = at.y;
                    world_.debrisKinds(&redN, &plainN);
                    std::printf("  (shatter said %d, the pool holds %d)\n", lastPieces_,
                                world_.looseCount());
                    // Past the flash, on the frame loop's own terms.
                    const float dt = 1.0f / 60.0f;
                    // -- ...AND HOW FAR UNDER THE GROUND ANY OF THEM GOT --
                    //
                    // (user 2026-09-15: "when the life dies and turns red, it
                    // clips straight through the ground".)
                    //
                    // The count going to zero is not enough on its own: a piece
                    // that falls through the world and is then retired reads
                    // exactly like one that landed and was cleared. So every
                    // frame, every live piece is measured against the ground of
                    // its own column and the worst is kept.
                    // LONG ENOUGH FOR THE CAP, not just for the usual case.
                    // A piece that comes to rest ON something -- a rock, a log
                    // -- never meets the terrain height its landing is measured
                    // against, and goes on the 2.5 s backstop instead. At 1.5 s
                    // the test saw that one still lying there and called the
                    // whole rule broken.
                    for (int f = 0; f < 200; ++f) {
                        // THE FLOOR THE SOLVER STANDS ON, MAINTAINED. The frame
                        // loop does this every tick and this test did not: it
                        // teleports to the animal and then steps physics
                        // against a patch still centred where it came from, so
                        // there was no collision floor under the corpse at all
                        // and every piece fell through a world that was not
                        // there. The engine was never asked the question.
                        if (world_.takeGroundDirty() ||
                            !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M,
                                                   kGroundMarginM))
                            rebuildGroundPatch();
                        physics_.step(dt);
                        simMs_ += double(dt) * 1000.0;
                        const WalkWorld ww = walkWorld();
                        world_.updateDebris(
                            physics_, player_.eyePosition(), simMs_,
                            [&](float ax, float az) { return walkGroundM(ww, ax, az); });
                        for (int b = 0; b < kDebrisInstances; ++b) {
                            Vec3 bp{0, 0, 0};
                            float bq[4] = {0, 0, 0, 1};
                            if (!world_.debrisPose(b, &bp, bq)) continue;
                            const float g = walkGroundM(ww, bp.x, bp.z);
                            if (g - bp.y > sank) sank = g - bp.y;
                            if (bp.y - bornY > rose) rose = bp.y - bornY;
                        }
                    }
                    after = world_.looseCount();
                    did = true;
                }
            }
            std::printf("\n  -- the corpse, half a second later --\n");
            if (!did)
                std::printf("  nothing in reach to kill -- not exercised\n");
            else if (before == 0)
                std::printf("  it broke into no pieces at all -- nothing to watch vanish\n");
            else
                std::printf("  pieces at the blow %d, after the red ran out %d  %s\n", before,
                            after,
                            after == 0 ? "gone, correct" : "STILL LYING THERE -- WRONG");
                // 0.2 m, NOT 0.5. A chip is a few voxels: resting ON the
                // ground puts its centre ABOVE the surface, so anything
                // meaningfully positive here is buried. Half a metre passed a
                // corpse that was 0.42 m inside a hillside and called it
                // correct, which is a threshold chosen to make a test go green.
                std::printf("  deepest under the ground %.2f m  %s\n", double(sank),
                            sank < 0.2f ? "rested on it, correct"
                                        : "IT SANK INTO THE GROUND -- WRONG");
                // 2.5 m, not 2.0: the pop was raised to 3.8-5.1 m/s on
                // 2026-09-15 ("have the life pop up more upon death"), which is
                // a rise of up to 1.32 m by itself, and a piece thrown off a
                // bank starts higher than the kill point. The number this is
                // really watching for is the depenetration rocket, and that
                // cleared SEVEN metres.
                std::printf("  highest above the kill   %.2f m  %s\n", double(rose),
                            rose < 2.5f ? "thrown, not fired"
                                        : "A PIECE WAS LAUNCHED -- WRONG");
                // "THERE SHOULD ONLY BE THE BROKEN UP RED PEICES." Counted
                // apart, because a corpse piece and a chip look the same in a
                // total: red ones are scenery and carry hurtT0, and anything
                // else in the pool at a kill is a piece of debris the player
                // can walk up and absorb.
                std::printf("  the pool holds %d red, %d plain  %s\n", redN, plainN,
                            plainN == 0 ? "only the corpse, correct"
                                        : "THERE ARE ORDINARY PIECES TOO -- WRONG");
        }

        std::printf("\n  -- and with a pick, which does not kill outright --\n");
        if (!need)
            std::printf("  no bunny in reach; the three-blow rule was not exercised\n");
        else
            std::printf("  a bunny took %d blows, v1 says %d  %s\n", gotHits, need,
                        needRight ? "correct" : "WRONG");

        std::printf("\n  %d species tried, %d died, %d wrong\n", tried, killed, wrong);
        std::printf("  %s\n", (tried > 0 && wrong == 0 && (!need || needRight))
                                   ? "PASS -- every blow landed, every carcass was right."
                                   : "FAIL -- see the lines above.");
    }


    // -----------------------------------------------------------------------
    // WHAT A BITE OF DIRT DOES THAT A BITE OF STONE DOES NOT.
    //
    // (user 2026-09-14: "digging dirt voxels is very glitchy. just match the
    // pick to stone mechanics, but for shovel to dirt.")
    //
    // THE TWO TOOLS ALREADY SHARE EVERY LINE OF THE EDIT. toolTakes lets the
    // pick at stone and the shovel at soil, and both arrive at the same
    // World::dig with the same radius -- so "match the pick's mechanics" cannot
    // be done by copying a path, because it is already the same path. Whatever
    // differs is in what the bite MEETS, and that is a measurement.
    //
    // Twelve bites into each, reported side by side: how much came out, how
    // many chunks had to be re-meshed, and -- the one that is not symmetric --
    // how many loose BODIES the bite cut free of the world.
    // -----------------------------------------------------------------------
    void runSoilTest() {
        std::printf("\n=== SOIL TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        const float x = player_.pos.x, z = player_.pos.z;
        TerrainMemo memo;
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        const int h = world_.terrainTopAt(ci, cj, memo);
        const auto matAt = [&](int i, int j, int y) {
            TerrainProbe pr(&world_.terrain, &world_.editStore());
            return pr.material(i, j, y);
        };
        std::printf("  standing at (%.0f, %.0f), surface voxel %d\n", x, z, h);
        // WHERE THE CRUST ENDS, which is the whole geometry of the question: a
        // shovel works in a layer a few voxels thick over stone, and a pick
        // works inside a solid that goes down for ever.
        int crust = 0;
        for (int y = h; y > h - 40; --y) {
            const uint8_t m = matAt(ci, cj, y);
            if (isStoneMat(m) || m == mat::BEDROCK) break;
            ++crust;
        }
        std::printf("  the crust here is %d voxels; a bite is %d across\n", crust,
                    kDigRadiusVox * 2 + 1);

        std::printf("\n  %-8s  %-6s  %-7s  %-7s  %s\n", "bite", "spoil", "chunks", "bodies",
                    "what it cut loose");
        for (int pass = 0; pass < 2; ++pass) {
            const bool soil = pass == 0;
            std::printf("  -- %s --\n", soil ? "SHOVEL into the dirt, from the surface down"
                                              : "PICK into the stone, well below it");
            // A FRESH COLUMN for each pass, so the second is not digging in the
            // first one's hole.
            const float bx = x + (soil ? 6.0f : -6.0f), bz = z;
            const int bi = int(std::floor(bx / VOXEL_M)), bj = int(std::floor(bz / VOXEL_M));
            TerrainMemo m2;
            const int top = world_.terrainTopAt(bi, bj, m2);
            int y = soil ? top : (top - crust - 8);
            world_.clearDebris(physics_);
            for (int k = 0; k < 12; ++k, y -= 2) {
                const int before = world_.looseCount();
                spoilN_ = 0;
                const size_t chunks =
                    world_.dig(Vec3(bx, (float(y) + 0.5f) * VOXEL_M, bz), kDigRadiusVox,
                               &spoilVol_, &spoilN_, &spoilAt_);
                int vox = 0;
                for (uint8_t v : spoilVol_)
                    if (v != mat::AIR) ++vox;
                const int bodies = world_.looseCount() - before;
                // ...and how big the biggest of them is, because one chip is
                // ordinary and a slab of hillside coming away is not.
                int biggest = 0;
                for (int b = 0; b < kDebrisInstances; ++b) {
                    const int n = world_.debrisVoxels(b);
                    if (n > biggest) biggest = n;
                }
                std::printf("  %-8d  %-6d  %-7zu  %-7d  %s\n", k + 1, vox, chunks, bodies,
                            bodies > 0 ? (biggest > 200 ? "A SLAB -- this is the glitch"
                                                        : "a chip")
                                       : "nothing");
            }
        }
        // ---- ...AND WHAT THE COLLISION FLOOR DID AROUND THE HOLE ---------
        //
        // THE OTHER HALF OF THE QUESTION, and the half a shovel shows and a
        // pick cannot. groundPatch dilates a dug sample onto its eight
        // neighbours so a narrow shaft has a floor a body can rest on rather
        // than a funnel it slides out of -- see the note there. The samples are
        // 0.4 m apart, so that widens the solver's crater to about 1.2 m, and
        // everything in this world that DIGS AT THE SURFACE leaves its spoil
        // lying inside that radius. A chip resting on ground that is plainly
        // still there, sinking into a hole the eye cannot see, is exactly what
        // "digging dirt is very glitchy" would look like.
        //
        // So: a chip is put down on UNDUG ground beside the pit and watched.
        {
            world_.clearDebris(physics_);
            const float bx = x + 6.0f, bz = z;
            TerrainMemo m3;
            const int bi = int(std::floor(bx / VOXEL_M)), bj = int(std::floor(bz / VOXEL_M));
            const float lip = float(world_.terrainTopAt(bi, bj, m3) + 1) * VOXEL_M;
            std::printf("\n  -- a chip set down beside the pit --\n");
            std::printf("  %-8s  %-9s  %-9s  %s\n", "away", "ground", "rested at", "verdict");
            for (int k = 0; k < 4; ++k) {
                const float away = 0.4f + float(k) * 0.4f;
                const float cx = bx + away, cz = bz;
                TerrainMemo m4;
                const int qi = int(std::floor(cx / VOXEL_M)), qj = int(std::floor(cz / VOXEL_M));
                const float g = float(world_.terrainTopAt(qi, qj, m4) + 1) * VOXEL_M;
                // A LUMP OF THE GROUND IT IS LYING ON, born just above it.
                spoilN_ = 3;
                spoilVol_.assign(27, mat::DIRT);
                const Vec3 still{0.0f, 0.0f, 0.0f};
                const int slot = world_.spawnDebris(physics_, spoilVol_, spoilN_,
                                                    Vec3(cx, g + 0.12f, cz), still, still, simMs_,
                                                    0.0f, nullptr, uint8_t(kDebrisSoil));
                if (slot < 0) continue;
                rebuildGroundPatch();
                const float dt = 1.0f / 60.0f;
                Vec3 pp{0, 0, 0};
                float qq[4] = {0, 0, 0, 1};
                // WHERE THE SOLVER LEFT IT, not where it ended up. Half a
                // second after it is born every small body is COLLECTED -- a
                // kinematic lerp to the player's eye, through anything in the
                // way -- so reading the last frame measures the pickup. The
                // shaft test learned this the same way. See debrisAbsorbing.
                float restY = pp.y;
                for (int f = 0; f < 90; ++f) {
                    if (world_.takeGroundDirty() ||
                        !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M,
                                               kGroundMarginM))
                        rebuildGroundPatch();
                    physics_.step(dt);
                    simMs_ += double(dt) * 1000.0;
                    const WalkWorld ww = walkWorld();
                    world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                        [&](float ax, float az) { return walkGroundM(ww, ax, az); });
                    if (!world_.debrisPose(slot, &pp, qq)) break;
                    if (world_.debrisAbsorbing(slot)) break;
                    restY = pp.y;
                }
                pp.y = restY;
                const float sank = g - pp.y;
                std::printf("  %-8.1f  %-9.2f  %-9.2f  %s\n", double(away), double(g),
                            double(pp.y),
                            sank > 0.25f ? "SANK INTO GROUND THAT IS STILL THERE -- WRONG"
                                         : "rested on the surface");
                world_.clearDebris(physics_);
            }
            (void)lip;
        }

        // ---- ...AND WHETHER A FLOWER OVER THE HOLE CAME DOWN WITH IT -----
        //
        // (user 2026-09-14: "flowers are still floating sometimes. investigate
        // and fix this.")
        //
        // THE SAME SWING, A DIFFERENT LIST. dropUndermined has been dropping
        // trees and rocks and caps since it was written, and it walks the
        // COLLIDERS -- so a flower, which is drawn and walked through and has
        // no collider at all, was never on any list the question was asked of.
        // This digs the ground out from under one and asks whether it is still
        // being drawn.
        {
            Vec3 spot{0, 0, 0};
            int had = 0;
            for (int r = 1; r < 240 && !had; ++r)
                for (int d = -r; d <= r && !had; ++d) {
                    const float o[4][2] = {{float(d), float(-r)}, {float(d), float(r)},
                                           {float(-r), float(d)}, {float(r), float(d)}};
                    for (int k = 0; k < 4 && !had; ++k) {
                        const Vec3 c(x + o[k][0] * VOXEL_M, 0.0f, z + o[k][1] * VOXEL_M);
                        const int m = world_.scatterShownNear(c, 0.25f);
                        if (!m) continue;
                        TerrainMemo fm;
                        const int fi = int(std::floor(c.x / VOXEL_M));
                        const int fj = int(std::floor(c.z / VOXEL_M));
                        spot = Vec3(c.x, float(world_.terrainTopAt(fi, fj, fm)) * VOXEL_M, c.z);
                        had = m;
                    }
                }
            std::printf("\n  -- a flower over a fresh hole --\n");
            if (!had) {
                std::printf("  no scatter in reach -- this run proved nothing\n");
            } else {
                world_.clearDebris(physics_);
                const int loose0 = world_.looseCount();
                // STRAIGHT DOWN THROUGH THE COLUMN IT STANDS ON. One bite
                // takes the surface and the next three take what is under it,
                // so there is nothing left within the three voxels of slack
                // dropScatterUndermined allows for a slope.
                for (int k = 0; k < 4; ++k) {
                    const Vec3 at2(spot.x, spot.y - float(k) * 3.0f * VOXEL_M, spot.z);
                    world_.dig(at2, kDigRadiusVox, &spoilVol_, &spoilN_, &spoilAt_);
                    world_.dropScatterUndermined(physics_, at2,
                                                 float(kDigRadiusVox) * VOXEL_M + 0.4f, simMs_);
                }
                const int after = world_.scatterShownNear(spot, 0.25f);
                std::printf("  standing before  %d\n", had);
                std::printf("  still drawn      %d  %s\n", after,
                            after == 0 ? "it came down, correct"
                                       : "STILL IN THE AIR OVER A HOLE -- WRONG");
                std::printf("  bodies it left   %d\n", world_.looseCount() - loose0);
            }
        }

        std::printf("\n  Read the BODIES column. A bite that cuts the world loose spawns one;\n");
        std::printf("  a bite fully inside a solid spawns none.\n");
    }

    void runFloatTest() {
        std::printf("\n=== UNDERMINE TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        std::printf("  spawn (%.1f, %.1f, %.1f)\n", player_.pos.x, player_.pos.y,
                    player_.pos.z);

        // Not named `near`; see the note in runFellTest.
        std::vector<Solid> around;
        world_.collidersNear(player_.pos, 120.0f, &around);
        int tried = 0, fell = 0, stood = 0;
        for (int kind = 0; kind <= 1; ++kind) {
            const Solid *pick = nullptr;
            float best = 1e30f;
            for (const Solid &s2 : around) {
                if (s2.modelKind != kind || !s2.vol || s2.hx <= 0.0f) continue;
                const float dx = s2.cx - player_.pos.x, dz = s2.cz - player_.pos.z;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; pick = &s2; }
            }
            if (!pick) {
                std::printf("  no %s within reach of the spawn\n", kind ? "rock" : "tree");
                continue;
            }
            const Solid so = *pick;
            ++tried;
            std::printf("\n  %s at (%.1f, %.1f, %.1f)  model %d  %d x %d voxels\n",
                        kind ? "rock" : "tree", so.tx, so.baseY, so.tz, int(so.modelIndex),
                        int(so.msx), int(so.msz));
            // The patch follows the player, and something about to fall needs a
            // floor -- the same lesson runFellTest paid for.
            player_.placeOnGround(walkWorld(), so.cx, so.cz);
            if (world_.takeGroundDirty() ||
                !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                rebuildGroundPatch();

            // ---- dig out every column it is standing on --------------------
            //
            // A metre down, with the real brush, one bite per column of the
            // model\'s own row zero. That is what a player levelling a spot does
            // and it is what left forty trees hanging in float_probe.
            long blows = 0;
            bool down = false;
            TerrainMemo digMemo;
            for (int mz = 0; mz < int(so.msz) && !down; ++mz)
                for (int mx = 0; mx < int(so.msx) && !down; ++mx) {
                    if (!solidVoxel(so, mx, 0, mz)) continue;
                    float wx = 0.0f, wz = 0.0f;
                    solidWorldSpace(so, (float(mx) + 0.5f) * VOXEL_M, (float(mz) + 0.5f) * VOXEL_M,
                                    &wx, &wz);
                    // FROM THE SURFACE DOWN, not from the model's base down.
                    // A placement is SUNK into the terrain, so the ground
                    // beside its row zero stands well above that row -- digging
                    // from the base leaves the shoulder in place and the model
                    // is still standing on it. The first run of this spent 128
                    // blows and moved nothing for exactly that reason.
                    const int ci = int(std::floor(wx / VOXEL_M));
                    const int cj = int(std::floor(wz / VOXEL_M));
                    const float topY =
                        float(world_.terrainTopAt(ci, cj, digMemo) + 1) * VOXEL_M;
                    for (float y = topY; y > so.baseY - 0.4f && !down; y -= 0.25f) {
                        const Vec3 p{wx, y, wz};
                        if (!world_.dig(p, kDigRadiusVox)) continue;
                        ++blows;
                        down = world_.dropUndermined(physics_, p, simMs_);
                    }
                }
            if (!down) {
                ++stood;
                std::printf("  FAIL -- %ld blows took the ground away and it is still there\n",
                            blows);
                continue;
            }
            ++fell;
            std::printf("  came down after %ld blows\n", blows);

            // ...and then it is a body like any other. Watch it settle.
            const float dt = 1.0f / 60.0f;
            for (int f = 0; f < 300; ++f) {
                if (!physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                    rebuildGroundPatch();
                physics_.step(dt);
                simMs_ += double(dt) * 1000.0;
                world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                    [&](float x, float z) {
                                        return float(world_.terrain.heightVox(
                                                         int(std::floor(x / VOXEL_M)),
                                                         int(std::floor(z / VOXEL_M))) +
                                                     1) *
                                               VOXEL_M;
                                    });
                if ((f % 60) != 0 && f != 299) continue;
                for (int i = 0; i < 512; ++i) {
                    Vec3 p{0, 0, 0}, lin{0, 0, 0}, ang{0, 0, 0};
                    float q[4] = {0, 0, 0, 1};
                    if (!world_.debrisPose(i, &p, q)) continue;
                    world_.debrisVel(physics_, i, &lin, &ang);
                    const float uy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
                    const float pitch =
                        acosf(uy < -1.0f ? -1.0f : (uy > 1.0f ? 1.0f : uy)) * 57.29578f;
                    std::printf("    %5.0f ms   y %7.2f   pitch %5.1fd   fall %5.2f m/s\n",
                                double(f) * dt * 1000.0, p.y, pitch, -lin.y);
                    break;
                }
            }
        }
        // ---- ...AND WHAT A BLOW ON A MODEL LEAVES BEHIND -----------------
        //
        // Asked of the ENGINE: real swings through World::carveModel, then a
        // per-voxel flood of the instance that took them, against the same
        // model as it was drawn. Six voxels a blow before dropModelHangers.
        std::printf("\n  --- a blow on a model ---\n");
        long modelBlows = 0, modelLeft = 0;
        for (int kind = 0; kind <= 1; ++kind) {
            for (const Solid &s2 : around) {
                if (s2.modelKind != kind || !s2.vol || s2.hx <= 0.0f) continue;
                const Solid so2 = s2;
                // ---------------------------------------------------------
                // FROM ONE SIDE, SWEEPING ACROSS THE CUT. Not round and round.
                //
                // This used to walk a full circle -- 45 degrees per blow, a
                // different face of the trunk every time -- and a tree cut like
                // that is never cut THROUGH. fellTree therefore never fired,
                // the loop never broke, and every blow counted the whole canopy
                // as "standing on nothing": a flat 31,450 voxels, which is the
                // number the note below this loop already warns about. The test
                // was measuring its own aim.
                //
                // runFellTest solves the same problem the same way and says
                // why: "every blow from the same point along the same ray eats
                // a TUNNEL through the wood, and a tunnel severs nothing", so a
                // player's aim wanders across the cut and this does too. One
                // side, one direction, a hand's width of wander.
                //
                // A ROCK IS UNAFFECTED. It is not severable, fellTree declines
                // it, and what this measures there -- chips left hanging in the
                // stone -- never depended on the angle.
                // ---------------------------------------------------------
                // SIXTY, WHICH IS runFellTest's BUDGET AND FOR ITS REASON.
                // 24 was enough for most trees and not for all of them, and a
                // tree that has been cut through but not felled reports its
                // whole canopy as hanging -- so the run-to-run variance in
                // which tree is nearest the spawn showed up as this test
                // passing and failing on alternate runs with nothing changed.
                // Measured: 0 voxels at a pinned spawn, 58,708 at a random one,
                // same binary.
                const float ang0 = 0.7853982f;   // one side, and it stays that side
                bool camedown = false;
                long kindLeft = 0;
                for (int b = 0; b < 60; ++b) {
                    const float wob = (float(b % 11) - 5.0f) * 0.12f;
                    const Vec3 eye{so2.cx + std::cos(ang0) * (so2.hx + 3.0f) - std::sin(ang0) * wob,
                                   so2.baseY + 1.2f + float(b % 3) * 0.1f,
                                   so2.cz + std::sin(ang0) * (so2.hz + 3.0f) + std::cos(ang0) * wob};
                    const Vec3 dir{-std::cos(ang0), 0.0f, -std::sin(ang0)};
                    if (!world_.carveModel(so2, eye, dir, 12.0f, kDigRadiusVox)) continue;
                    ++modelBlows;
                    // THE WHOLE SWING PATH, not half of it. carveModel alone
                    // leaves a severed limb sitting in the model, because it is
                    // fellTree that hands a piece that big to the solver -- and
                    // a test that skips it measures 31,450 voxels of its own
                    // omission, which is what the first run of this did.
                    if (world_.fellTree(physics_, so2, dir, simMs_)) { camedown = true; break; }
                    // INTO A LOCAL, and committed below only if this model
                    // actually came down -- see the note after the loop.
                    const long l = world_.looseVoxelsNow(so2);
                    if (l > kindLeft) kindLeft = l;
                }
                // ...AND IF IT NEVER CAME DOWN, SAY SO RATHER THAN COUNTING IT.
                // A tree still standing has its crown connected through its own
                // trunk; a tree that has been severed and refused a body has
                // the whole crown loose, and the number that produces is the
                // canopy's size, not a floating-geometry bug. Reporting it as
                // one is how this test cried wolf.
                if (kind == 0 && !camedown)
                    std::printf("  (the tree did not come down in 60 blows -- its crown is not"
                                " counted; see the note in this loop)\n");
                else if (kindLeft > modelLeft)
                    modelLeft = kindLeft;
                break;   // one of each kind is enough; the flood is the slow part
            }
        }
        std::printf("  %ld blows on a tree and a rock: worst %ld voxels left standing"
                    " on nothing\n",
                    modelBlows, modelLeft);
        // ---- WHAT A NON-ZERO HERE MEANS, AND WHAT IT DOES NOT -------------
        //
        // THE TWO FLOODS RUN AT DIFFERENT GRAIN, and that is the whole of it.
        // fellTree decides what has come away using COARSE cells
        // (kSeverCell); this counts FINE voxels. A coarse cell is solid if
        // any voxel in it is, so the coarse flood leaks across a one-voxel gap
        // the fine one stops at -- and a blow can therefore disconnect tens of
        // thousands of fine voxels while the sever test correctly reports that
        // nothing structural has come off.
        //
        // So this number is real -- those voxels ARE standing on nothing, and
        // nothing-floats says they should fall -- but it is a KNOWN GAP in the
        // grain of the sever test, not a regression in whatever was last
        // changed. It surfaces on some spawns and not others because it needs
        // a blow that cuts a fine bridge without cutting a coarse one.
        //
        // Closing it means running the sever flood at voxel grain, or coarse
        // first and fine inside the cells the coarse pass calls contested. It
        // is not a one-line fix and it has never been attempted.
        if (modelLeft > 0)
            std::printf("  (coarse sever vs fine flood -- see the note here before blaming\n"
                        "   whatever changed last)\n");

        // ---- ...AND THE GROUND ITSELF ------------------------------------
        //
        // Asked of the ENGINE, through the same terrainSolidAt every other
        // consumer uses, rather than of a reimplementation of it. Dig the way a
        // player digs, then flood the ground around the hole from what is
        // provably standing on bedrock and count what the flood cannot reach.
        std::printf("\n  --- the ground ---\n");
        world_.collidersNear(player_.pos, 120.0f, &around);   // the set moved on
        long sites = 0, siteFloat = 0, voxFloat = 0, worstSite = 0;
        double digNs = 0.0;
        long digN = 0;
        uint32_t rs = 20260909u;
        auto rnd = [&]() {
            rs ^= rs << 13;
            rs ^= rs >> 17;
            rs ^= rs << 5;
            return rs;
        };
        TerrainMemo tmemo;
        for (int site = 0; site < 40; ++site) {
            const float sx = player_.pos.x + float(int(rnd() % 400u)) - 200.0f;
            const float sz = player_.pos.z + float(int(rnd() % 400u)) - 200.0f;
            const int ci = int(std::floor(sx / VOXEL_M));
            const int cj = int(std::floor(sz / VOXEL_M));
            const int h = world_.terrain.heightVox(ci, cj, tmemo);
            // Thirty blows, clustered, exactly as float_probe's sweep is --
            // and TIMED, because dropTerrainHangers runs inside every one of
            // them and hitching on a pick swing is a live complaint.
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 30; ++b) {
                const int px = ci + int(rnd() % 13u) - 6;
                const int pz = cj + int(rnd() % 13u) - 6;
                const int py = h + int(rnd() % 13u) - 9;
                world_.dig(Vec3{(float(px) + 0.5f) * VOXEL_M, (float(py) + 0.5f) * VOXEL_M,
                                (float(pz) + 0.5f) * VOXEL_M},
                           kDigRadiusVox);
            }
            digNs += double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
            digN += 30;
            ++sites;
            // ...and now count what is left standing on nothing.
            const int R = 24, n2 = R * 2 + 1;
            const int i0 = ci - R, j0 = cj - R, y0 = h - R - 6;
            std::vector<uint8_t> sol(size_t(n2) * n2 * n2, 0), seen(size_t(n2) * n2 * n2, 0);
            auto ix = [&](int a, int bb, int c) {
                return size_t(a) + size_t(c) * size_t(n2) + size_t(bb) * size_t(n2) * size_t(n2);
            };
            long solid = 0;
            for (int c = 0; c < n2; ++c)
                for (int a = 0; a < n2; ++a)
                    for (int bb = 0; bb < n2; ++bb)
                        if (world_.terrainSolidAt(i0 + a, j0 + c, y0 + bb, tmemo)) {
                            sol[ix(a, bb, c)] = 1;
                            ++solid;
                        }
            std::vector<int> st;
            for (int c = 0; c < n2; ++c)
                for (int a = 0; a < n2; ++a)
                    if (sol[ix(a, 0, c)]) {
                        seen[ix(a, 0, c)] = 1;
                        st.push_back(int(ix(a, 0, c)));
                    }
            long reached = long(st.size());
            static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            while (!st.empty()) {
                const int p = st.back();
                st.pop_back();
                const int a = p % n2, c = (p / n2) % n2, bb = p / (n2 * n2);
                for (const int *o : off) {
                    const int x2 = a + o[0], y2 = bb + o[1], z2 = c + o[2];
                    if (x2 < 0 || y2 < 0 || z2 < 0 || x2 >= n2 || y2 >= n2 || z2 >= n2) continue;
                    const size_t q = ix(x2, y2, z2);
                    if (!sol[q] || seen[q]) continue;
                    seen[q] = 1;
                    ++reached;
                    st.push_back(int(q));
                }
            }
            // COUNTED IN THE INTERIOR ONLY. A voxel on this box's own wall may
            // be held up by stone just outside it, which the flood cannot see,
            // so counting it would be measuring the ruler rather than the wood.
            long f2 = 0;
            for (int bb = 1; bb < n2 - 1; ++bb)
                for (int c = 1; c < n2 - 1; ++c)
                    for (int a = 1; a < n2 - 1; ++a)
                        if (sol[ix(a, bb, c)] && !seen[ix(a, bb, c)]) ++f2;
            (void)solid;
            (void)reached;
            if (f2 > 0) {
                ++siteFloat;
                voxFloat += f2;
                if (f2 > worstSite) worstSite = f2;
            }
        }
        std::printf("  %.3f ms per blow, dig and the hanger sweep together (%ld blows)\n", 
                    digN ? digNs / double(digN) / 1e6 : 0.0, digN);
        std::printf("  %ld dig sites, 30 blows each: %ld left ground hanging, %ld voxels,"
                    " worst %ld\n",
                    sites, siteFloat, voxFloat, worstSite);

        std::printf("\n=== VERDICT ===\n");
        std::printf("  %d undermined, %d came down, %d left standing on nothing\n", tried, fell,
                    stood);
        std::printf("  models: %ld voxels left hanging over %ld blows\n", modelLeft,
                    modelBlows);
        std::printf("  ground: %ld voxels of terrain standing on nothing\n", voxFloat);
        std::printf("  %s\n", (stood || voxFloat || modelLeft > 0)
                                     ? "FAIL"
                                     : "PASS -- nothing is left in the air.");
    }

    // -----------------------------------------------------------------------
    // TICK EVERY POPULATION AT A POINT, the way one frame of the wood does.
    //
    // WHY A WARMUP EXISTS AT ALL is written out at length in the offline
    // report, which does the same thing for the same reason: a population that
    // FILLS ITSELF OVER TIME reports its spawn state if you tick it once, and
    // most of these have a settling time measured in tens of seconds -- the
    // flying flock is born on a ring at 0.78-0.94 of 105 m and needs to cross
    // it before it is distributed the way play sees it.
    //
    // ONE LOOP RATHER THAN SIX, which is the one thing this does differently:
    // the report warms each population separately with its own count because
    // it is making a PICTURE and each system has its own settling time. A test
    // of where the animals ARE wants the state a running frame produces, and
    // that is all six advancing on one clock.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // EVERY POPULATION ONTO THE BAND, IN ONE CALL.
    //
    // The frame loop publishes them where each is ticked, which is six places;
    // a headless test ticks them all in one loop (warmLife) and then has to do
    // the same. It exists because --kill-test asks the BAND what is under the
    // crosshair -- see render/lifehit.h -- and an animal that has not been
    // published is, as far as that question is concerned, not there.
    // -----------------------------------------------------------------------
    void publishLife() {
        flock_.publish(world_);
        birds_.publish(world_);
        lake_.publish(world_);
        flock2_.publish(world_, kButterflySlots + kBirdSlots + kLakeSlots);
        bunnies_.publish(world_, kBunnySlot0);
        bunnies_.publishSkunks(world_, kMarchSlot0);
        bees_.publish(world_, kBeeSlot0);
        critters_.publish(world_, kCritterSlot0);
        world_.flushFlyerInstances();
    }

    void warmLife(const Vec3 &at, int seconds) {
        const auto groundAt = [this](float x, float z) {
            return float(world_.terrain.heightVox(int(std::floor(x / VOXEL_M)),
                                                  int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
        };
        world_.collidersNear(at, kBirdKeepM, &perches_);
        world_.decorNear(5, at, kBeeHiveM, &hivesNear_);
        world_.decorNear(2, at, kBeeHiveM + kBeeFlowerM, &bloomsNear_);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < seconds * 60; ++i) {
            flock_.update(dt, world_, at);
            birds_.update(dt, perches_, at);
            lake_.update(dt, world_.terrain, at);
            flock2_.update(dt, at, groundAt, &perches_);
            bunnies_.update(dt, at, groundAt,
                            [this](float x, float z) { return wetColumnAt(x, z); }, perches_,
                            [this](float x) { return world_.terrain.birchMix(x); },
                            // ...AND NOT ON THE BEACH -- see Bunnies::blocked.
                            [this](float x, float z) { return sandAt(x, z); });
            bees_.update(dt, at, hivesNear_, bloomsNear_, &perches_);
            lake_.bankSpots(uint32_t(frameTick_), 8, &banksNear_);
            critters_.update(dt, at, groundAt,
                             [this](float x, float z) { return wetColumnAt(x, z); },
                             [this](float x) { return world_.terrain.birchMix(x); }, banksNear_,
                             perches_, isNight(), Vec3(0.0f, 0.0f, 0.0f),
                             [this](float x, float z) { return waterTopAt(x, z); });
        }
    }

    // -----------------------------------------------------------------------
    // PUT A SEED IN THE GROUND UNDER THE CROSSHAIR.
    //
    // (user 2026-09-14: "have it where the player can right click tilled land
    // with seeds to place down seeds. however if there is no water present, the
    // tilled land goes back to dirt/grass like it does currently.")
    //
    // WHAT PLANTING ACTUALLY IS HERE: the till's revert is CANCELLED. Turned
    // earth grows back over after kTillSec "since nothing was ever planted in
    // it" -- v1's own words for its own rule -- so the whole of "something has
    // been planted in it" is that the clock stops. Nothing new is drawn, no
    // second kind of voxel exists, and there is no crop system behind this.
    //
    // AND ONLY IF THERE IS WATER. That is the second half of the ask and it is
    // what makes a seed bed a PLACE rather than a thing you do anywhere: a bed
    // out of reach of water is turned earth with a seed in it, and turned earth
    // grows back. So a dry planting costs you the seed and the bed reverts on
    // its own clock, exactly as it does now.
    //
    // THE SEED IS SPENT EITHER WAY, which is deliberate. Refusing to plant
    // without water would need the rule explained in the moment -- there is no
    // UI here to explain it in -- and "I planted it and it did not take" is a
    // thing a player can see happen and learn from.
    // -----------------------------------------------------------------------
    bool plantSeed() {
        if (seedsTool_ < 0 || !held_.ready()) return false;
        if (held_.selected() != seedsTool_) return false;
        if (!held_.tool(seedsTool_).carried) return false;

        const Vec3 eye = player_.eyePosition(), dir = forward();
        const Swing aim = swingRay(walkWorld(), eye, dir);
        // TILLED ONLY, NOT SEEDED. A bed that already has a seed in it is not
        // a bed to put a second one in -- and saying so here is what stops a
        // held stack draining into one square metre of dirt.
        if (!aim.hit || aim.kind != Swing::Ground || aim.material != mat::TILLED) return false;

        const bool wet = waterWithin(aim.point, kSeedWaterM);
        const size_t held = world_.plantAt(aim.point, kTillRadiusM, wet);
        if (!held) return false;

        // ONE SEED PER PLANTING, taken through the same door a drop leaves by:
        // dropSelected is what decrements a stack and gives the slot up when
        // the last one goes. See HeldItem::take.
        held_.dropSelected();
        std::printf("v2: planted at (%.1f, %.1f, %.1f) -- %s, %zu columns\n", aim.point.x,
                    aim.point.y, aim.point.z,
                    wet ? "water in reach, the bed keeps" : "NO WATER, it will grow back over",
                    held);
        std::fflush(stdout);
        return true;
    }

    // IS THERE WATER WITHIN REACH OF THIS BED. A ring search rather than a
    // square, and it stops at the first wet column -- the answer is a bool and
    // the nearest one is not more true than any other.
    bool waterWithin(const Vec3 &at, float reachM) {
        for (float r = 0.0f; r <= reachM; r += 0.5f) {
            const int steps = (r < 0.5f) ? 1 : maxi(8, int(2.0f * PI * r / 0.5f));
            for (int k = 0; k < steps; ++k) {
                const float a = float(k) / float(steps) * 2.0f * PI;
                if (wetColumnAt(at.x + cosf(a) * r, at.z + sinf(a) * r)) return true;
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // A SHAFT FINDS SOMETHING ALIVE -- v1's one-shot bow.
    //
    // The same consequences a killing blow has, in the same order, and reached
    // through the same two calls -- so an arrow cannot drift away from what an
    // axe does. What it does NOT share is the reach test: a melee swing asks
    // what is under the crosshair within 5.3 m, and an arrow is simply AT the
    // point it has flown to.
    // -----------------------------------------------------------------------
    bool arrowKill(const Vec3 &p) {
        if (!world_.flyersLoaded()) return false;
        const int slot = lifeHits_.at(world_, p);
        if (slot < 0) return false;
        const LifeKind kind = lifeKindAt(slot);
        if (!kind.alive()) return false;
        // ONE SHOT, WHATEVER IT IS. v1's arrow count is 1 for everything.
        const LifeHits::Blow b = lifeHits_.strike(world_, slot, kind, /*oneBlow=*/true, simMs_);
        if (!b.landed) return false;
        particles_.hitSparks(b.at, simMs_, /*red=*/true);
        if (!b.killed) return true;
        lastPieces_ = world_.shatterFlyer(physics_, slot, p, simMs_);
        particles_.deathBurst(b.at, simMs_);
        if (kind.meat) dropMeatAt(b.at);
        killLifeAt(slot);
        lifeHits_.clear(slot);
        std::printf("v2: an arrow killed the %s -- %d pieces%s\n", kind.name, lastPieces_,
                    kind.meat ? ", left a steak" : "");
        std::fflush(stdout);
        return true;
    }

    // -----------------------------------------------------------------------
    // A SHAFT TAKES A CHIP OUT OF WHAT IT HIT -- v1's arrowChop.
    //
    // (user 2026-09-15: "have arrow take out tiny peices of material. v1 did
    // this. the peice should be as big as the knife chunk as seen in v1".)
    //
    // HOW BIG IS "THE KNIFE CHUNK". v1 measures a bite as a COUNT of voxels
    // lifted out of a sphere, and the knife's is
    //
    //     max(4, round(chopBite * KNIFE_BITE * KNIFE_SCALE))
    //         = max(4, round(30 * 0.5 * 0.5)) = 8 voxels
    //
    // (v1's own arrow takes ARROW_CHOP_BITE = 10, so the user's "as big as the
    // knife chunk" is asking for slightly less than v1's arrow, not for a
    // different order of thing.)
    //
    // THIS ENGINE HAS NO BITE COUNT -- a carve here is the whole sphere -- so
    // the size is set by the RADIUS, and the radius had to be MEASURED rather
    // than worked out. The arithmetic says a radius-1 sphere is the 7 cells
    // within one of the centre, which looked like the answer; the engine says
    //
    //     radius 1   3 voxels     radius 2   5 voxels     kDigRadiusVox 3   113
    //
    // because the sphere is centred on the voxel the shaft went INTO, which is
    // at the surface -- so a good half of it is the air the arrow flew through,
    // and how much of the rest is stone depends on how square-on it struck.
    // Radius 2 is what actually lands nearest v1's eight.
    //
    // AGAINST THE SWING'S 113, which is the ratio that matters: an axe takes
    // twenty times this out of the same boulder. A shaft chips; it does not
    // dig, and there is no radius here that could be mistaken for one.
    //
    // NOTHING ALIVE IS CARVED, which is v1's rule stated the same way ("a
    // creature in the way cancels the chop outright") and here it is free --
    // Arrows only records an impact for a shaft that struck the WORLD, because
    // one that struck an animal took the kill branch and never reached this.
    // -----------------------------------------------------------------------
    static constexpr int kArrowChipVox = 2;

    void arrowChip(const Vec3 &at, const Vec3 &dir) {
        // -- WHAT IT WAS, ASKED THE WAY A SWING ASKS ------------------------
        //
        // Backing off along the shaft and re-probing, rather than inventing a
        // second classifier. swingRay is what decides rock-or-trunk-or-ground
        // for every blow in the game, and a chip that classified its own
        // material would be a second opinion to keep in step with the first --
        // which is the shape of every material bug in this file.
        //
        // 0.4 m BACK because that is comfortably more than the 24 cm substep
        // the impact point was found on, so the ray starts outside the surface
        // whatever angle the shaft came in at; and swingRay's reach is metres,
        // so a target 40 cm ahead is never out of range.
        const Vec3 from = at - dir * 0.4f;
        const Swing sw = swingRay(wideWalkWorld(kArrowSolidsM), from, dir);
        if (!sw.hit) return;

        // The swing's own three arms, at the chip's radius. Braced exactly as
        // they are there, and for the reason written over them: an else that
        // takes only the assignment, or a test squeezed between the arms, has
        // broken that chain twice.
        std::vector<uint8_t> vol;
        int n = 0;
        Vec3 spoilAt{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        bool dug = false;
        if (sw.kind == Swing::Loose) {
            dug = world_.carveDebris(physics_, lastDebris_, kArrowChipVox, simMs_, &vol, &n,
                                     &spoilAt, &yaw);
        } else if (sw.kind == Swing::Rock || sw.kind == Swing::Trunk) {
            dug = world_.carveModel(sw.solid, sw.eye, sw.dir, sw.reach, kArrowChipVox, &vol, &n,
                                    &spoilAt, &yaw);
        } else {
            yaw = 0.0f;   // terrain is not turned
            dug = world_.dig(sw.point, kArrowChipVox, &vol, &n, &spoilAt);
        }
        if (!dug || n <= 0) return;

        // ...AND THE CHIP IS A BODY, exactly as a swing's is: dropped where it
        // came from with no throw and no spin, stopped by the voxels it was cut
        // out of, then collected off the ground. See the long note over the
        // swing's own spawnDebris for why nothing pushes it anywhere.
        const Vec3 kNoVel{0.0f, 0.0f, 0.0f};
        const Solid *srcRock =
            (sw.kind == Swing::Rock || sw.kind == Swing::Trunk) ? &sw.solid : nullptr;
        const uint8_t takesAs = sw.kind == Swing::Loose  ? lastDebris_.takes
                                : sw.soft                ? uint8_t(kDebrisSoft)
                                : sw.kind == Swing::Trunk ? uint8_t(kDebrisWood)
                                : sw.kind == Swing::Rock  ? uint8_t(kDebrisStone)
                                : isSoilMat(sw.material)  ? uint8_t(kDebrisSoil)
                                                          : uint8_t(kDebrisStone);
        // ...AND IT LIES THERE UNTIL YOU WALK UP TO IT. The last argument is
        // the whole of that: see kArrowAbsorbM, and v1's note on why a piece
        // knocked off from across the clearing must be no easier to collect
        // than one you cut standing over it.
        lastChipSlot_ = world_.spawnDebris(physics_, vol, n, spoilAt, kNoVel, kNoVel, simMs_,
                                           yaw, srcRock, takesAs, kArrowAbsorbM);
        lastChipAt_ = spoilAt;
        lastChipN_ = n;   // --kill-test reads these two; nothing else does
    }

    // -----------------------------------------------------------------------
    // A CARCASS LEAVES ONE STEAK -- v1's dropMeat.
    //
    // (user 2026-09-14: "then lastly, have life drop a raw steak".)
    //
    // AN ORDINARY DROP, which is v1's own choice and its own words: "lands as
    // an ordinary drop, so it hovers, spins and can be picked up like anything
    // else". Nothing about meat needs a second kind of object -- the wheat and
    // the seeds already come off the world this way.
    //
    // AT THE ANIMAL, which for a fish means ON THE WATER. v1 special-cases that
    // and says why: every other kill drops at the ground height of the column,
    // and for a fish that is the SEABED, so the meat would sink out of sight
    // under however many voxels it was swimming in -- "the one drop the player
    // cannot walk to is the one they have to swim down for". Drops::spill takes
    // a world point and the body it came off was AT the surface it died at, so
    // handing it the animal's own position is that rule for free.
    // -----------------------------------------------------------------------
    void dropMeatAt(const Vec3 &at) {
        if (steakTool_ < 0) return;
        const Tool &st = held_.tool(steakTool_);
        if (st.models.empty()) return;
        // -- ...AND A FISH LEAVES ITS MEAT ON THE SURFACE -------------------
        //
        // (user 2026-09-15: "when killing a fish have the raw steak float above
        // the water. just like in v1".)
        //
        // v1 SPECIAL-CASES THIS AND SAYS WHY: every other kill drops at the
        // ground height of the column, and for a fish that is the SEABED -- so
        // the meat sinks out of sight under however many voxels it was swimming
        // in, and "the one drop the player cannot walk to is the one they have
        // to swim down for".
        //
        // BY THE WATER RATHER THAN BY THE SPECIES, which is the same rule said
        // in terms this engine can check anywhere: if there is water over the
        // point the animal died at, the meat belongs on top of it. That covers
        // the fish, and it also covers a duck shot over a lake and a frog on a
        // bank, none of which had to be named.
        // MOVING THE LAUNCH WAS NOT ENOUGH, and that is the whole of why the
        // first cut of this still put the steak on the seabed. A spill FLIES,
        // and Drops ended every arc at the terrain height -- so lifting the
        // start to the waterline just meant the meat was released at the
        // surface and fell through it. The floor is the half that matters; see
        // Drops::Item::floorY.
        Vec3 p = at;
        const float top = waterTopAt(at.x, at.z);
        float floorY = -1e9f;
        if (top > at.y) {
            p.y = top;
            floorY = top;
        }
        drops_.spill(steakTool_, st.models[0], st.sx, st.sy, st.sz, p, 0.0f, floorY);
    }

    // -----------------------------------------------------------------------
    // WHAT IS IN THIS BAND SLOT, WITH THE ONE THING THE TABLE CANNOT KNOW.
    //
    // render/lifehit.h answers from the band's LAYOUT, which is exact for every
    // population but one: the six marchers share a run and which one a slot
    // holds is a property of the marcher, not of the slot. A worm and a skunk
    // are neighbours in it, and one of them leaves a carcass.
    // -----------------------------------------------------------------------
    LifeKind lifeKindAt(int slot) const {
        LifeKind k = lifeAtSlot(slot);
        if (!k.alive() || slot < kMarchSlot0 || slot >= kMarchSlot0 + kMarchSlots) return k;
        switch (bunnies_.marchKindAt(slot - kMarchSlot0)) {
            case kMarchSkunk:      return {"skunk", true, false};
            case kMarchArmadillo:  return {"armadillo", true, false};
            case kMarchPorcupine:  return {"porcupine", true, false};
            case kMarchMouse:      return {"mouse", true, false};
            // v1's WORM band: one hit, and nothing left behind.
            case kMarchWorm:       return {"worm", false, true};
            case kMarchSnake:      return {"snake", true, false};
            default:               return {};
        }
    }

    // -----------------------------------------------------------------------
    // ...AND WHICH POPULATION OWNS IT.
    //
    // The same switch shape as nearestLife, and it exists for the same reason:
    // the one thing a band slot cannot tell you is which container the animal
    // standing in it came out of. Six classes, one line each.
    // -----------------------------------------------------------------------
    bool killLifeAt(int slot) {
        if (slot < 0 || slot >= kFlyerInstances) return false;
        int s = slot;
        if (s < kButterflySlots) return flock_.killSlot(s);
        s -= kButterflySlots;
        if (s < kBirdSlots) return birds_.killSlot(s);
        s -= kBirdSlots;
        if (s < kLakeSlots) return lake_.killSlot(s);
        s -= kLakeSlots;
        if (s < kFlockSlots) return flock2_.killSlot(s);
        s -= kFlockSlots;
        if (s < kBunnySlots) return bunnies_.killSlot(s);
        s -= kBunnySlots;
        if (s < kMarchSlots) return bunnies_.killMarcher(s);
        s -= kMarchSlots;
        if (s < kBeeSlots) return bees_.killSlot(s);
        s -= kBeeSlots;
        if (s < kButtonSlots) return false;   // a button is not alive
        s -= kButtonSlots;
        if (s < kCritterSlots) return critters_.killSlot(s);
        return false;   // the particles
    }

    // -----------------------------------------------------------------------
    // A BLOW ON A LIVING THING -- v1's hitCreature, with v2's consequences.
    //
    // Returns whether the swing landed on an animal at all, which is what
    // spends it. Everything below the split is what v1 does in the same order:
    // the sparks and the flash on EVERY blow, wounding or fatal, and the
    // carcass, the poof and the meat only on the one that kills.
    //
    // THE AXE KILLS OUTRIGHT, which is v1's rule for its own axe: "the axe is
    // the killing tool and ignores this entirely". Everything else -- a pick, a
    // shovel, a hoe, an empty hand -- wears the animal down over three, and the
    // frail species die to anything in one.
    // -----------------------------------------------------------------------
    bool strikeLife() {
        if (!world_.flyersLoaded()) return false;
        const int slot = lifeHits_.aim(world_, pos_, forward());
        if (slot < 0) return false;
        const LifeKind kind = lifeKindAt(slot);
        if (!kind.alive()) return false;
        const bool axe = held_.ready() && held_.carrying() && held_.takes() == Takes::Wood;
        const LifeHits::Blow b = lifeHits_.strike(world_, slot, kind, axe, simMs_);
        lastKilled_ = b.killed;
        if (!b.landed) return false;

        // ---- every blow: the red embers ----------------------------------
        //
        // v1 fires these above its own wound/kill split and says why: "SPARKS
        // ON EVERY BLOW -- the same embers a shaft already threw, now on any
        // hit, wounding or killing. Fired here, before the wound/kill split, so
        // hits one, two and three all show it."
        particles_.hitSparks(b.at, simMs_, /*red=*/true);
        if (!b.killed) {
            std::printf("v2: hit the %s -- %d of %d\n", kind.name, b.hits, b.needed);
            std::fflush(stdout);
            return true;
        }

        // ---- ...and the one that kills -----------------------------------
        //
        // IN v1'S ORDER, WHICH MATTERS FOR THE FIRST TWO. The corpse comes
        // apart AT THE HIT, not when the flash ends: "shattering at reap meant
        // the animal stayed whole for the entire half-second of red and only
        // burst once the red was over, so the two never shared a frame." The
        // pieces are in the air while the red is still running.
        const int pieces = world_.shatterFlyer(physics_, slot, pos_, simMs_);
        lastPieces_ = pieces;
        particles_.deathBurst(b.at, simMs_);
        if (kind.meat) dropMeatAt(b.at);
        killLifeAt(slot);
        lifeHits_.clear(slot);
        std::printf("v2: killed the %s -- %d pieces%s\n", kind.name, pieces,
                    kind.meat ? ", left a steak" : "");
        std::fflush(stdout);
        return true;
    }

    // -----------------------------------------------------------------------
    // TURN THE EARTH UNDER THE CROSSHAIR -- v1'S hoeTill, AS A SWING.
    //
    // True when a column actually turned, which is the caller's signal that the
    // blow is spent. False is ordinary: a hoe swung at rock, at a beach, at the
    // sky, or at ground it has already been over does nothing at all and says
    // so, and the swing then falls through to the wheat and to the ordinary
    // bite chain exactly as if this had not been asked.
    //
    // THE GROUND, NOT A MODEL. v1 marches its own ray here rather than reusing
    // the chop; this reuses `lastSwing_`, which has already resolved the trunks
    // and the boulders against the terrain on distance -- so a hoe swung at a
    // rock standing in a field turns nothing, which is right, instead of
    // tilling the dirt behind it.
    // -----------------------------------------------------------------------
    bool tillGround() {
        if (held_.takes() != Takes::Earth) return false;
        if (!lastSwing_.hit || lastSwing_.kind != Swing::Ground) return false;
        const size_t n = world_.till(lastSwing_.point, kTillRadiusM, simMs_ * 0.001);
        if (!n) return false;
        if (opt_.swingLog) {
            std::printf("v2: tilled %zu columns at (%.1f, %.1f, %.1f), %zu still turned\n", n,
                        lastSwing_.point.x, lastSwing_.point.y, lastSwing_.point.z,
                        world_.tilledCount());
            std::fflush(stdout);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // CUT THE WHEAT UNDER THE CROSSHAIR, AND DROP WHAT IT WAS MADE OF.
    //
    // True when a plant actually broke -- which is the caller's signal that the
    // swing is spent and the ground behind it must be left alone.
    //
    // THE BLADE RAY, NOT lastSwing_.material. A blade is not solid (see
    // bladeRay), so the ordinary swing goes straight through a field and lands
    // on the dirt -- its material is the SOIL, every time, and asking it about
    // wheat would never be true. The second march is what finds the plant, and
    // it is bounded by however far the first one got, so wheat on the far side
    // of a boulder is not cut through the boulder.
    //
    // WHEAT ONLY, not every blade. isBlade covers the green ramps too and
    // mowing the lawn is not what was asked for; isWheat is the straw, in
    // either wood.
    //
    // ONE OF EACH, AND THEY LAND APART. The two drops leave on opposite
    // bearings from the stalk -- "one a piece", and two items spilling onto the
    // same square read as one.
    // -----------------------------------------------------------------------
    bool breakWheat() {
        const Vec3 eye = player_.eyePosition(), dir = forward();
        const float reach = lastSwing_.hit ? lastSwing_.dist : swingReachM(dir);
        const BladeHit bh = bladeRay(walkWorld(), eye, dir, reach);
        if (!bh.hit || !isWheat(bh.material)) return false;

        // -- ONLY THE HOE, AND EVERYTHING ELSE KNOCKS ---------------------
        //
        // AND IT STILL SPENDS THE BLOW. Returning false here would let the
        // chain fall through to the bite, so an axe swung at a stand of wheat
        // would knock AND dig a hole in the dirt behind it -- two answers to
        // one swing, and the second one is a crater the player did not ask for.
        // The plant is what the crosshair was on; it is what the swing meets.
        //
        // ASKED AFTER THE RAY, not before it, so a tool that cannot harvest
        // still has to be AIMED at wheat to be told so. A knock on every swing
        // anywhere would be worse than silence.
        if (held_.takes() != Takes::Earth) {
            toolSfx_.knock();
            return true;
        }
        if (wheatTool_ < 0 && seedsTool_ < 0) return false;

        // -- ONE PATCH, ONE PAYOUT (user 2026-09-14: "have one patch of wheat
        //    drop one seed and one wheat. not multiple per patch.") ----------
        //
        // A TUFT IS THE PATCH, and it is the only grouping this world has --
        // see VoxelTerrain::tuftAt. Blades are a per-column hash with no
        // grouping at all, so the first cut of this mowed a fixed 0.5 m bite,
        // which is SMALLER THAN A TUFT: every swing found more of the same
        // plant standing and paid out again.
        //
        // THE FIX IS NOT A TALLY, IT IS CUTTING THE WHOLE PLANT. Take the tuft
        // at its own radius and there is nothing left to swing at -- the
        // "already cut" test in mow() then does the bookkeeping for free, and
        // it stays correct across a save, a chunk unload, or two players.
        float rad = 0.0f, want = 0.0f, sx = bh.point.x, sz = bh.point.z;
        if (!world_.terrain.tuftAt(bh.point.x, bh.point.z, &rad, &want, &sx, &sz))
            rad = kLooseWheatM;   // a stray tall blade outside any tuft
        if (!world_.mow(Vec3(sx, bh.point.y, sz), rad)) return false;

        // A BEARING OFF THE PLANT, not off the player: two swings at the same
        // tuft from two sides should not throw the drops the same way, and the
        // stalk's own position is the only thing in this that is about the
        // plant rather than about you.
        const float base = std::atan2(bh.point.x, bh.point.z);
        int n = 0;
        for (int k = 0; k < 2; ++k) {
            const int slot = k ? seedsTool_ : wheatTool_;
            if (slot < 0) continue;
            const Tool &t = held_.tool(slot);
            if (t.models.empty()) continue;
            drops_.spill(slot, t.models[0], t.sx, t.sy, t.sz,
                         Vec3(bh.point.x, bh.point.y + 0.15f, bh.point.z),
                         base + (k ? 2.2f : -2.2f));
            ++n;
        }
        if (n) {
            std::printf("v2: wheat broken at (%.1f, %.1f, %.1f) -- %d dropped\n", bh.point.x,
                        bh.point.y, bh.point.z, n);
            std::fflush(stdout);
        }
        return true;
    }

    // Is a body standing here inside a trunk? The arrival check -- gathered at
    // the SPOT rather than at the player, for the reason teleportTo carries.
    bool trunkAt(float x, float z) {
        std::vector<Solid> around;
        world_.collidersNear(Vec3(x, player_.pos.y, z), 8.0f, &around);
        WalkWorld w;
        w.terrain = &world_.terrain;
        w.edits = &world_.editStore();
        w.solids = around.data();
        w.solidCount = int(around.size());
        return player_.blocked(w, x, z);
    }

    // -----------------------------------------------------------------------
    // === LOCATE TEST === -- what /locate can find, and where it puts you.
    //
    // WHAT THIS IS FOR. /locate <animal> is nineteen rows of a table wired to
    // six populations, and a row wired to the WRONG ONE still compiles, still
    // teleports, and still prints a confident reply. The six fish are the
    // sharpest case: they share one vector and are told apart by an integer,
    // so `catfish` pointing at species 5 would quietly take you to a blue gill
    // for ever. Nothing about that is visible from a screenshot -- the two are
    // fish-shaped and both are in the water.
    //
    // SO THE SURVEY IS THE TEST. Every row is asked, once, from one spawn:
    // what it found, how far off, and -- for the fish -- whether the six
    // answers are six DIFFERENT animals, checked against the lake's own
    // census. A row that finds nothing is not a failure by itself (half the
    // table is gated to one wood) but it must find nothing for the reason the
    // table says, which is the `wood` column printed beside it.
    //
    // AND THEN ONE ARRIVAL OF EACH KIND, because the survey never moves: the
    // land case proves standNear's ordinary answer is the stand-off on dry
    // ground outside a trunk, and the water case proves the same loop walks
    // out of a lake instead of dropping the player on the bed.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === CLIP TEST === -- is any animal inside a tree or a rock?
    //
    // WHAT THIS IS FOR. "the flys were caught flying inside a big rock." The
    // fix for that is spread over five files and nine populations, and every
    // one of those populations is a loop that runs a few hundred times a
    // second in a world nobody is looking at. A rule of that shape is not
    // verified by a screenshot -- you would have to be standing next to the
    // one boulder that has a swarm in it, on the frame it is there.
    //
    // SO THE CHECK IS THE ENGINE'S OWN QUESTION ASKED BACK. solidsTouch is
    // what the animals are now steered by; this walks every live creature
    // every frame and asks it, which means a population that is exempted by
    // accident fails here rather than in a report months later. The list comes
    // from livePoints, which every population had to grow -- nine structs in
    // seven files with nothing in common but a position.
    //
    // WHAT IS ALLOWED TO BE INSIDE SOMETHING. One thing: a perched songbird,
    // which is sitting on a branch in a crown. It is counted and printed
    // rather than skipped -- see LifeAt::inTree.
    //
    // IT MUST BE RUN IN BOTH WOODS. Half the table is gated to one of them
    // (--pine pins the other), and a bee, a frog, a mouse and a snake do not
    // exist at all in the pine band.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // === HOE TEST === -- does a swing turn the earth, and does it grow back?
    //
    // WHAT THIS IS FOR. v1's hoeTill is five rules in one function and four of
    // them are refusals -- only soil, never sand, one layer only, and put it
    // all back after forty-five seconds. A refusal that does not fire looks
    // exactly like a refusal that does until you are standing on a tilled
    // beach, and the one that puts it back cannot be watched at all without
    // waiting three quarters of a minute in front of the right square metre.
    //
    // THE REVERT IS TESTED BY MOVING THE CLOCK, not by waiting: World::till
    // stamps each column with the time it was turned and tillRevert takes a
    // time, so handing it one three minutes later is the same arithmetic the
    // engine does and needs no frames at all.
    // -----------------------------------------------------------------------
    void runHoeTest() {
        std::printf("\n=== HOE TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();

        int hoe = -1;
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Earth) hoe = i;
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood, hoe in slot %d\n", pos_.x,
                    pos_.y, pos_.z, world_.terrain.woodName(pos_.x), hoe);
        if (hoe < 0) {
            std::printf("  FAIL -- the hoe did not load.\n");
            return;
        }
        held_.select(hoe);

        // -- WHETHER A WHOLE BITE IS TURNABLE, ASKED IN ONE PLACE ----------
        //
        // ONE COLUMN IS NOT THE BITE. A till is a disc of kTillRadiusM and a
        // shore is a metre of soil against a metre of sand, so a centre that
        // passes on its own can sit in a bite that is mostly refused -- and
        // the test then measures the column under the crosshair, which may be
        // one of the refused ones. That reported "surface 4 -> 4" and "DUG
        // ITSELF DEEPER" on a hoe that had done exactly the right thing.
        //
        // IT IS A LAMBDA BECAUSE TWO PLACES NEED IT and they were not the
        // same place: the site search asks it of the column it walks to, and
        // the swing asks it of the column the RAY LANDS ON, which is a metre
        // further on and was never checked at all.
        // The column the search approves, for the aim below to point at.
        Vec3 site{0.0f, 0.0f, 0.0f};
        const auto discTurnable = [&](int i, int j) {
            TerrainMemo dm;
            const int rv = int(kTillRadiusM / VOXEL_M) + 1;
            for (int dz = -rv; dz <= rv; ++dz)
                for (int dx = -rv; dx <= rv; ++dx) {
                    if (dx * dx + dz * dz > rv * rv) continue;
                    const int qi = i + dx, qj = j + dz;
                    const int qh = world_.terrain.heightVox(qi, qj, dm);
                    const uint8_t qt = world_.terrain.topMaterial(qi, qj, qh, dm);
                    if (isSand(qt) || !isSoilMat(qt)) return false;
                }
            return true;
        };

        // -- STAND ON GRASS, because half the rule is about the grass ---
        //
        // v1 lifts the strands off a column it turns and lays them back on
        // the revert; v2 gets both for free from the mesher's own rule (an
        // edited column grows nothing), which is exactly the kind of thing
        // that is true until it is not. A pine floor is bare in patches, so
        // a spawn picked at random tests it about half the time -- this
        // walks to a column that definitely has blades on it.
        {
            // -- A GRASSY SPOT, AND BESIDE WATER IF THERE IS ANY --------
            //
            // TWO PASSES, AND THE SECOND ONE IS WHY. The planting rule has
            // two halves -- a bed keeps if water is in reach and grows back
            // if it is not -- and a test that walks to the nearest grass
            // lands four metres from a lake about never. Three seeds in a
            // row all reported the dry case, which is the half that would
            // still pass on an engine that ignored water altogether.
            //
            // So: look for grass NEAR WATER first, and settle for plain
            // grass only if the wood has no shore in reach. The report says
            // which it got, and the verdict asserts whichever that was.
            TerrainMemo gm;
            bool got = false, nearWater = false;
            // -- GO TO THE SHORE FIRST, IF THE WORLD HAS ONE ------------
            //
            // A RING SEARCH FROM THE SPAWN IS THE WRONG TOOL. Sixty metres
            // of it found no water on four seeds running, because the
            // nearest lake was a hundred and seventy metres off -- and
            // widening the ring is quadratic in something that was already
            // the slowest part of this test.
            //
            // nearestWater spirals six kilometres and is what /locate uses
            // for exactly this. Move there, stream it, and let the two
            // passes below pick a grassy column off the shore.
            {
                float wx = 0.0f, wz = 0.0f;
                if (nearestWater(&wx, &wz)) {
                    for (int w = 0; w < 300; ++w) {
                        world_.update(Vec3(wx, pos_.y, wz));
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    player_.placeOnGround(walkWorld(), wx, wz);
                    pos_ = player_.eyePosition();
                    std::printf("  walked to the nearest shore (%.0f, %.0f)\n",
                                double(wx), double(wz));
                }
            }
            const int a0 = int(std::floor(pos_.x / VOXEL_M));
            const int b0 = int(std::floor(pos_.z / VOXEL_M));
            for (int pass = 0; pass < 3 && !got; ++pass)
                for (int r = 2; r < 600 && !got; ++r)
                    for (int d = -r; d <= r && !got; ++d) {
                        const int cand[4][2] = {{a0 + d, b0 - r}, {a0 + d, b0 + r},
                                                {a0 - r, b0 + d}, {a0 + r, b0 + d}};
                        for (int k = 0; k < 4 && !got; ++k) {
                            const int i = cand[k][0], j = cand[k][1];
                            const int h = world_.terrain.heightVox(i, j, gm);
                            const uint8_t t = world_.terrain.topMaterial(i, j, h, gm);
                            // -- THE TWO PASSES WANT DIFFERENT COLUMNS ---
                            //
                            // A SHORE IS SAND AND SAND GROWS NOTHING, so
                            // 'grass within four metres of water' is a
                            // column this world mostly does not have --
                            // which is why demanding both found neither.
                            //
                            // So each pass asks for the thing its half of
                            // the test needs: pass 0 wants WATER in reach
                            // (the planting rule) and does not care about
                            // blades; pass 1 wants BLADES (the strand
                            // removal) and does not care about water. The
                            // verdict then asserts whichever it got --
                            // between the two runs everything is covered,
                            // and neither run pretends to cover the other.
                            if (isSand(t)) continue;   // the hoe refuses it anyway
                            // -- THREE PASSES, THREE THINGS TO PROVE ----
                            //
                            //   0  water in reach   -- the planting rule
                            //   1  grass AND a flower standing in the bed
                            //      -- the strand removal AND the scatter
                            //   2  grass alone      -- the fallback
                            //
                            // PASS 1 EXISTS BECAUSE PASS 2 PASSED WITHOUT
                            // PROVING ANYTHING. 'scatter standing 0 -> 0'
                            // is a green tick on a disc that never had a
                            // flower in it, which is exactly the shape of
                            // the bug being tested for -- reported twice,
                            // and the second time the code was wrong in a
                            // way no test here could see.
                            if (pass >= 1 && world_.terrain.strandRows(i, j, t, gm) <= 0)
                                continue;
                            // -- AND THE WHOLE DISC HAS TO BE TURNABLE ---
                            //
                            // ONE COLUMN IS NOT THE BITE. A till is a disc
                            // of kTillRadiusM and a shore is a metre of
                            // soil against a metre of sand, so a centre
                            // that passes on its own can sit in a bite that
                            // is mostly refused -- 35 columns of 81 -- and
                            // the test then measures the column under the
                            // crosshair, which may be one of the refused
                            // ones. That reported 'surface 4 -> 4' and
                            // 'DUG ITSELF DEEPER' on a hoe that had done
                            // exactly the right thing.
                            if (!discTurnable(i, j)) continue;
                            const Vec3 c((float(i) + 0.5f) * VOXEL_M, 0.0f,
                                         (float(j) + 0.5f) * VOXEL_M);
                            // The first pass demands a shore; the second
                            // takes anything. 2 m rather than the rule's 4,
                            // so the swing lands inside it with room.
                            const bool wet = waterWithin(c, 2.0f);
                            if (pass == 0 && !wet) continue;
                            // ASKED OF THE RESIDENT WORLD, which the ring
                            // around a streamed spawn is. A column the
                            // streamer has not reached answers zero and is
                            // simply not chosen by this pass.
                            if (pass == 1 &&
                                world_.scatterShownNear(c, kTillRadiusM) <= 0)
                                continue;
                            // STREAM IT FIRST -- the walk and the mow both
                            // read resident chunks, and a shore six hundred
                            // voxels off is outside the spawn's ring.
                            for (int w = 0; w < 200; ++w) {
                                world_.update(Vec3(c.x, pos_.y, c.z));
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            // STAND BACK FROM IT AND REMEMBER IT. Standing back
                            // is what a player does; remembering it is what stops
                            // the ray landing on a shore nobody checked.
                            player_.placeOnGround(walkWorld(), c.x, c.z - 1.2f);
                            pos_ = player_.eyePosition();
                            site = c;
                            got = true;
                            nearWater = wet;
                        }
                    }
            std::printf("  moved to grass: %s%s\n", got ? "yes" : "none found",
                        (got && nearWater) ? " (beside water)" : "");
        }
        // LOOK DOWN AND FORWARD, which is how anybody tills.
        pitch_ = -55.0f;
        yaw_ = 180.0f;

        TerrainMemo memo;
        const auto matAt = [&](int i, int j, int y) {
            TerrainProbe pr(&world_.terrain, &world_.editStore());
            return pr.material(i, j, y);
        };

        // THE COLUMN THE SWING LANDS ON, not the one under your boots.
        // The first cut of this measured the player's own column and read
        // "surface 13 -> 13" while eighty-one columns a metre in front had
        // plainly turned. A hoe is swung at the ground AHEAD -- that is
        // what a ray aimed sixty degrees down means -- so the test has to
        // look where the tool lands and not where the feet are.
        // -- AND THE RAY IS AIMED AT THE BITE THAT WAS CHOSEN -------------
        //
        // IT WAS NOT LANDING ON IT. The search approves a column, the player
        // stands back from it and looks down at a FIXED ANGLE -- and where
        // that ray comes down depends on the slope and on the eye height. One
        // run landed 1.1 m from the column it had just approved, on sand, and
        // reported the hoe broken; sweeping the pitch to find a landing was
        // the same guess with more tries.
        //
        // SO IT IS POINTED AT THE COLUMN, which is what "swing at that spot"
        // means and what a player does with a mouse. Everything downstream
        // still reads lastSwing_, so the tool is exercised exactly as it is in
        // the game -- only the aiming stops being a coincidence.
        const auto aimAtSite = [&]() {
            const Vec3 t((float(int(std::floor(site.x / VOXEL_M))) + 0.5f) * VOXEL_M,
                         walkGroundM(walkWorld(), site.x, site.z) - 0.05f,
                         (float(int(std::floor(site.z / VOXEL_M))) + 0.5f) * VOXEL_M);
            Vec3 d{t.x - pos_.x, t.y - pos_.y, t.z - pos_.z};
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len > 1e-4f) { d.x /= len; d.y /= len; d.z /= len; }
            // KEPT IN SYNC so anything that reads the camera rather than the
            // swing -- the held tool, the log line -- agrees with the ray.
            yaw_ = atan2f(d.x, -d.z) * 180.0f / PI;
            pitch_ = asinf(d.y) * 180.0f / PI;
            lastSwing_ = swingRay(walkWorld(), pos_, d);
        };
        aimAtSite();
        std::printf("  swing hit %s, material %u\n",
                    lastSwing_.hit ? (lastSwing_.kind == Swing::Ground ? "ground" : "a model")
                                   : "NOTHING",
                    unsigned(lastSwing_.material));
        if (!lastSwing_.hit) {
            std::printf("  FAIL -- the swing found no ground to aim at.\n");
            return;
        }
        if (!discTurnable(int(std::floor(lastSwing_.point.x / VOXEL_M)),
                          int(std::floor(lastSwing_.point.z / VOXEL_M)))) {
            std::printf("  the ray landed on ground the hoe refuses -- "
                        "this run proved nothing\n");
            return;
        }
        const int i0 = int(std::floor(lastSwing_.point.x / VOXEL_M));
        const int j0 = int(std::floor(lastSwing_.point.z / VOXEL_M));
        const int h0 = world_.terrain.heightVox(i0, j0, memo);
        const uint8_t top0 = matAt(i0, j0, h0);
        // -- COUNTED OVER THE WHOLE DISC, NOT AT THE LANDING COLUMN ----
        //
        // Blades are a per-column hash and the pine floor is patchy, so a
        // single column is bare about as often as not -- two runs of this
        // in a row reported "0 blade rows" from a spot the player had been
        // walked to BECAUSE it had grass on it, because the swing lands a
        // column further on than the feet. The disc is what the hoe turns,
        // so the disc is what to count.
        const auto discGrass = [&]() {
            const int rv = int(kTillRadiusM / VOXEL_M) + 1;
            int n = 0;
            for (int dz = -rv; dz <= rv; ++dz)
                for (int dx = -rv; dx <= rv; ++dx)
                    if (dx * dx + dz * dz <= rv * rv) n += bladeRowsAt(i0 + dx, j0 + dz);
            return n;
        };
        const int grass0 = discGrass();
        // WHAT IS STANDING ON THIS BED. Flowers, caps and cones -- the
        // scatter that has no support of its own. Reported twice as not
        // going away, so it is counted rather than looked at.
        const int scatter0 = world_.scatterShownNear(lastSwing_.point, kTillRadiusM);
        std::printf("  it lands on (%d, %d): surface %u at y %d, %d blade voxels in the disc\n",
                    i0, j0, unsigned(top0), h0, grass0);

        const bool turned = tillGround();
        const uint8_t topA = matAt(i0, j0, h0);
        const uint8_t belowA = matAt(i0, j0, h0 - 1);
        const int grassA = discGrass();
        const int scatterA = world_.scatterShownNear(lastSwing_.point, kTillRadiusM);
        std::printf("\n  -- the swing --\n");
        std::printf("  tilled            %s, %zu columns still turned\n",
                    turned ? "yes" : "NO", world_.tilledCount());
        std::printf("  surface voxel     %u -> %u (air is %u)\n", unsigned(top0), unsigned(topA),
                    unsigned(mat::AIR));
        std::printf("  the one below     -> %u (tilled is %u)\n", unsigned(belowA),
                    unsigned(mat::TILLED));
        std::printf("  blade voxels      %d -> %d\n", grass0, grassA);
        std::printf("  scatter standing  %d -> %d\n", scatter0, scatterA);

        // ---- one layer only -------------------------------------------------
        //
        // ASKED OF THE COLUMN, NOT OF THE COUNT. A second swing legitimately
        // turns MORE columns -- the ground under the crosshair just dropped a
        // voxel, so the ray lands slightly differently and catches fresh rim
        // that the first disc missed. v1 does the same and its `tillSet` is
        // per CELL for exactly this reason. What must not happen is this
        // column going down a second voxel.
        lastSwing_ = swingRay(walkWorld(), pos_, forward());
        tillGround();
        const uint8_t topB = matAt(i0, j0, h0);
        const uint8_t belowB = matAt(i0, j0, h0 - 1);
        const bool deeper = !(topB == mat::AIR && belowB == mat::TILLED);
        std::printf("  swung again       %s (surface %u, below %u)\n",
                    deeper ? "DUG ITSELF DEEPER -- WRONG" : "one layer only, correct",
                    unsigned(topB), unsigned(belowB));

        // ---- ...AND A SEED STOPS IT GROWING BACK, IF THERE IS WATER --------
        //
        // (user 2026-09-14: "if there is no water present, the tilled land
        // goes back to dirt/grass like it does currently.")
        //
        // BOTH HALVES, ON THE SAME BED. A test that only planted where
        // there is water would pass on an engine that ignored water
        // entirely, which is the more likely mistake of the two -- there is
        // nothing to write to make a seed take, only something to write to
        // make it NOT take.
        const bool wetHere = waterWithin(lastSwing_.point, kSeedWaterM);
        const size_t bed = world_.plantAt(lastSwing_.point, kTillRadiusM, wetHere);
        // WHAT A SOWN BED LOOKS LIKE, COUNTED RATHER THAN SAMPLED. Three
        // seed voxels lie somewhere ON the bed -- not at a spot this test can
        // name, and deliberately not over the column under the crosshair -- so
        // the disc is swept for them. The band is generous in y because the
        // till lowers a column by one and the ground is not flat.
        int seedVox = 0;
        {
            const int rr = maxi(1, int(std::ceil(kTillRadiusM / VOXEL_M)));
            for (int dj = -rr; dj <= rr; ++dj)
                for (int di = -rr; di <= rr; ++di) {
                    if (di * di + dj * dj > rr * rr) continue;
                    // WIDE ENOUGH FOR A SLOPE. Each seed sits at ITS OWN
                    // column's turned voxel, and a bed of eighty-one columns on
                    // a hillside spans more than the six voxels this allowed --
                    // so a run would count two of three and report the planting
                    // broken when it was the window that was too narrow.
                    for (int y = h0 - 8; y <= h0 + 5; ++y)
                        if (isSeed(matAt(i0 + di, j0 + dj, y))) ++seedVox;
                }
        }
        // AND THE BED IS STILL A BED. This is the whole of the report -- the
        // seed used to be written OVER the tilled voxel, so planting read as
        // the till coming undone.
        const uint8_t sown = matAt(i0, j0, h0 - 1);
        std::printf("\n  -- planting --\n");
        std::printf("  water within %.0f m  %s\n", double(kSeedWaterM),
                    wetHere ? "yes" : "no");
        std::printf("  bed found         %zu columns\n", bed);
        // A SOWN BED HAS TO LOOK SOWN -- the whole of the report was that
        // planting changed nothing you could see.
        std::printf("  seed voxels       %d  %s\n", seedVox,
                    seedVox == 3 ? "three, correct" : "SHOULD BE THREE -- WRONG");
        std::printf("  the bed still     %u (tilled is %u, seed is %u)\n",
                    unsigned(sown), unsigned(mat::TILLED), unsigned(mat::SEED_0));

        // ---- and it grows back ----------------------------------------------
        std::vector<Vec3> back;
        world_.tillRevert(simMs_ * 0.001 + 300.0, &back);
        const uint8_t topR = matAt(i0, j0, h0);
        // A KEPT BED IS AIR *OR A SEED LYING IN IT*. The till empties the
        // surface voxel and a planting puts three seeds back into it, so on a
        // bed that kept, the column under the crosshair reads a seed about
        // a third of the time -- which this called "GREW BACK" on a bed that
        // had plainly done nothing of the kind.
        const bool keptTop = topR == mat::AIR || isSeed(topR);
        const int grassR = discGrass();
        const int scatterR = world_.scatterShownNear(lastSwing_.point, kTillRadiusM);
        std::printf("\n  -- five minutes later --\n");
        std::printf("  surface voxel     %u -> %u (was %u before the hoe)\n", unsigned(topA),
                    unsigned(topR), unsigned(top0));
        std::printf("  blade voxels      %d -> %d (was %d)\n", grassA, grassR,
                    grass0);
        std::printf("  scatter standing  %d -> %d (was %d)\n", scatterA, scatterR,
                    scatter0);
        std::printf("  still turned      %zu\n", world_.tilledCount());
        std::printf("  seeds handed back %zu\n", back.size());
        std::printf("  the bed %s\n",
                    wetHere ? (keptTop ? "KEPT -- planted beside water, correct"
                                                : "GREW BACK -- a planted bed must not")
                            : (topR == top0 ? "grew back -- no water, correct"
                                            : "KEPT -- it has no water, WRONG"));

        // grass0 > 0 IS PART OF THE PASS. Without it a run that happened to
        // land on bare dirt reports a green tick for a rule it never exercised,
        // which is the failure this whole file is written against.
        // THE REVERT'S EXPECTATION FLIPS ON THE WATER. A bed beside a lake is
        // SUPPOSED not to grow back once a seed is in it, so asserting the
        // restoration unconditionally would fail the very case the feature
        // exists for. Which case this spawn is came out of waterWithin, so the
        // test asserts whichever one it got rather than demanding one.
        // tilledCount() GOES TO ZERO EITHER WAY, and asserting otherwise was
        // this test contradicting its own engine: tillRevert DROPS a planted
        // record from the list without undoing it -- it has nothing left to do,
        // and leaving it would walk it again on every frame for the session.
        // So what says a planted bed kept is the GROUND, not the bookkeeping.
        const bool revertRight = wetHere ? keptTop
                                         : (topR == top0 && grassR == grass0 &&
                                            world_.tilledCount() == 0);
        // ---- ...AND THE WET CASE, FORCED --------------------------------
        //
        // THE HARNESS COULD NOT GET TO A WET ONE ON ITS OWN. A shore in
        // this world is SAND, sand grows nothing and the hoe refuses it, so
        // the nearest column a till can even touch is metres inland -- four
        // seeds and a walk to the nearest lake all came back dry. Waiting
        // for a world that happens to have turnable soil within four metres
        // of water is a test that passes by not running.
        //
        // SO THE WATER ANSWER IS SUPPLIED RATHER THAN FOUND. plantAt takes
        // `keep` as an argument precisely because the caller decides it,
        // and handing it true here exercises the half that can actually go
        // wrong: whether a planted bed survives its own revert.
        //
        // WHAT THIS DOES NOT TEST is waterWithin -- whether the engine
        // AGREES there is water. That is a ring search over wetColumnAt,
        // the same predicate the frogs and the lake are placed from, and it
        // is exercised everywhere else. The join between the two is the one
        // line plantSeed writes, and it is not covered here.
        // ...AND ON A WET RUN THE BED ABOVE ALREADY PROVED IT. The forced
        // block below cannot run there -- the ground is still turned, so a
        // second till finds nothing to turn and never gets as far as planting.
        bool keptWhenPlanted = wetHere;
        {
            aimAtSite();
            if (tillGround()) {
                const size_t n2 = world_.plantAt(lastSwing_.point, kTillRadiusM, true);
                const size_t before2 = world_.tilledCount();
                world_.tillRevert(simMs_ * 0.001 + 600.0);
                // A PLANTED RECORD IS DROPPED FROM THE LIST WITHOUT BEING
                // UNDONE, so the count going to zero is expected -- what is
                // being asked is whether the GROUND came back.
                const uint8_t topP = matAt(i0, j0, h0);
                keptWhenPlanted = n2 > 0 && (topP == mat::AIR || isSeed(topP));
                std::printf("\n  -- planted, then five more minutes --\n");
                std::printf("  bed planted       %zu columns of %zu\n", n2, before2);
                std::printf("  surface voxel     %u (air is %u, a seed is %u)\n",
                            unsigned(topP), unsigned(mat::AIR), unsigned(mat::SEED_0));
                std::printf("  the planted bed   %s\n",
                            keptWhenPlanted ? "KEPT, correct"
                                            : "GREW BACK -- a seed must stop the revert");
            }
        }

        // ---- ...AND THE FLOWERS, ON A BED THAT ACTUALLY HAS SOME --------
        //
        // (user 2026-09-14, twice: "the flowers are still not dissapering
        // when being tilled under".)
        //
        // ASKED WHERE THE FLOWERS ARE, NOT WHERE THE SWING WENT. Three
        // runs of the sequence above reported 'scatter standing 0 -> 0',
        // which is a green tick on a disc that never had a flower in it --
        // the same vacuous pass the grass count had before it was made to
        // go looking. Walking the player somewhere floral fought the
        // streamer; this does not move at all, it scans the chunks that are
        // ALREADY resident for a spot with scatter on it and tills that.
        //
        // NOT THROUGH A SWING, deliberately. The swing is covered above;
        // what is unproven here is hideScatterOn, and reaching it through a
        // ray means a second thing that can fail to find anything.
        bool flowersWent = false, flowersBack = false, flowersStayedDown = false;
        {
            Vec3 spot{0, 0, 0};
            int had = 0;
            for (int r = 1; r < 240 && !had; ++r)
                for (int d = -r; d <= r && !had; ++d) {
                    const float o[4][2] = {{float(d), float(-r)}, {float(d), float(r)},
                                           {float(-r), float(d)}, {float(r), float(d)}};
                    for (int k = 0; k < 4 && !had; ++k) {
                        const Vec3 c(pos_.x + o[k][0] * VOXEL_M, 0.0f,
                                     pos_.z + o[k][1] * VOXEL_M);
                        const int m = world_.scatterShownNear(c, kTillRadiusM);
                        if (!m) continue;
                        // ...AND ON GROUND THE HOE WILL ACTUALLY TURN, or
                        // the till does nothing and the flowers stay for a
                        // reason that is not the one being tested.
                        TerrainMemo fm;
                        const int fi = int(std::floor(c.x / VOXEL_M));
                        const int fj = int(std::floor(c.z / VOXEL_M));
                        const int fh = world_.terrain.heightVox(fi, fj, fm);
                        const uint8_t ft = world_.terrain.topMaterial(fi, fj, fh, fm);
                        if (isSand(ft) || !isSoilMat(ft)) continue;
                        spot = Vec3(c.x, (float(fh) + 0.5f) * VOXEL_M, c.z);
                        had = m;
                    }
                }
            std::printf("\n  -- the scatter --\n");
            if (!had) {
                std::printf("  no flowers, caps or cones on turnable ground in reach -- this run proved nothing\n");
            } else {
                const size_t n3 = world_.till(spot, kTillRadiusM, simMs_ * 0.001 + 1000.0);
                const int after3 = world_.scatterShownNear(spot, kTillRadiusM);
                // -- ...AND STILL DOWN AFTER THE CHUNK COMES BACK ----------
                //
                // THE COUNT ABOVE IS NOT THE TEST. It reads the masks in the
                // same breath as the till, and this bug lives entirely in what
                // happens NEXT: a till edits the ground, editing the ground
                // re-meshes the chunk, and adoptMany rebuilds decorDesc from
                // the scatter with every mask back on. Twice this test passed
                // on an engine where the flowers came back a frame later --
                // which is exactly what was being reported, and the only thing
                // the test could not see.
                //
                // SO THE MESHER IS LET ANSWER. update() takes what the workers
                // have finished and adopts it; sixty passes is far more than
                // the handful of chunks one bite touches needs.
                for (int k = 0; k < 60; ++k) world_.update(player_.pos);
                const int adopted3 = world_.scatterShownNear(spot, kTillRadiusM);
                world_.tillRevert(simMs_ * 0.001 + 2000.0);
                const int back3 = world_.scatterShownNear(spot, kTillRadiusM);
                flowersWent = n3 > 0 && after3 == 0;
                flowersStayedDown = adopted3 == 0;
                flowersBack = back3 == had;
                std::printf("  standing on the bed  %d\n", had);
                std::printf("  after the till       %d  %s\n", after3,
                            flowersWent ? "gone, correct"
                                        : "STILL STANDING OVER TURNED EARTH -- WRONG");
                std::printf("  after the re-mesh    %d  %s\n", adopted3,
                            flowersStayedDown
                                ? "still down, correct"
                                : "STOOD BACK UP ON ADOPT -- WRONG (this is the bug)");
                std::printf("  after it grew back   %d  %s\n", back3,
                            flowersBack ? "back, correct" : "DID NOT COME BACK -- WRONG");
            }
        }

        // grass0 > 0 IS ASSERTED ONLY ON THE DRY RUN, which is the one that
        // went looking for blades. The wet run stood on a shore to test the
        // planting rule and a shore has no grass on it -- insisting on both
        // would fail the run that is testing the thing it was sent to test.
        // ONE SEED BACK PER PLANTING, and only on the run whose bed actually
        // reverted -- a kept bed still has the seed in it.
        const bool seedBack = wetHere ? (back.empty()) : (back.size() == 1);
        const bool pass = turned && topA == mat::AIR && belowA == mat::TILLED && !deeper &&
                          grassA == 0 && bed > 0 && revertRight && keptWhenPlanted &&
                          seedVox == 3 && sown == mat::TILLED && seedBack &&
                          (wetHere || grass0 > 0) &&
                          // NOTHING LEFT STANDING ON TURNED EARTH, and it all
                          // comes back with the ground -- unless the bed kept,
                          // in which case it is still turned and they stay down.
                          scatterA == 0 && (wetHere ? true : scatterR == scatter0) &&
                          flowersWent && flowersStayedDown && flowersBack;
        std::printf("\n  %s\n", pass ? "PASS -- it turned the earth, refused to dig itself "
                                       "deeper, and grew back."
                                     : "FAIL -- see the lines above.");
    }

    // -----------------------------------------------------------------------
    // === WHEAT TEST === -- does a swing break a plant, and does it pay out?
    //
    // WHAT THIS IS FOR. The feature is five pieces in five files -- a second
    // ray, an edit that removes nothing solid, a mesher rule that was already
    // there, two kit slots and the drop pool -- and every one of them fails
    // QUIETLY. A ray that never meets a blade, a mow that takes no cells, a
    // probe that goes on reporting the plant it just cut, a drop spilled into a
    // slot nothing can pick up: all four look identical from outside, which is
    // a swing that does nothing.
    //
    // So this walks the whole chain with no window: find a stand of wheat, aim
    // at it, swing, and check each link by what it left behind.
    //
    // IT AIMS THE REAL CAMERA AND CALLS THE REAL HANDLER. breakWheat reads
    // forward() and eyePosition(), so pointing the player at the plant is the
    // only honest way to exercise the ray -- calling mow() directly would test
    // everything except the part most likely to be wrong.
    // -----------------------------------------------------------------------
    // -- THE PALETTE, AS SOMETHING YOU CAN OPEN IN MAGICAVOXEL -------------
    //
    // WHERE THIS IS CALLED FROM IS THE WHOLE MEASUREMENT. The table is served
    // first-come and it is built by the load order -- the terrain ramps, the
    // pines, the birches, the rocks and the decor, the held kit's reservation,
    // then the flyer band -- so a plate taken halfway through that sequence is
    // a plate of a palette that never renders. This runs at the BOTTOM of
    // onLoad, after every loader and after --stage and --level have had their
    // turn, which is the only point at which the table is the thing the game
    // actually runs on.
    //
    // WHAT IT COSTS: nothing but the boot. No window (pass --background), no
    // frame, no chunk streaming beyond what World::build already did.
    //
    // ADD --stage TO INCLUDE THE ASSET DECK and --level for the building.
    // Those two places register colours of their own and a plain run does not
    // open either, so the plain run is the WOOD's table -- which is the one
    // nearly every asset is authored against.
    void runPaletteVox() {
        std::printf("\n=== PALETTE PLATE ===\n");
        const Palette &pal = world_.palette;
        std::vector<PlateCell> terrain, model;
        // Indexed BY ID so the ramps below can be gathered by name; the band is
        // dense and small, and an id-indexed table is what the mat:: constants
        // are addresses into.
        std::vector<PlateCell> terrainCell(static_cast<size_t>(mat::TREE_BASE));
        int foliage = 0, exact = 0;
        for (int id = 1; id < pal.used(); ++id) {
            PlateCell c;
            c.id = id;
            c.foliage = pal.isFoliage(uint8_t(id));
            c.exact = pal.authoredExact(uint8_t(id));
            std::array<uint8_t, 3> src{};
            if (pal.authoredColor(uint8_t(id), &src)) {
                // A MODEL ENTRY, AND THE PLATE SHOWS WHAT THE ART CARRIED.
                // Not what it renders: a needle is lifted 1.7x on the way in,
                // so the albedo is a green no .vox file holds and nothing
                // would ever match against. See Palette::authoredColor.
                c.rgb = src;
                model.push_back(c);
                foliage += c.foliage ? 1 : 0;
                exact += c.exact ? 1 : 0;
            } else {
                // THE TERRAIN BAND. Nothing authored these -- the grass and
                // soil ramps are derived from the trees, the stone band from
                // the boulders, the wheat from the grass -- so the stored
                // albedo IS the honest colour, and it is not lifted.
                const Vec3 a = pal[uint8_t(id)].albedo;
                c.rgb = {uint8_t(Palette::srgbByte(a.x)), uint8_t(Palette::srgbByte(a.y)),
                         uint8_t(Palette::srgbByte(a.z))};
                terrainCell[size_t(id)] = c;
            }
        }
        // -- THE TERRAIN BAND MOVES AS RAMPS, NOT AS CELLS ------------------
        //
        // Every one of these ranges is a GRADIENT and its id order is the
        // gradient's order -- six greens sampled across the pines' foliage,
        // four shades of one soil, the six stones setStoneBand spreads by
        // luminance, ten wheat shades tan to brown. Sorting the cells by colour
        // would put six greens in six different places and there would be no
        // ramp left to read.
        //
        // So the RAMP is the unit: each block keeps its own order, and
        // orderBlocksByHue decides where the block goes. Declared off the mat::
        // constants rather than as literals so the layout cannot drift from the
        // table it describes; anything in the band that no range claims becomes
        // a block of one and finds its own hue neighbours (mat::MOSS ends up
        // beside the grass, mat::DIRT beside the soils).
        const struct {
            uint8_t base, count;
        } kRamps[] = {
            {mat::GRASS_0, mat::GRASS_COUNT},   {mat::BGRASS_0, mat::BGRASS_COUNT},
            {mat::SOIL_0, mat::SOIL_COUNT},     {mat::LITTER_0, mat::LITTER_COUNT},
            {mat::STONE_0, mat::STONE_COUNT},   {mat::SAND_0, mat::SAND_COUNT},
            {mat::WHEAT_0, mat::WHEAT_COUNT},   {mat::BWHEAT_0, mat::BWHEAT_COUNT},
            {mat::SEED_0, mat::SEED_COUNT},
        };
        std::vector<std::vector<PlateCell>> blocks;
        std::vector<uint8_t> owner(size_t(mat::TREE_BASE), 0u);
        for (const auto &r : kRamps) {
            std::vector<PlateCell> block;
            for (int k = 0; k < int(r.count); ++k) {
                const int id = int(r.base) + k;
                if (id < 1 || id >= int(terrainCell.size())) continue;
                owner[size_t(id)] = 1u;
                if (terrainCell[size_t(id)].id) block.push_back(terrainCell[size_t(id)]);
            }
            if (!block.empty()) blocks.push_back(block);
        }
        for (int id = 1; id < int(mat::TREE_BASE); ++id)
            if (!owner[size_t(id)] && terrainCell[size_t(id)].id)
                blocks.push_back({terrainCell[size_t(id)]});
        terrain = orderBlocksByHue(blocks);
        std::printf("  %d terrain entries (ids 1..%d -- ramps, and NOT authorable: "
                    "nearestModelColor starts at %d)\n",
                    int(terrain.size()), int(mat::TREE_BASE) - 1, int(mat::TREE_BASE));
        std::printf("  %d model entries -- %d classified FOLIAGE (lifted, translucent), "
                    "%d minted EXACT (the held kit)\n",
                    int(model.size()), foliage, exact);
        if (!writePalettePlate(opt_.paletteVoxOut, terrain, model, pal.used(),
                               pal.overflowedColors()))
            std::fprintf(stderr, "v2: --palette-vox: could not write %s\n",
                         opt_.paletteVoxOut.c_str());
    }

    void runWheatTest() {
        std::printf("\n=== WHEAT TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood\n", pos_.x, pos_.y, pos_.z,
                    world_.terrain.woodName(pos_.x));
        std::printf("  kit: wheat slot %d, seeds slot %d\n", wheatTool_, seedsTool_);
        if (wheatTool_ < 0 || seedsTool_ < 0) {
            std::printf("  FAIL -- the drop models did not load.\n");
            return;
        }

        // ---- find a stand of wheat within arm's reach ----------------------
        //
        // WHEAT IS NOT EVERYWHERE. It is the tall band of the blade ramp, so
        // most columns have none -- this walks outward from the player and
        // takes the first one that has some, then stands next to it.
        // A PERIMETER WALK, NOT A FILLED SQUARE. The first cut of this scanned
        // every cell of every ring and was therefore cubic in the radius, which
        // is why it only reached nine metres -- and nine metres of pine wood
        // very often has no wheat in it at all. Walking the ring itself is
        // linear per ring, so forty metres costs less than nine did.
        TerrainMemo memo;
        int wi = 0, wj = 0, wy = 0;
        bool found = false;
        const int p0i = int(std::floor(pos_.x / VOXEL_M));
        const int p0j = int(std::floor(pos_.z / VOXEL_M));
        const auto tryCell = [&](int i, int j) {
            if (found) return;
            const int h = world_.terrain.heightVox(i, j, memo);
            const uint8_t top = world_.terrain.topMaterial(i, j, h, memo);
            const int sr = world_.terrain.strandRows(i, j, top);
            if (sr <= 0) return;
            if (!isWheat(world_.terrain.bladeMaterial(i, j, sr))) return;
            int lo = 0, hi = 0;
            VoxelTerrain::bladeSpan(h, sr, &lo, &hi);
            wi = i;
            wj = j;
            wy = (lo + hi) / 2;
            found = true;
        };
        for (int r = 1; r < 400 && !found; ++r) {
            for (int d = -r; d <= r && !found; ++d) {
                tryCell(p0i + d, p0j - r);
                tryCell(p0i + d, p0j + r);
                tryCell(p0i - r, p0j + d);
                tryCell(p0i + r, p0j + d);
            }
        }
        if (!found) {
            std::printf("  NO WHEAT WITHIN 40 m -- this run proved nothing. Try another "
                        "--spawn.\n");
            return;
        }
        // STREAM THE GROUND UNDER IT before standing there: the walk below and
        // the mow both read the edit layer through resident chunks, and a stand
        // of wheat forty metres off may be outside the ring the spawn built.
        for (int i = 0; i < 200; ++i) {
            world_.update(Vec3((float(wi) + 0.5f) * VOXEL_M, pos_.y,
                               (float(wj) + 0.5f) * VOXEL_M));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const Vec3 stalk((float(wi) + 0.5f) * VOXEL_M, (float(wy) + 0.5f) * VOXEL_M,
                         (float(wj) + 0.5f) * VOXEL_M);
        std::printf("  wheat at (%.1f, %.1f, %.1f), %.1f m off\n", stalk.x, stalk.y, stalk.z,
                    std::hypot(stalk.x - pos_.x, stalk.z - pos_.z));

        // ---- stand next to it and look at it -------------------------------
        player_.placeOnGround(walkWorld(), stalk.x + 0.9f, stalk.z + 0.9f);
        pos_ = player_.eyePosition();
        // DEGREES, AND -Z IS FORWARD. Camera::direction takes degrees and
        // builds (cos p sin y, sin p, -cos p cos y) -- so a yaw taken as
        // atan2(x, z) points the wrong way round the compass and a pitch in
        // radians is a rounding error away from level. Either one aims the
        // swing at the sky, which reads as "breakWheat found nothing" with
        // the wheat plainly standing in front of it.
        const Vec3 to = stalk - pos_;
        yaw_ = std::atan2(to.x, -to.z) * 57.29578f;
        pitch_ = std::atan2(to.y, std::hypot(to.x, to.z)) * 57.29578f;
        const Vec3 aim = forward();
        std::printf("  standing at (%.1f, %.1f, %.1f), aim (%.2f, %.2f, %.2f)\n", pos_.x,
                    pos_.y, pos_.z, aim.x, aim.y, aim.z);
        {
            const BladeHit pr = bladeRay(walkWorld(), pos_, aim, swingReachM(aim));
            std::printf("  blade ray         %s %s at %.2f m\n",
                        pr.hit ? "hit" : "MISSED",
                        pr.hit ? (isWheat(pr.material) ? "wheat" : "green grass") : "",
                        double(pr.dist));
        }

        const int before = bladeRowsAt(wi, wj);
        const int drops0 = drops_.count();

        // ---- FIRST, THE WRONG TOOL ------------------------------------------
        //
        // (user 2026-09-14: "make it where only the hoe can break the wheat.
        // if any other tool does it, play the antibreak sound.")
        //
        // TESTED BEFORE THE HOE IS EVEN PICKED UP, because the failure this
        // guards against is not that the axe does nothing -- it is that the
        // axe HARVESTS, which is what it did until now and which a test that
        // only swings the right tool would never see.
        //
        // THE SOUND IS NOT ASSERTED and cannot usefully be: there is no audio
        // device on this path. What is asserted is that the blow was SPENT --
        // breakWheat returning true is what stops the chain digging a crater in
        // the dirt behind the plant -- and that nothing was cut and nothing paid.
        int axeSlot = -1;
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Wood) axeSlot = i;
        bool axeSpent = false, axeQuiet = true;
        if (axeSlot >= 0) {
            held_.select(axeSlot);
            axeSpent = breakWheat();
            axeQuiet = (bladeRowsAt(wi, wj) == before) && (drops_.count() == drops0);
            std::printf("\n  -- the axe --\n");
            std::printf("  blow spent        %s\n",
                        axeSpent ? "yes -- it knocks and stops there"
                                 : "NO -- it would fall through and dig");
            std::printf("  took nothing      %s\n",
                        axeQuiet ? "correct" : "IT HARVESTED -- WRONG");
        }

        // ---- ...AND NOW THE HOE ---------------------------------------------
        int hoeSlot = -1;
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).takes == Takes::Earth) hoeSlot = i;
        std::printf("  hoe in slot %d\n", hoeSlot);
        if (hoeSlot < 0) {
            std::printf("  FAIL -- no hoe in the kit, so nothing can harvest.\n");
            return;
        }
        held_.select(hoeSlot);

        // ---- the swing ------------------------------------------------------
        const bool broke = breakWheat();
        const int after = bladeRowsAt(wi, wj);
        const int drops1 = drops_.count();
        std::printf("\n  -- the swing --\n");
        std::printf("  breakWheat        %s\n", broke ? "yes" : "NO -- it found nothing");
        std::printf("  blade rows here   %d -> %d\n", before, after);
        std::printf("  drops in flight   %d -> %d\n", drops0, drops1);

        // ---- and a second swing must pay NOTHING ---------------------------
        //
        // The half of this that is easy to get wrong: the mesher stops drawing
        // a cut column but the PROBE has its own copy of the rule, and if the
        // two disagree the plant is gone from the screen and still there to
        // swing at. That is a field that pays out for ever.
        const bool again = breakWheat();
        std::printf("  swung again       %s\n",
                    again ? "PAID OUT TWICE -- WRONG" : "nothing left, correct");

        // ---- ...AND NOWHERE ELSE IN THE SAME PATCH EITHER ------------
        //
        // THIS IS THE REPORT, and re-swinging at the same column does not
        // test it: "have one patch of wheat drop one seed and one wheat.
        // not multiple per patch." A tuft is a metre or so across and the
        // old bite was half that, so the way to farm one plant for ever was
        // to aim a step to the left. So this walks the tuft's own radius
        // and swings at every remaining stalk it can find in it.
        float trad = 0.0f, twant = 0.0f, tsx = stalk.x, tsz = stalk.z;
        const bool inTuft =
            world_.terrain.tuftAt(stalk.x, stalk.z, &trad, &twant, &tsx, &tsz);
        int extra = 0, stalksLeft = 0;
        if (inTuft) {
            const int rv = int(trad / VOXEL_M) + 1;
            const int c0 = int(std::floor(tsx / VOXEL_M)), d0 = int(std::floor(tsz / VOXEL_M));
            for (int dz = -rv; dz <= rv; ++dz)
                for (int dx = -rv; dx <= rv; ++dx) {
                    if (dx * dx + dz * dz > rv * rv) continue;
                    if (bladeRowsAt(c0 + dx, d0 + dz) <= 0) continue;
                    ++stalksLeft;
                    player_.placeOnGround(walkWorld(), (float(c0 + dx) + 0.5f) * VOXEL_M + 0.9f,
                                          (float(d0 + dz) + 0.5f) * VOXEL_M + 0.9f);
                    pos_ = player_.eyePosition();
                    const Vec3 t2(((float(c0 + dx) + 0.5f) * VOXEL_M) - pos_.x, 0.0f,
                                  ((float(d0 + dz) + 0.5f) * VOXEL_M) - pos_.z);
                    yaw_ = std::atan2(t2.x, -t2.z) * 57.29578f;
                    pitch_ = -35.0f;
                    held_.select(hoeSlot);   // the only thing that can pay out
                    if (breakWheat()) ++extra;
                }
        }
        std::printf("  tuft %.1f m across: %d stalks still standing in it, %d more payouts\n",
                    double(trad * 2.0f), stalksLeft, extra);
        // A STALK LEFT IS A FAILURE EVEN IF IT PAYS NOTHING. It was reported as
        // "one strand of wheat was left" -- the player sees the plant, not the
        // ledger, and a patch that is cut except for one stem reads as a bug
        // whether or not swinging at it again would hand out a second seed.
        if (stalksLeft) std::printf("      ^ A CUT PATCH MUST BE EMPTY -- WRONG\n");

        // ---- walk over them -------------------------------------------------
        std::printf("\n  -- absorbing --\n");
        int got = 0;
        float firstAt = -1.0f, lastAt = -1.0f;
        for (int f = 0; f < 60 * 12 && got < drops1 - drops0; ++f) {
            drops_.update(1.0f / 60.0f, walkWorld(), player_.pos, player_.eyePosition());
            // EVERY ARRIVAL ON THE FRAME, not the first -- see
            // Drops::arrivedThisTick. The old loop took one per tick, which
            // was a complete answer while only one item could be in flight
            // and is a silent leak now that a pile converges together.
            for (const int back : drops_.arrivedThisTick()) {
                const float at = float(f) / 60.0f;
                if (firstAt < 0.0f) firstAt = at;
                lastAt = at;
                std::printf("  picked up %s (slot %d) after %.2f s\n",
                            held_.tool(back).name, back, double(at));
                held_.give(back);
                ++got;
            }
        }
        // -- AND HOW FAR APART THEY LANDED, which is the whole of the ask
        //    (user 2026-09-14: "absorb multiple object at once, instead of
        //    one at a time in a line") -------------------------------------
        //
        // ONE FLIGHT IS kGrabSec, so two items absorbed one after the other
        // land a whole flight apart and two absorbed together land on the
        // same frame or within a step of it. The gap is the measurement; the
        // count never changed and never would have.
        std::printf("  first at %.2f s, last at %.2f s -- %.2f s apart\n",
                    double(firstAt), double(lastAt), double(lastAt - firstAt));
        std::printf("  carrying wheat    %s\n", held_.tool(wheatTool_).carried ? "yes" : "NO");
        std::printf("  carrying seeds    %s\n", held_.tool(seedsTool_).carried ? "yes" : "NO");

        // TOGETHER MEANS TOGETHER: the two drops leave the plant on the same
        // frame and are the same distance from the player, so if they are
        // absorbed concurrently they arrive within a few frames of each other.
        // A whole kGrabSec apart is the old one-at-a-time behaviour wearing a
        // passing count.
        const bool together = (lastAt - firstAt) < 0.5f * kGrabSec;
        const bool pass = broke && after == 0 && before > 0 && (drops1 - drops0) == 2 &&
                          !again && extra == 0 && stalksLeft == 0 && axeSpent && axeQuiet &&
                          together && got == 2 &&
                          held_.tool(wheatTool_).carried && held_.tool(seedsTool_).carried;
        std::printf("\n  %s\n", pass ? "PASS -- the wheat broke, paid one of each, and both "
                                        "were absorbed."
                                      : "FAIL -- see the lines above.");
    }

    // How many blade voxels are still standing on this column, as the PROBE
    // sees it -- which is the same answer the mesher draws. See mow().
    int bladeRowsAt(int i, int j) {
        TerrainProbe probe(&world_.terrain, &world_.editStore());
        TerrainMemo memo;
        const int h = world_.terrain.heightVox(i, j, memo);
        const uint8_t top = world_.terrain.topMaterial(i, j, h, memo);
        const int sr = world_.terrain.strandRows(i, j, top);
        if (sr <= 0) return 0;
        int lo = 0, hi = 0;
        VoxelTerrain::bladeSpan(h, sr, &lo, &hi);
        int n = 0;
        for (int y = lo; y <= hi; ++y) n += isBlade(probe.material(i, j, y)) ? 1 : 0;
        return n;
    }

    void runClipTest() {
        std::printf("\n=== CLIP TEST ===\n");
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        const bool birch = world_.terrain.birchAt(pos_.x);
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood, %04.1fh, sun %.1f deg (%s)\n",
                    pos_.x, pos_.y, pos_.z, birch ? "birch" : "pine",
                    double(opt_.timeOfDay * 24.0f), double(world_.sky.elevationDeg()),
                    isNight() ? "night" : "day");

        // THE WOOD AROUND THE SPAWN, gathered once and wide. This is the same
        // list the animals themselves are given (see the perch gather in
        // update) -- if the check gathered its own, the two could disagree and
        // a pass would mean nothing.
        world_.collidersNear(pos_, kBirdKeepM, &perches_);
        int rocks = 0;
        for (const Solid &sd : perches_) rocks += sd.standable ? 1 : 0;
        std::printf("  %d solids in reach -- %d standable (rocks), %d not (trunks)\n",
                    int(perches_.size()), rocks, int(perches_.size()) - rocks);

        // ---- settle, then watch --------------------------------------------
        //
        // A POPULATION REPORTS ITS SPAWN STATE IF YOU ONLY TICK IT ONCE, which
        // is the lesson the bunny report and the bee report each had to learn
        // separately. Thirty seconds to fill and settle, then sixty watched --
        // long enough for a fly bunch to cross a clearing at 1 m/s and for
        // every bee to run two full errands.
        warmLife(pos_, 30);

        // -- WHAT THE THREE NUMBERS MEAN, because "inside" alone said too
        //    little to act on.
        //
        //    touch   the body's rim met a model -- a wing clipping a corner
        //    centre  the animal's own position is in the solid, which is the
        //            report ("caught flying INSIDE a big rock") and is the one
        //            that matters
        //    deep    how far it would have to rise to get out, walked upward
        //            in tenths -- a centimetre is a surface, a metre is a
        //            creature living in the stone
        //
        // The first cut of this printed a fourth number that was nonsense: the
        // radius at which solidsTouch stopped firing, walked outward to six
        // metres. That probes the RIM of a circle, so it reports whether there
        // is a model within six metres of the animal, which there usually is in
        // a wood. It read 6.00 m for every single hit, which is what a metric
        // that is measuring nothing looks like.
        struct Worst {
            int touch = 0, centre = 0, wet = 0;
            float deep = 0.0f;
            Vec3 at{0, 0, 0};
            bool rock = false;
        };
        std::map<std::string, Worst> bad;
        std::map<std::string, long> seen;
        // -- HOW FAR EACH SPECIES ACTUALLY WENT -----------------------------
        //
        // THE OTHER HALF OF THE ANSWER, and without it a pass means nothing.
        // Every fix in this change is a REFUSAL -- do not take that step, do
        // not hold that line -- and the way a refusal fails is by refusing
        // everything. A wood where no animal is inside a rock because no
        // animal has moved since it was born passes the check above perfectly.
        //
        // Matched by INDEX and only while the count is steady, which is as much
        // identity as livePoints has: a slot that dies renumbers the rest. Any
        // jump over two metres in a frame is a birth or a recycle rather than a
        // step, and is dropped.
        std::map<std::string, double> went;
        std::map<std::string, long> wentN;
        // -- AND HOW HIGH IT FLIES, which is the other thing a report of
        //    "the flys are too low" or "the ducks sit too deep" needs a
        //    number for. Height above the local ground, averaged.
        std::map<std::string, double> agl;
        std::vector<LifeAt> prev;
        std::vector<LifeAt> who;
        long samples = 0, inTree = 0;

        const auto groundAt = [this](float x, float z) {
            return float(world_.terrain.heightVox(int(std::floor(x / VOXEL_M)),
                                                  int(std::floor(z / VOXEL_M))) + 1) * VOXEL_M;
        };
        // -- THE BROAD PHASE, WHICH THE ANIMALS DO NOT NEED AND THIS DOES --
        //
        // A creature is handed a list gathered at 2.8 m and this one is
        // gathered at 115, because the check has to see every animal at once
        // and they are spread over the whole streaming ring. Walking two
        // thousand models per creature per frame through the voxel test is a
        // minute of arithmetic to answer a question about a metre of space.
        //
        // REJECTED ON THE MODEL'S OWN EXTENT, not on the collider's. The
        // ellipse is measured over the first two metres of a model and a domed
        // boulder is WIDER higher up (see measureCollider), so a reject built
        // from hx/hz would quietly stop testing the overhangs -- which are
        // exactly the shapes this is here to catch.
        const auto insideAny = [this](const Vec3 &p, float r, const Solid **which) {
            for (const Solid &sd : perches_) {
                if (p.y > sd.top) continue;
                float reach = (sd.hx > sd.hz ? sd.hx : sd.hz);
                if (sd.msx > 0 && sd.msz > 0) {
                    const float ex = float(sd.msx) * VOXEL_M, ez = float(sd.msz) * VOXEL_M;
                    const float diag = std::sqrt(ex * ex + ez * ez) * 0.5f;
                    if (diag > reach) reach = diag;
                }
                reach += r + 0.5f;
                const float dx = p.x - sd.cx, dz = p.z - sd.cz;
                if (dx * dx + dz * dz > reach * reach) continue;
                if (solidsTouch(&sd, 1, p.x, p.y, p.z, r, VOXEL_M)) {
                    if (which) *which = &sd;
                    return true;
                }
            }
            return false;
        };

        const float dt = 1.0f / 60.0f;
        for (int frame = 0; frame < 30 * 60; ++frame) {
            birds_.update(dt, perches_, pos_);
            lake_.update(dt, world_.terrain, pos_);
            flock2_.update(dt, pos_, groundAt, &perches_);
            bunnies_.update(dt, pos_, groundAt,
                            [this](float x, float z) { return wetColumnAt(x, z); }, perches_,
                            [this](float x) { return world_.terrain.birchMix(x); },
                            // ...AND NOT ON THE BEACH -- see Bunnies::blocked.
                            [this](float x, float z) { return sandAt(x, z); });
            bees_.update(dt, pos_, hivesNear_, bloomsNear_, &perches_);
            flock_.update(dt, world_, pos_);
            lake_.bankSpots(uint32_t(frame), 8, &banksNear_);
            critters_.update(dt, pos_, groundAt,
                             [this](float x, float z) { return wetColumnAt(x, z); },
                             [this](float x) { return world_.terrain.birchMix(x); }, banksNear_,
                             perches_, isNight(), Vec3(0.0f, 0.0f, 0.0f),
                             [this](float x, float z) { return waterTopAt(x, z); });

            who.clear();
            critters_.livePoints(&who);
            bees_.livePoints(&who);
            flock_.livePoints(&who);
            bunnies_.livePoints(&who);
            flock2_.livePoints(&who);
            birds_.livePoints(&who);
            lake_.livePoints(&who);

            if (prev.size() == who.size())
                for (size_t q = 0; q < who.size(); ++q) {
                    if (prev[q].what != who[q].what) continue;
                    const float dx = who[q].p.x - prev[q].p.x, dz = who[q].p.z - prev[q].p.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 > 4.0f) continue;
                    went[who[q].what] += double(std::sqrt(d2));
                    ++wentN[who[q].what];
                }
            prev = who;

            for (const LifeAt &a : who) {
                ++samples;
                ++seen[a.what];
                agl[a.what] += double(a.p.y - groundAt(a.p.x, a.p.z));
                // -- IS IT UNDER THE LAKE ---------------------------------
                //
                // The second invariant, and it exists because of a report the
                // first one could never have caught: "a lady bug was caught
                // swimming in water". Nothing was inside a solid -- a lake is
                // not a solid -- and the animal was a foot under the surface
                // holding exactly the altitude it had been told to hold.
                if (!a.inWater) {
                    const float top = waterTopAt(a.p.x, a.p.z);
                    if (a.p.y < top) ++bad[a.what].wet;
                }
                if (a.inTree) { ++inTree; continue; }
                const Solid *hit = nullptr;
                if (!insideAny(a.p, a.r, &hit)) continue;
                Worst &w = bad[a.what];
                ++w.touch;
                // -- THE FIRST ONE OF EACH KIND, IN FULL ---------------------
                //
                // A count says a rule is broken and says nothing about which
                // rule. One line naming the model, its footprint, its base and
                // its top against the creature's own position is the whole
                // diagnosis, and the first offence is as good as any: these
                // failures come in runs of one animal over many frames, so the
                // hundredth is the same animal as the first.
                if (w.touch == 1 && hit) {
                    std::printf("    first %-10s at (%.1f, %.1f, %.1f) r %.2f -- %s "
                                "centre (%.1f, %.1f) h (%.1f, %.1f) base %.1f top %.1f "
                                "model %dx%d vol %s\n",
                                a.what, a.p.x, a.p.y, a.p.z, double(a.r),
                                hit->standable ? "ROCK" : "tree", hit->cx, hit->cz, hit->hx,
                                hit->hz, hit->baseY, hit->top, int(hit->msx), int(hit->msz),
                                hit->vol ? "yes" : "NO");
                }
                if (!insideAny(a.p, 0.0f, nullptr)) continue;   // a graze, not a burial
                ++w.centre;
                // HOW FAR IN, which is the number that says whether this is a
                // wing clipping a corner or a swarm living in a boulder. Walked
                // outward rather than solved: the shape is a voxel grid and has
                // no inside-distance to ask for.
                // STRAIGHT UP UNTIL IT IS OUT, which is a distance the shape
                // can actually answer: every model in this world has air over
                // it eventually, and the climb crosses exactly the stone that
                // is on top of the animal.
                float d = 0.0f;
                for (float up = 0.1f; up <= 8.0f; up += 0.1f) {
                    if (!insideAny(Vec3(a.p.x, a.p.y + up, a.p.z), 0.0f, nullptr)) break;
                    d = up;
                }
                if (d >= w.deep) {
                    w.deep = d;
                    w.at = a.p;
                    w.rock = hit && hit->standable;
                }
            }
        }

        // ---- the report ----------------------------------------------------
        std::printf("\n  -- 30 s watched, %ld creature-frames --\n", samples);
        std::printf("  %-11s %9s %6s %6s %6s %7s %7s   %s\n", "species", "frames", "touch",
                    "centre", "wet", "m/s", "agl", "deepest");
        long hitTotal = 0;
        for (const auto &kv : seen) {
            const auto it = bad.find(kv.first);
            const long wn = wentN.count(kv.first) ? wentN[kv.first] : 0;
            const double mps = wn ? went[kv.first] / (double(wn) * double(dt)) : 0.0;
            const double up = kv.second ? agl[kv.first] / double(kv.second) : 0.0;
            if (it == bad.end()) {
                std::printf("  %-11s %9ld %6d %6d %6d %7.2f %7.2f   -\n", kv.first.c_str(),
                            kv.second, 0, 0, 0, mps, up);
                continue;
            }
            hitTotal += it->second.centre + it->second.wet;
            std::printf("  %-11s %9ld %6d %6d %6d %7.2f %7.2f   %.1f m under a %s at "
                        "(%.0f, %.0f, %.0f)\n",
                        kv.first.c_str(), kv.second, it->second.touch, it->second.centre,
                        it->second.wet, mps, up, double(it->second.deep),
                        it->second.rock ? "rock" : "tree", it->second.at.x, it->second.at.y,
                        it->second.at.z);
        }
        std::printf("\n  %ld perched-songbird frames in a crown (expected, not counted)\n",
                    inTree);
        if (perches_.empty())
            std::printf("  NO SOLIDS IN REACH -- this run proved nothing. Move the spawn.\n");
        else if (hitTotal == 0)
            std::printf("  PASS -- nothing was ever inside a tree or a rock, or under the "
                        "lake.\n");
        else
            std::printf("  FAIL -- %ld creature-frames with the animal ITSELF in a solid or "
                        "under water.\n", hitTotal);
    }

    void runLocateTest() {
        std::printf("\n=== LOCATE TEST ===\n");
        // The spawn first -- runFellTest's own note: this runs before the
        // player has been placed, so streaming round pos would stream the
        // origin and survey a wood nobody is standing in.
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        pos_ = player_.eyePosition();
        const bool birch = world_.terrain.birchAt(pos_.x);
        std::printf("  spawn (%.0f, %.0f, %.0f) -- the %s wood\n", pos_.x, pos_.y, pos_.z,
                    birch ? "birch" : "pine");

        warmLife(pos_, 30);

        // ---- the survey ---------------------------------------------------
        std::printf("\n  -- what /locate finds from here --\n");
        std::printf("  %-11s %-6s %8s   %s\n", "name", "lives", "distance", "at");
        int found = 0, expectedMisses = 0, wrong = 0;
        for (const LifeName &ln : lifeNames()) {
            Vec3 at{0, 0, 0};
            const char *lives = ln.water ? "water" : ln.wood < 0 ? "both" : ln.wood ? "birch"
                                                                                    : "pine";
            if (nearestLife(ln.life, &at)) {
                const float d = std::hypot(at.x - pos_.x, at.z - pos_.z);
                std::printf("  %-11s %-6s %6.0f m   (%.0f, %.0f, %.0f)\n", ln.name, lives, d,
                            at.x, at.y, at.z);
                ++found;
                // A row gated to the OTHER wood that finds one anyway means
                // the gate in the table disagrees with the gate in the engine.
                if (!ln.water && ln.wood >= 0 && (ln.wood == 1) != birch) {
                    std::printf("      ^ TABLE WRONG: found in the %s wood\n",
                                birch ? "birch" : "pine");
                    ++wrong;
                }
                continue;
            }
            // Nothing found. Expected when the row is gated elsewhere -- by
            // wood, by water, or by the CLOCK.
            const bool asleep = ln.night && !isNight();
            const bool elsewhere =
                ln.water ? true : (ln.wood >= 0 && (ln.wood == 1) != birch);
            std::printf("  %-11s %-6s %8s   %s\n", ln.name, lives, "-",
                        asleep      ? "(after dark only -- try --time 23)"
                        : elsewhere ? "(not this wood -- /locate will travel)"
                                    : "NONE, AND IT SHOULD BE HERE");
            if (asleep || elsewhere) ++expectedMisses;
            else ++wrong;
        }
        std::printf("  %d found, %d away in another wood, %d WRONG\n", found, expectedMisses,
                    wrong);

        // ---- the six fish are six different fish ---------------------------
        //
        // The census counts by the species field; the table reaches them by
        // the same integer. If a row has the wrong one, two names return the
        // same animal and a name with fish in the lake returns nothing.
        {
            int byFish[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int pads = 0, flies = 0;
            lake_.census(byFish, &pads, &flies);
            std::printf("\n  -- the lake's own census, against the table --\n");
            static const char *kFish[6] = {"salmon", "bass", "koi", "minnow", "catfish",
                                           "bluegill"};
            for (int s = 0; s < 6; ++s) {
                Vec3 at{0, 0, 0};
                float d = 0.0f;
                const bool got = lake_.nearestFish(pos_, s, &at, &d);
                std::printf("  species %d  %-9s %2d alive   /locate %-9s %s\n", s, kFish[s],
                            byFish[s], kFish[s],
                            got ? "finds one" : (byFish[s] ? "FINDS NOTHING -- WRONG" : "none"));
                if (!got && byFish[s]) ++wrong;
            }
            std::printf("  %d lily pads, %d dragonflies, %d ducks\n", pads, flies,
                        lake_.ducksLiving(true));
        }

        // ---- the THREE WOODS, which are places rather than animals --------
        //
        // (user 2026-09-16: "give me a /locate oak".)
        //
        // THIS HALF EXISTED AND WAS NEVER CHECKED, and it had just broken.
        // nearestBandX had the period written out as `2.0f * kBandW` -- right
        // for two woods, silently wrong for three -- so /locate pine would have
        // walked to a multiple of 1600 m while the pine band repeats every
        // 2400. You would have arrived among the wrong trees with a confident
        // reply saying otherwise, which is the same failure the animal half of
        // this test exists to catch.
        //
        // ASKED OF THE WORLD IT LANDS IN, not of the number it returns: teleport,
        // then ask the terrain which wood is actually underfoot.
        std::printf("\n  -- the woods --\n");
        {
            const Vec3 was = pos_;
            for (const BiomeName &bn : biomeNames()) {
                const std::string reply = runCommand(std::string("/locate ") + bn.name);
                const char *landed = world_.terrain.woodName(pos_.x);
                const bool right = std::string(landed) == bn.name;
                std::printf("  /locate %-6s -> %8.0f  lands in the %-5s wood  %s\n", bn.name,
                            double(pos_.x), landed,
                            right ? "correct" : "THE WRONG WOOD -- WRONG");
                if (!right) ++wrong;
            }
            // ...and its ALIAS reaches the same row, which is the other way a
            // table like this rots: a name that answers and an alias that does
            // not look identical until someone types the alias.
            for (const BiomeName &bn : biomeNames()) {
                const std::string reply = runCommand(std::string("/locate ") + bn.alias);
                const bool known = reply.find("nothing called") == std::string::npos;
                if (!known) {
                    std::printf("  alias '%s' is not recognised -- WRONG\n", bn.alias);
                    ++wrong;
                }
            }
            teleportTo(was.x, was.z);
        }

        // ---- and where it puts you -----------------------------------------
        std::printf("\n  -- the arrival --\n");
        for (int pass = 0; pass < 2; ++pass) {
            // Pass 0 takes the first LAND row that is here, pass 1 the first
            // WATER row: the two halves of standNear, and the water half is
            // the one that has a lake bed to avoid.
            const LifeName *pick = nullptr;
            Vec3 at{0, 0, 0};
            for (const LifeName &ln : lifeNames()) {
                if (ln.water != (pass == 1)) continue;
                if (!nearestLife(ln.life, &at)) continue;
                pick = &ln;
                break;
            }
            if (!pick) {
                std::printf("  no %s life in range to try -- /locate would travel\n",
                            pass ? "water" : "land");
                continue;
            }
            const Vec3 target = at;
            const std::string reply = runCommand(std::string("/locate ") + pick->name);
            // THE GROUND YOU LANDED ON HAS TO BE STREAMED BEFORE IT CAN BE
            // ASKED ABOUT. collidersNear only sees RESIDENT chunks, so a trunk
            // check run on the frame of the teleport reads an empty list and
            // reports "no trunk" whatever is standing there -- a check that
            // always passes, which is worse than no check. The terrain and
            // water questions are generated rather than resident and do not
            // care, but they may as well wait with it.
            for (int i = 0; i < 200; ++i) {
                world_.update(player_.pos);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const float off = std::hypot(target.x - pos_.x, target.z - pos_.z);
            const bool wet = wetColumnAt(pos_.x, pos_.z);
            const bool tree = trunkAt(pos_.x, pos_.z);
            std::printf("  /locate %-10s -> \"%s\"\n", pick->name, reply.c_str());
            // "from" RATHER THAN "asked", because the stand-off is where the
            // ring STARTS and not what the answer has to be: a fish five
            // metres out from the bank is reached from the bank, which is
            // 2.0 m asked and 5.0 m arrived at and nothing wrong. Only the
            // land rows should land on their own number.
            std::printf("      stood (%.0f, %.0f)  %.1f m off (ring from %.1f)  "
                        "in water: %s  in a trunk: %s\n",
                        pos_.x, pos_.z, off, pick->stand, wet ? "YES -- FAIL" : "no",
                        tree ? "YES -- FAIL" : "no");
            // Facing it. The whole point of the aim is that the animal is on
            // the crosshair, and a yaw that is 180 degrees out looks exactly
            // like a teleport that went nowhere.
            const Vec3 f = forward();
            const Vec3 to = normalize(Vec3(target.x - pos_.x, target.y - pos_.y,
                                           target.z - pos_.z));
            const float dot = f.x * to.x + f.y * to.y + f.z * to.z;
            std::printf("      facing it: %s (cos %.3f)\n", dot > 0.999f ? "yes" : "NO -- FAIL",
                        dot);
            if (wet || tree || dot <= 0.999f) ++wrong;
            // Put the world back under the player before the second pass --
            // the survey above moved them, and the next row is chosen from
            // populations that were filled somewhere else.
            for (int i = 0; i < 120; ++i) world_.update(player_.pos);
        }

        // ---- THE FROG, WHICH IS GATED BY A WOOD *AND* BY WATER -------------
        //
        // (user 2026-09-14: "I dont see any frogs even when doing /locate
        // frog.")
        //
        // ITS ROW USED TO SAY `water = false`, so a miss fell through to the
        // birch BAND CENTRE -- dry birch wood, where fillFrogs cannot place one
        // because it places from a bank and from nothing else. The command ran,
        // reported success, moved you, and took you to the one kind of place
        // the animal does not live. Every time. So the check is not "does
        // /locate answer" but "does where it puts you have frogs in it a moment
        // later", which is the only thing the player was ever asking.
        {
            const LifeName *fr = nullptr;
            for (const LifeName &ln : lifeNames())
                if (std::string(ln.name) == "frog") fr = &ln;
            std::printf("\n  -- /locate frog, the one row gated by a wood AND by water --\n");
            if (!fr) {
                std::printf("  NO FROG ROW -- WRONG\n");
                ++wrong;
            } else {
                const std::string said = locateLife(*fr);
                std::printf("  said: %s\n", said.c_str());
                pos_ = player_.eyePosition();
                const bool birchThere = world_.terrain.birchAt(pos_.x);
                // HOW FAR THE WATER IS FROM WHERE IT PUT YOU. A frog stands
                // within kFrogShoreM of a bank, so an arrival that is not
                // beside one is an arrival with no frogs in its future.
                float best = 1e9f;
                for (float r = 0.0f; r <= 40.0f; r += 1.0f) {
                    const int steps = (r < 1.0f) ? 1 : maxi(8, int(2.0f * PI * r));
                    for (int k = 0; k < steps; ++k) {
                        const float a = float(k) / float(steps) * 2.0f * PI;
                        if (!wetColumnAt(pos_.x + cosf(a) * r, pos_.z + sinf(a) * r)) continue;
                        best = r;
                        r = 41.0f;
                        break;
                    }
                }
                std::printf("  arrived (%.0f, %.0f) -- the %s wood, water %.0f m away\n", pos_.x,
                            pos_.z, birchThere ? "birch" : "PINE", double(best));
                // ...AND THEN GIVE IT THE MOMENT THE REPLY PROMISES.
                warmLife(pos_, 20);
                Vec3 at{0, 0, 0};
                const bool got = nearestLife(Life::Frog, &at);
                std::printf("  frogs after 20 s  %s\n",
                            got ? "yes -- and that is the whole point" : "NONE -- WRONG");
                if (!got || !birchThere || best > 20.0f) ++wrong;
            }
        }

        // ---- ...AND IS ANY OF IT ACTUALLY ON SCREEN ------------------------
        //
        // (user 2026-09-14, three times: "I dont see the frog on the field.")
        //
        // EVERY CHECK IN THIS FILE UNTIL NOW READ THE HOST. livePoints, the
        // census, /locate, the speeds -- all of them ask the population where
        // it thinks it is, and all of them were perfectly happy about a frog
        // that has never once been drawn. The thing none of them touched is the
        // PUBLISH: setFlyerInstance drops a slot past the end of a population's
        // run in silence, and the critters' run was two short before the frog
        // was even added.
        //
        // So this walks the band itself. It is the only test here that would
        // have caught it, and it is four lines.
        {
            // PUBLISHED FIRST, AND THAT IS NOT A DETAIL. warmLife TICKS the
            // populations and never publishes them -- it settles behaviour, it
            // does not draw -- so asking the band straight after it reports
            // every run empty, which is a test that fails on everything and
            // therefore says nothing. The first cut of this did exactly that and
            // read "0 drawn" for the bunnies too.
            critters_.publish(world_, kCritterSlot0);
            bunnies_.publish(world_, kBunnySlot0);
            bunnies_.publishSkunks(world_, kMarchSlot0);
            bees_.publish(world_, kBeeSlot0);
            std::printf("\n  -- published into the flyer band --\n");
            const int runs[][3] = {
                {kCritterSlot0, kCritterSlots, 0},
                {kBunnySlot0, kBunnySlots, 1},
                {kMarchSlot0, kMarchSlots, 2},
                {kBeeSlot0, kBeeSlots, 3},
            };
            static const char *kRun[4] = {"critters", "bunnies", "marchers", "bees"};
            for (const auto &r : runs) {
                int on = 0;
                for (int k = 0; k < r[1]; ++k) on += world_.flyerShown(r[0] + k) ? 1 : 0;
                std::printf("  %-9s slots %3d..%-3d  %d drawn\n", kRun[r[2]], r[0],
                            r[0] + r[1] - 1, on);
            }
            // THE FROG IS THE LAST RUN INSIDE THE CRITTERS' RUN, which is why it
            // was the one that fell off. Counted on its own, by the same
            // arithmetic Critters::publish walks.
            const int frog0 = kCritterSlot0 + kFireflyCount + kAntCount + kHouseflyCount +
                              kLbugCount;
            int frogsDrawn = 0;
            for (int k = 0; k < kFrogCount; ++k)
                frogsDrawn += world_.flyerShown(frog0 + k) ? 1 : 0;
            Vec3 fat{0, 0, 0};
            const int frogsAlive = nearestLife(Life::Frog, &fat) ? 1 : 0;
            std::printf("  frog run  slots %3d..%-3d  %d drawn, %s alive on the host\n", frog0,
                        frog0 + kFrogCount - 1, frogsDrawn, frogsAlive ? "some" : "none");
            if (frogsAlive && !frogsDrawn) {
                std::printf("      ^ ALIVE AND NOT DRAWN -- the band is too short\n");
                ++wrong;
            }
        }

        std::printf("\n  %s\n", wrong ? "FAIL" : "PASS -- every row is wired to its own animal.");
    }

    void runFellTest() {
        std::printf("\n=== FELL TEST ===\n");
        // WHERE THE SPAWN IS, BEFORE ANYTHING ELSE. This runs before the
        // player has been put anywhere, so pos is still the origin -- and
        // streaming the world round the origin finds a wood nobody is standing
        // in. The first run of this printed "no tree within 80 m" for exactly
        // that reason.
        player_.pos = Vec3(opt_.camX, 0.0f, opt_.camZ);

        // The world has to exist before anything can be felled in it, and the
        // chunks are meshed on worker threads -- so this gives them time rather
        // than spinning on a queue they have not filled yet.
        for (int i = 0; i < 400; ++i) {
            world_.update(player_.pos);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        std::printf("  spawn (%.1f, %.1f, %.1f)\n", player_.pos.x, player_.pos.y,
                    player_.pos.z);

        // NOT NAMED 'near'. windows.h still defines near and far as empty macros
        // from the segmented-memory era, so `std::vector<Solid> near;` compiles
        // as `std::vector<Solid> ;` and the errors name neither of them. This
        // file already carries the same note twice; here is the third time.
        std::vector<Solid> around;
        world_.collidersNear(player_.pos, 120.0f, &around);
        const Solid *tree = nullptr;
        float best = 1e30f;
        for (const Solid &s : around) {
            if (s.modelKind != 0 || !s.vol || s.hx <= 0.0f) continue;
            const float dx = s.cx - player_.pos.x, dz = s.cz - player_.pos.z;
            if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; tree = &s; }
        }
        if (!tree) {
            std::printf("  no tree within 80 m of the spawn -- try another --spawn\n");
            return;
        }
        const Solid so = *tree;
        std::printf("  tree at (%.1f, %.1f, %.1f)  model %d  %d x %d voxels\n", so.tx, so.baseY,
                    so.tz, int(so.modelIndex), int(so.msx), int(so.msz));
        // STAND WHERE THE TREE IS. The ground patch follows the player, and a
        // player who has just chopped a tree down is next to it -- so the test
        // has to be too, or it measures a fall over ground that was never
        // streamed into the scene.
        player_.placeOnGround(walkWorld(), so.cx, so.cz);

        // ---- cut it through, aiming at the TRUNK from beside it ------------
        //
        // NOT THE MIDDLE OF ITS BOX. A birch is a trunk with the crown leaning
        // off it, so the box centre can be five metres from the wood -- the
        // same trap the placement fell into. The trunk is where the model is
        // solid at its own row zero, which is its underside.
        double bx = 0.0, bz = 0.0;
        long nb = 0;
        for (int mz = 0; mz < int(so.msz); ++mz)
            for (int mx = 0; mx < int(so.msx); ++mx)
                if (solidVoxel(so, mx, 0, mz)) {
                    bx += double(mx) + 0.5;
                    bz += double(mz) + 0.5;
                    ++nb;
                }
        if (!nb) {
            std::printf("  the model has nothing at its base\n");
            return;
        }
        float wx = 0.0f, wz = 0.0f;
        solidWorldSpace(so, float(bx / double(nb)) * VOXEL_M, float(bz / double(nb)) * VOXEL_M,
                        &wx, &wz);
        std::printf("  trunk at (%.1f, %.1f)\n", wx, wz);
        // SWEPT ACROSS THE TRUNK, not drilled into it. Every blow from the
        // same point along the same ray eats a TUNNEL through the wood, and a
        // tunnel severs nothing -- forty of those left the tree standing. A
        // player's aim wanders across the cut, so this does too.
        const float cutY = so.baseY + 1.2f;
        int blows = 0;
        bool down = false;
        for (; blows < 60 && !down; ++blows) {
            const float off = (float(blows % 11) - 5.0f) * 0.12f;
            const Vec3 eye{wx + 3.0f, cutY + (float(blows % 3) - 1.0f) * 0.1f, wz + off};
            const Vec3 dir{-1.0f, 0.0f, 0.0f};
            if (!world_.carveModel(so, eye, dir, 5.0f, kDigRadiusVox, &spoilVol_, &spoilN_,
                                   &spoilAt_, &spoilYaw_))
                continue;
            down = world_.fellTree(physics_, so, dir, simMs_);
        }
        if (!down) {
            std::printf("  %d blows and it never came down\n", blows);
            return;
        }
        std::printf("  felled after %d blows\n\n", blows);
        std::printf("  %6s %9s %9s %9s %9s %9s %9s %9s\n", "ms", "x", "y", "z", "pitch",
                    "fall m/s", "spin r/s", "in ground");

        // ---- and then watch it -------------------------------------------
        //
        // THE GROUND HAS TO BE IN THE SCENE. The frame loop builds the height
        // field patch when the player leaves the last one; nothing here does,
        // so without this the tree falls through a world with no floor in it --
        // which is a fault in the test rather than in the game, and the first
        // run of this spent its whole trace proving it.
        const float dt = 1.0f / 60.0f;
        for (int f = 0; f < 900; ++f) {
            if (world_.takeGroundDirty() ||
                !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                rebuildGroundPatch();
            physics_.step(dt);
            simMs_ += double(dt) * 1000.0;
            world_.updateDebris(physics_, player_.eyePosition(), simMs_,
                                [&](float x, float z) { return player_.surfaceAt(walkWorld(), x, z); });
            if ((f % 30) != 0) continue;
            for (int i = 0; i < 512; ++i) {
                Vec3 p{0, 0, 0}, lin{0, 0, 0}, ang{0, 0, 0};
                float q[4] = {0, 0, 0, 1};
                if (!world_.debrisPose(i, &p, q)) continue;
                world_.debrisVel(physics_, i, &lin, &ang);
                // How far off upright the model's own +Y has been tipped.
                const float uy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
                const float pitch = acosf(uy < -1.0f ? -1.0f : (uy > 1.0f ? 1.0f : uy)) *
                                    57.29578f;
                // HOW MUCH OF THE DRAWN TREE IS UNDER THE GROUND RIGHT NOW.
                // Per voxel, per column -- see World::debrisSink. A body that
                // sweeps through the hillside on its way over and comes out
                // clean is a different fault from one that settles buried, and
                // without this column they look identical in a trace.
                const WalkWorld wwT = walkWorld();
                const World::Sink sk = world_.debrisSink(
                    i, [&](float x, float z) { return walkGroundM(wwT, x, z); });
                std::printf("  %6.0f %9.2f %9.2f %9.2f %8.1fd %9.2f %9.2f  %5.1f%% %.1fm\n",
                            double(f) * dt * 1000.0, p.x, p.y, p.z, pitch, -lin.y,
                            sqrtf(ang.x * ang.x + ang.y * ang.y + ang.z * ang.z),
                            100.0 * double(sk.under) / double(maxi(1, sk.solid)), double(sk.worst));
                break;
            }
        }
        // ...AND WHETHER IT ENDED UP INSIDE ANYTHING. The reported bug was a
        // tree clipping into a rock as it felled, and this is that question
        // asked of the collider the solver was actually given.
        int clipped = 0, boxes = 0;
        for (int i = 0; i < 512; ++i) {
            Vec3 p{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            clipped = world_.debrisClip(i);
            boxes = world_.debrisWindowBoxes();
            break;
        }
        std::printf("\n  collider boxes inside a rock or a trunk at rest: %d"
                    "   (static window %d boxes)\n",
                    clipped, boxes);

        // -- ...AND HOW MUCH OF THE TREE IS INSIDE THE HILLSIDE --------------
        //
        // The line above asks the SOLVER's shape about ROCKS. This asks the
        // DRAWN tree about the GROUND, which is the question "the tree clips
        // through the terrain" is actually about -- and the one nothing here
        // has ever asked. See World::debrisSink for why it is per voxel and
        // per column rather than a bounding box against a corner height.
        {
            const WalkWorld wwS = walkWorld();
            for (int i = 0; i < kDebrisInstances; ++i) {
                Vec3 p{0, 0, 0};
                float q[4] = {0, 0, 0, 1};
                if (!world_.debrisPose(i, &p, q)) continue;
                const World::Sink sk = world_.debrisSink(
                    i, [&](float x, float z) { return walkGroundM(wwS, x, z); });
                if (!sk.solid) continue;
                std::printf("  slot %d: %d of %d voxels are under the ground (%.1f%%), "
                            "deepest %.2f m at (%.1f, %.1f, %.1f)\n",
                            i, sk.under, sk.solid,
                            100.0 * double(sk.under) / double(maxi(1, sk.solid)), double(sk.worst),
                            double(sk.worstAt[0]), double(sk.worstAt[1]), double(sk.worstAt[2]));
            }
        }

        // ---- AND THE ONE NUMBER THAT SAYS "IT LANDED" ---------------------
        //
        // The trajectory above reports the body's ORIGIN, which for a felled
        // tree is the model's base corner -- metres from the wood once the
        // thing is lying down, and below the ground by construction because the
        // collider starts at the CUT. Reading it as a height is what made a
        // floating tree and a buried one look the same in this trace, twice.
        //
        // So: the SHAPES' own bounds against the ground under them. A collider
        // barely taller than it is wide is a tree that never got a trunk -- the
        // birch failure, where an 11 m tree was given an 0.8 m stub, could not
        // topple, and left the mesh hanging in the air.
        for (int i = 0; i < 512; ++i) {
            Vec3 p{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            // CONTINUE, NOT BREAK. debrisPose answers for a slot whose actor
            // has already been swept, and breaking there printed nothing at all
            // -- which reads as "the test did not run" rather than "that slot
            // was stale", and cost a rebuild to tell apart.
            if (!world_.debrisBounds(physics_, i, &lo, &hi)) continue;
            const WalkWorld ww = walkWorld();
            float g = walkGroundM(ww, lo.x, lo.z);
            g = maxf(g, walkGroundM(ww, hi.x, lo.z));
            g = maxf(g, walkGroundM(ww, lo.x, hi.z));
            g = maxf(g, walkGroundM(ww, hi.x, hi.z));
            g = maxf(g, walkGroundM(ww, (lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f));
            std::printf("\n  collider   %.1f x %.1f x %.1f m   longest side %.1f m\n",
                        double(hi.x - lo.x), double(hi.y - lo.y), double(hi.z - lo.z),
                        double(maxf(hi.x - lo.x, maxf(hi.y - lo.y, hi.z - lo.z))));
            std::printf("  at rest    underside %.2f m, ground %.2f m  -->  %+.2f m %s\n",
                        double(lo.y), double(g), double(lo.y - g),
                        (lo.y - g > 0.5f) ? "FLOATING" : "resting");
            break;
        }
        std::printf("\n  (pitch 0 = still standing, 90 = flat on the ground)\n");

        // -------------------------------------------------------------------
        // ...AND NOW CHOP THE LOG THAT IS LYING THERE.
        //
        // "the player should be able to hit a fellen tree with an axe for
        // example and it break even when felled on the ground."
        //
        // Two separate things to prove, and the first one is the whole of the
        // reported bug: can the swing SEE it. Before World::debrisRay the
        // answer was no and could not be anything else -- a felled tree is not
        // in w.solids, which is the only list the swing walked.
        //
        // The second is the break. Nothing counts blows: the bite is carved out
        // of the body's own voxels and the body is re-made as however many
        // six-connected pieces that left, so "it broke" is the loose count
        // going up on the blow that reached the far side of the trunk.
        // -------------------------------------------------------------------
        std::printf("\n=== CHOP TEST -- the axe against the log on the ground ===\n");
        int log = -1;
        Vec3 lo{0, 0, 0}, hi{0, 0, 0};
        for (int i = 0; i < kDebrisInstances; ++i) {
            Vec3 p{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            if (!world_.debrisBounds(physics_, i, &lo, &hi)) continue;
            log = i;
            break;
        }
        if (log < 0) {
            std::printf("  nothing loose left to chop\n");
            return;
        }

        // STAND BESIDE THE WOOD, NOT BESIDE THE BOX.
        //
        // A felled tree is a long thin thing lying at an angle inside a big
        // box, so the middle of its bounds is a point in the air next to the
        // log about as often as it is the log -- and the swing reaches 5.3 m,
        // which the far side of that box is not within. Same trap runFellTest
        // hit aiming at a standing birch, same answer: aim at where the model
        // is SOLID. See World::debrisAim.
        Vec3 wood{0, 0, 0};
        if (!world_.debrisAim(log, &wood, /*butt=*/true)) {
            std::printf("  the body has no voxels to aim at\n");
            return;
        }
        // ACROSS THE TRUNK, NOT ALONG IT. The collider's longest horizontal
        // side is the log's own axis, and the cut plane is perpendicular to it.
        const bool alongX = (hi.x - lo.x) >= (hi.z - lo.z);
        const Vec3 mid = wood;
        const Vec3 dir = alongX ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 eye0 = alongX ? Vec3{mid.x, mid.y, mid.z - 2.5f}
                                 : Vec3{mid.x - 2.5f, mid.y, mid.z};
        std::printf("  log slot %d   collider %.1f x %.1f x %.1f m   axis %c   %d voxels\n", log,
                    double(hi.x - lo.x), double(hi.y - lo.y), double(hi.z - lo.z),
                    alongX ? 'X' : 'Z', world_.debrisVoxels(log));
        std::printf("  wood at (%.1f, %.1f, %.1f) -- the THICK end -- swinging from 2.5 m %s\n",
                    wood.x, wood.y, wood.z, alongX ? "-Z" : "-X");

        // ---- 1. CAN THE SWING SEE IT AT ALL --------------------------------
        {
            DebrisHit dh;
            const bool saw = world_.debrisRay(eye0, dir, swingReachM(dir), &dh);
            std::printf("  the swing ray %s\n",
                        saw ? "FINDS the log" : "FINDS NOTHING -- this is the reported bug");
            if (saw)
                std::printf("    slot %d at %.2f m   voxel (%d, %d, %d)   material %u   %s\n",
                            dh.slot, double(dh.t), dh.vox[0], dh.vox[1], dh.vox[2],
                            unsigned(dh.mat),
                            dh.takes == kDebrisWood   ? "wood -- an axe takes it"
                            : dh.takes == kDebrisSoft ? "mushroom -- either tool takes it"
                                                      : "stone");
            else
                return;
        }

        // ---- 2. AND DOES IT BREAK ------------------------------------------
        //
        // ONE CUT PLANE, AND THE BLOW GOES WHEREVER THERE IS STILL WOOD IN IT.
        //
        // A FIXED SWEEP DOES NOT SEVER ANYTHING, which cost a run to learn: a
        // band of seventeen heights cut seventeen tunnels through the trunk, the
        // rays then passed clean through the holes they had made, and the log
        // sat there at 63,388 voxels for another three hundred and sixty blows.
        // The wood that was holding it together was ABOVE AND BELOW the band --
        // branches, and on a pine a great deal of needle -- and nothing was ever
        // aimed at it.
        //
        // So each blow scans the cut plane for whatever is still there and hits
        // that. A player does this without thinking about it; a test has to be
        // told. When the scan comes back empty the plane is clear, which is the
        // same thing as the log being in two pieces.
        const int before = world_.looseCount();
        const int vox0 = world_.debrisVoxels(log);
        int cuts = 0, landed = 0, broke = -1;
        double carveMs = 0.0;
        const float dt2 = 1.0f / 60.0f;
        for (; cuts < 600 && broke < 0; ++cuts) {
            const float slide = (float(cuts % 5) - 2.0f) * 0.05f;
            DebrisHit dh;
            bool found = false;
            for (int step = 0; step < 61 && !found; ++step) {
                // Outwards from the middle of the trunk, so the kerf is worked
                // from the wood the aim found rather than from the top down.
                const int k = (step + 1) / 2;
                const float rise = ((step & 1) ? -1.0f : 1.0f) * float(k) * 0.1f;
                Vec3 eye = eye0;
                eye.y = mid.y + rise;
                if (alongX)
                    eye.x = mid.x + slide;
                else
                    eye.z = mid.z + slide;
                found = world_.debrisRay(eye, dir, swingReachM(dir), &dh);
            }
            if (found) {
                // WHAT A BLOW ON A LOG COSTS. The bite re-meshes the body's
                // whole volume and rebuilds its structure, which is the same
                // work felling one does -- once per swing, so half a second
                // apart, but it is on the frame and worth a number.
                const auto t0 = std::chrono::steady_clock::now();
                const bool bit = world_.carveDebris(physics_, dh, kDigRadiusVox, simMs_,
                                                    &spoilVol_, &spoilN_, &spoilAt_, &spoilYaw_);
                carveMs += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
                if (bit) ++landed;
            }
            // A few frames between blows, the way a swing is half a second
            // apart -- so a piece that has come away has somewhere to fall.
            for (int f = 0; f < 6; ++f) {
                physics_.step(dt2);
                simMs_ += double(dt2) * 1000.0;
                world_.updateDebris(physics_, player_.eyePosition(), simMs_, [&](float x, float z) {
                    return player_.surfaceAt(walkWorld(), x, z);
                });
            }
            if (world_.looseCount() > before) broke = cuts + 1;
            if (((cuts + 1) % 150) == 0)
                std::printf("    %3d blows: %d landed, %d voxels left\n", cuts + 1, landed,
                            world_.debrisVoxels(log));
        }
        std::printf("  %d blows, %d of them carved wood\n", cuts, landed);
        std::printf("  bodies %d -> %d\n", before, world_.looseCount());
        if (landed)
            std::printf("  %.2f ms per blow -- the bite, the re-mesh and the break together\n",
                        carveMs / double(landed));
        if (broke > 0)
            std::printf("  PASS -- the log came apart on blow %d\n", broke);
        else if (landed > 0)
            std::printf("  the axe bit %d times but the log held (voxels %d -> %d)\n", landed,
                        vox0, world_.debrisVoxels(log));
        else
            std::printf("  FAIL -- no blow landed on it\n");
        if (broke <= 0) return;

        // ---- AND WHAT THE PIECES DO AFTERWARDS -----------------------------
        //
        // A break that leaves two bodies in the debris band is only half of it.
        // Each piece has to be a REAL body: its own collider, resting on the
        // ground rather than hanging where the log used to be (the rule is that
        // nothing floats), and hittable again -- because "chop it in half and
        // then chop the halves" is the next thing anybody does.
        for (int f = 0; f < 240; ++f) {
            physics_.step(dt2);
            simMs_ += double(dt2) * 1000.0;
            world_.updateDebris(physics_, player_.eyePosition(), simMs_, [&](float x, float z) {
                return player_.surfaceAt(walkWorld(), x, z);
            });
        }
        std::printf("\n  four seconds later:\n");
        const WalkWorld ww2 = walkWorld();
        for (int i = 0; i < kDebrisInstances; ++i) {
            Vec3 p{0, 0, 0}, bl{0, 0, 0}, bh{0, 0, 0};
            float q[4] = {0, 0, 0, 1};
            if (!world_.debrisPose(i, &p, q)) continue;
            if (!world_.debrisBounds(physics_, i, &bl, &bh)) continue;
            float g = walkGroundM(ww2, bl.x, bl.z);
            g = maxf(g, walkGroundM(ww2, bh.x, bl.z));
            g = maxf(g, walkGroundM(ww2, bl.x, bh.z));
            g = maxf(g, walkGroundM(ww2, bh.x, bh.z));
            // ...and can it be hit again. Asked from right beside the piece,
            // which is where the player is standing by now.
            Vec3 aim{0, 0, 0};
            bool again = false;
            if (world_.debrisAim(i, &aim)) {
                DebrisHit dh2;
                const Vec3 e2{aim.x - 2.0f, aim.y, aim.z};
                again = world_.debrisRay(e2, Vec3{1.0f, 0.0f, 0.0f}, swingReachM(Vec3{1, 0, 0}),
                                         &dh2);
            }
            // ...AND WHETHER SOMETHING IS HOLDING IT UP. "Above the ground" is
            // not "in the air": a branch cut off a trunk that is still standing
            // on its end can quite properly come to rest against the next tree,
            // and the static window round the body is exactly the boxes of the
            // wood near it. So the piece is asked what is UNDER it and how fast
            // it is moving, and only a piece with nothing under it and nothing
            // happening to it is floating.
            Vec3 lin{0, 0, 0}, ang{0, 0, 0};
            world_.debrisVel(physics_, i, &lin, &ang);
            const float speed = sqrtf(lin.x * lin.x + lin.y * lin.y + lin.z * lin.z);
            float under = 0.0f;
            const int what = physics_.typeBelow(Vec3{(bl.x + bh.x) * 0.5f, bl.y - 0.05f,
                                                     (bl.z + bh.z) * 0.5f},
                                                40.0f, &under);
            const bool held = (what >= 0 && under < 1.0f) || speed > 0.05f;
            std::printf("    slot %2d  %6d voxels  %4.1f x %4.1f x %4.1f m  underside %+.2f m of "
                        "ground  %-8s  %.2f m/s  under: %s\n",
                        i, world_.debrisVoxels(i), double(bh.x - bl.x), double(bh.y - bl.y),
                        double(bh.z - bl.z), double(bl.y - g),
                        (bl.y - g > 0.5f && !held) ? "FLOATING" : "resting", double(speed),
                        what < 0 ? "nothing within 40 m"
                                 : (what == 5 ? "the ground" : "a static box"));
            if (!again) std::printf("      CANNOT BE HIT\n");
        }
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
                                    [this](float x) { return world_.terrain.birchMix(x); },
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
                                         [this](float x) { return world_.terrain.birchMix(x); },
                                         banksNear_, perches_, isNight(),
                                         Vec3(0.0f, 0.0f, 0.0f),
                                         [this](float x, float z) {
                                             return waterTopAt(x, z);
                                         });
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
                        std::printf("  %-8s none placed%s\n", kMarchSpec[mk].name,
                                    kMarchSpec[mk].wood < 0
                                        ? ""
                                        : (kMarchSpec[mk].wood == 0 ? " (pine only)"
                                                                    : " (birch only)"));
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
                                        [this](float x) { return world_.terrain.birchMix(x); },
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

    // -----------------------------------------------------------------------
    // HOW MUCH OF A WOOD THE LISTENER IS STANDING IN.  0 in the open, 1 under
    // a closed canopy, and it drives nothing but the ambience volume.
    //
    // IT IS THE PLANTING RULE, NOT A SECOND OPINION.  scene/chunks.h decides
    // whether a cell grows a tree with
    //
    //     saturate((dens - 0.30) / 0.32) * 0.92 + 0.05
    //
    // and the canopy-closure ramp inside it is reused here verbatim. A
    // separate "am I in a forest" field would be a second definition of the
    // wood, and two definitions drift apart the first time either is tuned --
    // the audible symptom being birds in a clearing.
    //
    // The 0.05 floor is deliberately NOT carried over. That floor is the
    // handful of stragglers a real clearing still has standing in it, and a
    // clearing you can still hear the wood from is not a clearing.
    //
    // BOTH BANDS COUNT: pine and birch are both woods, so birchMix does not
    // appear here at all.
    //
    // AND THERE IS NO SLOPE TEST, though the planter has one. kTreeSlope stops
    // a tree standing on scree; it does not stop scree being in the middle of
    // a forest, and cutting the birds because you stepped onto a boulder field
    // would put a hard edge in a signal that is otherwise smooth everywhere.
    //
    // THE WATER FADE IS KEPT, AND RAMPED. topMaterial paints sand up to 3.4 m
    // and the planter refuses that band outright -- a beach and a lake are the
    // one part of this world with no wood in them. Ramped over the four metres
    // above it rather than switched at it, which also lands the full level at
    // the same 7.6 m chooseSpawn calls "well clear of the shore". A step test
    // on the height field is exactly the pop the smoothing in
    // Ambience::update should not be asked to hide.
    // -----------------------------------------------------------------------
    float forestGain() const {
        const VoxelTerrain &t = world_.terrain;
        const float x = pos_.x, z = pos_.z;

        // The band's line. A dry band answers kNoWater, so `above` comes out
        // enormous and the gain saturates -- which is what "nowhere near water"
        // should mean here.
        const float above = t.heightM(x, z) - (t.waterAt(x) + 0.8f);
        if (!(above > 0.0f)) return 0.0f;
        const float wet = above < 4.0f ? above * 0.25f : 1.0f;

        const float closure = (t.standDensity(x, z) - 0.30f) / 0.32f;
        return wet * (closure < 0.0f ? 0.0f : (closure > 1.0f ? 1.0f : closure));
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
        float bestX = opt_.camX, bestZ = opt_.camZ;
        bool found = false;

        // -- HOW OPEN IS IT, AND IS THE OPENING FACING THE SUN --------------
        //
        // The stand-density field is what plants the trees -- the gate in
        // scene/chunks.h keeps 5% of the base density at 0.30 and 97% at 0.62 --
        // so asking it is asking how many trunks are here, without a chunk
        // having to exist.
        //
        // A DISC, NOT A POINT. One cell of low density is a hole between two
        // trees, not a glade; a ring at twelve metres is what tells the two
        // apart.
        //
        // AND ALONG THE SUN'S OWN LINE, which is the half that actually
        // delivers sunlight. A clearing is not the same thing as a sunlit
        // clearing: at the default elevation of 24 degrees a 22 m pine throws
        // about fifty metres of shadow, so a twenty-metre glade with a wall of
        // trees on its sunward side is in shade for the whole morning. The
        // sunward samples run out to sixty metres and carry half the score.
        //
        // The azimuth is the one the options carry rather than the clock's,
        // because the clock is not running yet when this is called -- and the
        // base is where the day starts, which is when a spawn happens.
        FbmMemo dm;
        const float sunA = opt_.sunAz * PI / 180.0f;
        const float sunX = cosf(sunA), sunZ = sinf(sunA);
        auto openness = [&](float px, float pz) {
            float local = t.standDensity(px, pz, dm);
            for (int k = 0; k < 8; ++k) {
                const float a2 = float(k) * (TWO_PI / 8.0f);
                local += t.standDensity(px + cosf(a2) * 12.0f, pz + sinf(a2) * 12.0f, dm);
            }
            local *= (1.0f / 9.0f);
            float sunward = 0.0f;
            for (int k = 1; k <= 6; ++k) {
                const float d2 = float(k) * 10.0f;
                sunward += t.standDensity(px + sunX * d2, pz + sunZ * d2, dm);
            }
            sunward *= (1.0f / 6.0f);
            return 0.5f * local + 0.5f * sunward;
        };

        // -- AND CAN THE GROUND ITSELF SEE THE SUN --------------------------
        //
        // Openness is about TRUNKS. This is about the hill, and at this sun it
        // matters just as much: 24 degrees of elevation means a rise of one
        // metre shadows two and a half metres of ground behind it, so a glade
        // on the wrong side of a ridge is a glade in shade all morning. The
        // first cut of this found beautifully sparse spots that were dark.
        //
        // One march along the sun's own bearing, out to ninety-odd metres,
        // asking whether the height field ever climbs above the line the sun
        // comes in on. It is the honest terrain-shadow test and it is cheap
        // enough BECAUSE it runs last -- only for a candidate that has already
        // beaten everything before it, which is a handful of times, not 512.
        //
        // The elevation is the option's rather than the clock's, for the reason
        // the azimuth is: the clock has not started when this is called.
        const float tanEl = tanf(maxf(2.0f, opt_.sunEl) * PI / 180.0f);
        auto sunlit = [&](float px, float pz, float ph) {
            for (int k = 1; k <= 16; ++k) {
                const float d2 = float(k) * 6.0f;
                if (t.heightM(px + sunX * d2, pz + sunZ * d2) > ph + d2 * tanEl) return false;
            }
            return true;
        };

        // THE BEST OF ALL OF THEM, not the first that passes. The old rule took
        // whatever candidate cleared a band and stopped, which is why it opened
        // in a wood as often as not: a band admits the thick end of itself just
        // as readily as the thin end.
        float bestScore = 1e9f;
        float anyX = opt_.camX, anyZ = opt_.camZ, anyScore = 1e9f;

        for (uint32_t i = 0; i < 512; ++i) {
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
            // Asked per candidate rather than hoisted: the waterline is per
            // band now, and a spawn search ranges far enough to cross one.
            if (h < t.waterAt(x) + 5.0f) continue;

            const int ci = int(floorf(x / VOXEL_M)), cj = int(floorf(z / VOXEL_M));
            const int slope = maxi(absi(t.heightVox(ci + 1, cj) - t.heightVox(ci - 1, cj)),
                                   absi(t.heightVox(ci, cj + 1) - t.heightVox(ci, cj - 1)));
            if (slope >= VoxelTerrain::kTreeSlope) continue;  // scree, not ground

            const float score = openness(x, z);
            // Kept whatever happens, so a seed that finds nothing ideal still
            // spawns somewhere sensible rather than at the world origin.
            if (score < anyScore) {
                anyScore = score;
                anyX = x;
                anyZ = z;
            }
            // TOO THICK is a wall of trunks to wake up in. TOO OPEN is a bald
            // patch, which is not the wood this engine is for -- the point is
            // to open in sunlight AMONG trees, not away from them.
            if (score > 0.45f || score < 0.20f) continue;
            if (score >= bestScore) continue;
            // LAST, because it is the expensive one -- see the note over it.
            if (!sunlit(x, z, h)) continue;
            bestScore = score;
            bestX = x;
            bestZ = z;
            found = true;
        }
        if (!found) {
            bestX = anyX;
            bestZ = anyZ;
            bestScore = anyScore;
        }

        opt_.camX = bestX;
        opt_.camZ = bestZ;
        std::printf("  spawn    %.1f, %.1f  openness %.2f%s  (--spawn %u to come back here)\n",
                    bestX, bestZ, double(bestScore), found ? "" : " -- nothing better found",
                    unsigned(seed));
        std::fflush(stdout);
    }

    // A spawn inside a trunk is a spawn you cannot walk out of: a tree is a
    // wall at every height, so the collision resolver has nowhere to push you.
    // The terrain test above cannot see trees -- they are placed per chunk and
    // no chunk existed yet -- so this runs once the ring is resident and steps
    // outward until the body fits.
    // How far a solid may rise above the ground before standing where it is
    // counts as being INSIDE it rather than on it. A voxel is 10 cm and the
    // player steps up rather more than that, so this is a low kerb: anything
    // taller is something you would be buried in.
    static constexpr float kSpawnStepM = 0.45f;

    void nudgeOutOfSolids() {
        // NOT named 'near'. windows.h, which this file includes for the mouse
        // capture, still defines near and far as empty macros from the segmented
        // memory era, and the error it produces names neither of them.
        std::vector<Solid> nearby;
        world_.collidersNear(player_.pos, 12.0f, &nearby);
        // -- INSIDE IS THE TEST, NOT UNSTANDABLE (user 2026-09-07) ----------
        //
        // "dont spawn me into rocks, or anything for that matter." This asked
        // `!s.standable`, which excludes precisely the thing that was being
        // complained about: a rock IS standable -- that is what lets you climb
        // a small one -- so every rock in the wood was skipped by the test
        // meant to keep you out of them, and a spawn inside a boulder was not a
        // near miss but a case the check declined to look at.
        //
        // Standable is the wrong question anyway. Standing ON a rock is fine
        // and being INSIDE one is not, and those differ by HEIGHT, not by kind.
        // So the test is vertical now: a solid blocks the spot if its footprint
        // holds you AND its top stands more than a step above the ground you
        // would be placed on -- which is the definition of being embedded in
        // it. A pebble whose top is within a step is something you walk onto,
        // and it still does not block.
        //
        // Trunks keep working unchanged: a trunk's top is a canopy twenty
        // metres up, so it fails the height test by a mile, exactly as it
        // failed the standable test before.
        //
        // AND THE FOOTPRINT IS THE MODEL'S VOXELS. `touches` is the collider
        // ellipse -- a circle round the widest part of a model's bottom two
        // metres -- so it covered several metres of open air beside a big
        // boulder and pushed a spawn out of ground that was perfectly clear,
        // while a body under a leaning crown was inside voxels the ellipse did
        // not reach. Asking the voxels answers both, and answers them about the
        // body's own height rather than about a shadow on the ground.
        TerrainMemo nm;
        auto blocked = [&](float x, float z) {
            const float g = world_.terrain.heightM(x, z, nm);
            for (const Solid &s : nearby) {
                if (s.hx <= 0.0f || s.hz <= 0.0f) continue;
                if (s.vol) {
                    if (solidBoxOverlap(s, x, g, z, g + kBodyHeightM, player_.halfWidth, VOXEL_M))
                        return true;
                    continue;
                }
                if (!touches(s, x, z, player_.halfWidth)) continue;
                if (s.top > g + kSpawnStepM) return true;
            }
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
                std::printf("  spawn    stepped %.1f m clear of solid ground cover\n", rad);
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

    // -----------------------------------------------------------------------
    // THE PIXEL FONT, and the one number that decides whether it looks like one
    // -----------------------------------------------------------------------
    //
    // 3x3-pixel.otf is drawn on a 128-unit grid inside a 640-unit em: five
    // cells to a capital, four to an x-height, six to an advance, and every
    // outline coordinate in the file a multiple of 128. So there is exactly
    // one thing that can go wrong with it, and it is the thing that goes wrong
    // with every pixel font -- a cell that does not land on a whole number of
    // screen pixels is a cell rendered as a grey smear, and a face made
    // entirely of squares has nothing else to look at.
    //
    // ImGui rasterises at SizePixels / (ascent - descent), and this face's
    // ascent and descent are 1024 and -256, so the divisor is 1280 and one
    // 128-unit cell lands at SizePixels/10 screen pixels. ONLY MULTIPLES OF
    // TEN ARE WHOLE CELLS. This quantises to them, and nothing downstream is
    // allowed to scale the result -- see the note on scale in styleV2.
    static float px3Px(float fbH) {
        // Matched to the face it replaces rather than to the line box, which
        // this font leaves half empty: a capital here is five cells, or half
        // the size, where Consolas at v2FontPx gives about six tenths of it.
        const float want = 1.2f * v2FontPx(fbH);
        // v2FontPx stops at 24, so in practice this is twenty up to about
        // 1200 lines and thirty above it -- two screen pixels to a cell, or
        // three.
        return clampf(roundf(want / 10.0f) * 10.0f, 20.0f, 30.0f);
    }

    ImFont *px3_ = nullptr;    // the face at px3Size_, or null for Consolas
    float px3Size_ = 0.0f;
    bool px3Failed_ = false;

    // Bakes the pixel font for this framebuffer and says whether it just did.
    //
    // WHY THIS RUNS INSIDE A FRAME, which is the one thing ImGui asks you not
    // to do to a font atlas: Falcor keeps its Gui private to SampleApp and
    // hands it out nowhere but onGuiRender, so this is the only place that can
    // reach addFont at all. The atlas is marked Locked between NewFrame and
    // Render to catch exactly this, so the lock comes off for the length of
    // the call and goes back on -- and the caller draws NOTHING on a frame
    // that baked, because rebuilding the atlas moves the white-pixel texel
    // that NewFrame had already cached for every filled rectangle. A frame
    // without a readout is invisible; a frame of panels filled with a piece of
    // a letter is not.
    //
    // Falcor rebuilds and re-uploads the atlas texture inside addFont and
    // nowhere else, which is why that call is here: it IS the upload. The copy
    // it loads for itself is at a hardcoded 14 px -- an eighth of a cell, and
    // unusable -- and is never drawn with. It is registered under the name
    // anyway so that a stray setActiveFont("px3") finds something, Falcor's
    // own dereferencing its iterator whether or not the lookup succeeded.
    bool bakePx3(Gui *gui, float fbH) {
        if (px3Failed_) return false;
        if (opt_.font.empty() || opt_.font == "off") {
            px3_ = nullptr;
            return false;
        }
        const float want = px3Px(fbH);
        if (px3_ && want == px3Size_) return false;

        // Asked before the atlas is touched rather than after: addFont throws
        // on a file it cannot read, and a throw halfway through would leave
        // ImGui holding a font the uploaded texture does not have -- which is
        // not a missing font, it is every glyph in the interface reading from
        // the wrong place in the atlas.
        if (FILE *fp = std::fopen(opt_.font.c_str(), "rb")) {
            std::fclose(fp);
        } else {
            std::fprintf(stderr, "v2: cannot open font %s -- drawing in Consolas\n",
                         opt_.font.c_str());
            px3Failed_ = true;
            return false;
        }

        ImGuiIO &io = ImGui::GetIO();
        const bool locked = io.Fonts->Locked;
        io.Fonts->Locked = false;

        ImFontConfig cfg;
        // NO OVERSAMPLING, WHICH IS NOT THE DEFAULT. stb's is a horizontal
        // prefilter: it rasterises at three times the width and blurs back
        // down so a glyph still reads at a fractional position. That is the
        // right answer for an outline face and the exact wrong one for a grid
        // of squares -- it is a blur, and here the squares are the whole
        // picture. PixelSnapH is the other half: it keeps the text origin
        // whole, so the cells cannot drift off the grid they were baked onto.
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        ImFont *face = io.Fonts->AddFontFromFileTTF(opt_.font.c_str(), want, &cfg);
        bool ok = face != nullptr;
        if (ok) {
            try {
                gui->addFont("px3", opt_.font);
            } catch (const std::exception &e) {
                std::fprintf(stderr, "v2: font atlas upload failed (%s)\n", e.what());
                ok = false;
            }
        }
        io.Fonts->Locked = locked;
        if (!ok) {
            px3Failed_ = true;
            px3_ = nullptr;
            return false;
        }
        // ---- NO CAPITALS, AND IT IS DONE IN THE FACE ---------------------
        //
        // Asked for as a rule about the interface rather than about any one
        // label, so it is kept somewhere no label can get past it: A to Z are
        // pointed at the glyphs for a to z, in this font and no other.
        //
        // NOT BY LOWERCASING THE STRINGS, which is the obvious way and is a
        // trap. ImGui hashes a widget's LABEL into its ID: rewriting the
        // literals renames every control in the engine, the positions and
        // sizes ImGui remembers between runs are filed under those names, and
        // any two labels differing only in case would collapse onto one id and
        // become one widget. Remapping the face leaves every string exactly as
        // it was written -- and catches the text v2 does not own as well, the
        // framework's own included.
        //
        // What is typed is also untouched: a capital in the console still
        // reaches runCommand as a capital, it is only drawn as a lowercase.
        //
        // TWO TABLES, because ImGui reads case-sensitively from both.
        // FindGlyph goes through IndexLookup and CalcTextSize takes a fast
        // path through IndexAdvanceX; remap only the first and the capitals
        // draw as lowercase but are still laid out at their old, wider
        // advance -- a line of gaps.
        if (face->IndexLookup.Size > 'z' && face->IndexAdvanceX.Size > 'z') {
            for (int up = 'A'; up <= 'Z'; ++up) {
                const int lo = up - 'A' + 'a';
                face->IndexLookup[up] = face->IndexLookup[lo];
                face->IndexAdvanceX[up] = face->IndexAdvanceX[lo];
            }
        }

        px3_ = face;
        px3Size_ = want;
        const bool lower = face->FindGlyph('A') == face->FindGlyph('a');
        std::printf("v2: text in %s at %.0f px (%.0f-pixel cells), %s\n", opt_.font.c_str(),
                    want, want / 10.0f,
                    lower ? "lowercase only" : "MIXED CASE -- the remap did not take");
        return true;
    }

    // The pixel font for the length of a window's contents.
    //
    // DECLARED AFTER THE WINDOW IT APPLIES TO, always: Falcor pushes its own
    // active font INSIDE Gui::Window -- pushWindow does it after Begin -- so a
    // push made before the window is the one that loses. Being destroyed
    // before the window is what then keeps the two pushes balanced.
    struct px3Font {
        bool on;
        px3Font(ImFont *f) : on(f != nullptr) {
            if (on) ImGui::PushFont(f);
        }
        ~px3Font() {
            if (on) ImGui::PopFont();
        }
    };

    struct styleV2 {
        Gui *gui;
        // What the windows have to pass to SetWindowFontScale to land on
        // v2FontPx. Falcor loads its fonts at fourteen points times whatever
        // the display scaling is, so the number is not knowable up front --
        // unless the pixel font is on, in which case it is one and the reason
        // is below.
        float scale = 1.0f;

        styleV2(Gui *g, ImFont *px, float veil, float fbH) : gui(g) {
            const float target = v2FontPx(fbH);
            if (px) {
                // ONE, AND IT IS THE WHOLE POINT. The pixel font is baked at
                // the size it is drawn at (px3Px), so there is nothing left to
                // scale -- and scaling is precisely what would undo it, since
                // ImGui resamples the atlas bilinearly and even an exact
                // doubling lands every destination pixel between two texels
                // and hands back a grey edge on every square. The window still
                // has to be TOLD one: SetWindowFontScale is remembered per
                // window, and these windows outlive a change of face.
                scale = 1.0f;
            } else {
                // The same fixed-pitch face v2 asked GDI for. Falcor registers
                // it at startup; it only has to be switched on.
                gui->setActiveFont("monospace");
                scale = target / maxf(1.0f, ImGui::GetFontSize());
                font_ = true;
            }

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
            if (font_) gui->setActiveFont("");
        }
        ImGuiStyle saved_;
        int colors_ = 0;
        bool font_ = false;  // whether the ctor moved Falcor's active font
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
        crosshair_ = Falcor::FullScreenPass::create(getDevice(), "v2/shaders/Crosshair.ps.slang");
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
        // ...AND NOT OVER A SCREEN THAT IS SWITCHING OFF. The crosshair is
        // drawn after the tone map, so it would sit there in full brightness
        // over a collapsing picture and then float alone on the black.
        if (!crosshair_ || menuOpen_ || quitting_) return;

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
    // -----------------------------------------------------------------------
    // THE DISTRIBUTION, AND WHAT THE BAD END OF IT WAS DOING.
    //
    // PERCENTILES RATHER THAN A MEAN, because the mean is the number that hid
    // this. What a player feels is the worst one per cent: at 60 fps a frame
    // has 16.7 ms, and every frame over that is a visible stutter however good
    // the average is.
    // -----------------------------------------------------------------------
    void printHitch() {
        if (hitch_.size() < 30) {
            std::printf("\n=== HITCH: only %zu frames, not enough to judge ===\n",
                        hitch_.size());
            return;
        }
        // THE FIRST SECOND IS THE PRIME, not the walk. Every chunk in the
        // starting disc arrives at once there; including it would report the
        // load as though it were something you feel while strolling.
        const size_t skip = std::min<size_t>(hitch_.size() / 4, 120);
        std::vector<HitchFrame> f(hitch_.begin() + long(skip), hitch_.end());
        std::vector<float> t;
        t.reserve(f.size());
        for (const HitchFrame &h : f) t.push_back(h.total);
        std::sort(t.begin(), t.end());
        auto pc = [&](double q) { return t[size_t(q * double(t.size() - 1))]; };
        const double budget = 16.7;   // one frame at 60 fps
        size_t over = 0, over2 = 0;
        double sum = 0.0;
        for (float v : t) {
            sum += v;
            if (v > budget) ++over;
            if (v > budget * 2.0) ++over2;
        }
        std::printf("\n=== HITCH: %zu frames walking (%zu of prime skipped) ===\n", f.size(),
                    skip);
        std::printf("  mean %.2f ms   median %.2f   p90 %.2f   p99 %.2f   worst %.2f\n",
                    sum / double(t.size()), pc(0.50), pc(0.90), pc(0.99), t.back());
        std::printf("  over 16.7 ms: %zu frames (%.1f%%)   over 33.3: %zu (%.1f%%)\n", over,
                    100.0 * double(over) / double(t.size()), over2,
                    100.0 * double(over2) / double(t.size()));

        // ...AND THE TEN WORST, with what streaming was doing on each. A hitch
        // that is all `stream` is the world arriving; one that is none of it is
        // somewhere else entirely, and that difference is the whole point of
        // printing both.
        std::vector<const HitchFrame *> worst;
        for (const HitchFrame &h : f) worst.push_back(&h);
        std::sort(worst.begin(), worst.end(),
                  [](const HitchFrame *a, const HitchFrame *b) { return a->total > b->total; });
        std::printf("\n  the ten worst frames\n");
        std::printf("  %7s %7s %7s %7s %7s %7s %7s %7s %7s %6s\n", "total", "stream", "blas",
                    "tlas", "rering", "drain", "phys", "life", "pub", "chunk");
        for (size_t i = 0; i < 10 && i < worst.size(); ++i) {
            const HitchFrame *h = worst[i];
            std::printf("  %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %6d\n",
                        double(h->total), double(h->stream), double(h->blas), double(h->tlas),
                        double(h->rering), double(h->drain), double(h->phys), double(h->life),
                        double(h->pub), h->adopted);
        }
        // WHERE THE TIME OVER BUDGET ACTUALLY WENT, summed over the bad frames
        // only. This is the number that says what to fix: the total bill from
        // --stats counts the cheap frames too, and they are not the problem.
        double bt = 0.0, bs = 0.0, bb = 0.0, bl = 0.0, bp = 0.0, br = 0.0;
        for (const HitchFrame &h : f) {
            if (h.total <= budget) continue;
            bt += h.total;
            bs += h.stream;
            bb += h.blas;
            bl += h.tlas;
            bp += h.pool;
            br += h.rering;
        }
        if (over) {
            std::printf("\n  across the %zu frames over budget, %.0f ms total:\n", over, bt);
            double bd = 0.0;
            for (const HitchFrame &h : f)
                if (h.total > budget) bd += h.drain;
            std::printf("    streaming %.0f ms (%.0f%%)  -- of which blas %.0f, tlas %.0f, "
                        "pool %.0f, rering %.0f, drain %.0f\n",
                        bs, 100.0 * bs / bt, bb, bl, bp, br, bd);
            double bph = 0.0, bl2 = 0.0, bpb = 0.0;
            for (const HitchFrame &h : f) {
                if (h.total <= budget) continue;
                bph += h.phys;
                bl2 += h.life;
                bpb += h.pub;
            }
            std::printf("    physics     %.0f ms (%.0f%%)\n", bph, 100.0 * bph / bt);
            std::printf("    life        %.0f ms (%.0f%%)\n", bl2, 100.0 * bl2 / bt);
            std::printf("    publish     %.0f ms (%.0f%%)\n", bpb, 100.0 * bpb / bt);
            // WHAT IS LEFT IS THE RENDER DISPATCH AND ANYTHING NOT YET
            // TIMED. A big number here is not an answer, it is the next
            // place to put a clock.
            std::printf("    unaccounted %.0f ms (%.0f%%)\n", bt - bs - bph - bl2 - bpb,
                        100.0 * (bt - bs - bph - bl2 - bpb) / bt);
        }
        std::fflush(stdout);
    }

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
            "\nv2 profile -- %zu frames at %dx%d -> %dx%d, %s\n"
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
        if (opt_.sharcStats && sharc_.available()) {
            const Sharc::Stats st = sharc_.readStats();
            const double cap = double(sharc_.capacity());
            std::printf(
                "  cache     %u of %u entries live (%.1f%% occupancy)\n"
                "  cache     %u queries, %u hit (%.1f%%), %u inserts dropped (bucket full)\n",
                st.live, sharc_.capacity(), 100.0 * double(st.live) / cap,
                st.queries, st.hits,
                st.queries ? 100.0 * double(st.hits) / double(st.queries) : 0.0,
                st.insertFails);
        }
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
            "  caps lock             crouch -- and descend, in fly mode\n"
            "                        KEEP HOLDING IT to go PRONE; release and you rise\n"
            "                        back through the crouch to standing in one go\n"
            "  F                     toggle fly mode\n"
            "  arrow keys            scrub time (up/down = fast)\n"
            "  X + scroll wheel      day/night speed -- scroll down past 0.25x to REWIND\n"
            "  Y                     SETTINGS MENU\n"
            // The console had never been listed here at all, which is most of
            // why /locate needed telling about twice.
            "  K                     the stack count beside the hand -- size, place, tilt\n"
            "  T                     CONSOLE -- /locate <animal|biome|water> takes\n"
            "                        you to the nearest one, /where says where you\n"
            "                        are, /help lists them. ENTER runs, ESC cancels\n"
            "  I  or  U             ASSET EDITOR -- a deck in the sky, with the\n"
            "                        porcupine standing in the middle of it\n"
            "  R                     RECORD -- press again to stop and save\n"
            "  - / =                 exposure down / up\n"
            "  [ / ]                 bounces down / up\n"
            "  P                     screenshot            F1   this help\n"
            // The water panel had this line and no longer has a key at all --
            // it is `--water-ui` now. See onKeyEvent, where L used to be.
            "  O                     THE BUILDING -- a level in its own sky;\n"
            "                        press again to come back to the wood\n"
            "  ESC                   free the mouse -- again for the PAUSE BUTTONS,\n"
            "                        a third time to quit\n"
            "                        red quits, green returns, purple is Discord\n\n");
        // THE EDITOR'S OWN, out of the class that binds them, so this list
        // cannot go on describing a key after it has moved.
        std::printf("  ...and on the asset editor\'s deck:\n");
        int nh = 0;
        const char *const *hr = AssetEdit::help(&nh);
        for (int i = 0; i < nh; ++i) std::printf("%s\n", hr[i]);
        std::printf("\n");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // Write the live settings back out as src/core/defaults.h.
    //
    // The path comes from V2_SOURCE_DIR, baked in by the build, rather than
    // being derived from the working directory -- the launcher runs the exe
    // from C:\voxelbit, so anything relative would land in the wrong tree and
    // report success while writing nothing anyone would ever compile.
    // -----------------------------------------------------------------------
    std::string bakeDefaults() {
#ifndef V2_SOURCE_DIR
        return "bake unavailable: built without V2_SOURCE_DIR";
#else
        // V2_SOURCE_DIR is "<engine>/src", so the engine root -- and the
        // rebuild script the user is about to be told to run -- is one level up.
        // Derived rather than hardcoded so a copy of this tree elsewhere still
        // reports its own path.
        const std::string srcDir = V2_SOURCE_DIR;
        const std::string root =
            srcDir.size() > 4 ? srcDir.substr(0, srcDir.size() - 4) : srcDir;
        const std::string path = srcDir + "/core/defaults.h";
        char clockText[16];
        clock_.clock(clockText, sizeof(clockText));
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return "could not write " + path;

        std::fprintf(f,
            "// ---------------------------------------------------------------------------\n"
            "// defaults.h -- the settings v2 starts with.\n"
            "//\n"
            "// GENERATED FILE. Everything below is rewritten wholesale by \"Bake as\n"
            "// default\" in the in-viewer settings menu (Y), so hand edits survive only\n"
            "// until the next bake -- but hand edits are perfectly fine, the format is just\n"
            "// constants and the file is checked in.\n"
            "//\n"
            "// The point of it is that the settings menu and the command line stop being\n"
            "// separate universes: fly around, tune the picture until it looks right, bake,\n"
            "// rebuild, and the thing you tuned is what v2 opens with.\n"
            "// ---------------------------------------------------------------------------\n"
            "#pragma once\n"
            "\n"
            "namespace v2 {\n"
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
            "constexpr float kNightBrightness = %.2ff;\n"
            "constexpr bool kBlueNoise = %s;\n"
            "constexpr bool kAutoExposure = %s;\n"
            "constexpr float kBloom = %.2ff;\n"
            "\n"
            "// HOW LOUD THE WOOD IS, as a master gain over the ambience bed -- what\n"
            "// actually reaches the voice is this times the canopy closure at your feet.\n"
            "// 1.00 is the bed at the level it was baked; a quarter of that is a\n"
            "// background rather than a foreground, and it is where the Volume slider\n"
            "// sits at its MIDPOINT. Menu row \"Volume\", or --ambience.\n"
            "constexpr float kAmbience = %.2ff;\n"
            "\n"
            "// HOW MUCH THE THING IN YOUR HAND MOVES as you walk -- a gain over the\n"
            "// stride and the breath in render/helditem.h, not a speed and not a shape.\n"
            "// 1.00 is the look those constants describe, so 2.00 is twice it; 0 nails\n"
            "// the tool to its pose for a reference screenshot. No menu row -- this one\n"
            "// is --hand-sway and a bake, as kBloom and kAutoExposure are.\n"
            "constexpr float kHandSway = %.2ff;\n"
            "\n"
            "}  // namespace defaults\n"
            "}  // namespace v2\n",
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
            clock_.cycleSpeed, atmo_.enabled ? "true" : "false", nightLevel_,
            // BAKED FROM THE LIVE OBJECTS, not from opt_. The menu writes
            // straight to tracer_ and post(), so opt_ still holds whatever the
            // command line said at start-up -- baking that would quietly
            // discard the thing just tuned, which is the one job this has.
            tracer_.blueNoise ? "true" : "false",
            tracer_.post().autoExposure ? "true" : "false", tracer_.post().bloom,
            // THE LIVE GAIN WHERE THERE IS ONE, and the option otherwise. A bake
            // under --no-sound or --background never opened the bed, so masterGain()
            // is the 1.0 the object was constructed with rather than anything anybody
            // chose -- and baking that would turn the wood up fourfold for having
            // tuned the picture with the sound off.
            ambience_.active() ? ambience_.masterGain() : opt_.ambience,
            // Straight off the live object, like the two above it: the menu
            // row writes into held_ and never into opt_, so opt_ still holds
            // whatever the command line said at start-up.
            held_.sway);
        std::fclose(f);
        // The FULL PATH, not just the file name. "run rebuild.bat" is only
        // useful if you already know which of the engine trees it lives in,
        // and the exe is launched from the repo root rather than from beside
        // its own source -- so the obvious place to look is the wrong one.
        return "baked. now run " + root + "/rebuild.bat";
#endif
    }
};

}  // namespace v2
