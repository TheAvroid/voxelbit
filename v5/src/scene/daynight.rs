// ---------------------------------------------------------------------------
// daynight.rs -- the clock the sun runs on.
//
// Ported from v2's scene/daynight.h, which was itself ported from the JS
// engine's `cycleSpeed` rather than reinvented, because that design had already
// been argued out there and the decisions in it are not obvious:
//
// ONE SIGNED LADDER, NOT A REVERSE MODE. cycle_speed runs -512 ... -0.25, 0.25
// ... 512 in x1.6 notches, and scrolling DOWN walks it off the bottom of the
// forward ladder straight into rewind. There is no reverse mode to enter or
// leave, and 1x is always the same number of notches away it was.
//
// ZERO IS DELIBERATELY NOT A RUNG. A stopped clock is what a pause is for; a
// rung the wheel cannot leave in a single notch reads as a jammed control.
//
// THE CROSSOVER COMPARISON NEEDS A FUDGE FACTOR, and the original comment
// records exactly why: 0.25 is not a power of 1.6, so the rung is only exact
// while a clamp keeps putting it there. Rewind deep, walk back, and the
// magnitude returns as 0.2500000000000001 -- which fails a bare `<= CS_MIN`,
// takes the floor branch, and silently eats a notch. 1.0001 is far wider than
// the ~1e-15 the round trip drifts and far narrower than the 1.6 gap to the
// next rung.
// ---------------------------------------------------------------------------

use bevy::prelude::*;
use std::f32::consts::TAU;

/// A day at 1x, in seconds. Twenty minutes, as in the JS engine.
pub const DAY_SECONDS: f32 = 1200.0;
pub const CS_MIN: f32 = 0.25;
pub const CS_MAX: f32 = 512.0;

#[derive(Resource)]
pub struct DayNight {
    /// Time of day in [0, 1).
    pub tday: f32,
    pub cycle_speed: f32,
    pub paused: bool,

    /// Which compass direction the arc is anchored to at NOON, and how high it
    /// climbs there. 68 and 48 are chosen together so that an 08:00 start
    /// reproduces v4's sun exactly -- azimuth 38, elevation 24 -- which is the
    /// light every reference image of this scene was made under.
    pub azimuth_base: f32,
    pub peak_elevation: f32,
}

impl Default for DayNight {
    fn default() -> Self {
        Self {
            // 7:00 am is where the JS engine starts and a good hour to open on:
            // the sun is up but still low enough to rake across the stand.
            tday: 7.0 / 24.0,
            cycle_speed: 1.0,
            paused: false,
            azimuth_base: 68.0,
            peak_elevation: 48.0,
        }
    }
}

impl DayNight {
    pub fn advance(&mut self, dt: f32) {
        if self.paused {
            return;
        }
        // The wrap is `raw - floor(raw)` rather than a remainder, so it is
        // correct for a NEGATIVE raw too -- which is the whole point of a
        // signed speed.
        let raw = self.tday + dt * self.cycle_speed / DAY_SECONDS;
        self.tday = raw - raw.floor();
    }

    /// One notch along the ladder. `up` = scrolled up = later in time.
    pub fn nudge_speed(&mut self, up: bool) {
        let a = self.cycle_speed.abs();
        let s = if self.cycle_speed < 0.0 { -1.0 } else { 1.0 };
        if up == (s > 0.0) {
            self.cycle_speed = s * CS_MAX.min(a * 1.6); // faster the way it already runs
        } else if a <= CS_MIN * 1.0001 {
            self.cycle_speed = -s * CS_MIN; // slowest notch: hand over to the other direction
        } else {
            self.cycle_speed = s * CS_MIN.max(a / 1.6); // toward zero, floored
        }
    }

    pub fn scrub_hours(&mut self, hours: f32) {
        let raw = self.tday + hours / 24.0;
        self.tday = raw - raw.floor();
    }

    // -----------------------------------------------------------------------
    // Sun position for the current time.
    //
    // theta is measured from SUNRISE, not from midnight, so tday 0.25 puts the
    // sun exactly on the horizon and 0.5 at its peak. Elevation goes negative
    // at night and the sky model is what deals with that -- the clock has no
    // opinion about how dark the sky gets.
    // -----------------------------------------------------------------------
    pub fn elevation_deg(&self) -> f32 {
        ((self.tday - 0.25) * TAU).sin() * self.peak_elevation
    }

    /// A HALF TURN ACROSS THE DAY, not a full one. Sweeping 360 degrees of
    /// compass in a day put the sun in the north at breakfast and had it
    /// running the wrong way round the sky; a real one rises east, crosses
    /// south, sets west -- about 180 degrees, centred on noon.
    pub fn azimuth_deg(&self) -> f32 {
        self.azimuth_base + (self.tday - 0.5) * 180.0
    }

    pub fn is_night(&self) -> bool {
        self.elevation_deg() <= 0.0
    }

    /// "07:23", for the HUD.
    pub fn clock(&self) -> String {
        let h = self.tday * 24.0;
        let hh = (h as i32) % 24;
        let mm = (((h - h.floor()) * 60.0) as i32) % 60;
        format!("{hh:02}:{mm:02}")
    }

    /// "x1.0" running forward, "<<x2.6" rewinding -- the JS engine's HUD form.
    pub fn speed_label(&self) -> String {
        if self.paused {
            "paused".into()
        } else if self.cycle_speed < 0.0 {
            format!("<<x{:.2}", -self.cycle_speed)
        } else {
            format!("x{:.2}", self.cycle_speed)
        }
    }
}
