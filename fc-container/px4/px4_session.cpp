/** @file px4_session.cpp @brief The pure PX4 protocol session. */
#include "px4_session.hpp"

#include <chrono>

namespace astrosim::px4 {

namespace {

using Seconds = std::chrono::duration<double>;

/**
 * @brief Convert a duration in seconds to the steady clock's duration type.
 * @param s Seconds.
 * @return The duration.
 */
fc::TimePoint::duration toDuration(double s) {
  return std::chrono::duration_cast<fc::TimePoint::duration>(Seconds(s));
}

/**
 * @brief Seconds elapsed between two time points.
 * @param later Later time.
 * @param earlier Earlier time.
 * @return later - earlier in seconds.
 */
double elapsed(fc::TimePoint later, fc::TimePoint earlier) {
  return Seconds(later - earlier).count();
}

}  // namespace

Px4Session::Px4Session(const Px4SessionConfig& config)
    : config_(config),
      hil_(kPx4SystemId, 51 /* MAV_COMP_ID_ONBOARD_COMPUTER4: the simulator */),
      mav_(kAdapterSystemId, kAdapterComponentId) {}

void Px4Session::onHilConnected(fc::TimePoint now) {
  (void)now;
  hil_connected_ = true;
  last_sensor_time_s_.reset();  // a new PX4 process starts its clock from the first HIL_SENSOR
  last_gps_time_s_.reset();
  last_hil_state_time_s_.reset();
  motors_.reset();
  hil_.takeTx();
}

void Px4Session::onHilDisconnected() {
  hil_connected_ = false;
  connected_ = false;
  hil_.takeTx();
  motors_.reset();
}

void Px4Session::feedHil(std::span<const std::uint8_t> data, fc::TimePoint) {
  for (const mavlink_message_t& msg : hil_.parse(data)) onHilMessage(msg);
}

void Px4Session::onHilMessage(const mavlink_message_t& msg) {
  // PX4 also sends HEARTBEAT and SET_MESSAGE_INTERVAL (a request for HIL_STATE_QUATERNION) on this link; the truth
  // state is sent at a fixed rate, so only the actuator controls matter.
  if (msg.msgid != MAVLINK_MSG_ID_HIL_ACTUATOR_CONTROLS) return;
  mavlink_hil_actuator_controls_t a;
  mavlink_msg_hil_actuator_controls_decode(&msg, &a);
  const fc::MotorCommand cmd = motorCommandFromActuatorControls(a);
  motors_ = cmd;
  telemetry_.setMotors(cmd.power);
}

void Px4Session::feedMavlink(std::span<const std::uint8_t> data, fc::TimePoint now) {
  for (const mavlink_message_t& msg : mav_.parse(data)) onMavlinkMessage(msg, now);
}

void Px4Session::onMavlinkMessage(const mavlink_message_t& msg, fc::TimePoint now) {
  if (msg.sysid != kPx4SystemId) return;
  if (msg.msgid == MAVLINK_MSG_ID_COMMAND_ACK) {
    mavlink_command_ack_t ack;
    mavlink_msg_command_ack_decode(&msg, &ack);
    if (ack.command == MAV_CMD_DO_SET_MODE) {
      if (ack.result == MAV_RESULT_ACCEPTED) {
        set_mode_denied_ = false;
      } else if (ack.result != MAV_RESULT_IN_PROGRESS) {
        set_mode_denied_ = true;
      }
    }
    return;
  }
  telemetry_.onMessage(msg, now);
}

double Px4Session::nextUniform() {
  rng_state_ ^= rng_state_ << 13;
  rng_state_ ^= rng_state_ >> 7;
  rng_state_ ^= rng_state_ << 17;
  return static_cast<double>(rng_state_ >> 11) * (2.0 / 9007199254740992.0) - 1.0;  // 53 random bits to [-1, 1)
}

void Px4Session::applyDither(fc::SensorFrame& f) {
  const SensorDither& d = config_.dither;
  for (std::size_t i = 0; i < 3; ++i) f.gyro[i] += d.gyro_rad_s * nextUniform();
  for (std::size_t i = 0; i < 3; ++i) f.accel[i] += d.accel_m_s2 * nextUniform();
  for (std::size_t i = 0; i < 3; ++i) f.mag[i] += d.mag_gauss * nextUniform();
  f.baro_pressure_pa += d.pressure_pa * nextUniform();
  f.baro_temperature_c += d.temperature_c * nextUniform();
}

void Px4Session::onSensors(const fc::SensorFrame& frame, fc::TimePoint) {
  telemetry_.setSimTime(frame.time_s);
  if (!hil_connected_ || (last_sensor_time_s_ && !(frame.time_s > *last_sensor_time_s_))) {
    ++ignored_frames_;
    return;
  }
  last_sensor_time_s_ = frame.time_s;
  if (config_.dither.enabled) {
    fc::SensorFrame dithered = frame;
    applyDither(dithered);
    hil_.sendHilSensor(makeHilSensor(dithered));
  } else {
    hil_.sendHilSensor(makeHilSensor(frame));
  }
  if (!last_gps_time_s_ || frame.time_s - *last_gps_time_s_ >= 1.0 / config_.gps_rate_hz - 1e-9) {
    last_gps_time_s_ = frame.time_s;
    hil_.sendHilGps(makeHilGps(frame));
  }
  if (frame.truth &&
      (!last_hil_state_time_s_ || frame.time_s - *last_hil_state_time_s_ >= 1.0 / config_.hil_state_rate_hz - 1e-9)) {
    last_hil_state_time_s_ = frame.time_s;
    if (const auto s = makeHilStateQuaternion(frame)) hil_.sendHilStateQuaternion(*s);
  }
}

void Px4Session::onPilot(const fc::PilotInput& input, fc::TimePoint now) {
  const bool first = !pilot_;
  const bool arm_before = pilot_ ? pilot_->arm : false;
  const bool mode_changed = !requested_mode_ || *requested_mode_ != input.mode;
  pilot_ = input;
  if (mode_changed) {
    requested_mode_ = input.mode;
    mode_requested_at_ = now;
    set_mode_sent_ = false;
    set_mode_denied_ = false;
  }
  if (input.arm && !arm_before) {
    arm_pending_ = true;
    arm_requested_at_ = now;
    arm_sent_ = false;
  }
  if (!input.arm) {
    arm_pending_ = false;
    if (arm_before || first) disarm_sent_ = false;
  }
}

bool Px4Session::connected(fc::TimePoint now) const {
  return hil_connected_ && telemetry_.heartbeatSeen() &&
         elapsed(now, telemetry_.lastHeartbeat()) <= config_.link_timeout_s;
}

bool Px4Session::modeRefused(fc::TimePoint now) const {
  if (!requested_mode_ || !telemetry_.activeMode()) return false;
  const Px4Mode want = toPx4Mode(*requested_mode_, config_.no_gps_mode);
  if (*telemetry_.activeMode() == want) return false;
  return set_mode_denied_ || elapsed(now, mode_requested_at_) >= config_.mode_refused_after_s;
}

void Px4Session::poll(fc::TimePoint now) {
  last_poll_ = now;
  connected_ = connected(now);
  if (!pilot_) return;

  // MANUAL_CONTROL: always, at its rate, so PX4 never sees the pilot as lost.
  if (now >= next_manual_) {
    mav_.sendManualControl(makeManualControl(*pilot_));
    next_manual_ = now + toDuration(1.0 / config_.manual_control_rate_hz);
  }

  const Px4Mode want = toPx4Mode(*requested_mode_, config_.no_gps_mode);
  const bool in_mode = telemetry_.activeMode() && *telemetry_.activeMode() == want;

  // Mode: ask now, then retry until PX4 reports it (a refused mode, e.g. GPS hold without a fix, may become
  // available later, so the request stays open).
  if (!in_mode && (!set_mode_sent_ || elapsed(now, last_set_mode_) >= config_.command_retry_s)) {
    mav_.sendSetMode(want);
    set_mode_sent_ = true;
    last_set_mode_ = now;
  }

  // Arm: only in the requested mode, on a rising edge, retried until armed or given up.
  if (arm_pending_) {
    if (telemetry_.armed()) {
      arm_pending_ = false;
    } else if (elapsed(now, arm_requested_at_) > config_.arm_give_up_s) {
      arm_pending_ = false;
    } else if (in_mode && (!arm_sent_ || elapsed(now, last_arm_cmd_) >= config_.command_retry_s)) {
      mav_.sendArmDisarm(true);
      arm_sent_ = true;
      last_arm_cmd_ = now;
    }
  }

  // Disarm: while the request is false and PX4 reports armed.
  if (!pilot_->arm && telemetry_.armed() &&
      (!disarm_sent_ || elapsed(now, last_disarm_cmd_) >= config_.command_retry_s)) {
    mav_.sendArmDisarm(false);
    disarm_sent_ = true;
    last_disarm_cmd_ = now;
  }
}

std::optional<fc::MotorCommand> Px4Session::takeMotors() {
  std::optional<fc::MotorCommand> out;
  out.swap(motors_);
  return out;
}

fc::FcTelemetry Px4Session::telemetry() const {
  return telemetry_.build(connected_, modeRefused(last_poll_));
}

void Px4Session::reset() {
  const Px4SessionConfig config = config_;
  *this = Px4Session(config);
}

}  // namespace astrosim::px4
