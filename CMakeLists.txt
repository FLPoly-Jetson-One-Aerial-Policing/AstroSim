cmake_minimum_required(VERSION 3.22)
project(astrosim CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Container build: -DASTROSIM_BUILD_GATEWAY=ON (gz Jetty). Host build: default (no gz).
option(ASTROSIM_BUILD_GATEWAY "Build gz-dependent targets" OFF)

if(ASTROSIM_BUILD_GATEWAY)
  find_package(gz-transport REQUIRED)
  find_package(gz-msgs REQUIRED)
  add_executable(bench_rtf tools/bench_rtf.cpp)
  target_link_libraries(bench_rtf gz-transport::gz-transport gz-msgs::gz-msgs)
endif()
