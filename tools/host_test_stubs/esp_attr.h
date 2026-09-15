/*
 * Minimal host-only stand-in for ESP-IDF's esp_attr.h -- just IRAM_ATTR,
 * which components/pogo_port/pogo_port.c applies to its GPIO ISR handler.
 * On host there is no separate IRAM/DRAM placement concept, so this is
 * simply empty -- see tools/host_test_stubs/esp_err.h's own comment for the
 * "put this directory first on the include path" convention this follows.
 */
#pragma once

#define IRAM_ATTR
#define DRAM_ATTR
