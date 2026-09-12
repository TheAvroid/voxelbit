// ---------------------------------------------------------------------------
// bricks.h -- the world, as a sparse set of 8^3 voxel bricks, and the packer
// that turns it into the buffers a DXR procedural-AABB store traces.
//
// NO GPU IN THIS FILE, AND NOTHING THAT NEEDS ONE. It includes materials.h and
// the shared layout header and nothing else, so the whole storage format can be
// filled, queried and packed by a plain g++ harness with no device, no Falcor
// and no shader compiler -- which is what tests/brick_test.cpp does. A store
// whose only test is "does the picture look right" is a store whose material
// indexing is wrong in a way you find three weeks later.
//
// ---------------------------------------------------------------------------
// WHY A BRICK AND NOT A VOXEL.
//
// The obvious thing to hand the hardware is one AABB per voxel. It is also
// hopeless: a 256 m square of terrain at 10 cm is 2560^2 columns, and even a
// single-voxel skin over it is 6.5 M AABBs -- a bottom-level structure of
// several gigabytes, rebuilt whenever anything changes.
//
// A brick of 8^3 divides that by 512 in the best case and by rather less in
// practice, and it moves the work to where each machine is good: the ray
// tracing cores do the part they are unbeatable at -- skipping the empty 99% of
// a wood in a few dozen nanoseconds -- and hand back a 0.8 m box, inside which
// a twenty-line DDA over a 512-bit mask finds the voxel. Neither half is doing
// the other's job.
//
// ---------------------------------------------------------------------------
// TWO KINDS OF BRICK, AND THE SECOND ONE IS MOST OF A HILLSIDE.
//
// A brick whose 512 voxels are ALL solid and all the same material needs no
// mask and no material run: the slab test against its AABB is the whole
// intersection. Underground, that is nearly everything. Keeping them apart is
// worth it twice -- 576 bytes saved per brick, and an intersection that costs
// six floating-point compares instead of a walk.
//
// ---------------------------------------------------------------------------
// AND THE ONES NOBODY CAN EVER SEE ARE NOT BUILT AT ALL.
//
// A full brick all six of whose face-neighbours are also full cannot be the
// first thing a ray from outside the ground meets: whichever face the ray comes
// in through, it crossed a solid neighbour to get there. Those bricks are
// dropped from the acceleration structure entirely -- they stay in the world,
// so an edit or a host query still finds them, they simply are not offered to
// the hardware. On a generated landscape this is most of the volume.
//
// THE ONE THING IT COSTS is a camera INSIDE the ground, which sees out through
// the hollow. This engine's camera flies and there is nothing to dig with, so
// the trade is free here; a backend that gains digging wants --no-cull-hidden
// and should say so.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "materials.h"
#include "../../shaders/stores/AabbShared.slang"

namespace v4 {

// -- the brick grid ---------------------------------------------------------
constexpr int BRICK_E = kBrickE;                     // 8 voxels
constexpr int BRICK_VOX = kBrickVox;                 // 512
constexpr int BRICK_WORDS = kBrickWords;             // 16
// How many distinct materials a brick may name with a palette. Six bits of
// V4Brick::flags hold N, so 63 is the format's ceiling; a brick past it writes
// raw bytes and sets N to 0. See the note where the run is packed.
constexpr int kMtlPaletteMax = 63;
constexpr float BRICK_M = float(BRICK_E) * VOXEL_M;  // 0.8 m

// A CHUNK IS THE ACCELERATION-STRUCTURE UNIT: one bottom-level structure, one
// instance in the top-level one. 32 bricks on a side, which is the engine's
// existing CHUNK_VOX of 256 voxels -- 25.6 m -- so anything already tuned
// against that number stays tuned. Unlike the old triangle engine's chunks
// these have a Y as well: a chunk is a cube, because a hollow under an arch is
// as worth skipping as an empty sky.
constexpr int CHUNK_BRICK = CHUNK_VOX / BRICK_E;  // 32
static_assert(CHUNK_BRICK * BRICK_E == CHUNK_VOX, "a chunk must be whole bricks");

// Brick coordinates go negative -- the world runs both ways from the origin --
// so every division by BRICK_E here is materials.h's floorDiv/floorMod and
// never C's truncating pair, which folds the two sides of zero onto one brick.

// 21 bits a side, signed: +/- 1,048,576 bricks, which is +/- 838 km. Packed
// rather than hashed as a tuple because this key is looked up several times per
// voxel during generation, and an unordered_map of a 64-bit integer is the
// cheapest thing that can answer.
inline uint64_t brickKey(int bx, int by, int bz) {
    return (uint64_t(uint32_t(bx) & 0x1FFFFFu) << 42) |
           (uint64_t(uint32_t(by) & 0x1FFFFFu) << 21) |
           (uint64_t(uint32_t(bz) & 0x1FFFFFu));
}

// The inverse. Sign-extends a 21-bit field: shifting up to the top of an int32
// and arithmetic-shifting back is the one spelling of it that does not depend
// on whether int is 32 bits or on how the compiler feels about bitfields.
inline int keyAxis(uint64_t k, int shift) {
    return int(int32_t(uint32_t((k >> shift) & 0x1FFFFFu) << 11) >> 11);
}
inline int keyX(uint64_t k) { return keyAxis(k, 42); }
inline int keyY(uint64_t k) { return keyAxis(k, 21); }
inline int keyZ(uint64_t k) { return keyAxis(k, 0); }

// ---------------------------------------------------------------------------
// One brick. 580 bytes, POD, no heap of its own.
//
// THE MATERIAL ARRAY IS DENSE HERE AND COMPACTED ONLY ON THE WAY TO THE DEVICE.
// A dense 512-byte array makes set() a store and at() a load, with no prefix
// sum and no reshuffling when a voxel is added -- which matters because
// generation writes every voxel exactly once but in no particular order. The
// packer pays the compaction, once, for the whole world.
// ---------------------------------------------------------------------------
struct Brick {
    uint32_t mask[BRICK_WORDS] = {};
    uint8_t mtl[BRICK_VOX] = {};
    uint16_t count = 0;  // set bits in mask, maintained by set()

    // -- HOW MANY OF THOSE ARE NOT OPAQUE, which so far means water ---------
    //
    // Kept for one caller: the whole-brick cull in packStore. "Full" is a
    // count of BITS and says nothing about what a ray does when it arrives, so
    // a brick of solid water walled in by solid water passed a test written
    // for rock and was dropped before the peel -- which never drops water --
    // could object. Every lake deeper than a brick lost its middle, and the
    // surface and the bed survived, so it read as a rendering bug and not a
    // missing brick. Scanning 512 material bytes per neighbour to answer it
    // would cost more than the cull saves; a counter costs the writer a
    // branch it was already paying for `count`.
    uint16_t clear = 0;

    static int bitOf(int lx, int ly, int lz) {
        return lx | (ly << kBrickShift) | (lz << (2 * kBrickShift));
    }
    bool bit(int b) const { return ((mask[b >> 5] >> (b & 31)) & 1u) != 0u; }
    bool full() const { return count == BRICK_VOX; }
    // Solid to its own edges AND opaque the whole way through: the only brick
    // that can seal a face laid against it without anything being looked at.
    bool sealed() const { return count == BRICK_VOX && clear == 0; }
};

// ---------------------------------------------------------------------------
// WHAT A BODY CAN WALK THROUGH.
//
// A ray hits grass; a person does not. The strands are two to five voxels tall
// and stand on more than half the ground, so a walker that collides with them
// climbs a step every time it enters a tuft and drops one leaving -- the gait
// stutters continuously over open meadow, and standing ON the tips puts the eye
// a quarter of a metre above the soil it is supposedly on.
//
// So the WALKER's idea of solid is not the STORE's. Nothing about the geometry
// changes: the blades are still voxels, still traced, still shade and still cast
// shadows. Only the host-side collision queries ask this instead.
//
// WATER IS NOT LISTED HERE ON PURPOSE. Wading is a swimming decision and not a
// collision one, and making a lake walk-through silently would put a body on
// the bed of it with its head under the surface.
// ---------------------------------------------------------------------------
inline bool walkSolid(uint8_t m) { return m != mat::AIR && !isGrass(m); }

// ---------------------------------------------------------------------------
// THE PART OF THE WORLD THAT IS A FORMULA RATHER THAN A BRICK.
//
// Under every column of terrain there are five hundred voxels of stone and ten
// of bedrock, and essentially none of them can ever be seen: the packer drops
// every voxel with six opaque neighbours, so all that survives is the skin.
// Writing them anyway cost 1.9 BILLION calls to set(), 2.2 GB of host memory
// and thirteen seconds of launch, to produce buffers identical to the ones you
// get without them.
//
// So the generator writes only the skin -- the band that pokes above its lowest
// neighbour, and the band that hangs below its highest neighbour's floor -- and
// hands over this, which answers for everything in between. It is a heightfield
// and a layer stack: three integers and a lookup.
//
// IT IS CONSULTED WHEREVER THE BRICKS SAY AIR, not only where a brick is
// missing, because the skin bands leave half-filled bricks behind them.
//
// WHICH MEANS IT CANNOT BE DUG. Clearing a voxel inside the implicit region
// does nothing -- the formula fills it straight back in. Nothing in this engine
// digs, and the day something does, the column it digs into has to be
// materialised into real bricks first. That is the whole price of this, and it
// is written here rather than discovered later.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// THE RECTANGLE OF COLUMNS A BUILD COVERS.
//
// The generator used to be written against the world: `half` and `side` were
// the extent, every array was side*side, and every index was (j + half) * side
// + (i + half). That is exactly one assumption -- that the thing being built is
// centred on the origin and is the whole world -- and it is the assumption an
// endless world cannot make.
//
// So the rectangle is named. It is still the whole extent for a one-shot build;
// it is a chunk plus its margin for a streamed one, and NOTHING ELSE IN THE
// GENERATOR HAS TO KNOW WHICH. Column coordinates stay absolute world voxels
// throughout -- the grid only translates them into the working arrays -- so a
// chunk's noise, its scatter hashes and its heights are identical to what the
// whole-world build would have put there, which is the property the streaming
// seams are verified against.
// ---------------------------------------------------------------------------
struct ColGrid {
    int i0 = 0, j0 = 0;  // inclusive, in world voxels
    int w = 0, h = 0;    // columns

    // -- WRAPPING, AND IT IS WHAT MAKES A WINDOW AFFORDABLE TO MOVE ---------
    //
    // A resident window is a rectangle that follows the player, and almost all
    // of it is the same ground it was a moment ago. Indexed from the corner,
    // moving it by one column moves EVERY column's slot and the whole window
    // has to be recomputed -- 3.7 M columns, about a second, every time you
    // cross a boundary.
    //
    // Wrapped, a column's slot is a function of the column alone -- i mod w --
    // so it never moves. Sliding the window changes only which columns are
    // CURRENT: the strip that left frees exactly the slots the strip that
    // entered needs, and the recompute is the strip rather than the window.
    //
    // `inside` is then the whole of the bookkeeping, and it is unchanged: it
    // still asks whether a column is in the rectangle, because two columns w
    // apart share a slot and only one of them is real.
    bool wrap = false;

    int i1() const { return i0 + w; }
    int j1() const { return j0 + h; }
    bool inside(int i, int j) const { return i >= i0 && j >= j0 && i < i1() && j < j1(); }
    size_t idx(int i, int j) const {
        if (!wrap) return size_t(j - j0) * size_t(w) + size_t(i - i0);
        return size_t(floorMod(j, h)) * size_t(w) + size_t(floorMod(i, w));
    }
    size_t count() const { return size_t(w) * size_t(h); }
};

struct ImplicitColumns {
    // THE RECTANGLE, AND IT IS NOT ASSUMED TO BE CENTRED ON THE ORIGIN.
    //
    // It was `half` and `side` -- a square around (0,0), which is what a world
    // built once in one piece is. A resident window around a player is not: it
    // is a rectangle that MOVES, and the formula under the skin has to follow
    // it. See ColGrid.
    std::vector<int16_t> height;  // g.w * g.h, row-major
    ColGrid g;
    int column = 0;      // solid voxels under each surface, surface included
    int soilVox = 0;     // of which the top layer, under the one-voxel surface
    int bedrockVox = 0;  // ...and the bottom layer
    uint8_t soil = 0, stone = 0, bedrock = 0;

