#include "power_base_reenroll/pb_reenroll.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "pb_reenroll";

const char *pb_reenroll_condition_name(pb_reenroll_condition_t condition)
{
    switch (condition) {
    case PB_REENROLL_COND_NOT_ENROLLED: return "not_enrolled";
    case PB_REENROLL_COND_ENROLLED: return "enrolled";
    case PB_REENROLL_COND_REMOTE_RELEASE_PENDING: return "remote_release_pending";
    case PB_REENROLL_COND_RESET_PENDING: return "reset_pending";
    case PB_REENROLL_COND_UNREADABLE: return "unreadable";
    }
    return "unknown";
}

const char *pb_reenroll_reason_name(pb_reenroll_reason_t reason)
{
    switch (reason) {
    case PB_REENROLL_REASON_NONE: return "none";
    case PB_REENROLL_REASON_JOURNAL_STORE_FAILED: return "journal_store_failed";
    case PB_REENROLL_REASON_NOT_ENROLLED: return "not_enrolled";
    case PB_REENROLL_REASON_REMOTE_RELEASE_PENDING: return "remote_release_pending";
    case PB_REENROLL_REASON_OTHER_RESET_PENDING: return "other_reset_pending";
    case PB_REENROLL_REASON_RECORD_CLEAR_FAILED: return "record_clear_failed";
    case PB_REENROLL_REASON_RECORD_STILL_PRESENT: return "record_still_present";
    case PB_REENROLL_REASON_NETWORK_ERASE_FAILED: return "network_erase_failed";
    case PB_REENROLL_REASON_NETWORK_NOT_EMPTY: return "network_not_empty";
    case PB_REENROLL_REASON_JOURNAL_FINAL_FAILED: return "journal_final_failed";
    }
    return "unknown";
}

const char *pb_reenroll_gate_name(pb_reenroll_gate_t gate)
{
    switch (gate) {
    case PB_REENROLL_GATE_OK: return "ok";
    case PB_REENROLL_GATE_DISABLED: return "disabled";
    case PB_REENROLL_GATE_ENROLLMENT_BUSY: return "enrollment_busy";
    case PB_REENROLL_GATE_REMOTE_UNENROLL: return "remote_unenroll";
    case PB_REENROLL_GATE_LOCAL_RESET: return "local_reset";
    case PB_REENROLL_GATE_ALREADY_REQUESTED: return "already_requested";
    }
    return "unknown";
}

const char *pb_reenroll_boot_action_name(pb_reenroll_boot_action_t action)
{
    switch (action) {
    case PB_REENROLL_BOOT_NORMAL: return "normal";
    case PB_REENROLL_BOOT_WINDOW: return "window";
    case PB_REENROLL_BOOT_RESTART: return "restart";
    case PB_REENROLL_BOOT_SAFE_STOP: return "safe_stop";
    }
    return "unknown";
}

static bool journal_unreadable(pb_reenroll_rec_status_t status)
{
    return status == PB_REENROLL_REC_CORRUPT || status == PB_REENROLL_REC_FOREIGN ||
           status == PB_REENROLL_REC_READ_ERROR;
}

void pb_reenroll_snapshot(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_reenroll_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->journal_status = pb_reenroll_journal_load(eui64, &out->journal);

    pb_enroll_record_t rec;
    out->record_status = pb_enroll_record_load(eui64, &rec);
    pb_reenroll_condition_t of_record;
    switch (out->record_status) {
    case PB_ENROLL_MARKER_VALID:
        if (pb_enroll_record_is_enrolled(&rec)) {
            of_record = PB_REENROLL_COND_ENROLLED;
        } else if (pb_enroll_record_is_final_release(&rec)) {
            of_record = PB_REENROLL_COND_NOT_ENROLLED;
        } else {
            of_record = PB_REENROLL_COND_REMOTE_RELEASE_PENDING;
        }
        break;
    case PB_ENROLL_MARKER_READ_ERROR: of_record = PB_REENROLL_COND_UNREADABLE; break;
    case PB_ENROLL_MARKER_ABSENT:
    case PB_ENROLL_MARKER_CORRUPT:
    case PB_ENROLL_MARKER_FOREIGN:
    default: of_record = PB_REENROLL_COND_NOT_ENROLLED; break;
    }
    pb_enroll_wipe(&rec, sizeof(rec)); /* a version 3 record holds a key */

    if (journal_unreadable(out->journal_status)) {
        out->condition = PB_REENROLL_COND_UNREADABLE;
    } else if (out->journal_status == PB_REENROLL_REC_VALID && out->journal.stage == PB_REENROLL_STAGE_APPROVED) {
        out->condition = PB_REENROLL_COND_RESET_PENDING;
    } else {
        out->condition = of_record;
    }
}

