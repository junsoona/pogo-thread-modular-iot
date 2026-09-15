/*
 * Host-only stand-in for OpenThread's openthread/thread.h -- only the two
 * types components/thread_manager/include/thread_manager/thread_manager.h
 * (and, through it, components/child_service/child_service.c and
 * components/parent_service/parent_service.c) actually reference:
 * otDeviceRole and otIp6Address. Values match the real OpenThread enum
 * exactly (openthread/include/openthread/thread.h) so any log/log-string
 * mapping stays meaningful.
 *
 * This stands in for the OpenThread/Thread-radio hardware+stack dependency
 * itself (same category as driver/gpio.h, driver/uart.h, nvs.h) -- real join
 * behavior is faked by tools/host_test_stubs/host_thread_manager_sim.c, a
 * separate, explicit stand-in for components/thread_manager/thread_manager.c
 * (which cannot be host-compiled at all: it wraps real OpenThread APIs).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OT_DEVICE_ROLE_DISABLED = 0,
    OT_DEVICE_ROLE_DETACHED = 1,
    OT_DEVICE_ROLE_CHILD = 2,
    OT_DEVICE_ROLE_ROUTER = 3,
    OT_DEVICE_ROLE_LEADER = 4,
} otDeviceRole;

typedef struct {
    union {
        uint8_t m8[16];
        uint16_t m16[8];
        uint32_t m32[4];
    } mFields;
} otIp6Address;

#ifdef __cplusplus
}
#endif
