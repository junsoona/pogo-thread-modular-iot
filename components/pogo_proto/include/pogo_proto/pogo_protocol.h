/*
 * POGO provisioning protocol v1 -- message types and payload (de)serialization.
 *
 * This sits on top of pogo_frame.h. A pogo_frame_t's `type` field holds one
 * of the pogo_msg_type_t values below and its `payload`/`len` holds the
 * packed struct for that type (SET_DATASET/PING/PONG/IDENTIFY_REQ carry
 * their payload directly with no wrapper struct -- see comments).
 *
 * Request/response pairs share the same frame SEQUENCE value, which the
 * pairing state machines use to detect and safely no-op retransmissions
 * (see upper_pairing.c / lower_pairing.c) instead of re-applying an action
 * (e.g. re-writing NVS or re-assigning a UUID) a second time.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POGO_MSG_IDENTIFY_REQ    = 0x01, /* parent -> child: docked, please identify yourself. Empty payload. */
    POGO_MSG_IDENTIFY_RESP   = 0x02, /* child -> parent: hw id + fw + sensor bitmap. */
    POGO_MSG_ASSIGN_UUID     = 0x03, /* parent -> child: assign an 8-byte UUID. */
    POGO_MSG_ASSIGN_SLOT     = 0x04, /* parent -> child: assign slot_id/topology_depth. */
    POGO_MSG_SET_DATASET     = 0x05, /* parent -> child: raw OpenThread Active Dataset TLVs (payload = TLV bytes). */
    POGO_MSG_ACK             = 0x06, /* child -> parent: generic positive ack; payload names which type was acked. */
    POGO_MSG_NACK            = 0x07, /* child -> parent: generic negative ack + reason code. */
    POGO_MSG_JOIN_RESULT     = 0x08, /* child -> parent: Thread join outcome, sent once the role settles or times out. */
    POGO_MSG_PING            = 0x09, /* parent -> child: periodic docking-detection probe. Empty payload. */
    POGO_MSG_PONG            = 0x0A, /* child -> parent: probe reply. Empty payload. */
    /*
     * Explicit request/response pair -- NOT a symmetric "both sides send the
     * same thing" exchange. Either side may be the initiator (whichever
     * attaches/boots and starts sending first): it sends HELLO_REQ and waits
     * for a same-seq HELLO_RESP to reach PEER_READY. A HELLO_REQ is answered
     * with HELLO_RESP in ANY state (including already-PEER_READY, e.g. the
     * peer rebooted while still docked) -- see pogo_port.c. Both carry the
     * same payload shape (pogo_peer_hello_t: protocol_version + peer_type).
     */
    POGO_MSG_PEER_HELLO_REQ  = 0x0B,
    POGO_MSG_SENSOR_HELLO    = 0x0C, /* parent -> child: "report your permanently-attached sensor's info". Empty payload. */
    POGO_MSG_SENSOR_INFO_ACK = 0x0D, /* child -> parent: sensor_type_bitmap/response_time_ms/status (see below). */
    POGO_MSG_POLL_CONFIG     = 0x0E, /* parent -> child: computed polling_period_ms for this child to use. */
    POGO_MSG_PEER_HELLO_RESP = 0x0F, /* reply to POGO_MSG_PEER_HELLO_REQ, same seq. */
} pogo_msg_type_t;

typedef enum {
    POGO_NACK_BAD_STATE          = 1, /* message not valid in the child's current pairing state */
    POGO_NACK_INVALID_DATASET    = 2, /* dataset TLVs failed otDatasetIsValid() */
    POGO_NACK_STORAGE_FAILED     = 3, /* NVS write failed */
    POGO_NACK_SENSOR_ERROR       = 4, /* the permanently-attached sensor failed to respond */
    POGO_NACK_POLL_CONFIG_RANGE  = 5, /* POLL_CONFIG polling_period_ms outside the child's accepted range */
    POGO_NACK_PEER_TYPE_CONFLICT = 6, /* PEER_HELLO peer_type contradicts BASE_ID hardware read */
} pogo_nack_reason_t;

#define POGO_UUID_LEN 8

/* Canonical peer-type enum: shared verbatim by pogo_port (attach-time
 * PEER_HELLO exchange) and role_manager (ROOT/INTERMEDIATE/LEAF rules), so
 * it lives here in the lowest-level wire-format component rather than being
 * redefined per-consumer. */
