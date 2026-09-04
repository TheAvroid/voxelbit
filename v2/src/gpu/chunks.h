// ---------------------------------------------------------------------------
// chunks.h -- the world, endlessly, a chunk at a time.
//
// The terrain was a fixed patch: one height field, meshed once at startup into
// one acceleration structure, with a hard edge you could walk to. This replaces
// it with a ring of chunks that follows the camera, so the wood goes on for as
// far as anyone cares to fly.
//
// THREE THINGS MAKE IT AFFORDABLE, and they are the whole design:
//
// 1. THE WORLD IS A PURE FUNCTION. heightM(x, z) and topMaterial(i, j, h) depend
//    on nothing but their arguments, so a chunk can be meshed by any thread at
//    any time with no shared state, no locking and no ordering. Nothing is ever
//    "generated" in the sense of being decided and stored -- it is recomputed,
//    identically, whenever it is needed. That is also why a chunk can be thrown
//    away and rebuilt later without the world changing under you.
//
// 2. MESHING RUNS ON WORKER THREADS, GAS BUILDS RUN BUDGETED ON THE MAIN ONE.
//    Meshing a chunk is 65k columns of noise and is the expensive half; it is
//    also pure CPU work, so it parallelises perfectly. Building the structure
//    afterwards has to happen on the thread that owns the CUDA context, so that
//    half is capped at a couple of chunks per frame. Crossing a chunk boundary
//    therefore costs a few frames of catch-up rather than one long stall.
//
// 3. THE SBT HAS FIXED CHUNK SLOTS. Every resident chunk owns one hitgroup
//    record for the life of its residency, and a chunk that is evicted hands its
//    slot to whatever replaces it. Only that one record is rewritten -- 32 bytes
//    over the bus -- instead of rebuilding the whole table every time the ring
//    moves.
//
// The IAS is the one thing still rebuilt whole when the resident set changes,
// because the instance count changes with it. At a few thousand instances that
// is well under a millisecond and not worth the complexity of anything cleverer.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "../optix/params.h"
#include "../scene/voxelworld.h"
#include "cuda_util.h"

namespace v2 {

// What a worker decides to put on the ground, in world column coordinates. The
// main thread turns these into instances once it knows the template handles.
struct Placement {
    int kind;   // 0 pine, 1 rock, 2 flower
    int index;  // which model of that kind
    int ci, cj;  // world column
    int h;       // terrain height there, in voxels
    int yaw;     // quarter turns
    uint32_t cell;  // hash stream for tint and sink
};

// The CPU-side result of meshing one chunk. Produced by a worker, consumed by
// the main thread.
struct ChunkBuild {
    int cx = 0, cz = 0;
    VoxMesh mesh;
    std::vector<Placement> decor;
};

// A chunk that is resident on the GPU.
struct Chunk {
    int cx = 0, cz = 0;
    int slot = -1;  // its SBT hitgroup record
    DeviceBuffer buffer;
    DeviceBuffer tri;
    OptixTraversableHandle gas = 0;
    size_t tris = 0;
    std::vector<OptixInstance> decor;
};

inline long long chunkKey(int cx, int cz) {
    // Packed so a std::map can order them; 21 bits each side is +/- a million
    // chunks, which at 25.6 m is a world about 50 000 km across.
    return (static_cast<long long>(cx) << 21) ^ static_cast<long long>(cz);
}

// ---------------------------------------------------------------------------
// The pool that meshes chunks.
//
// Deliberately dumb: a queue of coordinates in, a queue of finished meshes out,
// one mutex over each. The work items are tens of milliseconds each, so there
// is nothing to gain from anything finer grained and a great deal to lose in
// making a rendering bug look like a threading one.
// ---------------------------------------------------------------------------
class ChunkMesher {
  public:
    // The terrain is copied, not referenced. It is a handful of floats and the
    // workers must never see it change halfway through a chunk.
    void start(const VoxelTerrain &terrain, int threads) {
        terrain_ = terrain;
        stop_ = false;
        for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
    }

    ~ChunkMesher() {
        {
            std::lock_guard<std::mutex> lk(inMx_);
            stop_ = true;
        }
        inCv_.notify_all();
        for (auto &t : workers_) if (t.joinable()) t.join();
    }

    void request(int cx, int cz) {
        {
            std::lock_guard<std::mutex> lk(inMx_);
            pending_.push_back({cx, cz});
        }
        inCv_.notify_one();
    }

    // Take one finished chunk, if there is one.
    bool take(ChunkBuild *out) {
        std::lock_guard<std::mutex> lk(outMx_);
        if (done_.empty()) return false;
        *out = std::move(done_.front());
        done_.pop_front();
        return true;
    }

