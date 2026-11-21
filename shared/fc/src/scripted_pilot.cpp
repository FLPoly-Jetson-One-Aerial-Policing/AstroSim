/// @file
/// @brief Implementation of ScriptedPilot (SPOOFED, TEMPORARY pilot input).
#include "astrosim/fc/scripted_pilot.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace astrosim::fc {

namespace {

/// @brief Throws unless @p v is finite and within [lo, hi].
/// @param what Field description for the message.
/// @param v Value to check.
/// @param lo Lower bound.
/// @param hi Upper bound.
void requireRange(const std::string& what, float v, float lo, float hi) {
  if (!std::isfinite(v) || v < lo || v > hi) {
    throw std::invalid_argument(what + " must be finite and in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
  }
}

/// @brief Linear interpolation.
/// @param a Value at fraction 0.
/// @param b Value at fraction 1.
/// @param f Fraction in [0, 1].
/// @return The interpolated value.
float lerp(float a, float b, double f) {
  return static_cast<float>(a + (b - a) * f);
}

}  // namespace

ScriptedPilot::ScriptedPilot(std::vector<Keyframe> keyframes, Interpolation interpolation)
    : keyframes_(std::move(keyframes)), interpolation_(interpolation) {
  double previous = 0;
  for (std::size_t i = 0; i < keyframes_.size(); ++i) {
    const Keyframe& k = keyframes_[i];
    const std::string where = "keyframe[" + std::to_string(i) + "]";
    if (!std::isfinite(k.time_s)) throw std::invalid_argument(where + ".time_s must be finite");
    if (i > 0 && !(k.time_s > previous)) {
      throw std::invalid_argument(where + ".time_s must be greater than the previous keyframe's");
    }
    previous = k.time_s;
    requireRange(where + ".roll", k.input.roll, -1.f, 1.f);
    requireRange(where + ".pitch", k.input.pitch, -1.f, 1.f);
    requireRange(where + ".yaw", k.input.yaw, -1.f, 1.f);
    requireRange(where + ".throttle", k.input.throttle, 0.f, 1.f);
  }
}

PilotInput ScriptedPilot::sample(double time_s) const {
  // Index of the first keyframe strictly after time_s.
  const auto next = std::upper_bound(keyframes_.begin(), keyframes_.end(), time_s,
                                     [](double t, const Keyframe& k) { return t < k.time_s; });
  if (next == keyframes_.begin()) return PilotInput{};
  const Keyframe& current = *(next - 1);
  PilotInput out = current.input;  // mode and arm always hold
  if (interpolation_ == Interpolation::kLinear && next != keyframes_.end()) {
    const double f = (time_s - current.time_s) / (next->time_s - current.time_s);
    out.roll = lerp(current.input.roll, next->input.roll, f);
    out.pitch = lerp(current.input.pitch, next->input.pitch, f);
    out.yaw = lerp(current.input.yaw, next->input.yaw, f);
    out.throttle = lerp(current.input.throttle, next->input.throttle, f);
  }
  return out;
}

}  // namespace astrosim::fc
