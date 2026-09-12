// ---------------------------------------------------------------------------
// generate.h -- what goes in the world, so that there is something to trace.
//
// GPU-FREE, LIKE bricks.h. It writes voxels through BrickWorld::set and knows
// nothing about acceleration structures, buffers or Falcor, which is what lets
// the test harness build the same wood a run builds and check it.
//
// ---------------------------------------------------------------------------
// THE WOOD IS THE AUTHORED ONE. Nine pine models, the boulders, the toadstools
// and the flowers, out of `game/assets` -- the same files every other engine in
// this project draws, so a seed means the same wood in all of them. They are
// stamped into the brick world as ordinary voxels: a pine is not an instance
// standing beside the ground, it IS ground, made of the same lattice.
//
// THE GRASS IS GENERATED AND THAT IS NOT AN OMISSION. A strand is two to five
// voxels; describing one in a file would cost more than computing it. And it is
// not a performance cost either -- measured on this world in the triangle
// engine, HAVING grass ran 18 % FASTER than not having it, because blades stop
// long grazing rays a few metres out instead of letting them run to the
// horizon. The instinct to thin it out is backwards.
//
// WITH NO ASSETS IT STILL RENDERS. Everything below has a procedural fallback
// -- a generated conifer, a noise-dented boulder, a minted palette -- so a
// missing or wrongly-pointed asset folder gives a plainer wood and a line in
// the status, not a black frame. --no-models forces that path for an A/B.
//
// ---------------------------------------------------------------------------
// THE GROUND IS A SHELL, AND THAT IS A MEMORY DECISION, NOT A MODEL ONE.
//
// A brick costs 580 bytes on the HOST whether it is full or empty, because its
// material array is dense there (see bricks.h). Filling a 192 m square down to
// bedrock is about 3.8 M bricks -- 2.2 GB -- to describe rock nobody can ever
// see, since the packer culls every brick that is walled in on all six sides
// before the hardware is offered one.
//
// So the generator lays a shell `skinM` deep under the surface and stops. The
// underside is open, and a camera that goes below the ground looks up through
// it; the camera flies and there is nothing to dig with, so nothing in this
// engine can reach that. A backend that gains digging deepens the skin, and
// pays for it.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../core/noise.h"
#include "bricks.h"
#include "materials.h"
#include "models.h"

namespace v4 {

// ---------------------------------------------------------------------------
// What to build.
// ---------------------------------------------------------------------------
struct GenOptions {
    uint32_t seed = 20260911u;

    // The square of world that gets voxels, centred on the origin, in metres.
    // Everything outside it is sky, and a ray that leaves it simply misses.
    float extentM = 192.0f;

    // -- THE COLUMN, IN LAYERS --------------------------------------------
    //
    // Surface, soil, stone, bedrock -- 526 voxels, 52.6 m, under every column.
    //
    // THE PEEL IS WHAT MAKES THIS AFFORDABLE. A hundred voxels of stone sounds
    // ruinous and costs the DEVICE almost nothing: every voxel with six opaque
    // neighbours is dropped before upload, so all that survives of the stone is
    // where it meets air -- a cliff face, the wall of a lake basin -- and all
    // that survives of the bedrock is its underside. What it does cost is HOST
    // memory during generation, because BrickWorld holds the world as written.
    // See the note at the top of this file.
    int soilVox = 15;     // 1.5 m of dirt
    int stoneVox = 500;   // 50 m of stone
    int bedrockVox = 10;  // 1 m of bedrock at the floor of the world

    // Write every buried voxel out by hand instead of handing the interior to
    // ImplicitColumns. Slow, enormous, and produces byte-identical device
    // buffers -- which is the point: it is what the harness checks the fast
    // path against.
    bool explicitFill = false;

    // A CAP on the whole column, in metres, for a cheap run. Zero is no cap --
    // lay the full stack above. This is what --skin used to mean when the world
    // was a 2.4 m shell and nothing lived under it.
    float skinM = 0.0f;

    // Mean ground height. Lifted a metre with the relief, purely so the valley
    // floors stay above the origin now that the hills swing twice as far: at
    // 5 the lowest ground measured -0.24 m, and a wood with its feet through
    // y = 0 is a needless thing to have to reason about.
    float baseM = 6.0f;
    // HOW FAR THE HILLS SWING ABOUT IT -- DOUBLED, from 11. The landform this
    // drives is domed rather than ridged (see terrainH), and the two changes
    // belong together: doubling the swing of a ridged field would have doubled
    // the number of faces too steep to hold soil, and a pine wood on bare rock
    // is not a pine wood.
    float reliefM = 22.0f;
    // Wavelength of the lowest octave -- widened by a third along with the
    // relief, so the hills grow in both directions at once. Twice as tall over
    // the same footprint is twice as steep; twice as tall over a third again
    // as much ground is a BIGGER hill rather than a sharper one, and measured,
    // this pairing comes out at a mean slope of 0.216 against the old field's
    // 0.234 -- gentler underfoot while standing twice as high, which is what
    // round-and-hilly means. It also keeps bare rock at nil: nothing on this
    // landform is too steep to hold soil.
    float hillM = 92.0f;

    // HOW ROUND THE HILLS ARE. The body fbm is stretched this far about its
    // mean before the rounding curve is applied (see terrainH). 1 leaves it in
    // the straight middle of sstep and the landform comes out as one soft
    // sheet; too much drives the tails into the clamp and gives plateaux with
    // cliffs between them. It is also the knob that sets how much of the
    // relief is actually USED -- a value fbm never reaches its own extremes, so
    // without the stretch a doubled relief would not read as doubled.
    float domeGain = 1.10f;
    // The fine roll's share of the relief. The ridge term it replaces took 0.30
    // -- this is deliberately a third of that, because the roll dresses the
    // hills rather than shaping them.
    float rollFrac = 0.085f;

    // Where the .vox files live. Empty means "use the procedural fallback".
    std::string pineDir;
    std::string decorDir;
    bool models = true;

    // The spacing of the scatter grids, in metres.
    //
    // EACH SET NEEDS A CELL WIDER THAN THE THING IT SCATTERS, and the toadstool
    // is the one that catches you out: rock.vox is 40 cm across but
    // mushroom.vox is 2.3 METRES TALL -- a fantasy toadstool, not a button. On
    // a 2.2 m grid it becomes a solid field of them, and one standing beside a
    // 2 m eye fills the frame with a smooth white mass that reads as a
    // rendering fault rather than as content. The authored pines are 7 to 11 m
    // across, so a grid tighter than 11 m merges the crowns into one ceiling
    // you can see neither the sky nor the ground through.
    float treeCellM = 11.0f;
    // Three boulder sets, three grids -- see the note on Models::rocksBig. The
    // big five are 12 to 23 m: they are landmarks, a handful to a wood, and on
    // anything like the small stones' grid they would overlap each other and
    // stand inside the trees.
    float rockBigCellM = 55.0f;
    float rockMidCellM = 22.0f;
    float rockCellM = 4.6f;
    float mushroomCellM = 15.0f;
    float flowerCellM = 2.2f;

    bool trees = true;
    bool rocks = true;
    bool grass = true;

    // -- ORE SEAMS IN THE STONE -------------------------------------------
    //
    // A cell either holds a seam or it does not, decided by hashing it -- the
    // same scatter every other set uses, run once per DEPTH BAND so each band
    // is populated independently.
    //
    // A SEAM IS THREE TO SIX BLOBS OF ONE MINERAL, jittered about a point, so
    // finding one means finding several of the same kind together. Scattering
    // single blobs gives a uniform speckle of everything everywhere, which is
    // the one thing a mineral deposit is not.
    std::string mineralPath;
    bool minerals = true;
    float oreCellM = 5.0f;    // the grid a seam is placed on
    float oreSpreadM = 1.6f;  // how far the blobs of one seam scatter
    int oreMin = 3, oreMax = 6;

    // THE MEAN FRACTION OF COLUMNS CARRYING A STRAND, averaged over the patch
    // field below -- not the fraction in any given place, which swings from
    // about an eighth of this to nearly twice it.
    //
    // The old flat gate kept 151 of 256 columns, or 0.590. Half of that is
    // 0.295, and this is a little over it because the field and the gate do not
    // compose linearly: a patch whose probability saturates at 1 cannot give
    // back what the clearings took. 0.307 is the value measured to halve the
    // strand count exactly -- 2,174,341 columns down to 1,087,431 on the
    // default world.
    float grassDensity = 0.307f;
    // How wide a meadow or a clearing is, in metres. Well under the tree grid,
    // so a patch is something you stand in rather than something you fly over,
    // and well over a strand, so the edge of one reads as an edge.
    float grassPatchM = 24.0f;

    // -- WATER -------------------------------------------------------------
    //
    // A MATERIAL, NOT A SURFACE SOMEONE DRAWS. mat::WATER is a voxel like any
    // other: a lake has a bed, a bank and a real depth, and the tracer's
    // dielectric branch keys off the material and off WHICH FACE was hit, so
    // the top ripples and the sides do not. It needs no second acceleration
    // structure -- which is the design that made the old per-chunk water BLAS
    // and its compaction collision impossible rather than fixed.
    bool water = true;

    // THE LINE IS A PERCENTILE OF THIS WORLD'S OWN HEIGHTS, not a number
    // carried over from another engine. That mistake has been made twice here:
    // a waterline of 2.6 m against a terrain whose lowest ground was 20.6 sat
    // eighteen metres under everything and read as "water is broken", and a
    // basin threshold of 0.065 fired on a field that bottomed out at 0.147.
    // Measured on v4's terrain: min 0.76, median 9.60, max 14.11 m -- so the
    // twelfth percentile is about 6.1 m and puts a tenth of the world under
    // water with lakes up to five metres deep.
    float waterPct = 0.12f;
    float waterM = 0.0f;  // an ABSOLUTE line in metres; 0 uses the percentile

