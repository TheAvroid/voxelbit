// cover.h -- what the ground actually IS, from aerial imagery.
//
// Loads a .vbcov (tools/naip2cov.py builds them out of USGS NAIP) and answers
// one question: at this world point, is it forest, meadow, rock, snow or water.
// One byte per sample, on exactly the same grid as the .vbdem beside it, so the
// two are indexed by the same arithmetic and cannot drift apart.
//
// THIS IS NOT COLOUR, AND THAT IS DELIBERATE. v2 has no material textures -- a
// 255-entry palette and a per-voxel hash, with the palette already at 232 -- so
// draping a photograph is not on the table. What the imagery is used for is
// PLACEMENT: where the trees are, where the bare rock is, where the meadows
// open up. That costs no palette entries and is the more useful half of the
// picture anyway.
//
// IT DOES NOT REPLACE THE TIMBERLINE, it works with it. The classifier reads
// alpine tundra at 4,000 m as vegetated and dark, which is true, and calls it
// forest, which is not -- telling a krummholz mat from a spruce needs texture,
// not colour. So cover decides the HORIZONTAL pattern (the meadows, the rock
// outcrops, the drainages, the ragged real edge of a stand) and the altitude
// timberline still caps it vertically. Each one covers the other's weakness.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

// A LOCAL VALUE NOISE, so this header keeps its only dependency the standard
// library. core/noise.h has a better one, but it lives in namespace v2 and
// drags vecmath in behind it, and all that is wanted here is a smooth wobble
// to push a waterline off the grid.
inline float chash2(int32_t x, int32_t z) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) * 2.3283064e-10f;
}
inline float vnoise2(float x, float z) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int32_t ix = int32_t(fx), iz = int32_t(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float a = chash2(ix, iz), b = chash2(ix + 1, iz);
    const float c = chash2(ix, iz + 1), d = chash2(ix + 1, iz + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

class CoverField {
  public:
    enum Class : uint8_t {
        Unknown = 0, Forest = 1, Meadow = 2, Rock = 3, Snow = 4, Water = 5
    };

    // Ten ground colours, taken from the window's own pixels. Ten because that
    // is the size of the BWHEAT ramp, which the birch wood owned and which
    // nothing reaches while the world is pine.
    static constexpr int kRamp = 10;

    #pragma pack(push, 1)
    struct Header {
        char     magic[8];          // "VBCOV03"
        int32_t  w, h;
        double   originLon, originLat;
        double   stepLon, stepLat;
        double   metresPerSampleX, metresPerSampleY;
        int32_t  rampCount;
        uint8_t  ramp[kRamp * 3];   // rgb, most common bare ground first
        int32_t  pad[28];
    };
    #pragma pack(pop)

    // shrink must match the DemField's, or the trees stand where the ground
    // is not. It is passed in rather than read from the file for exactly that
    // reason: there is one scale in the world and it belongs to the terrain.
    bool load(const std::string &path, float shrink = 1.0f) {
        ok_ = false;
        shrink_ = (shrink > 0.01f) ? shrink : 1.0f;
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) { snprintf(err_, sizeof err_, "cannot open %s", path.c_str()); return false; }
        Header hd{};
        if (fread(&hd, sizeof hd, 1, f) != 1) { fclose(f); snprintf(err_, sizeof err_, "short header"); return false; }
        if (memcmp(hd.magic, "VBCOV03", 7) != 0) { fclose(f); snprintf(err_, sizeof err_, "bad magic (want VBCOV02; rebuild with tools/naip2cov.py)"); return false; }
        if (hd.w <= 1 || hd.h <= 1) { fclose(f); snprintf(err_, sizeof err_, "degenerate grid"); return false; }
        g_.resize((size_t)hd.w * hd.h);
        const size_t got = fread(g_.data(), 1, g_.size(), f);
        shore_.resize((size_t)hd.w * hd.h);
        const size_t got2 = fread(shore_.data(), 1, shore_.size(), f);
        fclose(f);
        if (got != g_.size() || got2 != shore_.size()) {
            snprintf(err_, sizeof err_, "short data"); return false;
        }
        w_ = hd.w; h_ = hd.h; mx_ = hd.metresPerSampleX; my_ = hd.metresPerSampleY;
        memcpy(ramp_, hd.ramp, sizeof ramp_);
        // -- WHAT THE GROUND IS BEYOND THE IMAGERY -- see raw() -----------
        //
        // The commonest RAW byte around the border ring, so the exterior is
        // continuous with the edge it leaves and keeps that class's colour
        // index as well as its class. Counted over the ring rather than the
        // whole raster: the middle of a window is not what its outside
        // adjoins.
        {
            int hist[256] = {0};
            for (int x = 0; x < w_; ++x) {
                ++hist[g_[size_t(x)]];
                ++hist[g_[size_t(h_ - 1) * size_t(w_) + size_t(x)]];
            }
            for (int y = 0; y < h_; ++y) {
                ++hist[g_[size_t(y) * size_t(w_)]];
                ++hist[g_[size_t(y) * size_t(w_) + size_t(w_ - 1)]];
            }
            int best = 0;
            for (int v = 1; v < 256; ++v)
                if (hist[v] > hist[best]) best = v;
            exterior_ = uint8_t(best);
        }
        ok_ = true;
        despeckleWater();
        return true;
    }

    bool ok() const { return ok_; }
    const char *err() const { return err_; }
    int w() const { return w_; }
    int h() const { return h_; }

    // ------------------------------- THE GRID ITSELF, WITH NOTHING DONE TO IT
    // raw()/at() below jitter the sample point on purpose, to break up the
    // 10.29 m posting so the cover does not draw as squares. That is right for
    // everything that ASKS ABOUT A POINT and wrong for anything that walks the
    // grid as a grid: a flood fill over a jittered sampler has neighbours that
    // are not neighbours, so scattered cells chain into one blob and a lake
    // comes out as a smear across a hillside (world/poi.h found exactly that).
    // This is the unjittered cell, for the walkers.
    uint8_t atGrid(int i, int j) const {
        if (!ok_) return Unknown;
        if (i < 0) i = 0; else if (i >= w_) i = w_ - 1;
        if (j < 0) j = 0; else if (j >= h_) j = h_ - 1;
        return uint8_t(g_[(size_t)j * w_ + i] >> 4);
    }

    // The same, addressed by a world point: the class actually recorded there,
    // with none of the jitter. Anything ASKING A QUESTION ABOUT THE DATA wants
    // this; anything DRAWING wants at(), because the jitter is what keeps the
    // posting from showing as squares.
    uint8_t atPoint(float x, float z) const {
        if (!ok_) return Unknown;
        return atGrid(int(std::lround(double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5)),
                      int(std::lround(double(z) * shrink_ / my_ + h_ * 0.5 - 0.5)));
    }

    // NEAREST, NEVER INTERPOLATED. These are labels, not measurements -- the
    // average of "rock" and "water" is not a thing, and half a class is a bug
    // that would show up as a band of the wrong material along every edge.
    // Outside the grid the border sample is held, matching DemField.
    uint8_t raw(float x, float z) const {
        if (!ok_) return Unknown;
        // ------------------------------------------------------ BREAK THE GRID
        // Nearest-neighbour on a 10.29 m posting draws the cover as flat,
        // axis-aligned tiles with hard edges -- "coloured squares everywhere",
        // and they show up on the class boundaries as much as the colour ones,
        // so the treeline squares off too.
        //
        // The fix cannot be interpolation: these are LABELS. Half of "rock" and
        // "water" is not a thing, and a blended colour index is a colour nobody
        // sampled. So the sample POINT is jittered instead, by up to a cell,
        // which leaves every answer a real class from a real posting while
        // making the boundary between two of them ragged.
        //
        // Hashed on the voxel column, so a given place answers the same way
        // every time it is asked. Get that wrong and the ground shimmers as
        // chunks reload, which reads as a renderer bug rather than a sampling
        // one. Voxel-resolution hashing also matches what the rest of the
        // engine does -- it is a palette and a per-voxel hash all the way down.
        // -- AND THE JITTER IS CAPPED IN WORLD METRES --------------------
        //
        // It is a fraction of a CELL, and a cell is mx_/shrink_ world metres --
        // 1.7 m at shrink 6, which is where the 1.4 was chosen, and 10.3 m at
        // 1:1. Uncapped, going to true scale turned a 2.4 m raggedness along
        // every cover boundary into a 14 m one: the treeline stopped being an
        // edge and became a scatter of trees strewn across the tundra, and the
        // ground colours smeared into each other over the same distance.
        //
        // 2.4 m IS THE NUMBER THAT WAS ALREADY WORKING -- one cell of the
        // shrunk world, kept as an absolute now that the cell is not. It is
        // about a tree's own width, which is the right scale for breaking up a
        // boundary between two kinds of ground.
        const float kMaxJitterM = 2.4f;
        const float cellX = std::min(kMaxJitterM, float(mx_ / shrink_));
        const float cellZ = std::min(kMaxJitterM, float(my_ / shrink_));
        uint32_t hv = uint32_t(int(std::floor(x / 0.1f))) * 73856093u ^
                      uint32_t(int(std::floor(z / 0.1f))) * 19349663u;
        hv ^= hv >> 13; hv *= 1274126177u; hv ^= hv >> 16;
        const float jx = (float(hv & 0xFFFFu) * (1.0f / 65535.0f) - 0.5f) * cellX * 1.4f;
        const float jz = (float((hv >> 16) & 0xFFFFu) * (1.0f / 65535.0f) - 0.5f) * cellZ * 1.4f;
        x += jx; z += jz;
        int i = int(std::lround(double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5));
        int j = int(std::lround(double(z) * shrink_ / my_ + h_ * 0.5 - 0.5));
        // -- OUTSIDE THE WINDOW IT IS ONE CLASS, NOT A ROW OF THEM --------
        //
        // (user 2026-09-19: "there are line artifacts in the terrain".)
        //
        // THE SAME FAULT THE HEIGHT HAD, one raster over -- see the note in
        // DemField::heightM. Clamping i alone leaves the class a function of
        // j, so past the east or west edge every ROW of the cover extends its
        // last sample outward for ever and the exterior is striped into
        // infinite bands. Measured at x = -4300: the class alternates 1, 3, 3,
        // 1, 3 ... along z and never changes along x.
        //
        // The height was fixed by fading to the border MEAN; a class cannot be
        // averaged, so the exterior takes ONE class outright -- the commonest
        // around the border ring. There is nothing left out there for a line
        // to be drawn with.
        //
        // NO FADE, AND NONE IS WANTED: this only bites where the clamp already
        // did, which is beyond the data, and the ground is sliding toward the
        // exterior plain over the same stretch. A dithered boundary was the
        // alternative and it buys a speckled band instead of a clean one.
        if (i < 0 || i >= w_ || j < 0 || j >= h_) return exterior_;
        return g_[(size_t)j * w_ + i];
    }

    // THE BYTE IS PACKED: class in the high nibble, ground-colour index in the
    // low one. Everything that used to read the byte as a class still can, as
    // long as it goes through here.
    // What the ground is once the imagery stops -- see raw(). One class for
    // the whole exterior, so the outside of the window cannot be striped.
    uint8_t exteriorRaw() const { return exterior_; }

    uint8_t at(float x, float z) const { return uint8_t(raw(x, z) >> 4); }
    int colourIndex(float x, float z) const { return int(raw(x, z) & 0x0F); }
    const uint8_t *ramp() const { return ramp_; }

    // Distance to the nearest open water, in world metres, or maxM if none is
    // found inside that. Rings outward and stops at the first hit, so a shore
    // costs a handful of lookups and open ground costs the full sweep.
    float waterDistance(float x, float z, float maxM) const {
        if (!ok_) return maxM;
        if (at(x, z) == Water) return 0.0f;
        const float step = float(mx_ / shrink_) * 2.0f;
        for (float r = step; r <= maxM; r += step) {
            const int n = std::max(8, int(6.2831853f * r / step));
            for (int k = 0; k < n; ++k) {
                const float a = float(k) * (6.2831853f / float(n));
                if (at(x + std::cos(a) * r, z + std::sin(a) * r) == Water) return r;
            }
        }
        return maxM;
    }

    // The nearest water's OFFSET, so a caller can face it. Same sweep as
    // waterDistance; returns false if there is none inside maxM.
    bool nearestWater(float x, float z, float maxM, float *dx, float *dz) const {
        if (!ok_) return false;
        const float step = float(mx_ / shrink_) * 2.0f;
        for (float r = step; r <= maxM; r += step) {
            const int n = std::max(8, int(6.2831853f * r / step));
            for (int k = 0; k < n; ++k) {
                const float a = float(k) * (6.2831853f / float(n));
                const float ox = std::cos(a) * r, oz = std::sin(a) * r;
                if (at(x + ox, z + oz) == Water) { *dx = ox; *dz = oz; return true; }
            }
        }
        return false;
    }

    // ------------------------------------- HOW FAR TO SHORE, IN REAL METRES
    // SIGNED: negative on land, positive in water, zero at the waterline. See
    // buildShoreField for why it has to be signed and what it was before.
    //
    // Read with BILINEAR interpolation -- which is the whole reason the lake
    // bed is smooth. The old version ringed outward at runtime until it left
    // the water: ~950 lookups inside heightM, the hottest function in the
    // engine, returning a value quantised to the cell AND wobbled by the
    // sampler's jitter. That was the hitching and the terraced bed in one.
    //
    // Interpolated, not nearest, and deliberately UNJITTERED: this is a
    // measurement, not a label, so averaging two neighbours is meaningful here
    // in a way it never is for a class.
    float shoreDistanceRaw(float x, float z) const {
        if (!ok_ || shore_.empty()) return 0.0f;
        const double gx = double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5;
        const double gy = double(z) * shrink_ / my_ + h_ * 0.5 - 0.5;
        int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
        float tx = float(gx - x0), ty = float(gy - y0);
        if (x0 < 0)       { x0 = 0;      tx = 0.0f; }
        if (x0 >= w_ - 1) { x0 = w_ - 2; tx = 1.0f; }
        if (y0 < 0)       { y0 = 0;      ty = 0.0f; }
        if (y0 >= h_ - 1) { y0 = h_ - 2; ty = 1.0f; }
        const uint8_t *r0 = &shore_[(size_t)y0 * w_ + x0];
        const uint8_t *r1 = r0 + w_;
        const float a = float(r0[0]) + (float(r0[1]) - float(r0[0])) * tx;
        const float b = float(r1[0]) + (float(r1[1]) - float(r1[0])) * tx;
        return (a + (b - a) * ty) - 128.0f;
    }

    // -- AND THE WATERLINE IS WARPED, BECAUSE SMOOTH IS NOT THE SAME AS FREE
    // -- OF THE GRID.
    //
    // Signing the field turns the staircase into a curve, but the curve still
    // has the raster's corners in it, rounded over one cell: a coastline made
    // of 1.7 m arcs, which from above still reads as a grid. No interpolation
    // can fix that, because the detail is not in the data -- a 10.29 m posting
    // is all there is, and a real shoreline has structure at every scale below
    // it.
    //
    // So the LOOKUP POSITION is warped by a little noise before the field is
    // read. That does not move the lake, it only decides which way the edge
    // wanders on its way round, which is the one thing about a coastline that
    // does not have to be measured to be right.
    //
    // IT FADES OUT WITHIN TWO CELLS OF THE WATERLINE, and that is not a detail
    // -- the same field is the lake bed's depth ramp (heightM multiplies it by
    // 0.45), so warping it in open water would put 4-voxel lumps across the
    // bottom. Near zero it is at full strength, by two cells it is off, and
    // the blend is continuous because the amplitude goes to zero before the
    // sample position does.
    static constexpr float kWarpM = 1.15f;       // world metres, ~0.7 of a cell
    float shoreDistance(float x, float z) const {
        const float raw = shoreDistanceRaw(x, z);
        const float band = 2.0f * float(mx_);    // real metres: two cells
        const float a = std::fabs(raw);
        if (a >= band) return raw;
        const float k = 1.0f - a / band;
        const float amp = kWarpM * k * k * (3.0f - 2.0f * k);
        const float wx = (vnoise2(x * 0.21f, z * 0.21f) - 0.5f) * 2.0f +
                         (vnoise2(x * 0.53f + 31.7f, z * 0.53f) - 0.5f) * 0.8f;
        const float wz = (vnoise2(x * 0.21f + 91.3f, z * 0.21f + 17.1f) - 0.5f) * 2.0f +
                         (vnoise2(x * 0.53f, z * 0.53f + 63.9f) - 0.5f) * 0.8f;
        return shoreDistanceRaw(x + amp * wx, z + amp * wz);
    }

    bool isForest(float x, float z) const { return at(x, z) == Forest; }

    // ------------------------------- IS THERE WATER HERE -- FOR GEOMETRY, NOT
    // FOR DRAWING, AND THE TWO ARE NOT THE SAME QUESTION.
    //
    // at() jitters its sample by up to a cell on purpose so the cover does not
    // draw as squares. That is right for a material or a tree site, where a
    // ragged boundary is the point, and WRONG for anything that decides shape:
    // a lake bed carved through the jittered class has columns inside the water
    // that took the dry branch, and each one stands up out of the lake. It was
    // 2.42% of all mapped water -- 35,495 columns, 20,489 of them breaking the
    // surface, the worst 22.7 m up -- and 94% of that was this sampler.
    //
    // THE ANSWER IS NOT THE RAW CELL EITHER. Nearest-neighbour on a 10.29 m
    // posting makes the shoreline an axis-aligned staircase, which is the same
    // complaint the jitter was added to fix.
    //
    // It reads the SHORE PLANE instead -- the baked chamfer distance, which is
    // zero on land, metres of water offshore, and bilinear on the way in. A
    // contour of a bilinear field is a smooth curve at any threshold, so the
    // waterline is neither ragged nor square, and it is the same answer every
    // time it is asked. The threshold is nearly zero because the bed's own
    // depth curve is 0.45 x this: at the contour the water is 7 cm deep, so
    // the bed meets the bank continuously rather than at a step.
    //
    // ANYTHING THAT SHAPES THE WORLD MUST USE THIS AND NOTHING ELSE. heightM
    // carves the bed, lakeLineAt puts the surface back, and topMaterial lays
    // the bed material -- three functions that have to agree about one edge.
    // ZERO, because the field is signed now: the waterline is where the
    // signed distance changes sign, not an offset into a one-sided jump.
    static constexpr float kShoreEdgeM = 0.0f;
    // ...and reading it back, in REAL metres. Zero on land and near the shore,
    // where the caller should be using the precise field anyway.
    float shoreDistanceFar(float x, float z) const {
        if (!ok_ || deep_.empty()) return 0.0f;
        const double gx = (double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5) / double(kDeepShift);
        const double gy = (double(z) * shrink_ / my_ + h_ * 0.5 - 0.5) / double(kDeepShift);
        int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
        float tx = float(gx - x0), ty = float(gy - y0);
        if (x0 < 0)            { x0 = 0;           tx = 0.0f; }
        if (x0 >= deepW_ - 1)  { x0 = deepW_ - 2;  tx = 1.0f; }
        if (y0 < 0)            { y0 = 0;           ty = 0.0f; }
        if (y0 >= deepH_ - 1)  { y0 = deepH_ - 2;  ty = 1.0f; }
        if (deepW_ < 2 || deepH_ < 2) return 0.0f;
        const uint8_t *r0 = &deep_[size_t(y0) * size_t(deepW_) + size_t(x0)];
        const uint8_t *r1 = r0 + deepW_;
        const float a = float(r0[0]) + (float(r0[1]) - float(r0[0])) * tx;
        const float b = float(r1[0]) + (float(r1[1]) - float(r1[0])) * tx;
        return (a + (b - a) * ty) * float(kDeepStepM);
    }

    bool waterHere(float x, float z) const {
        return ok() && !shore_.empty() && shoreDistance(x, z) > kShoreEdgeM;
    }

    // ------------------------ A RAMP ENTRY THE IMAGERY TOOK OFF THE WATER
    // The ground ramp is spread by luminance percentile over the window's own
    // pixels, and nothing stopped water pixels being in that population. In
    // rmnp50 exactly one entry came out of it: #384848, a desaturated teal at
    // the dark end, which then painted every flat beside the lake -- reported
    // as "this blue terrain near water".
    //
    // Blue-dominant is the test because NOTHING ELSE IN THE RAMP IS. The other
    // nine entries of that window run r > g > b by 16 to 64 points, which is
    // what soil, rock and dry grass all look like; open water is the only thing
    // in an aerial photograph of Colorado that is bluer than it is red.
    bool rampIsWater(int i) const {
        if (i < 0 || i >= kRamp) return false;
        const int r = ramp_[i * 3], g = ramp_[i * 3 + 1], b = ramp_[i * 3 + 2];
        return b >= g && b > r + 6;
    }

    // ------------------------------------------------- HOW CLOSED THE STAND IS
    // The fraction of a neighbourhood that is forest, 0..1. Sampled on two
    // rings rather than a solid disc: sixteen lookups describe a stand well
    // enough to tell a closed canopy from an edge, and this is asked once per
    // candidate tree.
    //
    // THE RADIUS IS STAND-SCALE, NOT CELL-SCALE. One cover cell is about 1.7
    // world metres at shrink 6; asking about a single cell would only ever
    // answer "forest" at a site the scatter already chose for being forest.
    // The rings sit at roughly 7 and 15 world metres, which is the scale a
    // thicket is distinguishable from a clearing edge at.
    float forestFraction(float x, float z) const {
        if (!ok_) return 0.0f;
        const float c = float(mx_ / shrink_);
        static const float kOff[16][2] = {
            { 4, 0}, {-4, 0}, {0,  4}, {0, -4}, { 3, 3}, {-3, 3}, {3, -3}, {-3, -3},
            { 9, 0}, {-9, 0}, {0,  9}, {0, -9}, { 6, 6}, {-6, 6}, {6, -6}, {-6, -6},
        };
        int n = 0;
        for (const auto &o : kOff)
            if (at(x + o[0] * c, z + o[1] * c) == Forest) ++n;
        return float(n) * (1.0f / 16.0f);
    }
    bool isBare(float x, float z) const {
        const uint8_t c = at(x, z);
        return c == Rock || c == Snow;
    }

    // -----------------------------------------------------------------------
    // THE BED WAS SQUARE BECAUSE THIS WAS A CHAMFER.
    //
    // (user 2026-09-19: "the terrain inside of water beds are very
    // square/rectangle".)
    //
    // WHAT WAS HERE was a 3-4 chamfer -- D1 = 10 for an orthogonal step, D2 =
    // 14 for a diagonal. It is the standard cheap distance transform and its
    // iso-distance contours are OCTAGONS: exact along the axes and the
    // diagonals, and about 5.4% long at 22.5 degrees between them. On its own
    // that is a small error in a distance.
    //
    // IT IS NOT A DISTANCE HERE, IT IS THE SHAPE OF THE GROUND. The lake bed
    // is `depth = 0.45 * toShoreW` and toShoreW comes straight out of this
    // field, so the bed's depth contours ARE this transform's iso-contours.
    // Octagonal iso-contours make an octagonal bed: flat facets meeting at
    // hard angles, axis-aligned over most of their length, which is exactly
    // "square/rectangle". Quantising those facets onto 0.1 m voxels then lays
    // a terrace along each one, and any terrace that clears the waterline
    // surfaces as a long straight island.
    //
    // NOTHING ABOUT THAT IS THE DATA. It is the transform.
    //
    // SO THIS IS EXACT EUCLIDEAN INSTEAD, and it costs nothing to be exact:
    // Felzenszwalb & Huttenlocher's lower-envelope-of-parabolas algorithm is
    // O(n) -- the same order as the two chamfer sweeps it replaces -- and it
    // is exact rather than approximate, so the contours are circles and there
    // are no preferred directions at all.
    //
    // The 1D transform computes D(p) = min_q ( (p-q)^2 + f(q) ) by walking the
    // lower envelope of the parabolas rooted at each q; running it down the
    // columns and then along the rows of the result gives the exact 2D squared
    // distance. See "Distance Transforms of Sampled Functions" (2012).
    //
    // UNITS ARE UNCHANGED so every caller keeps working: in, 0 marks a seed
    // and anything non-zero is "unknown"; out, the distance in TENTHS OF A
    // CELL, which buildShoreField and buildDeepField both convert with
    // `* mx_ / 10.0`.
    // -----------------------------------------------------------------------
    static void edt1d(const float *f, float *d, int n, int *v, float *z) {
        const float kBig = 1e20f;
        int k = 0;
        v[0] = 0;
        z[0] = -kBig;
        z[1] = kBig;
        for (int q = 1; q < n; ++q) {
            float s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) /
                      float(2 * q - 2 * v[k]);
            while (s <= z[k]) {
                --k;
                s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) /
                    float(2 * q - 2 * v[k]);
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = kBig;
        }
        k = 0;
        for (int q = 0; q < n; ++q) {
            while (z[k + 1] < float(q)) ++k;
            const float dq = float(q - v[k]);
            d[q] = dq * dq + f[v[k]];
        }
    }

    static void chamfer(std::vector<int> &d, int w_, int h_) {
        const size_t n = size_t(w_) * size_t(h_);
        if (!n) return;
        const float kBig = 1e20f;
        std::vector<float> f(n);
        for (size_t k = 0; k < n; ++k) f[k] = d[k] ? kBig : 0.0f;

        const int m = (w_ > h_ ? w_ : h_);
        // static_cast, not size_t(m): `col(size_t(m))` is a vexing parse and
        // declares a FUNCTION returning vector<float>.
        const size_t mz = static_cast<size_t>(m);
        std::vector<float> col(mz), res(mz), z(mz + 1);
        std::vector<int> v(mz);

        // down the columns...
        for (int i = 0; i < w_; ++i) {
            for (int j = 0; j < h_; ++j) col[size_t(j)] = f[size_t(j) * w_ + i];
            edt1d(col.data(), res.data(), h_, v.data(), z.data());
            for (int j = 0; j < h_; ++j) f[size_t(j) * w_ + i] = res[size_t(j)];
        }
        // ...then along the rows of that, which is the exact 2D transform.
        for (int j = 0; j < h_; ++j) {
            float *row = &f[size_t(j) * w_];
            for (int i = 0; i < w_; ++i) col[size_t(i)] = row[i];
            edt1d(col.data(), res.data(), w_, v.data(), z.data());
            for (int i = 0; i < w_; ++i) row[i] = res[size_t(i)];
        }
        for (size_t k = 0; k < n; ++k) {
            const float d2 = f[k];
            d[k] = (d2 >= 1e19f) ? (1 << 28)
                                 : int(std::sqrt(double(d2)) * 10.0 + 0.5);
        }
    }

  private:
    // -----------------------------------------------------------------------
    // THE WATER MASK IS SPECKLED, AND IT COMES THAT WAY.
    //
    // (user 2026-09-18: "clean this up", with a photograph of a waterline
    // broken into rectangular teeth.)
    //
    // THE ENGINE WAS NOT DOING THIS TO IT. Printed side by side, the raw mask
    // out of the file already looks like the photograph: isolated water cells
    // standing inland, pinholes inside the lake, and a shoreline that goes in
    // and out by a cell every few cells. A per-pixel classifier has no idea
    // what a lake is -- it decides each 10.29 m cell on its own from four
    // numbers, and at a shoreline half of them are a coin toss between wet sand
    // and shallow water. Everything downstream then faithfully reproduced it.
    //
    // A 3x3 MAJORITY, TWICE. A cell whose eight neighbours disagree with it
    // loses: an island of one becomes water, a pinhole of one becomes land, and
    // a genuine shoreline -- which has five or more of its neighbours on its
    // own side -- does not move at all. Twice, because one pass leaves the
    // two-cell specks that a single pass cannot outvote.
    //
    // ...AND THE SHORE PLANE HAS TO FOLLOW IT. That plane was baked from the
    // dirty mask, and everything that decides where water IS reads it -- so
    // cleaning one without the other would leave the geometry taking its
    // shoreline from a distance field that still had the teeth in it. The
    // chamfer is two sweeps and it is cheap here in a way it is not in the
    // bake's Python: measured at 4859 x 4859 it costs about a fifth of a
    // second, once, at load.
    //
    // IT IS DONE AT LOAD RATHER THAN IN naip2cov BECAUSE IT NEEDS NO REBAKE.
    // The .vbcov files are 47 MB and the tool that makes them re-fetches NAIP;
    // doing it here fixes every window that already exists, including the three
    // on disk. If it ever moves into the bake, this can go -- a clean mask
    // survives this pass unchanged, which is the property that makes it safe to
    // run on both.
    void despeckleWater() {
        // The rebuild at the end is not optional: load() reads the ONE-SIDED
        // plane naip2cov baked, and nothing may read it until buildShoreField
        // has replaced it with the signed one. So a grid too small to
        // despeckle still falls through to it.
        if (!ok_) return;
        if (w_ < 3 || h_ < 3) { buildShoreField(); return; }
        const size_t n = size_t(w_) * size_t(h_);
        std::vector<uint8_t> wet(n), next(n);
        for (size_t k = 0; k < n; ++k) wet[k] = (g_[k] >> 4) == Water ? 1u : 0u;
        for (int pass = 0; pass < 2; ++pass) {
            next = wet;
            for (int j = 1; j < h_ - 1; ++j)
                for (int i = 1; i < w_ - 1; ++i) {
                    const size_t k = size_t(j) * w_ + i;
                    int on = 0;
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di)
                            if (di || dj) on += wet[k + size_t(dj) * w_ + di];
                    // Five of eight is a majority of the neighbourhood, so a
                    // straight edge (which has exactly four either side) is
                    // left alone and only a cell the neighbourhood outvotes
                    // moves.
                    if (wet[k] && on <= 2) next[k] = 0u;
                    else if (!wet[k] && on >= 6) next[k] = 1u;
                }
            wet.swap(next);
        }
        // Write the decision back into the class nibble, keeping the colour.
        // A cell that GAINED water takes the ramp entry it already had; the
        // bed is painted by the terrain, not from here, so nothing reads it.
        for (size_t k = 0; k < n; ++k) {
            const uint8_t cls = g_[k] >> 4, col = g_[k] & 0x0Fu;
            if (wet[k] && cls != Water) g_[k] = uint8_t((Water << 4) | col);
            else if (!wet[k] && cls == Water) g_[k] = uint8_t((Rock << 4) | col);
        }
        buildShoreField();
    }

    // THE SHORE PLANE IS SIGNED, AND THAT IS WHY THE WATERLINE IS A CURVE.
    //
    // (user 2026-09-18, with a photograph looking down on a beach: "can you
    // work on the shorelines? make them smoother, not rounded squares." The
    // same complaint had already been answered once at the runtime end -- see
    // "THE SHORE BAND, AND WHY IT WAS A COMB" in voxelworld.h -- and the note
    // there asserts the contour "cannot be square" because the plane is
    // bilinear. It was square anyway, and this is why.)
    //
    // WHAT naip2cov BAKES IS ONE-SIDED: zero on every land cell, then metres
    // of water offshore. Measured across Granby's east bank, the plane reads
    // 0.00 on land and 35 to 90 m one cell later. Interpolating that is
    // perfectly smooth and completely useless, because the contour the engine
    // asks for sits at kShoreEdgeM of a jump that size -- one part in fifty of
    // a cell. The waterline is therefore PINNED to the raster boundary, and a
    // raster boundary at 10.29 m postings and shrink 6 is a staircase with
    // 17-voxel treads. Rounded squares, exactly as reported.
    //
    // A SIGNED FIELD PUTS THE CONTOUR WHERE THE DATA PUTS IT. Run the chamfer
    // transform twice -- once out of the water into the land, once out of the
    // land into the water -- and store the difference. Now the field crosses
    // zero BETWEEN two cells instead of jumping at one, so the zero contour is
    // a genuine interpolated boundary that can sit anywhere inside a cell, and
    // it moves continuously as the two neighbours' values change.
    //
    // IT IS BUILT HERE AND NOT BAKED, ON PURPOSE. The engine already owns this
    // transform -- despeckleWater has to re-run it after it cleans the mask --
    // and doing it at load costs one pass over the grid and keeps every .vbcov
    // on disk valid. Bumping the format would have invalidated five baked
    // windows and a NAIP refetch for each.
    //
    // ONE METRE PER STEP, WHICH IS A TENTH OF A CELL. The byte is the signed
    // distance biased by 128, so it spans +-127 m -- past the 67 m the depth
    // ramp needs for full depth -- and ten levels across a cell is far finer
    // than the boundary it is describing.
    void buildShoreField() {
        const size_t n = size_t(w_) * size_t(h_);
        if (n == 0) return;
        const int INF = 1 << 28;
        std::vector<int> into(n), out(n);
        for (size_t k = 0; k < n; ++k) {
            const bool wet = (g_[k] >> 4) == Water;
            into[k] = wet ? INF : 0;   // distance from land, measured into water
            out[k] = wet ? 0 : INF;    // distance from water, measured into land
        }
        chamfer(into, w_, h_);
        chamfer(out, w_, h_);
        buildDeepField(into);
        shore_.resize(n);
        for (size_t k = 0; k < n; ++k) {
            const bool wet = (g_[k] >> 4) == Water;
            const int d = wet ? into[k] : out[k];
            double m = double(d) * mx_ / 10.0;      // chamfer units -> real metres
            if (!wet) m = -m;
            int b = int(m < 0 ? m - 0.5 : m + 0.5) + 128;
            if (b < 1) b = 1;
            if (b > 255) b = 255;
            shore_[k] = uint8_t(b);
        }
    }

    // -----------------------------------------------------------------------
    // HOW FAR OUT TO SEA THIS IS -- the same chamfer, kept coarse and wide.
    //
    // (user 2026-09-18: "the water has missing terrain on the ocean floor".)
    //
    // shore_ IS A SIGNED BYTE AND IT SATURATES AT 127 m. That is deliberate and
    // correct for everything that reads it -- the foam band, the sand, the
    // waterline warp all live within a few metres of the line, and one metre a
    // count is far finer than the boundary it describes. The note above even
    // says the range is "past the 67 m the depth ramp needs for full depth",
    // which was true of a LAKE.
    //
    // AN OCEAN IS NOT A LAKE. Measured on acadia: 55% of all water samples sit
    // at exactly 127.0 m, the ceiling, so the bed carve read one number across
    // the entire bay and laid a dead-level plane. At 5 m the water's own
    // extinction leaves a tenth of the blue, and a featureless plane at a tenth
    // brightness is indistinguishable from no terrain at all.
    //
    // THE TRUE DISTANCE WAS ALREADY COMPUTED AND THROWN AWAY -- `into` holds
    // it, at full resolution, and only the quantisation into a byte loses it.
    // So this keeps a second copy: a quarter of the resolution and eight
    // metres a count, which is 2 km of range for a fortieth of the memory. A
    // SEA BED does not need better than eight metres, and nothing that needs
    // the precise field is touched.
    //
    // NO FORMAT CHANGE AND NO RE-BAKE, which is the same argument buildShoreField
    // itself makes: the engine already owns this transform, so widening it
    // costs one downsampled pass and keeps every .vbcov on disk valid.
    void buildDeepField(const std::vector<int> &into) {
        deepW_ = (w_ + kDeepShift - 1) / kDeepShift;
        deepH_ = (h_ + kDeepShift - 1) / kDeepShift;
        deep_.assign(size_t(deepW_) * size_t(deepH_), 0);
        for (int j = 0; j < deepH_; ++j)
            for (int i = 0; i < deepW_; ++i) {
                const int sx = std::min(w_ - 1, i * kDeepShift);
                const int sy = std::min(h_ - 1, j * kDeepShift);
                const size_t k = size_t(sy) * size_t(w_) + size_t(sx);
                // Dry land is zero, not a negative: this field only ever
                // answers "how far out to sea", and the sign lives in shore_.
                const bool wet = (g_[k] >> 4) == Water;
                const double m = wet ? double(into[k]) * mx_ / 10.0 : 0.0;
                int b = int(m / double(kDeepStepM) + 0.5);
                if (b < 0) b = 0;
                if (b > 255) b = 255;
                deep_[size_t(j) * size_t(deepW_) + size_t(i)] = uint8_t(b);
            }
    }


    // The 3-4 chamfer, forward then backward. Seeds are the zeros already in
    // `d`; everything else must start at INF.

    std::vector<uint8_t> g_;
    // The one raw byte the exterior wears -- see raw(). Commonest on the
    // border ring, decided at load.
    uint8_t exterior_ = 0;
    std::vector<uint8_t> shore_;   // SIGNED distance to shore, 128 = the line
    // ...and the wide, coarse one the sea bed is shaped from -- see
    // buildDeepField for why shore_ alone cannot answer it.
    static constexpr int kDeepShift = 4;     // cells per sample
    static constexpr int kDeepStepM = 8;     // real metres per count -> 2 km range
    std::vector<uint8_t> deep_;
    int deepW_ = 0, deepH_ = 0;
    int w_ = 0, h_ = 0;
    double mx_ = 1, my_ = 1;
    float shrink_ = 1.0f;
    uint8_t ramp_[kRamp * 3] = {0};
    bool ok_ = false;
    char err_[160] = {0};
};