    size_t inFlight() {
        std::lock_guard<std::mutex> lk(inMx_);
        return pending_.size() + busy_;
    }

    // Footprints of the decor models, so a worker can space them without
    // reaching into the GPU-side templates.
    struct Footprint { int sx, sz, sy; };
    std::vector<Footprint> pineFoot, rockFoot, flowerFoot;
    float rockDensity = 0.010f, flowerDensity = 0.22f;
    // Multiplies the stand-density gate. The old engine capped the wood at a
    // fixed number of trees over a fixed patch; an endless world has no total
    // to cap, so the control has to be a DENSITY -- 1.0 is every site the
    // terrain will accept, which is a genuinely dark forest to stand in.
    float treeDensity = 0.31f;  // 0.55, less a quarter, less a quarter again
    float treeStride = 2.4f;
    uint32_t seed = 20260904u;

  private:
    VoxelTerrain terrain_;
    std::vector<std::thread> workers_;
    std::deque<std::pair<int, int>> pending_;
    std::deque<ChunkBuild> done_;
    std::mutex inMx_, outMx_;
    std::condition_variable inCv_;
    bool stop_ = false;
    size_t busy_ = 0;

    void run() {
        for (;;) {
            std::pair<int, int> job;
            {
                std::unique_lock<std::mutex> lk(inMx_);
                inCv_.wait(lk, [this] { return stop_ || !pending_.empty(); });
                if (stop_) return;
                job = pending_.front();
                pending_.pop_front();
                ++busy_;
            }

            ChunkBuild b;
            b.cx = job.first;
            b.cz = job.second;
            b.mesh = terrain_.meshChunk(b.cx, b.cz);
            scatter(&b);

            {
                std::lock_guard<std::mutex> lk(outMx_);
                done_.push_back(std::move(b));
            }
            {
                std::lock_guard<std::mutex> lk(inMx_);
                --busy_;
            }
        }
    }

    // -----------------------------------------------------------------------
    // What stands on this chunk.
    //
    // Every decision is hashed on the WORLD column, never on a chunk-local one,
    // so a tree is in the same place regardless of which chunk is meshing it
    // and regardless of what the camera has visited. That is the property that
    // makes eviction safe: a chunk rebuilt an hour later is identical.
    //
    // Spacing is only enforced WITHIN a chunk. A proper Poisson process would
    // need to see across the boundary, which would mean either a shared grid or
    // meshing the neighbours first -- and the visible cost of not doing it is
    // that two trees occasionally stand closer than they should across a seam.
    // That is a far better trade than serialising the chunk builds.
    // -----------------------------------------------------------------------
    void scatter(ChunkBuild *b) {
        const int I0 = b->cx * CHUNK_VOX, J0 = b->cz * CHUNK_VOX;
        const int wl = int(terrain_.waterLevel / VOXEL_M);

        struct Placed { float x, z, r; };
        std::vector<Placed> placed;

        // ---- trees ---------------------------------------------------------
        if (!pineFoot.empty()) {
            const int steps = int(CHUNK_M / treeStride);
            for (int j = 0; j <= steps; ++j) {
                for (int i = 0; i <= steps; ++i) {
                    const float bx = float(I0) * VOXEL_M + float(i) * treeStride;
                    const float bz = float(J0) * VOXEL_M + float(j) * treeStride;
                    const uint32_t cell = hashU32(uint32_t(int(bx * 16.0f)),
                                                  uint32_t(int(bz * 16.0f)) ^ 0x9E37u);

                    const float x = bx + (hashUnit(seed + 11u, cell) - 0.5f) * treeStride * 1.8f;
                    const float z = bz + (hashUnit(seed + 12u, cell) - 0.5f) * treeStride * 1.8f;

                    const int ci = int(floorf(x / VOXEL_M));
                    const int cj = int(floorf(z / VOXEL_M));
                    if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX)
                        continue;  // it belongs to a neighbour

                    const int h = terrain_.heightVox(ci, cj);
                    if (h <= wl + 8) continue;

                    const int slope =
                        maxi(absi(terrain_.heightVox(ci + 1, cj) - terrain_.heightVox(ci - 1, cj)),
                             absi(terrain_.heightVox(ci, cj + 1) - terrain_.heightVox(ci, cj - 1)));
                    if (slope >= VoxelTerrain::kTreeSlope) continue;

                    const float dens = terrain_.standDensity(x, z);
                    if (hashUnit(seed + 13u, cell) >
                        (saturate((dens - 0.30f) / 0.32f) * 0.92f + 0.05f) * treeDensity)
                        continue;

                    const int k = int(hashUnit(seed + 15u, cell) * float(pineFoot.size())) %
                                  int(pineFoot.size());
                    const int yaw = int(hashUnit(seed + 16u, cell) * 4.0f) & 3;
                    const Footprint &f = pineFoot[size_t(k)];
                    const float footX = float((yaw & 1) ? f.sz : f.sx) * VOXEL_M;
                    const float footZ = float((yaw & 1) ? f.sx : f.sz) * VOXEL_M;
                    const float keep = maxf(1.5f, 0.30f * maxf(footX, footZ));

                    bool clash = false;
                    for (const Placed &q : placed) {
                        const float dx = q.x - x, dz = q.z - z;
                        if (dx * dx + dz * dz < maxf(keep, q.r) * maxf(keep, q.r)) {
                            clash = true;
                            break;
                        }
                    }
                    if (clash) continue;

                    placed.push_back({x, z, keep});
                    b->decor.push_back({0, k, ci, cj, h, yaw, cell});
                }
            }
        }

