/*
 * Test-control API for the fake in-memory NVS store backing nvs.h/
 * nvs_flash.h in this directory (see nvs_sim.c). Used ONLY by the test file
 * itself (tools/test_power_base_thread_config.c), never by the real
 * component under test -- the real component only ever sees nvs.h/
 * nvs_flash.h's ordinary ESP-IDF-shaped API.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
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

/* Makes the NEXT nvs_commit() call return `err` without persisting
 * anything, then auto-clears (one-shot) -- models a flash write failure. */
void nvs_sim_force_next_commit_error(esp_err_t err);

/* Simulated flash for multi-boot tests: loads the whole store from `path`
 * (an empty store if the file does not exist yet) and makes every later
 * successful nvs_commit() rewrite that file atomically. A test models a
 * reboot/power loss by running each boot in a fresh child process that
 * calls this with the same path -- everything that was only in RAM
 * (including values set but never committed) is gone, exactly like a real
 * power cycle. Returns false if the file exists but cannot be read. */
bool nvs_sim_set_backing_file(const char *path);

/*
 * Fault injection for one kind of call (added for
 * tools/test_power_base_reenroll.c; nothing changes for a test that does not
 * call it). The `nth` matching call from now on (1 = the next one) of `op`
 * -- on `key`, or on any key when `key` is NULL; COMMIT has no key -- goes
 * wrong, once:
 *   err != ESP_OK   the call returns `err` and changes nothing;
 *   err == ESP_OK   SET_BLOB / ERASE_KEY / ERASE_ALL only: the call returns
 *                   ESP_OK and changes nothing (a write or an erase that
 *                   claims success and did not happen -- what a read-back is
 *                   there to catch).
 * One pending fault per kind of call. nvs_sim_reset() clears them all.
 */
typedef enum {
    NVS_SIM_OP_SET_BLOB = 0,
    NVS_SIM_OP_GET_BLOB,
    NVS_SIM_OP_ERASE_KEY,
    NVS_SIM_OP_COMMIT,
    /* nvs_entry_find(); `key` is ignored. With err != ESP_OK the namespace
     * "could not be read". */
    NVS_SIM_OP_ENTRY_FIND,
    /* nvs_erase_all(); `key` is ignored. */
    NVS_SIM_OP_ERASE_ALL,
    NVS_SIM_OP_COUNT,
} nvs_sim_op_t;

void nvs_sim_inject(nvs_sim_op_t op, const char *key, int nth, esp_err_t err);

/* Power loss: the process ends (exit code NVS_SIM_POWER_CUT_EXIT) right
 * before the `nth` nvs_commit() from now on would have written the backing
 * file -- so that commit and everything after it never happened. 0 = off. */
#define NVS_SIM_POWER_CUT_EXIT 77
void nvs_sim_power_cut_at_commit(int nth);

/* How many nvs_commit() calls returned ESP_OK since the last nvs_sim_reset()
 * / nvs_sim_set_backing_file(). */
int nvs_sim_commit_count(void);

/* Reads a blob straight out of the store, for assertions (no fault applies).
 * Returns its length, or -1 when the namespace or key is absent. */
int nvs_sim_peek_blob(const char *partition, const char *ns, const char *key, void *out, size_t cap);

#ifdef __cplusplus
}
#endif
