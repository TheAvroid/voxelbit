// ---------------------------------------------------------------------------
// voxel_test.cpp -- the new voxel stack, checked with no GPU and no window.
//
// src/voxel/ is deliberately free of Falcor, D3D12 and Slang, so the part of
// this rewrite most likely to be wrong -- the bit arithmetic in the mesher --
// can be compiled by a bare g++ and answered in a second, rather than through
// a five-minute build and a screenshot. Same reasoning as the scatter harness
// over in tools/.
//
// Build and run:
//   g++ -std=c++20 -O2 -I src tests/voxel_test.cpp -o build/voxel_test && ./build/voxel_test
//
// THE MESHER TEST IS AN EXACT ONE, not a smoke test. It rebuilds the face set
// by brute force -- six neighbour lookups for every one of the 262,144 voxels
// -- rasterises every emitted quad back down to the voxel faces it claims, and
// requires the two sets to be IDENTICAL. That catches the three things that go
// wrong with a merge: a face missed, a face emitted twice by overlapping
// quads, and a quad that has drifted onto a face nobody asked for.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "voxel/mesher.h"
#include "voxel/quad.h"

using namespace v2::vox;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++g_fail;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// A dense source, padded by one column on each horizontal side, which is what
// the mesher asks for. Deliberately the dumbest possible implementation: this
// is the reference the clever one is checked against.
// ---------------------------------------------------------------------------
struct DenseSource {
    static constexpr int P = BRICK_VOX + 2;  // 66, padded
    std::vector<uint8_t> solid;              // [z+1][x+1][y]
    std::vector<uint8_t> mtl;
    bool above = false, below = false;

    DenseSource() : solid(size_t(P) * P * BRICK_VOX, 0), mtl(size_t(P) * P * BRICK_VOX, 1) {}

    size_t idx(int x, int y, int z) const {
        return (size_t(z + 1) * P + size_t(x + 1)) * BRICK_VOX + size_t(y);
    }
    void set(int x, int y, int z, uint8_t m) {
        solid[idx(x, y, z)] = 1;
        mtl[idx(x, y, z)] = m;
    }
    bool at(int x, int y, int z) const {
        if (y < 0 || y >= BRICK_VOX) return false;
        if (x < -1 || x > BRICK_VOX || z < -1 || z > BRICK_VOX) return false;
        return solid[idx(x, y, z)] != 0;
    }

    // --- what the mesher consumes ---
    Column column(int x, int z) const {
        Column c = 0;
        for (int y = 0; y < BRICK_VOX; ++y)
            if (at(x, y, z)) c |= Column(1) << y;
        return c;
    }
    // Nothing here is water, so what covers a voxel is what it is made of.
    Column occluder(int x, int z) const { return column(x, z); }
    bool solidAbove(int, int) const { return above; }
    bool solidBelow(int, int) const { return below; }
    uint8_t material(int x, int y, int z) const { return mtl[idx(x, y, z)]; }
    uint8_t strand(int, int, int) const { return 0; }
};

// Reverse of planeToLocal, so a quad can be walked back to its voxels.
static void localToPlane(uint8_t dir, int x, int y, int z, int *plane, int *row, int *bit) {
    switch (dir) {
        case mdir::POS_Y:
        case mdir::NEG_Y: *plane = y; *row = z; *bit = x; break;
        case mdir::POS_X:
        case mdir::NEG_X: *plane = x; *row = z; *bit = y; break;
        default: *plane = z; *row = x; *bit = y; break;
    }
}

// ---------------------------------------------------------------------------
// Mesh a source, then prove the quads describe exactly the faces the brute
// force pass finds.
// ---------------------------------------------------------------------------
static void verify(const char *name, const DenseSource &src, size_t *quadsOut = nullptr) {
    static MeshScratch scratch;
    std::vector<PackedQuad> quads;
    meshBrick(src, scratch, &quads);

    // --- brute force: which (voxel, dir) pairs are faces ---
    const int N = BRICK_VOX;
    std::vector<uint8_t> want(size_t(N) * N * N * 6, 0);
    auto fidx = [&](int x, int y, int z, int d) {
        return ((size_t(z) * N + size_t(x)) * N + size_t(y)) * 6 + size_t(d);
    };
    static const int dx[6] = {0, 0, 1, -1, 0, 0};
    static const int dy[6] = {1, -1, 0, 0, 0, 0};
    static const int dz[6] = {0, 0, 0, 0, 1, -1};
    size_t wantCount = 0;
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x)
            for (int y = 0; y < N; ++y) {
                if (!src.at(x, y, z)) continue;
                for (int d = 0; d < 6; ++d) {
                    const int nx = x + dx[d], ny = y + dy[d], nz = z + dz[d];
                    bool nSolid;
                    if (ny >= N) nSolid = src.above;
                    else if (ny < 0) nSolid = src.below;
                    else nSolid = src.at(nx, ny, nz);
                    if (!nSolid) { want[fidx(x, y, z, d)] = 1; ++wantCount; }
                }
            }

    // --- rasterise the quads back down ---
    std::vector<uint8_t> got(size_t(N) * N * N * 6, 0);
    size_t doubled = 0, stray = 0, area = 0;
    for (PackedQuad q : quads) {
        const uint8_t dir = quadDir(q);
        const int w = quadW(q), h = quadH(q);
        int plane, row, bit;
        localToPlane(dir, quadX(q), quadY(q), quadZ(q), &plane, &row, &bit);
        area += size_t(w) * size_t(h);
        for (int dr = 0; dr < w; ++dr)
            for (int db = 0; db < h; ++db) {
                int x, y, z;
                planeToLocal(dir, plane, row + dr, bit + db, &x, &y, &z);
                if (x < 0 || x >= N || y < 0 || y >= N || z < 0 || z >= N) { ++stray; continue; }
                if (got[fidx(x, y, z, dir)]) ++doubled;
                got[fidx(x, y, z, dir)] = 1;
                if (!want[fidx(x, y, z, dir)]) ++stray;
            }
    }
    size_t missing = 0;
    for (size_t i = 0; i < want.size(); ++i)
        if (want[i] && !got[i]) ++missing;

    std::printf("%s: %zu quads, %zu faces (%.2fx merge)\n", name, quads.size(), wantCount,
                quads.size() ? double(wantCount) / double(quads.size()) : 0.0);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: no missing faces (%zu)", name, missing);
    check(missing == 0, buf);
    std::snprintf(buf, sizeof buf, "%s: no doubled faces (%zu)", name, doubled);
    check(doubled == 0, buf);
    std::snprintf(buf, sizeof buf, "%s: no stray faces (%zu)", name, stray);
    check(stray == 0, buf);
    std::snprintf(buf, sizeof buf, "%s: area matches face count (%zu vs %zu)", name, area,
                  wantCount);
    check(area == wantCount, buf);
    if (quadsOut) *quadsOut = quads.size();
}

