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

#include "core/noise.h"
#include "core/vecmath.h"
#include "world/cover.h"
#include "world/dem.h"
#include "voxel/vox.h"

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
// THE BIRCH'S OWN GRASS. Six more greens, read off the BIRCH models the way
// GRASS_0 is read off the pines -- see deriveGroundFromTrees.
//
// A SECOND RAMP RATHER THAN A WIDER ONE, and the reason is that the two woods
// are different colours. GRASS_0 used to be filled from every foliage entry
// that loaded, pines and birches together, so the blades in both woods were a
// blend of the two and matched neither. Splitting them is the only way a birch
// meadow can be the birches' green and a pine floor the pines'.
constexpr uint8_t BGRASS_0 = 32;
constexpr uint8_t BGRASS_COUNT = 6;  // 32..37

// -- ...AND HOW MUCH OF IT THE FLOOR UNDER THE OAKS IS ALLOWED -------------
//
// (user 2026-09-17: "I want you to make the terrain in the oak forest match
//  the same green as the grass strands.")
//
// THE FLOOR AND THE BLADES WERE ALREADY THE SAME RAMP AND STILL DID NOT
// MATCH, which is the whole of this constant. topMaterial lays mat::BGRASS_0
// under the oaks and bladeMaterial grows mat::BGRASS_0 out of it, so there was
// no colour to correct -- but the two read the ramp in completely different
// ways, and they landed on disjoint halves of it:
//
//   * a BLADE reads it as a GRADIENT along its own height, one shade per
//     STRAND_ROW_STEP up from the soil. Short grass is 3 to 6 voxels, so a
//     blade tops out at shade (6-1)/2 == 2 and NEVER reaches 3, 4 or 5.
//   * the FLOOR reads it as a SCATTER, hashVoxel % BGRASS_COUNT, so half its
//     area wore the three lightest greens -- greens no blade in the wood has
//     ever shown.
//
// MEASURED over the oak band, albedo luminance, before: floor 0.3048 against
// blade 0.2358 -- THE GROUND WAS 29% BRIGHTER THAN THE GRASS STANDING IN IT.
// That is the same failure kBroadleafFloorV was introduced to fix one level
// up, and the same answer: the hue was never wrong, only the value.
//
// SO THE FLOOR SCATTERS OVER WHAT THE SWARD ACTUALLY REACHES, and that is not
// a fudge -- it is what the gradient already says. The ramp encodes a light
// field over height: a blade's base is buried in its own sward and sees no
// sky, its tip sees all of it. The floor BETWEEN the blades is at exactly the
// height of their bases, in exactly that shadow, so the bottom of the ramp is
// where it belongs. Reading the whole ramp there was lighting the ground as
// if it stood at the top of the grass.
//
// STILL A SCATTER, NOT A FLAT GREEN. Three shades rather than six is plenty
// to keep the floor off a painted plane -- sand does the job on four and rock
// on six over much flatter ground -- and collapsing it to one value is the
// mistake the note at the head of this file spends a paragraph on.
//
// DERIVED, NOT PICKED. See BLADE_RAMP_REACH below: raise grassMaxRows and the
// floor follows the grass up the ramp on its own.
constexpr uint8_t BGRASS_FLOOR_COUNT = 3;  // 32..34

