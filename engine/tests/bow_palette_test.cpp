// bow_palette_test -- an arrow nudge must not cost the palette a single entry.
//
// THE BUG THIS PINS DOWN (user 2026-09-14, "the bow is broken, missing
// voxels"). World::addHeldVox registers every held colour with exact=true;
// World::replaceHeldVox -- the path an arrow nudge takes -- asked for the same
// colours WITHOUT it. An exact key and a quantised key live in two different
// key spaces on purpose, so the second ask found none of the first ask's
// entries and minted them all over again. A plain start leaves the table at 254
// of 255, so they did not fit, forModelColor served mat::AIR, and the bow came
// back with holes in it.
//
// Neither call touches the GPU, so the two loops are reproduced here verbatim
// and run against the real Palette and the real bow file. What is checked is
// the thing that actually broke: the SECOND registration must mint nothing.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "render/bow.h"
#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;
static const char *chk(bool ok, const char *good, const char *bad) {
    if (!ok) ++fails;
    return ok ? good : bad;
}

static const char *kBow = "C:/voxelbit/game/assets/stone_tools/bow_arrow/bow/base.vox";

// gpu/world.h addHeldVox, the colour loop.
static int askAsLoad(Palette &p, const VoxModel &mo, int *air) {
    std::vector<bool> used(256, false);
    for (uint8_t v : mo.m) used[v] = true;
    int n = 0;
    for (int e = 1; e <= 255; ++e)
        if (used[size_t(e)]) {
            ++n;
            if (p.forModelColor(mo.pal[size_t(e) - 1], true, /*exact=*/true) == mat::AIR) ++*air;
        }
    return n;
}
// gpu/world.h replaceHeldVox, the colour loop -- the line that was wrong.
static int askAsNudge(Palette &p, const VoxModel &mo, int *air) {
    std::vector<bool> used(256, false);
    for (uint8_t v : mo.m) used[v] = true;
    int n = 0;
    for (int e = 1; e <= 255; ++e)
        if (used[size_t(e)]) {
            ++n;
            if (p.forModelColor(mo.pal[size_t(e) - 1], true, /*exact=*/true) == mat::AIR) ++*air;
        }
    return n;
}

static void loadStrip(Palette &p, const BowStrip &s, int (*ask)(Palette &, const VoxModel &, int *),
                      int *air) {
    for (const VoxModel &m : s.withArrow) ask(p, m, air);
    for (const VoxModel &m : s.bowOnly) ask(p, m, air);
}