    // WHAT THE TOP LAYER OF EACH COLUMN IS MADE OF, where it is not soil.
    //
    // THE FORMULA HAS TO KNOW THIS, and it is not cosmetic: a beach is
    // FLATTENED, so its skin band is one or two voxels and the rest of its sand
    // falls in the part only the formula describes. A formula that answered
    // "soil" there disagreed with the explicit fill about what the brick was
    // made of, which turned a uniform brick into a masked one and made the two
    // produce different buffers. 0 soil, 1 sand, 2 rock.
    std::vector<uint8_t> surf;
    uint8_t sand = 0, rock = 0;

    uint8_t topLayer(int i, int j) const {
        if (surf.empty() || !g.inside(i, j)) return soil;
        const uint8_t k = surf[g.idx(i, j)];
        return k == 1u ? sand : (k == 2u ? rock : soil);
    }

    bool valid() const { return g.w > 0 && !height.empty(); }

    // The surface of this column, or kNoTop where there is no terrain.
    int topAt(int i, int j) const {
        if (!g.inside(i, j)) return -1000000;
        return int(height[g.idx(i, j)]);
    }

    uint8_t at(int i, int y, int j) const {
        const int h = topAt(i, j);
        if (h == -1000000) return 0;
        const int k = h - y;  // depth below the surface
        if (k < 0 || k >= column) return 0;
        if (k <= soilVox) return topLayer(i, j);
        return (k >= column - bedrockVox) ? bedrock : stone;
    }
};

// ---------------------------------------------------------------------------
// The world.
// ---------------------------------------------------------------------------
class BrickWorld {
  public:
    // -----------------------------------------------------------------------
    // A CURSOR, AND IT IS THE DIFFERENCE BETWEEN A GENERATOR THAT TAKES A
    // SECOND AND ONE THAT TAKES TEN.
    //
    // Filling a landscape writes voxels in COLUMNS: the same (i, j) with y
    // running down, so eight consecutive writes land in the same brick and the
    // ninth moves to its neighbour. Looking the brick up in the hash map every
    // time pays a hash, a bucket walk and a cache miss for each of those eight.
    //
    // The cursor remembers the last brick it touched. It is a value the caller
    // holds rather than state on the world, because two threads filling two
    // regions must not share one -- and because a stale cursor is then
    // impossible to have: it is scoped to the loop that made it, and every
    // entry point through it re-checks the key.
    // -----------------------------------------------------------------------
    struct Cursor {
        uint64_t key = ~uint64_t(0);
        Brick *b = nullptr;
    };

    // -- writing ------------------------------------------------------------
    //
    // Setting a voxel to AIR clears it; the brick is kept even when it empties,
    // because a brick that has held something once usually holds something
    // again, and the packer skips empty ones anyway.
    void set(int i, int y, int j, uint8_t m) {
        Cursor c;
        set(c, i, y, j, m);
    }

    void set(Cursor &c, int i, int y, int j, uint8_t m) {
        const int bx = floorDiv(i, BRICK_E), by = floorDiv(y, BRICK_E), bz = floorDiv(j, BRICK_E);
        const uint64_t k = brickKey(bx, by, bz);
        if (k != c.key || c.b == nullptr) {
            auto it = bricks_.find(k);
            if (it == bricks_.end()) {
                if (m == mat::AIR) return;  // nothing to record
                it = bricks_.emplace(k, Brick{}).first;
                note(bx, by, bz);
            }
            // SAFE ACROSS THE INSERT ABOVE. std::unordered_map guarantees that
            // references to its elements survive a rehash -- only iterators are
            // invalidated -- so a pointer kept here stays good until the brick
            // itself is erased, and nothing erases bricks.
            c.key = k;
            c.b = &it->second;
        }
        Brick &b = *c.b;
        const int bit =
            Brick::bitOf(floorMod(i, BRICK_E), floorMod(y, BRICK_E), floorMod(j, BRICK_E));
        const bool was = b.bit(bit);
        const uint8_t prev = was ? b.mtl[bit] : uint8_t(mat::AIR);
        if (m != mat::AIR) {
            b.mask[bit >> 5] |= (1u << (bit & 31));
            b.mtl[bit] = m;
            if (!was) {
                ++b.count;
                ++voxels_;
            }
            // Brick::clear -- and it has to survive an OVERWRITE, not just a
            // first write: the generator pours water over a column and the
            // decorators write over it again.
            if (prev == mat::WATER && m != mat::WATER) --b.clear;
            else if (prev != mat::WATER && m == mat::WATER) ++b.clear;
        } else if (was) {
            b.mask[bit >> 5] &= ~(1u << (bit & 31));
            b.mtl[bit] = 0;
            --b.count;
            --voxels_;
            if (prev == mat::WATER) --b.clear;
        }
    }

    // -- reading, and this is the SECOND INTERFACE gpu/world.h warns about ---
    //
    // The shader seam answers rays. Placing anything on the ground, settling a
    // spawn, or a walker's collision test are HOST questions, and a store that
    // cannot answer them is a store that only half works.
    uint8_t at(int i, int y, int j) const {
        auto it = bricks_.find(brickKey(floorDiv(i, BRICK_E), floorDiv(y, BRICK_E), floorDiv(j, BRICK_E)));
        if (it != bricks_.end()) {
            const int bit =
                Brick::bitOf(floorMod(i, BRICK_E), floorMod(y, BRICK_E), floorMod(j, BRICK_E));
            if (it->second.bit(bit)) return it->second.mtl[bit];
        }
        // A BRICK THAT EXISTS IS NOT THE WHOLE ANSWER. The skin bands leave
        // half-filled bricks straddling the boundary, so an unset bit still has
        // to ask the formula -- see ImplicitColumns.
        return implicit_.valid() ? implicit_.at(i, y, j) : mat::AIR;
    }

    bool solidAt(int i, int y, int j) const { return at(i, y, j) != mat::AIR; }

    // The same question a walker asks: solid to a BODY, which grass is not.
    bool walkSolidAt(int i, int y, int j) const { return walkSolid(at(i, y, j)); }

    // topAt, skipping anything a body walks through -- so the ground under a
    // meadow is the soil, not the tips of the blades standing in it.
    int topWalkAt(int i, int j, int fromY) const {
        int y = topAt(i, j, fromY);
        // Grass is a handful of voxels deep at most, so this walks down a few
        // and stops. Bounded anyway: a column of nothing but grass over a void
        // would otherwise scan to the bottom of the world.
        for (int guard = 0; guard < 16 && y != kNoTop; ++guard) {
            if (walkSolid(at(i, y, j))) return y;
            y = topAt(i, j, y - 1);
        }
        return y;
    }

    // The highest filled voxel in this column. Scans DOWNWARD OVER BRICKS, not
    // voxels: an empty brick is one hash miss and skips eight voxels of sky at
    // a time, and the scan is bounded by the world's own extent rather than by
    // a guessed ceiling.
    static constexpr int kNoTop = -1000000;

    // -----------------------------------------------------------------------
    // THE HIGHEST FILLED VOXEL AT OR BELOW `fromY`, AND THE BOUND IS THE POINT.
    //
    // In a volume "the ground" is not a property of the column, it is a
    // property of where you are IN the column. Searching from the top of the
    // world tells a body standing under an arch that the ground is the arch
    // above it, and a body in a cave that the ground is the hillside over its
    // head. A walker asks from its own feet and gets the surface it is actually
    // standing on.
    //
    // The unbounded form below is the one for placing things from the sky, and
    // it is that same call with the ceiling taken off.
    // -----------------------------------------------------------------------
    int topAt(int i, int j, int fromY) const {
        if (bricks_.empty()) return kNoTop;
        const int bx = floorDiv(i, BRICK_E), bz = floorDiv(j, BRICK_E);
        const int lx = floorMod(i, BRICK_E), lz = floorMod(j, BRICK_E);
        // THE PARTIAL LIMIT APPLIES TO THE BRICK THAT HOLDS fromY, NOT TO
        // WHICHEVER BRICK THE SEARCH HAPPENS TO START IN.
        //
        // They are the same brick only when fromY is inside the world. Ask from
        // ABOVE the world -- which is exactly what placing something from the
        // sky does -- and the start is clamped down to the top brick while
        // fromY's offset still says "look no higher than row 0 of it", so the
        // search skips seven eighths of the brick it starts in and reports a
        // surface most of a metre too low.
        const int fromBy = floorDiv(fromY, BRICK_E);
        const int startBy = std::min(maxBy_, fromBy);

        // -- THE FORMULA FIRST, AND IT IS A FLOOR UNDER THE SCAN ------------
        //
        // THE EXPLICIT SCAN USED TO RUN TO minBy_ AND ONLY THEN ASK THE
        // FORMULA, AND A BODY FELL THROUGH THE WORLD.
        //
        // The generator writes only the SKIN -- one voxel on flat ground --
        // and hands the other five hundred to ImplicitColumns. So a query from
        // inside the hillside finds no written voxel where the body actually
        // is, walks down through half a kilometre of implicit rock finding
        // nothing, and reaches the BOTTOM skin band, which is written. It then
        // answers "the ground here is the bedrock, fifty-two metres down", and
        // that is a true statement about written voxels and a lie about the
        // world.
        //
        // Dropping out of fly mode is how you meet it: from fifteen metres up a
        // falling body is still inside the one written voxel of skin when it is
        // first tested and lands; from forty it is travelling 0.7 m a frame,
        // clears the skin in a single step, and is told the floor is at the
        // bottom of the world. It falls all the way there. [[nothing-floats]]
        // in reverse, and it cost a session to find because every query ABOVE
        // ground is answered correctly by the same code.
        //
        // The formula's answer is O(1) and it is the HIGHEST solid at or below
        // fromY wherever the skin is silent -- so it is computed first and used
        // as the floor the scan stops at. Nothing below it can win, the scan
        // gets shorter rather than longer, and where both have something the
        // higher of the two is returned.
        const int im = implicitTop(i, j, fromY);
        const int stopBy = (im == kNoTop) ? minBy_ : std::max(minBy_, floorDiv(im, BRICK_E));
        for (int by = startBy; by >= stopBy; --by) {
            auto it = bricks_.find(brickKey(bx, by, bz));
            if (it == bricks_.end()) continue;
            const Brick &b = it->second;
            const int hiLy = (by == fromBy) ? std::min(BRICK_E - 1, floorMod(fromY, BRICK_E))
                                            : BRICK_E - 1;
            for (int ly = hiLy; ly >= 0; --ly)
                if (b.bit(Brick::bitOf(lx, ly, lz))) {
                    // Within the brick the formula's answer lives in, an
                    // explicit voxel can still be BELOW it -- the bottom of a
                    // skin band straddling the boundary. Take the higher.
                    const int y = by * BRICK_E + ly;
                    return (im != kNoTop && im > y) ? im : y;
                }
        }
        return im;
    }

    // The highest solid voxel at or below fromY that the FORMULA accounts for.
    // The explicit scan runs first and wins; this is what answers where the
    // skin bands leave off.
    int implicitTop(int i, int j, int fromY) const {
        if (!implicit_.valid()) return kNoTop;
        const int h = implicit_.topAt(i, j);
        if (h == kNoTop) return kNoTop;
        const int y = std::min(h, fromY);
        if (y < h - implicit_.column + 1) return kNoTop;
        return y;
    }

    int topAt(int i, int j) const {
        if (bricks_.empty()) return implicit_.valid() ? implicit_.topAt(i, j) : kNoTop;
        const int bx = floorDiv(i, BRICK_E), bz = floorDiv(j, BRICK_E);
        const int lx = floorMod(i, BRICK_E), lz = floorMod(j, BRICK_E);
        for (int by = maxBy_; by >= minBy_; --by) {
            auto it = bricks_.find(brickKey(bx, by, bz));
            if (it == bricks_.end()) continue;
            const Brick &b = it->second;
            for (int ly = BRICK_E - 1; ly >= 0; --ly)
                if (b.bit(Brick::bitOf(lx, ly, lz))) return by * BRICK_E + ly;
        }
        return implicit_.valid() ? implicit_.topAt(i, j) : kNoTop;
    }

    // The same answer as a world height, which is what placement wants. The TOP
    // face of that voxel, not its bottom: something standing here rests on it.
    float groundM(int i, int j) const {
        const int t = topAt(i, j);
        return (t == kNoTop) ? 0.0f : float(t + 1) * VOXEL_M;
    }
    // ABOVE EVERYTHING. The world's own ceiling, in metres -- what a search
    // that wants "the highest ground anywhere in this column" starts from, as
    // against a walker's bounded search which starts from its own feet.
    float skyM() const {
        return bricks_.empty() ? 0.0f : float((maxBy_ + 1) * BRICK_E) * VOXEL_M;
    }

