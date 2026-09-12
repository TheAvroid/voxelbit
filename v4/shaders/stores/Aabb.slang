// ---------------------------------------------------------------------------
// stores/Aabb.slang -- the voxel store, traced by the ray tracing cores.
//
// A brick of 8^3 voxels is one AABB in a bottom-level acceleration structure
// and one procedural primitive to the hardware. Traversal is therefore split
// exactly down the middle:
//
//   THE HARDWARE finds which bricks a ray passes through, in order, skipping
//   the empty 99 % of a wood in its own silicon. This is the part no software
//   walk can compete with -- it is a BVH descent in fixed-function units, with
//   its own cache hierarchy, running while the shader is stalled on something
//   else.
//
//   THIS SHADER decides what happens inside one 0.8 m box: a DDA over a
//   512-bit occupancy mask, at most twenty-two steps, with the mask small
//   enough to stay in registers. This is the part the hardware cannot do,
//   because a voxel is not a primitive it knows about.
//
// Neither half is doing the other's job, and that is the whole design. A pure
// software march has to traverse the empty space itself; a pure triangle
// representation has to BUILD the surface, which is the thing that makes an
// edit expensive.
//
// ---------------------------------------------------------------------------
// THE CANDIDATE LOOP, AND THE ONE THING ABOUT IT THAT IS NOT OBVIOUS.
//
// Inline ray tracing has no attribute payload: CommitProceduralPrimitiveHit()
// records a `t` and nothing else, so there is no way to hand the winning
// voxel's coordinate, normal and material out of the intersection the way a
// real intersection shader would through its attribute struct.
//
// The way through is that the commit MONOTONICALLY SHRINKS the query's TMax.
// Every commit is therefore closer than the last, so the LAST set of locals
// written beside a commit is the winning hit. `best` below is exactly that --
// not a running minimum, and no comparison is needed, because the hardware has
// already done the comparison by refusing to hand back a candidate beyond the
// current TMax.
//
// ---------------------------------------------------------------------------
// EVERYTHING IS IN WORLD SPACE, AND THE INSTANCE TRANSFORMS ARE THE IDENTITY.
//
// A chunk's AABBs are built in world metres rather than chunk-local, so
// CandidateObjectRayOrigin() equals the world ray origin and there is no
// object-to-world matrix anywhere in this file. It gives up instancing -- two
// identical chunks cannot share one structure -- which this engine has no use
// for, and it removes the single largest source of "the shadows are the right
// shape and in the wrong place" bugs in a store like this.
//
// The walk itself runs in VOXEL units, not metres: the ray origin AND its
// direction are both divided by kVoxelM and the direction is never normalised,
// so `t` comes out in the same units it went in -- metres, directly comparable
// with the RayQuery's own. Dividing only the origin, or normalising on the way
// in, gives a world whose shadows are the right shape and the wrong length,
// which reads as a lighting bug and is not one.
// ---------------------------------------------------------------------------
#ifndef V4_STORE_AABB_SLANG
#define V4_STORE_AABB_SLANG

#include "AabbShared.slang"

// Use the per-brick octant byte to reject a candidate before touching a mask
// word. Host-selected so the two can be measured against each other.
#ifndef V4_OCTANT_SKIP
#define V4_OCTANT_SKIP 1
#endif

// The chunks, as one top-level structure of identity-transformed instances.
// Each instance's InstanceID is its chunk's FIRST BRICK INDEX, so the brick a
// candidate belongs to is one add and no indirection.
RaytracingAccelerationStructure gStoreTlas;

// One header per brick, every chunk's run contiguous and in the same order as
// that chunk's AABBs.
StructuredBuffer<V4Brick> gStoreBricks;

// EVERY BRICK'S MASK AND ITS MATERIALS, IN ONE BUFFER AND IN THAT ORDER.
//
// 16 words of occupancy, then one byte per solid voxel packed four to a word,
// immediately after -- see the note on V4Brick::mtlBase for why the run is
// compacted by rank.
//
// THEY SHARE A BUFFER BECAUSE THEY SHARE CACHE LINES. With the materials in an
// array of their own, resolving one was a scattered fetch into eleven megabytes
// nothing else in the frame had touched, and it cost 3 ms of an 18 ms trace at
// 4K. A brick's material now sits within a few hundred bytes of the mask word
// the walk just read.
//
// A ByteAddressBuffer would read the material byte in one Load, but a
// structured buffer of uints is what Falcor binds with no view plumbing, and
// the shift is two instructions.
StructuredBuffer<uint> gStoreMasks;

