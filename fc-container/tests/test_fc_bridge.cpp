/**
 * @file test_fc_bridge.cpp
 * @brief Deterministic tests of FcBridge using a fake flight computer, fake transports and a manual clock.
 */
#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <limits>
#include <variant>

#include "adapter.hpp"
#include "astrosim/fc/scripted_pilot.hpp"
#include "astrosim/fc/synth_sensors_TEMPORARY.hpp"
#include "astrosim/proto/fc_telemetry.hpp"
#include "astrosim/proto/wire.hpp"
#include "fc_bridge.hpp"

namespace astrosim::fcbridge {
namespace {

namespace wire = proto::wire;
namespace fcwire = proto::fc_wire;
using std::chrono::milliseconds;

constexpr Endpoint kEngine{0x7F000001, 8765};
constexpr Endpoint kDisplayA{0x0A000001, 6000};
constexpr Endpoint kDisplayB{0x0A000002, 6001};

/** @brief Transport double: tests push inbound datagrams and inspect what was sent. */
class FakeTransport : public net::Transport {
 public:
  std::optional<net::Datagram> receive() override {
    if (inbox.empty()) return std::nullopt;
    net::Datagram d = std::move(inbox.front());
    inbox.pop_front();
    return d;
  }
  void send(const Endpoint& to, std::span<const std::byte> bytes) override {
    sent.push_back({to, std::vector<std::byte>(bytes.begin(), bytes.end())});
  }
  std::deque<net::Datagram> inbox;
  std::vector<net::Datagram> sent;
};

/** @brief FlightComputer double that records inputs and returns scripted outputs. */
class FakeFc : public fc::FlightComputer {
 public:
  void onSensors(const fc::SensorFrame& f, fc::TimePoint) override { frames.push_back(f); }
  void onPilot(const fc::PilotInput& p, fc::TimePoint) override { pilots.push_back(p); }
  void poll(fc::TimePoint) override { ++polls; }
  std::optional<fc::MotorCommand> takeMotors() override {
    auto c = pending;
    pending.reset();
    return c;
  }
  fc::FcTelemetry telemetry() const override { return tel; }
  void reset() override { ++resets; }

  std::vector<fc::SensorFrame> frames;
  std::vector<fc::PilotInput> pilots;
  std::optional<fc::MotorCommand> pending;
  fc::FcTelemetry tel = [] {
    fc::FcTelemetry t;
    t.connected = true;
    return t;
  }();
  int polls = 0;
  int resets = 0;
};

/** @brief SensorSource double: one frame per State, time_s copied, accel.x = sim_time. */
class FakeSensors : public fc::SensorSource {
 public:
  std::optional<fc::SensorFrame> onState(const proto::StateMsg& s) override {
    states.push_back(s);
    if (skip) return std::nullopt;
    fc::SensorFrame f;
    f.time_s = s.sim_time;
    f.gyro = {s.ang_vel[0], s.ang_vel[1], s.ang_vel[2]};
    return f;
  }
  void reset() override {}
  bool isSynthesized() const override { return synthesized; }
  std::vector<proto::StateMsg> states;
  bool skip = false;
  bool synthesized = true;
};

/** @brief PilotSource double returning a fixed command and recording sample times. */
class FakePilot : public fc::PilotSource {
 public:
  fc::PilotInput sample(double t) const override {
    times.push_back(t);
    fc::PilotInput p;
    p.throttle = static_cast<float>(t);
    return p;
  }
  bool isSpoofed() const override { return spoofed; }
  mutable std::vector<double> times;
  bool spoofed = true;
};

/** @brief Fixture owning the bridge, fakes and a manual clock. */
class BridgeTest : public ::testing::Test {
 protected:

  void advance(milliseconds d) { now += d; }
  /** @brief Step the bridge in 1 ms increments for @p d. */
  void run(milliseconds d) {
    for (milliseconds t{0}; t < d; ++t) {
      advance(milliseconds(1));
      bridge.step(now);
    }
  }
  void pushState(double sim_time, std::uint8_t flags = 0, std::uint32_t echo = 0) {
    proto::StateMsg s;
    s.sim_time = sim_time;
    s.flags = flags;
    s.echo_seq = echo;
    s.ang_vel = {1.f, 2.f, 3.f};
    engine_link.inbox.push_back({kEngine, wire::encode(s, state_seq++)});
  }
  void pushSubscribe(const Endpoint& from) {
    tel_link.inbox.push_back({from, fcwire::encode(proto::SubscribeMsg{}, 0)});
  }

