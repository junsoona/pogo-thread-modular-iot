/*
 * "Is anything of a Thread network left in this board's flash?" -- read from
 * the two stores themselves (docs/power-base-management.md, section 4).
 *
 * OpenThread keeps its network in two places on ESP-IDF v6.1:
 *
 *   - its settings, the NVS namespace "openthread": the Active Dataset
 *     WITHOUT its keys, the network info, the parent, ...;
 *   - PSA Internal Trusted Storage, the NVS namespace "psa_its": OpenThread
 *     is built with key references there, so the Network Key and the PSKc are
 *     persistent PSA keys under OpenThread's own key ids.
 *
 * "Erased" needs both. Neither is asked through OpenThread or the PSA key
 * store: otDatasetIsCommissioned() answers from a flag in RAM,
 * otPlatSettingsWipe() ignores the result of its nvs_erase_all(), the erase
 * destroys the keys with the result ignored, and the PSA key store answers
 * from its RAM cache first. The PSA entries are read with the storage
 * function itself (the one behind components/power_base_ot_store's switch, so
 * the answer is the flash's also while the switch is engaged).
 *
 * The erase below is the same in the other direction: it removes what is in
 * the two stores themselves, past the switch. Before the switch OpenThread's
 * own erase (otInstanceErasePersistentInfo()) does that. After it that call
 * only empties the switch's tables in RAM -- so if the flash is found not
 * empty then, this is the one erase that reaches it.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True only when the settings namespace in `nvs_partition` holds not one
 * entry. False also when it could not be read. */
bool pb_stored_network_settings_empty(const char *nvs_partition);

/* True only when storage says "does not exist" for every one of OpenThread's
 * persistent keys. Any other answer -- present, damaged, unreadable -- is
 * false. */
bool pb_stored_network_keys_gone(void);

/* Both of the above: NOTHING is left. This is the proof of an erase.
 *
 * When it says yes, that is remembered for the rest of this boot, and from
 * that moment the Network Key's id is watched (pb_volatile_store.h): see the
 * next function for what that is for. Asking changes nothing in flash. */
bool pb_stored_network_is_erased(const char *nvs_partition);

/*
 * "Is a network stored?" -- for the moments when "nothing is left" cannot be
 * the question any more, because OpenThread has started since.
 *
 * At every start OpenThread imports a random key under the Network Key's id
 * when none is stored (Instance::AfterInit() -> KeyManager::Init() ->
 * StoreNetworkKey(key, overwrite = false)). When one IS stored it leaves it
 * alone -- so an entry under that id is, by itself, either the key of no
 * network that some start made, or a network's own key that an erase failed
 * to remove, and the store cannot tell which. Where it lies says nothing.
 *
 * What can tell is this boot's own history, and that is what is asked here.
 * True only when
 *   - the settings namespace holds not one entry;
 *   - none of the six other persistent keys exists (the PSKc, the Active and
 *     Pending Dataset's copies of the Network Key and the PSKc, the ECDSA
 *     key);
 *   - and the Network Key's id either does not exist, or holds an entry of
 *     KNOWN origin: in this boot both stores were read back empty
 *     (pb_stored_network_is_erased() said yes), since that read-back
 *     exactly one write of that id -- and no remove -- went through to the
 *     platform, and the entry in the flash is that write, byte for byte.
 *     That one write is the key the start made on an empty store: nothing
 *     else stores under that id without removing first (the PSA key store
 *     refuses to import over an existing key).
 * Any answer of a store that is not "exists" or "does not exist" is a store
 * that could not be read: false.
 *
 * So after a restart this is false until that boot has erased and read back:
 * a board that looks at a record saying "erased" does exactly that BEFORE it
 * starts OpenThread (pb_mgmt_released_prestart()), which removes whatever
 * lies under the Network Key's id -- an earlier start's key, or a network's
 * -- and lets this start make its own where it can be seen.
 */
bool pb_stored_network_holds_no_network(const char *nvs_partition);

/* Erases every entry of the settings namespace in `nvs_partition`, in the
 * flash, whatever the switch says. ESP_OK = nothing was there, or the erase
 * and its commit returned no error. Not proof: read it back. */
esp_err_t pb_stored_network_settings_erase(const char *nvs_partition);

/* Removes every one of OpenThread's persistent keys from storage itself.
 * ESP_OK = each was removed or was not there. Not proof: read it back. */
esp_err_t pb_stored_network_keys_erase(void);

/* Both of the above; both are tried even when the first fails. To be
 * followed by pb_stored_network_is_erased(). */
esp_err_t pb_stored_network_erase(const char *nvs_partition);

#ifdef __cplusplus
}
#endif
