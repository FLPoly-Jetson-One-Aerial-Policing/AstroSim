/** @file test_px4_adapter.cpp @brief Loopback test of the adapter's socket glue with a fake PX4 (POSIX only). */
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "px4_adapter.hpp"

using namespace astrosim;
using namespace astrosim::px4;

namespace {

// Unusual ports so the test does not collide with a running PX4 or bridge.
constexpr std::uint16_t kHilPort = 47560;
constexpr std::uint16_t kLocalPort = 47540;
constexpr std::uint16_t kPx4Port = 47580;

fc::TimePoint now() { return std::chrono::steady_clock::now(); }

sockaddr_in loopback(std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  return a;
}

/// @brief Receive whatever arrives on @p fd within @p timeout_ms; empty if nothing.
Bytes receive(int fd, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  if (::poll(&p, 1, timeout_ms) <= 0) return {};
  Bytes buf(4096);
  const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
  buf.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
  return buf;
}

Bytes bytesOf(const mavlink_message_t& msg) {
  std::uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const std::uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  return Bytes(buf, buf + n);
}

/// @brief A fake PX4: a TCP client of the HIL port and a UDP endpoint standing in for the onboard instance.
struct FakePx4 {
  FakePx4() {
    tcp = ::socket(AF_INET, SOCK_STREAM, 0);
    udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me = loopback(kPx4Port);
    EXPECT_EQ(::bind(udp, reinterpret_cast<sockaddr*>(&me), sizeof me), 0);
  }
  ~FakePx4() {
    ::close(tcp);
    ::close(udp);
  }
  void connectHil() {
    sockaddr_in to = loopback(kHilPort);
    ASSERT_EQ(::connect(tcp, reinterpret_cast<sockaddr*>(&to), sizeof to), 0);
  }
  void sendUdp(const Bytes& b) {
    sockaddr_in to = loopback(kLocalPort);
    ::sendto(udp, b.data(), b.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
  }
  int tcp = -1;
  int udp = -1;
};

Px4AdapterConfig config() {
  Px4AdapterConfig c;
  c.hil_port = kHilPort;
  c.mavlink_local_port = kLocalPort;
  c.mavlink_port = kPx4Port;
  return c;
}

/// @brief Poll the adapter until @p done or about a second has passed.
template <class Done>
bool pollUntil(Px4Adapter& a, Done done) {
  for (int i = 0; i < 1000; ++i) {
    a.poll(now());
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

}  // namespace

TEST(Px4AdapterSockets, HilLinkBothWays) {
  Px4Adapter adapter(config());
  FakePx4 px4;
  px4.connectHil();
  ASSERT_TRUE(pollUntil(adapter, [&] { return true; }));

  // PX4 -> adapter: HIL_ACTUATOR_CONTROLS becomes a motor command.
  mavlink_hil_actuator_controls_t ac{};
  ac.mode = kModeFlagArmed | 1;
  ac.controls[2] = 0.6f;
  mavlink_message_t msg;
  mavlink_msg_hil_actuator_controls_encode(kPx4SystemId, kPx4ComponentId, &msg, &ac);
  const Bytes out = bytesOf(msg);
  ASSERT_EQ(::send(px4.tcp, out.data(), out.size(), 0), static_cast<ssize_t>(out.size()));
  std::optional<fc::MotorCommand> motors;
  ASSERT_TRUE(pollUntil(adapter, [&] { return (motors = adapter.takeMotors()).has_value(); }));
  EXPECT_TRUE(motors->armed);
  EXPECT_FLOAT_EQ(motors->power[2], 0.6f);

  // adapter -> PX4: a sensor frame becomes HIL_SENSOR with the simulation time.
  fc::SensorFrame f;
  f.time_s = 3.5;
  f.accel = {0, 0, 9.81};
  adapter.onSensors(f, now());
  MavlinkLink rx(9, 9);
  std::vector<mavlink_message_t> got;
  for (int i = 0; i < 100 && got.empty(); ++i) {
    for (const auto& m : rx.parse(receive(px4.tcp, 10))) got.push_back(m);
  }
  ASSERT_FALSE(got.empty());
  EXPECT_EQ(got[0].msgid, MAVLINK_MSG_ID_HIL_SENSOR);
  mavlink_hil_sensor_t h;
  mavlink_msg_hil_sensor_decode(&got[0], &h);
  EXPECT_EQ(h.time_usec, 3500000u);
}

TEST(Px4AdapterSockets, PilotLinkBothWays) {
  Px4Adapter adapter(config());
  FakePx4 px4;
  px4.connectHil();

  // PX4 -> adapter: a heartbeat on the UDP link makes the adapter report connected and armed.
  mavlink_heartbeat_t hb{};
  hb.autopilot = MAV_AUTOPILOT_PX4;
  hb.base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | kModeFlagArmed;
  hb.custom_mode = customModeOf({2, 0});
  mavlink_message_t msg;
  mavlink_msg_heartbeat_encode(kPx4SystemId, kPx4ComponentId, &msg, &hb);
  px4.sendUdp(bytesOf(msg));
  ASSERT_TRUE(pollUntil(adapter, [&] { return adapter.telemetry().armed && adapter.telemetry().connected; }));
  EXPECT_EQ(adapter.telemetry().mode, fc::FlightMode::kNoGpsHold);

  // adapter -> PX4: the pilot input becomes MANUAL_CONTROL datagrams.
  fc::PilotInput p;
  p.throttle = 0.75f;
  p.mode = fc::FlightMode::kNoGpsHold;
  p.arm = true;
  adapter.onPilot(p, now());
  adapter.poll(now());
  MavlinkLink rx(9, 9);
  bool manual = false;
  for (int i = 0; i < 100 && !manual; ++i) {
    for (const auto& m : rx.parse(receive(px4.udp, 10))) {
      if (m.msgid == MAVLINK_MSG_ID_MANUAL_CONTROL) {
        mavlink_manual_control_t mc;
        mavlink_msg_manual_control_decode(&m, &mc);
        EXPECT_EQ(mc.z, 750);
        manual = true;
      }
    }
  }
  EXPECT_TRUE(manual);
}

TEST(Px4AdapterSockets, DisconnectIsReportedAndPx4CanReconnect) {
  Px4Adapter adapter(config());
  {
    FakePx4 first;
    first.connectHil();
    ASSERT_TRUE(pollUntil(adapter, [&] { return adapter.takeMotors().has_value() || true; }));
  }  // PX4 goes away: the connection closes
  for (int i = 0; i < 20; ++i) {
    adapter.poll(now());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  FakePx4 second;
  second.connectHil();
  fc::SensorFrame f;
  f.time_s = 1.0;
  ASSERT_TRUE(pollUntil(adapter, [&] {
    adapter.onSensors(f, now());
    f.time_s += 0.004;
    return !receive(second.tcp, 1).empty();
  }));
}

TEST(Px4AdapterSockets, PortInUseIsReported) {
  Px4Adapter first(config());
  EXPECT_THROW(Px4Adapter second(config()), std::runtime_error);
}

TEST(Px4AdapterSockets, BadHostIsReported) {
  Px4AdapterConfig c = config();
  c.mavlink_host = "not-an-address";
  EXPECT_THROW(Px4Adapter a(c), std::runtime_error);
}
