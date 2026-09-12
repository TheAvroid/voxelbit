// ---------------------------------------------------------------------------
// stores/AabbShared.slang -- the layout the AABB store's host and device halves
// both have to agree about. Compiled twice: once by Slang as part of every
// program that traces, once by cl.exe as a C++ header from gpu/store.h.
//
// EVERY MEMBER OF EVERY STRUCT HERE IS A SCALAR, AND THAT IS DELIBERATE.
//
// A StructuredBuffer element is laid out by the shader compiler, and the rules
// it uses for a vector member are not C++'s: an int3 is aligned to 16 bytes by
// some compilers and to 4 by others, so a struct with an int3 followed by three
// uints is 32 bytes here and 24 bytes there, with nothing to say which you got.
// Written as scalars there is no rule to disagree about -- the struct is eight
// four-byte words in both languages and the static_assert in gpu/store.h says
// so. It costs three lines of `int3(b.ox, b.oy, b.oz)` in the shader, which is
// a cheap price for a layout that cannot silently drift.
//
// The constants block DOES follow the 16-byte row rule, because it is a real
// constant buffer -- see the note at the top of Shared.slang.
// ---------------------------------------------------------------------------
#ifndef V4_STORE_AABB_SHARED_SLANG
#define V4_STORE_AABB_SHARED_SLANG

#if defined(__cplusplus)
#include <cstdint>
namespace v4
{
using uint = uint32_t;
#endif

// ---------------------------------------------------------------------------
// THE BRICK, AND WHY IT IS EIGHT VOXELS ON A SIDE.
//
// A brick is the unit the hardware sees: one AABB in a bottom-level
// acceleration structure, one procedural primitive, one candidate handed back
// to the shader. Everything inside it is the shader's own business, walked with
// a DDA that the ray tracing cores know nothing about.
//
// So the size is a trade between the two traversals. Bigger bricks mean fewer
// AABBs for the BVH to hold and a cheaper build, and a longer software walk
// inside each one. Smaller bricks mean the BVH skips empty space more finely --
// which is what the hardware is FOR -- and a bigger table.
//
// Eight is where a brick's occupancy mask is exactly sixteen 32-bit words: 512
// bits, one cache line and a half, loaded by the walk a word at a time. It is
// also 0.8 m at this engine's voxel size, which is about the size of the
// features a pine actually has -- a branch, a trunk, a cluster of needles --
// so a brick is usually either mostly empty or mostly full and rarely half.
// ---------------------------------------------------------------------------
// THE SIZE IS ONE NUMBER AND IT IS SWEEPABLE. gpu/store.h passes this straight
// through to the shader as a define, so the two halves cannot disagree about it
// -- which they would the moment somebody edited one and rebuilt only that one.
//
// MEASURED, RTX 4070, the 192 m wood, 1280x720 with Ray Reconstruction, with
// the peel and tight bounds on. `reconstruct` was flat at 1.55-1.59 ms across
// all three, which is what says the machine was quiet enough to believe:
//
//     brick    AABBs     device    trace    frame    startup
//     4^3    1,437,037   103.7 MB  2.80 ms  7.52 ms   7.1 s
//     8^3      343,169    55.7 MB  3.34 ms  8.24 ms   4.2 s     <- default
//    16^3       79,966    60.3 MB  4.86 ms 10.28 ms   3.9 s
//
// 16^3 loses on both counts -- the in-brick walk is up to 46 steps and the
// hardware has too little to cull with. 4^3 is genuinely 16 % faster to trace,
// and pays for it with 1.9x the memory: at that size the 32-byte header and the
// 24-byte AABB are paid 1.4 million times and dominate everything the masks
// cost. 8^3 is the default because a 48 MB and 3-second launch penalty is a bad
// trade for 12 fps in an engine already over 120 -- but on a smaller world, or
// one where the frame matters more than the launch, 2 is the right answer and
// this is the one line to change.
//
// It is a COMPILE-TIME number and cannot be a flag: kBrickWords is the length of
// the mask array in Brick, and the shader indexes the bit layout with it.
#ifndef V4_BRICK_SHIFT
#define V4_BRICK_SHIFT 3
#endif
static const int kBrickShift = V4_BRICK_SHIFT;
static const int kBrickE = 1 << kBrickShift;                   // voxels per edge
static const int kBrickVox = kBrickE * kBrickE * kBrickE;      // 512
static const int kBrickWords = kBrickVox / 32;                 // 16
// A DDA across an 8^3 grid crosses at most 8+8+8-2 = 22 cells. Twenty-four is
// the cap the loop carries, and it exists so a denormal direction cannot turn
// one candidate into an unbounded loop -- which on this driver is a TDR, not a
// slow frame.
static const int kBrickMaxSteps = 3 * kBrickE;

// Bit order inside a brick: x fastest, then y, then z.
//     bit = x | (y << kBrickShift) | (z << 2 * kBrickShift)
// The host packs the mask and the material run in this order and the walk reads
// them in it. It is stated here rather than in either half because it is the
// one thing the two CANNOT disagree about: a mismatch does not fail, it returns
// the wrong voxel's material, which looks like a palette bug.

// ---------------------------------------------------------------------------
// One brick's header. 32 bytes, eight words, all scalars.
// ---------------------------------------------------------------------------
struct V4Brick
{
    // The brick's minimum corner, in WHOLE VOXELS. Integers rather than the
    // AABB's floats because the hit has to name the voxel it landed in as an
    // integer -- see the note on VoxelHit::ijk in Store.slang -- and re-deriving
    // that from a float position makes the surface hash swim under the camera.
    int ox;
    int oy;
    int oz;

