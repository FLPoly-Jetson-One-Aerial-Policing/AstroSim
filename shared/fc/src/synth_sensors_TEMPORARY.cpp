/// @file
/// @brief TemporarySynthSensors implementation.
/// @warning SYNTHESIZED from engine truth state and TEMPORARY; see synth_sensors_TEMPORARY.hpp.
#include "astrosim/fc/synth_sensors_TEMPORARY.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "astrosim/fc/frames.hpp"

namespace astrosim::fc {

namespace {

constexpr double kGravity = 9.80665;              ///< Standard gravity, m/s^2.
constexpr double kSeaLevelPressurePa = 101325.0;  ///< ISA sea-level pressure, Pa.
constexpr double kSeaLevelTempK = 288.15;         ///< ISA sea-level temperature, K.
constexpr double kLapseRate = 0.0065;             ///< ISA troposphere lapse rate, K/m.
constexpr double kPressureExponent = 5.255876;    ///< g*M/(R*L) for the ISA troposphere.
constexpr double kWgs84A = 6378137.0;             ///< WGS84 semi-major axis, m.
constexpr double kWgs84E2 = 6.69437999014e-3;     ///< WGS84 first eccentricity squared.
constexpr double kDegPerRad = 180.0 / std::numbers::pi;

/// @brief Whether every element is finite.
/// @param v Array to test.
template <std::size_t N, typename T>
bool allFinite(const std::array<T, N>& v) {
  return std::all_of(v.begin(), v.end(), [](T x) { return std::isfinite(x); });
}

}  // namespace

TemporarySynthSensors::TemporarySynthSensors(const SynthConfig& config) : config_(config) {
  if (!(std::abs(config_.home_lat_deg) <= 90)) throw std::invalid_argument("home_lat_deg must be in [-90, 90]");
  if (!(std::abs(config_.home_lon_deg) <= 180)) throw std::invalid_argument("home_lon_deg must be in [-180, 180]");
  if (!std::isfinite(config_.home_alt_m)) throw std::invalid_argument("home_alt_m must be finite");
  if (!allFinite(config_.mag_field_enu_gauss)) throw std::invalid_argument("mag_field_enu_gauss must be finite");
  if (!(config_.accel_filter_tau_s >= 0) || !std::isfinite(config_.accel_filter_tau_s)) {
    throw std::invalid_argument("accel_filter_tau_s must be finite and >= 0");
  }
}

void TemporarySynthSensors::reset() {
  have_previous_ = false;
  have_accel_ = false;
  previous_time_s_ = 0;
  previous_vel_enu_ = {};
  accel_enu_ = {};
}

std::optional<SensorFrame> TemporarySynthSensors::onState(const proto::StateMsg& s) {
  const Vec3 vel{s.vel[0], s.vel[1], s.vel[2]};
  const Vec3 ang{s.ang_vel[0], s.ang_vel[1], s.ang_vel[2]};
  if (!std::isfinite(s.sim_time) || !allFinite(s.pos) || !allFinite(s.quat) || !allFinite(s.vel) ||
      !allFinite(s.ang_vel)) {
    return std::nullopt;
  }

  if (have_previous_ && s.sim_time < previous_time_s_) reset();  // engine restarted
  if (!have_previous_) {
    have_previous_ = true;
    previous_time_s_ = s.sim_time;
    previous_vel_enu_ = vel;
    return std::nullopt;
  }
  const double dt = s.sim_time - previous_time_s_;
  if (!(dt > 0)) return std::nullopt;  // duplicate timestamp

  Vec3 raw{};
  for (int i = 0; i < 3; ++i) raw[i] = (vel[i] - previous_vel_enu_[i]) / dt;
  if (!have_accel_) {
    accel_enu_ = raw;
    have_accel_ = true;
  } else {
    const double alpha = dt / (config_.accel_filter_tau_s + dt);
    for (int i = 0; i < 3; ++i) accel_enu_[i] += alpha * (raw[i] - accel_enu_[i]);
  }
  previous_time_s_ = s.sim_time;
  previous_vel_enu_ = vel;

  const Quat q = frames::normalize({s.quat[0], s.quat[1], s.quat[2], s.quat[3]});

  SensorFrame f;
  f.time_s = s.sim_time;
  f.gyro = ang;
  f.accel = frames::rotateInverse(q, {accel_enu_[0], accel_enu_[1], accel_enu_[2] + kGravity});
  f.mag = frames::rotateInverse(q, config_.mag_field_enu_gauss);

  // Barometer: ISA troposphere (clamped so the power law stays valid).
  const double alt_msl = config_.home_alt_m + s.pos[2];
  const double h = std::clamp(alt_msl, -500.0, 11000.0);
  const double temp_k = kSeaLevelTempK - kLapseRate * h;
  f.baro_pressure_pa = kSeaLevelPressurePa * std::pow(temp_k / kSeaLevelTempK, kPressureExponent);
  f.baro_altitude_m = alt_msl;
  f.baro_temperature_c = temp_k - 273.15;

  // GPS: local tangent plane around home, using the WGS84 radii of curvature at the home latitude.
  const double lat0 = config_.home_lat_deg / kDegPerRad;
  const double sin_lat = std::sin(lat0);
  const double w = 1.0 - kWgs84E2 * sin_lat * sin_lat;
  const double r_prime_vertical = kWgs84A / std::sqrt(w);
  const double r_meridian = kWgs84A * (1.0 - kWgs84E2) / (w * std::sqrt(w));
  f.gps.lat_deg = config_.home_lat_deg + s.pos[1] / r_meridian * kDegPerRad;
  const double cos_lat = std::cos(lat0);
  f.gps.lon_deg = config_.home_lon_deg + (cos_lat > 1e-9 ? s.pos[0] / (r_prime_vertical * cos_lat) * kDegPerRad : 0.0);
  f.gps.alt_m = alt_msl;
  f.gps.vel_ned = {s.vel[1], s.vel[0], -s.vel[2]};
  f.gps.fix = proto::kGps3dFix;
  f.gps.satellites = config_.gps_satellites;

  Truth t;
  t.quat = q;
  t.pos_enu = {s.pos[0], s.pos[1], s.pos[2]};
  t.vel_enu = vel;
  f.truth = t;
  return f;
}

}  // namespace astrosim::fc