pb_reenroll_gate_t pb_reenroll_gate(const pb_reenroll_gate_input_t *in)
{
    if (!in->feature_enabled) {
        return PB_REENROLL_GATE_DISABLED;
    }
    if (in->already_requested) {
        return PB_REENROLL_GATE_ALREADY_REQUESTED;
    }
    /* A remote unenroll that is being carried out comes first. */
    if (in->remote_unenroll_active) {
        return PB_REENROLL_GATE_REMOTE_UNENROLL;
    }
    if (in->local_reset_active) {
        return PB_REENROLL_GATE_LOCAL_RESET;
    }
    if (in->enrollment_busy) {
        return PB_REENROLL_GATE_ENROLLMENT_BUSY;
    }
    return PB_REENROLL_GATE_OK;
}

/* The enrollment record as the reset may leave it: gone, or a final release
 * (a remote unenroll's receipt: no network, no endpoint, no key). */
static bool record_is_clear(const uint8_t eui64[PB_ENROLL_EUI64_LEN], bool *is_final_release)
{
    pb_enroll_record_t rec;
    pb_enroll_marker_status_t status = pb_enroll_record_load(eui64, &rec);
    bool final_release = status == PB_ENROLL_MARKER_VALID && pb_enroll_record_is_final_release(&rec);
    pb_enroll_wipe(&rec, sizeof(rec));
    if (is_final_release != NULL) {
        *is_final_release = final_release;
    }
    return status == PB_ENROLL_MARKER_ABSENT || final_release;
}

/* Steps 2..5 (see pb_reenroll.h). The journal says APPROVED for `op_id`.
 * Every step can be repeated: a boot that carries on runs all of them. */
static pb_reenroll_reset_result_t run_steps(const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                                            const uint8_t op_id[PB_REENROLL_OP_ID_LEN], const pb_reenroll_ops_t *ops,
                                            pb_reenroll_reason_t *why)
{
    ops->quiesce(ops->ctx);

    bool final_release = false;
    (void)record_is_clear(eui64, &final_release);
    if (!final_release) {
        if (pb_enroll_marker_clear() != ESP_OK) {
            ESP_LOGE(TAG, "reset: the enrollment record could not be erased -- the next boot tries again");
            *why = PB_REENROLL_REASON_RECORD_CLEAR_FAILED;
            return PB_REENROLL_RESET_INCOMPLETE;
        }
    }
    if (!record_is_clear(eui64, NULL)) {
        ESP_LOGE(TAG, "reset: the enrollment record is still there after its erase -- the next boot tries again");
        *why = PB_REENROLL_REASON_RECORD_STILL_PRESENT;
        return PB_REENROLL_RESET_INCOMPLETE;
    }

    if (ops->network_erase(ops->ctx) != ESP_OK) {
        ESP_LOGE(TAG, "reset: the stored network could not be erased -- the next boot tries again");
        *why = PB_REENROLL_REASON_NETWORK_ERASE_FAILED;
        return PB_REENROLL_RESET_INCOMPLETE;
    }
    if (!ops->network_erased(ops->ctx)) {
        ESP_LOGE(TAG, "reset: the erase returned no error but the network store is not empty -- the next boot tries "
                      "again");
        *why = PB_REENROLL_REASON_NETWORK_NOT_EMPTY;
        return PB_REENROLL_RESET_INCOMPLETE;
    }
    /* Read once more, last: nothing above may have put it back. */
    if (!record_is_clear(eui64, NULL)) {
        *why = PB_REENROLL_REASON_RECORD_STILL_PRESENT;
        return PB_REENROLL_RESET_INCOMPLETE;
    }

    pb_reenroll_journal_t done;
    memset(&done, 0, sizeof(done));
    done.stage = PB_REENROLL_STAGE_VERIFIED;
    memcpy(done.op_id, op_id, PB_REENROLL_OP_ID_LEN);
    if (pb_reenroll_journal_store(eui64, &done) != ESP_OK) {
        ESP_LOGE(TAG, "reset: the journal could not be closed -- the next boot checks everything again");
        *why = PB_REENROLL_REASON_JOURNAL_FINAL_FAILED;
        return PB_REENROLL_RESET_INCOMPLETE;
    }
    *why = PB_REENROLL_REASON_NONE;
    ESP_LOGW(TAG, "reset verified: the enrollment record and the stored network are gone (cfg untouched); this "
                  "board is unenrolled%s",
             final_release ? "; the release receipt of an earlier remote unenroll was kept" : "");
    return PB_REENROLL_RESET_VERIFIED;
}

