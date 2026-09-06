// ---------------------------------------------------------------------------
// v4 -- an endless voxel pine forest, path traced on NVIDIA Falcor.
//
//   The world is 10 cm voxels, the same grid the pine_1..9 assets are authored
//   on, so a trunk stands in ground made of the same lattice it is. The terrain
//   is a height field evaluated as a pure function of position and meshed to
//   its exposed faces only -- interior voxels never become geometry. You walk
//   it as a person: 18 voxels to the eye, gravity, a jump, and a head bob.
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

// D3D12 reads this out of the PROCESS IMAGE before anything of ours runs, so it
// has to be exported from the executable and cannot live inside Falcor.dll.
// Without it the redistributable D3D12 runtime beside the exe is ignored, the
// OS one is used instead, and Falcor says so at startup.
FALCOR_EXPORT_D3D12_AGILITY_SDK

using namespace v4;

namespace {

void usage() {
    std::printf(
        "v4 -- voxel pine forest, path traced on Falcor (DXR inline ray tracing)\n"
        "\n"
        "  With no --out, v4 opens a window and you can walk around in it.\n"
        "  With --out, it renders one frame offline and exits.\n"
        "\n"
        "  --width N --height N      image / window size    (default 1280x720)\n"
        "  --spp N                   samples per pixel      (default 64, offline only)\n"
        "  --spf N                   samples traced per displayed frame (viewer)\n"
        "  --shadow-rays N           sun samples per vertex           (default 1)\n"
        "  --shadow-ray-depth N      bounces that get them            (default 1)\n"
        "  --settle                  let a still camera converge; default holds the grain\n"
        "  --background              open minimised, never take focus or the mouse\n"
        "  --walk F                  offline: advance F m/frame, film resets as it would\n"
        "                            when walking\n"
        "  --depth N                 max path length        (default 10)\n"
        "  --rr N                    first bounce Russian roulette may kill (default 1)\n"
        "  --clamp F                 firefly ceiling on indirect light, 0 = off (default 24)\n"
        "  --out PATH                render offline to this png and exit\n"
        "  --scale F                 traced res as a fraction of the window        (0.70)\n"
        "  --speed F                 walk speed, m/s                                (9.2)\n"
        "  --eye F                   eye height, metres -- 18 voxels               (1.80)\n"
        "  --seed N                  world seed            (default 20260904)\n"
        "  --view N                  chunks of 25.6 m kept resident, radius          (12)\n"
        "  --density F               how thick the wood is, 0..1                   (0.31)\n"
        "  --grass F                 fraction of grass columns with a strand      (0.105)\n"
        "  --grass-rows MIN MAX      strand height in voxels                        (3 6)\n"
        "  --flowers F               how thick a flower bed is, 0..1               (0.45)\n"
        "  --rocks F                 rock density                                 (0.010)\n"
        "  --pines DIR               folder with pine_1..9.vox\n"
        "  --decor DIR               folder with rocks/ and flowers.vox\n"
        "  --sun-az DEG --sun-el DEG sun position, offline\n"
        "  --time H                  viewer start hour, 0-24\n"
        "  --cycle N                 day/night speed, negative rewinds (default 1)\n"
        "                            a day is 20 minutes at 1x; X + wheel changes it\n"
        "  --turbidity F             haze, 2 clear .. 8    (default 2.8)\n"
        "  --cam-x F --cam-z F       camera ground position (default -6, 34)\n"
        "  --yaw DEG --pitch DEG     camera direction      (default 205, 7)\n"
        "  --fov DEG                 vertical fov          (default 50)\n"
        "  --aperture F              lens diameter, metres (default 0.055)\n"
        "  --focus F                 focus distance        (default: auto)\n"
        "  --exposure F              tone-map exposure\n"
        "  --fog F                   haze density          (default 0.0022)\n"
        "  --no-dlss                 turn off DLSS Ray Reconstruction and accumulate\n"
        "                            instead -- unbiased, and far noisier while walking\n"
        "  --dlss MODE               ultra-performance | performance | balanced |\n"
        "                            quality | dlaa            (default quality)\n"
        "  --shot PATH               run the viewer, write a png, quit -- the only way\n"
        "                            to see a TEMPORAL renderer, which --out cannot\n"
        "  --shot-frame N            how many frames first          (default 240)\n"
        "  --shot-walk               hold W while they run\n"
        "  --shot-dt F               simulated seconds per frame  (default 1/60), so\n"
        "                            two captures cover the same ground\n"
        "  --stats                   print per-frame timings in the viewer\n"
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
        else if (a == "--time") {
            float h = 7.0f;
            argFloat(argc, argv, i, &h);
            o->timeOfDay = (h / 24.0f) - floorf(h / 24.0f);
        }
        else if (a == "--cycle") argFloat(argc, argv, i, &o->cycleSpeed);
        else if (a == "--sun-az") argFloat(argc, argv, i, &o->sunAz);
        else if (a == "--sun-el") argFloat(argc, argv, i, &o->sunEl);
        else if (a == "--turbidity") argFloat(argc, argv, i, &o->turbidity);
        else if (a == "--cam-x") argFloat(argc, argv, i, &o->camX);
        else if (a == "--cam-z") argFloat(argc, argv, i, &o->camZ);
        else if (a == "--yaw") argFloat(argc, argv, i, &o->yaw);
        else if (a == "--pitch") argFloat(argc, argv, i, &o->pitch);
        else if (a == "--eye") argFloat(argc, argv, i, &o->eye);
        else if (a == "--fov") argFloat(argc, argv, i, &o->fov);
        else if (a == "--aperture") argFloat(argc, argv, i, &o->aperture);
        else if (a == "--focus") argFloat(argc, argv, i, &o->focus);
        else if (a == "--exposure") argFloat(argc, argv, i, &o->r.exposure);
        else if (a == "--fog") argFloat(argc, argv, i, &o->r.fogDensity);
        else if (a == "--no-dlss") o->dlss = false;
        else if (a == "--dlss") {
            if (i + 1 >= argc) { std::fprintf(stderr, "v4: --dlss needs a mode\n"); return false; }
            const std::string m = argv[++i];
            if (m == "ultra-performance") o->dlssQuality = DlssQuality::UltraPerformance;
            else if (m == "performance") o->dlssQuality = DlssQuality::Performance;
            else if (m == "balanced") o->dlssQuality = DlssQuality::Balanced;
            else if (m == "quality") o->dlssQuality = DlssQuality::Quality;
            else if (m == "dlaa") o->dlssQuality = DlssQuality::Dlaa;
            else {
                std::fprintf(stderr, "v4: unknown DLSS mode %s\n", m.c_str());
                return false;
            }
        }
        else if (a == "--shot") { if (i + 1 < argc) o->shotPath = argv[++i]; }
        else if (a == "--shot-frame") argInt(argc, argv, i, &o->shotFrame);
        else if (a == "--shot-walk") o->shotWalk = true;
        else if (a == "--shot-dt") argFloat(argc, argv, i, &o->shotDt);
        else if (a == "--stats") o->stats = true;
        else if (a == "--hdr") o->writeHdr = true;
        else if (a == "--vulkan") *vulkan = true;
        else if (a == "--debug") *debugLayer = true;
        else if (a == "--seed") { int s = 0; argInt(argc, argv, i, &s); o->r.seed = uint32_t(s); }
        else if (a == "--scale") argFloat(argc, argv, i, &o->scale);
        else if (a == "--speed") argFloat(argc, argv, i, &o->speed);
        else if (a == "--out" || a == "--render") {
            if (i + 1 < argc) { o->out = argv[++i]; o->outGiven = true; }
        }
        else {
            std::fprintf(stderr, "v4: unknown option %s\n", a.c_str());
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
        std::fprintf(stderr, "v4: nonsensical image parameters\n");
        return 1;
    }
    o.scale = clampf(o.scale, 0.15f, 2.0f);

    SampleAppConfig c;
    c.deviceDesc.type = vulkan ? Falcor::Device::Type::Vulkan : Falcor::Device::Type::D3D12;
    c.deviceDesc.enableDebugLayer = debugLayer;
    c.windowDesc.width = o.r.width;
    c.windowDesc.height = o.r.height;
    c.windowDesc.title = o.background ? "v4 [background] -- pine forest (Falcor)"
                                      : "v4 -- pine forest (Falcor)";
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
        std::printf("v4 -- Falcor 8.0, %s, on %s\n", vulkan ? "Vulkan" : "D3D12",
                    app.getDevice()->getInfo().adapterName.c_str());
        std::fflush(stdout);
        return app.run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\nv4: %s\n", e.what());
        return 1;
    }
}
