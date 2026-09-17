// ---------------------------------------------------------------------------
// SharcResolve.cs.slang -- fold this frame's cache samples into the running one.
//
// One thread per cache entry. The update pass writes raw sums into the
// accumulation buffer with atomics; this pass turns each of those into the
// running cross-frame average that the tracer actually reads, and evicts
// entries nothing has written to for a while.
//
// WHY IT IS A SEPARATE PASS AT ALL. The update pass cannot do this itself: its
// threads arrive at an entry in whatever order the hash sends them, several at
// once, so no one of them knows when the entry is finished. Resolve runs after
// the update has completed, exactly once per entry, which is the only place the
// "how many samples did this cell get this frame" question has an answer.
//
// THE TWO WINDOW PARAMETERS ARE THE WHOLE OF THE QUALITY TRADE:
//   accumulationFrameNum  how long the running average remembers. Longer is
//                         smoother and slower to notice the sun moving.
//   staleFrameNumMax      how long an entry survives with nothing written to
//                         it. Shorter frees the map faster and costs occupancy
//                         churn; SHARC_STALE_FRAME_NUM_MIN stops it going low
//                         enough to thrash.
// ---------------------------------------------------------------------------
#define SHARC_QUERY 1
#include "Sharc.slang"

[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const uint entry = tid.x;
    if (entry >= gSharcCapacity)
        return;

    SharcResolveParameters rp;
    // The PREVIOUS camera position, because the resolve is judging entries that
    // were written against last frame's grid: the hash level of a cell depends
    // on its distance from the camera, so handing it this frame's position
    // would have it measure cells against a grid they were never filed in.
    rp.cameraPositionPrev = gSharcCameraPosPrev;
    rp.accumulationFrameNum = gSharcAccumFrames;
    rp.staleFrameNumMax = gSharcStaleFrames;
    rp.frameIndex = gSharcFrame;

    SharcResolveEntry(entry, sharcParams(), rp);

    // Occupancy, counted AFTER the resolve so evicted entries are already
    // gone from the total rather than counted one last time on their way out.
    if (gSharcStatsOn != 0 && gSharcHashEntries[entry] != 0)
        InterlockedAdd(gSharcStats[0], 1u);
}
