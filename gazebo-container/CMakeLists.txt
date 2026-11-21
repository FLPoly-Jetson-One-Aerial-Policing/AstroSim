cmake_minimum_required(VERSION 3.20)
project(astrosim_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ASTROSIM_BUILD_TESTS "Build and register unit tests (including shared/proto)" ON)
option(ASTROSIM_BUILD_GZ "Build gz-dependent binaries (gateway, astrosim-probe)" OFF)

# Applied globally, before add_subdirectory, so shared/proto is sanitized too.
if(ASTROSIM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

if(ASTROSIM_BUILD_TESTS)
  enable_testing()
  set(ASTROSIM_PROTO_BUILD_TESTS ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/proto shared/proto)

find_package(tomlplusplus REQUIRED)

# Strict warnings on our own targets only.
function(astrosim_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

add_library(astrosim_gateway_core STATIC
  gateway/config.cpp
  gateway/gateway.cpp
  gateway/log.cpp
  gateway/transport.cpp)
target_include_directories(astrosim_gateway_core PUBLIC gateway)
target_link_libraries(astrosim_gateway_core PUBLIC astrosim::proto tomlplusplus::tomlplusplus)
astrosim_warnings(astrosim_gateway_core)

if(ASTROSIM_BUILD_TESTS)
  find_package(GTest REQUIRED)
  include(GoogleTest)
  foreach(name test_gateway test_config)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE astrosim_gateway_core GTest::gtest_main)
    astrosim_warnings(${name})
    gtest_discover_tests(${name})
  endforeach()
  target_compile_definitions(test_config PRIVATE
    ASTROSIM_GATEWAY_TOML="${CMAKE_CURRENT_SOURCE_DIR}/config/gateway.toml")
endif()

if(ASTROSIM_BUILD_GZ)
  find_package(gz-transport REQUIRED)
  find_package(gz-msgs REQUIRED)
  find_package(gz-math REQUIRED)

  add_executable(gateway gateway/main.cpp gateway/gz_engine.cpp gateway/udp_transport.cpp)
  target_link_libraries(gateway PRIVATE astrosim_gateway_core
    gz-transport::gz-transport gz-msgs::gz-msgs gz-math::gz-math)
  astrosim_warnings(gateway)

  # gz-free: only proto + POSIX sockets.
  add_executable(astrosim-probe gateway/probe.cpp)
  target_link_libraries(astrosim-probe PRIVATE astrosim::proto)
  astrosim_warnings(astrosim-probe)

  include(GNUInstallDirs)
  install(TARGETS gateway astrosim-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
