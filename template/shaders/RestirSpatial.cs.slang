// ---------------------------------------------------------------------------
// RestirSpatial.cs.slang -- reuse what the pixels AROUND this one found.
//
// Temporal reuse gets a pixel everything it has seen over time. Spatial reuse
// gets it everything its neighbours saw this instant, which is the half that
// still works the frame after the camera moves -- exactly when temporal history
// has just been thrown away and the image would otherwise be at its noisiest.
//
// The two together are why ReSTIR works while walking. Standing still, temporal
// carries hundreds of samples; moving, temporal is rejected and spatial is
// carrying the frame.
//
// ---------------------------------------------------------------------------
// NEIGHBOURS ARE PICKED IN A DISC, AT RANDOM, PER PIXEL AND PER FRAME.
//
// Not a fixed kernel. A fixed pattern makes every pixel reuse the same
// relative neighbours, so the same handful of samples spread through the image
// in a visible lattice -- structured error, which the eye finds far more
// objectionable than the noise it replaced. Randomising the offsets turns that
// structure back into noise, which the denoiser downstream is good at.
//
// ---------------------------------------------------------------------------
// THE JACOBIAN IS NOT OPTIONAL HERE, and this is the pass that needs it.
//
// A neighbour saw its sample point from ITS position. This pixel is somewhere
// else, so the same point subtends a different solid angle and presents a
// different cosine. Merging without correcting for that systematically
// over-weights samples reused across grazing geometry, and a voxel wood is all
// grazing geometry -- every trunk is a cylinder of perpendicular faces. The
// symptom is a bright fringe hugging every silhouette. See restirJacobian.
// ---------------------------------------------------------------------------
#include "Restir.slang"
#include "Shared.slang"

StructuredBuffer<Reservoir> gIn;
RWStructuredBuffer<Reservoir> gOut;

Texture2D<float4> gNormRough;
Texture2D<float> gDepth;
Texture2D<float4> gPrimaryPos;  // world position of the primary hit, w = valid

cbuffer RestirSpatialCB {
    uint2 gDim;
    uint gFrame;
    uint gTaps;        // neighbours to try
    float gRadius;     // in pixels
    float gDepthTol;
    float gNormalTol;
    uint gEnabled;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (any(tid.xy >= gDim))
        return;
    const uint idx = tid.y * gDim.x + tid.x;

    Reservoir r = gIn[idx];
    const float4 pos = gPrimaryPos[tid.xy];
    if (gEnabled == 0 || pos.w == 0.0f) {
        gOut[idx] = r;
        return;
    }

    Rng rng;
    rng.init(uint64_t(idx) + 0x9E37u, uint64_t(gFrame) * 0xBF58476D1CE4E5B9ull);

    const float dCentre = gDepth[tid.xy];
    const float3 nCentre = gNormRough[tid.xy].xyz;
    const float3 xr = pos.xyz;

    for (uint t = 0; t < gTaps; ++t)
    {
        // A uniform point in the disc: sqrt on the radius, or the samples pile
        // up in the middle and the reuse radius is effectively half what it says.
        const float2 u = rng.next2D();
        const float ang = u.x * 6.2831853f;
        const float rad = sqrt(u.y) * gRadius;
        const int2 q = int2(tid.xy) + int2(int(cos(ang) * rad), int(sin(ang) * rad));

        if (any(q < int2(0)) || any(q >= int2(gDim)))
            continue;
        if (all(q == int2(tid.xy)))
            continue;

        // The same two rejections the temporal pass makes, for the same reason:
        // a neighbour on a different surface is not a neighbour.
        const float dq = gDepth[uint2(q)];
        if (abs(dq - dCentre) > gDepthTol * max(dCentre, 1e-3f))
            continue;
        if (dot(gNormRough[uint2(q)].xyz, nCentre) < gNormalTol)
            continue;

        const float4 pq = gPrimaryPos[uint2(q)];
        if (pq.w == 0.0f)
            continue;

        Reservoir n = gIn[q.y * gDim.x + q.x];
        if (n.M == 0u || n.W <= 0.0f)
            continue;

        // How the geometry differs between where the neighbour stood and where
        // this pixel stands, for the point they are both now claiming to see.
        const float J = restirJacobian(xr, pq.xyz, n.xs, n.ns);
        if (J <= 0.0f)
            continue;

        // AND THE SAMPLE MUST BE ON THE RIGHT SIDE. A neighbour around a corner
        // can hold a point this pixel cannot possibly see; without this the
        // light leaks straight through the trunk between them.
        if (dot(normalize(n.xs - xr), nCentre) <= 0.0f)
            continue;

        restirMerge(r, n, J, rng);
    }

    restirFinalise(r);
    gOut[idx] = r;
}
