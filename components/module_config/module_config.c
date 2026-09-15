#include "module_config/module_config.h"
#include "module_config/gpio_profile.h"
#include "pogo_proto/pogo_crc16.h"

#include <string.h>
#include <stdio.h>
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
 * Schema v1 then stores upstream_port at 54 and downstream_port at 72
 * (= 90 bytes total). Schema v2 inserts this 14-byte UART extension:
 *   54      2     uart_tx_gpio (i16)
 *   56      2     uart_rx_gpio (i16)
 *   58      4     uart_baud_rate (u32)
 *   62      1     uart_mode (u8: 0 active, 1 passive)
 *   63      1     reserved (u8, must be 0)
 *   64      4     uart_warmup_ms (u32)
 *   68      18    upstream_port
 *   86      18    downstream_port
 *   = 104 bytes total. The loader accepts both layouts so installing new
 * firmware does not force existing I2C modules to re-flash their cfg image.
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
#define CFG_V1_HEADER_LEN 54
#define CFG_V1_BLOB_LEN (CFG_V1_HEADER_LEN + 2 * PORT_BLOB_LEN)
#define CFG_V2_HEADER_LEN 68
#define CFG_V2_BLOB_LEN (CFG_V2_HEADER_LEN + 2 * PORT_BLOB_LEN)

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

static void unpack_blob(const uint8_t *blob, size_t blob_len, module_config_t *out)
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
    size_t port_offset = CFG_V1_HEADER_LEN;
    out->uart_tx_gpio = -1;
    out->uart_rx_gpio = -1;
    if (blob_len == CFG_V2_BLOB_LEN) {
        out->uart_tx_gpio = rd_i16(blob + 54);
        out->uart_rx_gpio = rd_i16(blob + 56);
        out->uart_baud_rate = rd_u32(blob + 58);
        out->uart_mode = (module_uart_mode_t)blob[62];
        out->uart_warmup_ms = rd_u32(blob + 64);
        port_offset = CFG_V2_HEADER_LEN;
    }
    unpack_port(blob + port_offset, &out->upstream_port);
    unpack_port(blob + port_offset + PORT_BLOB_LEN, &out->downstream_port);
}

/*
 * ESP32-H2 GPIO usability profile (section 1 of the follow-up spec, GPIO
 * classification corrected in the follow-up GPIO-correction pass -- see
 * components/module_config/gpio_profile.h for the exact policy and why
 * GPIO2/3 are NOT strapping pins and GPIO6/7 ARE rejected here even though
 * they are not "reserved" in the strapping/flash/USB sense: they are simply
 * not bonded out on the ESP32-H2 SuperMini/SiP package at all).
 */
