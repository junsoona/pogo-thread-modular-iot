/*
 * Pure BH1750 raw-count -> milli-lux conversion, split out from bh1750.c so
 * it has zero ESP-IDF/FreeRTOS dependency and can be host-compiled directly
 * for tools/test_bh1750_convert.c (section 4 of the follow-up spec: "변환식과
 * 바이트 순서를 host test로 검증").
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BH1750 high-resolution mode datasheet formula: lux = raw_count / 1.2.
 * Returns milli-lux (lux * 1000) as an integer, i.e. raw_count * 10000 / 12,
 * computed via a 64-bit intermediate so the multiply never overflows a
 * uint32_t (raw_count is a 16-bit ADC result, max 65535).
 *
 * correction_permille applies module_config.calibration[0] as an optional
 * fixed-point scale factor in parts-per-thousand (1000 = x1.000, i.e. no
 * correction); 0 is treated the same as 1000 (no correction) -- see
 * configs/prototype_bh1750.json's calibration comment.
 */
uint32_t bh1750_raw_to_milli_lux(uint16_t raw_count, int32_t correction_permille);

/* Big-endian 2-byte -> raw_count, matching the byte order BH1750 sends the
 * measurement result in (MSB first). */
uint16_t bh1750_bytes_to_raw(uint8_t msb, uint8_t lsb);

#ifdef __cplusplus
}
#endif
