// Why the nuketown light pole hangs when it is cut, and what it costs to make
// it fall -- measured against the map itself rather than reasoned about.
//
// GPU-FREE ON PURPOSE. voxel/vox.h has no Falcor in it, so the whole level can
// be loaded, seated and flooded here in a second. That is what makes it
// affordable to ask the question of EVERY post in the map at once, and to fire
// several hundred test rounds at walls, instead of aiming the engine at one
// spot and reading a screenshot.
//
//   g++ -std=c++20 -O2 -I src tests/level_pole_test.cpp -o build/level_pole_test.exe
//
// It replicates, exactly and deliberately:
//   seatLevelOnItsFoundation   world.h -- fills gaps up to kLevelSeatVox
//   markLevelGrounded          world.h -- 6-connected flood from y == 0
//   dropLevelHangers           world.h -- the seed box, the per-seed visited
//                                         set, the `attached` memo, and the
//                                         cap / span / wasHeld / contact guards
//
// If any of those change in world.h this file is silently wrong, so it prints
// the constants it used.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "voxel/vox.h"

using namespace v2;

// ---- the constants under test, copied from world.h ------------------------
static constexpr int kLevelSeatVox = 4;
static constexpr int kLevelHangCap = 8000;
static constexpr int kMarkSpan = 12;      // kLevelHangSpan today
static constexpr int kIslandSpan = 48;    // what the proposal would allow an island
static constexpr float kEmbeddedFrac = 0.15f;
static constexpr int kLevelHangMax = 4;
static constexpr int kBulletChipVox = 1;
// A SINGLE ROUND DOES NOT SEVER A POST, and the first run of this test proved
// it by testing nothing: a radius-1 bite is seven voxels and a light pole is
// five across, so the post stayed joined and every candidate was skipped. What
// the screenshot shows is a post cut THROUGH, so that is what A cuts -- and B
// fires bursts rather than single rounds for the same reason.
static constexpr int kSeverVox = 3;
static constexpr int kBurst = 6;
static constexpr float VOXEL_M_ = 0.1f;
static constexpr float kLevelAtX = -4096.0f, kLevelAtY = 640.0f, kLevelAtZ = -4096.0f;

static int SX = 0, SY = 0, SZ = 0;
static std::vector<uint8_t> vol;
static std::vector<uint8_t> grounded;

