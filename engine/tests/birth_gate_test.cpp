// birth_gate_test -- the three rules that decide where a creature may be born,
// and the one that was quietly stopping a lake from filling.
//
// "The life in the water has a very delayed spawn." A view cone with no far
// limit refuses every site on the water you are walking TOWARD, because that is
// exactly where you are looking -- so the lake fills behind you and is empty
// when you arrive. This pins all three rules against each other.
//
//     g++ -std=c++20 -O2 -I src tests/birth_gate_test.cpp -o build/birth_gate_test.exe
#include <cmath>
#include <cstdio>

#include "core/noise.h"
using namespace v2;

static int fails = 0;
static void chk(bool ok, const char *what) {
    if (!ok) ++fails;
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
}

// A gate that has settled: ticked past the teleport waiver, facing -Z.
static BirthGate settled(float fx, float fz) {
    BirthGate g;
    g.tick(1.0f / 60.0f, 0.0f, 0.0f, fx, fz);       // the first tick is a "teleport"
    for (int i = 0; i < 120; ++i) g.tick(1.0f / 60.0f, 0.0f, 0.0f, fx, fz);
    return g;
}

int main() {
    // Facing -Z, which is this engine's zero heading.
    BirthGate g = settled(0.0f, -1.0f);

    std::printf("THE WAIVER IS SPENT (two seconds of standing still)\n");
    chk(!g.waived(), "the teleport waiver has expired");

    std::printf("\nRULE 1: NOTHING INSIDE %.0f m, WHEREVER YOU LOOK\n", double(kBirthMinM));
    chk(!g.mayAt(0.0f, -10.0f), "10 m dead ahead is refused");
    chk(!g.mayAt(0.0f, 10.0f), "10 m directly behind is refused");
    chk(!g.mayAt(20.0f, 0.0f), "20 m to the side is refused");

    std::printf("\nRULE 2: PAST %.0f m, ANY DIRECTION -- what fills the lake you are\n"
                "        walking at. Without this the whole approach is refused.\n",
                double(kBirthFarM));
    chk(g.mayAt(0.0f, -120.0f), "120 m DEAD AHEAD is allowed");
    chk(g.mayAt(0.0f, -95.0f), "95 m dead ahead is allowed");
    chk(g.mayAt(0.0f, 120.0f), "120 m behind is allowed");

    std::printf("\nRULE 3: BETWEEN THE TWO, ONLY OUT OF VIEW\n");
    chk(!g.mayAt(0.0f, -50.0f), "50 m dead ahead is refused");
    chk(!g.mayAt(-15.0f, -47.0f), "50 m at 18 degrees off the nose is refused");
    chk(g.mayAt(0.0f, 50.0f), "50 m directly behind is allowed");
    chk(g.mayAt(50.0f, 0.0f), "50 m square to the side is allowed (70 degree cone)");

    std::printf("\nAND A TELEPORT WAIVES ALL THREE, which is what keeps the place you\n"
                "were just dropped into from being bare.\n");
    {
        BirthGate t;
        t.tick(1.0f / 60.0f, 0.0f, 0.0f, 0.0f, -1.0f);
        chk(t.waived(), "the first tick of a session waives");
        chk(t.mayAt(0.0f, -5.0f), "...and 5 m dead ahead is allowed while it does");
        // Walking does not waive it; a jump no walk could make does.
        BirthGate w = settled(0.0f, -1.0f);
        w.tick(1.0f / 60.0f, 0.15f, 0.0f, 0.0f, -1.0f);   // 9 m/s, a sprint
        chk(!w.waived(), "a sprinting step does NOT waive it");
        w.tick(1.0f / 60.0f, 120.0f, 0.0f, 0.0f, -1.0f);  // the pause room closing
        chk(w.waived(), "a 120 m jump in one tick does");
    }

    std::printf("\nNO HEADING GIVEN MEANS NO CONE -- which is what every population but\n"
                "the lake passes, and what the offline renders pass.\n");
    {
        BirthGate n = settled(0.0f, 0.0f);
        chk(n.mayAt(0.0f, -50.0f), "50 m dead ahead is allowed when no heading was given");
        chk(!n.mayAt(0.0f, -10.0f), "...but the 30 m floor still holds");
    }

    std::printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
