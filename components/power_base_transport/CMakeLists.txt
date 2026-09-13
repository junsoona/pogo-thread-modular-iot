idf_component_register(
    SRCS "power_base_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES topology_proto topology_registry sensor_channel sensor_root_aggregator
             thread_manager openthread esp_hw_support
)
