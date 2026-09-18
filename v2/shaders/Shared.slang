// ---------------------------------------------------------------------------
// Shared.slang -- the contract between the host and the GPU.
//
// Compiled twice: once by Slang as part of the tracer, once by cl.exe as a C++
// header. That is what the HOST_CODE switch below is for, and it is the same
// trick Falcor uses for its own shared types.
//
// THE ONLY HARD RULE IN THIS FILE IS THE PACKING, and it is worth stating
// because breaking it does not fail to compile -- it silently reads the wrong
// field and the picture goes wrong somewhere else entirely.
//
// A constant buffer lays its members out in 16-byte rows, and a member may not
// straddle a row boundary. C++ has no such rule: it packs on natural alignment,
// so a float3 followed by another float3 sits at offset 12 in C++ and at offset
// 16 on the GPU. Every struct here is therefore written so that EVERY 16-BYTE
// ROW IS EXACTLY FILLED -- a float3 is always followed by a float, and anything
// that would need an array is a float4 with an unused w. Where a field exists
// only to fill a row it is named pad and does nothing else.
//
// The static_asserts at the bottom are the enforcement. They check the sizes a
// correct layout produces, so a member added in the middle without its padding
// breaks the build here rather than the render later.
// ---------------------------------------------------------------------------
#pragma once

#if defined(__cplusplus)
#define HOST_CODE 1
#endif

