/** @file test_px4_telemetry.cpp @brief Telemetry assembly from canned MAVLink messages. */
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

#include "px4_telemetry.hpp"

using namespace astrosim;
using namespace astrosim::px4;

namespace {

const fc::TimePoint kT0 = fc::TimePoint{} + std::chrono::seconds(100);

mavlink_message_t heartbeat(std::uint8_t sys, std::uint8_t autopilot, bool armed, const Px4Mode& mode) {
  mavlink_heartbeat_t h{};
  h.autopilot = autopilot;
  h.type = MAV_TYPE_OCTOROTOR;
  h.base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | (armed ? kModeFlagArmed : 0);
  h.custom_mode = customModeOf(mode);
  mavlink_message_t m;
  mavlink_msg_heartbeat_encode(sys, 1, &m, &h);
  return m;
}

mavlink_message_t attitude(float roll, float pitch, float yaw) {
  mavlink_attitude_t a{};
  a.roll = roll;
  a.pitch = pitch;
  a.yaw = yaw;
  mavlink_message_t m;
  mavlink_msg_attitude_encode(1, 1, &m, &a);
  return m;
}

mavlink_message_t globalPosition(std::int32_t alt_mm, std::int32_t rel_mm, std::int16_t vx_cms, std::int16_t vy_cms) {
  mavlink_global_position_int_t p{};
  p.alt = alt_mm;
  p.relative_alt = rel_mm;
  p.vx = vx_cms;
  p.vy = vy_cms;
  mavlink_message_t m;
  mavlink_msg_global_position_int_encode(1, 1, &m, &p);
  return m;
}

mavlink_message_t vfrHud(float groundspeed, float climb) {
  mavlink_vfr_hud_t v{};
  v.groundspeed = groundspeed;
  v.climb = climb;
  mavlink_message_t m;
  mavlink_msg_vfr_hud_encode(1, 1, &m, &v);
  return m;
}

mavlink_message_t gpsRaw(std::uint8_t fix, std::uint8_t sats) {
  mavlink_gps_raw_int_t g{};
  g.fix_type = fix;
  g.satellites_visible = sats;
  mavlink_message_t m;
  mavlink_msg_gps_raw_int_encode(1, 1, &m, &g);
  return m;
}

mavlink_message_t sysStatus(std::uint16_t mv, std::int8_t pct, bool rc_present, bool rc_healthy) {
  mavlink_sys_status_t s{};
  s.voltage_battery = mv;
  s.battery_remaining = pct;
  const std::uint32_t rc = MAV_SYS_STATUS_SENSOR_RC_RECEIVER;
  if (rc_present) s.onboard_control_sensors_present = rc, s.onboard_control_sensors_enabled = rc;
  if (rc_healthy) s.onboard_control_sensors_health = rc;
  mavlink_message_t m;
  mavlink_msg_sys_status_encode(1, 1, &m, &s);
  return m;
}

mavlink_message_t estimator(std::uint16_t flags) {
  mavlink_estimator_status_t e{};
  e.flags = flags;
  mavlink_message_t m;
  mavlink_msg_estimator_status_encode(1, 1, &m, &e);
  return m;
}

constexpr std::uint16_t kHealthyFlags = 1 | 2 | 4 | 8 | 16 | 32;  // attitude, velocity h/v, position h rel/abs, v abs

/// @brief A telemetry object that has seen a complete, healthy set of messages.
Px4Telemetry healthy() {
  Px4Telemetry t;
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, true, {3, 0}), kT0);
  t.onMessage(attitude(0.1f, -0.2f, -1.5707963f), kT0);
  t.onMessage(globalPosition(498000, 10000, 100, 0), kT0);
  t.onMessage(vfrHud(1.0f, 1.5f), kT0);
  t.onMessage(gpsRaw(3, 12), kT0);
  t.onMessage(sysStatus(16000, 80, true, true), kT0);
  t.onMessage(estimator(kHealthyFlags), kT0);
  return t;
}

}  // namespace

