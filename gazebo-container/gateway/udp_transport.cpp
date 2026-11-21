/** @file udp_transport.cpp @brief POSIX UDP transport. */
#include "udp_transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include "astrosim/proto/wire.hpp"

namespace astrosim::gateway {

namespace {
constexpr std::size_t kRecvBuffer = 2048;  // > kMaxDatagramSize so oversize datagrams are detected
static_assert(kRecvBuffer > proto::wire::kMaxDatagramSize);

/** @brief Build a system_error from errno with context. */
[[noreturn]] void throwErrno(const char* what) {
  throw std::system_error(errno, std::system_category(), what);
}
}  // namespace

UdpTransport::UdpTransport(const std::string& bind_addr, std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    throw std::invalid_argument("invalid IPv4 bind address: " + bind_addr);
  }
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) throwErrno("socket");
  const int one = 1;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
      ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) < 0) {
    const int err = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::system_error(err, std::system_category(), "bind " + bind_addr + ":" + std::to_string(port));
  }
}

UdpTransport::~UdpTransport() {
  if (fd_ >= 0) ::close(fd_);
}

std::optional<Datagram> UdpTransport::receive() {
  std::array<std::byte, kRecvBuffer> buf;
  sockaddr_in from{};
  socklen_t from_len = sizeof from;
  ssize_t n = 0;
  for (;;) {
    from_len = sizeof from;
    n = ::recvfrom(fd_, buf.data(), buf.size(), MSG_TRUNC, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n >= 0) break;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return std::nullopt;
    throwErrno("recvfrom");
  }
  // With MSG_TRUNC n is the real datagram length; keep at most what fits in the buffer. Anything above
  // kMaxDatagramSize is rejected by the core as BadSize either way.
  const std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(n), buf.size());
  Datagram d;
  d.peer = {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
  d.bytes.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(len));
  return d;
}

void UdpTransport::send(const Endpoint& to, std::span<const std::byte> bytes) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(to.addr);
  addr.sin_port = htons(to.port);
  for (;;) {
    const ssize_t n =
        ::sendto(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    if (n >= 0) return;
    if (errno == EINTR) continue;
    const int err = errno;
    if (const auto suppressed = send_warn_.allow(std::to_string(err), log::RateLimiter::Clock::now())) {
      log::warn("sendto {} failed: {} ({} similar suppressed)", to.toString(), std::strerror(err), *suppressed);
    }
    return;
  }
}

}  // namespace astrosim::gateway
