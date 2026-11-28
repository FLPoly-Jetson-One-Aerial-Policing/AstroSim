/** @file test_airframe.cpp @brief Tests of the PX4 airframe generator against the sample aircraft file. */
#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <sstream>
#include <string>

#include "airframe.hpp"

using namespace astrosim;

namespace {

/// @brief The `param set-default NAME VALUE` lines of a generated script as a name to value map.
std::map<std::string, double> params(const std::string& text) {
  std::map<std::string, double> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string a, b, name;
    double value = 0;
    if (ls >> a >> b >> name >> value && a == "param" && b == "set-default") out[name] = value;
  }
  return out;
}

aircraft::Aircraft sample() {
  return aircraft::loadAircraft(ASTROSIM_AIRCRAFT_TOML);
}

}  // namespace

TEST(Airframe, FileNameAndHeader) {
  const px4::Airframe f = px4::generateAirframe(sample(), {});
  EXPECT_EQ(f.autostart_id, px4::kAirframeAutostartId);
  EXPECT_EQ(f.file_name, std::to_string(px4::kAirframeAutostartId) + "_astrosim_aircraft");
  EXPECT_EQ(f.text.rfind("#!/bin/sh\n", 0), 0u);
  EXPECT_NE(f.text.find(". ${R}etc/init.d/rc.mc_defaults"), std::string::npos);
  EXPECT_NE(f.text.find("GENERATED"), std::string::npos);
  EXPECT_EQ(f.text.find("acro"), std::string::npos);
  EXPECT_EQ(f.text.find("ACRO"), std::string::npos);
}

TEST(Airframe, RotorCountAndOutputs) {
  const auto p = params(px4::generateAirframe(sample(), {}).text);
  EXPECT_EQ(p.at("CA_AIRFRAME"), 0);
  EXPECT_EQ(p.at("CA_ROTOR_COUNT"), 8);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(p.at("PWM_MAIN_FUNC" + std::to_string(i + 1)), 101 + i);  // identity: PX4 output i is motor i
  }
  EXPECT_EQ(p.count("CA_ROTOR8_PX"), 0u);
}

TEST(Airframe, PositionsAreFrdRelativeToTheLoadedCentreOfMass) {
  const aircraft::Aircraft a = sample();
  const aircraft::MassProperties loaded = aircraft::loadedMass(a);
  const auto p = params(px4::generateAirframe(a, {}).text);
  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    const std::string n = "CA_ROTOR" + std::to_string(i);
    EXPECT_NEAR(p.at(n + "_PX"), a.motors[i].xyz_m[0] - loaded.center_of_mass_m[0], 1e-4) << i;
    EXPECT_NEAR(p.at(n + "_PY"), -(a.motors[i].xyz_m[1] - loaded.center_of_mass_m[1]), 1e-4) << i;
    EXPECT_NEAR(p.at(n + "_PZ"), -(a.motors[i].xyz_m[2] - loaded.center_of_mass_m[2]), 1e-4) << i;
  }
  // Motor 0 is front-right: forward positive, right (FRD y) positive. Hubs are above the centre of mass: z negative.
  EXPECT_GT(p.at("CA_ROTOR0_PX"), 1.0);
  EXPECT_GT(p.at("CA_ROTOR0_PY"), 0.4);
  EXPECT_LT(p.at("CA_ROTOR0_PZ"), 0.0);
  // Motor 1 is rear-left.
  EXPECT_LT(p.at("CA_ROTOR1_PX"), -1.0);
  EXPECT_LT(p.at("CA_ROTOR1_PY"), -0.4);
}

TEST(Airframe, MomentCoefficientSignFollowsSpinDirection) {
  const aircraft::Aircraft a = sample();
  const auto p = params(px4::generateAirframe(a, {}).text);
  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    const double km = p.at("CA_ROTOR" + std::to_string(i) + "_KM");
    const double mag = a.rotorTypeOf(a.motors[i]).moment_constant_m;
    // PX4: positive for a counter-clockwise rotor, negative for clockwise.
    EXPECT_NEAR(km, a.motors[i].direction == aircraft::Direction::kCcw ? mag : -mag, 1e-4) << i;
  }
  EXPECT_GT(p.at("CA_ROTOR0_KM"), 0);  // motor 0 ccw
  EXPECT_LT(p.at("CA_ROTOR2_KM"), 0);  // motor 2 cw
  EXPECT_LT(p.at("CA_ROTOR4_KM"), 0);  // motor 4 (lower, opposite of motor 0) cw
}

