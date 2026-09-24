// ---------------------------------------------------------------------------
// app_cinema.inl -- CINEMA MODE, on [C].
//
// (user 2026-09-23: "add a cinema mode on the c key. when pressing c, all ui
//  dissapears. the tool in hand dissapears. its just the game itself. when in
//  cinema mode, display cinema in red at the top of the screen. when recording
//  the screen, dont display the recording text in the recording playback. also
//  when in cinema mode, then left clicking on a peice of life, have the camera
//  follow the life in a cinematic, smooth way. press right click to free up
//  the camera from the life.")
//
// WHAT IT HIDES, AND WHERE:
//   * the interface -- onGuiRender draws the CINEMA label and returns, so the
//     readout, REC badge, notices, console reply, panels and watermark all go;
//   * the crosshair and the vitals rim -- skipped in the blit+hud block;
//   * the tool in hand -- hidden where it is DRAWN; the kit is not touched
//     (see toggleCinema for why hiding it through held_.shown doubled tools);
//   The pause menu and the console still draw if you open them: something you
//   are typing into or clicking on has to be seen.
//
// THE RECORDING. The recorder reads the tracer's display texture, upstream of
// every one of these (see the note over recorder_.tick), so a take never had
// the REC badge in it and never has the CINEMA label either -- checked against
// recordings/v2_take_019.mp4. What cinema adds is that the REC badge is gone
// from the SCREEN too, so a capture of the window is as clean as the take.
//
// THE FOLLOW. Every creature in this engine is a slot in one instance band
// (see lifehit.h) and World::flyerBox hands over the box it is drawn in, this
// frame. A click picks the slot the view ray passes through -- aim() without
// its reach, with a forgiveness that grows with distance so a bird at 60 m can
// be clicked -- and the rig then chases that box: three-quarters behind it when
// it moves, a slow orbit when it stops, every quantity eased by a critically
// damped spring so nothing snaps. Right click hands the camera back, and it
// eases back to the eye rather than cutting there.
//
// THE PLAYER DOES NOT MOVE while the camera is away -- streaming, life and the
// birth gate all stay centred on the body, and moving a body you cannot see is
// only confusing. A follow ends by itself if the animal stops being drawn (it
// died, or was recycled out of range) or the camera is dragged past
// kCineLeashM from the body, where the world stops being streamed.
// ---------------------------------------------------------------------------

static constexpr float kCinePickM = 250.0f;      // how far a click can reach
static constexpr float kCineLeashM = 160.0f;     // the rig's longest way from the body
static constexpr float kCineCamSmooth = 0.35f;   // s -- the camera's spring
static constexpr float kCineLookSmooth = 0.18f;  // s -- where it points
static constexpr float kCineAzSmooth = 1.2f;     // s -- swinging round behind
static constexpr float kCineReturnSmooth = 0.3f; // s -- flying back to the eye
static constexpr float kCineOrbitRad = 0.12f;    // rad/s round a still animal
static constexpr float kCineElev = 0.28f;        // rad above it, ~16 degrees

