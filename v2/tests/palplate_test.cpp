// palplate_test -- the palette plate --palette-vox writes must be a .vox file
// v2's own reader can open, it must carry the AUTHORED colours, and it must not
// drop one when the table is full.
//
// THREE CLAIMS, AND ONLY THE FIRST ONE FAILS LOUDLY.
//
// 1. THE FILE. writePalettePlate hand-assembles SIZE/XYZI/RGBA chunks, and
//    every byte of that is a chance to be one off: MagicaVoxel colour indices
//    start at 1 and are stored at (i-1)*4, which is the single most common
//    mistake anybody makes with this format and produces a plate that is
//    entirely the wrong colour with nothing to say so. So the file is read
//    back with scene/vox.h -- the engine's own reader, the one that loads every
//    asset in the game -- and every cell is compared against what went in.
//
// 2. THE COLOUR. Palette::forModelColor LIFTS a foliage albedo 1.7x with a
//    floor under its blue before storing it, so reading the plate out of
//    MaterialLook would put greens on it that are in no .vox file anywhere and
//    that nothing would ever match against -- a plate nobody could author
//    against, which is the only thing a plate is for. This checks that a
//    needle's cell holds what the ART carried and NOT what the table renders,
//    against the real pine models, and that the two really are different (a
//    test that passes because the lift did nothing is not a test).
//
// 3. A FULL TABLE STILL FITS, AND IT DID NOT. Found on the first real plate:
//    61 terrain entries and 192 model ones is 253 cells, the gap that starts
//    the model band on a fresh row makes 256, and the plate has 255. The gap
//    won and the LAST MODEL COLOUR WAS DROPPED -- an entry the engine holds,
//    missing from the only picture of what the engine holds, with nothing in
//    the file to say so. The alignment yields now, and the second half of this
//    test fills a table to the ceiling to keep it yielding.
//
// Neither the plate nor the palette touches the GPU, so both run here for real.
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "scene/palplate.h"
#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;
static void chk(bool ok, const char *what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static const char *kPine = "C:/voxelbit/game/assets/foilage/pine9/pine_1.vox";
static const char *kPine2 = "C:/voxelbit/game/assets/foilage/pine9/pine_5.vox";
static const char *kOut = "C:/voxelbit/v2/build/palplate_test.vox";

// gpu/world.h's colour loop, which is all of it that matters here: every
// palette entry the model actually uses, in entry order.
static int registerModel(Palette *p, const char *path, bool conifer) {
    VoxModel mo;
    std::string err;
    if (!voxLoad(path, &mo, &err)) {
        std::printf("  cannot read %s: %s\n", path, err.c_str());
        ++fails;
        return 0;
    }
    std::vector<bool> used(256, false);
    for (uint8_t v : mo.m) used[v] = true;
    int n = 0;
    for (int e = 1; e <= 255; ++e)
        if (used[size_t(e)]) {
            p->forModelColor(mo.pal[size_t(e) - 1], conifer);
            ++n;
        }
    return n;
}

// app.h runPaletteVox, reproduced -- the split between what the plate takes
// from the art and what it takes from the table is the thing under test.
static void buildCells(const Palette &pal, std::vector<PlateCell> *terrain,
                       std::vector<PlateCell> *model, int *foliage, int *lifted) {
    for (int id = 1; id < pal.used(); ++id) {
        PlateCell c;
        c.id = id;
        c.foliage = pal.isFoliage(uint8_t(id));
        c.exact = pal.authoredExact(uint8_t(id));
        std::array<uint8_t, 3> src{};
        const Vec3 a = pal[uint8_t(id)].albedo;
        const std::array<uint8_t, 3> rendered{{uint8_t(Palette::srgbByte(a.x)),
                                               uint8_t(Palette::srgbByte(a.y)),
                                               uint8_t(Palette::srgbByte(a.z))}};
        if (pal.authoredColor(uint8_t(id), &src)) {
            c.rgb = src;
            model->push_back(c);
            if (c.foliage) {
                ++*foliage;
                if (src != rendered) ++*lifted;
            }
        } else {
            c.rgb = rendered;
            terrain->push_back(c);
        }
    }
}

// The layout writePalettePlate lays down, rebuilt here as the oracle: id-ordered
// terrain, a gap to the next row IF there is room for one, the sorted model
// band, then black.
static std::vector<PlateCell> expectedCells(const std::vector<PlateCell> &terrain,
                                            const std::vector<PlateCell> &model, int *bandStart) {
    std::vector<PlateCell> e = terrain;
    const size_t pad = (8 - e.size() % 8) % 8;
    if (e.size() + pad + model.size() <= 255)
        for (size_t i = 0; i < pad; ++i) e.push_back(PlateCell());
    *bandStart = int(e.size());
    for (const PlateCell &c : sortPlateBand(model)) e.push_back(c);
    while (e.size() < 255) e.push_back(PlateCell());
    return e;
}

// Read the plate back and compare it, cell by cell, against that oracle.
// Comparing SETS of colours would pass a plate whose rows were shuffled, and
// comparing counts would pass one that repeated a family and dropped another --
// both of which still look like a plate.
static void checkAgainst(const std::vector<PlateCell> &expect, const std::vector<PlateCell> &model,
                         int bandStart, const char *what) {
    VoxModel plate;
    std::string err;
    if (!voxLoad(kOut, &plate, &err)) {
        std::printf("  %s: cannot read back: %s\n", what, err.c_str());
        ++fails;
        return;
    }
    chk(plate.sx == 8 && plate.sy == 32 && plate.sz == 1, "8 x 32 x 1, a panel-wide plate");
    int filled = 0;
    for (uint8_t v : plate.m)
        if (v) ++filled;
    chk(filled == 255, "255 cells, so the free tail is drawn and countable");

    int mismatched = 0;
    for (int n = 0; n < 255; ++n) {
        const uint8_t idx = plate.at(n % 8, n / 8, 0);
        const std::array<uint8_t, 4> p =
            idx ? plate.pal[size_t(idx) - 1] : std::array<uint8_t, 4>{{0, 0, 0, 0}};
        if (std::array<uint8_t, 3>{{p[0], p[1], p[2]}} != expect[size_t(n)].rgb) ++mismatched;
    }
    chk(mismatched == 0, "all 255 cells came back byte-for-byte");

    // EVERY MODEL ENTRY IS ON THE PLATE, exactly once. This is the check that
    // the full table broke: the sort may move a colour anywhere it likes, and
    // may not lose one off the end.
    std::map<int, int> seen;
    for (int n = bandStart; n < 255; ++n)
        if (expect[size_t(n)].id) ++seen[expect[size_t(n)].id];
    bool all = seen.size() == model.size();
    for (const PlateCell &c : model) all = all && seen[c.id] == 1;
    chk(all, "every model entry is on the plate, exactly once");

    // THE TAIL IS BLACK BY POSITION, not by count. Counting blacks would pass
    // for the wrong reason the day an asset authors a black voxel -- the skunk
    // is exactly that -- so what is checked is that every cell past the band is
    // empty, which is what makes the headroom countable by eye.
    int tailWrong = 0;
    for (int n = bandStart + int(model.size()); n < 255; ++n) {
        const uint8_t idx = plate.at(n % 8, n / 8, 0);
        if (!idx) continue;
        const auto &p = plate.pal[size_t(idx) - 1];
        if (p[0] || p[1] || p[2]) ++tailWrong;
    }
    chk(tailWrong == 0, "every slot past the table is black, so headroom is visible");

    int opaque = 0;
    for (int i = 0; i < 255; ++i)
        if (plate.pal[size_t(i)][3] == 255) ++opaque;
    chk(opaque == 255, "every swatch is opaque");
}


// -- ONE PLACE PER FAMILY, which is the whole point of the sort ------------
//
// "greens should be next to greens, reds should be next to reds" (user
// 2026-09-15), against a rule that split every family smaller than a row of
// eight into two pieces at opposite ends of the plate. What that means exactly:
// walking the band, the number of RUNS of a family must equal the number of
// families -- no family may be entered, left, and entered again.
//
// The neutral test is the SORT'S OWN (plate::neutral), not a prettier one:
// classifying by saturation here while the sort classifies by CHROMA would
// report a broken run for the near-blacks that the sort placed exactly as
// asked -- which is the mistake the analysis of the first plate made.
static std::string familyOf(const std::array<uint8_t, 3> &c) {
    float h = 0.0f, s = 0.0f, v = 0.0f;
    plate::hsv(c, &h, &s, &v);
    if (plate::neutral(s, v)) return "grey";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "hue%d", int(h / 30.0f));   // twelve families
    return buf;
}