    // How far above the line the sand reaches, and how hard the beach flattens
    // AT THE WATER'S EDGE. A bank that keeps the hillside's own slope is a
    // hillside that happens to be yellow; what reads as a shore is the BREAK in
    // slope.
    //
    // bankFlat is the slope multiplier at the waterline only. It is eased back
    // to 1.0 -- the ground's own slope -- by the top of the band, because a
    // CONSTANT multiplier does not meet the terrain it borders: at 0.25 over a
    // nine-voxel rise the last bank column sits at waterY+3 and its untouched
    // neighbour at waterY+10, which is a seven-voxel cliff ringing every lake.
    // See the ease in the bank pass.
    float bankRiseM = 0.9f;
    float bankFlat = 0.25f;

    // Build the small acceptance scene instead of a landscape. See buildDemo.
    bool demo = false;
};

// ---------------------------------------------------------------------------
// The palette slots and the models, built together because the palette IS
// built from the models. What the generator carries around.
// ---------------------------------------------------------------------------
// HOW FAR A SET'S WIDEST PIECE REACHES FROM ITS ANCHOR, in voxels.
//
// The margin a chunk must search for models it does not own -- see ClipWriter.
// Measured off the pieces rather than assumed: a big boulder reaches 12.7 m and
// a flower 20 cm, so a flower pass given the boulder's margin looks at 7.6
// times the ground it can possibly need.
//
// AND IT BOUGHT NOTHING MEASURABLE, which is worth recording. Per-class margins
// took a chunk crossing from 548 ms to 540 -- inside the noise. The scan is a
// hash of a cell coordinate and a bounds test; what a chunk actually spends its
// time on is evaluating the noise for its strip of new columns and writing half
// a million voxels into the brick map. The margin is kept because it is the
// honest bound and it stops the cost growing if a wider model is ever added,
// not because it made anything faster.
inline int pieceReach(const std::vector<VoxPiece> &set, int floorVox) {
    int m = floorVox;
    for (const VoxPiece &p : set)
        for (const VoxPiece::Vox &v : p.vox) {
            const int ax = v.x < 0 ? -int(v.x) : int(v.x);
            const int az = v.z < 0 ? -int(v.z) : int(v.z);
            if (ax > m) m = ax;
            if (az > m) m = az;
        }
    return m + 2;
}

struct Content {
    Models models;

    // ...computed ONCE per world, in buildPalette. Walking every voxel of every
    // model is nothing at load and is 81 times too much per chunk.
    int reachPine = 40, reachBig = 0, reachMid = 0, reachSmall = 0;
    int reachMush = 0, reachFlower = 0, reachOre = 0;
    // The procedural fallback's own minted ids, used only when no pines loaded.
    std::vector<uint8_t> bark, needle;
    std::string status;

    bool authored() const { return models.loaded(); }
};

// ---------------------------------------------------------------------------
// MINT THE COLOURS FIRST, AND IN THIS ORDER.
//
// Everything about the ground's colour is derived from what stands on it:
// deriveGroundFromTrees reads the grass, soil and litter ramps off the entries
// the PINES minted, and setStoneBand reads the stone ramp off the boulders. So
// the models have to be loaded before a single voxel of terrain is written, and
// Models::load is where the ordering inside that is enforced -- see the note on
// it.
// ---------------------------------------------------------------------------
inline Content buildPalette(Palette &pal, const GenOptions &o) {
    Content c;

    if (o.models && !o.pineDir.empty()) {
        c.models.mineralPath = o.minerals ? o.mineralPath : std::string();
        c.models.load(o.pineDir, o.decorDir, pal);
    }

    if (!c.models.loaded()) {
        // -- THE FALLBACK, so a missing asset folder is a plainer wood and not
        // a black frame ------------------------------------------------------
        //
        // FOUR BROWNS THAT ARE BROWN ENOUGH TO BE READ AS SOIL.
        // deriveGroundFromTrees accepts a bark entry only if it is warm,
        // mid-dark and saturated between 0.35 and 0.80 -- the floor keeps a
        // grey out and the ceiling keeps a scarlet cut-end out. A pretty,
        // saturated bark of (94, 62, 38) linearises to a saturation of 0.83 and
        // is silently rejected: the ground falls back to its default brown and
        // stops matching the wood, for a reason nothing prints.
        static const uint8_t kBark[4][3] = {
            {110, 84, 60}, {96, 72, 50}, {124, 96, 70}, {84, 64, 46}};
        for (const auto &b : kBark) c.bark.push_back(pal.forModelColor({b[0], b[1], b[2], 255}, true));

        // GREEN-DOMINANT, WHICH IS HOW forModelColor CLASSIFIES FOLIAGE. It is
        // not a palette range: an entry whose green beats both its neighbours
        // gets a needle's roughness, a needle's translucency, and the lift and
        // blue floor that stop a backlit crown reading as a black cut-out.
        static const uint8_t kNeedle[5][3] = {
            {44, 68, 36}, {58, 82, 44}, {36, 56, 30}, {70, 96, 52}, {84, 106, 60}};
        for (const auto &n : kNeedle)
            c.needle.push_back(pal.forModelColor({n[0], n[1], n[2], 255}, true));

        pal.markPinesLoaded();

        // The STONE ramp that mat::ROCK is spread over on the device.
        // setStoneBand keeps only entries unsaturated enough to be stone and
        // needs at least STONE_COUNT of them, or it silently leaves the
        // defaults -- so these are all near-neutral and there are more of them
        // than the ramp needs.
        std::vector<std::array<uint8_t, 4>> stone;
        static const uint8_t kGrey[8] = {52, 64, 76, 88, 100, 112, 124, 136};
        for (uint8_t v : kGrey)
            stone.push_back({v, uint8_t(v - v / 24), uint8_t(v - v / 12), 255});
        pal.setStoneBand(stone);
    }

    pal.deriveGroundFromTrees();

    // The scatter margins, measured off whatever actually loaded. The floor on
    // the pines covers the procedural fallback's 38-voxel crown, which is used
    // when no .vox tree did.
    c.reachPine = pieceReach(c.models.pines, 40);
    c.reachBig = pieceReach(c.models.rocksBig, 0);
    c.reachMid = pieceReach(c.models.rocksMid, 0);
    c.reachSmall = pieceReach(c.models.rocksSmall, 0);
    c.reachMush = pieceReach(c.models.mushrooms, 0);
    c.reachFlower = pieceReach(c.models.flowers, 0);
    for (int q = 0; q < int(Ore::Count); ++q)
        c.reachOre = std::max(c.reachOre, pieceReach(c.models.ore[q], 0));

    c.status = c.models.loaded() ? c.models.status
                                 : std::string("generated -- no .vox models (") +
                                       (o.models ? c.models.status : std::string("--no-models")) +
                                       ")";
    return c;
}

// ---------------------------------------------------------------------------
// THE HEIGHT FIELD -- ROUND HILLS, NOT RIDGES, AND THAT IS A DELIBERATE SWAP.
//
// This used to be warped fbm plus a RIDGED multifractal, which puts a crease
// along the top of every rise: 1 - |2n - 1| has a corner at n = 0.5 and the
// corner is the point of it, so a landscape built that way reads as
// ridge-and-valley with sharp spines. That is right for a mountain and wrong
// for a rolling wood, and it is also the one term that fights the relief being
// doubled -- creases get proportionally sharper as the swing grows, so twice
// the elevation with a ridge term is twice the number of faces too steep to
// hold soil, which the bare-column test then turns to bare rock.
//
// So the ridge is gone and the body is DOMED instead. sstep is 3t^2 - 2t^3: its
// derivative is zero at both ends, which is exactly the shape of a hill --
// rounded over the top, rounded into the valley floor, and steepest on the
// flank between them. Stretching the fbm about its mean before the sstep is
// what stops the whole field living in the flat middle of that curve, where it
// would come out as one gentle sheet.
//
// FOUR OCTAVES IN THE BODY, NOT FIVE. The fifth is a tenth of the amplitude at
// a sixteenth of the wavelength, and at double relief that is a metre of chop
// over four metres of ground -- sandpaper on what is meant to be a smooth
// dome. The fine detail the surface still needs comes back below it as `roll`,
// at a twelfth of the relief instead of a third, so it dresses the hills
// rather than deciding their shape.
//
// `ridged()` stays in core/noise.h. Nothing calls it now, but it is the
// primitive a mountain biome would want and it costs nothing to leave.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// THE GRASS PATCH FIELD. Same stretch-then-round shape as the landform, for the
// same reason -- see the grass section of buildTerrain.
//
// kGrassSparse and kGrassFull are a PAIR and must keep summing to 2 either side
// of a half: the sharpened field averages 0.5, so a mean density of exactly
// GenOptions::grassDensity needs kGrassSparse + kGrassFull * 0.5 == 1. Change
// one without the other and the option stops meaning what it says.
// ---------------------------------------------------------------------------
constexpr float kGrassPatchGain = 3.1f;
constexpr float kGrassSparse = 0.12f;        // a clearing: bare litter, a few blades
constexpr float kGrassFull = 1.76f;          // a thicket: as full as the old flat gate

// "There is no water here", as a height. Shared by the waterline, the fill and
// BrickWorld::waterM, so a world with the water switched off says the same
// thing everywhere instead of three sentinels that have to be kept in step.
constexpr int kNoWaterLine = -1000000;

struct TerrainNoise {
    // `rg` held the ridge memo and now holds the fine roll -- same eight
    // octaves of lattice cache, different field sampled through it.
    FbmMemo wx, wz, wm, rg;
};

inline float terrainH(TerrainNoise &n, float x, float z, const GenOptions &o) {
    const float s = 1.0f / o.hillM;
    const float ox = float(o.seed & 0xFFFFu) * 0.017f;
    const float oz = float((o.seed >> 16) & 0xFFFFu) * 0.023f;

    // The body of the landform: warped so the hills meander instead of running
    // along the noise lattice's axes.
    const float body = warpedFbm(n.wx, n.wz, n.wm, x * s + ox, z * s + oz, 0.55f, 4);

    // STRETCH, THEN ROUND. A four-octave value fbm sits well inside [0, 1] --
    // measured, this one spends nearly all its time between about 0.3 and 0.7
    // -- so feeding it straight to sstep would only ever use the straight part
    // of the curve. Widening about 0.5 first puts the tails into the flat ends,
    // which is where the domes and the valley floors come from.
    const float dome = sstep(clampf((body - 0.5f) * o.domeGain + 0.5f, 0.0f, 1.0f));

    // The dressing: a gentle roll three times finer than the hills, at a
    // twelfth of the relief. Enough that a hillside is not a machined surface,
    // small enough that it never makes a slope the soil cannot sit on.
    const float roll = fbm(n.rg, x * s * 3.1f + ox, z * s * 3.1f + oz, 3);

    return o.baseM + o.reliefM * (dome - 0.5f) + o.reliefM * o.rollFrac * (roll - 0.5f);
}

// ---------------------------------------------------------------------------
// THE PROCEDURAL CONIFER, for when there are no models.
//
// Trunk, then whorls of needles up a tapering cone. The shape that matters is
// that a conifer is MOSTLY AIR -- about 1.5 % of its bounding box is solid --
// which is exactly the case a brick store is built for: the ray tracing cores
// skip the empty boxes and only the handful of bricks a branch passes through
// are ever walked. Everything about a tree comes out of the hash of its cell.
// ---------------------------------------------------------------------------
template <class World>
inline void stampPine(World &w, BrickWorld::Cursor &cur, const Content &c, int ci, int cj,
                      int baseY, uint32_t h) {
    if (c.bark.empty() || c.needle.empty()) return;
    const float r0 = hashUnit(h, 11u), r1 = hashUnit(h, 23u), r2 = hashUnit(h, 37u);
    const float r3 = hashUnit(h, 51u), r4 = hashUnit(h, 67u);

    const int height = int(110.0f + 130.0f * r0);       // 11 .. 24 m
    const int trunkR = int(2.0f + 2.0f * r1);           // 20 .. 40 cm
    const float crownR = 16.0f + 22.0f * r2;            // 1.6 .. 3.8 m
    const int spokes = 5 + int(4.0f * r3);              // 5 .. 8 branches a whorl
    const float phase = r4 * 6.2831853f;
    const int whorl = 9 + int(7.0f * hashUnit(h, 83u)); // 0.9 .. 1.6 m apart

    const uint8_t bark = c.bark[h % c.bark.size()];
    // Two greens per tree rather than one, picked from the ramp: a crown of a
    // single flat value reads as plastic no matter how good the lighting is.
    const uint8_t nA = c.needle[(h >> 3) % c.needle.size()];
    const uint8_t nB = c.needle[(h >> 7) % c.needle.size()];

    for (int y = -4; y < height; ++y) {
        const float t = float(y) / float(height);
        const int r = std::max(1, int(float(trunkR) * (1.0f - 0.55f * t) + 0.5f));
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx)
                if (dx * dx + dz * dz <= r * r) w.set(cur, ci + dx, baseY + y, cj + dz, bark);
    }

