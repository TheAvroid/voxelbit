// ---------------------------------------------------------------------------
// viewer.h -- walking around inside the render.
//
// The bargain is v4's, and it survives both the move to the GPU and the removal
// of the denoiser:
//
//   MOVING   one sample per pixel and a short path. Noisy, but the frame rate
//            is high enough that the noise is moving too, and you can navigate
//            by it.
//   STILL    keep the accumulation, add a sample per frame, let the path run to
//            full length. The image converges under you while you stand still.
//
// The accumulation is a device buffer and clearing it is a flag on the next
// launch, so the moving/still trade costs nothing to switch and is purely the
// path length.
//
// CONTROLS follow v3 and v4, deliberately, so the muscle memory carries between
// the engines. ESC always takes two presses -- the first gives the mouse back,
// the second quits -- because a rule that is only sometimes true is worse than
// one that is never true.
// ---------------------------------------------------------------------------
#pragma once

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../core/defaults.h"
#include "../scene/daynight.h"
#include "../core/image.h"
#include "../gpu/renderer.h"
#include "camera.h"
#include "menu.h"
#include "player.h"
#include "overlay.h"

// Windows ships an OpenGL 1.1 gl.h and nothing newer, so the 1.2 clamp mode has
// no declaration there even though every driver written this century supports
// it. Declaring the enum is enough -- glTexParameteri takes it as a plain int,
// so no function pointer has to be resolved.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace v2 {

struct ViewerOptions {
    int width = 1280;
    int height = 720;
    float scale = 1.0f;  // output resolution as a fraction of the window
    bool stats = false;
    float sunAz = 38.0f, sunEl = 24.0f;
    float timeOfDay = 7.0f / 24.0f;  // [0,1)
    float cycleSpeed = 1.0f;
    float speed = 9.2f;   // walk, m/s -- the JS engine's 46 voxels/s, doubled
    float eye = 1.80f;    // 18 voxels
    int movingDepth = 4;  // path length while moving
    int trees = 900;      // carried only so a bake can record it
};

class Viewer {
  public:
    Viewer(GpuScene &scene, Renderer &renderer, const RenderSettings &base,
           const ViewerOptions &opt)
        : scene_(scene), rend_(renderer), base_(base), opt_(opt) {}

    int run(Vec3 startPos, float startYaw, float startPitch, float fovDeg) {
        player_.walk = opt_.speed;
        player_.eye = opt_.eye;
        player_.placeOnGround(scene_.terrain, startPos.x, startPos.z);
        pos_ = player_.eyePosition();
        yaw_ = startYaw;
        pitch_ = startPitch;
        fov_ = fovDeg;
        sunAz_ = opt_.sunAz;
        sunEl_ = opt_.sunEl;
        clock_.tday = opt_.timeOfDay;
        clock_.cycleSpeed = opt_.cycleSpeed;
        clock_.azimuthBase = opt_.sunAz;
        applySun(true);

        if (!glfwInit()) {
            std::fprintf(stderr, "v2: glfwInit failed\n");
            return 1;
        }
        // No core-profile request: the GPU's entire job in this context is to
        // blit one texture, and the fixed pipeline does that with no extension
        // loader and no shader compilation to go wrong on someone else's driver.
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
        // Windows will otherwise restore whatever size this window class was
        // last left at, which silently ignores --width and --height and makes
        // two runs incomparable without either of them looking wrong.
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE);
        window_ = glfwCreateWindow(opt_.width, opt_.height, "v2 -- pine forest (OptiX)", nullptr,
                                   nullptr);
        if (!window_) {
            std::fprintf(stderr, "v2: could not open a window\n");
            glfwTerminate();
            return 1;
        }
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(0);  // the tracer is the limit, not the display

        // Launched from a .bat, the CONSOLE takes the foreground and the new
        // window does not. Every key then goes to the console and the controls
        // look dead. Ask for focus explicitly.
        glfwShowWindow(window_);
        glfwFocusWindow(window_);

        // ...and then insist on the size that was asked for. Windows restores a
        // remembered placement for a window class it has seen before, and the
        // GLFW_MAXIMIZED hint does not override it: --width and --height were
        // being silently ignored on any run after one where the window had been
        // maximised. That is worse than it sounds, because --scale is a
        // FRACTION of the framebuffer -- so the render resolution was wrong by
        // whatever factor the restored window happened to be off by, and
        // nothing on screen said so.
        glfwRestoreWindow(window_);
        glfwSetWindowSize(window_, opt_.width, opt_.height);

