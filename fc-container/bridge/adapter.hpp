/**
 * @file adapter.hpp
 * @brief The flight computer adapter factory: the seam where a concrete FlightComputer is chosen by config.
 *
 * Built in: `adapter = "null"` (NullAdapter, TEST ONLY). `adapter = "px4"` is provided by the PX4 adapter library
 * (fc-container/px4), which is compiled in when CMake option ASTROSIM_BUILD_PX4 is ON; that library must define
 * the free function declared below. To add another flight computer, add a name to Config::Fc validation, a branch
 * in makeAdapter() and an implementation of fc::FlightComputer.
 */
#pragma once
#include <memory>

#include "astrosim/fc/flight_computer.hpp"
#include "config.hpp"

namespace astrosim::fcbridge {

#ifdef ASTROSIM_HAVE_PX4
/**
 * @brief Create the PX4 SITL adapter. DEFINED BY THE PX4 ADAPTER LIBRARY (fc-container/px4), declared here.
 * @param config Validated bridge configuration; the adapter reads Config::px4 (and may read other sections).
 * @return A new adapter. It must not block in its constructor beyond opening its listening sockets.
 * @throws std::exception on failure (for example a port that cannot be bound); main() reports it as fatal.
 */
std::unique_ptr<fc::FlightComputer> makePx4Adapter(const Config& config);
#endif

/**
 * @brief Create the flight computer named by Config::fc.adapter.
 * @param config Validated bridge configuration.
 * @return The adapter.
 * @throws ConfigError if the named adapter was not compiled in (for example "px4" without ASTROSIM_BUILD_PX4).
 */
std::unique_ptr<fc::FlightComputer> makeAdapter(const Config& config);

}  // namespace astrosim::fcbridge
