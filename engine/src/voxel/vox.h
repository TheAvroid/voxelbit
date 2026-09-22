// ---------------------------------------------------------------------------
// vox.h -- a MagicaVoxel .vox reader, ported from voxelbit's rust/src/vox.rs.
//
// TWO COORDINATE SYSTEMS, AND THE ONE PLACE THEY MEET
// MagicaVoxel is Z-UP: a model's sz is its height and sx/sy are the ground
// plane. This renderer is Y-UP. The swap happens exactly once, in toWorld(),
// which emits the layout the stamper wants -- so nothing downstream has to
// remember which convention it is holding. Getting this wrong does not crash;
// it lays the tree on its side.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace v2 {

// A parsed model in the file's own Z-up layout, indexed x + y*sx + z*sx*sy.
struct VoxModel {
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> m;                    // palette indices; 0 is empty
    std::array<std::array<uint8_t, 4>, 255> pal{};  // entry i (1-based) is pal[i-1]

    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return 0;
        return m[size_t(x) + size_t(y) * sx + size_t(z) * sx * sy];
    }
};

// -- HOW BIG A .vox IS ALLOWED TO CLAIM TO BE --------------------------------
//
// A SANITY CHECK ON A FILE, NOT A BUDGET. Nothing structural in this parser
// cares how large a model is -- every index is long long or size_t and the
// storage is one flat vector -- so this exists purely so that a truncated or
// hand-edited header claiming a few billion cells allocates nothing and is
// reported as a bad file instead.
//
// IT WAS 64 M AND NUKETOWN AT 3x DOES NOT FIT (user 2026-09-17: "revoxelize
// the nuketown scene and make it 3x bigger"). That map is 489 x 285 x 977 =
// 136 M cells, which is 130 MB as the uint8 grid this fills -- entirely
// ordinary, and it was being refused as "implausible model dimensions", which
// is the least helpful message it could have produced.
//
// ...AND 256 M DOES NOT FIT THE ARCADE (user 2026-09-18: "put it next to the
// depot map. move the maps 100 meters from one another"). Several maps in ONE
// grid is mostly AIR: a hundred metres of gap between two of them, across a
// 68 m frontage and 28 m of headroom, is 193 M cells of nothing on its own --
// more than the maps either side of it. The arcade as it stands is 677 x 285 x
// 2918 = 563 M.
//
// ...AND 768 M DOES NOT FIT IT EITHER, ONCE A MAP DOUBLES (user 2026-09-18:
// "revoxelize the canyons map. make it twice as large"). A dense grid is a
// BOX, so doubling one map costs eight times the cells AND doubles the
// frontage the gap has to span AND pads every other map out to the new width.
// Measured, the whole of it:
//
//     canyon at 2x   1355 x 473 x 1884   1207 M
//     the 100 m gap  1355 x 473 x 1000    641 M
//     nuketown       1355 x 473 x  977    626 M   (489 wide, padded to 1355)
//                                        -------
//                                        2475 M
//
// 3 G IS THE LINE NOW. What it costs is worth stating plainly, because the
// count is the smallest of the three numbers: the engine holds the level TWICE
// -- levelVol_ and the dressed levelDisplay_ -- so a grid this size is about
// **5 GB of host memory**, and the meshed result is tens of millions of
// triangles in the block BLAS. The .vox FILE does not grow with it (empty
// pieces are never written) and neither does VRAM per empty cell, so the thing
// to watch when this next moves is the TRIANGLE count the voxelizer prints,
// not this constant.
//
// GAP_M IN THE VOXELIZER IS THE CHEAPEST THING TO TURN DOWN if it ever has to
// come back: a quarter of this budget is a hundred metres of nothing.
inline constexpr long long kVoxMaxCells = 3072LL << 20;

// A model in WORLD layout: x and z horizontal, y vertical, indexed
// x + z*sx + y*sx*sz -- the same indexing the voxel grid uses.
struct VoxAsset {
    int sx = 0, sy = 0, sz = 0;  // sy is the height
    std::vector<uint8_t> a;

    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return 0;
        return a[size_t(x) + size_t(z) * sx + size_t(y) * sx * sz];
    }
    int voxelCount() const {
        int n = 0;
        for (uint8_t v : a)
            if (v) ++n;
        return n;
    }
};

// MagicaVoxel's built-in palette, for files carrying no RGBA chunk. Generated
// from the documented 6x6x6 ramp rather than pasted as 255 literals.
inline std::array<std::array<uint8_t, 4>, 255> defaultPalette() {
    std::array<std::array<uint8_t, 4>, 255> p{};
    const uint8_t lv[6] = {255, 204, 153, 102, 51, 0};
    size_t i = 0;
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b)
                if (i < 255) p[i++] = {lv[r], lv[g], lv[b], 255};
    return p;
}

