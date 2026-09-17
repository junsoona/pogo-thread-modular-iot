/*
 * Power Base -> Linux OTBR host northbound bridge exporter (this follow-up:
 * "Matter endpoint를 바로 만들기 전에 Power Base의 aggregator 데이터를 Linux
 * OTBR 호스트로 전달하는 northbound bridge transport"). An OPTIONAL,
 * best-effort mirror -- components/power_base_transport (topology_registry +
 * sensor_root_aggregator) remains the sole authoritative collector either
 * way; this component only ever forwards a copy of what it already decided.
 *
 * Wiring (no circular dependency): this component depends on
 * components/power_base_transport (for its bridge-observer types and
 * get_node_count()/get_node() accessors) and registers itself as that
 * component's bridge observer FROM WITHIN linux_bridge_exporter_init()
 * itself -- power_base_transport never depends on this component or even
 * knows it exists by name (see power_base_transport.h's own top comment on
 * that observer interface).
 *
 * Runs its own dedicated FreeRTOS task with a bounded event queue. Every
 * power_base_transport_bridge_observer_t callback this component registers
 * (on_sample_stored/on_topology_applied/on_node_offline) fires from within
 * the OpenThread task's own context and does NOTHING but a non-blocking
 * xQueueSend() -- see linux_bridge_exporter.c's own header comment for the
 * full design, including what happens on queue overflow (the event is
 * dropped with a rate-limited warning; the periodic 5-second full topology
 * resync this task also runs is what keeps the Linux side eventually
 * consistent regardless of any dropped event -- see this codebase's
 * requirement that a stalled/unreachable Linux host must NEVER affect POGO
 * provisioning, Thread join, "/s"/"/t" handling, aggregator storage, the
 * topology registry, or any other sensor module's own communication).
 *
 * Sends components/bridge_export_proto-encoded messages as OpenThread CoAP
 * Confirmable POSTs to the configured Linux OTBR host, port, and resources
 * "/pogo/v1/sample", "/pogo/v1/topology", "/pogo/v1/heartbeat" -- reusing
 * components/coap_response_timing's shared response-wait derivation (same
 * CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS Kconfig source of truth
 * sensor_transport.c/topology_reporter.c already use, so this never
 * independently drifts from the rest of this codebase's CoAP timing policy).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "power_base_bridge_config/power_base_bridge_config.h"
#include "power_base_transport/power_base_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the exporter's own task and registers it as power_base_transport's
 * bridge observer. Call once, after power_base_transport_init() (this
 * exporter's own observer callbacks assume the registry/aggregator already
 * exist) and after Thread is joined enough for esp_openthread_get_instance()
 * to be valid (same ordering constraint sensor_transport_init()/
 * topology_reporter_init() already have).
 *
 * cfg->enabled MUST be true when this is called (the caller,
 * power_base/main/app_main.c, is expected to simply not call this at all
 * when components/power_base_bridge_config reported the exporter disabled --
 * see that component's own header comment on why a disabled/absent
 * "pb_bridge" config is never a boot failure). Passing an already-disabled
 * cfg here returns ESP_OK without starting anything, as a defensive no-op,
 * but callers should prefer not calling this function at all in that case.
 *
 * Returns a genuine resource-creation failure (ESP_ERR_NO_MEM, task-create
 * failure) as non-ESP_OK -- per this follow-up's own boot policy (see
 * power_base_bridge_config.h), the caller must NOT treat this as a reason to
 * halt the whole power base; it only means the optional Linux mirror does
 * not run this boot.
 */
esp_err_t linux_bridge_exporter_init(const uint8_t power_base_uuid[TOPOLOGY_UUID_LEN],
                                      const power_base_bridge_config_t *cfg);

#ifdef __cplusplus
}
#endif
