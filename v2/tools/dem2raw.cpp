// dem2raw -- USGS 3DEP GeoTIFF -> flat float32 heightmap for the voxel world.
//
// Mosaics a lon/lat window out of the one-degree 3DEP tiles and writes a .vbdem:
// a small header followed by w*h float32 elevations in metres, row-major, north
// up. The engine memory-maps that and samples it; nothing at runtime has to know
// what a TIFF is.
//
// These tiles are LZW (compression 5), floating-point predictor (3), tiled
// 512x512, 32-bit float. The predictor is the trap: each row is stored as byte
// PLANES after a byte-wise delta, and plane 0 holds the MOST significant byte,
// so for a little-endian file the planes go back in REVERSE order. Get it wrong
// and the header still parses, the extent and nodata are right, and every
// elevation comes out as a plausible-looking float like 962949611520.0. It is
// pixel-for-pixel silent. (libtiff tif_predict.c: fpAcc)
//
//   g++ -O2 -std=c++17 -o dem2raw dem2raw.cpp -lz
//
//   dem2raw --dir <tiledir> --prefix USGS_13 --out co.vbdem \
//           --center <lon> <lat> --km <size>
//
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <zlib.h>

#ifndef M_PI
constexpr double M_PI = 3.14159265358979323846;
#endif

namespace {

// ---------------------------------------------------------------- tiff reader

struct Tag { uint16_t type = 0; uint64_t count = 0; std::vector<uint8_t> raw; };

struct Tiff {
    FILE *f = nullptr;
    bool le = true, big = false;
    std::map<uint16_t, Tag> tags;

    ~Tiff() { if (f) fclose(f); }

    template <class T> T sw(T v) const {
        // The host is little-endian everywhere this runs; swap only for "MM".
        if (le) return v;
        T o; const uint8_t *s = (const uint8_t *)&v; uint8_t *d = (uint8_t *)&o;
        for (size_t i = 0; i < sizeof(T); ++i) d[i] = s[sizeof(T) - 1 - i];
        return o;
    }

    static size_t typeSize(uint16_t t) {
        switch (t) {
            case 1: case 2: case 6: case 7: return 1;
            case 3: case 8: return 2;
            case 4: case 9: case 11: return 4;
            case 5: case 10: case 12: case 16: case 17: case 18: return 8;
            default: return 0;
        }
    }

    bool open(const std::string &path) {
        f = fopen(path.c_str(), "rb");
        if (!f) return false;
        uint8_t h[8];
        if (fread(h, 1, 8, f) != 8) return false;
        if (h[0] == 'I' && h[1] == 'I') le = true;
        else if (h[0] == 'M' && h[1] == 'M') le = false;
        else return false;
        uint16_t ver; memcpy(&ver, h + 2, 2); ver = sw(ver);
        uint64_t off;
        if (ver == 42) { uint32_t o; memcpy(&o, h + 4, 4); off = sw(o); }
        else if (ver == 43) {
            big = true;
            uint8_t h2[8]; if (fread(h2, 1, 8, f) != 8) return false;
            uint64_t o; memcpy(&o, h2, 8); off = sw(o);
        } else return false;
        return readIFD(off);
    }

