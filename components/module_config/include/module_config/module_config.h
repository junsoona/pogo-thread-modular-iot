/*
 * Fixed board/sensor configuration -- the permanent ESP32-H2<->sensor pairing
 * (see docs/module-config.md for the full field list and wire layout).
 *
 * This is loaded once at boot from a dedicated NVS partition ("cfg",
 * see partitions.csv) that is DISTINCT from the "nvs" partition used for
 * dynamic topology/Thread state (role_manager/parent_service/child_service,
 * OpenThread's own dataset copy). Application code must treat the returned
 * module_config_t as read-only for the rest of the boot: driver_id, sensor
 * wiring and calibration never change while a role transition happens.
 *
 * The "cfg" partition also carries a second, separate NVS namespace
 * (MODULE_CONFIG_DEV_NAMESPACE) used only by the two-UART development boards
 * described in docs/pogo-interface.md ("development compat mode"). A real
 * production module_config.bin image never writes that namespace at all,
 * so module_config_load_dev_mode() safely returns MODULE_DEV_MODE_AUTO on
 * real hardware.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODULE_CONFIG_NVS_PARTITION  "cfg"
#define MODULE_CONFIG_NAMESPACE      "module_cfg"
#define MODULE_CONFIG_DEV_NAMESPACE  "dev_cfg"
#define MODULE_CONFIG_SCHEMA_VERSION 1u

typedef enum {
    MODULE_BUS_I2C = 0,
    MODULE_BUS_SPI = 1,
} module_bus_type_t;

/*
 * One logical POGO port's fixed wiring. UART/GPIO numbers are "fixed" in the
 * sense that they come from this config partition, not compiled-in
 * constants -- the actual values are still unknown until the final PCB is
 * routed (see docs/pogo-interface.md). A value of -1 means "this GPIO does
 * not exist for this port on this board" (used by the dev-board compat
 * images, which have no DETECT/BASE_ID hardware at all).
 */
typedef struct {
    int32_t  uart_num;
    int32_t  tx_gpio;
    int32_t  rx_gpio;
    int32_t  detect_in_gpio;
    int32_t  detect_out_gpio;
    int32_t  base_id_gpio;         /* only ever meaningful on the UPSTREAM port */
    bool     detect_out_active_low;
    uint32_t debounce_ms;
} module_port_config_t;

typedef struct {
    uint32_t schema_version;
    uint32_t config_revision;
    uint32_t driver_id;              /* selects one sensor_provider driver; see sensor_provider.h */
    uint16_t sensor_type_bitmap;
    module_bus_type_t bus_type;
    uint8_t  bus_number;
    int32_t  i2c_sda_gpio;
    int32_t  i2c_scl_gpio;
    uint16_t i2c_address;
    int32_t  spi_mosi_gpio;
    int32_t  spi_miso_gpio;
    int32_t  spi_sclk_gpio;
    int32_t  spi_cs_gpio;
    uint32_t spi_clock_hz;
    int32_t  calibration[4];         /* driver-interpreted; unused slots are 0 */
    uint32_t measurement_timeout_ms;
    module_port_config_t upstream_port;
    module_port_config_t downstream_port;
} module_config_t;

typedef enum {
    MODULE_DEV_MODE_AUTO     = 0, /* final hardware: role_manager derives ROOT/INTERMEDIATE/LEAF from BASE_ID + PEER_HELLO */
    MODULE_DEV_MODE_DEV_ROOT = 1, /* dev board: single UART mapped to DOWNSTREAM only, role forced to ROOT */
    MODULE_DEV_MODE_DEV_LEAF = 2, /* dev board: single UART mapped to UPSTREAM only, role forced to LEAF */
} module_dev_mode_t;

/*
 * Loads and validates the fixed config from the "cfg" NVS partition
 * (namespace MODULE_CONFIG_NAMESPACE): checks the blob is present, its
 * length matches this firmware's wire layout, its CRC16 matches the stored
 * value, and its schema_version matches MODULE_CONFIG_SCHEMA_VERSION.
 *
 * On any failure this logs a single clear "CONFIG_ERROR: ..." line, zeroes
 * *out, and returns a non-ESP_OK code. Callers MUST NOT start the sensor
 * provider, POGO pairing, or any Thread network in that case -- there is no
 * driver auto-detection and no "guess a previous dataset" fallback (see
 * section 9 of the implementation spec).
 */
esp_err_t module_config_load(module_config_t *out);

/*
 * Reads the development-board override (namespace MODULE_CONFIG_DEV_NAMESPACE).
 * Defaults to MODULE_DEV_MODE_AUTO if the namespace/key is absent, which is
 * always the case for a real module_config.bin image -- only the dev_root/
 * dev_leaf config images (see configs/, tools/) set this.
 */
module_dev_mode_t module_config_load_dev_mode(void);

const char *module_dev_mode_to_str(module_dev_mode_t mode);

#ifdef __cplusplus
}
#endif
