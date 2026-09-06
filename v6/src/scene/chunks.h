// ---------------------------------------------------------------------------
// chunks.h -- the world, endlessly, a chunk at a time. CPU side only.
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
// 2. MESHING RUNS ON WORKER THREADS, STRUCTURE BUILDS RUN BUDGETED ON THE MAIN ONE.
//    Meshing a chunk is 65k columns of noise and is the expensive half; it is
//    also pure CPU work, so it parallelises perfectly. Building the structure
//    afterwards has to be recorded on the immediate command list, so that half
//    is capped at a couple of chunks per frame. Crossing a chunk boundary
//    therefore costs a few frames of catch-up rather than one long stall.
//
// 3. NOTHING PER-CHUNK IS BOUND TO THE PIPELINE. Under OptiX this was the
//    fiddliest part of the design: every resident chunk owned a shader binding
//    table record for the life of its residency, handed its slot on when it was
//    evicted, and published 32 bytes over the bus whenever the ring moved.
//    Inline ray tracing has no table, so a chunk carries its triangle-attribute
//    offset in the instance record the structure needed anyway, and residency
//    stops being a binding problem. See gpu/world.h.
//
// The top-level structure is rebuilt whole when the resident set changes,
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

#include "collide.h"
#include "voxelworld.h"

namespace v6 {

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

    // Meshing cost, summed across workers. Atomic rather than guarded because
    // it is written from every worker and read from the main thread, and a
    // profile counter must never be able to serialise the thing it measures.
    std::atomic<double> meshMs{0.0};
    std::atomic<size_t> meshCount{0};

    // Footprints of the decor models, so a worker can space them without
    // reaching into the GPU-side templates.
    // sx, sz, sy is the model's bounding box; baseX and baseZ are the footprint
    // where it MEETS THE GROUND, which is a different and usually smaller
    // number -- see kBaseHeightM in collide.h.
    struct Footprint { int sx, sz, sy, baseX, baseZ; };
    static constexpr bool kNoRockSlopeTest = true;
    std::vector<Footprint> pineFoot, rockFoot, flowerFoot;
    // Density INSIDE a colony now, not over the whole wood: the patches cover
    // a fifth of the ground, so the old 0.22 spread over everything is about
    // this much concentrated into them. A bed wants to look like a bed.
    float rockDensity = 0.010f, flowerDensity = 0.45f;
    // Multiplies the stand-density gate. The old engine capped the wood at a
    // fixed number of trees over a fixed patch; an endless world has no total
    // to cap, so the control has to be a DENSITY -- 1.0 is every site the
    // terrain will accept, which is a genuinely dark forest to stand in.
    //
    // THE COUNT FALLS BY SLIGHTLY LESS THAN THIS DOES, and it is worth knowing
    // why before chasing the difference. This scales how many sites are
    // OFFERED; the spacing test below then rejects any that land too close to
    // one already placed. Offer fewer and proportionally fewer of them clash,
    // so a quarter off here is measured as 22-24% fewer pines on the ground,
    // and the exact figure depends on how thick the stand was to begin with.
    float treeDensity = 0.2325f;  // 0.55, less a quarter, three times over
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
            const auto t0 = std::chrono::steady_clock::now();
            b.mesh = terrain_.meshChunk(b.cx, b.cz);
            scatter(&b);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            for (double cur = meshMs.load(std::memory_order_relaxed);
                 !meshMs.compare_exchange_weak(cur, cur + ms, std::memory_order_relaxed);)
                ;
            meshCount.fetch_add(1, std::memory_order_relaxed);

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
        // The scatter grids are coarse -- 2.4 m for trees, 0.9 for flowers --
        // so a memo hits far less often here than in the mesher. It still hits:
        // the height field's slowest octave is eighty metres across, and these
        // rows walk a chunk twenty-five metres wide.
        TerrainMemo memo;

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

                    const int h = terrain_.heightVox(ci, cj, memo);
                    if (h <= wl + 8) continue;

