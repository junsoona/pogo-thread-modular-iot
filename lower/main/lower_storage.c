#include "lower_storage.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "lower_storage";
static const char *NAMESPACE = "pogo";

static const char *KEY_UUID = "uuid";
static const char *KEY_SLOT = "slot";
static const char *KEY_DS_LEN = "ds_len";
static const char *KEY_DS_TLV = "ds_tlv";

esp_err_t lower_storage_init(void)
{
    /* nvs_flash_init() itself is called once at app startup (app_main.c);
     * this just confirms the "pogo" namespace is reachable. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t lower_storage_save_provision(const uint8_t uuid[POGO_UUID_LEN], uint8_t slot_id,
                                        const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    do {
        err = nvs_set_blob(handle, KEY_UUID, uuid, POGO_UUID_LEN);
        if (err != ESP_OK) break;
        err = nvs_set_u8(handle, KEY_SLOT, slot_id);
        if (err != ESP_OK) break;
        err = nvs_set_u16(handle, KEY_DS_LEN, dataset_len);
        if (err != ESP_OK) break;
        err = nvs_set_blob(handle, KEY_DS_TLV, dataset_tlvs, dataset_len);
        if (err != ESP_OK) break;
        err = nvs_commit(handle);
    } while (0);

    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_provision failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t lower_storage_load_provision(uint8_t uuid_out[POGO_UUID_LEN], uint8_t *slot_id_out, uint8_t *dataset_out,
                                        size_t dataset_buf_size, uint16_t *dataset_len_out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err; /* ESP_ERR_NVS_NOT_FOUND on first-ever boot */
    }

    size_t uuid_len = POGO_UUID_LEN;
    uint16_t ds_len = 0;

    do {
        err = nvs_get_blob(handle, KEY_UUID, uuid_out, &uuid_len);
        if (err != ESP_OK) break;
        err = nvs_get_u8(handle, KEY_SLOT, slot_id_out);
        if (err != ESP_OK) break;
        err = nvs_get_u16(handle, KEY_DS_LEN, &ds_len);
        if (err != ESP_OK) break;
        if (ds_len > dataset_buf_size) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        size_t ds_buf_len = ds_len;
        err = nvs_get_blob(handle, KEY_DS_TLV, dataset_out, &ds_buf_len);
        if (err != ESP_OK) break;
        *dataset_len_out = ds_len;
    } while (0);

    nvs_close(handle);
    return err;
}
