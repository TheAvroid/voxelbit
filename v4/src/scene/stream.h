// ---------------------------------------------------------------------------
// stream.h -- A WORLD THAT FOLLOWS THE PLAYER, AND HAS NO EDGE.
//
// The world used to be a box: one extent, built once at load, and a wall of
// nothing at 96 m. This keeps a WINDOW of it resident instead -- a square of
// chunks centred on wherever the player is -- and slides that window as they
// move, building the chunks that enter and dropping the ones that leave.
//
// ---------------------------------------------------------------------------
// WHY THIS IS ONLY A HUNDRED LINES
//
// Because everything hard about it was made true first, and each piece is
// checked on its own:
//
//   NOTHING IS GLOBAL. The one world-wide reduction -- the waterline, which was
//   a percentile of every height in the build -- became a percentile of the
//   FIELD, taken once. Two chunks built an hour apart agree about where the
//   water is without either of them knowing the other exists. See waterLineVox.
//
//   A CHUNK IS THE SAME GROUND WHEREVER IT IS BUILT FROM. Proved by building a
//   world twice, in one piece and a chunk at a time, and comparing every voxel:
//   303 million of them, none different. See testChunkedMatchesWhole.
//
//   EVERY VOXEL HAS EXACTLY ONE OWNER. Writing is clipped to the chunk that
//   owns the column, so a boulder straddling a seam is written by one chunk and
//   not by the other -- which is what lets either be dropped. See ClipWriter.
//
// ---------------------------------------------------------------------------
// THE COLUMNS ARE THE WINDOW'S, NOT THE CHUNK'S, AND THAT IS THE OPTIMISATION
//
// A chunk needs the ground for 21.7 m beyond its own edge: a big boulder
// reaches 12.7 m from its anchor and is seated on the lowest ground under a
// 9 m footprint, so a chunk has to know about ground it does not own. Computed
// per chunk that margin is FOUR TIMES the chunk's own area -- the noise would
// be evaluated four times over for every column in the world.
//
// So the columns belong to the window and are computed once each. The array
// WRAPS (see ColGrid::wrap), so sliding the window does not move any column's
// slot: the strip that leaves frees exactly the slots the strip that enters
// needs, and the work per step is the strip rather than the window. Crossing a
// chunk boundary re-evaluates 25.6 m of new ground, not 230 m of old.
// ---------------------------------------------------------------------------
#ifndef V4_SCENE_STREAM_H
#define V4_SCENE_STREAM_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

#include "generate.h"

namespace v4 {

class StreamWorld {
  public:
    // A chunk is the store's chunk: 32 bricks of 8 voxels, 25.6 m. Matching it
    // is what lets one resident chunk be one bottom-level structure.
    static constexpr int kChunkCols = 32 * BRICK_E;

    // How far the ground must be known beyond the chunk that uses it. 127 for
    // the reach of the largest boulder, 90 for the footprint it is seated over,
    // and a little over for the neighbour rules. Measured, not chosen.
    static constexpr int kColMargin = 224;

    struct Change {
        std::vector<ColGrid> added, removed;
        bool moved = false;
        // Where a crossing's time went: evaluating the noise for the strip of
        // ground that came into view, against writing the voxels of the chunks
        // that entered. They have different fixes, so they are measured apart.
        double columnsMs = 0.0, chunksMs = 0.0;
    };

    // Chunks each way from the one the player is in: 4 gives a 9-chunk window,
    // 230 m across, which is about the resident set the fixed world had.
    void configure(int radiusChunks) { radius_ = std::max(1, radiusChunks); }
    int radiusChunks() const { return radius_; }
    float residentM() const { return float(span()) * VOXEL_M; }

    const BrickWorld &world() const { return world_; }
    BrickWorld &world() { return world_; }
    bool started() const { return started_; }
    int centreX() const { return cx_; }
    int centreZ() const { return cz_; }
    size_t residentChunks() const { return resident_.size(); }
    double lastMergeMs() const { return lastMergeMs_; }
    double lastBuildMs() const { return lastBuildMs_; }
    int lastThreads() const { return lastThreads_; }