    float groundM(float x, float z) const {
        return groundM(int(std::floor(x / VOXEL_M)), int(std::floor(z / VOXEL_M)));
    }

    // -- what is in here ----------------------------------------------------
    const std::unordered_map<uint64_t, Brick> &bricks() const { return bricks_; }
    uint64_t voxelCount() const { return voxels_; }
    size_t brickCount() const { return bricks_.size(); }
    int minBrickY() const { return minBy_; }
    int maxBrickY() const { return maxBy_; }
    void reserve(size_t n) { bricks_.reserve(n); }
    void clear() {
        bricks_.clear();
        implicit_ = ImplicitColumns{};
        waterM_ = -1e9f;
        voxels_ = 0;
        any_ = false;
    }

    // THE FORMULA UNDER THE SKIN. Handed over once, after the height field is
    // built and before anything queries the world. See ImplicitColumns.
    void setImplicit(ImplicitColumns im) { implicit_ = std::move(im); }
    const ImplicitColumns &implicit() const { return implicit_; }

    // -- THE PUBLISHED COPY, WRITEABLE, FOR ONE CALLER ----------------------
    //
    // THE COPY IS WHAT MAKES THE THREADING SAFE. A streamed world keeps filling
    // its window's columns on a worker while the frame is drawing, so what the
    // tracer and the walker read has to be a snapshot nobody is writing to --
    // which is why setImplicit takes a value and not a pointer.
    //
    // But a snapshot of the whole window is 130 MB at the view distance v2 uses
    // -- 43 million columns -- and rebuilding it on every crossing was most of
    // what the render thread spent committing one. The streamer updates the
    // strips that actually moved instead; see StreamWorld::publishStrips. It is
    // the only caller, and it is on the render thread by construction.
    ImplicitColumns &implicitMut() { return implicit_; }

    // -- THE WATERLINE, IN METRES, and the one thing a voxel lookup cannot --
    //
    // Water is a material on the grid, so "is this voxel water" is answered per
    // voxel and no global line is needed to draw a lake. But the tracer has one
    // question that is about a point in EMPTY AIR: did the eye start this frame
    // under the surface? A path only ever learns it is in water by CROSSING
    // into it, so a camera that begins submerged got no absorption at all --
    // swimming looked like standing in air until you surfaced and dived again.
    //
    // Far below the world where the generator laid no water, so the comparison
    // is false everywhere it should be.
    float waterM() const { return waterM_; }
    void setWaterM(float m) { waterM_ = m; }

    // Does the brick here seal a face laid against it? See Brick::sealed --
    // "full of bits" is a different question and answering this one with it
    // deleted the inside of every lake.
    // -- TAKE ANOTHER WORLD'S BRICKS INTO THIS ONE --------------------------
    //
    // How a chunk built on a worker thread joins the resident world. It is a
    // MOVE of hash-map nodes, not a re-write of voxels: the staging world is
    // finished with afterwards.
    //
    // THE CHUNKS MUST NOT OVERLAP, and by construction they cannot. A chunk's
    // rectangle is a whole number of bricks and writing is clipped to it (see
    // ClipWriter), so no two chunks ever touch the same brick and a key
    // collision here would mean one of those two invariants had broken. The
    // assert is the cheap way to find out which.
    // `took`, when given, collects the keys this world now owns from `other` --
    // which is what lets the chunk be evicted later without a search.
    void absorb(BrickWorld &&other, std::vector<uint64_t> *took = nullptr) {
        if (took) took->reserve(took->size() + other.bricks_.size());
        // -- SPLICED, NOT COPIED ---------------------------------------------
        //
        // merge() RELINKS the source's nodes into this map: no allocation, and
        // no move of the 580-byte brick that emplace would have done for every
        // one of them. On a 25-chunk crossing that is 137,000 bricks, and it is
        // the difference between a 100 ms commit on the render thread and a
        // 30 ms one.
        //
        // It leaves behind anything whose key is already here -- which by
        // construction is nothing, because a chunk's rectangle is a whole
        // number of bricks and writing is clipped to it, so no two chunks ever
        // touch the same brick. The loop below is the proof rather than the
        // plan: it handles the collision correctly if the invariant is ever
        // broken, and normally runs zero times.
        for (auto &kv : other.bricks_) {
            if (took) took->push_back(kv.first);
            note(keyX(kv.first), keyY(kv.first), keyZ(kv.first));
            voxels_ += uint64_t(kv.second.count);
        }
        bricks_.merge(other.bricks_);
        for (auto &kv : other.bricks_) {
            // Only collisions reach here; their counts were added above and
            // have to come back off before the merge below re-adds them.
            voxels_ -= uint64_t(kv.second.count);
            auto it = bricks_.find(kv.first);
            if (it == bricks_.end()) continue;
            // Two chunks wrote the same brick: merge rather than lose voxels,
            // and it is a bug upstream if it ever happens.
            Brick &dst = it->second;
            for (int b = 0; b < BRICK_VOX; ++b) {
                if (!kv.second.bit(b)) continue;
                if (!dst.bit(b)) { ++dst.count; ++voxels_; }
                else if (dst.mtl[b] == mat::WATER) --dst.clear;
                dst.mask[b >> 5] |= (1u << (b & 31));
                dst.mtl[b] = kv.second.mtl[b];
                if (kv.second.mtl[b] == mat::WATER) ++dst.clear;
            }
        }
        other.bricks_.clear();
        other.voxels_ = 0;
    }

    // -- DROP EXACTLY THESE BRICKS ------------------------------------------
    //
    // How a chunk leaves the resident window. The caller knows precisely which
    // bricks are its own -- it built them into a world of its own before they
    // were absorbed -- so eviction is that many erases and not a search.
    //
    // THE RECTANGLE FORM BELOW IS O(THE WHOLE WORLD) and was 340 ms of a 560 ms
    // window crossing: it walked half a million bricks once for each of nine
    // departing chunks, to find the six thousand that were theirs. Keeping the
    // keys costs 50 KB a chunk.
    void eraseBricks(const std::vector<uint64_t> &keys) {
        for (uint64_t k : keys) {
            auto it = bricks_.find(k);
            if (it == bricks_.end()) continue;
            voxels_ -= uint64_t(it->second.count);
            bricks_.erase(it);
        }
    }

    // -- DROP EVERY BRICK WHOSE COLUMN IS IN THIS RECTANGLE -----------------
    //
    // How a chunk leaves the resident window. It is exact because writing is
    // exact: ClipWriter gives every voxel to the one chunk whose rectangle
    // contains its column, so a chunk's bricks are entirely its own and nothing
    // else has written into them. Erasing by COLUMN and not by brick key is
    // what keeps that true when a chunk's rectangle is not brick-aligned.
    //
    // voxels_ is corrected as it goes, because it is what the store reports and
    // a count that only ever grows would make a window that had run for an hour
    // look like a world that never freed anything.
    void eraseColumns(const ColGrid &r) {
        for (auto it = bricks_.begin(); it != bricks_.end();) {
            const int bx = keyX(it->first), bz = keyZ(it->first);
            // A brick is dropped only if EVERY column it covers is in the
            // rectangle -- a brick straddling the edge belongs to whichever
            // chunk owns its columns, and dropping it would take voxels that
            // are still resident with it.
            const int i0 = bx * BRICK_E, j0 = bz * BRICK_E;
            const bool mine = r.inside(i0, j0) && r.inside(i0 + BRICK_E - 1, j0 + BRICK_E - 1);
            if (!mine) { ++it; continue; }
            voxels_ -= uint64_t(it->second.count);
            it = bricks_.erase(it);
        }
    }

    bool sealedBrickAt(int bx, int by, int bz) const {
        auto it = bricks_.find(brickKey(bx, by, bz));
        return it != bricks_.end() && it->second.sealed();
    }

  private:
    void note(int bx, int by, int bz) {
        if (!any_) {
            minBx_ = maxBx_ = bx;
            minBy_ = maxBy_ = by;
            minBz_ = maxBz_ = bz;
            any_ = true;
            return;
        }
        minBx_ = std::min(minBx_, bx);
        maxBx_ = std::max(maxBx_, bx);
        minBy_ = std::min(minBy_, by);
        maxBy_ = std::max(maxBy_, by);
        minBz_ = std::min(minBz_, bz);
        maxBz_ = std::max(maxBz_, bz);
    }

    ImplicitColumns implicit_;
    float waterM_ = -1e9f;
    std::unordered_map<uint64_t, Brick> bricks_;
    uint64_t voxels_ = 0;
    bool any_ = false;
    int minBx_ = 0, maxBx_ = 0, minBy_ = 0, maxBy_ = 0, minBz_ = 0, maxBz_ = 0;
};

// ---------------------------------------------------------------------------
// THE PACKED FORM -- exactly the bytes the device gets, and nothing else.
//
// Produced in one pass over the world and handed to gpu/store.h, which uploads
// it and builds the structures. Keeping it a plain value type is what lets the
// test harness check the encoding against the world it came from without ever
// touching a device.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ONE CHUNK'S SHARE OF THE DEVICE BUFFERS.
//
// The unit the pack is built and CACHED in. A chunk's bytes depend on its own
// bricks and -- through the peel, which asks a voxel's six neighbours -- on the
// bricks immediately around it, and on nothing else in the world. So a window
// that slides can keep the chunks that did not move and re-emit only the ones
// that did, which is the difference between 5 seconds a crossing and half of
// one at the view distance v2 uses.
//
// maskBase and mtlBase inside `brick` are relative to THIS chunk's mask vector
// until it is concatenated; see the join in packStore for the two headers where
// those fields are not offsets at all.
// ---------------------------------------------------------------------------
struct PackedChunk {
    std::vector<V4Brick> brick;
    std::vector<float> aabb;
    std::vector<uint32_t> mask;
    uint64_t boxVox = 0, uniform = 0, merged = 0, detail = 0, mono = 0;

    // ONE OF THIS CHUNK'S BRICK KEYS, and it answers "is this chunk still in
    // the world" in a single lookup. The alternative was building the set of
    // every chunk the world holds, which means hashing all 3.4 million bricks
    // on every pack to find out something about two thousand chunks.
    //
    // A probe is enough because chunks arrive and leave WHOLE -- a chunk's
    // voxels are written by it alone and erased by it alone; see ClipWriter.
    uint64_t probe = 0;

    size_t bytes() const {
        return brick.size() * sizeof(V4Brick) + aabb.size() * sizeof(float) +
               mask.size() * sizeof(uint32_t);
    }
};

// The chunks a previous pack produced, so the next one can keep most of them.
// Handed to packStore along with the set of chunks whose bricks have changed;
// empty means "rebuild everything", which is what a world built once wants.
// ---------------------------------------------------------------------------
// A RANGE ALLOCATOR, AND IT IS WHAT STOPS THE DEVICE REBUILDING EVERYTHING.
//
// Each chunk owns a contiguous run of bricks -- it has to, because the shader's
// brick index is `CandidateInstanceID() + CandidatePrimitiveIndex()` and the
// instance carries the run's start. Lay the chunks out end to end and one chunk
// changing size shifts every chunk after it, so every bottom-level structure
// has to be rebuilt: 281 ms of a 348 ms crossing, for 250 chunks that moved out
// of 2,105.
//
// Given a STABLE address instead, an untouched chunk's structure stays valid
// and is never rebuilt, and its bytes are never re-uploaded. So a chunk keeps
// its slot for as long as it fits, and slots are recycled as the window slides
// -- which it does in a steady rhythm, the same number arriving as leaving.
//
// CAPACITY IS ROUNDED UP so that a chunk whose contents change slightly -- a
// tree grew, a boulder came into range -- keeps the slot it has instead of
// freeing and reallocating, which is what would fragment this.
// ---------------------------------------------------------------------------
struct SlotAlloc {
    struct Range {
        uint32_t first = 0, cap = 0;
    };
    std::vector<Range> free;
    uint32_t top = 0;      // high-water mark; nothing above it has been used
    uint32_t granule = 1;  // capacities are a multiple of this

    void reset(uint32_t g) {
        free.clear();
        top = 0;
        granule = g < 1u ? 1u : g;
    }

    uint32_t roundUp(uint32_t n) const { return ((n + granule - 1u) / granule) * granule; }

