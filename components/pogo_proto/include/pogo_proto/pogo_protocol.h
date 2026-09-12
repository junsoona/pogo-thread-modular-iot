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
    POGO_MSG_IDENTIFY_REQ  = 0x01, /* upper -> lower: docked, please identify yourself. Empty payload. */
    POGO_MSG_IDENTIFY_RESP = 0x02, /* lower -> upper: hw id + fw + sensor bitmap placeholder. */
    POGO_MSG_ASSIGN_UUID   = 0x03, /* upper -> lower: assign an 8-byte UUID. */
    POGO_MSG_ASSIGN_SLOT   = 0x04, /* upper -> lower: assign a 1-byte slot id. */
    POGO_MSG_SET_DATASET   = 0x05, /* upper -> lower: raw OpenThread Active Dataset TLVs (payload = TLV bytes). */
    POGO_MSG_ACK           = 0x06, /* lower -> upper: generic positive ack; payload names which type was acked. */
    POGO_MSG_NACK          = 0x07, /* lower -> upper: generic negative ack + reason code. */
    POGO_MSG_JOIN_RESULT   = 0x08, /* lower -> upper: Thread join outcome, sent once the role settles or times out. */
    POGO_MSG_PING          = 0x09, /* upper -> lower: periodic docking-detection probe. Empty payload. */
    POGO_MSG_PONG          = 0x0A, /* lower -> upper: probe reply. Empty payload. */
} pogo_msg_type_t;

typedef enum {
    POGO_NACK_BAD_STATE       = 1, /* message not valid in the lower's current pairing state */
    POGO_NACK_INVALID_DATASET = 2, /* dataset TLVs failed otDatasetIsValid() */
    POGO_NACK_STORAGE_FAILED  = 3, /* NVS write failed */
} pogo_nack_reason_t;

#define POGO_UUID_LEN 8

typedef struct {
    uint8_t mac[6];         /* station MAC, from esp_efuse_mac_get_default() */
    uint8_t fw_version;     /* firmware build tag, informational only */
    uint8_t sensor_bitmap;  /* reserved for future sensor discovery; always 0 in this MVP */
} pogo_identify_resp_t;

typedef struct {
    uint8_t uuid[POGO_UUID_LEN];
} pogo_assign_uuid_t;

typedef struct {
    uint8_t slot_id;
} pogo_assign_slot_t;

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

#ifdef __cplusplus
}
#endif
