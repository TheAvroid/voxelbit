// ---------------------------------------------------------------------------
// palplate.h -- v2's LIVE material table, written out as a MagicaVoxel plate.
//
// WHY THIS EXISTS. v2's palette is not a file. It is BUILT at boot, entry by
// entry, as each asset loads: Palette::forModelColor mints a slot the first
// time a colour is asked for and every later asset that is near it SHARES that
// slot. So what the table holds -- and the order it holds it in -- is a
// property of the whole load sequence (the terrain ramps, then the pines, the
// birches, the rocks, the decor, the held kit's reservation, then the flyer
// band) and there has never been a static list of it to open in MagicaVoxel.
// Authoring new art has therefore meant guessing at what is already in there.
// This writes the real thing out, straight off the running engine.
//
// WHY IT MATTERS WHEN AUTHORING, AND IT MATTERS MORE HERE THAN IN v1. A
// palette id in v2 is a MATERIAL: roughness, specular, translucency, alpha and
// ior all hang off the entry, and Palette::forModelColor decides a conifer's
// green is FOLIAGE from the colour alone and gives that entry a needle's 0.45
// translucency and a 1.7x lift. Two colours that land on one entry are one
// surface. So the difference between picking a colour that is exactly on this
// plate and one that is merely close is not cosmetic:
//
//   * same kQuantStep bucket -> you get that entry, for nothing, and you know
//                               what it means. THIS IS WHAT THE PLATE IS FOR.
//   * within kModelMatch (16) of an existing entry, for a caller that passes a
//     tolerance (the flyer band does) -> you are handed SOMEBODY ELSE'S entry
//     and its material with it. v1's own note on this is a pink bird that
//     landed 5/255 from a cactus flower, inherited cactusTab and stung the
//     player.
//   * further out -> a new id is minted IF THE TABLE HAS ROOM. It holds 255
//     and that is a format limit, not a budget -- the id is eight bits of the
//     packed triangle word. Past the ceiling forModelColor returns mat::AIR
//     and the model simply is not drawn, silently. See the start-up line.
//
// THE COLOUR ON THE PLATE IS THE AUTHORED ONE, NOT THE RENDERED ONE, and for
// foliage those are not the same number. A needle's albedo is lifted 1.7x with
// a floor under its blue before it is stored, so reading the plate back out of
// MaterialLook would hand you a green no .vox file contains and that nothing
// would ever match against. Palette remembers what the art actually carried
// (see authoredColor) for exactly this. Foliage cells are marked `~` in the map
// below, because what you will SEE in the wood is that entry lifted.
//
// THE LAYOUT. An 8x32 plate -- MagicaVoxel's palette panel is eight wide, so a
// plate row is a panel row -- in two bands:
//
//   the TERRAIN band first, ids 1..61, as RAMPS: each ramp (six greens off the
//   pines, four soils, three litters, six stones off the boulders, four sands,
//   two wheat ramps of ten) keeps its own id order, because that order IS the
//   gradient -- and the ramps themselves are ordered by hue, so the two woods'
//   greens sit together instead of eight rows apart. THEY ARE NOT AUTHORABLE:
//   nearestModelColor starts its scan at mat::TREE_BASE, so no colour in a
//   .vox file can ever be handed one of these. They are on the plate to be
//   matched BY EYE, so a new asset sits in the wood's own range rather than
//   beside it.
//
//   then the MODEL band, ids 62 and up, in ONE continuous run: near-neutrals
//   first by brightness -- and NEAR-NEUTRAL IS A CHROMA TEST, not a
//   saturation one, or the near-blacks are sorted by a hue nobody can see
//   and land as a hole in the middle of the colour (see kGreyChroma) --
//   then every chromatic colour in hue order, serpentined
//   within 15 degree buckets so each bucket ends where the next begins. One
//   run means ONE PLACE PER FAMILY -- see the note over sortPlateBand for the
//   rule that replaced and why it had to.
//
// The two bands are separated by whatever black it takes to start the model
// band on a fresh row, and the free tail is black too: an unminted id is simply
// absent, so without the padding the plate would just stop early and the
// headroom -- the whole thing you want to know before adding an asset -- would
// be invisible. Id 0 is air and nothing in the table is (0,0,0) unless an asset
// authored it black, so a black cell reads as "free" everywhere but in the
// skunk's family.
//
// THE INDEX IS NOT THE ID. A cell's position here is its sort position, and the
// engine never reads this file: the loaders match your voxels' RGB against the
// table (forModelColor takes the colour, not an index), so the palette index
// your .vox happens to carry is never consulted. That is what makes sorting
// safe, and "grouped by hue" beats "grouped by load order" for authoring
// against. The map printed on the way out gives the real ids back, for when you
// need to ask what a colour MEANS rather than what it looks like.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace v2 {

