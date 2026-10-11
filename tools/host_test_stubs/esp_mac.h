/*
 * Host-only stand-in for ESP-IDF's esp_mac.h -- only esp_efuse_mac_get_default(),
 * used by components/child_service/child_service.c (handle_identify_req(),
 * compute_permanent_uuid()) and components/sensor_service/sensor_service.c
 * (sensor_service_get_module_uuid()'s ROOT/not-yet-provisioned fallback).
 *
 * Returns a fixed, non-zero 8-byte EUI-64 -- child_service.c explicitly
 * refuses an all-zero read (see compute_permanent_uuid()'s own comment), so
 * this must never be all-zero. A single fixed value is correct for a host
 * test simulating one module identity: child_service.c's own
 * compute_permanent_uuid() and sensor_service.c's module-uuid fallback both
 * read the SAME physical efuse on real hardware, so both returning the same
 * fixed value here (each translation union gets this `static` function
 * inline, but all return the identical literal) matches that real-hardware
 * invariant without needing any cross-translation-unit state.
 */
#pragma once

#include <string.h>
#ifdef HOST_ESP_MAC_LAST_BYTE_ENV
#include <stdlib.h>
#endif
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    static const uint8_t fixed_mac[8] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01};
    memcpy(mac, fixed_mac, 8);
#ifdef HOST_ESP_MAC_LAST_BYTE_ENV
    /* Opt-in (tools/test_module_upstream_regeneration.c's chain mode): one
     * process per board, each given its own last EUI-64 byte in the
     * environment variable this macro names. */
    const char *last = getenv(HOST_ESP_MAC_LAST_BYTE_ENV);
    if (last != NULL) {
        mac[7] = (uint8_t)strtoul(last, NULL, 0);
    }
#endif
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
