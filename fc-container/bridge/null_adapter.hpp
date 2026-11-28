/**
 * @file null_adapter.hpp
 * @brief NullAdapter: a trivial FlightComputer for TESTING ONLY. It outputs zero power and flies nothing.
 *
 * @warning TEST / SMOKE-TEST USE ONLY (config `adapter = "null"`). It does not stabilize or fly the aircraft. It
 * exists so the bridge, the engine link and the telemetry stream can be exercised without a real flight computer.
 */
#pragma once
#include <optional>

#include "astrosim/fc/flight_computer.hpp"

namespace astrosim::fcbridge {

/**
 * @brief Test-only flight computer: always connected, mirrors the pilot's arm and mode, commands zero power.
 *
 * Telemetry reports the mirrored mode and armed state, and the altitude of the latest sensor frame, so a display
 * can be exercised against it.
 */
class NullAdapter final : public fc::FlightComputer {
 public:
  /**
   * @brief Record the barometric altitude for telemetry.
   * @param frame Latest sensor frame.
   */
  void onSensors(const fc::SensorFrame& frame, fc::TimePoint) override {
    telemetry_.time_s = frame.time_s;
    telemetry_.gps_fix = frame.gps.fix;
    telemetry_.gps_sats = frame.gps.satellites;
    telemetry_.alt_msl_m = static_cast<float>(frame.baro_altitude_m);
    have_motors_ = true;
  }
  /**
   * @brief Mirror the pilot's mode and arm request.
   * @param input Latest pilot input.
   */
  void onPilot(const fc::PilotInput& input, fc::TimePoint) override {
    telemetry_.armed = input.arm;
    telemetry_.mode = input.mode;
    armed_ = input.arm;
  }
  /** @brief Nothing to service. */
  void poll(fc::TimePoint) override {}
  /** @return An all-zero (armed as requested) command after each sensor frame; nullopt otherwise. */
  std::optional<fc::MotorCommand> takeMotors() override {
    if (!have_motors_) return std::nullopt;
    have_motors_ = false;
    fc::MotorCommand c;
    c.armed = armed_;
    return c;
  }
  /** @return Telemetry with `connected` true. */
  fc::FcTelemetry telemetry() const override {
    fc::FcTelemetry t = telemetry_;
    t.connected = true;
    return t;
  }
  /** @brief Return to the power-on state. */
  void reset() override {
    telemetry_ = {};
    armed_ = false;
    have_motors_ = false;
  }

 private:
  fc::FcTelemetry telemetry_;
  bool armed_ = false;
  bool have_motors_ = false;
};

}  // namespace astrosim::fcbridge
