// ---------------------------------------------------------------------------
// vox.h -- a MagicaVoxel .vox reader, ported from voxelbit's rust/src/vox.rs.
//
// TWO COORDINATE SYSTEMS, AND THE ONE PLACE THEY MEET
// MagicaVoxel is Z-UP: a model's sz is its height and sx/sy are the ground
// plane. This renderer is Y-UP. The swap happens exactly once, in toWorld(),
// which emits the layout the stamper wants -- so nothing downstream has to
// remember which convention it is holding. Getting this wrong does not crash;
// it lays the tree on its side.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace v2 {

// A parsed model in the file's own Z-up layout, indexed x + y*sx + z*sx*sy.
struct VoxModel {
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> m;                    // palette indices; 0 is empty
    std::array<std::array<uint8_t, 4>, 255> pal{};  // entry i (1-based) is pal[i-1]

    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return 0;
        return m[size_t(x) + size_t(y) * sx + size_t(z) * sx * sy];
    }
};

// A model in WORLD layout: x and z horizontal, y vertical, indexed
// x + z*sx + y*sx*sz -- the same indexing the voxel grid uses.
struct VoxAsset {
    int sx = 0, sy = 0, sz = 0;  // sy is the height
    std::vector<uint8_t> a;

    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return 0;
        return a[size_t(x) + size_t(z) * sx + size_t(y) * sx * sz];
    }
    int voxelCount() const {
        int n = 0;
        for (uint8_t v : a)
            if (v) ++n;
        return n;
    }
};

// MagicaVoxel's built-in palette, for files carrying no RGBA chunk. Generated
// from the documented 6x6x6 ramp rather than pasted as 255 literals.
inline std::array<std::array<uint8_t, 4>, 255> defaultPalette() {
    std::array<std::array<uint8_t, 4>, 255> p{};
    const uint8_t lv[6] = {255, 204, 153, 102, 51, 0};
    size_t i = 0;
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b)
                if (i < 255) p[i++] = {lv[r], lv[g], lv[b], 255};
    return p;
}

inline int32_t i32le(const uint8_t *b, size_t o) {
    int32_t v;
    std::memcpy(&v, b + o, 4);
    return v;
}

