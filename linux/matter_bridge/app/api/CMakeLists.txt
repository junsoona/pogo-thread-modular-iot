# PogoHome Local API v1 -- the read-only HTTP/SSE server pogo_matter_bridge
# optionally exposes over --api-interface/--api-port (see
# linux/matter_bridge/app/chip/main.cpp and docs/pogohome-api-v1.md).
# Deliberately buildable and testable with nothing beyond a C/C++ compiler +
# CMake, exactly like linux/matter_bridge/ingest's and app/core's own
# CMakeLists.txt -- no CHIP_ROOT, no Matter SDK needed here at all.
# linux/matter_bridge/app/chip/ is the only part of this application that
# links against connectedhomeip, and it links this library rather than
# duplicating any of this logic (see app/BUILD.gn).
#
# Build:
#   cd linux/matter_bridge/app/api
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(pogo_matter_bridge_api LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# The real, unmodified production wire-format sources this library's JSON
# mapping is defined against (sensor_channel_type_t/sensor_scale_t/
# sensor_channel_status_t, topology_role_t) -- never reimplemented or
# renumbered here, see include/pogohome_api/sensor_kind.hpp's own top
# comment. sensor_channel.c/topology_proto.c themselves are not needed here
# (this library never packs/unpacks wire bytes, only reads already-decoded
# ingest::NodeState/ingest::ChannelReading fields) -- only their headers'
# enum definitions are.
#
# ingest/src/registry.cpp is the REAL, unmodified ingest::Registry this
# library's http_server.cpp reads from at runtime -- a second, independent
# compile of the exact same file linux/matter_bridge/ingest/CMakeLists.txt
# and app/BUILD.gn already compile into their own binaries (same "same
# sources, more than one binary" pattern those two already use), needed
# here so this library's own host tests can exercise Server against a real,
# fully-functional Registry rather than a stub.
add_library(pogohome_api STATIC
    src/hex.cpp
    src/sensor_kind.cpp
    src/json_snapshot.cpp
    src/interface_address.cpp
    src/http_server.cpp
    src/hub_identity.cpp
    src/mdns_advertiser.cpp
    ${REPO_ROOT}/linux/matter_bridge/ingest/src/registry.cpp
)
target_include_directories(pogohome_api PUBLIC
    include
    ${REPO_ROOT}/linux/matter_bridge/ingest/include
    ${REPO_ROOT}/components/sensor_channel/include
    ${REPO_ROOT}/components/topology_proto/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome_api PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(pogohome_api PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

enable_testing()

add_executable(pogohome_api_unit_tests
    tests/test_sensor_kind.cpp
    tests/test_json_snapshot.cpp
    tests/test_interface_address.cpp
    tests/test_http_server.cpp
    tests/test_multi_power_base.cpp
    tests/test_hub_identity.cpp
    tests/test_mdns_advertiser.cpp
    tests/test_main.cpp
)
target_link_libraries(pogohome_api_unit_tests PRIVATE pogohome_api)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome_api_unit_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME pogohome_api_unit_tests COMMAND pogohome_api_unit_tests)
