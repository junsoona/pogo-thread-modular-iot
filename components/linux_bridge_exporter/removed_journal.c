/* See removed_journal.h. */
#include "removed_journal.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "topology_registry/topology_registry.h"

static const char *TAG = "removed_journal";

#define JOURNAL_NAMESPACE "pb_rm_journal"
#define JOURNAL_KEY "uuids"
/* Blob layout: [0] format version, [1] uuid count, then count x 8-byte
 * module_uuids. NVS already CRC-checks every entry; the version/count
 * header only guards against a layout change or a truncated value. */
#define JOURNAL_VERSION 1u
#define JOURNAL_HEADER_LEN 2u
#define JOURNAL_MAX_UUIDS TOPOLOGY_REGISTRY_MAX_NODES
#define JOURNAL_MAX_BLOB_LEN (JOURNAL_HEADER_LEN + JOURNAL_MAX_UUIDS * TOPOLOGY_UUID_LEN)

size_t removed_journal_load(uint8_t out[][TOPOLOGY_UUID_LEN], size_t capacity)
{
    nvs_handle_t h;
    if (nvs_open(JOURNAL_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    uint8_t blob[JOURNAL_MAX_BLOB_LEN];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, JOURNAL_KEY, blob, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return 0;
    }
    size_t count = (err == ESP_OK && len >= JOURNAL_HEADER_LEN) ? blob[1] : 0;
    if (err != ESP_OK || len < JOURNAL_HEADER_LEN || blob[0] != JOURNAL_VERSION || count > JOURNAL_MAX_UUIDS ||
        len != JOURNAL_HEADER_LEN + count * TOPOLOGY_UUID_LEN) {
        ESP_LOGE(TAG, "removed-tombstone journal unreadable or malformed (err=%s len=%u) -- discarded; any removal "
                      "it held will not be re-sent to Linux",
                 esp_err_to_name(err), (unsigned)len);
        (void)nvs_erase_key(h, JOURNAL_KEY);
        (void)nvs_commit(h);
        nvs_close(h);
        return 0;
    }
    nvs_close(h);
    if (count > capacity) {
        count = capacity;
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(out[i], blob + JOURNAL_HEADER_LEN + i * TOPOLOGY_UUID_LEN, TOPOLOGY_UUID_LEN);
    }
    return count;
}

esp_err_t removed_journal_save(const uint8_t uuids[][TOPOLOGY_UUID_LEN], size_t count)
{
    if (count > JOURNAL_MAX_UUIDS) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(JOURNAL_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (count == 0) {
        err = nvs_erase_key(h, JOURNAL_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        uint8_t blob[JOURNAL_MAX_BLOB_LEN];
        blob[0] = JOURNAL_VERSION;
        blob[1] = (uint8_t)count;
        for (size_t i = 0; i < count; i++) {
            memcpy(blob + JOURNAL_HEADER_LEN + i * TOPOLOGY_UUID_LEN, uuids[i], TOPOLOGY_UUID_LEN);
        }
        err = nvs_set_blob(h, JOURNAL_KEY, blob, JOURNAL_HEADER_LEN + count * TOPOLOGY_UUID_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
