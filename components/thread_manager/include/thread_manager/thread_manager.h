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
 * ROOT: resumes a previously-commissioned dataset if one exists in
 * OpenThread's own storage, otherwise forms a new network from this
 * component's Kconfig (OPENTHREAD_NETWORK_*) defaults. Router-eligible,
 * full network data -- a Leader/Router candidate.
 */
esp_err_t thread_manager_start_as_root(void);

/*
 * INTERMEDIATE/LEAF: joins using a dataset obtained from the upstream
 * parent (already validated via thread_manager_validate_dataset_tlvs()).
 * poll_period_ms is applied (via otLinkSetPollPeriod) only for
 * THREAD_MANAGER_ROLE_LEAF; pass 0 to use this component's Kconfig default.
 *
 * LEAF applies, before enabling Thread: router eligible=false,
 * mRxOnWhenIdle=false, mDeviceType=false, mNetworkData=false (SED).
 * INTERMEDIATE applies: router eligible=true, full FTD link mode -- and,
 * per spec, never forms its own separate network.
 */
esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t poll_period_ms);

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

/* Non-blocking: true and fills *out_role if the role changed since the last
 * call (backed by a small queue fed from the OpenThread state-changed
 * callback), false if nothing new. */
bool thread_manager_poll_role_change(otDeviceRole *out_role);

#ifdef __cplusplus
}
#endif
