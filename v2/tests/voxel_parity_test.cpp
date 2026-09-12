// ---------------------------------------------------------------------------
// voxel_parity_test.cpp -- the brick path against the world function itself.
//
//   g++ -std=c++20 -O2 -I src tests/voxel_parity_test.cpp -o build/voxel_parity_test.exe
//
// voxel_test.cpp proves the MESHER exact on synthetic grids. voxel_store_test
// proves the STORE agrees with itself. Neither touches the question that has
// to be answered before any of this drives gpu/world.h:
//
//   DOES THE BRICK PATH DRAW THE SAME WORLD THE TERRAIN FUNCTION DESCRIBES?
//
// Everything between the two is adapter -- the padded ring across a brick
// seam, the material at a boundary column, the edit overlay, the surface test
// that decides a brick is not worth meshing. Those are where a rewrite puts
// its holes, and they are invisible to both existing tests because both supply
// their own idea of what a voxel is.
//
// So this one does not. It asks VoxelTerrain directly for every voxel of every
// brick the store built, derives the face set by brute force, and requires the
// store's merged quads to rasterise back to EXACTLY that -- same faces, same
// materials, none missing, none doubled, none invented.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "voxel/store.h"

using namespace v2;
using namespace v2::vox;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!ok) ++g_fail;
}

// A face is a voxel and a direction. The material rides along as the value, so
// a disagreement about WHAT a surface is made of fails as loudly as a missing
// one -- that is the failure a merged quad hides best.
// ---------------------------------------------------------------------------
// A face is a voxel and a direction, packed into one key.
//
// TWENTY BITS A COMPONENT, NOT TWENTY-ONE, AND THE ARITHMETIC IS THE REASON.
// Three 21-bit fields plus three bits of direction is 66 bits and does not fit.
// The first version of this packed them at shifts 43 / 22 / 3 anyway, so y's
// top two bits LANDED ON TOP OF j's bottom two. Both sides of every comparison
// used the same function, so the tests still passed -- which is exactly the
// problem: two genuinely different faces could collide onto one key and a real
// disagreement would be silently absorbed.
//
// 20 bits signed is +-524,288 voxels, or +-52 km at 10 cm. The world runs out
// of float precision long before it runs out of key.
// ---------------------------------------------------------------------------
static constexpr int FK_BITS = 20;
static constexpr uint64_t FK_MASK = (1ull << FK_BITS) - 1ull;

static uint64_t faceKey(int i, int j, int y, uint8_t dir) {
    return ((uint64_t(uint32_t(i)) & FK_MASK) << 43) | ((uint64_t(uint32_t(j)) & FK_MASK) << 23) |
           ((uint64_t(uint32_t(y)) & FK_MASK) << 3) | uint64_t(dir & 7u);
}

// Sign-extend one field back out of a key.
static int fkPart(uint64_t k, int shift) {
    const uint32_t v = uint32_t((k >> shift) & FK_MASK);
    return int(v << (32 - FK_BITS)) >> (32 - FK_BITS);
}

// ---------------------------------------------------------------------------
// The chunk's heightfield and surface materials, gathered once so the truth
// pass can ask about a voxel in constant time. Without this the brute force is
// 12 million columns of fractal noise and the test takes minutes instead of a
// second -- the same reason ColumnStack exists.
// ---------------------------------------------------------------------------
struct Truth {
    static constexpr int P = CHUNK_VOX + 2;
    // Heights padded by TWO: a surface material reads the slope one column each
    // way, and the blade tests below ask about materials one column outside the
    // chunk. Same reason ColumnStack carries PADH.
    static constexpr int PH = CHUNK_VOX + 4;
    int i0, j0;
    std::vector<int> h;          // indexed by idxH, padded by two
    std::vector<uint8_t> top;    // indexed by idx, padded by one
    std::vector<uint8_t> sr;     // blade rows, indexed by idx
    // The lake line over each padded column, kNoWaterVox where none.
    std::vector<int> wy;
    const EditStore *ed;
    std::shared_ptr<const ChunkEdits> ce;

