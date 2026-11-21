/**
 * @file main.cpp
 * @brief Engine gateway executable: wires config, GzEngine, UdpTransport and Gateway into a poll() event loop.
 */
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "config.hpp"
#include "gateway.hpp"
#include "gz_engine.hpp"
#include "log.hpp"
#include "udp_transport.hpp"

namespace {

using namespace astrosim;
using namespace astrosim::gateway;

constexpr const char* kVersion = "0.1.0";
constexpr const char* kDefaultConfig = "/opt/astrosim/config/gateway.toml";

/// Set by the signal handler; the only global.
std::atomic<bool> g_stop{false};

/** @brief Signal handler: request a clean shutdown. */
void onSignal(int) {
  g_stop.store(true);
}

/** @brief Install onSignal for SIGTERM and SIGINT without SA_RESTART so poll() returns EINTR. */
void installSignalHandlers() {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
}

/** @brief Print usage to @p out. */
void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: gateway [--config PATH] [--help] [--version]\n"
               "  --config PATH  gateway TOML file (default %s)\n",
               kDefaultConfig);
}

/** @brief Log the effective configuration at info level. */
void logConfig(const Config& c) {
  log::info("config: network {}:{} max_subscribers={}", c.network.bind, c.network.port, c.network.max_subscribers);
  log::info("config: timing state_rate_hz={} deadman={}s lease={}s subscriber={}s engine_stale={}s stats={}s",
            c.timing.state_rate_hz, c.timing.deadman_timeout_s, c.timing.lease_timeout_s, c.timing.subscriber_timeout_s,
            c.timing.engine_stale_s, c.timing.stats_interval_s);
  log::info("config: engine model={} odometry={} command={} motor_speed='{}'", c.engine.model, c.engine.odometry_topic,
            c.engine.motor_command_topic, c.engine.motor_speed_topic);
  for (std::size_t i = 0; i < c.motors.size(); ++i) {
    log::info("config: motor[{}] -> actuator {} max {} rad/s", i, c.motors[i].actuator_index, c.motors[i].max_rad_s);
  }
}

/**
 * @brief Run the event loop until a stop is requested.
 * @param transport Bound UDP transport.
 * @param gw Gateway logic.
 * @param cfg Configuration (rates).
 */
void run(UdpTransport& transport, Gateway& gw, const Config& cfg) {
  using SteadyClock = std::chrono::steady_clock;
  const auto period =
      std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / cfg.timing.state_rate_hz));
  const auto stats_period =
      std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(cfg.timing.stats_interval_s));

  auto next_tick = SteadyClock::now() + period;
  auto next_stats = SteadyClock::now() + stats_period;
  pollfd pfd{transport.fd(), POLLIN, 0};

  while (!g_stop.load()) {
    const auto now = SteadyClock::now();
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(next_tick - now);
    const int timeout_ms = static_cast<int>(std::max<std::chrono::milliseconds::rep>(wait.count(), 0));
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::system_category(), "poll");
    }
    if (rc > 0 && (pfd.revents & (POLLERR | POLLNVAL))) {
      throw std::runtime_error("socket error reported by poll()");
    }

    if (rc > 0 && (pfd.revents & POLLIN)) {
      while (auto d = transport.receive()) gw.onDatagram(d->peer, d->bytes, SteadyClock::now());
    }

    const auto t = SteadyClock::now();
    if (t >= next_tick) {
      for (const auto& out : gw.tick(t)) transport.send(out.peer, out.bytes);
      next_tick += period;
      if (next_tick <= t) next_tick = t + period;  // fell behind: skip, do not burst
    }
    if (t >= next_stats) {
      log::info("stats: {}", gw.formatStats());
      next_stats += stats_period;
      if (next_stats <= t) next_stats = t + stats_period;
    }
  }
}

}  // namespace

/**
 * @brief Gateway entry point: load config, start the engine adapter and UDP loop.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on clean shutdown or --help/--version, 2 for bad arguments or configuration, 1 for a runtime failure.
 */
int main(int argc, char** argv) {
  std::string config_path = kDefaultConfig;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    if (a == "--version") {
      std::printf("astrosim gateway %s\n", kVersion);
      return 0;
    }
    if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      std::fprintf(stderr, "gateway: unknown or incomplete argument '%s'\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }

  Config cfg;
  try {
    cfg = loadConfig(config_path, processEnv);
  } catch (const ConfigError& e) {
    log::error("configuration error: {}", e.what());
    return 2;
  }
  log::setLevel(*log::parseLevel(cfg.log.level));
  log::info("astrosim gateway {} starting, config {}", kVersion, config_path);
  logConfig(cfg);

  try {
    installSignalHandlers();
    GzEngine engine(cfg);
    UdpTransport transport(cfg.network.bind, cfg.network.port);
    Gateway gw(engine, cfg);
    log::info("listening on {}:{}", cfg.network.bind, cfg.network.port);

    try {
      run(transport, gw, cfg);
    } catch (...) {
      engine.setMotors({});  // never leave the rotors spinning on a fatal error
      throw;
    }

    log::info("shutting down: zeroing motors");
    engine.setMotors({});
    log::info("final stats: {}", gw.formatStats());
  } catch (const std::exception& e) {
    log::error("fatal: {}", e.what());
    return 1;
  }
  return 0;
}