// -- WHEAT: WHAT A TALL BLADE WEARS -----------------------------------------
//
// "can you make the tall grass have a more wheat color to them. maybe at the
// tips at the top its a brown, then a lighter brown to yellow/tan on the way
// back down." (user 2026-09-14), then, the same day: "actually make the base
// of the tall wheat grass, the same green as the grass green. then from there,
// go to wheat. a lighter color overal."
//
// IT ROOTS IN THE GREEN AND DRIES ON THE WAY UP, which is the whole shape:
// shade 0 IS the wood's own GRASS_0, byte for byte, so the foot of a tuft and
// the short blades standing around it are the same colour and the tuft grows
// OUT of the sward instead of being planted in it. From there it crosses to
// straw over four shades and spends the top half of the blade there.
//
// SO THERE ARE TWO RAMPS AFTER ALL, one per wood, and the first cut of this
// had one and argued for it: "a dried seed head is not foliage, so splitting
// it would be inventing a difference". That reasoning is sound about the DRY
// end and says nothing about the wet one -- and the base is now required to
// match a green that IS per-wood. A shared ramp would put the pines' green at
// the foot of every birch tuft, which is the exact mismatch GRASS/BGRASS was
// split to stop. The dry end still agrees between them; it is reached by
// blending toward one shared straw table.
//
// TEN SHADES, AND THE COUNT IS REACH RATHER THAN RESOLUTION. The device reads
// this ramp along the blade's height from the ground -- ABSOLUTE, not
// normalised per blade; see groundShade() -- at one shade per STRAND_ROW_STEP
// voxels. A tall blade is tallGrassMinRows..tallGrassMaxRows (15..20) voxels,
// so ten shades at a step of two is exactly the reach needed to grade the whole
// of one. Six, as the green ramps have, would be spent by voxel 12 and leave
// the top third of every tuft one flat colour.
//
// IT GETS LIGHTER ALL THE WAY UP, unlike the first cut, which ran tan down to a
// dark brown and was too heavy. Green is a dark albedo and straw is a bright
// one, so a blade drying out is a blade getting lighter -- and that agrees with
// the light field the green ramps already encode, where a base buried in its
// own sward sees no sky and a tip sees all of it. The two used to pull against
// each other; now they do not. The last two shades take the warm tan-brown of a
// seed head, which is a HUE turn rather than a fall in value.
//
// See fillWheatRamp, which is the one place any of this is arithmetic.
constexpr uint8_t WHEAT_0 = 38;
constexpr uint8_t WHEAT_COUNT = 10;  // 38..47, off the pines' green
// ------------------------------------------------------- THE GROUND'S COLOUR
// RECLAIMED FROM THE BIRCH. 48..57 is BWHEAT, the birch wood's wheat ramp, and
// the world is pine -- woodMix returns pure pine at every x, so nothing reaches
// those ten entries. They are the ground's colour now, filled from the aerial
// imagery's own pixels, because "the mountains are still grey" was true and the
// reason was that all bare ground was one flat mat::ROCK. Colorado's is not
// grey: the ramp this window produced is #887858, #a89868, #988868 -- tan and
// olive, which is what decomposed granite looks like.
//
// TEN ENTRIES AND NOT ONE MORE. The palette is 232 of 255 ([[v2-palette-is-full]]),
// so minting new entries risks the silent AIR failure; reusing dead ones costs
// nothing. If the birch wood is ever turned back on, these collide -- that is
// what --all-woods has to be checked against.
constexpr uint8_t BWHEAT_0 = 48;
constexpr uint8_t BWHEAT_COUNT = 10;  // 48..57, off the birches'
// -- TURNED EARTH, WHICH THE HOE MAKES AND NOTHING ELSE DOES --------------
//
// v1 MINTS THIS AT RUNTIME and says so at length: "the one palette id in the
// game minted at RUNTIME, so the sweeps that fill solidTab/digOnlyTab/SUP.CLASS
// beside the material tables cannot see it and it has to write its own" -- and
// then has to handle a FULL table by stealing the nearest existing shade, which
// silently re-describes whatever owned it for the rest of the session.
//
// None of that is necessary here. v2's fixed ids are a compile-time block below
// TREE_BASE with room in it, so tilled earth is simply one of them: authored
// with the rest of the ground, checked by the same static_asserts, and unable
// to collide with a model's colour by construction. The palette is at 227 of
// 255 and this is the 228th -- see the kit's own warning line.
constexpr uint8_t TILLED = 58;
// ...AND THE SEED ITSELF, WHICH SITS ON THE BED RATHER THAN BEING IT.
//
// (user 2026-09-14: "planting grass turns the tilled dirt to regular dirt. this
// is wrong. I should see 3 seeds in the form of 3 voxels on the tilled dirt".)
//
// THE FIRST CUT WROTE THIS OVER THE TILLED VOXEL, which is why it read as the
// bed un-tilling itself: a sown bed was a DIFFERENT BROWN where a tilled one
// had been, so the turned earth you had just made went away the moment you
// planted in it. Two browns a shade apart is not "there is seed here", it is
// "the thing you did came undone".
//
// A SEED GOES ON TOP. The till leaves the old surface voxel as AIR -- that is
// what lowering the column means -- so there is a free voxel directly over
// every tilled column, and three of them wearing this is three seeds lying on
// the bed. Nothing about the bed changes.
// -- THREE OF THEM, AND THEY ARE THE HELD MODEL'S OWN COLOURS ----------
//
// (user 2026-09-14: "have the seeds that are planted in the ground match the
// color voxels as the seeds in hand. they are different colors.")
//
// THE FIRST CUT INVENTED A COLOUR. It was pale straw -- a reasonable guess at
// what a seed looks like, and nothing to do with the art: seeds.vox is THREE
// VOXELS IN THREE BRIGHT GREENS, sRGB (171,255,76), (110,255,84) and
// (83,255,121). Holding one thing and planting another is exactly what was
// reported.
//
// THREE IDS RATHER THAN ONE, because the model has three colours and a planting
// puts down three voxels -- so each planted seed can be one of the model's own
// voxels rather than an average of them. The ramp idiom is the engine's own:
// see GRASS_0, SOIL_0, SAND_0, every one of which is a run of shades under one
// name with an isX() predicate over it.
constexpr uint8_t SEED_0 = 59;
constexpr uint8_t SEED_COUNT = 3;   // 59..61
// ------------------------------------------------------- THE GROUND'S COLOUR
// Ten entries of its own, filled from the aerial imagery's own pixels, because
// "the mountains are still grey" was true and the cause was that all bare
// ground was one flat mat::ROCK. Colorado's is not grey: the ramp a Cheesman
// window produces runs #383828 to #b8a878, dark olive to pale tan, which is
// what decomposed granite looks like.
//
// THESE ARE NEW SLOTS, NOT RECLAIMED ONES, and that cost a whole build to
// learn. The first attempt aliased them onto BWHEAT (48..57) on the reasoning
// that the birch wood is unreachable while the world is pine -- but isWheat()
// tests the RANGE, so every voxel painted with the ground ramp came back as
// wheat: breakable, blade-rendered, and indistinguishable from a wheat field.
// A dead range is not a free range while a predicate still claims it. Ten of
// the twenty-three spare entries is the honest price.
constexpr uint8_t GROUND_0 = 62;
constexpr uint8_t GROUND_COUNT = 10;   // 62..71
// ------------------------------------------------------------------- SNOW
// (user 2026-09-18: "I want you to add snow to the tall mountain peaks.")
//
// THREE SHADES, FOR THE REASON THE GROUND RAMP GIVES ABOVE. One value over a
// whole summit reads as a painted plane because the eye finds the repeat
// instantly, and snow is the worst case for that -- it is the brightest thing
// in the world and it covers the largest unbroken surfaces.
//
// NEW SLOTS, NOT RECLAIMED ONES, and the note over GROUND_0 is why: a dead
// range is not a free range while a predicate still claims it. Three of the
// thirteen entries that were still spare, taking the table from 242 to 245.
//
// THEY SIT BETWEEN THE GROUND RAMP AND TREE_BASE so the one invariant this
// file has about ids still holds -- everything the terrain paints is below
// TREE_BASE, which is where the model palette starts allocating. The
// static_asserts in world.h check both links of that chain.
constexpr uint8_t SNOW_0 = 72;
constexpr uint8_t SNOW_COUNT = 3;   // 72..74
constexpr uint8_t TREE_BASE = 75;  // model palette entries are allocated from here up
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
    // See V6Material::alpha in Shared.slang. OPAQUE BY DEFAULT and every
    // material in the world leaves it alone -- Palette::setAlpha is the only
    // writer and the fly's wing is its only subject.
    float alpha = 1.0f;
    // See V6Material::ior. 0 is "an ordinary surface", which is everything but
    // the smoke; Palette::setDielectric is the only writer.
    float ior = 0.0f;
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
    // -----------------------------------------------------------------------
    // NEAR-IDENTICAL COLOURS SHARE AN ENTRY.
    //
    // THE TABLE IS 255 ENTRIES AND THAT IS A FORMAT LIMIT, NOT A BUDGET. A
    // material id is eight bits of the packed triangle word (material 0-7,
    // direction 8-10, strand 11-15) and that word is full, so the ceiling
    // cannot be raised without widening every triangle in the engine.
    //
    // MEASURED WHEN THE TOOLS DISAPPEARED: the models between them asked for
    // 372 colours. 255 were served and **117 were returned as AIR** -- and
    // because the held items are the LAST thing to register, what went missing
    // was every stone head in the game. Nothing said so; Palette::overflowed()
    // had counted it since the table was written and nothing ever called it.
    //
    // So the key is QUANTISED. Voxel art routinely carries five or six shades a
    // couple of units apart -- a bass alone brought eighteen -- and at this step
    // those collapse onto one entry. The step is small enough to be invisible:
    // the worst error is half of it, which is under two per cent of a channel
    // and well inside the tone curve's own rounding.
    //
    // THE FIRST COLOUR IN A BUCKET WINS AND ITS EXACT VALUE IS WHAT RENDERS --
    // the quantisation decides only whether two colours SHARE, never what is
    // drawn. So nothing shifts hue; some things merely stop being distinct.
    //
    // WHAT IT COSTS is ramp resolution. A model's own shading ramp can lose a
    // step where two of its shades fall in one bucket, which on a six-shade
    // trunk is a trunk with five. That is the trade against a stone axe head
    // that is not drawn at all.
    //
    // -- AND AT TEN IT WAS COSTING TOO MUCH (user 2026-09-13: "the axe doesnt
    // show the right pallette", and again "keep pursuing the stone tools
    // pallete errors") -------------------------------------------------------
    //
    // MEASURED ON THE THING THAT WAS COMPLAINED ABOUT. The stone axe's head is
    // a SEVEN STEP grey ramp -- 107, 113, 120, 127, 134, 140, 147 -- hand
    // painted to give nineteen voxels a shape. Quantised at 10 it comes back as
    // FIVE: 120 merges with 127 and 140 with 147, and two of the seven steps
    // the artist drew stop existing. That is a palette error in the exact sense
    // of the word, it was introduced by the fix for the overflow, and it is
    // what "not right" is.
    //
    //     step 10   7 greys -> 5 entries
    //     step  8   7 greys -> 6
    //     step  6   7 greys -> 7      <-- the whole ramp survives
    //
    // AND SIX DOES NOT FIT. Measured, not guessed: at 6 the world asks for more
    // than the table has and the start-up report reads
    //
    //     PALETTE FULL -- 61 colours could not be registered and render as AIR
    //
    // which is the disappearing-tools bug all over again, and worse. The step
    // stays at TEN for the world, and what changes instead is that the HELD
    // ITEMS stop going through it -- see `exact` below. Twenty-odd voxels in
    // front of the eye are the art anybody actually looks at, and there are few
    // enough of them to pay for in full.
    //
    // v1 SAYS THE SAME THING FROM THE OTHER SIDE. Its note on the view-model
    // light refuses to apply the world's per-voxel grain to a held item because
    // "it scrambles hand-authored .vox gradients (the axe handle steps by
    // ~5%)". A 5% step is six units of 255 -- under this step, and over the
    // exact one. Both engines end up protecting the same thing.
    static constexpr int kQuantStep = 10;

    // -- AND A COARSER GRID FOR THE THINGS THAT MOVE ----------------------
    //
    // "The newly imported life is missing voxels" (user 2026-09-14), which is
    // this table full and forModelColor handing back AIR -- measured at 255 of
    // 255 with 16 distinct colours refused across 109 calls, and what goes
    // without is whatever loaded last, which is the newest animal.
    //
    // MEASURED, PER GROUP, off the real art (the harness counts distinct keys
    // at a range of steps):
    //
    //                                 step10  step14  step18  step24
    //     world: trees, rocks, decor      91      87      76      66
    //     the flyer band, deduplicated   158     136     113      97
    //
    // ...against a budget of about 194: the table is 255, the terrain ramps
    // take the first 38, and the held kit reserves 23 EXACTLY (see `exact`).
    // 91 + 158 does not fit and never did -- the band has been one animal away
    // from this since the fish landed.
    //
    // -- AND SNAPPING TO A COARSER GRID IS THE WRONG WAY TO GET IT ---------
    //
    // The first fix rounded a band colour onto a 24-unit lattice before
    // registering it. It fit -- and it broke the skunk, which is the one animal
    // in the wood that is black and white. Rounding each channel INDEPENDENTLY
    // pulls a near-neutral apart, because r, g and b sit near different bucket
    // edges. MEASURED on the shipped art:
    //
    //     (  0,  0,  0) -> ( 12, 12, 12)   the black body, lifted to grey
    //     ( 44, 44, 48) -> ( 36, 36, 60)   a neutral dark, gone BLUE
    //     ( 50, 49, 43) -> ( 60, 60, 36)   a warm grey, gone OLIVE
    //     (234,238,246) -> (228,228,252)   a cool white, gone blue
    //
    // A hue cast on a neutral is far more visible than a loss of gradient, and
    // that is what "the skunk colours are broke, voxels are showing but colours
    // are wrong" was.
    //
    // WHAT WORKS IS MATCHING, NOT MOVING. If a colour is within kModelMatch of
    // one the table already holds, it gets that entry -- and if it is not, it
    // is minted EXACTLY as authored. Black stays black, because nothing near
    // black is in the table until the skunk puts it there. Same measurement,
    // same art, nearest-match at 16:
    //
    //     (  0,  0,  0) -> (  0,  0,  0)   exact
    //     ( 44, 44, 48) -> ( 47, 47, 47)   still neutral
    //     worst error 12.9/255 against the snap's 20.8, and 160 entries
    //     against its 177
    //
    // THIS IS v1'S palShare TOLERANCE, and v1's own warning comes with it: an
    // id is a MATERIAL. Over there a pink bird landed 5/255 from the cactus
    // flower, inherited cactusTab, and stung the player. Here the materials
    // that MEAN something are the terrain families -- grass, soil, litter,
    // stone, sand, water, foam -- and they are all below TREE_BASE, so the
    // search starts there and can never hand a model a shade the world
    // re-rolls per voxel in groundShade.
    static constexpr int kModelMatch = 16;

    // The nearest MODEL entry to this colour within `tol`, or 0 for none.
    // Euclidean in sRGB, which is what the art was authored in.
    // `avoidFoliage` keeps a creature's colour off an id that carries BEHAVIOUR.
    // v1 learned this the hard way and its note is the clearest statement of it:
    // a body stamped with a foliage id "stops colliding", one wearing a cactus
    // id stings. v2's foliage bit does the same two jobs -- it is the fell
    // collider's wood/leaf classifier and it turns on translucency -- so a
    // rabbit snapped onto a leaf would go see-through and read as crown to
    // World::fellTree. Scenery may land on scenery; life may not.
    uint8_t nearestModelColor(const std::array<uint8_t, 4> &c, int tol,
                              bool avoidFoliage = false) const {
        int best = tol * tol + 1;
        uint8_t hit = 0;
        for (int id = mat::TREE_BASE; id < int(next_); ++id) {
            if (avoidFoliage && foliage_[size_t(id)]) continue;
            const Vec3 a = look_[size_t(id)].albedo;
            const int dr = srgbByte(a.x) - int(c[0]);
            const int dg = srgbByte(a.y) - int(c[1]);
            const int db = srgbByte(a.z) - int(c[2]);
            const int d = dr * dr + dg * dg + db * db;
            if (d < best) {
                best = d;
                hit = uint8_t(id);
            }
        }
        return hit;
    }

    // -----------------------------------------------------------------------
    // THE NEAREST CANOPY ENTRY, WHICH IS A DIFFERENT QUESTION FROM THE NEAREST
    // ENTRY -- and asking the general one instead is a real bug, found here.
    //
    // The tree fruit carries three to five voxels of stem and leaf, authored
    // (171,178,100), and it should wear the crown it hangs in. Asked of
    // nearestModelColor over the whole model range it came back with
    // (180,180,108), 12 away and NOT GREEN -- a straw shade off some other
    // model that simply happens to sit closer in RGB than any leaf does. The
    // nearest canopy green is 29 away, and 29 away is the right answer,
    // because the question is "which leaf" and not "which colour".
    //
    // This is what the browser engine means by `near(fj.pal[fj.nbody],
    // OAKLEAF)`: it hands the search a LIST of canopy ids rather than the
    // table. Here the list already exists -- foliage_ is set by forModelColor
    // as each entry is classified -- so the restriction is one test.
    //
    // Returns 0 if nothing has been classified as foliage yet, which is a real
    // state (no tree model loaded) and is why every caller has a fallback.
    // -----------------------------------------------------------------------
    uint8_t nearestFoliage(const std::array<uint8_t, 4> &c) const {
        int best = 1 << 30;
        uint8_t hit = 0;
        for (int id = mat::TREE_BASE; id < int(next_); ++id) {
            if (!foliage_[size_t(id)]) continue;
            const Vec3 a = look_[size_t(id)].albedo;
            const int dr = srgbByte(a.x) - int(c[0]);
            const int dg = srgbByte(a.y) - int(c[1]);
            const int db = srgbByte(a.z) - int(c[2]);
            const int d = dr * dr + dg * dg + db * db;
            if (d < best) {
                best = d;
                hit = uint8_t(id);
            }
        }
        return hit;
    }

    // The inverse of what forModelColor stores. One place, so a comparison
    // against an authored colour cannot drift from the conversion that made it.
    static int srgbByte(float linear) {
        const float s = (linear <= 0.0031308f) ? linear * 12.92f
                                               : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
        const int q = int(s * 255.0f + 0.5f);
        return q < 0 ? 0 : (q > 255 ? 255 : q);
    }

    // WHICH KEY SPACE AN ENTRY LIVES IN. An exact key is the colour itself and
    // a quantised one is the colour over kQuantStep, and the two ranges OVERLAP
    // numerically -- (18, 24, 20) exact is the same integer as (180, 240, 200)
    // quantised. One bit above both keeps them apart, so an exact request can
    // never be handed a bucket a rougher colour opened.
    static constexpr uint32_t kExactKeyBit = 0x1000000u;

    // `exact` -- DO NOT MERGE THIS COLOUR WITH ANYTHING. For the handful of
    // models that are held in front of the eye, where a shading ramp is the
    // whole of the art and losing a step of it is visible. Everything the WORLD
    // is made of still quantises: there are 372 colours out there and 255
    // places to put them.
    // `matchTol` -- IF IT IS NOT IN THE TABLE, IS SOMETHING CLOSE ENOUGH?
    // Zero for the world, which mints what it asks for; kModelMatch for the
    // flyer band, whose 158 distinct colours do not fit beside the world's 91.
    // See the note over kModelMatch for why this and not a coarser step.
    uint8_t forModelColor(const std::array<uint8_t, 4> &c, bool conifer = true,
                          bool exact = false, int matchTol = 0) {
        const uint32_t qr = uint32_t(c[0] / kQuantStep), qg = uint32_t(c[1] / kQuantStep),
                       qb = uint32_t(c[2] / kQuantStep);
        const uint32_t key = exact ? (kExactKeyBit | (uint32_t(c[0]) << 16) |
                                      (uint32_t(c[1]) << 8) | uint32_t(c[2]))
                                   : ((qr << 16) | (qg << 8) | qb);
        auto it = index_.find(key);
        if (it != index_.end()) return it->second;
        // NOT IN ITS OWN BUCKET, BUT NEAR SOMETHING. Cached under this key, so
        // the next voxel of the same colour costs a hash lookup rather than a
        // scan of the table.
        //
        // NOT NAMED `near`. windows.h still defines near and far as empty
        // macros from the segmented-memory era, so the declaration compiles as
        // `const uint8_t = ...` and the error names neither -- the FIFTH time
        // in this project. birds.h, app.h and lake.h all carry the same note.
        if (matchTol > 0) {
            const uint8_t shared = nearestModelColor(c, matchTol);
            if (shared) {
                index_[key] = shared;
                return shared;
            }
        }
        if (next_ >= mat::COUNT) {
            ++overflow_;
            // ...AND HOW MANY DISTINCT COLOURS THAT REALLY IS, which is not the
            // same number and is the only one worth acting on.
            //
            // overflow_ counts failed CALLS. Once the table is full, a colour
            // that would have DEDUPED fails again on every lookup -- so a single
            // refused shade that fifty models happen to share reports as fifty,
            // and the total inflates nonlinearly with how many models ask rather
            // than with how much you cannot see. Measured here: reserving the
            // held kit early moved the shortfall off the bow and the count went
            // from 14 to 240, which reads like a catastrophe and is ONE colour
            // asked for by the whole flyer band.
            //
            // The keys are already the dedup identity, so a set of the refused
            // ones is the honest count. It is bounded by how many distinct
            // colours the world wanted and cannot grow without bound.
            overflowKeys_.insert(key);
            // -- SNAP TO THE NEAREST COLOUR, DO NOT HAND BACK AIR -----------
            //
            // v1's palNearest, and its note is the argument: "the wrong colour
            // is a bug you can see, and a wrapped id is one you cannot". v2's
            // version of the invisible bug is this line returning AIR -- the
            // voxel is not drawn at all, so a model that loads once the table
            // is full comes out with HOLES in it, silently, and what you cannot
            // see is whatever loaded last.
            //
            // Snapping costs a shade. It cannot cost a voxel, and it cannot
            // cost the thing that made "the newly imported life is missing
            // voxels" take a day to find.
            //
            // THE OVERFLOW COUNTERS ARE STILL KEPT. This is a fallback, not a
            // licence: the startup line and the stderr warning still report
            // exactly how many distinct colours could not be minted, so a table
            // that has actually run out still says so loudly.
            const uint8_t snapped = nearestModelColor(c, 1 << 15, /*avoidFoliage=*/!conifer);
            return snapped ? snapped : mat::AIR;
        }

        const uint8_t id = next_++;
        // -- WHAT THE ART ACTUALLY CARRIED ------------------------------
        //
        // Kept because the albedo stops being an answer to that question three
        // lines below: a needle is lifted 1.7x with a floor under its blue, so
        // srgbByte(albedo) on a foliage entry hands back a green that is in no
        // .vox file anywhere and that nothing would ever match against.
        //
        // The plate written by --palette-vox exists to be authored AGAINST --
        // a colour picked off it has to land in the same kQuantStep bucket as
        // the colour that minted the entry, or the pick is only a resemblance
        // -- and the authored value is the only one that can promise that.
        src_[id] = {c[0], c[1], c[2]};
        srcExact_[id] = exact ? 1u : 0u;
        MaterialLook &m = look_[id];
        m.albedo = Vec3(srgbToLinearF(float(c[0]) / 255.0f), srgbToLinearF(float(c[1]) / 255.0f),
                        srgbToLinearF(float(c[2]) / 255.0f));

        // Green-dominant is foliage; anything else on a conifer is wood.
        const bool foliage = conifer && (c[1] > c[0] && c[1] > c[2]);
        // ...AND THE ANSWER IS KEPT, because the physics wants it too. A felled
        // tree's collider is built from the wood and not from the crown -- see
        // World::fellTree -- and the only place in the engine that knows a
        // green from a bark is right here, where the colour is still in hand.
        // Recovering it later from the MaterialLook would be inferring a fact
        // that was already known and thrown away.
        foliage_[id] = foliage ? 1u : 0u;
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

    // ...AND WHERE THE BIRCHES START INSIDE THAT RANGE. loadPines() loads both
    // species into one vector, so pineEnd_ alone cannot tell a needle from a
    // birch leaf -- it only separates the trees from the rocks. Called by the
    // loader as it reaches the first birch model.
    void markBirchStart() { birchStart_ = next_; }

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

    // THE GROUND BAND, straight from the imagery. Ten rgb triples, most common
    // bare ground first. Called before World::build -- a colour minted after it
    // renders flat 170-grey until the table is uploaded again, which is the
    // failure in [[v2-palette-uploads-and-the-level-table]].
    void setGroundBand(const uint8_t *rgb, int n) {
        for (int k = 0; k < mat::GROUND_COUNT; ++k) {
            const int i = (n > 0) ? (k * n / mat::GROUND_COUNT) : 0;
            const uint8_t r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
            set(uint8_t(mat::GROUND_0 + k), srgbToLinearF(float(r) / 255.0f),
                srgbToLinearF(float(g) / 255.0f), srgbToLinearF(float(b) / 255.0f),
                0.90f);
        }
    }

    const MaterialLook &operator[](uint8_t id) const { return look_[id]; }

    // -- WHAT ENTRY DID THAT COLOUR REALLY END UP ON? ------------------------
    //
    // WRITTEN BECAUSE AN ASSUMPTION ABOUT IT WAS WRONG, twice in one afternoon.
    // A caller that wants a material to itself -- the firefly's glow, the fly's
    // wing -- registers its colour with `exact` and then keeps the id. That id
    // is NOT necessarily the one the model wears: exact stores under a key of
    // its own (kExactKeyBit), while the model's own voxels come back through
    // the SNAP, and nearestModelColor takes the first strictly-nearest entry it
    // finds scanning upward. An earlier entry of the same colour therefore wins
    // -- the asset deck's pure white beat a pure-white wing registered after it,
    // and the wing's material sat in the table wearing nothing.
    //
    // The symptom was nothing at all: the render was pixel-for-pixel identical
    // with the wing alpha at 0 and at 1, which is exactly what an orphaned
    // material looks like.
    //
    // So ASK, rather than assume. This is the same resolution the snap does,
    // without registering anything, and `sharing` says how many OTHER entries
    // are close enough that the answer could have gone elsewhere -- which is the
    // number that decides whether a private material is really private.
    uint8_t resolveModelColor(const std::array<uint8_t, 4> &c, int tol, int *sharing = nullptr) const {
        if (sharing) *sharing = 0;
        uint8_t hit = nearestModelColor(c, tol);
        if (sharing && hit) {
            for (int id = mat::TREE_BASE; id < int(next_); ++id) {
                const Vec3 a = look_[size_t(id)].albedo;
                const int dr = srgbByte(a.x) - int(c[0]);
                const int dg = srgbByte(a.y) - int(c[1]);
                const int db = srgbByte(a.z) - int(c[2]);
                if (dr * dr + dg * dg + db * db <= tol * tol) ++(*sharing);
            }
        }
        return hit;
    }

    // -- THE ONE WAY A MATERIAL BECOMES SEE-THROUGH --------------------------
    //
    // A SETTER RATHER THAN A FIELD ON forModelColor, because being see-through
    // is a property of one entry and not of the colour that asked for it: the
    // caller has to REGISTER ITS COLOUR EXACTLY first (see kModelMatch), or the
    // snap hands it an entry the rest of the world is already wearing and half
    // the wood goes transparent. The firefly's emissive material is claimed the
    // same way and for the same reason.
    void setAlpha(uint8_t id, float a) {
        if (!id) return;   // AIR
        look_[size_t(id)].alpha = a;
    }
    // ...AND THE WHOLE SURFACE, for the one entry that is a DIELECTRIC rather
    // than a see-through version of an ordinary one. See kHouseflyWingAlpha:
    // being glass is four numbers that have to move together, and setting only
    // the first is what made a fly's wing read as torn paper.
    //
    // The albedo is SCALED, not replaced: the hue is what the art authored and
    // the lightness is what a dielectric is allowed to have of its own.
    // -- THE OTHER WAY TO BE SEE-THROUGH, AND THE ONE THAT IS NOT NOISY ----
    //
    // setAlpha above is a STOCHASTIC pass-through; this is water's own path, a
    // delta lobe with refraction, and it is deterministic. See V6Material::ior
    // for the measurement and for why the smoke needed it.
    //
    // THE ENTRY HAS TO BE PRIVATE, exactly as setAlpha's does and for the same
    // reason: being a dielectric belongs to the ENTRY, so anything else wearing
    // it turns to glass too.
    void setDielectric(uint8_t id, float ior, float rough = 0.0f) {
        if (!id) return;   // AIR
        look_[size_t(id)].ior = ior;
        look_[size_t(id)].roughness = rough;
    }

    void setGlass(uint8_t id, float a, float rough, float spec, float diffuse) {
        if (!id) return;   // AIR
        MaterialLook &m = look_[size_t(id)];
        m.alpha = a;
        m.roughness = rough;
        m.specular = spec;
        m.albedo = m.albedo * diffuse;
    }
    // Is this material id a LEAF rather than wood? False for everything that
    // is not a model colour, which is the right answer for all of them: the
    // ground, the water and the stone are none of them foliage.
    bool isFoliage(uint8_t id) const { return foliage_[id] != 0u; }
    // -- THE COLOUR THAT ASKED FOR THIS ENTRY, as against the one it renders.
    //
    // False for everything below mat::TREE_BASE and for anything unminted: the
    // terrain band is built from linear numbers in buildGround and derived from
    // the trees afterwards, so no .vox file authored it and there is nothing
    // honest to report. Read srgbByte(albedo) for those -- they are not lifted,
    // and they are not matchable either (nearestModelColor starts at TREE_BASE).
    bool authoredColor(uint8_t id, std::array<uint8_t, 3> *out) const {
        if (id < mat::TREE_BASE || id >= next_) return false;
        if (out) *out = src_[id];
        return true;
    }
    // Was it minted unquantised? That is the held kit and nothing else -- see
    // the `exact` argument of forModelColor.
    bool authoredExact(uint8_t id) const { return srcExact_[id] != 0u; }
    const std::vector<MaterialLook> &table() const { return look_; }
    int used() const { return next_; }

    // -----------------------------------------------------------------------
    // -- A SECOND TABLE, FOR A PLACE THE WOOD IS NEVER ON SCREEN WITH -------
    //
    // (user 2026-09-17: "can you make sure that the nuketown map is using a
    // different color palette from the real world one? They exist in seperate
    // levels so surely we can have multiple color paletes for multiple
    // worlds?")
    //
    // YES, AND IT IS THE ONLY REAL ANSWER TO A TABLE THAT KEEPS FILLING UP.
    // 255 is a FORMAT limit -- eight bits of the packed triangle word, see the
    // note over forModelColor -- so it cannot be raised. But it is 255 entries
    // PER TABLE, and the level is a separate place by construction: the wood is
    // not in the acceleration structure while you are there, so its trees, its
    // animals and its flowers cannot be on screen to disagree about what entry
    // 200 means. Two tables, swapped at the door, and the level stops competing
    // with the wood for colours it will never be seen beside.
    //
    // WHAT MUST STILL AGREE, and this is the whole of the danger: anything
    // drawn in BOTH places. That is the held kit (you carry it through the
    // door), the fixed mat:: band the level borrows -- BGRASS for the lawns,
    // ROCK for the flex -- and the wood's FLOWER models, whose voxels are
    // stamped into the level. The caller passes those in as `reserved` and this
    // allocator never touches them.
    //
    // TOP-DOWN, so the level's own colours land as far from the fixed band as
    // the table allows and a reserved range low down is never in the way.
    //
    // MERGING IS AGAINST THE LEVEL'S OWN ENTRIES ONLY. Two colours that would
    // have shared in the wood's table share here too; what does not happen any
    // more is a level colour being bent onto a TREE's green because the tree
    // got there first.
    void beginLevelTable(const std::vector<uint8_t> &reserved) {
        levelLook_ = look_;
        levelReserved_ = reserved;
        levelReserved_.resize(256, 0);
        levelIndex_.clear();
        // THE LAST REAL ENTRY, WHICH IS 254 AND NOT 255. The table is
        // mat::COUNT long and mat::COUNT IS 255, so the ids run 0..254 -- a
        // top-down allocator that starts at the count writes one past the end
        // on its very first colour. It did, and the symptom named nothing: the
        // heap corruption sat quiet through the whole of start-up and the
        // engine then died between the last line it prints and its first frame,
        // with no error on either stream and nothing in the log to say which of
        // the day's changes had done it.
        levelNext_ = int(levelLook_.size()) - 1;
        levelMinted_ = 0;
        levelOverflow_ = 0;
        // -- WHICH ENTRIES ARE ACTUALLY THE LEVEL'S -- and it is NOT "all of
        // them". levelLook_ starts as a COPY of the wood's table, so most of it
        // is the wood's colours frozen at this instant, and a frozen copy is
        // wrong for anything registered later that is drawn in BOTH places --
        // the held kit above all. World::uploadMaterials reads this mask and
        // takes the wood's LIVE entry wherever the level never minted one, so
        // the only thing that differs between the two tables is what the level
        // itself put there.
        levelOwn_.assign(256, 0);
        haveLevel_ = true;
    }

    uint8_t forLevelColor(const std::array<uint8_t, 4> &c) {
        if (!haveLevel_) return forModelColor(c, false);
        const uint32_t key = (uint32_t(c[0] / kQuantStep) << 16) |
                             (uint32_t(c[1] / kQuantStep) << 8) | uint32_t(c[2] / kQuantStep);
        auto it = levelIndex_.find(key);
        if (it != levelIndex_.end()) return it->second;
        while (levelNext_ > int(mat::TREE_BASE) && levelReserved_[size_t(levelNext_)]) --levelNext_;
        if (levelNext_ <= int(mat::TREE_BASE)) {
            ++levelOverflow_;
            return mat::AIR;
        }
        const uint8_t id = uint8_t(levelNext_--);
        MaterialLook &m = levelLook_[id];
        m = MaterialLook{};
        m.albedo = Vec3(srgbToLinearF(float(c[0]) / 255.0f), srgbToLinearF(float(c[1]) / 255.0f),
                        srgbToLinearF(float(c[2]) / 255.0f));
        // A LEVEL IS BUILT, NOT GROWN. There is no conifer rule here and there
        // should not be: the foliage branch in forModelColor exists to stop a
        // pine canopy reading as a black cut-out, and a green wall in a map is
        // a green wall. Bark's roughness for everything, which is what that
        // function gives anything that is not a needle.
        m.roughness = 0.88f;
        m.specular = 0.020f;
        m.translucency = 0.0f;
        levelIndex_.emplace(key, id);
        levelOwn_[id] = 1;
        ++levelMinted_;
        return id;
    }

    // Lend the level an entry it must NOT allocate for itself -- the bulb's
    // glass, which is the tracer's emitter test and has to be one known id.
    const std::vector<MaterialLook> &levelTable() const { return levelLook_; }
    bool hasLevelTable() const { return haveLevel_; }
    // Did the LEVEL mint this entry, as against inheriting it from the wood?
    // See beginLevelTable, and World::uploadMaterials, which is the only
    // caller and the reason this exists.
    bool levelOwns(uint8_t id) const {
        return haveLevel_ && id < levelOwn_.size() && levelOwn_[id] != 0;
    }
    int levelMinted() const { return levelMinted_; }
    int levelOverflowed() const { return levelOverflow_; }
    // How many entries the level could still take. Printed at start-up so the
    // day this one fills up is a number rather than a surprise, exactly as the
    // wood's own line is.
    int levelFree() const {
        int n = 0;
        for (int i = int(mat::TREE_BASE) + 1; i <= levelNext_; ++i)
            if (!levelReserved_[size_t(i)]) ++n;
        return n;
    }
    int overflowed() const { return overflow_; }
    // HOW MANY COLOURS YOU ACTUALLY CANNOT SEE, as against how many times the
    // table said no. Prefer this one in any report a human reads: overflowed()
    // is call count and inflates with the number of models that share a refused
    // shade. See the overflow branch in forModelColor.
    int overflowedColors() const { return int(overflowKeys_.size()); }
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
    // HOW MUCH OF THE CANOPY'S VALUE THE FLOOR UNDER IT KEEPS.
    //
    // "make the grass/grass strands in the oak forest match the colors of the
    //  oak trees foilage"                                  -- user, 2026-09-17
    //
    // THE HUE ALREADY MATCHED AND THE VALUE DID NOT, which is the only reason
    // this is one number rather than a new ramp. Measured off the running
    // engine before changing anything -- the broadleaf foliage entries against
    // the ramp built from them, both as sRGB bytes:
    //
    //     foliage   (110,151, 66) (137,185, 71) (140,183,102) (173,215,116)
    //     BGRASS    ( 83,115, 51) ( 97,129, 68) (105,138, 73) (130,162, 87)
    //
    // Same hue family throughout, uniformly about 27% down in sRGB. And the
    // oak has no quarrel with the BIRCH here either: an oak green lands within
    // 14 of a birch one and the oak set loads at matchTol 30, so the oaks mint
    // almost nothing and wear the birches' entries -- the ramp built from those
    // entries is already the oak's own colour. There was nothing to correct
    // except how dark it is.
    //
    // WHY IT IS SCALED AT ALL, AND WHY 0.52 WAS NEARLY RIGHT. A foliage entry
    // is not the authored colour: forModelColor lifts it 1.7x with a floor
    // under the blue, because a needle sits inside its own crown in shade. So
    // 1/1.7 = 0.588 is "undo the shade lift", and 0.52 is just under it -- the
    // old shared constant was, in effect, the sunlit reading of the canopy
    // colour, and defensible.
    //
    // WHY THE BROADLEAF WOODS GET MORE ANYWAY. The lift's premise is a closed
    // crown, and it is strongest exactly where it is least true: a birch and an
    // oak wood are bright, open-floored and lit through the leaves rather than
    // under them. Pushing the broadleaf floor above "undo the lift" is reading
    // that transmitted light into the albedo, which is the cheap way to get it
    // -- there is no subsurface term here to get it honestly.
    //
    // THE CONIFER FLOOR IS LEFT ALONE at the old value. A pine wood's floor is
    // needle duff in deep shade; it is dark because it is, and the note under
    // GRASS_0 about the floor glowing under a dark canopy is about this ramp.
    // Sharing one constant would have brightened it for a complaint that was
    // never about it.
    static constexpr float kConiferFloorV = 0.52f;
    static constexpr float kBroadleafFloorV = 0.72f;
    // HOW HARD THE BROADLEAF GREENS ARE PULLED APART -- see the contrast block
    // in fillGrassRamp for what this does and why it pivots rather than
    // stretches.
    //
    // 2.5 takes the three shades the oak wood actually shows from a 1.53x
    // spread to 2.91x, which is the difference between a floor that reads as
    // one painted green and one you can see the blades standing in. It is a
    // CONTRAST control and not a brightness one: the mean of those three moves
    // 0.2497 -> 0.2529.
    static constexpr float kBroadleafContrast = 2.5f;
    // The most any of these may reflect. A leaf is not a mirror and an albedo
    // over 1 is not a bright surface, it is a broken one -- see the cap in
    // fillGrassRamp's contrast block.
    static constexpr float kAlbedoCeil = 0.85f;

    // -----------------------------------------------------------------------
    // ONE RAMP FILL, USED BY BOTH WOODS. The pines' greens become GRASS_0 and
    // the birches' BGRASS_0; everything about how a ramp is built is here once
    // so the two cannot drift apart.
    // `valueScale` is how much of the FOLIAGE's value the floor keeps; see the
    // long note on it below, and kConiferFloorV / kBroadleafFloorV for why the
    // two woods do not use the same number.
    void fillGrassRamp(uint8_t base, uint8_t count, std::vector<uint8_t> src,
                       float valueScale, float contrast = 1.0f, int pivotK = -1) {
        std::sort(src.begin(), src.end(), [this](uint8_t a, uint8_t b) {
            return luminance(look_[a].albedo) < luminance(look_[b].albedo);
        });

        for (int k = 0; k < int(count); ++k) {
            MaterialLook &m = look_[base + k];
            if (src.empty()) {
                // No models loaded: a plain green, so the world still renders.
                m.albedo = Vec3(0.14f + 0.04f * k, 0.26f + 0.05f * k, 0.10f + 0.02f * k);
            } else {
                const float t = (int(count) > 1)
                                    ? float(k) / float(int(count) - 1) *
                                          float(src.size() - 1)
                                    : 0.0f;
                const size_t i0 = size_t(t);
                const size_t i1 = mini(int(src.size()) - 1, int(i0) + 1);
                const float f = t - float(i0);
                const Vec3 c = look_[src[i0]].albedo * (1.0f - f) +
                               look_[src[i1]].albedo * f;
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
                m.albedo = c * valueScale + Vec3(0.006f, 0.008f, 0.005f);
            }
            m.roughness = 0.88f;
            m.specular = 0.022f;
            // Thin blades, lit from behind at a low sun -- the same reason the
            // needles have it. Without it a strand is a black stick at dawn.
            // Kept well below the needles' 0.45: grass caught the sun from
            // every angle at that value and the whole floor glowed.
            m.translucency = 0.22f;
        }

        // -- ...AND THEN PULLED APART, BECAUSE IT READ AS ONE COLOUR -------
        //
        // (user 2026-09-17: "in the oak forest, I want more contrast between
        //  the terrain/grass voxels. basically all of the grass. it looks like
        //  one color.")
        //
        // AND IT MEASURED AS ONE COLOUR. Everything green in the oak wood --
        // the floor and every blade standing on it -- lives in shades 0..2 of
        // this ramp, because a blade reads it as a gradient at one shade per
        // STRAND_ROW_STEP and short grass is only 3 to 6 voxels tall. Those
        // three shades spanned luminance 0.1944 to 0.2982: a range of 1.53x,
        // or sRGB (96,132,59) to (122,160,85). They are close enough that the
        // eye reads a single flat green, which is the report.
        //
        // The SOURCE is narrow to begin with -- broadleaf foliage is a tight
        // family of greens -- so interpolating along it faithfully, which is
        // what the loop above does, cannot produce contrast that is not in the
        // art. This puts it there.
        //
        // A POWER ABOUT A PIVOT, NOT A STRETCH ABOUT THE MEAN, and the
        // difference is the whole reason this works. The shades in use are the
        // BOTTOM of the ramp, so expanding about the ramp's own mean drags all
        // of them down and the wood just gets darker. Pivoting on the middle
        // shade the wood actually SHOWS holds the visible brightness still and
        // spends the expansion entirely on spread: measured, 0.1944..0.2982
        // becomes 0.1285..0.3738 -- 2.91x instead of 1.53x -- while the mean
        // of the three moves 0.2497 -> 0.2529.
        //
        // ONE SCALAR PER SHADE, so the hue cannot move. That is the identical
        // argument valueScale makes a few lines up, and it is why this can be
        // a contrast control rather than a recolour.
        if (contrast != 1.0f && count > 1) {
            const int pk = (pivotK >= 0 && pivotK < int(count)) ? pivotK : int(count) / 2;
            const float pivot = luminance(look_[base + pk].albedo);
            if (pivot > 1e-5f) {
                for (int k = 0; k < int(count); ++k) {
                    MaterialLook &m = look_[base + k];
                    const float l = luminance(m.albedo);
                    if (l <= 1e-5f) continue;
                    m.albedo = m.albedo * (powf(l / pivot, contrast) * pivot / l);
                    // ...AND NOTHING MAY REFLECT MORE THAN IT RECEIVES.
                    // Expanding the TOP of the ramp is how a power about a low
                    // pivot pays for the spread it puts at the bottom, and at
                    // 2.5 the brightest shade came out at 1.07 in green --
                    // an albedo over one, which is a surface that gains energy
                    // on every bounce. Measured, and invisible in the oak
                    // because nothing there wears that shade; it would have
                    // shown up in the straw ramp, which blends these greens.
                    //
                    // Scaled by the WHOLE colour rather than clamped per
                    // channel, or the cap would desaturate the shade it
                    // catches -- green is the largest channel here, so a
                    // per-channel clamp would pull green down toward red and
                    // blue and turn the top of the ramp grey.
                    const float mx = maxComp(m.albedo);
                    if (mx > kAlbedoCeil) m.albedo = m.albedo * (kAlbedoCeil / mx);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // A TALL BLADE'S RAMP: THIS WOOD'S GREEN AT THE FOOT, DRYING TO STRAW.
    //
    // Run once per wood, right after that wood's grass ramp, and it READS that
    // ramp -- which is the whole point. "make the base of the tall wheat grass,
    // the same green as the grass green": shade 0 is a copy of `grassBase`, so
    // the foot of a tuft is the same colour as the short blades round it
    // whatever the pines or the birches turned out to be. Authoring a green
    // here instead would be a third opinion about what this wood's grass is,
    // and it would be wrong the first time an artist re-exported a tree.
    //
    // THE STRAW END IS AUTHORED AND SHARED. Green belongs to the wood; straw
    // does not -- grass that has gone over is the same colour whatever is
    // growing near it -- so the two woods blend toward one table and only their
    // wet ends differ. That is also why this is one function called twice.
    //
    // THE CROSSFADE IS OVER SHADES 1..5, so at STRAND_ROW_STEP = 2 a tall blade
    // is green for its bottom ~4 voxels, turning over the next ~8, and straw for
    // the top half. Short grass never sees any of it: a short blade tops out at
    // row 3 and this family is only ever written on a tall one -- see
    // bladeMaterial.
    //
    // IT RISES IN VALUE THE WHOLE WAY, which is both halves of the brief at
    // once ("a lighter color overal"). Green is a dark albedo and straw is a
    // bright one, so drying out IS getting lighter, and that now agrees with
    // the light field the green ramps encode rather than fighting it. The last
    // two shades turn warm for the seed head -- a change of HUE, not a fall in
    // value.
    // -----------------------------------------------------------------------
    void fillWheatRamp(uint8_t base, uint8_t count, uint8_t grassBase, uint8_t grassCount) {
        // WHAT A DRY BLADE IS, in sRGB because these are picked by eye and sRGB
        // is what a colour picker shows. Only shades 2 and up are really used --
        // 0 and 1 are almost entirely the green -- but the table is full length
        // so the blend has something to aim at from the first step.
        static const uint8_t kStraw[10][3] = {
            {150, 150,  96},   // 0  (barely reached -- the green wins here)
            {166, 162, 100},   // 1
            {186, 176, 108},   // 2  the turn
            {202, 188, 118},   // 3
            {214, 197, 128},   // 4
            {222, 203, 136},   // 5  straw
            {226, 206, 141},   // 6
            {228, 207, 144},   // 7  the palest of it
            // -- THE HEAD, AND IT IS A HUE TURN RATHER THAN A FALL -----------
            //
            // The first cut of this ran the top third down to a dark brown and
            // read as heavy, which is what "a lighter color overal" was about.
            // These two are still the ripe tan the request asks for -- r/g
            // climbs from 1.24 at the peak to 1.36 here, so they are visibly
            // browner -- while giving up only about an eighth of the value.
            // Measured rather than judged: tests/wheat_grass_test.cpp pins the
            // tip at no less than 0.85 of the peak and requires it to be warmer.
            {226, 200, 138},   // 8  the head: warmer, barely darker
            {222, 193, 133},   // 9  ...and a light tan-brown at the very tip
        };
        for (int k = 0; k < int(count); ++k) {
            // HOW DRY THIS SHADE IS. 0 at the soil, 1 from shade 5 up.
            const float w = clampf((float(k) - 1.0f) / 4.0f, 0.0f, 1.0f);
            const MaterialLook &g = look_[grassBase + mini(k, int(grassCount) - 1)];
            const int s = mini(k, 9);
            const Vec3 dry(srgbToLinearF(float(kStraw[s][0]) / 255.0f),
                           srgbToLinearF(float(kStraw[s][1]) / 255.0f),
                           srgbToLinearF(float(kStraw[s][2]) / 255.0f));
            MaterialLook &m = look_[base + k];
            m.albedo = g.albedo * (1.0f - w) + dry * w;
            // Straw is drier and stiffer than a living blade, so it roughens and
            // loses some of the back-lit glow -- but it is still a thin blade
            // and must not become a black stick at dawn. Both eased on the same
            // w, so a shade cannot be green-coloured and straw-surfaced.
            m.roughness = g.roughness * (1.0f - w) + 0.95f * w;
            m.specular = g.specular;
            m.translucency = g.translucency * (1.0f - w) + 0.16f * w;
        }
    }

    void deriveGroundFromTrees() {
        std::vector<uint8_t> foliage, bfoliage, bark;
        const int end = pineEnd_ > mat::TREE_BASE ? pineEnd_ : next_;
        const int bstart = (birchStart_ > mat::TREE_BASE) ? birchStart_ : end;
        for (int i = mat::TREE_BASE; i < end; ++i) {
            // SPLIT BY WHICH WOOD MINTED THE ENTRY. Colours are deduplicated
            // across models, so a green both species use is minted by whichever
            // loaded first -- the pines -- and stays in their set. Only greens
            // the birches alone carry land in theirs, which is exactly the
            // difference we want to draw.
            if (look_[i].translucency > 0.0f) {
                if (i >= bstart) bfoliage.push_back(uint8_t(i));
                else foliage.push_back(uint8_t(i));
                continue;
            }

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
        fillGrassRamp(mat::GRASS_0, mat::GRASS_COUNT, foliage, kConiferFloorV);
        // THE BIRCHES' OWN, from the entries only they carry. Falls back to
        // the pines' set if no birch model loaded, so a pine-only world still
        // has grass rather than the built-in default green.
        // THE FALLBACK TAKES THE CONIFER VALUE WITH IT. If no birch or oak
        // model loaded there is no broadleaf green to be brighter than, and
        // this ramp is the pines' set -- lifting it then would just be a
        // second, paler pine floor. Measured: --oak alone reports 0 broadleaf
        // greens and takes this branch.
        // ...AND PULLED APART. See the contrast block in fillGrassRamp: the
        // broadleaf woods read as one flat green without it, because the only
        // part of this ramp they ever show is its bottom third.
        //
        // THE PIVOT IS THE MIDDLE OF WHAT IS SHOWN, derived rather than
        // picked -- mat::BGRASS_FLOOR_COUNT is already the number of shades a
        // blade can reach (and the number the floor scatters over), so the
        // middle of it is the one shade that must not move.
        //
        // THE PINE RAMP IS DELIBERATELY LEFT FLAT. Needle duff in deep shade
        // genuinely is close to one colour, the request was about the oak, and
        // the note over kConiferFloorV is a standing warning about brightening
        // that floor.
        fillGrassRamp(mat::BGRASS_0, mat::BGRASS_COUNT,
                      bfoliage.empty() ? foliage : bfoliage,
                      bfoliage.empty() ? kConiferFloorV : kBroadleafFloorV,
                      bfoliage.empty() ? 1.0f : kBroadleafContrast,
                      int(mat::BGRASS_FLOOR_COUNT) / 2);

        // -- AND WHAT THE TALL BLADES DRY TO, one ramp per wood ---------------
        //
        // AFTER the greens, and that order is load-bearing rather than tidy:
        // shade 0 of each of these is a COPY of that wood's GRASS_0, so running
        // it first would root every tuft in whatever the slot held before the
        // trees were read. See fillWheatRamp.
        fillWheatRamp(mat::WHEAT_0, mat::WHEAT_COUNT, mat::GRASS_0, mat::GRASS_COUNT);
        fillWheatRamp(mat::BWHEAT_0, mat::BWHEAT_COUNT, mat::BGRASS_0, mat::BGRASS_COUNT);

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
        // THE WHEAT RAMPS ARE NOT AUTHORED HERE ANY MORE. They start at their
        // wood's own GRASS_0, so they cannot be built until the trees have been
        // read -- see fillWheatRamp, called from deriveGroundFromTrees. What
        // stood here was a hand-typed sRGB table, which was the right shape
        // while straw was one colour for the whole world and the wrong one the
        // moment its foot had to match a green this file does not choose.
        // v1'S TILLED EARTH, CONVERTED. Its sRGB (150, 116, 76) decoded to
        // linear, the way mat::SAND above is -- "a DARKER brown than the first
        // pass (user) -- turned soil, not dust". Rougher than the sand it sits
        // beside, because broken ground scatters in every direction.
        set(mat::TILLED, 0.30f, 0.17f, 0.07f, 0.98f);
        // THE HELD MODEL'S THREE GREENS, CONVERTED. seeds.vox is sRGB
        // (171,255,76), (110,255,84), (83,255,121) and this table is LINEAR --
        // see the note over the sand, which is the same conversion and the same
        // trap. A seed on the ground and a seed in your hand are now one fact.
        set(mat::SEED_0 + 0, 0.407f, 1.000f, 0.072f, 0.92f);
        set(mat::SEED_0 + 1, 0.156f, 1.000f, 0.089f, 0.92f);
        set(mat::SEED_0 + 2, 0.087f, 1.000f, 0.191f, 0.92f);
        set(mat::SILT, 0.22f, 0.20f, 0.16f, 0.95f);
        // -- SNOW ---------------------------------------------------------
        // Near white, and NOT pure white: snow lit by a blue sky carries that
        // blue in its shadowed facets, and 1.0 across all three channels is the
        // one value that cannot show a shadow at all. The three shades are a
        // couple of percent apart, which is invisible as a colour difference
        // and is exactly enough to stop a summit reading as one flat plane.
        //
        // ROUGH -- the last argument is the roughness, and old snow is matte.
        // A specular highlight on a mountainside would read as ice.
        // THREE PERCENT APART, NOT NINE. The first cut of these ran 0.86 to
        // 0.94 -- which is what "a couple of percent" above was meant to mean
        // and is not what it was -- and on a snowfield that is not a subtle
        // variation, it is confetti. The per-column hash puts a different shade
        // on neighbouring columns, and on the one-voxel risers between terraces
        // the darker one faces you square on, so an 8% step reads as blue flecks
        // scattered over the whole mountain. White ground is the least
        // forgiving surface there is for this: there is no texture to hide a
        // step in and nothing darker nearby to judge it against.
        set(mat::SNOW_0 + 0, 0.930f, 0.945f, 0.970f, 0.95f);
        set(mat::SNOW_0 + 1, 0.945f, 0.955f, 0.975f, 0.95f);
        set(mat::SNOW_0 + 2, 0.915f, 0.935f, 0.965f, 0.95f);
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
    // One bit per id: was this colour classified as a leaf when it was minted?
    std::vector<uint8_t> foliage_ = std::vector<uint8_t>(mat::COUNT, 0u);
    // ...and the colour that minted it, before the foliage lift touched it.
    // 768 bytes to make the table authorable -- see authoredColor.
    std::vector<std::array<uint8_t, 3>> src_ =
        std::vector<std::array<uint8_t, 3>>(mat::COUNT, std::array<uint8_t, 3>{{0, 0, 0}});
    std::vector<uint8_t> srcExact_ = std::vector<uint8_t>(mat::COUNT, 0u);
    std::map<uint32_t, uint8_t> index_;
    uint8_t next_ = mat::TREE_BASE;
    // -- THE LEVEL'S OWN TABLE -- see beginLevelTable ----------------------
    //
    // A full copy of look_ with the level's colours written over the entries
    // the level does not need the wood's meaning of. Allocated TOP-DOWN, so
    // levelNext_ walks toward TREE_BASE rather than away from it.
    std::vector<MaterialLook> levelLook_;
    std::vector<uint8_t> levelReserved_;
    std::vector<uint8_t> levelOwn_;
    std::map<uint32_t, uint8_t> levelIndex_;
    int levelNext_ = 255;
    int levelMinted_ = 0;
    int levelOverflow_ = 0;
    bool haveLevel_ = false;
    int pineEnd_ = 0;
    int birchStart_ = 0;
    int stoneFromRocks_ = 0;
    int overflow_ = 0;
    // The refused keys, deduped -- see forModelColor's overflow branch for why
    // the call count on its own is misleading.
    std::unordered_set<uint32_t> overflowKeys_;
};

inline bool isGrass(uint8_t m) { return m >= mat::GRASS_0 && m < mat::GRASS_0 + mat::GRASS_COUNT; }
inline bool isBGrass(uint8_t m) {
    return m >= mat::BGRASS_0 && m < mat::BGRASS_0 + mat::BGRASS_COUNT;
}
// A TALL BLADE, WHICH IS STILL GRASS -- see mat::WHEAT_0. Kept out of isGrass()
// on purpose: that one means "the pines' green ramp" and is asked by code that
// cares which RAMP it is holding, not what the plant is.
// EITHER WOOD'S STRAW. Two ramps, one question -- nothing that asks this cares
// which wood a tall blade dried out in.
inline bool isWheat(uint8_t m) {
    return (m >= mat::WHEAT_0 && m < mat::WHEAT_0 + mat::WHEAT_COUNT) ||
           (m >= mat::BWHEAT_0 && m < mat::BWHEAT_0 + mat::BWHEAT_COUNT);
}
// ...and the question "is this a blade of anything", for the callers that mean
// the plant. All four ramps, both woods, green and gone-over.
inline bool isBlade(uint8_t m) { return isGrass(m) || isBGrass(m) || isWheat(m); }
// A BEACH IS NOT A SEED BED. v1 is explicit that sand is in its dig table
// because the SHOVEL moves it, "which is a different question from whether a
// hoe can make a seed bed out of a beach" -- so the hoe asks this and the
// shovel does not. Both the single id and the four grains the device spreads it
// over, because either can be the surface of a shore.
// A SEED LYING ON TURNED EARTH. Three shades, one question.
inline bool isSeed(uint8_t m) {
    return m >= mat::SEED_0 && m < mat::SEED_0 + mat::SEED_COUNT;
}

inline bool isSand(uint8_t m) {
    return m == mat::SAND || (m >= mat::SAND_0 && m < mat::SAND_0 + mat::SAND_COUNT);
}
inline bool isSoil(uint8_t m) { return m >= mat::SOIL_0 && m < mat::SOIL_0 + mat::SOIL_COUNT; }
inline bool isSnow(uint8_t m) { return m >= mat::SNOW_0 && m < mat::SNOW_0 + mat::SNOW_COUNT; }
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
// at most sixteen voxels tall, so `(v.y - base) & 15` is its row above the soil
// -- and the +1 is what distinguishes a blade from the ground it stands in,
// since a base row of zero is a real base row.
//
// The dir field is three bits wide now rather than a whole byte, so anything
// reading it MUST mask. See triFace here and faceNormal in Trace.cs.slang.
// ---------------------------------------------------------------------------
constexpr int TRI_DIR_SHIFT = 8, TRI_DIR_MASK = 0x7;
constexpr int TRI_STRAND_SHIFT = 11, TRI_STRAND_MASK = 0x1F;
// -----------------------------------------------------------------------
// THE RAMP STEPS TWO VOXELS, AND THAT IS WHAT BUYS 15-20 TALL GRASS.
//
// The field stores `1 + (base row & mask)` and the device recovers
// `row = v.y - base`, so the MASK is the tallest blade that can be told apart.
// Four bits of row is sixteen voxels, and the packed triangle word has no
// seventeenth bit left -- bits 0..7 are the material, 8..10 the direction,
// 11..15 this. The word is full.
//
// AND THE ZERO CANNOT BE RECLAIMED. It is the sentinel for "not a blade", and
// it is load-bearing for something that is not obvious: a FLOWER'S STEM is
// mapped onto the grass ramp (see loadModelSet's stemId), so a model voxel can
// wear kGrass0 with no strand code at all. Drop the sentinel and every flower
// stem in the world is shaded as a blade at some arbitrary row.
//
// So the RESOLUTION is spent instead of the range: the base row is stored
// HALVED, and the device halves v.y the same way, which doubles what four bits
// reach -- 32 voxels, or 3.2 m of grass.
//
// WHAT IT COSTS, stated plainly:
//
//   * The gradient steps every TWO voxels rather than every one. There are six
//     shades in the ramp and `min(row, count-1)` clamps, so the gradient was
//     spent by row 5 and is now spent by row 11. On a 20-voxel blade that is
//     better, not worse -- the old encoding ran out of gradient in the bottom
//     quarter of the strand.
//   * A blade's first shade depends on the PARITY of its base row, because
//     floor(y/2) - floor(base/2) is off by one when the two disagree. It is
//     constant along any one blade, so a strand simply starts on shade 0 or
//     shade 1. In a sward of six close greens that reads as sward.
//
// BOTH SIDES MUST FLOOR THE SAME WAY. `>>` on a signed int is an arithmetic
// shift in C++ and in Slang, which IS floor division by two, so they agree --
// including below y = 0, which no blade reaches anyway (the lowest ground in
// the world is 1.8 m, and grass grows above it).
// -----------------------------------------------------------------------
// FIVE BITS OF STRAND, NOT FOUR -- and this spends the word's last spare bit.
//
// The field is `1 + (base row & mask)`, so the mask is what the device can
// tell apart and the +1 is what distinguishes a blade from the ground it
// stands in (a base row of zero is a real base row). Four bits meant three
// bits of row, which is eight voxels, and the note that used to be here said
// so as a format limit.
//
// TALL GRASS ASKED FOR 10-13 (user 2026-09-13), and there was no way to give
// it inside eight: a blade thirteen tall would have its ninth row recovered as
// row 1, so the top of every tall strand would wear the DARKEST green in the
// ramp -- the gradient would run bright, then snap black, twice.
//
// bit 15 was the only thing left in the word and this is what it is for.
// Sixteen rows is 1.6 m of grass, which is past anything that has been asked
// for; there is nothing after it, so the next height request is a wider word.
// -----------------------------------------------------------------------
// 32 voxels: sixteen values of the stored half-row, two voxels each.
constexpr int STRAND_MAX_ROWS = 32;
// How many voxels one step of the stored row covers. See the note above.
constexpr int STRAND_ROW_STEP = 2;

// -- HOW TALL SHORT GRASS STANDS, AND WHAT THAT MAKES THE FLOOR ------------
//
// AT NAMESPACE SCOPE, AND THAT IS NOT A STYLE CHOICE. These were members of
// VoxelTerrain and a constexpr member function cannot be CALLED to initialise
// a constexpr member of the same class -- the class is not complete yet, and
// the error says so in exactly those words. They belong out here anyway: they
// are properties of the grass, and both the terrain and the shader's contract
// read them.
//
// The ceiling is a constant rather than a literal in the member because
// mat::BGRASS_FLOOR_COUNT is DERIVED from it -- the floor under the oaks
// scatters over exactly the shades the sward standing on it can reach, so the
// two cannot be changed independently without the ground and the grass
// drifting apart again. The static_asserts are the whole of that contract.
//
// The BIRCH ceiling is the shorter of the two (birchGrassMaxRows 5), so the
// pine's is the reach for both: a shorter blade cannot reach a shade a taller
// one misses.
constexpr int kGrassMinRows = 3;
constexpr int kGrassMaxRows = 6;
// One shade per STRAND_ROW_STEP, counting the shade AT the soil -- see
// groundShade()'s gradient, which is floor(row / step) clamped.
constexpr int kBladeRampReach = (kGrassMaxRows - 1) / STRAND_ROW_STEP + 1;
static_assert(int(mat::BGRASS_FLOOR_COUNT) == kBladeRampReach,
              "the oak floor scatters over shades no blade reaches, or misses some it does");
static_assert(int(mat::BGRASS_FLOOR_COUNT) <= int(mat::BGRASS_COUNT),
              "the floor cannot use more of the ramp than the ramp has");

// -- ...AND IN WHAT PROPORTION A BLADE ACTUALLY WEARS THEM -----------------
//
// (user 2026-09-17: "again, I want you to take the terrain grass, and make it
//  match the same colors as the grass strand grass".)
//
// THE FLOOR AND THE BLADES ALREADY USED THE SAME THREE GREENS AND STILL DID
// NOT MATCH, because they mix them in different proportions -- and a different
// mix of the same paints is a different colour. The floor scattered them
// uniformly, 33/33/33; a blade lays them down its own height, two voxels per
// shade from the soil up, so a 3-voxel blade never reaches shade 2 at all and
// the aggregate is weighted to the dark end. Measured, the floor was 15.5%
// brighter than the grass standing in it -- worse than the 5.9% it started at,
// because widening the ramp for contrast also widened the gap this opens.
//
// SO THE WEIGHTS ARE COUNTED, NOT CHOSEN. Every blade height the oak grows,
// every voxel row in it, binned by the shade groundShade would give it:
//
//     rows 3   0 0 1          rows 5   0 0 1 1 2
//     rows 4   0 0 1 1        rows 6   0 0 1 1 2 2
//
// which is 8 / 7 / 3 out of 18 -- 44.4%, 38.9%, 16.7%, the measured
// distribution to a tenth of a per cent. Deal the floor those odds and the two
// average to the same green by construction.
//
// DERIVED AT COMPILE TIME so raising the grass moves the floor with it. The
// shader mirrors the three numbers and is asserted against them in world.h.
constexpr int bladeShadeWeight(int shade) {
    int n = 0;
    for (int r = kGrassMinRows; r <= kGrassMaxRows; ++r)
        for (int v = 0; v < r; ++v) {
            const int k = v / STRAND_ROW_STEP;
            const int c = int(mat::BGRASS_FLOOR_COUNT) - 1;
            if ((k > c ? c : k) == shade) ++n;
        }
    return n;
}
constexpr int kBladeW0 = bladeShadeWeight(0);
constexpr int kBladeW1 = bladeShadeWeight(1);
constexpr int kBladeW2 = bladeShadeWeight(2);
constexpr int kBladeWTotal = kBladeW0 + kBladeW1 + kBladeW2;
static_assert(mat::BGRASS_FLOOR_COUNT == 3,
              "the floor's weight table is written for three shades");
static_assert(kBladeWTotal > 0, "a blade with no voxels has no colour");


// The value the mesher stores for a blade standing on surface voxel `hc` --
// its base row is the one above. Never 0, which is what marks a face as a
// blade at all.
inline uint8_t strandCodeFor(int baseRow) {
    return uint8_t(1 + ((baseRow >> 1) & (STRAND_MAX_ROWS / STRAND_ROW_STEP - 1)));
}

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
// WHERE FRUIT HANGS IN AN OAK -- the same anchors, RINGED instead of scattered.
//
// (user 2026-09-17: "add apples and oranges to some of the trees in the oak
//  forest. import the v1 mechanics of this.")
//
// collectPerches already answers the hard question -- a cell with wood over it
// and clear air below, so nothing can hang inside the dome or float near a
// branch instead of on one -- and a fruit wants exactly that cell. What it does
// NOT answer is WHICH of them, and for a crop that matters in a way it does not
// for cones.
//
// A PINE DROPS FOURTEEN CONES AND AN OAK CARRIES SIX TO EIGHTEEN APPLES. The
// cones are drawn uniformly and there are enough of them that clumping averages
// out; six fruit drawn the same way land in whatever corner of the crown the
// hash happens to favour, and a tree with all its apples on one side reads as a
// mistake rather than as a tree. The browser engine hit this first and its
// answer is the one worth copying: sort the anchors by ANGLE about the crown's
// centre, and take the j-th fruit out of the j-th angular sector. The crop then
// rings the crown by construction, at any count, with no rejection loop.
//
// EVENLY SAMPLED TO A CAP, which is the second half of the same trick. These
// crowns offer hundreds of anchors and a list that long costs memory per model
// for no placement freedom -- 96 sectors is more than five times the most fruit
// any tree carries. Sampling AFTER the sort spreads the survivors around the
// crown; sampling before it would keep one arc and throw the rest away.
//
// ITS OWN LIST RATHER THAN A SORT IN PLACE. `perches` is shared with the
// pinecones, which draw uniformly and do not care about order -- but they do
// care about WHICH index a hash lands on, so permuting the list they read would
// move every cone in the pine wood for a change that has nothing to do with
// pines. Cheaper to be wrong about memory than about that.
// ---------------------------------------------------------------------------
inline std::vector<Perch> fruitAnchors(const std::vector<Perch> &src, int sx, int sz,
                                       int cap = 96) {
    std::vector<Perch> a = src;
    const float cx = float(sx) * 0.5f, cz = float(sz) * 0.5f;
    auto ang = [&](const Perch &p) { return atan2f(float(p.z) - cz, float(p.x) - cx); };
    std::sort(a.begin(), a.end(),
              [&](const Perch &p, const Perch &q) { return ang(p) < ang(q); });
    if (int(a.size()) <= cap) return a;
    std::vector<Perch> out;
    out.reserve(size_t(cap));
    // The half-step keeps the samples off both ends of the sorted run, so the
    // first and last survivors are a sector apart like every other pair rather
    // than adjacent across the wrap.
    for (int i = 0; i < cap; ++i)
        out.push_back(a[size_t((float(i) + 0.5f) / float(cap) * float(a.size()))]);
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
// ONE BLOCK OF A BIG ASSET, MESHED AGAINST THE WHOLE OF IT.
//
// meshAsset's sibling, and the difference is the only interesting thing about
// it: the LOOP runs over the block, and the NEIGHBOUR TEST reads the full grid.
// Mesh a block as an asset in its own right and every one of its six faces
// comes out walled, because the voxels on the other side of the cut are simply
// not in the asset -- 128 blocks of a map would gain the surface area of 128
// boxes, all of it buried inside solid rock and all of it traced.
//
// WHY BLOCKS AT ALL: a bullet takes a chip out of the level, and the level is
// 136 M cells that take 383 ms to mesh in one piece. Nothing can carve that.
// Cut into 64-voxel columns it is 3-4 ms for the block the round hit, which a
// weapon can afford. See World::carveLevel.
//
// Vertices are RELATIVE to the block's own corner, so the instance transform
// places it -- exactly as every other asset in this engine is placed.
// ---------------------------------------------------------------------------
inline VoxMesh meshAssetBlock(const VoxAsset &a, const std::vector<uint8_t> &idOfEntry, float scale,
                              int bx0, int by0, int bz0, int bsx, int bsy, int bsz) {
    VoxMesh m;
    const float s = scale;
    auto solid = [&](int x, int y, int z) -> bool {
        const uint8_t v = a.at(x, y, z);      // at() answers 0 outside the grid
        return v != 0 && idOfEntry[v] != mat::AIR;
    };
    const int x1e = mini(bx0 + bsx, a.sx), y1e = mini(by0 + bsy, a.sy), z1e = mini(bz0 + bsz, a.sz);
    for (int y = by0; y < y1e; ++y)
        for (int z = bz0; z < z1e; ++z)
            for (int x = bx0; x < x1e; ++x) {
                const uint8_t v = a.at(x, y, z);
                if (!v) continue;
                const uint8_t id = idOfEntry[v];
                if (id == mat::AIR) continue;
                const float x0 = float(x - bx0) * s, x1 = x0 + s;
                const float y0 = float(y - by0) * s, y1 = y0 + s;
                const float z0 = float(z - bz0) * s, z1 = z0 + s;
                if (!solid(x, y + 1, z))
                    m.addQuad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, id,
                              face::POS_Y);
                if (!solid(x, y - 1, z))
                    m.addQuad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, id,
                              face::NEG_Y);
                if (!solid(x + 1, y, z))
                    m.addQuad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, id,
                              face::POS_X);
                if (!solid(x - 1, y, z))
                    m.addQuad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, id,
                              face::NEG_X);
                if (!solid(x, y, z + 1))
                    m.addQuad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, id,
                              face::POS_Z);
                if (!solid(x, y, z - 1))
                    m.addQuad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, id,
                              face::NEG_Z);
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
    FbmMemo detail;  // the sub-metre roughness laid over measured ground
    // THE BIRCH'S ROLL AND SWELL ARE ITS OWN NOW, and only a column inside
    // the seam ever asks for them alongside the pine's -- see heightM. They
    // replace the ridge memo rather than adding to the struct, the ridged
    // octave having gone with it.
    FbmMemo birchRoll, birchSwell;
    // ...and the oak's, on the same terms: only a column inside a seam ever
    // asks for a second wood's field alongside its own.
    FbmMemo oakA, oakB;
    FbmMemo stand, litter, grassMask;                       // topMaterial
    // The tall-grass field had an FbmMemo here. It is a jittered site lattice
    // now (see tuftAt), which is hashes alone -- no noise, nothing to cache.
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
// -- WHICH WOODS A SPECIES LIVES IN, as a set. See VoxelTerrain::woodBit.
//
// One bit per band so a table can say "both broadleaf bands" (kWoodBroad),
// which is what the mouse, the grass snake and the frog all want and which no
// single index can express.
inline constexpr uint8_t kWoodPine = 1u;
inline constexpr uint8_t kWoodBirch = 2u;
inline constexpr uint8_t kWoodOak = 4u;
inline constexpr uint8_t kWoodBroad = kWoodBirch | kWoodOak;
inline constexpr uint8_t kWoodAll = kWoodPine | kWoodBirch | kWoodOak;

enum class Biome : uint8_t {
    Pine,   // the original: high relief, ridges, basins, a needle floor
    Birch,  // low rounded hills, one light green everywhere, beehives
    // -- ...AND THE OAK, IMPORTED FROM v1 (user 2026-09-16) -----------------
    //
    // A THIRD KIND OF PLACE, not a third set of trees in one of the first two.
    // v1 gives its oak forest its own mask (oakM), its own height field (oakH)
    // and its own floor, and the shape of the wood is what makes it read as
    // somewhere else: a pine is a spire and a birch is a column, and an oak is
    // WIDER THAN IT IS TALL. Measured off the seven models --
    //
    //     oak_1   2.1 m tall,  3.4 m across   (a bush)
    //     oak_4  10.7 m tall,  8.9 m across
    //     oak_7  17.1 m tall, 17.0 m across
    //
    // -- against birches that are 18 to 30 m of near-vertical trunk. A stand of
    // oaks closes overhead rather than striping the view.
    Oak,
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

    // -----------------------------------------------------------------------
    // THE BOUNDING BOX OF THE EDITED COLUMNS, IN WORLD VOXELS.
    //
    // (user 2026-09-17: "when tilling the land, the terrain glitches out".)
    //
    // THE GROUND PATCH ASKED THE WRONG QUESTION AND IT COST 25 ms A REBUILD.
    // World::groundPatch skips its fifteen-neighbour sweep where nothing has
    // been dug, and it decided that per CHUNK -- but a chunk is hundreds of
    // cells and a hoe bite is one, so the first till turned the sweep back on
    // for the whole chunk and every cell in it paid the lookups again. MEASURED
    // over the same walk, with and without the hoe in hand:
    //
    //     sampling   2-4 ms, 4 rebuilds      ->   20-31 ms, 35 rebuilds
    //
    // -- 25 ms landing on the frame of every swing, and that is what "the
    // terrain glitches out" is: a stall, not a hole.
    //
    // A BOX IS THE RIGHT GRANULARITY. Four ints, exact for the one shape an
    // edit ever has (a disc or a sphere), and it answers "is this 4x4 cell
    // anywhere near an edit" in four comparisons instead of sixteen hash
    // lookups. It is never SHRUNK -- a revert leaves it as it was, which costs
    // a sweep over ground that no longer needs one and can never miss one that
    // does.
    int ei0 = INT_MAX, ej0 = INT_MAX, ei1 = INT_MIN, ej1 = INT_MIN;

    void noteColumn(int i, int j) {
        if (i < ei0) ei0 = i;
        if (j < ej0) ej0 = j;
        if (i > ei1) ei1 = i;
        if (j > ej1) ej1 = j;
    }
    // Does [a0,a1] x [b0,b1] (world voxels) touch anything edited here?
    bool touchesRect(int a0, int b0, int a1, int b1) const {
        return ei0 <= ei1 && a1 >= ei0 && a0 <= ei1 && b1 >= ej0 && b0 <= ej1;
    }

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
    // -----------------------------------------------------------------------
    // FORGET EVERY EDIT THE PLAYER HAS MADE.
    //
    // (user 2026-09-17: "let me press q to refresh the game" -- G, in the end.)
    //
    // Every pit, every tilled bed, every planted seed. The generator is a pure
    // function of (x, z), so the wood that comes back is the one it always
    // described -- this map IS the difference between the world as generated
    // and the world as played in, and dropping it is the whole of a reload.
    //
    // THE CHUNKS HAVE TO GO WITH IT. A meshed chunk already has these edits
    // baked into its geometry, so clearing this alone would leave every hole on
    // screen until something happened to re-mesh it. See World::reloadWorld,
    // which is the only caller and does both.
    void clear() {
        std::lock_guard<std::mutex> lk(mx_);
        byChunk_.clear();
    }

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
                            slot->noteColumn(ni, nj);
                    auto &span = slot->col[ChunkEdits::ckey(ni, nj)];
                            if (span.first == 0 && span.second == 0) span = {y, y + 1};
                            else {
                                span.first = mini(span.first, y);
                                span.second = maxi(span.second, y + 1);
                            }
                        }
                }
        return publish(touched);
    }

    // -----------------------------------------------------------------------
    // ...AND THE SAME EDIT OVER A LIST SOMEBODY ELSE CHOSE.
    //
    // carve() above decides its own shape -- a sphere -- which is right for a
    // tool head and wrong for anything that has to pick its voxels by what they
    // are MADE OF. EditStore has no terrain to ask, so the choosing cannot
    // happen in here; World::mow does it and hands the answer over.
    //
    // Everything else is carve()'s, including the part that matters: each cell
    // is published to every chunk whose column ring reaches it, so a blade cut
    // at a chunk seam is cut in both of them.
    // -----------------------------------------------------------------------
    std::vector<std::pair<int, int>> carveCells(const std::vector<std::array<int, 3>> &cells) {
        std::map<std::pair<int, int>, std::shared_ptr<ChunkEdits>> touched;
        std::lock_guard<std::mutex> lk(mx_);
        for (const std::array<int, 3> &c : cells) {
            const int i = c[0], j = c[1], y = c[2];
            for (int rj = -1; rj <= 1; ++rj)
                for (int ri = -1; ri <= 1; ++ri) {
                    const int ni = i + ri, nj = j + rj;
                    const int cx = floorDiv(ni, CHUNK_VOX), cz = floorDiv(nj, CHUNK_VOX);
                    auto &slot = touched[{cx, cz}];
                    if (!slot) {
                        const auto it = byChunk_.find(ChunkEdits::ckey(cx, cz));
                        slot = it == byChunk_.end() ? std::make_shared<ChunkEdits>()
                                                    : std::make_shared<ChunkEdits>(*it->second);
                    }
                    slot->vox[ChunkEdits::vkey(i, j, y)] = mat::AIR;
                    slot->noteColumn(ni, nj);
                    auto &span = slot->col[ChunkEdits::ckey(ni, nj)];
                    if (span.first == 0 && span.second == 0) span = {y, y + 1};
                    else {
                        span.first = mini(span.first, y);
                        span.second = maxi(span.second, y + 1);
                    }
                }
        }
        return publish(touched);
    }

    // -----------------------------------------------------------------------
    // ...AND THE SAME EDIT WITH A MATERIAL IN IT RATHER THAN AIR.
    //
    // THE FORMAT ALWAYS ALLOWED THIS and nothing had ever used it: ChunkEdits
    // maps a voxel to a `uint8_t`, and voxel/columns.h's applyEdits already
    // reads it both ways -- "if (m == mat::AIR) clear the bit; else set it" --
    // so an edit that PUTS something there meshes correctly with no change to
    // the mesher at all. Every writer so far has been a carve, so every edit so
    // far has been AIR.
    //
    // The hoe is the first thing that adds rather than removes. See World::till.
    // -----------------------------------------------------------------------
    std::vector<std::pair<int, int>> writeCells(
        const std::vector<std::pair<std::array<int, 3>, uint8_t>> &cells) {
        std::map<std::pair<int, int>, std::shared_ptr<ChunkEdits>> touched;
        std::lock_guard<std::mutex> lk(mx_);
        for (const auto &cm : cells) {
            const int i = cm.first[0], j = cm.first[1], y = cm.first[2];
            for (int rj = -1; rj <= 1; ++rj)
                for (int ri = -1; ri <= 1; ++ri) {
                    const int ni = i + ri, nj = j + rj;
                    const int cx = floorDiv(ni, CHUNK_VOX), cz = floorDiv(nj, CHUNK_VOX);
                    auto &slot = touched[{cx, cz}];
                    if (!slot) {
                        const auto it = byChunk_.find(ChunkEdits::ckey(cx, cz));
                        slot = it == byChunk_.end() ? std::make_shared<ChunkEdits>()
                                                    : std::make_shared<ChunkEdits>(*it->second);
                    }
                    slot->vox[ChunkEdits::vkey(i, j, y)] = cm.second;
                    slot->noteColumn(ni, nj);
                    auto &span = slot->col[ChunkEdits::ckey(ni, nj)];
                    if (span.first == 0 && span.second == 0) span = {y, y + 1};
                    else {
                        span.first = mini(span.first, y);
                        span.second = maxi(span.second, y + 1);
                    }
                }
        }
        return publish(touched);
    }

    // -----------------------------------------------------------------------
    // ...AND TAKING AN EDIT BACK, which is what "it grows back over" needs.
    //
    // THE COLUMN SPAN HAS TO GO WITH THE VOXELS OR THE GRASS NEVER RETURNS.
    // `col` is not bookkeeping -- StrandColumns reads it as "this column has
    // been touched, grow nothing" and meshChunk reads it as "voxel-mesh this
    // one". Erasing the voxels and leaving the span behind would put the dirt
    // back and leave the tuft that was standing on it gone for ever, which is
    // the revert half-working in the way that looks like it worked.
    //
    // REBUILT FROM WHAT IS LEFT rather than narrowed by arithmetic: a chunk's
    // edit set is small, the spans overlap in ways that do not subtract, and a
    // span that is merely nearly right is a seam that meshes one way on one
    // side. See tillRevert.
    // -----------------------------------------------------------------------
    std::vector<std::pair<int, int>> eraseCells(const std::vector<std::array<int, 3>> &cells) {
        std::map<std::pair<int, int>, std::shared_ptr<ChunkEdits>> touched;
        std::lock_guard<std::mutex> lk(mx_);
        for (const std::array<int, 3> &c : cells) {
            const int i = c[0], j = c[1], y = c[2];
            for (int rj = -1; rj <= 1; ++rj)
                for (int ri = -1; ri <= 1; ++ri) {
                    const int cx = floorDiv(i + ri, CHUNK_VOX), cz = floorDiv(j + rj, CHUNK_VOX);
                    auto &slot = touched[{cx, cz}];
                    if (!slot) {
                        const auto it = byChunk_.find(ChunkEdits::ckey(cx, cz));
                        if (it == byChunk_.end()) continue;
                        slot = std::make_shared<ChunkEdits>(*it->second);
                    }
                    slot->vox.erase(ChunkEdits::vkey(i, j, y));
                }
        }
        for (auto &kv : touched) {
            if (!kv.second) continue;
            ChunkEdits &ce = *kv.second;
            ce.col.clear();
            for (const auto &v : ce.vox) {
                const uint64_t k = v.first;
                const int i = int((k >> 42) & 0x1fffffu), j = int((k >> 21) & 0x1fffffu);
                const int y = int(k & 0x1fffffu);
                const int si = (i & 0x100000) ? i - 0x200000 : i;
                const int sj = (j & 0x100000) ? j - 0x200000 : j;
                const int sy = (y & 0x100000) ? y - 0x200000 : y;
                for (int rj = -1; rj <= 1; ++rj)
                    for (int ri = -1; ri <= 1; ++ri) {
                        ce.noteColumn(si + ri, sj + rj);
                        auto &span = ce.col[ChunkEdits::ckey(si + ri, sj + rj)];
                        if (span.first == 0 && span.second == 0) span = {sy, sy + 1};
                        else {
                            span.first = mini(span.first, sy);
                            span.second = maxi(span.second, sy + 1);
                        }
                    }
            }
        }
        return publish(touched);
    }

    // Floor division: chunk -1 must hold voxel -1, not voxel 0.
    static int floorDiv(int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }

  private:
    // The tail every writer above shares: republish the copies and say which
    // chunks moved. Called with the lock already held.
    std::vector<std::pair<int, int>> publish(
        std::map<std::pair<int, int>, std::shared_ptr<ChunkEdits>> &touched) {
        std::vector<std::pair<int, int>> out;
        out.reserve(touched.size());
        for (auto &kv : touched) {
            if (!kv.second) continue;
            byChunk_[ChunkEdits::ckey(kv.first.first, kv.first.second)] = kv.second;
            out.push_back(kv.first);
        }
        return out;
    }

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
    // TILLED EARTH IS STILL SOIL. A shovel that could not move ground a hoe had
    // just turned over would be the one patch of dirt in the world you cannot
    // dig, which is a trap rather than a rule.
    // -- AND THE BROADLEAF GREEN, WHICH IS THE OAK'S WHOLE FLOOR ------------
    //
    // (user 2026-09-16: "when planting a seed or tilling the land with a hoe,
    // the terrain glitches out".)
    //
    // isGrass WAS HERE AND isBGrass WAS NOT, and that gap became the oak wood's
    // entire surface the day its floor was made green. Everything that asks
    // "can a tool move this ground" runs through here -- toolTakes(Takes::Soil),
    // the hoe's till, the shovel's dig -- so in the oak the hoe found no soil
    // under the crosshair at all. --hoe-test said it plainly once it was run
    // there: "the swing found no ground to aim at".
    //
    // isGrass itself is vestigial -- the pine and birch floors have been soil
    // and litter since the green paint was removed -- but it is the exact slot
    // this needed, and leaving it while adding the other ramp keeps the rule
    // honest: a green FLOOR is diggable ground whichever wood painted it.
    return m == mat::DIRT || m == mat::SAND || m == mat::SILT || m == mat::TILLED ||
           isSeed(m) || isGrass(m) || isBGrass(m) || isSoil(m) || isLitter(m);
}

