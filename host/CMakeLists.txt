cmake_minimum_required(VERSION 3.22)
project(astrosim_host CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# Wire protocol library, shared with the container build.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/proto shared/proto)

add_executable(test_input tests/test_input.cpp)
target_link_libraries(test_input astrosim_proto)
target_include_directories(test_input PRIVATE app)
add_test(NAME input COMMAND test_input)

# Campus greybox: header-only scene (shared/scene), its SDF generator, and a test that the committed SDF is current.
add_executable(gen_campus tools/gen_campus.cpp)
target_include_directories(gen_campus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/scene)
add_executable(test_campus tests/test_campus.cpp)
target_include_directories(test_campus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/scene)
target_compile_definitions(test_campus PRIVATE ASTROSIM_CAMPUS_SDF="${CMAKE_CURRENT_SOURCE_DIR}/../container/models/campus/model.sdf")
add_test(NAME campus COMMAND test_campus)

# raylib for the viewer. AUTO: use an installed raylib (pkg-config), else fetch and build a pinned 5.5.
# SYSTEM: installed only (viewer skipped if missing). FETCH: always build from source. OFF: no viewer.
set(ASTROSIM_RAYLIB AUTO CACHE STRING "raylib source: AUTO | SYSTEM | FETCH | OFF")
set_property(CACHE ASTROSIM_RAYLIB PROPERTY STRINGS AUTO SYSTEM FETCH OFF)
set(RAYLIB_TARGET "")
if(NOT ASTROSIM_RAYLIB STREQUAL "OFF")
  if(NOT ASTROSIM_RAYLIB STREQUAL "FETCH")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(RAYLIB QUIET IMPORTED_TARGET raylib)
    endif()
    if(RAYLIB_FOUND)
      set(RAYLIB_TARGET PkgConfig::RAYLIB)
      message(STATUS "raylib: using system ${RAYLIB_VERSION}")
    endif()
  endif()
  if(NOT RAYLIB_TARGET AND NOT ASTROSIM_RAYLIB STREQUAL "SYSTEM")
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
      URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
      URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(raylib)
    set(RAYLIB_TARGET raylib)
    message(STATUS "raylib: building 5.5 from source (needs X11/OpenGL dev headers, see SETUP.md)")
  endif()
endif()
if(RAYLIB_TARGET)
  add_executable(viewer app/viewer.cpp)
  target_link_libraries(viewer astrosim_proto ${RAYLIB_TARGET})
  target_include_directories(viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/scene)
else()
  message(STATUS "raylib not available: viewer not built")
endif()

add_executable(climb tools/climb.cpp)
target_link_libraries(climb astrosim_proto)
add_executable(latency tools/latency.cpp)
target_link_libraries(latency astrosim_proto)
