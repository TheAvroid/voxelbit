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
        char     magic[8];          // "VBCOV02"
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
        if (memcmp(hd.magic, "VBCOV02", 7) != 0) { fclose(f); snprintf(err_, sizeof err_, "bad magic (want VBCOV02; rebuild with tools/naip2cov.py)"); return false; }
        if (hd.w <= 1 || hd.h <= 1) { fclose(f); snprintf(err_, sizeof err_, "degenerate grid"); return false; }
        g_.resize((size_t)hd.w * hd.h);
        const size_t got = fread(g_.data(), 1, g_.size(), f);
        fclose(f);
        if (got != g_.size()) { snprintf(err_, sizeof err_, "short data"); return false; }
        w_ = hd.w; h_ = hd.h; mx_ = hd.metresPerSampleX; my_ = hd.metresPerSampleY;
        memcpy(ramp_, hd.ramp, sizeof ramp_);
        ok_ = true;
        return true;
    }

    bool ok() const { return ok_; }
    const char *err() const { return err_; }
    int w() const { return w_; }
    int h() const { return h_; }

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
        const float cellX = float(mx_ / shrink_), cellZ = float(my_ / shrink_);
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

    bool isForest(float x, float z) const { return at(x, z) == Forest; }
    bool isBare(float x, float z) const {
        const uint8_t c = at(x, z);
        return c == Rock || c == Snow;
    }

  private:
    std::vector<uint8_t> g_;
    int w_ = 0, h_ = 0;
    double mx_ = 1, my_ = 1;
    float shrink_ = 1.0f;
    uint8_t ramp_[kRamp * 3] = {0};
    bool ok_ = false;
    char err_[160] = {0};
};
