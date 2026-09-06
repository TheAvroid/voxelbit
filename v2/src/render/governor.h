// ---------------------------------------------------------------------------
// governor.h -- hold the frame rate in a band, by giving up the cheapest
//               thing first.
//
// THE TARGET IS A BAND, NOT A NUMBER: 60 fps is the floor, 120 is where it
// stops trying to buy more. A single target is unimplementable -- anything that
// hits exactly one number is either oscillating around it or lying -- and a
// floor on its own gives up quality it does not have to. Between the two the
// governor does nothing at all, which is the state it should be in most of the
// time.
//
// ---------------------------------------------------------------------------
// WHY THERE ARE TWO AXES AND NOT ONE LADDER.
//
// The obvious design is a single quality ladder, five rungs, step up and down
// it. That is wrong here, and the reason is written in the settings menu three
// files away: the Resolution slider notches to ten percent because "every
// distinct value reallocates the film and its guides and recreates the DLSS
// feature -- and that last one submits and WAITS on the device". A governor
// built on one ladder would reach for that knob as readily as any other, and a
// governor that stalls the device to save two milliseconds has made the frame
// rate worse in exactly the moment it was trying to help.
//
// So the knobs are split by what they COST TO TURN, not by what they save:
//
//   FAST AXIS   bounces, and sun samples per vertex. Changing either is a
//               constant in the next dispatch. Nothing is reallocated, no
//               pipeline is rebuilt, the device is never waited on. These move
//               freely, every time the governor looks.
//
//   SLOW AXIS   the DLSS quality mode, then the render scale. Both reallocate
//               the film and both stall. These move only when the fast axis has
//               been pinned at its limit for a sustained stretch -- seconds,
//               not frames -- and then by one step, with a long cooldown after.
//
// The fast axis is tried to exhaustion before the slow one is touched at all.
//
// ---------------------------------------------------------------------------
// IT WILL NOT RUN WHILE THE FILM IS CONVERGING, and that is not a special case
// bolted on -- it is the whole reason the engine has two renderers.
//
// With Ray Reconstruction on there is no film: every frame is reconstructed
// from one sample plus its guides, so changing the bounce count costs the next
// frame and nothing before it. The governor is free.
//
// With Ray Reconstruction OFF the film accumulates for as long as the camera
// holds still, and every setting the governor could touch calls invalidate()
// and throws that film away. A governor chasing 60 fps against a converging
// accumulator would reset it forever: the frame rate while accumulating is
// whatever one sample costs, it never improves by standing still, so the
// governor would keep spending quality to fix a number that was never going to
// move, and the picture -- which was busy becoming correct -- would restart
// every time. So when the film is live and the camera is still, the governor
// stands down and says so.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>

namespace v2 {

// What the governor is allowed to turn, handed in by the app each frame and
// written back through. Deliberately a view onto the app's own Options rather
// than a copy: there is exactly one set of live settings in this engine, the
// menu edits it directly, and a governor holding a second copy would be a
// second opinion about what the renderer is currently doing.
struct GovernorKnobs {
    int *maxDepth = nullptr;     // bounces, moving and still
    int *movingDepth = nullptr;
    int *shadowRays = nullptr;   // sun samples per vertex
    float *scale = nullptr;      // render scale, 0.10 .. 2.00
    int *dlssQuality = nullptr;  // DlssQuality enum, as an int
};

// The floors and ceilings the governor may move between. These are NOT the
// menu's limits -- the menu lets you ask for 32 bounces and 10% scale, and
// should. These are the range within which the picture is still the picture:
// below three bounces a conifer canopy loses its depth entirely, and below 50%
// scale Ray Reconstruction is upscaling from less than it needs.
struct GovernorLimits {
    int minDepth = 3, maxDepth = 16;
    int minShadowRays = 1, maxShadowRays = 4;
    float minScale = 0.50f, maxScale = 1.00f;
    int minDlss = 0, maxDlss = 4;  // UltraPerformance .. DLAA
};

class QualityGovernor {
  public:
    bool enabled = false;      // off unless asked for; see the menu row
    float floorFps = 60.0f;    // below this, give something up
    float ceilingFps = 120.0f; // above this, take something back