TEST(Airframe, ThrustCoefficientsAreRelativeToTheMeanRotor) {
  const auto p = params(px4::generateAirframe(sample(), {}).text);
  double sum = 0;
  for (int i = 0; i < 8; ++i) sum += p.at("CA_ROTOR" + std::to_string(i) + "_CT");
  EXPECT_NEAR(sum / 8, 6.5, 0.01);
  EXPECT_GT(p.at("CA_ROTOR0_CT"), p.at("CA_ROTOR4_CT"));  // the upper rotor produces more thrust than the lower one
}

TEST(Airframe, ThrustModelAndHoverThrust) {
  const aircraft::Aircraft a = sample();
  double total = 0;
  for (const aircraft::Motor& m : a.motors) {
    const aircraft::RotorType& r = a.rotorTypeOf(m);
    total += r.motor_constant_n_s2 * r.max_rot_velocity_rad_s * r.max_rot_velocity_rad_s;
  }
  const double expected = aircraft::loadedMass(a).mass_kg * 9.80665 / total;
  const px4::Airframe f = px4::generateAirframe(a, {});
  const auto p = params(f.text);
  EXPECT_EQ(p.at("THR_MDL_FAC"), 1);
  EXPECT_NEAR(p.at("MPC_THR_HOVER"), expected, 1e-4);
  EXPECT_NEAR(f.hover_thrust, expected, 1e-12);
  EXPECT_GT(expected, 0.4);
  EXPECT_LT(expected, 0.7);
}

TEST(Airframe, StickScalingComesFromTheOptions) {
  px4::AirframeOptions o;
  o.max_tilt_deg = 30;
  o.max_yaw_rate_dps = 60;
  const auto p = params(px4::generateAirframe(sample(), o).text);
  EXPECT_EQ(p.at("MPC_MAN_TILT_MAX"), 30);
  EXPECT_EQ(p.at("MPC_MAN_Y_MAX"), 60);
  EXPECT_EQ(p.at("MC_YAWRATE_MAX"), 60);
}

TEST(Airframe, NoRcNoGcsSitlParameters) {
  const px4::Airframe f = px4::generateAirframe(sample(), {});
  const auto p = params(f.text);
  EXPECT_EQ(p.at("COM_RC_IN_MODE"), 1);
  EXPECT_EQ(p.at("NAV_RCL_ACT"), 0);
  EXPECT_EQ(p.at("NAV_DLL_ACT"), 0);
  EXPECT_EQ(p.at("COM_ARM_WO_GPS"), 1);
  EXPECT_NE(f.text.find("# COM_RC_IN_MODE 1"), std::string::npos);  // each one is commented
  EXPECT_NE(f.text.find("# NAV_RCL_ACT 0"), std::string::npos);
  EXPECT_NE(f.text.find("# NAV_DLL_ACT 0"), std::string::npos);
}

TEST(Airframe, RejectsAnAircraftThatCannotHover) {
  aircraft::Aircraft a = sample();
  a.airframe.mass_kg = 2000;
  EXPECT_THROW(px4::generateAirframe(a, {}), std::invalid_argument);
}

TEST(Airframe, RejectsBadOptions) {
  px4::AirframeOptions o;
  o.max_tilt_deg = 0;
  EXPECT_THROW(px4::generateAirframe(sample(), o), std::invalid_argument);
  o = {};
  o.max_yaw_rate_dps = std::nan("");
  EXPECT_THROW(px4::generateAirframe(sample(), o), std::invalid_argument);
}

TEST(Airframe, RejectsMoreRotorsThanPx4Supports) {
  aircraft::Aircraft a = sample();
  while (a.motors.size() <= 12) a.motors.push_back(a.motors.front());
  EXPECT_THROW(px4::generateAirframe(a, {}), std::invalid_argument);
}

