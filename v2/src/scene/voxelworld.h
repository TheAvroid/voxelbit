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
#include <mutex>
#include <unordered_map>
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
// THE FLOOR OF THE WORLD. Below the stone, and the last thing there is: a
// tool that reaches this finds something it cannot get through. Slot 6 was
// NEEDLE_LITTER before that became a ramp of its own below, so this is a reuse
// of a free number rather than a new one.
constexpr uint8_t BEDROCK = 6;

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
// THE STONE RAMP -- the terrain's rock, wearing the BOULDERS' OWN COLOURS.
//
// mat::ROCK is a single flat grey, and a single grey is what dug stone used
// to look like: a slab, beside twenty-seven boulder models carrying half a
// dozen real stone tones each. These six entries are filled AT LOAD from the
// rock models' own palettes (see Palette::setStoneBand), so the ground you
// dig into is made of the same stone as the rocks lying on top of it.
//
// THE HOST STILL STORES ONE ID. The mesher writes mat::ROCK and nothing else,
// which is what keeps a hillside merging into long runs; the device spreads
// that one id over this ramp per voxel, exactly as it already does for grass
// and soil. See groundShade().
constexpr uint8_t STONE_0 = 20;
constexpr uint8_t STONE_COUNT = 6;   // 20..25
// WATER, AND IT IS A VOXEL LIKE ANYTHING ELSE.
//
// v1 stores water in the grid -- WATER_T and WATER_B are palette ids there, not
// a surface someone draws -- and that is what this is. It matters more than
// tidiness: the previous attempt in this engine gave water its own per-chunk
// acceleration structure because "a dielectric cannot share one with the
// ground", and that is what collided in the compaction queue and replaced whole
// chunks of terrain with a flat sheet. A material needs no second structure. It
// merges into the same runs, rides the same BLAS, and cannot desynchronise from
// the ground it sits in because it IS the ground's mesh.
//
// ONE ID, not v1's two: which face of a water voxel a ray hit is already packed
// into the triangle, so the surface ripple keys off the face direction instead
// of off a second material. See the water branch in Trace.cs.slang.
constexpr uint8_t WATER = 26;
constexpr uint8_t TREE_BASE = 27;  // model palette entries are allocated from here up
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

    // FILL THE STONE RAMP FROM THE ROCK MODELS THEMSELVES.
    //
    // Given every colour the boulder .vox files actually use, this keeps the
    // stone-looking ones and spreads six of them across the range by
    // luminance -- darkest first, so the ramp reads as one material lit
    // differently rather than as six unrelated greys.
    //
    // GREEN IS DROPPED. The rocks are grown over with moss at load, and moss
    // in the middle of a freshly dug hole would be a strange thing to find.
    void setStoneBand(std::vector<std::array<uint8_t, 4>> cols) {
        std::vector<std::array<uint8_t, 4>> keep;
        for (const auto &c : cols) {
            const int mx = maxi(int(c[0]), maxi(int(c[1]), int(c[2])));
            const int mn = mini(int(c[0]), mini(int(c[1]), int(c[2])));
            if (mx < 8) continue;                       // black, not a stone
            if (mx - mn > mx / 3) continue;             // too saturated: moss or a runic vein
            keep.push_back(c);
        }
        if (keep.size() < size_t(mat::STONE_COUNT)) return;   // leave the defaults
        std::sort(keep.begin(), keep.end(),
                  [](const std::array<uint8_t, 4> &a, const std::array<uint8_t, 4> &b) {
                      return (a[0] * 2 + a[1] * 5 + a[2]) < (b[0] * 2 + b[1] * 5 + b[2]);
                  });
        for (int k = 0; k < mat::STONE_COUNT; ++k) {
            const size_t idx = keep.size() * size_t(k) / size_t(mat::STONE_COUNT);
            const auto &c = keep[mini(idx, keep.size() - 1)];
            set(uint8_t(mat::STONE_0 + k), srgbToLinearF(float(c[0]) / 255.0f),
                srgbToLinearF(float(c[1]) / 255.0f), srgbToLinearF(float(c[2]) / 255.0f),
                0.88f);
        }
        stoneFromRocks_ = int(keep.size());
    }
    int stoneSampleCount() const { return stoneFromRocks_; }

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
        // Darker and flatter than ROCK, so the change of layer reads as a
        // change of material rather than a change of light.
        set(mat::BEDROCK, 0.17f, 0.17f, 0.18f, 0.97f);
        // A GREY RAMP UNTIL THE ROCKS ARE LOADED. setStoneBand replaces these
        // with the boulders' real tones; these are only what stone looks like
        // if that never happens, and they bracket mat::ROCK rather than
        // wandering off it.
        for (int k = 0; k < mat::STONE_COUNT; ++k) {
            const float t = float(k) / float(mat::STONE_COUNT - 1);
            const float g = 0.34f + 0.16f * t;
            set(uint8_t(mat::STONE_0 + k), g, g * 0.99f, g * 0.94f, 0.88f);
        }

    }

    std::vector<MaterialLook> look_ = std::vector<MaterialLook>(mat::COUNT);
    std::map<uint32_t, uint8_t> index_;
    uint8_t next_ = mat::TREE_BASE;
    int pineEnd_ = 0;
    int stoneFromRocks_ = 0;
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

// ---------------------------------------------------------------------------
// WHAT IS IN EACH BLOCK OF A MODEL: nothing, some of it, or all of it.
//
// A COARSE SUMMARY THAT PAYS FOR ITSELF TWICE. Re-meshing a damaged boulder
// costs 33.8 ms because it walks all 3.2 million of its voxels and asks each
// one about its six neighbours -- and the answer is the same for almost all of
// them. A voxel deep inside the rock has six solid neighbours and emits
// nothing; a voxel out in the air has nothing to emit either. Only the shell
// matters, and the shell is a few per cent of the model.
//
// So the volume is summarised in blocks of kBlockVox, once, and the mesher
// skips any block that provably cannot emit a face: an EMPTY one, and a FULL
// one whose six neighbouring blocks are also full. What is left is the
// boundary, which is what was wanted all along.
//
// The same summary answers the connectivity question -- see World::looseFraction
// -- so a damaged instance keeps one of these and updates only the blocks a
// bite touched, and neither of the two costs the walk again.
// ---------------------------------------------------------------------------
constexpr int kBlockVox = 8;

enum : uint8_t { BLOCK_EMPTY = 0, BLOCK_MIXED = 1, BLOCK_FULL = 2 };

inline void blockDims(int sx, int sy, int sz, int *bx, int *by, int *bz) {
    *bx = (sx + kBlockVox - 1) / kBlockVox;
    *by = (sy + kBlockVox - 1) / kBlockVox;
    *bz = (sz + kBlockVox - 1) / kBlockVox;
}

inline void blockSummary(const std::vector<uint8_t> &vol, int sx, int sy, int sz,
                         std::vector<uint8_t> *out) {
    int bx = 0, by = 0, bz = 0;
    blockDims(sx, sy, sz, &bx, &by, &bz);
    out->assign(size_t(bx) * size_t(by) * size_t(bz), BLOCK_EMPTY);
    // Counted rather than tested, so a block that runs off the edge of the
    // model is MIXED rather than FULL -- the faces on that edge are real.
    std::vector<uint32_t> solid(out->size(), 0), total(out->size(), 0);
    for (int y = 0; y < sy; ++y)
        for (int z = 0; z < sz; ++z) {
            const size_t row = size_t(z) * size_t(sx) + size_t(y) * size_t(sx) * size_t(sz);
            const int bj = y / kBlockVox, bk = z / kBlockVox;
            for (int x = 0; x < sx; ++x) {
                const size_t bi = size_t(x / kBlockVox) + size_t(bk) * size_t(bx) +
                                  size_t(bj) * size_t(bx) * size_t(bz);
                ++total[bi];
                if (vol[row + size_t(x)] != mat::AIR) ++solid[bi];
            }
        }
    const int full = kBlockVox * kBlockVox * kBlockVox;
    for (size_t i = 0; i < out->size(); ++i) {
        if (solid[i] == 0) (*out)[i] = BLOCK_EMPTY;
        else if (solid[i] == uint32_t(full) && total[i] == uint32_t(full)) (*out)[i] = BLOCK_FULL;
        else (*out)[i] = BLOCK_MIXED;
    }
}

