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
 *
 * driver_id values are the single common registry the follow-up spec's
 * section 2 asks for ("driver ID와 sensor bitmap은 명시적인 공통 헤더에서
 * 관리한다") -- configs JSON files (see configs/prototype_sht45.json etc.)
 * reference these same numeric values by name, resolved by
 * tools/pack_module_config.py's DRIVER_IDS table, which mirrors this list
 * with a "must match" comment (the same pattern already used for
 * SENSOR_DRIVER_ID_MOCK), and nothing else ever repeats or reinvents them.
 * Each is a 4-byte ASCII tag (like "MOCK") purely so a raw NVS/log dump is
 * human-legible; the numeric value, not the mnemonic, is what's actually
 * compared.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "module_config/module_config.h"
#include "sensor_channel/sensor_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reserved driver_id for the development-only mock sensor. A production
 * module_config.bin (dev override AUTO) must never select this -- module
 * main.c enforces that (see docs/module-config.md). */
#define SENSOR_DRIVER_ID_MOCK    0x4D4F434Bu /* ASCII "MOCK" */

/* Real driver_ids (section 4/5/10 of the follow-up spec). Each selects
 * exactly one compiled-in sensor_provider_ops_t via sensor_provider_lookup(). */
#define SENSOR_DRIVER_ID_BH1750  0x42483137u /* ASCII "BH17" -- ROHM BH1750 ambient light sensor */
#define SENSOR_DRIVER_ID_SHT45   0x53483435u /* ASCII "SH45" -- Sensirion SHT45 temp/humidity sensor */
#define SENSOR_DRIVER_ID_SGP40   0x53473430u /* ASCII "SG40" -- Sensirion SGP40 VOC sensor */
#define SENSOR_DRIVER_ID_SGP41   0x53473431u /* ASCII "SG41" -- Sensirion SGP41 VOC+NOx sensor */
#define SENSOR_DRIVER_ID_PMS5003 0x504D3530u /* ASCII "PM50" -- terminal-only Plantower PMS5003 UART sensor */

typedef enum {
    SENSOR_STATUS_OK               = 0,
    SENSOR_STATUS_NOT_INITIALIZED  = 1,
    SENSOR_STATUS_TIMEOUT          = 2,
    SENSOR_STATUS_BUS_ERROR        = 3,
    SENSOR_STATUS_DRIVER_ERROR     = 4,
    SENSOR_STATUS_CRC_ERROR        = 5, /* Sensirion word CRC mismatch (SHT4x/SGP4x) */
    SENSOR_STATUS_RANGE_ERROR      = 6, /* raw reading decoded but outside the sensor's valid range */
    SENSOR_STATUS_CONDITIONING     = 7, /* SGP4x warm-up/conditioning: not a failure, but not a trustworthy index yet */
} sensor_status_t;

/*
 * One driver read. sensor_type_bitmap/response_time_ms are unchanged from
 * before this refactor -- pogo_sensor_info_ack_t (POGO SENSOR_HELLO wire
 * reply, see pogo_protocol.h) only ever serializes those two fields plus
 * fw_version/status, so the existing POGO provisioning wire format needs no
 * change (section 6: "기존 POGO provisioning protocol은 가능한 호환성
 * 유지"). channel_count/channels are the actual per-channel values, added
 * for the new periodic Thread-plane reporting path (sensor_service ->
 * sensor_channel_pack() -> sensor_transport, NOT POGO) -- a driver written
 * before this field existed would simply leave channel_count at 0, which
 * SENSOR_HELLO handling never looked at anyway.
 */
typedef struct {
    uint16_t sensor_type_bitmap;
    uint32_t response_time_ms; /* measured elapsed time for this one reading */
    uint8_t  channel_count;
    sensor_channel_reading_t channels[SENSOR_CHANNEL_MAX_COUNT];
} sensor_measurement_t;

/* Opaque per-driver instance; each driver defines its own concrete struct
 * and only ever hands out a pointer to it. */
typedef struct sensor_provider_ctx_s sensor_provider_ctx_t;

typedef struct {
    const char *name;

    /* Allocates/initializes the driver against the fixed board config
     * (bus wiring, calibration, measurement_timeout_ms). Must not probe any
     * I2C/SPI address other than the one module_config specifies (section 4:
     * "지정된 주소만 사용하며 다른 주소를 자동 탐색하지 않음"). */
    esp_err_t (*init)(const module_config_t *cfg, sensor_provider_ctx_t **out_ctx);

    /* Fixed for a given paired sensor; does not change across role changes. */
    uint16_t (*get_sensor_bitmap)(sensor_provider_ctx_t *ctx);

    /*
     * The driver's own minimum polling period (ms), e.g. SGP4x's Gas Index
     * Algorithm requires exactly 1 Hz sampling (section 10). Sensors with no
     * such constraint return 0. sensor_service combines this with the
     * dynamic response-time-derived formula:
     *   effective_polling_period_ms = max(dynamic_formula_result, driver_minimum_period_ms)
     * and this same value is used both for the transmitted POLL_CONFIG and
     * the actual periodic measurement task (never two different periods).
     */
    uint32_t (*get_min_polling_period_ms)(sensor_provider_ctx_t *ctx);

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

/* Returns NULL if driver_id names no driver compiled into this firmware.
 * Callers must treat NULL as CONFIG_ERROR: do not probe other driver_ids. */
const sensor_provider_ops_t *sensor_provider_lookup(uint32_t driver_id);

/*
 * Registers an externally-compiled driver under driver_id -- BH1750/SHT45/
 * SGP40/SGP41/PMS5003 each live in their own component (bh1750, sht45, ...)
 * rather than inside sensor_provider itself (the way the mock driver does),
 * specifically so a real driver component can depend on sensor_provider's
 * types (module_config_t, sensor_provider_ops_t) without sensor_provider
 * needing to depend back on every driver -- ESP-IDF component REQUIRES must
 * not cycle. module/main/app_main.c calls this once for each compiled-in
 * real driver, before module_config_load()/sensor_provider_lookup() are
 * used. Returns ESP_ERR_NO_MEM if the (fixed-size) registry is already full,
 * ESP_ERR_INVALID_ARG for a NULL ops.
 */
esp_err_t sensor_provider_register(uint32_t driver_id, const sensor_provider_ops_t *ops);

#ifdef __cplusplus
}
#endif