#ifdef HOST_CODE
#include "Utils/Math/Vector.h"
namespace v2
{
using float2 = Falcor::float2;
using float3 = Falcor::float3;
using float4 = Falcor::float4;
using uint2 = Falcor::uint2;
// uint4 is only used by V2Holo, whose glyph bitmaps are an ARRAY -- and a
// constant buffer gives every array element a 16-byte stride whatever its
// element type, so a bare `uint bits[12]` would occupy 192 bytes on the GPU and
// 48 in C++. Four to a row is the packing the hardware was going to use anyway,
// written down so both ends agree about it.
using uint4 = Falcor::uint4;
// int3 is only used by V6DdgiConsts, but it has to be aliased here for the same
// reason every other vector type is: without it cl.exe parses the member as a
// bare int, the struct comes out short, and the only thing that notices is the
// static_assert at the bottom of this file -- which is exactly its job.
using int3 = Falcor::int3;
using uint = uint32_t;
#endif

// The edge of a voxel, in metres. Duplicated from voxelworld.h rather than
// included, because that header is C++ and this one has to compile as Slang.
// A static_assert in world.h checks the two agree.
static const float kVoxelM = 0.1f;

// ---------------------------------------------------------------------------
// THE ATMOSPHERE'S PLANET, AND THE SIZES OF ITS THREE TABLES.
//
// Here rather than in Atmosphere.slang because the host needs them too: it
// creates the textures at these dimensions and converts the camera's altitude
// from metres to this radius. Atmosphere.slang carries the MEDIUM -- the
// scattering coefficients, the scale heights, the ozone tent -- which only the
// GPU evaluates and which therefore stays there.
// ---------------------------------------------------------------------------
static const float kAtmoGroundR = 6360.0f;  // km
static const float kAtmoTopR = 6460.0f;     // km

static const int kAtmoTransW = 256, kAtmoTransH = 64;
static const int kAtmoMultiW = 32, kAtmoMultiH = 32;
static const int kAtmoSkyW = 192, kAtmoSkyH = 108;

// WHERE THE GROUND RAMPS LIVE IN THE MATERIAL TABLE.
//
// Duplicated from mat:: in voxelworld.h for the same reason kVoxelM is -- this
// header has to compile as Slang and cannot include a C++ one -- and checked
// against it by static_assert in world.h, because a silent disagreement here
// would paint the floor in tree bark.
//
// The mesher stores only the BOTTOM of a ramp. Which shade a voxel actually
// takes is decided per voxel on the device; see groundShade() in Trace.cs.slang
// for why that is where it belongs.
static const uint kGrass0 = 7;
static const uint kGrassCount = 6;
// THE BIRCH WOOD'S OWN GRASS. A second ramp, not a wider one: the two woods
// are different greens, and one ramp spanning both matched neither. Mirrors
// mat::BGRASS_0 exactly.
static const uint kBGrass0 = 32;
static const uint kBGrassCount = 6;
// ...AND HOW MANY OF THOSE SIX THE GROUND ITSELF IS ALLOWED. A blade reads
// this ramp as a gradient up its own height and short grass is 3 to 6 voxels,
// so no blade ever reaches the top three shades; the floor scattered over all
// six and came out 29% brighter than the grass standing in it. Mirrors
// mat::BGRASS_FLOOR_COUNT, which derives it from the blade height cap.
static const uint kBGrassFloorCount = 3;
// ...AND IN WHAT PROPORTION. The floor scattered those three uniformly while a
// blade lays them down its own height two voxels per shade, so the same three
// greens averaged to two different colours and the ground read as not matching
// the grass standing in it -- measured at 15.5% brighter. These are the
// blade's own odds, counted over every height the oak grows: 8 / 7 / 3 of 18.
// Mirrors VoxelTerrain::kBladeW0/1/2, which derives them and is asserted
// against these in gpu/world.h.
static const uint kBGrassFloorW0 = 8;
static const uint kBGrassFloorW1 = 7;
static const uint kBGrassFloorW2 = 3;
// TALL GRASS, WHICH GOES OVER TO STRAW ON THE WAY UP. Two more blade ramps,
// one per wood for the same reason GRASS and BGRASS are split -- each STARTS at
// its own wood's GRASS_0, so the foot of a tuft is the colour of the short
// blades around it and the tuft grows out of the sward instead of being planted
// in it. TEN shades rather than six, because these grade a 15-to-20 voxel blade
// rather than a 6-voxel one, and they get LIGHTER all the way up: green is a
// dark albedo and straw is a bright one. Mirrors mat::WHEAT_0 / mat::BWHEAT_0.
static const uint kWheat0 = 38;
static const uint kWheatCount = 10;
static const uint kBWheat0 = 48;
static const uint kBWheatCount = 10;
static const uint kSoil0 = 13;
static const uint kSoilCount = 4;
static const uint kLitter0 = 17;
// THE STONE RAMP. One id on the host (kRock), six shades here -- filled at load
// from the boulder models' own palettes, so dug terrain is the same stone as
// the rocks lying on it. See mat::STONE_0 and Palette::setStoneBand.
static const uint kRock = 1;
static const uint kStone0 = 20;
static const uint kStoneCount = 6;
// THE SAND RAMP, on the same principle as the stone one above: one id on the
// host so a beach still merges into long runs, four shades here so it does not
// render as a single flat plane. Mirrors mat::SAND / mat::SAND_0 exactly.
static const uint kSand = 4;
static const uint kSand0 = 28;
static const uint kSandCount = 4;
static const uint kLitterCount = 3;
// WATER. A material, not an instance kind -- see mat::WATER in voxelworld.h for
// why the per-chunk water structure is gone. The shading branch keys off this
// and off the triangle's FACE, so a lake's top ripples and its edges do not.
static const uint kWater = 26;
// Churned shore water. It is water for every purpose a ray cares about --
// notably that a shadow ray must pass THROUGH it, see traceOccluded.
static const uint kFoam = 27;

// THE WAVE HAS NO GEOMETRIC BUDGET, because it needs none.
//
// There used to be a constant here for how much room the swell was given --
// first as a lid over the water, then as a downward offset under it. Both
// were wrong, and in the same way: they tried to give the wave somewhere to
// live in the ACCELERATION STRUCTURE. A lid stood its side faces half a metre
// over every beach (4,743 of 4,743 lake edges), and a downward offset just
// mirrors that, since the sides stay where they were meshed.
//
// v1 needs no budget at all: it backs the ray up from the flat hit and
// marches, so a crest is found in the empty space the ray already crossed.
// See marchWaterVoxels in Trace.cs.slang. The water is meshed flush at the
// waterline and stays there.

// HOW A TRIANGLE'S WORD IS LAID OUT.
//
//     bits  0..7   material
//     bits  8..10  face direction (0..5)
//     bits 11..15  strand code: 0 for everything that is not a grass blade,
//                  otherwise 1 + (the blade's base row & 15)
//
// THE WORD IS FULL. Bit 15 was the spare and the tall grass took it -- see
// STRAND_MAX_ROWS in voxelworld.h for what it bought and what it costs.
//
// Duplicated from packTri in voxelworld.h and asserted against it in world.h,
// for the same reason the ramps above are. The direction was a whole byte
// until the blades wanted somewhere to put their gradient, so ANYTHING READING
// IT HAS TO MASK -- an unmasked shift now returns a strand code as well and
// faceNormal would answer -Z for every blade in the frame.
static const uint kTriDirShift = 8;
static const uint kTriDirMask = 0x7u;
static const uint kTriStrandShift = 11;
static const uint kTriStrandMask = 0x1Fu;
// A strand is at most THIRTY-TWO voxels tall: four bits of stored row at two
// voxels a step. The mesher clamps to it, and world.h static_asserts that this
// and STRAND_MAX_ROWS still agree. See the long note over STRAND_MAX_ROWS in
// voxelworld.h for why the resolution was spent rather than the range.
static const uint kStrandRowMask = 0xFu;
static const int kStrandRowShift = 1;   // STRAND_ROW_STEP == 1 << this

// SLANG ONLY. This file is otherwise types and constants, and it has to be,
// because cl.exe compiles it as a header in half a dozen translation units --
// a function at namespace scope here would be a duplicate symbol at link time.
// The host already has its own accessors next to packTri in voxelworld.h; the
// static_asserts in world.h are what keep the two spellings honest.
#ifndef HOST_CODE
uint triDir(uint packed) { return (packed >> kTriDirShift) & kTriDirMask; }
uint triMaterial(uint packed) { return packed & 0xFFu; }
uint triStrand(uint packed) { return (packed >> kTriStrandShift) & kTriStrandMask; }
#endif

// The sun subtends about 0.53 degrees; this is the cosine of its half-angle.
// Also duplicated, also asserted.
static const float kSunCosThetaMax = 0.99998932f;

// The solid angle that cone subtends, in steradians -- about 6.7e-5 for a
// half-degree sun. Written down once because two different shaders need it and
// only one of them gets it for free: the camera path's NEE divides by the cone
// pdf, and 1/pdf IS this number, while a probe shader that light-samples
// nothing has to multiply by it explicitly. See the note in ProbeTrace.
static const float kSunSolidAngle = 6.28318530717958647692f * (1.0f - kSunCosThetaMax);

// What a surface is, which is all the shading needs to know beyond its
// material. The terrain and the rocks are the same thing to the shader; a tree
// differs only in taking a per-instance tint, and water is its own BSDF.
static const uint KIND_TERRAIN = 0;
static const uint KIND_WATER = 1;
static const uint KIND_TREE = 2;
// The tool in the player's hand. It is an instance like any other -- see
// World::setHeldInstance -- and the kind exists for exactly two things the
// tracer has to do differently with it, both in Trace.cs.slang: normalise the
// transformed normal, because this is the one instance carrying a scale; and
// write a zero motion vector, because it is the one instance bolted to the
// camera.
static const uint KIND_HELD = 3;

// A BUTTERFLY, and anything else that flies its own frames through the wood.
//
// It is an ordinary instance in the ordinary structure: the same triangle pool,
// the same material table, the same shadow and bounce rays as the terrain it
// flies over. That is the whole point of it -- a flyer here is lit by the wood
// it is in, and lights it back, which a sprite composited over the finished
// frame can never do.
//
// So this kind is not a second shading path. It exists for exactly two things
// the tracer has to do differently, both in Trace.cs.slang:
//
//   NORMALISE THE TRANSFORMED NORMAL, for the same reason the held item does.
//   A flyer's 3x3 is a free turn about Y times the scale it materialises
//   through, so a face normal comes out of it that factor short of unit.
//
//   REPROJECT IT AS SOMETHING THAT MOVED. The motion vector below assumes a
//   static world and a moving camera, which is true of every other instance in
//   this scene and false of this one. prevOffset carries how far the instance
//   travelled since the last frame, and subtracting it is the difference
//   between a butterfly and a smear where a butterfly was.
static const uint KIND_FLYER = 4;

// ---------------------------------------------------------------------------
// A PIECE THAT HAS COME LOOSE, and why it needs its own kind.
//
// Every other voxel in this world stands still, so its shade can be picked by
// hashing WHERE IT IS: groundShade turns a material family into one exact
// shade using floor(p / kVoxelM), and a voxel that never moves always lands in
// the same cell and always wears the same green.
//
// A broken-off chunk moves. The same hash then re-rolls its colours every
// frame as it drifts across cell boundaries, and a moss voxel straddling one
// draws two greens at once -- the moss crawls and splits while the piece is in
// the air.
//
// So a loose body's shades are resolved ONCE, on the host, when its mesh is
// built (see meshVolume), and the material byte it carries is already the
// final one. This kind is what tells the device not to roll it again.
static const uint KIND_LOOSE = 5;

// ---------------------------------------------------------------------------
// THE TWO RAY MASKS, AND WHY A VIEWMODEL NEEDS ONE.
//
// A held tool is ten centimetres from the eye and the world comes right up to
// the lens -- grass at the camera, a trunk you can lean on. Put the tool in the
// structure and let the primary ray take the nearest hit, and the wood wins:
// press against a birch and the trunk swallows the axe whole, which is not
// something a hand does.
//
// So the primary ray is traced TWICE, once against each mask, and the tool
// takes the pixel whenever it was hit at all -- in front of everything, which
// is where a hand is. Every OTHER ray uses both bits and sees both, which is
// the whole reason the tool is in the structure: it casts a shadow, it bounces
// light, and it shows up in what the wood around it is lit by.
//
// The alternative -- one trace, nearest wins -- is one ray cheaper and is what
// the first cut did. It is in the git history as a picture of an axe inside a
// tree.
// ---------------------------------------------------------------------------
static const uint MASK_WORLD = 0x01;
static const uint MASK_HELD = 0x02;

// ---------------------------------------------------------------------------
// The camera, as the tracer wants it: a basis and two half-extents, with none
// of the trigonometry left to do per pixel.
// ---------------------------------------------------------------------------
struct V6Camera
{
    float3 pos;
    float halfW;
    float3 u; // right
    float halfH;
    float3 v; // up
    float aperture;
    float3 w; // forward, unit
    float focusDist;
};

// ---------------------------------------------------------------------------
// Preetham's daylight model, already fitted.
//
// The fit is tan(), pow() and a dozen polynomial evaluations, and it depends
// only on the sun angle and the turbidity -- not on the ray. Doing it per ray
// would be several hundred wasted instructions on every escaping ray in the
// scene, so it happens once a frame on the host and only the coefficients cross
// the bus.
//
// A, B, C, D, E and the two normalisers are per-channel in xyY, so they are
// float4 with an unused w rather than float[3]: an array in a constant buffer
// has a 16-byte STRIDE, which would make a float[3] occupy 48 bytes on the GPU
// and 12 in C++.
// ---------------------------------------------------------------------------
struct V6Sky
{
    float3 sunDir;
    float skyScale;
    float3 sunRadiance; // constant across the disk
    // THE VIEWER'S RADIUS IN KILOMETRES, for the atmosphere path -- planet plus
    // the camera's altitude. Took the padding word, exactly as moonKey did
    // below, so V6Sky is still 192 bytes and the static_assert still holds.
    // It lives in V6Sky rather than V6Params because skyDome() takes a V6Sky
    // and nothing else, and it is read by ProbeTrace as well as the tracer.
    float atmoViewHeight;
    float3 groundAlbedo;
    // 0 for the Preetham fit, 1 for the Hillaire tables. A float rather than a
    // bool or an int because this is the last word of a 16-byte row and a
    // 4-byte scalar of any type fits; float keeps the comparison in skyDome()
    // free of an integer unit that shader has no other use for.
    float atmoOn;
    float4 A, B, C, D, E;
    float4 zenith; // Y in cd/m^2, then x and y
    float4 normF;

