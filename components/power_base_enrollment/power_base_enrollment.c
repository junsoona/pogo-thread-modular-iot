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

/* ---- bridge endpoint ---- */

pb_enroll_endpoint_check_t pb_enroll_bridge_endpoint_check(const pb_enroll_bridge_endpoint_t *ep)
{
    const uint8_t *a = ep->addr;
    bool first80_zero = true;
    for (int i = 0; i < 10; i++) {
        if (a[i] != 0) {
            first80_zero = false;
        }
    }
    if (first80_zero) {
        bool last48_zero = (a[10] | a[11] | a[12] | a[13] | a[14] | a[15]) == 0;
        if (last48_zero) {
            return PB_ENROLL_ENDPOINT_UNSPECIFIED;
        }
        if (a[10] == 0 && a[11] == 0 && a[12] == 0 && a[13] == 0 && a[14] == 0 && a[15] == 1) {
            return PB_ENROLL_ENDPOINT_LOOPBACK;
        }
        if ((a[10] == 0xff && a[11] == 0xff) || (a[10] == 0 && a[11] == 0)) {
            return PB_ENROLL_ENDPOINT_IPV4_EMBEDDED;
        }
    }
    if (a[0] == 0xff) {
        return PB_ENROLL_ENDPOINT_MULTICAST;
    }
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) {
        return PB_ENROLL_ENDPOINT_LINK_LOCAL;
    }
    if (a[0] != 0xfd) {
        return PB_ENROLL_ENDPOINT_NOT_MESH_LOCAL;
    }
    /* Thread RLOCs and ALOCs are <Mesh-Local Prefix>:0000:00ff:fe00:<locator>. */
    if (a[8] == 0 && a[9] == 0 && a[10] == 0 && a[11] == 0xff && a[12] == 0xfe && a[13] == 0) {
        return PB_ENROLL_ENDPOINT_LOCATOR;
    }
    if (ep->port == 0) {
        return PB_ENROLL_ENDPOINT_PORT_ZERO;
    }
    return PB_ENROLL_ENDPOINT_OK;
}

const char *pb_enroll_endpoint_check_name(pb_enroll_endpoint_check_t check)
{
    switch (check) {
    case PB_ENROLL_ENDPOINT_OK: return "ok";
    case PB_ENROLL_ENDPOINT_UNSPECIFIED: return "unspecified";
    case PB_ENROLL_ENDPOINT_LOOPBACK: return "loopback";
    case PB_ENROLL_ENDPOINT_IPV4_EMBEDDED: return "ipv4_embedded";
    case PB_ENROLL_ENDPOINT_MULTICAST: return "multicast";
    case PB_ENROLL_ENDPOINT_LINK_LOCAL: return "link_local";
    case PB_ENROLL_ENDPOINT_NOT_MESH_LOCAL: return "not_mesh_local";
    case PB_ENROLL_ENDPOINT_LOCATOR: return "locator";
    case PB_ENROLL_ENDPOINT_PORT_ZERO: return "port_zero";
    }
    return "unknown";
}

bool pb_enroll_bridge_endpoint_in_network(const pb_enroll_bridge_endpoint_t *ep, const pb_enroll_network_id_t *id)
{
    return memcmp(ep->addr, id->mesh_local_prefix, sizeof(id->mesh_local_prefix)) == 0;
}

/* ---- completion record ----
 *
 * Version 1 (PB_ENROLL_MARKER_LEN = 36 bytes, all fields byte arrays):
 *   [0..3]   magic "PBE1"
 *   [4]      version 1
 *   [5..7]   zero
 *   [8..15]  EUI-64 of the board that enrolled
 *   [16..23] Extended PAN ID of the joined network
 *   [24..31] Mesh-Local Prefix (first 8 bytes) of the joined network
 *   [32..35] CRC-32 (IEEE, little-endian) of bytes [0..31]
 *
 * Version 2 (PB_ENROLL_MARKER_V2_LEN = 56 bytes):
 *   [0..3]   magic "PBE1"
 *   [4]      version 2
 *   [5]      flags: 0x01 = bridge endpoint present (always set); others zero
 *   [6..7]   zero
 *   [8..31]  as version 1
 *   [32..47] bridge IPv6 address (network byte order)
 *   [48..49] bridge port, big-endian
 *   [50..51] zero
 *   [52..55] CRC-32 (IEEE, little-endian) of bytes [0..51]
 */

