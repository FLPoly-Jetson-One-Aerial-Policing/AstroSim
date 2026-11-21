/**
 * @file aircraft.cpp
 * @brief Aircraft TOML parsing and validation. See aircraft.hpp and AIRCRAFT.md.
 *
 * Every key is required, unknown keys are rejected, and every error message starts with the full key path.
 */
#include "astrosim/aircraft/aircraft.hpp"

#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

namespace astrosim::aircraft {

namespace {

/** @brief Throw an AircraftError of the form "<path>: <what>". */
[[noreturn]] void fail(const std::string& path, const std::string& what) {
  throw AircraftError(std::format("{}: {}", path, what));
}

/**
 * @brief Reads keys from one TOML table while tracking its key path.
 *
 * Each accessor requires its key to be present and of the right type, and marks it as known. finish() then rejects
 * any key that no accessor asked for, so unknown keys are an error. No accessor takes a default.
 */
class Reader {
 public:
  /**
   * @brief Wrap a table.
   * @param table The TOML table (must outlive the Reader).
   * @param path The table's key path, e.g. "motor[3]"; empty for the document root.
   */
  Reader(const toml::table& table, std::string path) : table_(table), path_(std::move(path)) {}

  /** @brief Full key path of a child key. */
  std::string keyPath(std::string_view key) const {
    return path_.empty() ? std::string(key) : std::format("{}.{}", path_, key);
  }

  /** @brief Any finite number (integers accepted). */
  double real(std::string_view key) { return toNumber(node(key), keyPath(key)); }

  /** @brief A finite number > 0. */
  double positive(std::string_view key) {
    const double v = real(key);
    if (!(v > 0.0)) fail(keyPath(key), "must be > 0");
    return v;
  }

  /** @brief A finite number >= 0. */
  double nonNegative(std::string_view key) {
    const double v = real(key);
    if (v < 0.0) fail(keyPath(key), "must be >= 0");
    return v;
  }

  /** @brief An array of 3 finite numbers. */
  Vec3 vec3(std::string_view key) { return toVec3(node(key), keyPath(key)); }

  /** @brief An array of 3 finite numbers, each > 0. */
  Vec3 positiveVec3(std::string_view key) {
    const Vec3 v = vec3(key);
    for (std::size_t i = 0; i < 3; ++i) {
      if (!(v[i] > 0.0)) fail(std::format("{}[{}]", keyPath(key), i), "must be > 0");
    }
    return v;
  }

  /** @brief An array of 3 finite numbers, each >= 0. */
  Vec3 nonNegativeVec3(std::string_view key) {
    const Vec3 v = vec3(key);
    for (std::size_t i = 0; i < 3; ++i) {
      if (v[i] < 0.0) fail(std::format("{}[{}]", keyPath(key), i), "must be >= 0");
    }
    return v;
  }

  /** @brief A string. */
  std::string string(std::string_view key) {
    const toml::node& n = node(key);
    const auto* s = n.as_string();
    if (!s) fail(keyPath(key), "expected a string");
    return s->get();
  }

  /** @brief An integer. */
  long long integer(std::string_view key) {
    const toml::node& n = node(key);
    const auto* v = n.as_integer();
    if (!v) fail(keyPath(key), "expected an integer");
    return v->get();
  }

  /** @brief A sub-table (regular or inline). */
  Reader table(std::string_view key) {
    const toml::node& n = node(key);
    const auto* t = n.as_table();
    if (!t) fail(keyPath(key), "expected a table");
    return Reader(*t, keyPath(key));
  }

  /** @brief A non-empty array of tables, one Reader per element with path "key[i]". */
  std::vector<Reader> tables(std::string_view key) {
    const toml::node& n = node(key);
    const auto* a = n.as_array();
    if (a && a->empty()) fail(keyPath(key), "at least one entry is required");
    if (!a || !a->is_array_of_tables()) fail(keyPath(key), "expected an array of tables");
    std::vector<Reader> out;
    for (std::size_t i = 0; i < a->size(); ++i) {
      out.emplace_back(*a->get(i)->as_table(), std::format("{}[{}]", keyPath(key), i));
    }
    return out;
  }

  /** @brief Throw if the table holds a key no accessor read. */
  void finish() const {
    for (const auto& [key, value] : table_) {
      (void)value;
      if (!seen_.contains(std::string(key.str()))) fail(keyPath(key.str()), "unknown key");
    }
  }

  /** @brief This table's key path. */
  const std::string& path() const { return path_; }

