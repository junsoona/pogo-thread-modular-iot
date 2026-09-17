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
 *                           the registry table is full (TOPOLOGY_REGISTRY_ERR_FULL),
 *                           or (correction pass) a "/s" sample that
 *                           sensor_root_aggregator itself reports as
 *                           SENSOR_ROOT_AGGREGATOR_BUSY/_FULL -- i.e. nothing
 *                           was actually stored, so this is never reported
 *                           as 2.04 Changed
 *

 * (OpenThread's CoAP implementation only defines the base RFC 7252 code set
 * -- there is no distinct "4.09 Conflict" available, hence folding
 * conflict/stale-session into 4.12 Precondition Failed.)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "sensor_channel/sensor_channel.h"
#include "topology_proto/topology_proto.h"
#include "topology_registry/topology_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Northbound bridge observer (this follow-up: "Power Base의 aggregator
 * 데이터를 Linux OTBR 호스트로 전달하는 northbound bridge transport"). A
 * generic callback interface -- this header/this.c file never mentions
 * components/linux_bridge_exporter by name, and linux_bridge_exporter never
 * needs to be a REQUIRES of this component -- so there is no circular
 * dependency: linux_bridge_exporter depends on power_base_transport (for
 * these types and topology_registry_entry_t), never the other way around.
 * power_base/main/app_main.c is the only place that wires the two together
 * (power_base_transport_set_bridge_observer(&observer) where `observer`'s
 * function pointers are linux_bridge_exporter's own).
 *
 * At most one observer is ever registered (a single power base has a single
 * optional northbound mirror) -- a second call to
 * power_base_transport_set_bridge_observer() simply replaces the first.
 * Registering NULL (or never registering at all, the default) disables every
 * callback below with zero overhead beyond one NULL check per event -- this
 * is exactly what happens when components/power_base_bridge_config reports
 * the exporter disabled, and it is what keeps "/s"/"/t" handling and
 * topology_registry/sensor_root_aggregator entirely independent of whether
 * any Linux host is even configured, let alone reachable (this follow-up's
 * own hard requirement: "Linux 수신기가 정지하거나 응답하지 않아도 ...
 * 계속 동작해야 한다").
 *
 * Every callback here fires AFTER the corresponding state change has
 * already been fully applied and (where relevant) every mutex released --
 * never from inside a critical section, and never blocking the CoAP
 * handler/stale-sweep caller on anything the observer itself might do (a
 * well-behaved observer, like linux_bridge_exporter, only ever does a
 * non-blocking bounded-queue enqueue from within these callbacks -- see that
 * component's own header comment).
 */
typedef struct {
    void *ctx;
    /* A new (module_uuid, sample_seq) sample was ACTUALLY newly recorded by
     * sensor_root_aggregator -- i.e.
     * sensor_root_aggregator_record_sample_result() returned
     * SENSOR_ROOT_AGGREGATOR_STORED, never for an exact-duplicate retransmit
     * (DUPLICATE), a momentarily-busy or full aggregator (BUSY/FULL -- see
     * power_base_transport.c's coap_sensor_handler(), which also never
     * reports these as CoAP 2.04 to the sender), or an unknown/offline uuid
     * or a bitmap-mismatched sample (all three rejected even earlier, before
     * the aggregator is ever called at all). */
    void (*on_sample_stored)(const sensor_sample_t *sample, void *ctx);
    /* A topology report was applied to the registry as a REAL change
     * (topology_registry_apply_report() returned TOPOLOGY_REGISTRY_OK, never
     * OK_DUPLICATE or any ERR_* value) -- `entry` is a snapshot of that
     * node's own registry entry immediately after the apply. */
    void (*on_topology_applied)(const topology_registry_entry_t *entry, void *ctx);
    /* A node's online flag flipped from true to false for a reason OTHER
     * than that node's own topology report (a direct DOWNSTREAM POGO detach
     * cascade, a report elsewhere in the tree cascading this node's subtree
     * offline, or a stale-timeout sweep). Fired once per affected uuid.
     * NOT fired for the reporting node of a DETACHED/offline-transitioning
     * report itself -- that node's own on_topology_applied callback already
     * conveys its new online=false state. */
    void (*on_node_offline)(const uint8_t uuid[TOPOLOGY_UUID_LEN], void *ctx);
} power_base_transport_bridge_observer_t;

/* Registers (or, passing NULL, clears) the bridge observer -- see this
 * header's own top comment. Call from power_base/main/app_main.c only,
 * before power_base_transport_init() is exercised by real traffic (there is
 * no internal locking around the observer pointer itself: it is intended to
 * be set once, at boot, exactly like every other wiring callback in this
 * codebase -- pb_get_collector, port_event_dispatcher, ...). */
void power_base_transport_set_bridge_observer(const power_base_transport_bridge_observer_t *observer);

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
