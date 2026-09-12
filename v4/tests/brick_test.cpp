// ---------------------------------------------------------------------------
// brick_test.cpp -- the voxel store, checked without a GPU.
//
// scene/bricks.h and scene/generate.h include nothing but each other, the core
// headers and the shared layout header, so the WHOLE storage format -- the
// brick grid, the uniform/detail split, the hidden-brick cull, the compacted
// material run and the traversal arithmetic -- can be built and verified by a
// plain g++ harness with no device, no Falcor and no shader compiler.
//
// That matters more here than in most places, because the two things most
// likely to be wrong in a store like this are both SILENT:
//
//   THE MATERIAL RUN IS COMPACTED BY POPCOUNT. If the shader counts bits in a
//   different order from the packer, every voxel still renders -- wearing its
//   neighbour's colour. There is no crash and no black frame, just a wood whose
//   palette looks subtly off, which is indistinguishable from a palette bug.
//
//   THE DDA IS A TRANSLITERATION. storeIntersectBrick in stores/Aabb.slang
//   cannot be run on the host, so traceRef below is the same arithmetic written
//   out again and checked against a brute-force walk of the world itself. If
//   the two disagree the walk is wrong, and the test says at which voxel.
//
// Build and run:
//     g++ -O2 -std=c++17 -I src tests/brick_test.cpp -o brick_test && ./brick_test
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "scene/bricks.h"
#include "scene/generate.h"
#include "render/player.h"

using namespace v4;

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++failures;
    }
}

static double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// ---------------------------------------------------------------------------
// Find a packed brick by its world brick coordinate. The packer sorts, so this
// could bisect; a linear map is built once instead because the test does it
// millions of times and clarity is worth more here than the log factor.
// ---------------------------------------------------------------------------
static std::unordered_map<uint64_t, uint32_t> indexPacked(const PackedStore &p) {
    std::unordered_map<uint64_t, uint32_t> m;
    m.reserve(p.brick.size() * 2);
    for (uint32_t i = 0; i < p.brick.size(); ++i) {
        const V4Brick &h = p.brick[i];
        const int bx = floorDiv(h.ox, BRICK_E), by = floorDiv(h.oy, BRICK_E),
                  bz = floorDiv(h.oz, BRICK_E);
        // A UNIFORM ENTRY MAY BE A MERGED BOX OF MANY BRICKS -- see
        // PackOptions::mergeUniform. Every brick it covers has to resolve to
        // it, or the peel test reports the ones that are not the origin as
        // holes in the world. They are not holes; they are inside a bigger box.
        if ((h.flags & 1u) != 0u) {
            const int ex = int(h.maskBase & 0xFFFFu) + 1, ey = int(h.maskBase >> 16) + 1,
                      ez = int(h.mtlBase) + 1;
            for (int dz = 0; dz < ez / BRICK_E; ++dz)
                for (int dy = 0; dy < ey / BRICK_E; ++dy)
                    for (int dx = 0; dx < ex / BRICK_E; ++dx)
                        m[brickKey(bx + dx, by + dy, bz + dz)] = i;
        } else {
            m[brickKey(bx, by, bz)] = i;
        }
    }
    return m;
}

// ---------------------------------------------------------------------------
// TEST 1 -- THE PEEL IS SOUND, AND IT IS THE ONE TEST THAT MATTERS MOST.
//
// The packer drops every voxel with six opaque face-neighbours, on the argument
// that a ray from outside the solid must cross a neighbour to reach it. That
// argument is only as good as the neighbour test, and getting it wrong is
// SILENT in the worst way: the hole is on the far side of a surface that still
// renders, so it shows up as light leaking out of a hillside hours later.
//
// So this checks the property directly, in both directions, over every voxel in
// the world:
//
//   SURVIVES   a voxel with even ONE air or water face must still be in the
//              packed store, wearing its own material. This is the direction
//              that breaks things.
//   DROPPED    a voxel with six opaque faces must be gone. This direction only
//              costs memory, but a peel that keeps everything is a peel that
//              silently stopped working, and the numbers would still look fine.
//
// WATER IS NOT OPAQUE. The tracer refracts through it, so the bed of a lake is
// visible from above and its voxels are exposed, not buried. Treating water as
// solid here would delete every lake bed and the test would agree with itself.
// ---------------------------------------------------------------------------
static bool opaqueIn(const BrickWorld &w, int i, int y, int j) {
    const uint8_t m = w.at(i, y, j);
    return m != mat::AIR && m != mat::WATER;
}

static void testPeel(const BrickWorld &w, const PackedStore &p, bool peeled) {
    const auto idx = indexPacked(p);
    uint64_t exposed = 0, lost = 0, wrongMtl = 0, buried = 0, keptBuried = 0;

    for (const auto &kv : w.bricks()) {
        const Brick &b = kv.second;
        if (b.count == 0) continue;
        const int bx = keyX(kv.first), by = keyY(kv.first), bz = keyZ(kv.first);
        auto it = idx.find(kv.first);

        for (int lz = 0; lz < BRICK_E; ++lz)
            for (int ly = 0; ly < BRICK_E; ++ly)
                for (int lx = 0; lx < BRICK_E; ++lx) {
                    const int bit = Brick::bitOf(lx, ly, lz);
                    if (!b.bit(bit)) continue;
                    const int i = bx * BRICK_E + lx, y = by * BRICK_E + ly,
                              j = bz * BRICK_E + lz;
                    const bool isWater = b.mtl[bit] == mat::WATER;
                    const bool hidden =
                        !isWater && opaqueIn(w, i - 1, y, j) && opaqueIn(w, i + 1, y, j) &&
                        opaqueIn(w, i, y - 1, j) && opaqueIn(w, i, y + 1, j) &&
                        opaqueIn(w, i, y, j - 1) && opaqueIn(w, i, y, j + 1);

                    const uint8_t got =
                        (it == idx.end()) ? mat::AIR : unpackVoxel(p, it->second, lx, ly, lz);
                    if (hidden) {
                        ++buried;
                        // A UNIFORM BRICK LEGITIMATELY ANSWERS FOR ITS OWN
                        // INTERIOR. It is solid everywhere and stores no mask
                        // and no material run at all -- the slab test against
                        // its box IS the intersection -- so its buried voxels
                        // cost nothing and unpackVoxel still has an answer for
                        // them. Counting those as a failed peel would be
                        // measuring the one case that is already optimal.
                        const bool uniformBrick =
                            it != idx.end() && (p.brick[it->second].flags & 1u) != 0u;
                        if (got != mat::AIR && !uniformBrick) ++keptBuried;
                    } else {
                        ++exposed;
                        if (got == mat::AIR) {
                            if (++lost <= 3)
                                std::printf("    LOST voxel %d %d %d mtl %u  (brick %s)\n",
                                            i, y, j, b.mtl[bit],
                                            it == idx.end() ? "absent" : "present");
                        } else if (got != b.mtl[bit]) {
                            if (++wrongMtl <= 3)
                                std::printf("    WRONG voxel %d %d %d: world %u, store %u\n",
                                            i, y, j, b.mtl[bit], got);
                        }
                    }
                }
    }
    std::printf("  peel         %llu exposed voxels kept, %llu buried (%.1f%% of the world)\n",
                (unsigned long long)exposed, (unsigned long long)buried,
                100.0 * double(buried) / double(exposed + buried ? exposed + buried : 1));
    check(lost == 0, "a voxel with an exposed face is MISSING from the packed store -- "
                     "the peel cut through a surface and there is a hole in the world");
    check(wrongMtl == 0, "an exposed voxel came back with the wrong material -- the material "
                         "run and the peeled mask disagree about which bits they describe");
    if (peeled)
        check(keptBuried == 0, "a fully buried voxel was uploaded -- the peel is not running");
}

