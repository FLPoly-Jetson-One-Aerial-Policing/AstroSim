/// @file
/// @brief Unit tests for the AstroSim protocol v1 wire library.
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "astrosim/proto/wire.hpp"

using namespace astrosim::proto;
using namespace astrosim::proto::wire;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// @brief The Control message stored in testdata/control.bin (seq 7).
ControlMsg goldenControl() {
  return {.power = {0.f, 0.125f, 0.25f, 0.5f, 0.75f, 1.f, 0.5f, 0.0625f}};
}

/// @brief The State message stored in testdata/state.bin (seq 0xFFFFFFFE).
StateMsg goldenState() {
  return {.sim_time = 123.456,
          .pos = {1234.5, -678.25, 90.125},
          .quat = {0.1f, -0.2f, 0.3f, 0.9273618f},
          .vel = {1.5f, -2.5f, 3.5f},
          .ang_vel = {0.01f, -0.02f, 0.03f},
          .motor_speed = {100.f, 200.f, 300.f, 400.f, 500.f, 600.f, 700.f, 800.f},
          .flags = kEngineOk | kControlActive,
          .echo_seq = 4242};
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
  // The golden file must also decode back to the message that produced it.
  EXPECT_TRUE(std::holds_alternative<Packet>(decode(expected)));
}

}  // namespace

TEST(Wire, Sizes) {
  EXPECT_EQ(encode(ControlMsg{}, 0).size(), 40u);
  EXPECT_EQ(encode(StateMsg{}, 0).size(), 117u);
  EXPECT_EQ(encode(SubscribeMsg{}, 0).size(), 8u);
  EXPECT_EQ(kMaxDatagramSize, kStateSize);
}

TEST(Wire, HeaderBytesPinEndianness) {
  const auto bytes = encode(SubscribeMsg{}, 0x01020304);
  EXPECT_EQ(bytes, bytesOf({0x7A, 0xA5, 0x01, 0x03, 0x04, 0x03, 0x02, 0x01}));
  const auto control = encode(ControlMsg{}, 0);
  EXPECT_EQ(control[2], std::byte{0x01});
  EXPECT_EQ(control[3], std::byte{0x01});
  EXPECT_EQ(encode(StateMsg{}, 0)[3], std::byte{0x02});
}

TEST(Wire, FloatBytesAreLittleEndian) {
  ControlMsg c;
  c.power[0] = 1.0f;  // 0x3F800000
  const auto bytes = encode(c, 0);
  EXPECT_EQ(bytes[8], std::byte{0x00});
  EXPECT_EQ(bytes[9], std::byte{0x00});
  EXPECT_EQ(bytes[10], std::byte{0x80});
  EXPECT_EQ(bytes[11], std::byte{0x3F});
}

TEST(Wire, ControlRoundTrip) {
  EXPECT_EQ(roundTrip(goldenControl(), 7), goldenControl());
  EXPECT_EQ(roundTrip(goldenControl(), 0xFFFFFFFEu), goldenControl());
}

TEST(Wire, StateRoundTrip) {
  EXPECT_EQ(roundTrip(goldenState(), 0xFFFFFFFEu), goldenState());
  EXPECT_EQ(roundTrip(StateMsg{}, 0), StateMsg{});
}

TEST(Wire, SubscribeRoundTrip) {
  roundTrip(SubscribeMsg{}, 1);
  roundTrip(SubscribeMsg{}, 0xFFFFFFFEu);
}

TEST(Wire, Golden) {
  checkGolden("control.bin", encode(goldenControl(), 7));
  checkGolden("state.bin", encode(goldenState(), 0xFFFFFFFEu));
  checkGolden("subscribe.bin", encode(SubscribeMsg{}, 1));
}

TEST(Decode, EmptyAndTooShort) {
  EXPECT_EQ(std::get<DecodeError>(decode({})), DecodeError::TooShort);
  EXPECT_EQ(decodeError(bytesOf({0x7A})), DecodeError::TooShort);
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x01, 0x03, 0, 0, 0})), DecodeError::TooShort);
}

TEST(Decode, BadMagic) {
  EXPECT_EQ(decodeError(bytesOf({0x00, 0x00, 0x01, 0x03, 0, 0, 0, 0})), DecodeError::BadMagic);
  // Byte-swapped magic is rejected.
  EXPECT_EQ(decodeError(bytesOf({0xA5, 0x7A, 0x01, 0x03, 0, 0, 0, 0})), DecodeError::BadMagic);
}

