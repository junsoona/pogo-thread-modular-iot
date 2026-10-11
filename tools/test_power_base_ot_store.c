/*
 * Host test for the REAL components/power_base_ot_store
 * (docs/power-base-management.md, section 4):
 *   pb_volatile_store.c   the one-way switch in front of OpenThread's settings
 *                         and of PSA Internal Trusted Storage
 *   pb_stored_network.c   "is anything of a Thread network left in flash?",
 *                         "is a network stored?" once OpenThread has started
 *                         (with the origin of what lies under the Network
 *                         Key's id), and the erase that reaches the flash
 *                         past the switch
 *
 * The two platform stores are fakes in this file that record every call; the
 * settings namespace the second file reads is the functional NVS simulator.
 * What a linker's --wrap does for the firmware is spelled out here: the
 * public names forward to the component's __wrap_ functions, and the fakes
 * carry the __real_ names. The same component is run against the real
 * OpenThread core and the real PSA key store by
 * tools/test_power_base_release_session_openthread.sh.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -DPB_VOLATILE_STORE_TEST_HOOKS \
 *      -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I tools/host_test_stubs/power_base_ot_store/ot -I tools/host_test_stubs/power_base_ot_store/psa \
 *      -I components/power_base_ot_store/include \
 *      -o /tmp/test_power_base_ot_store tools/test_power_base_ot_store.c \
 *      components/power_base_ot_store/pb_volatile_store.c components/power_base_ot_store/pb_stored_network.c \
 *      tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_ot_store
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nvs.h"
#include "nvs_sim.h"
#include "openthread/platform/settings.h"
#include "power_base_ot_store/pb_stored_network.h"
#include "power_base_ot_store/pb_volatile_store.h"
#include "psa_crypto_its.h"

void pb_volatile_store_test_reset(void);
void pb_stored_network_test_reset(void);

otError __wrap_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
otError __wrap_otPlatSettingsSet(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __wrap_otPlatSettingsAdd(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __wrap_otPlatSettingsDelete(otInstance *, uint16_t, int);
void __wrap_otPlatSettingsWipe(otInstance *);
psa_status_t __wrap_psa_its_get_info(psa_storage_uid_t, struct psa_storage_info_t *);
psa_status_t __wrap_psa_its_get(psa_storage_uid_t, uint32_t, uint32_t, void *, size_t *);
psa_status_t __wrap_psa_its_set(psa_storage_uid_t, uint32_t, const void *, psa_storage_create_flags_t);
psa_status_t __wrap_psa_its_remove(psa_storage_uid_t);

otError __real_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
otError __real_otPlatSettingsSet(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __real_otPlatSettingsAdd(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __real_otPlatSettingsDelete(otInstance *, uint16_t, int);
void __real_otPlatSettingsWipe(otInstance *);
psa_status_t __real_psa_its_get_info(psa_storage_uid_t, struct psa_storage_info_t *);
psa_status_t __real_psa_its_get(psa_storage_uid_t, uint32_t, uint32_t, void *, size_t *);
psa_status_t __real_psa_its_set(psa_storage_uid_t, uint32_t, const void *, psa_storage_create_flags_t);
psa_status_t __real_psa_its_remove(psa_storage_uid_t);

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

/* ======================= the platform's stores (fakes) ======================= */

/* Every call that reaches a platform function, by kind. */
static struct {
    unsigned settings_reads, settings_writes, its_reads, its_writes;
} g_real;

/* The fake flash: one settings value and a handful of ITS entries are all
 * the scenarios need. */
static struct {
    bool present;
    uint16_t key;
    uint8_t value[16];
    uint16_t len;
} g_flash_setting;

#define FLASH_ITS_MAX 8
static struct {
    bool present;
    psa_storage_uid_t uid;
    /* What was stored (its first byte also says whose entry it is, for this
     * file's assertions). */
    uint8_t data[200];
    uint32_t len;
} g_flash_its[FLASH_ITS_MAX];
/* A get_info / a get of this uid fails although the entry is there (0 =
 * none). */
static psa_storage_uid_t g_flash_its_info_fails_for;
static psa_storage_uid_t g_flash_its_get_fails_for;
/* What the flash answers for a uid it does not hold (a damaged or unreadable
 * store answers something else than "does not exist"). */
static psa_status_t g_flash_its_missing_status = PSA_ERROR_DOES_NOT_EXIST;
/* A remove of this uid fails and removes nothing (0 = none). */
static psa_storage_uid_t g_flash_its_remove_fails_for;

static void flash_reset(void)
{
    memset(&g_real, 0, sizeof(g_real));
    memset(&g_flash_setting, 0, sizeof(g_flash_setting));
    memset(g_flash_its, 0, sizeof(g_flash_its));
    g_flash_its_missing_status = PSA_ERROR_DOES_NOT_EXIST;
    g_flash_its_remove_fails_for = 0;
    g_flash_its_info_fails_for = 0;
    g_flash_its_get_fails_for = 0;
}

static bool flash_its_has(psa_storage_uid_t uid)
{
    for (int i = 0; i < FLASH_ITS_MAX; i++) {
        if (g_flash_its[i].present && g_flash_its[i].uid == uid) {
            return true;
        }
    }
    return false;
}

static int flash_its_index(psa_storage_uid_t uid)
{
    for (int i = 0; i < FLASH_ITS_MAX; i++) {
        if (g_flash_its[i].present && g_flash_its[i].uid == uid) {
            return i;
        }
    }
    return -1;
}

/* Straight into the fake flash: nothing of the code under test sees it. */
static void flash_its_write(psa_storage_uid_t uid, const void *data, uint32_t len)
{
    int i = flash_its_index(uid);
    for (int k = 0; i < 0 && k < FLASH_ITS_MAX; k++) {
        if (!g_flash_its[k].present) {
            i = k;
        }
    }
    if (i < 0 || len > sizeof(g_flash_its[i].data)) {
        return;
    }
    g_flash_its[i].present = true;
    g_flash_its[i].uid = uid;
    g_flash_its[i].len = len;
    memset(g_flash_its[i].data, 0, sizeof(g_flash_its[i].data));
    if (len != 0) {
        memcpy(g_flash_its[i].data, data, len);
    }
}

/* A four-byte entry whose first byte says whose it is. */
static void flash_its_put_as(psa_storage_uid_t uid, char whose)
{
    const char entry[4] = {whose, '.', '.', '.'};
    flash_its_write(uid, entry, sizeof(entry));
}