// ---------------------------------------------------------------------------
// THE SAME SURFACE EXTRACTION, over a volume of GLOBAL MATERIAL IDS.
//
// meshAsset below works on a VoxAsset -- palette entries plus the table that
// maps them -- which is what a freshly loaded .vox is. A DAMAGED instance is
// not that: it is ModelTemplate::volume, already resolved to global ids, with
// the voxels a pick took out set to AIR. Same rule either way, and it is the
// rule this whole engine runs on: emit a face where solid meets air, and keep
// everything else.
// ---------------------------------------------------------------------------
inline VoxMesh meshVolume(const std::vector<uint8_t> &vol, int sx, int sy, int sz, float scale,
                          bool resolveShades = false, const std::vector<uint8_t> *blocks = nullptr) {
    VoxMesh m;
    const float s = scale;
    // THE SHADE IS DECIDED HERE, NOT ON THE DEVICE.
    //
    // A family id -- grass, soil, litter -- is normally turned into one exact
    // shade by hashing the voxel's WORLD position, which works because voxels
    // do not move. This mesh belongs to something that does: the hash re-rolls
    // as the piece drifts across cell boundaries, and a voxel straddling one
    // draws two shades at once. That is moss on a broken-off chunk crawling and
    // splitting while it is in the air.
    //
    // Resolved once here, from the piece's OWN coordinates, it is fixed for as
    // long as the piece exists. KIND_LOOSE is what tells the device not to roll
    // it again.
    // ONLY FOR SOMETHING THAT MOVES. A damaged ROCK is re-meshed through here
    // too, and it does not move -- its shades must keep coming from the world
    // position hash like every other static voxel, or the moss it still carries
    // changes colour the moment it is re-meshed. Resolving is for the loose
    // piece alone. See KIND_LOOSE.
    auto resolved = [resolveShades](uint8_t id, int x, int y, int z) -> uint8_t {
        if (!resolveShades) return id;
        uint8_t base = 0, count = 0;
        if (id >= mat::GRASS_0 && id < mat::GRASS_0 + mat::GRASS_COUNT) {
            base = mat::GRASS_0;
            count = mat::GRASS_COUNT;
        } else if (id >= mat::SOIL_0 && id < mat::SOIL_0 + mat::SOIL_COUNT) {
            base = mat::SOIL_0;
            count = mat::SOIL_COUNT;
        } else if (id >= mat::LITTER_0 && id < mat::LITTER_0 + mat::LITTER_COUNT) {
            base = mat::LITTER_0;
            count = mat::LITTER_COUNT;
        } else {
            return id;
        }
        // The same avalanche the device uses, on the piece's own voxel.
        uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 1103515245u +
                     uint32_t(z) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return uint8_t(base + h % uint32_t(count));
    };
    auto at = [&](int x, int y, int z) -> uint8_t {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return mat::AIR;
        // VOXASSET (WORLD) LAYOUT, which is x + z*sx + y*sx*sz and NOT the
        // x + y*sx + z*sx*sy of the raw model struct beside it in vox.h. The
        // two differ only by which axis strides furthest, so getting it wrong
        // does not crash -- it transposes the boulder, which is a great deal
        // harder to notice. ModelTemplate::volume is filled from VoxAsset.
        return vol[size_t(x) + size_t(z) * size_t(sx) + size_t(y) * size_t(sx) * size_t(sz)];
    };
    // WHICH BLOCKS CAN EMIT ANYTHING AT ALL -- see blockSummary. Without the
    // summary every voxel is walked, which on a boulder is 3.2 million of them
    // to change the nine hundred a pick took out.
    int bx = 0, by = 0, bz = 0;
    blockDims(sx, sy, sz, &bx, &by, &bz);
    auto blockAt = [&](int i, int j, int k) -> uint8_t {
        if (i < 0 || j < 0 || k < 0 || i >= bx || j >= by || k >= bz) return BLOCK_EMPTY;
        return (*blocks)[size_t(i) + size_t(k) * size_t(bx) +
                         size_t(j) * size_t(bx) * size_t(bz)];
    };
    auto skipBlock = [&](int i, int j, int k) {
        if (!blocks) return false;
        const uint8_t b = blockAt(i, j, k);
        if (b == BLOCK_EMPTY) return true;   // nothing in it to have a face
        if (b != BLOCK_FULL) return false;
        // Solid, and walled in on all six sides by solid: every voxel in it has
        // six solid neighbours, so not one of them can show a face.
        return blockAt(i - 1, j, k) == BLOCK_FULL && blockAt(i + 1, j, k) == BLOCK_FULL &&
               blockAt(i, j - 1, k) == BLOCK_FULL && blockAt(i, j + 1, k) == BLOCK_FULL &&
               blockAt(i, j, k - 1) == BLOCK_FULL && blockAt(i, j, k + 1) == BLOCK_FULL;
    };

    for (int y = 0; y < sy; ++y)
        for (int z = 0; z < sz; ++z) {
            const int bj = y / kBlockVox, bk = z / kBlockVox;
            for (int x = 0; x < sx; ++x) {
                if (blocks && skipBlock(x / kBlockVox, bj, bk)) {
                    // Straight to the end of this block's run along x.
                    x = (x / kBlockVox + 1) * kBlockVox - 1;
                    continue;
                }
                const uint8_t id = resolved(at(x, y, z), x, y, z);
                if (id == mat::AIR) continue;
                const float x0 = float(x) * s, x1 = x0 + s;
                const float y0 = float(y) * s, y1 = y0 + s;
                const float z0 = float(z) * s, z1 = z0 + s;
                if (at(x, y + 1, z) == mat::AIR)
                    m.addQuad({x0,y1,z0},{x0,y1,z1},{x1,y1,z1},{x1,y1,z0}, id, face::POS_Y);
                if (at(x, y - 1, z) == mat::AIR)
                    m.addQuad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}, id, face::NEG_Y);
                if (at(x + 1, y, z) == mat::AIR)
                    m.addQuad({x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}, id, face::POS_X);
                if (at(x - 1, y, z) == mat::AIR)
                    m.addQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, id, face::NEG_X);
                if (at(x, y, z + 1) == mat::AIR)
                    m.addQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, id, face::POS_Z);
                if (at(x, y, z - 1) == mat::AIR)
                    m.addQuad({x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}, id, face::NEG_Z);
            }
        }
    return m;
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
    FbmMemo shelf;                                          // the beach's own relief -- see heightM
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
    // THE CARVE, DECIDED ONCE. One bit per voxel in the band a cave may occupy,
    // indexed DOWN FROM EACH COLUMN'S OWN SURFACE (k = h - y) so the band
    // follows the terrain instead of spanning the chunk's whole relief. The
    // mesher asks about a voxel roughly seven times -- once for itself and once
    // per neighbouring face -- and evaluating two 3D fbms on each of those is
    // what made caves cost 385 ms a chunk against 10.6 without them. Reused
    // across chunks like every other buffer here.
    std::vector<uint64_t> carve;
    // HOW DEEP THE SOIL IS, PER COLUMN, padded by one. crustVox is four hashes
    // and three lerps and depends on nothing but (i, j) -- and the voxel pass
    // was calling it once per VOXEL, seven times over for the face tests. It is
    // filled beside the top material, which walks the same padded grid.
    std::vector<uint8_t> crust;
    // THE WATERLINE OVER EACH COLUMN, as a voxel row, or -1 where the column is
    // dry. Per column and not per chunk because the two woods have different
    // lines and a chunk is 25.6 m against an 800 m band -- see waterAt.
    std::vector<int16_t> wet;
    std::vector<uint8_t> wmerge;  // claimed columns, for the lake surface's 2D merge
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

// ---------------------------------------------------------------------------
// THE EDIT LAYER -- the world is procedural(seed) + edits, and this is the
// second term.
//
// AN UNTOUCHED WORLD IS ZERO BYTES. Only voxels somebody has actually changed
// are stored, keyed by world voxel coordinate, so a forest nobody has swung at
// costs nothing at all. That is what makes this affordable where a full mutable
// grid was not: the previous attempts sized storage by the WORLD, and this one
// sizes it by the DAMAGE.
//
// PUBLISHED BY COPY, NEVER MUTATED IN PLACE. The mesher runs on worker threads
// and must never observe a half-written edit. A swing builds the chunk's new
// edit set on the main thread and publishes it as a shared_ptr<const>; a worker
// takes that pointer under a brief lock and then reads it with no lock at all.
// An edit is rare and a mesh is not, so the contention sits on the rare side.
// ---------------------------------------------------------------------------
struct ChunkEdits {
    // World voxel -> material. mat::AIR is a hole somebody dug.
    std::unordered_map<uint64_t, uint8_t> vox;
    // The columns touched, each with the y span worth re-examining. meshChunk
    // voxel-meshes exactly these and leaves every other column on the fast
    // heightmap path -- see the note over the voxel pass.
    std::unordered_map<uint64_t, std::pair<int, int>> col;

    static uint64_t vkey(int i, int j, int y) {
        return (uint64_t(uint32_t(i) & 0x1fffffu) << 42) |
               (uint64_t(uint32_t(j) & 0x1fffffu) << 21) |
               uint64_t(uint32_t(y) & 0x1fffffu);
    }
    static uint64_t ckey(int i, int j) {
        return (uint64_t(uint32_t(i) & 0x1fffffu) << 21) | uint64_t(uint32_t(j) & 0x1fffffu);
    }
    bool voxel(int i, int j, int y, uint8_t *out) const {
        const auto it = vox.find(vkey(i, j, y));
        if (it == vox.end()) return false;
        *out = it->second;
        return true;
    }
    bool column(int i, int j, int *lo, int *hi) const {
        const auto it = col.find(ckey(i, j));
        if (it == col.end()) return false;
        *lo = it->second.first;
        *hi = it->second.second;
        return true;
    }
};

// The edit sets of every chunk that has any, and the one thing that writes them.
class EditStore {
  public:
    std::shared_ptr<const ChunkEdits> get(int cx, int cz) const {
        std::lock_guard<std::mutex> lk(mx_);
        const auto it = byChunk_.find(ChunkEdits::ckey(cx, cz));
        return it == byChunk_.end() ? nullptr : it->second;
    }

