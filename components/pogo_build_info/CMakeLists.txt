idf_component_register(
    SRCS "pogo_build_info.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES log
)

# The identity string must be in the image even when nothing calls
# pogo_build_info_string() and the log level compiles the boot line away (the
# RCP): the linker's section garbage collection would drop it otherwise. Same
# mechanism ESP-IDF uses for its own application description (-u esp_app_desc).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pogo_build_info_string")

# The build identity as a header in the BUILD directory, refreshed at every build:
# tools/cmake/pogo_version.cmake (pogo_build_info_attach).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    pogo_build_info_attach(${COMPONENT_LIB})
endif()