pb_reenroll_reset_result_t pb_reenroll_reset_begin(const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                                                   const uint8_t op_id[PB_REENROLL_OP_ID_LEN],
                                                   const pb_reenroll_ops_t *ops, pb_reenroll_reason_t *why)
{
    pb_reenroll_reason_t ignored;
    if (why == NULL) {
        why = &ignored;
    }
    *why = PB_REENROLL_REASON_NONE;

    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(eui64, &snap);

    if (snap.condition == PB_REENROLL_COND_RESET_PENDING) {
        if (memcmp(snap.journal.op_id, op_id, PB_REENROLL_OP_ID_LEN) != 0) {
            *why = PB_REENROLL_REASON_OTHER_RESET_PENDING;
            return PB_REENROLL_RESET_NOT_STARTED;
        }
        /* The same operation again: carried on, not started twice. */
        return run_steps(eui64, op_id, ops, why);
    }
    if (snap.condition == PB_REENROLL_COND_NOT_ENROLLED) {
        *why = PB_REENROLL_REASON_NOT_ENROLLED;
        return PB_REENROLL_RESET_NOT_STARTED;
    }
    if (snap.condition == PB_REENROLL_COND_REMOTE_RELEASE_PENDING) {
        *why = PB_REENROLL_REASON_REMOTE_RELEASE_PENDING;
        return PB_REENROLL_RESET_NOT_STARTED;
    }
    /* ENROLLED, or UNREADABLE (this operation then replaces what could not
     * be read). */

    pb_reenroll_journal_t approved;
    memset(&approved, 0, sizeof(approved));
    approved.stage = PB_REENROLL_STAGE_APPROVED;
    memcpy(approved.op_id, op_id, PB_REENROLL_OP_ID_LEN);
    if (pb_reenroll_journal_store(eui64, &approved) != ESP_OK) {
        /* Not stored, or not known to be: nothing is erased. If the failed
         * store left an APPROVED journal behind after all, it is taken back,
         * so that no later boot starts a reset this boot refused. A journal
         * that was unreadable before stays as it is: the board stays
         * stopped. */
        pb_reenroll_journal_t left;
        pb_reenroll_rec_status_t left_status = pb_reenroll_journal_load(eui64, &left);
        const bool left_approved = left_status == PB_REENROLL_REC_VALID && left.stage == PB_REENROLL_STAGE_APPROVED;
        if (left_approved || (journal_unreadable(left_status) && !journal_unreadable(snap.journal_status))) {
            if (pb_reenroll_journal_discard() != ESP_OK) {
                ESP_LOGE(TAG, "reset: the journal could not be stored and could not be taken back -- nothing was "
                              "erased now; the next boot acts on whatever the journal then says");
            }
        }
        ESP_LOGE(TAG, "reset: the approval could not be journaled -- not started, nothing erased");
        *why = PB_REENROLL_REASON_JOURNAL_STORE_FAILED;
        return PB_REENROLL_RESET_NOT_STARTED;
    }
    ESP_LOGW(TAG, "reset approved and journaled -- erasing this board's enrollment record and stored network");
    return run_steps(eui64, op_id, ops, why);
}

