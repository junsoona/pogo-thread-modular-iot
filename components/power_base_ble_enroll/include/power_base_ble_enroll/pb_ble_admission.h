/*
 * Admission rules for the BLE enrollment transport (pb_thread:mode=2) --
 * see docs/power-base.md "BLE 등록 전달 경로" and pb_ble_proto.h.
 *
 * This file decides; it never touches BLE, OpenThread or NVS. The Joiner,
 * the completion marker and the service start stay where they were:
 * components/power_base_enrollment, reached only through
 * pb_ble_worker_ops_t. The same code runs on the board and, unmodified, in
 * tools/test_power_base_ble_admission.c.
 *
 * What is decided here
 *   - which pb-status / pb-start / pb-cancel messages are well formed;
 *   - the challenge: 16 random bytes issued by pb-status, bound to the
 *     Security 2 session it was issued in, valid PB_BLE_CHALLENGE_TTL_MS,
 *     usable once;
 *   - the single attempt record: at most one BLE-started attempt exists; a
 *     second request cannot replace it, a retransmission of the same request
 *     does not start a second Joiner;
 *   - what reaches the enrollment worker: a bounded queue of
 *     PB_BLE_CMD_QUEUE_LEN commands, each tagged with the attempt generation
 *     it belongs to. A full queue is answered PB_BLE_CODE_BUSY, never
 *     "accepted";
 *   - when BLE enrollment closes for this boot (enrolled, too many failed
 *     authentications, advertising window over).
 *
 * Two kinds of caller
 *   BLE side (NimBLE host task, inside protocomm's callbacks):
 *     pb_ble_adm_session_changed(), pb_ble_adm_sec_request_begin/end(),
 *     pb_ble_adm_handle_status/start/cancel(). These only read and write
 *     this struct: no OpenThread call, no NVS write, no BLE shutdown, no
 *     blocking.
 *   Worker side (the enrollment task): pb_ble_adm_worker_poll(). It takes
 *     commands out of the queue and calls the real enrollment controller
 *     through pb_ble_worker_ops_t with the struct lock RELEASED.
 *
 * Locking: none inside. The owner serialises the BLE-side calls with one
 * leaf mutex and hands pb_ble_adm_worker_poll() lock/unlock hooks for the
 * same mutex. Nothing is called with that mutex held except the two
 * pb_ble_adm_ops_t primitives (random bytes, SHA-256).
 *
 * Secrets: the PSKd lives in exactly one slot here from pb-start until the
 * worker takes it (or the request is cancelled / the queue was full); the
 * slot and the caller's decrypted request buffer are wiped on every path.
 * While an attempt is queued or running, a SHA-256 over the request
 * (domain | message type | target | challenge | PSKd | bridge endpoint) is
 * kept to recognise a retransmission; it is wiped when the attempt ends.
 * Nothing here logs a PSKd, a challenge, a request id or a bridge address.
 *
 * Endpoint policy (pb_enroll_endpoint_policy_t, from the prepared image):
 * with REQUIRED, PB_BLE_MSG_START_REQ -- the start without an endpoint -- is
 * answered PB_BLE_CODE_UNAVAILABLE as soon as it is known to be a well-formed
 * start for an open board; with INVALID every start is. That happens before
 * the request digest, the request-id lookup and the challenge: nothing is
 * queued, no attempt record is made, the challenge of the session stays
 * usable, and a retransmission gets the same answer. A PB_BLE_MSG_START_EP_REQ
 * sent afterwards -- with the same challenge and even the same request id --
 * is judged as if the refused message had never arrived. The controller
 * applies the same policy again when the worker reaches it.
 *
 * Management credential (PB_BLE_MSG_START_MGMT_REQ): a start that carries
 * the bridge endpoint (judged exactly like PB_BLE_MSG_START_EP_REQ, also by
 * the endpoint policy) and the hub's id, enrollment generation and
 * management key. The credential is a secret like the PSKd: it sits in the
 * slot next to it, leaves with it, and is wiped wherever the PSKd is. The
 * request digest covers it, so the same request id with another generation
 * or key is a conflict, never a duplicate.
 *
 * Bridge endpoint (PB_BLE_MSG_START_EP_REQ): checked here, in the BLE
 * callback, against the same rules the controller applies
 * (pb_enroll_bridge_endpoint_check()); the checked copy travels to the worker
 * in the slot next to the PSKd and is dropped with it. The BLE side never
 * writes it to NVS and never calls OpenThread with it; storing it is the
 * controller's job, after the board has attached.
 *
 * Local reset (PB_BLE_MSG_LOCAL_RESET_REQ / PB_BLE_MSG_LOCAL_RESET_STATUS_REQ,
 * docs/power-base.md "BOOT 버튼 로컬 재등록"): only on a board that has the
 * re-enrollment button (pb_ble_adm_set_local_reset()); on any other board
 * both messages are unknown types, answered as before. The command is judged
 * here, in the BLE callback -- target, challenge, confirmation byte, the
 * open window, what the board holds, the one operation it remembers -- and
 * then only queued: the journal, the erase and its verification are the
 * worker's (pb_ble_worker_ops_t.local_reset), with the struct lock released.
 * A window boot (`window` = true) is a boot in which the old enrollment was
 * deliberately not resumed: every pb-start is refused with
 * PB_BLE_CODE_NOT_WAITING, the board being enrolled does not close BLE, and
 * the window closes PB_BLE_REENROLL_WINDOW_MS after it opened, connected or
 * not -- unless a reset was accepted, which is then carried to its end.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_ble_enroll/pb_ble_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Advertising / enrollment window, counted from pb_ble_adm_init(). When it
 * ends BLE enrollment closes until the next power cycle. An attempt already
 * queued or running keeps BLE open until it ends. */
