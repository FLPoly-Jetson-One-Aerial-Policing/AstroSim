/** @file test_px4_session.cpp @brief Tests of the pure PX4 session using a scripted fake PX4. */
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

#include "px4_session.hpp"

using namespace astrosim;
using namespace astrosim::px4;

namespace {

using std::chrono::milliseconds;

/// @brief Time point `ms` milliseconds after an arbitrary non-zero origin.
fc::TimePoint at(int ms) {
  return fc::TimePoint{} + std::chrono::seconds(1000) + milliseconds(ms);
}

/// @brief Encode a MAVLink message to bytes.
Bytes bytesOf(const mavlink_message_t& msg) {
  std::uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const std::uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  return Bytes(buf, buf + n);
}

Bytes heartbeatBytes(bool armed, const Px4Mode& mode) {
  mavlink_heartbeat_t h{};
  h.autopilot = MAV_AUTOPILOT_PX4;
  h.type = MAV_TYPE_OCTOROTOR;
  h.base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | (armed ? kModeFlagArmed : 0);
  h.custom_mode = customModeOf(mode);
  mavlink_message_t m;
  mavlink_msg_heartbeat_encode(kPx4SystemId, kPx4ComponentId, &m, &h);
  return bytesOf(m);
}

Bytes ackBytes(std::uint16_t command, std::uint8_t result) {
  mavlink_command_ack_t a{};
  a.command = command;
  a.result = result;
  mavlink_message_t m;
  mavlink_msg_command_ack_encode(kPx4SystemId, kPx4ComponentId, &m, &a);
  return bytesOf(m);
}

Bytes actuatorBytes(bool armed, float value) {
  mavlink_hil_actuator_controls_t a{};
  a.mode = 1 | (armed ? kModeFlagArmed : 0);
  for (int i = 0; i < 16; ++i) a.controls[i] = value + 0.01f * static_cast<float>(i);
  mavlink_message_t m;
  mavlink_msg_hil_actuator_controls_encode(kPx4SystemId, kPx4ComponentId, &m, &a);
  return bytesOf(m);
}

fc::SensorFrame frameAt(double t, bool truth = false) {
  fc::SensorFrame f;
  f.time_s = t;
  f.accel = {0, 0, 9.81};
  f.gps.fix = 3;
  f.gps.satellites = 12;
  if (truth) f.truth = fc::Truth{};
  return f;
}

/// @brief What a MAVLink byte stream from the session contained.
struct Seen {
  int manual = 0, arm = 0, disarm = 0, sensor = 0, gps = 0, state = 0;
  std::vector<Px4Mode> set_modes;
  mavlink_manual_control_t last_manual{};
  std::uint64_t last_sensor_usec = 0;
};

/// @brief A session with a fake PX4 on both links.
class Rig {
 public:
  explicit Rig(const Px4SessionConfig& cfg = {}) : s(cfg) {}

  /// @brief Collect and classify everything the session has emitted on both links.
  void drain() {
    for (const auto& m : mav_rx.parse(s.takeMavlinkTx())) classify(m);
    for (const auto& m : hil_rx.parse(s.takeHilTx())) classify(m);
  }

  void classify(const mavlink_message_t& m) {
    switch (m.msgid) {
      case MAVLINK_MSG_ID_MANUAL_CONTROL:
        ++seen.manual;
        mavlink_msg_manual_control_decode(&m, &seen.last_manual);
        break;
      case MAVLINK_MSG_ID_COMMAND_LONG: {
        mavlink_command_long_t c;
        mavlink_msg_command_long_decode(&m, &c);
        if (c.command == MAV_CMD_COMPONENT_ARM_DISARM) ++(c.param1 > 0.5f ? seen.arm : seen.disarm);
        if (c.command == MAV_CMD_DO_SET_MODE) {
          seen.set_modes.push_back({static_cast<std::uint8_t>(c.param2), static_cast<std::uint8_t>(c.param3)});
        }
        break;
      }
      case MAVLINK_MSG_ID_HIL_SENSOR: {
        ++seen.sensor;
        mavlink_hil_sensor_t h;
        mavlink_msg_hil_sensor_decode(&m, &h);
        seen.last_sensor_usec = h.time_usec;
        break;
      }
      case MAVLINK_MSG_ID_HIL_GPS: ++seen.gps; break;
      case MAVLINK_MSG_ID_HIL_STATE_QUATERNION: ++seen.state; break;
      default: break;
    }
  }

