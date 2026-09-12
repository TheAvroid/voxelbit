// ---------------------------------------------------------------------------
// models.h -- the .vox assets the wood is actually made of, as lists of voxels.
//
// Nine authored pines, the boulders, the toadstools and the flowers, loaded out
// of `game/assets` and stamped into the brick world as ordinary voxels. A pine
// is not an instance standing beside the ground here -- it IS ground, made of
// the same lattice, and everything that hits terrain hits it by the same test.
//
// GPU-FREE, like everything else under scene/. It includes vox.h, materials.h
// and nothing else, so tests/brick_test.cpp can load the real assets and check
// the real wood.
//
// ---------------------------------------------------------------------------
// A MODEL IS A LIST OF OCCUPIED VOXELS, NOT A DENSE BOX.
//
// A pine is about 109 x 264 x 102 -- 2.9 million cells -- of which roughly
// 38,000 are solid. That is 1.3 % full, because a tree is mostly the air
// between its branches. Stamping from a dense array means scanning 2.9 M cells
// per tree; stamping from the list touches only what exists.
//
// Materials are resolved AT LOAD through the engine's own Palette, so nothing
// downstream ever holds a .vox palette index. 0 is air in both spaces, so the
// two agree about emptiness for free.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "materials.h"
#include "vox.h"

namespace v4 {

struct VoxPiece {
    struct Vox {
        int16_t x, y, z;  // x/z relative to the anchor, y up from the base
        uint8_t m;
    };
    std::vector<Vox> vox;
    int sx = 0, sy = 0, sz = 0;
    int ax = 0, az = 0;  // the anchor inside that extent
    int rMax = 0;        // widest reach from the anchor, for spacing
};

// ---------------------------------------------------------------------------
// Loading, and the two traps that make a .vox file lie to you.
//
// A FILE MAY HOLD SEVERAL PIECES, meaning two different things:
//   pine_1.vox   two pieces at the SAME (x, y) footprint stacked in z -- one
//                256 tall and one 8 -- because a .vox coordinate is a byte and
//                cannot reach past 255. They are ONE tree.
//   flowers.vox  six pieces spread along x at 8-voxel intervals. They are six
//                DIFFERENT flowers.
// Nothing in the format says which you have, so the rule is geometric: pieces
// whose footprints OVERLAP are one object. Take only the first and every tall
// pine loses its top; merge everything and the flowers become one clump.
//
// AND ONLY COLOURS A VOXEL ACTUALLY WEARS GET AN ID. Files ship MagicaVoxel's
// whole 255-entry palette whether the artist used it or not, and every entry
// has alpha 255 -- so "skip the transparent ones" skips nothing, the first
// model claims every free id, and everything after it falls back to grey.
// Walking the voxel data instead costs one pass.
// ---------------------------------------------------------------------------
inline bool loadPieces(const std::string &path, Palette &pal, bool conifer,
                       std::vector<VoxPiece> *out) {
    std::vector<uint8_t> raw;
    VoxScene sc;
    std::string err;
    if (!voxLoadScene(path, &raw, &sc, &err) || sc.pieces.empty()) return false;

    const size_t n = sc.pieces.size();
    std::vector<int> group(n);
    for (size_t i = 0; i < n; ++i) group[i] = int(i);
    auto rootOf = [&](int a) {
        while (group[size_t(a)] != a) a = group[size_t(a)] = group[size_t(group[size_t(a)])];
        return a;
    };
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            const VoxScene::Piece &A = sc.pieces[i];
            const VoxScene::Piece &B = sc.pieces[j];
            if (A.ox < B.ox + B.sx && B.ox < A.ox + A.sx && A.oy < B.oy + B.sy &&
                B.oy < A.oy + A.sy) {
                const int ra = rootOf(int(i)), rb = rootOf(int(j));
                if (ra != rb) group[size_t(rb)] = ra;
            }
        }

