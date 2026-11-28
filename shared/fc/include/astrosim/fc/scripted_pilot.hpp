#pragma once
/**
 * @file scripted_pilot.hpp
 * @brief ScriptedPilot: a SPOOFED, TEMPORARY pilot that replays a time-keyed keyframe script.
 *
 * @warning THIS IS NOT A REAL PILOT. The pilot input is spoofed from a script so the flight computer can be
 * exercised end to end before real input exists. The bridge flags it everywhere (startup WARN, the stats line
 * `pilot=SPOOFED`, and FcProvenance::kPilotSpoofed in telemetry).
 * @todo Replace with real pilot input (separate ticket, FC-agnostic input message). This class and its config
 * section are to be removed or demoted to a test fixture.
 */
#include <vector>

#include "astrosim/fc/pilot_source.hpp"

namespace astrosim::fc {

/// @brief One point of the pilot script.
struct Keyframe {
  double time_s = 0;  ///< Time at which this command takes effect, s.
  PilotInput input;   ///< Command from this time on.
};

/// @brief How the continuous axes (roll, pitch, yaw, throttle) behave between keyframes.
enum class Interpolation {
  kHold,    ///< Sample and hold: the value of the latest keyframe.
  kLinear,  ///< Linear ramp from one keyframe to the next.
};

/**
 * @brief SPOOFED pilot (TEMPORARY): replays keyframes from configuration.
 *
 * The discrete fields (mode, arm) always sample-and-hold. Before the first keyframe the command is the default
 * PilotInput (disarmed, zero sticks, GPS hold). After the last keyframe the last command holds.
 *
 * @warning Spoofed input, not a real pilot. See the file header.
 */
class ScriptedPilot final : public PilotSource {
 public:
  /**
   * @brief Build a script.
   * @param keyframes Keyframes with strictly increasing, finite times.
   * @param interpolation Behaviour of the continuous axes between keyframes.
   * @throws std::invalid_argument if times are not finite and strictly increasing, or an axis is out of range
   * (roll, pitch, yaw in [-1, 1]; throttle in [0, 1]) or not finite.
   */
  explicit ScriptedPilot(std::vector<Keyframe> keyframes, Interpolation interpolation = Interpolation::kHold);

  /**
   * @brief Pilot command at a time.
   * @param time_s Seconds since the script started.
   * @return The scripted command at @p time_s.
   */
  PilotInput sample(double time_s) const override;

  /** @return Always true: this input is spoofed. */
  bool isSpoofed() const override { return true; }

 private:
  std::vector<Keyframe> keyframes_;
  Interpolation interpolation_;
};

}  // namespace astrosim::fc