// Parse the first model in a .vox file.
//
// Chunks are walked rather than assumed to be in a fixed order, and unknown
// ones (nTRN, nSHP, MATL, LAYR and the rest of the scene-graph extensions) are
// skipped by their declared size -- a reader that assumed SIZE and XYZI came
// first would work on these files and break on the next MagicaVoxel export.
inline bool voxParse(const std::vector<uint8_t> &raw, VoxModel *out, std::string *err) {
    auto fail = [&](const char *why) {
        if (err) *err = why;
        return false;
    };
    if (raw.size() <= 8 || std::memcmp(raw.data(), "VOX ", 4) != 0) return fail("not a .vox file");

    bool haveSize = false;
    int sx = 0, sy = 0, sz = 0;
    const uint8_t *voxels = nullptr;
    size_t voxelBytes = 0;
    auto pal = defaultPalette();

    size_t o = 8;
    while (o + 12 <= raw.size()) {
        const uint8_t *id = raw.data() + o;
        const size_t content = size_t(std::max(0, i32le(raw.data(), o + 4)));
        const size_t children = size_t(std::max(0, i32le(raw.data(), o + 8)));
        const size_t body = o + 12;
        if (body + content > raw.size()) break;

        if (std::memcmp(id, "MAIN", 4) == 0) {
            // Descend into MAIN rather than skipping it: its children are the payload.
            o = body + content;
            continue;
        }
        if (std::memcmp(id, "SIZE", 4) == 0 && content >= 12) {
            sx = i32le(raw.data(), body);
            sy = i32le(raw.data(), body + 4);
            sz = i32le(raw.data(), body + 8);
            haveSize = true;
        } else if (std::memcmp(id, "XYZI", 4) == 0 && content >= 4 && voxels == nullptr) {
            const size_t n = size_t(std::max(0, i32le(raw.data(), body)));
            const size_t need = 4 + n * 4;
            if (content < need) return fail("XYZI shorter than its own count");
            voxels = raw.data() + body + 4;
            voxelBytes = n * 4;
        } else if (std::memcmp(id, "RGBA", 4) == 0 && content >= 1024) {
            for (int i = 0; i < 255; ++i) {
                const size_t p = body + size_t(i) * 4;
                pal[i] = {raw[p], raw[p + 1], raw[p + 2], raw[p + 3]};
            }
        }
        o = body + content + children;
    }

    if (!haveSize) return fail("no SIZE chunk");
    if (!voxels) return fail("no XYZI chunk");
    if (sx <= 0 || sy <= 0 || sz <= 0 || double(sx) * sy * sz > 64.0 * (1 << 20))
        return fail("implausible model dimensions");

    out->sx = sx;
    out->sy = sy;
    out->sz = sz;
    out->pal = pal;
    out->m.assign(size_t(sx) * sy * sz, 0);
    for (size_t q = 0; q + 4 <= voxelBytes; q += 4) {
        const int x = voxels[q], y = voxels[q + 1], z = voxels[q + 2];
        const uint8_t c = voxels[q + 3];
        // Out-of-range voxels are dropped rather than fatal: a hand-edited file
        // occasionally carries one past its own SIZE, and losing it beats
        // refusing the tree.
        if (x < sx && y < sy && z < sz)
            out->m[size_t(x) + size_t(y) * sx + size_t(z) * sx * sy] = c;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Every model in the file, not just the first.
//
// A .vox may carry many SIZE/XYZI pairs -- flowers.vox has six, one per flower
// -- and voxParse deliberately stops at the first because that is what a single
// -model asset wants. This walks the whole chunk list instead, pairing each
// SIZE with the XYZI that follows it. The RGBA chunk is shared: it appears once,
// usually AFTER the models, so the palette is applied to all of them at the end
// rather than as it is found.
// ---------------------------------------------------------------------------
inline bool voxParseAll(const std::vector<uint8_t> &raw, std::vector<VoxModel> *out,
                        std::string *err) {
    if (raw.size() <= 8 || std::memcmp(raw.data(), "VOX ", 4) != 0) {
        if (err) *err = "not a .vox file";
        return false;
    }

    auto pal = defaultPalette();
    int sx = 0, sy = 0, sz = 0;
    bool haveSize = false;

    size_t o = 8;
    while (o + 12 <= raw.size()) {
        const uint8_t *id = raw.data() + o;
        const size_t content = size_t(std::max(0, i32le(raw.data(), o + 4)));
        const size_t children = size_t(std::max(0, i32le(raw.data(), o + 8)));
        const size_t body = o + 12;
        if (body + content > raw.size()) break;

        if (std::memcmp(id, "MAIN", 4) == 0) {
            o = body + content;  // descend: the children are the payload
            continue;
        }
        if (std::memcmp(id, "SIZE", 4) == 0 && content >= 12) {
            sx = i32le(raw.data(), body);
            sy = i32le(raw.data(), body + 4);
            sz = i32le(raw.data(), body + 8);
            haveSize = (sx > 0 && sy > 0 && sz > 0 &&
                        double(sx) * sy * sz <= 64.0 * (1 << 20));
        } else if (std::memcmp(id, "XYZI", 4) == 0 && content >= 4 && haveSize) {
            const size_t n = size_t(std::max(0, i32le(raw.data(), body)));
            if (content >= 4 + n * 4) {
                VoxModel m;
                m.sx = sx;
                m.sy = sy;
                m.sz = sz;
                m.m.assign(size_t(sx) * sy * sz, 0);
                const uint8_t *v = raw.data() + body + 4;
                for (size_t q = 0; q + 4 <= n * 4; q += 4) {
                    const int x = v[q], y = v[q + 1], z = v[q + 2];
                    if (x < sx && y < sy && z < sz)
                        m.m[size_t(x) + size_t(y) * sx + size_t(z) * sx * sy] = v[q + 3];
                }
                out->push_back(std::move(m));
            }
            haveSize = false;  // one XYZI per SIZE
        } else if (std::memcmp(id, "RGBA", 4) == 0 && content >= 1024) {
            for (int i = 0; i < 255; ++i) {
                const size_t p = body + size_t(i) * 4;
                pal[i] = {raw[p], raw[p + 1], raw[p + 2], raw[p + 3]};
            }
        }
        o = body + content + children;
    }

    if (out->empty()) {
        if (err) *err = "no models in file";
        return false;
    }
    for (VoxModel &m : *out) m.pal = pal;
    return true;
}

inline bool voxLoadAll(const std::string &path, std::vector<VoxModel> *out, std::string *err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(size_t(n > 0 ? n : 0));
    const size_t got = raw.empty() ? 0 : std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    raw.resize(got);
    return voxParseAll(raw, out, err);
}

inline bool voxLoad(const std::string &path, VoxModel *out, std::string *err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(size_t(n > 0 ? n : 0));
    const size_t got = raw.empty() ? 0 : std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    raw.resize(got);
    return voxParse(raw, out, err);
}

// One model's sub-box, converted to world layout and trimmed to its own bounds.
// This is rotation 0 of vox.rs's four quarter-turns: the model's z becomes the
// world's y, and its y becomes the world's z.
inline VoxAsset toWorld(const VoxModel &mo, int x0, int x1) {
    int minX = x1, maxX = x0 - 1, minY = mo.sy, maxY = -1, minZ = mo.sz, maxZ = -1;
    for (int z = 0; z < mo.sz; ++z)
        for (int y = 0; y < mo.sy; ++y)
            for (int x = x0; x < x1; ++x)
                if (mo.at(x, y, z)) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                    minZ = std::min(minZ, z);
                    maxZ = std::max(maxZ, z);
                }

    VoxAsset out;
    if (maxX < minX) return out;  // empty slab

    out.sx = maxX - minX + 1;
    out.sz = maxY - minY + 1;  // model y is world z
    out.sy = maxZ - minZ + 1;  // model z is world y (the height)
    out.a.assign(size_t(out.sx) * out.sy * out.sz, 0);

    for (int z = minZ; z <= maxZ; ++z)
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x) {
                const uint8_t v = mo.at(x, y, z);
                if (!v) continue;
                const int wx = x - minX, wz = y - minY, wy = z - minZ;
                out.a[size_t(wx) + size_t(wz) * out.sx + size_t(wy) * out.sx * out.sz] = v;
            }
    return out;
}

// Several models authored side by side in one file, split on the empty columns
// between them. birch.vox holds four birch variants this way, so "load the
// birch" means picking one of these rather than stamping the whole row.
inline std::vector<VoxAsset> splitAlongX(const VoxModel &mo) {
    std::vector<bool> occupied(size_t(mo.sx), false);
    for (int x = 0; x < mo.sx; ++x)
        for (int z = 0; z < mo.sz && !occupied[size_t(x)]; ++z)
            for (int y = 0; y < mo.sy; ++y)
                if (mo.at(x, y, z)) {
                    occupied[size_t(x)] = true;
                    break;
                }

    std::vector<VoxAsset> out;
    int x = 0;
    while (x < mo.sx) {
        if (!occupied[size_t(x)]) {
            ++x;
            continue;
        }
        int end = x;
        while (end < mo.sx && occupied[size_t(end)]) ++end;
        VoxAsset a = toWorld(mo, x, end);
        if (a.sx > 0) out.push_back(std::move(a));
        x = end;
    }
    return out;
}

}  // namespace v2
