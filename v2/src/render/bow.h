// ---------------------------------------------------------------------------
// bow.h -- the bow's draw, cut out of one file.
//
// A bow is not one model. bow/base.vox holds EIGHT pieces under two shape
// nodes: seven bow frames at depths 3, 5, 7, 5, 3, 2, 3, and one arrow. The
// draw is those frames played in order, and the release is the same frames
// played out the other side -- the string springs past its rest depth (that 2)
// and settles back. Composing the file the ordinary way lays all eight on top
// of one another, which is why scene/vox.h grew VoxScene: this needs the pieces
// and the node translations that place them.
//
// ---------------------------------------------------------------------------
// TWO ALIGNMENTS, AND BOTH ARE THE JS ENGINE'S
//
// THE FRONT IS FIXED. MagicaVoxel centres every frame on the shared node
// translation, so a 7-deep frame reaches two voxels further forward than the
// 3-deep rest pose: drawn, the face of the bow creeps away from you. The far
// edge is
//
//     t + ceil(d/2) - 1 + shift
//
// so it stands still when shift = ceil(d0/2) - ceil(d/2), which is what each
// frame's bow voxels are moved by. The JS engine's note records getting this
// wrong once, with (d0 - d): that is the STRING's travel, double the correction
// and applied in the wrong place, and it dragged the whole bow backwards.
//
// THE ARROW RIDES THE STRING. Its travel IS (d0 - d), a notch above the bow's
// centre line, and it is turned by ARROW_ROT about its own box centre and
// nudged by a per-frame offset -- both baked from that engine's arrow panel on
// 2026-08-04 and transcribed below.
//
// ---------------------------------------------------------------------------
// ONE GRID FOR ALL FOURTEEN
//
// Every frame is cut into the same box, and both strips -- nocked and bare --
// share it. Two things depend on that. The strips are interchangeable frame for
// frame, so the arrow leaving on release cannot shift the bow by a voxel; and
// the held pose is measured from the grid's CENTRE, so a frame that sized
// itself to its own contents would swing the bow about as it drew.
// ---------------------------------------------------------------------------
#pragma once

#include "../scene/vox.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace v2 {

// The arrow's orientation on the string, in quarter turns about x, y and z --
// user bake, 2026-08-04. Applied to the ARROW ALONE.
inline constexpr int kArrowRot[3] = {0, 2, 2};

// ONE OFFSET PER DRAW FRAME, same bake: across one voxel, and stepping further
// out as the string draws back. Seven, because the strip is seven.
//
// TWO VOXELS FURTHER ALONG THE SHAFT THAN THE ORIGINAL BAKE (user 2026-09-07),
// tuned in the settings panel and folded in here rather than left as a running
// offset -- which is what baking one means. The middle column of every row is
// the JS engine's number plus two; nothing else moved, and the pose the bow is
// held at is unchanged.
//
// FOLDED INTO ALL SEVEN and not only the three the draw shows, because the
// shared grid is sized over every frame's placed arrow. Baking it into a
// subset would size a different box for the same picture, and the box is what
// the held pose is measured from.
inline constexpr int kArrowPos[7][3] = {{1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {0, 2, 0},
                                        {0, 2, 0}, {0, 2, 0}, {0, 2, 0}};

// ---------------------------------------------------------------------------
// AND ONE MORE, THE SAME FOR EVERY FRAME, WHICH IS THE PART YOU CAN TUNE.
//
// The table above is a RELATIONSHIP -- the arrow stepping back as the string
// draws -- and it is right. What is easy to be wrong about is where the whole
// thing sits: an arrow half a voxel off the string reads as wrong from the
// first frame and stays wrong through all seven. So the adjustment is one
// offset added to every frame, which moves the arrow without disturbing the
// draw the table describes.
//
// IN WHOLE VOXELS, and it could not be anything else: the arrow is stamped into
// a voxel grid, so a fractional offset has nowhere to land. At 10 cm a voxel,
// one step is one voxel.
//
// THE BOW DOES NOT MOVE WITH IT, and that falls out of how the grid is sized
// rather than from anything here. The centre c2 is fixed by the bow and the
// UNPLACED arrow, in the first sizing pass; the placed arrow only grows the box
// symmetrically about that centre in the second. The held pose is measured from
// the centre, so the bow stands still however far the arrow is nudged -- which
// is exactly what makes this tunable while you look at it.
// ---------------------------------------------------------------------------
struct ArrowOffset {
    int across = 0;  // the file's x
    int along = 0;   // its y, down the shaft -- the direction the string draws
    int up = 0;      // its z
};

// A composed strip: the same frames with the arrow on the string and without
// it, in one shared grid.
struct BowStrip {
    std::vector<VoxModel> withArrow;  // nocked -- what you draw
    std::vector<VoxModel> bowOnly;    // loosed -- the arrow has gone
    int frames = 0;
    bool ok() const { return frames > 0; }
};

namespace detail {

// A voxel on its way through the composer, in file space.
struct BowVox {
    long long x, y, z;
    uint8_t c;
};

// Right-handed quarter turns in the .vox frame: x across, y along the shaft,
// z up. The JS engine's arrowRotQ, turn for turn.
inline void bowRotQ(long long *x, long long *y, long long *z, const int *r) {
    for (int i = 0; i < r[0]; ++i) { const long long t = *y; *y = -*z; *z = t; }
    for (int i = 0; i < r[1]; ++i) { const long long t = *z; *z = -*x; *x = t; }
    for (int i = 0; i < r[2]; ++i) { const long long t = *x; *x = -*y; *y = t; }
}

inline void bowBox(const std::vector<BowVox> &v, long long *lo, long long *hi) {
    lo[0] = lo[1] = lo[2] = 1LL << 40;
    hi[0] = hi[1] = hi[2] = -(1LL << 40);
    for (const BowVox &p : v) {
        const long long q[3] = {p.x, p.y, p.z};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], q[k]);
            hi[k] = std::max(hi[k], q[k]);
        }
    }
}

