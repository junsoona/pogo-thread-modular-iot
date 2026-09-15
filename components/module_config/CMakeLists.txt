idf_component_register(
    SRCS "module_config.c" "gpio_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash pogo_proto
    PRIV_REQUIRES soc
)
