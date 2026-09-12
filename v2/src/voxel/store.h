// ---------------------------------------------------------------------------
// store.h -- the bricks that are actually kept, filed under a 3D key.
//
// mesher.h turns one brick into quads and forgets it; columns.h answers what a
// brick contains and forgets that too. Neither of them is a STORE, and without
// one the 106x edit win measured in tests/voxel_bench.cpp is unreachable: the
// bench only sees it because it holds a ColumnStack across its loop, which is
// the very thing nothing in the engine was doing.
//
// So this file is the answer to "shouldn't a voxel game store voxels in data".
// It does now -- but what it stores is the part that is expensive to recompute,
// and that turns out NOT to be the occupancy bits:
//
// ---------------------------------------------------------------------------
// THE GATHER IS THE COST, AND IT IS THE THING WORTH CACHING.
//
// Measured on this terrain at chunk (0,0), min of 9: a chunk of bricks costs
// 15.72 ms to build and 14.23 ms of that is ColumnStack::gather -- the fractal
// noise behind heightVox and topMaterial. The bit arithmetic that replaced
// v2's per-voxel walk is the remaining 1.49 ms.
//
// That is the whole shape of the problem. Occupancy is (1 << (h+1)) - 1 and
// costs nothing to produce once h is known; h costs ~200 ns a column because
// it is six fbm fields deep. A store that kept occupancy bits and recomputed
// heights would be caching the free half. So what is filed here is:
//
//   ColumnStack   the gathered heightfield of one 64x64 column footprint,
//                 SHARED BY THE WHOLE VERTICAL STACK -- the field does not
//                 depend on y, so the bricks stacked above one another want
//                 the same 4,356 heights and would otherwise recompute them.
//
//   Brick         the merged quads, keyed in 3D. This is the unit an edit
//                 invalidates, and the reason an edit stops costing a chunk.
//
// ---------------------------------------------------------------------------
// WHY THE STACK CACHE IS BOUNDED AND THE BRICK CACHE IS NOT.
//
// A ColumnStack is 21 KB. The 12-chunk ring is 625 chunks of 4x4 stacks, so
// keeping every one resident is 10,000 stacks and 210 MB of heightfield for a
// world that is supposed to cost nothing when nobody has touched it. That
// trade is the wrong way round.
//
// It is also unnecessary, because of what a stack is FOR once its chunk is
// built: re-meshing a brick somebody edited. Edits happen within arm's reach.
// An LRU of a few hundred stacks therefore holds exactly the ground the player
// is standing on without being told where that is, and a stack evicted from
// under a distant edit costs one gather to bring back -- 14 ms, once, for
// something nobody can see happen.
//
// Bricks are small (a chunk of them is 0.26 MB of quads against v2's 4.97 MB
// of vertices) and are evicted with their chunk by the streamer, so they are
// held plainly.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "columns.h"
#include "mesher.h"
#include "quad.h"

namespace v2 {
namespace vox {

// A chunk must be a whole number of bricks. CHUNK_VOX is 256 and BRICK_VOX 64,
// so this holds -- asserted rather than assumed, because the two constants live
// in different files and a chunk that stopped being a whole number of bricks
// would fail as a seam of missing geometry rather than as an error.
static_assert(CHUNK_VOX % BRICK_VOX == 0, "a chunk must be a whole number of bricks");
static_assert(CHUNK_VOX / BRICK_VOX == CHUNK_BRICKS, "CHUNK_BRICKS disagrees with CHUNK_VOX");

// The 2D key of a column stack. Same 21-bit packing as brickKey and
// ChunkEdits::ckey, for the reason quad.h gives: two spellings of one idea is
// how a cache and its owner come to disagree about which brick they mean.
inline uint64_t stackKey(int bx, int bz) {
    return (uint64_t(uint32_t(bx) & 0x1FFFFFu) << 21) | uint64_t(uint32_t(bz) & 0x1FFFFFu);
}

// Floor division, so brick -1 holds voxel -1 rather than voxel 0. EditStore has
// the same function; it is restated because this header does not own that one
// and must not come to depend on the order the two are included.
inline int floorDivI(int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }

// ---------------------------------------------------------------------------
// One brick as it is kept: its merged quads, and a revision that changes every
// time they do.
//
// THE REVISION IS FOR THE GPU CACHE THAT DOES NOT EXIST YET. A brick whose
// quads have been uploaded is identified by (key, rev); re-meshing bumps rev
// and that is the whole staleness test, with no comparison of quad arrays and
// no flag anybody can forget to set. Four bytes now, and the question does not
// have to be reopened later.
// ---------------------------------------------------------------------------
struct Brick {
    std::vector<PackedQuad> quads;
    // Where the water's quads begin in the list above. Everything before it is
    // ground. Kept because the two are built by separate passes and a caller
    // that wants one without the other -- a collider, or a depth-sorted draw --
    // would otherwise have to test the material of every quad to find out.
    size_t water = 0;
    // ...and where the grass begins, after the water. Ground, then water, then
    // blades: three passes, one list.
    size_t strands = 0;
    uint32_t rev = 0;
    bool surface = false;  // whether the procedural stack said this could hold one
};

// ---------------------------------------------------------------------------
class BrickStore {
  public:
    BrickStore(const VoxelTerrain *terrain, const EditStore *edits, size_t stackCap = 512)
        : t_(terrain), ed_(edits), stackCap_(stackCap) {}