class VoxelTerrain {
  public:
    // -------------------------------------------------- THE GROUND'S SOURCE
    // Empty by default, and while it is empty this class is exactly what it
    // always was -- the invented landform below runs untouched. Load a .vbdem
    // and heightM stops asking it and starts reading measured ground instead.
    // One branch, in one place, because heightM is already the single point
    // every other system goes through for "how high is it here".
    // -----------------------------------------------------------------------
    bool loadDem(const std::string &path, float baseM = 20.0f,
                 float shrink = 1.0f, float exag = 1.0f) {
        if (!dem_.load(path, baseM, shrink, exag)) return false;
        // THE TIMBERLINE IS AN ALTITUDE, so it only exists once there is a real
        // altitude to compare against. The datum has already been subtracted
        // from the ground, so it has to come off the treeline too or the line
        // sits 2.5 km above a world that is only 1.8 km tall and nothing is
        // ever above it.
        // THROUGH THE SAME CONVERSION AS THE GROUND. Once the dataset can be
        // shrunk, "3505 m above sea level" is no longer "3505 minus a datum" --
        // it is that altitude put through exactly the mapping heightM uses, or
        // the treeline drifts off the mountain the moment the scale changes.
        timberlineWorldM = dem_.aslToWorld(kTimberlineAslM);
        // The fade is a vertical distance too, so it compresses with the rest.
        timberlineFadeM = kTimberlineFadeAslM * (dem_.exag() / dem_.shrink());
        return true;
    }

    // ~11,500 ft, which is where the spruce and fir give out in the Sawatch.
    // Above it Colorado is rock and tundra, and a wood that ignores it puts
    // pines on the summit of a 14er -- the one thing that would say loudest
    // that this terrain is not really Colorado.
    static constexpr float kTimberlineAslM = 3505.0f;
    float timberlineWorldM = -1.0f;   // < 0 = no DEM, no timberline, old behaviour
    static constexpr float kTimberlineFadeAslM = 180.0f;  // in REAL metres
    float timberlineFadeM = kTimberlineFadeAslM;  // in world metres, set on load
    // Peak-to-peak roughness added over measured ground, in world metres.
    float demDetailM = 0.0f;   // 0 = the measurement and nothing added; see heightM
    // How deep a mapped lake gets at its middle, world metres. 5 world m is
    // 30 real m at shrink 6, which is about Cheesman.
    float kLakeDepthM = 5.0f;
    // -- AND HOW A SEA CARRIES ON PAST THAT ---------------------------------
    //
    // kLakeDepthM is where the shore ramp levels off; beyond it the bed falls
    // away at this much per world metre of extra distance from shore, down to
    // kSeaDepthM. See the carve in heightM for why a hard stop there was the
    // whole of the "missing terrain on the ocean floor" report.
    //
    // THE GRADE IS TINY AND THAT IS THE POINT: 1.5 cm per metre is a 0.9
    // degree slope, far too gentle to read as a hillside underwater, but over
    // the few hundred metres of open water in a window it is the difference
    // between a bed with form and one flat plane.
    //
    // kSeaDepthM IS A VISIBILITY BUDGET, not a guess at real bathymetry -- the
    // DEM has none, because 3DEP maps still water as a level plane and the bed
    // here is invented either way. Trace.cs.slang's waterSigma leaves 10% of
    // blue at 5 m and 4% at 8 m over the round trip down and back, so past
    // about eight metres a bed is academic: it is there, and nothing can see
    // it. Deepening this further makes the sea darker, not more interesting.
    float kSeaGradeW = 0.015f;
    float kSeaDepthM = 8.0f;

    // -----------------------------------------------------------------------
    // THE CONTOUR IS WARPED, WHICH IS NOT THE SAME AS DITHERING THE HEIGHT.
    //
    // (user 2026-09-19: "there are also straight lines forming in the terrain",
    // and the same thing on the lake bed in every underwater shot.)
    //
    // WHAT A TERRACE IS: quantising ANY smooth ramp onto 0.1 m voxels steps the
    // column height a whole voxel at a time, so a constant grade gives treads of
    // constant width and every tread edge lies exactly along a contour. Nothing
    // in the data or the interpolation causes it; see dem.h for the smoothstep
    // that was built, shipped and made no difference.
    //
    // WHAT WAS HERE was half a voxel of WHITE noise per column, and its
    // geometry is worth stating because it is why it was not enough. A dither
    // of amplitude A frays the tread edge over a band A/grade wide, and the
    // tread itself is VOXEL_M/grade wide -- so the fray is the same FRACTION of
    // a tread at every grade, and the dither is no weaker on gentle ground. It
    // is only more OBVIOUS there, because that fraction of a 25 m tread is nine
    // metres of randomly flipped columns, which reads as exactly the speckle
    // "remove that noise from all terrain" was about. That is what the old
    // grade gate was really protecting against, and it bought the protection by
    // switching the fix off over the widest treads in the world -- the ones
    // that show most. Measured over Ouachita: treads wider than 12.5 m got a
    // mean weight of 0.06, i.e. nothing.
    //
    // SO THE NOISE IS SMOOTH INSTEAD OF WHITE, and that single change removes
    // the trade. A value-noise field a few metres across, half a voxel peak to
    // peak, displaces the CONTOUR sideways by amplitude/grade -- eleven metres
    // on a 25 m tread, two and a half on the 5.5 m treads of the snowfield the
    // rings were photographed on -- so the line stops being a line. And because
    // the field is smooth, two neighbouring columns differ by the noise's own
    // gradient (under 2 mm at this wavelength) rather than by a whole voxel, so
    // it CANNOT speckle, on any grade, including none.
    //
    // It still cannot move a column further than the rounding already does, so
    // the standing "no noise on terrain" rule holds for the same reason it did
    // before: every column still lands within half a voxel of the measurement,
    // which is strictly closer than a terrace, and half a voxel is two orders
    // of magnitude under 3DEP's own vertical error.
    // -----------------------------------------------------------------------
    float kDitherFlat = 0.004f;    // under this the dither is speckle, not a fray
    float kDitherSteep = 0.30f;    // over this a tread is already a voxel wide
    float kDitherVox = 0.55f;      // peak to peak, voxels -- measured by eye
    float kWarpFlat = 0.0012f;     // under this there is no step in view at all
    float kWarpFull = 0.0036f;     // ...and by this the warp is at full strength
    float kWarpVox = 0.90f;        // peak to peak, voxels
    float kWarpCellM = 5.0f;       // wavelength, world metres

    // -- THE DITHER, AND THE GATE THAT WAS ALREADY MEASURED BY EYE ----------
    //
    // 0.55 AND NOT 1.0, on the snowfield at (1050, 1275). At a full voxel every
    // column can flip, so the whole slope becomes speckle and the terraces go
    // with the smooth ground between them. At 0.55 only a column already near a
    // voxel boundary moves: the TREAD stays flat and its EDGE frays. Unchanged.
    float terraceDitherWeight(float grade) const {
        if (grade <= kDitherFlat || grade >= kDitherSteep) return 0.0f;
        const float up = minf(1.0f, (grade - kDitherFlat) / (4.0f * kDitherFlat));
        const float dn = minf(1.0f, (kDitherSteep - grade) / (0.5f * kDitherSteep));
        const float w = minf(up, dn);
        return w * w * (3.0f - 2.0f * w);
    }

    // -- ...AND THE WARP, WHICH REACHES FURTHER DOWN THE GRADE ---------------
    float terraceWarpWeight(float grade) const {
        if (grade <= kWarpFlat || grade >= kDitherSteep) return 0.0f;
        const float up = minf(1.0f, (grade - kWarpFlat) / (kWarpFull - kWarpFlat));
        const float dn = minf(1.0f, (kDitherSteep - grade) / (0.5f * kDitherSteep));
        const float w = minf(up, dn);
        return w * w * (3.0f - 2.0f * w);
    }

    // -----------------------------------------------------------------------
    // WHAT A COLUMN GETS ADDED TO BREAK ITS TERRACE, in world metres.
    //
    // TWO MECHANISMS THAT HAND OVER, AND THE HAND-OVER IS THE POINT. This was
    // the dither alone, and an A/B rendered on the snowfield says that is the
    // right tool where it is switched on and the wrong one where it is off:
    //
    //   dither  frays the tread EDGE into dashes. On the 5.5 m treads at
    //           (1050, 1275) the fray is 3 m wide and the ring stops being a
    //           line. Rendered both ways, the dashes beat anything smooth,
    //           because a broken line is not a line.
    //   warp    a smooth value-noise field, half a voxel deep and a few metres
    //           across, that moves the CONTOUR sideways by amplitude/grade
    //           instead of moving the column. It leaves the line CONTINUOUS --
    //           measurably worse at 5.5 m, where it only makes the ring wavy --
    //           but it cannot speckle at any grade, because two neighbours
    //           differ by the field's own gradient rather than a whole voxel.
    //
    // So the dither keeps every grade it already had, and the warp is weighted
    // by (1 - dither) so it appears only as the dither fades out. That fade is
    // the whole complaint: the dither's fray is the same FRACTION of a tread at
    // every grade, so on gentle ground it is not weaker, it is physically wider
    // -- nine metres of randomly flipped columns on a 25 m tread, which is the
    // speckle "remove that noise from all terrain" was about, and is why the
    // gate is there. Measured over Ouachita, treads wider than 12.5 m were
    // getting a mean dither weight of 0.06, i.e. nothing, and those are the
    // widest and most visible bands in the world. (user 2026-09-19: "there are
    // also straight lines forming in the terrain".)
    //
    // Neither can move a column further than the rounding already does, so the
    // standing "no noise on terrain" rule holds: every column still lands
    // within half a voxel of the measurement, which is strictly closer than a
    // terrace -- a tread is a whole voxel of error held in a straight line.
    // -----------------------------------------------------------------------
    float terraceBreakM(float x, float z, float grade) const {
        float out = 0.0f;
        const float d = terraceDitherWeight(grade);
        if (d > 0.0f) {
            const int ci = int(std::floor(x / VOXEL_M));
            const int cj = int(std::floor(z / VOXEL_M));
            const float n = hashUnit(uint32_t(ci) * 2654435761u, uint32_t(cj) * 40503u);
            out += (n - 0.5f) * (kDitherVox * VOXEL_M) * d;
        }
        const float w = terraceWarpWeight(grade) * (1.0f - d);
        if (w > 0.0f) {
            const float n =
                vnoise(x * (1.0f / kWarpCellM) + 71.3f, z * (1.0f / kWarpCellM) + 19.7f);
            out += (n - 0.5f) * (kWarpVox * VOXEL_M) * w;
        }
        return out;
    }

