idf_component_register(
    SRCS "sensor_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_service sensor_channel thread_manager node_calc topology_gate openthread esp_hw_support
)
