/*
 * Local re-enrollment of a Power Base (pb_thread:mode=2 + BLE build) -- the
 * decisions. See docs/power-base.md "BOOT 버튼 로컬 재등록".
 *
 * Why it exists: a board whose hub is gone, or that has to move to another
 * Thread network, cannot be told to unenroll by that hub
 * (docs/power-base-management.md). Its owner can still reset it: hold the
 * BOOT button, authenticate with the board's own setup code over BLE, and
 * confirm. Nothing else resets it -- not the button, not a BLE connection,
 * not a successful authentication.
 *
 * This component decides and stores; it never calls OpenThread, BLE, GPIO or
 * FreeRTOS. power_base/main supplies those through pb_reenroll_ops_t, so the
 * same code runs unmodified in tools/test_power_base_reenroll.c against the
 * NVS simulator.
 *
 * How the window is opened -- by a restart, not on top of a running board:
 *
 *   normal boot, button held 5 s and released
 *     -> pb_reenroll_gate() (nothing else under way)
 *     -> pb_reenroll_window_store()  one-shot request, committed + read back
 *     -> esp_restart()
 *   next boot: pb_reenroll_boot()
 *     -> the request is consumed (erased, read back) BEFORE anything opens
 *     -> PB_REENROLL_BOOT_WINDOW: Thread stays disabled, the enrollment is
 *        NOT resumed, no service and no management poll is started; only the
 *        BLE service runs, for PB_REENROLL_WINDOW_MS
 *   the window ends without an accepted reset (time, wrong setup code,
 *   disconnect, power cut)
 *     -> restart -> an ordinary boot: the record and the Dataset are exactly
 *        what they were, so the enrollment is resumed by the usual boot rule
 *
 * So "pausing" the old enrollment is simply not resuming it for one boot,
 * and "restoring" it is the ordinary boot path. A window by itself writes
 * nothing but its own request record.
 *
 * The reset (pb_reenroll_reset_begin / pb_reenroll_reset_resume), in the one
 * order that survives a power cut at any point:
 *
 *   1. journal APPROVED       committed and read back; a failure here ends
 *                             it with nothing changed
 *   2. quiesce                Joiner and Thread stopped
 *   3. enrollment record      erased and read back absent (marker, bridge
 *                             endpoint, management credential). A final
 *                             release record -- a remote unenroll's receipt,
 *                             which holds no network, endpoint or key -- is
 *                             kept
 *   4. OpenThread's store     erased, then read back empty
 *   5. journal VERIFIED       committed and read back
 *
 * From step 1 on, no boot resumes the old enrollment: pb_reenroll_boot()
 * carries on with the reset first, before anything else looks at the
 * enrollment record. A step that fails leaves the journal APPROVED; the next
 * boot tries again. A journal that cannot be read, is damaged or names
 * another board stops the board (PB_REENROLL_BOOT_SAFE_STOP): Thread off, no
 * resume, no new join -- until a new authenticated, confirmed reset replaces
 * it.
 *
 * What this never does: it never erases the "nvs" partition as a whole,
 * never touches "cfg" (wiring, identity, the BLE setup credential), never
 * touches a sensor module, and never makes a release receipt -- a local
 * reset is not a remote unenroll and tells the old hub nothing.
 *
 * Priority against a remote unenroll: a release that is under way
 * (record UNENROLLING, or RELEASED with the erase pending) comes first. The
 * window is not opened for such a board and a reset command is refused; a
 * reset that was journaled comes before everything.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_reenroll/pb_reenroll_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long the re-enrollment window stays open. Counted from its start;
 * connecting or authenticating does not extend it. */
#define PB_REENROLL_WINDOW_MS (120u * 1000u)

/* What the board holds, as the reset sees it. The numbers are on the BLE
 * wire (pb_ble_proto.h, PB_BLE_LR_COND_*). */
typedef enum {
    /* No enrollment this board would resume: no record, a damaged or
     * foreign one (never resumed; the next accepted join clears it), or a
     * final release. Nothing to reset. */
    PB_REENROLL_COND_NOT_ENROLLED = 0,
    /* A record this board resumes from: version 1, 2, or 3 in state ENROLLED. */
    PB_REENROLL_COND_ENROLLED = 1,
    /* A remote unenroll is under way. It comes first. */
    PB_REENROLL_COND_REMOTE_RELEASE_PENDING = 2,
    /* A journaled local reset is not finished. */
    PB_REENROLL_COND_RESET_PENDING = 3,
    /* The reset journal or the enrollment record could not be read, or the
     * journal is damaged or foreign. */
    PB_REENROLL_COND_UNREADABLE = 4,
} pb_reenroll_condition_t;

const char *pb_reenroll_condition_name(pb_reenroll_condition_t condition);

typedef struct {
    pb_reenroll_condition_t condition;
    pb_reenroll_rec_status_t journal_status;
    pb_reenroll_journal_t journal; /* meaningful when journal_status is VALID */
    pb_enroll_marker_status_t record_status;
} pb_reenroll_snapshot_t;

/* Reads the journal and the enrollment record. Never writes; holds no secret
 * afterwards. */
void pb_reenroll_snapshot(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_reenroll_snapshot_t *out);

/* ---- may the button open a window now? ---- */

typedef struct {
    /* The build has the feature and the button GPIO passed its check. */
    bool feature_enabled;
    /* A Joiner is running or attaching, or a BLE start is queued. */
    bool enrollment_busy;
    /* An authenticated unenroll command is being carried out. */
    bool remote_unenroll_active;
    /* A local reset is queued, running or journaled. */
    bool local_reset_active;
    /* This boot already wrote a window request (the restart is pending). */
    bool already_requested;
} pb_reenroll_gate_input_t;

