// ---------------------------------------------------------------------------
// spawn_probe.cpp -- does a spawn seed actually take you somewhere NEW, and is
// there water there?
//
//   g++ -std=c++20 -O2 -I src tests/spawn_probe.cpp -o build/spawn_probe.exe
//   ./build/spawn_probe.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// ("currently the player spawns in the same spot everytime. instead have the
//  player spawn at different locations that have water. pick a biome at
//  random." user 2026-09-19.)
//
// WHY A PROBE AND NOT JUST A RENDER. chooseSpawn lives inside a 4 GB build and
// prints one line per run, so "is it the same spot every time" is a question
// you answer by launching the engine twenty times -- about an hour -- and
// "which biome did it pick" is not in the line at all. Every input it uses
// (the DEM, the cover, standDensity, the band layout) is GPU-free and loads in
// a second here, so the whole distribution can be measured at once.
//
// IT RUNS BOTH PICKERS SIDE BY SIDE: the old ring around opt_.camX/camZ and
// the proposed band-wide one, over the same seeds, so "different locations"
// is a number rather than an impression.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "world/voxelworld.h"

using namespace v2;

namespace {

constexpr float kPI = 3.14159265358979f;

// The default world's spawn anchor: real metres from the window centre,
// divided by the shrink exactly as app_load.inl does it.
constexpr float kDefCamXReal = -12173.0f, kDefCamZReal = 8026.0f;

// The sun the options start at -- chooseSpawn reads the option rather than the
// clock, because the clock has not started when it runs.
constexpr float kSunAzDeg = 30.0f, kSunElDeg = 24.0f;

struct Site {
    float x = 0.0f, z = 0.0f;
    float open = 0.0f;
    float water = 1e9f;
    uint8_t wood = 0;
    bool found = false;
};

// ---- the scoring chooseSpawn does, lifted verbatim so the probe is honest --
struct Scorer {
    const VoxelTerrain &t;
    FbmMemo dm;
    float sunX = 0.0f, sunZ = 0.0f, tanEl = 0.0f;

    explicit Scorer(const VoxelTerrain &tt) : t(tt) {
        const float a = kSunAzDeg * kPI / 180.0f;
        sunX = std::cos(a);
        sunZ = std::sin(a);
        tanEl = std::tan(std::max(2.0f, kSunElDeg) * kPI / 180.0f);
    }

    float openness(float px, float pz) {
        float local = t.standDensity(px, pz, dm);
        for (int k = 0; k < 8; ++k) {
            const float a2 = float(k) * (2.0f * kPI / 8.0f);
            local += t.standDensity(px + std::cos(a2) * 12.0f, pz + std::sin(a2) * 12.0f, dm);
        }
        local *= (1.0f / 9.0f);
        float sunward = 0.0f;
        for (int k = 1; k <= 6; ++k) {
            const float d2 = float(k) * 10.0f;
            sunward += t.standDensity(px + sunX * d2, pz + sunZ * d2, dm);
        }
        sunward *= (1.0f / 6.0f);
        return 0.5f * local + 0.5f * sunward;
    }

    bool sunlit(float px, float pz, float ph) {
        for (int k = 1; k <= 16; ++k) {
            const float d2 = float(k) * 6.0f;
            if (t.heightM(px + sunX * d2, pz + sunZ * d2) > ph + d2 * tanEl) return false;
        }
        return true;
    }