    // -- the moon ------------------------------------------------------------
    //
    // HUNG AT THE ANTI-SOLAR POINT, so it is up exactly when the sun is down and
    // the two never share the sky. That is also why the phase cannot come from
    // its POSITION: geometrically it would be full every single night. The phase
    // is a separate clock, and the disc is shaded from a synthetic light swung
    // by it -- see moonDisk in Sky.slang.
    float3 moonDir;
    float moonPhase;    // 0..1 around the synodic month; 0 is full
    float3 moonRadiance;  // the DISC.s own radiance, sized for the tone curve
    // How much brighter the KEY light is than the disc. They cannot be one
    // number: a daylight-calibrated curve cannot hold a body bright enough to
    // light a wood AND show a gradient across its own face. Took the padding
    // word, so the struct is the same size.
    float moonKey;
};

// ---------------------------------------------------------------------------
// Everything the BSDF asks a surface for, per material id. One of these per
// entry in the palette; the whole table is a few kilobytes and is uploaded once.
// ---------------------------------------------------------------------------
struct V6Material
{
    float3 albedo;
    float roughness;
    float specular;    // F0 at normal incidence
    float translucency; // fraction of the diffuse lobe that passes through
    // -- HOW MUCH OF THE RAY THIS SURFACE ACTUALLY STOPS -------------------
    //
    // 1 is opaque, which every material in the world is but one. Below it the
    // path loop passes the ray STRAIGHT THROUGH with probability 1 - alpha and
    // traces again -- see the see-through block in Trace.cs.slang.
    //
    // NOT `translucency`, WHICH IS A DIFFERENT THING and already spoken for:
    // that is the fraction of the diffuse lobe that scatters out of the far
    // side, which is how a leaf glows with the sun behind it. A leaf is not
    // see-through and a fly's wing is. One is about where the light goes, the
    // other about whether the surface is there at all.
    //
    // It takes pad0 rather than growing the record, so V6Material stays at its
    // asserted 32 bytes.
    float alpha;
    // -- ...AND WHETHER IT IS A DIELECTRIC RATHER THAN A SURFACE -----------
    //
    // (user 2026-09-15: "make the smoke voxels grey, then make them
    // transparent like the water. they look scratchy with alot of noise".)
    //
    // 0 IS "NOT ONE", which every material in the world is but two. Above 1 the
    // tracer gives this entry water's own treatment: a delta lobe with real
    // refraction, which is DETERMINISTIC.
    //
    // WHY THAT IS THE FIX AND alpha IS NOT. alpha passes the ray through with
    // probability 1 - alpha and traces again -- a coin flip per pixel per
    // frame. At the smoke's 0.2 that is four rays in five missing it, so each
    // pixel independently sees smoke or does not, and what you get is a cloud
    // of speckle that no denoiser can settle because there is nothing stable
    // under it. The fly's wing was the first thing to wear alpha and the first
    // complaint about it was the same word: it was taken off again.
    //
    // A dielectric asks no coin. Every ray that meets the face refracts through
    // it, so the pixel is the same every frame and the image is clean.
    //
    // It takes pad1, so V6Material stays at its asserted 32 bytes.
    float ior;
};

// ---------------------------------------------------------------------------
// One per instance in the acceleration structure, indexed by InstanceID().
//
// This is the whole of what the shader binding table used to carry, and it
// collapsed to a structured buffer the moment the pipeline went inline: there
// are no hit groups to bind, so "which triangle array does this instance read"
// is just a number the instance carries.
//
// triOffset is in UINT16 UNITS, not bytes -- it indexes the packed-tri pool
// the same way a primitive index does, so the shader adds the two and is done.
// ---------------------------------------------------------------------------
struct V6Instance
{
    float3 tint;
    uint triOffset;
    // HOW FAR THIS INSTANCE MOVED SINCE THE LAST FRAME, in world metres: the
    // point it is at now, less this, is where it was. Zero for everything that
    // stands still -- the wood, the water, the chunks -- so the motion vector's
    // static-world formula is untouched for all of them.
    //
    // NOT WRITTEN BY WHATEVER MOVED THE OBJECT. World::place derives it from
    // the transform it is handed and the one it handed last frame, for every
    // instance that is placed rather than built once. That is deliberate and it
    // is worth the note: while this WAS written by hand, two of the three
    // things in this scene that move never wrote it, and both ghosted the whole
    // length of their travel.
    //
    // AHEAD OF `kind` AND NOT AFTER IT. A float3 wants a 16-byte boundary in a
    // structured buffer on the Vulkan path; put here it starts one, and the
    // record is the same 32 bytes it always was. Put after `kind` it would
    // start at 20, and what a member that straddles a row reads is not an
    // error -- it is the wrong numbers.
    float3 prevOffset;
    uint kind; // one of KIND_*

