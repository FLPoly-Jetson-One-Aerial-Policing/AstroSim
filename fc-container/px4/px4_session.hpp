/**
 * @file px4_session.hpp
 * @brief Px4Session: the pure protocol logic of the PX4 adapter. No sockets, no clock reads; time is injected.
 *
 * The session sits between the FC interface (sensor frames and pilot input in, motor commands and telemetry out)
 * and the two byte streams the socket glue (Px4Adapter) moves:
 *  - the HIL link (TCP; PX4's simulator_mavlink is the client): HIL_SENSOR / HIL_GPS / HIL_STATE_QUATERNION out,
 *    HIL_ACTUATOR_CONTROLS in. Lockstep: PX4's clock is set from the HIL_SENSOR time, which is the simulation time.
 *  - the pilot link (UDP; PX4's onboard MAVLink instance): MANUAL_CONTROL, ARM_DISARM and DO_SET_MODE out,
 *    PX4's telemetry in.
 *
 * Mode policy: the two interface modes map to POSCTL and to ALTCTL (or STABILIZED); never ACRO. The session asks
 * PX4 for the requested mode (retrying), arms only once PX4 is in it, and reports `mode_refused` if PX4 does not
 * enter it. It never substitutes another mode.
 * Arm policy: arming is an event on a rising edge of PilotInput::arm (retried until PX4 reports armed or a timeout);
 * if PX4 disarms by itself (for example auto-disarm after landing) the session does not re-arm until the request
 * has gone false and true again.
 */
#pragma once
#include <cstdint>
#include <optional>
#include <span>

#include "astrosim/fc/types.hpp"
#include "mavlink_codec.hpp"
#include "px4_telemetry.hpp"

namespace astrosim::px4 {

/**
 * @brief Dither added to the HIL_SENSOR values (uniform noise of +-amplitude per axis), to defeat PX4's stuck-sensor
 * detection.
 *
 * PX4's sensor data validators (src/modules/sensors/data_validator/DataValidator.cpp) flag a sensor whose value is
 * exactly the same for 100 consecutive samples as STALE and drop it (`sensors status`: "state: STALE", then
 * "ekf2 missing data" and no arming). A real sensor always has noise; the engine's synthesized sensors are noise
 * free and constant at rest. This is an adapter-side workaround for that PX4 behaviour, not a sensor model: the
 * amplitudes are only about as large as the quantization step of PX4's simulated IMU (gyro about 1 mrad/s,
 * accelerometer about 5 mm/s^2). Disable it when the sensors carry their own noise. Deterministic (fixed seed).
 */
struct SensorDither {
  bool enabled = true;          ///< Apply the dither.
  double gyro_rad_s = 2e-3;     ///< Amplitude on each gyro axis, rad/s.
  double accel_m_s2 = 0.03;     ///< Amplitude on each accelerometer axis, m/s^2.
  double mag_gauss = 2e-3;      ///< Amplitude on each magnetometer axis, gauss.
  double pressure_pa = 0.5;     ///< Amplitude on the static pressure, Pa.
  double temperature_c = 0.01;  ///< Amplitude on the temperature, degrees Celsius.
};

/// @brief Tunables of Px4Session. The defaults suit PX4 SITL.
struct Px4SessionConfig {
  SensorDither dither;                         ///< Stuck-sensor guard, see SensorDither.
  NoGpsMode no_gps_mode = NoGpsMode::kAltctl;  ///< PX4 mode implementing kNoGpsHold.
  double manual_control_rate_hz = 50;  ///< MANUAL_CONTROL rate (PX4 treats manual control as lost after ~0.5 s).
  double gps_rate_hz = 10;             ///< HIL_GPS rate, in simulation time.
  double hil_state_rate_hz = 25;       ///< HIL_STATE_QUATERNION (truth) rate, in simulation time.
  double link_timeout_s = 3;           ///< PX4 silent (no HEARTBEAT) this long means disconnected.
  double command_retry_s = 1;          ///< Interval between retries of DO_SET_MODE and ARM_DISARM.
  double mode_refused_after_s = 2.5;   ///< Wrong mode this long after a request counts as refused.
  double arm_give_up_s = 15;           ///< Stop retrying an arm request after this long.
};

/**
 * @brief The PX4 protocol session. See the file header for the model.
 *
 * Threading: single thread; all methods are called from the bridge event loop.
 */
class Px4Session {
 public:
  /**
   * @brief Create a session.
   * @param config Tunables.
   */
  explicit Px4Session(const Px4SessionConfig& config = {});

