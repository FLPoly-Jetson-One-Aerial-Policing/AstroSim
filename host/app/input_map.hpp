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

inline bool anyKey(const Keys& k) { return k.w || k.a || k.s || k.d || k.up || k.left || k.down || k.right; }
inline bool anyAxis(const ControlMsg& c) { return c.roll != 0 || c.pitch != 0 || c.yaw != 0 || c.throttle != 0; }

enum class InputSource { Keyboard, Gamepad };

// Last-touched device wins, so a connected but idle (or unmapped, all-zero) pad never locks out the keyboard.
// Keyboard wins ties; with nothing touched the previous choice holds.
inline InputSource pickSource(InputSource cur, bool keysActive, bool padActive, bool padPresent) {
  if (!padPresent || keysActive) return InputSource::Keyboard;
  if (padActive) return InputSource::Gamepad;
  return cur;
}

// Auto hover: a pilot assist that, when on and the pilot's pitch/roll stick is centred, flies that stick to bring
// horizontal velocity to zero (where the craft ends up is the pilot's business). Pitch/roll stick means a tilt /
// horizontal acceleration request, full deflection = kStickAccel (the gateway's 25 deg max tilt,
// container/gateway/mixer.hpp kTiltAccel). Vertical needs nothing: centred throttle already commands zero climb.
constexpr float kStickAccel = 4.5745f;  // m/s^2 at full pitch/roll stick
constexpr float kHoverGain = 1.0f;      // 1/s: requested deceleration per m/s (full stick above ~4.6 m/s)
constexpr float kHoverSettled = 0.1f;   // m/s: horizontal speed below this counts as hovering

struct HoverAssist {
  bool on = false;

  void toggle() { on = !on; }

  // On and actually hovering: horizontal speed (world, m/s) within kHoverSettled of zero. Vertical is the
  // throttle's (altitude hold) business, not the assist's, so it is not counted.
  bool settled(const float vel[3]) const {
    return on && std::hypot(vel[0], vel[1]) < kHoverSettled;
  }

  // Fills c.pitch / c.roll when active. vel is world (ENU), yaw the heading (rad, ENU). Returns whether it acted.
  bool apply(ControlMsg& c, const float vel[3], float yaw) const {
    if (!on || c.pitch != 0 || c.roll != 0) return false;
    const float ax = -kHoverGain * vel[0], ay = -kHoverGain * vel[1];
    // World acceleration -> heading-relative stick: pitch forward, roll right (= -left).
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    float fwd = (cy * ax + sy * ay) / kStickAccel, left = (-sy * ax + cy * ay) / kStickAccel;
    const float m = std::hypot(fwd, left);
    if (m > 1) { fwd /= m; left /= m; }
    c.pitch = fwd;
    c.roll = -left;
    return true;
  }
};

}  // namespace astrosim
