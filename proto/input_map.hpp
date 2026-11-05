#pragma once
// Pure input mapping (no raylib dependency). Stick convention is raylib's: axes in [-1,1], stick-up/left negative.
#include <algorithm>
#include <cmath>

#include "messages.hpp"

namespace astrosim {

constexpr float kStickDeadzone = 0.10f;

// Radial deadzone with rescale so the output still reaches +/-1 at full deflection. NaN -> (0,0).
inline void deadzone(float& x, float& y, float dz = kStickDeadzone) {
  if (!std::isfinite(x) || !std::isfinite(y)) { x = y = 0; return; }
  float mag = std::hypot(x, y);
  if (mag < dz) { x = y = 0; return; }
  float scale = (std::min(mag, 1.f) - dz) / (1.f - dz) / mag;
  x *= scale;
  y *= scale;
}

// Mode 2 layout: left stick Y = throttle (up climbs), left X = yaw (right = clockwise),
// right stick Y = pitch (up = forward), right X = roll (right = right).
inline ControlMsg mapGamepad(float lx, float ly, float rx, float ry) {
  deadzone(lx, ly);
  deadzone(rx, ry);
  return sanitize({rx, -ry, lx, -ly});  // {roll, pitch, yaw, throttle}
}

struct Keys {
  bool w = false, a = false, s = false, d = false;              // "left stick": W/S throttle, A/D yaw
  bool up = false, left = false, down = false, right = false;  // "right stick": up/down pitch, left/right roll
};

// Same layout as the gamepad: WASD = left stick (throttle/yaw), arrows = right stick (pitch/roll).
inline ControlMsg mapKeys(const Keys& k) {
  auto axis = [](bool pos, bool neg) { return (pos ? 1.f : 0.f) - (neg ? 1.f : 0.f); };
  return {axis(k.right, k.left), axis(k.up, k.down), axis(k.d, k.a), axis(k.w, k.s)};  // roll, pitch, yaw, throttle
}

}  // namespace astrosim