    bool readIFD(uint64_t off) {
        if (fseek(f, (long)off, SEEK_SET) != 0) return false;
        uint64_t n;
        if (big) { uint64_t v; if (fread(&v, 8, 1, f) != 1) return false; n = sw(v); }
        else     { uint16_t v; if (fread(&v, 2, 1, f) != 1) return false; n = sw(v); }
        const size_t esz = big ? 20 : 12;
        std::vector<uint8_t> buf(esz * n);
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) return false;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t *e = buf.data() + i * esz;
            uint16_t id, ty; memcpy(&id, e, 2); memcpy(&ty, e + 2, 2);
            id = sw(id); ty = sw(ty);
            uint64_t cnt; const uint8_t *rest;
            if (big) { uint64_t c; memcpy(&c, e + 4, 8); cnt = sw(c); rest = e + 12; }
            else     { uint32_t c; memcpy(&c, e + 4, 4); cnt = sw(c); rest = e + 8; }
            const size_t ts = typeSize(ty);
            if (!ts) continue;
            const size_t total = ts * (size_t)cnt, inl = big ? 8 : 4;
            Tag t; t.type = ty; t.count = cnt; t.raw.resize(total);
            if (total <= inl) memcpy(t.raw.data(), rest, total);
            else {
                uint64_t p;
                if (big) { uint64_t v; memcpy(&v, rest, 8); p = sw(v); }
                else     { uint32_t v; memcpy(&v, rest, 4); p = sw(v); }
                const long cur = ftell(f);
                if (fseek(f, (long)p, SEEK_SET) != 0) continue;
                if (fread(t.raw.data(), 1, total, f) != total) continue;
                fseek(f, cur, SEEK_SET);
            }
            tags[id] = std::move(t);
        }
        return true;
    }

    uint64_t num(uint16_t id, size_t i = 0, uint64_t dflt = 0) const {
        auto it = tags.find(id);
        if (it == tags.end() || i >= it->second.count) return dflt;
        const Tag &t = it->second;
        const uint8_t *p = t.raw.data() + i * typeSize(t.type);
        switch (t.type) {
            case 1: case 2: case 6: case 7: return *p;
            case 3: case 8:  { uint16_t v; memcpy(&v, p, 2); return sw(v); }
            case 4: case 9:  { uint32_t v; memcpy(&v, p, 4); return sw(v); }
            case 16: case 17:{ uint64_t v; memcpy(&v, p, 8); return sw(v); }
            default: return dflt;
        }
    }

    double dbl(uint16_t id, size_t i, double dflt = 0.0) const {
        auto it = tags.find(id);
        if (it == tags.end() || i >= it->second.count) return dflt;
        const Tag &t = it->second;
        if (t.type == 12) { double v; memcpy(&v, t.raw.data() + i * 8, 8); return sw(v); }
        if (t.type == 11) { float  v; memcpy(&v, t.raw.data() + i * 4, 4); return sw(v); }
        return dflt;
    }

    double nodata() const {
        auto it = tags.find(42113);
        if (it == tags.end()) return -999999.0;
        std::string s((const char *)it->second.raw.data(), it->second.raw.size());
        return atof(s.c_str());
    }
};

// ------------------------------------------------------------------ lzw (tiff)

void lzwDecode(const uint8_t *in, size_t n, std::vector<uint8_t> &out) {
    out.clear();
    std::vector<std::string> dict;
    dict.reserve(4096);
    auto reset = [&] { dict.clear(); for (int i = 0; i < 256; ++i) dict.push_back(std::string(1, (char)i)); dict.resize(258); };
    reset();
    int width = 9;
    size_t pos = 0; const size_t nbits = n * 8;
    std::string prev;
    while (pos + width <= nbits) {
        const size_t byte = pos >> 3; const int bit = pos & 7;
        uint32_t chunk = 0;
        for (int k = 0; k < 3; ++k) chunk = (chunk << 8) | (byte + k < n ? in[byte + k] : 0);
        const int code = (chunk >> (24 - bit - width)) & ((1 << width) - 1);
        pos += width;
        if (code == 256) { reset(); width = 9; prev.clear(); continue; }
        if (code == 257) break;
        std::string entry;
        if (code < (int)dict.size() && !(code >= 256 && code < 258)) entry = dict[code];
        else if (!prev.empty()) entry = prev + prev[0];
        else break;
        out.insert(out.end(), entry.begin(), entry.end());
        if (!prev.empty()) dict.push_back(prev + entry[0]);
        prev = entry;
        if ((int)dict.size() + 1 >= (1 << width) && width < 12) ++width;
    }
}

// --------------------------------------------------------------- one dem tile