static void flash_its_put(psa_storage_uid_t uid)
{
    flash_its_put_as(uid, '?');
}

static char flash_its_whose(psa_storage_uid_t uid)
{
    int i = flash_its_index(uid);
    return i < 0 ? 0 : (char)g_flash_its[i].data[0];
}

otError __real_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue,
                                 uint16_t *aValueLength)
{
    (void)aInstance;
    g_real.settings_reads++;
    if (!g_flash_setting.present || g_flash_setting.key != aKey || aIndex != 0) {
        return OT_ERROR_NOT_FOUND;
    }
    if (aValueLength != NULL) {
        if (aValue != NULL) {
            memcpy(aValue, g_flash_setting.value,
                   *aValueLength < g_flash_setting.len ? *aValueLength : g_flash_setting.len);
        }
        *aValueLength = g_flash_setting.len;
    }
    return OT_ERROR_NONE;
}

otError __real_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance;
    g_real.settings_writes++;
    g_flash_setting.present = true;
    g_flash_setting.key = aKey;
    g_flash_setting.len = aValueLength > sizeof(g_flash_setting.value) ? sizeof(g_flash_setting.value) : aValueLength;
    memcpy(g_flash_setting.value, aValue, g_flash_setting.len);
    return OT_ERROR_NONE;
}

otError __real_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    return __real_otPlatSettingsSet(aInstance, aKey, aValue, aValueLength);
}

otError __real_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    (void)aInstance, (void)aIndex;
    g_real.settings_writes++;
    if (!g_flash_setting.present || g_flash_setting.key != aKey) {
        return OT_ERROR_NOT_FOUND;
    }
    g_flash_setting.present = false;
    return OT_ERROR_NONE;
}

void __real_otPlatSettingsWipe(otInstance *aInstance)
{
    (void)aInstance;
    g_real.settings_writes++;
    g_flash_setting.present = false;
}

psa_status_t __real_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info)
{
    g_real.its_reads++;
    if (uid == g_flash_its_info_fails_for) {
        return (psa_status_t)-146; /* PSA_ERROR_STORAGE_FAILURE */
    }
    if (!flash_its_has(uid)) {
        return g_flash_its_missing_status;
    }
    p_info->size = g_flash_its[flash_its_index(uid)].len;
    p_info->flags = 0;
    return PSA_SUCCESS;
}

psa_status_t __real_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length)
{
    g_real.its_reads++;
    int i = flash_its_index(uid);
    if (uid == g_flash_its_get_fails_for) {
        return (psa_status_t)-146; /* PSA_ERROR_STORAGE_FAILURE */
    }
    if (i < 0) {
        return PSA_ERROR_DOES_NOT_EXIST;
    }
    if (data_offset > g_flash_its[i].len || data_length > g_flash_its[i].len - data_offset) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (p_data != NULL && data_length != 0) {
        memcpy(p_data, &g_flash_its[i].data[data_offset], data_length);
    }
    if (p_data_length != NULL) {
        *p_data_length = data_length;
    }
    return PSA_SUCCESS;
}

psa_status_t __real_psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                                psa_storage_create_flags_t create_flags)
{
    (void)create_flags;
    g_real.its_writes++;
    flash_its_write(uid, p_data, data_length);
    return PSA_SUCCESS;
}

psa_status_t __real_psa_its_remove(psa_storage_uid_t uid)
{
    g_real.its_writes++;
    if (uid == g_flash_its_remove_fails_for) {
        return (psa_status_t)-146; /* PSA_ERROR_STORAGE_FAILURE */
    }
    int i = flash_its_index(uid);
    if (i < 0) {
        return PSA_ERROR_DOES_NOT_EXIST;
    }
    g_flash_its[i].present = false;
    return PSA_SUCCESS;
}

/* ======================= what --wrap does: the public names ======================= */

/* False models an image that was linked WITHOUT the wrap options: the public
 * names then are the platform's own functions. */
static bool g_wrapped = true;

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    return g_wrapped ? __wrap_otPlatSettingsGet(aInstance, aKey, aIndex, aValue, aValueLength)
                     : __real_otPlatSettingsGet(aInstance, aKey, aIndex, aValue, aValueLength);
}

otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    return __wrap_otPlatSettingsSet(aInstance, aKey, aValue, aValueLength);
}

otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    return __wrap_otPlatSettingsAdd(aInstance, aKey, aValue, aValueLength);
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    return __wrap_otPlatSettingsDelete(aInstance, aKey, aIndex);
}

void otPlatSettingsWipe(otInstance *aInstance)
{
    __wrap_otPlatSettingsWipe(aInstance);
}

psa_status_t psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info)
{
    return g_wrapped ? __wrap_psa_its_get_info(uid, p_info) : __real_psa_its_get_info(uid, p_info);
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

static unsigned real_calls(void)
{
    return g_real.settings_reads + g_real.settings_writes + g_real.its_reads + g_real.its_writes;
}

static void start(const char *title)
{
    printf("\n-- %s --\n", title);
    pb_volatile_store_test_reset();
    pb_stored_network_test_reset();
    flash_reset();
    nvs_sim_reset();
    g_wrapped = true;
}

/* ======================= scenarios: the switch ======================= */

/* OpenThread's key numbers (src/core/common/settings.hpp). */
#define KEY_ACTIVE_DATASET 0x0001
#define KEY_NETWORK_INFO 0x0003
#define KEY_CHILD_INFO 0x0005
/* OpenThread's persistent key ids on ESP-IDF v6.1 (pb_stored_network.c). */
#define UID_NETWORK_KEY ((psa_storage_uid_t)0x20001)
#define UID_DATASET_NETWORK_KEY ((psa_storage_uid_t)0x20003)

static void scenario_before_the_switch(void)
{
    start("before the switch every call is the platform's, unchanged");
    static const uint8_t dataset[] = {1, 2, 3, 4, 5};
    uint8_t out[16];
    uint16_t len = sizeof(out);
    struct psa_storage_info_t info;

    CHECK(!pb_volatile_store_is_engaged(), "the switch is off after a reset");
    CHECK(otPlatSettingsSet(NULL, KEY_ACTIVE_DATASET, dataset, sizeof(dataset)) == OT_ERROR_NONE &&
              g_flash_setting.present && g_real.settings_writes == 1,
          "a settings write reaches the flash");
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, out, &len) == OT_ERROR_NONE && len == sizeof(dataset) &&
              memcmp(out, dataset, sizeof(dataset)) == 0,
          "and is read back from it");
    CHECK(otPlatSettingsGet(NULL, KEY_NETWORK_INFO, 0, out, &len) == OT_ERROR_NOT_FOUND,
          "the platform's answers come through as they are");
    CHECK(psa_its_set(UID_NETWORK_KEY, 4, "abcd", 0) == PSA_SUCCESS && flash_its_has(UID_NETWORK_KEY),
          "a key-store write reaches the flash");
    CHECK(psa_its_get_info(UID_NETWORK_KEY, &info) == PSA_SUCCESS && info.size == 4, "and is found there");
    CHECK(psa_its_remove(UID_NETWORK_KEY) == PSA_SUCCESS && !flash_its_has(UID_NETWORK_KEY), "a remove reaches it");
    otPlatSettingsWipe(NULL);
    CHECK(!g_flash_setting.present, "and so does a wipe");

    pb_volatile_store_stats_t stats;
    pb_volatile_store_get_stats(&stats);
    CHECK(stats.settings_kept == 0 && stats.its_kept == 0, "nothing was kept in RAM");
}

