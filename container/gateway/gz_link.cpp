#include "gz_link.hpp"

#include <gz/msgs/boolean.pb.h>
#include <gz/msgs/twist.pb.h>

#include <gz/math/Quaternion.hh>

#include <algorithm>
#include <chrono>

namespace astrosim {
namespace {
double wallNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

GzLink::GzLink(const std::string& model) {
  std::string lower = model;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  twist_pub_ = node_.Advertise<gz::msgs::Twist>("/" + model + "/gazebo/command/twist");
  enable_pub_ = node_.Advertise<gz::msgs::Boolean>("/" + model + "/enable");
  node_.Subscribe("/model/" + lower + "/odometry", &GzLink::onOdom, this);
}

void GzLink::onOdom(const gz::msgs::Odometry& m) {
  StateMsg s;
  s.sim_time = m.header().stamp().sec() + m.header().stamp().nsec() * 1e-9;
  const auto& p = m.pose().position();
  const auto& q = m.pose().orientation();
  const auto& v = m.twist().linear();
  const auto& w = m.twist().angular();
  s.pos[0] = p.x(); s.pos[1] = p.y(); s.pos[2] = p.z();
  s.quat[0] = q.x(); s.quat[1] = q.y(); s.quat[2] = q.z(); s.quat[3] = q.w();
  // OdometryPublisher reports linear velocity in the body frame; the wire carries it in the world frame like pos.
  const gz::math::Vector3d vw = gz::math::Quaterniond(q.w(), q.x(), q.y(), q.z()).RotateVector({v.x(), v.y(), v.z()});
  s.vel[0] = vw.X(); s.vel[1] = vw.Y(); s.vel[2] = vw.Z();
  s.ang_vel[0] = w.x(); s.ang_vel[1] = w.y(); s.ang_vel[2] = w.z();
  VelCmd cmd;
  {
    std::lock_guard<std::mutex> l(mu_);
    state_ = s;
    cmd = world_cmd_;
  }
  last_pose_wall_ = wallNow();
  publish(cmd, s.quat);
}

bool GzLink::connected() { return last_pose_wall_ > 0 && wallNow() - last_pose_wall_ < 0.5; }

StateMsg GzLink::state() {
  std::lock_guard<std::mutex> l(mu_);
  return state_;
}

// The velocity controller takes body-frame commands and rotates them by its current attitude, so the world-frame
// command must be converted with an attitude as fresh as possible: an old one at speed reads as a large velocity
// error (28 m/s x 0.03 rad ~ 1 m/s) while pitching. Converting here and again on every odometry update keeps
// the attitude age to one odometry period instead of a whole control interval.
void GzLink::publish(const VelCmd& world, const float quat[4]) {
  const VelCmd c = worldToBody(world, quat);
  gz::msgs::Twist t;
  t.mutable_linear()->set_x(c.vx);
  t.mutable_linear()->set_y(c.vy);
  t.mutable_linear()->set_z(c.vz);
  t.mutable_angular()->set_z(c.yaw_rate);
  twist_pub_.Publish(t);
}

void GzLink::command(const VelCmd& c) {
  StateMsg s;
  {
    std::lock_guard<std::mutex> l(mu_);
    world_cmd_ = c;
    s = state_;
  }
  publish(c, s.quat);
}

void GzLink::enable() {
  gz::msgs::Boolean b;
  b.set_data(true);
  enable_pub_.Publish(b);
}

}  // namespace astrosim
