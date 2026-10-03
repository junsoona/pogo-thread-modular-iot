/*
 * Host-only stand-in for ESP-IDF's nvs.h/nvs_flash.h -- only the small
 * subset components/child_service/child_service.c actually calls
 * (save_dynamic_record()/save_collector_dynamic_record(): nvs_open,
 * nvs_set_blob/u8/u16/u32, nvs_commit, nvs_close). This is a stand-in for
 * the flash/NVS hardware dependency itself (same category as driver/gpio.h,
 * driver/uart.h), NOT a mirror of child_service.c's own logic under test --
 * child_service.c's real NVS-result error handling (refuse to ACK/join on a
 * write failure) runs unmodified against this.
 *
 * Always succeeds and discards every value -- host tests here only care
 * that child_service.c's control flow reaches "durably saved" and proceeds
 * (see docs/state-machine.md: this module never reads any of it back on
 * boot anyway, so a host test asserting on the actual bytes written would
 * not be testing anything the real firmware itself relies on).
 */
#pragma once

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

static inline esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)namespace_name;
    (void)open_mode;
    *out_handle = 1;
    return ESP_OK;
}

static inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    return ESP_OK;
}

static inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

static inline esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

static inline esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

/* Added for components/topology_reporter's host test
 * (tools/test_module_downstream_fault_isolation.c links the REAL
 * topology_reporter.c, whose load_next_boot_generation() reads its boot
 * counter back) -- consistent with "discards every value" above: nothing is
 * ever stored, so a read always reports the key as absent (real ESP-IDF
 * value of ESP_ERR_NVS_NOT_FOUND). */
#define ESP_ERR_NVS_NOT_FOUND 0x1102

static inline esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return ESP_ERR_NVS_NOT_FOUND;
}

static inline esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

static inline void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

#ifdef __cplusplus
}
#endif
