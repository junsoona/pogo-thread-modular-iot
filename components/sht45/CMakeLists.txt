idf_component_register(
    SRCS "sht45.c" "sht45_convert.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_provider esp_driver_i2c esp_timer
    PRIV_REQUIRES esp_rom
)
