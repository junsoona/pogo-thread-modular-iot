#include "sht45/sht45_convert.h"

uint8_t sht45_crc8(uint8_t msb, uint8_t lsb)
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

bool sht45_crc8_selftest(void)
{
    return sht45_crc8(0xBE, 0xEF) == 0x92;
}

int32_t sht45_raw_to_milli_celsius(uint16_t raw_t)
{
    /* T_milli = -45000 + 175000 * raw_t / 65535, integer-only via a 64-bit
     * intermediate (175000 * 65535 ~= 1.15e10, overflows a 32-bit signed
     * multiply). */
    int64_t scaled = (int64_t)175000 * (int64_t)raw_t;
    return (int32_t)(-45000 + scaled / 65535);
}

int32_t sht45_raw_to_milli_rh(uint16_t raw_rh)
{
    int64_t scaled = (int64_t)125000 * (int64_t)raw_rh;
    int32_t rh_milli = (int32_t)(-6000 + scaled / 65535);
    if (rh_milli < 0) {
        rh_milli = 0; /* clamp to the physically valid [0, 100]%RH range */
    } else if (rh_milli > 100000) {
        rh_milli = 100000;
    }
    return rh_milli;
}
