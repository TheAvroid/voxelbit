// ---------------------------------------------------------------------------
// defaults.h -- the settings v7 starts with.
//
// GENERATED FILE. Everything below is rewritten wholesale by "Bake as
// default" in the in-viewer settings menu (Y), so hand edits survive only
// until the next bake -- but hand edits are perfectly fine, the format is just
// constants and the file is checked in.
//
// The point of it is that the settings menu and the command line stop being
// separate universes: fly around, tune the picture until it looks right, bake,
// rebuild, and the thing you tuned is what v7 opens with.
// ---------------------------------------------------------------------------
#pragma once

namespace v7 {
namespace defaults {

constexpr float kScale = 1.00f;
constexpr int kDepth = 6;
constexpr int kMovingDepth = 4;
constexpr float kExposure = 0.70f;
constexpr float kShadowLift = 0.140f;
constexpr float kSpeed = 9.2f;
constexpr float kSensitivity = 0.12f;
constexpr float kEye = 2.00f;
constexpr float kFov = 90.0f;
constexpr float kSunAz = 6.9f;
constexpr float kSunEl = 46.6f;
constexpr int kWidth = 3820;
constexpr int kHeight = 1990;
constexpr int kTrees = 900;
constexpr float kTimeOfDay = 0.5388f;  // 12:55
constexpr float kCycleSpeed = 1.00f;
constexpr bool kAtmosphere = true;

}  // namespace defaults
}  // namespace v7
