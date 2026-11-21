# The PX4 SITL adapter and the airframe generator. Included from fc-container/CMakeLists.txt with
# ASTROSIM_BUILD_PX4=ON; it relies on the parent's astrosim_warnings() and test setup.
#
# Targets:
#   astrosim_mavlink        INTERFACE, the MAVLink C headers as a SYSTEM include (so -Werror applies to our code only)
#   astrosim_px4_core       pure, portable: codec, telemetry, session (no sockets)
#   astrosim_px4_airframe   pure: airframe script generation
#   astrosim_px4_adapter    POSIX sockets: Px4Adapter and makePx4Adapter()
#   astrosim-px4-airframe   executable
#
# MAVLink headers: mavlink/c_library_v2, a pinned commit. Set ASTROSIM_MAVLINK_DIR to a checkout (the Dockerfile
# does); otherwise the pinned commit is downloaded at configure time into the build directory.
set(ASTROSIM_MAVLINK_COMMIT "d2062a25f05fc85213830427058401b0ec345430" CACHE STRING
    "mavlink/c_library_v2 commit used when ASTROSIM_MAVLINK_DIR is not set")
set(ASTROSIM_MAVLINK_DIR "" CACHE PATH "Directory of a mavlink/c_library_v2 checkout (contains common/mavlink.h)")

if(NOT ASTROSIM_MAVLINK_DIR)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # timestamps of extracted files are the extraction time
  endif()
  # The archive of a pinned commit; it has no CMakeLists.txt to add (SOURCE_SUBDIR points at nothing).
  FetchContent_Declare(astrosim_mavlink_src
    URL https://github.com/mavlink/c_library_v2/archive/${ASTROSIM_MAVLINK_COMMIT}.tar.gz
    SOURCE_SUBDIR astrosim_no_cmake_here)
  FetchContent_MakeAvailable(astrosim_mavlink_src)
  set(ASTROSIM_MAVLINK_DIR ${astrosim_mavlink_src_SOURCE_DIR})
endif()
if(NOT EXISTS ${ASTROSIM_MAVLINK_DIR}/common/mavlink.h)
  message(FATAL_ERROR "ASTROSIM_MAVLINK_DIR=${ASTROSIM_MAVLINK_DIR} does not contain common/mavlink.h")
endif()

add_library(astrosim_mavlink INTERFACE)
target_include_directories(astrosim_mavlink SYSTEM INTERFACE ${ASTROSIM_MAVLINK_DIR})

add_library(astrosim_px4_core STATIC mavlink_codec.cpp px4_telemetry.cpp px4_session.cpp)
target_include_directories(astrosim_px4_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(astrosim_px4_core PUBLIC astrosim_mavlink astrosim::fc)
astrosim_warnings(astrosim_px4_core)

add_library(astrosim_px4_airframe STATIC airframe.cpp)
target_include_directories(astrosim_px4_airframe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(astrosim_px4_airframe PUBLIC astrosim::aircraft)
astrosim_warnings(astrosim_px4_airframe)

# POSIX sockets: container-only, like the bridge's UDP transport.
add_library(astrosim_px4_adapter STATIC px4_adapter.cpp)
target_include_directories(astrosim_px4_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Depends on the configuration library only (never on the bridge core, which links this library).
target_link_libraries(astrosim_px4_adapter PUBLIC astrosim_px4_core astrosim_fc_config astrosim::log)
target_compile_definitions(astrosim_px4_adapter PUBLIC ASTROSIM_HAVE_PX4=1)
astrosim_warnings(astrosim_px4_adapter)

add_executable(astrosim-px4-airframe airframe_main.cpp)
target_link_libraries(astrosim-px4-airframe PRIVATE astrosim_px4_airframe astrosim_fc_config)
astrosim_warnings(astrosim-px4-airframe)

if(ASTROSIM_BUILD_TESTS)
  foreach(name test_airframe test_mavlink_codec test_px4_session test_px4_telemetry test_px4_adapter)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE astrosim_px4_core astrosim_px4_airframe astrosim_px4_adapter GTest::gtest_main)
    astrosim_warnings(${name})
    if(name STREQUAL "test_px4_adapter")
      # Binds fixed loopback ports: never run concurrently with itself.
      gtest_discover_tests(${name} PROPERTIES RESOURCE_LOCK astrosim_px4_ports)
    else()
      gtest_discover_tests(${name})
    endif()
  endforeach()
  target_compile_definitions(test_airframe PRIVATE ASTROSIM_AIRCRAFT_TOML="${ASTROSIM_AIRCRAFT_TOML}")
endif()
