#include "power_base_enrollment/power_base_enrollment.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "pb_enroll";

/* ---- PSKd ---- */

bool pb_enroll_pskd_is_valid(const char *pskd, size_t len)
{
    if (pskd == NULL || len < PB_ENROLL_PSKD_MIN_LEN || len > PB_ENROLL_PSKD_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = pskd[i];
        bool digit = (c >= '0' && c <= '9');
        bool upper = (c >= 'A' && c <= 'Z');
        if (!(digit || upper) || c == 'I' || c == 'O' || c == 'Q' || c == 'Z') {
            return false;
        }
    }
    return true;
}

void pb_enroll_wipe(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (len--) {
        *p++ = 0;
    }
}

/* ---- completion marker ----
 *
 * Layout (PB_ENROLL_MARKER_LEN = 36 bytes, all fields byte arrays):
 *   [0..3]   magic "PBE1"
 *   [4]      version 1
 *   [5..7]   zero
 *   [8..15]  EUI-64 of the board that enrolled
 *   [16..23] Extended PAN ID of the joined network
 *   [24..31] Mesh-Local Prefix (first 8 bytes) of the joined network
 *   [32..35] CRC-32 (IEEE, little-endian) of bytes [0..31]
 */

static const uint8_t k_marker_magic[4] = {'P', 'B', 'E', '1'};
#define MARKER_VERSION 1u
#define MARKER_CRC_OFFSET 32

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

static void marker_encode(uint8_t out[PB_ENROLL_MARKER_LEN], const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                          const pb_enroll_network_id_t *id)
{
    memset(out, 0, PB_ENROLL_MARKER_LEN);
    memcpy(&out[0], k_marker_magic, sizeof(k_marker_magic));
    out[4] = MARKER_VERSION;
    memcpy(&out[8], eui64, PB_ENROLL_EUI64_LEN);
    memcpy(&out[16], id->ext_pan_id, sizeof(id->ext_pan_id));
    memcpy(&out[24], id->mesh_local_prefix, sizeof(id->mesh_local_prefix));
    uint32_t crc = crc32_ieee(out, MARKER_CRC_OFFSET);
    out[32] = (uint8_t)crc;
    out[33] = (uint8_t)(crc >> 8);
    out[34] = (uint8_t)(crc >> 16);
    out[35] = (uint8_t)(crc >> 24);
}

pb_enroll_marker_status_t pb_enroll_marker_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_enroll_network_id_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return PB_ENROLL_MARKER_ABSENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: nvs_open failed (%s)", esp_err_to_name(err));
        return PB_ENROLL_MARKER_READ_ERROR;
    }

    size_t len = 0;
    err = nvs_get_blob(h, PB_ENROLL_NVS_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return PB_ENROLL_MARKER_ABSENT;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "marker: size query failed (%s)", esp_err_to_name(err));
        return PB_ENROLL_MARKER_READ_ERROR;
    }
    if (len != PB_ENROLL_MARKER_LEN) {
        nvs_close(h);
        ESP_LOGW(TAG, "marker: unexpected size %u -- ignored", (unsigned)len);
        return PB_ENROLL_MARKER_CORRUPT;
    }

    uint8_t raw[PB_ENROLL_MARKER_LEN];
    err = nvs_get_blob(h, PB_ENROLL_NVS_KEY, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: read failed (%s)", esp_err_to_name(err));
        return PB_ENROLL_MARKER_READ_ERROR;
    }

    uint32_t stored_crc =
        (uint32_t)raw[32] | ((uint32_t)raw[33] << 8) | ((uint32_t)raw[34] << 16) | ((uint32_t)raw[35] << 24);
    static const uint8_t zero3[3] = {0};
    if (memcmp(&raw[0], k_marker_magic, sizeof(k_marker_magic)) != 0 || raw[4] != MARKER_VERSION ||
        memcmp(&raw[5], zero3, sizeof(zero3)) != 0 || crc32_ieee(raw, MARKER_CRC_OFFSET) != stored_crc) {
        ESP_LOGW(TAG, "marker: magic/version/CRC check failed -- ignored");
        return PB_ENROLL_MARKER_CORRUPT;
    }
    if (memcmp(&raw[8], eui64, PB_ENROLL_EUI64_LEN) != 0) {
        ESP_LOGW(TAG, "marker: written for another EUI-64 -- ignored");
        return PB_ENROLL_MARKER_FOREIGN;
    }
    memcpy(out->ext_pan_id, &raw[16], sizeof(out->ext_pan_id));
    memcpy(out->mesh_local_prefix, &raw[24], sizeof(out->mesh_local_prefix));
    return PB_ENROLL_MARKER_VALID;
}