inline int32_t i32le(const uint8_t *b, size_t o) {
    int32_t v;
    std::memcpy(&v, b + o, 4);
    return v;
}

// ---------------------------------------------------------------------------
// THE SCENE GRAPH, AND WHY A TREE NEEDS ONE
//
// A .vox XYZI record packs each coordinate in a single byte, so no one model
// can be more than 256 voxels on a side -- 25.6 m on this 10 cm grid. The 100 ft
// pines and birches are 305 voxels tall and do not fit in one.
//
// MagicaVoxel's own answer to that is the scene graph: a file may hold SEVERAL
// models, each placed by an nTRN transform, so a 30 m tree is two stacked
// objects rather than one illegal model. That is what tools/revoxel_trees_tall.py
// writes and what the code below composes back into the single grid the stamper
// wants. The files stay ordinary MagicaVoxel documents -- open one and you get
// pieces you can edit -- and the 256 ceiling applies to each PIECE, not to the
// tree.
//
// ONLY TRANSLATION IS HONOURED. Every sub-model shipped here is axis-aligned,
// and a rotated one would need the 3x3 basis packed into _r that this
// deliberately does not carry. A rotated file composes as though it were
// identity, which is visibly wrong rather than quietly wrong.
// ---------------------------------------------------------------------------

// One SIZE/XYZI pair, still pointing into the caller's buffer.
struct VoxChunkModel {
    int sx = 0, sy = 0, sz = 0;
    const uint8_t *voxels = nullptr;
    size_t voxelBytes = 0;
};

// A scene-graph node. nTRN carries one child and a translation, nGRP a list of
// children, nSHP a list of model indices -- one struct holds all three because
// the traversal treats them almost identically.
struct VoxNode {
    enum Kind { Trn, Grp, Shp };
    int id = -1;
    Kind kind = Trn;
    int tx = 0, ty = 0, tz = 0;
    std::vector<int> kids;  // child node ids, or model indices when kind is Shp
};

// Walk a MagicaVoxel DICT: an int32 count, then that many (STRING key, STRING
// value) pairs, each STRING an int32 length followed by its bytes. Returns the
// offset just past the dict, or 0 if it runs off the end of the chunk -- so
// every caller can treat 0 as "malformed, stop". When wantKey is set the
// matching value is copied out.
inline size_t voxSkipDict(const std::vector<uint8_t> &raw, size_t o, size_t end,
                          const char *wantKey, std::string *value) {
    if (o + 4 > end) return 0;
    const int32_t n = i32le(raw.data(), o);
    o += 4;
    if (n < 0) return 0;
    for (int i = 0; i < n; ++i) {
        std::string kv[2];
        for (int half = 0; half < 2; ++half) {
            if (o + 4 > end) return 0;
            const int32_t len = i32le(raw.data(), o);
            o += 4;
            if (len < 0 || o + size_t(len) > end) return 0;
            kv[half].assign(reinterpret_cast<const char *>(raw.data()) + o, size_t(len));
            o += size_t(len);
        }
        if (wantKey && value && kv[0] == wantKey) *value = kv[1];
    }
    return o;
}

// "_t" is three space-separated integers. A malformed one leaves the
// translation at zero, which stacks that piece at the origin -- wrong, but in
// bounds.
inline void voxTranslation(const std::string &s, VoxNode *node) {
    int v[3] = {0, 0, 0};
    if (std::sscanf(s.c_str(), "%d %d %d", &v[0], &v[1], &v[2]) == 3) {
        node->tx = v[0];
        node->ty = v[1];
        node->tz = v[2];
    }
}

// One nTRN, nGRP or nSHP chunk. Anything that does not parse cleanly is dropped
// by the caller rather than failing the file: a graph we cannot read falls back
// to the single-model path, which is what this reader did before the graph
// existed.
inline bool voxParseNode(const std::vector<uint8_t> &raw, const uint8_t *id, size_t body,
                         size_t end, VoxNode *node) {
    size_t o = body;
    if (o + 4 > end) return false;
    node->id = i32le(raw.data(), o);
    o += 4;

    if (std::memcmp(id, "nTRN", 4) == 0) {
        node->kind = VoxNode::Trn;
        o = voxSkipDict(raw, o, end, nullptr, nullptr);
        if (!o || o + 16 > end) return false;
        const int child = i32le(raw.data(), o);
        const int frames = i32le(raw.data(), o + 12);
        o += 16;
        // The translation lives in the FRAME dict, not the node's own. A static
        // scene has exactly one frame; a keyframed one takes the first.
        for (int f = 0; f < frames; ++f) {
            std::string t;
            o = voxSkipDict(raw, o, end, "_t", &t);
            if (!o) return false;
            if (f == 0) voxTranslation(t, node);
        }
        node->kids.push_back(child);
        return true;
    }
    if (std::memcmp(id, "nGRP", 4) == 0) {
        node->kind = VoxNode::Grp;
        o = voxSkipDict(raw, o, end, nullptr, nullptr);
        if (!o || o + 4 > end) return false;
        const int n = i32le(raw.data(), o);
        o += 4;
        if (n < 0 || o + size_t(n) * 4 > end) return false;
        for (int i = 0; i < n; ++i) node->kids.push_back(i32le(raw.data(), o + size_t(i) * 4));
        return true;
    }
    if (std::memcmp(id, "nSHP", 4) == 0) {
        node->kind = VoxNode::Shp;
        o = voxSkipDict(raw, o, end, nullptr, nullptr);
        if (!o || o + 4 > end) return false;
        const int n = i32le(raw.data(), o);
        o += 4;
        if (n < 0) return false;
        for (int i = 0; i < n; ++i) {
            if (o + 4 > end) return false;
            node->kids.push_back(i32le(raw.data(), o));
            o += 4;
            o = voxSkipDict(raw, o, end, nullptr, nullptr);  // per-model attributes
            if (!o) return false;
        }
        return true;
    }
    return false;
}

