// ---------------------------------------------------------------------------
// voxel_bench.cpp -- the new brick mesher against v2's chunk mesher, on the
// real terrain generator, with no GPU in the way.
//
// This is the measurement the rewrite has to justify itself with, and it is
// deliberately an A/B on the SAME ground: one 256 x 256 voxel chunk of the
// actual world, meshed both ways.
//
//   v2   VoxelTerrain::meshChunk -- one call, 65,536 columns, full world
//        height, emitting Vec3 positions and 32-bit indices.
//   v4   the brick path -- 4 x 4 column stacks, each sliced into 64^3 bricks,
//        skipping every brick with no surface in it, emitting packed uint64
//        quads.
//
// Build:
//   g++ -std=c++20 -O2 -I src tests/voxel_bench.cpp -o build/voxel_bench.exe
//
// THE TWO NUMBERS THAT MATTER are at the bottom: how long a full chunk takes
// to mesh, and how long ONE BRICK takes -- because after v4 the second is what
// an axe blow costs, and in v2 it was the first.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "voxel/columns.h"
#include "voxel/store.h"
#include "voxel/mesher.h"

using namespace v2;
using namespace v2::vox;

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main(int argc, char **argv) {
    // Where in the world to measure. The terrain cost swings a long way
    // between two spots in the same wood, so a comparison is only worth
    // anything if both halves stand on the same ground -- same reason v3.bat
    // insists on a pinned --spawn.
    int cx = (argc > 1) ? std::atoi(argv[1]) : 0;
    int cz = (argc > 2) ? std::atoi(argv[2]) : 0;
    // MINIMUM OF N RUNS, NOT A MEAN. The first version of this file reported
    // 9.43, 16.28 and 28.29 ms for the SAME v2 chunk on three consecutive
    // runs -- a 3x spread from whatever else the machine was doing. A mean
    // over that is a measurement of the background load; the minimum is the
    // closest thing to the work itself.
    const int reps = (argc > 3) ? std::atoi(argv[3]) : 7;

    VoxelTerrain terrain;

    std::printf("chunk (%d, %d)\n", cx, cz);
    std::printf("brick %d^3 (%.1f m), chunk %d^3 (%.1f m)\n\n", BRICK_VOX,
                BRICK_VOX * VOXEL_M, BRICK_VOX * CHUNK_BRICKS, BRICK_VOX * CHUNK_BRICKS * VOXEL_M);

    // ---- v2: one chunk, the old way ---------------------------------------
    double v2ms = 0.0;
    size_t v2tris = 0, v2bytes = 0;
    {
        ChunkScratch scratch;
        // Warm the memo and the scratch vectors so the first call is not
        // measuring resize().
        terrain.meshChunk(cx, cz, scratch, nullptr);

        v2ms = 1e30;
        VoxMesh m;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = Clock::now();
            VoxMesh mr = terrain.meshChunk(cx, cz, scratch, nullptr);
            const double ms = msSince(t0);
            if (ms < v2ms) { v2ms = ms; m = std::move(mr); }
        }
        v2tris = m.triCount();
        // What actually crosses the bus: positions, indices, and the per
        // triangle word.
        v2bytes = m.position.size() * sizeof(Vec3) + m.index.size() * sizeof(uint32_t) +
                  m.tri.size() * sizeof(uint16_t);
    }

    // ---- v4: the same ground, as bricks -----------------------------------
    double v4ms = 0.0, gatherMs = 0.0;
    size_t v4quads = 0, bricksMeshed = 0, bricksSkipped = 0;
    double worstBrickMs = 0.0;
    {
        static MeshScratch mesh;
        std::vector<PackedQuad> quads;
        TerrainMemo memo;

        // Warm-up pass, same as above.
        {
            ColumnStack st;
            st.gather(terrain, memo, cx * CHUNK_BRICKS, cz * CHUNK_BRICKS);
            TerrainColumns tc(terrain, st, nullptr, st.hMin / BRICK_VOX);
            std::vector<PackedQuad> tmp;
            meshBrick(tc, mesh, &tmp);
        }

        v4ms = 1e30;
        for (int r = 0; r < reps; ++r) {
        double repGather = 0.0;
        size_t repQuads = 0, repMeshed = 0, repSkipped = 0;
        const auto t0 = Clock::now();
        for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
            for (int sx = 0; sx < CHUNK_BRICKS; ++sx) {
                ColumnStack st;
                const auto tg = Clock::now();
                st.gather(terrain, memo, cx * CHUNK_BRICKS + sx, cz * CHUNK_BRICKS + sz);
                repGather += msSince(tg);

                int byLo, byHi;
                st.brickRange(&byLo, &byHi);
                for (int by = byLo; by <= byHi; ++by) {
                    if (!st.brickHasSurface(by)) { ++repSkipped; continue; }
                    ++repMeshed;

                    const auto tb = Clock::now();
                    TerrainColumns tc(terrain, st, nullptr, by);
                    quads.clear();
                    meshBrick(tc, mesh, &quads);
                    repQuads += quads.size();

                    const double bms = msSince(tb);
                    if (bms > worstBrickMs) worstBrickMs = bms;
                }
            }
        const double rms = msSince(t0);
        if (rms < v4ms) {
            v4ms = rms;
            gatherMs = repGather;
            v4quads = repQuads;
            bricksMeshed = repMeshed;
            bricksSkipped = repSkipped;
        }
        }
    }

    const size_t v4bytes = v4quads * sizeof(PackedQuad);
    const size_t v4tris = v4quads * 2;
    const size_t v2allTris = v2tris;

    std::printf("v2  meshChunk       %8.2f ms   %8zu tris   %7.2f MB upload\n", v2ms,
                v2allTris, double(v2bytes) / (1024.0 * 1024.0));
    std::printf("v4  bricks          %8.2f ms   %8zu tris   %7.2f MB upload\n", v4ms,
                v4tris, double(v4bytes) / (1024.0 * 1024.0));
    std::printf("    of which gather %8.2f ms   (the heightfield, shared by the stack)\n",
                gatherMs);
    std::printf("    bricks meshed %zu, skipped %zu (%.0f%% of the stack had no surface)\n",
                bricksMeshed, bricksSkipped,
                100.0 * double(bricksSkipped) / double(bricksMeshed + bricksSkipped));

    std::printf("\n--- the ratios ---\n");
    std::printf("  mesh time      %.2fx %s\n", v2ms / v4ms, (v4ms < v2ms) ? "faster" : "SLOWER");
    std::printf("  triangles      %.2fx %s\n", double(v2allTris) / double(v4tris),
                (v4tris < v2allTris) ? "fewer" : "MORE");
    std::printf("  upload bytes   %.2fx less\n", double(v2bytes) / double(v4bytes));

    std::printf("\n--- what an edit costs ---\n");
    std::printf("  v2: a dug voxel re-meshes the whole chunk   %8.2f ms\n", v2ms);
    std::printf("  v4: a dug voxel re-meshes one brick         %8.2f ms  (worst seen)\n",
                worstBrickMs);
    std::printf("  ratio %.0fx\n", v2ms / (worstBrickMs > 0 ? worstBrickMs : 1e-6));

    // ---- the same edit, through the store the engine would actually use ---
    //
    // THE LOOP ABOVE IS NOT WHAT AN EDIT COSTS. It holds a ColumnStack in a
    // local and re-meshes a brick out of it, which flatters the number: the
    // engine has to FIND the stack, find the brick, decide which neighbours
    // the edit was visible from, and re-mesh every one of them. All of that is
    // BrickStore, and it is the only figure worth quoting.
    {
        EditStore ed;
        BrickStore store(&terrain, &ed);
        store.buildChunk(cx, cz);

        // A swing lands on the surface, mid-chunk, and takes a bite of radius
        // three -- the same carve EditStore::carve performs for a real blow.
        const int i = cx * CHUNK_VOX + 100, j = cz * CHUNK_VOX + 140;
        const int y = terrain.heightVox(i, j);

        double best = 1e30, bestCarve = 0, bestTouch = 0, bestFlush = 0;
        int bricks = 0;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = Clock::now();
            ed.carve(i, j, y, 3);
            const auto t1 = Clock::now();
            store.touchCarve(i, j, y, 3);
            const auto t2 = Clock::now();
            const int n = store.flush();
            const double ms = msSince(t0);
            if (ms < best) {
                best = ms;
                bricks = n;
                bestCarve = std::chrono::duration<double, std::milli>(t1 - t0).count();
                bestTouch = std::chrono::duration<double, std::milli>(t2 - t1).count();
                bestFlush = msSince(t2);
            }
        }
        std::printf("\n--- and through BrickStore, which is what the engine would call ---\n");
        std::printf("  carve + touch + re-mesh   %8.3f ms   (%d brick%s)\n", best, bricks,
                    bricks == 1 ? "" : "s");
        std::printf("  against v2's whole chunk  %8.2f ms   -- %.0fx\n", v2ms, v2ms / best);
        std::printf("    EditStore::carve  %8.3f ms   (v2 code, unchanged)\n", bestCarve);
        std::printf("    touchCarve        %8.3f ms\n", bestTouch);
        std::printf("    flush (re-mesh)   %8.3f ms\n", bestFlush);
        std::printf("  stacks held %zu, bricks held %zu, gathers %zu\n", store.residentStacks(),
                    store.residentBricks(), store.gathers());
    }
    return 0;
}
