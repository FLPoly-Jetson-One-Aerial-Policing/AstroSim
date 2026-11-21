/**
 * @file test_fc_config.cpp
 * @brief Tests of bridge configuration parsing, validation and environment overrides.
 */
#include <gtest/gtest.h>

#include <map>

#include "config.hpp"

namespace astrosim::fcbridge {
namespace {

/** @brief EnvLookup over a fixed map. */
EnvLookup envOf(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(std::string(name));
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}
const EnvLookup kNoEnv = envOf({});

/** @brief Parse and return the ConfigError message, or "" if it parsed. */
std::string errorOf(std::string_view toml, const EnvLookup& env = kNoEnv) {
  try {
    parseConfig(toml, env);
  } catch (const ConfigError& e) {
    return e.what();
  }
  return "";
}

TEST(FcConfig, EmptyFileGivesDefaults) {
  const Config c = parseConfig("", kNoEnv);
  EXPECT_EQ(c.engine.port, 8765);
  EXPECT_DOUBLE_EQ(c.timing.control_rate_hz, 250);
  EXPECT_DOUBLE_EQ(c.timing.telemetry_rate_hz, 30);
  EXPECT_EQ(c.telemetry.port, 8766);
  EXPECT_EQ(c.fc.adapter, "px4");
  EXPECT_EQ(c.fc.sensor_source, "synthesized_temporary");
  EXPECT_TRUE(c.pilot.script.empty());
}

TEST(FcConfig, DefaultFcTomlLoadsAndMatchesStructDefaults) {
  const Config file = loadConfig(ASTROSIM_FC_TOML, kNoEnv);
  const Config def;
  EXPECT_EQ(file.engine.host, def.engine.host);
  EXPECT_EQ(file.engine.port, def.engine.port);
  EXPECT_DOUBLE_EQ(file.timing.control_rate_hz, def.timing.control_rate_hz);
  EXPECT_DOUBLE_EQ(file.timing.subscribe_interval_s, def.timing.subscribe_interval_s);
  EXPECT_DOUBLE_EQ(file.timing.pilot_rate_hz, def.timing.pilot_rate_hz);
  EXPECT_DOUBLE_EQ(file.timing.fc_timeout_s, def.timing.fc_timeout_s);
  EXPECT_DOUBLE_EQ(file.timing.telemetry_rate_hz, def.timing.telemetry_rate_hz);
  EXPECT_EQ(file.telemetry.port, def.telemetry.port);
  EXPECT_EQ(file.fc.adapter, def.fc.adapter);
  EXPECT_DOUBLE_EQ(file.home.lat_deg, def.home.lat_deg);
  EXPECT_DOUBLE_EQ(file.mag.north_gauss, def.mag.north_gauss);
  EXPECT_EQ(file.px4.hil_port, def.px4.hil_port);
}

TEST(FcConfig, DefaultFcTomlScriptShape) {
  const Config c = loadConfig(ASTROSIM_FC_TOML, kNoEnv);
  ASSERT_GE(c.pilot.script.size(), 8u);
  EXPECT_FALSE(c.pilot.script.front().input.arm);
  EXPECT_FALSE(c.pilot.script.back().input.arm);
  bool saw_gps = false, saw_nogps = false, saw_armed = false;
  for (const auto& k : c.pilot.script) {
    saw_gps |= k.input.mode == fc::FlightMode::kGpsHold && k.input.arm;
    saw_nogps |= k.input.mode == fc::FlightMode::kNoGpsHold && k.input.arm;
    saw_armed |= k.input.arm;
  }
  EXPECT_TRUE(saw_gps && saw_nogps && saw_armed);
  // It must be accepted by the real pilot and synth constructors too.
  EXPECT_NO_THROW(fc::ScriptedPilot(c.pilot.script, c.pilot.interpolation));
  EXPECT_NO_THROW(fc::TemporarySynthSensors(c.synthConfig()));
}

TEST(FcConfig, ParsesAllSections) {
  const Config c = parseConfig(R"(
[engine]
host = "engine"
port = 9000
[timing]
control_rate_hz = 100
subscribe_interval_s = 1
pilot_rate_hz = 20
fc_timeout_s = 1.5
telemetry_rate_hz = 10
stats_interval_s = 3
[telemetry]
bind = "127.0.0.1"
port = 9001
max_subscribers = 2
subscriber_timeout_s = 2
[fc]
adapter = "null"
[home]
lat_deg = -33.5
lon_deg = 151.25
alt_m = 12
[mag]
east_gauss = 0.1
north_gauss = 0.3
up_gauss = 0.4
[px4]
hil_port = 5000
mavlink_local_port = 14541
mavlink_host = "px4"
mavlink_port = 14581
max_tilt_deg = 30
max_yaw_rate_dps = 60
sensor_dither = false
no_gps_mode = "stabilized"
[log]
level = "debug"
)",
                              kNoEnv);
  EXPECT_EQ(c.engine.host, "engine");
  EXPECT_EQ(c.engine.port, 9000);
  EXPECT_DOUBLE_EQ(c.timing.control_rate_hz, 100);
  EXPECT_DOUBLE_EQ(c.timing.fc_timeout_s, 1.5);
  EXPECT_EQ(c.telemetry.max_subscribers, 2u);
  EXPECT_EQ(c.fc.adapter, "null");
  EXPECT_DOUBLE_EQ(c.home.lat_deg, -33.5);
  EXPECT_EQ(c.px4.mavlink_host, "px4");
  EXPECT_EQ(c.px4.mavlink_local_port, 14541);
  EXPECT_EQ(c.px4.mavlink_port, 14581);
  EXPECT_DOUBLE_EQ(c.px4.max_tilt_deg, 30);
  EXPECT_DOUBLE_EQ(c.px4.max_yaw_rate_dps, 60);
  EXPECT_FALSE(c.px4.sensor_dither);
  EXPECT_EQ(c.px4.no_gps_mode, "stabilized");
  EXPECT_EQ(c.log.level, "debug");
  const fc::SynthConfig s = c.synthConfig();
  EXPECT_DOUBLE_EQ(s.home_lon_deg, 151.25);
  EXPECT_EQ(s.mag_field_enu_gauss, (fc::Vec3{0.1, 0.3, 0.4}));
}

TEST(FcConfig, ParsesPilotScript) {
  const Config c = parseConfig(R"(
[pilot]
interpolation = "linear"
[[pilot.script]]
time_s = 1
[[pilot.script]]
time_s = 2.5
roll = 0.25
pitch = -0.5
yaw = 1
throttle = 0.75
mode = "no_gps_hold"
arm = true
)",
                              kNoEnv);
  EXPECT_EQ(c.pilot.interpolation, fc::Interpolation::kLinear);
  ASSERT_EQ(c.pilot.script.size(), 2u);
  EXPECT_EQ(c.pilot.script[0].input, fc::PilotInput{});  // defaults: zero sticks, gps_hold, disarmed
  EXPECT_DOUBLE_EQ(c.pilot.script[1].time_s, 2.5);
  EXPECT_EQ(c.pilot.script[1].input, (fc::PilotInput{0.25f, -0.5f, 1.f, 0.75f, fc::FlightMode::kNoGpsHold, true}));
}

