#include "power_base_thread_config/power_base_thread_config.h"

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pb_thread_config";

/* Must match components/power_base_config's POWER_BASE_CONFIG_NVS_PARTITION
 * -- the SAME physical "cfg" NVS partition (see power_base/partitions.csv),
 * a different namespace within it. Kept as this component's own literal
 * (rather than including power_base_config.h) so this component has no
 * dependency on power_base_config at all -- see this component's header
 * comment on why it stays small/independent. */
#define PB_THREAD_CONFIG_NVS_PARTITION "cfg"

static bool s_partition_ready = false;

static esp_err_t ensure_partition(void)
{
    if (s_partition_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init_partition(PB_THREAD_CONFIG_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cfg partition init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_partition_ready = true;
    return ESP_OK;
}

esp_err_t power_base_thread_config_load(power_base_thread_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = POWER_BASE_THREAD_MODE_STANDALONE;

    esp_err_t err = ensure_partition();
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(PB_THREAD_CONFIG_NVS_PARTITION, POWER_BASE_THREAD_CONFIG_NAMESPACE, NVS_READONLY,
                                   &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* No "pb_thread" namespace at all -- a pre-existing/legacy image
         * (or a freshly-generated one that simply never had an external
         * Dataset requested). NOT an error: treated as STANDALONE. */
        ESP_LOGI(TAG, "no '%s' namespace found -- treating as STANDALONE (legacy-image compatible)",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE);
        out->mode = POWER_BASE_THREAD_MODE_STANDALONE;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: cannot open namespace '%s': %s", POWER_BASE_THREAD_CONFIG_NAMESPACE,
                 esp_err_to_name(err));
        memset(out, 0, sizeof(*out));
        return err;
    }

    uint8_t mode_raw = 0;
    err = nvs_get_u8(h, "mode", &mode_raw);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace present but no "mode" key -- same legacy-compatible
         * STANDALONE treatment as a missing namespace entirely. */
        ESP_LOGI(TAG, "'%s' namespace present but no 'mode' key -- treating as STANDALONE",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE);
        nvs_close(h);
        out->mode = POWER_BASE_THREAD_MODE_STANDALONE;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: '%s:mode' unreadable: %s", POWER_BASE_THREAD_CONFIG_NAMESPACE,
                 esp_err_to_name(err));
        nvs_close(h);
        memset(out, 0, sizeof(*out));
        return err;
    }

    if (mode_raw == (uint8_t)POWER_BASE_THREAD_MODE_STANDALONE) {
        nvs_close(h);
        out->mode = POWER_BASE_THREAD_MODE_STANDALONE;
        ESP_LOGI(TAG, "loaded: mode=STANDALONE");
        return ESP_OK;
    }
    if (mode_raw != (uint8_t)POWER_BASE_THREAD_MODE_EXTERNAL_OTBR) {
        ESP_LOGE(TAG, "CONFIG_ERROR: unsupported '%s:mode'=%u (expected 0=STANDALONE or 1=EXTERNAL_OTBR)",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE, (unsigned)mode_raw);
        nvs_close(h);
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_ARG;
    }

    /* EXTERNAL_OTBR: active_tlvs is mandatory from here on. Fail closed on
     * anything wrong -- the caller must never fall back to forming/resuming
     * a standalone network (see this component's header comment and
     * docs/power-base.md). */
    size_t tlv_len = 0;
    err = nvs_get_blob(h, "active_tlvs", NULL, &tlv_len); /* size-query form: out_value == NULL */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: EXTERNAL_OTBR mode but '%s:active_tlvs' missing/unreadable: %s -- fail "
                      "closed, no standalone fallback",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE, esp_err_to_name(err));
        nvs_close(h);
        memset(out, 0, sizeof(*out));
        return ESP_ERR_NOT_FOUND;
    }
    if (tlv_len == 0) {
        ESP_LOGE(TAG, "CONFIG_ERROR: EXTERNAL_OTBR mode but '%s:active_tlvs' is empty (0 bytes) -- fail closed, "
                      "no standalone fallback",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE);
        nvs_close(h);
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_SIZE;
    }
    if (tlv_len > POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN) {
        ESP_LOGE(TAG, "CONFIG_ERROR: EXTERNAL_OTBR mode but '%s:active_tlvs' is %u bytes (max %u) -- fail "
                      "closed, no standalone fallback",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE, (unsigned)tlv_len,
                 (unsigned)POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN);
        nvs_close(h);
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_SIZE;
    }

    size_t read_len = tlv_len;
    err = nvs_get_blob(h, "active_tlvs", out->active_tlvs, &read_len);
    nvs_close(h);
    if (err != ESP_OK || read_len != tlv_len) {
        ESP_LOGE(TAG, "CONFIG_ERROR: EXTERNAL_OTBR mode but '%s:active_tlvs' could not be fully read: %s -- fail "
                      "closed, no standalone fallback",
                 POWER_BASE_THREAD_CONFIG_NAMESPACE, esp_err_to_name(err));
        memset(out, 0, sizeof(*out));
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    out->mode = POWER_BASE_THREAD_MODE_EXTERNAL_OTBR;
    out->active_tlvs_len = (uint16_t)tlv_len;
    /* Dataset bytes are NEVER logged -- only mode and length (contains the
     * Network Key). See this component's header comment. */
    ESP_LOGI(TAG, "loaded: mode=EXTERNAL_OTBR active_tlvs_len=%u bytes (contents hidden)", (unsigned)tlv_len);
    return ESP_OK;
}
