/*
 * parent_service owns the DOWNSTREAM pogo_port only (section 7): it drives
 * the full downstream provisioning sequence (section 12) -- IDENTIFY,
 * UUID/slot(+depth) assignment, SENSOR_HELLO/SENSOR_INFO_ACK, the computed
 * POLL_CONFIG, the Active Dataset TLV handoff, and the JOIN_RESULT wait.
 * Runs for role_manager roles ROOT and INTERMEDIATE; LEAF never runs it.
 *
 * Per section 7, it must not start a session until role_manager_is_upstream_ready()
 * is true, even if a DOWNSTREAM module docked earlier -- it defers and
 * re-checks instead.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "module_config/module_config.h"
#include "pogo_port/pogo_port.h"
#include "pogo_proto/pogo_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t parent_service_init(const module_config_t *cfg);

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
