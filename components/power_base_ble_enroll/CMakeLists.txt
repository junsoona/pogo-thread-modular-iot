idf_component_register(
    SRCS "pb_ble_admission.c" "pb_ble_service.c"
    INCLUDE_DIRS "include"
    REQUIRES power_base_enrollment power_base_ble_auth protocomm
    PRIV_REQUIRES mbedtls
)