static void scenario_engage(void)
{
    start("turning the switch, and noticing that it is not in the path");
    CHECK(pb_volatile_store_engage(NULL) && pb_volatile_store_is_engaged(),
          "in an image linked with the wrap options: engaged, and both stores answer through it");
    CHECK(real_calls() == 0, "its two checking reads did not touch the flash");
    CHECK(pb_volatile_store_engage(NULL), "engaging again changes nothing");

    start("(an image linked without the wrap options)");
    g_wrapped = false;
    CHECK(!pb_volatile_store_engage(NULL),
          "not in the path: engage says so -- the caller must not enable Thread on the strength of it");
    CHECK(g_real.settings_writes == 0 && g_real.its_writes == 0, "and the check itself wrote nothing");
}

static void scenario_settings_in_ram(void)
{
    start("after the switch: OpenThread's settings live in RAM and the flash is never called");
    static const uint8_t old_dataset[] = {9, 9, 9};
    uint8_t dataset[254];
    uint8_t out[300];
    uint16_t len;
    for (size_t i = 0; i < sizeof(dataset); i++) {
        dataset[i] = (uint8_t)(i * 7 + 1);
    }
    /* Something that is in flash when the switch is turned (the firmware
     * turns it only on an empty store; this shows the flash is not read
     * either). */
    otPlatSettingsSet(NULL, KEY_ACTIVE_DATASET, old_dataset, sizeof(old_dataset));
    CHECK(pb_volatile_store_engage(NULL), "engaged");
    memset(&g_real, 0, sizeof(g_real));

    len = sizeof(out);
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, out, &len) == OT_ERROR_NOT_FOUND,
          "the RAM table starts empty: what is in flash is not served");
    CHECK(otPlatSettingsSet(NULL, KEY_ACTIVE_DATASET, dataset, sizeof(dataset)) == OT_ERROR_NONE,
          "a full-size Active Dataset (254 bytes) is accepted");
    len = sizeof(out);
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, out, &len) == OT_ERROR_NONE && len == sizeof(dataset) &&
              memcmp(out, dataset, sizeof(dataset)) == 0,
          "and read back whole");
    len = 10;
    memset(out, 0xee, sizeof(out));
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, out, &len) == OT_ERROR_NONE && len == sizeof(dataset) &&
              memcmp(out, dataset, 10) == 0 && out[10] == 0xee,
          "a short buffer gets its part and the full length, nothing past its end");
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, NULL, NULL) == OT_ERROR_NONE &&
              otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 1, NULL, NULL) == OT_ERROR_NOT_FOUND,
          "existence can be asked without a buffer; there is one value");

    static const uint8_t v1[] = {0x11}, v2[] = {0x22, 0x22}, v3[] = {0x33, 0x33, 0x33};
    CHECK(otPlatSettingsAdd(NULL, KEY_CHILD_INFO, v1, sizeof(v1)) == OT_ERROR_NONE &&
              otPlatSettingsAdd(NULL, KEY_CHILD_INFO, v2, sizeof(v2)) == OT_ERROR_NONE &&
              otPlatSettingsAdd(NULL, KEY_CHILD_INFO, v3, sizeof(v3)) == OT_ERROR_NONE,
          "three values added under one key");
    len = sizeof(out);
    bool second = otPlatSettingsGet(NULL, KEY_CHILD_INFO, 1, out, &len) == OT_ERROR_NONE && len == 2 && out[0] == 0x22;
    CHECK(second, "they keep their order: index 1 is the second");
    CHECK(otPlatSettingsDelete(NULL, KEY_CHILD_INFO, 0) == OT_ERROR_NONE, "deleting index 0");
    len = sizeof(out);
    CHECK(otPlatSettingsGet(NULL, KEY_CHILD_INFO, 0, out, &len) == OT_ERROR_NONE && len == 2 && out[0] == 0x22 &&
              otPlatSettingsGet(NULL, KEY_CHILD_INFO, 2, NULL, NULL) == OT_ERROR_NOT_FOUND,
          "moves the others up");
    CHECK(otPlatSettingsDelete(NULL, KEY_CHILD_INFO, 7) == OT_ERROR_NOT_FOUND, "an index that is not there: not found");
    CHECK(otPlatSettingsSet(NULL, KEY_CHILD_INFO, v3, sizeof(v3)) == OT_ERROR_NONE &&
              otPlatSettingsGet(NULL, KEY_CHILD_INFO, 1, NULL, NULL) == OT_ERROR_NOT_FOUND,
          "Set replaces every value of the key with one");
    CHECK(otPlatSettingsDelete(NULL, KEY_CHILD_INFO, -1) == OT_ERROR_NONE &&
              otPlatSettingsDelete(NULL, KEY_CHILD_INFO, -1) == OT_ERROR_NOT_FOUND,
          "Delete with index -1 removes the key; a second time there is nothing");
    len = sizeof(out);
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, out, &len) == OT_ERROR_NONE && len == sizeof(dataset) &&
              memcmp(out, dataset, sizeof(dataset)) == 0,
          "none of that disturbed the Dataset next to it");

    /* Room: the table is finite and says so. */
    unsigned accepted = 0;
    while (accepted < 100 && otPlatSettingsAdd(NULL, KEY_NETWORK_INFO, dataset, 200) == OT_ERROR_NONE) {
        accepted++;
    }
    pb_volatile_store_stats_t stats;
    pb_volatile_store_get_stats(&stats);
    CHECK(accepted >= 6 && accepted < 100 && stats.settings_refused == 1,
          "a write that does not fit is refused (and counted), not passed on");
    len = sizeof(out);
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, out, &len) == OT_ERROR_NONE && len == sizeof(dataset) &&
              memcmp(out, dataset, sizeof(dataset)) == 0,
          "and what was stored before stays intact");
    CHECK(otPlatSettingsSet(NULL, KEY_NETWORK_INFO, v1, sizeof(v1)) == OT_ERROR_NONE,
          "a Set that frees more than it needs fits again");

    otPlatSettingsWipe(NULL);
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, NULL, NULL) == OT_ERROR_NOT_FOUND, "a wipe empties the RAM table");
    CHECK(real_calls() == 0, "in all of this not one call reached the platform's settings or key store");
    CHECK(g_flash_setting.present && g_flash_setting.len == sizeof(old_dataset),
          "the flash is exactly as it was when the switch was turned (not even the wipe went through)");
}