    Truth(const VoxelTerrain &t, const EditStore *edits, int cx, int cz)
        : i0(cx * CHUNK_VOX), j0(cz * CHUNK_VOX), h(size_t(PH) * PH), top(size_t(P) * P),
          sr(size_t(P) * P), wy(size_t(P) * P), ed(edits) {
        TerrainMemo memo;
        for (int lz = -2; lz <= CHUNK_VOX + 1; ++lz)
            for (int lx = -2; lx <= CHUNK_VOX + 1; ++lx)
                h[idxH(lx, lz)] = t.heightVox(i0 + lx, j0 + lz, memo);
        for (int lz = -1; lz <= CHUNK_VOX; ++lz)
            for (int lx = -1; lx <= CHUNK_VOX; ++lx) {
                wy[idx(lx, lz)] = t.lakeLineAt(t.wx(i0 + lx), t.wx(j0 + lz), memo);
                const int slope = std::max(std::abs(h[idxH(lx + 1, lz)] - h[idxH(lx - 1, lz)]),
                                           std::abs(h[idxH(lx, lz + 1)] - h[idxH(lx, lz - 1)]));
                const uint8_t tm =
                    t.topMaterial(i0 + lx, j0 + lz, h[idxH(lx, lz)], slope, memo);
                top[idx(lx, lz)] = tm;
                sr[idx(lx, lz)] = t.strandRows(i0 + lx, j0 + lz, tm);
            }
        if (ed) ce = ed->get(cx, cz);
    }

    size_t idx(int lx, int lz) const { return size_t(lz + 1) * P + size_t(lx + 1); }
    size_t idxH(int lx, int lz) const { return size_t(lz + 2) * PH + size_t(lx + 2); }
    bool inPad(int lx, int lz) const {
        return lx >= -1 && lx <= CHUNK_VOX && lz >= -1 && lz <= CHUNK_VOX;
    }

    // WATER, DERIVED THE SAME WAY THE STORE DERIVES IT but written out
    // independently: bed + 1 up to the line, and only where the ground is
    // under the line at all.
    //
    // AN EDITED VOXEL IS NOT WATER, whatever the span says. The water does not
    // FLOW into a hole -- the line is a function of the landform and a dig does
    // not move it -- but it must not paint OVER one either. That distinction
    // cost 71 faces: digging up into a lake left voxels the mesher drew as
    // water and TerrainProbe, which checks edits first, called air. Found by
    // tests/voxel_probe_test.cpp, and this is the same rule stated
    // independently so the two cannot quietly re-converge on a wrong answer.
    bool edited(int lx, int lz, int ly) const {
        if (!ce) return false;
        uint8_t m = 0;
        return ce->voxel(i0 + lx, j0 + lz, ly, &m);
    }
    bool water(int lx, int ly, int lz) const {
        const int line = wy[idx(lx, lz)];
        if (line == VoxelTerrain::kNoWaterVox) return false;
        const int hc = h[idxH(lx, lz)];
        if (hc > line) return false;
        if (ly <= hc || ly > line) return false;
        return !edited(lx, lz, ly);
    }

    // A BLADE, derived independently of StrandColumns. Rows come from the
    // terrain's own strandRows (one definition, deliberately shared -- a second
    // copy of the hash is how the two meshers come to disagree about where the
    // grass is), but the EDIT rule and the geometry are written out again here.
    int bladeRows(int lx, int lz) const {
        if (ce) {
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    int lo = 0, hi = 0;
                    if (ce->column(i0 + lx + di, j0 + lz + dj, &lo, &hi)) return 0;
                }
        }
        return sr[idx(lx, lz)];
    }
    bool blade(int lx, int ly, int lz) const {
        const int rows = bladeRows(lx, lz);
        if (rows == 0) return false;
        const int hc = h[idxH(lx, lz)];
        return ly > hc && ly <= hc + rows;
    }

    bool solid(const VoxelTerrain &t, int lx, int ly, int lz) const {
        if (ce) {
            uint8_t m = 0;
            if (ce->voxel(i0 + lx, j0 + lz, ly, &m)) return m != mat::AIR;
        }
        return ly <= h[idxH(lx, lz)];
    }
    uint8_t material(const VoxelTerrain &t, int lx, int ly, int lz) const {
        if (ce) {
            uint8_t m = 0;
            if (ce->voxel(i0 + lx, j0 + lz, ly, &m)) return m;
        }
        return t.materialAt(i0 + lx, j0 + lz, ly, h[idxH(lx, lz)], top[idx(lx, lz)]);
    }
};

