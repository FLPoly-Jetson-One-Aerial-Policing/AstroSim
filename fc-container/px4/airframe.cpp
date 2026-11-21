/** @file airframe.cpp @brief PX4 airframe script generation. */
#include "airframe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace astrosim::px4 {

namespace {

constexpr double kGravity = 9.80665;       ///< m/s^2.
constexpr std::size_t kMaxPx4Rotors = 12;  ///< CA_ROTOR_COUNT maximum in PX4's control allocator.
constexpr double kCtReference = 6.5;       ///< PX4's default CA_ROTORn_CT; the mean rotor gets this value.
constexpr int kMotorFunctionBase = 101;    ///< PX4 output function of "Motor 1".

/// Rate-loop crossover per unit of 1 / rotor lag, and attitude-loop bandwidth likewise. Chosen from SITL runs of the
/// sample aircraft (lag 0.1 s): rate crossovers of 2.5 rad/s with an attitude loop of 3 rad/s oscillated, 5 rad/s
/// with 2.5 rad/s hovered steadily; the values below keep the same nesting (rate about 2.4x the attitude loop).
constexpr double kRateBandwidthPerLag = 0.6;
constexpr double kAttitudeBandwidthPerLag = 0.25;
constexpr double kRateI = 1.3333;  ///< PX4's default I/P ratio of the roll/pitch rate loops (0.2 / 0.15).
constexpr double kRateD = 0.02;    ///< PX4's default D/P ratio of the roll/pitch rate loops (0.003 / 0.15), s.

/// @brief The hover thrust margin: the aircraft must be able to produce at least this multiple of its weight.
constexpr double kMinThrustToWeight = 1.1;

/**
 * @brief Solve the 4x4 system M x = b by Gaussian elimination with partial pivoting (M is symmetric positive definite
 * here, but no property is relied upon).
 * @param m Matrix (modified).
 * @param b Right-hand side (modified).
 * @return The solution.
 * @throws std::invalid_argument if the matrix is singular (a degenerate rotor layout).
 */
std::array<double, 4> solve4(std::array<std::array<double, 4>, 4> m, std::array<double, 4> b) {
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::fabs(m[r][c]) > std::fabs(m[pivot][c])) pivot = r;
    }
    if (std::fabs(m[pivot][c]) < 1e-12) {
      throw std::invalid_argument("airframe: the rotor layout cannot control roll, pitch, yaw and thrust");
    }
    std::swap(m[c], m[pivot]);
    std::swap(b[c], b[pivot]);
    for (int r = c + 1; r < 4; ++r) {
      const double f = m[r][c] / m[c][c];
      for (int k = c; k < 4; ++k) m[r][k] -= f * m[c][k];
      b[r] -= f * b[c];
    }
  }
  std::array<double, 4> x{};
  for (int r = 3; r >= 0; --r) {
    double v = b[r];
    for (int k = r + 1; k < 4; ++k) v -= m[r][k] * x[k];
    x[r] = v / m[r][r];
  }
  return x;
}

/**
 * @brief Derive the control gains. See AirframeTuning and generateAirframe().
 * @param a Aircraft.
 * @param px Rotor x positions relative to the loaded centre of mass, FRD, m.
 * @param py Rotor y positions, FRD, m.
 * @param km Signed moment coefficients.
 * @param ct Relative thrust coefficients (mean kCtReference).
 * @param mean_thrust Mean rotor maximum thrust, N.
 * @return The tuning.
 */