  /** @brief Control messages sent to the engine so far, as (seq, power). */
  std::vector<std::pair<std::uint32_t, proto::ControlMsg>> controls() const {
    std::vector<std::pair<std::uint32_t, proto::ControlMsg>> out;
    for (const auto& d : engine_link.sent) {
      const auto r = wire::decode(d.bytes);
      const auto& pk = std::get<wire::Packet>(r);
      if (const auto* c = std::get_if<proto::ControlMsg>(&pk.msg)) out.emplace_back(pk.seq, *c);
    }
    return out;
  }
  std::vector<std::uint32_t> subscribeSeqs() const {
    std::vector<std::uint32_t> out;
    for (const auto& d : engine_link.sent) {
      const auto r = wire::decode(d.bytes);
      const auto& pk = std::get<wire::Packet>(r);
      if (std::holds_alternative<proto::SubscribeMsg>(pk.msg)) out.push_back(pk.seq);
    }
    return out;
  }
  std::vector<std::pair<Endpoint, fc::FcTelemetry>> telemetrySent() const {
    std::vector<std::pair<Endpoint, fc::FcTelemetry>> out;
    for (const auto& d : tel_link.sent) {
      const auto r = fcwire::decode(d.bytes);
      out.emplace_back(d.peer, std::get<proto::FcTelemetryMsg>(std::get<fcwire::Packet>(r).msg));
    }
    return out;
  }
  /** @brief Give the FC an armed command with a recognizable power, delivered at the next step. */
  void armedCommand(float p = 0.4f) {
    fc::MotorCommand c;
    c.power.fill(p);
    c.armed = true;
    fc.pending = c;
  }

  Config config;
  FakeTransport engine_link, tel_link;
  FakeFc fc;
  FakeSensors sensors;
  FakePilot pilot;
  FcBridge bridge{fc, sensors, pilot, engine_link, tel_link, kEngine, config};
  TimePoint now{std::chrono::seconds(100)};
  std::uint32_t state_seq = 0;
};

TEST_F(BridgeTest, SubscribesImmediatelyAndAtLeastOncePerSecond) {
  bridge.step(now);
  EXPECT_EQ(subscribeSeqs().size(), 1u);
  run(milliseconds(10'000));
  const auto seqs = subscribeSeqs();
  EXPECT_GE(seqs.size(), 10'000u / 1000u);                       // >= 1 Hz
  EXPECT_NEAR(static_cast<double>(seqs.size()), 10.0 / 0.5, 2);  // default 0.5 s period
  for (std::size_t i = 0; i < seqs.size(); ++i) EXPECT_EQ(seqs[i], i) << "subscribe seq increments";
}

TEST_F(BridgeTest, ControlHasIncreasingSeqAtConfiguredRate) {
  bridge.step(now);
  run(milliseconds(1000));
  const auto c = controls();
  EXPECT_NEAR(static_cast<double>(c.size()), 250.0, 3);
  for (std::size_t i = 0; i < c.size(); ++i) EXPECT_EQ(c[i].first, i + 1);
}

TEST_F(BridgeTest, ControlRateFollowsConfig) {
  config.timing.control_rate_hz = 100;
  FcBridge b(fc, sensors, pilot, engine_link, tel_link, kEngine, config);
  b.step(now);
  for (int i = 0; i < 1000; ++i) {
    advance(milliseconds(1));
    b.step(now);
  }
  EXPECT_NEAR(static_cast<double>(controls().size()), 100.0, 2);
}

TEST_F(BridgeTest, LateCallerDoesNotBurst) {
  bridge.step(now);
  const auto before = controls().size();
  advance(milliseconds(1000));
  bridge.step(now);
  EXPECT_EQ(controls().size(), before + 1);
}

TEST_F(BridgeTest, ZeroPowerBeforeTheFcHasCommandedAnything) {
  run(milliseconds(50));
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kNoCommand);
  for (const auto& [seq, c] : controls()) EXPECT_EQ(c.power, (std::array<float, 8>{})) << seq;
}

TEST_F(BridgeTest, PassesArmedCommandThroughSanitized) {
  fc::MotorCommand c;
  c.power = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 2.f, std::numeric_limits<float>::quiet_NaN()};
  c.armed = true;
  fc.pending = c;
  run(milliseconds(10));
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kNone);
  const auto p = controls().back().second.power;
  EXPECT_EQ(p, (std::array<float, 8>{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 1.f, 0.f}));
}

TEST_F(BridgeTest, ZeroWhenDisarmed) {
  armedCommand();
  run(milliseconds(10));
  ASSERT_EQ(controls().back().second.power[0], 0.4f);
  fc::MotorCommand c;
  c.power.fill(0.9f);  // a disarmed FC with leftover power must still produce zero
  c.armed = false;
  fc.pending = c;
  run(milliseconds(10));
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kDisarmed);
  EXPECT_EQ(controls().back().second.power, (std::array<float, 8>{}));
}

TEST_F(BridgeTest, ZeroWhenFcDisconnected) {
  armedCommand();
  run(milliseconds(10));
  fc.tel.connected = false;
  run(milliseconds(10));
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kDisconnected);
  EXPECT_EQ(controls().back().second.power, (std::array<float, 8>{}));
  fc.tel.connected = true;
  armedCommand();
  run(milliseconds(10));
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kNone);
  EXPECT_EQ(controls().back().second.power[0], 0.4f);
}