// Parse a .vox file into ONE model.
//
// Chunks are walked rather than assumed to be in a fixed order, and unknown
// ones (MATL, LAYR, rCAM and the rest) are skipped by their declared size -- a
// reader that assumed SIZE and XYZI came first would work on these files and
// break on the next MagicaVoxel export.
//
// A file holding one model is read exactly as it always was: the graph is
// ignored and the grid is that model's own. A file holding several is composed
// through its nTRN translations into one grid -- see the note above; that is
// how a tree taller than the format's own ceiling is stored.
//
// THIS CHANGED WHAT A MULTI-MODEL FILE MEANS HERE, so it is worth being plain
// about: voxParse used to return the FIRST model and silently drop the rest, and
// it now returns all of them assembled. That is the right reading for this
// function -- voxLoad means "one asset", and an asset in several pieces is still
// one asset -- while "several assets in one file" is what voxLoadAll below is
// for. The only shipped files carrying more than one model are flowers.vox and
// mushroom.vox; both go through voxLoadAll, so nothing in the engine reads them
// through here. A caller that wanted just the first model of a row wants
// voxLoadAll and an index.
// ---------------------------------------------------------------------------
// A FILE BEFORE ANYTHING IS COMPOSED: every shape the scene graph places, with
// the corner it places it at, and the palette they share.
//
// voxParse below flattens all of this into one grid, which is what a tree or a
// rock wants -- the stamper has no use for knowing a pine was authored in two
// pieces. The BOW does: its file is one arrow and six bow frames sharing a
// node, and composing them into a single grid would lay all seven on top of
// each other. It needs the pieces and their placements, which is exactly what
// voxParse computes on its way past and used to throw away.
//
// ONE WALK, TWO CONSUMERS. voxParse is a thin composer over this now rather
// than a second copy of the chunk reader: a .vox file is parsed in exactly one
// place, so a fix to the graph traversal cannot reach the trees and miss the
// bow.
// ---------------------------------------------------------------------------
struct VoxScene {
    struct Piece {
        int sx = 0, sy = 0, sz = 0;        // the model's own extent
        long long ox = 0, oy = 0, oz = 0;  // its MINIMUM corner in file space
        const uint8_t *voxels = nullptr;   // x, y, z, colour index quads
        size_t voxelBytes = 0;
        // WHICH nSHP NODE PUT IT THERE, and it is not decoration. MagicaVoxel
        // lets one shape carry SEVERAL models -- that is how the bow's seven
        // draw frames are authored, all on one node and all centred on the same
        // translation -- so "which piece belongs with which" is a question only
        // the shape can answer. Composing a file ignores it; the bow's strip is
        // built out of it. See parseBowStrip in render/bow.h.
        int shape = 0;
    };
    // In scene-graph order, which is the order MagicaVoxel lists the shapes and
    // therefore the order an artist thinks of them in.
    std::vector<Piece> pieces;
    std::array<std::array<uint8_t, 4>, 255> pal{};

    // The colour index at (x, y, z) of one piece, in that piece's own frame.
    uint8_t at(size_t i, int x, int y, int z) const {
        const Piece &p = pieces[i];
        if (x < 0 || y < 0 || z < 0 || x >= p.sx || y >= p.sy || z >= p.sz) return 0;
        for (size_t q = 0; q + 4 <= p.voxelBytes; q += 4)
            if (p.voxels[q] == x && p.voxels[q + 1] == y && p.voxels[q + 2] == z)
                return p.voxels[q + 3];
        return 0;
    }
};

