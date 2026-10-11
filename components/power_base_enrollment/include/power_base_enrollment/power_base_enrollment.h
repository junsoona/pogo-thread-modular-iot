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
 * "done"): one blob -- magic, version, this board's EUI-64, the joined
 * network's Extended PAN ID and Mesh-Local Prefix, CRC-32. It holds no
 * secret (no PSKd, no Network Key, nothing derived from either). It is
 * written only after milestone 2 and erased before every new Joiner attempt.
 * At boot, mode 2 resumes Thread only if the marker is valid, belongs to
 * this EUI-64, and the Active Dataset OpenThread stored is complete and
 * matches the marker's network; anything else is the safe wait. A Dataset
 * left over from a STANDALONE test has no marker, so it is never resumed.
 *
 * Two record versions share that one key, told apart by size and version:
 *   version 1 (36 bytes)  the marker alone. Written by an enrollment that
 *                         delivered no bridge endpoint (bench console, the
 *                         version 1 BLE start); the exporter then uses the
 *                         prepared image's cfg/pb_bridge exactly as before.
 *   version 2 (56 bytes)  the marker AND the Linux bridge endpoint (IPv6
 *                         address + port) delivered with that enrollment, in
 *                         the same blob and under the same CRC, so the
 *                         "enrolled" fact and the endpoint can never come
 *                         from two different attempts and a write cut short
 *                         leaves neither.
 * A delivered endpoint lives only in RAM (pb_enroll_ctl_t.pending_endpoint)
 * until milestone 2: cancel, any failure and a replaced attempt drop it. It
 * is stored only if it lies inside the Mesh-Local Prefix of the network the
 * board actually joined. A version 2 record whose endpoint does not pass
 * pb_enroll_bridge_endpoint_check(), or lies outside the record's own
 * Mesh-Local Prefix, is CORRUPT -- the board waits; it never falls back to
 * the cfg address.
 *
 * Endpoint policy (pb_enroll_endpoint_policy_t, from the prepared image --
 * see power_base_bridge_config.h): a board whose image carries no bridge
 * address ON PURPOSE must be given the endpoint with its enrollment.
 *   OPTIONAL  (default; every image without the policy) nothing below applies.
 *   REQUIRED  a Joiner request without an endpoint is refused with
 *             ESP_ERR_NOT_ALLOWED before anything is started or erased, by
 *             whatever transport it came (BLE, bench console, a direct
 *             call); and at boot a valid version 1 record -- an enrollment
 *             without an endpoint -- is not resumed: Thread and the services
 *             stay off (reported as marker_corrupt: a record version this
 *             board's configuration does not accept). Nothing is erased by
 *             booting; the next accepted request clears the old record as
 *             always. A valid version 2 record resumes as usual.
 *   INVALID   the image's policy cannot be trusted: every NEW Joiner request
 *             is refused the same way, with or without an endpoint. Boot is
 *             as with OPTIONAL: an already enrolled board is not taken down
 *             because its cfg image went bad.
 * What is NOT here: following a hub whose address changes later. This
 * component stores the endpoint it is given and restores it at boot; it
 * never refreshes it by itself. A version 3 record's endpoint can be
 * replaced by components/power_base_mgmt (pb_mgmt_client.h, "locate") after
 * the same hub -- same hub id, generation and key -- confirmed a new one
 * inside the same Thread network: one more store of the whole record with
 * only the endpoint changed. Versions 1 and 2 hold no key to check such an
 * answer with, so their endpoint is never replaced; a hub that moved needs
 * a new enrollment there. A network that was re-formed (another Dataset) is
 * a new enrollment in every version.
 *
 * Management credential (docs/power-base-management.md): an enrollment may
 * also deliver the hub's id, an enrollment generation and a 32-byte
 * management key -- what later authenticates a remote "unenroll". Such an
 * enrollment always has a bridge endpoint too, and is stored as
 *   version 3 (200 bytes)  marker, endpoint, credential and the release
 *                          state, in one blob under one CRC.
 * Unlike versions 1 and 2, a version 3 record HOLDS A SECRET (the key)
 * until its release is final. Nothing here logs it. Its state:
 *   ENROLLED     resumed at boot exactly like version 2.
 *   UNENROLLING  an authenticated unenroll command was journaled. Never
 *                resumed: the caller (power_base/main) finishes the release.
 *   RELEASED     this enrollment is out of use for good. Never resumed.
 *                With PB_ENROLL_RELEASE_F_ERASE_PENDING the stored join
 *                information (the Thread Dataset) has NOT been confirmed
 *                erased yet: the record still names the network, the
 *                endpoint and the hub and still holds the key, because the
 *                board has to tell the hub "released" and, once the erase
 *                is verified, has to make its release receipt with it.
 *                Without that flag the record is final: the Dataset erase
 *                was verified BEFORE it was written, the key and everything
 *                about the network are gone, and what is left is the
 *                release receipt (hub id, generation, command id and the MAC
 *                of the UNENROLL_DONE/ERASED message) -- no secret.
 * The controller treats UNENROLLING and RELEASED as "not enrolled": the
 * board waits, Thread untouched, last result NONE. Versions 1 and 2 are read
 * and resumed exactly as before and are never rewritten as version 3 by
 * reading them; only a new enrollment that delivered a credential writes
 * version 3.
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
#define PB_ENROLL_MARKER_LEN 36    /* record version 1 */
#define PB_ENROLL_MARKER_V2_LEN 56 /* record version 2: with the bridge endpoint */
#define PB_ENROLL_RECORD_V3_LEN 200 /* record version 3: with the management credential */
#define PB_ENROLL_RECORD_MAX_LEN PB_ENROLL_RECORD_V3_LEN

#define PB_ENROLL_HUB_ID_LEN 16
#define PB_ENROLL_GENERATION_LEN 16
#define PB_ENROLL_MGMT_KEY_LEN 32
#define PB_ENROLL_COMMAND_ID_LEN 16
#define PB_ENROLL_MAC_LEN 32
/* Byte [50] of a version 3 record and byte [64] of the BLE start that
 * delivers the credential. */
#define PB_ENROLL_MGMT_FORMAT_VERSION 1u

#define PB_ENROLL_BRIDGE_ADDR_LEN 16

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

/* Where the Linux bridge's ingest listener is, as a client delivered it with
 * the enrollment: the OTBR's Mesh-Local EID and its CoAP port. Not a secret. */
typedef struct {
    uint8_t addr[PB_ENROLL_BRIDGE_ADDR_LEN]; /* IPv6, network byte order */
    uint16_t port;
} pb_enroll_bridge_endpoint_t;

typedef enum {
    PB_ENROLL_ENDPOINT_OK = 0,
    PB_ENROLL_ENDPOINT_UNSPECIFIED,    /* :: */
    PB_ENROLL_ENDPOINT_LOOPBACK,       /* ::1 */
    PB_ENROLL_ENDPOINT_IPV4_EMBEDDED,  /* ::ffff:a.b.c.d (mapped) or ::a.b.c.d (compatible) */
    PB_ENROLL_ENDPOINT_MULTICAST,      /* ff00::/8 */
    PB_ENROLL_ENDPOINT_LINK_LOCAL,     /* fe80::/10 */
    PB_ENROLL_ENDPOINT_NOT_MESH_LOCAL, /* not in fd00::/8, where Thread Mesh-Local Prefixes live */
    PB_ENROLL_ENDPOINT_LOCATOR,        /* IID 0000:00ff:fe00:xxxx: an RLOC or ALOC, not an EID */
    PB_ENROLL_ENDPOINT_PORT_ZERO,
} pb_enroll_endpoint_check_t;

/* What a delivered endpoint must be before any network is known: a Thread
 * Mesh-Local EID (never an RLOC/ALOC, which moves with the topology) and a
 * non-zero port. Pure; never logs the address. */
pb_enroll_endpoint_check_t pb_enroll_bridge_endpoint_check(const pb_enroll_bridge_endpoint_t *ep);
const char *pb_enroll_endpoint_check_name(pb_enroll_endpoint_check_t check);
/* True when the endpoint's first 64 bits are `id`'s Mesh-Local Prefix. */
bool pb_enroll_bridge_endpoint_in_network(const pb_enroll_bridge_endpoint_t *ep, const pb_enroll_network_id_t *id);

/* What the hub issued for one enrollment. `key` is a secret. */
typedef struct {
    uint8_t hub_id[PB_ENROLL_HUB_ID_LEN];
    uint8_t generation[PB_ENROLL_GENERATION_LEN];
    uint8_t key[PB_ENROLL_MGMT_KEY_LEN];
} pb_enroll_mgmt_credential_t;

/* True when hub id, generation and key are each not all zero. */
bool pb_enroll_mgmt_credential_is_valid(const pb_enroll_mgmt_credential_t *cred);

/* Byte [6] of a version 3 record. */
typedef enum {
    PB_ENROLL_MGMT_NONE = 0,        /* versions 1 and 2: no credential */
    PB_ENROLL_MGMT_ENROLLED = 1,
    PB_ENROLL_MGMT_UNENROLLING = 2, /* command journaled, release not yet stored */
    PB_ENROLL_MGMT_RELEASED = 3,    /* out of use for good; see the release flags */
} pb_enroll_mgmt_state_t;

/* Byte [7] of a RELEASED version 3 record. */
/* The stored join information is not confirmed erased yet. Alone: no other
 * flag is ever set with it. */
#define PB_ENROLL_RELEASE_F_ERASE_PENDING 0x01u
/* Final records only: the hub acknowledged the "released" notice (a note for
 * logs; it changes nothing). */
#define PB_ENROLL_RELEASE_F_NOTICE_ACKED 0x02u
/* Final records only, and always set in one: the erase was verified and the
 * record holds the release receipt. */
#define PB_ENROLL_RELEASE_F_ERASED 0x04u

/* The stored record: the marker, for version 2 the endpoint with it, and for
 * version 3 the management credential and release state as well. */
typedef struct {
    pb_enroll_network_id_t id;
    bool has_bridge_endpoint; /* versions 2 and 3 (not a final RELEASED record) */
    pb_enroll_bridge_endpoint_t bridge_endpoint;

    /* Version 3 only; everything below is zero for versions 1 and 2. */
    bool has_mgmt;
    pb_enroll_mgmt_state_t mgmt_state;
    uint8_t release_flags; /* RELEASED only */
    /* hub_id and generation: every state. key: every state but a final
     * RELEASED record. */
    pb_enroll_mgmt_credential_t mgmt;
    uint8_t command_id[PB_ENROLL_COMMAND_ID_LEN]; /* UNENROLLING, RELEASED */
    /* A final RELEASED record only: the MAC of this release's
     * UNENROLL_DONE/ERASED message -- the release receipt. */
    uint8_t done_mac[PB_ENROLL_MAC_LEN];
    uint8_t ack_mac[PB_ENROLL_MAC_LEN]; /* always zero (kept for the layout) */
} pb_enroll_record_t;

/* True for a record that must be resumed as an enrollment: version 1, 2, or
 * version 3 in state ENROLLED. */
static inline bool pb_enroll_record_is_enrolled(const pb_enroll_record_t *rec)
{
    return !rec->has_mgmt || rec->mgmt_state == PB_ENROLL_MGMT_ENROLLED;
}

/* True for a RELEASED version 3 record whose Dataset erase was verified: no
 * network, no endpoint, no key -- only the release receipt. */
static inline bool pb_enroll_record_is_final_release(const pb_enroll_record_t *rec)
{
    return rec->has_mgmt && rec->mgmt_state == PB_ENROLL_MGMT_RELEASED &&
           (rec->release_flags & PB_ENROLL_RELEASE_F_ERASE_PENDING) == 0;
}

typedef enum {
    PB_ENROLL_MARKER_ABSENT = 0,
    PB_ENROLL_MARKER_VALID,
    /* Wrong size, magic, version, flags, reserved bytes or CRC; or a version
     * 2 record whose endpoint is not usable for its own network. */
    PB_ENROLL_MARKER_CORRUPT,
    PB_ENROLL_MARKER_FOREIGN,    /* intact, but for another EUI-64 */
    PB_ENROLL_MARKER_READ_ERROR, /* NVS itself failed */
} pb_enroll_marker_status_t;

/* Any version. *out is zeroed unless the result is VALID. A VALID version 3
 * record may be in any of its states: check pb_enroll_record_is_enrolled()
 * before treating it as an enrollment. The caller wipes *out when it is done
 * with a record that holds a key (pb_enroll_wipe). */
pb_enroll_marker_status_t pb_enroll_record_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_enroll_record_t *out);
/* set_blob + commit of ONE blob: version 1 without an endpoint (the same 36
 * bytes as before), version 2 with one, version 3 when rec->has_mgmt.
 * ESP_ERR_INVALID_ARG for an endpoint that fails the check or lies outside
 * rec->id's Mesh-Local Prefix, and for a version 3 record whose fields do not
 * fit its state (nothing is written). On any other failure the caller must
 * not treat the board as enrolled; the stored value is then unspecified (see
 * pb_enroll_ctl_tick). */