// ---------------------------------------------------------------------------
// TEST 2 -- the traversal.
//
// traceRef is a line-for-line transliteration of storeIntersectBrick plus the
// candidate loop around it, except that the acceleration structure is replaced
// by a walk over the bricks in front order. The comparison is against
// traceBrute, which steps the world one VOXEL at a time and knows nothing about
// bricks, masks or compaction.
//
// They must agree about whether there is a hit, which voxel it is, and the
// material -- and about `t` to within the size of a voxel, which is what the
// brute walk can resolve.
// ---------------------------------------------------------------------------
struct RefHit {
    bool hit = false;
    float t = 0.0f;
    int ijk[3] = {0, 0, 0};
    int axis = -1;
    uint8_t mtl = 0;
};

// The packed-store walk. `ro`/`rd` in VOXEL units, exactly as the shader.
static bool refIntersectBrick(const PackedStore &p, uint32_t bi, const float ro[3],
                              const float rd[3], float tMin, float tMax, RefHit *out) {
    const V4Brick &b = p.brick[bi];
    const int org[3] = {b.ox, b.oy, b.oz};
    // THE SAME FIELD WIDTH THE PACKER USED. Hardcoding three bits here worked
    // perfectly at 8^3 and silently decoded garbage at every other brick size,
    // which is exactly the drift a mirror exists to catch and instead caused.
    const uint32_t bs = uint32_t(kBrickShift), bm = uint32_t(kBrickE - 1);
    // A uniform brick may be a MERGED BOX of many -- its extent lives in the two
    // words a mask would use. Same rule as stores/Aabb.slang; if these two ever
    // disagree the traversal test says at which voxel.
    const bool uniformBrick = (b.flags & 1u) != 0u;
    const int blo[3] = {uniformBrick ? 0 : int(b.bounds & bm),
                        uniformBrick ? 0 : int((b.bounds >> bs) & bm),
                        uniformBrick ? 0 : int((b.bounds >> (2 * bs)) & bm)};
    const int bhi[3] = {
        uniformBrick ? int(b.maskBase & 0xFFFFu) : int((b.bounds >> (3 * bs)) & bm),
        uniformBrick ? int(b.maskBase >> 16) : int((b.bounds >> (4 * bs)) & bm),
        uniformBrick ? int(b.mtlBase) : int((b.bounds >> (5 * bs)) & bm)};
    float inv[3], tlo[3], thi[3];
    for (int k = 0; k < 3; ++k) {
        inv[k] = 1.0f / rd[k];
        const float a = (float(org[k] + blo[k]) - ro[k]) * inv[k];
        const float c = (float(org[k] + bhi[k] + 1) - ro[k]) * inv[k];
        tlo[k] = std::fmin(a, c);
        thi[k] = std::fmax(a, c);
    }
    const float tBox = std::fmax(tlo[0], std::fmax(tlo[1], tlo[2]));
    const float tEnter = std::fmax(tBox, tMin);
    const float tExit = std::fmin(std::fmin(thi[0], std::fmin(thi[1], thi[2])), tMax);
    if (tEnter > tExit) return false;

    int axis = 0;
    if (tlo[1] > tlo[0]) axis = 1;
    if (tlo[2] > tlo[0] && tlo[2] > tlo[1]) axis = 2;

    if (b.flags & 1u) {
        out->hit = true;
        out->t = tEnter;
        for (int k = 0; k < 3; ++k) {
            const int v = int(std::floor(ro[k] + rd[k] * tEnter));
            out->ijk[k] = std::max(org[k] + blo[k], std::min(org[k] + bhi[k], v));
        }
        out->axis = axis;
        out->mtl = uint8_t((b.flags >> 8) & 0xFFu);
        return true;
    }

    // -- THE WALK, transliterated from storeIntersectBrick ------------------
    //
    // An octant rejection pass, then a flat cell walk. Written out again from
    // the other side for the reason the whole harness exists: the shader cannot
    // be run here, so this is the only thing that checks it, and a mirror that
    // paraphrases proves nothing.
    int stp[3], stp01[3];
    float tDelta[3];
    for (int k = 0; k < 3; ++k) {
        stp[k] = rd[k] < 0.0f ? -1 : 1;
        stp01[k] = stp[k] > 0 ? 1 : 0;
        tDelta[k] = std::fabs(inv[k]);
    }

    {
        const int H = BRICK_E / 2;
        int olo[3], ohi[3], o[3];
        float tNextOct[3], tDeltaOct[3];
        for (int k = 0; k < 3; ++k) {
            olo[k] = blo[k] / H;
            ohi[k] = bhi[k] / H;
            // Subtract before dividing: integer division truncates toward zero.
            const int oc = (int(std::floor(ro[k] + rd[k] * tEnter)) - org[k]) / H;
            o[k] = std::max(olo[k], std::min(ohi[k], oc));
            tNextOct[k] =
                std::fmax((float(org[k] + (o[k] + stp01[k]) * H) - ro[k]) * inv[k], tEnter);
            tDeltaOct[k] = tDelta[k] * float(H);
        }
        bool reachable = false;
        float tO = tEnter;
        for (int so = 0; so < 8; ++so) {
            if (o[0] < olo[0] || o[1] < olo[1] || o[2] < olo[2] || o[0] > ohi[0] ||
                o[1] > ohi[1] || o[2] > ohi[2])
                break;
            if (b.octants & (1u << (unsigned(o[0]) | (unsigned(o[1]) << 1) |
                                    (unsigned(o[2]) << 2)))) {
                reachable = true;
                break;
            }
            if (tNextOct[0] <= tNextOct[1] && tNextOct[0] <= tNextOct[2]) {
                o[0] += stp[0];
                tO = tNextOct[0];
                tNextOct[0] += tDeltaOct[0];
            } else if (tNextOct[1] <= tNextOct[2]) {
                o[1] += stp[1];
                tO = tNextOct[1];
                tNextOct[1] += tDeltaOct[1];
            } else {
                o[2] += stp[2];
                tO = tNextOct[2];
                tNextOct[2] += tDeltaOct[2];
            }
            if (tO > tExit) break;
        }
        if (!reachable) return false;
    }

    int v[3];
    float tNext[3];
    for (int k = 0; k < 3; ++k) {
        const int cell = int(std::floor(ro[k] + rd[k] * tEnter)) - org[k];
        v[k] = std::max(blo[k], std::min(bhi[k], cell));
        tNext[k] = std::fmax((float(org[k] + v[k] + stp01[k]) - ro[k]) * inv[k], tEnter);
    }

    float tCur = tEnter;
    int axisCur = axis;
    for (int s2 = 0; s2 < kBrickMaxSteps; ++s2) {
        const int bit = Brick::bitOf(v[0], v[1], v[2]);
        // WORD-SPARSE, the same rule stores/Aabb.slang walks by: a word that
        // held nothing was never written, so a voxel in one is air without a
        // load. Same arithmetic from the other side -- if these two ever
        // disagree the traversal test says at which voxel.
        const uint32_t wmap = b.octants >> 8;
        const uint32_t wrd = uint32_t(bit) >> 5;
        const bool onWord = ((wmap >> wrd) & 1u) != 0u;
        const uint32_t wpos = popcount32(wmap & ((1u << wrd) - 1u));
        if (onWord && ((p.mask[b.maskBase + wpos] >> (bit & 31)) & 1u)) {
            out->hit = true;
            out->t = tCur;
            for (int k = 0; k < 3; ++k) out->ijk[k] = org[k] + v[k];
            out->axis = axisCur;
            out->mtl = unpackVoxel(p, bi, v[0], v[1], v[2]);
            return true;
        }
        if (tNext[0] <= tNext[1] && tNext[0] <= tNext[2]) {
            v[0] += stp[0];
            tCur = tNext[0];
            tNext[0] += tDelta[0];
            axisCur = 0;
            if (v[0] < blo[0] || v[0] > bhi[0]) break;
        } else if (tNext[1] <= tNext[2]) {
            v[1] += stp[1];
            tCur = tNext[1];
            tNext[1] += tDelta[1];
            axisCur = 1;
            if (v[1] < blo[1] || v[1] > bhi[1]) break;
        } else {
            v[2] += stp[2];
            tCur = tNext[2];
            tNext[2] += tDelta[2];
            axisCur = 2;
            if (v[2] < blo[2] || v[2] > bhi[2]) break;
        }
        if (tCur > tExit) break;
    }
    return false;
}

