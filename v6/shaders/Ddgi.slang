// ---------------------------------------------------------------------------
// Ddgi.slang -- reading the irradiance probes.
//
// This is the consumer side of PHASE A's indirect half. ProbeTrace.cs.slang
// fills the probes; RTXGI's blend pass turns rays into octahedral maps; this
// file is how a shading point asks the result a question.
//
// It mirrors DDGIGetVolumeIrradiance() from RTXGI's Irradiance.hlsl rather than
// including it, and that is a deliberate trade. Including the SDK's HLSL would
// drag DDGIRootConstants, the packed volume-descriptor struct and RTXGI's own
// resource-register scheme into a Slang translation unit that has none of them.
// Reimplementing the lookup is about eighty lines and leaves v6's shaders
// depending on nothing but the four probe textures and their dimensions.
//
// THE PRICE OF THAT TRADE IS THAT EVERY CONVENTION HERE MUST MATCH THE BLEND
// SHADER THAT WROTE THE ATLAS -- octahedral orientation, border layout,
// irradiance encoding gamma, and the Chebyshev visibility test. Each is called
// out below beside the SDK function it corresponds to.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"
#include "Vecmath.slang"

// ---------------------------------------------------------------------------
// Octahedral mapping. Mirrors DDGIGetOctahedralCoordinates() /
// DDGIGetOctahedralDirection() in ProbeOctahedral.hlsl.
//
// A probe stores a whole sphere of directions in a square, and the octahedral
// parameterisation is what makes that square almost area-uniform -- far better
// than a lat-long map, which wastes most of its texels near the poles and
// distorts exactly where a horizon line falls.
// ---------------------------------------------------------------------------
float2 ddgiOctEncode(float3 d)
{
    const float l1 = abs(d.x) + abs(d.y) + abs(d.z);
    float2 uv = d.xy * (1.0f / max(1e-8f, l1));
    if (d.z < 0.0f)
    {
        // Fold the lower hemisphere out across the diagonals.
        uv = (1.0f - abs(uv.yx)) * float2(uv.x >= 0.0f ? 1.0f : -1.0f,
                                          uv.y >= 0.0f ? 1.0f : -1.0f);
    }
    return uv;
}

// ---------------------------------------------------------------------------
// Where one probe's texels live in the atlas.
//
// EVERY PROBE CARRIES A ONE-TEXEL BORDER and the border is not padding -- it is
// a copy of the opposite edge, written by the blend pass so that hardware
// bilinear filtering wraps correctly across the octahedral seam. So the uv has
// to be mapped into the INTERIOR texels and then offset past the border. Miss
// this and every probe shows a seam that moves as the camera turns.
// ---------------------------------------------------------------------------
float2 ddgiProbeUV(int3 coords, float2 octUv, int3 counts, float numTexels, float numInterior)
{
    // Probe position within the slice, matching the layout
    // GetDDGIVolumeTextureDimensions() produces for a Y-up volume: x across,
    // z down, y as the array slice.
    const float2 probeXY = float2(float(coords.x), float(coords.z));

    const float2 texelXY = (octUv * 0.5f + 0.5f) * numInterior + 1.0f;  // +1 skips the border
    const float2 atlasXY = probeXY * numTexels + texelXY;
    const float2 atlasSize = float2(float(counts.x), float(counts.z)) * numTexels;
    return atlasXY / atlasSize;
}

