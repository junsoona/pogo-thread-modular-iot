#include "module_config/module_config.h"
#include "pogo_proto/pogo_crc16.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "module_config";
static bool s_partition_ready = false;

/*
 * Wire layout of the "blob" NVS value (namespace MODULE_CONFIG_NAMESPACE).
 * All multi-byte integers are big-endian, matching pogo_proto's convention.
 * This layout is the single source of truth shared with tools/pack_module_config.py
 * (see docs/module-config.md) -- if you change it here, update both.
 *
 *   offset  size  field
 *   0       4     schema_version (u32)
 *   4       4     config_revision (u32)
 *   8       4     driver_id (u32)
 *   12      2     sensor_type_bitmap (u16)
 *   14      1     bus_type (u8)
 *   15      1     bus_number (u8)
 *   16      2     i2c_sda_gpio (i16)
 *   18      2     i2c_scl_gpio (i16)
 *   20      2     i2c_address (u16)
 *   22      2     spi_mosi_gpio (i16)
 *   24      2     spi_miso_gpio (i16)
 *   26      2     spi_sclk_gpio (i16)
 *   28      2     spi_cs_gpio (i16)
 *   30      4     spi_clock_hz (u32)
 *   34      16    calibration[4] (i32 each)
 *   50      4     measurement_timeout_ms (u32)
 *   54      18    upstream_port   (module_port_config_t blob, see PORT_BLOB_LEN below)
 *   72      18    downstream_port (module_port_config_t blob)
 *   = 90 bytes total (CFG_BLOB_LEN)
 *
 * Per-port blob (18 bytes):
 *   0  2  uart_num (i16, -1 = port not wired on this board)
 *   2  2  tx_gpio (i16)
 *   4  2  rx_gpio (i16)
 *   6  2  detect_in_gpio (i16, -1 = no DETECT hardware)
 *   8  2  detect_out_gpio (i16, -1 = no DETECT hardware)
 *   10 2  base_id_gpio (i16, -1 = n/a)
 *   12 1  detect_out_active_low (u8 bool)
 *   13 1  reserved (u8, must be 0)
 *   14 4  debounce_ms (u32)
 */
