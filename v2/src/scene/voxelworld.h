// ---------------------------------------------------------------------------
// voxelworld.h -- the world as 10 cm voxels, meshed into faces for the RT cores.
//
// The terrain is quantised to the same 10 cm grid the pine models are authored
// on, so a trunk sits in ground made of the lattice the trunk is made of. A
// smooth landscape under a voxel tree reads as two different games.
//
// WHY FACES AND NOT BOXES: a voxel column stack could go into the acceleration
// structure as one procedural AABB per column, which is far less memory. It
// would also be far slower. Procedural primitives are intersected by a shader
// the SM runs, and under inline ray tracing that shader is a loop the caller
// has to run itself, which is worse again. Triangles are intersected by the RT
// cores in fixed function. On a 4070 that is most of an order of magnitude on a
// scene traced tens of millions of times a frame. So
// the surface is extracted as quads -- only faces with nothing in front of
// them. Interior voxels never become geometry at all, which is what keeps a
// 1.5-million-column patch tractable.
//
// THE FACE DIRECTION IS STORED, NOT RECONSTRUCTED, and under DXR that decision
// pays for itself twice over. The Embree engine had a geometric normal handed
// to it per hit and never stored one; OptiX could do the same, but only on a
// structure built with random vertex access, which costs memory on every
// acceleration structure in the scene to recompute a cross product for a face
// that was axis-aligned when it was emitted and is axis-aligned still.
//
// Inline ray tracing sharpens the argument. A RayQuery hands back an instance
// id, a primitive index and the barycentrics, and NOTHING ELSE -- there is no
// vertex data to ask for at all without going back to the index and vertex
// buffers by hand. Storing the direction the face was emitted in costs one byte
// per triangle, makes the normal exact by construction, and means the shader
// never reads a position or an index: the vertex buffers exist purely to build
// the acceleration structure and are never bound to anything afterwards.
//
// It is packed with the material into a single uint16 so the hit path touches
// one array, not two.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_set>
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
constexpr uint8_t UNUSED_6 = 6;  // was NEEDLE_LITTER, now a ramp of its own below

