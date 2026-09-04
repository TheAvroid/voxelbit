// ---------------------------------------------------------------------------
// params.h -- the contract between the host and the GPU.
//
// This is the only header both the MinGW build and the nvcc build include for
// layout rather than for code, so every struct in it is a POD whose bytes mean
// the same thing on both sides. Nothing here has a constructor that does work,
// a virtual, or a pointer to anything but device memory.
//
// TWO CONVENTIONS WORTH STATING, because both are the kind that fail silently:
//
//   * A CUdeviceptr on the host is a plain `const T*` here. The two are the
//     same 64 bits, and spelling it as a pointer means the device code reads it
//     without a cast at every use.
// ---------------------------------------------------------------------------
#pragma once

// OptiX's headers deliberately do not pull in the CUDA vector types, so float2
// and float4 have to come from somewhere. Under nvcc that is vector_types.h.
// On the host it is NOT: CUDA's copy drags in crt/host_defines.h, which
// redefines __cdecl -- a macro MinGW supplies as a target builtin and its own
// <windows.h> then uses. The redefinition is harmless but unsilenceable, and
// undefining it would break every Win32 header downstream.
//
// So the host gets the three types spelled out. This is not a guess at the ABI:
// CUDA declares float2 as 8-byte aligned and float4 as 16-byte aligned, and
// those alignments are what the alignas below reproduce. They have to match
// exactly, because these structs are written by a kernel and read by the host.
#if defined(__CUDACC__)
#include <vector_types.h>
#else
struct alignas(8) float2 { float x, y; };
struct alignas(16) float4 { float x, y, z, w; };
struct alignas(4) uchar4 { unsigned char x, y, z, w; };
#endif

#include <optix.h>

#include "../core/vecmath.h"
#include "../scene/sky.h"
#include "../scene/voxelworld.h"

namespace v2 {

// One miss program per ray kind. The shadow ray reuses the radiance ray's
// hitgroup records -- it disables closest-hit entirely, so it never reads one --
// which is why the SBT needs no second set of them and the stride stays 1.
enum RayType : unsigned int { RAY_RADIANCE = 0, RAY_SHADOW = 1, RAY_TYPE_COUNT = 2 };

enum SurfaceKind : unsigned int { KIND_TERRAIN = 0, KIND_WATER = 1, KIND_TREE = 2 };

// ---------------------------------------------------------------------------
// One per geometry in the scene: the terrain, the water, and each of the nine
// pines. Reached through the SBT, so the instance's sbtOffset picks it.
// ---------------------------------------------------------------------------
struct HitGroupData {
    const unsigned short *tri;  // packTri(material, face), one per triangle
    unsigned int kind;          // SurfaceKind
    unsigned int pad;
};

// One per instance, indexed by optixGetInstanceId(). Terrain and water get an
// identity entry so the hit program needs no special case for them.
struct InstanceData {
    Vec3 tint{1.0f, 1.0f, 1.0f};
    float pad = 0.0f;
};

// ---------------------------------------------------------------------------
// The camera, as the raygen program wants it: a basis and two half-extents,
// with none of the trigonometry left to do per pixel.
//
// Kept as a struct rather than loose fields because the launch carries TWO of
// them -- this frame's and last frame's -- and the second exists only so a
// first-hit point can be projected back into the previous frame to make a
// motion vector. A static world plus a moving camera means that reprojection is
// exact; there is no need to store or interpolate anything per surface.
// ---------------------------------------------------------------------------
struct CameraGPU {
    Vec3 pos;
    Vec3 u;  // right
    Vec3 v;  // up
    Vec3 w;  // forward, unit
    float halfW, halfH;
    float aperture, focusDist;
};

struct LaunchParams {
    // -- film, all at render resolution -------------------------------------
    // The accumulators hold sums; .w carries the sample count so a resolve
    // never has to consult a second buffer to know what to divide by.
    float4 *accum;

    // The resolved mean, which is what the tone mapper reads.
    float4 *color;

    unsigned int width, height;

    // TWO COUNTERS, AND THEY MEAN DIFFERENT THINGS.
    //
    // `frame` is the ACCUMULATION index: how many samples are already in the
    // film for this camera. It returns to zero the instant the camera moves,
    // because every sample in the film was drawn for a view that no longer
    // exists.
    //
    // `tick` only ever increases. It seeds the sampler and picks the jitter,
    // and it must NOT reset with the accumulation -- a temporal denoiser and an
    // upscaler both reconstruct from the sample pattern MOVING between frames,
    // so freezing the jitter at Halton(0) while the camera flies hands them the
    // same sub-pixel offset every frame and nothing to reconstruct from.
    //
    // Conflating these two was a real bug: it left the upscaler doing 2x
    // reconstruction from a fixed sample grid with no history, which is exactly
    // the condition under which it looks worst.
    unsigned int frame;
    unsigned int tick;
    unsigned int seed;

    // Cap on how many samples the film averages, 0 for no cap.
    //
    // THIS IS WHAT A MOVING SUN NEEDS INSTEAD OF A RESET. With a day/night
    // cycle running, the lighting changes a little every single frame, so
    // "throw the accumulation away when the sun moves" fires roughly twice a
    // second -- and with the denoiser off that snaps a converged image back to
    // raw single-sample noise, over and over. It reads exactly like the screen
    // resetting, because that is what it is.
    //
    // Capping the sample count instead turns the film into an exponential
    // moving average: once it holds `maxAccum` samples each new one is worth
    // 1/maxAccum and the oldest fade out. The image tracks the sun continuously
    // and never restarts. It lags by roughly the window, which at 128 samples
    // and 60 fps is two seconds of a twenty-minute day -- invisible.
    unsigned int maxAccum;

    // -- camera --------------------------------------------------------------
    CameraGPU cam;
    Vec2 jitter;  // sub-pixel offset, in pixels, shared by the whole frame

    // -- integrator ----------------------------------------------------------
    int maxDepth;
    int rrStart;
    float clampIndirect;
    float fogDensity;  // extinction per metre at y = 0
    float fogHeight;   // e-folding height of the haze, metres

    // -- world ---------------------------------------------------------------
    SkyGPU sky;
    const MaterialLook *materials;
    const InstanceData *instances;
    OptixTraversableHandle handle;
};

// ---------------------------------------------------------------------------
// Projecting a world point into a camera, in pixels.
//
// The inverse of the raygen's ray construction, and it lives here so the two
// cannot drift: if the camera basis changes shape, both ends fail to compile
// together rather than one end quietly producing motion vectors for a camera
// the renderer no longer has.
//
// Returns false behind the camera, where the perspective divide flips sign and
// the answer would be a plausible-looking pixel in the wrong half of the frame.
// ---------------------------------------------------------------------------
V2_FN bool projectToPixel(const CameraGPU &c, Vec3 p, unsigned int width, unsigned int height,
                          Vec2 *out) {
    const Vec3 d = p - c.pos;
    const float zc = dot(d, c.w);
    if (zc <= 1e-4f) return false;
    const float sx = dot(d, c.u) / (zc * c.halfW);
    const float sy = dot(d, c.v) / (zc * c.halfH);
    *out = Vec2((sx + 1.0f) * 0.5f * float(width), (1.0f - sy) * 0.5f * float(height));
    return true;
}

}  // namespace v2
