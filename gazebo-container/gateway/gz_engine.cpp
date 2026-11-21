/** @file gz_engine.cpp @brief gz-transport implementation of Engine. */
#include "gz_engine.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>

#include <gz/math/Quaternion.hh>
#include <gz/math/Vector3.hh>
#include <gz/msgs/model.pb.h>
#include <gz/msgs/odometry.pb.h>

#include "gz_names.hpp"
#include "log.hpp"

namespace astrosim::gateway {

GzEngine::GzEngine(const Config& config, const MotorBinding& binding)
    : binding_(binding), stale_after_(config.timing.engine_stale_s) {
  const std::string command_topic = gznames::motorCommandTopic();
  command_pub_ = node_.Advertise<gz::msgs::Actuators>(command_topic);
  if (!command_pub_.Valid()) throw std::runtime_error(std::format("cannot advertise '{}'", command_topic));

  const std::string odometry_topic = gznames::odometryTopic();
  if (!node_.Subscribe(odometry_topic, &GzEngine::onOdometry, this)) {
    throw std::runtime_error(std::format("cannot subscribe to '{}'", odometry_topic));
  }

  for (std::size_t i = 0; i < proto::kNumMotors; ++i) joint_to_motor_.emplace(gznames::rotorJointName(i), i);
  const std::string joint_topic = gznames::jointStateTopic();
  if (!node_.Subscribe(joint_topic, &GzEngine::onJointState, this)) {
    throw std::runtime_error(std::format("cannot subscribe to '{}'", joint_topic));
  }
}

void GzEngine::onOdometry(const gz::msgs::Odometry& m) {
  proto::StateMsg s;
  const auto& p = m.pose().position();
  const auto& q = m.pose().orientation();
  const auto& v = m.twist().linear();
  const auto& w = m.twist().angular();
  s.sim_time = static_cast<double>(m.header().stamp().sec()) + static_cast<double>(m.header().stamp().nsec()) * 1e-9;
  s.pos = {p.x(), p.y(), p.z()};
  s.quat = {static_cast<float>(q.x()), static_cast<float>(q.y()), static_cast<float>(q.z()), static_cast<float>(q.w())};
  // OdometryPublisher reports linear velocity in the body frame; the wire carries it in the world frame like pos.
  const gz::math::Vector3d vw = gz::math::Quaterniond(q.w(), q.x(), q.y(), q.z()).RotateVector({v.x(), v.y(), v.z()});
  s.vel = {static_cast<float>(vw.X()), static_cast<float>(vw.Y()), static_cast<float>(vw.Z())};
  s.ang_vel = {static_cast<float>(w.x()), static_cast<float>(w.y()), static_cast<float>(w.z())};

  std::lock_guard lock(mutex_);
  s.motor_speed = state_.motor_speed;  // keep the latest feedback
  state_ = s;
  have_odometry_ = true;
  last_odometry_ = SteadyClock::now();
}

void GzEngine::onJointState(const gz::msgs::Model& msg) {
  std::lock_guard lock(mutex_);
  for (const auto& joint : msg.joint()) {
    const auto it = joint_to_motor_.find(joint.name());
    if (it == joint_to_motor_.end()) continue;
    // Direction of rotation is irrelevant for the protocol; report the magnitude.
    state_.motor_speed[it->second] =
        static_cast<float>(std::abs(joint.axis1().velocity()) * binding_.motor_speed_scale);
  }
}

EngineState GzEngine::snapshot() {
  std::lock_guard lock(mutex_);
  EngineState out;
  out.state = state_;
  out.fresh = have_odometry_ && (SteadyClock::now() - last_odometry_) < stale_after_;
  return out;
}

void GzEngine::setMotors(const std::array<float, proto::kNumMotors>& power) {
  gz::msgs::Actuators msg;
  for (std::size_t i = 0; i < power.size(); ++i)
    msg.add_velocity(static_cast<double>(power[i]) * binding_.max_rad_s[i]);
  std::lock_guard lock(publish_mutex_);
  if (!command_pub_.Publish(msg)) log::warn("failed to publish motor command on '{}'", gznames::motorCommandTopic());
}

}  // namespace astrosim::gateway
