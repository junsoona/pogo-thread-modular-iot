idf_component_register(
    SRCS "app_main.c" "enrollment_runtime.c" "enrollment_console.c" "enrollment_ble.c"
    INCLUDE_DIRS "."
    REQUIRES pogo_proto pogo_port parent_service power_base_config power_base_thread_config
             power_base_bridge_config power_base_transport linux_bridge_exporter
             power_base_enrollment thread_manager nvs_flash esp_event esp_netif esp_hw_support openthread
             console power_base_ble_auth power_base_ble_enroll protocomm bt
)
