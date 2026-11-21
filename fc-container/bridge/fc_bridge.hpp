/**
 * @file fc_bridge.hpp
 * @brief FcBridge: the pure core between the engine protocol, a FlightComputer adapter and avionics displays.
 *
 * No sockets and no clock reads: byte movement goes through net::Transport and time is injected into step().
 *
 * @warning In this version the sensors are SYNTHESIZED from engine truth state (TEMPORARY) and the pilot input is
 * SPOOFED by a script (TEMPORARY). The bridge reports both in its stats line and in FcTelemetry::provenance.
 */
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "astrosim/fc/flight_computer.hpp"
#include "astrosim/fc/pilot_source.hpp"
#include "astrosim/fc/sensor_source.hpp"
#include "astrosim/log/log.hpp"
#include "astrosim/net/transport.hpp"
#include "config.hpp"

namespace astrosim::fcbridge {

using net::Endpoint;                      ///< Endpoint type from the shared transport abstraction.
using Clock = std::chrono::steady_clock;  ///< Monotonic clock used for all bridge timing.
using TimePoint = Clock::time_point;      ///< A point in time on Clock.

/** @brief Monotonic counters describing bridge activity. */
struct BridgeStats {
  std::uint64_t rx_engine = 0;            ///< Datagrams received from the engine endpoint.
  std::uint64_t rx_state = 0;             ///< Well-formed State messages received.
  std::uint64_t engine_malformed = 0;     ///< Malformed datagrams from the engine endpoint.
  std::uint64_t engine_unexpected = 0;    ///< Well-formed messages the engine must not send (Control, Subscribe).
  std::uint64_t engine_foreign = 0;       ///< Datagrams on the engine link from some other endpoint (dropped).
  std::uint64_t sensor_frames = 0;        ///< Sensor frames handed to the FC.
  std::uint64_t sensor_skipped = 0;       ///< States that produced no sensor frame.
  std::uint64_t pilot_samples = 0;        ///< Pilot commands handed to the FC.
  std::uint64_t motor_commands = 0;       ///< MotorCommands taken from the FC.
  std::uint64_t tx_subscribe = 0;         ///< Subscribe keepalives sent to the engine.
  std::uint64_t tx_control = 0;           ///< Control messages sent to the engine.
  std::uint64_t tx_control_failsafe = 0;  ///< Of those, the ones that were forced to zero power.
  std::uint64_t failsafe_entries = 0;     ///< Times the bridge entered failsafe.
  std::uint64_t deadman_seen = 0;         ///< Times the engine reported DEADMAN_TRIPPED (rising edges).
  std::uint64_t rx_telemetry_subscribe = 0;      ///< Well-formed telemetry Subscribe messages.
  std::uint64_t telemetry_malformed = 0;         ///< Malformed datagrams on the telemetry port.
  std::uint64_t telemetry_unexpected = 0;        ///< Well-formed telemetry messages a client must not send.
  std::uint64_t telemetry_subscriber_rejects = 0;///< Subscribers refused because the cap was reached.
  std::uint64_t tx_telemetry = 0;                ///< Telemetry datagrams sent.
};

/** @brief Why the bridge is forcing zero power. */
enum class FailsafeReason : std::uint8_t {
  kNone,          ///< Not in failsafe: the FC's command is passed through.
  kNoCommand,     ///< The FC has not produced any motor command yet.
  kFcSilent,      ///< The FC produced no new command for longer than fc_timeout_s.
  kDisconnected,  ///< The FC reports it is not connected.
  kDisarmed,      ///< The FC's command says disarmed.
};

/**
 * @brief Human-readable name of a failsafe reason.
 * @param r Reason.
 * @return Static string such as "fc_silent".
 */
const char* toString(FailsafeReason r);

/**
 * @brief The flight computer bridge.
 *
 * Engine side: keeps itself subscribed (Subscribe at subscribe_interval_s), feeds each State through the
 * SensorSource into FlightComputer::onSensors, and sends Control at control_rate_hz with an increasing seq. The
 * Control power is the FC's latest command, or ALL ZERO when the FC is disconnected, disarmed, has produced
 * nothing yet or has been silent longer than fc_timeout_s. Control is sent regardless of State, so the engine's
 * deadman never trips while the bridge is alive.
 *
 * Pilot side: at pilot_rate_hz the PilotSource is sampled at the latest engine sim_time (so the script follows the
 * simulation clock) and handed to FlightComputer::onPilot. Nothing is sampled before the first State.
 *
 * Telemetry side: subscribers on the telemetry transport (Subscribe keepalive, subscriber_timeout_s, a cap of
 * max_subscribers, like the engine gateway) receive FlightComputer::telemetry() at telemetry_rate_hz, with the
 * provenance flags overlaid by the bridge and the motors replaced by the power actually sent to the engine.
 *
 * Threading: single-threaded; call step() from the event loop.
 */
class FcBridge {
 public:
  /**
   * @param fc Flight computer adapter; must outlive the bridge.
   * @param sensors Source of sensor frames from engine State; must outlive the bridge.
   * @param pilot Source of pilot input; must outlive the bridge.
   * @param engine_link Transport used to talk to the engine (unbound peer allowed); must outlive the bridge.
   * @param telemetry_link Transport bound to the telemetry port; must outlive the bridge.
   * @param engine Engine endpoint. Datagrams on engine_link from any other endpoint are dropped.
   * @param config Validated configuration (timing and telemetry sections are used).
   */
  FcBridge(fc::FlightComputer& fc, fc::SensorSource& sensors, fc::PilotSource& pilot, net::Transport& engine_link,
           net::Transport& telemetry_link, const Endpoint& engine, const Config& config);