    // bit 0        UNIFORM -- every one of its 512 voxels is solid and they
    //              are all the same material, so there is no mask and no
    //              material run, and the AABB slab test IS the intersection.
    // bit 1        MONOCHROME -- it has a mask like any other brick, but every
    //              voxel the mask keeps wears the SAME material, so the run is
    //              not stored and the resolve is this word instead of a
    //              popcount over sixteen mask loads. A third of the detail
    //              bricks in a wood are this: a patch of grass is all one
    //              green id, a stretch of hillside all one litter id, and the
    //              device spreads each over its ramp per voxel anyway.
    // bit 2        HOLDS WATER -- at least one voxel the brick keeps is
    //              mat::WATER. It is not a rendering property; it is what lets
    //              the OCCLUSION walk stay cheap.
    //
    //              Water does not cast a shadow: a shadow ray that stopped on
    //              it left every lake bed in full darkness, which no amount of
    //              caustic on top of the albedo could rescue -- the caustics
    //              multiply a surface the sun was never reaching. So the
    //              occlusion walk has to know which voxels are water, and
    //              resolving a material on every shadow-ray hit in the world to
    //              find that out would pay a popcount over sixteen mask loads
    //              for a wood that is 89 % dry. With this bit only the bricks
    //              that could possibly contain water pay it, and a shadow ray
    //              dying in a canopy never does.
    // bits 8..15   that material, when either of bits 0 and 1 is set.
    uint flags;

    // Where this brick's mask starts, as a uint INDEX into gStoreMasks.
    // Meaningless when uniform.
    //
    // A DETAIL BRICK OWNS kBrickWords + 2 WORDS THERE, not kBrickWords. The two
    // extra hold a POPCOUNT PREFIX -- how many bits are set in everything below
    // word 4, below word 8 and below word 12, as three 16-bit fields:
    //
    //     [kBrickWords + 0]   prefix(4) | prefix(8) << 16
    //     [kBrickWords + 1]   prefix(12)
    //
    // WITHOUT THEM, RESOLVING A MATERIAL WALKS THE WHOLE MASK. The run is
    // compacted by rank, so a voxel's byte sits at mtlBase plus the number of
    // set bits before it -- up to fifteen countbits over fifteen separately
    // loaded words, on every hit. Measured at 4K that was 3.01 ms of an 18.16 ms
    // trace. With the prefix it is one load and at most three more, because the
    // walk only ever has to finish the group of four the voxel lands in.
    //
    // Three fields and not four: the prefix below word 0 is zero.
    uint maskBase;

    // Where this brick's material run starts, as a BYTE index into gStoreMtl.
    //
    // THE RUN HOLDS ONE BYTE PER SOLID VOXEL, NOT PER VOXEL. A brick's
    // materials are stored compacted in bit order, so the byte for a voxel is
    // found by counting the set bits before it in the mask. That is a handful
    // of countbits() in the walk and it is what keeps a surface brick at
    // roughly its occupancy rather than at a flat 512 bytes -- over a wood,
    // about a third of the memory.
    uint mtlBase;

    // THE VOXELS THIS BRICK ACTUALLY KEEPS, as an inclusive box inside it:
    // three bits an axis for the low corner, three for the high.
    //
    //     kBrickShift bits per field, six fields, low corner then high:
    //     loX loY loZ hiX hiY hiZ, least significant first. At 8^3 that is
    //     three bits each and eighteen in all; at 16^3, four and twenty-four.
    //
    // THE AABB IN THE ACCELERATION STRUCTURE IS THIS BOX, NOT THE BRICK. A
    // brick holding three grass voxels in one corner used to hand the hardware
    // a 0.8 m cube a hundred and seventy times bigger than its contents, which
    // is the opposite of what a bounding volume is for. Fitting the box to the
    // contents is most of what makes the structure worth having.
    //
    // The shader needs it as well as the hardware: the hardware uses it to
    // decide whether to offer the candidate at all, and the walk uses it to
    // start inside the occupied part instead of stepping through the margin.
    uint bounds;

    // WHICH OF THE EIGHT HALVES OF THIS BRICK HOLD ANYTHING. One bit per
    // octant, indexed ox | (oy << 1) | (oz << 2).
    //
    // THIS IS WHAT STOPS THE WALK BEING THE WHOLE COST OF THE STORE. Measured
    // by ablation -- committing at the box without walking it -- the hardware's
    // own traversal is 0.65 ms of a 4.34 ms trace and the in-brick DDA is 3.09.
    // Most of that is not finding voxels, it is NOT finding them: inside its
    // tight box a brick is only about a quarter full, so the common case was a
    // ray stepping through twenty-two cells and hitting nothing at all.
    //
    // With these eight bits the walk is two levels. The outer one steps octants
    // -- at most four of them -- and an empty octant costs ONE bit test instead
    // of up to ten cell steps. A candidate that holds nothing on the ray's path
    // now exits after a handful of tests rather than a full traversal.
    //
    // It is free: this word was pad.
    uint octants;
};

// ---------------------------------------------------------------------------
// The store's constants. ONE 16-BYTE ROW.
//
// bind() carries these along with the buffers, which is the whole point of the
// seam -- see the note at the top of gpu/store.h. Nothing outside this backend
// assembles them and nothing outside it knows they exist.
// ---------------------------------------------------------------------------
struct V4StoreConsts
{
    uint brickCount;  // how many entries gStoreBricks holds; diagnostics only
    uint debugMode;   // 0 off, 1 shade the brick AABBs, 2 shade the step count
    uint maxSteps;    // the in-brick walk's cap; kBrickMaxSteps unless swept
    uint pad0;
};

#if defined(__cplusplus)
}  // namespace v4
#endif

#endif  // V4_STORE_AABB_SHARED_SLANG
