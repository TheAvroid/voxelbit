// ---------------------------------------------------------------------------
// Trace.cs.slang -- the whole renderer, as one compute shader.
//
// THIS FILE IS THE POINT OF v2. The integrator is v2's, unchanged in every
// decision that affects the picture: unidirectional paths, next-event
// estimation to the sun's disk, MIS against the BSDF, analytic height fog on
// the primary segment. What changed is that it stopped being five programs and
// a binding table and became one kernel.
//
// WHY THAT IS WORTH DOING. OptiX -- and DXR's pipeline mode with it -- is built
// around a shader binding table: geometry is bound to hit-group records, the
// driver dispatches into them on a hit, and the shading code lives in a program
// the caller never calls directly. That design earns its keep when different
// geometry needs genuinely different shading code. This scene does not have
// that. It has ONE hit path, and every hit does the same three things: look up
// a packed uint16, expand it to a normal and a material, come back.
//
// So the whole table was overhead. It cost:
//
//   * A RECORD PER RESIDENT CHUNK, allocated from a fixed slot pool, published
//     over the bus whenever the ring moved, with the slot lifetime hand-managed
//     against eviction. That is gone entirely -- an instance now carries an
//     offset into one shared buffer, and the offset is just a number in the
//     instance record the acceleration structure already needed.
//
//   * A PIPELINE TO LINK AND A STACK TO SIZE. v2 had to flatten its path loop
//     into raygen specifically so the continuation stack would not be sized for
//     the deepest path times every thread in flight. Inline ray tracing has no
//     continuation stack at all: the loop below is an ordinary loop in an
//     ordinary function, and the ray is a call.
//
//   * A SEPARATE SHADOW RAY TYPE with its own miss program, existing only to
//     clear a payload register. Here it is a RayQuery with
//     ACCEPT_FIRST_HIT_AND_END_SEARCH and no shading of any kind.
//
// What is left reads like the CPU tracer this lineage started as, which is the
// real argument: the shape of the code now matches the shape of the algorithm.
//
// ALL GEOMETRY IS DECLARED OPAQUE, which is what lets Proceed() return
// immediately -- there is no any-hit work to run, so traversal completes inside
// the RT cores and the shader never re-enters it. Foliage is cut out by its
// voxels, not by an alpha test, so there is nothing an any-hit could do.
// ---------------------------------------------------------------------------

#include "Shared.slang"
#include "Vecmath.slang"
#include "BlueNoise.slang"
#include "Material.slang"
#include "Sky.slang"
#include "Restir.slang"
#include "VolFog.slang"
#include "Clouds.slang"
#include "Ddgi.slang"

// The hash cache. Included in BOTH of this file's compilations -- the query one
// that the camera uses and the update one that fills it -- because they share
// every helper below and only differ in which entry point they end at.
#if V2_SHARC_QUERY || V2_SHARC_UPDATE
#if V2_SHARC_UPDATE
#define SHARC_UPDATE 1
#else
#define SHARC_QUERY 1
#endif
#include "Sharc.slang"
#endif

// THE CACHE IS COMPILED IN ONLY WHERE IT CAN RUN. Nrc.slang uses CoopVec, which
// does not exist on a D3D12 target below Shader Model 6.10 -- including it
// unconditionally does not disable the cache, it makes the WHOLE TRACER fail to
// compile, and the engine dies at startup with a shader error that mentions
// nothing about neural anything. The host sets this define only after it has
// confirmed the capability; see gpu/neural.h and gpu/tracer.h.
#ifdef V2_NRC
#include "Nrc.slang"
#endif

// NVIDIA's cache, which is a different thing from the one above and is
// mutually exclusive with it -- both answer "what leaves this point", so
// running the pair would add the indirect twice with nothing to notice.
// gpu/tracer.h sets at most one of the two defines.
#ifdef V2_NRCSDK
#include "NrcSdk.slang"
#endif

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
RaytracingAccelerationStructure gScene;

// packTri(material, face), one uint16 per triangle, for every instance in the
// scene at once. An instance's triOffset plus the primitive index addresses it.
// See world.h for the allocator that hands out the ranges.
ByteAddressBuffer gTriPool;

StructuredBuffer<V6Instance> gInstances;
StructuredBuffer<V6Material> gMaterials;

// The radiance cache: 14 KB of weights, and the buffer the tracer writes its
// training records into. Both are bound every frame whether or not the cache is
// on -- see the note on gDiffRadiance below for why an unbound declared
// resource is a dispatch-time validation error rather than a dead branch.
#ifdef V2_NRC
ByteAddressBuffer gNrcWeights;
RWStructuredBuffer<NrcSample> gNrcSamples;
#endif

// ReSTIR needs no special hardware -- it is plain Slang and compiles on both
// backends -- so unlike the cache it is bound unconditionally and switched at
// runtime. gRestirFinal is LAST frame's resampled result, which is what this
// frame shades from; gRestirCandidate is where this frame's own bounce goes.
RWStructuredBuffer<Reservoir> gRestirCandidate;
StructuredBuffer<Reservoir> gRestirFinal;
RWTexture2D<float4> gPrimaryPos;

// The world-anchored fog volume, two cascades. R is sun visibility, G is sky
// visibility, A is density -- NOT radiance. What a parcel of air can see is a
// property of the air; how bright it looks is a property of where you are
// standing, so the radiance is assembled below, at march time, where the view
// direction exists. See the long note in VolFog.slang.
Texture3D<float4> gFogVol0;   // near cascade, fine cells
Texture3D<float4> gFogVol1;   // far cascade, coarse cells
SamplerState gFogSampler;

// The sky-view table, when the atmosphere path is on. Bound unconditionally --
// atmosphere.h creates the texture whether the feature is enabled or not -- so
// there is no unbound-resource case to guard; sky.atmoOn decides whether
// skyDome() actually reads it.
Texture2D<float4> gAtmoSkyView;
SamplerState gAtmoSamp;

// The cloud density cache -- a TILE of an endless deck, so the march samples it
// at frac(world / tile) and it never needs refilling as the player moves. Filled
// The moon's face. A photograph, sampled flat -- see moonDisk in Sky.slang.
Texture2D<float4> gMoonTex;
SamplerState gMoonSamp;

// once by CloudFill; this shader only ever reads it.
Texture3D<float> gCloudVol;
SamplerState gCloudSamp;

// -- the irradiance probes ---------------------------------------------------
//
// Bound every frame whether or not the volume is running, for the same reason
// the radiance cache is: a declared resource with nothing behind it is a
// validation error at dispatch time, not a dead branch. With DDGI off these
// point at a 1x1 placeholder the tracer owns and gParams.giMode is 0, so
// nothing ever reads them.
Texture2DArray<float4> gDdgiIrradiance;
Texture2DArray<float4> gDdgiDistance;
Texture2DArray<float4> gDdgiProbeData;
SamplerState gDdgiSampler;

cbuffer gDdgiCB
{
    V6DdgiConsts gDdgiConsts;
};

// The film. accum holds sums with the sample count in .w, so a resolve never
// has to consult a second buffer to know what to divide by.
RWTexture2D<float4> gAccum;
RWTexture2D<float4> gColor;

// ---------------------------------------------------------------------------
// The guide buffers DLSS Ray Reconstruction reads.
//
// EVERY ONE OF THESE IS A BY-PRODUCT OF THE PRIMARY HIT, and that is the whole
// reason this is cheap. A rasteriser has to draw the scene a second time to
// produce a G-buffer; here the first bounce of the path already knows the
// position, the normal, the material and the distance, and writing them out is
// six stores in a branch that runs once per pixel.
//
// Ray Reconstruction is not a denoiser bolted on after the fact -- it replaces
// the accumulation entirely. It is trained on exactly this: a one-sample path
// traced image plus the albedo and geometry that generated it, from which it
// can separate what is noise in the LIGHTING from what is detail in the
// SURFACE. That distinction is the thing a spatial denoiser cannot make, and it
// is why v2 removed every denoiser it tried: on a voxel canopy they turned
// thousands of needle-sized faces to felt, because the felt and the needles
// look the same once you have thrown the albedo away.
// ---------------------------------------------------------------------------
RWTexture2D<float4> gGuideAlbedo;    // diffuse albedo of the primary surface
RWTexture2D<float4> gGuideSpecular;  // specular albedo, Fresnel at this angle
RWTexture2D<float4> gGuideNormRough; // world normal in xyz, linear roughness in w
RWTexture2D<float> gGuideDepth;      // NDC depth, matching the projection below
RWTexture2D<float2> gGuideMotion;    // render pixels, this frame -> last frame

// ---------------------------------------------------------------------------
// PHASE B -- the demodulated channels.
//
// The guides above are what DLSS Ray Reconstruction reads, and RR does its own
// demodulation internally from them. These four are the OTHER consumer: NRD,
// and anything else that has to be handed lighting with the texture already
// divided out. They are written only when gParams.demodulate is set, so the
// Ray Reconstruction path -- which wants the composited colour and its guides,
// and nothing else -- pays nothing for them.
//
// WHY THE SPLIT HAS TO HAPPEN HERE AND NOT IN THE PASS. A demodulation pass
// downstream can divide a composited image by an albedo, but it cannot take
// radiance apart into the lobe that produced it: once diffuse and specular have
// been summed into one pixel, which fraction belonged to which is gone. That
// fraction is known exactly at the vertex where the BSDF was evaluated, and
// nowhere else -- so the SPLIT is made here and the DIVISION is the pass.
//
// gEmission is what no albedo modulates at all: sky seen directly down the
// primary ray, and the light the haze scatters in on the way. It has to be held
// out or the remultiply at the end would tint the sky with whatever surface
// happened to be behind it.
// ---------------------------------------------------------------------------
RWTexture2D<float4> gDiffRadiance; // diffuse-lobe radiance, still modulated
RWTexture2D<float4> gSpecRadiance; // specular-lobe radiance, still modulated
RWTexture2D<float4> gEmission;     // modulated by nothing: sky, and in-scatter
// The exact divisor for gDiffRadiance: the surface albedo with NO lobe weight
// folded in. Deliberately NOT gGuideAlbedo, which carries an extra (1 - kt) for
// Ray Reconstruction's benefit -- the translucency lobe is modulated by kt
// rather than (1 - kt), and both lobes land in this one channel, so the only
// factor they share is the albedo itself. Dividing by anything else would not
// invert.
RWTexture2D<float4> gDemodAlbedo;

cbuffer gParamsCB
{
    V6Params gParams;
};

// ---------------------------------------------------------------------------
// The face direction was recorded when the quad was emitted, so the normal is
// exact rather than reconstructed -- no cross product, and no need to keep a
// vertex buffer readable after the structure is built.
//
// Material in the low byte, face direction in the high byte, matching packTri()
// in voxelworld.h.
// ---------------------------------------------------------------------------
uint loadPackedTri(uint triOffset, uint primitiveIndex)
{
    // The pool is addressed in uint16 units; a ByteAddressBuffer loads 32 bits
    // from a 4-byte-aligned offset, so take the dword containing it and pick
    // the half.
    const uint byteOfs = (triOffset + primitiveIndex) * 2u;
    const uint dword = gTriPool.Load(byteOfs & ~3u);
    return ((byteOfs & 2u) != 0u) ? (dword >> 16) : (dword & 0xFFFFu);
}

