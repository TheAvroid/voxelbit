// ---------------------------------------------------------------------------
// voxelworld.h -- the world as 10 cm voxels, meshed into faces for OptiX.
//
// The terrain is quantised to the same 10 cm grid the pine models are authored
// on, so a trunk sits in ground made of the lattice the trunk is made of. A
// smooth landscape under a voxel tree reads as two different games.
//
// WHY FACES AND NOT BOXES: a voxel column stack could go to OptiX as one custom
// AABB primitive per column, which is far less memory. It would also be far
// slower. Custom primitives are intersected by a shader the SM runs; triangles
// are intersected by the RT cores in fixed function. On a 4070 that is most of
// an order of magnitude on a scene traced tens of millions of times a frame. So
// the surface is extracted as quads -- only faces with nothing in front of
// them. Interior voxels never become geometry at all, which is what keeps a
// 1.5-million-column patch tractable.
//
// WHAT v2 ADDS IS THE FACE DIRECTION. v4 let Embree hand back a geometric
// normal per hit and never stored one. OptiX will do the same via
// optixGetTriangleVertexData, but only on a GAS built with random vertex access
// -- which costs memory on every acceleration structure in the scene, to
// recompute a cross product for a face that was axis-aligned when it was
// emitted and is axis-aligned still. Storing the direction the face was emitted
// in costs one byte per triangle and makes the normal exact by construction.
// It is packed with the material into a single uint16 so the closest-hit
// program touches one array, not two.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "../core/noise.h"
#include "../core/vecmath.h"
#include "vox.h"

