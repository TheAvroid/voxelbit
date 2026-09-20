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
        // -- WHAT THE GROUND DOES ONCE THE DATA STOPS -- see heightM --------
        //
        // The MEAN OF THE BORDER, not of the whole raster: the exterior is
        // continuous with the edge, so the number that belongs to it is the
        // number the edge is already saying. A whole-raster mean would put the
        // plain at the average of a mountain range, and the fade would then
        // run uphill off a valley floor and downhill off a summit.
        //
        // In ASL, because that is what every sample in g_ is and the
        // conversion happens once, at the return.
        {
            double sum = 0.0;
            long n = 0;
            for (int x = 0; x < w_; ++x) {
                sum += double(g_[size_t(x)]) + double(g_[size_t(h_ - 1) * size_t(w_) + size_t(x)]);
                n += 2;
            }
            for (int y = 0; y < h_; ++y) {
                sum += double(g_[size_t(y) * size_t(w_)]) +
                       double(g_[size_t(y) * size_t(w_) + size_t(w_ - 1)]);
                n += 2;
            }
            exterior_ = n ? float(sum / double(n)) : minM_;
        }
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

    // HOW FAR PAST THE DATA THIS POINT IS, 0 inside and 1 once the fade is
    // done -- the same curve heightM uses, exposed so the terrain can put its
    // own relief back on top of the plain. See VoxelTerrain::heightM.
    float outFraction(float x, float z) const {
        if (!ok_) return 0.0f;
        const double gx = double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5;
        const double gy = double(z) * shrink_ / my_ + h_ * 0.5 - 0.5;
        const float out =
            v2::maxf(v2::maxf(0.0f, v2::maxf(float(-gx), float(gx - double(w_ - 1)))),
                     v2::maxf(0.0f, v2::maxf(float(-gy), float(gy - double(h_ - 1)))));
        if (out <= 0.0f) return 0.0f;
        const float t = v2::minf(1.0f, out / kEdgeFadeCells);
        return t * t * (3.0f - 2.0f * t);
    }

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
        // -- HOW FAR OUTSIDE THE DATA WE ARE, BEFORE THE CLAMP EATS IT ----
        //
        // (user 2026-09-19, standing at -4230, 318, -781: "there are a
        //  straightline artifacts. investigate and fix".)
        //
        // The window is 8333 m across, so its edge is at 4167 -- that spot is
        // SIXTY-THREE METRES OUTSIDE IT. Past the edge the clamp below pins x0
        // and tx, which makes the height independent of x and still a function
        // of z: every ROW of the raster extends its last sample outward for
        // ever, and the exterior becomes a fan of perfectly straight ridges
        // and gullies. That is the artifact -- long parallel lines down an
        // otherwise smooth face, exactly as reported.
        //
        // The note below already says the exterior is "exactly flat". It is
        // flat ALONG X and corrugated along Z, and that is the half nobody had
        // stood in.
        //
        // MEASURED BEFORE THE CLAMP, because afterwards there is nothing left
        // to measure.
        const float outCells =
            v2::maxf(v2::maxf(0.0f, v2::maxf(float(-gx), float(gx - double(w_ - 1)))),
                     v2::maxf(0.0f, v2::maxf(float(-gy), float(gy - double(h_ - 1)))));

        if (x0 < 0)      { x0 = 0;      tx = 0.0f; }
        if (x0 >= w_ - 1){ x0 = w_ - 2; tx = 1.0f; }
        if (y0 < 0)      { y0 = 0;      ty = 0.0f; }
        if (y0 >= h_ - 1){ y0 = h_ - 2; ty = 1.0f; }
        // -- WHY THE WEIGHTS ARE PLAIN LINEAR, AFTER TRYING NOT TO BE -------
        //
        // (user 2026-09-18, with a picture of a shoreline stepping in a regular
        // zigzag and a lake bed striped like a ploughed field: "can you build
        // an ai to clean up abnormalities in the terrain like this".)
        //
        // SMOOTHSTEP WAS TRIED HERE AND IT DID NOT WORK. The reasoning was
        // sound -- a bilinear patch is a ruled facet and the facets meet at a
        // crease, so t*t*(3-2t) makes the join C1 and the contours curve. It
        // was built, shipped into a render of Lake Granby's bed, and the
        // corduroy was still there, pixel for pixel. Measured on four
        // transects: the longest flat tread went 56 -> 43 voxels on one and
        // 45 -> 60 on another, which is noise, not a fix.
        //
        // THE REASON IS THAT THE INTERPOLANT WAS NEVER THE PROBLEM. Terracing
        // is what QUANTISING a smooth ramp onto 0.1 m voxels does, and every
        // smooth ramp does it: a 2% grade puts a step every fifty voxels
        // whatever curve you draw between the postings. Smoothstep is in fact
        // slightly WORSE at the one place it was supposed to help, because
        // zero slope at each posting means the widest tread of all sits
        // exactly on the sample.
        //
        // THE REAL FIX IS A SUB-VOXEL DITHER at the point of quantisation,
        // and it lives in VoxelWorld::heightM, where the voxel grid actually
        // is. See "THE STEPS ARE DITHERED, NOT SMOOTHED" there. What this
        // function owes it is the SLOPE, which is what heightAndGrade is for.
        const float *r0 = &g_[(size_t)y0 * w_ + x0];
        const float *r1 = r0 + w_;
        const float a = r0[0] + (r0[1] - r0[0]) * tx;
        const float b = r1[0] + (r1[1] - r1[0]) * tx;
        const float h = a + (b - a) * ty;
        // -- ...AND OFF THE END IT SETTLES TO ONE HEIGHT -------------------
        //
        // See outCells. A clamped sample is still a per-row value, so leaving
        // it alone draws the straight ridges; fading it to a SINGLE number
        // leaves the exterior with no row and no column in it to draw a line
        // with. The interior is untouched -- outCells is zero everywhere
        // inside the window, so this is `h` and nothing else for every column
        // the data actually covers.
        //
        // OVER kEdgeFadeCells, which is about 200 world metres at the postings
        // this engine loads. Short enough that the fade is not a landscape
        // feature in its own right, long enough that the border relief sinks
        // rather than being sheared off at the boundary.
        //
        // THE PLAIN IS THE HONEST ANSWER AND NOT A NICE ONE -- it is what "the
        // data stops here" looks like. chooseSpawn already refuses to put you
        // out here (it keeps 200 m of margin) and /locate clamps into the
        // window; what this fixes is walking off the edge on your own, which
        // nothing prevents.
        if (outCells <= 0.0f) return aslToWorld(h);
        const float t = v2::minf(1.0f, outCells / kEdgeFadeCells);
        return aslToWorld(h + (exterior_ - h) * (t * t * (3.0f - 2.0f * t)));
    }

    // THE HEIGHT AND THE SLOPE, OUT OF THE SAME FOUR TAPS.
    //
    // A bilinear patch has an EXACT analytic gradient -- the same four samples
    // and three subtractions -- so a caller that needs to know how steep the
    // ground is does not have to probe heightM four more times a column.
    // VoxelWorld::heightM needs exactly that, to decide whether quantising
    // this column to 0.1 m is going to show as a terrace.
    //
    // `grade` comes back as RISE OVER RUN IN WORLD UNITS -- world Y per world
    // metre, so 0.02 is a 2% slope in the world the player walks, with the
    // shrink and the exaggeration already folded in.
    float heightAndGrade(float x, float z, float *grade) const {
        if (!ok_) { *grade = 0.0f; return 0.0f; }
        const double gx = double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5;
        const double gy = double(z) * shrink_ / my_ + h_ * 0.5 - 0.5;
        int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
        float tx = float(gx - x0), ty = float(gy - y0);
        // THE SAME MEASUREMENT heightM MAKES, and it has to be here too --
        // THIS is the function the terrain's ground actually calls
        // (VoxelTerrain::heightM asks heightAndGrade, because it wants the
        // slope in the same four taps). Fading only the other one left the
        // straight ridges exactly where they were and was how this was found.
        const float outCells =
            v2::maxf(v2::maxf(0.0f, v2::maxf(float(-gx), float(gx - double(w_ - 1)))),
                     v2::maxf(0.0f, v2::maxf(float(-gy), float(gy - double(h_ - 1)))));
        if (x0 < 0)      { x0 = 0;      tx = 0.0f; }
        if (x0 >= w_ - 1){ x0 = w_ - 2; tx = 1.0f; }
        if (y0 < 0)      { y0 = 0;      ty = 0.0f; }
        if (y0 >= h_ - 1){ y0 = h_ - 2; ty = 1.0f; }
        const float *r0 = &g_[(size_t)y0 * w_ + x0];
        const float *r1 = r0 + w_;
        const float dx0 = r0[1] - r0[0], dx1 = r1[1] - r1[0];
        const float a = r0[0] + dx0 * tx;
        const float b = r1[0] + dx1 * tx;
        // Per CELL first, then per world metre: one cell is mx_ real metres,
        // which is mx_/shrink_ world metres across, and the vertical carries
        // the same exag_/shrink_ that aslToWorld applies.
        const float vy = exag_ / shrink_;
        const float ddx = (dx0 + (dx1 - dx0) * ty) * vy / float(mx_ / shrink_);
        const float ddy = (b - a) * vy / float(my_ / shrink_);
        *grade = std::sqrt(ddx * ddx + ddy * ddy);
        const float h = a + (b - a) * ty;
        if (outCells <= 0.0f) return aslToWorld(h);
        // ...AND THE SLOPE GOES WITH IT. A plain has none, and leaving the
        // grade at the border's value would tell every caller that reads it --
        // the snow shed, the rock scatter, terraceBreakM -- that the flat
        // exterior is a mountainside.
        const float t = v2::minf(1.0f, outCells / kEdgeFadeCells);
        const float k = t * t * (3.0f - 2.0f * t);
        *grade *= (1.0f - k);
        return aslToWorld(h + (exterior_ - h) * k);
    }

    // Where a lon/lat lands in world metres -- the only way to aim at a real
    // place, and what makes the summit check below possible.
    void worldOf(double lon, double lat, float *x, float *z) const {
        const double gx = (lon - originLon_) / stepLon_;
        const double gy = (originLat_ - lat) / stepLat_;
        *x = float((gx - w_ * 0.5 + 0.5) * mx_ / shrink_);
        *z = float((gy - h_ * 0.5 + 0.5) * my_ / shrink_);
    }

    // ...AND THE WAY BACK. worldOf alone is enough to AIM at a recalled
    // coordinate; it is not enough to ASK what the data already found. Naming
    // the summits and lakes in poi.h is that second question -- take the peak
    // the DEM says is there, read off its real position, and check it against a
    // published one -- and it cannot be asked without the inverse.
    void lonLatOf(float x, float z, double *lon, double *lat) const {
        const double gx = double(x) * shrink_ / mx_ + w_ * 0.5 - 0.5;
        const double gy = double(z) * shrink_ / my_ + h_ * 0.5 - 0.5;
        if (lon) *lon = originLon_ + gx * stepLon_;
        if (lat) *lat = originLat_ - gy * stepLat_;
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
    // The one height the ground settles to outside the window -- the mean of
    // the border ring, in ASL. See heightM's fade.
    float exterior_ = 0.0f;
    // ...and how far that fade runs, in RASTER CELLS, so a coarse window and a
    // fine one fade over the same number of POSTINGS rather than the same
    // number of metres.
    //
    // 150 IS ABOUT 260 WORLD METRES at rmnp50's posting, and the number is set
    // by the GRADE rather than by taste. The drop is whatever the local border
    // happens to sit above the ring mean -- 82 m at the spot this was reported
    // from -- so 20 cells put a 2.4 grade on it, which is a cliff, and the fix
    // read worse than the artifact. At 150 the same drop is about 0.3, which
    // is a hillside.
    static constexpr float kEdgeFadeCells = 150.0f;
    int w_ = 0, h_ = 0;
    double mx_ = 1, my_ = 1;
    double originLon_ = 0, originLat_ = 0, stepLon_ = 1, stepLat_ = 1;
    float minM_ = 0, maxM_ = 0, datum_ = 0;
    float shrink_ = 1.0f, exag_ = 1.0f, baseM_ = 20.0f;
    bool ok_ = false;
    char err_[160] = {0};
};