int main() {
    std::printf("=== mesher ===\n");

    // A flat floor. The merge should collapse each of the two horizontal
    // surfaces to a single quad -- if it does not, the row extension is broken.
    {
        DenseSource s;
        s.below = true;  // sitting on more world, so no down faces
        for (int z = 0; z < BRICK_VOX; ++z)
            for (int x = 0; x < BRICK_VOX; ++x)
                for (int y = 0; y <= 10; ++y) s.set(x, y, z, 2);
        // Solid padding: no side faces at the brick edge.
        for (int z = -1; z <= BRICK_VOX; ++z)
            for (int x = -1; x <= BRICK_VOX; ++x)
                if (x < 0 || x >= BRICK_VOX || z < 0 || z >= BRICK_VOX)
                    for (int y = 0; y <= 10; ++y) s.set(x, y, z, 2);
        size_t n = 0;
        verify("flat floor", s, &n);
        check(n == 1, "flat floor merges to exactly one quad");
    }

    // One voxel alone: six faces, six quads, and nothing merged.
    {
        DenseSource s;
        s.set(20, 20, 20, 3);
        size_t n = 0;
        verify("single voxel", s, &n);
        check(n == 6, "single voxel is exactly six quads");
    }

    // A step, which is what a terrain column actually looks like. Exercises
    // the vertical run merge that turns a ten-voxel drop into one quad a band.
    {
        DenseSource s;
        s.below = true;
        for (int z = 0; z < BRICK_VOX; ++z)
            for (int x = 0; x < BRICK_VOX; ++x) {
                const int h = (x < 32) ? 10 : 24;
                for (int y = 0; y <= h; ++y) s.set(x, y, z, 2);
            }
        verify("step", s);
    }

    // Two materials meeting. The merge must NOT cross the boundary, and the
    // two halves must each still merge internally.
    {
        DenseSource s;
        s.below = true;
        for (int z = 0; z < BRICK_VOX; ++z)
            for (int x = 0; x < BRICK_VOX; ++x)
                for (int y = 0; y <= 10; ++y) s.set(x, y, z, (z < 32) ? 2 : 5);
        for (int z = -1; z <= BRICK_VOX; ++z)
            for (int x = -1; x <= BRICK_VOX; ++x)
                if (x < 0 || x >= BRICK_VOX || z < 0 || z >= BRICK_VOX)
                    for (int y = 0; y <= 10; ++y) s.set(x, y, z, 2);
        size_t n = 0;
        verify("two materials", s, &n);
        check(n == 2, "a material seam splits the floor into exactly two quads");
    }

    // A full brick with empty padding: the six sides, nothing inside. This is
    // the case that catches an interior face leaking out of the hulling.
    {
        DenseSource s;
        for (int z = 0; z < BRICK_VOX; ++z)
            for (int x = 0; x < BRICK_VOX; ++x)
                for (int y = 0; y < BRICK_VOX; ++y) s.set(x, y, z, 7);
        size_t n = 0;
        verify("solid brick", s, &n);
        check(n == 6, "a solid brick is six quads, not 393216");
    }

    // A hollow shell -- interior air, so the inward-facing surfaces are real.
    {
        DenseSource s;
        for (int z = 0; z < BRICK_VOX; ++z)
            for (int x = 0; x < BRICK_VOX; ++x)
                for (int y = 0; y < BRICK_VOX; ++y) {
                    const bool edge = x < 2 || x >= BRICK_VOX - 2 || z < 2 ||
                                      z >= BRICK_VOX - 2 || y < 2 || y >= BRICK_VOX - 2;
                    if (edge) s.set(x, y, z, 4);
                }
        verify("hollow shell", s);
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