    // Take a bite of radius r (in voxels) out of the world at a voxel centre,
    // and report which chunks now need re-meshing. COPY ON WRITE: each touched
    // chunk's set is copied, added to, and republished, so any worker already
    // reading the old one keeps a consistent view until it finishes.
    std::vector<std::pair<int, int>> carve(int ci, int cj, int cy, int r) {
        std::map<std::pair<int, int>, std::shared_ptr<ChunkEdits>> touched;
        const int r2 = r * r;
        std::lock_guard<std::mutex> lk(mx_);
        for (int dy = -r; dy <= r; ++dy)
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di) {
                    if (di * di + dj * dj + dy * dy > r2) continue;
                    const int i = ci + di, j = cj + dj, y = cy + dy;
                    // A HOLE IS VISIBLE FROM THE CHUNK NEXT DOOR. The rock
                    // beside it needs a face pointing in, and that rock may
                    // belong to another chunk -- which has its own edit set and
                    // would otherwise never learn the hole exists. So each
                    // carved voxel is published to every chunk whose column
                    // ring reaches it, and each of those columns is marked for
                    // the voxel pass. Nine writes at a chunk seam, one anywhere
                    // else, and no seam left open either way.
                    for (int rj = -1; rj <= 1; ++rj)
                        for (int ri = -1; ri <= 1; ++ri) {
                            const int ni = i + ri, nj = j + rj;
                            const int cx = floorDiv(ni, CHUNK_VOX), cz = floorDiv(nj, CHUNK_VOX);
                            auto &slot = touched[{cx, cz}];
                            if (!slot) {
                                const auto it = byChunk_.find(ChunkEdits::ckey(cx, cz));
                                slot = it == byChunk_.end()
                                           ? std::make_shared<ChunkEdits>()
                                           : std::make_shared<ChunkEdits>(*it->second);
                            }
                            slot->vox[ChunkEdits::vkey(i, j, y)] = mat::AIR;
                            auto &span = slot->col[ChunkEdits::ckey(ni, nj)];
                            if (span.first == 0 && span.second == 0) span = {y, y + 1};
                            else {
                                span.first = mini(span.first, y);
                                span.second = maxi(span.second, y + 1);
                            }
                        }
                }
        std::vector<std::pair<int, int>> out;
        out.reserve(touched.size());
        for (auto &kv : touched) {
            byChunk_[ChunkEdits::ckey(kv.first.first, kv.first.second)] = kv.second;
            out.push_back(kv.first);
        }
        return out;
    }

    // Floor division: chunk -1 must hold voxel -1, not voxel 0.
    static int floorDiv(int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }

  private:
    mutable std::mutex mx_;
    std::unordered_map<uint64_t, std::shared_ptr<const ChunkEdits>> byChunk_;
};
// IS THIS MATERIAL STONE -- the question a pick asks before it bites.
// BEDROCK is deliberately NOT stone here: it is the floor of the world and
// nothing is meant to get through it. See mat::BEDROCK.
inline bool isStoneMat(uint8_t m) { return m == mat::ROCK; }

