// ---------------------------------------------------------------------------
// voxel_store_test.cpp -- the 3D-keyed brick store, checked with no GPU and no
// window.
//
//   g++ -std=c++20 -O2 -I src tests/voxel_store_test.cpp -o build/voxel_store_test.exe
//
// voxel_test.cpp already proves the MESHER exact against a brute-force face
// set. What is unproven once a store exists is different and is the thing that
// actually breaks caches:
//
//   DOES THE INCREMENTAL PATH AGREE WITH THE FULL REBUILD, BYTE FOR BYTE?
//
// A store that re-meshes one brick after an edit is only correct if the world
// it produces is indistinguishable from the world a cold build would produce.
// Anything less and the difference accumulates: the player digs, the chunk is
// evicted at the ring edge, it comes back cold and LOOKS DIFFERENT. That is
// the failure mode a screenshot cannot find and byte-equality can, so it is
// what this file checks -- the same discipline the endless-world work used.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <vector>

#include "voxel/store.h"

using namespace v2;
using namespace v2::vox;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!ok) ++g_fail;
}

// Every brick of one chunk, flattened in key order, so two stores can be
// compared without caring how their hash tables happen to be arranged.
static std::vector<std::pair<uint64_t, std::vector<PackedQuad>>> snapshot(const BrickStore &s,
                                                                         int cx, int cz) {
    std::vector<std::pair<uint64_t, std::vector<PackedQuad>>> out;
    for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
        for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
            for (int by = -4; by < 24; ++by) {
                const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                if (const Brick *b = s.find(bx, by, bz))
                    out.push_back({brickKey(bx, by, bz), b->quads});
            }
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    return out;
}

static bool sameSnapshot(const std::vector<std::pair<uint64_t, std::vector<PackedQuad>>> &a,
                         const std::vector<std::pair<uint64_t, std::vector<PackedQuad>>> &b,
                         const char **why) {
    if (a.size() != b.size()) { *why = "different brick counts"; return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first) { *why = "different brick keys"; return false; }
        if (a[i].second != b[i].second) { *why = "quads differ in a brick"; return false; }
    }
    return true;
}