  /// @brief Poll every millisecond from `from_ms` to `to_ms` (inclusive), then drain.
  void run(int from_ms, int to_ms) {
    for (int t = from_ms; t <= to_ms; ++t) {
      s.poll(at(t));
      drain();
    }
  }

  void heartbeat(int ms, bool armed, const Px4Mode& mode) { s.feedMavlink(heartbeatBytes(armed, mode), at(ms)); }

  Px4Session s;
  Seen seen;
  MavlinkLink mav_rx{9, 9};
  MavlinkLink hil_rx{9, 9};
};

fc::PilotInput pilot(fc::FlightMode mode, bool arm, float throttle = 0.5f) {
  fc::PilotInput p;
  p.mode = mode;
  p.arm = arm;
  p.throttle = throttle;
  return p;
}

}  // namespace

// ---- HIL link ---------------------------------------------------------------------------------------------

TEST(Px4SessionHil, FramesAreDroppedWhileTheLinkIsDown) {
  Rig r;
  r.s.onSensors(frameAt(1.0), at(0));
  r.drain();
  EXPECT_EQ(r.seen.sensor, 0);
  EXPECT_EQ(r.s.ignoredFrames(), 1u);
}

TEST(Px4SessionHil, SensorTimeIsSimulationTime) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onSensors(frameAt(123.456), at(0));
  r.drain();
  EXPECT_EQ(r.seen.sensor, 1);
  EXPECT_EQ(r.seen.last_sensor_usec, 123456000u);
}

TEST(Px4SessionHil, GpsAndTruthAreRateLimitedInSimulationTime) {
  Rig r;
  r.s.onHilConnected(at(0));
  for (int i = 0; i < 1000; ++i) r.s.onSensors(frameAt(10.0 + 0.004 * i, true), at(i));  // 250 Hz: 1000 frames are 4 s
  r.drain();
  EXPECT_EQ(r.seen.sensor, 1000);
  EXPECT_NEAR(r.seen.gps, 40, 1);     // 10 Hz over 4 s
  EXPECT_NEAR(r.seen.state, 100, 2);  // 25 Hz over 4 s
}

TEST(Px4SessionHil, TimeMustAdvance) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onSensors(frameAt(5.0), at(0));
  r.s.onSensors(frameAt(5.0), at(1));  // duplicate
  r.s.onSensors(frameAt(4.0), at(2));  // backwards: the engine restarted, PX4's clock cannot follow
  r.s.onSensors(frameAt(5.004), at(3));
  r.drain();
  EXPECT_EQ(r.seen.sensor, 2);
  EXPECT_EQ(r.s.ignoredFrames(), 2u);
}

TEST(Px4SessionHil, ReconnectStartsAFreshClock) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onSensors(frameAt(50.0), at(0));
  r.s.onHilDisconnected();
  r.s.onHilConnected(at(10));  // a restarted PX4
  r.s.onSensors(frameAt(1.0), at(10));
  r.drain();
  EXPECT_EQ(r.seen.sensor, 1);  // only the new connection delivers a frame
  EXPECT_EQ(r.seen.last_sensor_usec, 1000000u);
}

TEST(Px4SessionHil, ActuatorControlsBecomeMotorCommandsOnce) {
  Rig r;
  r.s.onHilConnected(at(0));
  EXPECT_FALSE(r.s.takeMotors().has_value());
  r.s.feedHil(actuatorBytes(true, 0.2f), at(1));
  const auto m = r.s.takeMotors();
  ASSERT_TRUE(m.has_value());
  EXPECT_TRUE(m->armed);
  EXPECT_FLOAT_EQ(m->power[0], 0.2f);
  EXPECT_FLOAT_EQ(m->power[7], 0.27f);
  EXPECT_FALSE(r.s.takeMotors().has_value());
  EXPECT_FLOAT_EQ(r.s.telemetry().motors[3], 0.23f);
}

TEST(Px4SessionHil, DisarmedActuatorControlsAreZero) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.feedHil(actuatorBytes(false, 0.9f), at(1));
  const auto m = r.s.takeMotors();
  ASSERT_TRUE(m.has_value());
  EXPECT_FALSE(m->armed);
  for (const float p : m->power) EXPECT_EQ(p, 0.0f);
}

