/** @file sdf_model.cpp @brief Aircraft to Gazebo SDF text generation. */
#include "sdf_model.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

#include "gz_names.hpp"

namespace astrosim::modelgen {

using aircraft::Aircraft;
using aircraft::AircraftError;
using aircraft::Geometry;
using aircraft::Inertia;
using aircraft::MassProperties;
using aircraft::Vec3;

namespace {

/** @brief Third row of the rotation matrix R = Rz(yaw) Ry(pitch) Rx(roll). */
std::array<double, 3> thirdRow(const Vec3& rpy) {
  const double sp = std::sin(rpy[1]), cp = std::cos(rpy[1]);
  const double sr = std::sin(rpy[0]), cr = std::cos(rpy[0]);
  return {-sp, cp * sr, cp * cr};
}

/** @brief Format "x y z" with shortest round-trip numbers. */
std::string vec(const Vec3& v) {
  return std::format("{} {} {}", v[0], v[1], v[2]);
}

/** @brief Format a pose element body "x y z r p y". */
std::string pose(const Vec3& xyz, const Vec3& rpy) {
  return vec(xyz) + " " + vec(rpy);
}

/** @brief Emit an <inertial> block. */
void inertial(std::ostream& o, const std::string& ind, const MassProperties& m) {
  o << ind << "<inertial>\n"
    << ind << "  <pose>" << pose(m.center_of_mass_m, {0, 0, 0}) << "</pose>\n"
    << ind << "  <mass>" << std::format("{}", m.mass_kg) << "</mass>\n"
    << ind << "  <inertia>\n"
    << ind << "    <ixx>" << std::format("{}", m.inertia_kg_m2.ixx) << "</ixx>\n"
    << ind << "    <ixy>" << std::format("{}", m.inertia_kg_m2.ixy) << "</ixy>\n"
    << ind << "    <ixz>" << std::format("{}", m.inertia_kg_m2.ixz) << "</ixz>\n"
    << ind << "    <iyy>" << std::format("{}", m.inertia_kg_m2.iyy) << "</iyy>\n"
    << ind << "    <iyz>" << std::format("{}", m.inertia_kg_m2.iyz) << "</iyz>\n"
    << ind << "    <izz>" << std::format("{}", m.inertia_kg_m2.izz) << "</izz>\n"
    << ind << "  </inertia>\n"
    << ind << "</inertial>\n";
}

/** @brief Emit a <geometry> block. */
void geometry(std::ostream& o, const std::string& ind, const Geometry& g) {
  o << ind << "<geometry>\n";
  if (const auto* b = std::get_if<aircraft::Box>(&g)) {
    o << ind << "  <box><size>" << vec(b->size_m) << "</size></box>\n";
  } else if (const auto* c = std::get_if<aircraft::Cylinder>(&g)) {
    o << ind << "  <cylinder><radius>" << std::format("{}", c->radius_m) << "</radius><length>"
      << std::format("{}", c->length_m) << "</length></cylinder>\n";
  } else {
    o << ind << "  <sphere><radius>" << std::format("{}", std::get<aircraft::Sphere>(g).radius_m)
      << "</radius></sphere>\n";
  }
  o << ind << "</geometry>\n";
}

/** @brief Emit a collision and a matching grey visual (visual is for debugging in the gz GUI only). */
void collisionAndVisual(std::ostream& o, const std::string& ind, const std::string& name, const Geometry& g,
                        const Vec3& xyz, const Vec3& rpy, const char* grey) {
  for (const char* kind : {"collision", "visual"}) {
    o << ind << "<" << kind << " name=\"" << name << (kind[0] == 'v' ? "_visual" : "") << "\">\n"
      << ind << "  <pose>" << pose(xyz, rpy) << "</pose>\n";
    geometry(o, ind + "  ", g);
    if (kind[0] == 'v') {
      o << ind << "  <material><ambient>" << grey << " 1</ambient><diffuse>" << grey << " 1</diffuse></material>\n";
    }
    o << ind << "</" << kind << ">\n";
  }
}

/** @brief Emit one MulticopterMotorModel plugin. */
void motorPlugin(std::ostream& o, const Aircraft& a, std::size_t i) {
  const auto& m = a.motors[i];
  const auto& r = a.rotorTypeOf(m);
  auto f = [](double v) { return std::format("{}", v); };
  o << "    <plugin filename=\"gz-sim-multicopter-motor-model-system\" "
       "name=\"gz::sim::systems::MulticopterMotorModel\">\n"
    << "      <robotNamespace>" << gznames::kModelName << "</robotNamespace>\n"
    << "      <jointName>" << gznames::rotorJointName(i) << "</jointName>\n"
    << "      <linkName>" << gznames::rotorLinkName(i) << "</linkName>\n"
    << "      <turningDirection>" << (m.direction == aircraft::Direction::kCcw ? "ccw" : "cw")
    << "</turningDirection>\n"
    << "      <timeConstantUp>" << f(r.time_constant_up_s) << "</timeConstantUp>\n"
    << "      <timeConstantDown>" << f(r.time_constant_down_s) << "</timeConstantDown>\n"
    << "      <maxRotVelocity>" << f(r.max_rot_velocity_rad_s) << "</maxRotVelocity>\n"
    << "      <motorConstant>" << f(r.motor_constant_n_s2) << "</motorConstant>\n"
    << "      <momentConstant>" << f(r.moment_constant_m) << "</momentConstant>\n"
    << "      <commandSubTopic>" << gznames::kMotorCommandSubTopic << "</commandSubTopic>\n"
    << "      <actuator_number>" << i << "</actuator_number>\n"
    << "      <rotorDragCoefficient>" << f(r.rotor_drag_coefficient) << "</rotorDragCoefficient>\n"
    << "      <rollingMomentCoefficient>" << f(r.rolling_moment_coefficient) << "</rollingMomentCoefficient>\n"
    << "      <motorSpeedPubTopic>motor_speed/" << i << "</motorSpeedPubTopic>\n"
    << "      <rotorVelocitySlowdownSim>" << f(a.simulation.rotor_velocity_slowdown) << "</rotorVelocitySlowdownSim>\n"
    << "      <motorType>velocity</motorType>\n"
    << "    </plugin>\n";
}

/** @brief Base-link inertial: loaded aircraft minus every rotor. @throws AircraftError if not physical. */
MassProperties baseLinkMass(const Aircraft& a) {
  MassProperties m = aircraft::loadedMass(a);
  try {
    for (std::size_t i = 0; i < a.motors.size(); ++i) m = aircraft::subtract(m, aircraft::rotorMass(a, i));
  } catch (const std::domain_error& e) {
    throw AircraftError(std::format("airframe/pilot/motor: base link mass remainder is not positive ({})", e.what()));
  }
  std::string why;
  if (!aircraft::isPhysicalInertia(m.inertia_kg_m2, &why)) {
    throw AircraftError(
        std::format("airframe.inertia_kg_m2: base link inertia (airframe + pilot - rotors) is not physical: {}", why));
  }
  return m;
}

}  // namespace

double lowestZ(const Geometry& g, const Vec3& xyz, const Vec3& rpy) {
  const auto row = thirdRow(rpy);
  double half = 0;
  if (const auto* b = std::get_if<aircraft::Box>(&g)) {
    for (int j = 0; j < 3; ++j) half += std::abs(row[j]) * b->size_m[j] / 2;
  } else if (const auto* c = std::get_if<aircraft::Cylinder>(&g)) {
    const double r22 = std::min(1.0, std::abs(row[2]));
    half = r22 * c->length_m / 2 + c->radius_m * std::sqrt(1 - r22 * r22);
  } else {
    half = std::get<aircraft::Sphere>(g).radius_m;
  }
  return xyz[2] - half;
}

double restHeight(const Aircraft& a) {
  double lowest = std::numeric_limits<double>::infinity();
  for (const auto& c : a.collisions) lowest = std::min(lowest, lowestZ(c.geometry, c.xyz_m, c.rpy_rad));
  for (const auto& m : a.motors) {
    const auto& r = a.rotorTypeOf(m);
    lowest =
        std::min(lowest, lowestZ(aircraft::Cylinder{r.prop_radius_m, r.collision_thickness_m}, m.xyz_m, {0, 0, 0}));
  }
  return -lowest;
}

GeneratedModel generateModel(const Aircraft& a) {
  const MassProperties base = baseLinkMass(a);
  auto f = [](double v) { return std::format("{}", v); };
  std::ostringstream o;
  o << "<?xml version=\"1.0\" ?>\n"
    << "<!-- Generated by astrosim-modelgen from the aircraft file for \"" << a.name
    << "\" (Gazebo collision and physics only). Do not edit. -->\n"
    << "<sdf version=\"1.9\">\n"
    << "  <model name=\"" << gznames::kModelName << "\">\n"
    << "    <pose>0 0 " << f(restHeight(a)) << " 0 0 0</pose>\n"
    << "    <link name=\"" << gznames::kBaseLink << "\">\n";
  inertial(o, "      ", base);
  for (const auto& c : a.collisions)
    collisionAndVisual(o, "      ", c.name, c.geometry, c.xyz_m, c.rpy_rad, "0.5 0.5 0.5");
  o << "    </link>\n";

  for (std::size_t i = 0; i < a.motors.size(); ++i) {
    const auto& m = a.motors[i];
    const auto& r = a.rotorTypeOf(m);
    const MassProperties rot = aircraft::rotorMass(a, i);
    const aircraft::Cylinder disc{r.prop_radius_m, r.collision_thickness_m};
    o << "    <link name=\"" << gznames::rotorLinkName(i) << "\">\n"
      << "      <pose>" << pose(m.xyz_m, {0, 0, 0}) << "</pose>\n";
    MassProperties local = rot;
    local.center_of_mass_m = {0, 0, 0};
    inertial(o, "      ", local);
    collisionAndVisual(o, "      ", "collision", disc, {0, 0, 0}, {0, 0, 0}, "0.3 0.3 0.3");
    o << "    </link>\n"
      << "    <joint name=\"" << gznames::rotorJointName(i) << "\" type=\"revolute\">\n"
      << "      <parent>" << gznames::kBaseLink << "</parent>\n"
      << "      <child>" << gznames::rotorLinkName(i) << "</child>\n"
      << "      <axis>\n"
      << "        <xyz>0 0 1</xyz>\n"
      << "        <limit><lower>-1e+16</lower><upper>1e+16</upper></limit>\n"
      << "        <dynamics><damping>0</damping></dynamics>\n"
      << "      </axis>\n"
      << "    </joint>\n";
  }

  for (std::size_t i = 0; i < a.motors.size(); ++i) motorPlugin(o, a, i);

  const auto& d = a.drag;
  o << "    <!-- Airframe drag on all body axes, Fossen sign (negative); added mass and Coriolis off. -->\n"
    << "    <plugin filename=\"gz-sim-hydrodynamics-system\" name=\"gz::sim::systems::Hydrodynamics\">\n"
    << "      <link_name>" << gznames::kBaseLink << "</link_name>\n"
    << "      <xU>" << f(-d.linear_n_s_m[0]) << "</xU>\n"
    << "      <xUabsU>" << f(-d.quadratic_n_s2_m2[0]) << "</xUabsU>\n"
    << "      <yV>" << f(-d.linear_n_s_m[1]) << "</yV>\n"
    << "      <yVabsV>" << f(-d.quadratic_n_s2_m2[1]) << "</yVabsV>\n"
    << "      <zW>" << f(-d.linear_n_s_m[2]) << "</zW>\n"
    << "      <zWabsW>" << f(-d.quadratic_n_s2_m2[2]) << "</zWabsW>\n"
    << "      <disable_added_mass>true</disable_added_mass>\n"
    << "      <disable_coriolis>true</disable_coriolis>\n"
    << "    </plugin>\n"
    << "    <!-- Rotor joint velocities for the gateway's motor speed feedback; joint velocity is rotor speed /"
       " rotorVelocitySlowdownSim. -->\n"
    << "    <plugin filename=\"gz-sim-joint-state-publisher-system\" name=\"gz::sim::systems::JointStatePublisher\">\n"
    << "      <topic>" << gznames::jointStateTopic() << "</topic>\n"
    << "    </plugin>\n"
    << "    <plugin filename=\"gz-sim-odometry-publisher-system\" name=\"gz::sim::systems::OdometryPublisher\">\n"
    << "      <dimensions>3</dimensions>\n"
    << "      <odom_publish_frequency>" << f(a.simulation.odometry_rate_hz) << "</odom_publish_frequency>\n"
    << "      <odom_topic>" << gznames::odometryTopic() << "</odom_topic>\n"
    << "    </plugin>\n"
    << "  </model>\n"
    << "</sdf>\n";

  GeneratedModel out;
  out.sdf = o.str();
  out.config = std::format(
      "<?xml version=\"1.0\"?>\n"
      "<model>\n"
      "  <name>{}</name>\n"
      "  <version>1.0</version>\n"
      "  <sdf version=\"1.9\">model.sdf</sdf>\n"
      "  <description>Collision and physics model of the {}, generated by astrosim-modelgen. Do not "
      "edit.</description>\n"
      "</model>\n",
      gznames::kModelName, a.name);
  return out;
}

}  // namespace astrosim::modelgen
