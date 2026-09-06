// ---------------------------------------------------------------------------
// VolFog.slang -- the froxel grid: how a voxel of it maps to the world, and
//                 what a slice of it is worth.
//
// REPLACES THE ANALYTIC HEIGHT FOG. That fog was one exp() along the primary
// ray: exact for a uniform exponential atmosphere, deterministic, free. What it
// could not do is be SHADOWED. Its in-scatter was the sky in that direction and
// nothing else, so the air was equally bright inside a stand of pines and out
// in the open, and a sunbeam through a gap in the canopy -- the thing a conifer
// wood is actually FOR -- could not exist.
//
// This is the Frostbite/Wronski froxel method, as implemented by
// github.com/diharaw/volumetric-fog: a 3D grid laid over the view frustum,
// lit once per froxel, then integrated front to back into scattering and
// transmittance. Once the medium is a grid rather than a formula, shadowing it
// is just a visibility test per froxel.
//
// ---------------------------------------------------------------------------
// WHAT THIS PORT DOES DIFFERENTLY FROM THE REFERENCE, and why.
//
// VISIBILITY IS A RAY QUERY, NOT A SHADOW MAP. The reference projects each
// froxel into a light-space depth map and compares, with a bias constant to
// tune. v7 has a top-level acceleration structure over the whole resident wood
// and traces inline already -- so a froxel can simply ASK whether the sun is
// occluded. No shadow map to render, no cascade to fit around a 625-chunk ring,
// no depth bias, and the answer is exact rather than filtered. It costs one
// shadow ray per froxel, which is the same shape of work the path tracer does
// per vertex anyway.
//
// DENSITY KEEPS ITS HEIGHT FALLOFF. The reference uses one constant density for
// the whole volume. v7's haze pools in the hollows and thins with altitude --
// that is what fogHeight meant -- and losing it would flatten the wood as
// surely as having no fog at all. Density is therefore the old exponential,
// evaluated per froxel instead of integrated along a ray.
//
// THE CAMERA IS RAY-BASED, so none of the reference's inverse view-projection
// machinery is needed. v7's camera is an origin and three axes; a froxel's
// world position is the origin plus a direction times a distance, which is
// three multiplies rather than a matrix inverse and a perspective divide.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"

// The grid. X and Y are a coarse version of the screen; Z runs from the near
// plane to the fog's far limit.
//
// SIXTY-FOUR SLICES, EXPONENTIALLY SPACED. Linear slices would put most of the
// resolution where there is nothing to see -- the far half of a 400 m view
// distance -- and none of it in the first ten metres, which is where a beam
// through the canopy is actually resolved. The exponential distribution is the
// same one the reference uses and the same reasoning as a depth buffer's.
static const int kFogX = 160;
static const int kFogY = 90;
static const int kFogZ = 64;

// Where slice z begins, in metres from the camera.
float fogSliceDistance(int z, float nearD, float farD)
{
    return nearD * pow(farD / nearD, (float(z) + 0.5f) / float(kFogZ));
}

float fogSliceThickness(int z, float nearD, float farD)
{
    return abs(fogSliceDistance(z + 1, nearD, farD) - fogSliceDistance(z, nearD, farD));
}

// The inverse: which slice a distance falls in. Needed when the frame is
// composited and a pixel's depth has to become a Z coordinate in the grid.
float fogDistanceToSlice(float dist, float nearD, float farD)
{
    return log(max(dist, nearD) / nearD) / log(farD / nearD) * float(kFogZ);
}

// A froxel's world position.
//
// `jitter` moves the sample within its slice, in [-0.5, 0.5]. Without it the
// grid is a lattice and the fog shows it as banding along every slice boundary;
// with it, and with the temporal blend in the injection pass, the banding
// becomes noise and then averages away.
float3 fogFroxelWorld(int3 coord, float jitter, V6Camera cam, float nearD, float farD)
{
    // Screen position of this column, in [-1, 1].
    const float2 uv = (float2(coord.xy) + 0.5f) / float2(kFogX, kFogY);
    const float2 ndc = uv * 2.0f - 1.0f;

    // The same ray the tracer would build for that point -- see the camera
    // block in Trace.cs.slang. Not normalised on purpose: distances along the
    // frustum are measured on the FORWARD axis, so scaling this by a slice
    // distance lands on the plane of that slice rather than on a sphere.
    const float3 dir = cam.w + cam.u * (ndc.x * cam.halfW) + cam.v * (ndc.y * cam.halfH);

    const float d = fogSliceDistance(coord.z, nearD, farD) +
                    jitter * fogSliceThickness(coord.z, nearD, farD);
    return cam.pos + dir * d;
}

// Henyey-Greenstein. `g` is the anisotropy: 0 is isotropic, positive is
// forward-scattering, which is what water droplets and dust in air actually do
// and what makes the air near the sun glow rather than the whole volume.
float fogPhase(float3 wo, float3 wi, float g)
{
    const float cosTheta = dot(wo, wi);
    const float denom = 1.0f + g * g + 2.0f * g * cosTheta;
    return (1.0f / (4.0f * 3.14159265f)) * (1.0f - g * g) / max(pow(denom, 1.5f), 1e-4f);
}

// Density at a world point: the old height falloff, per froxel.
float fogDensityAt(float3 p, float baseDensity, float heightFalloff)
{
    if (baseDensity <= 0.0f)
        return 0.0f;
    const float H = max(1.0f, heightFalloff);
    return baseDensity * exp(-max(0.0f, p.y) / H);
}
