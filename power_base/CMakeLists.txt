# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

# Product version (PROJECT_VER -> the application description) from the
# repository's one definition, versions.json -- see docs/versioning.md.
include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/pogo_version.cmake)
pogo_version_setup(power_base)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(power_base)
