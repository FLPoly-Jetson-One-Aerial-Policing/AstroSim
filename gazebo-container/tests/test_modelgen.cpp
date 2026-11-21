/**
 * @file test_modelgen.cpp
 * @brief Tests of the Gazebo model generator (gz-free, text level).
 */
#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "astrosim/aircraft/aircraft.hpp"
#include "astrosim/aircraft/mass.hpp"
#include "gz_names.hpp"
#include "sdf_model.hpp"

namespace astrosim::modelgen {
namespace {

using aircraft::Aircraft;

/** @brief The shipped aircraft file. */
Aircraft shipped() {
  return aircraft::loadAircraft(ASTROSIM_AIRCRAFT_TOML);
}

/** @brief Count non-overlapping occurrences of @p needle in @p text. */
int count(const std::string& text, const std::string& needle) {
  int n = 0;
  for (auto p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size())) ++n;
  return n;
}

TEST(ModelGen, RestHeightOfShippedFile) {
  EXPECT_NEAR(restHeight(shipped()), 0.45, 1e-12);
}

TEST(ModelGen, BaseRemainderRecombinesToLoadedMass) {
  const Aircraft a = shipped();
  aircraft::MassProperties m = aircraft::loadedMass(a);
  for (std::size_t i = 0; i < a.motors.size(); ++i) m = aircraft::subtract(m, aircraft::rotorMass(a, i));
  EXPECT_NEAR(m.mass_kg, 171.0 - 8.0, 1e-9);
  for (std::size_t i = 0; i < a.motors.size(); ++i) m = aircraft::combine(m, aircraft::rotorMass(a, i));
  const auto loaded = aircraft::loadedMass(a);
  EXPECT_NEAR(m.mass_kg, 171.0, 1e-9);
  for (int k = 0; k < 3; ++k) EXPECT_NEAR(m.center_of_mass_m[k], loaded.center_of_mass_m[k], 1e-9);
  EXPECT_NEAR(m.inertia_kg_m2.ixx, loaded.inertia_kg_m2.ixx, 1e-9);
  EXPECT_NEAR(m.inertia_kg_m2.iyy, loaded.inertia_kg_m2.iyy, 1e-9);
  EXPECT_NEAR(m.inertia_kg_m2.izz, loaded.inertia_kg_m2.izz, 1e-9);
}

TEST(ModelGen, SdfStructure) {
  const Aircraft a = shipped();
  const GeneratedModel g = generateModel(a);
  EXPECT_EQ(count(g.sdf, "<link name=\"rotor_"), 8);
  EXPECT_EQ(count(g.sdf, "<link name=\"base_link\">"), 1);
  EXPECT_EQ(count(g.sdf, "<joint name=\"rotor_"), 8);
  EXPECT_EQ(count(g.sdf, "name=\"gz::sim::systems::MulticopterMotorModel\""), 8);
  EXPECT_EQ(count(g.sdf, "<model name=\"aircraft\">"), 1);
  EXPECT_NE(g.sdf.find("Jetson ONE"), std::string::npos);
  EXPECT_NE(g.sdf.find("<pose>0 0 0.45"), std::string::npos);
  EXPECT_NE(g.sdf.find("<odom_topic>/model/aircraft/odometry</odom_topic>"), std::string::npos);
  EXPECT_NE(g.config.find("<name>aircraft</name>"), std::string::npos);

  // Each motor plugin carries its own actuator number and spin direction.
  std::size_t pos = 0;
  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    pos = g.sdf.find("MulticopterMotorModel", pos);
    ASSERT_NE(pos, std::string::npos);
    const std::size_t end = g.sdf.find("</plugin>", pos);
    const std::string plugin = g.sdf.substr(pos, end - pos);
    EXPECT_NE(plugin.find("<actuator_number>" + std::to_string(i) + "</actuator_number>"), std::string::npos);
    const char* dir = a.motors[i].direction == aircraft::Direction::kCcw ? "ccw" : "cw";
    EXPECT_NE(plugin.find(std::string("<turningDirection>") + dir + "<"), std::string::npos);
    EXPECT_NE(plugin.find("<jointName>" + gznames::rotorJointName(i) + "<"), std::string::npos);
    pos = end;
  }
}