namespace v2 {

// The edge of a voxel, in metres. This is the number the pine assets are
// authored against -- they come out 22.5 m tall at this scale -- and every
// other length in the world follows from it.
constexpr float VOXEL_M = 0.1f;

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------
namespace mat {
constexpr uint8_t AIR = 0;
constexpr uint8_t ROCK = 1;
constexpr uint8_t DIRT = 2;
constexpr uint8_t MOSS = 3;
constexpr uint8_t SAND = 4;
constexpr uint8_t SILT = 5;
constexpr uint8_t NEEDLE_LITTER = 6;  // the dropped-needle floor a conifer stand builds

// GROUND COLOURS BORROWED FROM THE TREES.
//
// A single flat green for grass and a single flat brown for soil is what made
// the floor read as a painted plane under a detailed canopy: the eye finds the
// repeat instantly when a whole hillside is one value. These slots are filled
// AFTER the pines load, from the greens and browns the models actually use, so
// the ground is made of the same palette as the things standing in it -- which
// is the cheapest possible way to make a scene look like it belongs together.
constexpr uint8_t GRASS_0 = 7;
constexpr uint8_t GRASS_COUNT = 4;   // 7..10
constexpr uint8_t SOIL_0 = 11;
constexpr uint8_t SOIL_COUNT = 3;    // 11..13
constexpr uint8_t TREE_BASE = 14;  // model palette entries are allocated from here up
constexpr uint8_t COUNT = 255;
}  // namespace mat

// Everything the BSDF asks a surface for, per material id. This struct is
// uploaded to the GPU verbatim, so it stays a POD of floats with no padding
// surprises: 4 floats, 16 bytes, one aligned load in the hit program.
struct MaterialLook {
    Vec3 albedo{0.4f, 0.4f, 0.4f};
    float roughness = 0.95f;
    float specular = 0.03f;
    float translucency = 0.0f;
    float pad0 = 0.0f, pad1 = 0.0f;
};

inline float srgbToLinearF(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

// ---------------------------------------------------------------------------
// The material table
//
// Palette entries arrive per model and are deduplicated by colour, so nine pine
// variants that share their greens share their material slots too.
//
// FOLIAGE IS CLASSIFIED FROM THE COLOUR, not from a palette index range. The
// nine assets do not agree on where in the palette their needles live, and
// hardcoding a split would put bark roughness on needles for most of them.
// ---------------------------------------------------------------------------
class Palette {
  public:
    Palette() { buildGround(); }

    uint8_t forModelColor(const std::array<uint8_t, 4> &c) {
        const uint32_t key = (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | uint32_t(c[2]);
        auto it = index_.find(key);
        if (it != index_.end()) return it->second;
        if (next_ >= mat::COUNT) {
            ++overflow_;
            return mat::AIR;
        }

        const uint8_t id = next_++;
        MaterialLook &m = look_[id];
        m.albedo = Vec3(srgbToLinearF(float(c[0]) / 255.0f), srgbToLinearF(float(c[1]) / 255.0f),
                        srgbToLinearF(float(c[2]) / 255.0f));

        // Green-dominant is foliage; anything else on a conifer is wood.
        const bool foliage = (c[1] > c[0] && c[1] > c[2]);
        if (foliage) {
            // Needles are waxy and thin enough to pass light. That translucency
            // is what stops a backlit canopy from reading as a black cut-out --
            // the single most common way a rendered conifer looks wrong.
            m.roughness = 0.50f;
            m.specular = 0.045f;
            m.translucency = 0.45f;
            // The authored olive is very dark once linearised, and a canopy of
            // it reads as a black mass under its own shadow. A gentle lift
            // toward the asset's own hue keeps the colour and finds the form.
            m.albedo = m.albedo * 1.7f + Vec3(0.012f, 0.020f, 0.006f);
        } else {
            m.roughness = 0.88f;  // bark is coarse
            m.specular = 0.020f;
            m.translucency = 0.0f;
        }
        index_.emplace(key, id);
        return id;
    }

    const MaterialLook &operator[](uint8_t id) const { return look_[id]; }
    const std::vector<MaterialLook> &table() const { return look_; }
    int used() const { return next_; }
    int overflowed() const { return overflow_; }

    // -----------------------------------------------------------------------
    // Fill the ground slots from the colours the pines turned out to use.
    //
    // Called once, after every model has been through forModelColor and before
    // the terrain is meshed -- which is the whole reason loadPines() now runs
    // BEFORE buildTerrain(). The greens are sampled spread across the foliage
    // entries rather than taken consecutively: adjacent palette entries in a
    // MagicaVoxel model are usually a shading ramp of one hue, so the first
    // four would have been four barely-different greens.
    // -----------------------------------------------------------------------
    void deriveGroundFromTrees() {
        std::vector<uint8_t> foliage, bark;
        for (int i = mat::TREE_BASE; i < next_; ++i) {
            if (look_[i].translucency > 0.0f) { foliage.push_back(uint8_t(i)); continue; }

            // NOT EVERY NON-GREEN ENTRY IS BARK. forModelColor classifies by
            // green dominance, so "bark" is really "everything else" -- and a
            // pine's palette carries reds and near-whites for cut ends and
            // highlights. Sampling those as soil painted scarlet and chalk-white
            // patches across whole hillsides, which is exactly what showed up.
            //
            // Soil has to be warm, mid-dark and unsaturated. Anything outside
            // that is a highlight, not a ground colour.
            const Vec3 c = look_[i].albedo;
            const float lum = luminance(c);
            const bool warm = c.x >= c.y && c.y >= c.z;
            const float sat = maxComp(c) > 0.0f ? (maxComp(c) - minf(c.x, minf(c.y, c.z))) /
                                                      maxComp(c)
                                                : 0.0f;
            if (warm && lum > 0.015f && lum < 0.30f && sat < 0.72f) bark.push_back(uint8_t(i));
        }

        for (int k = 0; k < mat::GRASS_COUNT; ++k) {
            MaterialLook &m = look_[mat::GRASS_0 + k];
            if (foliage.empty()) {
                // No models loaded: a plain green, so the world still renders.
                m.albedo = Vec3(0.14f + 0.04f * k, 0.26f + 0.05f * k, 0.10f + 0.02f * k);
            } else {
                const size_t pick = (foliage.size() * (2 * k + 1)) / (2 * mat::GRASS_COUNT);
                // Ground grass sits in the open and is lit far harder than
                // needles inside a crown, so the canopy value read straight
                // across looks like wet moss. Lifted, and pushed slightly
                // yellow, which is what distinguishes grass from conifer.
                // The foliage albedo has ALREADY been lifted 1.7x by
                // forModelColor, for a canopy that sits in its own shadow.
                // Lifting it again on top of that is what made the first pass
                // read as fluorescent plastic; grass in the open wants a small
                // nudge toward yellow and nothing more.
                const Vec3 c = look_[foliage[pick]].albedo;
                // 0.45, not 1.0, and the reason is LIGHTING not colour. The
                // foliage value is tuned for needles sitting inside their own
                // crown, in shadow most of the day. Grass stands in the open
                // taking full sun, so the same albedo renders far brighter --
                // reading it across literally is what made the floor glow
                // yellow-green under a dark canopy. Matching how they LOOK
                // means the grass albedo has to be well below the foliage one.
                m.albedo = Vec3(c.x * 0.45f + 0.008f, c.y * 0.44f + 0.010f, c.z * 0.32f);
            }
            m.roughness = 0.88f;
            m.specular = 0.022f;
            // Thin blades, lit from behind at a low sun -- the same reason the
            // needles have it. Without it a strand is a black stick at dawn.
            // Kept well below the needles' 0.45: grass caught the sun from
            // every angle at that value and the whole floor glowed.
            m.translucency = 0.22f;
        }

        for (int k = 0; k < mat::SOIL_COUNT; ++k) {
            MaterialLook &m = look_[mat::SOIL_0 + k];
            if (bark.empty()) {
                m.albedo = Vec3(0.26f + 0.05f * k, 0.19f + 0.03f * k, 0.13f + 0.02f * k);
            } else {
                const size_t pick = (bark.size() * (2 * k + 1)) / (2 * mat::SOIL_COUNT);
                const Vec3 c = look_[bark[pick]].albedo;
                // Soil is darker and less saturated than the trunk it came
                // from -- bark read literally makes the ground look like decking.
                m.albedo = Vec3(c.x * 0.85f + 0.020f, c.y * 0.80f + 0.014f, c.z * 0.78f + 0.010f);
            }
            m.roughness = 0.95f;
            m.specular = 0.020f;
            m.translucency = 0.0f;
        }

    }

  private:
    void set(uint8_t id, float r, float g, float b, float rough) {
        look_[id].albedo = Vec3(r, g, b);
        look_[id].roughness = rough;
        look_[id].specular = 0.025f;
        look_[id].translucency = 0.0f;
    }

    void buildGround() {
        // Deliberately desaturated: a path tracer bounces light between these
        // surfaces many times, and a saturated ground compounds into a colour
        // cast over everything above it.
        // These are LINEAR albedos already -- an earlier pass ran them through
        // the sRGB decode a second time and produced a forest floor four times
        // too dark to read.
        set(mat::ROCK, 0.42f, 0.41f, 0.39f, 0.88f);
        set(mat::DIRT, 0.29f, 0.22f, 0.15f, 0.95f);
        set(mat::MOSS, 0.24f, 0.34f, 0.16f, 0.92f);
        set(mat::SAND, 0.68f, 0.61f, 0.45f, 0.85f);
        set(mat::SILT, 0.22f, 0.20f, 0.16f, 0.95f);
        set(mat::NEEDLE_LITTER, 0.20f, 0.14f, 0.09f, 0.97f);
    }

    std::vector<MaterialLook> look_ = std::vector<MaterialLook>(mat::COUNT);
    std::map<uint32_t, uint8_t> index_;
    uint8_t next_ = mat::TREE_BASE;
    int overflow_ = 0;
};

inline bool isGrass(uint8_t m) { return m >= mat::GRASS_0 && m < mat::GRASS_0 + mat::GRASS_COUNT; }
inline bool isSoil(uint8_t m) { return m >= mat::SOIL_0 && m < mat::SOIL_0 + mat::SOIL_COUNT; }

// ---------------------------------------------------------------------------
// Face directions. The index is stored per triangle and turned back into a
// normal by a six-entry table on the device.
// ---------------------------------------------------------------------------
namespace face {
constexpr uint8_t POS_Y = 0, NEG_Y = 1, POS_X = 2, NEG_X = 3, POS_Z = 4, NEG_Z = 5;
}

// Kept next to the constants above so the two cannot drift apart. Indexed by a
// face:: value; used on the device to expand a stored direction into a normal.
V2_FN Vec3 faceNormal(uint8_t dir) {
    switch (dir) {
        case face::POS_Y: return Vec3(0.0f, 1.0f, 0.0f);
        case face::NEG_Y: return Vec3(0.0f, -1.0f, 0.0f);
        case face::POS_X: return Vec3(1.0f, 0.0f, 0.0f);
        case face::NEG_X: return Vec3(-1.0f, 0.0f, 0.0f);
        case face::POS_Z: return Vec3(0.0f, 0.0f, 1.0f);
        default:          return Vec3(0.0f, 0.0f, -1.0f);
    }
}

// Material in the low byte, face direction in the high byte.
V2_FN uint16_t packTri(uint8_t material, uint8_t dir) {
    return uint16_t(material) | (uint16_t(dir) << 8);
}
V2_FN uint8_t triMaterial(uint16_t p) { return uint8_t(p & 0xFFu); }
V2_FN uint8_t triFace(uint16_t p) { return uint8_t(p >> 8); }

// ---------------------------------------------------------------------------
// A meshed voxel surface: quads, plus what each triangle is and which way it
// faces.
// ---------------------------------------------------------------------------
struct VoxMesh {
    std::vector<Vec3> position;
    std::vector<uint32_t> index;
    std::vector<uint16_t> tri;  // packTri(material, face), one per triangle

    size_t triCount() const { return index.size() / 3; }

    void addQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint8_t m, uint8_t dir) {
        const uint32_t base = uint32_t(position.size());
        position.push_back(a);
        position.push_back(b);
        position.push_back(c);
        position.push_back(d);
        index.insert(index.end(), {base, base + 1, base + 2});
        index.insert(index.end(), {base, base + 2, base + 3});
        const uint16_t p = packTri(m, dir);
        tri.push_back(p);
        tri.push_back(p);
    }
};

// ---------------------------------------------------------------------------
// Face extraction for a dense grid -- used for the pine models.
//
// The six directions are walked separately and a face is emitted only where the
// neighbour is empty. For a conifer that is most of them: the canopy is nearly
// all surface, which is why a tree of 30k voxels still costs a few hundred
// thousand triangles.
// ---------------------------------------------------------------------------
inline VoxMesh meshAsset(const VoxAsset &a, const std::vector<uint8_t> &idOfEntry, float scale) {
    VoxMesh m;
    const float s = scale;

    auto solid = [&](int x, int y, int z) -> bool {
        const uint8_t v = a.at(x, y, z);
        return v != 0 && idOfEntry[v] != mat::AIR;
    };

    for (int y = 0; y < a.sy; ++y)
        for (int z = 0; z < a.sz; ++z)
            for (int x = 0; x < a.sx; ++x) {
                const uint8_t v = a.at(x, y, z);
                if (!v) continue;
                const uint8_t id = idOfEntry[v];
                if (id == mat::AIR) continue;

                const float x0 = float(x) * s, x1 = x0 + s;
                const float y0 = float(y) * s, y1 = y0 + s;
                const float z0 = float(z) * s, z1 = z0 + s;

                // Wound counter-clockwise seen from outside, so the winding and
                // the stored direction agree about which way is out.
                if (!solid(x, y + 1, z))
                    m.addQuad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, id, face::POS_Y);
                if (!solid(x, y - 1, z))
                    m.addQuad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, id, face::NEG_Y);
                if (!solid(x + 1, y, z))
                    m.addQuad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, id, face::POS_X);
                if (!solid(x - 1, y, z))
                    m.addQuad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, id, face::NEG_X);
                if (!solid(x, y, z + 1))
                    m.addQuad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, id, face::POS_Z);
                if (!solid(x, y, z - 1))
                    m.addQuad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, id, face::NEG_Z);
            }
    return m;
}

