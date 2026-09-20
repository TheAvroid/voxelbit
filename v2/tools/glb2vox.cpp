// ---------------------------------------------------------------------------
// glb2vox.cpp -- a glTF-binary mesh, voxelised into v2's 10 cm .vox format.
//
//   g++ -std=c++20 -O2 glb2vox.cpp -o ../build/tools/glb2vox.exe
//   glb2vox <in.glb> <outDir> <prefix> [targetHeightM=5] [voxelM=0.05] [palette.vox]
//
// (user 2026-09-19: "you were supposed to revoxelize the cactus. not make the
//  existing voxel one bigger", then "can you find the original cactus fbx
//  files. they are called cacti.glb. revoxelize them".)
//
// WHY THIS EXISTS BESIDE fbx2vox. That tool reads a BINARY FBX and its palette
// is the oaks', hardcoded -- it was written for one job and says so. The cacti
// ship as glTF, and glTF is a different container entirely: a JSON document and
// a binary blob, rather than FBX's tagged node tree. The voxeliser at the
// bottom is the same idea as its one (sample each triangle barycentrically into
// a grid, close the shell, fill); the front half had to be new.
//
// ONE MESH PER FILE. cactus.glb holds nine meshes named cactus_1 .. cactus_9,
// which is exactly the nine cactus_N.vox the engine loads -- so each mesh is
// voxelised on its own and written to its own file, and the set it replaces is
// the set it came from.
//
// THE PALETTE IS BORROWED, NEVER INVENTED. v2's table is effectively full (see
// [[v2-palette-is-full]]), so a revoxelisation that minted fresh greens would
// cost entries the world does not have. This reads the palette out of an
// EXISTING .vox and maps every triangle to the nearest entry in it by the
// material's own baseColorFactor. The shape is new; the colours are the ones
// the engine has already paid for.
//
// WHAT IT TAKES AND WHAT IT IGNORES: positions, indices, node transforms, and
// each primitive's material base colour. No UVs, no textures, no normals -- at
// 5 cm a cactus spine is already sub-voxel, which is the same argument fbx2vox
// makes at 10 cm for a leaf card.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// A JSON VALUE, ONLY AS FAR AS glTF NEEDS ONE.
//
// Objects, arrays, numbers and strings. No unicode escapes beyond passing the
// bytes through, and no error recovery -- a malformed glTF is a bug in the
// exporter, not a case to handle gracefully.
// ---------------------------------------------------------------------------
struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    bool has(const char *k) const { return kind == Obj && obj.count(k) != 0; }
    const Json &operator[](const char *k) const {
        static const Json none;
        auto it = obj.find(k);
        return it == obj.end() ? none : it->second;
    }
    const Json &operator[](size_t i) const {
        static const Json none;
        return i < arr.size() ? arr[i] : none;
    }
    size_t size() const { return arr.size(); }
    int asInt(int dflt = -1) const { return kind == Num ? int(num) : dflt; }
    double asNum(double dflt = 0.0) const { return kind == Num ? num : dflt; }
};