 private:
  /** @brief Look up a required key and mark it as known. */
  const toml::node& node(std::string_view key) {
    seen_.insert(std::string(key));
    const toml::node* n = table_.get(key);
    if (!n) fail(keyPath(key), "required key missing");
    return *n;
  }

  /** @brief Convert a node to a finite double (integers allowed). */
  static double toNumber(const toml::node& n, const std::string& path) {
    double v = 0;
    if (const auto* f = n.as_floating_point()) {
      v = f->get();
    } else if (const auto* i = n.as_integer()) {
      v = static_cast<double>(i->get());
    } else {
      fail(path, "expected a number");
    }
    if (!std::isfinite(v)) fail(path, "expected a finite number");
    return v;
  }

  /** @brief Convert a node to a Vec3 of finite numbers. */
  static Vec3 toVec3(const toml::node& n, const std::string& path) {
    const auto* a = n.as_array();
    if (!a || a->size() != 3) fail(path, "expected an array of 3 numbers");
    Vec3 v{};
    for (std::size_t i = 0; i < 3; ++i) v[i] = toNumber(*a->get(i), std::format("{}[{}]", path, i));
    return v;
  }

  const toml::table& table_;
  std::string path_;
  std::set<std::string> seen_;
};

/**
 * @brief Require a name made only of [A-Za-z0-9_] characters.
 * @param path Key path of the name, for the error message.
 * @param name The name.
 */
void checkName(const std::string& path, const std::string& name) {
  if (name.empty()) fail(path, "must not be empty");
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) fail(path, std::format("'{}' is not a valid name (allowed characters: A-Z a-z 0-9 _)", name));
  }
}

/** @brief Require that @p inertia is physical; the error is reported at @p path. */
void checkInertia(const std::string& path, const Inertia& inertia) {
  std::string why;
  if (!isPhysicalInertia(inertia, &why)) fail(path, why);
}

/** @brief Read a `{ixx, iyy, izz, ixy, ixz, iyz}` inertia table and validate it. */
Inertia readInertia(Reader& parent, std::string_view key) {
  Reader t = parent.table(key);
  Inertia i{};
  i.ixx = t.real("ixx");
  i.iyy = t.real("iyy");
  i.izz = t.real("izz");
  i.ixy = t.real("ixy");
  i.ixz = t.real("ixz");
  i.iyz = t.real("iyz");
  t.finish();
  checkInertia(t.path(), i);
  return i;
}

/** @brief Read an `[airframe]`/`[pilot]` section. */
MassProperties readMass(Reader& root, std::string_view key) {
  Reader t = root.table(key);
  MassProperties m{};
  m.mass_kg = t.positive("mass_kg");
  m.center_of_mass_m = t.vec3("center_of_mass_m");
  m.inertia_kg_m2 = readInertia(t, "inertia_kg_m2");
  t.finish();
  return m;
}

/** @brief Read one `[[collision]]` entry. */
Collision readCollision(Reader& r) {
  Collision c;
  c.name = r.string("name");
  checkName(r.keyPath("name"), c.name);
  const std::string shape = r.string("shape");
  if (shape == "box") {
    c.geometry = Box{r.positiveVec3("size_m")};
  } else if (shape == "cylinder") {
    Cylinder cyl{};
    cyl.radius_m = r.positive("radius_m");
    cyl.length_m = r.positive("length_m");
    c.geometry = cyl;
  } else if (shape == "sphere") {
    c.geometry = Sphere{r.positive("radius_m")};
  } else {
    fail(r.keyPath("shape"), std::format("'{}' is not a shape (expected \"box\", \"cylinder\" or \"sphere\")", shape));
  }
  c.xyz_m = r.vec3("xyz_m");
  c.rpy_rad = r.vec3("rpy_rad");
  r.finish();
  return c;
}

/** @brief Read one `[[rotor_type]]` entry. */
RotorType readRotorType(Reader& r) {
  RotorType t{};
  t.name = r.string("name");
  checkName(r.keyPath("name"), t.name);
  t.prop_radius_m = r.positive("prop_radius_m");
  t.collision_thickness_m = r.positive("collision_thickness_m");
  t.rotating_mass_kg = r.positive("rotating_mass_kg");
  {
    Reader in = r.table("rotating_inertia_kg_m2");
    t.rotating_inertia_kg_m2 = Inertia{in.positive("ixx"), in.positive("iyy"), in.positive("izz"), 0.0, 0.0, 0.0};
    in.finish();
    checkInertia(in.path(), t.rotating_inertia_kg_m2);
  }
  t.max_rot_velocity_rad_s = r.positive("max_rot_velocity_rad_s");
  t.motor_constant_n_s2 = r.positive("motor_constant_n_s2");
  t.moment_constant_m = r.positive("moment_constant_m");
  t.time_constant_up_s = r.positive("time_constant_up_s");
  t.time_constant_down_s = r.positive("time_constant_down_s");
  t.rotor_drag_coefficient = r.nonNegative("rotor_drag_coefficient");
  t.rolling_moment_coefficient = r.nonNegative("rolling_moment_coefficient");
  r.finish();
  return t;
}

