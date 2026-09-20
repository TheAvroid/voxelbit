#pragma once
// ---------------------------------------------------------------------------
// vitals.h -- v1's two bars: health and hunger.
//
// (user 2026-09-19: "import the damage/hunger mechanics from v1 into v2".)
//
// A straight port of v1's sim/vitals.js. Until this file v2 had NO mortality at
// all -- no health, no hunger, no hazard could touch you, and `swallowed` in
// app_capture.inl printed "ate one" and did nothing with it.
//
// ---- WHAT THE TWO BARS ARE ------------------------------------------------
//
// FIVE POINTS EACH, both integers. v1's own words for why five: the health bar
// is drawn as the pixelation creeping in from the edge of the screen and it has
// five states because the bar has five points, so a fractional heal would leave
// the screen between two of them. Hunger is the same subtraction in gold.
//
// HEALTH NEVER REGENERATES. Nothing moves it up but eating and nothing moves it
// down but a hazard -- "the red on the screen is a standing bill, and food is
// the only way to pay it". Hunger is what paces the healing, because hunger is
// what empties on its own.
//
// ---- WHAT A HAZARD'S `amount` MEANS ---------------------------------------
//
// Every hazard quotes damage on Minecraft's 20-point scale, where a heart is 4
// points, and the conversion happens HERE at the single door they all come
// through -- see kVitDmgPerPoint. That is v1's arrangement and it is kept for
// its reason: the numbers at the nine hazard sites are tuned, and rewriting
// them into a new unit is how a tuned curve gets quietly detuned. One health
// point per heart's worth, and never less than one, so no blow is free.
//
// ---- ONE DEVIATION FROM v1, AND IT IS A NUMBER ----------------------------
//
// v1 charges the SPRINT rate above 0.62 m/s. Its own walk is 4.6 m/s (WALK 46
// voxels), so that threshold is true of a stroll, a crouch-walk and everything
// else -- which contradicts the comment two lines above it in v1 ("Walking is
// FREE ... a stroll that starves you turns exploring into a chore"). The code
// and the comment disagree and the CODE is the mechanic the user plays, so the
// number is ported as it stands and the disagreement is written down here
// rather than resolved silently. What it works out to: walking drains a hunger
// point every ~70 s and the full bar in about six minutes of moving.
//
// ---- HOW IT REPORTS ---------------------------------------------------------
//
// v1 throws its bursts from inside the drain, at the one place a point is
// actually spent, so no route into it has to remember to. This has no way to
// reach the particle system from here, so it COUNTS them instead: `redBursts`
// and `goldBursts` are drained by the caller each frame. Same property -- the
// count is incremented at the one place the point is spent -- without this
// header knowing what a spark is.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>

#include "core/vecmath.h"

namespace v2 {

// FIVE, AND BOTH BARS ARE FIVE. See the note above: the screen effect has five
// states because the bar does.
inline constexpr int kVitHpMax = 5;
inline constexpr int kVitFoodMax = 5;

// A heart's worth of damage on the 20-point scale every hazard quotes in.
inline constexpr int kVitDmgPerPoint = 4;

// ONE POINT OF A FIVE-POINT BAR, at half pace -- v1's own arithmetic,
// 4.0 x 4 x 2.
inline constexpr float kExhStep = 32.0f;

// Per METRE travelled.
inline constexpr float kExhWalk = 0.0f;
inline constexpr float kExhSprint = 0.1f;
inline constexpr float kExhSwim = 0.01f;
// Per event.
inline constexpr float kExhJump = 0.05f;
inline constexpr float kExhSprintJump = 0.2f;
inline constexpr float kExhIdle = 0.02f;   // per second
inline constexpr float kExhHurt = 0.1f;
inline constexpr float kExhAttack = 0.1f;
inline constexpr float kExhMine = 0.005f;

// ABOVE THIS IS "SPRINTING" as far as the drain is concerned -- the ground
// actually covered, not the sprint KEY, so being shoved or sliding counts
// honestly. v1's number; see the deviation note at the top of the file.
inline constexpr float kExhSprintMs = 0.62f;

// STARVATION -- the only thing that moves health on a clock, and it fires ONLY
// at an empty bar, which the gold has been announcing for four whole points. It
// goes through hurt() with `bypass`, so it charges no exhaustion of its own:
// that is what stops the original feeding itself into a spiral.
inline constexpr float kStarveSecs = 4.0f;

// A TELEPORT IS NOT A WALK. The drain is charged off the real position delta,
// so a respawn or a /locate hop would otherwise charge thousands of metres and
// empty the bar in one frame.
inline constexpr float kVitTeleportM = 3.0f;

// How long the hit kick rides down. An animation, not a mechanic.
inline constexpr float kVitHurtFall = 0.55f;

// -- THE DEATH SCREEN -------------------------------------------------------
//
// IT FADES rather than cutting: dying mid-swing and being slammed straight to
// black reads as a crash, and the fade is what leaves the last frame of the
// world legible long enough to see what killed you.
inline constexpr double kGameOverFadeMs = 900.0;
// ...AND HOW LONG IT HOLDS before the body gets up again. Long enough to read
// two lines and register that the run ended, short enough not to be a wait.
inline constexpr double kGameOverHoldMs = 4000.0;

struct Vitals {
    int hp = kVitHpMax;
    int food = kVitFoodMax;
    float exh = 0.0f;
    float starveT = 0.0f;
    float hurtT = 0.0f;   // 1 on the blow, down to 0 over kVitHurtFall

    // ---- events, drained by the caller each frame ----
    int redBursts = 0;             // health points lost since the last drain
    int goldBursts = 0;            // hunger points lost since the last drain
    const char *deathWhy = nullptr;   // set on the frame hp reaches 0