// Stand in for the acceleration structure: hand every brick to the intersector
// and keep the nearest. Slow and obviously correct, which is what a reference
// is for -- the hardware does the same thing in front-to-back order and stops
// early, and the ANSWER has to be identical either way.
static RefHit tracePacked(const PackedStore &p, const std::unordered_map<uint64_t, uint32_t> &idx,
                          const float o[3], const float d[3], float tMax) {
    float ro[3], rd[3];
    for (int k = 0; k < 3; ++k) {
        ro[k] = o[k] / VOXEL_M;
        float dv = d[k] / VOXEL_M;
        if (std::fabs(dv) < 1e-8f) dv = dv < 0.0f ? -1e-8f : 1e-8f;
        rd[k] = dv;
    }
    RefHit best;
    float bestT = tMax;
    for (uint32_t bi = 0; bi < p.brick.size(); ++bi) {
        RefHit h;
        if (!refIntersectBrick(p, bi, ro, rd, 0.0f, bestT, &h)) continue;
        best = h;
        bestT = h.t;
    }
    (void)idx;
    return best;
}

// ---------------------------------------------------------------------------
// THE REFERENCE, AND IT HAS TO BE EXACT RATHER THAN MERELY OBVIOUS.
//
// The first version of this stepped the ray a quarter of a voxel at a time and
// reported the first solid sample. It disagreed with the brick walk on 3 % of
// rays -- every one of them a grazing hit where a point sample landed a voxel
// past the face the exact walk entered through, and none of them a fault in the
// thing being tested. A sampled reference cannot adjudicate a DDA, because the
// DDA is the more accurate of the two.
//
// So this is a full Amanatides-Woo walk over the WORLD's voxels, with no
// knowledge of bricks, masks, compaction or the packed buffers. It shares no
// code with the thing it checks; it simply asks BrickWorld::at for one voxel at
// a time, which is the slowest and least clever way to answer the question and
// therefore the right one to believe.
// ---------------------------------------------------------------------------
static RefHit traceBrute(const BrickWorld &w, const float o[3], const float d[3], float tMax) {
    RefHit h;
    float ro[3], rd[3], inv[3], tNext[3], tDelta[3];
    int v[3], stp[3];
    for (int k = 0; k < 3; ++k) {
        ro[k] = o[k] / VOXEL_M;
        float dv = d[k] / VOXEL_M;
        if (std::fabs(dv) < 1e-8f) dv = dv < 0.0f ? -1e-8f : 1e-8f;
        rd[k] = dv;
        inv[k] = 1.0f / rd[k];
        v[k] = int(std::floor(ro[k]));
        stp[k] = rd[k] < 0.0f ? -1 : 1;
        tDelta[k] = std::fabs(inv[k]);
        tNext[k] = (float(v[k] + (stp[k] > 0 ? 1 : 0)) - ro[k]) * inv[k];
    }
    float tCur = 0.0f;
    for (int s = 0; s < 200000; ++s) {
        const uint8_t m = w.at(v[0], v[1], v[2]);
        if (m != mat::AIR) {
            h.hit = true;
            h.t = tCur;
            h.ijk[0] = v[0];
            h.ijk[1] = v[1];
            h.ijk[2] = v[2];
            h.mtl = m;
            return h;
        }
        int k = 0;
        if (tNext[1] < tNext[0]) k = 1;
        if (tNext[2] < tNext[k]) k = 2;
        v[k] += stp[k];
        tCur = tNext[k];
        tNext[k] += tDelta[k];
        if (tCur > tMax) break;
    }
    return h;
}

