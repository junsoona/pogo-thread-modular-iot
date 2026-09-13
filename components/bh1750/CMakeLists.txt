idf_component_register(
    SRCS "bh1750.c" "bh1750_convert.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_provider esp_driver_i2c esp_timer
)
