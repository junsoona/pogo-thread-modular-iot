/*
 * Test-control API for the fake in-memory NVS store backing nvs.h/
 * nvs_flash.h in this directory (see nvs_sim.c). Used ONLY by the test file
 * itself (tools/test_power_base_thread_config.c), never by the real
 * component under test -- the real component only ever sees nvs.h/
 * nvs_flash.h's ordinary ESP-IDF-shaped API.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wipes the entire fake store and clears any pending forced-error
 * injection. Call at the start of every scenario so scenarios never leak
 * state into each other. */
void nvs_sim_reset(void);

/* Creates an empty namespace (no keys at all) if it doesn't already exist --
 * models "the 'pb_thread' namespace exists but has no 'mode' key yet"
 * without needing any put_* call. No-op if the namespace already exists. */
void nvs_sim_create_namespace(const char *partition, const char *ns);

/* Creates the namespace if needed, then sets/overwrites a u8 key in it. */
void nvs_sim_put_u8(const char *partition, const char *ns, const char *key, uint8_t value);

/* Creates the namespace if needed, then sets/overwrites a u16 key in it. */
void nvs_sim_put_u16(const char *partition, const char *ns, const char *key, uint16_t value);

/* Creates the namespace if needed, then sets/overwrites a blob key in it.
 * len == 0 is allowed (models an explicitly-empty blob). */
void nvs_sim_put_blob(const char *partition, const char *ns, const char *key, const void *data, size_t len);

/* Makes the NEXT nvs_flash_init_partition() call return `err` instead of
 * ESP_OK, then auto-clears (one-shot) -- models a genuine NVS/partition
 * read error, distinct from "namespace/key simply absent". */
void nvs_sim_force_next_init_error(esp_err_t err);

#ifdef __cplusplus
}
#endif