#define PB_BLE_ADV_WINDOW_MS (15u * 60u * 1000u)
/* A challenge is valid this long after pb-status issued it. */
#define PB_BLE_CHALLENGE_TTL_MS (60u * 1000u)
/* Security 2 handshake messages that ended in an error (wrong setup code,
 * malformed or downgraded session data). At this count BLE enrollment
 * closes until the next power cycle. */
#define PB_BLE_MAX_AUTH_FAILURES 5u
/* All Security 2 handshake messages, good or bad (a complete handshake is
 * two). Bounds how much SRP work one boot will do for strangers. */
#define PB_BLE_MAX_SEC_REQUESTS 40u
/* Commands waiting for the enrollment worker. */
#define PB_BLE_CMD_QUEUE_LEN 4
/* The re-enrollment window, counted from pb_ble_adm_set_local_reset(). */
#define PB_BLE_REENROLL_WINDOW_MS (120u * 1000u)
/* After a local reset ended, BLE stays up this long so that a client that
 * is still connected can read the result; then the board restarts. */
#define PB_BLE_LR_RESULT_GRACE_MS 3000u

#define PB_BLE_SHA256_LEN 32

typedef enum {
    PB_BLE_OPEN = 0,
    PB_BLE_CLOSE_ENROLLED,       /* the board is enrolled */
    PB_BLE_CLOSE_AUTH_LOCKED,    /* PB_BLE_MAX_AUTH_FAILURES or PB_BLE_MAX_SEC_REQUESTS reached */
    PB_BLE_CLOSE_WINDOW_EXPIRED, /* PB_BLE_ADV_WINDOW_MS passed with no attempt in progress */
    /* Window boot only: a local reset ended (verified or not) and its grace
     * period is over. The board restarts. */
    PB_BLE_CLOSE_LOCAL_RESET_DONE,
} pb_ble_close_reason_t;

const char *pb_ble_close_reason_name(pb_ble_close_reason_t reason);

typedef struct {
    /* Fills buf with cryptographically strong random bytes. */
    bool (*random)(uint8_t *buf, size_t len, void *ctx);
    bool (*sha256)(const uint8_t *data, size_t len, uint8_t out[PB_BLE_SHA256_LEN], void *ctx);
    void *ctx;
} pb_ble_adm_ops_t;

typedef struct {
    pb_enroll_state_t state;
    pb_enroll_result_t last;
    uint32_t attempt;
} pb_ble_ctl_status_t;

