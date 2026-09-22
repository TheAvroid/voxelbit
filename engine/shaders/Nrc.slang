// ---------------------------------------------------------------------------
// Nrc.slang -- the neural radiance cache: an MLP that learns what the wood
//              looks like from any point, in any direction.
//
// WHAT IT IS FOR. A path tracer spends most of its cost on the TAIL of the
// path. The first bounce decides almost everything you can see; bounces four
// through ten are carrying a few percent of the energy and most of the noise,
// and every one of them is a full ray cast into 91 million triangles.
//
// The cache is a bet that the tail is PREDICTABLE. Outgoing radiance is a
// smooth function of where you are and which way you are looking -- a small
// network can learn it, and once it has, a path can stop early and ask the
// network what the rest of it would have returned. Two bounces plus a query
// instead of ten bounces.
//
// This is the same idea as NVIDIA's Neural Radiance Cache, at the scale that
// makes sense on an Ada card: cooperative vectors run through the DP4a path
// here rather than Blackwell's in-shader tensor cores, so the network has to be
// small enough that the query is genuinely cheaper than the rays it replaces.
//
// ---------------------------------------------------------------------------
// THE SHAPE, AND WHY IT IS THIS SHAPE.
//
//     32 inputs -> 64 -> 64 -> 16, of which the first 3 are RGB
//
// Two hidden layers of 64. Wider is not obviously better here -- the function
// being learned is one wood under one sun, not a general light transport
// operator -- and 64 is a width the hardware likes. The output layer is 16
// rather than 3 because a cooperative vector wants a sensible width and the
// extra thirteen values cost nothing; they are simply ignored.
//
// THE INPUT ENCODING IS THE PART THAT MATTERS. A raw xyz fed to an MLP gives a
// blurry cache: the network cannot represent anything that varies faster than
// its first layer, and position varies fast. So position goes in through a
// frequency encoding -- sin and cos at three octaves -- which is what lets a
// small network hold sharp spatial detail. Direction, normal and albedo go in
// raw; they are already smooth.
//
//   18  position, 3 axes x 3 frequencies x (sin, cos)
//    3  outgoing direction
//    3  shading normal
//    3  albedo -- the network predicts radiance, and albedo is most of it
//    1  roughness
//    4  padding to 32
//
// ---------------------------------------------------------------------------
// ROW MAJOR, DELIBERATELY, AND IT COSTS SOMETHING.
//
// coopVecMatMul takes a layout, and InferencingOptimal is the fast one: the
// driver rearranges the matrix into whatever its hardware actually wants. It is
// also unreachable from here. The rearranging is done by a DEVICE API call
// (vkConvertCooperativeVectorMatrixNV), Falcor does not expose it, and reaching
// past Falcor to the raw VkDevice to make one call would mean owning the device
// -- see Device::Desc::existingVulkanHandles for the machinery that would take.
//
// RowMajor needs no conversion: the weights are laid out the obvious way and
// the shader reads them directly. It is slower per matmul. It is also the
// difference between a cache that exists and one that does not, and the network
// is small enough that the layout is not what decides whether this pays.
// ---------------------------------------------------------------------------
#pragma once

// voxelFaceIndex and kVoxelM. The SAME definition the radiance cache and the
// world-space reservoirs file under, because a network trained on "face 3 of
// a bark voxel" and a cache keyed on a different numbering of the faces would
// disagree about the world without either of them being wrong.
#include "VoxelKey.slang"

// Layer widths. Shared with the C++ side through Shared.slang's NrcLayout,
// because a disagreement here is a network that reads its own weights at the
// wrong offsets and produces plausible-looking nonsense.
// MUST MATCH kNrcMaxSamples in gpu/nrc.h.
static const uint kNrcSlots = 1u << 16;

static const int kNrcIn = 32;
static const int kNrcHidden = 64;
static const int kNrcOut = 16;

