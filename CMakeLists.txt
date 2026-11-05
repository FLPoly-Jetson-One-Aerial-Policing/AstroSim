cmake_minimum_required(VERSION 3.22)
project(astrosim CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# Host-buildable protocol library (no gz deps).
add_library(astrosim_proto STATIC proto/wire.cpp)
target_include_directories(astrosim_proto PUBLIC proto)
add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire astrosim_proto)
target_compile_definitions(test_wire PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/proto/testdata")
add_test(NAME wire COMMAND test_wire)

add_library(astrosim_gateway_core STATIC gateway/gateway.cpp)
target_include_directories(astrosim_gateway_core PUBLIC gateway)
target_link_libraries(astrosim_gateway_core PUBLIC astrosim_proto)
add_executable(test_gateway tests/test_gateway.cpp)
target_link_libraries(test_gateway astrosim_gateway_core pthread)
add_test(NAME gateway COMMAND test_gateway)

add_executable(climb tools/climb.cpp)
target_link_libraries(climb astrosim_proto)

# Container build: -DASTROSIM_BUILD_GATEWAY=ON (gz Jetty). Host build: default (no gz).
option(ASTROSIM_BUILD_GATEWAY "Build gz-dependent targets" OFF)

if(ASTROSIM_BUILD_GATEWAY)
  find_package(gz-transport REQUIRED)
  find_package(gz-msgs REQUIRED)
  add_executable(bench_rtf tools/bench_rtf.cpp)
  target_link_libraries(bench_rtf gz-transport::gz-transport gz-msgs::gz-msgs)
  add_executable(motor_spike tools/motor_spike.cpp)
  target_link_libraries(motor_spike gz-transport::gz-transport gz-msgs::gz-msgs)
  add_executable(gateway gateway/gateway_main.cpp gateway/gz_link.cpp)
  target_link_libraries(gateway astrosim_gateway_core gz-transport::gz-transport gz-msgs::gz-msgs)
endif()
