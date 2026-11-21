/// @file
/// @brief Implementation of the AstroSim FC telemetry protocol v1 wire encoding (see FC_TELEMETRY.md).
#include "astrosim/proto/fc_telemetry.hpp"

#include "le_codec.hpp"

namespace astrosim::proto::fc_wire {

static_assert(kTelemetrySize == 95);
static_assert(kSubscribeSize == 8);

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

/// @brief Packs the boolean status fields into the wire status byte.
/// @param m Message.
/// @return Bitwise OR of FcStatusBits.
std::uint8_t statusByte(const FcTelemetryMsg& m) {
  return static_cast<std::uint8_t>((m.connected ? kStatusConnected : 0) | (m.armed ? kStatusArmed : 0) |
                                   (m.mode_refused ? kStatusModeRefused : 0));
}

}  // namespace

std::vector<std::byte> encode(const FcTelemetryMsg& m, std::uint32_t seq) {
  Writer w = startPacket(Type::Telemetry, seq, kTelemetrySize);
  w.f64(m.time_s);
  w.u8(statusByte(m));
  w.u8(static_cast<std::uint8_t>(m.mode));
  w.u8(m.gps_fix);
  w.u8(m.gps_sats);
  w.u8(m.provenance);
  w.u16(m.warnings);
  w.f32(m.roll_rad);
  w.f32(m.pitch_rad);
  w.f32(m.heading_rad);
  w.f32(m.alt_msl_m);
  w.f32(m.alt_agl_m);
  w.f32(m.vspeed_mps);
  w.f32(m.ground_speed_mps);
  w.f32(m.course_rad);
  w.f32(m.battery_pct);
  w.f32(m.battery_v);
  w.array(m.motors, &Writer::f32);
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

  switch (type) {
    case Type::Telemetry: {
      if (datagram.size() != kTelemetrySize) return DecodeError::BadSize;
      FcTelemetryMsg m;
      m.time_s = r.f64();
      const std::uint8_t status = r.u8();
      m.connected = (status & kStatusConnected) != 0;
      m.armed = (status & kStatusArmed) != 0;
      m.mode_refused = (status & kStatusModeRefused) != 0;
      const std::uint8_t mode = r.u8();
      if (mode > static_cast<std::uint8_t>(FlightMode::kNoGpsHold)) return DecodeError::BadValue;
      m.mode = static_cast<FlightMode>(mode);
      m.gps_fix = r.u8();
      m.gps_sats = r.u8();
      m.provenance = r.u8();
      m.warnings = r.u16();
      m.roll_rad = r.f32();
      m.pitch_rad = r.f32();
      m.heading_rad = r.f32();
      m.alt_msl_m = r.f32();
      m.alt_agl_m = r.f32();
      m.vspeed_mps = r.f32();
      m.ground_speed_mps = r.f32();
      m.course_rad = r.f32();
      m.battery_pct = r.f32();
      m.battery_v = r.f32();
      r.floats(m.motors);
      return Packet{seq, m};
    }
    case Type::Subscribe:
      if (datagram.size() != kSubscribeSize) return DecodeError::BadSize;
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
    case DecodeError::BadValue: return "bad_value";
  }
  return "unknown";
}

}  // namespace astrosim::proto::fc_wire