// GROUND COLOURS BORROWED FROM THE TREES.
//
// A single flat green for grass and a single flat brown for soil is what made
// the floor read as a painted plane under a detailed canopy: the eye finds the
// repeat instantly when a whole hillside is one value. These slots are filled
// AFTER the pines load, from the greens and browns the models actually use, so
// the ground is made of the same palette as the things standing in it -- which
// is the cheapest possible way to make a scene look like it belongs together.
// THREE RAMPS, and the mesher only ever names the bottom of one.
//
// Which SHADE a voxel takes is chosen on the device, from a hash of the voxel
// itself -- see groundShade() in Trace.cs.slang. That is why the counts live
// here and in Shared.slang both, and why the terrain no longer carries a field
// for picking between them.
constexpr uint8_t GRASS_0 = 7;
// SIX greens, sampled across the pines' own foliage entries, so the number is
// how much of the trees' range the floor gets to show.
constexpr uint8_t GRASS_COUNT = 6;   // 7..12
constexpr uint8_t SOIL_0 = 13;
// Shades of ONE brown -- see deriveGroundFromTrees.
constexpr uint8_t SOIL_COUNT = 4;    // 13..16
// The dropped-needle floor a conifer stand builds: the same brown again, at the
// dark end of it, so the canopy still browns the ground beneath it.
constexpr uint8_t LITTER_0 = 17;
constexpr uint8_t LITTER_COUNT = 3;  // 17..19
constexpr uint8_t TREE_BASE = 20;  // model palette entries are allocated from here up
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

    // `conifer` is whether the green-dominant rule below applies to this model
    // at all, and it is true for everything the WOOD is made of -- which is
    // everything that calls this, bar one.
    //
    // A BUTTERFLY'S WING IS NOT A NEEDLE, and the lime one is the reason this
    // argument exists. The rule reads a green as foliage and gives it a
    // needle's translucency and a needle's 1.7x lift, and both are wrong twice
    // over on a wing: the JS engine was told in as many words to stop making
    // these translucent ("in the pine forest the butterflys wings seem to be
    // transparent, revert that change, they should be solid", 2026-08-16), and
    // the lift is calibrated for an authored olive that is nearly black once
    // linearised -- applied to a saturated lime it puts the albedo past one,
    // which is a surface that returns more light than reaches it.
    uint8_t forModelColor(const std::array<uint8_t, 4> &c, bool conifer = true) {
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
        const bool foliage = conifer && (c[1] > c[0] && c[1] > c[2]);
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

            // A FLOOR UNDER THE BLUE, and it is the difference between a
            // shaded canopy and a black one.
            //
            // These greens arrive from a MagicaVoxel palette, and an artist
            // picking a green picks a SATURATED one: the nine pines carry
            // needles with blue between 0.006 and 0.021. A real conifer needle
            // reflects about 0.04 in blue -- vegetation is never that pure.
            //
            // On a sunlit needle it makes no difference worth seeing. On a
            // SHADED one it is most of the picture, because the only light
            // reaching the underside of a crown is skylight, and skylight is
            // half blue by irradiance -- measured here as (3.5, 5.1, 8.8) on a
            // flat patch, the blue channel being the largest of the three. A
            // needle reflecting 0.6% of it has nothing to give back, so the
            // whole underside of the wood went to a flat dark grey-green that
            // no exposure or tone curve could recover, because the light really
            // was being absorbed.
            //
            // With the floor, what a needle returns of SKYLIGHT rises by 5 to
            // 23% -- most for the darkest needles, which are the ones that were
            // black -- while what it returns of SUNLIGHT rises 2 to 9%. It
            // lifts the shadows and leaves the highlights, which is the shape
            // the problem has.
            m.albedo.z = maxf(m.albedo.z, 0.038f);
        } else {
            m.roughness = 0.88f;  // bark is coarse
            m.specular = 0.020f;
            m.translucency = 0.0f;
        }
        index_.emplace(key, id);
        return id;
    }

    // Where the pines stopped and the rocks began.
    //
    // deriveGroundFromTrees ran over the WHOLE palette, which by the time it is
    // called also holds twenty-six rocks and six flowers. Rock grey passes a
    // "warm and unsaturated" test comfortably -- 0.048 0.047 0.041 is warm by
    // the letter of it -- so stone was being sampled as soil, and the ground
    // came out in patches of brown and patches of grey. Recording the boundary
    // is what makes the function's name true.
    void markPinesLoaded() { pineEnd_ = next_; }

    const MaterialLook &operator[](uint8_t id) const { return look_[id]; }
    const std::vector<MaterialLook> &table() const { return look_; }
    int used() const { return next_; }
    int overflowed() const { return overflow_; }
    // How many model entries have been handed out. Read by World::replaceHeldVox
    // to tell "this model brought a new colour" from "it brought the same ones
    // it did last time", which decides whether the GPU's copy of the table is
    // stale at all.
    int minted() const { return next_; }

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
        const int end = pineEnd_ > mat::TREE_BASE ? pineEnd_ : next_;
        for (int i = mat::TREE_BASE; i < end; ++i) {
            if (look_[i].translucency > 0.0f) { foliage.push_back(uint8_t(i)); continue; }

            // NOT EVERY NON-GREEN ENTRY IS BARK. forModelColor classifies by
            // green dominance, so "bark" is really "everything else" -- and a
            // pine's palette carries reds and near-whites for cut ends and
            // highlights. Sampling those as soil painted scarlet and chalk-white
            // patches across whole hillsides, which is exactly what showed up.
            //
            // Soil has to be warm, mid-dark and BROWN. The old test asked for
            // warm and unsaturated, and a grey satisfies both: r >= g >= b is
            // true of almost any near-neutral, and a near-neutral is by
            // definition unsaturated. So a saturation FLOOR does most of the
            // work here -- it is the line between a brown and a stone.
            const Vec3 c = look_[i].albedo;
            const float lum = luminance(c);
            const bool warm = c.x >= c.y && c.y >= c.z && c.x > c.z * 1.5f;
            const float sat = maxComp(c) > 0.0f ? (maxComp(c) - minf(c.x, minf(c.y, c.z))) /
                                                      maxComp(c)
                                                : 0.0f;
            if (warm && lum > 0.015f && lum < 0.30f && sat > 0.35f && sat < 0.80f)
                bark.push_back(uint8_t(i));
        }

        // SORTED INTO A RAMP, then read ALONG it rather than sampled from it.
        //
        // The nine pines share a palette and it holds only five greens, so
        // picking a nearest entry per slot handed two slots the same colour --
        // six materials, five of them distinct, and the duplicate did nothing
        // but cost a patch boundary with no colour change across it. Reading
        // between the entries gives six greens that are all still the pines'
        // own, because every one of them is on the line between two needles.
        //
        // Sorted by luminance first: the palette's order is the artist's, and
        // it is not monotonic, so interpolating along it unsorted would walk
        // back and forth across the ramp instead of up it.
        std::sort(foliage.begin(), foliage.end(), [this](uint8_t a, uint8_t b) {
            return luminance(look_[a].albedo) < luminance(look_[b].albedo);
        });

        for (int k = 0; k < mat::GRASS_COUNT; ++k) {
            MaterialLook &m = look_[mat::GRASS_0 + k];
            if (foliage.empty()) {
                // No models loaded: a plain green, so the world still renders.
                m.albedo = Vec3(0.14f + 0.04f * k, 0.26f + 0.05f * k, 0.10f + 0.02f * k);
            } else {
                const float t = (mat::GRASS_COUNT > 1)
                                    ? float(k) / float(int(mat::GRASS_COUNT) - 1) *
                                          float(foliage.size() - 1)
                                    : 0.0f;
                const size_t i0 = size_t(t);
                const size_t i1 = mini(int(foliage.size()) - 1, int(i0) + 1);
                const float f = t - float(i0);
                const Vec3 c = look_[foliage[i0]].albedo * (1.0f - f) +
                               look_[foliage[i1]].albedo * f;
                // THE SAME HUE AS THE NEEDLES, ONLY DARKER -- and the "only"
                // is the whole change. This used to scale the three channels
                // by 0.45, 0.44 and 0.32, which is a scale plus a shove
                // towards yellow, and a shove towards yellow is a DIFFERENT
                // COLOUR. The floor came out its own shade of olive standing
                // under trees that were green, which reads as two materials
                // that happen to be near each other rather than one wood.
                //
                // Scaling all three by the same number cannot change the hue
                // by construction: it is the pine's green at a lower value,
                // which is exactly what grass under conifers is.
                //
                // WHY IT IS SCALED AT ALL is lighting, not colour. The foliage
                // albedo is tuned for needles sitting inside their own crown,
                // in shadow most of the day; grass stands in the open taking
                // full sun, so the same albedo renders far brighter. Reading it
                // across literally is what once made the floor glow under a
                // dark canopy.
                m.albedo = c * 0.52f + Vec3(0.006f, 0.008f, 0.005f);
            }
            m.roughness = 0.88f;
            m.specular = 0.022f;
            // Thin blades, lit from behind at a low sun -- the same reason the
            // needles have it. Without it a strand is a black stick at dawn.
            // Kept well below the needles' 0.45: grass caught the sun from
            // every angle at that value and the whole floor glowed.
            m.translucency = 0.22f;
        }

        // -- ONE BROWN, IN SEVERAL SHADES ------------------------------------
        //
        // This used to take a DIFFERENT bark entry per soil slot, spread across
        // whatever browns the pines happened to carry. A pine's palette holds
        // several genuinely different browns -- a grey-brown for weathered bark,
        // a red-brown for the cut, a yellow-brown for lit sapwood -- so the
        // ground came out in patches of visibly different colours, which reads
        // as three soils rather than as soil.
        //
        // A TRUNK does not look like that, and it is the thing being copied: a
        // trunk is one hue at several values, a shading ramp. So one base is
        // chosen and the slots are that base scaled. A pure multiply cannot
        // move the hue, so the ramp is a brown by construction, and every patch
        // boundary is a change of light rather than a change of material.
        //
        // The base is the MEDIAN of the accepted barks by luminance, not the
        // mean: averaging several browns together gives a grey, because that is
        // what averaging colours does.
        Vec3 soilBase(0.26f, 0.19f, 0.13f);
        if (!bark.empty()) {
            std::vector<uint8_t> byLum = bark;
            std::sort(byLum.begin(), byLum.end(), [this](uint8_t a, uint8_t b) {
                return luminance(look_[a].albedo) < luminance(look_[b].albedo);
            });
            const Vec3 c = look_[byLum[byLum.size() / 2]].albedo;
            // Darker than the trunk it came from -- bark read literally makes
            // the ground look like decking.
            soilBase = c * 0.85f + Vec3(0.018f, 0.014f, 0.010f);
        }
        for (int k = 0; k < mat::SOIL_COUNT; ++k) {
            MaterialLook &m = look_[mat::SOIL_0 + k];
            // 0.74 to 1.22 across the slots, centred on the base. Wide enough
            // that the ground is not flat, narrow enough that no step reads as
            // a different material.
            const float f =
                0.74f + 0.48f * (float(k) / float(maxi(1, int(mat::SOIL_COUNT) - 1)));
            m.albedo = soilBase * f;
            m.roughness = 0.95f;
            m.specular = 0.020f;
            m.translucency = 0.0f;
        }

        // The needle floor under a thick stand: the same brown again, at the
        // dark end of it. A ramp rather than the single colour it used to be,
        // because a flat patch of one brown sitting in ground that is scattered
        // across six is the only thing on the floor that would still look
        // painted on.
        for (int k = 0; k < mat::LITTER_COUNT; ++k) {
            MaterialLook &m = look_[mat::LITTER_0 + k];
            const float f =
                0.58f + 0.30f * (float(k) / float(maxi(1, int(mat::LITTER_COUNT) - 1)));
            m.albedo = soilBase * f;
            m.roughness = 0.97f;
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

    }

    std::vector<MaterialLook> look_ = std::vector<MaterialLook>(mat::COUNT);
    std::map<uint32_t, uint8_t> index_;
    uint8_t next_ = mat::TREE_BASE;
    int pineEnd_ = 0;
    int overflow_ = 0;
};

inline bool isGrass(uint8_t m) { return m >= mat::GRASS_0 && m < mat::GRASS_0 + mat::GRASS_COUNT; }
inline bool isSoil(uint8_t m) { return m >= mat::SOIL_0 && m < mat::SOIL_0 + mat::SOIL_COUNT; }
inline bool isLitter(uint8_t m) {
    return m >= mat::LITTER_0 && m < mat::LITTER_0 + mat::LITTER_COUNT;
}

// ---------------------------------------------------------------------------
// Face directions. The index is stored per triangle and turned back into a
// normal by a six-entry table on the device.
// ---------------------------------------------------------------------------
namespace face {
constexpr uint8_t POS_Y = 0, NEG_Y = 1, POS_X = 2, NEG_X = 3, POS_Z = 4, NEG_Z = 5;
}

