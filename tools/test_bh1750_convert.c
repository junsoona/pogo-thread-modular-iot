/*
 * Host-compiled test for the BH1750 raw -> milli-lux conversion
 * (components/bh1750/bh1750_convert.c) -- section 4/13 of the follow-up
 * spec: "변환식과 바이트 순서를 host test로 검증".
 *
 * Links against the REAL, unmodified bh1750_convert.c (no ESP-IDF/FreeRTOS
 * dependency).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/bh1750/include \
 *      -o /tmp/test_bh1750_convert tools/test_bh1750_convert.c \
 *      components/bh1750/bh1750_convert.c \
 *   && /tmp/test_bh1750_convert
 */
#include <stdio.h>
#include <stdint.h>

#include "bh1750/bh1750_convert.h"

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
    /* Byte order: BH1750 sends MSB first. */
    CHECK(bh1750_bytes_to_raw(0x01, 0x00) == 256, "bytes_to_raw: MSB-first byte order (0x01,0x00) -> 256");
    CHECK(bh1750_bytes_to_raw(0x00, 0x01) == 1, "bytes_to_raw: (0x00,0x01) -> 1");
    CHECK(bh1750_bytes_to_raw(0xFF, 0xFF) == 65535, "bytes_to_raw: (0xFF,0xFF) -> 65535 (max)");
    CHECK(bh1750_bytes_to_raw(0x00, 0x00) == 0, "bytes_to_raw: (0x00,0x00) -> 0");

    /* Datasheet formula: lux = raw / 1.2 -- checked at several exact values
     * (chosen so integer division lands exactly, to avoid off-by-rounding
     * ambiguity in the test itself). */
    CHECK(bh1750_raw_to_milli_lux(0, 0) == 0, "convert: raw=0 -> 0 milli-lux");
    CHECK(bh1750_raw_to_milli_lux(12, 0) == 10000, "convert: raw=12 -> 10.000 lux (12/1.2=10)");
    CHECK(bh1750_raw_to_milli_lux(120, 0) == 100000, "convert: raw=120 -> 100.000 lux");
    CHECK(bh1750_raw_to_milli_lux(1200, 0) == 1000000, "convert: raw=1200 -> 1000.000 lux");
    CHECK(bh1750_raw_to_milli_lux(1, 0) == 833, "convert: raw=1 -> 0.833 lux (1000/1.2 truncated)");

    /* correction_permille: 0 and 1000 both mean "no correction". */
    CHECK(bh1750_raw_to_milli_lux(120, 1000) == 100000, "convert: correction=1000 (x1.000) == no correction");
    CHECK(bh1750_raw_to_milli_lux(120, 0) == bh1750_raw_to_milli_lux(120, 1000),
          "convert: correction=0 behaves the same as correction=1000");
    /* 500 permille = x0.5 */
    CHECK(bh1750_raw_to_milli_lux(120, 500) == 50000, "convert: correction=500 (x0.500) halves the result");
    /* 2000 permille = x2.0 */
    CHECK(bh1750_raw_to_milli_lux(120, 2000) == 200000, "convert: correction=2000 (x2.000) doubles the result");

    /* Max raw_count must not overflow a uint32_t through the *10000 step. */
    uint32_t max_result = bh1750_raw_to_milli_lux(65535, 0);
    CHECK(max_result == (uint32_t)(((uint64_t)65535 * 10000u) / 12u), "convert: max raw_count does not overflow/wrap");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