// ---------------------------------------------------------------------------
// The lookup.
//
// Eight probes around the point, trilinearly weighted, each weight further
// scaled by two things a plain trilinear blend has no way to know:
//
//   THE WRAP TERM   a probe behind the surface should not light it. The
//                   smooth (dot(dir, normal) + 1) * 0.5 form is used rather
//                   than a hard cutoff because a hard one makes the weight
//                   discontinuous as the point moves between cells, which
//                   reads as banding on a slowly curving surface.
//
//   CHEBYSHEV       the real work. Each probe also stored the mean and mean
//                   square of the distance it saw in that direction, and those
//                   two numbers give a variance -- so the probe can answer
//                   "how likely is it that I can actually SEE this point"
//                   rather than just "how far away am I". This is what stops
//                   light leaking through a trunk from a probe on the far side
//                   of it, and on a voxel wood, which is nothing but thin
//                   occluders, it is the difference between usable and not.
// ---------------------------------------------------------------------------
float3 ddgiSampleIrradiance(float3 worldPos, float3 normal, float3 viewDir,
                            V6DdgiConsts v,
                            Texture2DArray<float4> irradianceTex,
                            Texture2DArray<float4> distanceTex,
                            Texture2DArray<float4> probeDataTex,
                            SamplerState samp)
{
    // Push the sample point off the surface before locating it in the grid.
    // Without this a point sitting exactly on a face lands in whichever cell
    // rounding chose, and neighbouring pixels disagree.
    const float3 bias = (normal * 0.2f + viewDir * 0.8f) * v.normalBias;
    const float3 p = worldPos + bias;

    const float3 half = (float3(v.probeCounts) - 1.0f) * 0.5f;
    const float3 gridOrigin = v.origin + float3(v.probeScroll) * v.probeSpacing
                              - half * v.probeSpacing;
    const float3 gridF = (p - gridOrigin) / v.probeSpacing;
    const int3 base = int3(floor(gridF));
    const float3 frac3 = saturate(gridF - float3(base));

    float3 sum = float3(0.0f);
    float weightSum = 0.0f;

    for (int i = 0; i < 8; ++i)
    {
        const int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 c = clamp(base + offset, int3(0), v.probeCounts - 1);

        const float3 trilinear = max(float3(1e-3f), lerp(1.0f - frac3, frac3, float3(offset)));
        float weight = trilinear.x * trilinear.y * trilinear.z;

        const float3 probePos = gridOrigin + float3(c) * v.probeSpacing;
        const uint3 dataIdx = uint3(uint(c.x + c.z * v.probeCounts.x), uint(c.y), 0);
        const float4 pdata = probeDataTex[dataIdx];
        const float3 probeWorld = probePos + pdata.xyz;   // relocation offset

        // An inactive probe is inside geometry and has nothing to say.
        if (v.useFixedRays != 0 && pdata.w < 0.0f) continue;

        const float3 toProbe = probeWorld - worldPos;
        const float dist = length(toProbe);
        const float3 dirToProbe = dist > 1e-5f ? (toProbe / dist) : normal;

        // -- the smooth backface term ---------------------------------------
        const float wrap = (dot(dirToProbe, normal) + 1.0f) * 0.5f;
        weight *= wrap * wrap + 0.2f;

        // -- Chebyshev visibility -------------------------------------------
        const float2 distOct = ddgiOctEncode(normalize(-dirToProbe));
        const float2 distUv = ddgiProbeUV(c, distOct, v.probeCounts,
                                          v.distanceTexels.x, v.distanceTexels.y);
        const float2 moments =
            distanceTex.SampleLevel(samp, float3(distUv, float(c.y)), 0).rg;
        const float mean = moments.x;
        if (dist > mean)
        {
            const float variance = abs(mean * mean - moments.y);
            // The +1e-2 keeps a probe with zero variance -- one staring at a
            // flat wall -- from producing a hard 0/1 step.
            float cheb = variance / max(1e-4f, variance + (dist - mean) * (dist - mean));
            cheb = max(cheb * cheb * cheb, 0.0f);
            weight *= cheb;
        }

        // Crush tiny weights to zero rather than letting thousands of them sum
        // into a haze. The threshold is RTXGI's own.
        if (weight < 0.05f) weight *= (weight * weight) / (0.05f * 0.05f);

        const float2 irrOct = ddgiOctEncode(normal);
        const float2 irrUv = ddgiProbeUV(c, irrOct, v.probeCounts,
                                         v.irradianceTexels.x, v.irradianceTexels.y);
        float3 irr = irradianceTex.SampleLevel(samp, float3(irrUv, float(c.y)), 0).rgb;

        // THE ATLAS IS STORED WITH A HIGH GAMMA, not because anything displays
        // it but because irradiance spans orders of magnitude and a 10-bit
        // unorm atlas cannot hold that linearly. Undo it here, exactly as the
        // blend pass applied it.
        irr = pow(irr, float3(v.irradianceGamma * 0.5f));

        sum += irr * weight;
        weightSum += weight;
    }

    if (weightSum <= 0.0f) return float3(0.0f);
    float3 result = sum / weightSum;
    result = result * result;              // the other half of the gamma
    return result * TWO_PI;                // stored as radiance; wanted as irradiance
}
