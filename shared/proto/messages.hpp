#pragma once
#include <cmath>
#include <cstdint>

namespace astrosim {

struct ControlMsg {
  float roll = 0, pitch = 0, yaw = 0, throttle = 0;  // each in [-1, 1]
};

enum StateFlags : uint8_t { kFailed = 1, kGzConnected = 2 };

struct StateMsg {
  double sim_time = 0;
  float pos[3] = {0, 0, 0};       // world (ENU), m
  float quat[4] = {0, 0, 0, 1};   // x y z w, body (FLU) to world
  float vel[3] = {0, 0, 0};       // world, m/s
  float ang_vel[3] = {0, 0, 0};   // body, rad/s
  uint8_t flags = 0;
  uint32_t echo_seq = 0;  // seq of the last Control the gateway applied
};

struct FaultMsg {
  uint8_t motor_index = 0;
};

inline float sanitizeAxis(float v) {
  if (!std::isfinite(v)) return 0.f;
  return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
}

inline ControlMsg sanitize(const ControlMsg& c) {
  return {sanitizeAxis(c.roll), sanitizeAxis(c.pitch), sanitizeAxis(c.yaw), sanitizeAxis(c.throttle)};
}

}  // namespace astrosim
