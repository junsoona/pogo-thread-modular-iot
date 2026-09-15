/*
 * Host-compiled UART-layer validation for components/pms5003/pms5003.c,
 * beyond tools/test_pms5003_parser.c's parser-only coverage: the wake/
 * ACTIVE-or-PASSIVE command sequence, PASSIVE mode's per-reading READ
 * command, checksum-error/partial-frame/noise resync, read/write UART
 * error handling, timeout handling, and init-failure UART cleanup.
 *
 * Links the REAL, UNMODIFIED components/pms5003/pms5003.c +
 * pms5003_parser.c against the same fake UART bank built for
 * tools/test_pogo_port_hotswap_recovery.c and
 * tools/test_conditioning_provisioning.c (tools/host_test_stubs/driver/uart.h),
 * plus tools/host_test_stubs/host_pms5003_fake_sensor.c as a thin, real-
 * protocol stand-in for the physical PMS5003 hardware (it answers with
 * real 32-byte frames / accepts real 7-byte commands, but decides WHEN to
 * answer and with what bytes -- never reimplementing pms5003.c's own
 * decode/state logic, which this test exercises for real throughout).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs \
 *      -I components/pms5003/include -I components/sensor_provider/include \
 *      -I components/sensor_channel/include -I components/module_config/include \
 *      -o /tmp/test_pms5003_driver tools/test_pms5003_driver.c \
 *      components/pms5003/pms5003.c components/pms5003/pms5003_parser.c \
 *      tools/host_test_stubs/host_uart_sim.c tools/host_test_stubs/host_gpio_sim.c \
 *      tools/host_test_stubs/host_pms5003_fake_sensor.c \
 *   && /tmp/test_pms5003_driver
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "pms5003/pms5003.h"
#include "pms5003/pms5003_parser.h"
#include "driver/uart.h"
#include "module_config/module_config.h"
#include "host_pms5003_fake_sensor.h"

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

#define PMS_UART 1

static module_config_t base_valid_config(void)
{
    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.driver_id = SENSOR_DRIVER_ID_PMS5003;
    cfg.bus_type = MODULE_BUS_UART;
    cfg.bus_number = PMS_UART;
    cfg.uart_tx_gpio = 0;
    cfg.uart_rx_gpio = 1;
    cfg.uart_baud_rate = 9600; /* PMS5003_REQUIRED_BAUD_RATE */
    cfg.uart_mode = MODULE_UART_MODE_PASSIVE;
    cfg.uart_warmup_ms = 0; /* not under test here -- see requirement 2's conditioning test */
    cfg.measurement_timeout_ms = 300;
    cfg.downstream_port.uart_num = -1; /* terminal-only */
    return cfg;
}

/* --- Scenario: config validation (UART1/GPIO0-1/9600 8N1, terminal-only) --- */
static void scenario_config_validation(void)
{
    printf("\n-- scenario: pms5003_init()'s own config validation --\n");
    host_uart_test_reset();

    module_config_t bad;
    sensor_provider_ctx_t *ctx = NULL;

    bad = base_valid_config();
    bad.bus_type = MODULE_BUS_I2C;
    CHECK(g_pms5003_provider_ops.init(&bad, &ctx) != ESP_OK, "rejects a non-UART bus_type");

    bad = base_valid_config();
    bad.uart_baud_rate = 115200;
    CHECK(g_pms5003_provider_ops.init(&bad, &ctx) != ESP_OK, "rejects a baud rate other than 9600");

    bad = base_valid_config();
    bad.uart_tx_gpio = -1;
    CHECK(g_pms5003_provider_ops.init(&bad, &ctx) != ESP_OK, "rejects a missing uart_tx_gpio");

    bad = base_valid_config();
    bad.uart_rx_gpio = -1;
    CHECK(g_pms5003_provider_ops.init(&bad, &ctx) != ESP_OK, "rejects a missing uart_rx_gpio");

    bad = base_valid_config();
    bad.downstream_port.uart_num = 0;
    CHECK(g_pms5003_provider_ops.init(&bad, &ctx) != ESP_OK,
          "rejects a config with the downstream POGO port still enabled (terminal-only invariant)");

    module_config_t good = base_valid_config();
    esp_err_t err = g_pms5003_provider_ops.init(&good, &ctx);
    CHECK(err == ESP_OK && ctx != NULL, "accepts UART1/GPIO0 TX/GPIO1 RX/9600-8N1/terminal-only");
    if (err == ESP_OK) {
        g_pms5003_provider_ops.deinit(ctx);
    }
}

