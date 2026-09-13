idf_component_register(
    SRCS "thread_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES openthread esp_netif esp_event esp_hw_support
)
