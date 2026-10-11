/*
 * child_service owns the UPSTREAM pogo_port only (section 7): it answers a
 * parent's IDENTIFY/ASSIGN_UUID/ASSIGN_SLOT/SENSOR_HELLO/POLL_CONFIG/
 * SET_COLLECTOR/SET_DATASET requests, then drives thread_manager to join
 * using the received dataset as either INTERMEDIATE or LEAF (whichever
 * role_manager has settled on). It runs regardless of whether role_manager's
 * role is INTERMEDIATE or LEAF -- both have an UPSTREAM parent; only ROOT
 * does not run child_service at all.
 *
 * SET_COLLECTOR (POGO_MSG_SET_COLLECTOR) is a required step between
 * POLL_CONFIG and SET_DATASET (see docs/power-base.md): it delivers the
 * collector endpoint (the power base's own Mesh-Local EID + CoAP port +
 * permanent UUID) this module -- and, if it becomes INTERMEDIATE, everything
 * docked below it -- should send "/s"/"/t" traffic to, via
 * components/collector_endpoint. SET_DATASET is now gated on this having
 * already succeeded (POGO_NACK_BAD_STATE otherwise): starting the actual
 * Thread join before the collector endpoint is known would let this module
 * join and start measuring with nowhere valid to send its data.
 */
#pragma once

#include "esp_err.h"
#include "module_config/module_config.h"
#include "pogo_port/pogo_port.h"
#include "role_manager/role_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t child_service_init(const module_config_t *cfg);

/* Feed every UPSTREAM pogo_port event here (events for DOWNSTREAM are
 * ignored). Safe to call from the pogo_port callback context directly --
 * this only enqueues. */
void child_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data);

/*
 * Lets an already-JOINED child re-apply its Thread link mode (LEAF <->
 * INTERMEDIATE), without re-provisioning, if role_manager's role changes
 * while the previously-received dataset is still valid (section 2: the
 * sensor pairing and an already-received dataset both survive a role
 * change; only the DOWNSTREAM-facing behavior differs).
 */
void child_service_on_role_change(module_role_t new_role, module_role_t old_role);

/*
 * This module's own topology_depth as assigned by ITS upstream parent (0 if
 * this module is ROOT, or if no ASSIGN_SLOT has been processed yet).
 * parent_service uses this to assign own_depth+1 to whatever it attaches
 * downstream.
 */
uint8_t child_service_get_own_topology_depth(void);

/*
 * This module's immediate upstream parent's own identity, as told by
 * POGO_MSG_ASSIGN_PARENT_INFO (power-base follow-up spec section 7: "부모
 * UUID 전달"). All-zero / false until that step of the current provisioning
 * session has completed. Used by topology_reporter to fill a topology
 * report's parent_uuid/upstream-is-power-base fields -- child_service itself
 * never sends a topology report.
 */
void child_service_get_parent_uuid(uint8_t out_uuid[POGO_UUID_LEN]);
bool child_service_get_parent_is_power_base(void);

/*
 * UPSTREAM provisioning generation: how many provisioning sessions from this
 * module's parent have been carried through to a Thread join (0 until the
 * first). It goes up by one, and only then, at the moment a session reaches
 * JOINED -- that is the point from which this module holds a new collector
 * endpoint, Dataset, depth and parent, all from the same session. Nothing
 * else moves it: not a role change (LEAF <-> INTERMEDIATE re-applies the link
 * mode with the SAME Dataset), not OpenThread's own role changes (a ROUTER
 * promotion, a radio detach and re-attach), not a retransmitted request.
 *
 * It exists for whoever derived something from "what this module was given":
 * parent_service (what it handed to the module docked below, see
 * parent_service_context_t.get_upstream_generation) and topology_reporter
 * (its ATTACHED registration). Those compare the number they last acted on
 * with the current one, so a new session is noticed however briefly
 * role_manager_is_upstream_ready() was false in between, and however late
 * they look.
 *
 * Readable from any task. Never 0 again once it has left 0.
 */
uint32_t child_service_get_upstream_generation(void);

#ifdef __cplusplus
}
#endif
