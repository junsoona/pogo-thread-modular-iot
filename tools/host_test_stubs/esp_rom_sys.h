/*
 * Host-only stand-in for ESP-IDF's esp_rom_sys.h -- only
 * esp_rom_delay_us(), a busy-wait microsecond delay, used by
 * components/sht45/sht45.c for its hardware-mandated post-command
 * recovery waits (see that file's own comment on SHT45_RESET_US/
 * SHT45_SERIAL_US: these are real minimum recovery times, not scheduling
 * hints, so pdMS_TO_TICKS()/vTaskDelay() -- which round to whole ticks --
 * are deliberately not used for them). Backed by nanosleep(), same
 * CLOCK_MONOTONIC basis as this directory's other timing stubs
 * (esp_timer.h, freertos/task.h).
 */
#pragma once

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void esp_rom_delay_us(uint32_t us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
}

#ifdef __cplusplus
}
#endif
