#include "sensirion_crc8/sensirion_crc8.h"

uint8_t sensirion_crc8(uint8_t msb, uint8_t lsb)
{
    uint8_t data[2] = {msb, lsb};
    uint8_t crc = 0xFF;
    for (int b = 0; b < 2; b++) {
        crc ^= data[b];
        for (int i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

bool sensirion_crc8_selftest(void)
{
    return sensirion_crc8(0xBE, 0xEF) == 0x92;
}
