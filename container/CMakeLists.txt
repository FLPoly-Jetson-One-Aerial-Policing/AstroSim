cmake_minimum_required(VERSION 3.22)
project(astrosim_container CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# Wire protocol library (shared/, also used by the host build; no gz deps).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/proto shared/proto)

add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire astrosim_proto)
target_compile_definitions(test_wire PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shared/proto/testdata")
add_test(NAME wire COMMAND test_wire)

add_library(astrosim_gateway_core STATIC gateway/gateway.cpp)
target_include_directories(astrosim_gateway_core PUBLIC gateway)
target_link_libraries(astrosim_gateway_core PUBLIC astrosim_proto)
add_executable(test_gateway tests/test_gateway.cpp)
target_link_libraries(test_gateway astrosim_gateway_core pthread)
add_test(NAME gateway COMMAND test_gateway)

# Image build: -DASTROSIM_BUILD_GATEWAY=ON (gz Jetty). Host-side build: default (no gz).
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