    // -----------------------------------------------------------------------
    // AND THE PART OF A FLYER THAT MOVES WITHOUT THE INSTANCE MOVING.
    //
    // prevOffset above is the whole story for anything rigid: the instance
    // travelled, so every point on it travelled by the same vector. A
    // butterfly is not rigid. Its flap is EIGHT AUTHORED GRIDS swapped under
    // one transform -- see the note over KIND_FLYER -- and between two of them
    // the body stands still while the wings step a voxel up or down. Reproject
    // a wing by the body's vector and Ray Reconstruction is told the wing held
    // still when it did not, which is exactly the lie that ghosts it: the
    // butterfly comes out a smear the size of two poses, and it never
    // sharpens, because it never gets two frames of the same wing.
    //
    // The step is a property of WHERE ON THE MODEL the surface is, and the art
    // makes that a one-dimensional question: the wings are a roll about the
    // body's own long axis, so how far a voxel rose is fixed by how far out
    // along the wing it sits. Two numbers cover the model -- the inner wing and
    // the tips -- and the body, which never moves, needs none.
    //
    //   x  how far the INNER wing rose since the last frame, world metres.
    //   y  ...and the TIPS. Both already carry the fade's scale.
    //   z  the model's own centre, in object metres: what the tracer measures
    //      "how far out along the wing" from.
    //
    // Zero for everything that is not mid-flap, which includes every instance
    // in the wood and a flyer whose pose did not change this frame. See
    // Butterflies::publish for where the two numbers come from -- they are
    // MEASURED off the eight grids at load, not written down here, so
    // re-authoring the flap cannot leave this describing the old one.
    // -----------------------------------------------------------------------
    float3 flap;
    // -- AND WHAT IT MEANS DEPENDS ON WHETHER THIS IS SET --------------------
    //
    // Padding until now, and it is the discriminator for a SECOND kind of
    // surface motion the transform cannot describe. Zero: `flap` above is the
    // butterfly's wing step, read as it always was. Non-zero: this instance
    // SPUN in place by this many radians about a vertical axis, and `flap` is
    // that axis in WORLD space rather than a wing measurement.
    //
    // The perched songbirds need it because their rotation lives in the ART.
    // Eleven authored frames ARE a quarter turn, so the bird turns 8.2 degrees
    // every 66 ms while its instance transform does not move at all -- the same
    // shape of problem as the wing, and unrepresentable the same way, but a
    // rotation instead of a lift. Rendering the turn through the transform
    // instead would fix the motion vector and lose the grid alignment that is
    // the whole reason the frames exist.
    //
    // The two readings are exclusive by construction: a wing step has no axis
    // and a spin has no wing.
    //
    // ...AND "BY CONSTRUCTION" HAS TO BE BUILT SOMEWHERE, WHICH IT WAS NOT.
    // The perched songbirds publish their axis on EVERY frame and only turn on
    // one frame in four (their eleven poses step every 66 ms), so three frames
    // in four arrived here with a zero angle and a live axis -- read, by the
    // branch below, as a wing measurement. flap.z is a world Z where the wing
    // code expects an object-space centre, so outAlong came out in the
    // thousands and every voxel took the far bucket: h.moved.y += flap.y, which
    // is the bird's own ALTITUDE IN METRES. Ray Reconstruction went looking for
    // history tens of metres above the bird, found nothing, and rebuilt those
    // pixels from the current frame alone -- which at DLAA is a whole sample
    // and at Balanced is 42% of one. That is the "the shading is flickering"
    // report, and the reason it went away with upscaling switched off.
    //
    // World::setFlyerInstance now treats a spin with a zero angle as no spin at
    // all and zeroes `flap` with it, in the one place that writes both fields.
    float flapPad;

    // -- STRUCK, AND FOR HOW MUCH LONGER ------------------------------------
    //
    // (user 2026-09-14: "when hitting life, it turns an emmisive red. all the
    // voxels that make up the life turn red".)
    //
    // 0 for everything in the world and for every animal that has not just been
    // hit; 1 at the instant of the blow, fading to 0 across half a second. The
    // shader reads it twice: it REPLACES the albedo, so every voxel of the
    // animal is red rather than tinted red, and it adds an emission, so the
    // flash reads in shadow -- which is v1's own two-part rule ("it lights
    // itself -- see the sun/AO block"; "otherwise an animal hit under a canopy
    // barely changed colour").
    //
    // A FIELD OF ITS OWN, AND THE RECORD GROWS BY A ROW FOR IT. There were two
    // cheaper places to put this and both are traps. `tint` is free on a flyer
    // -- it is only read for KIND_TREE -- but then "is this animal flashing" is
    // "is its tint not white", which is exactly the implicit discriminator that
    // made flapPad necessary one field above. And a new KIND would cost this
    // instance its flap reprojection for the half second it is dying, which is
    // the one thing this engine has been burned by three times. Sixteen bytes
    // an instance is the honest price.
    float hurt;
    float3 hurtPad;
};

// ---------------------------------------------------------------------------
// Everything the probe passes need to know about the volume.
//
// A SECOND COPY OF NUMBERS RTXGI ALREADY HAS, and deliberately so. The SDK
// keeps its own packed volume descriptor in a structured buffer laid out for
// its own shaders; reading that from Slang would mean reproducing
// DDGIVolumeDescGPUPacked field for field and tracking it across SDK versions.
// These are the eleven values v2's own shaders actually use, filled from the
// volume's public getters each frame, so the two can never disagree about a
// field neither of them names the same way.
//
// The 16-byte row rule at the top of this file applies here exactly as it does
// to V6Params -- every row below is filled.
// ---------------------------------------------------------------------------
struct V6DdgiConsts
{
    float3 origin;
    float maxRayDistance;

    int3 probeCounts;
    int numRays;

    float3 probeSpacing;
    float normalBias;

    // The per-frame random rotation applied to the spherical Fibonacci ray set.
    // See the note in ProbeTrace.cs.slang for why 192 fixed directions would
    // not be enough without it.
    float4 rayRotation;

    // How far a SCROLLING volume has renumbered its probes. A scrolling volume
    // moves without moving its probes: the grid indices shift and only the
    // plane that just came into range is relearned.
    int3 probeScroll;
    int useClassification;

    float2 irradianceTexels;  // x: including border, y: interior only
    float2 distanceTexels;