bool cinema_ = false;
// -- THE WHEEL PULLS THE CAMERA BACK (user 2026-09-23: "let me zoom out in
// cinema mode with the scroll wheel. creating more distance from the life") --
// A multiplier on the follow distance, 20% a notch: down is further out, up is
// closer, as every map and editor does it. Kept for the session, so a second
// animal is filmed from where you liked the first. The spring already under the
// camera turns each notch into a glide rather than a jump.
static constexpr float kCineZoomStep = 1.2f;
static constexpr float kCineZoomMin = 0.6f, kCineZoomMax = 10.0f;
float cineZoom_ = 1.0f;
// The follow's own state -- see "NOTHING HERE CHANGES THE DISTANCE BUT YOU".
float cineBaseDist_ = 2.0f;               // set at the click, from the box then
float cineLookUp_ = 0.0f;                 // ...and how far above its middle to look
Vec3 cineAnchor_{0, 0, 0}, cineAnchorVel_{0, 0, 0};
float cineZoomSm_ = 1.0f, cineZoomVel_ = 0.0f;
float cineLift_ = 0.0f, cineLiftVel_ = 0.0f;
float cineOccl_ = 1.0f, cineOcclVel_ = 0.0f;   // the part of the distance a trunk or rock leaves clear
bool cineArrived_ = false;
// -- ...AND WASD PUTS IT WHERE YOU WANT IT (user 2026-09-23: "let me adjust
// where the camera is in cinema mode using the wasds. for example being able to
// move right and left with the a and d keys. but also up and down.") --
// While following, A/D swing the camera round the creature and W/S raise and
// lower it; the wheel above is already in-and-out, so the four keys and the
// wheel are the three axes between them. Both are OFFSETS on the automatic
// framing, measured against the animal's heading, so a side-on shot stays
// side-on when it turns -- and kept for the session, like the zoom. Once you
// have steered, the idle orbit stops for that follow: a camera you placed that
// then wanders off on its own is a camera you are fighting.
static constexpr float kCineSteerRad = 1.2f;   // rad/s round it on A/D, ~70 deg
static constexpr float kCineLiftRad = 0.7f;    // rad/s up and down on W/S, ~40 deg
static constexpr float kCineElevMin = -0.05f, kCineElevMax = 1.35f;
float cineAzUser_ = 0.0f;
float cineElevGoal_ = kCineElev, cineElev_ = kCineElev, cineElevVel_ = 0.0f;
bool cineSteered_ = false;
float cineAzAuto_ = 0.0f;   // the automatic part: behind the heading, or the idle orbit
uint32_t cineTestKeys_ = 0; // V2_CINEMA_TEST's held keys: 1 A, 2 D, 4 W, 8 S
bool cineKey(Input::Key k, uint32_t bit) {   // not const: getInputState() is not
    return (cineTestKeys_ & bit) != 0 ||
           (!consoleOpen_ && !menuOpen_ && getInputState().isKeyDown(k));
}
void cinemaZoom(float wheel) {
    if (wheel == 0.0f) return;
    cineZoom_ = clampf(wheel > 0.0f ? cineZoom_ / kCineZoomStep : cineZoom_ * kCineZoomStep,
                       kCineZoomMin, kCineZoomMax);
}
int cineSlot_ = -1;          // the band slot being followed; -1 is none
bool cineRig_ = false;       // the rig owns the frame camera (following or returning)
Vec3 cineCam_{0, 0, 0}, cineCamVel_{0, 0, 0};
Vec3 cineLook_{0, 0, 0}, cineLookVel_{0, 0, 0};
Vec3 cinePrev_{0, 0, 0}, cineVel_{0, 0, 0};
float cineAz_ = 0.0f, cineAzVel_ = 0.0f, cineAzGoal_ = 0.0f;

// A critically damped spring -- Game Programming Gems 4's SmoothDamp. It
// never overshoots, and a target that jumps is chased rather than snapped to.
static float cineDamp(float cur, float goal, float *vel, float smooth, float dt) {
    const float w = 2.0f / maxf(1e-4f, smooth);
    const float x = w * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float c = cur - goal;
    const float t = (*vel + w * c) * dt;
    *vel = (*vel - w * t) * e;
    return goal + (c + t) * e;
}
static Vec3 cineDamp(const Vec3 &cur, const Vec3 &goal, Vec3 *vel, float smooth, float dt) {
    return Vec3(cineDamp(cur.x, goal.x, &vel->x, smooth, dt),
                cineDamp(cur.y, goal.y, &vel->y, smooth, dt),
                cineDamp(cur.z, goal.z, &vel->z, smooth, dt));
}
// ...and on an ANGLE, which has to go the short way round.
static float cineDampAngle(float cur, float goal, float *vel, float smooth, float dt) {
    float d = fmodf(goal - cur + PI, 2.0f * PI);
    if (d < 0.0f) d += 2.0f * PI;
    return cineDamp(cur, cur + d - PI, vel, smooth, dt);
}

