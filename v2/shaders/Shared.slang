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
static const uint kSoil0 = 13;
static const uint kSoilCount = 4;
static const uint kLitter0 = 17;
static const uint kLitterCount = 3;

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
    float pad0, pad1;
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
    float flapPad;
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
};

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
static_assert(sizeof(V6Instance) == 48, "V6Instance must fill whole 16-byte rows");
static_assert(sizeof(V6Params) == 592, "V6Params must fill whole 16-byte rows");
static_assert(sizeof(V6DdgiConsts) == 112, "V6DdgiConsts must fill whole 16-byte rows");
} // namespace v2
#endif
