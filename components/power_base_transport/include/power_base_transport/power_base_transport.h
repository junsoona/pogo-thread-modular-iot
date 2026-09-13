/*
 * Power base's Thread CoAP receiving side (section 10 of the power-base
 * follow-up spec): the FINAL destination for every sensor module's periodic
 * samples ("/s", reusing sensor_channel's existing wire format unmodified)
 * AND every topology report ("/t", components/topology_proto). Owns the
 * topology_registry_t instance and its mutex (the registry itself has none
 * of its own -- see topology_registry.h) and the sensor_root_aggregator
 * singleton (no sensor_service/sensor_provider dependency at all: the power
 * base has no sensor of its own).
 *
 * A malformed frame, an unknown UUID, or a validation-rejected topology
 * report is logged and dropped -- never crashes. A Confirmable request
 * always gets a response either way, but (code review follow-up section 3)
 * the CoAP response CODE now distinguishes outcomes instead of always
 * replying 2.04 Changed:
 *
 *   2.04 Changed   -- newly applied (a real state change)
 *   2.03 Valid     -- an idempotent exact-duplicate retransmit (already
 *                     applied, nothing new -- see
 *                     TOPOLOGY_REGISTRY_OK_DUPLICATE)
 *   4.00 Bad Request     -- malformed/unparseable payload
 *   4.04 Not Found       -- "/s" sensor sample from a UUID never reported to
 *                           the topology registry at all
 *   4.12 Precondition Failed -- well-formed but semantically rejected: any
 *                           topology_registry_result_t error (self-parent,
 *                           cycle, depth mismatch, stale/duplicate session,
 *                           ...), OR a "/s" sample from a KNOWN but OFFLINE
 *                           uuid, OR a sensor_type_bitmap that is not a
 *                           subset of that node's own reported bitmap
 *   5.03 Service Unavailable -- couldn't acquire the registry mutex in time,
 *                           or the registry table is full (TOPOLOGY_REGISTRY_ERR_FULL)
 *
 * (OpenThread's CoAP implementation only defines the base RFC 7252 code set
 * -- there is no distinct "4.09 Conflict" available, hence folding
 * conflict/stale-session into 4.12 Precondition Failed.)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "topology_proto/topology_proto.h"
#include "topology_registry/topology_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the CoAP server and registers both resources. Call once, after
 * thread_manager_init() (needs the OT instance) and once this power base's
 * own stable identity (power_base_identity.h) is known.
 */
esp_err_t power_base_transport_init(const uint8_t power_base_uuid[TOPOLOGY_UUID_LEN]);

/*
 * Call when this power base's own physical DOWNSTREAM POGO port detaches
 * (a direct pogo_port DETACHED event) -- an immediate hardware signal, so
 * every currently-online depth==1 registry node (and, via cascade, its whole
 * subtree) is marked offline right away instead of waiting for a stale
 * timeout (section 9: "전원 모듈의 직접 연결이 끊기면 첫 번째 모듈과 그 모든
 * 하위 항목을 offline 처리").
 */
void power_base_transport_on_direct_downstream_detached(void);

/*
 * Marks any currently-online node whose last report is older than
 * stale_timeout_ms as offline. Call periodically (e.g. once every few
 * seconds), not on every CoAP request.
 */
void power_base_transport_sweep_stale(uint32_t now_ms, uint32_t stale_timeout_ms);

size_t power_base_transport_get_node_count(void);
bool power_base_transport_get_node(size_t index, topology_registry_entry_t *out);

#ifdef __cplusplus
}
#endif
