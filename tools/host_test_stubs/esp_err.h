/*
 * Minimal host-only stand-in for ESP-IDF's esp_err.h, used ONLY to
 * host-compile components whose logic has zero real hardware/FreeRTOS
 * dependency but which still spell their return type `esp_err_t` for
 * consistency with the rest of the firmware (e.g. components/node_calc).
 *
 * Put this directory FIRST on the include path (-I tools/host_test_stubs)
 * so it shadows the real esp_err.h from the ESP-IDF SDK. Values match
 * ESP-IDF's real esp_err.h exactly, so a test's expectations (ESP_OK == 0,
 * etc.) still hold if this ever needs to be checked against the real thing.
 */
#pragma once

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_SIZE  0x106
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_TIMEOUT       0x107
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x110
