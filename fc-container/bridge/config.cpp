/* TOML loading, environment overrides and validation. */
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <initializer_list>
#include <sstream>

#include <toml++/toml.hpp>

#include "astrosim/log/log.hpp"

namespace astrosim::fcbridge {

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

/** @brief Reads the scalar keys of one TOML table, applying env overrides on top of file values. */
class SectionReader {
 public:
  SectionReader(const toml::table* tbl, std::string name, const EnvLookup& env)
      : tbl_(tbl), name_(std::move(name)), env_(env) {}

  std::string path(std::string_view key) const { return name_ + "." + std::string(key); }

  /** @brief True if the key is present in the file (not counting the environment). */
  bool has(std::string_view key) const { return node(key) != nullptr; }

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

  /** @brief Boolean value of @p key (env accepts true/false/1/0), from env, then file, else @p def. */
  bool getBool(std::string_view key, bool def) const {
    if (const auto e = envValue(key)) {
      if (*e == "true" || *e == "1") return true;
      if (*e == "false" || *e == "0") return false;
      fail(path(key), std::format("expected true or false, got '{}' from {}", *e, envName(key)));
    }
    if (const toml::node* n = node(key)) {
      if (n->is_boolean()) return *n->value<bool>();
      fail(path(key), "expected true or false");
    }
    return def;
  }

  /** @brief getDouble() that additionally requires a finite value > 0. */
  double getPositive(std::string_view key, double def) const {
    const double v = getDouble(key, def);
    if (!std::isfinite(v) || v <= 0) fail(path(key), std::format("must be > 0 and finite (got {})", v));
    return v;
  }

  /** @brief getDouble() that additionally requires a finite value in [lo, hi]. */
  double getInRange(std::string_view key, double def, double lo, double hi) const {
    const double v = getDouble(key, def);
    if (!std::isfinite(v) || v < lo || v > hi) fail(path(key), std::format("must be in [{}, {}] (got {})", lo, hi, v));
    return v;
  }

