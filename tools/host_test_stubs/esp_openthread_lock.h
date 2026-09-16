/*
 * Host-only stand-in for ESP-IDF's esp_openthread_lock.h -- only the two
 * functions components/sensor_transport/sensor_transport.c calls
 * (esp_openthread_lock_acquire()/esp_openthread_lock_release()). Real bodies
 * in tools/host_test_stubs/host_ot_coap_sim.c always succeed immediately: a
 * host test process has no separate OpenThread task to lock against, so
 * there is nothing real to serialize (same reasoning as
 * tools/host_test_stubs/freertos/FreeRTOS.h's taskENTER_CRITICAL() no-op).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool esp_openthread_lock_acquire(uint32_t timeout_ticks);
void esp_openthread_lock_release(void);

#ifdef __cplusplus
}
#endif
