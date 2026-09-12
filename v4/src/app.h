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
#include "gpu/waveworks.h"
#include "gpu/atmosphere.h"
#include "gpu/volfog.h"
#include "gpu/cuda.h"
#include "gpu/tracer.h"
#include "gpu/world.h"
#include "render/audio.h"
#include "render/camera.h"
#include "render/player.h"
#include "render/recorder.h"
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
    // and need a 50-series.
    //
    // THEY ARE NOT CLAMPED ON ADA, THEY ARE REFUSED. This used to say they were
    // clamped to 2x anyway; measured on the RTX 4070, `--fg 4x` comes back
    // "DLSSGSetOptions failed (38)" and frame generation is then OFF -- not
    // degraded, off, with opt_.frameGen still reading On4x. That is why the
    // settings row shows Streamline::frameGeneration() rather than this field,
    // and why it only offers what maxGeneratedFrames() says the card can do.
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

    // NO GENERATOR CONFIGURATION. What stood here -- a view radius, tree, grass,
    // flower and rock densities, and the asset directories to scatter them from
    // -- configured the thing that decided what the world CONTAINED. There is no
    // world and no generator; a backend that authors content brings its own.

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
    // ---- WHAT THE STORE BUILDS (scene/generate.h) -----------------------
    //
    // THESE BELONG TO THE BACKEND, NOT TO THE RENDERER, and they are gathered
    // here only because the command line is. Every one of them is copied
    // straight onto world_.store.gen in onLoad and read nowhere else -- so a
    // different store is a different block of these and no other edit, which is
    // the same rule the shader seam follows.
    //
    // --demo authors the acceptance scene instead of a landscape: a floor, a
    // stepped wall across a chunk seam, an ARCH and a pillar with a cave
    // through it. The arch is the test -- nothing that stores one height per
    // column can hold it.
    bool demo = false;
    // ---- THE ASSETS THE WOOD IS MADE OF ---------------------------------
    //
    // The same folders every engine in this project draws from, so a seed means
    // the same wood in all of them. Empty, or pointing at nothing, falls back
    // to a generated conifer and a noise-dented boulder rather than to a black
    // frame -- the startup line says which you got. --no-models forces the
    // fallback, which is the honest A/B of authored content against generated.
    std::string pineDir;
    std::string decorDir;
    // mineral.vox, which is not in the decoration folder with the rest.
    std::string mineralPath;
    bool minerals = true;
    bool models = true;
    bool grass = true;
    // The MEAN fraction of columns carrying a strand, and how wide a meadow or
    // a clearing is. The density is a mean over the patch field and not a flat
    // probability -- see the grass section of scene/generate.h.
    float grassDensity = 0.307f;
    float grassPatchM = 24.0f;

    // The square of world that gets voxels, centred on the origin. Everything
    // outside it is sky; a ray that leaves it misses.
    float worldM = 192.0f;
    // How deep the ground is stored under its surface. It is a SHELL -- see the
    // note at the top of scene/generate.h for why, and what it costs.
    // A CAP on the whole column, in metres. Zero lays the full layer stack --
    // surface, 15 voxels of soil, 500 of stone, 10 of bedrock, 52.6 m in all.
    //
    // IT COSTS ALMOST NOTHING NOW. The generator writes only the skin of the
    // terrain and hands the sealed interior to a formula, so a fifty-metre
    // column and a two-metre one differ by a few megabytes of host memory
    // rather than by two gigabytes. See ImplicitColumns in scene/bricks.h.
    float skinM = 0.0f;
    int soilVox = 15;
    int stoneVox = 500;
    int bedrockVox = 10;
    // How far the hills swing about the mean. DOUBLED from 11 -- and it has to
    // match scene/generate.h's own default, because this block is copied over
    // it wholesale at build() and whichever is wrong here wins.
    float reliefM = 22.0f;
    float treeCellM = 11.0f; // the scatter grid a pine is planted on
    // Water. The line defaults to a PERCENTILE of this world's own heights --
    // see the note in scene/generate.h about why a number carried over from
    // another engine has been wrong here twice.
    bool water = true;
    float waterPct = 0.12f;
    float waterM = 0.0f;     // an absolute line in metres; 0 uses the percentile
    // -- A NEW WORLD EVERY LAUNCH, AND 0 IS WHAT ASKS FOR ONE -------------
    //
    // This was a date constant, so every run built the SAME wood, the same
    // hills and the same lakes -- a procedural generator producing one world.
    // The default is now drawn from the clock at startup.
    //
    // THE LIBRARY DEFAULT IS STILL FIXED, and that is not an inconsistency:
    // GenOptions::seed stays a constant so tests/brick_test.cpp, the scatter
    // probes and anything else that compares two packs of "the same world" go
    // on comparing the same world. Only the GAME rolls a new one, and
    // --world-seed N pins it again -- which is what every A/B measurement must
    // pass, because a 54 % swing between two woods would otherwise look like
    // whatever change was being measured. bench.bat pins it.
    uint32_t worldSeed = 0u;

    // -- THE WORLD HAS NO EDGE -----------------------------------------------
    //
    // A window of chunks that follows the player -- see scene/stream.h.
    // --no-stream falls back to the fixed box, which is what every pinned
    // measurement in this engine was taken against: bench.bat photographs one
    // world from three fixed cameras, and a world that rebuilds itself
    // underneath that is not a benchmark.
    bool stream = true;

    // -- THE VIEW DISTANCE, AND IT IS v2's -----------------------------------
    //
    // 12 chunks each way: a 25x25 ring, 640 m across, which is exactly what v2
    // keeps resident (`viewChunks = 12` over the same 25.6 m chunk). Asked for
    // by name, 2026-09-11.
    //
    // IT IS NOT FREE HERE AND v2 IS NOT THE SAME ENGINE. v2 meshes a chunk into
    // triangles; this one keeps 3.4 million bricks and 350 MB of device buffers
    // for the same ground. What it costs, measured:
    //
    //     radius  across   launch   hitch per 25.6 m crossing   trace
    //       4     230 m    1.2 s     96 ms                      2.99 ms
    //       8     435 m    3.0 s    ~220 ms                     ~3.3 ms
    //      12     640 m    9.1 s    470 ms                      3.63 ms
    //
    // THE TRACE BARELY MOVES -- 3.63 against 2.99 ms for seven times the ground
    // -- so this is affordable to DRAW. What it is not yet is affordable to
    // SLIDE: 365 of those 470 ms are the device rebuilding all 2,105 bottom
    // level structures and re-uploading 350 MB, because every brick lives in
    // one contiguous buffer and swapping a chunk in place needs a slot
    // allocator that is not written yet. Until it is, --stream-radius trades
    // the hitch back down.
    int streamRadius = 12;
    bool trees = true;
    bool rocks = true;
    // Keep the bricks nobody can reach. See the hidden-brick note in
    // scene/bricks.h: they are dropped because a ray from outside the ground
    // has to cross a solid neighbour to reach one, which stops being true if
    // anything can ever put the camera underground.
    bool cullHidden = true;
    // Upload the voxels nothing can reach. See the long note on PackOptions::peel
    // -- a voxel with six opaque neighbours can never be the first thing a ray
    // from outside the solid meets, and on this world that is 82 % of them.
    bool peel = true;
    // Fit every AABB to the voxels its brick keeps rather than to the brick.
    bool tightBounds = true;
    // Merge runs of adjacent uniform bricks into one larger AABB. ON: it is
    // output-identical, 6.1 MB smaller and 18.5 % fewer primitives, for about
    // 1 % of trace. See the long note on PackOptions::mergeUniform.
    // --no-merge-uniform turns it off.
    bool mergeUniform = true;
    // The two payload compressions. See PackOptions::sparseMask -- both are
    // format switches the shader reads without knowing they exist.
    // Shader execution reordering. OFF, and the reason is a crash, not a
    // measurement -- see the note at the top of gpu/ser.h. --ser opts in.
    bool ser = false;
    bool sparseMask = true;
    bool palette = true;
    bool octantSkip = false;
    // Bricks on a side of one bottom-level acceleration structure. 0 means
    // DERIVE IT -- packStore falls back to CHUNK_BRICK, which is
    // CHUNK_VOX / BRICK_E and so is 25.6 m at every brick size.
    //
    // IT WAS THE LITERAL 32, AND THAT SILENTLY BROKE EVERY BRICK-SIZE SWEEP.
    // 32 bricks is 25.6 m only at 8^3. At 4^3 it is 12.8 m and at 16^3 it is
    // 51.2 m -- so changing V4_BRICK_SHIFT also changed the chunk, the BLAS
    // count, and (because --view counts CHUNKS) the radius of world that stays
    // resident. Measured at 4^3 the literal gave 1210 bottom-level structures
    // against 190, and a resident world of 31.75 M voxels against 446.25 M.
    // Three variables moving at once, reported as one number.
    int chunkBricks = 0;
    // 0 off, 1 paint each brick its own colour -- which makes the acceleration
    // structure's granularity visible -- 2 paint how many candidate bricks the
    // ray was handed, which is the number the whole design tries to keep small.
    int storeDebug = 0;
    // Cap the in-brick DDA at this many steps. 0 is kBrickMaxSteps. Sweeping it
    // says how much of the walk's cost is in its long tail.
    int maxSteps = 0;

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
    // WHERE THE EYE STARTS VERTICALLY, and why it is normally not a number.
    //
    // The template's camera opened at y = 2 because there was nothing under it.
    // There is now, and a fixed height is the wrong thing on a landscape: it is
    // below the hills on the default seed and the engine opens inside one. So
    // the default is "settle onto whatever the store says is here", and --cam-y
    // is the override for a run that wants an exact, repeatable eye -- which an
    // A/B of two stores does.
    float camY = 2.0f;
    bool camYGiven = false;
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

        // THE WORLD STARTS EMPTY, AND EVERY KNOB THAT USED TO BE SET HERE HAS
        // NOTHING LEFT TO TUNE.
        //
        // What stood here: a seed, a biome, grass density, flower and rock and
        // tree densities, pinecones per tree, the asset directories to scatter
        // from, and a view radius in chunks. All of it configured a GENERATOR --
        // the thing that decided what the world contained before anyone touched
        // it. There is no generator now; scene/volume.h starts with nothing in
        // it and gains voxels only when something writes them.
        //
        // The only one that survives is the one that was never about content:
        // the sky. Streaming budgets, window radii and chunk counts belong to
        // whichever backend has chunks, and are configured on it.
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
        // NO SPAWN SEARCH. chooseSpawn walked the generator looking for a
        // clearing on dry ground well away from a shore -- a question only a
        // generated world can answer. An empty one has no clearings and no
        // shore, so the camera simply starts where it was asked to.
        if (opt_.groundStats) { skyStats(); shutdown(0); return; }

        // BEFORE THE MODELS LOAD, because the load is the only moment their
        // voxels exist and it throws them away when it is done.

        auto t0 = std::chrono::steady_clock::now();

        // -- ROLL THE WORLD, AND PRINT THE NUMBER THAT WOULD ROLL IT AGAIN --
        //
        // Resolved here, once, because this is the only place it is read. The
        // clock is the source: a wood you cannot get back is no use when the
        // thing you want to report is IN it, so the seed is printed on the same
        // line that says the world was built, and --world-seed N returns to it
        // exactly. Nothing else in the generator draws on the clock -- every
        // hill, tree, boulder and lake still comes out of this one number.
        //
        // Never 0, because 0 is the flag that asked for a roll.
        if (opt_.worldSeed != 0u) {
            worldSeed_ = opt_.worldSeed;
        } else {
            const uint64_t t = uint64_t(std::chrono::high_resolution_clock::now()
                                            .time_since_epoch()
                                            .count());
            // Mix, do not truncate: consecutive launches differ in the LOW bits
            // of a clock, and the generator spreads the seed's low half over x
            // and its high half over z (see the offsets in buildHeight) -- so
            // handing it a counter gives two worlds that differ along one axis
            // and are the same wood along the other.
            uint64_t h = t * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull;
            h ^= h >> 32; h *= 0x94D049BB133111EBull;
            h ^= h >> 29;
            worldSeed_ = uint32_t(h);
            if (worldSeed_ == 0u) worldSeed_ = 1u;
        }
        std::printf("v4: world seed %u%s\n", worldSeed_,
                    opt_.worldSeed != 0u ? " (pinned)" : " -- --world-seed this to build it again");
        std::fflush(stdout);

        // EVERYTHING THE BACKEND NEEDS, IN ONE PLACE AND BEFORE build(). The
        // renderer does not read any of these and never branches on them; it
        // copies the block across and asks the store to make itself.
        world_.store.gen.seed = worldSeed_;
        world_.store.gen.extentM = opt_.worldM;
        world_.store.gen.skinM = opt_.skinM;
        world_.store.gen.soilVox = opt_.soilVox;
        world_.store.gen.stoneVox = opt_.stoneVox;
        world_.store.gen.bedrockVox = opt_.bedrockVox;
        world_.store.gen.reliefM = opt_.reliefM;
        world_.store.gen.treeCellM = opt_.treeCellM;
        world_.store.gen.water = opt_.water;
        world_.store.gen.waterPct = opt_.waterPct;
        world_.store.gen.waterM = opt_.waterM;
        world_.store.gen.trees = opt_.trees;
        world_.store.gen.rocks = opt_.rocks;
        world_.store.gen.grass = opt_.grass;
        world_.store.gen.grassDensity = opt_.grassDensity;
        world_.store.gen.grassPatchM = opt_.grassPatchM;
        world_.store.gen.pineDir = opt_.pineDir;
        world_.store.gen.decorDir = opt_.decorDir;
        world_.store.gen.mineralPath = opt_.mineralPath;
        world_.store.gen.minerals = opt_.minerals;
        world_.store.gen.models = opt_.models;
        world_.store.gen.demo = opt_.demo;
        // -- ENDLESS, AND WHERE IT STARTS ------------------------------------
        //
        // The window is centred on the spawn rather than the origin, because
        // the spawn is a command-line position and a window built around (0,0)
        // would be thrown away on the first frame the camera moved.
        world_.store.stream = opt_.stream;
        world_.store.streamRadius = opt_.streamRadius;
        world_.store.spawnX = opt_.camX;
        world_.store.spawnZ = opt_.camZ;

        world_.store.pack.cullHidden = opt_.cullHidden;
        world_.store.pack.peel = opt_.peel;
        world_.store.pack.tightBounds = opt_.tightBounds;
        world_.store.pack.mergeUniform = opt_.mergeUniform;
        world_.store.pack.sparseMask = opt_.sparseMask;
        world_.store.pack.palette = opt_.palette;
        world_.store.octantSkip = opt_.octantSkip;
        world_.store.pack.chunkBricks = opt_.chunkBricks;
        world_.store.debugMode = uint32_t(opt_.storeDebug);
        world_.store.maxSteps = uint32_t(opt_.maxSteps < 0 ? 0 : opt_.maxSteps);
        if (!world_.build(getDevice(), ctx)) {
            shutdown(1);
            return;
        }
        std::printf("  store    %.2f s -- %s\n", secondsSince(t0), world_.storeStatus());
        std::printf("  models   %s\n", world_.storeModels());

        // THERE IS NO WORLD, AND THAT IS THE POINT OF THIS ENGINE.
        //
        // What used to be reported here -- chunks authored, active voxels,
        // tiles, host and device megabytes -- were all facts about ONE backend,
        // printed by a startup that had to know what a chunk and a tile were. A
        // store that is a dense bitmask has neither; one that is a BLAS of brick
        // AABBs has neither. So the startup asks for a single line of status and
        // lets the backend decide what is worth saying in it.
        //
        // Nothing is primed and nothing is streamed either, because the null
        // store holds nothing to stream. A backend does that inside its own
        // build() and its own update(), which is where the knowledge of what a
        // chunk is belongs.


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
#if V4_HAS_NRCSDK
        // NVIDIA'S CACHE, AND IT HAS TO COME BEFORE tracer_.init.
        //
        // Whether this is on decides which PROGRAMS the tracer compiles: the
        // NRC buffers only exist in a build that defined V4_NRCSDK. Bring it up
        // afterwards and the host binds five resources the shader never
        // declared -- "No member named 'gNrcQueryPathInfo' found", at the first
        // dispatch rather than at startup.
        // THE WAVE SIMULATION. Only its cascades are used -- WaveWorks renders
        // through hardware tessellation and a path tracer has no such stage,
        // so the surface is built from the displacement here instead. See
        // gpu/waveworks.h.
        if (waveWorks_.init(getDevice()))
            std::printf("  waves    %s\n", waveWorks_.status().c_str());
        else
            std::printf("  waves    Gerstner: %s\n", waveWorks_.status().c_str());
        std::fflush(stdout);

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

        // REORDERING BEFORE THE TRACER, NOT AFTER. ser_.init registers the
        // NVAPI extension slot, which the driver reads when a PIPELINE IS
        // CREATED -- and tracer_.init is what creates them. Called the other way
        // round it succeeds, changes nothing, and says it is on.
        ser_.setEnabled(opt_.ser);
        ser_.init(getDevice());
        tracer_.setSer(&ser_);
        std::printf("  reorder  %s\n", ser_.status());
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

        // RTX MEGA GEOMETRY IS GONE WITH THE TRIANGLES IT ACCELERATED.
        //
        // Clusters build a BLAS out of triangle CLAS records. There are no
        // triangles and no bottom-level structures, so there is nothing for the
        // feature to accelerate -- this is not "disabled", it is inapplicable.
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
        // The fog traces its own shadow rays, so it is specialised on the
        // geometry backend exactly like the tracer's own passes. See volfog.h.
        Falcor::DefineList fogStoreDefs;
        world_.storeDefines(fogStoreDefs);
        if (volfog_.init(getDevice(), fogStoreDefs)) {
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
        // THE .cso DIRECTORY HERE AND THE ONE CMakeLists.txt STAGES INTO MUST
        // MATCH. RTXGI's eight probe shaders are compiled by
        // compile_ddgi_shaders.bat into shaders/v4/ddgi as a POST_BUILD step and
        // loaded from disk as bytecode -- they are not Falcor programs and
        // nothing resolves them through the shader search path. Change one
        // spelling and the probes report a missing file at startup.
        if (ddgi_.init(getDevice(), Falcor::getRuntimeDirectory() / "shaders" / "v4")) {
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

        // NOTHING IS IN THE AIR. The butterflies and the songbirds were
        // instanced models -- meshed, transformed, handed to the TLAS. With one
        // volume and no instance table there is nowhere to put them; an
        // animated object in a volume march needs its own transformed grid,
        // which is new work rather than a port. See the note on the deleted
        // members below.

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
            // THE OFFLINE PATH SETS THE WATER ITSELF, because it never runs
            // onFrameRender -- see the note over renderOffline. Without this
            // every --out render had waterY at its "no water anywhere" default
            // and waterTime at zero: a submerged camera got no absorption and
            // the waves stood still. Every verification render taken before this
            // was quietly lying about both.
            // NO WATERLINE. waterAt() was a property of the generator -- the
            // height a lake sat at in a given band. Water is a material in the
            // volume now, so "where is the water" is answered per voxel by the
            // march, and there is no global line to set. Kept explicit rather
            // than left at a default, because the default is "no water
            // anywhere" and that happens to be correct for an empty world.
            tracer_.waterY = -1e9f;
            tracer_.waterTime = 0.0f;
            renderOffline(ctx);
            shutdown(0);
            return;
        }

        flycam_.walk = opt_.speed;
        flycam_.fly = opt_.startFly;
        flycam_.eye = opt_.eye;
        // -- AND PUT IT ABOVE THE GROUND -------------------------------------
        //
        // THE HOLE THE TEMPLATE LEFT HERE IS NOW FILLABLE. It could not settle
        // a spawn because it had no world to ask; this store answers groundM on
        // the HOST, out of the same bricks the acceleration structures were
        // packed from, so the camera and the rays cannot disagree about where
        // the ground is.
        //
        // WITHOUT IT THE DEFAULT LAUNCH LOOKS BROKEN AND DOES NOT LOOK BROKEN
        // FOR THIS REASON. The camera starts at y = 2 with a pitch of +7 --
        // aimed slightly UP -- so on a landscape whose mean height is 5 m it
        // opens INSIDE a hill, sees the inside faces of the voxels around it,
        // and reports a black or solid-brown frame that reads as a failed
        // store. An explicit --cam-y is honoured and skips this.
        //
        // THE FEET GO ON THE GROUND AND THE EYE RIDES --eye ABOVE THEM, which
        // is exactly what the offline path in renderOffline does, so a window
        // and an --out of the same arguments frame the same picture. --cam-y is
        // an absolute EYE height and skips both.
        // STAND THE BODY ON THE GROUND, and let it find a clear spot if the
        // one it was given is inside a trunk or a boulder. placeOnGround
        // searches from the sky, steps the feet onto the surface under the
        // whole footprint rather than under the anchor, and drops into fly mode
        // if there is genuinely nothing there -- which is the only honest
        // answer in an empty world, and better than falling forever.
        flycam_.placeOnGround(walkWorld(), opt_.camX, opt_.camZ);
        if (opt_.camYGiven) {
            flycam_.pos.y = opt_.camY - flycam_.eye;
            flycam_.onGround = false;
        }
        pos_ = flycam_.eyePosition();
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
        // THE STAGE EDITOR IS GONE WITH THE INSTANCES IT ARRANGED. It laid
        // loaded models out on a turntable to photograph them; there are no
        // models and no instance table to place them in.

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
            std::printf("v4: wrote %s at %ux%u (window, with the interface)\n",
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
        // THE FILM IS RESET WHENEVER A CHUNK LANDS. update() used to return
        // whether one had; it streams unconditionally now, so ask the streamer
        // what it committed. Miss this and an edit appears in the world while
        // the accumulated film still holds hundreds of samples of the world
        // before it -- the change fades in over a second instead of happening.
        world_.update(pos_);

        // THE SPAWN IS NOT CHECKED, BECAUSE THERE IS NOTHING TO CHECK IT
        // AGAINST. What stood here waited for the chunk under the player to
        // stream in and then pushed them out of whatever solid they had spawned
        // inside -- the fix for "you spawned me inside a rock", which was a test
        // asked before the rock existed. A store that can be spawned inside will
        // need it back, and will need it HERE rather than in onLoad, for exactly
        // the same reason.

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

        if (processInput(dt)) tracer_.resetAccumulation();

        // A SCRIPTED DROP, on its named frame. Runs the same three lines the
        // Q handler does; there is no key event on this path to reach them
        // through.

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
            // WHAT IS LEFT OF THE WATERLINE, AND IT IS ONE QUESTION.
            //
            // waterAt() asked the generator how high the lake stood in a given
            // band, and the tracer drew the water from it. That is gone: water
            // is mat::WATER in the grid, so where a lake IS and how deep it is
            // are per-voxel facts the march already answers, and there is no
            // global surface for two woods to disagree about.
            //
            // One question survives, and it is not about a voxel: did the EYE
            // begin this frame under the surface? A path only learns it is in
            // water by CROSSING into it, so a camera that starts submerged is
            // in water no ray ever entered -- it got no absorption and no
            // caustics until it surfaced and dived again. That needs a height
            // to compare the eye against, and the generator has exactly one.
            // Far below the world when it laid no water at all.
            tracer_.waterY = world_.store.world().waterM();
            // ...and a WALL clock for the waves. Not the day clock: X plus
            // scroll runs that at up to forty times speed and backwards, and a
            // lake that reverses its chop when you scrub the sun is a bug.
            waveClock_ += dt;
            tracer_.waterTime = waveClock_;
            // ...and the simulation runs on that same clock, so the cascades and
            // the shading normal can never disagree about what time it is.
            waveWorks_.tick(double(waveClock_));
            // NOTHING IS RE-MESHED. refreshWater rebuilt the lake's triangles
            // from the simulation every frame; a water voxel is just a voxel,
            // and its surface ripple is a shading normal the tracer computes
            // from the same wave clock -- see the water branch in
            // Trace.cs.slang.


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
            // THE SOUND GOES WITH THE SNATCH, so it lands on the frame the item
            // leaves the ground rather than 360 ms later when the flight
            // arrives -- see ToolSounds::pickedUp for that engine's own note
            // about having got this the wrong way round first.
        }

        // Everything that has come off the static world: the solver, and the
        // pieces it is carrying. Before the arrows only because both want the
        // same frame's dt and this one also owns the clock they are timed on.

        // The shafts in the air. AFTER processInput, so one loosed this frame
        // starts moving on the frame it left rather than the next.
        // ...and each one that stopped this tick lands with a thud. Gone with
        // the shafts that made it.

        // The bed follows the canopy. Fed the same dt as the walk and the day
        // cycle -- the shot clock when one is running -- so a scripted move
        // and a live one fade identically.
        ambience_.update(dt, forestGain());

        // Simulation is over: the camera is where it is going to be and the
        // store has streamed. Everything after this is the GPU's frame.
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

#if V4_HAS_NRCSDK
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
        // THE HAND, THE SHAFTS AND EVERYTHING THAT FLEW ARE PUBLISHED NOWHERE.
        //
        // This block placed the tool in the camera's frame, wrote its pose into
        // the instance table, handed the tracer a motion vector for it -- the
        // one surface in the scene whose movement cannot be derived from where
        // it is in the world -- and then refitted the TLAS around all of it.
        //
        // None of those exist. There is no instance table, no acceleration
        // structure to refit, and nothing that moves: every surface in an
        // all-static volume reprojects from camera motion alone, which is what
        // the terrain always did and is now what everything does.

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
#if V4_HAS_NRCSDK
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
            const std::string shot = outputPath("v4_shot_%03d.png", &shotIndex_);
            const char *name = shot.c_str();
            if (tracer_.writePng(ctx, name))
                std::printf("v4: wrote %s at %dx%d\n", name, tracer_.displayWidth(),
                            tracer_.displayHeight());
            else
                std::fprintf(stderr, "v4: could not write %s\n", name);
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
                    std::printf("v4: wrote %s at %dx%d after %d frames (%s)\n",
                                opt_.shotPath.c_str(), tracer_.displayWidth(),
                                tracer_.displayHeight(), shotFrames_,
                                tracer_.denoising() ? "reconstructed" : "accumulated");
                else if (!opt_.shotPath.empty())
                    std::fprintf(stderr, "v4: could not write %s\n", opt_.shotPath.c_str());
                if (opt_.profile) printProfile();
                // The march writes its own picture, so it saves its own file
                // beside the traced one -- the two are the same camera and the
                // same frame, which is what makes them comparable.
                // A SCRIPTED DROP SAYS WHERE IT ENDED UP, not just where it was
                // thrown from. The toss print above is half a measurement: what
                // the floor under a dropped item is worth cannot be read off a
                // screenshot, because the grass stands taller than the gap.
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
                            flycam_.fly ? "fly" : (flycam_.onGround ? "ground" : "air"),
                            flycam_.speed(), pos_.x, pos_.y, pos_.z);
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
            Gui::Window fpsWin(pGui, "v4fps", {0, 0}, {0, 0}, kBare);
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
            Gui::Window hud(pGui, "v4hud", {0, 0}, {12, 12}, kBare);
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
            Gui::Window cop(pGui, "v4copy", {0, 0}, {0, 0}, kBare);
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
        ImGui::TextUnformatted("v4  settings");
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
        // NO VOXEL COUNTERS. Chunks, active voxels and tiles are facts about
        // one backend; a store that is a dense bitmask or a BLAS of bricks has
        // none of them. A backend that wants a line here can put one behind
        // storeStatus().
        ImGui::TextUnformatted(fmt("store      %s", world_.storeStatus()).c_str());
        ImGui::TextUnformatted(fmt("models     %s", world_.storeModels()).c_str());
        ImGui::PopStyleColor();
        w.separator();

        // ---- DLSS: what is traced, and what is shown ------------------------
        //
        // AT THE TOP, AND TOGETHER, BECAUSE THEY ARE ONE TRADE AND IT IS THE
        // BIGGEST ONE IN THE PANEL. The quality mode decides how many pixels
        // are actually traced; frame generation decides how many of the frames
        // shown were rendered at all. Between them they move the fps on the
        // title line further than every row below combined, so they are read
        // first.
        //
        // NO ON/OFF ROW FOR RAY RECONSTRUCTION, and that is deliberate. At one
        // sample a pixel it is not an enhancement, it is the thing that makes
        // the image an image, and offering to switch it off is offering to
        // break the renderer. The dropdown chooses the MODE; --no-dlss still
        // exists for a reference render, which is the only context where the
        // accumulating film is the right answer.
            // ---- THE RESOLUTION, WHICH IS THE BIGGEST LEVER IN THE PANEL ----
        //
        // ABOVE THE QUALITY MODE, AND OUTSIDE ITS BLOCK, because it means
        // something with the denoiser switched off too -- without DLSS the
        // produced size IS the traced size and this is the whole control.
        // With it they multiply: at 0.70 and balanced, a 3820 x 1990 window
        // is produced at 2674 x 1393 and traced at about 1737 x 905.
        //
        // IT IS THE LARGEST LEVER IN THE ENGINE and the only one left that
        // moves trace time. Every storage change measured has bought memory
        // instead -- word-sparse masks, per-brick palettes, uniform merging
        // -- and the one that moved trace, 4^3 bricks, cost three times the
        // memory. Reordering, which would have attacked divergence, is
        // raygeneration-only and unavailable to a compute tracer. Pixels
        // are what is left.
        //
        // THE READOUT ABOVE IS THE FEEDBACK. "%d x %d traced" updates the
        // moment this moves, which matters because the number that governs
        // the frame is the TRACED one and it is two multiplications away
        // from this slider.
        //
        // NO invalidate() AND NO RESEED. The frame loop recomputes the
        // produced size every frame and tracer_.resize() no-ops unless it
        // changed -- and a resize already tells the film to forget, because
        // reprojecting yesterday's pixels onto a different grid is what
        // makes a resize flash.
        {
            float sc = opt_.scale;
            if (w.slider("Resolution", sc, 0.25f, 1.0f, false, "%.2f x")) {
                opt_.scale = sc;
            }
        }

        if (dlss_.available() && opt_.dlss) {
            // Ordered cheapest to dearest, which is also the order the internal
            // resolution climbs. DLAA traces every pixel -- it is the top of
            // this list rather than a separate switch because that is exactly
            // what it is: the quality ladder with the upscale taken out.
            static const Gui::DropdownList kQualityList = {
                {uint32_t(DlssQuality::UltraPerformance), "ultra performance"},
                {uint32_t(DlssQuality::Performance), "performance"},
                {uint32_t(DlssQuality::Balanced), "balanced"},
                {uint32_t(DlssQuality::Quality), "quality"},
                {uint32_t(DlssQuality::Dlaa), "DLAA -- trace every pixel"},
            };
            uint32_t q = uint32_t(opt_.dlssQuality);
            if (w.dropdown("Ray Reconstruction", kQualityList, q)) {
                opt_.dlssQuality = DlssQuality(q);
                // THE TRACER'S COPY IS THE ONE NGX IS ACTUALLY HANDED, and
                // forgetting it is a silent half-change. The frame loop
                // re-derives the render SIZE from opt_ every frame, so setting
                // only opt_ would resize the trace correctly and still ask NGX
                // to reconstruct it as the OLD mode -- which does not fail, it
                // just never quite resolves, and reads as a soft image rather
                // than as a bug.
                tracer_.setQuality(opt_.dlssQuality);
                invalidate();
            }
            // NO "traced -> shown" NOTE UNDER THIS ROW. The status block three
            // lines above already carries those exact numbers and updates from
            // the same place; printing them twice inside four lines reads as a
            // rendering fault rather than as feedback.
        } else if (!opt_.dlss) {
            // --no-dlss: the film is accumulating instead, and a reconstruction
            // mode is not a thing that has a meaning here.
            w.text("Ray Reconstruction off (--no-dlss) -- accumulating");
        } else {
            w.text(fmt("DLSS unavailable: %s", dlss_.status().c_str()));
        }

        // ---- frame generation -----------------------------------------------
        //
        // ONLY WHAT THIS CARD CAN DO IS OFFERED. 2x is one generated frame and
        // is what Ada does; 3x and 4x are DLSS 4 multi-frame generation and need
        // a 50-series. Selecting 4x on a 40-series is not an error -- DLSS-G
        // clamps and carries on -- so an ungated list would offer three rows
        // that behave identically and give no hint why. Streamline is asked
        // rather than assumed; see Streamline::maxGeneratedFrames.
        if (sl_.hasFrameGeneration()) {
            // AT LEAST 2x. maxGeneratedFrames reports 0 until DLSS-G has state
            // to report, which on the first frames of a run it has not, and a
            // list that loses its only working entry for a second is a row that
            // flickers. Whatever is in force is always offered too, so a mode
            // set with --fg can never fall off its own dropdown.
            const int cap = maxi(1, sl_.maxGeneratedFrames());
            // WHAT IS IN FORCE, NOT WHAT WAS ASKED FOR, and the two really do
            // part company: `--fg 4x` on an Ada card is REFUSED outright --
            // "DLSSGSetOptions failed (38)" -- rather than clamped to 2x, so
            // opt_.frameGen reads On4x while nothing at all is being generated.
            // A row showing the request would then say "4x" over a frame rate
            // that had not moved, which is the most misleading thing it could
            // say.
            const FrameGen inForce = sl_.frameGeneration();
            Gui::DropdownList fgList;
            auto offer = [&](FrameGen m, const char *name) {
                if (m == inForce || frameGenExtraFrames(m) <= cap)
                    fgList.push_back({uint32_t(m), name});
            };
            offer(FrameGen::Off, "off");
            offer(FrameGen::On2x, "2x");
            offer(FrameGen::On3x, "3x  (RTX 50-series)");
            offer(FrameGen::On4x, "4x  (RTX 50-series)");

            uint32_t f = uint32_t(inForce);
            if (w.dropdown("Frame generation", fgList, f)) {
                opt_.frameGen = FrameGen(f);
                // Straight through, because the frame loop cannot turn this
                // back on by itself -- see applyFrameGen.
                if (applyFrameGen())
                    fgMenuNote_.clear();
                else
                    fgMenuNote_ = "refused: " + sl_.status();
            } else if (opt_.frameGen != inForce && fgMenuNote_.empty()) {
                // Asked for on the command line and not granted. Said once,
                // here, rather than left to be inferred from a number.
                fgMenuNote_ = std::string(frameGenName(opt_.frameGen)) +
                              " asked for but refused -- this GPU generates at most " +
                              std::to_string(cap + 1) + "x";
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            if (!fgMenuNote_.empty()) {
                ImGui::TextUnformatted(fmt("   %s", fgMenuNote_.c_str()).c_str());
            } else if (inForce == FrameGen::Off) {
                ImGui::TextUnformatted(fmt("   %.0f fps, every frame rendered", fps_).c_str());
            } else {
                ImGui::TextUnformatted(
                    fmt("   %.0f rendered/s -> %.0f shown/s", fps_, fps_ + genFps_).c_str());
                // THE ONE FAILURE THAT LOOKS LIKE A BUG AND IS NOT. DLSS-G
                // refuses to interpolate into a swapchain the compositor is not
                // showing -- "DLSS-G disabled: window not focused" in its own
                // log -- so an unfocused or minimised window generates nothing
                // while reporting itself perfectly healthy. You cannot read this
                // row without focus, but you CAN leave the panel open, click
                // away and come back to a zero, which is exactly the moment
                // somebody starts debugging a working integration.
                if (genFps_ <= 0.0f)
                    ImGui::TextUnformatted("   not generating -- the window must have focus");
            }
            ImGui::PopStyleColor();
        } else {
            w.text(fmt("Frame generation unavailable: %s", sl_.status().c_str()));
        }
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
        w.slider("Walk speed", flycam_.walk, 0.2f, 200.0f);
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
            flycam_.fly = !flycam_.fly;
            if (!flycam_.fly) flycam_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v4: %s\n", flycam_.fly ? "flying" : "walking");
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
                std::printf("v4: day/night %s\n", lbl);
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
            // "v2" IS A FORMAT VERSION, NOT THE ENGINE'S NAME. The reader above
            // parses it as "v%d" and insists on 2, because the numbers used to
            // mean the window rect and now mean the visible frame. Renaming it
            // with the engine would make every saved window position unreadable
            // -- and silently, since an unparsed file just centres the window.
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
                std::printf("v4: wrote %s (%u batches trained)\n",
                            opt_.nrcSave.c_str(), nrc_.batches());
            else
                std::printf("v4: could not write %s\n", opt_.nrcSave.c_str());
            std::fflush(stdout);
        }
        // Before the window goes: an audio device held open past it is the
        // one kind of leak you can hear. ORDERED -- every source voice is made
        // from the engine and must be destroyed before it, so the two voices
        // go first and the device last.
        ambience_.stop();
        audio_.close();
        saveWindowPlacement();
    }

    void onResize(uint32_t, uint32_t) override { tracer_.resetAccumulation(); }

  private:
    Options opt_;
    World world_;
    Ser ser_;
    Tracer tracer_;
    Dlss dlss_;
    // A PERSON, NOT A CAMERA IN SPACE. The template shipped a FlyCam because it
    // had no ground to stand on; this engine answers topAt and solidAt on the
    // host out of the same bricks the acceleration structures are packed from,
    // so the walker from the engine these assets came from drops straight in.
    // F still toggles flight.
    Player flycam_;

    // The walker's view of the world. Rebuilt per call rather than cached: it
    // is one pointer, and a cached one is a pointer to a world that has been
    // regenerated underneath it.
    WalkWorld walkWorld() const {
        WalkWorld w;
        w.world = &world_.store.world();
        return w;
    }
    // THE HAND, THE BOW, THE RAIN AND THE BUTTERFLIES ARE GONE.
    //
    // Every one of them was an INSTANCE: a .vox model meshed to triangles,
    // given a transform, and handed to the TLAS each frame. With the world as a
    // single volume there is no instance table to put them in and no
    // acceleration structure to transform them into -- an animated object in a
    // volume march has to be its own small grid, transformed at march time,
    // which is real work and not a port.
    // False until the left button has been seen UP once -- see onMouseEvent.
    // HOW BIG A BITE, in voxels of radius. Six is 60 cm at VOXEL_M -- a
    // pick-sized hole rather than a crater, and small enough that the voxel
    // pass touches roughly a dozen columns out of a chunk's 65 536.
    static constexpr int kDigRadiusVox = 3;
    // HOW FAR THE CROSSHAIR REACHES, in metres. Five is about what an arm and
    // a tool cover, and it bounds the host-side DDA -- see World::pick.
    static constexpr float kReachM = 5.0f;
    bool swingArmed_ = false;
    // What the right button builds with. One material rather than a hotbar,
    // because there is nothing to carry and nothing to run out of.
    // The last pose the menu's copy row printed, kept so the row can show it
    // back rather than the player having to find the console.
    std::string poseCopied_;
    vb::AudioDevice audio_;
    vb::Ambience ambience_;
    // What the settings panel has asked the arrow to be, and whether the frame
    // still has to act on it. The rows edit this rather than the tool's own
    // offset, so dragging stays responsive while the rebuild happens a frame
    // later -- see the note on the rows.
    Falcor::ref<Falcor::FullScreenPass> crosshair_;
    DayNight clock_;  // owns the sun; sunAz_/sunEl_ are its output
    bool placedTwice_ = false;

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
    WaveWorks waveWorks_;
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
    // The seed this run's world was actually built from -- opt_.worldSeed when
    // it was pinned, a rolled one when it was 0. See onLoad.
    uint32_t worldSeed_ = 0u;
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
    // Why the last frame-generation change from the settings menu was refused,
    // if it was. Empty means it took. Held rather than re-derived because
    // Streamline::status() moves on -- by the time the row is drawn again it
    // may be reporting something unrelated.
    std::string fgMenuNote_;
    Neural neural_;
    Nrc nrc_;
    VolFog volfog_;
    Clouds clouds_;
    Atmosphere atmo_;
    Ddgi ddgi_;
    Sharc sharc_;
    Restir restir_;