    // -----------------------------------------------------------------------
    // Put the window somewhere and build everything in it. The one expensive
    // call; every later one moves a strip.
    // -----------------------------------------------------------------------
    void start(const GenOptions &o, const Content &c, float camX, float camZ) {
        o_ = o;
        c_ = &c;
        wc_ = WorldConstants::of(o_);
        cx_ = chunkOf(camX);
        cz_ = chunkOf(camZ);
        started_ = true;

        cols_ = Columns();
        cols_.g = windowGrid(cx_, cz_);
        cols_.soilVox = std::max(0, o_.soilVox);
        cols_.stoneVox = std::max(0, o_.stoneVox);
        cols_.bedrockVox = std::max(0, o_.bedrockVox);
        cols_.column = 1 + cols_.soilVox + cols_.stoneVox + cols_.bedrockVox;
        if (o_.skinM > 0.0f)
            cols_.column = std::min(cols_.column, std::max(1, int(o_.skinM / VOXEL_M)));
        const size_t n = cols_.g.count();
        cols_.height.assign(n, 0);
        cols_.bare.assign(n, 0);
        cols_.wet.assign(n, 0);
        cols_.sand.assign(n, 0);
        fillColumns(cols_, o_, wc_, cols_.g);

        world_.clear();
        publishColumns();
        resident_.clear();
        std::vector<std::pair<int, int>> all;
        for (int j = cz_ - radius_; j <= cz_ + radius_; ++j)
            for (int i = cx_ - radius_; i <= cx_ + radius_; ++i) all.emplace_back(i, j);
        buildChunks(all);
    }

    // -----------------------------------------------------------------------
    // Follow the player. Does nothing at all until they cross into another
    // chunk, which is the common case and has to cost nothing.
    // -----------------------------------------------------------------------
    // Would a crossing happen here? Asked every frame, so it must cost nothing.
    bool wouldMove(float camX, float camZ) const {
        return started_ && (chunkOf(camX) != cx_ || chunkOf(camZ) != cz_);
    }
    // How far behind the player the window has fallen, in chunks. Trailing is
    // normal at speed; see kMaxStep.
    int lagChunks(float camX, float camZ) const {
        return std::max(std::abs(chunkOf(camX) - cx_), std::abs(chunkOf(camZ) - cz_));
    }

    // -----------------------------------------------------------------------
    // THE EXPENSIVE HALF, AND IT TOUCHES NOTHING ANYONE IS READING.
    //
    // prepare() evaluates the noise for the strip of ground that just came into
    // view and builds the chunks that entered, into worlds of their own. The
    // resident world is not modified and neither is the copy of the columns the
    // walker reads, so this can run on a worker thread while the frame carries
    // on drawing the window as it was.
    //
    // commit() is the other half: erase what left, take in what arrived,
    // republish the columns. That one has to happen where nothing else is
    // reading the world -- on the render thread, between frames -- and it is
    // the only part that does.
    // -----------------------------------------------------------------------
    // AT MOST THIS MANY CHUNKS PER CROSSING, and it is what stops a runaway.
    //
    // A crossing costs work in proportion to how far the window moved. Fly fast
    // enough and the player gains ground during one, so the next is bigger,
    // which takes longer, which loses more ground still: measured at 36 m/s the
    // window went 25 chunks behind, then 119, then 265, then the whole 625 --
    // each crossing slower than the last, and a hitch that grew from 110 ms to
    // 3 seconds.
    //
    // Capping the step makes the work per crossing bounded whatever the player
    // does. The window then TRAILS rather than jumping: four chunks a crossing
    // against a pipeline of about a second is 100 m/s of catch-up, comfortably
    // faster than flying, so it closes the gap over the next few crossings and
    // the hitch stays the size it is meant to be.
    // ONE CHUNK, because at this view distance one chunk of movement is
    // already a whole edge column of the window -- 25 chunks of work for a
    // 25x25 ring. Stepping four at a time to catch up quicker just made one
    // crossing do four columns: 184 chunks, a 750 ms commit, and a hitch four
    // times the size for no more ground covered per second.
    //
    // At one, a crossing is the smallest unit of work there is and the hitch
    // stays where it belongs. If the player is faster than that, the window
    // TRAILS -- it holds station a few chunks behind and you see less ahead and
    // more behind, which at 640 m across is not something you can pick out.
    static constexpr int kMaxStep = 1;

