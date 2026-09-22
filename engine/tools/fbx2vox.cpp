// ---------------------------------------------------------------------------
// fbx2vox.cpp -- a binary FBX mesh, voxelised into v2's 10 cm .vox format.
//
//   g++ -std=c++20 -O2 -I ../src fbx2vox.cpp -lz -o ../build/tools/fbx2vox.exe
//   fbx2vox <in.fbx> <out.vox> [targetHeightM=17] [voxelM=0.1]
//
// WRITTEN FOR THE OAKS (user 2026-09-19: "voxelize those files into our 10cm
// voxel format ... make sure when you voxelize oak trees, they are around 17
// meters tall"), and deliberately no more general than that job needs.
//
// ---------------------------------------------------------------------------
// WHAT IT TAKES OUT OF THE FBX, AND WHAT IT THROWS AWAY.
//
// Vertices, PolygonVertexIndex, and the NAME of the Model each Geometry hangs
// off. Nothing else -- no UVs, no materials, no textures.
//
// THAT IS NOT LAZINESS, IT IS THE RESOLUTION. These trees carry their leaves as
// alpha-cut cards and the cards are about 5 cm across; a voxel is 10 cm. An
// opacity map can only decide something SMALLER than the thing it is cutting
// out, so at this scale it has nothing left to say -- a card is already sub-
// voxel and either fills its voxel or does not. What the map would buy is a
// JPEG decoder and a UV pipeline for an answer the grid cannot represent.
//
// The one thing texture assignment IS needed for -- bark against leaf -- the
// file gives away for free: each mesh is its own Model, named for the texture
// it wears (OT0303_Oak_bark_diff_02, OT0303_Oak_leaf_diff_01). So the split is
// a substring test on a name, and it is exact rather than sampled.
//
// ---------------------------------------------------------------------------
// AND THE PALETTE IS THE OAKS' OWN, DELIBERATELY UNCHANGED.
//
// Seven entries: three bark browns and four leaf greens, lifted byte for byte
// out of the oak_*.vox files this replaces. v2's palette is FULL at 255 and
// world.h folds the oaks' wood onto the shared trunk colours and their leaves
// onto the birches' greens -- "one wood colour for every trunk in the world",
// which was asked for and then argued for. New colours here would be new
// entries, or worse, near-misses that loadModelSet's matchTol=30 folds
// somewhere unintended. See [[v2-palette-is-full]].
//
// WHICH of the seven a voxel gets is AMBIENT OCCLUSION, ranked. The source
// model's shading lives in textures this does not read, so the shade has to
// come from the geometry: count the occupied neighbours in a small sphere,
// sort every voxel of a class by that, and cut the sorted list at the same
// proportions the old oak_7.vox used. Deep canopy comes out dark, the outside
// of the crown light, and the histogram matches the model being replaced
// instead of being invented.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <zlib.h>

// ---------------------------------------------------------------------------
// The binary FBX record tree.
//
// A record is [EndOffset, NumProperties, PropertyListLen, NameLen, Name,
// properties..., nested records..., null record]. The three leading fields are
// 32-bit up to FBX 7400 and 64-bit from 7500, which is the one version check
// that matters -- read a 7500 file with 32-bit offsets and every record lands
// in the middle of the previous one's data.
// ---------------------------------------------------------------------------
namespace fbx {

struct Prop {
    char type = 0;                 // Y C I F D L S R  or  f d l i b for arrays
    long long i = 0;               // scalar integer
    double d = 0;                  // scalar real
    std::string s;                 // S / R payload
    std::vector<double> ad;        // decoded f/d array
    std::vector<long long> ai;     // decoded i/l/b array
};

struct Node {
    std::string name;
    std::vector<Prop> props;
    std::vector<Node> kids;
    const Node *child(const char *n) const {
        for (const Node &k : kids)
            if (k.name == n) return &k;
        return nullptr;
    }
};

struct Reader {
    std::vector<uint8_t> d;
    bool wide = false;
    size_t p = 0;

    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        d.resize(size_t(n));
        const size_t got = fread(d.data(), 1, d.size(), f);
        fclose(f);
        if (got != d.size()) return false;
        if (d.size() < 27 || memcmp(d.data(), "Kaydara FBX Binary  ", 20) != 0) return false;
        uint32_t ver = 0;
        memcpy(&ver, &d[23], 4);
        wide = ver >= 7500;
        p = 27;
        return true;
    }

