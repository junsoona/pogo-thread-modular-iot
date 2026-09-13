idf_component_register(
    SRCS "sensor_root_aggregator.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_channel esp_timer
)
