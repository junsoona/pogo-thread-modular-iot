/*
 * Owns the ONE sensor driver this module is permanently paired with,
 * independent of ROOT/INTERMEDIATE/LEAF role (section 3 of the follow-up
 * spec). child_service uses sensor_service_measure_once() for its
 * SENSOR_HELLO response instead of touching sensor_provider_ops_t directly;
 * this component also runs the periodic real-data sampling task and, on
 * ROOT, the local sample aggregator.
 *
 * Responsibilities:
 *   - Copy+own module_config_t (same reasoning as child_service's s_cfg --
 *     see child_service.c).
 *   - Initialize the selected sensor_provider driver exactly once at boot;
 *     never re-initialize/re-select on a role change (section 2/3: no
 *     hot-swap, no re-discovery).
 *   - Serialize every measurement (provisioning AND periodic) through one
 *     mutex, so a periodic sample can never race a SENSOR_HELLO measurement.
 *   - Run the periodic measurement task at whatever effective period is
 *     currently set (see sensor_service_compute_effective_period_ms()).
 *   - ROOT: record its own permanent sensor's samples straight into the
 *     local aggregator (no wireless hop needed for its own data).
 *   - INTERMEDIATE/LEAF: hand a completed sample to the registered
 *     "sample ready" callback (wired to sensor_transport by module/main/
 *     app_main.c -- see that callback's doc comment for why the wiring goes
 *     through a callback instead of a direct component dependency).
 *   - Gate ONLY the wireless hand-off on Thread join state
 *     (sensor_service_set_thread_joined()) -- periodic measurement itself
 *     keeps running regardless (section 3: "Thread 미가입 상태에서는 무선
 *     전송만 중지").
 *   - Detach (UPSTREAM or DOWNSTREAM) never touches the driver or its
 *     config -- only role_manager/child_service/parent_service react to
 *     detach; sensor_service has nothing detach-specific to do at all.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "module_config/module_config.h"
#include "sensor_provider/sensor_provider.h"
#include "sensor_channel/sensor_channel.h"
#include "role_manager/role_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the one paired sensor driver (module_config.driver_id) exactly
 * once. Also performs one self-calibration measurement (doubling as the
 * "프로비저닝용 응답시간 측정" this component owns) to seed an initial
 * effective polling period via node_calc -- see sensor_service.c. */
esp_err_t sensor_service_init(const module_config_t *cfg);

/*
 * Mutex-protected single measurement: start_measurement()+read_measurement()
 * as one atomic unit. Used by BOTH child_service's SENSOR_HELLO handler and
 * this component's own periodic task, so the two can never measure
 * concurrently against the same physical sensor.
 */
esp_err_t sensor_service_measure_once(sensor_measurement_t *out, TickType_t timeout_ticks);

uint16_t sensor_service_get_sensor_bitmap(void);
sensor_status_t sensor_service_get_status(void);

/* The paired driver's own minimum polling period (0 if it has none). */
uint32_t sensor_service_get_driver_min_period_ms(void);

/*
 * effective_polling_period_ms = max(dynamic_period_ms, driver_minimum) --
 * the ONE place this combination happens (section 10 of the follow-up
 * spec), so a caller (child_service's POLL_CONFIG handler, or this
 * component's own ROOT self-calibration) never computes it two different
 * ways.
 */
uint32_t sensor_service_compute_effective_period_ms(uint32_t dynamic_period_ms);

/* Starts (first call) or retunes (later calls, e.g. a fresh POLL_CONFIG
 * after re-provisioning) the periodic sampling task at this already-combined
 * effective period. 0 pauses periodic sampling (kept at 0 until the first
 * real value is known). */
void sensor_service_set_polling_period_ms(uint32_t effective_period_ms);

/* role_manager's role-change callback should also reach here (ROOT commits
 * samples locally; INTERMEDIATE/LEAF hand them to the sample-ready
 * callback). */
void sensor_service_on_role_change(module_role_t new_role, module_role_t old_role);

/* Gates ONLY the wireless send step -- see this header's top comment. */
void sensor_service_set_thread_joined(bool joined);

#define SENSOR_SERVICE_UUID_LEN SENSOR_UUID_LEN

/* Called by child_service once ASSIGN_UUID is processed (the provisioning
 * UUID this module was given by its parent). Until this is called (ROOT, or
 * a not-yet-provisioned child), sensor_service_get_module_uuid() derives a
 * stable pseudo-UUID from this module's own MAC instead. */
void sensor_service_set_provisioned_uuid(const uint8_t uuid[SENSOR_SERVICE_UUID_LEN]);
void sensor_service_get_module_uuid(uint8_t out_uuid[SENSOR_SERVICE_UUID_LEN]);

/*
 * Registered by module/main/app_main.c once both sensor_service and
 * sensor_transport are initialized -- deliberately a callback, not a direct
 * sensor_service -> sensor_transport component dependency, because
 * sensor_transport already depends on sensor_service (to read local samples/
 * push ROOT-received ones into the aggregator) and a component dependency
 * cycle is not allowed. Called from sensor_service's own periodic task
 * context (INTERMEDIATE/LEAF only, and only while Thread-joined -- see
 * sensor_service_set_thread_joined()).
 */
typedef void (*sensor_service_sample_ready_cb_t)(const sensor_sample_t *sample);
void sensor_service_set_sample_ready_cb(sensor_service_sample_ready_cb_t cb);

/* --- ROOT-side local aggregator (also fed by sensor_transport's CoAP
 * resource handler for samples received from INTERMEDIATE/LEAF modules) ---
 *
 * The actual storage/dedup logic now lives in sensor_root_aggregator (see
 * that component's header for why: power_base_transport needs the exact
 * same logic with no sensor driver at all). Everything below is a thin,
 * behavior-preserving wrapper -- every existing caller (module/main/
 * app_main.c, sensor_transport.c) keeps using these exact names/signatures. */
#include "sensor_root_aggregator/sensor_root_aggregator.h"

#define SENSOR_SERVICE_MAX_NODES SENSOR_ROOT_AGGREGATOR_MAX_NODES

typedef sensor_root_aggregator_node_t sensor_service_node_record_t;

/*
 * Records one sample (this module's own, if ROOT, or one relayed up over
 * CoAP from a child). Deduplicates by (module_uuid, sample_seq): an exact
 * repeat of the last-recorded seq for that UUID is dropped without
 * re-storing or double-counting (section 8: "(UUID, sample_seq)로 중복을
 * 제거한다"). A brand-new UUID takes the first free/oldest slot once
 * SENSOR_SERVICE_MAX_NODES is reached (bounded, permanently-paired
 * installation -- see the follow-up spec's section 0: modules/sensors are
 * never hot-swapped or auto-discovered at scale).
 */
void sensor_service_root_record_sample(const sensor_sample_t *sample);

/* Increments a node's error_count (a malformed/duplicate-rejected/otherwise
 * bad delivery attributable to a known module_uuid) without touching its
 * last_sample. */
void sensor_service_root_record_error(const uint8_t module_uuid[SENSOR_SERVICE_UUID_LEN]);

size_t sensor_service_root_get_node_count(void);
/* Returns false if index is out of range or that slot has never been used. */
bool sensor_service_root_get_node(size_t index, sensor_service_node_record_t *out);

#ifdef __cplusplus
}
#endif
