// ---------------------------------------------------------------------------
// collide.h -- the things in the wood a body can walk into.
//
// The terrain needs nothing here. It is a height field and a pure function of
// position, so the player queries it directly and the answer does not depend on
// what the GPU has resident. Trees and rocks are not like that: they are
// instanced models, and the only place a particular tree's position exists at
// all is the instance transform the renderer builds for it. So a collider has
// to be made ALONGSIDE that transform, out of the same numbers -- see
// makeInstance in scene_gpu.h, which fills one as it writes the other. Two
// independent placements would agree until the first time a rule changed, and
// then the trunk would be somewhere the tree was not, which is the kind of bug
// you cannot see in a screenshot.
//
// WHAT A COLLIDER IS: an upright elliptic cylinder -- a centre, two half
// extents and a top. Not the model's triangles, and not a ray into the
// acceleration structure either. Both of those are exact and both would tie
// where you can walk to what happens to be built on the GPU this frame, which
// is the one property the ground query was written to avoid.
//
// The ellipse rather than the box it is inscribed in, because a boulder is
// round and so is a trunk. On a five-metre rock the box's corner is a metre of
// invisible wall, and invisible walls in a wood are the thing players notice.
//
// WHY THE COLLIDER IS MEASURED FROM THE VOXELS. A pine model is 22 metres tall
// and its canopy starts NINE metres up; the trunk under it is 40 to 80 cm
// across depending on which of the nine it is, and in several of them the trunk
// is not centred in the model. One guessed radius would be wrong twice over --
// too fat for the thin ones, and off-centre for most. measureCollider() takes
// the model's extent in the first two metres above its base, which is the only
// part of it a walking body can ever reach: that is the trunk for a pine and
// the cross-section at body height for a rock, with no per-model table to keep
// in step with the assets.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"
#include "vox.h"

namespace v6 {

// How tall a body is, in metres -- Player::eye plus a little, from player.h.
// The collider is measured over this much of the model and no more: a pine's
// branches above it are not something a walking body can be stopped by.
constexpr float kBodyHeightM = 2.0f;

// How much of a model counts as its BASE -- the part that has to meet the
// ground. A rock is set into the terrain by a few voxels, so what matters for
// whether it sits or hangs is its width down there, not its widest point.
constexpr float kBaseHeightM = 0.6f;

// A collider in the model's own frame, in metres, measured from the centre of
// the model's footprint -- which is the point a placement sets on the column.
struct ModelCollider {
    float cx = 0.0f, cz = 0.0f;  // offset of the collider from that centre
    float hx = 0.0f, hz = 0.0f;  // half extents
    float top = 0.0f;            // the highest voxel, above the model's base
    // The footprint where the model meets the ground, in VOXELS, measured over
    // kBaseHeightM. Not the bounding box: a boulder is usually widest around
    // its middle, and the middle is not what has to be supported.
    int baseX = 0, baseZ = 0;
    bool solid() const { return hx > 0.0f && hz > 0.0f; }
};

// ---------------------------------------------------------------------------
// HOW DEEP A PLACED MODEL IS SET INTO THE GROUND, in voxels.
//
// ONE definition, because two things need it and they have to agree: the
// scatter uses it to decide whether the ground under a rock is flat enough to
// hide its base, and makeInstance uses it to actually put the rock there. Two
// copies of this rule would agree until the first time one of them changed.
// ---------------------------------------------------------------------------
inline int decorSink(int kind, int sy, uint32_t seed, uint32_t cell) {
    // A tree standing exactly on the surface looks like it is on tiptoe.
    if (kind == 0) return 1 + int(hashUnit(seed + 17u, cell) * 2.0f);
    // A rock is sunk in proportion to its own height, which is what makes a
    // boulder read as embedded in the ground rather than set down on it.
    if (kind == 1) return 1 + int(float(sy) * 0.18f);
    return 1;
}

// One solid thing, placed in the world.
struct Solid {
    float cx = 0.0f, cz = 0.0f;  // centre of the footprint, world metres
    // Half extents; the footprint is the ellipse inscribed in them. Zero by
    // default and never pushed that way -- a Solid that was never filled in
    // must not read as a one-metre block sitting at the world origin.
    float hx = 0.0f, hz = 0.0f;
    float top = 0.0f;            // world y of the top
    // A rock is standable -- you can be blocked by one, step up onto a small
    // one, or land on it. A trunk is not: its top is a canopy twenty metres up
    // and nothing should ever be put there.
    bool standable = false;
};

// Does a body of half-width w, centred at (x, z), touch the footprint?
//
// Exact rather than approximate. Dividing through by the half extents maps the
// ellipse to a unit circle and the body's square to a rectangle, and the
// closest-point distance from a rectangle to the origin is the same expression
// in either space -- so this is a box-circle test, which has an exact form,
// wearing an ellipse's clothes.
inline bool touches(const Solid &s, float x, float z, float w) {
    const float dx = maxf(0.0f, fabsf(x - s.cx) - w) / s.hx;
    const float dz = maxf(0.0f, fabsf(z - s.cz) - w) / s.hz;
    return dx * dx + dz * dz < 1.0f;
}

// -----------------------------------------------------------------------
// The widest extent of a model in the first `bodyM` metres above its base,
// plus its full height.
//
// The height needs no search: toWorld() has already trimmed the asset to its
// own bounding box, so the top voxel is the top layer.
// -----------------------------------------------------------------------
inline ModelCollider measureCollider(const VoxAsset &a, float voxelM, float bodyM) {
    ModelCollider c;
    if (a.sx <= 0 || a.sy <= 0 || a.sz <= 0) return c;
    c.top = float(a.sy) * voxelM;

    // The footprint at the base, in voxels. Same sweep as below over a much
    // shallower slab -- see kBaseHeightM.
    {
        const int base = mini(a.sy, maxi(1, int(kBaseHeightM / voxelM + 0.5f)));
        int bx0 = a.sx, bx1 = -1, bz0 = a.sz, bz1 = -1;
        for (int y = 0; y < base; ++y)
            for (int z = 0; z < a.sz; ++z)
                for (int x = 0; x < a.sx; ++x)
                    if (a.at(x, y, z)) {
                        bx0 = mini(bx0, x);
                        bx1 = maxi(bx1, x);
                        bz0 = mini(bz0, z);
                        bz1 = maxi(bz1, z);
                    }
        if (bx1 >= 0) {
            c.baseX = bx1 - bx0 + 1;
            c.baseZ = bz1 - bz0 + 1;
        }
    }

    const int slab = mini(a.sy, maxi(1, int(bodyM / voxelM + 0.5f)));
    int x0 = a.sx, x1 = -1, z0 = a.sz, z1 = -1;
    for (int y = 0; y < slab; ++y)
        for (int z = 0; z < a.sz; ++z)
            for (int x = 0; x < a.sx; ++x)
                if (a.at(x, y, z)) {
                    x0 = mini(x0, x);
                    x1 = maxi(x1, x);
                    z0 = mini(z0, z);
                    z1 = maxi(z1, z);
                }
    if (x1 < 0) return c;  // nothing at all at body height: not solid

    c.hx = float(x1 - x0 + 1) * voxelM * 0.5f;
    c.hz = float(z1 - z0 + 1) * voxelM * 0.5f;
    c.cx = (float(x0 + x1 + 1) * 0.5f - float(a.sx) * 0.5f) * voxelM;
    c.cz = (float(z0 + z1 + 1) * 0.5f - float(a.sz) * 0.5f) * voxelM;
    return c;
}

}  // namespace v6
