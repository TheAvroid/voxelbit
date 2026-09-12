// ---------------------------------------------------------------------------
// voxel_expand_test.cpp -- the quads, expanded to triangles, still describe the
// same world and still face outwards.
//
//   g++ -std=c++20 -O2 -I src tests/voxel_expand_test.cpp -o build/voxel_expand_test.exe
//
// voxel_parity_test proves the QUADS match the terrain function. Expansion is
// the step after that, and it can go wrong in two ways the quad test cannot
// see, because a PackedQuad has no vertices in it:
//
//   THE EXTENTS. W lies along the ROW axis and H along the BIT axis, and which
//   world axis each of those is depends on the direction. Swap them and the
//   coverage is transposed -- a run that should be 40 long in z becomes 40 in x.
//   That is what the coverage half below catches.
//
//   THE WINDING. The tracer takes a geometric normal from the triangle, so a
//   quad wound the other way is a surface lit from inside. It renders BLACK
//   rather than missing, which is the kind of fault that gets blamed on a
//   material and costs a day. That is what the normal half catches, and it is
//   why this file exists separately at all.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "voxel/expand.h"
#include "voxel/store.h"

using namespace v2;
using namespace v2::vox;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!ok) ++g_fail;
}

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

// The outward normal each face:: value is supposed to have.
static const int kNormal[6][3] = {{0, 1, 0},  {0, -1, 0}, {1, 0, 0},
                                  {-1, 0, 0}, {0, 0, 1},  {0, 0, -1}};

