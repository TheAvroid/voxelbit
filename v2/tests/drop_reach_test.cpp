// ---------------------------------------------------------------------------
// drop_reach_test.cpp -- can a swimmer reach a steak floating beside them?
//
//   g++ -std=c++20 -O2 -I src tests/drop_reach_test.cpp -o build/drop_reach_test.exe
//
// ("I cant pick up steak when its floating in the water and im swimming. fix
// this." user 2026-09-18.)
//
// THIS IS A GEOMETRY TEST AND THAT IS THE WHOLE POINT. The bug was never a
// gate, a flag or a state machine -- nothing refused the pickup. Drops::update
// measured its 1.6 m reach from `player`, which is the SOLES OF THE FEET, and a
// swimmer is the one body in this game whose feet are nowhere near the thing
// they are next to. So the failure is a distance, it is decided entirely by
// four constants that live in two headers, and it can be checked without a GPU,
// a lake, or a frame of the game.
//
// IT SWEEPS THE BOB rather than testing one pose, because the old rule was not
// uniformly broken -- it worked near the top of the swim cycle and failed at
// the bottom, which is exactly the shape of a bug that gets reported as "I
// can't pick it up" and then does not reproduce when someone tries once.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cmath>
#include <algorithm>

#include <cstdlib>
#include <string>

#include "player/player.h"

using namespace v2;

// -- THE CONSTANTS ARE READ OUT OF drops.h, NOT COPIED FROM IT ---------------
//
// drops.h cannot be included here: it pulls in world/world.h, which is Falcor,
// which is a four-gigabyte build this test exists to avoid. Copying the two
// numbers instead would make this file agree with drops.h exactly once -- on
// the day it was written -- and then drift in silence, which for a test that
// reports PASS is worse than not existing.
//
// So it parses them. If either is renamed or removed the test says so and
// fails, which is the behaviour wanted: the coupling is real, so it should be
// loud.
static float constFromHeader(const char *name, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL -- cannot open %s\n", path); exit(1); }
    std::string txt;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, n);
    fclose(f);
    const std::string key = std::string("constexpr float ") + name + " = ";
    const size_t at = txt.find(key);
    if (at == std::string::npos) {
        printf("FAIL -- %s is no longer in %s under that name\n", name, path);
        exit(1);
    }
    return float(atof(txt.c_str() + at + key.size()));
}

// The two rules, written out here rather than called, so this file states what
// it believes and a change in drops.h that breaks the belief shows up as a
// disagreement instead of as two things moving together.
static bool oldReach(float dropY, float horiz, float feetY, float reach) {
    const float dy = dropY - feetY;
    return horiz * horiz + dy * dy < reach * reach;
}
static bool newReach(float dropY, float horiz, float feetY, float eyeY, float reach) {
    const float lo = std::min(feetY, eyeY), hi = std::max(feetY, eyeY);
    const float dy = dropY - std::min(hi, std::max(lo, dropY));
    return horiz * horiz + dy * dy < reach * reach;
}

int main() {
    const float kPickupM = constFromHeader("kPickupM", "src/player/drops.h");
    const float kDropHoverM = constFromHeader("kDropHoverM", "src/player/drops.h");
    Player p;   // for the swim constants, which are its own members
    const float surface = 100.0f;
    // Where the meat sits. dropMeatAt hands spill() the water top as the item's
    // floorY, so it comes to rest a hover above the surface -- see the landing
    // in Drops::update.
    const float steakY = surface + kDropHoverM;

    printf("swim geometry, surface at %.1f m\n", surface);
    printf("  the steak floats at        %.2f m  (surface + kDropHoverM %.2f)\n", steakY,
           kDropHoverM);
    printf("  a treading eye rides       %.2f .. %.2f m  (swimRise %.2f +- swimBob %.2f)\n",
           surface + p.swimRise - p.swimBob, surface + p.swimRise + p.swimBob, p.swimRise,
           p.swimBob);
    printf("  so the feet hang at        %.2f .. %.2f m  (eye %.2f below)\n",
           surface + p.swimRise - p.swimBob - p.eye, surface + p.swimRise + p.swimBob - p.eye,
           p.eye);
    printf("  reach is %.2f m\n\n", kPickupM);

    // Directly overhead is the easiest case there is: if it fails at zero
    // horizontal distance it fails everywhere.
    int oldOk = 0, newOk = 0;
    const int kSteps = 180;
    float worstOldGap = 0.0f;
    for (int i = 0; i < kSteps; ++i) {
        const float ph = float(i) * (6.2831853f / float(kSteps));
        const float eyeY = surface + p.swimRise + std::sin(ph) * p.swimBob;
        const float feetY = eyeY - p.eye;
        if (oldReach(steakY, 0.0f, feetY, kPickupM)) ++oldOk;
        else worstOldGap = std::max(worstOldGap, (steakY - feetY) - kPickupM);
        if (newReach(steakY, 0.0f, feetY, eyeY, kPickupM)) ++newOk;
    }
    printf("  swimming, steak directly overhead, over one bob cycle:\n");
    printf("    measured from the FEET   reachable %3d%% of the cycle\n", oldOk * 100 / kSteps);
    printf("    measured down the BODY   reachable %3d%% of the cycle\n", newOk * 100 / kSteps);
    if (worstOldGap > 0.0f)
        printf("    at the worst of the bob the feet were %.2f m SHORT of reaching it\n",
               worstOldGap);

    // ...and how far to the side you may be, at the worst point of the bob.
    const float worstEye = surface + p.swimRise - p.swimBob;
    const float worstFeet = worstEye - p.eye;
    float oldSide = 0.0f, newSide = 0.0f;
    for (float hz = 0.0f; hz < 3.0f; hz += 0.01f) {
        if (oldReach(steakY, hz, worstFeet, kPickupM)) oldSide = hz;
        if (newReach(steakY, hz, worstFeet, worstEye, kPickupM)) newSide = hz;
    }
    printf("\n  at the bottom of the bob, how far to the side it may be:\n");
    printf("    from the FEET  %.2f m\n    down the BODY  %.2f m\n", oldSide, newSide);

    // AND THE LAND CASE MUST NOT REGRESS. An item on open ground hovers the
    // same kDropHoverM over it, and the feet are ON that ground -- the case the
    // old rule was written for and the one it got right.
    const float groundY = 50.0f, dropY = groundY + kDropHoverM;
    float landOld = 0.0f, landNew = 0.0f;
    for (float hz = 0.0f; hz < 3.0f; hz += 0.01f) {
        if (oldReach(dropY, hz, groundY, kPickupM)) landOld = hz;
        if (newReach(dropY, hz, groundY, groundY + p.eye, kPickupM)) landNew = hz;
    }
    printf("\n  standing on land, how far to the side a drop may be:\n");
    printf("    from the FEET  %.2f m\n    down the BODY  %.2f m\n", landOld, landNew);

    const bool pass = newOk == kSteps && newSide > 1.0f && landNew >= landOld;
    printf("\n%s\n",
           pass ? "PASS -- reachable through the whole swim cycle, and land is no worse"
                : "FAIL -- a swimmer still cannot reach a steak floating beside them");
    return pass ? 0 : 1;
}