    Range take(uint32_t need) {
        const uint32_t cap = roundUp(need < 1u ? 1u : need);
        // FIRST FIT OVER THE SMALLEST THAT WORKS -- best fit, really, and it is
        // worth the scan: the free list is short (the chunks that left on the
        // last crossing) and a bad fit here is permanent fragmentation.
        size_t best = free.size();
        for (size_t i = 0; i < free.size(); ++i)
            if (free[i].cap >= cap && (best == free.size() || free[i].cap < free[best].cap))
                best = i;
        if (best < free.size()) {
            Range r = free[best];
            free.erase(free.begin() + long(best));
            // Hand back what is not needed, so one huge freed slot does not
            // swallow a small chunk for the rest of the session.
            if (r.cap > cap + granule) {
                Range rest{r.first + cap, r.cap - cap};
                r.cap = cap;
                give(rest);
            }
            return r;
        }
        Range r{top, cap};
        top += cap;
        return r;
    }

    void give(Range r) {
        if (r.cap == 0) return;
        // Coalesce with a neighbour if there is one, so a window that has slid
        // for an hour does not end up with a free list of ten thousand crumbs.
        for (Range &f : free) {
            if (f.first + f.cap == r.first) {
                f.cap += r.cap;
                return;
            }
            if (r.first + r.cap == f.first) {
                f.first = r.first;
                f.cap += r.cap;
                return;
            }
        }
        free.push_back(r);
    }
};

struct PackCache {
    std::unordered_map<uint64_t, PackedChunk> chunk;

    // Where each chunk's bricks (and therefore its AABBs, one per brick) and
    // its mask words live in the device buffers, and for how long they fit.
    std::unordered_map<uint64_t, SlotAlloc::Range> brickSlot, maskSlot;
    SlotAlloc bricks, masks;

    void clear() {
        chunk.clear();
        brickSlot.clear();
        maskSlot.clear();
        bricks.reset(64);
        masks.reset(1024);
    }
    size_t bytes() const {
        size_t n = 0;
        for (const auto &kv : chunk) n += kv.second.bytes();
        return n;
    }
};

struct PackedStore {
    // One bottom-level structure's worth: a contiguous run of bricks, and the
    // same run of AABBs. The instance's InstanceID is set to `first`, so the
    // shader's brick index is CandidateInstanceID() + CandidatePrimitiveIndex()
    // with no indirection at all.
    struct Chunk {
        int cx = 0, cy = 0, cz = 0;
        uint32_t first = 0;  // index of the chunk's first brick in `brick`
        uint32_t count = 0;  // how many it has
    };

    std::vector<float> aabb;     // 6 floats per brick, WORLD METRES
    std::vector<V4Brick> brick;  // one header per brick, parallel to `aabb`
    // ONE BUFFER FOR BOTH. Per non-uniform brick: its 16 mask words, then its
    // material run packed four bytes to a word, immediately after. Keeping them
    // in separate buffers cost a cache miss on every hit -- see packStore.
    std::vector<uint32_t> mask;
    std::vector<Chunk> chunk;

    // Diagnostics, for the banner and the overlay.
    uint64_t voxels = 0;   // in the world
    uint64_t kept = 0;     // ...of which reachable, and therefore uploaded
    uint64_t uniform = 0;  // bricks that needed no mask
    uint64_t detail = 0;   // bricks that did
    uint64_t hidden = 0;   // bricks dropped because nothing can reach them
    uint64_t merged = 0;   // uniform bricks absorbed into a larger box
    uint64_t mono = 0;     // detail bricks whose kept voxels are all ONE material
    // How much of the brick volume the AABBs actually cover, 0..1. One is the
    // old behaviour -- every box the full 8^3 -- and the lower it goes the less
    // empty space the hardware is being asked to consider.
    double fill = 1.0;

    // WHERE THE PACK'S TIME WENT. Two numbers, because they have completely
    // different fixes: the peel is per brick and parallel, and the assembly is
    // one thread walking every surviving brick to lay out the buffers. A
    // streamed world re-packs whenever its window moves, so which of these
    // dominates decides whether the answer is caching the peel or not
    // rebuilding the buffers at all.
    double peelMs = 0.0, assembleMs = 0.0;

    // -- WHAT ACTUALLY CHANGED, so the device can be told only that ----------
    //
    // Filled when the pack was given a cache: the brick ranges whose contents
    // are new (their AABBs are the same ranges, six floats each), the mask word
    // ranges, and the indices into `chunk` whose bottom-level structure has to
    // be rebuilt. Empty with no cache, which means "all of it".
    struct Span {
        uint32_t first = 0, count = 0;
    };
    std::vector<Span> dirtyBricks, dirtyMask;
    std::vector<uint32_t> dirtyChunks;
    bool incremental = false;

    uint64_t bytes() const {
        return uint64_t(aabb.size() * sizeof(float) + brick.size() * sizeof(V4Brick) +
                        mask.size() * sizeof(uint32_t));
    }
};

struct PackOptions {
    // -----------------------------------------------------------------------
    // PEEL THE VOXELS NOTHING CAN REACH.
    //
    // A voxel with six opaque face-neighbours can never be the first thing a
    // ray from outside the solid meets: whichever face the ray comes in
    // through, it crossed a neighbour to get there. So it is not uploaded --
    // no mask bit, no material byte. On this world that is most of them: the
    // terrain shell is 24 voxels deep and only the top one or two are ever
    // seen, a boulder is solid all the way through, and a pine's trunk is a
    // filled cylinder.
    //
    // IT IS THE SAME RULE THE BRICK CULL USES, ONE LEVEL DOWN, and it subsumes
    // it: a brick with nothing left after the peel is dropped outright, which
    // catches the 95 %-full brick under a slope that the whole-brick test
    // misses. What it does NOT do is hollow anything out from the outside --
    // every voxel with even one air or water face survives, so the solid keeps
    // a complete skin and a ray from any direction still stops on it.
    //
    // WATER IS NOT OPAQUE, and that matters. The tracer refracts through it, so
    // the lake bed is visible from above and its voxels are NOT buried.
    //
    // Off (--no-peel) uploads the world as written, which is what a backend
    // that can put the camera inside solid ground would want.
    bool peel = true;

    // Fit each AABB to the voxels the brick actually keeps, instead of handing
    // the hardware the whole 8^3 box. See the note in packStore.
    bool tightBounds = true;

    // -- MERGE RUNS OF UNIFORM BRICKS INTO ONE BOX -------------------------
    //
    // A uniform brick is solid throughout and all one material, so a run of
    // adjacent ones IS a larger solid box -- and a box needs one AABB, not one
    // per brick. This is the AABB analogue of greedy meshing, and the only form
    // of it that means anything in a store that emits no triangles.
    //
    // MEASURED by tests/uniform_merge_probe.cpp on the 192 m wood: 66,127
    // uniform bricks collapse to 3,451 boxes, the largest 1,024 bricks, taking
    // the structure from 402,802 AABBs to 340,126 -- 15.6 % fewer. Three
    // hundred and forty boxes swallow ninety per cent of them.
    //
    // IT COSTS THE SHADER NOTHING. The uniform path never looked at a mask: it
    // answers from the slab test and a field of `flags`. All it needs is a box
    // bigger than one brick, and maskBase/mtlBase are dead on a uniform brick
    // and can carry the extent -- see the note on them in AabbShared.slang.
    //
    // A BOX MAY NOT CROSS A CHUNK SEAM. A chunk is one bottom-level structure
    // and its AABBs are a contiguous slice of one buffer, so the merge runs
    // within a chunk and stops at its edge.
    //
    // -- MEASURED, ON ONE BUILD AND ONE WORLD -----------------------------
    //
    //     trace   off 13.68 / 13.86 ms    on 13.63 / 13.57 ms
    //     device  off 50.4 + 13.6 MB      on 46.7 + 11.2 MB
    //     AABBs   off 370,098             on 301,792   (-18.5 %)
    //     fill    off 52 %                on 64 %
    //
    // 72,511 uniform bricks become 4,205 boxes, seventeen to one. The renders
    // hash EQUAL, so it is output-preserving.
    //
    // THE TRACE WIN IS SMALL AND THE MEMORY WIN IS NOT. 1.2 % is near this
    // scene's noise, though both merged runs beat both unmerged ones; 6.1 MB
    // and 68,306 primitives are unambiguous. It is on for the memory and the
    // structure, not for the frame.
    //
    // WHY IT IS NOT FASTER: the primitives it removes are the CHEAPEST ones. A
    // uniform candidate was already a slab test and an immediate return -- no
    // mask load, no walk. The 297,587 DETAIL bricks are untouched, and they are
    // where the time is. Dropping primitives only pays if they were expensive.
    //
    // THE TRAP, AND IT BIT ONCE: a uniform header's box now comes from
    // maskBase/mtlBase, so EVERY uniform brick must write its extent -- see the
    // note where the unmerged ones are emitted. Left at zero the shader reads a
    // one-voxel box and rays pass through solid ground. It does not look like a
    // store bug in a render; it looks like holes you blame on the peel.
    // tests/brick_test.cpp caught it as 80 missed rays of 1600.
    bool mergeUniform = true;

    // -- THE TWO PAYLOAD COMPRESSIONS, SWITCHABLE SO THEY CAN BE A/B'd ------
    //
    // Both formats are SELF-DESCRIBING, so one shader reads all four
    // combinations and nothing on the device has to know these exist:
    //
    //   sparseMask off writes every word and sets the kept-word map to all
    //   ones. The sparse reader then computes pos == word and behaves exactly
    //   like the dense one it replaced -- the only residue is a countbits a
    //   step, which is precisely the cost worth isolating.
    //
    //   palette off writes raw bytes and sets N to 0, which is already the
    //   overflow path, so it is a format the reader must handle regardless.
    // -- MEASURED, PINNED, TWO RUNS A LEG ----------------------------------
    //
    //     --world-seed 20260904 --spawn 7, one build, 36.65 M voxels every leg:
    //
    //         neither   23.53 / 23.53 ms   64.6 MB
    //         sparse    23.48 / 23.54     54.5 MB
    //         palette   23.86 / 23.58     57.8 MB
    //         BOTH      23.51 / 23.50     47.6 MB
    //
    // **BOTH ARE FREE AND TOGETHER THEY SAVE 17.0 MB, 26 %.** Trace does not
    // move at all -- the spread across four configurations is smaller than the
    // spread between two runs of one of them.
    //
    // THE FIRST TIME THESE WERE MEASURED THE SEEDS WERE NOT PINNED, and the
    // same four legs read 20.68 / 22.62 / 17.83 / 23.22 -- which says sparse is
    // 14 % FASTER and the palette costs 2 ms, and both are noise.
    // `--world-seed` rolls a new world every launch and `--spawn 0` moves the
    // camera every launch, so each leg was a different world seen from a
    // different place. Pin both or measure nothing.
    //
    // WHY THEY ARE FREE rather than slower: the walk was never bound by the
    // arithmetic these add. It is bound by how many candidate-steps there are,
    // which neither changes. And the mask being smaller does not speed it up
    // either -- the earlier finding that it is already in L1 holds.
    bool sparseMask = true;
    bool palette = true;

    // Drop bricks that are walled in on all six sides. Subsumed by `peel` when
    // that is on; kept so the two can be measured apart.
    bool cullHidden = true;

    // HOW BIG A BOTTOM-LEVEL STRUCTURE IS, in bricks on a side. 32 is the
    // engine's 25.6 m chunk. It is a real trade and not a tidiness choice: more
    // chunks mean a cheaper rebuild when one of them changes, and a deeper
    // traversal -- every ray descends the top-level structure to find the
    // instance and then the bottom-level one inside it -- while fewer, larger
    // chunks give the builder more to work with and one less level to walk.
    int chunkBricks = CHUNK_BRICK;

