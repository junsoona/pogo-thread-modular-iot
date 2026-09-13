idf_component_register(
    SRCS "topology_reporter.c"
    INCLUDE_DIRS "include"
    REQUIRES topology_proto role_manager child_service sensor_service module_config pogo_port
             thread_manager node_calc topology_gate openthread esp_hw_support nvs_flash
)
