idf_component_register(
    SRCS "sensor_provider.c" "mock_sensor_provider.c"
    INCLUDE_DIRS "include"
    REQUIRES module_config esp_timer
)
