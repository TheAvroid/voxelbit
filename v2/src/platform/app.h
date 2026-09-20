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
// ...AND DXGI, FOR THE ONE THING FALCOR'S Device::Info DOES NOT CARRY: how
// much video memory this card has. See the gpu line in the boot report.
#pragma comment(lib, "dxgi.lib")

#include "core/defaults.h"
#include "render/neural.h"
#include "render/streamline.h"
#include "render/nrc.h"
#include "render/nrcsdk.h"
#include "render/atmosphere.h"
#include "render/volfog.h"
#include "render/cuda.h"
#include "render/clusters.h"
#include "player/physics.h"
#include "render/tracer.h"
#include "world/poi.h"
#include "world/world.h"
#include "platform/audio.h"
#include "render/camera.h"
#include "player/arrows.h"
#include "player/bullets.h"
#include "ai/bees.h"
#include "ai/critters.h"
#include "render/particles.h"
#include "ai/lifehit.h"
#include "ai/birds.h"
#include "ai/butterflies.h"
#include "player/drops.h"
#include "player/helditem.h"
#include "ui/holotext.h"
#include "ai/birdflock.h"
#include "ai/bunnies.h"
#include "ui/assetedit.h"
#include "ai/lake.h"
#include "player/toolsound.h"
#include "player/vitals.h"
#include "player/player.h"
#include "platform/recorder.h"
#include "world/daynight.h"
#include "world/palplate.h"

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
    // 0x1DF -- EVERYTHING EXCEPT kWFSunPath (user 2026-09-18: "turn off sunlight
    // to bed on default"). That term is the other half of Beer-Lambert: it
    // darkens the BED's albedo by the sun's own slant path down through the
    // water, so a deep lake stops wearing its shallows' brightness. Off, the
    // sun reaches the bed at full strength and the water reads clearer. See
    // kWF* in Shared.slang; [L] toggles it back.
    // 0x3DF -- the ten water terms, less kWFSunPath. Bit 9 (kWFSunGlare) is
    // ON by default because it is what a lake actually does; it has a switch
    // now because it is also the brightest thing in the frame and it used to be
    // impossible to turn off without losing the sparkle with it.
    uint32_t waterFlags = 0x3DFu;
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
    // PINE WAS THE WHOLE WORLD AND THE BIRCH IS BACK (user 2026-09-18: "also
    // fix the birch forest. its not in the world").
    //
    // This was TRUE from 2026-09-17 ("pine is the whole world now"), which set
    // `terrain.forced` in the ordinary game -- so `woodMix` returned pure pine
    // at every x and the birch and oak landforms, species tables and life gates
    // were never reached. Nothing was deleted and nothing had to be restored:
    // false is the three bands, exactly as they were.
    //
    // AND IT IS WHY /locate birch COULD ONLY REFUSE. With the world pinned
    // there was no birch anywhere to travel to, and the reply told the player
    // to "restart without it" -- a flag they had never typed, because this
    // default was doing the pinning. See the biome branch in app_console.inl.
    //
    // `--pine` still pins it, which is what a reproducible capture wants.
    bool pineOnly = false;
    // --oak: pin the world to the oak wood, as --pine and --birch do for
    // theirs. Worth more here than for the other two: the band tiling
    // moved when the oak was inserted, so a coordinate is no longer a
    // reliable way to name a wood and this is.
    bool oakOnly = false;
    // --cherry: pin the world to the cherry wood, as --oak does for the oak.
    // It is the OAK's ground with pink crowns and its own two species, so this
    // is the flag for looking at blossom without walking 2.8 km for it.
    bool cherryOnly = false;
    // --desert: pin the world to the open sand. Its own landform, its own
    // floor, no water and no forest life -- see Biome::Desert.
    bool desertOnly = false;
    // --coords: open with the x/y/z readout already up. The same flag [F3]
    // toggles and the settings checkbox drives -- see showCoords_.
    bool coords = false;
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

    // 15, not 12: measured 2026-09-18, 384 m instead of 307 m at the SAME
    // ~68 fps and 7.6 GB of a 12 GB card. 18 is where the card fills and
    // the driver starts spilling to host memory, so this is the last rung
    // that is free rather than the last one that runs.
    int view = 15;
    // HALVED (user 2026-09-18: "can you reduce the pine tree density by 50%").
    // 0.3210 -> 0.1605. This is the knob app_load pushes into World::treeDensity
    // and the one the scatter multiplies by stemFill, so halving it halves the
    // offered sites everywhere the stand table is not already saturating.
    float treeDensity = 0.1605f;
    // THE OAK'S SHARE, AND IT IS NOT treeDensity. Halving the number is not
    // halving the wood: this is the share of lattice cells that OFFER a
    // candidate, and the spacing rejection culls from that, so the headcount
    // moves by LESS than the knob does -- count ~ density^0.93 on the current
    // models. 0.0975 is the value that actually halves the wood, measured;
    // the ladder is over ChunkMesher::oakDensity.
    float oakDensity = 0.0975f;

    // -- A MEASURED 10 cm PATCH, LAID OVER THE DEM -------------------------
    // --inset <file.vbins> --inset-at <x> <z>. Built by tools/pc2vox.exe from
    // a point cloud; see src/world/inset.h for why it is a height and not a
    // volume. Empty means the DEM alone, which is the default.
    std::string insetPath;
    float insetX = 0.0f, insetZ = 0.0f;
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
    // A .vbdem written by tools/dem2raw.cpp. Empty means the invented
    // landform; set it and the ground becomes measured elevation.
    std::string demPath = "C:/voxelbit/v2/assets/dem/rmnp50.vbdem";
    float demBaseM = 20.0f;
    // HOW MANY REAL METRES IN ONE WORLD METRE. Colorado at true scale puts
    // only ~29 DEM postings across the 300 m view disc, so a 10 km mountain
    // arrives as one smooth ramp with no shape in it. Six packs 175 postings
    // into the same disc. demExag puts vertical back after the shrink.
    // ------------------------------------ SIX REAL METRES TO THE WORLD METRE
    // Taken to 1.0 on 2026-09-18 and REVERTED the same day at the user's word
    // ("revert the pine forest scale changes"). True scale worked -- it built
    // and rendered at 55-120 Mpaths/s, and nothing structural objected, because
    // chunks are indexed in X and Z only and the GRADE is identical either way.
    // What it costs is the view: the chunk ring is a 300 m disc, so at 1:1 a
    // mountain is only visible once you are standing on it, and the 50 km
    // window is mostly somewhere you will never walk.
    //
    // `--dem-scale 1` still does all of it, and everything it needed is still
    // here: stemDiv derives from this, the cover's jitter is capped in world
    // metres (CoverField::raw) and the timberline fade converts on load.
    float demScale = 6.0f;
    float demExag = 1.0f;
    // 0: THE MEASURED GROUND AND NOTHING ADDED TO IT (user 2026-09-18: "we're
    // looking for smooth terrain without noise"). This scales BOTH noise
    // octaves on the DEM path -- see VoxelTerrain::heightM -- so --dem-detail
    // 0.45 restores the old roughness exactly and anything between fades it in.
    float demDetail = 0.0f;   // sub-metre roughness over the measured ground
    // --dem-rough: the SAME argument answered per cover class instead of
    // globally. 0 is off and is the default; 1 is the table in
    // VoxelTerrain::roughFor. It scales itself down as the source posting
    // improves, so a 1 m .vbdem invents 18% of what a 10.29 m one does.
    float demRough = 0.0f;
    // Force chooseSpawn() to run even on the --out path, which normally skips
    // it. Without this the only spawn reachable headlessly is the pinned one,
    // so the code the game actually opens with cannot be tested at all.
    bool spawnPick = false;
    bool yawGiven = false;   // --yaw was passed; do not aim the spawn
    // Wave amplitude as a gain on the shader's kWave table. 0 = flat water,
    // which is what v1 ships and what a mapped lake wants.
    // ------------------------------------------------- THE WAVES ARE ON
    // (user 2026-09-18: "you turned off the waves. turn them back on.")
    //
    // This was 0 because the Gerstner field WAS the "water mounds" reported
    // twice -- the four components sum to 0.256 m of displacement over a 5.2 m
    // longest wavelength, which is a sea state, and on a still alpine lake it
    // reads as rolling humps rather than as water moving.
    //
    // ZEROING IT WAS THE WRONG SHAPE OF FIX. "No waves" and "the wrong waves"
    // are not the only two options, and the amplitude was never the thing
    // anyone asked to lose. 0.45 is a bit under half the authored sea state:
    // 0.115 m of displacement, which is chop you can watch without a hump you
    // can stand on. 1.0 is the full table and it is one slider away on [L].
    // 1.000 -- THE FULL AUTHORED TABLE (user 2026-09-18: "make the default wave
    // height 1.000"). 0.256 m of displacement over a 5.2 m longest wavelength.
    // 0.45 was a hedge against the "water mounds" this term caused when it had
    // no slider; it has one on [L] now, so the default can be the real thing.
    float waves = 1.0f;
    // Real hectares per world hectare for stand density. 0 = shrink^2 (36 at
    // shrink 6, one world tree per real tree). 6 gives a fuller wood.
    float stemDiv = 0.0f;
    // Aerial-imagery land cover on the same grid as the DEM. Decides where
    // trees stand and where the ground is bare, from a photograph rather
    // than from a threshold. Empty or missing = the old behaviour.
    std::string coverPath = "C:/voxelbit/v2/assets/dem/rmnp50.vbcov";
    // WHETHER THE IMAGERY IS BELIEVED ABOUT THE GROUND as well as about the
    // water. See VoxelTerrain::coverGround: --acadia turns it off because the
    // Colorado classifier reads a Maine forest as bare rock, and --cover-water
    // is the flag for any other window it gets wrong.
    bool coverGround = true;
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
    bool biteTest = false;
    bool foodTest = false;   // --food-test: pick an apple and eat it
    bool floatAudit = false;   // --float-audit: what the level leaves unsupported
    // --float-sweep: the WHOLE world's floating geometry, with nothing excused.
    // See runFloatSweep for why --float-test was not enough.
    bool floatSweep = false;
    // --rip-test: shoot the level's walls headlessly and measure the SHAPE of
    // whatever the hanger sweep cuts loose. See runRipTest.
    bool ripTest = false;
    // --pole-test: cut every free-standing post in the level and check it
    // comes down. The companion to --rip-test; see runPoleTest for why
    // neither may be the only one run.
    bool poleTest = false;
    int refreshFrame = -1;   // --refresh-frame N: press G on frame N
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
    // THE TWO FRUIT, AND THEY ARE THE FILES THE TREES USE -- see the kit note
    // for why one model serves both the crown and the hand.
    std::string apple = "C:/voxelbit/game/assets/decoration/fruit_apple.vox";
    std::string orange = "C:/voxelbit/game/assets/decoration/fruit_orange.vox";
    // -- THE ASSAULT RIFLE, WHICH IS THE LEVEL'S AND NOT THE WOOD'S --------
    //
    // (user 2026-09-17: "spawn the player in with an assault rifle when
    // spawning into the o. dont let the player have the gun in the regular
    // sandbox yet.")
    //
    // A path like every other held model, and the FIRST FRAME of the fire
    // strip rather than a base.vox -- this gun has no base.vox. The numbered
    // frames beside it are v1's muzzle flash and its reload, which v2 has
    // nothing to do with yet; 00 is the gun at rest and that is all this needs.
    //
    // IT IS STOWED AT START-UP AND GIVEN ONLY BY [O]. See the kit block below,
    // and App::standInLevel / leaveLevel for the door that hands it over.
    // Loading it here rather than on the keypress is not an optimisation -- it
    // is the rule the pause room and the stone tools were both broken by:
    // colours are served first-come at start-up, and a model that waits for a
    // keypress is asking last, when the table is full. See
    // HeldItem::prewarmColors.
    std::string rifle = "C:/voxelbit/game/assets/guns/assault_rifle/fire/00.vox";
    // -- ...AND THE NINE FRAMES OF ITS RELOAD ------------------------------
    //
    // (user 2026-09-18: "there are animations for the reload cycle. look in
    // the assault file.")
    //
    // A DIRECTORY, NOT A LIST OF FILES. HeldItem::addGun takes every .vox in
    // it in numeric order, so re-timing the cycle is a matter of adding or
    // removing frames on disk -- see stripFiles for why a list in the source
    // would be a frame that silently never plays.
    //
    // NOT DERIVED FROM `rifle` ABOVE, though it sits beside it. A path built
    // by string surgery on another path is a path that breaks the day somebody
    // points --rifle at a gun whose rest pose is called base.vox, which is what
    // every OTHER gun in the folder has.
    std::string rifleReload = "C:/voxelbit/game/assets/guns/assault_rifle/reload";
    // -- THE PISTOL, WHICH IS THE LEVEL'S SECOND GUN -----------------------
    //
    // (user 2026-09-18: "import the pistol asset into the fps mode. put it in
    // the inventory, when the player scrolls up it selects it.")
    //
    // SAME SHAPE AS THE RIFLE ABOVE: a rest pose and a reload strip beside it,
    // loaded through HeldItem::addGun. This one's rest pose lives under
    // `shoot/` rather than `fire/` and there is no base.vox here either -- the
    // folder is v1's sheet, cut up, and every gun in it is numbered frames and
    // nothing else.
    //
    // 22 VOXELS IN 13 SHADES over the whole strip, three of which the rifle
    // already owns. See the palette note beside the prewarm list: the ten it
    // mints are the reason that list has to know about the reload frames too --
    // four of them carry two shades the rest pose does not.
    std::string pistol = "C:/voxelbit/game/assets/guns/pistol/shoot/00.vox";
    std::string pistolReload = "C:/voxelbit/game/assets/guns/pistol/reload";
    // -- ...AND THE LAMP YOU PLACE THEM WITH -------------------------------
    //
    // (user 2026-09-17: "put a bulb in my hand when I scroll up from the
    // assault rifle".) The pause room's own art, which is also what gets
    // stamped into the map -- so what is in your hand is the thing you are
    // putting on the ceiling and not a picture of it.
    std::string bulb = "C:/voxelbit/source/wip/technology/lightbulb.vox";
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
    // ...AND A SCRIPTED ROUND, which is the only way to see the rifle's
    // chip without a hand on the mouse. --level --fire-frame N pulls the
    // trigger on frame N through fireRifle() itself -- not a copy of it --
    // and reports what came out of the wall six frames later, by which
    // time a 120 m/s round has certainly landed.
    int fireFrame = -1;
    // ...AND A SCRIPTED RELOAD, for the same reason as --fire-frame and on the
    // same clock. Nine drawn frames that only ever play while a magazine is
    // empty cannot be looked at, measured or regression tested by hand: the
    // whole cycle is 1800 ms and it starts on a keypress nobody can time.
    // --level --reload-frame 40 --shot-frame 70 photographs the middle of it,
    // and the pair of them is the only way to see that the gun does not jump
    // when the box grows two rows to hold the magazine. See kReloadMs.
    int reloadFrame = -1;
    // TURNS OF THE WHEEL AFTER ARRIVING IN THE LEVEL, so what one scroll up
    // puts in the hand can be photographed. See the --level dispatch.
    int scroll = 0;
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

    // SPAWN IN THE TREES, NOT ON THE SUMMIT. World (0,0) is the middle of
    // the DEM window, and that window is centred on Mount Elbert -- so the
    // old default put the player on the bare rock of a 14er, 890 m above
    // the timberline, and the wood reported "0 trees, 710 rocks". This is
    // a forested bench 534 m BELOW the treeline at a 20% grade, with the
    // ground still rising 256 m within the view disc so there is a
    // mountain to look at. --cam-x/--cam-z still override.
    // ON A LAKE SHORE IN ROCKY MOUNTAIN NATIONAL PARK, 2,554 m. REAL metres
    // from the window centre (app_load.inl divides by the shrink).
    //
    // Found by scoring every water body in the window on size AND on how much
    // the ground rises within one view disc of it: this one is 18,240 water
    // samples with 409 m of rise inside 1.8 km, which is water in front and
    // mountain behind. The park's biggest lake scored worse -- 91,886 samples
    // but only 81 m of relief, which is a pond on a flat.
    float camX = -12173.0f, camZ = 8026.0f;
    // Whether the two above were ASKED for. A named camera is a named camera:
    // it pins the offline render, it pins a scripted capture, and it turns the
    // random spawn off. Without this the flags would be silently overwritten by
    // a spawn the user did not ask for.
    bool camGiven = false;
    // -- ...AND WHETHER A *FLAG* NAMED THE PLACE, WHICH IS NOT THE SAME THING -
    //
    // (user 2026-09-19: "have the player spawn at different locations that have
    // water. pick a biome at random.")
    //
    // chooseSpawn roams the whole window now, so the pair above is only a
    // STARTING POINT -- and five flags set it without setting camGiven, which
    // splits into two kinds:
    //
    //   * --lake, --peak and --front NAME A FEATURE. "Take me to the lake" has
    //     one right answer and roaming would quietly turn it into "somewhere
    //     else entirely" while still printing a happy spawn line -- the same
    //     class of silent wrongness the picker itself just had. These set it.
    //
    //   * --acadia and --ouachita pick a WORLD, and their coordinate is a hint
    //     inside it. Both of their own notes already say the picker is meant
    //     to wander off it, and --acadia's was scored for RELIEF rather than
    //     water, which is why it spawned 400 m from any. These do not.
    //
    // So: camGiven is "do not pick at all", camPlace is "pick, but near here".
    bool camPlace = false;
    // 0 means "somewhere new", which is the default for the viewer. Any other
    // value returns to the same place -- the seed is printed on every launch so
    // a spot worth finding again can be. It chooses the BIOME and the shore as
    // well now, so one number still reproduces the whole spawn.
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

    #include "platform/app_load.inl"
    #include "platform/app_frame.inl"