// ---------------------------------------------------------------------------
// WHICH SHADE OF GROUND THIS VOXEL IS.
//
// A pine's bark is half a dozen browns scattered voxel by voxel, and that
// scatter is most of why a trunk reads as bark rather than as a painted
// cylinder. The ground used to pick its shade from a three-metre noise field
// instead, so it read as patches of one green beside patches of another -- the
// same six colours, arranged like a map instead of like a material.
//
// So the mesher stores only which FAMILY a face belongs to -- grass, soil,
// litter -- and the shade is drawn here, from a hash of the voxel the ray
// landed in. That is one integer hash on a path that already has the hit point
// in hand, and it is strictly cheaper than the alternative: a material that
// changed every three metres broke the top-face merge at every patch boundary,
// so naming only the family leaves longer runs and FEWER triangles than the
// patches cost.
// ---------------------------------------------------------------------------
uint hashVoxel(int3 v)
{
    // The same avalanche as ihash3 in core/noise.h. It does not have to agree
    // with the host -- nothing on the host asks this question -- but there is
    // no reason to invent a second one.
    uint h = uint(v.x) * 374761393u + uint(v.y) * 1103515245u + uint(v.z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// TAKES THE WHOLE PACKED WORD, not just the material byte, because a grass
// blade's shade is not drawn from the same place the ground's is -- see the
// gradient below.
uint groundShade(uint packed, float3 p, float3 ng)
{
    const uint mtl = triMaterial(packed);
    uint base, count;
    if (mtl >= kGrass0 && mtl < kGrass0 + kGrassCount)
    {
        base = kGrass0;
        count = kGrassCount;
    }
    else if (mtl >= kBGrass0 && mtl < kBGrass0 + kBGrassCount)
    {
        base = kBGrass0;
        count = kBGrassCount;
    }
    else if (mtl >= kWheat0 && mtl < kWheat0 + kWheatCount)
    {
        base = kWheat0;
        count = kWheatCount;
    }
    else if (mtl >= kBWheat0 && mtl < kBWheat0 + kBWheatCount)
    {
        base = kBWheat0;
        count = kBWheatCount;
    }
    else if (mtl >= kSoil0 && mtl < kSoil0 + kSoilCount)
    {
        base = kSoil0;
        count = kSoilCount;
    }
    else if (mtl >= kLitter0 && mtl < kLitter0 + kLitterCount)
    {
        base = kLitter0;
        count = kLitterCount;
    }
    else if (mtl == kRock)
    {
        // ROCK IS ONE ID AND SIX SHADES. The mesher writes kRock for a whole
        // hillside so its faces merge into long runs; the variation is spent
        // here instead, per voxel, exactly as grass and soil already do it.
        base = kStone0;
        count = kStoneCount;
    }
    else if (mtl == kSand)
    {
        // SAND IS ONE ID AND FOUR SHADES, for the reason ROCK is -- and on a
        // beach the reason is sharper. bankShaped flattens the shore on
        // purpose, so a beach is a wide dead-level plane; one flat colour over
        // that reads as painted board, which is what "the white bank" was.
        base = kSand0;
        count = kSandCount;
    }
    else
    {
        return mtl; // water, and everything a model brought with it
    }

    // HALF A VOXEL ALONG THE NORMAL, so the sample lands INSIDE the voxel the
    // face belongs to rather than exactly on its boundary -- where the floor of
    // a coordinate is a coin toss and every edge in the frame would shimmer
    // between two shades as the camera moved.
    const int3 v = int3(floor((p - ng * (0.5f * kVoxelM)) / kVoxelM));

    // -----------------------------------------------------------------------
    // A BLADE IS A GRADIENT, NOT A SCATTER.
    //
    // The hash above is right for the ground, which is a material seen from
    // above: what varies over a lawn is the plant, and the plant changes from
    // one tuft to the next. A standing blade is the opposite case. Its base is
    // buried in its own sward and sees almost no sky; its tip sees all of it.
    // Scattering shades up a blade paints that away and leaves a green stick.
    //
    // So a strand reads the ramp ALONG ITS HEIGHT -- kGrass0 at the soil, the
    // lightest green at the tip -- and the ramp is already sorted that way,
    // being the pines' own foliage taken in order of luminance.
    //
    // MEASURED FROM THE GROUND, NOT NORMALISED PER BLADE, which is a choice
    // and not a shortcut. Normalising would make every strand span the whole
    // ramp, so a three-voxel blade and a six-voxel one standing side by side
    // would disagree about the colour of the same height -- per-blade paint
    // rather than a light field. Absolute, they agree everywhere, short blades
    // stay dark because a short blade IS in shadow, and only the ones tall
    // enough to clear the sward carry a lit tip.
    //
    // The row comes back from the three bits the mesher stored: modular
    // arithmetic, so it is right below sea level too, where v.y is negative.
    // -----------------------------------------------------------------------
    const uint strand = triStrand(packed);
    // ALL FOUR BLADE RAMPS READ ALONG THE BLADE. `base` and `count` already say
    // which one this is, so the gradient is written once for every wood and for
    // both the green and the gone-over -- the straw ramps are the same
    // mechanism on a taller blade with a longer ramp (ten shades instead of
    // six, so it reaches a tuft's tip rather than running out halfway up it).
    if (strand != 0u &&
        (base == kGrass0 || base == kBGrass0 || base == kWheat0 || base == kBWheat0))
    {
        // HALVED, THE SAME WAY THE HOST HALVED THE BASE -- see the note over
        // STRAND_MAX_ROWS in voxelworld.h. `>>` on a signed int is arithmetic,
        // so this is floor division and the two agree at every y.
        // MODULO kStrandPhases, NOT MASKED. The host encodes the blade's base
// row the same way -- see STRAND_PHASES in voxelworld.h -- because zero
// is the "not a blade" sentinel and only 31 codes are left. Masking by
// the field gave the host 1..32, and one ground row in thirty-two packed
// onto the sentinel and dropped out of this branch entirely.
        const uint strandBase = (strand - 1u) % kStrandPhases;
        uint row = (uint(int(v.y) >> kStrandRowShift) + kStrandPhases - strandBase)
                   % kStrandPhases;
        // -- ...AND THE STRAW STILL PAIRS ITS ROWS ----------------------
        //
        // Ten shades over a fifteen-to-twenty voxel tuft: at one shade per
        // voxel it would reach its lightest halfway up and stand there, which
        // is exactly what the note over these ramps argues against. The green
        // ramps are six shades over a six-voxel blade and want every voxel.
        // Both read the SAME stored base -- the host now keeps it at full
        // resolution, so the halving is free to live on this side.
        if (base == kWheat0 || base == kBWheat0) row >>= 1;
        return base + min(row, count - 1u);
    }

    // -- ...AND THE FLOOR DOES NOT GET THE WHOLE RAMP UNDER THE OAKS ------
    //
    // (user 2026-09-17: "make the terrain in the oak forest match the same
    //  green as the grass strands.")
    //
    // The ground and the grass were ALREADY the same ramp and still did not
    // match, because the two halves of this function read it differently. The
    // gradient above walks a blade from the soil up, and short grass is 3 to 6
    // voxels -- so at one shade per STRAND_ROW_STEP a blade stops at shade 2
    // and the top three greens are ones nothing standing in the wood wears.
    // The scatter below then handed the floor all six, and half the ground
    // came out a green with no grass to match it: measured 0.3048 albedo
    // luminance against the blades' 0.2358, 29% brighter.
    //
    // So the floor scatters over what the sward REACHES. That is the gradient's
    // own argument, not a correction to it -- this ramp is a light field over
    // height, and ground between blades sits at the height of their buried
    // bases, not at their tips.
    //
    // ONLY THE SCATTER. `count` is left alone above so the gradient still has
    // the whole ramp to climb, which is what a tall blade would need if the
    // grass ceiling ever rises -- and mat::BGRASS_FLOOR_COUNT would then rise
    // with it, by construction.
    const uint scatter = (base == kBGrass0) ? kBGrassFloorCount : count;
    // -- ...AND THE FLOOR UNDER THE OAKS DEALS THEM A BLADE'S OWN ODDS -
    //
    // (user 2026-09-17: "take the terrain grass, and make it match the
    //  same colors as the grass strand grass".)
    //
    // SAME PAINTS, DIFFERENT RECIPE, DIFFERENT COLOUR. Narrowing the
    // floor to the three shades a blade can reach made the two share a
    // palette and not an appearance: a uniform scatter mixes them
    // 33/33/33 while a blade lays them down its own height, two voxels
    // per shade from the soil, so a short blade never reaches the top
    // one at all. Measured, the ground came out 15.5% brighter than the
    // grass standing in it.
    //
    // The weights are COUNTED off the blade heights the oak actually
    // grows -- see VoxelTerrain::bladeShadeWeight, which derives them at
    // compile time and is asserted against these. 8 / 7 / 3 of 18.
    if (base == kBGrass0)
    {
        const uint wt[6] = {kBGrassFloorW0, kBGrassFloorW1, kBGrassFloorW2,
                            kBGrassFloorW3, kBGrassFloorW4, kBGrassFloorW5};
        uint tot = 0u;
        for (int wi = 0; wi < 6; ++wi) tot += wt[wi];
        uint w = hashVoxel(v) % max(tot, 1u);
        uint pick = 0u;
        for (int wj = 0; wj < 6; ++wj)
        {
            if (w < wt[wj]) { pick = uint(wj); break; }
            w -= wt[wj];
        }
        return base + pick;
    }
    return base + hashVoxel(v) % scatter;
}

float3 faceNormal(uint dir)
{
    switch (dir)
    {
    case 0: return float3(0.0f, 1.0f, 0.0f);  // +Y
    case 1: return float3(0.0f, -1.0f, 0.0f); // -Y
    case 2: return float3(1.0f, 0.0f, 0.0f);  // +X
    case 3: return float3(-1.0f, 0.0f, 0.0f); // -X
    case 4: return float3(0.0f, 0.0f, 1.0f);  // +Z
    default: return float3(0.0f, 0.0f, -1.0f);
    }
}

// ---------------------------------------------------------------------------
// Push a ray start off the surface along the geometric normal, on the side it
// is leaving by.
//
// THE OFFSET IS MEASURED IN FLOAT PRECISION, NOT IN METRES, and getting that
// wrong is how a wood ends up with black grass.
//
// A constant epsilon is wrong here: the world runs to kilometres and a float
// coordinate loses precision as it grows, so an offset that clears the surface
// at the origin sits inside it a mile out. The previous form scaled with the
// point's magnitude -- correct in shape -- but at 1e-4 per metre, which is
// enormous. At a kilometre from the origin it pushed the ray start a WHOLE
// VOXEL off the surface, and at three kilometres, three voxels.
//
// In open ground nobody would notice. In grass, a blade is one voxel wide and
// its neighbours are four away, so every shadow ray was starting inside or
// behind the next blade along -- occlusion that is not there, reported as
// shadow. The floor went black, and it went blacker the further you walked from
// the origin, which is why it only appeared once the spawn stopped being fixed.
//
// What actually has to be cleared is a few units in the last place. An ULP at
// distance d is about d * 2^-23, so 17 of them is d * 2e-6: six millimetres at
// three kilometres rather than thirty centimetres, and the same 17 ULP of
// margin everywhere. The floor keeps it safe close to the origin, where the
// relative term underflows.
// ---------------------------------------------------------------------------
float3 offsetRay(float3 p, float3 ng, float3 dir)
{
    const float scale = max(3e-5f, 2e-6f * maxComp(abs(p)));
    return p + faceforward3(ng, dir) * scale;
}

// ---------------------------------------------------------------------------
// Projecting a world point back into a camera, in pixels.
//
// The exact inverse of the ray construction in main(), and it lives beside it
// so the two cannot drift: if the camera basis ever changes shape, both ends
// fail to compile together rather than one end quietly producing motion vectors
// for a camera the renderer no longer has.
//
// Returns false behind the camera, where the perspective divide flips sign and
// the answer would be a plausible-looking pixel in the wrong half of the frame.
// ---------------------------------------------------------------------------
bool projectToPixel(V6Camera c, float3 p, uint2 dim, out float2 outPix)
{
    outPix = float2(0.0f, 0.0f);
    const float3 d = p - c.pos;
    const float zc = dot(d, c.w);
    if (zc <= 1e-4f)
        return false;
    const float sx = dot(d, c.u) / (zc * c.halfW);
    const float sy = dot(d, c.v) / (zc * c.halfH);
    outPix = float2((sx + 1.0f) * 0.5f * float(dim.x), (1.0f - sy) * 0.5f * float(dim.y));
    return true;
}

// Depth as a rasteriser would have left it: the standard DirectX perspective
// mapping of view-space z into [0,1]. DLSS is told it is hardware depth, so it
// has to be that shape and not linear metres -- and because the projection is
// written out here rather than inherited from a matrix nobody owns, it is
// guaranteed to be the same projection the motion vectors were made with.
float ndcDepth(float viewZ, float n, float f)
{
    if (viewZ <= n)
        return 0.0f;
    return saturate((f / (f - n)) * (1.0f - n / viewZ));
}

// ---------------------------------------------------------------------------
// What a hit turned out to be. The equivalent of v2's PathVertex, except that
// it is a local variable in the caller rather than something reached through
// two payload registers holding a pointer.
// ---------------------------------------------------------------------------
struct Hit
{
    bool hit;
    float t;
    float3 p;
    float3 ng; // geometric, for offsetting the next ray
    float3 ns; // shading, for the BSDF frame
    // The tool in the player's hand, which the motion vector has to know about
    // and nothing else does. See the write in the guide block.
    bool held;
    // ...and whether this surface is an animal that has just been struck, which
    // only the emission at the bottom of the path loop reads. See
    // V6Instance::hurt.
    float hurt;
    // ...and how far this surface's instance travelled since the last frame,
    // which the motion vector is the only reader of as well. Zero for all of
    // the wood; a butterfly is the only thing in this scene that moves through
    // it. See KIND_FLYER in Shared.slang.
    float3 moved;
    // Which material this face actually wears, after groundShade. Zero when
    // the ray hit nothing.
    uint mtl;
    // -----------------------------------------------------------------------
    // THE STILL SURFACE THIS WATER HIT BELONGS TO, in world y. Only meaningful
    // when the hit IS water.
    //
    // WHY IT CANNOT BE gParams.waterY, WHICH IS WHAT IT USED TO BE. That
    // uniform is one scalar for the whole frame -- `terrain.waterAt(pos.x)`,
    // the waterline of the band the CAMERA is standing in. The world's bands
    // are 800 m wide and their lines are 33.0 m (pine) and 5.5 m (birch), so a
    // pond twenty-seven metres away in y is routinely being measured against
    // the wrong surface.
    //
    // It drew as a REGULAR LATTICE OF NEAR-BLACK BLOBS across a pond, which is
    // the artifact in the user's debug video, and the tiling is the swell's own
    // 5.2 m wavelength showing through: the crest-exit push below fires only
    // where the marched crest comes out ABOVE this number, so the wave field
    // decides which patches of water get pushed and which do not. Bisected by
    // deploying a shader that marches but does not move the hit -- 6.05% of the
    // frame against the bug, 1.5% against the swell turned off entirely.
    //
    // The quad's own y is right here and free: marchWaterVoxels already takes
    // it as `baseTop`.
    // -----------------------------------------------------------------------
    float waterTop;
    // HOW MUCH WHITE CHURN IS ON THIS BIT OF WATER. 0 everywhere that is not
    // shore, and never 1 even where it is -- see foamMix.
    //
    // On the Hit rather than folded into the albedo because foam is not a
    // colour the water IS, it is something floating ON it: the path loop has
    // to take its share of the light BEFORE the surface splits into reflection
    // and refraction. Folded into albedo it would be foam seen THROUGH the
    // water. See the foam block in the dielectric branch.
    float foam;
    Surface mat;
};

#ifdef V2_NRCSDK
// ---------------------------------------------------------------------------
// v2's Hit, described the way NVIDIA's cache wants it.
//
// LIVES HERE AND NOT IN NrcSdk.slang, because `Hit` and MAT_DIELECTRIC are the
// tracer's own types and that header is also included by NrcSdkResolve, which
// has no path tracer in it. Putting this there compiled fine from the tracer
// and then failed to link the resolve pass with "undefined identifier 'Hit'",
// out of a file that never mentions one.
//
// THE SPECULAR TERM IS A SCALAR HERE AND A COLOUR THERE. v2's materials carry
// one specular number -- this wood is dielectric bark, needles, stone and
// water, none of which is a coloured metal -- so the F0 handed over is that
// number greyed out. Inventing a tinted F0 would describe a material the engine
// does not have.
//
// isDeltaLobe IS THE WATER, and it matters more than it looks. A delta surface
// has no radiance to cache: the cache stores what leaves a point in all
// directions and a mirror leaves in exactly one. Marking it lets the library
// skip the vertex rather than fit noise to it.
// ---------------------------------------------------------------------------
NrcSurfaceAttributes nrcSdkSurface(Hit h, float3 rayDir)
{
    NrcSurfaceAttributes sa;
    sa.encodedPosition = NrcEncodePosition(h.p, gNrcConstants);
    sa.roughness = h.mat.roughness;
    sa.specularF0 = float3(h.mat.specular, h.mat.specular, h.mat.specular);
    sa.diffuseReflectance = h.mat.albedo;
    sa.shadingNormal = h.ns;
    // TOWARDS the viewer, which is the opposite of the ray. Handing over the
    // ray direction instead is a sign error the cache cannot detect: it simply
    // learns the wood lit from behind.
    sa.viewVector = -rayDir;
    sa.isDeltaLobe = (h.mat.type == MAT_DIELECTRIC);
    return sa;
}
#endif

// `mask` is which of the two worlds this ray can see -- see MASK_WORLD and
// MASK_HELD in Shared.slang. Everything but the primary ray passes both.
// ---------------------------------------------------------------------------
// THE WATER SURFACE, PORTED FROM v1.
//
// GERSTNER, NOT TWO SINE TRAINS. What was here was sin(x) + sin(z) with no
// clock in it at all -- a lake frozen mid-chop, and one whose crests ran along
// the axes because that is what a sum of two axis-aligned trains does. This is
// v1's own table (render/buffers.js, GW): four trains, their directions
// deliberately non-parallel, wavelengths 5.2 / 2.3 / 1.1 / 0.67 m.
//
// IT CONVERTS EXACTLY. v1 measures in VOXELS and this in METRES, and the term
// that matters is k * amplitude, which is dimensionless -- (2pi/52) * 1.30 in
// voxels is (2pi/5.2) * 0.130 in metres, the same 0.157. The dispersion is the
// deep-water relation w = sqrt(g k) with v1's own 0.21 on the front, which is
// what halved the speed there ("water 50% slower"); it comes out identical
// because 98 per voxel IS 9.8 per metre.
//
// THE Q TERM PINCHES THE CRESTS. Without it a Gerstner sum is a sine sum with
// extra steps: Q is what makes the troughs broad and the crests sharp, which is
// most of what separates water from corrugated iron.
// ---------------------------------------------------------------------------
// THE WAVE FIELD, DECLARED ONCE.
//
// waterHeight and waterNormal are the same surface -- one is the derivative of
// the other -- so the components cannot live in two tables. They did, and a
// drift between them would tilt the shading off the geometry with nothing to
// catch it.
//
// v1'S TABLE, AT v1'S SCALE. Its GW is written in VOXELS -- wavelengths
// 52/23/11/6.7 and amplitudes 1.30/0.72/0.34/0.20 -- so at 10 cm that is
// 5.2/2.3/1.1/0.67 m and 0.130/0.072/0.034/0.020 m, summing to 0.256 m.
//
// THESE WERE DOUBLED, AND THE REASON GIVEN FOR DOUBLING THEM WAS WRONG. The
// note said a quarter-metre ripple is invisible at this range, so the waves
// were scaled 2x. But nothing about the swell was visible at the time for a
// different reason entirely -- the surface was a smooth shading normal on a
// flat quad, and no amplitude makes a bump map grow a silhouette. The size
// was never the problem; the missing geometry was. Scaled back, because
// matching v1 is the point and v1's water is the reference.
//
// Steepness A*k stays what v1 chose: scaling both together is a no-op on it.
//
//   x, z, wavelength (m), amplitude (m)   -- 0.256 m total, v1 exactly
// ---------------------------------------------------------------------------
static const float4 kWave[4] = { float4( 0.834f,  0.552f, 5.20f, 0.130f),
                                 float4(-0.416f,  0.909f, 2.30f, 0.072f),
                                 float4( 0.966f, -0.259f, 1.10f, 0.034f),
                                 float4( 0.309f, -0.951f, 0.67f, 0.020f) };
static const float kWaveQ[4]  = { 0.55f, 0.50f, 0.45f, 0.40f };
static const float kWavePH[4] = { 0.0f, 2.1f, 4.4f, 1.3f };

// ---------------------------------------------------------------------------
// THE WAVE HEIGHT, which is the field waterNormal has always been the
// DERIVATIVE of. Displacement about the mean waterline, in metres.
//
// Same four components, same phases, same deep-water relation -- so the
// surface this returns and the normal that shades it cannot disagree. The
// amplitudes sum to 0.256 m, which is what VoxelTerrain::waveVoxMax has to
// clear.
// ---------------------------------------------------------------------------
float waterHeight(float2 p, float t)
{
    float h = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float k  = 6.2831853f / kWave[i].z;
        const float om = 0.21f * sqrt(9.8f * k);
        h += gParams.waterWaveGain * kWave[i].w * cos(k * (kWave[i].x * p.x + kWave[i].y * p.y) - om * t + kWavePH[i]);
    }
    return h;
}

// ---------------------------------------------------------------------------
// WHERE THE RAY REALLY MEETS THE WATER -- v1's VOXELISED COLUMN MARCH.
//
// The structure holds only a flat lid over the lake. The surface is found here
// instead, and it is found the way v1 finds it: by walking COLUMNS along the
// ray, evaluating the Gerstner field at each, and FLOORING that height to the
// voxel grid.
//
// THE FLOOR IS THE WHOLE POINT AND I HAD IT WRONG THREE TIMES. A smooth
// analytic surface renders as smooth water in a voxel world and reads as
// foreign. Quantising it per column gives the swell real voxel steps -- and
// because the march happens per PIXEL, those steps move continuously without
// a single triangle being rebuilt. Voxel look, continuous motion, no re-mesh:
// the three things I kept insisting had to trade against each other.
//
// SIDE FACES ARE WHAT MAKE IT READ AS VOXELS. When the ray enters a column
// already below that column's top, it has struck the vertical step between two
// crests, not the water's top. v1 darkens those by 0.74 and says why: "darker
// step sides give the swell its silhouette".
//
// A COLUMN DDA, NOT A FIXED STEP. Stepping by distance either misses thin
// columns or oversamples fat ones; crossing one column boundary at a time is
// exact and costs two compares an iteration.
// ---------------------------------------------------------------------------
// HOW MANY COLUMNS THE MARCH MAY WALK. Each is one voxel of LATERAL travel,
// so this is how far across the water the search reaches -- and a grazing ray
// crosses a lot of water per unit of drop, which is why 28 (2.8 m) put a hard
// line across the lake at the distance it gave up. 48 pushes that out; the
// fallback below is what stops it being a line at all.
// HOW BRIGHT THE SUN-SPARKLE ON THE WATER IS. 0 turns it off and costs
// nothing -- the whole block folds away. See the note at its use.
// BACK ON at the user's request 2026-09-13, having been 0 since the 12th.
// 1 is v1's own strength. This is the one number to move: 0 folds the whole
// block away and costs nothing.
static const float kGlintStrength = 1.0f;

// Is this water term switched on? See kWF* in Shared.slang and the [I] panel.
bool waterOn(uint bit) { return (gParams.waterFlags & bit) != 0u; }

static const int kWaveSteps = 48;

// HOW FAR BACK ALONG THE RAY THE MARCH STARTS, and how far past the flat hit
// it is allowed to run, in metres. v1's 3.8 and 34/38 voxels. This is now the
// FLOOR of a reach that grows with range -- see kWaveCrestM.
static const float kWaveBackM = 0.38f;
static const float kWaveFarM  = 3.40f;

// It is defined further down, next to the glint and the caustics it was written
// for. The march wants it too and comes first in the file.
float featureSizeM(float dist, float baseM);

// ---------------------------------------------------------------------------
// WHY THE MARCH USED TO GIVE UP AT A FIXED DISTANCE, AND WHAT IT COST.
//
// Reported twice: "the water is still not rendering right at a distance and
// then renders correctly when moving closer". From the shore at eye height it
// is unmistakable -- the swell carries voxel steps for the first sixty metres
// or so and then STOPS DEAD along a horizontal line, and everything past it is
// a flat pale sheet up to the far bank.
//
// THE LINE IS GEOMETRY, NOT A TOLERANCE. A crest stands about 0.26 m above the
// still surface (the four amplitudes summed), and the march finds one by
// backing up along the ray until it is above that. The ray length that takes is
//
//     crest / |rd.y|
//
// and a ray from a 4 m eye to water 80 m out has |rd.y| near 0.05, so it needs
// FIVE metres of back-up. The window was a constant 3.4. Past the distance
// where those two cross, every pixel falls through to the smooth analytic
// normal -- and a smooth normal at a grazing angle is a mirror, which is one
// flat sheet of sky. That is the "cut in half" being reported, and the earlier
// fix (shading the fallback with waterNormal rather than with the flat quad)
// made the line SOFTER without moving it.
//
// SO THE REACH IS DERIVED RATHER THAN CHOSEN, and the step budget is bought
// back the way this shader already buys it back for the glint and the caustics:
// the DDA's lateral cell GROWS WITH RANGE (featureSizeM). At 300 m a 10 cm
// column is a third of a pixel across, so marching at that size spends
// forty-eight steps covering five metres of water and integrates to a flat wash
// anyway. Marched at the size a feature has to be to survive to the screen, the
// same forty-eight steps reach forty.
//
// THE HEIGHT QUANTISATION STAYS AT kVoxelM, and that is not an oversight. The
// swell is at most a couple of voxels tall; snapping it to a 0.8 m cell would
// round every crest to zero and hand back the flat sheet by another route. It
// is the LATERAL cell that grows -- wider plateaus, same step height, which is
// exactly what "hold the feature to a few pixels" means here.
// ---------------------------------------------------------------------------
static const float kWaveCrestM = 0.30f;   // 0.130 + 0.072 + 0.034 + 0.020, rounded up

bool marchWaterVoxels(float3 ro, float3 rd, float tHit, float baseTop, float t,
                      out float tOut, out bool sideFace, out float3 sideN)
{
    tOut = tHit;
    sideFace = false;
    sideN = float3(0.0f, 1.0f, 0.0f);
    // THE LATERAL CELL, GROWN WITH RANGE -- see the note over kWaveCrestM. Up
    // close featureSizeM returns kVoxelM exactly, so nothing about the near
    // water moves.
    const float vs = featureSizeM(tHit, kVoxelM);
    // ...AND THE REACH, FROM THE GEOMETRY RATHER THAN FROM A CONSTANT. Far
    // enough to clear a crest at this ray's slope, and never further than the
    // step budget can actually walk. 0.45 rather than 1 because the window is
    // spent on BOTH sides of the hit, and a diagonal ray crosses more than one
    // column per cell of travel. Floored at the old constant so close water is
    // byte for byte what it was.
    const float reach = max(kWaveFarM,
                            min(kWaveCrestM / max(abs(rd.y), 0.02f),
                                float(kWaveSteps) * vs * 0.45f));

    // ---------------------------------------------------------------------
    // IT STARTS BEHIND THE HIT, AND THAT IS THE WHOLE TRICK.
    //
    // This is what I could not see for a whole session. A crest stands ABOVE
    // the flat water surface, and there is no geometry up there for a ray to
    // find -- so I built a lid to hold one, and the lid's side faces stood
    // proud of every beach. v1 has no lid because it never needs one: it
    // walks BACKWARDS along the ray from the flat hit and marches forward
    // again, so a crest is found at a SMALLER t than the surface that led the
    // ray here.
    //
    // BACKING UP IS SAFE, and provably so: the water was the CLOSEST hit, so
    // everything between the ray's origin and it is empty space. Moving the
    // hit earlier along the same ray cannot pass through anything.
    //
    // What it costs is that a crest is not in the acceleration structure, so
    // it casts no shadow and no other ray can find it. v1 accepts exactly
    // that, and it is invisible next to having a silhouette at all.
    // ---------------------------------------------------------------------
    float tq = max(max(tHit - kWaveBackM / max(abs(rd.y), 0.08f), tHit - reach), 0.0f);
    const float3 pW = ro + rd * tq;

    int2 c = int2(floor(float2(pW.x, pW.z) / vs));
    const float sx = rd.x >= 0.0f ? 1.0f : -1.0f;
    const float sz = rd.z >= 0.0f ? 1.0f : -1.0f;
    const float adx = max(abs(rd.x), 1e-5f), adz = max(abs(rd.z), 1e-5f);
    float tmx = ((float(c.x) + max(sx, 0.0f)) * vs - pW.x) / (sx * adx) + tq;
    float tmz = ((float(c.y) + max(sz, 0.0f)) * vs - pW.z) / (sz * adz) + tq;
    const float stepX = vs / adx, stepZ = vs / adz;

    // WHICH WALL THE RAY LAST CROSSED, so a step face can be given the normal
    // it actually has. v1 never needs this -- it shades the hit and moves on --
    // but v2 REFRACTS through the surface, and a vertical wall wearing a y-up
    // normal bends the path into the swell and reads as a dark blot.
    int axis = -1;

    for (int i = 0; i < kWaveSteps; ++i)
    {
        const float2 cw = (float2(c) + 0.5f) * vs;
        // THE OFFSET IS QUANTISED, NOT THE ABSOLUTE HEIGHT. v1 writes
        // `baseTop + floor(wv + 0.5)`, so the surface is always a whole
        // number of voxels above or below the still waterline and lands on
        // the same grid the terrain around it does. Flooring the absolute
        // height instead would let the whole sheet sit half a voxel off.
        // kVoxelM, NOT vs. The cell may be a metre wide out at the horizon;
        // the STEP is still one voxel, because that is the grid the water
        // shares with the terrain and a crest rounded to a metre is no crest.
        //
        // -------------------------------------------------------------------
        // AND IT NEVER DIPS BELOW THE STILL SURFACE. THIS IS THE BLACK EDGE.
        //
        // The swell is four Gerstner waves summing to about +-0.26 m, and it is
        // measured from `baseTop` -- the meshed water top. In deep water a
        // trough 26 cm down is simply water. IN TEN CENTIMETRES OF WATER IT IS
        // SIXTEEN CENTIMETRES INSIDE THE BED.
        //
        // The march has no idea how deep the water is: it is handed the SURFACE
        // and the wave field and nothing else. So at the shallowest columns it
        // reported a water surface below the terrain, the refracted ray was
        // born inside the ground, and the next thing it met was the inside of
        // the bed -- which shades as an opaque hit with the path already
        // inside it. Black. It drew as a staircase of black voxels along every
        // sandy edge, and it appeared the moment one-voxel columns started
        // counting as wet (see kWetMinVox in voxelworld.h).
        //
        // Clamping at `baseTop` costs the trough half of the swell: the water
        // now rides between the still line and 26 cm above it rather than
        // either side of it. Rendered side by side that is very hard to see --
        // the swell's whole character is the stepped CRESTS and those are
        // untouched -- and it is the half that cannot be right anyway, because
        // there is no guarantee anywhere in this engine that there is water
        // below the line to dip into.
        //
        // The exact fix would be the DEPTH in the shader, damping the amplitude
        // as the water shoals the way real waves do. The water quad's strand
        // field is free and could carry it -- but the greedy merge keys on that
        // field, so a per-column depth splits every lake's quads by depth band
        // and buys triangles on all of them to fix 13 cm of mean level nobody
        // can see. Not worth it.
        // -------------------------------------------------------------------
        const float wh =
            max(baseTop, baseTop + floor(waterHeight(cw, t) / kVoxelM + 0.5f) * kVoxelM);
        const float tNext = min(tmx, tmz);

        // Already below this column's top: the ray came in through the
        // vertical step between two crests.
        if (ro.y + rd.y * tq <= wh)
        {
            tOut = tq;
            sideFace = true;
            // The wall faces back the way the ray came.
            if (axis == 0) sideN = float3(-sx, 0.0f, 0.0f);
            else if (axis == 1) sideN = float3(0.0f, 0.0f, -sz);
            return true;
        }
        // Crosses this column's top inside it.
        if (ro.y + rd.y * tNext <= wh)
        {
            tOut = (wh - ro.y) / rd.y;
            sideFace = false;
            return true;
        }

        if (tmx < tmz) { tq = tmx; tmx += stepX; c.x += int(sx); axis = 0; }
        else           { tq = tmz; tmz += stepZ; c.y += int(sz); axis = 1; }
        if (tq > tHit + reach) return false;
    }
    return false;
}

float3 waterNormal(float2 p, float t)
{
    // x, z, wavelength (m), amplitude (m)
    float nx = 0.0f, nz = 0.0f, ny = 1.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float k  = 6.2831853f / kWave[i].z;
        const float om = 0.21f * sqrt(9.8f * k);
        const float ph = k * (kWave[i].x * p.x + kWave[i].y * p.y) - om * t + kWavePH[i];
        const float sn = sin(ph);
        nx += gParams.waterWaveGain * kWave[i].x * k * kWave[i].w * sn;
        nz += gParams.waterWaveGain * kWave[i].y * k * kWave[i].w * sn;
        ny -= gParams.waterWaveGain * kWaveQ[i]  * k * kWave[i].w * cos(ph);
    }
    // max(ny, 0.3) is v1's: a steep enough sum can drive the vertical component
    // negative, and a surface normal pointing INTO the water refracts inside out.
    return normalize(float3(nx, max(ny, 0.30f), nz));
}