AirframeTuning deriveTuning(const aircraft::Aircraft& a, const std::vector<double>& px, const std::vector<double>& py,
                            const std::vector<double>& km, const std::vector<double>& ct, double mean_thrust) {
  const std::size_t n = ct.size();
  // PX4 effectiveness rows (control_allocator, ActuatorEffectivenessRotors): rotor axis is (0, 0, -1) in FRD, so
  // roll = -ct*py, pitch = ct*px, yaw = ct*km, thrust z = -ct. The pseudo-inverse mix is B^T (B B^T)^-1.
  std::vector<std::array<double, 4>> col(n);  // B^T rows: one rotor each
  for (std::size_t i = 0; i < n; ++i) col[i] = {-ct[i] * py[i], ct[i] * px[i], ct[i] * km[i], -ct[i]};
  std::array<std::array<double, 4>, 4> gram{};
  for (std::size_t i = 0; i < n; ++i) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) gram[r][c] += col[i][r] * col[i][c];
    }
  }
  // mix[i][axis]: solve gram * x = e_axis for each axis, then mix = B^T x.
  std::vector<std::array<double, 3>> mix(n);
  for (int axis = 0; axis < 3; ++axis) {
    std::array<double, 4> e{};
    e[axis] = 1.0;
    const std::array<double, 4> x = solve4(gram, e);
    for (std::size_t i = 0; i < n; ++i) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += col[i][k] * x[k];
      mix[i][axis] = v;
    }
  }
  // ControlAllocationPseudoInverse::updateControlAllocationMatrixScale().
  auto rpScale = [&](int axis) {
    double sq = 0;
    int nonzero = 0;
    for (std::size_t i = 0; i < n; ++i) {
      sq += mix[i][axis] * mix[i][axis];
      if (std::fabs(mix[i][axis]) > 1e-3) ++nonzero;
    }
    return nonzero > 0 ? std::sqrt(sq / (nonzero / 2.0)) : 1.0;
  };
  const double rp_scale = std::fmax(rpScale(0), rpScale(1));
  double yaw_scale = 0;
  for (std::size_t i = 0; i < n; ++i) yaw_scale = std::fmax(yaw_scale, mix[i][2]);
  if (!(rp_scale > 0) || !(yaw_scale > 0)) throw std::invalid_argument("airframe: degenerate control allocation");

  const double newton_per_ct = mean_thrust / kCtReference;  // rotor thrust = ct * newton_per_ct * actuator command
  const aircraft::Inertia inertia = aircraft::loadedMass(a).inertia_kg_m2;

  AirframeTuning t;
  double lag = 0;
  for (const aircraft::Motor& m : a.motors) {
    const aircraft::RotorType& r = a.rotorTypeOf(m);
    lag = std::fmax(lag, 0.5 * (r.time_constant_up_s + r.time_constant_down_s));
  }
  t.rotor_lag_s = lag;
  t.rate_bandwidth_rad_s = kRateBandwidthPerLag / lag;
  t.roll_authority_rad_s2 = newton_per_ct / rp_scale / inertia.ixx;
  t.pitch_authority_rad_s2 = newton_per_ct / rp_scale / inertia.iyy;
  t.yaw_authority_rad_s2 = newton_per_ct / yaw_scale / inertia.izz;
  // Bounds are PX4's parameter ranges (MC_ROLLRATE_P 0.01..0.5, MC_YAWRATE_P 0..0.6, MC_YAW_P 0..5).
  t.roll_rate_p = std::clamp(t.rate_bandwidth_rad_s / t.roll_authority_rad_s2, 0.01, 0.5);
  t.pitch_rate_p = std::clamp(t.rate_bandwidth_rad_s / t.pitch_authority_rad_s2, 0.01, 0.5);
  t.yaw_rate_p = std::clamp(t.rate_bandwidth_rad_s / t.yaw_authority_rad_s2, 0.01, 0.6);
  t.roll_pitch_att_p = std::clamp(kAttitudeBandwidthPerLag / lag, 0.5, 6.5);
  t.yaw_att_p = std::clamp(t.yaw_rate_p * t.yaw_authority_rad_s2 / 4.0, 0.2, 2.8);
  t.xy_vel_p_acc = t.roll_pitch_att_p / 2.5;
  t.xy_p = 0.4 * t.xy_vel_p_acc;
  return t;
}

}  // namespace

