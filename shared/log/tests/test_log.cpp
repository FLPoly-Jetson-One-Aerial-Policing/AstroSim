/// @file
/// @brief Unit tests for the shared logger and rate limiter.
#include <gtest/gtest.h>

#include <chrono>

#include "astrosim/log/log.hpp"

namespace astrosim::log {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

/// @brief Known level names parse; anything else does not.
TEST(Log, ParseLevel) {
  EXPECT_EQ(parseLevel("trace"), Level::Trace);
  EXPECT_EQ(parseLevel("warn"), Level::Warn);
  EXPECT_EQ(parseLevel("error"), Level::Error);
  EXPECT_FALSE(parseLevel("loud").has_value());
  EXPECT_FALSE(parseLevel("").has_value());
}

/// @brief setLevel() is observable through level().
TEST(Log, SetLevel) {
  const Level before = level();
  setLevel(Level::Error);
  EXPECT_EQ(level(), Level::Error);
  setLevel(before);
}

/// @brief The first event per key passes, later ones are suppressed and counted until the interval elapses.
TEST(RateLimiter, AllowsOncePerInterval) {
  RateLimiter limiter(seconds(5));
  const RateLimiter::Clock::time_point t0{};
  EXPECT_EQ(limiter.allow("a", t0), 0u);
  EXPECT_FALSE(limiter.allow("a", t0 + seconds(1)).has_value());
  EXPECT_FALSE(limiter.allow("a", t0 + seconds(2)).has_value());
  EXPECT_EQ(limiter.allow("a", t0 + seconds(5)), 2u);
  EXPECT_FALSE(limiter.allow("a", t0 + seconds(5) + milliseconds(1)).has_value());
}

/// @brief Keys are limited independently.
TEST(RateLimiter, KeysAreIndependent) {
  RateLimiter limiter(seconds(5));
  const RateLimiter::Clock::time_point t0{};
  EXPECT_EQ(limiter.allow("a", t0), 0u);
  EXPECT_EQ(limiter.allow("b", t0), 0u);
  EXPECT_FALSE(limiter.allow("a", t0 + seconds(1)).has_value());
}

}  // namespace
}  // namespace astrosim::log
