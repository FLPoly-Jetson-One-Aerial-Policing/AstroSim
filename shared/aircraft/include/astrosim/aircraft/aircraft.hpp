/**
 * @file aircraft.hpp
 * @brief Aircraft data file (TOML): schema types, parsing and validation.
 *
 * The aircraft file is the single source of truth for the aircraft's physical data (mass properties, collision
 * shapes, rotors, drag). There are no default values: every key is required, unknown keys are rejected, and every
 * error names the offending key path (e.g. "motor[3].xyz_m"). See shared/aircraft/AIRCRAFT.md for the schema.
 *
 * Frame: FLU body frame (x forward, y left, z up), SI units; the unit is part of each key name.
 * Portable C++20, no OS or Gazebo dependencies, so engine, tools and display can all use it.
 */
#pragma once
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "astrosim/aircraft/mass.hpp"

namespace astrosim::aircraft {

/** @brief Schema version this parser understands (the file's required `schema_version`). */
inline constexpr long long kSchemaVersion = 1;

/** @brief Thrown for an unreadable, malformed or invalid aircraft file. The message names the offending key. */
class AircraftError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/** @brief Box primitive. */
struct Box {
  Vec3 size_m;  ///< Full extents along the primitive's local x, y, z; each > 0.
};
/** @brief Cylinder primitive; its axis is the primitive's local z. */
struct Cylinder {
  double radius_m;  ///< Radius, > 0.
  double length_m;  ///< Length along local z, > 0.
};
/** @brief Sphere primitive. */
struct Sphere {
  double radius_m;  ///< Radius, > 0.
};
/** @brief One collision primitive shape. */
using Geometry = std::variant<Box, Cylinder, Sphere>;

/** @brief One body collision primitive (`[[collision]]`). */
struct Collision {
  std::string name;  ///< Unique among collisions; matches [A-Za-z0-9_]+.
  Geometry geometry;  ///< Shape and size.
  Vec3 xyz_m;         ///< Primitive origin in the body frame.
  Vec3 rpy_rad;       ///< Orientation, SDF fixed-axis roll-pitch-yaw.
};

/** @brief A motor + propeller definition shared by several motors (`[[rotor_type]]`). */
struct RotorType {
  std::string name;                    ///< Unique among rotor types; matches [A-Za-z0-9_]+.
  double prop_radius_m;                ///< Propeller radius, > 0.
  double collision_thickness_m;        ///< Thickness of the prop-disc collision cylinder, > 0.
  double rotating_mass_kg;             ///< Mass of the rotating parts (prop + motor rotor), > 0.
  Inertia rotating_inertia_kg_m2;      ///< Spin inertia about the hub; file gives ixx, iyy, izz only (products 0).
  double max_rot_velocity_rad_s;       ///< Rotor speed at full power, > 0.
  double motor_constant_n_s2;          ///< Thrust = motor_constant * w^2, N/(rad/s)^2, > 0.
  double moment_constant_m;            ///< Drag torque / thrust, m, > 0.
  double time_constant_up_s;           ///< First-order spin-up time constant, > 0.
  double time_constant_down_s;         ///< First-order spin-down time constant, > 0.
  double rotor_drag_coefficient;       ///< Rotor (blade-flapping) drag coefficient, >= 0.
  double rolling_moment_coefficient;   ///< Rolling moment coefficient, >= 0.
};

/** @brief Rotor spin direction seen from above (looking down -z). */
enum class Direction { kCcw, kCw };

/** @brief One motor (`[[motor]]`). Array order is the protocol motor index and Jetson's motor numbering. */
struct Motor {
  std::size_t rotor_type;  ///< Index into Aircraft::rotor_types (resolved from the file's rotor_type name).
  Vec3 xyz_m;              ///< Hub position in the body frame; the spin axis is body z.
  Direction direction;     ///< Spin direction.
};

/** @brief Airframe drag (`[drag]`), given as positive magnitudes along body x, y, z. */
struct Drag {
  Vec3 linear_n_s_m;       ///< Linear damping, N/(m/s), each >= 0.
  Vec3 quadratic_n_s2_m2;  ///< Quadratic damping, N/(m/s)^2, each >= 0.
};

/** @brief Simulation-only knobs (`[simulation]`), not aircraft data. */
struct Simulation {
  double rotor_velocity_slowdown;  ///< Gazebo MulticopterMotorModel rotorVelocitySlowdownSim, > 0.
  double odometry_rate_hz;         ///< Odometry publish rate, > 0.
};

/** @brief A complete, validated aircraft description. Only obtainable from parseAircraft()/loadAircraft(). */
struct Aircraft {
  std::string name;                   ///< Display/log label (`[aircraft] name`), non-empty.
  MassProperties airframe;            ///< Empty aircraft INCLUDING its rotors (`[airframe]`).
  MassProperties pilot;               ///< Seated pilot (`[pilot]`).
  std::vector<Collision> collisions;  ///< Body collision primitives, at least one.
  std::vector<RotorType> rotor_types; ///< Rotor definitions, at least one.
  std::vector<Motor> motors;          ///< Motors in protocol order, at least one.
  Drag drag;                          ///< Airframe drag.
  Simulation simulation;              ///< Simulation knobs.

  /**
   * @brief The rotor type of a motor.
   * @param m A motor of this aircraft.
   * @return Its RotorType.
   */
  const RotorType& rotorTypeOf(const Motor& m) const { return rotor_types.at(m.rotor_type); }
};

/**
 * @brief Mass properties of the loaded aircraft (airframe + pilot).
 * @param a Aircraft.
 * @return combine(a.airframe, a.pilot).
 */
MassProperties loadedMass(const Aircraft& a);

/**
 * @brief Mass properties of one motor's rotating parts, placed at its hub.
 * @param a Aircraft.
 * @param motor Motor index.
 * @return rotating_mass_kg at motors[motor].xyz_m with rotating_inertia_kg_m2.
 * @throws std::out_of_range if @p motor is out of range.
 */
MassProperties rotorMass(const Aircraft& a, std::size_t motor);

/**
 * @brief Parse and validate aircraft TOML text.
 * @param toml_text TOML document.
 * @return The validated aircraft.
 * @throws AircraftError on syntax errors, missing or unknown keys, wrong types, out-of-range values, bad
 *         references or non-physical inertia. The message starts with the key path.
 */
Aircraft parseAircraft(std::string_view toml_text);

/**
 * @brief Read, parse and validate an aircraft file.
 * @param path Path to the TOML file.
 * @return The validated aircraft.
 * @throws AircraftError if the file cannot be read or is invalid (message prefixed with the path).
 */
Aircraft loadAircraft(const std::filesystem::path& path);

}  // namespace astrosim::aircraft
