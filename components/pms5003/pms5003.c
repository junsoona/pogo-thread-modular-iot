/*
 * See pms5003.h's top comment: this is a registrable interface, not a
 * working driver. init() always fails, honestly and loudly, rather than
 * pretending to read a UART that does not exist on this hardware
 * generation. Selecting SENSOR_DRIVER_ID_PMS5003 in a real config.json
 * therefore always produces a clear boot-time CONFIG_ERROR-style log
 * instead of a confusing silent failure or fabricated data.
 */
#include "pms5003/pms5003.h"

#include "esp_log.h"

static const char *TAG = "pms5003";

static esp_err_t pms5003_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    (void)cfg;
    (void)out_ctx;
    ESP_LOGE(TAG,
             "PMS5003 hardware transport blocked by two-UART allocation -- ESP32-H2 has exactly two UART "
             "controllers and both are committed to the UPSTREAM/DOWNSTREAM POGO ports (see "
             "docs/pogo-interface.md). The frame parser (pms5003_parser.h/.c) and this sensor_provider "
             "interface are complete and host-tested; only a real transport (recommended: an external "
             "I2C-to-UART bridge) is missing. Refusing to start rather than fabricate a reading.");
    return ESP_ERR_NOT_SUPPORTED;
}

static uint16_t pms5003_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return (uint16_t)((1u << SENSOR_CHANNEL_PM1_0) | (1u << SENSOR_CHANNEL_PM2_5) | (1u << SENSOR_CHANNEL_PM10));
}

static uint32_t pms5003_get_min_polling_period_ms(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return 0;
}

static esp_err_t pms5003_start_measurement(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED; /* unreachable in practice: init() above never succeeds */
}

static esp_err_t pms5003_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    (void)ctx;
    (void)out;
    (void)timeout_ticks;
    return ESP_ERR_NOT_SUPPORTED;
}

static sensor_status_t pms5003_get_status(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return SENSOR_STATUS_NOT_INITIALIZED;
}

static void pms5003_deinit(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
}

const sensor_provider_ops_t g_pms5003_provider_ops = {
    .name = "pms5003",
    .init = pms5003_init,
    .get_sensor_bitmap = pms5003_get_sensor_bitmap,
    .get_min_polling_period_ms = pms5003_get_min_polling_period_ms,
    .start_measurement = pms5003_start_measurement,
    .read_measurement = pms5003_read_measurement,
    .get_status = pms5003_get_status,
    .deinit = pms5003_deinit,
};
