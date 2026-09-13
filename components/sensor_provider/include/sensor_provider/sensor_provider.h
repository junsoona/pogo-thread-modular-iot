/*
 * Common interface for the ONE sensor driver a module is permanently paired
 * with (see section 2 of the implementation spec: no hot-swap, no driver
 * auto-detection, no fallback to a different driver_id). module_config.driver_id
 * selects exactly one entry from the built-in registry at boot; if it does
 * not match a supported driver, that is CONFIG_ERROR, not "try the next one".
 *
 * response_time_ms is never a compile-time constant: it is measured by the
 * driver itself, from start_measurement() to a ready read_measurement(), and
 * is what parent_service turns into a polling period (see docs/state-machine.md).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "module_config/module_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENSOR_STATUS_OK               = 0,
    SENSOR_STATUS_NOT_INITIALIZED  = 1,
    SENSOR_STATUS_TIMEOUT          = 2,
    SENSOR_STATUS_BUS_ERROR        = 3,
    SENSOR_STATUS_DRIVER_ERROR     = 4,
} sensor_status_t;

typedef struct {
    uint16_t sensor_type_bitmap;
    uint32_t response_time_ms; /* measured elapsed time for this one reading */
} sensor_measurement_t;

/* Opaque per-driver instance; each driver defines its own concrete struct
 * and only ever hands out a pointer to it. */
typedef struct sensor_provider_ctx_s sensor_provider_ctx_t;

typedef struct {
    const char *name;

    /* Allocates/initializes the driver against the fixed board config
     * (bus wiring, calibration, measurement_timeout_ms). */
    esp_err_t (*init)(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx);

    /* Fixed for a given paired sensor; does not change across role changes. */
    uint16_t (*get_sensor_bitmap)(sensor_provider_ctx_t *ctx);

    /* Begins one measurement cycle (non-blocking). */
    esp_err_t (*start_measurement)(sensor_provider_ctx_t *ctx);

    /*
     * Blocks (up to timeout_ticks) until the measurement begun by
     * start_measurement() completes or measurement_timeout_ms elapses.
     * On success, out->response_time_ms is the actual elapsed time for
     * *this* measurement -- callers must not assume it matches any
     * previous call.
     */
    esp_err_t (*read_measurement)(sensor_provider_ctx_t *ctx, sensor_measurement_t *out, TickType_t timeout_ticks);

    sensor_status_t (*get_status)(sensor_provider_ctx_t *ctx);

    void (*deinit)(sensor_provider_ctx_t *ctx);
} sensor_provider_ops_t;

/*
 * Reserved driver_id for the development-only mock sensor. A production
 * module_config.bin (dev override AUTO) must never select this -- module
 * main.c enforces that (see docs/module-config.md).
 */
#define SENSOR_DRIVER_ID_MOCK 0x4D4F434Bu /* ASCII "MOCK" */

/* Returns NULL if driver_id names no driver compiled into this firmware.
 * Callers must treat NULL as CONFIG_ERROR: do not probe other driver_ids. */
const sensor_provider_ops_t *sensor_provider_lookup(uint32_t driver_id);

#ifdef __cplusplus
}
#endif