    std::vector<uint8_t> idOfEntry(256, uint8_t(mat::AIR));
    std::vector<bool> seen(256, false);
    const size_t before = out->size();

    for (size_t g = 0; g < n; ++g) {
        if (rootOf(int(g)) != int(g)) continue;

        long long lo[3] = {1LL << 40, 1LL << 40, 1LL << 40};
        long long hi[3] = {-(1LL << 40), -(1LL << 40), -(1LL << 40)};
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            if (rootOf(int(i)) != int(g)) continue;
            const VoxScene::Piece &p = sc.pieces[i];
            for (size_t q = 0; q + 4 <= p.voxelBytes; q += 4) {
                if (!p.voxels[q + 3]) continue;
                const long long f[3] = {p.ox + p.voxels[q], p.oy + p.voxels[q + 1],
                                        p.oz + p.voxels[q + 2]};
                for (int k = 0; k < 3; ++k) {
                    lo[k] = (std::min)(lo[k], f[k]);
                    hi[k] = (std::max)(hi[k], f[k]);
                }
                ++total;
            }
        }
        if (!total) continue;

        VoxPiece m;
        m.vox.reserve(total);
        m.sx = int(hi[0] - lo[0] + 1);
        m.sz = int(hi[1] - lo[1] + 1);  // file y is world z
        m.sy = int(hi[2] - lo[2] + 1);  // file z is world y, the height

        // THE ANCHOR IS FILE-SPACE (0, 0), NOT THE BOX CENTRE. MagicaVoxel
        // models are authored about their own origin and the artist put the
        // trunk there; the occupied box is lopsided because branches are, so
        // centring on it leans every trunk by whatever the crown weighed.
        m.ax = (std::min)((std::max)(int(-lo[0]), 0), m.sx - 1);
        m.az = (std::min)((std::max)(int(-lo[1]), 0), m.sz - 1);

        for (size_t i = 0; i < n; ++i) {
            if (rootOf(int(i)) != int(g)) continue;
            const VoxScene::Piece &p = sc.pieces[i];
            for (size_t q = 0; q + 4 <= p.voxelBytes; q += 4) {
                const uint8_t e = p.voxels[q + 3];
                if (!e) continue;
                if (!seen[e]) {
                    seen[e] = true;
                    idOfEntry[e] = pal.forModelColor(sc.pal[size_t(e) - 1], conifer);
                }
                VoxPiece::Vox v;
                v.x = int16_t(p.ox + p.voxels[q] - lo[0] - m.ax);
                v.z = int16_t(p.oy + p.voxels[q + 1] - lo[1] - m.az);
                v.y = int16_t(p.oz + p.voxels[q + 2] - lo[2]);
                v.m = idOfEntry[e];
                m.vox.push_back(v);
                m.rMax = (std::max)(m.rMax, (std::max)(std::abs(int(v.x)), std::abs(int(v.z))));
            }
        }
        out->push_back(std::move(m));
    }
    return out->size() > before;
}

// ---------------------------------------------------------------------------
// EVERYTHING THE WOOD IS MADE OF, AND THE ORDER IT HAS TO BE LOADED IN.
//
// The palette is built FROM THE MODELS, and the order is load-bearing three
// times over:
//
//   1. The pines go first and are marked `conifer`, so forModelColor's
//      green-dominant rule gives their needles a needle's roughness, a needle's
//      translucency and the lift and blue floor that stop a backlit crown
//      reading as a black cut-out.
//   2. markPinesLoaded() runs BETWEEN the pines and everything else. Without it
//      deriveGroundFromTrees walks every entry added so far -- which by then
//      includes the boulders' greys, the toadstool's cap and the flowers'
//      petals -- and the hillside is sampled from a toadstool. That really
//      happened in the engine this came from: the ground came out in patches of
//      brown and patches of grey.
//   3. setStoneBand takes the BOULDERS' own tones, so the stone the terrain is
//      made of is the same stone lying on top of it.
//
// deriveGroundFromTrees is called last, by the caller, once all of that is in
// place.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// WHAT IS IN THE ROCK. Six ore blobs of a dozen voxels each, out of one file.
//
// CLASSIFIED BY COLOUR, NOT BY THE ORDER THEY APPEAR IN. mineral.vox is six
// pieces spread along x, exactly as flowers.vox is, and nothing in the format
// says which is which -- the file simply happens to run coal, diamond, iron,
// emerald, ruby, gold today. Reading it by index means an artist reordering the
// model silently puts diamond in the topsoil and coal at the bottom of the
// world, with nothing to notice it. The albedo is what actually distinguishes
// them, so that is what is asked.
// ---------------------------------------------------------------------------
enum class Ore { Coal, Iron, Emerald, Ruby, Diamond, Gold, Count };

