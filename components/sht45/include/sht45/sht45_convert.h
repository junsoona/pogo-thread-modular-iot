/*
 * Pure SHT4x CRC-8 + raw-word -> milli-unit conversion, split out from
 * sht45.c so it has zero ESP-IDF/FreeRTOS dependency and can be
 * host-compiled directly for tools/test_sht45_convert.c (section 5/13 of the
 * follow-up spec: "공식 예제 벡터 또는 자체 고정 벡터로 CRC와 변환식 host
 * test 작성").
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sensirion CRC-8 (poly 0x31, init 0xFF, no reflect, xorout 0x00), used by
 * SHT4x (and SGP4x) to protect each 16-bit word on the wire. Known-answer
 * vector from Sensirion's own datasheets/example code: CRC8(0xBE, 0xEF) ==
 * 0x92 -- checked by sht45_crc8_selftest() below.
 */
uint8_t sht45_crc8(uint8_t msb, uint8_t lsb);
bool sht45_crc8_selftest(void);

/*
 * SHT4x datasheet conversion formulas, computed as milli-units (value *
 * 1000) via integer-only arithmetic so no floating point ever touches the
 * wire (section 5/6: "부동소수점 값을 wire format에 직접 전송하지 마라"):
 *   T[degC] = -45 + 175 * S_T / 65535
 *   RH[%RH] =  -6 + 125 * S_RH / 65535, clamped to the physically valid
 *              [0, 100] %RH range (section 5: "humidity 결과의 유효 범위
 *              처리") -- a raw reading can compute slightly outside this
 *              range near the sensor's rails, which is expected and NOT a
 *              CRC/communication error.
 */
int32_t sht45_raw_to_milli_celsius(uint16_t raw_t);
int32_t sht45_raw_to_milli_rh(uint16_t raw_rh);

#ifdef __cplusplus
}
#endif