// ---------------------------------------------------------------------------
// The terrain
//
// A heightfield, so the world is a pure function of (i, j) and never has to be
// stored: one integer height and one surface material per column.
// ---------------------------------------------------------------------------
// One chunk is this many voxel columns on a side. 256 columns is 25.6 m, which
// is the balance the numbers actually push you to: big enough that a chunk's
// build amortises the fixed cost of an acceleration structure, small enough
// that meshing one is a few tens of milliseconds and the ring around the camera
// can be extended a chunk at a time without a visible hitch.
constexpr int CHUNK_VOX = 256;
constexpr float CHUNK_M = float(CHUNK_VOX) * VOXEL_M;

// Floor division and modulo that stay correct at negative coordinates -- the
// world runs in both directions from the origin, and C's truncating / and %
// fold the negative side onto the positive one, which puts a seam through 0.
inline int floorDiv(int a, int b) { return (a >= 0) ? a / b : -(((-a) + b - 1) / b); }
inline int floorMod(int a, int b) { const int m = a % b; return m < 0 ? m + b : m; }

class VoxelTerrain {
  public:
    float waterLevel = 2.6f;  // metres

    // WORLD COLUMN INDICES, not patch-relative ones.
    //
    // The terrain used to live inside a fixed patch centred on the origin, so
    // every lookup was offset by halfSize. Endless terrain has no centre and no
    // edge: column I simply sits at I * VOXEL_M, for any I in either direction,
    // and a chunk is a range of those. Removing the offset is most of what made
    // the height field chunkable at all -- it was already a pure function of
    // position, it just had a patch bolted around it.
    float wx(int i) const { return float(i) * VOXEL_M; }

