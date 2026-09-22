// ---------------------------------------------------------------------------
// HOW EVEN THE SAND STEPS ARE, MEASURED AS TREAD WIDTHS.
//
// (user 2026-09-17: "when making the sandbanks flatter, you seem to only be
//  making the bottom layers flat instead of all of the sand layers. make all of
//  the sand bank steps more even.")
//
// A "step" is one 10 cm voxel of height on the shore; its TREAD is how far you
// walk in plan before the ground rises to the next one. The complaint is about
// the RATIO between the widest tread and the narrowest one inside the sand,
// not about any single width -- a beach whose first step is fourteen metres
// deep and whose last is thirty centimetres reads as a flat pan with a stack of
// kerbs behind it, however flat the average is.
//
// WHY A HARNESS AND NOT A SCREENSHOT. The quantity is a derivative of the
// height field over a plan distance, and it is 10 cm tall -- a render shows it
// only where the sun happens to rake across it, and cannot say whether the top
// of the sand is four times steeper than the bottom or forty. scene/*.h is
// GPU-free, so the real field answers in a second.
//
//   g++ -std=c++17 -O2 -I src -o bank_survey.exe tests/bank_survey.cpp
// ---------------------------------------------------------------------------
#include "scene/voxelworld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace v2;

namespace {

// One transect: walk +z from a column that is under water until the ground has
// climbed clear of the sand band, recording where each voxel step happens.
//
// RETURNS THE TREADS IN ORDER, lowest first, so the shape of the run is
// visible rather than only its spread -- "wide then narrow" and "narrow then
// wide" have the same min and max and are not the same beach.
bool transect(const VoxelTerrain &t, float x, float z0, float wl, int sandVox,
              std::vector<float> *treads) {
    treads->clear();
    const float kStep = 0.02f;   // 2 cm, well under the narrowest tread seen
    const float kMaxM = 400.0f;  // give up rather than walk the whole band

    // Must start WET, or this is not a shore.
    TerrainMemo m0;
    if (t.heightM(x, z0, m0) >= wl) return false;

    // -- WALK UP THE GRADIENT, NOT UP +z -----------------------------------
    //
    // THIS WAS MEASURING SHORE ORIENTATION, NOT SHORE STEEPNESS. Walking a
    // fixed +z crosses a shore that runs east-west square on and one that runs
    // north-south almost lengthwise, and the second kind reports treads metres
    // deep because the ground barely rises along the path. That is the whole
    // of the 15x "shore to shore" spread the first version of this harness
    // printed, and it is an artefact of the harness -- it nearly bought a
    // terrain change to fix it.
    //
    // The honest transect is the steepest-ascent one: the beach's gradient is
    // what the complaint is about, so the walk has to be perpendicular to the
    // contour it is measuring.
    float ux = 0.0f, uz = 1.0f;
    {
        TerrainMemo a, b, c;
        const float h0 = t.heightM(x, z0, a);
        const float gx = t.heightM(x + 0.5f, z0, b) - h0;
        const float gz = t.heightM(x, z0 + 0.5f, c) - h0;
        const float gl = std::sqrt(gx * gx + gz * gz);
        if (gl < 1e-6f) return false;   // dead flat: no shore to speak of
        ux = gx / gl;
        uz = gz / gl;
    }

    const float sandTop = wl + float(sandVox) * VOXEL_M;
    int lastVox = -100000;
    float lastAt = 0.0f;
    bool started = false;
    for (float s = 0.0f; s < kMaxM; s += kStep) {
        const float px = x + ux * s, pz = z0 + uz * s;
        TerrainMemo m;
        const float h = t.heightM(px, pz, m);
        if (h < wl) continue;  // still in the water
        if (h > sandTop) break;
        const int v = int(std::floor((h - wl) / VOXEL_M));
        if (!started) {
            started = true;
            lastVox = v;
            lastAt = s;
            continue;
        }
        if (v <= lastVox) continue;  // same step, or a dip -- keep walking
        // One or more steps happened between lastAt and z. Spread the run
        // evenly over them; at 2 cm sampling a multi-voxel jump is a genuinely
        // steep piece of shore, not a sampling artefact.
        const int n = v - lastVox;
        const float w = (s - lastAt) / float(n);
        for (int k = 0; k < n; ++k) treads->push_back(w);
        lastVox = v;
        lastAt = s;
    }
    return treads->size() >= 4;
}

struct Stat {
    int transects = 0;
    std::vector<float> first, last, all;
    // One entry per transect: that beach's own mean tread, for the spread
    // report in survey().
    std::vector<float> mean;
};

void survey(const VoxelTerrain &t, const char *name, float centre) {
    const float wl = t.waterAt(centre);
    if (wl == VoxelTerrain::kNoWater) {
        std::printf("  %-6s no water in this band\n", name);
        return;
    }
    const int sandVox = t.sandRiseVoxAt(centre);
    Stat s;
    std::vector<float> treads;
    // Sweep a grid of start points; most are not shores and are skipped.
    for (float x = centre - 300.0f; x <= centre + 300.0f; x += 7.0f)
        for (float z = -600.0f; z <= 600.0f; z += 3.0f) {
            if (!transect(t, x, z, wl, sandVox, &treads)) continue;
            ++s.transects;
            s.first.push_back(treads.front());
            s.last.push_back(treads.back());
            float sum = 0.0f;
            for (float w : treads) {
                s.all.push_back(w);
                sum += w;
            }
            s.mean.push_back(sum / float(treads.size()));
            z += 40.0f;  // do not re-measure the same beach
        }
    if (s.transects < 8) {
        std::printf("  %-6s only %d usable transects -- not enough to judge\n", name,
                    s.transects);
        return;
    }
    auto med = [](std::vector<float> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const float f = med(s.first), l = med(s.last);
    // -- AND THE SPREAD ACROSS SHORES, WHICH IS A DIFFERENT COMPLAINT ------
    //
    // "theres still some areas where the sand banks are steeper then other
    //  areas" is not about the run of treads inside ONE beach -- that is the
    // in-beach ratio above -- it is two beaches not matching EACH OTHER. So
    // the mean tread is taken per transect and the SPREAD of those means is
    // reported: p90 over p10, where 1.0 is every shore in the wood cut at the
    // same gradient.
    std::vector<float> per = s.mean;
    std::sort(per.begin(), per.end());
    const float p10 = per[size_t(0.10 * double(per.size() - 1))];
    const float p90 = per[size_t(0.90 * double(per.size() - 1))];
    std::printf("  %-6s water %5.1f m  sand %2d vox  %4d tx   "
                "first %5.2f last %5.2f  in-beach %5.1fx   "
                "shore p10 %5.2f p90 %5.2f  %5.1fx\n",
                name, double(wl), sandVox, s.transects, double(f), double(l),
                double(f / l), double(p10), double(p90), double(p90 / p10));
}

}  // namespace

int main() {
    VoxelTerrain t;
    const VoxelTerrain tBase;
    std::printf("\n=== SAND BANK STEP SURVEY ===\n");
    std::printf("  a tread is the plan distance covered by ONE 10 cm step of shore.\n");
    std::printf("  RATIO is the thing asked about: the first step against the last,\n");
    std::printf("  inside the sand. 1.0 would be a perfectly even bank.\n\n");
    survey(t, "pine", VoxelTerrain::bandCentre(Biome::Pine));
    survey(t, "birch", VoxelTerrain::bandCentre(Biome::Birch));
    survey(t, "oak", VoxelTerrain::bandCentre(Biome::Oak));

    // -- AND THE LANDFORM EITHER SIDE, because "make the birch more hilly like
    //    the oak" is a claim about the height distribution and nothing else.
    // -- HOW MUCH OF THE WOOD IS BEACH ------------------------------------
    //
    // (user 2026-09-17: "now theres way too much sandy banks".)
    //
    // The thing the eye actually counts, and it is NOT the waterline or the
    // sand band on their own: a beach is wide in PLAN, and flattening the bank
    // spreads the same 1.4 m of sand over however much ground that gradient
    // takes to climb. So the honest measure is the share of DRY land wearing
    // sand -- wet columns are lake bed and were never the complaint.
    // -- HOW MANY BODIES OF WATER, AND HOW BIG ----------------------------
    //
    // (user 2026-09-17: "your creating multiple puddles, I would rather have
    //  1-2 larger bodies of water".)
    //
    // AND IT IS THE SAME QUESTION AS "too much sand". A beach is SHORELINE, and
    // shoreline is what you get per body -- twenty puddles have far more edge
    // between them than one lake of the same area, so the sand follows the
    // COUNT rather than the wet fraction. Measuring the bodies is therefore the
    // honest way to judge both complaints at once.
    //
    // Flood-filled over the sampled grid, 4-connected. Anything under a few
    // cells is noise in the sampling rather than a pond and is counted apart.
    std::printf("\n=== BODIES OF WATER ===\n");
    struct WBand {
        const char *n;
        Biome b;
    };
    const WBand kWB[] = {
        {"pine", Biome::Pine}, {"birch", Biome::Birch}, {"oak", Biome::Oak}};
    // basinT is a member, so the threshold can be swept here without
    // rebuilding the header -- which is the knob that trades how much water
    // there is against how many pieces it is in.
    const float kSweepT[] = {0.40f, 0.34f, 0.28f};
    for (float bt : kSweepT) {
    VoxelTerrain t = tBase;
    t.basinT = bt;
    std::printf("  -- basinT %.2f --\n", double(bt));
    for (const WBand &e : kWB) {
        const float c = VoxelTerrain::bandCentre(e.b);
        const float wl = t.waterAt(c);
        if (wl == VoxelTerrain::kNoWater) continue;
        const int NX = 340, NZ = 600;
        const float step = 2.0f;
        std::vector<uint8_t> wet(size_t(NX) * NZ, 0);
        for (int i = 0; i < NX; ++i)
            for (int j = 0; j < NZ; ++j) {
                TerrainMemo m;
                const float x = c - 340.0f + float(i) * step;
                const float z = -600.0f + float(j) * step;
                if (t.heightM(x, z, m) < wl) wet[size_t(i) * NZ + j] = 1;
            }
        std::vector<int> area;
        std::vector<int> stack;
        for (int i = 0; i < NX; ++i)
            for (int j = 0; j < NZ; ++j) {
                if (!wet[size_t(i) * NZ + j]) continue;
                int n = 0;
                stack.clear();
                stack.push_back(i * NZ + j);
                wet[size_t(i) * NZ + j] = 0;
                while (!stack.empty()) {
                    const int k = stack.back();
                    stack.pop_back();
                    ++n;
                    const int ci = k / NZ, cj = k % NZ;
                    const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (auto &dd : d) {
                        const int ni = ci + dd[0], nj = cj + dd[1];
                        if (ni < 0 || nj < 0 || ni >= NX || nj >= NZ) continue;
                        if (!wet[size_t(ni) * NZ + nj]) continue;
                        wet[size_t(ni) * NZ + nj] = 0;
                        stack.push_back(ni * NZ + nj);
                    }
                }
                area.push_back(n);
            }
        std::sort(area.begin(), area.end(), std::greater<int>());
        const float cellA = step * step;
        int big = 0;
        long total = 0;
        for (int a : area) {
            total += a;
            if (float(a) * cellA >= 400.0f) ++big;   // 400 m2, i.e. 20 m across
        }
        std::printf("  %-6s %4zu bodies (%d over 400 m2)   largest %7.0f m2   "
                    "biggest share %4.0f%%   wet %4.1f%%\n",
                    e.n, area.size(), big,
                    area.empty() ? 0.0 : double(area[0]) * double(cellA),
                    area.empty() ? 0.0 : 100.0 * double(area[0]) / double(total),
                    100.0 * double(total) / double(NX * NZ));
    }
    }

    std::printf("\n=== HOW MUCH OF THE DRY LAND IS SAND ===\n");
    struct SandBand {
        const char *n;
        Biome b;
    };
    const SandBand kSB[] = {
        {"pine", Biome::Pine}, {"birch", Biome::Birch}, {"oak", Biome::Oak}};
    for (const SandBand &e : kSB) {
        const float c = VoxelTerrain::bandCentre(e.b);
        const float wl = t.waterAt(c);
        const float sand = t.sandRiseAt(c);
        long dry = 0, sandy = 0;
        for (float x = c - 340.0f; x <= c + 340.0f; x += 2.0f)
            for (float z = -600.0f; z <= 600.0f; z += 2.0f) {
                TerrainMemo m;
                const float h = t.heightM(x, z, m);
                if (wl != VoxelTerrain::kNoWater && h < wl) continue;   // lake bed
                ++dry;
                if (wl != VoxelTerrain::kNoWater && h <= wl + sand) ++sandy;
            }
        std::printf("  %-6s sand band %4.2f m   %6.2f%% of dry land is beach\n", e.n,
                    double(sand), 100.0 * double(sandy) / double(dry ? dry : 1));
    }

    std::printf("\n=== LANDFORM ===\n");
    const struct {
        const char *name;
        Biome b;
    } kB[] = {{"pine", Biome::Pine}, {"birch", Biome::Birch}, {"oak", Biome::Oak}};
    for (const auto &e : kB) {
        const float c = VoxelTerrain::bandCentre(e.b);
        std::vector<float> h;
        for (float x = c - 340.0f; x <= c + 340.0f; x += 2.0f)
            for (float z = -600.0f; z <= 600.0f; z += 2.0f) {
                TerrainMemo m;
                h.push_back(t.heightM(x, z, m));
            }
        std::sort(h.begin(), h.end());
        auto pc = [&](double q) { return h[size_t(q * double(h.size() - 1))]; };
        // Relief is p95-p5 rather than max-min: one spike is not a hill.
        std::printf("  %-6s p5 %6.2f  med %6.2f  p95 %6.2f   relief %6.2f m   water %5.1f\n",
                    e.name, double(pc(0.05)), double(pc(0.50)), double(pc(0.95)),
                    double(pc(0.95) - pc(0.05)), double(t.waterAt(c)));
    }
    std::printf("\n");
    return 0;
}
