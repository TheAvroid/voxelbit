// ---------------------------------------------------------------------------
// crowd_probe.cpp -- is the thinning hitting thickets and sparing open ground?
//
//   g++ -std=c++20 -O2 -I src tests/crowd_probe.cpp -o build/crowd_probe.exe
//
// THE REASON THIS EXISTS RATHER THAN READING TREE COUNTS OFF A RENDER. The
// engine prints "N trees" for the resident chunk ring, and that ring is not a
// constant: a cold first run reported 441 chunks where a warm one reports 709,
// so the same world gave 2694 trees and then 3704. Every decor count moved with
// it -- rocks and flowers too -- because the AREA changed, not the density.
// Comparing those numbers across runs measures streaming, not the gate.
//
// This asks the gate directly over a fixed 600 m square, so the answer depends
// on nothing but the code under test.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "world/voxelworld.h"
using namespace v2;
int main(){
    VoxelTerrain t;
    t.loadDem("assets/dem/front60.vbdem", 20.0f, 6.0f, 1.0f);
    t.loadCover("assets/dem/front60.vbcov");
    const CoverField &c = t.cover();
    struct Spot { const char *n; float x, z; };
    Spot spots[] = {{"peak flank", 1719.0f, 3074.0f}, {"lake shore", -1786.0f, -3275.0f}};
    for (auto &s : spots) {
        long forest=0, crowd=0, n=0; double sumf=0;
        int hist[11]={0};
        for (float dz=-300; dz<=300; dz+=2.0f)
            for (float dx=-300; dx<=300; dx+=2.0f) {
                const float x=s.x+dx, z=s.z+dz;
                ++n;
                if (c.at(x,z) != CoverField::Forest) continue;
                ++forest;
                const float f = c.forestFraction(x,z);
                sumf += f;
                hist[std::min(10,int(f*10.0f))]++;
                if (t.stemFill(x,z,t.heightVox(int(x/VOXEL_M),int(z/VOXEL_M))) < 1.0f) ++crowd;
            }
        printf("%s  (%.0f, %.0f)\n", s.n, s.x, s.z);
        printf("  columns %ld, forest %ld (%.1f%%)\n", n, forest, 100.0*forest/n);
        if (!forest) { printf("  no forest here\n\n"); continue; }
        printf("  mean forestFraction %.3f\n", sumf/forest);
        printf("  thinned by crowding %ld  (%.1f%% of forest columns)\n",
               crowd, 100.0*crowd/forest);
        printf("  fraction histogram: ");
        for (int i=0;i<=10;i++) if(hist[i]) printf("%.1f:%d ", i*0.1, hist[i]);
        printf("\n\n");
    }
}