// The pieces of a file, still pointing into `raw` -- which must outlive the
// scene, exactly as VoxChunkModel does.
inline bool voxParseScene(const std::vector<uint8_t> &raw, VoxScene *out, std::string *err) {
    auto fail = [&](const char *why) {
        if (err) *err = why;
        return false;
    };
    if (raw.size() <= 8 || std::memcmp(raw.data(), "VOX ", 4) != 0) return fail("not a .vox file");

    std::vector<VoxChunkModel> models;
    std::vector<VoxNode> nodes;
    bool haveSize = false;
    int sx = 0, sy = 0, sz = 0;
    out->pal = defaultPalette();
    out->pieces.clear();

    size_t o = 8;
    while (o + 12 <= raw.size()) {
        const uint8_t *id = raw.data() + o;
        const size_t content = size_t(std::max(0, i32le(raw.data(), o + 4)));
        const size_t children = size_t(std::max(0, i32le(raw.data(), o + 8)));
        const size_t body = o + 12;
        if (body + content > raw.size()) break;

        if (std::memcmp(id, "MAIN", 4) == 0) {
            // Descend into MAIN rather than skipping it: its children are the payload.
            o = body + content;
            continue;
        }
        if (std::memcmp(id, "SIZE", 4) == 0 && content >= 12) {
            sx = i32le(raw.data(), body);
            sy = i32le(raw.data(), body + 4);
            sz = i32le(raw.data(), body + 8);
            haveSize = (sx > 0 && sy > 0 && sz > 0);
        } else if (std::memcmp(id, "XYZI", 4) == 0 && content >= 4 && haveSize) {
            const size_t n = size_t(std::max(0, i32le(raw.data(), body)));
            if (content < 4 + n * 4) return fail("XYZI shorter than its own count");
            VoxChunkModel m;
            m.sx = sx;
            m.sy = sy;
            m.sz = sz;
            m.voxels = raw.data() + body + 4;
            m.voxelBytes = n * 4;
            models.push_back(m);
            haveSize = false;  // one XYZI per SIZE
        } else if (std::memcmp(id, "RGBA", 4) == 0 && content >= 1024) {
            for (int i = 0; i < 255; ++i) {
                const size_t q = body + size_t(i) * 4;
                out->pal[size_t(i)] = {raw[q], raw[q + 1], raw[q + 2], raw[q + 3]};
            }
        } else if (std::memcmp(id, "nTRN", 4) == 0 || std::memcmp(id, "nGRP", 4) == 0 ||
                   std::memcmp(id, "nSHP", 4) == 0) {
            VoxNode node;
            if (voxParseNode(raw, id, body, body + content, &node)) nodes.push_back(node);
        }
        o = body + content + children;
    }

    if (models.empty()) return fail("no SIZE/XYZI pair");

    struct Placed {
        int model, x, y, z, shape;
    };
    std::vector<Placed> placed;
    if (models.size() == 1) {
        placed.push_back({0, 0, 0, 0, 0});
    } else {
        // Depth-first from node 0, MagicaVoxel's root, summing translations on
        // the way down. An explicit stack rather than recursion, and a visited
        // set, so a malformed or cyclic graph terminates instead of blowing the
        // C stack.
        std::vector<bool> seen(nodes.size(), false);
        std::vector<std::array<int, 4>> stack;  // node id, then the translation so far
        stack.push_back({0, 0, 0, 0});
        while (!stack.empty()) {
            const std::array<int, 4> cur = stack.back();
            stack.pop_back();
            size_t idx = nodes.size();
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].id == cur[0]) {
                    idx = i;
                    break;
                }
            if (idx == nodes.size() || seen[idx]) continue;
            seen[idx] = true;
            const VoxNode &n = nodes[idx];
            const int tx = cur[1] + n.tx, ty = cur[2] + n.ty, tz = cur[3] + n.tz;
            if (n.kind == VoxNode::Shp) {
                for (int m : n.kids)
                    if (m >= 0 && m < int(models.size()))
                        placed.push_back({m, tx, ty, tz, n.id});
            } else {
                for (int k : n.kids) stack.push_back({k, tx, ty, tz});
            }
        }
        // Several models but no graph we could follow: fall back to the first
        // one alone, which is what this function returned before it could
        // compose. A short tree beats no tree.
        if (placed.empty()) placed.push_back({0, 0, 0, 0, 0});
    }

    // nTRN gives a piece's CENTRE, so its minimum corner is that translation
    // less half its size, truncated -- the same integer halving MagicaVoxel
    // does on the way in.
    for (const Placed &p : placed) {
        const VoxChunkModel &m = models[size_t(p.model)];
        VoxScene::Piece pc;
        pc.sx = m.sx;
        pc.sy = m.sy;
        pc.sz = m.sz;
        pc.ox = p.x - m.sx / 2;
        pc.oy = p.y - m.sy / 2;
        pc.oz = p.z - m.sz / 2;
        pc.voxels = m.voxels;
        pc.voxelBytes = m.voxelBytes;
        pc.shape = p.shape;
        out->pieces.push_back(pc);
    }
    return true;
}