    const int y0 = int(float(height) * 0.28f);
    for (int y = y0; y < height + 6; ++y) {
        const float frac = float(y - y0) / float(height + 6 - y0);
        float r = crownR * std::pow(std::max(0.0f, 1.0f - frac), 0.75f);
        const int intoWhorl = ((height + 6 - y) % whorl);
        r *= 0.62f + 0.38f * (1.0f - float(intoWhorl) / float(whorl));
        if (r < 1.0f) continue;

        const int ri = int(r) + 1;
        for (int dz = -ri; dz <= ri; ++dz) {
            for (int dx = -ri; dx <= ri; ++dx) {
                const float d = std::sqrt(float(dx * dx + dz * dz));
                if (d > r) continue;
                const float ang = std::atan2(float(dz), float(dx));
                const float spoke =
                    0.5f + 0.5f * std::cos(ang * float(spokes) + phase + float(y) * 0.11f);
                if (d > r * (0.30f + 0.70f * spoke)) continue;
                const float k = ihash3(ci + dx, baseY + y, cj + dz);
                if (k < 0.34f + 0.34f * (d / std::max(1.0f, r))) continue;
                w.set(cur, ci + dx, baseY + y, cj + dz, (k > 0.67f) ? nA : nB);
            }
        }
    }
}

// A procedural boulder: an ellipsoid of stone, dented by 3D noise so it is not
// an egg. Only used when rock.vox did not load.
template <class World>
inline void stampRock(World &w, BrickWorld::Cursor &cur, int ci, int cj, int baseY,
                      uint32_t h) {
    const float rx = 6.0f + 16.0f * hashUnit(h, 5u);
    const float ry = 5.0f + 12.0f * hashUnit(h, 15u);
    const float rz = 6.0f + 16.0f * hashUnit(h, 25u);
    const int ir = int(std::max(rx, std::max(ry, rz))) + 2;
    for (int dy = -ir; dy <= ir; ++dy)
        for (int dz = -ir; dz <= ir; ++dz)
            for (int dx = -ir; dx <= ir; ++dx) {
                const float q = float(dx * dx) / (rx * rx) + float(dy * dy) / (ry * ry) +
                                float(dz * dz) / (rz * rz);
                const float dent = fbm3(float(ci + dx) * 0.09f, float(baseY + dy) * 0.09f,
                                        float(cj + dz) * 0.09f, 3);
                if (q > 0.7f + 0.5f * dent) continue;
                w.set(cur, ci + dx, baseY + dy, cj + dz, mat::ROCK);
            }
}

// ---------------------------------------------------------------------------
// THE WATERLINE, AND IT IS A PROPERTY OF THE FIELD RATHER THAN OF A REGION.
//
// THIS IS WHAT MAKES AN ENDLESS WORLD POSSIBLE AT ALL, and it was the only
// genuinely global thing left in the generator. The line used to be a
// percentile of the heights THIS BUILD had just computed -- an nth_element over
// every column in the extent -- which is a fine answer for a world built once
// in one piece and no answer at all for one built a chunk at a time: the
// twelfth percentile of a hilltop chunk is above the twelfth percentile of a
// valley chunk, so two neighbouring chunks would pour their lakes at different
// heights and the seam between them would be a wall of water.
//
// The height field is STATIONARY -- warped fbm over an infinite lattice, with
// the same distribution everywhere -- so the percentile can be taken from the
// FIELD instead, once, over a fixed window that has nothing to do with which
// region is being built. Every chunk then agrees about the waterline without
// consulting any other chunk, which is the property the whole streaming design
// rests on.
//
// THE WINDOW IS 4 km SQUARE AT 8 m SPACING, and both numbers are deliberate.
// The lowest octave of the landform is `hillM` long -- 70 m by default -- so 8 m
// steps sample within a hill rather than tracing one, and 4 km spans it about
// sixty times on each axis. A window of the size the world used to be would
// inherit exactly the sampling error this is removing: 192 m is under three
// wavelengths, so the "percentile" was really the height of whichever hill
// happened to sit at the origin.
//
// 262,144 evaluations, about seventy milliseconds, ONCE for a world -- see
// WorldConstants, which is what stops every chunk paying it.
// ---------------------------------------------------------------------------
inline int waterLineVox(const GenOptions &o) {
    if (!o.water) return kNoWaterLine;
    // An absolute line, when one is asked for, still wins and costs nothing.
    if (o.waterM > 0.0f) return int(o.waterM / VOXEL_M);

    static const int kSide = 512;
    static const float kStep = 8.0f;
    std::vector<int16_t> sample;
    sample.reserve(size_t(kSide) * size_t(kSide));
    for (int j = 0; j < kSide; ++j) {
        // One TerrainNoise per ROW, exactly as the fill does -- it memoises the
        // lattice cell of the last sample, and a row is what makes that pay.
        TerrainNoise n;
        for (int i = 0; i < kSide; ++i) {
            const float x = (float(i) - float(kSide) * 0.5f) * kStep;
            const float z = (float(j) - float(kSide) * 0.5f) * kStep;
            // int() of the metres, so this is the same quantity the fill puts
            // in height[] and not a float the comparison would round elsewhere.
            sample.push_back(int16_t(int(terrainH(n, x, z, o) / VOXEL_M)));
        }
    }
    const size_t q = size_t(clampf(o.waterPct, 0.0f, 1.0f) * float(sample.size() - 1));
    std::nth_element(sample.begin(), sample.begin() + long(q), sample.end());
    return int(sample[q]);
}

// ---------------------------------------------------------------------------
// EVERYTHING A CHUNK NEEDS TO KNOW THAT IT CANNOT WORK OUT FOR ITSELF.
//
// Which is, so far, one integer -- and that is the point of the type rather
// than an argument against it. A chunk generator that takes GenOptions and a
// rectangle is only correct if NOTHING else global is reachable from inside it,
// and a named place for the exceptions is what makes a new one obvious instead
// of letting it be smuggled in as another static. Computed once per world and
// passed down.
// ---------------------------------------------------------------------------
struct WorldConstants {
    int waterY = kNoWaterLine;

    static WorldConstants of(const GenOptions &o) {
        WorldConstants k;
        k.waterY = waterLineVox(o);
        return k;
    }
};