    // The atlas is stored with a high gamma because irradiance spans orders of
    // magnitude and a 10-bit unorm cannot hold that linearly.
    float irradianceGamma;
    float viewBias;
    float ddgiPad0, ddgiPad1;
};

// ---------------------------------------------------------------------------
// THE LABEL THAT HANGS OVER A BUTTON.
//
// A word standing in the air of the pause room, a hand's width above the button
// it names. NOT AN OVERLAY: it is a plane at real world coordinates that the
// camera ray is tested against, so it has parallax, it is hidden by anything in
// front of it, and it reprojects for the denoiser out of the same motion vector
// every surface in the wood uses. Walk sideways and it slides across the wall
// behind it, because it is genuinely in front of it.
//
// PORTED FROM v1'S STACK BADGE, which is the x{n} beside the held tool -- its
// note says "drawn INTO the image (user: not HTML in the corner)". v1 packs
// each of its digits into 25 bits of a uint and asks one bit per pixel whether
// to paint. That is exactly what happens below, and the only thing that has
// changed is WHERE the question is asked: v1 projects a world anchor down to a
// canvas pixel on the CPU and rasterises the glyphs flat on the glass, tilting
// and shearing them so that they LOOK like they are in the scene. A path tracer
// does not have to pretend -- it can intersect the quad the badge is drawn on
// and get the perspective, the occlusion and the depth for free.
//
// THE FONT IS ON THE HOST. What crosses is a bitmap per glyph, so changing a
// label is a string literal in render/holotext.h and never a shader edit.
// ---------------------------------------------------------------------------
// THREE FOR THE PAUSE-ROOM BUTTONS AND ONE FOR THE HAND. The fourth is the
// stack count beside the held item (user 2026-09-14: "import the stacked item
// numbers next to the hand held object ... similar to the current main menu
// floating text") -- which is what that phrase means: the same world-space
// glyphs the room's labels are drawn with, not a 2D overlay.
//
// THE TWO ARE NEVER UP TOGETHER, so this could have been slot 0 re-used. It is
// not, because "never together" is a rule about two features that know nothing
// about each other, and the day the room grows a fourth button the hand's
// number would appear on it.
// -- AND HOW MANY MATERIALS RENDER AS LIGHT WITHOUT BEING ONE -----------
//
// See V6Params::emitters. Four: v1's amber ember, the red one a blow on a
// living thing throws, and two spare -- this is the table that stops the next
// emissive particle being a fourth copy of the firefly's two fields.
static const int kEmitSlots = 4;

// -- ...AND HOW MANY PENDANTS A PLACE MAY HANG ---------------------------
//
// See V6Params::bulbs. bulbPos is ONE position and sampleBulbSplit traces one
// shadow ray at it, which was right while the only lit room in the engine was
// the pause room. Nuketown has eight, and picking the nearest to the CAMERA --
// the first cut -- makes a room go dark as you walk out of it. Reported as
// "the lightbulb seems to turn off when the player is away and turn on when
// the player is near", which is exactly what a light that follows the eye does.
//
// THE FIX IS PER SHADING POINT, NOT PER FRAME, and that is why this is an array
// rather than a better choice of the one: each shaded vertex lights from
// whichever bulb is nearest IT, so every room is lit by its own lamp at all
// times and the cost is still exactly ONE shadow ray. What it gives up is a
// room lit by two of its own bulbs at once, which no room in this map has.
//
// TWELVE. The map measures eight and a slot is 16 bytes.
static const int kBulbSlots = 12;

// How much of the shadow ray nearest a lamp is ignored, in metres -- enough to
// clear the whole fitting and not just the glass. See sampleBulbSplit, where
// the cap printed a dark disc on every ceiling in the map.
static const float kBulbSelfClearM = 0.55f;

// -- THE WOUND RED -------------------------------------------------------
//
// v1's HURT_RED, unchanged, and its note is worth keeping with it: "it has to
// stay <= 1 in every channel", because that engine writes sqrt(albedo) into an
// rgba8unorm buffer and anything above 1 is clamped there and decodes as 1.0.
// The same is true here for the albedo half of the flash; the EMISSION half is
// scaled separately and may go as bright as it likes.
static const float3 kHurtRed = float3(1.0f, 0.07f, 0.055f);
// ...and how hard it glows. Far under the spark's 42: a spark is one voxel and
// an animal is a few hundred, so the same radiance would be a flare.
static const float kHurtNits = 6.0f;

static const int kHoloSlots = 4;      // three pause-room buttons + the hand
static const int kHoloHand = 3;       // ...and which one the hand's is
static const int kHoloCols = 5;       // a glyph's own grid
static const int kHoloRows = 6;       // five for the letter, a sixth for the tail of a g
static const int kHoloAdvance = 6;    // ...and the sixth column is the gap between them
static const int kHoloMaxGlyphs = 12; // as long a word as one slot can carry

struct V2Holo
{
    // The TOP-LEFT corner of the block of text, in world metres -- the corner of
    // the first glyph's first cell, not the middle of the word. A rasteriser's
    // origin, because that is what the cell arithmetic below wants; the host
    // centres the word on its button before it writes this.
    float3 org;
    // The edge of one glyph pixel, in metres. ZERO IS THE OFF SWITCH: a slot
    // nobody has written draws nothing, and leaving the room is one memset.
    float cell;

    // Unit vectors along the page: +right is the reading direction, +up is up
    // the letters. They are handed over rather than derived from a normal
    // because the text has a ROLL that a normal alone cannot express.
    float3 right;
    uint count; // glyphs in the word, 0..kHoloMaxGlyphs

    float3 up;
    float pad0;

    // What a lit glyph pixel emits, in the same working units as bulbRadiance.
    //
    // IT IS AN EMITTER AND NOT A PAINTED SURFACE, and against this room that is
    // not a stylistic choice. The walls are white at 226 and lit to a mean of
    // 180, so anything reflective enough to read against them would have to be
    // brighter than white paint -- and anything ADDED to them (which is what a
    // translucent hologram is) disappears into them. A saturated colour that
    // owes nothing to the room's light is the only thing that stays legible on
    // a white wall, and it is also what a hologram is.
    float3 tint;
    float pad1;

    // The word, one 30-bit bitmap per glyph: bit (row * kHoloCols + col), row 0
    // at the TOP and col 0 at the LEFT. v1's GLYPH5 packs its digits the same
    // way, 25 bits to a 5x5 cell.
    uint4 bits[3];
};

// ---------------------------------------------------------------------------
// The per-launch constants.
// ---------------------------------------------------------------------------
struct V6Params
{
    V6Camera cam;

    // LAST FRAME'S CAMERA, and it exists for exactly one reason: a motion
    // vector. The world is static and only the camera moves, so where a surface
    // was on screen last frame is not something to store per surface -- it is
    // this frame's hit point projected through last frame's camera, exactly.
    // That is the whole of the motion vector pass, and it costs one dot product
    // per pixel because the primary hit is already in hand.
    V6Camera prevCam;

    V6Sky sky;

    uint2 dim;

    // TWO COUNTERS, AND THEY MEAN DIFFERENT THINGS.
    //
    // `frame` is the ACCUMULATION index: how many samples are already in the
    // film for this camera. It returns to zero the instant the camera moves,
    // because every sample in the film was drawn for a view that no longer
    // exists.
    //
    // `tick` only ever increases. It seeds the sampler and picks the jitter,
    // and it must NOT reset with the accumulation -- successive frames have to
    // draw different samples, and seeding from an index that keeps returning to
    // zero redraws the same one over and over.
    uint frame;
    uint tick;

    uint seed;
    uint maxAccum; // cap on how many samples the film averages, 0 for none
    int maxDepth;
    int rrStart;

    float clampIndirect; // firefly ceiling on indirect light, 0 = off
    float fogDensity;    // extinction per metre at y = 0
    float fogHeight;     // e-folding height of the haze, metres

    // Whether gAccum is a FILM or a formality.
    //
    // It is a film whenever more than one sample will land in it -- the
    // accumulating renderer, or several samples per displayed frame. Under Ray
    // Reconstruction at one sample a frame it is neither read nor written, and
    // saying so here is worth a full-resolution RGBA32F read and write per
    // frame. Took the padding word this struct was already carrying, so the
    // layout is unchanged.
    int keepFilm;

    float2 jitter; // sub-pixel offset in pixels, shared by the whole frame
    int shadowRays;
    int shadowRayDepth;

    // Non-zero when the frame is being handed to the denoiser rather than
    // accumulated. It does not change a single ray -- it only says whether the
    // guide buffers below are worth writing, and whether the film should
    // accumulate or start clean every frame.
    int writeGuides;
    // The near and far planes the guide depth is written against. Only DLSS
    // ever reads it, and only to tell one surface from another across a frame,
    // so the range is chosen to span the world rather than to be precise.
    float depthNear, depthFar;

    // PHASE B. Non-zero when the tracer should also write the demodulated
    // channels -- the diffuse and specular radiance kept apart, the emission
    // held out, and the albedo to divide the diffuse one by. Costs four
    // full-resolution stores per pixel, so it is off on the Ray Reconstruction
    // path, which wants the composited colour and its guides and nothing else.
    //
    // Took the padding word this struct was already carrying, so the layout --
    // and the static_assert below -- is unchanged.
    int demodulate;

