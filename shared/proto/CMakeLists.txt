# Protocol library: wire encode/decode + UDP client link. Shared by container/ and host/.
add_library(astrosim_proto STATIC wire.cpp udp_link.cpp)
target_include_directories(astrosim_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(astrosim_proto PUBLIC Threads::Threads)
