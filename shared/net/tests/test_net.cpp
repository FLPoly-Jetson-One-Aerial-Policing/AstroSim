/// @file
/// @brief Unit tests for the transport abstraction and the POSIX UDP transport (loopback).
#include <gtest/gtest.h>

#include <stdexcept>

#include "astrosim/net/transport.hpp"
#ifdef __unix__
#include "astrosim/net/udp_transport.hpp"
#endif

namespace astrosim::net {
namespace {

/// @brief Endpoint formats as dotted quad plus port and orders by address, then port.
TEST(Endpoint, FormatAndOrder) {
  const Endpoint e{0x7F000001u, 8765};
  EXPECT_EQ(e.toString(), "127.0.0.1:8765");
  EXPECT_LT((Endpoint{1, 9}), (Endpoint{2, 1}));
  EXPECT_LT((Endpoint{1, 1}), (Endpoint{1, 2}));
}

#ifdef __unix__
/// @brief A fresh socket binds, has a valid descriptor and reports no pending datagram; bad addresses throw.
TEST(UdpTransport, BindAndEmptyReceive) {
  UdpTransport rx("127.0.0.1", 0);
  EXPECT_FALSE(rx.receive().has_value());
  EXPECT_THROW(UdpTransport("not-an-address", 0), std::invalid_argument);
  EXPECT_GE(rx.fd(), 0);
}
#endif

}  // namespace
}  // namespace astrosim::net
