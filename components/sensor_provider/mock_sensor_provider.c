/*
 * Development-only mock sensor_provider (see docs/module-config.md).
 *
 * Fixed mock behavior (section 11 of the implementation spec):
 *   - sensor_type_bitmap is always 0x0001 (not read from module_config --
 *     the mock stands in for "some real sensor", not for a specific board).
 *   - measurement delay is always 120ms.
 *   - measurement_timeout_ms is the one thing taken from module_config.
 *
 * response_time_ms handed to the caller is measured with esp_timer, not
 * hard-coded, so parent_service's polling-period calculation is exercised
 * against a real (if simulated) timing measurement.
 */
#include "sensor_provider/sensor_provider.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mock_sensor";

#define MOCK_SENSOR_BITMAP  0x0001u
#define MOCK_DELAY_MS       120u
#define MOCK_DEFAULT_TIMEOUT_MS 1000u

struct sensor_provider_ctx_s {
    uint32_t measurement_timeout_ms;
    int64_t  start_time_us;
    bool     measuring;
    sensor_status_t status;
};

static struct sensor_provider_ctx_s s_mock_ctx; /* one sensor per module, ever (section 2) */

static esp_err_t mock_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    s_mock_ctx.measurement_timeout_ms = (cfg->measurement_timeout_ms != 0) ? cfg->measurement_timeout_ms
                                                                            : MOCK_DEFAULT_TIMEOUT_MS;
    s_mock_ctx.measuring = false;
    s_mock_ctx.status = SENSOR_STATUS_OK;

    ESP_LOGW(TAG, "mock sensor_provider ACTIVE (development only, not a real driver) -- "
                  "bitmap=0x%04x delay=%ums timeout=%ums",
             MOCK_SENSOR_BITMAP, MOCK_DELAY_MS, (unsigned)s_mock_ctx.measurement_timeout_ms);

    *out_ctx = &s_mock_ctx;
    return ESP_OK;
}

static uint16_t mock_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return MOCK_SENSOR_BITMAP;
}

static esp_err_t mock_start_measurement(sensor_provider_ctx_t *ctx)
{
    ctx->start_time_us = esp_timer_get_time();
    ctx->measuring = true;
    ctx->status = SENSOR_STATUS_OK;
    return ESP_OK;
}

static esp_err_t mock_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (!ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_start = xTaskGetTickCount();
    while (1) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
        if (elapsed_ms >= MOCK_DELAY_MS) {
            ctx->measuring = false;
            ctx->status = SENSOR_STATUS_OK;
            out->sensor_type_bitmap = MOCK_SENSOR_BITMAP;
            out->response_time_ms = elapsed_ms;
            return ESP_OK;
        }
        if (elapsed_ms >= ctx->measurement_timeout_ms) {
            ctx->measuring = false;
            ctx->status = SENSOR_STATUS_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        if ((xTaskGetTickCount() - wait_start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT; /* caller's wait budget expired; measurement may still be running */
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static sensor_status_t mock_get_status(sensor_provider_ctx_t *ctx)
{
    return ctx->status;
}

static void mock_deinit(sensor_provider_ctx_t *ctx)
{
    ctx->measuring = false;
}

const sensor_provider_ops_t g_mock_sensor_provider_ops = {
    .name = "mock",
    .init = mock_init,
    .get_sensor_bitmap = mock_get_sensor_bitmap,
    .start_measurement = mock_start_measurement,
    .read_measurement = mock_read_measurement,
    .get_status = mock_get_status,
    .deinit = mock_deinit,
};
