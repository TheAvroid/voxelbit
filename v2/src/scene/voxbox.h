// ---------------------------------------------------------------------------
// A PATCH OF VOXELS, AS BOXES A SOLVER CAN HOLD.
//
// THE WHOLE PROBLEM THIS SOLVES. A chunk knocked out of a boulder is a rigid
// body standing in the hole it just made, and for it to behave like one the
// solver has to be given a shape that HAS that hole. It never has been:
//
//   * a convex hull cannot have a dent at all -- a 30 cm bite out of a 20 m
//     boulder leaves the hull byte-identical -- so the piece is born "inside"
//     solid stone and grinds down through it. That is four rewrites of this
//     feature, and every one of them was arguing with the shape rather than
//     fixing it.
//   * a height field is 2.5D. It has the hole when the bite is off the top and
//     does not when the bite is under an overhang, which is worse than either
//     answer consistently.
//   * the whole model as a triangle mesh is 1.08 million triangles for one
//     boulder, and it would have to be re-cooked on every blow.
//
// So: a WINDOW. A few metres cube around wherever the piece is, sampled off the
// same voxels everything else in this engine now asks -- the damaged instance's
// own array, so the hole is really in it -- and greedily merged into boxes.
// PhysX takes boxes with no cooking at all, the union of them is EXACTLY the
// solid voxels, and a body cannot tunnel through a solid because the interior
// merges into big boxes rather than a shell.
//
// GREEDY, IN THREE PASSES. Run along X while the cells stay solid and unclaimed;
// widen that run along Z while whole rows match; then raise the slab along Y
// while whole layers match. A boulder's interior collapses into a handful of
// large boxes and only its surface costs anything, which is what makes the
// count affordable: a 32-cell window through the face of a big rock is a few
// hundred boxes rather than the sixteen thousand cells it holds.
//
// EXACT, and that is testable without a device -- see scratch's window_probe.
// The union of the boxes equals the solid set, they never overlap, and no box
// covers a cell that is not solid. A collider that is merely approximately the
// rock is how a piece ends up half-buried in a face that looks flat.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"

#include <cstdint>
#include <vector>

namespace v2 {

// One box, in world metres: centre and half extents. What PxBoxGeometry wants.
struct VoxBox {
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
};

// `solid(i, j, k)` is asked for window-local cell indices, i in [0,nx) and so
// on. Cell (i,j,k) covers world [origin + (i,j,k)*voxel, + voxel).
//
// `maxBoxes` is a ceiling, not a target: a window that would need more than
// that is one the caller has sized wrong, and quietly handing PhysX ten
// thousand shapes would be worse than saying so. Returns false if it hit it.
template <class SolidCell>
inline bool greedyBoxes(int nx, int ny, int nz, const Vec3 &origin, float voxel, SolidCell solid,
                        std::vector<VoxBox> *out, size_t maxBoxes = 4096) {
    out->clear();
    if (nx <= 0 || ny <= 0 || nz <= 0) return true;

    const size_t plane = size_t(nx) * size_t(nz);
    std::vector<uint8_t> filled(plane * size_t(ny), 0);
    std::vector<uint8_t> taken(plane * size_t(ny), 0);
    auto ix = [&](int i, int j, int k) {
        return size_t(i) + size_t(k) * size_t(nx) + size_t(j) * plane;
    };

    // Asked once per cell and then only read: `solid` reaches into a voxel
    // array through a transform, and a greedy merge asks about the same cell
    // many times over.
    for (int j = 0; j < ny; ++j)
        for (int k = 0; k < nz; ++k)
            for (int i = 0; i < nx; ++i)
                filled[ix(i, j, k)] = solid(i, j, k) ? uint8_t(1) : uint8_t(0);

    auto free_ = [&](int i, int j, int k) {
        const size_t p = ix(i, j, k);
        return filled[p] && !taken[p];
    };

    for (int j = 0; j < ny; ++j) {
        for (int k = 0; k < nz; ++k) {
            for (int i = 0; i < nx; ++i) {
                if (!free_(i, j, k)) continue;

                // ---- 1. as far along X as it stays solid -------------------
                int i1 = i;
                while (i1 + 1 < nx && free_(i1 + 1, j, k)) ++i1;

                // ---- 2. as wide along Z as WHOLE ROWS match ----------------
                int k1 = k;
                for (bool go = true; go && k1 + 1 < nz;) {
                    for (int t = i; t <= i1; ++t)
                        if (!free_(t, j, k1 + 1)) { go = false; break; }
                    if (go) ++k1;
                }

                // ---- 3. as tall along Y as WHOLE LAYERS match --------------
                int j1 = j;
                for (bool go = true; go && j1 + 1 < ny;) {
                    for (int kk = k; kk <= k1 && go; ++kk)
                        for (int t = i; t <= i1; ++t)
                            if (!free_(t, j1 + 1, kk)) { go = false; break; }
                    if (go) ++j1;
                }

                for (int jj = j; jj <= j1; ++jj)
                    for (int kk = k; kk <= k1; ++kk)
                        for (int t = i; t <= i1; ++t) taken[ix(t, jj, kk)] = 1;

                VoxBox b;
                b.hx = 0.5f * float(i1 - i + 1) * voxel;
                b.hy = 0.5f * float(j1 - j + 1) * voxel;
                b.hz = 0.5f * float(k1 - k + 1) * voxel;
                b.cx = origin.x + (float(i) * voxel) + b.hx;
                b.cy = origin.y + (float(j) * voxel) + b.hy;
                b.cz = origin.z + (float(k) * voxel) + b.hz;
                out->push_back(b);
                if (out->size() >= maxBoxes) return false;

                i = i1;   // nothing left to start from before the run ends
            }
        }
    }
    return true;
}


}   // namespace v2