    template <class T> T take() {
        T v{};
        memcpy(&v, &d[p], sizeof(T));
        p += sizeof(T);
        return v;
    }
    uint64_t takeOff() { return wide ? take<uint64_t>() : uint64_t(take<uint32_t>()); }

    // The array properties are the only thing here worth being careful about:
    // [length, encoding, compressedLength], and encoding 1 is a raw zlib
    // stream -- not gzip, no header of its own beyond zlib's two bytes.
    void takeArray(Prop *pr) {
        const uint32_t n = take<uint32_t>();
        const uint32_t enc = take<uint32_t>();
        const uint32_t cl = take<uint32_t>();
        const size_t esz = (pr->type == 'd' || pr->type == 'l') ? 8 : (pr->type == 'b' ? 1 : 4);
        std::vector<uint8_t> raw;
        const uint8_t *src = &d[p];
        if (enc == 1) {
            raw.resize(size_t(n) * esz);
            uLongf out = uLongf(raw.size());
            if (uncompress(raw.data(), &out, src, uLong(cl)) != Z_OK) raw.assign(raw.size(), 0);
            src = raw.data();
        }
        if (pr->type == 'f' || pr->type == 'd') {
            pr->ad.resize(n);
            for (uint32_t k = 0; k < n; ++k) {
                if (pr->type == 'f') {
                    float v;
                    memcpy(&v, src + size_t(k) * 4, 4);
                    pr->ad[k] = v;
                } else {
                    double v;
                    memcpy(&v, src + size_t(k) * 8, 8);
                    pr->ad[k] = v;
                }
            }
        } else {
            pr->ai.resize(n);
            for (uint32_t k = 0; k < n; ++k) {
                if (pr->type == 'l') {
                    int64_t v;
                    memcpy(&v, src + size_t(k) * 8, 8);
                    pr->ai[k] = v;
                } else if (pr->type == 'b') {
                    pr->ai[k] = int8_t(src[k]);
                } else {
                    int32_t v;
                    memcpy(&v, src + size_t(k) * 4, 4);
                    pr->ai[k] = v;
                }
            }
        }
        p += cl;
    }

    // Returns false at the null record that ends a sibling list.
    bool node(Node *out) {
        const uint64_t end = takeOff();
        const uint64_t nprop = takeOff();
        const uint64_t plen = takeOff();
        const uint8_t nl = take<uint8_t>();
        if (end == 0) return false;
        out->name.assign((const char *)&d[p], nl);
        p += nl;
        const size_t pend = p + size_t(plen);
        for (uint64_t k = 0; k < nprop; ++k) {
            Prop pr;
            pr.type = char(take<uint8_t>());
            switch (pr.type) {
                case 'C': pr.i = take<uint8_t>(); break;
                case 'B': pr.i = take<uint8_t>(); break;
                case 'Y': pr.i = take<int16_t>(); break;
                case 'I': pr.i = take<int32_t>(); break;
                case 'L': pr.i = take<int64_t>(); break;
                case 'F': pr.d = take<float>(); break;
                case 'D': pr.d = take<double>(); break;
                case 'S':
                case 'R': {
                    const uint32_t ln = take<uint32_t>();
                    pr.s.assign((const char *)&d[p], ln);
                    p += ln;
                    break;
                }
                default: takeArray(&pr); break;
            }
            out->props.push_back(std::move(pr));
        }
        p = pend;
        while (p < end) {
            Node k;
            if (!node(&k)) break;
            out->kids.push_back(std::move(k));
        }
        p = size_t(end);
        return true;
    }

    std::vector<Node> roots() {
        std::vector<Node> out;
        while (p + 32 < d.size()) {
            Node n;
            if (!node(&n)) break;
            out.push_back(std::move(n));
        }
        return out;
    }
};

// An FBX object name is "Name\x00\x01Class"; only the first half is wanted.
inline std::string shortName(const std::string &s) {
    const size_t z = s.find('\0');
    return z == std::string::npos ? s : s.substr(0, z);
}

}  // namespace fbx

