// ---------------------------------------------------------------------------
// defaults.h -- the settings v6 starts with.
//
// GENERATED FILE. Everything below is rewritten wholesale by "Bake as
// default" in the in-viewer settings menu (Y), so hand edits survive only
// until the next bake -- but hand edits are perfectly fine, the format is just
// constants and the file is checked in.
//
// The point of it is that the settings menu and the command line stop being
// separate universes: fly around, tune the picture until it looks right, bake,
// rebuild, and the thing you tuned is what v6 opens with.
// ---------------------------------------------------------------------------
#pragma once

namespace v6 {
namespace defaults {

constexpr float kScale = 1.00f;
constexpr int kDepth = 6;
constexpr int kMovingDepth = 4;
constexpr float kExposure = 1.25f;
constexpr float kShadowLift = 0.140f;
constexpr float kSpeed = 9.2f;
constexpr float kSensitivity = 0.120f;
constexpr float kEye = 2.00f;
constexpr float kFov = 80.0f;
constexpr float kSunAz = 31.4f;
constexpr float kSunEl = 31.5f;
constexpr int kWidth = 3820;
constexpr int kHeight = 1990;
constexpr int kTrees = 900;
constexpr float kTimeOfDay = 0.6361f;  // 15:15
constexpr float kCycleSpeed = 1.00f;

}  // namespace defaults
}  // namespace v6
