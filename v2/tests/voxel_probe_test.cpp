// ---------------------------------------------------------------------------
// voxel_probe_test.cpp -- the renderer and the gameplay query are one world.
//
//   g++ -std=c++20 -O2 -I src tests/voxel_probe_test.cpp -o build/voxel_probe_test.exe
//
// THE DEFECT THIS EXISTS TO PREVENT was live until today and no test could see
// it, because every test compared the mesher against a model of the TERRAIN.
// Nothing compared it against the thing the GAME asks.
//
//     TerrainProbe::material returned mat::AIR for every voxel above the
//     surface. So a lake -- which is entirely above the surface -- was empty
//     space to the swing ray, to the held item, and to any query that ever
//     wants to know what is here. The renderer drew water; nothing else
//     believed in it.
//
// The two are composed through VoxelTerrain::aboveAt now, and this is the test
// that keeps them composed: FOR EVERY FACE THE STORE DRAWS, the probe must
// report that face's material at that voxel. If the mesher and the probe ever
// disagree about what a voxel is, the world you see stops being the world you
// can touch, and that is a class of bug no screenshot finds.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "voxel/store.h"

using namespace v2;
using namespace v2::vox;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!ok) ++g_fail;
}

int main(int argc, char **argv) {
    const int cx = (argc > 1) ? std::atoi(argv[1]) : 0;
    const int cz = (argc > 2) ? std::atoi(argv[2]) : 0;

    VoxelTerrain terrain;
    EditStore edits;
    {
        const int i = cx * CHUNK_VOX + 100, j = cz * CHUNK_VOX + 140;
        edits.carve(i, j, terrain.heightVox(i, j), 4);
    }

    BrickStore store(&terrain, &edits);
    store.buildChunk(cx, cz);
    TerrainProbe probe(&terrain, &edits);

    std::printf("chunk (%d, %d): %zu quads\n\n", cx, cz, store.residentQuads());

    // ---- every drawn face, asked of the probe -----------------------------
    std::printf("=== the surface the probe agrees with ===\n");
    size_t faces = 0, wrong = 0;
    std::map<int, size_t> wrongByMat;
    std::map<int, size_t> seenByMat;
    int byLo = 0, byHi = 0;
    store.chunkYRange(cx, cz, &byLo, &byHi);
    for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
        for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
            for (int by = byLo; by <= byHi; ++by) {
                const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                const Brick *b = store.find(bx, by, bz);
                if (!b) continue;
                for (PackedQuad q : b->quads) {
                    const uint8_t dir = quadDir(q), mtl = quadMaterial(q);
                    for (int a = 0; a < quadW(q); ++a)
                        for (int c = 0; c < quadH(q); ++c) {
                            int x = quadX(q), y = quadY(q), z = quadZ(q);
                            switch (dir) {
                                case face::POS_Y:
                                case face::NEG_Y: z += a; x += c; break;
                                case face::POS_X:
                                case face::NEG_X: z += a; y += c; break;
                                default: x += a; y += c; break;
                            }
                            const int wi = bx * BRICK_VOX + x, wj = bz * BRICK_VOX + z,
                                      wy = by * BRICK_VOX + y;
                            ++faces;
                            ++seenByMat[mtl];
                            if (probe.material(wi, wj, wy) != mtl) {
                                ++wrong;
                                ++wrongByMat[mtl];
                            }
                        }
                }
            }
    std::printf("  %zu drawn faces checked\n", faces);
    for (const auto &kv : seenByMat)
        std::printf("      material %3d : %8zu drawn%s\n", kv.first, kv.second,
                    wrongByMat.count(kv.first) ? "   <-- DISAGREES" : "");
    if (wrong)
        for (const auto &kv : wrongByMat)
            std::printf("      material %3d : %zu disagree\n", kv.first, kv.second);
    check(faces > 10000, "the chunk drew enough to mean something");
    check(wrong == 0, "the probe reports every drawn face's own material");

    // ---- water is a thing you can ask about, not just draw ----------------
    std::printf("\n=== water is visible to a query ===\n");
    {
        // Walk the chunk for a wet column, then check the whole water body
        // under it -- not only the surface, which is all the mesher draws.
        TerrainMemo memo;
        int wi = 0, wj = 0, line = 0;
        bool found = false;
        for (int j = 0; j < CHUNK_VOX && !found; j += 4)
            for (int i = 0; i < CHUNK_VOX && !found; i += 4) {
                const int a = cx * CHUNK_VOX + i, b = cz * CHUNK_VOX + j;
                if (terrain.lakeColumn(a, b, memo, &line)) { wi = a; wj = b; found = true; }
            }
        if (!found) {
            std::printf("  (this chunk is dry -- run with a wet one, e.g. -18 -17)\n");
        } else {
            const int h = terrain.heightVox(wi, wj, memo);
            size_t wet = 0, blocked = 0;
            for (int y = h + 1; y <= line; ++y) {
                if (probe.inWater(wi, wj, y)) ++wet;
                if (probe.solid(wi, wj, y)) ++blocked;
            }
            const int depth = line - h;
            std::printf("  column (%d,%d): bed %d, line %d, %d voxels deep\n", wi, wj, h, line,
                        depth);
            std::printf("  probe says water in %zu of them, blocking in %zu\n", wet, blocked);
            check(depth > 0, "the column really is under water");
            check(wet == size_t(depth), "the WHOLE water body answers as water, not just its top");
            check(blocked == 0, "water does not block a body or a swing");
            check(probe.material(wi, wj, line + 1) == mat::AIR, "and the air above it is air");
            check(probe.material(wi, wj, h) != mat::WATER, "the bed under it is not water");
        }
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
