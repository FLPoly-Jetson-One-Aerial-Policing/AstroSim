/**
 * @file binding.hpp
 * @brief Binds the protocol's eight motors to the aircraft file's motor data (gz-free).
 */
#pragma once
#include <array>

#include "astrosim/aircraft/aircraft.hpp"
#include "astrosim/proto/messages.hpp"
#include "config.hpp"

namespace astrosim::gateway {

/** @brief Per-motor scaling taken from the aircraft file. Protocol motor i drives actuator index i. */
struct MotorBinding {
  std::array<double, proto::kNumMotors> max_rad_s;  ///< Rotor speed at power 1.0 for each motor.
  double motor_speed_scale;  ///< Joint velocity times this = rotor rad/s (rotorVelocitySlowdownSim).
};

/**
 * @brief Derive the motor binding from an aircraft.
 * @param a Validated aircraft.
 * @return max_rad_s[i] = rotor type's max_rot_velocity_rad_s of motor i; scale = simulation.rotor_velocity_slowdown.
 * @throws ConfigError if the aircraft does not have exactly proto::kNumMotors motors.
 */
MotorBinding bindAircraft(const aircraft::Aircraft& a);

}  // namespace astrosim::gateway