    bool aboveTimberlineVox(int hVox) const {
        return timberlineWorldM >= 0.0f && hVox * VOXEL_M >= timberlineWorldM;
    }

    // A HARD LINE IS A CONTOUR, and a contour drawn across a mountain reads as
    // a bug. Trees thin out through the band below the line instead, on a hash
    // of the column so the answer is stable -- the same column must refuse a
    // tree every time it is asked or the wood flickers as chunks reload.
    // -----------------------------------------------------------------------
    // SNOW ON THE HIGH GROUND, AND IT IS THE SAME SHAPE AS THE TIMBERLINE.
    //
    // (user 2026-09-18: "I want you to add snow to the tall mountain peaks.")
    //
    // A DITHERED LINE, NOT A CONTOUR. Switching at an elevation draws a level
    // ring right round every peak -- the one shape a mountain never has -- so
    // the same per-column hash the timberline uses fades snow in across a band,
    // and the edge comes out ragged and follows the ground instead of cutting
    // across it.
    //
    // 3,650 m IS CHOSEN FROM THE PARK, not from a preference. Rocky Mountain's
    // summer snow sits in the couloirs and on the north faces above roughly
    // that, and it is far enough over the 3,505 m timberline that snow and the
    // last trees do not fight over the same ground. In this window it puts
    // snow on Longs (4,344), Meeker, Ypsilon, Chiefs Head, Hagues, Otis,
    // Hallett and Specimen, and on nothing in the valleys.
    //
    // THE IMAGERY CAN PUT IT LOWER BUT NEVER HIGHER. Where the classifier
    // actually saw snow, it is snow whatever the altitude says -- that is a
    // measurement and this is a rule of thumb. The reverse is not allowed: NAIP
    // is flown in summer, so "no snow in the photograph" is not evidence of
    // bare rock in the way "snow in the photograph" is evidence of snow.
    static constexpr float kSnowlineAslM = 3770.0f;
    static constexpr float kSnowFadeAslM = 220.0f;   // in REAL metres
    // How deep it lies where it lies at all, in WORLD metres. Eight voxels: a
    // layer you can see the edge of from across a cirque, and thin enough that
    // it follows the rock underneath instead of burying it into a dome.
    static constexpr float kSnowDeepM = 0.8f;
    // The grades snow holds on and slides off. Nothing under the first keeps a
    // full load; nothing over the second keeps any.
    static constexpr float kSnowHoldGrade = 0.35f;
    static constexpr float kSnowShedGrade = 0.80f;

    // ------------------------------------------- SMOOTH VALUE NOISE, NO MEMO
    // The fbm the terrain uses carries an FbmMemo, and this is asked from
    // materialAt, which has no memo to hand and is called per voxel. Two
    // lattice reads and a smoothstep is enough for drifts.
    static float snowNoise(float x, float z) {
        const float xi = floorf(x), zi = floorf(z);
        const float fx = x - xi, fz = z - zi;
        const float ux = fx * fx * (3.0f - 2.0f * fx);
        const float uz = fz * fz * (3.0f - 2.0f * fz);
        auto H = [](float a, float b) {
            uint32_t h = uint32_t(int(a)) * 374761393u ^ uint32_t(int(b)) * 668265263u;
            h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
            return float(h & 0xFFFFu) * (1.0f / 65535.0f);
        };
        const float a = H(xi, zi), b = H(xi + 1.0f, zi);
        const float c = H(xi, zi + 1.0f), d = H(xi + 1.0f, zi + 1.0f);
        const float lo = a + (b - a) * ux, hi = c + (d - c) * ux;
        return lo + (hi - lo) * uz;
    }

    // -----------------------------------------------------------------------
    // HOW DEEP THE SNOW LIES HERE, IN WORLD METRES. 0 is bare ground.
    //
    // (user 2026-09-18: "make the snow on peaks ON the terrain as well as the
    // terrain itself. theres also square patches of snow, this is wrong, it
    // needs to look natural" + "you have snow at all elevations instead of just
    // the peaks".)
    //
    // THREE THINGS WERE WRONG WITH THE FIRST CUT AND ONE LINE CAUSED TWO OF
    // THEM. It read the cover's Snow class and set the fade straight to full:
    //
    //     if (cover_.atPoint(...) == CoverField::Snow) t = 1.0f;
    //
    // That IGNORED ALTITUDE, so anywhere the classifier said snow got snow --
    // and NAIP is flown in SUMMER, where its snow class is mostly bright bare
    // rock and pale sand scattered at every elevation. Hence snow in the
    // valleys. And it read atPoint, the unjittered cell, so each patch came out
    // as a hard 10.29 m square. The reasoning written over it -- "snow in the
    // photograph is evidence of snow" -- is exactly backwards for summer
    // imagery, and the DEM was the reliable signal all along. The cover is not
    // consulted at all now.
    //
    // THE THIRD WAS THE EDGE. A per-column hash against the fade gives white
    // noise: single columns of snow speckled over bare rock, which is not what
    // a snowline looks like from any distance. What follows is a FIELD instead,
    // and every term in it is something snow actually does:
    //
    //   ALTITUDE   it gets deeper the higher you go, over a 220 m fade.
    //   SLOPE      it slides off steep ground. This is the term that makes it
    //              look natural rather than painted: snow fills the couloirs
    //              and benches and leaves the buttresses bare, so the pattern
    //              follows the shape of the mountain instead of cutting across
    //              it. Nothing else here can do that.
    //   DRIFTS     a smooth noise, tens of metres across, so two neighbouring
    //              gullies are not identically full.
    //
    // The edge comes out of the depth reaching zero, which is continuous by
    // construction -- there is no threshold to be ragged or square.
    //
    // IT TAKES THE BARE HEIGHT AS AN ARGUMENT so heightM, which has already
    // computed it, does not pay for it twice -- and so that this stays a pure
    // function of (x, z). materialAt has to agree with heightM about the
    // thickness of the layer to the voxel, and the only way to guarantee that
    // is for both to ask the same function about the same point.
    // -----------------------------------------------------------------------
    float snowDepthAt(float x, float z, float bareY) const {
        if (!dem_.ok()) return 0.0f;
        const float asl = dem_.worldToAsl(bareY);
        float t = (asl - (kSnowlineAslM - kSnowFadeAslM)) / kSnowFadeAslM;
        if (t <= 0.0f) return 0.0f;      // the valleys, and this is the whole world
        t = minf(1.0f, t);
        // ...it slides off anything steep. Measured over 8 real metres, which is
        // the scale a slab actually fails on.
        const float sh = dem_.shrink();
        const float d = 4.0f / sh;
        const float rise = maxf(fabsf(dem_.heightM(x + d, z) - dem_.heightM(x - d, z)),
                                fabsf(dem_.heightM(x, z + d) - dem_.heightM(x, z - d)));
        const float grade = rise * sh / 8.0f;
        t *= clampf((kSnowShedGrade - grade) / (kSnowShedGrade - kSnowHoldGrade), 0.0f, 1.0f);
        if (t <= 0.0f) return 0.0f;
        // ...and it drifts. 0.35 + 0.65 keeps a floor under it, so a snowfield
        // is uneven rather than moth-eaten.
        t *= 0.35f + 0.65f * snowNoise(x * 0.09f + 11.3f, z * 0.09f + 4.7f);
        return t * kSnowDeepM;
    }
    float snowDepthM(float x, float z) const {
        if (!dem_.ok()) return 0.0f;
        return snowDepthAt(x, z, dem_.heightM(x, z));
    }
    // Half a voxel of lying snow is the thinnest that can be drawn at all.
    bool snowAt(int i, int j) const {
        return snowDepthM(wx(i), wx(j)) >= 0.5f * VOXEL_M;
    }

    uint8_t snowShade(int i, int j) const {
        uint32_t h = uint32_t(i) * 374761393u ^ uint32_t(j) * 1274126177u;
        h ^= h >> 15; h *= 2654435761u; h ^= h >> 13;
        return uint8_t(mat::SNOW_0 + int(h % uint32_t(mat::SNOW_COUNT)));
    }

    bool treeRejectedByAltitude(int i, int j, int hVox) const {
        if (timberlineWorldM < 0.0f) return false;
        const float t = (hVox * VOXEL_M - (timberlineWorldM - timberlineFadeM)) /
                        timberlineFadeM;
        if (t <= 0.0f) return false;
        if (t >= 1.0f) return true;
        uint32_t h = uint32_t(i) * 374761393u ^ uint32_t(j) * 668265263u;
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        return float(h & 0xFFFFu) * (1.0f / 65535.0f) < t;
    }
    const DemField &dem() const { return dem_; }
    bool usingDem() const { return dem_.ok(); }
    DemField dem_;

    // ---------------------------------------------------- WHAT THE GROUND IS
    // Optional, and loaded AFTER the DEM because it has to be handed the same
    // shrink -- one scale in the world, and it belongs to the terrain.
    bool loadCover(const std::string &path) {
        return cover_.load(path, dem_.ok() ? dem_.shrink() : 1.0f);
    }
    const CoverField &cover() const { return cover_; }
    bool usingCover() const { return cover_.ok(); }
    CoverField cover_;

    // Does the imagery say a tree belongs here? With no cover loaded this is
    // true everywhere, which is the old behaviour exactly.
    bool coverAllowsTree(float x, float z) const {
        if (!cover_.ok()) return true;
        // NOTHING GROWS IN THE LAKE, WHATEVER ELSE IS DOUBTED. The water
        // class is trusted under --cover-water -- it is the half of the
        // imagery that is right -- so it has to be asked BEFORE the gate
        // opens, or every pond and the whole sea comes up planted with birch.
        // Rendered: a flooded wood standing in blue, which is what this line
        // being below the next one looks like.
        if (cover_.at(x, z) == CoverField::Water) return false;
        if (!coverGround) return true;
        return cover_.at(x, z) == CoverField::Forest;
    }

    // -- TRUST THE PICTURE FOR THE WATER AND NOT FOR THE GROUND -----------
    //
    // (user 2026-09-18: "import the acadia national park dataset, and use our
    // birch trees ontop of the terrain".)
    //
    // A .vbcov CARRIES TWO DIFFERENT CLAIMS and they are not equally good
    // outside Colorado. Over Mount Desert Island the classifier gets the WATER
    // right -- 13.0% of the window, half of it under 3 m, the rest clustered
    // at 83 m where Eagle Lake and Jordan Pond actually are, mean grade 5% --
    // and the ground badly wrong: 41.9% bare ROCK at a median of 64 m, on an
    // island whose granite is all above 250 m. Rendered, that is a lavender
    // waste with 2,867 trees on it against 8,018 with the imagery ignored.
    //
    // So this switch says WHICH HALF to believe. False keeps mappedWater,
    // lakeLineAt and the shore distance -- the sea, the ponds and their banks
    // -- and hands the trees, the ground colour and the timberline back to the
    // engine. Without it Acadia has to choose between a forest and a coast.
    bool coverGround = true;

    // ------------------------------------------- THIN ONLY THE THICKEST STANDS
    // (user 2026-09-18: "decrease the density by 50%, but only where the trees
    // are very very dense -- leave the sparse areas alone".)
    //
    // The knob that already existed, grassDensity's sibling, is GLOBAL: turning
    // it down takes the same fraction out of a closed canopy and out of the
    // half-dozen pines on an open bench, and the open ground is where losing
    // trees shows. So the rate is driven by how closed the stand actually is,
    // measured from the imagery -- the one thing that knows the difference.
    //
    // RAMPED, NOT SWITCHED. Nothing happens below kCrowdT; from there the
    // rejection rises smoothly to kCrowdMax at a fully closed canopy. A hard
    // threshold would draw a visible line around every thicket, which is the
    // same mistake the hard cover edges made before they were jittered.
    //
    // Hashed on the column so a site answers the same way every time it is
    // asked, or the wood thins and refills as chunks reload.
    // ------------------------------------------- REAL FRONT RANGE STAND DENSITY
    // Replaces the crowding hack entirely. That thinned by how closed the
    // canopy looked; this asks what actually grows at this altitude and puts
    // that many stems there.
    //
    // Stems per REAL hectare, by zone. These are field numbers for the
    // Colorado Front Range, not a curve that looked nice:
    //
    //     ponderosa savanna   2000-2600 m     100 -  250
    //     Douglas-fir/mixed   2400-2900 m     400 -  700
    //     lodgepole           2700-3200 m    1000 - 2500   (dense, even-aged)
    //     spruce-fir          3000-3500 m     400 -  900
    //     krummholz           3400 m+          50 -  200
    //
    // Interpolated between zone midpoints so a hillside changes forest type
    // gradually, the way one does.
    static float realStemsPerHa(float aslM) {
        struct P { float m, stems; };
        static const P k[] = {{1800.f, 120.f}, {2300.f, 175.f}, {2650.f, 550.f},
                              {2950.f, 1750.f}, {3250.f, 1200.f}, {3450.f, 650.f},
                              {3600.f, 120.f}, {3800.f, 20.f}, {4400.f, 0.f}};
        const int n = int(sizeof k / sizeof k[0]);
        if (aslM <= k[0].m) return k[0].stems;
        for (int i = 1; i < n; ++i)
            if (aslM <= k[i].m) {
                const float t = (aslM - k[i-1].m) / (k[i].m - k[i-1].m);
                return k[i-1].stems + (k[i].stems - k[i-1].stems) * t;
            }
        return k[n-1].stems;
    }

    // WHAT THE SCATTER ACTUALLY PRODUCES, measured: 145 trees per world
    // hectare of forested ground at the lake and 148 on the peak flank before
    // any thinning. Near enough uniform, which is why one number works as the
    // divisor for an acceptance rate.
    static constexpr float kScatterStemsPerWorldHa = 146.0f;

    // THE DIVISOR, AND WHY IT IS NOT shrink SQUARED BY DEFAULT.
    //
    // Strictly, a world hectare COVERS shrink^2 = 36 real hectares, so "one
    // tree in the world for one tree in Colorado" divides real stems/ha by 36.
    // That is the honest count -- and it was rendered, and it is a BARE
    // MOUNTAIN: one pine in a 300 m view of ground the imagery calls forest.
    // Correct, and useless.
    //
    // Dividing by the shrink itself keeps the real SHAPE of the stand tables
    // -- dense lodgepole at 2,900 m, thin krummholz at 3,600, savanna down at
    // the lake -- at a density you can walk through. That is the default.
    // `--stem-div 36` is the literal one-for-one; `--stem-div 1` is real
    // density at real scale.
    float stemDiv = 0.0f;   // 0 = derive it from the shrink

    float stemTargetPerWorldHa(float aslM) const {
        const float sh = dem_.ok() ? dem_.shrink() : 1.0f;
        const float div = (stemDiv > 0.0f) ? stemDiv : sh;
        return realStemsPerHa(aslM) / maxf(1.0f, div);
    }

    // Hashed on the column so a site answers the same way every time, or the
    // wood thins and refills as chunks reload.
    // -----------------------------------------------------------------------
    // HOW FULL THE LATTICE HAS TO BE HERE -- ONE RULE, BOTH DIRECTIONS.
    //
    // This replaces treeRejectedByDensity, which could only ever THIN: it
    // computed target/delivered and threw candidates away when that was under
    // one, and did nothing at all when it was over. That was a fair description
    // of the problem at shrink 6, where the table asks for more than the
    // scatter can offer only in the two densest bands -- and it is the wrong
    // shape entirely at 1:1, where the table asks for 1,750 stems a hectare and
    // the wood was delivering 146. Twelve times too sparse, silently, because
    // the only lever pointed downwards.
    //
    // IT IS A MULTIPLIER ON THE ACCEPTANCE now, so over one fills the lattice
    // and under one empties it, and the saturation is free: the scatter tests
    // `hash > standGate * density * fill`, and a product over 1.0 is simply
    // never rejected. There is no second clamp to keep in step with the first.
    //
    // WHAT IT CANNOT DO is exceed the lattice. The pine grid is 2.4 m and the
    // clash test refuses anything within a trunk's width of a tree already
    // placed, so there is a ceiling no acceptance rate can pass -- see
    // kScatterStemsPerWorldHa for what the pipeline actually delivers. Asking
    // for more than that is not an error, it just stops helping.
    // -----------------------------------------------------------------------
    float stemFill(float x, float z, int hVox) const {
        (void)x; (void)z;
        if (!dem_.ok()) return 1.0f;
        const float asl = dem_.worldToAsl(float(hVox) * VOXEL_M);
        return clampf(stemTargetPerWorldHa(asl) / kScatterStemsPerWorldHa, 0.0f, 16.0f);
    }

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
    //     ... | pine | birch | oak | pine | birch | oak | ...
    //          -400   +400  +1200  +2000  +2800  +3600     metres, band centres
    //
    // 800 m a band, which is a real walk -- a minute and a half at the new run
    // speed -- and wider than the 307 m view radius, so a band fills the view
    // rather than being a stripe you see both edges of.
    //
    // THE OAK TOOK A NEW SLOT RATHER THAN MOVING THE OTHER TWO. The period went
    // from 2W to 3W and the oak was put at [W, 2W), which leaves the birch band
    // exactly where it was at [0, W) and puts pine at [2W, 3W) -- whose centre
    // 2.5W is the same -W/2 it always was once the period wraps. bandCentre
    // therefore answers identically for both existing woods, so /locate, the
    // default spawn and every --birch / --pine render still land where they did.
    //
    // WHAT IT DOES NOT PRESERVE, AND CANNOT: the wood at an ARBITRARY x beyond
    // the first period. Inserting a band changes the tiling, so x = 1200 was
    // pine and is oak, x = 2000 was birch and is pine, and so on outward. There
    // is no way to add a third wood to a repeating tiling without that being
    // true -- a saved coordinate far from the origin may now name a different
    // forest, and a screenshot taken at one is not reproducible from the
    // coordinate alone. Pinning the wood with --pine / --birch / --oak is, and
    // that is what those flags are for.
    //
    // `biome` survives as the FORCED override for --birch and --pine: set it
    // and the bands are ignored. That is what makes a screenshot or a profile
    // run reproducible without having to also pin a position.
    // ---------------------------------------------------------------------
    static constexpr float kBandW = 800.0f;    // metres of one band
    static constexpr float kBandBlend = 90.0f; // metres the two are mixed over

    // Where the centre of each band sits, so /locate has somewhere to send you.
    static float bandCentre(Biome b) {
        return (b == Biome::Birch)  ? kBandW * 0.5f
               : (b == Biome::Oak)  ? kBandW * 1.5f
                                    : -kBandW * 0.5f;
    }

    // HOW MANY BANDS THE WORLD REPEATS OVER. One number, so nothing downstream
    // can hold a stale copy of the period: App::nearestBandX had `2.0f * kBandW`
    // written out and became silently wrong the moment the oak was inserted --
    // /locate would have walked you to a multiple of the OLD period, which lands
    // in whichever wood happens to be there.
    static constexpr float bandCount() { return 3.0f; }

    // Which band index a biome occupies in [0, 3W). See the note above for why
    // these three numbers and not some other three.
    static int bandIndex(Biome b) {
        return (b == Biome::Birch) ? 0 : (b == Biome::Oak) ? 1 : 2;
    }

    // -----------------------------------------------------------------------
    // HOW MUCH OF EACH WOOD THIS COLUMN IS, AND IT IS ONE FUNCTION.
    //
    // The two-wood version returned a single scalar and every caller read it as
    // "1 means birch, 0 means pine" -- which stops being true the moment there
    // is a third wood, because 0 now means "pine OR oak". So the primitive is
    // the three weights, and birchMix/oakMix are views onto it.
    //
    // THEY SUM TO EXACTLY 1 AT EVERY x, which is what lets a caller lerp with
    // them without normalising. Only ADJACENT bands ever mix: a seam is 90 m
    // and a band is 800, so no column is ever within reach of two seams, and
    // the blend is always between the band you are in and one neighbour.
    //
    // A PURE FUNCTION OF x -- no noise, no memo -- so anything may ask it at
    // any time, which several callers rely on.
    // -----------------------------------------------------------------------
    static void woodWeights(float x, float *pine, float *birch, float *oak) {
        const float period = bandCount() * kBandW;
        float u = fmodf(x, period);
        if (u < 0.0f) u += period;
        const int band = mini(2, int(u / kBandW));   // 0 birch, 1 oak, 2 pine
        const float within = u - float(band) * kBandW;
        float w[3] = {0.0f, 0.0f, 0.0f};
        const float half = kBandBlend * 0.5f;
        if (within < half) {
            // Near the LOW edge: mixing with the band before this one.
            const float t = sstep(saturate(within / kBandBlend + 0.5f));
            w[band] = t;
            w[(band + 2) % 3] = 1.0f - t;
        } else if (within > kBandW - half) {
            // ...and near the HIGH edge, with the band after it.
            const float t = sstep(saturate((kBandW - within) / kBandBlend + 0.5f));
            w[band] = t;
            w[(band + 1) % 3] = 1.0f - t;
        } else {
            w[band] = 1.0f;
        }
        *birch = w[0];
        *oak = w[1];
        *pine = w[2];
    }

    // 0 in the pine band, 1 in the birch band, eased across the seam -- and 0
    // through the whole oak band, which is what every existing caller wants
    // until it is taught about the oak: "not birch" falls back to the pine
    // answer, and the pine answer is the one this engine shipped with.
    static float birchWeight(float x) {
        float p = 0.0f, b = 0.0f, o = 0.0f;
        woodWeights(x, &p, &b, &o);
        return b;
    }
    static float oakWeight(float x) {
        float p = 0.0f, b = 0.0f, o = 0.0f;
        woodWeights(x, &p, &b, &o);
        return o;
    }

    // THE SAME SIGNED DISTANCE, IN METRES, because the waterline needs it and
    // birchWeight throws it away. Positive inside the birch, negative inside
    // the pine, magnitude = how far in. Asking birchMix instead does not work:
    // the waterline's old cutoff sat at mix <= 0.001, and sstep is quadratic
    // near zero, so the whole useful range of mix there spans about two metres
    // of world -- nothing to fade over. Distance has 800 m of band to use.
    // STILL SIGNED TOWARD THE BIRCH, and the oak counts as "not birch" here
    // for the same reason birchWeight does -- its one caller, waterSeamFade,
    // exists only for the dry-birch case and early-returns before reaching this
    // whenever birchWater is set, which it now always is.
    static float bandDist(float x) {
        const float period = bandCount() * kBandW;
        float u = fmodf(x, period);
        if (u < 0.0f) u += period;
        if (u < kBandW) return minf(u, kBandW - u);           // inside the birch
        return -minf(u - kBandW, period - u);                 // oak or pine
    }

    // Forced, when --birch or --pine pinned it; otherwise whatever the bands
    // say at this position.
    bool forced = false;
    Biome biome = Biome::Pine;
    bool birchAt(float x) const { return forced ? (biome == Biome::Birch) : birchWeight(x) >= 0.5f; }
    float birchMix(float x) const {
        return forced ? (biome == Biome::Birch ? 1.0f : 0.0f) : birchWeight(x);
    }
    bool oakAt(float x) const { return forced ? (biome == Biome::Oak) : oakWeight(x) >= 0.5f; }
    // WHICH WOOD THIS IS, BY NAME. Five call sites asked `birchAt ? "birch" :
    // "pine"` and every one of them called the oak band a pine wood -- /locate
    // sent you to "the pine wood at 1200", which is the oak's own centre. One
    // function so a fourth wood cannot reintroduce that five times over.
    const char *woodName(float x) const {
        return oakAt(x) ? "oak" : birchAt(x) ? "birch" : "pine";
    }
    // -----------------------------------------------------------------------
    // WHICH WOOD THIS IS, AS A BIT, FOR THE POPULATIONS THAT BELONG TO ONE.
    //
    // (user 2026-09-17: "add the grass snake, frog, and mouse to the oak
    //  forest. remove the armadillo and porcupine from the oak forest.")
    //
    // Every per-wood life gate used to be
    // `(sp.wood == 1) != (birchMix(sx) >= 0.5f)`, which is a TWO-wood test --
    // and birchMix is 0 through the whole oak band, so a pine-only species
    // PASSED there and a birch-only species was REFUSED. Not a mis-tuning: the
    // oak silently answering "pine" to every question about which wood it is.
    // The armadillo, the porcupine and the fly stood in the oak with "pine
    // only" printed beside them at load, and the mouse, the grass snake and the
    // frog could not be there at all.
    //
    // A BIT RATHER THAN AN INDEX, so a table can name a SET -- "both broadleaf
    // bands" is the commonest answer and has no index. v1 reached the same
    // shape from the other side: its BIO_OAKF is read by sim/nav.js as EITHER
    // broadleaf band, and DES_BIRCHF exists only to narrow one species back out
    // of the oak again.
    //
    // THE LARGEST WEIGHT WINS, rather than a threshold on one mix. Inside a
    // seam both neighbours have weight and the larger is the wood you are in --
    // the same answer woodName gives, so a creature and the /locate line that
    // sends you to it cannot disagree about where it lives.
    uint8_t woodBit(float x) const {
        float wp = 0.0f, wb = 0.0f, wo = 0.0f;
        woodMix(x, &wp, &wb, &wo);
        if (wo >= wb && wo >= wp) return kWoodOak;
        return (wb >= wp) ? kWoodBirch : kWoodPine;
    }
    float oakMix(float x) const {
        return forced ? (biome == Biome::Oak ? 1.0f : 0.0f) : oakWeight(x);
    }
    // ...and all three at once, for the callers that genuinely need to weigh
    // them against each other rather than ask twice.
    void woodMix(float x, float *pine, float *birch, float *oak) const {
        if (forced) {
            *pine = biome == Biome::Pine ? 1.0f : 0.0f;
            *birch = biome == Biome::Birch ? 1.0f : 0.0f;
            *oak = biome == Biome::Oak ? 1.0f : 0.0f;
            return;
        }
        woodWeights(x, pine, birch, oak);
    }
    // The old whole-world question, kept for the things that genuinely are
    // global: which model sets to LOAD, and whether the hive pass can run at
    // all. Both woods' trees are loaded whenever the bands are live.
    bool birch() const { return !forced || biome == Biome::Birch; }
    bool oak() const { return !forced || biome == Biome::Oak; }

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

    // RAISED ONE METRE -- ten voxels (user 2026-09-13, "can you just raise the
    // water level by 10 voxels"). Moved HERE, on the still level itself,
    // rather than as a lift on the voxel it quantises to: everything
    // downstream reads waterAt -- the basin carve, the bank shaping, the depth
    // rule, the swim, the lake life's water field -- so raising the number
    // itself keeps all of them describing one lake. A lift applied at
    // waterVoxAt would move the SURFACE and leave the carve shaping a basin
    // for the old one.
    // 34 -> 36 (user 2026-09-17: "increase the water surface area ... theres
    // way too much sand"). Two metres of line is 3.3x the water and it CUTS
    // the beach rather than growing it, which is the part worth stating
    // plainly: the shore band is a fixed rise above the LINE, so raising the
    // line does not widen it -- it moves it up onto steeper ground, where the
    // same rise covers less plan area. Measured over 640 x 1600 m of pine:
    //
    //     34.0   4.41% wet, 28 bodies, 68 dry sandy columns per 100 wet
    //     36.0   8.17% wet, 38 bodies, 53
    //     38.0  13.41% wet, 44 bodies, 40
    //
    // 38 was tempting and is too much -- 13% of a wood under water is a
    // wetland, and the body count says it gets there by flooding the ground
    // between lakes rather than by making lakes.
    float pineWater = 36.0f;          // metres, read off the pine field above
    // The fade starts AT the band boundary and runs 120 m. sstep has zero
    // derivative at each end, so the line is already flat where the fade
    // approaches 1 -- which is exactly where the lakes are -- and all the steep
    // part happens where the line is far under the ground and there is no
    // water to tilt. Starting 90 m inside and running 160 left only 300 m of
    // an 800 m band at full waterline and HALVED the world's water.
    float waterSeamEdgeM = 0.0f;
    float waterSeamFadeM = 120.0f;
    // HOW FAR THE LINE SINKS, and it must clear the BIRCH wood, not just the
    // pine. 16 m looked right against pine's minimum of 20.8 and was badly
    // wrong: the sunk line carries on through the birch band at that value,
    // and 33 - 16 = 17 sits ABOVE the birch median of 14.3, so it flooded the
    // wood it was meant to keep dry. 30 puts it at 3.0 m, under birch's
    // minimum of 5.8. The line must end below EVERY ground it sinks through.
    float waterSeamDropM = 30.0f;
    // 9.5 m, AND THE BIRCH WOOD HAS LAKES NOW (user 2026-09-12). It was
    // kNoWater, and the note above records why: a single 33 m line put 85.1%
    // of the birch wood under water, because that wood's floor sits at a
    // median of 14.3 m against the pine's 48.4.
    //
    // The answer was never "no water", it was "not the PINE's line". 9.5 sits
    // just under the birch field's own p5 of 9.9 -- the same relationship
    // pineWater has to the pine field, whose p5 is 33.8 against a line of 33.
    // Measured rather than guessed; see tests/water_survey.cpp, which reports
    // the two bands separately for exactly this.
    // 5.5, AND IT IS DELIBERATELY UNDER THE BIRCH FIELD'S MINIMUM of 5.8.
    //
    // That is what makes "lakes or ponds, no puddles" true by construction:
    // natural birch ground is never below this line, so water can only exist
    // where a basin was actually CARVED. There are no shallow sheets left over
    // from the landform dipping under a line, which is what 9.5 gave -- 45,000
    // wet columns a metre deep, read as "large flat banks and small puddles".
    //
    // At 9.5 the wood also flooded once the carve started working: birch
    // ground sits a median 14.3 m against that line, so any real carve pulled
    // enormous areas under it -- 16.3% of the region wet.
    // 7.5 m, ANOTHER METRE (user 2026-09-13, "raise the water level in the
    // birch forest, there's too much empty sandy bank"). Swept in
    // tests/water_survey.cpp, which now takes birchWater as argv[8] for this:
    //
    //     birchWater   birch wet   bodies   SAND PER WATER COLUMN
    //        6.5        4.60%       560          0.81
    //        7.5        6.56%       659          0.73   <-- the floor
    //        8.0        7.69%       661          0.76
    //        9.0       10.57%       789          0.82
    //
    // THE LEVER SATURATES AT 7.5 AND THEN REVERSES, which is the thing worth
    // knowing and is not what anyone expects of "raise the water". The sand
    // band is defined by VERTICAL extent, so on the birch's nearly flat
    // lakeside ground it is enormous in PLAN -- the same trap bankRiseM's
    // note records. Past 7.5 each further metre floods a wide flat apron and
    // hands the new shoreline a wider sand ring than it drowned.
    //
    // So this is the whole of what raising the line can do about bare bank:
    // 0.81 -> 0.73 against the pine wood's 0.45. What closes the rest of that
    // gap is a shorter sand band for this wood -- 0.7 m puts it at 0.46 --
    // and that is a separate constant, not this one.
    // ONE BROADLEAF LINE, 2026-09-17 ("the birch and oak should share the same
    // terrain/water generation"). 7.5 was measured against the birch's OWN
    // field, which no longer exists -- heightM now gives the birch oakHeight,
    // and a waterline is a property of the height DISTRIBUTION rather than of
    // the wood, so the old number stopped meaning anything the moment the
    // ground under it changed. See the note over oakWater, which says exactly
    // this about the oak and is the reason that one had to be re-measured too.
    // 5 -> 6, WITH THE OAK, and the two still match each other. See
    // pineWater for the lever and tests/puddle_survey.cpp for the sweep:
    //
    //     5.0   6.52% wet, 15 bodies, 57 dry sandy columns per 100 wet
    //     6.0   8.52% wet, 24 bodies, 54
    //     7.0  10.86% wet, 27 bodies, 52
    //
    // THE BIRCH HAS THE PINE'S MECHANICS ALREADY and has had since both woods
    // were put through one carve and one bank -- see the single return at the
    // end of heightM. What it does not have, and must not, is the pine's
    // NUMBER: 36 m sits eleven metres over the birch field's median. A wood
    // with a quarter of the relief needs its own line against its own height
    // distribution, and that is the whole of the difference between them.
    float birchWater = 6.0f;   // == oakWater, and must stay equal
    // -- AND THE OAK'S, WHICH IS NOT OPTIONAL ---------------------------------
    //
    // (user 2026-09-16: "import the oak forest from v1 into v2".)
    //
    // THE OAK BAND WOULD HAVE DROWNED WITHOUT THIS. waterAt blended pine to
    // birch on birchMix, and birchMix is 0 through the whole oak band -- so the
    // oak would have been handed the PINE line at 34.0 m over a floor that runs
    // 9.4 to 16.2. Every column of it, twenty metres down. Nothing about that
    // would have looked like a waterline; it would have looked like the oak
    // forest failing to generate.
    //
    // 8.2 m, AND IT WAS MEASURED RATHER THAN REASONED. The first value here was
    // 11.1, put a quarter of the way up the oak's 9.4..16.2 formula range on
    // the argument that the birch's 7.5 sits a quarter of the way up ITS field.
    // That flooded 27.3% of the oak band.
    //
    // THE DOUBLE SMOOTHSTEP IS WHY. It pushes the distribution into the middle
    // of the range, so the band's real spread is far tighter than the formula's
    // endpoints suggest -- swept over a 900 m square, the oak runs
    //
    //     min 6.2   p5 7.8   med 12.5   p95 15.3   max 16.5
    //
    // -- and 11.1 is not a quarter of the way up that, it is essentially the
    // median. A quarter of a RANGE and a quarter of a DISTRIBUTION are not the
    // same number and this field is the worst case for assuming they are.
    //
    // 5.0 puts 7.7% of the band under water, against the birch's 7.6%: a wood
    // with lakes in it rather than a marsh. tests/oak_survey.cpp prints the
    // sweep that chose it, and re-running it is how to move this number.
    //
    // IT MOVED WITH THE HILLS AND THAT IS THE POINT OF KEEPING THE SWEEP. When
    // the oak was given real relief -- 6.8 m of v1's literal port became 26 --
    // the floor dropped out from under this line and the same 8.2 that had been
    // right went to 17.2% wet. A waterline is not a property of the wood, it is
    // a property of the wood's height DISTRIBUTION, so it has to be re-measured
    // every time the field moves. Nothing warns you: a drowned band still
    // renders.
    // 5 -> 6, with the birch, and for the same sweep -- see birchWater. The
    // oak ends up the wettest of the three at 11.3% because its floor was
    // already dropped kOakDropM to give it water at all; that is the band
    // reading as a valley wood rather than a hillside one, and it is wanted.
    float oakWater = 6.0f;

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
    // TWICE AS DEEP IN THE BIRCH (user 2026-09-13). That wood's whole relief is
    // 22 m against the pine's 90, so a basin cut to the pine's 4.5 m reads as a
    // puddle in it -- the ground simply does not have far to fall. 9.0 gives
    // the birch lakes the same presence the pine's have.
    //
    // BLENDED ON birchMix, not switched: the carve is applied to a HEIGHT, and
    // a step in how deep the ground is cut is a step in the ground.
    float birchBasinBed = 6.0f;

