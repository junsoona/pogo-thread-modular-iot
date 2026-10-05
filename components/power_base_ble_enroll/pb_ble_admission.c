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
 * endpoint block). The endpoint block is bytes [44..63] of a
 * PB_BLE_MSG_START_EP_REQ and absent for PB_BLE_MSG_START_REQ, so the same
 * request id with another message type, address or port never has the
 * digest of the request that was accepted. */
static bool request_tag(const pb_ble_adm_t *a, const uint8_t *req, size_t pskd_offset, size_t pskd_len,
                        uint8_t out[PB_BLE_SHA256_LEN])
{
    uint8_t buf[sizeof(k_tag_domain) + 1 + PB_BLE_EUI64_LEN + PB_BLE_CHALLENGE_LEN + 1 + PB_ENROLL_PSKD_MAX_LEN +
                START_EP_BLOCK_LEN];
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
    if (req[1] == PB_BLE_MSG_START_EP_REQ) {
        memcpy(&buf[n], &req[START_EP_VERSION_OFFSET], START_EP_BLOCK_LEN);
        n += START_EP_BLOCK_LEN;
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

bool pb_ble_adm_attempt_active(const pb_ble_adm_t *a)
{
    return rec_active(a);
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
    /* Two messages share this endpoint; the type byte names the layout. */
    const bool with_endpoint = (in[1] == PB_BLE_MSG_START_EP_REQ);
    if (!with_endpoint && in[1] != PB_BLE_MSG_START_REQ) {
        return PB_BLE_CODE_MALFORMED;
    }
    const size_t fixed_len = with_endpoint ? PB_BLE_START_EP_REQ_FIXED_LEN : PB_BLE_START_REQ_FIXED_LEN;
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
    const uint8_t *request_id = &in[REQ_ID_OFFSET];
    if (is_zero(request_id, PB_BLE_REQUEST_ID_LEN)) {
        return PB_BLE_CODE_MALFORMED;
    }
    if (a->closed != PB_BLE_OPEN) {
        return PB_BLE_CODE_CLOSED;
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
        (void)q_push(a, PB_BLE_CMD_START, a->rec_gen); /* space was checked above */
        code = PB_BLE_CODE_ACCEPTED;
    }
    pb_enroll_wipe(tag, sizeof(tag));
    pb_enroll_wipe(&endpoint, sizeof(endpoint));
    return code;
}

size_t pb_ble_adm_handle_start(pb_ble_adm_t *a, uint8_t *in, size_t inlen, uint8_t out[PB_BLE_SHORT_RESP_LEN],
                               uint32_t now_ms)
{
    /* The answer's type follows the message that was sent, when that much of
     * it is this protocol version's; anything else is answered as pb-start. */
    const bool ep_message =
        in != NULL && inlen >= 2 && in[0] == PB_BLE_PROTO_VERSION && in[1] == PB_BLE_MSG_START_EP_REQ;
    uint8_t code = start_admit(a, in, inlen, now_ms);
    if (in != NULL) {
        pb_enroll_wipe(in, inlen);
    }
    if (code == PB_BLE_CODE_ACCEPTED) {
        ESP_LOGI(TAG, "start accepted (generation %u, %s) and queued for the enrollment worker -- not a Joiner result",
                 (unsigned)a->rec_gen, ep_message ? "with a bridge endpoint" : "no bridge endpoint");
    } else if (code != PB_BLE_CODE_DUPLICATE) {
        ESP_LOGW(TAG, "start refused (code 0x%02x) -- no Joiner started", (unsigned)code);
    }
    return short_resp(out, ep_message ? PB_BLE_MSG_START_EP_RESP : PB_BLE_MSG_START_RESP, code);
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
    if (a->ctl.state == PB_ENROLL_STATE_ENROLLED) {
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
        bool have_endpoint = false;
        bool run_start = false;
        bool run_cancel = false;
        memset(&endpoint, 0, sizeof(endpoint));

        w_lock(w);
        bool have = q_pop(a, &cmd);
        if (have && cmd.gen == a->rec_gen) {
            if (cmd.kind == PB_BLE_CMD_START && a->rec_phase == PB_BLE_REC_QUEUED && a->pskd_pending) {
                memcpy(pskd, a->pskd, sizeof(pskd));
                /* The checked copy the BLE callback left; from here on only
                 * the controller holds it. */
                have_endpoint = a->endpoint_pending;
                endpoint = a->endpoint;
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
            esp_err_t err = w->request_join(pskd, have_endpoint ? &endpoint : NULL, w->ctx);
            pb_enroll_wipe(pskd, sizeof(pskd));
            pb_enroll_wipe(&endpoint, sizeof(endpoint));
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
        }
    }

    memset(&st, 0, sizeof(st));
    w->get_status(&st, w->ctx);
    w_lock(w);
    publish(a, &st);
    pb_ble_close_reason_t reason = apply_close_rules(a, now_ms);
    w_unlock(w);
    return reason;
}
