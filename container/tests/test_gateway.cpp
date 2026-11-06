#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <mutex>
#include <thread>
#include <vector>

#include "gateway.hpp"

using namespace astrosim;
using Clock = std::chrono::steady_clock;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct FakeGz : GzSide {
  std::mutex mu;
  bool conn = true;
  std::vector<VelCmd> cmds;
  int enables = 0;
  bool connected() override { std::lock_guard<std::mutex> l(mu); return conn; }
  StateMsg state() override { StateMsg s; s.pos[2] = 1.5f; s.sim_time = 3.0; return s; }
  void command(const VelCmd& c) override { std::lock_guard<std::mutex> l(mu); cmds.push_back(c); }
  void enable() override { std::lock_guard<std::mutex> l(mu); ++enables; }
  size_t count() { std::lock_guard<std::mutex> l(mu); return cmds.size(); }
  VelCmd last() { std::lock_guard<std::mutex> l(mu); return cmds.back(); }
};

struct Client {
  int fd;
  sockaddr_in to{};
  explicit Client(uint16_t port) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    to.sin_family = AF_INET; to.sin_port = htons(port); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
  }
  ~Client() { ::close(fd); }
  void send(const std::vector<uint8_t>& b) { ::sendto(fd, b.data(), b.size(), 0, (sockaddr*)&to, sizeof to); }
  void sendControl(const ControlMsg& m, uint32_t seq) { send(wire::encode(m, seq)); }
  // Next valid State within timeout_ms, else nullopt. Skips non-State packets.
  std::optional<StateMsg> recvState(int timeout_ms) {
    auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
      if (left <= 0) return std::nullopt;
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, left) <= 0) return std::nullopt;
      uint8_t buf[512];
      ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      auto pkt = wire::decode(buf, (size_t)n);
      if (pkt) if (auto* s = std::get_if<StateMsg>(&pkt->msg)) return *s;
    }
  }
  void drain() { uint8_t b[512]; while (::recv(fd, b, sizeof b, MSG_DONTWAIT) > 0) {} }
};

struct Harness {
  FakeGz gz;
  GatewayConfig cfg;
  std::unique_ptr<Gateway> gw;
  std::atomic<bool> stop{false};
  std::thread th;
  bool idle_exit = false;
  explicit Harness(GatewayConfig c) : cfg(c) {
    cfg.port = 0;
    gw = std::make_unique<Gateway>(gz, cfg);
    th = std::thread([this] { idle_exit = gw->run(stop); });
  }
  ~Harness() { stop = true; th.join(); }
};

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static void testMixer() {
  VelCmd v = mix({0, 1, 0, 0});  CHECK(v.vx == kMaxHoriz && v.vy == 0 && v.vz == 0 && v.yaw_rate == 0);
  v = mix({1, 0, 0, 0});         CHECK(v.vy == -kMaxHoriz);
  v = mix({0, 0, 1, 0});         CHECK(v.yaw_rate == -kMaxYaw);
  v = mix({0, 0, 0, 1});         CHECK(v.vz == kMaxVert);
  v = mix({0, 0, 0, 0});         CHECK(v.vx == 0 && v.vy == 0 && v.vz == 0 && v.yaw_rate == 0);
  v = mix({0, 7, 0, -9});        CHECK(v.vx == kMaxHoriz && v.vz == -kMaxVert);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  v = mix({nan, nan, nan, nan}); CHECK(v.vx == 0 && v.vy == 0 && v.vz == 0 && v.yaw_rate == 0);
}

static void testStreamAndEcho() {
  Harness h({});
  Client c(h.gw->port());
  c.sendControl({0, 0.5f, 0, 0.25f}, 10);
  auto s = c.recvState(500);
  CHECK(s.has_value());
  // wait for a state that has the echo applied
  int n = 0; bool echoed = false;
  auto t0 = Clock::now();
  while (Clock::now() - t0 < std::chrono::milliseconds(500)) {
    auto st = c.recvState(100);
    if (!st) break;
    ++n;
    if (st->echo_seq == 10) echoed = true;
    CHECK(st->pos[2] == 1.5f && (st->flags & kGzConnected));
  }
  CHECK(echoed);
  CHECK(n > 15 && n < 45);  // ~60 Hz until the 0.5 s deadman... stream continues; loose bound over 0.5 s window
  CHECK(h.gz.count() >= 1);
  VelCmd v = h.gz.cmds.front();
  CHECK(v.vx == 0.5 * kMaxHoriz && v.vz == 0.25 * kMaxVert);
  sleepMs(1100);
  CHECK(h.gz.enables >= 1);
}

