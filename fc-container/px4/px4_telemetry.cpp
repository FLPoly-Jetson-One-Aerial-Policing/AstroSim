/** @file px4_telemetry.cpp @brief Telemetry assembly from PX4's MAVLink stream. */
#include "px4_telemetry.hpp"

#include <cmath>

namespace astrosim::px4 {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

/**
 * @brief Wrap an angle into [0, 2*pi).
 * @param a Angle, rad.
 * @return The wrapped angle.
 */
float wrap2Pi(double a) {
  a = std::fmod(a, kTwoPi);
  if (a < 0) a += kTwoPi;
  if (a >= kTwoPi) a = 0;
  return static_cast<float>(a);
}

/// @brief ESTIMATOR_STATUS_FLAGS bits used for health (MAVLink common).
constexpr std::uint16_t kEstAttitude = 1;
constexpr std::uint16_t kEstVelocityVert = 4;
constexpr std::uint16_t kEstPosVertAbs = 32;
constexpr std::uint16_t kEstGpsGlitch = 1024;
constexpr std::uint16_t kEstAccelError = 2048;

/// @brief Battery percentage below which kWarnLowBattery is raised.
constexpr float kLowBatteryPct = 15.0f;

}  // namespace

void Px4Telemetry::onMessage(const mavlink_message_t& msg, fc::TimePoint now) {
  if (msg.sysid != kPx4SystemId) return;
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
      mavlink_heartbeat_t hb;
      mavlink_msg_heartbeat_decode(&msg, &hb);
      if (hb.autopilot != MAV_AUTOPILOT_PX4) return;  // other components on the link (e.g. gimbal) also heartbeat
      heartbeat_seen_ = true;
      last_heartbeat_ = now;
      armed_ = (hb.base_mode & kModeFlagArmed) != 0;
      if ((hb.base_mode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) != 0) {
        active_mode_ = px4ModeFromCustomMode(hb.custom_mode);
        if (const auto m = fromPx4Mode(*active_mode_)) last_mode_ = *m;
      } else {
        active_mode_ = Px4Mode{};  // a non-custom mode: not one of ours
      }
      break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
      mavlink_attitude_t a;
      mavlink_msg_attitude_decode(&msg, &a);
      roll_rad_ = a.roll;
      pitch_rad_ = a.pitch;
      yaw_rad_ = a.yaw;
      break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
      mavlink_global_position_int_t p;
      mavlink_msg_global_position_int_decode(&msg, &p);
      alt_msl_m_ = static_cast<float>(p.alt / 1000.0);
      alt_agl_m_ = static_cast<float>(p.relative_alt / 1000.0);
      vn_mps_ = static_cast<float>(p.vx / 100.0);
      ve_mps_ = static_cast<float>(p.vy / 100.0);
      if (std::hypot(vn_mps_, ve_mps_) > 0.3) course_rad_ = wrap2Pi(std::atan2(ve_mps_, vn_mps_));
      break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
      mavlink_vfr_hud_t v;
      mavlink_msg_vfr_hud_decode(&msg, &v);
      vspeed_mps_ = v.climb;
      ground_speed_mps_ = v.groundspeed;
      break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
      mavlink_gps_raw_int_t g;
      mavlink_msg_gps_raw_int_decode(&msg, &g);
      gps_seen_ = true;
      gps_fix_ = g.fix_type >= 3 ? proto::kGps3dFix : (g.fix_type == 2 ? proto::kGps2dFix : proto::kGpsNoFix);
      gps_sats_ = g.satellites_visible == 255 ? 0 : g.satellites_visible;
      break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
      mavlink_sys_status_t s;
      mavlink_msg_sys_status_decode(&msg, &s);
      battery_known_ = s.voltage_battery != UINT16_MAX;
      battery_v_ = battery_known_ ? static_cast<float>(s.voltage_battery / 1000.0) : 0.0f;
      battery_pct_ = s.battery_remaining >= 0 ? static_cast<float>(s.battery_remaining) : 0.0f;
      rc_present_ = (s.onboard_control_sensors_present & MAV_SYS_STATUS_SENSOR_RC_RECEIVER) != 0 &&
                    (s.onboard_control_sensors_enabled & MAV_SYS_STATUS_SENSOR_RC_RECEIVER) != 0;
      rc_healthy_ = (s.onboard_control_sensors_health & MAV_SYS_STATUS_SENSOR_RC_RECEIVER) != 0;
      break;
    }
    case MAVLINK_MSG_ID_ESTIMATOR_STATUS: {
      mavlink_estimator_status_t e;
      mavlink_msg_estimator_status_decode(&msg, &e);
      estimator_seen_ = true;
      estimator_flags_ = e.flags;
      break;
    }
    default:
      break;
  }
}

fc::FcTelemetry Px4Telemetry::build(bool connected, bool mode_refused) const {
  fc::FcTelemetry t;
  t.time_s = time_s_;
  t.connected = connected;
  t.armed = armed_;
  t.mode_refused = mode_refused;
  t.mode = last_mode_;
  t.gps_fix = gps_fix_;
  t.gps_sats = gps_sats_;
  t.roll_rad = roll_rad_;
  t.pitch_rad = pitch_rad_;
  t.heading_rad = wrap2Pi(yaw_rad_);
  t.alt_msl_m = alt_msl_m_;
  t.alt_agl_m = alt_agl_m_;
  t.vspeed_mps = vspeed_mps_;
  t.ground_speed_mps = ground_speed_mps_;
  t.course_rad = course_rad_;
  t.battery_pct = battery_pct_;
  t.battery_v = battery_v_;
  t.motors = motors_;

  std::uint16_t w = 0;
  if (!gps_seen_ || gps_fix_ < proto::kGps3dFix) w |= proto::kWarnGpsLost;
  const std::uint16_t need = kEstAttitude | kEstVelocityVert | kEstPosVertAbs;
  if (!estimator_seen_ || (estimator_flags_ & need) != need || (estimator_flags_ & (kEstGpsGlitch | kEstAccelError)) != 0) {
    w |= proto::kWarnEkfUnhealthy;
  }
  if (rc_present_ && !rc_healthy_) w |= proto::kWarnPilotLost;
  if (battery_known_ && battery_pct_ < kLowBatteryPct) w |= proto::kWarnLowBattery;
  t.warnings = w;
  return t;
}

}  // namespace astrosim::px4