static esp_err_t check_gpio_field(const char *field_name, int32_t gpio)
{
    if (!gpio_profile_in_range(gpio)) {
        ESP_LOGE(TAG, "CONFIG_ERROR: %s=%d is out of range for ESP32-H2 (valid: -1 or 0..%d)", field_name,
                 (int)gpio, GPIO_PROFILE_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    const char *reason = gpio_profile_reserved_reason(gpio);
    if (reason != NULL) {
        ESP_LOGE(TAG, "CONFIG_ERROR: %s=%d is reserved on ESP32-H2 (%s) -- see docs/pogo-interface.md", field_name,
                 (int)gpio, reason);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/*
 * Sanity-checks the unpacked config beyond CRC/schema (which module_config_load()
 * already checked): a value that unpacks cleanly but describes an
 * impossible or unsupported board is still CONFIG_ERROR, not "try it and
 * see" (section 8 of the follow-up spec, tightened further by section 1 of
 * the follow-up hardware-bring-up spec). Every failure logs one
 * "CONFIG_ERROR: ..." line naming the specific field and value.
 *
 * dev_mode gates the checks that only make sense for a real two-port AUTO
 * board (full DETECT_IN/DETECT_OUT wiring, a real UPSTREAM BASE_ID) --
 * configs/dev_root.json and configs/dev_leaf.json are deliberately preserved
 * as a one-UART, no-DETECT regression fixture (see docs/pogo-interface.md
 * "development compat mode") and must keep validating exactly as before.
 */
static esp_err_t validate_config(const module_config_t *cfg, module_dev_mode_t dev_mode)
{
    if (cfg->bus_type != MODULE_BUS_I2C && cfg->bus_type != MODULE_BUS_SPI &&
        cfg->bus_type != MODULE_BUS_UART) {
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
    int32_t used_gpios[2 * 5 + 4 + 2];
    const char *used_field_names[2 * 5 + 4 + 2];
    int used_count = 0;

    const module_port_config_t *ports[2] = {&cfg->upstream_port, &cfg->downstream_port};
    const char *port_names[2] = {"upstream_port", "downstream_port"};
    static const char *field_suffix[5] = {".tx_gpio", ".rx_gpio", ".detect_in_gpio", ".detect_out_gpio",
                                           ".base_id_gpio"};
    char field_buf[2][5][40];

    bool auto_mode = (dev_mode == MODULE_DEV_MODE_AUTO);

    for (int i = 0; i < 2; i++) {
        const module_port_config_t *p = ports[i];
        int32_t candidates[5] = {p->tx_gpio, p->rx_gpio, p->detect_in_gpio, p->detect_out_gpio, p->base_id_gpio};
        for (int j = 0; j < 5; j++) {
            snprintf(field_buf[i][j], sizeof(field_buf[i][j]), "%s%s", port_names[i], field_suffix[j]);
            esp_err_t gerr = check_gpio_field(field_buf[i][j], candidates[j]);
            if (gerr != ESP_OK) {
                return gerr;
            }
        }
        if (p->uart_num >= 0) {
            /* ESP32-H2 has exactly two UART controllers (UART0/UART1) --
             * both are committed to the two POGO ports, so a valid uart_num
             * for an enabled port is 0 or 1, never anything else. */
            if (p->uart_num != 0 && p->uart_num != 1) {
                ESP_LOGE(TAG, "CONFIG_ERROR: %s.uart_num=%d is not a valid ESP32-H2 UART controller (0 or 1)",
                         port_names[i], (int)p->uart_num);
                return ESP_ERR_INVALID_ARG;
            }
            if (p->tx_gpio < 0 || p->rx_gpio < 0) {
                ESP_LOGE(TAG, "CONFIG_ERROR: %s is enabled (uart_num=%d) but tx_gpio/rx_gpio is not set", port_names[i],
                         (int)p->uart_num);
                return ESP_ERR_INVALID_ARG;
            }
            if (auto_mode && (p->detect_in_gpio < 0 || p->detect_out_gpio < 0)) {
                /* Real hardware (dev_mode=AUTO): an enabled port must have
                 * DETECT_IN and DETECT_OUT wired, or attach/detach can never
                 * be sensed and role_manager can never derive a role for it
                 * (section 1: "활성화된 포트에 UART, TX, RX, DETECT_IN,
                 * DETECT_OUT이 모두 있는지 검사"). The dev_root/dev_leaf
                 * compat images are exempt (dev_mode != AUTO) -- see
                 * docs/pogo-interface.md "development compat mode". */
                ESP_LOGE(TAG, "CONFIG_ERROR: %s is enabled with dev_mode=AUTO but detect_in_gpio=%d/detect_out_gpio=%d "
                              "-- both are required on real (AUTO) hardware",
                         port_names[i], (int)p->detect_in_gpio, (int)p->detect_out_gpio);
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
        for (int j = 0; j < 5; j++) {
            if (candidates[j] >= 0) {
                used_gpios[used_count] = candidates[j];
                used_field_names[used_count] = field_buf[i][j];
                used_count++;
            }
        }
    }

    if (cfg->upstream_port.uart_num >= 0 && cfg->downstream_port.uart_num >= 0 &&
        cfg->upstream_port.uart_num == cfg->downstream_port.uart_num) {
        ESP_LOGE(TAG, "CONFIG_ERROR: upstream_port and downstream_port both enabled on the same uart_num=%d",
                 (int)cfg->upstream_port.uart_num);
        return ESP_ERR_INVALID_ARG;
    }

    /* BASE_ID is only ever meaningful on the UPSTREAM-facing side of a
     * module (see docs/pogo-interface.md) -- a DOWNSTREAM port declaring one
     * is always a config authoring mistake, in every dev_mode, not just AUTO. */
    if (cfg->downstream_port.base_id_gpio != -1) {
        ESP_LOGE(TAG, "CONFIG_ERROR: downstream_port.base_id_gpio=%d must be -1 (BASE_ID is UPSTREAM-only)",
                 (int)cfg->downstream_port.base_id_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (auto_mode && cfg->upstream_port.base_id_gpio < 0) {
        /* Real hardware (AUTO): ROOT/power-base detection depends entirely
         * on reading UPSTREAM BASE_ID -- see role_manager's determination
         * table. Missing it here would silently make ROOT unreachable. */
        ESP_LOGE(TAG, "CONFIG_ERROR: dev_mode=AUTO but upstream_port.base_id_gpio is not set (-1) -- "
                      "ROOT cannot ever be derived without it");
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->bus_type == MODULE_BUS_I2C) {
        esp_err_t gerr = check_gpio_field("i2c_sda_gpio", cfg->i2c_sda_gpio);
        if (gerr == ESP_OK) {
            gerr = check_gpio_field("i2c_scl_gpio", cfg->i2c_scl_gpio);
        }
        if (gerr != ESP_OK) {
            return gerr;
        }
        if (cfg->i2c_sda_gpio >= 0) {
            used_gpios[used_count] = cfg->i2c_sda_gpio;
            used_field_names[used_count++] = "i2c_sda_gpio";
        }
        if (cfg->i2c_scl_gpio >= 0) {
            used_gpios[used_count] = cfg->i2c_scl_gpio;
            used_field_names[used_count++] = "i2c_scl_gpio";
        }
    } else if (cfg->bus_type == MODULE_BUS_SPI) {
        const char *spi_names[4] = {"spi_mosi_gpio", "spi_miso_gpio", "spi_sclk_gpio", "spi_cs_gpio"};
        int32_t spi_gpios[4] = {cfg->spi_mosi_gpio, cfg->spi_miso_gpio, cfg->spi_sclk_gpio, cfg->spi_cs_gpio};
        for (int i = 0; i < 4; i++) {
            esp_err_t gerr = check_gpio_field(spi_names[i], spi_gpios[i]);
            if (gerr != ESP_OK) {
                return gerr;
            }
            if (spi_gpios[i] >= 0) {
                used_gpios[used_count] = spi_gpios[i];
                used_field_names[used_count++] = spi_names[i];
            }
        }
    } else {
        if (cfg->schema_version < 2) {
            ESP_LOGE(TAG, "CONFIG_ERROR: UART sensor bus requires module config schema v2");
            return ESP_ERR_INVALID_VERSION;
        }
        if (cfg->bus_number > 1) {
            ESP_LOGE(TAG, "CONFIG_ERROR: UART bus_number=%u is not a valid ESP32-H2 UART controller (0 or 1)",
                     (unsigned)cfg->bus_number);
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->uart_baud_rate == 0) {
            ESP_LOGE(TAG, "CONFIG_ERROR: uart_baud_rate must not be 0");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->uart_mode != MODULE_UART_MODE_ACTIVE && cfg->uart_mode != MODULE_UART_MODE_PASSIVE) {
            ESP_LOGE(TAG, "CONFIG_ERROR: unsupported uart_mode=%d", (int)cfg->uart_mode);
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t gerr = check_gpio_field("uart_tx_gpio", cfg->uart_tx_gpio);
        if (gerr == ESP_OK) {
            gerr = check_gpio_field("uart_rx_gpio", cfg->uart_rx_gpio);
        }
        if (gerr != ESP_OK || cfg->uart_tx_gpio < 0 || cfg->uart_rx_gpio < 0) {
            if (gerr == ESP_OK) {
                ESP_LOGE(TAG, "CONFIG_ERROR: UART sensor bus requires uart_tx_gpio and uart_rx_gpio");
            }
            return ESP_ERR_INVALID_ARG;
        }
        used_gpios[used_count] = cfg->uart_tx_gpio;
        used_field_names[used_count++] = "uart_tx_gpio";
        used_gpios[used_count] = cfg->uart_rx_gpio;
        used_field_names[used_count++] = "uart_rx_gpio";

        const module_port_config_t *down = &cfg->downstream_port;
        if (down->uart_num != -1 || down->tx_gpio != -1 || down->rx_gpio != -1 ||
            down->detect_in_gpio != -1 || down->detect_out_gpio != -1 || down->base_id_gpio != -1) {
            ESP_LOGE(TAG, "CONFIG_ERROR: UART sensor modules are terminal-only; downstream_port must be fully disabled");
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->upstream_port.uart_num == (int32_t)cfg->bus_number) {
            ESP_LOGE(TAG, "CONFIG_ERROR: sensor UART%u conflicts with enabled upstream POGO UART",
                     (unsigned)cfg->bus_number);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (cfg->driver_id == 0x504D3530u) { /* SENSOR_DRIVER_ID_PMS5003 without a module_config -> sensor_provider cycle */
        if (cfg->bus_type != MODULE_BUS_UART || cfg->uart_baud_rate != 9600) {
            ESP_LOGE(TAG, "CONFIG_ERROR: PMS5003 requires a 9600-baud UART sensor bus");
            return ESP_ERR_INVALID_ARG;
        }
    } else if (cfg->bus_type == MODULE_BUS_UART) {
        ESP_LOGE(TAG, "CONFIG_ERROR: UART sensor bus is currently supported only for PMS5003");
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (int i = 0; i < used_count; i++) {
        for (int j = i + 1; j < used_count; j++) {
            if (used_gpios[i] == used_gpios[j]) {
                ESP_LOGE(TAG, "CONFIG_ERROR: GPIO%d is assigned to more than one function (%s and %s)",
                         (int)used_gpios[i], used_field_names[i], used_field_names[j]);
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

    uint8_t blob[CFG_V2_BLOB_LEN];
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
        if (blob_len != CFG_V1_BLOB_LEN && blob_len != CFG_V2_BLOB_LEN) {
            ESP_LOGE(TAG, "CONFIG_ERROR: config blob is %u bytes, firmware expects schema-v1 %u or schema-v2 %u",
                     (unsigned)blob_len, (unsigned)CFG_V1_BLOB_LEN, (unsigned)CFG_V2_BLOB_LEN);
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
        unpack_blob(blob, blob_len, out);
        bool schema_matches_layout =
            (out->schema_version == 1 && blob_len == CFG_V1_BLOB_LEN) ||
            (out->schema_version == MODULE_CONFIG_SCHEMA_VERSION && blob_len == CFG_V2_BLOB_LEN);
        if (!schema_matches_layout) {
            ESP_LOGE(TAG, "CONFIG_ERROR: unsupported schema/layout combination: schema_version=%u blob_len=%u",
                     (unsigned)out->schema_version, (unsigned)blob_len);
            result = ESP_ERR_INVALID_VERSION;
            break;
        }
        /* module_config_load_dev_mode() reads a different namespace on the
         * same partition and is safe to call before nvs_close(h) below (it
         * opens its own handle) -- see its own doc comment. Needed here
         * (not just by module main, which also calls it separately) so the
         * AUTO-only checks inside validate_config() (full DETECT wiring,
         * UPSTREAM BASE_ID) are enforced at load time, not left to whatever
         * happens later at runtime with a half-wired port. */
        result = validate_config(out, module_config_load_dev_mode());
        if (result != ESP_OK) {
            break; /* validate_config() already logged the specific CONFIG_ERROR */
        }
    } while (0);

    nvs_close(h);

    if (result != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return result;
    }

    const char *bus_name = out->bus_type == MODULE_BUS_I2C ? "i2c" :
                           out->bus_type == MODULE_BUS_SPI ? "spi" : "uart";
    ESP_LOGI(TAG, "loaded: schema=%u rev=%u driver_id=%u sensor_bitmap=0x%04x bus=%s#%u timeout=%ums",
             (unsigned)out->schema_version, (unsigned)out->config_revision, (unsigned)out->driver_id,
             out->sensor_type_bitmap, bus_name, out->bus_number,
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
