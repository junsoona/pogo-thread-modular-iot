idf_component_register(
    SRCS "app_main.c" "lower_thread.c" "lower_storage.c" "lower_pairing.c"
    INCLUDE_DIRS "."
    REQUIRES pogo_proto openthread esp_netif esp_event nvs_flash esp_hw_support
)
