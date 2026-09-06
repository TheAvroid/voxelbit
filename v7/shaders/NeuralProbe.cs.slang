// ---------------------------------------------------------------------------
// NeuralProbe.cs.slang -- does this device actually do cooperative vectors?
//
// This shader renders nothing and is dispatched over one thread. It exists to
// be COMPILED, because compiling it is the only honest test of the thing v7's
// whole toolchain was rebuilt for.
//
// The chain that has to hold, end to end:
//
//   1. the Slang that Falcor loads must KNOW the CoopVec type -- 2024.1.34 does
//      not, and that is why v7 ships its own 2026.13.1 (see build.bat)
//   2. it must lower coopVecMatMul to SPV_NV_cooperative_vector
//   3. slang-gfx must have enabled VK_NV_cooperative_vector on the device
//   4. the driver must expose it -- 610.88 does, at revision 4
//
// Any one of those missing and this file does not compile, which is exactly
// what makes it useful: neural.h creates a ComputePass from it at startup and
// reports which link of the chain broke, rather than the engine discovering it
// halfway through a frame.
//
// WHY A MATMUL AND NOT JUST THE TYPE. Declaring a CoopVec compiles on far more
// configurations than multiplying by one does; the matrix multiply is the
// instruction that actually needs the extension, so it is the one worth asking
// about. The shape below -- 16 in, 16 out, FP16, inferencing-optimal layout --
// is one layer of the smallest MLP anyone would really run.
// ---------------------------------------------------------------------------

// The weights. Bytes rather than a typed buffer because that is what
// coopVecMatMul wants: it reads the matrix in a driver-chosen layout whose
// element order is deliberately not specified, so there is nothing meaningful
// to type it AS.
ByteAddressBuffer gWeights;
RWStructuredBuffer<float> gOut;

[shader("compute")]
[numthreads(32, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    // A constant input, because this probe is about whether the instruction
    // EXISTS, not about what it computes. Anything data-dependent here would
    // risk the optimiser folding the matmul away and the probe passing without
    // ever having emitted one.
    CoopVec<float16_t, 16> x = CoopVec<float16_t, 16>(float16_t(1.0));

    let y = coopVecMatMul<float16_t, 16>(
        x,
        CoopVecComponentType::Float16,
        gWeights,
        /* matrix offset */ 0,
        CoopVecComponentType::Float16,
        CoopVecMatrixLayout::InferencingOptimal,
        /* transpose */ false,
        /* stride */ 32);

    // Written out so nothing above can be dead-code eliminated.
    if (tid.x == 0)
        gOut[0] = float(y[0]);
}