typedef enum {
    POGO_PEER_TYPE_UNKNOWN     = 0,
    POGO_PEER_TYPE_POWER_BASE  = 1,
    POGO_PEER_TYPE_MODULE      = 2,
} pogo_peer_type_t;

typedef struct {
    uint8_t protocol_version; /* echoes POGO_FRAME_VERSION; a mismatch is a PROTOCOL_VERSION_ERROR upstream */
    uint8_t peer_type;        /* one of pogo_peer_type_t -- what *I* am, from this module's own point of view */
} pogo_peer_hello_t;

/*
 * A received peer_type is only ever valid as MODULE or POWER_BASE --
 * POGO_PEER_TYPE_UNKNOWN (or any other byte value) coming FROM a peer is a
 * protocol error, not "no opinion" (that meaning is reserved for this
 * module's own not-yet-determined state, e.g. pogo_port_get_peer_type()
 * before PEER_READY).
 */
static inline bool pogo_peer_type_is_valid(uint8_t v)
{
    return v == (uint8_t)POGO_PEER_TYPE_MODULE || v == (uint8_t)POGO_PEER_TYPE_POWER_BASE;
}

typedef struct {
    uint8_t mac[6];         /* station MAC, from esp_efuse_mac_get_default() */
    uint8_t fw_version;     /* firmware build tag, informational only */
    uint8_t sensor_bitmap;  /* low 8 bits of this module's fixed sensor_type_bitmap (module_config) */
} pogo_identify_resp_t;

typedef struct {
    uint8_t uuid[POGO_UUID_LEN];
} pogo_assign_uuid_t;

typedef struct {
    uint8_t slot_id;         /* position among this parent's downstream children (currently always 0: one DOWNSTREAM port) */
    uint8_t topology_depth;  /* hops from the ROOT, e.g. ROOT's direct child = 1 */
} pogo_assign_slot_t;

typedef struct {
    uint16_t sensor_type_bitmap;
    uint32_t response_time_ms; /* measured by the child's sensor_provider, not a protocol constant */
    uint8_t  fw_version;
    uint8_t  sensor_status;   /* 0 = OK, nonzero = sensor_provider status code */
} pogo_sensor_info_ack_t;

typedef struct {
    uint32_t polling_period_ms;
} pogo_poll_config_t;

typedef struct {
    uint8_t acked_type; /* one of pogo_msg_type_t */
} pogo_ack_t;

typedef struct {
    uint8_t acked_type; /* one of pogo_msg_type_t */
    uint8_t reason;     /* one of pogo_nack_reason_t */
} pogo_nack_t;

typedef struct {
    uint8_t success; /* 1 = joined, 0 = failed/timed out */
    uint8_t role;    /* otDeviceRole at the time of reporting */
} pogo_join_result_t;

/*
 * Each pack_* serializes `in` into `out` (capacity out_size) and returns the
 * number of bytes written, or 0 if out_size is too small.
 * Each unpack_* parses `data`/`len` into `out` and returns false if `len`
 * does not exactly match the expected size for that message.
 */
size_t pogo_pack_identify_resp(const pogo_identify_resp_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_identify_resp(const uint8_t *data, uint16_t len, pogo_identify_resp_t *out);

size_t pogo_pack_assign_uuid(const pogo_assign_uuid_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_assign_uuid(const uint8_t *data, uint16_t len, pogo_assign_uuid_t *out);

size_t pogo_pack_assign_slot(const pogo_assign_slot_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_assign_slot(const uint8_t *data, uint16_t len, pogo_assign_slot_t *out);

size_t pogo_pack_ack(const pogo_ack_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_ack(const uint8_t *data, uint16_t len, pogo_ack_t *out);

size_t pogo_pack_nack(const pogo_nack_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_nack(const uint8_t *data, uint16_t len, pogo_nack_t *out);

size_t pogo_pack_join_result(const pogo_join_result_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_join_result(const uint8_t *data, uint16_t len, pogo_join_result_t *out);

size_t pogo_pack_peer_hello(const pogo_peer_hello_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_peer_hello(const uint8_t *data, uint16_t len, pogo_peer_hello_t *out);

size_t pogo_pack_sensor_info_ack(const pogo_sensor_info_ack_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_sensor_info_ack(const uint8_t *data, uint16_t len, pogo_sensor_info_ack_t *out);

size_t pogo_pack_poll_config(const pogo_poll_config_t *in, uint8_t *out, size_t out_size);
bool   pogo_unpack_poll_config(const uint8_t *data, uint16_t len, pogo_poll_config_t *out);

#ifdef __cplusplus
}
#endif