// Value noise, for the caustics only. Nothing else in this shader needed one.
float wHash2(float2 c)
{
    uint h = uint(int(c.x)) * 374761393u + uint(int(c.y)) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h ^ (h >> 16)) * 2.3283064e-10f;
}
float wNoise2(float2 p)
{
    const float2 f = floor(p), t = p - f;
    const float2 u = t * t * (3.0f - 2.0f * t);
    const float a = wHash2(f), b = wHash2(f + float2(1, 0));
    const float c = wHash2(f + float2(0, 1)), d = wHash2(f + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// CAUSTICS -- v1's caust(), and it is not a simulation of anything. Two noise
// fields drifting at different speeds; where they COINCIDE the difference goes
// to zero, and those coincidence lines form the moving web. The pow(.., 5) is
// what keeps it webs rather than mist.
//
// The frequencies convert like the waves do: v1 samples in voxels at 0.09 and
// 0.11, which is 0.9 and 1.1 per metre.
// ---------------------------------------------------------------------------
// HOW BIG A FEATURE HAS TO BE TO SURVIVE BEING LOOKED AT FROM A DISTANCE.
//
// "The effects on the water go away when you get far enough out" -- and they
// were never switched off. Nothing in this file gates on range. What happens
// is arithmetic: the glint keys off 10 cm cells and the caustics off roughly
// metre-wide webs, and at 100 m a pixel covers most of a metre. Hundreds of
// cells then fall inside one pixel and average to their own mean, which is a
// flat sheen. Ray Reconstruction finishes the job. Rendering the same frame at
// --no-dlss --spf 16 shows the sparkle IS still being computed out there; it
// simply cannot be resolved.
//
// So the features grow with distance instead of vanishing -- the same trick as
// a mip level, and for exactly the same reason. A cell is held to at least
// kFeaturePx pixels across, so it stays visible at any range; up close the
// clamp does nothing and the effect is the authored one, voxel for voxel.
//
// kPixelRad is the angle one pixel subtends, near enough: ~70 degrees of
// horizontal field over ~1400 px. It does not have to be exact -- it sets
// where the growth starts, and being out by a factor of two moves that
// distance, not the look.
// ---------------------------------------------------------------------------
static const float kPixelRad  = 0.0009f;
static const float kFeaturePx = 3.0f;

float featureSizeM(float dist, float baseM)
{
    return max(baseM, dist * kPixelRad * kFeaturePx);
}

// ---------------------------------------------------------------------------
// FOAM, THE WAY v1 MIXES IT -- AND THE WAY THIS ENGINE ALREADY GOT IT WRONG
// ONCE.
//
// v1:
//     foam   = max(foam, step(0.35, ivhash(...) * (0.55 + 0.45*sin(...))));
//     foamK  = clamp(foam, 0, 1) * 0.8;
//     albedo = mix(albedo, FOAM_C, foamK);
//
// ...so it reads as CHURN: scattered, shifting, and never fully white.
//
// v2's first attempt made every shallow column a SOLID white material and
// lifted it a voxel so it stood proud of the swell. A solid white band raised
// all the way round a lake is a kerb, not surf -- it is why foamDepthVox sat
// at 0 for a session. The lift is right in v1, where it carries a broken-up
// band; here it turned a bad colour into a bad silhouette.
//
// The fix is not the colour, it is that foam must be PATCHY and LEVEL. The
// host still says WHICH columns are shore -- foamColumn, by depth, and that
// part always worked. This says how much churn is on any given SPOT of them,
// and the answer is never all of it.
//
// PER VOXEL CELL, held to a few pixels at range for exactly the reason the
// glint is -- see featureSizeM. Churn finer than a pixel averages into a flat
// white wash, which is the one thing it must not look like.
// ---------------------------------------------------------------------------
static const float  kFoamK      = 0.80f;                        // v1's cap
static const float3 kFoamColour = float3(0.92f, 0.95f, 0.96f);  // mat::FOAM's own

// ---------------------------------------------------------------------------
// ...AND IT HAS TO CLUMP, WHICH THE FIRST VERSION DID NOT.
//
// The per-cell hash alone is WHITE NOISE: every cell decides on its own, so
// about half of them churn, evenly, everywhere the host called shore. Down a
// steep bank the band is under a metre wide and that reads as surf. Over a
// shallow BAY it is fifteen metres wide, and fifteen metres of evenly dithered
// white is not foam, it is a rash -- measured on a pine inlet, the whole bay
// speckled to its middle.
//
// Real surf GATHERS. So a low-frequency mask decides where there is foam at
// all, and the per-cell hash only breaks up its inside. The mask drifts, slowly
// and in its own direction, which keeps a shoreline alive without any of it
// reading as a scrolling texture.
// ---------------------------------------------------------------------------
float foamMix(float2 p, float t)
{
    // ~2 m tongues, in WORLD space so the patches belong to the shore rather
    // than to the camera, drifting far more slowly than the cells twinkle: a
    // shoreline moves, it does not flicker.
    const float gate = smoothstep(0.42f, 0.68f,
                                  wNoise2(p * 0.55f + float2(t * 0.045f, -t * 0.030f)));
    if (gate <= 0.0f) return 0.0f;

    const float cellM = featureSizeM(0.0f, kVoxelM);
    const float2 cell = floor(p / cellM);
    const float hsh = frac(sin(cell.x * 127.1f + cell.y * 311.7f) * 43758.5453f);
    // The sine is what stops the band being a fixed stencil: a cell drifts in
    // and out of churning on its own phase, so the shore boils instead of
    // holding a pattern.
    const float w = 0.55f + 0.45f * sin(t * 1.9f + hsh * 6.2831853f);
    return step(0.35f, hsh * w) * kFoamK * gate;
}

float waterCaustic(float2 p, float t, float scale)
{
    // scale < 1 stretches the webs, which is what keeps them resolvable far
    // away. It is 1 at close range, so nothing about the near look moves.
    const float n1 = wNoise2(p * (0.9f * scale) + float2( t * 0.275f, t * 0.170f));
    const float n2 = wNoise2(p * (1.1f * scale) + float2(-t * 0.235f, t * 0.145f));
    return pow(saturate(1.0f - abs(n1 - n2) * 2.6f), 5.0f);
}


Hit traceRadiance(float3 o, float3 d, uint mask = 0xFFu)
{
    Hit h;
    h.hit = false;
    h.t = 0.0f;
    h.p = float3(0.0f);
    h.ng = float3(0.0f, 1.0f, 0.0f);
    h.ns = float3(0.0f, 1.0f, 0.0f);
    h.held = false;
    h.mtl = 0u;
    h.foam = 0.0f;
    h.waterTop = 0.0f;
    h.mat.type = MAT_OPAQUE;
    h.mat.albedo = float3(0.0f);
    h.mat.roughness = 1.0f;
    h.mat.specular = 0.0f;
    h.mat.translucency = 0.0f;
    h.mat.alpha = 1.0f;   // a miss is opaque; nothing may see through nothing
    h.mat.ior = 1.0f;

    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-5f;
    ray.TMax = INF;

    // FORCE_OPAQUE IS A PROMISE, NOT A CHANGE. Every geometry in this scene is
    // already built with RtGeometryFlags::Opaque, so no any-hit shader can ever
    // run and not one ray behaves differently -- but the flag has to be on the
    // QUERY for the compiler to know that. Given it, the whole any-hit path
    // disappears: Proceed() is known to complete in one go, the state machine
    // it would otherwise have to keep for a re-entrant traversal is gone, and
    // the registers that state machine occupied go back to the shader.
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, mask, ray);
    q.Proceed(); // opaque by construction: nothing to run, one call is enough

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return h;

    const V6Instance inst = gInstances[q.CommittedInstanceID()];
    const uint packed = loadPackedTri(inst.triOffset, q.CommittedPrimitiveIndex());

    h.hit = true;
    h.t = q.CommittedRayT();
    h.p = o + d * h.t;

    // Object to world for a normal. Every transform in this scene is a quarter
    // turn about Y plus a translation, so the 3x3 is orthonormal and IS its own
    // inverse transpose -- no adjugate needed, which is the other thing the
    // quarter-turn-only rule buys besides keeping the voxel silhouette crisp.
    const float3x4 o2w = q.CommittedObjectToWorld3x4();
    const float3 nObj = faceNormal(triDir(packed));
    // NO normalize(). Every transform in this scene is a quarter turn about Y
    // plus a translation, so the 3x3 is a signed permutation of the identity --
    // and nObj is a unit axis. The product is therefore another unit axis with
    // exactly one non-zero component of exactly +/-1, and normalising it is a
    // reciprocal square root of exactly 1.0 on every hit in the frame.
    float3 ng = mul((float3x3)o2w, nObj);
    // THE ONE INSTANCE THAT CARRIES A SCALE. Every other transform in this
    // scene is a quarter turn about Y plus a translation, which is why the
    // product above needs no normalise (see the note just made). The held item
    // is a model authored at one voxel per unit and shrunk to the few
    // millimetres a tool voxel actually measures, so its 3x3 is a rotation
    // times that scale and the transformed normal comes out the same factor
    // short of unit. Normalising only here keeps the fast path fast.
    //
    // AND THE FLYERS, for a reason of their own: a butterfly's transform is a
    // free turn about Y -- still orthonormal, so still no adjugate -- times the
    // scale it fades in through, which leaves the normal short by exactly that
    // scale.
    if (inst.kind == KIND_HELD || inst.kind == KIND_FLYER)
        ng = normalize(ng);
    h.ng = ng;
    h.ns = ng;
    h.held = (inst.kind == KIND_HELD);
    h.hurt = inst.hurt;
    // HOW FAR THIS SURFACE'S INSTANCE TRAVELLED SINCE THE LAST FRAME, and no
    // test on the kind: the host derives this for everything it PLACES and
    // leaves it at zero for everything it builds once, so the wood, the water
    // and the forty thousand chunks behind them read a zero here and the
    // static-world reprojection below is untouched for all of them. Asking
    // "is this a flyer?" was how a shaft in flight got a zero it should not
    // have had -- it was not a flyer, it was KIND_TERRAIN. See World::place.
    h.moved = inst.prevOffset;

    // -- ...AND HOW FAR IT TURNED, FOR A LOOSE BODY -------------------------
    //
    // (user 2026-09-22: "the trees are flickering when the player chops them
    //  down".) A felled tree and the pieces it breaks into TURN, and a turn
    // moves every surface by a different amount -- the crown of a 26 m trunk
    // going over travels metres while its butt barely moves. prevOffset alone
    // gave every surface the same vector, so Ray Reconstruction was reprojecting
    // the whole falling tree from the wrong place. For these instances the host
    // sends prevOffset as the travel of the instance ORIGIN and flap as the turn
    // since last frame, axis * angle, taking a vector of the body now back to
    // where it pointed then -- see World::SlotXf. Rotate this hit's offset from
    // the origin back through it, and the difference is how far the surface
    // itself turned. Flyers and the tool own `flap` and are excluded; every
    // other instance leaves it at zero and is untouched.
    //
    // ...EXCEPT A FLYER WITH treeFlags BIT 1, which is a creature whose
    // TRANSFORM turned -- the frog's snap, the flock's bank -- and the host has
    // put exactly this channel's pair in for it: flap = axis * angle, prevOffset
    // re-based to the origin. See World::setFlyerInstance and flyerTurn.
    const bool flyerTurned = inst.kind == KIND_FLYER && (inst.treeFlags & 2u) != 0u;
    if ((inst.kind != KIND_FLYER && inst.kind != KIND_HELD && any(inst.flap != 0.0f)) ||
        flyerTurned)
    {
        const float3x4 o2w = q.CommittedObjectToWorld3x4();
        const float3 r = h.p - float3(o2w[0][3], o2w[1][3], o2w[2][3]);
        const float th = length(inst.flap);
        const float3 k = inst.flap / th;
        const float ct = cos(th), st = sin(th);
        const float3 rWas = r * ct + cross(k, r) * st + k * (dot(k, r) * (1.0f - ct));
        h.moved = inst.prevOffset + (r - rWas);
    }

    // -- AND THE TOOL, WHICH IS NOT SOMEWHERE IN THE WORLD AT ALL -----------
    //
    // Everything above is a correction ON TOP of the static-world reprojection:
    // a surface sits at a world point, the eye moves around it, and prevOffset
    // says how far the point itself also went. The tool breaks that at the
    // root. It is bolted to the eye, so turning moves its world point by the
    // largest amount in the frame while its PIXEL does not move at all -- and
    // no correction expressed as "how far the point went" can undo a term that
    // has to be cancelled rather than added.
    //
    // So this one is reprojected properly: back to object space, forward
    // through the pose the tool had LAST frame. That pose was built on last
    // frame's camera, so the eye's own motion is inside it and cancels against
    // prevCam when the guide is written; what does not cancel is the bob, the
    // swing and the draw, which are the tool's own and are exactly the motion
    // the old zero left out. See V6Params::heldPrev0.
    if (inst.kind == KIND_HELD && gParams.heldPrevValid != 0)
    {
        const float3 op = mul(q.CommittedWorldToObject3x4(), float4(h.p, 1.0f));
        const float3 was = float3(dot(gParams.heldPrev0, float4(op, 1.0f)),
                                  dot(gParams.heldPrev1, float4(op, 1.0f)),
                                  dot(gParams.heldPrev2, float4(op, 1.0f)));
        h.moved = h.p - was;
    }

    // -- ...AND HOW FAR THE WING UNDER IT ROSE ------------------------------
    //
    // prevOffset moved the whole instance. On a flyer mid-flap the surface
    // moved further than that: the pose changed under a transform that did
    // not, so a wing voxel is a voxel higher or lower than the one the last
    // frame drew while the body is exactly where the body was. inst.flap
    // carries that step -- see the note over it in Shared.slang -- and the
    // only question left here is which part of the model this hit is on.
    //
    // WHICH IS ANSWERED IN OBJECT SPACE, and cheaply. The 3x3 is a rotation
    // times the fade's scale, so its first COLUMN is the model's own x axis in
    // the world, of length that scale: projecting the hit onto it and dividing
    // by its square length is the object x, with the scale divided back out.
    // No inverse and no branch on the fade.
    //
    // THE QUARTER VOXEL IS NOT A FUDGE FACTOR. A hit lands ON a face, and the
    // +/-x faces of the body column sit exactly on the boundary between the
    // body's bucket and the inner wing's -- so rounding the raw coordinate
    // picks between two answers by the last bit of a subtraction. nObj is the
    // face's own outward normal in object space, so stepping back along it
    // puts the sample inside the voxel that was actually hit, which is the
    // voxel whose motion is being asked about. Every other face has a zero
    // x component here and is untouched.
    // A SPIN IN PLACE, which is the songbirds -- see V6Instance::flapPad. The
    // surface turned about a vertical axis while the instance stood still, so
    // where a point WAS is that point rotated back by the same angle about the
    // same axis. h.moved is "subtract this to get where it was", so it is the
    // difference between the two.
    //
    // Exact rather than approximate, and it costs a sin, a cos and six
    // multiplies -- there is no reason to linearise a rotation this cheap.
    if (inst.kind == KIND_FLYER && inst.flapPad != 0.0f)
    {
        const float a = -inst.flapPad;
        const float ca = cos(a), sa = sin(a);
        const float rx = h.p.x - inst.flap.x;
        const float rz = h.p.z - inst.flap.z;
        const float3 prev =
            float3(inst.flap.x + ca * rx + sa * rz, h.p.y, inst.flap.z - sa * rx + ca * rz);
        h.moved += h.p - prev;
    }
    else if (inst.kind == KIND_FLYER && !flyerTurned && any(inst.flap.xy != 0.0f))
    {
        const float3 ax = float3(o2w[0][0], o2w[1][0], o2w[2][0]);
        const float xObj = dot(h.p - float3(o2w[0][3], o2w[1][3], o2w[2][3]), ax) /
                           max(dot(ax, ax), 1e-12f);
        const float outAlong =
            abs((xObj - inst.flap.z) * (1.0f / kVoxelM) - nObj.x * 0.25f);
        // The tips are one bucket and everything past them joins it: this art
        // is five voxels across, so there is nothing past them to join.
        const float rise = (outAlong < 0.5f) ? 0.0f
                           : (outAlong < 1.5f) ? inst.flap.x
                                               : inst.flap.y;
        // h.moved is "subtract this from where it is to get where it was", so
        // a wing that ROSE this frame was lower: the rise adds.
        //
        // -- ...AND SOME WINGS DO NOT RISE, THEY SWEEP --------------------
        //
        // (user 2026-09-20: "the flys are no flying smoothly. they are very
        //  jittery.")
        //
        // The butterfly's wing steps in the model's Y and this branch was
        // written for it, so the step is added to world Y. THE HOUSEFLY IS ONE
        // VOXEL TALL: its four poses move the two wing voxels along the model's
        // OWN Z (z = 2, 1, 0, 1 over the strip, body fixed at the middle), so
        // there is no rise to add and the old branch could not describe it at
        // all. Two of its three voxels were therefore handed the body's motion
        // vector while they stepped 10 cm, 24 times a second -- which is the
        // same smear this whole field exists to stop, on a model small enough
        // that the wings ARE the animal.
        //
        // V6Instance::treeFlags bit 0 is the discriminator. It is a tree's
        // field and a flyer never had one, so this costs no space; it is
        // written for every flyer by setFlyerInstance, so it can never be read
        // stale.
        if ((inst.treeFlags & 1u) != 0u)
        {
            // The model's own +Z in the world. NORMALISED, because the 3x3
            // carries the fade's scale and the host has already put the fade
            // into `rise` -- exactly as the butterfly does with its own.
            const float3 az = float3(o2w[0][2], o2w[1][2], o2w[2][2]);
            h.moved += az * (rise / max(length(az), 1e-6f));
        }
        else
            h.moved.y += rise;
    }

    // WATER IS A MATERIAL NOW, NOT AN INSTANCE KIND.
    //
    // It used to be one world-sized quad in its own KIND_WATER instance, and
    // then briefly a per-chunk structure of its own -- which is what collided in
    // the compaction queue and replaced whole chunks of ground with a flat
    // sheet. A lake is rows of kWater in the terrain's own mesh; this branch is
    // all that is needed to shade it, and KIND_WATER is kept only so the editor
    // stage's own quad still works.
    //
    // THE RIPPLE KEYS OFF THE FACE, which is the reason one material id is
    // enough where v1 needed two. A lake's TOP is what has a wave on it; its
    // edges are vertical and a y-up ripple normal on them would tilt the
    // surface into the bank and refract sideways.
    // kFoam BELONGS IN THIS TEST. The shore band is its own material on the
    // quad so the greedy merge splits at its edge (see WaterColumns::material)
    // -- but it is still water, and without it here a shore column falls
    // through to the OPAQUE path below and is drawn as a solid white block.
    // That is the white wall in the note over foamMix, reached by a second
    // route.
    const uint triMat = triMaterial(packed);
    if (inst.kind == KIND_WATER || triMat == kWater || triMat == kFoam)
    {
        const uint fdir = (packed >> kTriDirShift) & kTriDirMask;
        const bool topFace = (inst.kind == KIND_WATER) || (fdir == 0u);   // face::POS_Y
        // THE QUAD IS THE STILL SURFACE, and it is recorded BEFORE the march
        // moves h.p off it -- see Hit::waterTop for what reading the global
        // instead cost.
        h.waterTop = h.p.y;
        bool wSide = false;   // the march may land on a vertical step between crests
        // THE SURFACE IS BOTH: VOXEL CRESTS FOR SHAPE, A NORMAL FOR MOTION.
        //
        // Each alone fails, and the two failures are opposite.
        //
        // A NORMAL ALONE is a bump map on a mirror. The shading says "wave"
        // and the silhouette says "plane", and at a grazing angle the eye
        // believes the silhouette -- which is what made this read as
        // corrugated iron.
        //
        // GEOMETRY ALONE CANNOT MOVE. VoxelTerrain::waveVox raises the water
        // column 0..waveVoxMax, so at 10 cm a column has THREE possible
        // heights. Advancing it re-meshes the lake into another still frame;
        // there is no smooth motion available at this quantisation whatever
        // the update rate, and staggering the re-mesh across chunks puts
        // neighbours at different phases and tears the surface at chunk seams.
        // Both were measured, and both looked worse than the problem.
        //
        // So the crests are STATIC and give the surface its real shape and
        // silhouette, and this perturbation moves continuously on top of them.
        // It is no longer tilting a flat quad -- it is tilting a surface that
        // already has geometry -- which is why it does not read as corduroy
        // any more. And it costs nothing: no re-mesh, no BLAS rebuild, no
        // phase seam. The clock is gParams.waterTime, a wall clock, so
        // scrubbing the sun does not run the sea backwards.
        //
        // THE RIPPLE KEYS OFF THE FACE. A lake's TOP is what has a wave on it;
        // its sides are vertical and a y-up ripple normal on them would tilt
        // the surface into the bank and refract sideways.
        if (topFace)
        {
            // DOWNWARD RAYS ONLY, which is v1's `rd.y < -0.01`. The march
            // walks back up the ray to look for a crest standing over the
            // flat surface, and that only makes sense for a ray coming down
            // onto the water. A ray leaving from underneath keeps the flat
            // top it already found.
            bool marched = false;
            if (waterOn(kWFSwell) && d.y < -0.01f)
            {
                float wt;
                bool ws;
                float3 wn;
                if (marchWaterVoxels(o, d, h.t, h.p.y, gParams.waterTime, wt, ws, wn))
                {
                    marched = true;
                    h.t = wt;
                    h.p = o + d * wt;
                    wSide = ws;
                    // A STEP SIDE IS VERTICAL AND MUST SAY SO. Left with the
                    // quad's y-up normal it refracts sideways into the swell,
                    // which is what drew the dark blocky blots across the lake.
                    if (ws) { h.ns = wn; h.ng = wn; }
                }
            }
            // -----------------------------------------------------------
            // WHERE THE MARCH FINDS NOTHING, SHADE THE WAVE INSTEAD.
            //
            // The search is bounded -- kWaveSteps columns of lateral travel --
            // and a ray crossing the lake at a grazing angle runs out of them
            // long before it meets the surface. It then fell back to the flat
            // meshed quad, which put a hard line across every lake at exactly
            // the distance the march gave up: near water with voxel steps,
            // far water a mirror. That is "the water got cut in half".
            //
            // Beyond that distance the steps are sub-pixel anyway, so what is
            // lost is a silhouette nobody can resolve -- and the analytic
            // normal is exactly the right thing to spend there instead. It
            // costs four cosines and no march.
            //
            // THIS IS WHY waterNormal IS STILL HERE. With the march succeeding
            // it would argue with real geometry and read as corrugated iron;
            // with the march having found nothing there is no geometry for it
            // to argue with.
            // -----------------------------------------------------------
            if (!marched && waterOn(kWFRipple))
                h.ns = waterNormal(float2(h.p.x, h.p.z), gParams.waterTime);
            // NO SHADING NORMAL. The surface has real voxel steps now, so the
            // silhouette carries the swell and a smooth Gerstner normal on top
            // of it would only argue with the geometry -- which is the
            // corrugated-iron look this replaced. v1 does the same: flat
            // facets, and the step sides darkened below.
        }
        h.mat.type = MAT_DIELECTRIC;
        h.mat.ior = 1.333f;
        h.mat.roughness = 0.0f;
        // A STEP FACE IS VERTICAL and keeps its flat geometric normal -- a
        // y-up ripple there would tilt it into the swell and refract sideways.
        // v1 darkens it by 0.74: "darker step sides give the swell its
        // silhouette", and that silhouette is the whole reason for marching.
        //
        // SET HERE, AFTER THE DEFAULTS. Setting it inside the branch above put
        // it two lines before `albedo = 1.0` and it was silently undone.
        h.mat.albedo = wSide ? float3(0.74f) : float3(1.0f);
        // ONLY THE TOP OF A SHORE COLUMN CHURNS. A vertical step face is the
        // side of the swell; whitening it would draw a bright rim under every
        // crest, which is the corrugated look the step darkening exists to
        // avoid.
        if (triMat == kFoam && topFace && !wSide && waterOn(kWFFoam))
            h.foam = foamMix(float2(h.p.x, h.p.z), gParams.waterTime);
        return h;
    }

    // THE RESOLVED SHADE, KEPT. groundShade turns a material FAMILY into the
    // exact shade this voxel wears, and until now the index was consumed on
    // the spot to look up a colour. The neural cache wants the index itself:
    // a discrete identity is a far better network input than the three floats
    // it decodes to -- see the encoding note in Nrc.slang.
    // A LOOSE PIECE ALREADY KNOWS ITS OWN COLOURS -- see KIND_LOOSE. Asking
    // the position hash again would re-roll them every frame it moves.
    const uint shade =
        (inst.kind == KIND_LOOSE) ? triMaterial(packed) : groundShade(packed, h.p, ng);
    // ...OUT OF THIS PLACE'S HALF OF THE TABLE. gMaterials holds the sandbox's
    // entries and then the arcade's; gParams.matBase says which set this frame
    // is standing in. See V6Params::matBase. `shade` itself is NOT offset --
    // h.mtl below is the neural cache's surface identity and must not move.
    // ...AND A MODEL THAT OWNS A TABLE READS ITS OWN, IN EITHER PLACE.
    //
    // See V6Instance::matBase for why this substitutes rather than adds. Zero
    // -- which is every instance in the program today -- falls through to the
    // place's offset, so this line is byte-for-byte what it was.
    const uint matBase = (inst.matBase != 0u) ? inst.matBase : gParams.matBase;
    const V6Material L = gMaterials[matBase + shade];
    h.mtl = shade;
    h.mat.type = MAT_OPAQUE;
    h.mat.albedo = L.albedo;

    h.mat.roughness = L.roughness;
    h.mat.specular = L.specular;
    h.mat.translucency = L.translucency;
    h.mat.alpha = L.alpha;
    // -- ...AND A WATER-LIKE ENTRY GETS WATER'S TREATMENT ------------------
    //
    // See V6Material::ior. Everything in the world answers 0 here and pays one
    // compare; the smoke answers with its own index of refraction and is drawn
    // the way the lake is -- refracted, not stochastically skipped.
    if (L.ior > 1.0f)
    {
        h.mat.type = MAT_DIELECTRIC;
        h.mat.ior = L.ior;
    }

    if (inst.kind == KIND_TREE)
    {
        // A few percent of per-tree hue. Nine models over thousands of trees
        // would otherwise show their repeat; this is enough to break it without
        // making the stand look like a paint chart.
        h.mat.albedo *= inst.tint;

        // -- ...AND EVERY LEAF IS ITS OWN LEAF ---------------------------
        //
        // (user 2026-09-19: "could you just randomize the current oak tree
        //  foilage colors? dont touch the wood colors. I want you to do the
        //  same for the cherry trees as well".)
        //
        // THE TINT ABOVE IS PER TREE, and that was the only variation a crown
        // had: three models over a thousand trees, each wearing four authored
        // greens in fixed places, so two oaks of the same model were the same
        // oak twice. The ground has never worked that way -- groundShade picks
        // a shade per VOXEL out of the family's ramp, which is why a hillside
        // does not tile -- and this is that grammar applied to a canopy.
        //
        // TRANSLUCENCY IS WHAT A LEAF IS. The palette sets it on foliage and
        // on blossom and on nothing else on a tree, and it is already the test
        // deriveGroundFromTrees uses to tell a leaf from bark. So the bark is
        // untouched by construction rather than by a material list that would
        // need keeping in step -- which is the ask's second half, "dont touch
        // the wood colors".
        //
        // IT IS THE CURRENT COLOURS, SCATTERED, NOT NEW ONES. A multiply about
        // 1.0 keeps each shade's own hue and moves its VALUE, so the crown
        // still reads as the ramp it was authored in and the four steps stop
        // being four flat regions. +-11% is roughly the gap between two
        // neighbouring steps of the oak's own ramp, which is the most it can
        // move without inventing a fifth.
        //
        // ...AND A LITTLE HUE WITH IT, half the amplitude and on its own
        // stream, because real foliage varies in more than brightness and a
        // pure value jitter reads as noise on one colour. The cherry gets both
        // for free: a blossom is translucent too.
        //
        // THE VOXEL, NOT THE POINT. Same derivation groundShade uses -- step
        // half a voxel back along the face normal so both sides of a face
        // agree on which voxel they belong to, or a leaf changes colour as the
        // ray crosses its own edge.
        if (h.mat.translucency > 0.0f)
        {
            const int3 lv = int3(floor((h.p - ng * (0.5f * kVoxelM)) / kVoxelM));
            const uint lh = hashVoxel(lv);
            const float val = 1.0f + (float(lh & 1023u) * (1.0f / 1023.0f) - 0.5f) * 0.22f;
            const float hue = (float((lh >> 10) & 1023u) * (1.0f / 1023.0f) - 0.5f) * 0.11f;
            h.mat.albedo *= val;
            // Opposed channels, so the shift is a HUE and not a second value:
            // what leaves red it takes from blue and the luminance holds.
            h.mat.albedo.r *= 1.0f + hue;
            h.mat.albedo.b *= 1.0f - hue;
            // -- ...AND HALF THE OAKS ARE A DARKER GREEN --------------------
            //
            // INSIDE THIS BRANCH ON PURPOSE. `translucency > 0` is the test
            // the note above calls "what a leaf is" -- foliage and blossom and
            // nothing else on a tree -- so the bark is untouched and the ask
            // stays what it was: a darker GREEN, not a darker tree.
            //
            // The pick is V6Instance::treeFlags bit 1, decided once on the
            // host per tree. Doing it here off a hash would either tie two
            // trees together (the tint) or let a tree change variant when its
            // chunk streams (the instance index).
            //
            // GREEN HELD, RED AND BLUE PULLED DOWN: a flat multiply just makes
            // a dim leaf, while taking the other two channels further deepens
            // the hue as it darkens, which is what a shade-grown oak actually
            // looks like against a sunlit one.
            if ((inst.treeFlags & 1u) != 0u)
                // 50% DARKER THAN THE LIGHT HALF, not 50% of what it was.
                // (user 2026-09-20: "make the dark green oak tree 50% darker".)
                // The green channel is the one the eye reads on foliage, so it
                // is the one set to half -- 0.78 -> 0.50 -- and the other two
                // scale by the same factor so the hue-deepening ratio the note
                // above describes is preserved rather than flattened.
                h.mat.albedo *= float3(0.44f, 0.50f, 0.40f);
        }
    }

    // -- ...AND AN ANIMAL THAT HAS JUST BEEN HIT IS RED ---------------------
    //
    // REPLACED, NOT MULTIPLIED. "all the voxels that make up the life turn
    // red" -- a multiply leaves a black eye black and a white wing pink, which
    // reads as a red LIGHT on the animal rather than as the animal flashing.
    // v1 mixes to the same constant for the same reason.
    //
    // THE WHOLE INSTANCE, which is what makes this cost nothing to decide: the
    // flash is a property of the animal, not of where the blow landed, so there
    // is no box to test a surface point against. v1 needs one -- its mammals
    // are stamped into the world grid and have no instance to hang this on --
    // and most of the size of its hit-flash block is that box being got right.
    if (inst.hurt > 0.0f) h.mat.albedo = kHurtRed;

    // Foliage is thin enough to light from behind, so its faces are treated as
    // two-sided: turn the normal to the viewer rather than letting a backfacing
    // needle voxel go black.
    if (L.translucency > 0.0f)
    {
        h.ns = faceforward3(h.ns, -d);
        h.ng = faceforward3(h.ng, -d);
    }
    return h;
}

// The cheapest occlusion query the hardware can express: first hit ends the
// search, and there is no shading of any kind attached to it. In a conifer
// canopy most shadow rays die inside the first metre of foliage, and this is
// most of the frame.
// WATER MUST NOT CAST A SHADOW, and until this argument existed it did.
//
// v2's lake is triangles in the chunk's own BLAS, so a shadow ray from the
// lake BED towards the sun hits the water's top face and reports occluded.
// The bed therefore received ZERO direct sun -- and nothing downstream could
// recover it, because the caustics are drawn by MULTIPLYING the bed's albedo,
// so they brighten a surface the sun never reached. v4 hit exactly this and
// its note is the better statement: "Two verification renders went looking for
// a broken refraction before this."
//
// PAYING FOR IT ONLY UNDER WATER. `passWater` is the path's own `inWater`, so
// a shadow ray dying in a canopy -- which is most of the frame -- takes the
// branch below unchanged, with ACCEPT_FIRST_HIT and one Proceed(). v4 gates
// the same work on a per-brick "this brick holds water" flag for the same
// reason.
//
// The slow path cannot use ACCEPT_FIRST_HIT: it has to know WHICH surface it
// found in order to step past it, and ACCEPT_FIRST_HIT returns any hit rather
// than the nearest, so advancing TMin past it could skip a real occluder.
bool traceOccluded(float3 o, float3 d, float tmax, bool passWater)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-5f;
    ray.TMax = tmax;

    if (!passWater)
    {
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(gScene,
                         RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE, 0xFF,
                         ray);
        q.Proceed();
        return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    }

    // A lake is a shell, not a solid: the mesher emits its top, its bed-facing
    // underside and its sides, so a ray leaving the bed crosses at most a few
    // water faces before it is in open air. Four is generous; running out
    // means calling the point LIT, which errs towards the bug this fixes
    // rather than back into it.
    for (int i = 0; i < 4; ++i)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return false;

        const V6Instance inst = gInstances[q.CommittedInstanceID()];
        const uint m = triMaterial(loadPackedTri(inst.triOffset, q.CommittedPrimitiveIndex()));
        if (m != kWater && m != kFoam)
            return true;  // something that really does block the sun

        ray.TMin = q.CommittedRayT() + 1e-3f;
        if (ray.TMin >= ray.TMax)
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// NEXT EVENT ESTIMATION: THE BULB.
//
// A POINT LIGHT, which makes this the shortest estimator in the file: there is
// exactly one direction to the light, so there is no light pdf, no cone to
// sample and no MIS partner -- a BSDF bounce can never find a point by chance,
// so nothing it returns can be double counted.
//
// THE SHADOW RAY STOPS SHORT OF THE GLASS. The bulb is real geometry in the
// BLAS (that is what the camera sees glowing), so a ray aimed at its centre
// would hit it and report the whole room occluded -- the light shadowing
// itself, and a room that stays black for a subtler reason than before. tmax is
// pulled back past the voxel sphere's corners rather than to its radius,
// because a blocky sphere is proud of the smooth one it approximates.
//
// THE FALLOFF HAS A FLOOR AT THE RADIUS. Inside the bulb, 1/d^2 goes to
// infinity; a point light is a fiction that stops being true at the surface of
// the thing it stands for, so it is clamped there rather than allowed to
// produce one white pixel if the player ever flies into it.
// ---------------------------------------------------------------------------
void sampleBulbSplit(Hit h, Frame fr, float3 wo, bool inWater, out float3 sumDiff,
                     out float3 sumSpec)
{
    sumDiff = float3(0.0f);
    sumSpec = float3(0.0f);
    // -- THE LIST IS THE SWITCH NOW, NOT THE MATERIAL --------------------
    //
    // (user 2026-09-17: "can you make the spark voxel emit volumetric light
    //  like the lightbulbs?")
    //
    // This used to return unless bulbMtl named a material, which was right
    // while every point light in the game was a pendant with glass on it. A
    // SPARK HAS NO SUCH MATERIAL -- it is an emitter (see V6Params::emitters)
    // and it lives in the wood, where bulbMtl is deliberately kNoBulb so a
    // level's id cannot light a pine. Gated on the material, a spark could
    // never light anything anywhere it actually exists.
    //
    // So the LIST decides: a filled slot is a light, whatever drew it.
    if (isBlack(gParams.bulbRadiance))
        return;
    if (gParams.bulbMtl == kNoBulb && gParams.bulbs[0].w < 0.0f)
        return;

    // WHICH LAMP LIGHTS THIS POINT: the nearest one to the SHADING POINT, and
    // not to the camera -- see kBulbSlots, where the difference is the whole
    // bug this array exists to fix. The loop only CHOOSES; there is still
    // exactly one shadow ray below, and a place with no array set falls back to
    // the single bulbPos the pause room used.
    float3 bulbAt = gParams.bulbPos;
    // ...AND HOW BRIGHT THAT ONE IS. w is a per-light gain now (see
    // V6Params::bulbs): 1 for a lamp, a fraction for an ember. Carried out of
    // the loop with the position, or the nearest light would be chosen from
    // the list and then lit at the lamp's intensity.
    float bulbGain = 1.0f;
    {
        float best = 1e30f;
        for (int bi = 0; bi < kBulbSlots; ++bi)
        {
            if (gParams.bulbs[bi].w < 0.0f)
                break;   // filled from 0 up; the first empty slot ends the list
            const float3 dq = gParams.bulbs[bi].xyz - h.p;
            // WEIGHTED BY THE GAIN, so a dim ember three metres away does not
            // outvote the lamp overhead simply by being closer. This is the
            // one shadow ray in the frame; it should be spent on whichever
            // light actually delivers the most here.
            const float qd = dot(dq, dq) / max(gParams.bulbs[bi].w, 1e-4f);
            if (qd < best)
            {
                best = qd;
                bulbAt = gParams.bulbs[bi].xyz;
                bulbGain = gParams.bulbs[bi].w;
            }
        }
    }

    const float3 toL = bulbAt - h.p;
    const float d2 = dot(toL, toL);
    if (d2 < 1e-8f)
        return;
    const float dist = sqrt(d2);
    const float3 wWorld = toL / dist;
    const float3 wi = fr.toLocal(wWorld);
    if (wi.z <= 0.0f && h.mat.translucency <= 0.0f)
        return;

    float pdf = 0.0f;
    float3 fD, fS;
    evalOpaqueSplit(h.mat, wo, wi, fD, fS, pdf);
    if (pdf <= 0.0f || (isBlack(fD) && isBlack(fS)))
        return;

    // -- THE LAMP DOES NOT SHADOW WHAT IT IS SCREWED TO --------------------
    //
    // (user 2026-09-17: "theres a circlular shadow caused by the bulb on the
    // cieling, remove it".)
    //
    // The shadow ray stops short of the light so the glass does not occlude
    // itself -- that is what the radius term has always been for. But a pendant
    // is not just glass: it has a CAP and a fitting sitting between the bulb and
    // the slab it hangs from, and the ceiling's rays to the light pass straight
    // through them. Every lamp in the map printed a dark disc on the ceiling
    // directly above it.
    //
    // A PHYSICAL SHADOW, AND STILL WRONG. A real ceiling rose does shade the
    // plaster it is bolted to, by a few millimetres and invisibly; here the cap
    // is a 10 cm voxel 40 cm from a point light, so it throws a ring you can
    // read across the room.
    //
    // So the last kBulbSelfClearM of the ray is ignored -- enough to clear the
    // whole fitting rather than just the glass. The cost is that a real
    // occluder within half a metre of a bulb stops casting, and in a room lit by
    // a pendant there is nothing that close to it but the pendant.
    const float tmax = dist - max(gParams.bulbRadius * 1.9f, kBulbSelfClearM);
    // STANDING INSIDE THE CLEARANCE IS NOT THE SAME AS BEING IN SHADOW.
    // `tmax <= 0` means the shading point is within the self-clear distance of
    // the bulb -- closer to it than the fitting is wide -- and returning there
    // dropped the light entirely, so walking up to a pendant turned it off. It
    // is the one place there is certainly nothing between you and the filament,
    // so the occlusion test is what gets skipped, not the light.
    if (tmax > 0.0f && traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, tmax, inWater))
        return;

    const float r = gParams.bulbRadius;
    const float3 E = gParams.bulbRadiance * bulbGain * (abs(wi.z) / max(d2, r * r));
    sumDiff = fD * E;
    sumSpec = fS * E;
}

