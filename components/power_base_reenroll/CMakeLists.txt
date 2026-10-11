idf_component_register(
    SRCS "pb_reenroll_button.c" "pb_reenroll_store.c" "pb_reenroll.c"
    INCLUDE_DIRS "include"
    REQUIRES power_base_enrollment module_config nvs_flash
)
