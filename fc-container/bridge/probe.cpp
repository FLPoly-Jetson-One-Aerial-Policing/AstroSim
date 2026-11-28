/*
 * astrosim-fc-probe: command-line client for the flight computer telemetry protocol (port 8766).
 *
 * Depends only on shared/proto and POSIX sockets; it does not use gz. `--telemetry` subscribes and prints each
 * FcTelemetry as one line, including the provenance flags that mark synthesized sensors and spoofed pilot input.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "astrosim/proto/fc_telemetry.hpp"

namespace {

using namespace astrosim::proto;
using Clock = std::chrono::steady_clock;

/// Set on SIGINT/SIGTERM; the only global.
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) {
  g_stop = 1;
}

/** @brief Raised for bad command-line arguments. */
struct UsageError {
  std::string message;
};

/** @brief Parsed command line. */
struct Options {
  bool telemetry = false;
  std::string host = "127.0.0.1";
  std::uint16_t port = fc_wire::kDefaultPort;
  long count = 0;  ///< Stop after this many telemetry messages (0 = unlimited).
};

void usage(std::FILE* out) {
  std::fputs(
      "usage: astrosim-fc-probe [--host H] [--port P] --telemetry [--count N]\n"
      "  --telemetry [--count N]   subscribe and print each FcTelemetry as one line\n"
      "defaults: --host 127.0.0.1 --port 8766\n",
      out);
}

/** @brief Parse a number with std::from_chars; throws UsageError naming @p what on failure. */
template <class T>
T parseNumber(std::string_view s, const char* what) {
  T v{};
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size())
    throw UsageError{std::string("invalid value for ") + what + ": '" + std::string(s) + "'"};
  return v;
}

Options parseArgs(int argc, char** argv) {
  Options o;
  const auto value = [&](int& i, const char* flag) -> std::string_view {
    if (i + 1 >= argc) throw UsageError{std::string(flag) + " needs a value"};
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--host") {
      o.host = std::string(value(i, "--host"));
    } else if (a == "--port") {
      const auto p = parseNumber<int>(value(i, "--port"), "--port");
      if (p < 1 || p > 65535) throw UsageError{"--port must be in 1..65535"};
      o.port = static_cast<std::uint16_t>(p);
    } else if (a == "--telemetry") {
      o.telemetry = true;
    } else if (a == "--count") {
      o.count = parseNumber<long>(value(i, "--count"), "--count");
    } else {
      throw UsageError{"unknown argument '" + std::string(a) + "'"};
    }
  }
  if (!o.telemetry) throw UsageError{"no mode given (--telemetry)"};
  if (o.count < 0) throw UsageError{"--count must be >= 0"};
  return o;
}

/** @brief A connected UDP socket to the flight computer. */
class Link {
 public:
  /** @brief Resolve @p host (IPv4 literal or name) and connect. @throws std::runtime_error on failure. */
  Link(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res); rc != 0) {
      throw std::runtime_error("cannot resolve '" + host + "': " + ::gai_strerror(rc));
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0 || ::connect(fd_, res->ai_addr, res->ai_addrlen) < 0) {
      const std::string msg = std::string("cannot create socket to ") + host + ": " + std::strerror(errno);
      ::freeaddrinfo(res);
      if (fd_ >= 0) ::close(fd_);
      throw std::runtime_error(msg);
    }
    ::freeaddrinfo(res);
  }
  ~Link() {
    if (fd_ >= 0) ::close(fd_);
  }
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  /** @brief Send a datagram; send errors (e.g. ECONNREFUSED from an earlier ICMP) are ignored. */
  void send(const std::vector<std::byte>& d) { (void)::send(fd_, d.data(), d.size(), 0); }

  /** @brief Wait up to @p timeout_ms for a telemetry message; other or invalid datagrams are skipped. */
  std::optional<FcTelemetryMsg> receiveTelemetry(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left < 0) return std::nullopt;
      pollfd p{fd_, POLLIN, 0};
      const int rc = ::poll(&p, 1, static_cast<int>(left));
      if (rc < 0) {
        if (errno == EINTR && !g_stop) continue;
        return std::nullopt;
      }
      if (rc == 0) return std::nullopt;
      std::array<std::byte, 2048> buf;
      const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
      if (n < 0) continue;  // e.g. ECONNREFUSED; keep waiting until the deadline
      const auto r = fc_wire::decode({buf.data(), static_cast<std::size_t>(n)});
      if (const auto* pk = std::get_if<fc_wire::Packet>(&r)) {
        if (const auto* t = std::get_if<FcTelemetryMsg>(&pk->msg)) return *t;
      }
    }
  }

 private:
  int fd_ = -1;
};

