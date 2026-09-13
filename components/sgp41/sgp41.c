/*
 * Sensirion SGP41 driver -- real ESP-IDF v6.1 i2c_master API (section 10 of
 * the follow-up spec). VOC and NOx Index each computed via their own
 * instance of the vendored, unmodified Sensirion Gas Index Algorithm
 * (components/sensirion_gas_index_algorithm -- see that directory's
 * THIRD_PARTY_NOTICE.md).
 */
#include "sgp41/sgp41.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensirion_crc8/sensirion_crc8.h"
#include "sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.h"

static const char *TAG = "sgp41";

#define SGP41_CMD_CONDITIONING  0x2612
#define SGP41_CMD_MEASURE_RAW   0x2619
#define SGP41_CMD_SELF_TEST     0x280E
#define SGP41_CMD_HEATER_OFF    0x3615

#define SGP41_CONDITIONING_MS_MAX 50u
#define SGP41_MEASURE_MS_MAX      50u
#define SGP41_SELF_TEST_MS        320u
#define SGP41_I2C_TIMEOUT_MS      200

/* Same documented default compensation values as SGP40 -- see sgp40.c's
 * comment; identical disclosure applies (section 10: never fake another
 * module's SHT45 reading). */
#define SGP41_DEFAULT_RH_TICKS  0x8000u
#define SGP41_DEFAULT_T_TICKS   0x6666u

/* Sensirion's own recommendation: run the dedicated conditioning command
 * (VOC raw only, no NOx) for the first ~10s before switching to the
 * combined measure-raw-signals command, to let the NOx baseline stabilize. */
#define SGP41_CONDITIONING_WINDOW_MS 10000u

struct sensor_provider_ctx_s {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint32_t measurement_timeout_ms;
    int64_t  start_time_us;
    int64_t  init_time_us;
    bool     measuring;
    bool     conditioning_now; /* which command was actually issued for the in-flight measurement */
    sensor_status_t status;
    GasIndexAlgorithmParams voc_algo;
    GasIndexAlgorithmParams nox_algo;
};

static struct sensor_provider_ctx_s s_ctx; /* one sensor per module, ever (section 2) */

static esp_err_t sgp41_write_cmd16(struct sensor_provider_ctx_s *ctx, uint16_t cmd)
{
    uint8_t buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    return i2c_master_transmit(ctx->dev, buf, sizeof(buf), SGP41_I2C_TIMEOUT_MS);
}

static esp_err_t sgp41_write_cmd16_with_rh_t(struct sensor_provider_ctx_s *ctx, uint16_t cmd, uint16_t rh_ticks,
                                              uint16_t t_ticks)
{
    uint8_t buf[8];
    buf[0] = (uint8_t)(cmd >> 8);
    buf[1] = (uint8_t)(cmd & 0xFF);
    buf[2] = (uint8_t)(rh_ticks >> 8);
    buf[3] = (uint8_t)(rh_ticks & 0xFF);
    buf[4] = sensirion_crc8(buf[2], buf[3]);
    buf[5] = (uint8_t)(t_ticks >> 8);
    buf[6] = (uint8_t)(t_ticks & 0xFF);
    buf[7] = sensirion_crc8(buf[5], buf[6]);
    return i2c_master_transmit(ctx->dev, buf, sizeof(buf), SGP41_I2C_TIMEOUT_MS);
}

