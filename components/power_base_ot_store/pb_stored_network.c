#include "power_base_ot_store/pb_stored_network.h"

#include <stdint.h>

#include "nvs.h"
#include "power_base_ot_store/pb_volatile_store.h"
#include "psa_crypto_its.h"

/* ESP-IDF keeps OpenThread's settings as blobs in this NVS namespace
 * (components/openthread/src/port/esp_openthread_settings.c, OT_NAMESPACE;
 * checked against ESP-IDF v6.1). */
#define OT_SETTINGS_NVS_NAMESPACE "openthread"

/* OpenThread's persistent PSA keys: OPENTHREAD_CONFIG_PSA_ITS_NVM_OFFSET plus
 * Crypto::Storage::KeyRefManager::Type -- the Network Key (1), the PSKc (2),
 * the Active and Pending Dataset's copies of both (3..6) and the ECDSA key
 * (7); these are what Instance::ErasePersistentInfo() destroys. The PSA key
 * store files a persistent key under its key id
 * (psa_its_identifier_of_slot()). Checked against ESP-IDF v6.1 by
 * tools/check_power_base_ot_store.py, which fails when a later ESP-IDF
 * changes the offset or adds a key. */
#define OT_PSA_KEY_REF_OFFSET 0x20000u
#define OT_PSA_KEY_REF_FIRST 1u
#define OT_PSA_KEY_REF_LAST 7u
/* KeyRefManager::kNetworkKey: the id KeyManager::Init() fills with a random
 * key at every start when it is empty, and leaves alone when it is not
 * (checked by the same tool). */
#define OT_PSA_KEY_REF_NETWORK_KEY 1u

/* In this boot both stores were read back empty at least once; the watch on
 * the Network Key's id runs from the last such read-back. RAM only. */
static bool s_read_back_empty;

/* The storage functions themselves, not the switch in front of them (see
 * pb_volatile_store.c for where these names come from). */
psa_status_t __real_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info);
psa_status_t __real_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length);
psa_status_t __real_psa_its_remove(psa_storage_uid_t uid);

bool pb_stored_network_settings_empty(const char *nvs_partition)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(nvs_partition, OT_SETTINGS_NVS_NAMESPACE, NVS_TYPE_ANY, &it);
    if (err == ESP_OK) {
        nvs_release_iterator(it);
        return false;
    }
    /* Anything but "no entry found" is a store that could not be read: not
     * proof of anything. */
    return err == ESP_ERR_NVS_NOT_FOUND;
}

bool pb_stored_network_keys_gone(void)
{
    bool gone = true;
    for (uint32_t type = OT_PSA_KEY_REF_FIRST; type <= OT_PSA_KEY_REF_LAST; type++) {
        struct psa_storage_info_t info;
        if (__real_psa_its_get_info((psa_storage_uid_t)(OT_PSA_KEY_REF_OFFSET + type), &info) !=
            PSA_ERROR_DOES_NOT_EXIST) {
            gone = false;
        }
    }
    return gone;
}

bool pb_stored_network_is_erased(const char *nvs_partition)
{
    /* Both are read even when the first already says no: no early way out
     * that a test of only one store would miss. */
    bool settings = pb_stored_network_settings_empty(nvs_partition);
    bool keys = pb_stored_network_keys_gone();
    if (settings && keys) {
        /* From here on, whatever appears under the Network Key's id is seen
         * being stored. */
        s_read_back_empty = true;
        pb_volatile_store_its_watch_start(OT_PSA_KEY_REF_OFFSET + OT_PSA_KEY_REF_NETWORK_KEY);
    }
    return settings && keys;
}

/* The entry under the Network Key's id is the one this boot saw being stored
 * after it had read both stores back empty: one write, never removed -- and
 * what is in the flash now is that write, byte for byte. The entry is read
 * for that comparison only (it is the key file of the key OpenThread made
 * for itself, or it does not match); it is wiped here and goes nowhere. */
static bool network_key_entry_is_this_boots(void)
{
    const psa_storage_uid_t uid = OT_PSA_KEY_REF_OFFSET + OT_PSA_KEY_REF_NETWORK_KEY;
    pb_volatile_store_its_watch_t watch;
    pb_volatile_store_its_watch_get(&watch);
    if (!s_read_back_empty || !watch.watching || watch.stored != 1 || watch.removed != 0) {
        return false;
    }
    uint8_t entry[PB_VOLATILE_STORE_WATCH_BYTES];
    struct psa_storage_info_t info;
    size_t got = 0;
    bool same = false;
    if (__real_psa_its_get_info(uid, &info) == PSA_SUCCESS && info.size <= sizeof(entry) &&
        __real_psa_its_get(uid, 0, info.size, entry, &got) == PSA_SUCCESS && got == info.size) {
        same = pb_volatile_store_its_watch_matches(entry, got);
    }
    volatile uint8_t *p = entry;
    for (size_t i = 0; i < sizeof(entry); i++) {
        p[i] = 0;
    }
    return same;
}

bool pb_stored_network_holds_no_network(const char *nvs_partition)
{
    bool settings = pb_stored_network_settings_empty(nvs_partition);
    bool keys = true;
    for (uint32_t type = OT_PSA_KEY_REF_FIRST; type <= OT_PSA_KEY_REF_LAST; type++) {
        struct psa_storage_info_t info;
        psa_status_t status = __real_psa_its_get_info((psa_storage_uid_t)(OT_PSA_KEY_REF_OFFSET + type), &info);
        bool acceptable = status == PSA_ERROR_DOES_NOT_EXIST;
        if (type == OT_PSA_KEY_REF_NETWORK_KEY && status == PSA_SUCCESS) {
            /* Something is stored where OpenThread keeps a key of its own --
             * and where a network's key lies as well. Only its origin says
             * which (see the header). */
            acceptable = network_key_entry_is_this_boots();
        }
        if (!acceptable) {
            keys = false;
        }
    }
    return settings && keys;
}

#if defined(PB_VOLATILE_STORE_TEST_HOOKS)
/* Host tests only, never compiled into a firmware: what a chip reset does to
 * this file's RAM. */
void pb_stored_network_test_reset(void);
void pb_stored_network_test_reset(void)
{
    s_read_back_empty = false;
}
#endif

esp_err_t pb_stored_network_settings_erase(const char *nvs_partition)
{
    /* Nothing there: nothing is opened for writing, and no namespace is
     * created for the sake of emptying it. */
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(nvs_partition, OT_SETTINGS_NVS_NAMESPACE, NVS_TYPE_ANY, &it);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_release_iterator(it);

    /* A handle of our own on the namespace: the erase does not depend on the
     * state of OpenThread's, and does not go through otPlatSettingsWipe(),
     * which the switch answers from RAM. */
    nvs_handle_t handle;
    err = nvs_open_from_partition(nvs_partition, OT_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t pb_stored_network_keys_erase(void)
{
    esp_err_t err = ESP_OK;
    for (uint32_t type = OT_PSA_KEY_REF_FIRST; type <= OT_PSA_KEY_REF_LAST; type++) {
        psa_status_t status = __real_psa_its_remove((psa_storage_uid_t)(OT_PSA_KEY_REF_OFFSET + type));
        if (status != PSA_SUCCESS && status != PSA_ERROR_DOES_NOT_EXIST) {
            err = ESP_FAIL; /* the others are still removed */
        }
    }
    return err;
}

esp_err_t pb_stored_network_erase(const char *nvs_partition)
{
    esp_err_t settings = pb_stored_network_settings_erase(nvs_partition);
    esp_err_t keys = pb_stored_network_keys_erase();
    return settings != ESP_OK ? settings : keys;
}
