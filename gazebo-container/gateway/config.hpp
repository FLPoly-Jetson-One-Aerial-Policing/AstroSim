/**
 * @file config.hpp
 * @brief Gateway configuration: TOML file plus ASTROSIM_* environment overrides, validated up front.
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "astrosim/proto/messages.hpp"

namespace astrosim::gateway {

/** @brief Thrown for any unreadable, malformed or invalid configuration. The message names the offending key. */
class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/** @brief Environment variable lookup; returns nullopt when the variable is unset. */
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/**
 * @brief EnvLookup backed by std::getenv.
 * @param name Environment variable name.
 * @return The value, or std::nullopt if the variable is unset.
 */
std::optional<std::string> processEnv(std::string_view name);

/** @brief Complete gateway configuration. Defaults match config/gateway.toml. */
struct Config {
  /** @brief UDP endpoint settings. */
  struct Network {
    std::uint16_t port = 8765;        ///< UDP port, 1..65535.
    std::string bind = "0.0.0.0";     ///< IPv4 dotted-quad address to bind.
    std::size_t max_subscribers = 8;  ///< Maximum concurrent State subscribers, >= 1.
  };
  /** @brief Rates and timeouts. All values are > 0. */
  struct Timing {
    double state_rate_hz = 60;        ///< State broadcast rate.
    double deadman_timeout_s = 0.5;   ///< Silence after which motors are zeroed.
    double lease_timeout_s = 1.0;     ///< Controller silence after which the lease is released.
    double subscriber_timeout_s = 5;  ///< Subscriber silence after which it is dropped.
    double engine_stale_s = 0.5;      ///< Odometry age after which the engine is reported not OK.
    double stats_interval_s = 10;     ///< Period of the stats log line.
  };
  /** @brief Engine (gz) bindings. Opaque to the core logic. */
  struct Engine {
    std::string model = "jetson_one";                                            ///< Model name.
    std::string odometry_topic = "/model/jetson_one/odometry";                   ///< Odometry topic.
    std::string motor_command_topic = "/jetson_one/gazebo/command/motor_speed";  ///< Actuator command topic.
    /// Joint-state topic (gz::msgs::Model) carrying the rotor joint velocities. Empty disables motor speed feedback.
    std::string motor_speed_topic = "/jetson_one/joint_state";
    /// Rotor joint name in the joint-state message; "{actuator}" is replaced by each motor's actuator_index.
    std::string motor_joint_name = "jetson_one/rotor_{actuator}_joint";
    /// Multiplier from joint velocity to rotor speed (the motor model's rotorVelocitySlowdownSim), > 0.
    double motor_speed_scale = 10.0;
  };
  /** @brief Mapping of one protocol motor to a physical actuator. */
  struct Motor {
    int actuator_index = 0;  ///< Index in the engine's actuator array, >= 0 and unique.
    double max_rad_s = 800;  ///< Rotor speed at power 1.0, > 0.
  };
  /** @brief Logging settings. */
  struct Log {
    std::string level = "info";  ///< One of trace, debug, info, warn, error.
  };

  Network network;                                                 ///< Network settings ([network]).
  Timing timing;                                                   ///< Rates and timeouts ([timing]).
  Engine engine;                                                   ///< Engine model and topics ([engine]).
  std::array<Motor, proto::kNumMotors> motors = identityMotors();  ///< Indexed by protocol motor number.
  Log log;                                                         ///< Logging settings ([log]).

 private:
  static std::array<Motor, proto::kNumMotors> identityMotors() {
    std::array<Motor, proto::kNumMotors> m;
    for (std::size_t i = 0; i < m.size(); ++i) m[i].actuator_index = static_cast<int>(i);
    return m;
  }
};

/**
 * @brief Parse and validate configuration text.
 * @param toml_text TOML document.
 * @param env Source of ASTROSIM_<SECTION>_<KEY> overrides for scalar keys (motors are file-only).
 * @return The validated configuration.
 * @throws ConfigError on syntax errors, unknown keys, wrong types, out-of-range values or malformed overrides.
 */
Config parseConfig(std::string_view toml_text, const EnvLookup& env);

/**
 * @brief Read, parse and validate a configuration file.
 * @param path Path to the TOML file.
 * @param env Source of environment overrides.
 * @return The validated configuration.
 * @throws ConfigError if the file cannot be read or is invalid.
 */
Config loadConfig(const std::filesystem::path& path, const EnvLookup& env);

}  // namespace astrosim::gateway
