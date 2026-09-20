// inset.h -- a measured 10 cm heightfield patch laid over the DEM.
//
// WHY, AND WHY IT IS A HEIGHT AND NOT A VOLUME.
//
// (user 2026-09-19: "I need 10cm detail, not 1 meter", then "implement bullet
// point 1" -- a voxelised inset over the view disc, DEM beyond it.)
//
// No public dataset carries 10 cm terrain with coverage; the best with a whole
// window behind it is 3DEP's 1 m, which at --dem-scale 1 leaves ten voxel
// columns of interpolation between every pair of measurements. The only sources
// that reach 10 cm are drone lidar and SfM, which cover a plot, not a world. So
// the measured detail has to be LOCAL, and everything outside it stays on the
// DEM.
//
// A VOLUME WOULD HAVE BEEN THE OTHER ANSWER AND IT DOES NOT FIT THIS ENGINE.
// columns.h opens by stating the property the whole design rests on:
//
//     "A PROCEDURAL TERRAIN COLUMN OF SURFACE HEIGHT h IS THE BITMASK
//      (1 << (h + 1)) - 1, AND IT COSTS NOTHING TO STORE BECAUSE IT IS NOT
//      STORED."
//
// ColumnStack carries `int32_t h[]` -- ONE surface height per column -- and the
// brick mesher, the collider and the edit layer are all built on that. A volume
// with overhangs cannot be expressed through it, so an inset that wanted them
// would have to become real bits everywhere it covered, which is the one thing
// the engine is built never to do. A 256 m patch at 0.1 m is 6.5 M columns; as
// heights that is 26 MB, as dense bits it is billions of voxels.
//
// SO: OVERHANGS ARE OUT OF SCOPE HERE and that is a deliberate limit, not an
// oversight. This buys measured 10 cm RELIEF -- the gullies, banks, root
// throw and tread that a 1 m posting averages away -- and nothing that a
// heightfield could not hold in principle.
//
// THE PATCH IS REBASED, NOT PLACED. A scan's absolute elevation belongs to
// where it was flown; dropping a 340 m Norwegian plot into a 20 m Arkansas
// valley would punch a tower through the world. So only the DEVIATION from the
// patch's own mean is used, added to whatever the DEM says at that spot. The
// patch brings texture, the DEM keeps the landform.
//
// AND IT IS FEATHERED. A hard boundary between measured and interpolated
// ground is a cliff exactly one voxel wide, running in a straight line, which
// is the same class of artefact as everything else fought in this file. The
// deviation fades to zero over the outer `kFeatherM` metres.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace v2 {

class HeightInset {
  public:
#pragma pack(push, 1)
    struct Header {
        char    magic[8];       // "VBINS01"
        int32_t w, h;           // samples
        float   stepM;          // metres per sample, in REAL metres
        float   meanM;          // the patch's own mean height, already removed
        int32_t pad[8];
    };
#pragma pack(pop)

    // How far in from the edge the deviation fades to nothing, in world metres.
    static constexpr float kFeatherM = 12.0f;

    bool load(const std::string &path, float worldX, float worldZ, float shrink) {
        ok_ = false;
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) { snprintf(err_, sizeof err_, "cannot open %s", path.c_str()); return false; }
        Header hd{};
        if (fread(&hd, sizeof hd, 1, f) != 1 || memcmp(hd.magic, "VBINS01", 7)) {
            fclose(f); snprintf(err_, sizeof err_, "not a .vbins"); return false;
        }
        if (hd.w < 2 || hd.h < 2) { fclose(f); snprintf(err_, sizeof err_, "degenerate"); return false; }
        dev_.resize((size_t)hd.w * hd.h);
        const size_t got = fread(dev_.data(), 4, dev_.size(), f);
        fclose(f);
        if (got != dev_.size()) { snprintf(err_, sizeof err_, "short data"); return false; }
        w_ = hd.w; h_ = hd.h;
        // The patch is sampled in REAL metres and the world is shrink times
        // smaller, so a patch metre is a world metre only at scale 1.
        stepW_ = hd.stepM / (shrink > 0.01f ? shrink : 1.0f);
        cx_ = worldX; cz_ = worldZ;
        halfW_ = 0.5f * float(w_) * stepW_;
        halfH_ = 0.5f * float(h_) * stepW_;
        meanM_ = hd.meanM;
        ok_ = true;
        return true;
    }

    bool ok() const { return ok_; }
    const char *err() const { return err_; }
    int w() const { return w_; }
    int h() const { return h_; }
    float stepWorldM() const { return stepW_; }
    float spanWorldM() const { return 2.0f * halfW_; }
    float meanM() const { return meanM_; }

    // Metres to ADD to the DEM's height here. Zero outside, and faded to zero
    // across the border so nothing steps.
    float deviationM(float x, float z) const {
        if (!ok_) return 0.0f;
        const float lx = x - cx_ + halfW_;
        const float lz = z - cz_ + halfH_;
        if (lx < 0.0f || lz < 0.0f || lx >= 2.0f * halfW_ || lz >= 2.0f * halfH_) return 0.0f;
        const float gx = lx / stepW_ - 0.5f, gz = lz / stepW_ - 0.5f;
        int x0 = int(std::floor(gx)), z0 = int(std::floor(gz));
        float tx = gx - float(x0), tz = gz - float(z0);
        if (x0 < 0) { x0 = 0; tx = 0.0f; }
        if (z0 < 0) { z0 = 0; tz = 0.0f; }
        if (x0 >= w_ - 1) { x0 = w_ - 2; tx = 1.0f; }
        if (z0 >= h_ - 1) { z0 = h_ - 2; tz = 1.0f; }
        const float *r0 = &dev_[(size_t)z0 * w_ + x0];
        const float *r1 = r0 + w_;
        const float a = r0[0] + (r0[1] - r0[0]) * tx;
        const float b = r1[0] + (r1[1] - r1[0]) * tx;
        const float d = a + (b - a) * tz;

        // feather: distance to the nearest border, in world metres
        const float e = std::min(std::min(lx, 2.0f * halfW_ - lx),
                                 std::min(lz, 2.0f * halfH_ - lz));
        if (e >= kFeatherM) return d;
        const float t = e / kFeatherM;
        return d * t * t * (3.0f - 2.0f * t);   // smoothstep, C1 at both ends
    }

  private:
    std::vector<float> dev_;
    int w_ = 0, h_ = 0;
    float stepW_ = 0.1f, cx_ = 0, cz_ = 0, halfW_ = 0, halfH_ = 0, meanM_ = 0;
    bool ok_ = false;
    char err_[160] = {0};
};

}  // namespace v2
