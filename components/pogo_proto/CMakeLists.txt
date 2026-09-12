idf_component_register(
    SRCS "pogo_crc16.c" "pogo_frame.c" "pogo_protocol.c" "pogo_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart
)
