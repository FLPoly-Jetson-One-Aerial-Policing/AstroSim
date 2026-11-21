/**
 * @file gz_names.hpp
 * @brief The single source of Gazebo-side names (model, links, joints, topics), shared by the model generator and
 *        the gateway's GzEngine so they cannot drift apart. Header-only and gz-free.
 */
#pragma once
#include <cstddef>
#include <string>

namespace astrosim::gznames {

/** @brief Name of the generated model (also its directory name and the motor plugins' robotNamespace). */
inline constexpr const char* kModelName = "aircraft";
/** @brief Name of the airframe link. */
inline constexpr const char* kBaseLink = "base_link";
/** @brief Sub-topic the motor model subscribes to, relative to the robot namespace. */
inline constexpr const char* kMotorCommandSubTopic = "command/motor_speed";

/**
 * @brief Name of the link carrying rotor @p i.
 * @param i Protocol motor index.
 * @return "rotor_<i>".
 */
inline std::string rotorLinkName(std::size_t i) {
  return "rotor_" + std::to_string(i);
}

/**
 * @brief Name of the revolute joint of rotor @p i (as published in the joint-state message).
 * @param i Protocol motor index.
 * @return "rotor_<i>_joint".
 */
inline std::string rotorJointName(std::size_t i) {
  return "rotor_" + std::to_string(i) + "_joint";
}

/** @brief Topic the motor model listens on (gz::msgs::Actuators): robotNamespace + "/" + sub-topic. */
inline std::string motorCommandTopic() {
  return std::string("/") + kModelName + "/" + kMotorCommandSubTopic;
}

/** @brief Odometry topic (set explicitly in the OdometryPublisher plugin). */
inline std::string odometryTopic() {
  return std::string("/model/") + kModelName + "/odometry";
}

/** @brief Joint-state topic (gz::msgs::Model) carrying the rotor joint velocities. */
inline std::string jointStateTopic() {
  return std::string("/") + kModelName + "/joint_state";
}

}  // namespace astrosim::gznames