    // GROW EVERY AABB BY A HAIR.
    //
    // The hardware clips a candidate against the AABB in METRES; the shader
    // re-derives the same box in VOXEL units, because that is the space its DDA
    // walks in, and 0.1 is not exactly representable in binary. The two agree
    // to within an ulp or so, and a ray grazing a face could in principle be
    // clipped away by one and accepted by the other. A tenth of a millimetre of
    // slack costs 0.01 % more candidate volume and removes the question.
    float aabbPadM = 1e-4f;
};

inline uint32_t popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

// ---------------------------------------------------------------------------
// Pack the world. Deterministic: the same world always produces byte-identical
// buffers, which is what makes a round-trip test mean anything.
// ---------------------------------------------------------------------------
inline void packStoreInto(PackedStore &p, const BrickWorld &w, const PackOptions &opt,
                          PackCache *cache, const std::unordered_set<uint64_t> *dirty);

inline PackedStore packStore(const BrickWorld &w, const PackOptions &opt = PackOptions()) {
    PackedStore p;
    packStoreInto(p, w, opt, nullptr, nullptr);
    return p;
}

// THE BUFFERS HAVE TO SURVIVE BETWEEN CALLS for the slotted form to mean
// anything: a chunk that did not change is not written, because its bytes are
// already at its address FROM THE LAST PACK. Hand the same PackedStore back
// every time; packStoreInto keeps the three buffers and rebuilds everything
// else.
inline PackedStore packStore(const BrickWorld &w, const PackOptions &opt, PackCache *cache,
                             const std::unordered_set<uint64_t> *dirty) {
    PackedStore p;
    packStoreInto(p, w, opt, cache, dirty);
    return p;
}

// `cache` and `dirty` make the pack INCREMENTAL, and they are how a streamed
// window stays affordable. `dirty` is the set of chunk keys whose bricks may
// have changed -- which must include the ring AROUND anything that moved,
// because a brick's peel asks its neighbours. Everything else is reused from
// the cache, bytes and all. Pass nothing for a one-shot world: it rebuilds
// every chunk and, if a cache is given, fills it.
inline void packStoreInto(PackedStore &p, const BrickWorld &w, const PackOptions &opt,
                          PackCache *cache, const std::unordered_set<uint64_t> *dirty) {
    // The three big buffers are KEPT -- see the note on packStore. Everything
    // else describes this pass and is rebuilt.
    p.chunk.clear();
    p.dirtyBricks.clear();
    p.dirtyMask.clear();
    p.dirtyChunks.clear();
    p.uniform = p.detail = p.hidden = p.merged = p.mono = p.kept = 0;
    p.incremental = false;
    if (cache == nullptr) {
        p.brick.clear();
        p.aabb.clear();
        p.mask.clear();
    }
    p.voxels = w.voxelCount();

    // -----------------------------------------------------------------------
    // THE PEEL. One pass over every brick, producing the mask that is actually
    // uploaded, and an early answer to "does this brick survive at all".
    //
    // A brick's own 512 bits are not enough to decide this -- a voxel on the
    // brick's face is buried or not depending on the NEIGHBOURING brick -- so
    // the test goes through the world. That is a hash lookup per boundary
    // voxel, which is why the interior of the brick is done first with pure bit
    // arithmetic and only the six faces fall through to the slow path.
    // -----------------------------------------------------------------------
    // OPAQUE, NOT MERELY SOLID. Water is a dielectric the tracer refracts
    // through, so the bed of a lake is visible from above and its voxels are
    // not buried -- and water itself is never buried by the thing it covers.
    // Treating water as solid here quietly deletes every lake bed.
    // AND WHERE THE BRICK SAYS NOTHING, ASK THE FORMULA.
    //
    // This is what keeps the skin bands from growing an inner surface. Without
    // it every band's underside faces a brick that is absent, reads as air, and
    // is uploaded as visible geometry -- doubling the terrain in the buffers to
    // describe faces sealed inside solid rock. `wy` is the WORLD height, which
    // is the only thing the formula is indexed by.
    const ImplicitColumns &im = w.implicit();
    auto opaqueVox = [&im](const Brick *b, int lx, int ly, int lz, int wi, int wy, int wj) {
        if (b) {
            const int bit = Brick::bitOf(lx, ly, lz);
            if (b->bit(bit)) return b->mtl[bit] != mat::WATER;
        }
        if (!im.valid()) return false;
        const uint8_t m = im.at(wi, wy, wj);
        return m != mat::AIR && m != mat::WATER;
    };
    auto brickPtr = [&](int bx, int by, int bz) -> const Brick * {
        auto it = w.bricks().find(brickKey(bx, by, bz));
        return it == w.bricks().end() ? nullptr : &it->second;
    };

    struct Peeled {
        uint32_t mask[BRICK_WORDS];
        int count;
        int lo[3], hi[3];  // occupied bounds inside the brick, inclusive
        // WHETHER THE BRICK IS SOLID THROUGHOUT, COUNTING THE FORMULA.
        //
        // Not b.full(): most of a hillside's interior is no longer WRITTEN, it
        // is answered by ImplicitColumns, so asking the bricks alone says "not
        // full" for bricks that are solid rock in every direction. Those are
        // exactly the ones worth emitting as uniform -- no mask, no material
        // run, the slab test IS the intersection -- and deciding it from the
        // bricks alone quietly gave that up and made the two fills disagree.
        bool allSolid;
        uint8_t uniMtl;  // the one material, when allSolid
    };

    // THE SIX NEIGHBOURS ARE FETCHED ONCE PER BRICK, NOT ONCE PER VOXEL, and
    // that is the difference between a pack that takes 70 ms and one that takes
    // 760. The first version of this asked the world for every neighbour of
    // every solid voxel -- six hash lookups times twenty-eight million voxels.
    // Inside a brick a neighbour is a bit in the same 512-bit word array; only
    // the six FACES leave it, and which brick they leave into does not change
    // from voxel to voxel.
    auto peelBrick = [&](const Brick &b, int bx, int by, int bz, Peeled *out) {
        const Brick *nX0 = brickPtr(bx - 1, by, bz), *nX1 = brickPtr(bx + 1, by, bz);
        const Brick *nY0 = brickPtr(bx, by - 1, bz), *nY1 = brickPtr(bx, by + 1, bz);
        const Brick *nZ0 = brickPtr(bx, by, bz - 1), *nZ1 = brickPtr(bx, by, bz + 1);
        const int ox = bx * BRICK_E, oy = by * BRICK_E, oz = bz * BRICK_E;
        for (int k = 0; k < BRICK_WORDS; ++k) out->mask[k] = 0;
        out->count = 0;
        out->lo[0] = out->lo[1] = out->lo[2] = BRICK_E;
        out->hi[0] = out->hi[1] = out->hi[2] = -1;
        out->allSolid = true;
        out->uniMtl = 0;

        const int E = BRICK_E - 1;
        for (int lz = 0; lz < BRICK_E; ++lz)
            for (int ly = 0; ly < BRICK_E; ++ly)
                for (int lx = 0; lx < BRICK_E; ++lx) {
                    const int bit = Brick::bitOf(lx, ly, lz);
                    // The material here counting the formula, which is what
                    // decides whether this brick is solid throughout.
                    uint8_t m = b.bit(bit) ? b.mtl[bit] : mat::AIR;
                    if (m == mat::AIR && im.valid()) m = im.at(ox + lx, oy + ly, oz + lz);
                    if (m == mat::AIR) out->allSolid = false;
                    else if (out->uniMtl == 0) out->uniMtl = m;
                    else if (out->uniMtl != m) out->allSolid = false;

                    if (!b.bit(bit)) continue;
                    if (opt.peel && b.mtl[bit] != mat::WATER) {
                        const int wi = ox + lx, wy = oy + ly, wj = oz + lz;
                        const bool walled =
                            (lx > 0 ? opaqueVox(&b, lx - 1, ly, lz, wi - 1, wy, wj)
                                    : opaqueVox(nX0, E, ly, lz, wi - 1, wy, wj)) &&
                            (lx < E ? opaqueVox(&b, lx + 1, ly, lz, wi + 1, wy, wj)
                                    : opaqueVox(nX1, 0, ly, lz, wi + 1, wy, wj)) &&
                            (ly > 0 ? opaqueVox(&b, lx, ly - 1, lz, wi, wy - 1, wj)
                                    : opaqueVox(nY0, lx, E, lz, wi, wy - 1, wj)) &&
                            (ly < E ? opaqueVox(&b, lx, ly + 1, lz, wi, wy + 1, wj)
                                    : opaqueVox(nY1, lx, 0, lz, wi, wy + 1, wj)) &&
                            (lz > 0 ? opaqueVox(&b, lx, ly, lz - 1, wi, wy, wj - 1)
                                    : opaqueVox(nZ0, lx, ly, E, wi, wy, wj - 1)) &&
                            (lz < E ? opaqueVox(&b, lx, ly, lz + 1, wi, wy, wj + 1)
                                    : opaqueVox(nZ1, lx, ly, 0, wi, wy, wj + 1));
                        if (walled) continue;  // buried: nothing can ever reach it
                    }
                    out->mask[bit >> 5] |= (1u << (bit & 31));
                    ++out->count;
                    const int v[3] = {lx, ly, lz};
                    for (int k = 0; k < 3; ++k) {
                        if (v[k] < out->lo[k]) out->lo[k] = v[k];
                        if (v[k] > out->hi[k]) out->hi[k] = v[k];
                    }
                }
    };

    // -----------------------------------------------------------------------
    // THE PEEL RUNS ON EVERY CORE, AND IT HAD TO.
    //
    // It is the most expensive thing in the pack -- a hundred million voxels,
    // six neighbour tests each -- and it is the cost this optimisation ADDED to
    // startup. Leaving it on one thread would have handed back in launch time
    // what the frame gained.
    //
    // It parallelises exactly: every brick's answer depends only on the world,
    // which nothing is writing, and each thread fills its own slice of a
    // pre-sized vector -- no lock, no allocation in the loop, and no shared
    // counter. The grouping afterwards stays sequential because it appends, and
    // it is cheap.
    //
    // THE ORDER IS THE KEY VECTOR'S, NOT THE HASH MAP'S, which is what keeps
    // the result the same every run -- see the note on sorting below.
    // -----------------------------------------------------------------------
    const int cb = opt.chunkBricks > 0 ? opt.chunkBricks : CHUNK_BRICK;
    auto chunkOf = [cb](uint64_t k) {
        return brickKey(floorDiv(keyX(k), cb), floorDiv(keyY(k), cb), floorDiv(keyZ(k), cb));
    };
    // REUSE IS PER CHUNK, so a brick in a chunk that is being kept is never
    // peeled at all -- which is most of the peel's cost gone with most of the
    // assembly's. A chunk is reusable only if the caller says nothing near it
    // moved AND the previous pack actually produced it.
    const bool incremental = cache != nullptr && dirty != nullptr && !cache->chunk.empty();
    auto reusable = [&](uint64_t ck) {
        return incremental && !dirty->count(ck) && cache->chunk.count(ck) != 0;
    };

    std::vector<uint64_t> allKeys;
    allKeys.reserve(w.bricks().size());
    for (const auto &kv : w.bricks()) {
        if (kv.second.count == 0) continue;
        if (reusable(chunkOf(kv.first))) continue;
        allKeys.push_back(kv.first);
    }

    std::vector<Peeled> peeledOf(allKeys.size());
    const auto tPeel0 = std::chrono::steady_clock::now();
    {
        unsigned nt = std::thread::hardware_concurrency();
        if (nt == 0) nt = 1;
        if (nt > 16u) nt = 16u;
        if (allKeys.size() < 4096) nt = 1;  // not worth the threads
        auto worker = [&](size_t lo, size_t hi) {
            for (size_t n = lo; n < hi; ++n) {
                const uint64_t k = allKeys[n];
                const Brick &b = w.bricks().at(k);
                const int bx = keyX(k), by = keyY(k), bz = keyZ(k);
                // The whole-brick test first: six hash lookups against the
                // peel's five hundred, and it answers most of a landscape's
                // interior outright.
                // SEALED, NOT MERELY FULL -- the peel's opacity rule, one
                // level up. A brick that is solid water inside solid water is
                // full on every count of bits and still visible from outside,
                // because the tracer refracts through all of it.
                if (opt.cullHidden && b.sealed() && w.sealedBrickAt(bx - 1, by, bz) &&
                    w.sealedBrickAt(bx + 1, by, bz) && w.sealedBrickAt(bx, by - 1, bz) &&
                    w.sealedBrickAt(bx, by + 1, bz) && w.sealedBrickAt(bx, by, bz - 1) &&
                    w.sealedBrickAt(bx, by, bz + 1)) {
                    peeledOf[n].count = 0;
                    continue;
                }
                peelBrick(b, bx, by, bz, &peeledOf[n]);
            }
        };
        if (nt <= 1) {
            worker(0, allKeys.size());
        } else {
            std::vector<std::thread> th;
            th.reserve(nt);
            const size_t span = (allKeys.size() + nt - 1) / nt;
            for (unsigned t = 0; t < nt; ++t) {
                const size_t lo = std::min(allKeys.size(), size_t(t) * span);
                const size_t hi = std::min(allKeys.size(), lo + span);
                if (lo < hi) th.emplace_back(worker, lo, hi);
            }
            for (std::thread &t : th) t.join();
        }
    }

    // -- group the survivors by chunk ---------------------------------------
    p.peelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                        tPeel0)
                   .count();
    const auto tAsm0 = std::chrono::steady_clock::now();