static void testNanSanitized() {
  Harness h({});
  Client c(h.gw->port());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  c.sendControl({nan, 1.f, nan, nan}, 1);
  sleepMs(100);
  CHECK(h.gz.count() == 1);
  VelCmd v = h.gz.last();
  CHECK(v.vx == kMaxHoriz && v.vy == 0 && v.vz == 0 && v.yaw_rate == 0);
}

static void testGarbageIgnored() {
  Harness h({});
  Client good(h.gw->port()), bad(h.gw->port());
  good.sendControl({0, 0, 0, 0}, 1);
  bad.send({1, 2, 3});
  bad.send(std::vector<uint8_t>(500, 0xAA));
  auto trunc = wire::encode(ControlMsg{}, 1); trunc.pop_back(); bad.send(trunc);
  auto wrongver = wire::encode(ControlMsg{}, 1); wrongver[2] = 9; bad.send(wrongver);
  bad.send(wire::encode(StateMsg{}, 1));  // clients must not be able to inject State
  CHECK(good.recvState(500).has_value());
  CHECK(!bad.recvState(300).has_value());  // never registered
  CHECK(h.gz.count() == 1);                // only the good control
}

static void testStaleSeq() {
  Harness h({});
  Client c(h.gw->port());
  c.sendControl({0, 1, 0, 0}, 10);
  sleepMs(50);
  c.sendControl({0, -1, 0, 0}, 5);   // stale
  c.sendControl({0, -1, 0, 0}, 10);  // duplicate
  sleepMs(100);
  CHECK(h.gz.count() == 1 && h.gz.last().vx == kMaxHoriz);
  c.sendControl({0, 0, 0, 0}, 11);
  sleepMs(50);
  CHECK(h.gz.count() == 2);
  // wraparound (fresh client so seq may start near the top)
  Client w(h.gw->port());
  w.sendControl({0, 1, 0, 0}, 0xFFFFFFFFu);
  w.sendControl({0, 0.5f, 0, 0}, 0u);
  sleepMs(100);
  CHECK(h.gz.count() == 4 && h.gz.last().vx == 0.5 * kMaxHoriz);
}

static void testDeadman() {
  GatewayConfig cfg; cfg.deadman_s = 0.15;
  Harness h(cfg);
  Client c(h.gw->port());
  c.sendControl({0, 1, 0, 1}, 1);
  sleepMs(60);
  CHECK(h.gz.count() == 1);
  sleepMs(300);
  CHECK(h.gz.count() == 2);  // exactly one neutral
  VelCmd v = h.gz.last();
  CHECK(v.vx == 0 && v.vy == 0 && v.vz == 0 && v.yaw_rate == 0);
  sleepMs(300);
  CHECK(h.gz.count() == 2);  // not repeated
  c.sendControl({0, 1, 0, 0}, 2);
  sleepMs(60);
  CHECK(h.gz.count() == 3 && h.gz.last().vx == kMaxHoriz);
}

static void testGzDisconnectedFlag() {
  Harness h({});
  Client c(h.gw->port());
  c.sendControl({}, 1);
  auto s = c.recvState(500); CHECK(s && (s->flags & kGzConnected));
  { std::lock_guard<std::mutex> l(h.gz.mu); h.gz.conn = false; }
  sleepMs(100); c.drain();
  s = c.recvState(500); CHECK(s && !(s->flags & kGzConnected));
}

static void testClientTimeoutAndSecondClient() {
  GatewayConfig cfg; cfg.client_timeout_s = 0.3;
  Harness h(cfg);
  Client a(h.gw->port()), b(h.gw->port());
  a.sendControl({}, 1);
  b.sendControl({}, 1);
  CHECK(a.recvState(300) && b.recvState(300));
  // b keeps talking, a goes silent
  for (int i = 2; i < 12; ++i) { b.sendControl({}, i); sleepMs(60); }
  a.drain();
  CHECK(!a.recvState(200).has_value());
  CHECK(b.recvState(200).has_value());
  // a returns (new registration)
  a.sendControl({}, 2);
  CHECK(a.recvState(300).has_value());
}

static void testIdleExit() {
  GatewayConfig cfg; cfg.idle_exit_s = 0.3;
  Harness h(cfg);
  sleepMs(700);
  CHECK(h.idle_exit);
}

int main() {
  testMixer();
  testStreamAndEcho();
  testNanSanitized();
  testGarbageIgnored();
  testStaleSeq();
  testDeadman();
  testGzDisconnectedFlag();
  testClientTimeoutAndSecondClient();
  testIdleExit();
  if (fails) { std::fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  std::puts("test_gateway OK");
  return 0;
}
