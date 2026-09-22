# Power Base -> Linux OTBR northbound bridge: Linux ingest program (this
# follow-up). Ubuntu x86_64 is the intended production target; this also
# builds and runs on macOS/other POSIX systems (plain BSD sockets, no
# Linux-specific syscalls) for local development.
#
# Recommended production build: libcoap3. This CMake project instead builds
# a small internal CoAP-over-UDP subset (ingest::minicoap, see that header's
# own top comment) to stay buildable/testable with nothing beyond a C++17
# compiler + CMake -- no system package installation required in a sandboxed
# dev/CI environment. The registry/session logic (registry.hpp/session.hpp)
# is completely decoupled from that transport shim, so swapping in a real
# libcoap3 server later only touches server.cpp/main.cpp.
#
# Build:
#   cd linux/matter_bridge/ingest
#   cmake -S . -B build
#   cmake --build build
#
# Run (production, port 5684 default):
#   ./build/ingest_server [port]
#
# Run tests (local loopback + pure unit tests, no external network/services):
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(pogo_bridge_ingest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# The REAL, unmodified production wire-format sources shared with the
# ESP32 firmware (components/bridge_export_proto, components/sensor_channel,
# components/pogo_proto's CRC16) -- never reimplemented here. All three are
# pure C with zero ESP-IDF/FreeRTOS dependency (see each header's own top
# comment), so they compile standalone exactly like they do for the
# firmware's own host tests (tools/test_bridge_export_proto.c).
add_library(pogo_wire_formats STATIC
    ${REPO_ROOT}/components/bridge_export_proto/bridge_export_proto.c
    ${REPO_ROOT}/components/sensor_channel/sensor_channel.c
    ${REPO_ROOT}/components/pogo_proto/pogo_crc16.c
)
target_include_directories(pogo_wire_formats PUBLIC
    ${REPO_ROOT}/components/bridge_export_proto/include
    ${REPO_ROOT}/components/sensor_channel/include
    ${REPO_ROOT}/components/pogo_proto/include
)

find_package(Threads REQUIRED)

add_library(ingest_core STATIC
    src/minicoap.cpp
    src/registry.cpp
    src/session.cpp
)
target_include_directories(ingest_core PUBLIC include)
# PUBLIC, not PRIVATE: registry.cpp's own bounded stale-timeout sweep thread
# (see ingest/registry.hpp's "Correction pass 2") makes ingest::Registry
# itself use std::thread/std::condition_variable now, not just server.cpp's
# CoAP loop -- every consumer of this library (ingest_server, every test
# below, and linux/matter_bridge/app's own GN build, which compiles this
# exact registry.cpp directly) needs pthread linked in on a standard Linux
# glibc toolchain (glibc >= 2.34 merges it into libc and would link fine
# either way, but this must not silently depend on that).
target_link_libraries(ingest_core PUBLIC pogo_wire_formats Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ingest_core PRIVATE -Wall -Wextra)
    target_compile_options(pogo_wire_formats PRIVATE -Wall -Wextra)
endif()

add_executable(ingest_server src/main.cpp src/server.cpp)
target_link_libraries(ingest_server PRIVATE ingest_core Threads::Threads)

enable_testing()

add_executable(ingest_unit_tests tests/test_registry_and_session.cpp)
target_link_libraries(ingest_unit_tests PRIVATE ingest_core)
add_test(NAME ingest_unit_tests COMMAND ingest_unit_tests)

add_executable(ingest_registry_stale_timeout_test tests/test_registry_stale_timeout.cpp)
target_link_libraries(ingest_registry_stale_timeout_test PRIVATE ingest_core)
add_test(NAME ingest_registry_stale_timeout_test COMMAND ingest_registry_stale_timeout_test)

add_executable(ingest_registry_snapshot_test tests/test_registry_snapshot.cpp)
target_link_libraries(ingest_registry_snapshot_test PRIVATE ingest_core Threads::Threads)
add_test(NAME ingest_registry_snapshot_test COMMAND ingest_registry_snapshot_test)

add_executable(ingest_loopback_test tests/test_loopback.cpp src/server.cpp)
target_link_libraries(ingest_loopback_test PRIVATE ingest_core Threads::Threads)
add_test(NAME ingest_loopback_test COMMAND ingest_loopback_test)

# Static check that the ACK path in src/server.cpp still uses
# sendmsg()+IPV6_PKTINFO rather than a plain sendto() -- see that test's own
# top comment for why this is needed in addition to the socket-based
# loopback regression above.
add_executable(ingest_ack_transport_static_test tests/test_ack_transport_static.cpp)
target_compile_definitions(ingest_ack_transport_static_test PRIVATE
    INGEST_SERVER_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/server.cpp")
add_test(NAME ingest_ack_transport_static_test COMMAND ingest_ack_transport_static_test)