int main() {
    std::string err;
    const BowStrip s = parseBowStrip(kBow, &err);
    if (!s.ok()) {
        std::printf("bow strip would not parse: %s\n", err.c_str());
        return 1;
    }

    // ---- 0. the composer is lossless, so a hole is never ITS doing --------
    int bowVox = 0, nockedVox = 0;
    for (uint8_t c : s.bowOnly[0].m)
        if (c) ++bowVox;
    for (uint8_t c : s.withArrow[0].m)
        if (c) ++nockedVox;
    bool even = true;
    for (int f = 0; f < s.frames; ++f) {
        int b = 0, w = 0;
        for (uint8_t c : s.bowOnly[size_t(f)].m)
            if (c) ++b;
        for (uint8_t c : s.withArrow[size_t(f)].m)
            if (c) ++w;
        even = even && b == bowVox && w == nockedVox;
    }
    std::printf("  0. %d frames, %d voxels bare / %d nocked in every one  [%s]\n", s.frames, bowVox,
                nockedVox,
                chk(s.frames == 7 && bowVox == 16 && nockedVox == 27 && even, "composed whole",
                    "WRONG -- the strip itself lost voxels"));

    // ---- 1. a nudge must cost NOTHING ------------------------------------
    {
        Palette p;
        int air = 0;
        loadStrip(p, s, askAsLoad, &air);
        const int afterLoad = p.used();
        loadStrip(p, s, askAsNudge, &air);
        const int afterNudge = p.used();
        std::printf("  1. bow loads to %d entries; one arrow nudge leaves it at %d  [%s]\n",
                    afterLoad, afterNudge,
                    chk(afterNudge == afterLoad, "no colour minted twice",
                        "WRONG -- the nudge re-registered the bow's colours"));
    }

    // ---- 2. ...even with the table as full as a real start leaves it ------
    //
    // 254 of 255 is the measured plain start. Fill to just under it, load the
    // bow, then nudge: nothing may come back AIR that did not already.
    {
        Palette p;
        for (int r = 0; r < 256 && p.used() < 242; r += 3)
            for (int g = 0; g < 256 && p.used() < 242; g += 11)
                p.forModelColor({uint8_t(r), uint8_t(g), uint8_t((r * 7 + g * 3) & 255), 255});
        const int world = p.used();
        int airLoad = 0;
        loadStrip(p, s, askAsLoad, &airLoad);
        const int afterLoad = p.used();
        int airNudge = 0;
        loadStrip(p, s, askAsNudge, &airNudge);
        std::printf("  2. world at %d/255, bow loads to %d (%d AIR); after a nudge %d/255, "
                    "%d AIR  [%s]\n",
                    world, afterLoad, airLoad, p.used(), airNudge,
                    chk(airLoad == 0 && airNudge == 0 && p.used() == afterLoad,
                        "the bow keeps every voxel", "WRONG -- the bow lost colours to the nudge"));
    }

    // ---- 3. and the colours it gets back are the ARTIST'S -----------------
    //
    // The other half of the same bug: a quantised ask also BENDS a colour to
    // whatever the world already minted within kQuantStep. Read every one back
    // out of the table and compare with the file.
    {
        Palette p;
        for (int r = 0; r < 256 && p.used() < 242; r += 3)
            for (int g = 0; g < 256 && p.used() < 242; g += 11)
                p.forModelColor({uint8_t(r), uint8_t(g), uint8_t((r * 7 + g * 3) & 255), 255});
        int air = 0;
        loadStrip(p, s, askAsLoad, &air);
        loadStrip(p, s, askAsNudge, &air);
        int worst = 0, gone = 0;
        const VoxModel &m = s.withArrow[0];
        std::vector<bool> used(256, false);
        for (uint8_t v : m.m) used[v] = true;
        for (int e = 1; e <= 255; ++e) {
            if (!used[size_t(e)]) continue;
            const uint8_t id = p.forModelColor(m.pal[size_t(e) - 1], true, /*exact=*/true);
            // COUNTED, NOT JUST SKIPPED. addHeldVox's own shift report skips
            // AIR silently, which is exactly why it read "worst colour shift
            // 0/255" through the whole of this bug. A colour that is not there
            // is the worst shift there is.
            if (id == mat::AIR) {
                ++gone;
                continue;
            }
            const Vec3 &got = p[id].albedo;
            const float lin[3] = {got.x, got.y, got.z};
            for (int k = 0; k < 3; ++k) {
                const float l = lin[k] < 0.0f ? 0.0f : (lin[k] > 1.0f ? 1.0f : lin[k]);
                const float srgb = l <= 0.0031308f ? l * 12.92f
                                                   : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
                const int d = int(srgb * 255.0f + 0.5f) - int(m.pal[size_t(e) - 1][size_t(k)]);
                worst = worst > (d < 0 ? -d : d) ? worst : (d < 0 ? -d : d);
            }
        }
        std::printf("  3. worst colour shift after a nudge: %d/255, %d colours missing  [%s]\n",
                    worst, gone,
                    chk(worst == 0 && gone == 0, "byte-accurate",
                        "WRONG -- the nudge quantised or starved the bow"));
    }

    // ---- 4. AND THE LOOPS ABOVE MUST STILL BE THE ONES THAT SHIP -----------
    //
    // Checks 1-3 reproduce gpu/world.h's two colour loops, because neither of
    // them can run without a device. That makes them a COPY, and a copy cannot
    // notice the day the original changes back -- which is the only way this
    // bug returns. So read the original and look at the call itself.
    {
        std::ifstream f("C:/voxelbit/engine/src/gpu/world.h");
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        const size_t at = src.find("bool replaceHeldVox(");
        bool ok = at != std::string::npos;
        if (ok) {
            // The colour loop is the first forModelColor inside that function.
            const size_t call = src.find("palette.forModelColor(", at);
            const size_t end = src.find(";", call);
            ok = call != std::string::npos && end != std::string::npos &&
                 src.substr(call, end - call).find("exact=*/true") != std::string::npos;
        }
        std::printf("  4. replaceHeldVox in gpu/world.h still asks exact  [%s]\n",
                    chk(ok, "pinned", "WRONG -- the real loop dropped exact=true again"));
    }

    std::printf("\n%s\n", fails ? "FAILED" : "all good");
    return fails ? 1 : 0;
}