TEST(Decode, BadVersion) {
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x02, 0x03, 0, 0, 0, 0})), DecodeError::BadVersion);
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x00, 0x03, 0, 0, 0, 0})), DecodeError::BadVersion);
}

TEST(Decode, UnknownType) {
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x01, 0x00, 0, 0, 0, 0})), DecodeError::UnknownType);
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x01, 0x04, 0, 0, 0, 0})), DecodeError::UnknownType);
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 0x01, 0xFF, 0, 0, 0, 0})), DecodeError::UnknownType);
}

TEST(Decode, BadSize) {
  for (auto bytes : {encode(ControlMsg{}, 0), encode(StateMsg{}, 0), encode(SubscribeMsg{}, 0)}) {
    auto longer = bytes;
    longer.push_back(std::byte{0});
    EXPECT_EQ(decodeError(longer), DecodeError::BadSize);
    if (bytes.size() > kHeaderSize) {
      bytes.pop_back();
      EXPECT_EQ(decodeError(bytes), DecodeError::BadSize);
    }
  }
}

TEST(Decode, CheckOrder) {
  // Bad magic wins over bad version/type/size.
  EXPECT_EQ(decodeError(bytesOf({0, 0, 9, 9, 0, 0, 0, 0, 0})), DecodeError::BadMagic);
  // Bad version wins over unknown type and size.
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 9, 9, 0, 0, 0, 0, 0})), DecodeError::BadVersion);
  // Unknown type wins over size.
  EXPECT_EQ(decodeError(bytesOf({0x7A, 0xA5, 1, 9, 0, 0, 0, 0, 0})), DecodeError::UnknownType);
}

TEST(Decode, DoesNotSanitize) {
  ControlMsg c;
  c.power = {kNaN, kInf, -kInf, -0.5f, 1.5f, 0.f, 1.f, 0.25f};
  const auto out = roundTrip(c, 1);
  EXPECT_TRUE(std::isnan(out.power[0]));
  EXPECT_EQ(out.power[1], kInf);
  EXPECT_EQ(out.power[2], -kInf);
  EXPECT_EQ(out.power[3], -0.5f);
  EXPECT_EQ(out.power[4], 1.5f);
}

TEST(Decode, ToString) {
  EXPECT_STREQ(toString(DecodeError::TooShort), "too_short");
  EXPECT_STREQ(toString(DecodeError::BadMagic), "bad_magic");
  EXPECT_STREQ(toString(DecodeError::BadVersion), "bad_version");
  EXPECT_STREQ(toString(DecodeError::UnknownType), "unknown_type");
  EXPECT_STREQ(toString(DecodeError::BadSize), "bad_size");
}

TEST(Sanitize, EdgeCases) {
  EXPECT_EQ(sanitizePower(kNaN), 0.f);
  EXPECT_EQ(sanitizePower(kInf), 0.f);
  EXPECT_EQ(sanitizePower(-kInf), 0.f);
  EXPECT_EQ(sanitizePower(-0.5f), 0.f);
  EXPECT_EQ(sanitizePower(1.5f), 1.f);
  EXPECT_EQ(sanitizePower(0.f), 0.f);
  EXPECT_EQ(sanitizePower(1.f), 1.f);
  EXPECT_EQ(sanitizePower(0.25f), 0.25f);
}

TEST(Sanitize, ControlMsg) {
  const ControlMsg in{.power = {kNaN, kInf, -kInf, -0.5f, 1.5f, 0.f, 1.f, 0.25f}};
  const ControlMsg expected{.power = {0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.25f}};
  EXPECT_EQ(sanitize(in), expected);
}

TEST(SeqNewer, Basics) {
  EXPECT_TRUE(seqNewer(2, 1));
  EXPECT_FALSE(seqNewer(1, 2));
  EXPECT_FALSE(seqNewer(5, 5));
}

TEST(SeqNewer, Wraparound) {
  EXPECT_TRUE(seqNewer(0, 0xFFFFFFFFu));
  EXPECT_TRUE(seqNewer(3, 0xFFFFFFFEu));
  EXPECT_FALSE(seqNewer(0xFFFFFFFFu, 0));
  EXPECT_FALSE(seqNewer(0xFFFFFFFEu, 3));
  EXPECT_TRUE(seqNewer(0x7FFFFFFFu, 0));
  EXPECT_FALSE(seqNewer(0x80000000u, 0));  // exactly half the space apart is not "newer"
}
