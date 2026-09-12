// ---------------------------------------------------------------------------
// quad.h -- the brick, and the 64-bit word one merged face is stored in.
//
// This file is the boundary between the new voxel stack and everything v2
// already had. Nothing above it knows how a voxel is stored; nothing below it
// knows what a BLAS is.
//
// ---------------------------------------------------------------------------
// WHY A BRICK IS 64 VOXELS AND A CHUNK IS STILL 256.
//
// v2 had ONE unit: a 256 x 256 column chunk, keyed in 2D, spanning the whole
// height of the world. That single choice is what made editing expensive --
// digging one voxel re-meshed 16.7 million of them and rebuilt a bottom-level
// structure -- and it is the thing this engine exists to fix.
//
// So the unit is split in two, and the two are chosen to serve different
// masters:
//
//   BRICK  64^3 voxels, 6.4 m.   The DIRTY unit. Small enough that a re-mesh
//                                after an axe blow is a fraction of a
//                                millisecond, and sized so one of its columns
//                                is exactly one uint64 -- see below. That is
//                                not a coincidence, it is the whole design.
//
//   CHUNK  4x4x4 bricks, 25.6 m. The STRUCTURE unit, and deliberately the same
//                                25.6 m v2 already used, so the chunk ring,
//                                the streamer and the top-level structure in
//                                gpu/world.h keep their tuning. What changes
//                                is that the key gains a Y.
//
// A brick is one cluster (a CLAS) and a chunk is one bottom-level structure
// assembled from its bricks. Replacing one brick after an edit re-instantiates
// the chunk rather than rebuilding it from loose triangles, which is precisely
// what gpu/clusters.h was written for and never got to do.
//
// ---------------------------------------------------------------------------
// THE COLUMN IS A BITMASK, AND THAT IS THE ONE IDEA WORTH TAKING WHOLE FROM
// OMAR OWIS' ENGINE.
//
// A column of 64 voxels is a uint64: bit y is set when that voxel is solid.
// Face extraction along a column then stops being a loop and becomes an AND:
//
//     up   = col & ~(col << 1)      every voxel whose neighbour above is air
//     down = col & ~(col >> 1)
//     posX = col & ~columnOfNeighbourInPosX
//
// Sixty-four voxels of face detection per instruction, on every axis. v2's
// mesher walked columns a voxel at a time and asked six questions at each one.
//
// THE PART HIS ENGINE DOES NOT HAVE, and the reason this fits voxelbit at all:
// a procedural terrain column of height h IS the bitmask (1 << h) - 1. It
// costs nothing to store because it is not stored. v2's world is a function of
// (i, j) and stays one -- 10 cm voxels across a 300 m view is 2 x 10^11 voxels
// and no chunk format in existence makes that dense. Only edits and assets
// become real bits. See columns.h.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#if defined(__has_include)
#if __has_include(<bit>)
#include <bit>
#endif
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace v2 {
namespace vox {

// One brick on a side. 64 because a column is then exactly a uint64; every
// other number in this file follows from that one.
constexpr int BRICK_VOX = 64;
// Bricks per chunk on a side, so a chunk stays the 25.6 m everything else in
// the engine was tuned against.
constexpr int CHUNK_BRICKS = 4;
constexpr int BRICK_COLS = BRICK_VOX * BRICK_VOX;  // 4096 columns in a brick

// A column of 64 voxels. Bit y is the voxel at local height y.
using Column = uint64_t;

// The whole column, and nothing in it. Named because ~Column(0) reads as a bit
// trick at the call site and COL_FULL reads as what it means.
constexpr Column COL_EMPTY = 0;
constexpr Column COL_FULL = ~Column(0);

// ---------------------------------------------------------------------------
// COUNT TRAILING ZEROS -- AND IT CANNOT BE std::countr_zero.
//
// THIS HEADER IS COMPILED TWO WAYS AND ONLY ONE OF THEM IS C++20. The GPU-free
// suite in tests/ builds with g++ at -std=c++20, where <bit> has countr_zero.
// The ENGINE builds through Falcor, whose CMakeLists pins **cxx_std_17** -- so
// in the build that actually ships, std::countr_zero does not exist.
//
// That gap is invisible to every test here, which is exactly how it reached a
// build: six GPU-free suites passing at C++20 while the engine would not
// compile at all. mesher.h already guards its concept behind __cpp_concepts for
// the same reason; this is the other half nobody wrote.
//
// tests/ is built at BOTH standards now -- see checkstd in the suite notes --
// so a C++20-only construct fails on the harness rather than on a four-gigabyte
// link.
// ---------------------------------------------------------------------------
inline int ctz64(uint64_t v) {
#if defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L
    return std::countr_zero(v);
#elif defined(_MSC_VER)
    unsigned long i = 0;
    return _BitScanForward64(&i, v) ? int(i) : 64;
#else
    return v ? __builtin_ctzll(v) : 64;
#endif
}

// A column solid from its floor up to (but not including) local height n.
//
// The two early returns are not defensive padding. `Column(1) << 64` is
// undefined behaviour in C++, and on x86 the shift count is taken mod 64 -- so
// the full-column case would silently return 1, which is a single voxel of
// bedrock where a solid brick was meant. It would look like a hole in the
// world and read like a meshing bug.
inline Column columnUpTo(int n) {
    if (n <= 0) return COL_EMPTY;
    if (n >= BRICK_VOX) return COL_FULL;
    return (Column(1) << n) - 1;
}

// A column solid over the INCLUSIVE local range [lo, hi], clipped to the brick.
// Water needs this where terrain needs only columnUpTo: a lake starts at the
// bed and stops at the line, so neither end is the floor of the brick.
//
// The full-width case is guarded for the same reason columnUpTo guards it --
// `Column(1) << 64` is undefined and x86 takes the shift mod 64, which would
// turn a brick full of water into a single voxel of it.
inline Column columnRange(int lo, int hi) {
    if (hi < 0 || lo >= BRICK_VOX || lo > hi) return COL_EMPTY;
    if (lo < 0) lo = 0;
    if (hi >= BRICK_VOX) hi = BRICK_VOX - 1;
    const int n = hi - lo + 1;
    if (n >= BRICK_VOX) return COL_FULL;
    return ((Column(1) << n) - 1) << lo;
}

// ---------------------------------------------------------------------------
// THE PACKED QUAD.
//
//     bits  0.. 5   x         quad origin in the brick, 0..63
//     bits  6..11   y
//     bits 12..17   z
//     bits 18..23   w - 1     extent along the face's first axis, 1..64
//     bits 24..29   h - 1     extent along its second, 1..64
//     bits 30..32   dir       a face:: value, 0..5
//     bits 33..40   material  a mat:: id, 0..255
//     bits 41..44   strand    v2's strand code, 0..15
//     bits 45..63   spare
//
// EIGHT BYTES A QUAD, against the seventy-six v2 uploaded. v2's VoxMesh stored
// four Vec3 positions (48 bytes), six 32-bit indices (24) and two packed
// triangle words (4) for every merged face, all built on the CPU and copied
// across. Here the CPU produces this word and a compute shader expands it to
// four vertices on the far side. That is a 9.5x cut in transfer for identical
// geometry, and it is the second-largest win in the rewrite after the dirty
// unit shrinking.
//
// W AND H ARE STORED BIASED BY ONE. A run can be the full 64 wide, which does
// not fit in six bits, and a run can never be zero wide -- so the useful range
// is 1..64 and it is kept as 0..63.
//
// THE DIRECTION IS v2's face:: VALUE, NOT OMAR'S. His engine numbers its axes
// 0..5 in its own order. Adopting that would mean touching faceNormal() in
// voxelworld.h, faceNormal() in Trace.cs.slang, and -- the one that would have
// hurt -- VoxelKey.slang, which is the single definition of what a surface IS
// for SHaRC, the world-space ReSTIR reservoirs and the neural cache. Those
// three have to agree about a face or they disagree about the world without
// any of them being wrong. The mesher is new; the meaning of "face 2" is not.
// ---------------------------------------------------------------------------
using PackedQuad = uint64_t;

constexpr int QUAD_X_SHIFT = 0, QUAD_Y_SHIFT = 6, QUAD_Z_SHIFT = 12;
constexpr int QUAD_W_SHIFT = 18, QUAD_H_SHIFT = 24, QUAD_DIR_SHIFT = 30;
constexpr int QUAD_MAT_SHIFT = 33, QUAD_STRAND_SHIFT = 41;

constexpr uint64_t QUAD_POS_MASK = 0x3F;  // 6 bits -- x, y, z, and the biased w and h
constexpr uint64_t QUAD_DIR_MASK = 0x07;
constexpr uint64_t QUAD_MAT_MASK = 0xFF;
constexpr uint64_t QUAD_STRAND_MASK = 0x0F;

inline PackedQuad packQuad(int x, int y, int z, int w, int h, uint8_t dir, uint8_t material,
                           uint8_t strand = 0) {
    return (uint64_t(uint32_t(x) & QUAD_POS_MASK) << QUAD_X_SHIFT) |
           (uint64_t(uint32_t(y) & QUAD_POS_MASK) << QUAD_Y_SHIFT) |
           (uint64_t(uint32_t(z) & QUAD_POS_MASK) << QUAD_Z_SHIFT) |
           (uint64_t(uint32_t(w - 1) & QUAD_POS_MASK) << QUAD_W_SHIFT) |
           (uint64_t(uint32_t(h - 1) & QUAD_POS_MASK) << QUAD_H_SHIFT) |
           (uint64_t(dir & QUAD_DIR_MASK) << QUAD_DIR_SHIFT) |
           (uint64_t(material) << QUAD_MAT_SHIFT) |
           (uint64_t(strand & QUAD_STRAND_MASK) << QUAD_STRAND_SHIFT);
}

inline int quadX(PackedQuad q) { return int((q >> QUAD_X_SHIFT) & QUAD_POS_MASK); }
inline int quadY(PackedQuad q) { return int((q >> QUAD_Y_SHIFT) & QUAD_POS_MASK); }
inline int quadZ(PackedQuad q) { return int((q >> QUAD_Z_SHIFT) & QUAD_POS_MASK); }
inline int quadW(PackedQuad q) { return int((q >> QUAD_W_SHIFT) & QUAD_POS_MASK) + 1; }
inline int quadH(PackedQuad q) { return int((q >> QUAD_H_SHIFT) & QUAD_POS_MASK) + 1; }
inline uint8_t quadDir(PackedQuad q) { return uint8_t((q >> QUAD_DIR_SHIFT) & QUAD_DIR_MASK); }
inline uint8_t quadMaterial(PackedQuad q) {
    return uint8_t((q >> QUAD_MAT_SHIFT) & QUAD_MAT_MASK);
}
inline uint8_t quadStrand(PackedQuad q) {
    return uint8_t((q >> QUAD_STRAND_SHIFT) & QUAD_STRAND_MASK);
}

// ---------------------------------------------------------------------------
// Brick coordinates, and the 64-bit key the GPU mesh cache files them under.
//
// 21 bits a component, signed, which is +-1,048,576 bricks -- 6,710 km each
// way at 6.4 m a brick. The world runs out of float precision a long time
// before it runs out of key.
//
// SAME LAYOUT AS ChunkEdits::vkey IN voxelworld.h, deliberately. v2 already
// packs three signed coordinates into 63 bits exactly this way, and two
// different packings of one idea inside a single engine is how a cache and its
// owner come to quietly disagree about which brick they are talking about.
// ---------------------------------------------------------------------------
inline uint64_t brickKey(int bx, int by, int bz) {
    return (uint64_t(uint32_t(bx) & 0x1FFFFFu) << 42) |
           (uint64_t(uint32_t(by) & 0x1FFFFFu) << 21) | uint64_t(uint32_t(bz) & 0x1FFFFFu);
}

}  // namespace vox
}  // namespace v2
