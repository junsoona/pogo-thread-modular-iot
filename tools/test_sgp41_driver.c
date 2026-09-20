/*
 * Host-compiled I2C-layer validation for components/sgp41/sgp41.c's
 * read_measurement() timeout handling -- the SAME real bug as
 * tools/test_sgp40_driver.c covers for SGP40 (see that file's own top
 * comment for the full explanation): the CALLER's own tick-budget path
 * used to return ESP_ERR_TIMEOUT without resetting `measuring`/`status`,
 * fixed this pass in both drivers identically.
 *
 * A fresh init's first measurement always falls inside SGP41's own 10s
 * conditioning window (SGP41_CONDITIONING_WINDOW_MS, see sgp41.c), so this
 * test exercises the conditioning command path (3-byte VOC-only response) --
 * the FIRST real code path any real boot takes too, and a representative
 * one: the tick-budget bug is in the shared polling loop, before either
 * command variant's own response-length branch is ever reached.
 *
 * Links the REAL, UNMODIFIED components/sgp41/sgp41.c against
 * tools/host_test_stubs/host_i2c_fake_device.c, components/sensirion_crc8/
 * sensirion_crc8.c, and the vendored components/sensirion_gas_index_algorithm.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I components/sgp41/include \
 *      -I components/sensor_provider/include -I components/sensor_channel/include \
 *      -I components/module_config/include -I components/sensirion_crc8/include \
 *      -I components/sensirion_gas_index_algorithm/include \
 *      -o /tmp/test_sgp41_driver tools/test_sgp41_driver.c \
 *      components/sgp41/sgp41.c components/sensirion_crc8/sensirion_crc8.c \
 *      components/sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.c \
 *      tools/host_test_stubs/host_i2c_fake_device.c -lm \
 *   && /tmp/test_sgp41_driver
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sgp41/sgp41.h"
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

#define SGP41_SELF_TEST_OK 0xD400u

static module_config_t base_valid_config(void)
{
    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.driver_id = SENSOR_DRIVER_ID_SGP41;
    cfg.bus_type = MODULE_BUS_I2C;
    cfg.bus_number = 0;
    cfg.i2c_sda_gpio = 0;
    cfg.i2c_scl_gpio = 1;
    cfg.i2c_address = 0x59;
    cfg.measurement_timeout_ms = 200; /* matches configs/prototype_sgp41.json */
    return cfg;
}

static void queue_self_test_pass(void)
{
    uint8_t resp[3];
    resp[0] = (uint8_t)(SGP41_SELF_TEST_OK >> 8);
    resp[1] = (uint8_t)(SGP41_SELF_TEST_OK & 0xFF);
    resp[2] = sensirion_crc8(resp[0], resp[1]);
    host_i2c_fake_device_queue_response(resp, sizeof(resp));
}

/* Queues a conditioning-window response: 3 bytes, SRAW_VOC only (no NOx --
 * see sgp41.c's own comment on why the conditioning command never returns
 * it). */
static void queue_conditioning_measurement(uint16_t sraw_voc)
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
    esp_err_t err = g_sgp41_provider_ops.init(&cfg, &ctx);
    CHECK(err == ESP_OK && ctx != NULL, "sgp41_init() succeeds against a real self-test PASS response");
    CHECK(g_sgp41_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK right after a successful init");
    return ctx;
}

/* --- Scenario: the caller's own tick budget running out mid-measurement -- */
static void scenario_tick_budget_timeout(void)
{
    printf("\n-- scenario: read_measurement()'s tick-budget timeout (this driver's own real bug fixed this "
           "pass, identical in shape to SGP40's) correctly marks the driver TIMED OUT, not silently 'still "
           "measuring' --\n");

    sensor_provider_ctx_t *ctx = init_ok();
    /* init_ok() already consumed one i2c_master_receive() call for the
     * self-test result -- everything below must never add another one. */
    int baseline_receives = host_i2c_fake_device_receive_call_count();

    esp_err_t err = g_sgp41_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds (a fresh init is always inside the 10s conditioning "
                          "window, so this issues the conditioning command)");

    /* timeout_ticks=1 (real milliseconds in this host stub, see
     * tools/host_test_stubs/freertos/task.h) is far smaller than both
     * SGP41_CONDITIONING_MS_MAX (50ms) and ctx->measurement_timeout_ms
     * (200ms) -- see test_sgp40_driver.c's own comment for why the ~2ms
     * poll interval makes this deterministic, not timing-flaky. No response
     * is queued -- this must never reach i2c_master_receive() at all. */
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_sgp41_provider_ops.read_measurement(ctx, &out, 1);
    CHECK(err == ESP_ERR_TIMEOUT, "read_measurement() returns ESP_ERR_TIMEOUT when the caller's tick budget expires");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "the tick-budget timeout returns BEFORE ever attempting the real I2C read");

    CHECK(g_sgp41_provider_ops.get_status(ctx) == SENSOR_STATUS_TIMEOUT,
          "get_status() reports SENSOR_STATUS_TIMEOUT immediately after the tick-budget timeout");

    err = g_sgp41_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_ERR_INVALID_STATE,
          "a further read_measurement() call with no intervening start_measurement() correctly reports "
          "ESP_ERR_INVALID_STATE -- proving `measuring` was actually reset to false, not left true");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "...and never attempts a real I2C read against the stale measurement at all");
}

/* --- Scenario: normal completion still works after the fix --- */
static void scenario_normal_completion_unaffected(void)
{
    printf("\n-- scenario: a normal (generous timeout_ticks) conditioning-window measurement still completes "
           "successfully -- the fix does not touch this path --\n");

    sensor_provider_ctx_t *ctx = init_ok();
    int baseline_receives = host_i2c_fake_device_receive_call_count(); /* the self-test's own read, see init_ok() */

    esp_err_t err = g_sgp41_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    queue_conditioning_measurement(23456);
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_sgp41_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_OK, "read_measurement() with a generous tick budget still succeeds normally");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives + 1,
          "exactly one real I2C read was attempted for this measurement");
    CHECK(g_sgp41_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK after a normal successful read");
    CHECK(out.channel_count == 2,
          "during conditioning, SGP41 reports exactly 2 channels (VOC_RAW, VOC_INDEX) -- never a fabricated NOx "
          "reading, see sgp41.c's own comment");
    CHECK(out.channels[0].type == SENSOR_CHANNEL_VOC_RAW && out.channels[0].value == 23456,
          "channel 0 is the raw SRAW_VOC value this test queued, unchanged");
    CHECK(out.channels[0].status == SENSOR_CHANNEL_STATUS_CONDITIONING && out.channels[1].status == SENSOR_CHANNEL_STATUS_CONDITIONING,
          "both channels are marked CONDITIONING, not OK, this early after init");

    err = g_sgp41_provider_ops.read_measurement(ctx, &out, 1000);
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
