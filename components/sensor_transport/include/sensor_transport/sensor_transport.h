/*
 * Thread data-plane (CoAP) transport for periodic sensor samples (section 8
 * of the follow-up spec) -- deliberately separate from components/pogo_proto
 * (the wired POGO provisioning protocol, left alone) and from
 * components/sensor_channel (the wire format this component moves, but
 * never interprets itself).
 *
 * ROOT: starts an OpenThread CoAP resource ("s") that accepts a
 * Confirmable POST of one sensor_channel_pack()-encoded sample, unpacks it,
 * hands it to sensor_service_root_record_sample() (which itself dedups by
 * (module_uuid, sample_seq)), and CoAP-ACKs it.
 *
 * INTERMEDIATE/LEAF: sensor_transport_send_sample() is registered (by
 * module/main/app_main.c) as sensor_service's sample-ready callback -- see
 * that callback's own doc comment for why the wiring goes through a
 * function pointer instead of a direct sensor_service -> sensor_transport
 * component dependency. Sends the sample as one Confirmable POST to the
 * CURRENT Leader/ROOT address (thread_manager_get_leader_address() -- never
 * a hardcoded address), retrying up to 3 times total with the UUID-hash
 * backoff schedule from section 8 (0..255ms, then 0..511ms, then
 * 0..1023ms) if no CoAP response arrives.
 *
 * Topology-registration gate (real-hardware first-sample/topology race,
 * see components/topology_gate's header comment): when this module is
 * running the AUTO path against an active power base
 * (`require_topology_registration` passed to sensor_transport_init()),
 * sensor_transport_send_sample() additionally refuses to even attempt a
 * "/s" wireless send while `topology_gate_is_open()` is false -- i.e. before
 * this module's own TOPOLOGY_REPORT_ATTACHED has received a real CoAP 2.xx
 * response from the power base's "/t" resource. This never touches the
 * bounded per-attempt CoAP retry/backoff schedule above; it just skips the
 * whole send attempt entirely (a quiet ESP_LOGD, not a per-cycle warning)
 * until the gate opens. DEV_ROOT/DEV_LEAF (module-to-module regression,
 * whose peer never runs power_base_transport and so never has a "/t"
 * resource at all) pass `require_topology_registration=false` and are
 * completely unaffected -- see docs/power-base.md.
 *
 * Power base reboot recovery: the power base's topology_registry is
 * RAM-only, so a reboot can make it reject an already-registered module's
 * sample as CoAP 4.04 Not Found ("unknown module"). When that happens (and
 * only when `require_topology_registration` is true), sensor_transport
 * closes the gate right away and calls topology_gate_request_reregister()
 * so topology_reporter re-attaches on its very next poll tick instead of
 * waiting for its normal heartbeat schedule.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sensor_channel/sensor_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the CoAP server and registers the sample-receiving resource. Call
 * once, after thread_manager_init() has brought up the OpenThread stack
 * (Thread itself need not be joined yet -- CoAP only needs the OT instance,
 * not network membership, to bind its UDP port) and after topology_gate_init()
 * (needed whenever require_topology_registration is true). Safe to call on
 * every role: only ROOT ever actually receives a POST here in practice,
 * since a client only ever addresses the current Leader (see
 * thread_manager_get_leader_address()), which in this topology is always
 * ROOT.
 *
 * require_topology_registration: true for the AUTO path against an active
 * power base (this module's own topology_reporter registers "/t" reports
 * with it) -- sensor_transport_send_sample() then also gates on
 * topology_gate_is_open(), see this header's top comment. false for
 * DEV_ROOT/DEV_LEAF module-to-module regression, whose peer has no "/t"
 * resource at all: sending stays gated on Thread-joined only, exactly as
 * before this gate existed.
 */
esp_err_t sensor_transport_init(bool require_topology_registration);

/*
 * Sends one sample toward ROOT. Never called for ROOT's own samples --
 * sensor_service records those straight into its local aggregator instead
 * (see sensor_service_root_record_sample()). Blocking: returns once the
 * sample was CoAP-acknowledged, all retries were exhausted, or Thread is not
 * currently joined (in which case this returns immediately without ever
 * touching the radio -- section 9: no busy-retry loop while detached).
 */
void sensor_transport_send_sample(const sensor_sample_t *sample);

#ifdef __cplusplus
}
#endif