static void scenario_its_in_ram(void)
{
    start("after the switch: PSA Internal Trusted Storage lives in RAM and the flash is never called");
    uint8_t key_file[68];
    uint8_t out[80];
    size_t n = 0;
    struct psa_storage_info_t info;
    for (size_t i = 0; i < sizeof(key_file); i++) {
        key_file[i] = (uint8_t)(0xa0 + i);
    }
    flash_its_put(UID_NETWORK_KEY); /* in flash; must not be served */
    CHECK(pb_volatile_store_engage(NULL), "engaged");
    memset(&g_real, 0, sizeof(g_real));

    CHECK(psa_its_get_info(UID_NETWORK_KEY, &info) == PSA_ERROR_DOES_NOT_EXIST,
          "the RAM table starts empty: what is in flash is not served");
    CHECK(psa_its_set(UID_NETWORK_KEY, sizeof(key_file), key_file, PSA_STORAGE_FLAG_NONE) == PSA_SUCCESS,
          "a persistent key's file (as the PSA key store writes it) is accepted");
    CHECK(psa_its_get_info(UID_NETWORK_KEY, &info) == PSA_SUCCESS && info.size == sizeof(key_file) && info.flags == 0,
          "its size and flags are reported");
    CHECK(psa_its_get(UID_NETWORK_KEY, 0, sizeof(key_file), out, &n) == PSA_SUCCESS && n == sizeof(key_file) &&
              memcmp(out, key_file, sizeof(key_file)) == 0,
          "and it is read back whole");
    CHECK(psa_its_get(UID_NETWORK_KEY, 60, 8, out, &n) == PSA_SUCCESS && n == 8 && memcmp(out, &key_file[60], 8) == 0,
          "a part of it, from an offset");
    CHECK(psa_its_get(UID_NETWORK_KEY, 60, 9, out, &n) == PSA_ERROR_INVALID_ARGUMENT &&
              psa_its_get(UID_NETWORK_KEY, 69, 0, out, &n) == PSA_ERROR_INVALID_ARGUMENT,
          "past its end is an invalid argument (as ESP-IDF's store answers)");
    CHECK(psa_its_get(UID_DATASET_NETWORK_KEY, 0, 4, out, &n) == PSA_ERROR_DOES_NOT_EXIST &&
              psa_its_remove(UID_DATASET_NETWORK_KEY) == PSA_ERROR_DOES_NOT_EXIST,
          "another uid does not exist");
    CHECK(psa_its_set(0, 4, key_file, 0) == PSA_ERROR_INVALID_HANDLE &&
              psa_its_set(UID_DATASET_NETWORK_KEY, 4, NULL, 0) == PSA_ERROR_INVALID_ARGUMENT,
          "uid 0 and a missing buffer are refused");
    CHECK(psa_its_set(UID_NETWORK_KEY, 4, "wxyz", 0) == PSA_SUCCESS &&
              psa_its_get_info(UID_NETWORK_KEY, &info) == PSA_SUCCESS && info.size == 4,
          "setting an existing uid replaces it");
    CHECK(psa_its_set(UID_DATASET_NETWORK_KEY, 4, "once", PSA_STORAGE_FLAG_WRITE_ONCE) == PSA_SUCCESS &&
              psa_its_set(UID_DATASET_NETWORK_KEY, 4, "more", 0) == PSA_ERROR_NOT_PERMITTED &&
              psa_its_remove(UID_DATASET_NETWORK_KEY) == PSA_ERROR_NOT_PERMITTED,
          "a write-once entry can be neither replaced nor removed");
    CHECK(psa_its_remove(UID_NETWORK_KEY) == PSA_SUCCESS && psa_its_get_info(UID_NETWORK_KEY, &info) ==
                                                                 PSA_ERROR_DOES_NOT_EXIST,
          "an entry can be removed");

    uint8_t big[PB_VOLATILE_STORE_ITS_BYTES];
    memset(big, 0x5a, sizeof(big));
    pb_volatile_store_stats_t stats;
    CHECK(psa_its_set((psa_storage_uid_t)0x20002, sizeof(big), big, 0) == PSA_ERROR_INSUFFICIENT_STORAGE,
          "an entry that does not fit is refused, not passed on");
    pb_volatile_store_get_stats(&stats);
    CHECK(stats.its_refused == 1 && stats.its_kept == 3, "kept and refused writes are counted");
    CHECK(real_calls() == 0, "in all of this not one call reached the platform's key store or settings");
    CHECK(flash_its_has(UID_NETWORK_KEY), "the flash is exactly as it was (not even the remove went through)");
}

static void scenario_reset_ends_it(void)
{
    start("a reset is the only way back, and takes the RAM tables with it");
    static const uint8_t v[] = {1, 2, 3};
    CHECK(pb_volatile_store_engage(NULL), "engaged");
    otPlatSettingsSet(NULL, KEY_ACTIVE_DATASET, v, sizeof(v));
    psa_its_set(UID_NETWORK_KEY, sizeof(v), v, 0);
    pb_volatile_store_test_reset(); /* what a chip reset does to RAM */
    struct psa_storage_info_t info;
    CHECK(!pb_volatile_store_is_engaged(), "after the reset the switch is off");
    CHECK(otPlatSettingsGet(NULL, KEY_ACTIVE_DATASET, 0, NULL, NULL) == OT_ERROR_NOT_FOUND &&
              psa_its_get_info(UID_NETWORK_KEY, &info) == PSA_ERROR_DOES_NOT_EXIST,
          "and the flash -- which never saw the Dataset or the key -- has neither");
    CHECK(g_real.settings_writes == 0 && g_real.its_writes == 0, "nothing was ever written to it");
}

