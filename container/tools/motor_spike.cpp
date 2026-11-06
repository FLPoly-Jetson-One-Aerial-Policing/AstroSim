// Motor-out spike. usage: motor_spike direct|override [dead_motor=0] [hover_speed=660]
//  direct   : controller never enabled; drive the 4 motors via /X3/gazebo/command/motor_speed
//  override : controller enabled and hovering at ~2 m, then publish motor-out on the same topic
#include <gz/msgs/actuators.pb.h>
#include <gz/msgs/boolean.pb.h>
#include <gz/msgs/odometry.pb.h>
#include <gz/msgs/twist.pb.h>
#include <gz/transport/Node.hh>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

static std::mutex mu;
static double z, roll, pitch, wx, wy;

static void onOdom(const gz::msgs::Odometry& m) {
  std::lock_guard<std::mutex> l(mu);
  const auto& q = m.pose().orientation();
  z = m.pose().position().z();
  roll = std::atan2(2 * (q.w() * q.x() + q.y() * q.z()), 1 - 2 * (q.x() * q.x() + q.y() * q.y()));
  pitch = std::asin(std::clamp(2 * (q.w() * q.y() - q.z() * q.x()), -1.0, 1.0));
  wx = m.twist().angular().x();
  wy = m.twist().angular().y();
}

static void show(const char* tag) {
  std::lock_guard<std::mutex> l(mu);
  std::printf("  [%s] z=%.3f roll=%.1fdeg pitch=%.1fdeg w=(%.2f,%.2f)\n", tag, z, roll * 57.2958, pitch * 57.2958, wx, wy);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  bool override_mode = argc > 1 && !std::strcmp(argv[1], "override");
  int dead = argc > 2 ? std::atoi(argv[2]) : 0;
  double hover = argc > 3 ? std::atof(argv[3]) : 660;

  gz::transport::Node node;
  node.Subscribe("/model/x3/odometry", onOdom);
  auto motors = node.Advertise<gz::msgs::Actuators>("/X3/gazebo/command/motor_speed");
  auto twist = node.Advertise<gz::msgs::Twist>("/X3/gazebo/command/twist");
  auto enable = node.Advertise<gz::msgs::Boolean>("/X3/enable");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // discovery

  auto sendMotors = [&](double a, double b, double c, double d) {
    gz::msgs::Actuators m;
    for (double v : {a, b, c, d}) m.add_velocity(v);
    motors.Publish(m);
  };
  auto hold = [&](double secs, auto&& fn, const char* tag) {
    auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
      fn();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (++n % 50 == 0) show(tag);
    }
    show(tag);
  };

  if (!override_mode) {
    std::puts("direct: motors only, controller idle");
    show("start");
    hold(2.0, [&] { sendMotors(700, 700, 700, 700); }, "all 700");
    hold(1.0, [&] { sendMotors(hover, hover, hover, hover); }, "all hover");
    double v[4] = {hover, hover, hover, hover}; v[dead] = 0;
    hold(1.5, [&] { sendMotors(v[0], v[1], v[2], v[3]); }, "motor-out");
  } else {
    std::puts("override: controller enabled + hovering, then motor-out on the same topic");
    gz::msgs::Boolean en; en.set_data(true); enable.Publish(en);
    gz::msgs::Twist up; up.mutable_linear()->set_z(1.0);
    hold(2.0, [&] { twist.Publish(up); }, "climb");
    gz::msgs::Twist zero;
    hold(1.0, [&] { twist.Publish(zero); }, "hover");
    double v[4] = {hover, hover, hover, hover}; v[dead] = 0;
    hold(1.5, [&] { twist.Publish(zero); sendMotors(v[0], v[1], v[2], v[3]); }, "motor-out+ctrl");
    std::puts("  now disable controller, keep motor-out");
    gz::msgs::Boolean off; off.set_data(false); enable.Publish(off);
    hold(1.5, [&] { sendMotors(v[0], v[1], v[2], v[3]); }, "motor-out, ctrl off");
  }
  return 0;
}
