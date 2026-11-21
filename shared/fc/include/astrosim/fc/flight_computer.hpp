#pragma once
/**
 * @file flight_computer.hpp
 * @brief The abstract flight computer adapter interface.
 *
 * Real flight computers are separate processes or hardware, so the interface is asynchronous and event driven:
 * the caller pushes inputs, pumps poll(), and collects whatever the FC has produced. Adapters implement this class
 * and nothing in the rest of the system knows which FC is behind it. See FC_INTERFACE.md.
 */
#include <optional>

#include "astrosim/fc/types.hpp"

namespace astrosim::fc {

/**
 * @brief Adapter boundary between the bridge and a concrete flight computer (PX4, ArduPilot, Jetson FC, ...).
 *
 * Threading: all methods are called from one thread (the bridge's event loop). Time is injected through the
 * @p now arguments, which come from a monotonic clock owned by the caller. Methods must not block.
 */
class FlightComputer {
 public:
  virtual ~FlightComputer() = default;

  /**
   * @brief Deliver one sensor sample to the FC.
   * @param frame Sensor sample, body FLU axes.
   * @param now Current monotonic time.
   */
  virtual void onSensors(const SensorFrame& frame, TimePoint now) = 0;

  /**
   * @brief Deliver the current pilot command to the FC.
   * @param input Pilot command (attitude-angle sticks, mode, arm).
   * @param now Current monotonic time.
   */
  virtual void onPilot(const PilotInput& input, TimePoint now) = 0;

  /**
   * @brief Service the FC link: read and write the sockets or hardware, advance timers. Must not block.
   * @param now Current monotonic time.
   */
  virtual void poll(TimePoint now) = 0;

  /**
   * @brief Take the newest motor command produced since the last call.
   * @return The command, or std::nullopt if the FC produced nothing new.
   */
  virtual std::optional<MotorCommand> takeMotors() = 0;

  /**
   * @brief Current avionics telemetry from the FC's own view.
   * @return A snapshot. The bridge overlays provenance flags, so adapters need not set them.
   */
  virtual FcTelemetry telemetry() const = 0;

  /// @brief Return to the power-on state (drop queued commands, link state and estimates).
  virtual void reset() = 0;
};

}  // namespace astrosim::fc