// One file, every piece composed into a single grid. What a tree or a rock
// wants; see VoxScene for the case that wants the pieces kept apart.
//
// -- ONE PIECE PER SHAPE, BECAUSE THE REST ARE FRAMES ----------------------
//
// A shape node carrying SEVERAL models is not an object in several parts, it is
// one object with an ANIMATION: MagicaVoxel writes a keyframed model that way,
// every frame on the same node and centred on the same translation. Composing
// those lays the whole cycle on top of itself.
//
// REPORTED AS "the koi fish didn't load right, it's loading all frames at once"
// (user 2026-09-13), and that is exactly what it was. assets/life/koi.vox holds
// TWELVE frames of a swim cycle under one shape; composed, its 26 voxels became
// 130 in the same 5 x 10 x 4 box -- a koi five times too solid, which is not a
// koi. The strips that work (salmon, bass, catfish) are numbered FILES in a
// folder, so they never came through here.
//
// A TREE IS UNAFFECTED, and that is the whole reason the test is per SHAPE
// rather than per file. The pieces a tall model is split across sit on
// DIFFERENT shape nodes at different translations -- see the note over
// VoxScene -- so each contributes its first (and only) model and the
// composition is what it always was.
//
// THE SCENE ITSELF STILL CARRIES EVERY FRAME. Only this function, the
// compose-into-one path, drops them; voxParseScene is the raw truth and the
// bow's seven draw frames are cut out of it (see parseBowStrip). A reader that
// wants the cycle asks for it -- voxLoadAll, or the scene -- and gets it.
inline bool voxParse(const std::vector<uint8_t> &raw, VoxModel *out, std::string *err) {
    VoxScene sc;
    if (!voxParseScene(raw, &sc, err)) return false;

    // The first piece of each shape node, in the order the walk found them.
    std::vector<VoxScene::Piece> use;
    use.reserve(sc.pieces.size());
    {
        std::vector<int> seen;
        for (const VoxScene::Piece &p : sc.pieces) {
            bool had = false;
            for (int sh : seen)
                if (sh == p.shape) { had = true; break; }
            if (had) continue;
            seen.push_back(p.shape);
            use.push_back(p);
        }
    }
    if (use.empty()) {
        if (err) *err = "no models in file";
        return false;
    }

    long long minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    for (size_t i = 0; i < use.size(); ++i) {
        const VoxScene::Piece &p = use[i];
        if (i == 0) {
            minX = p.ox; minY = p.oy; minZ = p.oz;
            maxX = p.ox + p.sx; maxY = p.oy + p.sy; maxZ = p.oz + p.sz;
            continue;
        }
        minX = std::min(minX, p.ox);
        minY = std::min(minY, p.oy);
        minZ = std::min(minZ, p.oz);
        maxX = std::max(maxX, p.ox + p.sx);
        maxY = std::max(maxY, p.oy + p.sy);
        maxZ = std::max(maxZ, p.oz + p.sz);
    }

    const long long w = maxX - minX, h = maxY - minY, d = maxZ - minZ;
    if (w <= 0 || h <= 0 || d <= 0 ||
        double(w) * double(h) * double(d) > double(kVoxMaxCells)) {
        if (err) *err = "implausible model dimensions";
        return false;
    }

    out->sx = int(w);
    out->sy = int(h);
    out->sz = int(d);
    out->pal = sc.pal;
    out->m.assign(size_t(w) * size_t(h) * size_t(d), 0);
    for (const VoxScene::Piece &p : use) {
        const long long ox = p.ox - minX, oy = p.oy - minY, oz = p.oz - minZ;
        for (size_t q = 0; q + 4 <= p.voxelBytes; q += 4) {
            const int x = p.voxels[q], y = p.voxels[q + 1], z = p.voxels[q + 2];
            const uint8_t c = p.voxels[q + 3];
            // Out-of-range voxels are dropped rather than fatal: a hand-edited
            // file occasionally carries one past its own SIZE, and losing it
            // beats refusing the tree.
            if (x >= p.sx || y >= p.sy || z >= p.sz) continue;
            const long long wx = ox + x, wy = oy + y, wz = oz + z;
            if (wx < 0 || wy < 0 || wz < 0 || wx >= w || wy >= h || wz >= d) continue;
            out->m[size_t(wx) + size_t(wy) * size_t(w) + size_t(wz) * size_t(w) * size_t(h)] = c;
        }
    }
    return true;
}

