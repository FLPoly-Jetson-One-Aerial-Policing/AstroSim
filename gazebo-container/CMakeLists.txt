cmake_minimum_required(VERSION 3.20)
project(astrosim_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ASTROSIM_BUILD_TESTS "Build and register unit tests (including shared/proto, log and net)" ON)
option(ASTROSIM_BUILD_SDF_TEST "Build the sdformat load test of the generated model (needs libsdformat)" OFF)
option(ASTROSIM_BUILD_GZ "Build gz-dependent binaries (gateway, astrosim-probe)" OFF)

# Applied globally, before add_subdirectory, so shared/proto is sanitized too.
if(ASTROSIM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

if(ASTROSIM_BUILD_TESTS)
  enable_testing()
  set(ASTROSIM_PROTO_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_LOG_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_NET_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(ASTROSIM_AIRCRAFT_BUILD_TESTS ON CACHE BOOL "" FORCE)
endif()

find_package(tomlplusplus REQUIRED)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/proto shared/proto)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/aircraft shared/aircraft)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/log shared/log)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/net shared/net)
set(ASTROSIM_AIRCRAFT_TOML ${CMAKE_CURRENT_SOURCE_DIR}/../shared/aircraft/jetson_one.toml)

# Strict warnings on our own targets only.
function(astrosim_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

# Model generator: aircraft file -> Gazebo SDF text. gz-free; always built.
add_library(astrosim_modelgen_core STATIC modelgen/sdf_model.cpp)
target_include_directories(astrosim_modelgen_core PUBLIC modelgen)
target_link_libraries(astrosim_modelgen_core PUBLIC astrosim::aircraft)
astrosim_warnings(astrosim_modelgen_core)

add_executable(astrosim-modelgen modelgen/main.cpp)
target_link_libraries(astrosim-modelgen PRIVATE astrosim_modelgen_core)
astrosim_warnings(astrosim-modelgen)
include(GNUInstallDirs)
install(TARGETS astrosim-modelgen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(astrosim_gateway_core STATIC
  gateway/binding.cpp
  gateway/config.cpp
  gateway/gateway.cpp)
target_include_directories(astrosim_gateway_core PUBLIC gateway)
target_link_libraries(astrosim_gateway_core PUBLIC astrosim::proto astrosim::aircraft astrosim::log astrosim::net tomlplusplus::tomlplusplus)
# gz_names.hpp is shared with the generator (header-only, gz-free).
target_include_directories(astrosim_gateway_core PUBLIC modelgen)
astrosim_warnings(astrosim_gateway_core)

if(ASTROSIM_BUILD_TESTS)
  find_package(GTest REQUIRED)
  include(GoogleTest)
  foreach(name test_gateway test_config test_binding)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE astrosim_gateway_core GTest::gtest_main)
    astrosim_warnings(${name})
    gtest_discover_tests(${name})
  endforeach()
  target_compile_definitions(test_config PRIVATE
    ASTROSIM_GATEWAY_TOML="${CMAKE_CURRENT_SOURCE_DIR}/config/gateway.toml")
  target_compile_definitions(test_binding PRIVATE ASTROSIM_AIRCRAFT_TOML="${ASTROSIM_AIRCRAFT_TOML}")

  add_executable(test_modelgen tests/test_modelgen.cpp)
  target_link_libraries(test_modelgen PRIVATE astrosim_modelgen_core GTest::gtest_main)
  target_compile_definitions(test_modelgen PRIVATE ASTROSIM_AIRCRAFT_TOML="${ASTROSIM_AIRCRAFT_TOML}")
  astrosim_warnings(test_modelgen)
  gtest_discover_tests(test_modelgen)

  if(ASTROSIM_BUILD_SDF_TEST)
    find_package(sdformat REQUIRED)
    add_executable(test_sdf_load tests/test_sdf_load.cpp)
    target_link_libraries(test_sdf_load PRIVATE astrosim_modelgen_core GTest::gtest_main sdformat::sdformat)
    target_compile_definitions(test_sdf_load PRIVATE ASTROSIM_AIRCRAFT_TOML="${ASTROSIM_AIRCRAFT_TOML}")
    astrosim_warnings(test_sdf_load)
    gtest_discover_tests(test_sdf_load)
  endif()
endif()

if(ASTROSIM_BUILD_GZ)
  find_package(gz-transport REQUIRED)
  find_package(gz-msgs REQUIRED)
  find_package(gz-math REQUIRED)

  add_executable(gateway gateway/main.cpp gateway/gz_engine.cpp)
  target_link_libraries(gateway PRIVATE astrosim_gateway_core
    gz-transport::gz-transport gz-msgs::gz-msgs gz-math::gz-math)
  astrosim_warnings(gateway)

  # gz-free: only proto + POSIX sockets.
  add_executable(astrosim-probe gateway/probe.cpp)
  target_link_libraries(astrosim-probe PRIVATE astrosim::proto)
  astrosim_warnings(astrosim-probe)

  install(TARGETS gateway astrosim-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