// IS THIS MATERIAL SOIL -- the question a shovel asks before it bites.
//
// THE LOOSE GROUND, AND EVERY RAMP THAT IS MADE OF IT. Grass, the soil under
// it, the needle litter a conifer stand drops, the shore sand and the silt
// below the water are one family to a shovel: they are what a blade moves
// rather than what a head breaks. That is also exactly the set the tool sounds
// already call silent (see toolsound.h) -- the same split, arrived at from the
// other side.
//
// THE RAMPS ARE ASKED THROUGH THEIR OWN PREDICATES rather than by comparing
// against STONE_0 and trusting the numbering never to move. mat::DIRT is in
// here too: the terrain does not currently place it, but it is the id anything
// building soil by hand would reach for, and a shovel that could not take it
// would be a trap for whatever does that next.
//
// ROCK AND BEDROCK ARE DELIBERATELY OUT. A shovel that took stone would be a
// pick, and the two exist to be different. See isStoneMat above.
inline bool isSoilMat(uint8_t m) {
    return m == mat::DIRT || m == mat::SAND || m == mat::SILT || isGrass(m) || isSoil(m) ||
           isLitter(m);
}

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

    // WHERE THE WATER SITS, AND IT HAS TO SIT INSIDE THE TERRAIN.
    //
    // This was 2.6 m and it produced NOT ONE WET COLUMN. Measured over a 2 km
    // square, the uncarved pine field runs:
    //
    //     min 20.6   p1 30.4   p5 35.2   p10 38.0   p25 42.8   med 48.5   max 74.6
    //
    // -- so a waterline at 2.6 m sat EIGHTEEN METRES BELOW THE LOWEST GROUND
    // ANYWHERE. Every previous attempt at water in this engine failed on that
    // one fact, and none of them were rendering bugs: there was no lake to draw
    // from any angle, so no shading or geometry could ever have shown one.
    //
    // 33 m is just under p5 -- read off the distribution rather than picked. It
    // catches the low country and leaves the wood above it dry.
    float waterLevel = 33.0f;  // metres -- THE PINE WOOD'S. See waterAt.

    // -----------------------------------------------------------------------
    // THE WATERLINE IS NOT ONE NUMBER FOR THE WORLD, and assuming it was
    // drowned half the map.
    //
    // The two woods have DISJOINT height ranges:
    //
    //     pine    min 20.6   p5 35.1   med 48.6   max 74.8
    //     birch   min  5.8   p5  9.4   med 13.2   max 19.7
    //
    // No single line serves both. One high enough to make lakes in the pine
    // wood sits TWENTY METRES over the birch wood's median and puts the whole
    // of it under; one that suits the birch is below the pine floor and is the
    // original "no lake anywhere" bug. Measured with the bands live, a global
    // 33 m put 52.56% of the world under water -- which reads in game as
    // "terrain is missing" plus "terrain that is completely flat", the flat
    // thing being the lake lying over it.
    //
    // So the line is asked PER COLUMN, off the same band function heightM
    // blends the two landforms with. AND THE SEAM COUNTS AS BIRCH: a column in
    // the 90 m blend is a hillside falling 35 m between two woods, the last
    // place a flat sheet of water belongs, so pure pine is required.
    // -----------------------------------------------------------------------
    static constexpr float kNoWater = -10000.0f;  // far under any terrain
    float waterAt(float x) const { return birchMix(x) <= 0.001f ? waterLevel : kNoWater; }

    // -----------------------------------------------------------------------
    // HOW FAR THIS COLUMN IS FROM THE BIRCH WOOD, AT A LAKE'S SCALE.
    //
    // waterAt is a STEP: pure pine gets a waterline, everything else gets none,
    // and the flip happens at one exact x. A lake that straddles that x is cut
    // in half along a perfectly straight north-south line, with its side wall
    // left standing in mid-air -- which is what "the water appears to be cut
    // off" is. Measured: steps at x = -2313, -1686, -713, -86.5, 887, 1513,
    // 2487 m, and 4.14% of every wet column in the world sits within five
    // metres of one.
    //
    // A MARGIN ALONE ONLY MOVES THE CUT. Refusing water within R of the step
    // puts a new step at R, and a basin sitting on THAT is sliced exactly the
    // same way. The cut has to land where there is no lake to cut.
    //
    // So this fades the basin CARVE instead. Basins near the seam get shallower
    // and shallower until they no longer reach the waterline at all, so the
    // water simply runs out before the step rather than being sliced by it --
    // and the terrain stays continuous, because a fading carve is a fading
    // carve rather than an edge.
    //
    // SAMPLED AT +-R SO A BASIN CANNOT STRADDLE THE FADE EITHER: R is larger
    // than the biggest lake measured (134 m across), so by the time the fade
    // starts biting, every column of any basin that could hold water is inside
    // it together.
    float lakeSeamR = 150.0f;
    float seamClear(float x) const {
        const float b = maxf(maxf(birchMix(x - lakeSeamR), birchMix(x)),
                             birchMix(x + lakeSeamR));
        return 1.0f - sstep(saturate(b * 20.0f));
    }

    // ---------------------------------------------------------------------
    // THE BASIN FIELD -- where a lake is ALLOWED to be.
    //
    // Named, not inline, because they are three SEPARATE levers -- area, rim
    // slope and depth -- that were being tuned as one and fighting each other.
    //
    //     config                  wet%   bodies  meanD   dry>1m%   dryMax
    //     old (0.0160/0.40)       0.00%       0      --     0.69%   10.1 m
    //     abandoned (18e36ff)     0.07%      11    1.56    17.14%   20.3 m
    //     this (0.0016/0.22)      2.66%      71    2.50     0.19%    5.2 m
    //
    // THE THRESHOLD IS READ OFF THE FIELD'S OWN DISTRIBUTION and cannot be
    // borrowed. v1 uses BASIN_T = 0.065, which fires on NOTHING here: this fbm
    // bottoms out at 0.147 and its p20 is 0.345, so the old 0.40 was carving a
    // fifth of the world and v1's 0.065 would carve none of it. 0.22 is p2.
    //
    // THE FREQUENCY WAS THE REAL BUG: 0.0160 with four octaves makes the mask
    // the POCKETS OF A DETAIL-SCALE SUM, firing in every dip on every hillside
    // instead of at landform scale. Then the gate has to reach up to catch real
    // low ground, and widening the gate is what drags hilltops down.
    //
    // THE GATE IS ABSOLUTE, AND THAT IS DELIBERATE. It was a fraction of the
    // landform's range dressed as metres, rescaled every time the terrain grew
    // -- and rescaling it upward is precisely the move that deletes hills. It
    // is pinned 14 m over the waterline instead. Anything that moves the floor
    // must move this WITH it, by hand; v1 carries the same warning on BASIN_LOW.
    float basinFreq = 0.0016f;      // base frequency of the basin noise
    int basinOct = 3;               // octaves under it
    // 0.22 -> 0.30, AND ONLY BECAUSE THE WATER IS GATED ON THIS NOW. While
    // "under the line" meant "wet", 0.22 was already producing an ocean off the
    // terrain's own low country and the threshold could not be touched. With
    // lakeColumn requiring the mask, this number sets LAKE COUNT directly:
    // measured over 36 km2, 0.22 -> 4 lakes, 0.26 -> 13, 0.30 -> 24. Dry-land
    // loss over 1 m goes 0.11% -> 0.38% -> 0.74%, against the 17.14% that killed
    // the abandoned attempt at 18e36ff -- so this is bought cheaply, but it IS
    // the number that starts dragging hills if it keeps rising.
    float basinT = 0.30f;           // AREA: the fraction of the field that carves at all
    float basinRamp = 0.05f;        // RIM: how far below T the pull reaches full strength
    float basinGate = 47.0f;        // absolute ceiling, in metres, over which no basin cuts
    float basinGateRamp = 14.0f;    // and how softly that ceiling closes
    // -3 -> -9: the bed sits nine metres under the line rather than three. A
    // basin whose floor stops just under the waterline floods a metre deep and
    // reads as a puddle; measured, this takes the deepest lakes from 6.0 m to
    // 9.3 m and the largest from 4,428 m2 to 6,804 m2 for almost no extra
    // dry-land loss (0.72% -> 0.74%), because it deepens basins that were
    // already carving rather than creating new ones.
    float basinBed = -9.0f;         // DEPTH: the bed, as an offset from waterLevel

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

        const float b =
            fbm(memo.basin, x * basinFreq + 311.7f, z * basinFreq + 157.3f, basinOct);
        if (b < basinT) {
            const float m = sstep(minf(1.0f, (basinT - b) / basinRamp));
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
            const float lowGate = saturate((basinGate - h) / basinGateRamp);
            // ...and fades out as the birch band comes within a lake's reach --
            // see seamClear. This is what stops a lake being sliced by the
            // waterline's step instead of ending on its own rim.
            h -= m * lowGate * seamClear(x) * (h - (waterLevel + basinBed));
        }

        // -------------------------------------------------------------------
        // A FLUSH SHELF IS AN ARTEFACT, and v1 carries this exact fix.
        //
        // The basin's rim pulls a broad apron down to within a voxel or two of
        // the line, and every column that lands there lands at the SAME
        // altitude. Measured on a transect through the lake at (-88, -104):
        // thirty metres of beach varying by thirty centimetres. Under grass and
        // litter a dead-level plateau is invisible; on bare sand it is a
        // staircase of perfectly straight treads, which is what the first
        // headless render of this beach actually showed.
        //
        // v1 (world/terrain.js, fillColumn) hit it on arctic snow and fixed it
        // the same way: give the shelf the coherent relief its own ground
        // carries, UPWARD ONLY. The max(0, ...) is not a detail -- it is what
        // preserves the guarantee the whole basin arm exists for, which is that
        // dry land never sits below the water plane. Raising a column can only
        // ever make it drier, so this cannot move a shoreline outward or strand
        // a lake column above the line.
        //
        // GATED AT OR ABOVE THE LINE so a lake bed is never touched: those
        // columns are what holds the water, and lifting one is how a lake
        // acquires a hole in the middle.
        {
            const float wlm = waterAt(x);
            if (wlm > 0.0f && h >= wlm - 0.05f && h <= wlm + 0.8f)
                h += maxf(0.0f, (fbm(memo.shelf, x * 0.09f + 3.1f, z * 0.09f + 8.7f, 3) - 0.42f) *
                                    0.9f);
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

    // HOW FAR DOWN THE STONE GOES BEFORE THE BEDROCK STARTS, in voxels, from
    // each column's own surface. 100 voxels is 10 m at VOXEL_M -- deep enough
    // that digging through it is an undertaking, shallow enough to be reachable
    // at all. If you meant a hundred METRES, this is the one number to change.
    int kBedrockVox = 100;

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

        const int wl = int(waterAt(wx(i)) / VOXEL_M);
        // -------------------------------------------------------------------
        // NO BEACH WITHOUT A BASIN TO HOLD THE WATER.
        //
        // Sand used to be painted on anything within a few voxels of the
        // waterline, which was right while every column under the line was wet.
        // It is not any more: water needs a BASIN now (see lakeColumn), and
        // without the same gate here the low country came out ringed with sandy
        // shores around nothing at all -- "areas that are empty of water even
        // though they have a sandy basin". A beach is the edge of a lake, so it
        // is allowed exactly where a lake is.
        // -------------------------------------------------------------------
        // The gate itself. Everything from here to the end of the beach block
        // is SKIPPED on a column no lake can reach, and the ordinary ground
        // arms below it run instead -- which is what a dry hollow should be
        // made of.
        // THE HEIGHT TEST FIRST, AND IT IS NOT A MICRO-OPTIMISATION. inBasin is
        // three octaves of 2D fbm and this runs on EVERY column in the world,
        // where the question it answers only matters within a few voxels of the
        // waterline -- a fraction of a percent of them. Asking the cheap integer
        // question first keeps the noise off every hillside in the wood.
        const bool basin = (wl >= 0) && (h <= wl + 7) && inBasin(i, j, memo);
        if (basin) {
        // UNDER THE LINE: sand near it, silt where it gets deep. A lakebed you
        // can see through the water is sand; further down it stops mattering and
        // silt is the darker, duller answer.
        if (h <= wl) return (wl - h <= 8) ? mat::SAND : mat::SILT;

        // -------------------------------------------------------------------
        // THE BEACH -- v1's, ported, and it is the DITHER that is being ported
        // rather than the sand.
        //
        // This was `h <= wl + 8`, one hard band that ended on a line, and a
        // straight contour of sand meeting forest floor is the one shape nothing
        // else in this terrain has. v1 (world/terrain.js, fillColumn) splits it:
        //
        //     solid sand      h1 <= WL + 4      -- five voxel steps
        //     soft edge       h1 <= WL + 6      -- dithered, thinning out
        //
        // FIVE STEPS BECAUSE A STEP IS A VOXEL (v1, user: "make the sand have
        // more steps.. 3-5"). The count of treads you can see is the number of
        // voxel levels the sand spans; two levels read as a single step, which
        // is what made v1's first beach look like a kerb.
        //
        // AND THE BLEND SITS ABOVE THE SAND, NEVER THROUGH IT. v1 learned this
        // from a screenshot of a beach speckled black: the dither used to start
        // inside the beach, so every MISS fell through to the litter arm below
        // and painted needle brown across most of a wide beach. The solid band
        // returns unconditionally here for exactly that reason -- nothing inside
        // the beach can miss.
        // -------------------------------------------------------------------
        if (h <= wl + 5) return mat::SAND;    // the beach proper: five steps, solid
        if (h <= wl + 7) {
            // Two levels of soft edge. The probability falls with height, so the
            // sand thins out INTO the forest floor with no line at either end --
            // sstep rather than linear so both ends of the fade are corner-free.
            //
            // THE DIVISOR IS THREE FOR A BAND OF TWO, and that is not an error.
            // Over two it reaches 1.0 exactly at the top level, whose sand
            // probability is then zero -- measured 48.3% at wl+6 and 0.0% at
            // wl+7, so the band dithered on one level and ended on a hard line
            // at the other, which is the artefact this whole arm exists to
            // remove. Three puts both levels strictly inside the fade: 74% then
            // 26%.
            const float t = saturate(float(h - wl - 5) / 3.0f);
            if (hashUnit(0x5A17u, hashU32(uint32_t(i), uint32_t(j))) < 1.0f - sstep(t))
                return mat::SAND;
        }
        }   // ...and the end of the basin gate.

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

    // -----------------------------------------------------------------------
    // IS THIS COLUMN UNDER WATER -- v1's own rule, ported.
    //
    // v1 (world/terrain.js, fillColumn):
    //     lake = h <= WL - 1 || (h === WL && any neighbour h < WL)
    //
    // EVERY BOUND SHIFTS BY ONE COMING ACROSS, because the two engines mean
    // different things by h: v1's is one PAST the top solid voxel and v2's IS
    // the top solid voxel. So v1's `h <= WL - 1` is `H <= wl - 2` here. Getting
    // that wrong is a one-voxel film of water over every shore, which is the
    // coplanar-with-the-bank artefact the second clause exists to avoid.
    //
    // THE SECOND CLAUSE IS WHY SHORES LOOK LIKE SHORES. A column sitting
    // exactly one voxel under the line is wet only if it JOINS real water --
    // otherwise a puddle appears in every isolated dip that happens to graze
    // the waterline, and a wood full of one-voxel puddles reads as wet paint.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE SURFACE IS MADE OF VOXELS, NOT OF GLASS.
    //
    // A lake was one flat merged quad at a single row, which in a world built
    // of 10 cm voxels reads as a pane of glass laid over the ground -- the one
    // surface in the wood with no voxel structure in it at all.
    //
    // This is v1's own Gerstner height field (render/buffers.js, GERSTH_WGSL --
    // the same four trains the shading normal uses, see waterNormal in
    // Trace.cs.slang), QUANTISED TO WHOLE VOXELS. The crests come out as steps
    // because everything here is steps, and the 2D merge below picks them up for
    // free: it already merges columns of EQUAL waterline, so a stepped surface
    // becomes a handful of rectangles at different heights instead of one.
    //
    // IT RAISES, NEVER LOWERS. Dropping a column would expose the bed through
    // the surface and put a hole in the lake; adding to it only ever makes the
    // water deeper, which nothing downstream minds.
    //
    // AND IT IS STILL AT ONE MOMENT IN TIME. The mesh is built when a chunk is
    // adopted and does not move afterwards, so these are FROZEN crests -- the
    // shape of water, not the motion of it. Animating them means re-meshing the
    // water every frame, which means water needs its own acceleration structure
    // again; the shading normal carries the movement in the meantime.
    int waveCrestVox(float x, float z) const {
        if (waveCrestMax <= 0) return 0;
        // v1's GW table: direction, wavelength (m), amplitude (voxels).
        struct W { float dx, dz, lam, amp; };
        // ONLY THE TWO LONG TRAINS CARRY GEOMETRY, and this is a cost decision
        // with a measurement behind it. All four gave 422,810 water triangles
        // against 7,618 flat -- 55x -- because the finest train is 0.67 m, under
        // seven voxels, so the crest changes every few columns and the 2D merge
        // has nothing left to merge. The 5.2 m and 2.3 m trains step over metres
        // instead, which merges into broad rectangles AND reads better: long
        // swells rather than chop. The fine detail is not lost -- it lives in the
        // shading normal, which is per pixel and costs no triangles at all.
        static const W kW[2] = {{ 0.834f,  0.552f, 5.20f, 1.30f},
                                {-0.416f,  0.909f, 2.30f, 0.72f}};
        static const float kPh[2] = {0.0f, 2.1f};
        float hh = 0.0f;
        for (int i = 0; i < 2; ++i) {
            const float k = 6.2831853f / kW[i].lam;
            hh += kW[i].amp * cosf(k * (kW[i].dx * x + kW[i].dz * z) + kPh[i]);
        }
        // The sum runs about +-2.5 voxels; shift it positive and clamp, so the
        // trough is the true waterline and the crests stand above it.
        const int c = int(hh + 2.0f);   // the two trains sum to about +-2
        return c < 0 ? 0 : (c > waveCrestMax ? waveCrestMax : c);
    }
    int waveCrestMax = 3;   // how many voxels a crest may stand above the line

    int waterRowAt(float x) const {
        const float w = waterAt(x);
        return (w <= 0.0f) ? -1 : int(w / VOXEL_M);   // birch returns kNoWater
    }

    // THE BASIN FIELD, ASKED DIRECTLY. heightM uses it to decide where to carve;
    // the water needs the same answer to decide where it is ALLOWED to stand.
    float basinField(float x, float z, TerrainMemo &memo) const {
        return fbm(memo.basin, x * basinFreq + 311.7f, z * basinFreq + 157.3f, basinOct);
    }
    bool inBasin(int i, int j, TerrainMemo &memo) const {
        return basinField(wx(i), wx(j), memo) < basinT;
    }

    // -----------------------------------------------------------------------
    // A LAKE HAS TO BE IN A BASIN, AND WITHOUT THIS IT IS AN OCEAN.
    //
    // "Below the waterline" is NOT the same question as "in a lake", and
    // treating it as one is what put an endless sheet of water across the wood.
    // Measured over a 10 km square: 2.68% of pine columns sat under the line,
    // and only **5.0% of them were anywhere the basin mask had fired**. The
    // other 95% was plain low ground -- and the lowest few percent of a fractal
    // landscape is not a scatter of ponds, it is the valley network, which
    // connects. The largest body came out 22,750 m2 and they run into each
    // other; from inside it reads as a coastline.
    //
    // THE PINE FIELD MAKES THIS INEVITABLE WITHOUT A GATE: its p5 is 35.2 m
    // against a 33 m line, so a few percent of the world floods on the terrain's
    // own shape no matter what the basin carve does.
    //
    // AND IT IS ALSO THE FIX FOR THE BIRCH BAND LOOKING DROWNED. `waterAt`
    // already gives birch no water (measured 0.00% wet), but 100% of birch
    // ground lies below the PINE line -- so a pine lake that reached the band
    // edge stood as a wall of water over ground twenty metres lower. A basin is
    // a bounded depression that has to close on its own rim, so it cannot run
    // to the seam and end in mid-air.
    // -----------------------------------------------------------------------
    bool lakeColumn(int i, int j, int h, TerrainMemo &memo) const {
        const int wl = waterRowAt(wx(i));
        if (wl < 0) return false;
        if (h > wl - 1) return false;               // cheap reject before the noise
        if (!inBasin(i, j, memo)) return false;     // low ground is not a lake
        if (h <= wl - 2) return true;
        // A rim column is wet only if it JOINS real water -- otherwise a puddle
        // appears in every dip that grazes the line.
        return heightVox(i - 1, j, memo) <= wl - 2 || heightVox(i + 1, j, memo) <= wl - 2 ||
               heightVox(i, j - 1, memo) <= wl - 2 || heightVox(i, j + 1, memo) <= wl - 2;
    }

    // -----------------------------------------------------------------------
    // THE CARVE -- and this is the change that makes the world a VOLUME.
    //
    // Solidity used to be `y <= h`: one comparison against a surface. A
    // comparison against a surface cannot describe a room under it, which is
    // why this world has never had a cave, an overhang or an arch that the
    // generator put there -- only ones a player dug. The height field is not
    // wrong, it is just 2.5D, and every previous attempt to get caves out of it
    // was arguing with that rather than replacing it.
    //
    // TUBES, NOT BLOBS, AND THAT IS THE WHOLE TRICK. A 3D fbm thresholded
    // directly gives spongework -- isolated bubbles that never connect, which
    // reads as rot rather than as a cave. |fbm - 0.5| small instead picks out
    // the field's MID-SURFACE, which is a connected sheet; intersect two such
    // sheets from decorrelated fields and what survives is their intersection,
    // which is a LINE. That is a tunnel, it branches where the sheets fold, and
    // it connects over hundreds of metres for free.
    //
    // TWO GATES, AND THEY EXIST FOR DIFFERENT REASONS. caveRegion is 2D and
    // CHEAP and says where a cave system is at all -- perhaps a tenth of the
    // world -- so nine columns in ten never evaluate the 3D field. That is a
    // performance gate. The roof and floor fades are not: they keep a tunnel
    // from shaving the topsoil off a hillside from underneath, and from cutting
    // into the bedrock that is supposed to be the floor of the world.
    //
    // THE ROOF FADE IS DELIBERATELY LEAKY. A cave system that never reaches the
    // surface is a cave system nobody finds. Where the region field is strong
    // the roof requirement relaxes to nothing, so the strongest systems open
    // their own mouths on a hillside and the rest stay sealed.
    // -----------------------------------------------------------------------
    float caveRegionFreq = 0.0035f;  // where cave SYSTEMS are, in x/z
    float caveRegionT = 0.610f;      // over this, a column may hold one
    float caveRegionMouth = 0.88f;   // ...and over THIS, it may break the surface
    float caveFreq = 0.020f;         // the tube field, horizontally
    // THIS MUST STAY CLOSE TO caveFreq, AND HERE IS THE FAILURE IT PREVENTS.
    //
    // At 0.034 (above the horizontal frequency) tunnels come out vertically
    // THIN: a mean bore of 6.8 voxels, 68 cm against a player 18 tall. A crack.
    // The obvious correction is to drop it a long way -- 0.013 was tried -- and
    // that is WRONG in a way no average catches. The carve is the intersection
    // of two sheets, and a sheet only stays a sheet while the field varies in y:
    // over the 90-voxel band 0.013 spans barely one lattice unit, so BOTH sheets
    // go vertically invariant, and the intersection of two vertical sheets is a
    // PRISM, not a tube. Columns were hollowed from the surface to the bedrock
    // -- one measured 40+ voxels of unbroken air under the turf, and its chunk
    // went 74,596 triangles to 484,236.
    //
    // THE MEAN BORE LOOKED FINE THROUGHOUT (15.5, better than 6.8). What catches
    // it is the TAIL: p99 of the carved runs, which went to the full band. Sweep
    // p99run and maxrun, never the mean alone.
    //
    // Isotropic with caveFreq keeps the two sheets transverse, which is the only
    // reason their intersection is a line. Measured here: p99 run 39 voxels,
    // breach 0.36%, 2.21% of the band carved.
    float caveFreqY = 0.020f;        // ...and vertically, at the SAME rate: transverse sheets cut tubes
    int caveOct = 3;
    float caveT = 0.062f;            // how near the mid-surface still counts -- the bore
    int caveRoofVox = 7;             // solid left under the surface, where sealed
    int caveFloorVox = 10;           // ...and over the bedrock
    // OFF (user 2026-09-10: "remove caves from the terrain").
    //
    // The whole carve is kept rather than deleted, behind this one flag, because
    // it is not the code that was the problem -- it was tuned on measurement and
    // the numbers are in [[v2-voxel-volume-and-caves]]: 14.3% of columns holding
    // cave, 0.36% breaching, p99 bore 39 voxels, and a mesh cost brought from
    // 385 ms a chunk down to 46. Setting this true restores all of it.
    //
    // With it false, `caved` returns false everywhere, the mesher's carve pass
    // is skipped entirely, and no column leaves the fast heightmap path on its
    // account -- so the terrain is exactly what it was before caves existed, and
    // costs exactly what it did.
    bool cavesOn = false;

    // Cheap, 2D, and the only thing most columns ever pay.
    float caveRegion(int i, int j, FbmMemo &m) const {
        return fbm(m, float(i) * caveRegionFreq + 613.1f, float(j) * caveRegionFreq + 271.9f, 3);
    }
    float caveRegion(int i, int j) const {
        FbmMemo m;
        return caveRegion(i, j, m);
    }

    // Is the voxel at (i, j, y) cut out of the volume? `reg` is that column's
    // caveRegion, handed in because a caller walking a column has it already.
    // The gates alone: integer rules about depth, no noise. Kept separate so the
    // sampled path in the mesher can apply them EXACTLY while interpolating only
    // the field -- see caveField.
    bool cavedGates(int i, int j, int y, int h, float reg) const {
        if (!cavesOn || reg <= caveRegionT) return false;
        if (h - y >= kBedrockVox - caveFloorVox) return false;  // never into the bedrock
        // How much solid the roof owes, relaxed to nothing in the strongest
        // systems so those open a mouth. sstep so the change is gradual across
        // the region rather than a ring of columns where the rule flips.
        const float open = saturate((reg - caveRegionT) / maxf(1e-4f, caveRegionMouth - caveRegionT));
        const int roof = int(float(caveRoofVox) * (1.0f - sstep(open)) + 0.5f);
        return h - y >= roof;
    }

    bool caved(int i, int j, int y, int h, float reg) const {
        if (!cavedGates(i, j, y, h, reg)) return false;
        return caveField(i, j, y) < caveT;
    }

    // HOW FAR THIS VOXEL IS FROM THE TUBE'S CENTRE LINE -- the continuous form
    // of the test above, so it can be SAMPLED AND INTERPOLATED rather than
    // evaluated per voxel. That is the whole performance story of this feature:
    // measured, `caved` at one call per voxel was 228 ms of a cave-heavy chunk
    // against 6 ms for the heights and 1 ms for the region gate.
    //
    // WHY INTERPOLATING IS SOUND HERE and is not merely cheaper. The field is
    // band-limited by construction: the finest octave has a wavelength of about
    // twelve voxels horizontally and nineteen vertically, so a four-voxel step
    // samples it several times per period. Interpolating between those samples
    // reconstructs it to well inside the noise's own amplitude, and what it
    // feeds is a THRESHOLD -- a bore edge moving by a fraction of a voxel is
    // not a difference anyone can see in a cave wall.
    //
    // THE EARLY-OUT SURVIVES, which matters because it is most of the saving on
    // the 87% of voxels that are not carved: max(a, b) >= a, so if the first
    // sheet alone is already past the threshold the second cannot bring the
    // maximum back under it, and returning `a` is a correct answer to the only
    // question asked of this value.
    float caveField(int i, int j, int y) const {
        const float fx = float(i) * caveFreq, fy = float(y) * caveFreqY, fz = float(j) * caveFreq;
        const float a = std::fabs(fbm3(fx, fy, fz, caveOct) - 0.5f);
        if (a >= caveT) return a;   // the cheaper sheet first: it rejects most voxels alone
        const float b = std::fabs(fbm3(fx + 41.7f, fy + 13.1f, fz + 77.3f, caveOct) - 0.5f);
        return maxf(a, b);
    }

    // How far apart the samples above are taken, down a column. Four is a
    // quarter of the work and the reconstruction is still several samples per
    // period of the finest octave; eight starts to round the bores off.
    int caveStrideY = 4;

    // Does this column hold ANY carved voxel? The mesher asks so it can route
    // the column to the voxel pass -- the heightmap path assumes solid from h
    // down and would draw a cave's roof as if the rock under it were still
    // there. Walks at a stride: the tube field's finest octave has a wavelength
    // of about fifteen voxels, so a stride of four cannot step over a bore.
    bool columnHasCave(int i, int j, int h, float reg) const {
        if (!cavesOn || reg <= caveRegionT) return false;
        const int lo = maxi(0, h - kBedrockVox + caveFloorVox);
        for (int y = h - caveRoofVox; y >= lo; y -= 4)
            if (caved(i, j, y, h, reg)) return true;
        return false;
    }

    // The material of the voxel at row y in column (i, j), given that column's
    // surface row h and what topMaterial put on it. AIR above the surface; the
    // surface keeps its own material; and a rock outcrop is rock the whole way
    // down rather than rock sitting on soil.
    // THREE LAYERS AND A SURFACE, top to bottom:
    //
    //   y == h                     the surface, whatever topMaterial chose
    //   within crustVox of it      SOIL_0 -- the dirt the grass is rooted in
    //   down to kBedrockVox        ROCK -- the stone the world is made of
    //   below that                 BEDROCK -- the floor, and nothing under it
    //
    // The depth is measured from the COLUMN'S OWN SURFACE, not from a fixed
    // altitude, so the bedrock follows the terrain rather than cutting across
    // it: a valley floor and a hilltop are both the same distance from it. A
    // flat bedrock plane would surface itself in the valleys.
    // The profile with NO carve -- what the column would hold if it were solid.
    // Split out because the mesher decides the carve ONCE into a bitset and must
    // not pay for it again on every neighbour query; everything else asks
    // materialAt, which is carve-aware and is still the one answer.
    uint8_t materialSolid(int i, int j, int y, int h, uint8_t top) const {
        if (y > h) return mat::AIR;
        if (h - y >= kBedrockVox) return mat::BEDROCK;
        if (y == h) return top;
        if (top == mat::ROCK) return mat::ROCK;
        return (h - y <= crustVox(i, j)) ? mat::SOIL_0 : mat::ROCK;
    }

    uint8_t materialAt(int i, int j, int y, int h, uint8_t top) const {
        if (y > h) return mat::AIR;
        if (h - y >= kBedrockVox) return mat::BEDROCK;
        // THE CARVE IS ASKED BEFORE THE SURFACE, so a tunnel that reaches a
        // hillside takes the turf with it and leaves a mouth, rather than a
        // roof of floating grass over a hollow.
        if (cavesOn && caved(i, j, y, h, caveRegion(i, j))) return mat::AIR;
        return materialSolid(i, j, y, h, top);
    }

    uint8_t topMaterial(int i, int j, int h, TerrainMemo &memo) const {
        // Only reached from the sparse scatter paths. Below the shore band the
        // slope is never consulted, so it is not worth four height evaluations
        // to compute one that will be discarded.
        const int wl = int(waterAt(wx(i)) / VOXEL_M);
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
    VoxMesh meshChunk(int cx, int cz, ChunkScratch &scratch,
                      const ChunkEdits *ed = nullptr) const {
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
        scratch.crust.resize((size_t(n) + 2) * (size_t(n) + 2));
        uint8_t *const cp = scratch.crust.data();
        auto CR8 = [&](int i, int j) -> uint8_t & {
            return cp[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        for (int j = -1; j <= n; ++j)
            for (int i = -1; i <= n; ++i) {
                const int slope =
                    maxi(absi(H(i + 1, j) - H(i - 1, j)), absi(H(i, j + 1) - H(i, j - 1)));
                T(i, j) = topMaterial(I0 + i, J0 + j, H(i, j), slope, memo);
                CR8(i, j) = uint8_t(mini(255, crustVox(I0 + i, J0 + j)));
            }

        // How tall a strand stands on each column, 0 for none. Computed for the
        // padded grid so a column on the edge can still ask its neighbours.
        // WRITTEN ON EVERY PATH, including the two that used to `continue` and
        // leave the value alone. That is what lets the grid be reused across
        // chunks without a fill: a stale row from the last chunk is overwritten
        // rather than inherited, and the zero goes into a cache line this loop
        // is touching anyway instead of into a separate 66 KB memset.
        // ------------------------------------------------------------------
        // WHICH COLUMNS LEAVE THE FAST PATH.
        //
        // A column that has been dug cannot be described by a height any more,
        // so it is meshed voxel by voxel below. ITS NEIGHBOURS GO WITH IT: the
        // rock beside a hole has a face pointing INTO that hole, and the
        // heightmap pass cannot know the hole is there. Meshing the ring as
        // well is what stops a dig leaving a window through the world.
        //
        // Everything else -- which is to say all of it, in a world nobody has
        // touched -- keeps the run-merged heightmap path unchanged.
        // ------------------------------------------------------------------
        std::unordered_set<uint64_t> slow;
        int yEditLo = 0, yEditHi = 0;
        if (ed && !ed->col.empty()) {
            bool first = true;
            for (const auto &kv : ed->col) {
                const int wi = int(int32_t(uint32_t(kv.first >> 21) & 0x1fffffu) << 11) >> 11;
                const int wj = int(int32_t(uint32_t(kv.first) & 0x1fffffu) << 11) >> 11;
                if (first) { yEditLo = kv.second.first; yEditHi = kv.second.second; first = false; }
                else { yEditLo = mini(yEditLo, kv.second.first); yEditHi = maxi(yEditHi, kv.second.second); }
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                        slow.insert(ChunkEdits::ckey(wi - I0 + di, wj - J0 + dj));
            }
        }
        // ------------------------------------------------------------------
        // AND THE COLUMNS A CAVE RUNS THROUGH, for exactly the reason above.
        //
        // The heightmap path can only say "solid from h down", so over a cave
        // it draws the roof as though the rock under it were still there --
        // the tunnel exists in the volume, in the collider and in every query,
        // and is simply invisible. Same failure as an un-meshed dig, same fix:
        // route the column to the voxel pass.
        //
        // THE RING GOES WITH IT, again. A tunnel wall is a face on the rock
        // BESIDE the carved column, and that rock may be a column the fast path
        // owns -- or one in the chunk next door, which is why the padded range
        // is walked rather than the interior.
        //
        // WHAT THIS COSTS is bounded by the region gate, not by the cave field:
        // measured at 11.4% of columns holding a carve, so seven columns in
        // eight keep the run-merged path that makes this terrain affordable.
        // ------------------------------------------------------------------
        // ------------------------------------------------------------------
        // THE WATER, AS VOXELS, AND WHY IT GOES THROUGH THE SAME PASS.
        //
        // A lake is rows of mat::WATER standing above the ground in a basin, so
        // the columns holding it cannot be described by "solid up to h" any more
        // than a cave can -- the fast path would draw the lakebed and stop. They
        // go to the voxel pass, which already emits a face wherever material
        // borders air and merges the sides into runs, so a lake's top comes out
        // as faces on its surface row and its interior costs nothing.
        //
        // THIS IS THE WHOLE OF WHAT REPLACED THE SEPARATE WATER STRUCTURE. No
        // second BLAS, no KIND_WATER instance, no second entry in the compaction
        // queue under the same chunk key -- which is the collision that put a
        // flat sheet where a hillside should be.
        // ------------------------------------------------------------------
        scratch.wet.assign(size_t(n + 2) * size_t(n + 2), int16_t(-1));
        int16_t *const wp = scratch.wet.data();
        auto WET = [&](int i, int j) -> int16_t & {
            return wp[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        {
            bool anyWet = false;
            for (int j = -1; j <= n; ++j)
                for (int i = -1; i <= n; ++i) {
                    const int wl = waterRowAt(wx(I0 + i));
                    if (wl < 0) continue;                 // the birch wood has no line
                    if (H(i, j) > wl - 1) continue;       // well clear of it: cheap reject
                    if (!lakeColumn(I0 + i, J0 + j, H(i, j), memo)) continue;
                    // The crest is added to the SURFACE only. lakeColumn above
                    // decided wetness against the true waterline, so a crest can
                    // never create a lake -- only stand on one.
                    WET(i, j) = int16_t(wl + waveCrestVox(wx(I0 + i), wx(J0 + j)));
                    anyWet = true;
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di)
                            slow.insert(ChunkEdits::ckey(i + di, j + dj));
                }
            (void)anyWet;
        }

        // THE CARVE IS DECIDED ONCE, HERE, AND READ AS BITS AFTER THIS.
        // caveRegion is 3 octaves of 2D fbm and `caved` up to two of 3D fbm;
        // asking them from inside the face loop meant paying both about seven
        // times per voxel, which measured 385 ms a chunk against 10.6 with
        // caves off. One pass down each column, one bit out, and the mesh loop
        // below touches no noise at all.
        const int caveBand = maxi(0, kBedrockVox - caveFloorVox);
        const size_t caveWords = size_t((caveBand + 63) / 64);
        const size_t caveStride = caveWords;
        auto CV = [&](int i, int j) -> uint64_t * {
            return scratch.carve.data() +
                   (size_t(j + 1) * size_t(n + 2) + size_t(i + 1)) * caveStride;
        };
        // k counts DOWN from the column's own surface, so a band that follows
        // the terrain costs 90 bits a column instead of the chunk's whole relief.
        auto carvedAt = [&](int i, int j, int y) -> bool {
            if (!cavesOn || i < -1 || j < -1 || i > n || j > n) return false;
            const int k = H(i, j) - y;
            if (k < 0 || k >= caveBand) return false;
            return ((CV(i, j)[size_t(k >> 6)] >> (k & 63)) & 1ull) != 0ull;
        };
        if (cavesOn) {
            scratch.carve.assign(size_t(n + 2) * size_t(n + 2) * caveStride, 0ull);
            FbmMemo rm;
            for (int j = -1; j <= n; ++j)
                for (int i = -1; i <= n; ++i) {
                    // The cheap gate, and most columns stop on it. Walking i
                    // fastest keeps the memo's lattice row warm across the row.
                    const float reg = caveRegion(I0 + i, J0 + j, rm);
                    if (reg <= caveRegionT) continue;
                    const int h = H(i, j);
                    uint64_t *const w = CV(i, j);
                    bool any = false;
                    // Sample the field down the column on the stride, then walk
                    // every voxel between two samples off the interpolation.
                    // The roof, floor and bedrock gates stay EXACT -- they are
                    // integer rules about depth, not noise, and rounding one of
                    // them would let a tunnel shave the turf off a hillside.
                    const int st = maxi(1, caveStrideY);
                    float f0 = caveField(I0 + i, J0 + j, h);
                    for (int k0 = 0; k0 < caveBand; k0 += st) {
                        const int k1 = mini(k0 + st, caveBand);
                        const float f1 = caveField(I0 + i, J0 + j, h - k1);
                        const float inv = 1.0f / float(k1 - k0);
                        for (int k = k0; k < k1; ++k) {
                            const float f = f0 + (f1 - f0) * (float(k - k0) * inv);
                            if (f >= caveT) continue;
                            if (!cavedGates(I0 + i, J0 + j, h - k, h, reg)) continue;
                            w[size_t(k >> 6)] |= 1ull << (k & 63);
                            any = true;
                        }
                        f0 = f1;
                    }
                    if (!any) continue;
                    // THE RING GOES WITH IT: a tunnel wall is a face on the
                    // rock BESIDE the carved column, and that rock may be one
                    // the fast path owns.
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di)
                            slow.insert(ChunkEdits::ckey(i + di, j + dj));
                }
        }
        auto ED = [&](int i, int j) { return !slow.empty() && slow.count(ChunkEdits::ckey(i, j)) != 0; };

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
                if (ED(i, j)) { ++i; continue; }   // voxel-meshed below
                const int hc = H(i, j);
                const uint8_t tm = T(i, j);
                int k = i + 1;
                while (k < n && !ED(k, j) && H(k, j) == hc && T(k, j) == tm) ++k;

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
                const int rows = ED(i, j) ? 0 : SR(i, j);
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
                    if (ED(i, j)) { ++inner; continue; }   // voxel-meshed below
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
                        if (ED(i2, j2) || H(i2, j2) != hc || H(i2 + di, j2 + dj) != nb ||
                            T(i2, j2) != tm ||
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
                        // ROCK, THEN BEDROCK. The same split materialAt
                        // makes, walked in runs: stone from the crust down to
                        // kBedrockVox below this column's surface, and the
                        // floor of the world under that. A bank deep enough to
                        // reach it shows it, which is the only way it is ever
                        // seen until something digs.
                        const int rockLo = maxi(nb + 1, hc - kBedrockVox + 1);
                        if (cursor > rockLo) {
                            sideBand(m, i, j, d, rockLo, cursor, mat::ROCK, run);
                            cursor = rockLo;
                        }
                        if (cursor > nb + 1)
                            sideBand(m, i, j, d, nb + 1, cursor, mat::BEDROCK, run);
                    }
                    inner = k;
                }
            }
        }

        // ------------------------------------------------------------------
        // THE VOXEL PASS -- the only place in this mesher that walks y.
        //
        // The heightmap above can say "this column is solid up to h" and
        // nothing else, which is exactly why carving never worked: a bite out
        // of a cliff is an OVERHANG, and a height cannot describe one. So the
        // handful of columns a swing touched are meshed the honest way --
        // voxel by voxel, a face wherever solid meets air -- and every other
        // column in the chunk keeps the run-merged path that makes this
        // terrain affordable at all.
        //
        // This is the "store the volume, draw only the surface" rule in one
        // loop: the material comes from materialAt (or the edit that covers
        // it), and geometry appears only where that material borders air.
        // ------------------------------------------------------------------
        if (!slow.empty()) {
            auto matAt = [&](int i, int j, int y) -> uint8_t {
                uint8_t e;
                if (ed && ed->voxel(I0 + i, J0 + j, y, &e)) return e;
                if (carvedAt(i, j, y)) return mat::AIR;
                // materialSolid's body, with the crust read from the grid
                // instead of hashed again for every one of these calls.
                const int h = H(i, j);
                if (y > h) {
                    // Above the ground: water if this column holds any and the
                    // row is at or under its line, else sky.
                    const int wl = int(WET(i, j));
                    return (wl >= 0 && y <= wl) ? mat::WATER : mat::AIR;
                }
                if (h - y >= kBedrockVox) return mat::BEDROCK;
                const uint8_t tm = T(i, j);
                if (y == h) return tm;
                if (tm == mat::ROCK) return mat::ROCK;
                return (h - y <= int(CR8(i, j))) ? mat::SOIL_0 : mat::ROCK;
            };
            std::vector<uint8_t> colMat;   // one column's profile, reused across columns
            // face:: order: POS_Y, NEG_Y, POS_X, NEG_X, POS_Z, NEG_Z.
            static const int kD[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                         {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
            for (const uint64_t key : slow) {
                const int ci = int(int32_t(uint32_t(key >> 21) & 0x1fffffu) << 11) >> 11;
                const int cj = int(int32_t(uint32_t(key) & 0x1fffffu) << 11) >> 11;
                if (ci < 0 || ci >= n || cj < 0 || cj >= n) continue;  // a neighbour chunk owns it
                // UP TO THE WATERLINE, not just the ground: the rows a lake
                // stands in are above H and are exactly what has to be meshed.
                const int yTop = maxi(int(H(ci, cj)), int(WET(ci, cj)));
                // DEEP ENOUGH FOR WHICHEVER PUT THIS COLUMN HERE. An edit
                // names its own y span; a carve does not, so the floor is the
                // deepest a carve is allowed to reach (see caved). Taking the
                // lower of the two covers a column that has both.
                int yBot = yTop - kBedrockVox + caveFloorVox - 2;
                if (ed && !ed->col.empty()) yBot = mini(yBot, yEditLo - 2);
                yBot = maxi(yBot, 0);
                const float x0 = float(I0 + ci) * s, x1 = x0 + s;
                const float z0 = float(J0 + cj) * s, z1 = z0 + s;
                // THE COLUMN'S OWN MATERIALS, ONCE. Every face test below asks
                // about this column and then about a neighbour, and the four
                // side directions were each re-deriving the same voxel: eleven
                // lookups a voxel where five will do. One pass down the column
                // into a scratch strip, and the loops below read it.
                colMat.assign(size_t(yTop - yBot + 3), mat::AIR);
                const int cmBase = yBot - 1;
                auto CM = [&](int y) -> uint8_t {
                    const int k = y - cmBase;
                    return (k < 0 || k >= int(colMat.size())) ? mat::AIR : colMat[size_t(k)];
                };
                for (int y = yBot - 1; y <= yTop + 1; ++y)
                    if (y - cmBase >= 0 && y - cmBase < int(colMat.size()))
                        colMat[size_t(y - cmBase)] = matAt(ci, cj, y);

                // THE TWO HORIZONTAL FACES, per voxel. A floor and a ceiling
                // are one voxel each in a column and there is nothing to merge
                // along -- merging THOSE wants a pass over slices, which is a
                // different loop from this one and buys far less.
                for (int y = yTop; y >= yBot; --y) {
                    const uint8_t mm = CM(y);
                    if (mm == mat::AIR) continue;
                    const float y0 = float(y) * s, y1 = y0 + s;
                    // THE LAKE SURFACE IS NOT EMITTED HERE. It is one flat
                    // height over a blob, which is the best case a rectangle
                    // merge ever gets -- and per column it was 448,962 water
                    // triangles where the merge gives a few thousand. The only
                    // water voxel with air above it is the surface row, so
                    // skipping water here skips exactly that pass's work.
                    if (CM(y + 1) == mat::AIR && mm != mat::WATER)
                        m.addQuad({x0,y1,z0},{x0,y1,z1},{x1,y1,z1},{x1,y1,z0}, mm, face::POS_Y);
                    if (CM(y - 1) == mat::AIR)
                        m.addQuad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}, mm, face::NEG_Y);
                }
                // THE FOUR SIDE FACES, AS RUNS. This is where the triangles
                // were: a tunnel wall is a tall vertical surface, and emitting
                // it a voxel at a time cost one quad per voxel per direction --
                // measured 5.88 M triangles against 2.72 M with caves off, most
                // of it wall. A run of voxels with the same material and air on
                // the same side is ONE quad however tall it is, which is the
                // rule the heightmap path already uses for a bank.
                for (int d = 2; d < 6; ++d) {
                    int runLo = 0, runHi = 0;
                    uint8_t runMat = mat::AIR;
                    // One past the bottom, so a run reaching yBot still closes.
                    for (int y = yTop; y >= yBot - 1; --y) {
                        uint8_t mm = mat::AIR;
                        if (y >= yBot) {
                            const uint8_t here = CM(y);
                            if (here != mat::AIR &&
                                matAt(ci + kD[d][0], cj + kD[d][1], y) == mat::AIR)
                                mm = here;
                        }
                        if (mm != mat::AIR && runMat == mm) { runLo = y; continue; }
                        if (runMat != mat::AIR) {
                            const float y0 = float(runLo) * s, y1 = float(runHi + 1) * s;
                            switch (d) {
                                case 2: m.addQuad({x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}, runMat, face::POS_X); break;
                                case 3: m.addQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, runMat, face::NEG_X); break;
                                case 4: m.addQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, runMat, face::POS_Z); break;
                                default:m.addQuad({x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}, runMat, face::NEG_Z); break;
                            }
                        }
                        runMat = mm;
                        runLo = runHi = y;
                    }
                }
            }
        }

        // ------------------------------------------------------------------
        // THE LAKE SURFACE, GREEDILY, IN TWO DIMENSIONS.
        //
        // Everything else in this mesher merges along ONE axis because the
        // terrain's columns are all different heights and a rectangle spanning
        // two of them would not be flat. A lake is the exception and the best
        // case there is: one height, one material, over a connected blob. So it
        // gets the full 2D merge -- widest run on a row, then the tallest block
        // of rows wet at the same line for that whole width.
        //
        // IT GOES IN THE TERRAIN'S OWN MESH, which is the point. This used to be
        // a second VoxMesh, a second BLAS and a second compaction entry under
        // the same chunk key; one of the two then overwrote the other and a
        // hillside became a flat sheet.
        // ------------------------------------------------------------------
        {
            scratch.wmerge.assign(size_t(n) * size_t(n), 0);
            uint8_t *const um = scratch.wmerge.data();
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n;) {
                    const int wl = int(WET(i, j));
                    if (wl < 0 || um[size_t(j) * n + size_t(i)]) {
                        ++i;
                        continue;
                    }
                    int w = 1;
                    while (i + w < n && !um[size_t(j) * n + size_t(i + w)] &&
                           int(WET(i + w, j)) == wl)
                        ++w;
                    int hh = 1;
                    for (bool ok = true; j + hh < n && ok;) {
                        for (int k = 0; k < w; ++k)
                            if (um[size_t(j + hh) * n + size_t(i + k)] ||
                                int(WET(i + k, j + hh)) != wl) {
                                ok = false;
                                break;
                            }
                        if (ok) ++hh;
                    }
                    for (int jj = j; jj < j + hh; ++jj)
                        for (int k = 0; k < w; ++k) um[size_t(jj) * n + size_t(i + k)] = 1;
                    const float x0 = float(I0 + i) * s, x1 = float(I0 + i + w) * s;
                    const float z0 = float(J0 + j) * s, z1 = float(J0 + j + hh) * s;
                    const float y1 = float(wl + 1) * s;   // the TOP of the surface row
                    m.addQuad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0},
                              mat::WATER, face::POS_Y);
                    i += w;
                }
        }

        return m;
    }

    // The water surface used to be built here, sized to the patch. With no
    // patch there is no size to give it, so GpuScene::buildWater makes one quad
    // larger than any ring will reach -- see scene_gpu.h.
};