// One cell of the plate. A free slot is id 0, drawn black.
struct PlateCell {
    int id = 0;
    std::array<uint8_t, 3> rgb{{0, 0, 0}};
    bool exact = false;    // minted under kExactKeyBit -- the held kit, unquantised
    bool foliage = false;  // classified as a needle: translucent, and lifted when it renders
};

namespace plate {

constexpr int kWidth = 8;    // MagicaVoxel's palette panel, so a plate row IS a panel row
constexpr int kHeight = 32;  // 8 x 32 = 256, one more cell than the table can ever hold
constexpr int kMaxCells = 255;

// Saturation below which a colour is a NEUTRAL and is sorted by brightness
// alone. v1's tool used 0.12 and found it far too generous -- a colour at 0.11
// and hue 340 is a pink, not a grey, and it was landing in the first rows
// anybody looks at, interleaved with the true neutrals by lightness.
constexpr float kGreySat = 0.05f;
constexpr float kHueBucket = 15.0f;

// -- ...AND SATURATION ALONE IS NOT THE TEST -----------------------------
//
// A near-BLACK has whatever saturation it likes and no hue anybody can see.
// MEASURED on the real plate, in the eight cells that sat between the browns
// and the yellows and read as a hole in the middle of the colour:
//
//     1c1915   s 0.14  v 0.11   -> sorted as an orange. It is black.
//     0d0c0b   s 0.14  v 0.05
//     645f5a   s 0.10  v 0.39   -> a warm grey, sorted as an orange
//     272723   s 0.13  v 0.15   -> sorted as a YELLOW, in among the greens
//
// against colours at the same saturation that are real:
//
//     322417   s 0.54  v 0.20   -> a dark brown, and it belongs in the browns
//     3a5300   s 1.00  v 0.33   -> a deep green
//
// What separates them is CHROMA -- saturation times value, how much colour is
// actually present -- not saturation, which is a ratio and says nothing about
// how much light is there to be coloured. Below this, a colour goes to the
// neutrals and sorts by brightness with the rest of them.
constexpr float kGreyChroma = 0.05f;

// Is there a hue here worth sorting by?
inline bool neutral(float s, float v) { return s < kGreySat || s * v < kGreyChroma; }

inline void hsv(const std::array<uint8_t, 3> &c, float *h, float *s, float *v) {
    const float r = float(c[0]) / 255.0f, g = float(c[1]) / 255.0f, b = float(c[2]) / 255.0f;
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    *v = mx;
    *s = (mx <= 0.0f) ? 0.0f : (mx - mn) / mx;
    if (mx == mn)
        *h = 0.0f;
    else if (mx == r)
        *h = std::fmod(60.0f * (g - b) / (mx - mn) + 360.0f, 360.0f);
    else if (mx == g)
        *h = 60.0f * (b - r) / (mx - mn) + 120.0f;
    else
        *h = 60.0f * (r - g) / (mx - mn) + 240.0f;
}

inline void u32le(std::vector<uint8_t> *out, uint32_t v) {
    out->push_back(uint8_t(v & 0xffu));
    out->push_back(uint8_t((v >> 8) & 0xffu));
    out->push_back(uint8_t((v >> 16) & 0xffu));
    out->push_back(uint8_t((v >> 24) & 0xffu));
}

// Little-endian chunk: four-byte id, content size, children size, then both.
inline void chunk(std::vector<uint8_t> *out, const char *id, const std::vector<uint8_t> &content,
                  const std::vector<uint8_t> &children = std::vector<uint8_t>()) {
    for (int i = 0; i < 4; ++i) out->push_back(uint8_t(id[i]));
    u32le(out, uint32_t(content.size()));
    u32le(out, uint32_t(children.size()));
    out->insert(out->end(), content.begin(), content.end());
    out->insert(out->end(), children.begin(), children.end());
}

}  // namespace plate