struct DemTile {
    Tiff t;
    int w = 0, h = 0, tw = 0, th = 0, bpp = 4, pred = 1, comp = 1;
    bool tiled = false;
    double ox = 0, oy = 0, sx = 0, sy = 0, nd = -999999.0;
    std::vector<uint64_t> offs, cnts;
    // A ROW-MAJOR SWEEP NEEDS A BLOCK-ROW, NOT A BLOCK. With one cached block
    // the sweep re-decodes every 512x512 LZW block once per scanline -- 512x
    // the work -- because x crosses all the blocks in a row before y advances.
    // An LRU deep enough to hold one block-row turns that back into one decode
    // per block. 48 blocks of 512x512 float is 48 MB, and a 10812-wide tile is
    // 22 blocks across.
    static constexpr size_t kCacheBlocks = 48;
    mutable std::map<int, std::vector<float>> cache_;
    mutable std::vector<int> lru_;

    bool open(const std::string &path) {
        if (!t.open(path)) return false;
        w = (int)t.num(256); h = (int)t.num(257);
        bpp = (int)t.num(258, 0, 32) / 8;
        comp = (int)t.num(259, 0, 1);
        pred = (int)t.num(317, 0, 1);
        nd = t.nodata();
        sx = t.dbl(33550, 0); sy = t.dbl(33550, 1);
        ox = t.dbl(33922, 3); oy = t.dbl(33922, 4);
        if (t.tags.count(324)) {
            tiled = true; tw = (int)t.num(322); th = (int)t.num(323);
            const Tag &o = t.tags[324], &c = t.tags[325];
            for (uint64_t i = 0; i < o.count; ++i) offs.push_back(t.num(324, i));
            for (uint64_t i = 0; i < c.count; ++i) cnts.push_back(t.num(325, i));
        } else {
            tiled = false; tw = w; th = (int)t.num(278, 0, h);
            const Tag &o = t.tags[273];
            for (uint64_t i = 0; i < o.count; ++i) offs.push_back(t.num(273, i));
            for (uint64_t i = 0; i < t.tags[279].count; ++i) cnts.push_back(t.num(279, i));
        }
        return w > 0 && h > 0 && !offs.empty() && sx != 0 && sy != 0;
    }

    // Undo the floating-point predictor: byte-wise delta per row, then put the
    // byte planes back. Plane 0 is the MOST significant byte -- reverse for LE.
    void unpredict(std::vector<uint8_t> &b, int rowW, int rows) const {
        if (pred != 3) {
            if (pred == 2) {
                const int stride = rowW * bpp;
                for (int r = 0; r < rows; ++r) {
                    uint8_t *p = b.data() + (size_t)r * stride;
                    for (int x = bpp; x < stride; ++x) p[x] = (uint8_t)(p[x] + p[x - bpp]);
                }
            }
            return;
        }
        const int stride = rowW * bpp;
        std::vector<uint8_t> tmp(stride);
        for (int r = 0; r < rows; ++r) {
            if ((size_t)(r + 1) * stride > b.size()) break;
            uint8_t *p = b.data() + (size_t)r * stride;
            for (int x = 1; x < stride; ++x) p[x] = (uint8_t)(p[x] + p[x - 1]);
            memcpy(tmp.data(), p, stride);
            for (int k = 0; k < bpp; ++k) {
                const int dst = t.le ? (bpp - 1 - k) : k;
                const uint8_t *plane = tmp.data() + (size_t)k * rowW;
                for (int i = 0; i < rowW; ++i) p[i * bpp + dst] = plane[i];
            }
        }
    }