// ---------------------------------------------------------------------------
// THE WORLD AS IT IS NOW: GENERATED, THEN DUG.
//
// The terrain is a height field and answers "is there ground here" in one
// comparison, which is why every gameplay query in this engine was written
// against it directly. The moment a tool could take a bite that stopped being
// the whole answer: the second term is the edit layer, and a query that skips
// it is looking at a world that no longer exists.
//
// WHAT THAT COST BEFORE THIS EXISTED. The swing ray marched the height field
// alone, so a hole was invisible to it -- the ray stopped at the ORIGINAL
// surface, hanging in the air above the pit, and reported the material that
// used to be there. One bite per column and every bite after it carved air and
// handed back a chunk of ground that was not there. A pick chipping a rock
// face never showed it, because a boulder is a model and models keep their own
// edited volume; it is the terrain half that was blind, and a shovel -- whose
// whole job is to dig the same spot until there is a pit -- shows it on the
// second swing.
//
// ONE PLACE THAT KNOWS WHERE THE HOLES ARE. World::terrainSolidAt used to be
// that place and said so; it is this now, and that one delegates, so there is
// still exactly one answer.
//
// THE CHUNK IS CACHED, NOT THE VOXEL. EditStore::get takes a lock, and a ray
// march asks up to four thousand times. A ray crosses one or two chunks, so
// remembering the last one asked for turns those four thousand locks into two
// -- and an untouched chunk answers nullptr once and costs nothing after that.
// ---------------------------------------------------------------------------
struct TerrainProbe {
    const VoxelTerrain *terrain = nullptr;
    // Null is legal and means "nobody has dug anything": every answer then
    // comes from the generator, which is what the world looked like before the
    // edit layer existed.
    const EditStore *edits = nullptr;

