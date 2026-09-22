// ---------------------------------------------------------------------------
// biome_hop_probe.cpp -- does [G] land you in the wood it names?
//
//   g++ -std=c++17 -O2 -I src tests/biome_hop_probe.cpp -o build/biome_hop.exe
//   ./build/biome_hop.exe [startX] [startZ]
//
// WHY. "the cherry spawn point, and the desert spawn points seem to be broken,
// they are not spawning in the right biomes" (user 2026-09-22). Every number
// that decides where [G] sends you is a pure function of x -- bandCentre, the
// tiling period, the roam, and woodBit, which is what the arriving player is
// standing in. None of it needs a GPU, a chunk or a DEM, so the whole question
// is arithmetic and belongs here rather than in five teleports.
//
// It walks the SAME two hash streams respawnToNextBiome does, in the same
// order, so a row here is the coordinate that function would actually produce.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "world/voxelworld.h"

using namespace v2;

// -- the pieces of App that decide a destination, copied verbatim -----------
//
// COPIED RATHER THAN INCLUDED, and that is the one weakness of this probe:
// app_locate.inl lives inside ForestApp, which pulls Falcor. If the real
// function changes and this does not, this stops describing it. Everything
// below is a transcription -- keep it that way, and check it against the
// source when a row here disagrees with the game.
// hashUnit IS THE ENGINE'S OWN -- core/noise.h, pulled in by voxelworld.h.
// Transcribing it would be one more thing that can drift; this cannot.
static float hashUnitP(uint32_t a, uint32_t b) { return v2::hashUnit(a, b); }

static const char *woodName(uint8_t bit) {
    switch (bit) {
        case kWoodPine: return "pine";
        case kWoodBirch: return "birch";
        case kWoodOak: return "oak";
        case kWoodCherry: return "cherry";
        case kWoodDesert: return "desert";
        default: return "?";
    }
}

struct Row { const char *name; Biome b; };
static const Row kCycle[] = {
    {"pine", Biome::Pine},
    {"birch", Biome::Birch},
    {"cherry", Biome::Cherry},
    {"desert", Biome::Desert},
    {"oak", Biome::Oak},
};

// App::kPinnedSpawns, transcribed.
struct Pin { Biome b; float x, z; };
static const Pin kPins[] = {
    {Biome::Pine, 2056.0f, 224.0f},
    {Biome::Pine, -2121.0f, -2073.0f},
    {Biome::Pine, 1942.0f, 2505.0f},
    {Biome::Birch, -3339.0f, -314.0f},
    {Biome::Desert, -228.0f, -375.0f},
    {Biome::Oak, 1273.0f, 51.0f},
};

static constexpr float kRespawnRoamM = 600.0f;

