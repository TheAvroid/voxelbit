// ---------------------------------------------------------------------------
// NrcUpdate.cs.slang -- apply one batch of gradients, then forget them.
//
// Separate from NrcTrain for the reason given there: every sample in a batch
// writes into every weight's gradient, so the update cannot happen until all of
// them have. This pass runs once per frame, one thread per weight.
//
// THE GRADIENT IS CLEARED HERE rather than in its own pass, because it has to
// be cleared exactly once between batches and this is the only pass that is
// guaranteed to touch every element exactly once.
//
// ---------------------------------------------------------------------------
// ADAM, AND THE REASON IS A MEASURED FAILURE RATHER THAN A PREFERENCE.
//
// This was plain SGD with momentum, on the argument that the problem is already
// well scaled -- the loss is in log space, the inputs are bounded -- so momentum
// alone should do, at a third of the memory traffic. The argument was wrong, and
// the way it was wrong is worth writing down, because it looked like a training
// instability and was not one.
//
// WHAT ACTUALLY HAPPENED. Under momentum 0.9 with the gradient clipped to +/-4,
// a weight whose gradient sits at the clip accumulates velocity to 4/(1-0.9) =
// 40, and then moves by learningRate * 40 EVERY STEP. At the shipped rate of
// 0.01 that is 0.4 a step: the weight crosses its own +/-8 clamp in twenty
// iterations and stays there. Lowering the rate only changed how long it took --
// diverged by 100 batches at 0.001, and still by 1500 at a tenth of that. The
// network always ended in the SAME state, 228 weights NaN and 2904 pinned at the
// clamp, which is the signature of a deterministic ramp rather than of noise.
//
// Adam divides the step by the running RMS of the gradient, so the step is
// bounded by roughly the learning rate whatever the gradient does. A weight can
// no longer ramp; it can only walk, at a rate that is set rather than emergent.
// That is the property this needed and momentum cannot provide at any tuning.
//
// The two extra state buffers are 58 KB together. The reason given for avoiding
// them was memory traffic on a 14 KB network, which was never the constraint.
//
// ---------------------------------------------------------------------------
// EVERY LOAD IS GUARDED AGAINST NON-FINITE VALUES, AND THAT IS THE OTHER HALF.
//
// One NaN gradient used to be permanent. clamp(NaN, -4, 4) is NaN, so the
// velocity became NaN, so the weight became NaN, and it stayed NaN for the rest
// of the session -- one bad training sample out of millions killing a weight
// forever. Adam does not fix that by itself: NaN propagates through the moment
// estimates just as happily as through a velocity.
//
// So a non-finite gradient is treated as no gradient, and a moment or a weight
// that has somehow gone non-finite is reset rather than propagated. The cost is
// three comparisons per weight per frame; the benefit is that one bad sample
// costs one update instead of the run.
//
// THE TEST IS ON THE BITS, and getting there took two wrong attempts. isfinite()
// and !(abs(x) < 1e30f) are both correct as written and both were compiled away:
// under an assumption of finite arithmetic every NaN test is provably false, so
// the guard simply is not there in the binary. nrcNotFinite in Nrc.slang reads
// the IEEE exponent field instead, which no float rule reaches.
// ---------------------------------------------------------------------------
#include "Nrc.slang"

RWByteAddressBuffer gWeights;
RWByteAddressBuffer gGrad;
// Adam's two moments, fp32 and full width. They are a running mean and a running
// mean square of the gradient, so they carry the same overflow argument the
// gradient buffer does -- see the backward-pass note in NrcTrain.cs.slang.
RWByteAddressBuffer gAdamM;
RWByteAddressBuffer gAdamV;

cbuffer NrcUpdateCB {
    float gLearningRate;
    float gBeta1;
    float gBeta2;
    float gEpsilon;
    uint gBatchSize;   // how many samples contributed; 0 means skip
    uint gWeightCount;
    // 1-BASED, and it has to be: the bias correction divides by 1 - beta^step,
    // which at step 0 is zero and makes the first update infinite.
    uint gStep;
    uint gPad;
};

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= gWeightCount || gBatchSize == 0)
        return;

    // Weights are fp16; the gradient and both moments are fp32 beside them, so
    // they index at twice the stride.
    const uint wOff = i * 2;
    const uint fOff = i * 4;

    float g = gGrad.Load<float>(fOff) / float(gBatchSize);
    if (nrcNotFinite(g))
        g = 0.0f;

    // Kept from the SGD version. Adam is scale-free in the long run, but one
    // sample off a specular caustic can carry a gradient orders of magnitude
    // above the batch, and letting that into the second moment makes every
    // OTHER step for that weight tiny for the next few hundred iterations.
    g = clamp(g, -4.0f, 4.0f);

    float m = gAdamM.Load<float>(fOff);
    float v = gAdamV.Load<float>(fOff);
    if (nrcNotFinite(m)) m = 0.0f;
    if (nrcNotFinite(v)) v = 0.0f;

    m = gBeta1 * m + (1.0f - gBeta1) * g;
    v = gBeta2 * v + (1.0f - gBeta2) * g * g;

    // Bias correction. Both moments start at zero, so without it the first few
    // hundred steps are scaled down by however far beta is from 1 -- and for
    // beta2 = 0.999 that means the network barely moves for its first thousand
    // batches and looks like it is simply not learning.
    const float c1 = max(1.0f - pow(gBeta1, float(gStep)), 1e-8f);
    const float c2 = max(1.0f - pow(gBeta2, float(gStep)), 1e-8f);
    const float mHat = m / c1;
    const float vHat = max(v / c2, 0.0f);

    float w = float(gWeights.Load<float16_t>(wOff));
    if (nrcNotFinite(w)) w = 0.0f;

    // fp32 arithmetic, fp16 out. The step is far smaller than the weight, and
    // doing that subtraction in half precision is how a network silently stops
    // learning: below about 6e-8 relative, w - step rounds straight back to w
    // and the update is discarded.
    float wNew = w - gLearningRate * mHat / (sqrt(vHat) + gEpsilon);
    if (nrcNotFinite(wNew))
        wNew = w;

    // The clamp stays, as the backstop it was meant to be rather than the
    // mechanism it became. An Adam step is bounded by about the learning rate,
    // so reaching +/-8 now takes thousands of consecutive same-signed steps.
    gWeights.Store<float16_t>(wOff, float16_t(clamp(wNew, -8.0f, 8.0f)));
    gAdamM.Store<float>(fOff, m);
    gAdamV.Store<float>(fOff, v);
    gGrad.Store<float>(fOff, 0.0f);
}
