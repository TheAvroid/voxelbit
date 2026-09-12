// ---------------------------------------------------------------------------
// VolFog.slang -- a WORLD-ANCHORED fog volume, and how a cell of it maps to
//                 the wood.
//
// WHAT CHANGED, AND WHY IT IS NOT A TWEAK.
//
// The froxel version laid its grid over the VIEW FRUSTUM: 160x90x64 cells built
// out of the camera's own basis and spaced exponentially in depth. That is the
// standard method and it has one real advantage, given up here on purpose --
// exponential slices spend most of their resolution in the first ten metres,
// where a beam through the canopy is actually resolved, and almost none at four
// hundred where nobody could tell.
//
// What it could not do is hold fog that EXISTS. Every cell was a function of
// where the camera stood and which way it faced, so turning around rebuilt the
// entire medium. The lighting in a cell described a different parcel of air
// each frame; the history had to be reprojected through the previous camera and
// was wrong wherever that reprojection was; and -- the one that showed -- an
// unshadowed term accumulated along whatever the sightline happened to be,
// which made the glow a property of the VIEWER rather than of the wood.
//
// This version stores the medium in the world. A cell is a fixed cubic region
// of forest. It holds what that air is doing whether or not anyone is looking
// at it, and it holds the same thing next frame.
//
// ---------------------------------------------------------------------------
// THE VOLUME STORES VISIBILITY, NOT RADIANCE, and that is the load-bearing
// decision.
//
// In-scattered radiance depends on the angle between the sun and the VIEW --
// that is what the phase function is -- so radiance is not a property of the
// air, and a world volume that stored it would be camera-dependent again in
// everything but name. What IS a property of the air is what the air can SEE:
//
//   R  sun visibility, 0 or 1, softened by the temporal blend
//   G  sky visibility, from the up ray
//   B  spare
//   A  density
//
// The lighting is then evaluated at MARCH time, where the view direction is
// known, from quantities the volume never had to guess. The consequence worth
// the trouble: this volume is correct for any viewer, from anywhere, and it
// does not have to be rebuilt when the camera moves -- only when the geometry
// or the sun does.
//
// ---------------------------------------------------------------------------
// TWO CASCADES, each snapped to its own cell size.
//
//   L0   fine, near     0.75 m cells,  96 m across
//   L1   coarse, far    4.00 m cells, 512 m across
//
// The clipmap answer to the problem exponential slices were solving: instead of
// one grid dense where the camera LOOKS, two grids dense where the camera IS.
// The near cascade carries the beams; the far one carries the haze on the ridge.
//
// SNAPPING IS WHAT MAKES IT WORLD-ANCHORED, and it is one floor(). The volume
// follows the player, but its origin is quantised to a whole number of its own
// cells, so a cell never straddles the world differently from one frame to the
// next. Without it the volume would slide continuously and every cell would
// resample a slightly different parcel of air every frame -- which is the
// swimming the froxel grid had, let back in through the side door.
//
// It also makes the history EXACT. Two frames' volumes differ by a WHOLE NUMBER
// OF CELLS, so reprojection is an integer offset instead of a projection
// through a matrix: a cell either has history, or it is off the edge of last
// frame's volume and has none. There is no disocclusion case left to be wrong
// about, which is the entire class of bug the froxel reprojection lived with.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"

// One cascade's resolution. Y is coarser than XZ because the haze is stratified
// by height -- it varies far more across the wood than up through it.
static const int kFogCellsX = 128;
static const int kFogCellsY = 48;
static const int kFogCellsZ = 128;
static const int kFogCascades = 2;

// The centre of cell `c`, in world metres.
float3 fogCellWorld(int3 c, float3 originWs, float3 cellSize)
{
    return originWs + (float3(c) + 0.5f) * cellSize;
}

// The extent of one cascade, in world metres.
float3 fogCascadeSpan(float3 cellSize)
{
    return cellSize * float3(float(kFogCellsX), float(kFogCellsY), float(kFogCellsZ));
}

// Where a world point falls in a cascade, as a [0,1] texture coordinate.
float3 fogWorldToUvw(float3 p, float3 originWs, float3 cellSize)
{
    return (p - originWs) / fogCascadeSpan(cellSize);
}

// Whether that coordinate is actually inside, with a margin so a trilinear tap
// never reaches past the edge and pulls in the clamped border.
bool fogInside(float3 uvw, float margin)
{
    return all(uvw > margin) && all(uvw < 1.0f - margin);
}

// Henyey-Greenstein. `g` is the anisotropy: 0 is isotropic, positive is
// forward-scattering, which is what water droplets and dust in air actually do
// and what makes the air near the sun glow rather than the whole volume.
//
// BOTH VECTORS POINT AWAY FROM THE SCATTERING POINT. This is the pbrt
// convention and the `+ 2g cos` below is only correct with it: forward
// scattering then means wo and wi are OPPOSITE, cos = -1, and the denominator
// collapses to (1 - g)^2, which is the peak.
//
// Handing it an inward vector instead flips the whole lobe back to front, and
// it does so silently -- the fog stays plausible, it is just brightest with the
// sun BEHIND you instead of in front.
float fogPhase(float3 wo, float3 wi, float g)
{
    const float cosTheta = dot(wo, wi);
    const float denom = 1.0f + g * g + 2.0f * g * cosTheta;
    return (1.0f / (4.0f * 3.14159265f)) * (1.0f - g * g) / max(pow(denom, 1.5f), 1e-4f);
}

// Density at a world point. Already world-space in the froxel version, and the
// one piece of it that needed no change at all.
float fogDensityAt(float3 p, float baseDensity, float heightFalloff)
{
    if (baseDensity <= 0.0f)
        return 0.0f;
    const float H = max(1.0f, heightFalloff);
    return baseDensity * exp(-max(0.0f, p.y) / H);
}
