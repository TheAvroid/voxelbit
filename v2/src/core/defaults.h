// ---------------------------------------------------------------------------
// defaults.h -- the settings v2 starts with.
//
// GENERATED FILE. Everything below is rewritten wholesale by the "Bake as
// default" row in the in-viewer settings menu (Y), so hand edits survive only
// until the next bake -- but hand edits are perfectly fine, the format is just
// constants and the file is checked in.
//
// The point of it is that the settings menu and the command line stop being
// separate universes: fly around, tune the picture until it looks right, bake,
// rebuild, and the thing you tuned is what v2 opens with.
//
// A bake writes SOURCE, not a config file, deliberately. A config read at
// startup would be one more thing that can be stale, missing, or disagree with
// the flags; a header means the defaults are visible in the diff, travel with
// the branch, and cost nothing at runtime.
// ---------------------------------------------------------------------------
#pragma once

namespace v2 {
namespace defaults {

constexpr float kScale = 0.70f;
constexpr int kDepth = 6;
constexpr int kMovingDepth = 4;
constexpr float kExposure = 1.25f;
constexpr float kEye = 1.80f;   // 18 voxels
constexpr float kSpeed = 9.2f;
constexpr float kFov = 50.0f;
constexpr float kSunAz = 38.0f;
constexpr float kSunEl = 24.0f;
constexpr int kWidth = 3820;
constexpr int kHeight = 1990;
constexpr int kTrees = 900;
constexpr float kTimeOfDay = 0.3333f;  // 08:00 -- reproduces v4 sun
constexpr float kCycleSpeed = 1.0f;

}  // namespace defaults
}  // namespace v2
