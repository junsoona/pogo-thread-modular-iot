/*
 * Host-compilable boundary-value check for the polling-period calculation
 * in components/parent_service/parent_service.c
 * (parent_service_calc_polling_period_ms). Not part of the ESP-IDF build --
 * this exercises a byte-for-byte copy of that pure, integer-only function on
 * the host so the section-17 boundary table can be checked without hardware
 * or an ESP-IDF test harness.
 *
 * IMPORTANT: if you change the algorithm in parent_service.c, update the
 * copy below to match, or this test silently stops meaning anything.
 *
 * Build & run:
 *   cc -Wall -Wextra -o /tmp/test_polling_calc tools/test_polling_calc.c && /tmp/test_polling_calc
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x106

/* --- exact copy of parent_service_calc_polling_period_ms() --- */
static esp_err_t calc_polling_period_ms(uint32_t response_time_ms, uint32_t *out_period_ms)
{
    if (response_time_ms == 0 || out_period_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t period;
    if (response_time_ms < 50) {
        period = (uint64_t)response_time_ms * 15;
    } else if (response_time_ms < 100) {
        period = (uint64_t)response_time_ms * 10;
    } else if (response_time_ms < 500) {
        period = (uint64_t)response_time_ms * 8;
    } else if (response_time_ms <= 1000) {
        period = (uint64_t)response_time_ms * 3;
    } else if (response_time_ms <= 10000) {
        period = (uint64_t)response_time_ms * 3 / 2;
    } else {
        period = (uint64_t)response_time_ms * 1;
    }
    if (period > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_period_ms = (uint32_t)period;
    return ESP_OK;
}

/* --- exact copy of parent_service_calc_backoff_ms() --- */
static uint16_t calc_backoff_ms(const uint8_t uuid[8])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) {
        h ^= uuid[i];
        h *= 16777619u;
    }
    return (uint16_t)(h & 0xFF);
}

static int g_failures = 0;

static void expect_ok(uint32_t rt, uint32_t expected)
{
    uint32_t got = 0xFFFFFFFFu;
    esp_err_t err = calc_polling_period_ms(rt, &got);
    if (err != ESP_OK || got != expected) {
        printf("FAIL rt=%u: expected period=%u, got err=%d period=%u\n", rt, expected, err, got);
        g_failures++;
    } else {
        printf("ok   rt=%u -> period=%u\n", rt, got);
    }
}

static void expect_err(uint32_t rt)
{
    uint32_t got = 0;
    esp_err_t err = calc_polling_period_ms(rt, &got);
    if (err == ESP_OK) {
        printf("FAIL rt=%u: expected an error, got period=%u\n", rt, got);
        g_failures++;
    } else {
        printf("ok   rt=%u -> error %d (as expected)\n", rt, err);
    }
}

int main(void)
{
    /* section 17 boundary table */
    expect_err(0);                 /* RT=0 is an error, not "instant" */
    expect_ok(49, 49 * 15);         /* RT<50: M=15 */
    expect_ok(50, 50 * 10);         /* 50<=RT<100: M=10 */
    expect_ok(99, 99 * 10);
    expect_ok(100, 100 * 8);        /* 100<=RT<500: M=8 */
    expect_ok(499, 499 * 8);
    expect_ok(500, 500 * 3);        /* 500<=RT<=1000: M=3 */
    expect_ok(1000, 1000 * 3);
    expect_ok(1001, 1001 * 3 / 2);  /* 1000<RT<=10000: M=1.5 (integer RT*3/2) */
    expect_ok(10000, 10000 * 3 / 2);
    expect_ok(10001, 10001 * 1);    /* RT>10000: M=1 */

    /* the mock sensor's documented expected value (section 11/12) */
    expect_ok(120, 960);

    /* sanity on the backoff hash: deterministic, always in [0,255] */
    uint8_t uuid_a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t uuid_b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint16_t ba = calc_backoff_ms(uuid_a);
    uint16_t bb = calc_backoff_ms(uuid_b);
    printf("backoff(all-zero uuid)=%u backoff(1..8 uuid)=%u\n", ba, bb);
    if (ba > 255 || bb > 255) {
        printf("FAIL: backoff out of [0,255] range\n");
        g_failures++;
    }
    if (calc_backoff_ms(uuid_a) != ba) {
        printf("FAIL: backoff is not deterministic\n");
        g_failures++;
    }

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