        glfwSetWindowUserPointer(window_, this);
        glfwSetKeyCallback(window_, onKey);
        glfwSetMouseButtonCallback(window_, onMouseButton);
        glfwSetCursorPosCallback(window_, onCursorPos);
        glfwSetScrollCallback(window_, onScroll);
        glfwSetFramebufferSizeCallback(window_, onResize);

        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &menuTex_);
        glBindTexture(GL_TEXTURE_2D, menuTex_);
        // NEAREST, and the quad is drawn at exactly one texel per pixel: the
        // whole reason the panel is its own texture is that the frame behind it
        // may be a quarter resolution, and text must not inherit that.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &hudTex_);
        glBindTexture(GL_TEXTURE_2D, hudTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        MenuTarget mt;
        mt.scale = &opt_.scale;
        mt.depth = &base_.maxDepth;
        mt.movingDepth = &opt_.movingDepth;
        mt.exposure = &base_.exposure;
        mt.speed = &player_.walk;
        mt.fov = &fov_;
        mt.clock = &clock_;
        menu_.bind(mt);

        glfwGetFramebufferSize(window_, &fbW_, &fbH_);
        printHelp();

        double last = glfwGetTime();
        double fpsAccum = 0.0, statAccum = 0.0;
        int fpsFrames = 0;

        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();
            // Queried every frame, not just from the resize callback: at
            // startup no callback has fired yet, and a minimised window reports
            // zero -- which used to clamp the render to the 8 px floor.
            glfwGetFramebufferSize(window_, &fbW_, &fbH_);
            if (fbW_ <= 0 || fbH_ <= 0) {
                glfwWaitEventsTimeout(0.1);  // minimised: do not burn a core on it
                continue;
            }
            // Windows applies its remembered placement on the first message
            // pump, i.e. AFTER the restore and resize done before this loop --
            // so on the first launch of a freshly built exe the window came up
            // maximised anyway. Re-assert the requested size for a few frames,
            // then stop, so a deliberate resize by the user still works.
            if (forceSize_ > 0) {
                --forceSize_;
                if (fbW_ != opt_.width || fbH_ != opt_.height) {
                    glfwRestoreWindow(window_);
                    glfwSetWindowSize(window_, opt_.width, opt_.height);
                    glfwGetFramebufferSize(window_, &fbW_, &fbH_);
                }
            }
            const double now = glfwGetTime();
            // Clamped so one slow frame cannot teleport the camera, but not so
            // tightly that movement stalls when the tracer is having a bad time.
            const float dt = float(now - last < 0.25 ? now - last : 0.25);
            last = now;

            // Accumulation only. The temporal history is carried across
            // movement on the motion vectors, which is what they are for.
            clock_.advance(dt);
            applySun(false);

            // Stream the world around the camera. A changed ring means new
            // geometry in frame, so what has been accumulated for the old one
            // no longer describes it.
            if (scene_.update(pos_)) rend_.resetAccumulation();
            if (processInput(dt)) rend_.resetAccumulation();
            renderFrame();
            present();
            glfwSwapBuffers(window_);

            fpsAccum += dt;
            statAccum += dt;
            ++fpsFrames;
            if (fpsAccum >= 0.5) {
                const double fps = fpsFrames / maxf(1e-4f, float(fpsAccum));
                fps_ = float(fps);
                clock_.clock(clockText_, sizeof(clockText_));
                char title[256];
                std::snprintf(title, sizeof(title),
                              "v2 -- pine forest  |  %dx%d  %u spp  %.1f fps%s",
                              rend_.outWidth(), rend_.outHeight(), rend_.samples(), fps,
                              looking_ ? "  [mouse captured]" : "");
                glfwSetWindowTitle(window_, title);
                if (opt_.stats && statAccum >= 2.0) {
                    std::printf(
                        "  %.1f fps  trace %5.2f  resolve %5.2f  blit %4.2f ms  %dx%d  "
                        "%d bounces  %s %.1f m/s  cam (%.1f %.1f %.1f)\n",
                        fps, rend_.traceMs(), rend_.resolveMs(), msBlit_, rend_.renderWidth(),
                        rend_.renderHeight(), lastDepth_,
                        player_.fly ? "fly" : (player_.onGround ? "ground" : "air"),
                        player_.speed(), pos_.x, pos_.y, pos_.z);
                    std::fflush(stdout);
                    statAccum = 0.0;
                }
                fpsAccum = 0.0;
                fpsFrames = 0;
            }
        }

        glfwDestroyWindow(window_);
        glfwTerminate();
        return 0;
    }

  private:
    GpuScene &scene_;
    Renderer &rend_;
    RenderSettings base_;
    ViewerOptions opt_;

    GLFWwindow *window_ = nullptr;
    unsigned tex_ = 0;
    int fbW_ = 1280, fbH_ = 720;

    Player player_;
    Vec3 pos_{0, 2, 0};  // the EYE, derived from the player every frame
    float yaw_ = 0.0f, pitch_ = 0.0f, fov_ = 50.0f;
    float sunAz_ = 38.0f, sunEl_ = 24.0f;
    DayNight clock_;  // owns the sun; sunAz_/sunEl_ are its output

    bool looking_ = false;   // cursor captured, mouse turns the camera
    bool holdLook_ = false;  // ...because the right button is held, v3 style
    bool quitArmed_ = false;
    bool moving_ = false;
    bool firstMouse_ = true;
    double lastX_ = 0.0, lastY_ = 0.0;

    std::vector<unsigned char> rgba_;
    int shotIndex_ = 0;
    float msBlit_ = 0.0f;
    static constexpr int kMenuX = 28, kMenuY = 24;  // panel origin, framebuffer px
    static constexpr int kHudX = 28, kHudY = 20;

    TextPanel hud_;
    unsigned hudTex_ = 0;
    int hudFont_ = 0;
    bool hudDirty_ = true;
    char hudText_[160] = {0};
    char clockText_[16] = {0};

    SettingsMenu menu_;
    TextPanel panel_;
    bool captureBeforeMenu_ = false;
    int mouseX_ = -1000, mouseY_ = -1000;  // cursor, in panel pixels
    unsigned menuTex_ = 0;
    int panelFont_ = 0;
    float fps_ = 0.0f;
    int forceSize_ = 8;  // frames left in which to re-assert --width/--height
    int lastDepth_ = 0;

    static Viewer *self(GLFWwindow *w) { return (Viewer *)glfwGetWindowUserPointer(w); }

    static double nowMs() {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    void printHelp() const {
        std::printf(
            "\ncontrols:\n"
            "  click the window first  -- it needs focus, the console steals it on launch\n"
            "\n"
            "  left click            capture the mouse and look freely\n"
            "  right-drag            look around without capturing\n"
            "  W A S D               walk (hold shift to sprint)\n"
            "  space                 jump\n"
            "  F                     toggle fly mode\n"
            "  scroll                zoom (field of view)\n"
            "  arrow keys            scrub time (up/down = fast)\n"
            "  X + scroll wheel      day/night speed -- scroll down past 0.25x to REWIND\n"
            "  Y                     SETTINGS MENU -- frees the mouse; click to change\n"


            "  - / =                 exposure down / up\n"
            "  [ / ]                 bounces down / up\n"
            "  P                     screenshot            F1   this help\n"
            "  ESC                   release the mouse; ESC again quits\n\n");
    }

    Vec3 forward() const { return Camera::direction(yaw_, pitch_); }
    Vec3 rightVec() const { return normalize(cross(forward(), Vec3(0, 1, 0))); }

    // -----------------------------------------------------------------------
    // Opening the menu hands the mouse back, and closing it takes it again if
    // it had it. A menu you can see but not point at is worse than no menu, and
    // a cursor that stays hidden while a panel is asking to be clicked reads as
    // the window having locked up.
    // -----------------------------------------------------------------------
    void setMenuOpen(bool on) {
        if (on == menu_.open) return;
        if (on) {
            captureBeforeMenu_ = looking_;
            if (looking_) setCapture(false);
            holdLook_ = false;
            menu_.open = true;
        } else {
            menu_.open = false;
            if (captureBeforeMenu_) setCapture(true);
            captureBeforeMenu_ = false;
        }
        std::printf("v2: settings menu %s\n", menu_.open ? "open" : "closed");
        std::fflush(stdout);
    }

    // Where the cursor is inside the panel, in the panel's own pixels.
    //
    // GLFW reports the cursor in SCREEN coordinates while the panel is drawn in
    // FRAMEBUFFER pixels, and on a scaled display those differ -- so the ratio
    // has to be applied or every click lands short on a high-DPI monitor.
    bool cursorInPanel(double cx, double cy, int *px, int *py) const {
        int winW = 1, winH = 1;
        glfwGetWindowSize(window_, &winW, &winH);
        if (winW <= 0 || winH <= 0) return false;
        const double sx = double(fbW_) / double(winW), sy = double(fbH_) / double(winH);
        *px = int(cx * sx) - kMenuX;
        *py = int(cy * sy) - (kMenuY + hud_.height() + 10);
        return true;
    }

    // -----------------------------------------------------------------------
    // Push the clock's sun into the sky, and say whether it moved enough to
    // matter.
    //
    // THE THRESHOLD IS THE POINT. At 1x the sun sweeps 0.3 degrees a second,
    // so every single frame moves it a little and a naive "did it change?"
    // would throw the accumulation away sixty times a second and never let a
    // still frame converge at all. A quarter of a degree is far below what is
    // visible in a frame and still lets a second's worth of samples pile up
    // between resets.
    // -----------------------------------------------------------------------
    bool applySun(bool force) {
        const float el = clock_.elevationDeg();
        const float az = clock_.azimuthDeg();
        // Refitting Preetham is a few dozen flops, but it is host work on the
        // critical path, so it is still worth skipping while nothing has moved.
        const float moved = fabsf(el - sunEl_) + fabsf(az - sunAz_);
        if (!force && moved < 0.02f) return false;
        sunEl_ = el;
        sunAz_ = az;
        scene_.sky.setSun(az, el);
        return true;
    }

    void setCapture(bool on) {
        looking_ = on;
        firstMouse_ = true;
        glfwSetInputMode(window_, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

    // -----------------------------------------------------------------------
    // Input
    // -----------------------------------------------------------------------
    static void onKey(GLFWwindow *w, int key, int, int action, int) {
        if (action != GLFW_PRESS) return;
        Viewer *v = self(w);

        if (key == GLFW_KEY_X) return;  // held modifier for the wheel; see onScroll
        if (key == GLFW_KEY_F) {
            v->player_.fly = !v->player_.fly;
            if (!v->player_.fly) v->player_.vy = 0.0f;  // do not inherit a climb as a fall
            std::printf("v2: %s\n", v->player_.fly ? "flying" : "walking");
            std::fflush(stdout);
            v->quitArmed_ = false;
            return;
        }
        if (key == GLFW_KEY_Y) {
            v->setMenuOpen(!v->menu_.open);
            v->quitArmed_ = false;
            return;
        }
        // The menu gets first refusal on every key while it is open, so the
        // arrows choose a row instead of moving the sun and the digits pick a
        // preset instead of doing nothing.
        if (v->menu_.key(key)) {
            v->quitArmed_ = false;
            return;
        }
        // ESC closes the menu before it starts arming the quit -- otherwise
        // dismissing a panel would leave the window one keypress from closing.
        if (key == GLFW_KEY_ESCAPE && v->menu_.open) {
            v->setMenuOpen(false);
            return;
        }

        if (key == GLFW_KEY_ESCAPE) {
            // First press hands the mouse back, second press quits. Whether the
            // cursor was captured or not, it always takes two.
            if (v->looking_) v->setCapture(false);
            v->holdLook_ = false;
            if (v->quitArmed_) {
                glfwSetWindowShouldClose(w, GLFW_TRUE);
            } else {
                v->quitArmed_ = true;
                std::printf("v2: press ESC again to quit\n");
                std::fflush(stdout);
            }
            return;
        }

        // Any other key means the session is still in use, so disarm. Otherwise
        // an ESC pressed minutes ago is still primed to close the window.
        v->quitArmed_ = false;

        if (key == GLFW_KEY_MINUS) v->base_.exposure = maxf(0.05f, v->base_.exposure * 0.8f);
        if (key == GLFW_KEY_EQUAL) v->base_.exposure = minf(40.0f, v->base_.exposure * 1.25f);
        if (key == GLFW_KEY_LEFT_BRACKET) {
            v->base_.maxDepth = maxi(1, v->base_.maxDepth - 1);
            std::printf("v2: bounces = %d\n", v->base_.maxDepth);
            v->rend_.resetAccumulation();
        }
        if (key == GLFW_KEY_RIGHT_BRACKET) {
            v->base_.maxDepth = mini(32, v->base_.maxDepth + 1);
            std::printf("v2: bounces = %d\n", v->base_.maxDepth);
            v->rend_.resetAccumulation();
        }
        if (key == GLFW_KEY_P) v->screenshot();
        if (key == GLFW_KEY_F1) v->printHelp();
        std::fflush(stdout);
    }

    static void onMouseButton(GLFWwindow *w, int button, int action, int) {
        Viewer *v = self(w);
        // Taking hold of the mouse means the session is in use, so an ESC
        // pressed earlier was not the first half of a quit.
        if (action == GLFW_PRESS) v->quitArmed_ = false;

        if (v->menu_.open) {
            if (action != GLFW_PRESS) return;
            // The position tracked by onCursorPos, NOT a fresh glfwGetCursorPos.
            // The two are the same for a real mouse, but only the tracked one is
            // guaranteed to be the position that produced the highlight now on
            // screen -- so a click always acts on the row the user can see is
            // selected, which is the only behaviour worth guaranteeing here.
            if (v->menu_.click(v->panel_, v->mouseX_, v->mouseY_)) return;
            // Clicking off the panel dismisses it, the way a dropdown does --
            // and deliberately does NOT re-capture the mouse, because a click
            // meant to close something should not also swallow the cursor.
            v->setMenuOpen(false);
            v->captureBeforeMenu_ = false;
            return;
        }

        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
            // Click to capture, the way a game does it. ESC gives it back.
            if (!v->looking_) v->setCapture(true);
            return;
        }
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            // v3's hold-to-look, kept so the habit carries over.
            if (action == GLFW_PRESS) {
                v->holdLook_ = true;
                if (!v->looking_) v->setCapture(true);
            } else if (action == GLFW_RELEASE && v->holdLook_) {
                v->holdLook_ = false;
                v->setCapture(false);
            }
        }
    }

    static void onCursorPos(GLFWwindow *w, double x, double y) {
        Viewer *v = self(w);
        if (v->menu_.open) {
            int px = 0, py = 0;
            if (v->cursorInPanel(x, y, &px, &py)) {
                v->mouseX_ = px;
                v->mouseY_ = py;
                v->menu_.hover(v->panel_, px, py);
            }
            return;  // the mouse belongs to the menu while it is up
        }
        if (!v->looking_) return;
        // The first event after a capture carries the jump from wherever the
        // cursor was to the centre; using it would spin the camera.
        if (v->firstMouse_) {
            v->lastX_ = x;
            v->lastY_ = y;
            v->firstMouse_ = false;
            return;
        }
        constexpr float sensitivity = 0.12f;
        const float dx = float(x - v->lastX_) * sensitivity;
        const float dy = float(v->lastY_ - y) * sensitivity;
        v->lastX_ = x;
        v->lastY_ = y;
        if (dx != 0.0f || dy != 0.0f) {
            // Wrapped rather than left to grow: a long session spinning one way
            // otherwise walks yaw into the thousands, where a float's steps get
            // coarse enough to make the turn visibly notchy.
            v->yaw_ = fmodf(v->yaw_ + dx, 360.0f);
            if (v->yaw_ < 0.0f) v->yaw_ += 360.0f;
            v->pitch_ = clampf(v->pitch_ + dy, -89.0f, 89.0f);
            v->moving_ = true;
            v->rend_.resetAccumulation();
        }
    }

    static void onScroll(GLFWwindow *w, double, double dy) {
        Viewer *v = self(w);

        // X IS A HELD MODIFIER, and it has to be POLLED rather than tracked
        // from key events: a stuck flag after an alt-tab that swallowed the
        // keyup would silently turn every later scroll into a time change.
        // glfwGetKey has no such failure mode -- it reports the key's state
        // now, and a window without focus reports it released.
        if (dy != 0.0 && glfwGetKey(w, GLFW_KEY_X) == GLFW_PRESS) {
            v->clock_.nudgeSpeed(dy > 0.0);
            char lbl[24];
            v->clock_.speedLabel(lbl, sizeof(lbl));
            std::printf("v2: day/night %s\n", lbl);
            std::fflush(stdout);
            return;  // never zoom while X is down
        }

        if (v->menu_.open) {
            if (dy != 0.0 && v->menu_.wheel(v->panel_, v->mouseX_, v->mouseY_, dy > 0 ? 1 : -1))
                return;
            return;  // never zoom the camera from under an open menu
        }
        const float f = clampf(v->fov_ - float(dy) * 2.0f, 10.0f, 100.0f);
        if (f != v->fov_) {
            v->fov_ = f;
            v->rend_.resetAccumulation();
        }
    }

    static void onResize(GLFWwindow *w, int width, int height) {
        Viewer *v = self(w);
        v->fbW_ = width > 0 ? width : 1;
        v->fbH_ = height > 0 ? height : 1;
        v->rend_.resetHistory();
    }

    bool processInput(float dt) {
        if (glfwGetWindowAttrib(window_, GLFW_FOCUSED) == 0) {
            moving_ = false;
            return false;
        }

        // The arrows scrub the CLOCK, not the sun directly: with a cycle
        // running, a manual elevation would be overwritten on the next frame
        // and the control would look broken.
        if (!menu_.open) {
            const float scrub = 1.5f * dt;  // hours per second held
            if (glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_PRESS) clock_.scrubHours(-scrub);
            if (glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_PRESS) clock_.scrubHours(scrub);
            if (glfwGetKey(window_, GLFW_KEY_UP) == GLFW_PRESS) clock_.scrubHours(scrub * 6.0f);
            if (glfwGetKey(window_, GLFW_KEY_DOWN) == GLFW_PRESS)
                clock_.scrubHours(-scrub * 6.0f);
        }

        const bool sprint = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                            glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        const bool jump = glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS;
        const bool down = glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                          glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS;

        // WASD in the horizontal plane only -- looking up must not walk you into
        // the sky. The forward vector is flattened and renormalised rather than
        // used directly, or a steep pitch would shorten every stride.
        const Vec3 f = forward();
        Vec3 flat(f.x, 0.0f, f.z);
        flat = (lengthSq(flat) > 1e-6f) ? normalize(flat) : Vec3(0.0f, 0.0f, -1.0f);
        const Vec3 r = normalize(cross(flat, Vec3(0, 1, 0)));

        Vec3 move(0.0f, 0.0f, 0.0f);
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) move += flat;
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) move -= flat;
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) move -= r;
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) move += r;
        if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS && player_.fly) move += Vec3(0, 0, 0);
        if (lengthSq(move) > 1e-6f) move = normalize(move);

        const Vec3 before = player_.eyePosition();
        player_.update(scene_.terrain, move, sprint, jump, down, dt);
        pos_ = player_.eyePosition();

        // The BOB counts as movement. It shifts the eye every frame while
        // walking, so the accumulated samples describe a viewpoint that no
        // longer exists -- exactly as if the camera had been flown.
        const bool camMoved = lengthSq(pos_ - before) > 1e-10f;
        moving_ = moving_ || camMoved;
        return camMoved;
    }

    // -----------------------------------------------------------------------
    // One frame
    // -----------------------------------------------------------------------
    void renderFrame() {
        // An edit that changes what gets traced makes the accumulation stale.
        // The menu raises this; exposure and fly speed deliberately do not.
        if (menu_.bakeRequested) {
            menu_.bakeRequested = false;
            menu_.bakeStatus = bakeDefaults();
            std::printf("v2: %s\n", menu_.bakeStatus.c_str());
            std::fflush(stdout);
        }
        if (menu_.invalidated) {
            menu_.invalidated = false;
            scene_.sky.setSun(sunAz_, sunEl_);
            // A scale change goes through resize(), which reallocates and
            // resets on its own; everything else here only stales the film.
            rend_.resetAccumulation();
        }
        // The OUTPUT resolution is what --scale means, so switching to the
        // upscaling model keeps the picture the same size and quarters the
        // pixels traced -- which is the only comparison worth making.
        rend_.resize(maxi(16, int(float(fbW_) * opt_.scale)),
                     maxi(16, int(float(fbH_) * opt_.scale)));

        RenderSettings cfg = base_;
        // Short paths while flying. This is the whole moving/still trade now,
        // and it needs nothing reallocated to switch.
        if (moving_) cfg.maxDepth = mini(base_.maxDepth, opt_.movingDepth);

        // A running clock means the lighting is never static, so the film has
        // to forget old samples rather than be thrown away when they go stale.
        // Paused, there is nothing to track and the accumulation runs free.
        cfg.maxAccum = (clock_.paused || clock_.cycleSpeed == 0.0f) ? 0u : 128u;
        lastDepth_ = cfg.maxDepth;

        Camera cam;
        cam.origin = pos_;
        cam.target = pos_ + forward() * 50.0f;
        cam.fovDeg = fov_;
        cam.aperture = 0.0f;  // a pinhole here; depth of field is for stills
        cam.focusDist = 40.0f;

        rend_.renderSample(cam.gpu(rend_.renderWidth(), rend_.renderHeight()), cfg);

        // Resolving means a tone map and an 8 MB read back, so it is skipped
        // on most frames once the image has settled: the frames it skips go
        // into accumulating instead, and the picture converges faster for
        // redrawing less often while it does.
        const bool resolveNow = moving_ || rend_.samples() <= 8 || (rend_.samples() % 3) == 0;

        if (resolveNow) {
            rend_.resolve(cfg);
            rend_.downloadDisplay(&rgba_);
        }

        moving_ = false;  // cleared only once the frame it applied to is drawn
    }

    void present() {
        const double t0 = nowMs();
        const int w = rend_.outWidth(), h = rend_.outHeight();
        if (rgba_.size() < size_t(w) * h * 4) {
            msBlit_ = 0.0f;
            return;
        }

        glBindTexture(GL_TEXTURE_2D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());

        glViewport(0, 0, fbW_, fbH_);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        // v flipped: the film's first row is the top of the image, GL's is the
        // bottom.
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
        glEnd();
        drawHud();
        drawMenu();
        glDisable(GL_TEXTURE_2D);
        msBlit_ = float(nowMs() - t0);
    }

    // -----------------------------------------------------------------------
    // The settings panel, as a second quad at WINDOW resolution.
    // -----------------------------------------------------------------------
    void drawMenu() {
        if (!menu_.open) return;

        // Scaled to the window so it stays readable on a 4K display without
        // swallowing a small one.
        const int fontPx = mini(26, maxi(13, fbH_ / 60));
        if (fontPx != panelFont_) {
            // Two steps, because the panel's size is quoted in CHARACTERS and
            // the character width is only known once a font has been realised.
            panel_.resize(16, 16, fontPx);
            panel_.resize(menu_.panelWidth(panel_), menu_.panelHeight(panel_), fontPx);
            panelFont_ = fontPx;
        }

        MenuStatus st;
        st.fps = fps_;
        st.traceMs = rend_.traceMs();
        st.resolveMs = rend_.resolveMs();
        st.renderW = rend_.renderWidth();
        st.renderH = rend_.renderHeight();
        st.outW = rend_.outWidth();
        st.outH = rend_.outHeight();
        st.samples = rend_.samples();
        st.trees = int(scene_.chunkCount());
        menu_.draw(panel_, st);

        const int pw = panel_.width(), ph = panel_.height();
        if (pw <= 0 || ph <= 0) return;

        blitPanel(panel_, menuTex_, kMenuX, kMenuY + hud_.height() + 10, true);
    }

    // -----------------------------------------------------------------------
    // Write the live settings back out as src/core/defaults.h.
    //
    // The path comes from V2_SOURCE_DIR, baked in by build.sh, rather than
    // being derived from the working directory -- the launcher runs the exe
    // from C:\voxelbit, so anything relative would land in the wrong tree and
    // report success while writing nothing anyone would ever compile.
    // -----------------------------------------------------------------------
    std::string bakeDefaults() {
#ifndef V2_SOURCE_DIR
        return "bake unavailable: built without V2_SOURCE_DIR";
#else
        const std::string path = std::string(V2_SOURCE_DIR) + "/core/defaults.h";
        char clockText[16];
        clock_.clock(clockText, sizeof(clockText));
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return "could not write " + path;

        std::fprintf(f,
            "// ---------------------------------------------------------------------------\n"
            "// defaults.h -- the settings v2 starts with.\n"
            "//\n"
            "// GENERATED FILE. Everything below is rewritten wholesale by the \"Bake as\n"
            "// default\" row in the in-viewer settings menu (Y), so hand edits survive only\n"
            "// until the next bake -- but hand edits are perfectly fine, the format is just\n"
            "// constants and the file is checked in.\n"
            "//\n"
            "// The point of it is that the settings menu and the command line stop being\n"
            "// separate universes: fly around, tune the picture until it looks right, bake,\n"
            "// rebuild, and the thing you tuned is what v2 opens with.\n"
            "//\n"
            "// A bake writes SOURCE, not a config file, deliberately. A config read at\n"
            "// startup would be one more thing that can be stale, missing, or disagree with\n"
            "// the flags; a header means the defaults are visible in the diff, travel with\n"
            "// the branch, and cost nothing at runtime.\n"
            "// ---------------------------------------------------------------------------\n"
            "#pragma once\n"
            "\n"
            "namespace v2 {\n"
            "namespace defaults {\n"
            "\n"
            "constexpr float kScale = %.2ff;\n"
            "constexpr int kDepth = %d;\n"
            "constexpr int kMovingDepth = %d;\n"
            "constexpr float kExposure = %.2ff;\n"
            "constexpr float kSpeed = %.1ff;\n"
            "constexpr float kFov = %.1ff;\n"
            "constexpr float kSunAz = %.1ff;\n"
            "constexpr float kSunEl = %.1ff;\n"
            "constexpr int kWidth = %d;\n"
            "constexpr int kHeight = %d;\n"
            "constexpr int kTrees = %d;\n"
            "constexpr float kTimeOfDay = %.4ff;  // %s\n"
            "constexpr float kCycleSpeed = %.2ff;\n"
            "\n"
            "}  // namespace defaults\n"
            "}  // namespace v2\n",
            opt_.scale, base_.maxDepth, opt_.movingDepth, base_.exposure, opt_.speed, fov_,
            sunAz_, sunEl_, fbW_, fbH_, opt_.trees, clock_.tday, clockText, clock_.cycleSpeed);
        std::fclose(f);
        return "baked -- run rebuild.bat to apply";
#endif
    }

    // -----------------------------------------------------------------------
    // The always-on readout, top left.
    //
    // Its own panel rather than a line inside the settings menu, because the
    // number you want while flying is the one you cannot open a menu to read --
    // opening the menu changes the frame rate it would be reporting.
    //
    // Rebuilt only when the TEXT changes, not every frame. The GDI draw plus
    // the texture upload is a fraction of a millisecond, but at 700 fps a
    // fraction of a millisecond is several percent, and the string only
    // actually changes twice a second.
    // -----------------------------------------------------------------------
    void drawHud() {
        const int fontPx = mini(24, maxi(12, fbH_ / 64));

        char line[160];
        std::snprintf(line, sizeof(line), " %.0f fps   %dx%d   %u spp   %s ", fps_,
                      rend_.outWidth(), rend_.outHeight(), rend_.samples(), clockText_);

        if (fontPx != hudFont_ || std::strcmp(line, hudText_) != 0) {
            std::snprintf(hudText_, sizeof(hudText_), "%s", line);
            if (fontPx != hudFont_) {
                hud_.resize(16, 16, fontPx);  // realise a font to get its metrics
                hudFont_ = fontPx;
            }
            hud_.resize(int(std::strlen(line)) * hud_.charWidth() + 16, hud_.lineHeight() + 10,
                        fontPx);
            hud_.clear();
            hud_.text(8, 5, line, RGB(235, 240, 248), true);
            hud_.compose(150);
            hudDirty_ = true;
        }

        blitPanel(hud_, hudTex_, kHudX, kHudY, hudDirty_);
        hudDirty_ = false;
    }

    // Upload (if changed) and draw one panel at window pixel (x, y).
    void blitPanel(const TextPanel &p, unsigned tex, int x, int y, bool upload) {
        const int pw = p.width(), ph = p.height();
        if (pw <= 0 || ph <= 0) return;

        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (upload)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         p.pixels());

        const float nx0 = float(x) / float(fbW_) * 2.0f - 1.0f;
        const float nx1 = float(x + pw) / float(fbW_) * 2.0f - 1.0f;
        const float ny0 = 1.0f - float(y) / float(fbH_) * 2.0f;
        const float ny1 = 1.0f - float(y + ph) / float(fbH_) * 2.0f;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(nx0, ny0);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(nx1, ny0);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(nx1, ny1);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(nx0, ny1);
        glEnd();
        glDisable(GL_BLEND);
    }

    void screenshot() {
        char name[64];
        std::snprintf(name, sizeof(name), "v2_shot_%03d.png", shotIndex_++);
        if (writePng(name, rgba_, rend_.outWidth(), rend_.outHeight()))
            std::printf("v2: wrote %s\n", name);
        else
            std::fprintf(stderr, "v2: could not write %s\n", name);
    }
};

}  // namespace v2
