/** @file test_mavlink_codec.cpp @brief Tests of the pure MAVLink codec: frames, conversions, mode mapping. */
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "mavlink_codec.hpp"

using namespace astrosim;
using namespace astrosim::px4;

namespace {

/// @brief A hovering frame with distinct values on every axis.
fc::SensorFrame frame() {
  fc::SensorFrame f;
  f.time_s = 12.345678;
  f.gyro = {0.1, 0.2, 0.3};
  f.accel = {1.0, 2.0, 9.81};
  f.mag = {0.2, 0.05, -0.4};
  f.baro_pressure_pa = 101325.0;
  f.baro_altitude_m = 488.5;
  f.baro_temperature_c = 15.0;
  f.gps.lat_deg = 47.397742;
  f.gps.lon_deg = 8.545594;
  f.gps.alt_m = 498.0;
  f.gps.vel_ned = {1.0f, 2.0f, -0.5f};
  f.gps.fix = 3;
  f.gps.satellites = 12;
  return f;
}

/// @brief Encode one message with its own framing state and parse it back through a second link.
template <class Send>
mavlink_message_t roundTrip(Send send) {
  MavlinkLink tx(1, 51), rx(2, 2);
  send(tx);
  const auto msgs = rx.parse(tx.takeTx());
  EXPECT_EQ(msgs.size(), 1u);
  return msgs.empty() ? mavlink_message_t{} : msgs[0];
}

}  // namespace

TEST(HilSensor, FieldsAndFrames) {
  const mavlink_hil_sensor_t m = makeHilSensor(frame());
  EXPECT_EQ(m.time_usec, 12345678u);
  // FLU to FRD: y and z flip.
  EXPECT_FLOAT_EQ(m.xacc, 1.0f);
  EXPECT_FLOAT_EQ(m.yacc, -2.0f);
  EXPECT_FLOAT_EQ(m.zacc, -9.81f);  // specific force of a hovering aircraft: -g in FRD (down is positive)
  EXPECT_FLOAT_EQ(m.xgyro, 0.1f);
  EXPECT_FLOAT_EQ(m.ygyro, -0.2f);
  EXPECT_FLOAT_EQ(m.zgyro, -0.3f);
  EXPECT_FLOAT_EQ(m.xmag, 0.2f);
  EXPECT_FLOAT_EQ(m.ymag, -0.05f);
  EXPECT_FLOAT_EQ(m.zmag, 0.4f);
  EXPECT_FLOAT_EQ(m.abs_pressure, 1013.25f);  // hPa
  EXPECT_FLOAT_EQ(m.pressure_alt, 488.5f);
  EXPECT_FLOAT_EQ(m.temperature, 15.0f);
  EXPECT_FLOAT_EQ(m.diff_pressure, 0.0f);
  EXPECT_EQ(m.id, 0);
}

TEST(HilSensor, FieldsUpdatedCoversWhatPx4Tests) {
  const mavlink_hil_sensor_t m = makeHilSensor(frame());
  // simulator_mavlink: ACCEL 0b111, GYRO 0b111000, MAG 0b111000000, BARO 0b1101000000000, DIFF_PRESS bit 10.
  EXPECT_EQ(m.fields_updated & 0b111u, 0b111u);
  EXPECT_EQ(m.fields_updated & 0b111000u, 0b111000u);
  EXPECT_EQ(m.fields_updated & 0b111000000u, 0b111000000u);
  EXPECT_EQ(m.fields_updated & 0b1101000000000u, 0b1101000000000u);
  EXPECT_EQ(m.fields_updated & (1u << 10), 0u);
  EXPECT_EQ(m.fields_updated, 0x1BFFu);
}

TEST(HilSensor, TimeIsTheSimulationTimeAndNeverNegative) {
  fc::SensorFrame f = frame();
  f.time_s = 0.000004;
  EXPECT_EQ(makeHilSensor(f).time_usec, 4u);
  f.time_s = -1;
  EXPECT_EQ(makeHilSensor(f).time_usec, 0u);
  f.time_s = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(makeHilSensor(f).time_usec, 0u);
}

TEST(HilGps, UnitsAndFields) {
  const mavlink_hil_gps_t g = makeHilGps(frame());
  EXPECT_EQ(g.time_usec, 12345678u);
  EXPECT_EQ(g.fix_type, 3);
  EXPECT_EQ(g.lat, 473977420);
  EXPECT_EQ(g.lon, 85455940);
  EXPECT_EQ(g.alt, 498000);  // mm
  EXPECT_EQ(g.vn, 100);      // cm/s
  EXPECT_EQ(g.ve, 200);
  EXPECT_EQ(g.vd, -50);
  EXPECT_EQ(g.vel, 224);  // ground speed, sqrt(1 + 4) m/s
  EXPECT_EQ(g.satellites_visible, 12);
  EXPECT_EQ(g.cog, 6343);  // atan2(east 2, north 1) = 63.43 degrees clockwise from north
  EXPECT_EQ(g.eph, 30);
  EXPECT_EQ(g.epv, 50);
}

