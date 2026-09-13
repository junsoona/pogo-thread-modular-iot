idf_component_register(
    SRCS "parent_service.c"
    INCLUDE_DIRS "include"
    REQUIRES pogo_proto pogo_port role_manager module_config child_service thread_manager esp_hw_support
)