/* What a local reset came to: pb_reenroll_reset_result_t. */
#define PB_BLE_LR_RESULT_NOT_STARTED 0u
#define PB_BLE_LR_RESULT_INCOMPLETE 1u
#define PB_BLE_LR_RESULT_VERIFIED 2u

/* What the board holds, as the worker read it from flash. */
typedef struct {
    uint8_t condition; /* PB_BLE_LR_COND_* */
    bool journal_valid;
    bool journal_verified; /* false = approved, not finished */
    uint8_t journal_op[PB_BLE_OP_ID_LEN];
} pb_ble_lr_snapshot_t;

/* The real enrollment controller, as the worker reaches it. */
typedef struct {
    /* pb_enrollment_request_join(): `pskd` is wiped before it returns.
     * `endpoint` is NULL for a version 1 start (nothing delivered), else the
     * checked endpoint of a PB_BLE_MSG_START_EP_REQ; only valid during the
     * call. `mgmt` is NULL unless the start was a PB_BLE_MSG_START_MGMT_REQ
     * (then `endpoint` is never NULL); the worker wipes its copy after the
     * call. */
    esp_err_t (*request_join)(char pskd[PB_ENROLL_PSKD_BUF_LEN], const pb_enroll_bridge_endpoint_t *endpoint,
                              const pb_enroll_mgmt_credential_t *mgmt, void *ctx);
    esp_err_t (*cancel)(void *ctx);
    void (*get_status)(pb_ble_ctl_status_t *out, void *ctx);
    /* The owner's leaf mutex; both may be NULL in a single-threaded test. */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
    /* Local reset; both NULL on a board without it. `local_reset` journals,
     * erases and verifies (pb_reenroll_reset_begin()) and returns
     * PB_BLE_LR_RESULT_*, with a PB_BLE_LR_REASON_* in *reason.
     * `get_local_reset` reads the journal and the record. Both write or read
     * flash: called with the struct lock released, never from a BLE
     * callback. */
    uint8_t (*local_reset)(const uint8_t op_id[PB_BLE_OP_ID_LEN], uint8_t *reason, void *ctx);
    void (*get_local_reset)(pb_ble_lr_snapshot_t *out, void *ctx);
} pb_ble_worker_ops_t;

typedef enum {
    PB_BLE_CMD_START = 1,
    PB_BLE_CMD_CANCEL = 2,
    PB_BLE_CMD_LOCAL_RESET = 3,
} pb_ble_cmd_kind_t;

typedef struct {
    uint8_t kind;
    uint32_t gen;
} pb_ble_cmd_t;

/* Internal phases; the wire sees pb_ble_phase_t (STARTING is shown as QUEUED). */
typedef enum {
    PB_BLE_REC_NONE = 0,
    PB_BLE_REC_QUEUED,
    PB_BLE_REC_STARTING, /* the worker holds the PSKd and is inside request_join() */
    PB_BLE_REC_RUNNING,
    PB_BLE_REC_FAILED,
    PB_BLE_REC_ENROLLED,
    PB_BLE_REC_CANCELLED,
} pb_ble_rec_phase_t;

/* The one local reset operation this struct remembers in RAM. Once the
 * worker has run it, the journal in flash is what says how far it got. */
typedef enum {
    PB_BLE_LR_REC_NONE = 0,
    PB_BLE_LR_REC_QUEUED,
    PB_BLE_LR_REC_RUNNING,     /* the worker is inside local_reset() */
    PB_BLE_LR_REC_ENDED,       /* journaled: see the snapshot */
    PB_BLE_LR_REC_NOT_STARTED, /* ended before anything was changed */
} pb_ble_lr_rec_phase_t;

