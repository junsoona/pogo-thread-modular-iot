/*
 * Power Base BLE enrollment wire protocol, version 1 -- the application
 * messages carried INSIDE an established protocomm Security 2 session (so
 * every byte below is AES-256-GCM protected on the air). The normative
 * description, including the BLE service/characteristic UUIDs, is
 * docs/power-base.md "BLE 등록 전달 경로"; keep both in step.
 *
 * Every multi-byte integer is big-endian. Every request and response has a
 * fixed size except the two start requests, whose size is fixed by their own
 * pskd_len field. A message of any other size is refused.
 *
 * protocomm endpoints (found by a client through the 0x2901 User Description
 * of each characteristic, never by position):
 *   "proto-ver"     version JSON, readable before authentication
 *   "prov-session"  Security 2 handshake (ESP-IDF components/protocomm)
 *   "pb-status"     identity + status, optionally issues a challenge; and,
 *                   as a second message on the same endpoint, the board's
 *                   release receipt (PB_BLE_MSG_RECEIPT_REQ, capability
 *                   PB_BLE_CAP_RELEASE_RECEIPT_V1); and, as a third, what
 *                   became of a local reset
 *                   (PB_BLE_MSG_LOCAL_RESET_STATUS_REQ, capability
 *                   PB_BLE_CAP_LOCAL_RESET_V1)
 *   "pb-start"      hands over the one-time Thread PSKd -- alone
 *                   (PB_BLE_MSG_START_REQ), with the Linux bridge endpoint
 *                   (PB_BLE_MSG_START_EP_REQ, capability
 *                   PB_BLE_CAP_BRIDGE_ENDPOINT_V1), or with the endpoint and
 *                   the hub's management credential
 *                   (PB_BLE_MSG_START_MGMT_REQ, capability PB_BLE_CAP_MGMT_V1);
 *                   and the local reset command of a board whose
 *                   re-enrollment window is open (PB_BLE_MSG_LOCAL_RESET_REQ,
 *                   capability PB_BLE_CAP_LOCAL_RESET_V1)
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
 * pb-start request WITH the bridge endpoint, message PB_BLE_MSG_START_EP_REQ
 * (PB_BLE_START_EP_REQ_FIXED_LEN + pskd_len = 70..96). A separate message on
 * the same endpoint; the version 1 request above keeps its bytes and meaning
 * and nothing was appended to it.
 *   [0] version  [1] PB_BLE_MSG_START_EP_REQ  [2] pskd_len (6..32)  [3] 0
 *   [4..11] target EUI-64   [12..27] request id (not all zero)
 *   [28..43] challenge from this session's last pb-status
 *   [44]     bridge endpoint format, PB_BLE_BRIDGE_ENDPOINT_VERSION (1)
 *   [45]     0 (reserved)
 *   [46..47] bridge CoAP port, big-endian, 1..65535
 *   [48..63] bridge IPv6 address, 16 bytes in network byte order: a Thread
 *            Mesh-Local EID (fd00::/8, interface id not 0000:00ff:fe00:xxxx)
 *   [64..]   PSKd, ASCII
 * Its response is the short response with type PB_BLE_MSG_START_EP_RESP and
 * the same codes as pb-start, plus PB_BLE_CODE_INVALID_ENDPOINT.
 *
 * Capability and old peers. "proto-ver" lists PB_BLE_CAP_BRIDGE_ENDPOINT_V1
 * in pb_enroll.cap when the board takes PB_BLE_MSG_START_EP_REQ; a client
 * sends that message only to a board that lists it. A board without it
 * (older firmware: "pb_enroll":{"proto":1}, no cap) answers that message as
 * any other unknown type on pb-start: PB_BLE_MSG_START_RESP with
 * PB_BLE_CODE_MALFORMED, nothing started. A client that does not know the
 * capability keeps using PB_BLE_MSG_START_REQ, which a board accepts
 * unchanged -- unless its prepared image requires the bridge endpoint with
 * the enrollment (an image made without a bridge address on purpose). Such a
 * board answers PB_BLE_MSG_START_REQ with PB_BLE_CODE_UNAVAILABLE, a code
 * every client already knows, instead of enrolling into Thread with nowhere
 * to send to. On every other board that message enrolls as before, without
 * a delivered endpoint.
 *
 * pb-start request WITH the bridge endpoint AND the management credential,
 * message PB_BLE_MSG_START_MGMT_REQ (PB_BLE_START_MGMT_REQ_FIXED_LEN +
 * pskd_len = 136..162). A third message on the same endpoint; the two above
 * keep their bytes. See docs/power-base-management.md.
 *   [0] version  [1] PB_BLE_MSG_START_MGMT_REQ  [2] pskd_len (6..32)  [3] 0
 *   [4..11] target EUI-64   [12..27] request id (not all zero)
 *   [28..43] challenge from this session's last pb-status
 *   [44..63] the bridge endpoint block, exactly as in PB_BLE_MSG_START_EP_REQ
 *   [64]      management format, PB_BLE_MGMT_VERSION (1)
 *   [65]      0 (reserved)
 *   [66..81]  hub id (not all zero)
 *   [82..97]  enrollment generation (not all zero)
 *   [98..129] management key, 32 bytes (not all zero) -- a secret
 *   [130..]   PSKd, ASCII
 * Its response is the short response with type PB_BLE_MSG_START_MGMT_RESP and
 * the codes of PB_BLE_MSG_START_EP_REQ, plus PB_BLE_CODE_INVALID_MANAGEMENT.
 * "proto-ver" lists PB_BLE_CAP_MGMT_V1 when the board takes it; a board
 * without it answers as for any unknown type (PB_BLE_MSG_START_RESP,
 * PB_BLE_CODE_MALFORMED).
 *
 * Release receipt, message PB_BLE_MSG_RECEIPT_REQ on "pb-status"
 * (PB_BLE_RECEIPT_REQ_LEN = 12). See docs/power-base-management.md: a board
 * that was unenrolled by its hub has no Thread network left once its stored
 * network is erased, so the one message that says "erased and verified" is
 * collected here, inside the Security 2 session, and relayed to the hub by
 * the client. Reading it changes nothing on the board and needs no challenge.
 *   [0] version  [1] PB_BLE_MSG_RECEIPT_REQ  [2] 0  [3] 0
 *   [4..11] target EUI-64
 * Response (PB_BLE_RECEIPT_RESP_LEN = 112)
 *   [0] version  [1] PB_BLE_MSG_RECEIPT_RESP  [2] code  [3] 0
 *   [4..11]   this board's real EUI-64
 *   [12]      receipt format, PB_BLE_RECEIPT_VERSION (1)
 *   [13]      1 = bytes [16..111] hold a receipt, 0 = this board has none
 *   [14..15]  0
 *   [16..111] the receipt: the complete management message UNENROLL_DONE
 *             with result ERASED (pb_mgmt_proto.h, 96 bytes), or zeros
 * Codes: OK, UNSUPPORTED_VERSION, MALFORMED, WRONG_TARGET. A board has a
 * receipt only while its enrollment record is a final release: after the
 * erase of its stored network was verified and until a new enrollment
 * attempt is accepted (which clears the record). The receipt holds no
 * secret (identifiers and a MAC); only the hub that issued the enrollment's
 * key can check it. A board without the capability answers this type like
 * any other unknown one on pb-status: PB_BLE_MSG_STATUS_RESP with
 * PB_BLE_CODE_MALFORMED.
 *
 * Local reset (capability PB_BLE_CAP_LOCAL_RESET_V1; docs/power-base.md "BOOT
 * 버튼 로컬 재등록"). Two messages, both new, on the two existing endpoints;
 * no earlier message, length, field or capability changed. A board lists the
 * capability only when its build has the re-enrollment button and the
 * button's GPIO passed its check. A board without it answers either message
 * like any other unknown type on that endpoint (PB_BLE_MSG_STATUS_RESP or
 * PB_BLE_MSG_START_RESP with PB_BLE_CODE_MALFORMED); a client sends neither
 * to a board that does not list the capability, and sends nothing else in
 * their place.
 *
 * Local reset command, message PB_BLE_MSG_LOCAL_RESET_REQ on "pb-start"
 * (PB_BLE_LOCAL_RESET_REQ_LEN = 48). It asks the board to erase its OWN
 * enrollment record and stored Thread network. It is taken only
 *   - inside the Security 2 session (like everything here),
 *   - for the EUI-64 that session's pb-status reported,
 *   - with the challenge of this session's last pb-status (fresh, used once),
 *   - with the confirmation byte set, and
 *   - while the board's physical re-enrollment window is open (the BOOT
 *     button was held and the window has not run out).
 *   [0] version  [1] PB_BLE_MSG_LOCAL_RESET_REQ
 *   [2] local reset format, PB_BLE_LOCAL_RESET_VERSION (1)  [3] 0
 *   [4..11]  target EUI-64   [12..27] operation id (not all zero)
 *   [28..43] challenge from this session's last pb-status
 *   [44]     PB_BLE_LOCAL_RESET_CONFIRM_ERASE (0x01): the user confirmed that
 *            the existing registration is erased. Any other value is
 *            MALFORMED.
 *   [45..47] 0
 * Its response is the short response with type PB_BLE_MSG_LOCAL_RESET_RESP:
 *   ACCEPTED   admitted and queued for the worker. NOT "erased": the worker
 *              journals the approval and only then erases. What became of it
 *              is asked with PB_BLE_MSG_LOCAL_RESET_STATUS_REQ.
 *   DUPLICATE  this operation id is the one already queued, in progress or
 *              verified (on a board that is still unenrolled). Nothing new.
 *   MALFORMED, WRONG_TARGET, CHALLENGE_INVALID, CHALLENGE_EXPIRED, CLOSED
 *                     as for pb-start (a request of another protocol version
 *                     is not this message: it is answered as pb-start,
 *                     PB_BLE_MSG_START_RESP with UNSUPPORTED_VERSION)
 *   BUSY              another operation is queued or in progress, a remote
 *                     unenroll is under way (it comes first), or the worker
 *                     queue is full
 *   REQUEST_CONFLICT  this operation id was used before and is over (it never
 *                     started, or the board was enrolled again since)
 *   WINDOW_CLOSED     no re-enrollment window is open (never opened in this
 *                     boot, or its time is over). Nothing changed.
 *   NOT_ENROLLED      the board holds no enrollment it would resume: there is
 *                     nothing to reset; enroll it the usual way.
 * A request that is refused changed nothing. One that is a duplicate or a
 * conflict, or comes without an open window, does not use up the challenge.
 *
 * Local reset status, message PB_BLE_MSG_LOCAL_RESET_STATUS_REQ on
 * "pb-status" (PB_BLE_LOCAL_RESET_STATUS_REQ_LEN = 28). Reading it changes
 * nothing and needs no challenge and no open window: it is how a client
 * finds out, after a lost answer or a restart of the board, what became of
 * the operation it sent -- instead of sending another one.
 *   [0] version  [1] PB_BLE_MSG_LOCAL_RESET_STATUS_REQ  [2] 0  [3] 0
 *   [4..11]  target EUI-64   [12..27] operation id to ask about, all zero = none
 * Response (PB_BLE_LOCAL_RESET_STATUS_RESP_LEN = 32)
 *   [0] version  [1] PB_BLE_MSG_LOCAL_RESET_STATUS_RESP  [2] code  [3] 0
 *   [4..11]  this board's real EUI-64
 *   [12]     local reset format, PB_BLE_LOCAL_RESET_VERSION (1)
 *   [13]     PB_BLE_LR_COND_*: what the board holds now
 *   [14]     PB_BLE_LR_F_* flags
 *   [15]     PB_BLE_LR_PHASE_* of the asked operation (0 unless F_OP_MATCH)
 *   [16]     PB_BLE_LR_REASON_* of that operation (0 unless F_OP_MATCH)
 *   [17]     0
 *   [18..19] seconds left in the re-enrollment window (0 unless F_WINDOW_OPEN)
 *   [20..31] 0
 * Codes: OK, UNSUPPORTED_VERSION, MALFORMED, WRONG_TARGET (then everything
 * from [12] on is zero; for the first two also [4..11]).
 * Four different things, never one "done":
 *   accepted   the command's ACCEPTED (queued)
 *   in progress  PHASE_IN_PROGRESS: the approval is journaled; the board
 *              never resumes its old enrollment again, and carries on at
 *              every boot until the erase is verified
 *   verified   PHASE_VERIFIED with COND_NOT_ENROLLED: the enrollment record
 *              and the stored network were read back empty. The board is an
 *              unenrolled board again.
 *   enrolled anew  not reported here at all: that is pb-start's business, and
 *              in the end the new hub's heartbeat_received.
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
#define PB_BLE_MSG_START_EP_REQ 0x04u /* pb-start with the bridge endpoint */
#define PB_BLE_MSG_START_MGMT_REQ 0x06u /* pb-start with the bridge endpoint and the management credential */
#define PB_BLE_MSG_RECEIPT_REQ 0x07u /* pb-status: the release receipt */
#define PB_BLE_MSG_LOCAL_RESET_STATUS_REQ 0x08u /* pb-status: what became of a local reset */
#define PB_BLE_MSG_LOCAL_RESET_REQ 0x09u /* pb-start: the local reset command */
#define PB_BLE_MSG_STATUS_RESP 0x81u
#define PB_BLE_MSG_START_RESP 0x82u
#define PB_BLE_MSG_CANCEL_RESP 0x83u
#define PB_BLE_MSG_START_EP_RESP 0x84u
#define PB_BLE_MSG_START_MGMT_RESP 0x86u
#define PB_BLE_MSG_RECEIPT_RESP 0x87u
#define PB_BLE_MSG_LOCAL_RESET_STATUS_RESP 0x88u
#define PB_BLE_MSG_LOCAL_RESET_RESP 0x89u

