idf_component_register(
    SRCS "sensor_provider.c" "mock_sensor_provider.c"
    INCLUDE_DIRS "include"
    REQUIRES module_config sensor_channel esp_timer
)
