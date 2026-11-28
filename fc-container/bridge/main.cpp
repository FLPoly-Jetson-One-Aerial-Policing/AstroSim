/*
 * astrosim-fc-bridge: wires config, the flight computer adapter, synthesized sensors, the spoofed pilot and
 * two UDP transports (engine link, telemetry server) into FcBridge and a poll() event loop.
 *
 * Warning: Sensors are SYNTHESIZED from engine truth state and the pilot is SPOOFED by a script. Both are
 * TEMPORARY and announced at startup, in the stats line and in the telemetry provenance flags.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "adapter.hpp"
#include "astrosim/fc/scripted_pilot.hpp"
#include "astrosim/fc/synth_sensors_TEMPORARY.hpp"
#include "astrosim/log/log.hpp"
#include "astrosim/net/udp_transport.hpp"
#include "config.hpp"
#include "fc_bridge.hpp"

namespace {

using namespace astrosim;
using namespace astrosim::fcbridge;

constexpr const char* kVersion = "0.1.0";
constexpr const char* kDefaultConfig = "/opt/astrosim/config/fc.toml";

/// Longest sleep in the event loop. The flight computer adapter has sockets of its own that are not in the poll
/// set; it is serviced (FlightComputer::poll) on every iteration, so the loop must come around at least this often.
constexpr std::chrono::milliseconds kMaxWait{1};

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
               "usage: astrosim-fc-bridge [--config PATH] [--help] [--version]\n"
               "  --config PATH    bridge TOML file (default %s)\n",
               kDefaultConfig);
}

/**
 * @brief Resolve the engine host to an IPv4 endpoint, retrying while the name is not yet known (container start).
 * @param host Host name or dotted quad.
 * @param port UDP port.
 * @return The endpoint, or std::nullopt if a stop was requested before it resolved.
 * @throws std::runtime_error if resolution has not succeeded after about 30 s.
 */
std::optional<net::Endpoint> resolveEngine(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  for (int attempt = 0; attempt < 30 && !g_stop.load(); ++attempt) {
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc == 0 && res) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
      const net::Endpoint ep{ntohl(sin->sin_addr.s_addr), port};
      ::freeaddrinfo(res);
      return ep;
    }
    if (res) ::freeaddrinfo(res);
    log::warn("cannot resolve engine host '{}': {} (retrying)", host, ::gai_strerror(rc));
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (g_stop.load()) return std::nullopt;
  throw std::runtime_error("cannot resolve engine host '" + host + "'");
}

/** @brief Log the effective configuration at info level. */
void logConfig(const Config& c) {
  log::info("config: engine {}:{}", c.engine.host, c.engine.port);
  log::info("config: timing control={} Hz subscribe={}s pilot={} Hz fc_timeout={}s telemetry={} Hz stats={}s",
            c.timing.control_rate_hz, c.timing.subscribe_interval_s, c.timing.pilot_rate_hz, c.timing.fc_timeout_s,
            c.timing.telemetry_rate_hz, c.timing.stats_interval_s);
  log::info("config: telemetry server {}:{} max_subscribers={} subscriber_timeout={}s", c.telemetry.bind,
            c.telemetry.port, c.telemetry.max_subscribers, c.telemetry.subscriber_timeout_s);
  log::info("config: adapter={} sensor_source={} home=({}, {}, {} m)", c.fc.adapter, c.fc.sensor_source,
            c.home.lat_deg, c.home.lon_deg, c.home.alt_m);
  log::info("config: pilot script with {} keyframe(s)", c.pilot.script.size());
}

/**
 * @brief Run the event loop until a stop is requested.
 * @param engine_link Engine transport.
 * @param telemetry_link Telemetry transport.
 * @param bridge Bridge logic.
 * @param cfg Configuration (stats interval).
 */