TEST(FcConfig, EnvOverridesScalarKeys) {
  const Config c = parseConfig("[engine]\nhost = \"a\"\nport = 1\n[timing]\ncontrol_rate_hz = 100\n",
                               envOf({{"ASTROSIM_ENGINE_HOST", "engine.local"},
                                      {"ASTROSIM_ENGINE_PORT", "9999"},
                                      {"ASTROSIM_TIMING_CONTROL_RATE_HZ", "400"},
                                      {"ASTROSIM_TELEMETRY_PORT", "9100"},
                                      {"ASTROSIM_FC_ADAPTER", "null"},
                                      {"ASTROSIM_PX4_NO_GPS_MODE", "stabilized"},
                                      {"ASTROSIM_HOME_LAT_DEG", "10.5"},
                                      {"ASTROSIM_LOG_LEVEL", "warn"}}));
  EXPECT_EQ(c.engine.host, "engine.local");
  EXPECT_EQ(c.engine.port, 9999);
  EXPECT_DOUBLE_EQ(c.timing.control_rate_hz, 400);
  EXPECT_EQ(c.telemetry.port, 9100);
  EXPECT_EQ(c.fc.adapter, "null");
  EXPECT_EQ(c.px4.no_gps_mode, "stabilized");
  EXPECT_DOUBLE_EQ(c.home.lat_deg, 10.5);
  EXPECT_EQ(c.log.level, "warn");
}

TEST(FcConfig, EnvOverridesAreValidatedToo) {
  EXPECT_NE(errorOf("", envOf({{"ASTROSIM_ENGINE_PORT", "70000"}})).find("engine.port"), std::string::npos);
  EXPECT_NE(errorOf("", envOf({{"ASTROSIM_ENGINE_PORT", "abc"}})).find("ASTROSIM_ENGINE_PORT"), std::string::npos);
  EXPECT_NE(errorOf("", envOf({{"ASTROSIM_TIMING_FC_TIMEOUT_S", "x"}})).find("timing.fc_timeout_s"),
            std::string::npos);
  EXPECT_NE(errorOf("", envOf({{"ASTROSIM_FC_SENSOR_SOURCE", "real"}})).find("fc.sensor_source"), std::string::npos);
}