  /** @brief getInt() restricted to a valid UDP port. */
  std::uint16_t getPort(std::string_view key, std::uint16_t def) const {
    const auto port = getInt(key, def);
    if (port < 1 || port > 65535) fail(path(key), std::format("must be in 1..65535 (got {})", port));
    return static_cast<std::uint16_t>(port);
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

void readEngine(const toml::table& root, const EnvLookup& env, Config::Engine& out) {
  const toml::table* t = sectionTable(root, "engine");
  if (t) rejectUnknownKeys(*t, "engine", {"host", "port"});
  const SectionReader r(t, "engine", env);
  out.host = r.getString("host", out.host);
  if (out.host.empty()) fail("engine.host", "must not be empty");
  out.port = r.getPort("port", out.port);
}

void readTiming(const toml::table& root, const EnvLookup& env, Config::Timing& out) {
  const toml::table* t = sectionTable(root, "timing");
  if (t) {
    rejectUnknownKeys(*t, "timing",
                      {"control_rate_hz", "subscribe_interval_s", "pilot_rate_hz", "fc_timeout_s", "telemetry_rate_hz",
                       "stats_interval_s"});
  }
  const SectionReader r(t, "timing", env);
  out.control_rate_hz = r.getPositive("control_rate_hz", out.control_rate_hz);
  out.subscribe_interval_s = r.getPositive("subscribe_interval_s", out.subscribe_interval_s);
  if (out.subscribe_interval_s > 1.0) {
    fail("timing.subscribe_interval_s",
         std::format("must be <= 1 (keepalive must be at least 1 Hz; got {})", out.subscribe_interval_s));
  }
  out.pilot_rate_hz = r.getPositive("pilot_rate_hz", out.pilot_rate_hz);
  out.fc_timeout_s = r.getPositive("fc_timeout_s", out.fc_timeout_s);
  out.telemetry_rate_hz = r.getPositive("telemetry_rate_hz", out.telemetry_rate_hz);
  out.stats_interval_s = r.getPositive("stats_interval_s", out.stats_interval_s);
}

void readTelemetry(const toml::table& root, const EnvLookup& env, Config::Telemetry& out) {
  const toml::table* t = sectionTable(root, "telemetry");
  if (t) rejectUnknownKeys(*t, "telemetry", {"bind", "port", "max_subscribers", "subscriber_timeout_s"});
  const SectionReader r(t, "telemetry", env);
  out.bind = r.getString("bind", out.bind);
  if (!isIpv4(out.bind)) fail("telemetry.bind", std::format("must be an IPv4 dotted quad (got '{}')", out.bind));
  out.port = r.getPort("port", out.port);
  const auto subs = r.getInt("max_subscribers", static_cast<std::int64_t>(out.max_subscribers));
  if (subs < 1) fail("telemetry.max_subscribers", std::format("must be >= 1 (got {})", subs));
  out.max_subscribers = static_cast<std::size_t>(subs);
  out.subscriber_timeout_s = r.getPositive("subscriber_timeout_s", out.subscriber_timeout_s);
}

void readFc(const toml::table& root, const EnvLookup& env, Config::Fc& out) {
  const toml::table* t = sectionTable(root, "fc");
  if (t) rejectUnknownKeys(*t, "fc", {"adapter", "sensor_source"});
  const SectionReader r(t, "fc", env);
  out.adapter = r.getString("adapter", out.adapter);
  if (out.adapter != "px4" && out.adapter != "null") {
    fail("fc.adapter", std::format("must be \"px4\" or \"null\" (test only) (got '{}')", out.adapter));
  }
  out.sensor_source = r.getString("sensor_source", out.sensor_source);
  if (out.sensor_source != "synthesized_temporary") {
    fail("fc.sensor_source",
         std::format("the only supported value is \"synthesized_temporary\" (got '{}')", out.sensor_source));
  }
}

void readHome(const toml::table& root, const EnvLookup& env, Config::Home& out) {
  const toml::table* t = sectionTable(root, "home");
  if (t) rejectUnknownKeys(*t, "home", {"lat_deg", "lon_deg", "alt_m"});
  const SectionReader r(t, "home", env);
  out.lat_deg = r.getInRange("lat_deg", out.lat_deg, -90, 90);
  out.lon_deg = r.getInRange("lon_deg", out.lon_deg, -180, 180);
  out.alt_m = r.getInRange("alt_m", out.alt_m, -1000, 100000);
}

void readMag(const toml::table& root, const EnvLookup& env, Config::Mag& out) {
  const toml::table* t = sectionTable(root, "mag");
  if (t) rejectUnknownKeys(*t, "mag", {"east_gauss", "north_gauss", "up_gauss"});
  const SectionReader r(t, "mag", env);
  out.east_gauss = r.getInRange("east_gauss", out.east_gauss, -2, 2);
  out.north_gauss = r.getInRange("north_gauss", out.north_gauss, -2, 2);
  out.up_gauss = r.getInRange("up_gauss", out.up_gauss, -2, 2);
}

void readPx4(const toml::table& root, const EnvLookup& env, Config::Px4& out) {
  const toml::table* t = sectionTable(root, "px4");
  if (t) {
    rejectUnknownKeys(*t, "px4",
                      {"hil_port", "mavlink_local_port", "mavlink_host", "mavlink_port", "no_gps_mode", "max_tilt_deg",
                       "max_yaw_rate_dps", "sensor_dither"});
  }
  const SectionReader r(t, "px4", env);
  out.hil_port = r.getPort("hil_port", out.hil_port);
  out.mavlink_local_port = r.getPort("mavlink_local_port", out.mavlink_local_port);
  out.mavlink_host = r.getString("mavlink_host", out.mavlink_host);
  if (out.mavlink_host.empty()) fail("px4.mavlink_host", "must not be empty");
  out.mavlink_port = r.getPort("mavlink_port", out.mavlink_port);
  out.no_gps_mode = r.getString("no_gps_mode", out.no_gps_mode);
  if (out.no_gps_mode != "altctl" && out.no_gps_mode != "stabilized") {
    fail("px4.no_gps_mode", std::format("must be \"altctl\" or \"stabilized\" (got '{}')", out.no_gps_mode));
  }
  out.max_tilt_deg = r.getInRange("max_tilt_deg", out.max_tilt_deg, 1, 60);
  out.max_yaw_rate_dps = r.getInRange("max_yaw_rate_dps", out.max_yaw_rate_dps, 1, 360);
  out.sensor_dither = r.getBool("sensor_dither", out.sensor_dither);
}

/** @brief Parse one [[pilot.script]] table. Not overridable from the environment. */
fc::Keyframe readKeyframe(const toml::table& t, const std::string& where) {
  rejectUnknownKeys(t, where, {"time_s", "roll", "pitch", "yaw", "throttle", "mode", "arm"});
  const EnvLookup no_env = [](std::string_view) { return std::nullopt; };
  const SectionReader r(&t, where, no_env);
  if (!r.has("time_s")) fail(where + ".time_s", "required");
  fc::Keyframe k;
  k.time_s = r.getInRange("time_s", 0, 0, 1e9);
  k.input.roll = static_cast<float>(r.getInRange("roll", 0, -1, 1));
  k.input.pitch = static_cast<float>(r.getInRange("pitch", 0, -1, 1));
  k.input.yaw = static_cast<float>(r.getInRange("yaw", 0, -1, 1));
  k.input.throttle = static_cast<float>(r.getInRange("throttle", 0, 0, 1));
  const std::string mode = r.getString("mode", "gps_hold");
  if (mode == "gps_hold") {
    k.input.mode = fc::FlightMode::kGpsHold;
  } else if (mode == "no_gps_hold") {
    k.input.mode = fc::FlightMode::kNoGpsHold;
  } else {
    fail(r.path("mode"), std::format("must be \"gps_hold\" or \"no_gps_hold\" (got '{}')", mode));
  }
  k.input.arm = r.getBool("arm", false);
  return k;
}

void readPilot(const toml::table& root, const EnvLookup& env, Config::Pilot& out) {
  const toml::table* t = sectionTable(root, "pilot");
  if (!t) return;
  rejectUnknownKeys(*t, "pilot", {"interpolation", "script"});
  const SectionReader r(t, "pilot", env);
  const std::string interp = r.getString("interpolation", "hold");
  if (interp == "hold") {
    out.interpolation = fc::Interpolation::kHold;
  } else if (interp == "linear") {
    out.interpolation = fc::Interpolation::kLinear;
  } else {
    fail("pilot.interpolation", std::format("must be \"hold\" or \"linear\" (got '{}')", interp));
  }
  const toml::node* s = t->get("script");
  if (!s) return;
  const toml::array* arr = s->as_array();
  if (!arr || !arr->is_array_of_tables()) fail("pilot.script", "expected an array of tables ([[pilot.script]])");
  for (std::size_t i = 0; i < arr->size(); ++i) {
    fc::Keyframe k = readKeyframe(*arr->get(i)->as_table(), std::format("pilot.script[{}]", i));
    if (!out.script.empty() && k.time_s <= out.script.back().time_s) {
      fail(std::format("pilot.script[{}].time_s", i),
           std::format("must be strictly greater than the previous keyframe's ({})", out.script.back().time_s));
    }
    out.script.push_back(k);
  }
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

}  // namespace

fc::SynthConfig Config::synthConfig() const {
  fc::SynthConfig s;
  s.home_lat_deg = home.lat_deg;
  s.home_lon_deg = home.lon_deg;
  s.home_alt_m = home.alt_m;
  s.mag_field_enu_gauss = {mag.east_gauss, mag.north_gauss, mag.up_gauss};
  return s;
}

Config parseConfig(std::string_view toml_text, const EnvLookup& env) {
  toml::table root;
  try {
    root = toml::parse(toml_text);
  } catch (const toml::parse_error& e) {
    throw ConfigError(std::format("TOML syntax error: {}", e.description()));
  }
  rejectUnknownKeys(root, "", {"engine", "timing", "telemetry", "fc", "home", "mag", "px4", "pilot", "log"});

  Config cfg;
  readEngine(root, env, cfg.engine);
  readTiming(root, env, cfg.timing);
  readTelemetry(root, env, cfg.telemetry);
  readFc(root, env, cfg.fc);
  readHome(root, env, cfg.home);
  readMag(root, env, cfg.mag);
  readPx4(root, env, cfg.px4);
  readPilot(root, env, cfg.pilot);
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

}  // namespace astrosim::fcbridge