TEST(Airframe, ControlGainsAreDerivedFromTheAircraft) {
  const px4::Airframe f = px4::generateAirframe(sample(), {});
  const px4::AirframeTuning& t = f.tuning;
  const auto p = params(f.text);
  // The written values are the derived ones.
  EXPECT_NEAR(p.at("MC_ROLLRATE_P"), t.roll_rate_p, 1e-4);
  EXPECT_NEAR(p.at("MC_PITCHRATE_P"), t.pitch_rate_p, 1e-4);
  EXPECT_NEAR(p.at("MC_YAWRATE_P"), t.yaw_rate_p, 1e-4);
  EXPECT_NEAR(p.at("MC_ROLL_P"), t.roll_pitch_att_p, 1e-3);
  EXPECT_NEAR(p.at("MC_PITCH_P"), t.roll_pitch_att_p, 1e-3);
  EXPECT_NEAR(p.at("MPC_XY_VEL_P_ACC"), t.xy_vel_p_acc, 1e-3);
  EXPECT_NEAR(p.at("MPC_XY_P"), t.xy_p, 1e-3);
  // Rotor lag is (0.08 + 0.12) / 2 for the sample aircraft; the rate loop crosses over at its inverse.
  EXPECT_NEAR(t.rotor_lag_s, 0.1, 1e-9);
  EXPECT_NEAR(t.rate_bandwidth_rad_s, 6.0, 1e-9);
  EXPECT_NEAR(t.roll_pitch_att_p, 2.5, 1e-9);
  // The cascade is nested: each outer loop is slower than the one inside it.
  EXPECT_LT(t.roll_pitch_att_p, t.rate_bandwidth_rad_s / 2);  // rate loop at least 2x the attitude loop
  EXPECT_LT(t.xy_vel_p_acc, t.roll_pitch_att_p);
  EXPECT_LT(t.xy_p, t.xy_vel_p_acc);
  // Rate gain times authority is the targeted crossover (before PX4's parameter-range clamp).
  EXPECT_NEAR(t.roll_rate_p * t.roll_authority_rad_s2, t.rate_bandwidth_rad_s, 0.05 * t.rate_bandwidth_rad_s);
  EXPECT_NEAR(t.pitch_rate_p * t.pitch_authority_rad_s2, t.rate_bandwidth_rad_s, 0.05 * t.rate_bandwidth_rad_s);
  // Roll and pitch share PX4's normalization scale, but the aircraft is longer than it is wide and its pitch
  // inertia is larger, so the pitch axis has less authority per unit inertia and needs the larger gain.
  EXPECT_GT(t.roll_authority_rad_s2, t.pitch_authority_rad_s2);
  EXPECT_GT(t.pitch_rate_p, t.roll_rate_p);
  // Yaw is weak (moment constant 0.04): by far the least authority.
  EXPECT_LT(t.yaw_authority_rad_s2, t.pitch_authority_rad_s2 / 5);
  // Gains stay inside PX4's parameter ranges.
  EXPECT_LE(t.roll_rate_p, 0.5);
  EXPECT_LE(t.pitch_rate_p, 0.5);
  EXPECT_LE(t.yaw_rate_p, 0.6);
}

TEST(Airframe, RateGainsScaleInverselyWithInertia) {
  aircraft::Aircraft a = sample();
  const px4::AirframeTuning base = px4::generateAirframe(a, {}).tuning;
  a.airframe.inertia_kg_m2.ixx *= 2;
  a.airframe.inertia_kg_m2.iyy *= 2;
  a.pilot.inertia_kg_m2.ixx *= 2;
  a.pilot.inertia_kg_m2.iyy *= 2;
  const px4::AirframeTuning heavier = px4::generateAirframe(a, {}).tuning;
  EXPECT_LT(heavier.roll_authority_rad_s2, base.roll_authority_rad_s2);
  EXPECT_GT(heavier.roll_rate_p, base.roll_rate_p);
  EXPECT_GT(heavier.pitch_rate_p, base.pitch_rate_p);
}

TEST(Airframe, SlowerRotorsGiveSlowerLoops) {
  aircraft::Aircraft a = sample();
  const px4::AirframeTuning base = px4::generateAirframe(a, {}).tuning;
  for (aircraft::RotorType& r : a.rotor_types) {
    r.time_constant_up_s *= 2;
    r.time_constant_down_s *= 2;
  }
  const px4::AirframeTuning slow = px4::generateAirframe(a, {}).tuning;
  EXPECT_NEAR(slow.rate_bandwidth_rad_s, base.rate_bandwidth_rad_s / 2, 1e-9);
  EXPECT_LT(slow.roll_pitch_att_p, base.roll_pitch_att_p);
  EXPECT_LT(slow.xy_vel_p_acc, base.xy_vel_p_acc);
}