// Byte offsets into the single weight buffer. RowMajor fp16, so a matrix of
// R rows by C columns occupies R*C*2 bytes and its row stride is C*2.
static const uint kNrcW0 = 0;                                    // 64 x 32
static const uint kNrcW1 = kNrcW0 + kNrcHidden * kNrcIn * 2;     // 64 x 64
static const uint kNrcW2 = kNrcW1 + kNrcHidden * kNrcHidden * 2; // 16 x 64
static const uint kNrcB0 = kNrcW2 + kNrcOut * kNrcHidden * 2;
static const uint kNrcB1 = kNrcB0 + kNrcHidden * 2;
static const uint kNrcB2 = kNrcB1 + kNrcHidden * 2;
static const uint kNrcBytes = kNrcB2 + kNrcOut * 2;

// One training record: a path vertex, plus the radiance the rest of that path
// actually gathered from it. Written by Trace.cs.slang, consumed by
// NrcTrain.cs.slang. MUST MATCH NrcSampleCpu in gpu/nrc.h.
struct NrcSample {
    float3 pos;
    // The same point in METRES. Not reconstructed from pos at training time:
    // pos is relative to a cache extent that follows the camera, and a record
    // written in one frame is trained in the next, by which time the origin it
    // was normalised against has moved.
    float3 world;
    uint mtl;
    float3 dir;
    float3 normal;
    float3 albedo;
    float roughness;
    float3 target;
    uint valid;
};

// ---------------------------------------------------------------------------
// IS THIS FLOAT NaN OR INFINITE -- TESTED ON THE BITS, AND THAT IS THE POINT.
//
// The obvious spellings are isfinite(x) and !(abs(x) < 1e30f), and both were
// tried here. Both are correct as written and both can be compiled away: a
// shader compiler is entitled to assume finite arithmetic, and under that
// assumption every NaN test is provably false and the guard disappears. What
// that looked like in practice was a network that still filled with NaN
// through guards that read as airtight, and weight files whose corruption
// was bit-identical across two entirely different optimisers -- which is the
// tell, because two optimisers cannot agree on garbage unless neither of
// them is what produced it.
//
// An exponent of all ones is inf or NaN by definition of IEEE 754, and
// asuint is an integer reinterpretation that no float fast-math rule reaches.
// ---------------------------------------------------------------------------
bool nrcNotFinite(float x)
{
    return (asuint(x) & 0x7F800000u) == 0x7F800000u;
}

bool nrcNotFinite3(float3 v)
{
    return nrcNotFinite(v.x) || nrcNotFinite(v.y) || nrcNotFinite(v.z);
}

// What the cache is asked about. Assembled at a path vertex.
struct NrcQuery {
    float3 pos;     // world position, already divided by the cache's extent
    float3 dir;     // outgoing direction, unit
    float3 normal;  // shading normal, unit
    float3 albedo;
    float roughness;
    // ---- the voxel-native half -------------------------------------------
    float3 world;   // the SAME point in METRES, undivided. The normalised
                    // position above is relative to a cache extent that follows
                    // the camera, so it cannot name a voxel; this can.
    float3 sun;     // direction to the sun, so the network conditions on the
                    // time of day instead of baking one into its weights
    uint mtl;       // which material this face wears, after groundShade
};

