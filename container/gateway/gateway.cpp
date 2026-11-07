#include "gateway.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace astrosim {
namespace {
double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

Gateway::Gateway(GzSide& gz, const GatewayConfig& cfg) : gz_(gz), cfg_(cfg) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (fd_ < 0) return;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(cfg.port);
  socklen_t len = sizeof a;
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 ||
      ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) < 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  port_ = ntohs(a.sin_port);
}

Gateway::~Gateway() { if (fd_ >= 0) ::close(fd_); }

void Gateway::onControl(Client& c, uint32_t seq, const ControlMsg& m, double now) {
  if (c.has_seq && !wire::seqNewer(seq, c.last_seq)) return;  // stale or duplicate
  c.has_seq = true;
  c.last_seq = seq;
  c.echo_seq = seq;
  gz_.command(headingToBody(mix(m), gz_.state().quat));
  last_control_ = now;
  neutral_sent_ = false;
}

void Gateway::drain(double now) {
  uint8_t buf[2048];
  for (;;) {
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen);
    if (n < 0) return;
    auto pkt = wire::decode(buf, static_cast<size_t>(n));
    if (!pkt) continue;
    if (auto* ctl = std::get_if<ControlMsg>(&pkt->msg)) {
      Key key{from.sin_addr.s_addr, from.sin_port};
      Client& c = clients_[key];
      if (c.last_seen == 0) c.addr = from;
      c.last_seen = now;
      onControl(c, pkt->seq, *ctl, now);
    }
    // Fault and State from clients: ignored (motor-out is a separate spike).
  }
}

void Gateway::tick(double now) {
  if (!neutral_sent_ && now - last_control_ > cfg_.deadman_s) {
    gz_.command(VelCmd{});
    neutral_sent_ = true;
  }
  for (auto it = clients_.begin(); it != clients_.end();) {
    it = (now - it->second.last_seen > cfg_.client_timeout_s) ? clients_.erase(it) : std::next(it);
  }
  StateMsg s = gz_.state();
  s.flags = gz_.connected() ? kGzConnected : 0;
  ++state_seq_;
  for (auto& [key, c] : clients_) {
    s.echo_seq = c.echo_seq;
    auto bytes = wire::encode(s, state_seq_);
    ::sendto(fd_, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&c.addr), sizeof c.addr);
  }
}

bool Gateway::run(const std::atomic<bool>& stop) {
  const double period = 1.0 / cfg_.tick_hz;
  double start = nowSec(), next = start, next_enable = start, last_client = start;
  while (!stop.load()) {
    double now = nowSec();
    if (now >= next) {
      next += period;
      if (next < now) next = now + period;  // fell behind: don't burst
      tick(now);
      if (!clients_.empty()) last_client = now;
      if (now >= next_enable) { gz_.enable(); next_enable = now + 1.0; }
      if (clients_.empty() && now - last_client > cfg_.idle_exit_s) return true;
    }
    pollfd p{fd_, POLLIN, 0};
    int wait_ms = static_cast<int>((next - nowSec()) * 1000);
    if (wait_ms < 0) wait_ms = 0;
    if (::poll(&p, 1, wait_ms) > 0) drain(nowSec());
  }
  return false;
}

}  // namespace astrosim
