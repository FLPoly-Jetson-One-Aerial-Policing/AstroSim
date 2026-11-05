#include "udp_link.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

#include "wire.hpp"

namespace astrosim {
namespace {
double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

UdpLink::UdpLink(const std::string& ipv4, uint16_t port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) return;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  if (inet_pton(AF_INET, ipv4.c_str(), &to.sin_addr) != 1 ||
      ::connect(fd_, reinterpret_cast<sockaddr*>(&to), sizeof to) < 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  th_ = std::thread([this] { rx(); });
}

UdpLink::~UdpLink() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  if (fd_ >= 0) ::close(fd_);
}

void UdpLink::send(const ControlMsg& c) {
  if (fd_ < 0) return;
  auto b = wire::encode(c, ++seq_);
  ::send(fd_, b.data(), b.size(), MSG_NOSIGNAL);  // ECONNREFUSED while the engine is down is expected
}

void UdpLink::rx() {
  while (!stop_) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) continue;
    uint8_t buf[512];
    ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) continue;
    auto pkt = wire::decode(buf, static_cast<size_t>(n));
    if (!pkt) continue;
    if (auto* s = std::get_if<StateMsg>(&pkt->msg)) {
      std::lock_guard<std::mutex> l(mu_);
      state_ = *s;
      last_rx_ = nowSec();
    }
  }
}

StateMsg UdpLink::latest() {
  std::lock_guard<std::mutex> l(mu_);
  return state_;
}

bool UdpLink::gatewayAlive(double timeout_s) { return last_rx_ > 0 && nowSec() - last_rx_ < timeout_s; }

bool UdpLink::connected(double timeout_s) {
  return gatewayAlive(timeout_s) && (latest().flags & kGzConnected);
}

}  // namespace astrosim
