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

// Angle mode with momentum, approximated on top of gz's velocity controller (a stand-in until a real SITL flight
// computer). The pitch/roll stick tilts the thrust vector up to 25 deg, i.e. a heading-relative horizontal
// acceleration of g*tan(tilt); throttle stays a climb-rate command (centred holds altitude). Momentum and
// coasting come from the sim itself: airframe drag a = kDragLin*v + kDragQuad*v*|v| is applied in gz
// (Hydrodynamics plugin in worlds/poc.sdf), sized so full tilt balances it at the governed top speed.
constexpr double kTiltAccel = 9.81 * 0.4663076581549986;  // g * tan(25 deg), m/s^2
constexpr double kDragLin = 0.08;                          // 1/s: low-speed coast-down time constant 12.5 s
constexpr double kDragQuad = (kTiltAccel - kDragLin * kMaxHoriz) / (kMaxHoriz * kMaxHoriz);  // 1/m: terminal = kMaxHoriz
// The controller's accel = velocityGain/mass * (command - velocity) (worlds/poc.sdf: 2.7 / 1.5 kg), so commanding
// velocity + a/kCtrlGain asks it for exactly a: the tilt follows the stick, and a centred stick asks for nothing
// (level, coasting under drag) rather than braking to a hover.
constexpr double kCtrlGain = 2.7 / 1.5;

inline double dragAccel(double v) { return kDragLin * v + kDragQuad * v * std::fabs(v); }

// World-frame (ENU) velocity command from the sticks, the heading (yaw, rad) and the current world velocity.
inline VelCmd fly(const ControlMsg& raw, double yaw, const float vel[3]) {
  const ControlMsg c = sanitize(raw);
  double fx = c.pitch, fy = -c.roll, f = std::hypot(fx, fy);  // heading frame, FLU
  if (f > 1) { fx /= f; fy /= f; }
  const double cy = std::cos(yaw), sy = std::sin(yaw);
  double ax = kTiltAccel * (cy * fx - sy * fy), ay = kTiltAccel * (sy * fx + cy * fy);  // world
  // Governor: past top speed, push along the velocity only enough to hold against (model) drag, less a pull-back
  // proportional to the overspeed, so small sim/model drag differences cannot carry the craft past the limit.
  const double vx = std::isfinite(vel[0]) ? vel[0] : 0, vy = std::isfinite(vel[1]) ? vel[1] : 0;
  const double speed = std::hypot(vx, vy);
  if (speed >= kMaxHoriz) {
    const double ux = vx / speed, uy = vy / speed, along = ax * ux + ay * uy;
    const double cap = dragAccel(speed) - kCtrlGain * (speed - kMaxHoriz);
    if (along > cap) { ax -= (along - cap) * ux; ay -= (along - cap) * uy; }
  }
  return {vx + ax / kCtrlGain, vy + ay / kCtrlGain, c.throttle * kMaxVert, -c.yaw * kMaxYaw};
}

inline double yawOf(const float q[4]) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  return std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
}

// MulticopterVelocityControl reads its linear command in the body frame and rotates it by the full attitude, so a
// nose-down "forward" would also mean "down". Commands are world-frame: rotate them into the body at q
// (x y z w, FLU body in ENU world) so the controller's rotation lands them back in the world.
inline VelCmd worldToBody(const VelCmd& c, const float q[4]) {
  double x = q[0], y = q[1], z = q[2], w = q[3], n = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(n > 1e-9)) return c;  // no attitude yet
  x /= n; y /= n; z /= n; w /= n;
  const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                          {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                          {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  VelCmd out = c;
  out.vx = R[0][0] * c.vx + R[1][0] * c.vy + R[2][0] * c.vz;  // R^T v
  out.vy = R[0][1] * c.vx + R[1][1] * c.vy + R[2][1] * c.vz;
  out.vz = R[0][2] * c.vx + R[1][2] * c.vy + R[2][2] * c.vz;
  return out;
}

}  // namespace astrosim
