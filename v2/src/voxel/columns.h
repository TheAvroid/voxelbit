// ---------------------------------------------------------------------------
// columns.h -- v2's world, presented to the new mesher as bitmasks.
//
// This is the adapter that makes the rewrite affordable, and it rests on one
// observation:
//
//     A PROCEDURAL TERRAIN COLUMN OF SURFACE HEIGHT h IS THE BITMASK
//     (1 << (h + 1)) - 1, AND IT COSTS NOTHING TO STORE BECAUSE IT IS NOT
//     STORED.
//
// Omar Owis' engine keeps real occupancy bits for every chunk, which is right
// for a world of a few hundred metres. voxelbit is 10 cm voxels over a 300 m
// view -- 2 x 10^11 voxels -- and no chunk format makes that dense. v2 only
// ever got away with it by keeping the world a FUNCTION of (i, j), and that
// property is preserved here rather than traded away.
//
// So a brick's 4,096 columns are computed, not fetched, and the binary mesher
// cannot tell the difference. Only ONE thing ever becomes real bits: what
// somebody has dug or built, which is v2's edit layer, unchanged.
//
// ---------------------------------------------------------------------------
// WHAT THIS FILE DOES *NOT* DO.
//
// It does not decide shades. materialAt and topMaterial return material
// FAMILIES -- kGrass0, kSoil0, mat::ROCK -- and groundShade() in the tracer
// picks the exact one per voxel by hashing the voxel coordinate. Returning a
// final shade here would split every merged quad along a colour boundary and
// cost the 2.2x voxelworld.h measured. See the note in mesher.h.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../scene/voxelworld.h"
#include "quad.h"

namespace v2 {
namespace vox {

// The padded span the mesher asks for: -1 .. BRICK_VOX inclusive.
constexpr int PAD = BRICK_VOX + 2;

inline int padIdx(int lx, int lz) { return (lz + 1) * PAD + (lx + 1); }

// ...AND THE HEIGHTS NEED A RING BEYOND THAT ONE.
//
// A surface material is chosen partly from the SLOPE, which reads the height
// one column each way. The mesher asks for materials over the padded span
// (-1 .. 64), so the heights behind them have to reach -2 .. 65. One extra ring
// of heights, 6% more gather, and the alternative is a grass blade on the edge
// column of a brick not knowing whether its neighbour has one -- which shows up
// as a seam of doubled blade walls at every brick boundary.
constexpr int PADH = BRICK_VOX + 4;

inline int padIdxH(int lx, int lz) { return (lz + 2) * PADH + (lx + 2); }

// ---------------------------------------------------------------------------
// The heights and surface materials one brick column-stack needs, gathered
// once and then read without touching the noise field again.
//
// ONE GATHER PER BRICK COLUMN-STACK, NOT PER BRICK. The heightfield does not
// depend on y, so the four bricks stacked above one another in a chunk share
// every height and every top material. Gathering per brick would evaluate the
// same 4,356 columns of fractal noise four times over.
//
// THE MEMO COMES IN FROM THE CALLER for the reason ChunkScratch gives: it is a
// cache that validates itself, so carrying it across bricks is either a free
// hit at a shared edge or a miss that recomputes. It cannot be wrong.
// ---------------------------------------------------------------------------
struct ColumnStack {
    int bx = 0, bz = 0;              // brick column in X and Z
    int i0 = 0, j0 = 0;              // world voxel origin of the brick
    int32_t h[PADH * PADH];  // surface row, padded by TWO -- see padIdxH
    uint8_t top[PAD * PAD];  // top material, padded by one
    // HOW TALL A BLADE STANDS ON EACH COLUMN, 0 for none, padded by one. A
    // blade is hidden by its neighbour over the span they share, so the brick
    // next door has to be askable -- same reason the heights are padded.
    uint8_t sr[PAD * PAD];