inline const char *oreName(Ore o) {
    switch (o) {
        case Ore::Coal: return "coal";
        case Ore::Iron: return "iron";
        case Ore::Emerald: return "emerald";
        case Ore::Ruby: return "ruby";
        case Ore::Diamond: return "diamond";
        default: return "gold";
    }
}

inline Ore classifyOre(const Vec3 &c);

// ---------------------------------------------------------------------------
// WHICH ORE A BLOB IS: CLASSIFY EVERY VOXEL AND TAKE THE MAJORITY.
//
// NOT the single most common colour, which is what this did first and which
// got emerald wrong every time. Each blob is a gem set in a MATRIX -- a dark
// red-brown rock, id 54, the same one in several of the pieces -- and in the
// emerald that matrix is tied for the most-used single entry. The tie went to
// the rock, the rock reads as ruby, and emerald was never placed at all while
// ruby was placed twice.
//
// Classifying per voxel and counting the answers is immune to that: the gem is
// sixteen voxels of green against six of matrix whichever entry happens to be
// the single commonest, and the matrix only wins on a blob that really is
// mostly matrix.
// ---------------------------------------------------------------------------
inline Ore oreOfPiece(const VoxPiece &p, const Palette &pal) {
    int votes[int(Ore::Count)] = {0};
    for (const VoxPiece::Vox &v : p.vox) votes[int(classifyOre(pal.table()[size_t(v.m)].albedo))]++;
    int best = 0;
    for (int k = 1; k < int(Ore::Count); ++k)
        if (votes[k] > votes[best]) best = k;
    return Ore(best);
}

inline Ore classifyOre(const Vec3 &c) {
    const float mx = std::max(c.x, std::max(c.y, c.z));
    // COAL FIRST, because it is the only one defined by being dark rather than
    // by a hue, and a near-black grey has no meaningful hue to test.
    if (mx < 0.15f) return Ore::Coal;
    if (c.z > c.x && c.z > c.y) return Ore::Diamond;  // the only blue one
    if (c.y > c.x) return Ore::Emerald;               // the only green one
    // Red-dominant: ruby is almost pure red, gold is red and green together,
    // and iron is the brown in between. The green fraction separates all three.
    const float g = (c.x > 0.0f) ? c.y / c.x : 0.0f;
    if (g < 0.20f) return Ore::Ruby;
    if (g > 0.60f) return Ore::Gold;
    return Ore::Iron;
}

struct Models {
    std::vector<VoxPiece> pines;

    // One list per ore, so a cluster can pick a kind and then a variant. Most
    // hold exactly one piece; the split is what lets a seam be all one mineral.
    std::vector<VoxPiece> ore[int(Ore::Count)];
    size_t oreCount() const {
        size_t n = 0;
        for (int k = 0; k < int(Ore::Count); ++k) n += ore[k].size();
        return n;
    }