/* What "proto-ver" lists in pb_enroll.cap for PB_BLE_MSG_START_EP_REQ. */
#define PB_BLE_CAP_BRIDGE_ENDPOINT_V1 "bridge_endpoint_v1"
/* Byte [44] of PB_BLE_MSG_START_EP_REQ. */
#define PB_BLE_BRIDGE_ENDPOINT_VERSION 1u
#define PB_BLE_BRIDGE_ADDR_LEN 16
/* What "proto-ver" lists in pb_enroll.cap for PB_BLE_MSG_START_MGMT_REQ. */
#define PB_BLE_CAP_MGMT_V1 "mgmt_v1"
/* Byte [64] of PB_BLE_MSG_START_MGMT_REQ. */
#define PB_BLE_MGMT_VERSION 1u
#define PB_BLE_MGMT_HUB_ID_LEN 16
#define PB_BLE_MGMT_GENERATION_LEN 16
#define PB_BLE_MGMT_KEY_LEN 32
/* What "proto-ver" lists in pb_enroll.cap for PB_BLE_MSG_RECEIPT_REQ. */
#define PB_BLE_CAP_RELEASE_RECEIPT_V1 "release_receipt_v1"
/* Byte [12] of PB_BLE_MSG_RECEIPT_RESP. */
#define PB_BLE_RECEIPT_VERSION 1u
/* The management message UNENROLL_DONE (pb_mgmt_proto.h). */
#define PB_BLE_RECEIPT_LEN 96