#define PORT_BLOB_LEN 18
#define CFG_HEADER_LEN 54
#define CFG_BLOB_LEN (CFG_HEADER_LEN + 2 * PORT_BLOB_LEN)

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int32_t rd_i16(const uint8_t *p)
{
    return (int16_t)rd_u16(p);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

static void unpack_port(const uint8_t *p, module_port_config_t *out)
{
    out->uart_num = rd_i16(p + 0);
    out->tx_gpio = rd_i16(p + 2);
    out->rx_gpio = rd_i16(p + 4);
    out->detect_in_gpio = rd_i16(p + 6);
    out->detect_out_gpio = rd_i16(p + 8);
    out->base_id_gpio = rd_i16(p + 10);
    out->detect_out_active_low = p[12] != 0;
    out->debounce_ms = rd_u32(p + 14);
}

static void unpack_blob(const uint8_t *blob, module_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema_version = rd_u32(blob + 0);
    out->config_revision = rd_u32(blob + 4);
    out->driver_id = rd_u32(blob + 8);
    out->sensor_type_bitmap = rd_u16(blob + 12);
    out->bus_type = (module_bus_type_t)blob[14];
    out->bus_number = blob[15];
    out->i2c_sda_gpio = rd_i16(blob + 16);
    out->i2c_scl_gpio = rd_i16(blob + 18);
    out->i2c_address = rd_u16(blob + 20);
    out->spi_mosi_gpio = rd_i16(blob + 22);
    out->spi_miso_gpio = rd_i16(blob + 24);
    out->spi_sclk_gpio = rd_i16(blob + 26);
    out->spi_cs_gpio = rd_i16(blob + 28);
    out->spi_clock_hz = rd_u32(blob + 30);
    for (int i = 0; i < 4; i++) {
        out->calibration[i] = rd_i32(blob + 34 + i * 4);
    }
    out->measurement_timeout_ms = rd_u32(blob + 50);
    unpack_port(blob + CFG_HEADER_LEN, &out->upstream_port);
    unpack_port(blob + CFG_HEADER_LEN + PORT_BLOB_LEN, &out->downstream_port);
}

static bool gpio_in_range(int32_t gpio)
{
    /* -1 means "not present on this board" and is always allowed; any other
     * negative value, or a value clearly outside a plausible GPIO count, is
     * a config authoring mistake. Deliberately generous (not tied to one
     * specific chip's exact usable pin list) since final GPIO numbers are
     * not decided by this refactor -- see docs/pogo-interface.md. */
    return gpio == -1 || (gpio >= 0 && gpio <= 31);
}

/*
 * Sanity-checks the unpacked config beyond CRC/schema (which module_config_load()
 * already checked): a value that unpacks cleanly but describes an
 * impossible or unsupported board is still CONFIG_ERROR, not "try it and
 * see" (section 8 of the follow-up spec). Every failure logs one
 * "CONFIG_ERROR: ..." line naming the specific problem.
 */
static esp_err_t validate_config(const module_config_t *cfg)
{
    if (cfg->bus_type != MODULE_BUS_I2C && cfg->bus_type != MODULE_BUS_SPI) {
        ESP_LOGE(TAG, "CONFIG_ERROR: unsupported bus_type=%d", (int)cfg->bus_type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (cfg->measurement_timeout_ms == 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: measurement_timeout_ms must not be 0");
        return ESP_ERR_INVALID_ARG;
    }

    /* Every GPIO actually in use, to catch one number assigned to more than
     * one function. -1 ("not present") entries are skipped. Sized for 2
     * ports x 5 GPIOs each, plus up to 4 bus GPIOs (SPI's worst case). */
    int32_t used_gpios[2 * 5 + 4];
    int used_count = 0;

    const module_port_config_t *ports[2] = {&cfg->upstream_port, &cfg->downstream_port};
    const char *port_names[2] = {"upstream_port", "downstream_port"};

    for (int i = 0; i < 2; i++) {
        const module_port_config_t *p = ports[i];
        if (!gpio_in_range(p->tx_gpio) || !gpio_in_range(p->rx_gpio) || !gpio_in_range(p->detect_in_gpio) ||
            !gpio_in_range(p->detect_out_gpio) || !gpio_in_range(p->base_id_gpio)) {
            ESP_LOGE(TAG, "CONFIG_ERROR: %s has a GPIO number out of range", port_names[i]);
            return ESP_ERR_INVALID_ARG;
        }
        if (p->uart_num >= 0) {
            if (p->tx_gpio < 0 || p->rx_gpio < 0) {
                ESP_LOGE(TAG, "CONFIG_ERROR: %s is enabled (uart_num=%d) but tx_gpio/rx_gpio is not set", port_names[i],
                         (int)p->uart_num);
                return ESP_ERR_INVALID_ARG;
            }
            if (p->detect_out_gpio >= 0 && !p->detect_out_active_low) {
                /* pogo_port's DETECT_OUT drive logic (set_detect_out() in
                 * pogo_port.c) is hardcoded to open-drain active-low; a
                 * config claiming active-high would silently behave as
                 * active-low anyway, which is worse than refusing to boot. */
                ESP_LOGE(TAG, "CONFIG_ERROR: %s.detect_out_active_low=false is not supported by the current "
                              "electrical design/pogo_port implementation (DETECT_OUT is open-drain active-low only)",
                         port_names[i]);
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
        int32_t candidates[5] = {p->tx_gpio, p->rx_gpio, p->detect_in_gpio, p->detect_out_gpio, p->base_id_gpio};
        for (int j = 0; j < 5; j++) {
            if (candidates[j] >= 0) {
                used_gpios[used_count++] = candidates[j];
            }
        }
    }

    if (cfg->upstream_port.uart_num >= 0 && cfg->downstream_port.uart_num >= 0 &&
        cfg->upstream_port.uart_num == cfg->downstream_port.uart_num) {
        ESP_LOGE(TAG, "CONFIG_ERROR: upstream_port and downstream_port both enabled on the same uart_num=%d",
                 (int)cfg->upstream_port.uart_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->bus_type == MODULE_BUS_I2C) {
        if (!gpio_in_range(cfg->i2c_sda_gpio) || !gpio_in_range(cfg->i2c_scl_gpio)) {
            ESP_LOGE(TAG, "CONFIG_ERROR: i2c_sda_gpio/i2c_scl_gpio out of range");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->i2c_sda_gpio >= 0) used_gpios[used_count++] = cfg->i2c_sda_gpio;
        if (cfg->i2c_scl_gpio >= 0) used_gpios[used_count++] = cfg->i2c_scl_gpio;
    } else {
        if (!gpio_in_range(cfg->spi_mosi_gpio) || !gpio_in_range(cfg->spi_miso_gpio) ||
            !gpio_in_range(cfg->spi_sclk_gpio) || !gpio_in_range(cfg->spi_cs_gpio)) {
            ESP_LOGE(TAG, "CONFIG_ERROR: spi_*_gpio out of range");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->spi_mosi_gpio >= 0) used_gpios[used_count++] = cfg->spi_mosi_gpio;
        if (cfg->spi_miso_gpio >= 0) used_gpios[used_count++] = cfg->spi_miso_gpio;
        if (cfg->spi_sclk_gpio >= 0) used_gpios[used_count++] = cfg->spi_sclk_gpio;
        if (cfg->spi_cs_gpio >= 0) used_gpios[used_count++] = cfg->spi_cs_gpio;
    }

    for (int i = 0; i < used_count; i++) {
        for (int j = i + 1; j < used_count; j++) {
            if (used_gpios[i] == used_gpios[j]) {
                ESP_LOGE(TAG, "CONFIG_ERROR: GPIO%d is assigned to more than one function", (int)used_gpios[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t ensure_partition(void)
{
    if (s_partition_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init_partition(MODULE_CONFIG_NVS_PARTITION);
    if (err != ESP_OK) {
        /* Unlike the dynamic "nvs" partition, we deliberately do NOT
         * auto-erase-and-retry here: "cfg" is generated offline and holds
         * the fixed sensor pairing, so a corrupt partition must be
         * re-flashed on purpose, never silently wiped by firmware. */
        ESP_LOGE(TAG, "CONFIG_ERROR: cfg partition init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_partition_ready = true;
    return ESP_OK;
}

esp_err_t module_config_load(module_config_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_err_t err = ensure_partition();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(MODULE_CONFIG_NVS_PARTITION, MODULE_CONFIG_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cannot open namespace '%s': %s", MODULE_CONFIG_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    uint8_t blob[CFG_BLOB_LEN];
    size_t blob_len = sizeof(blob);
    uint16_t stored_crc = 0;
    esp_err_t result = ESP_OK;

    do {
        err = nvs_get_blob(h, "blob", blob, &blob_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CONFIG_ERROR: 'blob' key missing/unreadable: %s", esp_err_to_name(err));
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        if (blob_len != CFG_BLOB_LEN) {
            ESP_LOGE(TAG, "CONFIG_ERROR: config blob is %u bytes, firmware expects %u (schema mismatch?)",
                     (unsigned)blob_len, (unsigned)CFG_BLOB_LEN);
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = nvs_get_u16(h, "crc", &stored_crc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CONFIG_ERROR: 'crc' key missing/unreadable: %s", esp_err_to_name(err));
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        uint16_t computed_crc = pogo_crc16_ccitt(blob, blob_len);
        if (computed_crc != stored_crc) {
            ESP_LOGE(TAG, "CONFIG_ERROR: blob CRC mismatch (stored=0x%04x computed=0x%04x) -- refusing to use it",
                     stored_crc, computed_crc);
            result = ESP_ERR_INVALID_CRC;
            break;
        }
        unpack_blob(blob, out);
        if (out->schema_version != MODULE_CONFIG_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "CONFIG_ERROR: unsupported schema_version=%u (firmware expects %u)",
                     (unsigned)out->schema_version, (unsigned)MODULE_CONFIG_SCHEMA_VERSION);
            result = ESP_ERR_INVALID_VERSION;
            break;
        }
        result = validate_config(out);
        if (result != ESP_OK) {
            break; /* validate_config() already logged the specific CONFIG_ERROR */
        }
    } while (0);

    nvs_close(h);

    if (result != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return result;
    }

    ESP_LOGI(TAG, "loaded: schema=%u rev=%u driver_id=%u sensor_bitmap=0x%04x bus=%s#%u timeout=%ums",
             (unsigned)out->schema_version, (unsigned)out->config_revision, (unsigned)out->driver_id,
             out->sensor_type_bitmap, out->bus_type == MODULE_BUS_I2C ? "i2c" : "spi", out->bus_number,
             (unsigned)out->measurement_timeout_ms);
    return ESP_OK;
}

module_dev_mode_t module_config_load_dev_mode(void)
{
    if (ensure_partition() != ESP_OK) {
        return MODULE_DEV_MODE_AUTO;
    }

    nvs_handle_t h;
    if (nvs_open_from_partition(MODULE_CONFIG_NVS_PARTITION, MODULE_CONFIG_DEV_NAMESPACE, NVS_READONLY, &h) !=
        ESP_OK) {
        return MODULE_DEV_MODE_AUTO; /* namespace absent entirely: normal production image */
    }

    uint8_t mode = MODULE_DEV_MODE_AUTO;
    esp_err_t err = nvs_get_u8(h, "dev_mode", &mode);
    nvs_close(h);

    if (err != ESP_OK || mode > MODULE_DEV_MODE_DEV_LEAF) {
        return MODULE_DEV_MODE_AUTO;
    }
    return (module_dev_mode_t)mode;
}

const char *module_dev_mode_to_str(module_dev_mode_t mode)
{
    switch (mode) {
    case MODULE_DEV_MODE_DEV_ROOT:
        return "DEV_ROOT";
    case MODULE_DEV_MODE_DEV_LEAF:
        return "DEV_LEAF";
    case MODULE_DEV_MODE_AUTO:
    default:
        return "AUTO";
    }
}
