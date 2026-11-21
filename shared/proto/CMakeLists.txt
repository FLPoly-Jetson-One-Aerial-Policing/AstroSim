cmake_minimum_required(VERSION 3.16)
project(astrosim_proto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_astrosim_proto_top_level ON)
else()
  set(_astrosim_proto_top_level OFF)
endif()

option(ASTROSIM_PROTO_BUILD_TESTS "Build the astrosim_proto tests" ${_astrosim_proto_top_level})
option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

# Warnings and sanitizers apply to this project's own targets only.
function(astrosim_proto_configure target)
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

add_library(astrosim_proto src/wire.cpp src/fc_telemetry.cpp)
add_library(astrosim::proto ALIAS astrosim_proto)
target_include_directories(astrosim_proto PUBLIC include)
target_compile_features(astrosim_proto PUBLIC cxx_std_20)
astrosim_proto_configure(astrosim_proto)

if(ASTROSIM_PROTO_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(astrosim_proto_tests tests/test_wire.cpp tests/test_fc_telemetry.cpp)
  target_link_libraries(astrosim_proto_tests PRIVATE astrosim::proto GTest::gtest_main)
  target_compile_definitions(astrosim_proto_tests PRIVATE
    GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
  astrosim_proto_configure(astrosim_proto_tests)
  gtest_discover_tests(astrosim_proto_tests)
endif()
