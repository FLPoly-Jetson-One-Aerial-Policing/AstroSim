#pragma once
/**
 * @file transport.hpp
 * @brief Datagram endpoint and transport abstraction.
 *
 * Pure, OS-free abstraction (no socket headers). A pure-logic core, such as the engine Gateway or the flight
 * computer bridge, uses only Endpoint, Datagram and Transport. The event loop moves bytes between a Transport
 * (UdpTransport in production) and the core, which keeps the core free of sockets and testable.
 */
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace astrosim::net {

/// @brief An IPv4 endpoint, in host byte order.
struct Endpoint {
  std::uint32_t addr = 0;  ///< IPv4 address, host byte order.
  std::uint16_t port = 0;  ///< UDP port, host byte order.

  /**
   * @brief Total ordering, so endpoints can key ordered containers.
   * @return Ordering by address, then port.
   */
  auto operator<=>(const Endpoint&) const = default;

  /**
   * @brief Format for logs.
   * @return "a.b.c.d:port".
   */
  std::string toString() const;
};

/// @brief One datagram and its peer (source on receive, destination on send).
struct Datagram {
  Endpoint peer;                 ///< Remote endpoint.
  std::vector<std::byte> bytes;  ///< Datagram payload.
};

/// @brief Abstract non-blocking datagram transport.
class Transport {
 public:
  virtual ~Transport() = default;

  /**
   * @brief Receive without blocking.
   * @return The next pending datagram, or std::nullopt if none is pending.
   */
  virtual std::optional<Datagram> receive() = 0;

  /**
   * @brief Send one datagram (best effort, like UDP).
   * @param to Destination endpoint.
   * @param bytes Datagram payload.
   */
  virtual void send(const Endpoint& to, std::span<const std::byte> bytes) = 0;
};

}  // namespace astrosim::net