float3 sampleBulb(Hit h, Frame fr, float3 wo, bool inWater)
{
    float3 a, b;
    sampleBulbSplit(h, fr, wo, inWater, a, b);
    return a + b;
}

// ---------------------------------------------------------------------------
// Next event estimation: the sun.
// ---------------------------------------------------------------------------
void sampleSunSplit(Hit h, Frame fr, float3 wo, inout Rng rng, BlueNoise bn, int depth,
                    bool inWater, out float3 sumDiff, out float3 sumSpec)
{
    sumDiff = float3(0.0f);
    sumSpec = float3(0.0f);

    const float3 Lsun = keyRadiance(gParams.sky);
    if (isBlack(Lsun))
        return;

    // MORE THAN ONE RAY AT THE SAME LIGHT. The extra samples are spent only on
    // the vertices that can pay for them: by the second bounce the throughput
    // is small, roulette is already killing paths, and what those vertices
    // contribute is too dim for their variance to show.
    const int n = (depth < gParams.shadowRayDepth) ? max(1, gParams.shadowRays) : 1;

    const Frame sf = Frame.make(keyDir(gParams.sky));
    const float lightPdf = uniformConePdf(keyCosR(gParams.sky));

    for (int i = 0; i < n; ++i)
    {
        // THE SHADOW RAY IS THE DIMENSION THAT MATTERS MOST HERE. This scene
        // is sun-dominated and its noise is overwhelmingly the sun's own
        // penumbra through a canopy, so if only one sample in the whole path
        // were to get the mask it would be this one.
        const float2 uSun = bn.next2DIf(i < 8, kBnDimSun + uint(i), rng);
        const float3 wWorld = sf.toWorld(sampleUniformCone(uSun, keyCosR(gParams.sky)));
        const float3 wi = fr.toLocal(wWorld);

        // A needle transmits, so a direction on the far side is still a valid
        // light path; an opaque surface below its own horizon is not.
        if (wi.z <= 0.0f && h.mat.translucency <= 0.0f)
            continue;

        float pdf = 0.0f;
        float3 fD, fS;
        evalOpaqueSplit(h.mat, wo, wi, fD, fS, pdf);
        if (pdf <= 0.0f || (isBlack(fD) && isBlack(fS)))
            continue;

        if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF, inWater))
            continue;

        const float w = powerHeuristic(1.0f, lightPdf, 1.0f, pdf);
        const float3 common = Lsun * (abs(wi.z) * w / lightPdf);
        float3 cD = fD * common;
        float3 cS = fS * common;

        // Fireflies: a grazing specular lobe against a 10^5 sun radiance can
        // produce a single sample worth more than the rest of the pixel. Only
        // the indirect bounces are clamped -- clamping the first would visibly
        // dim every sunlit surface in the frame.
        //
        // Applied per sample rather than to the mean: the ceiling is on what a
        // single estimate may be worth, and averaging first would let one spike
        // drag the mean up to the limit and hold it there.
        //
        // The ceiling is on the SUM of the two lobes, exactly as it was before
        // they were told apart, and both are scaled by the one factor -- so
        // splitting the channels cannot change what the clamp does.
        if (depth > 0 && gParams.clampIndirect > 0.0f)
        {
            const float m = maxComp(cD + cS);
            if (m > gParams.clampIndirect)
            {
                const float k = gParams.clampIndirect / m;
                cD *= k;
                cS *= k;
            }
        }
        sumDiff += cD;
        sumSpec += cS;
    }

    // Rays that returned nothing -- below the horizon, occluded, black BSDF --
    // are zeros in the average, not skipped. Dividing by the number that got
    // through instead would turn a half-shadowed vertex into a fully lit one.
    const float inv = 1.0f / float(n);
    sumDiff *= inv;
    sumSpec *= inv;
}

// The composited form, for every vertex whose lobe split nobody downstream
// asks about. One implementation, so the two cannot disagree.
// PASSES A SWITCHED-OFF SAMPLER, and that is the whole reason this wrapper did
// not simply gain a parameter. It is called only from depth > 0, where a
// screen-space pattern describes nothing -- the paths reaching those vertices
// left the pixel long ago. Making that explicit here means the depth test lives
// in one place rather than being re-derived at each call site.
float3 sampleSun(Hit h, Frame fr, float3 wo, inout Rng rng, int depth, bool inWater)
{
    float3 dif, spc;
    sampleSunSplit(h, fr, wo, rng, BlueNoise.make(uint2(0u, 0u), 0u, 0u), depth, inWater, dif,
                   spc);
    return dif + spc;
}

// ---------------------------------------------------------------------------
// Next event estimation: the sky dome.
//
// WHY THIS EXISTS AT ALL, when the dome was already being collected. A path
// that escapes the canopy lands on the sky and the dome arrives at full
// weight. That is correct, and before this it was the ONLY way light ever
// reached a shadowed pixel -- under a canopy the sun is occluded by
// definition, so the dome is the whole of the light.
//
// The trouble is what reaching it that way costs. It takes a whole path
// segment, and that segment has to survive two things first:
//
//   RUSSIAN ROULETTE, which at a foliage albedo of 0.19 kills roughly four
//   paths in five at the first bounce. Unbiased, so the film converges -- but
//   what it converges FROM is a spike distribution, most pixels holding zero
//   and a few holding a large escape. A tone curve compresses the spikes and
//   leaves the zeros alone, so the eye reads the mean as far darker than it is.
//
//   THE DEPTH BUDGET, which at the last vertex draws a direction, updates the
//   throughput, and then exits the loop without ever tracing it. That one is
//   not variance at all -- it is energy the estimator simply drops, and it
//   drops it in exactly the places that are already darkest.
//
// A dome sample here is an occlusion ray and nothing more. It does not
// continue, so roulette cannot take it, and it is taken at every vertex
// INCLUDING the last one -- which is where the truncation loss lived.
//
// THE SAMPLER IS COSINE-WEIGHTED ABOUT THE SHADING NORMAL, deliberately the
// same shape the diffuse lobe draws from. As a DISTRIBUTION that buys nothing:
// MIS between two identical strategies is two half-weight copies of one
// estimator. What it buys is that the two copies are INDEPENDENT and one of
// them is unconditional, so the variance of the sky term halves and the half
// that does not depend on surviving roulette is the half the shadows were
// missing. Importance-sampling the Perez dome by luminance would do better
// still; it needs a CDF rebuilt whenever the sun moves, which this does not.
//
// THE DOME ONLY. The solar disk is next-event-sampled on its own cone in
// sampleSunSplit, and skyDome() excludes the disk entirely -- so the two light
// sources never overlap, and neither MIS pair needs to know about the other.
// ---------------------------------------------------------------------------

// The pdf the dome sampler would have used for a direction leaving a surface
// with this shading normal.
//
// It has to be evaluable for ANY direction, not just for ones this sampler
// drew, because the escape path asks it the reverse question: "could the light
// sampler have found the direction the BSDF chose?" Without an answer to that
// there is no MIS weight, and without the weight the dome gets counted twice.
float skyDomePdf(float3 ns, float3 wWorld)
{
    return max(0.0f, dot(ns, wWorld)) * INV_PI;
}

void sampleSkySplit(Hit h, Frame fr, float3 wo, inout Rng rng, BlueNoise bn, int depth,
                    bool inWater, out float3 sumDiff, out float3 sumSpec)
{
    sumDiff = float3(0.0f);
    sumSpec = float3(0.0f);
    if (gParams.skyRays <= 0)
        return;

    // The same staircase the sun uses, and for the same reason: by the second
    // bounce the throughput is small enough that this vertex's variance does
    // not show in the frame.
    const int n = (depth < gParams.skyRayDepth) ? max(1, gParams.skyRays) : 1;

    for (int i = 0; i < n; ++i)
    {
        // Drawn in the local frame, so the sample is already the direction the
        // diffuse lobe cares about and no rotation is needed to get its pdf.
        const float3 wi = sampleCosineHemisphere(bn.next2DIf(i < 8, kBnDimSky + uint(i), rng));
        const float lightPdf = wi.z * INV_PI;
        if (lightPdf <= 1e-6f)
            continue;

        const float3 wWorld = fr.toWorld(wi);

        float pdf = 0.0f;
        float3 fD, fS;
        evalOpaqueSplit(h.mat, wo, wi, fD, fS, pdf);
        if (pdf <= 0.0f || (isBlack(fD) && isBlack(fS)))
            continue;

        const float3 Lsky = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, wWorld);
        if (isBlack(Lsky))
            continue;

        if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF, inWater))
            continue;

        const float w = powerHeuristic(1.0f, lightPdf, 1.0f, pdf);
        const float3 common = Lsky * (abs(wi.z) * w / lightPdf);
        float3 cD = fD * common;
        float3 cS = fS * common;

        // The same indirect ceiling the sun term carries, applied the same way
        // -- per sample, on the sum of the two lobes. The dome is four orders
        // of magnitude below the solar disk so it will rarely reach it, but a
        // grazing specular lobe can still manufacture a spike and the clamp
        // costs nothing when it does not fire.
        if (depth > 0 && gParams.clampIndirect > 0.0f)
        {
            const float m = maxComp(cD + cS);
            if (m > gParams.clampIndirect)
            {
                const float k = gParams.clampIndirect / m;
                cD *= k;
                cS *= k;
            }
        }
        sumDiff += cD;
        sumSpec += cS;
    }

    // Occluded and below-horizon samples are zeros in the average, not skipped
    // -- the same rule as the sun, and for the same reason.
    const float inv = 1.0f / float(n);
    sumDiff *= inv;
    sumSpec *= inv;
}

float3 sampleSky(Hit h, Frame fr, float3 wo, inout Rng rng, int depth, bool inWater)
{
    float3 dif, spc;
    sampleSkySplit(h, fr, wo, rng, BlueNoise.make(uint2(0u, 0u), 0u, 0u), depth, inWater, dif,
                   spc);
    return dif + spc;
}

// ---------------------------------------------------------------------------
// Aerial perspective, ray-marched through the world volume.
//
// WHY THIS IS A MARCH NOW AND USED TO BE A LOOKUP.
//
// The froxel grid was integrated front to back by a pass of its own, in a grid
// whose z axis WAS the view ray -- so a pixel's fog was a single fetch at its
// own depth. A world volume cannot do that: view rays cross it at every angle,
// so there is no axis to pre-integrate along. The integral has to be taken per
// pixel, here.
//
// That is the honest price of world-anchored fog, and it is not small: one
// fetch becomes kFogSteps of them. What it buys is fog that is the same fog
// from every direction, that does not swim when you turn, and whose brightness
// is not a function of how far you happen to be able to see.
//
// EXPONENTIAL STEPS, WHICH IS WHERE THE FROXEL GRID'S BEST PROPERTY COMES BACK.
// The volume is uniform in space, but the MARCH need not be uniform along the
// ray. Steps grow geometrically with distance, so the first few metres -- where
// a beam through the canopy is actually resolved -- take most of the samples,
// and the far haze, which is smooth, takes the few it needs. The same reasoning
// the old exponential slices used, moved from the storage to the integration.
//
// The transmittance is handed back separately from the composited result so the
// demodulated channels can be attenuated by the SAME number -- fog that
// disagreed between the two would show as a halo the moment the textures were
// multiplied back on.
// ---------------------------------------------------------------------------
static const int kFogSteps = 40;


