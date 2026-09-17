#include "power_base_bridge_config/power_base_bridge_config.h"
#include "pogo_proto/pogo_crc16.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pb_bridge_config";

/*
 * Wire layout of the "blob" NVS value (namespace POWER_BASE_BRIDGE_CONFIG_NAMESPACE).
 * All multi-byte integers big-endian, matching every other blob in this
 * codebase (power_base_config's own "power_base_cfg:blob" included).
 *
 *   offset  size  field
 *   0       4     schema_version (u32)
 *   4       1     enabled (u8 bool)
 *   5       1     reserved (u8, must be 0)
 *   6       16    host_addr (raw IPv6 bytes, network byte order as-is)
 *   22      2     port (u16)
 *   = 24 bytes total (CFG_BLOB_LEN)
 */
#define CFG_BLOB_LEN 24

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void unpack_blob(const uint8_t *blob, uint32_t *schema_version, power_base_bridge_config_t *out)
{
    memset(out, 0, sizeof(*out));
    *schema_version = rd_u32(blob + 0);
    out->enabled = blob[4] != 0;
    memcpy(out->host_addr, blob + 6, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    out->port = rd_u16(blob + 22);
}

/* True if `addr` is the IPv6 unspecified address (::, all-zero). */
static bool is_unspecified(const uint8_t addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN])
{
    for (unsigned i = 0; i < POWER_BASE_BRIDGE_CONFIG_ADDR_LEN; i++) {
        if (addr[i] != 0) {
            return false;
        }
    }
    return true;
}

/* True if `addr` is an IPv6 multicast address (ff00::/8). */
static bool is_multicast(const uint8_t addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN])
{
    return addr[0] == 0xFF;
}

/* True if `addr` is an IPv6 link-local unicast address (fe80::/10):
 * the first 10 bits are 1111111010, i.e. byte0==0xFE and the top two bits
 * of byte1 are exactly 0b10. */
static bool is_link_local(const uint8_t addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN])
{
    return addr[0] == 0xFE && (addr[1] & 0xC0) == 0x80;
}

static esp_err_t validate_enabled_config(const power_base_bridge_config_t *cfg)
{
    if (is_unspecified(cfg->host_addr)) {
        ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge enabled but host_addr is the unspecified address (::) -- refusing "
                      "to start the bridge exporter");
        return ESP_ERR_INVALID_ARG;
    }
    if (is_multicast(cfg->host_addr)) {
        ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge enabled but host_addr is a multicast address (ff00::/8) -- refusing "
                      "to start the bridge exporter");
        return ESP_ERR_INVALID_ARG;
    }
    if (is_link_local(cfg->host_addr)) {
        ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge enabled but host_addr is a link-local address (fe80::/10) -- this "
                      "component has no interface-index to disambiguate a link-local scope; use the Linux OTBR "
                      "host's real Mesh-Local/backbone address (see `sudo ot-ctl ipaddr mleid`) -- refusing to "
                      "start the bridge exporter");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->port == 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge enabled but port is 0 -- refusing to start the bridge exporter");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool s_partition_ready = false;

static esp_err_t ensure_partition(void)
{
    if (s_partition_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init_partition(POWER_BASE_BRIDGE_CONFIG_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cfg partition init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_partition_ready = true;
    return ESP_OK;
}

esp_err_t power_base_bridge_config_load(power_base_bridge_config_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_err_t err = ensure_partition();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(POWER_BASE_BRIDGE_CONFIG_NVS_PARTITION, POWER_BASE_BRIDGE_CONFIG_NAMESPACE,
                                   NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* No "pb_bridge" namespace at all -- a pre-existing/legacy image, or
         * one simply never generated with a bridge host configured. NOT an
         * error: exporter stays disabled. */
        ESP_LOGI(TAG, "no '%s' namespace found -- bridge exporter disabled (legacy-image compatible)",
                 POWER_BASE_BRIDGE_CONFIG_NAMESPACE);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cannot open namespace '%s': %s", POWER_BASE_BRIDGE_CONFIG_NAMESPACE,
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
            ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge 'blob' key missing/unreadable: %s", esp_err_to_name(err));
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        if (blob_len != CFG_BLOB_LEN) {
            ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge config blob is %u bytes, firmware expects %u (schema mismatch?)",
                     (unsigned)blob_len, (unsigned)CFG_BLOB_LEN);
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = nvs_get_u16(h, "crc", &stored_crc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge 'crc' key missing/unreadable: %s", esp_err_to_name(err));
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        uint16_t computed_crc = pogo_crc16_ccitt(blob, blob_len);
        if (computed_crc != stored_crc) {
            ESP_LOGE(TAG, "CONFIG_ERROR: pb_bridge blob CRC mismatch (stored=0x%04x computed=0x%04x) -- refusing "
                          "to use it",
                     stored_crc, computed_crc);
            result = ESP_ERR_INVALID_CRC;
            break;
        }
        uint32_t schema_version;
        unpack_blob(blob, &schema_version, out);
        if (schema_version != POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "CONFIG_ERROR: unsupported pb_bridge schema_version=%u (firmware expects %u)",
                     (unsigned)schema_version, (unsigned)POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION);
            result = ESP_ERR_INVALID_VERSION;
            break;
        }
        if (out->enabled) {
            result = validate_enabled_config(out);
            if (result != ESP_OK) {
                break; /* validate_enabled_config() already logged the specific CONFIG_ERROR */
            }
        }
    } while (0);

    nvs_close(h);

    if (result != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return result;
    }

    if (out->enabled) {
        ESP_LOGI(TAG, "loaded: pb_bridge enabled=true port=%u (host address never logged)", (unsigned)out->port);
    } else {
        ESP_LOGI(TAG, "loaded: pb_bridge enabled=false -- bridge exporter will not start");
    }
    return ESP_OK;
}
