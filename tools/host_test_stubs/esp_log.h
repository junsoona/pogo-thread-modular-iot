/*
 * Minimal host-only stand-in for ESP-IDF's esp_log.h -- ESP_LOGE/W/I/D/V and
 * esp_err_to_name(), used by every real production .c file a host test in
 * this directory links unmodified (components/pogo_port, components/
 * child_service, components/parent_service, components/sensor_service,
 * components/pms5003, ...). See tools/host_test_stubs/esp_err.h's own
 * comment for the "put this directory first on the include path" convention.
 *
 * Host tests here run several real FreeRTOS-task-turned-pthread "tasks"
 * concurrently (see freertos/task.h) that may log at the same time -- a
 * process-wide mutex keeps one line from interleaving with another, purely
 * for readable test output; it has no bearing on the logic under test.
 */
#pragma once

#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include "esp_err.h"

static pthread_mutex_t g_host_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline void host_esp_log(const char *level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_host_log_mutex);
    fprintf(stderr, "%s (%s) ", level, tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&g_host_log_mutex);
    va_end(ap);
}

#define ESP_LOGE(tag, fmt, ...) host_esp_log("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) host_esp_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) host_esp_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) host_esp_log("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) host_esp_log("V", tag, fmt, ##__VA_ARGS__)

/* Added for components/power_base_transport's host test (this follow-up:
 * northbound bridge transport) -- power_base_transport.c has used
 * ESP_LOG_LEVEL_LOCAL()/esp_log_level_t for its own ATTACHED-vs-HEARTBEAT
 * log-level choice since before this stub existed, but had no host test of
 * its own to require it here yet. Levels match ESP-IDF's real esp_log.h
 * ordering (higher = more verbose). */
typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

#define ESP_LOG_LEVEL_LOCAL(level, tag, fmt, ...)                                                                    \
    do {                                                                                                             \
        switch (level) {                                                                                             \
        case ESP_LOG_ERROR: ESP_LOGE(tag, fmt, ##__VA_ARGS__); break;                                                \
        case ESP_LOG_WARN: ESP_LOGW(tag, fmt, ##__VA_ARGS__); break;                                                 \
        case ESP_LOG_DEBUG: ESP_LOGD(tag, fmt, ##__VA_ARGS__); break;                                                \
        case ESP_LOG_VERBOSE: ESP_LOGV(tag, fmt, ##__VA_ARGS__); break;                                              \
        case ESP_LOG_INFO:                                                                                           \
        default: ESP_LOGI(tag, fmt, ##__VA_ARGS__); break;                                                           \
        }                                                                                                             \
    } while (0)

static inline const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default: return "ESP_ERR_UNKNOWN";
    }
}