static inline size_t idx(int x, int y, int z) {
    return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
}
static inline bool solid(int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
    return vol[idx(x, y, z)] != 0;
}
static const int kN[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

static void seatOnFoundation() {
    long long filled = 0;
    int columns = 0;
    for (int z = 0; z < SZ; ++z)
        for (int x = 0; x < SX; ++x) {
            if (!solid(x, 0, z)) continue;
            int top = 0;
            while (top + 1 < SY && solid(x, top + 1, z)) ++top;
            int next = -1;
            for (int d = 2; d <= kLevelSeatVox + 1 && top + d < SY; ++d)
                if (solid(x, top + d, z)) { next = top + d; break; }
            if (next < 0) continue;
            const uint8_t m = vol[idx(x, top, z)];
            for (int y = top + 1; y < next; ++y) { vol[idx(x, y, z)] = m; ++filled; }
            ++columns;
        }
    std::printf("  seated: %lld voxels filled under %d columns\n", filled, columns);
}

static void markGrounded() {
    grounded.assign(vol.size(), 0);
    std::vector<size_t> stack;
    long long n = 0;
    for (int z = 0; z < SZ; ++z)
        for (int x = 0; x < SX; ++x) {
            if (!solid(x, 0, z) || grounded[idx(x, 0, z)]) continue;
            stack.clear();
            stack.push_back(idx(x, 0, z));
            grounded[idx(x, 0, z)] = 1;
            while (!stack.empty()) {
                const size_t k = stack.back();
                stack.pop_back();
                ++n;
                const int yy = int(k / (size_t(SX) * size_t(SZ)));
                const int rem = int(k % (size_t(SX) * size_t(SZ)));
                const int zz = rem / SX, xx = rem % SX;
                for (const auto &nb : kN) {
                    const int ax = xx + nb[0], ay = yy + nb[1], az = zz + nb[2];
                    if (!solid(ax, ay, az)) continue;
                    const size_t nk = idx(ax, ay, az);
                    if (grounded[nk]) continue;
                    grounded[nk] = 1;
                    stack.push_back(nk);
                }
            }
        }
    std::printf("  grounded: %lld voxels stand on the foundation\n", n);
}

// ---------------------------------------------------------------------------
// THE TWO RULES.
//
// CURRENT: the flood is budgeted at kMarkSpan, and exhausting that budget is
// itself the verdict -- the piece is called attached. A light pole is 35 voxels
// tall, so it never survives the budget and never reaches the guards below it.
//
// PROPOSED: the flood is budgeted at kIslandSpan, which is a COST bound rather
// than a verdict, and the verdict is taken afterwards from what the piece turns
// out to be:
//   touching == 0                     -> an island. Nothing anywhere touches
//                                        it, so it is floating by definition.
//   span <= kMarkSpan && contact <= f -> shot-mark sized, as today.
//   otherwise                         -> masonry; it stays.
enum Rule { RULE_CURRENT, RULE_PROPOSED };

struct Freed {
    int size = 0, span = 0, touching = 0;
    float contact = 0.0f;
    // voxels over the bounding BOX -- world.h splits anything under kHullFillOk
    // into octants, so this says whether a post falls whole or in pieces.
    float fill = 0.0f;
    // solid map inside the piece's own bounding box that is NOT part of it --
    // an exact answer to the question kHullFillOk only estimates, because the
    // box is a superset of the convex hull.
    int foreignInBox = 0;
};

// One sweep over the box a bite leaves, replicating dropLevelHangers including
// the per-seed visited set and the `attached` memo -- both of which change the
// answer, so leaving either out would measure a sweep that does not exist.
static std::vector<Freed> sweep(int cx, int cy, int cz, int radiusVox, Rule rule) {
    std::vector<Freed> out;
    const int r = std::max(1, radiusVox) + 2;
    const int budget = (rule == RULE_CURRENT) ? kMarkSpan : kIslandSpan;
    std::unordered_set<uint32_t> attached, seen;
    std::vector<int> stack, comp;
    int pieces = 0;
    for (int sy = std::max(0, cy - r); sy <= std::min(SY - 1, cy + r) && pieces < kLevelHangMax; ++sy)
    for (int sz = std::max(0, cz - r); sz <= std::min(SZ - 1, cz + r) && pieces < kLevelHangMax; ++sz)
    for (int sx = std::max(0, cx - r); sx <= std::min(SX - 1, cx + r) && pieces < kLevelHangMax; ++sx) {
        if (!solid(sx, sy, sz)) continue;
        const uint32_t s0 = uint32_t(idx(sx, sy, sz));
        if (attached.count(s0)) continue;
        comp.clear(); stack.clear(); seen.clear();
        stack.push_back(int(s0)); seen.insert(s0);
        bool isGround = false, capped = false;
        int lo[3] = {sx, sy, sz}, hi[3] = {sx, sy, sz};
        while (!stack.empty()) {
            const int k = stack.back(); stack.pop_back();
            const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
            const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
            const int z = rem / SX, x = rem % SX;
            comp.push_back(k);
            if (y == 0) { isGround = true; break; }
            lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x);
            lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
            lo[2] = std::min(lo[2], z); hi[2] = std::max(hi[2], z);
            if (int(comp.size()) >= kLevelHangCap || hi[0] - lo[0] >= budget ||
                hi[1] - lo[1] >= budget || hi[2] - lo[2] >= budget) { capped = true; break; }
            bool met = false;
            for (const auto &nb : kN) {
                const int nx = x + nb[0], ny = y + nb[1], nz = z + nb[2];
                if (!solid(nx, ny, nz)) continue;
                const uint32_t nk = uint32_t(idx(nx, ny, nz));
                if (attached.count(nk)) { met = true; break; }
                if (seen.count(nk)) continue;
                seen.insert(nk);
                stack.push_back(int(nk));
            }
            if (met) { isGround = true; break; }
        }
        if (isGround || capped) {
            for (int k : comp) attached.insert(uint32_t(k));
            continue;
        }
        if (comp.empty()) continue;
        bool wasHeld = false;
        for (int k : comp)
            if (grounded[size_t(k)]) { wasHeld = true; break; }
        if (!wasHeld) continue;
        int touching = 0;
        for (int k : comp) {
            const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
            const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
            const int z = rem / SX, x = rem % SX;
            for (const auto &nb : kN) {
                const int ax = x + nb[0], ay = y + nb[1], az = z + nb[2];
                if (!solid(ax, ay, az)) continue;
                if (seen.count(uint32_t(idx(ax, ay, az)))) continue;
                ++touching; break;
            }
        }
        const float contact = float(touching) / std::max(1.0f, float(comp.size()));
        const int span = std::max(hi[0] - lo[0] + 1, std::max(hi[1] - lo[1] + 1, hi[2] - lo[2] + 1));
        bool free_ = false;
        if (rule == RULE_CURRENT) {
            free_ = (contact <= kEmbeddedFrac);
        } else {
            if (touching == 0) free_ = true;
            else if (span <= kMarkSpan && contact <= kEmbeddedFrac) free_ = true;
        }
        if (!free_) { for (int k : comp) attached.insert(uint32_t(k)); continue; }
        const float fill = float(comp.size()) /
                           std::max(1.0f, float(hi[0] - lo[0] + 1) * float(hi[1] - lo[1] + 1) *
                                          float(hi[2] - lo[2] + 1));
        int foreign = 0;
        {
            std::unordered_set<uint32_t> own(seen);
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int z = lo[2]; z <= hi[2]; ++z)
                    for (int x = lo[0]; x <= hi[0]; ++x)
                        if (solid(x, y, z) && !own.count(uint32_t(idx(x, y, z)))) ++foreign;
        }
        out.push_back({int(comp.size()), span, touching, contact, fill, foreign});
        ++pieces;
        // the real sweep removes the piece; do the same so a second seed in the
        // same box cannot find it again
        for (int k : comp) vol[size_t(k)] = 0;
    }
    return out;
}

