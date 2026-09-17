// ---------------------------------------------------------------------------
// CloudFill.cs.slang -- evaluate the cloud node graph once per texel.
//
// This is the pass that makes the deck affordable. The graph in Clouds.slang
// visits 27 neighbour cells per Voronoi octave and runs several of them; the
// march cannot pay that per step, so it is paid HERE, once per texel, and the
// march becomes a single texture fetch.
//
// ---------------------------------------------------------------------------
// FILLED A BAND OF SLICES AT A TIME, THEN STOPPED.
//
// Doing the whole volume in one dispatch is a visible hitch at startup -- two
// million texels of fractal Voronoi. Doing a band per frame spreads it over a
// handful of frames instead, and because the graph has NO evolution clock (the
// deck drifts by a lookup offset in the march, not by refilling) the fill is
// IDEMPOTENT: once every slice has been written the pass has nothing left to
// do and the host stops dispatching it entirely.
//
// That is also why there is one volume here and not the source's ping-pong
// pair. The pair exists to cross-fade a rebuild; with nothing ever rebuilding
// there is no seam to hide, and it saves both the memory and -- since a march
// step would have to read both and blend -- the texture traffic on the hottest
// path in the renderer.
// ---------------------------------------------------------------------------
#include "Clouds.slang"

RWTexture3D<float> gCloudVol;

cbuffer CloudFillCB {
    uint3 gDim;         // the volume's resolution
    uint gSliceBegin;   // first y slice this dispatch owns
    uint gSliceCount;   // how many of them
    // The two knobs worth turning without a rebuild. Changing either means the
    // cache no longer describes the sky it was filled for, so the host has to
    // refill it -- see Clouds::rebuild.
    float gCut;         // how much of the sky is cloud
    float gRegVar;      // how far coverage varies from region to region
    uint gPad0;
};

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const uint y = gSliceBegin + tid.z;
    if (tid.x >= gDim.x || tid.y >= gDim.z || tid.z >= gSliceCount || y >= gDim.y)
        return;

    // Texel centres. x and z run across one TILE of the endless deck, y across
    // the depth of the slab.
    const float u = (float(tid.x) + 0.5f) / float(gDim.x);
    const float w = (float(tid.y) + 0.5f) / float(gDim.z);
    const float zn = (float(y) + 0.5f) / float(gDim.y);

    // Tile space: xz over the noise period, y over the deck's height expressed
    // in the SAME base noise units. Keeping that honest is what stops the
    // clouds being stretched or squashed against their own horizontal shape.
    const float CG_YSPAN = (CLOUD_HI - CLOUD_LO) / CG_TILE * float(CG_PER);
    const float3 p = float3(u * float(CG_PER), zn * CG_YSPAN, w * float(CG_PER));

    gCloudVol[uint3(tid.x, y, tid.y)] = cgDensity(p, zn, gCut, gRegVar);
}