    // How far ABOVE the waterline the carve still reaches, and over how many
    // metres it fades in. Both are relative to the line -- see the note in
    // heightM, which is where getting this wrong cost the lakes their depth.
    float basinGateOver = 14.0f;
    float basinGateRamp = 14.0f;

    // -----------------------------------------------------------------------
    // NO PUDDLES: GROUND THAT DIPS UNDER THE LINE WITHOUT A BASIN IS FILLED IN.
    //
    // (user 2026-09-17: "can you prevent little puddles from forming? we want
    //  lakes more then ponds".)
    //
    // THERE ARE TWO WAYS TO BE WET IN THIS ENGINE AND ONLY ONE OF THEM IS A
    // LAKE. A basin is CARVED -- basinAt clears basinT, the hollow is cut to
    // basinBed, and what fills it is a body of water with a shape somebody
    // chose. The other way is an accident: the landform happens to wander a few
    // centimetres under a line that is drawn across the whole band, and the dip
    // fills. That second kind is every puddle in the world, it is arbitrarily
    // small, and no amount of moving the waterline removes it -- lowering the
    // line just moves the accident somewhere else, which is what the birch's
    // long sweep of waterlines was really discovering.
    //
    // THE BIRCH ALREADY HAD THE ANSWER, BY CONSTRUCTION AND BY LUCK. Its line
    // sits UNDER its field minimum, so there are no accidental dips there at
    // all -- see birchWater, whose note calls that out in as many words: "water
    // can only exist where a basin was actually CARVED". That property is worth
    // having on purpose and in every wood, rather than as a coincidence of one
    // band's numbers that any change to its relief would break. Giving the oak
    // real water (kOakDropM) put its line back INSIDE its field, which is why
    // the puddles turned up there first.
    //
    // SO THE GROUND IS RAISED, NOT THE WATER TEST GATED. A gate on the wet test
    // is what this engine tried before and removed, and its note says why: a
    // threshold leaves a DRY PIT below the waterline the moment it bites, which
    // is a cliff at the edge of the water. Lifting the ground has no such edge
    // -- the column stops being under the line because it is not under the line
    // any more, and everything downstream (the mesher, the sand, the walk, the
    // lake life) agrees without being told.
    //
    // AND IT IS EXACTLY THE COMPLEMENT OF THE CARVE, WHICH IS WHAT MAKES IT
    // SAFE. Note the sense of this field: in basinAt a LOW value is a bowl.
    // The carve runs on `b < basinT`, at full strength by basinT - 0.10 and at
    // nothing by basinT, so the fill is full at basinT and gone by
    // basinT - puddleFade. Wherever the carve does anything, this does
    // proportionally less; where the carve does nothing -- which is every
    // accidental dip in the world -- this does all of it.
    //
    // GETTING THAT SENSE BACKWARDS IS WORTH RECORDING, because the measurement
    // read as a plausible result rather than as a bug: the first cut filled
    // where b was LOW, which is inside the lakes, and it took the birch from
    // 6.52% wet to 1.54% and its largest body from 17,940 m2 to 5,132. That is
    // "fewer, smaller lakes" -- a believable answer to a request phrased as
    // "fewer puddles", arrived at by eating the lakes from the middle outwards.
    // The histogram is what showed it: the >10k bucket emptied in all three
    // woods at once, and nothing that only removed puddles could do that.
    // 0.50 RATHER THAN basinT ITSELF, and the sweep is why. At 0.40 the fill
    // reached full strength the instant the carve stopped, which chopped the
    // RIM off every real lake -- the pine wood went 4.41% wet to 1.89% and its
    // biggest body lost a fifth of its area. Held back to 0.50 the fill only
    // bites where the basin field is well clear of any bowl, and the measured
    // difference is entirely ponds:
    //
    //                 wet          bodies    >400 m2   biggest      in >2k m2
    //     pine    4.41 -> 2.57%    28 -> 17   23 -> 12  7452 -> 5856  69 -> 71%
    //     birch   6.52 -> 6.52%    15 -> 15   10 -> 10  17940 (same)  90 -> 90%
    //     oak     9.11 -> 8.14%    21 -> 16   15 -> 10  29400 (same)  89 -> 93%
    //
    // THE BIRCH IS THE CONTROL AND IT DOES NOT MOVE AT ALL. Its line already
    // sits under its field minimum, so it had no accidental water to lose --
    // which is exactly the prediction, and the reason to believe the other two
    // lost puddles rather than lake.
    float puddleT = 0.50f;     // where the fill is at full strength
    // -- OFF BY DEFAULT, AND IT IS THE FILL THAT MADE THE SAND BANKS -------
    //
    // (user 2026-09-17: "THERES no water in the oak forest ... increase the
    //  water surface area ... theres way too much sand".)
    //
    // THIS IS WHAT THE FILL ACTUALLY DOES, and it took a per-band shore census
    // to see it. It lifts sub-waterline ground to wl + VOXEL_M -- one voxel
    // ABOVE the line -- and the shore rule paints sand on everything up to
    // wl + sandRise. So every puddle it removed became a BEACH. It did not
    // make the water tidier, it converted it into the exact thing being
    // complained about, and it did so worst in the wood with the widest
    // shore:
    //
    //                  wet        dry sand per 100 wet
    //     pine    2.51 -> 4.41%      196 -> 68
    //     birch   6.52 -> 6.52%       58 -> 57
    //     oak     8.12 -> 9.11%       72 -> 53
    //
    // The birch is the control again and does not move, for the same reason it
    // did not move when the fill landed: its line sits under its field minimum
    // so it had no shallow water to convert.
    //
    // KEPT, NOT DELETED. The machinery is correct for what it was asked --
    // "lakes more then ponds" -- and it is the only thing in here that can
    // express that. What it cost was not visible from the pond census it was
    // tuned on, only from the shore census that came after. Set puddleFade
    // back to 0.20 to have it, and re-run the shore ratio before believing it
    // is free. The anti-puddle job is done by the WATERLINE now: a higher line
    // merges neighbouring bodies rather than erasing them, which is the same
    // goal reached from the other side and it adds surface area instead of
    // taking it away.
    float puddleFade = 0.0f;   // 0 = off; 0.20 was the tuned width when it ran
    // AND A CEILING ON HOW MUCH GROUND MAY BE INVENTED. Without one, a deep
    // hollow that happens to score no basin would be filled to the brim and
    // read as a plateau -- tens of metres of flat ground where there was a
    // valley. At 4.0 m a filled dip can never be deeper than a carved lake's
    // own bed (basinBed 4.5), so anything deeper than this keeps its water and
    // becomes a pond on its own merits.
    float puddleCapM = 4.0f;

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
    // 5.0 m, DOUBLED (user 2026-09-12: "flatten the banks out, make the steps
    // twice as flat"). Halving bankFlat alone did almost nothing -- 70.0% of
    // shore steps were level before and 70.3% after -- and the reason is worth
    // writing down, because it is not obvious and it wasted a sweep.
    //
    // THE BAND IS DEFINED BY VERTICAL EXTENT, so the shore always climbs about
    // bankRiseM across whatever plan distance the natural ground takes to rise
    // that far. k only decides how that climb is DISTRIBUTED inside the band,
    // never how much of it there is: the overall gradient is the terrain's,
    // whatever bankFlat says. Flattening reshapes, it does not reduce.
    //
    // What does reduce it is making the band TALLER THAN THE SAND. The visible
    // shore is sandRiseM (1.4 m), and with the band at 5 m that sits in the
    // ease's flat early portion instead of most of the way up it:
    //
    //     at d = 1.4 m    k = 0.692, rise 0.97 m   (flat 0.25, band 2.5)
    //                     k = 0.292, rise 0.41 m   (flat 0.125, band 5.0)
    //
    // 2.4x flatter over the sand, with the acceptance test still clean at
    // worst 3 voxels and 0 of 4+. The cost is a much larger flattened apron --
    // 165,649 beach columns against 93,559 -- whose outer part is grass rather
    // than sand, since the sand band did not move.
    float bankRiseM = 5.0f;
    // -----------------------------------------------------------------------
    // ...AND WHAT A SMALL BODY OF WATER GETS INSTEAD.
    //
    // bankRiseM was a single constant, so a 0.4 m2 puddle was given the same
    // 5 m flattening as a 4,784 m2 lake -- and because a shallow lake sits in
    // a WEAK basin, which is a gentle slope, that band is enormous in plan.
    // "Shallow water with large flat banks" is exactly that.
    //
    // The band now scales with the basin's carve strength, which is the same
    // number that decides how big the lake in it is. Measured at the shores of
    // five bodies:
    //
    //     4,784 m2  4.8 m deep   m = 0.682
    //       301 m2  1.7 m        m = 0.000
    //       221 m2  4.6 m        m = 0.000
    //        78 m2  0.3 m        m = 0.000
    //       0.4 m2  0.2 m        m = 0.386
    //
    // So the big lake keeps most of its apron and the small ones fall back to
    // this minimum. The one false positive is four columns across.
    //
    // TWO PROXIES THAT DO NOT WORK, measured, so they are not re-tried: the
    // same field at TWO octaves instead of four (the 0.4 m2 puddle scored
    // 0.973, higher than the big lake's 0.839), and water depth probed a fixed
    // 4 m out, which is zero for everything but the largest body and so
    // discriminates nothing among the rest.
    //
    // A SCALE, NOT A GATE, and the distinction is the whole history of this
    // file. The basin gate that used to cut the water and the bank produced
    // walls because it was a threshold; this varies continuously, and
    // bankShaped is continuous at d == rise for any rise (k reaches 1.0 there,
    // so the shaped height IS the natural one), so a smoothly varying band
    // cannot introduce a seam.
    float bankRiseMinM = 1.5f;
    // -----------------------------------------------------------------------
    // 0.125, HALVED FROM 0.25 (user 2026-09-12: "make the steps twice as
    // flat"). The shore now falls at an EIGHTH of the ground's own slope at
    // the water's edge rather than a quarter.
    //
    // THE COST IS WIDER TREADS, and it is worth stating because it is the
    // opposite of what "flatter" sounds like it should do. A gentler slope
    // crossed by a 10 cm quantisation puts more plan distance between one
    // contour and the next, so each terrace gets WIDER -- the beach reads as
    // fewer, larger steps rather than as a smooth ramp. bankGrainM is what
    // keeps their edges from running as clean contour lines; see it below.
    // 0.125 -> 0.070 (user 2026-09-16: "make the sandy banks flatter"). The
    // shore now falls at a fourteenth of the rate the ground around it does,
    // where it fell at an eighth.
    //
    // AND THE GRAIN GOES UP WITH IT, which is not optional -- see the note
    // below, which is about exactly this knob. A gentler slope crossed by a
    // 10 cm quantisation gives WIDER treads, so flattening the bank and
    // terracing it are the same action: at 0.070 each contour stretches almost
    // twice as far in plan as it did at 0.125. bankGrainM is the only thing
    // stopping that reading as a flight of boards, so it is raised in the same
    // breath rather than left to be discovered later.
    // -----------------------------------------------------------------------
    // 0.070 -> 0.50, AND IT MEANS SOMETHING DIFFERENT NOW.
    //
    // "when making the sandbanks flatter, you seem to only be making the bottom
    //  layers flat instead of all of the sand layers. make all of the sand bank
    //  steps more even."                                     -- user 2026-09-17
    //
    // THE OLD SHAPE COULD NOT DO WHAT THE OLD NUMBER PROMISED. `k` ramped from
    // bankFlat at the water to 1.0 at the top of the band, so the multiplier
    // was only ever bankFlat at ONE POINT -- the waterline. The effective
    // slope, d(out)/dd = bf + (1-bf)(S + uS'), runs 0.07 -> 1.23 -> 1.0 across
    // the band: flat at the water, and then STEEPER THAN THE NATURAL GROUND
    // through the middle, because a curve that compresses the bottom has to get
    // the height back somewhere. Measured as treads, which is what the eye
    // reads:
    //
    //     first step / last step, inside the sand
    //       pine 4.0x    birch 4.4x    oak 3.9x
    //
    // A beach whose first terrace is four times the depth of its last is what
    // "only the bottom layers are flat" is.
    //
    // WHAT IT IS NOW: a CONSTANT slope over the sand, then the catch-up above
    // it. Inside the flat core `k` does not vary at all, so out = d * k is
    // linear, every 10 cm contour is the same distance from the last, and the
    // ratio is 1 by construction rather than by tuning.
    //
    // WHY 0.50 AND NOT SOMETHING FLATTER. The core has to be wide enough in
    // NATURAL height to contain the whole sand band -- d0 = sandRise / slope --
    // and everything above it has to climb the rest of `rise` in what is left,
    // at an average of (1 - t0*slope)/(1 - t0). Flatter sand is a wider beach
    // AND a steeper strip of grass behind it, and there is no arrangement that
    // escapes that: over the band the mean slope is 1 whatever happens inside.
    // 0.50 puts the core at d0 = 2.8 m of a 5 m band and the catch-up at 1.64x
    // natural, against the 1.23x the OLD curve already reached in the middle of
    // the sand. So the steepest part of the shore barely moves; what changes is
    // that it is now in the grass above the beach rather than in the beach.
    float bankFlat = 0.50f;

    // HOW MUCH OF THE BAND THE FLAT CORE MAY TAKE, whatever the arithmetic
    // asks for. A puddle gets rise = bankRiseMinM = 1.5 m, which is shorter
    // than the sand band itself, so d0/rise comes out above 1 and the catch-up
    // would have no room at all. Capping it leaves a quarter of even the
    // smallest bank to climb in, at the cost of the top voxel or two of a
    // puddle's sand not being perfectly even -- which is the right thing to
    // give up, a puddle having about four steps in total.
    float bankCoreMax = 0.75f;

    // -----------------------------------------------------------------------
    // THE BROADLEAF BEACH, WHICH IS ONE SLOPE AND ONE WIDTH EVERYWHERE.
    //
    // (user 2026-09-17: "make the sand banks consistently flat all the way
    //  around. only apply this to birch and oak forests.")
    //
    // bankBroadRiseM is chosen to make the floor below BITE rather than to be
    // a band width in its own right: the derived slope is
    // sandRise / (bankCoreMax * rise), and at 8 m that is 0.233, comfortably
    // under bankBroadFlat -- so the clamp always takes the floor and the slope
    // is bankBroadFlat on every broadleaf shore whatever its basin. Lowering
    // it toward 5.3 m would let the derived value rise above the floor again
    // and the steep patches would come back, so it has margin on purpose.
    //
    // 0.35 IS FLATTER THAN THE SHORE HAS EVER BEEN, and deliberately: the sand
    // band is 1.4 m, so at this slope a beach is 4.0 m of natural rise wide
    // against the 2.55 m the old curve gave. Flatter and wider is the direction
    // every ask about these banks has pointed since 2026-09-12.
    // 8 -> 11 and 0.35 -> 0.25, a second pass on the same ask. With the slope
    // now a CONSTANT the spread that is left is the ground's own: measured
    // shore to shore, the gentlest tenth of broadleaf beaches run a 5 m tread
    // (a gradient near 0.02 -- a pan) and the steepest tenth about 0.35 m
    // (0.29). So nothing is steep in absolute terms any more; what is left is
    // that the steepest end is ten times the gentlest, and it is the steepest
    // end the eye picks out. Lowering the constant lowers exactly that end --
    // the pans cannot get much flatter, being already limited by the ground --
    // so it closes the gap from the only side that can move.
    //
    // THE RISE HAS TO GO UP WITH IT or the floor stops biting and the derived
    // slope comes back: sandRise / (coreMax * rise) must stay under
    // bankBroadFlat, which at 0.25 needs rise above 7.5 m. 11 keeps the margin
    // the 8 had.
    //
    // WHAT IT COSTS is the strip above the beach: the catch-up now climbs from
    // 1.4 m to 11 m over the natural rise left in the band, about 1.95x the
    // ground's own gradient. That is a firmer rise behind the sand, and it is
    // the same trade recorded over bankFlat -- over the whole band the mean
    // slope is 1 whatever happens inside it, so a flatter beach is a steeper
    // something-else, always.
    // 11 -> 6, FOLLOWING THE BAND DOWN. This only ever had to be big enough
    // that the derived slope stays UNDER bankBroadFlat so the floor takes it
    // (see above): at a 0.5 m band that needs 2.7 m, so 6 keeps a wide margin
    // while reshaping less than half the ground 11 did. A bank band is ground
    // the terrain no longer chooses the shape of, so it should be no larger
    // than the job.
    float bankBroadRiseM = 6.0f;
    float bankBroadFlat = 0.25f;

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
    // -----------------------------------------------------------------------
// -----------------------------------------------------------------------
    // 0.09, PUT BACK (user 2026-09-17: "the sand banks are jagged instead of
    // smooth now, make them smooth like they were").
    //
    // It was raised to 0.17 with the wavelength halved, to break up the clean
    // parallel contours that making the treads EVEN had produced -- and it did
    // break them up. It also made the beach jagged, which is the louder fault:
    // a grain of 0.17 is nearly two voxels of wobble on a surface quantised at
    // one, so the sand stopped reading as a smooth slope with a texture and
    // started reading as rubble.
    //
    // THE STAIRCASE IS THE ACCEPTED COST, then, and it is the right way round:
    // a smooth bank with visible contours is a beach, and a rough one is not a
    // beach at all. The contours are also much less of a problem than they were
    // when this was raised, because bankBroadFlat now holds the broadleaf banks
    // at a gentle CONSTANT slope -- the treads are wide and few rather than
    // stacked, which is what the grain was fighting.
    float bankGrainM = 0.09f;  // metres, plus and minus -- see bankFlat
    float bankGrainF = 1.10f;  // ~0.9 m wavelength

    // -----------------------------------------------------------------------
    // ...AND NONE OF IT ON THE BROADLEAF SHORE.
    //
    // "have the sand have a smooth step down effect instead of this random
    //  effect. this is how it used to be. one sand layer after the other."
    //                                                       -- user 2026-09-17
    //
    // THE GRAIN IS THE RANDOM EFFECT, and this is the third time it has been
    // asked about from a different direction. It exists to break the shore's
    // contour lines up so a flat bank does not read as a flight of dead-level
    // boards -- and on a shore whose slope VARIED that was right, because the
    // treads were all different depths anyway and the wobble hid the seam
    // between them.
    //
    // THE BROADLEAF BANK IS NOT THAT SHORE ANY MORE. bankBroadFlat holds it at
    // one constant gradient, so its contours are already even, concentric and
    // the same width -- a flight of steps is exactly what it IS, and what the
    // user is asking to be allowed to see. Displacing each contour by up to a
    // whole voxel does not make that smoother, it makes a clean step ragged,
    // which is the "random effect".
    //
    // ZERO ON THE BROADLEAF SIDE, faded on the same weight everything else
    // here uses so the pine's shore -- which still has a varying slope and
    // still wants its texture -- is untouched, and the two meet across the band
    // seam without a line.
    float bankBroadGrainM = 0.0f;

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
    // THE DEFAULT IS NAMED so it can be asserted on WITHOUT constructing a
    // VoxelTerrain. gpu/world.h guards "there is no water lid" with a
    // static_assert, and it used to spell that `VoxelTerrain{}.waveVoxMax == 0`
    // -- which quietly required this whole class to stay a LITERAL TYPE. The
    // first member with a non-trivial destructor (the DEM's grid) broke that,
    // and the error lands in world.h on a line nobody touched, naming neither
    // the member nor the file that added it. Asserting on the constant itself
    // checks exactly the same thing and cannot be broken by a member.
    static constexpr int kWaveVoxMaxDefault = 0;
    int waveVoxMax = kWaveVoxMaxDefault;  // 0 = the lake tops at the waterline

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
    // THAT IS NOW WHAT IT DOES, and the band is back on (user 2026-09-13:
    // "add foam around the edges like done in v1. white voxels around the
    // edges"). Two numbers changed and the diagnosis above is why:
    //
    //   foamDepthVox 3  water 30 cm deep or less is shore, and shore churns.
    //                   The host still decides WHICH columns -- that half was
    //                   never the problem. THE TAIL IS WHAT THIS SETS, NOT THE
    //                   MEDIAN, and that is the whole of the tuning. Measured
    //                   across 2.4 km, band width where a scanline crosses a
    //                   shore:
    //
    //                       1   med 0.1 m  p95 0.3 m  max  0.8 m   0.2% of water
    //                       2   med 0.3 m  p95 1.5 m  max  7.0 m   4.3%
    //                       3   med 0.5 m  p95 2.9 m  max 13.7 m   8.2%
    //                       4   med 0.8 m  p95 4.6 m  max 16.6 m  12.0%
    //                       6   med 1.3 m  p95 7.4 m  max 35.5 m  18.8%
    //
    //                   Every median in that table is plausible surf, which is
    //                   exactly why the median is the wrong thing to read. 4
    //                   shipped first and a rendered pine inlet settled it: a
    //                   gently shelving bay is shallow for FIFTEEN METRES, so
    //                   the whole of it went white and read as scum on a pond
    //                   rather than as a shoreline. Clumping it in the shader
    //                   helped and could not fix it -- half of a rash is still
    //                   a rash.
    //
    //                   3 cuts the worst case from 16.6 m to 13.7 and the
    //                   median to half a metre, which is a surf line you can
    //                   see without it reaching into open water. 2 also reads
    //                   correctly and is drier; 1 is a single voxel, which
    //                   nothing can see at all.
    //
    //                   A NOTE ON WHAT THE BAY ACTUALLY WAS, since it cost a
    //                   rebuild: the white speckle over that inlet is the sun
    //                   GLINT (kGlintStrength, v1's pixelGlisten), not this.
    //                   Toggling kWFFoam off moved 0.06% of the frame and
    //                   toggling kWFGlint moved 7%. Foam and glint are both
    //                   scattered white per-voxel cells and they are very easy
    //                   to confuse by eye -- use the [I] panel before tuning
    //                   either.
    //
    //   foamLiftVox  0  AND IT STAYS 0. The lift is what made it a wall. The
    //                   band is broken up in the SHADER now (foamMix in
    //                   Trace.cs.slang) where it costs no geometry and cannot
    //                   stand proud of anything; v1 needs the lift because its
    //                   band is already broken up before it is raised.
    //
    // mat::FOAM is still its own material, but only so the greedy merge splits
    // at the band's edge and the shader can find it with no neighbour test --
    // the shader treats it as water, not as a white solid.
    int foamDepthVox = 3;  // 0 = no foam band
    // ONE VOXEL, AND IT IS BROKEN UP BEFORE IT IS RAISED -- see foamCrest.
    // v1 lifts its foam for a reason worth restating: the band has to stand
    // proud BEFORE the surface is intersected, not after. Lifting a hit that
    // has already been found only moves that pixel's depth, and "the foam kept
    // the silhouette of the flat water because the pixels it should have grown
    // into were never tested against the water at all".
    // BACK TO 0 (user 2026-09-13: "revert the foam raised by one voxel
    // change"). It was raised on request the same day and taken down again;
    // foamCrest is left in place because it costs nothing while this is 0 and
    // it is the thing that made the lift survivable -- the band is broken up
    // in geometry before it is raised, so it never became the kerb the first
    // attempt was. Setting this to 1 is all it takes to have it back.
    int foamLiftVox = 0;   // ...and how far it would stand proud
    // What fraction of the shore is in a surf tongue, and how solid a tongue
    // is inside itself. 0.55 x 0.62 is about a third of the band raised, in
    // runs of a couple of metres.
    float foamCrestTongue = 0.55f;
    float foamCrestFill = 0.62f;

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
    // 1.4 -> 0.9 (user 2026-09-17: "theres way too much sand"). The pine's
    // beach was nearly three times the broadleaf's 0.5 and it is the widest
    // shore in the world, so it is where the complaint lands hardest. At the
    // new line this takes the pine from 53 dry sandy columns per 100 wet to
    // 43, and changes no other number in the survey -- sand cannot make a
    // column wet or dry, it only decides what the dry ones are made of.
    float sandRiseM = 0.9f;
    // -----------------------------------------------------------------------
    // 0.7 m IN THE BIRCH, HALF THE PINE'S (user 2026-09-13, "there's too much
    // empty sandy bank"). The wood already keeps its own waterline and its own
    // basin depth for the same underlying reason, and this is the third face of
    // it.
    //
    // A SAND BAND IS VERTICAL, SO ITS WIDTH IN PLAN IS THE GROUND'S SLOPE --
    // which is the whole of why one constant cannot serve both woods. The birch
    // relief is 22 m against the pine's 90, and at the lakes it is flatter
    // still, so 1.4 m of rise there covers several times the plan distance it
    // does in the pine. Measured over a 3,200 m square as sand columns per
    // WATER column -- the ratio, not the count, because sand scales with a
    // lake's perimeter and a bigger lake having more of it is correct:
    //
    //     sandRise    pine    birch
    //       1.4       0.45     0.73
    //       0.9       0.33     0.53
    //       0.7       0.28     0.46     <-- birch now reads like the pine
    //       0.5       0.24     0.39
    //
    // RAISING THE WATERLINE IS NOT THE LEVER FOR THIS, and the sweep recorded
    // at birchWater is the evidence: it bottoms out at 0.73 and then REVERSES,
    // because past that each further metre floods a flat apron and hands the
    // new shoreline a wider ring than it drowned.
    //
    // BLENDED ON birchMix, NOT SWITCHED, matching birchBasinBed -- though for a
    // weaker reason, since this picks a MATERIAL and a step in it is a ragged
    // line rather than a step in the ground. It is blended because the two
    // woods' shores meet along the band seam and a straight north-south edge is
    // the one shape nothing else in this terrain has.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // 0.7 -> 1.4 -> 0.5. THE BROADLEAF BAND, AND IT IS NOT THE PINE'S ANY MORE.
    //
    // (user 2026-09-17: "now theres way too much sandy banks".)
    //
    // TWO CHANGES COMPOUNDED AND THIS IS WHERE THEY MET. Unifying the two
    // broadleaf woods doubled the birch's band (0.7 -> 1.4), and flattening the
    // bank to bankBroadFlat spread whatever band there is over the plan
    // distance that gradient needs to climb it -- so the SAME 1.4 m covered
    // more than twice the ground it used to. MEASURED as the share of dry land
    // wearing sand:
    //
    //     pine 3.35%      birch 26.08%      oak 19.42%
    //
    // A quarter of the birch wood was beach. The band height is the right lever
    // rather than bankBroadFlat, and that is the whole point of separating
    // them: beach width is sandRise / slope, so lowering the band narrows the
    // shore WITHOUT making it steeper again -- which is what every earlier ask
    // about these banks was for. Raising the slope instead would have undone
    // them.
    //
    // IT IS NOT SHARED WITH THE PINE any more either. sandRiseAt lerps
    // sandRiseM -> this on the broadleaf weight, so the conifer shore keeps the
    // 1.4 m it was measured at and only the two woods that were asked about
    // move.
    float birchSandRiseM = 0.5f;  // the BROADLEAF band; the pine's is sandRiseM