typedef struct {
    pb_ble_adm_ops_t ops;
    uint8_t eui64[PB_BLE_EUI64_LEN];
    bool joiner_eui64_ok;
    /* OPTIONAL after pb_ble_adm_init(). */
    pb_enroll_endpoint_policy_t endpoint_policy;

    /* Bumped whenever the Security 2 session changes: transport session
     * opened or closed, and every handshake message. A challenge is only
     * good in the epoch it was issued in. */
    uint32_t epoch;

    pb_ble_close_reason_t closed;
    bool auth_locked;
    uint32_t sec_requests;
    uint32_t auth_failures;
    uint32_t window_deadline_ms;

    bool ch_valid;
    uint32_t ch_epoch;
    uint32_t ch_issued_ms;
    uint8_t ch[PB_BLE_CHALLENGE_LEN];

    /* The one attempt record. */
    pb_ble_rec_phase_t rec_phase;
    uint32_t rec_gen;        /* bumped for every accepted pb-start */
    uint32_t rec_ctl_attempt; /* controller attempt number once RUNNING */
    uint8_t rec_result;
    bool rec_cancel_requested;
    uint8_t rec_request_id[PB_BLE_REQUEST_ID_LEN];
    uint8_t rec_tag[PB_BLE_SHA256_LEN];

    /* The PSKd of a QUEUED attempt, and the bridge endpoint that came with
     * it (PB_BLE_MSG_START_EP_REQ only). Both leave together. */
    bool pskd_pending;
    char pskd[PB_ENROLL_PSKD_BUF_LEN];
    bool endpoint_pending;
    pb_enroll_bridge_endpoint_t endpoint;
    /* The management credential that came with it
     * (PB_BLE_MSG_START_MGMT_REQ only). A secret; leaves with the PSKd. */
    bool mgmt_pending;
    pb_enroll_mgmt_credential_t mgmt;

    /* Bounded command queue to the worker. */
    pb_ble_cmd_t q[PB_BLE_CMD_QUEUE_LEN];
    uint8_t q_head;
    uint8_t q_count;

    /* Last controller status the worker published. */
    pb_ble_ctl_status_t ctl;

    /* ---- local reset; all zero on a board without it ---- */
    bool lr_supported; /* the two local reset messages are answered */
    bool lr_window;    /* this boot is a re-enrollment window boot */
    /* What the worker last read from flash, and whether it must read again. */
    pb_ble_lr_snapshot_t lr_snap;
    bool lr_refresh;
    pb_ble_lr_rec_phase_t lr_phase;
    uint32_t lr_gen; /* bumped for every accepted reset command */
    uint8_t lr_reason;
    uint8_t lr_op[PB_BLE_OP_ID_LEN];
    /* Window boot: a reset was run to its end (verified or not); BLE closes
     * at lr_end_deadline_ms. */
    bool lr_ended;
    uint32_t lr_end_deadline_ms;
} pb_ble_adm_t;

/* `joiner_eui64_ok`: OpenThread's factory EUI-64 equals `eui64`. */
void pb_ble_adm_init(pb_ble_adm_t *a, const pb_ble_adm_ops_t *ops, const uint8_t eui64[PB_BLE_EUI64_LEN],
                     bool joiner_eui64_ok, uint32_t now_ms);

/* What the prepared image says about a start without a bridge endpoint. Set
 * once, right after pb_ble_adm_init() and before any transport is started. */
void pb_ble_adm_set_endpoint_policy(pb_ble_adm_t *a, pb_enroll_endpoint_policy_t policy);

/* Makes this a board that answers the local reset messages (`supported`),
 * and, with `window`, a re-enrollment window boot whose window opens now.
 * Set once, right after pb_ble_adm_init() and before any transport is
 * started. Without this call the board behaves exactly as before. */
void pb_ble_adm_set_local_reset(pb_ble_adm_t *a, bool supported, bool window, uint32_t now_ms);

/* ---- BLE side ---- */

/* A transport session was opened or closed (connect / disconnect). Drops the
 * challenge. An attempt already accepted is NOT touched. */
void pb_ble_adm_session_changed(pb_ble_adm_t *a);

/* Around every Security 2 handshake message. begin() returns false when the
 * message must not be handed to the SDK at all (closed or locked). */
bool pb_ble_adm_sec_request_begin(pb_ble_adm_t *a);
void pb_ble_adm_sec_request_end(pb_ble_adm_t *a, bool ok);