cbuffer V4StoreCB
{
    V4StoreConsts gStore;
};

// ---------------------------------------------------------------------------
// A direction no component of which is exactly zero.
//
// The slab test and the DDA both divide by the direction. An axis-aligned ray
// gives 1/0 = +-inf, which is fine and is what the slab test wants -- right up
// until the ray also starts exactly ON that slab, where (bmin - o) is 0 and the
// product is 0 * inf = NaN. A NaN then propagates through min/max in whichever
// direction the hardware feels like, and the symptom is a scatter of single
// black pixels on perfectly axis-aligned surfaces -- which is most of them, in
// a voxel world.
//
// Nudging to 1e-8 keeps the reciprocal finite (1e8) and enormous, which behaves
// exactly like infinity everywhere it matters and cannot produce a NaN.
// ---------------------------------------------------------------------------
float3 storeSafeDir(float3 d)
{
    const float e = 1e-8f;
    return float3(abs(d.x) < e ? (d.x < 0.0f ? -e : e) : d.x,
                  abs(d.y) < e ? (d.y < 0.0f ? -e : e) : d.y,
                  abs(d.z) < e ? (d.z < 0.0f ? -e : e) : d.z);
}

// WORD-SPARSE. A word that held nothing was never written, so a voxel in one is
// air WITHOUT A LOAD -- `wordMap` (V4Brick::octants bits 8..23) says which of the
// sixteen survive, and the kept ones are contiguous from maskBase.
//
// It costs one countbits and one test a step and saves 46 % of the biggest thing
// in the store -- 20.5 MB of masks becomes 11.1. Word-sparse and not
// octant-sparse for a reason that is not the ratio (they measured 46.1 % against
// 45.7 %, a wash): dropping empty WORDS leaves the LINEAR bit order intact, so
// the material run's popcount rank is unchanged and this file and scene/bricks.h
// keep the one ordering they already agree about.
bool storeMaskBit(uint maskBase, uint wordMap, uint bit)
{
    const uint wi = bit >> 5u;
    if (((wordMap >> wi) & 1u) == 0u) return false;
    const uint pos = countbits(wordMap & ((1u << wi) - 1u));
    return ((gStoreMasks[maskBase + pos] >> (bit & 31u)) & 1u) != 0u;
}

// The material of one voxel of a non-uniform brick.
//
// COUNT THE SET BITS BEFORE IT. The run holds one byte per solid voxel in bit
// order, so a voxel's byte is at mtlBase + popcount(everything before it). At
// most fifteen countbits() and one masked one -- a handful of instructions
// against 512 bytes per brick saved, which over a wood is about a third of the
// store. The order this counts in IS the order bricks.h packs in; they cannot
// be allowed to drift, and AabbShared.slang states it once for both.
// TAKES THE TWO BASES, NOT THE HEADER, because it is no longer called from
// inside the walk -- see kMtlPending. Passing V4Brick kept all eight of its
// words live across the DDA for the sake of two.
// `np` is the brick's palette size, from V4Brick::flags bits 16..21. ZERO means
// the palette overflowed and this brick wrote RAW bytes -- see the note in
// scene/bricks.h. Otherwise: N palette bytes at mtlBase, flushed to a WORD
// boundary, then the indices packed at ceil(log2 N) bits each.
//
// 52 % of detail bricks wear two materials or fewer, which is ONE BIT a voxel
// against eight; 12.7 MB of runs becomes 5.5. It is affordable only because the
// resolve moved out of the walk -- see kMtlPending -- so this runs once per RAY.
uint storeVoxelMtl(uint maskBase, uint wordMap, uint mtlBase, uint np, uint bit)
{
    const uint wi = bit >> 5u;
    const uint pos = countbits(wordMap & ((1u << wi) - 1u));

    // THE RANK IS UNCHANGED BY THE SPARSITY: dropped words were all zero and
    // contributed nothing to the count of set bits before this voxel.
    uint before = 0u;
    for (uint k = 0u; k < pos; ++k) before += countbits(gStoreMasks[maskBase + k]);
    const uint lo = bit & 31u;
    before += countbits(gStoreMasks[maskBase + pos] & ((lo == 0u) ? 0u : ((1u << lo) - 1u)));

    if (np == 0u)
    {
        const uint raw = mtlBase + before;  // BYTE index into the SAME buffer
        return (gStoreMasks[raw >> 2u] >> ((raw & 3u) * 8u)) & 0xFFu;
    }

    uint bits = 1u;
    while ((1u << bits) < np) ++bits;

    // ceil(N/4) WORDS past mtlBase, not N bytes: the palette is flushed a word
    // at a time. Reading from byte mtlBase + N instead is off by up to three
    // bytes whenever N is not a multiple of four, which decodes a neighbouring
    // voxel's index -- and that does not fail, it paints the wrong colour.
    const uint absBit = (((mtlBase >> 2u) + ((np + 3u) >> 2u)) << 5u) + before * bits;
    const uint w0 = absBit >> 5u;
    const uint sh = absBit & 31u;
    uint packed = gStoreMasks[w0] >> sh;
    // Only when the field straddles the boundary, which also guarantees sh > 0 --
    // a shift by 32 is taken mod 32 on this hardware and would read back the
    // word unshifted.
    if (sh + bits > 32u) packed |= gStoreMasks[w0 + 1u] << (32u - sh);

    const uint idx = mtlBase + (packed & ((1u << bits) - 1u));
    return (gStoreMasks[idx >> 2u] >> ((idx & 3u) * 8u)) & 0xFFu;
}

