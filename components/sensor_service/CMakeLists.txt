idf_component_register(
    SRCS "sensor_service.c"
    INCLUDE_DIRS "include"
    REQUIRES module_config sensor_provider sensor_channel role_manager node_calc sensor_root_aggregator
             esp_timer esp_hw_support
)