#if V4_HAS_NRCSDK
    NrcSdk nrcSdk_;
    uint32_t nrcSdkLogTick_ = 0;
#endif
    Cuda cuda_;

    // Where the wood was when U was pressed -- see the handler.
    Vec3 woodPos_{0, 0, 0};
    float woodYaw_ = 0.0f, woodPitch_ = 0.0f;
    bool woodFly_ = false;
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

    // -----------------------------------------------------------------------
    // SWITCH FRAME GENERATION TO opt_.frameGen, NOW.
    //
    // THE FRAME LOOP CANNOT DO THIS, AND THAT IS THE WHOLE REASON THIS EXISTS.
    // Its re-declare block is guarded on `frameGeneration() != Off`, so the
    // moment the mode reaches Off nothing in the loop ever calls
    // slDLSSGSetOptions again and the feature can never be switched back ON --
    // the same trap the start-up note describes, arrived at from the other
    // side. A menu row that could only ever turn it off would be worse than no
    // menu row.
    //
    // IT SYNCS slFgDim_/slFgOut_ RATHER THAN CLEARING THEM. Streamline warns
    // that a repeated slDLSSGSetOptions() for one frame "is a redundant call or
    // a race condition with Present()", and it means the race -- so having
    // declared the sizes here, the frame loop must not declare them again for
    // the same frame. Recording them is what stops it.
    //
    // The sizes are the tracer's, which is right BECAUSE THE TWO DIFFER: depth
    // and motion vectors are by-products of the primary hit and are at RENDER
    // size, while the colour being interpolated is the presented frame. See the
    // long note on Streamline::setFrameGeneration.
    // -----------------------------------------------------------------------
    bool applyFrameGen() {
        if (!sl_.hasFrameGeneration()) return false;
        // Before the first resize the tracer has no size yet; the start-up path
        // has its own guess for that moment and this is only reached from the
        // menu, but a zero extent would be declared as valid and quietly break
        // the tagging.
        if (tracer_.width() <= 0 || tracer_.displayWidth() <= 0) return false;
        const uint2 renderDim{uint32_t(tracer_.width()), uint32_t(tracer_.height())};
        const uint2 outDim{uint32_t(tracer_.displayWidth()),
                           uint32_t(tracer_.displayHeight())};
        if (!sl_.setFrameGeneration(opt_.frameGen, renderDim, outDim)) return false;
        slFgDim_ = renderDim;
        slFgOut_ = outDim;
        // Whatever DLSS-G would interpolate against is from before the mode
        // changed. Tell it not to trust it.
        slReset_ = true;
        return true;
    }

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
    // THE DEMO SCENE BELONGS TO A BACKEND, AND IT IS WORTH BUILDING EARLY.
    //
    // What stood here authored a test scene into the empty world -- a floor, a
    // ring of arches, towers, steps across a chunk seam, and a crater bitten
    // out of the floor -- and every piece was chosen to catch a specific way of
    // getting a store wrong:
    //
    //   the floor     one box fill. Does the store TILE a solid interior, or
    //                 does it pay per voxel for rock nobody can see?
    //   the arches    solid, air, solid in one column. A heightfield cannot
    //                 express it; if the traversal is quietly treating the
    //                 world as a height per column, the gap fills in.
    //   the towers    tall and thin, so a grazing ray has to step a fine
    //                 feature rather than skip it.
    //   the steps     each a separate box crossing a chunk seam with real
    //                 geometry rather than empty space -- seam bugs show here
    //                 and nowhere else.
    //   the crater    a carve. The destruction path has to make the store
    //                 SMALLER, which is the case most representations fail.
    //
    // Author it through the GENERIC edit API rather than a backend's own, so
    // every store gets a byte-identical world and an A/B between two of them
    // compares the stores rather than two different scenes.
    // -----------------------------------------------------------------------

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
            std::printf("v4: recording stopped -- encoding\n");
            std::fflush(stdout);
            return;
        }
        // Still writing the last one. Refuse rather than queue: two sink
        // writers and two encoder threads for one hardware encoder is a way to
        // make both takes worse.
        if (recorder_.busy()) {
            std::printf("v4: still finishing the last take\n");
            std::fflush(stdout);
            return;
        }

        if (tracer_.displayWidth() <= 0) return;

        const std::string take = outputPath("v4_take_%03d.mp4", &takeIndex_);
        const char *name = take.c_str();
        const double nowSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (!recorder_.start(name, tracer_.displayWidth(), tracer_.displayHeight(),
                             opt_.recMaxWidth, opt_.recFps, 1, nowSec)) {
            std::fprintf(stderr, "v4: could not start recording\n");
            return;
        }
        std::printf("v4: recording to %s -- %dx%d @ %d fps (R again to stop)\n", name,
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
    // THE CONSOLE.
    //
    // /locate IS GONE WITH THE THING IT NAVIGATED. It took you to the nearest
    // band of a named biome, or to the nearest water. Both were questions about
    // a GENERATOR: the biomes were north-south bands of a noise field and a
    // lake was wherever the basin carve dipped under a waterline. An empty
    // world has no bands, no waterline and no "nearest" anything -- it has
    // whatever you put in it, wherever you put it.
    //
    // /where survives, because where you are is still a fact.
    // -----------------------------------------------------------------------
    std::string runCommand(const std::string &line) {
        std::string cmd = line;
        while (!cmd.empty() && (cmd.front() == '/' || cmd.front() == ' ')) cmd.erase(cmd.begin());
        const size_t sp = cmd.find(' ');
        const std::string verb = cmd.substr(0, sp);

        if (verb == "where") {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%.0f, %.0f, %.0f -- %s", pos_.x, pos_.y, pos_.z,
                          world_.storeStatus());
            return std::string(buf);
        }
        if (verb == "help")
            return std::string("/where   ENTER runs and closes   ESC cancels");
        return std::string("unknown command '" + verb + "' -- try /help");
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

        const Vec3 before = flycam_.eyePosition();
        flycam_.update(walkWorld(), move, sprint, jump, down, crouch, dt);
        pos_ = flycam_.eyePosition();

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
        // -- NOTHING TO DIG AND NOTHING TO PLACE ----------------------------
        //
        // What stood here was the shortest honest demonstration that an edit is
        // a write: the ray under the crosshair found a voxel and the mouse took
        // material out of the tree or put it back. It needs three things this
        // engine does not have -- a host-side pick, a carve and a fill -- and
        // all three belong to a backend rather than to the renderer.
        //
        // A store that supports editing should put them back here. It is worth
        // doing early: an edit path is the fastest way to find out whether a
        // representation is actually usable, because the cost of changing one
        // voxel is the number most storage schemes are quietly bad at.

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
        // NOTHING ANIMATES ANY MORE. This also counted a swinging tool and an
        // arrow in flight, because the samples already in the film were drawn
        // for a viewmodel that had since moved. With an all-static volume the
        // camera is the only thing that can invalidate the film.
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
        // THE OFFLINE CAMERA STANDS ON THE GROUND AGAIN. The template left this
        // as an absolute height because it had no world to query; this store
        // answers groundM() on the host, so --eye is a height ABOVE the ground
        // once more and --cam-y is the absolute override. It matters more here
        // than in the window: an --out render has no second chance to notice
        // that it was taken from inside a hill.
        cam.origin = Vec3(opt_.camX,
                          opt_.camYGiven ? opt_.camY
                                         : world_.groundM(opt_.camX, opt_.camZ) + opt_.eye,
                          opt_.camZ);
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



    void renderOffline(Falcor::RenderContext *ctx) {
        // Offline never denoises -- Ray Reconstruction is temporal and there is
        // nothing temporal about one frame -- so traced and shown are the same
        // size and the accumulator does the work.
        tracer_.setDenoising(false);
        tracer_.resize(opt_.r.width, opt_.r.height, opt_.r.width, opt_.r.height);

        Camera cam;
        // THE OFFLINE CAMERA STANDS ON THE GROUND AGAIN. The template left this
        // as an absolute height because it had no world to query; this store
        // answers groundM() on the host, so --eye is a height ABOVE the ground
        // once more and --cam-y is the absolute override. It matters more here
        // than in the window: an --out render has no second chance to notice
        // that it was taken from inside a hill.
        cam.origin = Vec3(opt_.camX,
                          opt_.camYGiven ? opt_.camY
                                         : world_.groundM(opt_.camX, opt_.camZ) + opt_.eye,
                          opt_.camZ);
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
            tracer_.renderSample(ctx, c.gpu(tracer_.width(), tracer_.height()), opt_.r);
            // ONE SUBMIT PER SAMPLE, NOT PER SIXTEEN.
            //
            // Batching sixteen samples into one command list was free when the
            // world was triangles: the RT cores finished a sample in a
            // millisecond or two and sixteen of them fitted comfortably inside
            // the driver's watchdog. A software DDA over a few million voxels
            // does not -- sixteen samples at 800x450 took longer than the two
            // seconds Windows allows a single submit, and the driver removed
            // the device. The failure is reported as DXGI_ERROR_DEVICE_REMOVED
            // from a command-buffer call, which names nothing to do with the
            // march and reads like a driver fault rather than a frame that was
            // simply asked to do too much at once.
            //
            // Submitting each sample keeps every command list well inside the
            // limit and costs one fence wait per sample, which against the work
            // in a sample is nothing.
            if (true) {
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
    // HOW MUCH WOOD THE AMBIENCE HEARS.
    //
    // This measured canopy closure and height over the waterline off the
    // generator -- how dense the trees were here, how far above the lake you
    // stood -- and faded the forest bed in and out with it. None of those
    // questions has an answer in an empty world.
    //
    // Kept at full rather than deleted, so the bed still plays: an ambience
    // that silently never starts is far harder to notice than one that does
    // not modulate. When the volume grows a way to ask "how enclosed is this
    // point", this is where it goes.
    float forestGain() const { return 1.0f; }


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
            std::fprintf(stderr, "v4: cannot open font %s -- drawing in Consolas\n",
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
                std::fprintf(stderr, "v4: font atlas upload failed (%s)\n", e.what());
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
        std::printf("v4: text in %s at %.0f px (%.0f-pixel cells), %s\n", opt_.font.c_str(),
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
        crosshair_ = Falcor::FullScreenPass::create(getDevice(), "v4/shaders/Crosshair.ps.slang");
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

        std::printf(
            "\nv4 profile -- %zu frames at %dx%d -> %dx%d, %s\n"
            "  frame     mean %.2f ms (%.0f fps)   median %.2f   p95 %.2f   p99 %.2f   max %.2f\n"
            "  hitches   %zu over 2x median (%.2f%%), %zu of them streaming\n"
            "  stream    %.3f ms/frame average, worst frame %.2f ms\n",
            n, tracer_.width(), tracer_.height(), tracer_.outWidth(), tracer_.outHeight(),
            tracer_.denoising() ? dlssQualityName(opt_.dlssQuality) : "accumulate",
            sum / double(n), 1000.0 * double(n) / maxf(1e-6f, float(sum)), med, pct(0.95),
            pct(0.99), sorted[n - 1], hitches, 100.0 * double(hitches) / double(n), streamHitches,
            streamSum / double(n), worstStream);

        // WHAT THE STORE COSTS IS THE STORE'S OWN BUSINESS TO REPORT.
        //
        // What stood here counted chunks, voxels, tiles, host bytes, device
        // bytes, slots and a flatten time -- seven numbers, every one of them
        // meaningless to a backend that is not a paged window of NanoVDB grids.
        //
        // THIS PROFILE IS THE ZERO OF EVERY STORE MEASUREMENT, and that is what
        // it is for. It reports what a frame costs with the geometry query
        // removed entirely: ray setup, the lighting passes, the denoiser. A
        // backend's true cost is its own profile minus this one, which is the
        // only way to say "the march costs 2x" and have it mean something.
        std::printf("  store     %s\n", world_.storeStatus());

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
            "namespace v4 {\n"
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
            "\n"
            "}  // namespace defaults\n"
            "}  // namespace v4\n",
            opt_.scale, opt_.r.maxDepth, opt_.movingDepth, opt_.r.exposure, opt_.r.shadowLift,
            flycam_.walk,
            opt_.sensitivity, flycam_.eye, fov_,
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
            // THE HAND'S SWAY IS NOT BAKED ANY MORE, because there is no hand.
            // Its format line went with it: a printf one argument short reads
            // whatever is next on the stack, and MSVC catches exactly that as
            // C4473 -- a warning most builds let past, which this one promotes
            // to an error. That is the right call.
            ambience_.active() ? ambience_.masterGain() : opt_.ambience);
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
