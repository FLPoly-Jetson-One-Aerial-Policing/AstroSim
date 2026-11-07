#pragma once
#include <cmath>

#include "messages.hpp"

namespace astrosim {

// Body-frame velocity command for the gz MulticopterVelocityControl (FLU: x forward, y left, z up).
struct VelCmd {
  double vx = 0, vy = 0, vz = 0, yaw_rate = 0;
};

constexpr double kMaxHoriz = 102.0 / 3.6;  // m/s; Jetson ONE governed top speed, 102 km/h
constexpr double kMaxVert = 2.0;   // m/s
constexpr double kMaxYaw = 1.0;    // rad/s

// pitch>0 forward, roll>0 right, yaw>0 clockwise (right turn), throttle>0 up. Input is sanitized here.
// The horizontal command is clamped as a vector, so a diagonal stick cannot exceed the governed speed.
inline VelCmd mix(const ControlMsg& raw) {
  const ControlMsg c = sanitize(raw);
  double vx = c.pitch * kMaxHoriz, vy = -c.roll * kMaxHoriz, h = std::hypot(vx, vy);
  if (h > kMaxHoriz) { vx *= kMaxHoriz / h; vy *= kMaxHoriz / h; }
  return {vx, vy, c.throttle * kMaxVert, -c.yaw * kMaxYaw};
}

// MulticopterVelocityControl reads its linear command in the body frame and rotates it by the full attitude, so a
// nose-down "forward" would also mean "down" (at 102 km/h and 14 deg pitch, ~7 m/s of sink). Sticks mean level
// heading-relative velocity: rotate the command by heading (yaw only) into the world, then into the body at q
// (x y z w, FLU body in ENU world), which the controller's rotation undoes.
inline VelCmd headingToBody(const VelCmd& c, const float q[4]) {
  double x = q[0], y = q[1], z = q[2], w = q[3], n = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(n > 1e-9)) return c;  // no attitude yet
  x /= n; y /= n; z /= n; w /= n;
  const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                          {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                          {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  const double yaw = std::atan2(R[1][0], R[0][0]), cy = std::cos(yaw), sy = std::sin(yaw);
  const double v[3] = {cy * c.vx - sy * c.vy, sy * c.vx + cy * c.vy, c.vz};  // world
  VelCmd out = c;
  out.vx = R[0][0] * v[0] + R[1][0] * v[1] + R[2][0] * v[2];  // R^T v
  out.vy = R[0][1] * v[0] + R[1][1] * v[1] + R[2][1] * v[2];
  out.vz = R[0][2] * v[0] + R[1][2] * v[1] + R[2][2] * v[2];
  return out;
}

}  // namespace astrosim