    // WHERE A LAKE MAY STAND OVER EACH COLUMN, padded like the heights and for
    // the same reason: the brick next door needs it to decide whether its own
    // water is covered.
    //
    // int16 because a waterline is a few hundred voxels and the array is
    // otherwise 17 KB a stack for a field that is constant across most of a
    // brick. kDryLine is the sentinel; it is NOT VoxelTerrain::kNoWaterVox,
    // which does not fit in sixteen bits -- that is exactly the overflow the
    // int version of this sentinel exists to avoid.
    static constexpr int16_t kDryLine = -32768;
    // PADDED BY TWO, like the heights and for a new reason: the depth rule
    // asks whether a FRINGE column's neighbour is properly wet, so a column
    // on the padded edge needs a line one further out than itself.
    int16_t wy[PADH * PADH];

    // WET, RESOLVED ONCE. VoxelTerrain::wetColumn is the same rule for a
    // caller with no array; this is the pass form, and the parity test holds
    // the two together.
    uint8_t wet[PAD * PAD];
    // The wave crest over each column, in voxels. Padded like the line: the
    // brick next door needs it to know whether its own water is covered.
    uint8_t cr[PAD * PAD];
    // The wave phase these crests were computed at. A gather is expensive and
    // the heights do not move, so an animated swell must NOT re-gather -- it
    // recomputes this one array, which is two sines a column.
    float crestTime = 0.0f;

    void gather(const VoxelTerrain &t, TerrainMemo &memo, int bxIn, int bzIn) {
        bx = bxIn;
        bz = bzIn;
        i0 = bx * BRICK_VOX;
        j0 = bz * BRICK_VOX;

        for (int lz = -2; lz <= BRICK_VOX + 1; ++lz)
            for (int lx = -2; lx <= BRICK_VOX + 1; ++lx) {
                h[padIdxH(lx, lz)] = t.heightVox(i0 + lx, j0 + lz, memo);
                // THE LINE OVER THE SAME SPAN AS THE HEIGHTS. Band only now --
                // one comparison per column and no basin field at all.
                const int line = t.lakeLineAt(t.wx(i0 + lx), t.wx(j0 + lz), memo);
                wy[padIdxH(lx, lz)] =
                    (line == VoxelTerrain::kNoWaterVox) ? kDryLine : int16_t(line);
                if (lx < -1 || lx > BRICK_VOX || lz < -1 || lz > BRICK_VOX) continue;
                cr[padIdx(lx, lz)] = uint8_t(t.waveCeilVox());
            }

        // ------------------------------------------------------------------
        // THE DEPTH RULE, AS A PASS. See VoxelTerrain::wetColumn -- deep water
        // is wet on its own, and a column one voxel shallower is wet only if a
        // neighbour is PROPERLY wet.
        //
        // ONE PASS, NOT v4'S TWO, and it is the same answer: the neighbour
        // test reads deepWet, which depends on nothing but that neighbour's
        // own height and line, so there is no order for it to cascade through.
        // v4 needs two passes because it writes its upgrade into the array it
        // is testing.
        // ------------------------------------------------------------------
        for (int lz = -1; lz <= BRICK_VOX; ++lz)
            for (int lx = -1; lx <= BRICK_VOX; ++lx) {
                const int hc = h[padIdxH(lx, lz)], ln = lineV(lx, lz);
                bool w = t.deepWet(hc, ln);
                if (!w && t.fringeWet(hc, ln))
                    w = t.deepWet(h[padIdxH(lx - 1, lz)], lineV(lx - 1, lz)) ||
                        t.deepWet(h[padIdxH(lx + 1, lz)], lineV(lx + 1, lz)) ||
                        t.deepWet(h[padIdxH(lx, lz - 1)], lineV(lx, lz - 1)) ||
                        t.deepWet(h[padIdxH(lx, lz + 1)], lineV(lx, lz + 1));
                wet[padIdx(lx, lz)] = w ? 1u : 0u;
            }

        // OVER THE PADDED SPAN, not just the interior. The slope each material
        // is chosen from reaches one column past it, which is what the second
        // ring of heights above is for.
        for (int lz = -1; lz <= BRICK_VOX; ++lz)
            for (int lx = -1; lx <= BRICK_VOX; ++lx) {
                const int hc = h[padIdxH(lx, lz)];
                const int slope =
                    std::max(std::abs(h[padIdxH(lx + 1, lz)] - h[padIdxH(lx - 1, lz)]),
                             std::abs(h[padIdxH(lx, lz + 1)] - h[padIdxH(lx, lz - 1)]));
                const uint8_t tm = t.topMaterial(i0 + lx, j0 + lz, hc, slope, memo);
                top[padIdx(lx, lz)] = tm;
                sr[padIdx(lx, lz)] = t.strandRows(i0 + lx, j0 + lz, tm);
            }

        crestTime = t.waveTime;
        computeBounds();
    }

