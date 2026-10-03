/*
 * Power Base BLE enrollment wire protocol, version 1 -- the application
 * messages carried INSIDE an established protocomm Security 2 session (so
 * every byte below is AES-256-GCM protected on the air). The normative
 * description, including the BLE service/characteristic UUIDs, is
 * docs/power-base.md "BLE 등록 전달 경로"; keep both in step.
 *
 * Every multi-byte integer is big-endian. Every request and response has a
 * fixed size except the start request, whose size is fixed by its own
 * pskd_len field. A message of any other size is refused.
 *
 * protocomm endpoints (found by a client through the 0x2901 User Description
 * of each characteristic, never by position):
 *   "proto-ver"     version JSON, readable before authentication
 *   "prov-session"  Security 2 handshake (ESP-IDF components/protocomm)
 *   "pb-status"     identity + status, optionally issues a challenge
 *   "pb-start"      hands over the one-time Thread PSKd
 *   "pb-cancel"     cancels the attempt this client started
 *
 * pb-status request (PB_BLE_STATUS_REQ_LEN = 20)
 *   [0] version  [1] PB_BLE_MSG_STATUS_REQ  [2] flags  [3] 0
 *   [4..19] request id to ask about, all zero = none
 * pb-status response (PB_BLE_STATUS_RESP_LEN = 40)
 *   [0] version  [1] PB_BLE_MSG_STATUS_RESP  [2] code  [3] 0
 *   [4..11]  this board's real EUI-64
 *   [12]     pb_enroll_state_t   [13] pb_enroll_result_t (last result)
 *   [14]     PB_BLE_STATUS_F_* flags
 *   [15]     pb_ble_phase_t of the asked request id (0 unless F_REQUEST_MATCH)
 *   [16]     result of that request (pb_enroll_result_t or PB_BLE_REQ_RESULT_*)
 *   [17]     0
 *   [18..19] challenge lifetime in seconds (0 unless F_CHALLENGE_ISSUED)
 *   [20..23] controller attempt counter
 *   [24..39] challenge (zero unless F_CHALLENGE_ISSUED)
 *
 * pb-start request (PB_BLE_START_REQ_FIXED_LEN + pskd_len = 50..76)
 *   [0] version  [1] PB_BLE_MSG_START_REQ  [2] pskd_len (6..32)  [3] 0
 *   [4..11] target EUI-64   [12..27] request id (not all zero)
 *   [28..43] challenge from this session's last pb-status
 *   [44..]  PSKd, ASCII
 * pb-start / pb-cancel response (PB_BLE_SHORT_RESP_LEN = 4)
 *   [0] version  [1] type  [2] code  [3] 0
 *
 * pb-cancel request (PB_BLE_CANCEL_REQ_LEN = 28)
 *   [0] version  [1] PB_BLE_MSG_CANCEL_REQ  [2] 0  [3] 0
 *   [4..11] target EUI-64   [12..27] request id
 *
 * Unknown version: a request whose first byte is not PB_BLE_PROTO_VERSION is
 * never acted on; the answer is the endpoint's normal response with
 * version = PB_BLE_PROTO_VERSION and code PB_BLE_CODE_UNSUPPORTED_VERSION.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_BLE_PROTO_VERSION 1u

#define PB_BLE_EP_VERSION "proto-ver"
#define PB_BLE_EP_SESSION "prov-session"
#define PB_BLE_EP_STATUS "pb-status"
#define PB_BLE_EP_START "pb-start"
#define PB_BLE_EP_CANCEL "pb-cancel"

#define PB_BLE_MSG_STATUS_REQ 0x01u
#define PB_BLE_MSG_START_REQ 0x02u
#define PB_BLE_MSG_CANCEL_REQ 0x03u
#define PB_BLE_MSG_STATUS_RESP 0x81u
#define PB_BLE_MSG_START_RESP 0x82u
#define PB_BLE_MSG_CANCEL_RESP 0x83u

#define PB_BLE_EUI64_LEN 8
#define PB_BLE_REQUEST_ID_LEN 16
#define PB_BLE_CHALLENGE_LEN 16

#define PB_BLE_STATUS_REQ_LEN 20
#define PB_BLE_STATUS_RESP_LEN 40
#define PB_BLE_START_REQ_FIXED_LEN 44
#define PB_BLE_START_REQ_MAX_LEN 76 /* PB_BLE_START_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN */
#define PB_BLE_CANCEL_REQ_LEN 28
#define PB_BLE_SHORT_RESP_LEN 4
/* Largest plaintext in either direction; Security 2 adds a 16-byte tag. */
#define PB_BLE_MAX_REQUEST_LEN PB_BLE_START_REQ_MAX_LEN
#define PB_BLE_MAX_RESPONSE_LEN PB_BLE_STATUS_RESP_LEN

