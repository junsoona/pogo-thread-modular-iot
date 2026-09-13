#include "bh1750/bh1750_convert.h"
#include <stdint.h>

uint16_t bh1750_bytes_to_raw(uint8_t msb, uint8_t lsb)
{
    return (uint16_t)(((uint16_t)msb << 8) | lsb);
}

uint32_t bh1750_raw_to_milli_lux(uint16_t raw_count, int32_t correction_permille)
{
    uint64_t milli_lux = ((uint64_t)raw_count * 10000u) / 12u;
    if (correction_permille != 0 && correction_permille != 1000) {
        milli_lux = (milli_lux * (uint64_t)correction_permille) / 1000u;
    }
    if (milli_lux > UINT32_MAX) {
        milli_lux = UINT32_MAX; /* saturate rather than wrap -- unreachable with a real 16-bit raw_count
                                    and any sane correction factor, but never silently wrap on a bad one */
    }
    return (uint32_t)milli_lux;
}