// -- THE SORT: ONE PASS, SO A FAMILY IS ONE PLACE --------------------------
//
// v1's rule was inherited here and had to go (user 2026-09-15: "greens should
// be next to greens, reds should be next to reds"). Over there each family
// takes as many WHOLE ROWS OF EIGHT as it can fill and the remainder pools at
// the bottom of the plate, so that a row is one family rather than the seam
// between two.
//
// THAT RULE SPLITS EVERY FAMILY SMALLER THAN A ROW IN TWO, and v2's table is
// full of those. MEASURED on the first real plate, over the 192 model entries:
//
//     family   cells  runs  split as
//     orange      47     5  [16, 6, 16, 7, 2]
//     red         43     5  [22, 9, 8, 1, 3]
//     grey        34    11  [21, 1, 1, ...]
//     green       21     3  [8, 2, 11]
//     ...        8 families in 33 RUNS
//
// Eleven of anything is one row of eight and three left over -- and those three
// went to the pool forty cells away, beside three oranges and two yellows from
// other families' remainders. Every family appeared twice, which is exactly the
// thing a plate is meant to prevent.
//
// SO THE ROW ALIGNMENT IS WHAT YIELDS, and it is the cheaper of the two. A
// family that starts mid-row costs the reader nothing they cannot see; a family
// in three pieces costs them the search that the plate exists to end.
//
// Neutrals lead, darkest to lightest -- hue is meaningless below a few per cent
// saturation, and sorting greys by it interleaves the stone, the bark shadows
// and the snow at random. Then every chromatic colour in hue order, one
// continuous run from red through green and blue back to pink.
//
// AND THE RUN SERPENTINES, which is the one piece of v1 worth keeping. Sorting
// every bucket light-to-dark makes a SAWTOOTH: each bucket ends dark and the
// next restarts light, so the seam between two families is the biggest jump on
// the plate even though each family is ordered. Alternating the direction means
// each bucket ENDS where the next BEGINS, and the whole band reads as one ramp.
inline std::vector<PlateCell> sortPlateBand(std::vector<PlateCell> band) {
    struct Keyed {
        PlateCell c;
        float h = 0.0f, s = 0.0f, v = 0.0f;
    };
    std::vector<Keyed> greys;
    std::vector<std::vector<Keyed>> buckets(size_t(360.0f / plate::kHueBucket) + 1);
    for (const PlateCell &c : band) {
        Keyed k;
        k.c = c;
        plate::hsv(c.rgb, &k.h, &k.s, &k.v);
        if (plate::neutral(k.s, k.v))
            greys.push_back(k);
        else
            buckets[size_t(k.h / plate::kHueBucket)].push_back(k);
    }
    // Dark to light, so the neutrals hand over to the first hue at their light
    // end and the seam is a light grey beside a light red.
    std::sort(greys.begin(), greys.end(), [](const Keyed &a, const Keyed &b) { return a.v < b.v; });

    std::vector<PlateCell> ordered;
    for (const Keyed &k : greys) ordered.push_back(k.c);

    int emitted = 0;   // counts NON-EMPTY buckets, so a gap in the hue circle
                       // cannot flip the serpentine and undo the seam it fixes
    for (auto &b : buckets) {
        if (b.empty()) continue;
        const bool lightFirst = (emitted++ % 2) == 0;
        std::sort(b.begin(), b.end(), [lightFirst](const Keyed &a, const Keyed &c) {
            if (a.v != c.v) return lightFirst ? a.v > c.v : a.v < c.v;
            return a.s > c.s;   // at equal brightness, the saturated one leads
        });
        for (const Keyed &k : b) ordered.push_back(k.c);
    }
    return ordered;
}

