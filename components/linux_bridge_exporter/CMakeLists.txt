idf_component_register(
    SRCS "linux_bridge_exporter.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_export_proto power_base_bridge_config power_base_transport sensor_channel
             coap_response_timing openthread esp_hw_support
)
