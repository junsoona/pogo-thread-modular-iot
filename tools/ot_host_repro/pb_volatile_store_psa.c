/*
 * Minimal reproduction, with the REAL PSA key store, of what
 * components/power_base_ot_store does to PSA Internal Trusted Storage
 * (docs/power-base-management.md, section 4).
 *
 * On ESP-IDF v6.1 OpenThread is built with key references: the Thread Network
 * Key and the PSKc are PERSISTENT PSA keys (KeyManager::StoreNetworkKey(),
 * DatasetManager::MoveKeysToSecureStorage()), which the PSA key store writes
 * through psa_its_set() -- on the board into the NVS namespace "psa_its".
 * This program makes the same PSA calls OpenThread's crypto_platform_psa.cpp
 * makes for such a key, against the TF-PSA-Crypto core of that ESP-IDF
 * ($IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto), and looks at the
 * store itself:
 *
 *   control   importing a persistent key writes it to storage, and after a
 *             restart of the key store it is still there;
 *   switch    with the switch engaged the same import succeeds, the key can be
 *             used, exported, dropped from the key store's RAM cache and
 *             loaded again, destroyed -- and storage never sees it; after a
 *             restart it does not exist.
 *   erase     with the switch engaged and keys in storage itself (what a
 *             session that got past the switch would leave), the key store's
 *             own destroy -- OpenThread's erase -- reports success and leaves
 *             storage as it was; pb_stored_network_keys_erase() removes them
 *             from storage, and the read-back says so.
 *   origin    whose key is under the Network Key's id: OpenThread's start
 *             imports one of its own only when none is stored, and leaves a
 *             stored one alone -- so the entry is accepted as OpenThread's
 *             only when this boot read the stores back empty, then saw that
 *             one write, and the key file in storage is that write. The real
 *             key store makes exactly one storage write per import and
 *             refuses to import over an existing key.
 *
 * "Storage" here is TF-PSA-Crypto's own file backend (core/psa_its_file.c,
 * one file per entry in the current directory), standing where ESP-IDF's
 * esp_psa_its.c stands on the board; the component under test is the very
 * file the firmware links. NOT covered: esp_psa_its.c, NVS, the link-time
 * --wrap, and OpenThread itself (tools/ot_host_repro/
 * pb_release_session_nexus.cpp runs OpenThread, without key references).
 *
 * Built and run by tools/test_power_base_release_session_openthread.sh, in an
 * empty directory.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "openthread/platform/settings.h"
#include "power_base_ot_store/pb_stored_network.h"
#include "power_base_ot_store/pb_volatile_store.h"
#include "psa/crypto.h"
#include "psa_crypto_its.h"

void pb_volatile_store_test_reset(void);
void pb_stored_network_test_reset(void);

/* components/power_base_ot_store, as the firmware links it. */
otError __wrap_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
psa_status_t __wrap_psa_its_get_info(psa_storage_uid_t, struct psa_storage_info_t *);
psa_status_t __wrap_psa_its_get(psa_storage_uid_t, uint32_t, uint32_t, void *, size_t *);
psa_status_t __wrap_psa_its_set(psa_storage_uid_t, uint32_t, const void *, psa_storage_create_flags_t);
psa_status_t __wrap_psa_its_remove(psa_storage_uid_t);

static int g_failures;

static void check(int condition, const char *what)
{
    printf("%s %s\n", condition ? "ok:  " : "FAIL:", what);
    if (!condition) {
        g_failures++;
    }
}

/* ---- what the PSA key store calls: with --wrap the linker does this ---- */

psa_status_t psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info)
{
    return __wrap_psa_its_get_info(uid, p_info);
}

psa_status_t psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                         size_t *p_data_length)
{
    return __wrap_psa_its_get(uid, data_offset, data_length, p_data, p_data_length);
}

psa_status_t psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                         psa_storage_create_flags_t create_flags)
{
    return __wrap_psa_its_set(uid, data_length, p_data, create_flags);
}

psa_status_t psa_its_remove(psa_storage_uid_t uid)
{
    return __wrap_psa_its_remove(uid);
}

/* ---- OpenThread's settings are not part of this program ---- */

