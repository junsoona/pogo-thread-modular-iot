# CHIP-SDK-independent core of pogo_matter_bridge: unit conversion, the
# module_uuid -> Matter endpoint ID persistence store, and the
# Registry-snapshot -> Matter-update reconciliation/conflict-detection logic
# (see include/matter_bridge/*.hpp's own top comments). Deliberately buildable
# and testable with nothing beyond a C/C++ compiler + CMake, exactly like
# linux/matter_bridge/ingest's own CMakeLists.txt -- no CHIP_ROOT, no Matter
# SDK, needed at all for this directory. linux/matter_bridge/app/chip/ is the
# ONLY part of this application that links against connectedhomeip, and it
# links this library rather than duplicating any of this logic.
#
# Build:
#   cd linux/matter_bridge/app/core
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(pogo_matter_bridge_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# The real, unmodified production wire-format/CRC sources this core reuses
# (never reimplemented here) -- sensor_channel.h's enum values are this
# core's only vocabulary for channel type/scale/status (value_mapping.cpp),
# and pogo_crc16_ccitt() is EndpointStore's own on-disk integrity check
# (endpoint_store.cpp), the exact same algorithm the rest of this codebase
# already uses for wire-format integrity.
add_library(pogo_matter_bridge_wire_formats STATIC
    ${REPO_ROOT}/components/pogo_proto/pogo_crc16.c
)
target_include_directories(pogo_matter_bridge_wire_formats PUBLIC
    ${REPO_ROOT}/components/pogo_proto/include
    ${REPO_ROOT}/components/sensor_channel/include
)

add_library(matter_bridge_core STATIC
    src/value_mapping.cpp
    src/endpoint_store.cpp
    src/bridge_manager.cpp
)
target_include_directories(matter_bridge_core PUBLIC
    include
    ${REPO_ROOT}/linux/matter_bridge/ingest/include
)
target_link_libraries(matter_bridge_core PUBLIC pogo_matter_bridge_wire_formats)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(matter_bridge_core PRIVATE -Wall -Wextra)
endif()

enable_testing()

add_executable(matter_bridge_core_unit_tests
    tests/test_value_mapping.cpp
    tests/test_endpoint_store.cpp
    tests/test_bridge_manager.cpp
    tests/test_removal_convergence.cpp
    tests/test_power_base_removal.cpp
    tests/test_main.cpp
    # The real ingest::Registry, so test_removal_convergence.cpp drives
    # BridgeManager from actual registry snapshots (header-only types
    # otherwise -- matter_bridge_core itself never links it).
    ${REPO_ROOT}/linux/matter_bridge/ingest/src/registry.cpp
)
find_package(Threads REQUIRED) # ingest::Registry runs its own sweep thread
target_link_libraries(matter_bridge_core_unit_tests PRIVATE matter_bridge_core Threads::Threads)
add_test(NAME matter_bridge_core_unit_tests COMMAND matter_bridge_core_unit_tests)
