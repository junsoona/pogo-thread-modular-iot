idf_component_register(
    SRCS "app_main.c" "module_wiring.c"
    INCLUDE_DIRS "."
    REQUIRES pogo_proto module_config sensor_provider sensor_channel sensor_service pogo_port role_manager
             thread_manager child_service parent_service sensor_transport topology_reporter topology_gate
             collector_endpoint
             bh1750 sht45 sgp40 sgp41 pms5003
             nvs_flash esp_event esp_netif esp_hw_support openthread
             pogo_build_info
)
