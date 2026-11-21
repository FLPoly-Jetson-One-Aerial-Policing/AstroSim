cmake_minimum_required(VERSION 3.16)
project(astrosim_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_astrosim_net_top_level ON)
else()
  set(_astrosim_net_top_level OFF)
endif()

option(ASTROSIM_NET_BUILD_TESTS "Build the astrosim_net tests" ${_astrosim_net_top_level})
option(ASTROSIM_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

# Sibling libraries, when this project is built standalone.
if(NOT TARGET astrosim::proto)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../proto shared/proto)
endif()
if(NOT TARGET astrosim::log)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../log shared/log)
endif()

# Warnings and sanitizers apply to this project's own targets only.
function(astrosim_net_configure target)
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

# transport.cpp is OS-free. udp_transport.cpp is POSIX and container-only, so it is built on UNIX hosts only.
set(_astrosim_net_sources src/transport.cpp)
if(UNIX)
  list(APPEND _astrosim_net_sources src/udp_transport.cpp)
endif()

add_library(astrosim_net ${_astrosim_net_sources})
add_library(astrosim::net ALIAS astrosim_net)
target_include_directories(astrosim_net PUBLIC include)
target_compile_features(astrosim_net PUBLIC cxx_std_20)
target_link_libraries(astrosim_net PUBLIC astrosim::proto astrosim::log)
astrosim_net_configure(astrosim_net)

if(ASTROSIM_NET_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(astrosim_net_tests tests/test_net.cpp)
  target_link_libraries(astrosim_net_tests PRIVATE astrosim::net GTest::gtest_main)
  astrosim_net_configure(astrosim_net_tests)
  gtest_discover_tests(astrosim_net_tests)
endif()