                    const int slope = maxi(absi(terrain_.heightVox(ci + 1, cj, memo) -
                                                terrain_.heightVox(ci - 1, cj, memo)),
                                           absi(terrain_.heightVox(ci, cj + 1, memo) -
                                                terrain_.heightVox(ci, cj - 1, memo)));
                    if (slope >= VoxelTerrain::kTreeSlope) continue;

                    const float dens = terrain_.standDensity(x, z, memo.stand);
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
    // Flowers grow in COLONIES, ONE SPECIES TO A COLONY. Roses stand with
    // roses, lavender with lavender.
    //
    // A uniform random scatter cannot express either half of that. Raise its
    // density and you get an even wash over the whole wood, which reads as
    // wallpaper; pick the model per site and you get every species shuffled
    // through every patch, which reads as a seed packet rather than a plant
    // that spread from one root.
    //
    // So a colony is an OBJECT here, not a threshold on a noise field. Sites
    // sit on a jittered eighteen-metre grid, two thirds of them are used, and
    // each one carries its own radius AND its own species. A point takes both
    // from whichever site covers it most strongly, so the species cannot
    // change inside a patch -- that is the property the old fbm-threshold
    // field could not offer at any frequency, because the field said where the
    // flowers were and nothing at all about what they were.
    //
    // Two colonies of different species can still meet, and that is fine: the
    // handover happens where both are at the outer edge of their falloff and
    // there is almost nothing growing on either side of the line.
    //
    // Inside a patch the placement is still jittered on the scatter grid
    // rather than clumped further, because a real colony is spread over its
    // ground, not piled at a point. The site says WHERE and WHAT, the grid
    // still says how far apart.
    // -----------------------------------------------------------------------
    struct Colony {
        float w = 0.0f;    // 0..1 how strongly this ground is inside a patch
        int species = 0;   // the one flower model the whole patch is made of
    };

    // Eighteen metres between sites, and the site sits in the middle half of
    // its cell. Both numbers are about SEPARATION: two neighbouring colonies
    // that overlap heavily are two species mixed again, just in bigger lumps.
    static constexpr float kColonyCell = 18.0f;

    Colony colonyAt(float x, float z, int speciesCount, FbmMemo &wobMemo) const {
        Colony best;
        if (speciesCount <= 0) return best;
        const int gi = int(floorf(x / kColonyCell));
        const int gj = int(floorf(z / kColonyCell));

        // The radius is modulated by a ~five-metre noise, sampled at the QUERY
        // point rather than the site, so the outline is lobed and irregular
        // instead of a disc -- a circle of flowers is as obviously authored as
        // no flowers at all.
        const float wob = 0.70f + 0.60f * fbm(wobMemo, x * 0.19f + 133.7f, z * 0.19f + 91.4f, 2);

        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                const uint32_t c = hashU32(uint32_t(gi + di) ^ 0x9E3779B9u, uint32_t(gj + dj));
                // Most of the ground has no colony on it at all. Without this
                // the patches tile and the wood is uniformly flowered again,
                // just in lumps.
                if (hashUnit(seed + 61u, c) > 0.68f) continue;

                const float sx = (float(gi + di) + 0.25f + 0.50f * hashUnit(seed + 62u, c)) *
                                 kColonyCell;
                const float sz = (float(gj + dj) + 0.25f + 0.50f * hashUnit(seed + 63u, c)) *
                                 kColonyCell;
                const float rad = (3.0f + 4.0f * hashUnit(seed + 64u, c)) * wob;
                const float dx = x - sx, dz = z - sz;
                const float d = sqrtf(dx * dx + dz * dz);
                if (d >= rad) continue;

                // Full density through the middle, fading over the outer third
                // so a patch thins out at its edge the way a spreading plant
                // does.
                const float w = sstep(saturate((rad - d) / (0.34f * rad)));
                if (w <= best.w) continue;
                best.w = w;
                best.species = int(hashUnit(seed + 65u, c) * float(speciesCount)) % speciesCount;
            }
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // Is the ground under a footprint flat enough to hide a base sunk this far?
    //
    // Sampled on a grid rather than at the corners: a boulder can span a gully
    // whose lowest point is nowhere near a corner, and a corners-only test
    // would put it straight over the top of one. Five by five over the
    // footprint is twenty-five height evaluations, and it runs only for the
    // handful of sites per chunk that have already passed the density gate.
    //
    // The tolerance is `sink - 1`. A rock hung on a column of height h has its
    // base at h + 1 - sink; ground at h - d meets that base exactly when
    // d == sink - 1, and anything deeper is a gap.
    // -----------------------------------------------------------------------
    bool groundHolds(int ci, int cj, int h, int bx, int bz, int sink, TerrainMemo &memo) const {
        const int allow = maxi(0, sink - 1);
        const int hx = bx / 2, hz = bz / 2;
        for (int sj = 0; sj <= 4; ++sj)
            for (int si = 0; si <= 4; ++si) {
                const int x = ci - hx + (bx * si) / 4;
                const int z = cj - hz + (bz * sj) / 4;
                if (h - terrain_.heightVox(x, z, memo) > allow) return false;
            }
        return true;
    }

