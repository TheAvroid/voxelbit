// ---------------------------------------------------------------------------
// daynight.h -- the clock the sun runs on.
//
// Ported from the JS engine's `cycleSpeed` rather than reinvented, because that
// design had already been argued out there and the decisions in it are not
// obvious:
//
// ONE SIGNED LADDER, NOT A REVERSE MODE. cycleSpeed runs -512 … -0.25, 0.25 …
// 512 in x1.6 notches, and scrolling DOWN walks it off the bottom of the
// forward ladder straight into rewind. There is no reverse mode to enter or
// leave, and 1x is always the same number of notches away it was.
//
// ZERO IS DELIBERATELY NOT A RUNG. A stopped clock is what a pause is for; a
// rung the wheel cannot leave in a single notch reads as a jammed control.
//
// THE CROSSOVER COMPARISON NEEDS A FUDGE FACTOR, and the JS comment records
// exactly why: 0.25 is not a power of 1.6, so the rung is only exact while a
// clamp keeps putting it there. Rewind deep, walk back, and the magnitude
// returns as 0.2500000000000001 -- which fails a bare `<= CS_MIN`, takes the
// floor branch, and silently eats a notch. 1.0001 is far wider than the ~1e-15
// the round trip drifts and far narrower than the 1.6 gap to the next rung.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdio>

#include "../core/vecmath.h"

namespace v2 {

// A day at 1x, in seconds. Twenty minutes, as in the JS engine.
constexpr float DAY_SECONDS = 1200.0f;
constexpr float CS_MIN = 0.25f, CS_MAX = 512.0f;

class DayNight {
  public:
    // Time of day in [0,1). 7/24 is 7:00 am, which is where the JS engine
    // starts and a good hour to open on: the sun is up but still low enough to
    // rake across the stand.
    float tday = 7.0f / 24.0f;
    float cycleSpeed = 1.0f;
    bool paused = false;

    // Which compass direction the arc is anchored to at NOON, and how high it
    // climbs there. 68 and 48 are chosen together so that the default 08:00
    // start reproduces the Embree engine's sun exactly -- azimuth 38, elevation 24 --
    // the light every reference image of this scene was made under.
    float azimuthBase = 68.0f;
    float peakElevation = 48.0f;

    // WHOLE DAYS ELAPSED, signed, which is what the moon phase runs on. A phase
    // needs a clock longer than one day and tday only carries the fraction, so
    // without this every night would show the same moon.
    float days = 0.0f;

    void advance(float dt) {
        if (paused) return;
        // The wrap is `raw - floor(raw)` rather than fmod, so it is correct for
        // a NEGATIVE raw too -- which is the whole point of a signed speed.
        const float raw = tday + dt * cycleSpeed / DAY_SECONDS;
        // floorf, not a wrap test: it is correct for a NEGATIVE raw too, so
        // running the cycle backwards runs the moon back through its phases.
        days += floorf(raw);
        tday = raw - floorf(raw);
    }

    // One notch along the ladder. `up` = scrolled up = later in time.
    void nudgeSpeed(bool up) {
        const float a = fabsf(cycleSpeed);
        const float s = cycleSpeed < 0.0f ? -1.0f : 1.0f;
        if (up == (s > 0.0f)) {
            cycleSpeed = s * minf(CS_MAX, a * 1.6f);  // faster the way it already runs
        } else if (a <= CS_MIN * 1.0001f) {
            cycleSpeed = -s * CS_MIN;  // slowest notch: hand over to the other direction
        } else {
            cycleSpeed = s * maxf(CS_MIN, a / 1.6f);  // toward zero, floored
        }
    }

    void scrubHours(float hours) {
        const float raw = tday + hours / 24.0f;
        tday = raw - floorf(raw);
    }

    // -----------------------------------------------------------------------
    // Sun position for the current time.
    //
    // theta is measured from SUNRISE, not from midnight, so tday 0.25 puts the
    // sun exactly on the horizon and 0.5 at its peak. Elevation goes negative
    // at night and Sky::setSun is what deals with that -- the clock has no
    // opinion about how dark the sky gets.
    // -----------------------------------------------------------------------
    float elevationDeg() const {
        const float theta = (tday - 0.25f) * TWO_PI;
        return peakElevation * sinf(theta);
    }

    // A HALF TURN ACROSS THE DAYLIGHT, and the REST OF THE TURN AT NIGHT.
    //
    // The daylight half is unchanged and the reasoning behind it still holds:
    // sweeping a uniform 360 degrees of compass across the day put the sun in
    // the north at breakfast and ran it the wrong way round the sky, where a
    // real one rises east, crosses south and sets west.
    //
    // WHAT THAT MODEL COULD NOT DO IS MEET ITSELF AT MIDNIGHT. Half a turn per
    // day means tday 1.0 gives azimuthBase + 90 and tday 0.0 gives
    // azimuthBase - 90, so the wrap was a 180 DEGREE JUMP. The sun is 48
    // degrees under the horizon at that moment so nobody ever saw it move --
    // but the moon is antipodal to the sun (Sky::moonDir), which puts it at
    // +48 degrees, high and in plain view, and it teleported across the sky
    // from one side to the other in a single frame. The night sky's own tint
    // follows sunDir and swung with it. That was "the moon resets".
    //
    // So the sun now completes a FULL turn every day, just not at a uniform
    // rate: 90 degrees spread across the twelve daylight hours, and the
    // remaining 270 covered at night, passing under the north at midnight.
    // Which is what the real one does. Continuous at both handovers by
    // construction -- dawn and dusk are exactly where the elevation above
    // crosses zero, tday 0.25 and 0.75, so the two branches meet at the same
    // azimuth rather than merely close to it.
    //
    // A pleasant consequence rather than a fix: the full moon is now due south
    // and at its highest at midnight, which is where a full moon belongs.
    float azimuthDeg() const {
        if (tday >= 0.25f && tday <= 0.75f)     // daylight: the arc as it was
            return azimuthBase + (tday - 0.5f) * 180.0f;
        // Night, covering the other 270 degrees over the other half of the day.
        // Written as two branches around midnight rather than one modular
        // expression so that each is obviously continuous with the daylight arc
        // it touches.
        if (tday > 0.75f)
            return azimuthBase + 45.0f + (tday - 0.75f) * 540.0f;   // dusk -> north
        return azimuthBase + 180.0f + tday * 540.0f;                // north -> dawn
    }

    bool isNight() const { return elevationDeg() <= 0.0f; }

    // "07:23", for the menu.
    void clock(char *out, int n) const {
        const float h = tday * 24.0f;
        const int hh = int(h) % 24;
        const int mm = int((h - floorf(h)) * 60.0f) % 60;
        std::snprintf(out, n, "%02d:%02d", hh, mm);
    }

    // "x1.0" running forward, "<<x2.6" rewinding -- the JS engine's HUD form.
    void speedLabel(char *out, int n) const {
        if (paused) {
            std::snprintf(out, n, "paused");
        } else if (cycleSpeed < 0.0f) {
            std::snprintf(out, n, "<<x%.2f", -cycleSpeed);
        } else {
            std::snprintf(out, n, "x%.2f", cycleSpeed);
        }
    }
};

}  // namespace v2