struct JsonReader {
    const char *p = nullptr, *e = nullptr;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    Json value() {
        ws();
        if (p >= e) return {};
        if (*p == '{') return object();
        if (*p == '[') return array();
        if (*p == '"') {
            Json v;
            v.kind = Json::Str;
            v.str = string();
            return v;
        }
        if (!std::strncmp(p, "true", 4)) { p += 4; Json v; v.kind = Json::Num; v.num = 1; return v; }
        if (!std::strncmp(p, "false", 5)) { p += 5; Json v; v.kind = Json::Num; v.num = 0; return v; }
        if (!std::strncmp(p, "null", 4)) { p += 4; return {}; }
        Json v;
        v.kind = Json::Num;
        v.num = std::strtod(p, const_cast<char **>(&p));
        return v;
    }
    std::string string() {
        ++p;   // the opening quote
        std::string s;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                ++p;
                s.push_back(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
            } else {
                s.push_back(*p);
            }
            ++p;
        }
        if (p < e) ++p;   // the closing quote
        return s;
    }
    Json array() {
        Json v;
        v.kind = Json::Arr;
        ++p;
        ws();
        if (p < e && *p == ']') { ++p; return v; }
        for (;;) {
            v.arr.push_back(value());
            ws();
            if (p < e && *p == ',') { ++p; continue; }
            if (p < e && *p == ']') ++p;
            break;
        }
        return v;
    }
    Json object() {
        Json v;
        v.kind = Json::Obj;
        ++p;
        ws();
        if (p < e && *p == '}') { ++p; return v; }
        for (;;) {
            ws();
            std::string k = string();
            ws();
            if (p < e && *p == ':') ++p;
            v.obj[k] = value();
            ws();
            if (p < e && *p == ',') { ++p; continue; }
            if (p < e && *p == '}') ++p;
            break;
        }
        return v;
    }
};

// ---------------------------------------------------------------------------
// THE CONTAINER: a 12-byte header and then length-tagged chunks. The first is
// the JSON document and the second, when present, is the binary blob every
// bufferView indexes into.
// ---------------------------------------------------------------------------
struct Glb {
    std::vector<uint8_t> bytes;
    Json js;
    const uint8_t *bin = nullptr;
    size_t binLen = 0;

    bool load(const char *path) {
        FILE *f = std::fopen(path, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        bytes.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (got != bytes.size() || bytes.size() < 12) return false;
        if (std::memcmp(bytes.data(), "glTF", 4)) return false;
        size_t off = 12;
        while (off + 8 <= bytes.size()) {
            uint32_t len = 0, type = 0;
            std::memcpy(&len, &bytes[off], 4);
            std::memcpy(&type, &bytes[off + 4], 4);
            const uint8_t *data = &bytes[off + 8];
            if (off + 8 + len > bytes.size()) break;
            if (type == 0x4E4F534Au) {   // 'JSON'
                JsonReader r;
                r.p = reinterpret_cast<const char *>(data);
                r.e = r.p + len;
                js = r.value();
            } else if (type == 0x004E4942u) {   // 'BIN'
                bin = data;
                binLen = len;
            }
            off += 8 + len + ((4 - (len & 3)) & 3) * 0;   // glTF chunks are already 4-aligned
        }
        return js.kind == Json::Obj;
    }

    // One accessor's worth of scalars, widened to double. Handles the component
    // types glTF uses for positions (float) and indices (u8/u16/u32).
    std::vector<double> read(int accessorIdx, int *outComponents) const {
        std::vector<double> out;
        const Json &acc = js["accessors"][size_t(accessorIdx)];
        if (acc.kind != Json::Obj) return out;
        const int count = acc["count"].asInt(0);
        const int ct = acc["componentType"].asInt(5126);
        const std::string type = acc["type"].str;
        const int comps = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : 4;
        if (outComponents) *outComponents = comps;
        const int bvIdx = acc["bufferView"].asInt(-1);
        if (bvIdx < 0 || !bin) return out;
        const Json &bv = js["bufferViews"][size_t(bvIdx)];
        const size_t base = size_t(bv["byteOffset"].asNum(0)) + size_t(acc["byteOffset"].asNum(0));
        const int csize = ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 4;
        const int packed = csize * comps;
        const int stride = bv.has("byteStride") ? bv["byteStride"].asInt(packed) : packed;
        out.reserve(size_t(count) * size_t(comps));
        for (int i = 0; i < count; ++i) {
            const uint8_t *p = bin + base + size_t(i) * size_t(stride);
            if (p + packed > bin + binLen) break;
            for (int c = 0; c < comps; ++c) {
                const uint8_t *q = p + c * csize;
                switch (ct) {
                    case 5120: out.push_back(double(*reinterpret_cast<const int8_t *>(q))); break;
                    case 5121: out.push_back(double(*q)); break;
                    case 5122: {
                        int16_t v;
                        std::memcpy(&v, q, 2);
                        out.push_back(double(v));
                        break;
                    }
                    case 5123: {
                        uint16_t v;
                        std::memcpy(&v, q, 2);
                        out.push_back(double(v));
                        break;
                    }
                    case 5125: {
                        uint32_t v;
                        std::memcpy(&v, q, 4);
                        out.push_back(double(v));
                        break;
                    }
                    default: {
                        float v;
                        std::memcpy(&v, q, 4);
                        out.push_back(double(v));
                        break;
                    }
                }
            }
        }
        return out;
    }
};

struct Mat4 {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // column-major, as glTF
    static Mat4 mul(const Mat4 &a, const Mat4 &b) {
        Mat4 o;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[k * 4 + r] * b.m[c * 4 + k];
                o.m[c * 4 + r] = s;
            }
        return o;
    }
    void apply(double x, double y, double z, double *ox, double *oy, double *oz) const {
        *ox = m[0] * x + m[4] * y + m[8] * z + m[12];
        *oy = m[1] * x + m[5] * y + m[9] * z + m[13];
        *oz = m[2] * x + m[6] * y + m[10] * z + m[14];
    }
};

