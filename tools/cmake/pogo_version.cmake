# POGO product version for an ESP-IDF project (docs/versioning.md).
#
# Include this from a project's CMakeLists.txt and call pogo_version_setup(<component>) BEFORE
# include($ENV{IDF_PATH}/tools/cmake/project.cmake):
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../tools/cmake/pogo_version.cmake)
#   pogo_version_setup(power_base)
#
# It sets PROJECT_VER -- which ESP-IDF puts into the application description
# (esp_app_desc_t.version, 32 bytes including the terminating NUL) -- from the repository's one
# version definition, versions.json, and makes CMake configure again when that file changes.
# The version is never taken from `git describe`, a version.txt or a Kconfig option.
#
# The build identity (commit, clean/dirty, source fingerprint) is NOT decided at configure time: it
# changes without a reconfigure. pogo_build_info_attach(<component library>) makes a component ask
# tools/pogo_version.py at every build; components/pogo_build_info uses it for the Power Base and
# the sensor module, and the RCP -- which shares no component with them -- for its own main.

function(pogo_version_setup component)
    get_filename_component(_pogo_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set(_pogo_file "${_pogo_root}/versions.json")
    if(NOT EXISTS "${_pogo_file}")
        message(FATAL_ERROR "POGO versions: ${_pogo_file} is missing")
    endif()
    file(READ "${_pogo_file}" _pogo_json)
    string(JSON _pogo_version ERROR_VARIABLE _pogo_error GET "${_pogo_json}" components ${component} version)
    if(_pogo_error OR NOT _pogo_version MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "POGO versions: components.${component}.version in ${_pogo_file} must be MAJOR.MINOR.PATCH "
                            "(found '${_pogo_version}' ${_pogo_error})")
    endif()
    string(LENGTH "${_pogo_version}" _pogo_length)
    if(_pogo_length GREATER 31)
        message(FATAL_ERROR "POGO versions: '${_pogo_version}' does not fit the 32-byte app description field")
    endif()
    set(PROJECT_VER "${_pogo_version}" PARENT_SCOPE)
    set(POGO_COMPONENT "${component}" CACHE INTERNAL "POGO component this project builds" FORCE)
    set(POGO_REPO_ROOT "${_pogo_root}" CACHE INTERNAL "POGO repository root" FORCE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pogo_file}")
    message(STATUS "POGO ${component} version ${_pogo_version} (from versions.json)")
endfunction()

# The build identity -- component, product version, commit, clean/dirty, source fingerprint, build
# kind and variant -- as a header (pogo_build_info_gen.h) in the BUILD directory of the calling
# component, never in the source tree, and on that component's private include path.
# tools/pogo_version.py writes it only when its content changes, and is asked at EVERY build: an
# incremental build after a source change or a commit therefore recompiles the one file that
# includes it, and a build with nothing changed recompiles nothing.
#
# Call it from a component's CMakeLists.txt after idf_component_register(). With -DPOGO_RELEASE=1
# the tool refuses a dirty tree or an unknown commit, and with it the whole build fails.
function(pogo_build_info_attach component_lib)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    if(NOT POGO_COMPONENT OR NOT POGO_REPO_ROOT)
        message(FATAL_ERROR "pogo_build_info: the project must call pogo_version_setup(<component>) "
                            "(tools/cmake/pogo_version.cmake) before project()")
    endif()
    idf_build_get_property(_pogo_python PYTHON)
    idf_build_get_property(_pogo_sdkconfig SDKCONFIG)
    set(_pogo_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
    set(_pogo_header "${_pogo_gen_dir}/pogo_build_info_gen.h")
    set(_pogo_stamp ${_pogo_python} "${POGO_REPO_ROOT}/tools/pogo_version.py" stamp
        --component ${POGO_COMPONENT} --out "${_pogo_header}" --sdkconfig "${_pogo_sdkconfig}" --quiet)
    if(POGO_RELEASE)
        list(APPEND _pogo_stamp --release)
    endif()
    # Once now, so that the header exists for the first compile ...
    execute_process(COMMAND ${_pogo_stamp} RESULT_VARIABLE _pogo_result)
    if(NOT _pogo_result EQUAL 0)
        message(FATAL_ERROR "pogo_build_info: tools/pogo_version.py stamp failed (${_pogo_result})")
    endif()
    # ... and at every build.
    add_custom_target(pogo_build_info_stamp
        COMMAND ${_pogo_stamp}
        BYPRODUCTS "${_pogo_header}"
        COMMENT "POGO build identity"
        VERBATIM)
    add_dependencies(${component_lib} pogo_build_info_stamp)
    target_include_directories(${component_lib} PRIVATE "${_pogo_gen_dir}")
endfunction()
