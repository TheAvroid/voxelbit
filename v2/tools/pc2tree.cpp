// pc2tree -- cut individual MEASURED trees out of a lidar plot as .vox models.
//
//   g++ -O2 -std=c++17 -o pc2tree pc2tree.cpp
//   pc2tree <plot.las> --out <dir> [--voxel 0.10] [--min-points 800] [--limit 0]
//
// (LAZ first: tools/laz2las.exe in.laz out.las)
//
// WHAT THIS IS FOR. v2's trees are authored models -- fbx2vox turns three
// artist FBXs into oak_1..3.vox and every oak in the world is one of those
// three. FOR-instance carries a per-point `treeID` over plots scanned at
// 4.5 cm, so the trees in it are REAL ones, individually labelled, and at 0.1 m
// a 13 m tree is 135 voxels: comfortably inside the 256 a single XYZI chunk
// addresses. So this is measured tree geometry entering the same pipeline the
// authored models use, rather than replacing it.
//
// THE CLASS SPLIT IS THE DATASET'S OWN and it lands exactly where v2 wants it:
//
//     4  stem            -> BARK
//     6  woody branches  -> BARK
//     5  live branches   -> LEAF   (the readMe calls this "green crown")
//     3  out-points      -> skipped: trees outside the annotated plot
//
// fbx2vox had to recover that split from texture names because an FBX does not
// know what a leaf is. A classified point cloud does.
//
// THE PALETTE IS THE OAKS' OWN, BYTE FOR BYTE, AND THAT IS NOT OPTIONAL.
// v2's palette is FULL at 255. world.h folds oak wood onto the shared trunk
// colours and oak leaves onto the birches' greens, and loadModelSet matches
// with matchTol=30 -- so a NEW green here would not be a new colour, it would
// be a near-miss that gets folded somewhere unintended and is then invisible
// to debug. These are the same seven entries fbx2vox writes, and the same
// ambient-occlusion ranking decides which of them a voxel gets, so the output
// histogram matches the models it sits beside.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

namespace {

struct Pt { float x, y, z; uint8_t bark; };

// Lifted out of tools/fbx2vox.cpp -- see the note above about why.
const uint8_t kPal[7][3] = {
    {0x54, 0x4c, 0x33}, {0x65, 0x59, 0x43}, {0x71, 0x68, 0x58},           // bark 1..3
    {0x50, 0x70, 0x2f}, {0x69, 0x8f, 0x32}, {0x73, 0x96, 0x48}, {0x82, 0xa1, 0x65}};  // leaf 4..7
const double kBarkMix[3] = {0.514, 0.315, 0.171};
const double kLeafMix[4] = {0.381, 0.273, 0.224, 0.122};

uint32_t hashU(uint32_t a) {
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; a ^= a >> 16;
    return a;
}

}  // namespace