// ---------------------------------------------------------------------------
// A WRITER THAT OWNS A RECTANGLE, AND WHY EVERY VOXEL MUST HAVE ONE OWNER.
//
// A big boulder reaches 12.7 m from the column it is anchored in -- measured,
// not guessed -- so a model planted in one chunk writes voxels into its
// neighbours. Two things follow, and they pull in opposite directions:
//
//   A CHUNK MUST DISCOVER MODELS IT DOES NOT CONTAIN, or the half of a boulder
//   that hangs over its edge would be missing whenever the chunk holding the
//   anchor was not resident. The scatter grid makes this free -- a cell's
//   contents are a hash of its coordinate, so any chunk can work out what its
//   neighbours grew without asking them.
//
//   ...AND IT MUST STILL WRITE ONLY ITS OWN VOXELS, or the same boulder is
//   stamped once by every chunk it touches, and unloading any one of them tears
//   a hole in the others.
//
// So discovery is wide and WRITING IS CLIPPED: every voxel belongs to exactly
// the chunk whose rectangle contains its column, whoever grew it. That makes a
// chunk's contents a function of the chunk alone, which is what lets one be
// dropped and rebuilt without consulting anything else.
//
// CLIPPING AT THE WRITE, NOT AT EACH CALLER. There are thirteen places in this
// file that put a voxel down -- the skin, the water pour, four model stamps,
// the procedural pine, the grass strands, the ores -- and a rule enforced at
// one of fourteen is a rule that will be broken by the fifteenth. Models::stamp
// is already templated on the world, so this substitutes for one.
//
// UNBOUNDED IS THE ONE-SHOT BUILD, and it is not the same picture: a world
// built in one piece lets a boulder at the extent's edge hang over it, because
// there is nothing out there for it to belong to. An endless world has no edge
// and never takes this path.
// ---------------------------------------------------------------------------
struct ClipWriter {
    BrickWorld *w = nullptr;
    ColGrid rect;
    bool clip = false;

    static ClipWriter unbounded(BrickWorld &world) {
        ClipWriter c;
        c.w = &world;
        return c;
    }
    static ClipWriter owning(BrickWorld &world, const ColGrid &r) {
        ClipWriter c;
        c.w = &world;
        c.rect = r;
        c.clip = true;
        return c;
    }

    void set(BrickWorld::Cursor &c, int i, int y, int j, uint8_t m) {
        if (clip && !rect.inside(i, j)) return;
        w->set(c, i, y, j, m);
    }
};

// ---------------------------------------------------------------------------
// WHAT A RECTANGLE OF GROUND IS, BEFORE ANY OF IT IS WRITTEN AS VOXELS.
//
// Every later pass -- the skin, the trees, the boulders, the grass -- reads the
// ground rather than the noise, so that they all land on the SAME surface the
// fill laid. That made these four arrays the real interface between the halves
// of the generator, and naming them is what lets the halves run at different
// times over different rectangles: a resident window computes its columns once,
// and each chunk of bricks inside it is written from them without a single
// noise evaluation of its own.
//
// THE HEIGHTS ARE NOT THE NOISE. The bank pass MOVES them -- a beach is
// flattened toward the waterline -- and the skin, the formula underneath and
// everything seated on the ground all have to see the moved height or they
// describe different worlds. That is why this is a value that gets built, and
// not a function anyone may call.
// ---------------------------------------------------------------------------
struct Columns {
    ColGrid g;
    std::vector<int16_t> height;  // AFTER the bank flatten
    std::vector<uint8_t> bare;    // too steep to hold soil
    std::vector<uint8_t> wet;     // water stands over this column
    std::vector<uint8_t> sand;    // lake bed or beach
    int waterY = kNoWaterLine;

    // The layer stack, carried here because the skin writer and
    // ImplicitColumns must agree about it exactly -- see the byte-equality
    // test in tests/brick_test.cpp.
    int soilVox = 0, stoneVox = 0, bedrockVox = 0, column = 0;

    bool has(int i, int j) const { return g.inside(i, j); }
    int heightAt(int i, int j) const { return int(height[g.idx(i, j)]); }
};

// The formula that answers for everything sealed under the skin. It is a view
// of the columns and carries no information they do not -- which is what makes
// the byte-equality test in tests/brick_test.cpp meaningful: build a world both
// ways and the device gets the same bytes.
inline ImplicitColumns implicitFrom(const Columns &col) {
    ImplicitColumns im;
    im.height = col.height;
    im.g = col.g;
    im.column = col.column;
    im.soilVox = col.soilVox;
    im.bedrockVox = col.bedrockVox;
    im.soil = mat::SOIL_0;
    im.stone = mat::ROCK;
    im.bedrock = mat::BEDROCK;
    im.sand = mat::SAND;
    im.rock = mat::ROCK;
    // 0 soil, 1 sand, 2 bare rock -- the same three the skin writer uses.
    im.surf.assign(col.g.count(), 0u);
    for (size_t k = 0; k < im.surf.size(); ++k)
        im.surf[k] = col.sand[k] ? 1u : (col.bare[k] ? 2u : 0u);
    return im;
}

