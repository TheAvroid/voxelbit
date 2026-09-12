// ---------------------------------------------------------------------------
// mesher.h -- one brick of 64^3 voxels turned into merged, packed quads.
//
// This is the replacement for VoxelTerrain::meshChunk, and the whole reason
// v4 exists. Two things changed and they are independent:
//
//   THE UNIT. meshChunk built 256 x 256 columns of the full world height and
//   was the smallest thing an edit could invalidate. This builds 64^3 and
//   nothing here knows about chunks at all.
//
//   THE METHOD. meshChunk walked voxels. This walks BITS: a column is a
//   uint64, and the six face masks for all 64 of its voxels fall out of six
//   bitwise operations. Face extraction stops scaling with the number of
//   voxels and starts scaling with the number of columns.
//
// ---------------------------------------------------------------------------
// THE PLANE TABLE, WHICH IS THE CONTRACT WITH THE GPU EXPANDER.
//
// Every face direction is meshed as 64 parallel PLANES. Within a plane the
// quads live on a 2D grid whose two axes are called ROW and BIT -- bit because
// that axis is the one carried in the bits of a uint64, which is what makes
// the merge along it a shift instead of a loop.
//
//   dir            plane axis   row axis   bit axis
//   POS_Y / NEG_Y      Y            Z          X
//   POS_X / NEG_X      X            Z          Y
//   POS_Z / NEG_Z      Z            X          Y
//
// A packed quad stores W along its ROW axis and H along its BIT axis. Anything
// that decodes a quad -- the expansion compute shader above all -- must use
// this same table or the world comes out transposed in a way that looks like
// the terrain generator is broken rather than the decoder.
//
// The Y rows are the only ones needing a transpose to build (bits run along X
// while the source columns carry bits along Y); the X and Z planes fall out of
// the column layout directly. See buildYPlanes.
//
// ---------------------------------------------------------------------------
// WHAT IS DELIBERATELY *NOT* TAKEN FROM OMAR OWIS' MESHER.
//
// His splits the face masks by EXACT COLOUR before merging, so two voxels of
// slightly different green never share a quad. That is correct for his engine
// and would be a measured regression here.
//
// v2 stores a material FAMILY on the quad -- kGrass0, kSoil0, kLitter0, or
// rock -- and groundShade() in the tracer picks the exact shade per voxel by
// hashing the voxel coordinate on the device. voxelworld.h records what
// splitting instead costs: the strands alone go from 51k triangles a chunk to
// 187k, "a 2.2x on the whole floor to change a colour".
//
// So the merge rule here is EQUALITY OF THE STORED MATERIAL BYTE, and the
// caller is expected to hand back family bases rather than final shades. Get
// that wrong in the column source and the quad count is what tells you.
// ---------------------------------------------------------------------------
#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <vector>

#include "quad.h"

namespace v2 {
namespace vox {

// v2's face:: values, restated here so this header stands alone and stays
// GPU-free. voxelworld.h is the original definition and they must agree.
namespace mdir {
constexpr uint8_t POS_Y = 0, NEG_Y = 1, POS_X = 2, NEG_X = 3, POS_Z = 4, NEG_Z = 5;
}

// ---------------------------------------------------------------------------
// The scratch one worker meshes through -- ONE PER WORKER, never one per
// brick, for exactly the reason ChunkScratch gives in voxelworld.h: the
// allocation is not the expensive part, the value-initialisation of memory
// that is about to be overwritten is.
//
// 196 KB, which sits in L2 and is walked linearly.
// ---------------------------------------------------------------------------
struct MeshScratch {
    // 6 directions x 64 planes x 64 rows of 64 bits.
    Column plane[6][BRICK_VOX][BRICK_VOX];
    // The vertical face masks before they are transposed into the Y planes.
    // Here rather than on the stack because they are 32 KB each and a worker
    // thread gets 1 MB by default -- and because, like everything else in this
    // struct, they are fully written before they are read on every brick.
    Column up[BRICK_VOX][BRICK_VOX];    // [z][x], bits are y
    Column down[BRICK_VOX][BRICK_VOX];  // [z][x], bits are y