static void testTraversal(const BrickWorld &w, const PackedStore &p, bool cullHidden) {
    const auto idx = indexPacked(p);
    uint32_t rays = 0, agree = 0, disagreeHit = 0, disagreeVoxel = 0, disagreeMtl = 0,
             ties = 0;

    // Fired from above, down at the ground, from a grid of angles -- the case
    // the renderer actually spends its rays on. The brute walk resolves `t` to
    // a quarter of a voxel, so the voxel identity is what is compared and `t`
    // only to within one voxel.
    for (int a = 0; a < 40; ++a) {
        for (int b = 0; b < 40; ++b) {
            const float u = float(a) / 39.0f, v = float(b) / 39.0f;
            const float o[3] = {-70.0f + 140.0f * u, 62.0f, -70.0f + 140.0f * v};
            // Angled enough to cross bricks diagonally, which is where a DDA
            // goes wrong; never axis-aligned, which the brute walk cannot
            // resolve on a face.
            const float dir[3] = {0.21f + 0.4f * u, -1.0f, 0.17f + 0.4f * v};
            float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};

            const RefHit pk = tracePacked(p, idx, o, d, 400.0f);
            const RefHit bf = traceBrute(w, o, d, 400.0f);
            ++rays;

            // A HIDDEN BRICK IS A LEGAL DISAGREEMENT AND ONLY IN ONE DIRECTION.
            // The cull removes bricks a ray from outside the ground cannot
            // reach, so the packed walk can never hit something the brute walk
            // misses -- but if the brute walk somehow started inside the
            // ground, it could hit something the packed walk does not. Ray
            // origins here are 60 m up, so neither should happen.
            if (pk.hit != bf.hit) {
                ++disagreeHit;
                continue;
            }
            if (!pk.hit) {
                ++agree;
                continue;
            }

            // -- WHAT ACTUALLY HAS TO BE TRUE -----------------------------
            //
            // NOT "the two walks land on the same voxel". That test looked
            // right and was wrong: where a ray passes exactly through the
            // corner at which eight voxels meet, two DDAs that break the tie
            // differently leave through different faces and diverge for METRES
            // afterwards -- and neither of them is mistaken. Chasing that as a
            // bug means chasing float arithmetic that is behaving.
            //
            // The two properties a store owes the renderer are simpler, and
            // asymmetric:
            //
            //   IT MUST NOT INVENT.  The voxel it reports has to be solid in
            //                        the world, wearing the material it said.
            //   IT MUST NOT MISS.    It cannot come back with a hit FARTHER
            //                        than the reference walk found, because
            //                        then it stepped over something real --
            //                        which is the failure that shows up as a
            //                        hole in a hillside.
            //
            // A packed hit NEARER than the reference is fine, and is exactly
            // the corner case above: the brute walk grazed past a voxel the
            // brick walk caught. The world is asked which of them is right.
            const uint8_t truth = w.at(pk.ijk[0], pk.ijk[1], pk.ijk[2]);
            if (truth == mat::AIR || truth != pk.mtl) {
                ++disagreeMtl;
                if (disagreeMtl <= 4)
                    std::printf("    INVENTED  voxel %d %d %d: store says %u, world says %u\n",
                                pk.ijk[0], pk.ijk[1], pk.ijk[2], pk.mtl, truth);
                continue;
            }
            if (pk.t > bf.t + VOXEL_M) {
                ++disagreeVoxel;
                if (disagreeVoxel <= 4)
                    std::printf("    MISSED    o (%.3f %.3f %.3f) d (%.4f %.4f %.4f)\n"
                                "      store t %.4f at %d %d %d, reference t %.4f at %d %d %d\n",
                                o[0], o[1], o[2], d[0], d[1], d[2], pk.t, pk.ijk[0], pk.ijk[1],
                                pk.ijk[2], bf.t, bf.ijk[0], bf.ijk[1], bf.ijk[2]);
                continue;
            }
            if (pk.ijk[0] != bf.ijk[0] || pk.ijk[1] != bf.ijk[1] || pk.ijk[2] != bf.ijk[2])
                ++ties;
            ++agree;
        }
    }
    std::printf("  traversal    %u rays, %u agree, %u corner ties (%u hit-miss, %u missed,"
                " %u invented)\n",
                rays, agree, ties, disagreeHit, disagreeVoxel, disagreeMtl);
    (void)cullHidden;
    check(disagreeHit == 0, "the brick walk and a brute voxel walk disagree about hit/miss");
    check(disagreeVoxel == 0, "the brick walk MISSED a voxel the reference walk found -- a ray steps over something solid, which is a hole in the world");
    check(disagreeMtl == 0, "the brick walk reported a voxel the world does not have there -- it invented geometry, or the popcount indexing is off");
}

// ---------------------------------------------------------------------------
// TEST 3 -- the pack is deterministic. Two packs of the same world have to be
// byte-identical, or an A/B between two runs is comparing two worlds.
// ---------------------------------------------------------------------------
static void testDeterminism(const BrickWorld &w) {
    const PackedStore a = packStore(w);
    const PackedStore b = packStore(w);
    check(a.brick.size() == b.brick.size() && a.mask == b.mask,
          "two packs of the same world differ");
    bool sameHeaders = a.brick.size() == b.brick.size();
    for (size_t i = 0; sameHeaders && i < a.brick.size(); ++i)
        sameHeaders = a.brick[i].ox == b.brick[i].ox && a.brick[i].oy == b.brick[i].oy &&
                      a.brick[i].oz == b.brick[i].oz && a.brick[i].flags == b.brick[i].flags &&
                      a.brick[i].maskBase == b.brick[i].maskBase &&
                      a.brick[i].mtlBase == b.brick[i].mtlBase;
    check(sameHeaders, "two packs of the same world produced different brick headers");
}

