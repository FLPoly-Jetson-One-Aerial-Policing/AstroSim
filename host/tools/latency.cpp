// Host-side latency: command -> gateway echo, and command -> first pose change, over UDP.
// usage: latency [host=127.0.0.1] [port=8765] [trials=20]
// Flies to 2 m, then per trial: hover 1.2 s, send a throttle step at an arbitrary phase of the 60 Hz tick,
// time (a) until a State echoes seq >= step seq, (b) until |z - z0| > 2 mm. Then recover and repeat.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wire.hpp"

using namespace astrosim;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static void report(const char* name, std::vector<double> v) {
  if (v.empty()) { std::printf("%s: no samples\n", name); return; }
  std::sort(v.begin(), v.end());
  auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
  std::printf("%s (n=%zu): p50=%.1f ms  p95=%.1f ms  max=%.1f ms\n", name, v.size(), pct(0.5), pct(0.95), v.back());
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  int port = argc > 2 ? std::atoi(argv[2]) : 8765;
  int trials = argc > 3 ? std::atoi(argv[3]) : 20;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET; to.sin_port = htons(port); inet_pton(AF_INET, host, &to.sin_addr);

  StateMsg s; bool have = false; uint32_t seq = 0;
  bool stateFresh = false;
  auto send = [&](const ControlMsg& c) { auto b = wire::encode(c, ++seq); ::sendto(fd, b.data(), b.size(), 0, (sockaddr*)&to, sizeof to); };
  auto pump = [&](int wait_ms) {  // read all pending States
    pollfd p{fd, POLLIN, 0};
    int w = wait_ms;
    while (::poll(&p, 1, w) > 0) {
      w = 0;
      uint8_t buf[512];
      ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      auto pkt = wire::decode(buf, (size_t)n);
      if (pkt) if (auto* st = std::get_if<StateMsg>(&pkt->msg)) { s = *st; have = true; stateFresh = true; }
    }
  };

  // Phase 0: climb to 2 m with a P controller.
  auto t0 = Clock::now(); auto next = t0;
  while (since(t0) < 8000) {
    ControlMsg c; c.throttle = std::clamp(0.8f * (2.0f - s.pos[2]) - 0.3f * s.vel[2], -1.f, 1.f);
    send(c); next += std::chrono::microseconds(16667); pump(std::max<long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count()));
  }
  if (!have || std::fabs(s.pos[2] - 2.0f) > 0.2f) { std::printf("could not reach hover (z=%.2f, have=%d)\n", s.pos[2], have); return 1; }
  std::printf("hover at z=%.3f; running %d trials\n", s.pos[2], trials);

  std::vector<double> echo, resp, resp05, respv;
  int missed = 0;
  for (int i = 0; i < trials; ++i) {
    // hover 1.2 s with a hold controller (so trial starts from rest), jittering the phase
    auto h0 = Clock::now(); next = h0;
    while (since(h0) < 1200 + (i * 7) % 17) {
      ControlMsg c; c.throttle = std::clamp(0.8f * (2.0f - s.pos[2]) - 0.3f * s.vel[2], -1.f, 1.f);
      send(c); next += std::chrono::microseconds(16667); pump(std::max<long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count()));
    }
    pump(0);
    float z0 = s.pos[2];
    ControlMsg step; step.throttle = 1.0f;
    send(step); uint32_t stepSeq = seq; auto ts = Clock::now();
    double te = -1, tr = -1, t05 = -1, tv = -1;
    while (since(ts) < 1000 && (te < 0 || tr < 0 || t05 < 0 || tv < 0)) {
      stateFresh = false;
      pump(2);
      if (!stateFresh) continue;
      if (te < 0 && !wire::seqNewer(stepSeq, s.echo_seq)) te = since(ts);
      if (tr < 0 && std::fabs(s.pos[2] - z0) > 0.002f) tr = since(ts);
      if (t05 < 0 && std::fabs(s.pos[2] - z0) > 0.0005f) t05 = since(ts);
      if (tv < 0 && std::fabs(s.vel[2]) > 0.02f) tv = since(ts);
      send(step);  // keep commanding
    }
    if (te >= 0) echo.push_back(te); else ++missed;
    if (tr >= 0) resp.push_back(tr);
    if (t05 >= 0) resp05.push_back(t05);
    if (tv >= 0) respv.push_back(tv);
    // recover: P-hold back to 2 m
    auto r0 = Clock::now(); next = r0;
    while (since(r0) < 2500) {
      ControlMsg c; c.throttle = std::clamp(0.8f * (2.0f - s.pos[2]) - 0.3f * s.vel[2], -1.f, 1.f);
      send(c); next += std::chrono::microseconds(16667); pump(std::max<long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count()));
    }
  }
  report("command -> echo seq", echo);
  report("command -> |dz| > 0.5 mm", resp05);
  report("command -> |vz| > 0.02 m/s", respv);
  report("command -> |dz| > 2 mm", resp);
  std::printf("missed echoes: %d\n", missed);
  bool pass = !resp.empty() && [&] { auto v = resp; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(0.95 * v.size()))] < 50.0; }();
  std::printf("latency p95 < 50 ms: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