        // ---- rocks and flowers --------------------------------------------
        // Both are scattered on a finer grid than the trees and take whatever
        // ground is left; a rock may sit on rock or soil, a flower only on
        // grass. Neither respects the tree spacing on purpose -- a boulder half
        // under a canopy is what a real wood looks like.
        scatterSmall(b, 1, rockFoot, rockDensity, 1.6f, false);
        scatterSmall(b, 2, flowerFoot, flowerDensity, 0.9f, true);
    }

    // -----------------------------------------------------------------------
    // Flowers grow in COLONIES, and a uniform random scatter cannot express
    // that -- raise its density and you get an even wash of flowers over the
    // whole wood, which reads as wallpaper. So the density is modulated by a
    // low-frequency field: about a fifteen-metre wavelength, thresholded so
    // most of the map is bare and the rest is a patch a few metres across.
    //
    // Inside a patch the placement is still jittered on its own grid rather
    // than clumped further, because a real colony is spread over its ground,
    // not piled at a point. That is the "not too tightly" part: the field says
    // WHERE, the grid still says how far apart.
    // -----------------------------------------------------------------------
    float colonyWeight(float x, float z) const {
        const float f = fbm(x * 0.065f + 811.3f, z * 0.065f + 447.9f, 3);
        // Below 0.44 there are none at all; it ramps to full over the next
        // tenth, so a colony has a soft edge rather than a cookie-cut one.
        return sstep(saturate((f - 0.44f) / 0.10f));
    }

    void scatterSmall(ChunkBuild *b, int kind, const std::vector<Footprint> &foot, float density,
                      float stride, bool grassOnly) {
        if (foot.empty() || density <= 0.0f) return;
        const int I0 = b->cx * CHUNK_VOX, J0 = b->cz * CHUNK_VOX;
        const int wl = int(terrain_.waterLevel / VOXEL_M);
        const int steps = int(CHUNK_M / stride);
        const uint32_t salt = (kind == 1) ? 0x51ED2701u : 0x27D4EB2Fu;

        for (int j = 0; j <= steps; ++j) {
            for (int i = 0; i <= steps; ++i) {
                const float bx = float(I0) * VOXEL_M + float(i) * stride;
                const float bz = float(J0) * VOXEL_M + float(j) * stride;
                const uint32_t cell =
                    hashU32(uint32_t(int(bx * 16.0f)) ^ salt, uint32_t(int(bz * 16.0f)));
                // Only the flowers colonise; a rock is where a rock is.
                const float w = (kind == 2) ? colonyWeight(bx, bz) : 1.0f;
                if (w <= 0.0f || hashUnit(seed + 41u, cell) >= density * w) continue;

                const float x = bx + (hashUnit(seed + 42u, cell) - 0.5f) * stride;
                const float z = bz + (hashUnit(seed + 43u, cell) - 0.5f) * stride;
                const int ci = int(floorf(x / VOXEL_M));
                const int cj = int(floorf(z / VOXEL_M));
                if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX) continue;

                const int h = terrain_.heightVox(ci, cj);
                if (h <= wl + 2) continue;
                const uint8_t top = terrain_.topMaterial(ci, cj, h);
                if (grassOnly && !isGrass(top)) continue;

                const int k = int(hashUnit(seed + 44u, cell) * float(foot.size())) %
                              int(foot.size());
                const int yaw = int(hashUnit(seed + 45u, cell) * 4.0f) & 3;
                b->decor.push_back({kind, k, ci, cj, h, yaw, cell});
            }
        }
    }
};

}  // namespace v2
