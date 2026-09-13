/*
 * Sensirion SHT45 driver -- real ESP-IDF v6.1 i2c_master API (section 5 of
 * the follow-up spec). High-precision measurement command (0xFD): typical
 * 6.9ms, max 8.2ms conversion time per the datasheet. Every 16-bit word on
 * the wire (serial number halves, T, RH) carries its own Sensirion CRC-8
 * (see sht45_convert.h) -- a mismatch anywhere is never treated as valid
 * data.
 */
#include "sht45/sht45.h"
#include "sht45/sht45_convert.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sht45";

/* SHT4x command bytes (datasheet). */
#define SHT45_CMD_MEASURE_HIGH_PRECISION 0xFD
#define SHT45_CMD_SOFT_RESET             0x94
#define SHT45_CMD_READ_SERIAL            0x89

#define SHT45_MEASURE_MS_MAX  10u /* datasheet max is 8.2ms; round up for margin */
#define SHT45_RESET_MS        2u  /* datasheet: allow >=1ms after soft reset before the next command */
#define SHT45_SERIAL_MS       10u
#define SHT45_I2C_TIMEOUT_MS  200

/*
 * SHT45_RESET_MS and SHT45_SERIAL_MS are hardware-mandated minimum recovery
 * times, not scheduling hints -- module/sdkconfig runs CONFIG_FREERTOS_HZ=100
 * (10ms/tick), so vTaskDelay(pdMS_TO_TICKS(2)) rounds DOWN to 0 ticks and
 * returns almost immediately: the very next command (READ_SERIAL) then goes
 * out before the sensor has actually finished its soft reset, which the
 * sensor NACKs. esp_rom_delay_us() busy-waits a real microsecond count
 * regardless of CONFIG_FREERTOS_HZ, so these two waits are converted to it
 * instead of vTaskDelay()/pdMS_TO_TICKS(). Both are one-time, sub-10ms waits
 * during init only -- never in a hot loop -- so the busy-wait cost is
 * negligible and does not risk the task watchdog. */
#define SHT45_RESET_US  (SHT45_RESET_MS * 1000u)
#define SHT45_SERIAL_US (SHT45_SERIAL_MS * 1000u)

/* The read_measurement() polling loop below re-checks elapsed time via
 * esp_timer_get_time() every iteration -- correctness never depends on this
 * delay's actual length. But at CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(2) is
 * also 0, so vTaskDelay(0) would return immediately and turn the wait into a
 * tight CPU-spinning poll instead of actually blocking. Round up to at least
 * 1 real tick so the loop still blocks, while the elapsed-time check (not
 * this tick count) remains the sole thing that decides when data is read. */
#define SHT45_POLL_DELAY_TICKS ((pdMS_TO_TICKS(2) > 0) ? pdMS_TO_TICKS(2) : (TickType_t)1)

struct sensor_provider_ctx_s {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint32_t measurement_timeout_ms;
    int64_t  start_time_us;
    bool     measuring;
    sensor_status_t status;
    uint32_t serial_number; /* diagnostic only, logged at init */
};

static struct sensor_provider_ctx_s s_ctx; /* one sensor per module, ever (section 2) */

static esp_err_t sht45_write_cmd(struct sensor_provider_ctx_s *ctx, uint8_t cmd)
{
    return i2c_master_transmit(ctx->dev, &cmd, 1, SHT45_I2C_TIMEOUT_MS);
}

