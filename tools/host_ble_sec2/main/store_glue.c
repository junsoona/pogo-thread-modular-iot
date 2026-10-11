/* See store_glue.h. */
#include "store_glue.h"

#include <string.h>

#include "nvs.h"
#include "nvs_sim.h"
#include "openthread/platform/settings.h"
#include "power_base_ot_store/pb_stored_network.h"
#include "power_base_ot_store/pb_volatile_store.h"
#include "psa_crypto_its.h"

#define OT_PARTITION "nvs"
#define UID_NETWORK_KEY ((psa_storage_uid_t)0x20001)
#define UID_DATASET_NETWORK_KEY ((psa_storage_uid_t)0x20003)

void pb_volatile_store_test_reset(void);
void pb_stored_network_test_reset(void);

/* components/power_base_ot_store, as the firmware links it: the names every
 * other object file's calls resolve to. */
psa_status_t __wrap_psa_its_get_info(psa_storage_uid_t, struct psa_storage_info_t *);
psa_status_t __wrap_psa_its_set(psa_storage_uid_t, uint32_t, const void *, psa_storage_create_flags_t);

/* ---- the key storage itself ---- */

#define ENTRIES 8
static struct {
    bool present;
    psa_storage_uid_t uid;
    uint8_t data[64];
    uint32_t len;
} g_its[ENTRIES];
static uint8_t g_faults;

static int its_index(psa_storage_uid_t uid)
{
    for (int i = 0; i < ENTRIES; i++) {
        if (g_its[i].present && g_its[i].uid == uid) {
            return i;
        }
    }
    return -1;
}

static void its_write(psa_storage_uid_t uid, const void *data, uint32_t len)
{
    int i = its_index(uid);
    for (int k = 0; i < 0 && k < ENTRIES; k++) {
        if (!g_its[k].present) {
            i = k;
        }
    }
    if (i < 0 || len > sizeof(g_its[i].data)) {
        return;
    }
    g_its[i].present = true;
    g_its[i].uid = uid;
    g_its[i].len = len;
    memset(g_its[i].data, 0, sizeof(g_its[i].data));
    if (len != 0) {
        memcpy(g_its[i].data, data, len);
    }
}

psa_status_t __real_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info);
psa_status_t __real_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length);
psa_status_t __real_psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                                psa_storage_create_flags_t create_flags);
psa_status_t __real_psa_its_remove(psa_storage_uid_t uid);

#define STORAGE_FAILURE ((psa_status_t)-146) /* PSA_ERROR_STORAGE_FAILURE */

psa_status_t __real_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info)
{
    if ((g_faults & HSTORE_FAULT_UNREADABLE) != 0) {
        return STORAGE_FAILURE;
    }
    int i = its_index(uid);
    if (i < 0) {
        return PSA_ERROR_DOES_NOT_EXIST;
    }
    p_info->size = g_its[i].len;
    p_info->flags = 0;
    return PSA_SUCCESS;
}

psa_status_t __real_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length)
{
    if ((g_faults & HSTORE_FAULT_UNREADABLE) != 0) {
        return STORAGE_FAILURE;
    }
    int i = its_index(uid);
    if (i < 0) {
        return PSA_ERROR_DOES_NOT_EXIST;
    }
    if (data_offset > g_its[i].len || data_length > g_its[i].len - data_offset) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (p_data != NULL && data_length != 0) {
        memcpy(p_data, &g_its[i].data[data_offset], data_length);
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
    its_write(uid, p_data, data_length);
    return PSA_SUCCESS;
}

psa_status_t __real_psa_its_remove(psa_storage_uid_t uid)
{
    if (uid == UID_NETWORK_KEY && (g_faults & HSTORE_FAULT_KEY_NOT_REMOVABLE) != 0) {
        return STORAGE_FAILURE;
    }
    int i = its_index(uid);
    if (i < 0) {
        return PSA_ERROR_DOES_NOT_EXIST;
    }
    g_its[i].present = false;
    return PSA_SUCCESS;
}

/* OpenThread's settings are read by the component straight from NVS (the
 * simulator); the platform's settings functions are only what the switch
 * would pass calls on to, and nothing here calls them. */
otError __real_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
otError __real_otPlatSettingsSet(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __real_otPlatSettingsAdd(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __real_otPlatSettingsDelete(otInstance *, uint16_t, int);
void __real_otPlatSettingsWipe(otInstance *);

otError __real_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    (void)aInstance, (void)aKey, (void)aIndex, (void)aValue, (void)aValueLength;
    return OT_ERROR_NOT_FOUND;
}

otError __real_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance, (void)aKey, (void)aValue, (void)aValueLength;
    return OT_ERROR_NONE;
}

otError __real_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance, (void)aKey, (void)aValue, (void)aValueLength;
    return OT_ERROR_NONE;
}

