/*
 * Sensirion SGP40 driver -- real ESP-IDF v6.1 i2c_master API (section 10 of
 * the follow-up spec). VOC Index computed via the vendored, unmodified
 * Sensirion Gas Index Algorithm (components/sensirion_gas_index_algorithm --
 * see that directory's THIRD_PARTY_NOTICE.md).
 */
#include "sgp40/sgp40.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensirion_crc8/sensirion_crc8.h"
#include "sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.h"

static const char *TAG = "sgp40";

#define SGP40_CMD_MEASURE_RAW   0x260F
#define SGP40_CMD_SELF_TEST     0x280E
#define SGP40_CMD_HEATER_OFF    0x3615

#define SGP40_MEASURE_MS_MAX    30u
#define SGP40_SELF_TEST_MS      320u /* >=10ms, safe under pdMS_TO_TICKS() at CONFIG_FREERTOS_HZ=100 */
#define SGP40_I2C_TIMEOUT_MS    200

/* code review follow-up (SHT45 wake-reboot fix, item 7): the
 * read_measurement() polling loop below re-checks elapsed time via
 * esp_timer_get_time() every iteration -- correctness never depends on this
 * delay's actual length. But module/sdkconfig runs CONFIG_FREERTOS_HZ=100
 * (10ms/tick), so pdMS_TO_TICKS(2) rounds DOWN to 0 ticks and vTaskDelay(0)
 * would return immediately, turning the wait into a tight CPU-spinning poll
 * instead of actually blocking. Round up to at least 1 real tick so the loop
 * still blocks; the elapsed-time check (not this tick count) remains the
 * sole thing that decides when data is read. */
#define SGP40_POLL_DELAY_TICKS ((pdMS_TO_TICKS(2) > 0) ? pdMS_TO_TICKS(2) : (TickType_t)1)

/* Fixed default RH/T compensation ticks for "no external humidity/temperature
 * source" (Sensirion's own documented defaults: 50%RH, 25degC -- see
 * https://github.com/Sensirion/embedded-i2c-sgp40's example usage). This
 * module does NOT read another module's SHT45 over the wire to compensate
 * (section 10: "다른 모듈의 SHT45 값을 받은 것처럼 꾸미지 않음") -- a board
 * that pairs SGP40 with its OWN on-board SHT45 in the future could feed a
 * real reading in here instead; until then this is the honest, disclosed
 * default, logged loudly at init. */
#define SGP40_DEFAULT_RH_TICKS  0x8000u
#define SGP40_DEFAULT_T_TICKS   0x6666u

/* How long after init this driver marks channel status as CONDITIONING for
 * reporting purposes (section 10: "conditioning/warm-up 상태를 정상 측정과
 * 구분") -- SGP40 has no separate I2C conditioning command (unlike SGP41),
 * so this is purely a reporting-level distinction layered on the same
 * measure command, tracking the hotplate's documented warm-up time. */
#define SGP40_CONDITIONING_WINDOW_MS 10000u

struct sensor_provider_ctx_s {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint32_t measurement_timeout_ms;
    int64_t  start_time_us;
    int64_t  init_time_us;
    bool     measuring;
    sensor_status_t status;
    GasIndexAlgorithmParams voc_algo;
};

static struct sensor_provider_ctx_s s_ctx; /* one sensor per module, ever (section 2) */

static esp_err_t sgp40_write_cmd16(struct sensor_provider_ctx_s *ctx, uint16_t cmd)
{
    uint8_t buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    return i2c_master_transmit(ctx->dev, buf, sizeof(buf), SGP40_I2C_TIMEOUT_MS);
}

static esp_err_t sgp40_write_measure_cmd(struct sensor_provider_ctx_s *ctx, uint16_t rh_ticks, uint16_t t_ticks)
{
    uint8_t buf[8];
    buf[0] = (uint8_t)(SGP40_CMD_MEASURE_RAW >> 8);
    buf[1] = (uint8_t)(SGP40_CMD_MEASURE_RAW & 0xFF);
    buf[2] = (uint8_t)(rh_ticks >> 8);
    buf[3] = (uint8_t)(rh_ticks & 0xFF);
    buf[4] = sensirion_crc8(buf[2], buf[3]);
    buf[5] = (uint8_t)(t_ticks >> 8);
    buf[6] = (uint8_t)(t_ticks & 0xFF);
    buf[7] = sensirion_crc8(buf[5], buf[6]);
    return i2c_master_transmit(ctx->dev, buf, sizeof(buf), SGP40_I2C_TIMEOUT_MS);
}

