/** @file log.cpp @brief Logger implementation. */
#include "astrosim/log/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace astrosim::log {

namespace {
std::atomic<Level> g_level{Level::Info};

/** @brief Fixed-width tag for a level. */
const char* tag(Level l) {
  switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
  }
  return "?????";
}
}  // namespace

std::optional<Level> parseLevel(std::string_view name) {
  if (name == "trace") return Level::Trace;
  if (name == "debug") return Level::Debug;
  if (name == "info") return Level::Info;
  if (name == "warn") return Level::Warn;
  if (name == "error") return Level::Error;
  return std::nullopt;
}

void setLevel(Level l) {
  g_level.store(l);
}
Level level() {
  return g_level.load();
}

void write(Level lvl, const std::string& message) {
  if (lvl < level()) return;
  const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  const std::string line = std::format("{:%F %T} {} {}\n", now, tag(lvl), message);
  std::fwrite(line.data(), 1, line.size(), stderr);
}

std::optional<std::uint64_t> RateLimiter::allow(const std::string& key, Clock::time_point now) {
  auto [it, inserted] = entries_.try_emplace(key, Entry{now, 0});
  if (inserted) return 0;
  Entry& e = it->second;
  if (now - e.last >= interval_) {
    const std::uint64_t suppressed = e.suppressed;
    e.last = now;
    e.suppressed = 0;
    return suppressed;
  }
  ++e.suppressed;
  return std::nullopt;
}

}  // namespace astrosim::log
