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

#include "log.hpp"

namespace astrosim::gateway {

namespace {

/** @brief Replace every "{actuator}" in @p pattern with @p index. */
std::string expandName(std::string pattern, int index) {
  static const std::string kToken = "{actuator}";
  for (std::size_t pos = pattern.find(kToken); pos != std::string::npos; pos = pattern.find(kToken, pos)) {
    const std::string value = std::to_string(index);
    pattern.replace(pos, kToken.size(), value);
    pos += value.size();
  }
  return pattern;
}

}  // namespace

GzEngine::GzEngine(const Config& config) : config_(config), stale_after_(config.timing.engine_stale_s) {
  int max_index = 0;
  for (const auto& m : config_.motors) max_index = std::max(max_index, m.actuator_index);
  actuator_count_ = static_cast<std::size_t>(max_index) + 1;

  command_pub_ = node_.Advertise<gz::msgs::Actuators>(config_.engine.motor_command_topic);
  if (!command_pub_.Valid()) {
    throw std::runtime_error(std::format("cannot advertise '{}'", config_.engine.motor_command_topic));
  }

  if (!node_.Subscribe(config_.engine.odometry_topic, &GzEngine::onOdometry, this)) {
    throw std::runtime_error(std::format("cannot subscribe to '{}'", config_.engine.odometry_topic));
  }

  if (!config_.engine.motor_speed_topic.empty()) {
    for (std::size_t i = 0; i < config_.motors.size(); ++i) {
      joint_to_motor_.emplace(expandName(config_.engine.motor_joint_name, config_.motors[i].actuator_index), i);
    }
    if (!node_.Subscribe(config_.engine.motor_speed_topic, &GzEngine::onJointState, this)) {
      throw std::runtime_error(std::format("cannot subscribe to '{}'", config_.engine.motor_speed_topic));
    }
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
        static_cast<float>(std::abs(joint.axis1().velocity()) * config_.engine.motor_speed_scale);
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
  for (std::size_t i = 0; i < actuator_count_; ++i) msg.add_velocity(0.0);
  for (std::size_t i = 0; i < power.size(); ++i) {
    msg.set_velocity(config_.motors[i].actuator_index, static_cast<double>(power[i]) * config_.motors[i].max_rad_s);
  }
  std::lock_guard lock(publish_mutex_);
  if (!command_pub_.Publish(msg))
    log::warn("failed to publish motor command on '{}'", config_.engine.motor_command_topic);
}

}  // namespace astrosim::gateway
