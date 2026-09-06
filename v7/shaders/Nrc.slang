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
    float3 dir;
    float3 normal;
    float3 albedo;
    float roughness;
    float3 target;
    uint valid;
};

// What the cache is asked about. Assembled at a path vertex.
struct NrcQuery {
    float3 pos;     // world position, already divided by the cache's extent
    float3 dir;     // outgoing direction, unit
    float3 normal;  // shading normal, unit
    float3 albedo;
    float roughness;
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
    return exp(clamp(logL, -20.0f, 8.0f));
}