    // THE CRESTS ALONE, at a new phase. Everything else in the stack is a
    // function of (x, z) and does not move; only the wave does. 4,356 columns
    // of two sines against a gather of six fbm fields each -- which is the
    // whole reason an animated lake is affordable.
    void refreshCrest(const VoxelTerrain &t) {
        if (crestTime == t.waveTime) return;
        crestTime = t.waveTime;
        for (int lz = -1; lz <= BRICK_VOX; ++lz)
            for (int lx = -1; lx <= BRICK_VOX; ++lx)
                cr[padIdx(lx, lz)] = uint8_t(t.waveCeilVox());
        computeBounds();  // the lid raises topMax
    }

    int heightAt(int lx, int lz) const { return h[padIdxH(lx, lz)]; }
    int rowsAt(int lx, int lz) const { return sr[padIdx(lx, lz)]; }
    int lineAt(int lx, int lz) const { return wy[padIdxH(lx, lz)]; }
    int crestAt(int lx, int lz) const { return cr[padIdx(lx, lz)]; }
    // The line in the TERRAIN's domain rather than the stack's int16 one --
    // kDryLine becomes kNoWaterVox, which is what every VoxelTerrain test
    // expects. The two sentinels differ because kNoWaterVox does not fit in
    // sixteen bits; see the note over wy.
    int lineV(int lx, int lz) const {
        const int16_t l = wy[padIdxH(lx, lz)];
        return (l == kDryLine) ? VoxelTerrain::kNoWaterVox : int(l);
    }
    bool wetAt(int lx, int lz) const { return wet[padIdx(lx, lz)] != 0u; }
    uint8_t topAt(int lx, int lz) const { return top[padIdx(lx, lz)]; }

    // -----------------------------------------------------------------------
    // WHICH BRICKS IN THE STACK CAN POSSIBLY HOLD A SURFACE.
    //
    // This is the single most valuable thing in the file after the bitmask
    // itself. The terrain here runs from about -1 m to 94 m, which is fifteen
    // bricks of vertical span -- but a column of world only has a SURFACE in
    // one or two of them. The rest are either entirely underground (every
    // voxel solid, every neighbour solid, not one face) or entirely open air.
    //
    // v2 had the same idea for its blocks and voxelworld.h states the rule:
    // skip "an EMPTY one, and a FULL one". What is new is that a brick is
    // small enough for the test to almost always succeed -- across a 256 m
    // chunk the heights vary far more than across 6.4 m, so v2's coarser
    // blocks were much likelier to straddle the surface and be walked.
    //
    // The min and max are over the PADDED range, which matters: a brick whose
    // own columns are all solid still shows a face if the column one outside
    // it is not.
    // -----------------------------------------------------------------------
    int hMin = 0, hMax = 0;
    // THE TOP OF ANYTHING IN THIS STACK, water included. A lake stands ABOVE
    // the ground it covers, so a brick holding nothing but water sits entirely
    // over hMax and the surface test would throw it away -- the lake would
    // exist to the collider and to every query while being invisible. Same
    // failure the buried-shaft case has, same fix.
    int topMax = 0;

