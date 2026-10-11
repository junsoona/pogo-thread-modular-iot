/*
 * The board's two network stores for the host harness: the REAL
 * components/power_base_ot_store (pb_stored_network.c, pb_volatile_store.c)
 * over a small fake key storage in this file and the NVS simulator.
 *
 * What is real: the store check the firmware's BLE receipt source asks
 * (pb_stored_network_holds_no_network()), the erase and the read-back
 * pb_mgmt_released_prestart() is given, and the watch on the Network Key's id.
 * What is modelled: the key storage itself (a table), and OpenThread's start
 * (KeyManager::Init(): a key under the Network Key's id is left alone; when
 * there is none, one is imported -- through the same wrapped names the linked
 * firmware's OpenThread goes through).
 *
 * This header has no PSA or OpenThread type in it on purpose: harness_main.c
 * is compiled with ESP-IDF's real headers, store_glue.c with the same
 * stand-in headers as tools/test_power_base_ot_store.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What is put into the stores themselves, with nothing looking. */
#define HSTORE_PLANT_OLD_NETWORK_KEY 1u /* only an entry under the Network Key's id: a network's key an erase left */
#define HSTORE_PLANT_NETWORK 2u         /* a stored network: a settings entry, the Network Key, the Dataset's copy */
#define HSTORE_PLANT_EARLIER_BOOT_KEY 3u /* only the key OpenThread made for itself in an earlier boot */

#define HSTORE_FAULT_KEY_NOT_REMOVABLE 0x01u /* a remove of the Network Key fails and removes nothing */
#define HSTORE_FAULT_UNREADABLE 0x02u        /* no read of the key storage succeeds */

/* Everything gone: both stores and the component's RAM. */
void hstore_wipe(void);
void hstore_plant(uint8_t what);
void hstore_set_faults(uint8_t faults);

/* A reset of the chip, as far as the store component goes: its RAM. */
void hstore_restart_ram(void);

/* pb_mgmt_prestart_ops_t, as power_base/main/enrollment_mgmt.c gives them. */
int hstore_erase(void *ctx);
bool hstore_erased(void *ctx);

/* OpenThread starts. True when it stored a key of its own. */
bool hstore_openthread_starts(void);

/* pb_mgmt_store_check_fn, as power_base/main/enrollment_ble.c gives it. */
bool hstore_no_network(void *ctx);

/* For the driver's assertions: [0] an entry is under the Network Key's id,
 * [1] its first byte ('N' a network's, 'b' OpenThread's own), [2] the settings
 * namespace holds an entry, [3] writes of that id seen since the last
 * read-back that found both stores empty, [4] removes seen. */
void hstore_state(uint8_t out[5]);
