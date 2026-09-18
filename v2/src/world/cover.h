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
        if (i < 0) i = 0; else if (i >= w_) i = w_ - 1;
        if (j < 0) j = 0; else if (j >= h_) j = h_ - 1;
        return g_[(size_t)j * w_ + i];
    }

    // THE BYTE IS PACKED: class in the high nibble, ground-colour index in the
    // low one. Everything that used to read the byte as a class still can, as
    // long as it goes through here.
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
    // Baked by naip2cov as a chamfer distance transform, and read here with
    // BILINEAR interpolation -- which is the whole reason the lake bed is
    // smooth now. The old version ringed outward at runtime until it left the
    // water: ~950 lookups inside heightM, the hottest function in the engine,
    // returning a value quantised to the cell AND wobbled by the sampler's
    // jitter. That was the hitching and the terraced bed in one.
    //
    // Interpolated, not nearest, and deliberately UNJITTERED: this is a
    // measurement, not a label, so averaging two neighbours is meaningful here
    // in a way it never is for a class.
    float shoreDistance(float x, float z) const {
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
        return a + (b - a) * ty;
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
    static constexpr float kShoreEdgeM = 1.0f;
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
        if (!ok_ || w_ < 3 || h_ < 3) return;
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
        rebuildShore();
    }

    // The chamfer transform naip2cov bakes, run again over the cleaned mask.
    // Same 3-4 weights, same real-metre units, same 255 clamp, so what comes
    // out is what the file would have held had the mask been clean when it was
    // written.
    void rebuildShore() {
        const size_t n = size_t(w_) * size_t(h_);
        const int INF = 1 << 28;
        std::vector<int> dist(n);
        for (size_t k = 0; k < n; ++k) dist[k] = (g_[k] >> 4) == Water ? INF : 0;
        const int D1 = 10, D2 = 14;
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                const size_t k = size_t(j) * w_ + i;
                if (!dist[k]) continue;
                int best = dist[k];
                if (i) best = std::min(best, dist[k - 1] + D1);
                if (j) {
                    best = std::min(best, dist[k - w_] + D1);
                    if (i) best = std::min(best, dist[k - w_ - 1] + D2);
                    if (i < w_ - 1) best = std::min(best, dist[k - w_ + 1] + D2);
                }
                dist[k] = best;
            }
        for (int j = h_ - 1; j >= 0; --j)
            for (int i = w_ - 1; i >= 0; --i) {
                const size_t k = size_t(j) * w_ + i;
                if (!dist[k]) continue;
                int best = dist[k];
                if (i < w_ - 1) best = std::min(best, dist[k + 1] + D1);
                if (j < h_ - 1) {
                    best = std::min(best, dist[k + w_] + D1);
                    if (i) best = std::min(best, dist[k + w_ - 1] + D2);
                    if (i < w_ - 1) best = std::min(best, dist[k + w_ + 1] + D2);
                }
                dist[k] = best;
            }
        for (size_t k = 0; k < n; ++k) {
            const double m = double(dist[k]) * mx_ / 10.0;
            shore_[k] = uint8_t(m > 255.0 ? 255 : (m < 0.0 ? 0 : m + 0.5));
        }
    }

    std::vector<uint8_t> g_;
    std::vector<uint8_t> shore_;   // distance to shore, real metres
    int w_ = 0, h_ = 0;
    double mx_ = 1, my_ = 1;
    float shrink_ = 1.0f;
    uint8_t ramp_[kRamp * 3] = {0};
    bool ok_ = false;
    char err_[160] = {0};
};