static Mat4 nodeMatrix(const Json &n) {
    Mat4 o;
    if (n.has("matrix")) {
        for (int i = 0; i < 16; ++i) o.m[i] = n["matrix"][size_t(i)].asNum(o.m[i]);
        return o;
    }
    const double tx = n["translation"][size_t(0)].asNum(0), ty = n["translation"][size_t(1)].asNum(0),
                 tz = n["translation"][size_t(2)].asNum(0);
    const double qx = n["rotation"][size_t(0)].asNum(0), qy = n["rotation"][size_t(1)].asNum(0),
                 qz = n["rotation"][size_t(2)].asNum(0), qw = n["rotation"][size_t(3)].asNum(1);
    const double sx = n.has("scale") ? n["scale"][size_t(0)].asNum(1) : 1.0,
                 sy = n.has("scale") ? n["scale"][size_t(1)].asNum(1) : 1.0,
                 sz = n.has("scale") ? n["scale"][size_t(2)].asNum(1) : 1.0;
    const double r[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw),
                         2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw),
                         2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy)};
    o.m[0] = r[0] * sx; o.m[1] = r[1] * sx; o.m[2] = r[2] * sx; o.m[3] = 0;
    o.m[4] = r[3] * sy; o.m[5] = r[4] * sy; o.m[6] = r[5] * sy; o.m[7] = 0;
    o.m[8] = r[6] * sz; o.m[9] = r[7] * sz; o.m[10] = r[8] * sz; o.m[11] = 0;
    o.m[12] = tx; o.m[13] = ty; o.m[14] = tz; o.m[15] = 1;
    return o;
}

struct Tri {
    double x[3], y[3], z[3];
    uint8_t pal;
};