// One sample of the volume: the near cascade if the point is inside it, the far
// one otherwise, nothing past the edge of both. The margin keeps a trilinear
// tap from reaching across the boundary and pulling in the clamped border.
bool fogSampleVolume(float3 p, out float4 s)
{
    s = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float3 uvw0 = fogWorldToUvw(p, gParams.fogOrigin0, float3(gParams.fogCell0));
    if (fogInside(uvw0, 0.002f)) {
        s = gFogVol0.SampleLevel(gFogSampler, uvw0, 0.0f);
        return true;
    }
    const float3 uvw1 = fogWorldToUvw(p, gParams.fogOrigin1, float3(gParams.fogCell1));
    if (fogInside(uvw1, 0.002f)) {
        s = gFogVol1.SampleLevel(gFogSampler, uvw1, 0.0f);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// WHAT A POINT LIGHT PUTS INTO THE AIR IN FRONT OF IT -- analytically.
//
// (user 2026-09-17: "can you make the spark voxel emit volumetric light like
//  the lightbulbs? this would effect the gun bullet and the spark animation.
//  but they use the same voxel anyway.")
//
// THE FIRST CUT OF THIS WAS A TERM INSIDE THE FOG MARCH AND IT COULD NOT HAVE
// WORKED. Two reasons, and the second is fatal: fogDensity is 0.001 per metre,
// so ten metres of clear air scatters about one per cent and a halo built out
// of it is invisible -- and volFog is switched OFF INDOORS (see tracer.h),
// which is precisely where the lamps and the rifle are. A glow that needs the
// world's haze is a glow that is absent from every room in the map.
//
// SO IT IS ITS OWN MEDIUM AND ITS OWN INTEGRAL. For a point light in a
// homogeneous medium the in-scattered radiance along a ray has a closed form,
// and it is one of the few in rendering that is genuinely exact:
//
//     integral of ds / (h^2 + s^2)  =  (1/h) * atan(s/h)
//
// where h is the perpendicular distance from the light to the ray and s runs
// along it from the light's closest approach. So the whole shaft costs two
// atans per light -- no march, no steps, no per-pixel jitter, and no shells to
// dither away. It is cheaper than the single step it replaces.
//
// OCCLUSION COMES FREE FROM THE SEGMENT. The integral runs only to `dist`, the
// first surface the eye ray hit, so a lamp on the far side of a wall
// contributes nothing beyond that wall. What it does still contribute is the
// air in FRONT of the wall, which is a real path (light goes round nothing, but
// the medium in front is genuinely lit by... nothing, in truth). That is the
// approximation, it is soft and small, and the alternative is a shadow ray per
// light per pixel.
//
// ISOTROPIC, DELIBERATELY. The fog march uses fogPhase for the sun because a
// sunbeam is forward-scattered and that is most of what makes it read as a
// beam. A lamp is surrounded by its glow from every side; giving it a lobe
// would make the halo depend on where you stand relative to a light that has
// no direction.
// ---------------------------------------------------------------------------

// How much the air scatters for THIS effect, per metre. Not fogDensity: see
// above. This is the knob that says how hazy a lit room looks. It is small
// enough that a lamp has a halo you can see and big enough that nothing else
// changes.
static const float kGlowSigma = 0.030f;

float3 pointLightGlow(float3 ro, float3 dir, float dist)
{
    if (isBlack(gParams.bulbRadiance) || gParams.bulbs[0].w < 0.0f)
        return float3(0.0f);
    const float t1 = min(dist, 60.0f);   // past this a lamp adds nothing visible
    if (t1 <= 0.0f)
        return float3(0.0f);
    float acc = 0.0f;
    for (int li = 0; li < kBulbSlots; ++li)
    {
        if (gParams.bulbs[li].w < 0.0f)
            break;
        const float3 toL = gParams.bulbs[li].xyz - ro;
        // Where along the ray the light is closest, and how far off it is
        // there. A light BEHIND the eye gives a negative tc, which the clamp
        // below turns into a segment that starts at 0 -- correct, not a
        // special case.
        const float tc = dot(toL, dir);
        const float h2 = max(dot(toL, toL) - tc * tc, 1e-4f);
        const float h = sqrt(h2);
        // The two ends of the visible segment, measured from closest approach.
        const float s0 = (0.0f - tc) / h;
        const float s1 = (t1 - tc) / h;
        acc += gParams.bulbs[li].w * (atan(s1) - atan(s0)) / h;
    }
    // 1/(4 pi) is the isotropic phase function; kGlowSigma is the medium.
    return gParams.bulbRadiance * acc * (kGlowSigma * 0.0795774715f * gParams.sparkGlow);
}
void fogTerms(float2 pixelUv, float3 dir, float dist, out float T, out float3 inscatter)
{
    T = 1.0f;
    inscatter = float3(0.0f);
    if (gParams.fogDensity <= 0.0f || gParams.volFog == 0)
        return;

    const float tNear = 0.25f;
    const float tFar = min(dist, gParams.fogFar);
    if (tFar <= tNear)
        return;

    // THE LIGHTING THAT DOES NOT VARY ALONG THE RAY, hoisted out of the loop.
    //
    // The sun's irradiance through the phase lobe for this view direction, and
    // the dome. Neither depends on where along the ray we are -- only on the
    // VISIBILITY of each, which is exactly what the volume stores. This is the
    // whole reason it can store visibility instead of radiance.
    //
    // wo points back toward the camera, which is the convention fogPhase is
    // written for; handing it the inward vector flips the lobe silently.
    const float3 wo = -dir;
    const float3 sunTerm = keyRadiance(gParams.sky) * keySolidAngle(gParams.sky) *
                           fogPhase(wo, keyDir(gParams.sky), gParams.fogAniso);
    const float3 skyTerm = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, float3(0.0f, 1.0f, 0.0f)) * gParams.fogAmbient;

    // A SPATIAL DITHER, AND DELIBERATELY NOT A TEMPORAL ONE.
    //
    // The geometric steps have to be broken up per pixel or they print
    // themselves on the screen as concentric shells. What they must NOT be
    // broken up by is TIME, and doing that was a bug that showed as the fog
    // ghosting through the trees whenever the camera moved quickly.
    //
    // The march writes its result into Lemit, which is handed to Ray
    // Reconstruction. RR accumulates temporally along the SURFACE motion
    // vectors -- but fog lives in the volume in FRONT of the surface, and its
    // parallax is not the surface's. Feed that a fresh white-noise value every
    // frame and RR has to reproject noise it cannot place: the error trails
    // behind, and it trails worst exactly where motion vectors are least
    // reliable and disocclusion is constant, which is a canopy. With the
    // denoiser off the accumulator simply averaged the noise away, which is why
    // the artefact vanished with upscaling disabled and looked like an
    // upscaler bug rather than a fog one.
    //
    // The temporal filtering this medium needs already exists, in the right
    // place: the VOLUME is blended over frames in VolFogInject. The march on
    // top of it should be a deterministic function of where the camera is, so
    // that a still camera yields a still image and a moving one yields only the
    // change that actually moved.
    //
    // Interleaved gradient noise: high frequency, well distributed, and fixed
    // per pixel. It breaks the shells as well as the hash did and gives RR a
    // signal that holds still.
    const float2 pixf = pixelUv * float2(gParams.dim);
    const float jitter =
        frac(52.9829189f * frac(0.06711056f * pixf.x + 0.00583715f * pixf.y));

    const float ratio = tFar / tNear;
    float3 scattering = float3(0.0f);
    float trans = 1.0f;
    float tPrev = 0.0f;

    for (int i = 1; i <= kFogSteps; ++i) {
        const float t = tNear * pow(ratio, float(i) / float(kFogSteps));
        const float ds = t - tPrev;
        const float3 p = gParams.cam.pos + dir * (tPrev + ds * jitter);
        tPrev = t;

        float4 s;
        if (!fogSampleVolume(p, s))
            continue;

        const float density = s.a;
        if (density <= 1e-6f)
            continue;

        const float stepT = exp(-density * ds);

        // THE ANALYTIC INTEGRAL OF THE STEP, not radiance times thickness.
        // Scattering and absorption happen together: light scattered at the far
        // end of a step is attenuated by the near end of the same step. The
        // closed form saturates, where multiplying through would let the fog
        // gain energy with density instead.
        const float3 stepL = sunTerm * s.r + skyTerm * s.g;

        scattering += stepL * (1.0f - stepT) * trans;
        trans *= stepT;

        // Nothing behind this is going to be seen through it.
        if (trans < 0.003f)
            break;
    }

    inscatter = scattering;
    T = trans;
}

float3 applyFog(float2 pixelUv, float3 L, float3 dir, float dist)
{
    float T;
    float3 inscatter;
    fogTerms(pixelUv, dir, dist, T, inscatter);
    return L * T + inscatter;
}

// ---------------------------------------------------------------------------
// THE WORDS STANDING IN THE PAUSE ROOM.
//
// A label is a rectangle of light at world coordinates with a bitmap cut out of
// it -- see V2Holo in Shared.slang. This is the intersection: a ray-plane test,
// a division into glyph cells, and one bit looked up. There is no texture, no
// sampler and no filter; a glyph pixel is 2.5 cm of world and the camera's own
// jitter antialiases its edges over the accumulation exactly as it does the
// edge of a voxel.
//
// WHAT IT IS PORTED FROM. v1 draws the x{n} beside the held tool this way --
// its GLYPH5 is eleven 25-bit masks and its blit asks `(GLYPH5[g] >> (y*5+x)) &
// 1` per pixel, which is the same question the last line below asks. The
// difference is the coordinate it is asked in: v1 has a raymarcher that shades
// one hit and stops, so its badge is rasterised on the CANVAS at a projected
// anchor and given a tilt and a shear to fake the angle. Here the quad is a
// real surface and the ray finds it, which is where the perspective, the
// occlusion, the depth and the motion vector all come from for nothing.
//
// PRIMARY RAYS ONLY, and that is deliberate rather than a saving. A label is
// not in the acceleration structure, so a bounce or a shadow ray that "hit" one
// would be an emitter no light sampler knows about -- fireflies on every
// diffuse bounce that happened to find a 2.5 cm letter, and a soft black shadow
// under every word, since nothing would pay for the light it stopped. Down the
// camera ray it is exactly what it looks like: a bright thing in front of a
// wall.
// ---------------------------------------------------------------------------
struct HoloHit
{
    bool hit;
    float t;
    float3 p;
    float3 n;
    float3 emit;
};

// The bitmap of glyph `gi`, written out rather than indexed. A constant buffer
// holds these four to a row (see V2Holo::bits) and a dynamic index into a vector
// is the one thing HLSL will happily compile into a scratch array in memory.
uint holoGlyphBits(V2Holo hg, int gi)
{
    const uint4 w = hg.bits[gi >> 2];
    const int k = gi & 3;
    return (k == 0) ? w.x : ((k == 1) ? w.y : ((k == 2) ? w.z : w.w));
}

// ---------------------------------------------------------------------------
// FALLING SNOW -- v1's flake lattice, ported.
//
// (user 2026-09-20: "the snow is supposed to come from the sky in the form of
//  one voxel snowflakes. make it snow, then the snow lands on the ground".)
//
// A FLAKE IS A REAL PRIMARY HIT, which is v1's own decision and the reason the
// weather sits in the world instead of on top of it: "flakes are REAL VOXELS
// in the primary trace: grain, the jittered sun ray, AO, fog and the denoiser
// treat them exactly like the static world". Here that means the march runs
// after the world trace and OVERWRITES the hit when a flake is nearer -- so a
// flake is lit by the sun and the sky through the ordinary path, casts into
// the guides, is fogged at its own depth and is reprojected like anything
// else. Nothing downstream knows it is weather.
//
// IT IS A LATTICE, NOT A PARTICLE SYSTEM. One flake per cell of a fixed grid,
// its position and phase hashed from the cell's integer coordinates -- so the
// field is infinite, costs no memory, needs no spawning or recycling, and is
// identical from every camera without anything being synchronised. The CPU
// never hears about it.
//
// A DDA AND NOT A FIXED-STEP MARCH, and the arithmetic is the argument: a
// flake is one voxel in a cell nearly nine times its width, so a step fine
// enough to land on one would be ~260 per pixel. Walking cell to cell and
// testing each flake EXACTLY is about thirty iterations for the same answer.
//
// THE FALL IS INSIDE THE CELL. Each flake descends through its own cell and
// wraps, so the lattice itself never moves -- which is what keeps the field
// seamless and stops the whole sky shearing when the clock wraps.
// ---------------------------------------------------------------------------
static const float kFlakeCellM = 0.85f;   // one flake to this much air
static const float kFlakeHalfM = 0.05f;   // ...and it is ONE VOXEL across: 10 cm
static const float kFlakeRangeM = 22.0f;  // past this the march stops
static const int kFlakeSteps = 30;        // cells walked at most
static const float kFlakeFallMS = 1.7f;   // metres a second, straight down
static const float kFlakeDriftM = 0.35f;  // ...and how far the wind leans it
// ...AND HOW MANY CELLS CARRY ONE. Raised from a quarter once the march
// stopped testing each cell's NEIGHBOURS: that sweep was finding flakes in
// cells the ray never entered, so it was inflating the apparent density by
// about three. Removing it was right -- the DDA is exact -- and this is the
// density that sweep had been faking, asked for honestly.
// ...AND A THIRD OF THEM. 0.70 was measured on screen and is a blizzard you
// cannot see the wood through; a third is heavy snow with the hillside still
// legible behind it.
static const float kFlakeFull = 0.22f;
// HOW CLOSE A FLAKE MAY BE DRAWN -- see the cull that reads it.
static const float kFlakeNearM = 1.0f;

struct FlakeHit
{
    bool hit;
    float t;
    float3 p;
    float3 n;
};

// HOW FAR THE WIND HAS PUSHED THE WHOLE FIELD, this instant. A function of
// the clock alone, so it is the same offset for every flake in the sky --
// which is what lets the march subtract it from the RAY once instead of
// adding it to each flake and then hunting for flakes in neighbouring cells.
float2 flakeLean()
{
    return float2(sin(gParams.snowTime * 0.6f), cos(gParams.snowTime * 0.41f)) *
           kFlakeDriftM;
}

uint flakeHash(int3 c)
{
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(c.z) * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Where the flake belonging to this cell is, right now.
// -- A FLAKE DOES NOT MOVE. THE FIELD DOES. -------------------------------
//
// (user 2026-09-20: "stop making the snow flicker" -- twice.)
//
// EACH FLAKE USED TO FALL THROUGH ITS OWN CELL AND WRAP BACK TO THE TOP,
// which is the flicker and it is not subtle: the cell is 0.85 m and the fall
// is 1.7 m/s, so EVERY flake in the sky teleported a cell upwards twice a
// second. A motion vector cannot help with that -- the flake genuinely is
// somewhere else -- and with thousands on screen the popping never stops.
//
// So the lattice is STATIC and the ray is moved through it instead, exactly
// as the wind is handled: a flake is a fixed point, the field slides down
// past the camera for ever, and there is no wrap to see. The offset is split
// into whole cells and a remainder -- the whole cells go into the cell INDEX,
// where an int carries them exactly, and only the remainder moves the ray.
// Without that split the offset grows without bound and the field loses
// precision after a few hours of snow.
//
// jy IS WHY THEY ARE NOT ALL ON ONE SHELF. With no vertical jitter every
// flake sits at the same height in its cell and the sky comes out in layers.
float3 flakeAt(int3 c)
{
    const uint h = flakeHash(c);
    const float jx = float(h & 0xFFu) * (1.0f / 255.0f);
    const float jz = float((h >> 8) & 0xFFu) * (1.0f / 255.0f);
    const float jy = float((h >> 16) & 0xFFu) * (1.0f / 255.0f);
    const float fall = 1.0f - jy;
    // NO LEAN IN HERE -- see flakeLean. It used to be a sine of the clock AND
    // the cell's own y, which meant a flake could sit outside the cell that
    // owns it, which meant the march had to test the neighbours either side
    // of every cell it walked. That is three times the work for a wind that
    // blows every flake the same way anyway.
    return float3((float(c.x) + jx) * kFlakeCellM,
                  (float(c.y) + 1.0f - fall) * kFlakeCellM,
                  (float(c.z) + jz) * kFlakeCellM);
}

// HOW FAR A FLAKE TRAVELLED SINCE THE LAST FRAME -- see the motion vector
// note where this is used. Every flake in the sky moves by the same vector:
// the fall is a constant and the wind is a function of the clock alone, so
// this needs no knowledge of WHICH flake it is asked about.
//
// EXACT NOW, not almost. While each flake fell inside its own cell this was
// a half-truth -- right until the flake wrapped, which it did twice a second.
// The field is a rigid body: every flake in the sky moves by this one vector,
// every frame, with nothing to except.
float3 flakeMoved()
{
    const float2 now = flakeLean();
    const float dt = max(0.0f, gParams.snowStep);
    const float t0 = gParams.snowTime - dt;
    const float2 was = float2(sin(t0 * 0.6f), cos(t0 * 0.41f)) * kFlakeDriftM;
    return float3(now.x - was.x, -kFlakeFallMS * dt, now.y - was.y);
}

// The nearest flake along [0, tMax), walked cell by cell.
FlakeHit flakeNearest(float3 o, float3 d, float tMax)
{
    FlakeHit out_;
    out_.hit = false;
    out_.t = tMax;
    out_.p = float3(0.0f);
    out_.n = float3(0.0f, 1.0f, 0.0f);
    if (gParams.snowFall <= 0.0f) return out_;

    const float reach = min(tMax, kFlakeRangeM);
    if (reach <= 0.0f) return out_;

    // HOW MANY CELLS CARRY A FLAKE AT ALL. Thinning the lattice is what
    // starting and stopping a storm looks like -- see V6Params::snowFall.
    //
    // kFlakeFull IS A QUARTER AND NOT ALL OF THEM. A flake in EVERY cell is
    // one per 0.61 m^3, and looking up through twenty-six metres of that is a
    // solid ceiling -- captured, and it read as a wall rather than as
    // weather. A quarter is one flake per 2.4 m^3, which is heavy snow you
    // can still see the sky through.
    const uint keep = uint(saturate(gParams.snowFall) * kFlakeFull * 255.0f);

    // THE WIND IS TAKEN OUT OF THE RAY, NOT PUT INTO THE FIELD. Moving the
    // origin the other way is exactly equivalent -- the lattice is infinite,
    // so a field pushed east and a ray pushed west meet in the same places --
    // and it keeps every flake inside the cell that owns it, which is what
    // makes one test per step correct.
    const float2 wind = flakeLean();
    // THE FALL, SPLIT -- see flakeAt. The remainder moves the ray; the whole
    // cells are added to the index at the bottom of this function, where they
    // cost nothing and never lose precision.
    const float fallM = gParams.snowTime * kFlakeFallMS;
    const float fallCellsF = floor(fallM / kFlakeCellM);
    const int fallCells = int(fallCellsF);
    const float fallFrac = fallM - fallCellsF * kFlakeCellM;
    o -= float3(wind.x, -fallFrac, wind.y);
    int3 c = int3(floor(o / kFlakeCellM));
    const int3 stepI = int3(d.x < 0.0f ? -1 : 1, d.y < 0.0f ? -1 : 1, d.z < 0.0f ? -1 : 1);
    // A ray exactly along an axis has no crossing on the others; a huge tNext
    // is the standard way to say "never" without a branch per axis per step.
    const float3 inv = float3(abs(d.x) < 1e-6f ? 1e30f : 1.0f / d.x,
                              abs(d.y) < 1e-6f ? 1e30f : 1.0f / d.y,
                              abs(d.z) < 1e-6f ? 1e30f : 1.0f / d.z);
    float3 tNext;
    tNext.x = ((float(c.x + max(stepI.x, 0)) * kFlakeCellM) - o.x) * inv.x;
    tNext.y = ((float(c.y + max(stepI.y, 0)) * kFlakeCellM) - o.y) * inv.y;
    tNext.z = ((float(c.z + max(stepI.z, 0)) * kFlakeCellM) - o.z) * inv.z;
    const float3 tDelta = abs(float3(kFlakeCellM, kFlakeCellM, kFlakeCellM) * inv);

    float t = 0.0f;
    for (int i = 0; i < kFlakeSteps && t < reach; ++i)
    {
        // -- do/while(false), AND IT IS LOAD-BEARING --------------------
        //
        // THE TESTS BELOW BAIL WITH `continue` and there has to be something
        // for them to continue. This was a `for k = -1..1` sweep over the
        // neighbouring cells; taking the sweep out and leaving a plain block
        // silently re-pointed every one of those at the DDA loop -- so the
        // first cell that held no flake skipped the ADVANCE, and the march
        // spent all thirty steps re-testing the cell it started in. It
        // compiled, it ran, and it drew no snow at all.
        //
        // A do/while(false) is the idiom for "a block you can jump out of":
        // continue goes to the condition, the condition is false, and control
        // lands on the advance below, which is what the sweep used to do.
        do
        {
            // THE WHOLE CELLS THE FIELD HAS FALLEN, carried in the index so
            // the ray offset stays inside one cell for ever.
            const int3 cc = int3(c.x, c.y + fallCells, c.z);
            if ((flakeHash(cc) >> 24) >= keep) continue;
            const float3 fp = flakeAt(cc);
            // -- A VOXEL, NOT A BALL -------------------------------------
            //
            // (user 2026-09-20: "make the snowflakes actual 10cm voxels
            //  instead of circles.")
            //
            // A SPHERE WAS THE CHEAPER TEST AND THE WRONG SHAPE. Everything
            // else in this world is a cube of exactly this size, and a
            // rounded flake is the one object in frame that does not belong
            // to the grid -- which is the whole look. The slab test is three
            // more lines and no more expensive in any way that matters.
            //
            // AXIS-ALIGNED, LIKE EVERY OTHER VOXEL. A tumbling flake would
            // want an oriented box and a rotation per cell per step; snow
            // falling flat is what the rest of the world looks like.
            const float3 lo3 = fp - kFlakeHalfM;
            const float3 hi3 = fp + kFlakeHalfM;
            const float3 t0 = (lo3 - o) * inv;
            const float3 t1 = (hi3 - o) * inv;
            const float3 tsm = min(t0, t1);
            const float3 tbg = max(t0, t1);
            const float tEnter = max(max(tsm.x, tsm.y), tsm.z);
            const float tExit = min(min(tbg.x, tbg.y), tbg.z);
            if (tExit < tEnter || tExit <= 0.0f) continue;
            const float hitT = tEnter;
            // -- NOTHING HITS YOU IN THE FACE ------------------------
            //
            // A flake born at the eye is a 10 cm cube a hand's breadth from
            // the lens, which fills a quarter of the screen with flat white --
            // captured, and it reads as a graphical fault rather than as
            // weather. v1 culls the same case in the same place ("a flake
            // that reaches the eye hits your face"). One metre is past the
            // near plane and past anything the player can be inside of.
            if (hitT <= kFlakeNearM || hitT >= out_.t) continue;
            out_.hit = true;
            out_.t = hitT;
            // ...AND BACK INTO THE WORLD, because `o` was shifted above.
            out_.p = o + d * hitT + float3(wind.x, -fallFrac, wind.y);
            // WHICH FACE, from which slab was the last to open -- the
            // standard read, and it is what makes a cube light like a cube
            // instead of like a ball with a flat shader on it.
            out_.n = (tEnter == tsm.x) ? float3(d.x < 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f)
                   : (tEnter == tsm.y) ? float3(0.0f, d.y < 0.0f ? 1.0f : -1.0f, 0.0f)
                                       : float3(0.0f, 0.0f, d.z < 0.0f ? 1.0f : -1.0f);
        } while (false);
        // ...and on to the next cell.
        if (tNext.x < tNext.y && tNext.x < tNext.z) { t = tNext.x; tNext.x += tDelta.x; c.x += stepI.x; }
        else if (tNext.y < tNext.z)                 { t = tNext.y; tNext.y += tDelta.y; c.y += stepI.y; }
        else                                        { t = tNext.z; tNext.z += tDelta.z; c.z += stepI.z; }
        // A flake already found nearer than the next cell boundary is the
        // answer -- nothing further along the ray can beat it.
        if (out_.hit && out_.t < t) break;
    }
    return out_;
}

// The nearest lit glyph pixel along [0, tMax), or no hit.
HoloHit holoNearest(float3 o, float3 d, float tMax)
{
    HoloHit out_;
    out_.hit = false;
    out_.t = tMax;
    out_.p = float3(0.0f);
    out_.n = float3(0.0f, 0.0f, 1.0f);
    out_.emit = float3(0.0f);

    [unroll]
    for (int s = 0; s < kHoloSlots; ++s)
    {
        const V2Holo hg = gParams.holo[s];
        // cell IS the on switch -- see the note on V2Tracer::clearHolos. In the
        // wood all three are zero and this is where every camera ray turns back.
        if (hg.cell <= 0.0f || hg.count == 0u)
            continue;

        const float3 n = cross(hg.right, hg.up);
        const float dn = dot(d, n);
        // Edge on. Not a special case worth handling: a plane seen exactly edge
        // on covers no pixels, so the honest answer is that it is not there.
        if (abs(dn) < 1e-6f)
            continue;
        const float t = dot(hg.org - o, n) / dn;
        if (t <= 1e-4f || t >= out_.t)
            continue;

        // Into the block's own frame. org is the TOP-LEFT corner, so v counts
        // DOWNWARD and the glyph rows are in the order holotext.h draws them.
        const float3 lp = o + d * t - hg.org;
        const float u = dot(lp, hg.right) / hg.cell;
        const float v = -dot(lp, hg.up) / hg.cell;
        if (v < 0.0f || v >= float(kHoloRows) || u < 0.0f)
            continue;

        const int cell = int(u);
        const int gi = cell / kHoloAdvance;
        if (gi >= int(hg.count))
            continue;
        const int cx = cell - gi * kHoloAdvance;
        if (cx >= kHoloCols)
            continue;   // the blank column that separates two glyphs
        const int cy = int(v);
        if ((holoGlyphBits(hg, gi) >> uint(cy * kHoloCols + cx) & 1u) == 0u)
            continue;

        out_.hit = true;
        out_.t = t;
        out_.p = o + d * t;
        // TOWARD THE EYE, WHICHEVER SIDE IT IS. The quad is two-sided -- it is a
        // hologram, and the room is small enough to walk behind one -- and a
        // normal pointing away would hand the denoiser a back-facing surface.
        out_.n = (dn < 0.0f) ? n : -n;
        out_.emit = hg.tint;
    }
    return out_;
}

// ---------------------------------------------------------------------------
// One camera path, start to finish.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gParams.dim.x || tid.y >= gParams.dim.y)
        return;
    const uint2 pix = tid.xy;
    const uint pixIndex = pix.y * gParams.dim.x + pix.x;

    // One PCG stream per pixel, advanced by the frame index. Seeded from the
    // monotonic tick, not the accumulation index: while the camera moves the
    // accumulation restarts every frame, and seeding from it would redraw the
    // SAME sample over and over.
    Rng rng;
    rng.init(
        uint64_t(pixIndex) + 1ull,
        uint64_t(gParams.tick) * 0x9E3779B97F4A7C15ull + uint64_t(gParams.seed)
    );

    // The blue-noise mask, keyed on the PIXEL and the TICK.
    //
    // The tick and not the accumulation index, for exactly the reason the rng
    // seed above uses it: while the camera moves the film restarts every frame,
    // and an index that keeps returning to zero would freeze the mask into a
    // fixed dither -- the one failure mode a blue-noise pattern has, and a far
    // uglier one than the white noise it replaced.
    const BlueNoise bn = BlueNoise.make(pix, gParams.tick, uint(gParams.blueNoise));

    // The jitter is a whole-frame offset from a Halton sequence, not a per-pixel
    // random one. It is what antialiases the image as samples accumulate: a
    // low-discrepancy sequence covers the pixel evenly in a few dozen frames,
    // where white noise leaves parts of it unvisited for runs at a time.
    const float px = float(pix.x) + 0.5f + gParams.jitter.x;
    const float py = float(pix.y) + 0.5f + gParams.jitter.y;

    const V6Camera c = gParams.cam;
    const float sx = (2.0f * px / float(gParams.dim.x) - 1.0f) * c.halfW;
    const float sy = (1.0f - 2.0f * py / float(gParams.dim.y)) * c.halfH;

    float3 o = c.pos;
    float3 d = normalize(c.w + c.u * sx + c.v * sy);
    if (c.aperture > 0.0f)
    {
        // Everything on the focal plane stays put; only the origin moves, which
        // is what puts the plane in focus and blurs by distance from it.
        const float3 focal = o + d * (c.focusDist / dot(d, c.w));
        // THE JITTER ABOVE IS DELIBERATELY LEFT ALONE. It is one offset for the
        // whole frame, which is what DLSS is told it is; making it per-pixel
        // would be a blue-noise pattern the upscaler is actively compensating
        // for in the wrong direction. The lens is a different matter -- it is a
        // genuine per-pixel 2D sample and bokeh noise is exactly the kind the
        // mask disperses well.
        const float2 l = sampleConcentricDisk(bn.next2D(kBnDimLens, rng));
        o = o + c.u * (l.x * c.aperture * 0.5f) + c.v * (l.y * c.aperture * 0.5f);
        d = normalize(focal - o);
    }

    const float3 primaryDir = d;

    float3 L = float3(0.0f), beta = float3(1.0f);

    // -- PHASE B: the same radiance, kept in three parts ---------------------
    //
    // WHAT wDiff IS. Past the primary vertex nobody tracks lobes any more: the
    // path carries one throughput and the light that comes back down it is
    // whatever it is. But that light was let through the primary surface BY a
    // lobe, and which one decides whether the albedo or the Fresnel term is the
    // factor to divide out. sampleOpaque draws a direction from one lobe and
    // then evaluates BOTH -- one-sample MIS over the lobes -- so the honest
    // answer is not "diffuse" or "specular" but the fraction each contributed
    // at the sampled direction. That fraction is wDiff, computed once at depth
    // 0, and every later contribution is split by it.
    //
    // Set to 1 up front so a path that never gets a primary bounce -- the ray
    // escapes, or the BSDF is black -- puts what it has in the diffuse channel
    // rather than nowhere.
    float3 Ldiff = float3(0.0f), Lspec = float3(0.0f), Lemit = float3(0.0f);
    float wDiff = 1.0f;
    float3 demodAlbedo = float3(0.0f);

    // The guides, filled in from the primary hit below. Held here rather than
    // written where they are found so there is exactly one store per texture
    // per pixel, on a path that also has to handle the ray escaping.
    float3 gAlbedo = float3(0.0f), gSpecular = float3(0.0f), gNormal = -primaryDir;
    float gRough = 1.0f, gViewZ = 0.0f;
    bool specularBounce = true; // the camera ray itself carries no light pdf
    float bsdfPdf = 1.0f;
    // The pdf the DOME sampler would have had for the direction this path is
    // currently travelling, carried forward from the vertex that chose it so
    // the escape branch can weight the two strategies against each other.
    // Zero means "no light sampler could have produced this direction", which
    // is the truth for the camera ray, for a specular bounce, and for every
    // frame where sky NEE is switched off.
    float skyPdfPrev = 0.0f;
    // STARTS TRUE IF THE EYE IS ALREADY UNDER. `inWater` could only ever be set
    // by CROSSING the surface, so swimming looked like standing in air until you
    // surfaced and dived again. gParams.waterY sits far below the world where
    // the band has no water, so this is false everywhere it should be.
    bool inWater = c.pos.y < gParams.waterY;
    // WHERE THIS PATH LAST CROSSED A WATER SURFACE. Seeded with the frame's
    // global for a camera that starts submerged -- it never crossed one, so
    // there is nothing better to say -- and replaced by the real quad's y the
    // first time the path refracts through one. See Hit::waterTop.
    float waterEntryY = gParams.waterY;
    float primaryDist = -1.0f;
    // Whether the primary vertex is the tool in the player's hand. Read once,
    // by the motion vector -- see the note where it is used.
    bool heldPrimary = false;
    // ...and, for the same reader, how far the thing under this pixel moved
    // since the last frame. Zero unless the primary ray landed on a flyer.
    float3 primaryMoved = float3(0.0f);

    // Beer-Lambert extinction under water, from the per-metre transmittance the
    // material carries. Hoisted out of the loop: it is three logs of constants.
    // v1's RATIO, which is the whole of "make the water more blue".
    //
    // What was here -- -log(0.42/0.62/0.55) -- absorbed BLUE harder than green
    // and was tuned when no lake in this engine was ever visible, so nothing
    // ever showed it up. Real water, and v1's, kills red first and lets blue
    // carry: v1 uses (0.24, 0.092, 0.0416) per 10 cm voxel, i.e. (2.4, 0.92,
    // 0.416) per metre, a 5.8:1 spread from red to blue against the 1.45:1 this
    // had -- in the wrong direction.
    //
    // SCALED TO HALF, AND ONLY THE SCALE. v1's lakes average 2.5 m deep and
    // these reach 9.4, where v1's coefficients leave 2% of blue and the water
    // reads as ink. Every channel is halved so the RATIO -- the colour -- is
    // exactly v1's while the water stays see-through at the depths this world
    // actually has: at 5 m, red 0.2%, green 10%, blue 35%.
    const float3 waterSigma = float3(1.20f, 0.46f, 0.208f);

    // -- the neural radiance cache: is this a training path? -----------------
    //
    // HASHED, NOT DRAWN FROM rng. Taking a number out of the pixel's PCG stream
    // would shift every random number after it, so switching the cache on would
    // change the image even in the frames where it predicts nothing. This has to
    // be a decision the renderer can make without disturbing anything else.
    // The primary vertex, kept for ReSTIR: resampling shades x1 using a sample
    // point that may have been found by a different pixel, so it needs to
    // re-evaluate the geometry from HERE rather than reuse the throughput the
    // path happened to carry.
    float3 primaryPos = float3(0.0f), primaryNormal = float3(0.0f, 1.0f, 0.0f);
    bool havePrimary = false;

    // The first indirect vertex, which is the candidate ReSTIR resamples and
    // also the point the radiance cache is asked about. One record serves both.
    bool giHaveVertex = false;
    float3 giPos = float3(0.0f), giNormal = float3(0.0f, 1.0f, 0.0f);
    float3 giBeta = float3(1.0f), giLBefore = float3(0.0f);

#ifdef V2_NRCSDK
    // THE CACHE'S OWN VIEW OF THIS PATH, carried alongside v2's beta and L.
    //
    // NrcContext is per pixel and per sample; NrcPathState is per path and is
    // what every call below threads through. The random number seeds the
    // library's own decision about which vertices to keep for training, so it
    // has to come from the same stream the rest of the path uses or the two
    // disagree about what this pixel is doing.
    NrcBuffers nrcSdkBuffers;
    NrcContext nrcSdkCtx = NrcCreateContext(gNrcConstants, nrcSdkBuffers, pix);
    NrcSetSampleIndex(nrcSdkCtx, 0u);
    NrcPathState nrcSdkPath = NrcCreatePathState(gNrcConstants, rng.nextFloat());
    bool nrcSdkStopAfterDirect = false;
#endif

#ifdef V2_NRC
    const uint nrcHash = (uint(pixIndex) * 73856093u) ^ (uint(gParams.tick) * 19349663u);
    const bool nrcTraining =
        gParams.nrcMode != 0 && (nrcHash % uint(max(1, gParams.nrcTrainEvery))) == 0u;
    bool nrcHaveVertex = false;
    float3 nrcBeta = float3(1.0f), nrcLBefore = float3(0.0f);
    NrcQuery nrcQ;
#endif

    for (int depth = 0; depth < gParams.maxDepth; ++depth)
    {
        Hit h;
        if (depth == 0)
        {
            // -- THE TOOL WINS THE PIXEL, WHATEVER IS IN FRONT OF IT --------
            //
            // Two traces, one per mask, and the held one is taken whenever it
            // hit at all rather than when it is nearer. See the note on the
            // masks in Shared.slang: at ten centimetres from the lens the world
            // is routinely closer than a hand, and letting the nearest hit win
            // puts the axe inside every trunk you lean on.
            //
            // ONLY AT DEPTH 0. Every bounce past this one passes both masks, so
            // the tool shadows, occludes and bounces light exactly as the
            // geometry it is -- which is the entire reason it is in the
            // structure rather than drawn over the top.
            const Hit hw = traceRadiance(o, d, MASK_WORLD);
            // (A bounding-sphere test to skip this second trace was measured
            //  2026-09-23 and bought nothing: the held mask culls every other
            //  instance at the top level almost for free -- +2% and -1% in two
            //  phases of the same run, inside the noise. Not worth a field.)
            const Hit hh = traceRadiance(o, d, MASK_HELD);
            h = hh.hit ? hh : hw;

            // -- AND THE BURIED TOOL DOES NOT GO BLACK --------------------
            //
            // The mask trick above wins the PIXEL for the tool; it does not
            // move the tool. Lean on a trunk and the axe is genuinely inside
            // it, so every ray cast from that surface -- the shadow ray to the
            // sun, every bounce -- starts inside wood and hits the inside of
            // wood. The answer comes back black, and it is not wrong: that is
            // what a surface buried in a tree is lit like.
            //
            // It is still not what should be drawn. A viewmodel intersecting
            // the world is an artefact of bolting an object to the camera, not
            // a situation the wood is in, and a black axe reads as a bug
            // because it is one.
            //
            // SO THE SHADING POINT COMES OUT, and only the point. The pixel is
            // still the tool's, the normal is still the tool's, the depth and
            // the motion vector are untouched -- what moves is where the
            // lighting is gathered FROM, to just in front of whatever the tool
            // is inside. That place is open air by construction, because the
            // ray reached it. The tool is then lit as though it were resting
            // against the trunk rather than inside it, which is exactly what it
            // looks like it is doing.
            //
            // Gated on the world being NEARER than the tool, which is the only
            // case this can fire in -- a tool standing in open shade is not
            // behind anything and keeps the shadow it is honestly in.
            if (hh.hit && hw.hit && hw.t < hh.t)
                h.p = o + d * max(0.0f, hw.t - 0.01f);
        }
        else
        {
            h = traceRadiance(o, d);
        }

        // -- ...AND A SNOWFLAKE IN FRONT OF IT ---------------------------
        //
        // THE WEATHER IS SPLICED INTO THE HIT, not drawn over the picture --
        // see flakeNearest. Everything below this line goes on as though the
        // ray had struck a small white surface in mid air, which is what it
        // did: the BSDF shades it, the sun ray shadows it, the sky lights it,
        // the fog fogs it at its own depth and the guides carry its normal so
        // the denoiser does not smear it across the trees behind.
        //
        // PRIMARY ONLY. A bounce ray that could hit a flake would have snow
        // shadowing the wood and lighting it back, which is a metre-scale
        // effect from something a tenth of a metre across -- and it would put
        // this march on every ray in the frame. v1 draws flakes in the
        // primary trace for the same two reasons.
        //
        // NOT THROUGH GLASS OR WATER either: `through` above is the refracted
        // segment and a flake found on it would be snow INSIDE the lake.
        if (depth == 0 && gParams.snowFall > 0.0f && !inWater)
        {
            const FlakeHit fh = flakeNearest(o, d, h.hit ? h.t : kFlakeRangeM);
            if (fh.hit)
            {
                h.hit = true;
                h.t = fh.t;
                h.p = fh.p;
                h.ng = fh.n;
                h.ns = fh.n;
                // -- AND ITS SURFACE, WHICH IS THE FIELD THAT SHADES ------
                //
                // h.mtl IS NOT THE MATERIAL. It is the neural cache's
                // discrete surface identity and nothing else reads it; what
                // every BSDF, every guide and every light sample asks is
                // Hit::mat. Setting the id alone left `mat` as the SKY ray's
                // zeroed struct -- albedo 0, specular 0 -- and a black body
                // lit by a noon sun is still black. The flakes drew as a
                // field of black blobs against the clouds, which is exactly
                // what an albedo of zero looks like and nothing to do with
                // the march that found them.
                const V6Material sm = gMaterials[gParams.matBase + kSnow0];
                h.mtl = kSnow0;
                h.mat.type = MAT_OPAQUE;
                h.mat.albedo = sm.albedo;
                h.mat.roughness = sm.roughness;
                h.mat.specular = sm.specular;
                // -- THE MATERIAL'S OWN, WITH NOTHING ADDED -------------
                //
                // (user 2026-09-20: "put the snow under the same lighting
                //  pass as everything else.")
                //
                // A translucency of 0.55 was invented here on the argument
                // that one crystal is lit from behind where a metre of lying
                // snow is not. That may be true of real snow and it made the
                // flakes brighter than any other white in the frame, which
                // reads as a second lighting model rather than as a material.
                // The entry is mat::SNOW_0 either way; now it is that entry
                // and nothing else, shaded by the one path every surface in
                // this world goes through.
                h.mat.translucency = sm.translucency;
                h.mat.alpha = 1.0f;
                h.mat.ior = 0.0f;
                h.waterTop = 0.0f;
                h.foam = 0.0f;
                h.held = false;
                h.hurt = 0.0f;
                // -- ...AND HOW FAR IT MOVED SINCE LAST FRAME ------------
                //
                // (user 2026-09-20: "stop making the snow flicker.")
                //
                // THIS WAS ZERO AND THAT IS THE FLICKER. `moved` is what the
                // motion vector is built from, and zero means "this surface
                // is part of the static world" -- so Ray Reconstruction
                // looked up last frame's colour at the pixel a STATIONARY
                // point would have been at, found the wood behind the flake,
                // and fought with itself every frame. A tenth of a metre
                // across and falling nearly two metres a second is the worst
                // possible case for that.
                //
                // The fall and the wind are both exact functions of the
                // clock, so where the flake WAS is not an estimate: it is the
                // same arithmetic one snowStep earlier.
                h.moved = flakeMoved();
            }
        }

        // -- A SURFACE YOU CAN SEE THROUGH ---------------------------------
        //
        // "Make their wings 50% transparent." A fly's wing is the only material
        // in this world with alpha under 1, and this is the whole mechanism:
        // with probability 1 - alpha the ray simply CARRIES ON as though the
        // surface were not there, and we trace again from just past it.
        //
        // -- AND AS OF 2026-09-14 NOTHING IN THE WORLD SETS alpha BELOW 1 ----
        //
        // "can you just make the fly wings a solid white voxel. just like
        // everything else in the world." The wing was this block's only
        // subject, through two designs -- a flat 50% pass-through and then a
        // Fresnel dielectric -- and it is now an ordinary opaque white voxel
        // with no material of its own at all.
        //
        // So this is dead until something wants it again. It is kept rather
        // than cut because it costs exactly one compare on a ray that hit
        // something, and because the mechanism is sound and was not the
        // problem; what was wrong was having it at all. If nothing has claimed
        // it by the next time somebody reads this, delete it.
        //
        // STOCHASTIC RATHER THAN BLENDED, and in a path tracer that is not a
        // compromise -- it is the cheaper and more correct of the two. A blend
        // needs the surface behind the wing, which means tracing it anyway, and
        // then has to decide what a half-wing does to a motion vector and to
        // the denoiser's guides. Passing through means the pixel is EITHER the
        // wing or what is behind it, every guide it writes is that surface's
        // own, and the average over the samples the accumulator and Ray
        // Reconstruction already take is the blend.
        //
        // IT DOES NOT SPEND A BOUNCE. This sits before `depth` is used for
        // anything, so a ray that slips through three wings still has its full
        // path length -- a wing is not an interaction, it is a gap.
        //
        // AND THE DISTANCE IS KEPT. h.t comes back measured from the new
        // origin, so the skipped run is added on: primaryDist, the motion
        // vector and gViewZ all read h.t, and a wing three centimetres thick
        // would otherwise pull the whole surface behind it that much nearer.
        //
        // THE CAP IS THREE. Two wings and a margin; past that the ray takes the
        // surface whatever its alpha, which is invisible at any thickness a
        // wing has and bounds the worst case.
        if (h.hit && h.mat.alpha < 1.0f)
        {
            float skipped = 0.0f;
            [loop] for (int wing = 0; wing < 3; ++wing)
            {
                if (!h.hit || h.mat.alpha >= 1.0f) break;
                if (rng.nextFloat() < h.mat.alpha) break;   // this one stopped it
                // Past the face, not on it: a ray restarted exactly at the hit
                // finds the same triangle again.
                const float3 through = h.p + d * 0.004f;
                skipped += h.t + 0.004f;
                // WORLD ONLY. The held tool has already won or lost the pixel
                // in the branch above, and re-running that argument here would
                // let an axe reappear behind a wing it was never in front of.
                h = traceRadiance(through, d, depth == 0 ? MASK_WORLD : 0xFFu);
            }
            h.t += skipped;
        }

        // -- AND A LABEL IN FRONT OF ALL OF IT TAKES THE PIXEL FROM BOTH ----
        //
        // The words over the pause room's buttons -- see holoNearest above, and
        // V2Holo in Shared.slang. They are not in the acceleration structure, so
        // they are tested here against whatever the structure found and win when
        // they are nearer.
        //
        // IT IS A SURFACE, NOT A SPRITE, and the four lines of guide below are
        // the whole reason that distinction is worth anything. A label written
        // over the finished image would have to be reprojected by the DEPTH OF
        // THE WALL BEHIND IT, because that is what the motion vector under it
        // says -- and the plane stands 25 cm proud of that wall, so every step
        // sideways would drag a ghost of the word across it under Ray
        // Reconstruction. Handing over the plane's own depth, its own normal and
        // its own hit point makes the reprojection exact, and it costs four
        // assignments because the path simply ENDS here.
        //
        // IT ENDS HERE BECAUSE IT IS OPAQUE, which is the one place this departs
        // from what a hologram usually means. An additive, see-through word is
        // the obvious reading of the brief and it is unreadable in THIS room:
        // the walls are white at 226 lit to a mean of 180, and adding light to a
        // surface that is already near the top of the range moves it almost
        // nowhere. A saturated emitter that owes the room nothing is the only
        // thing that stays legible against it -- and it is still light rather
        // than paint, because it is brighter than the wall and blooms.
        //
        // NO ALBEDO AND NO SPECULAR IN THE GUIDES. The denoiser demodulates by
        // albedo and a light has none worth dividing out; this is the same entry
        // the bulb makes a few lines below, and for the same reason.
        if (depth == 0)
        {
            const HoloHit hg = holoNearest(o, d, h.hit ? h.t : 1e30f);
            if (hg.hit)
            {
                primaryDist = hg.t;
                gViewZ = dot(hg.p - c.pos, c.w);
                gNormal = hg.n;
                gAlbedo = float3(0.0f);
                gSpecular = float3(0.0f);
                gRough = 1.0f;
                demodAlbedo = float3(1.0f);
                wDiff = 1.0f;
                L += beta * hg.emit;
                Lemit += beta * hg.emit;
                break;
            }
        }

        if (inWater)
        {
            // -----------------------------------------------------------------
            // ABSORPTION *AND* IN-SCATTER, and the second half is not optional.
            //
            // Beer-Lambert alone is a FILTER: it can only ever darken what is
            // behind it, so over a shaded lakebed it makes black water, which is
            // exactly what "still not blue" was. The blue of real water is not
            // mostly the bed seen through a blue filter -- it is light scattered
            // back out of the water column itself, and a lake over dark mud is
            // still blue because of it. v1 says the same thing in one line:
            // "Beer-Lambert with no in-scatter is a black lake, and in-scatter
            // with no absorption is fog over a perfectly clear one."
            //
            // So the segment gets both, in the right order: what the water adds
            // over this distance is deposited BEFORE beta is attenuated by it,
            // because the light scattered towards the eye is not itself absorbed
            // over the same length.
            //
            // The colour is v1's scatC (0.018, 0.070, 0.092) -- blue-green,
            // dimmer than any surface, and scaled by the sun because a lake at
            // night does not glow.
            const float wDist = h.hit ? h.t : 60.0f;
            const float3 wTr = exp(waterSigma * -wDist);
            const float wSun = saturate(0.25f + gParams.sky.sunDir.y * 2.0f);
            if (waterOn(kWFScatter))
            {
                // -------------------------------------------------------------
                // GOD RAYS -- THE IN-SCATTER IS MARCHED, NOT INTEGRATED.
                //
                // (user 2026-09-18: "can you add god rays to underwater like v1
                // had.")
                //
                // WHY THE CLOSED FORM COULD NEVER HAVE SHOWN THEM. What was
                // here was scatC * (1 - wTr): the exact analytic integral of
                // in-scatter through a HOMOGENEOUS medium. It is the right
                // answer to the question it asks, and that question has no
                // position in it -- every point along the segment contributes
                // the same thing, so the water can only ever come out as an
                // even wash. A shaft is the opposite: it is the in-scatter
                // being BRIGHTER in some places than others.
                //
                // WHAT MAKES THE SHAFTS is the same thing that makes the webs on
                // the bed. Sunlight is focused by the surface before it ever
                // reaches the water below, so the volume is lit through the very
                // same caustic pattern -- v1 projects each sample UP the sun
                // direction onto the surface and reads the caustic there, which
                // is exactly where that sample's light came through. The webs on
                // the bed and the shafts in the water are one phenomenon seen
                // two ways, and they line up because they share waterCaustic.
                //
                // IT IS THE SAME BRIGHTNESS IT WAS. The step weight is
                // waterSigma * dt * exp(-waterSigma * t), which sums to exactly
                // (1 - wTr) over the segment -- so with the caustic flat this is
                // the old term to floating-point noise. v1's modulation is
                // 0.45 + 1.75 * caust, whose mean is 1.0 for the same reason:
                // this adds structure without moving the exposure.
                //
                // FOUR STEPS, JITTERED. v1 uses four and says why -- the
                // estimator is unbiased, so the accumulator and Ray
                // Reconstruction converge on the same image a finer march would
                // give, and only the per-frame noise rises. Unjittered, four
                // steps would band into four visible shells.
                const int kWSteps = 4;
                const float dtW = wDist / float(kWSteps);
                const float3 scatC = float3(0.018f, 0.070f, 0.092f);
                // Floored for the same reason the bed's slant path is: a sun on
                // the horizon would otherwise divide the downward path to
                // infinity and black the whole column out in one frame.
                const float sinEl = max(gParams.sky.sunDir.y, 0.08f);
                const bool shafts = waterOn(kWFCaustic) && gParams.sky.sunDir.y > 0.0f;
                float tw = dtW * rng.nextFloat();
                float3 acc = float3(0.0f);
                [unroll] for (int ws = 0; ws < kWSteps; ++ws)
                {
                    const float3 p = o + d * tw;
                    // HOW DEEP THIS SAMPLE IS, measured from the surface the
                    // path actually came in through. waterEntryY rather than
                    // gParams.waterY for the reason the bed's note gives: the
                    // global is the CAMERA band's line and this segment may be
                    // under a pond in another one.
                    const float dBelow = max(0.0f, waterEntryY - p.y);
                    // The sun's own loss on the way down to here. This is what
                    // makes a shaft fade with depth rather than running to the
                    // bed at full strength.
                    const float3 lightT = exp(waterSigma * -(dBelow / sinEl));
                    float caW = 1.0f;
                    if (shafts)
                    {
                        // Up the sun to the surface: that is where this sample's
                        // light came through, so that is where to read the web.
                        const float2 up = float2(p.x, p.z) +
                                          float2(gParams.sky.sunDir.x, gParams.sky.sunDir.z) *
                                              (dBelow / sinEl);
                        caW = 0.45f + 1.75f * waterCaustic(up, gParams.waterTime,
                                                           1.11f / featureSizeM(tw, 1.11f));
                    }
                    acc += scatC * wSun * caW * lightT * exp(waterSigma * -tw) *
                           (waterSigma * dtW);
                    tw += dtW;
                }
                L += beta * acc;
            }
            if (waterOn(kWFAbsorb)) beta *= wTr;

            // -----------------------------------------------------------------
            // CAUSTICS -- the webs a lake's surface focuses onto its bed.
            //
            // NOT TRACED, AND DELIBERATELY. A path tracer finds these honestly,
            // through specular-to-diffuse paths, and at the sample counts a
            // moving camera has they arrive as salt-and-pepper fireflies rather
            // than as webs. v1 draws them instead; waterCaustic is v1's.
            //
            // THE GATE IS `inWater`, AND THE FIRST VERSION GOT THIS WRONG. It
            // asked whether the hit was below gParams.waterY, which is true of
            // every hillside under 33 m whether or not there is a drop of water
            // above it -- so the webs appeared across dry ground in the open
            // wood. Being INSIDE the water is the real question, and it is the
            // one the path already answers: this segment is under the surface,
            // so whatever it lands on is being seen through water by definition.
            //
            // AND IT NEEDS THE SUN, because caustics are focused SUNLIGHT. Faded
            // rather than switched, or the webs vanish on a single frame.
            if (h.hit && h.mat.type == MAT_OPAQUE)
            {
                // -------------------------------------------------------------
                // THE OTHER HALF OF BEER-LAMBERT, AND IT IS NOT OPTIONAL NOW.
                //
                // With water no longer casting a shadow (see traceOccluded) the
                // sun reaches the bed at FULL strength, and only the EYE path
                // is attenuated. Sand under a metre of water then comes out as
                // bright as the dry beach beside it -- v4's note calls the
                // result "a swimming pool", and it is the exact artifact you
                // trade for the black bed if you fix one without the other.
                //
                // Light is absorbed on the way DOWN too. Marching that is a
                // second trace per shaded point, so it is analytic instead:
                // the surface is the plane at gParams.waterY, the slant path
                // through it is depth / sin(elevation), and the bed's albedo
                // carries the loss. v4 does exactly this.
                //
                // FREE SIDE EFFECT, and a wanted one: the caustics below fade
                // with depth, because they multiply an albedo this has already
                // darkened. A deep lake stops wearing its shallows' webs.
                if (waterOn(kWFSunPath))
                {
                    // THE SURFACE THE PATH CAME IN THROUGH. Same correction as
                    // the crest exit and for the same reason -- gParams.waterY
                    // is the camera band's line and this bed may be under a
                    // pond in the other band. waterEntryY is recorded where the
                    // path last crossed a surface; it falls back to the global
                    // for a submerged camera that never crossed one.
                    const float depth = max(0.0f, waterEntryY - h.p.y);
                    // Floored so a sun on the horizon does not divide the slant
                    // path to infinity and take the bed to black in one frame.
                    const float sinEl = max(gParams.sky.sunDir.y, 0.08f);
                    h.mat.albedo *= exp(-waterSigma * (depth / sinEl));
                }

                const float sun = waterOn(kWFCaustic)
                                      ? saturate(gParams.sky.sunDir.y * 8.0f)
                                      : 0.0f;
                if (sun > 0.0f)
                    // The webs are about 1.1 m across as authored; held to
                    // kFeaturePx pixels so they do not wash out at range.
                    h.mat.albedo *=
                        1.0f + 1.6f * sun *
                                   waterCaustic(float2(h.p.x, h.p.z), gParams.waterTime,
                                                1.11f / featureSizeM(h.t, 1.11f));
            }
        }

        if (!h.hit)
        {
#ifdef V2_NRCSDK
            // The path left the world. The cache needs to know, because a
            // path that escaped is a COMPLETE observation -- the radiance
            // along it is final and can be trained on, where a path that was
            // merely cut short cannot.
            NrcUpdateOnMiss(nrcSdkPath);
#endif
            // Escaped: the sky. The dome is never light-sampled, so it always
            // arrives at full weight; the disk may have been, so it is weighted
            // against the cone pdf that could have found it.
            float3 sky = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, d);
            // THE DOME IS NOW REACHABLE TWO WAYS, so it can no longer arrive at
            // full weight. Before sky NEE existed this was correct because
            // continuing the path was the only strategy that could find the
            // dome; now sampleSkySplit finds it too, and a pair of strategies
            // that both pay in full is the textbook double count -- it would
            // show up as the whole sky getting brighter, not as an error.
            //
            // Guarded on skyPdfPrev rather than on the toggle so that turning
            // sky NEE off restores the old weight exactly, with no second place
            // to keep in step.
            if (!specularBounce && skyPdfPrev > 0.0f)
                sky *= powerHeuristic(1.0f, bsdfPdf, 1.0f, skyPdfPrev);
            // The moon rides in the same slot as the sun disk: same cone size,
            // same MIS weight, and exactly one of them is ever non-zero
            // because they sit at opposite points of the sky.
            const float3 disk = skyDisk(gParams.sky, d) + moonDisk(gParams.sky, d, gMoonTex, gMoonSamp);
            if (!isBlack(disk))
            {
                const float lightPdf = uniformConePdf(keyCosR(gParams.sky));
                const float w = specularBounce ? 1.0f : powerHeuristic(1.0f, bsdfPdf, 1.0f, lightPdf);
                sky += disk * w;
            }
            // -- SHOOTING STARS, down the PRIMARY ray and at night -----------
            //
            // Ahead of the deck, so a cloud drawn over them hides one -- which
            // is what a cloud does. Emissive and added rather than composited:
            // a meteor is a light in the sky, not a surface in front of it.
            //
            // depth == 0 for the same reason the deck is: a meteor is worth
            // nothing to an indirect bounce that escaped, and eight slots of
            // arithmetic on every one of those would be paid millions of times
            // a frame to change no pixel.
            if (depth == 0)
            {
                // NIGHT, from the sun's own elevation, smoothed across the
                // last few degrees so nothing switches on at a hard edge. The
                // meteors are gone entirely by the time the sun is up, which
                // is one compare on a value that is exactly 0 all day.
                const float ny = saturate((0.02f - gParams.sky.sunDir.y) * 8.3333f);
                sky += meteors(d, gParams.skyTime, ny * ny * (3.0f - 2.0f * ny));
            }

            // -- THE DECK, over the sky and only down the PRIMARY ray --------
            //
            // Composited as a surface with alpha rather than blended against
            // the raw sky, so an opaque cloud hides the sun DISK completely
            // instead of letting a fifth of a very bright disk through as a
            // crisp circle. The brightness still arrives -- as the deck
            // scattering the sun, which is what the phase function and the
            // light march are for, so a cloud in front of the sun glows.
            //
            // depth == 0 ONLY. A miss at depth > 0 is an indirect bounce that
            // escaped, and putting a 28-step march with a 4-step light march
            // inside it on every one of those would cost more than the entire
            // path trace. The sky those bounces see is the clear-sky dome, which
            // is the same approximation every other cache in here already makes.
            if (depth == 0 && gParams.clouds != 0)
            {
                // THE UNBOOSTED DISC, THEN THE DECK'S OWN NIGHT KEY. Taking
                // keyRadiance here took moonKey with it -- 300, calibrated for
                // surfaces lit through NEE -- while ambC below comes from the
                // dome with no boost at all. sunC 1.62 against ambC 0.003 is
                // the white deck over the black sky: the moon's surface
                // calibration leaking into a consumer that never wanted it.
                const float3 sunC = keyDiscRadiance(gParams.sky) *
                                    keySolidAngle(gParams.sky) * gParams.cloudSun *
                                    (gParams.sky.sunDir.y > 0.0f ? 1.0f : gParams.cloudMoonKey);
                const float3 ambC =
                    skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, float3(0.0f, 1.0f, 0.0f)) * gParams.cloudAmb;
                // The colour a far cloud fades INTO: the dome just above the
                // horizon, with no sun disk in it. Taking the disk here would
                // let a distant cloud fade toward the brightness of the sun
                // itself, which is the hole-in-the-cloud the alpha composite
                // exists to prevent.
                const float3 hazeC =
                    skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, normalize(float3(d.x, max(d.y, 0.02f), d.z)));
                const float4 deck =
                    cloudMarch(gCloudVol, gCloudSamp, gParams.cam.pos, d, keyDir(gParams.sky),
                               sunC, ambC, hazeC, gParams.cloudWindT,
                               cgIGN(float2(pix), gParams.tick));
                sky = sky * (1.0f - deck.a) + deck.rgb;
            }
            L += beta * sky;
            // The sky down the PRIMARY ray is modulated by no surface at all,
            // so it is emission. Deeper than that it arrived through the
            // primary lobe like everything else and splits with it.
            if (depth == 0)
            {
                Lemit += beta * sky;
                primaryDist = 1e5f;
            }
            else
            {
                const float3 c = beta * sky;
                Ldiff += c * wDiff;
                Lspec += c * (1.0f - wDiff);
            }
            break;
        }

        if (depth == 0)
        {
            primaryDist = h.t;
            heldPrimary = h.held;
            primaryMoved = h.moved;

            // -- the guides ---------------------------------------------------
            // Split into the two albedos the denoiser demodulates by. It is
            // reconstructing LIGHTING, so anything that is really surface
            // colour has to be handed over separately or it gets smoothed away
            // with the noise -- which on a voxel wood means the needles.
            gViewZ = dot(h.p - c.pos, c.w);
            gNormal = h.ns;
            if (h.mat.type == MAT_DIELECTRIC)
            {
                // Water reflects and refracts and has no albedo of its own.
                // Telling the denoiser it is a smooth mirror is what stops it
                // trying to average a reflection across the surface.
                gAlbedo = float3(0.0f);
                gSpecular = float3(1.0f);
                gRough = 0.0f;
                // Water has no albedo to divide out, and everything it returns
                // came off a delta specular lobe -- so the diffuse channel is
                // empty and its divisor never gets used. One, not zero, so a
                // demodulate pass that divides unconditionally stays finite.
                demodAlbedo = float3(1.0f);
                wDiff = 0.0f;
            }
            else
            {
                const float kt = h.mat.translucency;
                // The same split sampleOpaque makes: what is left of the
                // diffuse lobe after the part that passes through.
                gAlbedo = h.mat.albedo * (1.0f - kt);
                gSpecular = float3(schlick(h.mat.specular, saturate(dot(h.ns, -d))));
                gRough = h.mat.roughness;
                // The plain albedo -- see the note on gDemodAlbedo at the top.
                demodAlbedo = h.mat.albedo;
            }
        }

        // -- THE BULB ITSELF, WHICH IS THE ONE SURFACE THAT GIVES LIGHT -----
        //
        // ONE COMPARE AGAINST THE RESOLVED MATERIAL INDEX. h.mtl is what
        // groundShade returned, and for a colour a model brought with it that
        // is the palette entry itself -- so this is exact, and it separates the
        // glass from the flex touching the top of it, which a distance test
        // cannot.
        //
        // ONLY DOWN THE PRIMARY RAY. A bounce that lands on the bulb has
        // already had the bulb's light delivered to it by the estimator above,
        // so paying again here would count it twice -- as a room that gets
        // brighter with maxDepth. Deeper paths simply end on it, which is what
        // an opaque surface does to a path that has already been paid.
        //
        // INTO Lemit, beside the sky. DLSS-RR demodulates by albedo, and the
        // bulb has no albedo worth dividing out -- it is a light, and filing it
        // as diffuse radiance would have the denoiser try to average a 60 nit
        // surface across its neighbours.
        // ...AND THE FIREFLY, on the same branch and for the same reason: it
        // is a light rather than a surface, so it must not be demodulated by an
        // albedo it does not have. What is different is that nothing samples
        // it -- see V6Params::glowMtl.
        // -- THE HIT FLASH EMITS, so a wound reads under a canopy ---------
        //
        // AT THE PRIMARY HIT ONLY, and added rather than returned: unlike a
        // spark or a firefly this is a SURFACE that also glows -- the animal
        // goes on taking the sun, the sky and its own shadow, and the red is
        // laid over that. Returning here instead would flatten a dying animal
        // into a silhouette.
        // =====================================================================
        // EVERY EMITTER BELOW PAYS OUT ON A DELTA PATH TOO, NOT ONLY AT DEPTH 0.
        //
        // (user 2026-09-19: "in the water, sparks are black. fix this.")
        //
        // THEY WERE BLACK, NOT DIM, AND THIS WAS THE WHOLE OF IT. `depth == 0`
        // is the standard guard against double counting -- an emitter reached
        // by an ordinary bounce has already been paid for by the light sampler
        // at the vertex before it, so adding it again would count it twice.
        // What that guard forgets is the case where NO light sampler ran.
        //
        // WATER IS A DELTA SURFACE AND SAYS SO: the refraction branch sets
        // specularBounce and zeroes skyPdfPrev precisely because "no light
        // sampler drew this". So a spark under the surface -- or seen through
        // it from above -- is reached at depth 1 with nothing having sampled
        // it, the branch pays nothing, and the path terminates on a light
        // carrying zero radiance. Black, at the one moment the effect is
        // supposed to be at its most dramatic.
        //
        // THE SKY ALREADY HAD THIS RIGHT and is the model for the fix: its MIS
        // weight is 1 when specularBounce, which is the same statement in the
        // other direction. `emissiveHere` is that rule once, for all four.
        //
        // It is not only water. A spark seen in a reflection, or through any
        // future glass, was equally black and for the same reason.
        // =====================================================================
        const bool emissiveHere = (depth == 0) || specularBounce;
        if (h.hurt > 0.0f && emissiveHere)
        {
            const float3 he = kHurtRed * (kHurtNits * h.hurt);
            L += beta * he;
            Lemit += beta * he;
        }
        if (h.mtl == gParams.glowMtl && gParams.glowMtl != kNoBulb)
        {
            if (emissiveHere)
            {
                L += beta * gParams.glowRadiance;
                Lemit += beta * gParams.glowRadiance;
            }
            break;
        }
        // ...AND THE SPARKS, on the same branch and for the third time the same
        // reason: a burning fleck is a light rather than a surface, so it must
        // not be demodulated by an albedo it does not have. See
        // V6Params::emitters -- the table is packed from 0, so this stops at
        // the first empty slot and a world with no sparks pays one compare.
        bool emitHit = false;
        for (int em = 0; em < kEmitSlots; ++em)
        {
            if (gParams.emitters[em].w < 0.0f) break;
            if (h.mtl != (uint)(gParams.emitters[em].w + 0.5f)) continue;
            if (emissiveHere)
            {
                L += beta * gParams.emitters[em].xyz;
                Lemit += beta * gParams.emitters[em].xyz;
            }
            emitHit = true;
            break;
        }
        if (emitHit) break;
        if (h.mtl == gParams.bulbMtl && gParams.bulbMtl != kNoBulb)
        {
            if (emissiveHere)
            {
                L += beta * gParams.bulbRadiance;
                Lemit += beta * gParams.bulbRadiance;
            }
            break;
        }

        // -- water: a delta surface, so no light sampling here at all ---------
        if (h.mat.type == MAT_DIELECTRIC)
        {
            // -------------------------------------------------------------
            // THE GLINT, PORTED FROM v1 (its pixelGlisten on water).
            //
            // A sharp specular column toward the sun, broken into per-voxel
            // CELLS that twinkle out of phase. Not a broad highlight: one cell
            // is one 10 cm voxel, and only about half of them participate, so
            // the effect is a scatter of hard sparks rather than a sheet of
            // sheen. That discreteness is what makes it read as voxel water.
            //
            // AS EMISSION, because v2's water is BSDF-sampled and there is no
            // shaded `col` here to mix into the way v1 has. Adding it to L at
            // the vertex is the same light by another route.
            //
            // THE DUTY CYCLE IS v1'S SECOND ATTEMPT AND THE NOTE MATTERS. Its
            // window was smoothstep(0.82, 1.0), which lit a cell for 28% of
            // its cycle and left barely 5% of the water sparkling at any
            // instant -- the user reported it as the pattern "removing itself,
            // then changing again", which is just a very low duty cycle seen
            // as blinking. Widened to 0.30-0.85: lit 63% of the time, fading
            // in and out instead of snapping.
            // -------------------------------------------------------------
            {
                // OFF (user 2026-09-12: "turn off the shiny white reflections
                // on the water"). This is v1's pixelGlisten -- a scatter of
                // hard white sparks where the surface points at the sun, one
                // per voxel cell, about half the cells taking part.
                //
                // Kept rather than deleted, and kept whole: the duty window,
                // the per-cell phase and the cell size are all tuned numbers
                // that took measuring, and v1's first attempt at the window
                // read as blinking. `kGlintStrength` is the one thing to move
                // if it is ever wanted back; 0 folds the block away entirely.
                const float gk = (waterOn(kWFGlint) ? kGlintStrength : 0.0f) *
                                 saturate(smoothstep(-0.02f, 0.10f, gParams.sky.sunDir.y));
                if (gk > 0.01f)
                {
                    const float3 refl = reflect(d, h.ns);
                    const float column = pow(max(dot(refl, gParams.sky.sunDir), 0.0f), 26.0f);
                    if (column > 1e-4f)
                    {
                        // ONE CELL IS ONE VOXEL UNTIL A VOXEL IS TOO SMALL TO
                        // SEE. Past that the cell grows with range so the
                        // sparkle stays a scatter of hard points instead of
                        // integrating away into a sheet -- see featureSizeM.
                        const float cellM = featureSizeM(h.t, kVoxelM);
                        const float2 cell = floor(float2(h.p.x, h.p.z) / cellM);
                        const float ph =
                            frac(sin(cell.x * 91.7f + cell.y * 47.3f) * 4321.7f) * 6.2831853f;
                        const float tw = sin(gParams.waterTime * 1.6f + ph) * 0.5f + 0.5f;
                        // ~half the cells take part -- scattered, not a sheet.
                        const float pick =
                            step(0.5f, frac(sin(cell.x * 12.9898f + cell.y * 78.233f) *
                                            43758.5453f));
                        const float spark = smoothstep(0.30f, 0.85f, tw) * pick;
                        L += beta * float3(1.35f, 1.25f, 1.0f) * (spark * column * gk * 0.85f);
                    }
                }
            }

            // -------------------------------------------------------------
            // THE CHURN TAKES ITS SHARE FIRST.
            //
            // Foam floats ON the water: it throws light straight back and hides
            // what is under it, in proportion. So it is paid out of beta BEFORE
            // the Fresnel split below -- taken after, it would be foam seen
            // through the surface rather than riding on it, and the reflection
            // would be laid over the top of it.
            //
            // SUN PLUS SKY, THE SAME FORM THE TRACED REFLECTION USES BELOW --
            // and the first version of this got it wrong in a way worth
            // recording. It lit the foam with skyDome(up) alone, which at noon
            // is the BLUEST light in the scene, so the band came out a faint
            // violet smear: measured against the same frame with the term off,
            // 0.77% of pixels touched at a maximum delta of 56/255. It was in
            // exactly the right place and did not read as foam at all.
            //
            // Whitecaps are white because they are lit by the SUN. So the band
            // is shaded as what it is -- a white Lambertian cap lying flat on
            // the water, normal straight up -- and it then sits in the same
            // light as the sand a metre away, which is the comparison the eye
            // actually makes. It reddens at dusk and greys in shade for free.
            //
            // ONE SHADOW RAY, and only where h.foam > 0: the band is about a
            // per cent of the frame, so this is a per-shore cost, not a
            // per-water-pixel one. passWater is TRUE -- foam floats ON the
            // lake, and the lake's own surface must not be allowed to shadow
            // it.
            //
            // waterOn(kWFFoam) is applied where h.foam is SET, not here, so
            // there is one gate rather than two that can disagree.
            // -------------------------------------------------------------
            if (h.foam > 0.0f)
            {
                const float3 up = float3(0.0f, 1.0f, 0.0f);
                // A Lambertian under a dome of radiance L scatters back albedo*L,
                // so the coefficient is 1 in principle; 0.8 pays for the trees
                // and the bank that take a bite out of the shore's sky.
                float3 lit = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, up) * 0.8f;
                const float ndl = max(gParams.sky.sunDir.y, 0.0f);
                if (ndl > 0.0f && !isBlack(gParams.sky.sunRadiance) &&
                    !traceOccluded(offsetRay(h.p, up, gParams.sky.sunDir), gParams.sky.sunDir,
                                   INF, true))
                    lit += INV_PI * gParams.sky.sunRadiance * ndl * kSunSolidAngle;
                L += beta * h.foam * kFoamColour * lit;
                beta *= (1.0f - h.foam);
            }

            const bool entering = dot(d, h.ns) < 0.0f;

            // -------------------------------------------------------------
            // DETERMINISTIC, THE WAY v1 DOES IT -- AND THIS IS WHY v1'S WATER
            // LOOKS SETTLED AND v2'S DID NOT.
            //
            // v1 is a raymarcher: at every water pixel it traces the
            // refraction, shades the bed, and blends the result against the
            // reflection by Fresnel. Every pixel gets BOTH, every frame, with
            // no noise in the split:
            //
            //     col = mix(refrC, reflC, clamp(fres * 0.45, 0.0, 1.0));
            //
            // v2 is a path tracer and was sampling ONE lobe --
            // `rng.nextFloat() < F` -- so at one sample a pixel was either a
            // mirror or a window, never the mix. The blend only existed as an
            // average over samples, and Ray Reconstruction had to invent the
            // rest of it. Every still I rendered looked fine because an
            // offline --out accumulates until it converges; in motion at 1 spp
            // it cannot.
            //
            // So the split is taken ANALYTICALLY: the reflection is added here
            // weighted by F, and the path continues through the refraction
            // weighted by (1 - F). No random choice, no variance, nothing for
            // the denoiser to guess at.
            //
            // THE REFLECTION IS THE SKY, NOT A TRACED BOUNCE. A second traced
            // path per water pixel is what the loop cannot do without a stack,
            // and the sky is what a lake mostly reflects. The cost is one
            // texture fetch against a whole extra path.
            // -------------------------------------------------------------
            {
                const float3 nF = entering ? h.ns : -h.ns;
                const float cosI = saturate(dot(-d, nF));
                float Fr;
                {
                    const float eta = entering ? (1.0f / h.mat.ior) : h.mat.ior;
                    const float s2t = eta * eta * (1.0f - cosI * cosI);
                    if (s2t >= 1.0f) { Fr = 1.0f; }
                    else
                    {
                        const float cT = sqrt(1.0f - s2t);
                        const float rs = (cosI - h.mat.ior * cT) / (cosI + h.mat.ior * cT);
                        const float rp = (h.mat.ior * cosI - cT) / (h.mat.ior * cosI + cT);
                        Fr = 0.5f * (rs * rs + rp * rp);
                    }
                    if (entering) Fr *= kWaterReflect;   // v1's 0.45 cap
                }
                const float3 rdir = reflect(d, nF);
                // LIFTED TO THE HORIZON RATHER THAN DROPPED.
                //
                // A wave's STEP SIDE has a near-horizontal normal, so its
                // reflection points sideways or down. Skipping those -- which
                // is what `if (rdir.y > 0)` did -- left every step face with no
                // reflection at all and nothing but absorbed refraction behind
                // it, i.e. BLACK. It drew hard dark bands along the swell,
                // precisely on the faces that most need a highlight.
                //
                // A downward reflection off a lake really shows the far bank,
                // which there is no second path to trace here; horizon sky is
                // the closest thing that is free and is never black.
                // ...BUT THE HORIZON IS THE BRIGHTEST BAND OF THE SKY, and a
                // wave STEP FACE is horizontal, so lifting every downward
                // reflection to the horizon lit every step face with the
                // brightest thing available. The swell came out as bright
                // stripes and the water ended up brighter than the dry sand
                // beside it, which is backwards: a lake reflects at most 45%
                // here and absorbs on the way through.
                //
                // What a downward reflection really sees is the far bank and
                // more water -- dark. With no second path to trace, the
                // water's own in-scatter colour is the honest stand-in, and it
                // is the same constant the absorption below uses. Upward
                // reflections still get real sky.
                const float3 rs = normalize(float3(rdir.x, max(rdir.y, 0.02f), rdir.z));
                float3 skyR = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, rs);
                // -- THE GLARE ON THE WATER -- see kWFSunGlare ----------------
                //
                // (user 2026-09-18: "turn off the suns glare ... its mixed in
                // with the pixel glint on the water. I need you to seperate
                // it", then "the sun glare checkmark does nothing".)
                //
                // AND IT DID NOTHING, BECAUSE THE FIRST ATTEMPT GUESSED. That
                // one withheld the sun's DISC from a ray that reached the sky by
                // a specular bounce off water -- and water in this engine never
                // makes such a ray. The branch below REFRACTS deterministically
                // (`refract(d, nF, eta)`, with the mirror direction kept only
                // for total internal reflection), so a ray meeting a lake from
                // above goes DOWN into it and the sky-miss code never sees it.
                // The flag was unreachable; the A/B rendered byte-identical
                // images with it on and off, which is what proved it.
                //
                // THE WASH IS THIS LINE. The surface's reflected radiance is the
                // sky sampled in the MIRROR direction, weighted by Fresnel --
                // and skyDome excludes the sun's disc but not the blazing
                // circumsolar sky around it. Aimed near the sun and multiplied
                // by a grazing Fresnel, that aureole is the broad hazy sheen.
                // It is not a sparkle and it is not the disc: it is the bright
                // part of the sky, in a mirror.
                //
                // OFF means the reflection is held to the ambient dome's
                // brightness -- the sky straight up, which has no aureole in it
                // -- so the water still reflects a sky of the right colour and
                // simply stops carrying the hot spot. Scaled rather than
                // replaced, so the directional tint survives. One extra dome
                // sample, and only when the flag is off.
                if (!waterOn(kWFSunGlare))
                {
                    const float3 amb =
                        skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, float3(0.0f, 1.0f, 0.0f));
                    const float lr = luminance(skyR), la = luminance(amb);
                    if (lr > la && lr > 1e-6f) skyR *= la / lr;
                }
                const float wSunR = saturate(0.25f + gParams.sky.sunDir.y * 2.0f);
                const float3 selfR = float3(0.018f, 0.070f, 0.092f) * wSunR;
                float3 reflR = lerp(selfR, skyR, saturate(rdir.y * 4.0f));

                // -------------------------------------------------------------
                // THE REFLECTION IS THE WORLD NOW, NOT JUST THE SKY -- AND THIS
                // IS WHY DISTANT WATER LOOKED DEAD.
                //
                // "The effects on the water go away at a distance." Nothing
                // gates on range: rendered at --no-dlss --spf 16 the glint and
                // the swell are still being computed out there. What changes
                // with distance is the ANGLE. Close to a lake you look down
                // into it, Fresnel is small, and most of what you see is the
                // refraction -- the lit bed, the caustics, the colour of the
                // water itself. Far away you see it at a graze, Fresnel goes to
                // one, and almost all of what you see is the reflection.
                //
                // And the reflection was the sky DOME alone, which over a lake
                // is a smooth gradient with nothing in it. So far water was a
                // flat sheet of one colour -- not because its effects had been
                // switched off, but because the one effect that survives a
                // grazing view had nothing to show.
                //
                // One traced ray fixes it: the treeline, the bank and the
                // shore come back into the water. Shaded cheaply -- a lambert
                // against the sun plus a sky term, the same form ProbeTrace
                // uses -- because this is a reflection in moving water and
                // nobody is going to read the detail in it.
                //
                // DEPTH 0 ONLY. It costs one trace and one shadow ray per
                // water pixel, and a reflection seen IN a reflection is not
                // worth a second round of that.
                // -------------------------------------------------------------
                if (waterOn(kWFReflect) && depth == 0 && entering)
                {
                    const Hit rh = traceRadiance(offsetRay(h.p, nF, rdir), rdir, MASK_WORLD);
                    if (rh.hit)
                    {
                        float3 lit = float3(0.0f);
                        const float ndl = max(dot(rh.ns, gParams.sky.sunDir), 0.0f);
                        if (ndl > 0.0f && !isBlack(gParams.sky.sunRadiance) &&
                            !traceOccluded(offsetRay(rh.p, rh.ng, gParams.sky.sunDir),
                                           gParams.sky.sunDir, INF, false))
                            lit += rh.mat.albedo * INV_PI * gParams.sky.sunRadiance * ndl *
                                   kSunSolidAngle;
                        // The sky it sits under, as a flat ambient. A hemisphere
                        // integral here would cost another trace for something
                        // this blurred.
                        lit += rh.mat.albedo *
                               skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp,
                                       float3(0.0f, 1.0f, 0.0f)) *
                               0.35f;
                        reflR = lit;
                    }
                }
                L += beta * Fr * reflR;
                beta *= (1.0f - Fr);

                // WHERE THE PATH WENT UNDER, kept for the bed's sun path --
                // see Hit::waterTop. The quad's own y, not the frame's global.
                if (entering && h.waterTop != 0.0f) waterEntryY = h.waterTop;

                // Refract deterministically; total internal reflection keeps
                // the ray inside, which is the one case that must still bend.
                const float eta = entering ? (1.0f / h.mat.ior) : h.mat.ior;
                float3 wi = refract(d, nF, eta);
                if (dot(wi, wi) < 1e-6f) wi = rdir;      // TIR: follow the mirror
                else inWater = entering;

                // ---------------------------------------------------------
                // A CREST IS NOT IN THE ACCELERATION STRUCTURE, SO THE RAY
                // LEAVING IT WOULD MEET THE WATER ALL OVER AGAIN.
                //
                // marchWaterVoxels finds crests standing ABOVE the meshed
                // surface, in air the ray has already crossed -- that is how
                // v1 gets a swell with no lid. But the meshed quad is still
                // down there at the waterline, so a ray refracting DOWN from a
                // crest hits it, takes a second dielectric interface, flips
                // inWater back, and loses most of its energy. It drew as dark
                // blocky patches all over the lake, and it is v4's "a ray
                // inside the water refracted at every voxel face" wearing
                // different clothes.
                //
                // There is ONE interface. Having entered at the crest, the ray
                // is in water from there down, so it is pushed past the quad
                // rather than being allowed to meet it.
                // ---------------------------------------------------------
                float3 ori = h.p;
                //
                // THIS SURFACE'S OWN LEVEL, NOT THE FRAME'S. See Hit::waterTop:
                // pushing to the camera band's waterline drove the ray metres
                // into (or clean through) any pond whose line differs, and drew
                // the lattice of black blobs in the debug video.
                const float stillY = (h.waterTop != 0.0f) ? h.waterTop : gParams.waterY;
                if (entering && wi.y < -1e-4f && ori.y > stillY)
                    ori += wi * ((stillY - 2e-3f - ori.y) / wi.y);

                o = offsetRay(ori, h.ng, wi);
                d = wi;
                specularBounce = true;
                skyPdfPrev = 0.0f;  // a delta surface: no light sampler drew this
                continue;
            }
        }

        // -- the neural radiance cache ---------------------------------------
        //
        // PLACED HERE because this is the first point where the vertex is fully
        // known -- position, shading normal, and the material's albedo and
        // roughness -- and still before any of the light gathered at it has been
        // added. Both halves need exactly that state.
        //
        // A training path REMEMBERS the vertex and keeps going: its job is to
        // find out what the cache should have said, which is the radiance the
        // rest of the path gathers from here. A querying path asks the network
        // and stops, which is the entire saving.
        // The primary vertex, captured once. Everything ReSTIR does at shading
        // time is measured from here.
        if (depth == 0 && !havePrimary) {
            primaryPos = h.p;
            primaryNormal = h.ns;
            havePrimary = true;
        }

        // The first indirect vertex: the candidate ReSTIR will resample. Taken
        // at depth 1 -- the point the camera ray's bounce landed on -- because
        // that is the vertex whose choice is responsible for the noise, and the
        // one a neighbouring pixel could plausibly have found too.
        if (depth == 1 && !giHaveVertex) {
            giHaveVertex = true;
            giPos = h.p;
            giNormal = h.ns;
            giBeta = beta;
            giLBefore = L;
        }

