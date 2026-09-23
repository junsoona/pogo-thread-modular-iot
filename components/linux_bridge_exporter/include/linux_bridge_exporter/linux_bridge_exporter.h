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
 * Runs its own dedicated FreeRTOS task with a bounded event queue (TOPOLOGY
 * events) and a bounded, uuid-coalescing table (SAMPLE events -- see
 * linux_bridge_exporter.c's own top comment for why the two are handled
 * differently). Every power_base_transport_bridge_observer_t callback this
 * component registers (on_sample_stored/on_topology_applied/on_node_offline)
 * fires from within the OpenThread task's own context and does NOTHING but
 * a STRICTLY non-blocking (0-tick) xQueueSend()/xSemaphoreTake() attempt --
 * on_topology_applied/on_node_offline enqueue into the topology queue,
 * on_sample_stored upserts into the pending-sample table -- never a CoAP
 * send, never a wait that could actually block the OpenThread task. See
 * linux_bridge_exporter.c's own header comment for the full design,
 * including what happens when a 0-tick attempt misses (the event is
 * dropped with a rate-limited warning, never retried in a loop; the
 * periodic full topology+sample resync this task also runs is what keeps
 * the Linux side eventually consistent regardless of any dropped event --
 * see this codebase's requirement that a stalled/unreachable Linux host
 * must NEVER affect POGO provisioning, Thread join, "/s"/"/t" handling,
 * aggregator storage, the topology registry, or any other sensor module's
 * own communication).
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/*
 * Correction pass (this follow-up, item 6: exporter timing host tests must
 * not depend on real wall-clock elapsed time -- see this header's own top
 * comment on exporter_task()'s periodic resync/heartbeat). TEST-ONLY: never
 * called by production code (module/power_base main.c never references
 * this symbol). Installs `now_ms_fn` as the source exporter_task() reads
 * instead of the real xTaskGetTickCount() for its own full-resync/heartbeat
 * interval comparisons ONLY -- NOT drain_pending_samples()'s separate, much
 * shorter per-uuid SAMPLE_MIN_INTERVAL_MS rate-limit gate (existing tests
 * exercise that one opening/closing over genuinely elapsed real time by
 * design), and not send timeouts/retry backoff (real blocking waits on
 * real synchronization primitives, not simple elapsed-time comparisons,
 * and not the source of the flakiness this fixes). Passing NULL (the
 * default, and the only thing production ever
 * implicitly has) restores the real clock -- so a host binary that never
 * calls this function is BYTE-FOR-BYTE the same as before this hook
 * existed. A test installs a fully test-controlled fake clock (e.g. an
 * atomic counter it bumps forward directly) to make a resync/heartbeat
 * interval elapse deterministically, on demand, regardless of how much
 * cumulative real wall-clock time the rest of the test process has used --
 * exporter_task() itself still polls on its own bounded, real ~200ms tick
 * (EXPORTER_POLL_TICK_MS), so a test only ever waits a bounded ~200ms of
 * real time for the NEXT poll to notice the fake clock has advanced, never
 * the real interval's own full duration.
 */
void linux_bridge_exporter_test_set_clock(TickType_t (*now_ms_fn)(void));

#ifdef __cplusplus
}
#endif
