/**
 * @file log.hpp
 * @brief Minimal leveled stderr logger and a per-key rate limiter for repeated warnings.
 */
#pragma once
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace astrosim::gateway::log {

/** @brief Message severity, ordered from most to least verbose. */
enum class Level {
  Trace,  ///< Very detailed diagnostics.
  Debug,  ///< Developer diagnostics.
  Info,   ///< Normal operational events.
  Warn,   ///< Recoverable problems.
  Error,  ///< Failures.
};

/**
 * @brief Parse a level name.
 * @param name One of trace, debug, info, warn, error.
 * @return The level, or nullopt if the name is not recognized.
 */
std::optional<Level> parseLevel(std::string_view name);

/**
 * @brief Set the minimum level that is emitted. Thread-safe.
 * @param level New minimum level.
 */
void setLevel(Level level);

/**
 * @brief Get the minimum level that is emitted. Thread-safe.
 * @return Current minimum level.
 */
Level level();

/**
 * @brief Write one timestamped line to stderr if the level is enabled.
 * @param lvl Severity of the message.
 * @param message Already formatted text.
 */
void write(Level lvl, const std::string& message);

/**
 * @brief Log a formatted message at Level::Trace.
 * @param fmt std::format format string, checked at compile time.
 * @param args Format arguments.
 */
template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Trace) write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}
/**
 * @brief Log a formatted message at Level::Debug.
 * @param fmt std::format format string, checked at compile time.
 * @param args Format arguments.
 */
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Debug) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
/**
 * @brief Log a formatted message at Level::Info.
 * @param fmt std::format format string, checked at compile time.
 * @param args Format arguments.
 */
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Info) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
/**
 * @brief Log a formatted message at Level::Warn.
 * @param fmt std::format format string, checked at compile time.
 * @param args Format arguments.
 */
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Warn) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
/**
 * @brief Log a formatted message at Level::Error.
 * @param fmt std::format format string, checked at compile time.
 * @param args Format arguments.
 */
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Error) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

/**
 * @brief Allows at most one event per interval for each key and counts what was suppressed.
 *
 * The caller supplies the time, so the limiter is deterministic under test. Not thread-safe.
 */
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;  ///< Clock the caller's time points come from.

  /** @param interval Minimum spacing between allowed events with the same key. */
  explicit RateLimiter(Clock::duration interval) : interval_(interval) {}

  /**
   * @brief Ask whether an event for @p key may be emitted now.
   * @param key Identifies the kind of event.
   * @param now Current time.
   * @return If allowed, the number of events suppressed since the last allowed one; otherwise nullopt.
   */
  std::optional<std::uint64_t> allow(const std::string& key, Clock::time_point now);

 private:
  struct Entry {
    Clock::time_point last;
    std::uint64_t suppressed = 0;
  };
  Clock::duration interval_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace astrosim::gateway::log
