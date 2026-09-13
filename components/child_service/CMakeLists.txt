idf_component_register(
    SRCS "child_service.c"
    INCLUDE_DIRS "include"
    REQUIRES pogo_proto pogo_port role_manager module_config sensor_service thread_manager
             nvs_flash esp_hw_support esp_timer
)