    void prepare(float camX, float camZ) {
        if (!started_) return;
        int nx = chunkOf(camX), nz = chunkOf(camZ);
        // ...EXCEPT WHEN IT IS NOT MOVEMENT AT ALL. Creeping a chunk at a time
        // towards somewhere the player has teleported to would take minutes, so
        // a gap the window could never close by walking is closed in one go.
        // It is the expensive path and it is meant to be: it happens once.
        const int lag = std::max(std::abs(nx - cx_), std::abs(nz - cz_));
        if (lag <= 2 * radius_ + 1) {
            nx = std::max(cx_ - kMaxStep, std::min(cx_ + kMaxStep, nx));
            nz = std::max(cz_ - kMaxStep, std::min(cz_ + kMaxStep, nz));
        }
        if (nx == cx_ && nz == cz_) return;

        pending_ = Pending();
        pending_.valid = true;
        pending_.cx = nx;
        pending_.cz = nz;

        // NO SPECIAL CASE FOR A JUMP CLEAR OF THE WINDOW. There was one -- it
        // called start() from commit(), which is the render thread, and a
        // 5.4-second freeze. The ordinary path already handles it: freshStrips
        // returns the whole new rectangle when the two do not overlap, and the
        // add list becomes every chunk in it. All of that is worker-side, and
        // only the splice is not.

        const ColGrid oldWin = cols_.g;
        cols_.g = windowGrid(nx, nz);
        const auto tc0 = std::chrono::steady_clock::now();
        pending_.strips = freshStrips(oldWin, cols_.g);
        for (const ColGrid &strip : pending_.strips) fillColumns(cols_, o_, wc_, strip);
        pending_.columnsMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc0)
                .count();

        for (int j = cz_ - radius_; j <= cz_ + radius_; ++j)
            for (int i = cx_ - radius_; i <= cx_ + radius_; ++i)
                if (std::abs(i - nx) > radius_ || std::abs(j - nz) > radius_)
                    pending_.evict.emplace_back(i, j);
        for (int j = nz - radius_; j <= nz + radius_; ++j)
            for (int i = nx - radius_; i <= nx + radius_; ++i)
                if (!resident_.count(key(i, j))) pending_.add.emplace_back(i, j);

