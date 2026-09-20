// ---------------------------------------------------------------------------
// seafloor_probe.cpp -- does the MESHER draw a top face under every column?
//
//   g++ -std=c++20 -O2 -I src tests/seafloor_probe.cpp -o build/tests/seafloor.exe
//   ./build/tests/seafloor.exe <dem> <cover> <shrink> <coverGround 0|1> <cx> <cz> <R>
//
// ("parts of the seafloor are still missing" -- user 2026-09-19, of an
// underwater shot with a large straight-edged dark wedge in the bed.)
//
// tests/seabed_probe.cpp already settled that the TERRAIN has a bed under
// every wet column -- it asks heightVox and finds no holes. That leaves the
// only other place a hole can live: the MESH. This builds the real BrickStore
// over a patch and asks, for EVERY column, whether a ground face was actually
// emitted at its surface voxel. A column the terrain has ground under and the
// store draws nothing for is a hole you can see through.
//
// WHAT IT CAUGHT, and the number to re-run against: ColumnStack::brickHasSurface
// called a brick "solid through" when hMin >= y1, which throws away a brick
// whose surface lies exactly on its TOP row -- so any ground flat enough across
// a 68-column stack and sitting on y == 63 (mod 64) lost its floor entirely.
// Lake Ouachita at (1083, 2606), 80 m square: 9,524 of 641,601 wet columns,
// one connected patch about 15 m across. Zero after the `>` .
//
// It is NOT water-specific -- a bed is only the flattest surface in the world,
// so it is where a flat-ground bug lands first.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "voxel/store.h"

using namespace v2;
using namespace v2::vox;

static inline uint64_t ckey2(int i, int j) {
    return (uint64_t(uint32_t(i)) << 32) | uint32_t(j);
}