TEST(HilGps, CourseUnknownWhenSlow) {
  fc::SensorFrame f = frame();
  f.gps.vel_ned = {0.0f, 0.0f, 0.0f};
  EXPECT_EQ(makeHilGps(f).cog, 65535);
  f.gps.vel_ned = {-1.0f, 0.0f, 0.0f};  // flying south: 180 degrees
  EXPECT_EQ(makeHilGps(f).cog, 18000);
  f.gps.vel_ned = {0.0f, -1.0f, 0.0f};  // flying west: 270 degrees
  EXPECT_EQ(makeHilGps(f).cog, 27000);
}

TEST(HilStateQuaternion, RequiresTruth) {
  EXPECT_FALSE(makeHilStateQuaternion(frame()).has_value());
}

TEST(HilStateQuaternion, AttitudeVelocityAndPositionInNed) {
  fc::SensorFrame f = frame();
  fc::Truth t;
  t.quat = {0, 0, 0, 1};        // body FLU aligned with world ENU: the nose points East
  t.vel_enu = {2.0, 1.0, 0.5};  // east 2, north 1, up 0.5
  f.truth = t;
  const auto s = makeHilStateQuaternion(f);
  ASSERT_TRUE(s.has_value());
  // Nose East in NED is a yaw of +90 degrees: (w, x, y, z) = (cos 45, 0, 0, sin 45).
  EXPECT_NEAR(s->attitude_quaternion[0], std::sqrt(0.5), 1e-6);
  EXPECT_NEAR(s->attitude_quaternion[1], 0.0, 1e-6);
  EXPECT_NEAR(s->attitude_quaternion[2], 0.0, 1e-6);
  EXPECT_NEAR(s->attitude_quaternion[3], std::sqrt(0.5), 1e-6);
  EXPECT_EQ(s->vx, 100);  // north
  EXPECT_EQ(s->vy, 200);  // east
  EXPECT_EQ(s->vz, -50);  // down
  EXPECT_EQ(s->lat, 473977420);
  EXPECT_EQ(s->alt, 498000);
  EXPECT_NEAR(s->rollspeed, 0.1f, 1e-6);
  EXPECT_NEAR(s->pitchspeed, -0.2f, 1e-6);
}

TEST(ManualControl, ScalingAndSigns) {
  fc::PilotInput in;
  in.roll = 0.5f;
  in.pitch = 0.25f;  // nose up
  in.yaw = -1.0f;
  in.throttle = 0.7f;
  const mavlink_manual_control_t m = makeManualControl(in);
  EXPECT_EQ(m.target, kPx4SystemId);
  EXPECT_EQ(m.y, 500);   // roll right positive
  EXPECT_EQ(m.x, -250);  // PX4: forward stick (positive x) is nose DOWN, so nose up is negative
  EXPECT_EQ(m.r, -1000);
  EXPECT_EQ(m.z, 700);  // throttle 0..1000, 500 neutral
}

TEST(ManualControl, ThrottleRangeAndClamping) {
  fc::PilotInput in;
  in.throttle = 0.0f;
  EXPECT_EQ(makeManualControl(in).z, 0);
  in.throttle = 0.5f;
  EXPECT_EQ(makeManualControl(in).z, 500);
  in.throttle = 1.0f;
  EXPECT_EQ(makeManualControl(in).z, 1000);
  in.throttle = 3.0f;
  in.roll = -9.0f;
  in.pitch = 9.0f;
  const mavlink_manual_control_t m = makeManualControl(in);
  EXPECT_EQ(m.z, 1000);
  EXPECT_EQ(m.y, -1000);
  EXPECT_EQ(m.x, -1000);
}

TEST(ManualControl, NonFiniteSticksAreNeutral) {
  fc::PilotInput in;
  in.roll = std::nanf("");
  in.pitch = std::numeric_limits<float>::infinity();
  in.yaw = std::nanf("");
  in.throttle = std::nanf("");
  const mavlink_manual_control_t m = makeManualControl(in);
  EXPECT_EQ(m.y, 0);
  EXPECT_EQ(m.x, 0);
  EXPECT_EQ(m.r, 0);
  EXPECT_EQ(m.z, 500);  // hold altitude
}