// -- THE KIT IS NOT TOUCHED ---------------------------------------------------
//
// (user 2026-09-23: "when going in cinema mode then coming back to regular
//  survival, my tools double and are stacked. prevent that from happening.")
//
// THIS HID THE TOOL BY SETTING held_.shown = false, and in HeldItem that flag
// does not mean "not drawn" -- it means AN EMPTY HAND. give() reads it: a
// pickup that lands while the hand is "empty" takes the hand, selects itself
// and sets shown back to true. So everything that flew to you during a take --
// a thrown tool walked back over, wheat (collected from any distance), seeds,
// a steak -- went through the empty-hand path, the hand filled up behind the
// camera's back, and the restore on the way out wrote the saved flag over a kit
// that no longer matched it. Three scripted runs with nothing arriving never
// reproduced it, which is itself the tell: it needs a pickup mid-take.
//
// So cinema changes nothing about the hand. The tool is hidden where it is
// DRAWN (handDrawn in app_frame.inl, and the stack badge in setStackBadge), the
// kit carries on exactly as in play, and leaving cinema has nothing to restore.
void toggleCinema() {
    cinema_ = !cinema_;
    // At once, not next frame: a cobra strike later this same frame must
    // already find the player out of reach. See Vitals::frozen.
    vitals_.frozen = cinema_;
    if (!cinema_) cinemaRelease();
    std::printf("v2: cinema %s\n", cinema_ ? "ON -- click life to follow it, right click to let go"
                                           : "off");
    std::fflush(stdout);
}

// -- THE CLICK --------------------------------------------------------------
// The ray is the view's, from the eye: the crosshair is hidden in cinema, but
// the centre of the screen is still where you are looking.
bool cinemaPick() {
    const Vec3 eye = pos_, dir = forward();
    const Vec3 tip = eye + dir * kCinePickM;
    int best = -1;
    float bestT = 1e30f;
    for (int i = 0; i < kFlyerInstances; ++i) {
        if (!lifeAtSlot(i).alive()) continue;
        Vec3 mid{0, 0, 0}, half{0, 0, 0};
        if (!world_.flyerBox(i, &mid, &half)) continue;
        // FORGIVENESS THAT GROWS WITH DISTANCE: a 15 cm songbird at 60 m is a
        // pixel or two, and a test as tight as a swing's would make it
        // unclickable. 1.5 cm per metre is about three pixels at 1080p.
        const float d = length(mid - eye);
        const float g = maxf(kAimForgiveM, d * 0.015f);
        const Vec3 grown{half.x + g, half.y + g, half.z + g};
        const float t = LifeHits::hitBox(eye, tip, mid, grown);
        if (t < 0.0f || t >= bestT) continue;
        bestT = t;
        best = i;
    }
    if (best < 0) return false;
    Vec3 mid{0, 0, 0}, half{0, 0, 0};
    world_.flyerBox(best, &mid, &half);
    if (!cineRig_) {
        // THE RIG STARTS AT THE EYE, so taking the camera is a move and not a cut.
        cineCam_ = pos_;
        cineLook_ = pos_ + forward() * length(mid - pos_);
        cineCamVel_ = cineLookVel_ = Vec3(0, 0, 0);
    }
    cineSlot_ = best;
    cineRig_ = true;
    cinePrev_ = mid;
    // THE DISTANCE IS DECIDED HERE, ONCE -- see the follow. The middle
    // half-extent, not the largest: a box is drawn round the whole pose, and a
    // butterfly's wings or a snake's length would put the camera back as if for
    // something three times the size.
    {
        const float lo = minf(half.x, minf(half.y, half.z));
        const float hi = maxf(half.x, maxf(half.y, half.z));
        const float s = half.x + half.y + half.z - lo - hi;
        cineBaseDist_ = clampf(0.6f + 3.2f * s, 1.4f, 12.0f);
        cineLookUp_ = half.y * 0.3f;
    }
    cineAnchor_ = mid;
    cineAnchorVel_ = Vec3(0, 0, 0);
    cineZoomSm_ = cineZoom_;
    cineZoomVel_ = 0.0f;
    cineOccl_ = 1.0f;
    cineOcclVel_ = cineLift_ = cineLiftVel_ = 0.0f;
    cineArrived_ = false;
    cineSteered_ = false;
    cineVel_ = Vec3(0, 0, 0);
    cineAz_ = atan2f(cineCam_.x - mid.x, cineCam_.z - mid.z);
    cineAzGoal_ = cineAz_;
    cineAzVel_ = 0.0f;
    std::printf("v2: cinema -- following %s (slot %d, %.1f m, box %.2f x %.2f x %.2f)\n",
                lifeAtSlot(best).name, best, double(length(mid - pos_)), double(half.x * 2.0f),
                double(half.y * 2.0f), double(half.z * 2.0f));
    std::fflush(stdout);
    return true;
}

