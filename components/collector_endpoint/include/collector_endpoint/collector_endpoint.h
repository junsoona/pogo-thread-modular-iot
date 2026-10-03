/*
 * FreeRTOS-task-safe holder of THIS sensor module's own copy of the
 * collector endpoint -- where its "/s" sensor samples and "/t" topology
 * reports (see components/sensor_transport, components/topology_reporter)
 * should be sent. Never the power base's own component: the power base IS
 * the collector, so it never needs to store one about itself (see
 * power_base/main/app_main.c's pb_get_collector(), which computes its own
 * identity/Mesh-Local EID directly on every call instead).
 *
 * Populated by child_service (module/main/app_main.c's UPSTREAM side) once
 * its own POGO_MSG_SET_COLLECTOR step succeeds, from whatever this module's
 * OWN upstream parent sent it (a power base's real identity, or -- one hop
 * further down the chain -- an INTERMEDIATE module's own already-received
 * copy of that SAME power base identity, forwarded unmodified: see
 * docs/power-base.md, "INTERMEDIATE forwards the ORIGINAL power base
 * endpoint, never its own address"). Consumed by sensor_transport.c and
 * topology_reporter.c (as the destination address for their own CoAP sends)
 * and by parent_service, via module/main/module_wiring.c's
 * parent_service_context_t.get_collector callback, to hand the SAME endpoint
 * down to whatever docks below this module.
 *
 * Thread-safety: every accessor copies into/out of a small struct under one
 * FreeRTOS critical section (a spinlock, portMUX_TYPE) rather than storing
 * or handing back a caller-owned pointer -- a caller's struct pointer is
 * never retained past the call that passed it in (collector_endpoint_set()
 * copies every field it is given), and collector_endpoint_get() always
 * returns a full-struct snapshot BY VALUE, never a pointer into this
 * component's own static storage, so a reader on one task can never observe
 * a half-updated struct while a writer on another task is mid-update.
 *
 * Never persists to flash on its own: dynamic per-session state like this
 * lives in the same runtime "nvs" NVS partition Thread's own Active Dataset
 * already uses (never the fixed "cfg" partition) -- see child_service.c's
 * own NVS write for the durable copy. This component itself is a RAM-only
 * cache for the current session; a fresh session (a new PEER_READY, a
 * detach, or upstream loss) always calls collector_endpoint_clear() before
 * any later collector_endpoint_set() can populate a new one.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COLLECTOR_ENDPOINT_UUID_LEN 8
#define COLLECTOR_ENDPOINT_ADDR_LEN 16 /* IPv6, network byte order */

typedef struct {
    uint8_t  power_base_uuid[COLLECTOR_ENDPOINT_UUID_LEN];
    uint8_t  addr[COLLECTOR_ENDPOINT_ADDR_LEN];
    uint16_t port;
    uint32_t generation;
    bool     valid;
} collector_endpoint_snapshot_t;

/* Creates the underlying lock. Call once, before child_service_init()/
 * sensor_transport_init()/topology_reporter_init() (all touch this
 * component) -- see module/main/app_main.c's boot sequence. Starts invalid
 * (collector_endpoint_is_valid() == false) until the first successful set(). */
esp_err_t collector_endpoint_init(void);

/*
 * Validates and stores a fresh copy of the collector endpoint. Rejects
 * (returns ESP_ERR_INVALID_ARG, leaving the previous snapshot untouched)
 * an unspecified/multicast/link-local address, port 0, or an all-zero
 * power_base_uuid -- the same checks pogo_collector_endpoint_is_valid()
 * performs on the decoded wire struct, re-applied here independently so
 * this component's own API contract does not rely on every caller having
 * already validated its inputs. Every byte is copied into this component's
 * own static storage; the caller's buffers are never retained.
 */
esp_err_t collector_endpoint_set(const uint8_t power_base_uuid[COLLECTOR_ENDPOINT_UUID_LEN],
                                  const uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN], uint16_t port,
                                  uint32_t generation);

/*
 * Invalidates the current endpoint (collector_endpoint_is_valid() becomes
 * false immediately) without necessarily zeroing the stored bytes -- callers
 * must check the `valid` field (or call collector_endpoint_is_valid()) and
 * never send to a snapshot whose valid is false. Called on a fresh
 * provisioning session starting, a detach, or upstream Thread membership
 * being lost (see child_service.c).
 */
void collector_endpoint_clear(void);

/* Fills *out with a full, consistent snapshot (by value) of the current
 * endpoint. Returns false (and leaves *out unmodified) only if this
 * component was never initialized; a merely-invalid (cleared or never-set)
 * endpoint still fills *out with valid=false and returns true -- callers
 * should check out->valid, not just the return value. Safe to call from any
 * task. */
bool collector_endpoint_get(collector_endpoint_snapshot_t *out);

/* Convenience: same as collector_endpoint_get(&tmp) && tmp.valid, for a
 * caller that only needs the boolean. */
bool collector_endpoint_is_valid(void);

#ifdef __cplusplus
}
#endif