TEST(Px4Telemetry, AssemblesAllFields) {
  Px4Telemetry t = healthy();
  t.setSimTime(42.5);
  t.setMotors({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f});
  const fc::FcTelemetry x = t.build(true, false);
  EXPECT_DOUBLE_EQ(x.time_s, 42.5);
  EXPECT_TRUE(x.connected);
  EXPECT_TRUE(x.armed);
  EXPECT_FALSE(x.mode_refused);
  EXPECT_EQ(x.mode, fc::FlightMode::kGpsHold);
  EXPECT_EQ(x.gps_fix, proto::kGps3dFix);
  EXPECT_EQ(x.gps_sats, 12);
  EXPECT_NEAR(x.roll_rad, 0.1f, 1e-6);
  EXPECT_NEAR(x.pitch_rad, -0.2f, 1e-6);
  EXPECT_NEAR(x.heading_rad, 3 * 3.14159265f / 2, 1e-4);  // yaw -90 degrees is heading 270 degrees
  EXPECT_NEAR(x.alt_msl_m, 498.0f, 1e-3);
  EXPECT_NEAR(x.alt_agl_m, 10.0f, 1e-3);
  EXPECT_NEAR(x.vspeed_mps, 1.5f, 1e-6);
  EXPECT_NEAR(x.ground_speed_mps, 1.0f, 1e-6);
  EXPECT_NEAR(x.course_rad, 0.0f, 1e-6);  // moving north
  EXPECT_NEAR(x.battery_v, 16.0f, 1e-6);
  EXPECT_NEAR(x.battery_pct, 80.0f, 1e-6);
  EXPECT_EQ(x.motors[7], 0.8f);
  EXPECT_EQ(x.warnings, 0);
  EXPECT_EQ(x.provenance, 0);  // the bridge sets provenance
}

TEST(Px4Telemetry, CourseFollowsVelocityAndHoldsWhenSlow) {
  Px4Telemetry t = healthy();
  t.onMessage(globalPosition(498000, 10000, 0, 200), kT0);  // east at 2 m/s
  EXPECT_NEAR(t.build(true, false).course_rad, 3.14159265f / 2, 1e-5);
  t.onMessage(globalPosition(498000, 10000, 0, 10), kT0);  // 0.1 m/s: course unreliable, keep the old one
  EXPECT_NEAR(t.build(true, false).course_rad, 3.14159265f / 2, 1e-5);
}

TEST(Px4Telemetry, ModeMapping) {
  Px4Telemetry t;
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, false, {2, 0}), kT0);  // ALTCTL
  EXPECT_EQ(t.build(true, false).mode, fc::FlightMode::kNoGpsHold);
  EXPECT_FALSE(t.build(true, false).armed);
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, false, {3, 0}), kT0);  // POSCTL
  EXPECT_EQ(t.build(true, false).mode, fc::FlightMode::kGpsHold);
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, false, {7, 0}), kT0);  // STABILIZED
  EXPECT_EQ(t.build(true, false).mode, fc::FlightMode::kNoGpsHold);
}

TEST(Px4Telemetry, UnrepresentableModeKeepsTheLastOneAndPassesRefusalThrough) {
  Px4Telemetry t;
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, true, {2, 0}), kT0);
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, true, {4, 6}), kT0);  // PX4 fell into auto land
  ASSERT_TRUE(t.activeMode().has_value());
  EXPECT_EQ(*t.activeMode(), (Px4Mode{4, 6}));
  const fc::FcTelemetry x = t.build(true, true);
  EXPECT_EQ(x.mode, fc::FlightMode::kNoGpsHold);  // not silently relabelled
  EXPECT_TRUE(x.mode_refused);
}

TEST(Px4Telemetry, IgnoresOtherSystemsAndOtherAutopilots) {
  Px4Telemetry t;
  t.onMessage(heartbeat(2, MAV_AUTOPILOT_PX4, true, {3, 0}), kT0);      // another system
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_INVALID, true, {3, 0}), kT0);  // a non-autopilot component of PX4's system
  EXPECT_FALSE(t.heartbeatSeen());
  EXPECT_FALSE(t.build(true, false).armed);
  mavlink_attitude_t a{};
  a.roll = 1;
  mavlink_message_t m;
  mavlink_msg_attitude_encode(7, 1, &m, &a);
  t.onMessage(m, kT0);
  EXPECT_EQ(t.build(true, false).roll_rad, 0.0f);
}