/* --- Scenario: wake -> ACTIVE/PASSIVE mode-select command sequence --- */
static void scenario_wake_mode_sequence(void)
{
    printf("\n-- scenario: wake (SLEEP) -> mode-select command sequence, both PASSIVE and ACTIVE --\n");

    for (int pass = 0; pass < 2; pass++) {
        bool active = (pass == 1);
        host_uart_test_reset();
        module_config_t cfg = base_valid_config();
        cfg.uart_mode = active ? MODULE_UART_MODE_ACTIVE : MODULE_UART_MODE_PASSIVE;

        sensor_provider_ctx_t *ctx = NULL;
        esp_err_t err = g_pms5003_provider_ops.init(&cfg, &ctx);
        CHECK(err == ESP_OK, active ? "init succeeds (ACTIVE)" : "init succeeds (PASSIVE)");

        uint8_t cmd1[PMS5003_COMMAND_LEN], cmd2[PMS5003_COMMAND_LEN];
        bool got1 = true, got2 = true;
        for (size_t i = 0; i < PMS5003_COMMAND_LEN; i++) {
            got1 = got1 && host_uart_test_take_tx_byte(PMS_UART, &cmd1[i], 200);
        }
        for (size_t i = 0; i < PMS5003_COMMAND_LEN; i++) {
            got2 = got2 && host_uart_test_take_tx_byte(PMS_UART, &cmd2[i], 200);
        }
        CHECK(got1 && got2, "two 7-byte commands (wake + mode-select) were sent during init");

        uint8_t expect_sleep[PMS5003_COMMAND_LEN];
        pms5003_build_command(PMS5003_COMMAND_SLEEP, 1, expect_sleep);
        CHECK(memcmp(cmd1, expect_sleep, PMS5003_COMMAND_LEN) == 0,
              "the FIRST command is SLEEP/wake (data_low=1), matching pms5003_build_command()'s real encoding");

        uint8_t expect_mode[PMS5003_COMMAND_LEN];
        pms5003_build_command(PMS5003_COMMAND_SET_MODE, active ? 1 : 0, expect_mode);
        CHECK(memcmp(cmd2, expect_mode, PMS5003_COMMAND_LEN) == 0,
              active ? "the SECOND command selects ACTIVE mode (data_low=1)"
                     : "the SECOND command selects PASSIVE mode (data_low=0)");

        if (err == ESP_OK) {
            g_pms5003_provider_ops.deinit(ctx);
        }
    }
}

/* --- Scenario: PASSIVE mode sends a READ command before each reading --- */
static void scenario_passive_read_command(void)
{
    printf("\n-- scenario: PASSIVE mode sends a READ command (0xE2) before every reading --\n");
    host_uart_test_reset();
    module_config_t cfg = base_valid_config();
    cfg.uart_mode = MODULE_UART_MODE_PASSIVE;

    sensor_provider_ctx_t *ctx = NULL;
    CHECK(g_pms5003_provider_ops.init(&cfg, &ctx) == ESP_OK, "init succeeds");
    /* Drain the wake+mode commands sent during init -- not under test here. */
    uint8_t discard;
    for (size_t i = 0; i < 2 * PMS5003_COMMAND_LEN; i++) {
        host_uart_test_take_tx_byte(PMS_UART, &discard, 200);
    }

    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK, "start_measurement succeeds");
    uint8_t cmd[PMS5003_COMMAND_LEN];
    bool got = true;
    for (size_t i = 0; i < PMS5003_COMMAND_LEN; i++) {
        got = got && host_uart_test_take_tx_byte(PMS_UART, &cmd[i], 200);
    }
    uint8_t expect_read[PMS5003_COMMAND_LEN];
    pms5003_build_command(PMS5003_COMMAND_READ, 0, expect_read);
    CHECK(got && memcmp(cmd, expect_read, PMS5003_COMMAND_LEN) == 0,
          "start_measurement() in PASSIVE mode sends exactly the real READ command");

    /* Answer it so the pending measurement doesn't leak into the next scenario. */
    uint8_t frame[32];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x42; frame[1] = 0x4D; frame[3] = 28;
    uint32_t sum = 0;
    for (int i = 0; i < 30; i++) sum += frame[i];
    frame[30] = (uint8_t)(sum >> 8); frame[31] = (uint8_t)sum;
    host_uart_test_push_rx(PMS_UART, frame, sizeof(frame));
    sensor_measurement_t meas;
    g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);

    g_pms5003_provider_ops.deinit(ctx);
}

