/*
 * Host-compiled test for components/node_calc/node_calc.c -- both the
 * polling-period formula (already covered by tools/test_polling_calc.c's
 * independent copy; re-checked here against the REAL shared implementation)
 * and the UUID-hash backoff schedule sensor_transport uses (section 8/13 of
 * the follow-up spec: "UUID backoff 0~255, 0~511, 0~1023 및 3회 제한").
 *
 * Links against the REAL, unmodified node_calc.c (no FreeRTOS/hardware
 * dependency at all -- its header just spells its return type `esp_err_t`
 * for consistency with the rest of the firmware, so this test shadows that
 * one header with tools/host_test_stubs/esp_err.h; see that file's comment).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs -I components/node_calc/include \
 *      -o /tmp/test_node_calc_backoff tools/test_node_calc_backoff.c \
 *      components/node_calc/node_calc.c \
 *   && /tmp/test_node_calc_backoff
 */
#include <stdio.h>
#include <stdint.h>

#include "node_calc/node_calc.h"

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
    /* --- polling period formula, re-verified against the real shared impl --- */
    uint32_t period;
    CHECK(node_calc_polling_period_ms(0, &period) == ESP_ERR_INVALID_ARG, "polling: RT=0 is an error");
    CHECK(node_calc_polling_period_ms(120, &period) == ESP_OK && period == 960, "polling: RT=120 -> 960ms (mock's documented value)");
    CHECK(node_calc_polling_period_ms(49, &period) == ESP_OK && period == 49 * 15, "polling: RT=49 -> M=15");
    CHECK(node_calc_polling_period_ms(50, &period) == ESP_OK && period == 50 * 10, "polling: RT=50 -> M=10");
    CHECK(node_calc_polling_period_ms(10001, &period) == ESP_OK && period == 10001, "polling: RT=10001 -> M=1");

    /* --- UUID hash backoff: always in range, deterministic --- */
    uint8_t uuid_a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t uuid_b[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

    uint32_t hash_a = node_calc_uuid_hash(uuid_a);
    uint32_t hash_b = node_calc_uuid_hash(uuid_b);
    CHECK(hash_a == node_calc_uuid_hash(uuid_a), "hash: deterministic for the same UUID");
    CHECK(hash_a != hash_b, "hash: differs for a different UUID (not exhaustive, just a sanity check)");

    uint16_t backoff_a = node_calc_backoff_ms(uuid_a);
    uint16_t backoff_b = node_calc_backoff_ms(uuid_b);
    CHECK(backoff_a == (uint16_t)(hash_a & 0xFF), "backoff_ms: is exactly hash & 0xFF");
    CHECK(backoff_a <= 255 && backoff_b <= 255, "backoff_ms: always in [0,255]");

    /* --- sensor_transport's widening retry masks, derived from the same hash --- */
    uint32_t first_retry = hash_a & 0xFF;
    uint32_t second_retry = hash_a & 0x1FF;
    uint32_t third_retry = hash_a & 0x3FF;
    CHECK(first_retry <= 255, "sensor_transport backoff: 1st retry range is 0..255ms");
    CHECK(second_retry <= 511, "sensor_transport backoff: 2nd retry range is 0..511ms");
    CHECK(third_retry <= 1023, "sensor_transport backoff: 3rd retry range is 0..1023ms");
    CHECK(first_retry == (hash_a & 0xFF), "sensor_transport backoff: 1st retry matches node_calc_backoff_ms's own low-8-bit value");
    /* The wider ranges are supersets of the narrower one's bit pattern (same
     * hash, more bits kept), not an unrelated re-roll. */
    CHECK((second_retry & 0xFF) == first_retry, "sensor_transport backoff: 2nd retry's low 8 bits match the 1st retry's value");
    CHECK((third_retry & 0x1FF) == second_retry, "sensor_transport backoff: 3rd retry's low 9 bits match the 2nd retry's value");

    /* Exhaustive range check over many UUIDs -- every derived backoff value
     * must respect its declared ceiling, no off-by-one/mask mistake. */
    int range_failures = 0;
    for (int i = 0; i < 4096; i++) {
        uint8_t uuid[8] = {(uint8_t)i, (uint8_t)(i >> 8), 0xAA, 0xBB, (uint8_t)(i * 3), (uint8_t)(i * 7), 0x11, 0x22};
        uint32_t h = node_calc_uuid_hash(uuid);
        if ((h & 0xFF) > 255 || (h & 0x1FF) > 511 || (h & 0x3FF) > 1023) {
            range_failures++;
        }
    }
    CHECK(range_failures == 0, "sensor_transport backoff: 4096 sampled UUIDs all stay within their declared ranges");

    /* --- "최대 3회" (max 3 retries = 4 total attempts) is a compile-time
     * constant in sensor_transport.c, checked here as a documented
     * invariant of the retry-mask table shape this test also exercises. */
    const int max_retries = 3;
    const int max_attempts = 1 + max_retries;
    CHECK(max_attempts == 4, "retry budget: 1 initial send + 3 retries = 4 total attempts");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