    bool brickHasSurface(int by) const {
        const int y0 = by * BRICK_VOX, y1 = y0 + BRICK_VOX - 1;
        if (hMin >= y1) return false;                          // solid through
        if (topMax < y0) return false;                         // open air
        return true;
    }

    // The inclusive range of bricks worth meshing in this stack.
    void brickRange(int *byLo, int *byHi) const {
        *byLo = (hMin + 1) / BRICK_VOX - 1;
        *byHi = topMax / BRICK_VOX + 1;
    }

    void computeBounds() {
        hMin = h[padIdxH(0, 0)];
        hMax = hMin;
        for (int k = 0; k < PADH * PADH; ++k) {
            hMin = std::min<int>(hMin, h[k]);
            hMax = std::max<int>(hMax, h[k]);
        }
        // ANYTHING THAT STANDS ABOVE THE GROUND RAISES THE CEILING, and both
        // things that do are easy to forget: a lake stands above the bed it
        // covers, and a blade stands above the soil it grows in. Either one in a
        // brick the surface test has thrown away is geometry that exists to
        // every query while being invisible.
        topMax = hMax;
        for (int lz = -1; lz <= BRICK_VOX; ++lz)
            for (int lx = -1; lx <= BRICK_VOX; ++lx) {
                const int k = padIdx(lx, lz), hc = h[padIdxH(lx, lz)];
                if (wet[k]) topMax = std::max<int>(topMax, lineV(lx, lz) + cr[k]);
                if (sr[k]) topMax = std::max<int>(topMax, hc + sr[k]);
            }
    }
};

// ---------------------------------------------------------------------------
// The ground of one brick, as a ColumnSource.
//
// EDITS TAKE THE SLOW PATH AND NOTHING ELSE DOES. ChunkEdits already carries
// the set of columns somebody has touched; a column not in that set is pure
// heightfield and answers in three instructions. A column in it is walked
// voxel by voxel over the span the edit set records. That is v2's own rule
// ("meshChunk voxel-meshes exactly these and leaves every other column on the
// fast heightmap path") and it is the reason an untouched world costs nothing.
// ---------------------------------------------------------------------------
class TerrainColumns {
  public:
    TerrainColumns(const VoxelTerrain &t, const ColumnStack &stack, const ChunkEdits *edits,
                   int by)
        : t_(t), s_(stack), ed_(edits), by_(by), y0_(by * BRICK_VOX) {}

    // --- the ColumnSource interface ---

    Column column(int lx, int lz) const {
        const int hc = s_.heightAt(lx, lz);
        Column c = columnUpTo(hc + 1 - y0_);
        if (ed_) applyEdits(lx, lz, &c);
        return c;
    }

    // WATER IS AIR TO THE GROUND, and that is the whole reason this is a
    // separate question from column(). A lake bed is ground with water on top
    // of it; if water counted as cover, the bed would emit no upward face and
    // the bottom of every lake would be a hole you could see the sky through.
    // The bed is meant to be visible THROUGH the water, so the terrain pass
    // treats the water as if it were not there -- which, for the purpose of
    // deciding whether the ground has a surface, it is not.
    Column occluder(int lx, int lz) const { return column(lx, lz); }

    bool solidAbove(int lx, int lz) const { return voxelSolid(lx, BRICK_VOX, lz); }
    bool solidBelow(int lx, int lz) const { return voxelSolid(lx, -1, lz); }