// A MATERIAL THE WALK FOUND BUT DID NOT FETCH.
//
// NOT AN ATTEMPT TO MAKE THE RESOLVE CHEAPER. Two of those were tried and
// neither worked -- a popcount prefix table (3.33 ms against 3.04) and holding
// the mask word across steps (3 % slower) -- because the mask is sixty-four
// contiguous bytes already in L1 and was never the cost. This changes HOW OFTEN
// the resolve runs instead.
//
// A COMMIT IS NOT A HIT. CommitProceduralPrimitiveHit only shrinks the query's
// TMax, so a ray handed three progressively nearer bricks resolves three
// materials and throws two away. Only the last one is ever read. So the walk
// records where the answer is and storeTrace fetches it once, after Proceed()
// has returned and the winner is known.
//
// maskBase == kMtlPending is the marker that a fetch is still owed. Everything
// already free stays inline and sets `mtl` directly: a uniform or monochrome
// brick answers from a field of `flags` in a register, and an occlusion query
// never needs a material at all.
static const uint kMtlPending = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// WHAT A RAY KNOWS BEFORE IT MEETS A BRICK.
//
// All four of these depend ONLY on the direction, and storeIntersectBrick used
// to rebuild every one of them for every candidate the hardware handed back --
// three reciprocals among them. A ray crosses many bricks, so that is the same
// arithmetic repeated per candidate to reach the same answer.
//
// AND IT MEASURED FLAT -- 15.58 ms against 15.72, inside the +-1.6 % this scene
// varies by run to run. The compiler was ALREADY doing it: storeIntersectBrick
// inlines into the candidate loop, `rd` is invariant across it, and hoisting
// invariant pure arithmetic is the first thing any optimiser does. Kept because
// it is free, output-identical and says so out loud -- not because it bought
// anything. Do not try it again expecting a win.
//
// WHAT THE SAME SESSION DID ESTABLISH is that the loop is not where the time is
// either: --max-steps 1 costs 15.90 ms against 15.72 at twenty-four, so capping
// the walk at a single step changes nothing. The tight AABB drops the ray inside
// the contents and it finds a voxel in a step or two. Neither the stepping nor
// its setup is the cost -- what remains is the number of CANDIDATES, which is
// why merging uniform bricks into bigger boxes is the lever and this was not.
// ---------------------------------------------------------------------------
struct RayConst
{
    float3 inv;     // 1 / rd, the slab test's and the DDA's divisor
    float3 tDelta;  // abs(inv), one cell's worth of t on each axis
    int3 stp;       // -1 or +1 per axis, the direction the walk steps
    int3 stp01;     // 1 where stp is positive, 0 where negative
};

RayConst storeRayConst(float3 rd)
{
    RayConst rc;
    rc.inv = 1.0f / rd;
    rc.tDelta = abs(rc.inv);
    rc.stp = int3(rd.x < 0.0f ? -1 : 1, rd.y < 0.0f ? -1 : 1, rd.z < 0.0f ? -1 : 1);
    rc.stp01 = int3(rc.stp.x > 0 ? 1 : 0, rc.stp.y > 0 ? 1 : 0, rc.stp.z > 0 ? 1 : 0);
    return rc;
}

