/**
 * @file px4_telemetry.hpp
 * @brief Assembles the FC telemetry (the FC's own view) from the MAVLink stream of PX4's onboard instance.
 *
 * Pure: feed it decoded MAVLink messages and read the snapshot. Sources (all streamed by PX4's "onboard" instance at
 * the pinned tag): HEARTBEAT (armed, mode), ATTITUDE, GLOBAL_POSITION_INT, VFR_HUD, GPS_RAW_INT, SYS_STATUS (battery,
 * RC sensor health) and ESTIMATOR_STATUS (EKF health flags). The motor outputs come from the adapter itself.
 */
#pragma once
#include <array>
#include <cstdint>
#include <optional>

#include "astrosim/fc/types.hpp"
#include "mavlink_codec.hpp"

namespace astrosim::px4 {

/**
 * @brief Collects PX4's MAVLink telemetry into an fc::FcTelemetry snapshot.
 *
 * Provenance flags are not set here; the bridge overlays them. `connected` and `mode_refused` are decided by the
 * session and passed to build().
 */
class Px4Telemetry {
 public:
  /**
   * @brief Consume one message from PX4. Messages from other systems and unknown messages are ignored.
   * @param msg A decoded MAVLink message.
   * @param now Current monotonic time (recorded for HEARTBEAT).
   */
  void onMessage(const mavlink_message_t& msg, fc::TimePoint now);

  /// @brief Record the simulation time of the latest sensor frame (reported as telemetry time).
  void setSimTime(double time_s) { time_s_ = time_s; }
  /// @brief Record the motor outputs commanded by PX4.
  void setMotors(const std::array<float, fc::kNumMotors>& motors) { motors_ = motors; }
  /// @brief Forget everything received.
  void reset() { *this = Px4Telemetry{}; }

  /** @return True once a HEARTBEAT from PX4 has been received. */
  bool heartbeatSeen() const { return heartbeat_seen_; }
  /** @return Time of the last HEARTBEAT (meaningful only if heartbeatSeen()). */
  fc::TimePoint lastHeartbeat() const { return last_heartbeat_; }
  /** @return PX4's active mode from the last HEARTBEAT, or std::nullopt before any. */
  std::optional<Px4Mode> activeMode() const { return active_mode_; }
  /** @return True if the last HEARTBEAT said armed. */
  bool armed() const { return armed_; }

  /**
   * @brief Build the telemetry snapshot.
   * @param connected The adapter's link state (HIL link up and PX4 heartbeating).
   * @param mode_refused The adapter's verdict that PX4 did not enter the requested mode.
   * @return The snapshot. `mode` is the active mode mapped to the interface, or the last mapped mode if PX4 is in
   * another one (see mode_refused).
   */
  fc::FcTelemetry build(bool connected, bool mode_refused) const;

 private:
  double time_s_ = 0;
  std::array<float, fc::kNumMotors> motors_{};

  bool heartbeat_seen_ = false;
  fc::TimePoint last_heartbeat_{};
  bool armed_ = false;
  std::optional<Px4Mode> active_mode_;
  fc::FlightMode last_mode_ = fc::FlightMode::kGpsHold;  ///< Last active mode that maps to the interface.

  float roll_rad_ = 0, pitch_rad_ = 0, yaw_rad_ = 0;
  float alt_msl_m_ = 0, alt_agl_m_ = 0;
  float vn_mps_ = 0, ve_mps_ = 0;
  float course_rad_ = 0;
  float vspeed_mps_ = 0, ground_speed_mps_ = 0;
  std::uint8_t gps_fix_ = 0, gps_sats_ = 0;
  bool gps_seen_ = false;

  float battery_v_ = 0, battery_pct_ = 0;
  bool battery_known_ = false;
  bool rc_present_ = false, rc_healthy_ = false;

  bool estimator_seen_ = false;
  std::uint16_t estimator_flags_ = 0;
};

}  // namespace astrosim::px4
