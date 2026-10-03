/*
 * Power Base enrollment state for pb_thread:mode=2 (AWAIT_COMMISSIONING) --
 * see docs/power-base.md "앱 등록을 위한 안전 대기 상태".
 *
 * This component holds the decisions only; it never calls OpenThread
 * itself. power_base/main supplies the OpenThread/Joiner operations through
 * pb_enroll_ops_t, so the same code runs unmodified in the host tests
 * (tools/test_power_base_enrollment.c) against fakes and the NVS simulator.
 *
 * Three separate milestones, never merged into one "success":
 *   1. Joiner success   -- the Commissioner accepted the PSKd and handed over
 *                          the Network Key (OpenThread then saves only Network
 *                          Key/Channel/PAN ID -- not a complete Dataset).
 *   2. Enrolled         -- Thread attached (CHILD/ROUTER/LEADER) with a
 *                          complete Active Dataset received from the network,
 *                          AND the completion marker below is committed.
 *                          Only now may the caller start services (exporter,
 *                          POGO downstream, DETECT_OUT).
 *   3. heartbeat_received -- decided by the Linux bridge alone, when it hears
 *                          this Base's heartbeat. Nothing here claims it.
 *
 * Completion marker (dynamic "nvs" partition, namespace "pb_enroll", key
 * "done"): one 36-byte blob -- magic, version, this board's EUI-64, the
 * joined network's Extended PAN ID and Mesh-Local Prefix, CRC-32. It holds
 * no secret (no PSKd, no Network Key, nothing derived from either). It is
 * written only after milestone 2 and erased before every new Joiner attempt.
 * At boot, mode 2 resumes Thread only if the marker is valid, belongs to
 * this EUI-64, and the Active Dataset OpenThread stored is complete and
 * matches the marker's network; anything else is the safe wait. A Dataset
 * left over from a STANDALONE test has no marker, so it is never resumed.
 *
 * Threading: pb_enroll_ctl_* are not internally locked. The caller serialises
 * every call (power_base/main uses one mutex) and never calls them from the
 * OpenThread task's callbacks -- the Joiner callback only posts its outcome.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PB_ENROLL_EUI64_LEN 8
/* Thread PSKd: 6..32 characters of 0-9 and A-Z except I, O, Q, Z (the same
 * rule OpenThread's JoinerPskd::IsPskdValid() and the Linux API apply). */
#define PB_ENROLL_PSKD_MIN_LEN 6
#define PB_ENROLL_PSKD_MAX_LEN 32
#define PB_ENROLL_PSKD_BUF_LEN (PB_ENROLL_PSKD_MAX_LEN + 1)

#define PB_ENROLL_NVS_NAMESPACE "pb_enroll"
#define PB_ENROLL_NVS_KEY "done"
#define PB_ENROLL_MARKER_LEN 36

/* OpenThread's Joiner ends on its own within about a minute (one discovery
 * scan, then bounded DTLS/entrust timeouts) and does not wait for the
 * Commissioner's window; this guard only covers a callback that never
 * arrives. */
#define PB_ENROLL_JOINER_GUARD_MS 180000u
/* After Joiner success: time allowed to attach AND receive the complete
 * Active Dataset. Past it, Thread is stopped and the board waits again. */
#define PB_ENROLL_ATTACH_DEADLINE_MS 120000u

/* True when pskd[0..len) is a valid Thread PSKd. Never logs it. */
bool pb_enroll_pskd_is_valid(const char *pskd, size_t len);

/* memset that the compiler may not remove. */
void pb_enroll_wipe(void *buf, size_t len);

typedef struct {
    uint8_t ext_pan_id[8];
    uint8_t mesh_local_prefix[8];
} pb_enroll_network_id_t;

/* What the caller read from OpenThread's stored Active Dataset. */
typedef struct {
    /* The Joiner's minimum: a Network Key is stored. */
    bool has_network_key;
    /* Active Timestamp, Network Key, Network Name, Extended PAN ID, Mesh-Local
     * Prefix, PAN ID and Channel are all present -- i.e. received from the
     * network, not just the Joiner's partial save. */
    bool complete;
    pb_enroll_network_id_t id; /* meaningful only when complete */
} pb_enroll_dataset_info_t;

