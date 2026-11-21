/** @file config.cpp @brief TOML loading, environment overrides and validation. */
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

#include "log.hpp"

namespace astrosim::gateway {

std::optional<std::string> processEnv(std::string_view name) {
  const char* v = std::getenv(std::string(name).c_str());
  if (!v) return std::nullopt;
  return std::string(v);
}

namespace {

[[noreturn]] void fail(const std::string& key, const std::string& what) {
  throw ConfigError(std::format("{}: {}", key, what));
}

/** @brief Throw if @p tbl contains a key not in @p allowed. @p where is the table's config path. */
void rejectUnknownKeys(const toml::table& tbl, const std::string& where,
                       std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, node] : tbl) {
    (void)node;
    if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
      const std::string name(key.str());
      fail(where.empty() ? name : where + "." + name, "unknown key");
    }
  }
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::optional<std::int64_t> parseInt(const std::string& s) {
  std::int64_t v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<double> parseDouble(const std::string& s) {
  double v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

/** @brief Check for a dotted quad of four decimal octets 0..255. */
bool isIpv4(const std::string& s) {
  std::size_t pos = 0;
  for (int part = 0; part < 4; ++part) {
    const std::size_t end = s.find('.', pos);
    const std::string octet = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const auto v = parseInt(octet);
    if (octet.empty() || !std::isdigit(static_cast<unsigned char>(octet[0])) || !v || *v > 255) return false;
    if ((end == std::string::npos) != (part == 3)) return false;
    pos = end + 1;
  }
  return true;
}

/**
 * @brief Reads the scalar keys of one TOML section, applying env overrides on top of file values.
 */
class SectionReader {
 public:
  SectionReader(const toml::table* tbl, std::string name, const EnvLookup& env)
      : tbl_(tbl), name_(std::move(name)), env_(env) {}

  std::string path(std::string_view key) const { return name_ + "." + std::string(key); }

  /** @brief Integer value of @p key, from env, then file, else @p def. */
  std::int64_t getInt(std::string_view key, std::int64_t def) const {
    if (const auto e = envValue(key)) {
      if (const auto v = parseInt(*e)) return *v;
      fail(path(key), std::format("expected an integer, got '{}' from {}", *e, envName(key)));
    }
    if (const toml::node* n = node(key)) {
      if (const auto v = n->value<std::int64_t>(); v && n->is_integer()) return *v;
      fail(path(key), "expected an integer");
    }
    return def;
  }

  /** @brief Floating-point value of @p key (integers accepted), from env, then file, else @p def. */
  double getDouble(std::string_view key, double def) const {
    if (const auto e = envValue(key)) {
      if (const auto v = parseDouble(*e)) return *v;
      fail(path(key), std::format("expected a number, got '{}' from {}", *e, envName(key)));
    }
    if (const toml::node* n = node(key)) {
      if (n->is_number()) return *n->value<double>();
      fail(path(key), "expected a number");
    }
    return def;
  }

  /** @brief String value of @p key, from env, then file, else @p def. */
  std::string getString(std::string_view key, const std::string& def) const {
    if (const auto e = envValue(key)) return *e;
    if (const toml::node* n = node(key)) {
      if (const auto v = n->value<std::string>()) return *v;
      fail(path(key), "expected a string");
    }
    return def;
  }

  /** @brief getDouble() that additionally requires a finite value > 0. */
  double getPositive(std::string_view key, double def) const {
    const double v = getDouble(key, def);
    if (!std::isfinite(v) || v <= 0) fail(path(key), std::format("must be > 0 and finite (got {})", v));
    return v;
  }

 private:
  const toml::node* node(std::string_view key) const { return tbl_ ? tbl_->get(key) : nullptr; }
  std::string envName(std::string_view key) const { return "ASTROSIM_" + upper(name_) + "_" + upper(std::string(key)); }
  std::optional<std::string> envValue(std::string_view key) const { return env_(envName(key)); }

  const toml::table* tbl_;
  std::string name_;
  const EnvLookup& env_;
};

/** @brief Fetch the optional sub-table @p name of @p root; throws if present but not a table. */
const toml::table* sectionTable(const toml::table& root, const std::string& name) {
  const toml::node* n = root.get(name);
  if (!n) return nullptr;
  if (!n->is_table()) fail(name, "expected a table");
  return n->as_table();
}

void readNetwork(const toml::table& root, const EnvLookup& env, Config::Network& out) {
  const toml::table* t = sectionTable(root, "network");
  if (t) rejectUnknownKeys(*t, "network", {"port", "bind", "max_subscribers"});
  const SectionReader r(t, "network", env);

  const auto port = r.getInt("port", out.port);
  if (port < 1 || port > 65535) fail("network.port", std::format("must be in 1..65535 (got {})", port));
  out.port = static_cast<std::uint16_t>(port);

  out.bind = r.getString("bind", out.bind);
  if (!isIpv4(out.bind)) fail("network.bind", std::format("must be an IPv4 dotted quad (got '{}')", out.bind));

  const auto subs = r.getInt("max_subscribers", static_cast<std::int64_t>(out.max_subscribers));
  if (subs < 1) fail("network.max_subscribers", std::format("must be >= 1 (got {})", subs));
  out.max_subscribers = static_cast<std::size_t>(subs);
}

void readTiming(const toml::table& root, const EnvLookup& env, Config::Timing& out) {
  const toml::table* t = sectionTable(root, "timing");
  if (t) {
    rejectUnknownKeys(*t, "timing",
                      {"state_rate_hz", "deadman_timeout_s", "lease_timeout_s", "subscriber_timeout_s",
                       "engine_stale_s", "stats_interval_s"});
  }
  const SectionReader r(t, "timing", env);
  out.state_rate_hz = r.getPositive("state_rate_hz", out.state_rate_hz);
  out.deadman_timeout_s = r.getPositive("deadman_timeout_s", out.deadman_timeout_s);
  out.lease_timeout_s = r.getPositive("lease_timeout_s", out.lease_timeout_s);
  out.subscriber_timeout_s = r.getPositive("subscriber_timeout_s", out.subscriber_timeout_s);
  out.engine_stale_s = r.getPositive("engine_stale_s", out.engine_stale_s);
  out.stats_interval_s = r.getPositive("stats_interval_s", out.stats_interval_s);
}

void readEngine(const toml::table& root, const EnvLookup& env, Config::Engine& out) {
  const toml::table* t = sectionTable(root, "engine");
  if (t)
    rejectUnknownKeys(*t, "engine",
                      {"model", "odometry_topic", "motor_command_topic", "motor_speed_topic", "motor_joint_name",
                       "motor_speed_scale"});
  const SectionReader r(t, "engine", env);
  out.model = r.getString("model", out.model);
  out.odometry_topic = r.getString("odometry_topic", out.odometry_topic);
  out.motor_command_topic = r.getString("motor_command_topic", out.motor_command_topic);
  out.motor_speed_topic = r.getString("motor_speed_topic", out.motor_speed_topic);
  out.motor_joint_name = r.getString("motor_joint_name", out.motor_joint_name);
  if (!out.motor_speed_topic.empty() && out.motor_joint_name.find("{actuator}") == std::string::npos) {
    fail("engine.motor_joint_name",
         std::format("must contain the placeholder {{actuator}} (got '{}')", out.motor_joint_name));
  }
  out.motor_speed_scale = r.getPositive("motor_speed_scale", out.motor_speed_scale);
}

void readLog(const toml::table& root, const EnvLookup& env, Config::Log& out) {
  const toml::table* t = sectionTable(root, "log");
  if (t) rejectUnknownKeys(*t, "log", {"level"});
  const SectionReader r(t, "log", env);
  out.level = r.getString("level", out.level);
  if (!log::parseLevel(out.level)) {
    fail("log.level", std::format("must be one of trace, debug, info, warn, error (got '{}')", out.level));
  }
}

/** @brief Read the required [[motor]] array: exactly kNumMotors entries with unique actuator indices. */
void readMotors(const toml::table& root, std::array<Config::Motor, proto::kNumMotors>& out) {
  const toml::array* arr = root.get_as<toml::array>("motor");
  if (!arr || !arr->is_array_of_tables())
    fail("motor", std::format("expected {} [[motor]] entries (got 0)", proto::kNumMotors));
  if (arr->size() != proto::kNumMotors) {
    fail("motor", std::format("expected exactly {} [[motor]] entries (got {})", proto::kNumMotors, arr->size()));
  }
  std::set<int> seen;
  for (std::size_t i = 0; i < arr->size(); ++i) {
    const toml::table& m = *arr->get(i)->as_table();
    const std::string where = std::format("motor[{}]", i);
    rejectUnknownKeys(m, where, {"actuator_index", "max_rad_s"});

    const auto idx = m.get("actuator_index");
    if (!idx || !idx->is_integer()) fail(where + ".actuator_index", "required integer");
    const auto idx_v = *idx->value<std::int64_t>();
    if (idx_v < 0 || idx_v > 1'000'000) fail(where + ".actuator_index", std::format("must be >= 0 (got {})", idx_v));
    if (!seen.insert(static_cast<int>(idx_v)).second) {
      fail(where + ".actuator_index", std::format("must be unique (got duplicate {})", idx_v));
    }

    const auto rad = m.get("max_rad_s");
    if (!rad || !rad->is_number()) fail(where + ".max_rad_s", "required number");
    const double rad_v = *rad->value<double>();
    if (!std::isfinite(rad_v) || rad_v <= 0)
      fail(where + ".max_rad_s", std::format("must be > 0 and finite (got {})", rad_v));

    out[i] = {static_cast<int>(idx_v), rad_v};
  }
}

}  // namespace

Config parseConfig(std::string_view toml_text, const EnvLookup& env) {
  toml::table root;
  try {
    root = toml::parse(toml_text);
  } catch (const toml::parse_error& e) {
    throw ConfigError(std::format("TOML syntax error: {}", e.description()));
  }
  rejectUnknownKeys(root, "", {"network", "timing", "engine", "motor", "log"});

  Config cfg;
  readNetwork(root, env, cfg.network);
  readTiming(root, env, cfg.timing);
  readEngine(root, env, cfg.engine);
  readMotors(root, cfg.motors);
  readLog(root, env, cfg.log);
  return cfg;
}

Config loadConfig(const std::filesystem::path& path, const EnvLookup& env) {
  std::ifstream in(path);
  if (!in) throw ConfigError(std::format("{}: cannot open config file", path.string()));
  std::ostringstream text;
  text << in.rdbuf();
  try {
    return parseConfig(text.str(), env);
  } catch (const ConfigError& e) {
    throw ConfigError(std::format("{}: {}", path.string(), e.what()));
  }
}

}  // namespace astrosim::gateway
