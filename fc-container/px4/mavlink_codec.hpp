/**
 * @file mavlink_codec.hpp
 * @brief Pure MAVLink codec for the PX4 adapter: conversions between the FC interface types and MAVLink messages,
 * mode mapping, and a stream object that frames and parses MAVLink without any socket or global state.
 *
 * Everything here is pure (no sockets, no clock reads, no global MAVLink channel state), so it is unit testable.
 * Frames and units follow FC_INTERFACE.md on the interface side and PX4 conventions on the MAVLink side: NED / FRD,
 * SI units except where a MAVLink field defines otherwise (HIL_SENSOR pressure in hPa, HIL_GPS in 1e-7 degrees,
 * mm and cm/s).
 *
 * The PX4 facts encoded here (HIL_SENSOR fields_updated, MANUAL_CONTROL ranges, custom_mode layout, HIL_ACTUATOR
 * mode flags) were checked against PX4-Autopilot at the pinned tag; see fc-container/px4/README.md.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The MAVLink C headers (mavlink/c_library_v2, common dialect) are included as a SYSTEM include directory so that
// -Werror applies to our code only.
#include <common/mavlink.h>

#include "astrosim/fc/types.hpp"

namespace astrosim::px4 {

/// @brief A growable byte buffer of encoded MAVLink frames.
using Bytes = std::vector<std::uint8_t>;

/// @brief MAVLink system id of PX4 SITL instance 0 (PX4 uses instance + 1).
inline constexpr std::uint8_t kPx4SystemId = 1;
/// @brief MAVLink component id of the PX4 autopilot (MAV_COMP_ID_AUTOPILOT1).
inline constexpr std::uint8_t kPx4ComponentId = 1;
/// @brief MAVLink system id this adapter uses towards PX4's onboard instance (a companion computer).
inline constexpr std::uint8_t kAdapterSystemId = 245;
/// @brief MAVLink component id this adapter uses (MAV_COMP_ID_ONBOARD_COMPUTER).
inline constexpr std::uint8_t kAdapterComponentId = 191;

/**
 * @brief HIL_SENSOR `fields_updated` bits that PX4's simulator_mavlink tests: accel (x,y,z), gyro, mag, absolute
 * pressure, pressure altitude and temperature. Differential pressure (bit 10) is not set: there is no airspeed.
 */
inline constexpr std::uint32_t kHilSensorFieldsUpdated = 0b111u | 0b111000u | 0b111000000u | 0b1101000000000u;

/// @brief Bit of the MAVLink `base_mode` / HIL_ACTUATOR_CONTROLS `mode` meaning "armed" (MAV_MODE_FLAG_SAFETY_ARMED).
inline constexpr std::uint8_t kModeFlagArmed = 128;

/// @brief PX4 main flight modes used in `custom_mode` (px4_custom_mode.h). There is deliberately no ACRO here.
enum class Px4MainMode : std::uint8_t {
  kAltctl = 2,      ///< PX4_CUSTOM_MAIN_MODE_ALTCTL: angle sticks, barometric altitude hold, no position hold.
  kPosctl = 3,      ///< PX4_CUSTOM_MAIN_MODE_POSCTL: angle sticks, GPS position hold.
  kStabilized = 7,  ///< PX4_CUSTOM_MAIN_MODE_STABILIZED: angle sticks, direct throttle.
};

/// @brief Which PX4 mode implements FlightMode::kNoGpsHold. Both are angle based; never ACRO.
enum class NoGpsMode : std::uint8_t {
  kAltctl,      ///< ALTCTL: angle sticks plus barometric altitude hold (default).
  kStabilized,  ///< STABILIZED: angle sticks plus direct throttle.
};

/// @brief A PX4 mode as main mode plus sub mode (the sub mode is 0 for all modes used here).
struct Px4Mode {
  std::uint8_t main_mode = 0;  ///< PX4_CUSTOM_MAIN_MODE_* value.
  std::uint8_t sub_mode = 0;   ///< PX4_CUSTOM_SUB_MODE_* value.

