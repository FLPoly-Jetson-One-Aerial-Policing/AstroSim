/**
 * @file test_mass.cpp
 * @brief Unit tests for the mass-property math (parallel axis, combine/subtract, inertia validity).
 */
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "astrosim/aircraft/mass.hpp"

using namespace astrosim::aircraft;

namespace {

/** @brief Expect two inertias to match element-wise within @p tol. */
void expectInertiaNear(const Inertia& a, const Inertia& b, double tol = 1e-9) {
  EXPECT_NEAR(a.ixx, b.ixx, tol);
  EXPECT_NEAR(a.iyy, b.iyy, tol);
  EXPECT_NEAR(a.izz, b.izz, tol);
  EXPECT_NEAR(a.ixy, b.ixy, tol);
  EXPECT_NEAR(a.ixz, b.ixz, tol);
  EXPECT_NEAR(a.iyz, b.iyz, tol);
}

/** @brief Expect two mass-property sets to match within @p tol. */
void expectNear(const MassProperties& a, const MassProperties& b, double tol = 1e-9) {
  EXPECT_NEAR(a.mass_kg, b.mass_kg, tol);
  for (int k = 0; k < 3; ++k) EXPECT_NEAR(a.center_of_mass_m[k], b.center_of_mass_m[k], tol);
  expectInertiaNear(a.inertia_kg_m2, b.inertia_kg_m2, tol);
}

const Inertia kZero{0, 0, 0, 0, 0, 0};

}  // namespace

TEST(Mass, TwoPointMassesParallelAxis) {
  // 1 kg at x=+1 and 1 kg at x=-1: CoM at origin, Iyy = Izz = 2, Ixx = 0.
  const MassProperties a{1.0, {1, 0, 0}, kZero};
  const MassProperties b{1.0, {-1, 0, 0}, kZero};
  const MassProperties c = combine(a, b);
  expectNear(c, {2.0, {0, 0, 0}, {0, 2, 2, 0, 0, 0}});
}

TEST(Mass, PointMassOffDiagonalSign) {
  // 1 kg at (1,1,0) plus 1 kg at (-1,-1,0): CoM at origin; ixy = -sum m x y = -2.
  const MassProperties c = combine({1.0, {1, 1, 0}, kZero}, {1.0, {-1, -1, 0}, kZero});
  expectNear(c, {2.0, {0, 0, 0}, {2, 2, 4, -2, 0, 0}});
}

TEST(Mass, CombineMovesCenterOfMass) {
  const MassProperties c = combine({3.0, {0, 0, 0}, {1, 1, 1, 0, 0, 0}}, {1.0, {4, 0, 0}, kZero});
  EXPECT_DOUBLE_EQ(c.mass_kg, 4.0);
  EXPECT_NEAR(c.center_of_mass_m[0], 1.0, 1e-12);
  // Shift of the 3 kg body by 1 m and the 1 kg point by 3 m along x: Iyy = 1 + 3*1 + 1*9 = 13.
  EXPECT_NEAR(c.inertia_kg_m2.ixx, 1.0, 1e-12);
  EXPECT_NEAR(c.inertia_kg_m2.iyy, 13.0, 1e-12);
  EXPECT_NEAR(c.inertia_kg_m2.izz, 13.0, 1e-12);
}

TEST(Mass, CombineSubtractRoundTrip) {
  const MassProperties body{86.0, {0.1, -0.05, 0.15}, {21, 59, 61, 1.5, -0.7, 0.3}};
  const MassProperties part{1.0, {1.05, -0.5, 0.52}, {0.01, 0.01, 0.02, 0.001, 0, -0.002}};
  const MassProperties total = combine(body, part);
  expectNear(subtract(total, part), body);
  // And the other order: remove the big body, get the part back (loses precision but still close).
  expectNear(subtract(total, body), part, 1e-7);
}

TEST(Mass, SubtractManyRotors) {
  const MassProperties airframe{86.0, {0, 0, 0.15}, {21, 59, 61, 0, 0, 0}};
  MassProperties acc = airframe;
  const MassProperties rotor{1.0, {1.05, 0.5, 0.52}, {0.01, 0.01, 0.02, 0, 0, 0}};
  acc = subtract(acc, rotor);
  acc = combine(acc, rotor);
  expectNear(acc, airframe);
}