esp_err_t pb_enroll_marker_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_enroll_network_id_t *id)
{
    uint8_t raw[PB_ENROLL_MARKER_LEN];
    marker_encode(raw, eui64, id);

    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: nvs_open for write failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, PB_ENROLL_NVS_KEY, raw, sizeof(raw));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: write failed (%s)", esp_err_to_name(err));
    }
    return err;
}

esp_err_t pb_enroll_marker_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: nvs_open for erase failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_key(h, PB_ENROLL_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: erase failed (%s)", esp_err_to_name(err));
    }
    return err;
}

/* ---- names ---- */

const char *pb_enroll_result_name(pb_enroll_result_t result)
{
    switch (result) {
    case PB_ENROLL_RESULT_NONE: return "none";
    case PB_ENROLL_RESULT_MARKER_CORRUPT: return "marker_corrupt";
    case PB_ENROLL_RESULT_MARKER_FOREIGN: return "marker_foreign";
    case PB_ENROLL_RESULT_MARKER_READ_ERROR: return "marker_read_error";
    case PB_ENROLL_RESULT_DATASET_MISSING: return "dataset_missing";
    case PB_ENROLL_RESULT_DATASET_MISMATCH: return "dataset_mismatch";
    case PB_ENROLL_RESULT_MARKER_CLEAR_FAILED: return "marker_clear_failed";
    case PB_ENROLL_RESULT_JOINER_START_FAILED: return "joiner_start_failed";
    case PB_ENROLL_RESULT_JOINER_SECURITY: return "joiner_security";
    case PB_ENROLL_RESULT_JOINER_NOT_FOUND: return "joiner_not_found";
    case PB_ENROLL_RESULT_JOINER_TIMEOUT: return "joiner_timeout";
    case PB_ENROLL_RESULT_JOINER_FAILED: return "joiner_failed";
    case PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED: return "joiner_guard_expired";
    case PB_ENROLL_RESULT_CANCELLED: return "cancelled";
    case PB_ENROLL_RESULT_THREAD_START_FAILED: return "thread_start_failed";
    case PB_ENROLL_RESULT_ATTACH_TIMEOUT: return "attach_timeout";
    case PB_ENROLL_RESULT_MARKER_WRITE_FAILED: return "marker_write_failed";
    case PB_ENROLL_RESULT_ENROLLED: return "enrolled";
    case PB_ENROLL_RESULT_RESUMED: return "resumed";
    }
    return "unknown";
}

const char *pb_enroll_state_name(pb_enroll_state_t state)
{
    switch (state) {
    case PB_ENROLL_STATE_WAITING: return "waiting";
    case PB_ENROLL_STATE_JOINING: return "joining";
    case PB_ENROLL_STATE_ATTACHING: return "attaching";
    case PB_ENROLL_STATE_ENROLLED: return "enrolled";
    }
    return "unknown";
}

/* ---- boot rule ---- */

pb_enroll_boot_action_t pb_enroll_decide_boot(pb_enroll_marker_status_t marker, const pb_enroll_network_id_t *marker_id,
                                              const pb_enroll_dataset_info_t *ds, pb_enroll_result_t *out_reason)
{
    pb_enroll_result_t reason;
    switch (marker) {
    case PB_ENROLL_MARKER_VALID:
        if (ds == NULL || !ds->complete) {
            reason = PB_ENROLL_RESULT_DATASET_MISSING;
        } else if (memcmp(&ds->id, marker_id, sizeof(*marker_id)) != 0) {
            reason = PB_ENROLL_RESULT_DATASET_MISMATCH;
        } else {
            reason = PB_ENROLL_RESULT_RESUMED;
        }
        break;
    case PB_ENROLL_MARKER_CORRUPT: reason = PB_ENROLL_RESULT_MARKER_CORRUPT; break;
    case PB_ENROLL_MARKER_FOREIGN: reason = PB_ENROLL_RESULT_MARKER_FOREIGN; break;
    case PB_ENROLL_MARKER_READ_ERROR: reason = PB_ENROLL_RESULT_MARKER_READ_ERROR; break;
    case PB_ENROLL_MARKER_ABSENT:
    default:
        /* Also when a Dataset is stored: without the marker it may be a
         * STANDALONE test network or an interrupted join. */
        reason = PB_ENROLL_RESULT_NONE;
        break;
    }
    if (out_reason != NULL) {
        *out_reason = reason;
    }
    return reason == PB_ENROLL_RESULT_RESUMED ? PB_ENROLL_BOOT_RESUME : PB_ENROLL_BOOT_WAIT;
}

