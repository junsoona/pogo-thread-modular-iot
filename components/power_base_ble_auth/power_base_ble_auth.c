#include "power_base_ble_auth/power_base_ble_auth.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pb_ble_auth";

static const uint8_t k_magic[4] = {'P', 'B', 'A', '1'};
#define REC_VERSION_OFFSET 4
#define REC_GROUP_OFFSET 5
#define REC_SALT_LEN_OFFSET 6
#define REC_RESERVED_OFFSET 7
#define REC_EUI64_OFFSET 8
#define REC_SALT_OFFSET 16
#define REC_VERIFIER_OFFSET 32
#define REC_CRC_OFFSET 416

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void wipe(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (len--) {
        *p++ = 0;
    }
}

/* True when the big-endian number in v[0..len) is 0 or 1. With such a
 * verifier SRP's shared secret no longer depends on the setup code. */
static bool verifier_is_degenerate(const uint8_t *v, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (v[i] != 0) {
            return false;
        }
    }
    return v[len - 1] <= 1;
}

static bool all_zero(const uint8_t *p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

const char *pb_ble_auth_status_name(pb_ble_auth_status_t status)
{
    switch (status) {
    case PB_BLE_AUTH_OK: return "ok";
    case PB_BLE_AUTH_ABSENT: return "absent";
    case PB_BLE_AUTH_CORRUPT: return "corrupt";
    case PB_BLE_AUTH_UNSUPPORTED_VERSION: return "unsupported_version";
    case PB_BLE_AUTH_FOREIGN: return "foreign_eui64";
    case PB_BLE_AUTH_READ_ERROR: return "read_error";
    }
    return "unknown";
}

pb_ble_auth_status_t pb_ble_auth_parse(const uint8_t *raw, size_t len, const uint8_t eui64[PB_BLE_AUTH_EUI64_LEN],
                                       pb_ble_auth_cred_t *out)
{
    memset(out, 0, sizeof(*out));
    if (raw == NULL || len != PB_BLE_AUTH_RECORD_LEN) {
        return PB_BLE_AUTH_CORRUPT;
    }
    uint32_t stored_crc = (uint32_t)raw[REC_CRC_OFFSET] | ((uint32_t)raw[REC_CRC_OFFSET + 1] << 8) |
                          ((uint32_t)raw[REC_CRC_OFFSET + 2] << 16) | ((uint32_t)raw[REC_CRC_OFFSET + 3] << 24);
    if (memcmp(raw, k_magic, sizeof(k_magic)) != 0 || crc32_ieee(raw, REC_CRC_OFFSET) != stored_crc) {
        return PB_BLE_AUTH_CORRUPT;
    }
    if (raw[REC_VERSION_OFFSET] != PB_BLE_AUTH_FORMAT_VERSION ||
        raw[REC_GROUP_OFFSET] != PB_BLE_AUTH_SRP_GROUP_SEC2_3072_SHA512) {
        return PB_BLE_AUTH_UNSUPPORTED_VERSION;
    }
    if (raw[REC_SALT_LEN_OFFSET] != PB_BLE_AUTH_SALT_LEN || raw[REC_RESERVED_OFFSET] != 0 ||
        all_zero(&raw[REC_SALT_OFFSET], PB_BLE_AUTH_SALT_LEN) ||
        verifier_is_degenerate(&raw[REC_VERIFIER_OFFSET], PB_BLE_AUTH_VERIFIER_LEN)) {
        return PB_BLE_AUTH_CORRUPT;
    }
    if (memcmp(&raw[REC_EUI64_OFFSET], eui64, PB_BLE_AUTH_EUI64_LEN) != 0) {
        return PB_BLE_AUTH_FOREIGN;
    }
    memcpy(out->salt, &raw[REC_SALT_OFFSET], PB_BLE_AUTH_SALT_LEN);
    memcpy(out->verifier, &raw[REC_VERIFIER_OFFSET], PB_BLE_AUTH_VERIFIER_LEN);
    return PB_BLE_AUTH_OK;
}

pb_ble_auth_status_t pb_ble_auth_load(const uint8_t eui64[PB_BLE_AUTH_EUI64_LEN], pb_ble_auth_cred_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_err_t err = nvs_flash_init_partition(PB_BLE_AUTH_NVS_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open the '%s' partition (%s)", PB_BLE_AUTH_NVS_PARTITION, esp_err_to_name(err));
        return PB_BLE_AUTH_READ_ERROR;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(PB_BLE_AUTH_NVS_PARTITION, PB_BLE_AUTH_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return PB_BLE_AUTH_ABSENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open namespace '%s' (%s)", PB_BLE_AUTH_NVS_NAMESPACE, esp_err_to_name(err));
        return PB_BLE_AUTH_READ_ERROR;
    }

    size_t len = 0;
    err = nvs_get_blob(h, PB_BLE_AUTH_NVS_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return PB_BLE_AUTH_ABSENT;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "size query failed (%s)", esp_err_to_name(err));
        return PB_BLE_AUTH_READ_ERROR;
    }
    if (len != PB_BLE_AUTH_RECORD_LEN) {
        nvs_close(h);
        return PB_BLE_AUTH_CORRUPT;
    }

    uint8_t raw[PB_BLE_AUTH_RECORD_LEN];
    err = nvs_get_blob(h, PB_BLE_AUTH_NVS_KEY, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        wipe(raw, sizeof(raw));
        ESP_LOGE(TAG, "read failed (%s)", esp_err_to_name(err));
        return PB_BLE_AUTH_READ_ERROR;
    }
    pb_ble_auth_status_t status = pb_ble_auth_parse(raw, len, eui64, out);
    wipe(raw, sizeof(raw));
    return status;
}

void pb_ble_auth_username(const uint8_t eui64[PB_BLE_AUTH_EUI64_LEN], char out[PB_BLE_AUTH_USERNAME_BUF_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < PB_BLE_AUTH_EUI64_LEN; i++) {
        out[2 * i] = hex[eui64[i] >> 4];
        out[2 * i + 1] = hex[eui64[i] & 0x0f];
    }
    out[2 * PB_BLE_AUTH_EUI64_LEN] = '\0';
}