/* Mirrors tools/test_pms5003_parser.c's own build_frame() helper -- a valid
 * 32-byte frame with a correct checksum, fixture data (not a reimplementation
 * of the real driver/parser logic under test). */
static void build_good_frame(uint8_t out[32], uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10)
{
    memset(out, 0, 32);
    out[0] = 0x42; out[1] = 0x4D; out[3] = 28;
    out[10] = (uint8_t)(pm1_0 >> 8); out[11] = (uint8_t)pm1_0;
    out[12] = (uint8_t)(pm2_5 >> 8); out[13] = (uint8_t)pm2_5;
    out[14] = (uint8_t)(pm10 >> 8); out[15] = (uint8_t)pm10;
    uint32_t sum = 0;
    for (int i = 0; i < 30; i++) sum += out[i];
    out[30] = (uint8_t)(sum >> 8); out[31] = (uint8_t)sum;
}

/* --- Scenario: checksum error / partial frame / noise, then resync --- */
static void scenario_checksum_partial_noise_resync(void)
{
    printf("\n-- scenario: checksum error, a partial frame, and noise are all recovered from (resync) --\n");
    host_uart_test_reset();
    module_config_t cfg = base_valid_config();
    cfg.uart_mode = MODULE_UART_MODE_PASSIVE;
    cfg.measurement_timeout_ms = 300;

    sensor_provider_ctx_t *ctx = NULL;
    CHECK(g_pms5003_provider_ops.init(&cfg, &ctx) == ESP_OK, "init succeeds");

    host_pms5003_fake_sensor_start(PMS_UART, false, 55, 66, 77);
    uint8_t good[32];
    build_good_frame(good, 55, 66, 77);

    /* Real electrical noise/corruption puts garbage ahead of (or instead of)
     * one transmission, but the following transmission is still intact --
     * each injection below is exactly ONE response to ONE READ command
     * (real PMS5003 PASSIVE-mode protocol is strictly request/response), so
     * the garbage and the real frame are both part of what the driver reads
     * back from that SAME READ. */

    /* 1) A bad-checksum full frame immediately followed by a good one. */
    uint8_t bad_checksum[32];
    build_good_frame(bad_checksum, 1, 2, 3);
    bad_checksum[30] ^= 0xFF; bad_checksum[31] ^= 0xFF; /* corrupt the checksum on purpose */
    uint8_t combo1[64];
    memcpy(combo1, bad_checksum, 32);
    memcpy(combo1 + 32, good, 32);
    host_pms5003_fake_sensor_inject_raw(combo1, sizeof(combo1));

    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK, "start_measurement (checksum-error case)");
    sensor_measurement_t meas;
    esp_err_t err = g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);
    CHECK(err == ESP_OK, "read_measurement still succeeds -- the real parser drops the bad-checksum frame and "
                          "resyncs onto the good one right behind it");
    CHECK(err == ESP_OK && meas.channels[0].value == 55, "the parsed values are from the GOOD frame, not the "
                                                           "corrupted one");

    /* 2) A truncated/partial frame: a real transmission cut off mid-frame
     * leaves the parser genuinely waiting for bytes that never arrive --
     * unlike the checksum/noise cases above, this is NOT resynchronized
     * within the SAME read (there is nothing after it in this one PASSIVE-
     * mode response to resync onto), so THIS measurement times out. Recovery
     * happens the normal way: the NEXT start_measurement() call resets the
     * parser (see pms5003.c) for a clean read. */
    uint8_t partial[10] = {0x42, 0x4D, 0x00, 28, 0, 0, 0, 0, 0, 0};
    host_pms5003_fake_sensor_inject_raw(partial, sizeof(partial));
    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK, "start_measurement (partial-frame case)");
    err = g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);
    CHECK(err == ESP_ERR_TIMEOUT, "a truncated frame with nothing after it times out this one measurement");

    host_pms5003_fake_sensor_set_values(55, 66, 77);
    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK,
          "start_measurement (fresh cycle right after the timeout)");
    err = g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);
    CHECK(err == ESP_OK && meas.channels[0].value == 55,
          "the VERY NEXT measurement cycle recovers cleanly -- start_measurement() resets the parser");

    /* 3) Pure noise (never even a valid 0x42 0x4D sync) ahead of a good one. */
    uint8_t noise[16] = {0x00, 0xFF, 0x11, 0x22, 0x33, 0x99, 0x88, 0x77, 0x42, 0x00, 0x4D, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t noise_then_good[16 + 32];
    memcpy(noise_then_good, noise, sizeof(noise));
    memcpy(noise_then_good + sizeof(noise), good, 32);
    host_pms5003_fake_sensor_inject_raw(noise_then_good, sizeof(noise_then_good));
    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK, "start_measurement (noise case)");
    err = g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);
    CHECK(err == ESP_OK, "read_measurement still succeeds despite leading noise");
    CHECK(meas.channels[0].value == 55 && meas.channels[1].value == 66 && meas.channels[2].value == 77,
          "the eventually-parsed frame's real PM values are exactly what the fake sensor was set to report");

    host_pms5003_fake_sensor_stop();
    g_pms5003_provider_ops.deinit(ctx);
}