    // -----------------------------------------------------------------------
    // The continuous landform, before quantisation.
    // -----------------------------------------------------------------------
    // ROUNDED, AND TWICE AS TALL.
    //
    // The old field was dominated by a ridged multifractal, and a ridged
    // multifractal is *for* creasing -- 1 - |2n-1| is a fold by construction,
    // which is the opposite of rounded. The dominant term is now a low-octave
    // warped fbm, which gives broad domes, plus an even lower-frequency swell
    // underneath it for the large forms. The ridge survives at a fifth of its
    // old weight purely so the landscape is not all one shape.
    //
    // The two fine octaves that used to sit on top are down to one at half the
    // amplitude. At 10 cm voxels those were quantising into single-voxel
    // stipple, which reads as gravel rather than as ground and cost a side quad
    // on nearly every column to draw.
    float heightM(float x, float z) const {
        const float roll = warpedFbm(x * 0.0130f, z * 0.0130f, 1.5f, 5);
        const float swell = fbm(x * 0.0070f + 71.3f, z * 0.0070f + 29.7f, 3);
        const float ridge = ridged(x * 0.0300f + 13.1f, z * 0.0300f + 7.3f, 3);

        float h = 2.0f + roll * 30.0f + swell * 12.0f + ridge * 3.0f;

        const float b = fbm(x * 0.0160f + 311.7f, z * 0.0160f + 157.3f, 4);
        if (b < 0.40f) {
            const float m = sstep(minf(1.0f, (0.40f - b) / 0.10f));
            // Scaled with the terrain. At twice the height the old 9 m gate sat
            // below almost every column and no basin ever cut, which would have
            // left the water plane buried under the whole patch.
            const float lowGate = saturate((18.0f - h) / 14.0f);
            h -= m * lowGate * (h - (waterLevel - 3.2f));
        }

        // AND THE FINE OCTAVE HAS TO STAY. Cutting it entirely was a mistake
        // the first pass made: at 10 cm voxels a slope quantises into steps
        // whose WIDTH is the voxel size over the gradient, so a field that is
        // smooth everywhere terraces into wide flat plateaus -- which reads as
        // worse, not rounder. Roundness belongs in the large shapes; the small
        // ones have to keep enough gradient to break the steps up.
        h += (fbm(x * 0.090f + 3.7f, z * 0.090f + 9.1f, 3) - 0.5f) * 1.2f;
        return h;
    }