/* ======================= scenarios: is the stored network erased? ======================= */

#define OT_PARTITION "nvs"

/* One entry in OpenThread's settings namespace, as ESP-IDF names them. */
static void settings_namespace_put(void)
{
    static const uint8_t blob[] = {0x0e, 0x08, 0, 0, 0, 0, 0, 1, 0, 0};
    nvs_sim_put_blob(OT_PARTITION, "openthread", "OT0100", blob, sizeof(blob));
}

static void scenario_stored_network(void)
{
    start("erased means BOTH stores: the settings and the persistent keys");
    CHECK(pb_stored_network_is_erased(OT_PARTITION), "nothing in either store: erased");
    CHECK(g_real.its_reads == 7, "all seven of OpenThread's persistent key ids were asked for");

    nvs_sim_create_namespace(OT_PARTITION, "openthread");
    CHECK(pb_stored_network_is_erased(OT_PARTITION), "an existing but empty settings namespace: erased");

    settings_namespace_put();
    CHECK(!pb_stored_network_settings_empty(OT_PARTITION) && !pb_stored_network_is_erased(OT_PARTITION),
          "one entry in the settings namespace: not erased");

    start("the Network Key outlives a settings wipe");
    /* What a board looks like when the settings were wiped and the key was
     * not destroyed: the old check (settings only) called this erased. */
    flash_its_put_as(UID_NETWORK_KEY, 'N');
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION),
          "[key origin review] a surviving old Network Key alone must not authorize a release receipt");
    CHECK(pb_stored_network_settings_empty(OT_PARTITION), "the settings namespace is empty");
    CHECK(!pb_stored_network_keys_gone() && !pb_stored_network_is_erased(OT_PARTITION),
          "but the Network Key is still in the key store: NOT erased");

    start("each of OpenThread's persistent keys counts");
    for (psa_storage_uid_t uid = 0x20001; uid <= 0x20007; uid++) {
        flash_reset();
        flash_its_put(uid);
        if (pb_stored_network_is_erased(OT_PARTITION)) {
            CHECK(false, "a persistent key of OpenThread was left and the store was called erased");
        }
    }
    CHECK(true, "any one of the seven left behind: not erased");
    flash_reset();
    flash_its_put((psa_storage_uid_t)0x20008);
    flash_its_put((psa_storage_uid_t)0x1ffff);
    flash_its_put((psa_storage_uid_t)0x504f474f50420001ull);
    CHECK(pb_stored_network_is_erased(OT_PARTITION), "entries that are not OpenThread's keys are not its business");

    start("a store that cannot be read proves nothing");
    g_flash_its_missing_status = (psa_status_t)-146; /* PSA_ERROR_STORAGE_FAILURE */
    CHECK(!pb_stored_network_keys_gone() && !pb_stored_network_is_erased(OT_PARTITION),
          "the key store answers with a failure instead of 'does not exist': not erased");
    g_flash_its_missing_status = PSA_ERROR_DOES_NOT_EXIST;
    nvs_sim_inject(NVS_SIM_OP_ENTRY_FIND, NULL, 1, ESP_FAIL);
    CHECK(!pb_stored_network_is_erased(OT_PARTITION), "the settings namespace cannot be read: not erased");
    CHECK(pb_stored_network_is_erased(OT_PARTITION), "(and readable again: erased)");

    start("the check reads the flash, not the switch's RAM tables");
    static const uint8_t v[] = {1, 2, 3};
    CHECK(pb_volatile_store_engage(NULL), "engaged");
    otPlatSettingsSet(NULL, KEY_ACTIVE_DATASET, v, sizeof(v));
    psa_its_set(UID_NETWORK_KEY, sizeof(v), v, 0);
    CHECK(pb_stored_network_is_erased(OT_PARTITION),
          "a Dataset and a Network Key that exist in RAM only: the flash is erased, and that is the answer");
    flash_its_put(UID_NETWORK_KEY);
    CHECK(!pb_stored_network_is_erased(OT_PARTITION),
          "a key that is in the flash is seen there although the switch is engaged");
    flash_reset();
    settings_namespace_put();
    CHECK(!pb_stored_network_is_erased(OT_PARTITION), "and so is a settings entry");
}

/* ======================= scenarios: whose key is under the Network Key's id? ======================= */

#define UID_PSKC ((psa_storage_uid_t)0x20002)

/* What OpenThread does at every start (Instance::AfterInit() ->
 * KeyManager::Init() -> StoreNetworkKey(random key, overwrite = false)),
 * through the public names, as OpenThread reaches the PSA key store and the
 * key store its storage: a key that is there is left alone; otherwise one is
 * imported. True when it stored one. */
static bool ot_start(void)
{
    struct psa_storage_info_t info;
    if (psa_its_get_info(UID_NETWORK_KEY, &info) != PSA_ERROR_DOES_NOT_EXIST) {
        return false;
    }
    return psa_its_set(UID_NETWORK_KEY, 4, "boot", PSA_STORAGE_FLAG_NONE) == PSA_SUCCESS;
}

/* What the firmware does on a released board before OpenThread starts
 * (pb_mgmt_released_prestart() with enrollment_mgmt.c's two operations). */
static bool prestart(void)
{
    (void)pb_stored_network_erase(OT_PARTITION);
    return pb_stored_network_is_erased(OT_PARTITION);
}

/* A restart: this component's RAM is gone, the flash is what it was. */
static void restart(void)
{
    pb_volatile_store_test_reset();
    pb_stored_network_test_reset();
    memset(&g_real, 0, sizeof(g_real));
}

