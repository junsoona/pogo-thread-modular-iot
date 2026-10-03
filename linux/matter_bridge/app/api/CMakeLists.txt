# PogoHome Local API v1 -- the HTTP/SSE server pogo_matter_bridge
# optionally exposes over --api-interface/--api-port, plus the TLS-only home
# settings sharing and Power Base enrollment routes, the pogohome-hubctl
# operator tool and the root-run pogohome-otbr-helper (see
# linux/matter_bridge/app/chip/main.cpp and docs/pogohome-api-v1.md).
# Buildable and testable with a C/C++ compiler, CMake and OpenSSL's
# development files (libssl-dev) -- no CHIP_ROOT, no Matter SDK needed here
# at all. OpenSSL is already required by the connectedhomeip Linux build this
# library ends up in (tools/build_matter_bridge.sh).
# linux/matter_bridge/app/chip/ is the only part of this application that
# links against connectedhomeip, and it links this library rather than
# duplicating any of this logic (see app/BUILD.gn).
#
# Build (add -DPOGOHOME_OT_CTL_PATH=... if the host's ot-ctl is not
# /usr/sbin/ot-ctl; see tools/pogohome_otbr_helper.cpp):
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
# The request protocol, input checks and bounded subprocess runner shared by
# the bridge and the root-run pogohome-otbr-helper (see
# include/pogohome_api/otbr_helper_protocol.hpp). No OpenSSL, so the helper
# links nothing it does not need.
add_library(pogohome_otbr_helper_core STATIC
    src/otbr_helper_protocol.cpp
    src/subprocess.cpp
)
target_include_directories(pogohome_otbr_helper_core PUBLIC include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome_otbr_helper_core PRIVATE -Wall -Wextra)
endif()

add_library(pogohome_api STATIC
    src/hex.cpp
    src/sensor_kind.cpp
    src/json_snapshot.cpp
    src/interface_address.cpp
    src/http_server.cpp
    src/hub_identity.cpp
    src/mdns_advertiser.cpp
    src/json_value.cpp
    src/crypto_util.cpp
    src/atomic_file.cpp
    src/home_settings.cpp
    src/settings_store.cpp
    src/device_auth.cpp
    src/tls_identity.cpp
    src/commissioner.cpp
    src/enrollment.cpp
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
find_package(OpenSSL REQUIRED)
target_link_libraries(pogohome_api PUBLIC pogohome_otbr_helper_core Threads::Threads ${CMAKE_DL_LIBS} OpenSSL::SSL
                      OpenSSL::Crypto)

# The operator tool for pairing codes and device access (see
# tools/pogohome_hubctl.cpp).
add_executable(pogohome-hubctl tools/pogohome_hubctl.cpp)
target_link_libraries(pogohome-hubctl PRIVATE pogohome_api)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome-hubctl PRIVATE -Wall -Wextra)
endif()

# The root-run OTBR helper for Power Base enrollment (see
# tools/pogohome_otbr_helper.cpp and docs/pogohome-api-v1.md "Installing the
# OTBR helper"). Built with the operating host's ot-ctl path and interface.
set(POGOHOME_OT_CTL_PATH "/usr/sbin/ot-ctl" CACHE STRING "Absolute path of the host's root-owned ot-ctl")
set(POGOHOME_OTBR_INTERFACE "wpan0" CACHE STRING "The OTBR's Thread interface name")
if(NOT POGOHOME_OT_CTL_PATH MATCHES "^/[A-Za-z0-9_./-]+$")
    message(FATAL_ERROR "POGOHOME_OT_CTL_PATH must be an absolute path of [A-Za-z0-9_./-]")
endif()
if(NOT POGOHOME_OTBR_INTERFACE MATCHES "^[a-z0-9]+$")
    message(FATAL_ERROR "POGOHOME_OTBR_INTERFACE must be [a-z0-9]+")
endif()
add_executable(pogohome-otbr-helper tools/pogohome_otbr_helper.cpp)
target_link_libraries(pogohome-otbr-helper PRIVATE pogohome_otbr_helper_core)
target_compile_definitions(pogohome-otbr-helper PRIVATE
    POGOHOME_OT_CTL_PATH="${POGOHOME_OT_CTL_PATH}"
    POGOHOME_OTBR_INTERFACE="${POGOHOME_OTBR_INTERFACE}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome-otbr-helper PRIVATE -Wall -Wextra)
endif()

enable_testing()

# Host tests only: the same helper pointed at a fake ot-ctl in the build
# directory (tests/fake_ot_ctl.cpp), without the root-ownership check.
set(POGOHOME_FAKE_OTBR_DIR ${CMAKE_CURRENT_BINARY_DIR}/fake_otbr)
add_executable(fake_ot_ctl tests/fake_ot_ctl.cpp)
set_target_properties(fake_ot_ctl PROPERTIES OUTPUT_NAME ot-ctl RUNTIME_OUTPUT_DIRECTORY ${POGOHOME_FAKE_OTBR_DIR})
add_executable(pogohome-otbr-helper-testbuild tools/pogohome_otbr_helper.cpp)
target_link_libraries(pogohome-otbr-helper-testbuild PRIVATE pogohome_otbr_helper_core)
target_compile_definitions(pogohome-otbr-helper-testbuild PRIVATE
    POGOHOME_OTBR_HELPER_TEST_BUILD=1
    POGOHOME_OT_CTL_PATH="${POGOHOME_FAKE_OTBR_DIR}/ot-ctl"
    POGOHOME_OTBR_CLI_TIMEOUT_MS=1500
)

add_executable(pogohome_api_unit_tests
    tests/test_sensor_kind.cpp
    tests/test_json_snapshot.cpp
    tests/test_interface_address.cpp
    tests/test_http_server.cpp
    tests/test_multi_power_base.cpp
    tests/test_hub_identity.cpp
    tests/test_mdns_advertiser.cpp
    tests/test_json_value.cpp
    tests/test_home_settings.cpp
    tests/test_settings_store.cpp
    tests/test_device_auth.cpp
    tests/test_settings_api.cpp
    tests/test_otbr_helper.cpp
    tests/test_enrollment.cpp
    tests/test_enrollment_api.cpp
    tests/test_main.cpp
)
target_link_libraries(pogohome_api_unit_tests PRIVATE pogohome_api)
target_compile_definitions(pogohome_api_unit_tests PRIVATE
    POGOHOME_TEST_OTBR_HELPER="$<TARGET_FILE:pogohome-otbr-helper-testbuild>"
    POGOHOME_TEST_FAKE_OTBR_DIR="${POGOHOME_FAKE_OTBR_DIR}"
)
add_dependencies(pogohome_api_unit_tests pogohome-otbr-helper-testbuild fake_ot_ctl)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome_api_unit_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME pogohome_api_unit_tests COMMAND pogohome_api_unit_tests)
