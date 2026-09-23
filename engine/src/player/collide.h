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

#include "core/vecmath.h"
#include "voxel/vox.h"

namespace v2 {

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
    // AND WHERE THAT FOOTPRINT SITS, in metres from the model's own centre.
    //
    // A SIZE WITHOUT AN OFFSET IS ONLY HALF AN ANSWER, and for a tree it is the
    // wrong half. A birch is a trunk with a crown leaning off it: the bounding
    // box is centred on the CROWN, and the trunk can stand five metres from the
    // middle of it (birch 7 and 10 are at 4.8 and 4.95 m). Anything that spaced
    // itself against the box centre was therefore protecting an empty patch of
    // air and leaving the trunk itself open -- see collectTrees in
    // scene/chunks.h, which is where that showed up as a mushroom growing out
    // of a birch. Free to measure: the sweep above already has the bounds.
    float baseCX = 0.0f, baseCZ = 0.0f;
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
// WHICH ROCKS ARE "BIG AND MID", as a range over the model list.
//
// loadRocks() in gpu/world.h loads the twenty-six rocks from a FIXED, NAMED
// list, in size order: five BIG, six Mid, seven Runic, eight Small. So the big
// and mid boulders are exactly the first eleven models, and this is the single
// place that turns that ordering into a rule.
//
// AN INDEX RANGE AND NOT A HEIGHT TEST, on purpose. Classifying by sy would
// look tidier and would quietly reclassify a squat Big as a Mid, or a tall
// Runic as a Big, the first time one of the assets was re-authored. The names
// are what the artist decided; the order is what the loader promises. If a rock
// is ever inserted into that list, this number moves with it -- which is why
// it names the boundary rather than counting.
constexpr int kRockBigEnd = 5;      // one past Big_5_BiG_0, where Mid_1 begins
constexpr int kRockBigMidEnd = 11;  // one past Mid_5_MID_0, where Runic_1 begins

inline int decorSink(int kind, int model, int sy, uint32_t seed, uint32_t cell) {
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
    if (kind == 1) {
        // THE SINK SCALES WITH THE STONE, so the three classes are three
        // numbers rather than one.
        //
        // A sink is how much of the flat underside the .vox models are
        // modelled with has to disappear into the ground before the rock reads
        // as sitting in the terrain instead of on it. That is a FRACTION of the
        // model, so it has to move when the model does -- and both large
        // classes were rescaled in loadRocks (big 4x, mid 2x on a side).
        //
        //     big   128-228 voxels tall   sink 40  (4.0 m)
        //     mid    56- 76               sink 20  (2.0 m)
        //     rest    8- 24               sink  5  (0.5 m)
        //
        // Nothing is swallowed by any of them: the shortest big is 128 and
        // keeps 88 above ground, the shortest mid is 56 and keeps 36.
        //
        // THIS IS ONLY THE FLOOR. What actually decides how deep a given stone
        // sits is groundDrop in scene/chunks.h, which measures the ground under
        // that particular footprint and deepens this number until no part of
        // the base is above the terrain. This is what a rock on FLAT ground
        // gets, and the minimum anything gets.
        if (model >= 0 && model < kRockBigEnd) return 40;
        if (model < kRockBigMidEnd) return 20;
        return 5;
    }
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

    // -- THIS MODEL HAS ROOMS IN IT ----------------------------------------
    //
    // Everything else in the wood is a FLOOR or a WALL and the two are
    // exclusive: blocked() skips every standable solid outright, because a rock
    // you walk into is meant to put you on top of itself and the step-up in
    // moveAxis is what does that. A BUILDING is both at once -- you stand on
    // its floors and you are stopped by its walls -- and it is the first thing
    // here that is, which is why this is a flag and not a shape.
    //
    // It changes two answers and nothing else. The floor under you becomes the
    // highest solid voxel AT OR BELOW the feet rather than the top of the
    // column (see solidColumnTopBelow), or standing in the lobby would put you
    // on the roof; and the body's box is tested against the voxels from the
    // step-up height to the top of its head, which is the ordinary voxel rule
    // and the reason a doorway is a doorway and a wall is not.
    //
    // NOTHING IN THE WOOD SETS IT. A tree, a rock, a mushroom and a hive go on
    // being exactly what they were.
    bool interior = false;

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

    // ...AND THE VOXELS THEMSELVES.
    //
    // A column heightfield answers "how high is the stone here", which is what
    // a FLOOR needs and all it needs. It cannot answer "is there stone HERE",
    // which is what a blow, an arrow and a body all need -- and answering that
    // with the ellipse below is what made a pick miss the top of a big
    // boulder. Same borrowing rule as `col`: it points at the ModelTemplate,
    // which lives for the run, and is re-pointed at a damaged instance's own
    // copy the moment one exists, so a hole knocked in a rock is a hole to
    // everything that asks.
    //
    // VoxAsset (world) layout: x + z*msx + y*msx*msz. Height is vsy; the two
    // horizontal extents are msx and msz above, shared with `col`.
    const uint8_t *vol = nullptr;
    int16_t vsy = 0;

    // ---- WHICH INSTANCE THIS IS ------------------------------------------
    //
    // A blow lands on a Solid, but breaking one means editing the INSTANCE it
    // stands for -- giving that one boulder a private copy of its model while
    // the other twenty-four placements of the same rock keep sharing the
    // original. These four fields are what turns a hit back into that
    // instance: which chunk holds it, which decor slot inside that chunk, and
    // which template it was stamped from.
    //
    // decorSlot is -1 for anything that is not a decor instance.
    long long ownerChunk = 0;
    int32_t decorSlot = -1;
    int16_t modelKind = -1, modelIndex = -1;
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
// A world point in the model's own frame, in metres from its (0,0) voxel
// corner. Shared by the two tests below so they cannot disagree about where
// the model is.
// ...and back again: a point in the model's own frame, in world metres. The
// same four numbers, transposed -- a quarter turn is orthonormal, so its
// inverse IS its transpose. Kept beside solidModelSpace because a forward and
// an inverse that live apart drift apart, and the symptom is a chunk that
// breaks off somewhere the hole is not.
inline void solidWorldSpace(const Solid &s, float px, float pz, float *wx, float *wz) {
    static const float R[4][4] = {
        { 1.0f,  0.0f,  0.0f,  1.0f},
        { 0.0f,  1.0f, -1.0f,  0.0f},
        {-1.0f,  0.0f,  0.0f, -1.0f},
        { 0.0f, -1.0f,  1.0f,  0.0f},
    };
    const float *r = R[s.yaw & 3];
    *wx = r[0] * px + r[1] * pz + s.tx;
    *wz = r[2] * px + r[3] * pz + s.tz;
}

// A DIRECTION in the model's frame -- the same quarter turn as
// solidModelSpace, without the translation. A vector is not a point, and
// putting a ray's direction through the point transform is the kind of mistake
// that only shows up on the rotated placements.
inline void solidModelDir(const Solid &s, float wx, float wz, float *px, float *pz) {
    static const float R[4][4] = {
        { 1.0f,  0.0f,  0.0f,  1.0f},
        { 0.0f,  1.0f, -1.0f,  0.0f},
        {-1.0f,  0.0f,  0.0f, -1.0f},
        { 0.0f, -1.0f,  1.0f,  0.0f},
    };
    const float *r = R[s.yaw & 3];
    *px = r[0] * wx + r[2] * wz;
    *pz = r[1] * wx + r[3] * wz;
}

inline void solidModelSpace(const Solid &s, float wx, float wz, float *px, float *pz) {
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
    *px = r[0] * dx + r[2] * dz;
    *pz = r[1] * dx + r[3] * dz;
}

// ---------------------------------------------------------------------------
// THE MODEL'S OWN VOXELS, WHICH ARE THE SHAPE.
//
// Everything below this line exists because the ellipse above is not the rock.
// measureCollider takes the widest cross-section of the first two metres above
// a model's base and calls that its footprint, which is a reasonable wall for a
// body to walk into and is not, in any sense, the boulder. It cannot represent
// a dome, an overhang, a notch, or the hole a pick just made -- and a blow
// tested against it lands where the stone is not, or misses where it is.
//
// The worst case is the one that gets reported: a body standing ON a big rock
// is INSIDE that ellipse, so the near root of the ray-cylinder quadratic is
// behind the eye and the far root is out the other side of a twenty-metre
// boulder -- past the reach of any tool. The swing then finds no model at all
// and falls through to the terrain, which is "hits do not register on top of
// the big rocks". No amount of widening or shrinking an ellipse fixes that; it
// is the wrong kind of shape.
//
// So these ask the voxels. They are the same three questions the terrain
// already answers exactly -- is this point solid, does this box overlap solid,
// what does this ray hit first -- asked of a model's grid instead of the
// world's, and they hug the model however it is shaped because they ARE the
// model.
// ---------------------------------------------------------------------------

// One voxel of the model, in the model's own grid. False off the grid.
inline bool solidVoxel(const Solid &s, int mx, int my, int mz) {
    if (!s.vol || mx < 0 || my < 0 || mz < 0 || mx >= int(s.msx) || my >= int(s.vsy) ||
        mz >= int(s.msz))
        return false;
    return s.vol[size_t(mx) + size_t(mz) * size_t(s.msx) +
                 size_t(my) * size_t(s.msx) * size_t(s.msz)] != 0;
}

// Is this world point inside solid stone (or wood) of this model?
inline bool solidAtWorld(const Solid &s, float wx, float wy, float wz, float voxel) {
    if (!s.vol) return false;
    float px = 0.0f, pz = 0.0f;
    solidModelSpace(s, wx, wz, &px, &pz);
    return solidVoxel(s, int(floorf(px / voxel)), int(floorf((wy - s.baseY) / voxel)),
                      int(floorf(pz / voxel)));
}

// Does an upright box -- half-width w about (x, z), from feetY to headY --
// contain any solid voxel of this model?
//
// A quarter turn maps a world-axis-aligned box to a model-axis-aligned box, so
// this is a plain range over the grid and not a rotated-box test. The cheap
// rejections come first because this is asked of every nearby model on every
// step, and only the one or two a body is actually near should pay for the
// range.
// AND THE SAME FOR A BODY THAT IS NOT SQUARE. A marcher is 0.5 m across and
// 1.4 m long and calling that a square is wrong in both directions at once --
// too fat to walk a gap it fits through, too thin along the nose to notice what
// it is walking into. The quarter turn means a world box is a model box either
// way round, so this is still a plain range and not a rotated test.
inline bool solidBoxOverlap(const Solid &s, float x, float feetY, float z, float headY, float wx,
                            float wz, float voxel);

inline bool solidBoxOverlap(const Solid &s, float x, float feetY, float z, float headY, float w,
                            float voxel) {
    return solidBoxOverlap(s, x, feetY, z, headY, w, w, voxel);
}

inline bool solidBoxOverlap(const Solid &s, float x, float feetY, float z, float headY, float wx,
                            float wz, float voxel) {
    if (!s.vol) return false;
    if (headY < s.baseY || feetY > s.top) return false;
    float px = 0.0f, pz = 0.0f;
    solidModelSpace(s, x, z, &px, &pz);
    // THE HALF EXTENTS TURN WITH THE MODEL. yaw 1 and 3 are quarter turns, so
    // what was the body's x extent is its z extent in the model's frame --
    // getting this wrong is invisible on three quarters of the placements.
    const float w = ((s.yaw & 1) ? wz : wx), h = ((s.yaw & 1) ? wx : wz);
    const float ex = float(s.msx) * voxel, ez = float(s.msz) * voxel;
    if (px + w < 0.0f || pz + h < 0.0f || px - w > ex || pz - h > ez) return false;

    const int x0 = maxi(0, int(floorf((px - w) / voxel)));
    const int x1 = mini(int(s.msx) - 1, int(floorf((px + w) / voxel)));
    const int z0 = maxi(0, int(floorf((pz - h) / voxel)));
    const int z1 = mini(int(s.msz) - 1, int(floorf((pz + h) / voxel)));
    const int y0 = maxi(0, int(floorf((feetY - s.baseY) / voxel)));
    const int y1 = mini(int(s.vsy) - 1, int(floorf((headY - s.baseY) / voxel)));
    for (int my = y0; my <= y1; ++my)
        for (int mz = z0; mz <= z1; ++mz)
            for (int mx = x0; mx <= x1; ++mx)
                if (s.vol[size_t(mx) + size_t(mz) * size_t(s.msx) +
                          size_t(my) * size_t(s.msx) * size_t(s.msz)] != 0)
                    return true;
    return false;
}

// ---------------------------------------------------------------------------
// THE FIRST SOLID VOXEL A RAY MEETS, in metres from the eye.
//
// Amanatides and Woo through the model's own grid -- the same march the terrain
// gets in swingRay, and for the same reason its note gives: a fixed-step sample
// can skip a voxel or land in one twice, and a blow that misses one time in
// twenty is indistinguishable from a blow that is broken.
//
// CLIPPED TO THE MODEL'S BOX FIRST. Without that a ray beginning fifty metres
// off would walk five hundred empty cells to reach the rock, per rock, per
// swing. With it, a model the ray does not pass through costs six divisions and
// nothing else, and one it does costs only the cells inside it.
//
// The origin is carried in VOXELS and the direction in voxels-per-metre, so `t`
// stays in world metres throughout: the caller's reach needs no conversion and
// the answer can be compared directly against a terrain hit.
// ---------------------------------------------------------------------------
// `voxOut`, when given, receives the model voxel that was hit -- three ints,
// x/y/z in the model's own grid. The carve wants that rather than the distance:
// re-deriving it from a point on a face is a rounding away from the voxel in
// front or the one behind, and it is free here because the march already knows.
//
// IT TAKES A GRID AND NOT A MODEL, and that is not tidying. A SECOND kind of
// thing needs this march and cannot be described as a Solid: a felled tree is
// no longer a placed instance but a rigid body wearing whatever rotation the
// solver gave it, and a Solid carries a quarter turn and nothing else. An axe
// that could not hit one was the whole of "the player is unable to interact
// with that felled object". One march, two callers, so a standing tree and the
// one lying beside it cannot come to disagree about where their voxels are.
// ---------------------------------------------------------------------------
inline bool rayVoxelGrid(const uint8_t *vol, const int n[3], const float o[3], const float d[3],
                         float tMax, float *tHit, int *voxOut = nullptr) {
    if (!vol || n[0] <= 0 || n[1] <= 0 || n[2] <= 0 || tMax <= 0.0f) return false;

    // ---- the box, so the march starts at the model and not at the eye ------
    float t0 = 0.0f, t1 = tMax;
    for (int k = 0; k < 3; ++k) {
        if (fabsf(d[k]) < 1e-9f) {
            if (o[k] < 0.0f || o[k] >= float(n[k])) return false;
            continue;
        }
        float a = (0.0f - o[k]) / d[k];
        float b = (float(n[k]) - o[k]) / d[k];
        if (a > b) { const float sw = a; a = b; b = sw; }
        t0 = maxf(t0, a);
        t1 = minf(t1, b);
        if (t0 > t1) return false;
    }

    // A hair past the face, so the entry cell is the one the ray is in rather
    // than whichever one the boundary rounds to.
    float t = t0 + 1e-4f;
    if (t > t1) t = t0;

    int v[3], step[3];
    float tNext[3], tDelta[3];
    const float kFar = 1e30f;
    for (int k = 0; k < 3; ++k) {
        const float p = o[k] + d[k] * t;
        v[k] = int(floorf(p));
        if (v[k] < 0) v[k] = 0;
        if (v[k] >= n[k]) v[k] = n[k] - 1;
        if (fabsf(d[k]) < 1e-9f) {
            step[k] = 0;
            tDelta[k] = kFar;
            tNext[k] = kFar;
        } else {
            step[k] = d[k] > 0.0f ? 1 : -1;
            tDelta[k] = 1.0f / fabsf(d[k]);
            const float bound = (step[k] > 0) ? float(v[k] + 1) : float(v[k]);
            tNext[k] = t + (bound - p) / d[k];
        }
    }

    // The guard is a backstop, not the exit: t1 is the far face of the box, so
    // the march ends at the model however long the reach is.
    for (int guard = 0; guard < 8192; ++guard) {
        if (t > t1) return false;
        if (vol[size_t(v[0]) + size_t(v[2]) * size_t(n[0]) +
                size_t(v[1]) * size_t(n[0]) * size_t(n[2])] != 0) {
            *tHit = maxf(0.0f, t);
            if (voxOut) { voxOut[0] = v[0]; voxOut[1] = v[1]; voxOut[2] = v[2]; }
            return true;
        }
        const int k = (tNext[0] < tNext[1]) ? ((tNext[0] < tNext[2]) ? 0 : 2)
                                            : ((tNext[1] < tNext[2]) ? 1 : 2);
        t = tNext[k];
        v[k] += step[k];
        if (v[k] < 0 || v[k] >= n[k]) return false;
        tNext[k] += tDelta[k];
    }
    return false;
}

// ...AND THE MODEL'S OWN GRID, WHICH IS THAT MARCH AFTER A CHANGE OF FRAME.
// A placement is a translation and a quarter turn, so the ray is turned into
// the model's axes once and the march does the rest.
inline bool rayModelVoxels(const Solid &s, const Vec3 &eye, const Vec3 &dir, float tMax,
                           float voxel, float *tHit, int *voxOut = nullptr) {
    if (!s.vol || s.msx <= 0 || s.vsy <= 0 || s.msz <= 0) return false;
    float mox = 0.0f, moz = 0.0f, mdx = 0.0f, mdz = 0.0f;
    solidModelSpace(s, eye.x, eye.z, &mox, &moz);
    solidModelDir(s, dir.x, dir.z, &mdx, &mdz);
    const float o[3] = {mox / voxel, (eye.y - s.baseY) / voxel, moz / voxel};
    const float d[3] = {mdx / voxel, dir.y / voxel, mdz / voxel};
    const int n[3] = {int(s.msx), int(s.vsy), int(s.msz)};
    return rayVoxelGrid(s.vol, n, o, d, tMax, tHit, voxOut);
}

// ---------------------------------------------------------------------------
// IS THE BODY OVER THIS MODEL AT ALL -- the WHOLE model, seen from above.
//
// This exists because `touches` below cannot answer that question and was
// being asked it. Its ellipse comes from measureCollider, which measures the
// widest extent in the first kBodyHeightM (two metres) ABOVE THE BASE. That is
// the right footprint for a WALL -- a trunk stops you where your body is -- and
// the wrong one for a FLOOR, because, as measureCollider's own note says, "a
// boulder is usually widest around its middle".
//
// So a big domed rock is wider on top than in its bottom two metres, and the
// ground query rejected the body before it ever looked up the column: you
// walked onto the wide part of the stone and dropped straight through it. The
// bigger the boulder the worse it was, which is why it was always the big ones.
//
// The model's own extent has no such problem, and the per-column lookup that
// follows is exact anyway -- this only has to be a cheap conservative gate that
// never rejects a point the column test would have accepted.
// ---------------------------------------------------------------------------
inline bool overModel(const Solid &s, float wx, float wz, float voxel, float w) {
    if (!s.col) return false;
    float px = 0.0f, pz = 0.0f;
    solidModelSpace(s, wx, wz, &px, &pz);
    return px >= -w && pz >= -w && px <= float(s.msx) * voxel + w &&
           pz <= float(s.msz) * voxel + w;
}

inline bool solidColumnTop(const Solid &s, float wx, float wz, float voxel, float *outY) {
    if (!s.col) return false;
    float px = 0.0f, pz = 0.0f;
    solidModelSpace(s, wx, wz, &px, &pz);
    const int mx = int(floorf(px / voxel));
    const int mz = int(floorf(pz / voxel));
    if (mx < 0 || mz < 0 || mx >= int(s.msx) || mz >= int(s.msz)) return false;
    const int16_t h = s.col[size_t(mx) + size_t(mz) * size_t(s.msx)];
    if (h <= 0) return false;
    *outY = s.baseY + float(h) * voxel;
    return true;
}

// ---------------------------------------------------------------------------
// THE SURFACE UNDER A BODY THAT IS INSIDE THE MODEL -- the first solid voxel at
// or below `ceilY`, rather than the top of the whole column.
//
// solidColumnTop above answers "how high is this model here".
//
// THAT WAS ONCE CALLED "THE ONLY QUESTION A ROCK CAN BE ASKED", on the grounds
// that a boulder is convex from outside and there is nothing under its skin to
// stand on. THE ROCKS ARE NOT CONVEX. They have flared caps, arches and
// overhangs, and under one of those the top of the column is three metres over
// the head of anybody standing there -- so asked the old question the walk put
// them on top of the rock, which is exactly what a building does in its lobby.
// (user 2026-09-18: "I cant move the player underneath a big rock".) The walk
// asks every standable solid this now; tests/rock_overhang_test.cpp pins it.
//
// A BUILDING was the first case, and its column
// runs roof, air, upper floor, air, ground floor, and the top of that column is
// eleven metres over the head of anybody standing in the lobby -- so asked the
// old question the walk puts them on the roof, instantly, the first time they
// move. That is not a tuning problem, it is the wrong question.
//
// `ceilY` is how high the body could possibly have climbed to since last frame:
// its feet plus the step-up. Scanning DOWN from there is what makes a stair a
// stair (the next tread is inside the reach) and a wall a wall (the floor above
// is not), with no per-storey knowledge anywhere.
//
// THE VOXELS, NOT THE HEIGHTFIELD, because the heightfield is one number per
// column and the whole point here is that a column has several surfaces. False
// when the body is off the footprint or there is nothing solid below it, which
// is the same "nothing to stand on" solidColumnTop returns and is what makes a
// walk off the edge a fall.
// ---------------------------------------------------------------------------
inline bool solidColumnTopBelow(const Solid &s, float wx, float wz, float ceilY, float voxel,
                                float *outY) {
    if (!s.vol) return false;
    float px = 0.0f, pz = 0.0f;
    solidModelSpace(s, wx, wz, &px, &pz);
    const int mx = int(floorf(px / voxel));
    const int mz = int(floorf(pz / voxel));
    if (mx < 0 || mz < 0 || mx >= int(s.msx) || mz >= int(s.msz)) return false;
    int my = int(floorf((ceilY - s.baseY) / voxel));
    if (my < 0) return false;
    if (my >= int(s.vsy)) my = int(s.vsy) - 1;
    const size_t stride = size_t(s.msx) * size_t(s.msz);
    const size_t col = size_t(mx) + size_t(mz) * size_t(s.msx);
    for (int y = my; y >= 0; --y)
        if (s.vol[col + size_t(y) * stride]) {
            // The TOP of that voxel -- a body stands on the surface, not in it.
            *outY = s.baseY + float(y + 1) * voxel;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// ONE LIVE CREATURE, FOR THE ONE CHECK THAT HAS TO SEE ALL OF THEM.
//
// Nine populations across seven files keep their members in nine different
// structs, and "is any animal inside anything" cannot be asked of nine private
// vectors. This is the only shape they have in common: where it is, how wide it
// is, and what to call it in a report. See runClipTest in app.h, which is the
// only caller and the reason this exists at all -- a rule nothing checks is a
// rule that comes back.
// ---------------------------------------------------------------------------
struct LifeAt {
    Vec3 p;
    const char *what = "";
    float r = 0.0f;         // half width, for solidsTouch
    // A PERCHED SONGBIRD IS SUPPOSED TO BE IN THE TREE. It is sitting on a
    // branch nine metres up, which is inside the crown by construction and is
    // the one case where being inside a solid is the feature. Counted and
    // printed, never failed.
    bool inTree = false;
    // ...AND A FISH IS SUPPOSED TO BE UNDER THE WATER. The second exemption,
    // and it has the same shape as the first: being submerged is the animal for
    // the nine populations in lake.h and a bug for every other one. See the
    // ladybug that was reported swimming.
    bool inWater = false;
};

// ---------------------------------------------------------------------------
// ...AND THE SAME QUESTIONS ASKED OF A LIST OF SOLIDS.
//
// WHY THESE EXIST AT ALL: the answer above was being written out again by every
// caller that needed it, and most of the callers needed it and did not know.
// The player, the bunnies and the butterflies each walked their own copy of the
// loop; the housefly, the firefly, the ladybug and the bee walked no loop at
// all. So "the flys were caught flying inside a big rock" is a true report
// about four species rather than about one, and the fix is not four guards --
// it is one question with one answer that nothing can ask differently.
//
// ONE LIST, TWO ENDS. solidsContain is the VOLUME -- may a body be here --
// and solidsFloor is the SURFACE -- what is under this column. A flying animal
// needs both, and the difference is not cosmetic: the terrain height field does
// not know a rock exists, so a fly told to hang 1.1 m over the ground hangs
// 1.1 m over the ground UNDERNEATH a five-metre boulder, which is a metre
// inside the stone. That is the reported bug exactly, and no amount of steering
// away from obstacles would have fixed it -- the fly never flew in. It was born
// there, and it stayed because the height it was holding was the height it was
// asked to hold.
// ---------------------------------------------------------------------------

// Is this world point inside any of them? Voxel-accurate wherever the model
// carries its own volume, and the inscribed ellipse only where it does not --
// the same two cases in the same order as insideWorld, which now calls this.
inline bool solidsContain(const Solid *list, int n, float x, float y, float z, float voxel) {
    for (int k = 0; k < n; ++k) {
        const Solid &s = list[k];
        if (s.hx <= 0.0f || s.hz <= 0.0f || y > s.top) continue;
        if (s.vol) {
            if (solidAtWorld(s, x, y, z, voxel)) return true;
            continue;
        }
        const float dx = (x - s.cx) / s.hx, dz = (z - s.cz) / s.hz;
        if (dx * dx + dz * dz < 1.0f) return true;
    }
    return false;
}

// The highest surface over this column: the terrain height `g` handed in,
// raised to the top of any model standing on it, by at most `rise`.
//
// A TRUNK IS NOT A FLOOR. Its column is nine metres of nothing and then twenty
// of canopy, and a flyer told to hold a metre over THAT climbs out of the wood
// the moment it passes a pine. Solid::standable is the distinction and it was
// already drawn here for the ladybug's descent -- a rock is something you can
// be on top of, a tree is something you go round.
//
// ...AND NEITHER IS A CLIFF, WHICH IS WHAT `rise` IS FOR. The first cut of this
// had no cap, on the reasoning that a rock is by definition a thing you can be
// on top of. That is true of a rock and this world has boulders TWENTY METRES
// tall and thirteen across (measured: base 13.9, top 34.6, model 133x113). A
// butterfly holding two metres over the ground was handed 34.6 as its ground,
// spent four and a half seconds climbing the inside of the stone at its 3 m/s
// cap, came off the top, and sank back into the flank at 0.9 m/s -- 85
// creature-frames a minute, up to eight metres in, and every one of them the
// FLOOR RULE working exactly as written.
//
// So the question a flyer is really asking is not "is this standable" but "is
// this a step or a wall". Under `rise` it is ground and the animal goes over
// it; above, this returns the terrain and the animal goes round -- which is
// what solidsContain and flySlide are there to make it do. The two halves
// cannot disagree because there is only one number.
inline float solidsFloor(const Solid *list, int n, float x, float z, float voxel, float g,
                         float rise) {
    float top = g;
    const float ceil = g + rise;
    for (int k = 0; k < n; ++k) {
        const Solid &s = list[k];
        if (!s.standable || s.top <= top) continue;   // cannot raise the answer
        float y = 0.0f;
        if (solidColumnTop(s, x, z, voxel, &y) && y > top && y <= ceil) top = y;
    }
    return top;
}

// ...AND THE SAME QUESTION FOR A BODY THAT HAS A SIZE.
//
// Centre and four rim points, which is the fan Bunnies::blocked already probes
// a marcher with and it is there for a reason worth repeating: a single centre
// probe lets a boulder's OVERHANG sit inside the animal. The volume path cannot
// be grown by a half-width the way an ellipse can -- a voxel grid has no
// half extents to add to -- so the body is walked round instead.
inline bool solidsTouch(const Solid *list, int n, float x, float y, float z, float r,
                        float voxel) {
    if (solidsContain(list, n, x, y, z, voxel)) return true;
    if (r <= 0.0f) return false;
    return solidsContain(list, n, x + r, y, z, voxel) ||
           solidsContain(list, n, x - r, y, z, voxel) ||
           solidsContain(list, n, x, y, z + r, voxel) ||
           solidsContain(list, n, x, y, z - r, voxel);
}

// MOVE A FLYING BODY, AND NEVER INTO STONE. True if the whole step was taken.
//
// Three cases in this order, and the middle one is the one that is easy to
// leave out and impossible to see afterwards:
//
//     the destination is clear    take the whole step
//     the ORIGIN is inside too    take it anyway
//     otherwise                   slide along whichever axis is free
//
// THE ESCAPE CLAUSE IS NOT A SAFETY NET, IT IS THE RECOVERY. A creature can be
// inside a solid without ever having flown into one -- born there before this
// rule existed, a tree felled across it, a player who built round it. A guard
// that only refuses ENTRY would hold that body perfectly still inside the rock
// for ever, which is a worse bug than the one being fixed and looks exactly
// like it. Paired with solidsFloor, which lifts the line it is holding to the
// top of the rock, an animal that starts inside one is out within a second.
//
// AND THE SLIDE IS WHAT KEEPS IT FROM STOPPING DEAD. An insect that refuses a
// step and keeps its heading pushes at the same face every frame and reads as
// stuck on an invisible wall; taking whichever axis is still free makes it
// graze along the boulder instead, which is what an insect does.
inline bool flySlide(const Solid *list, int n, float voxel, float r, float *x, float *y,
                     float *z, float nx, float ny, float nz) {
    if (!solidsTouch(list, n, nx, ny, nz, r, voxel)) {
        *x = nx; *y = ny; *z = nz;
        return true;
    }
    // KEYED ON THE CENTRE, NOT ON THE RIM. Written first as solidsTouch --
    // "it is already in something, let it move" -- which hands the free pass to
    // any creature whose WING is brushing a boulder, and a brushing creature is
    // then free to keep brushing for as long as it likes. Measured at six
    // butterfly-frames a minute against a 7 m rock in the pine wood: the animal
    // itself was never inside anything, and a wingtip was, for a tenth of a
    // second at a time.
    //
    // solidsContain is the honest test of "buried": the creature's own position
    // is in the stone, there is no step out of it that is not also inside it,
    // and refusing to move would hold it there for ever. A rim that touches is
    // refused like any other and slides instead.
    if (solidsContain(list, n, *x, *y, *z, voxel)) {
        *x = nx; *y = ny; *z = nz;
        return true;
    }
    if (!solidsTouch(list, n, nx, ny, *z, r, voxel)) { *x = nx; *y = ny; return false; }
    if (!solidsTouch(list, n, *x, ny, nz, r, voxel)) { *y = ny; *z = nz; return false; }
    if (!solidsTouch(list, n, *x, ny, *z, r, voxel)) *y = ny;
    return false;
}

// ---------------------------------------------------------------------------
// THE MODEL'S FOOTPRINT IN WORLD AXES -- WHICH IS NOT THE COLLIDER'S.
//
// A pine's collider is its TRUNK: 40 to 80 cm across, because that is all a
// walking body can ever meet. Its model is the whole tree, crowns included,
// nine metres wide. Those two numbers being different is the entire design and
// it is right -- but anything that gathers "the solids near p" by the collider
// and then tests the VOXELS has quietly agreed to two different trees.
//
// That is what it did. World::collidersNear filtered on cx/hx, so a butterfly
// 3.5 m from a trunk did not have that tree in its list at all, and flew
// through the branches of a tree it could not see. Every guard in this change
// was working perfectly on a list the crown was missing from.
//
// The model centre is not the collider centre either -- measureCollider offsets
// one from the other, and the placement puts the model's CORNER at tx/tz -- so
// this walks the corner through the instance transform rather than assuming.
inline void solidWorldBox(const Solid &s, float voxel, float *cx, float *cz, float *hx,
                          float *hz) {
    if (!s.msx || !s.msz) {   // no model: the collider is all there is
        *cx = s.cx;
        *cz = s.cz;
        *hx = s.hx;
        *hz = s.hz;
        return;
    }
    const float ex = float(s.msx) * voxel * 0.5f, ez = float(s.msz) * voxel * 0.5f;
    solidWorldSpace(s, ex, ez, cx, cz);
    // A quarter turn swaps the two extents and leaves the box axis-aligned.
    *hx = (s.yaw & 1) ? ez : ex;
    *hz = (s.yaw & 1) ? ex : ez;
    // The collider is measured from the voxels, so it should already be inside
    // this -- but it is a separate measurement and a max costs nothing.
    if (s.hx > *hx) *hx = s.hx;
    if (s.hz > *hz) *hz = s.hz;
}

// Does a body of half-width w, centred at (x, z), touch the footprint?
//
// Exact rather than approximate. Dividing through by the half extents maps the
// ellipse to a unit circle and the body's square to a rectangle, and the
// closest-point distance from a rectangle to the origin is the same expression
// in either space -- so this is a box-circle test, which has an exact form,
// wearing an ellipse's clothes.
// -- A SOLID WITH NO EXTENT TOUCHES NOTHING ------------------------------
//
// (user 2026-09-22: "still when cutting down a tree, Im getting invisible
//  barriers to the player".)
//
// THIS DIVIDED BY THE HALF EXTENTS AND dropSolid SETS THEM TO ZERO. Those two
// facts sat a file apart for months. `dropSolid` is how the engine says "this
// instance is gone" -- it zeroes hx/hz and nulls the pointers -- and
// Player::blocked falls through to this function for exactly the solids whose
// `vol` is null, which is exactly the ones dropSolid just emptied. So every
// felled tree fed 0/0 and x/0 into the walk.
//
// AND THE ANSWER IS NOT EVEN CONSISTENTLY WRONG. This engine builds with
// /fp:fast and /arch:AVX2 (see the compile line), which lets the compiler
// assume no NaNs and rewrite a/b as a*(1/b) -- so what a zero extent produces
// is whatever the optimiser makes of it that day. Measured, it blocked: a
// felled oak left a barrier running fourteen metres from its own stump, along
// the line z == cz where the numerator is zero and the quotient is 0/0.
//
// The guard is also the honest statement of the geometry. An ellipse with a
// zero axis has no interior; nothing can be inside it. Returning false is not
// a workaround for the division, it is the answer the division was failing to
// compute.
inline bool touches(const Solid &s, float x, float z, float w) {
    if (!(s.hx > 0.0f) || !(s.hz > 0.0f)) return false;
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
            // Same expression as cx/cz below, over the base slab rather than
            // the body one -- so the two cannot drift apart.
            c.baseCX = (float(bx0 + bx1 + 1) * 0.5f - float(a.sx) * 0.5f) * voxelM;
            c.baseCZ = (float(bz0 + bz1 + 1) * 0.5f - float(a.sz) * 0.5f) * voxelM;
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

}  // namespace v2
