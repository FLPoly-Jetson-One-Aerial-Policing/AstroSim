/*
 * Flight computer bridge configuration: TOML file plus ASTROSIM_* environment overrides, validated up front.
 *
 * Same conventions as the engine gateway: every scalar key can be overridden by ASTROSIM_<SECTION>_<KEY>, unknown
 * keys are rejected, and every error names the offending key. The pilot script (an array of tables) is file-only.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "astrosim/fc/scripted_pilot.hpp"
#include "astrosim/fc/synth_sensors_TEMPORARY.hpp"

namespace astrosim::fcbridge {

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

/** @brief Complete bridge configuration. Defaults match config/fc.toml. */
struct Config {
  /** @brief The engine to talk to ([engine]). */
  struct Engine {
    std::string host = "127.0.0.1";  ///< Engine host name or IPv4 address (resolved by the executable).
    std::uint16_t port = 8765;       ///< Engine UDP port, 1..65535.
  };
  /** @brief Rates and timeouts ([timing]). All values are > 0. */
  struct Timing {
    double control_rate_hz = 250;       ///< Rate of Control messages to the engine.
    double subscribe_interval_s = 0.5;  ///< Subscribe keepalive period; at most 1 s (the engine drops at 5 s).
    double pilot_rate_hz = 50;          ///< Rate at which the PilotSource is sampled and handed to the FC.
    double fc_timeout_s = 0.5;          ///< FC silence (no new MotorCommand) after which zero power is sent.
    double telemetry_rate_hz = 30;      ///< Rate of FcTelemetry broadcasts.
    double stats_interval_s = 10;       ///< Period of the stats log line.
  };
  /** @brief The FC telemetry server ([telemetry]). */
  struct Telemetry {
    std::string bind = "0.0.0.0";        ///< IPv4 dotted-quad address to bind.
    std::uint16_t port = 8766;           ///< UDP port, 1..65535.
    std::size_t max_subscribers = 8;     ///< Maximum concurrent subscribers, >= 1.
    double subscriber_timeout_s = 5;     ///< Subscriber silence after which it is dropped.
  };
  /** @brief Which flight computer and sensor source to use ([fc]). */
  struct Fc {
    /// "px4" (the PX4 SITL adapter, built with ASTROSIM_BUILD_PX4) or "null" (TEST ONLY: outputs zero power).
    std::string adapter = "px4";
    /// TEMPORARY: the only accepted value is "synthesized_temporary" (sensors synthesized from engine truth).
    std::string sensor_source = "synthesized_temporary";
  };
  /** @brief World origin used by the synthesized sensors ([home]). */
  struct Home {
    double lat_deg = 47.397742;  ///< Latitude of the world origin, degrees, [-90, 90].
    double lon_deg = 8.545594;   ///< Longitude of the world origin, degrees, [-180, 180].
    double alt_m = 488.0;        ///< Altitude of the world origin above mean sea level, m.
  };
  /** @brief World magnetic field in ENU, gauss ([mag]). */
  struct Mag {
    double east_gauss = 0.0;    ///< East component.
    double north_gauss = 0.2;   ///< North component.
    double up_gauss = -0.4;     ///< Up component (negative in the northern hemisphere).
  };
  /**
   * @brief PX4 adapter settings ([px4]); see fc-container/px4/README.md.
   *
   * Two links to PX4 inside the FC container: the HIL link (this process listens on TCP, PX4's simulator_mavlink
   * connects) and the MAVLink link to PX4's "onboard" instance (UDP; this process listens on mavlink_local_port,
   * PX4 sends there and listens on mavlink_port). The defaults are those of PX4 SITL instance 0.
   */
  struct Px4 {
    std::uint16_t hil_port = 4560;               ///< TCP port the adapter listens on for PX4's HIL connection.
    std::uint16_t mavlink_local_port = 14540;    ///< UDP port this process binds (PX4's onboard instance sends here).
    std::string mavlink_host = "127.0.0.1";      ///< IPv4 address of PX4's onboard MAVLink instance.
    std::uint16_t mavlink_port = 14580;          ///< UDP port PX4's onboard MAVLink instance listens on.
    std::string no_gps_mode = "altctl";          ///< PX4 mode for kNoGpsHold: "altctl" or "stabilized".
    double max_tilt_deg = 25;                    ///< Tilt of a full roll/pitch stick (MPC_MAN_TILT_MAX), degrees, (0, 60].
    double max_yaw_rate_dps = 45;                ///< Yaw rate of a full yaw stick (MPC_MAN_Y_MAX), deg/s, (0, 360].
    /// Add a tiny dither to the sensors: PX4 treats a constant (noise-free) sensor as stuck. See px4/README.md.
    bool sensor_dither = true;
  };
  /**
   * @brief SPOOFED, TEMPORARY pilot script ([pilot], [[pilot.script]]).
   * @warning Not a real pilot. Replaced when real pilot input exists.
   */
  struct Pilot {
    fc::Interpolation interpolation = fc::Interpolation::kHold;  ///< Behaviour of the axes between keyframes.
    std::vector<fc::Keyframe> script;                            ///< Keyframes, strictly increasing time_s.
  };
  /** @brief Logging settings ([log]). */
  struct Log {
    std::string level = "info";  ///< One of trace, debug, info, warn, error.
  };

  Engine engine;        ///< [engine].
  Timing timing;        ///< [timing].
  Telemetry telemetry;  ///< [telemetry].
  Fc fc;                ///< [fc].
  Home home;            ///< [home].
  Mag mag;              ///< [mag].
  Px4 px4;              ///< [px4].
  Pilot pilot;          ///< [pilot].
  Log log;              ///< [log].

  /**
   * @brief Build the SynthConfig for fc::TemporarySynthSensors from [home] and [mag].
   * @return The sensor synthesis settings.
   */
  fc::SynthConfig synthConfig() const;
};

/**
 * @brief Parse and validate configuration text.
 * @param toml_text TOML document.
 * @param env Source of ASTROSIM_<SECTION>_<KEY> overrides for scalar keys.
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

}  // namespace astrosim::fcbridge
