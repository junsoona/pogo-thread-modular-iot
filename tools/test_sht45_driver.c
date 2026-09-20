/*
 * Host-compiled I2C-layer validation for components/sht45/sht45.c's
 * read_measurement() timeout handling -- the SAME real bug tools/
 * test_sgp40_driver.c covers for SGP40 (see that file's own top comment for
 * the full real-world-impact explanation, which applies here identically):
 * the CALLER's own tick-budget path used to return ESP_ERR_TIMEOUT without
 * resetting `measuring`/`status`, fixed this pass in all four I2C sensor
 * drivers (SHT45, BH1750, SGP40, SGP41) identically.
 *
 * Links the REAL, UNMODIFIED components/sht45/sht45.c +
 * components/sht45/sht45_convert.c against
 * tools/host_test_stubs/host_i2c_fake_device.c (a fake I2C device).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I components/sht45/include \
 *      -I components/sensor_provider/include -I components/sensor_channel/include \
 *      -I components/module_config/include \
 *      -o /tmp/test_sht45_driver tools/test_sht45_driver.c \
 *      components/sht45/sht45.c components/sht45/sht45_convert.c \
 *      tools/host_test_stubs/host_i2c_fake_device.c \
 *   && /tmp/test_sht45_driver
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sht45/sht45.h"
#include "sht45/sht45_convert.h"
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
    cfg.driver_id = SENSOR_DRIVER_ID_SHT45;
    cfg.bus_type = MODULE_BUS_I2C;
    cfg.bus_number = 0;
    cfg.i2c_sda_gpio = 0;
    cfg.i2c_scl_gpio = 1;
    cfg.i2c_address = 0x44;
    cfg.measurement_timeout_ms = 200; /* matches configs/prototype_sht45.json */
    return cfg;
}

/* Queues the 6-byte READ_SERIAL response sht45_init() reads back: two
 * 16-bit words, each with its own real sht45_crc8() trailer. */
static void queue_serial_number(uint16_t word0, uint16_t word1)
{
    uint8_t resp[6];
    resp[0] = (uint8_t)(word0 >> 8);
    resp[1] = (uint8_t)(word0 & 0xFF);
    resp[2] = sht45_crc8(resp[0], resp[1]);
    resp[3] = (uint8_t)(word1 >> 8);
    resp[4] = (uint8_t)(word1 & 0xFF);
    resp[5] = sht45_crc8(resp[3], resp[4]);
    host_i2c_fake_device_queue_response(resp, sizeof(resp));
}

/* Queues a measurement response: [T_msb, T_lsb, T_crc, RH_msb, RH_lsb, RH_crc]. */
static void queue_measurement(uint16_t raw_t, uint16_t raw_rh)
{
    uint8_t resp[6];
    resp[0] = (uint8_t)(raw_t >> 8);
    resp[1] = (uint8_t)(raw_t & 0xFF);
    resp[2] = sht45_crc8(resp[0], resp[1]);
    resp[3] = (uint8_t)(raw_rh >> 8);
    resp[4] = (uint8_t)(raw_rh & 0xFF);
    resp[5] = sht45_crc8(resp[3], resp[4]);
    host_i2c_fake_device_queue_response(resp, sizeof(resp));
}

static sensor_provider_ctx_t *init_ok(void)
{
    host_i2c_fake_device_reset();
    queue_serial_number(0x1234, 0x5678);
    module_config_t cfg = base_valid_config();
    sensor_provider_ctx_t *ctx = NULL;
    esp_err_t err = g_sht45_provider_ops.init(&cfg, &ctx);
    CHECK(err == ESP_OK && ctx != NULL, "sht45_init() succeeds against a real serial-number-read response");
    CHECK(g_sht45_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK right after a successful init");
    return ctx;
}

/* --- Scenario: the caller's own tick budget running out mid-measurement -- */
static void scenario_tick_budget_timeout(void)
{
    printf("\n-- scenario: read_measurement()'s tick-budget timeout (this driver's own real bug fixed this "
           "pass, identical in shape to SGP40/SGP41's) correctly marks the driver TIMED OUT, not silently "
           "'still measuring' --\n");

    sensor_provider_ctx_t *ctx = init_ok();
    /* init_ok() already consumed one i2c_master_receive() call for the
     * serial number -- everything below must never add another one. */
    int baseline_receives = host_i2c_fake_device_receive_call_count();

    esp_err_t err = g_sht45_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    /* timeout_ticks=1 (real milliseconds in this host stub, see
     * tools/host_test_stubs/freertos/task.h) is far smaller than both
     * SHT45_MEASURE_MS_MAX (10ms) and ctx->measurement_timeout_ms (200ms) --
     * see test_sgp40_driver.c's own comment for why the ~2ms poll interval
     * makes this deterministic. No response is queued -- this must never
     * reach i2c_master_receive() at all. */
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_sht45_provider_ops.read_measurement(ctx, &out, 1);
    CHECK(err == ESP_ERR_TIMEOUT, "read_measurement() returns ESP_ERR_TIMEOUT when the caller's tick budget expires");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives,
          "the tick-budget timeout returns BEFORE ever attempting the real I2C read");

    CHECK(g_sht45_provider_ops.get_status(ctx) == SENSOR_STATUS_TIMEOUT,
          "get_status() reports SENSOR_STATUS_TIMEOUT immediately after the tick-budget timeout");

    /* Synthetic driver-API-contract probe (see test_sgp40_driver.c's own
     * comment on why this exact sequence -- read_measurement() again with
     * NO intervening start_measurement() -- never happens through
     * sensor_service.c's real sensor_service_measure_once(), which always
     * pairs them). Without the fix, this would silently re-enter the
     * polling loop against the stale start_time_us instead of cleanly
     * failing. */
    err = g_sht45_provider_ops.read_measurement(ctx, &out, 1000);
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
    int baseline_receives = host_i2c_fake_device_receive_call_count(); /* the serial-number read, see init_ok() */

    esp_err_t err = g_sht45_provider_ops.start_measurement(ctx);
    CHECK(err == ESP_OK, "start_measurement() succeeds");

    uint16_t raw_t = 20000;
    uint16_t raw_rh = 30000;
    queue_measurement(raw_t, raw_rh);
    sensor_measurement_t out;
    memset(&out, 0, sizeof(out));
    err = g_sht45_provider_ops.read_measurement(ctx, &out, 1000);
    CHECK(err == ESP_OK, "read_measurement() with a generous tick budget still succeeds normally");
    CHECK(host_i2c_fake_device_receive_call_count() == baseline_receives + 1,
          "exactly one real I2C read was attempted for this measurement");
    CHECK(g_sht45_provider_ops.get_status(ctx) == SENSOR_STATUS_OK, "status is OK after a normal successful read");
    CHECK(out.channel_count == 2, "SHT45 always reports exactly 2 channels (TEMPERATURE, HUMIDITY)");
    CHECK(out.channels[0].type == SENSOR_CHANNEL_TEMPERATURE &&
              out.channels[0].value == sht45_raw_to_milli_celsius(raw_t),
          "channel 0 matches the real sht45_raw_to_milli_celsius() conversion of the raw word this test queued");
    CHECK(out.channels[1].type == SENSOR_CHANNEL_HUMIDITY && out.channels[1].value == sht45_raw_to_milli_rh(raw_rh),
          "channel 1 matches the real sht45_raw_to_milli_rh() conversion of the raw word this test queued");

    err = g_sht45_provider_ops.read_measurement(ctx, &out, 1000);
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
