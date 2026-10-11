#include "power_base_mgmt/pb_mgmt_proto.h"

#include <string.h>

/* Body offsets, after the common header. */
#define BODY PB_MGMT_HEADER_LEN

static const uint8_t k_mac_domain[PB_MGMT_MAC_DOMAIN_LEN] = PB_MGMT_MAC_DOMAIN;

size_t pb_mgmt_message_len(uint8_t type)
{
    switch (type) {
    case PB_MGMT_TYPE_POLL: return PB_MGMT_POLL_LEN;
    case PB_MGMT_TYPE_IDLE: return PB_MGMT_IDLE_LEN;
    case PB_MGMT_TYPE_UNENROLL: return PB_MGMT_UNENROLL_LEN;
    case PB_MGMT_TYPE_UNENROLL_ACCEPTED: return PB_MGMT_UNENROLL_ACCEPTED_LEN;
    case PB_MGMT_TYPE_UNENROLL_DONE: return PB_MGMT_UNENROLL_DONE_LEN;
    case PB_MGMT_TYPE_RESULT_ACK: return PB_MGMT_RESULT_ACK_LEN;
    case PB_MGMT_TYPE_HELLO: return PB_MGMT_HELLO_LEN;
    case PB_MGMT_TYPE_HELLO_ACK: return PB_MGMT_HELLO_ACK_LEN;
    case PB_MGMT_TYPE_LOCATE: return PB_MGMT_LOCATE_LEN;
    case PB_MGMT_TYPE_LOCATE_ACK: return PB_MGMT_LOCATE_ACK_LEN;
    default: return 0;
    }
}