    // Column height in VOXELS -- the one place the world is quantised.
    int heightVox(int i, int j) const {
        const float h = heightM(wx(i), wx(j));
        return int(floorf(h / VOXEL_M));
    }

    // Slope, in voxels of drop across two columns. Both thresholds moved up
    // with the doubled height: the terrain is twice as tall, so the same
    // hillside now measures twice the drop and the old numbers would have
    // turned most of the patch to bare rock and refused to plant on it.
    static constexpr int kRockSlope = 18;
    static constexpr int kTreeSlope = 15;

    // How much of the grass carries a strand, and how many of those flower.
    //
    // DENSITY IS WHAT MAKES IT READ AS GRASS. A strand is one voxel across
    // because that is the smallest thing the lattice can express, so at a tenth
    // coverage they stand isolated and every one reads as a fence post. Grass
    // is a MASS -- it only looks like grass once the blades are close enough to
    // occlude each other, and that means most of a grass column carrying one.
    // The fraction of GRASS-TOPPED COLUMNS that grow a blade -- not how much of
    // the ground is grass, which is the fbm threshold in topMaterial.
    //
    // Halved three times from the 0.85 that first made it read as a sward. At
    // this density the blades no longer close into a mass, which is the point:
    // the ground colour shows between them and they read as scattered tufts on
    // grass rather than as the grass itself.
    float grassDensity = 0.105f;
    float flowerChance = 0.035f;
    int grassMinRows = 3, grassMaxRows = 6;
    uint32_t strandSeed = 20260904u;

    float standDensity(float x, float z) const {
        return fbm(x * 0.0165f + 71.3f, z * 0.0165f + 44.1f, 3);
    }

    // -----------------------------------------------------------------------
    // Which material shows on top of a column.
    //
    // THE SURFACE IS A BAND, NOT A SKIN: a single coloured top voxel reads as
    // paint on stone the moment the camera nears a slope, because a steep
    // column shows its SIDE rather than its top. The soil band underneath is
    // what makes a cut bank look like earth.
    // -----------------------------------------------------------------------
    // THE SLOPE IS PASSED IN, not measured here, and that is the single
    // largest saving in the whole generator. heightM is roughly twenty-three
    // octaves of value noise; differencing the four neighbours to get a slope
    // therefore costs five height evaluations per column instead of one. The
    // mesher has already computed every one of those heights into a grid, so
    // handing the slope over turns 5x the noise work into 1x.
    //
    // The convenience overload below keeps the old signature for the scatter
    // code, which asks about a few thousand scattered columns rather than every
    // column in a chunk and has no grid to read from.
    uint8_t topMaterial(int i, int j, int h, int slope) const {
        const int wl = int(waterLevel / VOXEL_M);
        if (h <= wl) return (wl - h <= 8) ? mat::SAND : mat::SILT;
        if (h <= wl + 8) return mat::SAND;  // the shore band

        if (slope >= kRockSlope) return mat::ROCK;  // too steep to hold soil

        const float x = wx(i), z = wx(j);
        // Litter follows the canopy, and the canopy follows the same density
        // field the trees are planted from -- so the floor browns where the
        // stand is thick, without storing a mask.
        if (standDensity(x, z) > 0.44f && fbm(x * 3.1f + 63.0f, z * 3.1f + 88.0f, 2) > 0.36f)
            return mat::NEEDLE_LITTER;

        // WHICH grass or WHICH soil comes from a field of its own, at a few
        // metres across. Picking per column from a hash would give confetti --
        // the variants have to form patches or they average back to the single
        // flat colour they were brought in to replace.
        const float patch = fbm(x * 0.30f + 117.3f, z * 0.30f + 241.1f, 3);
        if (fbm(x * 0.55f + 31.7f, z * 0.55f + 17.2f, 4) > 0.46f) {
            const int k = mini(int(mat::GRASS_COUNT) - 1, int(patch * float(mat::GRASS_COUNT)));
            return uint8_t(mat::GRASS_0 + k);
        }
        const int k = mini(int(mat::SOIL_COUNT) - 1, int(patch * float(mat::SOIL_COUNT)));
        return uint8_t(mat::SOIL_0 + k);
    }