// -- ...AND THE TERRAIN BAND, WHERE THE RAMP IS THE UNIT -------------------
//
// The same complaint applies to the first rows and the fix cannot be the same.
// Those entries are RAMPS -- six greens sampled off the pines, four soils, six
// stones off the boulders, ten wheat shades -- and a ramp's order is its own:
// sorting the CELLS would shuffle six greens by brightness into six different
// places and destroy the one thing they are.
//
// So the ramp moves as a block. Each block keeps its id order, and the blocks
// are ordered by the hue of their mean colour, greys first -- which puts the
// grass beside the birch grass beside the moss, the soils beside the litter,
// and the stone beside the bedrock, instead of the id order that had the green
// ramps eight rows apart because one wood was added after the other.
inline std::vector<PlateCell> orderBlocksByHue(std::vector<std::vector<PlateCell>> blocks) {
    struct Keyed {
        std::vector<PlateCell> cells;
        float h = 0.0f, s = 0.0f, v = 0.0f;
    };
    std::vector<Keyed> keyed;
    for (auto &b : blocks) {
        if (b.empty()) continue;
        Keyed k;
        int r = 0, g = 0, bl = 0;
        for (const PlateCell &c : b) {
            r += c.rgb[0];
            g += c.rgb[1];
            bl += c.rgb[2];
        }
        const int n = int(b.size());
        const std::array<uint8_t, 3> mean{{uint8_t(r / n), uint8_t(g / n), uint8_t(bl / n)}};
        plate::hsv(mean, &k.h, &k.s, &k.v);
        k.cells = std::move(b);
        keyed.push_back(std::move(k));
    }
    std::sort(keyed.begin(), keyed.end(), [](const Keyed &a, const Keyed &b) {
        const bool ga = plate::neutral(a.s, a.v), gb = plate::neutral(b.s, b.v);
        if (ga != gb) return ga;                  // neutrals lead, as above
        if (ga) return a.v < b.v;                 // ...darkest first
        return a.h < b.h;
    });
    std::vector<PlateCell> out;
    for (const Keyed &k : keyed)
        for (const PlateCell &c : k.cells) out.push_back(c);
    return out;
}

