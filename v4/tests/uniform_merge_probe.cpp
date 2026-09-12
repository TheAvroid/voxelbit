// ---------------------------------------------------------------------------
// uniform_merge_probe.cpp -- how much is there to win by merging uniform bricks?
//
// A uniform brick is solid and monochrome, so a run of adjacent ones is a BOX,
// and a box needs ONE AABB rather than one per brick. That is the AABB analogue
// of greedy meshing, and it would cut BVH primitives. Whether it is worth
// building depends entirely on whether the uniform bricks that SURVIVE the
// hidden-brick cull form runs or are scattered singletons -- the cull keeps the
// ones with an exposed face, which is a shell, and a shell may not merge.
//
// GPU-FREE. This is the same generator and packer the engine runs.
//     g++ -O2 -std=c++17 -I src tests/uniform_merge_probe.cpp -o umprobe
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "scene/bricks.h"
#include "scene/generate.h"

using namespace v4;

struct Key {
    int x, y, z;
    bool operator<(const Key &o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
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

    std::printf("world %.0f m, %d^3 bricks, %zu chunks, %zu bricks packed\n",
                o.extentM, kBrickE, p.chunk.size(), p.brick.size());
    std::printf("  uniform %llu  detail %llu  dropped %llu\n",
                (unsigned long long)p.uniform, (unsigned long long)p.detail,
                (unsigned long long)p.hidden);

    // -- greedy 3D box merge of uniform bricks, WITHIN a chunk ---------------
    // A merged box may not cross a chunk seam: a chunk is one bottom-level
    // structure and its AABBs are a contiguous slice of one buffer.
    size_t uniformTotal = 0, boxes = 0, biggest = 0;
    std::map<size_t, size_t> runHist;  // bricks-per-box -> how many boxes

    for (size_t c = 0; c < p.chunk.size(); ++c) {
        std::map<Key, uint32_t> u;  // brick coord -> material, uniform only
        const uint32_t first = p.chunk[c].first, n = p.chunk[c].count;
        for (uint32_t i = first; i < first + n; ++i) {
            const V4Brick &b = p.brick[i];
            if (!(b.flags & 1u)) continue;
            u[{b.ox / BRICK_E, b.oy / BRICK_E, b.oz / BRICK_E}] = (b.flags >> 8) & 0xFFu;
        }
        uniformTotal += u.size();

        std::map<Key, bool> used;
        for (const auto &kv : u) {
            if (used[kv.first]) continue;
            const uint32_t m = kv.second;
            const Key s = kv.first;
            // extend +x, then +y for the whole x-span, then +z for the slab
            int ex = 0;
            while (true) {
                Key k{s.x + ex + 1, s.y, s.z};
                auto it = u.find(k);
                if (it == u.end() || it->second != m || used[k]) break;
                ++ex;
            }
            int ey = 0;
            while (true) {
                bool ok = true;
                for (int dx = 0; dx <= ex && ok; ++dx) {
                    Key k{s.x + dx, s.y + ey + 1, s.z};
                    auto it = u.find(k);
                    if (it == u.end() || it->second != m || used[k]) ok = false;
                }
                if (!ok) break;
                ++ey;
            }
            int ez = 0;
            while (true) {
                bool ok = true;
                for (int dx = 0; dx <= ex && ok; ++dx)
                    for (int dy = 0; dy <= ey && ok; ++dy) {
                        Key k{s.x + dx, s.y + dy, s.z + ez + 1};
                        auto it = u.find(k);
                        if (it == u.end() || it->second != m || used[k]) ok = false;
                    }
                if (!ok) break;
                ++ez;
            }
            for (int dx = 0; dx <= ex; ++dx)
                for (int dy = 0; dy <= ey; ++dy)
                    for (int dz = 0; dz <= ez; ++dz) used[{s.x + dx, s.y + dy, s.z + dz}] = true;
            const size_t vol = size_t(ex + 1) * size_t(ey + 1) * size_t(ez + 1);
            ++boxes;
            if (vol > biggest) biggest = vol;
            runHist[vol > 8 ? 9 : vol]++;
        }
    }

    const size_t before = p.brick.size();
    const size_t after = before - uniformTotal + boxes;
    std::printf("\nuniform bricks in the structure : %zu\n", uniformTotal);
    std::printf("after greedy 3D merge           : %zu boxes  (largest %zu bricks)\n", boxes,
                biggest);
    std::printf("AABBs %zu -> %zu   (%.1f %% fewer)\n", before, after,
                100.0 * double(before - after) / double(before ? before : 1));
    std::printf("\nbox size histogram (bricks per box):\n");
    for (const auto &kv : runHist)
        std::printf("  %s%zu%s : %zu boxes\n", kv.first == 9 ? ">" : "", kv.first == 9 ? size_t(8) : kv.first,
                    kv.first == 9 ? "" : " ", kv.second);
    return 0;
}