    const std::vector<float> &block(int idx) const {
        auto hit = cache_.find(idx);
        if (hit != cache_.end()) return hit->second;
        if (cache_.size() >= kCacheBlocks) {
            cache_.erase(lru_.front());
            lru_.erase(lru_.begin());
        }
        std::vector<float> &cache = cache_[idx];
        lru_.push_back(idx);
        cache.assign((size_t)tw * th, (float)nd);
        if (idx < 0 || idx >= (int)offs.size()) return cache;
        std::vector<uint8_t> raw(cnts[idx]);
        FILE *fp = t.f;
        if (fseek(fp, (long)offs[idx], SEEK_SET) != 0) return cache;
        if (fread(raw.data(), 1, raw.size(), fp) != raw.size()) return cache;
        std::vector<uint8_t> data;
        if (comp == 1) data.swap(raw);
        else if (comp == 5) lzwDecode(raw.data(), raw.size(), data);
        else if (comp == 8 || comp == 32946) {
            data.resize((size_t)tw * th * bpp);
            uLongf dl = (uLongf)data.size();
            if (uncompress(data.data(), &dl, raw.data(), (uLong)raw.size()) != Z_OK) return cache;
            data.resize(dl);
        } else return cache;
        data.resize((size_t)tw * th * bpp, 0);
        unpredict(data, tw, th);
        memcpy(cache.data(), data.data(), std::min(data.size(), cache.size() * 4));
        return cache;
    }

    float at(int px, int py) const {
        if (px < 0 || py < 0 || px >= w || py >= h) return (float)nd;
        const int across = tiled ? (w + tw - 1) / tw : 1;
        const int bx = tiled ? px / tw : 0, by = py / th;
        const std::vector<float> &v = block(by * across + bx);
        const int lx = tiled ? px % tw : px, ly = py % th;
        const size_t k = (size_t)ly * tw + lx;
        return k < v.size() ? v[k] : (float)nd;
    }

    bool covers(double lon, double lat) const {
        return lon >= ox && lon < ox + sx * w && lat <= oy && lat > oy - sy * h;
    }
    // Bilinear in pixel space; nodata poisons the sample so the caller can fill.
    float sample(double lon, double lat) const {
        const double fx = (lon - ox) / sx - 0.5, fy = (oy - lat) / sy - 0.5;
        const int x0 = (int)floor(fx), y0 = (int)floor(fy);
        const double tx = fx - x0, ty = fy - y0;
        float acc = 0; double wsum = 0;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const float v = at(x0 + dx, y0 + dy);
                if (v == (float)nd || !std::isfinite(v)) continue;
                const double wgt = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
                acc += (float)(v * wgt); wsum += wgt;
            }
        return wsum > 1e-9 ? (float)(acc / wsum) : (float)nd;
    }
};

// ------------------------------------------------------------------ the mosaic

struct Mosaic {
    std::string dir, prefix;
    std::map<std::string, std::unique_ptr<DemTile>> open_;

    static std::string nameFor(double lon, double lat) {
        // tiles are named by their NORTHWEST corner
        const int la = (int)ceil(lat), lo = (int)ceil(-lon);
        char b[32]; snprintf(b, sizeof b, "n%02dw%03d", la, lo);
        return b;
    }
    DemTile *tileFor(double lon, double lat) {
        const std::string key = nameFor(lon, lat);
        auto it = open_.find(key);
        if (it != open_.end()) return it->second.get();
        auto t = std::make_unique<DemTile>();
        const std::string path = dir + "/" + prefix + "_" + key + ".tif";
        if (!t->open(path)) { open_[key] = nullptr; return nullptr; }
        DemTile *p = t.get(); open_[key] = std::move(t); return p;
    }
    float sample(double lon, double lat) {
        DemTile *t = tileFor(lon, lat);
        if (!t) return NAN;
        const float v = t->sample(lon, lat);
        return (v == (float)t->nd) ? NAN : v;
    }
};

#pragma pack(push, 1)
struct Header {
    char     magic[8];      // "VBDEM01"
    int32_t  w, h;          // samples
    double   originLon;     // NW corner, degrees
    double   originLat;
    double   stepLon;       // degrees per sample, east-positive
    double   stepLat;       // degrees per sample, south-positive
    double   metresPerSampleX;
    double   metresPerSampleY;
    float    minM, maxM;
    int32_t  pad[8];
};
#pragma pack(pop)

} // namespace