// ---------------------------------------------------------------------------
// PASS ONE, OVER A RECTANGLE: the height field, what the surface is made of,
// where the water stands and where the sand is.
//
// Pure in the rectangle: every column's answer depends on the noise at that
// column, on the world's constants, and -- for the one neighbour rule in the
// lake test -- on the four columns around it. So the same rectangle always
// produces the same columns, and a bigger rectangle agrees with a smaller one
// everywhere except within one column of the smaller one's edge. That single
// column is the reason a region is built with a margin.
// ---------------------------------------------------------------------------
// -- FILL PART OF A SET OF COLUMNS, OR ALL OF IT ---------------------------
//
// `rect` is what gets recomputed; `col.g` is the array it is written into, and
// may be much larger and may WRAP. That is the whole of what a rolling window
// needs: when it slides, only the strip that entered is filled, into the slots
// the strip that left has just vacated.
//
// IT IS IDEMPOTENT, AND THAT IS NOT AN ACCIDENT. Filling a rectangle twice must
// give the same answer, because a strip is filled one column wider than it
// strictly needs to be -- the lake rule reads four neighbours, so a column just
// outside the strip can change when the strip lands beside it. The bank pass is
// the reason this needs saying: it MOVES the height toward the waterline, so
// running it twice on an already-flattened column would flatten it again. It
// cannot happen here because pass one rewrites the height from the noise first,
// so every fill starts from raw ground.
inline void fillColumns(Columns &col, const GenOptions &o, const WorldConstants &wc,
                        const ColGrid &rect) {
    const ColGrid &g = col.g;
    if (g.w <= 0 || g.h <= 0 || rect.w <= 0 || rect.h <= 0) return;
    std::vector<int16_t> &height = col.height;
    std::vector<uint8_t> &bareCol = col.bare;
    std::vector<uint8_t> &wetCol = col.wet;
    std::vector<uint8_t> &sandCol = col.sand;
    int waterY = kNoWaterLine;

    // ROW BY ROW, x INNERMOST. NoiseCell memoises the lattice cell the last
    // sample fell in, and consecutive columns are 10 cm apart against a lowest
    // octave seventy metres long -- so along a row the memo hits hundreds of
    // times in a row and misses once. Walking columns instead would miss every
    // time and the generator would be an order of magnitude slower.
    // -- ROW BY ROW, AND EVERY ROW IS ITS OWN --------------------------------
    //
    // Nothing here reads another column: the height is the noise at this point,
    // and the slope is three more samples of it. So the rows go out to every
    // core, which is most of what a streamed window pays when it slides --
    // 199 ms of a 235 ms crossing was this loop evaluating the noise for the
    // strip of ground that had just come into view.
    //
    // THE MEMO IS PER ROW ALREADY. TerrainNoise caches the lattice cell the
    // last sample fell in and consecutive columns are 10 cm apart, so a row is
    // exactly the unit that memo pays off over -- which is why it was declared
    // inside the j loop and why splitting on j costs nothing.
    auto fillRow = [&](int j) {
        TerrainNoise n;
        for (int i = rect.i0; i < rect.i1(); ++i) {
            const float x = float(i) * VOXEL_M, z = float(j) * VOXEL_M;
            const int hv = int(terrainH(n, x, z, o) / VOXEL_M);
            const size_t idx = g.idx(i, j);
            height[idx] = int16_t(hv);

            // -- the slope, which decides what the surface is made of --------
            //
            // Sampled from the noise rather than from the neighbouring columns,
            // because the neighbour to the east has not been written yet on the
            // first row and reading it would make the first row of every run
            // different from the rest.
            const float hE = terrainH(n, x + 0.4f, z, o);
            const float hN = terrainH(n, x, z + 0.4f, o);
            const float hC = float(hv) * VOXEL_M;
            const float slope = (std::fabs(hE - hC) + std::fabs(hN - hC)) / 0.4f;
            const bool bare = slope > 0.85f;
            bareCol[idx] = bare ? 1u : 0u;

            // -- AND CLEAR WHAT THE WATER PASSES ONLY EVER SET --------------
            //
            // wetCol and sandCol are written by rules that say "this column is
            // wet" and never "this column is not", because in a world built
            // once the arrays start at zero and nothing ever needs unsaying.
            //
            // A ROLLING WINDOW REUSES SLOTS. Its column array wraps, so the
            // ground entering on one side lands in the slots the ground leaving
            // the other side has just vacated -- carrying that ground's flags
            // with it. A dry hilltop inherited a lake from 230 m away and
            // poured water over itself. It survived eight window steps and
            // appeared on the ninth, because until then the stale slots were
            // out in the margin where nothing reads them.
            //
            // Clearing here rather than in the water pass is deliberate: this
            // loop is the one place guaranteed to visit every column of the
            // rectangle being filled, which is what makes fillColumns
            // idempotent and therefore safe to overlap.
            wetCol[idx] = 0u;
            sandCol[idx] = 0u;

        }
    };

    {
        unsigned nt = std::thread::hardware_concurrency();
        if (nt == 0) nt = 1;
        if (nt > 16u) nt = 16u;
        if (rect.h < 16) nt = 1;
        if (nt <= 1) {
            for (int j = rect.j0; j < rect.j1(); ++j) fillRow(j);
        } else {
            std::vector<std::thread> th;
            th.reserve(nt);
            std::atomic<int> next{rect.j0};
            const int end = rect.j1();
            for (unsigned t = 0; t < nt; ++t)
                th.emplace_back([&fillRow, &next, end]() {
                    for (;;) {
                        const int j = next.fetch_add(1);
                        if (j >= end) break;
                        fillRow(j);
                    }
                });
            for (std::thread &t : th) t.join();
        }
    }

    // -----------------------------------------------------------------------
    // WHERE THE WATER IS, AND WHERE THE SAND IS. Between the two passes,
    // because it MOVES THE GROUND: a bank is flattened, and the skin pass and
    // the formula under it both have to see the flattened height or they
    // describe two different worlds.
    // -----------------------------------------------------------------------
    if (o.water) {
        // -- the line, and it does NOT come from this region ----------------
        //
        // See waterLineVox. Taking it from the heights just computed made the
        // waterline a property of whatever rectangle was being built, which is
        // the one thing a chunk may not depend on.
        waterY = wc.waterY;

        if (waterY > kNoWaterLine) {
            // -- v1's LAKE RULE, and every bound shifts by one -------------
            //
            // v1's height is one PAST the top solid voxel and this one IS it,
            // so v1's `h <= WL-1` is `H <= waterY-2`. A column exactly one
            // voxel under the line is wet only if a NEIGHBOUR is properly wet
            // -- and that second clause is the whole thing: without it every
            // shallow dip in the terrain becomes a one-voxel puddle and the
            // world is covered in wet patches nobody would call a lake.
            for (int j = rect.j0; j < rect.j1(); ++j)
                for (int i = rect.i0; i < rect.i1(); ++i) {
                    const size_t idx = g.idx(i, j);
                    if (int(height[idx]) <= waterY - 2) wetCol[idx] = 1u;
                }
            for (int j = rect.j0; j < rect.j1(); ++j)
                for (int i = rect.i0; i < rect.i1(); ++i) {
                    const size_t idx = g.idx(i, j);
                    if (wetCol[idx] || int(height[idx]) != waterY - 1) continue;
                    // NOT `near`. windef.h defines that as a macro -- it is
                    // empty on any modern target, so the line becomes
                    // `bool = false;` and MSVC reports "no variable declared
                    // before '='" while g++, which never sees windows.h,
                    // compiles the file perfectly.
                    //
                    // THIS IS WHY A REGION IS BUILT WITH A MARGIN. It reads the
                    // four neighbours, so a column on the rectangle's own edge
                    // would be judged against columns that are not there -- and
                    // the answer decides whether water is poured. Built to the
                    // edge, a chunk's rim would disagree with the same ground
                    // built as part of a larger rectangle. See ColGrid.
                    bool touching = false;
                    if (g.inside(i - 1, j)) touching |= wetCol[g.idx(i - 1, j)] == 1u;
                    if (g.inside(i + 1, j)) touching |= wetCol[g.idx(i + 1, j)] == 1u;
                    if (g.inside(i, j - 1)) touching |= wetCol[g.idx(i, j - 1)] == 1u;
                    if (g.inside(i, j + 1)) touching |= wetCol[g.idx(i, j + 1)] == 1u;
                    if (touching) wetCol[idx] = 2u;  // after the sweep, so it cannot cascade
                }

            // -- FLAT SANDY BANKS -----------------------------------------
            //
            // Sand is laid on everything the water touches and on the rise just
            // above it, and that rise is FLATTENED toward the line. A bank that
            // keeps the hillside's slope is a hillside that happens to be
            // yellow; what the eye reads as a shore is the break in slope, so
            // the break is what gets built.
            const int rise = std::max(1, int(o.bankRiseM / VOXEL_M));
            for (int j = rect.j0; j < rect.j1(); ++j)
                for (int i = rect.i0; i < rect.i1(); ++i) {
                    const size_t idx = g.idx(i, j);
                    const int h = int(height[idx]);
                    if (wetCol[idx]) {
                        sandCol[idx] = 1u;  // the bed of a lake is sand too
                        continue;
                    }
                    // ANY GROUND WITHIN bankRise OF THE LINE IS A BANK, and
                    // there is deliberately no "is a lake nearby" test.
                    //
                    // There was one, searching two voxels out, and it produced
                    // no banks at all: the waterline is a global HEIGHT, the
                    // slopes here are gentle, and a column sitting 0.9 m above
                    // the line is metres away in PLAN from one sitting 0.2 m
                    // below it. Twenty centimetres of search found nothing and
                    // every lake came out as grass meeting water with no shore
                    // between them.
                    //
                    // The height band is the right test on its own: with one
                    // waterline for the world, ground this close to it IS the
                    // shore, and a wider search would only be a slower way of
                    // agreeing.
                    if (h > waterY + rise) continue;
                    sandCol[idx] = 1u;

                    // -- AND THE TWO VOXELS THE BAND USED TO FALL THROUGH ---
                    //
                    // `h <= waterY` used to `continue` here, which left a strip
                    // of ground that was neither wet nor bank: the wet sweep
                    // claims everything at or below waterY-2, the band began at
                    // waterY+1, and the columns at waterY-1 and waterY belonged
                    // to neither. Twenty centimetres of shore, wearing soil and
                    // GROWING GRASS -- because sandAt() is what keeps the
                    // scatterers off a beach, and it was false there. Every
                    // lake had a green fringe between its sand and its water.
                    //
                    // They are shore, so they are sand. They are already at or
                    // below the line, so there is nothing to ease -- the ease
                    // is for ground standing ABOVE the water.
                    if (h <= waterY) continue;

                    // -- EASED, NOT FLATTENED, OR THE BEACH ENDS IN A CLIFF --
                    //
                    // A constant multiplier lowers the whole band by a constant
                    // FRACTION, so the top of the band is lowered most and the
                    // ground just outside it is not lowered at all. At the
                    // defaults -- a nine-voxel rise flattened to a quarter --
                    // the last bank column lands at waterY+3 against untouched
                    // terrain at waterY+10: a seven-voxel step running right
                    // around every lake, which is the one thing a beach must
                    // not have.
                    //
                    // So the multiplier is bankFlat at the water and 1.0 -- the
                    // ground's own slope -- at the top of the band, eased
                    // between them with smoothstep. Smoothstep and not a linear
                    // ramp because its derivative is zero at BOTH ends: the
                    // beach leaves the water flat and joins the hillside at the
                    // hillside's own gradient, so neither seam is a crease. A
                    // linear ramp reaches 1.75x the natural slope at the top
                    // and bulges outward just before it meets the hill.
                    //
                    // Monotonic by construction: d * (bf + (1-bf)*S(d/D)) has
                    // derivative bf + (1-bf)(9u^2 - 8u^3), which is at least bf
                    // for u in [0,1]. The bank can never fold back on itself.
                    const int d = h - waterY - 1;
                    const int D = rise - 1;
                    const float u = D > 0 ? float(d) / float(D) : 0.0f;
                    const float ease = u * u * (3.0f - 2.0f * u);
                    const float k = o.bankFlat + (1.0f - o.bankFlat) * ease;
                    height[idx] = int16_t(waterY + 1 + int(float(d) * k + 0.5f));
                }
        }
    }

    col.waterY = waterY;
}

// The whole rectangle at once, which is what a world built in one piece is.
inline Columns buildColumns(const GenOptions &o, const WorldConstants &wc, const ColGrid &g) {
    Columns col;
    col.g = g;
    // Surface + soil + stone + bedrock, capped by --skin if it was given.
    col.soilVox = std::max(0, o.soilVox);
    col.stoneVox = std::max(0, o.stoneVox);
    col.bedrockVox = std::max(0, o.bedrockVox);
    col.column = 1 + col.soilVox + col.stoneVox + col.bedrockVox;
    if (o.skinM > 0.0f)
        col.column = std::min(col.column, std::max(1, int(o.skinM / VOXEL_M)));
    if (g.w <= 0 || g.h <= 0) return col;
    col.height.assign(g.count(), 0);
    col.bare.assign(g.count(), 0);
    col.wet.assign(g.count(), 0);
    col.sand.assign(g.count(), 0);
    fillColumns(col, o, wc, g);
    return col;
}

