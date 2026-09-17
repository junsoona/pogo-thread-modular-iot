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
 * This follow-up (correction pass on the northbound bridge transport):
 * power_base_transport's "/s" handler needs to know whether a sample was
 * ACTUALLY newly stored before it fires its bridge-observer's
 * on_sample_stored() or reports a CoAP success code -- a caller that only
 * ever saw a void return had no way to distinguish "stored", "an idempotent
 * duplicate retransmit", "the aggregator was momentarily too busy to take
 * its own mutex", or "the table is full" (all four were previously folded
 * into the same silent no-return-value outcome, and the CoAP layer once
 * incorrectly reported 2.04 Changed / exported to Linux even when nothing
 * was actually stored).
 */
typedef enum {
    SENSOR_ROOT_AGGREGATOR_STORED,    /* a genuinely new (module_uuid, sample_seq) was recorded */
    SENSOR_ROOT_AGGREGATOR_DUPLICATE, /* exact repeat of the last-recorded seq for this uuid -- not re-stored */
    SENSOR_ROOT_AGGREGATOR_BUSY,      /* could not acquire the aggregator's own mutex in time -- nothing stored */
    SENSOR_ROOT_AGGREGATOR_FULL,      /* a brand-new uuid, but the table is already full -- nothing stored */
    SENSOR_ROOT_AGGREGATOR_INVALID,   /* sample == NULL, or the aggregator was never initialized */
} sensor_root_aggregator_result_t;

/*
 * Records one sample (this node's own, or one relayed up over CoAP) and
 * reports exactly what happened -- see sensor_root_aggregator_result_t
 * above. Deduplicates by (module_uuid, sample_seq): an exact repeat of the
 * last-recorded seq for that UUID is dropped without re-storing
 * (DUPLICATE). A brand-new UUID past SENSOR_ROOT_AGGREGATOR_MAX_NODES is
 * dropped and logged (FULL), never overwriting an existing node's slot.
 * Only SENSOR_ROOT_AGGREGATOR_STORED means new data now sits in this
 * aggregator -- every other result means the aggregator's stored state is
 * completely unchanged by this call.
 */
sensor_root_aggregator_result_t sensor_root_aggregator_record_sample_result(const sensor_sample_t *sample);

/*
 * Back-compat wrapper for every pre-existing caller that only ever needed
 * "store it, I don't need to know what happened" (sensor_service.c's
 * MODULE_ROLE_ROOT/DEV_ROOT path) -- behaviorally IDENTICAL to before this
 * follow-up, just implemented in terms of
 * sensor_root_aggregator_record_sample_result() now instead of duplicating
 * the storage logic.
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
