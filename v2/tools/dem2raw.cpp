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
#include <filesystem>
#include <system_error>
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

    // GeoTIFF stores its CRS in a key directory inside tag 34735: a 4-short
    // header followed by 4-short entries {keyId, tiffTagLocation, count,
    // valueOrOffset}. Only the short-valued keys are needed here (location 0),
    // which is where ProjectedCSTypeGeoKey (3072) and GTModelTypeGeoKey (1024)
    // both live.
    uint64_t geoKey(uint16_t key, uint64_t dflt = 0) const {
        auto it = tags.find(34735);
        if (it == tags.end() || it->second.count < 8) return dflt;
        const uint64_t n = num(34735, 3);
        for (uint64_t i = 0; i < n; ++i) {
            const uint64_t base = 4 + i * 4;
            if (base + 3 >= it->second.count) break;
            if (num(34735, base) != key) continue;
            if (num(34735, base + 1) != 0) return dflt;   // not a short value
            return num(34735, base + 3);
        }
        return dflt;
    }

    double nodata() const {
        auto it = tags.find(42113);
        if (it == tags.end()) return -999999.0;
        std::string s((const char *)it->second.raw.data(), it->second.raw.size());
        return atof(s.c_str());
    }
};

// ------------------------------------------------------------------ utm

// THE 1 m TILES ARE PROJECTED AND THE 1/3" TILES ARE NOT, and that is the only
// thing standing between this tool and ten times the resolution. 3DEP's 1 m
// product is posted on a UTM grid in metres; the 1/3 arc-second product is
// posted on a geographic grid in degrees. Same tags, same reader, different
// space -- so a tile carries its CRS and every lookup converts the caller's
// lon/lat into the tile's own coordinates before touching a pixel.
//
// Forward only (lon/lat -> easting/northing): the sweep in main() walks the
// output grid in degrees and asks each tile for a sample, so the inverse is
// never needed. Snyder's series, accurate to under a millimetre within a zone.
//
// DATUM: the 1 m tiles are NAD83(2011), the window centres are read off a
// WGS84 map, and the two differ by 1-2 m horizontally in CONUS. That is a
// CONSTANT TRANSLATION of the whole window, not a distortion, and it is well
// inside the precision a centre is quoted to (4 decimal places is 11 m). So
// the ellipsoid below is used for both and no datum shift is applied.
namespace utm {

constexpr double kA = 6378137.0;
constexpr double kF = 1.0 / 298.257223563;
constexpr double kE2 = kF * (2.0 - kF);
constexpr double kK0 = 0.9996;

struct Zone { int zone = 0; bool south = false; };

// EPSG -> UTM zone. NAD83 is 269xx (1N..23N), NAD83(2011) is 6330+ (1N..19N),
// WGS84 is 326xx north / 327xx south. Anything else is refused rather than
// guessed at, because a wrong zone puts the window hundreds of km away and
// still returns perfectly plausible elevations.
inline bool zoneFor(int epsg, Zone *z) {
    if (epsg >= 26901 && epsg <= 26923) { z->zone = epsg - 26900; z->south = false; return true; }
    if (epsg >= 32601 && epsg <= 32660) { z->zone = epsg - 32600; z->south = false; return true; }
    if (epsg >= 32701 && epsg <= 32760) { z->zone = epsg - 32700; z->south = true;  return true; }
    if (epsg >= 6330  && epsg <= 6348)  { z->zone = epsg - 6329;  z->south = false; return true; }
    return false;
}

inline void forward(const Zone &z, double lonDeg, double latDeg, double *east, double *north) {
    const double lon0 = (z.zone * 6 - 183) * M_PI / 180.0;
    const double phi = latDeg * M_PI / 180.0;
    const double lam = lonDeg * M_PI / 180.0;
    const double ep2 = kE2 / (1.0 - kE2);
    const double sp = sin(phi), cp = cos(phi), tp = tan(phi);
    const double N = kA / sqrt(1.0 - kE2 * sp * sp);
    const double T = tp * tp;
    const double C = ep2 * cp * cp;
    const double A = (lam - lon0) * cp;
    const double M = kA * ((1.0 - kE2 / 4 - 3 * kE2 * kE2 / 64 - 5 * kE2 * kE2 * kE2 / 256) * phi
                         - (3 * kE2 / 8 + 3 * kE2 * kE2 / 32 + 45 * kE2 * kE2 * kE2 / 1024) * sin(2 * phi)
                         + (15 * kE2 * kE2 / 256 + 45 * kE2 * kE2 * kE2 / 1024) * sin(4 * phi)
                         - (35 * kE2 * kE2 * kE2 / 3072) * sin(6 * phi));
    const double A2 = A * A, A3 = A2 * A, A4 = A3 * A, A5 = A4 * A, A6 = A5 * A;
    *east = kK0 * N * (A + (1 - T + C) * A3 / 6
                         + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * A5 / 120) + 500000.0;
    *north = kK0 * (M + N * tp * (A2 / 2 + (5 - T + 9 * C + 4 * C * C) * A4 / 24
                   + (61 - 58 * T + T * T + 600 * C - 330 * ep2) * A6 / 720));
    if (z.south) *north += 10000000.0;
}

// The index needs the other direction ONCE per tile: a projected tile has to
// report where it is in lon/lat before anything can decide whether to ask it.
inline void inverse(const Zone &z, double east, double north, double *lonDeg, double *latDeg) {
    const double lon0 = (z.zone * 6 - 183) * M_PI / 180.0;
    if (z.south) north -= 10000000.0;
    const double ep2 = kE2 / (1.0 - kE2);
    const double M = north / kK0;
    const double mu = M / (kA * (1.0 - kE2 / 4 - 3 * kE2 * kE2 / 64 - 5 * kE2 * kE2 * kE2 / 256));
    const double e1 = (1.0 - sqrt(1.0 - kE2)) / (1.0 + sqrt(1.0 - kE2));
    const double e1_2 = e1 * e1, e1_3 = e1_2 * e1, e1_4 = e1_3 * e1;
    const double p1 = mu + (3 * e1 / 2 - 27 * e1_3 / 32) * sin(2 * mu)
                         + (21 * e1_2 / 16 - 55 * e1_4 / 32) * sin(4 * mu)
                         + (151 * e1_3 / 96) * sin(6 * mu)
                         + (1097 * e1_4 / 512) * sin(8 * mu);
    const double sp = sin(p1), cp = cos(p1), tp = tan(p1);
    const double C1 = ep2 * cp * cp, T1 = tp * tp;
    const double N1 = kA / sqrt(1.0 - kE2 * sp * sp);
    const double R1 = kA * (1.0 - kE2) / pow(1.0 - kE2 * sp * sp, 1.5);
    const double D = (east - 500000.0) / (N1 * kK0);
    const double D2 = D * D, D3 = D2 * D, D4 = D3 * D, D5 = D4 * D, D6 = D5 * D;
    const double phi = p1 - (N1 * tp / R1) * (D2 / 2
                        - (5 + 3 * T1 + 10 * C1 - 4 * C1 * C1 - 9 * ep2) * D4 / 24
                        + (61 + 90 * T1 + 298 * C1 + 45 * T1 * T1 - 252 * ep2 - 3 * C1 * C1) * D6 / 720);
    const double lam = lon0 + (D - (1 + 2 * T1 + C1) * D3 / 6
                        + (5 - 2 * C1 + 28 * T1 - 3 * C1 * C1 + 8 * ep2 + 24 * T1 * T1) * D5 / 120) / cp;
    *latDeg = phi * 180.0 / M_PI;
    *lonDeg = lam * 180.0 / M_PI;
}

} // namespace utm

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
    // The tile's own space. `projected` false means ox/oy/sx/sy are degrees
    // (the 1/3" product); true means metres in `zone` (the 1 m product).
    bool projected = false;
    int epsg = 0;
    utm::Zone zone;
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
        // GTModelTypeGeoKey: 1 = projected, 2 = geographic. Trust the
        // projected-CRS key over it, since that is the one that names the zone.
        epsg = (int)t.geoKey(3072, 0);
        if (epsg && utm::zoneFor(epsg, &zone)) projected = true;
        else if (t.geoKey(1024, 2) == 1 && !epsg) {
            fprintf(stderr, "  projected tile with no recognised CRS key\n");
            return false;
        }
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

    // Caller speaks lon/lat; the tile may not. One conversion, in one place,
    // so nothing downstream has to know which product it is reading.
    void native(double lon, double lat, double *x, double *y) const {
        if (!projected) { *x = lon; *y = lat; return; }
        utm::forward(zone, lon, lat, x, y);
    }
    // A LON/LAT BOX ROUND A UTM TILE IS NOT ITS CORNERS. Grid north is not true
    // north away from the central meridian, so a UTM square is a curved
    // quadrilateral in lon/lat and its extreme longitude is on an edge, not a
    // corner. Walking the whole border rather than the four corners is what
    // makes the box a superset; it is only an index, and covers() is exact.
    void bbox(double *lo0, double *la0, double *lo1, double *la1) const {
        const double x0 = ox, x1 = ox + sx * w, y0 = oy - sy * h, y1 = oy;
        *lo0 = *la0 = 1e30; *lo1 = *la1 = -1e30;
        for (int k = 0; k <= 64; ++k) {
            const double t = k / 64.0;
            const double pts[4][2] = {{x0 + (x1 - x0) * t, y0}, {x0 + (x1 - x0) * t, y1},
                                      {x0, y0 + (y1 - y0) * t}, {x1, y0 + (y1 - y0) * t}};
            for (const auto &pt : pts) {
                double lo = pt[0], la = pt[1];
                if (projected) utm::inverse(zone, pt[0], pt[1], &lo, &la);
                *lo0 = std::min(*lo0, lo); *lo1 = std::max(*lo1, lo);
                *la0 = std::min(*la0, la); *la1 = std::max(*la1, la);
            }
        }
    }
    size_t cachedBytes() const { return cache_.size() * (size_t)tw * th * 4; }
    void dropCache() const { cache_.clear(); lru_.clear(); }

    bool covers(double lon, double lat) const {
        double x, y; native(lon, lat, &x, &y);
        return x >= ox && x < ox + sx * w && y <= oy && y > oy - sy * h;
    }
    // Bilinear in pixel space; nodata poisons the sample so the caller can fill.
    float sample(double lon, double lat) const {
        double nx, ny; native(lon, lat, &nx, &ny);
        const double fx = (nx - ox) / sx - 0.5, fy = (oy - ny) / sy - 0.5;
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
    // The 1/3" product is a clean one-degree grid, so a name is enough and
    // nothing has to be opened to find a tile. The 1 m product is NOT: its
    // tiles are named for the acquisition project that flew them --
    // USGS_1M_13_x68y440_CO_SoPlatteRiver_Lot5_2013.tif -- with the grid
    // reference in a project-local x/y that means nothing outside it. So that
    // product is indexed by what each file SAYS it covers, which also makes
    // the tool indifferent to how a supplier names anything.
    bool byName = true;

    struct Entry {
        std::unique_ptr<DemTile> tile;
        double lo0 = 0, la0 = 0, lo1 = 0, la1 = 0;
        std::string path;
        size_t hits = 0;        // samples this tile actually answered
    };
    std::vector<Entry> idx_;
    std::map<std::string, std::unique_ptr<DemTile>> open_;
    int last_ = -1;

    // 48 blocks of 512x512 float is 48 MB a tile, which was free when one tile
    // was open at a time and is not when a 12 km window at 1 m spans nine.
    static constexpr size_t kCacheCapBytes = 512u << 20;

    static std::string nameFor(double lon, double lat) {
        // tiles are named by their NORTHWEST corner
        const int la = (int)ceil(lat), lo = (int)ceil(-lon);
        char b[32]; snprintf(b, sizeof b, "n%02dw%03d", la, lo);
        return b;
    }

    // Returns false only if the directory cannot be read at all; an empty
    // index is reported by the caller, which knows the window.
    bool buildIndex() {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<std::string> files;
        for (fs::directory_iterator it(dir, ec), e; it != e; it.increment(ec)) {
            if (ec) break;
            const std::string pth = it->path().string();
            const std::string ext = it->path().extension().string();
            if (ext == ".tif" || ext == ".TIF" || ext == ".tiff") files.push_back(pth);
        }
        if (files.empty()) return false;
        std::sort(files.begin(), files.end());
        printf("index   %zu tiles in %s\n", files.size(), dir.c_str());
        for (const std::string &pth : files) {
            Entry e;
            e.tile = std::make_unique<DemTile>();
            if (!e.tile->open(pth)) continue;
            e.tile->bbox(&e.lo0, &e.la0, &e.lo1, &e.la1);
            e.path = pth;
            // Opening is cheap -- header only -- but the block cache is not,
            // so nothing is decoded until a sample actually lands in it.
            idx_.push_back(std::move(e));
        }
        if (!idx_.empty()) {
            const DemTile *t = idx_[0].tile.get();
            printf("        crs %s, %d x %d, %.3f m/px\n",
                   t->projected ? ("EPSG:" + std::to_string(t->epsg)).c_str() : "geographic",
                   t->w, t->h, t->projected ? t->sx : t->sx * 111320.0);
        }
        byName = false;
        return true;
    }

    void trim() {
        size_t total = 0;
        for (const Entry &e : idx_) total += e.tile->cachedBytes();
        if (total <= kCacheCapBytes) return;
        for (int i = 0; i < (int)idx_.size() && total > kCacheCapBytes; ++i) {
            if (i == last_) continue;
            const size_t was = idx_[i].tile->cachedBytes();
            if (!was) continue;
            idx_[i].tile->dropCache();
            total -= was;
        }
    }

    DemTile *tileFor(double lon, double lat) {
        if (byName) {
            const std::string key = nameFor(lon, lat);
            auto it = open_.find(key);
            if (it != open_.end()) return it->second.get();
            auto t = std::make_unique<DemTile>();
            const std::string path = dir + "/" + prefix + "_" + key + ".tif";
            if (!t->open(path)) { open_[key] = nullptr; return nullptr; }
            DemTile *p = t.get(); open_[key] = std::move(t); return p;
        }
        // A row-major sweep stays inside one tile for thousands of samples, so
        // the last hit is checked before anything else and the scan below runs
        // about once a tile crossing rather than once a sample.
        if (last_ >= 0 && last_ < (int)idx_.size() && idx_[last_].tile->covers(lon, lat))
            return idx_[last_].tile.get();
        for (int i = 0; i < (int)idx_.size(); ++i) {
            const Entry &e = idx_[i];
            if (lon < e.lo0 || lon > e.lo1 || lat < e.la0 || lat > e.la1) continue;
            if (!e.tile->covers(lon, lat)) continue;   // the bbox is a superset
            last_ = i;
            trim();
            return e.tile.get();   // hits counted in sample(), not here
        }
        return nullptr;
    }

    size_t missed = 0;   // samples that landed on no tile at all

    // ------------------------------------------------------------------
    // A TILE THAT COVERS A POINT IS NOT A TILE THAT HAS IT.
    //
    // 3DEP's 1 m product is published per ACQUISITION LOT, and a lot's tiles
    // are full-size rectangles with nodata wherever that lot did not fly. Two
    // lots therefore publish tiles at the SAME grid reference, each holding
    // half the ground -- AR_Ouachita_B5_2016 and _B6_2016 both have x47y384,
    // and between them they cover it.
    //
    // Stopping at the first tile whose bounds contain the point leaves a clean
    // band of holes where one lot's footprint ends, which looks exactly like a
    // window that reaches past the data. It is not: the data is in the
    // directory, in the other file. So nodata falls through to the next
    // candidate and only a point that every covering tile calls nodata is a
    // hole.
    // ------------------------------------------------------------------
    float sample(double lon, double lat) {
        if (byName) {
            DemTile *t = tileFor(lon, lat);
            if (!t) { ++missed; return NAN; }
            const float v = t->sample(lon, lat);
            return (v == (float)t->nd) ? NAN : v;
        }
        // The last tile that ANSWERED is tried first: a row-major sweep stays
        // inside one lot for thousands of samples, so the scan below runs
        // about once per crossing and not once per sample.
        if (last_ >= 0 && last_ < (int)idx_.size() && idx_[last_].tile->covers(lon, lat)) {
            const float v = idx_[last_].tile->sample(lon, lat);
            if (v != (float)idx_[last_].tile->nd && std::isfinite(v)) {
                ++idx_[last_].hits;
                return v;
            }
        }
        bool anyCovered = false;
        for (int i = 0; i < (int)idx_.size(); ++i) {
            Entry &e = idx_[i];
            if (lon < e.lo0 || lon > e.lo1 || lat < e.la0 || lat > e.la1) continue;
            if (!e.tile->covers(lon, lat)) continue;     // the bbox is a superset
            anyCovered = true;
            const float v = e.tile->sample(lon, lat);
            if (v == (float)e.tile->nd || !std::isfinite(v)) continue;   // that lot did not fly it
            last_ = i;
            ++e.hits;
            trim();
            return v;
        }
        if (!anyCovered) ++missed;
        return NAN;
    }

    // WHY THIS IS PRINTED AND NOT JUST COUNTED. A window that reaches past the
    // tiles you fetched comes out as a bare hole count -- "52522508 samples
    // with no data" -- which says nothing about WHICH ground is missing or
    // which tile to go and get. It cost a rebuild and a gigabyte of the wrong
    // tiles to find out that the answer was two files.
    //
    // THE TRAP THAT CAUSED IT: a 3DEP 1 m tile is named for its NORTH-WEST
    // corner, exactly as the 1/3" tiles are -- `x47y385` covers northing
    // 3,839,994..3,850,006, NOT 3,850,000..3,860,000. Read the y as the south
    // edge and you fetch a row of tiles one step too far south: the window
    // then has a clean band of holes across the top, which looks precisely
    // like a lidar project boundary and is not one.
    void report(double lo0, double la0, double lo1, double la1) const {
        if (byName) return;
        printf("tiles   window lon %.5f..%.5f lat %.5f..%.5f%s", lo0, lo1, la0, la1, "\n");
        for (const Entry &e : idx_) {
            const char *base = strrchr(e.path.c_str(), '/');
            printf("        %-46s %9zu samples%s", base ? base + 1 : e.path.c_str(),
                   e.hits, "\n");
        }
        if (missed)
            printf("        %zu samples fell on NO TILE -- the window reaches past%s"\n"        what is in the directory; check the NW-corner naming.%s",
                   missed, "\n", "\n");
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

// A WRONG ZONE STILL RETURNS PLAUSIBLE ELEVATIONS, which is the same class of
// silent failure as the floating-point predictor above: the window lands
// somewhere else entirely and every number in it looks like terrain. So the
// projection is checked against things that are true by construction.
int utmTest() {
    int bad = 0;
    // 1. ON THE CENTRAL MERIDIAN THE EASTING IS EXACTLY THE FALSE EASTING.
    //    This is the one value in the whole series that is known in closed
    //    form, and it is the test a transposed sign or a wrong zone fails.
    for (int z = 10; z <= 19; ++z) {
        utm::Zone zz{z, false};
        const double cm = z * 6 - 183;
        for (double lat = 26.0; lat <= 49.0; lat += 4.0) {
            double e, n; utm::forward(zz, cm, lat, &e, &n);
            if (fabs(e - 500000.0) > 1e-6) {
                printf("FAIL zone %d lat %.0f: easting %.6f != 500000%s", z, lat, e, "\n");
                ++bad;
            }
        }
    }
    // 2. ROUND TRIP over the whole of CONUS, at the zone edges as well as the
    //    middle, since the series is weakest 3 degrees off the meridian.
    double worst = 0.0;
    for (int z = 10; z <= 19; ++z) {
        utm::Zone zz{z, false};
        const double cm = z * 6 - 183;
        for (double dl = -3.0; dl <= 3.0; dl += 0.5)
            for (double lat = 25.0; lat <= 50.0; lat += 1.0) {
                double e, n, lo, la;
                utm::forward(zz, cm + dl, lat, &e, &n);
                utm::inverse(zz, e, n, &lo, &la);
                const double mLon = (lo - (cm + dl)) * 111320.0 * cos(lat * M_PI / 180.0);
                const double mLat = (la - lat) * 111132.0;
                worst = std::max(worst, sqrt(mLon * mLon + mLat * mLat));
            }
    }
    printf("round trip   worst %.6f mm over CONUS zones 10-19%s", worst * 1000.0, "\n");
    if (worst > 0.001) { printf("FAIL round trip over 1 mm%s", "\n"); ++bad; }
    // 3. A KNOWN POINT. NGS publishes Mount Elbert's summit mark; zone 13N.
    //    A degree of latitude is ~110.9 km, so a northing that is not within a
    //    few metres of 4,330,000 at 39.1178 N is a broken series, not rounding.
    {
        utm::Zone zz{13, false};
        double e, n; utm::forward(zz, -106.4453, 39.1178, &e, &n);
        printf("elbert       zone 13N  E %.1f  N %.1f%s", e, n, "\n");
        if (e < 370000 || e > 385000 || n < 4325000 || n > 4335000) {
            printf("FAIL elbert outside the expected 15 km box%s", "\n"); ++bad;
        }
    }
    printf("%s%s", bad ? "utm-test FAILED" : "utm-test PASS", "\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    std::string dir = "C:/geo/dem/colorado/13arcsec_10m", prefix = "USGS_13", out = "co.vbdem";
    double clon = -106.4453, clat = 39.1178, km = 40.0;
    // 0 means "the source's own 1/3 arc-second posting", spelled exactly as it
    // always was so every .vbdem built before this flag existed still rebuilds
    // byte for byte.
    double stepM = 0.0;
    bool forceIndex = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--center" && i + 2 < argc) { clon = atof(argv[++i]); clat = atof(argv[++i]); }
        else if (a == "--km" && i + 1 < argc) km = atof(argv[++i]);
        else if (a == "--step-m" && i + 1 < argc) stepM = atof(argv[++i]);
        else if (a == "--index") forceIndex = true;
        else if (a == "--utm-test") return utmTest();
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
    //
    // --step-m SETS THE OUTPUT POSTING, which is the whole point of reading a
    // 1 m source: at --dem-scale 1 a 10.29 m posting is 103 voxel columns of
    // interpolation between two measurements, and a 1 m posting is ten. The
    // output grid is square on the ground either way.
    const double stepLat = stepM > 0.0 ? stepM / mPerDegLat : 1.0 / (3600.0 * 3.0);
    const double stepLon = stepM > 0.0 ? stepM / mPerDegLon
                                       : stepLat * (mPerDegLat / mPerDegLon);
    const int h = (int)(2 * halfLat / stepLat);
    const int w = (int)(2 * halfLon / stepLon);

    Mosaic m; m.dir = dir; m.prefix = prefix;
    // Name mode needs the one-degree tile the window centre falls in to exist.
    // If it does not, this is not the 1/3" product and the directory is
    // indexed by what its files say they cover instead.
    {
        const std::string probe = dir + "/" + prefix + "_" + Mosaic::nameFor(clon, clat) + ".tif";
        FILE *pf = forceIndex ? nullptr : fopen(probe.c_str(), "rb");
        if (pf) fclose(pf);
        else if (!m.buildIndex() || m.idx_.empty()) {
            fprintf(stderr, "no usable tiles in %s\n", dir.c_str());
            return 1;
        }
    }

    const double originLon = clon - halfLon, originLat = clat + halfLat;
    printf("window  %.1f km square at %.5f, %.5f\n", km, clon, clat);
    printf("grid    %d x %d samples, %.3f m x %.3f m spacing\n",
           w, h, stepLon * mPerDegLon, stepLat * mPerDegLat);

    printf("output  %.0f MB\n", (double)((size_t)w * h * 4) / 1048576.0);
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

    m.report(originLon, originLat - stepLat * h, originLon + stepLon * w, originLat);
    printf("elev    %.1f .. %.1f m  (relief %.1f m)\n", lo, hi, hi - lo);
    printf("holes   %zu samples with no data\n", holes);
    printf("wrote   %s  (%.1f MB)\n", out.c_str(),
           (sizeof(Header) + grid.size() * 4) / 1048576.0);
    return 0;
}
