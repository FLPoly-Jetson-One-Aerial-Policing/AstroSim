#pragma once
/**
 * @file types.hpp
 * @brief Value types of the flight computer (FC) interface: sensors in, pilot input in, motors and telemetry out.
 *
 * FC-agnostic: nothing here is PX4-, ArduPilot- or Jetson-specific. Adapters translate to and from these types.
 * Standard C++20 only: no OS headers and no MAVLink. Frames and units are defined in FC_INTERFACE.md; in short,
 * world is ENU (x East, y North, z Up), body is FLU (x Forward, y Left, z Up), SI units.
 */
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "astrosim/proto/fc_telemetry.hpp"
#include "astrosim/proto/messages.hpp"

namespace astrosim::fc {

/// @brief Number of motors (matches the engine protocol).
inline constexpr std::size_t kNumMotors = proto::kNumMotors;

/// @brief Monotonic time point. Always injected by the caller; nothing in this library reads a clock.
using TimePoint = std::chrono::steady_clock::time_point;

/// @brief A 3-vector (x, y, z) in the frame stated by the field that holds it.
using Vec3 = std::array<double, 3>;

/// @brief A unit quaternion (x, y, z, w), Hamilton convention.
using Quat = std::array<double, 4>;

/**
 * @brief The flight modes of the Jetson ONE flight computer. Exactly two.
 *
 * Both modes command attitude ANGLE from the roll and pitch sticks, never rate. There is no acro mode.
 * Each adapter maps these two modes onto its flight computer's native modes. Identical to the wire enum.
 */
using FlightMode = proto::FlightMode;

/// @brief Avionics telemetry from the FC's own view (estimates, not engine truth). Identical to the wire message.
using FcTelemetry = proto::FcTelemetryMsg;

/// @brief GPS measurement (geodetic position, NED velocity).
struct Gps {
  double lat_deg = 0;              ///< Latitude, degrees, WGS84.
  double lon_deg = 0;              ///< Longitude, degrees, WGS84.
  double alt_m = 0;                ///< Altitude above mean sea level, m.
  std::array<float, 3> vel_ned{};  ///< Velocity, m/s, north-east-down.
  std::uint8_t fix = 0;            ///< Fix type: 0 none, 2 2D, 3 3D (see proto::FcGpsFix).
  std::uint8_t satellites = 0;     ///< Satellites used.

  /// @brief Member-wise equality.
  friend bool operator==(const Gps&, const Gps&) = default;
};

/// @brief Ground truth of the aircraft, for adapters that want it (for example HIL state logging).
struct Truth {
  Quat quat{0, 0, 0, 1};  ///< Attitude, body FLU to world ENU.
  Vec3 pos_enu{};         ///< Position, m, world ENU relative to the world origin.
  Vec3 vel_enu{};         ///< Velocity, m/s, world ENU.

  /// @brief Member-wise equality.
  friend bool operator==(const Truth&, const Truth&) = default;
};

/// @brief One sample of all sensors, in body FLU axes. Produced by a SensorSource, consumed by a FlightComputer.
struct SensorFrame {
  double time_s = 0;              ///< Simulation time of the sample, s.
  Vec3 gyro{};                    ///< Angular rate, rad/s, body FLU.
  Vec3 accel{};                   ///< Specific force (what an accelerometer reads), m/s^2, body FLU.
  Vec3 mag{};                     ///< Magnetic field, gauss, body FLU.
  double baro_pressure_pa = 0;    ///< Static pressure, Pa.
  double baro_altitude_m = 0;     ///< Pressure altitude above mean sea level, m.
  double baro_temperature_c = 0;  ///< Air temperature, degrees Celsius.
  Gps gps;                        ///< GPS measurement.
  std::optional<Truth> truth;     ///< Ground truth, if the source provides it.

  /// @brief Member-wise equality.
  friend bool operator==(const SensorFrame&, const SensorFrame&) = default;
};

/**
 * @brief Pilot command, FC-agnostic.
 *
 * The roll and pitch sticks command attitude ANGLE in both flight modes, never rate.
 */
struct PilotInput {
  float roll = 0;      ///< Roll angle demand in [-1, 1], scaled by the FC's maximum tilt. Right positive.
  float pitch = 0;     ///< Pitch angle demand in [-1, 1], scaled by the FC's maximum tilt. Nose-up positive.
  float yaw = 0;       ///< Yaw demand in [-1, 1]. Clockwise (from above) positive.
  float throttle = 0;  ///< Throttle in [0, 1].
  FlightMode mode = FlightMode::kGpsHold;  ///< Requested flight mode.
  bool arm = false;                        ///< Requested armed state.

  /// @brief Member-wise equality.
  friend bool operator==(const PilotInput&, const PilotInput&) = default;
};

/// @brief Motor command produced by the FC.
struct MotorCommand {
  std::array<float, kNumMotors> power{};  ///< Normalized power per protocol motor, [0, 1].
  bool armed = false;                     ///< The FC is armed; when false the bridge sends zero power.

  /// @brief Member-wise equality.
  friend bool operator==(const MotorCommand&, const MotorCommand&) = default;
};

}  // namespace astrosim::fc