    TerrainProbe(const VoxelTerrain *t, const EditStore *e) : terrain(t), edits(e), memo_(&own_) {}
    // ...OR BORROW A MEMO THE CALLER IS ALREADY KEEPING WARM. A probe made per
    // call inside a loop would throw away the generator's octave cache on every
    // iteration and recompute the same lattice cells for the same column -- see
    // TerrainMemo. Handing one in costs nothing and keeps the loop's cache.
    TerrainProbe(const VoxelTerrain *t, const EditStore *e, TerrainMemo &m)
        : terrain(t), edits(e), memo_(&m) {}
    // NOT COPYABLE: memo_ points into own_ for the default constructor, and a
    // copy would leave the new probe reading the old one's cache.
    TerrainProbe(const TerrainProbe &) = delete;
    TerrainProbe &operator=(const TerrainProbe &) = delete;

    // The cheap half, for the step of a march: solid or not, and no material.
    bool solid(int i, int j, int y) {
        uint8_t m = mat::AIR;
        if (edited(i, j, y, &m)) return m != mat::AIR;
        return terrain && y <= terrain->heightVox(i, j, *memo_);
    }

    // ...and the whole answer, which is worth asking once, where the ray
    // stopped. mat::AIR for anything that is not there.
    //
    // THE VOXEL, NOT THE COLUMN'S SURFACE. This used to report topMaterial, and
    // the note beside it argued that the march stops on the top of a column, so
    // the surface IS what was hit. That holds for a floor and for nothing else:
    // look at a hillside and the ray stops on the SIDE of a taller column,
    // metres below its top, where the mesher drew the bands materialAt
    // describes -- grass over soil over rock. Reporting the grass there told a
    // pick that a bare rock face was turf, and would tell a shovel that a stone
    // cliff was diggable. materialAt is what the mesher itself asks, so this
    // cannot disagree with what is on the screen.
    uint8_t material(int i, int j, int y) {
        uint8_t m = mat::AIR;
        if (edited(i, j, y, &m)) return m;
        if (!terrain) return mat::AIR;
        const int h = terrain->heightVox(i, j, *memo_);
        if (y > h) return mat::AIR;
        return terrain->materialAt(i, j, y, h, terrain->topMaterial(i, j, h, *memo_));
    }

  private:
    bool edited(int i, int j, int y, uint8_t *out) {
        if (!edits) return false;
        const int cx = EditStore::floorDiv(i, CHUNK_VOX), cz = EditStore::floorDiv(j, CHUNK_VOX);
        if (!have_ || cx != cx_ || cz != cz_) {
            ce_ = edits->get(cx, cz);
            cx_ = cx;
            cz_ = cz;
            have_ = true;
        }
        return ce_ && ce_->voxel(i, j, y, out);
    }
    std::shared_ptr<const ChunkEdits> ce_;
    TerrainMemo own_;
    TerrainMemo *memo_ = nullptr;
    int cx_ = 0, cz_ = 0;
    bool have_ = false;
};

}  // namespace v2