    // -----------------------------------------------------------------------
    // Build every brick of one chunk that can hold a surface.
    //
    // THE RESULT IS KEYED IN 3D AND THE REQUEST IS NOT. A caller asks for the
    // chunk column (cx, cz) and gets back every brick in it worth having --
    // typically two or three of the fifteen the world is tall, because
    // brickHasSurface rejects the solid rock below and the open air above.
    // Keying what comes back in 3D is the point; making the caller guess which
    // y to ask for would only move the surface search outwards.
    // -----------------------------------------------------------------------
    int buildChunk(int cx, int cz) {
        int built = 0;
        for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
            for (int sx = 0; sx < CHUNK_BRICKS; ++sx) {
                const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                const ColumnStack &st = stack(bx, bz);

                int byLo, byHi;
                st.brickRange(&byLo, &byHi);

                // The bricks the heightfield says hold a surface, PLUS any an
                // edit has reached. The second term is not a refinement:
                // somebody sinking a shaft into solid rock creates a surface
                // inside a brick the procedural range calls "solid through",
                // and skipping it leaves the shaft invisible while remaining
                // present to the collider -- the same failure the cave work hit
                // from the other direction, and the same fix.
                std::unordered_set<int> editedY;
                editedBricksIn(bx, bz, &editedY);
                for (int by : editedY) {
                    if (by < byLo) byLo = by;
                    if (by > byHi) byHi = by;
                }

                for (int by = byLo; by <= byHi; ++by) {
                    const bool surf = st.brickHasSurface(by);
                    if (!surf && editedY.find(by) == editedY.end()) continue;
                    meshInto(st, bx, by, bz, surf);
                    ++built;
                }
            }
        return built;
    }

    const Brick *find(int bx, int by, int bz) const {
        const auto it = bricks_.find(brickKey(bx, by, bz));
        return it == bricks_.end() ? nullptr : &it->second;
    }

    // The vertical extent this chunk actually has bricks over, inclusive.
    // False when it has none.
    //
    // MAINTAINED ON WRITE RATHER THAN SEARCHED. Anything walking a chunk's
    // bricks needs to know where to start, and the honest alternatives are both
    // bad: scan the whole brick table per chunk (it holds the entire ring), or
    // walk the full signed y range and miss on two million keys. A pair of ints
    // updated in meshInto costs nothing and cannot drift, because it is written
    // by the only function that creates a brick.
    bool chunkYRange(int cx, int cz, int *byLo, int *byHi) const {
        const auto it = chunkY_.find(ChunkEdits::ckey(cx, cz));
        if (it == chunkY_.end()) return false;
        *byLo = it->second.first;
        *byHi = it->second.second;
        return true;
    }

