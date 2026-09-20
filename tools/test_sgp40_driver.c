/*
 * Host-compiled I2C-layer validation for components/sgp40/sgp40.c's
 * read_measurement() timeout handling -- specifically the CALLER's own
 * tick-budget path (the `timeout_ticks` parameter FreeRTOS callers pass in,
 * distinct from the driver's own elapsed-real-time-vs-measurement_timeout_ms
 * check a few lines above it in the same loop). Before this pass, that path
 * returned ESP_ERR_TIMEOUT WITHOUT resetting `measuring` to false or
 * `status` to SENSOR_STATUS_TIMEOUT.
 *
 * Real-world impact of the bug (this is what actually happens in
 * production, via components/sensor_service/sensor_service.c's
 * sensor_service_measure_once(), this driver's only real caller):
 * `status` stays whatever it was (SENSOR_STATUS_OK) instead of
 * SENSOR_STATUS_TIMEOUT for that one polling cycle, so
 * sensor_service_get_status() misreports a timed-out reading as healthy
 * until the NEXT periodic measurement's own start_measurement()/
 * read_measurement() pair naturally overwrites it -- sensor_service_measure_once()
 * always calls start_measurement() again immediately before every
 * read_measurement(), so `measuring` staying stuck true is never actually
 * OBSERVED by this driver's real caller. What scenario_tick_budget_timeout()
 * below ALSO checks (calling read_measurement() a second time with no
 * intervening start_measurement()) is a synthetic driver-API-contract probe,
 * not a sequence sensor_service.c ever produces -- see that scenario's own
 * comment for exactly what it demonstrates: without the fix, that second
 * call would silently re-enter the polling loop against the STALE original
 * start_time_us instead of cleanly returning ESP_ERR_INVALID_STATE.
 *
 * This test drives the real read_measurement() into exactly the buggy
 * tick-budget path and asserts both halves of the fix directly.
 *
 * Links the REAL, UNMODIFIED components/sgp40/sgp40.c against
 * tools/host_test_stubs/host_i2c_fake_device.c (a fake I2C device, see that
 * file's own header) and the real components/sensirion_crc8/sensirion_crc8.c
 * + vendored components/sensirion_gas_index_algorithm (sgp40.c calls both
 * directly; neither is reimplemented here).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I components/sgp40/include \
 *      -I components/sensor_provider/include -I components/sensor_channel/include \
 *      -I components/module_config/include -I components/sensirion_crc8/include \
 *      -I components/sensirion_gas_index_algorithm/include \
 *      -o /tmp/test_sgp40_driver tools/test_sgp40_driver.c \
 *      components/sgp40/sgp40.c components/sensirion_crc8/sensirion_crc8.c \
 *      components/sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.c \
 *      tools/host_test_stubs/host_i2c_fake_device.c -lm \
 *   && /tmp/test_sgp40_driver
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sgp40/sgp40.h"
#include "module_config/module_config.h"
#include "sensirion_crc8/sensirion_crc8.h"
#include "host_i2c_fake_device.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        g_checks++;                                                                                                  \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

#define SGP40_SELF_TEST_OK 0xD400u

static module_config_t base_valid_config(void)
{
    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.driver_id = SENSOR_DRIVER_ID_SGP40;
    cfg.bus_type = MODULE_BUS_I2C;
    cfg.bus_number = 0;
    cfg.i2c_sda_gpio = 0;
    cfg.i2c_scl_gpio = 1;
    cfg.i2c_address = 0x59;
    cfg.measurement_timeout_ms = 200; /* matches configs/prototype_sgp40.json */
    return cfg;
}

/* Queues the self-test response sgp40_init() reads back after issuing
 * SGP40_CMD_SELF_TEST -- 3 bytes: msb, lsb, crc8(msb,lsb). Real, unmodified
 * sensirion_crc8() computes the trailer, exactly as the real sensor would
 * be expected to. */
static void queue_self_test_pass(void)
{
    uint8_t resp[3];
    resp[0] = (uint8_t)(SGP40_SELF_TEST_OK >> 8);
    resp[1] = (uint8_t)(SGP40_SELF_TEST_OK & 0xFF);
    resp[2] = sensirion_crc8(resp[0], resp[1]);
    host_i2c_fake_device_queue_response(resp, sizeof(resp));
}

/* Queues a measurement response (3 bytes: sraw_voc msb/lsb + crc8). */
static void queue_measurement(uint16_t sraw_voc)
{
    uint8_t resp[3];
    resp[0] = (uint8_t)(sraw_voc >> 8);
    resp[1] = (uint8_t)(sraw_voc & 0xFF);
    resp[2] = sensirion_crc8(resp[0], resp[1]);
    host_i2c_fake_device_queue_response(resp, sizeof(resp));
}