// ---------------------------------------------------------------------------
// PASS TWO AND EVERYTHING AFTER IT, OVER A RECTANGLE IT OWNS.
//
// `col` is the ground -- already built, and covering MORE than this rectangle,
// because the skin reads its neighbours and a boulder is seated on the lowest
// ground under its whole footprint. `own` is what this call writes. `disc` is
// how far out it looks for models that reach in; see ClipWriter for why those
// are two different rectangles.
//
// For a world built in one piece all three are the same and nothing is clipped,
// which is how the packed buffers stayed byte-identical through the extraction.
// ---------------------------------------------------------------------------
inline void buildBody(BrickWorld &w, const Content &c, const GenOptions &o,
                      const Columns &col, const ColGrid &own, const ColGrid &disc,
                      bool clipped) {
    const ColGrid &g = col.g;
    const std::vector<int16_t> &height = col.height;
    const std::vector<uint8_t> &bareCol = col.bare;
    const std::vector<uint8_t> &wetCol = col.wet;
    const std::vector<uint8_t> &sandCol = col.sand;
    const int waterY = col.waterY;
    const int soilVox = col.soilVox;
    const int bedrockVox = col.bedrockVox;
    const int column = col.column;
    (void)wetCol;
    (void)soilVox;

    BrickWorld::Cursor cur;
    ClipWriter out = clipped ? ClipWriter::owning(w, own) : ClipWriter::unbounded(w);

    auto heightOf = [&](int i, int j, bool *inside) -> int {
        *inside = g.inside(i, j);
        return *inside ? int(height[g.idx(i, j)]) : 0;
    };

    for (int j = own.j0; j < own.j1(); ++j) {
        for (int i = own.i0; i < own.i1(); ++i) {
            const size_t idx = g.idx(i, j);
            const int hv = int(height[idx]);
            const bool bare = bareCol[idx] != 0u;
            const bool sand = sandCol[idx] != 0u;
            const int bottom = hv - column + 1;

            int mn = hv, mx = hv;
            bool edge = false;
            static const int kdx[4] = {-1, 1, 0, 0}, kdz[4] = {0, 0, -1, 1};
            for (int n = 0; n < 4; ++n) {
                bool in = false;
                const int hn = heightOf(i + kdx[n], j + kdz[n], &in);
                if (!in) { edge = true; break; }
                mn = std::min(mn, hn);
                mx = std::max(mx, hn);
            }

            int topLo, botHi;
            if (edge || o.explicitFill) {
                topLo = bottom;
                botHi = hv;
            } else {
                topLo = std::max(bottom, std::min(hv, mn + 1));
                botHi = std::max(bottom, std::min(hv, mx - column));
            }

            auto put = [&](int y) {
                const int k = hv - y;
                uint8_t m;
                // THE BOTTOM TWO LAYERS DO NOT CARE WHETHER THE SURFACE IS
                // BARE. A cliff has soil nowhere and stone everywhere, but the
                // stone still ends and the bedrock still begins at the same
                // depth under it -- otherwise the floor of the world follows
                // the weather.
                if (k >= column - bedrockVox) {
                    m = mat::BEDROCK;
                } else if (k > soilVox) {
                    m = mat::ROCK;
                } else if (sand) {
                    // SAND GOES DOWN AS FAR AS THE SOIL WOULD, not one voxel.
                    // A one-voxel skin of it shows its brown underside the
                    // moment the shore is seen from below the waterline, which
                    // through clear water is most of the time.
                    m = mat::SAND;
                } else if (bare) {
                    m = mat::ROCK;
                } else if (k == 0) {
                    // THE SURFACE IS LITTER, NOT GRASS, AND THAT COST AN
                    // AFTERNOON ONCE. mat::GRASS_0 is the FOLIAGE ramp and
                    // every entry in it carries translucency 0.22, because a
                    // needle is thin enough to be lit from behind. A whole
                    // ground plane in it transmits a fifth of the light
                    // reaching it instead of absorbing it, that light feeds the
                    // bounce that lights it, and the frame blows out to flat
                    // WHITE with a palette that is entirely correct.
                    //
                    // A blade may be translucent. A floor is opaque. So the
                    // floor is the LITTER ramp and the green goes on the
                    // strands standing in it.
                    m = mat::LITTER_0;
                } else {
                    m = mat::SOIL_0;
                }
                out.set(cur, i, y, j, m);
            };

            for (int y = topLo; y <= hv; ++y) put(y);
            for (int y = bottom; y <= botHi; ++y) put(y);

            // -- POUR THE WATER ------------------------------------------
            //
            // Every air voxel from the bed up to the line, so a lake has a
            // floor, a bank and a real DEPTH -- five metres at the deepest
            // here. Not a plane laid at the waterline: a single quad has no
            // volume for light to travel through, and the caustics, the
            // absorption and the shafts all come from the travel.
            //
            // THE WATER IS WRITTEN EXPLICITLY EVEN THOUGH THE BED BELOW IT IS
            // A FORMULA. It has to be -- it is above the terrain surface, which
            // is where ImplicitColumns stops.
            if (wetCol[idx])
                for (int y = hv + 1; y <= waterY; ++y) out.set(cur, i, y, j, mat::WATER);
        }
    }

    auto groundAt = [&](int i, int j) -> int {
        if (!g.inside(i, j)) return BrickWorld::kNoTop;
        return int(height[g.idx(i, j)]);
    };
    // Nothing grows on a beach or in a lake. `gy < waterY` alone is not enough:
    // a bank stands just ABOVE the line and is still sand.
    auto sandAt = [&](int i, int j) -> bool {
        if (!g.inside(i, j)) return false;
        return sandCol[g.idx(i, j)] != 0u;
    };

    auto bareAt = [&](int i, int j) -> bool {
        if (!g.inside(i, j)) return true;
        return bareCol[g.idx(i, j)] != 0u;
    };

    // -----------------------------------------------------------------------
    // SEAT A MODEL ON THE GROUND UNDER ITS WHOLE FOOTPRINT, not on the ground
    // under its anchor.
    //
    // This world has hills; the engine these models came from had a flat floor
    // and could sink everything a fixed five voxels. On a slope that leaves the
    // low side of a 9 m pine standing on tiptoe with daylight under it -- the
    // single most obvious way a stamped model looks wrong, and the thing
    // NOTHING FLOATS exists to prevent.
    //
    // So the base goes at the LOWEST ground in the footprint, and then a few
    // voxels lower still. The radius is capped well below a pine's full reach:
    // a crown 5 m wide is 15 m up in the air and has no business deciding how
    // deep the trunk is buried.
    // -----------------------------------------------------------------------
    auto seat = [&](int i, int j, int radius, int sink) -> int {
        int lo = groundAt(i, j);
        if (lo == BrickWorld::kNoTop) return BrickWorld::kNoTop;
        const int step = std::max(1, radius / 4);
        for (int dz = -radius; dz <= radius; dz += step)
            for (int dx = -radius; dx <= radius; dx += step) {
                if (dx * dx + dz * dz > radius * radius) continue;
                const int g = groundAt(i + dx, j + dz);
                if (g != BrickWorld::kNoTop) lo = std::min(lo, g);
            }
        return lo - sink;
    };

    // -----------------------------------------------------------------------
    // A SCATTER GRID. A cell either holds one of the thing or it does not,
    // decided by hashing its coordinate; the jitter, the model and the rotation
    // come out of the same hash. Nothing is stored and nothing is planted in
    // advance, so the same seed always grows the same wood.
    // -----------------------------------------------------------------------
    auto scatter = [&](float cellM, uint32_t gate, uint32_t salt, int reach, auto &&place) {
        // The rectangle this pass has to LOOK at: what it owns, grown by how
        // far this kind of thing reaches in from outside it.
        ColGrid disc = own;
        disc.i0 -= reach;
        disc.j0 -= reach;
        disc.w += 2 * reach;
        disc.h += 2 * reach;
        const int cell = std::max(1, int(cellM / VOXEL_M));
        // THE CELLS THAT COULD REACH THIS RECTANGLE, WHICH IS NOT THE SAME AS
        // THE CELLS INSIDE IT. `disc` is the owned rectangle grown by the
        // furthest a model reaches from its anchor -- see ClipWriter. A chunk
        // therefore plants its neighbours' boulders and keeps only the half
        // that lands on its own ground.
        const int ci0 = floorDiv(disc.i0, cell) - 1, ci1 = floorDiv(disc.i1(), cell) + 1;
        const int cj0 = floorDiv(disc.j0, cell) - 1, cj1 = floorDiv(disc.j1(), cell) + 1;
        for (int cz = cj0; cz <= cj1; ++cz)
            for (int cx = ci0; cx <= ci1; ++cx) {
                const uint32_t h = Models::hash2(cx, cz, o.seed ^ salt);
                if ((h & 0xffu) > gate) continue;
                // Jitter by a sixth of a cell, so the wood is not a lattice but
                // two trunks still cannot meet.
                const int jit = cell / 6;
                const int px = cx * cell + cell / 2 + int((h >> 8) & 0x3fu) * jit / 32 - jit;
                const int pz = cz * cell + cell / 2 + int((h >> 14) & 0x3fu) * jit / 32 - jit;
                if (!disc.inside(px, pz)) continue;
                place(px, pz, h);
            }
    };

    const Models &M = c.models;

    // ---- the pines --------------------------------------------------------
    if (o.trees) {
        scatter(o.treeCellM, 168u, 0x9E3779B9u, c.reachPine,
                [&](int px, int pz, uint32_t h) {
            // NOT ON A CLIFF. A bare column is one the soil pass already judged
            // too steep to hold anything; a 9 m crown rooted in it hangs off
            // the face, and no amount of sinking fixes that.
            if (bareAt(px, pz) || sandAt(px, pz)) return;
            const int gy = seat(px, pz, 25, 5);
            if (gy == BrickWorld::kNoTop || gy < waterY) return;
            const uint32_t h2 = Models::hash2(int(h), 0x27D4EB2F, o.seed);
            if (M.loaded())
                Models::stamp(out, cur, M.pines[h2 % M.pines.size()], px, gy, pz,
                              int((h2 >> 11) & 3u));
            else
                stampPine(out, cur, c, px, pz, gy, h2);
        });
    }

    // ---- boulders, in their three sizes -----------------------------------
    //
    // SEATED ON THE LOWEST GROUND UNDER THEM, with a footprint radius that
    // scales with the set. A 20 m boulder set down on the height under its
    // anchor alone stands on one corner over a slope -- the same failure a tree
    // has, and far more obvious on something that wide.
    auto placeRocks = [&](const std::vector<VoxPiece> &set, float cellM, uint32_t gate,
                          uint32_t salt, int radius, int sink, int reach) {
        if (set.empty()) return;
        scatter(cellM, gate, salt, reach, [&](int px, int pz, uint32_t h) {
            const int gy = seat(px, pz, radius, sink);
            if (gy == BrickWorld::kNoTop || gy < waterY) return;
            Models::stamp(out, cur, set[(h >> 20) % set.size()], px, gy, pz, int((h >> 18) & 3u));
        });
    };

    // THE GATES ARE HALVED FROM WHAT THEY WERE -- 110/130/150 became 54/64/75.
    //
    // A gate keeps a cell when the low byte of its hash is <= the gate, so the
    // fraction kept is (gate + 1) / 256 and halving the rocks means halving
    // that: 111/256 -> 55/256, 131/256 -> 65/256, 151/256 -> 76/256. It is the
    // GATE and not the cell size that moves, deliberately. Widening the grids
    // instead would thin the boulders by pushing them apart, which spaces them
    // evenly; dropping cells leaves the survivors where they were, so the wood
    // keeps clusters and bare stretches instead of turning into a lattice at
    // lower density.
    //
    // The three sets still cannot share a grid -- see the note on placeRocks
    // and on Models::rocksBig.
    if (o.rocks) {
        if (M.loaded() && M.rockCount() > 0) {
            placeRocks(M.rocksBig, o.rockBigCellM, 54u, 0xB16B16B1u, 90, 14, c.reachBig);
            placeRocks(M.rocksMid, o.rockMidCellM, 64u, 0x31D31D31u, 36, 6, c.reachMid);
            placeRocks(M.rocksSmall, o.rockCellM, 75u, 0xA5A5A5A5u, 8, 2, c.reachSmall);
        } else {
            scatter(o.rockCellM, 75u, 0xA5A5A5A5u, 26, [&](int px, int pz, uint32_t h) {
                const int gy = seat(px, pz, 6, 2);
                if (gy == BrickWorld::kNoTop || gy < waterY) return;
                stampRock(out, cur, px, pz, gy - 1, h);
            });
        }
    }

    if (M.loaded() && !M.mushrooms.empty())
        scatter(o.mushroomCellM, 70u, 0x5EED5EEDu, c.reachMush,
                [&](int px, int pz, uint32_t h) {
            if (bareAt(px, pz)) return;
            const int gy = seat(px, pz, 8, 1);
            if (gy == BrickWorld::kNoTop || gy < waterY) return;
            Models::stamp(out, cur, M.mushrooms[(h >> 20) % M.mushrooms.size()], px, gy, pz,
                          int((h >> 18) & 3u));
        });

    if (M.loaded() && !M.flowers.empty())
        scatter(o.flowerCellM, 60u, 0xF10EF10Eu, c.reachFlower,
                [&](int px, int pz, uint32_t h) {
            if (bareAt(px, pz)) return;
            const int gy = groundAt(px, pz);
            if (gy == BrickWorld::kNoTop || gy < waterY) return;
            Models::stamp(out, cur, M.flowers[(h >> 20) % M.flowers.size()], px, gy, pz,
                          int((h >> 18) & 3u));
        });

    // ---- grass strands ----------------------------------------------------
    //
    // GRASS_0, AND HERE IT IS RIGHT. This is the foliage ramp the floor above
    // must not use: six greens read off the pines' own needles, each carrying
    // translucency 0.22. On a plane that makes the ground glow; on a blade two
    // to five voxels tall it is the entire reason a strand backlit by a low sun
    // reads as grass rather than as a black stick.
    //
    // -----------------------------------------------------------------------
    // THE DENSITY IS A FIELD, NOT A NUMBER, AND THAT IS THE WHOLE CHANGE HERE.
    //
    // This used to be one flat gate -- hash the column, keep it if the low byte
    // came under 151, which is 59 % of every column that is not too steep. A
    // constant probability over the whole world gives grass with no structure
    // in it: statistically identical everywhere, so there is no glade to walk
    // into and no thicket to come out of, and the floor it stands in is never
    // visible because there is never a gap wide enough to see it through.
    //
    // So the gate is now modulated by a low-frequency field. `patch` is three
    // octaves at grassPatchM, sharpened by sstep so it commits to one side or
    // the other instead of hovering in the middle; where it is high the meadow
    // is as full as the old flat gate ever was, and where it is low there are a
    // few blades on open LITTER floor. The mean over the field is
    // grassDensity -- that is what the 0.12/1.76 pair is for, since 0.12 + 1.76
    // * 0.5 is exactly 1 and the sharpened field averages a half.
    //
    // THE FLOOR IS UNTOUCHED BY ANY OF THIS. Thinning the strands reveals more
    // of the LITTER ramp laid in buildTerrain above; it does not change what
    // that ramp is. The dirt was always under the grass -- there is simply more
    // of it to see now.
    //
    // A ROW AT A TIME, WITH A MEMO, for the same reason the terrain fill is:
    // consecutive columns are 10 cm apart against a field tens of metres long,
    // so the noise lattice cell hits hundreds of times in a row. Evaluating it
    // per column without one would cost more than the strands do.
    //
    // The cheap rejections come FIRST -- a bare column and a column with no
    // ground never reach the noise at all.
    // -----------------------------------------------------------------------
    // ---- ORE SEAMS IN THE STONE -------------------------------------------
    //
    // THE BANDS ARE MEASURED FROM THE SURFACE, NOT FROM SEA LEVEL, so a seam
    // follows the hillside above it rather than cutting across it. The stone
    // runs from just under the soil down to the bedrock, and it is divided in
    // three:
    //
    //     upper third   coal, iron        the ores you find near the top
    //     middle third  emerald, ruby
    //     bottom third  gold, diamond     and diamond deepest of all
    //
    // ALMOST ALL OF THIS IS INVISIBLE AND THAT IS CORRECT. A seam sealed inside
    // rock has six opaque neighbours, so the packer drops every voxel of it and
    // it costs the device nothing. What survives is the seams that happen to
    // meet open air -- a cliff face, the wall of a basin, the rim of the world
    // -- which is exactly where an ore deposit should be visible from outside.
    // The rest is there for whatever digs.
    if (o.minerals && M.oreCount() > 0) {
        struct Band {
            float lo, hi;  // fraction of the way down the stone
            Ore kinds[2];
            int nKinds;
        };
        const Band bands[3] = {
            {0.00f, 0.33f, {Ore::Coal, Ore::Iron}, 2},
            {0.33f, 0.66f, {Ore::Emerald, Ore::Ruby}, 2},
            // GOLD IS THE ONE THE BRIEF DID NOT NAME. mineral.vox holds six
            // ores and five were listed, so it goes deep with diamond, which is
            // where a player expects to find it.
            {0.66f, 1.00f, {Ore::Gold, Ore::Diamond}, 2},
        };

        const int stoneTop = soilVox + 1;              // depth of the first stone voxel
        const int stoneBot = column - bedrockVox - 1;  // ...and the last
        const int stoneRun = stoneBot - stoneTop;

        if (stoneRun > 8) {
            for (int b = 0; b < 3; ++b) {
                const Band &bd = bands[b];
                const int kLo = stoneTop + int(float(stoneRun) * bd.lo);
                const int kHi = stoneTop + int(float(stoneRun) * bd.hi);
                // A SEAM SCATTERS ITS BLOBS AWAY FROM THE ANCHOR, so its reach
                // is the spread plus the widest ore piece -- not the piece
                // alone. Ores sit inside the stone where nothing can see a
                // seam, but a chunk that lost half a vein would still differ
                // from the world built whole, and the test compares every
                // voxel including the buried ones.
                const int oreReach = c.reachOre + std::max(1, int(o.oreSpreadM / VOXEL_M));
                scatter(o.oreCellM, 110u, 0x05E1u + uint32_t(b) * 0x9E37u, oreReach,
                        [&](int px, int pz, uint32_t h) {
                            const int hv = groundAt(px, pz);
                            if (hv == BrickWorld::kNoTop) return;

                            // One mineral per seam, one depth per seam.
                            const Ore kind = bd.kinds[(h >> 5) % uint32_t(bd.nKinds)];
                            const std::vector<VoxPiece> &set = M.ore[int(kind)];
                            if (set.empty()) return;
                            const int kSeam = kLo + int((h >> 9) % uint32_t(kHi - kLo + 1));

                            const int spread = std::max(1, int(o.oreSpreadM / VOXEL_M));
                            const int n = o.oreMin +
                                          int((h >> 21) % uint32_t(std::max(1, o.oreMax - o.oreMin + 1)));
                            for (int q = 0; q < n; ++q) {
                                const uint32_t hq = Models::hash2(px * 31 + q, pz * 17 + q * 7, h);
                                const int dx = int(hq % uint32_t(2 * spread + 1)) - spread;
                                const int dz = int((hq >> 8) % uint32_t(2 * spread + 1)) - spread;
                                const int dk = int((hq >> 16) % uint32_t(2 * spread + 1)) - spread;
                                const int k = std::min(stoneBot, std::max(stoneTop, kSeam + dk));
                                // The depth is measured from THIS blob's own
                                // column, so a seam under a slope stays the same
                                // distance into the rock all the way along it.
                                const int hb = groundAt(px + dx, pz + dz);
                                if (hb == BrickWorld::kNoTop) continue;
                                Models::stamp(out, cur, set[(hq >> 24) % set.size()], px + dx,
                                              hb - k, pz + dz, int((hq >> 12) & 3u));
                            }
                        });
            }
        }
    }

    if (o.grass) {
        const float ps = 1.0f / std::max(1.0f, o.grassPatchM);
        const float gox = float(o.seed & 0x7FFu) * 0.031f + 11.7f;
        const float goz = float((o.seed >> 11) & 0x7FFu) * 0.027f + 3.9f;
        const float dens = clampf(o.grassDensity, 0.0f, 1.0f);
        // OVER WHAT THIS CALL OWNS, NOT OVER THE COLUMN ARRAY. They are the
        // same rectangle for a world built in one piece, and for a streamed
        // chunk `g` is the whole resident WINDOW -- so this ran the grass test
        // over 7.6 million columns for every one of 81 chunks. The clip kept it
        // correct and it cost twenty seconds a world.
        for (int j = own.j0; j < own.j1(); ++j) {
            FbmMemo gm;
            for (int i = own.i0; i < own.i1(); ++i) {
                if (bareAt(i, j) || sandAt(i, j)) continue;
                const int gy = groundAt(i, j);
                if (gy == BrickWorld::kNoTop || gy < waterY) continue;

                const float patch = fbm(gm, float(i) * VOXEL_M * ps + gox,
                                        float(j) * VOXEL_M * ps + goz, 3);
                // Widen about the mean, then round, exactly as the landform is
                // domed above -- and for the same reason. A raw value fbm lives
                // in the middle of its range, and a sparse patch has to be an
                // actual clearing rather than a slightly thinner meadow.
                const float t = sstep(clampf((patch - 0.5f) * kGrassPatchGain + 0.5f,
                                             0.0f, 1.0f));
                const float p = dens * (kGrassSparse + kGrassFull * t);

                const uint32_t h = Models::hash2(i, j, o.seed ^ 0x6A55u);
                if (float(h & 0xffu) >= p * 256.0f) continue;
                const int n = 2 + int((h >> 9) % 4u);
                for (int k = 1; k <= n; ++k) out.set(cur, i, gy + k, j, mat::GRASS_0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// THE LANDSCAPE.
// ---------------------------------------------------------------------------
inline void buildTerrain(BrickWorld &w, const Content &c, const GenOptions &o,
                         const WorldConstants &wc) {
    const int half = int(o.extentM * 0.5f / VOXEL_M);
    if (half <= 0) return;
    // The whole extent, centred on the origin -- which is what a one-shot build
    // is. See ColGrid.
    ColGrid g;
    g.i0 = -half;
    g.j0 = -half;
    g.w = half * 2;
    g.h = half * 2;

    const Columns col = buildColumns(o, wc, g);

    // NAMED BACK ONTO THE LOCALS THE REST OF THIS FUNCTION ALREADY USES. The
    // passes below were written against these four arrays and read them a
    // hundred times between them; binding references here is what kept the
    // extraction above to a cut rather than a rewrite, and the packed buffers
    // byte-identical across it.
    const std::vector<int16_t> &height = col.height;
    const std::vector<uint8_t> &bareCol = col.bare;
    const std::vector<uint8_t> &wetCol = col.wet;
    const std::vector<uint8_t> &sandCol = col.sand;
    const int waterY = col.waterY;
    const int soilVox = col.soilVox;
    const int bedrockVox = col.bedrockVox;
    const int column = col.column;
    (void)wetCol;

    BrickWorld::Cursor cur;
    // The whole extent is built in one piece, so nothing is clipped -- see
    // ClipWriter. A region build is the other factory.
    ClipWriter out = ClipWriter::unbounded(w);

    // -----------------------------------------------------------------------
    // PASS TWO: WRITE THE SKIN, AND HAND THE REST OVER AS A FORMULA.
    //
    // Five hundred voxels of stone under every column is 1.9 billion calls to
    // set(), 2.2 GB of host memory and thirteen seconds of launch -- to produce
    // device buffers IDENTICAL to the ones you get without them, because the
    // packer drops every voxel with six opaque neighbours anyway.
    //
    // So only the skin is written, and it is two bands:
    //
    //   THE TOP BAND is where this column stands above its LOWEST neighbour.
    //   Everything higher than that neighbour's surface has air beside it. On
    //   flat ground it is one voxel; on a slope it is the drop.
    //
    //   THE BOTTOM BAND is where this column hangs below its HIGHEST
    //   neighbour's floor -- the underside of the world, and the same argument
    //   upside down.
    //
    // Everything between them is sealed inside rock and is answered by
    // ImplicitColumns, which the peel consults wherever a brick says air. See
    // the note on it in bricks.h, including the one thing it costs.
    //
    // A COLUMN AT THE EDGE OF THE WORLD IS WRITTEN WHOLE. Its neighbour is not
    // lower, it is ABSENT -- open air all the way down -- so the entire side of
    // it is a visible face.
    // -----------------------------------------------------------------------
    if (!o.explicitFill) {
        w.setImplicit(implicitFrom(col));
        // ...and the waterline, for the one question a per-voxel lookup cannot
        // answer -- see BrickWorld::waterM.
        w.setWaterM(waterY > kNoWaterLine ? float(waterY) * VOXEL_M + VOXEL_M : -1e9f);
    }
    // One piece: what it owns, what it looks at and what it writes are all the
    // same rectangle, and a boulder at the extent's edge may hang over it.
    buildBody(w, c, o, col, g, g, /*clipped*/ false);
}

// ---------------------------------------------------------------------------
// THE ACCEPTANCE SCENE.
//
// Small, fast and deliberately made of the things a heightfield CANNOT be. The
// arch is the test: nothing that stores one height per column can hold it, so
// if the arch renders with a hole under it the store really is volumetric. The
// stepped wall crosses a chunk boundary on purpose -- a seam there is the
// classic way a per-chunk acceleration structure goes wrong, and it shows up as
// a missing column of voxels exactly 25.6 m from the origin.
// ---------------------------------------------------------------------------
inline void buildDemo(BrickWorld &w, const Content &c, const GenOptions &o) {
    BrickWorld::Cursor cur;
    const int half = int(std::min(o.extentM, 96.0f) * 0.5f / VOXEL_M);

    // A floor, two voxels thick. Litter over stone -- see the long note in
    // buildTerrain about why a ground PLANE is never laid in the grass ramp.
    for (int j = -half; j < half; ++j)
        for (int i = -half; i < half; ++i) {
            w.set(cur, i, 0, j, mat::ROCK);
            w.set(cur, i, 1, j, mat::LITTER_0);
        }

    // A stepped wall running east, crossing the chunk seam at x = 256 voxels.
    for (int i = 100; i < 420; ++i) {
        const int h = 6 + (i - 100) / 12;
        for (int y = 2; y < 2 + h; ++y)
            for (int j = -4; j <= 4; ++j) w.set(cur, i, y, j, mat::SOIL_0);
    }

    // THE ARCH. A half-torus standing on two feet, with air under it.
    {
        const int cx = -140, cz = 0, span = 90, thick = 9;
        for (int y = 0; y <= span + thick; ++y)
            for (int dx = -(span + thick); dx <= span + thick; ++dx)
                for (int dz = -14; dz <= 14; ++dz) {
                    const float d = std::sqrt(float(dx * dx + y * y));
                    if (d < float(span) || d > float(span + thick)) continue;
                    w.set(cur, cx + dx, 2 + y, cz + dz, mat::ROCK);
                }
        // The feet, so it stands rather than floats.
        for (int side = -1; side <= 1; side += 2)
            for (int y = 0; y < 3; ++y)
                for (int dx = -(span + thick); dx <= -span; ++dx)
                    for (int dz = -14; dz <= 14; ++dz)
                        w.set(cur, cx + side * (-dx), 2 + y, cz + dz, mat::ROCK);
    }

    // A pillar, and a cave carved straight through it -- solid geometry with a
    // hole in it that no height per column can describe.
    {
        const int cx = 60, cz = -110;
        for (int y = 2; y < 160; ++y)
            for (int dz = -22; dz <= 22; ++dz)
                for (int dx = -22; dx <= 22; ++dx) {
                    if (dx * dx + dz * dz > 22 * 22) continue;
                    w.set(cur, cx + dx, y, cz + dz, mat::ROCK);
                }
        for (int y = 20; y < 70; ++y)
            for (int dz = -30; dz <= 30; ++dz)
                for (int dx = -30; dx <= 30; ++dx) {
                    const float q = float((y - 45) * (y - 45)) / (25.0f * 25.0f) +
                                    float(dx * dx) / (40.0f * 40.0f);
                    if (q > 1.0f) continue;
                    w.set(cur, cx + dx, y, cz + dz, mat::AIR);
                }
    }

    // ---- THE MODEL LINE-UP -------------------------------------------------
    //
    // One of everything, in rows, so that "did the assets load and are they the
    // right size" is one look rather than a hunt through a wood. Laid out in
    // VOXELS and kept inside the floor slab on purpose: a model placed past the
    // slab's edge stands on nothing and hangs in the air, which reads as a
    // seating bug in the generator rather than as a demo laid out carelessly.
    if (o.trees) {
        const int edge = half - 60;  // keep everything a few metres off the rim
        if (c.models.loaded()) {
            // The nine pines, at 10 m centres. They are 7 to 11 m across, so
            // this is deliberately just tight enough that the crowns touch --
            // which is what a stand looks like, and what the wood's own 11 m
            // scatter grid is set just above.
            const int n = int(c.models.pines.size());
            for (int k = 0; k < n; ++k) {
                const int x = -(n - 1) * 50 + k * 100;
                if (std::abs(x) > edge) continue;
                Models::stamp(w, cur, c.models.pines[size_t(k)], x, 2, 150, k & 3);
            }
            // A row of decor in front of them, at its real scale beside a 2 m
            // eye -- which is the only way to see that mushroom.vox is 2.3 m
            // tall and not a button.
            //
            // AT z = -25 m, AND THAT IS NOT ARBITRARY. The stepped wall runs
            // along z = 0 from x = 10 to 42 m, which is exactly where the
            // toadstool and the flowers land if the row is laid near the
            // origin -- and a 40 cm flower behind an 8 m wall is a flower you
            // conclude did not load.
            int x = -edge + 40;
            auto row = [&](const std::vector<VoxPiece> &set, int gapVox) {
                for (const VoxPiece &m : set) {
                    if (x > edge) return;
                    Models::stamp(w, cur, m, x, 2, -250, 0);
                    x += gapVox;
                }
            };
            row(c.models.rocksSmall, 40);
            row(c.models.mushrooms, 60);
            row(c.models.flowers, 30);
            // And the two big sets well clear of everything, because they are
            // 6 m and 20 m and would swallow the row above.
            if (!c.models.rocksMid.empty())
                Models::stamp(w, cur, c.models.rocksMid[0], -edge + 60, 2, 300, 0);
            if (!c.models.rocksBig.empty())
                Models::stamp(w, cur, c.models.rocksBig[0], edge - 160, 2, 330, 0);
        } else {
            stampPine(w, cur, c, -40, 150, 2, 0x5bd1e995u);
        }
    }
}

// ---------------------------------------------------------------------------
inline void generate(BrickWorld &w, const Content &c, const GenOptions &o,
                     const WorldConstants &wc) {
    if (o.demo)
        buildDemo(w, c, o);
    else
        buildTerrain(w, c, o, wc);
}

// The one-shot form: resolve the world's constants and build the whole extent.
// Every caller that builds a world in one piece -- the app today, the test
// harness, the probes -- goes through this and is unaffected by the seam above.
inline void generate(BrickWorld &w, const Content &c, const GenOptions &o) {
    generate(w, c, o, WorldConstants::of(o));
}

}  // namespace v4