static const uint8_t k_marker_magic[4] = {'P', 'B', 'E', '1'};
#define MARKER_VERSION 1u
#define MARKER_CRC_OFFSET 32
#define MARKER_V2_VERSION 2u
#define MARKER_V2_FLAG_ENDPOINT 0x01u
#define MARKER_V2_ADDR_OFFSET 32
#define MARKER_V2_PORT_OFFSET 48
#define MARKER_V2_CRC_OFFSET 52

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

static void marker_encode(uint8_t out[PB_ENROLL_MARKER_LEN], const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                          const pb_enroll_network_id_t *id)
{
    memset(out, 0, PB_ENROLL_MARKER_LEN);
    memcpy(&out[0], k_marker_magic, sizeof(k_marker_magic));
    out[4] = MARKER_VERSION;
    memcpy(&out[8], eui64, PB_ENROLL_EUI64_LEN);
    memcpy(&out[16], id->ext_pan_id, sizeof(id->ext_pan_id));
    memcpy(&out[24], id->mesh_local_prefix, sizeof(id->mesh_local_prefix));
    put_crc_le(&out[MARKER_CRC_OFFSET], crc32_ieee(out, MARKER_CRC_OFFSET));
}

static void record_v2_encode(uint8_t out[PB_ENROLL_MARKER_V2_LEN], const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                             const pb_enroll_record_t *rec)
{
    memset(out, 0, PB_ENROLL_MARKER_V2_LEN);
    memcpy(&out[0], k_marker_magic, sizeof(k_marker_magic));
    out[4] = MARKER_V2_VERSION;
    out[5] = MARKER_V2_FLAG_ENDPOINT;
    memcpy(&out[8], eui64, PB_ENROLL_EUI64_LEN);
    memcpy(&out[16], rec->id.ext_pan_id, sizeof(rec->id.ext_pan_id));
    memcpy(&out[24], rec->id.mesh_local_prefix, sizeof(rec->id.mesh_local_prefix));
    memcpy(&out[MARKER_V2_ADDR_OFFSET], rec->bridge_endpoint.addr, PB_ENROLL_BRIDGE_ADDR_LEN);
    out[MARKER_V2_PORT_OFFSET] = (uint8_t)(rec->bridge_endpoint.port >> 8);
    out[MARKER_V2_PORT_OFFSET + 1] = (uint8_t)rec->bridge_endpoint.port;
    put_crc_le(&out[MARKER_V2_CRC_OFFSET], crc32_ieee(out, MARKER_V2_CRC_OFFSET));
}

static bool record_equal(const pb_enroll_record_t *x, const pb_enroll_record_t *y)
{
    return memcmp(x->id.ext_pan_id, y->id.ext_pan_id, sizeof(x->id.ext_pan_id)) == 0 &&
           memcmp(x->id.mesh_local_prefix, y->id.mesh_local_prefix, sizeof(x->id.mesh_local_prefix)) == 0 &&
           x->has_bridge_endpoint == y->has_bridge_endpoint &&
           memcmp(x->bridge_endpoint.addr, y->bridge_endpoint.addr, PB_ENROLL_BRIDGE_ADDR_LEN) == 0 &&
           x->bridge_endpoint.port == y->bridge_endpoint.port;
}

/* True when the endpoint may be stored with / used for the network `id`. */
static bool endpoint_fits_network(const pb_enroll_bridge_endpoint_t *ep, const pb_enroll_network_id_t *id)
{
    return pb_enroll_bridge_endpoint_check(ep) == PB_ENROLL_ENDPOINT_OK && pb_enroll_bridge_endpoint_in_network(ep, id);
}

