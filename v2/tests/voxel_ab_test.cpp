// ---------------------------------------------------------------------------
// voxel_ab_test.cpp -- meshChunk against the brick path, on the same chunk.
//
//   g++ -std=c++20 -O2 -I src tests/voxel_ab_test.cpp -o build/voxel_ab_test.exe
//
// ChunkMesher::useBricks switches between the two. Everything else in this
// suite checks the new path against a reference of its own making; this one
// checks it against THE CODE IT REPLACES, because that is what the flag
// actually swaps and therefore what a regression would look like.
//
// The triangles are expected to differ -- the brick path merges where
// meshChunk does not, which is the whole point. What must NOT differ is the
// SURFACE: the set of (voxel, direction) faces and the material on each. So
// both meshes are rasterised back down to faces and the two sets compared.
//
// A difference here is not automatically a bug in the bricks. It is a list of
// what flipping the flag changes, which is the thing worth knowing before it
// is flipped.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
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

struct Face {
    uint8_t mtl = 0, strand = 0;
};

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

static int flatAxis(uint8_t dir) {
    if (dir == face::POS_Y || dir == face::NEG_Y) return 1;
    if (dir == face::POS_X || dir == face::NEG_X) return 0;
    return 2;
}
static bool isPositive(uint8_t dir) {
    return dir == face::POS_Y || dir == face::POS_X || dir == face::POS_Z;
}

