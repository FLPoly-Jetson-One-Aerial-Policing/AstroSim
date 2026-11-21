cmake_minimum_required(VERSION 3.16)
project(astrosim_fc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_astrosim_fc_top_level ON)
else()
  set(_astrosim_fc_top_level OFF)
endif()

option(ASTROSIM_FC_BUILD_TESTS "Build the astrosim_fc tests" ${_astrosim_fc_top_level})
option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

# The wire types (State, FC telemetry) come from the sibling proto library when built standalone.
if(NOT TARGET astrosim::proto)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../proto shared/proto)
endif()

# Warnings and sanitizers apply to this project's own targets only.
function(astrosim_fc_configure target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(ASTROSIM_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# Pure portable C++20: no OS headers, no MAVLink.
add_library(astrosim_fc src/synth_sensors_TEMPORARY.cpp src/scripted_pilot.cpp)
add_library(astrosim::fc ALIAS astrosim_fc)
target_include_directories(astrosim_fc PUBLIC include)
target_compile_features(astrosim_fc PUBLIC cxx_std_20)
target_link_libraries(astrosim_fc PUBLIC astrosim::proto)
astrosim_fc_configure(astrosim_fc)

if(ASTROSIM_FC_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(astrosim_fc_tests tests/test_frames.cpp tests/test_synth_sensors.cpp tests/test_scripted_pilot.cpp)
  target_link_libraries(astrosim_fc_tests PRIVATE astrosim::fc GTest::gtest_main)
  astrosim_fc_configure(astrosim_fc_tests)
  gtest_discover_tests(astrosim_fc_tests)
endif()
