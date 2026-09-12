// ---------------------------------------------------------------------------
// expand.h -- packed quads back into the VoxMesh gpu/world.h already builds
// BLASes from.
//
// THIS IS THE SEAM THAT LETS THE BRICK PATH DRIVE THE ENGINE TODAY, and it is
// deliberately the boring half of the rewrite. The 8-byte PackedQuad exists so
// that a compute shader on the far side of the bus can expand it to four
// vertices, and that is where the 19.4x upload saving lives. But the expansion
// is the SAME arithmetic wherever it runs, and doing it on the CPU first buys
// something the shader cannot:
//
//   the brick mesher replaces meshChunk with NO shader work, NO BLAS change
//   and NO change to what crosses the bus -- so if the world comes out wrong,
//   the fault is in the bricks and not in a new GPU path that landed the same
//   day.
//
// The 2x triangle saving is real through this path because it comes from the
// MERGE, not from the packing. Only the upload saving waits for the shader.
//
// ---------------------------------------------------------------------------
// THE WINDING IS COPIED FROM voxelworld.h AND MUST STAY COPIED.
//
// Each of the six cases below is the same vertex order VoxelTerrain's own voxel
// pass emits. That is not stylistic: the tracer derives a geometric normal from
// the triangle, and a face wound the other way is a surface lit from inside --
// it renders black rather than missing, which is the kind of fault that gets
// blamed on a material. tests/voxel_expand_test.cpp checks every quad's
// cross-product against its face:: direction for exactly this reason.
// ---------------------------------------------------------------------------
#pragma once

#include "../scene/voxelworld.h"
#include "quad.h"
#include "store.h"

namespace v2 {
namespace vox {

// ---------------------------------------------------------------------------
// One brick's quads, in world metres.
//
// The two extents come off the quad by the plane table in mesher.h: W lies
// along the ROW axis and H along the BIT axis, and which world axis each of
// those is depends on the direction. Getting this pair swapped transposes the
// world in a way that looks like a broken terrain generator, so it is written
// out per direction rather than folded into a table.
// ---------------------------------------------------------------------------
inline void expandBrick(const Brick &b, int bx, int by, int bz, VoxMesh *out) {
    const float s = VOXEL_M;
    const int i0 = bx * BRICK_VOX, j0 = bz * BRICK_VOX, k0 = by * BRICK_VOX;

    for (PackedQuad q : b.quads) {
        const uint8_t dir = quadDir(q), mtl = quadMaterial(q), str = quadStrand(q);
        const int w = quadW(q), h = quadH(q);
        const int lx = quadX(q), ly = quadY(q), lz = quadZ(q);

        int spanX = 1, spanY = 1, spanZ = 1;
        switch (dir) {
            case face::POS_Y:
            case face::NEG_Y: spanZ = w; spanX = h; break;
            case face::POS_X:
            case face::NEG_X: spanZ = w; spanY = h; break;
            default: spanX = w; spanY = h; break;  // POS_Z, NEG_Z
        }

        const float x0 = float(i0 + lx) * s, x1 = x0 + float(spanX) * s;
        const float y0 = float(k0 + ly) * s, y1 = y0 + float(spanY) * s;
        const float z0 = float(j0 + lz) * s, z1 = z0 + float(spanZ) * s;

        switch (dir) {
            case face::POS_Y:
                out->addQuad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, mtl, dir, str);
                break;
            case face::NEG_Y:
                out->addQuad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, mtl, dir, str);
                break;
            case face::POS_X:
                out->addQuad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, mtl, dir, str);
                break;
            case face::NEG_X:
                out->addQuad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, mtl, dir, str);
                break;
            case face::POS_Z:
                out->addQuad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, mtl, dir, str);
                break;
            default:
                out->addQuad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, mtl, dir, str);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Every resident brick of one chunk, as a single VoxMesh -- a drop-in for what
// VoxelTerrain::meshChunk returns.
//
// THE SEARCH RANGE IS THE WORLD'S, NOT A GUESS. Bricks exist only where the
// surface test kept one or an edit created one, and both are sparse; walking
// the full signed range would be 2 million lookups a chunk. The store is asked
// for its own vertical extent instead.
// ---------------------------------------------------------------------------
inline VoxMesh expandChunk(const BrickStore &store, int cx, int cz) {
    VoxMesh m;
    int byLo, byHi;
    if (!store.chunkYRange(cx, cz, &byLo, &byHi)) return m;

    // Measured at roughly 1.5 quads a column on this terrain before the merge;
    // after it a chunk is about 34k quads, so this is the observed size rather
    // than a doubling guess.
    m.position.reserve(size_t(34000) * 4);
    m.index.reserve(size_t(34000) * 6);
    m.tri.reserve(size_t(34000) * 2);

    for (int sz = 0; sz < CHUNK_BRICKS; ++sz)
        for (int sx = 0; sx < CHUNK_BRICKS; ++sx)
            for (int by = byLo; by <= byHi; ++by) {
                const int bx = cx * CHUNK_BRICKS + sx, bz = cz * CHUNK_BRICKS + sz;
                if (const Brick *b = store.find(bx, by, bz)) expandBrick(*b, bx, by, bz, &m);
            }
    return m;
}

}  // namespace vox
}  // namespace v2