otError __real_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    (void)aInstance, (void)aKey, (void)aIndex;
    return OT_ERROR_NOT_FOUND;
}

void __real_otPlatSettingsWipe(otInstance *aInstance)
{
    (void)aInstance;
}

/* ---- what harness_main.c drives ---- */

void hstore_restart_ram(void)
{
    pb_volatile_store_test_reset();
    pb_stored_network_test_reset();
}

void hstore_wipe(void)
{
    memset(g_its, 0, sizeof(g_its));
    g_faults = 0;
    (void)pb_stored_network_settings_erase(OT_PARTITION);
    hstore_restart_ram();
}

void hstore_plant(uint8_t what)
{
    static const uint8_t dataset[] = {0x0e, 0x08, 0, 0, 0, 0, 0, 1, 0, 0};
    switch (what) {
    case HSTORE_PLANT_OLD_NETWORK_KEY: its_write(UID_NETWORK_KEY, "Net.", 4); break;
    case HSTORE_PLANT_NETWORK:
        nvs_sim_put_blob(OT_PARTITION, "openthread", "OT0100", dataset, sizeof(dataset));
        its_write(UID_NETWORK_KEY, "Net.", 4);
        its_write(UID_DATASET_NETWORK_KEY, "Net.", 4);
        break;
    case HSTORE_PLANT_EARLIER_BOOT_KEY: its_write(UID_NETWORK_KEY, "boot", 4); break;
    default: break;
    }
}

void hstore_set_faults(uint8_t faults)
{
    g_faults = faults;
}

int hstore_erase(void *ctx)
{
    (void)ctx;
    return (int)pb_stored_network_erase(OT_PARTITION);
}

bool hstore_erased(void *ctx)
{
    (void)ctx;
    return pb_stored_network_is_erased(OT_PARTITION);
}

bool hstore_openthread_starts(void)
{
    struct psa_storage_info_t info;
    if (__wrap_psa_its_get_info(UID_NETWORK_KEY, &info) != PSA_ERROR_DOES_NOT_EXIST) {
        return false; /* a key is there (or the store cannot be read): left alone */
    }
    return __wrap_psa_its_set(UID_NETWORK_KEY, 4, "boot", PSA_STORAGE_FLAG_NONE) == PSA_SUCCESS;
}

bool hstore_no_network(void *ctx)
{
    (void)ctx;
    return pb_stored_network_holds_no_network(OT_PARTITION);
}

void hstore_state(uint8_t out[5])
{
    pb_volatile_store_its_watch_t watch;
    pb_volatile_store_its_watch_get(&watch);
    int i = its_index(UID_NETWORK_KEY);
    out[0] = i >= 0 ? 1 : 0;
    out[1] = i >= 0 ? g_its[i].data[0] : 0;
    out[2] = nvs_sim_peek_blob(OT_PARTITION, "openthread", "OT0100", NULL, 0) >= 0 ? 1 : 0;
    out[3] = (uint8_t)watch.stored;
    out[4] = (uint8_t)watch.removed;
}