/* What "proto-ver" lists in pb_enroll.cap for the two local reset messages. */
#define PB_BLE_CAP_LOCAL_RESET_V1 "local_reset_v1"
/* Byte [2] of PB_BLE_MSG_LOCAL_RESET_REQ and byte [12] of
 * PB_BLE_MSG_LOCAL_RESET_STATUS_RESP. */
#define PB_BLE_LOCAL_RESET_VERSION 1u
#define PB_BLE_OP_ID_LEN 16
/* Byte [44] of PB_BLE_MSG_LOCAL_RESET_REQ. */
#define PB_BLE_LOCAL_RESET_CONFIRM_ERASE 0x01u

/* Byte [13] of PB_BLE_MSG_LOCAL_RESET_STATUS_RESP: what the board holds. */
#define PB_BLE_LR_COND_NOT_ENROLLED 0u           /* nothing it would resume: nothing to reset */
#define PB_BLE_LR_COND_ENROLLED 1u               /* an enrollment it resumes from */
#define PB_BLE_LR_COND_REMOTE_RELEASE_PENDING 2u /* a remote unenroll is under way (it comes first) */
#define PB_BLE_LR_COND_RESET_PENDING 3u          /* a journaled local reset is not finished */
#define PB_BLE_LR_COND_UNREADABLE 4u             /* the journal or the record cannot be read */
#define PB_BLE_LR_COND_MAX PB_BLE_LR_COND_UNREADABLE

