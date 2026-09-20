/*
 * Host-compiled I2C-layer validation for components/bh1750/bh1750.c's
 * read_measurement() timeout handling -- the SAME real bug tools/
 * test_sgp40_driver.c covers for SGP40 (see that file's own top comment for
 * the full real-world-impact explanation, which applies here identically):
 * the CALLER's own tick-budget path used to return ESP_ERR_TIMEOUT without
 * resetting `measuring`/`status`, fixed this pass in all four I2C sensor
 * drivers (SHT45, BH1750, SGP40, SGP41) identically. This driver's own
 * pre-fix comment ("measurement may still be running") was true of the
 * physical BH1750 conversion, which the datasheet says completes and
 * auto-powers-down on its own regardless -- but that observation never
 * extended to `status`, which is this driver's own bookkeeping, not the
 * sensor's physical state.
 *
 * Links the REAL, UNMODIFIED components/bh1750/bh1750.c +
 * components/bh1750/bh1750_convert.c against
 * tools/host_test_stubs/host_i2c_fake_device.c (a fake I2C device). Unlike
 * SHT45/SGP40/SGP41, BH1750's own init() never calls i2c_master_receive()
 * at all (no self-test/serial-number readback -- just POWER_ON + RESET
 * command writes), so no response needs to be queued for init here.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I components/bh1750/include \
 *      -I components/sensor_provider/include -I components/sensor_channel/include \
 *      -I components/module_config/include \
 *      -o /tmp/test_bh1750_driver tools/test_bh1750_driver.c \
 *      components/bh1750/bh1750.c components/bh1750/bh1750_convert.c \
 *      tools/host_test_stubs/host_i2c_fake_device.c \
 *   && /tmp/test_bh1750_driver
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "bh1750/bh1750.h"
#include "bh1750/bh1750_convert.h"
#include "module_config/module_config.h"
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

static module_config_t base_valid_config(void)
{
    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.driver_id = SENSOR_DRIVER_ID_BH1750;
    cfg.bus_type = MODULE_BUS_I2C;
    cfg.bus_number = 0;
    cfg.i2c_sda_gpio = 0;
    cfg.i2c_scl_gpio = 1;
    cfg.i2c_address = 0x23;
    cfg.measurement_timeout_ms = 300; /* matches configs/prototype_bh1750.json */
    cfg.calibration[0] = 1000;        /* correction_permille=1000 (no correction), see bh1750_convert.c */
    return cfg;
}

/* Queues a 2-byte raw measurement response (no CRC -- BH1750 has none). */
static void queue_measurement(uint16_t raw)
{
    uint8_t resp[2] = {(uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF)};
    host_i2c_fake_device_queue_response(resp, sizeof(resp));
}

static sensor_provider_ctx_t *init_ok(void)
{
    host_i2c_fake_device_reset();
    module_config_t cfg = base_valid_config();
    sensor_provider_ctx_t *ctx = NULL;
    esp_err_t err = g_bh1750_provider_ops.init(&cfg, &ctx);
    CHECK(err == ESP_OK && ctx != NULL, "bh1750_init() succeeds (POWER_ON + RESET, no readback)");
    CHECK(g_bh1750_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK right after a successful init");
    return ctx;
}

/* --- Scenario: the caller's own tick budget running out mid-measurement -- */
static void scenario_tick_budget_timeout(void)
{
    printf("\n-- scenario: read_measurement()'s tick-budget timeout (this driver's own real bug fixed this "
           "pass, identical in shape to SHT45/SGP40/SGP41's) correctly marks the driver TIMED OUT, not "
           "silently 'still measuring' --\n");

    sensor_provider_ctx_t *ctx = init_ok();
    int baseline_receives = host_i2c_fake_device_receive_call_count(); /* 0 -- BH1750 init never reads */
    CHECK(baseline_receives == 0, "bh1750_init() itself never calls i2c_master_receive()");

    esp_err_t err = g_bh1750_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    /* timeout_ticks=1 (real milliseconds in this host stub, see
     * tools/host_test_stubs/freertos/task.h) is far smaller than both
     * BH1750_CONVERSION_MS_MAX (180ms) and ctx->measurement_timeout_ms
     * (300ms) -- see test_sgp40_driver.c's own comment for why the ~5ms
     * poll interval (BH1750_POLL_DELAY_TICKS) makes this deterministic. No
     * response is queued -- this must never reach i2c_master_receive() at
     * all. */
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_bh1750_provider_ops.read_measurement(ctx, &out, 1);
    CHECK(err == ESP_ERR_TIMEOUT, "read_measurement() returns ESP_ERR_TIMEOUT when the caller's tick budget expires");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "the tick-budget timeout returns BEFORE ever attempting the real I2C read");

    CHECK(g_bh1750_provider_ops.get_status(ctx) == SENSOR_STATUS_TIMEOUT,
          "get_status() reports SENSOR_STATUS_TIMEOUT immediately after the tick-budget timeout -- the physical "
          "BH1750 conversion may well complete/auto-power-down on its own regardless (datasheet behavior), but "
          "this driver's OWN status bookkeeping must reflect the timeout it actually observed");

    /* Synthetic driver-API-contract probe (see test_sgp40_driver.c's own
     * comment on why this exact sequence never happens through
     * sensor_service.c's real sensor_service_measure_once()). */
    err = g_bh1750_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_ERR_INVALID_STATE,
          "a further read_measurement() call with no intervening start_measurement() correctly reports "
          "ESP_ERR_INVALID_STATE -- proving `measuring` was actually reset to false, not left true");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "...and never attempts a real I2C read against the stale measurement at all");
}

/* --- Scenario: normal completion still works after the fix --- */
static void scenario_normal_completion_unaffected(void)
{
    printf("\n-- scenario: a normal (generous timeout_ticks) measurement still completes successfully -- the "
           "fix does not touch this path --\n");

    sensor_provider_ctx_t *ctx = init_ok();

    esp_err_t err = g_bh1750_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    uint16_t raw = 21600; /* -> 18000 millilux at correction_permille=1000, see bh1750_convert.c's own formula */
    queue_measurement(raw);
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_bh1750_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_OK, "read_measurement() with a generous tick budget still succeeds normally");
    CHECK(host_i2c_fake_device_receive_call_count() == 1, "exactly one real I2C read was attempted for this "
                                                            "measurement");
    CHECK(g_bh1750_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK after a normal successful read");
    CHECK(out.channel_count == 1, "BH1750 always reports exactly 1 channel (ILLUMINANCE)");
    CHECK(out.channels[0].type == SENSOR_CHANNEL_ILLUMINANCE &&
              (uint32_t) out.channels[0].value == bh1750_raw_to_milli_lux(raw, 1000),
          "the channel value matches the real bh1750_raw_to_milli_lux() conversion of the raw word this test "
          "queued");

    err = g_bh1750_provider_ops.read_measurement(ctx, &out, 1000);
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