// Which world axis is flat for a direction, and whether the face sits on the
// voxel's far side.
static int flatAxis(uint8_t dir) {
    if (dir == face::POS_Y || dir == face::NEG_Y) return 1;
    if (dir == face::POS_X || dir == face::NEG_X) return 0;
    return 2;
}
static bool isPositive(uint8_t dir) {
    return dir == face::POS_Y || dir == face::POS_X || dir == face::POS_Z;
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
    const size_t quads = store.residentQuads();

    const VoxMesh m = expandChunk(store, cx, cz);
    std::printf("chunk (%d, %d): %zu quads -> %zu tris, %zu verts\n\n", cx, cz, quads,
                m.triCount(), m.position.size());

    check(m.triCount() == quads * 2, "every quad became exactly two triangles");
    check(m.tri.size() == m.triCount(), "one packed word per triangle");
    check(m.index.size() == m.triCount() * 3, "the index buffer matches the triangle count");

    // ---- the winding -----------------------------------------------------
    //
    // Each quad's first triangle is (base, base+1, base+2) and its outward
    // normal is (b - a) x (c - a). Compared by SIGN per axis rather than by
    // magnitude: these quads are axis-aligned, so exactly one component is
    // non-zero and its sign is the whole question.
    std::printf("=== winding ===\n");
    {
        size_t wrong = 0, checked = 0;
        int firstBad = -1;
        auto sign = [](float v) { return (v > 1e-9f) ? 1 : ((v < -1e-9f) ? -1 : 0); };
        for (size_t t = 0; t < m.triCount(); t += 2) {  // one test per quad
            const Vec3 a = m.position[m.index[t * 3]];
            const Vec3 b = m.position[m.index[t * 3 + 1]];
            const Vec3 c = m.position[m.index[t * 3 + 2]];
            const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const uint8_t dir = triFace(m.tri[t]);
            ++checked;
            if (sign(nx) != kNormal[dir][0] || sign(ny) != kNormal[dir][1] ||
                sign(nz) != kNormal[dir][2]) {
                if (!wrong++) firstBad = int(dir);
            }
        }
        std::printf("  %zu quads checked, %zu wound inside out\n", checked, wrong);
        if (wrong) std::printf("  (first bad direction: %d)\n", firstBad);
        check(wrong == 0, "every quad faces the way its direction says");
    }

    // ---- the coverage ----------------------------------------------------
    //
    // Walked off the TRIANGLES this time, not off the quads, so a mistake in
    // expandBrick cannot hide behind the packing it came from.
    std::printf("\n=== coverage ===\n");
    {
        const float s = VOXEL_M;
        std::unordered_map<uint64_t, uint8_t> got;
        size_t doubled = 0;
        for (size_t t = 0; t < m.triCount(); t += 2) {
            const uint32_t base = m.index[t * 3];
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int k = 0; k < 4; ++k) {
                const Vec3 p = m.position[base + k];
                const float v[3] = {p.x, p.y, p.z};
                for (int d = 0; d < 3; ++d) {
                    lo[d] = std::min(lo[d], v[d]);
                    hi[d] = std::max(hi[d], v[d]);
                }
            }
            const uint8_t dir = triFace(m.tri[t]);
            const uint8_t mtl = triMaterial(m.tri[t]);
            const int flat = flatAxis(dir);
            const bool pos = isPositive(dir);

            // Inclusive voxel range. On the flat axis a positive face sits on
            // the voxel's FAR side, so the plane index is one past the voxel.
            int v0[3], v1[3];
            for (int d = 0; d < 3; ++d) {
                const int q0 = int(std::lround(lo[d] / s)), q1 = int(std::lround(hi[d] / s));
                if (d == flat) v0[d] = v1[d] = pos ? q0 - 1 : q0;
                else { v0[d] = q0; v1[d] = q1 - 1; }
            }
            for (int x = v0[0]; x <= v1[0]; ++x)
                for (int y = v0[1]; y <= v1[1]; ++y)
                    for (int z = v0[2]; z <= v1[2]; ++z) {
                        const uint64_t k = faceKey(x, z, y, dir);
                        if (got.find(k) != got.end()) ++doubled;
                        got[k] = mtl;
                    }
        }
        std::printf("  %zu faces rasterised from triangles, %zu doubled\n", got.size(), doubled);
        check(doubled == 0, "no face is covered twice after expansion");

        // The same set as the quads carried, rebuilt from the store so the two
        // halves are derived independently.
        std::unordered_map<uint64_t, uint8_t> want;
        int byLo = 0, byHi = 0;
        store.chunkYRange(cx, cz, &byLo, &byHi);
        for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
            for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
                for (int by = byLo; by <= byHi; ++by) {
                    const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                    const Brick *b = store.find(bx, by, bz);
                    if (!b) continue;
                    for (PackedQuad q : b->quads) {
                        const uint8_t dir = quadDir(q);
                        for (int aa = 0; aa < quadW(q); ++aa)
                            for (int cc = 0; cc < quadH(q); ++cc) {
                                int x = quadX(q), y = quadY(q), z = quadZ(q);
                                switch (dir) {
                                    case face::POS_Y:
                                    case face::NEG_Y: z += aa; x += cc; break;
                                    case face::POS_X:
                                    case face::NEG_X: z += aa; y += cc; break;
                                    default: x += aa; y += cc; break;
                                }
                                want[faceKey(bx * BRICK_VOX + x, bz * BRICK_VOX + z,
                                             by * BRICK_VOX + y, dir)] = quadMaterial(q);
                            }
                    }
                }

        size_t missing = 0, stray = 0, wrongMat = 0;
        for (const auto &kv : want) {
            const auto it = got.find(kv.first);
            if (it == got.end()) ++missing;
            else if (it->second != kv.second) ++wrongMat;
        }
        for (const auto &kv : got)
            if (want.find(kv.first) == want.end()) ++stray;
        std::printf("  against the quads: %zu missing, %zu stray, %zu wrong material\n", missing,
                    stray, wrongMat);
        check(missing == 0 && stray == 0, "expansion covers exactly the quads' faces");
        check(wrongMat == 0, "expansion keeps each face's material");
        check(want.size() > 50000, "the chunk was substantial enough for this to mean something");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