  /**
   * @brief Run one iteration: drain both transports, service the FC, then do everything that is due.
   *
   * Each periodic duty (subscribe, pilot, control, telemetry) runs at most once per call; a late caller skips
   * missed periods instead of bursting. The first call performs every duty immediately.
   * @param now Current time.
   */
  void step(TimePoint now);

  /**
   * @brief Send all-zero Control (three times, UDP being lossy) to the engine (used on shutdown). Also resets the FC.
   * @param now Current time.
   */
  void shutdown(TimePoint now);

  /** @return The earliest time at which step() has something periodic to do (for sleeping). */
  TimePoint nextDeadline() const;

  /** @return Current counters; valid for the bridge's lifetime. */
  const BridgeStats& stats() const { return stats_; }

  /** @return The failsafe reason currently in force (kNone when passing the FC's command through). */
  FailsafeReason failsafe() const { return failsafe_; }

  /** @return The seq of the last Control sent (0 before the first). */
  std::uint32_t controlSeq() const { return control_seq_; }

  /**
   * @brief Summarize counters and session state for logs.
   * @return One line, including `sensors=SYNTHESIZED|MEASURED` and `pilot=SPOOFED|REAL`.
   */
  std::string formatStats() const;

 private:
  void onEngineDatagram(const Endpoint& from, std::span<const std::byte> bytes, TimePoint now);
  void onTelemetryDatagram(const Endpoint& from, std::span<const std::byte> bytes, TimePoint now);
  void sendSubscribe();
  void samplePilot(TimePoint now);
  void sendControl(TimePoint now);
  void broadcastTelemetry(TimePoint now);
  void expireSubscribers(TimePoint now);
  FailsafeReason evaluateFailsafe(TimePoint now) const;
  void warnLimited(const std::string& key, TimePoint now, const std::string& message);

  fc::FlightComputer& fc_;
  fc::SensorSource& sensors_;
  fc::PilotSource& pilot_;
  net::Transport& engine_link_;
  net::Transport& telemetry_link_;
  Endpoint engine_;

  Clock::duration subscribe_period_;
  Clock::duration pilot_period_;
  Clock::duration control_period_;
  Clock::duration telemetry_period_;
  Clock::duration fc_timeout_;
  Clock::duration subscriber_timeout_;
  std::size_t max_subscribers_;

  BridgeStats stats_;
  log::RateLimiter warn_limiter_;

  bool started_ = false;
  TimePoint next_subscribe_{};
  TimePoint next_pilot_{};
  TimePoint next_control_{};
  TimePoint next_telemetry_{};

  std::uint32_t subscribe_seq_ = 0;
  std::uint32_t control_seq_ = 0;

  bool have_state_ = false;          ///< At least one State has arrived.
  double sim_time_ = 0;              ///< sim_time of the latest State.
  std::uint8_t engine_flags_ = 0;    ///< StateMsg::flags of the latest State.
  std::uint32_t echo_seq_ = 0;       ///< StateMsg::echo_seq of the latest State.

  std::optional<fc::MotorCommand> last_cmd_;  ///< Latest command from the FC.
  TimePoint last_cmd_time_{};                 ///< When last_cmd_ arrived.
  FailsafeReason failsafe_ = FailsafeReason::kNoCommand;
  std::array<float, fc::kNumMotors> sent_power_{};  ///< Power in the latest Control sent.

  std::map<Endpoint, TimePoint> subscribers_;  ///< Telemetry subscriber -> last time heard.
  std::uint32_t telemetry_seq_ = 0;
};

}  // namespace astrosim::fcbridge
