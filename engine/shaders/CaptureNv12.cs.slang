// ---------------------------------------------------------------------------
// CaptureNv12.cs.slang -- the recorder's colour conversion, on the GPU.
//
// WHY THIS EXISTS AT ALL: the obvious way to record is to read the display
// texture back and convert on the host. That texture is RGBA32Float, so a
// 1080p frame is 33 MB and a 60 fps take is 2 GB/s over PCIe -- which the bus
// can do, but only by spending the whole frame doing it. NV12 is 1.5 bytes a
// pixel, so the same frame is 3.1 MB and the take is 186 MB/s: eleven times
// less, and it is the format the encoder wants anyway, so the conversion is
// not an extra step but the one that was going to happen regardless.
//
// THE OUTPUT IS A BUFFER, NOT A TEXTURE, and deliberately. A read-back texture
// carries a driver-chosen row pitch that has to be un-strided on the host; a
// buffer is exactly the bytes Media Foundation wants, in the order it wants
// them, and the copy into the encoder's sample is a single memcpy.
//
// THE TRANSFER FUNCTION IS DONE HERE. The display texture holds LINEAR values
// -- the swapchain is BGRA8UnormSrgb and the hardware applies the curve on the
// way out (see Tonemap.cs.slang). A recording never goes through that
// swapchain, so if this shader wrote the linear values straight into Y the
// video would come out crushed and dark. Same encodeSrgb the screenshot path
// applies on the host, done once per pixel here instead.
// ---------------------------------------------------------------------------

Texture2D<float4> gSrc;
SamplerState gSamp;
RWByteAddressBuffer gDst;

cbuffer gCaptureCB
{
    // The NV12 frame. Width is a multiple of 4 and height a multiple of 2 by
    // construction -- see the alignment note over the stores below.
    uint2 gOutDim;
    // The sub-rectangle of gSrc that is a picture, in texels. The display
    // texture is allocated at the OUTPUT size and the tone map may have filled
    // only the traced part of it, so this is not the texture's size.
    float2 gSrcDim;
    // One over the source texture's ALLOCATED size, for texel -> uv. Not one
    // over gSrcDim: the picture is a sub-rectangle of a bigger surface.
    float2 gInvSrcTex;
    // Source pixels per output pixel, and how many taps to average across.
    float2 gStep;
    uint2 gTaps;
    // 1 when gSrc already holds sRGB-encoded values (the export path reads
    // decoded video frames, which are 8-bit and already curved); 0 for the
    // live path, whose source is linear. Everything in between is averaged in
    // LINEAR light either way, which is the whole reason this is a flag rather
    // than two shaders.
    uint gDecodeSrgb;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

// The exact sRGB piecewise curve, both directions. Not a 2.2 power: the toe
// matters here because a video recorder is judged on its shadows, and this
// engine has spent a lot of effort on exactly those.
float srgbToLinear(float c)
{
    return c <= 0.04045f ? c * (1.0f / 12.92f) : pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

float linearToSrgb(float c)
{
    c = saturate(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

// One output pixel, box-averaged in linear light, returned as R'G'B' -- that
// is, sRGB-ENCODED. The BT.709 matrix below is defined on gamma-encoded
// components, so encoding has to happen before the matrix and not after.
float3 fetchGamma(uint2 outPix)
{
    const uint tx = max(1u, min(gTaps.x, 8u));
    const uint ty = max(1u, min(gTaps.y, 8u));
    const float2 origin = float2(outPix) * gStep;

    float3 lin = float3(0.0f);
    for (uint j = 0; j < ty; ++j)
    {
        for (uint i = 0; i < tx; ++i)
        {
            // Tap centres spread evenly across the source rect this output
            // pixel covers. With gStep == 1 and one tap this lands exactly on
            // the texel centre, so a same-size capture is a straight copy and
            // the sampler never interpolates.
            float2 p = origin + float2((float(i) + 0.5f) / float(tx), (float(j) + 0.5f) / float(ty)) * gStep;
            // Clamped to the PICTURE, not to the texture: past gSrcDim the
            // surface holds whatever the last larger frame left there.
            p = min(p, gSrcDim - 0.5f);
            float3 c = gSrc.SampleLevel(gSamp, p * gInvSrcTex, 0).rgb;
            if (gDecodeSrgb != 0)
                c = float3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
            lin += c;
        }
    }
    lin *= 1.0f / float(tx * ty);
    return float3(linearToSrgb(lin.r), linearToSrgb(lin.g), linearToSrgb(lin.b));
}

// BT.709, LIMITED (video) range -- 16..235 luma, 16..240 chroma.
//
// Full range would keep eight more codes of contrast, and it is the wrong
// choice: an H.264 elementary stream that says nothing is assumed limited by
// every player there is, and a full-range picture played as limited comes out
// with crushed blacks and clipped highlights. The media type is tagged to
// match (MF_MT_VIDEO_NOMINAL_RANGE), so this is stated twice rather than
// hoped for once.
float lumaY(float3 g)
{
    return 0.2126f * g.r + 0.7152f * g.g + 0.0722f * g.b;
}

uint quant(float v, float lo, float span)
{
    return uint(clamp(lo + span * v + 0.5f, 0.0f, 255.0f));
}

// ---------------------------------------------------------------------------
// One thread writes a 4 x 2 pixel block, and that shape is not arbitrary.
//
// A ByteAddressBuffer store must be 4-byte aligned. Four horizontal luma
// samples are exactly one aligned uint per row when the frame width is a
// multiple of 4; the same block is two chroma samples, which is one more
// aligned uint in the interleaved UV plane at the same x offset. So the whole
// block is three aligned stores and no read-modify-write anywhere -- which is
// why the capture size is rounded to a multiple of 4 by 2 on the host rather
// than passed through as-is.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const uint2 blocks = uint2(gOutDim.x >> 2, gOutDim.y >> 1);
    if (tid.x >= blocks.x || tid.y >= blocks.y) return;

    const uint x0 = tid.x << 2;
    const uint y0 = tid.y << 1;
    const uint w = gOutDim.x;

    float3 g[2][4];
    for (uint j = 0; j < 2; ++j)
        for (uint i = 0; i < 4; ++i) g[j][i] = fetchGamma(uint2(x0 + i, y0 + j));

    uint yRow[2];
    for (uint j = 0; j < 2; ++j)
    {
        uint packed = 0;
        for (uint i = 0; i < 4; ++i) packed |= quant(lumaY(g[j][i]), 16.0f, 219.0f) << (i * 8);
        yRow[j] = packed;
    }
    gDst.Store(y0 * w + x0, yRow[0]);
    gDst.Store((y0 + 1) * w + x0, yRow[1]);

    // Chroma is the 2x2 average, which is the standard box siting for 4:2:0 --
    // co-sited horizontally, centred vertically. Averaged on R'G'B' rather
    // than on Cb/Cr because the two are equivalent (the matrix is linear) and
    // this way is two adds instead of eight.
    uint uv = 0;
    for (uint k = 0; k < 2; ++k)
    {
        const float3 a = 0.25f * (g[0][k * 2] + g[0][k * 2 + 1] + g[1][k * 2] + g[1][k * 2 + 1]);
        const float yv = lumaY(a);
        const uint cb = quant((a.b - yv) * (1.0f / 1.8556f), 128.0f, 224.0f);
        const uint cr = quant((a.r - yv) * (1.0f / 1.5748f), 128.0f, 224.0f);
        uv |= cb << (k * 16);
        uv |= cr << (k * 16 + 8);
    }
    gDst.Store(gOutDim.x * gOutDim.y + tid.y * w + x0, uv);
}
