#include "pogo_proto/pogo_crc16.h"
#include <string.h>

uint16_t pogo_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

bool pogo_crc16_selftest(void)
{
    static const char kVector[] = "123456789";
    return pogo_crc16_ccitt((const uint8_t *)kVector, strlen(kVector)) == 0x29B1;
}
