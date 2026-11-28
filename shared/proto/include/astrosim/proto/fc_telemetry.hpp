#pragma once
/**
 * @file fc_telemetry.hpp
 * @brief AstroSim flight computer telemetry protocol v1: message types and wire encoding.
 *
 * A separate stream from the engine protocol (wire.hpp): the flight computer (FC) publishes its own view of the
 * aircraft (estimates, modes, warnings) to avionics displays. Same framing idea as the engine protocol (one
 * message per UDP datagram, little-endian, fixed size) but its own magic, version and port. The normative
 * specification is shared/proto/FC_TELEMETRY.md.
 * @verbatim
   header      (8):  magic u16 = 0xA5FC | version u8 | type u8 | seq u32
   FcTelemetry (+87): time_s f64 | status u8 | mode u8 | gps_fix u8 | gps_sats u8 | provenance u8 | warnings u16 |
                      roll f32 | pitch f32 | heading f32 | alt_msl f32 | alt_agl f32 | vspeed f32 |
                      ground_speed f32 | course f32 | battery_pct f32 | battery_v f32 | motors f32[8]
   Subscribe   (+0)
   @endverbatim
 * Standard C++20 only: no OS headers.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "astrosim/proto/messages.hpp"

namespace astrosim::proto {

/// @brief The flight modes of the Jetson ONE flight computer. Exactly two; both command attitude ANGLE (never rate).
enum class FlightMode : std::uint8_t {
  kGpsHold = 0,    ///< Attitude-angle sticks with GPS position hold.
  kNoGpsHold = 1,  ///< Attitude-angle sticks without GPS (no position hold).
};

/// @brief Bits of FcTelemetryMsg::warnings (failsafe and warning mask).
enum FcWarning : std::uint16_t {
  kWarnGpsLost = 1,        ///< GPS fix lost or unusable.
  kWarnEkfUnhealthy = 2,   ///< State estimator is not healthy.
  kWarnPilotLost = 4,      ///< Pilot (RC) input lost.
  kWarnLowBattery = 8,     ///< Battery low.
  kWarnMotorFailure = 16,  ///< A motor failure was detected.
};

/// @brief Bits of FcTelemetryMsg::provenance: how trustworthy/temporary the data feeding the FC is.
enum FcProvenance : std::uint8_t {
  kSensorsSynthesized = 1,  ///< TEMPORARY: sensors were synthesized from engine truth state, not a sensor model.
  kPilotSpoofed = 2,        ///< TEMPORARY: pilot input is a scripted spoof, not a real pilot.
};

/// @brief GPS fix type values carried in FcTelemetryMsg::gps_fix.
enum FcGpsFix : std::uint8_t {
  kGpsNoFix = 0,  ///< No fix.
  kGps2dFix = 2,  ///< 2D fix.
  kGps3dFix = 3,  ///< 3D fix.
};

/**
 * @brief Avionics telemetry: the flight computer's own view of the aircraft.
 *
 * All values are the FC's estimate or state, never engine truth. Frames: heading and course are measured
 * clockwise from true north; vertical speed is positive up.
 */
struct FcTelemetryMsg {
  double time_s = 0;                       ///< Simulation time of the sensor frame this was derived from, s.
  bool connected = false;                  ///< The FC (adapter link) is alive and talking.
  bool armed = false;                      ///< Motors armed.
  bool mode_refused = false;               ///< The requested FlightMode was refused (e.g. GPS hold, no fix).
  FlightMode mode = FlightMode::kGpsHold;  ///< Active flight mode.
  std::uint8_t gps_fix = 0;                ///< FcGpsFix value.
  std::uint8_t gps_sats = 0;               ///< Satellites used.
  std::uint8_t provenance = 0;             ///< Bitwise OR of FcProvenance.
  std::uint16_t warnings = 0;              ///< Bitwise OR of FcWarning.
  float roll_rad = 0;                      ///< Estimated roll, rad, right wing down positive.
  float pitch_rad = 0;                     ///< Estimated pitch, rad, nose up positive.
  float heading_rad = 0;                   ///< Estimated heading, rad, clockwise from north, [0, 2*pi).
  float alt_msl_m = 0;                     ///< Altitude above mean sea level, m.
  float alt_agl_m = 0;                     ///< Altitude above ground level, m.
  float vspeed_mps = 0;                    ///< Vertical speed, m/s, positive up.
  float ground_speed_mps = 0;              ///< Ground speed, m/s.
  float course_rad = 0;                    ///< Course over ground, rad, clockwise from north, [0, 2*pi).
  float battery_pct = 0;                   ///< Battery remaining, percent (placeholder until a battery model).
  float battery_v = 0;                     ///< Battery voltage, V (placeholder until a battery model).
  std::array<float, kNumMotors> motors{};  ///< Motor outputs actually commanded, [0,1], protocol motor order.