    // ---------------------------------------------------------------------
    // The smoothed frame rate the governor decides on.
    //
    // NOT the instantaneous one. Streaming a chunk in costs the main thread a
    // structure build and a top-level rebuild, and that lands in a single frame
    // -- so the instantaneous rate dips hard every time you cross a chunk
    // boundary, through no fault of any setting. A governor reading that would
    // drop the resolution because you walked north.
    //
    // An exponential moving average with a ~1 second constant rides over those
    // and still answers a real change in load within a second or so.
    void sample(float dtSeconds) {
        if (dtSeconds <= 0.0f || dtSeconds > 1.0f) return;  // a hitch, or a breakpoint
        const float fps = 1.0f / dtSeconds;
        if (smoothed_ <= 0.0f) { smoothed_ = fps; return; }
        const float a = std::min(1.0f, dtSeconds / kAverageSeconds);
        smoothed_ += (fps - smoothed_) * a;
    }

    float fps() const { return smoothed_; }

    // ---------------------------------------------------------------------
    // Decide. Returns true if anything was changed, and sets `stalls` if what
    // changed was on the slow axis -- the caller uses that to know the next
    // frame will be long through no fault of the scene.
    //
    // `filmIsLive` is: accumulating, and the camera is holding still. See the
    // header -- this is the case the governor must not touch.
    bool update(float dtSeconds, const GovernorKnobs &k, const GovernorLimits &lim,
                bool filmIsLive, bool *stalls = nullptr) {
        if (stalls) *stalls = false;
        sample(dtSeconds);
        if (!enabled) { note_ = "off"; return false; }
        if (smoothed_ <= 0.0f) return false;
        if (filmIsLive) {
            note_ = "standing by: the film is converging";
            return false;
        }

        slowCooldown_ = std::max(0.0f, slowCooldown_ - dtSeconds);

        // IN BAND IS THE ANSWER MOST OF THE TIME, and it is an answer: reset
        // the pressure that would otherwise have accumulated toward a slow-axis
        // move. Without this a frame rate that spends a minute hovering just
        // over the floor eventually drops the resolution anyway, having never
        // actually been too slow.
        if (smoothed_ >= floorFps && smoothed_ <= ceilingFps) {
            pressure_ = 0.0f;
            note_ = fmtNote("holding", k);
            return false;
        }

        const bool tooSlow = smoothed_ < floorFps;

        // -----------------------------------------------------------------
        // FAST AXIS FIRST, ALWAYS. Bounces before sun samples, because a bounce
        // is a whole extra path segment and a sun sample is one shadow ray --
        // the bounce is the bigger lever, so it is the one that answers a
        // shortfall soonest.
        if (tooSlow) {
            if (lower(k.maxDepth, lim.minDepth) || lower(k.movingDepth, lim.minDepth) ||
                lower(k.shadowRays, lim.minShadowRays)) {
                pressure_ = 0.0f;
                note_ = fmtNote("easing", k);
                return true;
            }
        } else {
            // Only climb back if there is real headroom. Restoring a bounce the
            // instant the rate crosses the ceiling is how a governor oscillates:
            // the bounce costs more than the margin that bought it, the rate
            // drops back under, and it is given up again next second.
            if (smoothed_ > ceilingFps * kHeadroom) {
                if (raise(k.shadowRays, lim.maxShadowRays) || raise(k.maxDepth, lim.maxDepth) ||
                    raise(k.movingDepth, lim.maxDepth)) {
                    pressure_ = 0.0f;
                    note_ = fmtNote("restoring", k);
                    return true;
                }
            }
        }

        // -----------------------------------------------------------------
        // SLOW AXIS, under protest. Reached only with the fast axis pinned, and
        // then only after the miss has persisted -- pressure is measured in
        // seconds out of band, not in frames, so a two-second hitch cannot
        // spend a resolution step.
        pressure_ += dtSeconds;
        if (pressure_ < kSlowAxisSeconds || slowCooldown_ > 0.0f) {
            note_ = fmtNote(tooSlow ? "pinned, holding" : "pinned, spare", k);
            return false;
        }

        bool moved = false;
        if (tooSlow) {
            // DLSS mode before render scale. The mode changes what is TRACED
            // and leaves what is PRESENTED alone, so the picture stays the size
            // it was and only its input gets cheaper -- which is the smaller
            // visible change of the two.
            moved = lower(k.dlssQuality, lim.minDlss) || lowerScale(k.scale, lim.minScale);
        } else if (smoothed_ > ceilingFps * kHeadroom) {
            moved = raiseScale(k.scale, lim.maxScale) || raise(k.dlssQuality, lim.maxDlss);
        }

        if (moved) {
            pressure_ = 0.0f;
            slowCooldown_ = kSlowAxisCooldown;
            if (stalls) *stalls = true;
            note_ = fmtNote(tooSlow ? "reducing" : "raising", k);
        }
        return moved;
    }