    // -- the neural radiance cache -------------------------------------------
    //
    // 0 off, 1 collecting training data only, 2 collecting AND querying. The
    // middle state is not a debug mode: the cache spends its first second of
    // life untrained, and a path that stops early at an untrained network puts
    // coloured fog in the wood. So the tracer trains for a while before it
    // trusts it, and nrc.h owns the decision.
    int nrcMode;
    int nrcQueryDepth;   // first bounce the cache may answer for
    int nrcTrainEvery;   // one pixel in this many writes a training record
    float nrcInvExtent;  // 1 / half-extent of the cached region, in metres

    // The cache learns positions RELATIVE TO A MOVING ORIGIN, because the world
    // is endless and a network cannot hold an unbounded coordinate. The origin
    // follows the camera in whole chunks, so the encoding stays stable while you
    // walk instead of shifting every frame and invalidating everything learned.
    float3 nrcOrigin;

    // -- ReSTIR GI -----------------------------------------------------------
    //
    // 0 off, 1 collecting candidates only, 2 collecting AND shading from the
    // resampled result. The middle state exists for the same reason the cache
    // has one: the reservoirs are empty on the first frame and for one frame
    // after any resize, and shading from an empty reservoir is a black image.
    //
    // Took the padding word the NRC block was already carrying, so the struct
    // is the same 400 bytes and the assert below is unchanged.
    int restirMode;

    // -- volumetric fog ------------------------------------------------------
    //
    // 0 falls back to no fog at all rather than to the old analytic model: the
    // analytic version is gone, and leaving a second implementation behind to
    // disagree with this one in edge cases is worse than the switch being
    // honest about what it does.
    //
    // fogNear is gone with the froxel grid. There is no slice distribution left
    // to map a pixel's depth into -- the tracer marches the world volume from
    // the camera, so all it needs is where to stop.
    //
    // THE ANISOTROPY AND THE SKY FILL ARE HERE NOW, and they were not before,
    // because the volume stores VISIBILITY and the radiance is assembled at
    // march time. The phase lobe needs the view direction, which only the
    // tracer has. See VolFog.slang.
    int volFog;
    float fogFar;
    float fogAniso;
    float fogAmbient;

    // -- HOW MUCH OF A POINT LIGHT THE AIR CARRIES ------------------------
    //
    // A WHOLE ROW FOR ONE FLOAT, and knowingly. This was a constant in
    // Trace.cs.slang whose own note said a fifth float on the fog row above
    // would cost a new 16-byte line to hold a number nothing outside that file
    // reads -- and then it was asked for on a slider, so something outside it
    // reads it. The three pads are what a cbuffer row costs; they are not
    // waste, they are the price of the one that is used.
    float sparkGlow;
    float sparkPad0;
    float sparkPad1;
    float sparkPad2;

    // Each cascade's min corner in world metres, and its cell size. Snapped to
    // a whole number of cells host-side; the shader only ever reads them.
    float3 fogOrigin0;
    float fogCell0;
    float3 fogOrigin1;
    float fogCell1;

    // -- the cloud deck --------------------------------------------------
    //
    // Marched only down the PRIMARY ray, where it is composited over the sky.
    // Doing it on every miss would put a 28-step march with a 4-step light
    // march inside it on every bounce that escapes, which is most of them.
    int clouds;
    float cloudWindT;   // wind as a lookup offset, on the day cycle clock
    float cloudSun;     // the deck brightness against this renderer illuminant
    float cloudAmb;
    // THE DECK'S OWN NIGHT KEY, and it is not moonKey.
    //
    // moonKey is 300 -- a SURFACE calibration, sized so next event estimation
    // can still land a navigable amount of light on the forest floor through a
    // daylight tone curve. The deck does not go through NEE. It multiplies the
    // key radiance directly while its ambient comes from the unboosted dome, so
    // inheriting that 300 made the two halves of one cloud disagree by the whole
    // factor and lit the deck about 550x its own sky. Its own number, so the
    // surface calibration and the deck calibration can move independently.
    float cloudMoonKey;

    // -- sky-dome next event estimation --------------------------------------
    //
    // How many dome samples a vertex takes, and how deep more than one is worth
    // paying for. Exactly the shape of shadowRays/shadowRayDepth above, and for
    // the same reason -- one more occlusion ray is far cheaper than one more
    // path.
    //
    // 0 turns the strategy off AND restores the dome to full weight on the
    // escape path, so the toggle is exact rather than approximate: with it off
    // the tracer computes the same numbers it computed before this existed.
    // See the MIS note in sampleSkySplit for why those two have to move
    // together.
    int skyRays;
    int skyRayDepth;

    // -- the irradiance cache ------------------------------------------------
    //
    // 0 none, 1 DDGI probes, 2 SHaRC. ONE knob rather than two flags, because
    // the two caches answer the same question in the same units: running both
    // would count the indirect light twice and nothing downstream could notice,
    // since neither cache can tell that the probe atlas and the hash cell
    // describe the same photons.
    int giMode;
    int giDepth;       // first bounce a cache may answer for
    float giStrength;  // scale on what it returns, 1 = as measured

    // -- the blue-noise sampler ----------------------------------------------
    //
    // Non-zero when the shallow sample dimensions come from the void-and-cluster
    // mask instead of the pixel's PCG stream. It does not change the estimator
    // and it does not change how many numbers the path draws -- see the note on
    // BlueNoise::next2DIf for why the rng is advanced either way -- so this is a
    // toggle whose two states are directly comparable.
    //
    // TOOK giPad0, so the struct is the same 528 bytes and the assert below is
    // unchanged.
    int blueNoise;
    // 1 when heldPrev below describes a real previous frame. TOOK giPad1, so
    // the rows either side of it are unchanged and the assert only moves by the
    // three rows actually added.
    int heldPrevValid;

    // -- THE NIGHT SKY'S OWN CLOCK -------------------------------------------
    //
    // Seconds, continuous, and it is NOT the frame counter. The meteors are
    // scheduled in windows of real time (see meteors() in Sky.slang), so a
    // counter would run them at whatever rate the machine happens to render
    // at -- faster on a good frame, and differently on every machine.
    //
    // A WHOLE NEW ROW, because giPad0 and giPad1 were both spent already. The
    // three words after it are the rest of that row and are the next thing to
    // take rather than growing the struct again; the assert at the bottom
    // moves by exactly one row, which is what it is there to make me notice.
    float skyTime;
    float skyPad0, skyPad1, skyPad2;

    // -----------------------------------------------------------------------
    // WHERE THE TOOL WAS, as its own object-to-world of the PREVIOUS frame --
    // three rows of a 3x4, because a float3x3 in a constant buffer is three
    // rows anyway and the translation may as well ride in the w.
    //
    // THE ONE INSTANCE THE STATIC-WORLD FORMULA CANNOT DESCRIBE AT ALL. Every
    // other surface in this scene is somewhere in the world and stays there
    // while the eye moves around it, which is what makes "this frame's hit
    // point through last frame's camera" the answer. The tool is bolted TO the
    // eye: turn, and its world point swings with you, so that formula reports
    // the largest motion in the frame for the one object that did not move on
    // screen at all.
    //
    // Zero was the old answer and it is exactly right for that -- and exactly
    // wrong for everything the tool does on its OWN. It bobs on the stride, it
    // swings, and a bow pulls through five frames of draw; all of that is real
    // screen motion, and telling Ray Reconstruction there was none is what
    // smears a walking axe. The two cannot both be had from a translation,
    // because one has to be CANCELLED and the other ADDED.
    //
    // So the tool is reprojected the way a rasteriser does it: take the hit
    // back to object space, forward through the transform it had LAST frame --
    // which was built on last frame's camera, so the eye's own motion is inside
    // it -- and project that through prevCam. Camera motion cancels because it
    // appears on both sides; the bob and the swing survive because they do not.
    //
    // Twelve numbers exactly: three rows of (three of rotation, one of
    // translation). Whether they mean anything is heldPrevValid above -- a tool
    // just drawn, or one that was hidden last frame, has no previous pose, and
    // the tracer then writes the zero it always used to.
    // -----------------------------------------------------------------------
    float4 heldPrev0, heldPrev1, heldPrev2;