static unsigned g_real_settings_calls;

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    return __wrap_otPlatSettingsGet(aInstance, aKey, aIndex, aValue, aValueLength);
}

otError __real_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue,
                                 uint16_t *aValueLength);
otError __real_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError __real_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError __real_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex);
void __real_otPlatSettingsWipe(otInstance *aInstance);

otError __real_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue,
                                 uint16_t *aValueLength)
{
    (void)aInstance, (void)aKey, (void)aIndex, (void)aValue, (void)aValueLength;
    g_real_settings_calls++;
    return OT_ERROR_NOT_FOUND;
}

otError __real_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance, (void)aKey, (void)aValue, (void)aValueLength;
    g_real_settings_calls++;
    return OT_ERROR_NONE;
}

otError __real_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance, (void)aKey, (void)aValue, (void)aValueLength;
    g_real_settings_calls++;
    return OT_ERROR_NONE;
}

otError __real_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    (void)aInstance, (void)aKey, (void)aIndex;
    g_real_settings_calls++;
    return OT_ERROR_NOT_FOUND;
}

void __real_otPlatSettingsWipe(otInstance *aInstance)
{
    (void)aInstance;
    g_real_settings_calls++;
}

/* ---- the storage itself: the files psa_its_file.c keeps ---- */

static int stored_entries(void)
{
    int count = 0;
    DIR *dir = opendir(".");
    if (dir == NULL) {
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 8 && strcmp(&e->d_name[n - 8], ".psa_its") == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* ---- the PSA calls OpenThread makes (src/core/crypto/crypto_platform_psa.cpp
 * and thread/key_manager.cpp, OpenThread as bundled with ESP-IDF v6.1) ---- */

/* OPENTHREAD_CONFIG_PSA_ITS_NVM_OFFSET + KeyRefManager::kNetworkKey / kActiveDatasetNetworkKey. */
#define OT_NETWORK_KEY_REF ((psa_key_id_t)0x20001)
#define OT_DATASET_NETWORK_KEY_REF ((psa_key_id_t)0x20003)

/* A made-up key; nothing here is a real network's. */
static const uint8_t k_key[16] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                  0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};

/* Another made-up key, standing for a network's. */
static const uint8_t k_network_key[16] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
                                          0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60};

static psa_status_t import_persistent_key(psa_key_id_t id, const uint8_t material[16]);

/* KeyManager::StoreNetworkKey(): an HMAC key that can sign and be exported,
 * persistent, under a fixed id. */
static psa_status_t import_persistent(psa_key_id_t id)
{
    return import_persistent_key(id, k_key);
}

static psa_status_t import_persistent_key(psa_key_id_t id, const uint8_t material[16])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t out = 0;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(&attributes, id);
    psa_status_t status = psa_import_key(&attributes, material, 16, &out);
    psa_reset_key_attributes(&attributes);
    return status;
}

static int exports_key(psa_key_id_t id, const uint8_t material[16])
{
    uint8_t out[32];
    size_t len = 0;
    return psa_export_key(id, out, sizeof(out), &len) == PSA_SUCCESS && len == 16 && memcmp(out, material, 16) == 0;
}

static int exports_the_key(psa_key_id_t id)
{
    uint8_t out[32];
    size_t len = 0;
    return psa_export_key(id, out, sizeof(out), &len) == PSA_SUCCESS && len == sizeof(k_key) &&
           memcmp(out, k_key, sizeof(k_key)) == 0;
}

/* otPlatCryptoHasKey(). */
static int has_key(psa_key_id_t id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status = psa_get_key_attributes(id, &attributes);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS;
}

static int signs(psa_key_id_t id)
{
    static const uint8_t message[] = "pogo";
    uint8_t mac[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t mac_len = 0;
    return psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, sizeof(message), mac, sizeof(mac), &mac_len) ==
               PSA_SUCCESS &&
           mac_len == sizeof(mac);
}

/* What a restart does to the key store: its RAM is gone, storage is what it
 * was. */
static void restart_key_store(void)
{
    mbedtls_psa_crypto_free();
    pb_volatile_store_test_reset();
    pb_stored_network_test_reset();
    if (psa_crypto_init() != PSA_SUCCESS) {
        check(0, "the key store starts again");
    }
}