/* --- Scenario: timeout and UART read-error handling --- */
static void scenario_timeout_and_uart_errors(void)
{
    printf("\n-- scenario: measurement timeout and a UART read error are both reported correctly --\n");
    host_uart_test_reset();
    module_config_t cfg = base_valid_config();
    cfg.uart_mode = MODULE_UART_MODE_PASSIVE;
    cfg.measurement_timeout_ms = 150;

    sensor_provider_ctx_t *ctx = NULL;
    CHECK(g_pms5003_provider_ops.init(&cfg, &ctx) == ESP_OK, "init succeeds");

    /* No responder at all -- times out. */
    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK, "start_measurement succeeds");
    sensor_measurement_t meas;
    esp_err_t err = g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);
    CHECK(err == ESP_ERR_TIMEOUT, "no response at all within measurement_timeout_ms -> ESP_ERR_TIMEOUT");
    CHECK(g_pms5003_provider_ops.get_status(ctx) == SENSOR_STATUS_TIMEOUT, "status reflects SENSOR_STATUS_TIMEOUT");

    /* A genuine UART read error (e.g. a DMA/hardware fault). */
    CHECK(g_pms5003_provider_ops.start_measurement(ctx) == ESP_OK, "start_measurement succeeds again");
    host_uart_test_force_next_read_fail(PMS_UART);
    err = g_pms5003_provider_ops.read_measurement(ctx, &meas, 500);
    CHECK(err == ESP_FAIL, "a UART read error is reported as ESP_FAIL");
    CHECK(g_pms5003_provider_ops.get_status(ctx) == SENSOR_STATUS_BUS_ERROR, "status reflects SENSOR_STATUS_BUS_ERROR");

    g_pms5003_provider_ops.deinit(ctx);
}

/* --- Scenario: a UART write failure mid-init cleans up UART resources --- */
static void scenario_init_failure_cleanup(void)
{
    printf("\n-- scenario: a write failure during init cleans up the UART driver (no resource leak) --\n");
    host_uart_test_reset();
    module_config_t cfg = base_valid_config();

    host_uart_test_force_next_write_fail(PMS_UART); /* the wake/SLEEP command write fails */
    sensor_provider_ctx_t *ctx = NULL;
    esp_err_t err = g_pms5003_provider_ops.init(&cfg, &ctx);
    CHECK(err != ESP_OK, "init fails cleanly when the wake command's UART write fails");

    /* If pms5003_init() had left the UART driver "installed" after this
     * failure (i.e. skipped uart_driver_delete() on this path), a fresh
     * init on the SAME bus_number would fail with ESP_ERR_INVALID_STATE
     * from our fake uart_driver_install()'s own already-installed guard --
     * see tools/host_test_stubs/host_uart_sim.c. */
    sensor_provider_ctx_t *ctx2 = NULL;
    esp_err_t err2 = g_pms5003_provider_ops.init(&cfg, &ctx2);
    CHECK(err2 == ESP_OK, "a subsequent normal init on the same UART succeeds -- the failed attempt's UART driver "
                          "was properly deleted, not leaked");
    if (err2 == ESP_OK) {
        g_pms5003_provider_ops.deinit(ctx2);
    }
}

int main(void)
{
    scenario_config_validation();
    scenario_wake_mode_sequence();
    scenario_passive_read_command();
    scenario_checksum_partial_noise_resync();
    scenario_timeout_and_uart_errors();
    scenario_init_failure_cleanup();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
