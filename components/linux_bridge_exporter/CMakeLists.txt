idf_component_register(
    SRCS "linux_bridge_exporter.c" "removed_journal.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_export_proto power_base_bridge_config power_base_transport sensor_channel
             sensor_root_aggregator coap_response_timing openthread esp_hw_support
    PRIV_REQUIRES nvs_flash topology_registry topology_proto
)