// Let go: the rig flies back to the eye on its own spring (see tickCinema),
// and hands the camera over once it is there.
void cinemaRelease() {
    if (cineSlot_ >= 0) {
        std::printf("v2: cinema -- let go\n");
        std::fflush(stdout);
    }
    cineSlot_ = -1;
}

// -- ONE FRAME OF THE RIG ------------------------------------------------------
// Called after life has published this frame's boxes and before the camera is
// built. Writes the frame camera through *camPos / *camDir when the rig owns it.
void tickCinema(float dt, Vec3 *camPos, Vec3 *camDir) {
    if (!cineRig_) return;
    dt = clampf(dt, 1e-4f, 0.1f);
    if (cineSlot_ >= 0) {
        Vec3 mid{0, 0, 0}, half{0, 0, 0};
        const bool drawn = lifeAtSlot(cineSlot_).alive() && world_.flyerBox(cineSlot_, &mid, &half);
        // A JUMP IS A DIFFERENT ANIMAL: the population recycled the slot to a
        // new site. Nothing real crosses 15 m in a frame.
        const bool jumped = drawn && length(mid - cinePrev_) > maxf(15.0f, 40.0f * dt);
        if (!drawn || jumped || length(cineCam_ - player_.eyePosition()) > kCineLeashM) {
            cinemaRelease();
        } else {
            // Its velocity, eased so a hop or a wing beat does not swing the camera.
            const Vec3 v = (mid - cinePrev_) * (1.0f / dt);
            cinePrev_ = mid;
            cineVel_ = cineVel_ + (v - cineVel_) * (1.0f - expf(-dt / 0.4f));
            // -- NOTHING HERE CHANGES THE DISTANCE BUT YOU -------------------
            //
            // (user 2026-09-23: "you also keep zooming in and out randomly,
            //  prevent this.") Three things were moving it, none of them asked:
            //
            //   * THE BOX. flyerBox is drawn round the POSE, so it breathes with
            //     every hop and wing beat -- a rabbit measured 0.59 m deep on
            //     one frame and 0.93 on another -- and the distance was worked
            //     out from it every frame. It is worked out ONCE now, at the
            //     click (cineBaseDist_), and only the wheel moves it after that.
            //   * THE LAG. The camera chased the animal on a spring, so a run
            //     stretched the gap and a stop closed it. Now it frames an
            //     ANCHOR -- the animal on a light spring -- at the set distance,
            //     so the gap to what it is framing does not change at all.
            //   * THE GROUND. A line from the animal to the camera clips every
            //     rise it crosses, and each clip pulled the camera in and let it
            //     back out: a pump. The ground is now answered by going UP (see
            //     cineLift_), which keeps the distance; only a trunk or a rock
            //     still pulls it in, and it eases back out slowly (cineOccl_).
            cineAnchor_ = cineDamp(cineAnchor_, mid, &cineAnchorVel_, 0.3f, dt);
            cineZoomSm_ = cineDamp(cineZoomSm_, cineZoom_, &cineZoomVel_, 0.3f, dt);
            const float dist = cineBaseDist_ * cineZoomSm_;
            const float speedH = sqrtf(cineVel_.x * cineVel_.x + cineVel_.z * cineVel_.z);
            // THREE-QUARTERS BEHIND when it goes somewhere; a slow orbit when it
            // does not -- a camera that sits dead still on a grazing rabbit
            // reads as a paused game. Unless you have placed it: see cineSteered_.
            if (speedH > 0.4f)
                cineAzAuto_ = atan2f(-cineVel_.x, -cineVel_.z) + 0.5f;
            else if (!cineSteered_)
                cineAzAuto_ += kCineOrbitRad * dt;
            // -- WASD: round it and up and down -- see kCineSteerRad. D moves
            // the camera to ITS right, which is +azimuth: d(pos)/d(az) is
            // (cos az, 0, -sin az), camRight()'s own formula for this view.
            const float steer = (cineKey(Input::Key::D, 2u) ? 1.0f : 0.0f) -
                                (cineKey(Input::Key::A, 1u) ? 1.0f : 0.0f);
            const float lift = (cineKey(Input::Key::W, 4u) ? 1.0f : 0.0f) -
                               (cineKey(Input::Key::S, 8u) ? 1.0f : 0.0f);
            if (steer != 0.0f) {
                cineAzUser_ += steer * kCineSteerRad * dt;
                cineSteered_ = true;
            }
            if (lift != 0.0f)
                cineElevGoal_ =
                    clampf(cineElevGoal_ + lift * kCineLiftRad * dt, kCineElevMin, kCineElevMax);
            cineAzGoal_ = cineAzAuto_ + cineAzUser_;
            // A key held is answered at once; the automatic framing keeps its
            // long, lazy swing.
            cineAz_ = cineDampAngle(cineAz_, cineAzGoal_, &cineAzVel_,
                                    steer != 0.0f ? 0.25f : kCineAzSmooth, dt);
            cineElev_ = cineDamp(cineElev_, cineElevGoal_, &cineElevVel_, 0.25f, dt);
            // A LITTLE AHEAD OF IT when it moves -- a subject framed dead
            // centre while it runs reads as the camera being dragged.
            const Vec3 look = cineAnchor_ + Vec3(0.0f, cineLookUp_, 0.0f) +
                              Vec3(cineVel_.x, 0.0f, cineVel_.z) * 0.2f;
            const WalkWorld ww = walkWorld();
            auto dirOf = [&](float el) {
                return Vec3(cosf(el) * sinf(cineAz_), sinf(el), cosf(el) * cosf(cineAz_));
            };
            // -- THE GROUND: GO UP, NOT IN ---------------------------------
            // The least extra elevation that clears the ground (and a metre
            // over it at the camera end -- the grass is knee high), up to
            // about 55 degrees more. It rises quickly and settles slowly, so a
            // bump the line grazed for a frame does not bob the camera.
            auto groundBlocks = [&](float el) {
                const Vec3 d = dirOf(el);
                const float step = maxf(0.2f, dist / 60.0f);
                for (float t = 0.3f; t <= dist + 1e-3f; t += step) {
                    const Vec3 p = look + d * minf(t, dist);
                    const float clear = t >= dist - step ? 1.0f : 0.2f;
                    if (p.y < walkGroundM(ww, p.x, p.z) + clear) return true;
                    if (t >= dist - step && p.y < waterTopAt(p.x, p.z) + 0.5f) return true;
                }
                return false;
            };
            float need = 0.0f;
            while (need < 0.96f && groundBlocks(cineElev_ + need)) need += 0.08f;
            cineLift_ = cineDamp(cineLift_, need, &cineLiftVel_, need > cineLift_ ? 0.2f : 1.2f, dt);
            const Vec3 dir = dirOf(minf(cineElev_ + cineLift_, 1.5f));
            // -- A TRUNK OR A ROCK: IN, QUICKLY, AND BACK OUT SLOWLY ---------
            // The solids the walk collides with (canopies are not solids, and a
            // camera in a crown of needles is a camera in a tree, which is fair).
            float reach = dist;
            {
                const float step = maxf(0.15f, dist / 200.0f);
                for (float t = 0.3f; t < dist && reach == dist; t += step) {
                    const Vec3 p = look + dir * t;
                    for (int k = 0; k < ww.solidCount; ++k) {
                        const Solid &sd = ww.solids[k];
                        if (fabsf(p.x - sd.cx) < sd.hx + 0.25f && fabsf(p.z - sd.cz) < sd.hz + 0.25f &&
                            p.y < sd.top + 0.2f) {
                            reach = maxf(0.8f, t - 0.3f);
                            break;
                        }
                    }
                }
            }
            // AS A FRACTION OF THE DISTANCE, so the slow way back out belongs
            // to the obstacle alone: a notch of the wheel is answered on the
            // zoom's own spring, not held back by a rock that is no longer there.
            const float occl = reach / maxf(1e-3f, dist);
            cineOccl_ = cineDamp(cineOccl_, occl, &cineOcclVel_, occl < cineOccl_ ? 0.12f : 1.5f, dt);
            const Vec3 want = look + dir * (dist * minf(1.0f, cineOccl_));
            // THE SWOOP OUT, and then held: on the way to it, a spring that
            // lengthens with the gap so a creature 50 m off is a flight of two
            // or three seconds rather than a jump; once there, a light one that
            // only takes the edge off, because everything it would smooth has
            // already been smoothed upstream.
            const float gap = length(cineCam_ - want);
            // ARRIVED once it is within a quarter of the distance: a moving
            // animal keeps any fixed gap from closing, and the swoop's long
            // spring would otherwise stay on for as long as it walks.
            if (gap < maxf(0.3f, 0.25f * dist)) cineArrived_ = true;
            const float smooth =
                cineArrived_ ? 0.1f : clampf(kCineCamSmooth + gap * 0.008f, kCineCamSmooth, 0.7f);
            cineCam_ = cineDamp(cineCam_, want, &cineCamVel_, smooth, dt);
            cineLook_ = cineDamp(cineLook_, look, &cineLookVel_, kCineLookSmooth, dt);
        }
    }
    if (cineSlot_ < 0) {
        // -- HOME AGAIN -------------------------------------------------------
        const Vec3 eye = player_.eyePosition();
        const Vec3 at = eye + forward() * 5.0f;
        cineCam_ = cineDamp(cineCam_, eye, &cineCamVel_, kCineReturnSmooth, dt);
        cineLook_ = cineDamp(cineLook_, at, &cineLookVel_, kCineReturnSmooth, dt);
        if (length(cineCam_ - eye) < 0.02f && length(cineLook_ - at) < 0.05f) {
            cineRig_ = false;   // the eye has the camera again
            return;
        }
    }
    // -- AND THE CAMERA ITSELF NEVER GOES UNDER --------------------------------
    // The clamp above keeps the DESTINATION out of the ground; the spring then
    // flies a straight line to it, and a straight line from the player to a
    // rabbit 50 m off goes through whatever dune is between them -- measured,
    // a frame of solid black. So the flown position is lifted too, every frame,
    // and loses its downward speed when it is.
    {
        const float gy = maxf(walkGroundM(walkWorld(), cineCam_.x, cineCam_.z) + 0.5f,
                              waterTopAt(cineCam_.x, cineCam_.z) + 0.3f);
        if (cineCam_.y < gy) {
            cineCam_.y = gy;
            if (cineCamVel_.y < 0.0f) cineCamVel_.y = 0.0f;
        }
    }
    const Vec3 d = cineLook_ - cineCam_;
    if (lengthSq(d) < 1e-8f) return;
    *camPos = cineCam_;
    *camDir = normalize(d);
    moving_ = true;   // a flown camera is a moving one: see the path-depth rule
}

