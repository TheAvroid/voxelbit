// ---------------------------------------------------------------------------
// VolFogMarch.cs.slang -- turn a grid of lit air into what a pixel sees.
//
// One thread per COLUMN of the grid, walking front to back and accumulating.
// Slice z ends up holding not its own scattering but everything between the
// camera and slice z: RGB is the light gathered so far, A is how much of what
// lies beyond still gets through. Compositing is then a single lookup at the
// pixel's own depth.
//
// SERIAL IN Z, AND THAT IS THE ALGORITHM RATHER THAN A LIMITATION. Each slice's
// contribution is attenuated by the transmittance of everything in front of it,
// so the walk cannot be parallelised over z without a prefix scan over a
// quantity that is only 64 long. 160 x 90 threads each doing 64 steps is
// already more parallelism than the machine needs.
// ---------------------------------------------------------------------------
#include "VolFog.slang"

Texture3D<float4> gFogIn;      // RGB in-scatter, A density -- from the inject pass
RWTexture3D<float4> gFogOut;   // RGB accumulated scattering, A transmittance

cbuffer VolFogMarchCB {
    float gNear;
    float gFar;
    uint gPad0;
    uint gPad1;
};

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(kFogX) || tid.y >= uint(kFogY))
        return;

    float3 scattering = float3(0.0f);
    float transmittance = 1.0f;

    for (int z = 0; z < kFogZ; ++z) {
        const int3 coord = int3(int(tid.x), int(tid.y), z);
        const float4 slice = gFogIn[coord];
        const float density = slice.a;

        if (density > 1e-6f) {
            const float thickness = fogSliceThickness(z, gNear, gFar);
            const float sliceT = exp(-density * thickness);

            // THE ANALYTIC INTEGRAL OF THE SLICE, not density times thickness.
            //
            // Scattering and absorption happen together: light scattered at the
            // back of a slice is attenuated by the front of the same slice. The
            // closed form of that -- (1 - e^-tau) / sigma -- is what keeps a
            // dense slice from being brighter than a thick one has any right to
            // be. Multiplying through would make the fog gain energy with
            // density instead of saturating, and it saturates in reality.
            const float3 sliceIntegral = slice.rgb * (1.0f - sliceT) / density;

            scattering += sliceIntegral * transmittance;
            transmittance *= sliceT;
        }

        gFogOut[coord] = float4(scattering, transmittance);
    }
}
