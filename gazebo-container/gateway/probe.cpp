/**
 * @file probe.cpp
 * @brief astrosim-probe: command-line client for the engine protocol (health check, State viewer, Control sender).
 *
 * Depends only on shared/proto and POSIX sockets; it does not use gz.
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
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "astrosim/proto/wire.hpp"

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
  enum class Mode { None, Health, Subscribe, Control } mode = Mode::None;
  std::string host = "127.0.0.1";
  std::uint16_t port = 8765;
  double timeout_s = 3.0;
  long count = 0;  ///< --subscribe: stop after this many States (0 = unlimited).
  double rate_hz = 100.0;
  double duration_s = 0;  ///< --control: 0 = until interrupted.
  std::array<float, kNumMotors> power{};
};

void usage(std::FILE* out) {
  std::fputs(
      "usage: astrosim-probe [--host H] [--port P] MODE\n"
      "  --health [--timeout S]               prints \"engine healthy\" and exits 0 on a State with\n"
      "                                       ENGINE_OK within S (default 3), else \"engine not responding: ...\", "
      "exit 1\n"
      "  --subscribe [--count N]              print each State as one line\n"
      "  --control p0 ... p7 [--rate HZ] [--duration S]\n"
      "                                       send Control (power 0..1 for 8 motors) at HZ (default 100)\n"
      "defaults: --host 127.0.0.1 --port 8765\n",
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
  const auto setMode = [&](Options::Mode m) {
    if (o.mode != Options::Mode::None) throw UsageError{"only one of --health, --subscribe, --control may be given"};
    o.mode = m;
  };
  const auto value = [&](int& i, const char* flag) -> std::string_view {
    if (i + 1 >= argc) throw UsageError{std::string(flag) + " needs a value"};
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--host")
      o.host = std::string(value(i, "--host"));
    else if (a == "--port") {
      const auto p = parseNumber<int>(value(i, "--port"), "--port");
      if (p < 1 || p > 65535) throw UsageError{"--port must be in 1..65535"};
      o.port = static_cast<std::uint16_t>(p);
    } else if (a == "--health")
      setMode(Options::Mode::Health);
    else if (a == "--subscribe")
      setMode(Options::Mode::Subscribe);
    else if (a == "--control") {
      setMode(Options::Mode::Control);
      if (i + static_cast<int>(kNumMotors) >= argc) throw UsageError{"--control needs 8 power values"};
      for (std::size_t m = 0; m < kNumMotors; ++m) o.power[m] = parseNumber<float>(argv[++i], "motor power");
    } else if (a == "--timeout")
      o.timeout_s = parseNumber<double>(value(i, "--timeout"), "--timeout");
    else if (a == "--count")
      o.count = parseNumber<long>(value(i, "--count"), "--count");
    else if (a == "--rate")
      o.rate_hz = parseNumber<double>(value(i, "--rate"), "--rate");
    else if (a == "--duration")
      o.duration_s = parseNumber<double>(value(i, "--duration"), "--duration");
    else
      throw UsageError{"unknown argument '" + std::string(a) + "'"};
  }
  if (o.mode == Options::Mode::None) throw UsageError{"no mode given (--health, --subscribe or --control)"};
  if (!(o.timeout_s > 0)) throw UsageError{"--timeout must be > 0"};
  if (o.count < 0) throw UsageError{"--count must be >= 0"};
  if (!(o.rate_hz > 0 && o.rate_hz <= 10000)) throw UsageError{"--rate must be in (0, 10000]"};
  if (o.duration_s < 0) throw UsageError{"--duration must be >= 0"};
  return o;
}

/** @brief A connected UDP socket to the engine. */
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

  /** @brief Wait up to @p timeout_ms for a State; other or invalid datagrams are skipped. */
  std::optional<std::pair<std::uint32_t, StateMsg>> receiveState(int timeout_ms) {
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
      const auto r = wire::decode({buf.data(), static_cast<std::size_t>(n)});
      if (const auto* pk = std::get_if<wire::Packet>(&r)) {
        if (const auto* s = std::get_if<StateMsg>(&pk->msg)) return std::make_pair(pk->seq, *s);
      }
    }
  }

 private:
  int fd_ = -1;
};