    void scatterSmall(ChunkBuild *b, int kind, const std::vector<Footprint> &foot, float density,
                      float stride, bool grassOnly) {
        if (foot.empty() || density <= 0.0f) return;
        TerrainMemo memo;
        FbmMemo wobMemo;
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
                // Only the flowers colonise; a rock is where a rock is. Asked
                // at the CELL BASE, not at the jittered position below, so
                // every flower in a patch answers from the same colony.
                Colony col;
                if (kind == 2) {
                    col = colonyAt(bx, bz, int(foot.size()), wobMemo);
                    if (col.w <= 0.0f) continue;
                }
                const float w = (kind == 2) ? col.w : 1.0f;
                if (hashUnit(seed + 41u, cell) >= density * w) continue;

                const float x = bx + (hashUnit(seed + 42u, cell) - 0.5f) * stride;
                const float z = bz + (hashUnit(seed + 43u, cell) - 0.5f) * stride;
                const int ci = int(floorf(x / VOXEL_M));
                const int cj = int(floorf(z / VOXEL_M));
                if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX) continue;

                const int h = terrain_.heightVox(ci, cj, memo);
                if (h <= wl + 2) continue;
                const uint8_t top = terrain_.topMaterial(ci, cj, h, memo);
                if (grassOnly && !isGrass(top)) continue;

                // The species is the COLONY's, not this cell's: that is the
                // whole point of the patch.
                const int k = (kind == 2)
                                  ? col.species
                                  : int(hashUnit(seed + 44u, cell) * float(foot.size())) %
                                        int(foot.size());
                const int yaw = int(hashUnit(seed + 45u, cell) * 4.0f) & 3;

                // ---- can this ground actually hold it? ----------------------
                //
                // A ROCK IS PLACED ON ONE COLUMN AND SPANS MANY. Its base sits
                // `sink` voxels below the height of the column it was hung on;
                // if the terrain falls away faster than that under the rest of
                // its footprint, the far side leaves the ground and a five
                // metre boulder is standing on one corner with daylight under
                // it. Trees have had a slope test since the beginning and rocks
                // never did, which is why it was always the boulders.
                //
                // The test is the FOOTPRINT, not a slope: a slope at the centre
                // says nothing about how far the ground has gone by the time it
                // reaches the edge of something four metres across. So the
                // terrain is sampled over the base and the deepest drop from
                // the centre column is what has to be hidden.
                //
                // Only what the ground has to SUPPORT is tested -- ground that
                // RISES under a rock buries it deeper, which is what a boulder
                // half-embedded in a bank looks like, and is fine.
                if (kind == 1 && !kNoRockSlopeTest) {
                    const Footprint &f = foot[size_t(k)];
                    const int footX = (yaw & 1) ? f.baseZ : f.baseX;
                    const int footZ = (yaw & 1) ? f.baseX : f.baseZ;
                    if (footX > 0 && footZ > 0 && !groundHolds(ci, cj, h, footX, footZ,
                                                         decorSink(1, f.sy, seed, cell), memo))
                        continue;
                }

                b->decor.push_back({kind, k, ci, cj, h, yaw, cell});
            }
        }
    }
};

}  // namespace v6
