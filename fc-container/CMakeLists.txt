cmake_minimum_required(VERSION 3.20)
project(astrosim_fc_container LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ASTROSIM_BUILD_TESTS "Build and register unit tests (including shared/proto, aircraft, log, net and fc)" ON)
# The PX4 adapter lives in fc-container/px4 (a separate change). When ON, that directory must define the target
# astrosim_px4_adapter exposing `makePx4Adapter(const Config&)` (see bridge/adapter.hpp).
option(ASTROSIM_BUILD_PX4 "Build the PX4 SITL adapter (fc-container/px4)" OFF)

# Applied globally, before add_subdirectory, so the shared libraries are sanitized too.
if(ASTROSIM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

if(ASTROSIM_BUILD_TESTS)
  enable_testing()
  set(ASTROSIM_PROTO_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_AIRCRAFT_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_LOG_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_NET_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_FC_BUILD_TESTS ON CACHE BOOL "" FORCE)
endif()

find_package(tomlplusplus REQUIRED)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/proto shared/proto)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/aircraft shared/aircraft)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/log shared/log)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/net shared/net)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/fc shared/fc)
set(ASTROSIM_AIRCRAFT_TOML ${CMAKE_CURRENT_SOURCE_DIR}/../shared/aircraft/jetson_one.toml)

# Strict warnings on our own targets only.
function(astrosim_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

# Pure logic: FcBridge, configuration, adapter factory, null adapter. No sockets, no clock reads.
add_library(astrosim_fc_bridge_core STATIC
  bridge/adapter.cpp
  bridge/config.cpp
  bridge/fc_bridge.cpp)
target_include_directories(astrosim_fc_bridge_core PUBLIC bridge)
target_link_libraries(astrosim_fc_bridge_core PUBLIC
  astrosim::proto astrosim::aircraft astrosim::log astrosim::net astrosim::fc tomlplusplus::tomlplusplus)
astrosim_warnings(astrosim_fc_bridge_core)

if(ASTROSIM_BUILD_PX4)
  add_subdirectory(px4)
  target_link_libraries(astrosim_fc_bridge_core PUBLIC astrosim_px4_adapter)
  target_compile_definitions(astrosim_fc_bridge_core PUBLIC ASTROSIM_HAVE_PX4=1)
endif()

# Executables are POSIX-only (container-only): UdpTransport and poll().
add_executable(astrosim-fc-bridge bridge/main.cpp)
target_link_libraries(astrosim-fc-bridge PRIVATE astrosim_fc_bridge_core)
astrosim_warnings(astrosim-fc-bridge)

# gz-free: only fc telemetry proto + POSIX sockets.
add_executable(astrosim-fc-probe bridge/probe.cpp)
target_link_libraries(astrosim-fc-probe PRIVATE astrosim::proto)
astrosim_warnings(astrosim-fc-probe)

include(GNUInstallDirs)
install(TARGETS astrosim-fc-bridge astrosim-fc-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ASTROSIM_BUILD_TESTS)
  find_package(GTest REQUIRED)
  include(GoogleTest)
  foreach(name test_fc_bridge test_fc_config)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE astrosim_fc_bridge_core GTest::gtest_main)
    astrosim_warnings(${name})
    gtest_discover_tests(${name})
  endforeach()
  target_compile_definitions(test_fc_config PRIVATE
    ASTROSIM_FC_TOML="${CMAKE_CURRENT_SOURCE_DIR}/config/fc.toml")
endif()
