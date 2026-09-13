/*
 * Pure, integer-only per-node scheduling math shared by parent_service (the
 * original owner of both functions) and sensor_service/sensor_transport (new
 * consumers, section 8 of the follow-up spec) -- pulled out into its own
 * dependency-free component specifically so sensor_service does not have to
 * REQUIRES parent_service, which would create a component dependency cycle
 * once child_service also depends on sensor_service (section 3):
 * sensor_service -> parent_service -> child_service -> sensor_service.
 *
 * parent_service_calc_polling_period_ms()/parent_service_calc_backoff_ms()
 * (components/parent_service/parent_service.h) keep their existing names and
 * signatures as thin wrappers around this component, so nothing that already
 * calls them (including tools/test_polling_calc.c, which independently
 * duplicates this exact logic for host testing and is unaffected either way)
 * needs to change.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_CALC_UUID_LEN 8

/*
 * response_time_ms -> polling_period_ms (section 12 of the original
 * implementation spec):
 *   RT<50ms:M=15  50<=RT<100:M=10  100<=RT<500:M=8
 *   500<=RT<=1000:M=3  1000<RT<=10000:M=1.5 (as RT*3/2)  RT>10000:M=1
 * ESP_ERR_INVALID_ARG for response_time_ms==0 (a measurement error, not
 * "instant"); ESP_ERR_INVALID_SIZE if the result would not fit uint32_t.
 */
esp_err_t node_calc_polling_period_ms(uint32_t response_time_ms, uint32_t *out_period_ms);

/*
 * Full 32-bit FNV-1a hash over an 8-byte UUID -- the source value behind
 * node_calc_backoff_ms() below (its low 8 bits) AND, since section 8 of the
 * follow-up spec, sensor_transport's widening per-retry backoff ranges
 * (masking more of the same bits for each successive retry: & 0xFF, &
 * 0x1FF, & 0x3FF for 0..255ms/0..511ms/0..1023ms), so every retry's delay is
 * still deterministically derived from the one per-module UUID hash instead
 * of a second, unrelated random source.
 */
uint32_t node_calc_uuid_hash(const uint8_t uuid[NODE_CALC_UUID_LEN]);

/*
 * Low 8 bits of node_calc_uuid_hash(), i.e. always in [0, 255] (section 13
 * of the original implementation spec). Used as the node record's
 * diagnostic backoff_delay_ms.
 */
uint16_t node_calc_backoff_ms(const uint8_t uuid[NODE_CALC_UUID_LEN]);

#ifdef __cplusplus
}
#endif