static void scenario_network_key_origin(void)
{
    pb_volatile_store_its_watch_t watch;

    start("a released board's boot: erased and read back before OpenThread starts, then OpenThread's own key");
    CHECK(prestart(), "before OpenThread starts: both stores erased and read back empty");
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "nothing at all is stored: no network");
    CHECK(ot_start() && flash_its_whose(UID_NETWORK_KEY) == 'b', "OpenThread starts and imports a key of its own");
    pb_volatile_store_its_watch_get(&watch);
    CHECK(watch.watching && watch.stored == 1 && watch.removed == 0,
          "that write was seen at the storage interface: one write of the Network Key's id, no remove");
    CHECK(!pb_stored_network_is_erased(OT_PARTITION), "the store is not 'erased' any more");
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION),
          "but no network is stored: the one entry there is the one this boot saw being made on an empty store");
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION) && !pb_stored_network_is_erased(OT_PARTITION) &&
              pb_stored_network_holds_no_network(OT_PARTITION),
          "asking again -- either question -- changes nothing about that");

    /* The next boot. The flash holds the key the previous start made. */
    restart();
    CHECK(flash_its_has(UID_NETWORK_KEY) && !pb_stored_network_holds_no_network(OT_PARTITION),
          "after a restart the same entry is of unknown origin to the new boot: not accepted because of where it lies");
    CHECK(prestart() && !flash_its_has(UID_NETWORK_KEY), "that boot erases it before OpenThread starts, and reads back");
    CHECK(ot_start() && pb_stored_network_holds_no_network(OT_PARTITION),
          "OpenThread makes a new one, seen being stored: no network -- every boot of a released board goes like this");

    start("an old Network Key that an erase left behind, alone, under a record that says 'erased'");
    flash_its_put_as(UID_NETWORK_KEY, 'N'); /* the network's key; nothing else is left */
    CHECK(pb_stored_network_settings_empty(OT_PARTITION) && !pb_stored_network_is_erased(OT_PARTITION),
          "(settings empty, no Dataset key, no PSKc: only the Network Key)");
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "not accepted: nothing in this boot says whose it is");
    /* What happens if the board simply starts OpenThread on it. */
    CHECK(!ot_start() && flash_its_whose(UID_NETWORK_KEY) == 'N',
          "OpenThread's start finds a key there and leaves it: the network's key is still what is stored");
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION),
          "and it is still not accepted -- a key OpenThread did not make is not OpenThread's for lying where its own would");
    /* The same flash, booted the way the firmware boots a released board. */
    restart();
    CHECK(prestart() && !flash_its_has(UID_NETWORK_KEY),
          "the firmware's boot: the old key is erased before OpenThread starts, and that is read back");
    CHECK(ot_start() && flash_its_whose(UID_NETWORK_KEY) == 'b' && pb_stored_network_holds_no_network(OT_PARTITION),
          "what is stored afterwards is OpenThread's new key, seen being made: now -- and only now -- no network");

    start("the old key cannot be erased");
    flash_its_put_as(UID_NETWORK_KEY, 'N');
    g_flash_its_remove_fails_for = UID_NETWORK_KEY;
    CHECK(!prestart(), "before OpenThread starts: the erase fails, the read-back says 'not empty'");
    CHECK(!ot_start() && flash_its_whose(UID_NETWORK_KEY) == 'N' && !pb_stored_network_holds_no_network(OT_PARTITION),
          "OpenThread leaves the old key where it is; not accepted");
    restart();
    CHECK(!prestart() && !ot_start() && !pb_stored_network_holds_no_network(OT_PARTITION),
          "the next boot, the same failure: the same answer -- nothing is assumed from having tried");
    g_flash_its_remove_fails_for = 0;
    restart();
    CHECK(prestart() && ot_start() && pb_stored_network_holds_no_network(OT_PARTITION),
          "a boot in which the erase works: erased, read back, OpenThread's own key, no network");

    start("the key store cannot be read");
    flash_its_put_as(UID_NETWORK_KEY, 'N');
    g_flash_its_info_fails_for = UID_NETWORK_KEY;
    CHECK(!pb_stored_network_is_erased(OT_PARTITION) && !pb_stored_network_holds_no_network(OT_PARTITION),
          "the Network Key's id answers with a failure: neither 'erased' nor 'no network'");
    g_flash_its_remove_fails_for = UID_NETWORK_KEY; /* and it is still there */
    CHECK(!prestart() && !pb_stored_network_holds_no_network(OT_PARTITION),
          "an erase whose read-back cannot read proves nothing");
    g_flash_its_info_fails_for = 0;
    g_flash_its_remove_fails_for = 0;
    restart();
    CHECK(prestart() && ot_start(), "(a boot with a readable store: erased, read back, OpenThread's own key)");
    g_flash_its_info_fails_for = UID_NETWORK_KEY;
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION),
          "and when that entry cannot be read later in the same boot: not 'no network' either");
    g_flash_its_info_fails_for = 0;
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "(readable again: no network)");

    start("the origin is this boot's history, not the place");
    CHECK(prestart() && ot_start() && pb_stored_network_holds_no_network(OT_PARTITION),
          "(a released board's boot, as above)");
    /* A network's key is put there the way OpenThread does it
     * (KeyManager::StoreNetworkKey(key, overwrite = true): destroy, import). */
    CHECK(psa_its_remove(UID_NETWORK_KEY) == PSA_SUCCESS && psa_its_set(UID_NETWORK_KEY, 4, "Net.", 0) == PSA_SUCCESS,
          "(the entry is replaced by a network's key: a remove and a write)");
    pb_volatile_store_its_watch_get(&watch);
    CHECK(watch.stored == 2 && watch.removed == 1 && !pb_stored_network_holds_no_network(OT_PARTITION),
          "that was seen: the entry is no longer the one this boot's start made -- not accepted");

    restart();
    flash_reset();
    CHECK(prestart() && ot_start(), "(a released board's boot)");
    CHECK(psa_its_set(UID_NETWORK_KEY, 4, "Net.", 0) == PSA_SUCCESS && !pb_stored_network_holds_no_network(OT_PARTITION),
          "written a second time without a remove: not accepted either (exactly one write is OpenThread's start)");

    restart();
    flash_reset();
    CHECK(prestart(), "(both stores read back empty)");
    flash_its_put_as(UID_NETWORK_KEY, 'N'); /* not through the public names: nothing saw it being stored */
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION),
          "an entry that is there although no write of it was seen: not accepted");

    /* Counting is not all: the entry has to BE the write that was seen. */
    restart();
    flash_reset();
    CHECK(prestart() && ot_start() && pb_stored_network_holds_no_network(OT_PARTITION), "(a released board's boot)");
    flash_its_put_as(UID_NETWORK_KEY, 'N'); /* changed past the storage interface: one write seen, no remove */
    pb_volatile_store_its_watch_get(&watch);
    CHECK(watch.stored == 1 && watch.removed == 0 && !pb_stored_network_holds_no_network(OT_PARTITION),
          "the entry changed with nothing looking: the count is still 'one write', and it is not accepted -- what "
          "is in the flash is not what was seen being stored");
    flash_its_write(UID_NETWORK_KEY, "boot", 4);
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "(the very bytes OpenThread's start stored: accepted)");
    flash_its_write(UID_NETWORK_KEY, "boot!", 5);
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "one byte longer: not accepted");
    flash_its_write(UID_NETWORK_KEY, "boo", 3);
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "one byte shorter: not accepted");
    flash_its_write(UID_NETWORK_KEY, "boot", 4);
    g_flash_its_get_fails_for = UID_NETWORK_KEY;
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "the entry cannot be read for the comparison: not accepted");
    g_flash_its_get_fails_for = 0;
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "(readable again: accepted)");

    restart();
    flash_reset();
    CHECK(prestart(), "(both stores read back empty)");
    static uint8_t big[PB_VOLATILE_STORE_WATCH_BYTES + 1];
    memset(big, 0x5a, sizeof(big));
    CHECK(psa_its_set(UID_NETWORK_KEY, sizeof(big), big, 0) == PSA_SUCCESS &&
              !pb_stored_network_holds_no_network(OT_PARTITION),
          "a first write too long to be kept for the comparison: not accepted (nothing is assumed about it)");
    restart();
    flash_reset();
    CHECK(prestart() && psa_its_set(UID_NETWORK_KEY, PB_VOLATILE_STORE_WATCH_BYTES, big, 0) == PSA_SUCCESS &&
              pb_stored_network_holds_no_network(OT_PARTITION),
          "(one that just fits: kept, compared, accepted)");
    CHECK(pb_volatile_store_its_watch_matches(big, PB_VOLATILE_STORE_WATCH_BYTES) &&
              !pb_volatile_store_its_watch_matches(big, PB_VOLATILE_STORE_WATCH_BYTES - 1) &&
              !pb_volatile_store_its_watch_matches(NULL, PB_VOLATILE_STORE_WATCH_BYTES),
          "the comparison itself: the same bytes and length, nothing less");
    CHECK(psa_its_remove(UID_NETWORK_KEY) == PSA_SUCCESS && !pb_volatile_store_its_watch_matches(big, PB_VOLATILE_STORE_WATCH_BYTES),
          "a remove forgets the kept bytes: nothing matches any more");

    restart();
    flash_reset();
    flash_its_put_as(UID_NETWORK_KEY, 'N');
    CHECK(!ot_start() && psa_its_remove(UID_NETWORK_KEY) == PSA_SUCCESS && ot_start(),
          "(no read-back in this boot; the old key is removed and a new one stored all the same)");
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION),
          "without the read-back that found both stores empty, a write that was seen proves nothing: not accepted");

    start("a stored network is seen whatever part of it is looked at");
    CHECK(prestart() && ot_start() && pb_stored_network_holds_no_network(OT_PARTITION), "(a released board's boot)");
    settings_namespace_put();
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "one entry in the settings namespace: a network is stored");
    nvs_sim_reset();
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "(gone again: no network)");
    bool any_missed = false;
    for (psa_storage_uid_t uid = 0x20002; uid <= 0x20007; uid++) {
        flash_its_put(uid);
        if (pb_stored_network_holds_no_network(OT_PARTITION)) {
            any_missed = true;
        }
        (void)__real_psa_its_remove(uid);
    }
    CHECK(!any_missed && pb_stored_network_holds_no_network(OT_PARTITION),
          "the PSKc, any Dataset's copy of a key, the ECDSA key: each alone means a network is stored");
    flash_its_put((psa_storage_uid_t)0x20008);
    flash_its_put((psa_storage_uid_t)0x504f474f50420001ull);
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "entries that are not OpenThread's keys are not its business");

    start("a store that cannot be read proves nothing");
    CHECK(prestart() && ot_start(), "(a released board's boot)");
    g_flash_its_missing_status = (psa_status_t)-146; /* PSA_ERROR_STORAGE_FAILURE */
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "the key store answers with a failure: not 'no network'");
    g_flash_its_missing_status = PSA_ERROR_DOES_NOT_EXIST;
    nvs_sim_inject(NVS_SIM_OP_ENTRY_FIND, NULL, 1, ESP_FAIL);
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "the settings namespace cannot be read: not 'no network'");
    CHECK(pb_stored_network_holds_no_network(OT_PARTITION), "(and readable again: no network)");

    start("it reads the flash, not the switch's RAM tables");
    static const uint8_t v[] = {1, 2, 3};
    CHECK(prestart() && ot_start() && pb_volatile_store_engage(NULL), "(a released board's boot; then the switch)");
    otPlatSettingsSet(NULL, KEY_ACTIVE_DATASET, v, sizeof(v));
    psa_its_set(UID_PSKC, sizeof(v), v, 0);
    CHECK(psa_its_remove(UID_NETWORK_KEY) == PSA_ERROR_DOES_NOT_EXIST && psa_its_set(UID_NETWORK_KEY, sizeof(v), v, 0) == PSA_SUCCESS,
          "(with the switch on, a Dataset, a PSKc and a Network Key are written -- into RAM)");
    pb_volatile_store_its_watch_get(&watch);
    CHECK(watch.stored == 1 && watch.removed == 0 && pb_stored_network_holds_no_network(OT_PARTITION),
          "writes that stayed in RAM are not counted as writes of the flash, and the flash holds no network");
    flash_its_put(UID_PSKC);
    CHECK(!pb_stored_network_holds_no_network(OT_PARTITION), "a PSKc in the flash is seen there although the switch is engaged");
}