static void familiesContiguous(const std::vector<PlateCell> &band, const char *what) {
    std::vector<std::string> runs;
    std::map<std::string, int> seen;
    for (const PlateCell &c : band) {
        const std::string f = familyOf(c.rgb);
        if (runs.empty() || runs.back() != f) {
            runs.push_back(f);
            ++seen[f];
        }
    }
    int split = 0;
    for (const auto &kv : seen)
        if (kv.second > 1) ++split;
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s: %d families, %d runs", what, int(seen.size()),
                  int(runs.size()));
    chk(split == 0, msg);
    if (split)
        for (const auto &kv : seen)
            if (kv.second > 1)
                std::printf("      %s is in %d pieces\n", kv.first.c_str(), kv.second);
}

int main() {
    std::printf("=== palplate_test ===\n");

    // ---- a real table, built the way the engine builds one ----------------
    Palette pal;
    const int asked = registerModel(&pal, kPine, true) + registerModel(&pal, kPine2, true);
    chk(asked > 0 && pal.used() > mat::TREE_BASE, "the pines registered colours");

    // ...and one EXACT entry, which is what the held kit does.
    const std::array<uint8_t, 4> held{{201, 67, 133, 255}};
    const uint8_t heldId = pal.forModelColor(held, false, /*exact=*/true);
    chk(heldId != mat::AIR && pal.authoredExact(heldId), "an exact entry reports itself exact");

    std::vector<PlateCell> terrain, model;
    int foliage = 0, lifted = 0;
    buildCells(pal, &terrain, &model, &foliage, &lifted);
    chk(foliage > 0, "the pines brought foliage entries");
    chk(lifted == foliage, "EVERY foliage cell differs from what it renders");
    chk(int(terrain.size()) == int(mat::TREE_BASE) - 1, "the terrain band is ids 1..61");

    chk(writePalettePlate(kOut, terrain, model, pal.used(), 0), "the plate was written");
    int bandStart = 0;
    const std::vector<PlateCell> expect = expectedCells(terrain, model, &bandStart);
    chk(bandStart % 8 == 0, "with room to spare, the model band starts on a row");
    checkAgainst(expect, model, bandStart, "the pines");
    familiesContiguous(sortPlateBand(model), "the pines");

    // ---- ...AND THE SAME WITH THE TABLE AT ITS CEILING --------------------
    //
    // 255 is a format limit -- the id is eight bits of the packed triangle word
    // -- and the wood already sits at 254 of it. A plate that only works while
    // there is headroom is a plate that stops working exactly when the table
    // becomes worth looking at.
    std::printf("\n  -- a table filled to the ceiling --\n");
    Palette full;
    for (int i = 0; i < 2000 && full.used() < int(mat::COUNT); ++i) {
        const std::array<uint8_t, 4> c{{uint8_t((i % 13) * 20), uint8_t(((i / 13) % 13) * 20),
                                        uint8_t(((i / 169) % 13) * 20), 255}};
        full.forModelColor(c, false);
    }
    chk(full.used() == int(mat::COUNT), "the table is full");

    std::vector<PlateCell> fterrain, fmodel;
    int ffoliage = 0, flifted = 0;
    buildCells(full, &fterrain, &fmodel, &ffoliage, &flifted);
    chk(int(fterrain.size()) + int(fmodel.size()) == int(mat::COUNT) - 1,
        "every entry became a cell");
    chk(writePalettePlate(kOut, fterrain, fmodel, full.used(), 0), "the full plate was written");
    int fband = 0;
    const std::vector<PlateCell> fexpect = expectedCells(fterrain, fmodel, &fband);
    checkAgainst(fexpect, fmodel, fband, "a full table");
    familiesContiguous(sortPlateBand(fmodel), "a full table");


    // ---- ...AND THE RAMPS MOVE WHOLE ------------------------------------
    //
    // The terrain band cannot be sorted by cell: six greens sampled across the
    // pines are a GRADIENT, and shuffling them by brightness into six places
    // leaves no ramp to read. orderBlocksByHue moves the ramp instead, so what
    // has to hold is BOTH: every block comes out unbroken and in its own order,
    // and the blocks land beside their hue neighbours.
    std::printf("\n  -- ramps move whole --\n");
    auto ramp = [](int id0, std::array<uint8_t, 3> a, std::array<uint8_t, 3> b, int n) {
        std::vector<PlateCell> v;
        for (int i = 0; i < n; ++i) {
            PlateCell c;
            c.id = id0 + i;
            const float t = float(i) / float(n - 1);
            for (int k = 0; k < 3; ++k)
                c.rgb[size_t(k)] =
                    uint8_t(int(a[size_t(k)]) + int(t * float(int(b[size_t(k)]) - int(a[size_t(k)]))));
            v.push_back(c);
        }
        return v;
    };
    const std::vector<PlateCell> grass = ramp(7, {{40, 70, 30}}, {{110, 160, 70}}, 6);
    const std::vector<PlateCell> soil = ramp(13, {{60, 40, 25}}, {{130, 95, 60}}, 4);
    const std::vector<PlateCell> bgrass = ramp(32, {{55, 85, 35}}, {{125, 175, 80}}, 6);
    const std::vector<PlateCell> laid = orderBlocksByHue({grass, soil, bgrass});
    chk(laid.size() == grass.size() + soil.size() + bgrass.size(), "every ramp cell survived");
    // The two green ramps are one run of twelve, whichever order they take.
    int greenRun = 0, best = 0;
    for (const PlateCell &c : laid) {
        const bool green = c.rgb[1] > c.rgb[0] && c.rgb[1] > c.rgb[2];
        greenRun = green ? greenRun + 1 : 0;
        best = greenRun > best ? greenRun : best;
    }
    chk(best == 12, "the two woods' green ramps ended up adjacent");
    // ...and each ramp is still its own gradient, in id order: an id may only
    // follow its predecessor or start a block.
    bool intact = true;
    for (size_t i = 1; i < laid.size(); ++i) {
        const int id = laid[i].id;
        const bool starts = (id == 7 || id == 13 || id == 32);
        if (id != laid[i - 1].id + 1 && !starts) intact = false;
    }
    chk(intact, "no ramp was broken in the middle");

    std::printf("%s\n", fails ? "FAILED" : "all good");
    std::remove(kOut);
    return fails ? 1 : 0;
}