    // -----------------------------------------------------------------------
    // An edit landed at a world voxel. Dirty every brick that can see it.
    //
    // THE RING IS 3x3x3 AND THAT IS NOT BELT AND BRACES. A brick's face masks
    // are built from a PADDED footprint reaching one voxel outside it, so a
    // voxel on a brick boundary is read by the brick next door as well -- and
    // if only the owning brick were re-meshed, the neighbour would keep the
    // face it drew against rock that is no longer there. That is v2's own
    // "A HOLE IS VISIBLE FROM THE CHUNK NEXT DOOR" rule from EditStore::carve,
    // now in the axis a chunk never had.
    //
    // For a voxel in a brick interior all twenty-seven keys resolve to the same
    // brick and collapse in the set, so the common case costs one entry.
    // -----------------------------------------------------------------------
    void touch(int i, int j, int y) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    dirty_.insert(brickKey(floorDivI(i + dx, BRICK_VOX),
                                           floorDivI(y + dy, BRICK_VOX),
                                           floorDivI(j + dz, BRICK_VOX)));
    }

    // The bite EditStore::carve takes, dirtied -- THE SAME SPHERE, and that is
    // the point of it existing.
    //
    // A caller left to write this loop itself writes the enclosing CUBE, which
    // is what the first version of voxel_bench.cpp did: 729 voxels touched for
    // a carve that moved 123, six times the hash inserts, and an edit that
    // measured 0.27 ms when it costs rather less. Nothing was WRONG -- the
    // extra keys collapse onto the same bricks -- which is exactly why it went
    // unnoticed. The shape belongs next to the store, not in each caller.
    void touchCarve(int ci, int cj, int cy, int r) {
        const int r2 = r * r;
        for (int dy = -r; dy <= r; ++dy)
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di)
                    if (di * di + dj * dj + dy * dy <= r2) touch(ci + di, cj + dj, cy + dy);
    }

    // Re-mesh everything a touch dirtied. Returns how many bricks were rebuilt.
    //
    // A dirty brick that was never resident is built anyway: that is somebody
    // digging into ground whose brick the surface test had skipped, and it is
    // exactly the moment a new brick has to come into existence.
    int flush() {
        int n = 0;
        for (uint64_t k : dirty_) {
            int bx, by, bz;
            unpackBrick(k, &bx, &by, &bz);
            const ColumnStack &st = stack(bx, bz);
            meshInto(st, bx, by, bz, st.brickHasSurface(by));
            ++n;
        }
        dirty_.clear();
        return n;
    }

    size_t dirtyCount() const { return dirty_.size(); }

    // Drop a chunk's bricks. The stacks are left to the LRU: they belong to no
    // chunk, they are cheap to hold and expensive to rebuild, and a streamer
    // evicts and re-admits the same chunk constantly at a ring edge.
    void evictChunk(int cx, int cz) {
        const int bx0 = cx * CHUNK_BRICKS, bz0 = cz * CHUNK_BRICKS;
        for (auto it = bricks_.begin(); it != bricks_.end();) {
            int qx, qy, qz;
            unpackBrick(it->first, &qx, &qy, &qz);
            const bool mine = qx >= bx0 && qx < bx0 + CHUNK_BRICKS && qz >= bz0 &&
                              qz < bz0 + CHUNK_BRICKS;
            it = mine ? bricks_.erase(it) : std::next(it);
        }
        chunkY_.erase(ChunkEdits::ckey(cx, cz));
    }

    size_t residentBricks() const { return bricks_.size(); }
    size_t residentStacks() const { return stacks_.size(); }
    size_t residentQuads() const {
        size_t n = 0;
        for (const auto &kv : bricks_) n += kv.second.quads.size();
        return n;
    }
    size_t gathers() const { return gathers_; }

    // 21-bit signed unpack, the inverse of brickKey.
    static void unpackBrick(uint64_t k, int *bx, int *by, int *bz) {
        *bx = sext21(uint32_t((k >> 42) & 0x1FFFFFu));
        *by = sext21(uint32_t((k >> 21) & 0x1FFFFFu));
        *bz = sext21(uint32_t(k & 0x1FFFFFu));
    }

  private:
    static int sext21(uint32_t v) { return int(v << 11) >> 11; }