void run(net::UdpTransport& engine_link, net::UdpTransport& telemetry_link, FcBridge& bridge, const Config& cfg) {
  using SteadyClock = std::chrono::steady_clock;
  const auto stats_period =
      std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(cfg.timing.stats_interval_s));
  auto next_stats = SteadyClock::now() + stats_period;
  pollfd pfds[2] = {{engine_link.fd(), POLLIN, 0}, {telemetry_link.fd(), POLLIN, 0}};

  while (!g_stop.load()) {
    const auto now = SteadyClock::now();
    const auto until = std::min(bridge.nextDeadline() - now, SteadyClock::duration(kMaxWait));
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(until);
    const int timeout_ms = static_cast<int>(std::max<std::chrono::milliseconds::rep>(wait.count(), 0));
    const int rc = ::poll(pfds, 2, timeout_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::system_category(), "poll");
    }
    for (const pollfd& p : pfds) {
      if (rc > 0 && (p.revents & (POLLERR | POLLNVAL))) throw std::runtime_error("socket error reported by poll()");
    }

    const auto t = SteadyClock::now();
    bridge.step(t);
    if (t >= next_stats) {
      log::info("stats: {}", bridge.formatStats());
      next_stats += stats_period;
      if (next_stats <= t) next_stats = t + stats_period;
    }
  }
}

}  // namespace

/**
 * @brief Bridge entry point: load config, build the adapter and event loop.
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
      std::printf("astrosim fc bridge %s\n", kVersion);
      return 0;
    }
    if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      std::fprintf(stderr, "astrosim-fc-bridge: unknown or incomplete argument '%s'\n", argv[i]);
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
  log::info("astrosim fc bridge {} starting, config {}", kVersion, config_path);
  log::warn("SENSORS ARE SYNTHESIZED FROM ENGINE TRUTH STATE (TEMPORARY; not a sensor model)");
  log::warn("PILOT INPUT IS SPOOFED BY A SCRIPT (TEMPORARY; not a real pilot): {} keyframe(s)",
            cfg.pilot.script.size());
  logConfig(cfg);

  std::unique_ptr<fc::FlightComputer> fc;
  std::unique_ptr<fc::TemporarySynthSensors> sensors;
  std::unique_ptr<fc::ScriptedPilot> pilot;
  try {
    sensors = std::make_unique<fc::TemporarySynthSensors>(cfg.synthConfig());
    pilot = std::make_unique<fc::ScriptedPilot>(cfg.pilot.script, cfg.pilot.interpolation);
    if (cfg.fc.adapter == "null") log::warn("adapter \"null\" is for TESTING ONLY: it outputs zero power");
    fc = makeAdapter(cfg);
  } catch (const ConfigError& e) {
    log::error("configuration error: {}", e.what());
    return 2;
  } catch (const std::invalid_argument& e) {
    log::error("configuration error: {}", e.what());
    return 2;
  } catch (const std::exception& e) {
    log::error("fatal: {}", e.what());
    return 1;
  }

  try {
    installSignalHandlers();
    const auto engine_ep = resolveEngine(cfg.engine.host, cfg.engine.port);
    if (!engine_ep) return 0;
    net::UdpTransport engine_link("0.0.0.0", 0);
    net::UdpTransport telemetry_link(cfg.telemetry.bind, cfg.telemetry.port);
    FcBridge bridge(*fc, *sensors, *pilot, engine_link, telemetry_link, *engine_ep, cfg);
    log::info("engine {} ({}), telemetry on {}:{}", engine_ep->toString(), cfg.engine.host, cfg.telemetry.bind,
              cfg.telemetry.port);

    try {
      run(engine_link, telemetry_link, bridge, cfg);
    } catch (...) {
      bridge.shutdown(std::chrono::steady_clock::now());  // never leave the rotors spinning on a fatal error
      throw;
    }

    log::info("shutting down: zeroing motors");
    bridge.shutdown(std::chrono::steady_clock::now());
    log::info("final stats: {}", bridge.formatStats());
  } catch (const std::exception& e) {
    log::error("fatal: {}", e.what());
    return 1;
  }
  return 0;
}
