/**
 * @file test_sdf_load.cpp
 * @brief Loads the generated model with sdformat to prove it is valid SDF (built only with ASTROSIM_BUILD_SDF_TEST).
 */
#include <gtest/gtest.h>

#include <sdf/Link.hh>
#include <sdf/Model.hh>
#include <sdf/Root.hh>

#include "astrosim/aircraft/aircraft.hpp"
#include "sdf_model.hpp"

namespace astrosim::modelgen {
namespace {

TEST(SdfLoad, GeneratedModelLoadsCleanly) {
  const auto a = aircraft::loadAircraft(ASTROSIM_AIRCRAFT_TOML);
  const GeneratedModel g = generateModel(a);

  sdf::Root root;
  const sdf::Errors errors = root.LoadSdfString(g.sdf);
  for (const auto& e : errors) ADD_FAILURE() << e.Message();
  ASSERT_TRUE(errors.empty());

  EXPECT_EQ(root.WorldCount(), 0u);
  const sdf::Model* model = root.Model();
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Name(), "aircraft");
  EXPECT_EQ(model->LinkCount(), 9u);
  EXPECT_EQ(model->JointCount(), 8u);
  double mass = 0;
  for (std::uint64_t i = 0; i < model->LinkCount(); ++i) mass += model->LinkByIndex(i)->Inertial().MassMatrix().Mass();
  EXPECT_NEAR(mass, 171.0, 1e-6);
}

}  // namespace
}  // namespace astrosim::modelgen
