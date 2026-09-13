/*
 * ROHM BH1750FVI driver -- real ESP-IDF v6.1 i2c_master API (section 4 of
 * the follow-up spec). One-Time High-Resolution Mode (0x20): 1 lx
 * resolution, max 180ms conversion time per the datasheet. Power-down
 * between measurements is the sensor's own default behavior after a
 * one-time command completes, so no explicit power-down write is needed.
 */
#include "bh1750/bh1750.h"
#include "bh1750/bh1750_convert.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bh1750";

/* BH1750 command bytes (datasheet). */
#define BH1750_CMD_POWER_DOWN        0x00
#define BH1750_CMD_POWER_ON          0x01
#define BH1750_CMD_RESET             0x07
#define BH1750_CMD_ONE_TIME_HRES     0x20 /* 1 lx resolution, max 180ms, auto power-down after */

#define BH1750_CONVERSION_MS_MAX     180u /* datasheet max for (one-time) H-Res mode */
#define BH1750_I2C_TIMEOUT_MS        200

struct sensor_provider_ctx_s {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint32_t measurement_timeout_ms;
    int32_t  correction_permille;
    int64_t  start_time_us;
    bool     measuring;
    sensor_status_t status;
};

static struct sensor_provider_ctx_s s_ctx; /* one sensor per module, ever (section 2) */

static esp_err_t bh1750_write_cmd(struct sensor_provider_ctx_s *ctx, uint8_t cmd)
{
    return i2c_master_transmit(ctx->dev, &cmd, 1, BH1750_I2C_TIMEOUT_MS);
}

static esp_err_t bh1750_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    struct sensor_provider_ctx_s *ctx = &s_ctx;
    ctx->measurement_timeout_ms = (cfg->measurement_timeout_ms != 0) ? cfg->measurement_timeout_ms : 300u;
    ctx->correction_permille = cfg->calibration[0];
    ctx->measuring = false;
    ctx->status = SENSOR_STATUS_NOT_INITIALIZED;
    ctx->bus = NULL;
    ctx->dev = NULL;

    if (cfg->bus_type != MODULE_BUS_I2C || cfg->i2c_sda_gpio < 0 || cfg->i2c_scl_gpio < 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: BH1750 requires an I2C bus with sda/scl configured");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (i2c_port_num_t)cfg->bus_number,
        .sda_io_num = (gpio_num_t)cfg->i2c_sda_gpio,
        .scl_io_num = (gpio_num_t)cfg->i2c_scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &ctx->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c_address, /* the ONE address module_config specifies -- never probed/auto-detected */
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(ctx->bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(addr=0x%02x) failed: %s", cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    /* Power-on/reset self-check (section 4: "power-on/reset ... 오류 검사") --
     * a failure here means the sensor is not actually present/responding at
     * the configured address, which is CONFIG_ERROR territory, not "keep
     * going and hope a later measurement works". */
    err = bh1750_write_cmd(ctx, BH1750_CMD_POWER_ON);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 POWER_ON write failed (addr=0x%02x): %s -- sensor not responding",
                 cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    err = bh1750_write_cmd(ctx, BH1750_CMD_RESET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 RESET write failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    ctx->status = SENSOR_STATUS_OK;
    ESP_LOGI(TAG, "BH1750 initialized: addr=0x%02x bus=i2c#%u sda=%d scl=%d timeout=%ums", cfg->i2c_address,
             cfg->bus_number, (int)cfg->i2c_sda_gpio, (int)cfg->i2c_scl_gpio, (unsigned)ctx->measurement_timeout_ms);
    *out_ctx = ctx;
    return ESP_OK;
}

static uint16_t bh1750_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return (uint16_t)(1u << SENSOR_CHANNEL_ILLUMINANCE);
}

static uint32_t bh1750_get_min_polling_period_ms(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return 0; /* no algorithmic minimum -- limited only by the ~180ms conversion time itself */
}

static esp_err_t bh1750_start_measurement(sensor_provider_ctx_t *ctx)
{
    esp_err_t err = bh1750_write_cmd(ctx, BH1750_CMD_ONE_TIME_HRES);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "BH1750 measurement command write failed: %s", esp_err_to_name(err));
        return err;
    }
    ctx->start_time_us = esp_timer_get_time();
    ctx->measuring = true;
    ctx->status = SENSOR_STATUS_OK;
    return ESP_OK;
}

static esp_err_t bh1750_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (!ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_start = xTaskGetTickCount();
    /* BH1750 has no "data ready" flag to poll -- the host must wait at least
     * the datasheet's max conversion time before reading. Wait in small
     * increments so a caller-side timeout is still honored promptly. */
    while (1) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
        if (elapsed_ms >= BH1750_CONVERSION_MS_MAX) {
            break;
        }
        if (elapsed_ms >= ctx->measurement_timeout_ms) {
            ctx->measuring = false;
            ctx->status = SENSOR_STATUS_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        if ((xTaskGetTickCount() - wait_start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT; /* caller's own wait budget expired; measurement may still be running */
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t raw_bytes[2];
    esp_err_t err = i2c_master_receive(ctx->dev, raw_bytes, sizeof(raw_bytes), BH1750_I2C_TIMEOUT_MS);
    ctx->measuring = false;
    uint32_t response_time_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "BH1750 result read failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t raw = bh1750_bytes_to_raw(raw_bytes[0], raw_bytes[1]);
    uint32_t milli_lux = bh1750_raw_to_milli_lux(raw, ctx->correction_permille);

    ctx->status = SENSOR_STATUS_OK;
    out->sensor_type_bitmap = bh1750_get_sensor_bitmap(ctx);
    out->response_time_ms = response_time_ms;
    out->channel_count = 1;
    out->channels[0] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_ILLUMINANCE,
        .scale = SENSOR_SCALE_MILLI,
        .status = SENSOR_CHANNEL_STATUS_OK,
        .value = (int32_t)milli_lux,
    };
    ESP_LOGD(TAG, "BH1750 read: raw=%u milli_lux=%u response_time=%ums", raw, (unsigned)milli_lux,
             (unsigned)response_time_ms);
    return ESP_OK;
}

static sensor_status_t bh1750_get_status(sensor_provider_ctx_t *ctx)
{
    return ctx->status;
}

static void bh1750_deinit(sensor_provider_ctx_t *ctx)
{
    /* Never actually called across a role change (section 3: the driver is
     * initialized exactly once at boot and never re-torn-down just because
     * this module's ROOT/INTERMEDIATE/LEAF role changes) -- provided for
     * interface completeness / a future orderly shutdown path. */
    if (ctx->dev) {
        i2c_master_bus_rm_device(ctx->dev);
        ctx->dev = NULL;
    }
    if (ctx->bus) {
        i2c_del_master_bus(ctx->bus);
        ctx->bus = NULL;
    }
    ctx->measuring = false;
}

const sensor_provider_ops_t g_bh1750_provider_ops = {
    .name = "bh1750",
    .init = bh1750_init,
    .get_sensor_bitmap = bh1750_get_sensor_bitmap,
    .get_min_polling_period_ms = bh1750_get_min_polling_period_ms,
    .start_measurement = bh1750_start_measurement,
    .read_measurement = bh1750_read_measurement,
    .get_status = bh1750_get_status,
    .deinit = bh1750_deinit,
};