esp_err_t pb_enroll_record_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_enroll_record_t *rec);

/* The marker part only, of a record that is an enrollment. A version 3
 * record in state UNENROLLING or RELEASED has no marker: ABSENT. */
pb_enroll_marker_status_t pb_enroll_marker_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_enroll_network_id_t *out);
/* A version 1 record. Same failure contract as pb_enroll_record_store(). */
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
    /* Boot: marker valid, stored Dataset is another network. An attempt that
     * carried a bridge endpoint: the network the board joined is not the one
     * that endpoint lies in (its /64 is not the joined Mesh-Local Prefix). */
    PB_ENROLL_RESULT_DATASET_MISMATCH,
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

typedef enum {
    PB_ENROLL_ENDPOINT_POLICY_OPTIONAL = 0,
    PB_ENROLL_ENDPOINT_POLICY_REQUIRED,
    PB_ENROLL_ENDPOINT_POLICY_INVALID,
} pb_enroll_endpoint_policy_t;

const char *pb_enroll_endpoint_policy_name(pb_enroll_endpoint_policy_t policy);

typedef struct {
    pb_enroll_ops_t ops;
    uint8_t eui64[PB_ENROLL_EUI64_LEN];
    /* OPTIONAL after pb_enroll_ctl_init(); see the top comment. */
    pb_enroll_endpoint_policy_t endpoint_policy;
    pb_enroll_state_t state;
    pb_enroll_result_t last;
    bool resumed;          /* ENROLLED came from the boot rule, not a Joiner */
    uint32_t attempt;      /* incremented per accepted Joiner start */
    uint32_t deadline_ms;  /* JOINING guard or ATTACHING deadline */
    /* The endpoint delivered with the attempt in progress. RAM only; set
     * while JOINING/ATTACHING, dropped on every way back to WAITING. */
    bool pending_endpoint;
    pb_enroll_bridge_endpoint_t pending_ep;
    /* ENROLLED only: the endpoint stored with this enrollment (this boot's
     * attempt, or the version 2 record a resume read). False = none was
     * delivered; the exporter keeps the prepared image's cfg/pb_bridge. */
    bool has_bridge_endpoint;
    pb_enroll_bridge_endpoint_t bridge_endpoint;
    /* The management credential delivered with the attempt in progress. RAM
     * only, wiped on every way back to WAITING and once it is stored. */
    bool pending_mgmt;
    pb_enroll_mgmt_credential_t pending_mgmt_cred;
    /* ENROLLED only: the stored record is version 3 in state ENROLLED. The
     * key itself is not kept here; whoever needs it reads the record. */
    bool has_mgmt;
} pb_enroll_ctl_t;