// The rig holds the player still: no walking, no looking, while it is away.
bool cinemaHoldsPlayer() const { return cineRig_; }

// -- V2_CINEMA_TEST=<dir> -- THE WHOLE THING, WITH NO KEYS -------------------
// Frame 200 enters cinema; 260 turns to the nearest creature within 40 m and
// clicks it; every 20th frame of the follow is written to <dir>; 520 lets go;
// the return is written every 10th frame. Pair it with --shot-ui <png>
// --shot-frame 640 to capture the WINDOW -- the label, and nothing else on it.
int cineTestFrame_ = 0;
int cineTestArrive_ = -1;
void tickCinemaTest(Falcor::RenderContext *ctx) {
    static const std::string dir = [] {
        const char *e = std::getenv("V2_CINEMA_TEST");
        return std::string(e ? e : "");
    }();
    if (dir.empty()) return;
    const int f = ++cineTestFrame_;
    if (f == 200 && !cinema_) toggleCinema();
    if (f == 260) {
        // V2_CINEMA_KIND=<name> (as lifehit.h names it: bunny, skunk, fish...)
        // follows the nearest of that species instead of the nearest anything.
        static const std::string kind = [] {
            const char *e = std::getenv("V2_CINEMA_KIND");
            return std::string(e ? e : "");
        }();
        int best = -1;
        float bestD = kind.empty() ? 40.0f : 120.0f;
        Vec3 at{0, 0, 0};
        for (int i = 0; i < kFlyerInstances; ++i) {
            Vec3 mid{0, 0, 0};
            if (!lifeAtSlot(i).alive() || !world_.flyerBox(i, &mid, nullptr)) continue;
            if (!kind.empty() && kind != lifeAtSlot(i).name) continue;
            const float d = length(mid - pos_);
            if (d < bestD) { bestD = d; best = i; at = mid; }
        }
        if (best < 0) {
            std::printf("v2: cinema test -- no creature within 40 m\n");
        } else {
            const Vec3 look = normalize(at - pos_);
            yaw_ = atan2f(look.x, -look.z) * 180.0f / PI;
            pitch_ = asinf(look.y) * 180.0f / PI;
            std::printf("v2: cinema test -- aimed at %s %.1f m off: %s\n", lifeAtSlot(best).name,
                        double(bestD), cinemaPick() ? "picked" : "MISSED");
        }
        std::fflush(stdout);
    }
    // THE SCHEDULE RUNS FROM ARRIVAL, not from the click: a creature 50 m off
    // is a swoop of a hundred-odd frames, and keys held during it measure the
    // swoop. From the frame the rig first sits on its mark (A):
    //   A..A+40 nothing -- the distance must not move on its own
    //   A+40..80 D, A+80..120 W, A+120 three notches out, A+170 back in,
    //   A+220 let go.
    if (cineArrived_ && cineTestArrive_ < 0) {
        cineTestArrive_ = f;
        std::printf("v2: cinema test -- arrived at f%d\n", f);
    }
    const int a = cineTestArrive_ < 0 ? -1 : f - cineTestArrive_;
    cineTestKeys_ = (a >= 40 && a < 80) ? 2u : (a >= 80 && a < 120) ? 4u : 0u;
    if (a == 120)
        for (int k = 0; k < 3; ++k) cinemaZoom(-1.0f);
    if (a == 170)
        for (int k = 0; k < 3; ++k) cinemaZoom(1.0f);
    if (a >= 0 && a <= 220 && a % 10 == 0 && cineSlot_ >= 0) {
        Vec3 mid{0, 0, 0};
        world_.flyerBox(cineSlot_, &mid, nullptr);
        const Vec3 off = cineCam_ - cineAnchor_;
        std::printf("v2: cinema test A+%d  to anchor %.2f m (wants %.2f, reach %.2f), to it %.2f m, "
                    "az %+.0f, elev %.0f%s\n",
                    a, double(length(off)), double(cineBaseDist_ * cineZoomSm_), double(cineBaseDist_ * cineZoomSm_ * minf(1.0f, cineOccl_)),
                    double(length(cineCam_ - mid)), double(atan2f(off.x, off.z) * 180.0f / PI),
                    double(asinf(off.y / maxf(1e-3f, length(off))) * 180.0f / PI),
                    cineTestKeys_ == 2u ? "  [D]" : cineTestKeys_ == 4u ? "  [W]" : "");
        if (a % 40 == 0) {
            char name[512];
            std::snprintf(name, sizeof(name), "%s/follow_A%03d.png", dir.c_str(), a);
            tracer_.writePng(ctx, name);
        }
    }
    if (a == 220) cinemaRelease();
    // V2_CINEMA_KIT: the whole kit before cinema and after it, to catch a
    // tool that comes back doubled (user 2026-09-23: "when going in cinema
    // mode then coming back to regular survival, my tools double").
    static const bool kitLog = std::getenv("V2_CINEMA_KIT") != nullptr;
    auto kit = [&](const char *when) {
        std::printf("v2: cinema kit %s -- hand %s, selected %d:", when,
                    held_.shown ? "shown" : "EMPTY", held_.selected());
        for (int i = 0; i < held_.count(); ++i)
            if (held_.tool(i).carried)
                std::printf("  %s x%d", held_.tool(i).name, held_.tool(i).stack);
        std::printf("\n");
    };
    if (kitLog && f == 150) {
        kit("before");
        std::printf("v2: cinema vitals before -- hp %d, food %d\n", vitals_.hp, vitals_.food);
    }
    // IN CINEMA: a pickup lands (the path that used to take the "empty" hand)
    // and a blow that would kill.
    if (kitLog && f == 210 && cinema_) {
        if (seedsTool_ >= 0) {
            const Tool &t = held_.tool(seedsTool_);
            drops_.spill(seedsTool_, t.models[0], t.sx, t.sy, t.sz, player_.pos, 0.0f);
        }
        vitals_.hurt(1000, "the cinema test");
        std::printf("v2: cinema vitals after a 1000 blow in cinema -- hp %d, food %d\n", vitals_.hp,
                    vitals_.food);
    }
    // C pressed MID-FOLLOW (A+100), not after the camera is home: the hand
    // comes back while the camera is still out by the animal.
    if (kitLog && a == 100 && cinema_) {
        toggleCinema();
        kit("just after");
    }
    if (kitLog && a == 318) {
        kit("after");
        std::printf("v2: cinema vitals after -- hp %d, food %d\n", vitals_.hp, vitals_.food);
    }
    if (kitLog && (a == 102 || a == 110 || a == 130 || a == 160)) {
        char name[512];
        std::snprintf(name, sizeof(name), "%s/hand_A%03d.png", dir.c_str(), a);
        tracer_.writePng(ctx, name);
    }
    if (a > 220 && a <= 300 && a % 20 == 0) {
        std::printf("v2: cinema test A+%d  returning: rig %s, %.2f m from the eye\n", a,
                    cineRig_ ? "still flying" : "HOME", double(length(cineCam_ - player_.eyePosition())));
    }
    if (a == 320) askShutdown(0);
    std::fflush(stdout);
}

