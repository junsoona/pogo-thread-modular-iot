/*
 * Thread adapter shared by all runtime roles (module always builds
 * CONFIG_OPENTHREAD_FTD=y once -- see section 8 of the implementation
 * spec). Merges what were previously two separate upper_thread/lower_thread
 * adapters into one: which link-mode/router-eligibility/poll-period
 * configuration applies depends on the *current* role_manager role, not on
 * a compile-time choice.
 *
 * Role transitions always go through thread_manager_stop() first (disable
 * Thread + IP6) before a new role's link mode / router eligibility is
 * applied and Thread is re-enabled -- OpenThread requires Thread disabled
 * while those are changed (ESP-IDF v6.1 / OpenThread API).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "openthread/thread.h"
#include "openthread/dataset.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THREAD_MANAGER_ROLE_ROOT = 0,
    THREAD_MANAGER_ROLE_INTERMEDIATE,
    THREAD_MANAGER_ROLE_LEAF,
} thread_manager_role_t;

/* Brings up the OpenThread stack (FTD). Does NOT enable IP6/Thread or bring
 * up any dataset yet -- call one of the functions below once role_manager
 * has settled on a role. Call exactly once at boot. */
esp_err_t thread_manager_init(void);

/*
 * Applies the same fixed link-mode configuration as
 * thread_manager_start_as_root() below (rx-on-when-idle=true, FTD,
 * full network data=true, router-eligible=true) and either resumes a
 * previously-commissioned dataset from OpenThread's own storage, or forms a
 * new network from this component's Kconfig (OPENTHREAD_NETWORK_*) defaults
 * if none exists yet.
 *
 * This is the NEUTRAL name for that behavior: it says nothing about Thread
 * roles (CHILD/ROUTER/LEADER) at all -- `otDeviceRole` is decided by Thread
 * itself once this device actually attaches, which can take an unbounded
 * amount of time (e.g. waiting for an OTBR or another FTD to be reachable)
 * and is NOT this function's concern. Use this for any device whose FIXED
 * APPLICATION role does not itself require or claim Thread Leadership --
 * currently `power_base/main/app_main.c` (see docs/power-base.md's
 * "module/Thread role decoupling" section: the power base's fixed role is
 * the physical POGO root/provisioning coordinator, decoupled from whatever
 * `otDeviceRole` it happens to hold).
 */
esp_err_t thread_manager_start_as_always_on_ftd(void);

/*
 * EXTERNAL_OTBR: joins an EXISTING Thread network formed by a Linux OTBR
 * (+ a separate ESP32-H2 RCP -- see docs/otbr-rcp.md), using its real
 * Active Dataset TLVs (read from components/power_base_thread_config, NEVER
 * hardcoded/invented here) instead of forming or resuming a dataset of this
 * device's own. This is the power base's counterpart to
 * thread_manager_start_as_always_on_ftd() for that case -- same fixed link
 * mode (rx-on-when-idle=true, full FTD, router-eligible=true), but the
 * dataset always comes from the caller, never from this component's own
 * Kconfig defaults or NVS-resume logic.
 *
 * Rejects (ESP_ERR_INVALID_ARG, no OpenThread call made at all):
 *   - dataset_tlvs == NULL
 *   - dataset_len == 0
 *   - dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH
 *
 * Otherwise: acquires the OpenThread lock, validates the TLVs with
 * otDatasetIsValid(&tlvs, /aActive=/true) (REAL semantic validation --
 * malformed TLVs, missing mandatory fields, etc. are all caught here, not
 * just re-checked for length), disables Thread/IP6, applies the SAME
 * always-on-FTD link mode as thread_manager_start_as_always_on_ftd(), then
 * calls esp_openthread_auto_start(&tlvs) to apply this exact dataset and
 * bring Thread up.
 *
 * On ANY failure (invalid TLVs, a lock timeout, any OpenThread API
 * returning an error) this function returns a non-ESP_OK code and NEVER
 * falls back to forming or resuming a standalone network of its own --
 * the caller (power_base/main/app_main.c) must halt rather than silently
 * degrade to STANDALONE behavior (see docs/power-base.md's EXTERNAL_OTBR
 * fail-closed policy). On success, logs only the dataset LENGTH and a
 * neutral "... Active Dataset accepted (N bytes) -- joining existing Thread
 * network" message -- the dataset bytes (Network Key etc.) are never
 * logged, same policy as thread_manager_join_with_dataset() below.
 */
esp_err_t thread_manager_start_as_always_on_ftd_with_dataset(const uint8_t *dataset_tlvs, uint16_t dataset_len);

/*
 * ROOT: identical underlying behavior to thread_manager_start_as_always_on_ftd()
 * above (same link mode, same resume-or-form logic) -- kept as a distinctly
 * named wrapper ONLY for `module`'s `MODULE_DEV_MODE_DEV_ROOT` regression
 * fixture (see docs/pogo-interface.md's "development compat mode"), which
 * reproduces the old `upper` project's behavior and whose own logs/tests
 * still refer to this device's role as "ROOT". New callers -- especially
 * anything that is not actually claiming to BE a fixed Thread ROOT/Leader --
 * should call thread_manager_start_as_always_on_ftd() instead; see that
 * function's own comment for why (e.g. `power_base` no longer calls this).
 */
esp_err_t thread_manager_start_as_root(void);