    // One line for the settings menu and the stats block.
    const std::string &note() const { return note_; }

    // Forget the history. Called when the window is resized or a preset is
    // pressed -- both change the cost of a frame by more than the governor
    // could account for, and averaging across them is averaging two different
    // renderers.
    void reset() {
        smoothed_ = 0.0f;
        pressure_ = 0.0f;
        slowCooldown_ = 0.0f;
    }

  private:
    // A second of frames behind the average, which is long enough to ride over
    // a chunk-boundary structure build and short enough to answer a real change
    // in load before it is annoying.
    static constexpr float kAverageSeconds = 1.0f;
    // How long the rate must sit out of band, with the fast axis pinned, before
    // a stalling change is worth making.
    static constexpr float kSlowAxisSeconds = 3.0f;
    // And how long to leave it alone afterwards, so the measurement that judges
    // the change is taken after the change has settled.
    static constexpr float kSlowAxisCooldown = 4.0f;
    // The margin over the ceiling required before anything is given back. Ten
    // percent: enough that the restored setting fits inside the headroom that
    // justified it.
    static constexpr float kHeadroom = 1.10f;

    static bool lower(int *v, int floorV) {
        if (!v || *v <= floorV) return false;
        --*v;
        return true;
    }
    static bool raise(int *v, int ceilV) {
        if (!v || *v >= ceilV) return false;
        ++*v;
        return true;
    }
    // Scale moves in the same ten percent notches the menu uses, for the same
    // reason: a finer step is a reallocation nobody can see the benefit of.
    static bool lowerScale(float *v, float floorV) {
        if (!v || *v <= floorV + 1e-3f) return false;
        *v = std::max(floorV, *v - 0.10f);
        return true;
    }
    static bool raiseScale(float *v, float ceilV) {
        if (!v || *v >= ceilV - 1e-3f) return false;
        *v = std::min(ceilV, *v + 0.10f);
        return true;
    }

    std::string fmtNote(const char *verb, const GovernorKnobs &k) const {
        char b[128];
        std::snprintf(b, sizeof(b), "%s at %.0f fps   %d bounces, %d sun, %d%%", verb, smoothed_,
                      k.maxDepth ? *k.maxDepth : 0, k.shadowRays ? *k.shadowRays : 0,
                      k.scale ? int(*k.scale * 100.0f + 0.5f) : 0);
        return b;
    }

    float smoothed_ = 0.0f;
    float pressure_ = 0.0f;      // seconds spent out of band with the fast axis pinned
    float slowCooldown_ = 0.0f;  // seconds until another stalling change is allowed
    std::string note_ = "off";
};

}  // namespace v2
