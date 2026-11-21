/// @file
/// @brief Conformance tests for the AstroSim FC telemetry protocol v1 wire library.
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "astrosim/proto/fc_telemetry.hpp"

using namespace astrosim::proto;
using namespace astrosim::proto::fc_wire;

namespace {

/// @brief The telemetry message stored in testdata/fc_telemetry.bin (seq 9).
FcTelemetryMsg goldenTelemetry() {
  FcTelemetryMsg m;
  m.time_s = 123.456;
  m.connected = true;
  m.armed = true;
  m.mode_refused = false;
  m.mode = FlightMode::kNoGpsHold;
  m.gps_fix = kGps3dFix;
  m.gps_sats = 12;
  m.provenance = kSensorsSynthesized | kPilotSpoofed;
  m.warnings = kWarnLowBattery | kWarnEkfUnhealthy;
  m.roll_rad = 0.05f;
  m.pitch_rad = -0.1f;
  m.heading_rad = 1.5f;
  m.alt_msl_m = 160.5f;
  m.alt_agl_m = 10.25f;
  m.vspeed_mps = 0.75f;
  m.ground_speed_mps = 2.5f;
  m.course_rad = 3.0f;
  m.battery_pct = 87.5f;
  m.battery_v = 48.25f;
  m.motors = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
  return m;
}

/// @brief Views a byte vector as a span.
std::span<const std::byte> asSpan(const std::vector<std::byte>& v) {
  return v;
}

/// @brief Encodes @p m with @p seq, decodes it, and checks type and seq.
/// @return The decoded message (default-constructed on failure, with a recorded test failure).
template <typename Msg>
Msg roundTrip(const Msg& m, std::uint32_t seq) {
  const auto bytes = encode(m, seq);
  const auto result = decode(asSpan(bytes));
  const auto* packet = std::get_if<Packet>(&result);
  EXPECT_NE(packet, nullptr);
  if (!packet) return Msg{};
  EXPECT_EQ(packet->seq, seq);
  const auto* out = std::get_if<Msg>(&packet->msg);
  EXPECT_NE(out, nullptr);
  return out ? *out : Msg{};
}

/// @brief Decodes @p bytes and expects a rejection.
/// @return The DecodeError reported.
DecodeError decodeError(const std::vector<std::byte>& bytes) {
  const auto result = decode(asSpan(bytes));
  const auto* err = std::get_if<DecodeError>(&result);
  EXPECT_NE(err, nullptr);
  return err ? *err : DecodeError::TooShort;
}

/// @brief Builds a byte vector from integer literals.
std::vector<std::byte> bytesOf(std::initializer_list<int> values) {
  std::vector<std::byte> out;
  for (int v : values) out.push_back(static_cast<std::byte>(v));
  return out;
}

/// @brief Compares @p actual with testdata/@p name, or rewrites the file when UPDATE_GOLDEN is set.
void checkGolden(const char* name, const std::vector<std::byte>& actual) {
  const std::string path = std::string(GOLDEN_DIR) + "/" + name;
  if (std::getenv("UPDATE_GOLDEN")) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    ASSERT_TRUE(out.good()) << path;
    return;
  }
  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in.good()) << "missing golden file " << path << " (run with UPDATE_GOLDEN=1)";
  const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::vector<std::byte> expected(reinterpret_cast<const std::byte*>(raw.data()),
                                        reinterpret_cast<const std::byte*>(raw.data()) + raw.size());
  EXPECT_EQ(actual, expected) << name;
  EXPECT_TRUE(std::holds_alternative<Packet>(decode(expected)));
}

}  // namespace

TEST(FcWire, Sizes) {
  EXPECT_EQ(encode(FcTelemetryMsg{}, 0).size(), 95u);
  EXPECT_EQ(encode(SubscribeMsg{}, 0).size(), 8u);
  EXPECT_EQ(kMaxDatagramSize, kTelemetrySize);
  EXPECT_EQ(kDefaultPort, 8766);
}

TEST(FcWire, HeaderBytesPinEndianness) {
  EXPECT_EQ(encode(SubscribeMsg{}, 0x01020304), bytesOf({0xFC, 0xA5, 0x01, 0x02, 0x04, 0x03, 0x02, 0x01}));
  const auto t = encode(FcTelemetryMsg{}, 0);
  EXPECT_EQ(t[0], std::byte{0xFC});
  EXPECT_EQ(t[1], std::byte{0xA5});
  EXPECT_EQ(t[2], std::byte{0x01});
  EXPECT_EQ(t[3], std::byte{0x01});
}

TEST(FcWire, PayloadBytesPinLayout) {
  const auto b = encode(goldenTelemetry(), 0);
  EXPECT_EQ(b[16], std::byte{kStatusConnected | kStatusArmed});  // status
  EXPECT_EQ(b[17], std::byte{1});                                // mode = no-GPS hold
  EXPECT_EQ(b[18], std::byte{3});                                // gps fix
  EXPECT_EQ(b[19], std::byte{12});                               // satellites
  EXPECT_EQ(b[20], std::byte{3});                                // provenance
  EXPECT_EQ(b[21], std::byte{kWarnEkfUnhealthy | kWarnLowBattery});  // warnings low byte
  EXPECT_EQ(b[22], std::byte{0});                                // warnings high byte
}

