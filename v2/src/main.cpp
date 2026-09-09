// ---------------------------------------------------------------------------
// v2 -- an endless voxel pine forest, path traced on NVIDIA Falcor.
//
//   The world is 10 cm voxels, the same grid the pine_1..9 assets are authored
//   on, so a trunk stands in ground made of the same lattice it is. The terrain
//   is a height field evaluated as a pure function of position and meshed to
//   its exposed faces only -- interior voxels never become geometry. You walk
//   it as a person: 20 voxels to the eye, gravity, a jump, and a head bob.
//
//   WHAT IS NEW IS THE PIPELINE, and nothing else. The world, the walk, the
//   day/night clock, the sky, the BSDFs and the integrator are v2's, carried
//   across with their numbers untouched. What changed is that the renderer
//   underneath them stopped being an OptiX pipeline and became one compute
//   shader doing inline ray tracing.
//
//   THAT IS THE WHOLE POINT OF THIS ENGINE. OptiX is built for offline
//   rendering, and it shows in the shape it forces on a game: a shader binding
//   table that has to be allocated, published and kept in step with what is
//   resident; a device artefact on disk that has to match the exe field for
//   field or the next launch dies with an illegal address; a continuation stack
//   sized for the deepest path any thread might take. None of that is about
//   drawing a forest. Under DXR inline ray tracing there is no table, no
//   separate artefact, and no stack -- the path loop is an ordinary loop in an
//   ordinary kernel, and a ray is a call. See shaders/Trace.cs.slang.
//
// Run with no --out to walk around; with --out to render one frame and exit.
// Scene, camera and sun are all reproducible from --seed.
// ---------------------------------------------------------------------------
#include "Core/SampleApp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "app.h"
#include "gpu/streamline.h"

// D3D12 reads this out of the PROCESS IMAGE before anything of ours runs, so it
// has to be exported from the executable and cannot live inside Falcor.dll.
// Without it the redistributable D3D12 runtime beside the exe is ignored, the
// OS one is used instead, and Falcor says so at startup.
FALCOR_EXPORT_D3D12_AGILITY_SDK

using namespace v2;