    // -----------------------------------------------------------------------
    // THE WATER, AND THE CLOCK IT MOVES ON.
    //
    // waterY is the surface height in METRES, or a long way below the world
    // when the band under the camera has no water (see VoxelTerrain::waterAt).
    // The shader had no waterline at all until now, which cost two things: a
    // camera that STARTS submerged got no absorption on its first segment,
    // because `inWater` could only be set by crossing the surface; and nothing
    // could tell whether a lit point was under water, which is what caustics
    // are drawn on.
    //
    // waterTime is seconds, and it exists because the wave field is the only
    // thing in this renderer that must not be still. The ripple it replaces was
    // two sine trains of POSITION with no clock in them at all -- a lake frozen
    // mid-chop.
    // -----------------------------------------------------------------------
    float waterY;
    float waterTime;
    // WHICH WATER TERMS ARE ON. One bit each, live from the [I] panel -- see
    // kWF* below. It takes HALF THE PAD that was already here, so V6Params is
    // the same size it was and nothing that asserts on that changes.
    uint waterFlags;
    // WAVE AMPLITUDE, AS A GAIN ON kWave. 0 is dead flat.
    //
    // IT TAKES waterPad rather than growing the record, so V6Params stays on
    // whole 16-byte rows -- adding a float outright trips the static_assert
    // below, which is exactly what it is there for.
    //
    // The wave field lived entirely in the shader as a static const table, so
    // "make the water flat" had no knob at all, and flattening the HEIGHTFIELD
    // did nothing: these displace the surface after the fact, so a mapped lake
    // still carried a 5.2 m swell. v1 ships its waves off; so does this.
    float waterWaveGain;

    // -----------------------------------------------------------------------
    // THE BULB. The only emitter in this engine that is not the sky.
    //
    // IT EXISTS BECAUSE A SEALED ROOM IS BLACK. The pause room is a closed
    // white box, and this is a path tracer with exactly two lights in it -- the
    // sun and the dome -- both of which are outside. The first render of the
    // room came back as an empty frame with a crosshair on it, which is the
    // correct image of a box with no light in it.
    //
    // A POINT LIGHT, NOT AN EMISSIVE MATERIAL. Adding emission to the palette
    // would mean a per-material radiance, light sampling over triangles, and an
    // MIS partner for every BSDF bounce in the wood -- a renderer-wide change to
    // light one room. A point at a known position costs one shadow ray at the
    // vertices that can see it and nothing at all anywhere else, because
    // bulbOn is 0 in the world.
    //
    // bulbRadiance is BOTH the intensity in the inverse-square law AND the
    // radiance the camera sees off the glass. They are not the same quantity
    // and deliberately share a number: a bulb you can look straight at is
    // blown-out white in any exposure that makes the room readable, so the
    // second use only has to be "well past white" -- and tying it to the first
    // means a brighter bulb is a brighter-LOOKING bulb, with no second dial to
    // keep in step.
    // -----------------------------------------------------------------------
    float3 bulbPos;
    float bulbRadius;
    float3 bulbRadiance;
    // WHICH MATERIAL IS THE GLASS, or kNoBulb. This is also the on switch --
    // one field, so there is no state in which the light is on and the thing it
    // comes out of is not, or the other way round.
    //
    // A MATERIAL TEST, NOT A DISTANCE ONE. The first version asked whether a
    // hit was within a radius of bulbPos, and that cannot tell the glass from
    // the CORD IT HANGS ON -- the flex touches the top of the sphere, so any
    // sphere generous enough to contain the bulb's own blocky corners contains
    // the bottom of the flex too, and a pendant with a glowing string is worse
    // than no pendant. groundShade returns a model colour's index unchanged
    // ("water, and everything a model brought with it"), so the index reaching
    // the shader IS the palette entry buildRoom asked for.
    uint bulbMtl;

    // -- AND A SECOND EMITTER, WHICH IS NOT A LIGHT ------------------------
    //
    // THE FIREFLY. bulbMtl above is a material test AND a point light: the
    // shader returns radiance for a voxel wearing it, and sampleBulbSplit
    // traces a shadow ray toward bulbPos on every bounce. The first half is
    // exactly what a firefly wants; the second half means nothing for it, since
    // there are six of them drifting apart and bulbPos is ONE position.
    //
    // So this is the material test alone. A voxel wearing glowMtl renders at
    // glowRadiance, lights nothing, and costs no shadow ray -- and after dark a
    // speck of yellow in an unlit wood is the whole of the effect anyway.
    // kNoBulb is the off switch here too, so a world with no fireflies loaded
    // pays one compare.
    float3 glowRadiance;
    uint glowMtl;

    // -- ...AND THE PARTICLES, WHICH ARE THE SAME IDEA A THIRD TIME --------
    //
    // A SPARK IS A MATERIAL, NOT A LIGHT. v1 throws four bright voxels at every
    // blow that lands and four more off a kill; they live under a second, there
    // are at most twenty in the air, and each is one voxel. What they need is
    // exactly what the firefly needs and nothing more: a voxel that renders at
    // a radiance of its own, lights nothing, and costs no shadow ray.
    //
    // A TABLE RATHER THAN A THIRD PAIR OF FIELDS. The port needs TWO of these
    // -- v1's amber ember and the RED one it throws when the thing struck is
    // alive -- and a third hand-written copy of glowRadiance/glowMtl is the
    // point at which to stop writing them out one at a time.
    //
    // glowMtl STAYS WHERE IT IS. It is the firefly's, it works, and folding it
    // in here would be a change to something nobody asked to change.
    //
    // xyz IS THE RADIANCE AND w IS THE MATERIAL ID, as a float -- a material id
    // is 0..255 and a float carries those exactly. A NEGATIVE w is an empty
    // slot, and the table is filled from 0 up, so the shader stops at the first
    // one: an engine with no sparks loaded pays a single compare.
    float4 emitters[kEmitSlots];

    // -- EVERY PENDANT IN THE PLACE, AND THE NEAREST ONE LIGHTS ------------
    //
    // xyz IS THE POSITION AND w IS 1 FOR A LIVE SLOT, negative for an empty
    // one -- the emitters' own convention above, for the emitters' own reason:
    // filled from 0 up, so the shader stops at the first negative and a world
    // with no lamps pays one compare.
    //
    // bulbPos ABOVE IS STILL THE FALLBACK and is still what a single-bulb
    // caller sets. This array overrides it when it has anything in it. See
    // kBulbSlots for why the choice is made per shading point.
    float4 bulbs[kBulbSlots];

