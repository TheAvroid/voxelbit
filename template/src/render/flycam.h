// ---------------------------------------------------------------------------
// flycam.h -- the camera controller, for an engine with nothing to stand on.
//
// This replaces a walker: gravity, a step height, a crouch, a body that could
// be blocked by geometry, and a spawn routine that searched for a clear column
// to put it in. Every one of those needed to ask the world a question -- is
// this voxel solid, how high is this column, what can I walk into near here --
// and this engine has no world to ask. So the camera flies, and the walker
// comes back with the first backend that can answer those questions on the
// HOST rather than only in a shader. See the note in gpu/world.h about the
// second interface.
//
// THE FIELDS ARE THE WALKER'S FIELDS ON PURPOSE. `fly`, `onGround`, `vy` and
// `halfWidth` are carried even though two of them are constant here, because
// the overlay, the HUD and the recorder already read them, and because the
// walker that comes back should drop into the same shape rather than force a
// second round of edits through app.h.
//
// MOTION IS EXPONENTIAL, NOT LINEAR. A fly camera in a renderer is used for two
// incompatible things -- crossing the scene, and easing up to a surface to look
// at one pixel of it -- and a single speed is wrong for both. Scroll multiplies
// rather than adds, so the same number of notches covers 0.2 m/s and 200 m/s
// and the control stays usable at either end.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>

#include "../core/vecmath.h"

namespace tpl {

class FlyCam {
  public:
    Vec3 pos{0.0f, 2.0f, 0.0f};

    // Metres from `pos` to the eye. Zero here -- there is no body for the eye
    // to sit on top of -- but kept so eyePosition() stays the one place the
    // question is answered, for the walker's return.
    float eye = 0.0f;

    float walk = 8.0f;   // metres per second at a normal press
    bool fly = true;     // always, in this engine
    bool onGround = false;
    float vy = 0.0f;
    float halfWidth = 0.3f;

    // How much faster a sprint is. Not a separate speed: a multiplier, so it
    // rides on top of whatever the scroll wheel has set.
    float sprintMul = 4.0f;

    Vec3 eyePosition() const { return Vec3(pos.x, pos.y + eye, pos.z); }

    float speed() const { return walk; }

    // `move` is the camera-relative direction the keys are asking for, already
    // built from the view basis by the caller -- x right, y unused, z forward.
    // jump/down are the vertical pair; crouch is accepted and ignored, because
    // there is nothing to crouch under.
    void update(Vec3 move, bool sprint, bool jump, bool down, bool crouch, float dt) {
        (void)crouch;
        const float v = walk * (sprint ? sprintMul : 1.0f);

        // NORMALISED, so a diagonal is not faster than a straight line. The
        // guard matters: normalising a zero vector is a division by zero that
        // reaches the camera as a NaN position, and once the position is NaN
        // every ray is NaN and the frame goes black with nothing in the log.
        const float len = std::sqrt(move.x * move.x + move.z * move.z);
        if (len > 1e-6f) {
            pos.x += (move.x / len) * v * dt;
            pos.z += (move.z / len) * v * dt;
        }

        if (jump) pos.y += v * dt;
        if (down) pos.y -= v * dt;

        // There is no ground, so there is no falling.
        vy = 0.0f;
        onGround = false;
    }

    // Put the camera somewhere. Named for what it does rather than for the
    // walker's placeOnGround, which searched downward for a surface: there is
    // no surface, and a name that implies one would be the kind of lie that
    // survives into a backend and is then hard to find.
    void warpTo(float x, float z) { pos = Vec3(x, pos.y, z); }

    // One scroll notch. Multiplicative -- see the note at the top.
    void nudgeSpeed(float notches) {
        walk = std::clamp(walk * std::pow(1.25f, notches), 0.05f, 500.0f);
    }
};

}  // namespace tpl
