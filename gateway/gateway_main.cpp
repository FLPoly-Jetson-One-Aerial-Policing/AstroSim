#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "gz_link.hpp"

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
  astrosim::GatewayConfig cfg;
  if (argc > 1) cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));
  if (argc > 2) cfg.idle_exit_s = std::atof(argv[2]);
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });

  astrosim::GzLink gz;
  astrosim::Gateway gw(gz, cfg);
  if (!gw.ok()) { std::fprintf(stderr, "gateway: cannot bind UDP %u\n", cfg.port); return 1; }
  std::fprintf(stderr, "gateway: listening on UDP %u\n", gw.port());
  bool idle = gw.run(g_stop);
  std::fprintf(stderr, idle ? "gateway: idle timeout, exiting\n" : "gateway: stopped\n");
  return 0;
}