/* ---- controller ---- */

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void back_to_waiting(pb_enroll_ctl_t *c, pb_enroll_result_t why)
{
    c->ops.thread_stop(c->ops.ctx);
    c->state = PB_ENROLL_STATE_WAITING;
    c->last = why;
    c->deadline_ms = 0;
    ESP_LOGW(TAG, "enrollment not completed (%s) -- Thread stopped, waiting for a new attempt",
             pb_enroll_result_name(why));
}

void pb_enroll_ctl_init(pb_enroll_ctl_t *c, const pb_enroll_ops_t *ops, const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    memcpy(c->eui64, eui64, PB_ENROLL_EUI64_LEN);
    c->state = PB_ENROLL_STATE_WAITING;
    c->last = PB_ENROLL_RESULT_NONE;
}

pb_enroll_state_t pb_enroll_ctl_boot(pb_enroll_ctl_t *c)
{
    pb_enroll_network_id_t marker_id;
    pb_enroll_marker_status_t marker = pb_enroll_marker_load(c->eui64, &marker_id);

    pb_enroll_dataset_info_t ds;
    memset(&ds, 0, sizeof(ds));
    bool have_ds = c->ops.read_dataset(&ds, c->ops.ctx);

    pb_enroll_result_t reason;
    pb_enroll_boot_action_t action = pb_enroll_decide_boot(marker, &marker_id, have_ds ? &ds : NULL, &reason);
    if (action == PB_ENROLL_BOOT_WAIT) {
        c->state = PB_ENROLL_STATE_WAITING;
        c->last = reason;
        ESP_LOGI(TAG, "boot: not enrolled (%s) -- Thread stays disabled", pb_enroll_result_name(reason));
        return c->state;
    }

    if (c->ops.thread_start_stored(true, c->ops.ctx) != ESP_OK) {
        /* Nothing was attached or started; leave the marker for a later
         * boot (the fault may be transient) and wait. */
        back_to_waiting(c, PB_ENROLL_RESULT_THREAD_START_FAILED);
        return c->state;
    }
    c->state = PB_ENROLL_STATE_ENROLLED;
    c->resumed = true;
    c->last = PB_ENROLL_RESULT_RESUMED;
    ESP_LOGI(TAG, "boot: enrollment marker and stored Dataset match -- rejoining that network");
    return c->state;
}