TEST(FcConfig, RejectsBadValues) {
  EXPECT_NE(errorOf("[engine]\nport = 0\n").find("engine.port"), std::string::npos);
  EXPECT_NE(errorOf("[engine]\nhost = \"\"\n").find("engine.host"), std::string::npos);
  EXPECT_NE(errorOf("[timing]\ncontrol_rate_hz = 0\n").find("timing.control_rate_hz"), std::string::npos);
  EXPECT_NE(errorOf("[timing]\ntelemetry_rate_hz = -1\n").find("timing.telemetry_rate_hz"), std::string::npos);
  EXPECT_NE(errorOf("[timing]\nsubscribe_interval_s = 1.5\n").find("timing.subscribe_interval_s"), std::string::npos);
  EXPECT_NE(errorOf("[timing]\nfc_timeout_s = \"x\"\n").find("expected a number"), std::string::npos);
  EXPECT_NE(errorOf("[telemetry]\nbind = \"nope\"\n").find("telemetry.bind"), std::string::npos);
  EXPECT_NE(errorOf("[telemetry]\nmax_subscribers = 0\n").find("telemetry.max_subscribers"), std::string::npos);
  EXPECT_NE(errorOf("[home]\nlat_deg = 91\n").find("home.lat_deg"), std::string::npos);
  EXPECT_NE(errorOf("[home]\nlon_deg = -181\n").find("home.lon_deg"), std::string::npos);
  EXPECT_NE(errorOf("[log]\nlevel = \"loud\"\n").find("log.level"), std::string::npos);
  EXPECT_NE(errorOf("[px4]\nno_gps_mode = \"acro\"\n").find("px4.no_gps_mode"), std::string::npos);
  EXPECT_NE(errorOf("[px4]\nhil_port = 70000\n").find("px4.hil_port"), std::string::npos);
  EXPECT_NE(errorOf("[px4]\nmax_tilt_deg = 90\n").find("px4.max_tilt_deg"), std::string::npos);
  EXPECT_NE(errorOf("[px4]\nmavlink_host = \"\"\n").find("px4.mavlink_host"), std::string::npos);
}

TEST(FcConfig, AdapterAndSensorSourceAreRestricted) {
  EXPECT_EQ(errorOf("[fc]\nadapter = \"px4\"\n"), "");
  EXPECT_EQ(errorOf("[fc]\nadapter = \"null\"\n"), "");
  EXPECT_NE(errorOf("[fc]\nadapter = \"ardupilot\"\n").find("fc.adapter"), std::string::npos);
  EXPECT_EQ(errorOf("[fc]\nsensor_source = \"synthesized_temporary\"\n"), "");
  EXPECT_NE(errorOf("[fc]\nsensor_source = \"engine\"\n").find("fc.sensor_source"), std::string::npos);
}

TEST(FcConfig, RejectsUnknownKeysAndSections) {
  EXPECT_NE(errorOf("[bogus]\nx = 1\n").find("bogus: unknown key"), std::string::npos);
  EXPECT_NE(errorOf("[timing]\nspeed = 1\n").find("timing.speed: unknown key"), std::string::npos);
  EXPECT_NE(errorOf("[pilot]\nextra = 1\n").find("pilot.extra"), std::string::npos);
  EXPECT_NE(errorOf("[[pilot.script]]\ntime_s = 1\nbogus = 2\n").find("pilot.script[0].bogus"), std::string::npos);
  EXPECT_NE(errorOf("[").find("TOML syntax error"), std::string::npos);
}

TEST(FcConfig, RejectsBadPilotScripts) {
  EXPECT_NE(errorOf("[[pilot.script]]\nroll = 0\n").find("pilot.script[0].time_s: required"), std::string::npos);
  EXPECT_NE(errorOf("[[pilot.script]]\ntime_s = 1\nroll = 2\n").find("pilot.script[0].roll"), std::string::npos);
  EXPECT_NE(errorOf("[[pilot.script]]\ntime_s = 1\nthrottle = -0.1\n").find("pilot.script[0].throttle"),
            std::string::npos);
  EXPECT_NE(errorOf("[[pilot.script]]\ntime_s = 1\nmode = \"acro\"\n").find("pilot.script[0].mode"),
            std::string::npos);
  EXPECT_NE(errorOf("[[pilot.script]]\ntime_s = 1\narm = \"yes\"\n").find("pilot.script[0].arm"), std::string::npos);
  EXPECT_NE(errorOf("[[pilot.script]]\ntime_s = 2\n[[pilot.script]]\ntime_s = 2\n").find("pilot.script[1].time_s"),
            std::string::npos);
  EXPECT_NE(errorOf("[pilot]\ninterpolation = \"cubic\"\n").find("pilot.interpolation"), std::string::npos);
  EXPECT_NE(errorOf("[pilot]\nscript = 3\n").find("pilot.script"), std::string::npos);
}

TEST(FcConfig, MissingFileIsAnError) {
  EXPECT_THROW(loadConfig("/nonexistent/fc.toml", kNoEnv), ConfigError);
}

}  // namespace
}  // namespace astrosim::fcbridge
