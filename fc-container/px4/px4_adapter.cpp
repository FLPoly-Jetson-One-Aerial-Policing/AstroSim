/** @file px4_adapter.cpp @brief POSIX socket glue of the PX4 adapter. */
#include "px4_adapter.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>

#include "astrosim/log/log.hpp"

namespace astrosim::px4 {

namespace {

/// @brief Largest backlog of unsent HIL bytes before the client is considered stuck and dropped.
constexpr std::size_t kMaxHilBacklog = 1u << 20;

/**
 * @brief Make a descriptor non-blocking.
 * @param fd Descriptor.
 * @throws std::runtime_error on failure.
 */
void setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw std::runtime_error(std::format("fcntl(O_NONBLOCK): {}", std::strerror(errno)));
  }
}

/**
 * @brief Build an IPv4 any-address socket address.
 * @param port Port, host byte order.
 * @return The address.
 */
sockaddr_in anyAddress(std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  return a;
}

}  // namespace

Px4Adapter::Px4Adapter(const Px4AdapterConfig& config) : config_(config), session_(config.session) {
  in_addr peer{};
  if (::inet_pton(AF_INET, config.mavlink_host.c_str(), &peer) != 1) {
    throw std::runtime_error(std::format("px4: mavlink_host '{}' is not an IPv4 address", config.mavlink_host));
  }
  peer_ip_ = peer.s_addr;
  peer_port_ = config.mavlink_port;

  try {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error(std::format("socket(TCP): {}", std::strerror(errno)));
    const int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in a = anyAddress(config.hil_port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
      throw std::runtime_error(std::format("cannot bind HIL TCP port {}: {}", config.hil_port, std::strerror(errno)));
    }
    if (::listen(listen_fd_, 1) < 0) throw std::runtime_error(std::format("listen: {}", std::strerror(errno)));
    setNonBlocking(listen_fd_);

    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) throw std::runtime_error(std::format("socket(UDP): {}", std::strerror(errno)));
    a = anyAddress(config.mavlink_local_port);
    if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
      throw std::runtime_error(
          std::format("cannot bind MAVLink UDP port {}: {}", config.mavlink_local_port, std::strerror(errno)));
    }
    setNonBlocking(udp_fd_);
  } catch (...) {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (udp_fd_ >= 0) ::close(udp_fd_);
    throw;
  }
  if (config.session.dither.enabled) {
    log::warn("px4: adding a tiny dither to the sensors (PX4 drops a sensor whose value never changes; the "
              "synthesized sensors are noise free). Set [px4] sensor_dither = false when sensors carry real noise.");
  }
  log::info("px4: HIL link listening on tcp/{}, MAVLink on udp/{} to {}:{}", config.hil_port,
            config.mavlink_local_port, config.mavlink_host, config.mavlink_port);
}

Px4Adapter::~Px4Adapter() {
  if (hil_fd_ >= 0) ::close(hil_fd_);
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (udp_fd_ >= 0) ::close(udp_fd_);
}

void Px4Adapter::dropHilClient(const char* why) {
  log::warn("px4: HIL connection closed ({})", why);
  if (hil_fd_ >= 0) ::close(hil_fd_);
  hil_fd_ = -1;
  hil_pending_.clear();
  session_.onHilDisconnected();
}

void Px4Adapter::acceptHil(fc::TimePoint now) {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;  // EAGAIN (nothing pending) or a transient error
    if (hil_fd_ >= 0) dropHilClient("replaced by a new connection");
    setNonBlocking(fd);
    const int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);  // lockstep: latency matters
    hil_fd_ = fd;
    session_.onHilConnected(now);
    log::info("px4: HIL connection established (PX4 simulator_mavlink connected)");
  }
}

void Px4Adapter::readHil(fc::TimePoint now) {
  std::uint8_t buf[4096];
  while (hil_fd_ >= 0) {
    const ssize_t n = ::recv(hil_fd_, buf, sizeof buf, 0);
    if (n > 0) {
      session_.feedHil({buf, static_cast<std::size_t>(n)}, now);
    } else if (n == 0) {
      dropHilClient("closed by PX4");
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    } else if (errno != EINTR) {
      dropHilClient(std::strerror(errno));
    }
  }
}

void Px4Adapter::readMavlink(fc::TimePoint now) {
  std::uint8_t buf[2048];
  for (;;) {
    const ssize_t n = ::recv(udp_fd_, buf, sizeof buf, 0);
    if (n > 0) {
      session_.feedMavlink({buf, static_cast<std::size_t>(n)}, now);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return;  // EAGAIN, or an ICMP-induced error: nothing more to read
    }
  }
}

void Px4Adapter::flush() {
  Bytes hil = session_.takeHilTx();
  if (hil_fd_ >= 0) {
    hil_pending_.insert(hil_pending_.end(), hil.begin(), hil.end());
    std::size_t sent = 0;
    while (sent < hil_pending_.size()) {
      const ssize_t n = ::send(hil_fd_, hil_pending_.data() + sent, hil_pending_.size() - sent, MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        dropHilClient(std::strerror(errno));
        return;
      }
    }
    hil_pending_.erase(hil_pending_.begin(), hil_pending_.begin() + static_cast<std::ptrdiff_t>(sent));
    if (hil_pending_.size() > kMaxHilBacklog) dropHilClient("PX4 stopped reading");
  }

  const Bytes mav = session_.takeMavlinkTx();
  if (!mav.empty()) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = peer_ip_;
    to.sin_port = htons(peer_port_);
    // At most a few small frames accumulate per poll (MANUAL_CONTROL plus an occasional command): one datagram.
    (void)::sendto(udp_fd_, mav.data(), mav.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  }
}

void Px4Adapter::onSensors(const fc::SensorFrame& frame, fc::TimePoint now) {
  session_.onSensors(frame, now);
  flush();  // lockstep: PX4 is waiting for this sample
}

void Px4Adapter::onPilot(const fc::PilotInput& input, fc::TimePoint now) {
  session_.onPilot(input, now);
}

void Px4Adapter::poll(fc::TimePoint now) {
  acceptHil(now);
  readHil(now);
  readMavlink(now);
  session_.poll(now);
  flush();
}

void Px4Adapter::reset() {
  if (hil_fd_ >= 0) {
    ::close(hil_fd_);
    hil_fd_ = -1;
  }
  hil_pending_.clear();
  session_.reset();
}

}  // namespace astrosim::px4

namespace astrosim::fcbridge {

std::unique_ptr<fc::FlightComputer> makePx4Adapter(const Config& config) {
  px4::Px4AdapterConfig c;
  c.hil_port = config.px4.hil_port;
  c.mavlink_local_port = config.px4.mavlink_local_port;
  c.mavlink_host = config.px4.mavlink_host;
  c.mavlink_port = config.px4.mavlink_port;
  c.session.no_gps_mode = config.px4.no_gps_mode == "stabilized" ? px4::NoGpsMode::kStabilized : px4::NoGpsMode::kAltctl;
  c.session.dither.enabled = config.px4.sensor_dither;
  return std::make_unique<px4::Px4Adapter>(c);
}

}  // namespace astrosim::fcbridge