// Kept next to the constants above so the two cannot drift apart. Indexed by a
// face:: value; used on the device to expand a stored direction into a normal.
inline Vec3 faceNormal(uint8_t dir) {
    switch (dir) {
        case face::POS_Y: return Vec3(0.0f, 1.0f, 0.0f);
        case face::NEG_Y: return Vec3(0.0f, -1.0f, 0.0f);
        case face::POS_X: return Vec3(1.0f, 0.0f, 0.0f);
        case face::NEG_X: return Vec3(-1.0f, 0.0f, 0.0f);
        case face::POS_Z: return Vec3(0.0f, 0.0f, 1.0f);
        default:          return Vec3(0.0f, 0.0f, -1.0f);
    }
}

// ---------------------------------------------------------------------------
// THE PACKED TRIANGLE WORD.
//
//     bits  0..7   material
//     bits  8..10  face direction (0..5)
//     bits 11..14  strand code: 0 for everything that is not a grass blade,
//                  otherwise 1 + (the blade's base row & 7)
//     bit   15     spare
//
// THE STRAND CODE IS A GRADIENT THAT COSTS NO GEOMETRY, and that is the whole
// reason it is here rather than in the material byte.
//
// A blade wants to be dark where it meets the ground and light at its tip --
// six voxels, six greens. The obvious way to say that is to name a shade per
// row, and a material is per QUAD, so it means one quad per voxel instead of
// one quad per uncovered span. Measured over 49 chunks of this terrain that
// takes the strands from 51k triangles a chunk to 187k, and strands are
// already 46% of the terrain's geometry -- a 2.2x on the whole floor to change
// a colour.
//
// So the quads stay merged and the SHADE IS DRAWN PER VOXEL ON THE DEVICE, the
// same way the ground's own scatter already is: groundShade() knows the voxel
// the ray landed in, and the only thing it is missing is where that blade
// started. Three bits of the base row are enough to recover it -- a strand is
// at most eight voxels tall, so `(v.y - base) & 7` is its row above the soil --
// and the fourth bit is what distinguishes a blade from the ground it stands
// in, since a base row of zero is a real base row.
//
// The dir field is three bits wide now rather than a whole byte, so anything
// reading it MUST mask. See triFace here and faceNormal in Trace.cs.slang.
// ---------------------------------------------------------------------------
constexpr int TRI_DIR_SHIFT = 8, TRI_DIR_MASK = 0x7;
constexpr int TRI_STRAND_SHIFT = 11, TRI_STRAND_MASK = 0xF;
// A blade eight voxels tall is the most the three stored bits can tell apart.
constexpr int STRAND_MAX_ROWS = 8;

// The value the mesher stores for a blade standing on surface voxel `hc` --
// its base row is the one above. Never 0, which is what marks a face as a
// blade at all.
inline uint8_t strandCodeFor(int baseRow) { return uint8_t(1 + (baseRow & 7)); }

inline uint16_t packTri(uint8_t material, uint8_t dir, uint8_t strand = 0) {
    return uint16_t(material) | (uint16_t(dir) << TRI_DIR_SHIFT) |
           (uint16_t(strand) << TRI_STRAND_SHIFT);
}
inline uint8_t triMaterial(uint16_t p) { return uint8_t(p & 0xFFu); }
inline uint8_t triFace(uint16_t p) { return uint8_t((p >> TRI_DIR_SHIFT) & TRI_DIR_MASK); }
inline uint8_t triStrand(uint16_t p) {
    return uint8_t((p >> TRI_STRAND_SHIFT) & TRI_STRAND_MASK);
}

// ---------------------------------------------------------------------------
// A meshed voxel surface: quads, plus what each triangle is and which way it
// faces.
// ---------------------------------------------------------------------------
struct VoxMesh {
    std::vector<Vec3> position;
    std::vector<uint32_t> index;
    std::vector<uint16_t> tri;  // packTri(material, face, strand), one per triangle

    size_t triCount() const { return index.size() / 3; }

