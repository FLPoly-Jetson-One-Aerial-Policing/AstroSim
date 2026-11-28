/**
 * @file px4_adapter.hpp
 * @brief Px4Adapter: the fc::FlightComputer implementation for PX4 SITL. Thin POSIX socket glue around Px4Session.
 *
 * @note POSIX sockets: this class is for the FC container (Linux) only, like the bridge's UDP transport. All
 * protocol logic lives in the portable, unit-tested Px4Session and the codec.
 *
 * Links: a non-blocking TCP server for the HIL link (PX4's simulator_mavlink connects to it; at most one client, a
 * newer connection replaces the older), and one non-blocking UDP socket for the pilot/telemetry link to PX4's
 * onboard MAVLink instance.
 */
#pragma once
#include <memory>
#include <string>

#include "adapter.hpp"
#include "astrosim/fc/flight_computer.hpp"
#include "px4_session.hpp"

namespace astrosim::px4 {

/// @brief Socket endpoints and session settings of the adapter.
struct Px4AdapterConfig {
  std::uint16_t hil_port = 4560;             ///< TCP port to listen on for PX4's HIL connection.
  std::uint16_t mavlink_local_port = 14540;  ///< UDP port to bind for PX4's onboard instance.
  std::string mavlink_host = "127.0.0.1";    ///< IPv4 address of PX4's onboard instance.
  std::uint16_t mavlink_port = 14580;        ///< UDP port PX4's onboard instance listens on.
  Px4SessionConfig session;                  ///< Protocol settings.
};

/**
 * @brief PX4 SITL flight computer adapter.
 *
 * Construction opens the listening sockets and never blocks. poll() accepts the HIL connection, moves bytes between
 * the sockets and the session, and runs the session timers; it must be called at least every few milliseconds.
 */
class Px4Adapter final : public fc::FlightComputer {
 public:
  /**
   * @brief Open the sockets.
   * @param config Endpoints and settings.
   * @throws std::runtime_error if a socket cannot be created or bound (for example the port is in use).
   */
  explicit Px4Adapter(const Px4AdapterConfig& config);
  ~Px4Adapter() override;
  Px4Adapter(const Px4Adapter&) = delete;
  Px4Adapter& operator=(const Px4Adapter&) = delete;

  /// @brief Encode and send the sensor frame to PX4 over the HIL link.
  /// @param frame Sensor frame to send.
  /// @param now Current monotonic time.
  void onSensors(const fc::SensorFrame& frame, fc::TimePoint now) override;
  /// @brief Hand the pilot input to the session.
  /// @param input Pilot input to forward.
  /// @param now Current monotonic time.
  void onPilot(const fc::PilotInput& input, fc::TimePoint now) override;
  /// @brief Accept, read and write the sockets and run the session timers.
  /// @param now Current monotonic time.
  void poll(fc::TimePoint now) override;
  /** @return The newest motor command from PX4, or std::nullopt. */
  std::optional<fc::MotorCommand> takeMotors() override { return session_.takeMotors(); }
  /** @return The telemetry assembled from PX4's MAVLink stream. */
  fc::FcTelemetry telemetry() const override { return session_.telemetry(); }
  /// @brief Close the HIL connection and return to the power-on state.
  void reset() override;

 private:
  /// @brief Accept a pending HIL connection, replacing any existing one.
  void acceptHil(fc::TimePoint now);
  /// @brief Read everything available on the HIL connection.
  void readHil(fc::TimePoint now);
  /// @brief Read every pending datagram on the UDP socket.
  void readMavlink(fc::TimePoint now);
  /// @brief Write the session's output to the sockets.
  void flush();
  /// @brief Close the HIL client connection and tell the session.
  void dropHilClient(const char* why);

  Px4AdapterConfig config_;
  Px4Session session_;
  int listen_fd_ = -1;
  int hil_fd_ = -1;
  int udp_fd_ = -1;
  Bytes hil_pending_;            ///< Unsent HIL bytes (partial writes).
  std::uint32_t peer_ip_ = 0;    ///< PX4's onboard instance IPv4 address, network byte order.
  std::uint16_t peer_port_ = 0;  ///< PX4's onboard instance UDP port, host byte order.
};

}  // namespace astrosim::px4

namespace astrosim::fcbridge {

/**
 * @brief Create the PX4 SITL adapter from the bridge configuration (the seam declared in adapter.hpp).
 * @param config Validated bridge configuration; Config::px4 is used.
 * @return The adapter.
 * @throws std::runtime_error if a socket cannot be opened.
 */
std::unique_ptr<fc::FlightComputer> makePx4Adapter(const Config& config);

}  // namespace astrosim::fcbridge