TEST(Px4Telemetry, HeartbeatBookkeeping) {
  Px4Telemetry t;
  EXPECT_FALSE(t.heartbeatSeen());
  EXPECT_FALSE(t.activeMode().has_value());
  t.onMessage(heartbeat(1, MAV_AUTOPILOT_PX4, true, {3, 0}), kT0);
  EXPECT_TRUE(t.heartbeatSeen());
  EXPECT_EQ(t.lastHeartbeat(), kT0);
  EXPECT_TRUE(t.armed());
}

TEST(Px4Telemetry, GpsFixMapping) {
  Px4Telemetry t = healthy();
  t.onMessage(gpsRaw(2, 5), kT0);
  EXPECT_EQ(t.build(true, false).gps_fix, proto::kGps2dFix);
  EXPECT_EQ(t.build(true, false).warnings & proto::kWarnGpsLost, proto::kWarnGpsLost);
  t.onMessage(gpsRaw(1, 0), kT0);  // MAVLink fix 1 is "no fix"
  EXPECT_EQ(t.build(true, false).gps_fix, proto::kGpsNoFix);
  t.onMessage(gpsRaw(6, 20), kT0);  // RTK fixed: a 3D fix for our purposes
  EXPECT_EQ(t.build(true, false).gps_fix, proto::kGps3dFix);
  EXPECT_EQ(t.build(true, false).warnings & proto::kWarnGpsLost, 0);
}

TEST(Px4Telemetry, WarningsBeforeAnyData) {
  const fc::FcTelemetry x = Px4Telemetry{}.build(false, false);
  EXPECT_FALSE(x.connected);
  EXPECT_NE(x.warnings & proto::kWarnGpsLost, 0);
  EXPECT_NE(x.warnings & proto::kWarnEkfUnhealthy, 0);
}

TEST(Px4Telemetry, EkfWarning) {
  Px4Telemetry t = healthy();
  t.onMessage(estimator(1 | 2), kT0);  // attitude and horizontal velocity only: no vertical position
  EXPECT_NE(t.build(true, false).warnings & proto::kWarnEkfUnhealthy, 0);
  t.onMessage(estimator(kHealthyFlags | 1024), kT0);  // GPS glitch
  EXPECT_NE(t.build(true, false).warnings & proto::kWarnEkfUnhealthy, 0);
  t.onMessage(estimator(kHealthyFlags), kT0);
  EXPECT_EQ(t.build(true, false).warnings & proto::kWarnEkfUnhealthy, 0);
}

TEST(Px4Telemetry, PilotLostFromRcSensorHealth) {
  Px4Telemetry t = healthy();
  t.onMessage(sysStatus(16000, 80, true, false), kT0);
  EXPECT_NE(t.build(true, false).warnings & proto::kWarnPilotLost, 0);
  t.onMessage(sysStatus(16000, 80, false, false), kT0);  // no RC sensor at all: not "lost"
  EXPECT_EQ(t.build(true, false).warnings & proto::kWarnPilotLost, 0);
}

TEST(Px4Telemetry, BatteryWarningAndUnknownValues) {
  Px4Telemetry t = healthy();
  t.onMessage(sysStatus(14000, 10, true, true), kT0);
  EXPECT_NE(t.build(true, false).warnings & proto::kWarnLowBattery, 0);
  t.onMessage(sysStatus(UINT16_MAX, -1, true, true), kT0);  // unknown
  const fc::FcTelemetry x = t.build(true, false);
  EXPECT_EQ(x.warnings & proto::kWarnLowBattery, 0);
  EXPECT_EQ(x.battery_v, 0.0f);
  EXPECT_EQ(x.battery_pct, 0.0f);
}

TEST(Px4Telemetry, ResetForgetsEverything) {
  Px4Telemetry t = healthy();
  t.reset();
  EXPECT_FALSE(t.heartbeatSeen());
  EXPECT_FALSE(t.build(true, false).armed);
  EXPECT_EQ(t.build(true, false).gps_sats, 0);
}