// ---------------------------------------------------------------------------
// TEST 4 -- the layout the shader is compiled against.
// ---------------------------------------------------------------------------
static void testLayout() {
    check(sizeof(V4Brick) == 32, "V4Brick is not 32 bytes -- the shader reads a different stride");
    check(sizeof(V4StoreConsts) == 16, "V4StoreConsts is not one 16-byte constant-buffer row");
    check(kBrickE == (1 << kBrickShift) && kBrickVox == kBrickE * kBrickE * kBrickE &&
              kBrickWords == kBrickVox / 32,
          "the brick constants do not agree with kBrickShift");
    // SIX FIELDS OF kBrickShift BITS HAVE TO FIT IN ONE WORD. At shift 5 they
    // would not, and the high corner would silently wrap to the low one.
    check(6 * kBrickShift <= 32, "V4Brick::bounds cannot hold six fields this wide");

    // The bit order, stated once in AabbShared.slang and relied on by both
    // halves. A transposition here is the silent failure the whole harness
    // exists for.
    check(Brick::bitOf(1, 0, 0) == 1, "bit order: x is not the fastest axis");
    check(Brick::bitOf(0, 1, 0) == kBrickE, "bit order: y is not the second axis");
    check(Brick::bitOf(0, 0, 1) == kBrickE * kBrickE, "bit order: z is not the slowest axis");

    // A hand-built brick with a known, awkward occupancy: every material must
    // come back from the compacted run, including across a word boundary.
    BrickWorld w;
    int n = 0;
    for (int lz = 0; lz < BRICK_E; ++lz)
        for (int ly = 0; ly < BRICK_E; ++ly)
            for (int lx = 0; lx < BRICK_E; ++lx)
                if (((lx * 7 + ly * 13 + lz * 29) % 5) != 0)
                    w.set(lx, ly, lz, uint8_t(1 + (n++ % 200)));
    const PackedStore p = packStore(w, PackOptions{});
    check(p.brick.size() == 1, "one brick of voxels did not pack into one brick");
    if (p.brick.size() == 1) {
        int bad = 0;
        for (int lz = 0; lz < BRICK_E; ++lz)
            for (int ly = 0; ly < BRICK_E; ++ly)
                for (int lx = 0; lx < BRICK_E; ++lx)
                    if (unpackVoxel(p, 0, lx, ly, lz) != w.at(lx, ly, lz)) ++bad;
        check(bad == 0, "the compacted material run does not round trip on a sparse brick");
    }
}

// ---------------------------------------------------------------------------
// TEST 5 -- THE HOST QUERY, which is the half the shader seam does not cover.
//
// A walker stands on what topAt tells it, so a wrong answer here is a body
// sunk in the floor or hovering over it -- and neither shows up in any of the
// ray tests above, because rays never ask.
//
// THE BOUND IS THE INTERESTING PART. "The ground" in a volume is not a property
// of the column, it is a property of where you are IN it: under an arch you
// stand on the floor, not on the arch. So topAt searches DOWN from a given
// height, and the two ways of getting that wrong are both silent -- searching
// from the world's ceiling puts you on the roof, and mishandling a search that
// starts ABOVE the world cuts it short and puts you most of a metre under.
// ---------------------------------------------------------------------------
static void testHostQueries() {
    BrickWorld w;
    // A slab from y=0..11, with a separate shelf at y=30..31 over part of it.
    for (int j = -4; j <= 4; ++j)
        for (int i = -4; i <= 4; ++i) {
            for (int y = 0; y <= 11; ++y) w.set(i, y, j, mat::ROCK);
            if (i >= 0) for (int y = 30; y <= 31; ++y) w.set(i, y, j, mat::ROCK);
        }

    check(w.topAt(-2, 0) == 11, "topAt missed the surface of a plain column");
    check(w.topAt(2, 0) == 31, "topAt did not find the shelf above the column");

    // FROM THE SKY. The search starts above every brick in the world, so the
    // clamp to the top brick must not also apply that height's offset INSIDE
    // the brick -- that reported 8 instead of 11 and put a spawn 0.3 m under.
    const int sky = int(w.skyM() / VOXEL_M);
    check(w.topAt(-2, 0, sky) == 11, "bounded topAt from above the world cut the search short");
    check(w.topAt(2, 0, sky) == 31, "bounded topAt from the sky missed the shelf");

    // FROM UNDER THE SHELF. This is the arch case: a body at y=20 is below the
    // shelf and must be told the slab, not the thing over its head.
    check(w.topAt(2, 0, 20) == 11, "bounded topAt returned a surface ABOVE the search height");
    check(w.topAt(2, 0, 11) == 11, "bounded topAt excluded the voxel at its own height");
    check(w.topAt(2, 0, 5) == 5, "bounded topAt did not stop at the height it was given");
    check(w.topAt(2, 0, -1) == BrickWorld::kNoTop, "bounded topAt found ground below the world");

    check(w.groundM(-2, 0) > 1.19f && w.groundM(-2, 0) < 1.21f,
          "groundM is not the TOP face of the highest voxel");
    check(w.topAt(99, 99) == BrickWorld::kNoTop, "topAt invented ground in an empty column");

    // -- GRASS IS SOMETHING YOU WALK THROUGH, NOT ON --------------------
    //
    // Four voxels of blade on top of the slab. A ray hits them; a body must
    // not, or it climbs a step entering every tuft and stands a quarter of a
    // metre above the soil it is supposedly on.
    for (int y = 12; y <= 15; ++y) w.set(-2, y, 0, mat::GRASS_0);
    check(w.topAt(-2, 0) == 15, "topAt should still see the grass -- rays do");
    check(w.topWalkAt(-2, 0, 40) == 11, "topWalkAt stood the body on the grass tips");
    check(w.walkSolidAt(-2, 13, 0) == false, "a blade of grass blocked the walker");
    check(w.solidAt(-2, 13, 0) == true, "the blade stopped being geometry");
    check(w.walkSolidAt(-2, 11, 0) == true, "the ground under the grass stopped being solid");
    // A whole column of nothing but grass has no floor in it at all.
    for (int y = 0; y <= 5; ++y) w.set(30, y, 30, mat::GRASS_0);
    check(w.topWalkAt(30, 30, 40) == BrickWorld::kNoTop,
          "topWalkAt found footing in a column that is only grass");

    std::printf("  host query   topAt, groundM, overhang, grass is walk-through\n");
}

