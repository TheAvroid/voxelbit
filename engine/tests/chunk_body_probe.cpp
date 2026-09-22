// ---------------------------------------------------------------------------
// chunk_body_probe.cpp -- what does building ONE debris body cost on the CPU?
//
//   g++ -std=c++17 -O2 -I src tests/chunk_body_probe.cpp -o build/chunkbody.exe
//   ./build/chunkbody.exe [pieces] [voxelsPerPiece]
//
// WHY. "theres bad lag" when a tree breaks (user 2026-09-22). The partition is
// off the frame now, so what is left is the DRAIN -- makeLooseBody, called
// kShatterPerFrame times a frame until the batch is built. Four things happen
// in there and only two of them need the render thread:
//
//     meshVolume     pure CPU   <- could move to the shatter worker
//     recordLooseBuild  GPU     <- must stay
//     greedyBoxes    pure CPU   <- could move
//     addCompoundBody  PhysX    <- must stay
//
// This times the two that could move, on a chunk the size the oak actually
// produces, so the decision is made on a number rather than on a guess about
// which half is expensive. Both are free functions in headers that do not pull
// Falcor, which is why this can be a one-second harness.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "world/voxelworld.h"
#include "voxel/voxbox.h"

using namespace v2;

int main(int argc, char **argv) {
    const int pieces = argc > 1 ? atoi(argv[1]) : 448;
    const int want = argc > 2 ? atoi(argv[2]) : 1032;

    // A PIECE THE SHAPE THE FLOOD MAKES, not a solid cube: the shatter report
    // says the oak's pieces run 204..2289 voxels with a longest edge of 36, so
    // a compact blob a bit under half solid is the honest stand-in. A solid
    // cube would UNDERSTATE the mesh, which is driven by exposed faces.
    const int n = 14;   // 14^3 = 2744 cells, ~1030 of them solid
    std::vector<uint8_t> vox(size_t(n) * n * n, mat::AIR);
    int solid = 0;
    for (int y = 0; y < n && solid < want; ++y)
        for (int z = 0; z < n && solid < want; ++z)
            for (int x = 0; x < n && solid < want; ++x) {
                // A little noise so the surface is not one flat slab.
                const uint32_t h = hashU32(uint32_t(x * 73856093) ^ uint32_t(y * 19349663),
                                           uint32_t(z * 83492791));
                if ((h & 3u) == 0u) continue;
                vox[size_t(x) + size_t(z) * n + size_t(y) * n * n] = 3;   // any non-AIR id
                ++solid;
            }
    std::printf("one piece: %d solid of %d cells (%.0f%%), box %dx%dx%d\n", solid, n * n * n,
                100.0 * solid / (n * n * n), n, n, n);

    // ---- meshVolume ------------------------------------------------------
    double meshMs = 0.0;
    size_t tris = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < pieces; ++i) {
            const VoxMesh m = meshVolume(vox, n, n, n, VOXEL_M, true);
            tris += m.triCount();
        }
        meshMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count();
    }

    // ---- greedyBoxes, the collider ---------------------------------------
    //
    // The same retry loop makeLooseBody runs: start at kFellCellM and coarsen
    // until the box count fits, which on a piece this size is usually one pass.
    double boxMs = 0.0;
    size_t boxes = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<VoxBox> out;
        for (int i = 0; i < pieces; ++i) {
            float cell = 0.4f;
            for (int tries = 0; tries < 8; ++tries) {
                const int q = (cell / VOXEL_M + 0.5f) < 1.0f ? 1 : int(cell / VOXEL_M + 0.5f);
                const int cx = (n + q - 1) / q;
                const int need = int(float(q * q * q) * 0.35f) < 1 ? 1 : int(float(q * q * q) * 0.35f);
                greedyBoxes(
                    cx, cx, cx, Vec3{0, 0, 0}, cell,
                    [&](int i2, int j2, int k2) {
                        int c = 0;
                        for (int b2 = 0; b2 < q; ++b2)
                            for (int a2 = 0; a2 < q; ++a2)
                                for (int e2 = 0; e2 < q; ++e2) {
                                    const int x = i2 * q + e2, y = j2 * q + b2, z = k2 * q + a2;
                                    if (x >= n || y >= n || z >= n) continue;
                                    if (vox[size_t(x) + size_t(z) * n + size_t(y) * n * n] ==
                                        mat::AIR)
                                        continue;
                                    if (++c >= need) return true;
                                }
                        return false;
                    },
                    &out, 64 * 4);
                if (out.empty() && cell > VOXEL_M * 1.01f) { cell *= 0.5f; continue; }
                if (out.size() <= 64) break;
                cell *= 1.5f;
            }
            boxes += out.size();
        }
        boxMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    }

    std::printf("\n%d pieces -- the CPU half of makeLooseBody\n", pieces);
    std::printf("  meshVolume   %8.1f ms total   %6.3f ms each   (%zu tris each)\n", meshMs,
                meshMs / pieces, tris / size_t(pieces));
    std::printf("  greedyBoxes  %8.1f ms total   %6.3f ms each   (%zu boxes each)\n", boxMs,
                boxMs / pieces, boxes / size_t(pieces));
    std::printf("  ------------------------------------------------\n");
    std::printf("  movable      %8.1f ms total   %6.3f ms each\n", meshMs + boxMs,
                (meshMs + boxMs) / pieces);
    std::printf("\n  at kShatterPerFrame = 24 that is %.1f ms a frame of pure CPU\n",
                (meshMs + boxMs) / pieces * 24.0);
    return 0;
}
