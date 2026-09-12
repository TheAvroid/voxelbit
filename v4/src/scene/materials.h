// ---------------------------------------------------------------------------
// materials.h -- what a voxel is made of, and what colour that is.
//
// EXTRACTED FROM voxelworld.h, WHICH NO LONGER EXISTS. That file was the world:
// a heightfield generator, a greedy surface mesher, a triangle packer and this.
// The first three are gone with the move to a volumetric store -- the world is
// an OpenVDB tree now, and nothing is meshed -- but the material table and the
// palette were never about triangles. They survived the rewrite unchanged, and
// they are here rather than in volume.h so that the thing which says what a
// voxel IS does not depend on the thing which says where voxels ARE.
//
// THE PALETTE IS STILL BUILT FROM THE MODELS. deriveGroundFromTrees and
// setStoneBand read their colours out of the .vox assets at load, so the ground
// is made of the same greens and browns as whatever is standing on it. That is
// unchanged by the store rewrite and is the reason a scene looks like it
// belongs together.
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

namespace v4 {

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
// THE BEDROCK RAMP -- the floor of the world, and the last thing there is.
//
// Six shades of near-black, spread per voxel on the device exactly as ROCK is
// spread over STONE_0. The HOST still writes one id, mat::BEDROCK, for the
// whole bottom of the world; groundShadeId turns it into one of these. A single
// flat black would read as a hole in the world rather than as stone, because
// nothing in a path tracer distinguishes an unlit surface from a missing one.
constexpr uint8_t BEDROCK_0 = 27;
constexpr uint8_t BEDROCK_COUNT = 6;  // 27..32
// Model palette entries are allocated from here up. IT MOVED WHEN THE BEDROCK
// RAMP WENT IN, which is safe because every model id is handed out at load by
// forModelColor and nothing stores one across a run -- but it is why the ramp
// went BELOW this line and not above it.
constexpr uint8_t TREE_BASE = 33;
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

    // -----------------------------------------------------------------------
    // GIVE ONE ALREADY-MINTED ENTRY A DIFFERENT SURFACE, keeping its colour.
    //
    // forModelColor classifies from the colour alone -- green on a conifer is a
    // needle, everything else is bark -- which is right for a wood and has
    // nothing useful to say about a gem. A ruby handed bark's 0.88 roughness
    // and 0.020 specular is a matt red pebble; what makes it read as a gem is
    // the highlight. So the loader mints the colour the usual way and then says
    // what the thing actually IS.
    // -----------------------------------------------------------------------
    void setFinish(uint8_t id, float roughness, float specular) {
        look_[id].roughness = roughness;
        look_[id].specular = specular;
    }

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
        // change of material rather than a change of light. Kept as the id the
        // WORLD stores; what gets drawn is the ramp below.
        set(mat::BEDROCK, 0.17f, 0.17f, 0.18f, 0.97f);

        // -- THE BEDROCK RAMP, dark grey through to near-black --------------
        //
        // LINEAR ALBEDOS, and low ones: 0.012 to 0.055 is charcoal through to
        // a dark slate. They are not pure neutral -- each carries a touch more
        // blue than red -- because a perfectly grey black under a blue sky
        // renders as a dead flat patch, and the faint cool cast is what lets
        // skylight pick out the form of it.
        //
        // ROUGH AND ALMOST UNREFLECTIVE. 0.98 roughness with 0.015 specular:
        // the one thing that stops a near-black surface reading as a hole is
        // the sheen along its grazing edges, and that is the specular term.
        for (int k = 0; k < mat::BEDROCK_COUNT; ++k) {
            const float t = float(k) / float(maxi(1, int(mat::BEDROCK_COUNT) - 1));
            const float g = 0.012f + 0.043f * t;
            set(uint8_t(mat::BEDROCK_0 + k), g * 0.94f, g * 0.97f, g * 1.06f, 0.98f);
            look_[uint8_t(mat::BEDROCK_0 + k)].specular = 0.015f;
        }
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
// Chunk geometry.
//
// 256 columns is 25.6 m. The number is unchanged from the triangle engine but
// the reason for it is not: it used to be the size that amortised building an
// acceleration structure against how long the chunk took to mesh. Nothing is
// meshed now, and what a chunk costs is one createNanoGrid over the voxels it
// actually holds -- so in an EMPTY world an untouched chunk costs nothing at
// all, and the size stopped being a tension to balance.
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


}  // namespace v4
