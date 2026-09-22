// covrefine -- re-cut a .vbcov's WATER at the DEM's own resolution.
//
//   g++ -O2 -std=c++17 -o covrefine covrefine.cpp
//   covrefine --dem fine.vbdem --cover coarse.vbcov --out fine.vbcov
//             [--min-area 400] [--strict]
//
// THE PROBLEM. The water mask comes from naip2cov classifying imagery onto the
// DEM's grid, and every window so far was built on the 1/3 arc-second product,
// so that grid is 10.29 m. At --dem-scale 1 one cover cell is 103 VOXELS. The
// bed is `depth = 0.45 * toShoreW`, so the whole shape of a lake bottom is a
// distance field computed on a raster whose cells are ten storeys wide. Making
// that transform exact (see CoverField::chamfer) removed the octagonal facets;
// it cannot make the raster finer.
//
// THE TRICK: THE DEM ALREADY KNOWS WHERE THE WATER IS, TO THE BIT.
// USGS hydro-flattens every water body -- each posting inside one is written at
// a single elevation, exactly equal, which is a thing that does not otherwise
// happen in a measured surface where even a car park wanders by centimetres.
// naip2cov already exploits this to CORRECT the imagery ("flat means water").
//
// So a 1 m .vbdem carries a 1 m water mask for free. No imagery, no download,
// no classifier: flood the elevation for connected runs of exactly equal float
// and keep the ones big enough to be a body of water. The other classes --
// forest, meadow, rock, snow -- have no such signal and are carried over from
// the coarse cover by nearest-neighbour, which is honest: this tool claims to
// sharpen the WATER and nothing else.
//
// WHY THAT IS SAFE TO MIX. CoverField indexes by world position through its own
// header, so a cover does not have to share the DEM's grid. It never did.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

namespace {

#pragma pack(push, 1)
struct DemHeader {
    char     magic[8];
    int32_t  w, h;
    double   originLon, originLat;
    double   stepLon, stepLat;
    double   metresPerSampleX, metresPerSampleY;
    float    minM, maxM;
    int32_t  pad[8];
};
static const int kRamp = 10;
struct CovHeader {
    char     magic[8];          // "VBCOV03"
    int32_t  w, h;
    double   originLon, originLat;
    double   stepLon, stepLat;
    double   metresPerSampleX, metresPerSampleY;
    int32_t  rampCount;
    uint8_t  ramp[kRamp * 3];
    int32_t  pad[28];
};
#pragma pack(pop)

enum : uint8_t { Unknown = 0, Forest = 1, Meadow = 2, Rock = 3, Snow = 4, Water = 5 };

}  // namespace