static sensor_provider_ctx_t *init_ok(void)
{
    host_i2c_fake_device_reset();
    queue_self_test_pass();
    module_config_t cfg = base_valid_config();
    sensor_provider_ctx_t *ctx = NULL;
    esp_err_t err = g_sgp40_provider_ops.init(&cfg, &ctx);
    CHECK(err == ESP_OK && ctx != NULL, "sgp40_init() succeeds against a real self-test PASS response");
    CHECK(g_sgp40_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK right after a successful init");
    return ctx;
}

/* --- Scenario: the caller's own tick budget running out mid-measurement -- */
static void scenario_tick_budget_timeout(void)
{
    printf("\n-- scenario: read_measurement()'s tick-budget timeout (a SEPARATE path from the "
           "elapsed_ms-vs-measurement_timeout_ms check, this driver's own real bug fixed this pass) "
           "correctly marks the driver TIMED OUT, not silently 'still measuring' --\n");

    sensor_provider_ctx_t *ctx = init_ok();
    /* init_ok() already consumed one i2c_master_receive() call for the
     * self-test result -- everything below must never add another one. */
    int baseline_receives = host_i2c_fake_device_receive_call_count();

    esp_err_t err = g_sgp40_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    /* timeout_ticks=1 (this host stub's TickType_t is real milliseconds,
     * see tools/host_test_stubs/freertos/task.h's own top comment) is far
     * smaller than SGP40_MEASURE_MS_MAX (30ms) and ctx->measurement_timeout_ms
     * (200ms) -- the poll loop's ~2ms-per-iteration real sleep
     * (SGP40_POLL_DELAY_TICKS) means the tick-budget branch trips on its
     * very next check, long before either of the other two ever could. No
     * response is queued -- if this ever reached i2c_master_receive() (it
     * must not), that call would itself fail, which would also make this
     * scenario's own assertions fail below, so a regression here cannot
     * silently pass either. */
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_sgp40_provider_ops.read_measurement(ctx, &out, 1);
    CHECK(err == ESP_ERR_TIMEOUT, "read_measurement() returns ESP_ERR_TIMEOUT when the caller's tick budget expires");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "the tick-budget timeout returns BEFORE ever attempting the real I2C read -- this is purely this "
          "driver's own polling-loop bookkeeping, not a bus-level failure");

    /* The fix, assertion 1: status must actually become TIMEOUT, not stay
     * whatever it was (SENSOR_STATUS_OK, set at the end of
     * start_measurement()) forever after. */
    CHECK(g_sgp40_provider_ops.get_status(ctx) == SENSOR_STATUS_TIMEOUT,
          "get_status() reports SENSOR_STATUS_TIMEOUT immediately after the tick-budget timeout (this is exactly "
          "the state transition that was missing before this pass's fix)");

    /* The fix, assertion 2: `measuring` must have been reset to false. This
     * specific call sequence (read_measurement() again with NO intervening
     * start_measurement()) never happens through sensor_service.c's real
     * sensor_service_measure_once(), which always pairs them -- this is a
     * synthetic probe of the driver's own API contract, not a reproduction
     * of a real caller's sequence. Without the fix, this second call would
     * NOT hit the `if (!ctx->measuring) return ESP_ERR_INVALID_STATE;` guard
     * at all (measuring is, buggily, still true) -- it would instead
     * silently re-enter the polling loop against the now-stale
     * start_time_us and race ahead to a real (and here, unanswered)
     * i2c_master_receive() call. */
    err = g_sgp40_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_ERR_INVALID_STATE,
          "a further read_measurement() call with no intervening start_measurement() correctly reports "
          "ESP_ERR_INVALID_STATE -- proving `measuring` was actually reset to false, not left true");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "...and, unlike the pre-fix behavior, never attempts a real I2C read against the stale measurement at "
          "all");
}

/* --- Scenario: normal completion still works after the fix (the fix only
 * touches the tick-budget branch; the elapsed_ms-based timeout and the
 * happy path must behave exactly as before) --- */
static void scenario_normal_completion_unaffected(void)
{
    printf("\n-- scenario: a normal (generous timeout_ticks) measurement still completes successfully -- the "
           "fix does not touch this path --\n");

    sensor_provider_ctx_t *ctx = init_ok();
    int baseline_receives = host_i2c_fake_device_receive_call_count(); /* the self-test's own read, see init_ok() */

    esp_err_t err = g_sgp40_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    queue_measurement(12345);
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_sgp40_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_OK, "read_measurement() with a generous tick budget still succeeds normally");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives + 1,
          "exactly one real I2C read was attempted for this measurement");
    CHECK(g_sgp40_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK after a normal successful read");
    CHECK(out.channel_count == 2, "SGP40 always reports exactly 2 channels (VOC_RAW, VOC_INDEX)");
    CHECK(out.channels[0].type == SENSOR_CHANNEL_VOC_RAW && out.channels[0].value == 12345,
          "channel 0 is the raw SRAW_VOC value this test queued, unchanged");

    /* A further read without a new start_measurement() must still be
     * rejected -- confirms the normal (non-timeout) path also resets
     * `measuring` to false, same as it always did. */
    err = g_sgp40_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_ERR_INVALID_STATE, "a further read_measurement() call with no new start_measurement() is "
                                        "rejected after a normal completion too");
}

int main(void)
{
    scenario_tick_budget_timeout();
    scenario_normal_completion_unaffected();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
