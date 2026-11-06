#include "wire.hpp"

#include <cstring>

namespace astrosim::wire {
namespace {

struct Writer {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(v & 0xff); u8(v >> 8); }
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 0xff); }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((v >> (8 * i)) & 0xff); }
  void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
  void f64(double v) { uint64_t u; std::memcpy(&u, &v, 8); u64(u); }
};

struct Reader {
  const uint8_t* p;
  uint8_t u8() { return *p++; }
  uint16_t u16() { uint16_t v = u8(); v |= uint16_t(u8()) << 8; return v; }
  uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(u8()) << (8 * i); return v; }
  uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(u8()) << (8 * i); return v; }
  float f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
  double f64() { uint64_t u = u64(); double v; std::memcpy(&v, &u, 8); return v; }
};

Writer header(Type t, uint32_t seq) {
  Writer w;
  w.u16(kMagic); w.u8(kVersion); w.u8(static_cast<uint8_t>(t)); w.u32(seq);
  return w;
}

}  // namespace

std::vector<uint8_t> encode(const ControlMsg& m, uint32_t seq) {
  Writer w = header(Type::Control, seq);
  w.f32(m.roll); w.f32(m.pitch); w.f32(m.yaw); w.f32(m.throttle);
  return w.b;
}

std::vector<uint8_t> encode(const StateMsg& m, uint32_t seq) {
  Writer w = header(Type::State, seq);
  w.f64(m.sim_time);
  for (float v : m.pos) w.f32(v);
  for (float v : m.quat) w.f32(v);
  for (float v : m.vel) w.f32(v);
  for (float v : m.ang_vel) w.f32(v);
  w.u8(m.flags); w.u32(m.echo_seq);
  return w.b;
}

std::vector<uint8_t> encode(const FaultMsg& m, uint32_t seq) {
  Writer w = header(Type::Fault, seq);
  w.u8(m.motor_index);
  return w.b;
}

std::optional<Packet> decode(const uint8_t* data, size_t len) {
  if (!data || len < kHeaderSize) return std::nullopt;
  Reader r{data};
  if (r.u16() != kMagic || r.u8() != kVersion) return std::nullopt;
  const uint8_t type = r.u8();
  Packet pkt;
  pkt.seq = r.u32();
  switch (static_cast<Type>(type)) {
    case Type::Control: {
      if (len != kControlSize) return std::nullopt;
      ControlMsg m;
      m.roll = r.f32(); m.pitch = r.f32(); m.yaw = r.f32(); m.throttle = r.f32();
      pkt.msg = m;
      return pkt;
    }
    case Type::State: {
      if (len != kStateSize) return std::nullopt;
      StateMsg m;
      m.sim_time = r.f64();
      for (float& v : m.pos) v = r.f32();
      for (float& v : m.quat) v = r.f32();
      for (float& v : m.vel) v = r.f32();
      for (float& v : m.ang_vel) v = r.f32();
      m.flags = r.u8(); m.echo_seq = r.u32();
      pkt.msg = m;
      return pkt;
    }
    case Type::Fault: {
      if (len != kFaultSize) return std::nullopt;
      pkt.msg = FaultMsg{r.u8()};
      return pkt;
    }
  }
  return std::nullopt;
}

}  // namespace astrosim::wire
