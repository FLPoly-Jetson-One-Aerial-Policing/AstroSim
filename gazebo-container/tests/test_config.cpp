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
  EXPECT_EQ(c.network.port, 8765);
  EXPECT_EQ(c.network.bind, "0.0.0.0");
  EXPECT_EQ(c.network.max_subscribers, 8u);
  EXPECT_EQ(c.timing.state_rate_hz, 250);
  EXPECT_EQ(c.timing.deadman_timeout_s, 0.5);
  EXPECT_EQ(c.timing.lease_timeout_s, 1.0);
  EXPECT_EQ(c.timing.subscriber_timeout_s, 5);
  EXPECT_EQ(c.timing.engine_stale_s, 0.5);
  EXPECT_EQ(c.timing.stats_interval_s, 10);
  EXPECT_EQ(c.log.level, "info");
}

TEST(Config, MissingFileIsError) {
  EXPECT_THROW(loadConfig("/nonexistent/gateway.toml", fakeEnv()), ConfigError);
}

TEST(Config, FileValuesOverrideDefaults) {
  const Config c = parseConfig("[network]\nport = 9000\n[timing]\ndeadman_timeout_s = 1\n", fakeEnv());
  EXPECT_EQ(c.network.port, 9000);
  EXPECT_EQ(c.timing.deadman_timeout_s, 1.0);
}

TEST(Config, EnvOverridesFile) {
  const Config c = parseConfig("[network]\nport = 9000\n", fakeEnv({{"ASTROSIM_NETWORK_PORT", "9100"},
                                                                    {"ASTROSIM_NETWORK_BIND", "127.0.0.1"},
                                                                    {"ASTROSIM_TIMING_DEADMAN_TIMEOUT_S", "0.25"},
                                                                    {"ASTROSIM_LOG_LEVEL", "debug"}}));
  EXPECT_EQ(c.network.port, 9100);
  EXPECT_EQ(c.network.bind, "127.0.0.1");
  EXPECT_EQ(c.timing.deadman_timeout_s, 0.25);
  EXPECT_EQ(c.log.level, "debug");
}

TEST(Config, MalformedEnvRejected) {
  expectError("", "network.port", fakeEnv({{"ASTROSIM_NETWORK_PORT", "abc"}}));
  expectError("", "network.port", fakeEnv({{"ASTROSIM_NETWORK_PORT", "80x"}}));
  expectError("", "timing.state_rate_hz", fakeEnv({{"ASTROSIM_TIMING_STATE_RATE_HZ", "fast"}}));
  expectError("", "timing.engine_stale_s", fakeEnv({{"ASTROSIM_TIMING_ENGINE_STALE_S", "nan"}}));
  expectError("", "network.port", fakeEnv({{"ASTROSIM_NETWORK_PORT", "70000"}}));
}

TEST(Config, ValidationMessagesNameTheKey) {
  expectError("[network]\nport = 0\n", "network.port: must be in 1..65535 (got 0)");
  expectError("[network]\nport = 65536\n", "network.port");
  expectError("[network]\nmax_subscribers = 0\n", "network.max_subscribers: must be >= 1");
  expectError("[timing]\ndeadman_timeout_s = -1\n", "timing.deadman_timeout_s: must be > 0 and finite (got -1)");
  expectError("[timing]\nstate_rate_hz = 0\n", "timing.state_rate_hz");
  expectError("[timing]\nlease_timeout_s = 0\n", "timing.lease_timeout_s");
  expectError("[timing]\nsubscriber_timeout_s = -5\n", "timing.subscriber_timeout_s");
  expectError("[timing]\nengine_stale_s = 0\n", "timing.engine_stale_s");
  expectError("[timing]\nstats_interval_s = 0\n", "timing.stats_interval_s");
  expectError("[timing]\nstats_interval_s = inf\n", "timing.stats_interval_s");
  expectError("[network]\nport = \"x\"\n", "network.port: expected an integer");
}

TEST(Config, UnknownKeysRejected) {
  expectError("[network]\nprot = 1\n", "network.prot: unknown key");
  expectError("[netwrk]\nport = 1\n", "netwrk: unknown key");
  // Sections that moved to the aircraft file are rejected, so a stale config fails loudly.
  expectError("[engine]\nmodel = \"x\"\n", "engine: unknown key");
  expectError("[[motor]]\nactuator_index = 0\nmax_rad_s = 800\n", "motor: unknown key");
}

TEST(Config, BadBindRejected) {
  for (const char* bad : {"localhost", "1.2.3", "1.2.3.4.5", "256.0.0.1", "1.2.3.-4", "", "1..2.3", " 1.2.3.4"}) {
    expectError(std::string("[network]\nbind = \"") + bad + "\"\n", "network.bind");
  }
  EXPECT_NO_THROW(parseConfig("[network]\nbind = \"192.168.1.20\"\n", fakeEnv()));
}

TEST(Config, BadLogLevelRejected) {
  expectError("[log]\nlevel = \"verbose\"\n", "log.level: must be one of");
  expectError("", "log.level", fakeEnv({{"ASTROSIM_LOG_LEVEL", "LOUD"}}));
}

TEST(Config, SyntaxErrorReported) {
  expectError("[network\n", "TOML syntax error");
}

}  // namespace
}  // namespace astrosim::gateway