/* ======================= scenarios: the erase that reaches the flash ======================= */

/* A network in both stores themselves: a settings entry and four keys. */
static void flash_network_put(void)
{
    settings_namespace_put();
    flash_its_put(UID_NETWORK_KEY);
    flash_its_put(UID_PSKC);
    flash_its_put(UID_DATASET_NETWORK_KEY);
    flash_its_put((psa_storage_uid_t)0x20004);
}

static void scenario_flash_erase(void)
{
    start("after the switch, OpenThread's own erase does not reach the flash");
    CHECK(pb_volatile_store_engage(NULL), "engaged");
    /* What a session that got past the switch would leave. */
    flash_network_put();
    memset(&g_real, 0, sizeof(g_real));
    CHECK(!pb_stored_network_is_erased(OT_PARTITION), "(a network is in the flash)");
    /* otInstanceErasePersistentInfo(): a settings wipe, and a destroy of
     * every persistent key -- through the public names. */
    otPlatSettingsWipe(NULL);
    for (psa_storage_uid_t uid = 0x20001; uid <= 0x20007; uid++) {
        (void)psa_its_remove(uid);
    }
    CHECK(g_real.settings_writes == 0 && g_real.its_writes == 0,
          "the wipe and the seven removes were answered from RAM: not one reached the platform");
    CHECK(!pb_stored_network_is_erased(OT_PARTITION) && flash_its_has(UID_NETWORK_KEY) &&
              nvs_sim_peek_blob(OT_PARTITION, "openthread", "OT0100", NULL, 0) >= 0,
          "and the flash is as it was: that erase erased nothing");

    CHECK(pb_stored_network_erase(OT_PARTITION) == ESP_OK, "pb_stored_network_erase(): no error");
    CHECK(g_real.its_writes == 7, "it removed all seven key ids from storage itself, past the switch");
    CHECK(nvs_sim_peek_blob(OT_PARTITION, "openthread", "OT0100", NULL, 0) < 0 && !flash_its_has(UID_NETWORK_KEY) &&
              !flash_its_has(UID_PSKC) && !flash_its_has(UID_DATASET_NETWORK_KEY) &&
              !flash_its_has((psa_storage_uid_t)0x20004),
          "the settings entry and every key are gone from the flash");
    CHECK(pb_stored_network_is_erased(OT_PARTITION), "and the read-back says so: erased");
    CHECK(pb_volatile_store_is_engaged(), "(the switch is still on: nothing here turns it back)");

    start("the erase is durable, and touches nothing else");
    char path[64];
    snprintf(path, sizeof(path), "/tmp/pb_ot_store_flash_%d.bin", (int)getpid());
    unlink(path);
    CHECK(nvs_sim_set_backing_file(path), "(a flash file)");
    static const uint8_t other[] = {7, 7};
    nvs_sim_put_blob(OT_PARTITION, "pb_enroll", "marker", other, sizeof(other));
    settings_namespace_put();
    flash_its_put((psa_storage_uid_t)0x20008); /* not one of OpenThread's */
    flash_its_put(UID_PSKC);
    int commits = nvs_sim_commit_count();
    CHECK(pb_stored_network_erase(OT_PARTITION) == ESP_OK && nvs_sim_commit_count() == commits + 1,
          "erased, with one commit");
    CHECK(nvs_sim_set_backing_file(path) && nvs_sim_peek_blob(OT_PARTITION, "openthread", "OT0100", NULL, 0) < 0,
          "after a restart the settings entry is still gone");
    CHECK(nvs_sim_peek_blob(OT_PARTITION, "pb_enroll", "marker", NULL, 0) == (int)sizeof(other) &&
              flash_its_has((psa_storage_uid_t)0x20008),
          "another namespace and another key id were not touched");
    unlink(path);

    start("nothing stored: nothing is opened, written or created");
    int before = nvs_sim_commit_count();
    CHECK(pb_stored_network_erase(OT_PARTITION) == ESP_OK && nvs_sim_commit_count() == before &&
              g_real.its_writes == 7,
          "no error, no commit; the seven removes each said 'does not exist'");
    CHECK(pb_stored_network_is_erased(OT_PARTITION), "erased, as before");

    start("an erase that fails says so, and the read-back is what counts");
    flash_network_put();
    nvs_sim_inject(NVS_SIM_OP_ERASE_ALL, NULL, 1, ESP_FAIL);
    CHECK(pb_stored_network_erase(OT_PARTITION) != ESP_OK, "the settings erase fails: an error");
    CHECK(!flash_its_has(UID_NETWORK_KEY) && !flash_its_has(UID_PSKC),
          "the keys were removed all the same (both stores are tried)");
    CHECK(!pb_stored_network_is_erased(OT_PARTITION), "not erased: the settings entry is still there");
    nvs_sim_inject(NVS_SIM_OP_COMMIT, NULL, 1, ESP_FAIL);
    CHECK(pb_stored_network_settings_erase(OT_PARTITION) != ESP_OK, "the commit fails: an error");
    CHECK(pb_stored_network_settings_erase(OT_PARTITION) == ESP_OK && pb_stored_network_is_erased(OT_PARTITION),
          "(the next attempt erases)");

    flash_network_put();
    nvs_sim_inject(NVS_SIM_OP_ERASE_ALL, NULL, 1, ESP_OK); /* claims success, erased nothing */
    CHECK(pb_stored_network_erase(OT_PARTITION) == ESP_OK && !pb_stored_network_is_erased(OT_PARTITION),
          "an erase that reports success and erased nothing: no error -- and the read-back says 'not erased'");
    CHECK(pb_stored_network_erase(OT_PARTITION) == ESP_OK && pb_stored_network_is_erased(OT_PARTITION), "(again: erased)");

    flash_network_put();
    g_flash_its_remove_fails_for = UID_DATASET_NETWORK_KEY;
    CHECK(pb_stored_network_erase(OT_PARTITION) != ESP_OK, "one key cannot be removed: an error");
    CHECK(!flash_its_has(UID_NETWORK_KEY) && !flash_its_has(UID_PSKC) && !flash_its_has((psa_storage_uid_t)0x20004) &&
              flash_its_has(UID_DATASET_NETWORK_KEY) && pb_stored_network_settings_empty(OT_PARTITION),
          "everything else was still removed");
    CHECK(!pb_stored_network_is_erased(OT_PARTITION) && !pb_stored_network_holds_no_network(OT_PARTITION),
          "not erased, and a later boot sees a stored network");
    nvs_sim_inject(NVS_SIM_OP_ENTRY_FIND, NULL, 1, ESP_FAIL);
    CHECK(pb_stored_network_settings_erase(OT_PARTITION) != ESP_OK, "a settings namespace that cannot be read: an error, not 'nothing there'");

    start("before the switch the same erase works as well");
    flash_network_put();
    CHECK(!pb_volatile_store_is_engaged() && pb_stored_network_erase(OT_PARTITION) == ESP_OK &&
              pb_stored_network_is_erased(OT_PARTITION),
          "erased and read back");
}

int main(void)
{
    scenario_before_the_switch();
    scenario_engage();
    scenario_settings_in_ram();
    scenario_its_in_ram();
    scenario_reset_ends_it();
    scenario_stored_network();
    scenario_network_key_origin();
    scenario_flash_erase();

    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
