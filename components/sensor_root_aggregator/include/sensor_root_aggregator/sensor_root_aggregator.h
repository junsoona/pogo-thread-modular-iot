/*
 * The bounded, mutex-protected, (module_uuid, sample_seq)-deduplicating
 * sensor-sample aggregator (section 8/13 of the follow-up spec: "CoAP
 * duplicate suppression"), extracted out of sensor_service so it can be used
 * by TWO different final destinations without either depending on a sensor
 * driver:
 *
 *   - sensor_service (module/, when role_manager forces MODULE_ROLE_ROOT via
 *     the DEV_ROOT regression fixture, or -- academically -- a passive
 *     power-base bench rig): its own permanently-paired sensor's samples are
 *     recorded here directly, and sensor_transport's CoAP "/s" resource
 *     handler forwards samples relayed up from INTERMEDIATE/LEAF children
 *     here too. sensor_service.c's sensor_service_root_record_sample()/
 *     _record_error()/_get_node_count()/_get_node() are now thin wrappers
 *     around this component, unchanged in behavior or signature for every
 *     existing caller (see docs/architecture.md).
 *   - power_base_transport (power_base/, the ONLY real ROOT in the final
 *     system): has no sensor of its own at all, so it must not depend on
 *     sensor_service (which owns a sensor_provider driver init that would be
 *     fake/unused on power hardware -- forbidden by this refactor's own
 *     ground rules). It uses this component directly instead.
 *
 * No ESP-IDF dependency beyond FreeRTOS (mutex) + esp_timer (timestamps) --
 * no module_config, no sensor_provider.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sensor_channel/sensor_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_ROOT_AGGREGATOR_MAX_NODES 16

typedef struct {
    bool     valid;
    uint8_t  module_uuid[SENSOR_UUID_LEN];
    bool     has_sample;
    uint32_t last_sample_seq;
    int64_t  last_seen_us;
    uint32_t error_count;
    sensor_sample_t last_sample;
} sensor_root_aggregator_node_t;

/* Allocates the mutex and clears all node slots. Call once, before any
 * record/query call. Safe to call again (e.g. from a host test) -- it simply
 * re-clears state. */
esp_err_t sensor_root_aggregator_init(void);

/*
 * Records one sample (this node's own, or one relayed up over CoAP).
 * Deduplicates by (module_uuid, sample_seq): an exact repeat of the
 * last-recorded seq for that UUID is dropped without re-storing. A
 * brand-new UUID past SENSOR_ROOT_AGGREGATOR_MAX_NODES is dropped and
 * logged, never overwriting an existing node's slot.
 */
void sensor_root_aggregator_record_sample(const sensor_sample_t *sample);

/* Increments a node's error_count without touching its last_sample. A
 * completely unknown module_uuid is a no-op (there is no slot to attribute
 * the error to) -- see docs/power-base.md's "알려지지 않은 UUID" handling,
 * which logs the error itself separately, at the transport layer. */
void sensor_root_aggregator_record_error(const uint8_t module_uuid[SENSOR_UUID_LEN]);

size_t sensor_root_aggregator_get_node_count(void);
/* Returns false if index is out of range or that slot has never been used. */
bool sensor_root_aggregator_get_node(size_t index, sensor_root_aggregator_node_t *out);

#ifdef __cplusplus
}
#endif
