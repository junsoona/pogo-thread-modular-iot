idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES pogo_proto module_config sensor_provider pogo_port role_manager thread_manager
             child_service parent_service nvs_flash esp_event esp_netif esp_hw_support openthread
)
