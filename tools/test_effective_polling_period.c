/*
 * Host-compiled test for effective_polling_period_ms = max(dynamic_period,
 * driver_minimum) -- section 10/13 of the follow-up spec: "sensor minimum
 * polling 적용". Mirrors sensor_service_compute_effective_period_ms()
 * (components/sensor_service/sensor_service.c) exactly -- a two-line pure
 * function, reproduced here rather than linked because the surrounding file
 * pulls in FreeRTOS/esp_timer/sensor_provider, same reasoning as
 * tools/test_polling_calc.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_effective_polling_period tools/test_effective_polling_period.c \
 *   && /tmp/test_effective_polling_period
 */
#include <stdio.h>
#include <stdint.h>

/* --- exact copy of sensor_service_compute_effective_period_ms() --- */
static uint32_t compute_effective_period_ms(uint32_t dynamic_period_ms, uint32_t driver_min_period_ms)
{
    return (dynamic_period_ms > driver_min_period_ms) ? dynamic_period_ms : driver_min_period_ms;
}

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
    /* BH1750/SHT45 today: driver_min=0 -- the dynamic (response-time-derived)
     * value always wins, unmodified. */
    CHECK(compute_effective_period_ms(960, 0) == 960, "driver_min=0 (BH1750/SHT45): effective == dynamic, unmodified");
    CHECK(compute_effective_period_ms(0, 0) == 0, "both zero: effective is 0 (no polling configured yet)");

    /* SGP4x-style driver requiring exactly 1000ms minimum (section 10). */
    const uint32_t sgp4x_min = 1000;
    CHECK(compute_effective_period_ms(500, sgp4x_min) == 1000, "dynamic (500) below driver_min (1000) is raised to 1000");
    CHECK(compute_effective_period_ms(1000, sgp4x_min) == 1000, "dynamic exactly at driver_min stays at driver_min");
    CHECK(compute_effective_period_ms(1500, sgp4x_min) == 1500, "dynamic (1500) above driver_min (1000) is left alone");

    /* A very fast sensor's own dynamic formula result must never be allowed
     * to undercut a slower algorithm's hard minimum -- this is the entire
     * point of the max() combination. */
    CHECK(compute_effective_period_ms(1, 1000) == 1000, "an extremely small dynamic period is still clamped up to driver_min");

    /* The SAME value must be usable for both what gets ACKed/applied and the
     * actual measurement task's schedule -- checked here as "calling twice
     * with the same inputs gives the same answer", i.e. the function is
     * pure/deterministic, not stateful. */
    CHECK(compute_effective_period_ms(700, 1000) == compute_effective_period_ms(700, 1000),
          "deterministic: repeated calls with the same inputs agree");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
