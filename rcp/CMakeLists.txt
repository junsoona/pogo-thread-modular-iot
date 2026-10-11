# Adapted from ESP-IDF v6.1's official example ($IDF_PATH/examples/openthread/ot_rcp)
# for this repository's Linux OTBR RCP firmware -- see docs/otbr-rcp.md.
#
# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Product version (PROJECT_VER -> the application description) from the
# repository's one definition, versions.json -- see docs/versioning.md. This
# is build tooling under ../tools; the RCP still shares no component and no
# source with the other firmware projects (tools/test_rcp_config.py), and its
# build identity is compiled from main/rcp_build_info.c.
include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/pogo_version.cmake)
pogo_version_setup(rcp)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(pogo_rcp)
