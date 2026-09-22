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

#include "voxel/expand.h"
#include "player/collide.h"
#include "world/voxelworld.h"

namespace v2 {

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
    // Whether this chunk drew any water. Only these need re-meshing when the
    // swell moves, and on this world that is under one chunk in a hundred --
    // which is the entire reason an animated lake is affordable.
    bool hasWater = false;
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
        threads_ = threads;
        stop_ = false;
        for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
    }

    // -----------------------------------------------------------------------
    // THE GENERATOR CHANGED, AND THE ONLY WAY TO TELL THE WORKERS IS TO
    // REPLACE THEM.
    //
    // (user 2026-09-20: "snow is not working at all" -- and `--snow` at
    // launch works perfectly, which is the whole clue.)
    //
    // A SETTING THAT CHANGES THE GROUND CANNOT BE SET ON World::terrain
    // ALONE. The note over start() says the terrain is copied rather than
    // referenced, and run() then takes a SECOND copy per worker -- `terr` --
    // once, outside the job loop, with that worker's BrickStore built around
    // it. So a worker's view of the world is fixed for the life of the
    // thread, and its brick cache is fixed with it. Setting snowOn on the
    // main thread's terrain and erasing every chunk does exactly nothing:
    // they are re-meshed, by the same workers, out of the same generator and
    // the same cached bricks, and come back identical.
    //
    // RESTARTING IS NOT A HEAVY HAMMER, IT IS THE ONLY CORRECT ONE. Handing
    // a live worker a new terrain mid-chunk is the bug start()'s note exists
    // to prevent, and even done safely it would leave every BrickStore full
    // of the old world's bricks. Ending the threads ends the caches with
    // them, which is the point rather than a side effect.
    //
    // THE QUEUES GO TOO. Anything pending describes a world that no longer
    // exists, and anything finished would be adopted as if it did.
    // -----------------------------------------------------------------------
    void restart(const VoxelTerrain &terrain) {
        joinWorkers();
        {
            std::lock_guard<std::mutex> lk(inMx_);
            pending_.clear();
            busy_ = 0;
        }
        {
            std::lock_guard<std::mutex> lk(outMx_);
            done_.clear();
        }
        terrain_ = terrain;
        stop_ = false;
        for (int i = 0; i < threads_; ++i) workers_.emplace_back([this] { run(); });
    }

    ~ChunkMesher() { joinWorkers(); }

    void joinWorkers() {
        {
            std::lock_guard<std::mutex> lk(inMx_);
            stop_ = true;
        }
        inCv_.notify_all();
        for (auto &t : workers_) if (t.joinable()) t.join();
        workers_.clear();
    }

    // -----------------------------------------------------------------------
    // `urgent` PUTS THE CHUNK AT THE FRONT, AND IT IS THE WHOLE OF THE HOE BUG.
    //
    // (user 2026-09-17: "the tilling with the hoe is still glitching out the
    //  terrain. pursue it further".)
    //
    // THIS QUEUE IS FIFO AND AN EDIT WAS JOINING THE BACK OF IT. Streaming
    // fills it continuously while the player walks -- that is what it is for --
    // and a chunk costs about 46 ms to mesh, so a hoe swing's re-mesh sat
    // behind however many chunks the streamer happened to have outstanding.
    // MEASURED, standing still and tilling at frame 15: the 81 columns are in
    // the edit layer immediately and the ground still had not changed on screen
    // at frame 34, nineteen frames later. Rendered with --no-dlss as well, so
    // it is not the denoiser holding a stale image: the GEOMETRY was still the
    // old ground.
    //
    // That is the glitch. You swing, the hoe knocks, the earth does not turn --
    // and then some fraction of a second later a disc of it pops. It gets worse
    // the more the streamer has to do, which is why it shows up while walking
    // and why chasing it as a frame-time spike only ever explained part of it.
    //
    // AN EDIT IS NOT STREAMING. A streamed chunk is at the edge of sight and
    // nobody can tell what order those land in; an edit is under the crosshair,
    // the player caused it, and it is the only thing they are looking at. So it
    // goes to the front. It cannot starve the streamer: the player can only
    // make a handful of edits a second and each is one or two chunks.
    void request(int cx, int cz, bool urgent = false) {
        {
            std::lock_guard<std::mutex> lk(inMx_);
            if (urgent)
                pending_.push_front({cx, cz});
            else
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
    // sx, sz, sy is the model's bounding box; baseX and baseZ the footprint
    // that meets the ground, and cx, cz where that footprint sits relative to
    // the box centre, in metres. The last two are what make a disc land on a
    // birch's TRUNK rather than under the middle of its crown -- see the note
    // on ModelCollider::baseCX in scene/collide.h.
    struct Footprint { int sx, sz, sy, baseX, baseZ; float cx, cz; };
    // GROUND SOMETHING ALREADY OCCUPIES: a box of half extents hx, hz with a
    // circle of radius r rolled around it. Both degenerate cases are used and
    // that is the point of writing it this way --
    //
    //   * A BOULDER is a circle: hx = hz = 0, r its radius. A stone is lumpy
    //     and round-ish, and a circle is the honest shape for it.
    //   * A TRUNK is a box: r = 0, hx and hz its base footprint. A circle
    //     INSCRIBES that footprint and therefore leaves the four corners
    //     unguarded, which is exactly where the last of the mushrooms were
    //     still standing in the bark -- measured at 7 in 1213 after the discs
    //     were re-centred, each a voxel or two deep.
    //
    // One expression covers both: push the query point out of the box first,
    // then compare what is left against the radii. With hx = hz = 0 that is
    // the circle-circle test this used to be, unchanged.
    struct Disc {
        float x, z, r;
        float hx = 0.0f, hz = 0.0f;
        // Does a circle of radius q at (qx, qz) reach this?
        bool reaches(float qx, float qz, float q) const {
            const float dx = maxf(0.0f, fabsf(qx - x) - hx);
            const float dz = maxf(0.0f, fabsf(qz - z) - hz);
            const float reach = q + r;
            return dx * dx + dz * dz < reach * reach;
        }
    };
    // The rock scatter's grid, named because two passes now have to agree about
    // it: the one that places rocks and the one that avoids them.
    static constexpr float kRockStride = 1.6f;
    // HOW FAR APART TWO OF THE SAME STONE HAVE TO BE.
    //
    // (user 2026-09-21: "prevent spawning THE SAME type of rock next to each
    //  other".)
    //
    // NOT a spacing rule -- two DIFFERENT boulders side by side is a rock
    // field and reads fine. What does not read is the same model twice in one
    // glance, because a voxel stone has a silhouette you recognise and the
    // repeat looks stamped. Fourteen metres is about the depth of a stand
    // (the same number the scatter notes use for "one view"), so a repeat is
    // pushed out of the glance that would pair them.
    static constexpr float kRockSameApartM = 14.0f;
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
    // -- THE DESERT'S TWO SCATTERS, WHICH ARE v1's ------------------------
    //
    // (user 2026-09-19: "import the desert assets from v1 ... create the
    //  desert biome now.")
    //
    // Nine cacti and six scrub bushes, out of the same asset folders v1 reads
    // (game/assets/foilage/cactus and desert_shrub). They go through
    // scatterSmall like the flowers and the mushrooms rather than through the
    // tree scatter: v1 calls them "the desert's second and third scatter" and
    // they are decorations with a footprint, not a stand with a canopy and a
    // spacing rejection.
    //
    // THE DENSITY IS THE GATE. scatterSmall multiplies by desertMix for these
    // two kinds, so they simply do not exist outside the sand and thin out
    // across the rim with it -- the same dither the floor uses. v1 gates them
    // on `desertM >= 0.85` and its note calls that "an ADMIT test, not a
    // reject"; a weight IS that test, with a soft edge for free.
    std::vector<Footprint> cactusFoot, shrubFoot;
    // HALVED 2026-09-19 ("reduce the cactus population by 50%. this includes
    // the shrubs as well"), from 0.055 and 0.10. The cacti are also twice the
    // size they were, so the sand reads fuller at half the count than it did
    // at the full one.
    float cactusDensity = 0.0275f;
    float shrubDensity = 0.05f;
    // The beehive's footprint. Empty in the pine wood, which is what turns the
    // hive pass off there -- no flag needed.
    std::vector<Footprint> hiveFoot;
    // Per pine model, every crown voxel resting on wood -- see collectPerches.
    std::vector<std::vector<Perch>> pinePerch;
    // Where the birches begin in pineFoot / pinePerch. Pines are [0, birchBase)
    // and birches [birchBase, size) -- see loadPines. 0 means every model is a
    // birch, size means every one is a pine, and both are what --birch / --pine
    // produce.
    int birchBase = 0;
    // ...and where the OAKS begin. Birches are [birchBase, oakBase) and oaks
    // [oakBase, size) -- see loadPines. Equal to the array size means no oak
    // models were loaded, which is what --pine and --birch produce.
    int oakBase = 0;
    // ...AND WHERE THE CHERRIES BEGIN, which is the same three models again in
    // blossom -- see tools/cherry_from_oak.py. Oaks are [oakBase, cherryBase)
    // and cherries [cherryBase, size). Equal to the array size means no cherry
    // models loaded and the cherry band plants ordinary oaks, which is the
    // right failure: a green wood rather than a bald strip.
    //
    // THE SCATTER DOES NOT DRAW FOR IT. There is no fourth species roll, on
    // purpose: a cherry wood IS an oak wood -- same footprints, same spacing,
    // same yaws, same everything -- so the draw stays three-way and the CHOSEN
    // oak is shifted into this range afterwards. That is what makes "identical
    // to the oak forest" true by construction instead of by two tables being
    // kept in step.
    int cherryBase = 0;
    // -- ...AND THE PALER HALF OF THE BLOSSOM ---------------------------
    //
    // (user 2026-09-19: "create a light pink variant of half the cherry trees.
    //  half the cherry trees are regular pink and the other half is lighter
    //  pink".)
    //
    // A FIFTH range, [cherryLightBase, size), holding the same three models a
    // third time under a paler ramp -- see tools/cherry_from_oak.py --variant
    // light. Equal to the array size means no light set was loaded, and the
    // whole wood is the ordinary blossom.
    int cherryLightBase = 0;
    // How much of the lattice the oak fills. Well under the birch's 0.84: an
    // oak_7 is seventeen metres across and the spacing rejection below keeps
    // 0.30 of a footprint clear, so a high roll here would be spent almost
    // entirely on candidates that are then thrown away for standing in each
    // other. Sparse and large is what an oak wood is.
    //
    // HALVED BY HEADCOUNT, NOT BY KNOB (user 2026-09-19: "cut the oak tree
    // density by 50%", then "actually cut it in half").
    //
    // 0.21 -> 0.0975. THE TWO ARE NOT THE SAME THING and that is the whole
    // reason this number looks arbitrary. This knob is the share of lattice
    // cells that OFFER a candidate; the spacing rejection then culls from that,
    // so the headcount does not track it one for one. Measured over --ouachita,
    // which is all oak, so the printed tree count IS the oak count:
    //
    //     density   trees    of baseline
    //     0.21      1305     100%     <- baseline
    //     0.117      758      58.1%
    //     0.105      688      52.7%   <- "half the knob" is NOT half the wood
    //     0.0993     661      50.6%
    //     0.0975     652      49.96%  <- half of 1305 is 652.5
    //
    // That is count ~ density^0.93, i.e. SUBLINEAR: halving the knob removes
    // slightly LESS than half the trees, because the ground freed by each tree
    // that goes is partly re-offered to its neighbours. The note this replaces
    // recorded the opposite (0.42 -> 0.21 giving 44%, an exponent of 1.18) and
    // it is not wrong -- the oaks were rebaked half again as wide since, and a
    // wider tree spends more of the knob on candidates that are then rejected
    // for standing in each other. So the exponent is a property of the CURRENT
    // models and must be re-measured whenever they change. Do not carry it.
    //
    // The count is spawn-independent here -- --spawn 7 and --spawn 42 both give
    // 1305 and 661 -- because --ouachita scores its own spawn and this total is
    // the whole world's decor rather than a view ring. That makes it a better
    // measurement than the per-spawn one the old note used.
    //
    // To re-measure:
    //   v2.exe --background --ouachita --oak-density <d> --out x.png | grep trees
    //
    // WAS 0.42 -> 0.21 (user 2026-09-17: "reduce the oak trees in half"), whose
    // measurement over one wood at --spawn 7 was 3079 trees -> 1355 (44%).
    //
    // THIS DEFAULT IS NOW OVERWRITTEN by World::oakDensity, which app_load
    // pushes from Options (--oak-density). It stays as the value a mesher built
    // without a World would use.
    float oakDensity = 0.0975f;
    // The subset of those wide enough to hang a beehive from -- empty in the
    // pine wood, which is one of the two things that turns the hive pass off.
    std::vector<std::vector<Perch>> pineHivePerch;
    // -- AND THE ORCHARD --------------------------------------------------
    //
    // The two fruit models, and per OAK model the anchors a crop can hang
    // from, angle-sorted so it rings the crown -- see fruitAnchors. Both are
    // empty outside the oak wood, which is what turns the pass off there:
    // the same no-flag arrangement hiveFoot uses for the pines.
    // A crop is at most this many, which is the browser engine's cap moved
    // 10 -> 20 when the counts were doubled: left at 10 the two giant tiers
    // would both clip to it and the doubling would land on every tier EXCEPT
    // the ones it was asked for. Also the size of hangFruit's column scan.
    static constexpr int kFruitMax = 20;
    // ...and what the count is a function of -- see hangFruit for how this was
    // solved rather than picked.
    static constexpr int kFruitFootDiv = 10000;
    // NO FRUIT BELOW 4 m. oak_1 is a 2.1 m bush and the next model up is 11 m,
    // so anywhere between them draws the same line; 40 voxels says WHY in the
    // units the argument is made in.
    static constexpr int kFruitMinTreeVox = 40;
    std::vector<Footprint> fruitFoot;
    std::vector<std::vector<Perch>> oakFruitPerch;
    int pineconesPerTree = 14;
    // mushroomFoot holds the small models first and the doubled ones after it.
    // Everything at or past this index is a big one.
    int mushroomBig0 = 0;
    // THE BIRCH FLOWERS BEGIN HERE. One file, loaded twice: once with its
    // stems mapped to the pine's grass ramp and once to the birch's, so a
    // flower's stem is the same green as the blade it is standing in. Same
    // shape as mushroomBig0 above.
    int flowerBirch0 = 0;
    // -- THE CHERRY WOOD'S COPY OF EACH DECORATION SET, 2026-09-19 --------
    //
    // (user: pink moss, pink flowers, pink mushrooms.)
    //
    // THE TREES' POSITIONAL RULE, three more times: everything before the base
    // is the ordinary set and everything from it is the same models wearing
    // mat::CPINK_0. Each copy is a FULL duplicate of what precedes it, which is
    // what lets the scatter shift be one line -- an index that was valid is
    // valid plus the base, so the big/small mushroom draw and the pine/birch
    // flower split both keep working inside the copy at the same offsets.
    //
    // Zero, or equal to the set's size, means no copy was loaded and the
    // cherry band gets the ordinary decoration -- which is the wood this was
    // before today.
    int rockCherry0 = 0;
    // ...AND THE SAND'S, WHICH IS THE SAME STONES WITH NO MOSS ON THEM. It
    // follows the cherry copy, so the ordinary set is [0, rockCherry0), the
    // pink one [rockCherry0, rockDesert0) and the bare one [rockDesert0, size).
    int rockDesert0 = 0;
    // WHERE THE BOULDERS STOP within one rock set -- big and mid are
    // [0, rockSmall0) and the pebbles run from it. See the desert's size cut
    // in scatterSmall, and World::loadRocks, which records it.
    int rockSmall0 = 0;
    // The mismatch report above runs on mesher threads, so its counter is
    // atomic and capped -- a wrong band would otherwise print per stone.
    mutable std::atomic<int> rockWarn_{0};

    // -- HOW MANY OF rockFoot ARE THE ORDINARY SET ------------------------
    //
    // The copies are appended, so the ordinary (green-moss) stones are
    // [0, FIRST COPY). Both places that pick a rock have to draw from that
    // range and then shift -- and for a while only one of them did:
    // collectRocks kept rolling over the whole list, so the disc it reserved
    // for a boulder was a DIFFERENT boulder's disc, and the flowers and
    // mushrooms it is there to keep out of the stone went back to growing
    // inside it.
    //
    // THE FIRST COPY, not the cherry one: a world pinned to the desert loads
    // the bare set and no pink one, so rockCherry0 is zero there and the bare
    // set is the first.
    int rockOrdinary() const {
        const int first = (rockCherry0 > 0 && rockDesert0 > 0)
                              ? mini(rockCherry0, rockDesert0)
                              : maxi(rockCherry0, rockDesert0);
        return (first > 0 && first < int(rockFoot.size())) ? first : int(rockFoot.size());
    }
    int flowerCherry0 = 0;
    int mushroomCherry0 = 0;
    // ...AND THE BLOSSOM'S BUSH, which is the desert's scrub with its flowers
    // turned pink -- see World::loadCherryBush. [0, shrubCherry0) is the red
    // one and [shrubCherry0, size) the pink.
    int shrubCherry0 = 0;
    // Density INSIDE a colony now, not over the whole wood: the patches cover
    // a fifth of the ground, so the old 0.22 spread over everything is about
    // this much concentrated into them. A bed wants to look like a bed.
    // Rocks down a quarter, 0.010 -> 0.0075. Asked for alongside the doubling
    // of the mid stones, and the two go together: eleven of the models are now
    // boulders rather than six, so the same density would have put noticeably
    // more large rock in the wood than before rather than the same amount at a
    // new size.
    // -----------------------------------------------------------------------
    // 0.24, WHICH IS v1's 0.08 THROUGH v2's GRASS FILTER.
    //
    // v1 plants on 8% of its candidate cells. v2 cannot use that number
    // directly, because scatterSmall is called with grassOnly and **a pine
    // column carries a blade only 31.8% of the time** -- so 0.08 here is 0.025
    // on the ground, and 0.08 measured 1 flower per 2,412 columns against v1's
    // 800. The filter is right and stays (a flower grows out of grass, not out
    // of bare litter); the rate in front of it has to be divided by it.
    //
    // MEASURED over the engine's own 625-chunk ring, as "one flower per N
    // columns" so it can be read against v1's 800 directly:
    //
    //              pine            birch
    //     0.08     1 per 2412      1 per 1332
    //     0.16     1 per 1203      1 per  668
    //     0.24     1 per  803      1 per  445     <-- shipped
    //     0.32     1 per  601      1 per  333
    //
    // 0.24 puts the PINE on v1's number to within half a per cent. The birch is
    // denser, at 445, and that is right twice over: its floor grows blades on
    // 63.6% of columns rather than 31.8, and v1 gives its oak/birch band the
    // full rate while thinning the pine forest to 0.375 of it. The ratio here
    // comes out of the ground cover rather than a second constant, which is the
    // same relationship without a number to keep in step.
    // -----------------------------------------------------------------------
    // HALVED, 0.24 -> 0.12 (user 2026-09-13: "reduce the frequency of the
    // flowers in half"). That is one flower per ~1,600 columns in the pine and
    // ~890 in the birch, against v1's 800 -- so v2 now sits at half v1's meadow
    // in the pine and just under it in the birch. The table above is still the
    // map: every figure in it doubles.
    float rockDensity = 0.0075f, flowerDensity = 0.12f;
    // -----------------------------------------------------------------------
    // THE FLOWERS ARE v1's NOW (user 2026-09-13, second pass: "the flowers are
    // still not right, still too sparse. look to v1s code into how to do the
    // flowers properly").
    //
    // THE STRUCTURE WAS WRONG, NOT THE NUMBER. Tripling a colony field three
    // times over never got there because v2 and v1 disagree about what a patch
    // IS:
    //
    //   v2   a colony GATES PRESENCE. Most of the wood has no bed on it at all,
    //        and flowers exist only inside the beds. Push it and you either run
    //        out of beds to add (coverage tops out) or the beds merge.
    //   v1   a uniform meadow. EVERY cell may carry a flower at one flat rate;
    //        the coarse patch decides only WHICH SPECIES. What reads as
    //        "grouped" is a drift of one colour into another, not a clump of
    //        plants in bare ground.
    //
    // v1's own numbers, from flowerAt in game/index.html:
    //
    //     FLWCELL   8 voxels    one candidate per 64 columns
    //     rate      0.08        "0.08 of cells, a QUARTER of the old
    //                            per-column density"
    //     FLWPATCH  12 cells    96 voxels of one species -- "holds ~11
    //                            flowers ... a drift of one colour into
    //                            another rather than a tile"
    //
    // which is one flower per 800 columns, or one per 8 square metres at 10 cm
    // voxels. v2 was at one per 8,844 columns -- ELEVEN TIMES sparser -- and no
    // amount of coverage was going to close that, because 96% coverage of an
    // 18 m lattice is still only one bed per 18 m.
    //
    // So the colony keeps its species half and loses its presence half, the
    // scatter grid drops from 1.35 m to v1's 0.8, and the rate becomes v1's
    // 0.08. See scatterSmall and colonyAt.
    //
    // THE PINE'S 0.375 IS NOT COPIED. v1 thins the pine forest to 37.5% of the
    // oak's rate and gives the oak and birch the full one. The ask here was "in
    // all biomes", twice, so both woods get the full rate.
    // -----------------------------------------------------------------------
    // (What follows is the previous pass, kept because its measurements are
    // still the reason the colony field is shaped the way it is.)
    // THREE TIMES THE FLOWERS (user 2026-09-13: "triple the rate of the
    // flowers. in all biomes. keep them grouped and somewhat spread out").
    //
    // These two are what moved, and BOTH of them had to become names first --
    // the coverage was a bare `> 0.68` inside colonyAt and the cell was a
    // static constexpr, so neither could be swept without a rebuild.
    //
    // WHY NOT THE OBVIOUS KNOB. There are four ways to get more flowers and
    // three of them break the other half of the request:
    //
    //   flowerDensity   how crowded one bed is. Tripling it packs the SAME
    //                   patches until they read as bedding rather than as a
    //                   plant that spread from one root. Left at 0.45, so a
    //                   bed looks exactly as it did.
    //   colony radius   makes each bed bigger. Three times the area is 1.7x
    //                   the radius, and at a 15 m lattice neighbouring beds
    //                   then overlap -- which is two species mixed again, the
    //                   one thing the colony field exists to prevent.
    //   coverage alone  tops out. Measured: even at 1.0 -- every site live,
    //                   which is the even wash colonyAt's own note warns about
    //                   -- an 18 m lattice only reaches 2.90x.
    //   CELL SIZE       more beds, same size, same spacing rules. This one.
    //
    // So it is mostly the lattice (18 -> 15 m, 1.44x the sites) and the rest
    // coverage (0.32 -> 0.72), which together put down 2.3x as many beds. The
    // count rises by more than the beds do because a denser lattice also fills
    // the gaps BETWEEN beds that the old spacing left, and by less than the
    // product because new sites land on ground an existing colony already
    // covered and colonyAt keeps only the strongest.
    //
    // MEASURED over the engine's own 625-chunk ring, both woods, at the three
    // places the pine-density notes use:
    //
    //                       pine                birch
    //     400, 0        1605 -> 4632  2.89x   3466 -> 10020  2.89x
    //     2000, 1500    1355 -> 4342  3.20x   3290 -> 10287  3.13x
    //     400, -3000    1245 -> 4254  3.42x   2970 -> 10072  3.39x
    //
    // Mean 3.15x. It runs a little over three where the wood was sparsest,
    // which is the right way round: those are the places that read as having
    // no flowers at all.
    //
    // A QUARTER OF THE GROUND HAS FLOWERS ON IT NOW, against an eighth before
    // -- 0.72 x (mean bed area 78 m^2) / (15 m cell) -- so there is still three
    // times as much bare floor as flowered, and the beds are still beds.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHAT FRACTION OF THE SPECIES REGIONS GROW ANYTHING AT ALL.
    //
    // 0.5 (user 2026-09-13: "reduce the amount of flowers in half again. just
    // reduce the amount of groups, but not the amount WITHIN a group").
    //
    // THIS IS THE ONLY KNOB THAT DOES THAT. flowerDensity is how crowded a bed
    // is and halving it would thin every bed; colonyCellM is how big a bed is
    // and shrinking it would make them all smaller. Making half the regions
    // BARREN leaves the surviving ones exactly as full and exactly as large as
    // they were, and there are half as many of them -- which is the sentence
    // above, read literally.
    //
    // A BARREN REGION IS NOT A GAP IN A LATTICE. The regions are a jittered
    // Voronoi, so a dead one is an irregular blob of bare floor between two
    // live beds, and its edges are the same wandering lines every other
    // boundary has. Turning off half a SQUARE grid would have read as a
    // chequerboard.
    // -----------------------------------------------------------------------
    float colonyCoverage = 0.5f;
    // HOW BIG ONE SPECIES' DRIFT IS. v1's FLWPATCH x FLWCELL = 12 x 8 = 96
    // voxels; this is that in metres. It is no longer a spacing between beds,
    // because there are no beds -- it is the scale at which the colour changes.
    // -----------------------------------------------------------------------
    // 10 m ACROSS (user 2026-09-13: "the flower groups are too big, reduce the
    // size of the group in half across all flowers"). This is the LINEAR
    // extent halved, 20 -> 10, which is what "half the size" says about
    // something you are looking at on the ground; the area goes to a quarter
    // and so does the flower count in a group.
    //
    // IT IS BACK NEAR v1's 9.6 AND THAT IS FINE NOW, which is worth stating
    // because the note this replaces argued the opposite. 20 was chosen to stop
    // the wood reading as a seed packet -- but that was never really the size,
    // it was the SHAPE: the regions tiled on an axis-aligned floor() grid, so
    // four species met at every lattice corner along ruled seams. colonyAt uses
    // a jittered Voronoi now, where a point belongs to exactly one site by
    // construction and two regions meet along one wandering line. With that
    // fixed, small regions read as separate beds rather than as confetti, and
    // the size is free to be whatever looks right.
    //
    // WHAT A GROUP HOLDS, at flowerDensity 0.12: about five flowers in the pine
    // and eleven in the birch, the difference being that the birch floor grows
    // blades on twice as many columns and a flower needs one. v1's own note
    // calls ~11 "enough to read as a patch of roses"; the pine is thinner than
    // that, and it is thin because the rate was halved and the group halved on
    // top of it. Both were asked for, in that order.
    // -----------------------------------------------------------------------
    float colonyCellM = 10.0f;
    // ONE CANDIDATE PER 0.8 m, v1's FLWCELL of 8 voxels. It is BOTH the grid
    // and the jitter -- a candidate is thrown up to half a cell either way --
    // and v1's note says why it is not larger: "the cell also sets the MINIMUM
    // SPACING ... much larger than 8 turns a uniform meadow into visible
    // clumps-and-gaps".
    float flowerStrideM = 0.8f;
    // Mushrooms are NOT colonised the way flowers are. A flower bed is a patch
    // and reads wrong scattered evenly; a mushroom in a conifer wood is mostly
    // a thing you come across on its own, so this is a flat low probability
    // over any grass the wood will grow.
    float mushroomDensity = 0.015f;
    // HALF AS MANY MUSHROOMS UNDER THE BIRCHES. A fungus in a conifer wood
    // lives on the needle litter, and the birch floor has none -- so this is
    // the pine's density scaled per position rather than a second constant, and
    // retuning the pine wood still moves both together.
    float birchMushroomScale = 0.5f;
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
    //
    // A QUARTER MORE PINES -- AND THE KNOB HAD TO GO UP BY 38% TO BUY THEM.
    // 0.2325 -> 0.3210.
    //
    // Asked for as a quarter more trees, and 0.2906 -- the knob itself plus a
    // quarter -- is not that, for the same reason the paragraph above gives
    // read backwards: offering a quarter more candidates puts proportionally
    // more of them within a trunk's width of one already standing, and the
    // spacing test takes the difference. 0.29 was measured at 7069 trees, which
    // is +16.9%, so two thirds of what it was asked for. The elasticity here is
    // about 0.66 -- a percent on the knob buys two thirds of a percent of wood
    // -- and it keeps falling as the stand thickens, which is why this was
    // solved by sweeping rather than by scaling.
    //
    // MEASURED, ring for ring on a pinned pine wood, at the three places the
    // birch notes below use:
    //
    //     400, 0        6046 -> 7557    +25.0%
    //     2000, 1500    5822 -> 7283    +25.1%
    //     400, -3000    6285 -> 7900    +25.7%
    //
    // THE LANDFORM CANNOT MOVE THESE NUMBERS, which is worth writing down
    // because it is not obvious and it is what made the smoothing in heightM
    // free: every one of the six figures above came back identical on the
    // rounded field. A tree's PLACE is decided by the lattice, the jitter, the
    // stand-density gate and the spacing test, none of which can see the
    // height; the two gates that can are the shore band and kTreeSlope, and
    // over a 625-chunk ring neither fires on ground either field produces. The
    // wood stands where it always stood, on smoother hills.
    //
    // The gate this multiplies tops out at 0.97, so there is still two thirds
    // of the knob's range left above this -- at 1.0 every cell the stand
    // density admits would take a tree and the clumping would flatten into an
    // even field, which is the thing that gate exists to prevent.
    // -----------------------------------------------------------------------
    // HOW A STAND-DENSITY READING BECOMES A PLANTING PROBABILITY.
    //
    // It was `saturate((dens - 0.30) / 0.32) * 0.92 + 0.05` -- linear in the
    // field, so every part of the wood scaled together and "more trees" could
    // only ever mean "more trees everywhere in the same proportion".
    //
    // SQUARED, so the increase lands in the THICKETS. The user asked for a
    // quarter more pine with the denser parts filled in further, and those are
    // two different requests: the first is the mean, the second is the shape.
    // Solved against a 141,000-sample survey of the field so the total is
    // exactly +25% and the distribution is what moved:
    //
    //     clearings  1.10x     mid wood  0.97x     thickets  1.47x
    //
    // The floor is RAISED to 0.090 as the curve is squared, which is what
    // keeps the clearings from emptying -- s^2 alone took them to 0.56x, and a
    // wood whose glades are barer is not what was asked for. Squaring with a
    // low floor also thins the mid wood hard (0.93x); at 0.090 it is 0.97x,
    // near enough untouched.
    // -----------------------------------------------------------------------
    // -- THE STAND GATE, AND ITS FOUR NUMBERS ARE NAMED NOW -----------------
    //
    // They were literals inside this function, which made the one question
    // worth asking about them impossible to answer cheaply: a total can be
    // raised entirely inside stands that were already thick, and that is
    // exactly the change nobody wants. Named and non-static, the scatter
    // harness can sweep them against THIS code rather than against a copy of
    // the formula that would drift from it.
    //
    //   knee/span  where the noise starts producing trees, and over what range
    //   gain       how thick the thickest stand gets
    //   floor      and what a CLEARING still gets, which is the one that fills
    //              the sparse areas in
    float standKnee = 0.30f, standSpan = 0.32f;
    // -- THE FLOOR IS 0.262 NOW, AND IT IS THE WHOLE OF THE +25% ------------
    //
    // "Increase the pine trees by 25%. fill in the sparse areas more." Those
    // are two requests and this one number answers both, which is why nothing
    // else in the gate moved.
    //
    // THE FLOOR IS WHAT A CLEARING GETS. Raising the GAIN would have put the
    // extra quarter where the wood was already thick -- a bigger total and the
    // same bare patches, which is the opposite of what was asked. Raising the
    // floor adds the same probability to every cell in the wood, so all of it
    // lands in the cells that had least.
    //
    // Measured over a 625-chunk pine ring with the scatter harness:
    //
    //                       total   /ha    p10   bottom decile   empty chunks
    //   0.090 (before)       8432   205.9    3       0.95             27
    //   0.262 (now)         10553   257.6    7       2.81             20
    //
    // +25.15% overall, and the thin end very nearly trebles. The thick stands
    // are untouched: p90 went 27 to 29 and the max 33 to 36, which is the floor
    // arriving there too and nothing more.
    float standGain = 1.20f, standFloor = 0.262f;

    float standGate(float dens) const {
        const float s = saturate((dens - standKnee) / standSpan);
        return s * s * standGain + standFloor;
    }

    // 0.3666: 0.3210 x 1.142. Not x1.25 -- squaring the curve raises its own
    // mean from 0.602 to 0.659, so the density only has to make up the rest.
    // The measured total is +25.0%.
    float treeDensity = 0.3666f;  // was 0.55, less a quarter three times, then +25% of wood

    // The closest two trunks may ever stand, whatever the stand table asks for.
    // It was the floor of the old canopy rule and keeps that job; what it no
    // longer shares is the job of spacing an OPEN wood, which the canopy term
    // beside it does on its own. See the clash test.
    float kTrunkKeepM = 1.5f;
    float treeStride = 2.4f;

    // ---- the birch wood ----------------------------------------------------
    //
    // 4.4 m between candidates against the pine's 2.4, carried over from the
    // browser engine's BKCELL = 44. A birch crown is wider than a pine's and
    // the wood is meant to read as open rather than dense, so the same tree
    // density on a coarser grid gives a stand you can see through.
    //
    // THE CLUMPING IS THE PINE'S, UNCHANGED. Both woods run the same
    // standDensity gate, which is a sixty-metre field -- so both get the same
    // behaviour of thick stands with clearings between them rather than trees
    // spread evenly. Only the grid under it changes.
    float birchStride = 4.4f;
    // A QUARTER FEWER BIRCHES, TWICE OVER. 0.84 -> 0.54 -> 0.363.
    //
    // -- THE SECOND QUARTER, 0.54 -> 0.363 -------------------------------
    //
    // Asked for the same way and paid for the same way, but it cost LESS of
    // the knob than the first one did: a 32.8% cut where the first took 35.7%.
    // That is the note below read forwards rather than backwards. The wood is
    // thinner than it was, so it sits further from the packing the spacing test
    // will allow, so less of what it stops offering was going to be rejected
    // anyway -- and the knob therefore has to over-reach by less to deliver the
    // same quarter.
    //
    // MEASURED at the same three places, ring for ring on a pinned birch wood:
    //
    //     400, 0        8977 -> 6734    -25.0%
    //     2000, 1500    8839 -> 6655    -24.7%
    //     400, -3000    9500 -> 7040    -25.9%
    //
    // AND THE PINE WOOD IS BIT FOR BIT UNCHANGED, which is the check this knob
    // always has to pass: a pinned pine wood gives 7557 trees and 967 rocks at
    // 400, 0 before and after, because nothing reads this outside the isBirch
    // branch. The hives thin with the birches as before -- 75 -> 58 at 400, 0 --
    // for the same reason they did the first time: one birch in a hundred is
    // still one birch in a hundred.
    //
    // -- THE FIRST QUARTER, 0.84 -> 0.54 ---------------------------------
    //
    // Asked for as a quarter off the frequency, and 0.63 -- the knob itself
    // less a quarter -- is not that. This is a probability per CANDIDATE, and
    // the spacing test below rejects fewer of the survivors as the wood thins,
    // so offering a quarter fewer gave back only 17% fewer trees. The birch
    // feels that far more than the pine does (see treeDensity, which loses
    // 22-24% for the same cut): the same 625-chunk ring holds 12002 birches
    // where a pinned pine wood holds 6010, which puts the birch much closer
    // to the packing the spacing test will allow, so more of what it stops
    // offering was going to be rejected anyway.
    //
    // MEASURED ring for ring on a pinned birch wood, at the three places the
    // birchExtra note below uses, with the trees at their present height:
    //
    //     400, 0        12002 -> 8977    -25.2%
    //     2000, 1500    11849 -> 8839    -25.4%
    //     400, -3000    12685 -> 9500    -25.1%
    //
    // It scales EVERY pass, pass 0 and the extra ones alike, so the wood thins
    // evenly rather than by undoing the second sweep -- the stands and the
    // clearings between them keep their shape, there is simply more room
    // inside a stand. The hives thin with it: one birch in a hundred is still
    // one birch in a hundred, and there are a quarter fewer birches to roll.
    //
    // Before that: DOUBLED, 0.42 -> 0.84. The opening value came from treating
    // the browser engine's coarser 4.4 m grid as "an open wood", and it read as
    // too open -- a birch stand is dense, it is the TRUNKS being pale and bare
    // that make it feel light rather than the gaps between them.
    //
    // The gate above it tops out at 0.97, so there was headroom at 0.84 and
    // there is a great deal now -- at 1.0 every cell the stand-density field
    // admits would take a tree and the clumping would flatten out into an even
    // field, which is the thing that gate exists to prevent.
    // -- HALVED, AND HALVED MEANS THE TREES, NOT THE KNOB ----------------
    //
    // (user 2026-09-18: "can you reduce the frequency of the birch forest
    // trees by 50%".)
    //
    // 0.3317 -> 0.1467, which is a 56% cut to the number and a 50.07% cut to
    // the wood. Those differ because this is a probability per CANDIDATE and
    // the spacing test below rejects fewer of the survivors as the stand
    // thins, so offering half as many sites never gives half as many trees --
    // the quarter-cut above measured 17% for exactly this reason.
    //
    // MEASURED with the engine's own ring count at (400, 0), --birch pinned:
    //
    //     0.3317   12054 trees     the wood as it was
    //     0.1400    5792 trees     -51.95%, overshot
    //     0.1467    6019 trees     -50.07%
    //
    // The two trials fit trees proportional to density^0.85 to four figures,
    // which is what picked 0.1467 rather than a third bisection step.
    //
    // 0.3317: 0.363 x 0.914, which is 1 / 1.0945 -- the factor by which
    // squaring standGate raised its own mean. The birch wood was not asked to
    // change, and it shares the gate, so its density is scaled back to hold
    // its tree count where it was.
    float birchDensity = 0.1467f;

    // TWICE AS MANY BIRCHES, AS AN EXTRA SWEEP RATHER THAN A BIGGER NUMBER.
    //
    // Asked for directly. Neither of the two obvious knobs can pay it:
    // birchDensity is a probability per cell and the gate above it tops out
    // at 0.97, so 0.84 has less than a fifth left in it; and the STRIDE is
    // shared with the pines by construction (see the note in the scatter),
    // so halving it would double the pine wood too and re-roll every tree in
    // the world on a lattice that no longer lines up with the old one.
    //
    // So the birch gets a second pass over the SAME lattice instead: one
    // more candidate per cell, jittered from a different hash, kept only
    // where the ground is birch. Three properties follow, and they are the
    // whole reason it is shaped this way:
    //
    //   * THE EXISTING WOOD DOES NOT MOVE. Pass 0 runs first and unchanged,
    //     so every pine and every birch that stood before still stands, in
    //     the same place. The new trees fill gaps.
    //   * THE PINES ARE UNTOUCHED. A later candidate that lands on pine
    //     ground is dropped rather than planted. A pinned pine wood comes
    //     back bit for bit: 6786 trees before and after.
    //   * SPACING IS STILL SHARED. Both passes push into the same `placed`
    //     list, so a new birch cannot grow through an old one -- which is
    //     exactly what a second LATTICE could not have promised, and is why
    //     the one-lattice rule survives this.
    //
    // MEASURED, on a pinned birch wood at three places, ring against ring:
    //
    //     400, 0        7055 -> 13967    1.98x
    //     2000, 1500    6884 -> 13583    1.97x
    //     400, -3000    7496 -> 14792    1.97x
    //
    // THAT TABLE IS FROM BEFORE THE TREES GREW, and the numbers in it no
    // longer come back. Re-run against the models it was measured on, 400, 0
    // still gives 13955 -- but those models have since been revoxelised
    // taller, a birch being 18.2 to 30.5 m now, and a wider trunk clashes more
    // often: the same ring on the same settings dropped to 12002. Growing the
    // wood upwards thinned it by 14% on its own, before birchDensity above
    // took its quarter.
    //
    // Re-measured where it stands now, at 400, 0: 4579 trees with these passes
    // off, 8977 with them on. 1.96x -- so the doubling this exists for is back,
    // and it is the THINNER wood that gave it back. Fewer candidates on the
    // same ground clash less often, which is the note above read from the
    // other end.
    //
    // AND ZERO IS THE OLD WOOD, EXACTLY, which is the check worth keeping:
    // birchExtra = 0 gives one pass and reproduced the pre-change build to
    // the tree -- 7055 trees, 1089 rocks, 74 hives at 400, 0. Anything that
    // disturbs pass 0 shows up there as a number that no longer matches.
    //
    // The hives ride along, 74 -> 157, because one birch in a hundred
    // carries one and there are twice as many birches to roll. The rocks
    // fall, 1089 -> 1000, and that is collectTrees working: a rock defers
    // to a trunk, and there are more trunks to defer to.
    // Extra candidates per cell, and FRACTIONAL because whole ones cannot
    // land on the number that was asked for. One whole extra pass offers
    // twice the birch and delivers 1.70x -- the spacing test takes the rest,
    // since twice the candidates on the same ground clash more often than
    // once. The part beyond the last whole pass is spent as a per-cell
    // probability, and that is what lets this be tuned to the figure measured
    // below rather than to whatever a whole number happens to land on.
    float birchExtra = 1.5f;
    // Mixed into the cell hash on the later passes, stepped by the pass so
    // that a third candidate is not a copy of the second. One rehash gives
    // the jitter, the species roll, the density gate, the model, the yaw,
    // the sink and the hive roll all their own streams -- one place to get
    // right instead of seven.
    static constexpr uint32_t kBirchPassSalt = 0x5B17u;

    // ONE BIRCH IN TWENTY CARRIES A BEEHIVE.
    //
    // The browser engine's BKHIVE, and its note records how it got there:
    // 0.10 -> 0.05 -> 0.02, then "make birch 1%". A hive is a landmark you come
    // across, not furniture -- at a tenth you meet one every few strides and it
    // stops being either.
    //
    // FIVE PER CENT (user 2026-09-14: "add behives to 5%% of the birch trees"),
    // which is v1's own middle value and a hive roughly every other stand
    // rather than every other wood. It matters more than it did: the bees are
    // here now, and a hive you never find is five bees you never meet.
    float birchHiveRate = 0.05f;
    // -- AND HOW MANY OAKS BEAR FRUIT --------------------------------------
    //
    // (user 2026-09-17: "add apples and oranges to some of the trees in the
    //  oak forest. import the v1 mechanics of this.")
    //
    // 0.15, WHICH IS THE BROWSER ENGINE'S OWN SETTLED VALUE and carries its
    // history with it: it shipped at 0.10, was measured in-game at 7 of 124
    // oaks, was doubled to 0.20, read as TOO MANY, and settled here -- about
    // one oak in nine. Importing the mechanic means importing the number it
    // arrived at, not re-deriving one.
    //
    // A SHARE OF THE TREES THAT CAN BEAR, not of every oak. The bush tier is
    // excluded below, and the browser engine keeps the exclusion visible in
    // the number rather than re-basing it onto the whole population.
    float oakFruitRate = 0.15f;
    uint32_t seed = 20260904u;
    // V4 SCAFFOLDING, AND OFF IN v2. This was the blank canvas a NanoVDB
    // world was going to be built into, and it defaulted ON there. v2 has no
    // VDB and no flag that turns it back on, so leaving it true means the
    // generator is never asked and the world renders as nothing but sky.
    bool emptyWorld = false;

    // ------------------------------------------------------------------
    // WHICH MESHER BUILDS A CHUNK.
    //
    // On: the brick path -- src/voxel, 64^3 bricks merged by bit arithmetic
    // and expanded back to triangles by expandChunk. Off: VoxelTerrain's
    // original per-voxel meshChunk.
    //
    // BOTH ARE KEPT AND THAT IS DELIBERATE. The two produce the same SURFACE
    // -- tests/voxel_parity_test.cpp checks every face of it against the
    // terrain function itself -- but not the same triangles, because the
    // brick path merges where meshChunk does not. So this flag is the A/B any
    // future "the world looks wrong" starts from, and it costs one branch per
    // chunk to keep.
    //
    // ------------------------------------------------------------------
    // ON. The brick path draws everything the old mesher drew, and water
    // besides.
    //
    // It was off while the bricks had no grass: meshChunk drew 51,962 blade
    // triangles a chunk and the bricks drew none, so turning it on would have
    // shipped a grassless world -- and, less obviously, made the triangle
    // saving look twice as good as it was (2.04x, when terrain-for-terrain it
    // is 1.35x). Blades are in the occupancy column now and mergePlane splits
    // on the strand code, so the face sets match exactly.
    //
    // MEASURED PER KIND at chunk (0,0), because comparing totals is only honest
    // once both paths draw the same things:
    //
    //     terrain   76,086 -> 56,210 tris   1.35x
    //     blades    51,962 -> 53,200 tris   0.98x   (a blade is 1 voxel wide;
    //                                                nothing to merge)
    //     water          0 ->    172 tris   meshChunk cannot draw it
    //     FACES    178,389 -> 178,389       identical, both directions
    //
    // Build time stays at parity (16.3 ms against 15.9) because 14.2 ms of both
    // is the heightfield noise, which is the same field either way.
    //
    // THE ONE PLACE THE TWO DISAGREE IS AROUND A DIG, and the old mesher is the
    // one that is wrong: 138 ground faces on chunk (-18,-16), mean 11.8 voxels
    // from the carve, each confirmed present by TerrainProbe and absent from
    // meshChunk. tests/voxel_ab_test.cpp documents it; voxel_parity_test is the
    // independent check that the bricks match the terrain function there.
    //
    // Set this false to get the old mesher back for an A/B -- it is kept for
    // exactly that, and costs one branch per chunk.
    bool useBricks = true;

    // -----------------------------------------------------------------------
    // THE WAVE PHASE THE NEXT JOB WILL BE MESHED AT, in seconds.
    //
    // Atomic and read once per job rather than per column: a worker copies the
    // terrain locally and stamps this into it before meshing, so two workers
    // can be a tick apart without racing and a chunk simply carries whatever
    // phase it was built at. The alternative -- one shared terrain mutated by
    // the main thread while workers read it -- is a data race on every float
    // in the generator.
    // -----------------------------------------------------------------------
    std::atomic<float> waveTime{0.0f};

    // So the scheduler can skip the whole cycle when the swell is off.
    bool waveVoxMaxIsZero() const { return terrain_.waveVoxMax <= 0; }

    // THE EDIT LAYER. Written on the main thread by a swing, read here by the
    // workers -- see EditStore, which publishes by copy so the two never race.
    EditStore edits;

  private:
    VoxelTerrain terrain_;
    std::vector<std::thread> workers_;
    int threads_ = 0;   // kept so restart() can raise the same crew
    std::deque<std::pair<int, int>> pending_;
    std::deque<ChunkBuild> done_;
    std::mutex inMx_, outMx_;
    std::condition_variable inCv_;
    bool stop_ = false;
    size_t busy_ = 0;

    // -----------------------------------------------------------------------
    // One chunk through the brick path, as the VoxMesh the rest of the engine
    // already knows how to build a BLAS from.
    //
    // THE BRICKS ARE DROPPED AGAIN ON PURPOSE. Keeping them is what makes an
    // edit cost 0.24 ms instead of 16, and it is worth having -- but not HERE.
    // These stores belong to mesh workers, and a dig happens on the main
    // thread against a chunk that may be resident in any of them or in none.
    // Holding them would grow a second copy of the ring per worker (22 bricks
    // a chunk, 625 chunks, four threads) to serve a lookup that cannot safely
    // reach it. The edit-side store is its own job; see src/voxel/store.h.
    //
    // The stack LRU is NOT dropped, and that is the part worth keeping: it is
    // the 14 ms of noise, and the next chunk this worker takes is usually
    // adjacent.
    VoxMesh meshChunkFromBricks(vox::BrickStore &store, int cx, int cz, bool *hasWater) {
        store.buildChunk(cx, cz);
        *hasWater = store.chunkHasWater(cx, cz);
        VoxMesh m = vox::expandChunk(store, cx, cz);
        store.evictChunk(cx, cz);
        return m;
    }

    void run() {
        // ONE PER WORKER, for the life of the thread -- the grids and the noise
        // memo inside it are pure working storage, and rebuilding them per
        // chunk was several hundred kilobytes of allocate-and-zero per job.
        // See ChunkScratch in voxelworld.h for why reuse is safe.
        ChunkScratch scratch;
        // ONE PER WORKER TOO, for the same reason and one more: the stack LRU
        // inside it is warm for the chunk NEXT DOOR, whose bricks share a
        // column footprint at the seam. A store per job would throw that away
        // on every chunk. It is not shared between workers because it is not
        // thread-safe by design -- the LRU, the noise memo and the mesh
        // scratch are all mutable, and a mutex around them would serialise
        // precisely the work the threads exist to spread.
        // A WORKER-LOCAL COPY OF THE TERRAIN, so the wave phase can be stamped
        // per job. It is a handful of floats; the alternative is the main
        // thread writing waveTime into a generator four workers are reading.
        VoxelTerrain terr = terrain_;
        vox::BrickStore bricks(&terr, &edits);

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
            // AN EMPTY WORLD, ON PURPOSE. The terrain generator and the
            // scatter both still exist and both still work -- they are simply
            // not asked. This is the blank canvas the VDB world gets built
            // into, and it is a switch rather than a deletion because the
            // generator is what will FILL those grids: heightVox, materialAt,
            // the crust and the bedrock are the source data, not the renderer.
            //
            // --world brings it back for comparison.
            if (!emptyWorld) {
                // The chunk's edits, if anybody has dug here. Null is the
                // ordinary case and costs one hash lookup per chunk.
                const std::shared_ptr<const ChunkEdits> ce = edits.get(b.cx, b.cz);
                if (useBricks) {
                    terr.waveTime = waveTime.load(std::memory_order_relaxed);
                    b.mesh = meshChunkFromBricks(bricks, b.cx, b.cz, &b.hasWater);
                } else {
                    b.mesh = terrain_.meshChunk(b.cx, b.cz, scratch, ce.get());
                }
                scatter(&b);
            }
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
    // PUBLIC, for the scatter harness. It is a pure function of the chunk
    // coordinates and the knobs above -- no threads, no GPU, no state of its
    // own -- which is exactly what lets a density be swept in a g++ loop
    // instead of by launching the engine a dozen times.
  public:
    void scatter(ChunkBuild *b) {
        const int I0 = b->cx * CHUNK_VOX, J0 = b->cz * CHUNK_VOX;
        // THE BAND'S WATERLINE, ONCE PER CHUNK. These gates keep trees, rocks
        // and flowers out of the shallows, and the line they measure against is
        // now per band -- the birch wood has none, so its gate must not reject
        // anything. Asked at the chunk's CENTRE column rather than per scatter
        // cell: a chunk is 25.6 m and the band seam is far wider than that, so
        // the only columns this can answer differently from a per-cell query
        // are within one chunk of a seam that has no water on one side of it
        // anyway. kNoWaterVox makes every gate below fall through.
        const int wl = terrain_.waterVoxAt(terrain_.wx(b->cx * CHUNK_VOX + CHUNK_VOX / 2));
        // The scatter grids are coarse -- 2.4 m for trees, 0.9 for flowers --
        // so a memo hits far less often here than in the mesher. It still hits:
        // the height field's slowest octave is eighty metres across, and these
        // rows walk a chunk twenty-five metres wide.
        TerrainMemo memo;

        struct Placed { float x, z, r; };
        std::vector<Placed> placed;

        // ---- trees ---------------------------------------------------------
        if (!pineFoot.empty()) {
            // Whichever wood this is, the SHAPE of the scatter is identical --
            // same jitter, same stand-density gate, same spacing rejection.
            // Only the grid pitch and how much of it fills changes.
            // THE GRID IS THE COARSER OF THE TWO, WORLD-WIDE. A scatter grid
            // has to be one lattice or trees would be generated twice over near
            // a seam -- once on each pitch -- and neither pass would know about
            // the other's spacing rejections. So the birch's 4.4 m is the grid
            // and each band fills its own share of it -- the birch 0.84, the
            // pine 0.3210 -- which is what lets one lattice serve both.
            const bool anyBirch = birchBase < int(pineFoot.size());
            const float tStride = anyBirch ? birchStride : treeStride;
            const int steps = int(CHUNK_M / tStride);
            // PASS 0 IS THE WOOD AS IT WAS. Every later pass is birch-only,
            // and runs after it so that nothing it adds can displace a tree
            // that was already there -- the spacing test rejects the newcomer,
            // never the incumbent. See birchPasses.
            const int passes = anyBirch ? 1 + int(ceilf(birchExtra)) : 1;
            for (int pass = 0; pass < passes; ++pass)
                for (int j = 0; j <= steps; ++j) {
                    for (int i = 0; i <= steps; ++i) {
                        const float bx = float(I0) * VOXEL_M + float(i) * tStride;
                        const float bz = float(J0) * VOXEL_M + float(j) * tStride;
                        const uint32_t cellBase = hashU32(uint32_t(int(bx * 16.0f)),
                                                          uint32_t(int(bz * 16.0f)) ^ 0x9E37u);
                        const uint32_t cell =
                            pass ? hashU32(cellBase, kBirchPassSalt + uint32_t(pass)) : cellBase;
                        // The fraction, spent on the LAST pass only: the whole
                        // passes under it offer a candidate in every cell, and
                        // this one offers it in that share of them. Tested here,
                        // before the jitter, because it is the cheapest of the
                        // three rejections a later pass can fail.
                        if (pass != 0) {
                            const float want = birchExtra - float(pass - 1);
                            if (want < 1.0f && hashUnit(seed + 0x71u, cell) >= want)
                                continue;
                        }

                        const float x = bx + (hashUnit(seed + 11u, cell) - 0.5f) * tStride * 1.8f;
                        const float z = bz + (hashUnit(seed + 12u, cell) - 0.5f) * tStride * 1.8f;

                        // The birch-only test, cheap half. birchMix is a pure
                        // function of x with no noise and no memo behind it,
                        // so a pine band leaves here rather than paying for
                        // the height and slope lookups below.
                        if (pass != 0 && terrain_.birchMix(x) <= 0.0f) continue;

                        const int ci = int(floorf(x / VOXEL_M));
                        const int cj = int(floorf(z / VOXEL_M));
                        if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX)
                            continue;  // it belongs to a neighbour

                        const int h = terrain_.heightVox(ci, cj, memo);
                        if (h <= wl + 8) continue;
                        // -- AND THE LAKES THE PHOTOGRAPH FOUND ------------
                        //
                        // (user 2026-09-18, looking down on a lake: "the water
                        // is missing ... looks like the terrain under the water
                        // is missing".)
                        //
                        // `wl` is the per-BAND procedural line, and on a DEM
                        // world waterVoxAt returns kNoWaterVox -- so `h <= wl +
                        // 8` falls through and there is NO WATER GATE AT ALL.
                        // Trees, rocks and flowers were being scattered across
                        // every mapped lake in the window, standing on the bed
                        // with five metres of water over them, which from above
                        // reads as a dark speckled pit where the lake should be.
                        //
                        // It was always wrong and it used to be nearly
                        // invisible: the imagery only had a lake where it
                        // happened to classify one. The DEM water pass in
                        // naip2cov.py then added 136,367 samples of lake to this
                        // window, and what had been a few boulders became the
                        // report.
                        //
                        // mappedWater is the SAME DOOR heightM carves the bed
                        // through and lakeLineAt puts the surface back through,
                        // so a site this rejects is exactly a site that is under
                        // water. It costs one bilinear read on dry land, which
                        // is where all but 1.6% of these calls land.
                        if (terrain_.mappedWater(x, z)) continue;

                        const int slope = maxi(absi(terrain_.heightVox(ci + 1, cj, memo) -
                                                    terrain_.heightVox(ci - 1, cj, memo)),
                                               absi(terrain_.heightVox(ci, cj + 1, memo) -
                                                    terrain_.heightVox(ci, cj - 1, memo)));
                        if (slope >= VoxelTerrain::kTreeSlope) continue;
                        if (terrain_.treeRejectedByAltitude(ci, cj, terrain_.heightVox(ci, cj, memo))) continue;
                        if (!terrain_.coverAllowsTree(terrain_.wx(ci), terrain_.wx(cj))) continue;

                        // WHICH WOOD IS THIS COLUMN IN. Asked at the tree's own
                        // jittered position rather than at the cell base, so a tree
                        // that jitters across the seam is the species of where it
                        // actually stands.
                        // -- ONE ROLL, THREE OUTCOMES --------------------
                        //
                        // A CATEGORICAL DRAW off the band weights, which is what
                        // the two-wood test already was: `bmix > r` reads as
                        // "birch if r falls in the birch's share of [0,1)". Three
                        // shares generalise that by laying them end to end.
                        //
                        // THE BIRCH KEEPS THE BOTTOM OF THE RANGE, and that is
                        // not arbitrary -- it is what makes this change invisible
                        // to every world that already existed. Order the
                        // cumulative pine-first and each cell draws a DIFFERENT
                        // species than it used to for the same seed, which would
                        // silently re-roll every tree in both existing woods and
                        // move every reference render in the repository. With
                        // the oak weighing nothing this is the old line, value
                        // for value.
                        //
                        // ONE hash, not three: three independent rolls would not
                        // be a partition and the species would stop matching the
                        // band's own proportions.
                        //
                        // RENORMALISED OVER WHAT IS LOADED, because --pine and
                        // --birch and --oak each load one set and the other two
                        // weights then have nowhere to go. Without this a forced
                        // run plants nothing through most of the world.
                        // -- NOTHING GROWS IN THE SAND -------------------
                        //
                        // The three wood weights sum to 1 MINUS the desert's
                        // (see VoxelTerrain::desertWeight), so they carry the
                        // thinning for every density in the engine -- but NOT
                        // for this one, and the reason is three lines down: if
                        // the three sum to nothing this scatter still picks a
                        // species ("plant whatever there IS rather than leaving
                        // a bald strip") and then reads a CONSTANT density for
                        // it. Correct for a world pinned to one wood, and in
                        // the desert it would plant a pine forest on the dunes.
                        //
                        // So this is the one place the desert is named. It is
                        // also the shape v1 uses at every one of its thirteen
                        // scatters -- `if (desertM(wx, wz) > 0.5) return null`
                        // -- and the halfway point is its rule too, so the
                        // treeline thins across the rim rather than stopping on
                        // a line.
                        if (terrain_.desertMix(x) >= 0.5f) continue;
                        float wPine = 0.0f, wBirch = 0.0f, wOak = 0.0f;
                        terrain_.woodMix(x, &wPine, &wBirch, &wOak);
                        const bool haveP = birchBase > 0;
                        const bool haveB = oakBase > birchBase;
                        const bool haveO = int(pineFoot.size()) > oakBase;
                        if (!haveP) wPine = 0.0f;
                        if (!haveB) wBirch = 0.0f;
                        if (!haveO) wOak = 0.0f;
                        const float wSum = wPine + wBirch + wOak;
                        int species;   // 0 pine, 1 birch, 2 oak
                        if (wSum <= 1e-6f) {
                            // This band wants a wood nothing loaded. Plant
                            // whatever there IS rather than leaving a bald strip.
                            species = haveP ? 0 : haveB ? 1 : 2;
                        } else {
                            const float roll = hashUnit(seed + 0x2C1Du, cell) * wSum;
                            species = (roll < wBirch)          ? 1
                                      : (roll < wBirch + wOak) ? 2
                                                               : 0;
                        }
                        const bool isBirch = species == 1;
                        const bool isOak = species == 2;
                        // And the exact half. Inside the ninety-metre seam
                        // the species roll can still come up pine, and a pine
                        // planted by the birch's own sweep is a pine the wood
                        // did not ask for.
                        if (pass != 0 && !isBirch) continue;
                        const float tDensity = isBirch  ? birchDensity
                                               : isOak  ? oakDensity
                                                        : treeDensity;

                        // -- AND HOW FULL THE STAND TABLE WANTS THIS LATTICE --
                        //
                        // stemFill is this elevation's own demand as a multiple
                        // of what the pipeline delivers -- see
                        // VoxelTerrain::stemFill. It used to be a separate
                        // rejection further up (treeRejectedByDensity), which
                        // could only ever THIN. Folded into the acceptance it
                        // fills as well, which is what true scale needs: the
                        // stand table asks for 1,750 stems a hectare at 2,900 m
                        // and the old rule had no way to ask for more than the
                        // 146 this scatter happened to deliver.
                        //
                        // A PRODUCT OVER 1.0 SATURATES ON ITS OWN, because no
                        // hash in [0,1) is ever greater than it. No second
                        // clamp to keep in step with the first.
                        const float fill = terrain_.stemFill(x, z, terrain_.heightVox(ci, cj, memo));
                        const float dens = terrain_.standDensity(x, z, memo.stand);
                        if (hashUnit(seed + 13u, cell) > standGate(dens) * tDensity * fill)
                            continue;

                        // The model comes from that species' own range.
                        const int lo = isBirch ? birchBase : isOak ? oakBase : 0;
                        const int hi = isBirch ? oakBase : isOak ? cherryBase : birchBase;
                        const int span = maxi(1, hi - lo);
                        int k = lo + (int(hashUnit(seed + 15u, cell) * float(span)) % span);
                        // ...AND IN THE CHERRY BAND IT IS THE SAME TREE IN
                        // BLOSSOM. See cherryBase: the roll above already chose
                        // WHICH oak, and this only changes which palette it is
                        // wearing, so the two woods are the same wood.
                        //
                        // -- ANY CHERRY AT ALL, NOT HALF OF ONE -------------
                        //
                        // (user 2026-09-21: "Im finding lone oak trees in
                        //  between the pine forest and cherry forest. remove
                        //  them from that area".)
                        //
                        // THIS TESTED `>= 0.5f` AND THAT IS WHERE THEY CAME
                        // FROM. `wOak` above is the THREE-WAY view, and that
                        // view folds the cherry into the oak -- woodWeights
                        // ends with `*oak += ch`. So through the pine|cherry
                        // seam the oak weight is entirely the CHERRY's, the
                        // species roll comes up "oak" in proportion to it,
                        // and then a half-threshold refused to dress it: a
                        // literal oak, standing alone in a seam that has no
                        // oak band anywhere near it.
                        //
                        // THE BANDS ARE WHY `> 0` IS EXACT RATHER THAN A
                        // TIGHTENING. The tiling is birch|oak|pine|cherry|
                        // desert and only ADJACENT bands mix, so the oak and
                        // the cherry -- one and three, with the pine between
                        // them -- can never both be non-zero at one x. A
                        // non-zero cherry weight therefore means the whole of
                        // the oak weight here IS the cherry, and every tree
                        // it plants is one. In the real oak band cherryMix is
                        // exactly zero and nothing changes.
                        if (isOak && cherryBase < int(pineFoot.size()) &&
                            terrain_.cherryMix(x) > 0.0f) {
                            k += cherryBase - oakBase;
                            // ...AND HALF OF THEM ARE THE PALE ONE. Its own
                            // hash, not a reuse of the species roll: that one
                            // picks WHICH of the three models and is already
                            // spoken for, so sharing it would tie shade to
                            // shape -- every cherry_2 pale, every cherry_1
                            // dark, in a wood of three trees. A separate salt
                            // over the same cell is an independent coin, so
                            // the two halves interleave.
                            // THE FIELD THE GROUND READS TOO -- see
                            // VoxelTerrain::cherryPale. This was a coin tossed
                            // on the tree's own cell, which nothing outside
                            // this function could reproduce; the petals under
                            // the tree have to agree with it, so the answer
                            // moved somewhere both can ask.
                            if (cherryLightBase > cherryBase &&
                                cherryLightBase < int(pineFoot.size()) &&
                                VoxelTerrain::cherryPale(x, z))
                                k += cherryLightBase - cherryBase;
                        }
                        const int yaw = int(hashUnit(seed + 16u, cell) * 4.0f) & 3;
                        const Footprint &f = pineFoot[size_t(k)];
                        const float footX = float((yaw & 1) ? f.sz : f.sx) * VOXEL_M;
                        const float footZ = float((yaw & 1) ? f.sx : f.sz) * VOXEL_M;
                        // -- A CLOSED STAND HAS INTERLOCKING CANOPIES -----
                        //
                        // The spacing was a share of the model's own FOOTPRINT,
                        // which is the canopy -- and a canopy is the wrong thing
                        // to hold apart in a thick wood. Real lodgepole at the
                        // 1,450 stems a hectare the stand table asks for at
                        // 2,900 m is trunks about 2.6 m apart with the crowns
                        // growing through one another; spacing by the crown caps
                        // the wood at a couple of hundred, and it did -- with the
                        // lattice offering 1,500 sites a hectare this test was
                        // throwing away nine of every ten.
                        //
                        // SO IT RELAXES WITH THE DEMAND. `fill` is precisely how
                        // closed the stand table says this ground should be, so
                        // it is the right thing to divide by: an open savanna at
                        // fill well under one keeps the full crown spacing and
                        // reads as parkland, and a closed stand at fill three or
                        // four falls to the floor and packs.
                        //
                        // THE FLOOR IS THE TRUNK, and it is not negotiable --
                        // two boles cannot stand in the same ground however
                        // thick the wood is.
                        const float canopy = 0.30f * maxf(footX, footZ);
                        const float keep = maxf(kTrunkKeepM, canopy / maxf(1.0f, fill));

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
                                int oi = 0, oj = 0;
                                baseOffsetVox(f, yaw, &oi, &oj);
                                const int drop = groundDrop(ci, cj, h, bX, bZ, memo, oi, oj);
                                extraSink = maxi(0, drop - base);
                            }
                        }

                        placed.push_back({x, z, keep});
                        b->decor.push_back({0, k, ci, cj, h, yaw, cell, 0, extraSink});
                        hangPinecones(b, k, ci, cj, h, yaw, cell, extraSink);
                        hangHive(b, k, ci, cj, h, yaw, cell, extraSink);
                        hangFruit(b, k, ci, cj, h, yaw, cell, extraSink);
                    }
                }
        }

        // ---- rocks and flowers --------------------------------------------
        // Both are scattered on a finer grid than the trees and take whatever
        // ground is left; a rock may sit on rock or soil, a flower only on
        // grass.
        //
        // NOTHING SMALL STANDS INSIDE A TRUNK ANY MORE. The note that used to
        // sit here said flowers and mushrooms ignore the trees entirely "and
        // should: growing under a canopy is what they do" -- which is right
        // about the CANOPY and was being used to excuse the trunk. Those are
        // different objects and collectTrees only ever measured the second: a
        // box a third of a metre across at the foot of the tree, not the
        // twelve-metre crown over it. Growing under the branches is still
        // exactly what they do; there is simply no longer a mushroom inside
        // the wood of the tree.
        //
        // THREE THINGS WERE WRONG AND ALL THREE HAD TO GO, which is why the
        // first two fixes each left a residue:
        //
        //   1. The small passes never consulted the trees at all -- this
        //      block.
        //   2. collectTrees centred its disc on the model's BOUNDING BOX, and
        //      a birch model is centred on its crown: birch 7 and 10 carry
        //      their trunks 4.8 and 4.95 m off that centre, so the guard was
        //      five metres from the trunk.
        //   3. The disc INSCRIBED the trunk's square footprint, leaving the
        //      four corners open, and the test was asked at the jittered
        //      position rather than at the column the model is stamped on.
        //
        // MEASURED PER VOXEL, not per bounding box: every placed model's own
        // occupancy mask laid down where the scatter put it, against the
        // columns the trees' base slabs occupy, over the 625-chunk ring at
        // 400, 0. "Sharing a column with a trunk" is the strictest reading of
        // the complaint there is.
        //
        //                    birch wood              pine wood
        //     mushrooms   156/1414 -> 0/1194     156/1373 -> 0/1142
        //     flowers     276/22396 -> 0/21753   162/10681 -> 0/10276
        //     rocks        36/802  -> 4/787       12/779  -> 1/764
        //
        // Zero, and it costs 15% of the mushrooms, 3% of the flowers and 2% of
        // the stones -- every one of them a model that was standing in a tree.
        //
        // THE FOUR REMAINING ROCKS ARE THE SHAPE OF A ROCK, not a bug in the
        // spacing: a boulder asks with a CIRCLE of its mean half extent, which
        // is the honest shape for something lumpy and round, and a long
        // irregular stone can still put a corner into bark. Worst case 25
        // columns, a quarter of a square metre against a twelve-metre
        // boulder, and it reads as a stone leaning on a tree.
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
        // ONE LIST FOR THE SMALL PASSES, because a mushroom has to clear both
        // and scatterSmall takes a single set to avoid. Concatenated rather
        // than passed as two, so the rejection stays the one loop it was: a
        // chunk's neighbourhood holds a couple of dozen stones and a hundred or
        // so trunks, and the scan runs only for a candidate every other test
        // has already accepted.
        std::vector<Disc> solid = rocks;
        solid.insert(solid.end(), trees.begin(), trees.end());
        // -- FANNED OUT BY HALF AGAIN (user 2026-09-07) --------------------
        //
        // 0.9 -> 1.35 m of grid. The stride is BOTH the spacing and the jitter
        // -- a candidate is placed at its cell and thrown up to half a stride
        // either way -- so widening it spreads the bed and loosens the lattice
        // in one move, which is what "fan them out" asks for. A jitter alone
        // would have made them noisier at the same spacing.
        //
        // It thins them too, and deliberately: area goes as the square, so a
        // flower bed is now a little over half as crowded. Raising the density
        // to hold the count would have put the spacing straight back.
        // v1's FLWCELL, 8 voxels -- see flowerStrideM. It used to be 1.35 m,
        // which is one candidate per 182 columns against v1's 64.
        scatterSmall(b, 2, flowerFoot, flowerDensity, flowerStrideM, true, &solid);
        scatterSmall(b, 3, mushroomFoot, mushroomDensity, 1.3f, true, &solid);
        // -- AND THE DESERT'S OWN TWO -----------------------------------
        //
        // grassOnly FALSE, which is the one argument that matters here: that
        // flag asks for a forest floor (soil, litter, the broadleaf green) and
        // the whole point of these is that they stand on SAND. Everything else
        // is the flower scatter's -- a lattice, a stride, and the solids
        // already placed in this chunk to keep out of.
        //
        // The cactus stride is wider than the shrub's because a saguaro is a
        // metre across and a scrub bush is a third of that; two kinds on one
        // stride would put every cactus in a bush.
        scatterSmall(b, 7, cactusFoot, cactusDensity, 3.2f, false, &solid);
        scatterSmall(b, 8, shrubFoot, shrubDensity, 1.8f, false, &solid);
    }

  private:
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
        // 0..1 how strongly this ground is inside a patch. ALWAYS 1 while
        // colonyCoverage is 1, which is what makes the scatter a meadow rather
        // than a set of beds -- see the v1 note over colonyCoverage.
        float w = 0.0f;
        int species = 0;   // the one flower model the whole patch is made of
    };

    // Eighteen metres between sites, and the site sits in the middle half of
    // its cell. Both numbers are about SEPARATION: two neighbouring colonies
    // that overlap heavily are two species mixed again, just in bigger lumps.

    Colony colonyAt(float x, float z, int speciesCount, FbmMemo &wobMemo) const {
        Colony best;
        if (speciesCount <= 0) return best;

        // ---- THE PATCH PICKS A COLOUR, IT DOES NOT PICK A PLACE -----------
        //
        // The whole ground is in a patch, which is v1's structure: what reads
        // as "grouped" is a drift of one colour into another, not a clump of
        // plants in bare ground.
        //
        // NEAREST SITE, NOT A FLOOR (user 2026-09-13: "dont mix different types
        // of flowers together. make sure to keep them seperate in their own
        // groups"). v1 uses `Math.floor(cx / FLWPATCH)`, which tiles the world
        // into axis-aligned SQUARES of one species. v1 gets away with it -- its
        // own note says "nothing draws the boundary, ~11 scattered plants do"
        // -- but a square grid puts four species round every lattice corner and
        // a straight seam between each pair, and at v2's larger patch that is
        // exactly what "mixed together" looks like: two colours a metre apart
        // along a ruled line, four of them meeting at a point.
        //
        // A jittered Voronoi has neither property. Every point belongs to
        // exactly ONE site, so a region is one species by construction; the
        // regions are irregular convex blobs rather than tiles; and two of them
        // meet along one wandering line instead of a cross.
        //
        // A 3x3 OF SITES IS ENOUGH, and it has to be: a site sits in the middle
        // half of its own cell, so the furthest a point can be from its own
        // site is under one cell and no site outside the ring can be nearer.
        //
        // The lobed-disc machinery below still runs when colonyCoverage is
        // turned down from 1; at 1 it cannot reject anything, so this is the
        // path every flower in the world takes.
        {
            const int gx = int(floorf(x / colonyCellM));
            const int gz = int(floorf(z / colonyCellM));
            float bestD = 1e30f;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const uint32_t pc =
                        hashU32(uint32_t(gx + di) ^ 0x9E3779B9u, uint32_t(gz + dj));
                    const float sx =
                        (float(gx + di) + 0.25f + 0.50f * hashUnit(seed + 62u, pc)) * colonyCellM;
                    const float sz =
                        (float(gz + dj) + 0.25f + 0.50f * hashUnit(seed + 63u, pc)) * colonyCellM;
                    const float dx = x - sx, dz = z - sz;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 >= bestD) continue;
                    bestD = d2;
                    // BARREN OR NOT, decided by the SITE and not by the point,
                    // so a dead region is dead all the way to its own
                    // boundaries rather than fading out. See colonyCoverage.
                    best.w = (hashUnit(seed + 61u, pc) < colonyCoverage) ? 1.0f : 0.0f;
                    best.species =
                        int(hashUnit(seed + 65u, pc) * float(speciesCount)) % speciesCount;
                }
            return best;
        }

        const int gi = int(floorf(x / colonyCellM));
        const int gj = int(floorf(z / colonyCellM));

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
                if (hashUnit(seed + 61u, c) > colonyCoverage) continue;

                const float sx = (float(gi + di) + 0.25f + 0.50f * hashUnit(seed + 62u, c)) *
                                 colonyCellM;
                const float sz = (float(gj + dj) + 0.25f + 0.50f * hashUnit(seed + 63u, c)) *
                                 colonyCellM;
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
        // THE SAME GRID THE TREE PASS USED, biome and all. If these two ever
        // disagree the rocks avoid trees that are not there and stand in ones
        // that are -- which is exactly the bug this function exists to fix,
        // reintroduced silently in the other wood.
        const bool anyBirch = birchBase < int(pineFoot.size());
        const float tStride = anyBirch ? birchStride : treeStride;
        if (pineFoot.empty()) return;
        // The band's waterline, as in scatter() -- see the note there.
        const int wl = terrain_.waterVoxAt(terrain_.wx(b->cx * CHUNK_VOX + CHUNK_VOX / 2));
        const int steps = int(CHUNK_M / tStride);
        const int passes = anyBirch ? 1 + int(ceilf(birchExtra)) : 1;
        for (int nz = -1; nz <= 1; ++nz)
            for (int nx = -1; nx <= 1; ++nx) {
                const int I0 = (b->cx + nx) * CHUNK_VOX, J0 = (b->cz + nz) * CHUNK_VOX;
                // THE SECOND BIRCH SWEEP TOO. If this walk and the placing
                // one ever disagree about how many candidates a cell offers,
                // the rocks avoid birches that are not there and grow through
                // the ones that are.
                for (int pass = 0; pass < passes; ++pass)
                    for (int j = 0; j <= steps; ++j)
                        for (int i = 0; i <= steps; ++i) {
                            const float bx = float(I0) * VOXEL_M + float(i) * tStride;
                            const float bz = float(J0) * VOXEL_M + float(j) * tStride;
                            const uint32_t cellBase = hashU32(uint32_t(int(bx * 16.0f)),
                                                              uint32_t(int(bz * 16.0f)) ^ 0x9E37u);
                            const uint32_t cell =
                                pass ? hashU32(cellBase, kBirchPassSalt + uint32_t(pass)) : cellBase;
                            if (pass != 0) {
                                const float want = birchExtra - float(pass - 1);
                                if (want < 1.0f && hashUnit(seed + 0x71u, cell) >= want)
                                    continue;
                            }
                            const float x =
                                bx + (hashUnit(seed + 11u, cell) - 0.5f) * tStride * 1.8f;
                            const float z =
                                bz + (hashUnit(seed + 12u, cell) - 0.5f) * tStride * 1.8f;
                            if (pass != 0 && terrain_.birchMix(x) <= 0.0f) continue;
                            const int ci = int(floorf(x / VOXEL_M));
                            const int cj = int(floorf(z / VOXEL_M));
                            // A tree belongs to the chunk holding its own column --
                            // the same test the placing pass makes, so a tree is
                            // counted once and by the chunk that will actually
                            // place it.
                            if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX)
                                continue;
                            const int h = terrain_.heightVox(ci, cj, memo);
                            // The band line, then the mapped lakes -- see the
                            // note in the first sweep.
                            if (h <= wl + 8) continue;
                            if (terrain_.mappedWater(x, z)) continue;
                            const int slope = maxi(absi(terrain_.heightVox(ci + 1, cj, memo) -
                                                        terrain_.heightVox(ci - 1, cj, memo)),
                                                   absi(terrain_.heightVox(ci, cj + 1, memo) -
                                                        terrain_.heightVox(ci, cj - 1, memo)));
                            if (slope >= VoxelTerrain::kTreeSlope) continue;
                            if (terrain_.treeRejectedByAltitude(ci, cj, terrain_.heightVox(ci, cj, memo))) continue;
                            if (!terrain_.coverAllowsTree(terrain_.wx(ci), terrain_.wx(cj))) continue;
                            // THE SAME CATEGORICAL DRAW scatter() MAKES, line
                            // for line. The note at the top of this function is
                            // the reason it is copied rather than approximated:
                            // if these two disagree the rocks avoid trees that
                            // are not there and stand in ones that are, and the
                            // oak is the widest thing in the world to stand a
                            // boulder inside of.
                            // THE SAME GATE scatter's twin makes -- see the
                            // note there. collectTrees mirrors that function
                            // deliberately, to keep rocks out of trunks, and a
                            // trunk it believes in that the scatter never
                            // planted is exactly the disagreement it exists to
                            // prevent.
                            if (terrain_.desertMix(x) >= 0.5f) continue;
                            float wPine = 0.0f, wBirch = 0.0f, wOak = 0.0f;
                            terrain_.woodMix(x, &wPine, &wBirch, &wOak);
                            const bool haveP = birchBase > 0;
                            const bool haveB = oakBase > birchBase;
                            const bool haveO = int(pineFoot.size()) > oakBase;
                            if (!haveP) wPine = 0.0f;
                            if (!haveB) wBirch = 0.0f;
                            if (!haveO) wOak = 0.0f;
                            const float wSum = wPine + wBirch + wOak;
                            int species;   // 0 pine, 1 birch, 2 oak
                            if (wSum <= 1e-6f) {
                                species = haveP ? 0 : haveB ? 1 : 2;
                            } else {
                                const float roll = hashUnit(seed + 0x2C1Du, cell) * wSum;
                                species = (roll < wBirch)          ? 1
                                          : (roll < wBirch + wOak) ? 2
                                                                   : 0;
                            }
                            const bool isBirch = species == 1;
                            const bool isOak = species == 2;
                            if (pass != 0 && !isBirch) continue;
                            const float tDensity = isBirch  ? birchDensity
                                                   : isOak  ? oakDensity
                                                            : treeDensity;
                            // -- AND HOW FULL THE STAND TABLE WANTS THIS LATTICE --
                            //
                            // stemFill is this elevation's own demand as a multiple
                            // of what the pipeline delivers -- see
                            // VoxelTerrain::stemFill. It used to be a separate
                            // rejection further up (treeRejectedByDensity), which
                            // could only ever THIN. Folded into the acceptance it
                            // fills as well, which is what true scale needs: the
                            // stand table asks for 1,750 stems a hectare at 2,900 m
                            // and the old rule had no way to ask for more than the
                            // 146 this scatter happened to deliver.
                            //
                            // A PRODUCT OVER 1.0 SATURATES ON ITS OWN, because no
                            // hash in [0,1) is ever greater than it. No second
                            // clamp to keep in step with the first.
                            const float fill = terrain_.stemFill(x, z, terrain_.heightVox(ci, cj, memo));
                            const float dens = terrain_.standDensity(x, z, memo.stand);
                            if (hashUnit(seed + 13u, cell) > standGate(dens) * tDensity * fill)
                                continue;
                            const int lo = isBirch ? birchBase : isOak ? oakBase : 0;
                            const int hi = isBirch ? oakBase : isOak ? cherryBase : birchBase;
                            const int span = maxi(1, hi - lo);
                            int k = lo + (int(hashUnit(seed + 15u, cell) * float(span)) % span);
                            // THE SAME SHIFT collectTrees' twin makes. This
                            // function mirrors scatter deliberately -- its own
                            // note says letting the two disagree is the bug it
                            // exists to fix -- and a cherry that is a different
                            // MODEL from the oak the other pass placed would be
                            // exactly that disagreement, because the footprint
                            // is what keeps rocks out of trunks.
                            // `> 0`, NOT `>= 0.5` -- and it has to match the
                            // scatter's copy exactly, for the reason this
                            // whole function exists. The long note there says
                            // why any cherry weight at all claims the tree.
                            if (isOak && cherryBase < int(pineFoot.size()) &&
                                terrain_.cherryMix(x) > 0.0f) {
                                k += cherryBase - oakBase;
                                // THE SAME COIN scatter's twin flips -- see the
                                // note there. These two functions are mirrored
                                // on purpose and a model they disagree about is
                                // exactly the fault that mirroring prevents:
                                // the footprint is what keeps rocks out of
                                // trunks, so a light cherry here and a dark one
                                // there is a boulder inside a tree.
                                // THE SAME FIELD scatter's twin reads -- see
                                // VoxelTerrain::cherryPale.
                                if (cherryLightBase > cherryBase &&
                                    cherryLightBase < int(pineFoot.size()) &&
                                    VoxelTerrain::cherryPale(x, z))
                                    k += cherryLightBase - cherryBase;
                            }
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
                            // ON THE TRUNK, NOT ON THE BOUNDING BOX. This used
                            // to sit at the tree's own (x, z), which is where
                            // the MODEL is centred -- and a birch model is
                            // centred on its crown. Birch 7 and 10 carry their
                            // trunks 4.8 and 4.95 m off that centre, so a
                            // half-metre disc was landing five metres from the
                            // thing it was meant to be guarding: the trunk was
                            // wide open and an empty patch of ground beside it
                            // was fenced off instead. That is a mushroom
                            // growing out of a birch, which is what was
                            // reported, and it was a rock growing out of one
                            // just as often.
                            //
                            // The column, not the jittered position, because
                            // that is where makeInstance actually stamps the
                            // model -- and then the model-space offset turned
                            // by the placement's own quarter turn, which is the
                            // 2x2 of the kRot matrix that function uses.
                            float ox = f.cx, oz = f.cz;
                            switch (yaw & 3) {
                                case 1: { const float t = ox; ox = oz; oz = -t; break; }
                                case 2: { ox = -ox; oz = -oz; break; }
                                case 3: { const float t = ox; ox = -oz; oz = t; break; }
                                default: break;
                            }
                            // AS A BOX, not as a circle: see Disc. The base
                            // footprint is what the trunk occupies, and a
                            // circle drawn inside it leaves the corners open.
                            out->push_back({float(ci) * VOXEL_M + ox, float(cj) * VOXEL_M + oz,
                                            0.0f, float(bxv) * VOXEL_M * 0.5f,
                                            float(bzv) * VOXEL_M * 0.5f});
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
    // -- ONE STONE PER CELL, AND WHAT IS AT A CELL IS A PURE FUNCTION OF
    //    WHERE THE CELL IS ------------------------------------------------
    //
    // Everything the scatter decides about a site comes out of the position
    // hash and nothing else. That is what lets the variety rule below look at
    // a NEIGHBOUR without having placed it: the neighbour is re-derived, not
    // remembered, so the answer is the same from every chunk that can see the
    // pair. See the boundary-agreement note over collectRocks -- this inherits
    // it, and it is the reason this takes a world position rather than a cell
    // index: the two callers count cells from different origins.
    struct RockAt {
        bool any = false;
        float x = 0.0f, z = 0.0f;
        int model = 0;      // what this cell rolls, BEFORE any variety shift
        uint32_t prio = 0;  // the position hash -- the tie-break
    };

    // ...AND THE OAK'S OWN WEIGHT, WHICH IS NOT OPTIONAL HERE.
    //
    // scatterSmall thins the stone across the oak and the desert rims --
    // `w *= 1 - 0.5 * max(oakMix, desertMix)` -- and collectRocks has always
    // used the plain density. That was HARMLESS while the two only had to
    // agree about the model at ONE cell: reserving a disc at a site with no
    // rock in it is merely conservative.
    //
    // THE VARIETY RULE MADE IT LOAD-BEARING. Each site now asks its NEIGHBOURS
    // what they roll, so a neighbour that one caller believes in and the other
    // does not is a neighbour that shifts one caller's model and not the
    // other's -- and then the disc reserved is a different stone's box from
    // the stone placed. Measured as `--clip-test --oak` failing with 47
    // creature-frames inside a rock, in the oak and nowhere else, which is
    // this term exactly.
    RockAt rockAtBase(float bx, float bz, int ordinary) const {
        RockAt r;
        if (ordinary <= 0) return r;
        const uint32_t c = hashU32(uint32_t(int(bx * 16.0f)) ^ kRockSalt,
                                   uint32_t(int(bz * 16.0f)));
        const float w = 1.0f - 0.5f * clampf(maxf(terrain_.oakMix(bx), terrain_.desertMix(bx)),
                                             0.0f, 1.0f);
        if (hashUnit(seed + 41u, c) >= rockDensity * w) return r;
        r.x = bx + (hashUnit(seed + 42u, c) - 0.5f) * kRockStride;
        r.z = bz + (hashUnit(seed + 43u, c) - 0.5f) * kRockStride;
        r.model = int(hashUnit(seed + 44u, c) * float(ordinary)) % ordinary;
        r.prio = c;
        r.any = true;
        return r;
    }

    // -----------------------------------------------------------------------
    // WHICH STONE THIS SITE GETS, ONCE ITS NEIGHBOURS HAVE HAD THEIR SAY.
    //
    // (user 2026-09-21: "prevent spawning THE SAME type of rock next to each
    //  other".)
    //
    // IT RE-ROLLS THE MODEL, IT DOES NOT DROP THE ROCK. Refusing the site
    // would thin the scatter to fix a repetition, which is the wrong trade:
    // the stone is wanted, it is the SHAPE that must differ. The site keeps
    // its place and walks forward through the ordinary set until it finds a
    // model no nearby twin has already claimed.
    //
    // ONE FUNCTION, TWO CALLERS, AND THAT IS NOT TIDINESS. collectRocks
    // reserves the disc that keeps other decor off the stone and scatterSmall
    // places the stone; the note over the roll in collectRocks records what it
    // cost the last time those two drew different models -- the reserved box
    // was the wrong size. A shift applied in one of them only would be that
    // bug again, so there is one place to apply it.
    //
    // AGAINST THE NEIGHBOUR'S BASE ROLL, not its final one. The final roll is
    // what this function computes, so asking for it would be circular; the
    // base is what the neighbour would have been and is enough to break up a
    // pair. A rare triple can still land two alike, which is a repeat further
    // apart rather than a repeat touching.
    //
    // STRICT AND ANTISYMMETRIC, so of any crowded pair exactly one has to
    // move, and both sides of a chunk seam agree on which.
    // -----------------------------------------------------------------------
    int rockVariety(float bx, float bz, float x, float z, uint32_t c, int base,
                    int ordinary) const {
        // V2_ROCK_VARIETY=0 turns the shift off, so "did the variety rule break
        // this" is one run rather than a rebuild off an old commit. Both
        // callers read the same flag, so the pair cannot disagree because of
        // it -- which is the one way a debug switch here could cause the very
        // bug it is meant to rule out.
        static const bool on = [] {
            const char *e = std::getenv("V2_ROCK_VARIETY");
            return !(e && e[0] == '0');
        }();
        if (!on || ordinary <= 1) return base;
        const int apart = int(ceilf(kRockSameApartM / kRockStride)) + 1;
        for (int t = 0; t < ordinary; ++t) {
            const int want = (base + t) % ordinary;
            bool taken = false;
            for (int dj = -apart; dj <= apart && !taken; ++dj)
                for (int di = -apart; di <= apart; ++di) {
                    if (!di && !dj) continue;
                    const RockAt n = rockAtBase(bx + float(di) * kRockStride,
                                                bz + float(dj) * kRockStride, ordinary);
                    if (!n.any || n.model != want) continue;
                    const float nx = n.x - x, nz = n.z - z;
                    if (nx * nx + nz * nz >= kRockSameApartM * kRockSameApartM) continue;
                    if (n.prio > c || (n.prio == c && (n.x < x || (n.x == x && n.z < z)))) {
                        taken = true;
                        break;
                    }
                }
            if (!taken) return want;
        }
        return base;
    }

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
                // THE ORDINARY SET, exactly as the scatter draws it -- see
                // rockOrdinary(). Rolling over the whole list here picked a
                // different stone from the one that will be PLACED, so the
                // disc reserved at this site was the wrong size.
                const int ord = rockOrdinary();
                const int base = int(hashUnit(seed + 44u, c) * float(ord)) % ord;
                // THE SAME SHIFT THE PLACING SITE APPLIES -- see rockVariety.
                const int k = rockVariety(bx, bz, x, z, c, base, ord);
                const Footprint &f = rockFoot[size_t(k)];
                // -- THE WHOLE STONE, NOT A CIRCLE THROUGH ITS MIDDLE ------
                //
                // (user 2026-09-19: "I saw a cactus clip into a rock. it was
                //  spawned that way".)
                //
                // This pushed a disc of the MEAN half-extent and left hx/hz
                // zero, so Disc::reaches tested a circle. A boulder is not a
                // circle: one three metres long and one wide has a mean radius
                // of one, and everything from the middle of that stone out to
                // its ends was ground the scatter believed was free. A cactus
                // dropped there is inside the rock on the frame it is born.
                //
                // A SQUARE OF THE LONGER SIDE, which is deliberately more than
                // the stone needs. The model is YAWED at the placing site (see
                // the `yaw & 1` swap in scatterSmall) and this function does
                // not draw that roll, so an exact hx/hz pair would be exact
                // for one orientation in two. The square encloses the stone
                // whichever way it lands, and the cost is decor kept a little
                // further off a long rock's short side.
                const float hm = 0.5f * float(maxi(f.sx, f.sz)) * VOXEL_M;
                out->push_back({x, z, 0.0f, hm, hm});
            }
    }

    // -----------------------------------------------------------------------
    // HOW FAR THE GROUND FALLS UNDER A MODEL'S BASE.
    //
    // WHERE THE BASE IS, NOT WHERE THE BOX IS, and that distinction is the
    // whole bug this carries the fix for. A model is stood on the terrain
    // height at its placement column, which is the middle of its BOUNDING BOX
    // -- and for a birch the bounding box is centred on the CROWN. Birch 7 and
    // birch 10 have their trunks 5.19 m and 5.40 m from the middle of their own
    // box, so the ground was being measured five metres from the tree and the
    // trunk was left standing over whatever the ground did where it actually
    // is. Measured: 6 trees in 958 hanging in the air, up to 1.10 m of daylight
    // under them, and every one of them a birch 7 or a birch 10.
    //
    // `oi`/`oj` is that offset, in world voxels, already turned by the
    // placement's quarter turn.
    //
    // EVERY COLUMN, not a sample of them. A base footprint is a trunk -- a
    // couple of dozen columns -- and a five-by-five sample of it can step over
    // the one that is low. Sampling coarsely to save work on something this
    // small was buying nothing and could only ever be wrong.
    // -----------------------------------------------------------------------
    int groundDrop(int ci, int cj, int h, int bx, int bz, TerrainMemo &memo, int oi = 0,
                   int oj = 0) const {
        if (bx <= 0 || bz <= 0) return 0;
        const int cx = ci + oi, cz = cj + oj;
        const int hx = bx / 2, hz = bz / 2;
        // A cap, for a footprint big enough that walking it column by column
        // would cost something -- a boulder, not a trunk. 64 a side is 4,096
        // lookups, and the stride only ever coarsens something already large.
        const int stepX = maxi(1, bx / 64), stepZ = maxi(1, bz / 64);
        int lowest = h;
        for (int dz = 0; dz <= bz; dz += stepZ)
            for (int dx = 0; dx <= bx; dx += stepX) {
                const int g = terrain_.heightVox(cx - hx + dx, cz - hz + dz, memo);
                if (g < lowest) lowest = g;
            }
        return maxi(0, h - lowest);
    }

    // The offset from a model's box centre to its BASE centre, in world voxels,
    // turned by the placement's quarter turn. Footprint::cx/cz is that offset
    // in the model's own frame, in metres -- see ModelCollider::baseCX.
    static void baseOffsetVox(const Footprint &f, int yaw, int *oi, int *oj) {
        const int dx = int(lroundf(f.cx / VOXEL_M));
        const int dz = int(lroundf(f.cz / VOXEL_M));
        switch (yaw & 3) {
            case 1: *oi = dz;  *oj = -dx; break;
            case 2: *oi = -dx; *oj = -dz; break;
            case 3: *oi = -dz; *oj = dx;  break;
            default: *oi = dx; *oj = dz;  break;
        }
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
        // A BIRCH DROPS NO CONES, and now that both species stand in one world
        // that is a question about THIS TREE rather than about the world. The
        // first version asked the world and hung fifty-five thousand pine cones
        // through the birch wood.
        if (pineIndex >= birchBase && birchBase < int(pineFoot.size())) return;
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

    // -----------------------------------------------------------------------
    // A BEEHIVE IN ONE BIRCH IN A HUNDRED.
    //
    // The same geometry as a pinecone and for the same reason: both hang in a
    // crown, both are placed from a perch collectPerches already found, and
    // both must ride the tree's quarter turn and its extra sink or they drift
    // out of it. So this is hangPinecones with three differences, and they are
    // all "a hive is not a cone":
    //
    //   * ONE, not fourteen. A hive is a landmark; the rate is per TREE rather
    //     than a count per tree.
    //   * ITS OWN HASH STREAM. Sharing the cone's would tie "does this tree get
    //     a hive" to "where does its first cone go", so the two would move
    //     together whenever either was retuned.
    //   * THE HIGHEST PERCH OF A FEW, not a uniform draw. A hive low in the
    //     trunk reads as a growth on the bark; hung out at height it reads as a
    //     hive. Four candidates and the highest wins -- cheap, and enough to
    //     push it into the crown without needing the browser engine's full
    //     clear-box test.
    //
    // The pine wood plants none of these: hiveFoot is empty there, so the first
    // line returns and there is no biome test to keep in step.
    // -----------------------------------------------------------------------
    void hangHive(ChunkBuild *b, int treeIndex, int ci, int cj, int h, int yaw, uint32_t cell,
                  int treeExtraSink) {
        if (hiveFoot.empty() || birchHiveRate <= 0.0f) return;
        if (treeIndex < 0 || size_t(treeIndex) >= pineHivePerch.size()) return;
        if (treeIndex < birchBase) return;  // a pine carries no hive

        // THE ELIGIBILITY TEST COMES BEFORE THE DRAW, and the order is the
        // difference between 1% and 0.76%.
        //
        // Drawn first, the rate is 1% of ALL birches -- and then every tree
        // whose crown offers nowhere to hang anything silently drops out
        // afterwards, so what actually reaches the wood is 1% times whatever
        // fraction of the sixteen models can carry a hive at all. Measured:
        // 0.76%. Asking "could this tree hold one" first makes the rate mean
        // what it says.
        // THE WIDE ANCHORS, not the cone ones -- a hive hung on a one-voxel
        // clear column has its four sides in the leaves.
        const std::vector<Perch> &pp = pineHivePerch[size_t(treeIndex)];
        if (pp.empty()) return;
        if (hashUnit(seed + 0xB33Fu, cell) >= birchHiveRate) return;

        const Footprint &pf = pineFoot[size_t(treeIndex)];
        const int treeSink = decorSink(0, treeIndex, pf.sy, seed, cell);

        // Four draws, keep the highest.
        const Perch *best = nullptr;
        for (int n = 0; n < 4; ++n) {
            const Perch &q = pp[hashU32(seed + 0x51EEu + uint32_t(n), cell) % pp.size()];
            if (!best || q.y > best->y) best = &q;
        }

        const int dx = int(best->x) - pf.sx / 2;
        const int dz = int(best->z) - pf.sz / 2;
        int rx = dx, rz = dz;
        switch (yaw & 3) {
            case 1: rx = dz;  rz = -dx; break;
            case 2: rx = -dx; rz = -dz; break;
            case 3: rx = -dz; rz = dx;  break;
            default: break;
        }

        const int k = int(hashU32(seed + 0x77A1u, cell) % hiveFoot.size());
        const int hyaw = int(hashU32(seed + 0x1D0Bu, cell) & 3u);
        // The perch is where the hive's TOP goes; makeInstance places by the
        // base, so drop it by the model's own height less one -- the same
        // arithmetic the cones use.
        const int top = int(best->y);
        const int base = top - (hiveFoot[size_t(k)].sy - 1);
        if (base < 0) return;
        b->decor.push_back({5, k, ci + rx, cj + rz, h, hyaw, cell, base - treeSink,
                            treeExtraSink});
    }

    // -----------------------------------------------------------------------
    // AN ORCHARD IN THE OAK WOOD -- apples and oranges, the browser engine's
    // rules brought across whole.
    //
    // (user 2026-09-17: "I want you to add apples and oranges to some of the
    //  trees in the oak forest. import the v1 mechanics of this.")
    //
    // It is hangPinecones' geometry again -- a perch, the tree's quarter turn
    // applied by hand, the tree's extra sink ridden down -- and four rules that
    // are all "a crop is not a scatter". Every one of them is the browser
    // engine's, and every one was argued there before it was written:
    //
    //   * ONE SPECIES PER TREE. An apple tree is an apple tree. Drawn on its
    //     own salt, 50/50, so it cannot correlate with the bearing roll: share
    //     that stream and every fruiting oak is the same fruit.
    //   * THE BUSH TIER BEARS NOTHING. A berry bush already carries fruit, and
    //     hanging a 40 cm apple in a 2 m shrub reads as litter. Asked as a
    //     HEIGHT rather than as an index, so adding a model to the oak set
    //     cannot quietly make a shrub bear -- see kFruitMinTreeVox.
    //   * THE COUNT COMES OFF THE CROWN'S OWN FOOTPRINT, not a constant. A
    //     young oak carries six and a giant eighteen, which is the difference
    //     between a wood with fruit in it and a wood with fruit on it.
    //   * ONE FRUIT PER COLUMN. Two apples in one column is one apple inside
    //     another; the angular draw makes it rare and does not make it
    //     impossible, so it is still refused.
    //
    // THE ELIGIBILITY TEST COMES BEFORE THE DRAW, for the reason written out
    // over hangHive: rolled first, the rate is 15% of all oaks and then every
    // tree with nowhere to hang anything silently drops out afterwards, so the
    // number stops meaning what it says.
    // -----------------------------------------------------------------------
    void hangFruit(ChunkBuild *b, int treeIndex, int ci, int cj, int h, int yaw, uint32_t cell,
                   int treeExtraSink) {
        if (fruitFoot.empty() || oakFruitRate <= 0.0f) return;
        if (treeIndex < oakBase || size_t(treeIndex) >= oakFruitPerch.size()) return;
        // ...AND NOT ON A TREE THAT IS IN FLOWER. The cherry models share the
        // oaks' perch lists by construction, so without this every cherry in
        // the band would hang apples and oranges. A crop of cherries is its own
        // model and its own palette entry and nobody has asked for one; blossom
        // and fruit are different seasons anyway.
        if (cherryBase < int(pineFoot.size()) && treeIndex >= cherryBase) return;

        const Footprint &pf = pineFoot[size_t(treeIndex)];
        // THE BUSH TIER, AS A HEIGHT. The oak set is a size ladder -- 2.1 m,
        // then 11 m and up -- so any line between the two is the same line;
        // saying it in metres is what makes it survive a new model.
        if (pf.sy < kFruitMinTreeVox) return;

        const std::vector<Perch> &pp = oakFruitPerch[size_t(treeIndex)];
        if (pp.empty()) return;
        if (hashUnit(seed + 0xF2A7u, cell) >= oakFruitRate) return;

        // ONE SPECIES, ON ITS OWN SALT.
        const int kind = int(hashU32(seed + 0x3C91u, cell) & 1u) % int(fruitFoot.size());
        const Footprint &ff = fruitFoot[size_t(kind)];

        // HOW BIG A CROP THIS CROWN CARRIES. The browser engine's expression,
        // verbatim in shape -- 2 * (3 + footprint / D), capped at 20 -- with D
        // re-derived for v2's own models rather than carried across as a
        // number. Its 2000 was calibrated against ITS oak set; v2's were
        // revoxelised half again as large on 2026-09-16 and are a different
        // ladder besides, so the constant that reproduces its documented
        // 6 / 6 / 8 / 10 / 16 / 18 ramp here is 10000. Solved rather than
        // guessed: each tier pins D to an interval and the six intersect at
        // (9702, 10213].
        //
        // THE x2 WRAPS THE WHOLE EXPRESSION, which is also the browser
        // engine's and also deliberate: folding it into the divisor instead
        // flattens the ramp's low end, and the doubling was asked for because
        // the BIG trees were carrying too few.
        const int foot = pf.sx * pf.sz;
        const int n = mini(kFruitMax, 2 * (3 + foot / kFruitFootDiv));

        const int treeSink = decorSink(0, treeIndex, pf.sy, seed, cell);

        // One fruit per column. A crop is at most 20, so a linear scan over
        // what has been placed is cheaper than any structure that could
        // replace it -- and it keeps the whole pass allocation-free.
        int takenI[kFruitMax], takenJ[kFruitMax];
        int taken = 0;

        for (int j = 0; j < n; ++j) {
            // THE j-TH FRUIT OUT OF THE j-TH ANGULAR SECTOR, jittered inside
            // it. oakFruitPerch is angle-sorted (see fruitAnchors), so this is
            // what rings the crop around the crown instead of clumping it.
            // The jitter spans 0.7 of a sector so two neighbours cannot swap
            // order and leave a gap.
            const float u = (float(j) + 0.15f +
                             hashUnit(seed + 0x6D2Bu + uint32_t(j) * 977u, cell) * 0.7f) /
                            float(n);
            const size_t a = size_t(u * float(pp.size())) % pp.size();
            const Perch &q = pp[a];

            // Offset from the model centre, then the tree's own quarter turn --
            // the same four cases as kRot, exactly as the cones do it. Get
            // this wrong and the crop hangs in the next tree along.
            const int dx = int(q.x) - pf.sx / 2;
            const int dz = int(q.z) - pf.sz / 2;
            int rx = dx, rz = dz;
            switch (yaw & 3) {
                case 1: rx = dz;  rz = -dx; break;
                case 2: rx = -dx; rz = -dz; break;
                case 3: rx = -dz; rz = dx;  break;
                default: break;
            }

            bool clash = false;
            for (int t = 0; t < taken && !clash; ++t)
                if (takenI[t] == rx && takenJ[t] == rz) clash = true;
            if (clash) continue;

            // The perch is where the fruit's TOP goes and makeInstance places
            // by the base, so drop it by the model's height less one -- the
            // cones' arithmetic, unchanged.
            const int base = int(q.y) - (ff.sy - 1);
            if (base < 0) continue;

            takenI[taken] = rx;
            takenJ[taken] = rz;
            ++taken;
            const int fyaw = int(hashU32(seed + 0x44B9u + uint32_t(j) * 613u, cell) & 3u);
            b->decor.push_back({6, kind, ci + rx, cj + rz, h, fyaw, cell, base - treeSink,
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
        // The band's waterline, as in scatter() -- see the note there.
        const int wl = terrain_.waterVoxAt(terrain_.wx(b->cx * CHUNK_VOX + CHUNK_VOX / 2));
        const int steps = int(CHUNK_M / stride);
        // A DISTINCT SALT PER KIND, so the hash streams do not line up. Two
        // kinds sharing a salt land on exactly the same cells and every
        // mushroom grows out of the middle of a flower.
        const uint32_t salt = (kind == 7)   ? 0x51F3A7C5u
                              : (kind == 8) ? 0x2D9B4E11u
                              : (kind == 1) ? kRockSalt
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
        // -- HOW MANY OF THESE ARE THE ORDINARY SET -----------------------
        //
        // The cherry wood's copies are APPENDED (see rockCherry0), so the list
        // handed in is twice as long as the one this scatter is meant to draw
        // from. Rolling over the whole of it is wrong twice over:
        //
        //   * it puts PINK-MOSSED STONES IN THE PINE WOOD, which is the copy
        //     being reachable from a band that is not the cherry;
        //   * and it changes the roll itself. `int(hash * n) % n` with n
        //     doubled picks a different model at every site in the world, so
        //     the first build of this moved every rock in every biome -- the
        //     clip test found geckos standing in trees in the DESERT, four
        //     bands away from anything that had been edited.
        //
        // So the draw is over the ordinary set and the cherry shift is applied
        // afterwards, which also makes the shift free to be a single `+=`.
        // THE FIRST COPY IS THE CEILING, whichever it is: a pinned world may
        // load the bare set without the pink one, so this cannot assume the
        // cherry base is the boundary.
        const int ordinary =
            (kind == 8 && shrubCherry0 > 0 && shrubCherry0 < int(foot.size())) ? shrubCherry0
            : (kind == 1) ? rockOrdinary()
                             : (kind == 2 && flowerCherry0 > 0 && flowerCherry0 < int(foot.size()))
                                 ? flowerCherry0
                             : (kind == 3 && mushroomCherry0 > 0 &&
                                mushroomCherry0 < int(foot.size()))
                                 ? mushroomCherry0
                                 : int(foot.size());
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
            // OVER THE ORDINARY SET, and this lambda only wants the RADIUS --
            // the cherry copy is the same geometry, so it never needs the
            // shift the placing site applies. What it does need is to draw the
            // SAME index that site draws, which is what `ordinary` guarantees.
            int kk = int(hashUnit(seed + 44u, c) * float(ordinary)) % ordinary;
            if (kind == 3 && mushroomBig0 > 0 && mushroomBig0 < ordinary) {
                const bool big = hashUnit(seed + 0x8B1Du, c) < 0.25f;
                const int lo = big ? mushroomBig0 : 0;
                const int hi = big ? ordinary : mushroomBig0;
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
                    // THE ORDINARY SET, for the reason written over it: a
                    // colony picks a SPECIES, and the cherry copies are the
                    // same species in a different palette. Counting them would
                    // both double the roll and let a pine meadow draw a pink
                    // one.
                    col = colonyAt(bx, bz, ordinary, wobMemo);
                    if (col.w <= 0.0f) continue;
                }
                float w = (kind == 2) ? col.w : 1.0f;
                // SCALED WHERE IT STANDS, not per chunk. A chunk near a seam
                // holds both woods, so a single factor for the whole chunk
                // would draw a straight edge in the mushrooms that the trees
                // and the ground around them do not have. birchMix is the same
                // 0..1 the terrain and the species selection read, so all three
                // cross the seam together.
                //
                // The CELL BASE rather than the jittered position: the jitter is
                // 1.3 m and a band is 800, so the difference cannot change the
                // answer, and asking here keeps it before the gate it feeds.
                if (kind == 3) w *= lerpf(1.0f, birchMushroomScale, terrain_.birchMix(bx));
                // THE DESERT'S TWO EXIST ONLY IN THE SAND. Multiplying by the
                // weight rather than testing it is what gives the rim its
                // fade: a cactus at the treeline is rare, one a hundred metres
                // in is ordinary, and nothing here has to know where the edge
                // is. See cactusFoot for why this is a density and not a gate.
                // -- ...AND NOT ONE STEP OUT OF IT ---------------------
                //
                // (user 2026-09-19: "I found a cactus in the cherry forest.
                //  remove it across the biome. make sure to keep the
                //  respective biomes assets within their biome.")
                //
                // THE FADE IS WHY IT LEAKED. desertMix does not stop at the
                // seam, it tapers through it -- that is the whole point of the
                // note above, and it is right about the rim looking better for
                // it. What it is wrong about is the OTHER side: a weight of
                // 0.3 two hundred metres inside the blossom is a real cactus
                // standing under a cherry tree, and no amount of rarity makes
                // that the right picture.
                //
                // So the fade stays and a GATE goes in front of it: woodBit is
                // the engine's one answer to "which wood is this column", the
                // same answer /locate and the moss on the boulder give, so the
                // sand's plants stop exactly where the sand stops being what
                // this place is called. Inside the desert nothing changes --
                // the taper toward the rim is the same taper it always was.
                if (kind == 7)
                    w *= (terrain_.woodBit(bx) & kWoodDesert) ? terrain_.desertMix(bx) : 0.0f;
                // THE BUSH GROWS IN TWO BANDS NOW. The sand has it as scrub
                // and the blossom has it as a flowering bush -- same models,
                // different bloom (see World::loadCherryBush) -- so the weight
                // is whichever of the two this column is, and it still thins
                // to nothing through every other wood.
                // ...AND THE BUSH IS GATED THE SAME WAY, ON WHICHEVER OF ITS
                // TWO BANDS THIS IS. Same argument as the cactus above: the
                // scrub taper belongs inside the sand and the flowering one
                // inside the blossom, and neither belongs in the pine between
                // them. Asked once, because woodBit is a single bit.
                // -- THE SCRUB IS THE SAND'S, AND ONLY THE SAND'S --------
                //
                // (user 2026-09-20: "I still see cactus bushes in the cherry
                //  forest, remove them".)
                //
                // THIS REVERSES "load v1's bush into the cherry forest
                // recoloured pink" (2026-09-19), and the reason it reads as a
                // cactus bush is that it IS one: the blossom's bush and the
                // desert's scrub are the same model in two palettes -- see
                // World::loadCherryBush -- so recolouring it pink made a pink
                // prickly pear rather than a woodland shrub.
                //
                // The cherry copies stay loaded and shrubCherry0 stays valid;
                // nothing else needs unpicking, and putting them back is this
                // one line.
                if (kind == 8)
                    w *= (terrain_.woodBit(bx) & kWoodDesert) ? terrain_.desertMix(bx) : 0.0f;
                // -- HALF THE STONE IN THREE OF THE FIVE BANDS -------------
                //
                // (user 2026-09-19: "reduce the rock density in the desert by
                //  half", and "in the cherry forest, cut down the rocks by
                //  50%. if the oak forest matches the density of the cherry
                //  forest, then reduce it by 50% as well".)
                //
                // THE OAK AND THE CHERRY DO MATCH, and not by coincidence --
                // the cherry band's weight folds into the oak's, so every
                // density in the engine answers in the cherry with the number
                // it gives in the oak, by construction. That is the whole of
                // [[v2-cherry-forest]]'s fold, and it is why the second half
                // of the ask is a yes: they are the same number, so halving
                // one halves the other whether you meant it or not. oakMix
                // covers both bands for exactly that reason.
                //
                // A WEIGHT, NOT A TEST, like everything else on this line: the
                // stone thins across the rim instead of halving on a line, and
                // a column that is half oak and half pine gets three quarters.
                if (kind == 1)
                    w *= 1.0f - 0.5f * clampf(maxf(terrain_.oakMix(bx),
                                                   terrain_.desertMix(bx)),
                                              0.0f, 1.0f);
                if (hashUnit(seed + 41u, cell) >= density * w) continue;

                const float x = bx + (hashUnit(seed + 42u, cell) - 0.5f) * stride;
                const float z = bz + (hashUnit(seed + 43u, cell) - 0.5f) * stride;
                const int ci = int(floorf(x / VOXEL_M));
                const int cj = int(floorf(z / VOXEL_M));
                if (ci < I0 || ci >= I0 + CHUNK_VOX || cj < J0 || cj >= J0 + CHUNK_VOX) continue;

                // WHERE THE MODEL ACTUALLY LANDS, which is not (x, z). The
                // jittered position picks a COLUMN and the model is then
                // centred on that column -- makeInstance offsets by
                // halfOf(fx), which puts the bounding-box centre exactly on
                // ci * VOXEL_M. So x and z are up to half a voxel out, and the
                // "is this inside something" tests below have to be asked at
                // the place the thing is drawn or they answer about a position
                // nothing occupies. Measured: that half voxel was the whole of
                // the residue left after the trunk discs were re-centred --
                // 0.3-0.5% of the small decor still grazing a trunk by up to
                // 9 cm, which this takes to zero.
                const float px = float(ci) * VOXEL_M, pz = float(cj) * VOXEL_M;

                const int h = terrain_.heightVox(ci, cj, memo);
                // The band line, then the mapped lakes -- see the note in the
                // tree sweep. Asked at px/pz, where the model actually lands.
                if (h <= wl + 2) continue;
                if (terrain_.mappedWater(px, pz)) continue;
                const uint8_t top = terrain_.topMaterial(ci, cj, h, memo);
                // ---------------------------------------------------------
                // A FLOWER STANDS IN THE GRASS, so it asks whether there IS
                // any -- not whether the FLOOR is green.
                //
                // The old test was `isGrass(top)`, and it was right while the
                // ground itself was painted mat::GRASS_0 over half the world.
                // The floor is soil and litter everywhere now and the grass is
                // only the blades, so that test answered false on every column
                // in the world and the flowers silently stopped being placed
                // -- 0 of them, where the note further up this file records
                // 276 in the same sweep.
                //
                // Asking for a BLADE is the same question the old test was
                // really asking, and it is a better one: a flower now grows
                // where grass actually grows, in both woods, rather than
                // wherever a paint happened to be.
                // ---------------------------------------------------------
                const int rows = grassOnly ? terrain_.strandRows(ci, cj, top, memo) : 0;
                if (grassOnly && rows <= 0) continue;
                // ...BUT NOT ON A TALL ONE (user 2026-09-14: "dont put flowers
                // on tall grass"). yLift below puts the bloom on TOP of the
                // blade it grows in, and a tuft blade is 15-20 voxels, so this
                // was a flower floating up to two metres over the floor with
                // nothing under it -- the stem is a few voxels long and the
                // grass it was supposed to be standing in is a separate model.
                //
                // The COLUMN is refused rather than the lift being capped: a
                // flower lying at the foot of grass twice its height is not
                // what a flower does either. Tufts are a few per cent of the
                // floor, so this costs almost nothing in bloom count -- see
                // tests/tall_grass_test.cpp, which counts both.
                if (kind == 2 && terrain_.tallStrand(rows)) continue;

                // The species is the COLONY's, not this cell's: that is the
                // whole point of the patch.
                // OVER THE ORDINARY SET, never the cherry copy -- see the
                // note over `ordinary`. The shift into the copy is applied
                // below, after the wood's own half-pick has had its say.
                int k = (kind == 2)
                            ? col.species
                            : int(hashUnit(seed + 44u, cell) * float(ordinary)) % ordinary;
                // ...AND A ROCK DOES NOT REPEAT ITS NEIGHBOUR -- see
                // rockVariety, which collectRocks calls with the same
                // arguments so the disc it reserved is this stone's box and
                // not some other stone's.
                if (kind == 1) k = rockVariety(bx, bz, x, z, cell, k, ordinary);
                // WHICH WOOD'S STEM. The colony picked a species out of the
                // first half; the second half is the same flowers wearing the
                // birch's green. Dithered on the column hash exactly as
                // bladeMaterial is, so the two do not disagree about a column
                // and the changeover is not a straight line.
                if (kind == 2 && flowerBirch0 > 0 && k < flowerBirch0 &&
                    terrain_.bladeMaterial(ci, cj) == mat::BGRASS_0)
                    k += flowerBirch0;

                // -- ...AND IN THE CHERRY BAND IT IS THE PINK COPY ---------
                //
                // AFTER the birch half above and BEFORE the mushroom size draw
                // below, and both orderings are load-bearing. The half above
                // decides WHICH flower and this only decides which palette it
                // wears, so running it first would shift into the copy and then
                // the half-adjust would shift straight back out of it. The draw
                // below picks from foot.size(), which includes this copy, so it
                // has to do its own shift rather than inherit one.
                if (ordinary < int(foot.size()) && k < ordinary) {
                    // -- ASK WHICH WOOD THIS IS, ONCE -----------------------
                    //
                    // (user 2026-09-19: "I see rocks in the desert that have
                    //  pink moss".)
                    //
                    // THIS TESTED cherryMix >= 0.5 AND THEN desertMix >= 0.5,
                    // and at the cherry|desert seam BOTH ARE EXACTLY 0.5 --
                    // measured at x = 3200. Cherry was asked first, so a two
                    // hundred metre strip of ground that is already half sand,
                    // and that woodName and /locate both call DESERT, was
                    // getting the pink-mossed stones.
                    //
                    // woodBit is the engine's one answer to "which wood is
                    // this": it returns a SINGLE bit, picked by the largest
                    // weight, so two bands can no longer both claim a column.
                    // Using it also means the stone agrees with the name --
                    // whatever /locate calls the ground you are standing on is
                    // the moss on the boulder beside you.
                    const uint8_t wood = terrain_.woodBit(px);
                    // -- HALF THE BIG STONES IN THE SAND ------------------
                    //
                    // (user 2026-09-19: "reduce the desert big/med rocks in
                    //  half".)
                    //
                    // NOT a second density: the count is already halved out
                    // here and halving it again would empty the dunes. This is
                    // about the MIX -- a big or a mid rock is sent to the
                    // pebble range half the time, so the sand keeps its stones
                    // and loses half its boulders.
                    //
                    // Its own hash stream, so the swap cannot correlate with
                    // which model was drawn: sharing seed+44 would send the
                    // same four boulders every time and keep the other one.
                    if (kind == 1 && (wood & kWoodDesert) && rockSmall0 > 0 &&
                        rockSmall0 < ordinary && k < rockSmall0 &&
                        hashUnit(seed + 0x5A17u, cell) < 0.5f) {
                        const int span = ordinary - rockSmall0;
                        k = rockSmall0 + (int(hashUnit(seed + 0x5A18u, cell) * float(span)) % span);
                    }
                    if (kind == 1 && (wood & kWoodDesert) && rockDesert0 > ordinary &&
                        rockDesert0 < int(foot.size()))
                        k += rockDesert0;
                    // Only kind 1 has a third range -- the flowers and the
                    // mushrooms do not grow out in the sand at all.
                    // -- ...AND PINK MOSS NEEDS BLOSSOM OVERHEAD ---------
                    //
                    // (user 2026-09-19, reported three times: "pink moss rocks
                    //  are still spawning outside the cherry forest".)
                    //
                    // THE BAND WAS NEVER THE WHOLE QUESTION. A diagnostic on
                    // every placed stone found ZERO columns where the range
                    // disagreed with woodBit, in five bands and in the measured
                    // world -- the rule was being followed exactly. What it was
                    // being asked was wrong: the cherry BAND runs up the
                    // mountain, and above the timberline it has no cherries in
                    // it. So the pink stones were real, correct by the rule,
                    // and sitting on bare alpine grass under snow with not a
                    // blossom in sight. That is what the report is.
                    //
                    // Petals already ask this question (see petalColumn) and
                    // they ask it of the stand field, so the two now agree
                    // about what "under the trees" means: where there is no
                    // canopy there is no fallen blossom and no pink moss.
                    // -- AND A MOSSED STONE HAS TO BE UNDER AN ACTUAL TREE --
                    //
                    // (user 2026-09-21: "if the pink moss large rocks are not
                    //  inside the tree line, remove the pink moss".)
                    //
                    // FOURTH REPORT, AND THE GATE WAS THE PROXY. This asked
                    // `standDensity > kPetalStand` -- the sixty-metre fbm --
                    // which says "thick wood somewhere around here" and not
                    // "there is a cherry over this stone". A boulder on the
                    // open edge of a stand passed it, and that is the pink rock
                    // outside the tree line.
                    //
                    // underCanopy replays the mesher's own tree lattice, so it
                    // answers about TREES. The petals moved to the same test in
                    // the same batch -- the note below about the two agreeing
                    // is still the rule, it is just on the right question now.
                    else if ((wood & kWoodCherry) &&
                             (kind != 1 || terrain_.underCanopy(px, pz, memo)) &&
                             (kind != 1 || (rockCherry0 > 0 && rockCherry0 < int(foot.size()))))
                        k += (kind == 1) ? rockCherry0 : ordinary;
                }

                // -- A STONE MUST WEAR THE WOOD IT STANDS IN -------------
                //
                // (user 2026-09-19, third report: "pink moss rocks are still
                //  spawning outside the cherry forest".)
                //
                // Twice now the logic has read correctly and the world has
                // disagreed, so this stops arguing and MEASURES: every placed
                // stone checks the range it landed in against the wood under
                // it, and a mismatch names its coordinate. If it never fires,
                // the pink stones are inside the blossom and the report is
                // about the seam; if it does, it says where.

                // TWO SIZES OF MUSHROOM, one in four of them the big one. The
                // draw is a separate hash stream from the species pick above:
                // sharing one would tie "which mushroom" to "how big", so every
                // big one would always be the same model.
                if (kind == 3 && mushroomBig0 > 0 && mushroomBig0 < int(foot.size())) {
                    // THE CHERRY COPY IS NOT PART OF THE SIZE DRAW. `hi` used
                    // to be the whole array, which was the whole array while
                    // the array was small-then-big; with a pink duplicate on
                    // the end that range spans the seam, so a "big" draw could
                    // land on a small pink one. The ceiling is the copy's base
                    // when there is one -- and the shift back into the copy is
                    // re-applied below, because this assignment overwrites the
                    // k the block above had already shifted.
                    const int capTop =
                        (mushroomCherry0 > mushroomBig0 && mushroomCherry0 < int(foot.size()))
                            ? mushroomCherry0
                            : int(foot.size());
                    const bool big = hashUnit(seed + 0x8B1Du, cell) < 0.25f;
                    const int lo = big ? mushroomBig0 : 0;
                    const int hi = big ? capTop : mushroomBig0;
                    k = lo + (int(hashUnit(seed + 0x3C7Fu, cell) * float(hi - lo)) % (hi - lo));
                    // THE SAME QUESTION EVERY OTHER DECORATION ASKS. This
                    // read `cherryMix >= 0.5`, which is a second opinion about
                    // where the blossom is -- and a chain of `mix >= 0.5` tests
                    // is exactly what put pink moss in the sand at the
                    // cherry|desert seam, where both are exactly a half. See
                    // the shift block above, and woodBit.
                    if (capTop < int(foot.size()) && (terrain_.woodBit(px) & kWoodCherry))
                        k += capTop;
                }
                const int yaw = int(hashUnit(seed + 45u, cell) * 4.0f) & 3;
                // ON TOP OF THE BLADE. yOff is exactly this mechanism -- see
                // Placement, where a pinecone uses it for the branch it is
                // perched on. One voxel down into the sward so the stem meets
                // the blade rather than floating a hair above it.
                const int yLift = (kind == 2 && rows > 0) ? maxi(0, rows - 1) : 0;
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
                        for (const Disc &d : *avoid)
                            if (d.reaches(px, pz, br)) { refuse = true; break; }
                    }

                    // -- AND NO BOULDERS UNDER THE OAKS ---------------------
                    //
                    // (user 2026-09-16: "remove the huge rocks in the oak
                    // forest".)
                    //
                    // BIG AND MID BOTH, and the measurements are why. The
                    // first cut took only the big five on the reasoning that
                    // "huge" meant the twenty-metre stones -- and left a mossy
                    // eight-metre boulder standing over the grass in the middle
                    // of the very first render. Measured across the set:
                    //
                    //     big    0..4    12.4 .. 22.2 m across
                    //     mid    5..10    6.7 ..  8.4 m across
                    //     runic 11..17    0.8 m
                    //     small 18..25    1.7 m
                    //
                    // There is no ambiguity about where "huge" stops: the step
                    // from mid to runic is a factor of EIGHT. Anything the
                    // re-roll below can reach is under two metres, so the oak
                    // keeps stones you walk around and loses the ones you walk
                    // under.
                    //
                    // REFUSED RATHER THAN SKIPPED, which is why this sits here
                    // and not at the top of the loop. Refusing re-rolls the
                    // model into the small range a few lines down -- the same
                    // path a spot too steep for a boulder already takes -- so
                    // the oak keeps its rock COUNT and loses only the size. A
                    // `continue` would have taken a stone off the ground every
                    // time, and thinned the wood instead of changing it.
                    //
                    // DITHERED ON THE COLUMN, not switched at oakMix = 0.5.
                    // Boulders thin out across the seam rather than stopping on
                    // a line of constant x, which is the same trick the floor
                    // and the blades use a few hundred lines away.
                    // HALF AS MANY MID AND BIG ROCKS (user 2026-09-18). Refused, not
                    // skipped, for the reason the note above gives: refusing re-rolls the
                    // model down into the small range, so the ground keeps its rock COUNT
                    // and loses only the SIZE. A `continue` here would thin the field
                    // instead, which is a different request.
                    //
                    // ITS OWN HASH SALT, so this composes with the oak's dither rather
                    // than fighting it, and the rocks that survive keep the positions and
                    // models they already had.
                    if (!refuse && k < kRockBigMidEnd && hashUnit(seed + 0x5B19u, cell) < 0.5f)
                        refuse = true;
                    // AND THE BIGGEST ONES AGAIN (user 2026-09-18: "reduce the frequency of
                    // big rocks in half"). kRockBigEnd is one past Big_5, so this catches
                    // only the true boulders and leaves the Mid_ range to the pass above.
                    // Compounded deliberately: a Big rock now has to survive both rolls.
                    if (!refuse && k < kRockBigEnd && hashUnit(seed + 0x6C2Du, cell) < 0.5f)
                        refuse = true;
                    if (!refuse && k < kRockBigMidEnd &&
                        terrain_.oakMix(px) > hashUnit(seed + 0x0A4Bu, cell))
                        refuse = true;

                    if (refuse) {
                        // -- INTO THE ORDINARY SMALL STONES, AND ONLY THOSE --
                        //
                        // (user 2026-09-19, the FOURTH report: "there are STILL
                        //  pink moss rocks outside the cherry forest".)
                        //
                        // THIS IS WHERE THEY CAME FROM, AND IT IS WHY THREE
                        // FIXES ABOVE ALL READ CORRECTLY. The band shift two
                        // hundred lines up chooses the range for the wood and
                        // gets it right; this re-roll then throws that away and
                        // draws from `foot.size()`, which is the WHOLE list --
                        // the ordinary stones, the cherry wood's pink copies
                        // and the desert's bare ones, end to end.
                        //
                        // So any boulder in any wood that was refused a site --
                        // for steepness, or by either of the two halving
                        // dithers, or by the oak's -- was re-rolled across all
                        // three sets. In the pine that is a pink stone under
                        // green trees a hundred metres outside the blossom,
                        // which is precisely the report.
                        //
                        // THE DIAGNOSTIC MISSED IT BY TWO HUNDRED LINES. The
                        // ROCK MISMATCH check that reported zero sat ABOVE this
                        // and measured the k the band shift had just set -- the
                        // value that was always right. It is below now.
                        //
                        // THE CEILING IS `ordinary`, which is the base of the
                        // first copy (see rockOrdinary). No band shift has to
                        // be re-applied afterwards: this block only runs when
                        // `k < kRockBigMidEnd`, and any shift would have put k
                        // far above that -- so a column that reaches here is
                        // one the shift declined.
                        //
                        // The mushroom draw has the same note for the same
                        // reason, one screen down: "with a pink duplicate on
                        // the end that range spans the seam".
                        const int smallTop =
                            (ordinary > kRockBigMidEnd && ordinary <= int(foot.size()))
                                ? ordinary
                                : int(foot.size());
                        const int nSmall = smallTop - kRockBigMidEnd;
                        if (nSmall <= 0) continue;
                        k = kRockBigMidEnd +
                            (int(hashUnit(seed + 0x5A17u, cell) * float(nSmall)) % nSmall);
                    }
                }

                // -- A PINK STONE MUST BE IN THE BLOSSOM ------------------
                //
                // BELOW EVERYTHING THAT PICKS A MODEL, which is the whole
                // lesson of the fourth report: the first version of this check
                // sat above the boulder re-roll, measured the one value that
                // was always correct, printed nothing, and was read as proof
                // the placement was right.
                //
                // ONE-DIRECTIONAL, and that is not laziness. A PLAIN stone in
                // the cherry band is legitimate -- above the timberline the
                // band has no cherries in it and the pink is gated on the stand
                // field (see the shift above) -- so only the other direction is
                // a fault.
                if (kind == 1 && rockCherry0 > 0 && rockCherry0 < int(foot.size())) {
                    const bool pink =
                        k >= rockCherry0 && (rockDesert0 <= rockCherry0 || k < rockDesert0);
                    if (pink && !(terrain_.woodBit(px) & kWoodCherry) &&
                        rockWarn_.fetch_add(1) < 8) {
                        std::printf("  ROCK MISMATCH  k %d of %d at (%.0f, %.0f) -- "
                                    "pink stone in the %s wood\n",
                                    k, int(foot.size()), double(px), double(pz),
                                    terrain_.woodName(px));
                        std::fflush(stdout);
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
                        int oi = 0, oj = 0;
                        baseOffsetVox(f, yaw, &oi, &oj);
                        const int drop = groundDrop(ci, cj, h, footX, footZ, memo, oi, oj);
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

                // NOTHING SMALL GROWS OUT OF A BOULDER -- OR OUT OF A
                // TRUNK. Last of all the tests, so it only runs for a placement
                // everything else has already accepted, and a linear scan
                // because a chunk's neighbourhood holds a couple of dozen rocks
                // and a hundred or so trees at most.
                //
                // No slack here, unlike the same-kind spacing above, which
                // allows a 0.9 overlap so two stones may touch. A mushroom is
                // not touching a rock, it is INSIDE it, and there is no
                // distance at which that reads as intentional.
                if (avoid) {
                    const Footprint &sf = foot[size_t(k)];
                    const float mr = 0.25f * float(sf.sx + sf.sz) * VOXEL_M;
                    bool inside = false;
                    for (const Disc &d : *avoid)
                        if (d.reaches(px, pz, mr)) { inside = true; break; }
                    if (inside) continue;
                }

                b->decor.push_back({kind, k, ci, cj, h, yaw, cell, yLift, extraSink});
            }
        }
    }
};

}  // namespace v2
