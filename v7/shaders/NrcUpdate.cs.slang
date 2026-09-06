// ---------------------------------------------------------------------------
// NrcUpdate.cs.slang -- apply one batch of gradients, then forget them.
//
// Separate from NrcTrain for the reason given there: every sample in a batch
// writes into every weight's gradient, so the update cannot happen until all of
// them have. This pass runs once per frame, one thread per weight.
//
// PLAIN SGD WITH MOMENTUM, not Adam. Adam wants two more full-sized state
// buffers and a bias correction, and buys its advantage mostly on problems with
// badly scaled gradients. This one is already well scaled -- the loss is in log
// space (see NrcTrain) and the inputs are bounded to [-1, 1] by construction --
// so momentum alone is enough, at a third of the memory traffic.
//
// THE GRADIENT IS CLEARED HERE rather than in its own pass, because it has to
// be cleared exactly once between batches and this is the only pass that is
// guaranteed to touch every element exactly once.
// ---------------------------------------------------------------------------
#include "Nrc.slang"

RWByteAddressBuffer gWeights;
RWByteAddressBuffer gGrad;
RWByteAddressBuffer gVelocity;

cbuffer NrcUpdateCB {
    float gLearningRate;
    float gMomentum;
    uint gBatchSize;  // how many samples contributed; 0 means skip
    uint gWeightCount;
};

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= gWeightCount || gBatchSize == 0)
        return;

    const uint byteOffset = i * 2;

    // fp16 in, fp32 arithmetic, fp16 out. The step is far smaller than the
    // weight, and doing that subtraction in half precision is how a network
    // silently stops learning: below about 6e-8 relative, w - lr*g rounds
    // straight back to w and the update is discarded.
    const float w = float(gWeights.Load<float16_t>(byteOffset));
    const float g = float(gGrad.Load<float16_t>(byteOffset)) / float(gBatchSize);
    const float v0 = float(gVelocity.Load<float16_t>(byteOffset));

    // Gradient clipping. One training sample from a specular caustic can carry
    // a gradient orders of magnitude above the batch, and in fp16 that is an
    // inf which poisons the weight permanently -- the network does not recover,
    // it just outputs NaN for the rest of the session.
    const float gc = clamp(g, -4.0f, 4.0f);

    const float v = gMomentum * v0 + gc;
    const float wNew = w - gLearningRate * v;

    gWeights.Store<float16_t>(byteOffset, float16_t(clamp(wNew, -8.0f, 8.0f)));
    gVelocity.Store<float16_t>(byteOffset, float16_t(clamp(v, -64.0f, 64.0f)));
    gGrad.Store<float16_t>(byteOffset, float16_t(0.0));
}
