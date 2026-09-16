idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES pogo_proto pogo_port parent_service power_base_config power_base_thread_config
             power_base_transport thread_manager nvs_flash esp_event esp_netif esp_hw_support openthread
)