#ifdef V2_NRC
        if (gParams.nrcMode != 0 && depth >= gParams.nrcQueryDepth && !nrcHaveVertex)
        {
            nrcQ.pos = (h.p - gParams.nrcOrigin) * gParams.nrcInvExtent;
            nrcQ.dir = -d;
            nrcQ.normal = h.ns;
            nrcQ.albedo = h.mat.albedo;
            nrcQ.roughness = h.mat.roughness;
            // The voxel half. h.ng and not h.ns: the face is a property of the
            // geometry, and water's shading normal is a ripple that would send
            // neighbouring queries to different faces of the same flat surface.
            nrcQ.world = h.p;
            nrcQ.normal = h.ng;
            nrcQ.mtl = h.mtl;
            nrcQ.sun = keyDir(gParams.sky);

            if (nrcTraining)
            {
                nrcHaveVertex = true;
                nrcBeta = beta;
                nrcLBefore = L;
            }
            else if (gParams.nrcMode == 2)
            {
                // Evaluated ONCE. The split channels have to be fed as well or
                // the demodulated route loses this energy entirely, but that is
                // the same radiance divided two ways -- not a second query.
                const float3 c = beta * nrcEval(gNrcWeights, nrcQ);
                L += c;
                Ldiff += c * wDiff;
                Lspec += c * (1.0f - wDiff);
                break;
            }
        }