bool pb_mgmt_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool pb_mgmt_is_zero(const uint8_t *p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

void pb_mgmt_wipe(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (len--) {
        *p++ = 0;
    }
}

static uint8_t allowed_flags(uint8_t type)
{
    return type == PB_MGMT_TYPE_IDLE ? PB_MGMT_IDLE_F_COMMAND_PENDING : 0;
}

/* Everything a message of this type may say, beyond its layout. */
static bool values_ok(const pb_mgmt_msg_t *m)
{
    if ((m->flags & (uint8_t)~allowed_flags(m->type)) != 0) {
        return false;
    }
    switch (m->type) {
    case PB_MGMT_TYPE_POLL: return m->state == PB_MGMT_STATE_ENROLLED;
    case PB_MGMT_TYPE_UNENROLL_DONE:
        return m->result == PB_MGMT_RESULT_RELEASED || m->result == PB_MGMT_RESULT_ERASED;
    case PB_MGMT_TYPE_RESULT_ACK:
        if (m->acked_type == PB_MGMT_TYPE_UNENROLL_ACCEPTED) {
            return m->acked_result == 0;
        }
        return m->acked_type == PB_MGMT_TYPE_UNENROLL_DONE &&
               (m->acked_result == PB_MGMT_RESULT_RELEASED || m->acked_result == PB_MGMT_RESULT_ERASED);
    case PB_MGMT_TYPE_LOCATE_ACK:
        /* Structure only: "is this an address a board may use" is the
         * board's own check (power_base_enrollment.h). */
        return m->endpoint_port != 0 && !pb_mgmt_is_zero(m->endpoint_addr, sizeof(m->endpoint_addr));
    default: return true;
    }
}

/* The message without its MAC. Returns that length, 0 if not encodable. */
static size_t encode_unsigned(const pb_mgmt_msg_t *m, uint8_t out[PB_MGMT_MAX_MESSAGE_LEN])
{
    size_t total = pb_mgmt_message_len(m->type);
    if (total == 0 || !values_ok(m)) {
        return 0;
    }
    memset(out, 0, PB_MGMT_MAX_MESSAGE_LEN);
    out[0] = PB_MGMT_PROTO_VERSION;
    out[1] = m->type;
    out[2] = m->flags;
    memcpy(&out[4], m->id.eui64, PB_MGMT_EUI64_LEN);
    memcpy(&out[12], m->id.hub_id, PB_MGMT_HUB_ID_LEN);
    memcpy(&out[28], m->id.generation, PB_MGMT_GENERATION_LEN);
    switch (m->type) {
    case PB_MGMT_TYPE_POLL:
        memcpy(&out[BODY], m->board_nonce, PB_MGMT_NONCE_LEN);
        memcpy(&out[BODY + 16], m->hub_nonce, PB_MGMT_NONCE_LEN);
        out[BODY + 32] = m->state;
        break;
    case PB_MGMT_TYPE_IDLE:
        memcpy(&out[BODY], m->board_nonce, PB_MGMT_NONCE_LEN);
        memcpy(&out[BODY + 16], m->hub_nonce, PB_MGMT_NONCE_LEN);
        break;
    case PB_MGMT_TYPE_UNENROLL:
        memcpy(&out[BODY], m->board_nonce, PB_MGMT_NONCE_LEN);
        memcpy(&out[BODY + 16], m->command_id, PB_MGMT_COMMAND_ID_LEN);
        break;
    case PB_MGMT_TYPE_UNENROLL_ACCEPTED:
        memcpy(&out[BODY], m->command_id, PB_MGMT_COMMAND_ID_LEN);
        break;
    case PB_MGMT_TYPE_UNENROLL_DONE:
        memcpy(&out[BODY], m->command_id, PB_MGMT_COMMAND_ID_LEN);
        out[BODY + 16] = m->result;
        break;
    case PB_MGMT_TYPE_RESULT_ACK:
        memcpy(&out[BODY], m->command_id, PB_MGMT_COMMAND_ID_LEN);
        out[BODY + 16] = m->acked_type;
        out[BODY + 17] = m->acked_result;
        break;
    case PB_MGMT_TYPE_HELLO:
    case PB_MGMT_TYPE_HELLO_ACK:
        out[BODY] = (uint8_t)(m->capabilities >> 24);
        out[BODY + 1] = (uint8_t)(m->capabilities >> 16);
        out[BODY + 2] = (uint8_t)(m->capabilities >> 8);
        out[BODY + 3] = (uint8_t)m->capabilities;
        break;
    case PB_MGMT_TYPE_LOCATE:
        memcpy(&out[BODY], m->board_nonce, PB_MGMT_NONCE_LEN);
        break;
    case PB_MGMT_TYPE_LOCATE_ACK:
        memcpy(&out[BODY], m->board_nonce, PB_MGMT_NONCE_LEN);
        memcpy(&out[BODY + 16], m->endpoint_addr, PB_MGMT_ENDPOINT_ADDR_LEN);
        out[BODY + 32] = (uint8_t)(m->endpoint_port >> 8);
        out[BODY + 33] = (uint8_t)m->endpoint_port;
        break;
    default: return 0;
    }
    return total - PB_MGMT_MAC_LEN;
}

static bool compute_mac(const uint8_t *unsigned_msg, size_t unsigned_len, const uint8_t key[PB_MGMT_KEY_LEN],
                        pb_mgmt_hmac_fn hmac, void *hmac_ctx, uint8_t out[PB_MGMT_MAC_LEN])
{
    uint8_t input[PB_MGMT_MAC_DOMAIN_LEN + PB_MGMT_MAX_MESSAGE_LEN];
    if (hmac == NULL || unsigned_len > PB_MGMT_MAX_MESSAGE_LEN) {
        return false;
    }
    memcpy(input, k_mac_domain, PB_MGMT_MAC_DOMAIN_LEN);
    memcpy(&input[PB_MGMT_MAC_DOMAIN_LEN], unsigned_msg, unsigned_len);
    bool ok = hmac(key, PB_MGMT_KEY_LEN, input, PB_MGMT_MAC_DOMAIN_LEN + unsigned_len, out, hmac_ctx);
    if (!ok) {
        pb_mgmt_wipe(out, PB_MGMT_MAC_LEN);
    }
    return ok;
}

size_t pb_mgmt_pack(const pb_mgmt_msg_t *msg, const uint8_t key[PB_MGMT_KEY_LEN], pb_mgmt_hmac_fn hmac, void *hmac_ctx,
                    uint8_t *out, size_t cap)
{
    uint8_t buf[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t mac[PB_MGMT_MAC_LEN];
    if (msg == NULL || key == NULL || out == NULL) {
        return 0;
    }
    size_t unsigned_len = encode_unsigned(msg, buf);
    if (unsigned_len == 0 || cap < unsigned_len + PB_MGMT_MAC_LEN ||
        !compute_mac(buf, unsigned_len, key, hmac, hmac_ctx, mac)) {
        return 0;
    }
    memcpy(out, buf, unsigned_len);
    memcpy(&out[unsigned_len], mac, PB_MGMT_MAC_LEN);
    return unsigned_len + PB_MGMT_MAC_LEN;
}

size_t pb_mgmt_pack_with_mac(const pb_mgmt_msg_t *msg, const uint8_t mac[PB_MGMT_MAC_LEN], uint8_t *out, size_t cap)
{
    uint8_t buf[PB_MGMT_MAX_MESSAGE_LEN];
    if (msg == NULL || mac == NULL || out == NULL) {
        return 0;
    }
    size_t unsigned_len = encode_unsigned(msg, buf);
    if (unsigned_len == 0 || cap < unsigned_len + PB_MGMT_MAC_LEN) {
        return 0;
    }
    memcpy(out, buf, unsigned_len);
    memcpy(&out[unsigned_len], mac, PB_MGMT_MAC_LEN);
    return unsigned_len + PB_MGMT_MAC_LEN;
}

bool pb_mgmt_parse(const uint8_t *data, size_t len, pb_mgmt_msg_t *out)
{
    if (data == NULL || out == NULL || len < PB_MGMT_HEADER_LEN + PB_MGMT_MAC_LEN) {
        return false;
    }
    if (data[0] != PB_MGMT_PROTO_VERSION || data[3] != 0) {
        return false;
    }
    size_t total = pb_mgmt_message_len(data[1]);
    if (total == 0 || len != total) {
        return false;
    }

    pb_mgmt_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = data[1];
    m.flags = data[2];
    memcpy(m.id.eui64, &data[4], PB_MGMT_EUI64_LEN);
    memcpy(m.id.hub_id, &data[12], PB_MGMT_HUB_ID_LEN);
    memcpy(m.id.generation, &data[28], PB_MGMT_GENERATION_LEN);

    /* Bytes of the body that must be zero. */
    const uint8_t *pad = NULL;
    size_t pad_len = 3;
    switch (m.type) {
    case PB_MGMT_TYPE_POLL:
        memcpy(m.board_nonce, &data[BODY], PB_MGMT_NONCE_LEN);
        memcpy(m.hub_nonce, &data[BODY + 16], PB_MGMT_NONCE_LEN);
        m.state = data[BODY + 32];
        pad = &data[BODY + 33];
        break;
    case PB_MGMT_TYPE_IDLE:
        memcpy(m.board_nonce, &data[BODY], PB_MGMT_NONCE_LEN);
        memcpy(m.hub_nonce, &data[BODY + 16], PB_MGMT_NONCE_LEN);
        break;
    case PB_MGMT_TYPE_UNENROLL:
        memcpy(m.board_nonce, &data[BODY], PB_MGMT_NONCE_LEN);
        memcpy(m.command_id, &data[BODY + 16], PB_MGMT_COMMAND_ID_LEN);
        break;
    case PB_MGMT_TYPE_UNENROLL_ACCEPTED:
        memcpy(m.command_id, &data[BODY], PB_MGMT_COMMAND_ID_LEN);
        break;
    case PB_MGMT_TYPE_UNENROLL_DONE:
        memcpy(m.command_id, &data[BODY], PB_MGMT_COMMAND_ID_LEN);
        m.result = data[BODY + 16];
        pad = &data[BODY + 17];
        break;
    case PB_MGMT_TYPE_RESULT_ACK:
        memcpy(m.command_id, &data[BODY], PB_MGMT_COMMAND_ID_LEN);
        m.acked_type = data[BODY + 16];
        m.acked_result = data[BODY + 17];
        pad = &data[BODY + 18];
        pad_len = 2;
        break;
    case PB_MGMT_TYPE_HELLO:
    case PB_MGMT_TYPE_HELLO_ACK:
        /* Every bit is read; which ones mean something is the reader's
         * business (an unknown one is not an error). */
        m.capabilities = ((uint32_t)data[BODY] << 24) | ((uint32_t)data[BODY + 1] << 16) |
                         ((uint32_t)data[BODY + 2] << 8) | (uint32_t)data[BODY + 3];
        pad = &data[BODY + 4];
        pad_len = 4;
        break;
    case PB_MGMT_TYPE_LOCATE:
        memcpy(m.board_nonce, &data[BODY], PB_MGMT_NONCE_LEN);
        pad = &data[BODY + 16];
        pad_len = 4;
        break;
    case PB_MGMT_TYPE_LOCATE_ACK:
        memcpy(m.board_nonce, &data[BODY], PB_MGMT_NONCE_LEN);
        memcpy(m.endpoint_addr, &data[BODY + 16], PB_MGMT_ENDPOINT_ADDR_LEN);
        m.endpoint_port = (uint16_t)(((uint16_t)data[BODY + 32] << 8) | (uint16_t)data[BODY + 33]);
        pad = &data[BODY + 34];
        pad_len = 2;
        break;
    default: return false;
    }
    if (pad != NULL && !pb_mgmt_is_zero(pad, pad_len)) {
        return false;
    }
    if (!values_ok(&m)) {
        return false;
    }
    /* An identity or an id made of zeros names nothing. */
    if (pb_mgmt_is_zero(m.id.eui64, sizeof(m.id.eui64)) || pb_mgmt_is_zero(m.id.hub_id, sizeof(m.id.hub_id)) ||
        pb_mgmt_is_zero(m.id.generation, sizeof(m.id.generation))) {
        return false;
    }
    const bool has_command_id = m.type == PB_MGMT_TYPE_UNENROLL || m.type == PB_MGMT_TYPE_UNENROLL_ACCEPTED ||
                                m.type == PB_MGMT_TYPE_UNENROLL_DONE || m.type == PB_MGMT_TYPE_RESULT_ACK;
    if (has_command_id && pb_mgmt_is_zero(m.command_id, sizeof(m.command_id))) {
        return false;
    }
    if ((m.type == PB_MGMT_TYPE_POLL || m.type == PB_MGMT_TYPE_IDLE || m.type == PB_MGMT_TYPE_UNENROLL ||
         m.type == PB_MGMT_TYPE_LOCATE || m.type == PB_MGMT_TYPE_LOCATE_ACK) &&
        pb_mgmt_is_zero(m.board_nonce, sizeof(m.board_nonce))) {
        return false;
    }
    *out = m;
    return true;
}

bool pb_mgmt_verify(const uint8_t *data, size_t len, const uint8_t key[PB_MGMT_KEY_LEN], pb_mgmt_hmac_fn hmac,
                    void *hmac_ctx)
{
    uint8_t mac[PB_MGMT_MAC_LEN];
    if (data == NULL || key == NULL || len < PB_MGMT_HEADER_LEN + PB_MGMT_MAC_LEN || len > PB_MGMT_MAX_MESSAGE_LEN) {
        return false;
    }
    if (!compute_mac(data, len - PB_MGMT_MAC_LEN, key, hmac, hmac_ctx, mac)) {
        return false;
    }
    bool same = pb_mgmt_ct_equal(mac, &data[len - PB_MGMT_MAC_LEN], PB_MGMT_MAC_LEN);
    pb_mgmt_wipe(mac, sizeof(mac));
    return same;
}

const uint8_t *pb_mgmt_mac_of(const uint8_t *data, size_t len)
{
    return &data[len - PB_MGMT_MAC_LEN];
}