pb_enroll_marker_status_t pb_enroll_record_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_enroll_record_t *out)
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
    if (len != PB_ENROLL_MARKER_LEN && len != PB_ENROLL_MARKER_V2_LEN) {
        nvs_close(h);
        ESP_LOGW(TAG, "marker: unexpected size %u -- ignored", (unsigned)len);
        return PB_ENROLL_MARKER_CORRUPT;
    }

    uint8_t raw[PB_ENROLL_MARKER_V2_LEN];
    const size_t expected = len;
    err = nvs_get_blob(h, PB_ENROLL_NVS_KEY, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: read failed (%s)", esp_err_to_name(err));
        return PB_ENROLL_MARKER_READ_ERROR;
    }
    if (len != expected) {
        ESP_LOGW(TAG, "marker: size changed while reading -- ignored");
        return PB_ENROLL_MARKER_CORRUPT;
    }

    /* The size names the version; the version byte must agree with it. */
    const bool v2 = (len == PB_ENROLL_MARKER_V2_LEN);
    const size_t crc_offset = v2 ? MARKER_V2_CRC_OFFSET : MARKER_CRC_OFFSET;
    static const uint8_t zero3[3] = {0};
    bool header_ok = memcmp(&raw[0], k_marker_magic, sizeof(k_marker_magic)) == 0;
    if (v2) {
        header_ok = header_ok && raw[4] == MARKER_V2_VERSION && raw[5] == MARKER_V2_FLAG_ENDPOINT && raw[6] == 0 &&
                    raw[7] == 0 && raw[50] == 0 && raw[51] == 0;
    } else {
        header_ok = header_ok && raw[4] == MARKER_VERSION && memcmp(&raw[5], zero3, sizeof(zero3)) == 0;
    }
    if (!header_ok || crc32_ieee(raw, crc_offset) != get_crc_le(&raw[crc_offset])) {
        ESP_LOGW(TAG, "marker: magic/version/CRC check failed -- ignored");
        return PB_ENROLL_MARKER_CORRUPT;
    }
    if (memcmp(&raw[8], eui64, PB_ENROLL_EUI64_LEN) != 0) {
        ESP_LOGW(TAG, "marker: written for another EUI-64 -- ignored");
        return PB_ENROLL_MARKER_FOREIGN;
    }

    pb_enroll_record_t rec;
    memset(&rec, 0, sizeof(rec));
    memcpy(rec.id.ext_pan_id, &raw[16], sizeof(rec.id.ext_pan_id));
    memcpy(rec.id.mesh_local_prefix, &raw[24], sizeof(rec.id.mesh_local_prefix));
    if (v2) {
        rec.has_bridge_endpoint = true;
        memcpy(rec.bridge_endpoint.addr, &raw[MARKER_V2_ADDR_OFFSET], PB_ENROLL_BRIDGE_ADDR_LEN);
        rec.bridge_endpoint.port = (uint16_t)(((uint16_t)raw[MARKER_V2_PORT_OFFSET] << 8) | raw[MARKER_V2_PORT_OFFSET + 1]);
        if (!endpoint_fits_network(&rec.bridge_endpoint, &rec.id)) {
            /* Intact bytes that this firmware would never have written. The
             * record is not half-trusted: no marker, no endpoint. */
            const char *why = pb_enroll_endpoint_check_name(pb_enroll_bridge_endpoint_check(&rec.bridge_endpoint));
            ESP_LOGW(TAG, "marker: stored bridge endpoint is not usable for the stored network (%s) -- ignored", why);
            return PB_ENROLL_MARKER_CORRUPT;
        }
    }
    *out = rec;
    return PB_ENROLL_MARKER_VALID;
}

pb_enroll_marker_status_t pb_enroll_marker_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_enroll_network_id_t *out)
{
    pb_enroll_record_t rec;
    pb_enroll_marker_status_t status = pb_enroll_record_load(eui64, &rec);
    *out = rec.id; /* zero unless VALID */
    return status;
}

esp_err_t pb_enroll_record_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_enroll_record_t *rec)
{
    uint8_t raw[PB_ENROLL_MARKER_V2_LEN];
    size_t raw_len;
    if (rec->has_bridge_endpoint) {
        if (!endpoint_fits_network(&rec->bridge_endpoint, &rec->id)) {
            ESP_LOGE(TAG, "marker: refusing to store a bridge endpoint that is not usable for the joined network");
            return ESP_ERR_INVALID_ARG;
        }
        record_v2_encode(raw, eui64, rec);
        raw_len = PB_ENROLL_MARKER_V2_LEN;
    } else {
        marker_encode(raw, eui64, &rec->id);
        raw_len = PB_ENROLL_MARKER_LEN;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: nvs_open for write failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, PB_ENROLL_NVS_KEY, raw, raw_len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marker: write failed (%s)", esp_err_to_name(err));
    }
    return err;
}

