/*
 * Sensirion CRC-8 (poly 0x31, init 0xFF, no reflect, xorout 0x00), shared by
 * components/sgp40 and components/sgp41 -- the SAME algorithm as
 * components/sht45/sht45_convert.c's sht45_crc8() (every Sensirion I2C
 * sensor in this codebase uses it), pulled out on its own here rather than
 * having sgp40/sgp41 each reach into sht45's internals or duplicate it a
 * second and third time.
 *
 * Known-answer vector (verified by sensirion_crc8_selftest()):
 *   sensirion_crc8(0xBE, 0xEF) == 0x92
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t sensirion_crc8(uint8_t msb, uint8_t lsb);
bool sensirion_crc8_selftest(void);

#ifdef __cplusplus
}
#endif
