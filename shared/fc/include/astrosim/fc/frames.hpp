#pragma once
/**
 * @file frames.hpp
 * @brief Frame conversions shared by adapters: world ENU <-> NED and body FLU <-> FRD, for vectors and quaternions.
 *
 * Conventions: ENU is x East, y North, z Up; NED is x North, y East, z Down. FLU is x Forward, y Left, z Up; FRD is
 * x Forward, y Right, z Down. Quaternions are (x, y, z, w), Hamilton, rotating body-frame vectors into the world
 * frame. All functions are pure and header-only.
 */
#include <cmath>

#include "astrosim/fc/types.hpp"

namespace astrosim::fc::frames {

/**
 * @brief Convert a world vector from ENU to NED.
 * @param v Vector in ENU.
 * @return The same vector in NED.
 */
constexpr Vec3 enuToNed(const Vec3& v) {
  return {v[1], v[0], -v[2]};
}

/**
 * @brief Convert a world vector from NED to ENU.
 * @param v Vector in NED.
 * @return The same vector in ENU.
 */
constexpr Vec3 nedToEnu(const Vec3& v) {
  return {v[1], v[0], -v[2]};
}

/**
 * @brief Convert a body vector from FLU to FRD.
 * @param v Vector in FLU.
 * @return The same vector in FRD.
 */
constexpr Vec3 fluToFrd(const Vec3& v) {
  return {v[0], -v[1], -v[2]};
}

/**
 * @brief Convert a body vector from FRD to FLU.
 * @param v Vector in FRD.
 * @return The same vector in FLU.
 */
constexpr Vec3 frdToFlu(const Vec3& v) {
  return {v[0], -v[1], -v[2]};
}

/**
 * @brief Hamilton product a * b (apply @p b first, then @p a).
 * @param a Left quaternion (x, y, z, w).
 * @param b Right quaternion (x, y, z, w).
 * @return The product.
 */
constexpr Quat multiply(const Quat& a, const Quat& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

/**
 * @brief Quaternion conjugate (the inverse for a unit quaternion).
 * @param q Quaternion.
 * @return (-x, -y, -z, w).
 */
constexpr Quat conjugate(const Quat& q) {
  return {-q[0], -q[1], -q[2], q[3]};
}

/**
 * @brief Normalize a quaternion; a zero or non-finite quaternion becomes the identity.
 * @param q Quaternion.
 * @return Unit quaternion.
 */
inline Quat normalize(const Quat& q) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(n > 1e-12) || !std::isfinite(n)) return {0, 0, 0, 1};
  return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}

/**
 * @brief Rotate a vector by a unit quaternion.
 * @param q Unit quaternion (body to world).
 * @param v Vector to rotate.
 * @return q * v * q^-1.
 */
constexpr Vec3 rotate(const Quat& q, const Vec3& v) {
  const Vec3 u{q[0], q[1], q[2]};
  const Vec3 t{2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
  return {v[0] + q[3] * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + q[3] * t[1] + (u[2] * t[0] - u[0] * t[2]),
          v[2] + q[3] * t[2] + (u[0] * t[1] - u[1] * t[0])};
}

/**
 * @brief Rotate a vector by the inverse of a unit quaternion (world to body).
 * @param q Unit quaternion (body to world).
 * @param v Vector to rotate.
 * @return q^-1 * v * q.
 */
constexpr Vec3 rotateInverse(const Quat& q, const Vec3& v) {
  return rotate(conjugate(q), v);
}

/**
 * @brief Convert an attitude from (body FLU to world ENU) to (body FRD to world NED).
 * @param q_enu_flu Attitude, body FLU to world ENU.
 * @return Attitude, body FRD to world NED (the sign of the quaternion is not canonicalized).
 */
constexpr Quat enuFluToNedFrd(const Quat& q_enu_flu) {
  constexpr double h = 0.70710678118654752440;
  constexpr Quat kNedFromEnu{h, h, 0, 0};  // 180 degrees about (1,1,0)/sqrt(2): swaps x/y, flips z
  constexpr Quat kFluFromFrd{1, 0, 0, 0};  // 180 degrees about x: flips y and z
  return multiply(multiply(kNedFromEnu, q_enu_flu), kFluFromFrd);
}

/**
 * @brief Convert an attitude from (body FRD to world NED) to (body FLU to world ENU).
 * @param q_ned_frd Attitude, body FRD to world NED.
 * @return Attitude, body FLU to world ENU (the sign of the quaternion is not canonicalized).
 */
constexpr Quat nedFrdToEnuFlu(const Quat& q_ned_frd) {
  constexpr double h = 0.70710678118654752440;
  constexpr Quat kEnuFromNed{h, h, 0, 0};  // the same 180 degree rotation: it is its own inverse
  constexpr Quat kFrdFromFlu{1, 0, 0, 0};
  return multiply(multiply(kEnuFromNed, q_ned_frd), kFrdFromFlu);
}

}  // namespace astrosim::fc::frames