    std::unordered_map<uint64_t, const Peeled *> peeled;
    peeled.reserve(allKeys.size() * 2);
    std::unordered_map<uint64_t, std::vector<uint64_t>> byChunk;
    byChunk.reserve(allKeys.size() / 64 + 16);
    for (size_t n = 0; n < allKeys.size(); ++n) {
        const Peeled &pk = peeledOf[n];
        if (pk.count == 0) {
            // Either walled in as a whole brick, or every voxel in it buried.
            ++p.hidden;
            continue;
        }
        p.kept += uint64_t(pk.count);
        peeled.emplace(allKeys[n], &pk);
        const int bx = keyX(allKeys[n]), by = keyY(allKeys[n]), bz = keyZ(allKeys[n]);
        (void)bx; (void)by; (void)bz;
        byChunk[chunkOf(allKeys[n])].push_back(allKeys[n]);
    }

    // THE CHUNKS THAT ARE BEING KEPT STILL HAVE TO APPEAR, and in the same
    // sorted order as everything else -- they are a slice of the same buffers.
    // Their bricks were never peeled, so byChunk has no entry for them and the
    // emit step skips them; the join takes their bytes from the cache.
    // ...AND ONLY IF THE WORLD STILL HAS IT. Trusting the cache alone was a
    // real bug: after the window jumped clear of itself and was rebuilt from
    // scratch, every chunk of the OLD position was still cached, was not in the
    // caller's dirty set, and was emitted into the pack as though it were
    // there. The store then described two worlds at once and the cache grew
    // without bound -- 2,111 chunks to 6,488 in five crossings.
    // ...AND ONLY IF THE WORLD STILL HAS IT. Trusting the cache alone was a
    // real bug: after the window jumped clear of itself, every chunk of the OLD
    // position was still cached, was not in the caller's dirty set, and was
    // emitted into the pack as though it were there. The store described two
    // worlds at once and the cache grew without bound -- 2,111 chunks to 6,488
    // in five crossings. One brick key settles it; see PackedChunk::probe.
    std::unordered_set<uint64_t> kept;
    if (incremental)
        for (const auto &kv : cache->chunk)
            if (!dirty->count(kv.first) && w.bricks().count(kv.second.probe))
                kept.insert(kv.first);

    // SORTED, AND BOTH LEVELS OF IT. An unordered_map's order depends on its
    // history, so an unsorted pack gives two different byte streams for two
    // worlds that are identical -- which defeats the round-trip test and makes
    // an A/B of two runs unreadable. It also groups each chunk's bricks in
    // space, which is what the structure builder wants anyway.
    std::vector<uint64_t> chunkKeys;
    chunkKeys.reserve(byChunk.size() + kept.size());
    for (const auto &kv : byChunk) chunkKeys.push_back(kv.first);
    for (uint64_t ck : kept) chunkKeys.push_back(ck);
    std::sort(chunkKeys.begin(), chunkKeys.end());

    p.chunk.reserve(chunkKeys.size());
    uint64_t boxVox = 0;  // total volume of the emitted AABBs, in voxels

    // -- EMIT EACH CHUNK INTO ITS OWN BUFFERS, ON EVERY CORE ----------------
    //
    // THIS IS THE PACK, AND IT WAS SEVEN EIGHTHS OF IT. Measured on the 192 m
    // world: the peel is 104 ms across all cores and laying out the buffers was
    // 717 ms on one. That is the wrong way round for something whose per-brick
    // work -- a header, sixteen mask words, a compacted material run and six
    // AABB floats -- has no dependency between bricks at all.
    //
    // The only thing that made it serial was the OUTPUT: three vectors that
    // every brick appended to, and a mask offset stored in each header as an
    // absolute index into one of them. So each chunk now fills its own vectors
    // and the offsets are fixed up when they are concatenated, in order. The
    // bytes are identical -- verified by hash, which is the whole reason that
    // harness exists.
    //
    // IT MATTERS MOST TO THE STREAMED WORLD. A window that slides re-packs
    // everything resident on every crossing, so this is a second off every
    // 25.6 m of walking; on a world built once it is a second off the launch.
    std::vector<PackedChunk> outs(chunkKeys.size());

    // SORTED SERIALLY, BEFORE THE THREADS. `byChunk[ck]` would INSERT on a
    // miss, and a map that can rehash under four threads is not a map.
    for (uint64_t ck : chunkKeys) {
        // find, NOT operator[] -- and this is the second time that distinction
        // has mattered in this function. The note below warns that `byChunk[ck]`
        // inserts under threads; it also inserts HERE, an empty vector for every
        // chunk being reused from the cache, which then made the join think
        // those chunks had been freshly emitted and write nothing for them. A
        // window that slid lost seven eighths of its world and reported no
        // error at all.
        auto it = byChunk.find(ck);
        if (it == byChunk.end()) continue;  // kept: the cache has its bytes
        std::sort(it->second.begin(), it->second.end());
    }

