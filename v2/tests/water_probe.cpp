// ---------------------------------------------------------------------------
// water_probe.cpp -- find a camera that is actually over open water.
//
//   g++ -std=c++20 -O2 -I src tests/water_probe.cpp -o build/water_probe.exe
//   ./build/water_probe.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// WHY THIS EXISTS. Every attempt to photograph the lake bed by guessing
// --cam-x/--cam-z has landed on the window EDGE -- the held-edge ground beyond
// the data, which renders as a brown void with a stepped boundary and looks
// exactly like a terrain bug. Three renders were spent that way before writing
// this, which is precisely what the note in the water-verification memory says
// not to do: aim with a host probe, because the host can ask the field where
// the water is and a camera cannot.
//
// WHAT MAKES A GOOD SHOT OF A BED, specifically:
//   * lots of water in view, so the bed fills the frame;
//   * FAR FROM ANY SHORE, because depth = 0.45 * toShoreW, so the deepest and
//     most obviously-shaped bed is the part furthest from land;
//   * well inside the window, so no held edge is visible;
//   * and a bank to stand on, so the camera has somewhere to be.
//
// It prints a ready-to-paste command line rather than coordinates, because the
// mistake being prevented is a transcription one.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "world/voxelworld.h"

int main(int argc, char **argv) {
    const char *demPath = argc > 1 ? argv[1] : "assets/dem/ouachita12.vbdem";
    const char *covPath = argc > 2 ? argv[2] : "assets/dem/ouachita12.vbcov";
    const float shrink = argc > 3 ? (float)atof(argv[3]) : 1.0f;

    v2::VoxelTerrain t;
    if (!t.loadDem(demPath, 20.0f, shrink, 1.0f)) {
        printf("dem: %s\n", t.dem().err());
        return 2;
    }
    if (!t.loadCover(covPath)) {
        printf("cover: %s\n", t.cover().err());
        return 2;
    }
    const float halfW = 0.5f * float(t.dem().w()) * float(t.dem().metresPerSampleX()) / shrink;
    printf("dem    %s  half-extent %.0f world m\n", demPath, halfW);

    // Stay well inside: the outer 12% is where the held edge shows.
    const float lim = halfW * 0.88f;
    const int N = 150;

    struct Best { float x = 0, z = 0, score = -1, shoreM = 0; };
    Best best;
    long wet = 0, dry = 0;
    for (int j = 0; j < N; ++j) {
        const float z = -lim + 2.0f * lim * (j + 0.5f) / N;
        for (int i = 0; i < N; ++i) {
            const float x = -lim + 2.0f * lim * (i + 0.5f) / N;
            const uint8_t cc = t.cover().at(x, z);
            if (cc != CoverField::Water) { ++dry; continue; }
            ++wet;
            // Signed: positive IN water. The further in, the deeper the bed.
            //
            // THE NEAR FIELD SATURATES AT 127 m AND THAT IS BY DESIGN -- shore_
            // is one signed byte per cell. Beyond about 120 m the answer comes
            // from deep_ instead, a decimated field at 8 m a count, so it
            // reaches 2040 m. heightM switches between them on exactly this
            // test and so must anything else that cares how far out it is.
            //
            // The first version of this probe read the near field alone and
            // reported "127 m from shore" for a point whose nearest land was
            // 1080 m away, which looked like a saturation BUG in the engine and
            // was a bug in the probe. Every other caller was audited after
            // that: they all work inside a few metres of the line, or they
            // switch like this one now does.
            const float nearM = t.cover().shoreDistance(x, z);
            const float sd = nearM < 120.0f
                                 ? nearM
                                 : std::max(nearM, t.cover().shoreDistanceFar(x, z));
            if (sd <= 0.0f) continue;
            // How much of a 300 m disc round this point is also wet -- a big
            // open body beats a narrow arm that happens to be deep.
            int open = 0, tot = 0;
            for (int a = 0; a < 16; ++a) {
                const float th = a * 6.28318f / 16.0f;
                for (float r = 60.0f; r <= 300.0f; r += 60.0f) {
                    ++tot;
                    if (t.cover().at(x + r * std::cos(th), z + r * std::sin(th))
                        == CoverField::Water) ++open;
                }
            }
            const float frac = float(open) / float(tot);
            const float score = sd * frac * frac;
            if (score > best.score) { best = {x, z, score, sd}; }
        }
    }
    printf("cover  %.1f%% of the sampled window is water\n",
           100.0 * double(wet) / double(wet + dry));
    if (best.score <= 0) {
        printf("no open water found inside the window\n");
        return 1;
    }
    printf("best   (%.0f, %.0f)  %.0f m from shore\n", best.x, best.z, best.shoreM);

    // Stand on the nearest bank, look at the open water. Walking outward along
    // the ray to the deep point until the cover turns dry finds a shore that
    // actually faces the body, rather than the nearest shore in any direction.
    float camX = best.x, camZ = best.z;
    float bx = 0, bz = 0;
    bool found = false;
    for (int a = 0; a < 64 && !found; ++a) {
        const float th = a * 6.28318f / 64.0f;
        for (float r = 20.0f; r <= 2500.0f; r += 20.0f) {
            const float px = best.x + r * std::cos(th), pz = best.z + r * std::sin(th);
            if (std::fabs(px) > lim || std::fabs(pz) > lim) break;
            if (t.cover().at(px, pz) != CoverField::Water) {
                if (!found || r < std::hypot(bx - best.x, bz - best.z)) {
                    bx = px; bz = pz; found = true;
                }
                break;
            }
        }
    }
    if (found) { camX = bx; camZ = bz; }
    const float yaw = std::atan2(best.z - camZ, best.x - camX) * 57.2957795f;
    v2::TerrainMemo memo;
    const float g = t.heightM(camX, camZ, memo);
    printf("bank   (%.0f, %.0f) ground %.1f m, %.0f m back from the deep point\n",
           camX, camZ, g, std::hypot(best.x - camX, best.z - camZ));

    printf("\n  v2.exe --background --ouachita \\\n"
           "    --cam-x %.0f --cam-z %.0f --eye %.0f --pitch -18 --yaw %.0f \\\n"
           "    --out shot.png\n",
           camX, camZ, g + 35.0f, yaw);
    printf("\n  (yaw points at the open water; raise --eye for more of the bed)\n");
    return 0;
}
