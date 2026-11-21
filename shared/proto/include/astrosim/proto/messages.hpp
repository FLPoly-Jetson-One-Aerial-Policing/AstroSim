#pragma once
/**
 * @file messages.hpp
 * @brief AstroSim engine protocol v1 message types.
 *
 * Plain value types exchanged between the flight computer, the physics engine and displays. The normative
 * specification, including byte layouts, is shared/proto/PROTOCOL.md.
 *
 * Standard C++20 only: no OS headers and no engine (Gazebo) types, so any client on any OS can use it.
 */
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace astrosim::proto {

/// @brief Number of motors carried by Control and State (Jetson ONE: 8). Changing it is a protocol version bump.
inline constexpr std::size_t kNumMotors = 8;

/**
 * @brief Motor command from the flight computer to the engine.
 *
 * Follows the PX4 `actuator_motors` convention for unidirectional motors.
 */
struct ControlMsg {
  /// @brief Normalized power per protocol motor, in [0,1]. Non-finite values mean "off" (see sanitize()).
  std::array<float, kNumMotors> power{};

  /// @brief Member-wise equality.
  friend bool operator==(const ControlMsg&, const ControlMsg&) = default;
};

/// @brief Bit flags carried in StateMsg::flags.
enum StateFlags : std::uint8_t {
  kEngineOk = 1,        ///< Engine state (odometry) is fresh.
  kControlActive = 2,   ///< A controller holds the lease and is within the deadman window.
  kDeadmanTripped = 4,  ///< Motors were zeroed because Control stopped arriving.
};

/**
 * @brief Aircraft state from the engine to subscribers (e.g. displays).
 *
 * World frame is ENU (x East, y North, z Up); body frame is FLU (x Forward, y Left, z Up).
 */
struct StateMsg {
  double sim_time = 0;                          ///< Simulation time, s.
  std::array<double, 3> pos{};                  ///< Position of the body origin, m, world frame.
  std::array<float, 4> quat{0, 0, 0, 1};        ///< Attitude quaternion (x, y, z, w), rotating body to world.
  std::array<float, 3> vel{};                   ///< Linear velocity, m/s, world frame.
  std::array<float, 3> ang_vel{};               ///< Angular velocity, rad/s, body frame.
  std::array<float, kNumMotors> motor_speed{};  ///< Actual rotor speed per protocol motor, rad/s.
  std::uint8_t flags = 0;                       ///< Bitwise OR of StateFlags.
  std::uint32_t echo_seq = 0;                   ///< Sequence number of the last Control the engine applied.

  /// @brief Member-wise equality.
  friend bool operator==(const StateMsg&, const StateMsg&) = default;
};

/**
 * @brief Subscription request/keepalive from any client to the engine. Has no payload.
 *
 * A client must re-send it (or Control) periodically to keep receiving StateMsg.
 */
struct SubscribeMsg {
  /// @brief All SubscribeMsg values are equal.
  friend bool operator==(const SubscribeMsg&, const SubscribeMsg&) = default;
};

/**
 * @brief Make one motor power value safe to apply.
 * @param v Raw power value.
 * @return @p v clamped to [0,1]; NaN and ±Inf map to 0 (PX4 uses NaN for "disarmed").
 */
inline float sanitizePower(float v) {
  if (!std::isfinite(v)) return 0.f;
  return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

/**
 * @brief Apply sanitizePower() to every motor of a Control message.
 * @param c Control message as received.
 * @return A copy with every power value in [0,1].
 */
inline ControlMsg sanitize(const ControlMsg& c) {
  ControlMsg out;
  for (std::size_t i = 0; i < kNumMotors; ++i) out.power[i] = sanitizePower(c.power[i]);
  return out;
}

}  // namespace astrosim::proto