TEST(Px4SessionHil, ActuatorControlsSplitAcrossReads) {
  Rig r;
  r.s.onHilConnected(at(0));
  const Bytes b = actuatorBytes(true, 0.5f);
  r.s.feedHil({b.data(), 10}, at(1));
  EXPECT_FALSE(r.s.takeMotors().has_value());
  r.s.feedHil({b.data() + 10, b.size() - 10}, at(2));
  EXPECT_TRUE(r.s.takeMotors().has_value());
}

// ---- sensor dither ----------------------------------------------------------------------------------------

namespace {

/// @brief The HIL_SENSOR messages a session emits for `n` identical, perfectly constant frames.
std::vector<mavlink_hil_sensor_t> constantSensorStream(const Px4SessionConfig& cfg, int n) {
  Px4Session s(cfg);
  s.onHilConnected(at(0));
  for (int i = 0; i < n; ++i) {
    fc::SensorFrame f = frameAt(1.0 + 0.004 * i);
    f.mag = {0.2, 0.05, -0.4};
    f.baro_pressure_pa = 95000.0;
    f.baro_temperature_c = 12.0;
    s.onSensors(f, at(i));
  }
  MavlinkLink rx(9, 9);
  std::vector<mavlink_hil_sensor_t> out;
  for (const auto& m : rx.parse(s.takeHilTx())) {
    if (m.msgid != MAVLINK_MSG_ID_HIL_SENSOR) continue;
    mavlink_hil_sensor_t h;
    mavlink_msg_hil_sensor_decode(&m, &h);
    out.push_back(h);
  }
  return out;
}

}  // namespace

TEST(Px4SessionDither, SensorsNeverRepeatExactlyWhenEnabled) {
  // PX4 drops a sensor whose value is identical for 100 samples in a row.
  const auto v = constantSensorStream({}, 300);
  ASSERT_EQ(v.size(), 300u);
  int equal_run = 0, max_run = 0;
  for (std::size_t i = 1; i < v.size(); ++i) {
    const bool same = v[i].xgyro == v[i - 1].xgyro && v[i].zacc == v[i - 1].zacc && v[i].xmag == v[i - 1].xmag &&
                      v[i].abs_pressure == v[i - 1].abs_pressure;
    equal_run = same ? equal_run + 1 : 0;
    max_run = std::max(max_run, equal_run);
  }
  EXPECT_LT(max_run, 3);
}

TEST(Px4SessionDither, StaysWithinTheConfiguredAmplitude) {
  const Px4SessionConfig cfg;
  for (const auto& h : constantSensorStream(cfg, 500)) {
    EXPECT_NEAR(h.xgyro, 0.0, cfg.dither.gyro_rad_s + 1e-9);
    EXPECT_NEAR(h.zacc, -9.81, cfg.dither.accel_m_s2 + 1e-5);  // FRD: down positive, so a hover reads -g
    EXPECT_NEAR(h.xmag, 0.2, cfg.dither.mag_gauss + 1e-6);
    EXPECT_NEAR(h.abs_pressure, 950.0, cfg.dither.pressure_pa / 100.0 + 1e-3);
    EXPECT_NEAR(h.temperature, 12.0, cfg.dither.temperature_c + 1e-5);
  }
}

TEST(Px4SessionDither, IsZeroMeanAndDeterministic) {
  const auto a = constantSensorStream({}, 2000);
  const auto b = constantSensorStream({}, 2000);
  double sum = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].xgyro, b[i].xgyro);  // same seed, same sequence: runs are reproducible
    sum += a[i].xgyro;
  }
  EXPECT_NEAR(sum / static_cast<double>(a.size()), 0.0, 3e-4);
}

TEST(Px4SessionDither, CanBeDisabled) {
  Px4SessionConfig cfg;
  cfg.dither.enabled = false;
  for (const auto& h : constantSensorStream(cfg, 50)) {
    EXPECT_FLOAT_EQ(h.zacc, -9.81f);
    EXPECT_EQ(h.xgyro, 0.0f);
    EXPECT_FLOAT_EQ(h.abs_pressure, 950.0f);
  }
}

// ---- pilot link -------------------------------------------------------------------------------------------

TEST(Px4SessionPilot, NothingIsSentBeforeThereIsAPilotInput) {
  Rig r;
  r.run(0, 100);
  EXPECT_EQ(r.seen.manual, 0);
  EXPECT_TRUE(r.seen.set_modes.empty());
}

