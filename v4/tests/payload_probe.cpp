// ---------------------------------------------------------------------------
// payload_probe.cpp -- what is actually left to compress in a brick?
//
// Dedup was measured and failed (1.2x, 6 % -- tests/dedup_probe.cpp). The
// header and the AABB are the irreducible cost of being a primitive. So the
// only compressible things left are the two the packer writes per detail brick,
// and this sizes both before anyone writes a line of shader:
//
//   THE MASK, 64 bytes flat, sixteen words, whether the brick holds one voxel
//   or five hundred. It is the single biggest item in the store. The packer
//   ALREADY computes an octant byte saying which of the eight halves hold
//   anything -- so a brick with three occupied octants could store 1 + 3*8 = 25
//   bytes instead of 64. This measures the occupied-octant distribution, which
//   is exactly the compression ratio that idea would get.
//
//   THE MATERIAL RUN, one byte per solid voxel. A third of detail bricks are
//   already monochrome and store none. But a brick with FOUR distinct materials
//   needs two bits a voxel, not eight, plus a tiny per-brick palette. This
//   measures the distinct-material distribution, which is that ratio.
//
// NEITHER IS FREE IN THE WALK. A sparse mask adds an indirection per octant and
// a packed material run adds a shift; this engine has measured repeatedly that
// per-step arithmetic is not what costs it, but it has also measured that the
// mask is already in L1, so SHRINKING it may buy less than the byte count says.
// Size it first, then decide.
//
// GPU-FREE.  g++ -O2 -std=c++17 -I src tests/payload_probe.cpp -o payloadprobe
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "scene/bricks.h"
#include "scene/generate.h"

using namespace v4;