typedef enum {
    PB_REENROLL_GATE_OK = 0,
    PB_REENROLL_GATE_DISABLED,
    PB_REENROLL_GATE_ENROLLMENT_BUSY,
    PB_REENROLL_GATE_REMOTE_UNENROLL,
    PB_REENROLL_GATE_LOCAL_RESET,
    PB_REENROLL_GATE_ALREADY_REQUESTED,
} pb_reenroll_gate_t;

pb_reenroll_gate_t pb_reenroll_gate(const pb_reenroll_gate_input_t *in);
const char *pb_reenroll_gate_name(pb_reenroll_gate_t gate);

/* ---- the reset ---- */

typedef struct {
    /* Stop the Joiner and Thread. Nothing else runs in a window boot. */
    void (*quiesce)(void *ctx);
    /* Disable Thread and erase OpenThread's persistent store. ESP_OK = the
     * calls were made without an error; that alone is not "erased". */
    esp_err_t (*network_erase)(void *ctx);
    /* Reads OpenThread's store back. True only when it holds nothing. */
    bool (*network_erased)(void *ctx);
    void *ctx;
} pb_reenroll_ops_t;

typedef enum {
    /* Nothing was changed by this call: the journal could not be stored, or
     * there was nothing this board may reset. */
    PB_REENROLL_RESET_NOT_STARTED = 0,
    /* Journaled, and a step failed. The journal says APPROVED: the next boot
     * carries on and never resumes the old enrollment. */
    PB_REENROLL_RESET_INCOMPLETE = 1,
    /* The record and OpenThread's store were read back empty and the journal
     * says VERIFIED. */
    PB_REENROLL_RESET_VERIFIED = 2,
} pb_reenroll_reset_result_t;

/* Why a reset is where it is. The numbers are on the BLE wire
 * (pb_ble_proto.h, PB_BLE_LR_REASON_*). */
typedef enum {
    PB_REENROLL_REASON_NONE = 0,
    PB_REENROLL_REASON_JOURNAL_STORE_FAILED = 1, /* nothing was changed */
    PB_REENROLL_REASON_NOT_ENROLLED = 2,         /* nothing to reset */
    PB_REENROLL_REASON_REMOTE_RELEASE_PENDING = 3,
    PB_REENROLL_REASON_OTHER_RESET_PENDING = 4,  /* another operation is journaled */
    PB_REENROLL_REASON_RECORD_CLEAR_FAILED = 5,
    PB_REENROLL_REASON_RECORD_STILL_PRESENT = 6,
    PB_REENROLL_REASON_NETWORK_ERASE_FAILED = 7,
    PB_REENROLL_REASON_NETWORK_NOT_EMPTY = 8,
    PB_REENROLL_REASON_JOURNAL_FINAL_FAILED = 9,
} pb_reenroll_reason_t;

const char *pb_reenroll_reason_name(pb_reenroll_reason_t reason);

/*
 * An authenticated, user-confirmed reset command for operation `op_id`
 * (worker context: it writes flash). Allowed when the board holds an
 * enrollment it would resume, or when its journal is unreadable (then this
 * operation replaces that journal); also carries on a journaled reset with
 * the same operation id. *why is always set.
 */
pb_reenroll_reset_result_t pb_reenroll_reset_begin(const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                                                   const uint8_t op_id[PB_REENROLL_OP_ID_LEN],
                                                   const pb_reenroll_ops_t *ops, pb_reenroll_reason_t *why);

/* ---- boot ---- */

typedef enum {
    /* Carry on with the ordinary mode 2 boot (pending remote release, boot
     * rule, safe wait). */
    PB_REENROLL_BOOT_NORMAL = 0,
    /* Open the re-enrollment window. The enrollment is not resumed in this
     * boot and nothing else is started. */
    PB_REENROLL_BOOT_WINDOW,
    /* A journaled reset was finished in this call: restart the chip
     * (OpenThread still holds in RAM what it loaded at init). */
    PB_REENROLL_BOOT_RESTART,
    /* Thread off, no resume, no join, no BLE enrollment. Only the button
     * (when the feature is there) can lead on from here. */
    PB_REENROLL_BOOT_SAFE_STOP,
} pb_reenroll_boot_action_t;

const char *pb_reenroll_boot_action_name(pb_reenroll_boot_action_t action);

typedef struct {
    pb_reenroll_boot_action_t action;
    pb_reenroll_rec_status_t journal_status; /* as found, before this boot did anything */
    /* A window request was found (whatever it held) and its erase was read
     * back. */
    bool window_consumed;
    /* WINDOW only: the journal is unreadable; the window is there so that a
     * new confirmed reset can replace it. */
    bool recovery;
    /* What the journaled reset came to, when one was carried on. */
    pb_reenroll_reset_result_t reset_result;
    pb_reenroll_reason_t reason;
    /* The board as it is after this call. */
    pb_reenroll_condition_t condition;
} pb_reenroll_boot_t;

/*
 * Mode 2 boot, after thread_manager_init() + thread_manager_stop() and
 * before anything looks at the enrollment record. In this order:
 *   1. a journaled reset is carried on (in every build, whether or not the
 *      feature is enabled: what was started is finished);
 *   2. an unreadable journal stops the board;
 *   3. a window request is consumed, and opens the window only when the
 *      feature is enabled, the request is this board's, and there is an
 *      enrollment to reset (or the journal to repair).
 * A board that never used the feature has no "pb_reenroll" namespace: this
 * then reads nothing, writes nothing and returns NORMAL.
 */
void pb_reenroll_boot(const uint8_t eui64[PB_ENROLL_EUI64_LEN], bool feature_enabled, const pb_reenroll_ops_t *ops,
                      pb_reenroll_boot_t *out);

#ifdef __cplusplus
}
#endif
