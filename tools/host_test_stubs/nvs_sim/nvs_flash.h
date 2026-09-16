/* Host-only stand-in for ESP-IDF's nvs_flash.h -- only the one function
 * components/power_base_thread_config/power_base_thread_config.c calls.
 * See nvs.h in this same directory for the "why a second, functional NVS
 * stub" explanation. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init_partition(const char *partition_name);

#ifdef __cplusplus
}
#endif
