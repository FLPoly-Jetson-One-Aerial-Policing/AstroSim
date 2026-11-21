/// @file
/// @brief Implementation of the AstroSim protocol v1 wire encoding (see PROTOCOL.md).
#include "astrosim/proto/wire.hpp"

#include <array>

#include "le_codec.hpp"

namespace astrosim::proto::wire {

static_assert(kControlSize == 40);
static_assert(kStateSize == 117);
static_assert(kSubscribeSize == 8);
static_assert(sizeof(float) == 4 && sizeof(double) == 8);

namespace {

using detail::Reader;
using detail::Writer;

/// @brief Creates a writer pre-filled with the 8-byte header.
/// @param type Message type id.
/// @param seq Sequence number.
/// @param total Total datagram size, used for reservation.
Writer startPacket(Type type, std::uint32_t seq, std::size_t total) {
  Writer w(total);
  w.u16(kMagic);
  w.u8(kVersion);
  w.u8(static_cast<std::uint8_t>(type));
  w.u32(seq);
  return w;
}

/// @brief Reads a Control payload (header already consumed).
ControlMsg readControl(Reader& r) {
  ControlMsg m;
  r.floats(m.power);
  return m;
}

/// @brief Reads a State payload (header already consumed).
StateMsg readState(Reader& r) {
  StateMsg m;
  m.sim_time = r.f64();
  r.doubles(m.pos);
  r.floats(m.quat);
  r.floats(m.vel);
  r.floats(m.ang_vel);
  r.floats(m.motor_speed);
  m.flags = r.u8();
  m.echo_seq = r.u32();
  return m;
}

}  // namespace

std::vector<std::byte> encode(const ControlMsg& m, std::uint32_t seq) {
  Writer w = startPacket(Type::Control, seq, kControlSize);
  w.array(m.power, &Writer::f32);
  return w.take();
}

std::vector<std::byte> encode(const StateMsg& m, std::uint32_t seq) {
  Writer w = startPacket(Type::State, seq, kStateSize);
  w.f64(m.sim_time);
  w.array(m.pos, &Writer::f64);
  w.array(m.quat, &Writer::f32);
  w.array(m.vel, &Writer::f32);
  w.array(m.ang_vel, &Writer::f32);
  w.array(m.motor_speed, &Writer::f32);
  w.u8(m.flags);
  w.u32(m.echo_seq);
  return w.take();
}

std::vector<std::byte> encode(const SubscribeMsg&, std::uint32_t seq) {
  return startPacket(Type::Subscribe, seq, kSubscribeSize).take();
}

DecodeResult decode(std::span<const std::byte> datagram) {
  if (datagram.size() < kHeaderSize) return DecodeError::TooShort;

  Reader r(datagram);
  if (r.u16() != kMagic) return DecodeError::BadMagic;
  if (r.u8() != kVersion) return DecodeError::BadVersion;
  const auto type = static_cast<Type>(r.u8());
  const std::uint32_t seq = r.u32();

  const auto expectSize = [&](std::size_t size) { return datagram.size() == size; };
  switch (type) {
    case Type::Control:
      if (!expectSize(kControlSize)) return DecodeError::BadSize;
      return Packet{seq, readControl(r)};
    case Type::State:
      if (!expectSize(kStateSize)) return DecodeError::BadSize;
      return Packet{seq, readState(r)};
    case Type::Subscribe:
      if (!expectSize(kSubscribeSize)) return DecodeError::BadSize;
      return Packet{seq, SubscribeMsg{}};
  }
  return DecodeError::UnknownType;
}

const char* toString(DecodeError e) {
  switch (e) {
    case DecodeError::TooShort: return "too_short";
    case DecodeError::BadMagic: return "bad_magic";
    case DecodeError::BadVersion: return "bad_version";
    case DecodeError::UnknownType: return "unknown_type";
    case DecodeError::BadSize: return "bad_size";
  }
  return "unknown";
}

}  // namespace astrosim::proto::wire
