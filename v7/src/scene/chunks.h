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
    // Voxels BELOW the sink decorSink already asks for -- how much further this
    // particular placement has to go down before nothing of its underside is
    // above the ground it spans. Zero on flat ground.
    //
    // IT IS CARRIED RATHER THAN RECOMPUTED because the two sides that need it
    // cannot both reach the terrain. decorSink is a pure function of the model,
    // and makeInstance in gpu/world.h -- which turns this into a transform, and
    // from that a collider -- has no height field to ask. Measuring it once
    // here, in the worker that already has the terrain open, is also what keeps
    // the visual and the collider from ever disagreeing: there is one number
    // and they both read it.
    int extraSink = 0;
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
    // A circle on the ground that something already occupies. Used to keep the
    // small decor out of the boulders -- see collectRocks.
    struct Disc { float x, z, r; };
    // The rock scatter's grid, named because two passes now have to agree about
    // it: the one that places rocks and the one that avoids them.
    static constexpr float kRockStride = 1.6f;
    static constexpr uint32_t kRockSalt = 0x51ED2701u;
    // ---- how flush is flush, and when to give up instead ------------------
    //
    // A DOUBLED BOULDER IS HELD TO FLAT GROUND -- but this is a matter of taste
    // rather than of correctness, and it is worth being clear which. Nothing
    // floats at any setting: groundDrop measures the ground under the base and
    // the model is sunk until it meets it. What this decides is how far into a
    // slope a boulder is allowed to bury ITSELF before a smaller stone is put
    // there instead.
    //
    // TWELVE, not eight. Eight is 80 cm of fall across a base several metres
    // wide, which sounds reasonable and is far stricter than it sounds -- the
    // largest models would have found a site one time in seven (see the table
    // in the scatter). Twelve is 1.2 m, which a 5.6 m boulder wears without
    // looking sunk, and it roughly doubles how many of them the wood carries.
    //
    // Everything smaller is simply sunk until it meets the ground, and only
    // refused when that would swallow it: past 55% of its own height there is
    // nothing left standing out to look at.
    int bigRockFlatVox = 12;
    float maxBuryFrac = 0.55f;
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
    // Rocks down a quarter, 0.010 -> 0.0075. Asked for alongside the doubling
    // of the mid stones, and the two go together: eleven of the models are now
    // boulders rather than six, so the same density would have put noticeably
    // more large rock in the wood than before rather than the same amount at a
    // new size.
    float rockDensity = 0.0075f, flowerDensity = 0.45f;
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

                    // SIT IT FLUSH. The slope test above is a gradient at the
                    // trunk and rejects the steep sites; this measures the
                    // ground under the whole base and sinks the trunk until
                    // nothing of it is standing clear. A pine passes the slope
                    // test on ground that still falls a few voxels across two
                    // metres of trunk, and that was enough to show daylight
                    // under the uphill side.
                    //
                    // The tree's own footprint, not the crown's: f.baseX/baseZ
                    // is where the model MEETS the ground -- a trunk -- and
                    // sinking a pine to clear the lowest ground under its
                    // twelve-metre canopy would put it underground.
                    int extraSink = 0;
                    {
                        const int bX = (yaw & 1) ? f.baseZ : f.baseX;
                        const int bZ = (yaw & 1) ? f.baseX : f.baseZ;
                        if (bX > 0 && bZ > 0) {
                            const int base = decorSink(0, k, f.sy, seed, cell);
                            const int drop = groundDrop(ci, cj, h, bX, bZ, memo);
                            extraSink = maxi(0, drop - base);
                        }
                    }

                    placed.push_back({x, z, keep});
                    b->decor.push_back({0, k, ci, cj, h, yaw, cell, 0, extraSink});
                    hangPinecones(b, k, ci, cj, h, yaw, cell, extraSink);
                }
            }
        }

        // ---- rocks and flowers --------------------------------------------
        // Both are scattered on a finer grid than the trees and take whatever
        // ground is left; a rock may sit on rock or soil, a flower only on
        // grass. Flowers and mushrooms still ignore the trees entirely, and
        // should: growing under a canopy is what they do.
        //
        // ROCKS NO LONGER DO. The note that used to sit here said a boulder
        // half under a canopy is what a real wood looks like, and that was
        // right for a three-metre stone. At twelve to twenty-two metres a
        // boulder does not sit under the canopy, it stands where the tree is.
        // THE ROCKS ARE COLLECTED BEFORE ANYTHING SMALL IS SCATTERED, so the
        // flowers and mushrooms can be kept out of them. A mushroom growing out
        // of the middle of a boulder was the reported symptom; the cause is
        // that each kind was spaced only against ITSELF -- different salt,
        // different stride, no idea the other existed.
        //
        // Collected rather than tested against b->decor, because a boulder
        // three metres across can overhang a chunk boundary and the mushroom it
        // swallows may belong to the neighbour. See collectRocks.
        std::vector<Disc> rocks;
        collectRocks(b, &rocks);
        // THE ROCKS KEEP OUT OF THE PINES. Collected across the 3x3 chunk
        // neighbourhood on each neighbour's own lattice -- see collectTrees for
        // why they cannot be re-derived from this one's.
        std::vector<Disc> trees;
        collectTrees(b, &trees, memo);
        scatterSmall(b, 1, rockFoot, rockDensity, kRockStride, false, &trees);
        scatterSmall(b, 2, flowerFoot, flowerDensity, 0.9f, true, &rocks);
        scatterSmall(b, 3, mushroomFoot, mushroomDensity, 1.3f, true, &rocks);
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
        return groundDrop(ci, cj, h, bx, bz, memo) <= maxi(0, sink - 1);
    }

    // -----------------------------------------------------------------------
    // HOW FAR THE GROUND FALLS AWAY UNDER A FOOTPRINT, in voxels, measured from
    // the column the model is hung on. Negative is never returned: ground that
    // only RISES cannot leave a gap.
    //
    // This is the measurement everything about sitting flush is built on, and
    // it replaces asking about a SLOPE. A slope is a gradient at a point and
    // says nothing useful about a thing four metres across: a rock can sit on a
    // column whose immediate neighbours are level and still have the ground
    // two metres down by the time it reaches its own edge. What matters is the
    // LOWEST ground anywhere under the base, because that is the one place
    // daylight gets in.
    //
    // The sample grid follows the footprint rather than being fixed at 5x5. A
    // mushroom eight voxels across does not need twenty-five height
    // evaluations -- it needs four -- and a doubled boulder seventy across
    // wants more than five to a side or it steps straight over a gully. One
    // sample per four voxels, clamped to [2, 6] a side, which is 4 evaluations
    // for the smallest and 36 for the largest.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // EVERY PINE THAT COULD REACH THIS CHUNK, as trunk circles on the ground.
    //
    // Rocks used to ignore trees entirely, and there was a comment saying so on
    // purpose: a boulder half under a canopy is what a real wood looks like.
    // That was true when a rock was three to five metres. It stopped being true
    // when the big stones were revoxelised to twelve and twenty-two, because a
    // stone that size does not sit under a canopy -- it contains the tree.
    //
    // THE TREE WINS, and the rock is the one that moves. There are two hundred
    // times more trees than boulders, a missing rock is invisible, and a tree
    // with a rock through it is not. It is also the only order that works
    // across a chunk boundary: the trees are already placed by the time the
    // rocks are scattered, so the rock can defer to them, while a tree cannot
    // defer to a rock that does not exist yet.
    //
    // THE NEIGHBOURS HAVE TO BE WALKED ON THEIR OWN LATTICE, which is the part
    // that is easy to get wrong. treeStride is 2.4 m and a chunk is 25.6, so
    // 25.6 / 2.4 is not a whole number and the tree grids of two chunks DO NOT
    // LINE UP -- each starts at its own origin. A neighbour's trees therefore
    // cannot be re-derived from this chunk's lattice at all; the only way to
    // know them is to run the neighbour's. Hence the 3x3 sweep. One ring is
    // enough because the widest rock reaches about ten metres and a chunk is
    // twenty-five, so nothing beyond the immediate neighbours can be touched.
    //
    // The intra-chunk spacing rejection is deliberately NOT applied. It depends
    // on the order the placing chunk happened to walk its own cells, which is
    // not reproducible from here -- and skipping it can only ADD trees to this
    // list, never remove one. An over-estimate costs a rock that a rejected
    // tree would have left room for; an under-estimate would put a rock through
    // a trunk, which is the thing being fixed.
    // -----------------------------------------------------------------------
    void collectTrees(ChunkBuild *b, std::vector<Disc> *out, TerrainMemo &memo) const {
        if (pineFoot.empty() || treeDensity <= 0.0f) return;
        const int wl = int(terrain_.waterLevel / VOXEL_M);
        const int steps = int(CHUNK_M / treeStride);
        for (int nz = -1; nz <= 1; ++nz)
            for (int nx = -1; nx <= 1; ++nx) {
                const int I0 = (b->cx + nx) * CHUNK_VOX, J0 = (b->cz + nz) * CHUNK_VOX;
                for (int j = 0; j <= steps; ++j)
                    for (int i = 0; i <= steps; ++i) {
                        const float bx = float(I0) * VOXEL_M + float(i) * treeStride;
                        const float bz = float(J0) * VOXEL_M + float(j) * treeStride;
                        const uint32_t cell = hashU32(uint32_t(int(bx * 16.0f)),
                                                      uint32_t(int(bz * 16.0f)) ^ 0x9E37u);
                        const float x =
                            bx + (hashUnit(seed + 11u, cell) - 0.5f) * treeStride * 1.8f;
                        const float z =
                            bz + (hashUnit(seed + 12u, cell) - 0.5f) * treeStride * 1.8f;
                        const int ci = int(floorf(x / VOXEL_M));
                        const int cj = int(floorf(z / VOXEL_M));
                        // A tree belongs to the chunk holding its own column --
                        // the same test the placing pass makes, so a tree is
                        // counted once and by the chunk that will actually
                        // place it.
                        if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX)
                            continue;
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
                        // THE TRUNK, not the crown. baseX/baseZ is what meets
                        // the ground; measuring the canopy would push rocks a
                        // full twelve metres from every pine and empty the wood
                        // of them. A boulder under branches is still fine -- it
                        // is a boulder through a TRUNK that is not.
                        const int bxv = (yaw & 1) ? f.baseZ : f.baseX;
                        const int bzv = (yaw & 1) ? f.baseX : f.baseZ;
                        if (bxv <= 0 || bzv <= 0) continue;
                        out->push_back({x, z, 0.25f * float(bxv + bzv) * VOXEL_M});
                    }
            }
    }

    // -----------------------------------------------------------------------
    // EVERY ROCK THAT COULD REACH THIS CHUNK, as circles on the ground.
    //
    // Walked over the chunk's own rock lattice PLUS a margin, because a stone
    // is placed by whichever chunk owns its centre column but is several metres
    // wide and reaches into its neighbours. Without the margin a mushroom could
    // still be swallowed within a rock-radius of every chunk edge, which is the
    // most visible place to get it wrong -- a seam of them.
    //
    // HASH-ONLY, exactly like the same-kind spacing test below it: this asks
    // where a rock WOULD go and how big it is, not whether the terrain under it
    // would have accepted it. Re-running the height, material and flatness
    // tests per cell would cost more than the whole scatter, and being wrong in
    // this direction only ever drops a mushroom that a rejected rock would have
    // left room for.
    //
    // The margin cells are keyed off THIS chunk's lattice arithmetic, which is
    // the same expression the OWNING chunk uses but reached from a different
    // origin -- so the two only agree if the float arithmetic agrees exactly.
    // It does, and that was worth checking rather than assuming: comparing
    // chunk N's view of its margin against chunk N+1's view of the same cells,
    // over an 80 x 80 spread of chunks, 26 244 boundary cells and NOT ONE
    // disagreement. The chunk pitch is a whole number of strides (25.6 / 1.6 =
    // 16) and both sides land on the same representable multiple, so the keys
    // are identical rather than merely close.
    // -----------------------------------------------------------------------
    void collectRocks(ChunkBuild *b, std::vector<Disc> *out) const {
        if (rockFoot.empty() || rockDensity <= 0.0f) return;
        float maxRad = 0.0f;
        for (const Footprint &f : rockFoot)
            maxRad = maxf(maxRad, 0.25f * float(f.sx + f.sz) * VOXEL_M);

        const int I0 = b->cx * CHUNK_VOX, J0 = b->cz * CHUNK_VOX;
        const int steps = int(CHUNK_M / kRockStride);
        const int margin = int(ceilf(maxRad / kRockStride)) + 1;

        for (int j = -margin; j <= steps + margin; ++j)
            for (int i = -margin; i <= steps + margin; ++i) {
                const float bx = float(I0) * VOXEL_M + float(i) * kRockStride;
                const float bz = float(J0) * VOXEL_M + float(j) * kRockStride;
                const uint32_t c = hashU32(uint32_t(int(bx * 16.0f)) ^ kRockSalt,
                                           uint32_t(int(bz * 16.0f)));
                if (hashUnit(seed + 41u, c) >= rockDensity) continue;
                const float x = bx + (hashUnit(seed + 42u, c) - 0.5f) * kRockStride;
                const float z = bz + (hashUnit(seed + 43u, c) - 0.5f) * kRockStride;
                const int k =
                    int(hashUnit(seed + 44u, c) * float(rockFoot.size())) % int(rockFoot.size());
                const Footprint &f = rockFoot[size_t(k)];
                out->push_back({x, z, 0.25f * float(f.sx + f.sz) * VOXEL_M});
            }
    }

    int groundDrop(int ci, int cj, int h, int bx, int bz, TerrainMemo &memo) const {
        if (bx <= 0 || bz <= 0) return 0;
        const int nx = maxi(2, mini(6, bx / 4 + 1));
        const int nz = maxi(2, mini(6, bz / 4 + 1));
        const int hx = bx / 2, hz = bz / 2;
        int lowest = h;
        for (int sj = 0; sj < nz; ++sj)
            for (int si = 0; si < nx; ++si) {
                const int x = ci - hx + (bx * si) / (nx - 1);
                const int z = cj - hz + (bz * sj) / (nz - 1);
                const int g = terrain_.heightVox(x, z, memo);
                if (g < lowest) lowest = g;
            }
        return maxi(0, h - lowest);
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
    // treeExtraSink is whatever the trunk was sunk BEYOND its own decorSink to
    // meet the ground. A cone rides the tree, so it has to come down by the
    // same amount or the whole scatter of them floats where the tree used to
    // be. It is passed through as the cone's OWN extraSink, which makeInstance
    // already subtracts for every kind -- so the two stay locked together
    // without a second rule about pinecones anywhere.
    void hangPinecones(ChunkBuild *b, int pineIndex, int ci, int cj, int h, int yaw,
                       uint32_t cell, int treeExtraSink) {
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
            b->decor.push_back({4, k, ci + rx, cj + rz, h, cyaw, cell, coneBase - treeSink,
                                treeExtraSink});
        }
    }

    // `avoid`, when given, is ground already taken by something of another
    // kind -- in practice the boulders. Nothing in this pass may stand inside
    // one. See collectRocks.
    void scatterSmall(ChunkBuild *b, int kind, const std::vector<Footprint> &foot, float density,
                      float stride, bool grassOnly, const std::vector<Disc> *avoid = nullptr) {
        if (foot.empty() || density <= 0.0f) return;
        TerrainMemo memo;
        FbmMemo wobMemo;
        const int I0 = b->cx * CHUNK_VOX, J0 = b->cz * CHUNK_VOX;
        const int wl = int(terrain_.waterLevel / VOXEL_M);
        const int steps = int(CHUNK_M / stride);
        // A DISTINCT SALT PER KIND, so the hash streams do not line up. Two
        // kinds sharing a salt land on exactly the same cells and every
        // mushroom grows out of the middle of a flower.
        const uint32_t salt = (kind == 1)   ? kRockSalt
                              : (kind == 3) ? 0x9E3779B1u
                                            : 0x27D4EB2Fu;

        // ── NOTHING GROWS THROUGH ANYTHING ELSE OF ITS OWN KIND ────────────
        //
        // The scatter grid is FINER THAN THE THINGS ON IT. Rocks sit on a 1.6 m
        // grid and jitter by half of it, so two neighbouring cells can put
        // their centres under a metre apart -- and a boulder is four or five
        // metres across. Adjacent cells therefore produced stones standing
        // inside one another as a matter of course, and the same for the big
        // mushrooms on their 1.3 m grid.
        //
        // Fixed by rejection rather than by widening the grid, because widening
        // it would thin the wood out everywhere to fix the few places two
        // neighbours happened to collide. A candidate looks at the cells around
        // it, and where two would overlap the one with the higher cell hash
        // survives. That rule is symmetric -- both cells reach the same verdict
        // about the pair without either knowing the other was considered -- and
        // it depends only on world position, so it is identical either side of
        // a chunk boundary. Two chunks meshed in different orders, or one
        // meshed alone, all agree.
        //
        // The neighbour test is deliberately HASH-ONLY: it re-derives where a
        // neighbour would go and how big it would be, but not whether the
        // terrain under it would have accepted it. Re-running the height,
        // material and slope tests per neighbour would cost far more than the
        // scatter itself, and being wrong in this direction only ever drops a
        // stone that a rejected neighbour would have left room for.
        const bool spaced = (kind == 1 || kind == 3);
        float maxRad = 0.0f;
        if (spaced)
            for (const Footprint &f : foot)
                maxRad = maxf(maxRad, 0.25f * float(f.sx + f.sz) * VOXEL_M);
        // Far enough that nothing outside it could reach this candidate.
        const int look = spaced ? int(ceilf(2.0f * maxRad / stride)) : 0;

        // Where the cell based at (bx, bz) would put its model, and how wide.
        // Returns false when that cell places nothing.
        auto candidate = [&](float bx, float bz, float *ox, float *oz, float *orad,
                             uint32_t *oprio) -> bool {
            const uint32_t c = hashU32(uint32_t(int(bx * 16.0f)) ^ salt,
                                       uint32_t(int(bz * 16.0f)));
            if (hashUnit(seed + 41u, c) >= density) return false;
            *ox = bx + (hashUnit(seed + 42u, c) - 0.5f) * stride;
            *oz = bz + (hashUnit(seed + 43u, c) - 0.5f) * stride;
            int kk = int(hashUnit(seed + 44u, c) * float(foot.size())) % int(foot.size());
            if (kind == 3 && mushroomBig0 > 0 && mushroomBig0 < int(foot.size())) {
                const bool big = hashUnit(seed + 0x8B1Du, c) < 0.25f;
                const int lo = big ? mushroomBig0 : 0;
                const int hi = big ? int(foot.size()) : mushroomBig0;
                kk = lo + (int(hashUnit(seed + 0x3C7Fu, c) * float(hi - lo)) % (hi - lo));
            }
            const Footprint &f = foot[size_t(kk)];
            *orad = 0.25f * float(f.sx + f.sz) * VOXEL_M;
            *oprio = c;
            return true;
        };

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
                int extraSink = 0;

                // ---- SIT IT FLUSH, OR DO NOT PLACE IT AT ALL ----------------
                //
                // A MODEL IS PLACED ON ONE COLUMN AND SPANS MANY. Its base sits
                // `sink` voxels below the height of that column; if the terrain
                // falls away further than that anywhere under its footprint,
                // that corner leaves the ground and you can see daylight under
                // a five-metre boulder.
                //
                // This used to be a yes/no test with a fixed sink, and it was
                // switched off -- kNoRockSlopeTest -- because rejecting every
                // site that was not flat enough for a constant burial depth
                // threw away most of the good ones too. The depth was the wrong
                // thing to hold constant.
                //
                // So the sink is MEASURED instead. groundDrop finds the lowest
                // ground under the base, and the model goes down far enough to
                // meet it; on flat ground that is decorSink's number and
                // nothing changes, and on a fall-away it is however much deeper
                // it takes. Rejection is now the exception rather than the rule,
                // and only for the two cases sinking cannot fix:
                //
                //   * it would BURY the thing. Past a fraction of its own
                //     height there is no longer a rock on the ground, there is
                //     a rock in a hole.
                //   * it is a DOUBLED BOULDER on ground that is not flat. Those
                //     are the eleven biggest models in the world and the only
                //     ones whose burial reads as landscape rather than as
                //     detail, so they are held to a real flatness bar rather
                //     than being sunk into compliance.
                //
                // Ground that RISES is not tested and never was: it buries the
                // model deeper, which is what half-embedded in a bank looks
                // like, and is fine.
                // A BOULDER THAT CANNOT SIT HERE BECOMES A SMALL STONE, it does
                // not become nothing. Rejecting outright was the first version
                // and it quietly did two jobs at once: it steered the big rocks
                // onto flat ground, which was asked for, and it also thinned the
                // rocks by a further third on top of the density cut, which was
                // not. Measured over a 5 km square, the share of sites flat
                // enough for a doubled boulder is
                //
                //   base width   2.0 m   3.5 m   5.0 m   7.0 m
                //   drop <=  8    71%     39%     24%     15%
                //   drop <= 12    91%     62%     42%     27%
                //
                // -- so at eight voxels the largest models would have found a
                // home on one site in seven, and eleven of the twenty-six models
                // are now boulders. Re-rolling into the small half of the set
                // keeps the rock COUNT at the density that was asked for and
                // lets the flatness rule decide only WHICH rock stands where.
                // THE WHOLE STONE, not its contact patch, decides whether a
                // site is flat enough -- those are two different questions and
                // they want two different footprints:
                //
                //   * SEATING asks about the part that touches the ground, so
                //     the block below uses baseX/baseZ, the bottom 0.6 m slab.
                //   * STEEPNESS asks about the whole silhouette. kBaseHeightM
                //     is a fixed 0.6 m, so on a 4x boulder the contact patch is
                //     the bottom six voxels of a model over a hundred tall -- a
                //     slice that says nothing about where the ground has got to
                //     by the time it reaches an overhang twelve metres out.
                //     Measuring it would happily cantilever a twenty-metre
                //     stone off the side of a hill.
                if (kind == 1 && k < kRockBigMidEnd) {
                    const Footprint &bf = foot[size_t(k)];
                    const int bX = (yaw & 1) ? bf.sz : bf.sx;
                    const int bZ = (yaw & 1) ? bf.sx : bf.sz;
                    // THE BAR IS FLOORED AT THE STONE'S OWN SINK, and that is
                    // the whole of what makes this usable at 4x.
                    //
                    // A big rock is already buried forty voxels -- four metres
                    // -- before the terrain is consulted at all, so a fall of
                    // anything up to forty is hidden by burial it was going to
                    // have anyway. Holding a sixteen-metre stone to a flat
                    // twelve voxels is asking for a 6% slope, which almost
                    // nothing in this landscape is: measured over a 5 km
                    // square, only ~10% of sites qualify at a 14 m footprint,
                    // and with big five of twenty-six models they stopped
                    // appearing at all.
                    //
                    // So "too steep" means the drop beats what the sink would
                    // have hidden regardless. That is still a real bar, it
                    // still refuses genuinely steep ground, and it rescales
                    // itself when the classes are rescaled instead of needing a
                    // new constant every time.
                    const int allow = maxi(bigRockFlatVox, decorSink(1, k, bf.sy, seed, cell));
                    bool refuse = bX > 0 && bZ > 0 &&
                                  groundDrop(ci, cj, h, bX, bZ, memo) > allow;

                    // AND IT MUST NOT SWALLOW A TREE. Tested here, against the
                    // BOULDER's own footprint, rather than left to the generic
                    // check further down -- because the answer for a boulder
                    // has to be "put a smaller stone here", not "put nothing
                    // here".
                    //
                    // The difference is the whole wood. A twenty-metre stone
                    // has a ten-metre radius, a pine stand runs about one tree
                    // per 29 m2, and a disc that size therefore covers a dozen
                    // trunks almost everywhere. Refusing outright would mean big
                    // rocks appear only in clearings AND take a rock off the
                    // ground every time they do not. Re-rolling means the
                    // clearing decides which SIZE of stone stands there, which
                    // is the same rule the flatness bar already follows.
                    if (!refuse && avoid) {
                        const float br = 0.25f * float(bf.sx + bf.sz) * VOXEL_M;
                        for (const Disc &d : *avoid) {
                            const float ddx = d.x - x, ddz = d.z - z;
                            const float reach = br + d.r;
                            if (ddx * ddx + ddz * ddz < reach * reach) { refuse = true; break; }
                        }
                    }

                    if (refuse) {
                        const int nSmall = int(foot.size()) - kRockBigMidEnd;
                        if (nSmall <= 0) continue;
                        k = kRockBigMidEnd +
                            (int(hashUnit(seed + 0x5A17u, cell) * float(nSmall)) % nSmall);
                    }
                }

                // Whatever model it ended up as, sink it until it meets the
                // ground -- and refuse only if that would swallow it.
                {
                    const Footprint &f = foot[size_t(k)];
                    const int footX = (yaw & 1) ? f.baseZ : f.baseX;
                    const int footZ = (yaw & 1) ? f.baseX : f.baseZ;
                    if (footX > 0 && footZ > 0) {
                        const int base = decorSink(kind, k, f.sy, seed, cell);
                        const int drop = groundDrop(ci, cj, h, footX, footZ, memo);
                        const int total = maxi(base, drop);
                        if (f.sy > 0 && float(total) > maxBuryFrac * float(f.sy)) continue;
                        extraSink = maxi(0, total - base);
                    }
                }

                // The overlap rejection described above. Last, so it only
                // runs for a placement everything else has already accepted.
                if (spaced) {
                    float mx = 0.0f, mz = 0.0f, mr = 0.0f;
                    uint32_t mp = 0u;
                    bool clear = true;
                    if (candidate(bx, bz, &mx, &mz, &mr, &mp)) {
                        for (int dj = -look; dj <= look && clear; ++dj)
                            for (int di = -look; di <= look; ++di) {
                                if (di == 0 && dj == 0) continue;
                                float nx = 0.0f, nz = 0.0f, nr = 0.0f;
                                uint32_t np = 0u;
                                if (!candidate(bx + float(di) * stride,
                                               bz + float(dj) * stride, &nx, &nz, &nr, &np))
                                    continue;
                                // Ties broken on the hash, so the pair agrees.
                                if (np <= mp) continue;
                                const float ddx = nx - mx, ddz = nz - mz;
                                // 0.9, not 1.0: two stones may touch. It is
                                // standing INSIDE one another that looks wrong.
                                const float reach = (mr + nr) * 0.9f;
                                if (ddx * ddx + ddz * ddz < reach * reach) {
                                    clear = false;
                                    break;
                                }
                            }
                    }
                    if (!clear) continue;
                }

                // NOTHING SMALL GROWS OUT OF A BOULDER. Last of all the
                // tests, so it only runs for a placement everything else has
                // already accepted, and a linear scan because a chunk holds a
                // couple of dozen rocks at most.
                //
                // No slack here, unlike the same-kind spacing above, which
                // allows a 0.9 overlap so two stones may touch. A mushroom is
                // not touching a rock, it is INSIDE it, and there is no
                // distance at which that reads as intentional.
                if (avoid) {
                    const Footprint &sf = foot[size_t(k)];
                    const float mr = 0.25f * float(sf.sx + sf.sz) * VOXEL_M;
                    bool inside = false;
                    for (const Disc &d : *avoid) {
                        const float ddx = d.x - x, ddz = d.z - z;
                        const float reach = mr + d.r;
                        if (ddx * ddx + ddz * ddz < reach * reach) { inside = true; break; }
                    }
                    if (inside) continue;
                }

                b->decor.push_back({kind, k, ci, cj, h, yaw, cell, 0, extraSink});
            }
        }
    }
};

}  // namespace v7
