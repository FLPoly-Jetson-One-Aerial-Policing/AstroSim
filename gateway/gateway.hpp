#pragma once
#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <utility>

#include "mixer.hpp"
#include "wire.hpp"

namespace astrosim {

// The engine side as the gateway sees it. GzLink implements this against gz-transport.
class GzSide {
 public:
  virtual ~GzSide() = default;
  virtual bool connected() = 0;   // fresh pose data is arriving
  virtual StateMsg state() = 0;   // latest pose/velocity; flags/echo_seq are filled by the gateway
  virtual void command(const VelCmd&) = 0;
  virtual void enable() = 0;      // (re)enable the velocity controller; called ~1 Hz
};

struct GatewayConfig {
  uint16_t port = 8765;
  double tick_hz = 60;
  double deadman_s = 0.5;
  double client_timeout_s = 5;
  double idle_exit_s = 600;  // run() returns when no client has been seen for this long
};

class Gateway {
 public:
  Gateway(GzSide& gz, const GatewayConfig& cfg);
  ~Gateway();
  bool ok() const { return fd_ >= 0; }
  uint16_t port() const { return port_; }
  // Runs until stop is set or the idle timeout expires. Returns true on idle exit.
  bool run(const std::atomic<bool>& stop);

 private:
  struct Client {
    sockaddr_in addr{};
    double last_seen = 0;
    uint32_t last_seq = 0;
    bool has_seq = false;
    uint32_t echo_seq = 0;
  };
  using Key = std::pair<uint32_t, uint16_t>;

  void drain(double now);
  void onControl(Client& c, uint32_t seq, const ControlMsg& m, double now);
  void tick(double now);

  GzSide& gz_;
  GatewayConfig cfg_;
  int fd_ = -1;
  uint16_t port_ = 0;
  std::map<Key, Client> clients_;
  double last_control_ = 0;
  bool neutral_sent_ = true;
  uint32_t state_seq_ = 0;
};

}  // namespace astrosim
