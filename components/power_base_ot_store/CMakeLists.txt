idf_component_register(
    SRCS "pb_volatile_store.c" "pb_stored_network.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES openthread mbedtls nvs_flash
)
