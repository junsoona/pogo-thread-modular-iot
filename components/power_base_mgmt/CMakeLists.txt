idf_component_register(
    SRCS "pb_mgmt_proto.c" "pb_mgmt_client.c"
    INCLUDE_DIRS "include"
    REQUIRES power_base_enrollment
)
