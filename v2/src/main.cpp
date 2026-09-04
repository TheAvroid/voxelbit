// ---------------------------------------------------------------------------
// v2 -- a voxel pine forest, path traced on NVIDIA OptiX.
//
//   The world is 10 cm voxels, the same grid the pine_1..9 assets are authored
//   on, so a trunk stands in ground made of the same lattice it is. The terrain
//   is a height field evaluated as a pure function of position and meshed to
//   its exposed faces only -- interior voxels never become geometry.
//
//   OptiX 9 does the intersection work on the RT cores: one acceleration
//   structure per pine model, instanced across the terrain in quarter turns, so
//   a stand of hundreds of trees costs the memory of nine.
//
//   There is no denoiser. Every mode of the OptiX one was tried and removed:
//   on a voxel canopy the spatial models turn thousands of needle-sized faces
//   to felt, and the temporal ones lag a moving camera. What is left is
//   accumulation -- a jittered sample per frame on a card fast enough to make
//   hundreds of them a second, with a rolling window so a moving sun does not
//   invalidate the lot.
//
// Run with no --out to walk around; with --out to render one frame and exit.
// Scene, camera and sun are all reproducible from --seed.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// optix_stubs.h declares the OptiX function table; exactly one translation
// unit in the program has to DEFINE it, and this is that unit. Include it in a
// second one and the link fails on a duplicate symbol; include it in none and
// every optixSomething() call dereferences a null table at the first launch.
#include <optix_function_table_definition.h>

#include "core/defaults.h"
#include "core/image.h"
#include "gpu/renderer.h"
#include "render/camera.h"
#include "render/viewer.h"

using namespace v2;

namespace {

struct Options {
    RenderSettings r;
    ViewerOptions v;
    std::string out = "forest.png";
    bool outGiven = false;  // absent means: open a window instead
    bool writeHdr = false;
    bool validation = false;

    int trees = defaults::kTrees;
    int view = 12;
    float treeDensity = 0.31f;
    float grass = 0.105f, flowers = 0.22f, rocks = 0.010f;
    int grassMin = 3, grassMax = 6;
    float extent = 62.0f;  // half-size of the voxel patch, metres
    std::string pines = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decor = "C:/voxelbit/game/assets/decoration";

    float sunAz = defaults::kSunAz;
    float sunEl = defaults::kSunEl;
    float turbidity = 2.8f;

    float camX = -6.0f, camZ = 34.0f;
    float yaw = 205.0f, pitch = 7.0f;
    float eye = 1.80f;
    float fov = defaults::kFov;
    float aperture = 0.055f;
    float focus = 0.0f;