inline bool voxParseAll(const std::vector<uint8_t> &raw, std::vector<VoxModel> *out,
                        std::string *err) {
    if (raw.size() <= 8 || std::memcmp(raw.data(), "VOX ", 4) != 0) {
        if (err) *err = "not a .vox file";
        return false;
    }

    auto pal = defaultPalette();
    // Where this call's models start -- see the assignment at the end. *out is
    // an ACCUMULATOR across a whole model set, not a fresh vector per file.
    const size_t firstModel = out->size();
    int sx = 0, sy = 0, sz = 0;
    bool haveSize = false;

    size_t o = 8;
    while (o + 12 <= raw.size()) {
        const uint8_t *id = raw.data() + o;
        const size_t content = size_t(std::max(0, i32le(raw.data(), o + 4)));
        const size_t children = size_t(std::max(0, i32le(raw.data(), o + 8)));
        const size_t body = o + 12;
        if (body + content > raw.size()) break;

        if (std::memcmp(id, "MAIN", 4) == 0) {
            o = body + content;  // descend: the children are the payload
            continue;
        }
        if (std::memcmp(id, "SIZE", 4) == 0 && content >= 12) {
            sx = i32le(raw.data(), body);
            sy = i32le(raw.data(), body + 4);
            sz = i32le(raw.data(), body + 8);
            haveSize = (sx > 0 && sy > 0 && sz > 0 &&
                        double(sx) * sy * sz <= double(kVoxMaxCells));
        } else if (std::memcmp(id, "XYZI", 4) == 0 && content >= 4 && haveSize) {
            const size_t n = size_t(std::max(0, i32le(raw.data(), body)));
            if (content >= 4 + n * 4) {
                VoxModel m;
                m.sx = sx;
                m.sy = sy;
                m.sz = sz;
                m.m.assign(size_t(sx) * sy * sz, 0);
                const uint8_t *v = raw.data() + body + 4;
                for (size_t q = 0; q + 4 <= n * 4; q += 4) {
                    const int x = v[q], y = v[q + 1], z = v[q + 2];
                    if (x < sx && y < sy && z < sz)
                        m.m[size_t(x) + size_t(y) * sx + size_t(z) * sx * sy] = v[q + 3];
                }
                out->push_back(std::move(m));
            }
            haveSize = false;  // one XYZI per SIZE
        } else if (std::memcmp(id, "RGBA", 4) == 0 && content >= 1024) {
            for (int i = 0; i < 255; ++i) {
                const size_t p = body + size_t(i) * 4;
                pal[i] = {raw[p], raw[p + 1], raw[p + 2], raw[p + 3]};
            }
        }
        o = body + content + children;
    }

    if (out->empty()) {
        if (err) *err = "no models in file";
        return false;
    }
    // -- ONLY THE MODELS THIS CALL ADDED --------------------------------
    //
    // (user 2026-09-20: "the black is on the cactus shrub itself".)
    //
    // THIS WAS `for (VoxModel &m : *out)` AND *out ACCUMULATES. loadModelSet
    // hands the same vector to every file in a set, so each file's palette was
    // stamped over every model loaded BEFORE it -- and the last file won.
    //
    // desert_shrub/6.vox has entries 8..16 zeroed (it uses only the olives),
    // while 1..5.vox carry the pinks and the cream there. Loading 6 last gave
    // all six shrubs a palette with BLACK where their blossom should be, which
    // is exactly the reported symptom. Measured:
    //
    //     1.vox pal[8] = (243,130,153)   ... 5.vox the same
    //     6.vox pal[8] = (  0,  0,  0)   <- stamped over all of them
    //
    // A file's palette belongs to ITS models and to nothing else.
    for (size_t i = firstModel; i < out->size(); ++i) (*out)[i].pal = pal;
    return true;
}

inline bool voxLoadAll(const std::string &path, std::vector<VoxModel> *out, std::string *err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(size_t(n > 0 ? n : 0));
    const size_t got = raw.empty() ? 0 : std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    raw.resize(got);
    if (!voxParseAll(raw, out, err)) return false;
    // -- V2_VOX_PROBE: WHAT THIS FILE'S PALETTE ACTUALLY CONTAINS -----------
    //
    // A substring of a path, or '*' for every file. Prints entry 8 -- the
    // first past the desert shrub's eight olives.
    //
    // HERE AND NOT IN voxParseAll, which takes raw BYTES and cannot say what
    // file they came from. The first version lived there, printed for every
    // asset in the game, and I read a pine's palette as a shrub's and believed
    // it. A diagnostic that cannot name its subject is not a diagnostic.
    if (const char *pp = std::getenv("V2_VOX_PROBE"))
        if ((*pp == 42 || path.find(pp) != std::string::npos) && !out->empty())
            std::printf("  [vox] %s -> %zu model(s), pal[8]=(%3u,%3u,%3u)\n", path.c_str(),
                        out->size(), unsigned(out->front().pal[8][0]),
                        unsigned(out->front().pal[8][1]), unsigned(out->front().pal[8][2]));
    return true;
}

// The pieces of a file, uncomposed. `raw` is kept by the caller because the
// scene points into it -- see VoxScene.
inline bool voxLoadScene(const std::string &path, std::vector<uint8_t> *raw, VoxScene *out,
                         std::string *err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    raw->assign(size_t(n > 0 ? n : 0), 0);
    const size_t got = raw->empty() ? 0 : std::fread(raw->data(), 1, raw->size(), f);
    std::fclose(f);
    raw->resize(got);
    return voxParseScene(*raw, out, err);
}