        const auto tk0 = std::chrono::steady_clock::now();
        stageChunks(pending_.add, pending_.staged);
        pending_.chunksMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tk0)
                .count();
    }

    bool hasPending() const { return pending_.valid; }

    bool commit(Change *out = nullptr) {
        if (!pending_.valid) return false;
        cx_ = pending_.cx;
        cz_ = pending_.cz;
        publishStrips(pending_.strips);
        for (const auto &e : pending_.evict) {
            auto it = resident_.find(key(e.first, e.second));
            if (it == resident_.end()) continue;
            world_.eraseBricks(it->second);
            resident_.erase(it);
            if (out) out->removed.push_back(chunkGrid(e.first, e.second));
        }
        for (size_t k = 0; k < pending_.add.size(); ++k) {
            std::vector<uint64_t> &took =
                resident_[key(pending_.add[k].first, pending_.add[k].second)];
            took.clear();
            world_.absorb(std::move(pending_.staged[k]), &took);
            if (out) out->added.push_back(chunkGrid(pending_.add[k].first, pending_.add[k].second));
        }
        if (out) {
            out->moved = true;
            out->columnsMs = pending_.columnsMs;
            out->chunksMs = pending_.chunksMs;
        }
        pending_ = Pending();
        return true;
    }

    // The one-shot form: prepare and commit together, for callers with no
    // worker thread -- the offline probes and the test harness.
    bool follow(float camX, float camZ, Change *out = nullptr) {
        if (!started_) return false;
        const int nx = chunkOf(camX), nz = chunkOf(camZ);
        if (nx == cx_ && nz == cz_) return false;
        prepare(camX, camZ);
        return commit(out);
    }

  private:
    static int chunkOf(float m) { return floorDiv(int(std::floor(m / VOXEL_M)), kChunkCols); }
    static uint64_t key(int i, int j) {
        return (uint64_t(uint32_t(i)) << 32) | uint64_t(uint32_t(j));
    }
    int span() const { return (2 * radius_ + 1) * kChunkCols; }

    ColGrid chunkGrid(int ci, int cj) const {
        ColGrid r;
        r.i0 = ci * kChunkCols;
        r.j0 = cj * kChunkCols;
        r.w = kChunkCols;
        r.h = kChunkCols;
        return r;
    }

    ColGrid windowGrid(int ci, int cj) const {
        ColGrid g;
        g.i0 = (ci - radius_) * kChunkCols - kColMargin;
        g.j0 = (cj - radius_) * kChunkCols - kColMargin;
        g.w = span() + 2 * kColMargin;
        g.h = g.w;
        g.wrap = true;  // the whole point -- see ColGrid::wrap
        return g;
    }

    // -----------------------------------------------------------------------
    // BUILD A SET OF CHUNKS, ON EVERY CORE.
    //
    // A chunk writes only the voxels inside its own rectangle (see ClipWriter)
    // and a rectangle is a whole number of bricks, so two chunks can never
    // touch the same brick -- which is exactly the condition for building them
    // at the same time. Each fills a world of its own and they are moved into
    // the resident one afterwards, which is a walk of hash-map nodes rather
    // than a re-write of voxels.
    //
    // NOT INTO THE RESIDENT WORLD DIRECTLY, even though their bricks are
    // disjoint: they share one hash map, and a map that rehashes while three
    // other threads are inserting into it is not a map.
    //
    // Measured on a 9-chunk crossing: 416 ms -> 78 ms.
    // -----------------------------------------------------------------------
    void buildChunks(const std::vector<std::pair<int, int>> &cs) {
        std::vector<BrickWorld> staged;
        stageChunks(cs, staged);
        for (size_t k = 0; k < cs.size(); ++k) {
            std::vector<uint64_t> &took = resident_[key(cs[k].first, cs[k].second)];
            took.clear();
            world_.absorb(std::move(staged[k]), &took);
        }
    }

    void stageChunks(const std::vector<std::pair<int, int>> &cs, std::vector<BrickWorld> &staged) {
        staged.clear();
        if (cs.empty()) return;
        staged.resize(cs.size());
        auto one = [&](size_t k) {
            const ColGrid own = chunkGrid(cs[k].first, cs[k].second);
            ColGrid disc = own;
            disc.i0 -= kColMargin;
            disc.j0 -= kColMargin;
            disc.w += 2 * kColMargin;
            disc.h += 2 * kColMargin;
            buildBody(staged[k], *c_, o_, cols_, own, disc, /*clipped*/ true);
        };

        const auto tb0 = std::chrono::steady_clock::now();
        unsigned nt = std::thread::hardware_concurrency();
        if (nt == 0) nt = 1;
        if (nt > 16u) nt = 16u;
        if (cs.size() < 2) nt = 1;
        if (nt <= 1) {
            for (size_t k = 0; k < cs.size(); ++k) one(k);
        } else {
            std::vector<std::thread> th;
            th.reserve(nt);
            std::atomic<size_t> next{0};
            for (unsigned t = 0; t < nt; ++t)
                th.emplace_back([&]() {
                    // A WORK QUEUE AND NOT A SLICE: chunks differ by several
                    // times in how much they hold -- a wood against a bare
                    // hillside -- so a static split leaves cores idle behind
                    // whichever thread drew the forest.
                    for (;;) {
                        const size_t k = next.fetch_add(1);
                        if (k >= cs.size()) break;
                        one(k);
                    }
                });
            for (std::thread &t : th) t.join();
        }

        lastBuildMs_ =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb0)
                .count();
        lastThreads_ = int(nt);
    }

    // Only the ground that moved, which at a large view distance is the
    // difference between a 170 ms commit and a 40 ms one. See
    // BrickWorld::republishStrips for why this is a copy at all.
    void publishStrips(const std::vector<ColGrid> &strips) {
        if (!o_.explicitFill) {
            ImplicitColumns &im = world_.implicitMut();
            if (im.height.size() != cols_.height.size() || im.surf.size() != cols_.height.size()) {
                world_.setImplicit(implicitFrom(cols_));  // first publish, or a new window shape
            } else {
                // Everything but the two arrays: the rectangle above all, or
                // the window would answer for where it used to be.
                im.g = cols_.g;
                im.column = cols_.column;
                im.soilVox = cols_.soilVox;
                im.bedrockVox = cols_.bedrockVox;
                // ...then only the ground that moved. Both sides index the same
                // wrapped array the same way, so a column's slot is its slot.
                for (const ColGrid &r : strips)
                    for (int j = r.j0; j < r.j1(); ++j)
                        for (int i = r.i0; i < r.i1(); ++i) {
                            const size_t k = cols_.g.idx(i, j);
                            im.height[k] = cols_.height[k];
                            im.surf[k] = cols_.sand[k] ? 1u : (cols_.bare[k] ? 2u : 0u);
                        }
            }
        }
        world_.setWaterM(cols_.waterY > kNoWaterLine ? float(cols_.waterY) * VOXEL_M + VOXEL_M
                                                     : -1e9f);
    }

    void publishColumns() {
        if (!o_.explicitFill) world_.setImplicit(implicitFrom(cols_));
        world_.setWaterM(cols_.waterY > kNoWaterLine ? float(cols_.waterY) * VOXEL_M + VOXEL_M
                                                     : -1e9f);
    }

    // The parts of `now` that `was` did not cover, as up to two rectangles.
    static std::vector<ColGrid> freshStrips(const ColGrid &was, const ColGrid &now) {
        std::vector<ColGrid> out;
        auto push = [&out, &now](int i0, int j0, int i1, int j1) {
            if (i1 <= i0 || j1 <= j0) return;
            // One column of slack on every side: the lake rule reads four
            // neighbours, so the columns just INSIDE the window beside a new
            // strip have to be re-judged now that the strip is there.
            //
            // ...AND THEN CLIPPED TO THE WINDOW, WHICH IS NOT A DETAIL. The
            // array wraps: it has exactly one slot per column of the window, so
            // the slot one column PAST the far edge is the slot of the column
            // at the NEAR edge -- which is still resident. Filling the halo
            // without this clip overwrote live ground at the opposite side of
            // the window with ground from 230 m away. It survived eight steps
            // east and failed on the first step north, because until the window
            // moves on the other axis the corruption lands in the margin.
            i0 = std::max(i0 - 1, now.i0);
            j0 = std::max(j0 - 1, now.j0);
            i1 = std::min(i1 + 1, now.i1());
            j1 = std::min(j1 + 1, now.j1());
            if (i1 <= i0 || j1 <= j0) return;
            ColGrid r;
            r.i0 = i0;
            r.j0 = j0;
            r.w = i1 - i0;
            r.h = j1 - j0;
            out.push_back(r);
        };
        // The band in x over the full height, then the band in z over what is
        // left of it -- so the corner where both moved is filled once.
        if (now.i0 < was.i0) push(now.i0, now.j0, std::min(was.i0, now.i1()), now.j1());
        if (now.i1() > was.i1()) push(std::max(was.i1(), now.i0), now.j0, now.i1(), now.j1());
        const int kx0 = std::max(now.i0, was.i0), kx1 = std::min(now.i1(), was.i1());
        if (kx1 > kx0) {
            if (now.j0 < was.j0) push(kx0, now.j0, kx1, std::min(was.j0, now.j1()));
            if (now.j1() > was.j1()) push(kx0, std::max(was.j1(), now.j0), kx1, now.j1());
        }
        return out;
    }

    // WHAT prepare() BUILT AND commit() HAS NOT TAKEN IN YET. The staged worlds
    // are whole chunks, finished and waiting; nothing in here is visible to the
    // walker or the packer until commit moves it across.
    struct Pending {
        bool valid = false;
        int cx = 0, cz = 0;
        std::vector<std::pair<int, int>> evict, add;
        std::vector<ColGrid> strips;
        std::vector<BrickWorld> staged;
        double columnsMs = 0.0, chunksMs = 0.0;
    };
    Pending pending_;

    GenOptions o_;
    const Content *c_ = nullptr;
    WorldConstants wc_;
    Columns cols_;
    BrickWorld world_;
    // chunk key -> the brick keys it owns, so eviction is an erase and not a
    // search over the whole world. See BrickWorld::eraseBricks.
    std::unordered_map<uint64_t, std::vector<uint64_t>> resident_;
    int radius_ = 4;
    int cx_ = 0, cz_ = 0;
    bool started_ = false;
    double lastMergeMs_ = 0.0, lastBuildMs_ = 0.0;
    int lastThreads_ = 0;
};

}  // namespace v4

#endif  // V4_SCENE_STREAM_H