/** @brief Minimal one-motor aircraft TOML with a configurable collision and hub. */
std::string tiny(const std::string& collision, double motor_z = 5.0, double pilot_mass = 10.0) {
  return R"(schema_version = 1
[aircraft]
name = "Tiny"
[airframe]
mass_kg = 10.0
center_of_mass_m = [0.0, 0.0, 0.0]
inertia_kg_m2 = { ixx = 1.0, iyy = 1.0, izz = 1.0, ixy = 0.0, ixz = 0.0, iyz = 0.0 }
[pilot]
mass_kg = )" +
         std::to_string(pilot_mass) + R"(
center_of_mass_m = [0.0, 0.0, 0.0]
inertia_kg_m2 = { ixx = 1.0, iyy = 1.0, izz = 1.0, ixy = 0.0, ixz = 0.0, iyz = 0.0 }
)" + collision +
         R"(
[[rotor_type]]
name = "r"
prop_radius_m = 0.1
collision_thickness_m = 0.01
rotating_mass_kg = 0.5
rotating_inertia_kg_m2 = { ixx = 0.001, iyy = 0.001, izz = 0.002 }
max_rot_velocity_rad_s = 100.0
motor_constant_n_s2 = 1e-3
moment_constant_m = 0.01
time_constant_up_s = 0.1
time_constant_down_s = 0.1
rotor_drag_coefficient = 0.0
rolling_moment_coefficient = 0.0
[[motor]]
rotor_type = "r"
xyz_m = [0.0, 0.0, )" +
         std::to_string(motor_z) + R"(]
direction = "cw"
[drag]
linear_n_s_m = [1.0, 1.0, 1.0]
quadratic_n_s2_m2 = [0.0, 0.0, 0.0]
[simulation]
rotor_velocity_slowdown = 1.0
odometry_rate_hz = 100.0
)";
}

TEST(ModelGen, RestHeightRotatedPrimitives) {
  // Box 2 x 1 x 0.5 at z = 3, pitched 90 degrees: z extent is the original x size / 2 = 1.
  Aircraft a = aircraft::parseAircraft(
      tiny("[[collision]]\nname = \"b\"\nshape = \"box\"\nsize_m = [2.0, 1.0, 0.5]\nxyz_m = [0.0, 0.0, 3.0]\n"
           "rpy_rad = [0.0, 1.5707963267948966, 0.0]\n"));
  EXPECT_NEAR(restHeight(a), -(3.0 - 1.0), 1e-9);

  // Cylinder r = 0.2, L = 2 rolled 90 degrees lies on its side: half height = r.
  a = aircraft::parseAircraft(tiny(
      "[[collision]]\nname = \"c\"\nshape = \"cylinder\"\nradius_m = 0.2\nlength_m = 2.0\nxyz_m = [0.0, 0.0, 1.0]\n"
      "rpy_rad = [1.5707963267948966, 0.0, 0.0]\n"));
  EXPECT_NEAR(restHeight(a), -(1.0 - 0.2), 1e-9);

  // Sphere r = 0.5 at z = 0.25 sticks 0.25 below z = 0.
  a = aircraft::parseAircraft(
      tiny("[[collision]]\nname = \"s\"\nshape = \"sphere\"\nradius_m = 0.5\nxyz_m = [0.0, 0.0, 0.25]\n"
           "rpy_rad = [0.0, 0.0, 0.0]\n"));
  EXPECT_NEAR(restHeight(a), 0.25, 1e-9);
}

TEST(ModelGen, RotorDiscCanBeLowest) {
  const Aircraft a = aircraft::parseAircraft(
      tiny("[[collision]]\nname = \"s\"\nshape = \"sphere\"\nradius_m = 0.5\nxyz_m = [0.0, 0.0, 0.0]\n"
           "rpy_rad = [0.0, 0.0, 0.0]\n",
           -2.0));
  EXPECT_NEAR(restHeight(a), 2.0 + 0.005, 1e-9);
}

TEST(ModelGen, NonPhysicalRemainderThrows) {
  // Rotor mass 0.5 kg is subtracted from a 20 kg aircraft: fine. Make the remainder's inertia non-physical by
  // placing the hub far away so the parallel-axis term exceeds the airframe inertia.
  const std::string col =
      "[[collision]]\nname = \"s\"\nshape = \"sphere\"\nradius_m = 0.5\nxyz_m = [0.0, 0.0, 0.0]\nrpy_rad = [0.0, 0.0, "
      "0.0]\n";
  const Aircraft a = aircraft::parseAircraft(tiny(col, 50.0));
  EXPECT_THROW(generateModel(a), aircraft::AircraftError);
}

}  // namespace
}  // namespace astrosim::modelgen