static esp_err_t sgp40_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    struct sensor_provider_ctx_s *ctx = &s_ctx;
    ctx->measurement_timeout_ms = (cfg->measurement_timeout_ms != 0) ? cfg->measurement_timeout_ms : 200u;
    ctx->measuring = false;
    ctx->status = SENSOR_STATUS_NOT_INITIALIZED;
    ctx->bus = NULL;
    ctx->dev = NULL;

    if (cfg->bus_type != MODULE_BUS_I2C || cfg->i2c_sda_gpio < 0 || cfg->i2c_scl_gpio < 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SGP40 requires an I2C bus with sda/scl configured");
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
        .device_address = cfg->i2c_address, /* fixed 0x59 per the SGP40 datasheet */
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(ctx->bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(addr=0x%02x) failed: %s", cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    /* Self-test (section 10 self-test requirement): 0xD400 = pass, 0x4B00 =
     * one or more sub-tests failed. Anything else (CRC failure, bus error)
     * is also a failure -- never proceed on an ambiguous result. */
    err = sgp40_write_cmd16(ctx, SGP40_CMD_SELF_TEST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SGP40 self-test command write failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SGP40_SELF_TEST_MS));
    uint8_t st[3];
    err = i2c_master_receive(ctx->dev, st, sizeof(st), SGP40_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SGP40 self-test result read failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    if (sensirion_crc8(st[0], st[1]) != st[2]) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SGP40 self-test result CRC mismatch");
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t test_result = (uint16_t)(((uint16_t)st[0] << 8) | st[1]);
    if (test_result != 0xD400) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SGP40 self-test FAILED (result=0x%04x, expected 0xD400)", test_result);
        ctx->status = SENSOR_STATUS_DRIVER_ERROR;
        return ESP_FAIL;
    }

    GasIndexAlgorithm_init(&ctx->voc_algo, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    ctx->init_time_us = esp_timer_get_time();
    ctx->status = SENSOR_STATUS_OK;

    ESP_LOGW(TAG, "SGP40 using FIXED default RH/T compensation (50%%RH / 25degC, rh_ticks=0x%04x t_ticks=0x%04x) -- "
                  "no external humidity/temperature source is wired to this driver (section 10 of the follow-up "
                  "spec: never borrow another module's SHT45 reading over the wire)",
             SGP40_DEFAULT_RH_TICKS, SGP40_DEFAULT_T_TICKS);
    ESP_LOGI(TAG, "SGP40 initialized: addr=0x%02x bus=i2c#%u sda=%d scl=%d timeout=%ums", cfg->i2c_address,
             cfg->bus_number, (int)cfg->i2c_sda_gpio, (int)cfg->i2c_scl_gpio, (unsigned)ctx->measurement_timeout_ms);
    *out_ctx = ctx;
    return ESP_OK;
}

static uint16_t sgp40_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return (uint16_t)((1u << SENSOR_CHANNEL_VOC_RAW) | (1u << SENSOR_CHANNEL_VOC_INDEX));
}

static uint32_t sgp40_get_min_polling_period_ms(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    /* The Gas Index Algorithm is defined for a 1 Hz sampling cadence
     * (section 10: "알고리즘 요구에 맞춰 최소 polling period를 1000ms로
     * 제한") -- sensor_service_compute_effective_period_ms() combines this
     * with the dynamic response-time-derived period. */
    return 1000u;
}

static esp_err_t sgp40_start_measurement(sensor_provider_ctx_t *ctx)
{
    esp_err_t err = sgp40_write_measure_cmd(ctx, SGP40_DEFAULT_RH_TICKS, SGP40_DEFAULT_T_TICKS);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "SGP40 measurement command write failed: %s", esp_err_to_name(err));
        return err;
    }
    ctx->start_time_us = esp_timer_get_time();
    ctx->measuring = true;
    ctx->status = SENSOR_STATUS_OK;
    return ESP_OK;
}

static esp_err_t sgp40_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (!ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t wait_start = xTaskGetTickCount();
    while (1) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
        if (elapsed_ms >= SGP40_MEASURE_MS_MAX) {
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
        vTaskDelay(SGP40_POLL_DELAY_TICKS);
    }

    uint8_t raw[3];
    esp_err_t err = i2c_master_receive(ctx->dev, raw, sizeof(raw), SGP40_I2C_TIMEOUT_MS);
    ctx->measuring = false;
    uint32_t response_time_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "SGP40 result read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (sensirion_crc8(raw[0], raw[1]) != raw[2]) {
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        ESP_LOGW(TAG, "SGP40 CRC mismatch on read -- discarding reading");
        return ESP_ERR_INVALID_CRC;
    }

    int32_t sraw_voc = (int32_t)(((uint16_t)raw[0] << 8) | raw[1]);
    int32_t voc_index = 0;
    GasIndexAlgorithm_process(&ctx->voc_algo, sraw_voc, &voc_index);

    bool conditioning = (uint32_t)((esp_timer_get_time() - ctx->init_time_us) / 1000) < SGP40_CONDITIONING_WINDOW_MS;
    uint8_t channel_status = conditioning ? SENSOR_CHANNEL_STATUS_CONDITIONING : SENSOR_CHANNEL_STATUS_OK;

    ctx->status = SENSOR_STATUS_OK;
    out->sensor_type_bitmap = sgp40_get_sensor_bitmap(ctx);
    out->response_time_ms = response_time_ms;
    out->channel_count = 2;
    out->channels[0] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_VOC_RAW, .scale = SENSOR_SCALE_NONE, .status = channel_status, .value = sraw_voc};
    out->channels[1] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_VOC_INDEX, .scale = SENSOR_SCALE_UNIT, .status = channel_status, .value = voc_index};
    ESP_LOGD(TAG, "SGP40 read: sraw_voc=%d voc_index=%d conditioning=%d response_time=%ums", (int)sraw_voc,
             (int)voc_index, conditioning, (unsigned)response_time_ms);
    return ESP_OK;
}

static sensor_status_t sgp40_get_status(sensor_provider_ctx_t *ctx)
{
    return ctx->status;
}

static void sgp40_deinit(sensor_provider_ctx_t *ctx)
{
    /* Turn the hotplate off before tearing down the bus -- never called
     * across a role change (section 3), only provided for interface
     * completeness / a future orderly shutdown path. */
    sgp40_write_cmd16(ctx, SGP40_CMD_HEATER_OFF);
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

const sensor_provider_ops_t g_sgp40_provider_ops = {
    .name = "sgp40",
    .init = sgp40_init,
    .get_sensor_bitmap = sgp40_get_sensor_bitmap,
    .get_min_polling_period_ms = sgp40_get_min_polling_period_ms,
    .start_measurement = sgp40_start_measurement,
    .read_measurement = sgp40_read_measurement,
    .get_status = sgp40_get_status,
    .deinit = sgp40_deinit,
};
