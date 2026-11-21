#pragma once
/**
 * @file synth_sensors_TEMPORARY.hpp
 * @brief TemporarySynthSensors: sensors SYNTHESIZED from engine truth state. TEMPORARY. Not a sensor model.
 *
 * @warning THESE SENSORS ARE SYNTHESIZED FROM ENGINE TRUTH STATE. They are noise-free, perfectly aligned and
 * unfailing: gyro, accelerometer, magnetometer, barometer and GPS are all computed from the engine's position,
 * attitude and velocity. They are a TEMPORARY stopgap so the flight computer can run end to end. They must not be
 * used to judge flight computer performance or to size any real sensor-dependent behaviour.
 * The config key is `sensor_source = "synthesized_temporary"` (the only accepted value for now), the bridge logs a
 * WARN at startup, its stats line shows `sensors=SYNTHESIZED`, and telemetry carries
 * proto::kSensorsSynthesized.
 * @todo Replace with engine-side sensors from the sensor/GPS story (separate ticket). The FlightComputer
 * interface and the adapters stay unchanged; only the SensorSource implementation is swapped. Then delete this file.
 */
#include <optional>

#include "astrosim/fc/sensor_source.hpp"

namespace astrosim::fc {

/// @brief Configuration of TemporarySynthSensors.
struct SynthConfig {
  double home_lat_deg = 0;                  ///< Geodetic latitude of the world origin, degrees, [-90, 90].
  double home_lon_deg = 0;                  ///< Geodetic longitude of the world origin, degrees, [-180, 180].
  double home_alt_m = 0;                    ///< Altitude of the world origin above mean sea level, m.
  Vec3 mag_field_enu_gauss{0.0, 0.2, -0.4}; ///< World magnetic field (East, North, Up), gauss.
  double accel_filter_tau_s = 0.02;         ///< Low-pass time constant of the finite-difference acceleration, s, >= 0.
  std::uint8_t gps_satellites = 12;         ///< Satellite count reported with the (always 3D) fix.
};

/**
 * @brief Synthesizes a SensorFrame from an engine State message.
 *
 * - gyro: the State angular velocity (already body FLU).
 * - accel: specific force R^T (dv/dt - g) from a low-passed finite difference of the State velocity over
 *   sim_time; hover reads about (0, 0, +g), free fall about 0.
 * - baro: International Standard Atmosphere at the altitude above mean sea level.
 * - mag: the configured world field rotated into the body frame.
 * - GPS: local ENU offset from the configured home converted to geodetic coordinates (WGS84 radii of curvature),
 *   NED velocity, always a 3D fix.
 *
 * Handling of time: the first State, a State with the same sim_time as the previous one, or a non-finite State
 * yields no frame; a State whose sim_time goes backwards is treated as an engine restart and resets the history.
 *
 * @warning SYNTHESIZED from truth, TEMPORARY. See the file header.
 */
class TemporarySynthSensors final : public SensorSource {
 public:
  /**
   * @brief Construct with a validated configuration.
   * @param config Home position, magnetic field and filter settings.
   * @throws std::invalid_argument if latitude/longitude are out of range or any value is non-finite or negative
   * where that is not allowed.
   */
  explicit TemporarySynthSensors(const SynthConfig& config);

  /**
   * @brief Synthesize one frame.
   * @param state Engine State message.
   * @return The frame, or std::nullopt for the first sample, a duplicate timestamp, a backwards timestamp (which
   * also resets the history) or a non-finite state.
   */
  std::optional<SensorFrame> onState(const proto::StateMsg& state) override;

  /// @brief Forget the previous sample and the filtered acceleration.
  void reset() override;

  /** @return Always true: these sensors are synthesized. */
  bool isSynthesized() const override { return true; }

 private:
  SynthConfig config_;
  bool have_previous_ = false;       ///< A previous sample exists.
  double previous_time_s_ = 0;       ///< sim_time of the previous sample.
  Vec3 previous_vel_enu_{};          ///< World velocity of the previous sample.
  bool have_accel_ = false;          ///< The acceleration filter is initialized.
  Vec3 accel_enu_{};                 ///< Low-passed world-frame kinematic acceleration, m/s^2.
};

}  // namespace astrosim::fc
