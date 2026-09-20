// pc2vox -- voxelise a LAS point cloud directly, with no heightfield.
//
//   g++ -O2 -std=c++17 -o pc2vox pc2vox.cpp
//   pc2vox <in.las> [--voxel 0.10] [--ground 2] [--close 5] [--vox out.vox]
//
// (LAZ first: tools/laz2las.exe in.laz out.las)
//
// WHY THIS IS A DIFFERENT METHOD AND NOT A BETTER DEM.
// Everything else here turns points into a HEIGHT per column and then extrudes
// it. That is 2.5D by construction: one surface per column, so an undercut
// bank, a boulder you can crawl under, a cave mouth and the space beneath a
// branch are all unrepresentable no matter how good the source is. The engine's
// caves exist only because a separate carve field digs them back out.
//
// This marks OCCUPIED VOXELS instead. A point is a measurement of "something
// is here", which is exactly what a voxel is, so the conversion is direct and
// the 2.5D assumption never enters.
//
// THE HARD PART IS NOT MARKING, IT IS FILLING.
// Lidar returns are a SHELL -- the surface that was visible to the scanner.
// Terrain has to be solid or you see through the ground, and the naive fix
// (fill every column from its top return to the floor) throws away precisely
// the overhangs this method exists to keep, and also fills the space under a
// tree canopy with rock.
//
// So the fill is in two steps, and the order matters:
//
//   1. CLOSE SMALL GAPS, per column, up to --close voxels. A gap of two or
//      three voxels between returns is the scanner missing a sample; a gap of
//      forty is a real void. Closing only the small ones repairs sampling
//      noise without inventing a ceiling. The threshold is the one number
//      here that is a judgement, so it is a flag.
//   2. FILL DOWNWARD FROM THE LOWEST GROUND RETURN. Below the deepest thing
//      the scanner saw is bedrock -- nothing measured it and nothing ever
//      will, and it has to be solid. Above that, whatever the returns say.
//
// Vegetation is left as a SHELL on purpose. A canopy is mostly air and drawing
// it solid would be a lie that also costs a fortune in voxels.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

namespace {

struct Las {
    FILE *f = nullptr;
    int fmt = 0, psize = 0;
    long long count = 0;
    long long offset = 0;
    double sx = 1, sy = 1, sz = 1, ox = 0, oy = 0, oz = 0;
    double minx = 0, miny = 0, minz = 0, maxx = 0, maxy = 0, maxz = 0;
    bool newStyle = false;

    bool open(const std::string &p) {
        if (p.size() > 4 && p.substr(p.size() - 4) == ".laz") {
            fprintf(stderr, "%s is LAZ. Run tools/laz2las.exe on it first.\n", p.c_str());
            return false;
        }
        f = fopen(p.c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p.c_str()); return false; }
        unsigned char h[375];
        if (fread(h, 1, 375, f) != 375) return false;
        if (memcmp(h, "LASF", 4)) { fprintf(stderr, "not a LAS file\n"); return false; }
        const int minor = h[25];
        const int hsize = *(uint16_t *)(h + 94);
        offset = *(uint32_t *)(h + 96);
        fmt = h[104] & 0x3F;
        psize = *(uint16_t *)(h + 105);
        count = *(uint32_t *)(h + 107);
        memcpy(&sx, h + 131, 8); memcpy(&sy, h + 139, 8); memcpy(&sz, h + 147, 8);
        memcpy(&ox, h + 155, 8); memcpy(&oy, h + 163, 8); memcpy(&oz, h + 171, 8);
        memcpy(&maxx, h + 179, 8); memcpy(&minx, h + 187, 8);
        memcpy(&maxy, h + 195, 8); memcpy(&miny, h + 203, 8);
        memcpy(&maxz, h + 211, 8); memcpy(&minz, h + 219, 8);
        // LAS 1.4 moved the count to 64 bits and may leave the legacy field 0 --
        // a file that "has no points" and is half a gigabyte long.
        if (minor >= 4 && hsize >= 375) {
            const unsigned long long big = *(uint64_t *)(h + 247);
            if (big) count = (long long)big;
        }
        newStyle = fmt >= 6;
        return count > 0 && psize > 0;
    }
};

// A run-length column: which voxels in this column are solid.
struct Col { std::vector<uint8_t> v; };

}  // namespace