esp_err_t pb_enroll_ctl_request_join(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN], uint32_t now_ms)
{
    esp_err_t result;
    size_t len = strnlen(pskd, PB_ENROLL_PSKD_BUF_LEN);

    if (c->state != PB_ENROLL_STATE_WAITING) {
        result = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (len == PB_ENROLL_PSKD_BUF_LEN || !pb_enroll_pskd_is_valid(pskd, len)) {
        result = ESP_ERR_INVALID_ARG;
        goto out;
    }
    /* The old marker must be gone before OpenThread's Dataset changes, so
     * that no interruption from here on can pair an old marker with a new,
     * partial or foreign Dataset. */
    if (pb_enroll_marker_clear() != ESP_OK) {
        c->last = PB_ENROLL_RESULT_MARKER_CLEAR_FAILED;
        result = ESP_FAIL;
        goto out;
    }

    c->attempt++;
    if (c->ops.joiner_start(pskd, c->attempt, c->ops.ctx) != ESP_OK) {
        c->ops.thread_stop(c->ops.ctx);
        c->last = PB_ENROLL_RESULT_JOINER_START_FAILED;
        result = ESP_FAIL;
        goto out;
    }
    c->state = PB_ENROLL_STATE_JOINING;
    c->deadline_ms = now_ms + PB_ENROLL_JOINER_GUARD_MS;
    c->resumed = false;
    ESP_LOGI(TAG, "Joiner started (attempt %u) -- this is not enrollment yet", (unsigned)c->attempt);
    result = ESP_OK;

out:
    pb_enroll_wipe(pskd, PB_ENROLL_PSKD_BUF_LEN);
    return result;
}

esp_err_t pb_enroll_ctl_cancel(pb_enroll_ctl_t *c)
{
    if (c->state == PB_ENROLL_STATE_JOINING) {
        c->ops.joiner_stop(c->ops.ctx);
    } else if (c->state != PB_ENROLL_STATE_ATTACHING) {
        return ESP_ERR_INVALID_STATE;
    }
    back_to_waiting(c, PB_ENROLL_RESULT_CANCELLED);
    return ESP_OK;
}

void pb_enroll_ctl_on_joiner_done(pb_enroll_ctl_t *c, uint32_t attempt, pb_enroll_joiner_outcome_t outcome,
                                  uint32_t now_ms)
{
    if (c->state != PB_ENROLL_STATE_JOINING || attempt != c->attempt) {
        ESP_LOGW(TAG, "ignoring a stale Joiner result (attempt %u, state %s)", (unsigned)attempt,
                 pb_enroll_state_name(c->state));
        return;
    }

    switch (outcome) {
    case PB_ENROLL_JOINER_OK: break;
    case PB_ENROLL_JOINER_ERR_SECURITY: back_to_waiting(c, PB_ENROLL_RESULT_JOINER_SECURITY); return;
    case PB_ENROLL_JOINER_ERR_NOT_FOUND: back_to_waiting(c, PB_ENROLL_RESULT_JOINER_NOT_FOUND); return;
    case PB_ENROLL_JOINER_ERR_TIMEOUT: back_to_waiting(c, PB_ENROLL_RESULT_JOINER_TIMEOUT); return;
    case PB_ENROLL_JOINER_ERR_OTHER:
    default: back_to_waiting(c, PB_ENROLL_RESULT_JOINER_FAILED); return;
    }

    /* Joiner success: OpenThread holds the Network Key, Channel and PAN ID
     * only. Enable Thread on exactly that; never form a network. */
    ESP_LOGI(TAG, "Joiner succeeded -- starting Thread on the joined credentials (not enrolled until attached)");
    if (c->ops.thread_start_stored(false, c->ops.ctx) != ESP_OK) {
        back_to_waiting(c, PB_ENROLL_RESULT_THREAD_START_FAILED);
        return;
    }
    c->state = PB_ENROLL_STATE_ATTACHING;
    c->deadline_ms = now_ms + PB_ENROLL_ATTACH_DEADLINE_MS;
}

void pb_enroll_ctl_tick(pb_enroll_ctl_t *c, bool attached, uint32_t now_ms)
{
    if (c->state == PB_ENROLL_STATE_JOINING) {
        if (time_reached(now_ms, c->deadline_ms)) {
            c->ops.joiner_stop(c->ops.ctx);
            back_to_waiting(c, PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED);
        }
        return;
    }
    if (c->state != PB_ENROLL_STATE_ATTACHING) {
        return;
    }

    if (attached) {
        pb_enroll_dataset_info_t ds;
        memset(&ds, 0, sizeof(ds));
        if (c->ops.read_dataset(&ds, c->ops.ctx) && ds.complete) {
            if (pb_enroll_marker_store(c->eui64, &ds.id) != ESP_OK) {
                /* Real NVS may already hold the new value when only the
                 * commit failed; remove it best-effort so the next boot
                 * does not resume what this boot refused to call enrolled. */
                (void)pb_enroll_marker_clear();
                back_to_waiting(c, PB_ENROLL_RESULT_MARKER_WRITE_FAILED);
                return;
            }
            c->state = PB_ENROLL_STATE_ENROLLED;
            c->last = PB_ENROLL_RESULT_ENROLLED;
            c->deadline_ms = 0;
            ESP_LOGI(TAG, "enrolled: attached with a complete Active Dataset and the marker is committed");
            return;
        }
    }
    if (time_reached(now_ms, c->deadline_ms)) {
        back_to_waiting(c, PB_ENROLL_RESULT_ATTACH_TIMEOUT);
    }
}
