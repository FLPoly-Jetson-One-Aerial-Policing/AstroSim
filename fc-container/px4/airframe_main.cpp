/**
 * @file airframe_main.cpp
 * @brief astrosim-px4-airframe: writes the PX4 airframe script generated from an aircraft file.
 */
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "airframe.hpp"
#include "config.hpp"

namespace {

/** @brief Print usage to @p out. */
void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: astrosim-px4-airframe --aircraft PATH --out DIR [--config PATH] [--help]\n"
               "       astrosim-px4-airframe --print-autostart-id\n"
               "  --aircraft PATH       aircraft TOML file (required)\n"
               "  --out DIR             directory to write %d_%s into (PX4's etc/init.d-posix/airframes)\n"
               "  --config PATH         bridge configuration; its [px4] max_tilt_deg and max_yaw_rate_dps are used\n"
               "                        (ASTROSIM_PX4_* overrides apply). Default values if omitted.\n"
               "  --print-autostart-id  print the SYS_AUTOSTART id of the generated airframe and exit\n",
               astrosim::px4::kAirframeAutostartId, astrosim::px4::kAirframeModelName);
}

}  // namespace

/**
 * @brief Entry point.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on success, 2 for bad arguments, configuration or aircraft file, 1 for an I/O failure.
 */
int main(int argc, char** argv) {
  std::string aircraft_path, out_dir, config_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    if (a == "--print-autostart-id") {
      std::printf("%d\n", astrosim::px4::kAirframeAutostartId);
      return 0;
    }
    if (a == "--aircraft" && i + 1 < argc) {
      aircraft_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      std::fprintf(stderr, "astrosim-px4-airframe: unknown or incomplete argument '%s'\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }
  if (aircraft_path.empty() || out_dir.empty()) {
    std::fprintf(stderr, "astrosim-px4-airframe: --aircraft and --out are required\n");
    usage(stderr);
    return 2;
  }

  astrosim::px4::Airframe airframe;
  try {
    astrosim::px4::AirframeOptions options;
    if (!config_path.empty()) {
      const astrosim::fcbridge::Config cfg =
          astrosim::fcbridge::loadConfig(config_path, astrosim::fcbridge::processEnv);
      options.max_tilt_deg = cfg.px4.max_tilt_deg;
      options.max_yaw_rate_dps = cfg.px4.max_yaw_rate_dps;
    }
    airframe = astrosim::px4::generateAirframe(astrosim::aircraft::loadAircraft(aircraft_path), options);
  } catch (const astrosim::aircraft::AircraftError& e) {
    std::fprintf(stderr, "astrosim-px4-airframe: aircraft error: %s\n", e.what());
    return 2;
  } catch (const astrosim::fcbridge::ConfigError& e) {
    std::fprintf(stderr, "astrosim-px4-airframe: configuration error: %s\n", e.what());
    return 2;
  } catch (const std::invalid_argument& e) {
    std::fprintf(stderr, "astrosim-px4-airframe: %s\n", e.what());
    return 2;
  }

  try {
    const std::filesystem::path path = std::filesystem::path(out_dir) / airframe.file_name;
    std::filesystem::create_directories(out_dir);
    std::ofstream f(path, std::ios::binary);
    f << airframe.text;
    f.flush();
    if (!f) {
      std::fprintf(stderr, "astrosim-px4-airframe: cannot write %s\n", path.string().c_str());
      return 1;
    }
    std::printf("astrosim-px4-airframe: wrote %s (hover thrust %.3f)\n", path.string().c_str(), airframe.hover_thrust);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "astrosim-px4-airframe: I/O error: %s\n", e.what());
    return 1;
  }
  return 0;
}
