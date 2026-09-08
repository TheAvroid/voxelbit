// ---------------------------------------------------------------------------
// edits.h -- the mutable overlay. What the player has taken out of the world.
//
// ---------------------------------------------------------------------------
// WHY AN OVERLAY AND NOT A VOXEL ARRAY.
//
// v2's world is a PURE FUNCTION -- heightVox(i, j) and materialAt(i, j, y) --
// and that is not an implementation detail, it is the reason the wood can be
// endless. A chunk is recomputed identically whenever it is needed, so it can
// be thrown away and rebuilt with nothing stored and nothing streamed. Storing
// the world as voxels instead would mean generating and keeping every chunk
// anyone has ever stood near, for ever.
//
// So the world stays a function and the EDITS are stored beside it. The pair
// answers "what is at this voxel" as: the function, unless this overlay has
// something to say. An untouched wood costs one empty hash map; a wood somebody
// has dug a hole in costs the hole.
//
// ---------------------------------------------------------------------------
// SNAPSHOTS, BECAUSE THE MESHER IS A THREAD POOL.
//
// Chunks are meshed on workers while the main thread plays the game, and a
// mesher asks about a quarter of a million voxels per chunk. Two things follow:
//
//   * A lock per query is out of the question -- that is the query, not an
//     occasional check around it.
//   * A plain shared map is a data race, because the player can swing a pick
//     while a worker is halfway through a chunk.
//
// So an edit does not mutate anything a worker can see. Each chunk's edits are
// an IMMUTABLE set behind a shared_ptr; carving builds a new one and swaps the
// pointer. A worker takes the pointer once, under one lock, at the top of the
// chunk it is meshing, and then reads it freely for the rest of the job with no
// synchronisation at all. Whatever it saw is a consistent view of some instant,
// which is exactly what a mesh needs to be -- and a carve that lands mid-job is
// picked up by the re-mesh that carve requested anyway.
//
// Copy-on-write is affordable here for the same reason the overlay is: a chunk
// holds the voxels somebody actually dug out of it, not its contents.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "voxelworld.h"  // ChunkEdit and EditView live there -- see the note in it

namespace v2 {

// ---------------------------------------------------------------------------
class Edits : public EditLookup {
  public:
    // The main thread's question, and the only one that takes a lock. Asked a
    // handful of times a frame -- by collision, the swing ray, a landing drop
    // and the physics ground tile -- never by a mesher worker, which reads a
    // snapshot instead. See VoxelTerrain::edits.
    bool removedAt(int i, int y, int j) const override {
        const int cx = floorDiv(i, CHUNK_VOX), cz = floorDiv(j, CHUNK_VOX);
        std::lock_guard<std::mutex> lk(mx_);
        auto it = map_.find(chunkKey(cx, cz));
        return it != map_.end() &&
               it->second->removed(i - cx * CHUNK_VOX, y, j - cz * CHUNK_VOX);
    }

    // The view a mesher works from. Null when this chunk has never been touched,
    // which is the common case and is why it is a pointer rather than a value --
    // an untouched chunk costs a failed hash lookup and nothing else.
    ChunkEditPtr snapshot(int cx, int cz) const {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = map_.find(chunkKey(cx, cz));
        return it == map_.end() ? ChunkEditPtr{} : it->second;
    }

    uint32_t versionOf(int cx, int cz) const {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = ver_.find(chunkKey(cx, cz));
        return it == ver_.end() ? 0u : it->second;
    }

    EditView view(int cx, int cz) const {
        EditView v;
        v.cx = cx;
        v.cz = cz;
        std::lock_guard<std::mutex> lk(mx_);
        {
            auto vi = ver_.find(chunkKey(cx, cz));
            v.ver = vi == ver_.end() ? 0u : vi->second;
        }
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                auto it = map_.find(chunkKey(cx + dx, cz + dz));
                if (it != map_.end()) v.c[size_t((dz + 1) * 3 + (dx + 1))] = it->second;
            }
        return v;
    }

    // -----------------------------------------------------------------------
    // TAKE A BALL OUT OF THE WORLD.
    //
    // A sphere rather than a cube for the same reason the chunk that flies off
    // is one: nothing breaks along three perfect planes. Centre and radius are
    // in voxels, and the ball may straddle a chunk boundary -- which is why the
    // writes are grouped by chunk first and applied per chunk after, so a ball
    // crossing four chunks is four copy-on-writes rather than one per voxel.
    //
    // Returns the chunks it touched, so the caller can ask for their meshes
    // back. Nothing here knows about meshes.
    // -----------------------------------------------------------------------
    std::vector<std::pair<int, int>> carveBall(int ci, int cy, int cj, int r) {
        std::unordered_map<long long, std::vector<uint32_t>> byChunk;
        std::vector<std::pair<int, int>> touched;
        const int rr = r * r;
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy + dz * dz > rr) continue;
                    const int i = ci + dx, y = cy + dy, j = cj + dz;
                    const int cx = floorDiv(i, CHUNK_VOX), cz = floorDiv(j, CHUNK_VOX);
                    const int lx = i - cx * CHUNK_VOX, lz = j - cz * CHUNK_VOX;
                    byChunk[chunkKey(cx, cz)].push_back(ChunkEdit::key(lx, y, lz));
                }

        std::lock_guard<std::mutex> lk(mx_);
        for (auto &kv : byChunk) {
            // COPY, ADD, SWAP. The old set stays valid for any worker still
            // reading it -- that is the whole contract, and it is what lets the
            // readers run without a lock.
            auto it = map_.find(kv.first);
            auto next = std::make_shared<ChunkEdit>();
            if (it != map_.end()) next->gone = it->second->gone;
            for (uint32_t k : kv.second) next->gone.insert(k);
            map_[kv.first] = next;
            // ...AND A VERSION, which is what makes invalidation safe. See the
            // note on World::carve: dropping a chunk outright while a mesh for
            // it was in flight let the ring ask for it again, and the two jobs
            // both landed and both claimed pool space. Versions instead: a
            // mesh is STAMPED with what it read, and a stale stamp is noticed
            // once, by the one place that already serialises requests.
            ++ver_[kv.first];
            touched.push_back({int(kv.first >> 32), int(int32_t(kv.first & 0xFFFFFFFF))});
        }
        return touched;
    }

    bool any() const {
        std::lock_guard<std::mutex> lk(mx_);
        return !map_.empty();
    }
    size_t chunksTouched() const {
        std::lock_guard<std::mutex> lk(mx_);
        return map_.size();
    }
    size_t voxelsRemoved() const {
        std::lock_guard<std::mutex> lk(mx_);
        size_t n = 0;
        for (const auto &kv : map_) n += kv.second->gone.size();
        return n;
    }

  private:
    static long long chunkKey(int cx, int cz) {
        return (long long(cx) << 32) | (long long(uint32_t(cz)));
    }
    mutable std::mutex mx_;
    std::unordered_map<long long, ChunkEditPtr> map_;
    std::unordered_map<long long, uint32_t> ver_;
};

}  // namespace v2