// ---------------------------------------------------------------------------
// TEST 6 -- WATER IS NOT A WALL, AND THE WHOLE-BRICK CULL HAS TO KNOW IT TOO.
//
// There are TWO tests for "can anything reach this voxel" and they have to
// agree. The peel asks it per voxel and knows water is a dielectric. Above it
// sits a shortcut that drops a brick outright when it and all six of its
// neighbours are full -- six hash lookups instead of five hundred neighbour
// tests, and it answers most of a hillside.
//
// "Full" was a count of BITS. A brick of solid water walled in by solid water
// is full on that count and still visible from outside, because the tracer
// refracts through every voxel of it -- so the shortcut dropped it before the
// peel, which never drops water, could object. What that looks like is NOT a
// missing brick: the lake surface and the lake bed are both skin and both
// survive, so the water renders and only its MIDDLE is gone. Depth-based
// absorption, caustics and god rays all read from a volume with a hole in it.
//
// AND IT ONLY APPEARED AT 192 m. A lake has to be deeper than one brick before
// any brick of it is surrounded, and at 96 m none was -- so checkstore.bat,
// which runs 96 m, was green while the build was wrong. Hence a hand-built
// world here: three bricks cubed, no generator, no assets, no extent.
// ---------------------------------------------------------------------------
static void testWaterIsNotSealed() {
    // 3x3x3 bricks of one material, so the middle brick is walled in on all six
    // faces by bricks that are solid to their own edges.
    const int N = 3 * BRICK_E;
    auto block = [N](BrickWorld &w, uint8_t m) {
        for (int j = 0; j < N; ++j)
            for (int y = 0; y < N; ++y)
                for (int i = 0; i < N; ++i) w.set(i, y, j, m);
    };

    // ROCK -- the shortcut must still fire. A cull that quietly stops working
    // doubles the store and every number still looks reasonable.
    {
        BrickWorld w;
        block(w, mat::ROCK);
        const PackedStore p = packStore(w);
        const auto idx = indexPacked(p);
        check(idx.count(brickKey(1, 1, 1)) == 0,
              "the whole-brick cull stopped dropping a rock brick buried in rock");
    }

    // WATER -- the same shape, and every voxel of it has to arrive.
    {
        BrickWorld w;
        block(w, mat::WATER);
        const PackedStore p = packStore(w);
        const auto idx = indexPacked(p);
        auto it = idx.find(brickKey(1, 1, 1));
        check(it != idx.end(),
              "a brick of solid water inside solid water was culled -- the middle of every "
              "lake deeper than one brick is missing from the store");
        if (it != idx.end()) {
            int wrong = 0;
            for (int lz = 0; lz < BRICK_E; ++lz)
                for (int ly = 0; ly < BRICK_E; ++ly)
                    for (int lx = 0; lx < BRICK_E; ++lx)
                        if (unpackVoxel(p, it->second, lx, ly, lz) != mat::WATER) ++wrong;
            check(wrong == 0, "the packed store does not read back as water inside a lake");
        }
        // And the peel property over the whole block, which is the same check
        // the landscape gets: no water voxel anywhere may be missing.
        testPeel(w, p, PackOptions{}.peel);
    }

    // ONE VOXEL OF WATER IN THE NEIGHBOUR, and the rock brick it touches must
    // fall through to the per-voxel peel rather than being culled. This is the
    // direction that proves the shortcut got narrower and not merely different:
    // a single drop against one face keeps exactly one voxel alive.
    {
        BrickWorld w;
        block(w, mat::ROCK);
        const int fx = 2 * BRICK_E, my = BRICK_E + 4, mz = BRICK_E + 4;  // +x neighbour face
        w.set(fx, my, mz, mat::WATER);
        const PackedStore p = packStore(w);
        const auto idx = indexPacked(p);
        auto it = idx.find(brickKey(1, 1, 1));
        check(it != idx.end(),
              "a rock brick facing one voxel of water was culled -- the lake bed is a hole");
        if (it != idx.end())
            check(unpackVoxel(p, it->second, BRICK_E - 1, 4, 4) == mat::ROCK,
                  "the voxel facing the water is not in the store");
    }

    std::printf("  sealed       water does not wall a brick in; rock still does\n");
}

// ---------------------------------------------------------------------------
// TEST 7 -- WHAT IS UNDER YOU WHEN YOU ARE INSIDE THE HILL.
//
// Every query in this engine that finds "the ground" searches DOWN from a
// height, and the generator writes only the SKIN -- one voxel on flat ground,
// with five hundred more answered by ImplicitColumns. So a query from a point
// INSIDE the terrain finds no written voxel where it is, and the version of
// topAt that scanned bricks to the bottom of the world before consulting the
// formula walked straight through half a kilometre of implicit rock and
// returned the BOTTOM skin band. "The ground here is the bedrock, fifty-two
// metres down" -- true about written voxels, a lie about the world.
//
// WHAT IT COST: a body that stopped flying forty metres up fell through the
// planet. At terminal speed a frame is seven voxels, so the first ground test
// after clearing the one-voxel skin was taken from inside the rock, and the
// answer sent it to the bedrock. From fifteen metres up the same fall lands
// correctly, because it is still inside the skin when it is first tested --
// which is why this is a test over DEPTHS and not a spot check.
//
// The invariant is simple and total: inside a solid column, the highest solid
// at or below your own height IS your own height.
// ---------------------------------------------------------------------------
static void testUnderTheSkin(const BrickWorld &w) {
    const ImplicitColumns &im = w.implicit();
    if (!im.valid()) return;
    const int sky = int(w.skyM() / VOXEL_M);
    long long asked = 0, wrong = 0;
    int wi = 0, wy = 0, wj = 0, wgot = 0;

    // A spread of columns rather than a block, so one flat meadow cannot carry
    // the whole test.
    for (int j = im.g.j0 + 3; j < im.g.j1() - 3; j += 37)
        for (int i = im.g.i0 + 3; i < im.g.i1() - 3; i += 41) {
            const int h = w.topAt(i, j, sky);
            if (h == BrickWorld::kNoTop) continue;
            // Straight down through the skin, the soil, the stone and into the
            // bedrock -- every band the formula speaks for.
            for (int d = 1; d <= 480; d += 7) {
                const int y = h - d;
                if (im.at(i, y, j) == mat::AIR) break;  // out of the column
                ++asked;
                const int got = w.topAt(i, j, y);
                if (got != y && ++wrong <= 3) { wi = i; wy = y; wj = j; wgot = got; }
            }
        }
    if (wrong)
        std::printf("    UNDER THE SKIN: at (%d %d %d) topAt said %d -- %d voxels too low\n",
                    wi, wy, wj, wgot, wy - wgot);
    std::printf("  under skin   %lld queries from inside the terrain, %lld wrong\n", asked, wrong);
    check(wrong == 0,
          "a ground query from INSIDE the terrain reported a surface far below -- the brick "
          "scan ran past the implicit interior to the bottom skin band, and a body that stops "
          "flying falls through the world");
}