    // -----------------------------------------------------------------------
    // WHICH VOXELS OF THIS COLUMN AN EDIT OWNS, set or cleared.
    //
    // AN EDITED VOXEL BELONGS TO THE EDIT AND TO NOTHING ELSE. The ground pass
    // has always honoured that -- applyEdits below both sets and clears -- but
    // the layers standing over it did not, and the two readers of the world
    // then disagreed about a dug lake:
    //
    //   dig into a lake bed and the carve turns bed voxels to air. The water
    //   span still starts one above the PROCEDURAL surface, which the dig did
    //   not move, so the mesher went on painting water through the hole while
    //   TerrainProbe -- which checks edits first -- correctly called it air.
    //   71 faces of one lake chunk, found by tests/voxel_probe_test.cpp.
    //
    // Returning the mask rather than applying it lets each layer decide: the
    // ground OVERLAYS edits (a placed block becomes ground), while water and
    // grass are simply ABSENT wherever an edit has claimed a voxel. Neither
    // gets to invent an answer the probe would contradict.
    // -----------------------------------------------------------------------
    Column editedMask(int lx, int lz) const {
        if (!ed_) return COL_EMPTY;
        const int wi = s_.i0 + lx, wj = s_.j0 + lz;
        const auto it = ed_->col.find(ChunkEdits::ckey(wi, wj));
        if (it == ed_->col.end()) return COL_EMPTY;
        Column m = COL_EMPTY;
        const int lo = std::max(it->second.first, y0_);
        const int hi = std::min(it->second.second, y0_ + BRICK_VOX - 1);
        for (int wy = lo; wy <= hi; ++wy) {
            uint8_t v = 0;
            if (ed_->voxel(wi, wj, wy, &v)) m |= Column(1) << (wy - y0_);
        }
        return m;
    }

    uint8_t material(int lx, int ly, int lz) const {
        const int wy = y0_ + ly;
        if (ed_) {
            uint8_t m = 0;
            if (ed_->voxel(s_.i0 + lx, s_.j0 + lz, wy, &m)) return m;
        }
        return t_.materialAt(s_.i0 + lx, s_.j0 + lz, wy, s_.heightAt(lx, lz), s_.topAt(lx, lz));
    }

    // Grass blades are scattered decoration rather than terrain and are not
    // meshed from the column store; they arrive as their own bricks. Kept on
    // the interface so the strand code has somewhere to live when they do.
    uint8_t strand(int, int, int) const { return 0; }

  private:
    // One voxel, including the row above and below the brick -- which is what
    // the vertical face masks need and is why ly may be -1 or BRICK_VOX.
    bool voxelSolid(int lx, int ly, int lz) const {
        const int wy = y0_ + ly;
        if (ed_) {
            uint8_t m = 0;
            if (ed_->voxel(s_.i0 + lx, s_.j0 + lz, wy, &m)) return m != mat::AIR;
        }
        return wy <= s_.heightAt(lx, lz);
    }

    // Overlay the edit set onto a procedural column. Only reached for columns
    // the edit set names, and only over the y span it recorded.
    void applyEdits(int lx, int lz, Column *c) const {
        const int wi = s_.i0 + lx, wj = s_.j0 + lz;
        const auto it = ed_->col.find(ChunkEdits::ckey(wi, wj));
        if (it == ed_->col.end()) return;
        const int lo = std::max(it->second.first, y0_);
        const int hi = std::min(it->second.second, y0_ + BRICK_VOX - 1);
        for (int wy = lo; wy <= hi; ++wy) {
            uint8_t m = 0;
            if (!ed_->voxel(wi, wj, wy, &m)) continue;
            const Column bit = Column(1) << (wy - y0_);
            if (m == mat::AIR) *c &= ~bit;
            else *c |= bit;
        }
    }