TEST(ActuatorControls, IdentityMapping) {
  mavlink_hil_actuator_controls_t a{};
  a.mode = kModeFlagArmed | 1;
  for (int i = 0; i < 8; ++i) a.controls[i] = 0.1f * static_cast<float>(i + 1);
  const fc::MotorCommand c = motorCommandFromActuatorControls(a);
  EXPECT_TRUE(c.armed);
  for (std::size_t i = 0; i < fc::kNumMotors; ++i) EXPECT_FLOAT_EQ(c.power[i], 0.1f * static_cast<float>(i + 1));
}

TEST(ActuatorControls, NanInfAndClamp) {
  mavlink_hil_actuator_controls_t a{};
  a.mode = kModeFlagArmed;
  a.controls[0] = std::nanf("");
  a.controls[1] = std::numeric_limits<float>::infinity();
  a.controls[2] = 1.5f;
  a.controls[3] = -0.5f;
  a.controls[4] = 0.75f;
  const fc::MotorCommand c = motorCommandFromActuatorControls(a);
  EXPECT_TRUE(c.armed);
  EXPECT_EQ(c.power[0], 0.0f);
  EXPECT_EQ(c.power[1], 0.0f);
  EXPECT_EQ(c.power[2], 1.0f);
  EXPECT_EQ(c.power[3], 0.0f);
  EXPECT_EQ(c.power[4], 0.75f);
}

TEST(ActuatorControls, DisarmedIsZero) {
  mavlink_hil_actuator_controls_t a{};
  a.mode = 1;  // custom mode, not armed
  for (int i = 0; i < 16; ++i) a.controls[i] = 0.9f;
  const fc::MotorCommand c = motorCommandFromActuatorControls(a);
  EXPECT_FALSE(c.armed);
  for (const float p : c.power) EXPECT_EQ(p, 0.0f);
}

TEST(Modes, InterfaceToPx4) {
  EXPECT_EQ(toPx4Mode(fc::FlightMode::kGpsHold, NoGpsMode::kAltctl), (Px4Mode{3, 0}));  // POSCTL
  EXPECT_EQ(toPx4Mode(fc::FlightMode::kGpsHold, NoGpsMode::kStabilized), (Px4Mode{3, 0}));
  EXPECT_EQ(toPx4Mode(fc::FlightMode::kNoGpsHold, NoGpsMode::kAltctl), (Px4Mode{2, 0}));      // ALTCTL
  EXPECT_EQ(toPx4Mode(fc::FlightMode::kNoGpsHold, NoGpsMode::kStabilized), (Px4Mode{7, 0}));  // STABILIZED
}

TEST(Modes, NeverAcro) {
  for (const auto fm : {fc::FlightMode::kGpsHold, fc::FlightMode::kNoGpsHold}) {
    for (const auto ng : {NoGpsMode::kAltctl, NoGpsMode::kStabilized}) {
      EXPECT_NE(toPx4Mode(fm, ng).main_mode, 5);  // PX4_CUSTOM_MAIN_MODE_ACRO
    }
  }
}

TEST(Modes, Px4ToInterface) {
  EXPECT_EQ(fromPx4Mode({3, 0}), fc::FlightMode::kGpsHold);
  EXPECT_EQ(fromPx4Mode({2, 0}), fc::FlightMode::kNoGpsHold);
  EXPECT_EQ(fromPx4Mode({7, 0}), fc::FlightMode::kNoGpsHold);
  EXPECT_FALSE(fromPx4Mode({1, 0}).has_value());  // manual
  EXPECT_FALSE(fromPx4Mode({5, 0}).has_value());  // acro: not representable
  EXPECT_FALSE(fromPx4Mode({6, 0}).has_value());  // offboard
  EXPECT_FALSE(fromPx4Mode({4, 3}).has_value());  // auto loiter (a failsafe hold)
  EXPECT_FALSE(fromPx4Mode({4, 6}).has_value());  // auto land
  EXPECT_FALSE(fromPx4Mode({3, 2}).has_value());  // position slow
}

TEST(Modes, CustomModeEncoding) {
  // px4_custom_mode.h: reserved 16 bits, then main_mode, then sub_mode.
  EXPECT_EQ(customModeOf({3, 0}), 3u << 16);
  EXPECT_EQ(customModeOf({4, 3}), (4u << 16) | (3u << 24));
  EXPECT_EQ(px4ModeFromCustomMode(customModeOf({2, 0})), (Px4Mode{2, 0}));
  EXPECT_EQ(px4ModeFromCustomMode(0x03040000u), (Px4Mode{4, 3}));
  for (const Px4Mode m : {Px4Mode{3, 0}, Px4Mode{2, 0}, Px4Mode{7, 0}, Px4Mode{4, 6}}) {
    EXPECT_EQ(px4ModeFromCustomMode(customModeOf(m)), m);
  }
}

