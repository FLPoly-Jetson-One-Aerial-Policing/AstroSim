/// @file
/// @brief Tests for the ENU/FLU <-> NED/FRD frame conversions.
#include <gtest/gtest.h>

#include "astrosim/fc/frames.hpp"

namespace astrosim::fc::frames {
namespace {

constexpr double kH = 0.70710678118654752440;

/// @brief Expect two vectors to match component-wise within 1e-12.
void expectVec(const Vec3& a, const Vec3& b) {
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(a[i], b[i], 1e-12) << "component " << i;
}

/// @brief Expect two quaternions to represent the same rotation (q and -q are equal).
void expectSameRotation(const Quat& a, const Quat& b) {
  const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  EXPECT_NEAR(std::abs(dot), 1.0, 1e-12);
}

/// @brief World vector conversions swap x/y and flip z.
TEST(Frames, WorldVectors) {
  expectVec(enuToNed({1, 2, 3}), {2, 1, -3});  // East 1, North 2, Up 3 -> N 2, E 1, D -3
  expectVec(nedToEnu({2, 1, -3}), {1, 2, 3});
}

/// @brief Body vector conversions flip y and z.
TEST(Frames, BodyVectors) {
  expectVec(fluToFrd({1, 2, 3}), {1, -2, -3});
  expectVec(frdToFlu({1, -2, -3}), {1, 2, 3});
}

/// @brief Vector conversions are involutions.
TEST(Frames, VectorRoundTrip) {
  const Vec3 v{0.3, -4.5, 7.25};
  expectVec(nedToEnu(enuToNed(v)), v);
  expectVec(frdToFlu(fluToFrd(v)), v);
}

/// @brief Rotation helpers: 90 degrees about z maps x to y, and the inverse undoes it.
TEST(Frames, RotateAndInverse) {
  const Quat yaw90{0, 0, kH, kH};
  expectVec(rotate(yaw90, {1, 0, 0}), {0, 1, 0});
  expectVec(rotateInverse(yaw90, {0, 1, 0}), {1, 0, 0});
  expectVec(rotate({0, 0, 0, 1}, {1, 2, 3}), {1, 2, 3});
}

/// @brief Hamilton product composes rotations right to left.
TEST(Frames, MultiplyComposes) {
  const Quat yaw90{0, 0, kH, kH};
  expectSameRotation(multiply(yaw90, yaw90), {0, 0, 1, 0});
  expectSameRotation(multiply(yaw90, conjugate(yaw90)), {0, 0, 0, 1});
}

/// @brief A non-unit or zero quaternion is normalized or replaced by the identity.
TEST(Frames, Normalize) {
  const Quat q = normalize({0, 0, 2, 2});
  EXPECT_NEAR(q[2], kH, 1e-12);
  EXPECT_NEAR(q[3], kH, 1e-12);
  EXPECT_EQ(normalize({0, 0, 0, 0}), (Quat{0, 0, 0, 1}));
}

/// @brief Identity FLU/ENU attitude (forward East, left North, up Up) in NED/FRD: forward -> East, right -> South.
TEST(Frames, IdentityOrientation) {
  const Quat q = enuFluToNedFrd({0, 0, 0, 1});
  expectVec(rotate(q, {1, 0, 0}), {0, 1, 0});   // FRD forward = East = NED (0,1,0)
  expectVec(rotate(q, {0, 1, 0}), {-1, 0, 0});  // FRD right = South
  expectVec(rotate(q, {0, 0, 1}), {0, 0, 1});   // FRD down = NED down
}

/// @brief Forward North (yaw +90 about Up in ENU) is the NED/FRD identity.
TEST(Frames, ForwardNorthIsNedIdentity) {
  const Quat q = enuFluToNedFrd({0, 0, kH, kH});
  expectSameRotation(q, {0, 0, 0, 1});
}

/// @brief Round trip of an arbitrary attitude, and consistency with the vector conversions.
TEST(Frames, QuaternionRoundTripAndConsistency) {
  const Quat q = normalize({0.1, -0.2, 0.3, 0.9});
  expectSameRotation(nedFrdToEnuFlu(enuFluToNedFrd(q)), q);

  // Rotating a body vector then converting to NED equals converting then rotating with the converted attitude.
  const Vec3 body_flu{0.4, -1.2, 2.0};
  const Vec3 world_enu = rotate(q, body_flu);
  const Vec3 via_ned = rotate(enuFluToNedFrd(q), fluToFrd(body_flu));
  expectVec(via_ned, enuToNed(world_enu));
}

}  // namespace
}  // namespace astrosim::fc::frames