    // Slope and waterline, the two hard gates that come before any scoring.
    bool standable(float x, float z, float *hOut) {
        const float h = t.heightM(x, z);
        if (hOut) *hOut = h;
        if (h < t.waterAt(x) + 5.0f) return false;
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        const int slope =
            std::max(std::abs(t.heightVox(ci + 1, cj) - t.heightVox(ci - 1, cj)),
                     std::abs(t.heightVox(ci, cj + 1) - t.heightVox(ci, cj - 1)));
        return slope < VoxelTerrain::kTreeSlope;
    }
};

// ---- THE OLD PICKER: a 30..400 m ring around one authored point ------------
Site pickOld(const VoxelTerrain &t, uint32_t seed, float camX, float camZ) {
    Scorer sc(t);
    Site best, any;
    float bestScore = 1e9f, anyScore = 1e9f;
    for (uint32_t i = 0; i < 512; ++i) {
        const float r = 30.0f + 370.0f * std::sqrt(hashUnit(seed + 1u, i));
        const float a = hashUnit(seed + 2u, i) * 6.2831853f;
        const float x = camX + r * std::cos(a), z = camZ + r * std::sin(a);
        float h = 0.0f;
        if (!sc.standable(x, z, &h)) continue;
        float score = sc.openness(x, z);
        float wd = 1e9f;
        if (t.usingCover()) {
            const float kWantM = 40.0f;
            wd = t.cover().waterDistance(x, z, kWantM * 3.0f);
            if (wd < 2.0f) continue;
            score += 0.9f * std::min(1.0f, std::max(0.0f, wd - kWantM) / kWantM);
        }
        if (score < anyScore) {
            anyScore = score;
            any = Site{x, z, score, wd, t.woodBit(x), false};
        }
        if (score > 0.45f || score < 0.20f) continue;
        if (score >= bestScore) continue;
        if (!sc.sunlit(x, z, h)) continue;
        bestScore = score;
        best = Site{x, z, score, wd, t.woodBit(x), true};
    }
    return best.found ? best : any;
}

// ---- THE PROPOSED PICKER: a biome at random, then a shore anywhere in it ---
//
// TWO STAGES, AND THE SPLIT IS THE WHOLE IDEA.
//
//   1. AN ANCHOR. Pick one of the woods at random, then take the FIRST sample
//      in that band, anywhere in the window, with water in reach. First rather
//      than best: a best-of-N over a whole window is a search for one global
//      optimum, and two seeds that both find it land in the same clearing --
//      measured, the first cut of this put two of twenty launches one metre
//      apart. First-hit sampling has no optimum to converge on.
//
//   2. THE GLADE, which is the old picker unchanged: a 30..400 m ring around
//      the anchor, scored for openness, sunlight and distance to water. That
//      is what makes a spawn pleasant rather than merely legal, and none of it
//      was ever the problem -- it was only ever asked around ONE anchor.
//
// The water gate lives in stage 1. Stage 2's score already refuses anything
// much over 50 m from a shore (0.9 * (d-40)/40 puts it past the 0.45 ceiling),
// so the glade cannot wander off the water the anchor was chosen for.
Site pickNew(const VoxelTerrain &t, uint32_t seed, float wantWaterM) {
    Scorer sc(t);

    // -- WHICH WOOD, AT RANDOM -----------------------------------------------
    uint8_t woods[3] = {kWoodPine, kWoodBirch, kWoodOak};
    int nW = 3;
    if (t.forced) {
        woods[0] = (t.biome == Biome::Birch) ? kWoodBirch
                   : (t.biome == Biome::Oak) ? kWoodOak
                                             : kWoodPine;
        nW = 1;
    }
    const uint8_t want = woods[int(hashUnit(seed + 7u, 0u) * float(nW)) % nW];

    // -- THE WINDOW, WITH THE SAME 200 m OF MARGIN /locate USES ---------------
    const float halfX = t.usingDem() ? std::max(0.0f, 0.5f * t.dem().spanX() - 200.0f) : 3000.0f;
    const float halfZ = t.usingDem() ? std::max(0.0f, 0.5f * t.dem().spanZ() - 200.0f) : 3000.0f;

    // -- THREE ATTEMPTS: ANCHOR, THEN GLADE ----------------------------------
    //
    // Stage one only promises a shore in the right wood. Whether there is a
    // GLADE within 400 m of it is stage two's question, and on rocky high
    // ground the answer is no -- two of twenty launches fell back to the
    // anchor, which is a legal spawn on bare scree. The anchor is the cheap
    // half, so it is what gets re-rolled.
    Site fallback;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const uint32_t sd = seed + attempt * 0x9E37u;
        bool haveAnchor = false;
        float ax = 0.0f, az = 0.0f, aw = 0.0f;
        for (uint32_t i = 0; i < 4096 && !haveAnchor; ++i) {
            const float x = -halfX + 2.0f * halfX * hashUnit(sd + 11u, i);
            const float z = -halfZ + 2.0f * halfZ * hashUnit(sd + 13u, i);
            if (!(t.woodBit(x) & want)) continue;
            float h = 0.0f;
            if (!sc.standable(x, z, &h)) continue;
            if (t.usingCover()) {
                const float d = t.cover().waterDistance(x, z, wantWaterM);
                if (d < 2.0f || d >= wantWaterM) continue;
                aw = d;
            }
            ax = x;
            az = z;
            haveAnchor = true;
        }
        if (!haveAnchor) break;   // no shore in this band; another roll will not grow one
        const Site got = pickOld(t, sd, ax, az);
        if (got.found) return got;
        if (!fallback.found && fallback.water > 1e8f)
            fallback = Site{ax, az, 0.0f, aw, t.woodBit(ax), false};
    }
    return fallback;
}

