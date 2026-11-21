/**
 * @file test_aircraft.cpp
 * @brief Tests for aircraft file parsing: the shipped file, every-required-key deletion, and rejection cases.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

#include "astrosim/aircraft/aircraft.hpp"

using namespace astrosim::aircraft;

namespace {

/** @brief Read the shipped aircraft file as text. */
std::string shippedText() {
  std::ifstream in(ASTROSIM_AIRCRAFT_TOML, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

/** @brief One step of a key path: a table key and, for arrays of tables, an element index. */
struct Step {
  std::string key;
  int index;  ///< -1 when the key is not indexed.
};
using Path = std::vector<Step>;

/** @brief Render a path as the parser reports it, e.g. "motor[3].xyz_m". */
std::string pathString(const Path& p) {
  std::string s;
  for (const Step& st : p) {
    if (!s.empty()) s += '.';
    s += st.key;
    if (st.index >= 0) s += "[" + std::to_string(st.index) + "]";
  }
  return s;
}

/** @brief Collect the path of every key in @p t (tables and array-of-tables are descended into). */
void collectKeys(const toml::table& t, Path prefix, std::vector<Path>& out) {
  for (const auto& [k, node] : t) {
    Path p = prefix;
    p.push_back({std::string(k.str()), -1});
    out.push_back(p);
    if (const auto* sub = node.as_table()) {
      collectKeys(*sub, p, out);
    } else if (const auto* arr = node.as_array(); arr && arr->is_array_of_tables()) {
      for (std::size_t i = 0; i < arr->size(); ++i) {
        Path q = prefix;
        q.push_back({std::string(k.str()), static_cast<int>(i)});
        collectKeys(*arr->get(i)->as_table(), q, out);
      }
    }
  }
}

/** @brief Walk to the table that holds the last step of @p p. */
toml::table* parentTable(toml::table& root, const Path& p) {
  toml::table* t = &root;
  for (std::size_t i = 0; i + 1 < p.size(); ++i) {
    toml::node* n = t->get(p[i].key);
    t = p[i].index >= 0 ? n->as_array()->get(static_cast<std::size_t>(p[i].index))->as_table() : n->as_table();
  }
  return t;
}

/** @brief The shipped file as a parsed table. */
toml::table shippedTable() {
  return toml::parse(shippedText());
}

/** @brief Serialise a table back to TOML text. */
std::string dump(const toml::table& t) {
  std::ostringstream s;
  s << toml::toml_formatter{t};
  return s.str();
}

/** @brief Apply @p mutate to the parsed shipped file and return the resulting TOML text. */
template <typename F>
std::string mutated(F&& mutate) {
  toml::table t = shippedTable();
  mutate(t);
  return dump(t);
}

/** @brief Return the error message from parsing @p text; fails the test if it does not throw. */
std::string errorOf(const std::string& text) {
  try {
    parseAircraft(text);
  } catch (const AircraftError& e) {
    return e.what();
  }
  ADD_FAILURE() << "expected AircraftError";
  return {};
}

/** @brief Expect parsing @p text to fail with a message starting with @p prefix. */
void expectError(const std::string& text, const std::string& prefix) {
  const std::string msg = errorOf(text);
  EXPECT_EQ(msg.rfind(prefix, 0), 0u) << "message: " << msg << "\nexpected prefix: " << prefix;
}

/** @brief Replace the value at "section.key" (table path) with @p v. */
template <typename V>
std::string withValue(const Path& p, V v) {
  return mutated([&](toml::table& t) { parentTable(t, p)->insert_or_assign(p.back().key, v); });
}

}  // namespace

TEST(Aircraft, ShippedFileLoads) {
  const Aircraft a = loadAircraft(ASTROSIM_AIRCRAFT_TOML);
  EXPECT_EQ(a.name, "Jetson ONE");
  EXPECT_EQ(a.motors.size(), 8u);
  EXPECT_EQ(a.rotor_types.size(), 2u);
  EXPECT_EQ(a.collisions.size(), 7u);
  EXPECT_NEAR(loadedMass(a).mass_kg, 171.0, 1e-9);
  EXPECT_EQ(a.rotorTypeOf(a.motors[0]).name, "upper");
  EXPECT_EQ(a.rotorTypeOf(a.motors[4]).name, "lower");
  EXPECT_EQ(a.motors[0].direction, Direction::kCcw);
  EXPECT_EQ(a.motors[4].direction, Direction::kCw);
  EXPECT_TRUE(std::holds_alternative<Box>(a.collisions[0].geometry));
  EXPECT_EQ(a.rotor_types[0].rotating_inertia_kg_m2.ixy, 0.0);
}

TEST(Aircraft, RotorMass) {
  const Aircraft a = loadAircraft(ASTROSIM_AIRCRAFT_TOML);
  const MassProperties r = rotorMass(a, 3);
  EXPECT_DOUBLE_EQ(r.mass_kg, 1.0);
  EXPECT_DOUBLE_EQ(r.center_of_mass_m[0], -1.05);
  EXPECT_THROW(rotorMass(a, 8), std::out_of_range);
}

TEST(Aircraft, LoadedMassCombinesAirframeAndPilot) {
  const Aircraft a = loadAircraft(ASTROSIM_AIRCRAFT_TOML);
  const MassProperties m = loadedMass(a);
  const MassProperties expect = combine(a.airframe, a.pilot);
  EXPECT_DOUBLE_EQ(m.center_of_mass_m[0], expect.center_of_mass_m[0]);
  EXPECT_TRUE(isPhysicalInertia(m.inertia_kg_m2));
}

TEST(Aircraft, MissingKeyNamesItsPathForEveryKey) {
  std::vector<Path> keys;
  collectKeys(shippedTable(), {}, keys);
  ASSERT_GT(keys.size(), 100u);
  for (const Path& p : keys) {
    const std::string text = mutated([&](toml::table& t) { parentTable(t, p)->erase(p.back().key); });
    expectError(text, pathString(p) + ": required key missing");
  }
}

TEST(Aircraft, UnknownKeysRejectedAtEveryLevel) {
  expectError(mutated([](toml::table& t) { t.insert("extra", 1); }), "extra: unknown key");
  expectError(mutated([](toml::table& t) { t["pilot"].as_table()->insert("extra", 1); }), "pilot.extra: unknown key");
  expectError(mutated([](toml::table& t) { t["motor"].as_array()->get(5)->as_table()->insert("extra", 1); }),
              "motor[5].extra: unknown key");
  expectError(mutated([](toml::table& t) { t["airframe"]["inertia_kg_m2"].as_table()->insert("ixw", 1.0); }),
              "airframe.inertia_kg_m2.ixw: unknown key");
  expectError(
      mutated([](toml::table& t) {
        t["rotor_type"].as_array()->get(1)->as_table()->at("rotating_inertia_kg_m2").as_table()->insert("ixy", 0.0);
      }),
      "rotor_type[1].rotating_inertia_kg_m2.ixy: unknown key");
}

TEST(Aircraft, ShapeSpecificKeysAreRejectedOnOtherShapes) {
  expectError(mutated([](toml::table& t) {
                auto* c = t["collision"].as_array()->get(1)->as_table();
                c->insert_or_assign("shape", "cylinder");
                c->erase("size_m");
                c->insert("radius_m", 0.1);
                c->insert("length_m", 1.0);
                c->insert("size_m", toml::array{1.0, 1.0, 1.0});
              }),
              "collision[1].size_m: unknown key");
  // Cylinder and sphere parse when well-formed.
  const std::string ok = mutated([](toml::table& t) {
    auto* c = t["collision"].as_array();
    c->get(1)->as_table()->insert_or_assign("shape", "cylinder");
    c->get(1)->as_table()->erase("size_m");
    c->get(1)->as_table()->insert("radius_m", 0.1);
    c->get(1)->as_table()->insert("length_m", 1.0);
    c->get(2)->as_table()->insert_or_assign("shape", "sphere");
    c->get(2)->as_table()->erase("size_m");
    c->get(2)->as_table()->insert("radius_m", 0.1);
  });
  const Aircraft a = parseAircraft(ok);
  EXPECT_TRUE(std::holds_alternative<Cylinder>(a.collisions[1].geometry));
  EXPECT_TRUE(std::holds_alternative<Sphere>(a.collisions[2].geometry));
}

TEST(Aircraft, BadSchemaVersion) {
  expectError(withValue({{"schema_version", -1}}, 2), "schema_version: unsupported version 2");
  expectError(withValue({{"schema_version", -1}}, "1"), "schema_version: expected an integer");
}

TEST(Aircraft, BadEnumStrings) {
  expectError(withValue({{"motor", 2}, {"direction", -1}}, "left"), "motor[2].direction: 'left'");
  const std::string text =
      mutated([](toml::table& t) { t["collision"].as_array()->get(0)->as_table()->insert_or_assign("shape", "cone"); });
  expectError(text, "collision[0].shape: 'cone'");
}

TEST(Aircraft, BadRotorTypeReference) {
  expectError(withValue({{"motor", 6}, {"rotor_type", -1}}, "middle"),
              "motor[6].rotor_type: unknown rotor type 'middle'");
}

TEST(Aircraft, DuplicateNames) {
  expectError(withValue({{"collision", 2}, {"name", -1}}, "tub"), "collision[2].name: duplicate name 'tub'");
  expectError(withValue({{"rotor_type", 1}, {"name", -1}}, "upper"), "rotor_type[1].name: duplicate name 'upper'");
}

TEST(Aircraft, InvalidNames) {
  expectError(withValue({{"collision", 0}, {"name", -1}}, "tub box"), "collision[0].name: 'tub box'");
  expectError(withValue({{"rotor_type", 0}, {"name", -1}}, "a<b"), "rotor_type[0].name: 'a<b'");
  expectError(withValue({{"collision", 0}, {"name", -1}}, ""), "collision[0].name: must not be empty");
  expectError(withValue({{"aircraft", -1}, {"name", -1}}, ""), "aircraft.name: must not be empty");
}

TEST(Aircraft, NonPositiveAndNonFiniteValues) {
  expectError(withValue({{"airframe", -1}, {"mass_kg", -1}}, 0.0), "airframe.mass_kg: must be > 0");
  expectError(withValue({{"pilot", -1}, {"mass_kg", -1}}, -5.0), "pilot.mass_kg: must be > 0");
  expectError(withValue({{"rotor_type", 1}, {"motor_constant_n_s2", -1}}, 0),
              "rotor_type[1].motor_constant_n_s2: must be > 0");
  expectError(withValue({{"rotor_type", 0}, {"rotor_drag_coefficient", -1}}, -1e-3),
              "rotor_type[0].rotor_drag_coefficient: must be >= 0");
  expectError(withValue({{"simulation", -1}, {"odometry_rate_hz", -1}}, std::nan("")),
              "simulation.odometry_rate_hz: expected a finite number");
  expectError(withValue({{"simulation", -1}, {"rotor_velocity_slowdown", -1}}, HUGE_VAL),
              "simulation.rotor_velocity_slowdown: expected a finite number");
  expectError(withValue({{"collision", 0}, {"size_m", -1}}, toml::array{1.0, 0.0, 1.0}),
              "collision[0].size_m[1]: must be > 0");
  expectError(withValue({{"drag", -1}, {"linear_n_s_m", -1}}, toml::array{1.0, -1.0, 1.0}),
              "drag.linear_n_s_m[1]: must be >= 0");
}

TEST(Aircraft, WrongTypes) {
  expectError(withValue({{"airframe", -1}, {"mass_kg", -1}}, "heavy"), "airframe.mass_kg: expected a number");
  expectError(withValue({{"motor", 0}, {"xyz_m", -1}}, toml::array{1.0, 2.0}),
              "motor[0].xyz_m: expected an array of 3 numbers");
  expectError(withValue({{"motor", 0}, {"xyz_m", -1}}, toml::array{1.0, 2.0, "z"}),
              "motor[0].xyz_m[2]: expected a number");
  expectError(withValue({{"airframe", -1}, {"inertia_kg_m2", -1}}, 3), "airframe.inertia_kg_m2: expected a table");
  expectError(withValue({{"motor", -1}}, 3), "motor: expected an array of tables");
  expectError(withValue({{"collision", -1}}, toml::array{}), "collision: at least one entry is required");
}

TEST(Aircraft, IntegersAcceptedAsNumbers) {
  const std::string text = withValue({{"airframe", -1}, {"mass_kg", -1}}, 86);
  EXPECT_DOUBLE_EQ(parseAircraft(text).airframe.mass_kg, 86.0);
}

TEST(Aircraft, NonPhysicalInertia) {
  expectError(mutated([](toml::table& t) {
                auto* i = t["airframe"]["inertia_kg_m2"].as_table();
                i->insert_or_assign("ixy", 100.0);
              }),
              "airframe.inertia_kg_m2: ");
  expectError(mutated([](toml::table& t) {
                auto* i = t["pilot"]["inertia_kg_m2"].as_table();
                i->insert_or_assign("izz", 100.0);  // triangle inequality violated
              }),
              "pilot.inertia_kg_m2: ");
  expectError(mutated([](toml::table& t) {
                t["rotor_type"]
                    .as_array()
                    ->get(0)
                    ->as_table()
                    ->at("rotating_inertia_kg_m2")
                    .as_table()
                    ->insert_or_assign("izz", 1.0);
              }),
              "rotor_type[0].rotating_inertia_kg_m2: ");
  expectError(mutated([](toml::table& t) {
                t["rotor_type"]
                    .as_array()
                    ->get(0)
                    ->as_table()
                    ->at("rotating_inertia_kg_m2")
                    .as_table()
                    ->insert_or_assign("ixx", -0.01);
              }),
              "rotor_type[0].rotating_inertia_kg_m2.ixx: must be > 0");
}

TEST(Aircraft, SyntaxErrorAndMissingFile) {
  EXPECT_THROW(parseAircraft("schema_version = = 1"), AircraftError);
  expectError("schema_version = = 1", "TOML syntax error");
  try {
    loadAircraft("/nonexistent/aircraft.toml");
    ADD_FAILURE() << "expected AircraftError";
  } catch (const AircraftError& e) {
    EXPECT_EQ(std::string(e.what()).rfind("/nonexistent/aircraft.toml: ", 0), 0u);
  }
}

TEST(Aircraft, LoadPrefixesPath) {
  // The shipped file is valid, so make a bad one in memory via the parse path instead: check prefix format on a
  // directory (unreadable as a file or empty -> missing schema_version).
  try {
    loadAircraft(std::filesystem::path(ASTROSIM_AIRCRAFT_TOML).parent_path() / "no_such.toml");
    ADD_FAILURE();
  } catch (const AircraftError& e) {
    EXPECT_NE(std::string(e.what()).find("no_such.toml"), std::string::npos);
  }
}