Airframe generateAirframe(const aircraft::Aircraft& a, const AirframeOptions& options) {
  if (a.motors.empty() || a.motors.size() > kMaxPx4Rotors) {
    throw std::invalid_argument(std::format("airframe: PX4 supports 1..{} rotors (the aircraft has {})", kMaxPx4Rotors,
                                            a.motors.size()));
  }
  if (!(options.max_tilt_deg > 0 && options.max_tilt_deg <= 60) || !std::isfinite(options.max_tilt_deg) ||
      !(options.max_yaw_rate_dps > 0 && options.max_yaw_rate_dps <= 360) || !std::isfinite(options.max_yaw_rate_dps)) {
    throw std::invalid_argument("airframe: max_tilt_deg must be in (0, 60] and max_yaw_rate_dps in (0, 360]");
  }

  const aircraft::MassProperties loaded = aircraft::loadedMass(a);
  std::vector<double> max_thrust;  // N per motor at full power
  for (const aircraft::Motor& m : a.motors) {
    const aircraft::RotorType& r = a.rotorTypeOf(m);
    max_thrust.push_back(r.motor_constant_n_s2 * r.max_rot_velocity_rad_s * r.max_rot_velocity_rad_s);
  }
  const double total_thrust = std::accumulate(max_thrust.begin(), max_thrust.end(), 0.0);
  const double weight = loaded.mass_kg * kGravity;
  if (!(total_thrust >= kMinThrustToWeight * weight)) {
    throw std::invalid_argument(std::format(
        "airframe: the aircraft cannot hover: maximum thrust {:.0f} N is below {:.1f} times the loaded weight {:.0f} N",
        total_thrust, kMinThrustToWeight, weight));
  }
  const double hover = weight / total_thrust;
  const double mean_thrust = total_thrust / static_cast<double>(max_thrust.size());

  Airframe out;
  out.file_name = std::format("{}_{}", kAirframeAutostartId, kAirframeModelName);
  out.hover_thrust = hover;

  // Per-rotor values, also needed for the gains.
  std::vector<double> px, py, pz, km, ct;
  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    const aircraft::Motor& m = a.motors[i];
    const aircraft::RotorType& r = a.rotorTypeOf(m);
    px.push_back(m.xyz_m[0] - loaded.center_of_mass_m[0]);
    py.push_back(-(m.xyz_m[1] - loaded.center_of_mass_m[1]));
    pz.push_back(-(m.xyz_m[2] - loaded.center_of_mass_m[2]));
    km.push_back((m.direction == aircraft::Direction::kCcw ? 1.0 : -1.0) * r.moment_constant_m);
    ct.push_back(kCtReference * max_thrust[i] / mean_thrust);
  }
  out.tuning = deriveTuning(a, px, py, km, ct, mean_thrust);

  std::string& t = out.text;
  t += std::format(
      "#!/bin/sh\n"
      "#\n"
      "# @name AstroSim aircraft (generated)\n"
      "# @type Octorotor (coaxial)\n"
      "#\n"
      "# GENERATED by astrosim-px4-airframe from the aircraft file '{}'. Do not edit: it is rewritten at every\n"
      "# container start. Geometry is in PX4's FRD body frame relative to the loaded centre of mass; rotors are in\n"
      "# protocol motor order (PX4 output i drives engine motor i).\n"
      "\n"
      ". ${{R}}etc/init.d/rc.mc_defaults\n"
      "\n",
      a.name);

  t += "# MAV_TYPE_OCTOROTOR\nparam set-default MAV_TYPE 14\n\n";
  t += "# Control allocation: multirotor with the rotor geometry below.\nparam set-default CA_AIRFRAME 0\n";
  t += std::format("param set-default CA_ROTOR_COUNT {}\n\n", a.motors.size());

  t +=
      "# Rotors. PX/PY/PZ: hub position relative to the loaded centre of mass, FRD, m.\n"
      "# KM: torque / thrust; positive for a counter-clockwise rotor seen from above, negative for clockwise (PX4).\n"
      "# CT: thrust coefficient relative to the mean rotor (PX4's default 6.5 is the mean), so the allocator knows\n"
      "# unequal rotors (e.g. the lower coaxial prop) apart.\n";
  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    t += std::format("param set-default CA_ROTOR{0}_PX {1:.4f}\n"
                     "param set-default CA_ROTOR{0}_PY {2:.4f}\n"
                     "param set-default CA_ROTOR{0}_PZ {3:.4f}\n"
                     "param set-default CA_ROTOR{0}_KM {4:.4f}\n"
                     "param set-default CA_ROTOR{0}_CT {5:.4f}\n",
                     i, px[i], py[i], pz[i], km[i], ct[i]);
  }
  t += "\n# Outputs: PWM_MAIN_FUNCn = Motor n, so PX4 output i is protocol motor i.\n";
  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    t += std::format("param set-default PWM_MAIN_FUNC{} {}\n", i + 1, kMotorFunctionBase + static_cast<int>(i));
  }

  t += std::format(
      "\n# Thrust model: the engine maps power linearly to rotor speed, so thrust ~ power^2 (PX4: factor 1 means\n"
      "# rel_thrust = rel_signal^2).\n"
      "param set-default THR_MDL_FAC 1\n"
      "# Hover thrust: loaded weight {:.1f} N over the sum of the rotors' maximum thrust {:.1f} N.\n"
      "param set-default MPC_THR_HOVER {:.4f}\n"
      "\n"
      "# Stick scaling from the configuration ([px4] max_tilt_deg, max_yaw_rate_dps): the roll/pitch sticks command\n"
      "# attitude ANGLE up to this tilt in both flight modes.\n"
      "param set-default MPC_MAN_TILT_MAX {:.1f}\n"
      "param set-default MPC_MAN_Y_MAX {:.1f}\n"
      "param set-default MC_YAWRATE_MAX {:.1f}\n"
      "\n",
      weight, total_thrust, hover, options.max_tilt_deg, options.max_yaw_rate_dps,
      options.max_yaw_rate_dps);

  const AirframeTuning& g = out.tuning;
  t += std::format(
      "# Control gains derived from the aircraft data (astrosim-px4-airframe; PX4's defaults are for a small, fast\n"
      "# quadrotor and make this aircraft oscillate). Rotor lag {:.3f} s sets the rate-loop crossover to {:.1f} rad/s and\n"
      "# the attitude loop to {:.1f} rad/s; the velocity loop is 0.4 of the attitude loop, the position loop 0.4 of that.\n"
      "# Angular acceleration per unit normalized command: roll {:.1f}, pitch {:.1f}, yaw {:.2f} rad/s^2.\n"
      "# Placeholders: they follow the guessed aircraft data and are to be re-derived (or flight-tuned) with real data.\n"
      "param set-default MC_ROLLRATE_P {:.4f}\n"
      "param set-default MC_ROLLRATE_I {:.4f}\n"
      "param set-default MC_ROLLRATE_D {:.5f}\n"
      "param set-default MC_PITCHRATE_P {:.4f}\n"
      "param set-default MC_PITCHRATE_I {:.4f}\n"
      "param set-default MC_PITCHRATE_D {:.5f}\n"
      "param set-default MC_YAWRATE_P {:.4f}\n"
      "param set-default MC_YAWRATE_I {:.4f}\n"
      "param set-default MC_ROLL_P {:.3f}\n"
      "param set-default MC_PITCH_P {:.3f}\n"
      "param set-default MC_YAW_P {:.3f}\n"
      "param set-default MPC_XY_VEL_P_ACC {:.3f}\n"
      "param set-default MPC_XY_P {:.3f}\n"
      "\n",
      g.rotor_lag_s, g.rate_bandwidth_rad_s, g.roll_pitch_att_p, g.roll_authority_rad_s2, g.pitch_authority_rad_s2, g.yaw_authority_rad_s2,
      g.roll_rate_p, g.roll_rate_p * kRateI, g.roll_rate_p * kRateD, g.pitch_rate_p, g.pitch_rate_p * kRateI,
      g.pitch_rate_p * kRateD, g.yaw_rate_p, g.yaw_rate_p * 0.5, g.roll_pitch_att_p, g.roll_pitch_att_p, g.yaw_att_p,
      g.xy_vel_p_acc, g.xy_p);

  t +=
      "# SITL without RC and without a ground station. The pilot input is MAVLink MANUAL_CONTROL from the bridge.\n"
      "# COM_RC_IN_MODE 1: manual control from MAVLink only (no RC receiver).\n"
      "param set-default COM_RC_IN_MODE 1\n"
      "# NAV_RCL_ACT 0: no failsafe action for manual control loss (there is no RC link to lose).\n"
      "param set-default NAV_RCL_ACT 0\n"
      "# NAV_DLL_ACT 0: no failsafe action for GCS data link loss (no ground station is attached).\n"
      "param set-default NAV_DLL_ACT 0\n"
      "# COM_ARM_WO_GPS 1: allow arming without a GPS fix (no-GPS hold must be flyable).\n"
      "param set-default COM_ARM_WO_GPS 1\n";
  return out;
}

}  // namespace astrosim::px4