    const VoxelTerrain &t_;
    const ColumnStack &s_;
    const ChunkEdits *ed_;
    int by_, y0_;
};

// ---------------------------------------------------------------------------
// The water of one brick, as a ColumnSource.
//
// A SECOND PASS OVER THE SAME BRICK, not a second kind of voxel in the first
// one. The mesher's whole method is that a column is a uint64 of occupancy,
// and occupancy is one bit -- there is no room in it for "solid, but you can
// see through it". Trying to carry both in one mask is what makes a lake come
// out as either a dark mirror with no bed under it or a bed with no surface
// over it, depending on which way the bit is read.
//
// So the two are meshed separately and the pair of questions the source
// answers is what keeps them consistent:
//
//   column()    the water itself -- bed + 1 up to the line.
//   occluder()  water OR ground. Water shows a face only where it meets AIR,
//               so it has no skin inside its own bed and none against the bank
//               it laps at. Its top face, and the sides where the lake ends in
//               open air, are all that survive.
//
// THE GROUND PASS USES THE SAME BRICK AND DOES NOT KNOW ABOUT ANY OF THIS. It
// meshes with water reading as air, so the bed keeps its upward faces and the
// bank keeps its sides, exactly as if the lake were not there.
//
// WHAT THIS DELIBERATELY DOES NOT DO: flow. A hole dug in a lake bed does not
// fill, and a wall built across a lake does not part it -- the water is a
// function of the landform, like the ground it sits on, and an edit that
// changes the ground does not move the line. Making water settle is a
// simulation and belongs nowhere near a mesher.
// ---------------------------------------------------------------------------
class WaterColumns {
  public:
    WaterColumns(const VoxelTerrain &t, const TerrainColumns &ground, const ColumnStack &stack,
                 int by)
        : t_(t), g_(ground), s_(stack), y0_(by * BRICK_VOX) {}

    Column column(int lx, int lz) const {
        // THE SPAN COMES FROM VoxelTerrain, not from a second copy of the rule
        // here -- see the note over waterSpan.
        const int hc = s_.heightAt(lx, lz);
        const int wl = s_.lineV(lx, lz);
        // THE SHORE BAND STANDS PROUD, and it is lifted HERE -- in the geometry,
        // before anything intersects it. v1 is emphatic about why: raising a hit
        // that has already been found only moves that pixel's depth, and "the
        // foam kept the silhouette of the flat water because the pixels it
        // should have grown into were never tested against the water at all".
        const int lo = hc + 1;
        const int hi = t_.waterTopVox(hc, wl, s_.crestAt(lx, lz), s_.wetAt(lx, lz));
        // An edit owns its voxel -- see TerrainColumns::editedMask. Water does
        // not flow into a hole, and it does not paint over one either.
        return columnRange(lo - y0_, hi - y0_) & ~g_.editedMask(lx, lz);
    }

    Column occluder(int lx, int lz) const { return column(lx, lz) | g_.column(lx, lz); }

    bool solidAbove(int lx, int lz) const { return covered(lx, BRICK_VOX, lz); }
    bool solidBelow(int lx, int lz) const { return covered(lx, -1, lz); }

    // THE BAND IS ITS OWN MATERIAL, so the merge splits at its edge and the
    // shader needs no neighbour test to find it.
    uint8_t material(int lx, int, int lz) const {
        return t_.foamColumn(s_.heightAt(lx, lz), s_.lineV(lx, lz)) ? mat::FOAM : mat::WATER;
    }
    uint8_t strand(int, int, int) const { return 0; }

    // Whether this brick holds any water at all, so the store can skip the
    // whole second pass -- which is the overwhelmingly common case, because
    // under one per cent of the world is wet.
    bool any() const {
        for (int lz = 0; lz < BRICK_VOX; ++lz)
            for (int lx = 0; lx < BRICK_VOX; ++lx)
                if (column(lx, lz) != COL_EMPTY) return true;
        return false;
    }

  private:
    // One voxel above or below the brick: is it water or ground? Either hides
    // the water's face, which is why this asks the union and not the water.
    bool covered(int lx, int ly, int lz) const {
        const int wy = y0_ + ly;
        const int hc = s_.heightAt(lx, lz);
        if (wy <= hc) return true;  // ground
        return wy <= t_.waterTopVox(hc, s_.lineV(lx, lz), s_.crestAt(lx, lz), s_.wetAt(lx, lz));
    }

