// dem.h -- real elevation as the world's height field.
//
// Loads a .vbdem (tools/dem2raw.cpp writes them out of USGS 3DEP GeoTIFFs) and
// answers the one question the terrain asks: how high is the ground at this
// world point. Nothing here knows what a TIFF is; the conversion happened
// offline and this is a flat float32 grid, north up, row major.
//
// TWO THINGS THIS HAS TO GET RIGHT, and both are silent when wrong.
//
// THE DATUM. Colorado's ground is 1,000-4,400 m above sea level, and a world
// that starts 30,000 voxels up is a world where every constant tuned against a
// 14-84 m landform is meaningless -- the water line, the tree floor, the spawn.
// What makes a mountain is RELIEF, not altitude, and subtracting a constant
// costs none of it. So the lowest sample in the window is parked at baseM and
// everything rides up from there: a 1,796 m relief window becomes ground from
// 20 m to 1,816 m, which is a mountain range by any measure and still lands in
// the range the rest of the engine was built for.
//
// THE AXIS. The grid is north-up: row 0 is the NORTHERN edge. World +z runs
// south. Map row to +z directly and the terrain is correct; flip it and you get
// a plausible-looking mirrored landscape that matches no map on earth and no
// coordinate anyone can check against. It is only detectable by probing a
// summit you know the position of, which is why loadCheck() exists.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

class DemField {
public:
    // Header written by tools/dem2raw.cpp. Packed and versioned by magic, so a
    // stale file is refused rather than read as garbage.
    #pragma pack(push, 1)
    struct Header {
        char     magic[8];          // "VBDEM01"
        int32_t  w, h;
        double   originLon, originLat;
        double   stepLon, stepLat;
        double   metresPerSampleX, metresPerSampleY;
        float    minM, maxM;
        int32_t  pad[8];
    };
    #pragma pack(pop)

    // shrink: how many REAL metres fit in one WORLD metre. 1 is true scale.
    // exag:   vertical multiplier applied AFTER the shrink. 1 is a true scale
    //         model (slopes identical to Colorado's); above 1 puts the height
    //         back that the shrink took out, at the cost of steeper ground.
    bool load(const std::string &path, float baseM = 20.0f,
              float shrink = 1.0f, float exag = 1.0f) {
        ok_ = false;
        shrink_ = (shrink > 0.01f) ? shrink : 1.0f;
        exag_ = (exag > 0.01f) ? exag : 1.0f;
        baseM_ = baseM;
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) { snprintf(err_, sizeof err_, "cannot open %s", path.c_str()); return false; }
        Header hd{};
        if (fread(&hd, sizeof hd, 1, f) != 1) { fclose(f); snprintf(err_, sizeof err_, "short header"); return false; }
        if (memcmp(hd.magic, "VBDEM01", 7) != 0) {
            fclose(f); snprintf(err_, sizeof err_, "bad magic"); return false;
        }
        if (hd.w <= 1 || hd.h <= 1) { fclose(f); snprintf(err_, sizeof err_, "degenerate grid"); return false; }
        g_.resize((size_t)hd.w * hd.h);
        const size_t got = fread(g_.data(), 4, g_.size(), f);
        fclose(f);
        if (got != g_.size()) { snprintf(err_, sizeof err_, "short data: %zu of %zu", got, g_.size()); return false; }

