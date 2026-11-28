/** @file mavlink_codec.cpp @brief Pure MAVLink codec for the PX4 adapter. */
#include "mavlink_codec.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "astrosim/fc/frames.hpp"

namespace astrosim::px4 {

namespace {

/**
 * @brief Round to the nearest integer and saturate to the range of an integer type; non-finite values become 0.
 * @tparam T Destination integer type.
 * @param v Value.
 * @return The converted value.
 */
template <class T>
T saturate(double v) {
  if (!std::isfinite(v)) return 0;
  const double lo = static_cast<double>(std::numeric_limits<T>::min());
  const double hi = static_cast<double>(std::numeric_limits<T>::max());
  return static_cast<T>(std::llround(std::clamp(v, lo, hi)));
}

/**
 * @brief Clamp a stick value to [lo, hi]; a non-finite value becomes the neutral value.
 * @param v Value.
 * @param lo Lower bound.
 * @param hi Upper bound.
 * @param neutral Result for a non-finite input.
 * @return The clamped value.
 */
double stick(double v, double lo, double hi, double neutral) {
  return std::isfinite(v) ? std::clamp(v, lo, hi) : neutral;
}

/**
 * @brief Convert seconds to microseconds as an unsigned count; negative or non-finite times become 0.
 * @param seconds Time in seconds.
 * @return Microseconds.
 */
std::uint64_t toUsec(double seconds) {
  const std::int64_t v = saturate<std::int64_t>(seconds * 1e6);
  return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

}  // namespace

Px4Mode toPx4Mode(fc::FlightMode mode, NoGpsMode no_gps) {
  if (mode == fc::FlightMode::kGpsHold) return {static_cast<std::uint8_t>(Px4MainMode::kPosctl), 0};
  return {static_cast<std::uint8_t>(no_gps == NoGpsMode::kStabilized ? Px4MainMode::kStabilized : Px4MainMode::kAltctl),
          0};
}

std::optional<fc::FlightMode> fromPx4Mode(const Px4Mode& mode) {
  if (mode.sub_mode != 0) return std::nullopt;
  switch (static_cast<Px4MainMode>(mode.main_mode)) {
    case Px4MainMode::kPosctl: return fc::FlightMode::kGpsHold;
    case Px4MainMode::kAltctl:
    case Px4MainMode::kStabilized: return fc::FlightMode::kNoGpsHold;
  }
  return std::nullopt;
}

Px4Mode px4ModeFromCustomMode(std::uint32_t custom_mode) {
  return {static_cast<std::uint8_t>((custom_mode >> 16) & 0xFF), static_cast<std::uint8_t>((custom_mode >> 24) & 0xFF)};
}

std::uint32_t customModeOf(const Px4Mode& mode) {
  return (static_cast<std::uint32_t>(mode.main_mode) << 16) | (static_cast<std::uint32_t>(mode.sub_mode) << 24);
}

mavlink_hil_sensor_t makeHilSensor(const fc::SensorFrame& frame) {
  const fc::Vec3 acc = fc::frames::fluToFrd(frame.accel);
  const fc::Vec3 gyro = fc::frames::fluToFrd(frame.gyro);
  const fc::Vec3 mag = fc::frames::fluToFrd(frame.mag);
  mavlink_hil_sensor_t m{};
  m.time_usec = toUsec(frame.time_s);
  m.xacc = static_cast<float>(acc[0]);
  m.yacc = static_cast<float>(acc[1]);
  m.zacc = static_cast<float>(acc[2]);
  m.xgyro = static_cast<float>(gyro[0]);
  m.ygyro = static_cast<float>(gyro[1]);
  m.zgyro = static_cast<float>(gyro[2]);
  m.xmag = static_cast<float>(mag[0]);
  m.ymag = static_cast<float>(mag[1]);
  m.zmag = static_cast<float>(mag[2]);
  m.abs_pressure = static_cast<float>(frame.baro_pressure_pa / 100.0);
  m.diff_pressure = 0;
  m.pressure_alt = static_cast<float>(frame.baro_altitude_m);
  m.temperature = static_cast<float>(frame.baro_temperature_c);
  m.fields_updated = kHilSensorFieldsUpdated;
  m.id = 0;
  return m;
}

mavlink_hil_gps_t makeHilGps(const fc::SensorFrame& frame) {
  const fc::Gps& g = frame.gps;
  const double vn = g.vel_ned[0], ve = g.vel_ned[1], vd = g.vel_ned[2];
  const double speed = std::hypot(vn, ve);
  constexpr double kPi = 3.14159265358979323846;
  mavlink_hil_gps_t m{};
  m.time_usec = toUsec(frame.time_s);
  m.fix_type = g.fix;
  m.lat = saturate<std::int32_t>(g.lat_deg * 1e7);
  m.lon = saturate<std::int32_t>(g.lon_deg * 1e7);
  m.alt = saturate<std::int32_t>(g.alt_m * 1e3);
  m.eph = 30;  // cm
  m.epv = 50;  // cm
  m.vel = saturate<std::uint16_t>(speed * 100.0);
  m.vn = saturate<std::int16_t>(vn * 100.0);
  m.ve = saturate<std::int16_t>(ve * 100.0);
  m.vd = saturate<std::int16_t>(vd * 100.0);
  if (speed < 0.2) {
    m.cog = 65535;  // unknown
  } else {
    double course = std::atan2(ve, vn);  // clockwise from north
    if (course < 0) course += 2 * kPi;
    m.cog = std::min<std::uint16_t>(saturate<std::uint16_t>(course * 180.0 / kPi * 100.0), 35999);
  }
  m.satellites_visible = g.satellites;
  m.id = 0;
  m.yaw = 0;  // unknown
  return m;
}

std::optional<mavlink_hil_state_quaternion_t> makeHilStateQuaternion(const fc::SensorFrame& frame) {
  if (!frame.truth) return std::nullopt;
  const fc::Truth& t = *frame.truth;
  fc::Quat q = fc::frames::normalize(fc::frames::enuFluToNedFrd(t.quat));
  if (q[3] < 0) q = {-q[0], -q[1], -q[2], -q[3]};
  const fc::Vec3 gyro = fc::frames::fluToFrd(frame.gyro);
  const fc::Vec3 acc = fc::frames::fluToFrd(frame.accel);
  const fc::Vec3 vel = fc::frames::enuToNed(t.vel_enu);
  mavlink_hil_state_quaternion_t m{};
  m.time_usec = toUsec(frame.time_s);
  m.attitude_quaternion[0] = static_cast<float>(q[3]);
  m.attitude_quaternion[1] = static_cast<float>(q[0]);
  m.attitude_quaternion[2] = static_cast<float>(q[1]);
  m.attitude_quaternion[3] = static_cast<float>(q[2]);
  m.rollspeed = static_cast<float>(gyro[0]);
  m.pitchspeed = static_cast<float>(gyro[1]);
  m.yawspeed = static_cast<float>(gyro[2]);
  m.lat = saturate<std::int32_t>(frame.gps.lat_deg * 1e7);
  m.lon = saturate<std::int32_t>(frame.gps.lon_deg * 1e7);
  m.alt = saturate<std::int32_t>(frame.gps.alt_m * 1e3);
  m.vx = saturate<std::int16_t>(vel[0] * 100.0);
  m.vy = saturate<std::int16_t>(vel[1] * 100.0);
  m.vz = saturate<std::int16_t>(vel[2] * 100.0);
  m.ind_airspeed = 0;
  m.true_airspeed = 0;
  m.xacc = saturate<std::int16_t>(acc[0] * 1000.0);
  m.yacc = saturate<std::int16_t>(acc[1] * 1000.0);
  m.zacc = saturate<std::int16_t>(acc[2] * 1000.0);
  return m;
}

mavlink_manual_control_t makeManualControl(const fc::PilotInput& input, std::uint8_t target_system) {
  mavlink_manual_control_t m{};
  m.target = target_system;
  m.x = saturate<std::int16_t>(-stick(input.pitch, -1, 1, 0) * 1000.0);  // forward stick (positive) is nose down
  m.y = saturate<std::int16_t>(stick(input.roll, -1, 1, 0) * 1000.0);
  m.z = saturate<std::int16_t>(stick(input.throttle, 0, 1, 0.5) * 1000.0);
  m.r = saturate<std::int16_t>(stick(input.yaw, -1, 1, 0) * 1000.0);
  m.buttons = 0;
  m.buttons2 = 0;
  m.enabled_extensions = 0;
  return m;
}

fc::MotorCommand motorCommandFromActuatorControls(const mavlink_hil_actuator_controls_t& msg) {
  fc::MotorCommand cmd;
  cmd.armed = (msg.mode & kModeFlagArmed) != 0;
  if (!cmd.armed) return cmd;
  for (std::size_t i = 0; i < fc::kNumMotors; ++i) {
    const float c = msg.controls[i];
    cmd.power[i] = std::isfinite(c) ? std::clamp(c, 0.0f, 1.0f) : 0.0f;
  }
  return cmd;
}

MavlinkLink::MavlinkLink(std::uint8_t system_id, std::uint8_t component_id)
    : system_id_(system_id), component_id_(component_id) {}

void MavlinkLink::append(const mavlink_message_t& msg) {
  std::uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const std::uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  tx_.insert(tx_.end(), buf, buf + n);
}

void MavlinkLink::sendHilSensor(const mavlink_hil_sensor_t& m) {
  mavlink_message_t msg;
  mavlink_msg_hil_sensor_encode_status(system_id_, component_id_, &tx_status_, &msg, &m);
  append(msg);
}

void MavlinkLink::sendHilGps(const mavlink_hil_gps_t& m) {
  mavlink_message_t msg;
  mavlink_msg_hil_gps_encode_status(system_id_, component_id_, &tx_status_, &msg, &m);
  append(msg);
}

void MavlinkLink::sendHilStateQuaternion(const mavlink_hil_state_quaternion_t& m) {
  mavlink_message_t msg;
  mavlink_msg_hil_state_quaternion_encode_status(system_id_, component_id_, &tx_status_, &msg, &m);
  append(msg);
}

void MavlinkLink::sendManualControl(const mavlink_manual_control_t& m) {
  mavlink_message_t msg;
  mavlink_msg_manual_control_encode_status(system_id_, component_id_, &tx_status_, &msg, &m);
  append(msg);
}

void MavlinkLink::sendArmDisarm(bool arm) {
  mavlink_message_t msg;
  mavlink_msg_command_long_pack_status(system_id_, component_id_, &tx_status_, &msg, kPx4SystemId, kPx4ComponentId,
                                       MAV_CMD_COMPONENT_ARM_DISARM, 0, arm ? 1.0f : 0.0f, 0, 0, 0, 0, 0, 0);
  append(msg);
}

void MavlinkLink::sendSetMode(const Px4Mode& mode) {
  mavlink_message_t msg;
  mavlink_msg_command_long_pack_status(system_id_, component_id_, &tx_status_, &msg, kPx4SystemId, kPx4ComponentId,
                                       MAV_CMD_DO_SET_MODE, 0, static_cast<float>(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED),
                                       static_cast<float>(mode.main_mode), static_cast<float>(mode.sub_mode), 0, 0, 0,
                                       0);
  append(msg);
}

void MavlinkLink::sendHeartbeat() {
  mavlink_message_t msg;
  mavlink_msg_heartbeat_pack_status(system_id_, component_id_, &tx_status_, &msg, MAV_TYPE_ONBOARD_CONTROLLER,
                                    MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
  append(msg);
}

Bytes MavlinkLink::takeTx() {
  Bytes out;
  out.swap(tx_);
  return out;
}

std::vector<mavlink_message_t> MavlinkLink::parse(std::span<const std::uint8_t> data) {
  std::vector<mavlink_message_t> out;
  mavlink_message_t msg;
  mavlink_status_t status;
  for (const std::uint8_t byte : data) {
    if (mavlink_frame_char_buffer(&rx_msg_, &rx_status_, byte, &msg, &status) == MAVLINK_FRAMING_OK) {
      out.push_back(msg);
    }
  }
  return out;
}

}  // namespace astrosim::px4