    void addQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint8_t m, uint8_t dir, uint8_t strand = 0) {
        const uint32_t base = uint32_t(position.size());
        position.push_back(a);
        position.push_back(b);
        position.push_back(c);
        position.push_back(d);
        index.insert(index.end(), {base, base + 1, base + 2});
        index.insert(index.end(), {base, base + 2, base + 3});
        const uint16_t p = packTri(m, dir, strand);
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
// MOSS ON THE ROCKS -- REAL VOXELS, written into the model before it is meshed.
//
// This has now been three things. First a material swap on the rock's own top
// face, which reads as a stain: the silhouette never changed, so from any angle
// where the top was foreshortened there was nothing to see. Then a slab of
// geometry standing 0.45 of a voxel proud, which looked right and was wrong in
// two ways -- it is not a voxel, in a world whose entire visual grammar is 10 cm
// cubes, and the COLLIDER NEVER KNEW ABOUT IT. Column heights are measured off
// the asset, so the moss was invisible to the feet and you stood inside it.
//
// The moss is now a voxel like every other voxel: written into the VoxAsset
// before meshAsset and columnTops ever see it, so the mesh, the collider and
// anything else added later all agree by construction rather than by being kept
// in step. There is no moss code in the mesher at all any more.
//
// PATCHY AT TWO SCALES. One hash per voxel is speckle, and speckle at 10 cm
// reads as noise rather than moss. The coarse term makes patches about half a
// metre across and the fine one breaks up their edges.
//
// The colour is GRASS_0 + k, the same ramp the ground grass is built from, so
// the moss is the wood's own green. Those materials need palette entries the
// model does not already use -- hence the search for free ones, and the quiet
// return if a model somehow uses all 255.
inline void growMoss(VoxAsset *a, std::vector<uint8_t> *idOfEntry, uint32_t seed) {
    if (!seed || a->sx <= 0) return;

    std::vector<bool> used(256, false);
    for (uint8_t v : a->a) used[v] = true;

    uint8_t tintEntry[mat::GRASS_COUNT];
    int tints = 0;
    for (int e = 1; e <= 255 && tints < int(mat::GRASS_COUNT); ++e)
        if (!used[e]) {
            tintEntry[tints] = uint8_t(e);
            (*idOfEntry)[e] = uint8_t(mat::GRASS_0 + tints);
            ++tints;
        }
    if (tints == 0) return;

    auto solid = [&](int x, int y, int z) -> bool {
        const uint8_t v = a->at(x, y, z);
        return v != 0 && (*idOfEntry)[v] != mat::AIR;
    };

    // Decided against the ORIGINAL model, then written. Growing moss as we go
    // would let a voxel just placed count as the rock under the next one, and
    // the moss would climb the boulder a layer per pass.
    struct Spot { int x, y, z; uint8_t e; };
    std::vector<Spot> spots;
    for (int y = 0; y < a->sy; ++y)
        for (int z = 0; z < a->sz; ++z)
            for (int x = 0; x < a->sx; ++x) {
                if (!solid(x, y, z) || solid(x, y + 1, z)) continue;
                const float coarse = hashUnit(uint32_t(x >> 2) * 73856093u ^ seed,
                                              uint32_t(z >> 2) ^ uint32_t(y >> 2) * 2654435761u);
                if (coarse > 0.72f) continue;
                const float fine = hashUnit(uint32_t(x) * 19349663u ^ seed,
                                            uint32_t(z) * 83492791u ^ uint32_t(y));
                if (fine > 0.82f) continue;
                const uint32_t t = hashU32(uint32_t(x) ^ seed,
                                           uint32_t(z) * 2654435761u ^ uint32_t(y));
                spots.push_back({x, y + 1, z, tintEntry[t % uint32_t(tints)]});
            }
    if (spots.empty()) return;

    // One more layer, because moss on the model's topmost voxel has nowhere to
    // go otherwise. y is the slowest axis, so the new layer is simply zeros on
    // the end of the vector.
    a->a.resize(size_t(a->sx) * size_t(a->sz) * size_t(a->sy + 1), 0);
    a->sy += 1;

    for (const Spot &sp : spots)
        a->a[size_t(sp.x) + size_t(sp.z) * size_t(a->sx) +
             size_t(sp.y) * size_t(a->sx) * size_t(a->sz)] = sp.e;
}


// THE TOP OF EVERY COLUMN IN A MODEL, in voxels above its base.
//
// This is what makes standing on a rock accurate. The collider it replaces was
// an elliptic cylinder with ONE height -- the model's highest voxel -- so a
// boulder was a flat-topped drum the size of its own bounding ellipse. You
// could stand on thin air a metre out from the stone, and the domed top you
// could see was not the surface you landed on.
//
// A column height per (x, z) is the actual surface, to the voxel. It is not
// the general solution -- an overhang has two surfaces in one column and this
// keeps the upper one -- but a boulder is a heightfield from above, which is
// the only direction a walking body meets it from.
//
// Zero means the column is empty, which is why the value stored is the top
// index PLUS ONE: a single voxel sitting on the base is 1, and its top surface
// is one voxel above the model's base.
inline std::vector<int16_t> columnTops(const VoxAsset &a,
                                       const std::vector<uint8_t> &idOfEntry) {
    std::vector<int16_t> t(size_t(a.sx) * size_t(a.sz), 0);
    for (int z = 0; z < a.sz; ++z)
        for (int x = 0; x < a.sx; ++x)
            for (int y = a.sy - 1; y >= 0; --y) {
                const uint8_t v = a.at(x, y, z);
                if (v != 0 && idOfEntry[v] != mat::AIR) {
                    t[size_t(x) + size_t(z) * size_t(a.sx)] = int16_t(y + 1);
                    break;
                }
            }
    return t;
}

// REVOXELISE A MODEL AT TWICE THE SIZE.
//
// Every voxel becomes a 2x2x2 block, so the result is a genuine voxel model at
// the SAME 10 cm grid as everything else -- twice as tall, twice as wide, and
// still made of cubes the size of every other cube in the world.
//
// NOT A SCALE FACTOR ON THE MESH, which is the tempting one-liner. meshAsset
// takes a scale and passing it 2 * VOXEL_M would produce a model twice as big
// out of voxels twice as big, which reads as the same mushroom seen closer up
// rather than as a bigger mushroom. It would also silently break every piece of
// placement arithmetic in makeInstance, all of which assumes a model's voxels
// are VOXEL_M across.
inline VoxAsset upscale2x(const VoxAsset &a) {
    VoxAsset o;
    o.sx = a.sx * 2;
    o.sy = a.sy * 2;
    o.sz = a.sz * 2;
    o.a.assign(size_t(o.sx) * size_t(o.sy) * size_t(o.sz), 0);
    for (int y = 0; y < o.sy; ++y)
        for (int z = 0; z < o.sz; ++z)
            for (int x = 0; x < o.sx; ++x)
                o.a[size_t(x) + size_t(z) * o.sx + size_t(y) * size_t(o.sx) * size_t(o.sz)] =
                    a.at(x / 2, y / 2, z / 2);
    return o;
}

// WHERE A PINECONE MAY SIT, in the pine model's own voxel coordinates.
struct Perch {
    int16_t x, y, z;
};

// Every spot in a pine where a cone can HANG: an empty voxel with solid wood
// directly ABOVE it.
//
// A cone is attached to the branch by its TOP -- it dangles. The first version
// tested the voxel BELOW and stood the cone on the branch like a bird, which
// prevents floating just as well and is the wrong way up.
//
// Testing the neighbour is still what makes floating impossible by
// construction. The alternative -- scatter cones through the crown and nudge
// them until something is hit -- has a tolerance in it, and a tolerance is a
// thing that is eventually wrong.
//
// ONLY THE CROWN, because below a third of the model's height a pine is bare
// trunk.
//
// AND ONLY WHERE IT CAN BE SEEN FROM BELOW, which is the condition that took a
// measurement to find. Nearly every voxel with wood above it is an interior gap
// sealed inside the crown by needles on every side; cones there are real,
// correct and completely invisible. Requiring the column BENEATH the perch to
// be clear to the bottom of the model leaves the undersides of the lowest
// branches -- which is exactly where you see cones from the ground.
// `clearW` widens the empty column the anchor needs, and it is the whole
// difference between an anchor for a CONE and an anchor for a BEEHIVE.
//
// A cone is one voxel across, so a one-voxel clear column is the right question
// and the default. A beehive is 5 x 5 x 5, and hung on a cone's anchor its
// SIDES sit in the leaves either side of that single clear column -- the model
// is technically unobstructed and visually buried, which is what the first
// birch render showed. Asking for the whole footprint to be clear is the same
// rule the browser engine's BIRCH_BANCH uses, and it costs a wider inner loop
// on a list built once per model at load.
inline std::vector<Perch> collectPerches(const VoxAsset &a,
                                         const std::vector<uint8_t> &idOfEntry,
                                         int clearW = 1) {
    std::vector<Perch> out;
    auto solid = [&](int x, int y, int z) -> bool {
        const uint8_t v = a.at(x, y, z);
        return v != 0 && idOfEntry[v] != mat::AIR;
    };
    const int r = (clearW - 1) / 2;
    const int y0 = a.sy / 3;
    for (int y = y0; y < a.sy; ++y)
        for (int z = 0; z < a.sz; ++z)
            for (int x = 0; x < a.sx; ++x) {
                if (solid(x, y, z) || !solid(x, y + 1, z)) continue;
                bool open = true;
                for (int yy = y - 1; yy >= 0 && open; --yy)
                    for (int dz = -r; dz <= r && open; ++dz)
                        for (int dx = -r; dx <= r && open; ++dx)
                            if (solid(x + dx, yy, z + dz)) open = false;
                if (open) out.push_back({int16_t(x), int16_t(y), int16_t(z)});
            }
    return out;
}

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
                    m.addQuad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, id,
                              face::POS_Y);
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

// ---------------------------------------------------------------------------
// One memo per noise CALL SITE in the terrain.
//
// Meshing walks the chunk in rows, and a row of 10 cm columns crosses a lattice
// cell of the coarsest octave about every eight hundred samples -- so the four
// corner hashes of every octave are, overwhelmingly, the same four numbers as
// last column. This is where they are kept. See NoiseCell in noise.h for why it
// is bit-exact and why that matters here in particular.
//
// One per site rather than one shared: a memo checks the cell before trusting
// it, so sharing would still be CORRECT, it would simply miss every time and
// pay a branch for the privilege.
// ---------------------------------------------------------------------------
struct TerrainMemo {
    FbmMemo warpX, warpZ, roll, swell, basin, fine;  // heightM, the pine's own
    // THE BIRCH'S ROLL AND SWELL ARE ITS OWN NOW, and only a column inside
    // the seam ever asks for them alongside the pine's -- see heightM. They
    // replace the ridge memo rather than adding to the struct, the ridged
    // octave having gone with it.
    FbmMemo birchRoll, birchSwell;
    FbmMemo stand, litter, grassMask;                       // topMaterial
};