inline bool voxLoad(const std::string &path, VoxModel *out, std::string *err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(size_t(n > 0 ? n : 0));
    const size_t got = raw.empty() ? 0 : std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    raw.resize(got);
    return voxParse(raw, out, err);
}

// One model's sub-box, converted to world layout and trimmed to its own bounds.
// This is rotation 0 of vox.rs's four quarter-turns: the model's z becomes the
// world's y, and its y becomes the world's z.
inline VoxAsset toWorld(const VoxModel &mo, int x0, int x1) {
    int minX = x1, maxX = x0 - 1, minY = mo.sy, maxY = -1, minZ = mo.sz, maxZ = -1;
    for (int z = 0; z < mo.sz; ++z)
        for (int y = 0; y < mo.sy; ++y)
            for (int x = x0; x < x1; ++x)
                if (mo.at(x, y, z)) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                    minZ = std::min(minZ, z);
                    maxZ = std::max(maxZ, z);
                }

    VoxAsset out;
    if (maxX < minX) return out;  // empty slab

    out.sx = maxX - minX + 1;
    out.sz = maxY - minY + 1;  // model y is world z
    out.sy = maxZ - minZ + 1;  // model z is world y (the height)
    out.a.assign(size_t(out.sx) * out.sy * out.sz, 0);

    for (int z = minZ; z <= maxZ; ++z)
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x) {
                const uint8_t v = mo.at(x, y, z);
                if (!v) continue;
                const int wx = x - minX, wz = y - minY, wy = z - minZ;
                out.a[size_t(wx) + size_t(wz) * out.sx + size_t(wy) * out.sx * out.sz] = v;
            }
    return out;
}

// ---------------------------------------------------------------------------
// The same conversion, WITHOUT the trim.
//
// toWorld() shrinks a model to its own occupied bounds, which is right for
// anything the scatter places -- a rock's collider and its footprint are
// measured off the asset, and padding an artist happened to leave would become
// invisible clearance around it.
//
// It is exactly wrong for a STRIP. The bow's seven draw frames are composed
// into one shared grid precisely so that swapping between them cannot shift the
// bow (see render/bow.h), and every held pose is measured from that grid's
// centre. Trim them and each frame gets its own box and its own centre: the
// bow lurches as it draws, and the arrow leaving on release moves it again.
// Measured, the seven came out 2x9x9, 2x9x10, 3x9x11 and so on -- seven
// different bows.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NOTHING IS HOLLOW.
//
// "Fill in the centers of trees with their corresponding bark voxels. the trees
// should not be hollow. you can fill in foilage with more green of the matching
// green."
//
// WHAT "HOLLOW" MEANS HERE IS PRECISE, AND IT HAD TO BE MEASURED BEFORE IT
// COULD BE FIXED. A pine's canopy is full of gaps and they are NOT cavities:
// they are the spaces between separate branches, open to the sky, and filling
// them would fuse a tree into a green cone. What is genuinely hollow is the air
// a model ENCLOSES -- the hole down the middle of a trunk, which on pine_1 is
// 158 voxels, half a per cent of the model.
//
// So the test is reachability, not "is it inside the bounding box": flood the
// empty space inward from the six faces of the model's own box, and anything
// empty the flood cannot reach is by definition sealed inside the wood.
//
// AND IT IS FILLED WITH WHAT SURROUNDS IT, not with a material chosen here.
// Each round, every cavity voxel touching something solid takes the commonest
// palette entry among those neighbours and becomes solid itself; repeat until
// none are left. A hole inside a trunk is walled in bark, so it fills with
// bark; one inside the crown is walled in needles, so it fills with that
// green. "Corresponding" falls out of the geometry rather than being a rule
// somebody has to keep in step with the art.
//
// IT IS INVISIBLE UNTIL SOMETHING CUTS THE MODEL, which is the point: a mesher
// throws away faces between two solid voxels either way, so this costs no
// triangles standing up. It shows the moment an axe takes a trunk in half.
// ---------------------------------------------------------------------------
inline int fillCavities(VoxAsset *a) {
    const int W = a->sx, H = a->sy, D = a->sz;
    if (W <= 2 || H <= 2 || D <= 2) return 0;
    const size_t n = size_t(W) * size_t(H) * size_t(D);
    auto id = [&](int x, int y, int z) {
        return size_t(x) + size_t(z) * size_t(W) + size_t(y) * size_t(W) * size_t(D);
    };

    // -- 1. what the outside air can reach ---------------------------------
    std::vector<uint8_t> seen(n, 0);
    std::vector<int> stack;
    stack.reserve(n / 8);
    auto push = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= W || y >= H || z >= D) return;
        const size_t k = id(x, y, z);
        if (seen[k] || a->a[k]) return;
        seen[k] = 1;
        stack.push_back(x);
        stack.push_back(y);
        stack.push_back(z);
    };
    for (int y = 0; y < H; ++y)
        for (int z = 0; z < D; ++z) { push(0, y, z); push(W - 1, y, z); }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) { push(x, y, 0); push(x, y, D - 1); }
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x) { push(x, 0, z); push(x, H - 1, z); }
    while (!stack.empty()) {
        const int z = stack.back(); stack.pop_back();
        const int y = stack.back(); stack.pop_back();
        const int x = stack.back(); stack.pop_back();
        push(x + 1, y, z); push(x - 1, y, z);
        push(x, y + 1, z); push(x, y - 1, z);
        push(x, y, z + 1); push(x, y, z - 1);
    }

    // -- 2. everything else that is empty is sealed in ---------------------
    std::vector<int> holes;
    for (int y = 0; y < H; ++y)
        for (int z = 0; z < D; ++z)
            for (int x = 0; x < W; ++x) {
                const size_t k = id(x, y, z);
                if (!a->a[k] && !seen[k]) { holes.push_back(x); holes.push_back(y); holes.push_back(z); }
            }
    const int filled = int(holes.size() / 3);
    if (!filled) return 0;

    // -- 3. ...and it takes the colour of its own walls ---------------------
    //
    // OUTSIDE IN, which is why this is a loop rather than one pass: the middle
    // of a thick cavity touches nothing solid on the first round, and would
    // otherwise have no wall to take a colour from.
    std::vector<int> left = holes;
    while (!left.empty()) {
        std::vector<int> again;
        bool any = false;
        for (size_t i = 0; i < left.size(); i += 3) {
            const int x = left[i], y = left[i + 1], z = left[i + 2];
            int tally[256] = {0};
            const int dx[6] = {1, -1, 0, 0, 0, 0};
            const int dy[6] = {0, 0, 1, -1, 0, 0};
            const int dz[6] = {0, 0, 0, 0, 1, -1};
            for (int e = 0; e < 6; ++e) {
                const int nx = x + dx[e], ny = y + dy[e], nz = z + dz[e];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= W || ny >= H || nz >= D) continue;
                const uint8_t v = a->a[id(nx, ny, nz)];
                if (v) ++tally[v];
            }
            int best = 0, bestN = 0;
            for (int e = 1; e < 256; ++e)
                if (tally[e] > bestN) { bestN = tally[e]; best = e; }
            if (!best) {
                again.push_back(x); again.push_back(y); again.push_back(z);
                continue;
            }
            a->a[id(x, y, z)] = uint8_t(best);
            any = true;
        }
        // A cavity with no solid neighbour at all cannot happen -- it was
        // unreachable from outside, so something walls it in -- but a guard
        // beats an infinite loop if the invariant ever stops holding.
        if (!any) break;
        left.swap(again);
    }
    return filled;
}