static esp_err_t sgp41_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    struct sensor_provider_ctx_s *ctx = &s_ctx;
    ctx->measurement_timeout_ms = (cfg->measurement_timeout_ms != 0) ? cfg->measurement_timeout_ms : 200u;
    ctx->measuring = false;
    ctx->status = SENSOR_STATUS_NOT_INITIALIZED;
    ctx->bus = NULL;
    ctx->dev = NULL;

    if (cfg->bus_type != MODULE_BUS_I2C || cfg->i2c_sda_gpio < 0 || cfg->i2c_scl_gpio < 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SGP41 requires an I2C bus with sda/scl configured");
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
        .device_address = cfg->i2c_address, /* fixed 0x59 per the SGP41 datasheet */
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(ctx->bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(addr=0x%02x) failed: %s", cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    err = sgp41_write_cmd16(ctx, SGP41_CMD_SELF_TEST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SGP41 self-test command write failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SGP41_SELF_TEST_MS));
    uint8_t st[3];
    err = i2c_master_receive(ctx->dev, st, sizeof(st), SGP41_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SGP41 self-test result read failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    if (sensirion_crc8(st[0], st[1]) != st[2]) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SGP41 self-test result CRC mismatch");
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t test_result = (uint16_t)(((uint16_t)st[0] << 8) | st[1]);
    if (test_result != 0xD400) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SGP41 self-test FAILED (result=0x%04x, expected 0xD400)", test_result);
        ctx->status = SENSOR_STATUS_DRIVER_ERROR;
        return ESP_FAIL;
    }

    GasIndexAlgorithm_init(&ctx->voc_algo, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    GasIndexAlgorithm_init(&ctx->nox_algo, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
    ctx->init_time_us = esp_timer_get_time();
    ctx->status = SENSOR_STATUS_OK;

    ESP_LOGW(TAG, "SGP41 using FIXED default RH/T compensation (50%%RH / 25degC, rh_ticks=0x%04x t_ticks=0x%04x) -- "
                  "no external humidity/temperature source is wired to this driver (section 10 of the follow-up "
                  "spec: never borrow another module's SHT45 reading over the wire)",
             SGP41_DEFAULT_RH_TICKS, SGP41_DEFAULT_T_TICKS);
    ESP_LOGI(TAG, "SGP41 initialized: addr=0x%02x bus=i2c#%u sda=%d scl=%d timeout=%ums", cfg->i2c_address,
             cfg->bus_number, (int)cfg->i2c_sda_gpio, (int)cfg->i2c_scl_gpio, (unsigned)ctx->measurement_timeout_ms);
    *out_ctx = ctx;
    return ESP_OK;
}

static uint16_t sgp41_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return (uint16_t)((1u << SENSOR_CHANNEL_VOC_RAW) | (1u << SENSOR_CHANNEL_VOC_INDEX) |
                       (1u << SENSOR_CHANNEL_NOX_RAW) | (1u << SENSOR_CHANNEL_NOX_INDEX));
}

static uint32_t sgp41_get_min_polling_period_ms(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return 1000u; /* Gas Index Algorithm requires ~1 Hz sampling (section 10) */
}

static bool sgp41_in_conditioning_window(const struct sensor_provider_ctx_s *ctx)
{
    return (uint32_t)((esp_timer_get_time() - ctx->init_time_us) / 1000) < SGP41_CONDITIONING_WINDOW_MS;
}

static esp_err_t sgp41_start_measurement(sensor_provider_ctx_t *ctx)
{
    ctx->conditioning_now = sgp41_in_conditioning_window(ctx);
    uint16_t cmd = ctx->conditioning_now ? SGP41_CMD_CONDITIONING : SGP41_CMD_MEASURE_RAW;
    esp_err_t err = sgp41_write_cmd16_with_rh_t(ctx, cmd, SGP41_DEFAULT_RH_TICKS, SGP41_DEFAULT_T_TICKS);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "SGP41 %s command write failed: %s", ctx->conditioning_now ? "conditioning" : "measurement",
                 esp_err_to_name(err));
        return err;
    }
    ctx->start_time_us = esp_timer_get_time();
    ctx->measuring = true;
    ctx->status = SENSOR_STATUS_OK;
    return ESP_OK;
}

