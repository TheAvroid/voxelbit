// ---------------------------------------------------------------------------
// NrcTrain.cs.slang -- teaching the cache, one frame at a time.
//
// The cache is not trained offline and shipped. It is trained ON THIS WOOD, on
// this walk, while you are walking it -- because the thing it has to predict is
// a function of where the sun is, which chunks are resident and where you are
// standing, and none of that exists until the frame does.
//
// WHERE THE TRAINING DATA COMES FROM. A small fraction of paths -- one pixel in
// a few hundred, chosen by the tracer -- are allowed to run to FULL depth
// instead of stopping at the cache. Those paths know the answer the cache was
// supposed to give: the radiance actually gathered down the rest of the path.
// Each one writes a record, and this pass fits the network to them.
//
// So the cache is always learning from the renderer it is accelerating, which
// is the property that makes it track a moving sun without anyone telling it
// the sun moved.
//
// ---------------------------------------------------------------------------
// ONE SUBGROUP PER SAMPLE, and that is forced rather than chosen.
//
// A cooperative vector is spread ACROSS a subgroup -- that is the whole point,
// it is what lets a matrix multiply issue as one instruction. So the unit of
// work here is a subgroup, not a thread: 32 lanes cooperate on one training
// sample. numthreads(32) makes one group exactly one sample, which keeps the
// indexing honest.
//
// ---------------------------------------------------------------------------
// GRADIENTS ARE ACCUMULATED, THEN APPLIED SEPARATELY.
//
// Many samples write into one gradient buffer with atomic accumulation, and a
// second pass (NrcUpdate) applies them and clears. Doing the update here would
// mean every sample racing to write every weight -- the last writer would win
// and the batch would collapse to a batch of one.
// ---------------------------------------------------------------------------
#include "Nrc.slang"

RWStructuredBuffer<NrcSample> gSamples;
ByteAddressBuffer gWeights;
RWByteAddressBuffer gGrad;

cbuffer NrcTrainCB {
    uint gSampleCount;
};

[shader("compute")]
[numthreads(32, 1, 1)]
void main(uint3 gid: SV_GroupID)
{
    if (gid.x >= gSampleCount)
        return;

    const NrcSample s = gSamples[gid.x];
    if (s.valid == 0)
        return;

    // CONSUMED, not just read. Without this the slot keeps its record and gets
    // trained on again every frame until something overwrites it -- which
    // quietly turns a stream of fresh samples into a handful of stale ones
    // repeated forever, and the cache stops tracking the sun.
    gSamples[gid.x].valid = 0u;

    NrcQuery q;
    q.pos = s.pos;
    q.dir = s.dir;
    q.normal = s.normal;
    q.albedo = s.albedo;
    q.roughness = s.roughness;

    // -- forward, keeping every activation the backward pass needs -----------
    let x = nrcEncode(q);

    let z0 = coopVecMatMulAdd<float16_t, kNrcHidden>(
        x, CoopVecComponentType::Float16,
        gWeights, kNrcW0, CoopVecComponentType::Float16,
        gWeights, kNrcB0, CoopVecComponentType::Float16,
        CoopVecMatrixLayout::RowMajor, false, kNrcIn * 2);
    let h0 = max(z0, CoopVec<float16_t, kNrcHidden>(float16_t(0.0)));

    let z1 = coopVecMatMulAdd<float16_t, kNrcHidden>(
        h0, CoopVecComponentType::Float16,
        gWeights, kNrcW1, CoopVecComponentType::Float16,
        gWeights, kNrcB1, CoopVecComponentType::Float16,
        CoopVecMatrixLayout::RowMajor, false, kNrcHidden * 2);
    let h1 = max(z1, CoopVec<float16_t, kNrcHidden>(float16_t(0.0)));

    let o = coopVecMatMulAdd<float16_t, kNrcOut>(
        h1, CoopVecComponentType::Float16,
        gWeights, kNrcW2, CoopVecComponentType::Float16,
        gWeights, kNrcB2, CoopVecComponentType::Float16,
        CoopVecMatrixLayout::RowMajor, false, kNrcHidden * 2);

    // -- the loss ------------------------------------------------------------
    //
    // FITTED IN LOG SPACE, matching nrcEval's exp on the way out. Squared error
    // on raw radiance is dominated by the brightest samples in the batch: a
    // sunlit needle at 400 and a shadowed trunk at 0.4 differ by a factor of a
    // thousand, so the trunk contributes about a millionth of the gradient and
    // the network simply never learns the shadows. In log space both are
    // errors of comparable size, which is the only way one small network fits
    // the whole dynamic range of a wood.
    //
    // The +1e-3 keeps log finite where a path gathered nothing at all, which
    // happens constantly -- most of a canopy is in shadow.
    const float3 t = log(max(s.target, float3(1e-3f)));

    // dL/dz2 for squared error, zero in the thirteen unused output slots so
    // they never accumulate a gradient and drift.
    CoopVec<float16_t, kNrcOut> d2;
    [ForceUnroll]
    for (int k = 0; k < kNrcOut; ++k) d2[k] = float16_t(0.0);
    d2[0] = float16_t(clamp(float(o[0]) - t.x, -8.0f, 8.0f));
    d2[1] = float16_t(clamp(float(o[1]) - t.y, -8.0f, 8.0f));
    d2[2] = float16_t(clamp(float(o[2]) - t.z, -8.0f, 8.0f));

    // -- backward ------------------------------------------------------------
    //
    // Layer 2. The weight gradient of a linear layer is the outer product of
    // the incoming delta with the activation that fed it -- which is exactly
    // what coopVecOuterProductAccumulate computes, in one instruction, straight
    // into the gradient buffer.
    coopVecOuterProductAccumulate(d2, h1, gGrad, kNrcW2, kNrcHidden * 2,
                                  CoopVecMatrixLayout::RowMajor,
                                  CoopVecComponentType::Float16);
    coopVecReduceSumAccumulate(d2, gGrad, kNrcB2);

    // Propagate through W2 (transpose = true), then through the ReLU: the
    // derivative is 1 where the unit fired and 0 where it did not, and h1 is
    // already the post-ReLU activation, so its sign is the mask.
    var d1 = coopVecMatMul<float16_t, kNrcHidden>(
        d2, CoopVecComponentType::Float16,
        gWeights, kNrcW2, CoopVecComponentType::Float16,
        CoopVecMatrixLayout::RowMajor, true, kNrcHidden * 2);
    [ForceUnroll]
    for (int k = 0; k < kNrcHidden; ++k)
        if (float(h1[k]) <= 0.0f) d1[k] = float16_t(0.0);

    coopVecOuterProductAccumulate(d1, h0, gGrad, kNrcW1, kNrcHidden * 2,
                                  CoopVecMatrixLayout::RowMajor,
                                  CoopVecComponentType::Float16);
    coopVecReduceSumAccumulate(d1, gGrad, kNrcB1);

    var d0 = coopVecMatMul<float16_t, kNrcHidden>(
        d1, CoopVecComponentType::Float16,
        gWeights, kNrcW1, CoopVecComponentType::Float16,
        CoopVecMatrixLayout::RowMajor, true, kNrcHidden * 2);
    [ForceUnroll]
    for (int k = 0; k < kNrcHidden; ++k)
        if (float(h0[k]) <= 0.0f) d0[k] = float16_t(0.0);

    coopVecOuterProductAccumulate(d0, x, gGrad, kNrcW0, kNrcIn * 2,
                                  CoopVecMatrixLayout::RowMajor,
                                  CoopVecComponentType::Float16);
    coopVecReduceSumAccumulate(d0, gGrad, kNrcB0);
}
