/*
 * CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0x0000).
 *
 * This is an error-detection check only. It is NOT authentication and NOT
 * encryption -- it only lets each side detect that a frame was corrupted
 * in transit so it can be dropped and the byte stream resynchronized.
 *
 * Known-answer test vector (verified by pogo_crc16_selftest()):
 *   pogo_crc16_ccitt("123456789", 9) == 0x29B1
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t pogo_crc16_ccitt(const uint8_t *data, size_t len);

/* Runs the known-answer test above. Returns true if this build's CRC
 * implementation matches the standard CRC16/CCITT-FALSE test vector. */
bool pogo_crc16_selftest(void);

#ifdef __cplusplus
}
#endif
