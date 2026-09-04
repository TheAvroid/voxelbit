// ---------------------------------------------------------------------------
// tonemap.cu -- linear HDR to 8-bit sRGB, on the GPU.
//
// v4 did this on the CPU across its worker pool, and measured 50 ms a frame at
// 1280x720 -- more than the path tracing it was displaying, because pow() per
// channel per pixel is a genuinely expensive thing to do a million times. It
// ended up behind an 8192-entry lookup table to make the viewer usable.
//
// On the GPU the honest version is free. The image is already in device memory
// when the denoiser finishes, the curve is a handful of FLOPs, and doing it
// here means the only thing that ever crosses the bus is the finished 8-bit
// frame: a quarter of the bytes of the float4 buffer it came from.
//
// This is a plain CUDA kernel, not an OptiX program. It is compiled to a cubin
// and loaded through the driver API next to the OptiX pipeline, which is why
// v2 links no CUDA runtime at all -- cudart is an MSVC import library and the
// host half of this engine is built by MinGW.
// ---------------------------------------------------------------------------

// The ACES filmic approximation (Narkowicz). Reinhard desaturates highlights
// toward white, which on a sunlit canopy turns every specular hit into a grey
// blob; this curve keeps the hue into the shoulder, so a bright needle stays
// green and a sun glint on water stays warm.
__device__ __forceinline__ float acesFilmic(float c) {
    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    const float r = (c * (a * c + b)) / (c * (cc * c + d) + e);
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}

__device__ __forceinline__ unsigned char encodeSrgb(float c) {
    c = acesFilmic(c);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * __powf(c, 1.0f / 2.4f) - 0.055f;
    const float v = s * 255.0f + 0.5f;
    return (unsigned char)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

extern "C" __global__ void tonemapKernel(const float4 *src, uchar4 *dst, int width, int height,
                                         float exposure) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int i = y * width + x;
    const float4 c = src[i];
    dst[i] = make_uchar4(encodeSrgb(c.x * exposure), encodeSrgb(c.y * exposure),
                         encodeSrgb(c.z * exposure), 255);
}

// ---------------------------------------------------------------------------
// The same curve, but leaving the result in floats.
//
// Used only by the offline path, which wants to write a PNG on the host and so
// needs the pixels back anyway. Keeping it as a separate entry point rather
// than reading back the uchar4 buffer means --hdr can still write the linear
// PFM from the same launch.
// ---------------------------------------------------------------------------
extern "C" __global__ void copyLinearKernel(const float4 *src, float4 *dst, int width, int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int i = y * width + x;
    dst[i] = src[i];
}
