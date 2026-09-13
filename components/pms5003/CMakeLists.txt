idf_component_register(
    SRCS "pms5003.c" "pms5003_parser.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_provider
)
