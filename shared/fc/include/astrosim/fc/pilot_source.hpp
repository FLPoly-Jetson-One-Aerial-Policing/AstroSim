#pragma once
/**
 * @file pilot_source.hpp
 * @brief Abstract source of pilot input.
 *
 * Today the only implementation is ScriptedPilot, a SPOOFED pilot driven by a scripted timeline. Real pilot input is
 * a separate ticket; an FC-agnostic input message is the eventual target.
 */
#include "astrosim/fc/types.hpp"

namespace astrosim::fc {

/// @brief Produces the pilot command for a given time.
class PilotSource {
 public:
  virtual ~PilotSource() = default;

  /**
   * @brief Pilot command at a time.
   * @param time_s Seconds since the pilot started (the caller's choice of origin, for example sim time).
   * @return The command in force at @p time_s.
   */
  virtual PilotInput sample(double time_s) const = 0;

  /**
   * @brief Whether the input is a scripted spoof rather than a real pilot.
   * @return True for spoofed input. The bridge reports this as FcProvenance::kPilotSpoofed.
   */
  virtual bool isSpoofed() const = 0;
};

}  // namespace astrosim::fc