/** @brief Flags as a "|"-joined list of names, or "-". */
std::string flagNames(std::uint8_t f) {
  std::string s;
  const auto add = [&](std::uint8_t bit, const char* n) {
    if (f & bit) s += (s.empty() ? "" : "|") + std::string(n);
  };
  add(kEngineOk, "ENGINE_OK");
  add(kControlActive, "CONTROL_ACTIVE");
  add(kDeadmanTripped, "DEADMAN_TRIPPED");
  return s.empty() ? "-" : s;
}

/** @brief Print one State on one line. */
void printState(const StateMsg& s) {
  std::printf(
      "t=%.3f pos=(%.3f %.3f %.3f) vel=(%.3f %.3f %.3f) quat=(%.4f %.4f %.4f %.4f) "
      "motor=(%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f) flags=%s echo_seq=%u\n",
      s.sim_time, s.pos[0], s.pos[1], s.pos[2], s.vel[0], s.vel[1], s.vel[2], s.quat[0], s.quat[1], s.quat[2],
      s.quat[3], s.motor_speed[0], s.motor_speed[1], s.motor_speed[2], s.motor_speed[3], s.motor_speed[4],
      s.motor_speed[5], s.motor_speed[6], s.motor_speed[7], flagNames(s.flags).c_str(), s.echo_seq);
  std::fflush(stdout);
}

int runHealth(Link& link, const Options& o) {
  const auto deadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.timeout_s));
  std::uint32_t seq = 0;
  bool got_state = false;
  while (Clock::now() < deadline && !g_stop) {
    link.send(wire::encode(SubscribeMsg{}, seq++));
    const auto slice =
        std::min<long>(250, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
    if (auto st = link.receiveState(static_cast<int>(std::max<long>(slice, 1)))) {
      got_state = true;
      if (st->second.flags & kEngineOk) {
        std::puts("engine healthy");
        return 0;
      }
    }
  }
  if (got_state) {
    std::puts("engine not responding: the engine is up but the simulation is not producing fresh data");
  } else {
    std::printf("engine not responding: no reply from %s:%u within %.1f s\n", o.host.c_str(),
                static_cast<unsigned>(o.port), o.timeout_s);
  }
  return 1;
}

int runSubscribe(Link& link, const Options& o) {
  std::uint32_t seq = 0;
  long received = 0;
  auto next_sub = Clock::now();
  while (!g_stop && (o.count == 0 || received < o.count)) {
    if (Clock::now() >= next_sub) {
      link.send(wire::encode(SubscribeMsg{}, seq++));
      next_sub += std::chrono::seconds(1);
    }
    if (auto st = link.receiveState(100)) {
      printState(st->second);
      ++received;
    }
  }
  if (o.count != 0 && received < o.count) return 1;
  return 0;
}

int runControl(Link& link, const Options& o) {
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / o.rate_hz));
  const auto start = Clock::now();
  const auto end =
      o.duration_s > 0
          ? start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration_s))
          : Clock::time_point::max();
  ControlMsg msg;
  msg.power = o.power;
  std::uint32_t seq = 1;
  auto next = start;
  auto next_print = start + std::chrono::milliseconds(500);
  while (!g_stop && Clock::now() < end) {
    link.send(wire::encode(msg, seq++));
    next += period;
    // Wait for the next send time, printing any State that arrives meanwhile (at most every 0.5 s).
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
      if (left <= 0 || g_stop) break;
      if (auto st = link.receiveState(static_cast<int>(left))) {
        if (Clock::now() >= next_print) {
          printState(st->second);
          next_print += std::chrono::milliseconds(500);
        }
      }
    }
    if (next < Clock::now() - period) next = Clock::now();  // do not burst after a stall
  }
  return 0;
}

}  // namespace

/**
 * @brief astrosim-probe entry point.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on success, 1 on failure (unhealthy engine, no State, network error), 2 for bad arguments.
 */
int main(int argc, char** argv) {
  Options opt;
  try {
    opt = parseArgs(argc, argv);
  } catch (const UsageError& e) {
    std::fprintf(stderr, "astrosim-probe: %s\n", e.message.c_str());
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
    switch (opt.mode) {
      case Options::Mode::Health: return runHealth(link, opt);
      case Options::Mode::Subscribe: return runSubscribe(link, opt);
      case Options::Mode::Control: return runControl(link, opt);
      case Options::Mode::None: break;
    }
  } catch (const std::runtime_error& e) {
    std::fprintf(stderr, "astrosim-probe: %s\n", e.what());
    return 1;
  }
  return 0;
}