// The six directions, in v2's face:: order, as (dx, dy, dz).
static const int kStep[6][3] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

int main(int argc, char **argv) {
    const int cx = (argc > 1) ? std::atoi(argv[1]) : 0;
    const int cz = (argc > 2) ? std::atoi(argv[2]) : 0;

    VoxelTerrain terrain;
    EditStore edits;

    // A dig and a buried pocket, so the edit overlay is under test too rather
    // than only the pristine heightfield.
    {
        const int i = cx * CHUNK_VOX + 100, j = cz * CHUNK_VOX + 140;
        edits.carve(i, j, terrain.heightVox(i, j), 4);
        edits.carve(i + 30, j - 20, terrain.heightVox(i + 30, j - 20) - 25, 5);
    }

    BrickStore store(&terrain, &edits);
    const int built = store.buildChunk(cx, cz);
    std::printf("chunk (%d, %d): %d bricks, %zu quads\n\n", cx, cz, built, store.residentQuads());

    Truth truth(terrain, &edits, cx, cz);

    // ---- what the world says ---------------------------------------------
    // MATERIAL IN THE LOW BYTE, STRAND CODE IN THE HIGH ONE. Comparing the
    // material alone would let every blade in the world wear its neighbour's
    // gradient without a single test noticing.
    std::unordered_map<uint64_t, uint16_t> want;
    want.reserve(1u << 20);
    size_t voxelsWalked = 0;
    for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
        for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
            for (int by = -8; by < 32; ++by) {
                const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                if (!store.find(bx, by, bz)) continue;  // the store skipped it; so do we
                const int lx0 = sx * BRICK_VOX, lz0 = sz * BRICK_VOX, y0 = by * BRICK_VOX;
                for (int z = 0; z < BRICK_VOX; ++z)
                    for (int x = 0; x < BRICK_VOX; ++x)
                        for (int y = 0; y < BRICK_VOX; ++y) {
                            const int lx = lx0 + x, lz = lz0 + z, wy = y0 + y;
                            ++voxelsWalked;
                            // THE GROUND. Water counts as AIR here, which is
                            // what keeps the lake bed visible through it.
                            if (truth.solid(terrain, lx, wy, lz)) {
                                for (uint8_t d = 0; d < 6; ++d) {
                                    const int nx = lx + kStep[d][0], ny = wy + kStep[d][1],
                                              nz = lz + kStep[d][2];
                                    if (truth.solid(terrain, nx, ny, nz)) continue;
                                    want[faceKey(truth.i0 + lx, truth.j0 + lz, wy, d)] =
                                        truth.material(terrain, lx, wy, lz);
                                }
                                continue;
                            }
                            // THE WATER. A face only where it meets air --
                            // neither its own body nor the bed it lies in.
                            if (truth.water(lx, wy, lz)) {
                                for (uint8_t d = 0; d < 6; ++d) {
                                    const int nx = lx + kStep[d][0], ny = wy + kStep[d][1],
                                              nz = lz + kStep[d][2];
                                    if (truth.solid(terrain, nx, ny, nz)) continue;
                                    if (truth.water(nx, ny, nz)) continue;
                                    want[faceKey(truth.i0 + lx, truth.j0 + lz, wy, d)] =
                                        mat::WATER;
                                }
                                continue;
                            }
                            // A BLADE. Hidden by the soil and by its
                            // neighbours' blades, never by water -- grass does
                            // not grow in a lake, so the two never meet.
                            if (truth.blade(lx, wy, lz)) {
                                const uint16_t key =
                                    uint16_t(truth.top[truth.idx(lx, lz)]) |
                                    (uint16_t(strandCodeFor(truth.h[truth.idxH(lx, lz)] + 1))
                                     << 8);
                                for (uint8_t d = 0; d < 6; ++d) {
                                    const int nx = lx + kStep[d][0], ny = wy + kStep[d][1],
                                              nz = lz + kStep[d][2];
                                    if (truth.solid(terrain, nx, ny, nz)) continue;
                                    if (truth.blade(nx, ny, nz)) continue;
                                    want[faceKey(truth.i0 + lx, truth.j0 + lz, wy, d)] = key;
                                }
                            }
                        }
            }
    std::printf("brute force: %zu voxels walked, %zu faces\n", voxelsWalked, want.size());

    // ---- what the store drew ---------------------------------------------
    std::unordered_map<uint64_t, uint16_t> got;
    got.reserve(want.size() * 2);
    size_t doubled = 0, quads = 0;
    for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
        for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
            for (int by = -8; by < 32; ++by) {
                const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                const Brick *b = store.find(bx, by, bz);
                if (!b) continue;
                for (PackedQuad q : b->quads) {
                    ++quads;
                    const uint8_t dir = quadDir(q);
                    const int w = quadW(q), hh = quadH(q);
                    // The plane table from mesher.h, run backwards: W lies
                    // along the ROW axis and H along the BIT axis, and which
                    // world axis each of those is depends on the direction.
                    for (int a = 0; a < w; ++a)
                        for (int c = 0; c < hh; ++c) {
                            int x = quadX(q), y = quadY(q), z = quadZ(q);
                            switch (dir) {
                                case face::POS_Y:
                                case face::NEG_Y: z += a; x += c; break;
                                case face::POS_X:
                                case face::NEG_X: z += a; y += c; break;
                                default: x += a; y += c; break;
                            }
                            const uint64_t k = faceKey(bx * BRICK_VOX + x, bz * BRICK_VOX + z,
                                                       by * BRICK_VOX + y, dir);
                            if (got.find(k) != got.end()) ++doubled;
                            got[k] = uint16_t(quadMaterial(q)) | (uint16_t(quadStrand(q)) << 8);
                        }
                }
            }
    std::printf("store:       %zu quads, %zu faces\n\n", quads, got.size());

    // ---- and they must be the same set -----------------------------------
    size_t missing = 0, stray = 0, wrongMat = 0;
    uint64_t firstMissing = 0, firstStray = 0, firstWrong = 0;
    for (const auto &kv : want) {
        const auto it = got.find(kv.first);
        if (it == got.end()) { if (!missing++) firstMissing = kv.first; }
        else if (it->second != kv.second) { if (!wrongMat++) firstWrong = kv.first; }
    }
    for (const auto &kv : got)
        if (want.find(kv.first) == want.end()) { if (!stray++) firstStray = kv.first; }

    auto show = [](const char *label, size_t n, uint64_t k) {
        if (!n) return;
        std::printf("    %s: %zu (first at voxel %d,%d,%d dir %d)\n", label, n,
                    fkPart(k, 43), fkPart(k, 23), fkPart(k, 3), int(k & 7));
    };
    show("missing", missing, firstMissing);
    show("stray", stray, firstStray);
    show("wrong material", wrongMat, firstWrong);

    check(missing == 0, "no face the world has is missing from the bricks");
    check(stray == 0, "no face the bricks draw is absent from the world");
    check(doubled == 0, "no face is covered by two quads");
    check(wrongMat == 0,
          "every face carries the material AND the strand code the world gives it");
    check(want.size() > 50000, "the chunk was substantial enough for this to mean something");

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