  /// @brief Member-wise equality.
  friend bool operator==(const Px4Mode&, const Px4Mode&) = default;
};

/**
 * @brief The PX4 mode that implements an interface flight mode.
 * @param mode kGpsHold maps to POSCTL; kNoGpsHold maps to ALTCTL or STABILIZED per @p no_gps.
 * @param no_gps Implementation of kNoGpsHold.
 * @return The PX4 mode. Never ACRO.
 */
Px4Mode toPx4Mode(fc::FlightMode mode, NoGpsMode no_gps);

/**
 * @brief The interface flight mode a PX4 mode corresponds to.
 * @param mode PX4 main/sub mode.
 * @return kGpsHold for POSCTL (the plain position mode), kNoGpsHold for ALTCTL and STABILIZED; std::nullopt for
 * any other mode (manual, acro, offboard, any auto mode such as a failsafe hold or land, POSCTL sub modes).
 */
std::optional<fc::FlightMode> fromPx4Mode(const Px4Mode& mode);

/**
 * @brief Split a MAVLink HEARTBEAT `custom_mode` into main and sub mode (px4_custom_mode.h: main in bits 16-23,
 * sub in bits 24-31).
 * @param custom_mode The heartbeat's custom_mode.
 * @return The mode.
 */
Px4Mode px4ModeFromCustomMode(std::uint32_t custom_mode);

/**
 * @brief Compose a HEARTBEAT-style `custom_mode` from a mode.
 * @param mode PX4 main/sub mode.
 * @return (main << 16) | (sub << 24).
 */
std::uint32_t customModeOf(const Px4Mode& mode);

/**
 * @brief Build HIL_SENSOR from a sensor frame (sensor 0, the one PX4 locksteps on).
 *
 * Vectors are converted FLU to FRD. `time_usec` is the simulation time, so PX4's lockstep clock follows the engine.
 * Accelerometer in m/s^2 (specific force), gyro rad/s, magnetometer gauss, absolute pressure in hPa (millibar),
 * pressure altitude in m, temperature in degrees Celsius. fields_updated is kHilSensorFieldsUpdated.
 * @param frame Sensor frame (body FLU).
 * @return The message.
 */
mavlink_hil_sensor_t makeHilSensor(const fc::SensorFrame& frame);

/**
 * @brief Build HIL_GPS from a sensor frame (GPS 0).
 *
 * Latitude/longitude in 1e-7 degrees, altitude in mm, velocities in cm/s (NED), course over ground in centidegrees
 * (65535, unknown, when below 0.2 m/s). eph/epv are fixed at 0.3 m / 0.5 m because the synthesized GPS has no error
 * model (the values are in cm).
 * @param frame Sensor frame; its `gps` member is used.
 * @return The message.
 */
mavlink_hil_gps_t makeHilGps(const fc::SensorFrame& frame);

/**
 * @brief Build HIL_STATE_QUATERNION (ground truth, logged by PX4) from a sensor frame that carries truth.
 * @param frame Sensor frame.
 * @return The message, or std::nullopt if @p frame has no truth.
 *
 * Attitude is body FRD to world NED as (w, x, y, z); velocity in cm/s NED; position from the frame's GPS
 * (the synthesized GPS is truth), altitude in mm; accelerations are the specific force FRD in m/s^2 scaled by 1000,
 * which is what PX4's simulator_mavlink divides by.
 */
std::optional<mavlink_hil_state_quaternion_t> makeHilStateQuaternion(const fc::SensorFrame& frame);