// Turn a set of voxels about its OWN box centre. DOUBLED COORDINATES, so a
// quarter turn stays exact whichever way the box's parity falls -- rounding
// only ever splits a genuine half voxel.
inline std::vector<BowVox> bowSpin(const std::vector<BowVox> &v, const int *rot) {
    if (v.empty()) return v;
    long long lo[3], hi[3];
    bowBox(v, lo, hi);
    const long long c2[3] = {lo[0] + hi[0], lo[1] + hi[1], lo[2] + hi[2]};
    std::vector<BowVox> out;
    out.reserve(v.size());
    for (const BowVox &p : v) {
        long long x = 2 * p.x - c2[0], y = 2 * p.y - c2[1], z = 2 * p.z - c2[2];
        bowRotQ(&x, &y, &z, rot);
        // llround rather than a truncating divide: the doubled coordinate is
        // odd exactly when the box has even extent on that axis, and rounding
        // toward zero would pull those voxels a half step toward the centre.
        out.push_back({std::llround(double(x + c2[0]) * 0.5),
                       std::llround(double(y + c2[1]) * 0.5),
                       std::llround(double(z + c2[2]) * 0.5), p.c});
    }
    return out;
}

// One piece's voxels in file space, with a per-frame shift.
inline void bowPlace(const VoxScene &sc, size_t pi, const long long *sft,
                     std::vector<BowVox> *out) {
    const VoxScene::Piece &p = sc.pieces[pi];
    for (size_t q = 0; q + 4 <= p.voxelBytes; q += 4) {
        const int x = p.voxels[q], y = p.voxels[q + 1], z = p.voxels[q + 2];
        if (x >= p.sx || y >= p.sy || z >= p.sz) continue;
        out->push_back({p.ox + x + sft[0], p.oy + y + sft[1], p.oz + z + sft[2],
                        p.voxels[q + 3]});
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Cut the strip. Returns an empty BowStrip if the file will not give one, which
// the caller treats as "no bow" rather than as a failure to start.
// ---------------------------------------------------------------------------
inline BowStrip parseBowStrip(const std::string &path, std::string *err,
                             const ArrowOffset &nudge = ArrowOffset{}) {
    using detail::BowVox;

    BowStrip out;
    std::vector<uint8_t> raw;
    VoxScene sc;
    if (!voxLoadScene(path, &raw, &sc, err)) return out;

    // -- which shape is the bow, and which the arrow ------------------------
    //
    // THE ONE WITH THE MOST PIECES IS THE BOW, which is the JS engine's rule
    // and is true by construction: the draw is a frame per piece and the arrow
    // is a single model. Asked by SIZE instead it would be a guess about which
    // of two boxes looks more like a bow.
    std::vector<int> shapeIds;
    for (const VoxScene::Piece &p : sc.pieces)
        if (std::find(shapeIds.begin(), shapeIds.end(), p.shape) == shapeIds.end())
            shapeIds.push_back(p.shape);
    if (shapeIds.size() < 1) {
        if (err) *err = "no shapes in " + path;
        return out;
    }

    auto piecesOf = [&](int shape) {
        std::vector<size_t> v;
        for (size_t i = 0; i < sc.pieces.size(); ++i)
            if (sc.pieces[i].shape == shape) v.push_back(i);
        return v;
    };

    int bowShape = shapeIds[0];
    for (int id : shapeIds)
        if (piecesOf(id).size() > piecesOf(bowShape).size()) bowShape = id;
    int arrowShape = -1;
    for (int id : shapeIds)
        if (id != bowShape && !piecesOf(id).empty()) { arrowShape = id; break; }

    const std::vector<size_t> bow = piecesOf(bowShape);
    const std::vector<size_t> arrow = arrowShape >= 0 ? piecesOf(arrowShape) : std::vector<size_t>();
    if (bow.empty()) {
        if (err) *err = "no bow frames in " + path;
        return out;
    }

    // The bow's own depth per frame. THE DRAW IS THIS NUMBER GROWING, and it
    // drives both alignments.
    std::vector<long long> depth;
    for (size_t i : bow) depth.push_back(sc.pieces[i].sy);
    const long long d0 = depth[0];
    const int n = int(bow.size());

    auto ceilHalf = [](long long v) { return (v + 1) / 2; };

    // RESIZED RATHER THAN SIZED IN THE DECLARATION. `bv(size_t(n))` is a
    // function declaration to a C++ parser -- the most vexing parse -- and it
    // fails a dozen lines later complaining that a vector cannot be
    // subscripted, which is true of the function it actually declared.
    std::vector<std::vector<BowVox>> bv, av;
    bv.resize(size_t(n));
    av.resize(size_t(n));
    for (int f = 0; f < n; ++f) {
        const long long bs[3] = {0, ceilHalf(d0) - ceilHalf(depth[size_t(f)]), 0};
        detail::bowPlace(sc, bow[size_t(f)], bs, &bv[size_t(f)]);
        const long long as[3] = {0, d0 - depth[size_t(f)], 1};
        for (size_t a : arrow) detail::bowPlace(sc, a, as, &av[size_t(f)]);
    }

    // -- the grid ----------------------------------------------------------
    //
    // Sized once, over every frame of both strips, and CENTRED where the
    // unturned strip's own box already was -- see the header for why the centre
    // is the part that matters.
    long long lo[3] = {1LL << 40, 1LL << 40, 1LL << 40};
    long long hi[3] = {-(1LL << 40), -(1LL << 40), -(1LL << 40)};
    auto grow = [&](const std::vector<BowVox> &v) {
        if (v.empty()) return;
        long long a[3], b[3];
        detail::bowBox(v, a, b);
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], a[k]);
            hi[k] = std::max(hi[k], b[k]);
        }
    };
    for (int f = 0; f < n; ++f) {
        grow(bv[size_t(f)]);
        grow(detail::bowSpin(av[size_t(f)], kArrowRot));
    }
    const long long c2[3] = {lo[0] + hi[0], lo[1] + hi[1], lo[2] + hi[2]};

    // The arrow as it is actually placed -- turned, then nudged by this frame's
    // baked offset -- which is what has to fit.
    std::vector<std::vector<BowVox>> placedArrow;
    placedArrow.resize(size_t(n));
    for (int f = 0; f < n; ++f) {
        const int *p = kArrowPos[size_t(f < 7 ? f : 6)];
        for (const BowVox &v : detail::bowSpin(av[size_t(f)], kArrowRot))
            placedArrow[size_t(f)].push_back({v.x + p[0] + nudge.across, v.y + p[1] + nudge.along,
                                              v.z + p[2] + nudge.up, v.c});
        grow(placedArrow[size_t(f)]);
    }

    // Re-centred on c2, so the pose measured against the strip's middle in the
    // engine this came from still means the same point here.
    for (int k = 0; k < 3; ++k) {
        const long long h = std::max(c2[k] - 2 * lo[k], 2 * hi[k] - c2[k]);
        lo[k] = (long long)std::floor(double(c2[k] - h) * 0.5);
        hi[k] = (long long)std::ceil(double(c2[k] + h) * 0.5);
    }
    const int gx = int(hi[0] - lo[0] + 1), gy = int(hi[1] - lo[1] + 1),
              gz = int(hi[2] - lo[2] + 1);
    if (gx <= 0 || gy <= 0 || gz <= 0) {
        if (err) *err = "bow strip has no extent";
        return out;
    }

    auto cut = [&](const std::vector<BowVox> &b, const std::vector<BowVox> &a) {
        VoxModel m;
        m.sx = gx;
        m.sy = gy;
        m.sz = gz;
        m.pal = sc.pal;
        m.m.assign(size_t(gx) * size_t(gy) * size_t(gz), 0);
        auto put = [&](const std::vector<BowVox> &v) {
            for (const BowVox &p : v) {
                const long long x = p.x - lo[0], y = p.y - lo[1], z = p.z - lo[2];
                if (x < 0 || y < 0 || z < 0 || x >= gx || y >= gy || z >= gz) continue;
                m.m[size_t(x) + size_t(y) * size_t(gx) + size_t(z) * size_t(gx) * size_t(gy)] =
                    p.c;
            }
        };
        put(b);
        put(a);
        return m;
    };

    for (int f = 0; f < n; ++f) {
        out.withArrow.push_back(cut(bv[size_t(f)], placedArrow[size_t(f)]));
        out.bowOnly.push_back(cut(bv[size_t(f)], {}));
    }
    out.frames = n;
    return out;
}

}  // namespace v2