/** @brief Read one `[[motor]]` entry, resolving its rotor type name against @p types. */
Motor readMotor(Reader& r, const std::vector<RotorType>& types) {
  Motor m{};
  const std::string type_name = r.string("rotor_type");
  bool found = false;
  for (std::size_t i = 0; i < types.size() && !found; ++i) {
    if (types[i].name == type_name) {
      m.rotor_type = i;
      found = true;
    }
  }
  if (!found) fail(r.keyPath("rotor_type"), std::format("unknown rotor type '{}'", type_name));
  m.xyz_m = r.vec3("xyz_m");
  const std::string dir = r.string("direction");
  if (dir == "ccw") {
    m.direction = Direction::kCcw;
  } else if (dir == "cw") {
    m.direction = Direction::kCw;
  } else {
    fail(r.keyPath("direction"), std::format("'{}' is not a direction (expected \"ccw\" or \"cw\")", dir));
  }
  r.finish();
  return m;
}

/** @brief Reject duplicate names among @p items (path is "<array>[i].name"). */
template <typename T>
void checkUniqueNames(const std::string& array, const std::vector<T>& items) {
  std::set<std::string> names;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!names.insert(items[i].name).second) {
      fail(std::format("{}[{}].name", array, i), std::format("duplicate name '{}'", items[i].name));
    }
  }
}

}  // namespace

MassProperties loadedMass(const Aircraft& a) {
  return combine(a.airframe, a.pilot);
}

MassProperties rotorMass(const Aircraft& a, std::size_t motor) {
  const Motor& m = a.motors.at(motor);
  const RotorType& t = a.rotorTypeOf(m);
  return {t.rotating_mass_kg, m.xyz_m, t.rotating_inertia_kg_m2};
}

Aircraft parseAircraft(std::string_view toml_text) {
  toml::table doc;
  try {
    doc = toml::parse(toml_text);
  } catch (const toml::parse_error& e) {
    throw AircraftError(std::format("TOML syntax error at line {}: {}", e.source().begin.line, e.description()));
  }

  Reader root(doc, "");
  Aircraft a{};

  const long long version = root.integer("schema_version");
  if (version != kSchemaVersion) {
    fail("schema_version",
         std::format("unsupported version {} (this build reads version {})", version, kSchemaVersion));
  }

  {
    Reader t = root.table("aircraft");
    a.name = t.string("name");
    if (a.name.empty()) fail(t.keyPath("name"), "must not be empty");
    t.finish();
  }
  a.airframe = readMass(root, "airframe");
  a.pilot = readMass(root, "pilot");

  for (Reader& r : root.tables("collision")) a.collisions.push_back(readCollision(r));
  checkUniqueNames("collision", a.collisions);

  for (Reader& r : root.tables("rotor_type")) a.rotor_types.push_back(readRotorType(r));
  checkUniqueNames("rotor_type", a.rotor_types);

  for (Reader& r : root.tables("motor")) a.motors.push_back(readMotor(r, a.rotor_types));

  {
    Reader t = root.table("drag");
    a.drag.linear_n_s_m = t.nonNegativeVec3("linear_n_s_m");
    a.drag.quadratic_n_s2_m2 = t.nonNegativeVec3("quadratic_n_s2_m2");
    t.finish();
  }
  {
    Reader t = root.table("simulation");
    a.simulation.rotor_velocity_slowdown = t.positive("rotor_velocity_slowdown");
    a.simulation.odometry_rate_hz = t.positive("odometry_rate_hz");
    t.finish();
  }
  root.finish();
  return a;
}

Aircraft loadAircraft(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw AircraftError(std::format("{}: cannot open aircraft file", path.string()));
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) throw AircraftError(std::format("{}: cannot read aircraft file", path.string()));
  try {
    return parseAircraft(text.str());
  } catch (const AircraftError& e) {
    throw AircraftError(std::format("{}: {}", path.string(), e.what()));
  }
}

}  // namespace astrosim::aircraft
