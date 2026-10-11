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
    src/build_info.cpp
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
    src/power_base_management.cpp
    ${REPO_ROOT}/linux/matter_bridge/ingest/src/registry.cpp
    # The REAL management wire format the Power Base firmware compiles
    # (pure C, no ESP-IDF dependency) -- never reimplemented here.
    ${REPO_ROOT}/components/power_base_mgmt/pb_mgmt_proto.c
)
target_include_directories(pogohome_api PUBLIC
    include
    ${REPO_ROOT}/linux/matter_bridge/ingest/include
    ${REPO_ROOT}/components/sensor_channel/include
    ${REPO_ROOT}/components/topology_proto/include
    ${REPO_ROOT}/components/power_base_mgmt/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome_api PRIVATE -Wall -Wextra)
endif()

# The build identity (docs/versioning.md): component, product version from
# the repository's versions.json, commit, clean/dirty, source fingerprint --
# as a header in the BUILD directory, never in the source tree.
# tools/pogo_version.py writes it only when its content changes and is asked
# at every build, so an incremental build after a source change or a commit
# recompiles build_info.cpp with the new identity. -DPOGO_RELEASE=ON makes it
# a release build, which the tool refuses for a dirty tree or unknown commit.
option(POGO_RELEASE "Stamp this build as a release (needs a clean tree and a known commit)" OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(POGO_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(POGO_BUILD_INFO_STAMP ${Python3_EXECUTABLE} "${REPO_ROOT}/tools/pogo_version.py" stamp --component linux_bridge
    --out "${POGO_BUILD_INFO_DIR}/pogo_build_info_gen.h" --variant "cmake-host-${CMAKE_BUILD_TYPE}" --quiet)
if(POGO_RELEASE)
    list(APPEND POGO_BUILD_INFO_STAMP --release)
endif()
execute_process(COMMAND ${POGO_BUILD_INFO_STAMP} RESULT_VARIABLE POGO_BUILD_INFO_RESULT)
if(NOT POGO_BUILD_INFO_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/pogo_version.py stamp failed (${POGO_BUILD_INFO_RESULT})")
endif()
add_custom_target(pogo_build_info_stamp
    COMMAND ${POGO_BUILD_INFO_STAMP}
    BYPRODUCTS "${POGO_BUILD_INFO_DIR}/pogo_build_info_gen.h"
    COMMENT "POGO build identity"
    VERBATIM)
add_dependencies(pogohome_api pogo_build_info_stamp)
target_include_directories(pogohome_api PRIVATE "${POGO_BUILD_INFO_DIR}")

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
    tests/test_power_base_management.cpp
    tests/test_removal_api.cpp
    tests/test_build_info.cpp
    tests/test_main.cpp
    # Power Base management is tested against the REAL firmware side
    # (docs/power-base-management.md): the board's management client and
    # enrollment record code, unmodified, over the firmware host tests' NVS
    # simulator -- and through the real ingest dispatch (session.cpp), which
    # needs the data path's wire formats to link.
    ${REPO_ROOT}/components/power_base_mgmt/pb_mgmt_client.c
    ${REPO_ROOT}/components/power_base_enrollment/power_base_enrollment.c
    ${REPO_ROOT}/tools/host_test_stubs/nvs_sim/nvs_sim.c
    ${REPO_ROOT}/linux/matter_bridge/ingest/src/session.cpp
    ${REPO_ROOT}/linux/matter_bridge/ingest/src/minicoap.cpp
    ${REPO_ROOT}/components/bridge_export_proto/bridge_export_proto.c
    ${REPO_ROOT}/components/sensor_channel/sensor_channel.c
    ${REPO_ROOT}/components/pogo_proto/pogo_crc16.c
)
target_include_directories(pogohome_api_unit_tests PRIVATE
    # nvs_sim first: its nvs.h is the functional simulator's.
    ${REPO_ROOT}/tools/host_test_stubs/nvs_sim
    ${REPO_ROOT}/tools/host_test_stubs
    ${REPO_ROOT}/components/power_base_enrollment/include
    ${REPO_ROOT}/components/bridge_export_proto/include
    ${REPO_ROOT}/components/pogo_proto/include
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

# The version a built binary prints is the one in versions.json -- read here
# from the file itself, independently of the stamp the binary was built with.
file(READ "${REPO_ROOT}/versions.json" POGO_VERSIONS_JSON)
string(JSON POGO_LINUX_BRIDGE_VERSION GET "${POGO_VERSIONS_JSON}" components linux_bridge version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPO_ROOT}/versions.json")
string(REPLACE "." "\\." POGO_LINUX_BRIDGE_VERSION_RE "${POGO_LINUX_BRIDGE_VERSION}")
add_test(NAME pogohome_build_info_matches_versions COMMAND pogohome-hubctl --version)
set_tests_properties(pogohome_build_info_matches_versions PROPERTIES PASS_REGULAR_EXPRESSION
    "^pogohome-hubctl ${POGO_LINUX_BRIDGE_VERSION_RE} \\(linux_bridge, build [0-9a-f]+(\\+dirty)?\\.[0-9a-f]+, (development|release)\\)")

# The same source built WITHOUT the stamp header (what a GN build without
# python3 gets): it must say "unknown", never a made-up version, and leave
# the health field out.
add_executable(pogohome_build_info_nostamp_test tests/test_build_info_nostamp.cpp src/build_info.cpp)
target_include_directories(pogohome_build_info_nostamp_test PRIVATE include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pogohome_build_info_nostamp_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME pogohome_build_info_nostamp COMMAND pogohome_build_info_nostamp_test)