TEST(Mass, SubtractRejectsNonPositiveRemainder) {
  const MassProperties a{1.0, {0, 0, 0}, {1, 1, 1, 0, 0, 0}};
  EXPECT_THROW(subtract(a, a), std::domain_error);
  EXPECT_THROW(subtract(a, {2.0, {0, 0, 0}, kZero}), std::domain_error);
  EXPECT_THROW(subtract(a, {std::numeric_limits<double>::quiet_NaN(), {0, 0, 0}, kZero}), std::domain_error);
}

TEST(Mass, PrincipalMomentsDiagonalSorted) {
  const auto e = principalMoments({3, 1, 2, 0, 0, 0});
  EXPECT_DOUBLE_EQ(e[0], 1);
  EXPECT_DOUBLE_EQ(e[1], 2);
  EXPECT_DOUBLE_EQ(e[2], 3);
}

TEST(Mass, PrincipalMomentsOfRotatedDiagonal) {
  // diag(1, 2, 4) rotated 30 degrees about z, 20 degrees about y: eigenvalues are unchanged.
  const double a = 0.5235987755982988, b = 0.3490658503988659;
  const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
  // R = Ry(b) * Rz(a)
  const double R[3][3] = {{cb * ca, -cb * sa, sb}, {sa, ca, 0}, {-sb * ca, sb * sa, cb}};
  const double d[3] = {1, 2, 4};
  double M[3][3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      M[i][j] = 0;
      for (int k = 0; k < 3; ++k) M[i][j] += R[i][k] * d[k] * R[j][k];
    }
  }
  const auto e = principalMoments({M[0][0], M[1][1], M[2][2], M[0][1], M[0][2], M[1][2]});
  EXPECT_NEAR(e[0], 1, 1e-12);
  EXPECT_NEAR(e[1], 2, 1e-12);
  EXPECT_NEAR(e[2], 4, 1e-12);
}

TEST(Mass, PrincipalMomentsIsotropic) {
  const auto e = principalMoments({2, 2, 2, 0, 0, 0});
  for (double v : e) EXPECT_DOUBLE_EQ(v, 2);
}

TEST(Mass, PhysicalInertiaAccepted) {
  std::string why;
  EXPECT_TRUE(isPhysicalInertia({21, 59, 61, 0, 0, 0}, &why)) << why;
  EXPECT_TRUE(isPhysicalInertia({2, 2, 2, 0, 0, 0}));
  EXPECT_TRUE(isPhysicalInertia({1, 1, 2, 0, 0, 0}));  // flat plate: triangle inequality met with equality
}

TEST(Mass, PhysicalInertiaRejectsNegative) {
  std::string why;
  EXPECT_FALSE(isPhysicalInertia({-1, 1, 1, 0, 0, 0}, &why));
  EXPECT_FALSE(why.empty());
  EXPECT_FALSE(isPhysicalInertia({0, 1, 1, 0, 0, 0}));
}

TEST(Mass, PhysicalInertiaRejectsNonPositiveDefinite) {
  // Diagonals positive, but products too large: eigenvalue 1 - 2 < 0.
  EXPECT_FALSE(isPhysicalInertia({1, 1, 1, 2, 0, 0}));
}

TEST(Mass, PhysicalInertiaRejectsTriangleViolation) {
  std::string why;
  EXPECT_FALSE(isPhysicalInertia({1, 1, 3, 0, 0, 0}, &why));
  EXPECT_NE(why.find("triangle"), std::string::npos);
}

TEST(Mass, PhysicalInertiaRejectsNonFinite) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(isPhysicalInertia({nan, 1, 1, 0, 0, 0}));
  EXPECT_FALSE(isPhysicalInertia({1, inf, 1, 0, 0, 0}));
  EXPECT_FALSE(isPhysicalInertia({1, 1, 1, 0, nan, 0}));
}