// ---------------------------------------------------------------------------
// The working storage a chunk is meshed through -- ONE PER WORKER, not one per
// chunk.
//
// These three grids never leave meshChunk: they are filled, read by the quad
// loops, and dropped. Allocating them per chunk cost about 400 KB of malloc and
// free every time -- and, less obviously but worse, 400 KB of value
// initialisation that the fill loops immediately overwrote. A worker meshes
// thousands of chunks over a walk, so that is the same four hundred kilobytes
// zeroed and thrown away thousands of times to hold numbers that were about to
// be written anyway.
//
// Reusing them across chunks is safe for a reason worth stating: every element
// is written before it is read, on every chunk. The heights and materials are
// filled over their whole padded extent, and the strand rows now assign zero on
// the paths that used to `continue` -- so there is no stale value from the last
// chunk that anything can see. resize() is a no-op after the first call, which
// is where the zeroing went.
//
// THE MEMO COMES ALONG, and reusing that is safe for a different reason: it is
// a cache that validates itself. Every lookup compares the cell it wants
// against the cell it holds, so an entry left over from the previous chunk is
// either genuinely the right cell -- which happens at the shared edge, and is
// then a free hit -- or a miss that recomputes. It cannot be wrong.
//
// HEIGHTS ARE int16. The field runs from about -10 voxels in a cut basin to
// about 940 at the top of the relief, against a range of +-32767, so the margin
// is three orders of magnitude and the assert below is there to notice if the
// amplitudes in heightM are ever raised far enough to matter. Halving the array
// is worth having in the slope pass, which reads four neighbours per column and
// is the one loop here whose speed is a question of how much of the grid is in
// cache.
// ---------------------------------------------------------------------------
struct ChunkScratch {
    TerrainMemo memo;
    std::vector<int16_t> h;    // padded by two: a slope reaches one past a material
    std::vector<uint8_t> top;  // padded by one
    std::vector<uint8_t> sr;   // padded by one
};

// ---------------------------------------------------------------------------
// WHICH WOOD THIS IS.
//
// One field, read in three places -- the height field, the surface material,
// and which tree the scatter plants. Everything else in the engine is biome
// blind: the chunk mesher, the colliders, the perch system that hangs things in
// crowns, the streamer and the whole renderer never ask.
//
// That is deliberate and it is why this is an enum on the terrain rather than a
// second world class. The two woods differ in their LANDFORM and their PALETTE,
// not in how a voxel becomes a triangle.
// ---------------------------------------------------------------------------
enum class Biome : uint8_t {
    Pine,   // the original: high relief, ridges, basins, a needle floor
    Birch,  // low rounded hills, one light green everywhere, beehives
};

class VoxelTerrain {
  public:
    // ---------------------------------------------------------------------
    // THE BIOMES ARE PLACES, NOT A SETTING.
    //
    // This started as one enum for the whole world, which was enough to look at
    // a birch wood but made "go to the birch forest" meaningless -- there was
    // nowhere to go, the world was already entirely one or the other. So the
    // biome is now a function of WHERE YOU ARE: bands running north-south,
    // alternating, repeating forever. The same shape the browser engine uses
    // (see BIOP and BIRCHC in src/world/window.js), with two bands instead of
    // seven.
    //
    //     ... | pine | birch | pine | birch | ...
    //          -400   +400    +1200  +2000        metres, band centres
    //
    // 800 m a band, which is a real walk -- a minute and a half at the new run
    // speed -- and wider than the 307 m view radius, so a band fills the view
    // rather than being a stripe you see both edges of.
    //
    // `biome` survives as the FORCED override for --birch and --pine: set it
    // and the bands are ignored. That is what makes a screenshot or a profile
    // run reproducible without having to also pin a position.
    // ---------------------------------------------------------------------
    static constexpr float kBandW = 800.0f;    // metres of one band
    static constexpr float kBandBlend = 90.0f; // metres the two are mixed over

    // Where the centre of each band sits, so /locate has somewhere to send you.
    static float bandCentre(Biome b) {
        return (b == Biome::Birch) ? kBandW * 0.5f : -kBandW * 0.5f;
    }

    // 0 in the pine band, 1 in the birch band, eased across the seam. A pure
    // function of x -- no noise, no memo -- so anything may ask it at any time.
    static float birchWeight(float x) {
        const float period = 2.0f * kBandW;
        float u = fmodf(x, period);
        if (u < 0.0f) u += period;
        // Boundaries at u = 0 and u = W. [0, W) is birch -- so the birch centre
        // is +W/2 -- and [W, 2W) is pine, whose centre 3W/2 is -W/2 wrapped.
        //
        // t is the SIGNED distance to the nearest boundary: positive inside the
        // birch, negative inside the pine, and its magnitude is how far in.
        // Writing it that way is what makes the blend one expression instead of
        // two mirrored ones that have to be kept in step.
        const float t = (u < kBandW) ? minf(u, kBandW - u) : -minf(u - kBandW, period - u);
        return sstep(saturate(t / kBandBlend * 0.5f + 0.5f));
    }

