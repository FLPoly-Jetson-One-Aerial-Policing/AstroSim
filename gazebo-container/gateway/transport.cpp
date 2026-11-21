/** @file transport.cpp @brief Endpoint formatting. */
#include "transport.hpp"

#include <format>

namespace astrosim::gateway {

std::string Endpoint::toString() const {
  return std::format("{}.{}.{}.{}:{}", (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, port);
}

}  // namespace astrosim::gateway
