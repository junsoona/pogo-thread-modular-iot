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
 * (domain | target | challenge | PSKd) is kept to recognise a retransmission;
 * it is wiped when the attempt ends. Nothing here logs a PSKd, a challenge
 * or a request id.
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

#define PB_BLE_SHA256_LEN 32

typedef enum {
    PB_BLE_OPEN = 0,
    PB_BLE_CLOSE_ENROLLED,       /* the board is enrolled */
    PB_BLE_CLOSE_AUTH_LOCKED,    /* PB_BLE_MAX_AUTH_FAILURES or PB_BLE_MAX_SEC_REQUESTS reached */
    PB_BLE_CLOSE_WINDOW_EXPIRED, /* PB_BLE_ADV_WINDOW_MS passed with no attempt in progress */
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

/* The real enrollment controller, as the worker reaches it. */
typedef struct {
    /* pb_enrollment_request_join(): `pskd` is wiped before it returns. */
    esp_err_t (*request_join)(char pskd[PB_ENROLL_PSKD_BUF_LEN], void *ctx);
    esp_err_t (*cancel)(void *ctx);
    void (*get_status)(pb_ble_ctl_status_t *out, void *ctx);
    /* The owner's leaf mutex; both may be NULL in a single-threaded test. */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} pb_ble_worker_ops_t;

typedef enum {
    PB_BLE_CMD_START = 1,
    PB_BLE_CMD_CANCEL = 2,
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

typedef struct {
    pb_ble_adm_ops_t ops;
    uint8_t eui64[PB_BLE_EUI64_LEN];
    bool joiner_eui64_ok;

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

    /* The PSKd of a QUEUED attempt. */
    bool pskd_pending;
    char pskd[PB_ENROLL_PSKD_BUF_LEN];

    /* Bounded command queue to the worker. */
    pb_ble_cmd_t q[PB_BLE_CMD_QUEUE_LEN];
    uint8_t q_head;
    uint8_t q_count;

    /* Last controller status the worker published. */
    pb_ble_ctl_status_t ctl;
} pb_ble_adm_t;

/* `joiner_eui64_ok`: OpenThread's factory EUI-64 equals `eui64`. */
void pb_ble_adm_init(pb_ble_adm_t *a, const pb_ble_adm_ops_t *ops, const uint8_t eui64[PB_BLE_EUI64_LEN],
                     bool joiner_eui64_ok, uint32_t now_ms);

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
 * request; handle_start() overwrites it with zeros before returning. */
size_t pb_ble_adm_handle_status(pb_ble_adm_t *a, const uint8_t *in, size_t inlen, uint8_t out[PB_BLE_STATUS_RESP_LEN],
                                uint32_t now_ms);
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

/* Wipes every secret held here (challenge, PSKd slot, request digest). */
void pb_ble_adm_wipe(pb_ble_adm_t *a);

#ifdef __cplusplus
}
#endif