    auto emitChunk = [&](size_t ci) {
        const uint64_t ck = chunkKeys[ci];
        if (byChunk.find(ck) == byChunk.end()) return;  // kept: the cache has it
        PackedChunk &out = outs[ci];
        const std::vector<uint64_t> &keys = byChunk.at(ck);


        // -- GREEDY-MERGE THE UNIFORM BRICKS INTO BOXES --------------------
        //
        // See PackOptions::mergeUniform. Everything not absorbed into a box is
        // emitted brick by brick below, exactly as it always was.
        std::vector<uint64_t> solo;
        solo.reserve(keys.size());
        if (!opt.mergeUniform) {
            solo = keys;
        } else {
            std::unordered_map<uint64_t, uint8_t> uni;
            for (uint64_t bk : keys) {
                const Peeled &pk = *peeled.at(bk);
                if (pk.allSolid && pk.uniMtl != mat::AIR)
                    uni.emplace(bk, uint8_t(pk.uniMtl));
                else
                    solo.push_back(bk);
            }
            std::unordered_map<uint64_t, bool> used;
            used.reserve(uni.size() * 2);
            // KEYS IS SORTED, so the boxes come out the same for the same world
            // -- the determinism test packs twice and compares the bytes.
            for (uint64_t bk : keys) {
                auto it = uni.find(bk);
                if (it == uni.end() || used[bk]) continue;
                const uint8_t m = it->second;
                const int sx = keyX(bk), sy = keyY(bk), sz = keyZ(bk);
                auto free1 = [&](int x, int y, int z) {
                    const uint64_t k2 = brickKey(x, y, z);
                    auto i2 = uni.find(k2);
                    return i2 != uni.end() && i2->second == m && !used[k2];
                };
                // Grow x, then y over the whole x-span, then z over the slab.
                int ex = 0, ey = 0, ez = 0;
                while (free1(sx + ex + 1, sy, sz)) ++ex;
                for (;;) {
                    bool ok = true;
                    for (int dx = 0; dx <= ex && ok; ++dx)
                        if (!free1(sx + dx, sy + ey + 1, sz)) ok = false;
                    if (!ok) break;
                    ++ey;
                }
                for (;;) {
                    bool ok = true;
                    for (int dx = 0; dx <= ex && ok; ++dx)
                        for (int dy = 0; dy <= ey && ok; ++dy)
                            if (!free1(sx + dx, sy + dy, sz + ez + 1)) ok = false;
                    if (!ok) break;
                    ++ez;
                }
                for (int dx = 0; dx <= ex; ++dx)
                    for (int dy = 0; dy <= ey; ++dy)
                        for (int dz = 0; dz <= ez; ++dz)
                            used[brickKey(sx + dx, sy + dy, sz + dz)] = true;

                V4Brick h{};
                h.ox = sx * BRICK_E;
                h.oy = sy * BRICK_E;
                h.oz = sz * BRICK_E;
                // ...and bit 2 if that one material is water, exactly as an
                // unmerged uniform brick sets it.
                h.flags = 1u | (m == mat::WATER ? 4u : 0u) | (uint32_t(m) << 8);
                // THE EXTENT, IN VOXELS, INCLUSIVE, in the two words a mask
                // would have used. A single brick is 7 on every axis, which is
                // exactly what an unmerged uniform brick used to mean.
                const uint32_t vx = uint32_t((ex + 1) * BRICK_E - 1);
                const uint32_t vy = uint32_t((ey + 1) * BRICK_E - 1);
                const uint32_t vz = uint32_t((ez + 1) * BRICK_E - 1);
                h.maskBase = vx | (vy << 16);
                h.mtlBase = vz;
                // Unread when uniform -- the shader takes the box from the two
                // fields above -- but written whole so a dump is not confusing.
                const int bsq = kBrickShift;
                h.bounds = uint32_t(BRICK_E - 1) << (3 * bsq) |
                           uint32_t(BRICK_E - 1) << (4 * bsq) |
                           uint32_t(BRICK_E - 1) << (5 * bsq);
                h.octants = 0xFFu;

                const float ob[3] = {float(h.ox) * VOXEL_M, float(h.oy) * VOXEL_M,
                                     float(h.oz) * VOXEL_M};
                const uint32_t ev[3] = {vx, vy, vz};
                for (int k = 0; k < 3; ++k) out.aabb.push_back(ob[k] - opt.aabbPadM);
                for (int k = 0; k < 3; ++k)
                    out.aabb.push_back(ob[k] + float(ev[k] + 1) * VOXEL_M + opt.aabbPadM);
                out.boxVox += uint64_t(vx + 1) * uint64_t(vy + 1) * uint64_t(vz + 1);
                out.brick.push_back(h);
                ++out.uniform;
                out.merged += uint64_t(ex + 1) * uint64_t(ey + 1) * uint64_t(ez + 1);
            }
        }

        for (uint64_t bk : solo) {
            const Brick &b = w.bricks().at(bk);
            const Peeled &pk = *peeled.at(bk);

            V4Brick h{};
            h.ox = keyX(bk) * BRICK_E;
            h.oy = keyY(bk) * BRICK_E;
            h.oz = keyZ(bk) * BRICK_E;

            // -- UNIFORM: SOLID THROUGHOUT, AND ALL OF IT ONE MATERIAL -----
            //
            // Decided on the brick as it STANDS, not on the peeled mask. A
            // brick that is solid everywhere really is solid everywhere: a ray
            // entering it stops on the face it came in through, which is what
            // the slab test already computes, so there is nothing to walk and
            // nothing to store. Deciding it from the peel would turn every one
            // into a hollow shell with a mask -- 576 bytes and a DDA to reach
            // the same answer.
            //
            // "As it stands" includes the formula -- see Peeled::allSolid.
            const bool uniform = pk.allSolid && pk.uniMtl != mat::AIR;
            if (uniform) {
                // Bit 2 says the brick holds water -- see the note on flags in
                // AabbShared.slang. A uniform brick is one material, so the
                // question is just what that material is.
                h.flags = 1u | (pk.uniMtl == mat::WATER ? 4u : 0u) |
                          (uint32_t(pk.uniMtl) << 8);
                // THE EXTENT, EVEN UNMERGED, AND THIS IS LOAD-BEARING. A
                // uniform header's box comes from these two fields now -- see
                // PackOptions::mergeUniform -- so an unmerged one has to say
                // "one brick" rather than leave them zero. Left at zero the
                // shader reads a box ONE VOXEL wide and rays walk straight
                // through solid ground: the traversal test caught it as 80
                // missed rays out of 1600, and it is invisible in a render
                // except as holes you would blame on the peel.
                h.maskBase = uint32_t(BRICK_E - 1) | (uint32_t(BRICK_E - 1) << 16);
                h.mtlBase = uint32_t(BRICK_E - 1);
                ++out.uniform;
            }

            int lo[3], hi[3];
            if (uniform) {
                // Solid to its own edges, so the box is the whole brick.
                for (int k = 0; k < 3; ++k) { lo[k] = 0; hi[k] = BRICK_E - 1; }
            } else {
                for (int k = 0; k < 3; ++k) { lo[k] = pk.lo[k]; hi[k] = pk.hi[k]; }
                h.flags = 0;
                h.maskBase = uint32_t(out.mask.size());
                // -- WORD-SPARSE: ONLY THE WORDS THAT HOLD ANYTHING ---------
                //
                // The mask was sixteen flat words whether the brick held one
                // voxel or five hundred, and it was the single biggest thing in
                // the store. Measured over this wood by tests/payload_probe.cpp,
                // only 8.1 of 16 words are non-zero on average: 20.5 MB becomes
                // 11.1 MB, 46 % saved.
                //
                // WORD-SPARSE AND NOT OCTANT-SPARSE, and the reason is not the
                // ratio -- octants measured 45.7 % against this 46.1 %, a wash.
                // It is that dropping empty WORDS preserves the LINEAR bit
                // order, so the material run's popcount rank is unchanged and
                // the packer and the shader keep the one ordering they already
                // agree about. Octant-sparse would have reordered both, and a
                // disagreement there does not fail -- it paints every voxel its
                // neighbour's colour.
                //
                // The map of which words are kept lives in `octants` bits 8..23,
                // which were pad. A zero word is simply absent, and a voxel in
                // one reads as air without a load.
                uint32_t wordMap = 0;
                for (int k = 0; k < BRICK_WORDS; ++k)
                    if (pk.mask[k] || !opt.sparseMask) {
                        wordMap |= 1u << k;
                        out.mask.push_back(pk.mask[k]);
                    }
                h.octants = wordMap << 8;  // the octant byte is OR'd in below
                // THE RUN, IN BIT ORDER, AND OVER THE PEELED MASK. The shader
                // finds a voxel's byte by counting the set bits before it in
                // the mask it was given, so this loop has to walk the same bits
                // -- writing the world's mask here and the peeled one above
                // would shift every material by the number of buried voxels
                // that came before it, and nothing would report it.
                // -- ONE MATERIAL, OR A RUN ---------------------------------
                //
                // Decided BEFORE anything is written, because a monochrome
                // brick writes no run at all. The material bytes are the
                // second largest thing on the device after the masks, and a
                // third of detail bricks wear a single id -- a patch of grass,
                // a stretch of litter -- so this is most of that third gone,
                // and their resolve becomes a field of a word already loaded
                // instead of up to sixteen dependent mask reads.
                int m0 = -1;
                bool one = true;
                for (int k = 0; k < BRICK_VOX && one; ++k)
                    if ((pk.mask[k >> 5] >> (k & 31)) & 1u) {
                        if (m0 < 0) m0 = b.mtl[k];
                        else if (b.mtl[k] != m0) one = false;
                    }
                if (one) {
                    h.flags = 2u | (m0 == int(mat::WATER) ? 4u : 0u) |
                              (uint32_t(m0 < 0 ? 0 : m0) << 8);
                    h.mtlBase = 0;
                    ++out.mono;
                } else {
                    // THE RUN, IN BIT ORDER, AND OVER THE PEELED MASK. The
                    // shader finds a voxel's byte by counting the set bits
                    // before it in the mask it was given, so this loop has to
                    // walk the same bits -- writing the world's mask here and
                    // the peeled one above would shift every material by the
                    // number of buried voxels before it, and nothing reports it.
                    // -- THE MATERIALS GO STRAIGHT AFTER THIS BRICK'S MASK ---
                    //
                    // ONE BUFFER, NOT TWO, AND THAT IS THE WHOLE POINT. They
                    // used to live in a separate 11 MB array, so resolving a
                    // material was a scattered fetch into a buffer nothing
                    // else had touched -- a cache miss on every hit, and 3 ms
                    // of an 18 ms trace at 4K.
                    //
                    // A POPCOUNT PREFIX TABLE WAS TRIED FIRST AND DID NOTHING
                    // -- 3.33 ms against 3.04 -- which is what identified
                    // this. The fifteen mask reads it was meant to save are
                    // sixty-four contiguous bytes and were already in L1. The
                    // miss was the material byte. Put it a hundred bytes from
                    // the mask word the walk just read and it rides in on the
                    // same cache lines.
                    // -- A PER-BRICK PALETTE, AND INDICES NARROWER THAN A BYTE
                    //
                    // One byte a voxel spends eight bits naming one of at most
                    // SIXTEEN things. Measured by tests/payload_probe.cpp over
                    // this wood: 52 % of detail bricks wear two materials or
                    // fewer -- ONE BIT a voxel -- 61 % wear four or fewer, and
                    // NOT ONE anywhere exceeds sixteen, so four bits is a hard
                    // ceiling. 12.7 MB of runs becomes 5.5.
                    //
                    // IT IS CHEAP ONLY BECAUSE THE RESOLVE MOVED. storeVoxelMtl
                    // ran at every COMMIT and now runs once per RAY (see
                    // kMtlPending in stores/Aabb.slang), so the extra shift and
                    // palette read are paid once rather than three or four
                    // times. Before that change this would not have been worth
                    // doing.
                    //
                    // LAYOUT AT mtlBase: N palette BYTES, then the indices
                    // packed at `bits` each, in the same LINEAR bit order the
                    // mask is counted in. N is in flags 16..21 and `bits` is
                    // DERIVED from it -- storing a width that can disagree with
                    // the count is a way to be wrong silently.
                    // N IS CAPPED, AND OVERFLOW FALLS BACK TO RAW BYTES.
                    // tests/payload_probe.cpp measured every brick in one wood
                    // at sixteen materials or fewer -- and a world is edited
                    // between runs, so "every brick, once" is not a rule the
                    // packer may rely on. A brick that exceeds the palette
                    // writes the old byte-per-voxel run and sets N to zero,
                    // which is the reader's signal to take that path.
                    //
                    // WITHOUT THE FALLBACK IT FAILS SILENTLY: a material the
                    // palette has no room for resolves to index 0, so every
                    // voxel of it wears pal[0]. The traversal still agrees
                    // about WHERE the geometry is, which is why only the
                    // material check catches it.
                    uint8_t pal[kMtlPaletteMax];
                    int np = 0;
                    bool palOverflow = false;
                    for (int k = 0; k < BRICK_VOX; ++k) {
                        if (!((pk.mask[k >> 5] >> (k & 31)) & 1u)) continue;
                        // Bit 2, over the PEELED mask: a water voxel the peel
                        // dropped is not in the buffers and cannot stop a
                        // shadow ray, so it must not make the walk pay for a
                        // material resolve either.
                        if (b.mtl[k] == mat::WATER) h.flags |= 4u;
                        const uint8_t m = b.mtl[k];
                        bool have = false;
                        for (int q = 0; q < np; ++q)
                            if (pal[q] == m) { have = true; break; }
                        if (have) continue;
                        if (np >= kMtlPaletteMax) { palOverflow = true; continue; }
                        pal[np++] = m;
                    }
                    if (palOverflow || !opt.palette) np = 0;  // 0 means RAW BYTES

                    h.flags |= uint32_t(np) << 16;
                    h.mtlBase = uint32_t(out.mask.size()) * 4u;  // BYTE index

                    if (np == 0) {
                        uint32_t acc = 0, n = 0;
                        for (int k = 0; k < BRICK_VOX; ++k)
                            if ((pk.mask[k >> 5] >> (k & 31)) & 1u) {
                                acc |= uint32_t(b.mtl[k]) << ((n & 3u) * 8u);
                                if ((++n & 3u) == 0u) { out.mask.push_back(acc); acc = 0; }
                            }
                        if (n & 3u) out.mask.push_back(acc);
                    } else {
                        int bits = 1;
                        while ((1 << bits) < np) ++bits;

                        uint32_t acc = 0, nb = 0;
                        for (int q = 0; q < np; ++q) {
                            acc |= uint32_t(pal[q]) << ((nb & 3u) * 8u);
                            if ((++nb & 3u) == 0u) { out.mask.push_back(acc); acc = 0; }
                        }
                        // FLUSHED TO A WORD BOUNDARY, which is why the reader
                        // starts the indices at ceil(N/4) WORDS past mtlBase and
                        // not at byte mtlBase + N.
                        if (nb & 3u) out.mask.push_back(acc);

                        uint64_t bitAcc = 0;
                        int bitN = 0;
                        for (int k = 0; k < BRICK_VOX; ++k) {
                            if (!((pk.mask[k >> 5] >> (k & 31)) & 1u)) continue;
                            uint32_t pi = 0;
                            for (int q = 0; q < np; ++q)
                                if (pal[q] == b.mtl[k]) { pi = uint32_t(q); break; }
                            bitAcc |= uint64_t(pi) << bitN;
                            bitN += bits;
                            if (bitN >= 32) {
                                out.mask.push_back(uint32_t(bitAcc & 0xFFFFFFFFull));
                                bitAcc >>= 32;
                                bitN -= 32;
                            }
                        }
                        if (bitN > 0) out.mask.push_back(uint32_t(bitAcc & 0xFFFFFFFFull));
                    }
                }
                ++out.detail;
            }
            if (!opt.tightBounds) {
                for (int k = 0; k < 3; ++k) { lo[k] = 0; hi[k] = BRICK_E - 1; }
            }

            // -- THE BOUNDS GO IN THE HEADER TOO, not only in the AABB -------
            //
            // The hardware clips against the AABB and the shader re-derives the
            // box in voxel units for its own slab test and DDA. If the shader
            // kept using the full 8^3 box it would still be CORRECT -- a
            // superset -- but it would start its walk further out and step
            // through the empty margin the AABB was tightened to exclude. Three
            // bits an axis is all it takes, and pad0 was already there.
            const int bs = kBrickShift;
            h.bounds = uint32_t(lo[0]) | (uint32_t(lo[1]) << bs) | (uint32_t(lo[2]) << (2 * bs)) |
                       (uint32_t(hi[0]) << (3 * bs)) | (uint32_t(hi[1]) << (4 * bs)) |
                       (uint32_t(hi[2]) << (5 * bs));

            // -- WHICH OCTANTS HOLD ANYTHING, over the mask that is UPLOADED --
            //
            // The peeled mask, not the world's: the walk reads these bits to
            // decide whether to look at cells at all, so a bit set for an
            // octant whose voxels were all peeled away would send it stepping
            // through eight hundred cubic centimetres of nothing, and a bit
            // CLEARED for an octant that has voxels would skip real geometry.
            // A uniform brick is solid everywhere, so all eight are set.
            {
                const int H = BRICK_E / 2;
                uint32_t oct = 0;
                if (uniform) {
                    oct = 0xFFu;
                } else {
                    for (int k = 0; k < BRICK_VOX; ++k) {
                        if (!((pk.mask[k >> 5] >> (k & 31)) & 1u)) continue;
                        const int lx = k & (BRICK_E - 1);
                        const int ly = (k >> kBrickShift) & (BRICK_E - 1);
                        const int lz = (k >> (2 * kBrickShift)) & (BRICK_E - 1);
                        oct |= 1u << ((lx / H) | ((ly / H) << 1) | ((lz / H) << 2));
                    }
                }
                h.octants |= oct;  // bits 0..7; 8..23 are the word map
            }

            const float o[3] = {float(h.ox) * VOXEL_M, float(h.oy) * VOXEL_M,
                                float(h.oz) * VOXEL_M};
            for (int k = 0; k < 3; ++k)
                out.aabb.push_back(o[k] + float(lo[k]) * VOXEL_M - opt.aabbPadM);
            for (int k = 0; k < 3; ++k)
                out.aabb.push_back(o[k] + float(hi[k] + 1) * VOXEL_M + opt.aabbPadM);
            out.boxVox += uint64_t(hi[0] - lo[0] + 1) * uint64_t(hi[1] - lo[1] + 1) *
                      uint64_t(hi[2] - lo[2] + 1);
            out.brick.push_back(h);
        }
        // AFTER the emit, not before: merging changes how many AABBs a chunk
        // owns, and c.count IS the primitive count the structure is built with.
    };

    {
        unsigned nt = std::thread::hardware_concurrency();
        if (nt == 0) nt = 1;
        if (nt > 16u) nt = 16u;
        if (chunkKeys.size() < 8) nt = 1;
        if (nt <= 1) {
            for (size_t ci = 0; ci < chunkKeys.size(); ++ci) emitChunk(ci);
        } else {
            std::vector<std::thread> th;
            th.reserve(nt);
            const size_t per = (chunkKeys.size() + nt - 1) / nt;
            for (unsigned t = 0; t < nt; ++t) {
                const size_t lo = size_t(t) * per;
                const size_t hi = std::min(chunkKeys.size(), lo + per);
                if (lo >= hi) break;
                th.emplace_back([&emitChunk, lo, hi]() {
                    for (size_t ci = lo; ci < hi; ++ci) emitChunk(ci);
                });
            }
            for (std::thread &t : th) t.join();
        }
    }

