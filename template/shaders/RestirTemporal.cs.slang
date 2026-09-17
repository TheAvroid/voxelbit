// ---------------------------------------------------------------------------
// RestirTemporal.cs.slang -- reuse what this pixel found LAST frame.
//
// The cheapest reuse there is. A pixel that was looking at a patch of canopy
// last frame is almost certainly still looking at it, so last frame's indirect
// sample is very likely still a good sample -- and it cost nothing, because it
// was already traced.
//
// Done well, this alone is most of ReSTIR's benefit. Each frame's reservoir
// carries forward everything the pixel has seen since the camera last moved
// enough to invalidate it, so a pixel standing still is integrating over
// hundreds of bounces while tracing one.
//
// ---------------------------------------------------------------------------
// WHERE THE PIXEL WAS, AND WHY THE ANSWER IS FREE HERE.
//
// The motion vector is already in hand. The tracer writes one per pixel for
// DLSS, computed exactly rather than estimated -- the world is static and only
// the camera moves, so last frame's screen position is this frame's hit point
// projected through last frame's camera. Reprojection here is a texture read
// and a subtraction.
//
// ---------------------------------------------------------------------------
// THE TWO REJECTIONS, and they matter more than the merging does.
//
// A reprojected sample is only valid if it is the SAME SURFACE. Two tests catch
// almost everything:
//
//   DEPTH   a large change in view depth means the pixel now sees something in
//           front of or behind what it saw -- a trunk edge, a branch crossing.
//   NORMAL  a large change in orientation means a different face, even at the
//           same depth. A voxel world is nothing but perpendicular faces
//           meeting at edges, so this one does the heavy lifting here.
//
// Accepting a bad reprojection is worse than rejecting a good one: a rejected
// sample costs this pixel one noisy frame, while an accepted wrong one bleeds
// light across a silhouette and then temporal reuse keeps it there.
// ---------------------------------------------------------------------------
#include "Restir.slang"
#include "Shared.slang"
#include "RestirWorld.slang"

StructuredBuffer<Reservoir> gCandidate;  // what this frame traced
StructuredBuffer<Reservoir> gHistory;    // what last frame ended up with
RWStructuredBuffer<Reservoir> gOut;

Texture2D<float4> gNormRough;      // this frame's primary normal, xyz
Texture2D<float> gDepth;           // this frame's primary view depth
Texture2D<float2> gMotion;         // render pixels, this frame -> last
Texture2D<float4> gPrevNormRough;  // last frame's, for the rejections
Texture2D<float> gPrevDepth;
// Where the primary ray landed, which is what names the voxel face this pixel
// is standing on. The spatial pass already takes this; the world map needs it
// for the same reason.
Texture2D<float4> gPrimaryPos;

