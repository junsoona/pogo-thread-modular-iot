idf_component_register(
    SRCS "pogo_port.c"
    INCLUDE_DIRS "include"
    REQUIRES pogo_proto esp_driver_gpio esp_driver_uart
)