int main() {
    VoxelTerrain terrain;
    const int cx = 0, cz = 0;

    // ---- the key packs and unpacks, including negatives ------------------
    std::printf("=== key ===\n");
    {
        bool ok = true;
        const int probe[] = {0, 1, -1, 63, -64, 1000, -1000, 1048575, -1048576};
        for (int a : probe)
            for (int b : probe)
                for (int c : probe) {
                    int qx, qy, qz;
                    BrickStore::unpackBrick(brickKey(a, b, c), &qx, &qy, &qz);
                    if (qx != a || qy != b || qz != c) ok = false;
                }
        check(ok, "brickKey round-trips over the signed 21-bit range");
    }

    // ---- a cold chunk ------------------------------------------------------
    std::printf("\n=== cold build ===\n");
    EditStore edits;
    BrickStore store(&terrain, &edits);
    const int built = store.buildChunk(cx, cz);
    std::printf("  %d bricks, %zu quads, %zu stacks gathered\n", built, store.residentQuads(),
                store.gathers());
    check(built > 0, "a chunk builds some bricks");
    check(store.gathers() == size_t(CHUNK_BRICKS * CHUNK_BRICKS),
          "one gather per column stack, not per brick");
    check(store.residentQuads() > 0, "the bricks carry quads");

    // ---- an edit dirties the brick that owns it, and only that one ---------
    std::printf("\n=== an edit in a brick interior ===\n");
    {
        // A voxel comfortably inside one brick in x, y and z, so the 27-ring
        // collapses to a single key.
        const int i = cx * CHUNK_VOX + 32, j = cz * CHUNK_VOX + 32;
        const int y = terrain.heightVox(i, j);
        BrickStore s2(&terrain, &edits);
        s2.buildChunk(cx, cz);
        s2.touch(i, j, y);
        std::printf("  dirty %zu\n", s2.dirtyCount());
        check(s2.dirtyCount() == 1, "an interior voxel dirties exactly one brick");
    }

    // ---- ...and an edit on a brick face dirties the neighbour too ----------
    std::printf("\n=== an edit on a brick boundary ===\n");
    {
        const int i = cx * CHUNK_VOX + BRICK_VOX;  // first column of the next brick in x
        const int j = cz * CHUNK_VOX + 32;
        const int y = terrain.heightVox(i, j);
        BrickStore s2(&terrain, &edits);
        s2.touch(i, j, y);
        std::printf("  dirty %zu\n", s2.dirtyCount());
        check(s2.dirtyCount() == 2, "a voxel on a brick face dirties both sides");
    }

    // ---- THE ONE THAT MATTERS: incremental == cold, byte for byte ---------
    std::printf("\n=== incremental vs cold rebuild ===\n");
    {
        // Dig where somebody would actually swing: at the surface, mid-chunk.
        const int i = cx * CHUNK_VOX + 100, j = cz * CHUNK_VOX + 140;
        const int y = terrain.heightVox(i, j);

        EditStore ed;
        BrickStore inc(&terrain, &ed);
        inc.buildChunk(cx, cz);
        const size_t before = inc.residentQuads();

        ed.carve(i, j, y, 3);
        for (int dy = -4; dy <= 4; ++dy)
            for (int dj = -4; dj <= 4; ++dj)
                for (int di = -4; di <= 4; ++di) inc.touch(i + di, j + dj, y + dy);
        const int remeshed = inc.flush();
        const size_t after = inc.residentQuads();
        std::printf("  %d bricks re-meshed, quads %zu -> %zu\n", remeshed, before, after);
        check(after != before, "the dig changed the geometry");

        // The same world, built from nothing, with the edit already in place.
        BrickStore cold(&terrain, &ed);
        cold.buildChunk(cx, cz);

        const char *why = "";
        const bool same = sameSnapshot(snapshot(inc, cx, cz), snapshot(cold, cx, cz), &why);
        if (!same) std::printf("  (%s)\n", why);
        check(same, "incremental re-mesh is byte-identical to a cold rebuild");
    }

    // ---- a shaft into rock the surface test would have skipped ------------
    std::printf("\n=== digging into solid rock ===\n");
    {
        const int i = cx * CHUNK_VOX + 70, j = cz * CHUNK_VOX + 70;
        const int bi = floorDivI(i, BRICK_VOX), bj = floorDivI(j, BRICK_VOX);

        EditStore ed;
        BrickStore s2(&terrain, &ed);
        s2.buildChunk(cx, cz);

        // FIND A BRICK THE SURFACE TEST ACTUALLY SKIPPED rather than assuming
        // some depth is deep enough. brickRange carries a brick of slack at
        // each end, so "40 voxels down" was still inside the built range and
        // this test passed without ever exercising the path it names.
        int by = floorDivI(terrain.heightVox(i, j), BRICK_VOX);
        while (by > -4 && s2.find(bi, by, bj) != nullptr) --by;
        const int y = by * BRICK_VOX + BRICK_VOX / 2;  // mid-brick, in buried rock
        const bool absentBefore = s2.find(bi, by, bj) == nullptr;

        ed.carve(i, j, y, 3);
        s2.touch(i, j, y);
        s2.flush();
        const Brick *b = s2.find(bi, by, bj);
        std::printf("  brick y=%d absent before: %s, quads after: %zu\n", by,
                    absentBefore ? "yes" : "no", b ? b->quads.size() : 0);
        check(absentBefore && b != nullptr && !b->quads.empty(),
              "a pocket dug in buried rock brings its brick into existence");

        // ...and that brick must also match a cold build that knows the edit.
        BrickStore cold(&terrain, &ed);
        cold.buildChunk(cx, cz);
        const Brick *cb = cold.find(bi, by, bj);
        check(cb != nullptr && b != nullptr && cb->quads == b->quads,
              "the buried pocket is byte-identical to a cold rebuild");
    }

    // ---- eviction takes the chunk and leaves the stacks -------------------
    std::printf("\n=== eviction ===\n");
    {
        BrickStore s2(&terrain, &edits);
        s2.buildChunk(cx, cz);
        s2.buildChunk(cx + 1, cz);
        const size_t all = s2.residentBricks();
        s2.evictChunk(cx, cz);
        std::printf("  bricks %zu -> %zu, stacks held %zu\n", all, s2.residentBricks(),
                    s2.residentStacks());
        check(s2.residentBricks() > 0 && s2.residentBricks() < all,
              "evicting one chunk leaves the other standing");
        check(s2.residentStacks() == size_t(2 * CHUNK_BRICKS * CHUNK_BRICKS),
              "the gathered heightfield survives its chunk");
    }

    // ---- the LRU is bounded ----------------------------------------------
    std::printf("\n=== the stack LRU ===\n");
    {
        BrickStore s2(&terrain, &edits, /*stackCap=*/8);
        for (int c = 0; c < 4; ++c) s2.buildChunk(c, 0);
        std::printf("  stacks held %zu of a cap of 8\n", s2.residentStacks());
        check(s2.residentStacks() <= 8, "the stack cache respects its cap");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
