// ---------------------------------------------------------------------------
// spawn_point_probe.cpp -- what is actually AT a spawn coordinate?
//
//   g++ -std=c++20 -O2 -I src tests/spawn_point_probe.cpp -o build/spawn_point_probe.exe
//   ./build/spawn_point_probe.exe <dem> <cover> <shrink> <x> <z> [eye]
//
// WHY. A --acadia spawn that roams to a shore renders BLACK, and from the log
// alone "water is 21 m away, facing it" is indistinguishable between the two
// explanations: a good spot rendered badly, and a spot that is under the sea.
// Every number that settles it -- the ground, the waterline the engine uses,
// what the imagery calls the cell, how far the signed shore field says the
// water is -- is GPU-free, so it is one second here instead of a render.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

#include "world/voxelworld.h"

using namespace v2;

int main(int argc, char **argv) {
    if (argc < 6) {
        std::printf("usage: %s <dem> <cover> <shrink> <x> <z> [eye]\n", argv[0]);
        return 2;
    }
    const std::string demPath = argv[1], covPath = argv[2];
    const float shrink = float(atof(argv[3]));
    const float x = float(atof(argv[4])), z = float(atof(argv[5]));
    const float eye = argc > 6 ? float(atof(argv[6])) : 1.7f;

    VoxelTerrain t;
    if (!t.loadDem(demPath, 20.0f, shrink, 1.0f)) {
        std::printf("FAIL  dem %s\n", demPath.c_str());
        return 1;
    }
    if (!t.loadCover(covPath)) {
        std::printf("FAIL  cover %s\n", covPath.c_str());
        return 1;
    }

    const float h = t.heightM(x, z);
    const float w = t.waterAt(x);
    std::printf("at (%.1f, %.1f)  in a %.0f x %.0f window\n", double(x), double(z),
                double(t.dem().spanX()), double(t.dem().spanZ()));
    std::printf("  ground heightM        %8.2f\n", double(h));
    std::printf("  waterAt (procedural)  %8.2f%s\n", double(w),
                w < -1.0e8f ? "   <- kNoWater: this band has no procedural line at all" : "");
    std::printf("  eye would sit at      %8.2f\n", double(h + eye));
    std::printf("  cover class here      %8d%s\n", int(t.cover().atPoint(x, z) >> 4),
                (t.cover().at(x, z) == CoverField::Water) ? "   <- WATER" : "");
    std::printf("  waterDistance(60)     %8.2f\n", double(t.cover().waterDistance(x, z, 60.0f)));
    std::printf("  shoreDistanceRaw      %8.2f  (real m; negative on land)\n",
                double(t.cover().shoreDistanceRaw(x, z)));

    // -- AND WHAT THE GROUND DOES AROUND IT ----------------------------------
    // A spot that is dry but sits in a bowl below the surrounding sea is still
    // a spot underwater; the eight neighbours say which.
    std::printf("  ground at 20 m out    ");
    for (int k = 0; k < 8; ++k) {
        const float a = float(k) * 0.785398f;
        std::printf("%7.1f", double(t.heightM(x + std::cos(a) * 20.0f, z + std::sin(a) * 20.0f)));
    }
    std::printf("\n");
    return 0;
}
