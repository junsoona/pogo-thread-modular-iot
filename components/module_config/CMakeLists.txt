idf_component_register(
    SRCS "module_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash pogo_proto
)
