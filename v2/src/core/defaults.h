// ---------------------------------------------------------------------------
// defaults.h -- the settings v2 starts with.
//
// GENERATED FILE. Everything below is rewritten wholesale by "Bake as
// default" in the in-viewer settings menu (Y), so hand edits survive only
// until the next bake -- but hand edits are perfectly fine, the format is just
// constants and the file is checked in.
//
// The point of it is that the settings menu and the command line stop being
// separate universes: fly around, tune the picture until it looks right, bake,
// rebuild, and the thing you tuned is what v2 opens with.
// ---------------------------------------------------------------------------
#pragma once

namespace v2 {
namespace defaults {

constexpr float kScale = 1.00f;
constexpr int kDepth = 6;
constexpr int kMovingDepth = 4;
constexpr float kExposure = 0.500f;
constexpr float kShadowLift = 0.350f;
constexpr float kSpeed = 4.97f;
constexpr float kSensitivity = 0.120f;
constexpr float kEye = 2.00f;
constexpr float kFov = 90.0f;
constexpr float kSunAz = 6.9f;
constexpr float kSunEl = 24.0f;
constexpr int kWidth = 3820;
constexpr int kHeight = 1990;
constexpr int kTrees = 900;
constexpr float kTimeOfDay = 0.4167f;  // 10:00
constexpr float kCycleSpeed = 1.00f;
constexpr bool kAtmosphere = true;

// HOW DARK THE NIGHT IS, as a multiplier over the moon's key light and the
// airglow floor together. 1.00 is the night v2 has always rendered; below it
// the wood goes dark, and 0 is the black the scattering model on its own
// actually implies. Menu row "Night brightness", or --night-brightness.
constexpr float kNightBrightness = 1.00f;

// The three settings added on 2026-09-06, all OFF so that every screenshot and
// every tuning decision taken before that date still reproduces exactly. Turn
// them on here, or with --blue-noise / --auto-exposure / --bloom, or from the Y
// menu (blue noise only -- the other two are command line and bake) and bake.
constexpr bool kBlueNoise = false;
constexpr bool kAutoExposure = false;
constexpr float kBloom = 0.0f;

// HOW LOUD THE WOOD IS, as a master gain over the ambience bed -- what
// actually reaches the voice is this times the canopy closure at your feet.
// 1.00 is the bed at the level it was baked; a quarter of that is a
// background rather than a foreground, and it is where the Volume slider
// sits at its MIDPOINT. Menu row "Volume", or --ambience.
constexpr float kAmbience = 0.25f;

// HOW MUCH THE THING IN YOUR HAND MOVES as you walk -- a gain over the
// stride and the breath in render/helditem.h, not a speed and not a shape.
// 1.00 is the look those constants describe, so 2.00 is twice it; 0 nails
// the tool to its pose for a reference screenshot. No menu row -- this one
// is --hand-sway and a bake, as kBloom and kAutoExposure are.
constexpr float kHandSway = 2.00f;

}  // namespace defaults
}  // namespace v2
