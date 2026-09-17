idf_component_register(
    SRCS "power_base_bridge_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash pogo_proto
)