int main(int argc, char **argv) {
    std::string in, voxOut, insetOut;
    double voxel = 0.10;
    int close = 5;
    int slice = -1;
    std::set<int> groundCls{2};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--voxel" && i + 1 < argc) voxel = atof(argv[++i]);
        else if (a == "--close" && i + 1 < argc) close = atoi(argv[++i]);
        else if (a == "--vox" && i + 1 < argc) voxOut = argv[++i];
        else if (a == "--slice" && i + 1 < argc) slice = atoi(argv[++i]);
        else if (a == "--inset" && i + 1 < argc) insetOut = argv[++i];
        else if (a == "--ground" && i + 1 < argc) {
            groundCls.clear();
            char *s = argv[++i];
            for (char *t = strtok(s, ","); t; t = strtok(nullptr, ",")) groundCls.insert(atoi(t));
        } else if (a[0] != '-') in = a;
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (in.empty()) {
        fprintf(stderr, "pc2vox <in.las> [--voxel 0.10] [--ground 2,40] [--close 5] [--vox out.vox]\n");
        return 2;
    }
    Las las;
    if (!las.open(in)) return 1;

    const int nx = int((las.maxx - las.minx) / voxel) + 1;
    const int ny = int((las.maxy - las.miny) / voxel) + 1;
    const int nz = int((las.maxz - las.minz) / voxel) + 1;
    printf("in      %s\n        format %d, %lld points, %.2f x %.2f x %.2f m\n",
           in.c_str(), las.fmt, las.count,
           las.maxx - las.minx, las.maxy - las.miny, las.maxz - las.minz);
    printf("grid    %d x %d x %d at %.3f m  (%.1f M voxels)\n",
           nx, ny, nz, voxel, double(nx) * ny * nz / 1e6);
    if ((double)nx * ny * nz > 3e9) {
        fprintf(stderr, "that is too big; raise --voxel\n");
        return 1;
    }

    // occupancy + which of them are ground, one bit each, as bytes for clarity
    std::vector<uint8_t> occ((size_t)nx * ny * nz, 0);
    std::vector<uint8_t> gnd((size_t)nx * ny * nz, 0);

    // ---- mark ------------------------------------------------------------
    std::vector<unsigned char> buf((size_t)las.psize * 65536);
    fseek(las.f, (long)las.offset, SEEK_SET);
    long long read = 0, marked = 0, outside = 0;
    std::vector<long long> clsHist(256, 0);
    while (read < las.count) {
        const size_t want = (size_t)std::min<long long>(65536, las.count - read);
        const size_t got = fread(buf.data(), las.psize, want, las.f);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) {
            const unsigned char *p = &buf[i * las.psize];
            const int32_t X = *(const int32_t *)(p + 0);
            const int32_t Y = *(const int32_t *)(p + 4);
            const int32_t Z = *(const int32_t *)(p + 8);
            const int cls = las.newStyle ? p[16] : (p[15] & 0x1F);
            clsHist[cls]++;
            const double wx = X * las.sx + las.ox;
            const double wy = Y * las.sy + las.oy;
            const double wz = Z * las.sz + las.oz;
            const int ix = int((wx - las.minx) / voxel);
            const int iy = int((wy - las.miny) / voxel);
            const int iz = int((wz - las.minz) / voxel);
            if (ix < 0 || iy < 0 || iz < 0 || ix >= nx || iy >= ny || iz >= nz) { ++outside; continue; }
            const size_t k = ((size_t)iz * ny + iy) * nx + ix;
            if (!occ[k]) ++marked;
            occ[k] = 1;
            if (groundCls.count(cls)) gnd[k] = 1;
        }
        read += (long long)got;
    }
    printf("marked  %lld voxels from %lld points (%.2f points a voxel)%s\n",
           marked, read, marked ? double(read) / double(marked) : 0.0,
           outside ? "  [some points outside the header's own bbox]" : "");
    printf("class   ");
    for (int c = 0; c < 256; ++c)
        if (clsHist[c]) printf("%d:%lld%s ", c, clsHist[c], groundCls.count(c) ? "(gnd)" : "");
    printf("\n");

    // ---- fill ------------------------------------------------------------
    long long closed = 0, filled = 0, emptyCols = 0, overhangs = 0;
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix) {
            // gather the column
            int lowestGround = -1, topAny = -1, runs = 0;
            bool prev = false;
            for (int iz = 0; iz < nz; ++iz) {
                const size_t k = ((size_t)iz * ny + iy) * nx + ix;
                if (occ[k]) {
                    if (!prev) ++runs;
                    topAny = iz;
                    if (gnd[k] && lowestGround < 0) lowestGround = iz;
                    prev = true;
                } else prev = false;
            }
            if (topAny < 0) { ++emptyCols; continue; }
            if (runs > 1) ++overhangs;

            // 1. close small gaps -- scanner noise, not architecture
            int lastOcc = -1;
            for (int iz = 0; iz <= topAny; ++iz) {
                const size_t k = ((size_t)iz * ny + iy) * nx + ix;
                if (!occ[k]) continue;
                if (lastOcc >= 0) {
                    const int gap = iz - lastOcc - 1;
                    if (gap > 0 && gap <= close)
                        for (int t = lastOcc + 1; t < iz; ++t) {
                            occ[((size_t)t * ny + iy) * nx + ix] = 1;
                            ++closed;
                        }
                }
                lastOcc = iz;
            }
            // 2. bedrock: solid below the lowest GROUND return. Below what the
            //    scanner saw, nothing was measured and nothing ever will be.
            const int base = (lowestGround >= 0) ? lowestGround : -1;
            if (base > 0)
                for (int iz = 0; iz < base; ++iz) {
                    const size_t k = ((size_t)iz * ny + iy) * nx + ix;
                    if (!occ[k]) { occ[k] = 1; ++filled; }
                }
        }
    long long total = 0;
    for (size_t k = 0; k < occ.size(); ++k) total += occ[k];
    printf("close   %lld voxels closed in gaps of <= %d\n", closed, close);
    printf("fill    %lld voxels of bedrock below the lowest ground return\n", filled);
    printf("solid   %lld voxels (%.2f%% of the grid)\n", total,
           100.0 * double(total) / (double(nx) * ny * nz));
    printf("cols    %lld of %d with NO return at all (%.2f%%)%s\n",
           emptyCols, nx * ny, 100.0 * double(emptyCols) / (double(nx) * ny),
           emptyCols ? "   <- holes; lower --voxel or accept them" : "");
    printf("3D      %lld columns have more than one run -- these are the\n"
           "        overhangs a heightfield cannot represent at all\n", overhangs);

    // ---- a .vbins height inset for the engine ----------------------------
    //
    // The TOP GROUND voxel per column, as a DEVIATION from the patch's own
    // mean. Absolute elevation belongs to where the scan was flown -- dropping
    // a 340 m Norwegian plot into a 20 m Arkansas valley would punch a tower
    // through the world -- so only the texture travels and src/world/inset.h
    // adds it to whatever the DEM says locally.
    //
    // GROUND ONLY. The top of a column is usually canopy, and a heightfield
    // whose surface is the top of a tree is a plateau with a wood painted on
    // it. Columns with no ground return are marked and filled from their
    // neighbours below, because a hole in a heightfield is a pit.
    if (!insetOut.empty()) {
        std::vector<float> dev((size_t)nx * ny, 0.0f);
        std::vector<uint8_t> have((size_t)nx * ny, 0);
        double sum = 0.0;
        long long n2 = 0;
        for (int iy = 0; iy < ny; ++iy)
            for (int ix = 0; ix < nx; ++ix) {
                int topGnd = -1;
                for (int iz = nz - 1; iz >= 0; --iz) {
                    const size_t k = ((size_t)iz * ny + iy) * nx + ix;
                    if (occ[k] && gnd[k]) { topGnd = iz; break; }
                }
                if (topGnd < 0) continue;
                const float zz = float(las.minz + topGnd * voxel);
                dev[(size_t)iy * nx + ix] = zz;
                have[(size_t)iy * nx + ix] = 1;
                sum += zz; ++n2;
            }
        if (!n2) {
            fprintf(stderr, "no ground returns at all; --inset needs --ground to match\n");
        } else {
            const float mean = float(sum / double(n2));
            // fill holes from the nearest filled neighbour, a few sweeps --
            // cheap, and a heightfield may not have gaps
            for (int pass = 0; pass < 8; ++pass) {
                long long fixedN = 0;
                for (int iy = 0; iy < ny; ++iy)
                    for (int ix = 0; ix < nx; ++ix) {
                        const size_t k = (size_t)iy * nx + ix;
                        if (have[k]) continue;
                        float acc = 0; int c = 0;
                        const int dxs[4] = {-1, 1, 0, 0}, dys[4] = {0, 0, -1, 1};
                        for (int t = 0; t < 4; ++t) {
                            const int a = ix + dxs[t], b = iy + dys[t];
                            if (a < 0 || b < 0 || a >= nx || b >= ny) continue;
                            if (have[(size_t)b * nx + a]) { acc += dev[(size_t)b * nx + a]; ++c; }
                        }
                        if (c) { dev[k] = acc / float(c); have[k] = 2; ++fixedN; }
                    }
                for (size_t k = 0; k < have.size(); ++k) if (have[k] == 2) have[k] = 1;
                if (!fixedN) break;
            }
            long long stillEmpty = 0;
            for (size_t k = 0; k < have.size(); ++k)
                if (!have[k]) { dev[k] = mean; ++stillEmpty; }
            for (size_t k = 0; k < dev.size(); ++k) dev[k] -= mean;
            float lo2 = 1e30f, hi2 = -1e30f;
            for (float v : dev) { lo2 = std::min(lo2, v); hi2 = std::max(hi2, v); }
            struct { char magic[8]; int32_t w, h; float stepM, meanM; int32_t pad[8]; } hd{};
            memcpy(hd.magic, "VBINS01", 8);
            hd.w = nx; hd.h = ny; hd.stepM = (float)voxel; hd.meanM = mean;
            FILE *o = fopen(insetOut.c_str(), "wb");
            if (o) {
                fwrite(&hd, sizeof hd, 1, o);
                fwrite(dev.data(), 4, dev.size(), o);
                fclose(o);
                printf("inset   %s  %dx%d at %.3f m, deviation %.2f..%.2f m"
                       " (mean %.2f removed), %lld columns had no ground%s",
                       insetOut.c_str(), nx, ny, voxel, lo2, hi2, mean, stillEmpty,
                       "\n");
            }
        }
    }

    // ---- an ASCII cross-section, which is the real check -----------------
    //
    // The numbers above cannot tell you whether the RESULT IS A FOREST or a
    // block of mush: "58% of columns have two runs" is equally true of a good
    // voxelisation and of one where the fill ran away. A vertical slice shows
    // it at a glance -- ground along the bottom, air, then canopy riding above
    // it, with trunks connecting the two.
    if (slice >= 0 && slice < ny) {
        printf("\nslice   y = %d (x across, z up, '#' ground, '+' other, '.' air)\n", slice);
        const int zTop = std::min(nz - 1, nz);
        for (int iz = zTop - 1; iz >= 0; iz -= std::max(1, nz / 46)) {
            printf("  %5.1fm |", las.minz + iz * voxel);
            for (int ix = 0; ix < std::min(nx, 150); ++ix) {
                const size_t k = ((size_t)iz * ny + slice) * nx + ix;
                putchar(occ[k] ? (gnd[k] ? '#' : '+') : '.');
            }
            putchar('\n');
        }
    }

    // ---- optional .vox for eyeballing ------------------------------------
    if (!voxOut.empty()) {
        // MagicaVoxel addresses 256 per axis in one model, so this writes the
        // densest 256^3 corner rather than silently truncating at the origin.
        const int sx = std::min(nx, 256), sy = std::min(ny, 256), sz = std::min(nz, 256);
        int bx = 0, by = 0, bz = 0;
        long long best = -1;
        for (int oy2 = 0; oy2 + sy <= ny; oy2 += std::max(1, sy / 2))
            for (int ox2 = 0; ox2 + sx <= nx; ox2 += std::max(1, sx / 2)) {
                long long c = 0;
                for (int z = 0; z < sz; z += 4)
                    for (int y = 0; y < sy; y += 4)
                        for (int x = 0; x < sx; x += 4)
                            c += occ[((size_t)z * ny + (oy2 + y)) * nx + (ox2 + x)];
                if (c > best) { best = c; bx = ox2; by = oy2; bz = 0; }
            }
        std::vector<uint8_t> xyzi;
        long long n = 0;
        for (int z = 0; z < sz; ++z)
            for (int y = 0; y < sy; ++y)
                for (int x = 0; x < sx; ++x) {
                    const size_t k = ((size_t)(bz + z) * ny + (by + y)) * nx + (bx + x);
                    if (!occ[k]) continue;
                    xyzi.push_back((uint8_t)x); xyzi.push_back((uint8_t)y);
                    xyzi.push_back((uint8_t)z);
                    xyzi.push_back(gnd[k] ? 1 : 2);   // 1 = ground, 2 = the rest
                    ++n;
                }
        FILE *f = fopen(voxOut.c_str(), "wb");
        if (f) {
            auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
            auto chunk = [&](const char *id, uint32_t c, uint32_t ch) {
                fwrite(id, 1, 4, f); u32(c); u32(ch);
            };
            fwrite("VOX ", 1, 4, f); u32(150);
            chunk("MAIN", 0, 12 + 12 + 12 + (uint32_t)(4 + xyzi.size()) + 12 + 1024);
            chunk("SIZE", 12, 0); u32(sx); u32(sy); u32(sz);
            chunk("XYZI", 4 + (uint32_t)xyzi.size(), 0);
            u32((uint32_t)n);
            fwrite(xyzi.data(), 1, xyzi.size(), f);
            // The RGBA chunk is not optional -- a file without one is read
            // against MagicaVoxel's default palette. (see fbx2vox.cpp)
            chunk("RGBA", 1024, 0);
            for (int i = 0; i < 256; ++i) {
                uint8_t e[4] = {90, 80, 70, 255};
                if (i == 0) { e[0] = 120; e[1] = 95; e[2] = 70; }     // ground
                else if (i == 1) { e[0] = 60; e[1] = 120; e[2] = 55; } // everything else
                fwrite(e, 1, 4, f);
            }
            fclose(f);
            printf("vox     %s  %lld voxels, %dx%dx%d from corner (%d,%d)\n",
                   voxOut.c_str(), n, sx, sy, sz, bx, by);
        }
    }
    fclose(las.f);
    return 0;
}