inline VoxAsset toWorldWhole(const VoxModel &mo) {
    VoxAsset out;
    out.sx = mo.sx;
    out.sz = mo.sy;  // model y is world z
    out.sy = mo.sz;  // model z is world y (the height)
    if (out.sx <= 0 || out.sy <= 0 || out.sz <= 0) return VoxAsset{};
    out.a.assign(size_t(out.sx) * size_t(out.sy) * size_t(out.sz), 0);
    for (int z = 0; z < mo.sz; ++z)
        for (int y = 0; y < mo.sy; ++y)
            for (int x = 0; x < mo.sx; ++x) {
                const uint8_t v = mo.at(x, y, z);
                if (!v) continue;
                out.a[size_t(x) + size_t(y) * size_t(out.sx) +
                      size_t(z) * size_t(out.sx) * size_t(out.sz)] = v;
            }
    return out;
}

// Several models authored side by side in one file, split on the empty columns
// between them. birch.vox holds four birch variants this way, so "load the
// birch" means picking one of these rather than stamping the whole row.
inline std::vector<VoxAsset> splitAlongX(const VoxModel &mo) {
    std::vector<bool> occupied(size_t(mo.sx), false);
    for (int x = 0; x < mo.sx; ++x)
        for (int z = 0; z < mo.sz && !occupied[size_t(x)]; ++z)
            for (int y = 0; y < mo.sy; ++y)
                if (mo.at(x, y, z)) {
                    occupied[size_t(x)] = true;
                    break;
                }

    std::vector<VoxAsset> out;
    int x = 0;
    while (x < mo.sx) {
        if (!occupied[size_t(x)]) {
            ++x;
            continue;
        }
        int end = x;
        while (end < mo.sx && occupied[size_t(end)]) ++end;
        VoxAsset a = toWorld(mo, x, end);
        if (a.sx > 0) out.push_back(std::move(a));
        x = end;
    }
    return out;
}

}  // namespace v2
