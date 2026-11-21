/**
 * @file test_gateway.cpp
 * @brief Deterministic tests of the Gateway logic using a fake engine and a manually advanced clock.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "gateway.hpp"

namespace astrosim::gateway {
namespace {

namespace wire = proto::wire;
using std::chrono::milliseconds;

/** @brief Engine double that records commands and returns a configurable snapshot. */
class FakeEngine : public Engine {
 public:
  EngineState snapshot() override { return state; }
  void setMotors(const std::array<float, proto::kNumMotors>& power) override {
    last = power;
    ++calls;
  }
  EngineState state;
  std::array<float, proto::kNumMotors> last{};
  int calls = 0;
};

constexpr Endpoint kFc{0x0A000001, 5000};
constexpr Endpoint kOther{0x0A000002, 5001};
constexpr Endpoint kDisplay{0x0A000003, 5002};

/** @brief Fixture owning the gateway, fake engine and a manual clock. */
class GatewayTest : public ::testing::Test {
 protected:
  GatewayTest() : gw(engine, config) {
    engine.state.fresh = true;
    engine.state.state.sim_time = 12.5;
    engine.state.state.pos = {1, 2, 3};
    engine.state.state.motor_speed[2] = 400.f;
  }

  void advance(milliseconds d) { now += d; }

  void sendControl(const Endpoint& from, std::uint32_t seq, float p = 0.5f) {
    proto::ControlMsg c;
    c.power.fill(p);
    gw.onDatagram(from, wire::encode(c, seq), now);
  }
  void sendControl(const Endpoint& from, std::uint32_t seq, const proto::ControlMsg& c) {
    gw.onDatagram(from, wire::encode(c, seq), now);
  }
  void subscribe(const Endpoint& from) { gw.onDatagram(from, wire::encode(proto::SubscribeMsg{}, 0), now); }

  /** @brief Decode the State from a datagram produced by tick(). */
  static std::pair<std::uint32_t, proto::StateMsg> decodeState(const Datagram& d) {
    const auto r = wire::decode(d.bytes);
    const auto& pkt = std::get<wire::Packet>(r);
    return {pkt.seq, std::get<proto::StateMsg>(pkt.msg)};
  }

  /** @brief Tick and return the flags seen by the first subscriber. */
  std::uint8_t tickFlags() {
    const auto out = gw.tick(now);
    EXPECT_FALSE(out.empty());
    return out.empty() ? 0 : decodeState(out.front()).second.flags;
  }

  FakeEngine engine;
  Config config;
  Gateway gw;
  TimePoint now{std::chrono::seconds(100)};
};

TEST_F(GatewayTest, ControlSetsSanitizedMotors) {
  proto::ControlMsg c;
  c.power = {std::numeric_limits<float>::quiet_NaN(), 2.f, -1.f, 0.25f,
             std::numeric_limits<float>::infinity(),  1.f, 0.f,  0.75f};
  sendControl(kFc, 1, c);
  ASSERT_EQ(engine.calls, 1);
  const std::array<float, 8> expected{0.f, 1.f, 0.f, 0.25f, 0.f, 1.f, 0.f, 0.75f};
  EXPECT_EQ(engine.last, expected);
}

TEST_F(GatewayTest, StaleAndDuplicateSeqDropped) {
  sendControl(kFc, 10);
  sendControl(kFc, 10);
  sendControl(kFc, 9);
  EXPECT_EQ(engine.calls, 1);
  EXPECT_EQ(gw.stats().stale_seq, 2u);
  sendControl(kFc, 11);
  EXPECT_EQ(engine.calls, 2);
}

TEST_F(GatewayTest, SeqWraparoundAccepted) {
  sendControl(kFc, 0xFFFFFFFEu);
  sendControl(kFc, 0xFFFFFFFFu);
  sendControl(kFc, 0u);
  sendControl(kFc, 1u);
  EXPECT_EQ(engine.calls, 4);
  EXPECT_EQ(gw.stats().stale_seq, 0u);
}

TEST_F(GatewayTest, SecondControllerRejectedWhileLeaseHeld) {
  sendControl(kFc, 1, 0.5f);
  advance(milliseconds(100));
  sendControl(kOther, 1, 0.9f);
  EXPECT_EQ(engine.calls, 1);
  EXPECT_EQ(engine.last[0], 0.5f);
  EXPECT_EQ(gw.stats().lease_rejects, 1u);
  advance(milliseconds(100));
  sendControl(kFc, 2, 0.6f);
  EXPECT_EQ(engine.last[0], 0.6f);
}

TEST_F(GatewayTest, TakeoverAfterLeaseExpiry) {
  sendControl(kFc, 100, 0.5f);
  advance(milliseconds(1100));
  // Takeover works even before tick() has released the lease, and a low seq is fine for a new holder.
  sendControl(kOther, 1, 0.9f);
  EXPECT_EQ(engine.last[0], 0.9f);
  EXPECT_EQ(gw.stats().lease_rejects, 0u);
  advance(milliseconds(10));
  sendControl(kFc, 101, 0.1f);
  EXPECT_EQ(gw.stats().lease_rejects, 1u);
  EXPECT_EQ(engine.last[0], 0.9f);
}

