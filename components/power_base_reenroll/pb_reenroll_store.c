#include "power_base_reenroll/pb_reenroll_store.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "pb_reenroll_st";

static const uint8_t k_window_magic[4] = {'P', 'B', 'W', '1'};
static const uint8_t k_journal_magic[4] = {'P', 'B', 'R', '1'};
#define WINDOW_CRC_OFFSET 20
#define JOURNAL_STAGE_OFFSET 5
#define JOURNAL_OP_OFFSET 16
#define JOURNAL_CRC_OFFSET 36
#define RECORD_MAX_LEN PB_REENROLL_JOURNAL_LEN

_Static_assert(PB_REENROLL_WINDOW_LEN <= RECORD_MAX_LEN, "one read buffer for both records");

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

static void put_crc_le(uint8_t *out, uint32_t crc)
{
    out[0] = (uint8_t)crc;
    out[1] = (uint8_t)(crc >> 8);
    out[2] = (uint8_t)(crc >> 16);
    out[3] = (uint8_t)(crc >> 24);
}

static uint32_t get_crc_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool all_zero(const uint8_t *p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

const char *pb_reenroll_rec_status_name(pb_reenroll_rec_status_t status)
{
    switch (status) {
    case PB_REENROLL_REC_ABSENT: return "absent";
    case PB_REENROLL_REC_VALID: return "valid";
    case PB_REENROLL_REC_CORRUPT: return "corrupt";
    case PB_REENROLL_REC_FOREIGN: return "foreign";
    case PB_REENROLL_REC_READ_ERROR: return "read_error";
    }
    return "unknown";
}

/* Reads `key` when it is exactly `expected_len` bytes. Never writes. */
static pb_reenroll_rec_status_t read_raw(const char *key, size_t expected_len, uint8_t raw[RECORD_MAX_LEN])
{
    memset(raw, 0, RECORD_MAX_LEN);
    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_REENROLL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return PB_REENROLL_REC_ABSENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: nvs_open failed (%s)", key, esp_err_to_name(err));
        return PB_REENROLL_REC_READ_ERROR;
    }
    size_t len = 0;
    err = nvs_get_blob(h, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return PB_REENROLL_REC_ABSENT;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "%s: size query failed (%s)", key, esp_err_to_name(err));
        return PB_REENROLL_REC_READ_ERROR;
    }
    if (len != expected_len) {
        nvs_close(h);
        ESP_LOGW(TAG, "%s: unexpected size %u", key, (unsigned)len);
        return PB_REENROLL_REC_CORRUPT;
    }
    err = nvs_get_blob(h, key, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        memset(raw, 0, RECORD_MAX_LEN);
        ESP_LOGE(TAG, "%s: read failed (%s)", key, esp_err_to_name(err));
        return PB_REENROLL_REC_READ_ERROR;
    }
    if (len != expected_len) {
        memset(raw, 0, RECORD_MAX_LEN);
        return PB_REENROLL_REC_CORRUPT;
    }
    return PB_REENROLL_REC_VALID;
}

/* set_blob + commit. The caller reads the result back. */
static esp_err_t write_raw(const char *key, const uint8_t *raw, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_REENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: nvs_open for write failed (%s)", key, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, key, raw, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: write failed (%s)", key, esp_err_to_name(err));
    }
    return err;
}

/* ---- window request ---- */

static void window_encode(uint8_t out[PB_REENROLL_WINDOW_LEN], const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    memset(out, 0, PB_REENROLL_WINDOW_LEN);
    memcpy(&out[0], k_window_magic, sizeof(k_window_magic));
    out[4] = PB_REENROLL_FORMAT_VERSION;
    memcpy(&out[8], eui64, PB_ENROLL_EUI64_LEN);
    put_crc_le(&out[WINDOW_CRC_OFFSET], crc32_ieee(out, WINDOW_CRC_OFFSET));
}

pb_reenroll_rec_status_t pb_reenroll_window_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    uint8_t raw[RECORD_MAX_LEN];
    pb_reenroll_rec_status_t status = read_raw(PB_REENROLL_NVS_KEY_WINDOW, PB_REENROLL_WINDOW_LEN, raw);
    if (status != PB_REENROLL_REC_VALID) {
        return status;
    }
    if (memcmp(&raw[0], k_window_magic, sizeof(k_window_magic)) != 0 || raw[4] != PB_REENROLL_FORMAT_VERSION ||
        !all_zero(&raw[5], 3) || !all_zero(&raw[16], 4) ||
        crc32_ieee(raw, WINDOW_CRC_OFFSET) != get_crc_le(&raw[WINDOW_CRC_OFFSET])) {
        return PB_REENROLL_REC_CORRUPT;
    }
    if (memcmp(&raw[8], eui64, PB_ENROLL_EUI64_LEN) != 0) {
        return PB_REENROLL_REC_FOREIGN;
    }
    return PB_REENROLL_REC_VALID;
}