/*
 * INTERMEDIATE/LEAF: joins using a dataset obtained from the upstream
 * parent (already validated via thread_manager_validate_dataset_tlvs()).
 *
 * `thread_data_poll_period_ms` is applied (via otLinkSetPollPeriod) only
 * for THREAD_MANAGER_ROLE_LEAF; pass 0 to use this component's Kconfig
 * default (CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS, 500ms by
 * default). **This is the Thread SED MAC data poll interval -- how often
 * this device wakes its radio to ask its parent "do you have anything
 * queued for me" -- and is DELIBERATELY DECOUPLED from any sensor
 * measurement/sampling period** (real-hardware finding: an earlier
 * revision passed the sensor's own `polling_period_ms`, e.g. 3528ms,
 * straight through to this parameter; a SED with a multi-second data-poll
 * interval only picks up a queued CoAP response from its parent on its
 * NEXT poll, which can easily arrive after the CoAP client's own
 * ACK-timeout wait already gave up -- observed on real hardware as
 * `sensor_transport`/`topology_reporter` timing out even though the power
 * base had already stored the sample. See
 * `components/coap_response_timing` and `sensor_service`'s own
 * measurement-period API, which this parameter has NO relationship to any
 * more -- the caller must never derive this value from a sensor's sampling
 * period). Callers that have no reason to override it should simply pass 0.
 *
 * LEAF applies, before enabling Thread: router eligible=false,
 * mRxOnWhenIdle=false, mDeviceType=false, mNetworkData=false (SED).
 * INTERMEDIATE applies: router eligible=true, full FTD link mode (always-on,
 * unaffected by `thread_data_poll_period_ms`, which this branch never
 * applies at all) -- and, per spec, never forms its own separate network.
 */
esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t thread_data_poll_period_ms);

/*
 * Safely disables Thread + IP6 (idempotent). Call before switching role, and
 * for UNASSIGNED/ROLE_ERROR (which must never auto-join or auto-form a
 * network -- section 8/16).
 */
esp_err_t thread_manager_stop(void);

/* Thread-safe (acquires the OpenThread API lock). */
esp_err_t thread_manager_get_active_dataset_tlvs(otOperationalDatasetTlvs *out_tlvs);
bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len);
otDeviceRole thread_manager_get_role(void);

/*
 * Current Leader's on-mesh (RLOC) address, via otThreadGetLeaderRloc() --
 * never a hardcoded/cached address (section 8 of the follow-up spec: "ROOT
 * 주소를 문자열로 하드코딩하지 말고 OpenThread API를 통해 현재 Leader/ROOT
 * 주소를 구한다"). In this topology ROOT is always the one node that forms
 * the network and is therefore always Leader (INTERMEDIATE/LEAF only ever
 * join an existing dataset, see thread_manager_join_with_dataset()), so "the
 * current Leader" and "ROOT" are the same node for as long as this module
 * chain runs -- sensor_transport uses this to reach ROOT's CoAP resource
 * without ever assuming a fixed IPv6 address. Returns ESP_ERR_INVALID_STATE
 * if this device is not yet attached to a Thread network (no Leader to
 * report).
 */
esp_err_t thread_manager_get_leader_address(otIp6Address *out_addr);

/*
 * This device's own Thread Mesh-Local EID, via otThreadGetMeshLocalEid()
 * (ESP-IDF v6.1 / components/openthread/openthread/include/openthread/thread.h:
 * "const otIp6Address *otThreadGetMeshLocalEid(otInstance *aInstance)" --
 * confirmed against that exact header, not assumed from memory). That
 * function returns a pointer into OpenThread's own internal state whose
 * lifetime is tied to the OpenThread API lock, so this wrapper copies the
 * 16 bytes out into *out_addr WHILE still holding the lock, before
 * releasing it -- the caller never touches OpenThread's own pointer.
 * Returns ESP_ERR_INVALID_STATE if OpenThread has no Mesh-Local EID to
 * report yet (should not happen once thread_manager_init() has run: unlike
 * the Leader RLOC, the Mesh-Local EID does not require this device to be
 * attached to a partition). Used by power_base's own collector-endpoint
 * identity (its ML-EID is what every sensor module in the chain sends
 * "/s"/"/t" traffic to, regardless of which Thread role -- CHILD, ROUTER, or
 * LEADER -- it currently holds or how many hops separate them) -- see
 * docs/power-base.md.
 */
esp_err_t thread_manager_get_mesh_local_eid(otIp6Address *out_addr);

/* Non-blocking: true and fills *out_role if the role changed since the last
 * call (backed by a small queue fed from the OpenThread state-changed
 * callback), false if nothing new. */
bool thread_manager_poll_role_change(otDeviceRole *out_role);

/*
 * Human-readable otDeviceRole name ("DISABLED"/"DETACHED"/"CHILD"/"ROUTER"/
 * "LEADER", or "UNKNOWN" for anything else) -- code review follow-up
 * section 5: "로그에는 숫자만 출력하지 말고 LEADER, ROUTER, CHILD 문자열을
 * 함께 출력한다". Every log line that names an otDeviceRole in this codebase
 * (power_base/main/app_main.c, child_service.c, parent_service.c) uses this
 * one function rather than each re-deriving its own mapping.
 */
const char *thread_manager_role_to_str(otDeviceRole role);

#ifdef __cplusplus
}
#endif