// ---------------------------------------------------------------------------
struct Tri {
    double x[3], y[3], z[3];
    bool leaf;
};

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: fbx2vox <in.fbx> <out.vox> [targetHeightM=17] [voxelM=0.1]\n");
        return 2;
    }
    const char *inPath = argv[1], *outPath = argv[2];
    const double targetH = argc > 3 ? atof(argv[3]) : 17.0;
    const double voxM = argc > 4 ? atof(argv[4]) : 0.1;

    fbx::Reader r;
    if (!r.load(inPath)) {
        fprintf(stderr, "fbx2vox: cannot read %s as a binary FBX\n", inPath);
        return 1;
    }
    std::vector<fbx::Node> roots = r.roots();
    const fbx::Node *objects = nullptr, *connections = nullptr;
    for (const fbx::Node &n : roots) {
        if (n.name == "Objects") objects = &n;
        else if (n.name == "Connections") connections = &n;
    }
    if (!objects) {
        fprintf(stderr, "fbx2vox: no Objects node\n");
        return 1;
    }

    // -- the Model a Geometry hangs off is where its name (and so its material
    // family) lives; Connections is the only thing that joins the two.
    std::map<long long, std::string> modelName;
    std::vector<std::pair<long long, const fbx::Node *>> geos;
    for (const fbx::Node &k : objects->kids) {
        if (k.props.size() < 2) continue;
        const long long id = k.props[0].i;
        if (k.name == "Model") modelName[id] = fbx::shortName(k.props[1].s);
        else if (k.name == "Geometry") geos.push_back({id, &k});
    }
    std::map<long long, long long> parentOf;
    if (connections)
        for (const fbx::Node &c : connections->kids)
            if (c.props.size() >= 3 && c.props[0].s == "OO") parentOf[c.props[1].i] = c.props[2].i;

    // -- every triangle in the file, tagged leaf or bark ---------------------
    std::vector<Tri> tris;
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (auto &g : geos) {
        const fbx::Node *V = g.second->child("Vertices");
        const fbx::Node *P = g.second->child("PolygonVertexIndex");
        if (!V || !P || V->props.empty() || P->props.empty()) continue;
        const std::vector<double> &v = V->props[0].ad;
        const std::vector<long long> &pi = P->props[0].ai;

        std::string nm;
        auto it = parentOf.find(g.first);
        if (it != parentOf.end()) {
            auto mi = modelName.find(it->second);
            if (mi != modelName.end()) nm = mi->second;
        }
        std::string low = nm;
        for (char &c : low) c = char(tolower((unsigned char)c));
        const bool leaf = low.find("leaf") != std::string::npos;
        printf("  mesh %-34s %9zu verts  %s\n", nm.empty() ? "(unnamed)" : nm.c_str(),
               v.size() / 3, leaf ? "LEAF" : "bark");

        // Polygons are fan-triangulated. A negative index is the last corner of
        // its polygon, stored as ~i -- that XOR is the only terminator there is.
        std::vector<long long> poly;
        for (long long idx : pi) {
            const bool last = idx < 0;
            poly.push_back(last ? ~idx : idx);
            if (!last) continue;
            for (size_t t = 2; t < poly.size(); ++t) {
                Tri tr;
                tr.leaf = leaf;
                const long long c[3] = {poly[0], poly[t - 1], poly[t]};
                bool ok = true;
                for (int k = 0; k < 3; ++k) {
                    const size_t b = size_t(c[k]) * 3;
                    if (b + 2 >= v.size()) { ok = false; break; }
                    tr.x[k] = v[b]; tr.y[k] = v[b + 1]; tr.z[k] = v[b + 2];
                }
                if (!ok) continue;
                for (int k = 0; k < 3; ++k) {
                    lo[0] = std::min(lo[0], tr.x[k]); hi[0] = std::max(hi[0], tr.x[k]);
                    lo[1] = std::min(lo[1], tr.y[k]); hi[1] = std::max(hi[1], tr.y[k]);
                    lo[2] = std::min(lo[2], tr.z[k]); hi[2] = std::max(hi[2], tr.z[k]);
                }
                tris.push_back(tr);
            }
            poly.clear();
        }
    }
    if (tris.empty()) {
        fprintf(stderr, "fbx2vox: no triangles\n");
        return 1;
    }

    // -- THE SCALE IS SET BY THE HEIGHT, and Z IS UP in both formats ---------
    // These files measure in centimetres and stand on z = 0; MagicaVoxel is
    // z-up too, so the axes pass straight through and only the scale changes.
    // v2's vox.h does the one swap to y-up when it loads.
    const double spanZ = hi[2] - lo[2];
    const double scale = (targetH / voxM) / spanZ;   // source units -> voxels
    const int sx = int(std::ceil((hi[0] - lo[0]) * scale)) + 1;
    const int sy = int(std::ceil((hi[1] - lo[1]) * scale)) + 1;
    const int sz = int(std::ceil(spanZ * scale)) + 1;
    printf("  source %.1f x %.1f x %.1f units, %zu triangles\n", hi[0] - lo[0], hi[1] - lo[1],
           spanZ, tris.size());
    printf("  grid   %d x %d x %d voxels  (%.2f x %.2f x %.2f m at %.0f cm)\n", sx, sy, sz,
           sx * voxM, sy * voxM, sz * voxM, voxM * 100.0);
    if (sx > 256 || sy > 256 || sz > 256) {
        fprintf(stderr, "fbx2vox: %d x %d x %d exceeds the 256 a single XYZI can address\n", sx,
                sy, sz);
        return 1;
    }

    // -- rasterise -----------------------------------------------------------
    // 0 empty, 1 leaf, 2 bark. BARK IS WRITTEN SECOND AND WINS: a branch inside
    // the crown should read as a branch, and the leaf cards pass straight
    // through the twigs they hang on.
    const size_t cells = size_t(sx) * sy * sz;
    std::vector<uint8_t> cls(cells, 0);
    auto put = [&](double fx, double fy, double fz, uint8_t c) {
        const int ix = int((fx - lo[0]) * scale);
        const int iy = int((fy - lo[1]) * scale);
        const int iz = int((fz - lo[2]) * scale);
        if (ix < 0 || iy < 0 || iz < 0 || ix >= sx || iy >= sy || iz >= sz) return;
        uint8_t &d = cls[size_t(ix) + size_t(iy) * sx + size_t(iz) * sx * sy];
        if (c >= d) d = c;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantLeaf = (pass == 0);
        for (const Tri &t : tris) {
            if (t.leaf != wantLeaf) continue;
            // A barycentric lattice fine enough that no two samples are more
            // than a third of a voxel apart, which is what keeps a shell
            // watertight where a triangle crosses a grid plane at a graze.
            double e1 = 0, e2 = 0;
            const double d1[3] = {t.x[1] - t.x[0], t.y[1] - t.y[0], t.z[1] - t.z[0]};
            const double d2[3] = {t.x[2] - t.x[0], t.y[2] - t.y[0], t.z[2] - t.z[0]};
            for (int k = 0; k < 3; ++k) { e1 += d1[k] * d1[k]; e2 += d2[k] * d2[k]; }
            const double n1 = std::sqrt(e1) * scale, n2 = std::sqrt(e2) * scale;
            const int steps = std::max(1, int(std::ceil(std::max(n1, n2) * 3.0)));
            const uint8_t c = t.leaf ? 1 : 2;
            for (int a = 0; a <= steps; ++a)
                for (int b = 0; a + b <= steps; ++b) {
                    const double u = double(a) / steps, w = double(b) / steps;
                    put(t.x[0] + d1[0] * u + d2[0] * w, t.y[0] + d1[1] * u + d2[1] * w,
                        t.z[0] + d1[2] * u + d2[2] * w, c);
                }
        }
    }

    // -- the shade, from ambient occlusion, ranked ---------------------------
    // See the header. The AO is the occupied fraction of a radius-2 sphere; the
    // ranking makes the output histogram match oak_7.vox's whatever the AO
    // range of this particular tree turns out to be.
    std::vector<float> ao(cells, 0.0f);
    std::vector<uint32_t> leafIdx, barkIdx;
    const int R = 2;
    for (int z = 0; z < sz; ++z)
        for (int y = 0; y < sy; ++y)
            for (int x = 0; x < sx; ++x) {
                const size_t i = size_t(x) + size_t(y) * sx + size_t(z) * sx * sy;
                if (!cls[i]) continue;
                int n = 0, occ = 0;
                for (int dz = -R; dz <= R; ++dz)
                    for (int dy = -R; dy <= R; ++dy)
                        for (int dx = -R; dx <= R; ++dx) {
                            if (dx * dx + dy * dy + dz * dz > R * R) continue;
                            ++n;
                            const int px = x + dx, py = y + dy, pz = z + dz;
                            if (px < 0 || py < 0 || pz < 0 || px >= sx || py >= sy || pz >= sz)
                                continue;
                            if (cls[size_t(px) + size_t(py) * sx + size_t(pz) * sx * sy]) ++occ;
                        }
                // A little hash jitter so the quantile cuts below do not draw
                // contours of their own through an evenly lit part of the crown.
                const uint32_t h = uint32_t(i) * 2654435761u;
                ao[i] = float(occ) / float(n) + (float(h >> 8 & 0xFFFF) / 65535.0f - 0.5f) * 0.05f;
                (cls[i] == 1 ? leafIdx : barkIdx).push_back(uint32_t(i));
            }

    // oak_7.vox's own split, most occluded first.
    static const double kLeafMix[4] = {0.381, 0.273, 0.224, 0.122};   // palette 4,5,6,7
    static const double kBarkMix[3] = {0.514, 0.315, 0.171};          // palette 1,2,3
    std::vector<uint8_t> pal(cells, 0);
    auto rank = [&](std::vector<uint32_t> &v, const double *mix, int n, int base) {
        std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return ao[a] > ao[b]; });
        size_t at = 0;
        for (int k = 0; k < n; ++k) {
            const size_t want = (k == n - 1) ? v.size() - at : size_t(mix[k] * double(v.size()));
            for (size_t j = 0; j < want && at < v.size(); ++j, ++at) pal[v[at]] = uint8_t(base + k);
        }
    };
    rank(leafIdx, kLeafMix, 4, 4);
    rank(barkIdx, kBarkMix, 3, 1);
    printf("  voxels %zu leaf + %zu bark = %zu\n", leafIdx.size(), barkIdx.size(),
           leafIdx.size() + barkIdx.size());

    // -- write the .vox ------------------------------------------------------
    // THE RGBA CHUNK IS NOT OPTIONAL. A file without one is read against
    // MagicaVoxel's built-in 255-colour table, and v2 registers what it finds:
    // see [[v2-palette-is-full]]. Seven entries, the oaks' own.
    static const uint8_t kPal[7][3] = {
        {0x54, 0x4c, 0x33}, {0x65, 0x59, 0x43}, {0x71, 0x68, 0x58},          // bark
        {0x50, 0x70, 0x2f}, {0x69, 0x8f, 0x32}, {0x73, 0x96, 0x48}, {0x82, 0xa1, 0x65}};  // leaf

    std::vector<uint8_t> xyzi;
    uint32_t count = 0;
    for (int z = 0; z < sz; ++z)
        for (int y = 0; y < sy; ++y)
            for (int x = 0; x < sx; ++x) {
                const size_t i = size_t(x) + size_t(y) * sx + size_t(z) * sx * sy;
                if (!pal[i]) continue;
                xyzi.push_back(uint8_t(x));
                xyzi.push_back(uint8_t(y));
                xyzi.push_back(uint8_t(z));
                xyzi.push_back(pal[i]);
                ++count;
            }

    FILE *f = fopen(outPath, "wb");
    if (!f) {
        fprintf(stderr, "fbx2vox: cannot write %s\n", outPath);
        return 1;
    }
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto chunk = [&](const char *id, uint32_t content, uint32_t children) {
        fwrite(id, 1, 4, f);
        u32(content);
        u32(children);
    };
    const uint32_t sizeChunk = 12 + 12, xyziChunk = 12 + 4 + count * 4, rgbaChunk = 12 + 1024;
    fwrite("VOX ", 1, 4, f);
    u32(150);
    chunk("MAIN", 0, sizeChunk + xyziChunk + rgbaChunk);
    chunk("SIZE", 12, 0);
    u32(uint32_t(sx));
    u32(uint32_t(sy));
    u32(uint32_t(sz));
    chunk("XYZI", 4 + count * 4, 0);
    u32(count);
    fwrite(xyzi.data(), 1, xyzi.size(), f);
    chunk("RGBA", 1024, 0);
    for (int i = 0; i < 256; ++i) {
        // Entry i of the chunk is palette index i+1, so the seven land on 1..7.
        uint8_t e[4] = {0, 0, 0, 0};
        if (i < 7) { e[0] = kPal[i][0]; e[1] = kPal[i][1]; e[2] = kPal[i][2]; e[3] = 255; }
        fwrite(e, 1, 4, f);
    }
    fclose(f);
    printf("  wrote  %s  %d x %d x %d, %u voxels\n", outPath, sx, sy, sz, count);
    return 0;
}
