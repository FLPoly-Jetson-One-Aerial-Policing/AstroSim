/** @file gateway.cpp @brief Gateway state machine implementation. */
#include "gateway.hpp"

#include <format>
#include <type_traits>
#include <variant>

namespace astrosim::gateway {

namespace wire = proto::wire;

namespace {

/** @brief Convert a duration in seconds (from config) to the steady clock's duration type. */
Clock::duration toDuration(double seconds) {
  return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

static_assert(static_cast<std::size_t>(wire::DecodeError::BadSize) + 1 == kDecodeErrorKinds,
              "update kDecodeErrorKinds");

constexpr auto kWarnInterval = std::chrono::seconds(5);
}  // namespace

Gateway::Gateway(Engine& engine, const Config& config)
    : engine_(engine),
      max_subscribers_(config.network.max_subscribers),
      deadman_timeout_(toDuration(config.timing.deadman_timeout_s)),
      lease_timeout_(toDuration(config.timing.lease_timeout_s)),
      subscriber_timeout_(toDuration(config.timing.subscriber_timeout_s)),
      warn_limiter_(kWarnInterval) {}

void Gateway::warnLimited(const std::string& key, TimePoint now, const std::string& message) {
  if (const auto suppressed = warn_limiter_.allow(key, now)) {
    if (*suppressed > 0) {
      log::warn("{} ({} similar suppressed)", message, *suppressed);
    } else {
      log::warn("{}", message);
    }
  }
}

void Gateway::onDatagram(const Endpoint& from, std::span<const std::byte> bytes, TimePoint now) {
  ++stats_.rx_datagrams;
  const wire::DecodeResult result = wire::decode(bytes);

  if (const auto* err = std::get_if<wire::DecodeError>(&result)) {
    ++stats_.malformed[static_cast<std::size_t>(*err)];
    warnLimited(std::format("malformed:{}", wire::toString(*err)), now,
                std::format("dropping malformed datagram from {}: {}", from.toString(), wire::toString(*err)));
    return;
  }

  const auto& packet = std::get<wire::Packet>(result);
  std::visit(
      [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, proto::SubscribeMsg>) {
          ++stats_.rx_subscribe;
          touchSubscriber(from, now, false);
        } else if constexpr (std::is_same_v<T, proto::ControlMsg>) {
          ++stats_.rx_control;
          onControl(from, packet.seq, msg, now);
        } else {
          ++stats_.unexpected_type;
          warnLimited("unexpected", now, std::format("dropping State message sent by client {}", from.toString()));
        }
      },
      packet.msg);
}

bool Gateway::leaseHeld(TimePoint now) const {
  return holder_ && now - holder_last_ <= lease_timeout_;
}

bool Gateway::leaseAvailableTo(const Endpoint& from, TimePoint now) const {
  return !leaseHeld(now) || *holder_ == from;
}

void Gateway::onControl(const Endpoint& from, std::uint32_t seq, const proto::ControlMsg& msg, TimePoint now) {
  const bool eligible = leaseAvailableTo(from, now);
  touchSubscriber(from, now, eligible);

  if (!eligible) {
    ++stats_.lease_rejects;
    warnLimited("lease", now,
                std::format("dropping Control from {}: lease held by {}", from.toString(), holder_->toString()));
    return;
  }

  if (!leaseHeld(now) || *holder_ != from) {
    holder_ = from;
    have_seq_ = false;
    log::info("controller {} took the lease", from.toString());
  }
  // Only applied Controls refresh the lease. If a restarted controller reuses the endpoint with a reset seq, its
  // Controls look stale; letting those refresh the lease would lock it out until the seq caught up. Instead the
  // lease expires after lease_timeout and the next Control re-acquires it with a fresh seq history.
  if (have_seq_ && !wire::seqNewer(seq, last_seq_)) {
    ++stats_.stale_seq;
    return;
  }
  holder_last_ = now;
  have_seq_ = true;
  last_seq_ = seq;

  engine_.setMotors(proto::sanitize(msg).power);
  echo_seq_ = seq;
  last_control_ = now;
  controlled_ = true;
  deadman_tripped_ = false;
}

void Gateway::touchSubscriber(const Endpoint& from, TimePoint now, bool exempt_from_cap) {
  const auto it = subscribers_.find(from);
  if (it != subscribers_.end()) {
    it->second = now;
    return;
  }
  if (subscribers_.size() >= max_subscribers_ && !exempt_from_cap) {
    ++stats_.subscriber_rejects;
    warnLimited("subscriber_cap", now,
                std::format("rejecting subscriber {}: limit of {} reached", from.toString(), max_subscribers_));
    return;
  }
  subscribers_.emplace(from, now);
  log::info("subscriber {} added ({} total)", from.toString(), subscribers_.size());
}

std::uint8_t Gateway::currentFlags(const EngineState& snap, TimePoint now) const {
  std::uint8_t flags = 0;
  if (snap.fresh) flags |= proto::kEngineOk;
  if (leaseHeld(now) && controlled_ && !deadman_tripped_ && now - last_control_ <= deadman_timeout_) {
    flags |= proto::kControlActive;
  }
  if (deadman_tripped_) flags |= proto::kDeadmanTripped;
  return flags;
}

std::vector<Datagram> Gateway::tick(TimePoint now) {
  for (auto it = subscribers_.begin(); it != subscribers_.end();) {
    if (now - it->second > subscriber_timeout_) {
      log::info("subscriber {} timed out", it->first.toString());
      it = subscribers_.erase(it);
    } else {
      ++it;
    }
  }

  if (controlled_ && !deadman_tripped_ && now - last_control_ > deadman_timeout_) {
    engine_.setMotors({});
    deadman_tripped_ = true;
    ++stats_.deadman_trips;
    log::warn("deadman tripped: no Control for more than {} ms, motors zeroed",
              std::chrono::duration_cast<std::chrono::milliseconds>(deadman_timeout_).count());
  }

  if (holder_ && !leaseHeld(now)) {
    log::info("controller {} lost the lease (silent)", holder_->toString());
    holder_.reset();
  }

  if (subscribers_.empty()) return {};

  const EngineState snap = engine_.snapshot();
  proto::StateMsg state = snap.state;
  state.flags = currentFlags(snap, now);
  state.echo_seq = echo_seq_;
  const std::vector<std::byte> bytes = wire::encode(state, state_seq_++);

  std::vector<Datagram> out;
  out.reserve(subscribers_.size());
  for (const auto& [endpoint, last_heard] : subscribers_) {
    (void)last_heard;
    out.push_back(Datagram{endpoint, bytes});
  }
  stats_.tx_state += out.size();
  return out;
}

std::string Gateway::formatStats() const {
  std::string malformed;
  std::uint64_t total_malformed = 0;
  for (std::size_t i = 0; i < stats_.malformed.size(); ++i) {
    total_malformed += stats_.malformed[i];
    if (stats_.malformed[i] == 0) continue;
    malformed += std::format(" {}={}", wire::toString(static_cast<wire::DecodeError>(i)), stats_.malformed[i]);
  }
  return std::format(
      "rx={} control={} subscribe={} malformed={}{} unexpected={} stale_seq={} lease_rejects={} subscriber_rejects={} "
      "deadman_trips={} tx_state={} subscribers={} controller={}",
      stats_.rx_datagrams, stats_.rx_control, stats_.rx_subscribe, total_malformed, malformed, stats_.unexpected_type,
      stats_.stale_seq, stats_.lease_rejects, stats_.subscriber_rejects, stats_.deadman_trips, stats_.tx_state,
      subscribers_.size(), holder_ ? holder_->toString() : std::string("none"));
}

}  // namespace astrosim::gateway