static void run_control(void)
{
    printf("\n==== control (no switch) ====\n");
    check(stored_entries() == 0, "storage is empty");
    check(import_persistent(OT_NETWORK_KEY_REF) == PSA_SUCCESS, "the Network Key is imported as a persistent key");
    check(stored_entries() == 1, "control: storage now holds one entry (the key)");
    restart_key_store();
    check(has_key(OT_NETWORK_KEY_REF) && exports_the_key(OT_NETWORK_KEY_REF),
          "control: after a restart the key is still there");
    /* What Instance::ErasePersistentInfo() does to it. */
    check(psa_destroy_key(OT_NETWORK_KEY_REF) == PSA_SUCCESS, "the erase destroys the key");
    check(stored_entries() == 0, "erased: storage is empty");
}

static void run_switch(void)
{
    pb_volatile_store_stats_t stats;

    printf("\n==== switch ====\n");
    /* A board that is enrolled and then erased, as above. */
    check(import_persistent(OT_NETWORK_KEY_REF) == PSA_SUCCESS && stored_entries() == 1,
          "enrolled: the Network Key is in storage");
    check(psa_destroy_key(OT_NETWORK_KEY_REF) == PSA_SUCCESS && stored_entries() == 0, "erased: storage is empty");

    check(pb_volatile_store_engage(NULL), "the switch is engaged and in the path of both stores");

    /* Setting the Dataset again: KeyManager::StoreNetworkKey() and
     * DatasetManager::MoveKeysToSecureStorage(). */
    check(import_persistent(OT_NETWORK_KEY_REF) == PSA_SUCCESS, "the Network Key is imported as a persistent key");
    check(import_persistent(OT_DATASET_NETWORK_KEY_REF) == PSA_SUCCESS, "and the Dataset's copy of it");
    check(stored_entries() == 0, "storage is still empty");
    check(has_key(OT_NETWORK_KEY_REF) && exports_the_key(OT_NETWORK_KEY_REF) && signs(OT_NETWORK_KEY_REF),
          "the key exists for the key store, exports and signs");
    /* The key store may drop a persistent key from its RAM cache at any
     * time and load it again from storage. */
    check(psa_purge_key(OT_NETWORK_KEY_REF) == PSA_SUCCESS, "the key store drops the key from its RAM cache");
    check(exports_the_key(OT_NETWORK_KEY_REF) && signs(OT_NETWORK_KEY_REF),
          "and loads it again -- from the switch's table, not from storage");
    check(import_persistent(OT_NETWORK_KEY_REF) == PSA_ERROR_ALREADY_EXISTS,
          "importing the same id again is refused, as storage would refuse it");
    check(psa_destroy_key(OT_DATASET_NETWORK_KEY_REF) == PSA_SUCCESS && !has_key(OT_DATASET_NETWORK_KEY_REF),
          "a key can be destroyed");
    check(stored_entries() == 0, "storage is still empty");

    pb_volatile_store_get_stats(&stats);
    check(stats.its_kept >= 2 && stats.its_refused == 0, "the key store did write -- into RAM, and it fitted");
    printf("      (ITS writes kept in RAM: %u)\n", (unsigned)stats.its_kept);
    check(g_real_settings_calls == 0, "nothing reached the (absent) settings store");

    restart_key_store();
    check(!has_key(OT_NETWORK_KEY_REF), "after a restart: the key does not exist");
    check(stored_entries() == 0, "after a restart: storage is empty");
}

/* The settings half of pb_stored_network.c reads an NVS namespace; here that
 * is the functional NVS simulator, and it stays empty. */
#define OT_PARTITION "nvs"