int main(int argc, char **argv) {
    float px = argc > 1 ? float(atof(argv[1])) : -2131.0f;
    float pz = argc > 2 ? float(atof(argv[2])) : -2073.0f;

    VoxelTerrain t;
    // THE DEM IS THE POINT OF THIS PROBE, not decoration. respawnToNextBiome
    // CLAMPS its destination to the DEM window, and that clamp is what drags a
    // far band's coordinate back into whichever band the window edge sits in.
    // Defaults from Options: rmnp50.vbdem, base 20, scale 6, exag 1.
    const char *demPath =
        argc > 3 ? argv[3] : "C:/voxelbit/engine/assets/dem/rmnp50.vbdem";
    const bool haveDem = t.loadDem(demPath, 20.0f, 6.0f, 1.0f);
    float clampHalfX = 0.0f, clampHalfZ = 0.0f;
    if (haveDem) {
        clampHalfX = std::fmax(0.0f, 0.5f * t.dem().spanX() - 200.0f);
        clampHalfZ = std::fmax(0.0f, 0.5f * t.dem().spanZ() - 200.0f);
        std::printf("dem %s  span %.0f x %.0f m  ->  destinations clamped to +-%.0f, +-%.0f\n\n",
                    demPath, t.dem().spanX(), t.dem().spanZ(), clampHalfX, clampHalfZ);
    } else {
        std::printf("dem: NOT LOADED (%s) -- no clamp, which is not how the game runs\n\n",
                    demPath);
    }

    // App::nearestBandX, transcribed -- INCLUDING the window step, which is
    // the thing this probe exists to check.
    auto nearestBandX = [&](Biome b) {
        const float period = VoxelTerrain::bandCount() * VoxelTerrain::kBandW;
        const float c = VoxelTerrain::bandCentre(b);
        const float k = floorf((px - c) / period + 0.5f);
        const float plain = c + k * period;
        if (!haveDem) return plain;
        const float lim = std::fmax(0.0f, clampHalfX - VoxelTerrain::kBandW * 0.4f);
        if (std::fabs(plain) <= lim) return plain;
        float best = 0.0f;
        bool have = false;
        for (int d = -3; d <= 3; ++d) {
            const float cand = c + (k + float(d)) * period;
            if (std::fabs(cand) > lim) continue;
            if (!have || std::fabs(cand - px) < std::fabs(best - px)) { best = cand; have = true; }
        }
        return have ? best : plain;
    };

    std::printf("band layout: period %.0f m, width %.0f m\n",
                VoxelTerrain::bandCount() * VoxelTerrain::kBandW, VoxelTerrain::kBandW);
    for (const Row &r : kCycle)
        std::printf("  %-7s index %d  centre %8.0f   woodBit there: %s\n", r.name,
                    VoxelTerrain::bandIndex(r.b), VoxelTerrain::bandCentre(r.b),
                    woodName(t.woodBit(VoxelTerrain::bandCentre(r.b))));

    // ---- the pinned rows -------------------------------------------------
    std::printf("\npinned spawns\n");
    for (const Pin &p : kPins) {
        const uint8_t got = t.woodBit(p.x);
        const int want = VoxelTerrain::bandIndex(p.b);
        float u = fmodf(p.x, VoxelTerrain::bandCount() * VoxelTerrain::kBandW);
        if (u < 0.0f) u += VoxelTerrain::bandCount() * VoxelTerrain::kBandW;
        const float within = u - floorf(u / VoxelTerrain::kBandW) * VoxelTerrain::kBandW;
        const float edge = std::fmin(within, VoxelTerrain::kBandW - within);
        const bool ok = want == ((got == kWoodBirch)    ? 0
                                 : (got == kWoodOak)    ? 1
                                 : (got == kWoodPine)   ? 2
                                 : (got == kWoodCherry) ? 3
                                                        : 4);
        std::printf("  %8.0f %8.0f  asked band %d  got %-7s  %-10s  %.0f m from the seam\n",
                    p.x, p.z, want, woodName(got), ok ? "ok" : "WRONG BAND", edge);
    }

    // ---- what the roam produces, press by press --------------------------
    //
    // THE HOP COUNT IS THE STREAM, so this replays a lap of the cycle the way a
    // player presses it rather than sampling each band independently.
    std::printf("\nrolled destinations, one lap of the cycle\n");
    std::printf("  %-8s %10s %10s   %-8s %s\n", "asked", "x", "z", "got", "verdict");
    uint32_t hops = 0;
    uint32_t pinnedVisit[8] = {};
    for (int lap = 0; lap < 3; ++lap) {
        for (const Row &r : kCycle) {
            ++hops;
            const float stride =
                (hashUnitP(0x9E37u + hops * 2654435761u, 0u) - 0.5f) * 2.0f * kRespawnRoamM;
            const float across = (hashUnitP(0x5A17u + hops * 2246822519u, 1u) - 0.5f) *
                                 VoxelTerrain::kBandW * 0.8f;
            float tx = nearestBandX(r.b) + across;
            float tz = pz + stride;

            int nPinned = 0;
            for (const Pin &p : kPins)
                if (p.b == r.b) ++nPinned;
            bool isPinned = false;
            if (nPinned > 0) {
                const uint32_t turn = pinnedVisit[size_t(r.b)]++ % uint32_t(nPinned);
                uint32_t seen = 0;
                for (const Pin &p : kPins)
                    if (p.b == r.b && seen++ == turn) { tx = p.x; tz = p.z; break; }
                isPinned = true;
            }
            // ...AND THE CLAMP, WHICH IS THE WHOLE QUESTION. respawnToNextBiome
            // applies it to every destination that is not a `nearestStand` hit,
            // pinned rows included.
            const float wantX = tx;
            if (haveDem) {
                tx = std::fmax(-clampHalfX, std::fmin(clampHalfX, tx));
                tz = std::fmax(-clampHalfZ, std::fmin(clampHalfZ, tz));
            }
            const uint8_t got = t.woodBit(tx);
            const bool ok = (r.b == Biome::Pine && got == kWoodPine) ||
                            (r.b == Biome::Birch && got == kWoodBirch) ||
                            (r.b == Biome::Oak && got == kWoodOak) ||
                            (r.b == Biome::Cherry && got == kWoodCherry) ||
                            (r.b == Biome::Desert && got == kWoodDesert);
            std::printf("  %-8s %10.0f %10.0f   %-8s %-10s%s%s\n", r.name, tx, tz,
                        woodName(got), ok ? "ok" : "WRONG BAND",
                        isPinned ? "  (pinned)" : "",
                        (wantX != tx) ? "  CLAMPED" : "");
            px = tx;
            pz = tz;
        }
    }
    return 0;
}