namespace {

void usage() {
    std::printf(
        "v2 -- voxel pine forest, path traced on Falcor (DXR inline ray tracing)\n"
        "\n"
        "  With no --out, v2 opens a window and you can walk around in it.\n"
        "  With --out, it renders one frame offline and exits.\n"
        "\n"
        "  --width N --height N      image / window size    (default 1280x720)\n"
        "  --spp N                   samples per pixel      (default 64, offline only)\n"
        "  --spf N                   samples traced per displayed frame (viewer)\n"
        "  --shadow-rays N           sun samples per vertex           (default 1)\n"
        "  --shadow-ray-depth N      bounces that get them            (default 1)\n"
        "  --settle                  let a still camera converge; default holds the grain\n"
        "  --background              open minimised, never take focus or the mouse\n"
        "                            (and silent -- see --no-sound)\n"
        "  --ambience F              forest ambience gain, 0 = silent   (default 0.25)\n"
        "                            what you hear is this times the canopy overhead\n"
        "  --sound PATH              the ambience bed; any file Media Foundation reads\n"
        "  --no-sound                open no audio device at all\n"
        "  --axe PATH --pick PATH    the .vox models the hand carries\n"
        "  --no-axe                  open empty-handed; H toggles, the wheel changes tool\n"
        "  --swing-log               print what each swing ran into\n"
        "  --swing-hold              hold the swing, as --shot-walk holds W\n"
        "  --fell-test               fell a tree with no window and print what the\n"
        "                            body does, frame by frame, then exit\n"
        "  --float-test              dig the ground out from under a tree and a rock\n"
        "                            with no window, and report whether they fall\n"
        "  --tool N                  which tool the hand opens with (0 axe, 1 pick, 2 bow)\n"
        "  --bow PATH --arrow PATH   the bow's draw strip, and what it looses\n"
        "  --draw-hold               hold the draw, as --swing-hold holds the swing\n"
        "  --shot-loose N            ...and let go on frame N, so a shot can be filmed\n"
        "  --walk F                  offline: advance F m/frame, film resets as it would\n"
        "                            when walking\n"
        "  --depth N                 max path length        (default 10)\n"
        "  --rr N                    first bounce Russian roulette may kill (default 1)\n"
        "  --clamp F                 firefly ceiling on indirect light, 0 = off (default 24)\n"
        "  --out PATH                render offline to this png and exit\n"
        "  --scale F                 game resolution as a fraction of the window   (0.70)\n"
        "                            NOT the window size -- the frame is MADE this big\n"
        "                            and stretched up to fill the window. Under DLSS it\n"
        "                            is what gets produced; the mode picks what is traced.\n"
        "  --flare F                 sun glare and lens flare, 0 = off             (2.0)\n"
        "  --rec SECONDS             record a take this long from startup, then exit\n"
        "  --rec-fps N               recorder capture rate, frames/s                 (60)\n"
        "  --rec-width N             cap the recording width; H.264 stops at 4096  (3840)\n"
        "  --speed F                 walk speed, m/s; sprint is 1.85x it           (4.97)\n"
        "  --sensitivity F           mouse look, degrees of turn per pixel         (0.12)\n"
        "  --eye F                   eye height, metres -- 20 voxels               (2.00)\n"
        "  --seed N                  world seed            (default 20260904)\n"
        "  --view N                  chunks of 25.6 m kept resident, radius          (12)\n"
        "  --density F               how thick the wood is, 0..1                 (0.3210)\n"
        "  --butterflies N           how many are in the air at once, 0 = none       (24)\n"
        "  --sfx F                   tool and weapon volume; 0 silences them        (1.0)\n"
        "  --arrow-pos X Y Z         nudge the nocked arrow, in whole 10 cm voxels:\n"
        "                            across, along the shaft, up -- the .vox file's own\n"
        "                            axes, and the three the settings panel edits (0 0 0)\n"
        "  --grass F                 fraction of grass columns with a strand      (0.105)\n"
        "  --grass-rows MIN MAX      strand height in voxels                        (3 6)\n"
        "  --flowers F               how thick a flower bed is, 0..1               (0.45)\n"
        "  --rocks F                 rock density                                (0.0075)\n"
        "  --pines DIR               folder with pine_1..9.vox\n"
        "  --decor DIR               folder with rocks/ and flowers.vox\n"
        "  --font PATH               the face all text is drawn in; \"off\" for Consolas\n"
        "                            (default the game's own 3x3-pixel.otf)\n"
        "  --sun-az DEG --sun-el DEG sun position, offline\n"
        "  --no-atmosphere           the OLD Preetham fit instead of Hillaire\n"
        "                            scattering -- cheaper, and its sunset freezes\n"
        "                            once the sun is under the horizon\n"
        "  --night-floor V           airglow/starlight the sky never goes below\n"
        "  --night-brightness M      how dark the night gets -- one master over the\n"
        "                            moon's key light AND the floor above. 1 is as\n"
        "                            rendered, 0 is black, 3 is a readable midnight\n"
        "  --blue-noise              void-and-cluster sampling on the shallow\n"
        "                            dimensions -- same variance, less of it visible\n"
        "  --auto-exposure           a trimmed histogram sets the stop, and adapts\n"
        "  --exposure-key V          what the middle of the frame is aimed at\n"
        "  --bloom V                 lens bloom strength, 0 off\n"
        "  --bloom-threshold V       where highlights start to bloom, exposed units\n"
        "  --time H                  viewer start hour, 0-24\n"
        "  --cycle N                 day/night speed, negative rewinds (default 1)\n"
        "                            a day is 20 minutes at 1x; X + wheel changes it\n"
        "  --turbidity F             haze, 2 clear .. 8    (default 2.8)\n"
        "  --cam-x F --cam-z F       camera ground position -- naming either one also\n"
        "                            turns off the random spawn\n"
        "  --spawn N                 spawn seed; 0 (the default) is somewhere new every\n"
        "                            launch. The world is unchanged -- only where in it\n"
        "                            you wake up. Viewer only; --out keeps its camera.\n"
        "  --yaw DEG --pitch DEG     camera direction      (default 205, 7)\n"
        "  --fov DEG                 vertical fov          (default 80)\n"
        "  --aperture F              OFFLINE lens diameter, metres      (0.055)\n"
        "  --focus F                 offline focus distance      (default: auto)\n"
        "  --exposure F              tone-map exposure\n"
        "  --shadow-lift F           tone curve toe, 0.03 crushed .. 0.20 open  (0.100)\n"
        "  --fog F                   haze density         (default 0.00164)\n"
        "  --fog-sky-under F         how much sky light reaches fog under the\n"
        "                            canopy, 0-1. The sky fill used to be added\n"
        "                            unshadowed, which piled up along whichever\n"
        "                            sightline was longest and read as a sun\n"
        "                            glare that followed the camera. 1 is that\n"
        "                            old behaviour back            (default 0.65)\n"
        "  --demodulate              PHASE B: split the lighting from the texture\n"
        "                            before denoising, and multiply it back after.\n"
        "                            Needed by NRD and the Super Resolution route;\n"
        "                            Ray Reconstruction demodulates internally and\n"
        "                            does NOT want it\n"
        "  --check-demod             prove the split is lossless -- one sample in,\n"
        "                            divided and multiplied straight back, compared\n"
        "                            against the composited trace -- then exit\n"
        "  --no-dlss                 turn off DLSS Ray Reconstruction and accumulate\n"
        "                            instead -- unbiased, and far noisier while walking\n"
        "  --rr-preset N             which Ray Reconstruction model: 5 = preset E\n"
        "                            (the default), 4 = D, 0 = the driver's own pick\n"
        "  --dlss MODE               ultra-performance | performance | balanced |\n"
        "                            quality | dlaa            (default quality)\n"
        "  --shot PATH               run the viewer, write a png, quit -- the only way\n"
        "                            to see a TEMPORAL renderer, which --out cannot\n"
        "  --shot-frame N            how many frames first          (default 240)\n"
        "  --shot-walk               hold W while they run\n"
        "  --shot-ui PATH            photograph the WINDOW instead -- the crosshair and\n"
        "                            the menu live there, not in the render\n"
        "  --menu                    open the settings panel at startup\n"
        "  --ground-stats            print what the ground is made of, region by region,\n"
        "                            and what it is lit by -- sun against sky -- then exit\n"
        "  --fly                     start in fly mode -- no collision, so a scripted\n"
        "  --birch                   pin the world to the birch wood: low rounded\n"
        "                            hills, one light green, beehives in 1%% of trees\n"
        "  --pine                    pin the world to the pine wood. Without either flag\n"
        "                            the two are BANDS you walk between -- T, /locate birch\n"
        "                            walk cannot park itself against a trunk\n"
        "  --shot-dt F               simulated seconds per frame  (default 1/60), so\n"
        "                            two captures cover the same ground\n"
        "  --stats                   print per-frame timings in the viewer\n"
        "  --profile                 measure --shot-frame frames and print where the\n"
        "                            time went -- percentiles, hitches, and how much of\n"
        "                            the main thread the streamer took. No png needed.\n"
        "  --restir                  ReSTIR GI: resample the indirect bounce across\n"
        "                            pixels and frames instead of tracing it fresh\n"
        "  --no-restir-world         ...without the world-space reservoirs, which are\n"
        "                            filed against the voxel face they were found on\n"
        "                            so a disocclusion has something to fall back on\n"
        "  --restir-world-m N        how much confidence one of those may carry (16)\n"
        "  --sharc-stats             count cache occupancy and hit rate, and print\n"
        "                            them with --profile\n"
        "  --sharc-stale N           frames an unwritten cache entry survives (32)\n"
        "  --sharc-hash-grid         file the radiance cache under the SDK's\n"
        "                            distance-quantised hash instead of the exact\n"
        "                            voxel face -- for comparing the two\n"
        "  --nrc-freq                the neural cache's old frequency encoding\n"
        "  --nrc-frozen              load weights and stop training\n"
        "  --nrc-load PATH           start from weights trained earlier -- the voxel\n"
        "                            encoding is what makes these transfer between\n"
        "                            worlds at all\n"
        "  --nrc-save PATH           write the trained weights out on exit\n"
        "  --vulkan                  use Vulkan instead of D3D12\n"
        "  --debug                   turn on the graphics debug layer (slow)\n"
        "  --hdr                     also write a linear .pfm\n");
}

bool argFloat(int argc, char **argv, int &i, float *out) {
    if (i + 1 >= argc) return false;
    *out = std::strtof(argv[++i], nullptr);
    return true;
}
bool argInt(int argc, char **argv, int &i, int *out) {
    if (i + 1 >= argc) return false;
    *out = std::atoi(argv[++i]);
    return true;
}
// Seeds are unsigned and use the whole range. atoi would saturate the top half
// of it at INT_MAX, so a seed the engine printed could not be typed back in.
bool argUint(int argc, char **argv, int &i, uint32_t *out) {
    if (i + 1 >= argc) return false;
    *out = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    return true;
}

// ---------------------------------------------------------------------------
// The cache and resampling flags, lifted OUT of the else-if chain below.
//
// NOT A TIDY-UP. That chain is one nested block per option as far as the
// compiler is concerned, and MSVC stops at 128 of them -- "compiler limit:
// blocks nested too deeply", which is exactly what adding these seven in line
// produced. Anything further of this kind belongs here too.
// ---------------------------------------------------------------------------
// The wood's life, the hand, and what they sound like -- lifted out of the
// else-if chain for the reason the note above gives, and because these six were
// what pushed it back over the limit.
bool parseLifeOpt(const std::string &a, int argc, char **argv, int &i, Options *o) {
    if (a == "--butterflies") { argInt(argc, argv, i, &o->butterflies); return true; }
    if (a == "--butterfly-dir") {
        if (i + 1 < argc) o->butterflyDir = argv[++i];
        return true;
    }
    if (a == "--sound-dir") {
        if (i + 1 < argc) o->soundDir = argv[++i];
        return true;
    }
    if (a == "--sfx") { argFloat(argc, argv, i, &o->sfx); return true; }
    // Three numbers, in whole voxels -- see ArrowOffset in render/bow.h.
    if (a == "--arrow-pos") {
        argInt(argc, argv, i, &o->arrowNudge.across);
        argInt(argc, argv, i, &o->arrowNudge.along);
        argInt(argc, argv, i, &o->arrowNudge.up);
        return true;
    }
    if (a == "--drop-frame") { argInt(argc, argv, i, &o->dropFrame); return true; }
    if (a == "--stage") { o->stageAtStart = true; return true; }
    // A gain over the stride and breath in render/helditem.h -- see kHandSway.
    if (a == "--hand-sway") { argFloat(argc, argv, i, &o->handSway); return true; }
    if (a == "--bird-dir") { if (i + 1 < argc) o->birdDir = argv[++i]; return true; }
    return false;
}

bool parseCacheOpt(const std::string &a, int argc, char **argv, int &i, Options *o) {
    // The SDK's distance-quantised hash grid, in place of v2's exact voxel face
    // key. For A/B comparison of the two -- see shaders/Sharc.slang.
    if (a == "--sharc-hash-grid") { o->sharcHashGrid = true; return true; }
    // Cache instrumentation, and the eviction window it exists to tune.
    if (a == "--sharc-stats") { o->sharcStats = true; return true; }
    if (a == "--sharc-stale") { argInt(argc, argv, i, &o->sharcStale); return true; }
    if (a == "--sharc-entries") { argInt(argc, argv, i, &o->sharcEntries); return true; }
    // ReSTIR GI, and the world-space reservoirs inside it.
    if (a == "--restir") { o->restir = true; return true; }
    if (a == "--no-restir-world") { o->noRestirWorld = true; return true; }
    if (a == "--restir-world-m") { argInt(argc, argv, i, &o->restirWorldM); return true; }
    // The neural cache: which encoding, and where its weights live.
    if (a == "--nrc-freq") { o->nrcFreqEncoding = true; return true; }
    // Load weights and DO NOT keep learning. What makes "train once, ship the
    // weights" a measurable claim rather than an assertion.
    if (a == "--nrc-frozen") { o->nrcFrozen = true; return true; }
    if (a == "--nrc-sdk") { o->nrcSdk = true; return true; }
    if (a == "--nrc-sdk-radiance") { argFloat(argc, argv, i, &o->nrcSdkRadiance); return true; }
    if (a == "--nrc-sdk-builtin") { o->nrcSdkBuiltin = true; o->nrcSdk = true; return true; }
    if (a == "--nrc-sdk-debug") { argInt(argc, argv, i, &o->nrcSdkDebug); o->nrcSdk = true; return true; }
    // The step size the cache learns at. Exposed because the default it
    // shipped with drives BOTH encodings into the weight clamp -- see the
    // note on learningRate in gpu/nrc.h.
    if (a == "--nrc-lr") { argFloat(argc, argv, i, &o->nrcLr); return true; }
    if (a == "--nrc-load") { if (i + 1 < argc) o->nrcLoad = argv[++i]; return true; }
    if (a == "--nrc-save") { if (i + 1 < argc) o->nrcSave = argv[++i]; return true; }
    return false;
}

bool parse(int argc, char **argv, Options *o, bool *vulkan, bool *debugLayer) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return false; }
        else if (a == "--width") argInt(argc, argv, i, &o->r.width);
        else if (a == "--height") argInt(argc, argv, i, &o->r.height);
        else if (a == "--spp") argInt(argc, argv, i, &o->r.spp);
        else if (a == "--walk") argFloat(argc, argv, i, &o->walk);
        else if (a == "--spf") argInt(argc, argv, i, &o->samplesPerFrame);
        else if (a == "--shadow-rays") argInt(argc, argv, i, &o->r.shadowRays);
        else if (a == "--shadow-ray-depth") argInt(argc, argv, i, &o->r.shadowRayDepth);
        else if (a == "--settle") o->constantGrain = false;
        else if (a == "--background") o->background = true;
        else if (a == "--depth") argInt(argc, argv, i, &o->r.maxDepth);
        else if (a == "--rr") argInt(argc, argv, i, &o->r.rrStart);
        else if (a == "--clamp") argFloat(argc, argv, i, &o->r.clampIndirect);
        else if (a == "--grass") argFloat(argc, argv, i, &o->grass);
        else if (a == "--flowers") argFloat(argc, argv, i, &o->flowers);
        else if (a == "--rocks") argFloat(argc, argv, i, &o->rocks);
        else if (a == "--density") argFloat(argc, argv, i, &o->treeDensity);
        else if (a == "--view") argInt(argc, argv, i, &o->view);
        else if (a == "--grass-rows") {
            argInt(argc, argv, i, &o->grassMin);
            argInt(argc, argv, i, &o->grassMax);
        }
        else if (a == "--pines") { if (i + 1 < argc) o->pines = argv[++i]; }
        else if (a == "--decor") { if (i + 1 < argc) o->decor = argv[++i]; }
        else if (a == "--font") { if (i + 1 < argc) o->font = argv[++i]; }
        else if (a == "--sound") { if (i + 1 < argc) o->sound = argv[++i]; }
        else if (a == "--ambience") argFloat(argc, argv, i, &o->ambience);
        else if (a == "--no-sound") o->soundOn = false;
        else if (a == "--axe") { if (i + 1 < argc) o->axe = argv[++i]; }
        else if (a == "--pick") { if (i + 1 < argc) o->pick = argv[++i]; }
        else if (a == "--tool") argInt(argc, argv, i, &o->tool);
        else if (a == "--bow") { if (i + 1 < argc) o->bow = argv[++i]; }
        else if (a == "--arrow") { if (i + 1 < argc) o->arrow = argv[++i]; }
        else if (a == "--draw-hold") o->drawHold = true;
        else if (a == "--shot-loose") argInt(argc, argv, i, &o->shotLoose);
        else if (a == "--no-axe") o->axeOn = false;
        else if (a == "--swing-log") o->swingLog = true;
        else if (a == "--fell-test") o->fellTest = true;
        else if (a == "--float-test") o->floatTest = true;
        else if (a == "--swing-hold") o->swingHold = true;
        else if (a == "--time") {
            float h = 7.0f;
            argFloat(argc, argv, i, &h);
            o->timeOfDay = (h / 24.0f) - floorf(h / 24.0f);
        }
        else if (a == "--cycle") argFloat(argc, argv, i, &o->cycleSpeed);
        else if (a == "--sun-az") argFloat(argc, argv, i, &o->sunAz);
        else if (a == "--sun-el") argFloat(argc, argv, i, &o->sunEl);
        else if (a == "--turbidity") argFloat(argc, argv, i, &o->turbidity);
        else if (a == "--cam-x") { argFloat(argc, argv, i, &o->camX); o->camGiven = true; }
        else if (a == "--cam-z") { argFloat(argc, argv, i, &o->camZ); o->camGiven = true; }
        else if (a == "--spawn") argUint(argc, argv, i, &o->spawnSeed);
        else if (a == "--yaw") argFloat(argc, argv, i, &o->yaw);
        else if (a == "--pitch") argFloat(argc, argv, i, &o->pitch);
        else if (a == "--eye") argFloat(argc, argv, i, &o->eye);
        else if (a == "--fov") argFloat(argc, argv, i, &o->fov);
        else if (a == "--pinecones") argInt(argc, argv, i, &o->pineconesPerTree);
        else if (a == "--collide-probe") o->collideProbe = true;
        else if (a == "--aperture") argFloat(argc, argv, i, &o->aperture);
        else if (a == "--focus") argFloat(argc, argv, i, &o->focus);
        else if (a == "--exposure") argFloat(argc, argv, i, &o->r.exposure);
        else if (a == "--shadow-lift") argFloat(argc, argv, i, &o->r.shadowLift);
        else if (a == "--deep-lift") argFloat(argc, argv, i, &o->r.deepLift);
        else if (a == "--deep-range") argFloat(argc, argv, i, &o->r.deepRange);
        else if (a == "--fog") argFloat(argc, argv, i, &o->r.fogDensity);
        else if (a == "--fog-aniso") { argFloat(argc, argv, i, &o->fogAniso); o->fogAnisoGiven = true; }
        else if (a == "--fog-ambient") { argFloat(argc, argv, i, &o->fogAmbient); o->fogAmbientGiven = true; }
        else if (a == "--fog-sky-under") { argFloat(argc, argv, i, &o->fogSkyUnder); o->fogSkyUnderGiven = true; }
        else if (a == "--cloud-cut") { argFloat(argc, argv, i, &o->cloudCut); o->cloudCutGiven = true; }
        else if (a == "--cloud-var") { argFloat(argc, argv, i, &o->cloudVar); o->cloudVarGiven = true; }
        else if (a == "--cloud-sun") { argFloat(argc, argv, i, &o->cloudSun); o->cloudSunGiven = true; }
        else if (a == "--atmosphere") o->atmosphere = true;
        else if (a == "--no-atmosphere") o->atmosphere = false;
        else if (a == "--night-floor") {
            argFloat(argc, argv, i, &o->nightFloor);
            o->nightFloorGiven = true;
        }
        // NO GIVEN-FLAG. It is a multiplier with a neutral value, so an unset
        // one is 1.0 and changes nothing -- there is no "unspecified" for it to
        // mean, which is the only thing a given-flag is ever for.
        else if (a == "--night-brightness") argFloat(argc, argv, i, &o->nightBrightness);
        else if (a == "--blue-noise") o->blueNoise = true;
        else if (a == "--no-blue-noise") o->blueNoise = false;
        else if (a == "--auto-exposure") o->autoExposure = true;
        else if (a == "--no-auto-exposure") o->autoExposure = false;
        else if (a == "--bloom") argFloat(argc, argv, i, &o->bloom);
        else if (a == "--bloom-threshold") {
            argFloat(argc, argv, i, &o->bloomThreshold);
            o->bloomThresholdGiven = true;
        }
        else if (a == "--exposure-key") {
            argFloat(argc, argv, i, &o->expKey);
            o->expKeyGiven = true;
        }
        else if (a == "--cloud-moon-key") { argFloat(argc, argv, i, &o->cloudMoonKey); o->cloudMoonKeyGiven = true; }
        else if (a == "--moon") { argFloat(argc, argv, i, &o->moonScale); o->moonScaleGiven = true; }
        else if (a == "--moon-key") { argFloat(argc, argv, i, &o->moonKey); o->moonKeyGiven = true; }
        else if (a == "--moon-phase") { argFloat(argc, argv, i, &o->moonPhase); o->moonPhaseGiven = true; }
        else if (a == "--demodulate") o->demodulate = true;
        else if (a == "--check-demod") { o->checkDemod = true; o->outGiven = true; }
        else if (a == "--no-dlss") o->dlss = false;
        else if (a == "--rr-preset") argInt(argc, argv, i, &o->rrPreset);
        else if (a == "--dlss") {
            if (i + 1 >= argc) { std::fprintf(stderr, "v2: --dlss needs a mode\n"); return false; }
            const std::string m = argv[++i];
            if (m == "ultra-performance") o->dlssQuality = DlssQuality::UltraPerformance;
            else if (m == "performance") o->dlssQuality = DlssQuality::Performance;
            else if (m == "balanced") o->dlssQuality = DlssQuality::Balanced;
            else if (m == "quality") o->dlssQuality = DlssQuality::Quality;
            else if (m == "dlaa") o->dlssQuality = DlssQuality::Dlaa;
            else {
                std::fprintf(stderr, "v2: unknown DLSS mode %s\n", m.c_str());
                return false;
            }
        }
        else if (a == "--shot") { if (i + 1 < argc) o->shotPath = argv[++i]; }
        else if (a == "--shot-frame") argInt(argc, argv, i, &o->shotFrame);
        else if (a == "--shot-walk") o->shotWalk = true;
        else if (a == "--shot-ui") { if (i + 1 < argc) o->shotUi = argv[++i]; }
        else if (a == "--menu") o->menuAtStart = true;
        else if (a == "--ground-stats") o->groundStats = true;
        else if (a == "--fly") o->startFly = true;
        else if (a == "--birch") o->birch = true;
        else if (a == "--pine") o->pineOnly = true;
        else if (a == "--shot-dt") argFloat(argc, argv, i, &o->shotDt);
        else if (a == "--stats") o->stats = true;
        else if (a == "--profile") o->profile = true;
        else if (a == "--hdr") o->writeHdr = true;
        else if (a == "--vulkan") *vulkan = true;
        else if (a == "--nrc") o->nrc = true;
        else if (parseCacheOpt(a, argc, argv, i, o)) { }
        else if (parseLifeOpt(a, argc, argv, i, o)) { }
        // Sky-dome next event estimation, and the irradiance cache. Both are on
        // by default; these turn them off or retune them without a rebuild,
        // which is also how a "did this change the picture" comparison is made.
        else if (a == "--sky-rays") argInt(argc, argv, i, &o->r.skyRays);
        else if (a == "--sky-ray-depth") argInt(argc, argv, i, &o->r.skyRayDepth);
        else if (a == "--no-sky-nee") o->r.skyRays = 0;
        // 0 none, 1 DDGI probes, 2 SHaRC.
        else if (a == "--gi") argInt(argc, argv, i, &o->r.giMode);
        else if (a == "--no-gi") o->r.giMode = 0;
        else if (a == "--gi-depth") argInt(argc, argv, i, &o->r.giDepth);
        else if (a == "--gi-strength") argFloat(argc, argv, i, &o->r.giStrength);
        else if (a == "--cluster-test") o->clusterTest = true;

        // Debug switches: the two halves of the reuse, separately, so a bias can
        // be attributed to one of them instead of guessed at.
        else if (a == "--fg") {
            if (i + 1 < argc) {
                const std::string m = argv[++i];
                o->frameGen = (m == "2x")   ? FrameGen::On2x
                              : (m == "3x") ? FrameGen::On3x
                              : (m == "4x") ? FrameGen::On4x
                                            : FrameGen::Off;
            }
        }
        else if (a == "--debug") *debugLayer = true;
        else if (a == "--seed") { int s = 0; argInt(argc, argv, i, &s); o->r.seed = uint32_t(s); }
        else if (a == "--scale") argFloat(argc, argv, i, &o->scale);
        else if (a == "--flare") { argFloat(argc, argv, i, &o->flare); o->flareGiven = true; }
        else if (a == "--rec") argFloat(argc, argv, i, &o->recSeconds);
        else if (a == "--rec-fps") argInt(argc, argv, i, &o->recFps);
        else if (a == "--rec-width") argInt(argc, argv, i, &o->recMaxWidth);
        else if (a == "--speed") argFloat(argc, argv, i, &o->speed);
        else if (a == "--sensitivity") argFloat(argc, argv, i, &o->sensitivity);
        else if (a == "--out" || a == "--render") {
            if (i + 1 < argc) { o->out = argv[++i]; o->outGiven = true; }
        }
        else {
            std::fprintf(stderr, "v2: unknown option %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    bool vulkan = false, debugLayer = false;
    if (!parse(argc, argv, &o, &vulkan, &debugLayer)) return 0;
    if (o.r.width < 8 || o.r.height < 8 || o.r.spp < 1) {
        std::fprintf(stderr, "v2: nonsensical image parameters\n");
        return 1;
    }
    o.scale = clampf(o.scale, 0.10f, 2.0f);

    // -- Streamline, BEFORE Falcor exists --------------------------------
    //
    // slInit has to run before the D3D12 device is created, because what
    // Streamline does is stand underneath device and swapchain creation. Once
    // Falcor has made a real device it is too late -- there is nothing left to
    // interpose on, and Frame Generation has nowhere to put a frame.
    //
    // That is why this is in main() and not in onLoad(): SampleApp creates the
    // device in its own constructor, so onLoad is already past the point of no
    // return. See gpu/streamline.h for the whole mechanism.
    //
    // Vulkan is skipped deliberately. This integration is D3D12 -- it asks for
    // sl::RenderAPI::eD3D12 and hands Streamline an ID3D12Device -- and calling
    // it on a Vulkan run would initialise a Streamline that then refuses every
    // feature, printing failures for something nobody asked for.
    // UNCONDITIONAL, matching v6. Guarding this on the backend looked tidy --
    // the integration is D3D12 -- but preInit only loads the interposer and
    // resolves entry points; it is init() that needs a device. Skipping it on
    // Vulkan changed nothing there and was one more way for the two engines to
    // differ while chasing why frame generation was inert.
    Streamline::preInit(Falcor::getRuntimeDirectory());

    SampleAppConfig c;
    c.deviceDesc.type = vulkan ? Falcor::Device::Type::Vulkan : Falcor::Device::Type::D3D12;
    c.deviceDesc.enableDebugLayer = debugLayer;
    c.windowDesc.width = o.r.width;
    c.windowDesc.height = o.r.height;
    c.windowDesc.title = o.background ? "v2 [background] -- pine forest (Falcor)"
                                      : "v2 -- pine forest (Falcor)";
    c.windowDesc.resizableWindow = true;
    // Straight to the taskbar rather than shown and then minimised: the latter
    // flashes a window across whatever the person at the keyboard is looking at.
    if (o.background) c.windowDesc.mode = Falcor::Window::WindowMode::Minimized;
    // No window at all for an offline render. SampleApp then skips the message
    // loop and simply runs, which is exactly what a one-frame render wants.
    c.headless = o.outGiven;

    // Every device failure in this engine arrives as an exception carrying the
    // call that failed and the driver's own description of why. Catching it
    // here means the exit path prints that, rather than the "terminate called
    // after throwing" an uncaught one would.
    try {
        ForestApp app(c, o);
        std::printf("v2 -- Falcor 8.0, %s, on %s\n", vulkan ? "Vulkan" : "D3D12",
                    app.getDevice()->getInfo().adapterName.c_str());
        std::fflush(stdout);
        return app.run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\nv2: %s\n", e.what());
        return 1;
    }
}