struct BrickHit
{
    float t;
    float3 ng;
    int3 ijk;
    uint mtl;
    uint maskBase;  // kMtlPending unless a resolve is still owed
    uint mtlBase;
    uint bit;
    uint wordMap;   // V4Brick::octants bits 8..23, the kept-word map
    uint np;        // palette size; 0 means the brick wrote raw bytes
};

// The outward face normal for a crossing of `axis` travelling along `d`: it
// points back the way the ray came, on that axis alone. A voxel face has
// exactly one non-zero component and there is no shading normal to bend.
float3 storeAxisNormal(int axis, float3 d)
{
    float3 n = float3(0.0f, 0.0f, 0.0f);
    if (axis == 0) n.x = (d.x < 0.0f) ? 1.0f : -1.0f;
    else if (axis == 1) n.y = (d.y < 0.0f) ? 1.0f : -1.0f;
    else n.z = (d.z < 0.0f) ? 1.0f : -1.0f;
    return n;
}

// ---------------------------------------------------------------------------
// ONE BRICK.
//
// `ro` and `rd` are in VOXEL units. tMin/tMax bound the search, and are the
// query's own -- the hardware will not offer a candidate past the current
// committed hit, so this is belt and braces rather than the real clip.
//
// `anyHit` skips resolving the material, which is the whole reason
// storeOccluded is a separate entry point: in a canopy most shadow rays die in
// the first metre of foliage, and the popcount walk that finds a needle's
// colour is pure waste when the answer is a bool.
// ---------------------------------------------------------------------------
bool storeIntersectBrick(uint brickIdx, float3 ro, float3 rd, RayConst rc, float tMin,
                         float tMax, bool anyHit, bool passWater, out BrickHit bh)
{
    bh.t = 0.0f;
    bh.ng = float3(0.0f, 1.0f, 0.0f);
    bh.ijk = int3(0, 0, 0);
    bh.mtl = 0u;
    bh.maskBase = kMtlPending;  // nothing owed unless the walk says so
    bh.mtlBase = 0u;
    bh.bit = 0u;
    bh.wordMap = 0u;
    bh.np = 0u;

    const V4Brick b = gStoreBricks[brickIdx];
    const int3 org = int3(b.ox, b.oy, b.oz);

    // THE OCCUPIED BOX, NOT THE BRICK. Three bits an axis each way -- see the
    // note on V4Brick::bounds. This is the same box the hardware clipped the
    // candidate against, so the two agree about where the contents are, and the
    // walk below starts inside them instead of stepping through the margin.
    const uint bs = uint(kBrickShift);
    const uint bm = uint(kBrickE - 1);
    // A UNIFORM BRICK MAY BE A MERGED BOX OF MANY BRICKS -- see
    // PackOptions::mergeUniform in scene/bricks.h. Its extent cannot fit in
    // `bounds`, which is three bits an axis, so it lives in the two words a
    // mask would have used and that a uniform brick has no use for. Everything
    // downstream is unchanged: the uniform path already answered from the slab
    // test and a field of `flags`, and it never cared how big the box was.
    const bool uniformBrick = (b.flags & 1u) != 0u;
    const int3 blo = uniformBrick ? int3(0, 0, 0)
                                  : int3(int(b.bounds & bm), int((b.bounds >> bs) & bm),
                                         int((b.bounds >> (2u * bs)) & bm));
    const int3 bhi =
        uniformBrick
            ? int3(int(b.maskBase & 0xFFFFu), int(b.maskBase >> 16u), int(b.mtlBase))
            : int3(int((b.bounds >> (3u * bs)) & bm), int((b.bounds >> (4u * bs)) & bm),
                   int((b.bounds >> (5u * bs)) & bm));
    const float3 bmin = float3(org + blo);
    const float3 bmax = float3(org + bhi + 1);

    const float3 inv = rc.inv;  // built once per ray -- see RayConst
    const float3 ta = (bmin - ro) * inv;
    const float3 tb = (bmax - ro) * inv;
    const float3 tlo = min(ta, tb);
    const float3 thi = max(ta, tb);

    const float tBox = max(max(tlo.x, tlo.y), tlo.z);
    float tEnter = max(tBox, tMin);
    const float tExit = min(min(min(thi.x, thi.y), thi.z), tMax);
    if (tEnter > tExit) return false;

    // Which face the ray came in through. `insideAtStart` is the case where the
    // ray BEGAN within the box -- a shadow ray leaving a surface does this
    // constantly -- and there is then no entry face to speak of, so the normal
    // is taken from the dominant axis of travel instead.
    int axis = 0;
    if (tlo.y > tlo.x) axis = 1;
    if (tlo.z > tlo.x && tlo.z > tlo.y) axis = 2;
    const bool insideAtStart = tBox < tMin;

    // -- ABLATION: STOP AT THE BOX AND DO NOT WALK IT -----------------------
    //
    // debugMode 3 commits the moment the ray enters a brick's box, without
    // looking at a single mask bit. The picture is wrong -- every brick becomes
    // solid -- and that is not the point: what it measures is the cost of the
    // hardware finding the bricks and handing them over, with the software walk
    // taken out entirely. Normal trace time minus this one IS the DDA, and
    // knowing which of the two dominates is the difference between optimising
    // the culling and optimising the loop. It is a uniform branch on a constant
    // buffer value, so it costs nothing when it is off.
    if (gStore.debugMode == 3u)
    {
        bh.t = tEnter;
        bh.ijk = clamp(int3(floor(ro + rd * tEnter)), org + blo, org + bhi);
        bh.ng = storeAxisNormal(axis, rd);
        bh.mtl = kStone0;
        return true;
    }

    // -- the uniform case ---------------------------------------------------
    //
    // Every voxel solid and all one material: the slab test IS the
    // intersection. Underground this is most of the bricks that survive the
    // hidden-brick cull, and it costs six compares against a walk.
    if (uniformBrick)
    {
        // A UNIFORM BRICK OF WATER STOPS NOTHING. See the note on anyHit
        // below: a shadow ray that died in a lake left its bed unlit, and the
        // middle of a lake is exactly where uniform water bricks are.
        if ((anyHit || passWater) && ((b.flags >> 8u) & 0xFFu) == kWater) return false;
        bh.t = tEnter;
        const float3 p = ro + rd * tEnter;
        bh.ijk = clamp(int3(floor(p)), org + blo, org + bhi);
        bh.ng = insideAtStart ? storeAxisNormal((abs(rd.x) > abs(rd.y))
                                                    ? ((abs(rd.x) > abs(rd.z)) ? 0 : 2)
                                                    : ((abs(rd.y) > abs(rd.z)) ? 1 : 2),
                                                rd)
                              : storeAxisNormal(axis, rd);
        bh.mtl = (b.flags >> 8u) & 0xFFu;
        return true;
    }

    // -- THE WALK ----------------------------------------------------------
    //
    // MEASURED, and this is what the shape of it is FOR. Ablating the walk
    // entirely -- committing at the box without looking at a mask bit -- splits
    // a 2.63 ms trace into 0.60 ms of ray setup and lighting with no geometry
    // at all, 0.35 ms for the hardware to find every brick a ray crosses and
    // hand it over, and 1.68 ms in here. The acceleration structure is nearly
    // free. This loop is the store.
    //
    // And most of it is spent finding NOTHING: inside its own tight box a brick
    // is about a quarter full, so the common case is a ray stepping through
    // twenty-two cells of a box it entered and leaving empty-handed.
    const int3 stp = rc.stp;
    const int3 stp01 = rc.stp01;

#if V4_OCTANT_SKIP
    // -- THE REJECTION PASS, over the eight octants ------------------------
    //
    // A SEPARATE, SEQUENTIAL LOOP AND NOT A NESTED ONE, and that is the whole
    // lesson of this function. Nesting the cell walk inside an octant walk --
    // the obvious way to use these bits -- was measured at 3.51 ms against the
    // flat loop's 2.63: re-seeding the inner DDA at every octant boundary and
    // holding both levels' state at once costs more in registers and occupancy
    // than the skipped cells save.
    //
    // Run FIRST and on its own, it costs four bit tests and reads nothing from
    // memory. If no octant on the ray's path holds anything, the candidate is
    // rejected without a single mask load -- which is the case the walk was
    // spending most of its time on. If one does, the flat walk below runs
    // exactly as it always did, and the pre-pass has cost four steps.
    {
        const int H = kBrickE / 2;
        const int3 olo = blo / H, ohi = bhi / H;
        // Subtract before dividing: integer division truncates toward zero, and
        // this world runs both ways from the origin.
        int3 o = clamp((int3(floor(ro + rd * tEnter)) - org) / H, olo, ohi);
        float3 tNextOct = max((float3(org + (o + stp01) * H) - ro) * inv,
                              float3(tEnter, tEnter, tEnter));
        const float3 tDeltaOct = abs(inv) * float(H);
        // NOT named `any` -- that shadows the intrinsic the loop below calls,
        // and the error names the call site rather than the declaration.
        bool reachable = false;
        float tO = tEnter;

        [loop]
        for (uint so = 0u; so < 8u; ++so)
        {
            if (any(o < olo) || any(o > ohi)) break;
            if ((b.octants & (1u << (uint(o.x) | (uint(o.y) << 1u) | (uint(o.z) << 2u)))) != 0u)
            {
                reachable = true;
                break;
            }
            if (tNextOct.x <= tNextOct.y && tNextOct.x <= tNextOct.z)
            {
                o.x += stp.x;
                tO = tNextOct.x;
                tNextOct.x += tDeltaOct.x;
            }
            else if (tNextOct.y <= tNextOct.z)
            {
                o.y += stp.y;
                tO = tNextOct.y;
                tNextOct.y += tDeltaOct.y;
            }
            else
            {
                o.z += stp.z;
                tO = tNextOct.z;
                tNextOct.z += tDeltaOct.z;
            }
            if (tO > tExit) break;
        }
        if (!reachable) return false;
    }
#endif

    const float3 pEnter = ro + rd * tEnter;
    int3 v = clamp(int3(floor(pEnter)) - org, blo, bhi);

    const float3 tDelta = rc.tDelta;
    // CLAMPED FORWARD. The boundary of the cell the entry point is in cannot be
    // behind that entry point, but float rounding can put it a few ulp behind --
    // and an un-clamped tNext below tEnter makes the first step report a `t`
    // smaller than where the ray entered the box, committing a hit in front of
    // geometry the hardware has already cleared.
    float3 tNext = max((float3(org + v + stp01) - ro) * inv, float3(tEnter, tEnter, tEnter));

    float tCur = tEnter;
    int axisCur = axis;
    bool first = true;


    // THE BOUNDS CHECK IS ONE AXIS, NOT SIX COMPARES, AND IT IS AT THE BOTTOM.
    //
    // The entry cell was clamped into [blo, bhi] before the loop, so it is in
    // range by construction and testing it again on the way in is pure cost.
    // After that, a DDA step moves exactly ONE axis -- so that is the only axis
    // that can have left the box, and the other four compares can never fire.
    // Six compares a step, on the loop that is 52 % of the whole trace.
    //
    // It bought 2 %, and that number is worth keeping: it says this loop is not
    // compare-bound. HOLDING THE MASK WORD ACROSS STEPS was tried on the
    // strength of it -- the word index is (y >> 2) | (z << 1) with no x in it,
    // so eight cells along x share one word -- and it came out 3 % SLOWER. The
    // load was already hitting cache and the extra compare cost more than it
    // saved. The walk is bound by how many candidate-steps there are, not by
    // any one thing inside a step.
    [loop]
    // THE CAP IS THE UNIFORM, NOT THE CONSTANT. gStore.maxSteps has been
    // uploaded every frame since this store was written and NOTHING READ IT --
    // the loop bounded itself with the compile-time kBrickMaxSteps, so the
    // "sweepable" cap AabbShared.slang advertises was not sweepable at all.
    //
    // Reading it here is what makes --max-steps a measurement instrument: cap
    // the walk at N and the trace tells you how much of its cost lives beyond
    // step N. The picture is wrong below the real maximum -- rays that would
    // have hit report a miss -- and that is fine, because the question is where
    // the TIME is, not what the frame looks like.
    for (uint s = 0u; s < gStore.maxSteps; ++s)
    {
        const uint bit = uint(v.x) | (uint(v.y) << bs) | (uint(v.z) << (2u * bs));
        if (storeMaskBit(b.maskBase, b.octants >> 8u, bit))
        {
            // -- WATER IS NOT AN INTERFACE TO A RAY ALREADY IN IT -------
            //
            // TWO CALLERS PASS THROUGH WATER, for two reasons that turn out to
            // be the same reason.
            //
            // A SHADOW RAY, always. One that stops on a water voxel leaves
            // every lake bed in total darkness, and nothing downstream can
            // recover from it: the caustics are drawn by multiplying the bed's
            // ALBEDO, so they brighten a surface the sun never reached and the
            // bed stays black however strong the webs are. Two renders of this
            // went looking for a broken refraction.
            //
            // A PATH ALREADY INSIDE THE WATER. Water is a material on a voxel
            // grid, so a lake's interior is a stack of voxel FACES, and the
            // tracer was treating every one of them as an air-water boundary:
            // a metre of water is eleven refractions, each bending the ray
            // further and reflecting a few per cent of it, and the path ran out
            // of bounces before it reached the bed. Raising --depth from the
            // default to 32 visibly cleared the water, which is what identified
            // this. There is ONE interface -- where the water meets air -- and
            // the march crossing from one water voxel to the next is not it.
            //
            // THE COST IS A MATERIAL RESOLVE INSIDE A WALK BUILT TO AVOID ONE,
            // so it is gated on flags bit 2, which the host sets only for
            // bricks that actually keep a water voxel. A shadow ray dying in a
            // canopy, in litter or in rock never reaches this.
            bool passThis = false;
            if ((anyHit || passWater) && (b.flags & 4u) != 0u)
            {
                const uint wm = ((b.flags & 2u) != 0u)
                                    ? ((b.flags >> 8u) & 0xFFu)
                                    : storeVoxelMtl(b.maskBase, b.octants >> 8u, b.mtlBase,
                                                    (b.flags >> 16u) & 0x3Fu, bit);
                passThis = (wm == kWater);
            }
            if (!passThis)
            {
            bh.t = tCur;
            bh.ijk = org + v;
            bh.ng = (first && insideAtStart)
                        ? storeAxisNormal((abs(rd.x) > abs(rd.y))
                                              ? ((abs(rd.x) > abs(rd.z)) ? 0 : 2)
                                              : ((abs(rd.y) > abs(rd.z)) ? 1 : 2),
                                          rd)
                        : storeAxisNormal(axisCur, rd);
            // ABLATION 4: walk the mask but do not resolve the material, so
            // the popcount prefix -- up to sixteen dependent mask loads on
            // every hit -- is taken out and what is left is the walk alone.
            // A MONOCHROME BRICK ANSWERS FROM A WORD ALREADY IN REGISTERS.
            // No run, no popcount, no mask re-reads -- see the flag bits in
            // AabbShared.slang. A third of detail bricks take this path.
            // THE FREE ANSWERS STAY HERE; THE ONE THAT COSTS IS DEFERRED.
            // See kMtlPending.
            if (anyHit || gStore.debugMode == 4u)
            {
                bh.mtl = 1u;
            }
            else if ((b.flags & 2u) != 0u)
            {
                bh.mtl = (b.flags >> 8u) & 0xFFu;
            }
            else
            {
                bh.maskBase = b.maskBase;
                bh.mtlBase = b.mtlBase;
                bh.bit = bit;
                // The resolve happens after Proceed() returns, by which time
                // this brick's header is long gone -- so what it needs travels
                // with the hit rather than being re-read.
                bh.wordMap = b.octants >> 8u;
                bh.np = (b.flags >> 16u) & 0x3Fu;
            }
            return true;
            }
        }

        if (tNext.x <= tNext.y && tNext.x <= tNext.z)
        {
            v.x += stp.x;
            tCur = tNext.x;
            tNext.x += tDelta.x;
            axisCur = 0;
            if (v.x < blo.x || v.x > bhi.x) break;
        }
        else if (tNext.y <= tNext.z)
        {
            v.y += stp.y;
            tCur = tNext.y;
            tNext.y += tDelta.y;
            axisCur = 1;
            if (v.y < blo.y || v.y > bhi.y) break;
        }
        else
        {
            v.z += stp.z;
            tCur = tNext.z;
            tNext.z += tDelta.z;
            axisCur = 2;
            if (v.z < blo.z || v.z > bhi.z) break;
        }
        first = false;
        if (tCur > tExit) break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// THE SEAM. Closest hit.
// ---------------------------------------------------------------------------
VoxelHit storeTrace(float3 o, float3 d, float tMax, bool insideWater)
{
    VoxelHit vh = storeMiss();

    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0f;
    ray.TMax = tMax;

    // RAY_FLAG_NONE, not FORCE_OPAQUE. Opacity is about whether an ANY-HIT
    // shader runs, and there is none here -- every geometry is built Opaque, so
    // a candidate arrives ready to commit. What cannot be switched off is the
    // candidate itself: a procedural primitive has no hardware intersection, so
    // Proceed() returns once per brick the ray touches and the loop below IS
    // the intersection test. That is the trade a voxel store makes.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(gStoreTlas, RAY_FLAG_NONE, 0xFFu, ray);

    const float3 ro = o / kVoxelM;
    const float3 rd = storeSafeDir(d / kVoxelM);
    const RayConst rc = storeRayConst(rd);

    BrickHit best;
    best.t = 0.0f;
    best.ng = float3(0.0f, 1.0f, 0.0f);
    best.ijk = int3(0, 0, 0);
    best.mtl = 0u;
    best.maskBase = kMtlPending;
    best.mtlBase = 0u;
    best.bit = 0u;
    best.wordMap = 0u;
    best.np = 0u;
    uint steps = 0u;

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            ++steps;
            BrickHit bh;
            // ONE ADD AND NO INDIRECTION. The instance carries its chunk's first
            // brick index as its InstanceID, and a chunk's bricks are contiguous
            // and in AABB order -- see PackedStore in bricks.h.
            const uint bi = q.CandidateInstanceID() + q.CandidatePrimitiveIndex();
            if (storeIntersectBrick(bi, ro, rd, rc, q.RayTMin(), q.CommittedRayT(), false,
                                    insideWater, bh))
            {
                best = bh;
                q.CommitProceduralPrimitiveHit(bh.t);
            }
        }
    }

    if (q.CommittedStatus() != COMMITTED_PROCEDURAL_PRIMITIVE_HIT) return vh;

    vh.hit = true;
    vh.t = q.CommittedRayT();
    // FROM THE RAY AND THE COMMITTED t, not from the walk's own pEnter. They
    // agree to the last bit here because the commit stored exactly this t --
    // but deriving it the same way for every backend is what keeps two stores
    // shading the same surface identically, which is the point of the seam.
    vh.p = o + d * vh.t;
    vh.ng = best.ng;
    vh.ijk = best.ijk;
    // THE ONE MATERIAL RESOLVE OF THE WHOLE RAY. Which brick won is not known
    // until Proceed() has returned -- see kMtlPending.
    vh.mtl = (best.maskBase == kMtlPending)
                 ? best.mtl
                 : storeVoxelMtl(best.maskBase, best.wordMap, best.mtlBase, best.np,
                                 best.bit);

    // Debug views, off unless asked for. They are here rather than in the
    // tracer because they are about the STORE: mode 1 paints each brick a
    // colour of its own, which makes the acceleration structure's granularity
    // visible, and mode 2 paints how many candidate bricks the ray was handed,
    // which is the number the whole design is trying to keep small.
    if (gStore.debugMode == 1u)
    {
        const uint h = uint(best.ijk.x >> 3) * 374761393u + uint(best.ijk.y >> 3) * 1103515245u +
                       uint(best.ijk.z >> 3) * 668265263u;
        vh.mtl = kGrass0 + ((h >> 7) % kGrassCount);
    }
    else if (gStore.debugMode == 2u)
    {
        vh.mtl = kStone0 + min(steps, uint(kStoneCount) - 1u);
    }
    return vh;
}

// ---------------------------------------------------------------------------
// Anything at all between here and there.
//
// A DIFFERENT AND MUCH CHEAPER WALK, which is why the seam keeps it a separate
// entry point rather than asking storeTrace().hit. It stops at the first solid
// voxel, never resolves a material, and lets the hardware abandon traversal on
// the first commit. In a conifer canopy most shadow rays die inside the first
// metre of foliage and this is most of the frame's rays; collapsing it into the
// closest-hit form is the single easiest way to give away half a store's
// performance.
// ---------------------------------------------------------------------------
bool storeOccluded(float3 o, float3 d, float tMax)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0f;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(gStoreTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, ray);

    const float3 ro = o / kVoxelM;
    const float3 rd = storeSafeDir(d / kVoxelM);
    const RayConst rc = storeRayConst(rd);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            BrickHit bh;
            const uint bi = q.CandidateInstanceID() + q.CandidatePrimitiveIndex();
            if (storeIntersectBrick(bi, ro, rd, rc, q.RayTMin(), q.CommittedRayT(), true,
                                    true, bh))
                q.CommitProceduralPrimitiveHit(bh.t);
        }
    }
    return q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT;
}

#endif  // V4_STORE_AABB_SLANG
