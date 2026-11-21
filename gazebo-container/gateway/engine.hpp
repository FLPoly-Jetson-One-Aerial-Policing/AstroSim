#pragma once
/**
 * @file engine.hpp
 * @brief The physics engine as the gateway sees it.
 *
 * GzEngine (gz_engine.hpp) implements this against gz-transport; tests use a fake. Must stay free of gz headers.
 */
#include <array>

#include "astrosim/proto/messages.hpp"

namespace astrosim::gateway {

/// @brief Snapshot of the engine's latest known state.
struct EngineState {
  bool fresh = false;     ///< Odometry was received within Config::Timing::engine_stale_s.
  proto::StateMsg state;  ///< Kinematics and motor speeds. `flags` and `echo_seq` are filled in by the Gateway.
};

/// @brief Abstract physics engine interface.
class Engine {
 public:
  virtual ~Engine() = default;

  /**
   * @brief Get the latest engine state.
   * @return A consistent snapshot. Must be thread-safe against engine callbacks.
   */
  virtual EngineState snapshot() = 0;

  /**
   * @brief Command the motors.
   * @param power Sanitized normalized power per protocol motor, each in [0,1].
   */
  virtual void setMotors(const std::array<float, proto::kNumMotors>& power) = 0;
};

}  // namespace astrosim::gateway
