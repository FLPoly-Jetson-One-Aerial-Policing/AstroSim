/**
 * @file sdf_model.hpp
 * @brief Generates the Gazebo model (model.sdf and model.config text) from an Aircraft. gz-free: pure text output.
 */
#pragma once
#include <string>

#include "astrosim/aircraft/aircraft.hpp"

namespace astrosim::modelgen {

/** @brief Generated model files. */
struct GeneratedModel {
  std::string sdf;     ///< Contents of model.sdf.
  std::string config;  ///< Contents of model.config.
};

/**
 * @brief Lowest z (in the body frame) of a collision primitive.
 * @param geometry Primitive shape.
 * @param xyz Primitive origin in the body frame.
 * @param rpy SDF fixed-axis roll-pitch-yaw of the primitive.
 * @return The minimum z over the primitive's volume.
 */
double lowestZ(const aircraft::Geometry& geometry, const aircraft::Vec3& xyz, const aircraft::Vec3& rpy);

/**
 * @brief Height of the body-frame origin above the ground when the aircraft rests on it.
 * @param a Aircraft.
 * @return Minus the lowest z over all body collisions and rotor-disc collisions.
 */
double restHeight(const aircraft::Aircraft& a);

/**
 * @brief Generate the Gazebo model.
 * @param a Validated aircraft.
 * @return model.sdf and model.config text.
 * @throws aircraft::AircraftError if the base-link remainder (airframe + pilot minus rotors) is not physical.
 */
GeneratedModel generateModel(const aircraft::Aircraft& a);

}  // namespace astrosim::modelgen
