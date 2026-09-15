#include "pms5003/pms5003.h"
#include "pms5003/pms5003_parser.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pms5003";

#define PMS5003_REQUIRED_BAUD_RATE 9600u
#define PMS5003_RX_BUFFER_SIZE 512
#define PMS5003_COMMAND_TIMEOUT_MS 200u
#define PMS5003_MIN_POLLING_PERIOD_MS 1000u
#define PMS5003_READ_SLICE_MS 20u

struct sensor_provider_ctx_s {
    uart_port_t uart_num;
    module_uart_mode_t mode;
    uint32_t measurement_timeout_ms;
    uint32_t warmup_ms;
    int64_t initialized_at_us;
    int64_t measurement_started_at_us;
    bool measuring;
    bool uart_installed;
    sensor_status_t status;
    pms5003_parser_t parser;
};

static struct sensor_provider_ctx_s s_ctx;

static esp_err_t write_command(struct sensor_provider_ctx_s *ctx, pms5003_command_t command, uint8_t data_low)
{
    uint8_t bytes[PMS5003_COMMAND_LEN];
    pms5003_build_command(command, data_low, bytes);
    int written = uart_write_bytes(ctx->uart_num, bytes, sizeof(bytes));
    if (written != (int)sizeof(bytes)) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(ctx->uart_num, pdMS_TO_TICKS(PMS5003_COMMAND_TIMEOUT_MS));
}