esp_err_t pb_enroll_marker_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_enroll_network_id_t *id)
{
    pb_enroll_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = *id;
    return pb_enroll_record_store(eui64, &rec);
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

const char *pb_enroll_endpoint_policy_name(pb_enroll_endpoint_policy_t policy)
{
    switch (policy) {
    case PB_ENROLL_ENDPOINT_POLICY_OPTIONAL: return "optional";
    case PB_ENROLL_ENDPOINT_POLICY_REQUIRED: return "required";
    case PB_ENROLL_ENDPOINT_POLICY_INVALID: return "invalid";
    }
    return "invalid";
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

/* The endpoint of an attempt that did not (or not yet) enroll never outlives
 * that attempt. */
static void drop_pending_endpoint(pb_enroll_ctl_t *c)
{
    c->pending_endpoint = false;
    memset(&c->pending_ep, 0, sizeof(c->pending_ep));
}

static void back_to_waiting(pb_enroll_ctl_t *c, pb_enroll_result_t why)
{
    c->ops.thread_stop(c->ops.ctx);
    drop_pending_endpoint(c);
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
    c->endpoint_policy = PB_ENROLL_ENDPOINT_POLICY_OPTIONAL;
}

void pb_enroll_ctl_set_endpoint_policy(pb_enroll_ctl_t *c, pb_enroll_endpoint_policy_t policy)
{
    c->endpoint_policy = policy;
}

pb_enroll_state_t pb_enroll_ctl_boot(pb_enroll_ctl_t *c)
{
    pb_enroll_record_t rec;
    pb_enroll_marker_status_t marker = pb_enroll_record_load(c->eui64, &rec);
    const pb_enroll_network_id_t marker_id = rec.id;

    if (marker == PB_ENROLL_MARKER_VALID && !rec.has_bridge_endpoint &&
        c->endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_REQUIRED) {
        /* An enrollment that delivered no endpoint, on a board whose image
         * has no address and requires one: resuming would start the services
         * with nowhere to send to. Thread is not touched, and nothing is
         * erased here -- the next accepted request clears this record. */
        c->state = PB_ENROLL_STATE_WAITING;
        c->last = PB_ENROLL_RESULT_MARKER_CORRUPT;
        ESP_LOGW(TAG, "boot: the enrollment record is version 1 (no bridge endpoint) but this board's image requires "
                      "a delivered endpoint -- not resumed, Thread stays disabled (enroll again with the endpoint)");
        return c->state;
    }

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
    /* A version 2 record: its endpoint passed the same checks as when it was
     * stored, and lies in the Mesh-Local Prefix the stored Dataset also has. */
    c->has_bridge_endpoint = rec.has_bridge_endpoint;
    c->bridge_endpoint = rec.bridge_endpoint;
    ESP_LOGI(TAG, "boot: enrollment marker and stored Dataset match -- rejoining that network (%s)",
             rec.has_bridge_endpoint ? "record v2: bridge endpoint restored" : "record v1: no delivered bridge endpoint");
    return c->state;
}

static esp_err_t request_join(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN],
                              const pb_enroll_bridge_endpoint_t *endpoint, uint32_t now_ms)
{
    esp_err_t result;
    size_t len = strnlen(pskd, PB_ENROLL_PSKD_BUF_LEN);

    if (c->state != PB_ENROLL_STATE_WAITING) {
        result = ESP_ERR_INVALID_STATE;
        goto out;
    }
    /* The image's endpoint policy, before anything else is looked at and
     * long before the old record is erased or a Joiner starts. It holds for
     * every caller of this controller: BLE worker, bench console, anyone. */
    if (c->endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_INVALID ||
        (c->endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_REQUIRED && endpoint == NULL)) {
        ESP_LOGW(TAG, "Joiner request refused: endpoint policy '%s' and the request %s -- nothing started, nothing "
                      "erased",
                 pb_enroll_endpoint_policy_name(c->endpoint_policy),
                 endpoint != NULL ? "carries an endpoint" : "carries no bridge endpoint");
        result = ESP_ERR_NOT_ALLOWED;
        goto out;
    }
    if (len == PB_ENROLL_PSKD_BUF_LEN || !pb_enroll_pskd_is_valid(pskd, len)) {
        result = ESP_ERR_INVALID_ARG;
        goto out;
    }
    if (endpoint != NULL && pb_enroll_bridge_endpoint_check(endpoint) != PB_ENROLL_ENDPOINT_OK) {
        result = ESP_ERR_INVALID_ARG;
        goto out;
    }
    /* A new attempt never inherits an earlier attempt's endpoint. */
    drop_pending_endpoint(c);
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
    if (endpoint != NULL) {
        c->pending_endpoint = true;
        c->pending_ep = *endpoint;
    }
    ESP_LOGI(TAG, "Joiner started (attempt %u, %s) -- this is not enrollment yet", (unsigned)c->attempt,
             endpoint != NULL ? "bridge endpoint held in RAM" : "no bridge endpoint delivered");
    result = ESP_OK;

out:
    pb_enroll_wipe(pskd, PB_ENROLL_PSKD_BUF_LEN);
    return result;
}

esp_err_t pb_enroll_ctl_request_join(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN], uint32_t now_ms)
{
    return request_join(c, pskd, NULL, now_ms);
}

