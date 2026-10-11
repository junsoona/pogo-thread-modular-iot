#include "power_base_ble_enroll/pb_ble_admission.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "pb_ble_adm";

/* Domain separation for the request digest (see pb_ble_admission.h). The
 * digest never leaves this file; "v2" is its second layout (message type and
 * bridge endpoint added). */
static const uint8_t k_tag_domain[16] = {'p', 'b', '-', 'b', 'l', 'e', '-', 's', 't', 'a', 'r', 't', '-', 'v', '2', 0};

/* pb-status puts these on the wire as numbers (docs/power-base.md lists
 * them): they must never be renumbered. */
_Static_assert(PB_ENROLL_STATE_WAITING == 0 && PB_ENROLL_STATE_JOINING == 1 && PB_ENROLL_STATE_ATTACHING == 2 &&
                   PB_ENROLL_STATE_ENROLLED == 3,
               "pb_enroll_state_t is part of the BLE wire format");
_Static_assert(PB_ENROLL_RESULT_NONE == 0 && PB_ENROLL_RESULT_MARKER_CORRUPT == 1 &&
                   PB_ENROLL_RESULT_MARKER_FOREIGN == 2 && PB_ENROLL_RESULT_MARKER_READ_ERROR == 3 &&
                   PB_ENROLL_RESULT_DATASET_MISSING == 4 && PB_ENROLL_RESULT_DATASET_MISMATCH == 5 &&
                   PB_ENROLL_RESULT_MARKER_CLEAR_FAILED == 6 && PB_ENROLL_RESULT_JOINER_START_FAILED == 7 &&
                   PB_ENROLL_RESULT_JOINER_SECURITY == 8 && PB_ENROLL_RESULT_JOINER_NOT_FOUND == 9 &&
                   PB_ENROLL_RESULT_JOINER_TIMEOUT == 10 && PB_ENROLL_RESULT_JOINER_FAILED == 11 &&
                   PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED == 12 && PB_ENROLL_RESULT_CANCELLED == 13 &&
                   PB_ENROLL_RESULT_THREAD_START_FAILED == 14 && PB_ENROLL_RESULT_ATTACH_TIMEOUT == 15 &&
                   PB_ENROLL_RESULT_MARKER_WRITE_FAILED == 16 && PB_ENROLL_RESULT_ENROLLED == 17 &&
                   PB_ENROLL_RESULT_RESUMED == 18,
               "pb_enroll_result_t is part of the BLE wire format");
_Static_assert(PB_BLE_START_REQ_MAX_LEN == PB_BLE_START_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN,
               "pb-start maximum length");
_Static_assert(PB_BLE_START_EP_REQ_MAX_LEN == PB_BLE_START_EP_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN,
               "pb-start (bridge endpoint) maximum length");
_Static_assert(PB_BLE_BRIDGE_ADDR_LEN == PB_ENROLL_BRIDGE_ADDR_LEN, "bridge address length");
_Static_assert(PB_BLE_START_MGMT_REQ_MAX_LEN == PB_BLE_START_MGMT_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN,
               "pb-start (management) maximum length");
_Static_assert(PB_BLE_MGMT_HUB_ID_LEN == PB_ENROLL_HUB_ID_LEN && PB_BLE_MGMT_GENERATION_LEN == PB_ENROLL_GENERATION_LEN &&
                   PB_BLE_MGMT_KEY_LEN == PB_ENROLL_MGMT_KEY_LEN && PB_BLE_MGMT_VERSION == PB_ENROLL_MGMT_FORMAT_VERSION,
               "management credential layout");

#define REQ_TARGET_OFFSET 4
#define REQ_ID_OFFSET 12
#define START_CHALLENGE_OFFSET 28
/* PB_BLE_MSG_START_EP_REQ only. */
#define START_EP_VERSION_OFFSET 44
#define START_EP_RESERVED_OFFSET 45
#define START_EP_PORT_OFFSET 46
#define START_EP_ADDR_OFFSET 48
/* version + reserved + port + address, as they are on the wire */
#define START_EP_BLOCK_LEN (PB_BLE_START_EP_REQ_FIXED_LEN - START_EP_VERSION_OFFSET)
/* PB_BLE_MSG_START_MGMT_REQ only; its endpoint block is at the same offsets. */
#define START_MGMT_VERSION_OFFSET 64
#define START_MGMT_RESERVED_OFFSET 65
#define START_MGMT_HUB_ID_OFFSET 66
#define START_MGMT_GENERATION_OFFSET 82
#define START_MGMT_KEY_OFFSET 98
/* version + reserved + hub id + generation + key, as they are on the wire */
#define START_MGMT_BLOCK_LEN (PB_BLE_START_MGMT_REQ_FIXED_LEN - START_MGMT_VERSION_OFFSET)

/* PB_BLE_MSG_LOCAL_RESET_REQ; target and operation id are where the starts
 * have target and request id. */
#define LR_FORMAT_OFFSET 2
#define LR_CHALLENGE_OFFSET 28
#define LR_CONFIRM_OFFSET 44
_Static_assert(PB_BLE_OP_ID_LEN == PB_BLE_REQUEST_ID_LEN, "the operation id sits where the request id does");
_Static_assert(PB_BLE_LOCAL_RESET_REQ_LEN == LR_CONFIRM_OFFSET + 4, "local reset request length");

/* ---- small helpers ---- */

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

/* Comparison whose duration does not depend on where the bytes differ. */
static bool ct_equal(const uint8_t *x, const uint8_t *y, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0;
}

