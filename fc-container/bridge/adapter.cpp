/** @file adapter.cpp @brief Flight computer adapter factory. */
#include "adapter.hpp"

#include <format>

#include "null_adapter.hpp"

namespace astrosim::fcbridge {

std::unique_ptr<fc::FlightComputer> makeAdapter(const Config& config) {
  if (config.fc.adapter == "null") return std::make_unique<NullAdapter>();
  if (config.fc.adapter == "px4") {
#ifdef ASTROSIM_HAVE_PX4
    return makePx4Adapter(config);
#else
    throw ConfigError("fc.adapter: \"px4\" was not compiled in (configure with -DASTROSIM_BUILD_PX4=ON)");
#endif
  }
  throw ConfigError(std::format("fc.adapter: unknown adapter '{}'", config.fc.adapter));
}

}  // namespace astrosim::fcbridge
