/// @file
/// @brief Tests for ScriptedPilot (the SPOOFED pilot).
#include <gtest/gtest.h>

#include <stdexcept>

#include "astrosim/fc/scripted_pilot.hpp"

namespace astrosim::fc {
namespace {

/// @brief Build a keyframe.
Keyframe key(double t, float roll, float pitch, float yaw, float throttle, FlightMode mode, bool arm) {
  return {t, PilotInput{roll, pitch, yaw, throttle, mode, arm}};
}

/// @brief A two-keyframe script from disarmed to armed climb.
std::vector<Keyframe> script() {
  return {key(1.0, 0.f, 0.f, 0.f, 0.0f, FlightMode::kGpsHold, true),
          key(3.0, 0.5f, -0.5f, 1.f, 1.0f, FlightMode::kNoGpsHold, true)};
}

/// @brief The pilot reports itself as spoofed.
TEST(ScriptedPilot, IsSpoofed) {
  EXPECT_TRUE(ScriptedPilot({}).isSpoofed());
}

/// @brief Before the first keyframe the command is the default: disarmed, zero sticks, GPS hold.
TEST(ScriptedPilot, DefaultBeforeFirstKeyframe) {
  const ScriptedPilot p(script());
  EXPECT_EQ(p.sample(-5.0), PilotInput{});
  EXPECT_EQ(p.sample(0.99), PilotInput{});
  EXPECT_EQ(ScriptedPilot({}).sample(10.0), PilotInput{});
}

/// @brief Sample-and-hold keeps each keyframe until the next one takes effect, then holds the last forever.
TEST(ScriptedPilot, SampleAndHold) {
  const ScriptedPilot p(script(), Interpolation::kHold);
  EXPECT_EQ(p.sample(1.0), script()[0].input);
  EXPECT_EQ(p.sample(2.99), script()[0].input);
  EXPECT_EQ(p.sample(3.0), script()[1].input);
  EXPECT_EQ(p.sample(1000.0), script()[1].input);
}

/// @brief Linear mode ramps the continuous axes while mode and arm hold.
TEST(ScriptedPilot, LinearInterpolation) {
  const ScriptedPilot p(script(), Interpolation::kLinear);
  const PilotInput mid = p.sample(2.0);
  EXPECT_FLOAT_EQ(mid.roll, 0.25f);
  EXPECT_FLOAT_EQ(mid.pitch, -0.25f);
  EXPECT_FLOAT_EQ(mid.yaw, 0.5f);
  EXPECT_FLOAT_EQ(mid.throttle, 0.5f);
  EXPECT_EQ(mid.mode, FlightMode::kGpsHold);  // mode holds until the next keyframe
  EXPECT_TRUE(mid.arm);
  EXPECT_EQ(p.sample(3.0), script()[1].input);  // at and after the last keyframe: hold
  EXPECT_EQ(p.sample(9.0), script()[1].input);
}

/// @brief Times must be finite and strictly increasing.
TEST(ScriptedPilot, RejectsBadTimes) {
  EXPECT_THROW(ScriptedPilot({key(1, 0, 0, 0, 0, FlightMode::kGpsHold, false),
                              key(1, 0, 0, 0, 0, FlightMode::kGpsHold, false)}),
               std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(2, 0, 0, 0, 0, FlightMode::kGpsHold, false),
                              key(1, 0, 0, 0, 0, FlightMode::kGpsHold, false)}),
               std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(std::nan(""), 0, 0, 0, 0, FlightMode::kGpsHold, false)}), std::invalid_argument);
}

/// @brief Axis values must be in range and finite.
TEST(ScriptedPilot, RejectsBadAxes) {
  EXPECT_THROW(ScriptedPilot({key(0, 1.5f, 0, 0, 0, FlightMode::kGpsHold, false)}), std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(0, 0, -1.5f, 0, 0, FlightMode::kGpsHold, false)}), std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(0, 0, 0, 2.f, 0, FlightMode::kGpsHold, false)}), std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(0, 0, 0, 0, -0.1f, FlightMode::kGpsHold, false)}), std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(0, 0, 0, 0, 1.1f, FlightMode::kGpsHold, false)}), std::invalid_argument);
  EXPECT_THROW(ScriptedPilot({key(0, std::nanf(""), 0, 0, 0, FlightMode::kGpsHold, false)}), std::invalid_argument);
}

/// @brief There are exactly two flight modes.
TEST(FlightModeContract, ExactlyTwoModes) {
  EXPECT_EQ(static_cast<int>(FlightMode::kGpsHold), 0);
  EXPECT_EQ(static_cast<int>(FlightMode::kNoGpsHold), 1);
}

}  // namespace
}  // namespace astrosim::fc
