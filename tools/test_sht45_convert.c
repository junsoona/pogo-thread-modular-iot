/*
 * Host-compiled test for SHT4x CRC-8 + raw conversion
 * (components/sht45/sht45_convert.c) -- section 5/13 of the follow-up spec.
 *
 * Links against the REAL, unmodified sht45_convert.c (no ESP-IDF/FreeRTOS
 * dependency).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/sht45/include \
 *      -o /tmp/test_sht45_convert tools/test_sht45_convert.c \
 *      components/sht45/sht45_convert.c \
 *   && /tmp/test_sht45_convert
 */
#include <stdio.h>
#include <stdint.h>

#include "sht45/sht45_convert.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s\n", msg);                                                                               \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

int main(void)
{
    /* Official Sensirion known-answer vector (same CRC-8 as SHT3x/SHT4x/SGP4x). */
    CHECK(sht45_crc8(0xBE, 0xEF) == 0x92, "crc8: known-answer vector (0xBE,0xEF) -> 0x92");
    CHECK(sht45_crc8_selftest(), "crc8_selftest() passes");
    /* CRC of an all-zero word is a fixed value for this poly/init/xorout
     * combination -- cross-checked against an independent Python
     * reference implementation of the same CRC-8/poly=0x31/init=0xFF/xorout=0x00. */
    CHECK(sht45_crc8(0x00, 0x00) == 0x81, "crc8: (0x00,0x00) -> 0x81");
    /* Changing a single bit must change the CRC (not exhaustive, just a
     * sanity check that this isn't an identity/no-op function). */
    CHECK(sht45_crc8(0x01, 0x00) != sht45_crc8(0x00, 0x00), "crc8: differs for different input");

    /* Datasheet formula boundary values, chosen so the division comes out
     * exact (raw values that are multiples of 65535/well-known fractions),
     * so the test itself has no rounding ambiguity. */
    CHECK(sht45_raw_to_milli_celsius(0) == -45000, "temp: raw=0 -> -45.000 degC (formula floor)");
    CHECK(sht45_raw_to_milli_celsius(65535) == 130000, "temp: raw=65535 -> 130.000 degC (formula ceiling)");
    {
        int64_t expect_temp = -45000 + ((int64_t)175000 * (65535 / 2)) / 65535;
        CHECK(sht45_raw_to_milli_celsius(65535 / 2) == expect_temp, "temp: raw=32767 matches the exact integer formula");
    }

    CHECK(sht45_raw_to_milli_rh(0) == 0, "rh: raw=0 clamps to 0 %RH (formula floor is -6%, clamped)");
    CHECK(sht45_raw_to_milli_rh(65535) == 100000, "rh: raw=65535 clamps to 100.000 %RH (formula ceiling is >100%, clamped)");
    /* Pick a raw value where the unclamped formula result is known to sit
     * safely inside [0,100] so the clamp is NOT exercised, to separately
     * confirm the un-clamped math itself. raw=3000 -> -6+125*3000/65535 ~= -0.28% (still clamps to 0). */
    int32_t mid_raw = 40000;
    int64_t expect_unclamped = -6000 + (int64_t)125000 * mid_raw / 65535;
    CHECK(sht45_raw_to_milli_rh((uint16_t)mid_raw) == expect_unclamped,
          "rh: raw=40000 (mid-range, no clamp needed) matches the exact integer formula");

    /* Both raw==0 and raw==65535 land outside [0,100]% before clamping --
     * confirms clamping is actually exercised, not a no-op that happens to
     * already be in range. */
    int64_t raw0_unclamped = -6000 + (int64_t)125000 * 0 / 65535;
    int64_t raw_max_unclamped = -6000 + (int64_t)125000 * 65535 / 65535;
    CHECK(raw0_unclamped < 0, "rh: raw=0's unclamped formula result is indeed negative (clamp is exercised)");
    CHECK(raw_max_unclamped > 100000, "rh: raw=65535's unclamped formula result is indeed >100% (clamp is exercised)");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
