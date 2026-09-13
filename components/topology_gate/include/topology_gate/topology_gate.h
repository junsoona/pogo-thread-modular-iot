/*
 * Cross-task-safe latch for whether THIS sensor module currently has a
 * live, CONFIRMED topology registration with the active power base -- i.e.
 * whether its own `TOPOLOGY_REPORT_ATTACHED` (or a later re-registration,
 * see below) has actually received a CoAP 2.xx response from the power
 * base's `"/t"` resource (`components/power_base_transport`), never merely
 * "the OpenThread transport delivered the bytes".
 *
 * Why this exists (real-hardware race, see README.md/docs/power-base.md):
 * `child_service` flips Thread-joined state and unblocks `sensor_service`'s
 * wireless sample hand-off the instant `otDeviceRole` settles to
 * CHILD/ROUTER/LEADER. `topology_reporter` runs on its own independent
 * ~500ms poll tick and only notices `role_manager_is_upstream_ready()`
 * becoming true on its NEXT tick. On real hardware this first sensor sample
 * can therefore reach the power base's `"/s"` resource before the power
 * base's `topology_registry` has ever heard of this module's uuid at all,
 * and gets bounced as CoAP 4.04 Not Found ("unknown module"). This gate
 * closes that window with an explicit readiness condition instead of a
 * fixed delay: `sensor_transport` (the reader) refuses to even attempt a
 * `"/s"` wireless send while the gate is closed, and `topology_reporter`
 * (the sole writer) opens it only once a real 2.xx response confirms the
 * power base actually knows this module.
 *
 * This is deliberately its OWN tiny, dependency-free (FreeRTOS event group
 * only) component rather than a field inside `topology_reporter` or
 * `sensor_transport` directly: `topology_reporter` already depends on
 * `sensor_service`, and `sensor_transport` already depends on
 * `sensor_service` too, so a direct `sensor_transport` <-> `topology_reporter`
 * dependency in EITHER direction is unnecessary here -- both simply depend
 * on this one leaf component for the actual synchronized state, and never
 * need to know about each other at all. The one exception is the
 * re-registration REQUEST (see `topology_gate_request_reregister()` below),
 * which flows the same way: `sensor_transport` sets it, `topology_reporter`
 * is the only one that ever reads/clears it -- still no direct dependency
 * between the two components, just this shared coordinator.
 *
 * Both bits live in ONE FreeRTOS event group specifically because an event
 * group's set/clear/get operations are already internally protected by
 * FreeRTOS -- this gives real synchronization across `topology_reporter`'s
 * own task (the writer of BIT_REGISTERED; the reader-and-clearer of
 * BIT_REREGISTER_REQUESTED) and `sensor_service`'s periodic task /
 * `sensor_transport` (the reader of BIT_REGISTERED; the writer of
 * BIT_REREGISTER_REQUESTED) without a second hand-rolled mutex/critical
 * section, and -- the actual point of introducing this component at all --
 * without the plain, unsynchronized `bool` that section 3 of the follow-up
 * spec calls out as unacceptable for a flag read and written from more than
 * one FreeRTOS task.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the underlying event group. Call once, before either
 * topology_reporter_init() or sensor_transport_init() can run (both touch
 * this gate) -- see module/main/app_main.c's boot sequence. Starts CLOSED,
 * with no re-registration request pending. */
esp_err_t topology_gate_init(void);

/* Called ONLY by topology_reporter, the sole writer of the "is registered"
 * bit: opens the gate once its own TOPOLOGY_REPORT_ATTACHED (or a later
 * re-registration attempt after topology_gate_request_reregister(), see
 * below) has received an actual CoAP 2.xx response from the power base. */
void topology_gate_open(void);

/*
 * Called ONLY by topology_reporter, on every event that must immediately
 * stop "/s" sending until a fresh registration succeeds: upstream
 * DETACHED/ERROR, role settling to UNASSIGNED/ROLE_ERROR, this module's own
 * Thread membership being lost, a brand new provisioning session starting,
 * or (see topology_gate_request_reregister()) the power base rejecting a
 * sample as unknown-module. "Immediately" here means within at most one
 * topology_reporter poll tick (TOPOLOGY_REPORTER_POLL_TICK_MS in
 * topology_reporter.c) -- the same bound topology_reporter.c's own
 * resync_downstream_connected() already documents for noticing a dropped
 * port event, not a synchronous cross-task guarantee.
 */
void topology_gate_close(void);

/*
 * Safe to call from ANY task (in practice, sensor_service's periodic
 * sampling task via sensor_transport_send_sample()): true once
 * topology_gate_open() has been called and stays true until the next
 * topology_gate_close(). A sensor sample must never be sent wirelessly
 * while this is false (section 1/6 of the follow-up spec).
 */
bool topology_gate_is_open(void);

/*
 * Called by sensor_transport when the power base rejects a "/s" sample as
 * unknown-module (CoAP 4.04 Not Found) -- its RAM-only topology_registry
 * forgot this module across a power base reboot (docs/power-base.md
 * section 11). Signals topology_reporter to force an immediate fresh
 * TOPOLOGY_REPORT_ATTACHED attempt on its very next poll tick instead of
 * waiting for its normal CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS
 * heartbeat schedule (section 8 of the follow-up spec: "30초 heartbeat만
 * 기다리지 않는다"). Idempotent: calling this again before
 * topology_gate_take_reregister_request() has consumed the previous request
 * has no additional effect. Does NOT close the gate itself -- the caller
 * (sensor_transport) does that separately via topology_gate_close(), and
 * topology_reporter closes it again on its own once it notices this request
 * if it had not already been closed.
 */
void topology_gate_request_reregister(void);

/*
 * Called ONLY by topology_reporter's own task, once per poll tick:
 * atomically reads AND clears the pending re-registration request. Returns
 * true at most once per topology_gate_request_reregister() call -- a
 * second, unrelated caller (there is none today, but see this header's
 * "ONLY" annotations above) could never both observe the same request as
 * pending.
 */
bool topology_gate_take_reregister_request(void);

#ifdef __cplusplus
}
#endif