static bool is_zero(const uint8_t *p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void drop_challenge(pb_ble_adm_t *a)
{
    a->ch_valid = false;
    a->ch_epoch = 0;
    a->ch_issued_ms = 0;
    pb_enroll_wipe(a->ch, sizeof(a->ch));
}

/* The PSKd slot and the bridge endpoint that travels with it. */
static void drop_pskd(pb_ble_adm_t *a)
{
    a->pskd_pending = false;
    pb_enroll_wipe(a->pskd, sizeof(a->pskd));
    a->endpoint_pending = false;
    pb_enroll_wipe(&a->endpoint, sizeof(a->endpoint));
    a->mgmt_pending = false;
    pb_enroll_wipe(&a->mgmt, sizeof(a->mgmt));
}

static void drop_tag(pb_ble_adm_t *a)
{
    pb_enroll_wipe(a->rec_tag, sizeof(a->rec_tag));
}

static bool rec_active(const pb_ble_adm_t *a)
{
    return a->rec_phase == PB_BLE_REC_QUEUED || a->rec_phase == PB_BLE_REC_STARTING ||
           a->rec_phase == PB_BLE_REC_RUNNING;
}

static bool lr_active(const pb_ble_adm_t *a)
{
    return a->lr_phase == PB_BLE_LR_REC_QUEUED || a->lr_phase == PB_BLE_LR_REC_RUNNING;
}

static uint8_t wire_phase(pb_ble_rec_phase_t phase)
{
    switch (phase) {
    case PB_BLE_REC_QUEUED:
    case PB_BLE_REC_STARTING: return PB_BLE_PHASE_QUEUED;
    case PB_BLE_REC_RUNNING: return PB_BLE_PHASE_RUNNING;
    case PB_BLE_REC_FAILED: return PB_BLE_PHASE_FAILED;
    case PB_BLE_REC_ENROLLED: return PB_BLE_PHASE_ENROLLED;
    case PB_BLE_REC_CANCELLED: return PB_BLE_PHASE_CANCELLED;
    case PB_BLE_REC_NONE:
    default: return PB_BLE_PHASE_NONE;
    }
}

static bool q_push(pb_ble_adm_t *a, pb_ble_cmd_kind_t kind, uint32_t gen)
{
    if (a->q_count >= PB_BLE_CMD_QUEUE_LEN) {
        return false;
    }
    uint8_t idx = (uint8_t)((a->q_head + a->q_count) % PB_BLE_CMD_QUEUE_LEN);
    a->q[idx].kind = (uint8_t)kind;
    a->q[idx].gen = gen;
    a->q_count++;
    return true;
}

static bool q_pop(pb_ble_adm_t *a, pb_ble_cmd_t *out)
{
    if (a->q_count == 0) {
        return false;
    }
    *out = a->q[a->q_head];
    a->q_head = (uint8_t)((a->q_head + 1) % PB_BLE_CMD_QUEUE_LEN);
    a->q_count--;
    return true;
}

static size_t short_resp(uint8_t out[PB_BLE_SHORT_RESP_LEN], uint8_t type, uint8_t code)
{
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = type;
    out[2] = code;
    out[3] = 0;
    return PB_BLE_SHORT_RESP_LEN;
}

/* SHA-256(domain | message type | target | challenge | pskd_len | pskd |
 * endpoint block | management block). The endpoint block is bytes [44..63]
 * of a PB_BLE_MSG_START_EP_REQ or PB_BLE_MSG_START_MGMT_REQ and absent for
 * PB_BLE_MSG_START_REQ; the management block is bytes [64..129] of a
 * PB_BLE_MSG_START_MGMT_REQ. So the same request id with another message
 * type, address, port, generation or key never has the digest of the
 * request that was accepted. */
static bool request_tag(const pb_ble_adm_t *a, const uint8_t *req, size_t pskd_offset, size_t pskd_len,
                        uint8_t out[PB_BLE_SHA256_LEN])
{
    uint8_t buf[sizeof(k_tag_domain) + 1 + PB_BLE_EUI64_LEN + PB_BLE_CHALLENGE_LEN + 1 + PB_ENROLL_PSKD_MAX_LEN +
                START_EP_BLOCK_LEN + START_MGMT_BLOCK_LEN];
    size_t n = 0;
    memcpy(&buf[n], k_tag_domain, sizeof(k_tag_domain));
    n += sizeof(k_tag_domain);
    buf[n++] = req[1];
    memcpy(&buf[n], &req[REQ_TARGET_OFFSET], PB_BLE_EUI64_LEN);
    n += PB_BLE_EUI64_LEN;
    memcpy(&buf[n], &req[START_CHALLENGE_OFFSET], PB_BLE_CHALLENGE_LEN);
    n += PB_BLE_CHALLENGE_LEN;
    buf[n++] = (uint8_t)pskd_len;
    memcpy(&buf[n], &req[pskd_offset], pskd_len);
    n += pskd_len;
    if (req[1] == PB_BLE_MSG_START_EP_REQ || req[1] == PB_BLE_MSG_START_MGMT_REQ) {
        memcpy(&buf[n], &req[START_EP_VERSION_OFFSET], START_EP_BLOCK_LEN);
        n += START_EP_BLOCK_LEN;
    }
    if (req[1] == PB_BLE_MSG_START_MGMT_REQ) {
        memcpy(&buf[n], &req[START_MGMT_VERSION_OFFSET], START_MGMT_BLOCK_LEN);
        n += START_MGMT_BLOCK_LEN;
    }
    bool ok = a->ops.sha256(buf, n, out, a->ops.ctx);
    pb_enroll_wipe(buf, sizeof(buf));
    return ok;
}

const char *pb_ble_close_reason_name(pb_ble_close_reason_t reason)
{
    switch (reason) {
    case PB_BLE_OPEN: return "open";
    case PB_BLE_CLOSE_ENROLLED: return "enrolled";
    case PB_BLE_CLOSE_AUTH_LOCKED: return "auth_locked";
    case PB_BLE_CLOSE_WINDOW_EXPIRED: return "window_expired";
    case PB_BLE_CLOSE_LOCAL_RESET_DONE: return "local_reset_done";
    }
    return "unknown";
}

void pb_ble_adm_init(pb_ble_adm_t *a, const pb_ble_adm_ops_t *ops, const uint8_t eui64[PB_BLE_EUI64_LEN],
                     bool joiner_eui64_ok, uint32_t now_ms)
{
    memset(a, 0, sizeof(*a));
    a->ops = *ops;
    memcpy(a->eui64, eui64, PB_BLE_EUI64_LEN);
    a->joiner_eui64_ok = joiner_eui64_ok;
    a->closed = PB_BLE_OPEN;
    a->window_deadline_ms = now_ms + PB_BLE_ADV_WINDOW_MS;
    a->rec_phase = PB_BLE_REC_NONE;
    a->ctl.state = PB_ENROLL_STATE_WAITING;
    a->ctl.last = PB_ENROLL_RESULT_NONE;
}

void pb_ble_adm_set_endpoint_policy(pb_ble_adm_t *a, pb_enroll_endpoint_policy_t policy)
{
    a->endpoint_policy = policy;
}

void pb_ble_adm_set_local_reset(pb_ble_adm_t *a, bool supported, bool window, uint32_t now_ms)
{
    a->lr_supported = supported;
    a->lr_window = supported && window;
    a->lr_refresh = supported;
    if (a->lr_window) {
        /* Counted from here, whatever connects or authenticates meanwhile. */
        a->window_deadline_ms = now_ms + PB_BLE_REENROLL_WINDOW_MS;
    }
}

bool pb_ble_adm_attempt_active(const pb_ble_adm_t *a)
{
    return rec_active(a);
}

bool pb_ble_adm_local_reset_active(const pb_ble_adm_t *a)
{
    return lr_active(a);
}

void pb_ble_adm_wipe(pb_ble_adm_t *a)
{
    drop_challenge(a);
    drop_pskd(a);
    drop_tag(a);
}

/* ---- BLE side ---- */

void pb_ble_adm_session_changed(pb_ble_adm_t *a)
{
    a->epoch++;
    drop_challenge(a);
}

bool pb_ble_adm_sec_request_begin(pb_ble_adm_t *a)
{
    if (a->closed != PB_BLE_OPEN || a->auth_locked) {
        return false;
    }
    /* Any handshake message replaces or advances the session: a challenge
     * from before it must not survive. */
    a->epoch++;
    drop_challenge(a);
    if (++a->sec_requests > PB_BLE_MAX_SEC_REQUESTS) {
        a->auth_locked = true;
        ESP_LOGW(TAG, "more than %u Security 2 handshake messages this boot -- BLE enrollment closes",
                 (unsigned)PB_BLE_MAX_SEC_REQUESTS);
        return false;
    }
    return true;
}

void pb_ble_adm_sec_request_end(pb_ble_adm_t *a, bool ok)
{
    if (ok) {
        return;
    }
    a->auth_failures++;
    ESP_LOGW(TAG, "Security 2 handshake message refused (%u of %u before BLE enrollment closes)",
             (unsigned)a->auth_failures, (unsigned)PB_BLE_MAX_AUTH_FAILURES);
    if (a->auth_failures >= PB_BLE_MAX_AUTH_FAILURES) {
        a->auth_locked = true;
    }
}

size_t pb_ble_adm_handle_status(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint8_t out[PB_BLE_STATUS_RESP_LEN],
                                uint32_t now_ms)
{
    memset(out, 0, PB_BLE_STATUS_RESP_LEN);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_STATUS_RESP;

    if (in == NULL || inlen < 1) {
        out[2] = PB_BLE_CODE_MALFORMED;
        return PB_BLE_STATUS_RESP_LEN;
    }
    if (in[0] != PB_BLE_PROTO_VERSION) {
        out[2] = PB_BLE_CODE_UNSUPPORTED_VERSION;
        return PB_BLE_STATUS_RESP_LEN;
    }
    if (inlen != PB_BLE_STATUS_REQ_LEN || in[1] != PB_BLE_MSG_STATUS_REQ ||
        (in[2] & (uint8_t)~PB_BLE_STATUS_REQ_F_ISSUE_CHALLENGE) != 0 || in[3] != 0) {
        out[2] = PB_BLE_CODE_MALFORMED;
        return PB_BLE_STATUS_RESP_LEN;
    }

    uint8_t flags = 0;
    memcpy(&out[4], a->eui64, PB_BLE_EUI64_LEN);
    out[12] = (uint8_t)a->ctl.state;
    out[13] = (uint8_t)a->ctl.last;
    put_u32(&out[20], a->ctl.attempt);
    if (rec_active(a)) {
        flags |= PB_BLE_STATUS_F_ATTEMPT_ACTIVE;
    }
    if (!a->joiner_eui64_ok) {
        flags |= PB_BLE_STATUS_F_JOINER_UNAVAILABLE;
    }
    const uint8_t *asked = &in[4];
    if (!is_zero(asked, PB_BLE_REQUEST_ID_LEN) && a->rec_phase != PB_BLE_REC_NONE &&
        ct_equal(asked, a->rec_request_id, PB_BLE_REQUEST_ID_LEN)) {
        flags |= PB_BLE_STATUS_F_REQUEST_MATCH;
        out[15] = wire_phase(a->rec_phase);
        out[16] = a->rec_result;
    }

    uint8_t code = PB_BLE_CODE_OK;
    if (a->closed != PB_BLE_OPEN) {
        code = PB_BLE_CODE_CLOSED;
    } else if ((in[2] & PB_BLE_STATUS_REQ_F_ISSUE_CHALLENGE) != 0) {
        drop_challenge(a);
        if (!a->ops.random(a->ch, sizeof(a->ch), a->ops.ctx)) {
            drop_challenge(a);
            code = PB_BLE_CODE_INTERNAL;
        } else {
            a->ch_valid = true;
            a->ch_epoch = a->epoch;
            a->ch_issued_ms = now_ms;
            flags |= PB_BLE_STATUS_F_CHALLENGE_ISSUED;
            put_u16(&out[18], (uint16_t)(PB_BLE_CHALLENGE_TTL_MS / 1000u));
            memcpy(&out[24], a->ch, PB_BLE_CHALLENGE_LEN);
        }
    }
    out[2] = code;
    out[14] = flags;
    return PB_BLE_STATUS_RESP_LEN;
}

bool pb_ble_adm_is_receipt_request(const uint8_t *in, size_t inlen)
{
    return in != NULL && inlen >= 2 && in[0] == PB_BLE_PROTO_VERSION && in[1] == PB_BLE_MSG_RECEIPT_REQ;
}

size_t pb_ble_adm_handle_receipt(const uint8_t eui64[PB_BLE_EUI64_LEN], const uint8_t *in, size_t inlen,
                                 uint8_t out[PB_BLE_RECEIPT_RESP_LEN], pb_ble_receipt_source_t source, void *ctx)
{
    memset(out, 0, PB_BLE_RECEIPT_RESP_LEN);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_RECEIPT_RESP;
    out[12] = PB_BLE_RECEIPT_VERSION;

    if (in == NULL || inlen < 1) {
        out[2] = PB_BLE_CODE_MALFORMED;
        return PB_BLE_RECEIPT_RESP_LEN;
    }
    if (in[0] != PB_BLE_PROTO_VERSION) {
        out[2] = PB_BLE_CODE_UNSUPPORTED_VERSION;
        return PB_BLE_RECEIPT_RESP_LEN;
    }
    if (inlen != PB_BLE_RECEIPT_REQ_LEN || in[1] != PB_BLE_MSG_RECEIPT_REQ || in[2] != 0 || in[3] != 0) {
        out[2] = PB_BLE_CODE_MALFORMED;
        return PB_BLE_RECEIPT_RESP_LEN;
    }
    memcpy(&out[4], eui64, PB_BLE_EUI64_LEN);
    if (!ct_equal(&in[4], eui64, PB_BLE_EUI64_LEN)) {
        out[2] = PB_BLE_CODE_WRONG_TARGET;
        return PB_BLE_RECEIPT_RESP_LEN;
    }
    out[2] = PB_BLE_CODE_OK;
    if (source != NULL && source(&out[16], ctx)) {
        out[13] = 1;
    } else {
        /* Whatever a failed source left behind is not a receipt. */
        memset(&out[16], 0, PB_BLE_RECEIPT_LEN);
    }
    return PB_BLE_RECEIPT_RESP_LEN;
}

static uint8_t start_admit(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint32_t now_ms)
{
    if (in == NULL || inlen < 1) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (in[0] != PB_BLE_PROTO_VERSION) {
        return PB_BLE_CODE_UNSUPPORTED_VERSION;
    }
    if (inlen < 2) {
        return PB_BLE_CODE_MALFORMED;
    }
    /* Three messages share this endpoint; the type byte names the layout. */
    const bool with_mgmt = (in[1] == PB_BLE_MSG_START_MGMT_REQ);
    const bool with_endpoint = with_mgmt || (in[1] == PB_BLE_MSG_START_EP_REQ);
    if (!with_endpoint && in[1] != PB_BLE_MSG_START_REQ) {
        return PB_BLE_CODE_MALFORMED;
    }
    const size_t fixed_len = with_mgmt ? PB_BLE_START_MGMT_REQ_FIXED_LEN
                                       : (with_endpoint ? PB_BLE_START_EP_REQ_FIXED_LEN : PB_BLE_START_REQ_FIXED_LEN);
    if (inlen < fixed_len || inlen > fixed_len + PB_ENROLL_PSKD_MAX_LEN || in[3] != 0) {
        return PB_BLE_CODE_MALFORMED;
    }
    size_t pskd_len = in[2];
    if (inlen != fixed_len + pskd_len) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (with_endpoint && in[START_EP_RESERVED_OFFSET] != 0) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (with_mgmt && in[START_MGMT_RESERVED_OFFSET] != 0) {
        return PB_BLE_CODE_MALFORMED;
    }
    const uint8_t *request_id = &in[REQ_ID_OFFSET];
    if (is_zero(request_id, PB_BLE_REQUEST_ID_LEN)) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (a->closed != PB_BLE_OPEN) {
        return PB_BLE_CODE_CLOSED;
    }
    /* A re-enrollment window boot: the old enrollment is only not resumed,
     * it is still there, and after a reset the board restarts before it
     * joins anything. No start is taken in such a boot; nothing is queued
     * and the challenge is not used up. */
    if (a->lr_window) {
        return PB_BLE_CODE_NOT_WAITING;
    }
    /* The prepared image's endpoint policy: a start that must not enroll this
     * board is turned away here -- before the digest, the request-id lookup
     * and the challenge, so it leaves no trace: no record, nothing queued,
     * the session's challenge still usable for the start that is allowed. */
    if (a->endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_INVALID ||
        (a->endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_REQUIRED && !with_endpoint)) {
        return PB_BLE_CODE_UNAVAILABLE;
    }

    uint8_t tag[PB_BLE_SHA256_LEN];
    if (!request_tag(a, in, fixed_len, pskd_len, tag)) {
        pb_enroll_wipe(tag, sizeof(tag));
        return PB_BLE_CODE_INTERNAL;
    }

    /* A request id this board already knows: either the same request again
     * (answered without starting anything) or a different one reusing the
     * id (refused). The attempt in progress is not touched either way. The
     * digest covers the message type and the bridge endpoint, so the same id
     * with another address or port is a conflict, never a duplicate. */
    if (a->rec_phase != PB_BLE_REC_NONE && ct_equal(request_id, a->rec_request_id, PB_BLE_REQUEST_ID_LEN)) {
        bool same = rec_active(a) && ct_equal(tag, a->rec_tag, sizeof(tag));
        pb_enroll_wipe(tag, sizeof(tag));
        return same ? PB_BLE_CODE_DUPLICATE : PB_BLE_CODE_REQUEST_CONFLICT;
    }

    /* A new request: whatever happens next, this session's challenge is
     * spent. */
    bool ch_present = a->ch_valid && a->ch_epoch == a->epoch;
    bool ch_match = ch_present && ct_equal(&in[START_CHALLENGE_OFFSET], a->ch, PB_BLE_CHALLENGE_LEN);
    bool ch_expired = ch_present && time_reached(now_ms, a->ch_issued_ms + PB_BLE_CHALLENGE_TTL_MS);
    drop_challenge(a);

    /* The endpoint as this request states it; used only if every check
     * below passes. */
    pb_enroll_bridge_endpoint_t endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    bool endpoint_ok = true;
    if (with_endpoint) {
        memcpy(endpoint.addr, &in[START_EP_ADDR_OFFSET], PB_BLE_BRIDGE_ADDR_LEN);
        endpoint.port = (uint16_t)(((uint16_t)in[START_EP_PORT_OFFSET] << 8) | in[START_EP_PORT_OFFSET + 1]);
        endpoint_ok = in[START_EP_VERSION_OFFSET] == PB_BLE_BRIDGE_ENDPOINT_VERSION &&
                      pb_enroll_bridge_endpoint_check(&endpoint) == PB_ENROLL_ENDPOINT_OK;
    }
    /* The credential as this request states it; a secret, wiped below. */
    pb_enroll_mgmt_credential_t mgmt;
    memset(&mgmt, 0, sizeof(mgmt));
    bool mgmt_ok = true;
    if (with_mgmt) {
        memcpy(mgmt.hub_id, &in[START_MGMT_HUB_ID_OFFSET], PB_BLE_MGMT_HUB_ID_LEN);
        memcpy(mgmt.generation, &in[START_MGMT_GENERATION_OFFSET], PB_BLE_MGMT_GENERATION_LEN);
        memcpy(mgmt.key, &in[START_MGMT_KEY_OFFSET], PB_BLE_MGMT_KEY_LEN);
        mgmt_ok = in[START_MGMT_VERSION_OFFSET] == PB_BLE_MGMT_VERSION && pb_enroll_mgmt_credential_is_valid(&mgmt);
    }

    uint8_t code;
    if (!ct_equal(&in[REQ_TARGET_OFFSET], a->eui64, PB_BLE_EUI64_LEN)) {
        code = PB_BLE_CODE_WRONG_TARGET;
    } else if (!ch_match) {
        code = PB_BLE_CODE_CHALLENGE_INVALID;
    } else if (ch_expired) {
        code = PB_BLE_CODE_CHALLENGE_EXPIRED;
    } else if (!pb_enroll_pskd_is_valid((const char *)&in[fixed_len], pskd_len)) {
        code = PB_BLE_CODE_INVALID_PSKD;
    } else if (!endpoint_ok) {
        code = PB_BLE_CODE_INVALID_ENDPOINT;
    } else if (!mgmt_ok) {
        code = PB_BLE_CODE_INVALID_MANAGEMENT;
    } else if (!a->joiner_eui64_ok) {
        code = PB_BLE_CODE_UNAVAILABLE;
    } else if (rec_active(a)) {
        code = PB_BLE_CODE_BUSY;
    } else if (a->ctl.state != PB_ENROLL_STATE_WAITING) {
        code = PB_BLE_CODE_NOT_WAITING;
    } else if (a->q_count >= PB_BLE_CMD_QUEUE_LEN) {
        code = PB_BLE_CODE_BUSY;
    } else {
        a->rec_gen++;
        a->rec_phase = PB_BLE_REC_QUEUED;
        a->rec_ctl_attempt = 0;
        a->rec_result = 0;
        a->rec_cancel_requested = false;
        memcpy(a->rec_request_id, request_id, PB_BLE_REQUEST_ID_LEN);
        memcpy(a->rec_tag, tag, sizeof(tag));
        drop_pskd(a); /* also clears an endpoint left from an earlier attempt */
        memcpy(a->pskd, &in[fixed_len], pskd_len);
        a->pskd_pending = true;
        if (with_endpoint) {
            a->endpoint = endpoint;
            a->endpoint_pending = true;
        }
        if (with_mgmt) {
            a->mgmt = mgmt;
            a->mgmt_pending = true;
        }
        (void)q_push(a, PB_BLE_CMD_START, a->rec_gen); /* space was checked above */
        code = PB_BLE_CODE_ACCEPTED;
    }
    pb_enroll_wipe(tag, sizeof(tag));
    pb_enroll_wipe(&endpoint, sizeof(endpoint));
    pb_enroll_wipe(&mgmt, sizeof(mgmt));
    return code;
}

/* ---- local reset ---- */

static bool window_open(const pb_ble_adm_t *a, uint32_t now_ms)
{
    return a->lr_window && a->closed == PB_BLE_OPEN && !time_reached(now_ms, a->window_deadline_ms);
}

bool pb_ble_adm_is_local_reset_status_request(const pb_ble_adm_t *a, const uint8_t *in, size_t inlen)
{
    return a->lr_supported && in != NULL && inlen >= 2 && in[0] == PB_BLE_PROTO_VERSION &&
           in[1] == PB_BLE_MSG_LOCAL_RESET_STATUS_REQ;
}

size_t pb_ble_adm_handle_local_reset_status(pb_ble_adm_t *a, const uint8_t *in, size_t inlen,
                                            uint8_t out[PB_BLE_LOCAL_RESET_STATUS_RESP_LEN], uint32_t now_ms)
{
    memset(out, 0, PB_BLE_LOCAL_RESET_STATUS_RESP_LEN);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_LOCAL_RESET_STATUS_RESP;

    if (in == NULL || inlen < 1) {
        out[2] = PB_BLE_CODE_MALFORMED;
        return PB_BLE_LOCAL_RESET_STATUS_RESP_LEN;
    }
    if (in[0] != PB_BLE_PROTO_VERSION) {
        out[2] = PB_BLE_CODE_UNSUPPORTED_VERSION;
        return PB_BLE_LOCAL_RESET_STATUS_RESP_LEN;
    }
    if (inlen != PB_BLE_LOCAL_RESET_STATUS_REQ_LEN || in[1] != PB_BLE_MSG_LOCAL_RESET_STATUS_REQ || in[2] != 0 ||
        in[3] != 0) {
        out[2] = PB_BLE_CODE_MALFORMED;
        return PB_BLE_LOCAL_RESET_STATUS_RESP_LEN;
    }
    memcpy(&out[4], a->eui64, PB_BLE_EUI64_LEN);
    if (!ct_equal(&in[REQ_TARGET_OFFSET], a->eui64, PB_BLE_EUI64_LEN)) {
        out[2] = PB_BLE_CODE_WRONG_TARGET;
        return PB_BLE_LOCAL_RESET_STATUS_RESP_LEN;
    }
    out[2] = PB_BLE_CODE_OK;
    out[12] = PB_BLE_LOCAL_RESET_VERSION;
    /* A reset the worker has not got to yet is "pending" already: nothing
     * new may start beside it. */
    out[13] = lr_active(a) ? PB_BLE_LR_COND_RESET_PENDING : a->lr_snap.condition;

    uint8_t flags = 0;
    if (window_open(a, now_ms)) {
        flags |= PB_BLE_LR_F_WINDOW_OPEN;
        /* Rounded up: a window with time left never reads as zero. */
        const uint32_t left_ms = a->window_deadline_ms - now_ms;
        put_u16(&out[18], (uint16_t)((left_ms + 999u) / 1000u));
    }
    const uint8_t *asked = &in[REQ_ID_OFFSET];
    if (!is_zero(asked, PB_BLE_OP_ID_LEN)) {
        const bool ram_match = a->lr_phase != PB_BLE_LR_REC_NONE && ct_equal(asked, a->lr_op, PB_BLE_OP_ID_LEN);
        const bool journal_match =
            a->lr_snap.journal_valid && ct_equal(asked, a->lr_snap.journal_op, PB_BLE_OP_ID_LEN);
        if (ram_match && lr_active(a)) {
            flags |= PB_BLE_LR_F_OP_MATCH;
            out[15] = a->lr_phase == PB_BLE_LR_REC_QUEUED ? PB_BLE_LR_PHASE_QUEUED : PB_BLE_LR_PHASE_IN_PROGRESS;
        } else if (journal_match) {
            /* What is in flash says how far the operation got. */
            flags |= PB_BLE_LR_F_OP_MATCH;
            out[15] = a->lr_snap.journal_verified ? PB_BLE_LR_PHASE_VERIFIED : PB_BLE_LR_PHASE_IN_PROGRESS;
            out[16] = (ram_match && !a->lr_snap.journal_verified) ? a->lr_reason : PB_BLE_LR_REASON_NONE;
        } else if (ram_match && a->lr_phase == PB_BLE_LR_REC_NOT_STARTED) {
            flags |= PB_BLE_LR_F_OP_MATCH;
            out[15] = PB_BLE_LR_PHASE_NOT_STARTED;
            out[16] = a->lr_reason;
        } else if (ram_match && a->lr_phase == PB_BLE_LR_REC_ENDED) {
            /* Run, and the journal has not been read back yet. */
            flags |= PB_BLE_LR_F_OP_MATCH;
            out[15] = PB_BLE_LR_PHASE_IN_PROGRESS;
            out[16] = a->lr_reason;
        }
    }
    out[14] = flags;
    return PB_BLE_LOCAL_RESET_STATUS_RESP_LEN;
}

static uint8_t local_reset_admit(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint32_t now_ms)
{
    /* The caller has checked version and type. */
    if (inlen != PB_BLE_LOCAL_RESET_REQ_LEN || in[LR_FORMAT_OFFSET] != PB_BLE_LOCAL_RESET_VERSION || in[3] != 0 ||
        in[LR_CONFIRM_OFFSET] != PB_BLE_LOCAL_RESET_CONFIRM_ERASE || in[LR_CONFIRM_OFFSET + 1] != 0 ||
        in[LR_CONFIRM_OFFSET + 2] != 0 || in[LR_CONFIRM_OFFSET + 3] != 0) {
        /* Also a request without the confirmation: that is not a reset
         * request. */
        return PB_BLE_CODE_MALFORMED;
    }
    const uint8_t *op_id = &in[REQ_ID_OFFSET];
    if (is_zero(op_id, PB_BLE_OP_ID_LEN)) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (a->closed != PB_BLE_OPEN) {
        return PB_BLE_CODE_CLOSED;
    }
    if (!ct_equal(&in[REQ_TARGET_OFFSET], a->eui64, PB_BLE_EUI64_LEN)) {
        return PB_BLE_CODE_WRONG_TARGET;
    }

    /* An operation this board already knows: answered without starting
     * anything and without using up the challenge. A second operation never
     * replaces the first. */
    const bool same_ram = a->lr_phase != PB_BLE_LR_REC_NONE && ct_equal(op_id, a->lr_op, PB_BLE_OP_ID_LEN);
    const bool same_journal = a->lr_snap.journal_valid && ct_equal(op_id, a->lr_snap.journal_op, PB_BLE_OP_ID_LEN);
    if (lr_active(a)) {
        return same_ram ? PB_BLE_CODE_DUPLICATE : PB_BLE_CODE_BUSY;
    }
    if (a->lr_phase == PB_BLE_LR_REC_ENDED) {
        /* Run in this boot; the board restarts next. Nothing else starts. */
        return same_ram ? PB_BLE_CODE_DUPLICATE : PB_BLE_CODE_BUSY;
    }
    if (a->lr_snap.journal_valid && !a->lr_snap.journal_verified) {
        return same_journal ? PB_BLE_CODE_DUPLICATE : PB_BLE_CODE_BUSY;
    }
    if (same_journal) {
        /* Verified. "The same request again" only while the board is still
         * what that reset left; once it was enrolled again the id is spent. */
        return a->lr_snap.condition == PB_BLE_LR_COND_NOT_ENROLLED ? PB_BLE_CODE_DUPLICATE : PB_BLE_CODE_REQUEST_CONFLICT;
    }
    if (same_ram) {
        return PB_BLE_CODE_REQUEST_CONFLICT; /* over: it never started, or its journal is gone */
    }

    /* The physical window: opened by the button in this boot, and not run
     * out. */
    if (!window_open(a, now_ms)) {
        return PB_BLE_CODE_WINDOW_CLOSED;
    }

    /* A new operation: whatever happens next, this session's challenge is
     * spent. */
    bool ch_present = a->ch_valid && a->ch_epoch == a->epoch;
    bool ch_match = ch_present && ct_equal(&in[LR_CHALLENGE_OFFSET], a->ch, PB_BLE_CHALLENGE_LEN);
    bool ch_expired = ch_present && time_reached(now_ms, a->ch_issued_ms + PB_BLE_CHALLENGE_TTL_MS);
    drop_challenge(a);
    if (!ch_match) {
        return PB_BLE_CODE_CHALLENGE_INVALID;
    }
    if (ch_expired) {
        return PB_BLE_CODE_CHALLENGE_EXPIRED;
    }

    switch (a->lr_snap.condition) {
    case PB_BLE_LR_COND_ENROLLED:
    case PB_BLE_LR_COND_UNREADABLE: /* this operation replaces what could not be read */
        break;
    case PB_BLE_LR_COND_NOT_ENROLLED:
        return PB_BLE_CODE_NOT_ENROLLED;
    case PB_BLE_LR_COND_REMOTE_RELEASE_PENDING: /* it comes first */
    case PB_BLE_LR_COND_RESET_PENDING:
    default:
        return PB_BLE_CODE_BUSY;
    }
    if (rec_active(a) || a->q_count >= PB_BLE_CMD_QUEUE_LEN) {
        return PB_BLE_CODE_BUSY;
    }

    a->lr_gen++;
    a->lr_phase = PB_BLE_LR_REC_QUEUED;
    a->lr_reason = PB_BLE_LR_REASON_NONE;
    memcpy(a->lr_op, op_id, PB_BLE_OP_ID_LEN);
    (void)q_push(a, PB_BLE_CMD_LOCAL_RESET, a->lr_gen); /* space was checked above */
    return PB_BLE_CODE_ACCEPTED;
}

size_t pb_ble_adm_handle_start(pb_ble_adm_t *a, uint8_t *in, size_t inlen, uint8_t out[PB_BLE_SHORT_RESP_LEN],
                               uint32_t now_ms)
{
    /* The local reset command, on a board that has it. On any other board
     * its type is unknown and is answered below like every unknown type. */
    if (a->lr_supported && in != NULL && inlen >= 2 && in[0] == PB_BLE_PROTO_VERSION &&
        in[1] == PB_BLE_MSG_LOCAL_RESET_REQ) {
        uint8_t lr_code = local_reset_admit(a, in, inlen, now_ms);
        pb_enroll_wipe(in, inlen);
        if (lr_code == PB_BLE_CODE_ACCEPTED) {
            ESP_LOGW(TAG, "local reset accepted (generation %u) and queued for the worker -- nothing is erased yet",
                     (unsigned)a->lr_gen);
        } else if (lr_code != PB_BLE_CODE_DUPLICATE) {
            ESP_LOGW(TAG, "local reset refused (code 0x%02x) -- nothing changed", (unsigned)lr_code);
        }
        return short_resp(out, PB_BLE_MSG_LOCAL_RESET_RESP, lr_code);
    }
    /* The answer's type follows the message that was sent, when that much of
     * it is this protocol version's; anything else is answered as pb-start. */
    const bool this_version = in != NULL && inlen >= 2 && in[0] == PB_BLE_PROTO_VERSION;
    const bool ep_message = this_version && in[1] == PB_BLE_MSG_START_EP_REQ;
    const bool mgmt_message = this_version && in[1] == PB_BLE_MSG_START_MGMT_REQ;
    uint8_t code = start_admit(a, in, inlen, now_ms);
    if (in != NULL) {
        pb_enroll_wipe(in, inlen);
    }
    if (code == PB_BLE_CODE_ACCEPTED) {
        ESP_LOGI(TAG, "start accepted (generation %u, %s) and queued for the enrollment worker -- not a Joiner result",
                 (unsigned)a->rec_gen,
                 mgmt_message ? "with a bridge endpoint and a management credential"
                              : (ep_message ? "with a bridge endpoint" : "no bridge endpoint"));
    } else if (code != PB_BLE_CODE_DUPLICATE) {
        ESP_LOGW(TAG, "start refused (code 0x%02x) -- no Joiner started", (unsigned)code);
    }
    return short_resp(out,
                      mgmt_message ? PB_BLE_MSG_START_MGMT_RESP
                                   : (ep_message ? PB_BLE_MSG_START_EP_RESP : PB_BLE_MSG_START_RESP),
                      code);
}

static uint8_t cancel_admit(pb_ble_adm_t *a, const uint8_t *in, size_t inlen)
{
    if (in == NULL || inlen < 1) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (in[0] != PB_BLE_PROTO_VERSION) {
        return PB_BLE_CODE_UNSUPPORTED_VERSION;
    }
    if (inlen != PB_BLE_CANCEL_REQ_LEN || in[1] != PB_BLE_MSG_CANCEL_REQ || in[2] != 0 || in[3] != 0) {
        return PB_BLE_CODE_MALFORMED;
    }
    const uint8_t *request_id = &in[REQ_ID_OFFSET];
    if (is_zero(request_id, PB_BLE_REQUEST_ID_LEN)) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (a->closed != PB_BLE_OPEN) {
        return PB_BLE_CODE_CLOSED;
    }
    if (!ct_equal(&in[REQ_TARGET_OFFSET], a->eui64, PB_BLE_EUI64_LEN)) {
        return PB_BLE_CODE_WRONG_TARGET;
    }
    /* Only the request id of the attempt names it; a client that did not
     * start the attempt does not know that id (pb-status never reveals it). */
    if (a->rec_phase == PB_BLE_REC_NONE || !ct_equal(request_id, a->rec_request_id, PB_BLE_REQUEST_ID_LEN)) {
        return PB_BLE_CODE_NO_SUCH_ATTEMPT;
    }
    switch (a->rec_phase) {
    case PB_BLE_REC_QUEUED:
        /* The worker has not seen it yet: nothing was started. Its START
         * command stays in the queue and is dropped as stale. */
        drop_pskd(a);
        drop_tag(a);
        a->rec_phase = PB_BLE_REC_CANCELLED;
        a->rec_result = (uint8_t)PB_ENROLL_RESULT_CANCELLED;
        return PB_BLE_CODE_OK;
    case PB_BLE_REC_STARTING:
    case PB_BLE_REC_RUNNING:
        if (a->rec_cancel_requested) {
            return PB_BLE_CODE_OK;
        }
        if (!q_push(a, PB_BLE_CMD_CANCEL, a->rec_gen)) {
            return PB_BLE_CODE_BUSY;
        }
        a->rec_cancel_requested = true;
        return PB_BLE_CODE_OK;
    case PB_BLE_REC_CANCELLED:
        return PB_BLE_CODE_OK;
    case PB_BLE_REC_FAILED:
    case PB_BLE_REC_ENROLLED:
    case PB_BLE_REC_NONE:
    default:
        /* Finished. In particular an enrolled board is never "cancelled":
         * its marker and its Thread link stay. */
        return PB_BLE_CODE_NO_SUCH_ATTEMPT;
    }
}

size_t pb_ble_adm_handle_cancel(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint8_t out[PB_BLE_SHORT_RESP_LEN])
{
    return short_resp(out, PB_BLE_MSG_CANCEL_RESP, cancel_admit(a, in, inlen));
}

/* ---- worker side ---- */

static void w_lock(const pb_ble_worker_ops_t *w)
{
    if (w->lock != NULL) {
        w->lock(w->ctx);
    }
}

static void w_unlock(const pb_ble_worker_ops_t *w)
{
    if (w->unlock != NULL) {
        w->unlock(w->ctx);
    }
}

/* Caller holds the lock. */
static void publish(pb_ble_adm_t *a, const pb_ble_ctl_status_t *st)
{
    if (a->lr_supported && st->state != a->ctl.state) {
        a->lr_refresh = true; /* enrolled, or no longer: the record changed */
    }
    a->ctl = *st;
    if (a->rec_phase != PB_BLE_REC_RUNNING) {
        return;
    }
    if (st->attempt != a->rec_ctl_attempt) {
        a->rec_phase = PB_BLE_REC_FAILED;
        a->rec_result = PB_BLE_REQ_RESULT_SUPERSEDED;
    } else if (st->state == PB_ENROLL_STATE_WAITING) {
        a->rec_phase = (st->last == PB_ENROLL_RESULT_CANCELLED) ? PB_BLE_REC_CANCELLED : PB_BLE_REC_FAILED;
        a->rec_result = (uint8_t)st->last;
    } else if (st->state == PB_ENROLL_STATE_ENROLLED) {
        a->rec_phase = PB_BLE_REC_ENROLLED;
        a->rec_result = (uint8_t)st->last;
    } else {
        return; /* joining / attaching: still running */
    }
    drop_tag(a);
}

/* Caller holds the lock. */
static pb_ble_close_reason_t apply_close_rules(pb_ble_adm_t *a, uint32_t now_ms)
{
    if (a->closed != PB_BLE_OPEN) {
        return a->closed;
    }
    pb_ble_close_reason_t reason = PB_BLE_OPEN;
    if (a->lr_window) {
        /* A window boot. The board being enrolled is why it is open, not a
         * reason to close it. A reset that was accepted is carried to its
         * end whatever the clock says; after it, a short time to read the
         * result and then the restart. */
        if (a->lr_ended) {
            if (time_reached(now_ms, a->lr_end_deadline_ms)) {
                reason = PB_BLE_CLOSE_LOCAL_RESET_DONE;
            }
        } else if (lr_active(a)) {
            reason = PB_BLE_OPEN;
        } else if (a->auth_locked) {
            reason = PB_BLE_CLOSE_AUTH_LOCKED;
        } else if (time_reached(now_ms, a->window_deadline_ms)) {
            reason = PB_BLE_CLOSE_WINDOW_EXPIRED;
        }
    } else if (a->ctl.state == PB_ENROLL_STATE_ENROLLED) {
        reason = PB_BLE_CLOSE_ENROLLED;
    } else if (a->auth_locked) {
        reason = PB_BLE_CLOSE_AUTH_LOCKED;
    } else if (time_reached(now_ms, a->window_deadline_ms) && !rec_active(a)) {
        reason = PB_BLE_CLOSE_WINDOW_EXPIRED;
    }
    if (reason != PB_BLE_OPEN) {
        a->closed = reason;
        drop_challenge(a);
        ESP_LOGW(TAG, "BLE enrollment closes for this boot (%s)", pb_ble_close_reason_name(reason));
    }
    return reason;
}

pb_ble_close_reason_t pb_ble_adm_worker_poll(pb_ble_adm_t *a, const pb_ble_worker_ops_t *w, uint32_t now_ms)
{
    pb_ble_ctl_status_t st;

    for (int pass = 0; pass < PB_BLE_CMD_QUEUE_LEN; pass++) {
        pb_ble_cmd_t cmd = {0};
        char pskd[PB_ENROLL_PSKD_BUF_LEN] = {0};
        pb_enroll_bridge_endpoint_t endpoint;
        pb_enroll_mgmt_credential_t mgmt;
        bool have_endpoint = false;
        bool have_mgmt = false;
        bool run_start = false;
        bool run_cancel = false;
        bool run_reset = false;
        uint8_t reset_op[PB_BLE_OP_ID_LEN] = {0};
        memset(&endpoint, 0, sizeof(endpoint));
        memset(&mgmt, 0, sizeof(mgmt));

        w_lock(w);
        bool have = q_pop(a, &cmd);
        if (have && cmd.kind == PB_BLE_CMD_LOCAL_RESET) {
            if (cmd.gen == a->lr_gen && a->lr_phase == PB_BLE_LR_REC_QUEUED && w->local_reset != NULL) {
                memcpy(reset_op, a->lr_op, sizeof(reset_op));
                a->lr_phase = PB_BLE_LR_REC_RUNNING;
                run_reset = true;
            }
        } else if (have && cmd.gen == a->rec_gen) {
            if (cmd.kind == PB_BLE_CMD_START && a->rec_phase == PB_BLE_REC_QUEUED && a->pskd_pending) {
                memcpy(pskd, a->pskd, sizeof(pskd));
                /* The checked copy the BLE callback left; from here on only
                 * the controller holds it. */
                have_endpoint = a->endpoint_pending;
                endpoint = a->endpoint;
                have_mgmt = a->mgmt_pending;
                mgmt = a->mgmt;
                drop_pskd(a);
                a->rec_phase = PB_BLE_REC_STARTING;
                run_start = true;
            } else if (cmd.kind == PB_BLE_CMD_CANCEL && a->rec_phase == PB_BLE_REC_RUNNING) {
                run_cancel = true;
            }
        }
        /* Anything else is a command for an attempt that was cancelled,
         * finished or replaced since it was queued: dropped. */
        w_unlock(w);
        if (!have) {
            break;
        }

        if (run_start) {
            esp_err_t err =
                w->request_join(pskd, have_endpoint ? &endpoint : NULL, have_mgmt ? &mgmt : NULL, w->ctx);
            pb_enroll_wipe(pskd, sizeof(pskd));
            pb_enroll_wipe(&endpoint, sizeof(endpoint));
            pb_enroll_wipe(&mgmt, sizeof(mgmt));
            memset(&st, 0, sizeof(st));
            w->get_status(&st, w->ctx);
            w_lock(w);
            if (a->rec_gen == cmd.gen && a->rec_phase == PB_BLE_REC_STARTING) {
                if (err == ESP_OK) {
                    a->rec_phase = PB_BLE_REC_RUNNING;
                    a->rec_ctl_attempt = st.attempt;
                } else {
                    a->rec_phase = PB_BLE_REC_FAILED;
                    a->rec_result = (err == ESP_FAIL) ? (uint8_t)st.last : PB_BLE_REQ_RESULT_START_REFUSED;
                    drop_tag(a);
                }
            }
            w_unlock(w);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "queued start did not reach the Joiner (%s)", esp_err_to_name(err));
            }
        } else if (run_cancel) {
            /* The controller refuses unless it is joining or attaching, so
             * an attempt that enrolled in the meantime is left alone. Its
             * outcome is read back through the status below. */
            (void)w->cancel(w->ctx);
        } else if (run_reset) {
            /* Journal, erase, verify: flash work, with the lock released. */
            uint8_t reason = PB_BLE_LR_REASON_NONE;
            uint8_t result = w->local_reset(reset_op, &reason, w->ctx);
            w_lock(w);
            if (a->lr_gen == cmd.gen && a->lr_phase == PB_BLE_LR_REC_RUNNING) {
                a->lr_reason = reason;
                if (result == PB_BLE_LR_RESULT_NOT_STARTED) {
                    a->lr_phase = PB_BLE_LR_REC_NOT_STARTED;
                } else {
                    a->lr_phase = PB_BLE_LR_REC_ENDED;
                    a->lr_ended = true;
                    a->lr_end_deadline_ms = now_ms + PB_BLE_LR_RESULT_GRACE_MS;
                }
                a->lr_refresh = true;
            }
            w_unlock(w);
            ESP_LOGW(TAG, "local reset run by the worker: result %u, reason %u", (unsigned)result, (unsigned)reason);
        }
    }

    memset(&st, 0, sizeof(st));
    w->get_status(&st, w->ctx);
    w_lock(w);
    publish(a, &st);
    const bool refresh = a->lr_supported && a->lr_refresh && w->get_local_reset != NULL;
    if (refresh) {
        a->lr_refresh = false;
    }
    w_unlock(w);
    if (refresh) {
        /* Reads flash: outside the lock, like the reset itself. */
        pb_ble_lr_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        w->get_local_reset(&snap, w->ctx);
        w_lock(w);
        a->lr_snap = snap;
        w_unlock(w);
    }
    w_lock(w);
    pb_ble_close_reason_t reason = apply_close_rules(a, now_ms);
    w_unlock(w);
    return reason;
}