TEST_F(BridgeTest, ZeroWhenFcSilentLongerThanTimeout) {
  armedCommand();
  run(milliseconds(10));
  run(milliseconds(450));  // still within fc_timeout_s = 0.5
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kNone);
  EXPECT_EQ(controls().back().second.power[0], 0.4f);
  run(milliseconds(100));
  EXPECT_EQ(bridge.failsafe(), FailsafeReason::kFcSilent);
  EXPECT_EQ(controls().back().second.power, (std::array<float, 8>{}));
  EXPECT_EQ(bridge.stats().failsafe_entries, 1u);  // fc_silent (the initial no_command is not an entry)
}

TEST_F(BridgeTest, FcIsPolledEveryStep) {
  run(milliseconds(20));
  EXPECT_EQ(fc.polls, 20);
}

TEST_F(BridgeTest, StateFlowsThroughSensorSourceToFc) {
  pushState(12.5);
  bridge.step(now);
  ASSERT_EQ(sensors.states.size(), 1u);
  ASSERT_EQ(fc.frames.size(), 1u);
  EXPECT_DOUBLE_EQ(fc.frames[0].time_s, 12.5);
  EXPECT_EQ(fc.frames[0].gyro, (fc::Vec3{1, 2, 3}));
  EXPECT_EQ(bridge.stats().rx_state, 1u);
  EXPECT_EQ(bridge.stats().sensor_frames, 1u);
}

TEST_F(BridgeTest, StateWithoutFrameIsCountedAndNotForwarded) {
  sensors.skip = true;
  pushState(1.0);
  bridge.step(now);
  EXPECT_TRUE(fc.frames.empty());
  EXPECT_EQ(bridge.stats().sensor_skipped, 1u);
}

TEST_F(BridgeTest, DatagramsFromOtherEndpointsAreDropped) {
  proto::StateMsg s;
  engine_link.inbox.push_back({kDisplayA, wire::encode(s, 0)});
  bridge.step(now);
  EXPECT_TRUE(fc.frames.empty());
  EXPECT_EQ(bridge.stats().engine_foreign, 1u);
}

TEST_F(BridgeTest, MalformedAndUnexpectedEngineDatagramsAreCounted) {
  engine_link.inbox.push_back({kEngine, std::vector<std::byte>(3)});
  engine_link.inbox.push_back({kEngine, wire::encode(proto::ControlMsg{}, 0)});
  bridge.step(now);
  EXPECT_EQ(bridge.stats().engine_malformed, 1u);
  EXPECT_EQ(bridge.stats().engine_unexpected, 1u);
  EXPECT_TRUE(fc.frames.empty());
}

TEST_F(BridgeTest, TracksEchoSeqAndDeadmanEdges) {
  pushState(1.0, proto::kControlActive, 7);
  bridge.step(now);
  EXPECT_NE(bridge.formatStats().find("echo_seq=7"), std::string::npos);
  pushState(1.1, proto::kDeadmanTripped, 7);
  pushState(1.2, proto::kDeadmanTripped, 7);
  bridge.step(now);
  EXPECT_EQ(bridge.stats().deadman_seen, 1u);  // rising edge only
  pushState(1.3, proto::kControlActive, 9);
  pushState(1.4, proto::kDeadmanTripped, 9);
  bridge.step(now);
  EXPECT_EQ(bridge.stats().deadman_seen, 2u);
}

