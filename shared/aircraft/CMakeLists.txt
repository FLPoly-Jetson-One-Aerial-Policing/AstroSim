cmake_minimum_required(VERSION 3.16)
project(astrosim_aircraft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_astrosim_aircraft_top_level ON)
else()
  set(_astrosim_aircraft_top_level OFF)
endif()

option(ASTROSIM_AIRCRAFT_BUILD_TESTS "Build the astrosim_aircraft tests" ${_astrosim_aircraft_top_level})
option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

# Warnings and sanitizers apply to this project's own targets only.
function(astrosim_aircraft_configure target)
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

find_package(tomlplusplus REQUIRED)

add_library(astrosim_aircraft src/aircraft.cpp src/mass.cpp)
add_library(astrosim::aircraft ALIAS astrosim_aircraft)
target_include_directories(astrosim_aircraft PUBLIC include)
target_compile_features(astrosim_aircraft PUBLIC cxx_std_20)
target_link_libraries(astrosim_aircraft PUBLIC tomlplusplus::tomlplusplus)
astrosim_aircraft_configure(astrosim_aircraft)

if(ASTROSIM_AIRCRAFT_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(astrosim_aircraft_tests tests/test_aircraft.cpp tests/test_mass.cpp)
  target_link_libraries(astrosim_aircraft_tests PRIVATE astrosim::aircraft GTest::gtest_main)
  target_compile_definitions(astrosim_aircraft_tests PRIVATE
    ASTROSIM_AIRCRAFT_TOML="${CMAKE_CURRENT_SOURCE_DIR}/jetson_one.toml")
  astrosim_aircraft_configure(astrosim_aircraft_tests)
  gtest_discover_tests(astrosim_aircraft_tests)
endif()
