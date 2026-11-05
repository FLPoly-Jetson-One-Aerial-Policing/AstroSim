// Host-side scripted client: P-controller climb to a target altitude over UDP, then hold. Prints z history.
// usage: climb [host=127.0.0.1] [port=8765] [target_z=2.0] [seconds=15]
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "wire.hpp"

using namespace astrosim;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  int port = argc > 2 ? std::atoi(argv[2]) : 8765;
  float target = argc > 3 ? std::atof(argv[3]) : 2.0f;
  double secs = argc > 4 ? std::atof(argv[4]) : 15.0;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET; to.sin_port = htons(port); inet_pton(AF_INET, host, &to.sin_addr);

  StateMsg s; bool have = false; uint32_t seq = 0;
  auto t0 = Clock::now(), next = t0, lastPrint = t0;
  float zmax = 0;
  while (std::chrono::duration<double>(Clock::now() - t0).count() < secs) {
    ControlMsg c;
    c.throttle = std::clamp(0.8f * (target - s.pos[2]) - 0.3f * s.vel[2], -1.f, 1.f);
    auto b = wire::encode(c, ++seq);
    ::sendto(fd, b.data(), b.size(), 0, (sockaddr*)&to, sizeof to);
    next += std::chrono::microseconds(16667);
    for (;;) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, std::max<long>(left, 0)) <= 0) break;
      uint8_t buf[512];
      ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      auto pkt = wire::decode(buf, (size_t)n);
      if (pkt) if (auto* st = std::get_if<StateMsg>(&pkt->msg)) { s = *st; have = true; zmax = std::max(zmax, s.pos[2]); }
    }
    if (Clock::now() - lastPrint > std::chrono::seconds(1)) {
      lastPrint = Clock::now();
      std::printf("t=%.1f sim=%.2f z=%.3f vz=%.3f echo=%u gz=%d\n",
                  std::chrono::duration<double>(Clock::now() - t0).count(), s.sim_time, s.pos[2], s.vel[2], s.echo_seq,
                  (s.flags & kGzConnected) ? 1 : 0);
    }
  }
  float err = std::fabs(s.pos[2] - target);
  std::printf("final z=%.3f target=%.2f err=%.3f zmax=%.3f -> %s\n", s.pos[2], target, err, zmax,
              have && err <= 0.2f ? "PASS" : "FAIL");
  return have && err <= 0.2f ? 0 : 1;
}
