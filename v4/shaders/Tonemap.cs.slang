// ---------------------------------------------------------------------------
// Tonemap.cs.slang -- linear HDR to the swapchain.
//
// The CPU engine in this lineage did this across a worker pool and measured
// 50 ms a frame at 1280x720 -- more than the path tracing it was displaying,
// because pow() per channel per pixel is a genuinely expensive thing to do a
// million times. It ended up behind an 8192-entry lookup table to make the
// viewer usable. On the GPU the honest version is free.
//
// The OUTPUT IS AN SRGB TARGET, so this writes LINEAR values and the hardware
// does the transfer function on the way out. v2 encoded sRGB by hand because it
// was writing bytes into a buffer it then blitted through OpenGL; here the
// swapchain format is BGRA8UnormSrgb and doing it in the shader as well would
// apply the curve twice and wash the frame out.
// ---------------------------------------------------------------------------

Texture2D<float4> gSrc;
RWTexture2D<float4> gDst;

cbuffer gTonemapCB
{
    uint2 gDim;
    float gExposure;
    float gPad;
};

// The ACES filmic approximation (Narkowicz). Reinhard desaturates highlights
// toward white, which on a sunlit canopy turns every specular hit into a grey
// blob; this curve keeps the hue into the shoulder, so a bright needle stays
// green and a sun glint on water stays warm.
float acesFilmic(float c)
{
    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    const float3 c = gSrc[tid.xy].rgb * gExposure;
    gDst[tid.xy] = float4(acesFilmic(c.r), acesFilmic(c.g), acesFilmic(c.b), 1.0f);
}