void pb_reenroll_boot(const uint8_t eui64[PB_ENROLL_EUI64_LEN], bool feature_enabled, const pb_reenroll_ops_t *ops,
                      pb_reenroll_boot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->action = PB_REENROLL_BOOT_NORMAL;
    out->reset_result = PB_REENROLL_RESET_NOT_STARTED;
    out->reason = PB_REENROLL_REASON_NONE;

    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(eui64, &snap);
    out->journal_status = snap.journal_status;

    /* 1. What was started is finished, before anything can resume. */
    if (snap.condition == PB_REENROLL_COND_RESET_PENDING) {
        ESP_LOGW(TAG, "boot: a local reset was journaled and not finished -- carrying on with it (the old "
                      "enrollment is not resumed)");
        out->reset_result = run_steps(eui64, snap.journal.op_id, ops, &out->reason);
        /* A window request has no place in such a boot. */
        if (pb_reenroll_window_load(eui64) != PB_REENROLL_REC_ABSENT) {
            out->window_consumed = pb_reenroll_window_consume() == ESP_OK;
        }
        out->action = out->reset_result == PB_REENROLL_RESET_VERIFIED ? PB_REENROLL_BOOT_RESTART
                                                                      : PB_REENROLL_BOOT_SAFE_STOP;
        if (out->action == PB_REENROLL_BOOT_SAFE_STOP) {
            ESP_LOGE(TAG, "boot: the local reset is NOT finished (%s) -- Thread stays disabled, nothing is resumed "
                          "and no join is accepted; tried again at the next boot",
                     pb_reenroll_reason_name(out->reason));
        }
        pb_reenroll_snapshot(eui64, &snap);
        out->condition = snap.condition;
        return;
    }

    /* A window request is used up before it can open anything, whatever it
     * holds and whatever follows. */
    const pb_reenroll_rec_status_t window = pb_reenroll_window_load(eui64);
    if (window != PB_REENROLL_REC_ABSENT) {
        out->window_consumed = pb_reenroll_window_consume() == ESP_OK;
        if (!out->window_consumed) {
            ESP_LOGE(TAG, "boot: the window request could not be erased -- no window is opened");
        }
    }
    const bool window_requested = window == PB_REENROLL_REC_VALID && out->window_consumed && feature_enabled;
    out->condition = snap.condition;

    /* 2. A journal that cannot be trusted stops the board. */
    if (journal_unreadable(snap.journal_status)) {
        if (window_requested) {
            out->action = PB_REENROLL_BOOT_WINDOW;
            out->recovery = true;
            ESP_LOGW(TAG, "boot: the reset journal is %s -- opening the re-enrollment window so that a new confirmed "
                          "reset can replace it (nothing is resumed)",
                     pb_reenroll_rec_status_name(snap.journal_status));
        } else {
            out->action = PB_REENROLL_BOOT_SAFE_STOP;
            ESP_LOGE(TAG, "boot: the reset journal is %s -- Thread stays disabled, nothing is resumed and no join is "
                          "accepted",
                     pb_reenroll_rec_status_name(snap.journal_status));
        }
        return;
    }

    /* 3. The window. */
    if (window != PB_REENROLL_REC_ABSENT && !window_requested) {
        ESP_LOGW(TAG, "boot: a window request was found (%s) and dropped%s", pb_reenroll_rec_status_name(window),
                 feature_enabled ? "" : " -- this build or board has no re-enrollment button");
    }
    if (!window_requested) {
        return;
    }
    if (snap.condition == PB_REENROLL_COND_ENROLLED) {
        out->action = PB_REENROLL_BOOT_WINDOW;
        ESP_LOGW(TAG, "boot: re-enrollment window requested by the button -- the enrollment is NOT resumed in this "
                      "boot; nothing is erased unless an authenticated reset is confirmed");
    } else if (snap.condition == PB_REENROLL_COND_UNREADABLE) {
        /* The enrollment record itself could not be read. */
        out->action = PB_REENROLL_BOOT_WINDOW;
        out->recovery = true;
        ESP_LOGW(TAG, "boot: re-enrollment window requested and the enrollment record cannot be read -- opening the "
                      "window");
    } else {
        ESP_LOGW(TAG, "boot: re-enrollment window requested, but there is nothing to reset here (%s) -- ordinary "
                      "boot",
                 pb_reenroll_condition_name(snap.condition));
    }
}
