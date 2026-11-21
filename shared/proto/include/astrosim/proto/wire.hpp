#pragma once
/**
 * @file wire.hpp
 * @brief AstroSim engine protocol v1 wire encoding.
 *
 * One message per UDP datagram, little-endian, fixed size per message type:
 * @verbatim
   header    (8):    magic u16 = 0xA57A | version u8 | type u8 | seq u32
   Control   (+32):  power f32[8]
   State     (+109): sim_time f64 | pos f64[3] | quat f32[4] | vel f32[3] | ang_vel f32[3] |
                     motor_speed f32[8] | flags u8 | echo_seq u32
   Subscribe (+0)
   @endverbatim
 * Serialization is byte-wise and independent of host endianness. Standard C++20 only.
 */
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "astrosim/proto/messages.hpp"

namespace astrosim::proto::wire {

inline constexpr std::uint16_t kMagic = 0xA57A;  ///< First two bytes of every datagram (LE: 0x7A 0xA5).
inline constexpr std::uint8_t kVersion = 1;      ///< Protocol version; any layout change bumps it.
inline constexpr std::size_t kHeaderSize = 8;    ///< Bytes in the common header.
/// @brief Total datagram size of a Control message (40).
inline constexpr std::size_t kControlSize = kHeaderSize + 4 * kNumMotors;
/// @brief Total datagram size of a State message (117).
inline constexpr std::size_t kStateSize = kHeaderSize + 8 + 8 * 3 + 4 * 4 + 4 * 3 + 4 * 3 + 4 * kNumMotors + 1 + 4;
/// @brief Total datagram size of a Subscribe message (8).
inline constexpr std::size_t kSubscribeSize = kHeaderSize;
/// @brief Largest valid datagram; a receive buffer of this size (or larger) suffices.
inline constexpr std::size_t kMaxDatagramSize = kStateSize;

/// @brief Message type identifier (header byte 3).
enum class Type : std::uint8_t {
  Control = 1,    ///< ControlMsg, flight computer -> engine.
  State = 2,      ///< StateMsg, engine -> subscribers.
  Subscribe = 3,  ///< SubscribeMsg, any client -> engine.
};

/// @brief Why decode() rejected a datagram. Checked in declaration order.
enum class DecodeError : std::uint8_t {
  TooShort,     ///< Shorter than the header.
  BadMagic,     ///< Magic is not kMagic.
  BadVersion,   ///< Version is not kVersion.
  UnknownType,  ///< Type byte is not a known Type.
  BadSize,      ///< Known type, but the length does not match it exactly.
};

/// @brief Any decodable message.
using Message = std::variant<ControlMsg, StateMsg, SubscribeMsg>;

/// @brief A decoded datagram: header sequence number plus message.
struct Packet {
  std::uint32_t seq = 0;  ///< Sender's sequence number (wraps; compare with seqNewer()).
  Message msg;            ///< Decoded message body.
};

/// @brief Either a decoded packet or the reason it was rejected.
using DecodeResult = std::variant<Packet, DecodeError>;

/**
 * @brief Encode a Control datagram.
 * @param msg Message to encode (not sanitized here).
 * @param seq Sender sequence number.
 * @return Exactly kControlSize bytes.
 */
std::vector<std::byte> encode(const ControlMsg& msg, std::uint32_t seq);

/**
 * @brief Encode a State datagram.
 * @param msg Message to encode.
 * @param seq Sender sequence number.
 * @return Exactly kStateSize bytes.
 */
std::vector<std::byte> encode(const StateMsg& msg, std::uint32_t seq);

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
 * @return The packet, or the first DecodeError found. Values are not sanitized; callers apply sanitize().
 */
DecodeResult decode(std::span<const std::byte> datagram);

/**
 * @brief Human-readable name of a decode error, for logs and stats.
 * @param e Error value.
 * @return Static string such as "bad_magic".
 */
const char* toString(DecodeError e);

/**
 * @brief Wraparound-safe sequence comparison.
 * @param a Candidate sequence number.
 * @param b Reference sequence number.
 * @return True if @p a is newer than @p b, treating the 32-bit space as circular.
 */
constexpr bool seqNewer(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::int32_t>(a - b) > 0;
}

}  // namespace astrosim::proto::wire