// ---------------------------------------------------------------------------
// Write the plate. `terrain` is expected in id order and `model` unsorted;
// `used` and `lost` are Palette::used() and overflowedColors(), reported here
// so the file and the number that explains it arrive together.
//
// Returns false only if the file could not be opened.
// ---------------------------------------------------------------------------
inline bool writePalettePlate(const std::string &path, const std::vector<PlateCell> &terrain,
                              std::vector<PlateCell> model, int used, int lost) {
    std::vector<PlateCell> cells = terrain;
    // Start the model band on a fresh row -- a family that begins mid-row is
    // the one thing the whole sort exists to prevent.
    //
    // ...UNLESS THE TABLE IS TOO FULL TO AFFORD IT, WHICH IT NOW IS. Measured
    // on the first real plate: 61 terrain entries and 192 model ones is 253,
    // three cells of gap makes 256, and 255 is every cell there is. The gap won
    // and the last model colour was dropped off the end -- an entry the engine
    // holds, silently missing from the one picture of the engine's colours.
    //
    // A ragged seam costs a reader one row of confusion. A missing swatch costs
    // them an authored colour they will then mint a second time against a table
    // with ONE slot left in it. So the alignment is the thing that yields.
    const size_t pad = (size_t(plate::kWidth) - cells.size() % size_t(plate::kWidth)) %
                       size_t(plate::kWidth);
    const bool aligned = cells.size() + pad + model.size() <= size_t(plate::kMaxCells);
    if (aligned)
        for (size_t i = 0; i < pad; ++i) cells.push_back(PlateCell());
    const size_t modelRow = cells.size() / size_t(plate::kWidth);
    const std::vector<PlateCell> sorted = sortPlateBand(std::move(model));
    cells.insert(cells.end(), sorted.begin(), sorted.end());
    // The padding can only cost what the free tail was holding, and 255 minus
    // the table is what is left. Truncating here rather than asserting because
    // a plate one row short is still a useful plate, and a full table is the
    // day you least want the tool that reports it to refuse.
    if (cells.size() > size_t(plate::kMaxCells)) cells.resize(size_t(plate::kMaxCells));
    const size_t live = cells.size();
    while (cells.size() < size_t(plate::kMaxCells)) cells.push_back(PlateCell());   // free: black

    std::vector<uint8_t> xyzi, rgba(1024, 0), size, body, file;
    plate::u32le(&xyzi, uint32_t(cells.size()));
    for (size_t n = 0; n < cells.size(); ++n) {
        const uint8_t mv = uint8_t(n + 1);   // MagicaVoxel colour indices start at 1
        xyzi.push_back(uint8_t(n % size_t(plate::kWidth)));
        xyzi.push_back(uint8_t(n / size_t(plate::kWidth)));
        xyzi.push_back(0);
        xyzi.push_back(mv);
        const size_t o = size_t(mv - 1) * 4;   // ...and are stored at (i - 1) * 4
        rgba[o] = cells[n].rgb[0];
        rgba[o + 1] = cells[n].rgb[1];
        rgba[o + 2] = cells[n].rgb[2];
        rgba[o + 3] = 255;   // v2 carries alpha on the MATERIAL, never on the palette
    }
    plate::u32le(&size, uint32_t(plate::kWidth));
    plate::u32le(&size, uint32_t(plate::kHeight));
    plate::u32le(&size, 1u);
    plate::chunk(&body, "SIZE", size);
    plate::chunk(&body, "XYZI", xyzi);
    plate::chunk(&body, "RGBA", rgba);
    const char kMagic[4] = {'V', 'O', 'X', ' '};
    for (int i = 0; i < 4; ++i) file.push_back(uint8_t(kMagic[i]));
    plate::u32le(&file, 150u);
    plate::chunk(&file, "MAIN", std::vector<uint8_t>(), body);

    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);

    // -- THE MAP, which is the half of this that is not a picture ---------
    //
    // The plate answers "what colour"; only the id answers "what material" --
    // which entry the firefly's glow is on, which greens are needles, which of
    // them the held kit reserved exactly. Printed rather than written beside
    // the file because it is read once, while deciding.
    std::printf("\n  index -> v2 material id   (index is PLATE POSITION; v2 matches art by RGB,\n");
    std::printf("                             never by index. `~` foliage, `*` exact/held kit)\n");
    for (size_t row = 0; row * size_t(plate::kWidth) < live; ++row) {
        std::printf("   %s %3d:", row == modelRow ? "model" : (row < modelRow ? "terra" : "     "),
                    int(row * size_t(plate::kWidth)) + 1);
        for (int k = 0; k < plate::kWidth; ++k) {
            const size_t n = row * size_t(plate::kWidth) + size_t(k);
            if (n >= live || cells[n].id == 0) {
                std::printf("    .");
                continue;
            }
            std::printf(" %3d%c", cells[n].id,
                        cells[n].exact ? '*' : (cells[n].foliage ? '~' : ' '));
        }
        std::printf("\n");
    }
    std::printf("\nv2: wrote %s\n", path.c_str());
    std::printf("    %d of 255 table entries on an 8x%d plate, %d slot(s) still free%s\n", used,
                plate::kHeight, int(plate::kMaxCells) - int(live),
                lost ? "  -- AND THE TABLE OVERFLOWED, see the palette line above" : "");
    if (!aligned)
        std::printf("    the model band starts mid-row: the table is too full to spend %d "
                    "cell(s) on a gap\n",
                    int(pad));
    std::fflush(stdout);
    return true;
}

}  // namespace v2
