/**
 * @file gateway.hpp
 * @brief Pure protocol logic of the engine gateway: subscribers, controller lease, seq checks, deadman, State.
 *
 * No sockets and no clock reads: callers feed datagrams and the current time in, and send what tick() returns.
 */
#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "astrosim/proto/wire.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "transport.hpp"

namespace astrosim::gateway {

using Clock = std::chrono::steady_clock;  ///< Monotonic clock used for all gateway timing.
using TimePoint = Clock::time_point;      ///< A point in time on Clock.

/** @brief Number of distinct proto::wire::DecodeError values. */
inline constexpr std::size_t kDecodeErrorKinds = 5;

/** @brief Monotonic counters describing gateway activity. */
struct Stats {
  std::uint64_t rx_datagrams = 0;  ///< All datagrams received, valid or not.
  std::uint64_t rx_control = 0;    ///< Well-formed Control messages received (before lease/seq checks).
  std::uint64_t rx_subscribe = 0;  ///< Well-formed Subscribe messages received.
  std::array<std::uint64_t, kDecodeErrorKinds> malformed{};  ///< Rejected datagrams, indexed by DecodeError.
  std::uint64_t unexpected_type = 0;                         ///< Well-formed messages a client must not send (State).
  std::uint64_t stale_seq = 0;                               ///< Control dropped for a stale or duplicate seq.
  std::uint64_t lease_rejects = 0;       ///< Control dropped because another endpoint holds the lease.
  std::uint64_t subscriber_rejects = 0;  ///< New endpoints refused because the subscriber cap was reached.
  std::uint64_t deadman_trips = 0;       ///< Times the deadman zeroed the motors.
  std::uint64_t tx_state = 0;            ///< State datagrams produced.
};

/**
 * @brief The engine gateway state machine.
 *
 * Session rules:
 *  - Any endpoint sending Subscribe or an accepted Control receives State until it is silent for subscriber_timeout_s.
 *  - Only one endpoint (the lease holder) may control; others are dropped until the holder is silent for
 * lease_timeout_s.
 *  - Stale or duplicate Control seqs are dropped; the first Control of a new lease is always accepted.
 *  - If no Control is applied for deadman_timeout_s the motors are zeroed once, until the next Control.
 *
 * Subscriber cap: at max_subscribers, new endpoints are refused. A Control sender that is eligible for the lease
 * (current holder, or lease free/expired) is exempt from the cap, so the controller can never be locked out by
 * display clients; this allows at most one subscriber beyond the cap. The tick rate is driven by the caller.
 */
class Gateway {
 public:
  /**
   * @param engine Engine to command and sample; must outlive the Gateway.
   * @param config Validated configuration (timing and network sections are used).
   */
  Gateway(Engine& engine, const Config& config);

  /**
   * @brief Handle one received datagram.
   * @param from Sender endpoint.
   * @param bytes Raw datagram.
   * @param now Current time.
   */
  void onDatagram(const Endpoint& from, std::span<const std::byte> bytes, TimePoint now);

  /**
   * @brief Advance timers and produce the periodic State broadcast.
   * @param now Current time.
   * @return One State datagram per subscriber (empty if none). All carry the same payload and state seq.
   */
  std::vector<Datagram> tick(TimePoint now);

  /**
   * @brief Current counters.
   * @return Reference valid for the Gateway's lifetime.
   */
  const Stats& stats() const { return stats_; }

  /**
   * @brief Summarize the counters and session state for logs.
   * @return One human-readable line.
   */
  std::string formatStats() const;

 private:
  void onControl(const Endpoint& from, std::uint32_t seq, const proto::ControlMsg& msg, TimePoint now);
  bool leaseHeld(TimePoint now) const;
  bool leaseAvailableTo(const Endpoint& from, TimePoint now) const;
  void touchSubscriber(const Endpoint& from, TimePoint now, bool exempt_from_cap);
  void warnLimited(const std::string& key, TimePoint now, const std::string& message);
  std::uint8_t currentFlags(const EngineState& snap, TimePoint now) const;

  Engine& engine_;
  std::size_t max_subscribers_;
  Clock::duration deadman_timeout_;
  Clock::duration lease_timeout_;
  Clock::duration subscriber_timeout_;

  Stats stats_;
  log::RateLimiter warn_limiter_;
  std::map<Endpoint, TimePoint> subscribers_;  ///< Endpoint -> last time heard.

  std::optional<Endpoint> holder_;  ///< Current lease holder.
  TimePoint holder_last_{};         ///< Last applied Control from the holder (drives lease expiry).
  bool have_seq_ = false;           ///< Whether last_seq_ is valid for the current lease.
  std::uint32_t last_seq_ = 0;      ///< Seq of the last applied Control.
  std::uint32_t echo_seq_ = 0;      ///< Reported as StateMsg::echo_seq.
  bool controlled_ = false;         ///< At least one Control has been applied.
  TimePoint last_control_{};        ///< Time of the last applied Control.
  bool deadman_tripped_ = false;
  std::uint32_t state_seq_ = 0;
};

}  // namespace astrosim::gateway
