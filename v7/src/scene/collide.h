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

namespace v7 {

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
    // FIVE VOXELS, HALF A METRE, for both trees and rocks.
    //
    // A tree standing exactly on the surface looks like it is on tiptoe, and
    // one or two voxels was never quite enough on a slope -- the uphill side of
    // the trunk still showed daylight under it.
    //
    // The rocks used to be sunk in proportion to their own height, 1 + sy*0.18,
    // which put a five-metre boulder ten voxels down and a small one two. A
    // flat five is what was asked for and it is a real change at both ends:
    // small rocks sit deeper than they did, big ones shallower. If the boulders
    // start reading as set down on the ground rather than embedded in it, this
    // is the number.
    if (kind == 0) return 5;
    if (kind == 1) return 5;
    // Mushrooms grow out of the ground rather than sitting on it.
    if (kind == 3) return 3;
    // A pinecone's height is given to it by the branch it is sitting on, not by
    // the ground, so it must not be sunk at all -- see the yOff note in
    // makeInstance.
    if (kind == 4) return 0;
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
    // Landing on this throws you back up instead of stopping you.
    bool bouncy = false;

    // ---- the voxel-accurate surface --------------------------------------
    //
    // `col` is the model's column heightfield, BORROWED from the ModelTemplate
    // and not owned. That is safe only because the templates are built once at
    // load and never touched again -- if models ever became reloadable this
    // becomes a dangling pointer, and it would dangle silently.
    //
    // The rest is enough to invert the instance transform: `tx`/`tz` are its
    // translation, `yaw` its quarter turn, `baseY` the world height of the
    // model's own voxel row zero.
    const int16_t *col = nullptr;
    int16_t msx = 0, msz = 0;
    uint8_t yaw = 0;
    float tx = 0.0f, tz = 0.0f;
    float baseY = 0.0f;
};

// The world height of the model surface under (wx, wz), to the voxel.
//
// THE INSTANCE TRANSFORM, INVERTED. The placement rotates the model about its
// own centre by a quarter turn and translates it; this undoes exactly that, so
// the two cannot disagree about where a voxel ended up. A rotation's inverse is
// its transpose, which for the four cases is just the four sign patterns below
// read the other way round.
//
// False when the body is off the model's footprint or over an empty column --
// the caller then has nothing to stand on here, which is the right answer and
// is the whole difference from the old ellipse.
// `voxel` is passed in rather than reached for, the same way measureCollider
// takes it: this header is deliberately independent of voxelworld.h, and one
// include to save one argument is not worth giving that up.
inline bool solidColumnTop(const Solid &s, float wx, float wz, float voxel, float *outY) {
    if (!s.col) return false;
    // m0, m2, m6, m8 of kRot, in makeInstance.
    static const float R[4][4] = {
        { 1.0f,  0.0f,  0.0f,  1.0f},
        { 0.0f,  1.0f, -1.0f,  0.0f},
        {-1.0f,  0.0f,  0.0f, -1.0f},
        { 0.0f, -1.0f,  1.0f,  0.0f},
    };
    const float *r = R[s.yaw & 3];
    const float dx = wx - s.tx, dz = wz - s.tz;
    // Transposed: px uses m0 and m6, pz uses m2 and m8.
    const float px = r[0] * dx + r[2] * dz;
    const float pz = r[1] * dx + r[3] * dz;
    const int mx = int(floorf(px / voxel));
    const int mz = int(floorf(pz / voxel));
    if (mx < 0 || mz < 0 || mx >= int(s.msx) || mz >= int(s.msz)) return false;
    const int16_t h = s.col[size_t(mx) + size_t(mz) * size_t(s.msx)];
    if (h <= 0) return false;
    *outY = s.baseY + float(h) * voxel;
    return true;
}

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

}  // namespace v7