TEST_F(BridgeTest, PilotIsNotSampledBeforeTheFirstState) {
  run(milliseconds(100));
  EXPECT_TRUE(fc.pilots.empty());
}

TEST_F(BridgeTest, PilotSampledAtConfiguredRateOnSimTime) {
  pushState(42.0);
  bridge.step(now);
  run(milliseconds(1000));
  EXPECT_NEAR(static_cast<double>(fc.pilots.size()), 50.0, 3);  // default 50 Hz
  for (const double t : pilot.times) EXPECT_DOUBLE_EQ(t, 42.0);
  EXPECT_FLOAT_EQ(fc.pilots.back().throttle, 42.f);
}

TEST_F(BridgeTest, PilotTimeFollowsLatestState) {
  pushState(1.0);
  bridge.step(now);
  advance(milliseconds(100));
  pushState(2.0);
  bridge.step(now);
  EXPECT_DOUBLE_EQ(pilot.times.back(), 2.0);
}

TEST_F(BridgeTest, TelemetryOnlyGoesToSubscribers) {
  run(milliseconds(100));
  EXPECT_TRUE(tel_link.sent.empty());
  pushSubscribe(kDisplayA);
  run(milliseconds(1000));
  const auto t = telemetrySent();
  EXPECT_NEAR(static_cast<double>(t.size()), 30.0, 3);  // default 30 Hz
  for (const auto& [to, msg] : t) EXPECT_EQ(to, kDisplayA);
}

TEST_F(BridgeTest, TelemetrySeqIncreasesAndCarriesFcTelemetry) {
  fc.tel.armed = true;
  fc.tel.alt_msl_m = 123.f;
  fc.tel.mode = fc::FlightMode::kNoGpsHold;
  pushSubscribe(kDisplayA);
  run(milliseconds(200));
  std::uint32_t prev = 0;
  bool first = true;
  for (const auto& d : tel_link.sent) {
    const auto pk = std::get<fcwire::Packet>(fcwire::decode(d.bytes));
    if (!first) {
      EXPECT_EQ(pk.seq, prev + 1);
    }
    first = false;
    prev = pk.seq;
  }
  const auto m = telemetrySent().back().second;
  EXPECT_TRUE(m.armed);
  EXPECT_FLOAT_EQ(m.alt_msl_m, 123.f);
  EXPECT_EQ(m.mode, fc::FlightMode::kNoGpsHold);
}

TEST_F(BridgeTest, TelemetryProvenanceIsOverlaidByTheBridge) {
  fc.tel.provenance = 0;  // the adapter says nothing
  pushSubscribe(kDisplayA);
  run(milliseconds(100));
  EXPECT_EQ(telemetrySent().back().second.provenance, proto::kSensorsSynthesized | proto::kPilotSpoofed);
}

TEST_F(BridgeTest, TelemetryProvenanceReflectsRealSources) {
  sensors.synthesized = false;
  pilot.spoofed = false;
  pushSubscribe(kDisplayA);
  run(milliseconds(100));
  EXPECT_EQ(telemetrySent().back().second.provenance, 0);
  EXPECT_NE(bridge.formatStats().find("sensors=MEASURED pilot=REAL"), std::string::npos);
}

TEST_F(BridgeTest, TelemetryMotorsAreTheOnesActuallySent) {
  pushSubscribe(kDisplayA);
  fc.tel.motors.fill(0.9f);  // what the adapter thinks; the failsafe is zeroing it
  run(milliseconds(100));
  EXPECT_EQ(telemetrySent().back().second.motors, (std::array<float, 8>{}));
  armedCommand(0.25f);
  run(milliseconds(100));
  EXPECT_EQ(telemetrySent().back().second.motors, (std::array<float, 8>{0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f,
                                                                         0.25f, 0.25f}));
}

TEST_F(BridgeTest, TelemetrySubscriberExpiresAndKeepaliveRenews) {
  pushSubscribe(kDisplayA);
  pushSubscribe(kDisplayB);
  run(milliseconds(100));
  // A keeps renewing every second, B goes silent.
  for (int i = 0; i < 7; ++i) {
    run(milliseconds(1000));
    pushSubscribe(kDisplayA);
  }
  tel_link.sent.clear();
  run(milliseconds(500));
  for (const auto& [to, msg] : telemetrySent()) EXPECT_EQ(to, kDisplayA);
  EXPECT_FALSE(tel_link.sent.empty());
  // Now A goes silent too.
  run(milliseconds(5500));
  tel_link.sent.clear();
  run(milliseconds(500));
  EXPECT_TRUE(tel_link.sent.empty());
}

