// ---------------------------------------------------------------------------
// camera.h -- a thin-lens pinhole with a real aperture, flattened for the GPU.
//
// Depth of field is not decoration in a wood. A forest is visually a mess of
// high-frequency detail at every depth, and everything being equally sharp is
// what makes a render of one read as noise. A shallow focus picks out the stand
// the frame is actually about and lets the rest fall away -- and because this
// is a path tracer, it costs nothing but the lens sample.
//
// The only difference from v4's version is where the work happens. There the
// camera generated rays itself, once per pixel per sample, on the CPU. Here it
// produces a CameraGPU -- a basis and two half-extents -- and the raygen
// program does the per-pixel arithmetic. All the trigonometry is on this side,
// evaluated once a frame instead of two million times.
//
// A NOTE ON DEPTH OF FIELD IN THE VIEWER: it is deliberately off there. An
// aperture spreads each pixel's samples over the whole lens, which is exactly
// the correlation a temporal upscaler cannot reconstruct through -- the history
// it is blending was drawn through a different part of the lens. Stills get the
// aperture; the interactive path gets a pinhole.
// ---------------------------------------------------------------------------
#pragma once

#include "../optix/params.h"

namespace v2 {

class Camera {
  public:
    Vec3 origin{0.0f, 2.0f, 0.0f};
    Vec3 target{0.0f, 2.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float fovDeg = 42.0f;
    float aperture = 0.0f;   // lens diameter in metres; 0 is a pinhole
    float focusDist = 0.0f;  // metres; 0 means focus on the target

    CameraGPU gpu(int width, int height) const {
        CameraGPU c;
        c.pos = origin;
        c.w = normalize(target - origin);
        c.u = normalize(cross(c.w, up));
        c.v = cross(c.u, c.w);

        const float theta = fovDeg * PI / 180.0f;
        c.halfH = tanf(theta * 0.5f);
        c.halfW = c.halfH * float(width) / float(height);
        c.aperture = aperture;
        c.focusDist = focusDist > 0.0f ? focusDist : maxf(0.1f, length(target - origin));
        return c;
    }

    // The camera direction from yaw and pitch in degrees -- the viewer's
    // representation, kept here so the offline and interactive paths cannot
    // disagree about which way 0 degrees points.
    static Vec3 direction(float yawDeg, float pitchDeg) {
        const float y = yawDeg * PI / 180.0f, p = pitchDeg * PI / 180.0f;
        return normalize(Vec3(cosf(p) * sinf(y), sinf(p), -cosf(p) * cosf(y)));
    }
};

}  // namespace v2