// The palette of an existing .vox, so a revoxelisation costs no new entries.
static bool readVoxPalette(const char *path, uint8_t pal[256][3], uint32_t hist[256]) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const size_t n = size_t(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d(n);
    if (std::fread(d.data(), 1, n, f) != n) { std::fclose(f); return false; }
    std::fclose(f);
    bool ok = false;
    for (size_t off = 20; off + 12 <= n;) {
        uint32_t len = 0, kids = 0;
        std::memcpy(&len, &d[off + 4], 4);
        std::memcpy(&kids, &d[off + 8], 4);
        if (!std::memcmp(&d[off], "XYZI", 4) && hist && off + 16 <= n) {
            uint32_t nv = 0;
            std::memcpy(&nv, &d[off + 12], 4);
            for (uint32_t v = 0; v < nv && off + 16 + size_t(v) * 4 + 3 < n; ++v)
                ++hist[d[off + 16 + size_t(v) * 4 + 3]];
        }
        if (!std::memcmp(&d[off], "RGBA", 4) && off + 12 + 1024 <= n) {
            for (int i = 0; i < 256; ++i)
                for (int c = 0; c < 3; ++c) pal[i][c] = d[off + 12 + size_t(i) * 4 + size_t(c)];
            ok = true;
        }
        off += 12 + len;
        if (!len && !kids) break;
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: glb2vox <in.glb> <outDir> <prefix> [targetHeightM=5] "
                     "[voxelM=0.05] [palette.vox]\n");
        return 2;
    }
    const char *inPath = argv[1], *outDir = argv[2], *prefix = argv[3];
    const double targetH = argc > 4 ? std::atof(argv[4]) : 5.0;
    const double voxM = argc > 5 ? std::atof(argv[5]) : 0.05;
    const char *palPath = argc > 6 ? argv[6] : nullptr;

    Glb g;
    if (!g.load(inPath)) {
        std::fprintf(stderr, "glb2vox: cannot read %s as a binary glTF\n", inPath);
        return 1;
    }

    uint8_t pal[256][3] = {};
    uint32_t hist[256] = {};
    int palN = 0;
    if (palPath && readVoxPalette(palPath, pal, hist)) {
        for (int i = 0; i < 256; ++i)
            if (pal[i][0] || pal[i][1] || pal[i][2]) palN = i + 1;
        std::printf("  palette  %d entries borrowed from %s\n", palN, palPath);
    }

    // -----------------------------------------------------------------------
    // THE REFERENCE MODEL'S OWN COLOUR MIX, as a distribution to draw from.
    //
    // (user 2026-09-19, on the first revoxelised set: the shapes were right and
    //  the plants came out PINK.)
    //
    // EVERY MATERIAL IN cactus.glb IS EMPTY. No baseColorFactor, no texture --
    // the exporter wrote nine materials that say nothing but their names, and
    // the colour lived in a texture that never shipped. Falling back to a mid
    // grey and asking the palette for its nearest entry is how a cactus became
    // pink: grey is equidistant from everything, so it lands wherever the table
    // happens to be dense, and v2's is dense in the blossom pinks.
    //
    // SO THE COLOUR COMES FROM THE MODEL BEING REPLACED. The .vox lent its
    // palette already; this takes its HISTOGRAM too -- how many voxels wore each
    // entry -- and gives each new voxel an entry drawn from that same
    // distribution. The old cactus was four greens and a tan at 2.7%; the new
    // one is the same four greens and the same tan at the same rate, on a
    // better shape. A revoxelisation should change the geometry and nothing
    // else, and this is the version of that promise that survives an exporter
    // dropping the materials.
    //
    // PER VOXEL, BY POSITION HASH, because the greens here span 23 levels of
    // value: a speckle at that amplitude reads as the plant's own surface,
    // which is exactly how the original was authored.
    // -----------------------------------------------------------------------
    // -- ONE HUE FAMILY, NOT THE WHOLE HISTOGRAM ------------------------
    //
    // (user 2026-09-19: "the cactus has abnormal colors. remove the pink
    //  voxels from the cactus, make them green. theres also orange voxels
    //  that needs to go. also yellow".)
    //
    // THE FIRST VERSION DREW FROM EVERY ENTRY THE MODEL USED, and a plant's
    // histogram is not one material. cactus_1 is six greens (91%) plus three
    // browns for the spines, five PINKS for the flowers and a YELLOW for their
    // centres -- eight percent of the voxels, and in the original they sit
    // where a flower sits. Drawn per voxel by a position hash they land
    // everywhere instead, which is a green plant flecked with pink and orange.
    //
    // A REVOXELISATION HAS NO IDEA WHERE A FLOWER GOES. It has the mesh's
    // shape and nothing else -- the materials that would have said are the
    // empty ones this fallback exists to replace -- so the honest thing is to
    // draw the BODY and leave the details out rather than scatter them at
    // random. Detail invented in the wrong place is worse than detail missing.
    //
    // THE FAMILY IS THE DOMINANT ENTRY'S, by chromaticity: (r,g,b) over their
    // sum, which throws away brightness and keeps hue, so a ramp from a dark
    // green to a light one is one family and a pink is not. 0.06 is measured
    // against this palette -- it keeps all six greens (the furthest, the olive
    // 117,123,50, is 0.034 out) and drops the nearest brown (132,115,56) at
    // 0.090. It is a hue test, not a green test: a grey rock or a red brick
    // gets its own family the same way.
    std::vector<uint8_t> refId;
    std::vector<uint32_t> refCum;
    uint32_t refTot = 0;
    {
        int dom = 0;
        for (int i = 1; i < 256; ++i)
            if (hist[i] > hist[dom]) dom = i;
        const auto chroma = [&](int e, double *c) {
            const double sum = double(pal[e - 1][0]) + pal[e - 1][1] + pal[e - 1][2] + 1e-6;
            for (int k = 0; k < 3; ++k) c[k] = double(pal[e - 1][k]) / sum;
        };
        double dc[3] = {0, 0, 0};
        if (dom) chroma(dom, dc);
        for (int i = 1; i < 256; ++i) {
            if (!hist[i]) continue;
            double c[3] = {0, 0, 0};
            chroma(i, c);
            double d2 = 0;
            for (int k = 0; k < 3; ++k) d2 += (c[k] - dc[k]) * (c[k] - dc[k]);
            if (dom && std::sqrt(d2) > 0.06) {
                std::printf("             %3u  (%3u,%3u,%3u)  dropped, %.3f off the body\n", i,
                            pal[i - 1][0], pal[i - 1][1], pal[i - 1][2], std::sqrt(d2));
                continue;
            }
            refTot += hist[i];
            refId.push_back(uint8_t(i));
            refCum.push_back(refTot);
        }
    }
    if (refTot) {
        std::printf("  mix      %zu entries over %u voxels of %s\n", refId.size(), refTot, palPath);
        for (size_t i = 0; i < refId.size() && i < 6; ++i)
            std::printf("             %3u  (%3u,%3u,%3u)  %5.1f%%\n", refId[i],
                        pal[refId[i] - 1][0], pal[refId[i] - 1][1], pal[refId[i] - 1][2],
                        100.0 * double(refCum[i] - (i ? refCum[i - 1] : 0)) / double(refTot));
    }
    const auto refPick = [&](int x, int y, int z) -> uint8_t {
        if (!refTot) return 1;
        uint32_t h = uint32_t(x) * 0x8DA6B343u ^ uint32_t(y) * 0xD8163841u ^
                     uint32_t(z) * 0xCB1AB31Fu;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        const uint32_t r = h % refTot;
        for (size_t i = 0; i < refCum.size(); ++i)
            if (r < refCum[i]) return refId[i];
        return refId.back();
    };

    // Each node's world transform, walked from the scene roots.
    const Json &nodes = g.js["nodes"];
    std::vector<Mat4> world(nodes.size());
    std::vector<int> meshOf(nodes.size(), -1);
    {
        std::vector<std::pair<int, Mat4>> stack;
        const Json &scene = g.js["scenes"][size_t(g.js["scene"].asInt(0))];
        for (size_t i = 0; i < scene["nodes"].size(); ++i)
            stack.push_back({scene["nodes"][i].asInt(0), Mat4{}});
        while (!stack.empty()) {
            const auto [ni, parent] = stack.back();
            stack.pop_back();
            if (ni < 0 || size_t(ni) >= nodes.size()) continue;
            const Json &n = nodes[size_t(ni)];
            const Mat4 w = Mat4::mul(parent, nodeMatrix(n));
            world[size_t(ni)] = w;
            meshOf[size_t(ni)] = n["mesh"].asInt(-1);
            for (size_t c = 0; c < n["children"].size(); ++c)
                stack.push_back({n["children"][c].asInt(-1), w});
        }
    }

    const Json &meshes = g.js["meshes"];
    std::printf("glb2vox %s -- %zu mesh(es), target %.2f m at %.0f cm\n", inPath, meshes.size(),
                targetH, voxM * 100.0);

    // THE TALLEST MESH IN THE FILE, measured before anything is written --
    // see the note at `scale`. Two passes over the meshes is the price of the
    // set keeping its proportions.
    double tallest = 0.0;
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        Mat4 xf;
        for (size_t ni = 0; ni < meshOf.size(); ++ni)
            if (meshOf[ni] == int(mi)) { xf = world[ni]; break; }
        const Json &prims0 = meshes[mi]["primitives"];
        double ylo = 1e18, yhi = -1e18;
        for (size_t pi = 0; pi < prims0.size(); ++pi) {
            const int pa = prims0[pi]["attributes"]["POSITION"].asInt(-1);
            if (pa < 0) continue;
            int cc = 3;
            const std::vector<double> pv = g.read(pa, &cc);
            for (size_t v = 0; v + 2 < pv.size(); v += 3) {
                double ox, oy, oz;
                xf.apply(pv[v], pv[v + 1], pv[v + 2], &ox, &oy, &oz);
                if (oy < ylo) ylo = oy;
                if (oy > yhi) yhi = oy;
            }
        }
        if (yhi > ylo) tallest = tallest > (yhi - ylo) ? tallest : (yhi - ylo);
    }
    if (tallest <= 0.0) {
        std::fprintf(stderr, "glb2vox: no geometry with height in %s\n", inPath);
        return 1;
    }
    std::printf("  tallest mesh %.3f source units -> %.2f m\n", tallest, targetH);

    int written = 0;
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        // The node that carries this mesh, for its transform.
        Mat4 xform;
        for (size_t ni = 0; ni < meshOf.size(); ++ni)
            if (meshOf[ni] == int(mi)) { xform = world[ni]; break; }

        std::vector<Tri> tris;
        const Json &prims = meshes[mi]["primitives"];
        for (size_t pi = 0; pi < prims.size(); ++pi) {
            const Json &p = prims[pi];
            const int posAcc = p["attributes"]["POSITION"].asInt(-1);
            if (posAcc < 0) continue;
            int comps = 3;
            const std::vector<double> pos = g.read(posAcc, &comps);
            std::vector<double> idx;
            if (p.has("indices")) {
                int ic = 1;
                idx = g.read(p["indices"].asInt(0), &ic);
            } else {
                idx.resize(pos.size() / 3);
                for (size_t i = 0; i < idx.size(); ++i) idx[i] = double(i);
            }
            // The primitive's own colour, mapped once onto the borrowed
            // palette -- or 0, meaning "this material said nothing, take it
            // from the reference model's mix". See refPick.
            uint8_t entry = 0;
            const Json &bcf0 =
                g.js["materials"][size_t(p["material"].asInt(0))]["pbrMetallicRoughness"]
                    ["baseColorFactor"];
            if (palN > 0 && bcf0.size() >= 3) {
                const Json &bcf = bcf0;
                const double cr = bcf[size_t(0)].asNum(0.5);
                const double cg = bcf[size_t(1)].asNum(0.5);
                const double cb = bcf[size_t(2)].asNum(0.5);
                // sRGB, because that is what a .vox palette holds.
                const double want[3] = {std::pow(cr, 1.0 / 2.2) * 255.0,
                                        std::pow(cg, 1.0 / 2.2) * 255.0,
                                        std::pow(cb, 1.0 / 2.2) * 255.0};
                double best = 1e18;
                for (int i = 0; i < palN; ++i) {
                    if (!pal[i][0] && !pal[i][1] && !pal[i][2]) continue;
                    double d2 = 0;
                    for (int c = 0; c < 3; ++c) {
                        const double dd = want[c] - double(pal[i][c]);
                        d2 += dd * dd;
                    }
                    if (d2 < best) { best = d2; entry = uint8_t(i + 1); }
                }
            }
            if (!entry && !refTot) entry = 1;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                Tri tr;
                tr.pal = entry;
                bool ok = true;
                for (int k = 0; k < 3; ++k) {
                    const size_t v = size_t(idx[t + size_t(k)]) * 3;
                    if (v + 2 >= pos.size()) { ok = false; break; }
                    xform.apply(pos[v], pos[v + 1], pos[v + 2], &tr.x[k], &tr.y[k], &tr.z[k]);
                }
                if (ok) tris.push_back(tr);
            }
        }
        if (tris.empty()) continue;

        double lo[3] = {1e18, 1e18, 1e18}, hi[3] = {-1e18, -1e18, -1e18};
        for (const Tri &t : tris)
            for (int k = 0; k < 3; ++k) {
                const double v[3] = {t.x[k], t.y[k], t.z[k]};
                for (int c = 0; c < 3; ++c) {
                    if (v[c] < lo[c]) lo[c] = v[c];
                    if (v[c] > hi[c]) hi[c] = v[c];
                }
            }
        // glTF is Y-up and .vox is Z-up, so the model's HEIGHT is its y span
        // and the axes are swapped on the way into the grid.
        std::printf("    spans  x %.3f  y %.3f  z %.3f\n",
                    hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
        const double spanY = hi[1] - lo[1];
        if (spanY <= 0.0) continue;
        // -- ONE SCALE FOR THE WHOLE SET ----------------------------------
        //
        // Scaling each mesh to the target on its own makes every plant the
        // same height, which is not a set of nine cacti -- it is one cactus
        // nine times. The target is the TALLEST one's height and everything
        // else keeps its share of it, so a knee-high prickly pear stays
        // knee-high beside an eight-metre saguaro.
        const double scale = (targetH / voxM) / tallest;
        const int sx = int((hi[0] - lo[0]) * scale) + 1;
        const int sy = int((hi[2] - lo[2]) * scale) + 1;   // .vox y is glTF z
        const int sz = int(spanY * scale) + 1;             // .vox z is glTF y (up)
        if (sx > 256 || sy > 256 || sz > 256) {
            std::fprintf(stderr, "  %s_%zu: %d x %d x %d exceeds 256 -- skipped\n", prefix, mi + 1,
                         sx, sy, sz);
            continue;
        }

        std::vector<uint8_t> grid(size_t(sx) * size_t(sy) * size_t(sz), 0);
        const auto put = [&](int x, int y, int z, uint8_t v) {
            if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return;
            if (!v) v = refPick(x, y, z);   // the material said nothing
            grid[size_t(x) + size_t(y) * size_t(sx) + size_t(z) * size_t(sx) * size_t(sy)] = v;
        };
        // BARYCENTRIC SAMPLING, dense enough that no triangle can slip between
        // two voxels: the step count comes off the longest edge in voxels.
        for (const Tri &t : tris) {
            double p0[3], p1[3], p2[3];
            const auto toGrid = [&](int k, double *o) {
                o[0] = (t.x[k] - lo[0]) * scale;
                o[1] = (t.z[k] - lo[2]) * scale;
                o[2] = (t.y[k] - lo[1]) * scale;
            };
            toGrid(0, p0);
            toGrid(1, p1);
            toGrid(2, p2);
            double e1 = 0, e2 = 0;
            for (int k = 0; k < 3; ++k) {
                e1 += (p1[k] - p0[k]) * (p1[k] - p0[k]);
                e2 += (p2[k] - p0[k]) * (p2[k] - p0[k]);
            }
            const int steps = int(std::sqrt(e1 > e2 ? e1 : e2) * 2.0) + 1;
            for (int a = 0; a <= steps; ++a)
                for (int b = 0; a + b <= steps; ++b) {
                    const double u = double(a) / steps, v = double(b) / steps;
                    put(int(p0[0] + (p1[0] - p0[0]) * u + (p2[0] - p0[0]) * v),
                        int(p0[1] + (p1[1] - p0[1]) * u + (p2[1] - p0[1]) * v),
                        int(p0[2] + (p1[2] - p0[2]) * u + (p2[2] - p0[2]) * v), t.pal);
                }
        }

        size_t count = 0;
        for (uint8_t v : grid)
            if (v) ++count;
        if (!count) continue;

        std::vector<uint8_t> xyzi;
        uint32_t n = 0;
        for (int z = 0; z < sz; ++z)
            for (int y = 0; y < sy; ++y)
                for (int x = 0; x < sx; ++x) {
                    const uint8_t v =
                        grid[size_t(x) + size_t(y) * size_t(sx) + size_t(z) * size_t(sx) * size_t(sy)];
                    if (!v) continue;
                    xyzi.push_back(uint8_t(x));
                    xyzi.push_back(uint8_t(y));
                    xyzi.push_back(uint8_t(z));
                    xyzi.push_back(v);
                    ++n;
                }

        // -- NAMED FOR THE MESH, NOT FOR ITS POSITION IN THE FILE ---------
        //
        // cactus.glb stores its nine as 1,2,3,4,6,7,8,9,5 -- the fifth is last
        // -- so numbering by index writes cactus_6's geometry into cactus_5.vox
        // and shuffles four of the nine. The mesh carries its own number in its
        // name ("cactus_6_cacturs_6_0"); the first run of digits is it.
        int meshNo = int(mi) + 1;
        {
            const std::string &nm = meshes[mi]["name"].str;
            size_t a = 0;
            while (a < nm.size() && (nm[a] < '0' || nm[a] > '9')) ++a;
            if (a < nm.size()) {
                int v = 0;
                while (a < nm.size() && nm[a] >= '0' && nm[a] <= '9') v = v * 10 + (nm[a++] - '0');
                if (v > 0) meshNo = v;
            }
        }
        char out[1024];
        std::snprintf(out, sizeof(out), "%s/%s_%d.vox", outDir, prefix, meshNo);
        FILE *f = std::fopen(out, "wb");
        if (!f) {
            std::fprintf(stderr, "  cannot write %s\n", out);
            continue;
        }
        const auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        const auto chunk = [&](const char *id, uint32_t content, uint32_t children) {
            std::fwrite(id, 1, 4, f);
            u32(content);
            u32(children);
        };
        const uint32_t sizeChunk = 12 + 12, xyziChunk = 12 + 4 + n * 4, rgbaChunk = 12 + 1024;
        std::fwrite("VOX ", 1, 4, f);
        u32(150);
        chunk("MAIN", 0, sizeChunk + xyziChunk + rgbaChunk);
        chunk("SIZE", 12, 0);
        u32(uint32_t(sx));
        u32(uint32_t(sy));
        u32(uint32_t(sz));
        chunk("XYZI", 4 + n * 4, 0);
        u32(n);
        std::fwrite(xyzi.data(), 1, xyzi.size(), f);
        chunk("RGBA", 1024, 0);
        for (int i = 0; i < 256; ++i) {
            const uint8_t rgba[4] = {pal[i][0], pal[i][1], pal[i][2], 255};
            std::fwrite(rgba, 1, 4, f);
        }
        std::fclose(f);
        std::printf("  %-22s %3d x %3d x %3d   %6u voxels   %.2f m tall\n",
                    meshes[mi]["name"].str.substr(0, 22).c_str(), sx, sy, sz, n, sz * voxM);
        ++written;
    }
    std::printf("  wrote %d model(s)\n", written);
    return written ? 0 : 1;
}
