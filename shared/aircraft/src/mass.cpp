/**
 * @file mass.cpp
 * @brief Mass-property composition and inertia checks. See mass.hpp.
 */
#include "astrosim/aircraft/mass.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace astrosim::aircraft {

namespace {

/** @brief Relative tolerance for the triangle inequality and for rounding in the checks. */
constexpr double kRelTol = 1e-9;

/**
 * @brief Inertia of a point mass at offset @p d from the reference point: m * (|d|^2 E - d d^T).
 * @param m Mass.
 * @param d Offset of the mass from the reference point.
 * @return The point mass's inertia tensor about the reference point (SDF off-diagonal convention).
 */
Inertia pointMassInertia(double m, const Vec3& d) {
  const double dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  return {m * (dd - d[0] * d[0]), m * (dd - d[1] * d[1]), m * (dd - d[2] * d[2]),
          -m * d[0] * d[1],       -m * d[0] * d[2],       -m * d[1] * d[2]};
}

/**
 * @brief Element-wise a + sign * b.
 * @param a Left tensor.
 * @param b Right tensor.
 * @param sign +1 or -1.
 * @return The combined tensor.
 */
Inertia addScaled(const Inertia& a, const Inertia& b, double sign) {
  return {a.ixx + sign * b.ixx, a.iyy + sign * b.iyy, a.izz + sign * b.izz,
          a.ixy + sign * b.ixy, a.ixz + sign * b.ixz, a.iyz + sign * b.iyz};
}

/**
 * @brief Inertia of a body about the frame origin.
 * @param p Body properties (inertia about its own CoM).
 * @return Inertia about the origin.
 */
Inertia aboutOrigin(const MassProperties& p) {
  return addScaled(p.inertia_kg_m2, pointMassInertia(p.mass_kg, p.center_of_mass_m), 1.0);
}

/**
 * @brief Build mass properties from an inertia about the origin.
 * @param mass Total mass.
 * @param moment First moment of mass (sum of m_i * c_i).
 * @param i_origin Inertia about the origin.
 * @return Properties with the inertia moved to the centre of mass.
 */
MassProperties fromOrigin(double mass, const Vec3& moment, const Inertia& i_origin) {
  const Vec3 com{moment[0] / mass, moment[1] / mass, moment[2] / mass};
  return {mass, com, addScaled(i_origin, pointMassInertia(mass, com), -1.0)};
}

bool finite(const Inertia& i) {
  return std::isfinite(i.ixx) && std::isfinite(i.iyy) && std::isfinite(i.izz) && std::isfinite(i.ixy) &&
         std::isfinite(i.ixz) && std::isfinite(i.iyz);
}

}  // namespace

std::array<double, 3> principalMoments(const Inertia& i) {
  const double p1 = i.ixy * i.ixy + i.ixz * i.ixz + i.iyz * i.iyz;
  const double q = (i.ixx + i.iyy + i.izz) / 3.0;
  if (p1 == 0.0) {
    std::array<double, 3> d{i.ixx, i.iyy, i.izz};
    std::sort(d.begin(), d.end());
    return d;
  }
  const double a = i.ixx - q, b = i.iyy - q, c = i.izz - q;
  const double p2 = a * a + b * b + c * c + 2.0 * p1;
  const double p = std::sqrt(p2 / 6.0);
  // B = (A - qI) / p, r = det(B) / 2.
  const double b00 = a / p, b11 = b / p, b22 = c / p;
  const double b01 = i.ixy / p, b02 = i.ixz / p, b12 = i.iyz / p;
  const double det = b00 * (b11 * b22 - b12 * b12) - b01 * (b01 * b22 - b12 * b02) + b02 * (b01 * b12 - b11 * b02);
  const double r = std::clamp(det / 2.0, -1.0, 1.0);
  constexpr double kPi = 3.14159265358979323846;
  const double phi = std::acos(r) / 3.0;
  const double e_max = q + 2.0 * p * std::cos(phi);
  const double e_min = q + 2.0 * p * std::cos(phi + 2.0 * kPi / 3.0);
  const double e_mid = 3.0 * q - e_max - e_min;
  return {e_min, e_mid, e_max};
}

bool isPhysicalInertia(const Inertia& i, std::string* why) {
  auto fail = [&](std::string msg) {
    if (why) *why = std::move(msg);
    return false;
  };
  if (!finite(i)) return fail("inertia contains a non-finite value");
  const auto e = principalMoments(i);
  const double scale = std::max({std::abs(e[0]), std::abs(e[1]), std::abs(e[2])});
  if (!(e[0] > kRelTol * scale)) {
    return fail(std::format("inertia is not positive-definite (smallest principal moment {})", e[0]));
  }
  if (e[2] > (e[0] + e[1]) * (1.0 + kRelTol)) {
    return fail(std::format("principal moments {}, {}, {} violate the triangle inequality", e[0], e[1], e[2]));
  }
  return true;
}

MassProperties combine(const MassProperties& a, const MassProperties& b) {
  const double m = a.mass_kg + b.mass_kg;
  const Vec3 moment{a.mass_kg * a.center_of_mass_m[0] + b.mass_kg * b.center_of_mass_m[0],
                    a.mass_kg * a.center_of_mass_m[1] + b.mass_kg * b.center_of_mass_m[1],
                    a.mass_kg * a.center_of_mass_m[2] + b.mass_kg * b.center_of_mass_m[2]};
  return fromOrigin(m, moment, addScaled(aboutOrigin(a), aboutOrigin(b), 1.0));
}

MassProperties subtract(const MassProperties& total, const MassProperties& part) {
  const double m = total.mass_kg - part.mass_kg;
  if (!std::isfinite(m) || !(m > 0.0)) {
    throw std::domain_error(std::format("remaining mass {} kg is not positive", m));
  }
  const Vec3 moment{total.mass_kg * total.center_of_mass_m[0] - part.mass_kg * part.center_of_mass_m[0],
                    total.mass_kg * total.center_of_mass_m[1] - part.mass_kg * part.center_of_mass_m[1],
                    total.mass_kg * total.center_of_mass_m[2] - part.mass_kg * part.center_of_mass_m[2]};
  return fromOrigin(m, moment, addScaled(aboutOrigin(total), aboutOrigin(part), -1.0));
}

}  // namespace astrosim::aircraft