    // -- ...AND JOIN THEM, WHICH IS WHERE THE OFFSETS ARE MADE REAL ---------
    //
    // A header's maskBase is an index into the ONE mask buffer and its mtlBase
    // a byte offset into the same, so each chunk's are relative to its own
    // vector until here. A UNIFORM brick's two fields are not offsets at all --
    // they carry the box's extent (see PackOptions::mergeUniform) -- and a
    // MONOCHROME brick's mtlBase is a zero that nothing reads. Adding a base to
    // either would corrupt a brick that looks perfectly well formed.
    // -- LAY THE CHUNKS OUT ------------------------------------------------
    //
    // Without a cache this is a plain concatenation, which is what a world
    // built once wants: dense, no holes, nothing to remember.
    //
    // WITH ONE, EVERY CHUNK KEEPS ITS ADDRESS. See SlotAlloc: a chunk that did
    // not change is not copied, not uploaded and -- the expensive part -- its
    // bottom-level structure is not rebuilt. The buffers then have gaps in
    // them, which costs memory and nothing else: a gap is never indexed,
    // because a brick is only ever reached through its own chunk's run.
    const bool slotted = cache != nullptr;
    if (!slotted) {
        size_t nb = 0, na = 0, nm = 0;
        for (size_t ci = 0; ci < chunkKeys.size(); ++ci) {
            const PackedChunk &o2 = outs[ci];
            nb += o2.brick.size();
            na += o2.aabb.size();
            nm += o2.mask.size();
        }
        p.brick.reserve(nb);
        p.aabb.reserve(na);
        p.mask.reserve(nm);
    } else {
        p.incremental = incremental;
        // Give back the slots of chunks that have left, BEFORE anything is
        // allocated -- that is what lets the arriving chunks reuse them and
        // keeps the high-water mark from climbing as the window travels.
        std::unordered_set<uint64_t> live(chunkKeys.begin(), chunkKeys.end());
        for (auto it = cache->brickSlot.begin(); it != cache->brickSlot.end();) {
            if (live.count(it->first)) { ++it; continue; }
            cache->bricks.give(it->second);
            it = cache->brickSlot.erase(it);
        }
        for (auto it = cache->maskSlot.begin(); it != cache->maskSlot.end();) {
            if (live.count(it->first)) { ++it; continue; }
            cache->masks.give(it->second);
            it = cache->maskSlot.erase(it);
        }
    }

    for (size_t ci = 0; ci < chunkKeys.size(); ++ci) {
        const uint64_t ck = chunkKeys[ci];
        // Freshly emitted, or kept from the last pack -- the join does not care
        // which, and that is the whole point of the unit.
        const bool fresh = byChunk.find(ck) != byChunk.end();
        PackedStore::Chunk c;
        c.cx = keyX(ck);
        c.cy = keyY(ck);
        c.cz = keyZ(ck);

        // -- A KEPT CHUNK IS ALREADY DONE, SO DO NOTHING TO IT ---------------
        //
        // Its bytes are in the buffers at its address from a previous pack and
        // its structure is still the structure of them. All that is needed is
        // its row in the chunk table.
        //
        // THIS WAS MOST OF THE PACK. Rebasing the mask offsets ran over every
        // chunk's bricks and then ran again to undo itself -- 3.3 million
        // bricks touched twice to publish the two thousand that had changed,
        // 600 ms for a crossing that moved one chunk.
        if (!fresh && slotted) {
            const SlotAlloc::Range &br = cache->brickSlot.at(ck);
            const PackedChunk &kc = cache->chunk.at(ck);
            c.first = br.first;
            c.count = uint32_t(kc.brick.size());
            boxVox += kc.boxVox;
            p.uniform += kc.uniform;
            p.merged += kc.merged;
            p.detail += kc.detail;
            p.mono += kc.mono;
            p.chunk.push_back(c);
            continue;
        }

        PackedChunk &out = fresh ? outs[ci] : cache->chunk.at(ck);

        uint32_t bFirst, mFirst;
        if (!slotted) {
            bFirst = uint32_t(p.brick.size());
            mFirst = uint32_t(p.mask.size());
        } else {
            // Keep the slot if it still fits; otherwise give it back and take
            // another. A chunk that grew by one brick does not move.
            auto bs = cache->brickSlot.find(ck);
            if (bs == cache->brickSlot.end() || bs->second.cap < out.brick.size()) {
                if (bs != cache->brickSlot.end()) {
                    cache->bricks.give(bs->second);
                    cache->brickSlot.erase(bs);
                }
                cache->brickSlot[ck] = cache->bricks.take(uint32_t(out.brick.size()));
            }
            auto ms2 = cache->maskSlot.find(ck);
            if (ms2 == cache->maskSlot.end() || ms2->second.cap < out.mask.size()) {
                if (ms2 != cache->maskSlot.end()) {
                    cache->masks.give(ms2->second);
                    cache->maskSlot.erase(ms2);
                }
                cache->maskSlot[ck] = cache->masks.take(uint32_t(out.mask.size()));
            }
            bFirst = cache->brickSlot[ck].first;
            mFirst = cache->maskSlot[ck].first;
        }
        c.first = bFirst;
        c.count = uint32_t(out.brick.size());

        // The headers' offsets are relative to this chunk's own mask vector
        // until here; see PackedChunk.
        for (V4Brick &h : out.brick) {
            if ((h.flags & 1u) != 0u) continue;  // uniform: an extent, not an offset
            h.maskBase += mFirst;
            if ((h.flags & 2u) == 0u) h.mtlBase += mFirst * 4u;  // mono: a zero nobody reads
        }

        if (!slotted) {
            p.brick.insert(p.brick.end(), out.brick.begin(), out.brick.end());
            p.aabb.insert(p.aabb.end(), out.aabb.begin(), out.aabb.end());
            p.mask.insert(p.mask.end(), out.mask.begin(), out.mask.end());
        } else {
            // WRITTEN ONLY IF IT CHANGED. An untouched chunk's bytes are
            // already where they belong, from a previous pack.
            //
            // -- AND SIZED WITH HEADROOM, BECAUSE THIS *IS* THE DEVICE BUFFER'S
            // SIZE. The buffers are created WITH their contents -- the only way
            // to fill one that does not leave it in CopyDest, which a structure
            // build may not read -- so the slack a sliding window needs has to
            // exist here. The high-water mark drifts up a little every crossing
            // (a chunk arriving is never exactly the size of the one that
            // left), and a buffer sized to today's mark cannot hold tomorrow's,
            // so it would be remade every time and the whole incremental path
            // would never run. Grown only, never shrunk.
            if (p.brick.size() < cache->bricks.top) {
                const size_t cap = size_t(cache->bricks.top) + cache->bricks.top / 2 + 1024;
                p.brick.resize(cap);
                p.aabb.resize(cap * 6);
            }
            if (p.aabb.size() < p.brick.size() * 6) p.aabb.resize(p.brick.size() * 6);
            if (p.mask.size() < cache->masks.top)
                p.mask.resize(size_t(cache->masks.top) + cache->masks.top / 2 + 4096);
            if (fresh || !incremental) {
                std::copy(out.brick.begin(), out.brick.end(), p.brick.begin() + bFirst);
                std::copy(out.aabb.begin(), out.aabb.end(), p.aabb.begin() + size_t(bFirst) * 6);
                std::copy(out.mask.begin(), out.mask.end(), p.mask.begin() + mFirst);
                p.dirtyBricks.push_back({bFirst, uint32_t(out.brick.size())});
                if (!out.mask.empty()) p.dirtyMask.push_back({mFirst, uint32_t(out.mask.size())});
                p.dirtyChunks.push_back(uint32_t(p.chunk.size()));
            }
        }

        // ...and the base comes straight back off, so what the cache holds is
        // always CHUNK-relative and can be placed at any offset next time.
        for (V4Brick &h : out.brick) {
            if ((h.flags & 1u) != 0u) continue;
            h.maskBase -= mFirst;
            if ((h.flags & 2u) == 0u) h.mtlBase -= mFirst * 4u;
        }
        if (cache && fresh) {
            out.probe = byChunk.at(ck).front();  // see PackedChunk::probe
            cache->chunk[ck] = std::move(outs[ci]);
        }
        boxVox += out.boxVox;
        p.uniform += out.uniform;
        p.merged += out.merged;
        p.detail += out.detail;
        p.mono += out.mono;
        p.chunk.push_back(c);
    }
    // EVICT WHAT IS NO LONGER IN THE WORLD, or the cache grows without bound as
    // the window travels -- it would hold every chunk ever visited.
    if (cache) {
        std::unordered_set<uint64_t> live(chunkKeys.begin(), chunkKeys.end());
        for (auto it = cache->chunk.begin(); it != cache->chunk.end();)
            it = live.count(it->first) ? std::next(it) : cache->chunk.erase(it);
    }
    if (!p.brick.empty())
        p.fill = double(boxVox) / (double(p.brick.size()) * double(BRICK_VOX));
    p.assembleMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tAsm0)
            .count();
}

// ---------------------------------------------------------------------------
// THE DEVICE WALK'S MATERIAL LOOKUP, IN C++. Used only by the test harness --
// but it lives here, beside the packer, because its whole value is that it is
// the same arithmetic written from the other side. A copy that drifts proves
// nothing, so if this ever disagrees with stores/Aabb.slang one of them is the
// bug and the test says which voxel.
// ---------------------------------------------------------------------------
inline uint8_t unpackVoxel(const PackedStore &p, uint32_t brickIdx, int lx, int ly, int lz) {
    const V4Brick &h = p.brick[brickIdx];
    if (h.flags & 1u) return uint8_t((h.flags >> 8) & 0xFFu);
    const int bit = Brick::bitOf(lx, ly, lz);
    const int word = bit >> 5;
    // WORD-SPARSE: a word that held nothing was never written, and a voxel in
    // one is air without a load. `octants` bits 8..23 say which survive.
    const uint32_t wordMap = h.octants >> 8;
    if (!((wordMap >> word) & 1u)) return mat::AIR;
    const uint32_t pos = popcount32(wordMap & ((1u << word) - 1u));
    if (!((p.mask[h.maskBase + pos] >> (bit & 31)) & 1u)) return mat::AIR;
    // Monochrome: it has a mask, but no run to index into.
    if (h.flags & 2u) return uint8_t((h.flags >> 8) & 0xFFu);
    // THE RANK IS UNCHANGED BY THE SPARSITY. Dropped words were all zero, so
    // they contributed nothing to the count of set bits before this voxel --
    // which is exactly why word-sparse was chosen over octant-sparse.
    uint32_t before = 0;
    for (uint32_t k = 0; k < pos; ++k) before += popcount32(p.mask[h.maskBase + k]);
    before += popcount32(p.mask[h.maskBase + pos] &
                         ((bit & 31) ? ((1u << (bit & 31)) - 1u) : 0u));
    // The per-brick palette: N bytes at mtlBase, then `bits`-wide indices.
    // N == 0 means the palette overflowed and this brick wrote RAW bytes.
    const uint32_t np = (h.flags >> 16) & 0x3Fu;
    if (np == 0u) {
        const uint32_t raw = h.mtlBase + before;
        return uint8_t((p.mask[raw >> 2] >> ((raw & 3u) * 8u)) & 0xFFu);
    }
    uint32_t bits = 1;
    while ((1u << bits) < np) ++bits;
    // THE INDICES START ON A WORD BOUNDARY, NOT AT mtlBase + np. The palette is
    // flushed a word at a time, so N bytes occupy ceil(N/4) WORDS and the packed
    // run begins after them. Reading from byte mtlBase+np instead is off by up
    // to three bytes whenever N is not a multiple of four -- which decodes a
    // neighbouring voxel's index, or runs off the palette and reads air.
    const uint32_t palWords = (np + 3u) >> 2;
    const uint64_t absBit =
        (uint64_t((h.mtlBase >> 2) + palWords) << 5) + uint64_t(before) * bits;
    const uint32_t w0 = uint32_t(absBit >> 5), sh = uint32_t(absBit & 31u);
    uint64_t v = uint64_t(p.mask[w0]) >> sh;
    if (sh + bits > 32u) v |= uint64_t(p.mask[w0 + 1]) << (32u - sh);
    const uint32_t pi = uint32_t(v & ((1ull << bits) - 1ull));
    const uint32_t idx = h.mtlBase + pi;  // BYTE index into the SAME buffer
    return uint8_t((p.mask[idx >> 2] >> ((idx & 3u) * 8u)) & 0xFFu);
}

}  // namespace v4