        w_ = hd.w; h_ = hd.h;
        mx_ = hd.metresPerSampleX; my_ = hd.metresPerSampleY;
        minM_ = hd.minM; maxM_ = hd.maxM;
        originLon_ = hd.originLon; originLat_ = hd.originLat;
        stepLon_ = hd.stepLon; stepLat_ = hd.stepLat;
        datum_ = minM_ - baseM;   // kept for reporting; aslToWorld is the truth
        ok_ = true;
        return true;
    }

    bool ok() const { return ok_; }
    const char *err() const { return err_; }
    int w() const { return w_; }
    int h() const { return h_; }
    float reliefM() const { return maxM_ - minM_; }
    float minM() const { return minM_; }
    float maxM() const { return maxM_; }
    float datum() const { return datum_; }
    // World extent of the data, in metres, centred on the origin.
    float spanX() const { return float(w_ * mx_ / shrink_); }
    float spanZ() const { return float(h_ * my_ / shrink_); }
    double metresPerSampleX() const { return mx_; }

    // THE HEIGHT, in world metres, datum removed.
    //
    // World (0,0) is the MIDDLE of the window, so the interesting terrain is
    // where the player starts rather than a corner they have to walk to.
    //
    // Outside the window the edge sample is held. That makes the world endless
    // without inventing terrain: walk far enough and the ground goes flat at
    // whatever height the border had. Mirroring would be seamless-looking and a
    // lie -- it would put a second Mount Elbert in the world.
    float heightM(float x, float z) const {
        if (!ok_) return 0.0f;
        // ONE WORLD METRE IS shrink_ REAL METRES. Compressing the ground is
        // the only way to get more of the dataset into a 300 m view disc: at
        // true scale that disc spans 29 postings of a 10 km mountain, which is
        // a smooth ramp with no shape in it. At shrink 6 it spans 175.
        const double gx = double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5;
        const double gy = double(z) * shrink_ / my_ + h_ * 0.5 - 0.5;
        int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
        float tx = float(gx - x0), ty = float(gy - y0);
        // Clamp the CELL and the weight together. Clamping only the index would
        // keep interpolating toward a neighbour that is the same sample, which
        // is harmless, but clamping the weight too makes the flat exterior
        // exactly flat instead of very slightly rippled at the border.
        if (x0 < 0)      { x0 = 0;      tx = 0.0f; }
        if (x0 >= w_ - 1){ x0 = w_ - 2; tx = 1.0f; }
        if (y0 < 0)      { y0 = 0;      ty = 0.0f; }
        if (y0 >= h_ - 1){ y0 = h_ - 2; ty = 1.0f; }
        const float *r0 = &g_[(size_t)y0 * w_ + x0];
        const float *r1 = r0 + w_;
        const float a = r0[0] + (r0[1] - r0[0]) * tx;
        const float b = r1[0] + (r1[1] - r1[0]) * tx;
        return aslToWorld(a + (b - a) * ty);
    }

    // Where a lon/lat lands in world metres -- the only way to aim at a real
    // place, and what makes the summit check below possible.
    void worldOf(double lon, double lat, float *x, float *z) const {
        const double gx = (lon - originLon_) / stepLon_;
        const double gy = (originLat_ - lat) / stepLat_;
        *x = float((gx - w_ * 0.5 + 0.5) * mx_ / shrink_);
        *z = float((gy - h_ * 0.5 + 0.5) * my_ / shrink_);
    }

    // THE ONE CONVERSION. Metres above sea level -> world Y. The timberline,
    // the probes and heightM all go through this, so they cannot disagree
    // about where a given altitude lands once the scale changes.
    float aslToWorld(float aslM) const {
        return (aslM - minM_) * (exag_ / shrink_) + baseM_;
    }
    // The inverse of aslToWorld. A column knows its world height; the stand
    // tables are written in metres above sea level, so something has to undo
    // the datum and the scale.
    float worldToAsl(float worldY) const {
        return (worldY - baseM_) * (shrink_ / exag_) + minM_;
    }
    float shrink() const { return shrink_; }
    float exag() const { return exag_; }

private:
    std::vector<float> g_;
    int w_ = 0, h_ = 0;
    double mx_ = 1, my_ = 1;
    double originLon_ = 0, originLat_ = 0, stepLon_ = 1, stepLat_ = 1;
    float minM_ = 0, maxM_ = 0, datum_ = 0;
    float shrink_ = 1.0f, exag_ = 1.0f, baseM_ = 20.0f;
    bool ok_ = false;
    char err_[160] = {0};
};