static void run_flash_erase(void)
{
    printf("\n==== after the switch: the erase that reaches storage ====\n");
    /* A network's keys in storage itself while the switch is on: what a
     * session that got past the switch would leave. They are put there
     * before the switch is turned; storage does not know the difference. */
    check(import_persistent(OT_NETWORK_KEY_REF) == PSA_SUCCESS &&
              import_persistent(OT_DATASET_NETWORK_KEY_REF) == PSA_SUCCESS && stored_entries() == 2,
          "(the Network Key and the Dataset's copy of it are in storage)");
    check(pb_volatile_store_engage(NULL), "the switch is engaged");
    check(!pb_stored_network_keys_gone(), "the read-back sees them in storage, past the switch");

    /* Instance::ErasePersistentInfo(): psa_destroy_key() for each key. */
    psa_status_t first = psa_destroy_key(OT_NETWORK_KEY_REF);
    psa_status_t second = psa_destroy_key(OT_DATASET_NETWORK_KEY_REF);
    check(first == PSA_SUCCESS && second == PSA_SUCCESS && !has_key(OT_NETWORK_KEY_REF) &&
              !has_key(OT_DATASET_NETWORK_KEY_REF),
          "OpenThread's erase destroys both keys; the key store reports success and no longer has them");
    check(stored_entries() == 2 && !pb_stored_network_keys_gone(),
          "storage still holds both: with the switch on, that erase never reached it");

    check(pb_stored_network_keys_erase() == ESP_OK, "pb_stored_network_keys_erase(): no error");
    check(stored_entries() == 0, "storage is empty");
    check(pb_stored_network_keys_gone() && pb_stored_network_is_erased(OT_PARTITION), "and the read-back says so");
    check(pb_stored_network_keys_erase() == ESP_OK && stored_entries() == 0, "erasing what is not there: no error, nothing");

    restart_key_store();
    check(!has_key(OT_NETWORK_KEY_REF) && !has_key(OT_DATASET_NETWORK_KEY_REF) && stored_entries() == 0,
          "after a restart: neither key exists");
}

/* What the firmware does on a released board before OpenThread starts
 * (pb_mgmt_released_prestart() with enrollment_mgmt.c's two operations). */
static int prestart(void)
{
    (void)pb_stored_network_erase(OT_PARTITION);
    return pb_stored_network_is_erased(OT_PARTITION);
}

/* What OpenThread's start does (KeyManager::Init() -> StoreNetworkKey(random
 * key, overwrite = false)): a stored key is left alone; otherwise the (absent)
 * key is destroyed and a new one imported. 1 when it imported one. */
static int openthread_starts(void)
{
    if (has_key(OT_NETWORK_KEY_REF)) {
        return 0;
    }
    (void)psa_destroy_key(OT_NETWORK_KEY_REF);
    return import_persistent(OT_NETWORK_KEY_REF) == PSA_SUCCESS;
}

