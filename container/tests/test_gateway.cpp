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
  // full diagonal is clamped to the governed speed, direction kept
  v = mix({1, 1, 0, 0});
  CHECK(std::fabs(std::hypot(v.vx, v.vy) - kMaxHoriz) < 1e-9 && std::fabs(v.vx + v.vy) < 1e-9 && v.vx > 0);
  v = mix({0.5f, 0.5f, 0, 0});   CHECK(std::fabs(v.vx - 0.5 * kMaxHoriz) < 1e-9);  // inside the limit: untouched
}

// q = rotation of `deg` about unit axis (ax, ay, az), x y z w
static void axisAngle(float q[4], double ax, double ay, double az, double deg) {
  double h = deg * M_PI / 360;
  q[0] = ax * std::sin(h); q[1] = ay * std::sin(h); q[2] = az * std::sin(h); q[3] = std::cos(h);
}
// what the gz controller does with a body command: rotate it into the world by q
static void bodyToWorld(const float q[4], const VelCmd& b, double out[3]) {
  double x = q[0], y = q[1], z = q[2], w = q[3];
  const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                          {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                          {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  for (int i = 0; i < 3; ++i) out[i] = R[i][0] * b.vx + R[i][1] * b.vy + R[i][2] * b.vz;
}
static bool near3(const double a[3], double x, double y, double z) {
  return std::fabs(a[0] - x) < 1e-5 && std::fabs(a[1] - y) < 1e-5 && std::fabs(a[2] - z) < 1e-5;
}

static void testHeadingToBody() {
  const VelCmd c{10, 2, -1, 0.5};
  float q[4] = {0, 0, 0, 1};
  VelCmd b = headingToBody(c, q);  // level: unchanged
  CHECK(std::fabs(b.vx - 10) < 1e-6 && std::fabs(b.vy - 2) < 1e-6 && std::fabs(b.vz + 1) < 1e-6 && b.yaw_rate == 0.5);
  float zero[4] = {0, 0, 0, 0};
  b = headingToBody(c, zero);      // no attitude yet: unchanged
  CHECK(b.vx == 10 && b.vy == 2 && b.vz == -1);
  double wv[3];
  axisAngle(q, 0, 0, 1, 90);       // heading only: still the same body command, world velocity turned with heading
  b = headingToBody(c, q); bodyToWorld(q, b, wv);
  CHECK(std::fabs(b.vx - 10) < 1e-5 && std::fabs(b.vy - 2) < 1e-5 && near3(wv, -2, 10, -1));
  axisAngle(q, 0, 1, 0, 14);       // nose down 14 deg (gz pitch +): forward stays level, no sink
  b = headingToBody(c, q); bodyToWorld(q, b, wv);
  CHECK(near3(wv, 10, 2, -1));
  axisAngle(q, 0.6, 0.48, 0.64, 33);  // arbitrary tilt + heading: world velocity = heading-rotated command
  b = headingToBody(c, q); bodyToWorld(q, b, wv);
  double x = q[0], y = q[1], z = q[2], w = q[3];
  double yaw = std::atan2(2 * (x * y + w * z), 1 - 2 * (y * y + z * z));
  CHECK(near3(wv, std::cos(yaw) * 10 - std::sin(yaw) * 2, std::sin(yaw) * 10 + std::cos(yaw) * 2, -1));
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
  testHeadingToBody();
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