esp_err_t pb_reenroll_window_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    uint8_t raw[PB_REENROLL_WINDOW_LEN];
    window_encode(raw, eui64);
    esp_err_t err = write_raw(PB_REENROLL_NVS_KEY_WINDOW, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    if (pb_reenroll_window_load(eui64) != PB_REENROLL_REC_VALID) {
        ESP_LOGE(TAG, "window request did not read back");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* erase + commit + read back, for either key. */
static esp_err_t erase_verified(const char *key, size_t expected_len)
{
    uint8_t raw[RECORD_MAX_LEN];
    /* Nothing stored: nothing to write, not even an empty namespace. */
    if (read_raw(key, expected_len, raw) == PB_REENROLL_REC_ABSENT) {
        return ESP_OK;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_REENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: nvs_open for erase failed (%s)", key, esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: erase failed (%s)", key, esp_err_to_name(err));
        return err;
    }
    if (read_raw(key, expected_len, raw) != PB_REENROLL_REC_ABSENT) {
        ESP_LOGE(TAG, "%s is still there after its erase", key);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t pb_reenroll_window_consume(void)
{
    return erase_verified(PB_REENROLL_NVS_KEY_WINDOW, PB_REENROLL_WINDOW_LEN);
}

/* ---- reset journal ---- */

static bool stage_is_known(uint8_t stage)
{
    return stage == PB_REENROLL_STAGE_APPROVED || stage == PB_REENROLL_STAGE_VERIFIED;
}

pb_reenroll_rec_status_t pb_reenroll_journal_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_reenroll_journal_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t raw[RECORD_MAX_LEN];
    pb_reenroll_rec_status_t status = read_raw(PB_REENROLL_NVS_KEY_JOURNAL, PB_REENROLL_JOURNAL_LEN, raw);
    if (status != PB_REENROLL_REC_VALID) {
        return status;
    }
    if (memcmp(&raw[0], k_journal_magic, sizeof(k_journal_magic)) != 0 || raw[4] != PB_REENROLL_FORMAT_VERSION ||
        !stage_is_known(raw[JOURNAL_STAGE_OFFSET]) || !all_zero(&raw[6], 2) || !all_zero(&raw[32], 4) ||
        all_zero(&raw[JOURNAL_OP_OFFSET], PB_REENROLL_OP_ID_LEN) ||
        crc32_ieee(raw, JOURNAL_CRC_OFFSET) != get_crc_le(&raw[JOURNAL_CRC_OFFSET])) {
        ESP_LOGW(TAG, "reset journal: magic/version/stage/CRC check failed");
        return PB_REENROLL_REC_CORRUPT;
    }
    if (memcmp(&raw[8], eui64, PB_ENROLL_EUI64_LEN) != 0) {
        ESP_LOGW(TAG, "reset journal: written for another EUI-64");
        return PB_REENROLL_REC_FOREIGN;
    }
    out->stage = (pb_reenroll_stage_t)raw[JOURNAL_STAGE_OFFSET];
    memcpy(out->op_id, &raw[JOURNAL_OP_OFFSET], PB_REENROLL_OP_ID_LEN);
    return PB_REENROLL_REC_VALID;
}

esp_err_t pb_reenroll_journal_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_reenroll_journal_t *journal)
{
    if (!stage_is_known((uint8_t)journal->stage) || all_zero(journal->op_id, PB_REENROLL_OP_ID_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[PB_REENROLL_JOURNAL_LEN];
    memset(raw, 0, sizeof(raw));
    memcpy(&raw[0], k_journal_magic, sizeof(k_journal_magic));
    raw[4] = PB_REENROLL_FORMAT_VERSION;
    raw[JOURNAL_STAGE_OFFSET] = (uint8_t)journal->stage;
    memcpy(&raw[8], eui64, PB_ENROLL_EUI64_LEN);
    memcpy(&raw[JOURNAL_OP_OFFSET], journal->op_id, PB_REENROLL_OP_ID_LEN);
    put_crc_le(&raw[JOURNAL_CRC_OFFSET], crc32_ieee(raw, JOURNAL_CRC_OFFSET));

    esp_err_t err = write_raw(PB_REENROLL_NVS_KEY_JOURNAL, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    pb_reenroll_journal_t back;
    if (pb_reenroll_journal_load(eui64, &back) != PB_REENROLL_REC_VALID || back.stage != journal->stage ||
        memcmp(back.op_id, journal->op_id, PB_REENROLL_OP_ID_LEN) != 0) {
        ESP_LOGE(TAG, "reset journal did not read back");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t pb_reenroll_journal_discard(void)
{
    return erase_verified(PB_REENROLL_NVS_KEY_JOURNAL, PB_REENROLL_JOURNAL_LEN);
}