TEST(Link, HilSensorRoundTrip) {
  const mavlink_hil_sensor_t in = makeHilSensor(frame());
  const mavlink_message_t msg = roundTrip([&](MavlinkLink& l) { l.sendHilSensor(in); });
  ASSERT_EQ(msg.msgid, MAVLINK_MSG_ID_HIL_SENSOR);
  EXPECT_EQ(msg.sysid, 1);
  mavlink_hil_sensor_t out;
  mavlink_msg_hil_sensor_decode(&msg, &out);
  EXPECT_EQ(out.time_usec, in.time_usec);
  EXPECT_FLOAT_EQ(out.zacc, in.zacc);
  EXPECT_FLOAT_EQ(out.abs_pressure, in.abs_pressure);
  EXPECT_EQ(out.fields_updated, in.fields_updated);
}

TEST(Link, SequenceNumbersIncrease) {
  MavlinkLink tx(1, 51), rx(2, 2);
  tx.sendHilSensor(makeHilSensor(frame()));
  tx.sendHilGps(makeHilGps(frame()));
  tx.sendHeartbeat();
  const auto msgs = rx.parse(tx.takeTx());
  ASSERT_EQ(msgs.size(), 3u);
  EXPECT_EQ(msgs[1].seq, static_cast<std::uint8_t>(msgs[0].seq + 1));
  EXPECT_EQ(msgs[2].seq, static_cast<std::uint8_t>(msgs[0].seq + 2));
  EXPECT_EQ(msgs[0].msgid, MAVLINK_MSG_ID_HIL_SENSOR);
  EXPECT_EQ(msgs[1].msgid, MAVLINK_MSG_ID_HIL_GPS);
  EXPECT_EQ(msgs[2].msgid, MAVLINK_MSG_ID_HEARTBEAT);
}

TEST(Link, ParserHandlesByteWiseChunksAndGarbage) {
  MavlinkLink tx(1, 51), rx(2, 2);
  tx.sendHilSensor(makeHilSensor(frame()));
  tx.sendManualControl(makeManualControl({}));
  Bytes stream = {0x00, 0x12, 0x99};  // garbage before the first frame
  const Bytes good = tx.takeTx();
  stream.insert(stream.end(), good.begin(), good.end());
  std::vector<mavlink_message_t> msgs;
  for (const std::uint8_t b : stream) {
    for (const auto& m : rx.parse({&b, 1})) msgs.push_back(m);
  }
  ASSERT_EQ(msgs.size(), 2u);
  EXPECT_EQ(msgs[0].msgid, MAVLINK_MSG_ID_HIL_SENSOR);
  EXPECT_EQ(msgs[1].msgid, MAVLINK_MSG_ID_MANUAL_CONTROL);
}

TEST(Link, CorruptedFrameIsRejected) {
  MavlinkLink tx(1, 51), rx(2, 2);
  tx.sendHilSensor(makeHilSensor(frame()));
  Bytes b = tx.takeTx();
  b[20] ^= 0xFF;
  EXPECT_TRUE(rx.parse(b).empty());
}

TEST(Link, ArmDisarmCommand) {
  const mavlink_message_t msg = roundTrip([](MavlinkLink& l) { l.sendArmDisarm(true); });
  ASSERT_EQ(msg.msgid, MAVLINK_MSG_ID_COMMAND_LONG);
  mavlink_command_long_t c;
  mavlink_msg_command_long_decode(&msg, &c);
  EXPECT_EQ(c.command, MAV_CMD_COMPONENT_ARM_DISARM);
  EXPECT_EQ(c.target_system, kPx4SystemId);
  EXPECT_EQ(c.target_component, kPx4ComponentId);
  EXPECT_FLOAT_EQ(c.param1, 1.0f);
  const mavlink_message_t d = roundTrip([](MavlinkLink& l) { l.sendArmDisarm(false); });
  mavlink_msg_command_long_decode(&d, &c);
  EXPECT_FLOAT_EQ(c.param1, 0.0f);
}

TEST(Link, SetModeCommand) {
  const mavlink_message_t msg = roundTrip([](MavlinkLink& l) { l.sendSetMode({3, 0}); });
  ASSERT_EQ(msg.msgid, MAVLINK_MSG_ID_COMMAND_LONG);
  mavlink_command_long_t c;
  mavlink_msg_command_long_decode(&msg, &c);
  EXPECT_EQ(c.command, MAV_CMD_DO_SET_MODE);
  EXPECT_FLOAT_EQ(c.param1, static_cast<float>(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED));
  EXPECT_FLOAT_EQ(c.param2, 3.0f);  // PX4_CUSTOM_MAIN_MODE_POSCTL
  EXPECT_FLOAT_EQ(c.param3, 0.0f);
}