// -------------------------------------------------------------------------
// The frequency encoding, and the rest of the input vector with it.
//
// Position arrives ALREADY NORMALISED to roughly [-1, 1] over the cache's
// extent -- done on the host side, because the extent follows the camera and
// the shader should not have to know how far the ring reaches.
// -------------------------------------------------------------------------
CoopVec<float16_t, kNrcIn> nrcEncode(NrcQuery q)
{
    float f[kNrcIn];

#if V2_NRC_VOXEL_FEATURES
    // =====================================================================
    // THE VOXEL-NATIVE ENCODING.
    //
    // Same 32 inputs, same network, entirely different question asked of it.
    //
    // WHAT THE FREQUENCY ENCODING IS FOR, AND WHY THIS WORLD DOES NOT NEED IT.
    // An MLP cannot represent anything that varies faster than its first layer,
    // and radiance in a wood varies fast -- it changes completely from one
    // voxel to the next. The standard answer is to encode position at several
    // octaves so the network can localise, and it works, but it makes the
    // network learn WHERE it is before it can learn what anything looks like.
    // That is why an NRC is slow to converge and blurry when it does: most of
    // its capacity goes on memorising a coordinate system.
    //
    // In this world the fast-varying part is not continuous at all. It is
    // WHICH BLOCK AND WHICH FACE -- a discrete identity the geometry already
    // carries exactly. Hand that to the network directly and the remaining
    // function is smooth: how does bark on a north face look, with the sun
    // there. A small network is good at smooth.
    //
    // AND THE PAYOFF IS NOT ONLY CONVERGENCE SPEED. Every input below except
    // the coarse position is a property of the MATERIAL AND ITS ORIENTATION,
    // not of a place. A wood generated from a different seed is made of the
    // same hundred materials on the same six faces under the same sun, so a
    // network trained here is not specific to here. That is what makes the
    // weights worth saving to disk, and it is not available to a renderer
    // whose scenes are unique triangle meshes.
    //
    //    6  material identity code    -- discrete, hashed, world-independent
    //    6  face, one-hot             -- the six axis-aligned planes
    //    2  position WITHIN the face  -- smooth, bounded, world-independent
    //   12  coarse position, 2 octaves -- large-scale variation only
    //    3  outgoing direction
    //    3  direction to the sun
    //   --
    //   32
    // =====================================================================
    int i = 0;

    // A FIXED RANDOM CODE PER MATERIAL, NOT A LEARNED EMBEDDING. A learned
    // table would need its own gradients and its own slice of the weight
    // buffer; a hash gives well-separated, repeatable codes for nothing, and
    // separation is all the first layer actually wants. The codes are the same
    // in every world, which is the property that lets weights transfer.
    {
        uint h = q.mtl * 2654435761u;
        [ForceUnroll]
        for (int k = 0; k < 6; ++k) {
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            // Signed and unit-scaled: an input the network sees as centred.
            f[i++] = float(h & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
        }
    }

    // ONE-HOT AND NOT THE NORMAL VECTOR. Three floats can express the six axis
    // directions, but they express a continuum the geometry does not have, so
    // the first layer has to carve six points out of a sphere before it can use
    // them. Six inputs, one of them set, hands it the answer.
    {
        const uint face = voxelFaceIndex(q.normal);
        [ForceUnroll]
        for (uint k = 0; k < 6u; ++k) f[i++] = (k == face) ? 1.0f : 0.0f;
    }

    // WHERE ON THE FACE, in 0..1 across the two axes the face spans. This is
    // the only sub-voxel term and it is deliberately world-independent: the
    // corner of a voxel looks like the corner of a voxel wherever it is.
    {
        // frac() and no bias term. frac is x - floor(x) here, so it already
        // returns 0..1 for negative coordinates -- and this world runs to
        // kilometres in both directions, so any additive bias large enough to
        // matter would also be large enough to lose the fraction it was added
        // to protect.
        const float3 inVox = frac(q.world / kVoxelM);
        const uint face = voxelFaceIndex(q.normal);
        // Skip the axis the face points along -- it is constant across a face.
        const float2 uv = (face < 2u) ? inVox.xz : ((face < 4u) ? inVox.yz : inVox.xy);
        f[i++] = uv.x * 2.0f - 1.0f;
        f[i++] = uv.y * 2.0f - 1.0f;
    }

    // TWO OCTAVES, NOT THREE. What is left for position to explain is the slow
    // part -- which side of the hill, how deep into the wood -- because
    // everything that varies per voxel is already above. The octave that used
    // to resolve individual voxels is the one dropped.
    [ForceUnroll]
    for (int axis = 0; axis < 3; ++axis) {
        const float p = q.pos[axis];
        [ForceUnroll]
        for (int oct = 0; oct < 2; ++oct) {
            const float w = float(1 << oct) * 3.14159265f;
            f[i++] = sin(p * w);
            f[i++] = cos(p * w);
        }
    }

    f[i++] = q.dir.x; f[i++] = q.dir.y; f[i++] = q.dir.z;
    f[i++] = q.sun.x; f[i++] = q.sun.y; f[i++] = q.sun.z;
    // 6 + 6 + 2 + 12 + 3 + 3 = 32 exactly. Nothing spare, and nothing to pad.
#else
    // 18: three axes, three octaves, sin and cos. The octaves are 1, 2 and 4 --
    // one more than that and the highest frequency is finer than the 10 cm
    // voxels the world is made of, which is detail the cache cannot be asked
    // to hold because the geometry does not have it either.
    int i = 0;
    [ForceUnroll]
    for (int axis = 0; axis < 3; ++axis) {
        const float p = q.pos[axis];
        [ForceUnroll]
        for (int oct = 0; oct < 3; ++oct) {
            const float w = float(1 << oct) * 3.14159265f;
            f[i++] = sin(p * w);
            f[i++] = cos(p * w);
        }
    }

    f[i++] = q.dir.x;    f[i++] = q.dir.y;    f[i++] = q.dir.z;
    f[i++] = q.normal.x; f[i++] = q.normal.y; f[i++] = q.normal.z;
    f[i++] = q.albedo.x; f[i++] = q.albedo.y; f[i++] = q.albedo.z;
    f[i++] = q.roughness;

    // 18 + 3 + 3 + 3 + 1 = 28, so four slots are spare. Written out rather
    // than looped: after the loops above the compiler no longer tracks i as a
    // constant, and [ForceUnroll] on a bound it cannot evaluate is a hard
    // error rather than a missed optimisation.
    f[28] = 0.0f; f[29] = 0.0f; f[30] = 0.0f; f[31] = 0.0f;
#endif

    CoopVec<float16_t, kNrcIn> v;
    [ForceUnroll]
    for (int k = 0; k < kNrcIn; ++k) v[k] = float16_t(f[k]);
    return v;
}

// -------------------------------------------------------------------------
// The forward pass. Two hidden layers with ReLU, a linear output.
//
// THE OUTPUT IS EXPONENTIATED rather than read straight out, and that is not
// cosmetic. Radiance is positive and spans orders of magnitude -- sunlit needles
// against shadowed trunk is a factor of hundreds -- and a linear output layer
// trained on that spends all its capacity on the bright end and represents the
// shadows as approximately zero. Learning the LOG and exponentiating turns a
// multiplicative error into an additive one, which is the error a network
// trained on squared loss can actually minimise.
//
// Clamped before the exp because an untrained network produces large values and
// exp of a large float16 is inf, which would put NaNs into the film within one
// frame of switching the cache on.
// -------------------------------------------------------------------------
float3 nrcEval(ByteAddressBuffer W, NrcQuery q)
{
    let x = nrcEncode(q);

    let h0 = max(coopVecMatMulAdd<float16_t, kNrcHidden>(
                     x, CoopVecComponentType::Float16,
                     W, kNrcW0, CoopVecComponentType::Float16,
                     W, kNrcB0, CoopVecComponentType::Float16,
                     CoopVecMatrixLayout::RowMajor, false, kNrcIn * 2),
                 CoopVec<float16_t, kNrcHidden>(float16_t(0.0)));

    let h1 = max(coopVecMatMulAdd<float16_t, kNrcHidden>(
                     h0, CoopVecComponentType::Float16,
                     W, kNrcW1, CoopVecComponentType::Float16,
                     W, kNrcB1, CoopVecComponentType::Float16,
                     CoopVecMatrixLayout::RowMajor, false, kNrcHidden * 2),
                 CoopVec<float16_t, kNrcHidden>(float16_t(0.0)));

    let o = coopVecMatMulAdd<float16_t, kNrcOut>(
        h1, CoopVecComponentType::Float16,
        W, kNrcW2, CoopVecComponentType::Float16,
        W, kNrcB2, CoopVecComponentType::Float16,
        CoopVecMatrixLayout::RowMajor, false, kNrcHidden * 2);

    const float3 logL = float3(float(o[0]), float(o[1]), float(o[2]));
    // A NaN here is not a bad pixel, it is a dead FRAME: the film accumulates,
    // so one NaN sample turns the whole image black and keeps it black. The
    // clamp below does not stop it -- clamp(NaN) is NaN on this hardware.
    if (nrcNotFinite3(logL))
        return float3(0.0f);
    return exp(clamp(logL, -20.0f, 8.0f));
}