/// @brief A controller restarted on the same endpoint (seq reset) regains control after one lease timeout.
TEST_F(GatewayTest, RestartedControllerWithResetSeqRecoversAfterLeaseTimeout) {
  sendControl(kFc, 5000, 0.5f);
  // Restart: seq resets to 0 and the controller sends continuously at 100 Hz.
  std::uint32_t seq = 0;
  for (int i = 0; i < 110; ++i) {
    advance(milliseconds(10));
    sendControl(kFc, seq++, 0.7f);
  }
  EXPECT_EQ(engine.last[0], 0.7f);
  EXPECT_GE(gw.stats().stale_seq, 1u);
}

TEST_F(GatewayTest, TickReleasesExpiredLease) {
  sendControl(kFc, 1);
  advance(milliseconds(1100));
  gw.tick(now);
  sendControl(kOther, 5);
  EXPECT_EQ(gw.stats().lease_rejects, 0u);
}

TEST_F(GatewayTest, DeadmanTripsOnceAndClearsOnNextControl) {
  sendControl(kFc, 1, 0.8f);
  EXPECT_EQ(engine.calls, 1);
  advance(milliseconds(400));
  gw.tick(now);
  EXPECT_EQ(engine.calls, 1);

  advance(milliseconds(150));
  gw.tick(now);
  EXPECT_EQ(engine.calls, 2);
  for (float v : engine.last) EXPECT_EQ(v, 0.f);
  EXPECT_EQ(gw.stats().deadman_trips, 1u);

  advance(milliseconds(100));
  gw.tick(now);
  EXPECT_EQ(engine.calls, 2);
  EXPECT_EQ(gw.stats().deadman_trips, 1u);

  sendControl(kFc, 2, 0.3f);
  EXPECT_EQ(engine.calls, 3);
  EXPECT_EQ(engine.last[0], 0.3f);
  EXPECT_EQ(tickFlags() & proto::kDeadmanTripped, 0);
}

TEST_F(GatewayTest, FlagsThroughSessionStages) {
  subscribe(kDisplay);
  EXPECT_EQ(tickFlags(), proto::kEngineOk);

  engine.state.fresh = false;
  EXPECT_EQ(tickFlags(), 0);
  engine.state.fresh = true;

  sendControl(kFc, 1);
  EXPECT_EQ(tickFlags(), proto::kEngineOk | proto::kControlActive);

  advance(milliseconds(600));
  EXPECT_EQ(tickFlags(), proto::kEngineOk | proto::kDeadmanTripped);

  sendControl(kFc, 2);
  EXPECT_EQ(tickFlags(), proto::kEngineOk | proto::kControlActive);
}

TEST_F(GatewayTest, ControlActiveClearsWithinDeadmanWindowEdge) {
  subscribe(kDisplay);
  sendControl(kFc, 1);
  advance(milliseconds(500));
  EXPECT_NE(tickFlags() & proto::kControlActive, 0);
  advance(milliseconds(1));
  EXPECT_EQ(tickFlags() & proto::kControlActive, 0);
}

TEST_F(GatewayTest, SubscribeOnlyClientReceivesStateWithEngineFieldsAndEcho) {
  subscribe(kDisplay);
  auto out = gw.tick(now);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].peer, kDisplay);
  auto [seq, s] = decodeState(out[0]);
  (void)seq;
  EXPECT_EQ(s.sim_time, 12.5);
  EXPECT_EQ(s.pos, (std::array<double, 3>{1, 2, 3}));
  EXPECT_EQ(s.motor_speed[2], 400.f);
  EXPECT_EQ(s.echo_seq, 0u);

  sendControl(kFc, 777);
  out = gw.tick(now);
  ASSERT_EQ(out.size(), 2u);
  for (const auto& d : out) EXPECT_EQ(decodeState(d).second.echo_seq, 777u);
}

TEST_F(GatewayTest, ControllerReceivesStateWithoutSubscribing) {
  sendControl(kFc, 1);
  const auto out = gw.tick(now);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].peer, kFc);
}

TEST_F(GatewayTest, SubscriberTimesOutUnlessRefreshed) {
  subscribe(kDisplay);
  subscribe(kOther);
  advance(milliseconds(4000));
  subscribe(kOther);
  advance(milliseconds(1500));
  const auto out = gw.tick(now);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].peer, kOther);
}

TEST_F(GatewayTest, SubscriberCapRejectsNewButRefreshesExisting) {
  config.network.max_subscribers = 2;
  Gateway capped(engine, config);
  const Endpoint a{1, 1}, b{2, 2}, c{3, 3};
  capped.onDatagram(a, wire::encode(proto::SubscribeMsg{}, 0), now);
  capped.onDatagram(b, wire::encode(proto::SubscribeMsg{}, 0), now);
  capped.onDatagram(c, wire::encode(proto::SubscribeMsg{}, 0), now);
  EXPECT_EQ(capped.stats().subscriber_rejects, 1u);
  capped.onDatagram(a, wire::encode(proto::SubscribeMsg{}, 0), now);
  EXPECT_EQ(capped.stats().subscriber_rejects, 1u);
  EXPECT_EQ(capped.tick(now).size(), 2u);
}