esp_err_t pb_enroll_ctl_request_join_with_endpoint(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN],
                                                   const pb_enroll_bridge_endpoint_t *endpoint, uint32_t now_ms)
{
    if (endpoint == NULL) {
        pb_enroll_wipe(pskd, PB_ENROLL_PSKD_BUF_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    return request_join(c, pskd, endpoint, now_ms);
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
            pb_enroll_record_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.id = ds.id;
            if (c->pending_endpoint) {
                /* The endpoint must be an address of the network this board
                 * really joined. Otherwise nothing is stored: no marker, so
                 * the Dataset OpenThread now holds is never resumed. */
                if (!pb_enroll_bridge_endpoint_in_network(&c->pending_ep, &ds.id)) {
                    ESP_LOGE(TAG, "the delivered bridge endpoint is not inside the joined network's Mesh-Local "
                                  "Prefix -- not enrolled");
                    back_to_waiting(c, PB_ENROLL_RESULT_DATASET_MISMATCH);
                    return;
                }
                rec.has_bridge_endpoint = true;
                rec.bridge_endpoint = c->pending_ep;
            } else if (c->endpoint_policy != PB_ENROLL_ENDPOINT_POLICY_OPTIONAL) {
                /* Not reachable through request_join(), which refuses such a
                 * request; kept so that no path writes a version 1 record on
                 * a board that must not have one. */
                ESP_LOGE(TAG, "attached without a delivered bridge endpoint although the endpoint policy is '%s' -- "
                              "not enrolled",
                         pb_enroll_endpoint_policy_name(c->endpoint_policy));
                back_to_waiting(c, PB_ENROLL_RESULT_MARKER_WRITE_FAILED);
                return;
            }
            esp_err_t stored = pb_enroll_record_store(c->eui64, &rec);
            if (stored == ESP_OK && rec.has_bridge_endpoint) {
                /* The exporter will send to what is in flash from the next
                 * boot on: read it back before calling this enrolled. */
                pb_enroll_record_t back;
                if (pb_enroll_record_load(c->eui64, &back) != PB_ENROLL_MARKER_VALID || !record_equal(&back, &rec)) {
                    stored = ESP_FAIL;
                }
            }
            if (stored != ESP_OK) {
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
            c->has_bridge_endpoint = rec.has_bridge_endpoint;
            c->bridge_endpoint = rec.bridge_endpoint;
            drop_pending_endpoint(c);
            ESP_LOGI(TAG, "enrolled: attached with a complete Active Dataset and the record is committed (%s)",
                     rec.has_bridge_endpoint ? "v2, with the delivered bridge endpoint" : "v1, no bridge endpoint");
            return;
        }
    }
    if (time_reached(now_ms, c->deadline_ms)) {
        back_to_waiting(c, PB_ENROLL_RESULT_ATTACH_TIMEOUT);
    }
}