/* Each writes the endpoint's response into `out` and returns its length
 * (PB_BLE_STATUS_RESP_LEN or PB_BLE_SHORT_RESP_LEN). `in` is the decrypted
 * request; handle_start() overwrites it with zeros before returning.
 * handle_start() takes both start messages and answers each with its own
 * response type. */
size_t pb_ble_adm_handle_status(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint8_t out[PB_BLE_STATUS_RESP_LEN],
                                uint32_t now_ms);
/* PB_BLE_MSG_RECEIPT_REQ on "pb-status": the board's release receipt
 * (pb_ble_proto.h). `source` fills `receipt` and returns true when the board
 * has one; NULL or false answers "none". Pure: reads only `eui64`, changes
 * nothing, needs no lock -- the caller must NOT hold its struct lock around
 * it, because `source` reads flash. Returns PB_BLE_RECEIPT_RESP_LEN. */
typedef bool (*pb_ble_receipt_source_t)(uint8_t receipt[PB_BLE_RECEIPT_LEN], void *ctx);
size_t pb_ble_adm_handle_receipt(const uint8_t eui64[PB_BLE_EUI64_LEN], const uint8_t *in, size_t inlen,
                                 uint8_t out[PB_BLE_RECEIPT_RESP_LEN], pb_ble_receipt_source_t source, void *ctx);
/* True when a request on "pb-status" is a PB_BLE_MSG_RECEIPT_REQ of this
 * protocol version (so the transport picks the handler and the response
 * size); everything else goes to pb_ble_adm_handle_status() as before. */
bool pb_ble_adm_is_receipt_request(const uint8_t *in, size_t inlen);
/* True when a request on "pb-status" is a PB_BLE_MSG_LOCAL_RESET_STATUS_REQ
 * of this protocol version AND this board answers it; on a board without
 * local reset it is false, so the request goes to pb_ble_adm_handle_status()
 * like any other unknown type. */
bool pb_ble_adm_is_local_reset_status_request(const pb_ble_adm_t *a, const uint8_t *in, size_t inlen);
/* PB_BLE_MSG_LOCAL_RESET_STATUS_REQ. Reads this struct only (what the worker
 * published); changes nothing. Returns PB_BLE_LOCAL_RESET_STATUS_RESP_LEN. */
size_t pb_ble_adm_handle_local_reset_status(pb_ble_adm_t *a, const uint8_t *in, size_t inlen,
                                            uint8_t out[PB_BLE_LOCAL_RESET_STATUS_RESP_LEN], uint32_t now_ms);
/* Also takes PB_BLE_MSG_LOCAL_RESET_REQ on a board with local reset, and
 * answers it with PB_BLE_MSG_LOCAL_RESET_RESP. */
size_t pb_ble_adm_handle_start(pb_ble_adm_t *a, uint8_t *in, size_t inlen, uint8_t out[PB_BLE_SHORT_RESP_LEN],
                               uint32_t now_ms);
size_t pb_ble_adm_handle_cancel(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint8_t out[PB_BLE_SHORT_RESP_LEN]);

/* ---- worker side ---- */

/*
 * One pass of the enrollment worker: runs every queued command against the
 * controller, publishes the controller's status, applies the close rules.
 * Returns PB_BLE_OPEN, or why BLE must be shut down now; the caller then
 * stops the BLE transport (never from inside a BLE callback). Keep calling
 * it after a close: a start accepted just before the close is still run.
 */
pb_ble_close_reason_t pb_ble_adm_worker_poll(pb_ble_adm_t *a, const pb_ble_worker_ops_t *w, uint32_t now_ms);

/* True while a BLE-started attempt is queued or running. */
bool pb_ble_adm_attempt_active(const pb_ble_adm_t *a);
/* True while a local reset is queued or running. */
bool pb_ble_adm_local_reset_active(const pb_ble_adm_t *a);

/* Wipes every secret held here (challenge, PSKd slot, management credential,
 * request digest) and the queued bridge endpoint. */
void pb_ble_adm_wipe(pb_ble_adm_t *a);

#ifdef __cplusplus
}
#endif