  /// @brief PX4 connected to the HIL TCP port: start a fresh lockstep session.
  /// @param now Current monotonic time.
  void onHilConnected(fc::TimePoint now);
  /// @brief The HIL TCP connection closed.
  void onHilDisconnected();
  /// @brief Bytes received on the HIL link (any chunking).
  /// @param data Received bytes.
  /// @param now Current monotonic time.
  void feedHil(std::span<const std::uint8_t> data, fc::TimePoint now);
  /// @brief Bytes received on the pilot/telemetry UDP link (one datagram at a time is fine).
  /// @param data Received bytes.
  /// @param now Current monotonic time.
  void feedMavlink(std::span<const std::uint8_t> data, fc::TimePoint now);

  /**
   * @brief A new sensor frame: encode HIL_SENSOR (and HIL_GPS / HIL_STATE_QUATERNION at their rates) for PX4.
   *
   * Dropped if the HIL link is down, or if time_s does not advance (PX4's clock cannot run backwards; an engine
   * restart needs a PX4 restart). Dropped frames are counted by ignoredFrames().
   * @param frame Sensor frame (body FLU).
   * @param now Current monotonic time.
   */
  void onSensors(const fc::SensorFrame& frame, fc::TimePoint now);
  /**
   * @brief The pilot command: remember it; MANUAL_CONTROL, mode and arm commands are produced by poll().
   * @param input Pilot input.
   * @param now Current monotonic time.
   */
  void onPilot(const fc::PilotInput& input, fc::TimePoint now);
  /**
   * @brief Advance timers: MANUAL_CONTROL at its rate, mode and arm commands and their retries, link state.
   * @param now Current monotonic time.
   */
  void poll(fc::TimePoint now);

  /** @return Bytes to write to the HIL link (cleared). */
  Bytes takeHilTx() { return hil_.takeTx(); }
  /** @return Bytes to send to PX4's onboard MAVLink instance (cleared). */
  Bytes takeMavlinkTx() { return mav_.takeTx(); }
  /** @return The newest motor command since the last call, or std::nullopt. */
  std::optional<fc::MotorCommand> takeMotors();
  /** @return The current telemetry snapshot (provenance flags not set). */
  fc::FcTelemetry telemetry() const;
  /** @return Number of sensor frames dropped because the time did not advance or the HIL link was down. */
  std::uint64_t ignoredFrames() const { return ignored_frames_; }
  /** @return True if the HIL TCP link is up. */
  bool hilConnected() const { return hil_connected_; }

  /// @brief Return to the power-on state (link states, estimates, pending commands).
  void reset();

 private:
  /// @brief Add the configured dither to a frame (in place).
  void applyDither(fc::SensorFrame& frame);
  /// @brief Next uniform random number in [-1, 1) (xorshift64, fixed seed).
  double nextUniform();
  /// @brief Handle one message from the HIL link.
  void onHilMessage(const mavlink_message_t& msg);
  /// @brief Handle one message from the pilot link.
  void onMavlinkMessage(const mavlink_message_t& msg, fc::TimePoint now);
  /// @brief Whether PX4 is heartbeating and the HIL link is up.
  bool connected(fc::TimePoint now) const;
  /// @brief Whether the requested mode has not been entered (denied, or too long).
  bool modeRefused(fc::TimePoint now) const;

  Px4SessionConfig config_;
  MavlinkLink hil_;  ///< HIL link framing (source: the simulator, system 1).
  MavlinkLink mav_;  ///< Pilot link framing (source: a companion computer).
  Px4Telemetry telemetry_;

  bool hil_connected_ = false;
  std::optional<double> last_sensor_time_s_;
  std::optional<double> last_gps_time_s_;
  std::optional<double> last_hil_state_time_s_;
  std::uint64_t ignored_frames_ = 0;
  fc::TimePoint last_poll_{};
  bool connected_ = false;  ///< As of the last poll().

  std::optional<fc::MotorCommand> motors_;
  std::uint64_t rng_state_ = 0x9E3779B97F4A7C15ull;  ///< Dither PRNG state.

  // Pilot side.
  std::optional<fc::PilotInput> pilot_;
  fc::TimePoint next_manual_{};
  std::optional<fc::FlightMode> requested_mode_;
  fc::TimePoint mode_requested_at_{};
  fc::TimePoint last_set_mode_{};
  bool set_mode_sent_ = false;
  bool set_mode_denied_ = false;
  bool arm_pending_ = false;
  fc::TimePoint arm_requested_at_{};
  fc::TimePoint last_arm_cmd_{};
  bool arm_sent_ = false;
  fc::TimePoint last_disarm_cmd_{};
  bool disarm_sent_ = false;
};

}  // namespace astrosim::px4