// ---------------------------------------------------------------------------
// TEST 8 -- STOP FLYING AND YOU LAND ON THE GROUND YOU LEFT.
//
// The symptom TEST 7 explains, checked end to end: the walker is GPU-free -- it
// is one pointer at a BrickWorld -- so the whole transition runs here.
//
// AND IT IS A SWEEP OVER HEIGHTS, because the two ways this breaks are both
// speed-dependent and neither shows at walking pace. Tunnelling needs a frame
// long enough to clear the skin; SINKING needs one long enough to bury the
// body before the landing test, which put it seventy centimetres under the
// hill after an eighty-metre drop and left it standing there.
// ---------------------------------------------------------------------------
static void testStopFlying(const BrickWorld &w) {
    WalkWorld ww;
    ww.world = &w;
    const float dt = 1.0f / 60.0f;
    static const float kFrom[] = {2.0f, 5.0f, 15.0f, 40.0f, 80.0f, 150.0f};
    float worst = 0.0f;
    int fell = 0;
    for (float above : kFrom) {
        Player p;
        p.placeOnGround(ww, 0.0f, 0.0f);
        const float ground = p.pos.y;
        p.pos.y = ground + above;   // fly up...
        p.vy = 0.0f;
        p.onGround = false;
        p.fly = false;              // ...and stop flying, exactly as F does
        int n = 0;
        for (; n < 4000 && !p.onGround; ++n)
            p.update(ww, Vec3(0.0f, 0.0f, 0.0f), false, false, false, false, dt);
        if (!p.onGround) { ++fell; continue; }
        const float off = fabsf(p.pos.y - ground);
        if (off > worst) worst = off;
    }
    std::printf("  stop flying  6 drops up to 150 m, worst landing error %.2f m\n", worst);
    check(fell == 0, "a body that stopped flying never found the ground at all");
    // One voxel of tolerance: the landing snaps to a voxel face, not to a float.
    check(worst <= VOXEL_M + 1e-4f,
          "a body that stopped flying landed somewhere other than the ground it left -- it "
          "either fell through the terrain or was buried in it");
}

// ---------------------------------------------------------------------------
// TEST 9 -- A WORLD BUILT A CHUNK AT A TIME IS THE SAME WORLD.
//
// This is the property an endless world rests on, and it is checked the only
// way that means anything: build the same ground twice -- once in one piece,
// once a chunk at a time -- and compare every voxel.
//
// THE CHUNKED BUILD IS DONE THE WAY THE ENGINE WILL DO IT. One set of columns
// for the whole resident window, because a chunk that recomputed its own would
// pay four times over for its margin; then each chunk writes the voxels it
// OWNS, discovering its neighbours' boulders for itself -- a big one reaches
// 12.7 m from its anchor -- and keeping only the half that lands on its own
// ground. Both halves of that matter: discover too narrowly and a boulder
// loses the part that overhangs, write too widely and every chunk stamps its
// neighbours' models as well, so dropping one tears holes in the others.
//
// THE COMPARISON IS INSET BY THE REACH, and that is not a fudge. A world built
// in one piece lets a boulder at the extent's edge hang over it, because there
// is nothing out there for it to belong to; a chunked build has no edge and no
// overhang. They are supposed to differ there and nowhere else.
// ---------------------------------------------------------------------------
static void testChunkedMatchesWhole(const GenOptions &base, const Content &content,
                                    const BrickWorld &one) {
    GenOptions o = base;
    const WorldConstants wc = WorldConstants::of(o);
    const int half = int(o.extentM * 0.5f / VOXEL_M);
    ColGrid g;
    g.i0 = -half;
    g.j0 = -half;
    g.w = half * 2;
    g.h = half * 2;

    const Columns col = buildColumns(o, wc, g);
    BrickWorld many;
    many.setImplicit(implicitFrom(col));
    many.setWaterM(one.waterM());

    const int CH = 256;         // one chunk of columns -- 32 bricks of 8
    const int reach = 127 + 2;  // measured over the real assets; see the margin note
    int chunks = 0;
    for (int j = g.j0; j < g.j1(); j += CH)
        for (int i = g.i0; i < g.i1(); i += CH) {
            ColGrid own;
            own.i0 = i;
            own.j0 = j;
            own.w = std::min(CH, g.i1() - i);
            own.h = std::min(CH, g.j1() - j);
            ColGrid disc = own;
            disc.i0 -= reach;
            disc.j0 -= reach;
            disc.w += 2 * reach;
            disc.h += 2 * reach;
            buildBody(many, content, o, col, own, disc, /*clipped*/ true);
            ++chunks;
        }

    const int in = reach + 1;
    long long diff = 0, solid = 0;
    int di = 0, dy = 0, dj = 0;
    unsigned da = 0, db = 0;
    const int y1 = int(one.skyM() / VOXEL_M);
    for (int j = g.j0 + in; j < g.j1() - in; ++j)
        for (int i = g.i0 + in; i < g.i1() - in; ++i)
            for (int y = -8; y < y1; ++y) {
                const uint8_t a = one.at(i, y, j), b = many.at(i, y, j);
                if (a != mat::AIR || b != mat::AIR) ++solid;
                if (a != b && !diff++) { di = i; dy = y; dj = j; da = a; db = b; }
            }
    if (diff)
        std::printf("    CHUNK SEAM at (%d %d %d): one piece %u, chunked %u\n", di, dy, dj, da, db);
    std::printf("  chunked      %d chunks, %lld voxels compared, %lld differ\n", chunks, solid,
                diff);
    check(diff == 0,
          "a world built a chunk at a time is not the world built in one piece -- a streamed "
          "chunk would be missing voxels its neighbour owns, or stamping voxels it does not");
}

// ---------------------------------------------------------------------------
// TEST 10 -- THE IMPLICIT INTERIOR IS EXACT.
//
// The generator writes only the skin of the terrain and hands everything sealed
// inside it to ImplicitColumns, a heightfield and three integers. That saves
// 1.9 billion set() calls and two gigabytes, and it is only worth anything if
// the device gets EXACTLY the same bytes either way.
//
// So build the world twice -- once with the formula, once with every buried
// voxel written out by hand -- and compare the packed buffers. Not the counts:
// the buffers. A peel that keeps one extra face, a material that shifts by one
// rank, an AABB a voxel too big, all show up here and nowhere else.
// ---------------------------------------------------------------------------
static void testImplicitMatchesExplicit(const GenOptions &base, const Content &content) {
    GenOptions a = base;
    a.extentM = 48.0f;  // small: the explicit side of this is deliberately slow
    GenOptions b = a;
    b.explicitFill = true;

    BrickWorld wa, wb;
    generate(wa, content, a);
    generate(wb, content, b);
    const PackedStore pa = packStore(wa), pb = packStore(wb);

    std::printf("  implicit     %.2f M voxels written vs %.2f M explicit, %zu bricks vs %zu\n",
                double(wa.voxelCount()) * 1e-6, double(wb.voxelCount()) * 1e-6,
                pa.brick.size(), pb.brick.size());

    check(wa.voxelCount() < wb.voxelCount(),
          "the implicit fill wrote as much as the explicit one -- it is not running");
    check(pa.brick.size() == pb.brick.size(), "implicit and explicit packed a different "
                                              "number of bricks");
    check(pa.kept == pb.kept, "implicit and explicit disagree about how many voxels survive");
    check(pa.mask == pb.mask, "the packed mask/material buffers differ between implicit and "
                              "explicit -- the formula and the voxels disagree somewhere");
    check(pa.aabb == pb.aabb, "the AABBs differ between implicit and explicit");
    bool sameHdr = pa.brick.size() == pb.brick.size();
    for (size_t i = 0; sameHdr && i < pa.brick.size(); ++i)
        sameHdr = pa.brick[i].ox == pb.brick[i].ox && pa.brick[i].oy == pb.brick[i].oy &&
                  pa.brick[i].oz == pb.brick[i].oz && pa.brick[i].flags == pb.brick[i].flags &&
                  pa.brick[i].bounds == pb.brick[i].bounds &&
                  pa.brick[i].octants == pb.brick[i].octants &&
                  pa.brick[i].maskBase == pb.brick[i].maskBase &&
                  pa.brick[i].mtlBase == pb.brick[i].mtlBase;
    check(sameHdr, "the brick headers differ between implicit and explicit");
}