    int bankRiseVox() const { return maxi(1, int(bankRiseM / VOXEL_M)); }
    // ASKED AT AN x, ALWAYS. There is deliberately no sandRiseVox() taking no
    // argument any more: it would answer with the pine's band everywhere, and a
    // caller in the birch wood would get a silently wrong shore rather than a
    // compile error.
    // ON THE BROADLEAF WEIGHT, NOT birchMix. birchMix is 0 through the whole
    // oak band, so this used to hand the oak the PINE's band -- which happened
    // to be the right number and was never a visible fault, but it was the same
    // two-wood assumption that drowned the oak in waterAt. The two constants
    // are equal today; the lerp is written on the weight that actually means
    // "broadleaf" so that stops being load-bearing.
    float sandRiseAt(float x) const {
        return lerpf(sandRiseM, birchSandRiseM, minf(1.0f, birchMix(x) + oakMix(x)));
    }
    int sandRiseVoxAt(float x) const { return maxi(1, int(sandRiseAt(x) / VOXEL_M)); }

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
    // `basin` is the carve's own field value at this column -- see
    // bankRiseMinM for what it is used for and for the two alternatives that
    // were measured and rejected.
    // -----------------------------------------------------------------------
    // `fine` IS HANDED IN RATHER THAN ALREADY ADDED, AND THAT IS THE SECOND
    // HALF OF "CONSISTENTLY FLAT ALL THE WAY AROUND".
    //
    // heightM used to do `h += fine` and then call this, so the fine octave was
    // INSIDE `d` -- and the bank multiplies d by a constant, so flattening the
    // shore flattened the fine octave with it. At bankBroadFlat that is the
    // detail's gradient cut to a third, and the note over `h += fine` says what
    // that detail is for: "the fine octave is most of what stands between a
    // hillside and a staircase ... its own gradient reaches 0.34, which holds
    // the steps to about 30 cm wherever the landform underneath has gone flat."
    // The flattening was quietly taking away the one thing holding the treads
    // down, exactly where the ground was already flat.
    //
    // WHAT THAT MEASURED AS. A tread is VOXEL_M / (k * groundSlope), so with
    // the detail scaled away a shore on flat ground had almost no gradient left
    // and its terraces ran metres deep, while a shore on a hillside kept
    // short ones. Shore to shore, p90 over p10 of the mean tread:
    //
    //     birch 16.9x    oak 15.1x     (pine, unflattened, 6.6x)
    //
    // That spread IS "some areas are steeper than other areas", and no constant
    // multiplier can close it: multiplying every slope by the same number
    // scales the variation, it does not remove it.
    //
    // ADDED AFTER THE FLATTENING, the detail keeps its full amplitude on the
    // beach whatever the bank does, so every shore has a real gradient and none
    // of them can spread into a pan. It stays SMOOTH -- an 11 m wavelength is a
    // gentle undulation, not the rubble that raising bankGrainM produced (user:
    // "the sand banks are jagged instead of smooth now").
    //
    // STILL CONTINUOUS AT THE TOP OF THE BAND: k reaches 1.0 there, so the
    // in-band value is wlm + rise + fine, and the out-of-band value is h + fine
    // with h = wlm + rise. The same number, with the detail on both sides.
    float bankShaped(float h, float wlm, float basin, float x, float z, float fine,
                     TerrainMemo &memo) const {
        if (wlm == kNoWater) return h + fine;
        // HOW BIG A BANK THIS BODY OF WATER HAS EARNED.
        // OVER 0.25 OF THE FIELD, NOT THE CARVE'S 0.10. The carve can afford a
        // tight ramp because it is applied to a height; this one changes the
        // WIDTH of the band, and a band whose edge moves quickly between
        // neighbours puts a step where the two edges do not line up. At 0.10
        // that measured as 6 steps of 4 voxels, against the rule that there be
        // none. A slower ramp also suits the question -- how big is this lake
        // is not a decision with a sharp boundary.
        const float bm =
            (basin >= basinT) ? 0.0f : sstep(minf(1.0f, (basinT - basin) / 0.25f));
        // -------------------------------------------------------------------
        // THE BROADLEAF BANDS GET A FIXED BAND, AND THAT IS THE WHOLE OF
        // "CONSISTENTLY FLAT ALL THE WAY AROUND".
        //
        // "theres still some areas where the sand banks are steeper then other
        //  areas. make the sand banks consistently flat all the way around.
        //  only apply this to birch and oak forests."       -- user 2026-09-17
        //
        // The steep patches are THIS LINE. `rise` is basin-driven -- a big lake
        // earns 5 m of bank and a puddle 1.5 -- and the core slope below is
        // derived from it, so a shore standing in thin basin got a band too
        // short to hold the sand, the slope clamped up toward 1.0, and that
        // stretch of shore came out at the hillside's own gradient with no
        // flattening at all. Two shores of the same lake can sit in different
        // basin values, so the steepness changed AROUND a single pond, which is
        // exactly what was reported.
        //
        // A FIXED RISE MAKES THE SLOPE A CONSTANT. With `rise` fixed at
        // bankBroadRiseM the derived slope is sandRise / (coreMax * rise) =
        // 1.4 / (0.75 * 8) = 0.233, which is under bankBroadFlat -- so the
        // FLOOR takes it and the answer is bankBroadFlat everywhere, for every
        // basin value, on every shore of every broadleaf lake. Not merely
        // similar: the same number.
        //
        // WHAT IT COSTS is the thing the basin ramp was protecting: a small
        // pond now gets the same generous bank a big lake does, so it sits in a
        // wider sandy bowl than its size argues for. That is the trade the ask
        // makes, and it is the right way round here -- "consistent" and
        // "scaled to the lake" cannot both be true, and only one of them was
        // asked for.
        //
        // BLENDED ON THE BROADLEAF WEIGHT, NOT SWITCHED. The pine keeps the
        // basin ramp, so the two meet across the band seam and a lake lying in
        // it must not find a step in its own shore -- the wall-in-the-lake
        // failure, at a fourth boundary. lerpf on the same weight the terrain
        // and the waterline use.
        const float broad = minf(1.0f, birchMix(x) + oakMix(x));
        const float rise = lerpf(bankRiseMinM + (bankRiseM - bankRiseMinM) * bm,
                                 bankBroadRiseM, broad);
        // THE `edge` FADE IS GONE WITH THE GATE IT EXISTED FOR. It ramped the
        // bank out over the basin mask so that gate's boundary was not a cliff
        // (6-voxel steps, 185 of them over 4 voxels). With no gate there is no
        // boundary to fade, and the band's own ease already carries the bank
        // into the hillside at the hillside's gradient.
        const float d = h - wlm;

        // -------------------------------------------------------------------
        // FLUSH WITH THE WATER, NOT ONE VOXEL UNDER IT.
        //
        // Ground sitting in the last voxel below the line is what made the
        // water stand proud of its own shore: the wet column beside it tops at
        // `line`, this one's surface is at `line - 1`, and 10 cm of water shows
        // above the sand. Measured: 2,538 of 4,687 lake edges.
        //
        // Snapping that band UP to the line removes it by construction. It is
        // 10 cm of terrain on a thin band, and it is a pure function of this
        // column -- no neighbour, so nothing for the gather and the point path
        // to disagree about.
        //
        // A RUN OF ATTEMPTS TO DO MORE THAN THIS WAS REVERTED (user 2026-09-13,
        // "can you just revert the previous shoreline changes"). They are worth
        // one line each so nobody tries them again believing they are new:
        // snapping the beach DOWN onto the line as well (flattens one column
        // and leaves the rest of the beach where it was); dropping kWetMinVox
        // to 1 so a one-voxel column is wet on its own (correct in itself, but
        // it put the swell's trough inside the bed and drew black voxels along
        // every sandy edge); and lifting the LINE a voxel above the still level
        // (flush, measured at 0.66% proud edges against 8.3%, but it floods a
        // little more ground -- 108 bodies became 121).
        // -------------------------------------------------------------------
        if (d < 0.0f && d >= -VOXEL_M) return wlm;

        if (d <= 0.0f || d > rise) return h + fine;
        const float u = d / rise;
        // -------------------------------------------------------------------
        // THE FLAT CORE: CONSTANT SLOPE OVER THE SAND, CATCH-UP ABOVE IT.
        //
        // t0 is where the sand ends measured in NATURAL height, as a fraction
        // of the band -- d0 = sandRise / bankFlat is the depth at which a
        // constant-slope bank has risen by one sand band. Below it the
        // multiplier does not move, so every contour inside the beach is the
        // same distance from the last. See bankFlat for the measurement this
        // replaced.
        //
        // STILL C1 AT THE JOIN, which is why the core can be a hard switch
        // rather than a third blend: smoothstep has zero derivative at both
        // ends, so S'(0) = 0 makes the slope continuous where the core stops,
        // and S'(1) = 0 makes it continuous where the bank meets the hillside.
        // A kink at either would be a contour line running dead straight along
        // the top of every beach in the world.
        //
        // STILL MONOTONIC. Inside the core out = d * bankFlat with bankFlat > 0;
        // above it `k` only rises while `d` rises, so the product does too.
        // THE CORE'S SLOPE IS DERIVED, NOT SET. First attempt used bankFlat
        // directly and made the ratio WORSE -- 3.9x to 6.0x in the oak -- for a
        // reason the constant cannot see: `rise` is not 5 m, it is between
        // bankRiseMinM and bankRiseM depending on how much basin this shore
        // stands in, and most shores are nearer the bottom of that. A 1.5 m
        // band has no room for a 2.8 m core, so the sand spilled into the
        // catch-up, which the core had just made SHORTER and therefore steeper.
        //
        // So the slope is whatever makes this band's sand fit its core:
        //   * never below bankFlat  -- do not over-flatten a shore with room,
        //   * never above 1.0       -- a bank steeper than the hill it is cut
        //                             into is a scarp, not a beach. A small
        //                             puddle simply gets no flattening, and
        //                             since k is then constant at 1.0 the whole
        //                             way, it is still perfectly EVEN, which is
        //                             what was actually asked for.
        // Continuous in `rise`, so neighbouring columns cannot disagree.
        const float sandM = sandRiseAt(x);
        // The FLOOR is what makes the broadleaf answer a constant -- see the
        // note over `rise` above, and bankBroadFlat for the number.
        const float slope =
            clampf(sandM / (bankCoreMax * rise), lerpf(bankFlat, bankBroadFlat, broad), 1.0f);
        const float t0 = minf(bankCoreMax, sandM / (slope * rise));
        const float v = (u <= t0) ? 0.0f : (u - t0) / (1.0f - t0);
        const float ease = v * v * (3.0f - 2.0f * v);
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
        const float k = slope + (1.0f - slope) * ease;
        // v4's `waterY + 1 + int(d * k)`, in the continuous form -- see the
        // one-voxel floor note above.
        // THE DETAIL AT FULL AMPLITUDE -- see the note on this function. The
        // one-voxel floor is applied over the top of it, so the detail can
        // never be what pushes a beach column under the line.
        const float flat = wlm + maxf(d * k + fine, VOXEL_M);

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
        // GRAIN ONLY WHERE THE SLOPE STILL VARIES -- see bankBroadGrainM. The
        // noise is skipped outright where the amplitude is zero, which is the
        // whole broadleaf shore: an fbm nobody scales by anything is a pure
        // cost, and this is the hot path for every column near water.
        const float grainM = lerpf(bankGrainM, bankBroadGrainM, broad);
        if (grainM <= 0.0f) return maxf(flat, wlm + VOXEL_M);
        const float g = (fbm(memo.bankGrain, x * bankGrainF, z * bankGrainF, 2) - 0.5f) * 2.0f;
        return maxf(flat + g * grainM * (1.0f - ease), wlm + VOXEL_M);
    }

    // -----------------------------------------------------------------------
    // THE WATERLINE SINKS INTO THE GROUND AT THE BAND SEAM. IT DOES NOT SWITCH
    // OFF, AND THAT IS THE WHOLE OF THIS FUNCTION'S HISTORY.
    //
    // It used to be `(birchMix(x) <= 0.001f) ? pineWater : birchWater`, with
    // birchWater = kNoWater. A hard threshold on x, so any lake reaching it
    // lost everything on the far side in a dead straight line WHATEVER the
    // ground there was doing. Measured over a 6 km sweep: 1,882 of 87,840
    // lake-edge faces ended with the ground across them still below the line,
    // the worst 95 voxels -- NINE AND A HALF METRES of water standing in open
    // air -- and they lined up on exactly three values of x.
    //
    // A BODY OF WATER CANNOT END AT A VERTICAL PLANE. It can only end where
    // the ground rises to meet it, or where the LINE falls to meet the ground.
    // The ground is not ours to raise -- a straight ridge along the seam would
    // be as artificial as the cut -- so the line falls instead, sinking below
    // the deepest ground as the seam approaches. The lake tapers to nothing:
    // depth goes to zero at the contact, which is what a shoreline IS.
    //
    // birchWater is still honoured if it is ever set to a real number; the
    // sinking only replaces the kNoWater case, which is the one that cut.
    // -----------------------------------------------------------------------
    float waterAt(float x) const {
        // -- A MEASURED WORLD HAS NO INVENTED SEA --------------------------
        //
        // (user 2026-09-18: "I would rather have larger terrain formations then
        // little small segments" + "getting these perfect lines in the terrain
        // generation", with photographs of a flat sheet of water standing over
        // real ground.)
        //
        // THIS IS A BAND CONSTANT AND IT DOES NOT KNOW WHAT A DEM IS. pineWater
        // is 36 world metres -- one number for a whole invented wood, which is
        // exactly right when the landform was invented to sit around it. Under a
        // DEM the datum puts the lowest measured ground at 20 m, so that same
        // constant is a sea covering everything under about 2,048 m above sea
        // level, and a good part of this window is under that.
        //
        // WHAT IT LOOKS LIKE IS BOTH COMPLAINTS AT ONCE. A level plane laid over
        // real relief cuts the valley floor into a scatter of small islands
        // wherever the ground crosses it -- the "little segments" -- and where
        // the DEM's own contours happen to run parallel it leaves long dead
        // straight waterlines, which is the "perfect lines". Neither is a bug in
        // the terrain: they are the shoreline of a sea that should not be there.
        //
        // THE MAPPED LAKES ARE THE WATER NOW. They come from the imagery, they
        // sit at the DEM's own level, and they are the whole of it -- see
        // mappedWater and lakeLineAt, which is asked before this is.
        if (dem_.ok()) return kNoWater;
        if (forced)
            return (biome == Biome::Birch) ? birchWater
                   : (biome == Biome::Oak) ? oakWater
                                           : pineWater;
        // ALL THREE WET: the line is the same weighted sum the terrain is, on
        // the same weights, so it cannot part company with the ground it has to
        // sit in. The two-wood version lerped on birchMix alone, which is 0
        // through the oak band -- see the note over oakWater for what that
        // would have done.
        if (birchWater != kNoWater) {
            float wp = 0.0f, wb = 0.0f, wo = 0.0f;
            woodMix(x, &wp, &wb, &wo);
            return wp * pineWater + wb * birchWater + wo * oakWater;
        }
        // BOTH WOODS WET: the line simply BLENDS between their two values on
        // the same weight the terrain blends on. No sink is needed and none
        // must be used -- there is no band where water stops existing, so
        // there is nothing to cut, and the old hard switch here is exactly
        // what sliced lakes in half along three lines of constant x.
        if (birchWater != kNoWater) return lerpf(pineWater, birchWater, birchMix(x));
        // Only the birch side dry: the line sinks under the ground instead of
        // switching off, so a lake at the seam tapers rather than being cut.
        return pineWater - (1.0f - waterSeamFade(x)) * waterSeamDropM;
    }

    // 1 well inside the pine wood, 0 at the seam and through the birch.
    float waterSeamFade(float x) const {
        if (forced) return (biome == Biome::Birch) ? 0.0f : 1.0f;
        // BOTH WOODS WET: there is no band where water stops existing, so
        // there is nothing to fade and nothing to protect. This fade exists
        // only for the dry-birch case -- left on with birchWater set it
        // multiplied the basin carve to ZERO across the whole birch band, so
        // its lakes were 1.0 m deep whatever basinBed said.
        if (birchWater != kNoWater) return 1.0f;
        const float inPine = -bandDist(x);   // metres into the pine, negative in birch
        return sstep(saturate((inPine - waterSeamEdgeM) / waterSeamFadeM));
    }
    // ------------------------------------ THE WATER SURFACE AT A *POINT*
    // waterAt(x) is the per-BAND procedural line -- one number for a whole
    // stripe of the world, and kNoWater entirely on the DEM path. Anything
    // that asks it about a mapped lake gets told there is no water there:
    // the camera never switched to its underwater look, and butterflies
    // happily cruised over the middle of Cheesman, because both asked
    // waterAt() and both were told the lake did not exist.
    //
    // This answers for a POINT, prefers the lake the imagery found, and falls
    // back to the band line everywhere else. See [[v2-waterY-is-a-global-lie]].
    float waterSurfaceAt(float x, float z) const {
        if (cover_.ok() && cover_.at(x, z) == CoverField::Water)
            return dem_.heightM(x, z);
        return waterAt(x);
    }

    int waterVoxAt(float x) const {
        const float w = waterAt(x);
        return (w == kNoWater) ? kNoWaterVox : int(w / VOXEL_M);
    }
    bool anyWater() const {
        return pineWater != kNoWater || birchWater != kNoWater || oakWater != kNoWater;
    }

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
    // THE OAK FOREST'S OWN GROUND -- v1's oakH, converted.
    //
    // (user 2026-09-16: "import the oak forest from v1 into v2".)
    //
    // v1's own words for it: "long wavelength, double-smoothstepped,
    // positive-only like duneH". The double smoothstep is the whole character
    // and it is why this is not just the birch field at another amplitude: one
    // sstep rounds the crests, and the second flattens the VALLEYS as well, so
    // what comes out is broad level floors separated by rounded rises rather
    // than continuous undulation. That is what an oak wood stands in.
    //
    // CONVERTED FROM VOXELS, which is the only real work in the port. v1 works
    // in 10 cm units throughout, so its constants are ten times these:
    //
    //     OAKF1 0.0018*0.85 /vox  ->  0.01530 /m       the two octaves
    //     OAKF2 0.0037*0.85 /vox  ->  0.03145 /m
    //     OAKY  20 + LIFT = 104 vox -> 10.4 m          the valley floor
    //     OAKHILL 58 vox            ->  5.8 m          crest above floor
    //     OAK_BOWL 10 vox           ->  1.0 m          how far under the floor
    //
    // -- and LIFT is 84 voxels, the height v1 floats its whole world above
    // bedrock, which is why the floor is 10.4 and not 2.
    //
    // SO THE OAK IS THE GENTLEST OF THE THREE, at 6.8 m of relief against the
    // birch's 22 and the pine's 70. That is v1's number and it is kept: three
    // woods want three characters, and "mountains, hills, lowland" is a better
    // spread than three variations on hills. It also puts the oak beside the
    // birch in the band order, where the two medians are within a couple of
    // metres and the seam between them is a walk rather than a hillside.
    // -----------------------------------------------------------------------
    // -- HOW BIG THE HILLS ARE, AND WHY THEY ARE NOT v1's NUMBERS ---------
    //
    // (user 2026-09-16: "make the oak forest terrain hilly and round similar
    // to v1".)
    //
    // v1's OWN VALUES WERE PORTED FIRST AND THEY ARE GENUINELY FLAT. The
    // conversion was checked twice and is right -- v1 says so itself, "a 2 m
    // person is 20 voxels and one METRE is 10" -- so OAKHILL 58 and OAK_BOWL 10
    // really are 5.8 m and 1.0 m, and v1's oak forest has 6.8 metres of relief
    // in total. Rendered at v2's scale that is not a wood with hills in it: it
    // is a plain, and flat enough that the 10 cm quantisation reads as contour
    // lines across the whole floor.
    //
    // SO THE SHAPE IS v1's AND THE SIZE IS NOT. What makes an oak wood look
    // like v1's is the DOUBLE SMOOTHSTEP -- one sstep rounds the crests, the
    // second flattens the valley floors, so the land is broad level bottoms
    // separated by rounded rises rather than continuous undulation. That is
    // kept exactly. What changes is the amplitude, which is the thing the eye
    // was actually judging:
    //
    //     pine    14 .. 84 m    70 m    ridged, a mountain range
    //     birch    2 .. 24 m    22 m    continuous rounded roll
    //     oak      3 .. 27 m    24 m    flat bottoms, round rises  <- here
    //     v1's oak 9.4 .. 16.2  6.8 m   the literal port
    //
    // AND THE WAVELENGTH IS STRETCHED to match. v1's octaves are 65 m and 32 m;
    // at four times the amplitude those give slopes you climb rather than walk,
    // so both are stretched by about 1.45 and the hills come out 95 m and 46 m
    // across. Broad is what "round" means at this height -- the same relief
    // over half the distance would be dunes.
    static constexpr float kOakF1 = 0.01055f;   // ~95 m hills
    static constexpr float kOakF2 = 0.02170f;   // ~46 m, v1's ratio kept
    // -----------------------------------------------------------------------
    // 3.0 -> 9.5, AND IT IS WHY THERE WERE FIFTY PUDDLES.
    //
    // (user 2026-09-17: "your creating multiple puddles, I would rather have
    //  1-2 larger bodies of water".)
    //
    // THE LAND ITSELF WAS UNDER THE WATERLINE. This floor is 3.0 and the bowl
    // term takes 2.5 more off a valley bottom, so the lowest natural ground sat
    // at 0.5 m against a line at 5.0 -- every flat bottom in the wood was wet
    // BEFORE any basin was carved. Water is a plain depth test (see
    // lakeLineAt, and do NOT re-add the basin gate -- it was measured
    // fragmenting one lake into 26 scraps), so the number of lakes was simply
    // the number of places the landform dipped: 51 in the oak, 59 in the birch,
    // the largest holding a fifth of the water.
    //
    // Lifting the floor clear of the line makes the un-carved wood DRY, and
    // then the only water is where basinAt digs a hollow -- which is what a
    // basin field is for and the shape it already has. Fewer, larger, and in
    // valleys rather than in every hollow.
    //
    // 9.5 - 2.5 = 7.0 m at the lowest, 2 m of clearance over the line. The
    // whole wood moves up 6.5 m with it; nothing reads that as absolute height
    // -- the waterline, the sand band and the bank are all measured FROM the
    // line, and the relief is untouched.
    // -- 8.0 -> 6.0, BECAUSE IT HAD DRAINED THE WOOD ---------------------
    //
    // (user 2026-09-17: "I dont see any water in the oak forest. match the
    //  same water properties as the pine forest. I just see sand banks with no
    //  water.")
    //
    // AND THAT IS EXACTLY WHAT THE NUMBERS SAID. A waterline is only
    // meaningful against the height DISTRIBUTION under it, and measured over
    // the three bands:
    //
    //     pine    water 34.00   p5 34.02    4.84% wet
    //     birch   water  5.00   p5  3.86    6.46% wet
    //     oak     water  5.00   p1  5.00    0.98% wet   <-- the report
    //
    // The pine's line sits at its FIFTH percentile. The oak's sat at its
    // FIRST -- the whole wood had been lifted clear of its own water, so the
    // only thing left was the sand band, which is painted from the line rather
    // than from the water and therefore still appeared. Sand banks with no
    // water in them, which is the report word for word.
    //
    // THE LIFT WAS DELIBERATE and its reasoning is below: a dry plain means
    // lakes only where basinAt digs, which is how you get few large bodies
    // instead of many puddles. That argument is sound and it overshot -- 6.5 m
    // of lift against a 2 m clearance target.
    //
    // LOWERED RATHER THAN THE LINE RAISED, and that is the one real choice
    // here. birchWater must equal oakWater (see the note over it -- they are
    // blended across the seam, and two different lines make a lake with a
    // sloping surface), so raising the line would have taken the birch from
    // 6.46% to about 12% and turned that wood into marsh to water this one.
    // This constant moves the oak alone.
    //
    // 6.0 is arithmetic, not a guess: the offset is purely additive, so the
    // band moves down 2.0 m with it and p5 lands at 4.87 -- just under the
    // 5.00 line, which is the pine's own relationship between the two.
    static constexpr float kOakFloorM = 8.0f;
    // -- ...AND THE OAK SITS LOWER IN ITS OWN BAND -----------------------
    //
    // LOWERING kOakFloorM MOVES BOTH WOODS, which is what the first attempt at
    // this did and why it is not the lever. The birch was given oakHeight when
    // the two were unified, so dropping the floor took the birch from 6.46% wet
    // to 11.61% -- drowning one wood to water the other.
    //
    // The BLEND is per-wood, though: heightM sums wBirch * oakHeight and
    // wOak * oakHeight separately, so handing the two calls different floors
    // moves the oak alone and still crosses the seam smoothly -- the seam is a
    // weighted sum of two continuous fields either way, so there is no step to
    // make. That is the whole reason this can be a constant rather than a
    // second waterline (birchWater must equal oakWater, or a lake at the seam
    // has a sloping surface).
    //
    // 3.0 m puts the oak's p5 at 3.87 against the birch's 3.86 -- the two woods
    // share a waterline AND now share a relationship to it.
    static constexpr float kOakDropM = 3.0f;
    static constexpr float kOakHillM = 24.0f;
    static constexpr float kOakBowlM = 2.5f;

    // `floorM` is where this field's lowest plain sits. The BIRCH passes
    // kOakFloorM and the OAK passes it less kOakDropM -- see the note there.
    static float oakHeight(float x, float z, TerrainMemo &memo, float floorM = kOakFloorM) {
        const float a = fbm(memo.oakA, x * kOakF1 + 91.7f, z * kOakF1 + 33.1f, 3);
        const float b = fbm(memo.oakB, x * kOakF2 + 47.3f, z * kOakF2 + 8.9f, 3);
        const float u = sstep(sstep(a * 0.82f + b * 0.18f));
        return floorM + kOakHillM * u - kOakBowlM * (1.0f - u) * (1.0f - u);
    }

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

        // ------------------------------------------------- REAL GROUND
        // WITH A DEM LOADED THE LANDFORM BELOW IS NOT ASKED AT ALL. Everything
        // after this point -- the per-wood amplitudes, the basin carve, the
        // bank, the puddle fill -- describes an INVENTED place, and a measured
        // one has no use for any of it. A carve that hollows a lake would cut a
        // hole in a real valley; a bank shaped for a procedural shoreline would
        // terrace a real one.
        //
        // THE FINE OCTAVE STAYS, and it is the only thing kept. The postings
        // are 10.29 m apart and the voxels are 0.1 m, so bilinear interpolation
        // lays down 103 voxels of perfectly flat ramp between every pair of
        // samples -- and the comment above says exactly what that looks like at
        // this scale: "a smooth slope terraces into wide flat plateaus". The
        // same +-0.6 m of noise that breaks the invented terrain up breaks up
        // the measured one, and it is far below the DEM's own vertical error,
        // so it costs no accuracy to add.
        if (dem_.ok()) {
            // ------------------------------- ROUGHNESS THE DATA CANNOT CARRY
            // `fine` above is NOT detail here, and that is why measured ground
            // reads as smooth dunes. Its wavelength is about 11 world metres;
            // at shrink 6 the DEM's own postings are 1.7 world metres apart, so
            // the "fine" octave is COARSER than the data it is meant to break
            // up, and all it does is add swells.
            //
            // This one is finer than the posting spacing on purpose -- roughly
            // a metre, four octaves -- so it puts texture BETWEEN the samples,
            // where bilinear interpolation would otherwise lay down a perfectly
            // flat ramp 17 voxels wide.
            //
            // THE AMPLITUDE IS DELIBERATELY SMALL. 3DEP's own vertical error is
            // metres; anything on that order would be inventing landform and
            // calling it measurement. A couple of decimetres is under the
            // error bar, invisible on a profile, and the difference between
            // ground that looks poured and ground that looks walked on.
            float grade = 0.0f;
            const float g = dem_.heightAndGrade(x, z, &grade);
            // ----------------------------------------------- A LAKE IS FLAT
            // AND THE NOISE MUST NOT TOUCH IT. 3DEP maps still water as a level
            // plane, so the DEM arrives perfectly flat here -- and then `fine`
            // (+-0.6 m) and the detail octave (+-0.22 m) were added on top of
            // it like any other ground, which is exactly the "water mounds"
            // that should not exist. Roughness belongs on ground, not on a
            // surface whose defining property is that it is level.
            //
            // THE BED IS CARVED, not painted. Returning mat::WATER as the top
            // material gave a water-coloured skin with earth immediately under
            // it -- water with no depth. The lake bottom drops away from the
            // shore instead, and lakeLineAt puts the surface back at the DEM's
            // own level, so the mesher fills bed-to-line with real water the
            // same way it does for the invented lakes.
            // waterHere, NOT at(). The class sampler jitters by up to a cell
            // for the renderer's sake, so a column in the middle of a lake
            // could take the dry branch and stand up out of the water --
            // measured at 2.42% of all mapped water before this, 94% of it
            // this sampler. See CoverField::waterHere; lakeLineAt and
            // topMaterial ask the same question through the same door, because
            // three functions disagreeing about one edge is what a wall in a
            // lake is made of.
            if (mappedWater(x, z)) {
                // shoreDistance is REAL metres off a baked, interpolated plane,
                // so this is smooth and costs one lookup. Converted to world
                // metres before it shapes the bed, or the lake is six times
                // deeper than it should be.
                // THE PRECISE FIELD WHILE IT LASTS, THEN THE WIDE ONE. shore_
                // saturates at 127 real metres (see CoverField::buildDeepField)
                // and 55% of an ocean is past that, so the shallow ramp keeps
                // its one-metre precision and the open water gets a distance
                // that actually varies.
                const float nearM = cover_.shoreDistance(x, z);
                const float shoreM =
                    nearM < 120.0f ? nearM : maxf(nearM, cover_.shoreDistanceFar(x, z));
                const float toShoreW = shoreM / dem_.shrink();
                // -- THE BED KEEPS GOING DOWN, IT DOES NOT STOP DEAD --------
                //
                // (user 2026-09-18: "the water has missing terrain on the
                // ocean floor".) IT WAS NOT MISSING. It was
                // `minf(kLakeDepthM, 0.45f * toShoreW)` -- a shore ramp and
                // then a HARD CLAMP -- so every column more than about eleven
                // world metres from any shore had exactly the same depth.
                // Measured on acadia: 68% of all wet columns sat at 5.00 m,
                // the bed was one dead-level plane across the whole bay, and
                // the water's own extinction leaves 10% of blue at that depth.
                // A featureless plane rendered at a tenth brightness is
                // indistinguishable from nothing being there, which is exactly
                // what it was reported as.
                //
                // That clamp is right for a LAKE, which is what it was written
                // for -- a pond has a middle and the middle is as deep as it
                // gets. An OCEAN has no middle inside the window, so the clamp
                // is the entire sea.
                //
                // So the ramp continues past it at a gentler grade, and the
                // shape comes from shoreDistance, which already knows where
                // the bays and the headlands are: a cove stays shallow, open
                // water falls away. SMOOTH, and deliberately no noise -- "we're
                // looking for smooth terrain without noise" (user, same day) is
                // about all terrain, and a sea bed is terrain.
                const float knee = kLakeDepthM / 0.45f;   // where the ramp used to stop
                const float depth =
                    toShoreW <= knee
                        ? 0.45f * toShoreW
                        : minf(kSeaDepthM, kLakeDepthM + kSeaGradeW * (toShoreW - knee));
                // -- AND THE BED TERRACES LIKE ANY OTHER SMOOTH RAMP --------
                //
                // This branch RETURNED here, so the anti-terracing below never
                // ran on a single wet column and every lake bed in the engine
                // kept its contour lines -- which is what an underwater shot is
                // mostly made of. The open-water grade is kSeaGradeW, 1.5 cm a
                // metre, so the treads are 6.7 m wide and lie in perfect rings
                // round the shore distance field.
                //
                // THE GRADE IS THE CARVE'S AND IS HANDED IN, and the FADE
                // comes off the depth. A carve's slope is CONSTANT within each
                // half of the ramp -- 0.45 inshore, kSeaGradeW out -- so a
                // weight derived from it would jump at the knee and again where
                // the depth clamps, and a jump in the break-up is itself a line
                // along a contour. Depth is smooth everywhere, so fading on it
                // cannot draw one: in over the first metre past the knee, out
                // over the last metre before the bed goes flat.
                //
                // The shallows are left alone deliberately. Inside the knee the
                // bed falls at 0.45, so the treads are 22 cm and there is
                // nothing to break; that strip is also where the sand band and
                // the foam are measured from.
                const float bedW = clampf(depth - kLakeDepthM, 0.0f, 1.0f) *
                                   clampf(kSeaDepthM - depth, 0.0f, 1.0f);
                return g - maxf(0.20f, depth) + bedW * terraceBreakM(x, z, kSeaGradeW);
            }
            // -- AND THE MEASURED GROUND IS SMOOTH -------------------------
            //
            // (user 2026-09-18: "can you work on noisy banks: make the banks
            // smooth. actually remove that noise from all terrain. we're
            // looking for smooth terrain without noise.")
            //
            // BOTH OCTAVES GO, AND THEY WERE THE WHOLE OF IT. `fine` is
            // +-0.6 world m -- six voxels of swell -- and `det` another +-0.22,
            // two more. On a slope they read as texture, which is what the
            // notes above were arguing for; on the flat sand of a bank they
            // read as exactly what was reported, a speckle of single voxels
            // standing proud of ground that the data says is level.
            //
            // THE ARGUMENT FOR KEEPING THEM WAS TERRACING, and at this scale it
            // does not hold. It was written for the invented landform, whose
            // samples are tens of metres apart; the DEM's postings are 10.29
            // real metres, which at shrink 6 is 1.7 world metres -- seventeen
            // voxels -- so the bilinear ramp between two samples is seventeen
            // voxels long, not a hundred. That is a slope, not a plateau.
            //
            // demDetailM SCALES BOTH so none of this is deleted: --dem-detail
            // 0.45 is the old ground exactly, and 0 -- the default now -- is
            // the measurement and nothing else.
            // -- AND THE SNOW LIES ON TOP OF ALL OF IT ---------------------
            // (user 2026-09-18: "make the snow on peaks ON the terrain as well
            // as the terrain itself".) It was a PAINT before -- topMaterial
            // returned a white id for the surface voxel and the ground was
            // exactly where it had always been. Snow that is on something has
            // to raise it, so the column grows by the depth and materialAt
            // fills what it grew by. Passing `g` keeps it one DEM lookup.
            const float snow = snowDepthAt(x, z, g);
            // ---------------------------- THE STEPS ARE WARPED, NOT SMOOTHED
            //
            // (user 2026-09-18, with a picture of stepped ground: "can you
            // build an ai to clean up abnormalities in the terrain like this:
            // it should be able to detect and fix the terrain artifacts".)
            //
            // THE TERRACES ARE NOT IN THE DATA AND NOT IN THE INTERPOLATION.
            // They are what happens when ANY smooth ramp is quantised onto a
            // 0.1 m voxel grid: a column's height changes a whole voxel at a
            // time, so a constant grade gives treads of constant width, every
            // one of them lying along a contour. Measured on the snowfield at
            // world (1050, 1275): a 1.83% grade, which is a step every 5.5
            // world metres -- and photographed there as a set of concentric
            // rings, because above the treeline nothing grows to break them.
            //
            // SMOOTHING THE INTERPOLANT DOES NOTHING, and that was not reasoned
            // but built: a smoothstep DemField::heightM shipped, the ground was
            // rendered through it, and the rings were unchanged. See the note
            // in dem.h. Any curve between the postings is quantised just the
            // same at the end.
            //
            // SO MOVE THE CONTOUR, DO NOT DITHER THE COLUMN. The argument, the
            // measurement that retired the white-noise dither that used to be
            // here, and the constants are all over terraceBreakM. The short
            // version: a dither's fray is the same fraction of a tread at every
            // grade, so it was never weaker on gentle ground -- it was only
            // switched OFF there, because nine metres of randomly flipped
            // columns is the speckle "remove that noise from all terrain" was
            // about. A smooth field half a voxel deep displaces the contour
            // instead, by amplitude/grade, and cannot speckle at any grade.
            if (demDetailM <= 0.0f) return g + snow + terraceBreakM(x, z, grade);
            const float det =
                (fbm(memo.detail, x * 0.90f + 17.3f, z * 0.90f + 41.7f, 4) - 0.5f) * demDetailM;
            return g + snow + fine * (demDetailM * (1.0f / 0.45f)) + det;
        }
        // -------------------------------------------------------------------

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
        // -------------------------------------------------------------------
        // THE BIRCH SIDE NO LONGER RETURNS HERE, AND THAT ONE LINE WAS BOTH OF
        // THE BIRCH WATER COMPLAINTS.
        //
        // It used to `return` its height straight out, and the note above said
        // exactly why that was safe: "The basin carve is skipped too -- it
        // exists to hollow out lakes, and this wood has no water in it yet."
        // That stopped being true the moment birchWater was set. The early
        // return was taking the CARVE and the BANK away from every pure-birch
        // column, so its water was uncarved natural dips -- measured at 1.0 m
        // deep across 45,000 columns, which is the "large flat banks and small
        // puddles" exactly -- and its shores were never shaped at all.
        //
        // Both woods now fall through to the same carve and the same bank. The
        // fine octave is added below for both, so it is not in the expression
        // here any more; it used to be, because this path ended at the return.
        // -------------------------------------------------------------------
        // -------------------------------------------------------------------
        // THE LANDFORM IS BLENDED HERE, BEFORE THE CARVE. THAT ORDER IS THE
        // WHOLE OF THIS BLOCK, AND GETTING IT WRONG BUILT A WALL THROUGH A LAKE
        // (user 2026-09-13, "getting a wall in the lake in the birch").
        //
        // It used to carve the PINE field and then lerp the carved result
        // toward a freshly evaluated, UNCARVED birch field -- except on columns
        // where mix >= 0.999, which took an early return with the full carve on
        // the birch field. So the carve's depth was multiplied by (1 - mix)
        // through the whole seam and then snapped back to full strength at one
        // value of mix. mix is a pure function of x, so that snap is a PLANE OF
        // CONSTANT x, and wherever a basin lay across it the ground fell off
        // the edge:
        //
        //     worst one-voxel step in x, 3.2 km square, 25.6 M columns
        //       was   7.31 m, on four lines of x, every one at bandDist 86.6
        //       now   the terrain's own gradient, nowhere in particular
        //
        // Disabling the carve (basinT = -1) took that 7.31 m to 0.21 m and the
        // lines vanished, which is what identified it: nothing else in this
        // field steps, and the step was never in the blend -- it was in WHAT
        // THE BLEND WAS APPLIED TO.
        //
        // CARVING THE BLENDED HEIGHT IS ALSO THE RIGHT ANSWER ON ITS OWN TERMS.
        // The carve asks "is this column low enough to hold water", and the
        // only height that can answer is the one the column actually has. A
        // seam column carved from the pine field alone was being asked about a
        // wood it is only fractionally in.
        //
        // COSTS THE SAME. A pure column still evaluates one field, a seam
        // column still evaluates both, and the birch's roll and swell keep
        // their own memos because only the seam asks for them alongside the
        // pine's. The warp is the same warp at the same frequency, so the
        // second warpedFbm walks straight into the memo the first one filled.
        //
        // THE SEAM ITSELF is ninety metres of blend between a wood whose median
        // floor is 48 m and one whose median is 13 -- a 35 m drop, so it is not
        // a detail, it is a hillside, and it wants to be walked down rather
        // than fallen off. sstep on both sides of birchWeight is what makes the
        // join C1: the gradient goes to zero at each end of the blend instead
        // of changing abruptly where the lerp starts and stops.
        // -------------------------------------------------------------------
        // -- A WEIGHTED SUM OF THREE FIELDS, WHICH IS WHAT THE LERP ALWAYS WAS
        //
        // (user 2026-09-16: "import the oak forest from v1 into v2".)
        //
        // The two-wood version was written as "the pine field, with the birch
        // lerped in", plus a fast path for pure birch. That is exactly
        // wPine*P + wBirch*B once you substitute wPine = 1 - wBirch -- so
        // writing it as the sum is not a rewrite of the maths, it is the same
        // number by a form that takes a third term.
        //
        // A PURE COLUMN IS BIT-FOR-BIT WHAT IT WAS. Pure pine evaluates
        // 1.0 * P and the old code evaluated P; pure birch likewise. Those are
        // the same float, not merely the same value, so every column away from
        // a seam is untouched.
        //
        // A SEAM COLUMN CAN DIFFER IN THE LAST BITS, and it is worth being
        // exact about that rather than claiming more: a + (b - a) * t and
        // (1 - t) * a + t * b are the same number in real arithmetic and round
        // differently in floating point. The disagreement is around a
        // micrometre on a field measured in tens of metres, which is four
        // orders of magnitude below the 10 cm quantisation every consumer sees
        // it through -- so it can only matter if a column sat exactly on a
        // voxel boundary, and then only by one voxel.
        //
        // EACH FIELD IS STILL ONLY EVALUATED WHERE IT WEIGHS ANYTHING. A pure
        // column does one field, a seam column does two, and no column ever
        // does three -- a seam is 90 m and a band is 800, so nothing is ever
        // within reach of two seams at once.
        //
        // AND IT IS STILL BLENDED BEFORE THE CARVE, which is the whole of the
        // note above this one: carving one wood's field and then lerping toward
        // another's uncarved one put a wall through a lake.
        float wPine = 0.0f, wBirch = 0.0f, wOak = 0.0f;
        woodMix(x, &wPine, &wBirch, &wOak);
        float h = 0.0f;
        if (wPine > 0.001f) {
            // ---------------------------------------------------------- pine
            // TWO OCTAVES EACH, AND NOTHING ABOVE THEM. See the note on this
            // function for what the missing ones were carrying and what taking
            // them out measured.
            const float roll =
                warpedFbm(memo.warpX, memo.warpZ, memo.roll, x * 0.0130f, z * 0.0130f, 1.5f, 2);
            const float swell = fbm(memo.swell, x * 0.0070f + 71.3f, z * 0.0070f + 29.7f, 2);
            h += wPine * (14.0f + roll * 48.0f + swell * 21.5f);
        }
        // ---------------------------------------------------------- birch
        // THE BIRCH IS THE OAK'S FIELD NOW, NOT ONE OF ITS OWN.
        //
        // "make the birch forest more hilly like the oak. the birch and oak
        //  should share the same terrain/water generation."  -- user 2026-09-17
        //
        // It used to be `2 + roll*15 + swell*7` over five roll octaves and
        // three swell. MEASURED against the oak over a 900 m square, and the
        // gap is not subtle -- it is the whole complaint:
        //
        //     birch  p5  6.26  med 12.54  p95 16.65   relief 10.39 m
        //     oak    p5  3.57  med 15.52  p95 24.32   relief 20.75 m
        //
        // Half the relief. Calling oakHeight is the literal reading of "share
        // the same terrain generation", and it is better than copying the
        // constants across: there is now ONE broadleaf landform and no second
        // place to edit when it is tuned.
        //
        // THE BIRCH|OAK SEAM DISAPPEARS BY CONSTRUCTION, which is the part
        // worth having. A seam column has wBirch > 0 and wOak > 0 and both
        // branches now call oakHeight(x, z, memo) -- the same function at the
        // same point, hitting the same memo, so the second call returns the
        // first one's value and the sum is (wBirch + wOak) * oakHeight, which
        // is exactly the pure-band height. No blend, so nothing to step.
        //
        // WHAT IS LOST: memo.birchRoll and memo.birchSwell are now unused by
        // this function. They are left in TerrainMemo rather than removed --
        // the struct is a cache of noise lattices, costs nothing unasked, and
        // the birch field is one line away if the two woods are ever split
        // again.
        if (wBirch > 0.001f) h += wBirch * oakHeight(x, z, memo);
        if (wOak > 0.001f) h += wOak * oakHeight(x, z, memo, kOakFloorM - kOakDropM);