TEST(Px4SessionPilot, ManualControlAt50Hz) {
  Rig r;
  fc::PilotInput p = pilot(fc::FlightMode::kGpsHold, false, 0.7f);
  p.pitch = 0.5f;
  r.s.onPilot(p, at(0));
  r.run(0, 999);
  EXPECT_NEAR(r.seen.manual, 50, 1);
  EXPECT_EQ(r.seen.last_manual.z, 700);
  EXPECT_EQ(r.seen.last_manual.x, -500);
}

TEST(Px4SessionPilot, ModeIsRequestedAndRetriedUntilPx4Reports) {
  Rig r;
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(0));
  r.run(0, 2500);
  ASSERT_GE(r.seen.set_modes.size(), 2u);  // asked, then retried about every second
  EXPECT_LE(r.seen.set_modes.size(), 4u);
  for (const Px4Mode& m : r.seen.set_modes) EXPECT_EQ(m, (Px4Mode{3, 0}));
  const std::size_t before = r.seen.set_modes.size();
  r.heartbeat(2500, false, {3, 0});  // PX4 is in POSCTL now
  r.run(2501, 6000);
  EXPECT_EQ(r.seen.set_modes.size(), before);  // no more requests
}

TEST(Px4SessionPilot, ModeChangeIsRequestedImmediately) {
  Rig r;
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(0));
  r.heartbeat(0, false, {3, 0});
  r.run(0, 100);
  EXPECT_TRUE(r.seen.set_modes.empty());
  r.s.onPilot(pilot(fc::FlightMode::kNoGpsHold, false), at(101));
  r.run(101, 110);
  ASSERT_EQ(r.seen.set_modes.size(), 1u);
  EXPECT_EQ(r.seen.set_modes[0], (Px4Mode{2, 0}));  // ALTCTL
}

TEST(Px4SessionPilot, NoGpsModeCanBeStabilized) {
  Px4SessionConfig c;
  c.no_gps_mode = NoGpsMode::kStabilized;
  Rig r(c);
  r.s.onPilot(pilot(fc::FlightMode::kNoGpsHold, false), at(0));
  r.run(0, 10);
  ASSERT_FALSE(r.seen.set_modes.empty());
  EXPECT_EQ(r.seen.set_modes[0], (Px4Mode{7, 0}));
}

TEST(Px4SessionPilot, ArmWaitsForTheRequestedMode) {
  Rig r;
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, true), at(0));
  r.heartbeat(0, false, {2, 0});  // PX4 still in ALTCTL
  r.run(0, 1500);
  EXPECT_EQ(r.seen.arm, 0);
  r.heartbeat(1500, false, {3, 0});
  r.run(1501, 1510);
  EXPECT_EQ(r.seen.arm, 1);
}

TEST(Px4SessionPilot, ArmIsRetriedUntilPx4ReportsArmed) {
  Rig r;
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, true), at(0));
  r.heartbeat(0, false, {3, 0});
  r.run(0, 2500);
  EXPECT_GE(r.seen.arm, 2);
  EXPECT_LE(r.seen.arm, 4);
  const int before = r.seen.arm;
  r.heartbeat(2500, true, {3, 0});
  r.run(2501, 6000);
  EXPECT_EQ(r.seen.arm, before);
}

TEST(Px4SessionPilot, ArmRequestGivesUp) {
  Px4SessionConfig c;
  c.arm_give_up_s = 3;
  Rig r(c);
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, true), at(0));
  r.heartbeat(0, false, {3, 0});
  r.run(0, 3500);
  const int at_give_up = r.seen.arm;
  r.run(3501, 8000);
  EXPECT_EQ(r.seen.arm, at_give_up);
}

TEST(Px4SessionPilot, DoesNotRearmAfterPx4AutoDisarm) {
  Rig r;
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, true), at(0));
  r.heartbeat(0, false, {3, 0});
  r.run(0, 100);
  EXPECT_EQ(r.seen.arm, 1);
  r.heartbeat(100, true, {3, 0});
  r.run(101, 200);
  r.heartbeat(200, false, {3, 0});  // landed: PX4 disarmed itself, while the script still says armed
  r.run(201, 5000);
  EXPECT_EQ(r.seen.arm, 1);
  // A fresh arm request (false then true) arms again.
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(5001));
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, true), at(5002));
  r.run(5002, 5010);
  EXPECT_EQ(r.seen.arm, 2);
}