void pb_enroll_ctl_init(pb_enroll_ctl_t *c, const pb_enroll_ops_t *ops, const uint8_t eui64[PB_ENROLL_EUI64_LEN]);

/* What the prepared image says about enrolling without a bridge endpoint.
 * Call after pb_enroll_ctl_init() and before pb_enroll_ctl_boot(). */
void pb_enroll_ctl_set_endpoint_policy(pb_enroll_ctl_t *c, pb_enroll_endpoint_policy_t policy);

/* Applies the boot rule once. Returns ENROLLED (Thread started on the stored
 * Dataset) or WAITING (Thread untouched: it stays disabled). */
pb_enroll_state_t pb_enroll_ctl_boot(pb_enroll_ctl_t *c);

/* `pskd` is a NUL-terminated buffer of PB_ENROLL_PSKD_BUF_LEN bytes; it is
 * wiped before this returns, whatever the outcome. ESP_OK: the Joiner is
 * running. ESP_ERR_INVALID_ARG: malformed PSKd (state unchanged).
 * ESP_ERR_INVALID_STATE: not WAITING. ESP_FAIL: see c->last.
 * ESP_ERR_NOT_ALLOWED: the endpoint policy refuses this request (REQUIRED:
 * this one, which has no endpoint; INVALID: every request) -- state, last
 * result, stored record and Dataset are untouched and no Joiner ran.
 * No bridge endpoint: enrolling writes a version 1 record. */
esp_err_t pb_enroll_ctl_request_join(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN], uint32_t now_ms);

/* The same, for an attempt that also delivers the bridge endpoint (copied;
 * kept in RAM until enrolled, then stored in the version 2 record).
 * ESP_ERR_INVALID_ARG also for an endpoint that fails
 * pb_enroll_bridge_endpoint_check() -- nothing is started or erased.
 * ESP_ERR_NOT_ALLOWED only under an INVALID endpoint policy. */
esp_err_t pb_enroll_ctl_request_join_with_endpoint(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN],
                                                   const pb_enroll_bridge_endpoint_t *endpoint, uint32_t now_ms);

/* The same, for an attempt that also delivers the management credential
 * (copied; kept in RAM until enrolled, then stored in the version 3 record;
 * the caller wipes its own copy). ESP_ERR_INVALID_ARG also for a credential
 * that fails pb_enroll_mgmt_credential_is_valid(). */
esp_err_t pb_enroll_ctl_request_join_managed(pb_enroll_ctl_t *c, char pskd[PB_ENROLL_PSKD_BUF_LEN],
                                             const pb_enroll_bridge_endpoint_t *endpoint,
                                             const pb_enroll_mgmt_credential_t *mgmt, uint32_t now_ms);

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