    bool alive() const { return hp > 0; }

    // 0 at full, 1..4 as the points go -- four shades, because the fifth step
    // is death rather than a colour.
    int redLevel() const { return maxi(0, mini(4, kVitHpMax - hp)); }
    int goldLevel() const { return maxi(0, mini(4, kVitFoodMax - food)); }

    // lx/lz seeded from the CURRENT position: left at zero the first tick
    // charges a walk from the world origin and empties the bar before the
    // player has moved.
    void reset(const Vec3 &p) {
        hp = kVitHpMax;
        food = kVitFoodMax;
        exh = 0.0f;
        starveT = 0.0f;
        hurtT = 0.0f;
        lx_ = p.x;
        lz_ = p.z;
        wasAir_ = false;
        seeded_ = true;
        redBursts = goldBursts = 0;
        deathWhy = nullptr;
    }

    // SPENDING THE BAR -- one place, so every charge goes through the same
    // clamp and the same drain.
    //
    // A `while`, NOT an `if`, and it matters at low frame rates: one tick can
    // carry more than a step's worth of exhaustion, and an `if` would discard
    // the remainder and make the drain frame-rate dependent. Capped at four
    // steps of debt so a long idle or fly stretch cannot bank a bill that
    // empties the bar the instant it lands.
    void exhaust(float e) {
        if (!(e > 0.0f) || hp <= 0) return;
        exh = minf(exh + e, kExhStep * 4.0f);
        for (int guard = 0; exh >= kExhStep && guard < 8; ++guard) {
            exh -= kExhStep;
            if (food > 0) {
                --food;
                ++goldBursts;   // the burst fires HERE, where the point is spent
            }
        }
    }

    void onAttack() { exhaust(kExhAttack); }
    void onMine() { exhaust(kExhMine); }

    // `bypass` is damage that charges no exhaustion -- drowning, falling, lava,
    // starvation. It is what stops starvation feeding itself into a death
    // spiral, and at the call sites it documents which hazards are unblockable.
    void hurt(int amount, const char *why, bool bypass = false) {
        if (amount <= 0 || hp <= 0) return;
        const int points = maxi(1, (amount + kVitDmgPerPoint - 1) / kVitDmgPerPoint);
        hp = maxi(0, hp - points);
        if (!bypass) exhaust(kExhHurt);
        hurtT = 1.0f;
        ++redBursts;
        // ONE DEATH, ONE THRESHOLD. v1 had three paths here (hp at zero, a
        // five-hit run, and starvation) and collapsed them to this one: a
        // player two points from dying cannot be looking at a clear screen, so
        // there is no hidden counter to reconcile against a visible bar.
        if (hp <= 0 && !deathWhy) deathWhy = why ? why : "you died";
    }

    // ONE BITE, ONE POINT OF EACH. The refusal is "there is nothing to gain",
    // not "hunger is full": refusing on a full stomach alone would lock a hurt
    // player out of healing until they had run around to burn off a bar they
    // cannot spend on purpose, which is a rule the screen never explains.
    bool eat(int gain = 1) {
        if (gain <= 0) return false;
        if (hp >= kVitHpMax && food >= kVitFoodMax) return false;
        hp = mini(kVitHpMax, hp + gain);
        food = mini(kVitFoodMax, food + gain);
        starveT = 0.0f;   // a bite is never followed by the tick it had earned
        return true;
    }

    // ---- THE TICK ---------------------------------------------------------
    //
    // Called unconditionally once a frame. In v1 this sat inside the movement
    // branch and silently stopped the moment fly mode engaged, which is the one
    // mistake worth not repeating.
    //
    // FLYING IS FREE, deliberately: it is a debug mode, not a way to play.
    void tick(float dt, const Vec3 &p, bool onGround, bool fly, bool swimming, bool sprintJump) {
        if (!seeded_) {
            lx_ = p.x;
            lz_ = p.z;
            seeded_ = true;
        }
        if (hurtT > 0.0f) hurtT = maxf(0.0f, hurtT - dt / kVitHurtFall);
        if (hp <= 0) return;

        if (!fly) {
            const float dx = p.x - lx_, dz = p.z - lz_;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > 0.0f && dist < kVitTeleportM) {
                const float speed = dist / maxf(dt, 1e-4f);
                exhaust(dist * (swimming    ? kExhSwim
                                : speed > kExhSprintMs ? kExhSprint
                                                       : kExhWalk));
            }
            // THE TAKEOFF EDGE, so one jump is charged once rather than every
            // airborne frame.
            if (!onGround && !wasAir_) exhaust(sprintJump ? kExhSprintJump : kExhJump);
            wasAir_ = !onGround;
            exhaust(kExhIdle * dt);
        }
        lx_ = p.x;
        lz_ = p.z;

        // The clock runs only on an empty bar and any bite resets it. Real
        // seconds -- nothing else here runs on a tick counter, and one hazard
        // does not justify bringing v1's 20 Hz accumulator back.
        if (food <= 0) {
            starveT += dt;
            if (starveT >= kStarveSecs) {
                starveT = 0.0f;
                // The GOLD burst as well as the red one hurt() fires: starving
                // is the one hunger event that costs health, so both bars are
                // being hit and both say so. Counted first, so the red lands on
                // top of it.
                ++goldBursts;
                hurt(kVitDmgPerPoint, "you starved", true);
            }
        } else {
            starveT = 0.0f;
        }
    }

  private:
    float lx_ = 0.0f, lz_ = 0.0f;
    bool wasAir_ = false;
    bool seeded_ = false;
};

}   // namespace v2