static esp_err_t sgp41_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (!ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t max_wait_ms = ctx->conditioning_now ? SGP41_CONDITIONING_MS_MAX : SGP41_MEASURE_MS_MAX;
    TickType_t wait_start = xTaskGetTickCount();
    while (1) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
        if (elapsed_ms >= max_wait_ms) {
            break;
        }
        if (elapsed_ms >= ctx->measurement_timeout_ms) {
            ctx->measuring = false;
            ctx->status = SENSOR_STATUS_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        if ((xTaskGetTickCount() - wait_start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    /* Conditioning returns only SRAW_VOC (1 word, 3 bytes); the combined
     * measure command returns SRAW_VOC + SRAW_NOX (2 words, 6 bytes). */
    uint8_t raw[6];
    size_t read_len = ctx->conditioning_now ? 3 : 6;
    esp_err_t err = i2c_master_receive(ctx->dev, raw, read_len, SGP41_I2C_TIMEOUT_MS);
    ctx->measuring = false;
    uint32_t response_time_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "SGP41 result read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (sensirion_crc8(raw[0], raw[1]) != raw[2]) {
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        ESP_LOGW(TAG, "SGP41 VOC CRC mismatch on read -- discarding reading");
        return ESP_ERR_INVALID_CRC;
    }
    if (!ctx->conditioning_now && sensirion_crc8(raw[3], raw[4]) != raw[5]) {
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        ESP_LOGW(TAG, "SGP41 NOx CRC mismatch on read -- discarding reading");
        return ESP_ERR_INVALID_CRC;
    }

    int32_t sraw_voc = (int32_t)(((uint16_t)raw[0] << 8) | raw[1]);
    int32_t voc_index = 0;
    GasIndexAlgorithm_process(&ctx->voc_algo, sraw_voc, &voc_index);

    uint8_t status = ctx->conditioning_now ? SENSOR_CHANNEL_STATUS_CONDITIONING : SENSOR_CHANNEL_STATUS_OK;

    out->sensor_type_bitmap = sgp41_get_sensor_bitmap(ctx);
    out->response_time_ms = response_time_ms;

    if (ctx->conditioning_now) {
        /* No NOx raw value at all during conditioning -- only report the
         * two VOC channels, both marked CONDITIONING. Section 10:
         * "conditioning/warm-up 상태를 정상 측정과 구분" -- here that also
         * means genuinely not fabricating a NOx reading that was never
         * taken. */
        out->channel_count = 2;
        out->channels[0] = (sensor_channel_reading_t){
            .type = SENSOR_CHANNEL_VOC_RAW, .scale = SENSOR_SCALE_NONE, .status = status, .value = sraw_voc};
        out->channels[1] = (sensor_channel_reading_t){
            .type = SENSOR_CHANNEL_VOC_INDEX, .scale = SENSOR_SCALE_UNIT, .status = status, .value = voc_index};
    } else {
        int32_t sraw_nox = (int32_t)(((uint16_t)raw[3] << 8) | raw[4]);
        int32_t nox_index = 0;
        GasIndexAlgorithm_process(&ctx->nox_algo, sraw_nox, &nox_index);

        out->channel_count = 4;
        out->channels[0] = (sensor_channel_reading_t){
            .type = SENSOR_CHANNEL_VOC_RAW, .scale = SENSOR_SCALE_NONE, .status = status, .value = sraw_voc};
        out->channels[1] = (sensor_channel_reading_t){
            .type = SENSOR_CHANNEL_VOC_INDEX, .scale = SENSOR_SCALE_UNIT, .status = status, .value = voc_index};
        out->channels[2] = (sensor_channel_reading_t){
            .type = SENSOR_CHANNEL_NOX_RAW, .scale = SENSOR_SCALE_NONE, .status = status, .value = sraw_nox};
        out->channels[3] = (sensor_channel_reading_t){
            .type = SENSOR_CHANNEL_NOX_INDEX, .scale = SENSOR_SCALE_UNIT, .status = status, .value = nox_index};
    }

    ctx->status = SENSOR_STATUS_OK;
    ESP_LOGD(TAG, "SGP41 read: conditioning=%d channels=%u response_time=%ums", ctx->conditioning_now,
             out->channel_count, (unsigned)response_time_ms);
    return ESP_OK;
}

static sensor_status_t sgp41_get_status(sensor_provider_ctx_t *ctx)
{
    return ctx->status;
}

static void sgp41_deinit(sensor_provider_ctx_t *ctx)
{
    sgp41_write_cmd16(ctx, SGP41_CMD_HEATER_OFF);
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

const sensor_provider_ops_t g_sgp41_provider_ops = {
    .name = "sgp41",
    .init = sgp41_init,
    .get_sensor_bitmap = sgp41_get_sensor_bitmap,
    .get_min_polling_period_ms = sgp41_get_min_polling_period_ms,
    .start_measurement = sgp41_start_measurement,
    .read_measurement = sgp41_read_measurement,
    .get_status = sgp41_get_status,
    .deinit = sgp41_deinit,
};