/* pb-status request flags */
#define PB_BLE_STATUS_REQ_F_ISSUE_CHALLENGE 0x01u

/* pb-status response flags */
#define PB_BLE_STATUS_F_ATTEMPT_ACTIVE 0x01u    /* a BLE-started attempt is queued or running */
#define PB_BLE_STATUS_F_REQUEST_MATCH 0x02u     /* the asked request id is that attempt (current or last) */
#define PB_BLE_STATUS_F_CHALLENGE_ISSUED 0x04u  /* bytes [24..39] hold a fresh challenge */
#define PB_BLE_STATUS_F_JOINER_UNAVAILABLE 0x08u /* this board cannot run a Joiner (EUI-64 mismatch) */

/* Response codes. 0x00..0x0f = the request was honoured. */
typedef enum {
    PB_BLE_CODE_OK = 0x00,        /* pb-status answered; pb-cancel taken */
    PB_BLE_CODE_ACCEPTED = 0x01,  /* pb-start admitted and queued. NOT Joiner success, NOT enrollment. */
    PB_BLE_CODE_DUPLICATE = 0x02, /* pb-start: this exact request was already accepted; nothing new started */

    PB_BLE_CODE_UNSUPPORTED_VERSION = 0x10,
    PB_BLE_CODE_MALFORMED = 0x11,          /* wrong size or type, non-zero reserved byte, all-zero request id */
    PB_BLE_CODE_WRONG_TARGET = 0x12,       /* target EUI-64 is not this board */
    PB_BLE_CODE_CHALLENGE_INVALID = 0x13,  /* none issued in this session, wrong value, or already used */
    PB_BLE_CODE_CHALLENGE_EXPIRED = 0x14,
    PB_BLE_CODE_INVALID_PSKD = 0x15,
    PB_BLE_CODE_BUSY = 0x16,               /* another attempt is in progress, or the worker queue is full */
    PB_BLE_CODE_REQUEST_CONFLICT = 0x17,   /* request id seen before with other content, or already finished */
    PB_BLE_CODE_NOT_WAITING = 0x18,        /* the board is not waiting for enrollment */
    PB_BLE_CODE_NO_SUCH_ATTEMPT = 0x19,    /* pb-cancel: that request id is not a cancellable attempt */
    PB_BLE_CODE_CLOSED = 0x1a,             /* BLE enrollment is closed for this boot */
    PB_BLE_CODE_UNAVAILABLE = 0x1b,        /* this board cannot run a Joiner */
    PB_BLE_CODE_INTERNAL = 0x1c,
} pb_ble_code_t;

/* Lifecycle of the one BLE-started attempt the board remembers. */
typedef enum {
    PB_BLE_PHASE_NONE = 0,
    PB_BLE_PHASE_QUEUED = 1,    /* accepted, the worker has not started the Joiner yet */
    PB_BLE_PHASE_RUNNING = 2,   /* Joiner running or attaching (see the state byte) */
    PB_BLE_PHASE_FAILED = 3,    /* ended without enrollment; see the request result byte */
    PB_BLE_PHASE_ENROLLED = 4,  /* attached + marker stored. The hub's heartbeat_received is still separate. */
    PB_BLE_PHASE_CANCELLED = 5,
} pb_ble_phase_t;

/* Request result values that are not a pb_enroll_result_t. */
#define PB_BLE_REQ_RESULT_START_REFUSED 0x80u /* the controller was no longer waiting when the worker got to it */
#define PB_BLE_REQ_RESULT_SUPERSEDED 0x81u    /* another attempt replaced it */

#ifdef __cplusplus
}
#endif
