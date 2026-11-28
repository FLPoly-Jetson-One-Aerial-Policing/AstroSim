/** @file fc_bridge.cpp @brief FcBridge implementation. */
#include "fc_bridge.hpp"

#include <algorithm>
#include <format>
#include <type_traits>
#include <variant>

#include "astrosim/proto/fc_telemetry.hpp"
#include "astrosim/proto/wire.hpp"

namespace astrosim::fcbridge {

namespace wire = proto::wire;
namespace fcwire = proto::fc_wire;

namespace {

/** @brief Convert a duration in seconds (from config) to the steady clock's duration type. */
Clock::duration toDuration(double seconds) {
  return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

constexpr auto kWarnInterval = std::chrono::seconds(5);

/** @brief Advance a periodic deadline by one period, skipping missed periods instead of bursting. */
void advance(TimePoint& next, Clock::duration period, TimePoint now) {
  next += period;
  if (next <= now) next = now + period;
}
}  // namespace

const char* toString(FailsafeReason r) {
  switch (r) {
    case FailsafeReason::kNone: return "none";
    case FailsafeReason::kNoCommand: return "no_command";
    case FailsafeReason::kFcSilent: return "fc_silent";
    case FailsafeReason::kDisconnected: return "fc_disconnected";
    case FailsafeReason::kDisarmed: return "disarmed";
  }
  return "unknown";
}

FcBridge::FcBridge(fc::FlightComputer& fc, fc::SensorSource& sensors, fc::PilotSource& pilot,
                   net::Transport& engine_link, net::Transport& telemetry_link, const Endpoint& engine,
                   const Config& config)
    : fc_(fc),
      sensors_(sensors),
      pilot_(pilot),
      engine_link_(engine_link),
      telemetry_link_(telemetry_link),
      engine_(engine),
      subscribe_period_(toDuration(config.timing.subscribe_interval_s)),
      pilot_period_(toDuration(1.0 / config.timing.pilot_rate_hz)),
      control_period_(toDuration(1.0 / config.timing.control_rate_hz)),
      telemetry_period_(toDuration(1.0 / config.timing.telemetry_rate_hz)),
      fc_timeout_(toDuration(config.timing.fc_timeout_s)),
      subscriber_timeout_(toDuration(config.telemetry.subscriber_timeout_s)),
      max_subscribers_(config.telemetry.max_subscribers),
      warn_limiter_(kWarnInterval) {}

void FcBridge::warnLimited(const std::string& key, TimePoint now, const std::string& message) {
  if (const auto suppressed = warn_limiter_.allow(key, now)) {
    if (*suppressed > 0) {
      log::warn("{} ({} similar suppressed)", message, *suppressed);
    } else {
      log::warn("{}", message);
    }
  }
}

void FcBridge::onEngineDatagram(const Endpoint& from, std::span<const std::byte> bytes, TimePoint now) {
  if (from != engine_) {
    ++stats_.engine_foreign;
    warnLimited("foreign", now, std::format("dropping datagram on the engine link from {}", from.toString()));
    return;
  }
  ++stats_.rx_engine;
  const wire::DecodeResult result = wire::decode(bytes);
  if (const auto* err = std::get_if<wire::DecodeError>(&result)) {
    ++stats_.engine_malformed;
    warnLimited(std::format("malformed:{}", wire::toString(*err)), now,
                std::format("dropping malformed datagram from the engine: {}", wire::toString(*err)));
    return;
  }
  const auto* state = std::get_if<proto::StateMsg>(&std::get<wire::Packet>(result).msg);
  if (!state) {
    ++stats_.engine_unexpected;
    warnLimited("unexpected", now, "dropping a non-State message from the engine");
    return;
  }
  ++stats_.rx_state;
  have_state_ = true;
  sim_time_ = state->sim_time;
  if ((state->flags & proto::kDeadmanTripped) && !(engine_flags_ & proto::kDeadmanTripped)) ++stats_.deadman_seen;
  engine_flags_ = state->flags;
  echo_seq_ = state->echo_seq;

  if (const auto frame = sensors_.onState(*state)) {
    ++stats_.sensor_frames;
    fc_.onSensors(*frame, now);
  } else {
    ++stats_.sensor_skipped;
  }
}

void FcBridge::onTelemetryDatagram(const Endpoint& from, std::span<const std::byte> bytes, TimePoint now) {
  const fcwire::DecodeResult result = fcwire::decode(bytes);
  if (const auto* err = std::get_if<fcwire::DecodeError>(&result)) {
    ++stats_.telemetry_malformed;
    warnLimited(
        std::format("tmalformed:{}", fcwire::toString(*err)), now,
        std::format("dropping malformed telemetry-port datagram from {}: {}", from.toString(), fcwire::toString(*err)));
    return;
  }
  const auto& packet = std::get<fcwire::Packet>(result);
  if (!std::holds_alternative<proto::SubscribeMsg>(packet.msg)) {
    ++stats_.telemetry_unexpected;
    warnLimited("tunexpected", now, std::format("dropping telemetry message sent by client {}", from.toString()));
    return;
  }
  ++stats_.rx_telemetry_subscribe;
  const auto it = subscribers_.find(from);
  if (it != subscribers_.end()) {
    it->second = now;
    return;
  }
  if (subscribers_.size() >= max_subscribers_) {
    ++stats_.telemetry_subscriber_rejects;
    warnLimited(
        "subscriber_cap", now,
        std::format("rejecting telemetry subscriber {}: limit of {} reached", from.toString(), max_subscribers_));
    return;
  }
  subscribers_.emplace(from, now);
  log::info("telemetry subscriber {} added ({} total)", from.toString(), subscribers_.size());
}

void FcBridge::sendSubscribe() {
  engine_link_.send(engine_, wire::encode(proto::SubscribeMsg{}, subscribe_seq_++));
  ++stats_.tx_subscribe;
}

void FcBridge::samplePilot(TimePoint now) {
  if (!have_state_) return;
  fc_.onPilot(pilot_.sample(sim_time_), now);
  ++stats_.pilot_samples;
}

FailsafeReason FcBridge::evaluateFailsafe(TimePoint now) const {
  if (!last_cmd_) return FailsafeReason::kNoCommand;
  if (!fc_.telemetry().connected) return FailsafeReason::kDisconnected;
  if (now - last_cmd_time_ > fc_timeout_) return FailsafeReason::kFcSilent;
  if (!last_cmd_->armed) return FailsafeReason::kDisarmed;
  return FailsafeReason::kNone;
}

void FcBridge::sendControl(TimePoint now) {
  const FailsafeReason reason = evaluateFailsafe(now);
  if (reason != failsafe_) {
    // Disarmed and not-yet-commanded are normal states; anything else is a fault.
    const bool normal =
        reason == FailsafeReason::kNone || reason == FailsafeReason::kDisarmed || reason == FailsafeReason::kNoCommand;
    if (reason == FailsafeReason::kNone) {
      log::info("failsafe cleared: passing the flight computer's motor commands to the engine");
    } else if (normal) {
      log::info("sending zero power: {}", toString(reason));
    } else {
      log::warn("failsafe: sending zero power ({})", toString(reason));
    }
    if (reason != FailsafeReason::kNone) ++stats_.failsafe_entries;
    failsafe_ = reason;
  }

  proto::ControlMsg msg;  // all zero
  if (reason == FailsafeReason::kNone)
    msg = proto::sanitize(proto::ControlMsg{last_cmd_->power});
  else
    ++stats_.tx_control_failsafe;
  sent_power_ = msg.power;
  engine_link_.send(engine_, wire::encode(msg, ++control_seq_));
  ++stats_.tx_control;
}

void FcBridge::expireSubscribers(TimePoint now) {
  for (auto it = subscribers_.begin(); it != subscribers_.end();) {
    if (now - it->second > subscriber_timeout_) {
      log::info("telemetry subscriber {} timed out", it->first.toString());
      it = subscribers_.erase(it);
    } else {
      ++it;
    }
  }
}

void FcBridge::broadcastTelemetry(TimePoint now) {
  expireSubscribers(now);
  if (subscribers_.empty()) return;
  fc::FcTelemetry t = fc_.telemetry();
  // The bridge, not the adapter, states where the FC's inputs come from, so no adapter can forget it.
  if (sensors_.isSynthesized()) t.provenance |= proto::kSensorsSynthesized;
  if (pilot_.isSpoofed()) t.provenance |= proto::kPilotSpoofed;
  t.motors = sent_power_;  // what was actually commanded, including the zero-power failsafe
  const std::vector<std::byte> bytes = fcwire::encode(t, telemetry_seq_++);
  for (const auto& [endpoint, last_heard] : subscribers_) {
    (void)last_heard;
    telemetry_link_.send(endpoint, bytes);
    ++stats_.tx_telemetry;
  }
}

void FcBridge::step(TimePoint now) {
  while (auto d = engine_link_.receive()) onEngineDatagram(d->peer, d->bytes, now);
  while (auto d = telemetry_link_.receive()) onTelemetryDatagram(d->peer, d->bytes, now);

  fc_.poll(now);
  while (auto cmd = fc_.takeMotors()) {  // keep the newest
    last_cmd_ = *cmd;
    last_cmd_time_ = now;
    ++stats_.motor_commands;
  }

  if (!started_) {
    started_ = true;
    next_subscribe_ = next_pilot_ = next_control_ = next_telemetry_ = now;
  }
  if (now >= next_subscribe_) {
    sendSubscribe();
    advance(next_subscribe_, subscribe_period_, now);
  }
  if (now >= next_pilot_) {
    samplePilot(now);
    advance(next_pilot_, pilot_period_, now);
  }
  if (now >= next_control_) {
    sendControl(now);
    advance(next_control_, control_period_, now);
  }
  if (now >= next_telemetry_) {
    broadcastTelemetry(now);
    advance(next_telemetry_, telemetry_period_, now);
  }
}

void FcBridge::shutdown(TimePoint) {
  sent_power_ = {};
  for (int i = 0; i < 3; ++i) {  // UDP is lossy; the engine deadman is the backstop
    engine_link_.send(engine_, wire::encode(proto::ControlMsg{}, ++control_seq_));
    ++stats_.tx_control;
  }
  fc_.reset();
}

TimePoint FcBridge::nextDeadline() const {
  return std::min({next_subscribe_, next_pilot_, next_control_, next_telemetry_});
}

std::string FcBridge::formatStats() const {
  return std::format(
      "sensors={} pilot={} rx_state={} malformed={} unexpected={} foreign={} frames={} skipped={} pilot_samples={} "
      "motor_cmds={} tx_subscribe={} tx_control={} control_zeroed={} failsafe={} failsafe_entries={} "
      "control_seq={} echo_seq={} engine_flags={} deadman_seen={} telemetry_subscribers={} tx_telemetry={} "
      "telemetry_malformed={} telemetry_rejects={}",
      sensors_.isSynthesized() ? "SYNTHESIZED" : "MEASURED", pilot_.isSpoofed() ? "SPOOFED" : "REAL", stats_.rx_state,
      stats_.engine_malformed, stats_.engine_unexpected, stats_.engine_foreign, stats_.sensor_frames,
      stats_.sensor_skipped, stats_.pilot_samples, stats_.motor_commands, stats_.tx_subscribe, stats_.tx_control,
      stats_.tx_control_failsafe, toString(failsafe_), stats_.failsafe_entries, control_seq_, echo_seq_, engine_flags_,
      stats_.deadman_seen, subscribers_.size(), stats_.tx_telemetry, stats_.telemetry_malformed,
      stats_.telemetry_subscriber_rejects);
}

}  // namespace astrosim::fcbridge