    const VoxelTerrain &t_;
    const TerrainColumns &g_;
    const ColumnStack &s_;
    int y0_;
};

// ---------------------------------------------------------------------------
// The grass of one brick, as a ColumnSource.
//
// STRUCTURALLY THE SAME TRICK AS WaterColumns, and for the same reason: a blade
// is solid but must not hide the ground it stands on. The soil under a blade
// keeps its upward face -- meshChunk has always drawn it, and a brick path that
// stopped drawing it would punch a 10 cm hole in the floor beside every tuft.
//
//   column()    the blade -- bed + 1 up to bed + rows.
//   occluder()  blade OR ground, so a blade shows no face where it is buried in
//               the neighbouring hillside and no BOTTOM face at all, its base
//               sitting on the soil. Both match meshChunk exactly.
//
// AND THE GROUND PASS TREATS A BLADE AS AIR, so the floor is meshed as though
// the grass were not there -- which is what keeps the two paths agreeing.
//
// THE STRAND CODE IS THE WHOLE POINT OF strand(). Every face of a blade carries
// the row its base stands on; the device turns height above that row into a
// shade, dark at the soil and light at the tip. mergePlane splits on it, so two
// blades on different rows never share a quad and never share a gradient.
//
// A COLUMN ANYBODY HAS EDITED GROWS NOTHING, which is meshChunk's rule (`ED(i,j)
// ? 0 : SR(i,j)`) and is not cosmetic: a blade is placed from a hash on the
// column, not from the ground, so it would otherwise stand in the air over a
// hole somebody had just dug under it.
// ---------------------------------------------------------------------------
class StrandColumns {
  public:
    StrandColumns(const TerrainColumns &ground, const ColumnStack &stack, const ChunkEdits *edits,
                  int by)
        : g_(ground), s_(stack), ed_(edits), y0_(by * BRICK_VOX) {}

    Column column(int lx, int lz) const {
        // Same span the probe answers from -- see VoxelTerrain::bladeSpan.
        int lo = 0, hi = 0;
        VoxelTerrain::bladeSpan(s_.heightAt(lx, lz), rowsAt(lx, lz), &lo, &hi);
        return columnRange(lo - y0_, hi - y0_);
    }

    Column occluder(int lx, int lz) const { return column(lx, lz) | g_.column(lx, lz); }

    bool solidAbove(int lx, int lz) const { return covered(lx, BRICK_VOX, lz); }
    bool solidBelow(int lx, int lz) const { return covered(lx, -1, lz); }

    uint8_t material(int lx, int, int lz) const { return s_.topAt(lx, lz); }

    uint8_t strand(int lx, int, int lz) const {
        return strandCodeFor(s_.heightAt(lx, lz) + 1);
    }

    bool any() const {
        for (int lz = 0; lz < BRICK_VOX; ++lz)
            for (int lx = 0; lx < BRICK_VOX; ++lx)
                if (column(lx, lz) != COL_EMPTY) return true;
        return false;
    }

  private:
    int rowsAt(int lx, int lz) const {
        if (ed_ && editedNear(lx, lz)) return 0;
        return s_.rowsAt(lx, lz);
    }

    // meshChunk marks a column slow if an edit lands within one of it, and
    // clears the grass over that whole 3x3 -- matched here rather than
    // approximated, because a mismatch is a blade that one mesher draws and the
    // other does not.
    bool editedNear(int lx, int lz) const {
        const int wi = s_.i0 + lx, wj = s_.j0 + lz;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                int lo = 0, hi = 0;
                if (ed_->column(wi + di, wj + dj, &lo, &hi)) return true;
            }
        return false;
    }

    bool covered(int lx, int ly, int lz) const {
        const int wy = y0_ + ly;
        const int hc = s_.heightAt(lx, lz);
        if (wy <= hc) return true;  // the soil it stands on
        const int rows = rowsAt(lx, lz);
        return rows > 0 && wy <= hc + rows;
    }

    const TerrainColumns &g_;
    const ColumnStack &s_;
    const ChunkEdits *ed_;
    int y0_;
};

}  // namespace vox
}  // namespace v2
