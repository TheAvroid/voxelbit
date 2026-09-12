// ---------------------------------------------------------------------------
// dedup_probe.cpp -- how much is there to win by SHARING identical bricks?
//
// This is the sparse-voxel-DAG's idea without the DAG's traversal. A DAG earns
// its compression by noticing that a subtree appears many times and storing it
// once; the price is a software descent, and every software descent this engine
// has measured has lost to the ray tracing cores -- v3 parked, the NanoVDB v5
// 4.6x slower, and the octant byte beaten twice inside the brick. So take the
// memory idea and leave the traversal alone: two bricks with the same payload
// are the same bytes, and one copy would do.
//
// The acceleration structure does NOT change. Every brick keeps its own header
// and its own AABB -- those are what make it a primitive at all. What could be
// shared is the mask and the material run.
//
// -- THIS PROBE WAS REWRITTEN 2026-09-11 BECAUSE ITS FIRST ANSWER WAS A LIE ---
//
// The first version read kBrickWords flat mask words at maskBase and one
// material byte per solid voxel. BOTH have been false since the word-sparse
// mask and the per-brick material palette landed. It therefore hashed a span
// that ran off the end of each brick and into its NEIGHBOUR's bytes -- and a
// hash over a neighbour is unique by construction, so it reported 97.8 % of
// bricks distinct and 1.3 % to be saved. That is not a measurement of the
// world, it is a measurement of the bug.
//
// The payload span is now DERIVED THE WAY THE SHADER DERIVES IT, and the total
// is checked against the packed buffer: if every brick's span is right, the
// spans tile p.mask exactly and the sum equals its size. That assertion is the
// only reason to believe any number below it.
//
//   mask      popcount(wordMap) words, wordMap in octants bits 8..23
//   material  at maskBase + that, and NOTHING if the brick is monochrome;
//             np = flags 16..21, np == 0 means raw bytes  -> ceil(solid/4)
//             words, otherwise ceil(np/4) palette words then the indices at
//             bits = ceil(log2(np)) each -> ceil(solid*bits/32) words.
//
// AND IT MEASURES THE LADDER, not just the brick. Dedup only pays when the
// space of possible nodes is smaller than the number of nodes you have: an 8^3
// mask is drawn from 2^512 and a 2^3 leaf from 2^8, so a DAG's compression all
// lives at the BOTTOM -- which is exactly the depth a descent has to reach.
// The three levels are reported side by side so that trade is a number.
//
// GPU-FREE. Same generator and packer the engine runs.
//     g++ -O2 -std=c++17 -I src tests/dedup_probe.cpp -o dedupprobe
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#include "scene/bricks.h"
#include "scene/generate.h"

using namespace v4;

// --------------------------------------------------------------------------
// WHERE A BRICK'S BYTES ARE. Derived exactly as stores/Aabb.slang derives it.
// --------------------------------------------------------------------------
struct Span {
    uint32_t maskAt = 0, maskWords = 0;  // the word-sparse occupancy
    uint32_t mtlAt = 0, mtlWords = 0;    // the palette + indices, or the raw run
    uint32_t solid = 0;                  // set bits in the mask
    uint32_t words() const { return maskWords + mtlWords; }
};

static Span spanOf(const PackedStore &p, const V4Brick &h) {
    Span s;
    const uint32_t wordMap = (h.octants >> 8) & 0xFFFFu;
    s.maskAt = h.maskBase;
    s.maskWords = popcount32(wordMap);
    for (uint32_t k = 0; k < s.maskWords; ++k) s.solid += popcount32(p.mask[s.maskAt + k]);

    s.mtlAt = s.maskAt + s.maskWords;
    if (h.flags & 2u) return s;  // monochrome: the id is in the header, no run

    const uint32_t np = (h.flags >> 16) & 0x3Fu;
    if (np == 0) {
        s.mtlWords = (s.solid + 3u) / 4u;  // RAW: one byte a voxel
    } else {
        uint32_t bits = 1;
        while ((1u << bits) < np) ++bits;
        s.mtlWords = (np + 3u) / 4u + (s.solid * bits + 31u) / 32u;
    }
    return s;
}

// The sparse words put back where they belong, for the sub-brick levels.
static void expand(const PackedStore &p, const V4Brick &h, uint32_t out[BRICK_WORDS]) {
    const uint32_t wordMap = (h.octants >> 8) & 0xFFFFu;
    uint32_t at = h.maskBase;
    for (int k = 0; k < BRICK_WORDS; ++k)
        out[k] = ((wordMap >> k) & 1u) ? p.mask[at++] : 0u;
}