  /// @brief Member-wise equality.
  friend bool operator==(const FcTelemetryMsg&, const FcTelemetryMsg&) = default;
};

/// @brief Bits of the wire status byte.
enum FcStatusBits : std::uint8_t {
  kStatusConnected = 1,    ///< FcTelemetryMsg::connected.
  kStatusArmed = 2,        ///< FcTelemetryMsg::armed.
  kStatusModeRefused = 4,  ///< FcTelemetryMsg::mode_refused.
};

}  // namespace astrosim::proto

namespace astrosim::proto::fc_wire {

inline constexpr std::uint16_t kMagic = 0xA5FC;      ///< First two bytes of every datagram (LE: 0xFC 0xA5).
inline constexpr std::uint8_t kVersion = 1;          ///< Protocol version; any layout change bumps it.
inline constexpr std::uint16_t kDefaultPort = 8766;  ///< Default UDP port the FC telemetry server listens on.
inline constexpr std::size_t kHeaderSize = 8;        ///< Bytes in the common header.
/// @brief Total datagram size of a Telemetry message (95).
inline constexpr std::size_t kTelemetrySize = kHeaderSize + 8 + 5 + 2 + 4 * 10 + 4 * kNumMotors;
/// @brief Total datagram size of a Subscribe message (8).
inline constexpr std::size_t kSubscribeSize = kHeaderSize;
/// @brief Largest valid datagram.
inline constexpr std::size_t kMaxDatagramSize = kTelemetrySize;

/// @brief Message type identifier (header byte 3).
enum class Type : std::uint8_t {
  Telemetry = 1,  ///< FcTelemetryMsg, FC -> subscribers.
  Subscribe = 2,  ///< SubscribeMsg, display -> FC.
};

/// @brief Why decode() rejected a datagram. Checked in declaration order.
enum class DecodeError : std::uint8_t {
  TooShort,     ///< Shorter than the header.
  BadMagic,     ///< Magic is not kMagic.
  BadVersion,   ///< Version is not kVersion.
  UnknownType,  ///< Type byte is not a known Type.
  BadSize,      ///< Known type, but the length does not match it exactly.
  BadValue,     ///< The mode byte is not a defined FlightMode.
};

/// @brief Any decodable message.
using Message = std::variant<FcTelemetryMsg, SubscribeMsg>;

/// @brief A decoded datagram: header sequence number plus message.
struct Packet {
  std::uint32_t seq = 0;  ///< Sender's sequence number (wraps).
  Message msg;            ///< Decoded message body.
};

/// @brief Either a decoded packet or the reason it was rejected.
using DecodeResult = std::variant<Packet, DecodeError>;

/**
 * @brief Encode a telemetry datagram.
 * @param msg Message to encode.
 * @param seq Sender sequence number.
 * @return Exactly kTelemetrySize bytes.
 */
std::vector<std::byte> encode(const FcTelemetryMsg& msg, std::uint32_t seq);

/**
 * @brief Encode a Subscribe datagram.
 * @param msg Message to encode (no payload).
 * @param seq Sender sequence number.
 * @return Exactly kSubscribeSize bytes.
 */
std::vector<std::byte> encode(const SubscribeMsg& msg, std::uint32_t seq);

/**
 * @brief Decode one datagram.
 * @param datagram Complete datagram bytes.
 * @return The packet, or the first DecodeError found. Float values are not sanitized.
 */
DecodeResult decode(std::span<const std::byte> datagram);

/**
 * @brief Human-readable name of a decode error, for logs and stats.
 * @param e Error value.
 * @return Static string such as "bad_magic".
 */
const char* toString(DecodeError e);

}  // namespace astrosim::proto::fc_wire