    // THE BOULDERS COME IN THREE SIZES AND THEY CANNOT SHARE A SCATTER GRID.
    //
    // The 26 rock models run from 0.7 m to 22.8 m tall. Put them all on one
    // grid and either the small ones are so sparse you never see one, or the
    // big ones overlap each other and the trees. So they are kept apart here,
    // at the size boundaries the assets were authored to, and each set gets a
    // cell comfortably wider than the thing it places -- see the note on the
    // cell sizes in GenOptions.
    //
    //     big    5 models, 12.2 - 22.8 m   landmarks, a handful per wood
    //     mid    6 models,  5.7 -  7.7 m   boulders you walk around
    //     small 15 models,  0.7 -  2.3 m   stones you walk over
    std::vector<VoxPiece> rocksBig, rocksMid, rocksSmall;
    std::vector<VoxPiece> mushrooms, flowers;
    std::string status;
    int missing = 0;

    size_t rockCount() const { return rocksBig.size() + rocksMid.size() + rocksSmall.size(); }

    // Where mineral.vox lives. Its own path because it is not in the decoration
    // folder with the rest -- see --minerals.
    std::string mineralPath;

    bool load(const std::string &pineDir, const std::string &decorDir, Palette &pal) {
        if (pineDir.empty()) {
            status = "no --pines directory";
            return false;
        }
        for (int i = 1; i <= 9; ++i)
            if (!loadPieces(pineDir + "/pine_" + std::to_string(i) + ".vox", pal, true, &pines))
                ++missing;
        pal.markPinesLoaded();

        // NOT conifer: a boulder's greys must not be offered to the foliage
        // ramp, and a toadstool's cap is not a needle either. A flower is the
        // borderline one and it is deliberately not conifer either -- the
        // foliage rule applies a 1.7x lift calibrated for an authored olive
        // that is nearly black once linearised, and on a saturated petal that
        // puts the albedo past one, which is a surface returning more light
        // than reaches it.
        if (!decorDir.empty()) {
            // NAMED, NOT GLOBBED. A fixed list means a missing file is a
            // warning about THAT file rather than a wood that silently has
            // fewer rocks in it than it should.
            static const char *kBig[] = {"BIG_1_BiG_0", "Big_2_BiG_0", "Big_3_BiG_0",
                                         "Big_4_BiG_0", "Big_5_BiG_0"};
            static const char *kMid[] = {"Mid_1_MID_0", "Mid_2_MID_0",     "Mid_3_MID_0",
                                         "Mid_4_MID_0", "Mid_4_MID_0_001", "Mid_5_MID_0"};
            static const char *kSmall[] = {
                "Runic_1_Runic_0", "Runic_2_Runic_0", "Runic_3_Runic_0", "Runic_4_Runic_0",
                "Runic_5_Runic_0", "Runic_6_Runic_0", "Runic_7_Runic_0", "Small_1_SMall_0",
                "Small_2_SMall_0", "Small_3_SMall_0", "Small_4_SMall_0", "Small_5_SMall_0",
                "Small_6_SMall_0", "Small_7_SMall_0", "Small_8_SMall_0"};
            const std::string rd = decorDir + "/rocks/";
            for (const char *n : kBig) loadPieces(rd + n + ".vox", pal, false, &rocksBig);
            for (const char *n : kMid) loadPieces(rd + n + ".vox", pal, false, &rocksMid);
            for (const char *n : kSmall) loadPieces(rd + n + ".vox", pal, false, &rocksSmall);
            // The loose pebble beside the set, which is what the older engines
            // scattered when they scattered one thing.
            loadPieces(decorDir + "/rock.vox", pal, false, &rocksSmall);

            loadPieces(decorDir + "/mushroom.vox", pal, false, &mushrooms);
            loadPieces(decorDir + "/flowers.vox", pal, false, &flowers);
        }

        // -- THE ORES, sorted into kinds by their own colour ----------------
        //
        // AFTER markPinesLoaded, like everything else that is not a tree. A
        // gem's colour must never reach deriveGroundFromTrees: emerald is the
        // most saturated green in the palette and a hillside sampled from it
        // would be luminous.
        if (!mineralPath.empty()) {
            std::vector<VoxPiece> raw;
            if (loadPieces(mineralPath, pal, false, &raw)) {
                for (VoxPiece &m : raw) {
                    const Ore k = oreOfPiece(m, pal);
                    // GEMS AND METAL ARE NOT BARK. forModelColor gave every one
                    // of these a matt bark finish, because that is what it does
                    // with anything not green on a conifer. A ruby wants a
                    // highlight to read as a gem at all, and coal wants the
                    // opposite -- it is the one thing down there that should
                    // stay dull.
                    const bool dull = (k == Ore::Coal);
                    for (const VoxPiece::Vox &v : m.vox)
                        pal.setFinish(v.m, dull ? 0.94f : 0.24f, dull ? 0.02f : 0.35f);
                    ore[int(k)].push_back(std::move(m));
                }
            } else {
                ++missing;
            }
        }

        // AND THE STONE RAMP FROM THE ROCKS THEMSELVES. mat::ROCK alone is one
        // flat grey, which is what dug stone looks like beside boulders carrying
        // half a dozen real tones. setStoneBand spreads the ground's single rock
        // id over the models' own greys -- and drops anything too saturated to
        // be stone, so the moss grown over a boulder does not end up inside a
        // hillside.
        {
            std::vector<std::array<uint8_t, 4>> greys;
            const std::vector<VoxPiece> *sets[] = {&rocksBig, &rocksMid, &rocksSmall};
            for (const std::vector<VoxPiece> *set : sets)
                for (const VoxPiece &r : *set)
                    for (const VoxPiece::Vox &v : r.vox) {
                        if (greys.size() >= 256) break;
                        const MaterialLook &L = pal.table()[v.m];
                        greys.push_back({uint8_t(L.albedo.x * 255.0f),
                                         uint8_t(L.albedo.y * 255.0f),
                                         uint8_t(L.albedo.z * 255.0f), 255});
                    }
            if (!greys.empty()) pal.setStoneBand(std::move(greys));
        }

        status = std::to_string(pines.size()) + " pines, " + std::to_string(rockCount()) +
                 " rocks (" + std::to_string(rocksBig.size()) + " big, " +
                 std::to_string(rocksMid.size()) + " mid, " + std::to_string(rocksSmall.size()) +
                 " small), " + std::to_string(mushrooms.size()) + " mushrooms, " +
                 std::to_string(flowers.size()) + " flowers, " + std::to_string(oreCount()) +
                 " ores, " + std::to_string(pal.used()) + " materials";
        if (missing) status += ", " + std::to_string(missing) + " FILES MISSING";
        return !pines.empty();
    }

    bool loaded() const { return !pines.empty(); }

    // A quarter turn about y. Exact -- no resampling, no seams -- which is what
    // makes nine models look like far more than nine.
    static void rotate(int rot, int x, int z, int *ox, int *oz) {
        switch (rot & 3) {
            case 0:  *ox = x;  *oz = z;  break;
            case 1:  *ox = -z; *oz = x;  break;
            case 2:  *ox = -x; *oz = -z; break;
            default: *ox = z;  *oz = -x; break;
        }
    }

    static uint32_t hash2(int a, int b, uint32_t salt) {
        uint32_t h = uint32_t(a) * 374761393u + uint32_t(b) * 668265263u + salt;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    // Put one model into the world with its base at voxel height `y`.
    template <class World, class Cursor>
    static void stamp(World &w, Cursor &cur, const VoxPiece &m, int i, int y, int j, int rot) {
        for (const VoxPiece::Vox &p : m.vox) {
            int ox, oz;
            rotate(rot, p.x, p.z, &ox, &oz);
            w.set(cur, i + ox, y + p.y, j + oz, p.m);
        }
    }
};

}  // namespace v4