    // Everything the settings menu can bake. Applied to r and v in run(),
    // after the defaults those structs declare for themselves -- so a baked
    // value wins over the struct default and a flag wins over both.
    Options() {
        r.width = defaults::kWidth;
        r.height = defaults::kHeight;
        r.maxDepth = defaults::kDepth;
        r.exposure = defaults::kExposure;
        v.scale = defaults::kScale;
        v.speed = defaults::kSpeed;
        v.eye = defaults::kEye;
        v.movingDepth = defaults::kMovingDepth;
        v.timeOfDay = defaults::kTimeOfDay;
        v.cycleSpeed = defaults::kCycleSpeed;
    }
};

void usage() {
    std::printf(
        "v2 -- voxel pine forest, path traced on OptiX 9\n"
        "\n"
        "  With no --out, v2 opens a window and you can walk around in it.\n"
        "  With --out, it renders one frame offline and exits.\n"
        "\n"
        "  --width N --height N      image / window size    (default 1280x720)\n"
        "  --spp N                   samples per pixel      (default 64, offline only)\n"
        "  --depth N                 max path length        (default 10)\n"
        "  --rr N                    first bounce Russian roulette may kill (default 1)\n"
        "  --clamp F                 firefly ceiling on indirect light, 0 = off (default 24)\n"
        "  --out PATH                render offline to this png and exit\n"
        "  --scale F                 viewer output res as a fraction of the window (0.70)\n"
        "  --speed F                 walk speed, m/s                          (4.6)\n""  --eye F                   eye height, metres -- 18 voxels         (1.80)\n"
        "  --seed N                  world seed            (default 20260904)\n"
        "  --view N                  chunks of 25.6 m kept resident, radius    (12)\n"
        "  --density F               how thick the wood is, 0..1            (0.31)\n"
        "  --grass F                 fraction of grass columns with a strand (0.105)\n"
        "  --grass-rows MIN MAX      strand height in voxels                 (3 6)\n"
        "  --flowers F               flower colony density                   (0.22)\n"
        "  --rocks F                 rock density                           (0.010)\n"
        "  --pines DIR               folder with pine_1..9.vox\n"
        "  --sun-az DEG --sun-el DEG sun position, offline  (default 38, 24)\n"
        "  --time H                  viewer start hour, 0-24          (default 7)\n"
        "  --cycle N                 day/night speed, negative rewinds (default 1)\n"
        "                            a day is 20 minutes at 1x; X + wheel changes it\n"
        "  --turbidity F             haze, 2 clear .. 8    (default 2.8)\n"
        "  --cam-x F --cam-z F       camera ground position (default -6, 34)\n"
        "  --yaw DEG --pitch DEG     camera direction      (default 205, 7)\n"
        "  --fov DEG                 vertical fov          (default 50)\n"
        "  --aperture F              lens diameter, metres (default 0.055)\n"
        "  --focus F                 focus distance        (default: auto)\n"
        "  --exposure F              tone-map exposure     (default 1.0)\n"
        "  --fog F                   haze density          (default 0.0022)\n"
        "  --stats                   print per-frame timings in the viewer\n"
        "  --validation              turn on OptiX validation mode (slow)\n"
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

bool parse(int argc, char **argv, Options *o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return false; }
        else if (a == "--width") argInt(argc, argv, i, &o->r.width);
        else if (a == "--height") argInt(argc, argv, i, &o->r.height);
        else if (a == "--spp") argInt(argc, argv, i, &o->r.spp);
        else if (a == "--depth") argInt(argc, argv, i, &o->r.maxDepth);
        else if (a == "--rr") argInt(argc, argv, i, &o->r.rrStart);
        else if (a == "--clamp") argFloat(argc, argv, i, &o->r.clampIndirect);
        else if (a == "--trees") argInt(argc, argv, i, &o->trees);
        else if (a == "--grass") argFloat(argc, argv, i, &o->grass);
        else if (a == "--flowers") argFloat(argc, argv, i, &o->flowers);
        else if (a == "--rocks") argFloat(argc, argv, i, &o->rocks);
        else if (a == "--density") argFloat(argc, argv, i, &o->treeDensity);
        else if (a == "--view") argInt(argc, argv, i, &o->view);
        else if (a == "--grass-rows") {
            argInt(argc, argv, i, &o->grassMin);
            argInt(argc, argv, i, &o->grassMax);
        }
        else if (a == "--extent") argFloat(argc, argv, i, &o->extent);
        else if (a == "--pines") { if (i + 1 < argc) o->pines = argv[++i]; }
        else if (a == "--decor") { if (i + 1 < argc) o->decor = argv[++i]; }
        else if (a == "--time") { float h = 7.0f; argFloat(argc, argv, i, &h); o->v.timeOfDay = (h / 24.0f) - floorf(h / 24.0f); }
        else if (a == "--cycle") argFloat(argc, argv, i, &o->v.cycleSpeed);
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
        else if (a == "--stats") o->v.stats = true;
        else if (a == "--validation") o->validation = true;
        else if (a == "--hdr") o->writeHdr = true;
        else if (a == "--seed") { int s = 0; argInt(argc, argv, i, &s); o->r.seed = uint32_t(s); }
        else if (a == "--scale") argFloat(argc, argv, i, &o->v.scale);
        else if (a == "--speed") argFloat(argc, argv, i, &o->v.speed);
        else if (a == "--out" || a == "--render") {
            if (i + 1 < argc) { o->out = argv[++i]; o->outGiven = true; }
        }
        else { std::fprintf(stderr, "v2: unknown option %s\n", a.c_str()); usage(); return false; }
    }
    return true;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// v2.optixir and tonemap.ptx sit next to the exe, which is where build.sh puts
// them. Resolving them from argv[0] rather than the working directory means the
// .bat can be double-clicked from anywhere.
std::string beside(const char *argv0, const char *name) {
    std::string p(argv0 ? argv0 : "");
    const size_t cut = p.find_last_of("/\\");
    return (cut == std::string::npos ? std::string() : p.substr(0, cut + 1)) + name;
}

int run(int argc, char **argv) {
    Options o;
    if (!parse(argc, argv, &o)) return 0;
    if (o.r.width < 8 || o.r.height < 8 || o.r.spp < 1) {
        std::fprintf(stderr, "v2: nonsensical image parameters\n");
        return 1;
    }
    o.v.width = o.r.width;
    o.v.height = o.r.height;
    o.v.sunAz = o.sunAz;
    o.v.sunEl = o.sunEl;
    o.v.trees = o.trees;
    o.v.eye = o.eye;
    o.v.scale = clampf(o.v.scale, 0.15f, 2.0f);

    // -----------------------------------------------------------------------
    // Device
    // -----------------------------------------------------------------------
    Pipeline pipe;
    pipe.initContext(o.validation);
    char devName[256] = {0};
    pipe.deviceName(devName, sizeof(devName));
    std::printf("v2 -- OptiX %d.%d.%d on %s\n", OPTIX_VERSION / 10000,
                (OPTIX_VERSION % 10000) / 100, OPTIX_VERSION % 100, devName);

    auto t0 = std::chrono::steady_clock::now();
    pipe.buildPipeline(beside(argv[0], "v2.optixir"), o.r.maxDepth);
    std::printf("  pipeline %.2f s\n", secondsSince(t0));

    // -----------------------------------------------------------------------
    // World
    // -----------------------------------------------------------------------
    t0 = std::chrono::steady_clock::now();
    GpuScene scene;
    scene.seed = o.r.seed;
    scene.terrain.grassDensity = clampf(o.grass, 0.0f, 1.0f);
    scene.flowerDensity = clampf(o.flowers, 0.0f, 1.0f);
    scene.rockDensity = clampf(o.rocks, 0.0f, 1.0f);
    scene.treeDensity = clampf(o.treeDensity, 0.0f, 1.0f);
    scene.decorDir = o.decor;
    scene.viewChunks = mini(15, maxi(1, o.view));
    scene.terrain.grassMinRows = maxi(1, o.grassMin);
    scene.terrain.grassMaxRows = maxi(o.grassMin, o.grassMax);
    // A stream of its own, so re-seeding the wood does not also reshuffle every
    // blade of grass in it -- the two are independent things to want varied.
    scene.terrain.strandSeed = o.r.seed + 991u;
    scene.pineDir = o.pines;
    scene.sky.turbidity = o.turbidity;
    scene.sky.setSun(o.sunAz, o.sunEl);
    if (!scene.build(pipe.context())) return 1;
    pipe.buildSbt(scene.hitGroups());

    // From here a chunk can publish its own SBT record when it takes a slot.
    scene.sbtOwner = &pipe;
    scene.sbtWrite = [](void *owner, unsigned index, const HitGroupData &d) {
        static_cast<Pipeline *>(owner)->writeHitRecord(index, d);
    };

    std::printf("  models   %.2f s -- %d pines, %d rocks, %d flowers, %d materials\n",
                secondsSince(t0), scene.loadedPines, scene.loadedRocks, scene.loadedFlowers,
                scene.palette.used());
    std::printf("           %.2f M unique tris in %d models at %.0f cm voxels\n",
                scene.uniqueTris / 1e6, scene.loadedPines + scene.loadedRocks +
                    scene.loadedFlowers, VOXEL_M * 100.0f);
    std::printf("           %.2f GB of video memory free after the build\n",
                double(pipe.freeVideoMemory()) / (1024.0 * 1024.0 * 1024.0));

    // Fill the ring before the first frame, so nobody sees a hole in the
    // ground while the workers catch up.
    t0 = std::chrono::steady_clock::now();
    {
        const float gy = scene.terrain.heightM(o.camX, o.camZ);
        scene.primeBlocking(Vec3(o.camX, gy + o.eye, o.camZ));
    }
    std::printf("  world    %.2f s -- %zu chunks of %.1f m resident, %.1f M tris, %zu instances\n",
                secondsSince(t0), scene.chunkCount(), CHUNK_M, scene.residentTris() / 1e6,
                scene.instanceCount());
    std::printf("           %.0f ms of that was GPU structure building\n", scene.gasMs());

    Renderer renderer;
    renderer.init(&pipe, &scene, beside(argv[0], "tonemap.ptx"));

    // -----------------------------------------------------------------------
    // Camera
    // -----------------------------------------------------------------------
    Camera cam;
    const float groundY = scene.terrain.heightM(o.camX, o.camZ);
    cam.origin = Vec3(o.camX, groundY + o.eye, o.camZ);
    cam.target = cam.origin + Camera::direction(o.yaw, o.pitch) * 50.0f;
    cam.fovDeg = o.fov;
    cam.aperture = o.aperture;
    cam.focusDist = o.focus > 0.0f ? o.focus : 40.0f;
    std::printf("  camera   (%.1f, %.1f, %.1f) fov %.0f focus %.1f m\n", cam.origin.x, cam.origin.y,
                cam.origin.z, cam.fovDeg, cam.focusDist);

    // -----------------------------------------------------------------------
    // Interactive, unless an output file was named
    // -----------------------------------------------------------------------
    if (!o.outGiven) {
        Viewer viewer(scene, renderer, o.r, o.v);
        return viewer.run(cam.origin, o.yaw, o.pitch, o.fov);
    }

    // -----------------------------------------------------------------------
    // Render one frame
    // -----------------------------------------------------------------------
    renderer.resize(o.r.width, o.r.height);

    const CameraGPU gcam = cam.gpu(renderer.renderWidth(), renderer.renderHeight());
    t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < o.r.spp; ++s) {
        renderer.renderSample(gcam, o.r);
        if ((s % 16) == 15 || s + 1 == o.r.spp) {
            renderer.sync();
            std::printf("\r  render   %5.1f%%  (%.1f s)", 100.0 * (s + 1) / o.r.spp,
                        secondsSince(t0));
            std::fflush(stdout);
        }
    }
    renderer.sync();
    const double renderSec = secondsSince(t0);
    std::printf("\r  render   %.2f s -- %.1f Mpaths/s                    \n", renderSec,
                double(o.r.width) * o.r.height * o.r.spp / renderSec / 1e6);

    renderer.resolve(o.r);
    renderer.sync();

    std::vector<unsigned char> rgba;
    renderer.downloadDisplay(&rgba);
    if (!writePng(o.out, rgba, renderer.outWidth(), renderer.outHeight())) {
        std::fprintf(stderr, "v2: could not write %s\n", o.out.c_str());
        return 1;
    }
    std::printf("  wrote    %s\n", o.out.c_str());

    if (o.writeHdr) {
        std::vector<float> linear;
        renderer.downloadLinear(&linear);
        const std::string p = o.out.substr(0, o.out.find_last_of('.')) + ".pfm";
        writePfm(p, linear, renderer.outWidth(), renderer.outHeight());
        std::printf("  wrote    %s\n", p.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    // Every CUDA and OptiX failure in this engine arrives as an exception
    // carrying the call that failed and the driver's own description of why.
    // Catching them here means the exit path prints that instead of the
    // "terminate called after throwing" that an uncaught one would.
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\nv2: %s\n", e.what());
        return 1;
    }
}