typedef enum {
    PB_ENROLL_MARKER_ABSENT = 0,
    PB_ENROLL_MARKER_VALID,
    PB_ENROLL_MARKER_CORRUPT,    /* wrong size, magic, version or CRC */
    PB_ENROLL_MARKER_FOREIGN,    /* intact, but for another EUI-64 */
    PB_ENROLL_MARKER_READ_ERROR, /* NVS itself failed */
} pb_enroll_marker_status_t;

pb_enroll_marker_status_t pb_enroll_marker_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_enroll_network_id_t *out);
/* set_blob + commit. On failure the caller must not treat the board as
 * enrolled; the stored value is then unspecified (see pb_enroll_ctl_tick). */
esp_err_t pb_enroll_marker_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_enroll_network_id_t *id);
/* ESP_OK also when there was nothing to erase. */
esp_err_t pb_enroll_marker_clear(void);

/* Outcome codes: why the board is waiting, or what the last attempt did.
 * Names (pb_enroll_result_name) are stable console/log strings. */
typedef enum {
    PB_ENROLL_RESULT_NONE = 0,             /* no marker; nothing attempted yet */
    PB_ENROLL_RESULT_MARKER_CORRUPT,
    PB_ENROLL_RESULT_MARKER_FOREIGN,
    PB_ENROLL_RESULT_MARKER_READ_ERROR,
    PB_ENROLL_RESULT_DATASET_MISSING,      /* marker valid, stored Dataset incomplete/absent */
    PB_ENROLL_RESULT_DATASET_MISMATCH,     /* marker valid, stored Dataset is another network */
    PB_ENROLL_RESULT_MARKER_CLEAR_FAILED,  /* could not erase the old marker: join refused */
    PB_ENROLL_RESULT_JOINER_START_FAILED,
    PB_ENROLL_RESULT_JOINER_SECURITY,      /* DTLS failed: wrong PSKd or not authorised */
    PB_ENROLL_RESULT_JOINER_NOT_FOUND,     /* no joinable network: OTBR/Commissioner absent, window closed */
    PB_ENROLL_RESULT_JOINER_TIMEOUT,
    PB_ENROLL_RESULT_JOINER_FAILED,        /* any other Joiner error */
    PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED, /* no Joiner result within PB_ENROLL_JOINER_GUARD_MS */
    PB_ENROLL_RESULT_CANCELLED,
    PB_ENROLL_RESULT_THREAD_START_FAILED,
    PB_ENROLL_RESULT_ATTACH_TIMEOUT,
    PB_ENROLL_RESULT_MARKER_WRITE_FAILED,
    PB_ENROLL_RESULT_ENROLLED,             /* this boot: Joiner + attach + marker */
    PB_ENROLL_RESULT_RESUMED,              /* this boot: valid marker + matching Dataset */
} pb_enroll_result_t;

const char *pb_enroll_result_name(pb_enroll_result_t result);

typedef enum {
    PB_ENROLL_BOOT_WAIT = 0,
    PB_ENROLL_BOOT_RESUME,
} pb_enroll_boot_action_t;

/* Pure boot rule (see the top comment). `ds` may be NULL (= nothing stored).
 * *out_reason gets the wait reason, or PB_ENROLL_RESULT_RESUMED. */
pb_enroll_boot_action_t pb_enroll_decide_boot(pb_enroll_marker_status_t marker, const pb_enroll_network_id_t *marker_id,
                                              const pb_enroll_dataset_info_t *ds, pb_enroll_result_t *out_reason);

typedef enum {
    PB_ENROLL_STATE_WAITING = 0, /* Thread disabled, no PSKd held, services off */
    PB_ENROLL_STATE_JOINING,     /* Joiner running */
    PB_ENROLL_STATE_ATTACHING,   /* Joiner succeeded; Thread enabled, not yet enrolled */
    PB_ENROLL_STATE_ENROLLED,    /* services may start (exactly once, by the caller) */
} pb_enroll_state_t;