    void meshInto(const ColumnStack &st, int bx, int by, int bz, bool surf) {
        const ChunkEdits *ce = chunkEditsFor(bx, bz);
        TerrainColumns tc(*t_, st, ce, by);
        Brick &b = bricks_[brickKey(bx, by, bz)];
        b.quads.clear();
        meshBrick(tc, scratch_, &b.quads);

        // THE WATER, AS A SECOND PASS INTO THE SAME QUAD LIST. It is one
        // material among the brick's others from here on -- no second BLAS, no
        // instance kind, nothing downstream that has to know a lake from a
        // hillside. See WaterColumns for why it cannot share the first pass.
        //
        // The any() test walks the brick's columns before committing to a full
        // mesh, and it pays: well under one per cent of the world is wet, so
        // almost every brick answers no after a few columns.
        b.water = b.quads.size();
        WaterColumns wc(tc, st, by);
        if (wc.any()) meshBrick(wc, scratch_, &b.quads);

        // THE GRASS, AS A THIRD PASS. Same shape as the water and for the same
        // reason -- a blade is solid but must not hide the soil it stands on.
        // See StrandColumns.
        b.strands = b.quads.size();
        StrandColumns sc(tc, st, ce, by);
        if (sc.any()) meshBrick(sc, scratch_, &b.quads);
        b.surface = surf;
        ++b.rev;

        const uint64_t ck = ChunkEdits::ckey(floorDivI(bx, CHUNK_BRICKS), floorDivI(bz, CHUNK_BRICKS));
        const auto it = chunkY_.find(ck);
        if (it == chunkY_.end()) chunkY_[ck] = {by, by};
        else {
            if (by < it->second.first) it->second.first = by;
            if (by > it->second.second) it->second.second = by;
        }
    }

    // The edit set covering this brick's columns. A brick is 64 voxels and a
    // chunk 256, so a brick interior never straddles two chunks -- and its
    // padded ring, which does, is safe to read through this one set because
    // EditStore::carve mirrors every carved voxel into all nine neighbouring
    // chunks for precisely this reason.
    const ChunkEdits *chunkEditsFor(int bx, int bz) {
        if (!ed_) return nullptr;
        const int cx = floorDivI(bx, CHUNK_BRICKS), cz = floorDivI(bz, CHUNK_BRICKS);
        held_ = ed_->get(cx, cz);
        return held_.get();
    }

    // Which bricks in this stack an edit has reached, by brick y index.
    void editedBricksIn(int bx, int bz, std::unordered_set<int> *out) {
        const ChunkEdits *ce = chunkEditsFor(bx, bz);
        if (!ce) return;
        const int i0 = bx * BRICK_VOX, j0 = bz * BRICK_VOX;
        for (const auto &kv : ce->vox) {
            const int i = sext21(uint32_t((kv.first >> 42) & 0x1FFFFFu));
            const int j = sext21(uint32_t((kv.first >> 21) & 0x1FFFFFu));
            if (i < i0 - 1 || i > i0 + BRICK_VOX || j < j0 - 1 || j > j0 + BRICK_VOX) continue;
            const int y = sext21(uint32_t(kv.first & 0x1FFFFFu));
            out->insert(floorDivI(y, BRICK_VOX));
        }
    }

    // -----------------------------------------------------------------------
    // The gathered heightfield of one column footprint, LRU by use.
    // -----------------------------------------------------------------------
    const ColumnStack &stack(int bx, int bz) {
        const uint64_t k = stackKey(bx, bz);
        const auto it = stacks_.find(k);
        if (it != stacks_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);  // most recently used
            return it->second->second;
        }
        lru_.emplace_front(k, ColumnStack{});
        lru_.front().second.gather(*t_, memo_, bx, bz);
        ++gathers_;
        stacks_[k] = lru_.begin();
        if (stacks_.size() > stackCap_) {
            stacks_.erase(lru_.back().first);
            lru_.pop_back();
        }
        return lru_.front().second;
    }

    const VoxelTerrain *t_ = nullptr;
    const EditStore *ed_ = nullptr;
    std::shared_ptr<const ChunkEdits> held_;  // keeps chunkEditsFor's return alive
    size_t stackCap_;

    std::unordered_map<uint64_t, Brick> bricks_;
    std::unordered_map<uint64_t, std::pair<int, int>> chunkY_;  // see chunkYRange
    std::list<std::pair<uint64_t, ColumnStack>> lru_;
    std::unordered_map<uint64_t, std::list<std::pair<uint64_t, ColumnStack>>::iterator> stacks_;
    std::unordered_set<uint64_t> dirty_;

    TerrainMemo memo_;
    MeshScratch scratch_;
    size_t gathers_ = 0;
};

}  // namespace vox
}  // namespace v2
