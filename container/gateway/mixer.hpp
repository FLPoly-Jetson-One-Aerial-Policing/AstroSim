#pragma once
#include "messages.hpp"

namespace astrosim {

// Body-frame velocity command for the gz MulticopterVelocityControl (FLU: x forward, y left, z up).
struct VelCmd {
  double vx = 0, vy = 0, vz = 0, yaw_rate = 0;
};

constexpr double kMaxHoriz = 3.0;  // m/s
constexpr double kMaxVert = 2.0;   // m/s
constexpr double kMaxYaw = 1.0;    // rad/s

// pitch>0 forward, roll>0 right, yaw>0 clockwise (right turn), throttle>0 up. Input is sanitized here.
inline VelCmd mix(const ControlMsg& raw) {
  const ControlMsg c = sanitize(raw);
  return {c.pitch * kMaxHoriz, -c.roll * kMaxHoriz, c.throttle * kMaxVert, -c.yaw * kMaxYaw};
}

}  // namespace astrosim
