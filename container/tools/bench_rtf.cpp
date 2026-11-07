// RTF + determinism benchmark. Runs in the engine container against a running `gz sim -s`.
// usage: bench_rtf [rtf_seconds=60] [world=multicopter] [model=jetson_one]
#include <gz/msgs/boolean.pb.h>
#include <gz/msgs/odometry.pb.h>
#include <gz/msgs/twist.pb.h>
#include <gz/msgs/world_control.pb.h>
#include <gz/msgs/world_stats.pb.h>
#include <gz/transport/Node.hh>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Pose { double x = 0, y = 0, z = 0; double t = -1; };

static std::mutex mu;
static double g_sim = 0, g_rtf = 0;
static bool g_have_stats = false;
static std::vector<double> g_rtf_samples;
static bool g_collect = false;
static Pose g_odom;

static double secs(const gz::msgs::Time &t) { return t.sec() + t.nsec() * 1e-9; }

static void onStats(const gz::msgs::WorldStatistics &m) {
  std::lock_guard<std::mutex> l(mu);
  g_sim = secs(m.sim_time());
  g_rtf = m.real_time_factor();
  g_have_stats = true;
  if (g_collect) g_rtf_samples.push_back(g_rtf);
}
static void onOdom(const gz::msgs::Odometry &m) {
  std::lock_guard<std::mutex> l(mu);
  g_odom = {m.pose().position().x(), m.pose().position().y(), m.pose().position().z(),
            secs(m.header().stamp())};
}
static double simNow() { std::lock_guard<std::mutex> l(mu); return g_sim; }

static void waitSim(double t) { while (simNow() < t) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

struct Seg { double until, vx, vz, yaw; };

// Sim-time-triggered scripted flight; returns odometry at sim t >= end.
static Pose runScenario(gz::transport::Node &node, gz::transport::Node::Publisher &pub,
                        const std::string &world, const std::string &model) {
  gz::msgs::WorldControl req; gz::msgs::Boolean rep;
  req.mutable_reset()->set_all(true);
  bool ok = false; node.Request("/world/" + world + "/control", req, 3000, rep, ok);
  if (!ok) std::fprintf(stderr, "warning: world reset failed\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  waitSim(0.0);

  gz::msgs::Boolean en; en.set_data(true);
  auto enablePub = node.Advertise<gz::msgs::Boolean>("/" + model + "/enable");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  enablePub.Publish(en);

  const std::vector<Seg> script = {{1, 0, 0, 0}, {3, 0, 1.0, 0}, {6, 1.0, 0, 0}, {8, 0, 0, 0.5}, {10, 0, 0, 0}};
  double t0 = simNow();
  for (const auto &s : script) {
    gz::msgs::Twist tw;
    tw.mutable_linear()->set_x(s.vx); tw.mutable_linear()->set_z(s.vz); tw.mutable_angular()->set_z(s.yaw);
    while (simNow() - t0 < s.until) { pub.Publish(tw); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  }
  for (int i = 0; i < 5000; ++i) {
    { std::lock_guard<std::mutex> l(mu); if (g_odom.t >= t0 + 10.0) break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> l(mu);
  return g_odom;
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const double rtfSecs = argc > 1 ? std::atof(argv[1]) : 60.0;
  const std::string world = argc > 2 ? argv[2] : "multicopter";
  const std::string model = argc > 3 ? argv[3] : "jetson_one";
  std::string lower = model; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  gz::transport::Node node;
  if (!node.Subscribe("/stats", onStats)) { std::fprintf(stderr, "subscribe /stats failed\n"); return 2; }
  if (!node.Subscribe("/model/" + lower + "/odometry", onOdom)) { std::fprintf(stderr, "subscribe odom failed\n"); return 2; }
  auto twistPub = node.Advertise<gz::msgs::Twist>("/" + model + "/gazebo/command/twist");

  for (int i = 0; i < 100 && !g_have_stats; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!g_have_stats) { std::fprintf(stderr, "no /stats: is gz sim running?\n"); return 2; }

  // Phase A: RTF over wall-clock window, idle vehicle.
  double simA, wallA = 0;
  { std::lock_guard<std::mutex> l(mu); simA = g_sim; g_rtf_samples.clear(); g_collect = true; }
  auto w0 = Clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>(rtfSecs));
  double wall = std::chrono::duration<double>(Clock::now() - w0).count();
  double simB;
  std::vector<double> s;
  { std::lock_guard<std::mutex> l(mu); g_collect = false; simB = g_sim; s = g_rtf_samples; }
  (void)wallA;
  std::sort(s.begin(), s.end());
  double mean = 0; for (double v : s) mean += v; if (!s.empty()) mean /= s.size();
  std::printf("RTF over %.1f s wall: overall=%.4f  mean=%.4f  min=%.4f  p5=%.4f  (n=%zu)\n", wall,
              (simB - simA) / wall, mean, s.empty() ? 0 : s.front(), s.empty() ? 0 : s[s.size() / 20], s.size());

  // Phase B: determinism.
  Pose a = runScenario(node, twistPub, world, model);
  Pose b = runScenario(node, twistPub, world, model);
  double d = std::sqrt(std::pow(a.x - b.x, 2) + std::pow(a.y - b.y, 2) + std::pow(a.z - b.z, 2));
  std::printf("run1 final: (%.4f, %.4f, %.4f) t=%.3f\n", a.x, a.y, a.z, a.t);
  std::printf("run2 final: (%.4f, %.4f, %.4f) t=%.3f\n", b.x, b.y, b.z, b.t);
  std::printf("determinism delta: %.4f m  (%s, threshold 0.01 m)\n", d, d <= 0.01 ? "PASS" : "FAIL");
  std::printf("RTF %s (threshold 0.95)\n", (simB - simA) / wall >= 0.95 ? "PASS" : "FAIL");
  return 0;
}