static esp_err_t pms5003_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    if (cfg == NULL || out_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->bus_type != MODULE_BUS_UART || cfg->uart_tx_gpio < 0 || cfg->uart_rx_gpio < 0 ||
        cfg->uart_baud_rate != PMS5003_REQUIRED_BAUD_RATE || cfg->downstream_port.uart_num >= 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: PMS5003 requires terminal-only UART 9600 8N1 with downstream disabled");
        return ESP_ERR_INVALID_ARG;
    }

    struct sensor_provider_ctx_s *ctx = &s_ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->uart_num = (uart_port_t)cfg->bus_number;
    ctx->mode = cfg->uart_mode;
    ctx->measurement_timeout_ms = cfg->measurement_timeout_ms;
    ctx->warmup_ms = cfg->uart_warmup_ms;
    ctx->status = SENSOR_STATUS_NOT_INITIALIZED;
    pms5003_parser_init(&ctx->parser);

    uart_config_t uart_cfg = {
        .baud_rate = PMS5003_REQUIRED_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(ctx->uart_num, PMS5003_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install(UART%u) failed: %s", (unsigned)ctx->uart_num, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    ctx->uart_installed = true;
    err = uart_param_config(ctx->uart_num, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(ctx->uart_num, cfg->uart_tx_gpio, cfg->uart_rx_gpio,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%u configuration failed: %s", (unsigned)ctx->uart_num, esp_err_to_name(err));
        uart_driver_delete(ctx->uart_num);
        ctx->uart_installed = false;
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    uart_flush_input(ctx->uart_num);
    /* A sleeping sensor always wakes into active mode. Give its command
     * parser a short bounded recovery interval, then select the configured
     * active/passive policy explicitly. Accuracy warm-up is handled by
     * channel CONDITIONING status below; boot and POGO provisioning do not
     * block for 30 seconds. */
    err = write_command(ctx, PMS5003_COMMAND_SLEEP, 1);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(PMS5003_COMMAND_TIMEOUT_MS));
        err = write_command(ctx, PMS5003_COMMAND_SET_MODE,
                            ctx->mode == MODULE_UART_MODE_ACTIVE ? 1 : 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS5003 wake/mode command failed: %s", esp_err_to_name(err));
        uart_driver_delete(ctx->uart_num);
        ctx->uart_installed = false;
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    uart_flush_input(ctx->uart_num);
    ctx->initialized_at_us = esp_timer_get_time();
    ctx->status = ctx->warmup_ms > 0 ? SENSOR_STATUS_CONDITIONING : SENSOR_STATUS_OK;
    *out_ctx = ctx;
    ESP_LOGI(TAG, "initialized: UART%u tx=%d rx=%d 9600 8N1 mode=%s warmup=%ums",
             (unsigned)ctx->uart_num, (int)cfg->uart_tx_gpio, (int)cfg->uart_rx_gpio,
             ctx->mode == MODULE_UART_MODE_PASSIVE ? "passive" : "active", (unsigned)ctx->warmup_ms);
    return ESP_OK;
}

static uint16_t pms5003_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return (uint16_t)((1u << SENSOR_CHANNEL_PM1_0) | (1u << SENSOR_CHANNEL_PM2_5) | (1u << SENSOR_CHANNEL_PM10));
}

static uint32_t pms5003_get_min_polling_period_ms(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return PMS5003_MIN_POLLING_PERIOD_MS;
}

static esp_err_t pms5003_start_measurement(sensor_provider_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->uart_installed || ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }
    uart_flush_input(ctx->uart_num);
    pms5003_parser_init(&ctx->parser);
    if (ctx->mode == MODULE_UART_MODE_PASSIVE) {
        esp_err_t err = write_command(ctx, PMS5003_COMMAND_READ, 0);
        if (err != ESP_OK) {
            ctx->status = SENSOR_STATUS_BUS_ERROR;
            return err;
        }
    }
    ctx->measurement_started_at_us = esp_timer_get_time();
    ctx->measuring = true;
    return ESP_OK;
}

static esp_err_t pms5003_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (ctx == NULL || out == NULL || !ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t caller_start = xTaskGetTickCount();
    pms5003_frame_t frame;
    uint8_t bytes[64];

    while (true) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - ctx->measurement_started_at_us) / 1000);
        if (elapsed_ms >= ctx->measurement_timeout_ms ||
            (xTaskGetTickCount() - caller_start) >= timeout_ticks) {
            ctx->measuring = false;
            ctx->status = SENSOR_STATUS_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        int n = uart_read_bytes(ctx->uart_num, bytes, sizeof(bytes), pdMS_TO_TICKS(PMS5003_READ_SLICE_MS));
        if (n < 0) {
            ctx->measuring = false;
            ctx->status = SENSOR_STATUS_BUS_ERROR;
            return ESP_FAIL;
        }
        for (int i = 0; i < n; i++) {
            if (!pms5003_parser_feed(&ctx->parser, bytes[i], &frame)) {
                continue;
            }
            ctx->measuring = false;
            uint32_t response_ms =
                (uint32_t)((esp_timer_get_time() - ctx->measurement_started_at_us) / 1000);
            uint32_t age_ms = (uint32_t)((esp_timer_get_time() - ctx->initialized_at_us) / 1000);
            bool conditioning = age_ms < ctx->warmup_ms;
            uint8_t channel_status = conditioning ? SENSOR_CHANNEL_STATUS_CONDITIONING : SENSOR_CHANNEL_STATUS_OK;
            ctx->status = conditioning ? SENSOR_STATUS_CONDITIONING : SENSOR_STATUS_OK;
            memset(out, 0, sizeof(*out));
            out->sensor_type_bitmap = pms5003_get_sensor_bitmap(ctx);
            out->response_time_ms = response_ms > 0 ? response_ms : 1;
            out->channel_count = 3;
            out->channels[0] = (sensor_channel_reading_t){
                .type = SENSOR_CHANNEL_PM1_0, .scale = SENSOR_SCALE_UNIT,
                .status = channel_status, .value = frame.pm1_0_atm};
            out->channels[1] = (sensor_channel_reading_t){
                .type = SENSOR_CHANNEL_PM2_5, .scale = SENSOR_SCALE_UNIT,
                .status = channel_status, .value = frame.pm2_5_atm};
            out->channels[2] = (sensor_channel_reading_t){
                .type = SENSOR_CHANNEL_PM10, .scale = SENSOR_SCALE_UNIT,
                .status = channel_status, .value = frame.pm10_atm};
            ESP_LOGD(TAG, "read: PM1.0=%u PM2.5=%u PM10=%u ug/m3 status=%s response=%ums",
                     frame.pm1_0_atm, frame.pm2_5_atm, frame.pm10_atm,
                     conditioning ? "conditioning" : "ok", (unsigned)out->response_time_ms);
            return ESP_OK;
        }
    }
}

static sensor_status_t pms5003_get_status(sensor_provider_ctx_t *ctx)
{
    return ctx == NULL ? SENSOR_STATUS_NOT_INITIALIZED : ctx->status;
}

static void pms5003_deinit(sensor_provider_ctx_t *ctx)
{
    if (ctx != NULL && ctx->uart_installed) {
        (void)write_command(ctx, PMS5003_COMMAND_SLEEP, 0);
        uart_driver_delete(ctx->uart_num);
        ctx->uart_installed = false;
    }
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
