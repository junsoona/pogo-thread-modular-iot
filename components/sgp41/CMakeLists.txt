idf_component_register(
    SRCS "sgp41.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_provider sensirion_crc8 sensirion_gas_index_algorithm esp_driver_i2c esp_timer
)