// FNV-1a. A collision would merge two different bricks and paint one of them
// wrong, so a real implementation compares the bytes after the hash matches.
// For COUNTING distinct payloads the hash alone is enough.
struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void byte(uint8_t b) { h = (h ^ b) * 1099511628211ull; }
    void word(uint32_t w) {
        byte(uint8_t(w));
        byte(uint8_t(w >> 8));
        byte(uint8_t(w >> 16));
        byte(uint8_t(w >> 24));
    }
};

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

    std::printf("v4 dedup probe -- %.0f m, %d^3 bricks\n", o.extentM, kBrickE);
    std::printf("  models       %s\n", content.status.c_str());
    std::printf("  packed       %zu bricks (%llu uniform, %llu detail, %llu mono)\n",
                p.brick.size(), (unsigned long long)p.uniform, (unsigned long long)p.detail,
                (unsigned long long)p.mono);
    std::printf("  device       %.1f MB total, of which payload %.1f MB\n",
                double(p.bytes()) / 1048576.0, double(p.mask.size() * 4) / 1048576.0);

    // -- THE SPANS MUST TILE THE BUFFER --------------------------------------
    //
    // Every detail brick owns a contiguous run and they are emitted back to
    // back, so the runs sum to the buffer and each starts where the last ended.
    // If this does not hold the spans are wrong and every number below it is
    // the previous version's mistake in a new costume.
    {
        uint64_t total = 0;
        uint32_t at = 0;
        size_t bad = 0;
        for (const V4Brick &h : p.brick) {
            if (h.flags & 1u) continue;
            const Span s = spanOf(p, h);
            // A MONOCHROME BRICK WRITES mtlBase = 0 AND MEANS IT -- it has no
            // run, so the field is not an offset and checking it against one
            // flags exactly the mono bricks and nothing else.
            if (s.maskAt != at || (!(h.flags & 2u) && h.mtlBase != s.mtlAt * 4u)) ++bad;
            at = s.maskAt + s.words();
            total += s.words();
        }
        const bool ok = (total == p.mask.size() && bad == 0);
        std::printf("  span check   %llu words derived vs %zu packed -- %s\n",
                    (unsigned long long)total, p.mask.size(), ok ? "TILES EXACTLY" : "MISMATCH");
        if (!ok) {
            std::printf("  %zu bricks start in the wrong place. The layout moved again;\n"
                        "  fix spanOf() before reading anything below.\n",
                        bad);
            return 1;
        }
    }

    // -- LEVEL 1: THE WHOLE BRICK, WHICH IS WHAT WAS ASKED -------------------
    uint64_t payload = 0, shared = 0, geomFlat = 0, geomShared = 0;
    size_t detail = 0, distinct = 0, distinctGeom = 0;
    {
        std::unordered_set<uint64_t> seen, seenGeom;
        seen.reserve(p.brick.size() * 2);
        seenGeom.reserve(p.brick.size() * 2);
        for (const V4Brick &h : p.brick) {
            if (h.flags & 1u) continue;
            ++detail;
            const Span s = spanOf(p, h);
            const uint64_t bytes = uint64_t(s.words()) * 4ull;
            payload += bytes;
            geomFlat += uint64_t(s.maskWords) * 4ull;

            Fnv g;
            g.word((h.octants >> 8) & 0xFFFFu);  // the word map is part of the shape
            for (uint32_t k = 0; k < s.maskWords; ++k) g.word(p.mask[s.maskAt + k]);
            if (seenGeom.insert(g.h).second) {
                ++distinctGeom;
                geomShared += uint64_t(s.maskWords) * 4ull;
            }

            Fnv f = g;
            f.word(h.flags);  // the mono id and the palette size belong to the payload
            for (uint32_t k = 0; k < s.mtlWords; ++k) f.word(p.mask[s.mtlAt + k]);
            if (seen.insert(f.h).second) {
                ++distinct;
                shared += bytes;
            }
        }
    }

    auto pct = [](uint64_t from, uint64_t to) {
        return from ? 100.0 * (double(from) - double(to)) / double(from) : 0.0;
    };

    std::printf("\n  -- 8^3 brick, the granularity that keeps the RT cores --\n");
    std::printf("  mask+material    %zu -> %zu distinct (%.2fx)   %.1f MB -> %.1f MB  (%.1f %% saved)\n",
                detail, distinct, distinct ? double(detail) / double(distinct) : 0.0,
                double(payload) / 1048576.0, double(shared) / 1048576.0, pct(payload, shared));
    std::printf("  mask only        %zu -> %zu distinct (%.2fx)   %.1f MB -> %.1f MB  (%.1f %% saved)\n",
                detail, distinctGeom, distinctGeom ? double(detail) / double(distinctGeom) : 0.0,
                double(geomFlat) / 1048576.0, double(geomShared) / 1048576.0,
                pct(geomFlat, geomShared));

    // -- LEVEL 2 AND 3: WHERE A DAG'S COMPRESSION ACTUALLY LIVES -------------
    //
    // An 8^3 brick as a DAG is one internal node over eight 4^3 children; the
    // standard SVDAG stops there and stores the 4^3 as a 64-bit leaf mask. So
    // the two numbers that decide it are how many DISTINCT 4^3 leaves the world
    // has, and what the internal nodes cost that a flat mask does not pay.
    {
        std::unordered_set<uint64_t> leaf4;
        std::unordered_set<uint32_t> leaf2;
        uint64_t n4 = 0, n2 = 0, internalBytes = 0;
        for (const V4Brick &h : p.brick) {
            if (h.flags & 1u) continue;
            uint32_t m[BRICK_WORDS];
            expand(p, h, m);
            auto bit = [&](int lx, int ly, int lz) {
                const int b = lx | (ly << kBrickShift) | (lz << (2 * kBrickShift));
                return (m[b >> 5] >> (b & 31)) & 1u;
            };
            uint32_t childMask = 0;
            for (int oz = 0; oz < 2; ++oz)
                for (int oy = 0; oy < 2; ++oy)
                    for (int ox = 0; ox < 2; ++ox) {
                        uint64_t sub = 0;
                        for (int z = 0; z < 4; ++z)
                            for (int y = 0; y < 4; ++y)
                                for (int x = 0; x < 4; ++x)
                                    if (bit(ox * 4 + x, oy * 4 + y, oz * 4 + z))
                                        sub |= 1ull << (x | (y << 2) | (z << 4));
                        if (!sub) continue;
                        childMask |= 1u << (ox | (oy << 1) | (oz << 2));
                        ++n4;
                        leaf4.insert(sub);
                        for (int qz = 0; qz < 2; ++qz)
                            for (int qy = 0; qy < 2; ++qy)
                                for (int qx = 0; qx < 2; ++qx) {
                                    uint32_t l = 0;
                                    for (int z = 0; z < 2; ++z)
                                        for (int y = 0; y < 2; ++y)
                                            for (int x = 0; x < 2; ++x) {
                                                const int sx = qx * 2 + x, sy = qy * 2 + y,
                                                          sz = qz * 2 + z;
                                                if ((sub >> (sx | (sy << 2) | (sz << 4))) & 1ull)
                                                    l |= 1u << (x | (y << 1) | (z << 2));
                                            }
                                    if (!l) continue;
                                    ++n2;
                                    leaf2.insert(l);
                                }
                    }
            internalBytes += 4ull + 4ull * popcount32(childMask);  // childmask + a ptr per child
        }
        std::printf("\n  -- the ladder: how many nodes, and how many of them are DIFFERENT --\n");
        std::printf("  8^3 nodes        %zu -> %zu distinct (%.2fx)   drawn from 2^512\n", detail,
                    distinctGeom, distinctGeom ? double(detail) / double(distinctGeom) : 0.0);
        std::printf("  4^3 nodes        %llu -> %zu distinct (%.0fx)   drawn from 2^64\n",
                    (unsigned long long)n4, leaf4.size(),
                    leaf4.size() ? double(n4) / double(leaf4.size()) : 0.0);
        std::printf("  2^3 nodes        %llu -> %zu distinct (%.0fx)   drawn from 2^8, so at most 255\n",
                    (unsigned long long)n2, leaf2.size(),
                    leaf2.size() ? double(n2) / double(leaf2.size()) : 0.0);

        // The honest accounting for the one design that keeps the hardware:
        // a per-brick internal node (NOT shared -- each brick needs its own
        // child pointers) plus one shared copy of each distinct 4^3 leaf.
        const uint64_t dagBytes = internalBytes + uint64_t(leaf4.size()) * 8ull;
        std::printf("\n  -- geometry, flat against a 4^3-leaf DAG --\n");
        std::printf("  flat masks       %.1f MB\n", double(geomFlat) / 1048576.0);
        std::printf("  DAG              %.1f MB  = %.1f MB of per-brick child pointers"
                    " + %.1f MB of %zu shared leaves\n",
                    double(dagBytes) / 1048576.0, double(internalBytes) / 1048576.0,
                    double(leaf4.size() * 8) / 1048576.0, leaf4.size());
        std::printf("  net on geometry  %+.1f %%   (and geometry is %.0f %% of the payload)\n",
                    -pct(geomFlat, dagBytes), 100.0 * double(geomFlat) / double(payload));
    }

    std::printf("\n  Each brick keeps its own 32-byte header and 24-byte AABB either way --\n"
                "  those are what make it a primitive, and they are NOT dedupable. They are\n"
                "  %.1f MB of the %.1f MB device total.\n",
                double(p.brick.size() * 32 + p.aabb.size() * 4) / 1048576.0,
                double(p.bytes()) / 1048576.0);
    return 0;
}