cbuffer RestirCB {
    uint2 gDim;
    uint gFrame;
    uint gMaxM;        // history cap -- see restirCapM
    float gDepthTol;   // relative depth change that still counts as the same surface
    float gNormalTol;  // cosine floor for the same
    uint gEnabled;
    uint gPad;
    // This frame's sub-pixel offset, in pixels. The motion vector is stored
    // jitter-free because that is what DLSS requires of it, so the pixel it
    // is measured FROM has to be reconstructed here: the surface this thread
    // is holding was found down a ray through tid + 0.5 + jitter, and adding
    // the vector to anything else lands up to half a pixel off the history.
    float2 gJitter;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (any(tid.xy >= gDim))
        return;
    const uint idx = tid.y * gDim.x + tid.x;

    Reservoir r = gCandidate[idx];
    if (gEnabled == 0) {
        restirFinalise(r);
        gOut[idx] = r;
        return;
    }

    // The candidate is already in the reservoir with its own weight; what
    // follows only ever ADDS history to it, so a pixel with no usable history
    // degrades exactly to a one-sample path trace rather than to nothing.
    Rng rng;
    rng.init(uint64_t(idx) + 1ull, uint64_t(gFrame) * 0x9E3779B97F4A7C15ull);

    const float2 mv = gMotion[tid.xy];
    const int2 prev = int2(floor(float2(tid.xy) + 0.5f + gJitter + mv));

    // Did this pixel keep its own history this frame? The world map is consulted
    // only when the answer is no -- see the long note in RestirWorld.slang for
    // why that restriction is what keeps the estimator unbiased.
    bool reused = false;

    if (all(prev >= int2(0)) && all(prev < int2(gDim)))
    {
        const float dNow = gDepth[tid.xy];
        const float dPrev = gPrevDepth[uint2(prev)];
        const float3 nNow = gNormRough[tid.xy].xyz;
        const float3 nPrev = gPrevNormRough[uint2(prev)].xyz;

        // RELATIVE depth, not absolute. The wood is 25 m chunks out to hundreds
        // of metres, so a tolerance that is right for a trunk in front of you is
        // far too tight for a ridge on the horizon.
        const bool depthOk = abs(dNow - dPrev) <= gDepthTol * max(dNow, 1e-3f);
        const bool normalOk = dot(nNow, nPrev) >= gNormalTol;

        if (depthOk && normalOk) {
            Reservoir h = gHistory[prev.y * gDim.x + prev.x];
            // Capped BEFORE merging, so a long-lived history cannot out-vote
            // this frame's fresh candidate and freeze the pixel. See
            // restirCapM in Restir.slang for what runs away without it.
            restirCapM(h, gMaxM);
            // Jacobian 1: same pixel, same surface, so the sample is being
            // reused from where it was found. Only SPATIAL reuse moves the
            // point of view, and only that needs the correction.
            restirMerge(r, h, 1.0f, rng);
            reused = true;
        }
    }

    // -----------------------------------------------------------------------
    // THE DISOCCLUSION PATH. The pixel is looking at something it was not
    // looking at last frame, so its own history is gone -- but the SURFACE has
    // been here all along, and other pixels have been learning about it.
    //
    // Jacobian 1, and that is not an approximation. The world reservoir was
    // filed by a pixel standing on this same voxel face, so the sample is being
    // reused from a point at most a voxel away along a shared plane; the change
    // of variables restirJacobian corrects for is the one that appears when the
    // point of view moves, and by construction it has barely moved. This is the
    // same argument the temporal merge above makes for reusing a reprojected
    // pixel, and it holds here for a stronger reason: the face is exact.
    // -----------------------------------------------------------------------
    const float4 hitPos = gPrimaryPos[tid.xy];
    const float3 hitP = hitPos.xyz;
    const float3 hitN = gNormRough[tid.xy].xyz;
    // w == 0 marks a pixel whose primary ray hit nothing -- sky. There is no
    // face to file against and none to read from. Same test the spatial pass
    // makes, and for the same reason.
    const bool onSurface = hitPos.w != 0.0f;

    if (!reused && onSurface) {
        Reservoir w = worldRead(hitP, hitN);
        restirMerge(r, w, 1.0f, rng);
    }

    restirFinalise(r);
    gOut[idx] = r;

    // -----------------------------------------------------------------------
    // ONLY PIXELS THAT KEPT THEIR OWN HISTORY MAY FILE ONE, AND THIS IS THE
    // WHOLE REASON THE ESTIMATOR SURVIVES.
    //
    // The pixels that just read from the map are excluded. Letting one write
    // back would close the loop the map exists to avoid: its reservoir now
    // CONTAINS the map's sample, so filing it would return that sample to the
    // face it came from with its M added on, and the next pixel to disocclude
    // there would draw it again with more confidence than the evidence
    // supports. Every read would inflate what the next read finds.
    //
    // That is the same defect restir.h records costing this engine 2.4x the
    // light it should have had, when the SPATIAL result was fed back as
    // history. The fix there was to feed back only the pixel's own past; the
    // fix here is the same rule, one level out -- a face only ever learns from
    // pixels that learnt it themselves.
    //
    // Filed AFTER finalising, because W is what a reader multiplies by and a
    // reservoir stored before restirFinalise carries a stale one.
    // -----------------------------------------------------------------------
    if (onSurface && reused)
        worldWrite(hitP, hitN, r);
}