TEST_F(GatewayTest, LeaseHolderIsExemptFromSubscriberCap) {
  config.network.max_subscribers = 2;
  Gateway capped(engine, config);
  const Endpoint a{1, 1}, b{2, 2};
  capped.onDatagram(a, wire::encode(proto::SubscribeMsg{}, 0), now);
  capped.onDatagram(b, wire::encode(proto::SubscribeMsg{}, 0), now);
  capped.onDatagram(kFc, wire::encode(proto::ControlMsg{}, 1), now);
  EXPECT_EQ(capped.stats().subscriber_rejects, 0u);
  EXPECT_EQ(engine.calls, 1);
  EXPECT_EQ(capped.tick(now).size(), 3u);
}

TEST_F(GatewayTest, NonHolderControlAtCapCountsBothRejects) {
  config.network.max_subscribers = 1;
  Gateway capped(engine, config);
  capped.onDatagram(kFc, wire::encode(proto::ControlMsg{}, 1), now);
  capped.onDatagram(kOther, wire::encode(proto::ControlMsg{}, 1), now);
  EXPECT_EQ(capped.stats().lease_rejects, 1u);
  EXPECT_EQ(capped.stats().subscriber_rejects, 1u);
  EXPECT_EQ(capped.tick(now).size(), 1u);
}

TEST_F(GatewayTest, MalformedCountersByKind) {
  const auto bytes = [](std::initializer_list<int> v) {
    std::vector<std::byte> b;
    for (int x : v) b.push_back(static_cast<std::byte>(x));
    return b;
  };
  const auto idx = [](wire::DecodeError e) { return static_cast<std::size_t>(e); };

  gw.onDatagram(kFc, bytes({1, 2, 3}), now);
  auto bad_magic = wire::encode(proto::SubscribeMsg{}, 0);
  bad_magic[0] = std::byte{0};
  gw.onDatagram(kFc, bad_magic, now);
  auto bad_version = wire::encode(proto::SubscribeMsg{}, 0);
  bad_version[2] = std::byte{99};
  gw.onDatagram(kFc, bad_version, now);
  auto unknown = wire::encode(proto::SubscribeMsg{}, 0);
  unknown[3] = std::byte{77};
  gw.onDatagram(kFc, unknown, now);
  auto bad_size = wire::encode(proto::ControlMsg{}, 0);
  bad_size.pop_back();
  gw.onDatagram(kFc, bad_size, now);

  const Stats& s = gw.stats();
  EXPECT_EQ(s.malformed[idx(wire::DecodeError::TooShort)], 1u);
  EXPECT_EQ(s.malformed[idx(wire::DecodeError::BadMagic)], 1u);
  EXPECT_EQ(s.malformed[idx(wire::DecodeError::BadVersion)], 1u);
  EXPECT_EQ(s.malformed[idx(wire::DecodeError::UnknownType)], 1u);
  EXPECT_EQ(s.malformed[idx(wire::DecodeError::BadSize)], 1u);
  EXPECT_EQ(s.rx_datagrams, 5u);
  EXPECT_EQ(engine.calls, 0);
  EXPECT_TRUE(gw.tick(now).empty());
}

TEST_F(GatewayTest, StateFromClientIsUnexpectedAndDropped) {
  gw.onDatagram(kFc, wire::encode(proto::StateMsg{}, 1), now);
  EXPECT_EQ(gw.stats().unexpected_type, 1u);
  EXPECT_TRUE(gw.tick(now).empty());
}

TEST_F(GatewayTest, StateSeqIncrementsPerTickAndIsSharedBySubscribers) {
  subscribe(kDisplay);
  subscribe(kOther);
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto out = gw.tick(now);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(decodeState(out[0]).first, i);
    EXPECT_EQ(decodeState(out[1]).first, i);
  }
  EXPECT_EQ(gw.stats().tx_state, 6u);
}

TEST_F(GatewayTest, NoDatagramsWithoutSubscribers) {
  EXPECT_TRUE(gw.tick(now).empty());
  EXPECT_EQ(gw.stats().tx_state, 0u);
}

TEST_F(GatewayTest, CountersAndFormatStats) {
  subscribe(kDisplay);
  sendControl(kFc, 1);
  EXPECT_EQ(gw.stats().rx_subscribe, 1u);
  EXPECT_EQ(gw.stats().rx_control, 1u);
  const std::string s = gw.formatStats();
  EXPECT_NE(s.find("rx=2"), std::string::npos);
  EXPECT_NE(s.find("10.0.0.1:5000"), std::string::npos);
}

}  // namespace
}  // namespace astrosim::gateway
