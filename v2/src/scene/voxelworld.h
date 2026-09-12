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
// Not an instance kind, not a surface anybody draws, not a second BLAS -- a
// material id written into the terrain like rock or sand. v1 does the same
// thing (its WATER_T/WATER_B are palette ids) and so does the DXR engine next
// door in v4.
//
// THIS IS ALSO WHAT MAKES THE OLD COMPACTION BUG IMPOSSIBLE RATHER THAN FIXED.
// An earlier attempt gave every chunk its own waterBlas, and recordChunkBuild
// queues a compaction under a chunk key whose drain has exactly ONE
// destination -- so two builds under one key meant the lake's flat quad
// overwrote the chunk's GROUND a frame or two after adoption. A material needs
// no second structure, so there is nothing left to collide. If anything ever
// reintroduces a per-chunk water BLAS, PendingCompact needs a bool water
// threaded through recordChunkBuild and branched in the drain.
//
// KIND_WATER in Shared.slang is left alone. It is an INSTANCE kind, its value
// is written into instance data host-side, and renumbering it shifts KIND_TREE
// and everything after it -- silently, into trees shaded as tools.
constexpr uint8_t WATER = 26;

// THE CHURNED BAND WHERE WATER MEETS LAND -- v1's shoreSurf, which is the most
// distinctive thing its lakes have and v2 had nothing of.
//
// ITS OWN MATERIAL RATHER THAN A SHADER TEST, because v2 cannot ask the GPU
// what is next door. v1 probes the voxel grid from the shader (voxAt at +-2 and
// +-4) and v2 has no grid on the device -- the water is triangles by then. So
// the decision is made where the knowledge is, on the host, and travels as a
// material id on the quad.
constexpr uint8_t FOAM = 27;
// THE SAND RAMP -- and it is the same fix the stone ramp above was.
//
// Sand was the ONLY ground material with no shade family. Grass has six, soil
// four, litter three, rock six; sand had one flat value, so a beach rendered
// as an unbroken plane of a single colour. That is exactly the failure the
// STONE_0 comment describes ("a single grey is what dug stone used to look
// like: a slab"), and on a beach it is worse, because bankShaped deliberately
// flattens the shore -- so the plane is not just uniform, it is metres wide
// and dead level, and it catches the sun square on.
//
// THAT IS WHAT "THE WHITE BANK" WAS. Not foam (that is off), and not the
// palette (mat::SAND is v1's own sRGB 203,183,145 and measures 207,187,151 in
// frame). It was a flat white-ish plane with one-voxel tan risers striping it,
// which reads as painted board rather than as sand.
//
// FOUR SHADES, AND THEIR MEAN IS EXACTLY mat::SAND, so nothing about the
// beach's overall colour moves -- only its uniformity. The blue channel swings
// wider than red and green because that is how sand actually varies: the
// darker grains are the damp ones and damp sand loses blue fastest, so the
// ramp runs warm-dark to cool-pale rather than just dim to bright.
//
// ONE ID ON THE HOST, exactly as with ROCK. The mesher writes mat::SAND for
// the whole beach so its faces still merge into long runs, and the device
// spreads that one id over these four per voxel -- see groundShade().
constexpr uint8_t SAND_0 = 28;
constexpr uint8_t SAND_COUNT = 4;  // 28..31
constexpr uint8_t TREE_BASE = 32;  // model palette entries are allocated from here up
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
        // v1'S SAND, CONVERTED. Its palette is sRGB (203, 183, 145); this
        // table is LINEAR (see the note above about decoding twice), so that
        // is (0.60, 0.47, 0.28).
        //
        // What was here -- (0.68, 0.61, 0.45) -- is brighter and much less
        // saturated, and the blue channel is the tell: 0.45 against 0.28 is
        // half again as much blue, which is exactly what turns tan into pale
        // grey. In sunlight it read as a white beach rather than a sandy one.
        set(mat::SAND, 0.60f, 0.47f, 0.28f, 0.85f);
        // ...AND THE FOUR GRAINS THE DEVICE SPREADS IT OVER. See mat::SAND_0
        // for why sand needs a ramp at all. The multipliers average 1.0, so
        // the beach keeps v1's colour and gains only its variation.
        {
            const float k[mat::SAND_COUNT] = {0.86f, 0.95f, 1.05f, 1.14f};
            for (int i = 0; i < int(mat::SAND_COUNT); ++i) {
                // Blue moves 1.6x as far as red and green: damp sand is the
                // dark sand, and what it loses is blue.
                const float f = k[i];
                const float fb = 1.0f + (f - 1.0f) * 1.6f;
                set(uint8_t(mat::SAND_0 + i), 0.60f * f, 0.47f * f, 0.28f * fb, 0.85f);
            }
        }
        set(mat::SILT, 0.22f, 0.20f, 0.16f, 0.95f);
        // WATER, AS A PLACEHOLDER TONE AND NOTHING MORE. A lake is a dielectric
        // and wants the delta lobe and the Beer-Lambert depth term that v1 and
        // v4 both carry in the tracer; none of that is here yet. This entry is
        // only so the geometry reads as water rather than as black while the
        // shading is still to come -- a deep blue-green, smooth enough to take
        // a specular highlight.
        set(mat::WATER, 0.06f, 0.16f, 0.21f, 0.06f);
        // Churned water is WHITE and ROUGH -- it is air in water, not a
        // mirror, so it must not take the dielectric's sheen.
        set(mat::FOAM, 0.92f, 0.95f, 0.96f, 0.85f);
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
    FbmMemo bankGrain;                                      // bankShaped
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

    // -----------------------------------------------------------------------
    // THE WATERLINE, AND IT IS PER BAND. This is the single number that killed
    // every earlier attempt at water in this engine, so it is worth stating
    // exactly what goes wrong.
    //
    // IT SHIPPED AT 2.6 m WHILE THE GROUND BOTTOMED OUT AT 5.8. Measured by
    // tests/water_survey.cpp over a 3.2 km square: a line at 2.6 floods
    // 0.000% of the world. There was no lake anywhere, and -- because
    // topMaterial's sand and silt are chosen by distance from the line -- no
    // SAND anywhere either. Both were dead code, and neither failure looked
    // like a waterline: it looked like a broken mesher or a broken material.
    //
    // AND A SINGLE GLOBAL LINE CANNOT WORK HERE, because the two woods have
    // different landforms:
    //
    //     pine    min 16.9   p1 30.7   p5 35.3   med 48.4   max 74.7
    //     birch   min  5.8   p1  8.5   p5  9.9   med 14.3   max 72.0
    //
    // A line at 33 makes lakes in the pine wood (2.4% of it) and puts 85.1% of
    // the BIRCH wood under water -- half the world, which is an ocean with a
    // forest in it. So the line is asked per column through waterAt, and the
    // birch band is dry.
    //
    // THE BIRCH WOOD BEING DRY IS A DECISION, not an omission. Giving it its
    // own lower line is a few lines of code here and a calibration job of its
    // own, and an uncalibrated guess is what the numbers above are a monument
    // to. Set birchWater to a metre value and re-run the survey if it is ever
    // wanted.
    // -----------------------------------------------------------------------
    static constexpr float kNoWater = -1.0e9f;
    // Compared against heights in VOXELS. Kept well inside int range, unlike
    // int(kNoWater / VOXEL_M), which overflows and lands anywhere.
    static constexpr int kNoWaterVox = -1000000000;

    float pineWater = 33.0f;          // metres, read off the pine field above
    float birchWater = kNoWater;      // the birch wood is dry -- see above

    // THE BASIN THRESHOLD. It decides where heightM CARVES A HOLLOW, and that
    // is now the only thing it decides -- it used to gate the wet test too,
    // and see lakeLineAt for the measurement that took that away. Carving and
    // filling are different questions: this one shapes the bowl, the
    // waterline says how high the water in it stands.
    float basinT = 0.40f;

    // HOW FAR BELOW THE LINE A BASIN IS PULLED, in metres -- the lake's depth
    // where the carve reaches full strength. At 3.2 the median lake was 1.3 m
    // deep, which reads as a wet patch rather than as water; it is swept in
    // tests/water_survey.cpp.
    // 4.5 puts p95 depth at 4.6 m against v4's stated 5.4 m ceiling. It was
    // 9.0, which ran p95 8.7 and max 9.3 -- twice v4's water, and absorption is
    // exponential in depth, so twice the depth is most of what made these lakes
    // read as ink where v4's read as pale blue. Swept in water_survey.
    float basinBed = 4.5f;

    // How far ABOVE the waterline the carve still reaches, and over how many
    // metres it fades in. Both are relative to the line -- see the note in
    // heightM, which is where getting this wrong cost the lakes their depth.
    float basinGateOver = 14.0f;
    float basinGateRamp = 14.0f;

    // -----------------------------------------------------------------------
    // THE BANK: how far above the line the shore reaches, and how hard it
    // flattens AT THE WATER'S EDGE.
    //
    // A bank that keeps the hillside's own slope is a hillside that happens to
    // be yellow. What the eye reads as a shore is the BREAK in slope, so the
    // break is what gets built -- ported from v4, which had it and v2 never
    // did. Until now a lake here met the hill at whatever gradient the basin
    // carve left, with no beach between them.
    //
    // EASED, NOT FLATTENED, AND THAT IS THE WHOLE TRAP. A constant multiplier
    // lowers the band by a constant FRACTION, so its top is lowered most and
    // the ground just outside it not at all. v4 measured what that costs at
    // these defaults -- a nine-voxel rise flattened to a quarter puts the last
    // bank column at waterY+3 against untouched terrain at waterY+10, a
    // SEVEN-VOXEL CLIFF ringing every lake, which the user reported as "a steep
    // 5 voxel or so drop off".
    //
    // So the multiplier is bankFlat at the water and 1.0 -- the ground's own
    // slope -- at the top of the band, eased with smoothstep. Smoothstep and
    // not a linear ramp because its derivative is zero at BOTH ends: the beach
    // leaves the water flat and joins the hillside at the hillside's gradient,
    // so neither seam is a crease.
    // -----------------------------------------------------------------------
    // 2.5 m, not v4's 0.9. THE BAND HEIGHT IS WHAT MAKES A BEACH READ AS ONE.
    // Flattening 0.9 m of vertical is a narrow strip on any ground that rises
    // at all -- measured, 25,438 beach columns against 81,975 at 3.0 m, and a
    // mean rise that only starts dropping (1.45 -> 1.24 m) once the band is
    // tall enough for the ease to have room to work in. It reads as a hard
    // sandy edge rather than a shore, which is what "the sand banks look
    // terrible" was.
    //
    // The sand band follows it automatically -- topMaterial asks bankRiseVox --
    // so the flattened ground and the sand on it are the same band by
    // construction, and cannot drift into half a beach.
    float bankRiseM = 2.5f;
    float bankFlat = 0.25f;

    // -----------------------------------------------------------------------
    // THE GRAIN THAT KEEPS THE FLAT BEACH FROM BEING A STAIRCASE.
    //
    // Flattening the bank is right and it is what was asked for, but it has a
    // cost nobody costed: a gentler slope crossed by a 10 cm quantisation
    // gives WIDER treads. At bankFlat 0.25 the shore falls a quarter as fast
    // as the ground around it, so each 10 cm contour stretches into a tread
    // metres across, and the beach renders as a flight of dead-level boards
    // with one-voxel risers striping it. That is the staircase in the shore
    // render -- the flattening and the terracing are the same knob.
    //
    // A SHADE RAMP WAS NOT ENOUGH. Sand got four shades (mat::SAND_0) for the
    // same reason rock has six, and it helps, but colour cannot break a
    // silhouette: the treads were still treads.
    //
    // So the height itself gets a little noise, and the terrace edges ravel
    // instead of running as clean contour lines. This is also what a real
    // beach does -- the wave-worked sand at the water's edge is the uneven
    // part -- so the fade below runs the right way round.
    //
    // AMPLITUDE IS SET AGAINST THE QUANTISATION, NOT BY EYE. +/-0.06 m is
    // 1.2 voxels peak to peak: enough for the contour to wander across a
    // voxel boundary and back, which is what ravels it, and not enough to
    // invent relief the landform does not have.
    //
    // FREQUENCY IS SET AGAINST THE STEP TEST. At ~0.9 m the steepest grain
    // gradient is 2*pi*0.06/0.9 = 0.42, so neighbouring columns differ by
    // 0.042 m -- under half a voxel, so it cannot add a step of its own. Take
    // the wavelength much below this and it starts failing the survey's
    // "nothing steps 4 voxels or more".
    // -----------------------------------------------------------------------
    float bankGrainM = 0.06f;  // metres, plus and minus
    float bankGrainF = 1.10f;  // ~0.9 m wavelength

    // -----------------------------------------------------------------------
    // THE WAVES ARE NOT GEOMETRY, AND THE WATER HAS NO LID. BOTH ARE ZERO.
    //
    // What this used to be: waveVox raised each water column 0..waveVoxMax so
    // that crests were real ground, and then -- when that turned out to
    // animate only by cutting between still frames -- a FLAT lid waveVoxMax
    // voxels over the line, with the tracer solving the real surface below it.
    // Both are the same mistake wearing different clothes. They put water
    // ABOVE the waterline, and the terrain is only shaped to contain water AT
    // the waterline.
    //
    // IT COST A WALL AROUND EVERY LAKE. The tracer's solve runs on the slab's
    // TOP faces only -- a vertical side face has a horizontal normal and
    // cannot wear a y-up wave -- so the sides stood at full lid height while
    // the sand beside them sits one voxel over the line. Measured over
    // 1200 x 1200 columns: 4,743 of 4,743 lake-edge faces stood proud of the
    // land next to them, 4,738 of those by exactly 5 voxels. Half a metre of
    // vertical water, continuous, all the way round. That is the "wall of
    // water" in the screenshot, and it is not a shading bug -- the geometry
    // really was up there.
    //
    // SO THE WATER ENDS AT THE LINE, which is what commit 9fe31c0 did and
    // what "fit the water function inside the terrain function" asks for. The
    // lake tops at waterTopVox == line, bankShaped flattens the ground around
    // it and the sand band starts a voxel above it, so the ground contains
    // the lake BY CONSTRUCTION rather than by a constant that has to be kept
    // in agreement with it. There is no height left for a wall to be made of.
    //
    // THE MOTION NEVER LIVED HERE ANYWAY. It is the shading normal -- see
    // waterNormal in Trace.cs.slang -- which is continuous, free, cannot tear
    // at a chunk seam and cannot poke through geometry that is not there. v1,
    // which is the water this is judged against, ships `waves: 0` for exactly
    // this reason: what makes its lake read right is the Fresnel cap, the
    // glint and the in-scatter, not swell.
    //
    // Kept at 0 rather than deleted: waveVox and the tick machinery in
    // gpu/world.h are measured and correct, and if the voxels ever get small
    // enough for a crest to be worth having, this is where it comes back.
    // Anything above 0 needs the shore solved first.
    // -----------------------------------------------------------------------
    int waveVoxMax = 0;  // 0 = the lake tops at the waterline; see above

    // -----------------------------------------------------------------------
    // THE SHORE BAND: how shallow water has to be to churn, and how far the
    // foam stands proud of the swell.
    //
    // DEPTH, NOT DISTANCE. v1 asks the voxel grid whether land is within two or
    // four voxels sideways; v2 has no grid to ask on the device and no padding
    // for a four-voxel probe on the host. Shallow water rings a lake exactly
    // where land is near, so the depth test finds the same band and costs one
    // subtraction against eight neighbour fetches.
    //
    // THE LIFT IS THE POINT, and v1 is emphatic about why: the band has to be
    // raised BEFORE the surface is intersected, not after. Lifting a hit that
    // has already been found only moves that pixel's depth -- "the foam kept
    // the silhouette of the flat water because the pixels it should have grown
    // into were never tested against the water at all". Raised here, in the
    // geometry, it has a real edge standing over the swell.
    // -----------------------------------------------------------------------
    // OFF. 0 disables the band entirely -- foamColumn() never fires, so no
    // FOAM material and no lift.
    //
    // WHAT I BUILT WAS NOT v1'S FOAM. v1 mixes foam INTO the water's own
    // colour, patchily, broken up by a hash and animated:
    //
    //     foam   = max(foam, step(0.35, ivhash(...) * (0.55 + 0.45*sin(...))));
    //     foamK  = clamp(foam, 0, 1) * 0.8;
    //     albedo = mix(albedo, FOAM_C, foamK);
    //
    // ...so it reads as churn: scattered, shifting, and never fully white.
    // This version made every shallow column a SOLID white material and then
    // lifted it a voxel to stand proud. A solid white band standing over the
    // water all the way round a lake is a wall, and that is exactly how it
    // looked. The lift -- which is right in v1, where it carries a broken-up
    // band -- is what turned a bad colour into a bad silhouette.
    //
    // Doing it properly means the patchy mix, in the shader, on the water
    // material: not a second material and not geometry. Left in place at 0 so
    // the machinery is there when that is wanted.
    int foamDepthVox = 0;  // 0 = no foam band
    int foamLiftVox = 1;   // ...and how far it would stand proud

    int waveCeilVox() const { return waveVoxMax; }

    // SECONDS, AND A WALL CLOCK. Not the day clock: X plus scroll runs that at
    // up to forty times speed and backwards, and a lake that reverses its chop
    // when you scrub the sun is a bug. Set per meshing job -- each worker keeps
    // its own copy of the terrain so this can differ between them without a
    // race, and a chunk simply carries whatever phase it was built at.
    float waveTime = 0.0f;

    int waveVox(float x, float z) const {
        if (waveVoxMax <= 0) return 0;
        // THE DEEP-WATER RELATION, so the two trains move at the speeds their
        // wavelengths demand rather than at one arbitrary rate: w = sqrt(g*k),
        // which is what stops a short chop and a long swell sliding over each
        // other like two printed sheets.
        const float a = sinf(x * 0.86f + z * 0.31f - waveTime * 2.92f);       // ~7.3 m
        const float b = sinf(x * -0.42f + z * 2.03f + 1.7f - waveTime * 4.5f);  // ~3.1 m
        const float u = 0.5f + 0.25f * (a + b);               // 0..1
        const int v = int(u * float(waveVoxMax + 1));
        return v < 0 ? 0 : (v > waveVoxMax ? waveVoxMax : v);
    }

    // -----------------------------------------------------------------------
    // HOW FAR UP THE SHORE THE SAND GOES -- AND IT IS NOT THE FLATTEN BAND.
    //
    // These were tied together on the argument that "the flattened ground and
    // the sand on it are the same band". That is wrong, and the render showed
    // it: flattening wants a TALL band (2.5 m) so the shore reads as flat,
    // while sand wants a SHORT one, because sand is a strip at the water's
    // edge and not a paint job up the hillside. Tied at 2.5 m the bank came
    // out white -- 85,640 sand faces on one lake chunk against 1,834 of foam --
    // which reads as a beach swallowing the wood rather than meeting it.
    //
    // 0.9 m is v4's, and v4's shore is the reference here.
    // -----------------------------------------------------------------------
    // 1.4 m. v4 uses 0.9, but with the bank now flattening 2.5 m the shore is
    // much broader in PLAN, and 0.9 m of rise across it left barely a strip of
    // sand visible -- the beach disappeared when the white band was removed.
    float sandRiseM = 1.4f;

    int bankRiseVox() const { return maxi(1, int(bankRiseM / VOXEL_M)); }
    int sandRiseVox() const { return maxi(1, int(sandRiseM / VOXEL_M)); }

    // -----------------------------------------------------------------------
    // The bank, as a function of one column's height alone.
    //
    // v4 does this as a pass over a mutable height GRID; v2's terrain is a pure
    // function of (x, z), so it is expressed continuously here instead of in
    // quantised voxels. Same curve, and it keeps heightM continuous -- which
    // the collider, forestGain and the spawn search all depend on.
    //
    // MONOTONIC BY CONSTRUCTION, so the bank can never fold back on itself:
    // d*(bf + (1-bf)*S(d/D)) has derivative bf + (1-bf)*S + d*(1-bf)*S'/D, and
    // S' = 6u(1-u) >= 0, so the whole thing is never below bf > 0.
    //
    // GATED EXACTLY LIKE THE WATER -- band and basin. Flattening a shore where
    // no lake can stand is the same class of mistake as painting sand there,
    // and that one put 5,958 dry sand pits in the wood.
    // -----------------------------------------------------------------------
    // NO "IS A LAKE NEARBY" TEST, and v4 is explicit about why it removed
    // its own: "There was one, searching two voxels out, and it produced no
    // banks at all: the waterline is a global HEIGHT, the slopes here are
    // gentle, and a column sitting 0.9 m above the line is metres away in
    // PLAN from one sitting 0.2 m below it. Twenty centimetres of search
    // found nothing and every lake came out as grass meeting water with no
    // shore between them."
    //
    // v2 reproduced that failure exactly, by a different route: the basin
    // gate below used to veto the bank, so a shore whose basin value had run
    // out kept the hillside's slope and its grass. ANY GROUND WITHIN
    // bankRiseM OF THE LINE IS A BANK. Height is the whole test.
    float bankShaped(float h, float wlm, float x, float z, TerrainMemo &memo) const {
        if (wlm == kNoWater) return h;
        // FADED OUT AT THE BASIN'S EDGE, exactly as the carve is.
        //
        // A hard gate here is a cliff: a column just inside the basin is
        // flattened to a quarter of its rise and its neighbour just outside is
        // untouched, so the two differ by most of the band. At a 2.5 m band
        // that measured as a SIX-VOXEL step with 185 of them over 4 voxels --
        // the very failure v4's ease exists to prevent, reintroduced at the
        // other end of the same function.
        //
        // THE `edge` FADE IS GONE WITH THE GATE IT EXISTED FOR. It ramped the
        // bank out over the basin mask so the gate's boundary was not a cliff
        // (6-voxel steps, 185 of them over 4 voxels). With no gate there is no
        // boundary to fade, and the band's own ease already carries the bank
        // into the hillside at the hillside's gradient.
        const float d = h - wlm;
        if (d <= 0.0f || d > bankRiseM) return h;
        const float u = d / bankRiseM;
        const float ease = u * u * (3.0f - 2.0f * u);
        // NEVER BELOW ONE VOXEL OVER THE LINE, and this clamp is the whole
        // difference between a beach and a drowned one.
        //
        // Without it the flattening pushes the innermost band under the water:
        // a column 5 cm above the line times a 0.25 multiplier lands AT the
        // line and turns into lake. So the beach loses its inner strip and
        // gains nothing at the top (where the ease is already 1.0), and
        // flattening made the shore NARROWER -- measured, 31,426 beach columns
        // down to 25,438. A beach that gets thinner the flatter you make it is
        // the wrong way round, and it is what "the sand banks look terrible"
        // was.
        //
        // v4 never had the bug because it works in integers and writes
        // `waterY + 1 + int(d * k)`, which floors at one voxel up by
        // construction. This is the continuous form of that floor.
        // k is the slope multiplier; `edge` fades it back to 1.0 (the ground's
        // own slope) as the basin mask falls away.
        const float k = bankFlat + (1.0f - bankFlat) * ease;
        // v4's `waterY + 1 + int(d * k)`, in the continuous form -- see the
        // one-voxel floor note above.
        const float flat = wlm + maxf(d * k, VOXEL_M);

        // THE GRAIN, FADED AT BOTH ENDS OF THE BAND so it cannot introduce a
        // seam of its own -- which would be the basin-gate cliff all over
        // again, at a third boundary.
        //
        //   * edge      dies with the basin mask, exactly as the flattening
        //               and the carve do, so all three boundaries coincide.
        //   * 1 - ease  dies at the TOP of the band, where k has already
        //               reached 1.0 and the bank has become ordinary ground.
        //               Full strength at the water's edge, which is both
        //               where the treads are widest and where a real beach is
        //               roughest.
        //
        // The one-voxel floor is re-applied afterwards: grain must never be
        // what pushes a beach column under the line and turns it into lake.
        // That bug cost 6,000 columns of shore once already.
        const float g = (fbm(memo.bankGrain, x * bankGrainF, z * bankGrainF, 2) - 0.5f) * 2.0f;
        return maxf(flat + g * bankGrainM * (1.0f - ease), wlm + VOXEL_M);
    }

    float waterAt(float x) const { return (birchMix(x) <= 0.001f) ? pineWater : birchWater; }
    int waterVoxAt(float x) const {
        const float w = waterAt(x);
        return (w == kNoWater) ? kNoWaterVox : int(w / VOXEL_M);
    }
    bool anyWater() const { return pineWater != kNoWater || birchWater != kNoWater; }

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

        // THE CARVE IS A FUNCTION OF THE WATERLINE AND EXISTS FOR NOTHING
        // ELSE. It pulls low ground down toward the line so that a lake has a
        // hollow to sit in. In a band with no water there is no line to pull
        // toward, and leaving it in would cut dry pits into a wood -- which is
        // exactly what removing the water from the old v4 left behind until it
        // was chased down. So a dry band is not carved.
        const float wlm = waterAt(x);
        const float b = (wlm == kNoWater)
                            ? 1.0f
                            : fbm(memo.basin, x * 0.0160f + 311.7f, z * 0.0160f + 157.3f, 4);
        if (b < basinT) {
            const float m = sstep(minf(1.0f, (basinT - b) / 0.10f));
            // THE GATE IS PINNED OVER THE WATERLINE, NOT OVER THE LANDFORM,
            // and that distinction is the whole reason the lakes were puddles.
            //
            // It used to read saturate((36 - h) / 28) -- an absolute height,
            // and the comment beside it explained that 36 was a FRACTION of
            // the landform's range, rescaled every time the terrain grew. That
            // works only while the waterline sits near the bottom of the
            // range, which it did at 2.6 m. At 33 m it does not: a column at
            // h = 34, one metre over the line and exactly where a lake bank
            // belongs, scored (36 - 34) / 28 = 0.07 and was barely carved at
            // all. Measured: sweeping basinBed from 3.2 m to 12 m moved the
            // median lake depth from 1.3 m to 1.7 m, because the depth was
            // never what limited it -- the gate was.
            //
            // Pinned over the line instead, the carve bites on exactly the
            // band that can hold water and ignores the hills, which is what it
            // was always for. basinGateOver 14 reproduces the calibrated
            // absolute gate of 47 recorded for a 33 m line.
            //
            // RESCALING THIS AS A FRACTION OF THE LANDFORM IS THE MOVE THAT
            // DELETES HILLS. It is relative to the WATER, and the water is the
            // only thing it should ever be relative to.
            const float lowGate = saturate((wlm + basinGateOver - h) / basinGateRamp);
            h -= m * lowGate * (h - (wlm - basinBed));
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
        // THE PINE SIDE RETURNS HERE, and it is where every lake in the world
        // is -- waterAt hands back kNoWater unless birchMix is under this very
        // threshold. Shaping the bank only at the blended return below meant it
        // ran on the 11% of columns inside a birch seam and on NONE of the
        // columns that have water, which measured as the bank doing nothing at
        // all whatever bankFlat was set to.
        if (mix <= 0.001f) return bankShaped(h, wlm, x, z, memo);

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
        // THE BANK IS APPLIED TO THE FINAL HEIGHT, after the birch blend, so
        // a column inside a seam is shaped from the height it actually has.
        // Water only exists where birchMix is ~0, so in practice this is the
        // pine side of the world and the lerp has already collapsed to h.
        return bankShaped(lerpf(h, 2.0f + bRoll * 15.0f + bSwell * 7.0f + fine, mix), wlm, x, z,
                          memo);
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

        // THE BED AND THE SHORE. Under the line it is sand for the first
        // eight voxels and silt below that; for eight voxels ABOVE it, the
        // beach. Asked per column through waterVoxAt, so a dry band gets
        // kNoWaterVox and neither test fires -- no beach in a wood with no
        // lake.
        // THE SAME LINE THE WATER USES. It is band-only now -- see
        // lakeLineAt for the measurement that removed the basin gate, and for
        // why "sand pits all over the wood with no water in them" was the
        // GATE deleting the water rather than the band painting spare sand.
        // With the gate gone the band-only rule leaves zero dry sand columns,
        // measured, which is what v4 has always done.
        const int wl = lakeLineAt(wx(i), wx(j), memo);
        // THE WHOLE BED IS SAND, NOT A SKIN OF IT OVER SILT.
        //
        // This used to grade to mat::SILT more than eight voxels down, which
        // sounds right and looks wrong: SILT is (0.22, 0.20, 0.16), nearly
        // black, and these lakes are 3.8 m deep at the median and 9.5 at the
        // worst -- so almost every bed was dark mud. Seen THROUGH water it is
        // then attenuated twice more, by the eye path and by the analytic
        // downward term, and the lake reads as ink. What you see through water
        // is the bed, so the bed is most of what the water looks like.
        //
        // v4 reached the same conclusion from the other side and its comment is
        // the better statement of it: sand goes down as far as the soil would,
        // because a one-voxel skin "shows its brown underside the moment the
        // shore is seen from below the waterline, which through clear water is
        // most of the time".
        // THE BED IS WHAT THE WATER ACTUALLY STANDS ON -- the depth rule, not
        // `h <= wl`. A ghost column does not become dirt: it falls through to
        // the shore band below and is BEACH, which is what dry ground at the
        // waterline is.
        if (wetColumn(i, j, h, wl, memo)) return mat::SAND;
        // THE SAME BAND THE BANK FLATTENS. Sand that is not flattened, or a
        // flattened shore that is not sand, would each be visibly half a beach.
        if (h <= wl + sandRiseVox()) return mat::SAND;  // the shore band

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
    // -----------------------------------------------------------------------
    // THE BASIN FIELD, ASKED DIRECTLY. heightM uses it to decide where to pull
    // the ground down; lakeColumn uses it to decide where water may stand. One
    // function so the two can never drift apart.
    // -----------------------------------------------------------------------
    float basinAt(float x, float z, TerrainMemo &memo) const {
        return fbm(memo.basin, x * 0.0160f + 311.7f, z * 0.0160f + 157.3f, 4);
    }

    // -----------------------------------------------------------------------
    // IS THIS COLUMN UNDER WATER, and if so up to which voxel.
    //
    // A LAKE HAS TO BE IN A BASIN OR IT IS AN OCEAN. Flooding every column
    // below the line fills whatever the landform happens to leave low, so a
    // valley floor running for a kilometre becomes a kilometre of water. The
    // basin field is what says "this is a hollow", and requiring it is the
    // difference between two dozen lakes and one sea.
    //
    // THE THREE TESTS ARE ORDERED BY COST. The band is a function of x alone
    // and rejects the whole birch wood for free; the basin field is four
    // octaves; the height is six fields and is asked last.
    // -----------------------------------------------------------------------
    // THE LINE WHERE WATER MAY STAND -- band and basin, WITHOUT the height
    // test. Split out because the mesher already holds the height for every
    // column it is working on, and re-deriving it there is the single most
    // expensive thing in the gather. A caller with a height finishes the job
    // with one comparison; one without it calls lakeColumn below.
    int lakeLineAt(float x, float z, TerrainMemo &memo) const {
        const int wl = waterVoxAt(x);
        if (wl == kNoWaterVox) return kNoWaterVox;
        // THERE IS NO BASIN GATE ON THE WATER. THE LINE IS THE LINE.
        //
        // There was one, and loosening it (basinWetMargin) was an attempt to
        // stop it cutting lakes off mid-slope. The honest fix is to delete it,
        // because MEASUREMENT SAYS IT NEVER HAD A JOB TO DO. Over four
        // 1200 x 1200 regions, comparing the gate against a plain depth test:
        //
        //   lake region    gated: 479,080 wet in 27 bodies, 26 of them
        //                         puddles under 1 m2
        //                  plain: 478,014 wet in ONE body, zero puddles
        //   (12000, 8000)  gated: NO WATER AT ALL
        //                  plain: one clean 178 m2 lake
        //   (0,0), (-30000, 25000): both zero
        //
        // The gate was added to stop "every shallow dip becoming a one-voxel
        // puddle". v2's field does not do that -- it is smooth, and a flat
        // line through a smooth heightfield gives connected regions, not
        // scatter. Zero puddles at all four sites without it. What the gate
        // actually did was fragment one lake into 26 scraps and DELETE whole
        // legitimate lakes elsewhere.
        //
        // AND IT IS WHY THE SAND LOOKED BROKEN TWICE. "Empty sand pits
        // everywhere with no water" was never a sand bug: the band-only sand
        // was right and the gate had removed the water beside it. Gating the
        // sand on the same predicate then produced "missing the sandy banks
        // completely". One cause, two symptoms, and both ends were wrong.
        //
        // basinAt still carves the hollows in heightM -- that is what makes
        // lakes rather than an ocean, and it is a different question from
        // where water stands once they exist. Water finds its level.
        return wl;
    }

    // -----------------------------------------------------------------------
    // HOW DEEP WATER HAS TO BE TO COUNT, AND THE RULE LIVES HERE ONLY.
    //
    // The old test was `h <= line`, and it disagreed with the mesher. The
    // water span is h+1 .. line, so at h == line that span is EMPTY -- the
    // column was wet to /locate water, to the sand rule and to the survey,
    // and held no water at all. Measured: 9,927 such GHOST columns on the big
    // lake (2.06% of its wet columns) and 2,763 on a small one, where the
    // fringe is proportionally far bigger -- 12.8%.
    //
    // v4's rule, which does not have the failure: a column needs kWetMinVox
    // of water on its own, and a column ONE voxel shallower than that is wet
    // only if a NEIGHBOUR is properly wet. v4 calls that second clause "the
    // whole thing" -- without it every shallow dip becomes a one-voxel
    // puddle. It also kills the ghosts by construction, because h == line is
    // two voxels short of the threshold.
    //
    // THE NEIGHBOUR CLAUSE IS WHY THIS CANNOT BE A PURE FUNCTION OF (i, j)
    // ALONE, and it is the one piece of v2's water that genuinely needs to
    // look sideways. The point path below pays four extra height evaluations
    // for it, and only on a fringe column; ColumnStack does it as a pass over
    // an array it already has. THE TWO MUST AGREE, and
    // tests/voxel_parity_test.cpp is what holds them to it -- it builds the
    // truth point-wise and compares the brick path against it face by face.
    // -----------------------------------------------------------------------
    static constexpr int kWetMinVox = 2;

    // Deep enough to be wet on its own account.
    bool deepWet(int h, int line) const {
        return line != kNoWaterVox && h <= line - kWetMinVox;
    }
    // Exactly one voxel shallower: wet only with a properly wet neighbour.
    bool fringeWet(int h, int line) const {
        return line != kNoWaterVox && h == line - kWetMinVox + 1;
    }

    // The point path. ColumnStack::wetAt is the same rule over the gather.
    bool wetColumn(int i, int j, int h, int line, TerrainMemo &memo) const {
        if (deepWet(h, line)) return true;
        if (!fringeWet(h, line)) return false;
        const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int ni = i + di[d], nj = j + dj[d];
            if (deepWet(heightVox(ni, nj, memo), lakeLineAt(wx(ni), wx(nj), memo))) return true;
        }
        return false;
    }

    bool lakeColumn(int i, int j, TerrainMemo &memo, int *waterY) const {
        const int wl = lakeLineAt(wx(i), wx(j), memo);
        if (wl == kNoWaterVox) return false;
        if (!wetColumn(i, j, heightVox(i, j, memo), wl, memo)) return false;
        *waterY = wl;
        return true;
    }

    // -----------------------------------------------------------------------
    // HOW TALL A GRASS BLADE STANDS ON THIS COLUMN, 0 for none.
    //
    // A pure function of the WORLD column and its surface material, which is
    // what lets two different meshers agree about it. Hashed on the world
    // column rather than a chunk-local one so a blade is in the same place no
    // matter which chunk -- or which mesher -- happens to build it; otherwise
    // the grass reshuffles every time a chunk is rebuilt.
    //
    // CLAMPED TO WHAT THE PACKED TRIANGLE CAN SAY. The device recovers a
    // blade's row from three bits of its base, so a ninth voxel would wrap to
    // the bottom of the gradient and wear the darkest green at the tip.
    // grassMinRows and grassMaxRows are tuning knobs; STRAND_MAX_ROWS is a
    // format limit.
    // -----------------------------------------------------------------------
    uint8_t strandRows(int i, int j, uint8_t top) const {
        if (!isGrass(top)) return 0;
        const uint32_t cell = hashU32(uint32_t(i), uint32_t(j));
        if (hashUnit(strandSeed, cell) >= grassDensity) return 0;
        const int span = maxi(1, grassMaxRows - grassMinRows + 1);
        return uint8_t(
            mini(STRAND_MAX_ROWS,
                 grassMinRows + mini(span - 1, int(hashUnit(strandSeed + 1u, cell) * span))));
    }

    // -----------------------------------------------------------------------
    // WHAT STANDS OVER THE GROUND IN A COLUMN, AS SPANS.
    //
    // Three things can occupy a voxel above the surface -- a blade, water, or
    // nothing -- and until these existed each was decided in TWO places: once
    // by the mesher, which draws it, and once by TerrainProbe, which every
    // gameplay query asks. The two agreed only because they were written on the
    // same day, and nothing stopped them drifting. They were also not equal:
    // the mesher knew about the lake and the probe did not, so to the swing ray
    // and the held item a lake was empty air.
    //
    // SPANS RATHER THAN A PER-VOXEL TEST because the mesher wants a bitmask and
    // the probe wants one voxel. A span serves both without either paying for
    // the other's shape. An empty span is hi < lo.
    //
    // THE PRIORITY IS AN INVARIANT, NOT A PREFERENCE. A blade needs a grass
    // surface and a wet column gets sand or silt, so the two are mutually
    // exclusive as the terrain is tuned today -- but that is a coincidence of
    // the material rules, not something enforced anywhere. Two layers claiming
    // one voxel would put two quads in it; parity would catch that as a doubled
    // face, but only on a chunk somebody happened to run. Fixing the order here
    // means both readers agree whatever the tuning does.
    // -----------------------------------------------------------------------
    enum class Above : uint8_t { None = 0, Blade = 1, Water = 2 };

    // A blade stands ON the surface voxel, so it starts one above it. rows == 0
    // gives hi < lo, which is the empty span.
    static void bladeSpan(int h, int rows, int *lo, int *hi) {
        *lo = h + 1;
        *hi = h + rows;
    }

    // Water fills from the bed to the line, and only where the bed is under the
    // line at all. A dry column gives hi < lo.
    // `crest` is waveVox for this column: the surface ends that many voxels
    // above the flat line. Whether the column is WET at all is still decided by
    // the flat line, so the lake's plan does not move with the swell.
    static void waterSpan(int h, int line, int *lo, int *hi, int crest = 0) {
        *lo = h + 1;
        *hi = (line == kNoWaterVox || h > line) ? h : line + crest;
    }

    // Is this column the churned band? Shallow, and wet at all.
    bool foamColumn(int h, int line) const {
        // <= 0 means OFF. Without this an unsigned-style read of the depth
        // test still fires on `line - h == 0` -- the columns whose bed sits
        // exactly at the waterline -- and leaves a one-voxel white ring all
        // the way round every lake. Measured: 1,060 foam faces still drawn on
        // a lake chunk after "disabling" it.
        if (foamDepthVox <= 0) return false;
        return line != kNoWaterVox && h <= line && (line - h) <= foamDepthVox;
    }

    // The water's top for a column, lift included. One definition so the
    // mesher, the probe and anything that asks later cannot disagree about
    // where the shore stands.
    // WET IS AN INPUT, because the depth rule needs a neighbour and this
    // function has no coordinates to ask one with. `h >= line` rather than
    // `h > line` is belt and braces: a column with zero voxels of water gets
    // no water top even if a caller hands in the wrong flag.
    int waterTopVox(int h, int line, int crest, bool wet) const {
        if (!wet || line == kNoWaterVox || h >= line) return h;
        return line + crest + (foamColumn(h, line) ? foamLiftVox : 0);
    }

    // waterTop is what waterTopVox returned for this column -- the line, the
    // lid clearance and the shore lift already folded in. Passed rather than
    // recomputed so a caller cannot use a different rule from the mesher's.
    static Above aboveAt(int y, int h, int rows, int waterTop) {
        if (y <= h) return Above::None;  // in the ground, not over it
        int lo = 0, hi = 0;
        bladeSpan(h, rows, &lo, &hi);
        if (y >= lo && y <= hi) return Above::Blade;
        if (y > h && y <= waterTop) return Above::Water;
        return Above::None;
    }

    uint8_t materialAt(int i, int j, int y, int h, uint8_t top) const {
        if (y > h) return mat::AIR;
        if (y == h) return top;
        if (h - y >= kBedrockVox) return mat::BEDROCK;
        if (top == mat::ROCK) return mat::ROCK;
        return (h - y <= crustVox(i, j)) ? mat::SOIL_0 : mat::ROCK;
    }

    uint8_t topMaterial(int i, int j, int h, TerrainMemo &memo) const {
        // Only reached from the sparse scatter paths. Below the shore band the
        // slope is never consulted, so it is not worth four height evaluations
        // to compute one that will be discarded.
        const int wl = lakeLineAt(wx(i), wx(j), memo);
        if (h <= wl + sandRiseVox()) return topMaterial(i, j, h, 0, memo);
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
        auto ED = [&](int i, int j) { return !slow.empty() && slow.count(ChunkEdits::ckey(i, j)) != 0; };

        scratch.sr.resize((size_t(n) + 2) * (size_t(n) + 2));
        uint8_t *const srp = scratch.sr.data();
        auto SR = [&](int i, int j) -> uint8_t & {
            return srp[size_t(j + 1) * (n + 2) + size_t(i + 1)];
        };
        for (int j = -1; j <= n; ++j)
            for (int i = -1; i <= n; ++i) {
                // THE RULE LIVES ON THE TERRAIN NOW, not here -- the brick
                // mesher needs exactly the same answer, and two copies of a
                // hash is how the grass comes out in two different places
                // depending on which mesher drew it.
                SR(i, j) = strandRows(I0 + i, J0 + j, T(i, j));
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
                return materialAt(I0 + i, J0 + j, y, H(i, j), T(i, j));
            };
            // face:: order: POS_Y, NEG_Y, POS_X, NEG_X, POS_Z, NEG_Z.
            static const int kD[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                         {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
            for (const uint64_t key : slow) {
                const int ci = int(int32_t(uint32_t(key >> 21) & 0x1fffffu) << 11) >> 11;
                const int cj = int(int32_t(uint32_t(key) & 0x1fffffu) << 11) >> 11;
                if (ci < 0 || ci >= n || cj < 0 || cj >= n) continue;  // a neighbour chunk owns it
                const int yTop = H(ci, cj);
                const int yBot = yEditLo - 2;
                const float x0 = float(I0 + ci) * s, x1 = x0 + s;
                const float z0 = float(J0 + cj) * s, z1 = z0 + s;
                for (int y = yTop; y >= yBot; --y) {
                    const uint8_t mm = matAt(ci, cj, y);
                    if (mm == mat::AIR) continue;
                    const float y0 = float(y) * s, y1 = y0 + s;
                    for (int d = 0; d < 6; ++d) {
                        if (matAt(ci + kD[d][0], cj + kD[d][1], y + kD[d][2]) != mat::AIR) continue;
                        switch (d) {
                            case 0: m.addQuad({x0,y1,z0},{x0,y1,z1},{x1,y1,z1},{x1,y1,z0}, mm, face::POS_Y); break;
                            case 1: m.addQuad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}, mm, face::NEG_Y); break;
                            case 2: m.addQuad({x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1}, mm, face::POS_X); break;
                            case 3: m.addQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, mm, face::NEG_X); break;
                            case 4: m.addQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, mm, face::POS_Z); break;
                            default:m.addQuad({x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0}, mm, face::NEG_Z); break;
                        }
                    }
                }
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
        const uint8_t top = terrain->topMaterial(i, j, h, *memo_);
        if (y <= h) return terrain->materialAt(i, j, y, h, top);

        // ABOVE THE GROUND IS NOT AUTOMATICALLY AIR, and it used to be. This
        // returned mat::AIR for every voxel over the surface, which meant a
        // lake was empty space to everything that was not the mesher -- the
        // swing ray, the held item, and any gameplay query that ever asks what
        // is here. The renderer knew about the water and nothing else did.
        //
        // Composed through VoxelTerrain::aboveAt so the answer cannot drift
        // from the one the mesher draws; tests/voxel_probe_test.cpp checks the
        // two against each other face by face.
        const int line = terrain->lakeLineAt(terrain->wx(i), terrain->wx(j), *memo_);
        const int wTop = terrain->waterTopVox(h, line, terrain->waveCeilVox(),
                                              terrain->wetColumn(i, j, h, line, *memo_));
        switch (VoxelTerrain::aboveAt(y, h, terrain->strandRows(i, j, top), wTop)) {
            case VoxelTerrain::Above::Blade: return top;
            // The churned band is its own material on the quad, so the probe
            // has to answer with it too or the renderer and the query disagree
            // about what a voxel of shore is.
            case VoxelTerrain::Above::Water:
                return terrain->foamColumn(h, line) ? mat::FOAM : mat::WATER;
            default: return mat::AIR;
        }
    }

    // IS THIS VOXEL UNDER WATER -- the question a body wants, and the one that
    // could not be asked at all before. Deliberately not folded into solid():
    // water does not stop a body or a swing, and conflating "there is something
    // here" with "it stops you" is what left the lake with nowhere to live.
    bool inWater(int i, int j, int y) {
        const uint8_t m = material(i, j, y);
        return m == mat::WATER || m == mat::FOAM;  // foam is water, churned
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
