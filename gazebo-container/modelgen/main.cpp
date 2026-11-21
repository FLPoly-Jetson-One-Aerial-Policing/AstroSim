/**
 * @file main.cpp
 * @brief astrosim-modelgen: writes the Gazebo model generated from an aircraft file.
 */
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "gz_names.hpp"
#include "sdf_model.hpp"

namespace {

/** @brief Print usage to @p out. */
void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: astrosim-modelgen --aircraft PATH --out DIR [--help]\n"
               "  --aircraft PATH  aircraft TOML file (required)\n"
               "  --out DIR        output directory; writes DIR/%s/model.sdf and model.config (required)\n",
               astrosim::gznames::kModelName);
}

/**
 * @brief Write a text file.
 * @param path Destination.
 * @param text Contents.
 * @return True on success.
 */
bool writeFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  f.flush();
  return static_cast<bool>(f);
}

}  // namespace

/**
 * @brief Entry point.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on success (or --help), 2 for bad arguments or an invalid aircraft file, 1 for an I/O failure.
 */
int main(int argc, char** argv) {
  std::string aircraft_path, out_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    if (a == "--aircraft" && i + 1 < argc) {
      aircraft_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_dir = argv[++i];
    } else {
      std::fprintf(stderr, "astrosim-modelgen: unknown or incomplete argument '%s'\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }
  if (aircraft_path.empty() || out_dir.empty()) {
    std::fprintf(stderr, "astrosim-modelgen: --aircraft and --out are required\n");
    usage(stderr);
    return 2;
  }

  astrosim::modelgen::GeneratedModel model;
  try {
    model = astrosim::modelgen::generateModel(astrosim::aircraft::loadAircraft(aircraft_path));
  } catch (const astrosim::aircraft::AircraftError& e) {
    std::fprintf(stderr, "astrosim-modelgen: aircraft error: %s\n", e.what());
    return 2;
  }

  try {
    const std::filesystem::path dir = std::filesystem::path(out_dir) / astrosim::gznames::kModelName;
    std::filesystem::create_directories(dir);
    if (!writeFile(dir / "model.sdf", model.sdf) || !writeFile(dir / "model.config", model.config)) {
      std::fprintf(stderr, "astrosim-modelgen: cannot write into %s\n", dir.string().c_str());
      return 1;
    }
    std::printf("astrosim-modelgen: wrote %s\n", dir.string().c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "astrosim-modelgen: I/O error: %s\n", e.what());
    return 1;
  }
  return 0;
}
