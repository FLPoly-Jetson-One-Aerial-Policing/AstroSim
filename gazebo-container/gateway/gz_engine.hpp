#pragma once
/**
 * @file gz_engine.hpp
 * @brief Engine implementation backed by gz-transport (the only place, with main.cpp, that includes gz).
 */
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <gz/msgs/actuators.pb.h>
#include <gz/msgs/model.pb.h>
#include <gz/msgs/odometry.pb.h>
#include <gz/transport/Node.hh>

#include "config.hpp"
#include "engine.hpp"

namespace astrosim::gateway {

/**
 * @brief Gazebo engine adapter.
 *
 * Subscribes to the model odometry (gz::msgs::Odometry) and, optionally, to a joint-state topic (gz::msgs::Model) for
 * rotor speeds (the MulticopterMotorModel does not publish speeds itself; its joints are read instead). Commands are
 * published as gz::msgs::Actuators. Callbacks run on gz-transport threads; all shared state is guarded by a mutex.
 */
class GzEngine : public Engine {
 public:
  /**
   * @brief Advertise the command topic and subscribe to the state topics.
   * @param config Validated configuration (engine and motor sections are used).
   * @throws std::runtime_error if the advertisement or any subscription fails.
   */
  explicit GzEngine(const Config& config);

  /** @brief Unsubscribes; no callback runs after destruction. */
  ~GzEngine() override = default;

  GzEngine(const GzEngine&) = delete;
  GzEngine& operator=(const GzEngine&) = delete;

  /** @copydoc Engine::snapshot */
  EngineState snapshot() override;

  /**
   * @copydoc Engine::setMotors
   * Publishes an Actuators message sized to the largest actuator index + 1; velocity[actuator_index] = power *
   * max_rad_s.
   */
  void setMotors(const std::array<float, proto::kNumMotors>& power) override;

 private:
  using SteadyClock = std::chrono::steady_clock;

  /** @brief Odometry callback: updates pose, world-frame velocity and the freshness timestamp. */
  void onOdometry(const gz::msgs::Odometry& msg);
  /** @brief Joint-state callback: updates rotor speeds from the configured joints. */
  void onJointState(const gz::msgs::Model& msg);

  Config config_;
  std::chrono::duration<double> stale_after_;
  std::size_t actuator_count_ = 0;

  std::map<std::string, std::size_t> joint_to_motor_;  ///< Joint name -> protocol motor index.

  std::mutex mutex_;  ///< Guards state_ and last_odometry_.
  proto::StateMsg state_;
  bool have_odometry_ = false;
  SteadyClock::time_point last_odometry_{};

  std::mutex publish_mutex_;  ///< Serializes publishing from setMotors.
  gz::transport::Node node_;  ///< Declared last so it is destroyed first (stops callbacks).
  gz::transport::Node::Publisher command_pub_;
};

}  // namespace astrosim::gateway