#endif

#if V2_SHARC_QUERY
        // -- the hash cache ---------------------------------------------------
        //
        // Same placement and the same argument as the probe query below: what
        // comes back is the radiance LEAVING this point, everything included,
        // so the path is finished and this vertex's own next event estimation
        // must not also run.
        //
        // Radiance, not irradiance -- so unlike the probe lookup there is no
        // albedo and no 1/pi here. The update pass stored what left the surface,
        // not what arrived at it.
        //
        // THE SEGMENT LENGTH TEST IS NOT OPTIONAL. A cache entry covers a whole
        // voxel of the hash grid, so a path segment shorter than that voxel
        // lands several neighbouring shading points in one cell and hands them
        // all the same answer. That is the blocky look SHaRC is blamed for, and
        // it is avoidable by simply carrying on tracing until the segment is
        // long enough to be filed honestly.
#if V2_SHARC_VOXEL_KEY
        // BOTH GATES, AND KEEPING THE SECOND ONE WAS A MEASUREMENT RATHER THAN
        // A GUESS. The first is the obvious one: an entry names a face of a
        // block, so the segment has to clear the block. The expectation was
        // that it replaced the old distance-dependent test outright, since that
        // test existed to keep a segment longer than a hash cell and there are
        // no hash cells any more.
        //
        // Dropping it made the picture WORSE -- against a 4000-frame reference,
        // RMSE 0.00727 with the one-voxel test alone against 0.00633 with both,
        // and nearly half again the energy bias. The old test turns out not to
        // be about cell size at all. It is a CONFIDENCE gate: it scales with
        // distance from the camera, and distance from the camera is exactly how
        // few update samples a face has had. An exact key removes the
        // uncertainty about WHERE a sample was filed; it does nothing about how
        // many samples are in there. So the two gates answer different
        // questions and the cache wants both.
        const bool sharcFilable = voxelSegmentLongEnough(h.t) && sharcSegmentLongEnough(h.p, h.t);
#else
        const bool sharcFilable = sharcSegmentLongEnough(h.p, h.t);
#endif
        if (gParams.giMode == 2 && depth >= gParams.giDepth && sharcFilable)
        {
            SharcHitData hd;
            hd.positionWorld = h.p;
            hd.normalWorld = h.ng;

            float3 cached;
#if V2_SHARC_VOXEL_KEY
            if (sharcVoxelGetCachedRadiance(sharcParams(), hd, cached, false))
#else
            if (SharcGetCachedRadiance(sharcParams(), hd, cached, false))
#endif
            {
                const float3 cs = beta * cached * gParams.giStrength;
                L += cs;
                Ldiff += cs * wDiff;
                Lspec += cs * (1.0f - wDiff);
                break;
            }
        }
#endif

        // -- the irradiance cache ---------------------------------------------
        //
        // PLACED HERE, ABOVE THIS VERTEX'S NEXT EVENT ESTIMATION, and the
        // placement IS the correctness argument.
        //
        // A probe holds the TOTAL irradiance arriving at a point: the sun it
        // can see, the sky it can see, and every bounce that ever reached it.
        // So the cache does not supplement this vertex's direct lighting, it
        // REPLACES it -- query it and the path is finished, because everything
        // the path would have gone on to gather is already inside the number.
        //
        // Run the sun and sky NEE below as well and the direct light is counted
        // twice: once sharply from the shadow ray and once smeared across a
        // six-metre probe grid. That does not look like an error, it looks like
        // a wood that gets brighter the more of it is cached -- which is the
        // hardest kind of bug to attribute, so the structure rules it out
        // instead. This sits above the NEE block and ends in a break, exactly
        // where the neural cache sits and for exactly the same reason.
        //
        // Dielectrics never reach here: that branch continues above.
        if (gParams.giMode == 1 && depth >= gParams.giDepth)
        {
            const float3 E = ddgiSampleIrradiance(h.p, h.ns, -d, gDdgiConsts,
                                                  gDdgiIrradiance, gDdgiDistance,
                                                  gDdgiProbeData, gDdgiSampler);
            // Lambert: a diffuse surface under an irradiance E leaves with
            // albedo/pi * E. The specular lobe goes with the rest of the path.
            // A probe grid holds no directional detail to give a mirror, and
            // pretending it does is what makes cached reflections read as fog.
            const float3 c = beta * h.mat.albedo * INV_PI * E * gParams.giStrength;
            L += c;
            Ldiff += c * wDiff;
            Lspec += c * (1.0f - wDiff);
            break;
        }

