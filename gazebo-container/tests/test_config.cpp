/**
 * @file test_config.cpp
 * @brief Tests of config parsing, environment overrides and validation errors.
 */
#include <gtest/gtest.h>

#include <map>
#include <string>

#include "config.hpp"

namespace astrosim::gateway {
namespace {

/** @brief Env lookup backed by a map. */
EnvLookup fakeEnv(std::map<std::string, std::string> vars = {}) {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(std::string(name));
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}

/** @brief Eight valid [[motor]] entries. */
std::string motors(int count = 8, bool duplicate_last = false) {
  std::string s;
  for (int i = 0; i < count; ++i) {
    const int idx = (duplicate_last && i == count - 1) ? 0 : i;
    s += "[[motor]]\nactuator_index = " + std::to_string(idx) + "\nmax_rad_s = 800\n";
  }
  return s;
}

/** @brief Assert that parsing @p toml throws a ConfigError whose message contains @p needle. */
void expectError(const std::string& toml, const std::string& needle, const EnvLookup& env = fakeEnv()) {
  try {
    parseConfig(toml, env);
    FAIL() << "expected ConfigError containing '" << needle << "'";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << "actual: " << e.what();
  }
}

TEST(Config, DefaultsFileParsesToDefaults) {
  const Config c = loadConfig(ASTROSIM_GATEWAY_TOML, fakeEnv());
  const Config d;
  EXPECT_EQ(c.network.port, 8765);
  EXPECT_EQ(c.network.bind, "0.0.0.0");
  EXPECT_EQ(c.network.max_subscribers, 8u);
  EXPECT_EQ(c.timing.state_rate_hz, 60);
  EXPECT_EQ(c.timing.deadman_timeout_s, 0.5);
  EXPECT_EQ(c.timing.lease_timeout_s, 1.0);
  EXPECT_EQ(c.timing.subscriber_timeout_s, 5);
  EXPECT_EQ(c.timing.engine_stale_s, 0.5);
  EXPECT_EQ(c.timing.stats_interval_s, 10);
  EXPECT_EQ(c.engine.model, "jetson_one");
  EXPECT_EQ(c.engine.odometry_topic, d.engine.odometry_topic);
  EXPECT_EQ(c.engine.motor_command_topic, d.engine.motor_command_topic);
  EXPECT_EQ(c.engine.motor_speed_topic, d.engine.motor_speed_topic);
  EXPECT_EQ(c.engine.motor_speed_topic, "/jetson_one/joint_state");
  EXPECT_EQ(c.log.level, "info");
  for (std::size_t i = 0; i < c.motors.size(); ++i) {
    EXPECT_EQ(c.motors[i].actuator_index, static_cast<int>(i));
    EXPECT_EQ(c.motors[i].max_rad_s, 800);
  }
}

TEST(Config, MissingFileIsError) {
  EXPECT_THROW(loadConfig("/nonexistent/gateway.toml", fakeEnv()), ConfigError);
}

TEST(Config, FileValuesOverrideDefaults) {
  const Config c = parseConfig("[network]\nport = 9000\n[timing]\ndeadman_timeout_s = 1\n" + motors(), fakeEnv());
  EXPECT_EQ(c.network.port, 9000);
  EXPECT_EQ(c.timing.deadman_timeout_s, 1.0);
}

TEST(Config, EnvOverridesFile) {
  const Config c =
      parseConfig("[network]\nport = 9000\n" + motors(), fakeEnv({{"ASTROSIM_NETWORK_PORT", "9100"},
                                                                  {"ASTROSIM_NETWORK_BIND", "127.0.0.1"},
                                                                  {"ASTROSIM_TIMING_DEADMAN_TIMEOUT_S", "0.25"},
                                                                  {"ASTROSIM_ENGINE_MOTOR_SPEED_TOPIC", "/x"},
                                                                  {"ASTROSIM_LOG_LEVEL", "debug"}}));
  EXPECT_EQ(c.network.port, 9100);
  EXPECT_EQ(c.network.bind, "127.0.0.1");
  EXPECT_EQ(c.timing.deadman_timeout_s, 0.25);
  EXPECT_EQ(c.engine.motor_speed_topic, "/x");
  EXPECT_EQ(c.log.level, "debug");
}

TEST(Config, MalformedEnvRejected) {
  expectError(motors(), "network.port", fakeEnv({{"ASTROSIM_NETWORK_PORT", "abc"}}));
  expectError(motors(), "network.port", fakeEnv({{"ASTROSIM_NETWORK_PORT", "80x"}}));
  expectError(motors(), "timing.state_rate_hz", fakeEnv({{"ASTROSIM_TIMING_STATE_RATE_HZ", "fast"}}));
  expectError(motors(), "timing.engine_stale_s", fakeEnv({{"ASTROSIM_TIMING_ENGINE_STALE_S", "nan"}}));
  expectError(motors(), "network.port", fakeEnv({{"ASTROSIM_NETWORK_PORT", "70000"}}));
}

TEST(Config, MotorFeedbackKeys) {
  expectError("[engine]\nmotor_joint_name = \"rotor\"\n" + motors(), "engine.motor_joint_name: must contain");
  expectError("[engine]\nmotor_speed_scale = 0\n" + motors(), "engine.motor_speed_scale");
  const Config c = parseConfig("[engine]\nmotor_speed_topic = \"\"\nmotor_speed_scale = 2.5\n" + motors(), fakeEnv());
  EXPECT_EQ(c.engine.motor_speed_topic, "");
  EXPECT_EQ(c.engine.motor_speed_scale, 2.5);
  EXPECT_EQ(c.engine.motor_joint_name, "jetson_one/rotor_{actuator}_joint");
}

TEST(Config, ValidationMessagesNameTheKey) {
  expectError("[network]\nport = 0\n" + motors(), "network.port: must be in 1..65535 (got 0)");
  expectError("[network]\nport = 65536\n" + motors(), "network.port");
  expectError("[network]\nmax_subscribers = 0\n" + motors(), "network.max_subscribers: must be >= 1");
  expectError("[timing]\ndeadman_timeout_s = -1\n" + motors(),
              "timing.deadman_timeout_s: must be > 0 and finite (got -1)");
  expectError("[timing]\nstate_rate_hz = 0\n" + motors(), "timing.state_rate_hz");
  expectError("[timing]\nlease_timeout_s = 0\n" + motors(), "timing.lease_timeout_s");
  expectError("[timing]\nsubscriber_timeout_s = -5\n" + motors(), "timing.subscriber_timeout_s");
  expectError("[timing]\nengine_stale_s = 0\n" + motors(), "timing.engine_stale_s");
  expectError("[timing]\nstats_interval_s = 0\n" + motors(), "timing.stats_interval_s");
  expectError("[timing]\nstats_interval_s = inf\n" + motors(), "timing.stats_interval_s");
  expectError("[network]\nport = \"x\"\n" + motors(), "network.port: expected an integer");
}

TEST(Config, UnknownKeysRejected) {
  expectError("[network]\nprot = 1\n" + motors(), "network.prot: unknown key");
  expectError("[netwrk]\nport = 1\n" + motors(), "netwrk: unknown key");
  expectError("[[motor]]\nactuator_index = 0\nmax_rad_s = 1\nextra = 1\n" + motors(7), "motor[0].extra: unknown key");
}

TEST(Config, MotorValidation) {
  expectError("", "motor");
  expectError(motors(7), "expected exactly 8 [[motor]] entries (got 7)");
  expectError(motors(9), "got 9");
  expectError(motors(8, true), "motor[7].actuator_index: must be unique");
  expectError("[[motor]]\nactuator_index = -1\nmax_rad_s = 800\n" + motors(7), "motor[0].actuator_index: must be >= 0");
  expectError("[[motor]]\nactuator_index = 0\nmax_rad_s = 0\n" + motors(7), "motor[0].max_rad_s: must be > 0");
  expectError("[[motor]]\nactuator_index = 0\n" + motors(7), "motor[0].max_rad_s");
}

TEST(Config, BadBindRejected) {
  for (const char* bad : {"localhost", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.-4", "", "1..2.3", " 1.2.3.4"}) {
    expectError(std::string("[network]\nbind = \"") + bad + "\"\n" + motors(), "network.bind");
  }
  EXPECT_NO_THROW(parseConfig("[network]\nbind = \"192.168.1.20\"\n" + motors(), fakeEnv()));
}

TEST(Config, BadLogLevelRejected) {
  expectError("[log]\nlevel = \"verbose\"\n" + motors(), "log.level: must be one of");
  expectError(motors(), "log.level", fakeEnv({{"ASTROSIM_LOG_LEVEL", "LOUD"}}));
}

TEST(Config, SyntaxErrorReported) {
  expectError("[network\n", "TOML syntax error");
}

}  // namespace
}  // namespace astrosim::gateway
