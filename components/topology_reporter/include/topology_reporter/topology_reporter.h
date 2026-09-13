/*
 * Module-side sender for topology reports (section 8 of the power-base
 * follow-up spec): tells the power base this module's own role, parent,
 * depth, sensor bitmap, and upstream/downstream connection state, over
 * Thread CoAP (resource "t" -- see components/power_base_transport, the
 * receiving side, which only power_base runs).
 *
 * Deliberately does NOT hook role_manager's role-change callback: a role
 * settles to LEAF/INTERMEDIATE the moment role_manager derives it from port
 * peer-types, which happens BEFORE (and independently of)
 * role_manager_is_upstream_ready() becoming true -- that flag only flips once
 * child_service finishes an actual Thread join (see role_manager.h's own doc
 * comment on it). Reacting to the role-change callback would either miss the
 * "now actually attached" moment entirely or need a second synchronization
 * mechanism for it. Instead this component runs its own short poll tick,
 * comparing role_manager_is_upstream_ready()/role_manager_get_role() against
 * what it last saw -- simple, race-free, and cheap (two function calls per
 * tick). DOWNSTREAM connectivity IS event-driven (topology_reporter_on_port_event()),
 * since pogo_port already delivers exactly the ATTACHED/PEER_READY/DETACHED/
 * ERROR events this needs with no polling required.
 *
 * Sends via Confirmable CoAP POST with the SAME bounded retry/backoff
 * schedule sensor_transport already uses (node_calc's UUID-hash mask
 * widening, 1 initial + 3 retries) -- no infinite retry loop, no busy
 * polling of the radio while not Thread-joined (a send attempt against no
 * Leader address simply returns false immediately, mirroring
 * sensor_transport_send_sample()'s policy). A send is only ever considered
 * successful once the power base's ACTUAL CoAP response code is a 2.xx --
 * an OpenThread-level "the bytes arrived" is not enough (code review
 * follow-up section 3).
 *
 * boot_session_generation (code review follow-up section 2) is a
 * NVS-persisted, monotonically-incrementing-per-boot counter
 * (load_next_boot_generation() in the .c file) rather than a random value --
 * topology_registry treats it as an opaque per-boot token either way (see
 * that component's own header comment), but a real counter gives a much
 * stronger ordering guarantee whenever NVS persistence itself succeeds. If
 * persisting a fresh boot generation fails, this component disables actual
 * report sending for the rest of this boot rather than risk reusing a
 * generation number a previous boot might reuse too (never "look
 * successful" on top of state that could not be durably committed) -- see
 * topology_reporter_init()'s own comment. The local per-boot sequence
 * counter never wraps: nearing its limit triggers a fresh persisted
 * generation and a sequence reset instead (see maybe_roll_over_sequence()).
 *
 * Data-plane readiness gate (real-hardware first-sample/topology race, see
 * components/topology_gate's header comment): this component is the ONLY
 * writer of that gate. It opens it the moment a TOPOLOGY_REPORT_ATTACHED (or
 * a later re-registration -- see topology_gate_request_reregister()) gets a
 * real CoAP 2.xx response, and closes it the instant `settled`
 * (role_manager_is_upstream_ready() && role in {LEAF, INTERMEDIATE}) turns
 * false -- upstream DETACHED/ERROR, role settling to UNASSIGNED/ROLE_ERROR,
 * this module's own Thread membership being lost, or a brand new
 * provisioning session starting all reach this component as `settled`
 * becoming false, within at most one TOPOLOGY_REPORTER_POLL_TICK_MS.
 */
#pragma once

#include "esp_err.h"
#include "module_config/module_config.h"
#include "pogo_port/pogo_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the reporter's own task. Call once, after sensor_transport_init()
 * (needs the OT CoAP layer already started -- see module/main/app_main.c's
 * boot sequence) and after role_manager_init()/child_service_init()/
 * parent_service_init() (reads their state every poll tick).
 */
esp_err_t topology_reporter_init(const module_config_t *cfg);

/*
 * Feed every pogo_port event (both ports) here -- events for UPSTREAM are
 * ignored; only DOWNSTREAM ATTACHED/PEER_READY/DETACHED/ERROR change this
 * module's own reported downstream_connected field. Safe to call from the
 * pogo_port callback context directly -- this only enqueues.
 */
void topology_reporter_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event,
                                      const pogo_port_event_data_t *data);

#ifdef __cplusplus
}
#endif