int main(int argc, char **argv) {
    const std::string demPath = argc > 1 ? argv[1] : "assets/dem/acadia10.vbdem";
    const std::string covPath = argc > 2 ? argv[2] : "assets/dem/acadia10.vbcov";
    const float shrink = argc > 3 ? float(atof(argv[3])) : 1.0f;
    const bool coverGround = argc > 4 ? atoi(argv[4]) != 0 : false;
    const float cxM = argc > 5 ? float(atof(argv[5])) : 0.0f;
    const float czM = argc > 6 ? float(atof(argv[6])) : 0.0f;
    const float RM = argc > 7 ? float(atof(argv[7])) : 120.0f;

    VoxelTerrain terrain;
    if (!terrain.loadDem(demPath, 20.0f, shrink, 1.0f)) {
        printf("FAIL dem: %s\n", terrain.dem().err());
        return 1;
    }
    if (!covPath.empty() && !terrain.loadCover(covPath)) {
        printf("FAIL cover: %s\n", terrain.cover().err());
        return 1;
    }
    terrain.coverGround = coverGround;
    printf("dem %s  cover %s  shrink %.1f  coverGround %d\n", demPath.c_str(), covPath.c_str(),
           shrink, int(coverGround));
    printf("patch centre (%.0f, %.0f) radius %.0f m\n\n", cxM, czM, RM);

    const int i0 = int(floorf((cxM - RM) / VOXEL_M)), i1 = int(floorf((cxM + RM) / VOXEL_M));
    const int j0 = int(floorf((czM - RM) / VOXEL_M)), j1 = int(floorf((czM + RM) / VOXEL_M));
    const int c0x = (int)floorf(float(i0) / CHUNK_VOX), c1x = (int)floorf(float(i1) / CHUNK_VOX);
    const int c0z = (int)floorf(float(j0) / CHUNK_VOX), c1z = (int)floorf(float(j1) / CHUNK_VOX);
    printf("chunks x %d..%d  z %d..%d  (%d total)\n", c0x, c1x, c0z, c1z,
           (c1x - c0x + 1) * (c1z - c0z + 1));

    EditStore edits;
    BrickStore store(&terrain, &edits, 4096);

    // Every upward face the store drew, by column, with its material.
    std::unordered_map<uint64_t, std::vector<std::pair<int, uint8_t>>> up;
    size_t quads = 0;

    for (int cz = c0z; cz <= c1z; ++cz)
        for (int cx = c0x; cx <= c1x; ++cx) {
            store.buildChunk(cx, cz);
            int byLo = 0, byHi = 0;
            if (!store.chunkYRange(cx, cz, &byLo, &byHi)) continue;
            for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
                for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
                    for (int by = byLo; by <= byHi; ++by) {
                        const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                        const Brick *b = store.find(bx, by, bz);
                        if (!b) continue;
                        for (PackedQuad q : b->quads) {
                            if (quadDir(q) != face::POS_Y) continue;
                            ++quads;
                            const uint8_t mtl = quadMaterial(q);
                            const int y = by * BRICK_VOX + quadY(q);
                            for (int a = 0; a < quadW(q); ++a)
                                for (int c = 0; c < quadH(q); ++c) {
                                    const int wi = bx * BRICK_VOX + quadX(q) + c;
                                    const int wj = bz * BRICK_VOX + quadZ(q) + a;
                                    up[ckey2(wi, wj)].push_back({y, mtl});
                                }
                        }
                    }
        }
    printf("upward quads gathered: %zu over %zu columns\n\n", quads, up.size());

    // ---- every wet column, asked for its bed ------------------------------
    TerrainMemo memo;
    long long wet = 0, holes = 0, wrongY = 0, noLid = 0;
    std::unordered_map<int, long long> holeMat;
    const int MAP = 61;
    std::vector<char> map(MAP * MAP, ' ');
    int firstI = 0, firstJ = 0, firstBed = 0;
    bool haveFirst = false;
    int lidI = 0, lidJ = 0;
    bool haveLidHole = false;

    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            int line = 0;
            const bool isWet = terrain.lakeColumn(i, j, memo, &line);
            ++wet;
            const int bed = terrain.heightVox(i, j, memo);
            const auto it = up.find(ckey2(i, j));
            bool drawn = false, any = false, lid = false;
            if (it != up.end())
                for (const auto &f : it->second) {
                    if (f.second == mat::WATER || f.second == mat::FOAM) { lid = true; continue; }
                    any = true;
                    if (f.first == bed) drawn = true;
                }
            if (isWet && !lid) ++noLid;
            const int mi = int(float(i - i0) / float(i1 - i0 + 1) * MAP);
            const int mj = int(float(j - j0) / float(j1 - j0 + 1) * MAP);
            char &cell = map[mj * MAP + mi];
            if (isWet && !lid) {
                if (!haveLidHole) { lidI = i; lidJ = j; haveLidHole = true; }
                cell = 'W';
            }
            if (!drawn) {
                ++holes;
                if (any) ++wrongY;
                if (!haveFirst) { firstI = i; firstJ = j; firstBed = bed; haveFirst = true; }
                if (it != up.end())
                    for (const auto &f : it->second) ++holeMat[f.second];
                cell = 'X';
            } else if (cell != 'X') {
                cell = '.';
            }
        }

    printf("columns checked      %lld\n", wet);
    printf("NO TOP FACE DRAWN    %lld  (%.2f%%)\n", holes,
           wet ? 100.0 * double(holes) / double(wet) : 0.0);
    printf("  ...of which had SOME ground face, just not at the bed: %lld\n", wrongY);
    if (haveFirst) {
        int line = 0;
        terrain.lakeColumn(firstI, firstJ, memo, &line);
        printf("  first: (%d, %d) bed %d line %d  depth %.2f m\n", firstI, firstJ, firstBed, line,
               float(line - firstBed) * VOXEL_M);
        const auto it = up.find(ckey2(firstI, firstJ));
        if (it == up.end()) printf("  no upward face of ANY kind on that column\n");
        else
            for (const auto &f : it->second)
                printf("  drawn: y=%d material=%d\n", f.first, int(f.second));
    }
    if (!holeMat.empty()) {
        printf("  materials present on hole columns:");
        for (const auto &kv : holeMat) printf(" %d(x%lld)", kv.first, kv.second);
        printf("\n");
    }
    printf("\n  '.' bed drawn   'X' bed missing   (x east ->, z south v)\n");
    for (int m = 0; m < MAP; ++m) {
        printf("  ");
        for (int n = 0; n < MAP; ++n) putchar(map[m * MAP + n]);
        putchar('\n');
    }
    return holes ? 2 : 0;
}