        // THE CARVE IS A FUNCTION OF THE WATERLINE AND EXISTS FOR NOTHING
        // ELSE. It pulls low ground down toward the line so that a lake has a
        // hollow to sit in. In a band with no water there is no line to pull
        // toward, and leaving it in would cut dry pits into a wood -- which is
        // exactly what removing the water from the old v4 left behind until it
        // was chased down. So a dry band is not carved.
        const float wlm = waterAt(x);
        const float b = (wlm == kNoWater)
                            ? 1.0f
                            : basinAt(x, z, memo);
        if (b < basinT) {
            // FADED WITH THE WATERLINE AT THE SEAM. A basin carves to
            // `wlm - basinBed`, so a hollow inside the fade would follow the
            // sinking line down and keep its full depth the whole way -- a
            // 4.5 m lake lying at an angle. Multiplying the carve by the same
            // fade means there is simply no hollow there to fill.
            const float m =
                sstep(minf(1.0f, (basinT - b) / 0.10f)) * waterSeamFade(x);
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
            // How far this wood's basins are cut. birchMix is a pure function
            // of x with no noise in it, so asking again here costs nothing.
            // ON THE BROADLEAF WEIGHT, NOT birchMix -- the third place this same
            // two-wood assumption was hiding (waterAt and sandRiseAt were the
            // others). birchMix is 0 through the whole oak band, so the oak was
            // being carved to the PINE's bed and its lakes came out shallower
            // than the birch's for no reason anybody chose.
            const float bed =
                lerpf(basinBed, birchBasinBed, minf(1.0f, birchMix(x) + oakMix(x)));
            h -= m * lowGate * (h - (wlm - bed));
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
        // NOT ADDED HERE ANY MORE -- it is handed to bankShaped, which puts it
        // on AFTER the flattening. See the note on that function: adding it
        // first meant the shore's flattening also flattened the detail that
        // stops a beach terracing, and that was most of the shore-to-shore
        // variation in steepness.

        // ONE RETURN, FOR EVERY COLUMN IN THE WORLD. The wood-specific branch
        // is upstream, in the landform; from the carve down there is a single
        // path, so there is no threshold left for the carve or the bank to step
        // across. Both woods have water, both want a shore, and the shore is
        // shaped from the height the column actually has.
        return puddleFilled(bankShaped(h, wlm, b, x, z, fine, memo), wlm, b);
    }