const char *pb_enroll_state_name(pb_enroll_state_t state);

typedef enum {
    PB_ENROLL_JOINER_OK = 0,
    PB_ENROLL_JOINER_ERR_SECURITY,
    PB_ENROLL_JOINER_ERR_NOT_FOUND,
    PB_ENROLL_JOINER_ERR_TIMEOUT,
    PB_ENROLL_JOINER_ERR_OTHER,
} pb_enroll_joiner_outcome_t;

typedef struct {
    /* Start the Joiner (IPv6 up, Thread still disabled). Asynchronous: the
     * outcome arrives later through pb_enroll_ctl_on_joiner_done() with the
     * same `attempt`. The PSKd is only valid during this call. */
    esp_err_t (*joiner_start)(const char *pskd, uint32_t attempt, void *ctx);
    /* Abort a running Joiner; its outcome is then never reported. */
    void (*joiner_stop)(void *ctx);
    /* Enable Thread on the Dataset OpenThread already stored, with the
     * always-on FTD link mode. Must never form a network: fails instead when
     * no usable Dataset is stored (`require_complete` = the resume rule). */
    esp_err_t (*thread_start_stored)(bool require_complete, void *ctx);
    /* Disable Thread and IPv6 (back to the silent wait). */
    void (*thread_stop)(void *ctx);
    /* False when OpenThread could not be read; treated as "nothing stored". */
    bool (*read_dataset)(pb_enroll_dataset_info_t *out, void *ctx);
    void *ctx;
} pb_enroll_ops_t;

typedef struct {
    pb_enroll_ops_t ops;
    uint8_t eui64[PB_ENROLL_EUI64_LEN];
    pb_enroll_state_t state;
    pb_enroll_result_t last;
    bool resumed;          /* ENROLLED came from the boot rule, not a Joiner */
    uint32_t attempt;      /* incremented per accepted Joiner start */
    uint32_t deadline_ms;  /* JOINING guard or ATTACHING deadline */
} pb_enroll_ctl_t;

void pb_enroll_ctl_init(pb_enroll_ctl_t *c, const pb_enroll_ops_t *ops, const uint8_t eui64[PB_ENROLL_EUI64_LEN]);

/* Applies the boot rule once. Returns ENROLLED (Thread started on the stored
 * Dataset) or WAITING (Thread untouched: it stays disabled). */
pb_enroll_state_t pb_enroll_ctl_boot(pb_enroll_ctl_t *c);

/* `pskd` is a NUL-terminated buffer of PB_ENROLL_PSKD_BUF_LEN bytes; it is
 * wiped before this returns, whatever the outcome. ESP_OK: the Joiner is
 * running. ESP_ERR_INVALID_ARG: malformed PSKd (state unchanged).
 * ESP_ERR_INVALID_STATE: not WAITING. ESP_FAIL: see c->last. */
esp_err_t pb_enroll_ctl_request_join(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN], uint32_t now_ms);

/* JOINING/ATTACHING -> WAITING (CANCELLED). ESP_ERR_INVALID_STATE otherwise. */
esp_err_t pb_enroll_ctl_cancel(pb_enroll_ctl_t *c);

/* A Joiner outcome for `attempt`; ignored unless it is the current attempt
 * and the state is JOINING. */
void pb_enroll_ctl_on_joiner_done(pb_enroll_ctl_t *c, uint32_t attempt, pb_enroll_joiner_outcome_t outcome,
                                  uint32_t now_ms);

/* Periodic check. `attached` = otDeviceRole is CHILD, ROUTER or LEADER. */
void pb_enroll_ctl_tick(pb_enroll_ctl_t *c, bool attached, uint32_t now_ms);

static inline bool pb_enroll_ctl_services_allowed(const pb_enroll_ctl_t *c)
{
    return c->state == PB_ENROLL_STATE_ENROLLED;
}

#ifdef __cplusplus
}
#endif