// -- THE LABEL ----------------------------------------------------------------
// CINEMA, red, top centre, in the interface's own 3x3 face. Drawn on the
// window after the recorder has read the frame, so it is never in a take.
void drawCinemaLabel(Gui *pGui, float fbW, float fbH) {
    if (!px3_) return;
    styleV2 style(pGui, px3_, 0.0f, fbH);   // no panel behind it
    Gui::Window w(pGui, "v2cinema", {0, 0}, {0, 0},
                  Gui::WindowFlags::AutoResize | Gui::WindowFlags::NoResize);
    px3Font face(px3_);
    // A QUARTER SMALLER and CLOSER TO THE EDGE (user 2026-09-23: "make the
    // cinema text 25% smaller. and move it up higher on the screen") -- 2.0 to
    // 1.5 of the interface scale, the same lever the watermark's quarter took,
    // and its top 1.2% of the way down rather than 3%.
    ImGui::SetWindowFontScale(style.scale * 1.5f);
    const char *text = "cinema";
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    ImGui::SetWindowPos(ImVec2(floorf((fbW - sz.x) * 0.5f) - pad.x,
                               floorf(maxf(0.0f, fbH * 0.012f - pad.y))));
    // THE SHADOW EVERY OTHER OVERLAY LINE HAS: a 3x3 pixel face is thin
    // strokes, and thin red over a bright sky reads as pink without one.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float off = maxf(1.0f, floorf(style.scale * 1.5f));
    ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + off, at.y + off), IM_COL32(0, 0, 0, 170), text);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(255, 26, 26));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