    // -----------------------------------------------------------------------
    // THE WORDS OVER THE BUTTONS. All-zero everywhere but the pause room -- see
    // V2Holo, and V2Tracer::clearHolos.
    // -----------------------------------------------------------------------
    V2Holo holo[kHoloSlots];
};

// No bulb. See V6Params::bulbMtl.
static const uint kNoBulb = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// THE WATER, TERM BY TERM.
//
// Every one of these is a thing the water does that can be looked at on its
// own, and half of them were argued about for a session apiece. Being able to
// turn one off while the others stay on is the difference between "the water
// looks wrong" and "it is the reflection".
//
// v1 does the same thing and for the same reason -- its WATER_BAKE is a row of
// these, and the note beside it says a panel that cannot speak for every
// caustic site "is a lie".
// ---------------------------------------------------------------------------
static const uint kWFGlint   = 1u << 0;  // the sun sparkle, v1's pixelGlisten
static const uint kWFCaustic = 1u << 1;  // the webs on the bed
static const uint kWFReflect = 1u << 2;  // the world in the surface, not just sky
static const uint kWFAbsorb  = 1u << 3;  // Beer-Lambert along the eye path
static const uint kWFScatter = 1u << 4;  // the in-scatter that stops it being ink
static const uint kWFSunPath = 1u << 5;  // Beer-Lambert on the way DOWN to the bed
static const uint kWFSwell   = 1u << 6;  // the voxel wave march
static const uint kWFRipple  = 1u << 7;  // the analytic normal where the march ends
static const uint kWFFoam    = 1u << 8;  // the white churn round the shore
// -- AND THE OTHER GLINT, WHICH IS NOT kWFGlint -----------------------------
//
// (user 2026-09-18: "can you give me a setting ... to turn off the suns glare
// ... its mixed in with the pixel glint on the water. I need you to seperate
// it" + "one is pixelated and one is not".)
//
// THERE ARE TWO AND THEY SHARE NOTHING BUT THE SUN. kWFGlint above is v1's
// pixelGlisten: an AUTHORED sparkle, added as emission, quantised to voxel
// cells by featureSizeM with about half of them alight at any moment. It is
// pixelated on purpose -- that discreteness is what makes it read as voxel
// water, and it is the one the checkbox has always controlled.
//
// THIS one is not authored at all. Water is MAT_DIELECTRIC with roughness 0 --
// a perfect mirror -- so it cannot be light-sampled, and nothing in NEE ever
// touches it. What happens instead is that the path bounces specularly off the
// wave normal, leaves the world, and finds the SUN DISK in the sky. Because
// every pixel's wave normal is a little different, the mirror direction
// scatters, and the disk lands across a broad region as a glitter path -- which
// the accumulator and DLSS then average into the hazy sheen. It is physically
// what a lake does, it is the brightest thing in the frame, and until now it
// had no switch of its own.
//
// OFF here means: a ray that reached the sky by a specular bounce OFF WATER
// does not collect the disk. The sky it reflects is untouched, every other
// surface's reflection of the sun is untouched, and kWFGlint is untouched.
static const uint kWFSunGlare = 1u << 9;  // the sun DISK mirrored off the waves
static const uint kWFAll     = 0x1FFu;

// EVERYTHING ON. The world reflection was taken out of the default on
// 2026-09-13 and put back the same day, and the reason it went is worth
// keeping: it was drawing a lattice of near-black blobs over every pond. That
// turned out to be Hit::waterTop -- the reflection was fine and the SURFACE it
// was measured against was the camera band's, not the pond's. With that fixed
// there is nothing to hold it back for.
//
// It costs one extra trace and one extra shadow ray per primary water pixel,
// which is the thing to look at first if the frame budget is ever tight.
static const uint kWFDefault = kWFAll;   // 0x1FF

// ---------------------------------------------------------------------------
// THE THING IN YOUR HAND.
//
// A held item is a small voxel grid drawn in CAMERA space -- it hangs off the
// eye rather than standing anywhere in the world -- so what crosses to the GPU
// is a box centre and the three axes of the box, all expressed in the camera's
// own frame. Held.slang walks the grid with a DDA.
//
// IT IS NOT SHADED SEPARATELY. The DDA runs inside the tracer, at the primary
// vertex, and what it produces is an ordinary Hit: a position, a face normal
// and a material id out of the SAME table every rock and trunk indexes. From
// there the path continues exactly as it would off a boulder -- sun and sky
// next-event estimation, indirect bounces, the radiance cache, the guide
// buffers, the denoiser. There is no second lighting model to keep in step
// with the first, which is the whole reason it was moved in here: an analytic
// one, however carefully matched, is a second answer to a question the
// renderer already answers, and it reads as a sticker over the wood.
//
// AXES RATHER THAN A QUATERNION OR EULER ANGLES, and it is the same choice the
// JS engine made for the same reason: the shader needs the three axes and
// nothing else, and building them on the host means the trigonometry happens
// once a frame instead of once a pixel.
//
// THE GRID IS THE .vox FILE'S OWN LAYOUT -- x wide, y deep, z tall, indexed
// x + y*w + z*w*d -- and NOT the y-up world layout scene/vox.h converts to.
// The poses in render/helditem.h were tuned against that layout in the engine
// this is ported from; running them against a converted grid lies the axe on
// its side.
// ---------------------------------------------------------------------------
struct V2Held
{
    float3 anchor; // centre of the item's box, CAMERA space, metres
    float voxel;   // metres along an edge of one item voxel

    float3 ax;  // the item's local +x (width), in camera space
    float show; // 0 = empty hand, and the tracer never tests it

    float3 ay;    // local +y (depth)
    float radius; // bounding sphere about `anchor`, metres -- the cheap reject

    float3 az;  // local +z (height)
    float pad0;

    int3 dims;  // the grid, in voxels
    int steps;  // DDA step ceiling -- the grid's own extent, from the host
};

#ifdef HOST_CODE
static_assert(sizeof(V2Held) == 80, "V2Held must fill whole 16-byte rows");
static_assert(sizeof(V6Camera) == 64, "V6Camera must fill whole 16-byte rows");
static_assert(sizeof(V6Sky) == 192, "V6Sky must fill whole 16-byte rows");
static_assert(sizeof(V6Material) == 32, "V6Material must fill whole 16-byte rows");
// 64, UP FROM 48, and the row is the hit flash -- see V6Instance::hurt.
static_assert(sizeof(V6Instance) == 64, "V6Instance must fill whole 16-byte rows");
static_assert(sizeof(V2Holo) == 112, "V2Holo must fill whole 16-byte rows");
// 672, UP FROM 656, and the latest sixteen are sparkGlow and its three pads
// -- one float on a slider, one row. See V6Params::sparkGlow.
// 656, UP FROM 640, and those sixteen are the firefly's glowRadiance + glowMtl
// -- one row, because they are packed as the float3 + uint pair immediately
// above them rather than as a lone uint that would cost a row to padding.
// ...AND THE HOLO SLOTS ARE COUNTED RATHER THAN TYPED. It said 3, and adding a
// fourth for the hand's stack badge broke this line rather than the struct --
// which is the assert doing its job, and also a number that should never have
// needed a person to notice. kHoloSlots is the one place that decides.
// ...AND THE EMITTER TABLE IS COUNTED THE SAME WAY, for the same reason: it is
// four float4s today and the number lives in exactly one place.
static_assert(sizeof(V6Params) == 672 + kEmitSlots * 16 + kBulbSlots * 16 + kHoloSlots * 112,
              "V6Params must fill whole 16-byte rows");
static_assert(sizeof(V6DdgiConsts) == 112, "V6DdgiConsts must fill whole 16-byte rows");
} // namespace v2
#endif
