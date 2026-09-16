idf_component_register(
    SRCS "sensor_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_service sensor_channel collector_endpoint node_calc topology_gate coap_response_timing
             openthread esp_hw_support
)