int main(int argc, char **argv) {
    std::string dir = "C:/geo/dem/colorado/13arcsec_10m", prefix = "USGS_13", out = "co.vbdem";
    double clon = -106.4453, clat = 39.1178, km = 40.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--center" && i + 2 < argc) { clon = atof(argv[++i]); clat = atof(argv[++i]); }
        else if (a == "--km" && i + 1 < argc) km = atof(argv[++i]);
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    // Square window in METRES on the ground, converted to degrees at this
    // latitude. Longitude degrees shrink with cos(lat) -- at 39N a degree of
    // longitude is 86 km, not 111 -- so a square in degrees is a rectangle on
    // the ground, and the world wants the ground to be square.
    const double mPerDegLat = 111132.0;
    const double mPerDegLon = 111320.0 * cos(clat * M_PI / 180.0);
    const double halfLat = (km * 1000.0 * 0.5) / mPerDegLat;
    const double halfLon = (km * 1000.0 * 0.5) / mPerDegLon;

    // SQUARE ON THE GROUND, NOT IN DEGREES. The source is posted on a
    // geographic grid, so one step of longitude is cos(lat) shorter than one
    // step of latitude -- 8.00 m against 10.29 m at 39N. Sampling that
    // straight into a voxel world stretches the terrain 29% east-west: every
    // slope, every drainage and every peak comes out leaning. Widening the
    // longitude step by the same ratio makes both axes 10.29 m, which is the
    // source's own north-south posting, so nothing is invented.
    const double stepLat = 1.0 / (3600.0 * 3.0);          // 1/3 arc-second
    const double stepLon = stepLat * (mPerDegLat / mPerDegLon);
    const int h = (int)(2 * halfLat / stepLat);
    const int w = (int)(2 * halfLon / stepLon);

    Mosaic m; m.dir = dir; m.prefix = prefix;

    const double originLon = clon - halfLon, originLat = clat + halfLat;
    printf("window  %.1f km square at %.5f, %.5f\n", km, clon, clat);
    printf("grid    %d x %d samples, %.3f m x %.3f m spacing\n",
           w, h, stepLon * mPerDegLon, stepLat * mPerDegLat);

    std::vector<float> grid((size_t)w * h);
    float lo = 1e30f, hi = -1e30f;
    size_t holes = 0;
    for (int j = 0; j < h; ++j) {
        const double lat = originLat - (j + 0.5) * stepLat;
        for (int i = 0; i < w; ++i) {
            const double lon = originLon + (i + 0.5) * stepLon;
            float v = m.sample(lon, lat);
            if (!std::isfinite(v)) { ++holes; v = 0.0f; }
            else { lo = std::min(lo, v); hi = std::max(hi, v); }
            grid[(size_t)j * w + i] = v;
        }
        if ((j & 255) == 0) { printf("\r  row %d/%d", j, h); fflush(stdout); }
    }
    printf("\r  %d rows done            \n", h);

    Header hd{};
    memcpy(hd.magic, "VBDEM01", 8);
    hd.w = w; hd.h = h;
    hd.originLon = originLon; hd.originLat = originLat;
    hd.stepLon = stepLon; hd.stepLat = stepLat;
    hd.metresPerSampleX = stepLon * mPerDegLon;
    hd.metresPerSampleY = stepLat * mPerDegLat;
    hd.minM = lo; hd.maxM = hi;

    FILE *fo = fopen(out.c_str(), "wb");
    if (!fo) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    fwrite(&hd, sizeof hd, 1, fo);
    fwrite(grid.data(), 4, grid.size(), fo);
    fclose(fo);

    printf("elev    %.1f .. %.1f m  (relief %.1f m)\n", lo, hi, hi - lo);
    printf("holes   %zu samples with no data\n", holes);
    printf("wrote   %s  (%.1f MB)\n", out.c_str(),
           (sizeof(Header) + grid.size() * 4) / 1048576.0);
    return 0;
}
