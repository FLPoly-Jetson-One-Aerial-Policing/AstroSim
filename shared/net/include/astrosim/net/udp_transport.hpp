#pragma once
/**
 * @file udp_transport.hpp
 * @brief Non-blocking IPv4 UDP implementation of Transport (POSIX sockets).
 *
 * @warning POSIX only (BSD sockets, poll-able descriptor). This is container-only code: it runs inside the Linux
 * engine and flight computer containers. Runtime code that must work on Windows/macOS uses the OS-free Transport
 * abstraction in transport.hpp and gets its own platform implementation.
 */
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "astrosim/log/log.hpp"
#include "astrosim/net/transport.hpp"

namespace astrosim::net {

/** @brief UDP socket bound to a local IPv4 address; owns and closes the descriptor. */
class UdpTransport : public Transport {
 public:
  /**
   * @brief Create, configure and bind the socket (non-blocking, SO_REUSEADDR).
   * @param bind_addr IPv4 dotted-quad address to bind.
   * @param port UDP port to bind.
   * @throws std::system_error if the socket cannot be created or bound; std::invalid_argument for a bad address.
   */
  UdpTransport(const std::string& bind_addr, std::uint16_t port);

  /** @brief Close the socket. */
  ~UdpTransport() override;

  UdpTransport(const UdpTransport&) = delete;
  UdpTransport& operator=(const UdpTransport&) = delete;

  /** @return The socket descriptor, for poll(). Remains owned by this object. */
  int fd() const { return fd_; }

  /**
   * @brief Receive one datagram without blocking.
   *
   * Datagrams larger than proto::wire::kMaxDatagramSize are returned with their true length (up to the internal
   * buffer) so the core counts them as malformed rather than silently truncating them to a valid size.
   * @return The datagram, or std::nullopt when none is pending.
   * @throws std::system_error on an unexpected socket error.
   */
  std::optional<Datagram> receive() override;

  /**
   * @brief Send one datagram. Errors are logged (rate-limited) and never thrown.
   * @param to Destination.
   * @param bytes Payload.
   */
  void send(const Endpoint& to, std::span<const std::byte> bytes) override;

 private:
  int fd_ = -1;
  log::RateLimiter send_warn_{std::chrono::seconds(5)};
};

}  // namespace astrosim::net
