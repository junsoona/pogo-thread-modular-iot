#include "power_base_config/power_base_config.h"
#include "module_config/gpio_profile.h"
#include "pogo_proto/pogo_crc16.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "power_base_config";
static bool s_partition_ready = false;

/*
 * Wire layout of the "blob" NVS value (namespace POWER_BASE_CONFIG_NAMESPACE).
 * All multi-byte integers are big-endian, matching module_config's own
 * convention. This is the single source of truth shared with
 * tools/pack_power_base_config.py -- if you change one, change the other and
 * re-generate every image.
 *
 *   offset  size  field
 *   0       4     schema_version (u32)
 *   4       4     config_revision (u32)
 *   8       2     downstream_port.uart_num (i16)
 *   10      2     downstream_port.tx_gpio (i16)
 *   12      2     downstream_port.rx_gpio (i16)
 *   14      2     downstream_port.detect_in_gpio (i16)
 *   16      2     downstream_port.detect_out_gpio (i16)
 *   18      1     downstream_port.detect_out_active_low (u8 bool)
 *   19      1     reserved (u8, must be 0)
 *   20      4     downstream_port.debounce_ms (u32)
 *   24      4     ack_timeout_ms (u32, 0 = parent_service's own Kconfig default)
 *   28      4     topology_stale_timeout_ms (u32)
 *   32      4     heartbeat_timeout_ms (u32)
 *   36      2     power_enable_gpio (i16, -1 = no power-switching hardware)
 *   38      1     power_enable_active_high (u8 bool)
 *   39      1     reserved (u8, must be 0)
 *   40      4     power_startup_delay_ms (u32)
 *   = 44 bytes total (CFG_BLOB_LEN)
 */
#define CFG_BLOB_LEN 44

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

static void unpack_blob(const uint8_t *blob, power_base_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema_version = rd_u32(blob + 0);
    out->config_revision = rd_u32(blob + 4);
    out->downstream_port.uart_num = rd_i16(blob + 8);
    out->downstream_port.tx_gpio = rd_i16(blob + 10);
    out->downstream_port.rx_gpio = rd_i16(blob + 12);
    out->downstream_port.detect_in_gpio = rd_i16(blob + 14);
    out->downstream_port.detect_out_gpio = rd_i16(blob + 16);
    out->downstream_port.detect_out_active_low = blob[18] != 0;
    out->downstream_port.debounce_ms = rd_u32(blob + 20);
    out->ack_timeout_ms = rd_u32(blob + 24);
    out->topology_stale_timeout_ms = rd_u32(blob + 28);
    out->heartbeat_timeout_ms = rd_u32(blob + 32);
    out->power_enable_gpio = rd_i16(blob + 36);
    out->power_enable_active_high = blob[38] != 0;
    out->power_startup_delay_ms = rd_u32(blob + 40);
}

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

static esp_err_t validate_config(const power_base_config_t *cfg)
{
    const power_base_port_config_t *p = &cfg->downstream_port;

    if (p->uart_num != 0 && p->uart_num != 1) {
        ESP_LOGE(TAG, "CONFIG_ERROR: downstream_port.uart_num=%d is not a valid ESP32-H2 UART controller (0 or 1)",
                 (int)p->uart_num);
        return ESP_ERR_INVALID_ARG;
    }

    int32_t candidates[5] = {p->tx_gpio, p->rx_gpio, p->detect_in_gpio, p->detect_out_gpio, cfg->power_enable_gpio};
    static const char *field_names[5] = {"downstream_port.tx_gpio", "downstream_port.rx_gpio",
                                          "downstream_port.detect_in_gpio", "downstream_port.detect_out_gpio",
                                          "power_enable_gpio"};
    for (int i = 0; i < 5; i++) {
        esp_err_t gerr = check_gpio_field(field_names[i], candidates[i]);
        if (gerr != ESP_OK) {
            return gerr;
        }
    }
    if (p->tx_gpio < 0 || p->rx_gpio < 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: downstream_port.tx_gpio/rx_gpio must be set -- the power base's one POGO port "
                      "is always enabled, unlike module_config's dev-compat exemption");
        return ESP_ERR_INVALID_ARG;
    }
    if (p->detect_in_gpio < 0 || p->detect_out_gpio < 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: downstream_port.detect_in_gpio/detect_out_gpio must both be set -- the power "
                      "base has no dev-compat mode to exempt this (see docs/power-base.md)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!p->detect_out_active_low) {
        ESP_LOGE(TAG, "CONFIG_ERROR: downstream_port.detect_out_active_low=false is not supported by the current "
                      "electrical design/pogo_port implementation (DETECT_OUT is open-drain active-low only)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (candidates[i] == candidates[j]) {
                ESP_LOGE(TAG, "CONFIG_ERROR: GPIO%d is assigned to more than one function (%s and %s)",
                         (int)candidates[i], field_names[i], field_names[j]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    if (cfg->power_enable_gpio >= 0) {
        for (int i = 0; i < 4; i++) {
            if (candidates[i] == cfg->power_enable_gpio) {
                ESP_LOGE(TAG, "CONFIG_ERROR: GPIO%d is assigned to more than one function (%s and power_enable_gpio)",
                         (int)cfg->power_enable_gpio, field_names[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    if (cfg->topology_stale_timeout_ms == 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: topology_stale_timeout_ms must not be 0");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->heartbeat_timeout_ms == 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: heartbeat_timeout_ms must not be 0");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->heartbeat_timeout_ms >= cfg->topology_stale_timeout_ms) {
        ESP_LOGE(TAG, "CONFIG_ERROR: heartbeat_timeout_ms=%u must be smaller than topology_stale_timeout_ms=%u "
                      "(a node would already read as stale before it could ever miss a heartbeat)",
                 (unsigned)cfg->heartbeat_timeout_ms, (unsigned)cfg->topology_stale_timeout_ms);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t ensure_partition(void)
{
    if (s_partition_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init_partition(POWER_BASE_CONFIG_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cfg partition init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_partition_ready = true;
    return ESP_OK;
}

esp_err_t power_base_config_load(power_base_config_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_err_t err = ensure_partition();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(POWER_BASE_CONFIG_NVS_PARTITION, POWER_BASE_CONFIG_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cannot open namespace '%s': %s", POWER_BASE_CONFIG_NAMESPACE,
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
        if (out->schema_version != POWER_BASE_CONFIG_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "CONFIG_ERROR: unsupported schema_version=%u (firmware expects %u)",
                     (unsigned)out->schema_version, (unsigned)POWER_BASE_CONFIG_SCHEMA_VERSION);
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

    ESP_LOGI(TAG, "loaded: schema=%u rev=%u uart=%d tx=%d rx=%d detect_in=%d detect_out=%d "
                  "ack_timeout_ms=%u stale_timeout_ms=%u heartbeat_timeout_ms=%u power_enable_gpio=%d",
             (unsigned)out->schema_version, (unsigned)out->config_revision, (int)out->downstream_port.uart_num,
             (int)out->downstream_port.tx_gpio, (int)out->downstream_port.rx_gpio,
             (int)out->downstream_port.detect_in_gpio, (int)out->downstream_port.detect_out_gpio,
             (unsigned)out->ack_timeout_ms, (unsigned)out->topology_stale_timeout_ms,
             (unsigned)out->heartbeat_timeout_ms, (int)out->power_enable_gpio);
    return ESP_OK;
}
