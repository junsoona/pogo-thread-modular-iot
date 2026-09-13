/*
 * parent_service owns the DOWNSTREAM pogo_port only (section 7): it drives
 * the full downstream provisioning sequence (section 12) -- IDENTIFY,
 * UUID/slot(+depth) assignment, ASSIGN_PARENT_INFO (power-base follow-up
 * section 7), SENSOR_HELLO/SENSOR_INFO_ACK, the computed POLL_CONFIG, the
 * Active Dataset TLV handoff, and the JOIN_RESULT wait.
 *
 * This component is reused VERBATIM by both a sensor module (ROOT/
 * INTERMEDIATE role) and the dedicated power-base project -- neither is a
 * compile-time choice. Rather than reaching into role_manager/child_service
 * directly (which a power base does not have -- it has no UPSTREAM port and
 * never runs role_manager at all, see docs/power-base.md), the caller
 * supplies a small parent_service_context_t at init: "may I provision a
 * child right now", "what is my own topology depth" (0 for ROOT/power base
 * itself; parent_service always assigns depth+1 downstream), "what is my own
 * UUID" (for ASSIGN_PARENT_INFO), and an optional per-call ACK-timeout
 * override (0 = the pogo_proto Kconfig default -- see CONFIG_POGO_ACK_TIMEOUT_MS).
 * A sensor module's app_main.c wires this to role_manager/child_service/
 * sensor_service; power_base/main/app_main.c wires it to its own
 * always-ready-once-Leader / fixed-depth-0 / power_base_config-derived
 * identity instead. Neither side is "pretending" to be the other -- see
 * docs/power-base.md's "POGO provisioning reuse" section.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "pogo_port/pogo_port.h"
#include "pogo_proto/pogo_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* True if a docked child may be provisioned right now. A sensor module
     * wires this to (role==ROOT||role==INTERMEDIATE) && role_manager_is_upstream_ready();
     * a power base wires it to "my own Thread network has reached
     * Leader/Router" (see docs/power-base.md's boot sequence). Re-checked on
     * every loop iteration while a peer is docked and idle -- never assumed
     * true just because it was true once. */
    bool (*provisioning_allowed)(void *ctx);
    /* This parent's OWN topology depth: 0 for the true network root (ROOT
     * sensor module fixture, or the power base itself), or this module's own
     * child_service_get_own_topology_depth() otherwise. The child docked on
     * this parent is always assigned depth+1. */
    uint8_t (*get_own_depth)(void *ctx);
    /* Fills out_uuid (POGO_UUID_LEN bytes) with this parent's own identity,
     * for ASSIGN_PARENT_INFO. A sensor module uses
     * sensor_service_get_module_uuid(); the power base uses its own
     * efuse/NVS-derived stable UUID (see power_base_identity.h). */
    void (*get_own_uuid)(uint8_t out_uuid[POGO_UUID_LEN], void *ctx);
    /* Role name for logging only (e.g. "ROOT", "INTERMEDIATE", "POWER_BASE"). */
    const char *(*role_name)(void *ctx);
    /* True if THIS parent is the dedicated power-base project (its own POGO
     * peer type is POGO_PEER_TYPE_POWER_BASE) -- carried in
     * ASSIGN_PARENT_INFO.parent_is_power_base so the child can cross-check it
     * against its own BASE_ID+PEER_HELLO-derived role. */
    bool is_power_base;
    /* 0 = use CONFIG_POGO_ACK_TIMEOUT_MS (the pogo_proto Kconfig default, and
     * every sensor module's choice, unchanged from before this refactor);
     * otherwise this many ms per provisioning-step retry attempt. Lets
     * power_base_config's handshake_timeout_ms field genuinely control
     * something instead of being a decorative field nobody reads. */
    uint32_t ack_timeout_ms;
    void *ctx;
} parent_service_context_t;

/*
 * No module_config_t parameter here (an earlier revision took one, unused --
 * power_base has no module_config_t at all, so a genuinely-needed parameter
 * would only ever have been a sensor-module concept leaking into a
 * component both projects share). Everything this component needs comes
 * from `provider`.
 */
esp_err_t parent_service_init(const parent_service_context_t *provider);

/* Feed every DOWNSTREAM pogo_port event here (events for UPSTREAM are
 * ignored). Safe to call from the pogo_port callback context -- only enqueues. */
void parent_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event,
                                   const pogo_port_event_data_t *data);

/*
 * Pure, integer-only polling-period calculation (section 12):
 *   RT<50ms:M=15  50<=RT<100:M=10  100<=RT<500:M=8
 *   500<=RT<=1000:M=3  1000<RT<=10000:M=1.5 (as RT*3/2)  RT>10000:M=1
 * Returns ESP_ERR_INVALID_ARG for response_time_ms==0 (treated as a
 * measurement error, not "instant"), ESP_ERR_INVALID_SIZE if the result
 * would not fit in uint32_t. Exposed directly for the boundary-value checks
 * in section 17 of the implementation spec.
 */
esp_err_t parent_service_calc_polling_period_ms(uint32_t response_time_ms, uint32_t *out_period_ms);

/*
 * Pure UUID-hash backoff (section 13): low 8 bits of an FNV-1a hash over the
 * 8 UUID bytes, i.e. always in [0, 255]. The actual retransmission use of
 * this value is future work -- only the node record field and this
 * calculation are in scope here.
 */
uint16_t parent_service_calc_backoff_ms(const uint8_t uuid[POGO_UUID_LEN]);

#ifdef __cplusplus
}
#endif