static esp_err_t sht45_init(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx)
{
    struct sensor_provider_ctx_s *ctx = &s_ctx;
    ctx->measurement_timeout_ms = (cfg->measurement_timeout_ms != 0) ? cfg->measurement_timeout_ms : 200u;
    ctx->measuring = false;
    ctx->status = SENSOR_STATUS_NOT_INITIALIZED;
    ctx->bus = NULL;
    ctx->dev = NULL;
    ctx->serial_number = 0;

    if (cfg->bus_type != MODULE_BUS_I2C || cfg->i2c_sda_gpio < 0 || cfg->i2c_scl_gpio < 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SHT45 requires an I2C bus with sda/scl configured");
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
        .device_address = cfg->i2c_address, /* the ONE address module_config specifies (normally 0x44) */
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(ctx->bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(addr=0x%02x) failed: %s", cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    /* Probe the ONE configured address (section 5's header comment: "Uses
     * the fixed I2C address from module_config ONLY -- never probes any
     * other address" -- no auto address scan, no auto driver switch). This
     * is a distinct, earlier failure mode from a command NACK below: a probe
     * failure means nothing ACKed this address at all (wrong address,
     * wiring, missing pull-ups, unpowered sensor), whereas a command write
     * failure below means something IS on the bus at this address but
     * rejected the specific command byte. Logged separately on purpose. */
    err = i2c_master_probe(ctx->bus, (uint16_t)cfg->i2c_address, SHT45_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHT45 i2c_master_probe(addr=0x%02x) failed: %s -- no device acknowledged this address "
                      "(check wiring/pull-ups/power; this is NOT a command-level NACK)",
                 cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }

    err = sht45_write_cmd(ctx, SHT45_CMD_SOFT_RESET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHT45 SOFT_RESET write failed (addr=0x%02x): %s -- device acked its address but NACKed "
                      "this command",
                 cfg->i2c_address, esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    esp_rom_delay_us(SHT45_RESET_US); /* hardware-mandated recovery time -- see SHT45_RESET_US's comment above */

    /* Communication self-check (section 5: "serial number 읽기 또는 명확한
     * 통신 self-check를 init 단계에 포함"): read + CRC-verify the serial
     * number. A CRC failure here means the sensor is not actually present/
     * responding correctly at the configured address -- CONFIG_ERROR
     * territory, not "keep going and hope a measurement works". */
    err = sht45_write_cmd(ctx, SHT45_CMD_READ_SERIAL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHT45 READ_SERIAL write failed: %s -- device acked its address but NACKed this command",
                 esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    esp_rom_delay_us(SHT45_SERIAL_US); /* hardware-mandated recovery time -- see SHT45_SERIAL_US's comment above */

    uint8_t serial_bytes[6];
    err = i2c_master_receive(ctx->dev, serial_bytes, sizeof(serial_bytes), SHT45_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHT45 READ_SERIAL result read failed: %s", esp_err_to_name(err));
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        return err;
    }
    if (sht45_crc8(serial_bytes[0], serial_bytes[1]) != serial_bytes[2] ||
        sht45_crc8(serial_bytes[3], serial_bytes[4]) != serial_bytes[5]) {
        ESP_LOGE(TAG, "CONFIG_ERROR: SHT45 serial number CRC mismatch -- sensor not communicating correctly");
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        return ESP_ERR_INVALID_CRC;
    }
    ctx->serial_number = ((uint32_t)serial_bytes[0] << 24) | ((uint32_t)serial_bytes[1] << 16) |
                          ((uint32_t)serial_bytes[3] << 8) | (uint32_t)serial_bytes[4];

    ctx->status = SENSOR_STATUS_OK;
    ESP_LOGI(TAG, "SHT45 initialized: addr=0x%02x bus=i2c#%u sda=%d scl=%d serial=0x%08x timeout=%ums",
             cfg->i2c_address, cfg->bus_number, (int)cfg->i2c_sda_gpio, (int)cfg->i2c_scl_gpio,
             (unsigned)ctx->serial_number, (unsigned)ctx->measurement_timeout_ms);
    *out_ctx = ctx;
    return ESP_OK;
}

static uint16_t sht45_get_sensor_bitmap(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return (uint16_t)((1u << SENSOR_CHANNEL_TEMPERATURE) | (1u << SENSOR_CHANNEL_HUMIDITY));
}

static uint32_t sht45_get_min_polling_period_ms(sensor_provider_ctx_t *ctx)
{
    (void)ctx;
    return 0; /* no algorithmic minimum */
}

static esp_err_t sht45_start_measurement(sensor_provider_ctx_t *ctx)
{
    esp_err_t err = sht45_write_cmd(ctx, SHT45_CMD_MEASURE_HIGH_PRECISION);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "SHT45 measurement command write failed: %s", esp_err_to_name(err));
        return err;
    }
    ctx->start_time_us = esp_timer_get_time();
    ctx->measuring = true;
    ctx->status = SENSOR_STATUS_OK;
    return ESP_OK;
}

static esp_err_t sht45_read_measurement(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (!ctx->measuring) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_start = xTaskGetTickCount();
    /* SHT4x has no "data ready" flag either -- wait at least the datasheet's
     * max high-precision conversion time before reading. */
    while (1) {
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
        if (elapsed_ms >= SHT45_MEASURE_MS_MAX) {
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
        vTaskDelay(SHT45_POLL_DELAY_TICKS);
    }

    uint8_t raw_bytes[6];
    esp_err_t err = i2c_master_receive(ctx->dev, raw_bytes, sizeof(raw_bytes), SHT45_I2C_TIMEOUT_MS);
    ctx->measuring = false;
    uint32_t response_time_ms = (uint32_t)((esp_timer_get_time() - ctx->start_time_us) / 1000);
    if (err != ESP_OK) {
        ctx->status = SENSOR_STATUS_BUS_ERROR;
        ESP_LOGW(TAG, "SHT45 result read failed: %s", esp_err_to_name(err));
        return err;
    }

    /* [T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC] -- each 16-bit word
     * carries its own CRC-8; a mismatch anywhere means this reading is
     * discarded entirely, never treated as partially-valid data (section 5:
     * "CRC 불일치 시 정상 데이터로 처리하지 않음"). */
    bool crc_t_ok = sht45_crc8(raw_bytes[0], raw_bytes[1]) == raw_bytes[2];
    bool crc_rh_ok = sht45_crc8(raw_bytes[3], raw_bytes[4]) == raw_bytes[5];
    if (!crc_t_ok || !crc_rh_ok) {
        ctx->status = SENSOR_STATUS_CRC_ERROR;
        ESP_LOGW(TAG, "SHT45 CRC mismatch on read (T_ok=%d RH_ok=%d) -- discarding reading", crc_t_ok, crc_rh_ok);
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_t = (uint16_t)(((uint16_t)raw_bytes[0] << 8) | raw_bytes[1]);
    uint16_t raw_rh = (uint16_t)(((uint16_t)raw_bytes[3] << 8) | raw_bytes[4]);
    int32_t milli_c = sht45_raw_to_milli_celsius(raw_t);
    int32_t milli_rh = sht45_raw_to_milli_rh(raw_rh);

    ctx->status = SENSOR_STATUS_OK;
    out->sensor_type_bitmap = sht45_get_sensor_bitmap(ctx);
    out->response_time_ms = response_time_ms;
    out->channel_count = 2;
    out->channels[0] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI, .status = SENSOR_CHANNEL_STATUS_OK,
        .value = milli_c};
    out->channels[1] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_HUMIDITY, .scale = SENSOR_SCALE_MILLI, .status = SENSOR_CHANNEL_STATUS_OK,
        .value = milli_rh};
    ESP_LOGD(TAG, "SHT45 read: milli_c=%d milli_rh=%d response_time=%ums", (int)milli_c, (int)milli_rh,
             (unsigned)response_time_ms);
    return ESP_OK;
}

static sensor_status_t sht45_get_status(sensor_provider_ctx_t *ctx)
{
    return ctx->status;
}

static void sht45_deinit(sensor_provider_ctx_t *ctx)
{
    /* Never actually called across a role change -- see bh1750.c's deinit
     * comment for why (section 3). */
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

const sensor_provider_ops_t g_sht45_provider_ops = {
    .name = "sht45",
    .init = sht45_init,
    .get_sensor_bitmap = sht45_get_sensor_bitmap,
    .get_min_polling_period_ms = sht45_get_min_polling_period_ms,
    .start_measurement = sht45_start_measurement,
    .read_measurement = sht45_read_measurement,
    .get_status = sht45_get_status,
    .deinit = sht45_deinit,
};