/**
 * @brief Build MANUAL_CONTROL from a pilot input.
 *
 * PX4 interpretation (verified in mavlink_receiver.cpp): x is the pitch stick in [-1000, 1000] where forward
 * (positive) is nose DOWN, y the roll stick (right positive), r the yaw stick (clockwise positive), and z the
 * throttle in [0, 1000] with 500 neutral. In ALTCTL and POSCTL the throttle stick is a climb-rate demand (500 holds
 * altitude); in STABILIZED it maps 0..500..1000 to minimum..hover..maximum thrust. Inputs are clamped.
 * The interface's pitch is nose-up positive, so x = -pitch.
 * @param input Pilot input (angle demands).
 * @param target_system Target system id (PX4's MAV_SYS_ID).
 * @return The message.
 */
mavlink_manual_control_t makeManualControl(const fc::PilotInput& input, std::uint8_t target_system = kPx4SystemId);

/**
 * @brief Map HIL_ACTUATOR_CONTROLS to a motor command.
 *
 * Identity motor mapping (PX4 output i drives protocol motor i, because the generated airframe defines the rotors
 * in protocol order). `armed` comes from the mode flags. A disarmed message, or a NaN or infinite control, gives
 * power 0; finite values are clamped to [0, 1].
 * @param msg The message.
 * @return The command.
 */
fc::MotorCommand motorCommandFromActuatorControls(const mavlink_hil_actuator_controls_t& msg);

/**
 * @brief One MAVLink link: encodes outgoing messages into a byte buffer and parses incoming bytes.
 *
 * Owns its own framing state (sequence numbers, parser), so any number of links can coexist and tests are
 * deterministic. Not thread safe.
 */
class MavlinkLink {
 public:
  /**
   * @brief Create a link.
   * @param system_id Source system id of outgoing messages.
   * @param component_id Source component id of outgoing messages.
   */
  MavlinkLink(std::uint8_t system_id, std::uint8_t component_id);

  /// @brief Encode HIL_SENSOR and append it to the output.
  /// @param msg Message to encode.
  void sendHilSensor(const mavlink_hil_sensor_t& msg);
  /// @brief Encode HIL_GPS and append it to the output.
  /// @param msg Message to encode.
  void sendHilGps(const mavlink_hil_gps_t& msg);
  /// @brief Encode HIL_STATE_QUATERNION and append it to the output.
  /// @param msg Message to encode.
  void sendHilStateQuaternion(const mavlink_hil_state_quaternion_t& msg);
  /// @brief Encode MANUAL_CONTROL and append it to the output.
  /// @param msg Message to encode.
  void sendManualControl(const mavlink_manual_control_t& msg);
  /**
   * @brief Encode COMMAND_LONG MAV_CMD_COMPONENT_ARM_DISARM to PX4 and append it to the output.
   * @param arm True to arm, false to disarm.
   */
  void sendArmDisarm(bool arm);
  /**
   * @brief Encode COMMAND_LONG MAV_CMD_DO_SET_MODE (custom mode) to PX4 and append it to the output.
   * @param mode PX4 main/sub mode.
   */
  void sendSetMode(const Px4Mode& mode);
  /// @brief Encode HEARTBEAT (a companion computer) and append it to the output.
  void sendHeartbeat();

  /**
   * @brief Take the encoded bytes accumulated so far.
   * @return The bytes (the internal buffer is cleared).
   */
  Bytes takeTx();

  /**
   * @brief Feed received bytes to the parser.
   * @param data Bytes from the link (any chunking; partial frames are kept).
   * @return The complete, checksum-valid messages decoded from @p data, in order.
   */
  std::vector<mavlink_message_t> parse(std::span<const std::uint8_t> data);

 private:
  /// @brief Append a packed message to tx_.
  void append(const mavlink_message_t& msg);

  std::uint8_t system_id_;
  std::uint8_t component_id_;
  mavlink_status_t tx_status_{};  ///< Sequence counter of outgoing frames.
  mavlink_status_t rx_status_{};  ///< Parser state.
  mavlink_message_t rx_msg_{};    ///< Parser frame buffer.
  Bytes tx_;
};

}  // namespace astrosim::px4