/** @brief Warning bits as a "|"-joined list of names, or "-". */
std::string warningNames(std::uint16_t w) {
  std::string s;
  const auto add = [&](std::uint16_t bit, const char* n) {
    if (w & bit) s += (s.empty() ? "" : "|") + std::string(n);
  };
  add(kWarnGpsLost, "GPS_LOST");
  add(kWarnEkfUnhealthy, "EKF_UNHEALTHY");
  add(kWarnPilotLost, "PILOT_LOST");
  add(kWarnLowBattery, "LOW_BATTERY");
  add(kWarnMotorFailure, "MOTOR_FAILURE");
  return s.empty() ? "-" : s;
}

/** @brief Provenance bits as a "|"-joined list of names, or "-". */
std::string provenanceNames(std::uint8_t p) {
  std::string s;
  const auto add = [&](std::uint8_t bit, const char* n) {
    if (p & bit) s += (s.empty() ? "" : "|") + std::string(n);
  };
  add(kSensorsSynthesized, "SENSORS_SYNTHESIZED");
  add(kPilotSpoofed, "PILOT_SPOOFED");
  return s.empty() ? "-" : s;
}

/** @brief Print one telemetry message on one line. */
void printTelemetry(const FcTelemetryMsg& t) {
  constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
  std::printf(
      "t=%.3f %s mode=%s%s %s gps=fix%u/%usat att(roll pitch hdg)=(%.1f %.1f %.1f)deg alt msl=%.2f agl=%.2f "
      "vspeed=%.2f gspeed=%.2f course=%.1fdeg batt=%.0f%% %.1fV warn=%s prov=%s\n",
      t.time_s, t.connected ? "connected" : "DISCONNECTED", t.mode == FlightMode::kGpsHold ? "GPS_HOLD" : "NO_GPS_HOLD",
      t.mode_refused ? "(REFUSED)" : "", t.armed ? "ARMED" : "disarmed", static_cast<unsigned>(t.gps_fix),
      static_cast<unsigned>(t.gps_sats), t.roll_rad * kRadToDeg, t.pitch_rad * kRadToDeg, t.heading_rad * kRadToDeg,
      static_cast<double>(t.alt_msl_m), static_cast<double>(t.alt_agl_m), static_cast<double>(t.vspeed_mps),
      static_cast<double>(t.ground_speed_mps), t.course_rad * kRadToDeg, static_cast<double>(t.battery_pct),
      static_cast<double>(t.battery_v), warningNames(t.warnings).c_str(), provenanceNames(t.provenance).c_str());
  std::fflush(stdout);
}

int runTelemetry(Link& link, const Options& o) {
  std::uint32_t seq = 0;
  long received = 0;
  auto next_sub = Clock::now();
  while (!g_stop && (o.count == 0 || received < o.count)) {
    if (Clock::now() >= next_sub) {
      link.send(fc_wire::encode(SubscribeMsg{}, seq++));
      next_sub += std::chrono::seconds(1);
    }
    if (auto t = link.receiveTelemetry(100)) {
      printTelemetry(*t);
      ++received;
    }
  }
  if (o.count != 0 && received < o.count) return 1;
  return 0;
}

}  // namespace

/**
 * @brief astrosim-fc-probe entry point.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on success, 1 on failure (no telemetry, network error), 2 for bad arguments.
 */
int main(int argc, char** argv) {
  Options opt;
  try {
    opt = parseArgs(argc, argv);
  } catch (const UsageError& e) {
    std::fprintf(stderr, "astrosim-fc-probe: %s\n", e.message.c_str());
    usage(stderr);
    return 2;
  }
  struct sigaction sa{};
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  try {
    Link link(opt.host, opt.port);
    return runTelemetry(link, opt);
  } catch (const std::runtime_error& e) {
    std::fprintf(stderr, "astrosim-fc-probe: %s\n", e.what());
    return 1;
  }
}