TEST(FcWire, TelemetryRoundTrip) {
  EXPECT_EQ(roundTrip(goldenTelemetry(), 9), goldenTelemetry());
  EXPECT_EQ(roundTrip(goldenTelemetry(), 0xFFFFFFFEu), goldenTelemetry());
  EXPECT_EQ(roundTrip(FcTelemetryMsg{}, 0), FcTelemetryMsg{});
}

TEST(FcWire, SubscribeRoundTrip) {
  roundTrip(SubscribeMsg{}, 1);
  roundTrip(SubscribeMsg{}, 0xFFFFFFFEu);
}

TEST(FcWire, ModeRefusedBitRoundTrips) {
  FcTelemetryMsg m;
  m.mode_refused = true;
  const auto out = roundTrip(m, 1);
  EXPECT_TRUE(out.mode_refused);
  EXPECT_FALSE(out.armed);
  EXPECT_FALSE(out.connected);
}

TEST(FcWire, Golden) {
  checkGolden("fc_telemetry.bin", encode(goldenTelemetry(), 9));
  checkGolden("fc_subscribe.bin", encode(SubscribeMsg{}, 1));
}

TEST(FcDecode, EmptyAndTooShort) {
  EXPECT_EQ(std::get<DecodeError>(decode({})), DecodeError::TooShort);
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 0x01, 0x02, 0, 0, 0})), DecodeError::TooShort);
}

TEST(FcDecode, BadMagic) {
  EXPECT_EQ(decodeError(bytesOf({0, 0, 0x01, 0x02, 0, 0, 0, 0})), DecodeError::BadMagic);
  EXPECT_EQ(decodeError(bytesOf({0xA5, 0xFC, 0x01, 0x02, 0, 0, 0, 0})), DecodeError::BadMagic);
  // The engine protocol's magic is rejected: the two streams cannot be confused.
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x01, 0x02, 0, 0, 0, 0})), DecodeError::BadMagic);
}

TEST(FcDecode, BadVersion) {
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 0x02, 0x02, 0, 0, 0, 0})), DecodeError::BadVersion);
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 0x00, 0x02, 0, 0, 0, 0})), DecodeError::BadVersion);
}

TEST(FcDecode, UnknownType) {
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 0x01, 0x00, 0, 0, 0, 0})), DecodeError::UnknownType);
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 0x01, 0x03, 0, 0, 0, 0})), DecodeError::UnknownType);
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 0x01, 0xFF, 0, 0, 0, 0})), DecodeError::UnknownType);
}

TEST(FcDecode, BadSize) {
  for (auto bytes : {encode(FcTelemetryMsg{}, 0), encode(SubscribeMsg{}, 0)}) {
    auto longer = bytes;
    longer.push_back(std::byte{0});
    EXPECT_EQ(decodeError(longer), DecodeError::BadSize);
    if (bytes.size() > kHeaderSize) {
      bytes.pop_back();
      EXPECT_EQ(decodeError(bytes), DecodeError::BadSize);
    }
  }
}

TEST(FcDecode, BadModeValue) {
  auto bytes = encode(FcTelemetryMsg{}, 0);
  bytes[17] = std::byte{2};
  EXPECT_EQ(decodeError(bytes), DecodeError::BadValue);
  bytes[17] = std::byte{0xFF};
  EXPECT_EQ(decodeError(bytes), DecodeError::BadValue);
}

TEST(FcDecode, ReservedStatusBitsIgnored) {
  auto bytes = encode(FcTelemetryMsg{}, 0);
  bytes[16] = std::byte{0xF8};  // only reserved bits set
  const auto result = decode(asSpan(bytes));
  const auto* packet = std::get_if<Packet>(&result);
  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(std::get<FcTelemetryMsg>(packet->msg), FcTelemetryMsg{});
}

TEST(FcDecode, CheckOrder) {
  EXPECT_EQ(decodeError(bytesOf({0, 0, 9, 9, 0, 0, 0, 0, 0})), DecodeError::BadMagic);
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 9, 9, 0, 0, 0, 0, 0})), DecodeError::BadVersion);
  EXPECT_EQ(decodeError(bytesOf({0xFC, 0xA5, 1, 9, 0, 0, 0, 0, 0})), DecodeError::UnknownType);
}

TEST(FcDecode, DoesNotSanitizeFloats) {
  FcTelemetryMsg m;
  m.roll_rad = std::numeric_limits<float>::quiet_NaN();
  m.pitch_rad = std::numeric_limits<float>::infinity();
  const auto out = roundTrip(m, 1);
  EXPECT_TRUE(std::isnan(out.roll_rad));
  EXPECT_EQ(out.pitch_rad, std::numeric_limits<float>::infinity());
}

TEST(FcDecode, ToString) {
  EXPECT_STREQ(toString(DecodeError::TooShort), "too_short");
  EXPECT_STREQ(toString(DecodeError::BadMagic), "bad_magic");
  EXPECT_STREQ(toString(DecodeError::BadVersion), "bad_version");
  EXPECT_STREQ(toString(DecodeError::UnknownType), "unknown_type");
  EXPECT_STREQ(toString(DecodeError::BadSize), "bad_size");
  EXPECT_STREQ(toString(DecodeError::BadValue), "bad_value");
}