int main(int argc, char **argv) {
    GenOptions o;
    o.extentM = (argc > 1) ? float(std::atof(argv[1])) : 96.0f;
    o.demo = (argc > 2 && std::string(argv[2]) == "demo");
    if (const char *sv = std::getenv("V4_STONE")) o.stoneVox = std::atoi(sv);
    if (const char *dv = std::getenv("V4_SOIL")) o.soilVox = std::atoi(dv);
    // THE REAL ASSETS BY DEFAULT, because the whole point of a GPU-free harness
    // is that it checks the wood a run actually builds. Pass "none" as the third
    // argument to test the procedural fallback instead.
    if (argc > 3 && std::string(argv[3]) == "none") {
        o.models = false;
    } else {
        o.pineDir = "C:/voxelbit/game/assets/foilage/pine9";
        o.decorDir = "C:/voxelbit/game/assets/decoration";
        o.mineralPath = "C:/voxelbit/source/wip/foilage/mineral.vox";
    }

    std::printf("v4 store harness -- %s, %.0f m, %d^3 bricks\n",
                o.demo ? "demo scene" : "landscape", o.extentM, kBrickE);

    testLayout();
    testHostQueries();
    testWaterIsNotSealed();

    Palette pal;
    const Content content = buildPalette(pal, o);
    std::printf("  models       %s\n", content.status.c_str());
    std::printf("  palette      %d entries used, %d stone samples\n", pal.used(),
                pal.stoneSampleCount());
    check(pal.used() > mat::TREE_BASE, "the generator minted no material ids");
    if (o.models && !o.pineDir.empty()) {
        check(content.authored(), "the nine pine models did not load");
        check(content.models.missing == 0, "some pine_N.vox files are missing");
        check(content.models.rockCount() >= 26, "the 26 boulder models did not all load");
        check(!content.models.rocksBig.empty() && !content.models.rocksMid.empty() &&
                  !content.models.rocksSmall.empty(),
              "one of the three boulder size sets is empty");
        check(!content.models.mushrooms.empty(), "mushroom.vox did not load");
        check(content.models.flowers.size() > 1,
              "flowers.vox came back as ONE piece -- the overlap rule merged six flowers "
              "into a clump");
        // A TALL PINE IS SPLIT ACROSS PIECES because a .vox coordinate is a
        // byte. If the overlap rule failed, every tree loses its top and comes
        // back around 25.6 m instead of its real height.
        int tallest = 0;
        for (const VoxPiece &p2 : content.models.pines) tallest = std::max(tallest, p2.sy);
        std::printf("  pines        %zu models, tallest %d voxels (%.1f m)\n",
                    content.models.pines.size(), tallest, float(tallest) * VOXEL_M);
        check(tallest > 256, "no pine is taller than 256 voxels -- the split-piece rule failed "
                             "and every tree lost its top");
    }
    // THE GROUND RAMP MUST STILL BE TRANSLUCENT, and that is not a typo. The
    // GRASS band is the FOLIAGE band; it is worn by standing strands, where
    // being lit from behind is the whole point. What must never happen is a
    // ground PLANE laid in it -- see the note in buildTerrain.
    check(pal[mat::GRASS_0].translucency > 0.0f,
          "the grass ramp lost its translucency -- strands will read as black sticks at dawn");
    check(pal[mat::LITTER_0].translucency == 0.0f,
          "the litter ramp is translucent -- a whole floor in it blows the frame to white");

    BrickWorld w;
    auto t0 = std::chrono::steady_clock::now();
    generate(w, content, o);
    const double genMs = msSince(t0);

    t0 = std::chrono::steady_clock::now();
    const PackedStore p = packStore(w);
    const double packMs = msSince(t0);

    const double hostMb =
        double(w.brickCount()) * double(sizeof(Brick)) / (1024.0 * 1024.0);
    std::printf("  world        %.2f M voxels in %zu bricks -- %.0f MB host, generated %.0f ms\n",
                double(w.voxelCount()) * 1e-6, w.brickCount(), hostMb, genMs);
    std::printf("  packed       %zu bricks (%llu uniform, %llu detail, %llu dropped), %zu chunks,"
                " %.1f MB device, packed %.0f ms\n",
                p.brick.size(), (unsigned long long)p.uniform, (unsigned long long)p.detail,
                (unsigned long long)p.hidden, p.chunk.size(),
                double(p.bytes()) / (1024.0 * 1024.0), packMs);
    std::printf("  mono         %llu of %llu detail bricks are a single material (%.0f%%)\n",
                (unsigned long long)p.mono, (unsigned long long)p.detail,
                100.0 * double(p.mono) / double(p.detail ? p.detail : 1));
    std::printf("  reachable    %.2f M of %.2f M voxels (%.1f%%), AABBs cover %.1f%% of their"
                " bricks\n",
                double(p.kept) * 1e-6, double(p.voxels) * 1e-6,
                100.0 * double(p.kept) / double(p.voxels ? p.voxels : 1), 100.0 * p.fill);

    // Every chunk's run has to be contiguous and in order, because the shader's
    // brick index is InstanceID + PrimitiveIndex and nothing checks it.
    uint32_t expect = 0;
    bool contiguous = true;
    for (const auto &c : p.chunk) {
        contiguous = contiguous && c.first == expect;
        expect += c.count;
    }
    check(contiguous, "a chunk's bricks are not a contiguous run -- InstanceID + PrimitiveIndex "
                      "would index the wrong brick");
    check(expect == p.brick.size(), "the chunks do not cover every brick");
    check(p.aabb.size() == p.brick.size() * 6, "there is not one AABB per brick");

    testUnderTheSkin(w);
    testStopFlying(w);
    if (!o.demo) testChunkedMatchesWhole(o, content, w);
    testPeel(w, p, PackOptions{}.peel);
    if (!o.demo) testImplicitMatchesExplicit(o, content);
    testTraversal(w, p, true);
    testDeterminism(w);

    if (failures == 0) std::printf("OK\n");
    else std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
