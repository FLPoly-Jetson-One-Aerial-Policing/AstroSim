#pragma once
// One UDP datagram per message, little-endian, fixed layout.
//   header (8): magic u16 = 0xA57A | version u8 | type u8 | seq u32
//   Control (+16): roll pitch yaw throttle f32
//   State   (+65): sim_time f64 | pos f32[3] | quat f32[4] | vel f32[3] | ang_vel f32[3] | flags u8 | echo_seq u32
//   Fault   (+1):  motor_index u8
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "messages.hpp"

namespace astrosim::wire {

constexpr uint16_t kMagic = 0xA57A;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 8;
constexpr size_t kControlSize = kHeaderSize + 16;
constexpr size_t kStateSize = kHeaderSize + 65;
constexpr size_t kFaultSize = kHeaderSize + 1;

enum class Type : uint8_t { Control = 1, State = 2, Fault = 3 };

struct Packet {
  uint32_t seq = 0;
  std::variant<ControlMsg, StateMsg, FaultMsg> msg;
};

std::vector<uint8_t> encode(const ControlMsg&, uint32_t seq);
std::vector<uint8_t> encode(const StateMsg&, uint32_t seq);
std::vector<uint8_t> encode(const FaultMsg&, uint32_t seq);

// nullopt on bad magic/version/type or any size mismatch. Does not sanitize values.
std::optional<Packet> decode(const uint8_t* data, size_t len);

// Wraparound-safe "a is newer than b".
inline bool seqNewer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

}  // namespace astrosim::wire