const char *woodWord(uint8_t bit) {
    return (bit & kWoodBirch) ? "birch" : (bit & kWoodOak) ? "oak" : "pine";
}

void report(const char *name, const std::vector<Site> &s) {
    // HOW SPREAD OUT ARE THEY. The mean pairwise distance is the number the
    // question actually asks -- "the same spot every time" is a mean of zero.
    double sum = 0.0;
    double n = 0.0;
    float minD = 1e30f;
    for (size_t i = 0; i < s.size(); ++i)
        for (size_t j = i + 1; j < s.size(); ++j) {
            const double d = std::hypot(double(s[i].x - s[j].x), double(s[i].z - s[j].z));
            sum += d;
            n += 1.0;
            minD = std::min(minD, float(d));
        }
    int byWood[3] = {0, 0, 0};
    int wet = 0, found = 0;
    for (const Site &q : s) {
        byWood[(q.wood & kWoodBirch) ? 1 : (q.wood & kWoodOak) ? 2 : 0]++;
        if (q.water < 1e8f && q.water < 120.0f) ++wet;
        if (q.found) ++found;
    }
    std::printf("\n  %s\n", name);
    std::printf("    mean pairwise distance %8.0f m   closest pair %6.0f m\n",
                n > 0.0 ? sum / n : 0.0, double(minD));
    std::printf("    woods  pine %2d  birch %2d  oak %2d\n", byWood[0], byWood[1], byWood[2]);
    std::printf("    %d of %d have water in reach, %d scored a proper glade\n", wet,
                int(s.size()), found);
}

}  // namespace