static void bite(int cx, int cy, int cz, int rad) {
    for (int dy = -rad; dy <= rad; ++dy)
        for (int dz = -rad; dz <= rad; ++dz)
            for (int dx = -rad; dx <= rad; ++dx)
                if (dx * dx + dy * dy + dz * dz <= rad * rad && solid(cx + dx, cy + dy, cz + dz))
                    vol[idx(cx + dx, cy + dy, cz + dz)] = 0;
}

// ---- finding the posts ----------------------------------------------------
struct Pole { int x = 0, z = 0, y0 = 0, y1 = 0; };

static std::vector<Pole> findPoles(int minH) {
    std::vector<Pole> out;
    std::vector<uint8_t> claimed(vol.size(), 0);
    for (int z = 1; z < SZ - 1; ++z)
        for (int x = 1; x < SX - 1; ++x) {
            if (!solid(x, 0, z)) continue;
            int y = 0;
            while (y + 1 < SY && solid(x, y + 1, z)) ++y;
            int s = -1;
            for (int yy = y + 1; yy < SY; ++yy)
                if (solid(x, yy, z)) { s = yy; break; }
            if (s < 0) continue;
            int e = s;
            while (e + 1 < SY && solid(x, e + 1, z)) ++e;
            if (e - s + 1 < minH) continue;
            if (claimed[idx(x, s, z)]) continue;
            const int my = (s + e) / 2;
            int girth = 0;
            for (int dz = -4; dz <= 4; ++dz)
                for (int dx = -4; dx <= 4; ++dx)
                    if (solid(x + dx, my, z + dz)) ++girth;
            if (girth > 25) continue;
            for (int yy = s; yy <= e; ++yy)
                for (int dz = -4; dz <= 4; ++dz)
                    for (int dx = -4; dx <= 4; ++dx)
                        if (solid(x + dx, yy, z + dz)) claimed[idx(x + dx, yy, z + dz)] = 1;
            out.push_back({x, z, s, e});
        }
    return out;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "C:/voxelbit/game/assets/level/nuketown.vox";
    VoxModel mo;
    std::string err;
    if (!voxLoad(path, &mo, &err)) { std::printf("cannot load %s: %s\n", path, err.c_str()); return 1; }
    VoxAsset a = toWorldWhole(mo);
    SX = a.sx; SY = a.sy; SZ = a.sz;
    vol = a.a;
    std::printf("level %s: %d x %d x %d\n", path, SX, SY, SZ);
    std::printf("constants: seat %d  cap %d  markSpan %d  islandSpan %d  embedded %.2f\n",
                kLevelSeatVox, kLevelHangCap, kMarkSpan, kIslandSpan, double(kEmbeddedFrac));
    seatOnFoundation();
    markGrounded();
    const std::vector<uint8_t> pristine = vol;
    const std::vector<Pole> poles = findPoles(20);

    // =====================================================================
    // A. THE POSTS. Cut each at the ankle and ask both rules.
    // =====================================================================
    std::printf("\n== A. cutting each free-standing post at its base ==\n");
    std::printf("%-30s %7s %6s %9s %8s  %-8s %s\n", "post at (world x,y,z)", "vox", "span",
                "touching", "contact", "current", "proposed");
    int curFall = 0, propFall = 0, tested = 0;
    for (const Pole &p : poles) {
        const int cutY = p.y0 + 2;
        if (cutY + 1 >= p.y1) continue;
        vol = pristine;
        bite(p.x, cutY, p.z, kSeverVox);
        int sy = -1;
        for (int yy = cutY + 2; yy <= p.y1; ++yy) if (solid(p.x, yy, p.z)) { sy = yy; break; }
        if (sy < 0) continue;
        {   // reject candidates still joined to the map -- wall columns, not posts
            std::unordered_set<uint32_t> seen;
            std::vector<int> st;
            st.push_back(int(idx(p.x, sy, p.z)));
            seen.insert(uint32_t(idx(p.x, sy, p.z)));
            bool reaches = false;
            int n = 0;
            while (!st.empty() && n < 20000) {
                const int k = st.back(); st.pop_back(); ++n;
                const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
                const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
                const int z = rem / SX, x = rem % SX;
                if (y == 0) { reaches = true; break; }
                for (const auto &nb : kN) {
                    const int nx = x + nb[0], ny = y + nb[1], nz = z + nb[2];
                    if (!solid(nx, ny, nz)) continue;
                    const uint32_t nk = uint32_t(idx(nx, ny, nz));
                    if (seen.count(nk)) continue;
                    seen.insert(nk); st.push_back(int(nk));
                }
            }
            if (reaches || n >= 20000) continue;
        }
        ++tested;
        vol = pristine; bite(p.x, cutY, p.z, kSeverVox);
        const std::vector<Freed> cur = sweep(p.x, cutY, p.z, kSeverVox, RULE_CURRENT);
        vol = pristine; bite(p.x, cutY, p.z, kSeverVox);
        const std::vector<Freed> prop = sweep(p.x, cutY, p.z, kSeverVox, RULE_PROPOSED);
        if (!cur.empty()) ++curFall;
        if (!prop.empty()) ++propFall;
        const Freed f = prop.empty() ? Freed{} : prop[0];
        std::printf("(%7.1f,%6.1f,%7.1f) %7d %6d %9d %7.3f  %-8s %s\n",
                    double(kLevelAtX + float(p.x) * VOXEL_M_),
                    double(kLevelAtY + float(p.y0) * VOXEL_M_),
                    double(kLevelAtZ + float(p.z) * VOXEL_M_),
                    f.size, f.span, f.touching, double(f.fill),
                    cur.empty() ? "HANGS" : "falls", prop.empty() ? "HANGS" : "falls");
        std::printf("%-30s %7s %6s %9s %7d  foreign voxels inside its own box\n", "", "", "", "",
                    f.foreignInBox);
    }
    std::printf("\n%d posts tested: current drops %d, proposed drops %d\n", tested, curFall, propFall);

    // =====================================================================
    // B. THE REGRESSION. Fire at the map and count what each rule tears off.
    //
    // This is the number that matters, not A: "it should only leave the\n    // initial shot mark" was reported three times, and a rule that drops the
    // pole by also dropping window mullions has traded one complaint for the
    // louder one.
    // =====================================================================
    const int shots = 600;
    std::printf("\n== B. %d rounds into the map ==\n", shots);
    std::mt19937 rng(20260918);
    std::vector<size_t> surface;
    for (int y = 2; y < SY; ++y)
        for (int z = 0; z < SZ; z += 3)
            for (int x = 0; x < SX; x += 3) {
                if (!solid(x, y, z)) continue;
                bool air = false;
                for (const auto &nb : kN)
                    if (!solid(x + nb[0], y + nb[1], z + nb[2])) { air = true; break; }
                if (air) surface.push_back(idx(x, y, z));
            }
    std::shuffle(surface.begin(), surface.end(), rng);
    int curN = 0, curVox = 0, curWorst = 0, propN = 0, propVox = 0, propWorst = 0, propIsland = 0;
    for (int i = 0; i < shots && i < int(surface.size()); ++i) {
        const size_t k = surface[size_t(i)];
        const int y = int(k / (size_t(SX) * size_t(SZ)));
        const int rem = int(k % (size_t(SX) * size_t(SZ)));
        const int z = rem / SX, x = rem % SX;
        // A BURST, NOT A ROUND. Six rounds scattered over a 5-voxel patch is
        // what a player does to a wall and is what actually disconnects
        // anything; single rounds severed nothing at all in 600 tries, which
        // made the first run of this test report a reassuring pair of zeroes
        // that meant only that the two rules had never been asked a question.
        std::uniform_int_distribution<int> jit(-2, 2);
        int jx[kBurst], jy[kBurst], jz[kBurst];
        for (int b = 0; b < kBurst; ++b) { jx[b] = jit(rng); jy[b] = jit(rng); jz[b] = jit(rng); }
        vol = pristine;
        for (int b = 0; b < kBurst; ++b) bite(x + jx[b], y + jy[b], z + jz[b], kBulletChipVox);
        const std::vector<uint8_t> afterBurst = vol;
        for (const Freed &f : sweep(x, y, z, kBulletChipVox + 2, RULE_CURRENT)) {
            ++curN; curVox += f.size; curWorst = std::max(curWorst, f.span);
        }
        vol = afterBurst;
        for (const Freed &f : sweep(x, y, z, kBulletChipVox + 2, RULE_PROPOSED)) {
            ++propN; propVox += f.size; propWorst = std::max(propWorst, f.span);
            if (f.touching == 0) ++propIsland;
        }
    }
    std::printf("%-10s %8s %8s %10s\n", "rule", "pieces", "voxels", "worst span");
    std::printf("%-10s %8d %8d %10d\n", "current", curN, curVox, curWorst);
    std::printf("%-10s %8d %8d %10d   (%d islands)\n", "proposed", propN, propVox, propWorst,
                propIsland);
    return 0;
}