int main(int argc, char **argv) {
    GenOptions o;
    o.extentM = (argc > 1) ? float(std::atof(argv[1])) : 192.0f;
    o.pineDir = "C:/voxelbit/game/assets/foilage/pine9";
    o.decorDir = "C:/voxelbit/game/assets/decoration";
    o.mineralPath = "C:/voxelbit/source/wip/foilage/mineral.vox";

    Palette pal;
    const Content content = buildPalette(pal, o);
    BrickWorld w;
    generate(w, content, o);
    const PackedStore p = packStore(w, PackOptions{});

    std::printf("v4 payload probe -- %.0f m, %d^3 bricks\n", o.extentM, kBrickE);
    std::printf("  packed       %zu bricks (%llu uniform, %llu detail, %llu mono), %.1f MB\n",
                p.brick.size(), (unsigned long long)p.uniform, (unsigned long long)p.detail,
                (unsigned long long)p.mono, double(p.bytes()) / (1024.0 * 1024.0));

    // -- how many octants does a detail brick actually occupy? --------------
    std::map<int, uint64_t> octHist;
    uint64_t maskFlat = 0, maskSparse = 0, maskWordSparse = 0, wordNz = 0, nzBricks = 0;
    // -- how many DISTINCT materials does a detail brick wear? --------------
    std::map<int, uint64_t> mtlHist;
    uint64_t runFlat = 0, runPacked = 0;

    const int H = BRICK_E / 2;
    for (const V4Brick &h : p.brick) {
        if (h.flags & 1u) continue;  // uniform: no mask, no run

        // octants, counted off the mask that was actually uploaded
        int occ = 0;
        uint32_t oct = 0;
        for (int k = 0; k < BRICK_VOX; ++k) {
            if (!((p.mask[h.maskBase + uint32_t(k >> 5)] >> (k & 31)) & 1u)) continue;
            const int lx = k & (BRICK_E - 1);
            const int ly = (k >> kBrickShift) & (BRICK_E - 1);
            const int lz = (k >> (2 * kBrickShift)) & (BRICK_E - 1);
            oct |= 1u << ((lx / H) | ((ly / H) << 1) | ((lz / H) << 2));
        }
        for (int b = 0; b < 8; ++b)
            if (oct & (1u << b)) ++occ;
        octHist[occ]++;
        maskFlat += uint64_t(BRICK_WORDS) * 4ull;
        // one byte of octant mask + eight bytes per occupied octant
        maskSparse += 1ull + uint64_t(occ) * 8ull;

        // WORD-SPARSE, the cheaper cousin: keep only the non-zero words plus a
        // 16-bit map of which. It preserves the LINEAR bit order, so the
        // material run's popcount rank is untouched and packer and shader keep
        // the ordering they already agree about. Octant-sparse reorders both.
        int nzw = 0;
        for (int k = 0; k < BRICK_WORDS; ++k)
            if (p.mask[h.maskBase + uint32_t(k)]) ++nzw;
        wordNz += uint64_t(nzw);
        nzBricks += 1ull;
        maskWordSparse += 2ull + uint64_t(nzw) * 4ull;

        // distinct materials among the voxels the mask keeps
        bool seen[256] = {false};
        int distinct = 0;
        uint64_t solid = 0;
        for (int k = 0; k < BRICK_VOX; ++k) {
            if (!((p.mask[h.maskBase + uint32_t(k >> 5)] >> (k & 31)) & 1u)) continue;
            ++solid;
            if (h.flags & 2u) continue;  // monochrome: the run is not stored
            uint32_t before = 0;
            for (int j = 0; j < (k >> 5); ++j) before += popcount32(p.mask[h.maskBase + uint32_t(j)]);
            before += popcount32(p.mask[h.maskBase + uint32_t(k >> 5)] &
                                 ((k & 31) ? ((1u << (k & 31)) - 1u) : 0u));
            const uint32_t idx = h.mtlBase + before;
            const uint8_t m = uint8_t((p.mask[idx >> 2] >> ((idx & 3u) * 8u)) & 0xFFu);
            if (!seen[m]) { seen[m] = true; ++distinct; }
        }
        if (h.flags & 2u) {
            mtlHist[1]++;  // monochrome, already free
            continue;
        }
        mtlHist[distinct]++;
        runFlat += solid;  // one byte a voxel
        // bits needed to index a per-brick palette, plus the palette itself
        int bits = 1;
        while ((1 << bits) < distinct) ++bits;
        if (distinct <= 1) bits = 0;
        runPacked += (solid * uint64_t(bits) + 7ull) / 8ull + uint64_t(distinct);
    }

    std::printf("\n  OCCUPIED OCTANTS per detail brick (a sparse mask stores 1 + 8*n bytes):\n");
    for (const auto &kv : octHist)
        std::printf("    %d octant%s : %llu bricks\n", kv.first, kv.first == 1 ? " " : "s",
                    (unsigned long long)kv.second);
    std::printf("    mask %.1f MB flat -> %.1f MB WORD-sparse (%.1f %% saved), avg %.1f of %d words\n",
                double(maskFlat) / (1024.0 * 1024.0),
                double(maskWordSparse) / (1024.0 * 1024.0),
                100.0 * double(maskFlat - maskWordSparse) / double(maskFlat ? maskFlat : 1),
                double(wordNz) / double(nzBricks ? nzBricks : 1), BRICK_WORDS);
    std::printf("    mask %.1f MB flat -> %.1f MB sparse  (%.1f %% saved)\n",
                double(maskFlat) / (1024.0 * 1024.0), double(maskSparse) / (1024.0 * 1024.0),
                100.0 * double(maskFlat - maskSparse) / double(maskFlat ? maskFlat : 1));

    std::printf("\n  DISTINCT MATERIALS per detail brick (a palette needs ceil(log2 n) bits):\n");
    uint64_t le2 = 0, le4 = 0, le16 = 0, tot = 0;
    for (const auto &kv : mtlHist) {
        tot += kv.second;
        if (kv.first <= 2) le2 += kv.second;
        if (kv.first <= 4) le4 += kv.second;
        if (kv.first <= 16) le16 += kv.second;
    }
    std::printf("    <=2 materials : %llu (%.0f %%)\n", (unsigned long long)le2,
                100.0 * double(le2) / double(tot ? tot : 1));
    std::printf("    <=4 materials : %llu (%.0f %%)\n", (unsigned long long)le4,
                100.0 * double(le4) / double(tot ? tot : 1));
    std::printf("    <=16 materials: %llu (%.0f %%)\n", (unsigned long long)le16,
                100.0 * double(le16) / double(tot ? tot : 1));
    std::printf("    run  %.1f MB flat -> %.1f MB packed (%.1f %% saved)\n",
                double(runFlat) / (1024.0 * 1024.0), double(runPacked) / (1024.0 * 1024.0),
                100.0 * double(runFlat - runPacked) / double(runFlat ? runFlat : 1));

    const double totalSave =
        double((maskFlat - maskSparse) + (runFlat - runPacked)) / (1024.0 * 1024.0);
    std::printf("\n  BOTH TOGETHER would take %.1f MB off a %.1f MB store (%.0f %%).\n", totalSave,
                double(p.bytes()) / (1024.0 * 1024.0),
                100.0 * totalSave / (double(p.bytes()) / (1024.0 * 1024.0)));
    return 0;
}
