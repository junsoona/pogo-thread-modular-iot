/*
 * Fixed power-base board configuration -- the power-base project's analogue
 * of components/module_config (see docs/module-config.md), NOT a reuse of
 * module_config_t itself: a power base has no sensor pairing at all (no
 * driver_id, no I2C/SPI bus, no calibration), and inventing dummy values for
 * those fields to force-fit module_config_t would be exactly the kind of
 * fake config this refactor's ground rules forbid. Instead this is its own
 * small, independent NVS-backed config, reusing what genuinely IS shared:
 * components/module_config/gpio_profile.h's ESP32-H2 GPIO usability policy
 * (same chip, same reserved-pin rules) and components/pogo_proto/pogo_crc16.h's
 * CRC16 for the same blob-integrity pattern module_config already uses.
 *
 * Loaded once at boot from the power_base project's own "cfg" NVS partition
 * (see power_base/partitions.csv), namespace POWER_BASE_CONFIG_NAMESPACE --
 * a physically separate flash image from `module`'s own "cfg" partition
 * (different board, different binary), so there is no naming collision to
 * worry about despite both using the partition name "cfg".
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POWER_BASE_CONFIG_NVS_PARTITION  "cfg"
#define POWER_BASE_CONFIG_NAMESPACE      "power_base_cfg"
#define POWER_BASE_CONFIG_SCHEMA_VERSION 1u

/*
 * The power base's ONE physical POGO port -- electrically identical to a
 * sensor module's DOWNSTREAM port (see docs/power-base.md's wiring table):
 * no BASE_ID field at all (BASE_ID is read on a MODULE's UPSTREAM side only;
 * the power base does not read any BASE_ID itself -- its own identity as
 * POWER_BASE is asserted purely via PEER_HELLO's own_peer_type, see
 * power_base/main/app_main.c). detect_in_gpio/detect_out_gpio are always
 * mandatory here (unlike module_config's AUTO-only requirement) -- a power
 * base has no dev-compat/forced-role concept at all.
 */
typedef struct {
    int32_t  uart_num;
    int32_t  tx_gpio;
    int32_t  rx_gpio;
    int32_t  detect_in_gpio;
    int32_t  detect_out_gpio;
    bool     detect_out_active_low;
    uint32_t debounce_ms;
} power_base_port_config_t;

typedef struct {
    uint32_t schema_version;
    uint32_t config_revision;
    power_base_port_config_t downstream_port;
    /* 0 = let parent_service use its own CONFIG_POGO_ACK_TIMEOUT_MS default;
     * otherwise this many ms per provisioning-step retry attempt (see
     * parent_service_context_t.ack_timeout_ms). */
    uint32_t ack_timeout_ms;
    /* Passed to topology_registry_sweep_stale()/power_base_transport_sweep_stale():
     * a currently-online node with no accepted report in this long is marked
     * offline. */
    uint32_t topology_stale_timeout_ms;
    /* Informational: how often a well-behaved module is expected to send a
     * HEARTBEAT report (see components/topology_reporter's own, separately
     * configured, CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS -- this
     * field documents/validates the SERVER-side expectation and should be
     * set comfortably larger than that Kconfig default so normal jitter is
     * never mistaken for staleness). */
    uint32_t heartbeat_timeout_ms;
    /* -1 = no power-switching GPIO on this hardware; the 5V rail is assumed
     * always on (see docs/power-base.md -- this firmware never claims to be
     * able to cut real 5V power when this is -1). */
    int32_t  power_enable_gpio;
    bool     power_enable_active_high;
    uint32_t power_startup_delay_ms;
} power_base_config_t;

/*
 * Loads and validates the fixed config from the "cfg" NVS partition. On any
 * failure this logs one clear "CONFIG_ERROR: ..." line, zeroes *out, and
 * returns a non-ESP_OK code -- the caller (power_base/main/app_main.c) MUST
 * NOT start Thread, the POGO port, or downstream provisioning in that case,
 * same policy as module_config_load().
 */
esp_err_t power_base_config_load(power_base_config_t *out);

#ifdef __cplusplus
}
#endif