    // Forced, when --birch or --pine pinned it; otherwise whatever the bands
    // say at this position.
    bool forced = false;
    Biome biome = Biome::Pine;
    bool birchAt(float x) const { return forced ? (biome == Biome::Birch) : birchWeight(x) >= 0.5f; }
    float birchMix(float x) const {
        return forced ? (biome == Biome::Birch ? 1.0f : 0.0f) : birchWeight(x);
    }
    // The old whole-world question, kept for the things that genuinely are
    // global: which model sets to LOAD, and whether the hive pass can run at
    // all. Both woods' trees are loaded whenever the bands are live.
    bool birch() const { return !forced || biome == Biome::Birch; }

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
    // SMOOTH AND ROUND, AT THE SAME ELEVATION. Asked for directly, and the
    // second half of that is the hard half: everything that takes roughness out
    // of a field takes height out with it, so every amplitude here had to be
    // re-fitted to put the height back.
    //
    //     was    4.0 + roll*60 x5 + swell*24 x3 + ridge*6
    //     now   14.0 + roll*48 x2 + swell*21.5 x2
    //
    // Three separate things were making the pine wood lumpy, and they live at
    // three different scales:
    //
    //   * THE RIDGED OCTAVE, at 33 m. A ridged multifractal is *for* creasing
    //     -- 1 - |2n-1| is a fold by construction -- so it cannot be smoothed,
    //     only removed. The birch wood dropped it long ago for exactly this
    //     reason and the note below records what that did; this is the same
    //     decision arriving in the pine.
    //   * ROLL'S TOP THREE OCTAVES, at 19, 9 and 4.5 m. Together they carried
    //     about 12 m of relief on features you cross in a stride or two, which
    //     is what made a hillside read as rubble rather than as a hill.
    //   * SWELL'S THIRD OCTAVE, at 35 m, worth another 3.4 m of the same.
    //
    // THE FREQUENCIES ARE STILL UNTOUCHED, which is the rule this field has
    // been grown under twice already. The hills are the same hills, in the same
    // places, at the same widths and the same heights -- what has gone is the
    // small stuff riding on them.
    //
    // MEASURED, 360k columns over 1.5 km square, pinned pine. Elevation is read
    // as the height DISTRIBUTION rather than as a range, since the ends of a
    // range are single columns; roundness as the mean sag from flat over a
    // baseline, which is a scale-by-scale answer to "how bumpy is it".
    //
    //     height m   mean   sd    p1     p5    p50    p95    p99
    //       was     48.08  8.65  28.83  33.94  48.06  62.37  67.81
    //       now     47.94  8.59  29.21  34.06  47.71  62.42  67.47
    //
    //     sag from flat, m    1 m     3 m    10 m    30 m   100 m   |grad|
    //       was               .075    .415   1.513   3.910   7.792   0.528
    //       now               .014    .088    .656   3.219   7.631   0.312
    //
    // So 82% less at a stride, 79% at three metres, 57% at ten -- and 2% at a
    // hundred, which is the number that says the LANDSCAPE did not change. The
    // gradient a body actually walks up fell by 41%.
    //
    // kRockSlope and kTreeSlope are left where they are. They are cliff guards
    // -- 3.6 and 3.0 m of drop across two 10 cm columns -- and neither fires on
    // a single column of that sample either before or after, so moving them
    // with the amplitudes, as the two earlier growth spurts had to, would only
    // start rejecting ground that is perfectly good to stand a tree on.
    //
    // AND IT IS CHEAPER. A pine column is 17 octaves of value noise where it
    // was 24: the three the roll dropped and the one the swell dropped are gone
    // along with the ridge's three. Only a column inside the 90 m seam pays
    // more, at 25, because it evaluates both woods' fields -- and the warp
    // underneath them is shared, so the second call hits the memo.
    //
    // The two fine octaves that used to sit on top are down to one at half the
    // amplitude. At 10 cm voxels those were quantising into single-voxel
    // stipple, which reads as gravel rather than as ground and cost a side quad
    // on nearly every column to draw.
    float heightM(float x, float z, TerrainMemo &memo) const {
        // THE FINE OCTAVE IS SHARED, and asked once. Both woods want it for the
        // same reason -- at 10 cm a smooth slope terraces into wide flat
        // plateaus and the small stuff is what breaks the steps up -- and
        // asking it twice inside the blend would be a second evaluation of the
        // most expensive thing here for no difference in the answer.
        const float fine = (fbm(memo.fine, x * 0.090f + 3.7f, z * 0.090f + 9.1f, 3) - 0.5f) * 1.2f;
        const float mix = birchMix(x);

        // ------------------------------------------------------------- birch
        // MUCH LOWER, AND ROUNDED. The pine wood is a mountain range -- 90 m of
        // relief -- because that is what makes a conifer stand read as
        // altitude. A birch wood is the opposite kind of place: open, gentle,
        // and low.
        //
        // Dropping the ridged octave entirely is what made these hills ROUND --
        // ridged noise is |1 - 2n|, which has a crease at every zero crossing,
        // and no amount of scaling it down removes the crease. What is left is
        // the warped fbm and the swell, both of which are smooth by
        // construction.
        //
        //     pine   14 + roll*48 x2 + swell*21.5 x2    14 .. 84 m
        //     birch   2 + roll*15 x5 + swell*7 x3        2 .. 24 m
        //
        // A quarter of the relief and no creases: hills you walk over rather
        // than climb. The basin carve is skipped too -- it exists to hollow out
        // lakes, and this wood has no water in it yet.
        //
        // THE BIRCH KEEPS ALL FIVE ROLL OCTAVES AND ALL THREE SWELL, and that
        // is the point of the split above: the pine was asked to be smoothed
        // and the birch was not, so the birch's field is the one it always had,
        // value for value. Its small stuff is worth about a metre and a half on
        // a wood with 22 m of relief, where the pine's was worth twelve on
        // ninety -- which is why one of them was asked about and the other was
        // not.
        if (mix >= 0.999f) {
            const float roll =
                warpedFbm(memo.warpX, memo.warpZ, memo.roll, x * 0.0130f, z * 0.0130f, 1.5f, 5);
            const float swell = fbm(memo.swell, x * 0.0070f + 71.3f, z * 0.0070f + 29.7f, 3);
            return 2.0f + roll * 15.0f + swell * 7.0f + fine;
        }

        // -------------------------------------------------------------- pine
        // TWO OCTAVES EACH, AND NOTHING ABOVE THEM. See the note on this
        // function for what the missing ones were carrying and what taking them
        // out measured.
        const float roll =
            warpedFbm(memo.warpX, memo.warpZ, memo.roll, x * 0.0130f, z * 0.0130f, 1.5f, 2);
        const float swell = fbm(memo.swell, x * 0.0070f + 71.3f, z * 0.0070f + 29.7f, 2);

        float h = 14.0f + roll * 48.0f + swell * 21.5f;

        const float b = fbm(memo.basin, x * 0.0160f + 311.7f, z * 0.0160f + 157.3f, 4);
        if (b < 0.40f) {
            const float m = sstep(minf(1.0f, (0.40f - b) / 0.10f));
            // Scaled with the terrain, every time it grows. This gate is a
            // FRACTION of the landform's range dressed up as metres: at the
            // original height it was 9, at twice that 18, and at four times it
            // is 36. Leave it behind and it sits below almost every column, no
            // basin ever cuts, and the water plane ends up buried under the
            // whole world with not a lake anywhere.
            //
            // THE SMOOTHING DID NOT MOVE IT, and that follows from what the
            // smoothing was fitted to do: the range this is a fraction of is
            // the same range it was.
            const float lowGate = saturate((36.0f - h) / 28.0f);
            h -= m * lowGate * (h - (waterLevel - 3.2f));
        }

        // AND THE FINE OCTAVE HAS TO STAY. Cutting it entirely was a mistake
        // the first pass made: at 10 cm voxels a slope quantises into steps
        // whose WIDTH is the voxel size over the gradient, so a field that is
        // smooth everywhere terraces into wide flat plateaus -- which reads as
        // worse, not rounder. Roundness belongs in the large shapes; the small
        // ones have to keep enough gradient to break the steps up.
        //
        // IT MATTERS MORE NOW THAN IT DID, which is worth saying plainly: this
        // field is smoother than the one that warning was written about, so the
        // fine octave is most of what stands between a hillside and a
        // staircase. Its own gradient reaches 0.34, which holds the steps to
        // about 30 cm wherever the landform underneath has gone flat.
        h += fine;
        if (mix <= 0.001f) return h;

        // THE SEAM. Ninety metres of blend between a wood whose median floor is
        // 48 m and one whose median is 13, which is a 35 m drop -- so this is
        // not a detail, it is a hillside, and it wants to be walked down rather
        // than fallen off. sstep on both sides of birchWeight is what makes the
        // join C1: the gradient goes to zero at each end of the blend instead
        // of changing abruptly where the lerp starts and stops.
        //
        // THE BIRCH SIDE IS ASKED FOR SEPARATELY HERE, at its own octave
        // counts, and that is the entire cost of the two woods no longer
        // sharing one field. It falls on the 11% of columns inside a seam and
        // on none of the others. The warp is the same warp at the same
        // frequency, so the second warpedFbm walks straight into the memo the
        // first one filled; only the two fbm memos have to be their own, which
        // is what the pair freed by the ridge is doing on TerrainMemo.
        const float bRoll = warpedFbm(memo.warpX, memo.warpZ, memo.birchRoll, x * 0.0130f,
                                      z * 0.0130f, 1.5f, 5);
        const float bSwell = fbm(memo.birchSwell, x * 0.0070f + 71.3f, z * 0.0070f + 29.7f, 3);
        return lerpf(h, 2.0f + bRoll * 15.0f + bSwell * 7.0f + fine, mix);
    }

    // The memo-less form, for the scatter paths -- see the note on TerrainMemo.
    float heightM(float x, float z) const {
        TerrainMemo memo;
        return heightM(x, z, memo);
    }

    // Column height in VOXELS -- the one place the world is quantised.
    int heightVox(int i, int j, TerrainMemo &memo) const {
        const float h = heightM(wx(i), wx(j), memo);
        return int(floorf(h / VOXEL_M));
    }
    int heightVox(int i, int j) const {
        TerrainMemo memo;
        return heightVox(i, j, memo);
    }

    // Slope, in voxels of drop across two columns. Both thresholds move with
    // the height of the terrain, and have now done so twice: the field is four
    // times its original relief, so the same hillside measures four times the
    // drop across the same two columns. Left at 9 and 7 -- or at 18 and 15 --
    // the world turns to bare rock and refuses to plant a tree on any of it.
    //
    // These are the numbers that decide what the wood LOOKS like, far more than
    // the amplitudes do: they are the line between a forested hill and a scree
    // slope, and they have to be kept in step with heightM by hand.
    static constexpr int kRockSlope = 36;
    static constexpr int kTreeSlope = 30;

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

    // Voxels of loose soil between the surface and the rock -- see crustVox.
    // The old emit loop had this as a literal 3; two to six reads as a bank
    // that thins and thickens rather than as a stripe ruled along the hill.
    int crustMin = 2, crustMax = 6;
    uint32_t crustSeed = 20260907u;

