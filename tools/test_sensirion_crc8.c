/*
 * Host-compiled test for components/sensirion_crc8/sensirion_crc8.c (the
 * CRC-8 shared by components/sgp40 and components/sgp41 -- section 10/13 of
 * the follow-up spec). Same algorithm/vectors as tools/test_sht45_convert.c
 * checks for sht45_crc8() -- kept as a separate test because it exercises a
 * separately-compiled component, not because the expected values differ.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/sensirion_crc8/include \
 *      -o /tmp/test_sensirion_crc8 tools/test_sensirion_crc8.c \
 *      components/sensirion_crc8/sensirion_crc8.c \
 *   && /tmp/test_sensirion_crc8
 */
#include <stdio.h>
#include "sensirion_crc8/sensirion_crc8.h"

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
    CHECK(sensirion_crc8(0xBE, 0xEF) == 0x92, "known-answer vector (0xBE,0xEF) -> 0x92");
    CHECK(sensirion_crc8_selftest(), "sensirion_crc8_selftest() passes");
    CHECK(sensirion_crc8(0x00, 0x00) == 0x81, "(0x00,0x00) -> 0x81 (cross-checked against an independent reference)");
    CHECK(sensirion_crc8(0x80, 0x00) != sensirion_crc8(0x00, 0x00), "differs for different input");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