TEST(Px4SessionPilot, DisarmWhileArmed) {
  Rig r;
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(0));
  r.heartbeat(0, true, {3, 0});
  r.run(0, 50);
  EXPECT_EQ(r.seen.disarm, 1);
  EXPECT_EQ(r.seen.arm, 0);
}

// ---- telemetry --------------------------------------------------------------------------------------------

TEST(Px4SessionTelemetry, ConnectedNeedsHilLinkAndHeartbeat) {
  Rig r;
  r.s.poll(at(0));
  EXPECT_FALSE(r.s.telemetry().connected);
  r.s.onHilConnected(at(0));
  r.s.poll(at(1));
  EXPECT_FALSE(r.s.telemetry().connected);  // no heartbeat yet
  r.heartbeat(10, false, {3, 0});
  r.s.poll(at(11));
  EXPECT_TRUE(r.s.telemetry().connected);
  r.s.poll(at(10 + 3500));  // heartbeat silent for 3.5 s
  EXPECT_FALSE(r.s.telemetry().connected);
  r.heartbeat(5000, false, {3, 0});
  r.s.poll(at(5001));
  EXPECT_TRUE(r.s.telemetry().connected);
  r.s.onHilDisconnected();
  r.s.poll(at(5002));
  EXPECT_FALSE(r.s.telemetry().connected);
}

TEST(Px4SessionTelemetry, ArmedAndModeFromHeartbeat) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.heartbeat(0, true, {2, 0});
  r.s.poll(at(1));
  const fc::FcTelemetry t = r.s.telemetry();
  EXPECT_TRUE(t.armed);
  EXPECT_EQ(t.mode, fc::FlightMode::kNoGpsHold);
}

TEST(Px4SessionTelemetry, ModeRefusedWhenPx4StaysInAnotherMode) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(0));
  r.heartbeat(0, false, {2, 0});  // PX4 stays in ALTCTL
  r.s.poll(at(1000));
  EXPECT_FALSE(r.s.telemetry().mode_refused);  // give PX4 a moment
  r.heartbeat(2000, false, {2, 0});
  r.s.poll(at(2600));
  const fc::FcTelemetry t = r.s.telemetry();
  EXPECT_TRUE(t.mode_refused);
  EXPECT_EQ(t.mode, fc::FlightMode::kNoGpsHold);  // the mode actually active, never silently substituted
  // PX4 enters the mode later (a fix arrived): the refusal clears.
  r.heartbeat(3000, false, {3, 0});
  r.s.poll(at(3001));
  EXPECT_FALSE(r.s.telemetry().mode_refused);
  EXPECT_EQ(r.s.telemetry().mode, fc::FlightMode::kGpsHold);
}

TEST(Px4SessionTelemetry, DeniedAckMarksTheModeRefusedAtOnce) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(0));
  r.heartbeat(0, false, {2, 0});
  r.s.feedMavlink(ackBytes(MAV_CMD_DO_SET_MODE, MAV_RESULT_DENIED), at(100));
  r.s.poll(at(101));
  EXPECT_TRUE(r.s.telemetry().mode_refused);
  r.s.feedMavlink(ackBytes(MAV_CMD_DO_SET_MODE, MAV_RESULT_ACCEPTED), at(200));
  r.s.poll(at(201));
  EXPECT_FALSE(r.s.telemetry().mode_refused);  // accepted again; the heartbeat will confirm
}

TEST(Px4SessionTelemetry, ModeRequestedAgainRestartsTheGracePeriod) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, false), at(0));
  r.heartbeat(0, false, {2, 0});
  r.s.poll(at(3000));
  EXPECT_TRUE(r.s.telemetry().mode_refused);
  r.s.onPilot(pilot(fc::FlightMode::kNoGpsHold, false), at(3001));  // asking for what PX4 already is in
  r.s.poll(at(3002));
  EXPECT_FALSE(r.s.telemetry().mode_refused);
}

TEST(Px4SessionTelemetry, ResetClearsEverything) {
  Rig r;
  r.s.onHilConnected(at(0));
  r.s.onPilot(pilot(fc::FlightMode::kGpsHold, true), at(0));
  r.heartbeat(0, true, {3, 0});
  r.s.reset();
  EXPECT_FALSE(r.s.hilConnected());
  r.s.poll(at(1));
  EXPECT_FALSE(r.s.telemetry().armed);
  EXPECT_FALSE(r.s.telemetry().connected);
  r.drain();
  EXPECT_EQ(r.seen.manual, 0);
}
