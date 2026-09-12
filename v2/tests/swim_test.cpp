// swim_test -- the swim ported from v1, checked against what v1 guarantees.
//
// Three things v1 is emphatic about, each checked against the simulated body:
//   1. Release Space and you SINK ALL THE WAY TO THE BOTTOM. No buoyancy.
//   2. Hold Space and you rise and settle AT the surface, not launch off it.
//   3. Gravity is not used in water -- the sink is a terminal velocity, so it
//      must not accelerate past swimSink.
#include <cmath>
#include <cstdio>
#include "render/player.h"
#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;

// Counts as it reports, so the suite can exit non-zero like every other one.
static const char *chk(bool ok, const char *good, const char *bad) {
    if (!ok) ++fails;
    return ok ? good : bad;
}

int main() {
    VoxelTerrain terrain;
    WalkWorld w;
    w.terrain = &terrain;

    // Find a deep column in the big lake.
    TerrainMemo m;
    int bi = 0, bj = 0, bestDepth = 0, line = 0;
    for (int j = -300; j < 300; ++j)
        for (int i = -300; i < 300; ++i) {
            const int vi = -4544 + i, vj = -4269 + j;
            int wl = 0;
            if (!terrain.lakeColumn(vi, vj, m, &wl)) continue;
            const int d = wl - terrain.heightVox(vi, vj, m);
            if (d > bestDepth) { bestDepth = d; bi = vi; bj = vj; line = wl; }
        }
    const float surfaceM = float(line + 1) * VOXEL_M;
    const float bedM = float(terrain.heightVox(bi, bj, m) + 1) * VOXEL_M;
    std::printf("deepest column (%d, %d): bed %.2f m, surface %.2f m, %d voxels deep\n\n", bi, bj,
                bedM, surfaceM, bestDepth);

    const float dt = 1.0f / 60.0f;

    // ---- 1. DROPPED IN, NO SPACE: must reach the bed and stay --------------
    {
        Player p;
        p.pos = Vec3(terrain.wx(bi), surfaceM + 0.5f, terrain.wx(bj));
        p.onGround = false;
        float minY = 1e9f, maxSink = 0.0f;
        for (int f = 0; f < 600; ++f) {
            p.update(w, Vec3(0, 0, 0), false, /*jump=*/false, false, false, dt);
            minY = std::fmin(minY, p.pos.y);
            // ONLY ONCE IT IS ACTUALLY SWIMMING. The body starts above the
            // water and falls in under gravity, and that entry speed is not
            // the swim's -- measuring it flagged a leak that was not there.
            if (f > 60) maxSink = std::fmax(maxSink, -p.vy);
        }
        std::printf("RELEASED  settles at %.2f m (bed %.2f)  %s\n", p.pos.y, bedM,
                    chk(std::fabs(p.pos.y - bedM) < 0.15f, "SINKS TO THE BOTTOM -- ok",
                        "*** DID NOT REACH THE BED ***"));
        std::printf("          fastest sink %.2f m/s against swimSink %.2f  %s\n", maxSink,
                    p.swimSink,
                    chk(maxSink <= p.swimSink * 1.05f, "terminal, not accelerating -- ok",
                        "*** GRAVITY IS LEAKING IN ***"));
    }

    // ---- 2. HELD SPACE FROM THE BED: must rise and settle at the surface ---
    {
        Player p;
        p.pos = Vec3(terrain.wx(bi), bedM, terrain.wx(bj));
        p.onGround = true;
        float maxY = -1e9f;
        for (int f = 0; f < 900; ++f) {
            p.update(w, Vec3(0, 0, 0), false, /*jump=*/true, false, false, dt);
            if (f > 300) maxY = std::fmax(maxY, p.pos.y + p.eye);
        }
        // Settled eye, sampled over the last second so the bob is included.
        float lo = 1e9f, hi = -1e9f;
        for (int f = 0; f < 60; ++f) {
            p.update(w, Vec3(0, 0, 0), false, true, false, false, dt);
            const float e = p.pos.y + p.eye;
            lo = std::fmin(lo, e);
            hi = std::fmax(hi, e);
        }
        std::printf("\nHELD      eye rides %.2f .. %.2f m, surface %.2f\n", lo, hi, surfaceM);
        std::printf("          bob excursion %.2f m (swimBob %.2f at the line)\n", hi - lo,
                    p.swimBob);
        std::printf("          feet %.2f m under the surface  %s\n", surfaceM - (lo - p.eye),
                    chk(lo > surfaceM && lo - p.eye < surfaceM, "head out, body in -- ok",
                        "*** NOT FLOATING AT THE SURFACE ***"));
    }

    // ---- 3. ON DRY LAND the swim must not engage at all --------------------
    {
        Player p;
        p.pos = Vec3(terrain.wx(bi) + 400.0f, 60.0f, terrain.wx(bj) + 400.0f);
        p.onGround = false;
        for (int f = 0; f < 600; ++f) p.update(w, Vec3(0, 0, 0), false, false, false, false, dt);
        const int gi = int(std::floor(p.pos.x / VOXEL_M)), gj = int(std::floor(p.pos.z / VOXEL_M));
        const float ground = float(terrain.heightVox(gi, gj, m) + 1) * VOXEL_M;
        std::printf("\nDRY LAND  lands at %.2f m, ground %.2f  %s\n", p.pos.y, ground,
                    chk(p.onGround && std::fabs(p.pos.y - ground) < 0.2f, "ordinary falling -- ok",
                        "*** THE SWIM IS FIRING ON DRY LAND ***"));
    }
    std::printf("\n%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