// Any VoxMesh, down to the voxel faces it covers. Both halves go through this
// one function so neither gets a rasteriser tuned to its own output.
static std::unordered_map<uint64_t, Face> toFaces(const VoxMesh &m, size_t *doubled) {
    std::unordered_map<uint64_t, Face> out;
    const float s = VOXEL_M;
    *doubled = 0;
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
        const int flat = flatAxis(dir);
        const bool pos = isPositive(dir);
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
                    if (out.find(k) != out.end()) ++(*doubled);
                    out[k] = Face{triMaterial(m.tri[t]), triStrand(m.tri[t])};
                }
    }
    return out;
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
    const std::shared_ptr<const ChunkEdits> ce = edits.get(cx, cz);

    ChunkScratch scratch;
    const VoxMesh oldM = terrain.meshChunk(cx, cz, scratch, ce.get());

    BrickStore store(&terrain, &edits);
    store.buildChunk(cx, cz);
    const VoxMesh newM = expandChunk(store, cx, cz);

    std::printf("chunk (%d, %d)\n", cx, cz);
    std::printf("  meshChunk   %8zu tris\n", oldM.triCount());
    std::printf("  bricks      %8zu tris\n", newM.triCount());

    // THE HONEST TRIANGLE COMPARISON, AND IT IS NOT THE ONE ABOVE.
    //
    // meshChunk draws grass blades and the brick path does not, so dividing
    // the two totals compares terrain+grass against terrain and calls the
    // missing grass a saving. The blades carry a non-zero strand code, which
    // is exactly what they are marked with, so they can be taken out of the
    // old count and the merge measured on the ground both paths actually draw.
    // BOTH SIDES SPLIT THE SAME WAY. Comparing totals is only honest once the
    // two paths draw the same things -- and while one of them was missing the
    // grass entirely, dividing the totals counted the absent grass as a saving
    // and reported 2.04x for what was really 1.27x. Split by the strand code,
    // which is exactly what marks a blade, and the merge can be read per kind.
    size_t oldTerrain = 0, oldBlades = 0, newTerrain = 0, newBlades = 0, newWater = 0;
    for (size_t t = 0; t < oldM.triCount(); ++t) {
        if (triStrand(oldM.tri[t])) ++oldBlades; else ++oldTerrain;
    }
    for (size_t t = 0; t < newM.triCount(); ++t) {
        if (triStrand(newM.tri[t])) ++newBlades;
        else if (triMaterial(newM.tri[t]) == mat::WATER) ++newWater;
        else ++newTerrain;
    }
    std::printf("    terrain  %7zu -> %7zu   %.2fx\n", oldTerrain, newTerrain,
                newTerrain ? double(oldTerrain) / double(newTerrain) : 0.0);
    std::printf("    blades   %7zu -> %7zu   %.2fx\n", oldBlades, newBlades,
                newBlades ? double(oldBlades) / double(newBlades) : 0.0);
    std::printf("    water    %7s -> %7zu   (meshChunk cannot draw it)\n", "0", newWater);

    size_t dOld = 0, dNew = 0;
    const auto fOld = toFaces(oldM, &dOld);
    const auto fNew = toFaces(newM, &dNew);
    std::printf("  meshChunk   %8zu faces (%zu doubled)\n", fOld.size(), dOld);
    std::printf("  bricks      %8zu faces (%zu doubled)\n\n", fNew.size(), dNew);

    // ---- what each path draws that the other does not ---------------------
    std::map<int, size_t> missingByMat, strayByMat;
    size_t missing = 0, stray = 0, wrongMat = 0, strandOnly = 0;
    for (const auto &kv : fOld) {
        const auto it = fNew.find(kv.first);
        if (it == fNew.end()) {
            ++missing;
            ++missingByMat[kv.second.mtl];
            if (kv.second.strand) ++strandOnly;
        } else if (it->second.mtl != kv.second.mtl) {
            ++wrongMat;
        }
    }
    size_t strayWater = 0;
    for (const auto &kv : fNew)
        if (fOld.find(kv.first) == fOld.end()) {
            ++stray;
            ++strayByMat[kv.second.mtl];
            if (kv.second.mtl == mat::WATER) ++strayWater;
        }

    std::printf("=== the difference ===\n");
    std::printf("  in meshChunk but not in the bricks: %zu (%zu of them grass blades)\n", missing,
                strandOnly);
    for (const auto &kv : missingByMat)
        std::printf("      material %3d : %zu\n", kv.first, kv.second);
    std::printf("  in the bricks but not in meshChunk: %zu (%zu of them water)\n", stray,
                strayWater);
    for (const auto &kv : strayByMat)
        std::printf("      material %3d : %zu\n", kv.first, kv.second);
    std::printf("  same face, different material:      %zu\n\n", wrongMat);

    check(dNew == 0, "the brick path covers no face twice");
    // ---------------------------------------------------------------------
    // WHAT THIS TEST ASSERTS, AND WHY IT IS NOT SYMMETRIC.
    //
    // NOTHING MAY BE MISSING. Every face meshChunk draws, the brick path must
    // draw too -- that is the whole promise of flipping useBricks, and it is
    // checked below with no exemptions now that the grass is in. While the
    // brick path had no strands this failed by 94,186 blade faces on one chunk.
    //
    // EXTRAS ARE A DIFFERENT MATTER, and there are two legitimate kinds:
    //
    //   WATER. meshChunk predates the lake and has no concept of mat::WATER.
    //   It cannot draw it, so every water face is an extra by construction.
    //
    //   FACES MESHCHUNK DROPS AROUND AN EDIT. Measured on chunk (-18,-16):
    //   138 ground faces, mean distance 11.8 voxels from the carve, every one
    //   confirmed by TerrainProbe as a face that SHOULD exist -- solid voxel,
    //   air neighbour. voxel_parity_test passes on the same chunk, which is the
    //   independent check that the bricks match the terrain FUNCTION there. So
    //   the old mesher is the one that is wrong, and the brick path is not
    //   allowed to hide that by asserting the two agree.
    //
    // Hence: missing must be zero, extras are reported and categorised.
    // ---------------------------------------------------------------------
    check(missing == 0, "the bricks draw every face the old mesher draws");
    std::printf("  (extras: %zu water, %zu ground -- the latter are faces meshChunk drops\n",
                strayWater, stray - strayWater);
    std::printf("   near an edit; see the note in this file)\n");
    check(wrongMat == 0, "every shared face keeps its material");
    // Kept as a separate line because it is the one that used to fail: while
    // the brick path had no strands, every missing face was a blade.
    check(strandOnly == 0, "no grass blade the old mesher drew is missing");

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