    uint8_t topMaterial(int i, int j, int h) const {
        // Only reached from the sparse scatter paths. Below the shore band the
        // slope is never consulted, so it is not worth four height evaluations
        // to compute one that will be discarded.
        const int wl = int(waterLevel / VOXEL_M);
        if (h <= wl + 8) return topMaterial(i, j, h, 0);
        const int slope = maxi(absi(heightVox(i + 1, j) - heightVox(i - 1, j)),
                               absi(heightVox(i, j + 1) - heightVox(i, j - 1)));
        return topMaterial(i, j, h, slope);
    }

    // -----------------------------------------------------------------------
    // Meshing the patch
    //
    // Only the skin: one top quad per column, and on each of the four sides a
    // quad spanning the drop to the neighbour -- split at the material bands so
    // a cut bank still shows soil over rock. Vertical runs are merged, so a
    // ten-voxel drop costs one quad per band, not ten.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // One chunk, meshed.
    //
    // The grid is padded by one column on every side so a face on the chunk
    // boundary can ask its neighbour how tall it is. Without the pad, every
    // chunk edge would emit the full side of its own columns and the seams
    // would show as walls -- and because both chunks would do it, the geometry
    // would be doubled there too.
    // -----------------------------------------------------------------------
    VoxMesh meshChunk(int cx, int cz) const {
        VoxMesh m;
        const int n = CHUNK_VOX;
        // Measured at roughly 1.5 quads per column across this terrain; two is
        // a comfortable margin. Growing these by doubling instead copies tens of
        // megabytes per chunk, which is meshing time spent on memcpy.
        m.position.reserve(size_t(n) * n * 8);
        m.index.reserve(size_t(n) * n * 12);
        m.tri.reserve(size_t(n) * n * 4);
        const int I0 = cx * CHUNK_VOX, J0 = cz * CHUNK_VOX;
        const float s = VOXEL_M;

        // TWO rings of padding on the heights, one on everything else. The
        // material at a column one outside the chunk needs that column's slope,
        // and a slope reaches one further again -- so the heights have to go out
        // to two while the materials only go out to one.
        std::vector<int> h((size_t(n) + 4) * (size_t(n) + 4));
        auto H = [&](int i, int j) -> int & { return h[size_t(j + 2) * (n + 4) + size_t(i + 2)]; };
        for (int j = -2; j <= n + 1; ++j)
            for (int i = -2; i <= n + 1; ++i) H(i, j) = heightVox(I0 + i, J0 + j);

        std::vector<uint8_t> top((size_t(n) + 2) * (size_t(n) + 2));
        auto T = [&](int i, int j) -> uint8_t & {
            return top[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        for (int j = -1; j <= n; ++j)
            for (int i = -1; i <= n; ++i) {
                const int slope =
                    maxi(absi(H(i + 1, j) - H(i - 1, j)), absi(H(i, j + 1) - H(i, j - 1)));
                T(i, j) = topMaterial(I0 + i, J0 + j, H(i, j), slope);
            }

        // How tall a strand stands on each column, 0 for none. Computed for the
        // padded grid so a column on the edge can still ask its neighbours.
        std::vector<uint8_t> sr((size_t(n) + 2) * (size_t(n) + 2), 0);
        auto SR = [&](int i, int j) -> uint8_t & {
            return sr[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        for (int j = -1; j <= n; ++j)
            for (int i = -1; i <= n; ++i) {
                if (!isGrass(T(i, j))) continue;
                // Hashed on the WORLD column, so a strand is in the same place
                // no matter which chunk happens to be meshing it -- otherwise
                // the grass would reshuffle every time a chunk was rebuilt.
                const uint32_t cell = hashU32(uint32_t(I0 + i), uint32_t(J0 + j));
                if (hashUnit(strandSeed, cell) >= grassDensity) continue;
                const int span = maxi(1, grassMaxRows - grassMinRows + 1);
                SR(i, j) = uint8_t(grassMinRows +
                                   mini(span - 1, int(hashUnit(strandSeed + 1u, cell) * span)));
            }

        // A side quad from voxel row lo up to row hi (exclusive), in one band,
        // spanning `run` columns along the wall's own axis.
        //
        // The run is what makes this worth doing. A wall was previously one
        // quad per column per band, so a fifty-metre bank of uniform height
        // emitted five hundred separate quads describing one flat rectangle.
        auto sideBand = [&](VoxMesh &out, int i, int j, int dir, int lo, int hi, uint8_t mtl,
                            int run) {
            if (hi <= lo) return;
            const bool alongZ = (dir == 0 || dir == 1);  // +/-X walls extend in z
            const float x0 = float(I0 + i) * s, x1 = x0 + (alongZ ? s : float(run) * s);
            const float z0 = float(J0 + j) * s, z1 = z0 + (alongZ ? float(run) * s : s);
            const float y0 = float(lo) * s, y1 = float(hi) * s;
            switch (dir) {
                case 0:
                    out.addQuad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, mtl,
                                face::POS_X);
                    break;
                case 1:
                    out.addQuad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, mtl,
                                face::NEG_X);
                    break;
                case 2:
                    out.addQuad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, mtl,
                                face::POS_Z);
                    break;
                case 3:
                    out.addQuad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, mtl,
                                face::NEG_Z);
                    break;
            }
        };

        // -------------------------------------------------------------------
        // A grass strand: a 1x1 column of voxels standing on the surface.
        //
        // NEIGHBOUR-AWARE, and that is what turns it from a field of fence
        // posts into grass. At the density it takes for a sward to read as a
        // sward, most strands are touching -- and a strand that emits all four
        // of its sides regardless is drawing the faces buried inside its
        // neighbours. That is not merely wasted geometry (it was about half of
        // it): those interior faces are what make a dense patch read as a
        // bundle of separate posts instead of one continuous mass, because
        // every blade keeps its own hard silhouette.
        //
        // So each side is emitted only over the rows the neighbour does NOT
        // cover. The uncovered part is at most two intervals -- above the
        // neighbour and below it -- which is why this takes a span rather than
        // a flag.
        //
        // No bottom face either. It is standing on the ground.
        // -------------------------------------------------------------------
        auto sideQuad = [&](VoxMesh &out, int i, int j, int dir, int lo, int hi, uint8_t mtl) {
            if (hi <= lo) return;
            const float x0 = float(I0 + i) * s, x1 = x0 + s;
            const float z0 = float(J0 + j) * s, z1 = z0 + s;
            const float y0 = float(lo) * s, y1 = float(hi) * s;
            switch (dir) {
                case 0: out.addQuad({x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}, mtl, face::POS_X); break;
                case 1: out.addQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, mtl, face::NEG_X); break;
                case 2: out.addQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, mtl, face::POS_Z); break;
                default:out.addQuad({x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}, mtl, face::NEG_Z); break;
            }
        };

        // The part of [lo,hi) that [nlo,nhi) does not cover, as up to two runs.
        auto emitUncovered = [&](VoxMesh &out, int i, int j, int dir, int lo, int hi, int nlo,
                                 int nhi, uint8_t mtl) {
            if (nhi <= nlo) { sideQuad(out, i, j, dir, lo, hi, mtl); return; }
            sideQuad(out, i, j, dir, lo, mini(hi, nlo), mtl);
            sideQuad(out, i, j, dir, maxi(lo, nhi), hi, mtl);
        };

        // -------------------------------------------------------------------
        // TOP FACES, MERGED ALONG X.
        //
        // One quad per column is the obvious way to do this and it is what the
        // engine did: 65 536 quads per chunk whatever the ground looked like.
        // But a top face only needs to be its own quad where something CHANGES
        // -- a step in height or a change of material. Everywhere else a run of
        // columns is one flat rectangle, and the rounder the terrain got the
        // longer those runs became.
        //
        // Merged only along X, not into rectangles. Full 2D greedy meshing
        // would do better again, but it needs a visited mask and a second pass,
        // and one dimension already takes most of what there is to take.
        // -------------------------------------------------------------------
        for (int j = 0; j < n; ++j) {
            int i = 0;
            while (i < n) {
                const int hc = H(i, j);
                const uint8_t tm = T(i, j);
                int k = i + 1;
                while (k < n && H(k, j) == hc && T(k, j) == tm) ++k;

                const float x0 = float(I0 + i) * s, x1 = float(I0 + k) * s;
                const float z0 = float(J0 + j) * s, z1 = z0 + s;
                const float yTop = float(hc + 1) * s;
                m.addQuad({x0, yTop, z0}, {x0, yTop, z1}, {x1, yTop, z1}, {x1, yTop, z0}, tm,
                          face::POS_Y);
                i = k;
            }
        }

        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int hc = H(i, j);
                const uint8_t tm = T(i, j);
                const float x0 = float(I0 + i) * s, x1 = x0 + s;
                const float z0 = float(J0 + j) * s, z1 = z0 + s;

                // Strands and flowers. Three to six voxels is 30-60 cm --
                // knee height beside a 22 m pine, which is what keeps it
                // reading as grass rather than as a hedge.
                const int rows = SR(i, j);
                if (rows > 0) {
                    // The cap used to be a coloured voxel standing in for a
                    // flower. Real models are instanced on the ground now, so a
                    // strand is just a strand.
                    const uint8_t cap = tm;
                    const int lo = hc + 1, hi = lo + rows;
                    static const int di[4] = {1, -1, 0, 0};
                    static const int dj[4] = {0, 0, 1, -1};
                    for (int d = 0; d < 4; ++d) {
                        const int ni = i + di[d], nj = j + dj[d];
                        const int nr = SR(ni, nj);
                        // The neighbour's solid span is its strand if it has
                        // one, and in either case the ground it stands on --
                        // which also hides anything at or below its own top.
                        const int nlo = (nr > 0) ? H(ni, nj) + 1 : hi;
                        const int nhi = (nr > 0) ? nlo + nr : hi;
                        const int ground = H(ni, nj) + 1;
                        // Below the neighbour's surface is buried in terrain.
                        emitUncovered(m, i, j, d, maxi(lo, ground), hi, nlo, nhi, tm);
                    }

                    const float yt = float(hi) * s;
                    m.addQuad({x0, yt, z0}, {x0, yt, z1}, {x1, yt, z1}, {x1, yt, z0}, cap,
                              face::POS_Y);
                }

            }
        }

        // -------------------------------------------------------------------
        // WALLS, MERGED ALONG THEIR OWN AXIS.
        //
        // Each of the four horizontal directions is walked separately, and for
        // each one the run extends along the axis the wall lies in: an east-
        // facing wall runs north-south, so it merges along z. A run continues
        // while the column height, the neighbour's height and the surface
        // material all hold, because those three are exactly what decide where
        // the material bands split -- if any changes, the quads below would
        // differ and the run has to end.
        //
        // The band structure inside a run is unchanged: the surface voxel, up
        // to three of soil, then rock to the neighbour's level.
        // -------------------------------------------------------------------
        static const int kDi[4] = {1, -1, 0, 0};
        static const int kDj[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int di = kDi[d], dj = kDj[d];
            const bool alongZ = (d == 0 || d == 1);

            for (int outer = 0; outer < n; ++outer) {
                int inner = 0;
                while (inner < n) {
                    const int i = alongZ ? outer : inner;
                    const int j = alongZ ? inner : outer;
                    const int hc = H(i, j);
                    const int nb = H(i + di, j + dj);
                    if (hc - nb <= 0) { ++inner; continue; }
                    const uint8_t tm = T(i, j);

                    int k = inner + 1;
                    while (k < n) {
                        const int i2 = alongZ ? outer : k;
                        const int j2 = alongZ ? k : outer;
                        if (H(i2, j2) != hc || H(i2 + di, j2 + dj) != nb || T(i2, j2) != tm) break;
                        ++k;
                    }
                    const int run = k - inner;

                    int cursor = hc + 1;
                    const int surfLo = maxi(nb + 1, hc);
                    sideBand(m, i, j, d, surfLo, cursor, tm, run);
                    cursor = surfLo;
                    if (cursor > nb + 1) {
                        if (tm != mat::ROCK) {
                            const int soilLo = maxi(nb + 1, hc - 3);
                            sideBand(m, i, j, d, soilLo, cursor, mat::SOIL_0 + 1, run);
                            cursor = soilLo;
                        }
                        if (cursor > nb + 1)
                            sideBand(m, i, j, d, nb + 1, cursor, mat::ROCK, run);
                    }
                    inner = k;
                }
            }
        }

        return m;
    }

    // The water surface used to be built here, sized to the patch. With no
    // patch there is no size to give it, so GpuScene::buildWater makes one quad
    // larger than any ring will reach -- see scene_gpu.h.
};

}  // namespace v2