#ifdef V2_NRCSDK
        // -- HAND THE VERTEX TO THE CACHE ------------------------------
        //
        // Placed after the sky, the water and the probe early-outs, so what
        // reaches here is an opaque surface the path is actually going to
        // shade -- which is the only kind of vertex the cache has any use
        // for.
        //
        // beta AND L GO IN BY REFERENCE, and that is the whole mechanism
        // rather than a calling convention. When the library decides this
        // path has gone far enough it writes a query record here, folds the
        // predicted tail into L, and returns Terminate -- so the path stops
        // and the cache supplies the rest. v2's own numbers are what it
        // reads and what it rewrites.
        {
            const NrcSurfaceAttributes nrcSa = nrcSdkSurface(h, d);
            const NrcProgressState nrcPs =
                NrcUpdateOnHit(nrcSdkCtx, nrcSdkPath, nrcSa, h.t, uint(depth), beta, L);
            if (nrcPs == NrcProgressState::TerminateImmediately)
                break;
            // TerminateAfterDirectLighting is the other stopping answer: the
            // vertex is cached, but the direct light at it is not, so it has
            // to be paid before leaving. The flag is carried down to the end
            // of the iteration rather than acted on here.
            nrcSdkStopAfterDirect = (nrcPs == NrcProgressState::TerminateAfterDirectLighting);
        }
#endif

        const Frame fr = Frame.make(h.ns);
        const float3 wo = fr.toLocal(-d);
        if (wo.z > 0.0f)
        {
            // At the primary vertex the two lobes are wanted apart, and this is
            // the one place the BSDF can still tell them apart for free -- it is
            // being evaluated anyway. Deeper down the split is inherited from
            // wDiff, so the cheaper composited form is used.
            if (depth == 0)
            {
                float3 neeD, neeS, skyD, skyS;
                sampleSunSplit(h, fr, wo, rng, bn, depth, inWater, neeD, neeS);
                // The dome, sampled as its own light. Summed into the sun's
                // channels rather than kept apart: they are the same two lobes
                // of the same surface, and everything downstream -- the
                // demodulated split, ReSTIR, the denoiser guides -- asks about
                // the lobe, not about which light it came from.
                sampleSkySplit(h, fr, wo, rng, bn, depth, inWater, skyD, skyS);
                neeD += skyD;
                neeS += skyS;
                // ...and the bulb, into the same two channels and for the same
                // reason: downstream asks which LOBE a photon left by, never
                // which lamp it came from.
                float3 bulbD, bulbS;
                sampleBulbSplit(h, fr, wo, inWater, bulbD, bulbS);
                neeD += bulbD;
                neeS += bulbS;
                L += beta * (neeD + neeS);
                Ldiff += beta * neeD;
                Lspec += beta * neeS;
            }
            else
            {
                const float3 c =
                    beta * (sampleSun(h, fr, wo, rng, depth, inWater) +
                            sampleSky(h, fr, wo, rng, depth, inWater) +
                            sampleBulb(h, fr, wo, inWater));
                L += c;
                Ldiff += c * wDiff;
                Lspec += c * (1.0f - wDiff);
            }
        }

#ifdef V2_NRCSDK
        // The cache took this vertex and only wanted its direct light. That
        // has now been added, so the path ends here rather than sampling a
        // direction it would never trace.
        if (nrcSdkStopAfterDirect)
            break;
#endif

        // THE TWO DRAWS ARE SEQUENCED BY HAND, not left as two arguments to one
        // call. Argument evaluation order is unspecified, and these two are not
        // interchangeable: the first picks the lobe and the second the direction
        // within it, so a compiler free to swap them would silently produce a
        // different image on a different driver.
        BsdfSample bs;
        if (depth == 0)
        {
            const float uLobe = rng.nextFloat();
            const float2 uDir = bn.next2D(kBnDimBsdf, rng);
            bs = sampleOpaqueU(h.mat, wo, uLobe, uDir);
        }
        else
        {
            bs = sampleOpaque(h.mat, wo, rng);
        }
        if (bs.pdf <= 0.0f || isBlack(bs.f))
            break;

#ifdef V2_NRCSDK
        // The pdf the cache reasons about when it decides how much this
        // vertex is worth training on. Set AFTER the sample is known good --
        // a rejected sample never became a path segment.
        NrcSetBrdfPdf(nrcSdkPath, bs.pdf);
#endif

        // WHICH LOBE LET THIS PATH THROUGH, as a fraction rather than a choice.
        // Weighted by luminance because the split has to be one scalar for a
        // three-channel radiance, and luminance is what the eye -- and every
        // denoiser downstream -- weights by.
        if (depth == 0)
        {
            float3 fD, fS;
            float dummyPdf;
            evalOpaqueSplit(h.mat, wo, bs.wi, fD, fS, dummyPdf);
            const float lD = luminance(fD), lS = luminance(fS);
            wDiff = (lD + lS > 1e-9f) ? (lD / (lD + lS)) : 1.0f;
        }

        const float3 contrib = bs.f * (abs(bs.wi.z) / bs.pdf);
        if (!allFinite(contrib))
            break;
        beta *= contrib;
        bsdfPdf = bs.pdf;
        specularBounce = bs.specular;

        const float3 wi = fr.toWorld(bs.wi);
        o = offsetRay(h.p, h.ng, wi);
        d = wi;

        // What the dome sampler would have thought of this direction, recorded
        // here because this is the one place both the shading normal and the
        // chosen direction are in hand. A specular lobe gets zero: a delta
        // direction is not one any light sampler could have drawn, so the
        // escape branch must pay it in full.
        skyPdfPrev =
            (gParams.skyRays > 0 && !bs.specular) ? skyDomePdf(h.ns, wi) : 0.0f;

        // Russian roulette on throughput, from the SECOND bounce. Starting it
        // late is the textbook advice and it is wrong here: a conifer canopy is
        // dark -- foliage albedo about 0.19 -- so a ray that enters one carries
        // almost nothing, and a late rrStart guarantees it several full bounces
        // of tree geometry first. It is unbiased either way; what changes is
        // variance per unit time, and early roulette wins that comfortably.
        if (depth >= gParams.rrStart)
        {
            const float q = min(0.95f, maxComp(beta));
            if (rng.nextFloat() > q)
                break;
            beta /= q;
        }
        if (maxComp(beta) <= 0.0f)
            break;
    }

#ifdef V2_NRCSDK
    // -- CLOSE THE PATH OUT TO THE CACHE ---------------------------------
    //
    // Written once per path, however the loop above ended -- escaped, cached,
    // rouletted or run out of bounces. This is what turns everything the path
    // accumulated into the training record the library fits against, so a path
    // that returns without it is one the cache never learns from.
    NrcWriteFinalPathInfo(nrcSdkCtx, nrcSdkPath, beta, L);
#endif

    if (!allFinite(L))
        L = float3(0.0f);
    if (primaryDist > 0.0f)
    {
        float T;
        float3 inscatter;
        fogTerms((float2(pix) + 0.5f) / float2(gParams.dim), primaryDir, primaryDist, T,
                 inscatter);
        // ...AND THE LAMPS AND THE EMBERS, WHICH DO NOT GO THROUGH THE FOG.
        // pointLightGlow is its own medium and its own integral precisely so
        // that it still works where fogTerms returns nothing -- indoors, and
        // in air too clear to march. Added to the in-scatter because that is
        // exactly what it is: light that never touched the surface.
        inscatter += pointLightGlow(gParams.cam.pos, primaryDir, primaryDist);
        L = L * T + inscatter;
        // The haze dims what the surface sent and adds light of its own. The
        // dimming is a property of the path, so it scales both lobes; the added
        // light never touched the surface, so it is emission.
        Ldiff *= T;
        Lspec *= T;
        Lemit = Lemit * T + inscatter;
    }

    if (!allFinite(Ldiff)) Ldiff = float3(0.0f);
    if (!allFinite(Lspec)) Lspec = float3(0.0f);
    if (!allFinite(Lemit)) Lemit = float3(0.0f);

    // -----------------------------------------------------------------------
    // The guides.
    //
    // THE MOTION VECTOR IS EXACT, and it is exact for a reason that is specific
    // to this world: nothing in it moves. Only the camera does. So where a
    // surface was on screen last frame is not something anyone has to store or
    // interpolate -- it is this frame's hit point projected through last
    // frame's camera, and that is a closed-form answer with no reprojection
    // error in it at all. A rasteriser has to carry a previous transform per
    // object to get the same thing approximately.
    //
    // BOTH ENDS ARE UNJITTERED, AND GETTING THAT WRONG IS WHAT GHOSTED.
    //
    // The previous end is unjittered for free: prevCam is a camera basis and
    // carries no sub-pixel offset, so projecting through it lands where a
    // jitter-free renderer would have drawn the surface. The current end is
    // NOT the pixel centre. This tracer builds the camera ray through
    // pix + 0.5 + jitter, so the surface's unjittered screen position IS
    // pix + 0.5 + jitter -- the jitter is already baked into which surface the
    // pixel found, and subtracting a bare centre leaves it in the vector.
    //
    // THE TEST THAT SETTLES IT: stand still. With the camera not moving,
    // prevCam == cam, so prevPix comes back as pix + 0.5 + jitter and the
    // motion vector must be exactly zero. Against a bare centre it came back
    // as the frame's jitter instead -- up to half a pixel of whole-screen
    // wobble, re-rolled from the Halton sequence every frame, handed to a
    // reconstruction that was standing perfectly still.
    //
    // WHY THE CANOPY WORE IT AND THE GROUND DID NOT. Half a pixel of history
    // offset costs nothing where the neighbouring pixel looks the same -- bark,
    // moss, a fogged ridge. A pine needle is one voxel wide against the sky, so
    // half a pixel is a FULL-CONTRAST error, and Ray Reconstruction cannot tell
    // a misregistered needle from a moving one: it blends, and the blend is the
    // trail. Worse, the error IS the jitter, which is the very quantity DLSS
    // uses to place the incoming sample -- so the sub-pixel accumulation that
    // makes reconstruction work was being fed offsets it had already been told.
    //
    // DLSS is told the vectors are jitter-free (the MVJittered flag is not set,
    // and Streamline is given motionVectorsJittered = eFalse); this is what
    // makes that claim true. The jitter still reaches DLSS on its own, with the
    // sign flip that converts this tracer's convention to a rasteriser's -- see
    // in.jitterX in tracer.h.
    // -----------------------------------------------------------------------
    // -- hand the cache what this path actually found ------------------------
    //
    // THE TARGET IS THE RADIANCE PER UNIT THROUGHPUT AT THAT VERTEX, which is
    // what the cache is defined to return. L - nrcLBefore is everything gathered
    // after the vertex, and it arrived multiplied by the throughput the path
    // carried into it, so dividing that back out is what makes the answer a
    // property of the VERTEX rather than of the path that happened to reach it.
    //
    // Clamped on the way in. One path through a specular caustic can carry a
    // value orders of magnitude above the batch, and in fp16 training that is
    // not an outlier, it is an inf that poisons the network permanently.
    // -- ReSTIR GI ------------------------------------------------------------
    //
    // THE CANDIDATE IS WHAT THIS PATH FOUND, expressed as a property of the
    // POINT rather than of the path: L - giLBefore is everything gathered after
    // the vertex, and it arrived scaled by the throughput the path carried into
    // it, so dividing that out gives the radiance leaving that point. Which is
    // the only form another pixel could use.
    if (gParams.restirMode != 0)
    {
        Reservoir cand = restirEmpty();
        if (giHaveVertex)
        {
            const float3 Ls = clamp((L - giLBefore) / max(giBeta, float3(1e-3f)),
                                    float3(0.0f), float3(64.0f));
            // The candidate's weight is its target over the pdf it was drawn
            // with. It was drawn by BSDF sampling and giBeta already carries
            // f*cos/pdf, so the balance has been paid; offering it at its own
            // target value is the unweighted-RIS form for a single candidate.
            const float pHat = luminance(Ls);
            cand.xs = giPos;
            cand.ns = giNormal;
            cand.Ls = Ls;
            cand.wSum = pHat;
            cand.W = 0.0f;
            cand.M = 1u;
        }
        gRestirCandidate[pixIndex] = cand;
        gPrimaryPos[pix] = float4(primaryPos, havePrimary ? 1.0f : 0.0f);

        // -- and shade from LAST frame's resampled reservoir ------------------
        //
        // ONLY THE DIFFUSE HALF. Specular indirect is not reusable between
        // pixels -- a mirror lobe from half a metre away is looking somewhere
        // else entirely -- so the split the tracer already computed is used to
        // route only the diffuse part through resampling and leave the rest as
        // it was traced. wDiff is that split; see the note where it is set.
        if (gParams.restirMode == 2 && havePrimary && giHaveVertex)
        {
            const Reservoir fr = gRestirFinal[pixIndex];
            const float3 toS = fr.xs - primaryPos;
            const float d2 = dot(toS, toS);
            const float3 wi = (d2 > 1e-8f) ? toS * rsqrt(d2) : primaryNormal;
            const bool usable = fr.M > 0u && fr.W > 0.0f && d2 > 1e-8f &&
                                dot(primaryNormal, wi) > 0.0f;

            // ONLY SWAP WHEN THERE IS SOMETHING TO SWAP IN.
            //
            // This is the bug that made the first version of this pass produce
            // a black wood. The subtraction was unconditional and the addition
            // was not, so every pixel whose reservoir was empty -- which is most
            // of them for the first few frames, and any pixel whose reprojection
            // and every spatial tap were rejected -- lost its indirect light
            // entirely and got nothing back. A pixel with no usable reservoir
            // must keep exactly what the path tracer gave it.
            if (usable)
            {
                // MATCHED TO THE FORM THE TRACER ALREADY PRODUCES, so that a
                // reservoir holding this pixel's own single candidate reproduces
                // the traced tail exactly and the estimator provably reduces to
                // the path tracer when there is no reuse.
                //
                // The tail is beta_v * Ls, and for a cosine-sampled diffuse
                // bounce beta_v is just the albedo -- the cosine and the 1/pi
                // cancel against the pdf. So the reused form is the same
                // product with the resampling weight carrying the aggregation.
                const float3 resampled =
                    clamp(gAlbedo * fr.Ls * fr.W, float3(0.0f), float3(32.0f));
                const float3 tail = L - giLBefore;
                L = L - tail * wDiff + resampled;
                Ldiff = Ldiff - tail * wDiff + resampled;
            }
        }
    }

#ifdef V2_NRC
    if (nrcHaveVertex)
    {
        NrcSample smp;
        smp.pos = nrcQ.pos;
        smp.dir = nrcQ.dir;
        smp.normal = nrcQ.normal;
        smp.albedo = nrcQ.albedo;
        smp.roughness = nrcQ.roughness;
        smp.world = nrcQ.world;
        smp.mtl = nrcQ.mtl;
        smp.target = clamp((L - nrcLBefore) / max(nrcBeta, float3(1e-3f)),
                           float3(0.0f), float3(64.0f));
        smp.valid = 1u;
        // Slots collide -- there are more pixels than slots -- and that is
        // harmless: a collision means one stochastic sample replaces another,
        // and both were equally valid. NrcTrain clears each slot as it consumes
        // it, so nothing here is ever trained on twice.
        gNrcSamples[uint(pixIndex) % kNrcSlots] = smp;
    }
#endif

    if (gParams.writeGuides != 0)
    {
        // An escaped ray has no surface, so the sky is given a point far down
        // its own direction. It reprojects correctly under rotation, which is
        // the only motion the sky has.
        const float3 worldP = (primaryDist > 0.0f && primaryDist < 1e4f)
                                  ? (c.pos + primaryDir * primaryDist)
                                  : (c.pos + primaryDir * 1e4f);

        // THE JITTER BELONGS IN curPix, and leaving it out was the ghost. The
        // ray for this pixel was built through pix + 0.5 + jitter, so that --
        // not the pixel centre -- is where this surface sits in the camera's
        // own unjittered screen space. projectToPixel is the exact inverse of
        // that construction, so putting the jitter back makes both ends of the
        // subtraction the same kind of quantity.
        const float2 curPix =
            float2(float(pix.x) + 0.5f, float(pix.y) + 0.5f) + gParams.jitter;
        float2 prevPix;
        // WHERE THIS SURFACE WAS, not where this point was. For the wood the
        // two are the same and primaryMoved is zero. For a butterfly they are
        // not: it flew, and reprojecting the point it is at NOW through the
        // last camera claims it was always there -- which hands Ray
        // Reconstruction a still animal on a moving one, and it believes it.
        const bool ok =
            projectToPixel(gParams.prevCam, worldP - primaryMoved, gParams.dim, prevPix);
        // ZERO ONLY WHEN THERE IS NOTHING BETTER, and for the held item that is
        // no longer the usual case. This used to read `heldPrimary ? 0`, which
        // was right about the problem and only half right as an answer: a
        // camera-locked surface really does land on the same pixel when the
        // eye moves, so zero beat reprojecting it as though it were a tree --
        // but the tool also bobs on the stride, swings, and pulls through a
        // bow's draw, and zero says none of that happened. A walking axe
        // ghosted for exactly that reason.
        //
        // primaryMoved now carries the tool's own pose change (see where it is
        // set), so the general formula is correct for it too and there is
        // nothing left to special-case here. The fallback survives for the
        // frames that have no previous pose to measure against -- a tool just
        // drawn, or one that was hidden -- which is where zero is still the
        // honest answer.
        const bool noHistory = heldPrimary && gParams.heldPrevValid == 0;
        gGuideMotion[pix] = (noHistory || !ok) ? float2(0.0f, 0.0f) : (prevPix - curPix);

        gGuideAlbedo[pix] = float4(gAlbedo, 0.0f);
        gGuideSpecular[pix] = float4(gSpecular, 0.0f);
        // Already unit: it is either a face normal (a unit axis, see above), a
        // normalised water ripple, or the negated primary direction. And the
        // texture is half precision, which cannot hold the last-bit difference
        // a re-normalise would make even if there were one.
        gGuideNormRough[pix] = float4(gNormal, gRough);
        gGuideDepth[pix] = (gViewZ > 0.0f)
                               ? ndcDepth(gViewZ, gParams.depthNear, gParams.depthFar)
                               : 1.0f;
    }

    // -----------------------------------------------------------------------
    // PHASE B, the half of it that belongs here: the SPLIT.
    //
    // Written still modulated -- albedo and Fresnel are left in. Dividing them
    // out is Demodulate.cs.slang's job, and keeping the two steps apart is what
    // makes the pass reusable: the same three channels feed a demodulating
    // denoiser and, summed, reproduce gColor exactly.
    //
    // THAT IDENTITY IS THE TEST. Ldiff + Lspec + Lemit == L, to floating point,
    // on every pixel -- run v2 with --check-demod and it is asserted on the
    // host against the composited buffer.
    // -----------------------------------------------------------------------
    if (gParams.demodulate != 0)
    {
        gDiffRadiance[pix] = float4(Ldiff, 1.0f);
        gSpecRadiance[pix] = float4(Lspec, 1.0f);
        gEmission[pix] = float4(Lemit, 1.0f);
        gDemodAlbedo[pix] = float4(demodAlbedo, 1.0f);
    }

    // -- accumulate ----------------------------------------------------------
    //
    // NOT ALWAYS. Under Ray Reconstruction at one sample a frame there is no
    // film at all: the reconstruction carries the history, the host throws the
    // accumulator away every frame, and `frame` is therefore always zero. The
    // sum of one sample is that sample. Reading and writing a full-resolution
    // RGBA32F surface to work that out is tens of megabytes a frame of pure
    // ceremony, so the host says when the film is real and it is skipped when
    // it is not.
    if (gParams.keepFilm == 0)
    {
        gColor[pix] = float4(L, 1.0f);
        return;
    }

    // frame 0 overwrites rather than adds, which is how an invalidation clears
    // the film without a separate clear dispatch.
    float4 acc = float4(L, 1.0f);
    if (gParams.frame > 0u)
    {
        const float4 p0 = gAccum[pix];
        acc = float4(p0.xyz + acc.xyz, p0.w + 1.0f);
    }

    // Roll the window forward once it is full. Scaling both parts together
    // keeps the mean exactly where it is; only the WEIGHT of what comes next
    // changes. This is what lets the film track a moving sun continuously
    // instead of being thrown away and restarted every time it shifts.
    if (gParams.maxAccum > 0u && acc.w > float(gParams.maxAccum))
    {
        const float k = float(gParams.maxAccum) / acc.w;
        acc *= k;
    }
    gAccum[pix] = acc;

    gColor[pix] = float4(acc.xyz / max(1.0f, acc.w), 1.0f);
}

#if V2_SHARC_UPDATE
// ---------------------------------------------------------------------------
// The SHaRC update pass: sparse paths whose only job is to fill the cache.
//
// A SECOND ENTRY POINT IN THIS FILE RATHER THAN A FILE OF ITS OWN, deliberately.
// The update has to shade exactly the way the camera path shades -- the same
// sun and sky next event estimation, the same BSDF, the same offsets -- or the
// cache is a record of a slightly different renderer and every path that
// terminates into it steps sideways into that other world. Sharing the file
// shares the helpers by construction; a separate file would share them by
// somebody remembering to.
//
// ONE PATH PER 5x5 BLOCK, which is 4 % of the pixels. The block's pixel is
// picked from the frame's hash, so successive frames walk over the whole screen
// and the cache ends up covering everything the camera can see rather than a
// fixed lattice of points.
//
// EVERY SEGMENT IS TREATED INDEPENDENTLY. This is the part of the algorithm
// that looks wrong and is not: the throughput is reset to one at every bounce
// and the radiance gathered is reset to zero, because what a cache entry stores
// is the radiance leaving THAT point -- a property of the point, not of the
// path that happened to arrive there. SharcSetThroughput is how the segment's
// throughput gets folded back in for the entries further up the path, and
// SharcUpdateHit is what propagates a vertex's radiance to the ones behind it.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void sharcUpdateMain(uint3 tid: SV_DispatchThreadID)
{
    const uint2 blocks = (gParams.dim + 4u) / 5u;
    if (tid.x >= blocks.x || tid.y >= blocks.y)
        return;

    const uint blockIndex = tid.y * blocks.x + tid.x;

    Rng rng;
    rng.init(uint64_t(blockIndex) + 1ull,
             uint64_t(gParams.tick) * 0x9E3779B97F4A7C15ull + uint64_t(gParams.seed) + 7919ull);

    // Somewhere in this block, different every frame.
    const uint2 pix = min(tid.xy * 5u + uint2(uint(rng.nextFloat() * 5.0f),
                                              uint(rng.nextFloat() * 5.0f)),
                          gParams.dim - 1u);

    const float px = float(pix.x) + rng.nextFloat();
    const float py = float(pix.y) + rng.nextFloat();

    const V6Camera c = gParams.cam;
    const float sx = (2.0f * px / float(gParams.dim.x) - 1.0f) * c.halfW;
    const float sy = (1.0f - 2.0f * py / float(gParams.dim.y)) * c.halfH;

    float3 o = c.pos;
    float3 d = normalize(c.w + c.u * sx + c.v * sy);

    const SharcParameters sp = sharcParams();
    SharcState state;
    SharcInit(state);

    // Reset at every vertex, not carried: see the note above.
    float3 throughput = float3(1.0f);

    for (int depth = 0; depth < gParams.maxDepth; ++depth)
    {
        const Hit h = traceRadiance(o, d);

        if (!h.hit)
        {
            // The sky closes the path and is written back along it. The disk is
            // included at full weight here because this path did not light
            // sample it on the way out -- there is no MIS partner to halve.
            SharcUpdateMiss(sp, state, skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, d) + skyDisk(gParams.sky, d));
            break;
        }

        // Water is a delta surface: it has no diffuse radiance to file under a
        // world position, so the path passes through without touching the cache.
        if (h.mat.type == MAT_DIELECTRIC)
        {
            const bool entering = dot(d, h.ns) < 0.0f;
            const Frame wfr = Frame.make(entering ? h.ns : -h.ns);
            const BsdfSample ws = sampleDielectric(h.mat, wfr.toLocal(-d), entering, rng);
            if (ws.pdf <= 0.0f)
                break;
            const float3 wwi = wfr.toWorld(ws.wi);
            o = offsetRay(h.p, h.ng, wwi);
            d = wwi;
            continue;
        }

        const Frame fr = Frame.make(h.ns);
        const float3 wo = fr.toLocal(-d);
        if (wo.z <= 0.0f)
            break;

        // The direct term at this vertex, by the same two estimators the camera
        // path uses. Depth is passed as 0 so both spend their full sample count:
        // an update path is rare enough that its vertices are worth resolving
        // properly, and what it writes is shared by every later camera path that
        // lands in the same cell.
        const float3 direct = sampleSun(h, fr, wo, rng, 0, false) +
                              sampleSky(h, fr, wo, rng, 0, false) +
                              sampleBulb(h, fr, wo, false);

        SharcHitData hd;
        hd.positionWorld = h.p;
        hd.normalWorld = h.ng;

        // Returns false when this vertex resolved from the cache and there is
        // nothing further to learn down this path.
#if V2_SHARC_VOXEL_KEY
        if (!sharcVoxelUpdateHit(sp, state, hd, direct, rng.nextFloat()))
#else
        if (!SharcUpdateHit(sp, state, hd, direct, rng.nextFloat()))
#endif
            break;

        const BsdfSample bs = sampleOpaque(h.mat, wo, rng);
        if (bs.pdf <= 0.0f || isBlack(bs.f))
            break;

        const float3 contrib = bs.f * (abs(bs.wi.z) / bs.pdf);
        if (!allFinite(contrib))
            break;
        throughput = contrib;

        const float3 wi = fr.toWorld(bs.wi);
        o = offsetRay(h.p, h.ng, wi);
        d = wi;

        // Hand the segment's throughput to the cache, then start the next one
        // from one again.
        SharcSetThroughput(state, throughput);
        throughput = float3(1.0f);

        // Roulette on the SEGMENT throughput. A cache-filling path wants to
        // reach further than a camera path -- the whole point is to record the
        // tail the camera path throws away -- so this starts later than
        // gParams.rrStart deliberately.
        if (depth >= max(gParams.rrStart, 3))
        {
            const float q = min(0.95f, maxComp(contrib));
            if (rng.nextFloat() > q)
                break;
        }
    }
}
#endif  // V2_SHARC_UPDATE
