/// @file
/// @brief Tests for TemporarySynthSensors (SYNTHESIZED, TEMPORARY sensors).
#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include "astrosim/fc/synth_sensors_TEMPORARY.hpp"

namespace astrosim::fc {
namespace {

constexpr double kG = 9.80665;
constexpr double kH = 0.70710678118654752440;

/// @brief A State at @p t with the given velocity, level attitude and position.
proto::StateMsg state(double t, std::array<float, 3> vel = {0, 0, 0}, std::array<double, 3> pos = {0, 0, 0}) {
  proto::StateMsg s;
  s.sim_time = t;
  s.vel = vel;
  s.pos = pos;
  return s;
}

/// @brief Feed two states and return the frame of the second.
SensorFrame second(TemporarySynthSensors& sensors, const proto::StateMsg& a, const proto::StateMsg& b) {
  EXPECT_FALSE(sensors.onState(a).has_value());
  const auto f = sensors.onState(b);
  EXPECT_TRUE(f.has_value());
  return f.value_or(SensorFrame{});
}

/// @brief The source reports itself as synthesized.
TEST(TemporarySynthSensors, IsSynthesized) {
  EXPECT_TRUE(TemporarySynthSensors(SynthConfig{}).isSynthesized());
}

/// @brief Hover: specific force is (0, 0, +g).
TEST(TemporarySynthSensors, HoverSpecificForce) {
  TemporarySynthSensors s(SynthConfig{});
  const auto f = second(s, state(1.0), state(1.004));
  EXPECT_NEAR(f.accel[0], 0, 1e-9);
  EXPECT_NEAR(f.accel[1], 0, 1e-9);
  EXPECT_NEAR(f.accel[2], kG, 1e-9);
  EXPECT_EQ(f.gyro, (Vec3{0, 0, 0}));
  EXPECT_DOUBLE_EQ(f.time_s, 1.004);
}

/// @brief Free fall: specific force is about zero.
TEST(TemporarySynthSensors, FreeFallSpecificForceIsZero) {
  TemporarySynthSensors s(SynthConfig{});
  const double dt = 0.004;
  const auto f = second(s, state(1.0), state(1.0 + dt, {0, 0, static_cast<float>(-kG * dt)}));
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(f.accel[i], 0, 1e-4);
}

/// @brief A tilted vehicle reads gravity along its own axes (rolled 90 degrees: +g along body y).
TEST(TemporarySynthSensors, SpecificForceIsInBodyAxes) {
  TemporarySynthSensors s(SynthConfig{});
  proto::StateMsg a = state(0), b = state(0.01);
  // Roll +90 degrees about the body x axis: body y (left) points Up? Rotating y by +90 about x gives z.
  a.quat = b.quat = {static_cast<float>(kH), 0, 0, static_cast<float>(kH)};
  const auto f = second(s, a, b);
  EXPECT_NEAR(f.accel[0], 0, 1e-6);
  EXPECT_NEAR(f.accel[1], kG, 1e-5);  // world Up expressed in this body frame is body +y
  EXPECT_NEAR(f.accel[2], 0, 1e-5);
}

/// @brief The finite difference is low-passed: a step in acceleration approaches its value gradually.
TEST(TemporarySynthSensors, AccelerationIsLowPassed) {
  SynthConfig c;
  c.accel_filter_tau_s = 0.1;
  TemporarySynthSensors s(c);
  EXPECT_FALSE(s.onState(state(0.0)).has_value());
  s.onState(state(0.01));                               // initializes the filter at zero acceleration
  const auto f = s.onState(state(0.02, {0.1f, 0, 0}));  // raw 10 m/s^2 along East
  ASSERT_TRUE(f.has_value());
  EXPECT_GT(f->accel[0], 0.0);
  EXPECT_LT(f->accel[0], 10.0);
}

/// @brief Angular velocity passes straight through to the gyro.
TEST(TemporarySynthSensors, GyroFromAngularVelocity) {
  TemporarySynthSensors s(SynthConfig{});
  proto::StateMsg a = state(0), b = state(0.01);
  b.ang_vel = {0.1f, -0.2f, 0.3f};
  const auto f = second(s, a, b);
  EXPECT_FLOAT_EQ(static_cast<float>(f.gyro[0]), 0.1f);
  EXPECT_FLOAT_EQ(static_cast<float>(f.gyro[1]), -0.2f);
  EXPECT_FLOAT_EQ(static_cast<float>(f.gyro[2]), 0.3f);
}

/// @brief The configured world magnetic field is rotated into the body frame.
TEST(TemporarySynthSensors, MagInBodyFrame) {
  SynthConfig c;
  c.mag_field_enu_gauss = {0.0, 0.2, -0.4};
  TemporarySynthSensors s(c);
  const auto level = second(s, state(0), state(0.01));
  EXPECT_NEAR(level.mag[0], 0.0, 1e-9);
  EXPECT_NEAR(level.mag[1], 0.2, 1e-9);
  EXPECT_NEAR(level.mag[2], -0.4, 1e-9);

  // Yaw +90 degrees (forward North): body forward sees the North component.
  TemporarySynthSensors t(c);
  proto::StateMsg a = state(0), b = state(0.01);
  a.quat = b.quat = {0, 0, static_cast<float>(kH), static_cast<float>(kH)};
  const auto north = second(t, a, b);
  EXPECT_NEAR(north.mag[0], 0.2, 1e-6);
  EXPECT_NEAR(north.mag[1], 0.0, 1e-6);
  EXPECT_NEAR(north.mag[2], -0.4, 1e-6);
}

/// @brief GPS from a known offset around the configured home.
TEST(TemporarySynthSensors, GpsFromOffset) {
  SynthConfig c;
  c.home_lat_deg = 45.0;
  c.home_lon_deg = -122.0;
  c.home_alt_m = 100.0;
  TemporarySynthSensors s(c);
  // 111.2 km North and 78.7 km East is roughly one degree north and one degree east at 45 degrees latitude.
  const auto f = second(s, state(0), state(0.01, {1, 2, 3}, {78700.0, 111200.0, 50.0}));
  EXPECT_NEAR(f.gps.lat_deg, 46.0, 0.01);
  EXPECT_NEAR(f.gps.lon_deg, -121.0, 0.02);
  EXPECT_NEAR(f.gps.alt_m, 150.0, 1e-9);
  EXPECT_FLOAT_EQ(f.gps.vel_ned[0], 2.f);   // North
  EXPECT_FLOAT_EQ(f.gps.vel_ned[1], 1.f);   // East
  EXPECT_FLOAT_EQ(f.gps.vel_ned[2], -3.f);  // Down
  EXPECT_EQ(f.gps.fix, 3);
  EXPECT_EQ(f.gps.satellites, 12);
  ASSERT_TRUE(f.truth.has_value());
  EXPECT_EQ(f.truth->pos_enu, (Vec3{78700.0, 111200.0, 50.0}));
}

/// @brief At the origin the GPS is exactly the home position.
TEST(TemporarySynthSensors, GpsAtHome) {
  SynthConfig c;
  c.home_lat_deg = 28.5;
  c.home_lon_deg = -80.6;
  c.home_alt_m = 3.0;
  TemporarySynthSensors s(c);
  const auto f = second(s, state(0), state(0.01));
  EXPECT_DOUBLE_EQ(f.gps.lat_deg, 28.5);
  EXPECT_DOUBLE_EQ(f.gps.lon_deg, -80.6);
  EXPECT_DOUBLE_EQ(f.gps.alt_m, 3.0);
}

/// @brief Barometric pressure falls monotonically with altitude and matches the ISA at known points.
TEST(TemporarySynthSensors, BaroMonotonicInAltitude) {
  double previous = 1e9;
  for (double z : {-100.0, 0.0, 10.0, 100.0, 1000.0, 5000.0, 10000.0}) {
    TemporarySynthSensors s(SynthConfig{});
    const auto f = second(s, state(0, {0, 0, 0}, {0, 0, z}), state(0.01, {0, 0, 0}, {0, 0, z}));
    EXPECT_LT(f.baro_pressure_pa, previous) << "z=" << z;
    EXPECT_DOUBLE_EQ(f.baro_altitude_m, z);
    previous = f.baro_pressure_pa;
  }
  TemporarySynthSensors s(SynthConfig{});
  const auto sea = second(s, state(0), state(0.01));
  EXPECT_NEAR(sea.baro_pressure_pa, 101325.0, 1e-6);
  EXPECT_NEAR(sea.baro_temperature_c, 15.0, 1e-9);
  TemporarySynthSensors t(SynthConfig{});
  const auto at1km = second(t, state(0, {0, 0, 0}, {0, 0, 1000}), state(0.01, {0, 0, 0}, {0, 0, 1000}));
  EXPECT_NEAR(at1km.baro_pressure_pa, 89875.0, 100.0);  // ISA at 1000 m
}

/// @brief First sample, duplicate time and non-finite input give no frame; backwards time resets history.
TEST(TemporarySynthSensors, TimeHandling) {
  TemporarySynthSensors s(SynthConfig{});
  EXPECT_FALSE(s.onState(state(5.0)).has_value());  // first sample
  EXPECT_FALSE(s.onState(state(5.0)).has_value());  // duplicate timestamp
  EXPECT_TRUE(s.onState(state(5.01)).has_value());
  EXPECT_FALSE(s.onState(state(1.0)).has_value());  // backwards: reset, new baseline
  EXPECT_TRUE(s.onState(state(1.01)).has_value());  // history restarted from 1.0
  proto::StateMsg bad = state(2.0);
  bad.vel[0] = std::nanf("");
  EXPECT_FALSE(s.onState(bad).has_value());
  EXPECT_TRUE(s.onState(state(1.02)).has_value());  // bad state did not disturb the baseline
}

/// @brief reset() forgets the baseline.
TEST(TemporarySynthSensors, Reset) {
  TemporarySynthSensors s(SynthConfig{});
  s.onState(state(0));
  EXPECT_TRUE(s.onState(state(0.01)).has_value());
  s.reset();
  EXPECT_FALSE(s.onState(state(0.02)).has_value());
}

/// @brief Invalid configuration is rejected.
TEST(TemporarySynthSensors, ConfigValidation) {
  SynthConfig c;
  c.home_lat_deg = 91;
  EXPECT_THROW(TemporarySynthSensors{c}, std::invalid_argument);
  c = SynthConfig{};
  c.home_lon_deg = -181;
  EXPECT_THROW(TemporarySynthSensors{c}, std::invalid_argument);
  c = SynthConfig{};
  c.accel_filter_tau_s = -1;
  EXPECT_THROW(TemporarySynthSensors{c}, std::invalid_argument);
  c = SynthConfig{};
  c.home_alt_m = std::nan("");
  EXPECT_THROW(TemporarySynthSensors{c}, std::invalid_argument);
}

}  // namespace
}  // namespace astrosim::fc
