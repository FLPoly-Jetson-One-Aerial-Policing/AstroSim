#pragma once
// Host-side client link to the gateway: sends Control, receives State on a background thread.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "messages.hpp"

namespace astrosim {

class UdpLink {
 public:
  UdpLink(const std::string& ipv4, uint16_t port);
  ~UdpLink();
  bool ok() const { return fd_ >= 0; }
  void send(const ControlMsg& c);
  StateMsg latest();
  // True while States are arriving (< timeout_s old) and the gateway reports the engine alive.
  bool connected(double timeout_s = 0.5);
  bool gatewayAlive(double timeout_s = 0.5);  // States arriving, regardless of the engine flag

 private:
  void rx();
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> seq_{0};
  std::atomic<double> last_rx_{0};
  std::mutex mu_;
  StateMsg state_;
  std::thread th_;
};

}  // namespace astrosim
