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

namespace v7 {

// What a worker decides to put on the ground, in world column coordinates. The
// main thread turns these into instances once it knows the template handles.
struct Placement {
    int kind;   // 0 pine, 1 rock, 2 flower, 3 mushroom, 4 pinecone
    int index;  // which model of that kind
    int ci, cj;  // world column
    int h;       // terrain height there, in voxels
    int yaw;     // quarter turns
    uint32_t cell;  // hash stream for tint and sink
    // Voxels ABOVE the model's own base. Zero for anything standing on the
    // ground; for a pinecone it is the branch it was perched on.
    int yOff = 0;
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
    std::vector<Footprint> pineFoot, rockFoot, flowerFoot, mushroomFoot, pineconeFoot;
    // Per pine model, every crown voxel resting on wood -- see collectPerches.
    std::vector<std::vector<Perch>> pinePerch;
    int pineconesPerTree = 14;
    // mushroomFoot holds the small models first and the doubled ones after it.
    // Everything at or past this index is a big one.
    int mushroomBig0 = 0;
    // Density INSIDE a colony now, not over the whole wood: the patches cover
    // a fifth of the ground, so the old 0.22 spread over everything is about
    // this much concentrated into them. A bed wants to look like a bed.
    float rockDensity = 0.010f, flowerDensity = 0.45f;
    // Mushrooms are NOT colonised the way flowers are. A flower bed is a patch
    // and reads wrong scattered evenly; a mushroom in a conifer wood is mostly
    // a thing you come across on its own, so this is a flat low probability
    // over any grass the wood will grow.
    float mushroomDensity = 0.015f;
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
        // ONE PER WORKER, for the life of the thread -- the grids and the noise
        // memo inside it are pure working storage, and rebuilding them per
        // chunk was several hundred kilobytes of allocate-and-zero per job.
        // See ChunkScratch in voxelworld.h for why reuse is safe.
        ChunkScratch scratch;

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
            b.mesh = terrain_.meshChunk(b.cx, b.cz, scratch);
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
                    hangPinecones(b, k, ci, cj, h, yaw, cell);
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
        scatterSmall(b, 3, mushroomFoot, mushroomDensity, 1.3f, true);
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

    // -----------------------------------------------------------------------
    // CONES ON A TREE THAT WAS JUST PLACED.
    //
    // Every candidate comes from collectPerches, so it is a voxel with wood
    // directly beneath it and a cone put there is sitting on the branch rather
    // than hanging in the air near one. Nothing here can float, and nothing
    // here needs a tolerance to say so.
    //
    // THE TREE'S QUARTER TURN HAS TO BE APPLIED BY HAND, and this is the part
    // that is easy to get wrong. The perch is in the pine model's own voxel
    // frame; the tree it belongs to has been rotated about the model centre by
    // makeInstance. So the offset from the centre is rotated the same way here
    // -- the same four cases as kRot, in integers -- and if the two ever
    // disagree the cones drift off into the neighbouring tree's canopy.
    void hangPinecones(ChunkBuild *b, int pineIndex, int ci, int cj, int h, int yaw,
                       uint32_t cell) {
        if (pineconeFoot.empty() || pineconesPerTree <= 0) return;
        if (pineIndex < 0 || size_t(pineIndex) >= pinePerch.size()) return;
        const std::vector<Perch> &pp = pinePerch[size_t(pineIndex)];
        if (pp.empty()) return;

        const Footprint &pf = pineFoot[size_t(pineIndex)];
        const int treeSink = decorSink(0, pineIndex, pf.sy, seed, cell);

        for (int n = 0; n < pineconesPerTree; ++n) {
            const uint32_t r = hashU32(seed + 0x9E37u + uint32_t(n), cell);
            const Perch &q = pp[r % pp.size()];

            // Offset from the model centre, in voxels, then the same quarter
            // turn kRot applies to the tree itself.
            const int dx = int(q.x) - pf.sx / 2;
            const int dz = int(q.z) - pf.sz / 2;
            int rx = dx, rz = dz;
            switch (yaw & 3) {
                case 1: rx = dz;  rz = -dx; break;
                case 2: rx = -dx; rz = -dz; break;
                case 3: rx = -dz; rz = dx;  break;
                default: break;
            }

            const int k = int(hashU32(seed + 0x5177u + uint32_t(n), cell) % pineconeFoot.size());
            const int cyaw = int(hashU32(seed + 0x2B3Du + uint32_t(n), cell) & 3u);
            // h is still the TREE's ground height and treeSink its sink, so the
            // cone's world height comes out as the tree's base plus the perch.
            // THE PERCH IS WHERE THE CONE'S TOP GOES, and makeInstance places
            // a model by its BASE, so the base is one model-height lower. The
            // -1 is because a cone `sy` voxels tall occupying rows
            // [base .. base + sy - 1] has its top row at base + sy - 1.
            const int coneTop = int(q.y);
            const int coneBase = coneTop - (pineconeFoot[size_t(k)].sy - 1);
            if (coneBase < 0) continue;
            b->decor.push_back({4, k, ci + rx, cj + rz, h, cyaw, cell, coneBase - treeSink});
        }
    }

    void scatterSmall(ChunkBuild *b, int kind, const std::vector<Footprint> &foot, float density,
                      float stride, bool grassOnly) {
        if (foot.empty() || density <= 0.0f) return;
        TerrainMemo memo;
        FbmMemo wobMemo;
        const int I0 = b->cx * CHUNK_VOX, J0 = b->cz * CHUNK_VOX;
        const int wl = int(terrain_.waterLevel / VOXEL_M);
        const int steps = int(CHUNK_M / stride);
        // A DISTINCT SALT PER KIND, so the hash streams do not line up. Two
        // kinds sharing a salt land on exactly the same cells and every
        // mushroom grows out of the middle of a flower.
        const uint32_t salt = (kind == 1)   ? 0x51ED2701u
                              : (kind == 3) ? 0x9E3779B1u
                                            : 0x27D4EB2Fu;

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
                int k = (kind == 2)
                            ? col.species
                            : int(hashUnit(seed + 44u, cell) * float(foot.size())) %
                                  int(foot.size());

                // TWO SIZES OF MUSHROOM, one in four of them the big one. The
                // draw is a separate hash stream from the species pick above:
                // sharing one would tie "which mushroom" to "how big", so every
                // big one would always be the same model.
                if (kind == 3 && mushroomBig0 > 0 && mushroomBig0 < int(foot.size())) {
                    const bool big = hashUnit(seed + 0x8B1Du, cell) < 0.25f;
                    const int lo = big ? mushroomBig0 : 0;
                    const int hi = big ? int(foot.size()) : mushroomBig0;
                    k = lo + (int(hashUnit(seed + 0x3C7Fu, cell) * float(hi - lo)) % (hi - lo));
                }
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
                                                         decorSink(1, k, f.sy, seed, cell), memo))
                        continue;
                }

                b->decor.push_back({kind, k, ci, cj, h, yaw, cell});
            }
        }
    }
};

}  // namespace v7