    // Only the planes need clearing: up and down are assigned on every column
    // on every path, including the empty-column one.
    void clear() { std::memset(plane, 0, sizeof(plane)); }
};

// ---------------------------------------------------------------------------
// What the mesher needs from the world, and nothing more.
//
// A source is asked for PADDED columns: lx and lz run -1..64, so a face on the
// brick boundary can ask the neighbouring brick whether it is covered. Without
// that the six sides of every brick would be emitted as walls, and because
// both neighbours would do it the geometry would be doubled there too --
// v2 hit this and the note over meshChunk still describes it.
//
// The Y direction is padded differently, and it has to be: a column IS the
// brick's height, so there is no room in the uint64 for a border row. Instead
// the source answers two single-bit questions -- solidAbove and solidBelow --
// which is all the y-face masks need.
//
// ---------------------------------------------------------------------------
#if defined(__cpp_concepts)
template <typename S>
concept ColumnSource = requires(const S &s, int a, int b, int c) {
    { s.column(a, b) } -> std::convertible_to<Column>;
    { s.occluder(a, b) } -> std::convertible_to<Column>;
    { s.solidAbove(a, b) } -> std::convertible_to<bool>;
    { s.solidBelow(a, b) } -> std::convertible_to<bool>;
    { s.material(a, b, c) } -> std::convertible_to<uint8_t>;
    { s.strand(a, b, c) } -> std::convertible_to<uint8_t>;
};
#define V4_COLUMN_SOURCE ColumnSource
#else
#define V4_COLUMN_SOURCE typename
#endif

// ---------------------------------------------------------------------------
// The six face masks for one column, from bit operations alone.
//
// For a direction d, bit y of the result is set when voxel y is solid AND its
// neighbour in d is not. That is the rule the whole engine runs on -- "emit a
// face where solid meets air" -- expressed 64 voxels at a time.
//
// The two vertical cases carry the only subtlety. "The voxel above y is solid"
// is (col >> 1) as a mask indexed by y, which reads zero at y = 63 where the
// answer actually lives in the brick above; the aboveBit term puts it back.
// Symmetrically at y = 0 for the downward faces. Dropping either one is a
// horizontal seam of doubled geometry at every brick boundary -- the same
// defect the XZ padding exists to prevent, in the axis that cannot be padded.
// ---------------------------------------------------------------------------
//
// THE OCCLUDER IS A SEPARATE ARGUMENT HERE FOR THE SAME REASON IT IS ON THE
// SIDES, and forgetting it on this axis is a bug that hides well: the side
// faces come out right, the lake surface comes out right, and every water
// voxel lying on the bed quietly emits a DOWNWARD face into the rock it is
// resting on. 63,927 of them on one lake chunk, all invisible from above.
//
// `col` says which voxels are mine. `occ` says what hides them. For terrain
// the caller passes the same column twice and nothing changes.
inline Column faceUp(Column col, Column occ, bool aboveSolid) {
    const Column above = (occ >> 1) | (aboveSolid ? (Column(1) << (BRICK_VOX - 1)) : 0);
    return col & ~above;
}
inline Column faceDown(Column col, Column occ, bool belowSolid) {
    const Column below = (occ << 1) | (belowSolid ? Column(1) : 0);
    return col & ~below;
}
inline Column faceSide(Column col, Column neighbour) { return col & ~neighbour; }

// ---------------------------------------------------------------------------
// The greedy merge, over one plane of face bits.
//
// `mask` is 64 rows of 64 bits and is CONSUMED: every bit that leaves as part
// of a quad is cleared, which is what stops a voxel being emitted twice.
//
// The two extents are found differently, and deliberately:
//
//   H, along the bit axis, comes from the bits -- countr_zero finds the start
//   of a run and the run is then walked while the material holds. A ten-voxel
//   drop costs one quad per band rather than ten.
//
//   W, along the row axis, comes from comparing whole rows: the next row must
//   carry the same span of bits AND the same material across it. This is where
//   a flat floor collapses from 4,096 quads to one.
//
// THE MATERIAL TEST IS INLINE RATHER THAN A PRE-SPLIT BY MATERIAL. Omar's
// mesher builds a hash map of colour -> mask per axis, which costs a map
// allocation and a full second pass over the faces. Testing as we merge costs
// one lookup per FACE voxel, and face voxels are a few percent of a brick.
// ---------------------------------------------------------------------------
// THE MERGE KEY IS (MATERIAL, STRAND), NOT MATERIAL ALONE.
//
// A grass blade carries the row it stands on in three bits, and the device
// turns height above that row into a shade -- dark at the soil, light at the
// tip. Two blades standing on DIFFERENT rows therefore want different keys
// even where they share a material: merge them and the whole quad takes the
// first one's base row, so a blade on a step gets its neighbour's gradient.
//
// It is one 16-bit compare instead of an 8-bit one, and for terrain and water
// the strand half is always zero, so nothing else changes. This is NOT the
// same thing as splitting by exact colour, which voxelworld.h measured at 2.2x
// the triangles -- the shade still comes from hashing the voxel on device.
template <typename MatFn, typename EmitFn>
inline void mergePlane(Column *mask, int planeIdx, MatFn material, EmitFn emit) {
    for (int row = 0; row < BRICK_VOX; ++row) {
        Column m = mask[row];
        while (m != 0) {
            const int bit = ctz64(m);
            const uint16_t mtl = material(planeIdx, row, bit);

            // --- H: extend along the bit axis while set and same material ---
            int h = 1;
            while (bit + h < BRICK_VOX && ((mask[row] >> (bit + h)) & 1) != 0 &&
                   material(planeIdx, row, bit + h) == mtl)
                ++h;

            const Column span = (h >= BRICK_VOX) ? COL_FULL : ((Column(1) << h) - 1);
            const Column spanAt = span << bit;

            // --- W: extend across rows while the whole span matches ---
            int w = 1;
            while (row + w < BRICK_VOX) {
                if ((mask[row + w] & spanAt) != spanAt) break;
                bool same = true;
                for (int k = 0; k < h; ++k)
                    if (material(planeIdx, row + w, bit + k) != mtl) { same = false; break; }
                if (!same) break;
                ++w;
            }

            for (int r = row; r < row + w; ++r) mask[r] &= ~spanAt;

            emit(planeIdx, row, bit, w, h, mtl);
            m = mask[row];
        }
    }
}

// ---------------------------------------------------------------------------
// Turn a (plane, row, bit) triple into brick-local (x, y, z) for a direction.
// The inverse of this lives in the expansion shader; see the plane table at
// the top of the file.
// ---------------------------------------------------------------------------
inline void planeToLocal(uint8_t dir, int plane, int row, int bit, int *x, int *y, int *z) {
    switch (dir) {
        case mdir::POS_Y:
        case mdir::NEG_Y: *x = bit; *y = plane; *z = row; break;
        case mdir::POS_X:
        case mdir::NEG_X: *x = plane; *y = bit; *z = row; break;
        default:  // POS_Z, NEG_Z
            *x = row; *y = bit; *z = plane; break;
    }
}

// ---------------------------------------------------------------------------
// Mesh one brick.
//
// Returns quads in brick-local coordinates; the caller adds the brick origin.
// Keeping them local is what lets an identical brick anywhere in the world
// share one cluster in the GPU cache -- a wall of stone is the same wall
// wherever it is built, and this is the property that makes that exploitable
// later.
// ---------------------------------------------------------------------------
template <V4_COLUMN_SOURCE S>
inline void meshBrick(const S &src, MeshScratch &scratch, std::vector<PackedQuad> *out) {
    scratch.clear();

    // ---- FACE HULLING -----------------------------------------------------
    //
    // One pass over the brick's 4,096 columns. Each produces six masks, and
    // four of them land in a plane array directly because their bit axis is
    // already Y. The two vertical ones need transposing and are handled below,
    // where the sparsity pays: a terrain column has exactly one up-face.
    for (int z = 0; z < BRICK_VOX; ++z) {
        for (int x = 0; x < BRICK_VOX; ++x) {
            const Column col = src.column(x, z);
            if (col == COL_EMPTY) {
                scratch.up[z][x] = 0;
                scratch.down[z][x] = 0;
                continue;
            }
            const Column occ = src.occluder(x, z);
            scratch.up[z][x] = faceUp(col, occ, src.solidAbove(x, z));
            scratch.down[z][x] = faceDown(col, occ, src.solidBelow(x, z));

            // OCCLUDER, NOT COLUMN, AND THE TWO DIFFER ONLY FOR WATER.
            //
            // A face is emitted where this source's own voxels meet something
            // that does not HIDE them, and "does not hide them" is not always
            // "is not one of mine". Terrain must show a face against water or
            // the lake bed is invisible from above; water must NOT show one
            // against terrain, or every lake carries a second skin inside its
            // own bed, z-fighting with it.
            //
            // So the source answers two questions: what it is made of, and
            // what covers it. For terrain the two are the same object and this
            // costs nothing -- see TerrainColumns::occluder.
            // plane = x, row = z, bit = y
            scratch.plane[mdir::POS_X][x][z] = faceSide(col, src.occluder(x + 1, z));
            scratch.plane[mdir::NEG_X][x][z] = faceSide(col, src.occluder(x - 1, z));
            // plane = z, row = x, bit = y
            scratch.plane[mdir::POS_Z][z][x] = faceSide(col, src.occluder(x, z + 1));
            scratch.plane[mdir::NEG_Z][z][x] = faceSide(col, src.occluder(x, z - 1));
        }
    }

    // ---- THE Y TRANSPOSE --------------------------------------------------
    //
    // Vertical faces arrive as "column (x, z), bits are y" and the merge wants
    // "plane y, row z, bits are x". Walking the set bits with countr_zero
    // rather than all 64 is what makes this cheap: on open terrain there is
    // one up-face per column, so this loop runs 4,096 times for a brick with
    // 262,144 voxels in it.
    for (int z = 0; z < BRICK_VOX; ++z)
        for (int x = 0; x < BRICK_VOX; ++x) {
            Column u = scratch.up[z][x];
            while (u) {
                const int y = ctz64(u);
                u &= u - 1;
                scratch.plane[mdir::POS_Y][y][z] |= Column(1) << x;
            }
            Column d = scratch.down[z][x];
            while (d) {
                const int y = ctz64(d);
                d &= d - 1;
                scratch.plane[mdir::NEG_Y][y][z] |= Column(1) << x;
            }
        }

    // ---- GREEDY MERGE, SIX DIRECTIONS -------------------------------------
    for (uint8_t dir = 0; dir < 6; ++dir) {
        for (int plane = 0; plane < BRICK_VOX; ++plane) {
            Column *rows = scratch.plane[dir][plane];

            // A plane with nothing in it is the overwhelmingly common case --
            // a brick of open terrain has one populated Y plane out of 64 --
            // so it is worth the scan to skip mergePlane entirely.
            bool any = false;
            for (int r = 0; r < BRICK_VOX; ++r)
                if (rows[r]) { any = true; break; }
            if (!any) continue;

            // Material in the low byte, strand code in the high one -- see
            // the note over mergePlane.
            auto matAt = [&](int pl, int row, int bit) -> uint16_t {
                int x, y, z;
                planeToLocal(dir, pl, row, bit, &x, &y, &z);
                return uint16_t(src.material(x, y, z)) |
                       (uint16_t(src.strand(x, y, z)) << 8);
            };
            auto emit = [&](int pl, int row, int bit, int w, int h, uint16_t key) {
                int x, y, z;
                planeToLocal(dir, pl, row, bit, &x, &y, &z);
                out->push_back(packQuad(x, y, z, w, h, dir, uint8_t(key & 0xFF),
                                        uint8_t(key >> 8)));
            };
            mergePlane(rows, plane, matAt, emit);
        }
    }
}

}  // namespace vox
}  // namespace v2