/* Byte [14]. */
#define PB_BLE_LR_F_WINDOW_OPEN 0x01u /* the physical re-enrollment window is open now */
#define PB_BLE_LR_F_OP_MATCH 0x02u    /* the asked operation id is one this board knows */

/* Byte [15]. */
#define PB_BLE_LR_PHASE_NONE 0u
#define PB_BLE_LR_PHASE_QUEUED 1u      /* accepted; the worker has not journaled it yet */
#define PB_BLE_LR_PHASE_IN_PROGRESS 2u /* journaled; the erase is not verified (yet) */
#define PB_BLE_LR_PHASE_VERIFIED 3u    /* record and stored network read back empty */
#define PB_BLE_LR_PHASE_NOT_STARTED 4u /* ended before anything was changed; see the reason */
#define PB_BLE_LR_PHASE_MAX PB_BLE_LR_PHASE_NOT_STARTED

/* Byte [16]: pb_reenroll_reason_t (power_base_reenroll/pb_reenroll.h). */
#define PB_BLE_LR_REASON_NONE 0u
#define PB_BLE_LR_REASON_JOURNAL_STORE_FAILED 1u
#define PB_BLE_LR_REASON_NOT_ENROLLED 2u
#define PB_BLE_LR_REASON_REMOTE_RELEASE_PENDING 3u
#define PB_BLE_LR_REASON_OTHER_RESET_PENDING 4u
#define PB_BLE_LR_REASON_RECORD_CLEAR_FAILED 5u
#define PB_BLE_LR_REASON_RECORD_STILL_PRESENT 6u
#define PB_BLE_LR_REASON_NETWORK_ERASE_FAILED 7u
#define PB_BLE_LR_REASON_NETWORK_NOT_EMPTY 8u
#define PB_BLE_LR_REASON_JOURNAL_FINAL_FAILED 9u
#define PB_BLE_LR_REASON_MAX PB_BLE_LR_REASON_JOURNAL_FINAL_FAILED