static void run_key_origin(void)
{
    pb_volatile_store_its_watch_t watch;

    printf("\n==== whose key is under the Network Key's id? ====\n");
    check(prestart() && stored_entries() == 0, "a released board's boot: storage erased and read back before OpenThread starts");
    check(openthread_starts() && stored_entries() == 1, "OpenThread starts, finds no key, and imports one of its own: it is in storage");
    pb_volatile_store_its_watch_get(&watch);
    check(watch.watching && watch.stored == 1 && watch.removed == 0,
          "for that import the key store made exactly one write of that id (and its destroy of a key that was "
          "not there removed nothing)");
    check(!pb_stored_network_is_erased(OT_PARTITION), "storage is not 'erased' any more");
    check(pb_stored_network_holds_no_network(OT_PARTITION),
          "and holds no network: the key file in storage is, byte for byte, the write that was seen");
    check(psa_purge_key(OT_NETWORK_KEY_REF) == PSA_SUCCESS && signs(OT_NETWORK_KEY_REF),
          "(the key store drops the key from its cache and loads it from storage again)");
    pb_volatile_store_its_watch_get(&watch);
    check(watch.stored == 1 && watch.removed == 0 && pb_stored_network_holds_no_network(OT_PARTITION),
          "which writes nothing: still one write, still no network");
    check(import_persistent(OT_NETWORK_KEY_REF) == PSA_ERROR_ALREADY_EXISTS,
          "the key store refuses to import over a stored key: whoever replaces it has to remove it first");
    pb_volatile_store_its_watch_get(&watch);
    check(watch.stored == 1 && watch.removed == 0, "(and the refused import wrote nothing)");

    /* The next boot. Nothing has asked the key store for that key yet when
     * the firmware erases: OpenThread, the only user of it, has not started. */
    restart_key_store();
    check(stored_entries() == 1 && !pb_stored_network_holds_no_network(OT_PARTITION),
          "after a restart the same key file is of unknown origin to the new boot: not accepted for lying there");
    check(prestart() && stored_entries() == 0,
          "that boot erases it from storage before OpenThread starts, and reads back");
    check(!has_key(OT_NETWORK_KEY_REF), "the key store, asked for the first time now, has no such key");
    check(openthread_starts() && pb_stored_network_holds_no_network(OT_PARTITION),
          "OpenThread makes a new one, seen being stored: no network");

    /* A network's key that an erase left, alone. */
    check(psa_destroy_key(OT_NETWORK_KEY_REF) == PSA_SUCCESS &&
              import_persistent_key(OT_NETWORK_KEY_REF, k_network_key) == PSA_SUCCESS,
          "(a network's key is stored under that id: OpenThread's StoreNetworkKey(key, overwrite) -- destroy, import)");
    pb_volatile_store_its_watch_get(&watch);
    check(watch.stored == 2 && watch.removed == 1 && !pb_stored_network_holds_no_network(OT_PARTITION),
          "that replacement was seen (a remove and a second write): the entry is not this boot's start's any more");
    restart_key_store();
    check(stored_entries() == 1 && !pb_stored_network_holds_no_network(OT_PARTITION),
          "a restart later -- storage holds the network's key and nothing else: not accepted");
    check(!openthread_starts() && exports_key(OT_NETWORK_KEY_REF, k_network_key),
          "OpenThread's start finds a key there and leaves it: what is stored is still the network's key");
    check(!pb_stored_network_holds_no_network(OT_PARTITION), "and it is still not accepted");
    restart_key_store();
    check(prestart() && stored_entries() == 0, "the firmware's boot: the network's key is erased from storage before OpenThread starts");
    check(openthread_starts() && exports_key(OT_NETWORK_KEY_REF, k_key) && !exports_key(OT_NETWORK_KEY_REF, k_network_key),
          "what OpenThread then imports is its own key, not the network's");
    check(pb_stored_network_holds_no_network(OT_PARTITION), "now storage holds no network");

    /* A Dataset stored again brings its copies of the keys. */
    check(import_persistent(OT_DATASET_NETWORK_KEY_REF) == PSA_SUCCESS && !pb_stored_network_holds_no_network(OT_PARTITION),
          "a Dataset's copy of the Network Key in storage: a network is stored");
    check(psa_destroy_key(OT_DATASET_NETWORK_KEY_REF) == PSA_SUCCESS && pb_stored_network_holds_no_network(OT_PARTITION),
          "(removed again: no network)");

    /* Why that erase has to come before anything starts: once the key store
     * has loaded a stored key it answers for it from RAM. */
    restart_key_store();
    check(has_key(OT_NETWORK_KEY_REF) && stored_entries() == 1,
          "(another boot -- and this time the key store is asked for the stored key BEFORE the erase: it loads it)");
    check(prestart() && stored_entries() == 0, "the erase removes the key file from storage all the same, and that is read back");
    check(has_key(OT_NETWORK_KEY_REF) && !openthread_starts() && stored_entries() == 0,
          "but the key store still answers for the key from its RAM, so OpenThread's start would import none: "
          "nothing of it is in storage any more, and it is gone with the next reset");
    check(pb_stored_network_holds_no_network(OT_PARTITION), "storage holds nothing: no network (the question is about storage)");
    check(psa_destroy_key(OT_NETWORK_KEY_REF) == PSA_SUCCESS && stored_entries() == 0 &&
              pb_stored_network_is_erased(OT_PARTITION),
          "(the key store drops it: storage was and is empty)");
}

int main(void)
{
    if (stored_entries() != 0) {
        printf("run this in an empty directory\n");
        return 2;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("psa_crypto_init failed\n");
        return 2;
    }
    run_control();
    run_switch();
    run_flash_erase();
    run_key_origin();
    mbedtls_psa_crypto_free();
    if (g_failures != 0) {
        printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nAll checks passed\n");
    return 0;
}
