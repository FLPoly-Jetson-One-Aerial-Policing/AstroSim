/**
 * @file test_binding.cpp
 * @brief Tests of the aircraft to protocol motor binding.
 */
#include <gtest/gtest.h>

#include <string>

#include "astrosim/aircraft/aircraft.hpp"
#include "binding.hpp"

namespace astrosim::gateway {
namespace {

/** @brief The shipped aircraft file. */
aircraft::Aircraft shipped() {
  return aircraft::loadAircraft(ASTROSIM_AIRCRAFT_TOML);
}

TEST(Binding, MapsShippedFile) {
  const aircraft::Aircraft a = shipped();
  const MotorBinding b = bindAircraft(a);
  for (std::size_t i = 0; i < b.max_rad_s.size(); ++i) {
    EXPECT_EQ(b.max_rad_s[i], a.rotorTypeOf(a.motors[i]).max_rot_velocity_rad_s);
  }
  EXPECT_EQ(b.motor_speed_scale, a.simulation.rotor_velocity_slowdown);
}

TEST(Binding, PerMotorSpeedsFollowRotorType) {
  aircraft::Aircraft a = shipped();
  a.rotor_types[1].max_rot_velocity_rad_s = 123.0;  // "lower" type: motors 4-7
  const MotorBinding b = bindAircraft(a);
  EXPECT_EQ(b.max_rad_s[0], 500.0);
  EXPECT_EQ(b.max_rad_s[4], 123.0);
  EXPECT_EQ(b.max_rad_s[7], 123.0);
}

TEST(Binding, WrongMotorCountRejected) {
  aircraft::Aircraft a = shipped();
  a.motors.pop_back();
  try {
    bindAircraft(a);
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("motor"), std::string::npos);
  }
  a = shipped();
  a.motors.push_back(a.motors.front());
  EXPECT_THROW(bindAircraft(a), ConfigError);
}

}  // namespace
}  // namespace astrosim::gateway