    float standDensity(float x, float z, FbmMemo &m) const {
        return fbm(m, x * 0.0165f + 71.3f, z * 0.0165f + 44.1f, 3);
    }
    float standDensity(float x, float z) const {
        FbmMemo m;
        return standDensity(x, z, m);
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
    uint8_t topMaterial(int i, int j, int h, int slope, TerrainMemo &memo) const {
        // ------------------------------------------------------------- birch
        // ONE GREEN, EVERYWHERE. No sand, no silt, no soil, no needle litter,
        // and no rock however steep the ground gets -- a birch wood is a
        // meadow with trees in it, and the moment a hillside turns brown it
        // stops reading as one.
        //
        // GRASS_0 is the right slot rather than a new ramp, and that is the
        // whole trick: those six ids are filled by deriveGroundFromTrees from
        // whatever foliage the loaded models actually use. Load birches and the
        // floor becomes the birches' own greens without a colour being written
        // down anywhere. "Light green matching the trees" is not a value here,
        // it is a consequence.
        //
        // It stays a six-step ramp rather than a single flat green for the
        // reason recorded at the top of this file: one value over a whole
        // hillside reads as a painted plane, because the eye finds the repeat
        // instantly. Birch foliage is a narrow range to begin with, so six
        // steps of it still reads as one colour -- solid, but not flat.
        // DITHERED ACROSS THE SEAM, not switched at it. A hard line at
        // mix = 0.5 would draw a straight north-south edge across the world
        // where the needle floor meets the meadow -- the one shape nothing else
        // in this terrain has. Testing the mix against a hash of the column
        // instead interleaves the two over the blend, so the woods dissolve
        // into each other the way a real treeline does.
        //
        // The hash is the COLUMN's, so it is stable: the same column answers
        // the same way every time it is meshed, from any chunk, on any thread.
        if (birchMix(wx(i)) > hashUnit(0x81E5u, hashU32(uint32_t(i), uint32_t(j))))
            return mat::GRASS_0;

        const int wl = int(waterLevel / VOXEL_M);
        if (h <= wl) return (wl - h <= 8) ? mat::SAND : mat::SILT;
        if (h <= wl + 8) return mat::SAND;  // the shore band

        if (slope >= kRockSlope) return mat::ROCK;  // too steep to hold soil

        const float x = wx(i), z = wx(j);

        // WHICH SHADE IS NOT DECIDED HERE ANY MORE.
        //
        // It used to come from a three-metre noise field, which is why the
        // floor read as patches of one green next to patches of another. A
        // pine does not look like that: its bark is half a dozen browns
        // scattered voxel by voxel, and that scatter is most of why a trunk
        // reads as bark rather than as a painted cylinder. The ground is
        // scattered the same way now, on the device, from a hash of the voxel
        // the ray hit -- see groundShade() in Trace.cs.slang.
        //
        // Moving it there is not merely tidier. A material that changed every
        // three metres broke the top-face merge at every patch boundary; naming
        // only the family leaves the runs unbroken, so this costs no triangles
        // and saves some. It also retires an entire fbm field per column.
        //
        // THE GRASS MASK IS ASKED FIRST, and that ordering is the whole fix.
        //
        // The litter rule used to run before it and take whatever it wanted.
        // Litter follows the canopy, the canopy follows the stand-density
        // field, and that field is a SIXTY-METRE feature -- so whole hillsides
        // came out two thirds litter while ground a few hundred metres away was
        // barely a third. Measured over 200 m squares the ground ran from 62%
        // brown to 80% brown, and grass from 33% down to 20%. The ratio was a
        // function of where you were standing, which is the one thing it should
        // not be.
        //
        // The mask itself has no such problem: its coarsest octave is under two
        // metres, so over any patch bigger than a few strides it averages to
        // the same fraction everywhere. Asking it first is therefore asking the
        // only field here that is evenly distributed by construction.
        if (fbm(memo.grassMask, x * 0.55f + 31.7f, z * 0.55f + 17.2f, 4) > 0.50f)
            return mat::GRASS_0;

        // What is left is brown either way, so the canopy is still allowed to
        // say WHICH brown -- needle litter under a thick stand, soil in the
        // open. That is what the rule was for; it was only ever the ratio it
        // had no business setting.
        if (standDensity(x, z, memo.stand) > 0.44f &&
            fbm(memo.litter, x * 3.1f + 63.0f, z * 3.1f + 88.0f, 2) > 0.36f)
            return mat::LITTER_0;

        return mat::SOIL_0;
    }

    uint8_t topMaterial(int i, int j, int h, int slope) const {
        TerrainMemo memo;
        return topMaterial(i, j, h, slope, memo);
    }

    // -----------------------------------------------------------------------
    // WHAT IS UNDERNEATH -- the crust, and the rock under it.
    //
    // The world was a SKIN. heightM said where the ground was and topMaterial
    // said what it was made of THERE, and between them they described a surface
    // with nothing behind it -- so "what is two voxels down" was not a question
    // this world could answer. The mesher knew an answer anyway, because it has
    // to draw the side of a step: surface voxel, three of soil, rock below. But
    // it knew it as three lines inside an emit loop, which is a drawing
    // decision rather than a fact about the world, and nothing else could ask.
    //
    // So the profile lives here and the mesher calls it. The picture is
    // deliberately almost unchanged -- the shape below is that emit loop's own
    // -- and what IS new is that the world has a subsurface at all: every depth
    // has a defined material, which is what a shovel needs to expose.
    //
    // THE CRUST VARIES, because a constant depth is a stripe. Value noise on a
    // 12-voxel lattice, smoothstepped: four hashes and three lerps a column,
    // against the several octaves of fbm the height already costs in the same
    // loop. Coarse enough that a bank cut through it shows soil thinning and
    // thickening like strata, where a per-column hash would give static.
    // -----------------------------------------------------------------------
    int crustVox(int i, int j) const {
        const int L = 12;  // lattice, in voxels
        const int ci = floorDiv(i, L), cj = floorDiv(j, L);
        const float fx = float(i - ci * L) / float(L), fz = float(j - cj * L) / float(L);
        const float ux = fx * fx * (3.0f - 2.0f * fx), uz = fz * fz * (3.0f - 2.0f * fz);
        const float a = hashUnit(crustSeed, hashU32(uint32_t(ci), uint32_t(cj)));
        const float b = hashUnit(crustSeed, hashU32(uint32_t(ci + 1), uint32_t(cj)));
        const float c = hashUnit(crustSeed, hashU32(uint32_t(ci), uint32_t(cj + 1)));
        const float d = hashUnit(crustSeed, hashU32(uint32_t(ci + 1), uint32_t(cj + 1)));
        const float lo = a + (b - a) * ux, hi = c + (d - c) * ux;
        const float t = lo + (hi - lo) * uz;
        const int span = maxi(1, crustMax - crustMin + 1);
        return crustMin + mini(span - 1, int(t * float(span)));
    }

    // The material of the voxel at row y in column (i, j), given that column's
    // surface row h and what topMaterial put on it. AIR above the surface; the
    // surface keeps its own material; and a rock outcrop is rock the whole way
    // down rather than rock sitting on soil.
    uint8_t materialAt(int i, int j, int y, int h, uint8_t top) const {
        if (y > h) return mat::AIR;
        if (y == h) return top;
        if (top == mat::ROCK) return mat::ROCK;
        return (h - y <= crustVox(i, j)) ? mat::SOIL_0 : mat::ROCK;
    }

    uint8_t topMaterial(int i, int j, int h, TerrainMemo &memo) const {
        // Only reached from the sparse scatter paths. Below the shore band the
        // slope is never consulted, so it is not worth four height evaluations
        // to compute one that will be discarded.
        const int wl = int(waterLevel / VOXEL_M);
        if (h <= wl + 8) return topMaterial(i, j, h, 0, memo);
        const int slope = maxi(absi(heightVox(i + 1, j, memo) - heightVox(i - 1, j, memo)),
                               absi(heightVox(i, j + 1, memo) - heightVox(i, j - 1, memo)));
        return topMaterial(i, j, h, slope, memo);
    }
    uint8_t topMaterial(int i, int j, int h) const {
        TerrainMemo memo;
        return topMaterial(i, j, h, memo);
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
    VoxMesh meshChunk(int cx, int cz, ChunkScratch &scratch) const {
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
        // ONE MEMO FOR THE WHOLE CHUNK, and the loop order is what makes it
        // pay: i on the inside means x advances by a voxel at a time while z
        // holds, so every octave's lattice cell is the one it was last column
        // for hundreds of columns at a stretch. See TerrainMemo.
        // Carried by the worker rather than built here -- see ChunkScratch.
        TerrainMemo &memo = scratch.memo;

        scratch.h.resize((size_t(n) + 4) * (size_t(n) + 4));
        int16_t *const hp = scratch.h.data();
        auto H = [&](int i, int j) -> int16_t & { return hp[size_t(j + 2) * (n + 4) + size_t(i + 2)]; };
        for (int j = -2; j <= n + 1; ++j)
            for (int i = -2; i <= n + 1; ++i) {
                const int hv = heightVox(I0 + i, J0 + j, memo);
                assert(hv > -32768 && hv < 32767);  // see the note on int16 in ChunkScratch
                H(i, j) = int16_t(hv);
            }

        scratch.top.resize((size_t(n) + 2) * (size_t(n) + 2));
        uint8_t *const tp = scratch.top.data();
        auto T = [&](int i, int j) -> uint8_t & {
            return tp[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        for (int j = -1; j <= n; ++j)
            for (int i = -1; i <= n; ++i) {
                const int slope =
                    maxi(absi(H(i + 1, j) - H(i - 1, j)), absi(H(i, j + 1) - H(i, j - 1)));
                T(i, j) = topMaterial(I0 + i, J0 + j, H(i, j), slope, memo);
            }

        // How tall a strand stands on each column, 0 for none. Computed for the
        // padded grid so a column on the edge can still ask its neighbours.
        // WRITTEN ON EVERY PATH, including the two that used to `continue` and
        // leave the value alone. That is what lets the grid be reused across
        // chunks without a fill: a stale row from the last chunk is overwritten
        // rather than inherited, and the zero goes into a cache line this loop
        // is touching anyway instead of into a separate 66 KB memset.
        scratch.sr.resize((size_t(n) + 2) * (size_t(n) + 2));
        uint8_t *const srp = scratch.sr.data();
        auto SR = [&](int i, int j) -> uint8_t & {
            return srp[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        for (int j = -1; j <= n; ++j)
            for (int i = -1; i <= n; ++i) {
                uint8_t &rows = SR(i, j);
                rows = 0;
                if (!isGrass(T(i, j))) continue;
                // Hashed on the WORLD column, so a strand is in the same place
                // no matter which chunk happens to be meshing it -- otherwise
                // the grass would reshuffle every time a chunk was rebuilt.
                const uint32_t cell = hashU32(uint32_t(I0 + i), uint32_t(J0 + j));
                if (hashUnit(strandSeed, cell) >= grassDensity) continue;
                const int span = maxi(1, grassMaxRows - grassMinRows + 1);
                // CLAMPED TO WHAT THE PACKED TRIANGLE CAN SAY. The device
                // recovers a blade's row from three bits of its base, so a
                // ninth voxel would wrap to the bottom of the gradient and
                // wear the darkest green at the tip. These two are tuning
                // knobs; the ceiling is a format.
                rows = uint8_t(mini(STRAND_MAX_ROWS,
                                    grassMinRows +
                                        mini(span - 1,
                                             int(hashUnit(strandSeed + 1u, cell) * span))));
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
        auto sideQuad = [&](VoxMesh &out, int i, int j, int dir, int lo, int hi, uint8_t mtl,
                            uint8_t sc) {
            if (hi <= lo) return;
            const float x0 = float(I0 + i) * s, x1 = x0 + s;
            const float z0 = float(J0 + j) * s, z1 = z0 + s;
            const float y0 = float(lo) * s, y1 = float(hi) * s;
            switch (dir) {
                case 0: out.addQuad({x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}, mtl, face::POS_X, sc); break;
                case 1: out.addQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, mtl, face::NEG_X, sc); break;
                case 2: out.addQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, mtl, face::POS_Z, sc); break;
                default:out.addQuad({x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}, mtl, face::NEG_Z, sc); break;
            }
        };

        // The part of [lo,hi) that [nlo,nhi) does not cover, as up to two runs.
        auto emitUncovered = [&](VoxMesh &out, int i, int j, int dir, int lo, int hi, int nlo,
                                 int nhi, uint8_t mtl, uint8_t sc) {
            if (nhi <= nlo) { sideQuad(out, i, j, dir, lo, hi, mtl, sc); return; }
            sideQuad(out, i, j, dir, lo, mini(hi, nlo), mtl, sc);
            sideQuad(out, i, j, dir, maxi(lo, nhi), hi, mtl, sc);
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
                    // WHAT MAKES A BLADE A GRADIENT rather than a green stick.
                    // Every face of this strand carries the row it stands on,
                    // and the device turns the height above that row into a
                    // shade -- dark at the soil, light at the tip. Stored per
                    // STRAND, not per row: the quads below still merge over
                    // whole spans, so the gradient costs nothing but these
                    // three bits. See the packed-triangle note above.
                    const uint8_t sc = strandCodeFor(lo);
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
                        emitUncovered(m, i, j, d, maxi(lo, ground), hi, nlo, nhi, tm, sc);
                    }

                    const float yt = float(hi) * s;
                    m.addQuad({x0, yt, z0}, {x0, yt, z1}, {x1, yt, z1}, {x1, yt, z0}, cap,
                              face::POS_Y, sc);
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
        // The crust at a CHUNK-LOCAL column. crustVox is a world function, as
        // everything in this file is -- the offsets are the mesher's business.
        auto CR = [&](int i, int j) { return crustVox(I0 + i, J0 + j); };

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
                        // ...AND THE CRUST IS PART OF THE KEY. A run exists
                        // because every column in it emits the SAME bands, and
                        // the crust depth is now one of the things deciding
                        // where a band splits -- merging across a change in it
                        // would paint one column's soil line across its
                        // neighbours'. Asked only at the columns a run is
                        // trying to grow past, so it costs four hashes at a
                        // boundary and nothing along a uniform bank.
                        if (H(i2, j2) != hc || H(i2 + di, j2 + dj) != nb || T(i2, j2) != tm ||
                            CR(i2, j2) != CR(i, j))
                            break;
                        ++k;
                    }
                    const int run = k - inner;

                    // THE BANDS ARE THE WORLD'S NOW, not this loop's -- see
                    // materialAt, whose profile this walks in runs rather than
                    // voxel by voxel. Asking it per voxel would be the same
                    // picture at several hundred times the cost; what matters
                    // is that the two cannot disagree, and the depth they both
                    // read is crustVox.
                    int cursor = hc + 1;
                    const int surfLo = maxi(nb + 1, hc);
                    sideBand(m, i, j, d, surfLo, cursor, tm, run);
                    cursor = surfLo;
                    if (cursor > nb + 1) {
                        if (tm != mat::ROCK) {
                            const int soilLo = maxi(nb + 1, hc - CR(i, j));
                            sideBand(m, i, j, d, soilLo, cursor, mat::SOIL_0, run);
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