int main(int argc, char **argv) {
    const std::string demPath = argc > 1 ? argv[1] : "assets/dem/rmnp50.vbdem";
    const std::string covPath = argc > 2 ? argv[2] : "assets/dem/rmnp50.vbcov";
    const float shrink = argc > 3 ? float(atof(argv[3])) : 6.0f;

    VoxelTerrain t;
    if (!t.loadDem(demPath, 20.0f, shrink, 1.0f)) {
        std::printf("FAIL  dem: %s\n", demPath.c_str());
        return 1;
    }
    if (!t.loadCover(covPath)) {
        std::printf("FAIL  cover: %s\n", covPath.c_str());
        return 1;
    }

    const float camX = kDefCamXReal / shrink, camZ = kDefCamZReal / shrink;
    std::printf("dem     %s  shrink %.1f\n", demPath.c_str(), double(shrink));
    std::printf("window  %.0f x %.0f world m   (x in +/- %.0f)\n", double(t.dem().spanX()),
                double(t.dem().spanZ()), double(0.5f * t.dem().spanX()));
    std::printf("anchor  the default spawn is (%.0f, %.0f), and the old picker never "
                "leaves 400 m of it\n",
                double(camX), double(camZ));
    std::printf("bands   %.0f m wide, repeating every %.0f m\n",
                double(VoxelTerrain::kBandW),
                double(VoxelTerrain::bandCount() * VoxelTerrain::kBandW));

    // -- HOW MUCH OF THE WINDOW IS EVEN A CANDIDATE --------------------------
    //
    // Asked before any picker runs, because if the answer is "almost none" then
    // a band-wide search is a search that fails and falls back, which would
    // look like the feature working and be the old behaviour wearing a hat.
    {
        Scorer sc(t);
        const float halfX = std::max(0.0f, 0.5f * t.dem().spanX() - 200.0f);
        const float halfZ = std::max(0.0f, 0.5f * t.dem().spanZ() - 200.0f);
        int n = 0, stand = 0, nearWater[4] = {0, 0, 0, 0};
        const float reach[4] = {40.0f, 80.0f, 120.0f, 200.0f};
        for (uint32_t i = 0; i < 20000; ++i) {
            const float x = -halfX + 2.0f * halfX * hashUnit(991u, i);
            const float z = -halfZ + 2.0f * halfZ * hashUnit(997u, i);
            ++n;
            float h = 0.0f;
            if (!sc.standable(x, z, &h)) continue;
            ++stand;
            for (int k = 0; k < 4; ++k) {
                const float d = t.cover().waterDistance(x, z, reach[k]);
                if (d >= 2.0f && d < reach[k]) ++nearWater[k];
            }
        }
        std::printf("\n  -- the window, sampled %d times --\n", n);
        std::printf("    standable (off the scree, 5 m above the waterline)  %5.1f%%\n",
                    100.0 * stand / n);
        for (int k = 0; k < 4; ++k)
            std::printf("    ...and with water inside %3.0f m                      %5.1f%%\n",
                        double(reach[k]), 100.0 * nearWater[k] / n);
    }

    // -- IS THE ROLL EVEN, AND DOES EVERY BAND HAVE A SHORE TO OFFER ---------
    //
    // Two different ways "pick a biome at random" comes out lopsided, and they
    // want telling apart: the ROLL itself being skewed, or the roll being fair
    // and one band having nowhere to put you, so its launches quietly fall
    // back. Twenty launches cannot separate those; three thousand can.
    {
        int roll[3] = {0, 0, 0};
        for (uint32_t k = 0; k < 3000u; ++k) {
            const uint32_t seed = hashU32(0xA51Eu, k) | 1u;
            roll[int(hashUnit(seed + 7u, 0u) * 3.0f) % 3]++;
        }
        std::printf("\n  -- the biome roll over 3000 seeds --\n");
        std::printf("    pine %4d   birch %4d   oak %4d  (even would be 1000 each)\n",
                    roll[0], roll[1], roll[2]);

        Scorer sc(t);
        const float halfX = std::max(0.0f, 0.5f * t.dem().spanX() - 200.0f);
        const float halfZ = std::max(0.0f, 0.5f * t.dem().spanZ() - 200.0f);
        const uint8_t bits[3] = {kWoodPine, kWoodBirch, kWoodOak};
        std::printf("\n  -- and how much shore each band actually holds --\n");
        for (int b = 0; b < 3; ++b) {
            int n = 0, wet = 0;
            for (uint32_t i = 0; i < 30000u; ++i) {
                const float x = -halfX + 2.0f * halfX * hashUnit(881u, i);
                const float z = -halfZ + 2.0f * halfZ * hashUnit(883u, i);
                if (!(t.woodBit(x) & bits[b])) continue;
                ++n;
                float h = 0.0f;
                if (!sc.standable(x, z, &h)) continue;
                const float d = t.cover().waterDistance(x, z, 60.0f);
                if (d >= 2.0f && d < 60.0f) ++wet;
            }
            std::printf("    %-5s  %6d samples in the band, %5.2f%% of them a shore\n",
                        woodWord(bits[b]), n, n ? 100.0 * wet / n : 0.0);
        }
    }

    // -- THE TWO PICKERS OVER THE SAME TWENTY SEEDS --------------------------
    std::vector<Site> oldS, newS;
    std::printf("\n  -- twenty launches --\n");
    std::printf("    %-10s  %-26s  %-26s\n", "seed", "old (ring round the anchor)",
                "new (a biome, then a shore)");
    for (int k = 0; k < 20; ++k) {
        const uint32_t seed = uint32_t(hashU32(0xA51Eu, uint32_t(k))) | 1u;
        const Site a = pickOld(t, seed, camX, camZ);
        const Site b = pickNew(t, seed, 120.0f);
        oldS.push_back(a);
        newS.push_back(b);
        char la[64], lb[64];
        std::snprintf(la, sizeof(la), "%7.0f,%7.0f %-5s", double(a.x), double(a.z),
                      woodWord(a.wood));
        std::snprintf(lb, sizeof(lb), "%7.0f,%7.0f %-5s w%3.0f", double(b.x), double(b.z),
                      woodWord(b.wood), double(std::min(b.water, 999.0f)));
        std::printf("    %-10u  %-26s  %-26s\n", unsigned(seed), la, lb);
    }
    report("old picker", oldS);
    report("new picker", newS);
    return 0;
}
