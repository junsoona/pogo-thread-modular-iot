/*
 * Minimal host-only stand-in for ESP-IDF's esp_timer.h -- only
 * esp_timer_get_time(), a free-running microsecond counter, used by
 * components/pms5003/pms5003.c (warm-up/response-time measurement),
 * components/child_service/child_service.c and
 * components/sensor_service/sensor_service.c. Backed by CLOCK_MONOTONIC,
 * same real-time basis as freertos/task.h's xTaskGetTickCount() (just a
 * finer unit), so elapsed-time math against either clock stays consistent
 * with real wall-clock time in a host test.
 */
#pragma once

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_nsec / 1000;
}

#ifdef __cplusplus
}
#endif