#define PB_BLE_EUI64_LEN 8
#define PB_BLE_REQUEST_ID_LEN 16
#define PB_BLE_CHALLENGE_LEN 16

#define PB_BLE_STATUS_REQ_LEN 20
#define PB_BLE_STATUS_RESP_LEN 40
#define PB_BLE_START_REQ_FIXED_LEN 44
#define PB_BLE_START_REQ_MAX_LEN 76 /* PB_BLE_START_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN */
#define PB_BLE_START_EP_REQ_FIXED_LEN 64
#define PB_BLE_START_EP_REQ_MAX_LEN 96 /* PB_BLE_START_EP_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN */
#define PB_BLE_START_MGMT_REQ_FIXED_LEN 130
#define PB_BLE_START_MGMT_REQ_MAX_LEN 162 /* PB_BLE_START_MGMT_REQ_FIXED_LEN + PB_ENROLL_PSKD_MAX_LEN */
#define PB_BLE_CANCEL_REQ_LEN 28
#define PB_BLE_SHORT_RESP_LEN 4
#define PB_BLE_RECEIPT_REQ_LEN 12
#define PB_BLE_RECEIPT_RESP_LEN 112 /* 16 + PB_BLE_RECEIPT_LEN */
#define PB_BLE_LOCAL_RESET_STATUS_REQ_LEN 28
#define PB_BLE_LOCAL_RESET_STATUS_RESP_LEN 32
#define PB_BLE_LOCAL_RESET_REQ_LEN 48
/* Largest plaintext in either direction; Security 2 adds a 16-byte tag. */
#define PB_BLE_MAX_REQUEST_LEN PB_BLE_START_MGMT_REQ_MAX_LEN
#define PB_BLE_MAX_RESPONSE_LEN PB_BLE_RECEIPT_RESP_LEN

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
    /* This board cannot run a Joiner for this request: it cannot run one at
     * all (EUI-64 mismatch; pb-status then has F_JOINER_UNAVAILABLE), or its
     * prepared image requires the bridge endpoint with the enrollment and
     * the request is PB_BLE_MSG_START_REQ, which carries none (pb-status has
     * NO F_JOINER_UNAVAILABLE then: PB_BLE_MSG_START_EP_REQ works), or the
     * image's endpoint policy is unusable (no start is taken). Nothing was
     * started, queued or erased, and the challenge was not used up. */
    PB_BLE_CODE_UNAVAILABLE = 0x1b,
    PB_BLE_CODE_INTERNAL = 0x1c,
    /* PB_BLE_MSG_START_EP_REQ only: unknown endpoint format, port 0, or an
     * address that is not a Thread Mesh-Local EID (unspecified, loopback,
     * IPv4-mapped/compatible, multicast, link-local, outside fd00::/8, or an
     * RLOC/ALOC). Nothing started. */
    PB_BLE_CODE_INVALID_ENDPOINT = 0x1d,
    /* PB_BLE_MSG_START_MGMT_REQ only: unknown management format, or a hub
     * id, generation or key made of zeros. Nothing started. */
    PB_BLE_CODE_INVALID_MANAGEMENT = 0x1e,
    /* PB_BLE_MSG_LOCAL_RESET_REQ only: no physical re-enrollment window is
     * open -- none was opened in this boot (the BOOT button was not held),
     * or its time is over. Nothing changed; the challenge was not used up. */
    PB_BLE_CODE_WINDOW_CLOSED = 0x1f,
    /* PB_BLE_MSG_LOCAL_RESET_REQ only: this board holds no enrollment it
     * would resume, so there is nothing to reset. Nothing changed. */
    PB_BLE_CODE_NOT_ENROLLED = 0x20,
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