TEST_F(BridgeTest, TelemetrySubscriberCap) {
  config.telemetry.max_subscribers = 1;
  FcBridge b(fc, sensors, pilot, engine_link, tel_link, kEngine, config);
  pushSubscribe(kDisplayA);
  pushSubscribe(kDisplayB);
  b.step(now);
  EXPECT_EQ(b.stats().telemetry_subscriber_rejects, 1u);
  advance(milliseconds(100));
  b.step(now);
  for (const auto& [to, msg] : telemetrySent()) EXPECT_EQ(to, kDisplayA);
}

TEST_F(BridgeTest, TelemetryPortRejectsGarbageAndNonSubscribe) {
  tel_link.inbox.push_back({kDisplayA, std::vector<std::byte>(2)});
  tel_link.inbox.push_back({kDisplayA, fcwire::encode(fc::FcTelemetry{}, 0)});
  // An engine-protocol Subscribe has the wrong magic here and must not subscribe.
  tel_link.inbox.push_back({kDisplayA, wire::encode(proto::SubscribeMsg{}, 0)});
  bridge.step(now);
  EXPECT_EQ(bridge.stats().telemetry_malformed, 2u);
  EXPECT_EQ(bridge.stats().telemetry_unexpected, 1u);
  run(milliseconds(100));
  EXPECT_TRUE(tel_link.sent.empty());
}

TEST_F(BridgeTest, ShutdownSendsZeroControlAndResetsFc) {
  armedCommand();
  run(milliseconds(20));
  bridge.shutdown(now);
  EXPECT_EQ(controls().back().second.power, (std::array<float, 8>{}));
  EXPECT_EQ(fc.resets, 1);
}

TEST_F(BridgeTest, StatsLineAnnouncesProvenance) {
  const std::string s = bridge.formatStats();
  EXPECT_NE(s.find("sensors=SYNTHESIZED"), std::string::npos);
  EXPECT_NE(s.find("pilot=SPOOFED"), std::string::npos);
}

TEST_F(BridgeTest, NextDeadlineIsInTheFuture) {
  bridge.step(now);
  EXPECT_GT(bridge.nextDeadline(), now);
  EXPECT_LE(bridge.nextDeadline(), now + milliseconds(20));
}

/** @brief The whole chain with the real synthesized sensors and scripted pilot. */
TEST(BridgeIntegration, RealSourcesDriveTheNullAdapter) {
  Config config;
  config.fc.adapter = "null";
  const auto adapter = makeAdapter(config);
  fc::TemporarySynthSensors sensors(config.synthConfig());
  fc::ScriptedPilot pilot({{0.0, {0.f, 0.f, 0.f, 0.5f, fc::FlightMode::kNoGpsHold, true}}});
  FakeTransport engine_link, tel_link;
  FcBridge bridge(*adapter, sensors, pilot, engine_link, tel_link, kEngine, config);
  TimePoint now{std::chrono::seconds(1)};
  for (int i = 0; i < 20; ++i) {
    proto::StateMsg s;
    s.sim_time = 0.004 * i;
    engine_link.inbox.push_back({kEngine, wire::encode(s, static_cast<std::uint32_t>(i))});
    now += std::chrono::milliseconds(4);
    bridge.step(now);
  }
  EXPECT_GT(bridge.stats().sensor_frames, 10u);
  EXPECT_GT(bridge.stats().pilot_samples, 0u);
  EXPECT_EQ(adapter->telemetry().mode, fc::FlightMode::kNoGpsHold);
  EXPECT_TRUE(adapter->telemetry().armed);
}

TEST(AdapterFactory, Px4IsAConfigErrorWhenNotCompiledIn) {
#ifndef ASTROSIM_HAVE_PX4
  Config config;
  config.fc.adapter = "px4";
  EXPECT_THROW(makeAdapter(config), ConfigError);
#else
  GTEST_SKIP() << "PX4 adapter compiled in";
#endif
}

}  // namespace
}  // namespace astrosim::fcbridge