    // -----------------------------------------------------------------------
    // RAISE AN ACCIDENTAL DIP BACK OVER THE LINE. See puddleT for why.
    //
    // `basin` is the carve's own field value at this column, already in hand --
    // this costs no noise evaluation anywhere in the world.
    //
    // ONE VOXEL CLEAR, not a hair: the wet test quantises to voxels, so a
    // column raised to the line itself lands in the same voxel as the line and
    // is still wet. Lifting by VOXEL_M is the smallest lift that is actually a
    // lift, and it is a tenth of what the eye can read on a 10 cm grid.
    // -----------------------------------------------------------------------
    float puddleFilled(float h, float wlm, float basin) const {
        if (wlm == kNoWater || puddleFade <= 0.0f) return h;
        const float below = wlm - h;
        if (below <= 0.0f) return h;   // dry ground is nobody's puddle
        // A LOW BASIN VALUE IS A BOWL -- see puddleT. So the fill grows with
        // `basin`, the opposite way round to the carve.
        const float g = sstep(saturate((basin - (puddleT - puddleFade)) / puddleFade));
        if (g <= 0.0f) return h;       // a real basin: leave the lake alone
        return h + g * (minf(below, puddleCapM) + VOXEL_M);
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
    // 0.307, v4's number, and it means something different from the 0.105 it
    // replaces. That one applied only to columns the surface mask had already
    // painted green -- about half of them -- so real coverage was near 0.05.
    // This applies to every dirt column, modulated by the patch field below,
    // and the blades are now the only grass there is.
    float grassDensity = 0.307f;

    // -----------------------------------------------------------------------
    // WHERE THE MEADOWS AND THE CLEARINGS ARE.
    //
    // A flat probability over the whole world gives grass with no structure in
    // it: statistically identical everywhere, so there is no glade to walk
    // into and no thicket to come out of. v4's fix, taken whole -- three
    // octaves at grassPatchM, sharpened by sstep so the field commits to one
    // side or the other instead of hovering in its middle, because a raw fbm
    // lives in the centre of its range and a sparse patch has to be an actual
    // clearing rather than a slightly thinner meadow.
    //
    // grassSparse AND grassFull ARE A PAIR: `sparse + full * 0.5 == 1` keeps
    // the mean of the modulated density at grassDensity, because the sharpened
    // field averages a half. Change one and the other has to move, or the
    // whole world gets denser while the knob that is supposed to say so has
    // not been touched.
    // -----------------------------------------------------------------------
    float grassPatchM = 24.0f;     // metres -- the size of a glade
    float grassPatchGain = 3.1f;   // how hard the field commits
    float grassSparse = 0.12f;     // a clearing: bare litter, a few blades
    float grassFull = 1.76f;       // a thicket
    float flowerChance = 0.035f;
    int grassMinRows = kGrassMinRows, grassMaxRows = kGrassMaxRows;
    // -----------------------------------------------------------------------
    // A FEW TALL STRANDS, AND NOTHING ELSE MOVES (user 2026-09-13, second
    // pass: "revert the grass changes. you seemed to just increase the density
    // of the grass. dont do that. just give me like 5-8 grass strands randomly
    // that reach 10-13 voxels tall").
    //
    // THE FIRST VERSION WAS A PATCH FIELD AND IT DID RAISE THE DENSITY -- it
    // blended the planting probability toward 0.85 inside a patch, so a tall
    // patch was also a THICK one. That is the half that was wrong: nothing
    // about how much grass there is was asked for.
    //
    // So the field is gone. What is left is a per-column draw, independent of
    // everything else: a blade that was going to be planted anyway is, rarely,
    // a tall one instead. Density is exactly what it was, the ordinary blades
    // are exactly what they were, and the tall strands stand alone in the sward
    // rather than in company -- which is what "5-8 randomly" describes.
    //
    // 0.0025 of BLADES, which is one tall strand per roughly 12 square metres
    // -- five to eight in the patch of floor in front of you and none of them
    // near another. Measured below in strandRows.
    // -----------------------------------------------------------------------
    // (The patch machinery this replaced is recorded in the git history; the
    // measurements that sized it are no longer true of anything.)
    // TALL GRASS PATCHES (user 2026-09-13: "add tall grass patches. something
    // like 10-13 tall grass strands").
    //
    // Not taller grass everywhere -- PATCHES, so the floor still reads as a
    // wood with rough corners in it rather than as a meadow. Its own field,
    // its own scale and its own phase, so a tall patch is not simply the
    // brightest part of the glade field wearing a different number.
    //
    // 10 TO 13 VOXELS, which is 1.0 to 1.3 m. That is over knee height on a
    // 1.8 m body and it is why this needed the packed word's last spare bit --
    // see STRAND_MAX_ROWS. Eight was a format limit, not a taste.
    //
    // ABOUT A TENTH OF THE FLOOR: measured 8.9% of the ground in the pine wood
    // and 9.6% in the birch, which is a patch you come across rather than a
    // meadow you are standing in.
    //
    // AND THEY ARE THICK. A patch of tall grass that is as sparse as the floor
    // around it reads as a few stray weeds; the thing being asked for is the
    // stand you have to wade through, so the density inside one goes to 0.85
    // whichever wood it is in. Both woods get them: the pine floor's are the
    // rough corners of a glade, the birch floor's are meadow.
    //
    // THE GAIN IS THE NUMBER THAT MATTERS, and the first attempt had it wrong.
    // At the glade field's own 3.1 the patches came out 1.9% of the ground and
    // -- worse -- the whole of one was in the BLEND, so the row histogram fell
    // away from the moment it got tall: 12507 columns at 10 rows against 1919
    // at 13. That is not a patch of tall grass, it is a smear that happens to
    // peak. A patch wants a flat CORE and a thin rim, which is a high gain.
    //
    // Measured over a 240 m square, 7.0 against 3.4 at the same threshold:
    //
    //     gain 3.4    1.9% of the ground    10:12507  11:8395  12:4840  13:1919
    //     gain 7.0    8.9%                  10:40080  11:35004 12:30149 13:23640
    //
    // and in the birch 9.6%, with 8 and 9 rows left as the rim -- 29432 and
    // 27818 columns against 44433 at ten.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // TALL GRASS COMES IN GROUPS (user 2026-09-13: "make the grass patches
    // together in groups instead of seperate tall grass strands").
    //
    // THE HEIGHT ONLY. This is the third shape this has taken and the middle
    // one is why the constraint is written down: the first version was a patch
    // field that blended the PLANTING PROBABILITY toward 0.85 inside a patch,
    // so a tall patch was also a thick one, and that was rejected in as many
    // words ("you seemed to just increase the density of the grass. dont do
    // that"). The second was a per-column draw with no field at all, which is
    // what "separate strands" describes.
    //
    // So: the field is back, and it touches the ROW BOUNDS AND NOTHING ELSE.
    // `p` is computed before it and never seen by it. A patch is a group of
    // tall blades standing in exactly the grass that was already going to be
    // there.
    //
    // BOTH BOUNDS MOVE TOGETHER, so a patch is 10 to 13 throughout rather than
    // tall blades scattered among 3-voxel stubs -- which would be the separate
    // strands again, wearing a field.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // A TUFT IS A SITE, NOT A THRESHOLD ON A NOISE FIELD.
    //
    // It was an fbm patch, and the thing that killed it is that a THRESHOLD
    // HAS NO SIZE. Measured over a 240 m square: the patches came out a median
    // 4.8 m across and a maximum of 25.7, so "however many strands are in one"
    // ranged from 0 to 137 with the same constant. You cannot ask a level set
    // for five to ten of anything.
    //
    // A site has a radius, so it has an area, so the count is arithmetic --
    // see the fill below. Same shape as the flower colonies next door and for
    // the same reason.
    // -----------------------------------------------------------------------
    // -- HALVED, AND THE COUNT WENT WITH IT -------------------------------
    //
    // "Reduce the tall grass patch in half while also keeping the density."
    // Those are two instructions and the second one is what makes the first
    // arithmetic rather than a guess: HALF AS WIDE is a QUARTER of the area, so
    // a tuft that holds the same grass per square metre holds a quarter as many
    // strands. 1.2-2.2 m -> 0.6-1.1 m and 60-120 strands -> 15-30.
    //
    // IT FALLS OUT EXACTLY, and that is worth seeing rather than trusting. The
    // per-column draw in strandRows is
    //
    //     chance = want / (columns in the disc x planting probability)
    //
    // and `columns` is pi r^2 / VOXEL_M^2. Quartering both `want` and r^2
    // leaves `chance` identical to the last bit, so the tufts are half the size
    // and every blade inside one is exactly as likely to be tall as it was.
    // Measured over a 400 m square: see the density figures in
    // tests/tall_grass_test.cpp, which compares strands-per-square-metre before
    // and after rather than strands per tuft.
    float tuftCellM = 13.0f;      // one candidate site per 13 m of lattice
    float tuftCoverage = 0.55f;   // ...and this fraction of them are real
    float tuftRadMinM = 0.6f;     // a tuft is 1.2 to 2.2 m across
    float tuftRadMaxM = 1.1f;
    // HOW MANY STRANDS STAND IN ONE, drawn per tuft (user 2026-09-13: "only
    // have 5-10 strands of grass in groups like this").
    //
    // A COUNT, NOT A PROBABILITY, and that is what makes it hold in both woods.
    // The birch floor grows blades on 62% of its columns against the pine's
    // 28%, so one fixed per-column chance would put twice as many strands in a
    // birch tuft as a pine one. The fill is divided by the local planting
    // probability instead, so the ARITHMETIC is "this many strands in this
    // area" and the wood cancels out.
    // 5-10, then 15-30, now 30-60 -- tripled and then DOUBLED (user
    // 2026-09-13: "double the density of the tall grass").
    //
    // THE TUFT DOES NOT GROW WITH IT, and that is the point of expressing this
    // as a count rather than a probability: the radius is untouched, so the
    // same patch of ground holds twice as many tall blades. It fills in rather
    // than spreading out, which is what DENSITY means.
    //
    // THE CEILING IS REAL AND IT IS CLOSER NOW. The draw runs after the
    // planting draw and only changes the HEIGHT of blades that were going to be
    // there, so a tuft cannot hold more tall strands than it holds blades:
    //
    //     chance = want / (columns in the disc x planting probability)
    //
    // and that is clamped to 1. In the PINE wood, where only 27.8% of columns
    // carry a blade, the smallest tuft (1.2 m radius, ~450 columns) has about
    // 126 blades in it -- so 60 is a chance of 0.48 and the clamp still does
    // not fire. Doubling again WOULD hit it, and the symptom would be quiet:
    // the count would simply stop rising in the pine while the birch kept going.
    // QUARTERED WITH THE RADIUS, which is what keeps the density -- see the
    // note over tuftRadMinM. It was 60-120 over a disc four times this area.
    float tuftStrandsMin = 15.0f, tuftStrandsMax = 30.0f;
    // -----------------------------------------------------------------------
    // A GROUP IS A HANDFUL OF STRANDS, NOT A FIELD OF THEM (user 2026-09-13:
    // "only have 5-10 strands of grass in groups like this").
    //
    // The patch field says WHERE a group is. It does not say that everything
    // inside it is tall -- a 11 m patch holds about 2,600 blades in the pine
    // wood, and turning all of them up is a meadow of reeds rather than a
    // clump you come across.
    //
    // So a second, sparse draw inside the patch picks the few that stand. The
    // chance is per BLADE and per patch-strength, so the tall ones thin out
    // toward a patch's edge exactly as its own falloff does, and a group reads
    // as a tuft rather than as a disc with a hard rim.
    //
    // -----------------------------------------------------------------------
    // 15 TO 20 VOXELS, which is 1.5 to 2.0 m -- head height on a 2.0 m eye.
    // This is what needed the ramp's resolution spending; see the long note
    // over STRAND_MAX_ROWS.
    // -----------------------------------------------------------------------
    int tallGrassMinRows = 15, tallGrassMaxRows = 20;
    // THE BIRCH FLOOR IS A MEADOW, not a wood with glades in it: its density is
    // flat, with no patch field and so no clearings. That much is unchanged and
    // was asked for directly.
    //
    // IT IS NO LONGER THE TALLER OF THE TWO, and the sentence that used to end
    // this note said it was ("its blades stand a voxel taller"). It now stands
    // SHORTER than the pine floor -- see birchGrassMinRows below, which is the
    // 2026-09-14 ask -- so the woods differ in density and in height, just not
    // in the direction they first did.
    // TWICE AS SPARSE, TWICE (user 2026-09-13: "make the grass in the birch
    // forest twice as sparse", and again 2026-09-14: "make the grass 2x more
    // sparse"). 0.62 -> 0.31 -> 0.155, which is the planting PROBABILITY per
    // column and therefore exactly half the blades over the same ground each
    // time.
    //
    // THE TALL-GRASS TUFTS DO NOT THIN WITH IT, and that is on purpose: their
    // fill is expressed as a COUNT and divided by the local planting
    // probability (see tuftStrandsMin), so the arithmetic is "this many strands
    // in this area" and halving the floor cancels out of it. A tuft in the
    // birch wood holds the same number of tall blades it did; what changed is
    // the carpet between the tufts, which is what was asked for.
    //
    // THE ONE PLACE THAT CANCELLATION RUNS OUT is a tuft whose target count is
    // more strands than the thinned floor has columns to put them on: `chance`
    // clamps at 1 and the tuft is then as full as the ground allows. Only the
    // smallest, fullest tufts can reach it: a 0.6 m disc is 113 columns, so a
    // target of 30 strands wants a chance of 30 / (113 x 0.155) = 1.71 and gets
    // the 17 the floor can carry. MEASURED over 300 m of birch, the strands per
    // tuft moved 21.0 -> 21.4, so nothing in the shipped spread reaches it.
    float birchGrassDensity = 0.155f;
    // ...and the oak's. See the note at its use in strandRows.
    //
    // 0.40 -> 0.20 (user 2026-09-17: "make the grass strands twice as sparse in
    // the oak"). Still the thickest floor in the world -- the birch's uniform
    // sward is 0.155 -- but the ground between the blades shows again, which at
    // 0.40 it essentially did not.
    float oakGrassDensity = 0.20f;
    // TWO VOXELS SHORTER ON AVERAGE (user 2026-09-14: "decrease the grass in
    // the birch forest by 2 voxels on average"), and that is why there is a
    // birch MIN as well as a birch max now.
    //
    // The height is drawn uniformly over loRows..hiRows, so the mean is the
    // midpoint of the pair and moving the pair down by two moves the mean down
    // by exactly two: 3..7 (mean 5) becomes 1..5 (mean 3). Shortening by the
    // top alone would have had to reach 3..3 to get there, which is a lawn --
    // the SPREAD is the character of the sward and it is preserved.
    //
    // THE PINE SIDE IS UNTOUCHED, which is the whole reason the floor of the
    // range is now lerped rather than flat: grassMinRows is the pine's (and
    // --grass-min's), and the crossfade carries the birch's own value in over
    // birchMix exactly as the ceiling already did.
    int birchGrassMinRows = 1;
    int birchGrassMaxRows = 5;
    uint32_t strandSeed = 20260904u;

    // HOW FAR DOWN THE STONE GOES BEFORE THE BEDROCK STARTS, in voxels, from
    // each column's own surface. 100 voxels is 10 m at VOXEL_M -- deep enough
    // that digging through it is an undertaking, shallow enough to be reachable
    // at all. If you meant a hundred METRES, this is the one number to change.
    int kBedrockVox = 100;

    // Voxels of loose soil between the surface and the rock -- see crustVox.
    // The old emit loop had this as a literal 3; two to six reads as a bank
    // that thins and thickens rather than as a stripe ruled along the hill.
    // -- HOW DEEP THE DIRT GOES (user 2026-09-14: "make dirt 5 voxels
    //    deeper") -------------------------------------------------------
    //
    // 2..6 became 7..11. BOTH ENDS, so the variation the lattice above spreads
    // over a hillside is untouched -- raising only the ceiling would have made
    // the thin patches thin against a deeper average, which reads as the dirt
    // getting patchier rather than deeper.
    //
    // THE HOE CARES, and it is why this is worth a note. A till takes the
    // surface voxel and turns the one under it; on a two-voxel crust that is
    // most of the dirt there was, and a second bite anywhere near it would have
    // been into stone. Seven gives a seed bed something to be a bed in.
    int crustMin = 7, crustMax = 11;
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
        // THE HIGH GROUND IS WHITE WITH OR WITHOUT A PHOTOGRAPH. snowAt needs
        // only the DEM; the cover can pull the line down where it saw snow, and
        // has nothing to say where it was not loaded.
        if (!cover_.ok() && snowAt(i, j)) return snowShade(i, j);
        // ------------------------------------------- THE PHOTOGRAPH FIRST
        // BEFORE THE SHORE BAND, AND THAT ORDER IS THE WHOLE FIX. This used to
        // sit below the sand branches, so every column near the waterline was
        // claimed by mat::SAND before the imagery was ever asked -- which is
        // why the ground came out one flat pale wash with a correct-looking
        // ramp loaded and doing nothing. The imagery KNOWS where the sand is;
        // the procedural band is a guess for a world that had no photograph.
        //
        // A LAKE IS WHERE THE PICTURE SAYS ONE IS. The DEM already holds its
        // surface flat and at the right height -- 3DEP maps still water as a
        // level plane -- so the ground needs no carving, only the right
        // material laid on top of it.
        if (cover_.ok()) {
            const float wxm = wx(i), wzm = wx(j);
            // Water is NOT returned here any more -- see heightM. This is the
            // lake BED now, and the mesher fills the column above it from
            // lakeLineAt. Painting mat::WATER on the top voxel is what made the
            // lake a skin with no depth.
            //
            // THE BED IS ASKED THROUGH waterHere, the same predicate that
            // carved it, so the silt cannot end one cell short of the water or
            // one cell into the bank.
            if (mappedWater(wxm, wzm)) return mat::SILT;
            // -- AND ABOVE EVERYTHING ELSE, THE SNOW ----------------------
            // After the water and before the ground, which is the only order
            // that works: a lake does not freeze over because it is high, and
            // bare rock at 4,000 m is under snow whatever colour the imagery
            // sampled off it. See snowAt.
            if (snowAt(i, j)) return snowShade(i, j);
            // ...AND THE GROUND ITSELF IS ONLY THE PICTURE'S WHERE THE PICTURE
            // IS TRUSTED FOR IT -- see coverGround. The water above this line
            // is believed either way; what is gated here is the imagery's
            // opinion that a column is bare.
            const uint8_t cc = coverGround ? cover_.at(wxm, wzm) : uint8_t(CoverField::Forest);
            if (cc == CoverField::Rock || cc == CoverField::Snow || cc == CoverField::Water) {
                const int ci = mini(int(mat::GROUND_COUNT) - 1, cover_.colourIndex(wxm, wzm));
                // ------------------------- A WATER COLOUR IS NOT A GROUND ONE
                // ("can you investigate this blue terrain near water... could
                // you instead turn it into a sandy color which already exists."
                // user 2026-09-18, with a photograph of it.)
                //
                // The ramp is spread by luminance percentile over the window's
                // own pixels and nobody kept water out of that population, so
                // rmnp50's darkest entry is #384848 -- a teal off the lake --
                // and it painted every flat beside the water blue-grey.
                //
                // SAND IS THE RIGHT ANSWER AND NOT JUST A NICER ONE. What is
                // actually under those pixels is wet shore: the strip the water
                // has been over recently, which the photograph sees dark and
                // blue for exactly that reason. mat::SAND is already what this
                // engine calls that, already in the palette, and already what
                // the bank a few metres up is wearing.
                //
                // Ramp entries are fixed at load, so this is a comparison
                // against a small table and not a colour decision per column.
                // ...BUT ONLY WHERE A WET SHORE IS POSSIBLE. Above the
                // timberline there is no beach: a water-coloured pixel up there
                // is shadowed rock or old snow in a north-facing couloir, and
                // painting it beach sand put 50,125 columns of it on the alpine
                // ridges. The next entry up the ramp is the darkest one that is
                // actually ground, which is what that rock is.
                if (cover_.rampIsWater(ci)) {
                    const float asl = dem_.worldToAsl(float(h) * VOXEL_M);
                    if (asl < kTimberlineAslM) return mat::SAND;
                    return uint8_t(mat::GROUND_0 + mini(int(mat::GROUND_COUNT) - 1, ci + 1));
                }
                return uint8_t(mat::GROUND_0 + ci);
            }
        }
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
        // THE BIRCH FLOOR IS DIRT TOO. It used to return mat::GRASS_0 here --
        // one flat green over the whole wood, dithered against a column hash
        // so the seam with the pine's needle floor was not a straight line.
        //
        // That was the same mistake the pine side had: a green PAINT standing
        // in for grass. The floor is soil and litter on both sides now and the
        // grass is only the blades, so there is no paint to dither and no seam
        // to hide -- the two woods differ by what GROWS on them, which is what
        // told them apart in the first place.
        //
        // The blades on this side are denser and taller: see strandRows.

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
        if (h <= wl + sandRiseVoxAt(wx(i))) return mat::SAND;  // the shore band

        if (slope >= kRockSlope) return mat::ROCK;  // too steep to hold soil
        // alpine, and no imagery this trusts for the ground -- see coverGround
        if (aboveTimberlineVox(h) && (!cover_.ok() || !coverGround)) return mat::ROCK;

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
        // THE FLOOR IS NEVER GREEN. THE WHOLE TERRAIN IS DIRT, AND THE GRASS
        // STANDS ON IT -- which is how v4 does it.
        //
        // What was here: a mask painted the SURFACE mat::GRASS_0 over about
        // half the world, and blades were then allowed only on those green
        // columns. So "grass" was two things at once -- a green paint and a
        // scattering of blades -- and the paint did most of the work. Ground
        // seen between blades was green, so there was never any floor visible,
        // and density could not vary without the paint's edge showing as a
        // colour boundary.
        //
        // Now the floor is soil or needle litter everywhere and the grass is
        // ONLY the blades. Thinning them reveals more litter; it does not
        // change what the litter is. v4's note puts it exactly: "the dirt was
        // always under the grass -- there is simply more of it to see now."
        //
        // The canopy still says WHICH brown -- needle litter under a thick
        // stand, soil in the open -- which is all that rule was ever for.
        // -- ...EXCEPT UNDER THE OAKS, WHERE IT IS GREEN -----------------
        //
        // (user 2026-09-16: "In the oak forest, make the dirt, grass.")
        //
        // THIS IS A DELIBERATE EXCEPTION TO THE RULE ABOVE, which says the
        // floor is never green and means it. The argument there is sound and
        // still holds for the other two woods: a green PAINT standing in for
        // grass does the blades' job for them, hides the floor completely, and
        // turns any change in blade density into a visible colour edge.
        //
        // AN OAK WOOD IS THE CASE THAT ARGUMENT DOES NOT COVER, and v1 says so
        // itself -- it has a whole material for this, OAKMOSS, and lays it
        // under the oaks and nowhere else. A closed broadleaf canopy shades out
        // the litter and what grows under it is moss and sward, not needles
        // over bare soil. The blades still stand on top and still carry the
        // detail; what changes is what shows BETWEEN them.
        //
        // BGRASS_0 RATHER THAN A RAMP OF ITS OWN, for two reasons and the
        // second is the binding one:
        //
        //   * it is already the broadleaf ramp in fact. deriveGroundFromTrees
        //     splits foliage at birchStart_ and runs to pineEnd_, which is
        //     marked AFTER loadPines returns -- and the oaks load inside it,
        //     after the birches. So any green an oak model mints lands in
        //     BGRASS by construction. It mints FEW, because the oak set folds
        //     at tolerance 30 and mostly shares greens the birches already own,
        //     so this ramp is birch-dominated with the oak's own shades in it
        //     -- which is the right colour for both woods and is why they can
        //     share one.
        //   * the palette has THREE entries free and a ramp is SIX. There is no
        //     third ramp to be had; see the note over mat::GRASS_0.
        //
        // DITHERED ON THE COLUMN HASH, not switched at oakMix = 0.5. A hard
        // test would draw a straight north-south line where the green meets the
        // soil -- the one shape nothing else in this terrain has -- and it is
        // the same trick, for the same reason, that bladeMaterial uses.
        if (oakMix(x) > hashUnit(0x6A1Cu, hashU32(uint32_t(i), uint32_t(j))))
            return mat::BGRASS_0;

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
    // -----------------------------------------------------------------------
    // ONE LAKE, NOT FIFTY PUDDLES.
    //
    // "your creating multiple puddles, I would rather have 1-2 larger bodies of
    //  water"                                              -- user 2026-09-17
    //
    // 0.0160 AND FOUR OCTAVES IS A FIELD FULL OF SMALL DIPS. At that frequency
    // a basin is about sixty metres across, and the three octaves under it add
    // wiggle at 30, 15 and 8 m -- so a hollow that would have been one lake is
    // broken into a handful of separate ones by detail far smaller than the
    // thing being described. MEASURED over a 680 x 1200 m square per band:
    //
    //     pine 35 bodies   birch 59   oak 51,  largest only ~20% of the water
    //
    // AND IT IS ALSO THE SAND COMPLAINT. A beach is SHORELINE, and shoreline
    // goes with the COUNT: fifty puddles have several times the edge of one
    // lake holding the same water, so cutting the count cuts the sand without
    // touching the sand band at all -- which is the lever that does not make
    // the shore steeper again.
    //
    // 0.0042 IS A ~240 m BASIN, and TWO octaves: enough to keep the outline
    // irregular, not enough to carve an island out of the middle of it. The
    // seed offsets are unchanged so the basins stay where they were, only
    // larger and joined up.
    static constexpr float kBasinF = 0.0020f;
    static constexpr int kBasinOct = 2;

    float basinAt(float x, float z, TerrainMemo &memo) const {
        return fbm(memo.basin, x * kBasinF + 311.7f, z * kBasinF + 157.3f, kBasinOct);
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
    // -----------------------------------------------------------------------
    // IS THERE A LAKE HERE? THE ONE DOOR, AND EVERY CALLER USES IT.
    //
    // heightM carves the bed, lakeLineAt puts the surface back over it, and
    // topMaterial lays the silt. Three functions, one edge -- and when they
    // each asked the cover separately through the JITTERED sampler they got
    // three different answers, so a column could have a bed and no water, or
    // water and no bed. Measured before this: 35,495 columns of mapped water
    // that were dry land, 20,489 of them standing above the surface, worst
    // 22.7 m up. It reads like a wall in the middle of a lake, because that is
    // what it is.
    //
    // TWO TESTS, AND THE SECOND IS THE DEFINITION OF A LAKE.
    //
    //   1. The imagery says water -- through CoverField::waterHere, which is
    //      the baked shore plane rather than the class, so the edge is a smooth
    //      contour instead of a jittered fringe or a square staircase.
    //
    //   2. THE GROUND UNDER IT IS LEVEL. The classifier reads deep shadow and
    //      wet rock as water, and where it does, this used to carve a flat lake
    //      into a mountainside: 5.78% of all the water in rmnp50 stood on
    //      ground steeper than 10%, and the worst of it on a 212% grade a
    //      kilometre from Longs Peak. Water finds its level -- ground that does
    //      not have one is not holding any.
    //
    // IT MEASURES THE WATER, NOT THE BANK, and the first version did not. A
    // grade taken across the column's four neighbours straddles the shoreline
    // on every edge cell and comes back with the BANK's slope, which on a
    // reservoir is near vertical -- so it refused a three-metre rim right round
    // every real lake and left a bathtub ring of dry ground at the waterline.
    // 1.98% of Granby went that way. Only neighbours that are themselves water
    // are measured, so a lake with a cliff on one side still reads level, which
    // is correct: the cliff is not the water.
    //
    // A cell with NO water beside it is refused outright. That is a single
    // stray classification, and one cell of lake is not a lake.
    //
    // 20% IS MEASURED, NOT PICKED. Over the whole window 71% of water sits on
    // under a 2% grade, which is what a lake surface reads as; the tail above
    // 20% was 2.23%, and it was shadow and wet rock on mountainsides.
    //
    // THE COST IS FOUR SHORE READS AND UP TO FOUR DEM READS, on a water column
    // and nowhere else. Both are bilinear reads of arrays in memory, and the
    // land branch this replaces spends two fbm octave stacks. Water is under
    // 2% of the window's columns.
    // -----------------------------------------------------------------------
    static constexpr float kLakeGradeMax = 0.20f;
    // How far inside the water a column has to be before the grade test stops
    // being asked, in REAL metres. See the note in mappedWater.
    // 6 m, AND THIS ONE NUMBER IS A TRADE THAT CANNOT BE WON HERE.
    //
    // The grade test is a poor witness near a bank -- the bed is sloping up to
    // meet the shore by definition -- and the imagery's water mask includes the
    // wet shore, so "water neighbours" there are bank, not surface. Measured on
    // Granby, every setting trades one defect for the other:
    //
    //     interior 25 m, probe 20 m   5,643 sand columns in the lakes
    //                                     22 water samples over a 30% grade
    //     interior  6 m, probe  8 m     192 sand columns in the lakes
    //                                  1,257 water samples over a 30% grade
    //
    // 6 m is taken because the sand slabs are the reported defect and they are
    // in the middle of a lake where anyone can see them, while the steep water
    // is on mountainsides above the timberline. It is a choice, not a fix.
    //
    // THE REAL FIX IS IN THE BAKE, and it is the test world/poi.h already
    // passes: flood the water mask, take each body's MEDIAN elevation, and drop
    // the cells that do not sit on it. That is exact, it is a connected
    // component away, and naip2cov.py can afford it because it runs once.
    // Nothing per-column at runtime can see a whole water body, which is why
    // every threshold here is a proxy for the thing that actually decides it.
    static constexpr float kLakeInteriorM = 6.0f;
    bool mappedWater(float x, float z) const {
        if (!cover_.ok()) return false;
        const float sd = cover_.shoreDistance(x, z);
        if (sd <= CoverField::kShoreEdgeM) return false;
        if (sd >= kLakeInteriorM) return true;   // unambiguously open water

        // -- THE SHORE BAND, AND WHY IT WAS A COMB -------------------------
        //
        // (user 2026-09-18: "clean this up", with a photograph of the waterline
        // broken into rectangular teeth.)
        //
        // The grade test used to be a separate BOOLEAN applied per column in
        // this band, and a boolean over a noisy measurement is a coin toss at
        // the margin: neighbouring columns a decimetre apart landed on either
        // side of the 20% line and the waterline came apart into fingers. The
        // measurement is noisy here for a structural reason -- within a few
        // metres of a bank there is barely any water left to measure the water
        // surface across, so it is mostly reading the beach.
        //
        // SO IT IS NO LONGER A SEPARATE TEST. The grade now moves the shore
        // distance a column must have before it counts as water:
        //
        //     flat lake surface   ->  1 m, which is the waterline as drawn
        //     steep ground        ->  25 m, which no small smear ever has
        //
        // Both fields are continuous, so the waterline is the contour of a
        // continuous function and cannot be ragged. The same evidence decides
        // the same cases as before -- a hillside smear still has nowhere near
        // 25 m of shore distance -- but it decides them by degree instead of by
        // a knife edge.
        //
        // -- AND IT WAS STILL SQUARE, WHICH THIS NOTE USED TO DENY ----------
        //
        // What stood here was "it cannot be square either: the shore plane is
        // bilinear". That is true of the plane and false of the CONTOUR, and
        // the difference cost a second report (user, later the same day: "make
        // them smoother, not rounded squares"). The plane naip2cov baked was
        // ONE-SIDED -- flat zero on every land cell, then 35 to 90 m one cell
        // later -- so asking for the kShoreEdgeM contour of it picked a level
        // one part in fifty up a cliff, and the waterline was pinned to the
        // raster edge: a staircase with 17-voxel treads. Interpolating a field
        // says nothing about where its contours land.
        //
        // CoverField::buildShoreField makes the plane signed, so the contour is
        // a real crossing between two cells. Measured on Granby's south shore,
        // the longest straight run of waterline went from 17.6 m to 3.0 m.
        const float sh = dem_.shrink();
        const float d = 8.0f / sh;
        const float h0 = dem_.heightM(x, z);
        const float q = d * 0.7071f;
        const float ox[8] = {d, -d, 0, 0, q, -q, q, -q};
        const float oz[8] = {0, 0, d, -d, q, q, -q, -q};
        float rise = 0.0f;
        int n = 0;
        for (int k = 0; k < 8; ++k) {
            const float sx = x + ox[k], sz = z + oz[k];
            if (cover_.shoreDistance(sx, sz) <= CoverField::kShoreEdgeM) continue;
            ++n;
            rise = maxf(rise, fabsf(dem_.heightM(sx, sz) - h0));
        }
        // No water beside it at 8 m is a channel or a stray -- see the note that
        // was here before; the shore distance tells those apart on its own.
        if (n == 0) return sd >= 6.0f;
        const float grade = rise * sh / 8.0f;
        const float t = clampf((grade - kLakeGradeMax * 0.25f) / (kLakeGradeMax * 0.75f),
                               0.0f, 1.0f);
        return sd > CoverField::kShoreEdgeM + t * (kLakeInteriorM - CoverField::kShoreEdgeM);
    }

    int lakeLineAt(float x, float z, TerrainMemo &memo) const {
        // ------------------------------------- THE MAPPED LAKE'S OWN SURFACE
        // Where the imagery says water, the line is the DEM's level at that
        // point and nothing else: no band line, no basin gate, no noise. That
        // is what makes it FLAT. heightM has already dropped the bed below it,
        // so the mesher has a real column of water to fill.
        //
        // This runs before waterVoxAt, which asks the per-BAND procedural line
        // (see the waterY note) and knows nothing about a lake the photograph
        // found.
        // THE SAME DOOR heightM CARVES THROUGH. It was cover_.at() here too,
        // and the two sample the jitter independently -- so a column could have
        // its bed carved and no line over it, or a line and no bed. That
        // disagreement is a wall standing in open water.
        if (mappedWater(x, z))
            return int(dem_.heightM(x, z) / VOXEL_M);
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
    // IS THIS COLUMN INSIDE A TALL-GRASS TUFT, and if so how big is it and how
    // many strands does it want?
    //
    // A JITTERED SITE LATTICE, scanned 3x3. A site sits in the middle half of
    // its own cell and a tuft's radius is well under half a cell, so nothing
    // outside the ring can reach this column -- the same bound the flower
    // colonies rely on.
    //
    // PURE, hash only, no noise and no memo: it is asked once per column that
    // already grew a blade, which is under a third of them.
    // -----------------------------------------------------------------------
    // ...AND WHERE ITS MIDDLE IS, for the caller that needs the PATCH and not
    // the column. `atX`/`atZ` may be null, which is what the grass pass passes.
    //
    // A TUFT IS THE ONLY THING IN THIS WORLD THAT IS A PATCH OF GRASS. The
    // blades themselves are a per-column hash with no grouping of any kind, so
    // "one patch of wheat" cannot be answered by looking at blades -- it is
    // this site, and its radius is the patch's extent. See App::breakWheat,
    // which mows the whole of one and pays out once for it.
    bool tuftAt(float x, float z, float *radOut, float *wantOut, float *atX = nullptr,
                float *atZ = nullptr) const {
        const int gi = int(floorf(x / tuftCellM));
        const int gj = int(floorf(z / tuftCellM));
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const uint32_t c = hashU32(uint32_t(gi + di) ^ 0x51ED2701u, uint32_t(gj + dj));
                if (hashUnit(strandSeed + 71u, c) > tuftCoverage) continue;
                const float sx = (float(gi + di) + 0.25f + 0.5f * hashUnit(strandSeed + 72u, c)) *
                                 tuftCellM;
                const float sz = (float(gj + dj) + 0.25f + 0.5f * hashUnit(strandSeed + 73u, c)) *
                                 tuftCellM;
                const float rad =
                    tuftRadMinM + (tuftRadMaxM - tuftRadMinM) * hashUnit(strandSeed + 74u, c);
                const float dx = x - sx, dz = z - sz;
                if (dx * dx + dz * dz >= rad * rad) continue;
                *radOut = rad;
                *wantOut = tuftStrandsMin +
                           (tuftStrandsMax - tuftStrandsMin) * hashUnit(strandSeed + 75u, c);
                if (atX) *atX = sx;
                if (atZ) *atZ = sz;
                return true;
            }
        return false;
    }

    // -----------------------------------------------------------------------
    // IS THIS BLADE ONE OF THE TALL ONES?
    //
    // ONE PLACE ASKS IT AND ONE PLACE ANSWERS IT. strandRows below SWITCHES a
    // chosen blade to tallGrassMinRows..tallGrassMaxRows, so the height is the
    // record of the decision and nothing else has to re-derive it from the
    // tuft lattice -- which would be a second copy of a rule that has already
    // been got wrong twice.
    //
    // The flower scatter is the caller (user 2026-09-14: "dont put flowers on
    // tall grass"). A flower stands ON a blade -- Placement::yOff lifts it
    // rows - 1 voxels so the stem meets the sward -- and a tall blade is 15 to
    // 20 voxels, so a flower planted in a tuft is a bloom hanging two metres in
    // the air with a stem that does not reach it.
    // -----------------------------------------------------------------------
    bool tallStrand(int rows) const { return rows >= tallGrassMinRows; }

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
    uint8_t strandRows(int i, int j, uint8_t top, TerrainMemo &memo) const {
        // ONLY ON THE DIRT FLOOR. This used to ask isGrass(top), which was the
        // surface paint; with the paint gone the question is whether this is
        // ground a blade could root in at all -- so sand, rock and a lake bed
        // still grow nothing, and that is the whole of what the old test was
        // really protecting.
        // -- ...AND THE OAK'S GREEN FLOOR, WHICH IS WHY IT HAD NO GRASS ----
        //
        // (user 2026-09-16: "there are still no grass strands in the oak
        // forest".)
        //
        // THIS TEST IS WHAT BROKE IT, and it broke the moment the oak floor was
        // made green a few hours earlier. It asks "is this ground a blade could
        // root in", and it answered by naming the two materials that existed
        // when it was written -- soil and litter. BGRASS_0 is neither, so every
        // column in the oak wood returned 0 and the wood grew not one blade.
        //
        // The floor LOOKED right, which is exactly why this was not obvious: a
        // green surface with no strands on it reads as grass rendered flat
        // rather than as grass that is missing.
        //
        // isBGrass ONLY REACHES THE OAK. The birch floor is soil and litter
        // like the pine's -- see the long note in topMaterial -- so the only
        // ground in the world wearing this ramp is the oak's.
        if (!isSoil(top) && !isLitter(top) && !isBGrass(top)) return 0;
        const float x = wx(i), z = wx(j);
        // HOW MUCH BIRCH THIS COLUMN IS. That wood is a meadow, so its floor
        // gets no clearings at all -- the patch field is faded out of the
        // density as the birch takes over, and the blades grow taller with it.
        const float bm = birchMix(x);
        const float ps = 1.0f / maxf(1.0f, grassPatchM);
        const float patch = fbm(memo.grassMask, x * ps + 31.7f, z * ps + 17.2f, 3);
        const float t = sstep(clampf((patch - 0.5f) * grassPatchGain + 0.5f, 0.0f, 1.0f));
        // The pine floor keeps its glades and thickets; the birch floor is
        // uniformly full, and the two crossfade so the seam is not a line.
        const float pineP = grassDensity * (grassSparse + grassFull * t);
        float p = lerpf(pineP, birchGrassDensity, bm);
        // -- AND THE OAK IS THICK WITH IT -------------------------------
        //
        // (user 2026-09-16: "have the grass strands be abundant in the oak
        // forest".)
        //
        // UNIFORM LIKE THE BIRCH'S AND MUCH DENSER. The pine floor keeps
        // glades and thickets because a conifer stand is patchy underfoot;
        // an oak wood under a closed broadleaf canopy is sward from trunk
        // to trunk, so the patch field is faded out here exactly as it is
        // for the birch -- what is left is one number.
        //
        // 0.40 against the birch's 0.155 and the pine's 0.037..0.578 swing.
        // It sits above the pine's THICKET average rather than between the
        // two woods, which is what "abundant" asks for: the floor a blade
        // does not grow on is the exception rather than the rule.
        //
        // SAFE TO APPLY AFTER THE BIRCH LERP because no column is ever in
        // two seams at once -- a seam is 90 m and a band is 800 -- so bm and
        // om are never both non-zero and neither blend can undo the other.
        const float om = oakMix(x);
        if (om > 0.0f) p = lerpf(p, oakGrassDensity, om);
        // ...and SHORTER on the birch side -- 1 to 5 voxels against the pine's
        // 3 to 6, which is two voxels off the mean of what the meadow used to
        // stand at. BOTH ENDS CROSSFADE: carrying only the ceiling over would
        // leave the birch range pinned to the pine's floor, and 3..5 is a mean
        // of 4, one voxel short of the ask. Rounded rather than truncated so
        // the crossfade reaches the full value instead of stopping a voxel
        // short.
        float loF = lerpf(float(grassMinRows), float(birchGrassMinRows), bm);
        float hiF = lerpf(float(grassMaxRows), float(birchGrassMaxRows), bm);

        const uint32_t cell = hashU32(uint32_t(i), uint32_t(j));
        if (hashUnit(strandSeed, cell) >= p) return 0;

        // ---- ...AND THE TALL PATCHES, WHICH ONLY CHANGE THE HEIGHT --------
        //
        // AFTER THE PLANTING DRAW, AND THAT PLACEMENT IS THE WHOLE POINT: `p`
        // is already spent, so nothing this does can add or remove a blade. It
        // decides how tall the blades that are ALREADY HERE stand.
        //
        // A SECOND FIELD, NOT A SECOND THRESHOLD ON THE GLADE ONE. Reusing the
        // glade field would make every tall patch the middle of a thicket,
        // which is the one place the floor is already busiest -- the patches
        // would never be seen against it.
        // ---- ...AND THE TALL TUFTS, WHICH ONLY CHANGE THE HEIGHT ----------
        //
        // AFTER THE PLANTING DRAW, AND THAT PLACEMENT IS THE WHOLE POINT: `p`
        // is already spent, so nothing here can add or remove a blade. It
        // decides how tall a few of the blades that are ALREADY HERE stand.
        // The first version of this blended `p` toward 0.85 inside a patch and
        // was rejected in as many words -- "you seemed to just increase the
        // density of the grass. dont do that".
        //
        // NOT BLENDED -- SWITCHED. A chosen blade is 15 to 20, full stop.
        // Easing the height with the field's falloff would ring every tuft
        // with 7- and 9-voxel blades, which is scattered strands wearing a
        // group.
        //
        // ITS OWN SALT, or it would correlate with the planting draw above and
        // the tall ones would all be blades that only just got planted.
        float tuftRad = 0.0f;
        float tuftWant = 0.0f;
        if (tuftAt(x, z, &tuftRad, &tuftWant)) {
            // HOW MANY COLUMNS THE TUFT COVERS, and how many of them grew a
            // blade -- that product is what `tuftWant` has to be spread over.
            const float cols = 3.14159265f * tuftRad * tuftRad / (VOXEL_M * VOXEL_M);
            const float chance = clampf(tuftWant / maxf(1.0f, cols * p), 0.0f, 1.0f);
            if (hashUnit(strandSeed + 0x5BD1u, cell) < chance) {
                loF = float(tallGrassMinRows);
                hiF = float(tallGrassMaxRows);
            }
        }
        const int loRows = maxi(1, int(loF + 0.5f));
        const int hiRows = maxi(loRows, int(hiF + 0.5f));
        const int span = maxi(1, hiRows - loRows + 1);
        return uint8_t(
            mini(STRAND_MAX_ROWS,
                 loRows + mini(span - 1, int(hashUnit(strandSeed + 1u, cell) * span))));
    }

    // For the handful of callers with no memo to hand. A memo is a cache that
    // validates itself, so this gives the same answer -- it just pays for it.
    uint8_t strandRows(int i, int j, uint8_t top) const {
        TerrainMemo memo;
        return strandRows(i, j, top, memo);
    }

    // -----------------------------------------------------------------------
    // WHICH WOOD'S GREEN A BLADE ON THIS COLUMN WEARS.
    //
    // DITHERED AGAINST A COLUMN HASH, not switched at mix = 0.5. A hard line
    // would draw a straight north-south edge across the world where one wood's
    // grass meets the other's -- the one shape nothing else in this terrain
    // has. Testing the mix against a hash of the column interleaves them over
    // the blend, so the two greens dissolve into each other the way a real
    // treeline does. This is the same dither the birch FLOOR used before the
    // floor became dirt; it is the right trick, it was just being spent on the
    // wrong thing.
    //
    // The hash is the COLUMN's, so it is stable: the same column answers the
    // same way every time it is meshed, from any chunk, on any thread.
    // -----------------------------------------------------------------------
    // BROADLEAF, NOT BIRCH. This asked birchMix alone, so a blade in the oak
    // wood came up 0 and wore the PINE ramp -- a dark blue-green needle colour
    // standing in a bright broadleaf wood, and standing on the BGRASS floor the
    // note in topMaterial just put under it. A tuft and the ground it roots in
    // have to be one colour; see the note over mat::BWHEAT_0, which says the
    // same thing about the foot of a tall blade.
    uint8_t bladeMaterial(int i, int j) const {
        const float x = wx(i);
        return (birchMix(x) + oakMix(x) > hashUnit(0x81E5u, hashU32(uint32_t(i), uint32_t(j))))
                   ? mat::BGRASS_0
                   : mat::GRASS_0;
    }

    // -----------------------------------------------------------------------
    // ...AND WHAT A BLADE OF THIS HEIGHT ACTUALLY WEARS.
    //
    // TWO ARGUMENTS ANSWER "WHICH WOOD", THREE ANSWER "WHICH PLANT", and the
    // difference matters at the call sites. A tall blade has gone over to straw
    // (see mat::WHEAT_0) and wears the same ramp in either wood -- so the
    // two-argument form above is still exactly right for the one caller that
    // wants the WOOD rather than the colour, which is the flower scatter
    // picking a stem in chunks.h. Asking it the three-argument question there
    // would read a tall column as a pine one and dither the wrong stem.
    //
    // THE HEIGHT IS PASSED IN RATHER THAN RE-DERIVED, which is tallStrand's own
    // rule: strandRows SWITCHES a chosen blade to the tall band, so the height
    // is the record of that decision and every caller already has it in hand.
    // Deriving it a second time from the tuft lattice would be a second copy of
    // a rule that has already been got wrong twice.
    // -----------------------------------------------------------------------
    // THE SAME DITHER DECIDES BOTH, which is what keeps a tuft's foot the
    // colour of the sward it stands in: a straw ramp begins at its own wood's
    // GRASS_0 (see fillWheatRamp), so a column that would have grown the
    // birches' green grows the birches' straw, and the two agree at the soil by
    // construction rather than by being tuned to.
    uint8_t bladeMaterial(int i, int j, int rows) const {
        const uint8_t green = bladeMaterial(i, j);
        if (!tallStrand(rows)) return green;
        return (green == mat::BGRASS_0) ? mat::BWHEAT_0 : mat::WHEAT_0;
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

    // -----------------------------------------------------------------------
    // ...AND WHICH OF THOSE COLUMNS ACTUALLY CARRY IT.
    //
    // THE LIFT IS BACK (user 2026-09-13: "can you raise the foam upwards by 1
    // voxel? just put a white voxel above the current foam voxel"), and the
    // reason it was a wall the first time is the reason this function exists.
    // A lift raises GEOMETRY. The shader's clumping breaks up the COLOUR and
    // cannot touch the silhouette, so an unbroken band raised a voxel all the
    // way round a lake is a white kerb however it is shaded.
    //
    // So the breakup moves to the host, where it can decide whether the voxel
    // is THERE at all. Two scales, which is what stops it reading as noise:
    //
    //   a ~2.4 m cell     tongues of surf, the same size the shader's mask uses
    //   a per-column hash inside them, so a tongue has a ragged edge rather
    //                     than a square one
    //
    // Result: raised white voxels in broken runs along the water's edge, which
    // is v1's shoreSurf. Nothing continuous, so nothing to read as a kerb.
    //
    // PURE, AND A FUNCTION OF THE WORLD COLUMN, so the mesher and TerrainProbe
    // cannot disagree about where the shore stands -- which is exactly what
    // tests/voxel_probe_test.cpp checks face by face.
    // -----------------------------------------------------------------------
    bool foamCrest(int i, int j) const {
        const uint32_t tongue = hashU32(uint32_t(floorDiv(i, 24)), uint32_t(floorDiv(j, 24)));
        if (hashUnit(0xF0A3u, tongue) > foamCrestTongue) return false;
        return hashUnit(0xF0A4u, hashU32(uint32_t(i), uint32_t(j))) < foamCrestFill;
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
    // THE WORLD COLUMN IS AN ARGUMENT NOW, because the lift is no longer a
    // property of the depth alone -- see foamCrest. Both callers have it: the
    // mesher through ColumnStack::i0/j0 and the probe from its own (i, j).
    int waterTopVox(int i, int j, int h, int line, int crest, bool wet) const {
        if (!wet || line == kNoWaterVox || h >= line) return h;
        const bool lift = foamLiftVox > 0 && foamColumn(h, line) && foamCrest(i, j);
        return line + crest + (lift ? foamLiftVox : 0);
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
        // -- THE LYING SNOW, WHICH IS A LAYER AND NOT A SKIN ----------------
        // Gated on the surface already being snow, so no column below the
        // snowline pays for this at all -- and above it, it is one DEM lookup
        // and a noise sample for the handful of voxels the layer is deep. What
        // is under the snow is whatever the mountain is, unchanged: the bands
        // below carry on from the bare height.
        if (isSnow(top)) {
            if (float(h - y) * VOXEL_M < snowDepthM(wx(i), wx(j))) return snowShade(i, j);
        }
        if (h - y >= kBedrockVox) return mat::BEDROCK;
        if (top == mat::ROCK) return mat::ROCK;
        return (h - y <= crustVox(i, j)) ? mat::SOIL_0 : mat::ROCK;
    }

    uint8_t topMaterial(int i, int j, int h, TerrainMemo &memo) const {
        // Only reached from the sparse scatter paths. Below the shore band the
        // slope is never consulted, so it is not worth four height evaluations
        // to compute one that will be discarded.
        const int wl = lakeLineAt(wx(i), wx(j), memo);
        if (h <= wl + sandRiseVoxAt(wx(i))) return topMaterial(i, j, h, 0, memo);
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
                    //
                    // GRASS, NOT THE FLOOR. This was tm -- the ground's own
                    // material -- which was right only while the ground under
                    // a blade was painted mat::GRASS_0. The floor is soil and
                    // litter now, and left alone this drew brown grass.
                    // ...AND HOW TALL, which decides green against straw.
                    const uint8_t cap = bladeMaterial(I0 + i, J0 + j, rows);
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
                        // `cap`, not tm: a blade's SIDES are grass too, and this was
                        // the half of it the top face did not cover.
                        emitUncovered(m, i, j, d, maxi(lo, ground), hi, nlo, nhi, cap, sc);
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

    // -----------------------------------------------------------------------
    // THE TOP OF THIS COLUMN AS IT IS NOW -- generated, then dug out.
    //
    // THE ONE THING EVERY WALKING QUERY WAS MISSING. heightVox answers the
    // GENERATOR's top in one comparison and cannot see an edit, so the player,
    // the arrows and the fliers all stood on the surface that used to be there:
    // dig a pit, walk into it, and you hover over the hole on the floor that is
    // no longer drawn. The renderer, the mesher and the swing ray had all been
    // moved onto the edit layer; the body had not.
    //
    // CAPPED AT kUndermineDepthVox, which is what makes it affordable. With
    // edits the top is a downward scan rather than a comparison, and the walk
    // asks this several times a frame per contact point -- so the scan is
    // bounded at 6.4 m, deeper than any bite the pick can take, and a column
    // with no edits in its chunk costs the same one comparison it always did.
    //
    // The cap is the same one World::terrainTopAt uses, and that function now
    // delegates here so the two cannot answer differently.
    // -----------------------------------------------------------------------
    static constexpr int kTopScanVox = 64;

    int topVox(int i, int j) {
        const int h = terrain ? terrain->heightVox(i, j, *memo_) : 0;
        if (!edits) return h;
        const std::shared_ptr<const ChunkEdits> ce =
            edits->get(EditStore::floorDiv(i, CHUNK_VOX), EditStore::floorDiv(j, CHUNK_VOX));
        if (!ce) return h;
        int y = h;
        for (int n = 0; n < kTopScanVox; ++n, --y) {
            uint8_t m = mat::AIR;
            if (!ce->voxel(i, j, y, &m)) return y;   // no edit here: still stone
            if (m != mat::AIR) return y;             // filled back in
        }
        return y;
    }

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
        const int wTop = terrain->waterTopVox(i, j, h, line, terrain->waveCeilVox(),
                                              terrain->wetColumn(i, j, h, line, *memo_));
        // ASKED ONCE AND USED TWICE. The height decides whether there is a
        // blade here AND, since tall grass is straw, which ramp it wears -- and
        // calling strandRows twice would be two chances for them to disagree.
        const int sr = terrain->strandRows(i, j, top);
        switch (VoxelTerrain::aboveAt(y, h, sr, wTop)) {
            // GRASS, not the floor, and WHICH grass -- the mesher says the
            // same, and tests/voxel_probe_test.cpp checks the two face by face.
            // ...AND A COLUMN SOMEBODY HAS EDITED GROWS NOTHING, which is
            // StrandColumns' rule (see voxel/columns.h) and has to be this
            // one's too. The mesher stops drawing a blade the moment an edit
            // lands within one column of it; this used to go on reporting the
            // blade for ever, so a mown tuft was gone from the screen and
            // still there to every query -- which is a wheat plant that pays
            // out a drop every time you swing at the air where it used to be.
            //
            // THE SAME 3x3, not an approximation of it. A rule that is nearly
            // the mesher's is the shape of bug this whole comment is about.
            case VoxelTerrain::Above::Blade:
                return bladeCut(i, j) ? mat::AIR : terrain->bladeMaterial(i, j, sr);
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

    // Has anything been dug within one column of this one? See the Blade case.
    bool bladeCut(int i, int j) {
        if (!edits) return false;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const int wi = i + di, wj = j + dj;
                const int cx = EditStore::floorDiv(wi, CHUNK_VOX);
                const int cz = EditStore::floorDiv(wj, CHUNK_VOX);
                // The probe's own one-chunk cache, reused -- a blade band
                // straddles at most two chunks and this is asked once a swing.
                if (!have_ || cx != cx_ || cz != cz_) {
                    ce_ = edits->get(cx, cz);
                    cx_ = cx;
                    cz_ = cz;
                    have_ = true;
                }
                int lo = 0, hi = 0;
                if (ce_ && ce_->column(wi, wj, &lo, &hi)) return true;
            }
        return false;
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
