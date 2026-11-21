/**
 * @file mass.hpp
 * @brief Rigid-body mass properties and their composition (parallel-axis theorem). Pure math, no I/O.
 *
 * Conventions: SI units, all quantities expressed in one common body frame (FLU: x forward, y left, z up).
 * Inertia products follow the SDF/URDF convention: ixy, ixz, iyz are the off-diagonal elements of the inertia
 * matrix itself, i.e. I = [[ixx, ixy, ixz], [ixy, iyy, iyz], [ixz, iyz, izz]].
 */
#pragma once
#include <array>
#include <string>

namespace astrosim::aircraft {

/** @brief A 3-vector (x, y, z) in metres or radians as the field name says. */
using Vec3 = std::array<double, 3>;

/** @brief Symmetric inertia tensor in kg*m^2 (SDF convention, see file comment). */
struct Inertia {
  double ixx;  ///< Moment about x.
  double iyy;  ///< Moment about y.
  double izz;  ///< Moment about z.
  double ixy;  ///< Matrix element (x, y).
  double ixz;  ///< Matrix element (x, z).
  double iyz;  ///< Matrix element (y, z).
};

/** @brief Mass, centre of mass and inertia about that centre of mass, all in the common body frame. */
struct MassProperties {
  double mass_kg;         ///< Mass, kg.
  Vec3 center_of_mass_m;  ///< Centre of mass position, m.
  Inertia inertia_kg_m2;  ///< Inertia about the centre of mass, body axes.
};

/**
 * @brief Principal moments (eigenvalues of the inertia matrix), ascending.
 * @param i Inertia tensor.
 * @return The three eigenvalues, smallest first.
 */
std::array<double, 3> principalMoments(const Inertia& i);

/**
 * @brief Check that an inertia tensor is physically realisable.
 * @param i Inertia tensor.
 * @param why If non-null and the check fails, receives a human-readable reason.
 * @return True if all values are finite, the tensor is positive-definite and the principal moments satisfy the
 *         triangle inequality (each <= the sum of the other two, within a small relative tolerance).
 */
bool isPhysicalInertia(const Inertia& i, std::string* why = nullptr);

/**
 * @brief Combine two bodies into one.
 * @param a First body.
 * @param b Second body.
 * @return Total mass, combined centre of mass, and inertia about the combined centre of mass.
 */
MassProperties combine(const MassProperties& a, const MassProperties& b);

/**
 * @brief Remove a part from a composite body (inverse of combine()).
 * @param total The composite body.
 * @param part The part to remove.
 * @return The remainder, such that combine(result, part) == total (to rounding).
 * @throws std::domain_error if the remaining mass is not > 0. The remainder's inertia is NOT validated;
 *         callers check it with isPhysicalInertia().
 */
MassProperties subtract(const MassProperties& total, const MassProperties& part);

}  // namespace astrosim::aircraft
