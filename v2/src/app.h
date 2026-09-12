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
#include "render/birds.h"
#include "render/butterflies.h"
#include "render/drops.h"
#include "render/helditem.h"
#include "render/toolsound.h"
#include "render/player.h"
#include "render/recorder.h"
#include "scene/daynight.h"

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
    int butterflies = 24;

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
    // Open straight into the asset editor -- what U does, without the key.
    bool stageAtStart = false;
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
        world_.terrain.forced = opt_.birch || opt_.pineOnly;
        world_.terrain.biome = opt_.birch ? Biome::Birch : Biome::Pine;
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
                    world_.terrain.birchAt(opt_.camX) ? "birch" : "pine", opt_.camX,
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
        // THE EDITOR, FROM THE COMMAND LINE. The same path U takes, so a shot
        // of the stage is a shot of the thing the key opens rather than of a
        // second arrangement that could drift from it.
        if (opt_.stageAtStart) {
            world_.setStage(true);
            const Vec3 c = World::stageCentre();
            // THREE METRES ON THE +Z SIDE, because yaw 0 looks down -Z in this
                // engine (Camera::direction). Standing on the -Z side and facing
                // that way put the subject squarely behind the camera.
                player_.pos = Vec3(c.x, c.y + 0.2f, c.z + 3.0f);
            player_.fly = true;
            player_.vy = 0.0f;
            yaw_ = 0.0f;
            pitch_ = -8.0f;
            pos_ = player_.eyePosition();
            birds_.stageOne(Vec3(c.x, c.y, c.z));
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
                      HeldPose{8.512f, -0.910f, 8.730f, 0.040f, -1.420f, 0.009f, 1.003f},
                      Takes::Soil);
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
            held_.addEmpty("empty hand");
            held_.select(opt_.tool);
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
        if (!spawnSettled_ && world_.chunkAt(player_.pos)) {
            spawnSettled_ = true;
            nudgeOutOfSolids();
            player_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
            pos_ = player_.eyePosition();
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
            const int back = drops_.update(dt, walkWorld(), player_.pos, player_.eyePosition());
            // THE SOUND GOES WITH THE SNATCH, so it lands on the frame the item
            // leaves the ground rather than 360 ms later when the flight
            // arrives -- see ToolSounds::pickedUp for that engine's own note
            // about having got this the wrong way round first.
            if (drops_.snatchedNow()) toolSfx_.pickedUp();
            if (back >= 0) {
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
        arrows_.update(dt, walkWorld());
        // ...and each one that stopped this tick lands with a thud, quieter the
        // further off it stuck. Drained here rather than inside the flight so
        // the sound is not fired from the 5 ms integration substep loop.
        for (const Vec3 &at : arrows_.landedThisTick())
            toolSfx_.arrowLanded(length(at - pos_));

        // ...and the flock. It gathers its OWN colliders rather than taking the
        // six metres around the player that walkWorld carries: a butterfly is
        // up to eighty metres away and the trunks it has to miss are the ones
        // around IT. See Butterflies::decide for the clock that keeps that
        // affordable.
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
        if (birds_.ready() && (frameTick_ % 30) == 0)
            world_.collidersNear(player_.pos, kBirdKeepM, &perches_);
        birds_.update(dt, perches_, player_.pos);

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
            world_.setHeldInstance(held_.model(), hx.m, hx.tx, hx.ty, hx.tz, hx.show);
            // ...and the tracer is told the same pose, because it is the one
            // thing in this scene whose motion vector cannot be worked out from
            // where it is in the world -- see V6Params::heldPrev0. It keeps its
            // own previous copy and steps it with prevCam_, which is the only
            // way the two can be guaranteed to describe the same frame.
            // ...AND WHICH TOOL IT IS, so a change of hands does not carry the
            // last one's motion vector onto this one. See heldPrevValid.
            tracer_.setHeldXform(hx.m, hx.tx, hx.ty, hx.tz, hx.show, held_.selected());
            arrows_.publish(world_);
            drops_.publish(world_);
            flock_.publish(world_);
            birds_.publish(world_);
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

        if (e.key == Input::Key::X) return true;  // held modifier for the wheel
        if (e.key == Input::Key::F) {
            player_.fly = !player_.fly;
            if (!player_.fly) player_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v2: %s\n", player_.fly ? "flying" : "walking");
            std::fflush(stdout);
            quitArmed_ = false;
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
        if (e.key == Input::Key::U && !consoleOpen_) {
            const bool on = !world_.staged();
            if (on) {
                woodPos_ = player_.pos;
                woodYaw_ = yaw_;
                woodPitch_ = pitch_;
                woodFly_ = player_.fly;
            }
            world_.setStage(on);
            if (on) {
                const Vec3 c = World::stageCentre();
                // Back from the middle and looking at it, so the cardinal is in
                // front of you the moment you arrive rather than underfoot.
                // THREE METRES ON THE +Z SIDE, because yaw 0 looks down -Z in this
                // engine (Camera::direction). Standing on the -Z side and facing
                // that way put the subject squarely behind the camera.
                player_.pos = Vec3(c.x, c.y + 0.2f, c.z + 3.0f);
                player_.fly = true;
                yaw_ = 0.0f;
                pitch_ = -8.0f;
                // ...and the subject, standing on the deck in front of you.
                birds_.stageOne(Vec3(c.x, c.y, c.z));
            } else {
                player_.pos = woodPos_;
                player_.fly = woodFly_;
                yaw_ = woodYaw_;
                pitch_ = woodPitch_;
            }
            player_.vy = 0.0f;  // no fall carried across the doorway
            pos_ = player_.eyePosition();
            tracer_.resetAccumulation();
            std::printf("v2: %s\n", on ? "asset editor" : "back to the wood");
            std::fflush(stdout);
            quitArmed_ = false;
            return true;
        }
        if (e.key == Input::Key::Y) {
            setMenuOpen(!menuOpen_);
            quitArmed_ = false;
            return true;
        }
        // T OPENS THE CONSOLE, and only when it is shut -- while it is open the
        // key belongs to whatever is being typed, and ImGui has the keyboard.
        if (e.key == Input::Key::T && !consoleOpen_ && !menuOpen_) {
            setConsoleOpen(true);
            quitArmed_ = false;
            return true;
        }
        if (e.key == Input::Key::Escape) {
            // ESC closes the menu before it starts arming the quit -- otherwise
            // dismissing a panel would leave the window one press from closing.
            if (consoleOpen_) {
                setConsoleOpen(false);
                return true;
            }
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
                std::printf("v2: press ESC again to quit\n");
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

        if (menuOpen_) return false;  // the mouse belongs to the menu while it is up

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
    bool swingArmed_ = false;
    // The last pose the menu's copy row printed, kept so the row can show it
    // back rather than the player having to find the console.
    std::string poseCopied_;
    Swing lastSwing_;
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
    std::vector<Solid> solids_;  // decor near the player, regathered each tick

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
    bool quitArmed_ = false;
    bool moving_ = false;
    bool menuOpen_ = false;
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

    // Where the wood was when U was pressed -- see the handler.
    Vec3 woodPos_{0, 0, 0};
    float woodYaw_ = 0.0f, woodPitch_ = 0.0f;
    bool woodFly_ = false;
    std::vector<Solid> perches_;  // trees a songbird may sit in -- see the note at its update
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
        if (physics_.available()) {
            // REBUILT WHEN THE GROUND MOVES, not only when the player does.
            // A dig changes the shape of the floor under everything that is
            // falling, and the patch is the only copy of it the solver has.
            if (world_.takeGroundDirty() ||
                !physics_.groundCovers(player_.pos.x, player_.pos.z, VOXEL_M, kGroundMarginM))
                rebuildGroundPatch();
            physics_.step(dt);
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
                    return float(world_.terrain.heightVox(int(std::floor(x / VOXEL_M)),
                                                          int(std::floor(z / VOXEL_M))) +
                                 1) *
                           VOXEL_M;
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
        world_.groundPatch(groundPatch_.data(), n, i0, j0, step);
        world_.takeGroundDirty();
        physics_.setGroundPatch(groundPatch_.data(), n, i0, j0, VOXEL_M, step);
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

    WalkWorld walkWorld() {
        world_.collidersNear(player_.pos, 6.0f, &solids_);
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
        };
        return t;
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
    bool nearestWater(float *outX, float *outZ) const {
        const VoxelTerrain &t = world_.terrain;
        TerrainMemo memo;
        auto wetAt = [&](float x, float z) {
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

    float nearestBandX(Biome b) const {
        const float period = 2.0f * VoxelTerrain::kBandW;
        const float c = VoxelTerrain::bandCentre(b);
        const float k = floorf((pos_.x - c) / period + 0.5f);
        return c + k * period;
    }

    void teleportTo(float x, float z) {
        player_.placeOnGround(walkWorld(), x, z);
        pos_ = player_.eyePosition();
        // EVERYTHING TEMPORAL HAS TO BE TOLD. The film, the fog's history and
        // the reconstruction all carry state about somewhere else entirely, and
        // blending out of it drags the old wood across the new one for a
        // second. The streamer re-rings itself from the new position on its own.
        moving_ = true;
        tracer_.resetAccumulation();
        volfog_.invalidate();
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
            if (arg.empty()) {
                std::string m = "locate what? try: water";
                for (size_t i = 0; i < biomeNames().size(); ++i)
                    m += ", " + std::string(biomeNames()[i].name);
                return m;
            }
            // WATER IS NOT A BIOME, so it is not a row in that table -- it is
            // a feature of the landform inside one. Handled before the band
            // loop, and it works in a pinned world too, unlike the bands.
            if (arg == "water" || arg == "lake") {
                float wx = 0.0f, wz = 0.0f;
                if (!nearestWater(&wx, &wz))
                    return std::string("no water within 6 km -- lakes sit in basins, "
                                       "and the birch wood has none at all");
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
                    return std::string("the world is pinned to one wood (--birch / --pine) -- "
                                       "restart without it to walk between them");
                }
                const float tx = nearestBandX(bn.biome);
                teleportTo(tx, pos_.z);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s forest -- %.0f, %.0f", bn.name, tx, pos_.z);
                return std::string(buf);
            }
            std::string m = "nothing called '" + arg + "'. try: water";
            for (size_t i = 0; i < biomeNames().size(); ++i)
                m += ", " + std::string(biomeNames()[i].name);
            return m;
        }
        if (verb == "where") {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%.0f, %.0f, %.0f -- the %s wood", pos_.x, pos_.y,
                          pos_.z,
                          world_.terrain.birchAt(pos_.x) ? "birch" : "pine");
            return std::string(buf);
        }
        if (verb == "help")
            return std::string(
                "/locate <biome|water>   /where   ENTER runs and closes   ESC cancels");
        return std::string("unknown command '" + verb + "' -- try /help");
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
        RECT rc{};
        POINT p{};
        if (::GetClientRect(hwnd, &rc) && ::GetCursorPos(&p)) {
            ::ScreenToClient(hwnd, &p);
            const int cx = (rc.right - rc.left) / 2, cy = (rc.bottom - rc.top) / 2;
            rx = float(p.x - cx);
            ry = float(cy - p.y);
            if (rx != 0.0f || ry != 0.0f) centreCursor();
        }
        if (rx == 0.0f && ry == 0.0f) return false;

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
                // ...and what it sounded like. The tool declares what it can
                // take (Takes, in render/helditem.h) and the blow decides the
                // rest, exactly as toolTakesFor and playToolHit split the job
                // in the engine this comes from.
                const Blow heard = toolSfx_.blow(held_.takes(), lastSwing_);
                felled_ = false;   // per blow, not per fell -- see the swing log
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
                if (lastSwing_.kind != Swing::Rock && lastSwing_.kind != Swing::Trunk) {
                    const Takes tk = held_.takes();
                    const bool wantStone =
                        tk == Takes::Stone &&
                        !(lastSwing_.kind == Swing::Ground && isStoneMat(lastSwing_.material));
                    const bool wantWood = tk == Takes::Wood;
                    if (wantStone || wantWood) {
                        const Swing ms = swingRayModels(walkWorld(), player_.eyePosition(),
                                                        forward());
                        if (ms.hit && ((wantStone && ms.kind == Swing::Rock) ||
                                       (wantWood && ms.kind == Swing::Trunk)))
                            lastSwing_ = ms;
                    }
                }
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
                        if (lastSwing_.kind == Swing::Rock || lastSwing_.kind == Swing::Trunk) {
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
                        else if (dug && physics_.available())
                            world_.dropUndermined(physics_, lastSwing_.point, simMs_);

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
                            world_.spawnDebris(physics_, spoilVol_, spoilN_, at, kNoVel, kNoSpin,
                                               simMs_, spoilYaw_, srcRock);
                        }
                    }
                }

                if (opt_.swingLog) {
                    static const char *kWhat[] = {"air", "ground", "trunk", "rock"};
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
                for (int b = 0; b < 24; ++b) {
                    const float ang = float(b) * 0.7853982f;
                    const Vec3 eye{so2.cx + std::cos(ang) * (so2.hx + 3.0f),
                                   so2.baseY + 0.6f + float(b % 5) * 0.5f,
                                   so2.cz + std::sin(ang) * (so2.hz + 3.0f)};
                    const Vec3 dir{-std::cos(ang), 0.0f, -std::sin(ang)};
                    if (!world_.carveModel(so2, eye, dir, 12.0f, kDigRadiusVox)) continue;
                    ++modelBlows;
                    // THE WHOLE SWING PATH, not half of it. carveModel alone
                    // leaves a severed limb sitting in the model, because it is
                    // fellTree that hands a piece that big to the solver -- and
                    // a test that skips it measures 31,450 voxels of its own
                    // omission, which is what the first run of this did.
                    if (world_.fellTree(physics_, so2, dir, simMs_)) break;
                    const long l = world_.looseVoxelsNow(so2);
                    if (l > modelLeft) modelLeft = l;
                }
                break;   // one of each kind is enough; the flood is the slow part
            }
        }
        std::printf("  %ld blows on a tree and a rock: worst %ld voxels left standing"
                    " on nothing\n",
                    modelBlows, modelLeft);

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
        std::printf("  %6s %9s %9s %9s %9s %9s %9s\n", "ms", "x", "y", "z", "pitch", "fall m/s",
                    "spin r/s");

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
                std::printf("  %6.0f %9.2f %9.2f %9.2f %8.1fd %9.2f %9.2f\n",
                            double(f) * dt * 1000.0, p.x, p.y, p.z, pitch, -lin.y,
                            sqrtf(ang.x * ang.x + ang.y * ang.y + ang.z * ang.z));
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
        std::printf("\n  (pitch 0 = still standing, 90 = flat on the ground)\n");
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
            for (int i = 0; i < 60; ++i) flock_.update(1.0f / 60.0f, world_, cam.origin);
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
            world_.refitTlas();
            Vec3 at{0, 0, 0};
            float d = 0.0f, lo = 0.0f, hi = 0.0f;
            flock_.band(&lo, &hi);
            if (flock_.nearest(cam.origin, &at, &d))
                std::printf("  flock    %d butterflies, %.1f to %.1f m up, nearest %.1f m at "
                            "(%.1f, %.1f, %.1f)\n",
                            flock_.flying(), double(lo), double(hi), double(d), double(at.x),
                            double(at.y), double(at.z));
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