int main(int argc, char **argv) {
    std::string demPath, covPath, outPath;
    double minAreaM2 = 400.0;      // a 20 m square -- smaller than any real pond
    bool strict = false;           // strict: ONLY flat ground is water
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dem" && i + 1 < argc) demPath = argv[++i];
        else if (a == "--cover" && i + 1 < argc) covPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--min-area" && i + 1 < argc) minAreaM2 = atof(argv[++i]);
        else if (a == "--strict") strict = true;
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (demPath.empty() || covPath.empty() || outPath.empty()) {
        fprintf(stderr, "need --dem, --cover and --out\n");
        return 2;
    }

    // ---- the fine DEM ----------------------------------------------------
    FILE *f = fopen(demPath.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", demPath.c_str()); return 1; }
    DemHeader dh{};
    if (fread(&dh, sizeof dh, 1, f) != 1 || memcmp(dh.magic, "VBDEM01", 7)) {
        fprintf(stderr, "%s is not a .vbdem\n", demPath.c_str()); return 1;
    }
    const size_t n = size_t(dh.w) * size_t(dh.h);
    std::vector<float> z(n);
    if (fread(z.data(), 4, n, f) != n) { fprintf(stderr, "short dem\n"); return 1; }
    fclose(f);
    printf("dem     %s  %dx%d  %.3f m posting\n", demPath.c_str(), dh.w, dh.h,
           dh.metresPerSampleX);

    // ---- the coarse cover ------------------------------------------------
    f = fopen(covPath.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", covPath.c_str()); return 1; }
    CovHeader ch{};
    if (fread(&ch, sizeof ch, 1, f) != 1 || memcmp(ch.magic, "VBCOV0", 6)) {
        fprintf(stderr, "%s is not a .vbcov\n", covPath.c_str()); return 1;
    }
    const size_t cn = size_t(ch.w) * size_t(ch.h);
    std::vector<uint8_t> cg(cn);
    if (fread(cg.data(), 1, cn, f) != cn) { fprintf(stderr, "short cover\n"); return 1; }
    fclose(f);
    printf("cover   %s  %dx%d  %.3f m posting  (%.1fx coarser)\n", covPath.c_str(),
           ch.w, ch.h, ch.metresPerSampleX, ch.metresPerSampleX / dh.metresPerSampleX);

    // ---- carry the non-water classes over, nearest neighbour -------------
    // The fine grid's lon/lat for a cell, mapped into the coarse grid. Both
    // headers carry their own origin and step, so nothing assumes they align.
    std::vector<uint8_t> out(n, uint8_t(Unknown << 4));
    for (int j = 0; j < dh.h; ++j) {
        const double lat = dh.originLat - (j + 0.5) * dh.stepLat;
        const double cj = (ch.originLat - lat) / ch.stepLat;
        int y = int(cj);
        if (y < 0) y = 0; else if (y >= ch.h) y = ch.h - 1;
        for (int i = 0; i < dh.w; ++i) {
            const double lon = dh.originLon + (i + 0.5) * dh.stepLon;
            const double ci = (lon - ch.originLon) / ch.stepLon;
            int x = int(ci);
            if (x < 0) x = 0; else if (x >= ch.w) x = ch.w - 1;
            out[size_t(j) * dh.w + i] = cg[size_t(y) * ch.w + x];
        }
    }

    // ---- flood the elevation for hydro-flattened bodies ------------------
    //
    // EXACT float equality is the whole test and it is deliberate. A measured
    // surface does not repeat a value across neighbours; a hydro-flattened one
    // does so across thousands. Anything approximate here would swallow gentle
    // real ground.
    const double cellArea = dh.metresPerSampleX * dh.metresPerSampleY;
    const size_t minCells = size_t(minAreaM2 / (cellArea > 1e-9 ? cellArea : 1.0) + 0.5);
    std::vector<uint8_t> mark(n, 0);        // 0 unseen, 1 rejected, 2 kept
    std::vector<int64_t> stack, region;
    size_t bodies = 0, wetCells = 0, dropped = 0;
    for (size_t s = 0; s < n; ++s) {
        if (mark[s]) continue;
        const float v = z[s];
        if (!std::isfinite(v)) { mark[s] = 1; continue; }
        // A lone cell equal to nothing is not a body; skip the common case fast.
        const int si = int(s % dh.w), sj = int(s / dh.w);
        bool anyEq = false;
        if (si > 0 && z[s - 1] == v) anyEq = true;
        else if (si < dh.w - 1 && z[s + 1] == v) anyEq = true;
        else if (sj > 0 && z[s - dh.w] == v) anyEq = true;
        else if (sj < dh.h - 1 && z[s + dh.w] == v) anyEq = true;
        if (!anyEq) { mark[s] = 1; continue; }

        region.clear();
        stack.clear();
        stack.push_back(int64_t(s));
        mark[s] = 1;
        while (!stack.empty()) {
            const int64_t k = stack.back();
            stack.pop_back();
            region.push_back(k);
            const int i = int(k % dh.w), j = int(k / dh.w);
            const int64_t nb[4] = {i > 0 ? k - 1 : -1,
                                   i < dh.w - 1 ? k + 1 : -1,
                                   j > 0 ? k - dh.w : -1,
                                   j < dh.h - 1 ? k + dh.w : -1};
            for (int t = 0; t < 4; ++t) {
                const int64_t m = nb[t];
                if (m < 0 || mark[size_t(m)]) continue;
                if (z[size_t(m)] != v) continue;
                mark[size_t(m)] = 1;
                stack.push_back(m);
            }
        }
        if (region.size() >= minCells) {
            ++bodies;
            wetCells += region.size();
            for (int64_t k : region) mark[size_t(k)] = 2;
        } else {
            dropped += region.size();
        }
    }
    printf("flat    %zu bodies of >= %.0f m2 (%zu cells), %zu cells in smaller runs\n",
           bodies, minAreaM2, wetCells, dropped);

    // ---- apply -----------------------------------------------------------
    //
    // TWO WAYS TO BE WRONG AND THEY PULL OPPOSITE. Trusting ONLY flatness loses
    // any water the DEM did not flatten -- a stream too narrow for the posting,
    // or a coast whose tidal zone was left as measured ground. Trusting the
    // coarse mask as well keeps those, at the cost of keeping its errors too.
    // --strict picks the first; the default keeps coarse water that TOUCHES a
    // flat body, which is the README's "not flat means not water" test run the
    // gentle way round.
    size_t added = 0, kept = 0, demoted = 0;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t col = out[k] & 0x0Fu;
        const uint8_t cls = out[k] >> 4;
        if (mark[k] == 2) {
            if (cls != Water) ++added;
            out[k] = uint8_t((Water << 4) | col);
            continue;
        }
        if (cls != Water) continue;
        if (strict) { out[k] = uint8_t((Rock << 4) | col); ++demoted; continue; }
        // keep it only if a flat body is within one coarse cell
        const int i = int(k % dh.w), j = int(k / dh.w);
        const int r = int(ch.metresPerSampleX / dh.metresPerSampleX + 0.5);
        bool near = false;
        for (int dj = -r; dj <= r && !near; dj += (r ? r : 1))
            for (int di = -r; di <= r && !near; di += (r ? r : 1)) {
                const int a = i + di, b = j + dj;
                if (a < 0 || b < 0 || a >= dh.w || b >= dh.h) continue;
                if (mark[size_t(b) * dh.w + a] == 2) near = true;
            }
        if (near) ++kept;
        else { out[k] = uint8_t((Rock << 4) | col); ++demoted; }
    }
    printf("water   %zu cells gained from flatness, %zu coarse-water kept,"
           " %zu demoted\n", added, kept, demoted);
    size_t finalWet = 0;
    for (size_t k = 0; k < n; ++k) if ((out[k] >> 4) == Water) ++finalWet;
    printf("        %.2f%% of the fine grid is water\n", 100.0 * double(finalWet) / double(n));

    // ---- write -----------------------------------------------------------
    CovHeader oh = ch;                      // ramp and magic carried over
    oh.w = dh.w; oh.h = dh.h;
    oh.originLon = dh.originLon; oh.originLat = dh.originLat;
    oh.stepLon = dh.stepLon; oh.stepLat = dh.stepLat;
    oh.metresPerSampleX = dh.metresPerSampleX;
    oh.metresPerSampleY = dh.metresPerSampleY;
    FILE *fo = fopen(outPath.c_str(), "wb");
    if (!fo) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    fwrite(&oh, sizeof oh, 1, fo);
    fwrite(out.data(), 1, n, fo);
    // The shore plane is REBUILT AT LOAD -- despeckleWater ends with
    // buildShoreField and its comment says the rebuild is not optional -- but
    // load() still reads this block and fails if it is short, so it is written
    // and left at the zero bias.
    std::vector<uint8_t> shore(n, 128);
    fwrite(shore.data(), 1, n, fo);
    fclose(fo);
    printf("wrote   %s  (%.1f MB)\n", outPath.c_str(),
           double(sizeof oh + 2 * n) / 1048576.0);
    return 0;
}
