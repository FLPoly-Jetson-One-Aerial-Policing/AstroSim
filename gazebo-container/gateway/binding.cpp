/** @file binding.cpp @brief Aircraft to protocol motor binding. */
#include "binding.hpp"

#include <format>

namespace astrosim::gateway {

MotorBinding bindAircraft(const aircraft::Aircraft& a) {
  if (a.motors.size() != proto::kNumMotors) {
    throw ConfigError(std::format("motor: the aircraft file has {} [[motor]] entries but the protocol needs exactly {}",
                                  a.motors.size(), proto::kNumMotors));
  }
  MotorBinding b{};
  for (std::size_t i = 0; i < b.max_rad_s.size(); ++i) {
    b.max_rad_s[i] = a.rotorTypeOf(a.motors[i]).max_rot_velocity_rad_s;
  }
  b.motor_speed_scale = a.simulation.rotor_velocity_slowdown;
  return b;
}

}  // namespace astrosim::gateway
