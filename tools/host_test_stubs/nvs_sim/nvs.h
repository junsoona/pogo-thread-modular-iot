/*
 * Host-only, FUNCTIONAL stand-in for ESP-IDF's nvs.h -- unlike
 * tools/host_test_stubs/nvs.h (a write-only, always-succeeds stub used by
 * components/child_service's host tests), this one actually stores and
 * returns values, backed by the in-memory fake in nvs_sim.c. It exists so a
 * host test can link the REAL components/power_base_thread_config/
 * power_base_thread_config.c unmodified and exercise its actual read/
 * compat/fail-closed policy against constructed NVS scenarios (see
 * tools/test_power_base_thread_config.c) -- production loader code under
 * test, not a re-implementation of its decision logic.
 *
 * Put THIS directory FIRST on the include path (ahead of
 * tools/host_test_stubs itself) so it shadows both this file and the
 * plain nvs.h stub. See nvs_sim.h for the test-control API used to
 * populate the fake store before calling the real loader.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

/* Real ESP-IDF values (components/nvs_flash/include/nvs.h) -- kept
 * numerically identical even though host tests never compare against the
 * real SDK directly, so a log/error code inspected by a human matches. */
#define ESP_ERR_NVS_BASE            0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND       (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_LENGTH  (ESP_ERR_NVS_BASE + 0x0C)

esp_err_t nvs_open_from_partition(const char *partition_name, const char *namespace_name, nvs_open_mode_t open_mode,
                                   nvs_handle_t *out_handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
void nvs_close(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