int main(int argc, char **argv) {
    std::string in, outDir;
    double voxel = 0.10;
    long minPoints = 800;
    int limit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--voxel" && i + 1 < argc) voxel = atof(argv[++i]);
        else if (a == "--min-points" && i + 1 < argc) minPoints = atol(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = atoi(argv[++i]);
        else if (a[0] != '-') in = a;
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (in.empty() || outDir.empty()) {
        fprintf(stderr, "pc2tree <plot.las> --out <dir> [--voxel 0.10] [--min-points 800]\n");
        return 2;
    }

    // ---- LAS header + the Extra Bytes VLR that carries treeID ------------
    FILE *f = fopen(in.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
    unsigned char h[375];
    if (fread(h, 1, 375, f) != 375 || memcmp(h, "LASF", 4)) {
        fprintf(stderr, "not a LAS file\n"); return 1;
    }
    const int minor = h[25];
    const int hsize = *(uint16_t *)(h + 94);
    const long long pOff = *(uint32_t *)(h + 96);
    const int nVlr = *(uint32_t *)(h + 100);
    const int fmt = h[104] & 0x3F;
    const int psize = *(uint16_t *)(h + 105);
    long long count = *(uint32_t *)(h + 107);
    double sx, sy, sz, ox, oy, oz;
    memcpy(&sx, h + 131, 8); memcpy(&sy, h + 139, 8); memcpy(&sz, h + 147, 8);
    memcpy(&ox, h + 155, 8); memcpy(&oy, h + 163, 8); memcpy(&oz, h + 171, 8);
    if (minor >= 4 && hsize >= 375) {
        const unsigned long long big = *(uint64_t *)(h + 247);
        if (big) count = (long long)big;
    }
    const bool newStyle = fmt >= 6;
    const int coreSize[11] = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
    const int core = (fmt >= 0 && fmt <= 10) ? coreSize[fmt] : 20;

    // Walk the VLRs for LASF_Spec / 4 (Extra Bytes) and find treeID's offset.
    int tidOff = -1;
    char tidType = 0;
    fseek(f, hsize, SEEK_SET);
    for (int i = 0; i < nVlr; ++i) {
        unsigned char vh[54];
        if (fread(vh, 1, 54, f) != 54) break;
        char user[17] = {0};
        memcpy(user, vh + 2, 16);
        const int recId = *(uint16_t *)(vh + 18);
        const int len = *(uint16_t *)(vh + 20);
        std::vector<unsigned char> body(len);
        if (len && fread(body.data(), 1, len, f) != (size_t)len) break;
        if (strcmp(user, "LASF_Spec") || recId != 4) continue;
        int off = core;
        const char tcode[11] = {0, 'B', 'b', 'H', 'h', 'I', 'i', 'Q', 'q', 'f', 'd'};
        const int tsize[11] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
        for (int e = 0; e + 192 <= len; e += 192) {
            const int dt = body[e + 2];
            char nm[33] = {0};
            memcpy(nm, &body[e + 4], 32);
            if (dt >= 1 && dt <= 10) {
                if (!strcmp(nm, "treeID")) { tidOff = off; tidType = tcode[dt]; }
                off += tsize[dt];
            }
        }
    }
    if (tidOff < 0) {
        fprintf(stderr, "no treeID extra dimension -- this plot is not annotated\n");
        return 1;
    }
    printf("in      %s\n        format %d, %lld points, treeID at +%d (%c)\n",
           in.c_str(), fmt, count, tidOff, tidType);

    // ---- gather points per tree ------------------------------------------
    std::map<long long, std::vector<Pt>> trees;
    std::vector<unsigned char> buf((size_t)psize * 65536);
    fseek(f, (long)pOff, SEEK_SET);
    long long read = 0, skippedOut = 0;
    while (read < count) {
        const size_t want = (size_t)std::min<long long>(65536, count - read);
        const size_t got = fread(buf.data(), psize, want, f);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) {
            const unsigned char *p = &buf[i * psize];
            const int cls = newStyle ? p[16] : (p[15] & 0x1F);
            if (cls == 3) { ++skippedOut; continue; }       // outside the plot
            if (cls != 4 && cls != 5 && cls != 6) continue; // not part of a tree
            long long tid = 0;
            if (tidType == 'd') { double d; memcpy(&d, p + tidOff, 8); tid = (long long)d; }
            else if (tidType == 'f') { float d; memcpy(&d, p + tidOff, 4); tid = (long long)d; }
            else if (tidType == 'I' || tidType == 'i') { int32_t d; memcpy(&d, p + tidOff, 4); tid = d; }
            else if (tidType == 'H' || tidType == 'h') { uint16_t d; memcpy(&d, p + tidOff, 2); tid = d; }
            else if (tidType == 'B') tid = p[tidOff];
            if (!tid) continue;
            Pt q;
            q.x = float(*(const int32_t *)(p + 0) * sx + ox);
            q.y = float(*(const int32_t *)(p + 4) * sy + oy);
            q.z = float(*(const int32_t *)(p + 8) * sz + oz);
            q.bark = (cls == 4 || cls == 6) ? 1 : 0;
            trees[tid].push_back(q);
        }
        read += (long long)got;
    }
    fclose(f);
    printf("trees   %zu annotated, %lld out-points skipped\n", trees.size(), skippedOut);

    // ---- one .vox per tree -----------------------------------------------
    int written = 0, tooBig = 0, tooSmall = 0;
    printf("\n%-8s %7s %7s %7s %7s %7s  %s\n",
           "id", "points", "h (m)", "w (m)", "voxels", "bark%", "file");
    for (auto &kv : trees) {
        if (limit && written >= limit) break;
        std::vector<Pt> &pts = kv.second;
        if ((long)pts.size() < minPoints) { ++tooSmall; continue; }
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const Pt &q : pts) {
            lo[0] = std::min(lo[0], q.x); hi[0] = std::max(hi[0], q.x);
            lo[1] = std::min(lo[1], q.y); hi[1] = std::max(hi[1], q.y);
            lo[2] = std::min(lo[2], q.z); hi[2] = std::max(hi[2], q.z);
        }
        const int nx = int((hi[0] - lo[0]) / voxel) + 1;
        const int ny = int((hi[1] - lo[1]) / voxel) + 1;
        const int nz = int((hi[2] - lo[2]) / voxel) + 1;
        if (nx > 256 || ny > 256 || nz > 256) {
            // A single XYZI chunk addresses 256 per axis; see fbx2vox. A tree
            // past that has to be split or coarsened, and silently cropping it
            // would produce a beheaded model that still loads.
            printf("%-8lld %7zu %7.2f %7.2f      --      --  SKIPPED: %dx%dx%d exceeds 256\n",
                   kv.first, pts.size(), hi[2] - lo[2], hi[0] - lo[0], nx, ny, nz);
            ++tooBig;
            continue;
        }
        // occupancy; bark wins over leaf where both land in a voxel, because a
        // branch running through foliage is still a branch.
        std::vector<uint8_t> occ((size_t)nx * ny * nz, 0);   // 0 empty, 1 leaf, 2 bark
        for (const Pt &q : pts) {
            const int ix = int((q.x - lo[0]) / voxel);
            const int iy = int((q.y - lo[1]) / voxel);
            const int iz = int((q.z - lo[2]) / voxel);
            if (ix < 0 || iy < 0 || iz < 0 || ix >= nx || iy >= ny || iz >= nz) continue;
            uint8_t &c = occ[((size_t)iz * ny + iy) * nx + ix];
            if (q.bark) c = 2; else if (!c) c = 1;
        }
        // ---- ambient occlusion, ranked, exactly as fbx2vox does it --------
        struct V { int x, y, z; uint8_t kind; int ao; };
        std::vector<V> vs;
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    const uint8_t c = occ[((size_t)z * ny + y) * nx + x];
                    if (!c) continue;
                    int ao = 0;
                    for (int dz = -2; dz <= 2; ++dz)
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx) {
                                const int a = x + dx, b = y + dy, e = z + dz;
                                if (a < 0 || b < 0 || e < 0 || a >= nx || b >= ny || e >= nz) continue;
                                if (occ[((size_t)e * ny + b) * nx + a]) ++ao;
                            }
                    // a little hash jitter so the quantile cuts do not draw
                    // straight lines through ties (fbx2vox does the same)
                    ao = ao * 64 + int(hashU(uint32_t(x * 73856093 ^ y * 19349663 ^ z * 83492791)) & 63);
                    vs.push_back({x, y, z, c, ao});
                }
        std::vector<uint32_t> bark, leaf;
        for (uint32_t i = 0; i < vs.size(); ++i) (vs[i].kind == 2 ? bark : leaf).push_back(i);
        auto rank = [&](std::vector<uint32_t> &v, const double *mix, int n, int base,
                        std::vector<uint8_t> &pal) {
            std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return vs[a].ao > vs[b].ao; });
            size_t at = 0;
            for (int s = 0; s < n; ++s) {
                size_t take = (s == n - 1) ? v.size() - at : size_t(mix[s] * double(v.size()) + 0.5);
                for (size_t k = 0; k < take && at < v.size(); ++k, ++at) pal[v[at]] = uint8_t(base + s);
            }
        };
        std::vector<uint8_t> pal(vs.size(), 1);
        rank(bark, kBarkMix, 3, 1, pal);
        rank(leaf, kLeafMix, 4, 4, pal);

        char path[512];
        snprintf(path, sizeof path, "%s/tree_%lld.vox", outDir.c_str(), kv.first);
        FILE *o = fopen(path, "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", path); continue; }
        std::vector<uint8_t> xyzi;
        xyzi.reserve(vs.size() * 4);
        for (uint32_t i = 0; i < vs.size(); ++i) {
            xyzi.push_back((uint8_t)vs[i].x); xyzi.push_back((uint8_t)vs[i].y);
            xyzi.push_back((uint8_t)vs[i].z); xyzi.push_back(pal[i]);
        }
        auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, o); };
        auto chunk = [&](const char *id, uint32_t c, uint32_t ch) {
            fwrite(id, 1, 4, o); u32(c); u32(ch);
        };
        fwrite("VOX ", 1, 4, o); u32(150);
        chunk("MAIN", 0, 12 + 12 + 12 + (uint32_t)(4 + xyzi.size()) + 12 + 1024);
        chunk("SIZE", 12, 0); u32(nx); u32(ny); u32(nz);
        chunk("XYZI", 4 + (uint32_t)xyzi.size(), 0);
        u32((uint32_t)vs.size());
        fwrite(xyzi.data(), 1, xyzi.size(), o);
        // The RGBA chunk is NOT optional -- without one the file is read
        // against MagicaVoxel's default palette. Entry i is written at i-1.
        chunk("RGBA", 1024, 0);
        for (int i = 0; i < 256; ++i) {
            uint8_t e[4] = {0, 0, 0, 255};
            if (i < 7) { e[0] = kPal[i][0]; e[1] = kPal[i][1]; e[2] = kPal[i][2]; }
            fwrite(e, 1, 4, o);
        }
        fclose(o);
        printf("%-8lld %7zu %7.2f %7.2f %7zu %6.1f%%  %s\n", kv.first, pts.size(),
               hi[2] - lo[2], hi[0] - lo[0], vs.size(),
               100.0 * double(bark.size()) / double(vs.size() ? vs.size() : 1),
               path + outDir.size() + 1);
        ++written;
    }
    printf("\nwrote   %d models (%d too small under --min-points, %d over 256 voxels)\n",
           written, tooSmall, tooBig);
    return 0;
}
