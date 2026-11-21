#pragma once
/**
 * @file sensor_source.hpp
 * @brief Abstract source of FC sensor frames, fed by engine State messages.
 *
 * Today the only implementation is TemporarySynthSensors, which SYNTHESIZES sensors from engine truth state. The
 * later sensor/GPS work replaces it with engine-side sensors; the FlightComputer interface does not change.
 */
#include <optional>

#include "astrosim/fc/types.hpp"
#include "astrosim/proto/messages.hpp"

namespace astrosim::fc {

/// @brief Turns engine state into sensor frames for the flight computer.
class SensorSource {
 public:
  virtual ~SensorSource() = default;

  /**
   * @brief Feed one engine state sample.
   * @param state Engine State message (world ENU, body FLU).
   * @return A sensor frame, or std::nullopt if none can be produced yet (for example the first sample of a
   * differentiating source, a duplicate timestamp or a non-finite input).
   */
  virtual std::optional<SensorFrame> onState(const proto::StateMsg& state) = 0;

  /// @brief Forget all history (for example after the engine restarted).
  virtual void reset() = 0;

  /**
   * @brief Whether the frames are synthesized rather than measured by a sensor model.
   * @return True for synthesized sensors. The bridge reports this as FcProvenance::kSensorsSynthesized.
   */
  virtual bool isSynthesized() const = 0;
};

}  // namespace astrosim::fc
