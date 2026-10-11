#include "power_base_ot_store/pb_volatile_store.h"

#include <stddef.h>
#include <string.h>

#include "openthread/platform/settings.h"
#include "psa_crypto_its.h"

/*
 * The platform's own functions. With -Wl,--wrap=<function> these names are
 * what the linker gives the wrapped definitions (ESP-IDF:
 * components/openthread/src/port/esp_openthread_settings.c and
 * components/mbedtls/port/psa_crypto_storage/esp_psa_its.c). The host tests,
 * whose linkers have no --wrap, get the same two names per function by
 * renaming at compile time; this file is the same for both.
 */
otError __real_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue,
                                 uint16_t *aValueLength);
otError __real_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError __real_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError __real_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex);
void __real_otPlatSettingsWipe(otInstance *aInstance);
psa_status_t __real_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info);
psa_status_t __real_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length);
psa_status_t __real_psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                                psa_storage_create_flags_t create_flags);
psa_status_t __real_psa_its_remove(psa_storage_uid_t uid);

otError __wrap_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue,
                                 uint16_t *aValueLength);
otError __wrap_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError __wrap_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError __wrap_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex);
void __wrap_otPlatSettingsWipe(otInstance *aInstance);
psa_status_t __wrap_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info);
psa_status_t __wrap_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length);
psa_status_t __wrap_psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                                psa_storage_create_flags_t create_flags);
psa_status_t __wrap_psa_its_remove(psa_storage_uid_t uid);

/* OpenThread calls its settings from its own task and PSA guards its key
 * store itself; the lock only keeps the two tables whole should that ever not
 * hold. Everything under it is a bounded copy. */
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define STORE_LOCK() portENTER_CRITICAL(&s_lock)
#define STORE_UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#define STORE_LOCK() ((void)0)
#define STORE_UNLOCK() ((void)0)
#endif

/* Read by engage() only: a settings key and an ITS uid nothing uses. */
#define PROBE_SETTINGS_KEY 0x7f50u
#define PROBE_ITS_UID 0x504f474f50420001ull

static volatile bool s_engaged;
static pb_volatile_store_stats_t s_stats;
/* The one watched storage id (pb_volatile_store.h), and the bytes of its one
 * write while there has been exactly one and no remove. */
static psa_storage_uid_t s_watch_uid;
static pb_volatile_store_its_watch_t s_watch;
static uint8_t s_watch_bytes[PB_VOLATILE_STORE_WATCH_BYTES];
static size_t s_watch_len;
static bool s_watch_kept;

/* Under the lock. */
static void watch_forget_bytes(void)
{
    volatile uint8_t *p = s_watch_bytes;
    for (size_t i = 0; i < sizeof(s_watch_bytes); i++) {
        p[i] = 0;
    }
    s_watch_len = 0;
    s_watch_kept = false;
}

/* ---- the settings table: records of key(2) | length(2) | value, in the
 * order they were added (which is the order of their indices) ---- */

#define ST_HEADER 4u

static uint8_t s_settings[PB_VOLATILE_STORE_SETTINGS_BYTES];
static size_t s_settings_used;

static uint16_t st_key(size_t off)
{
    uint16_t v;
    memcpy(&v, &s_settings[off], sizeof(v));
    return v;
}

static uint16_t st_len(size_t off)
{
    uint16_t v;
    memcpy(&v, &s_settings[off + 2], sizeof(v));
    return v;
}

static size_t st_size(size_t off)
{
    return ST_HEADER + st_len(off);
}

static void st_remove_at(size_t off)
{
    size_t size = st_size(off);
    memmove(&s_settings[off], &s_settings[off + size], s_settings_used - off - size);
    s_settings_used -= size;
    /* What was moved down leaves a copy behind. */
    memset(&s_settings[s_settings_used], 0, size);
}

/* Offset of the `index`-th record of `key`, or SIZE_MAX. */
static size_t st_find(uint16_t key, int index)
{
    int seen = 0;
    for (size_t off = 0; off < s_settings_used; off += st_size(off)) {
        if (st_key(off) == key) {
            if (seen == index) {
                return off;
            }
            seen++;
        }
    }
    return SIZE_MAX;
}

static size_t st_bytes_of(uint16_t key)
{
    size_t bytes = 0;
    for (size_t off = 0; off < s_settings_used; off += st_size(off)) {
        if (st_key(off) == key) {
            bytes += st_size(off);
        }
    }
    return bytes;
}

static void st_append(uint16_t key, const uint8_t *value, uint16_t len)
{
    memcpy(&s_settings[s_settings_used], &key, sizeof(key));
    memcpy(&s_settings[s_settings_used + 2], &len, sizeof(len));
    if (len != 0) {
        memcpy(&s_settings[s_settings_used + ST_HEADER], value, len);
    }
    s_settings_used += ST_HEADER + len;
}

static otError st_store(uint16_t key, const uint8_t *value, uint16_t len, bool replace)
{
    if (value == NULL && len != 0) {
        return OT_ERROR_NO_BUFS;
    }
    size_t freed = replace ? st_bytes_of(key) : 0;
    if (s_settings_used - freed + ST_HEADER + len > sizeof(s_settings)) {
        s_stats.settings_refused++;
        return OT_ERROR_NO_BUFS;
    }
    if (replace) {
        size_t off;
        while ((off = st_find(key, 0)) != SIZE_MAX) {
            st_remove_at(off);
        }
    }
    st_append(key, value, len);
    s_stats.settings_kept++;
    return OT_ERROR_NONE;
}

otError __wrap_otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue,
                                 uint16_t *aValueLength)
{
    otError result = OT_ERROR_NOT_FOUND;
    STORE_LOCK();
    s_stats.settings_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        size_t off = aIndex < 0 ? SIZE_MAX : st_find(aKey, aIndex);
        if (off != SIZE_MAX) {
            uint16_t len = st_len(off);
            if (aValueLength != NULL) {
                uint16_t room = *aValueLength;
                *aValueLength = len;
                if (aValue != NULL) {
                    memcpy(aValue, &s_settings[off + ST_HEADER], room < len ? room : len);
                }
            }
            result = OT_ERROR_NONE;
        }
    }
    STORE_UNLOCK();
    return engaged ? result : __real_otPlatSettingsGet(aInstance, aKey, aIndex, aValue, aValueLength);
}

otError __wrap_otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    otError result = OT_ERROR_NONE;
    STORE_LOCK();
    s_stats.settings_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        result = st_store(aKey, aValue, aValueLength, true);
    }
    STORE_UNLOCK();
    return engaged ? result : __real_otPlatSettingsSet(aInstance, aKey, aValue, aValueLength);
}

otError __wrap_otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    otError result = OT_ERROR_NONE;
    STORE_LOCK();
    s_stats.settings_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        result = st_store(aKey, aValue, aValueLength, false);
    }
    STORE_UNLOCK();
    return engaged ? result : __real_otPlatSettingsAdd(aInstance, aKey, aValue, aValueLength);
}

otError __wrap_otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    otError result = OT_ERROR_NOT_FOUND;
    STORE_LOCK();
    s_stats.settings_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        if (aIndex < 0) {
            size_t off;
            while ((off = st_find(aKey, 0)) != SIZE_MAX) {
                st_remove_at(off);
                result = OT_ERROR_NONE;
            }
        } else {
            size_t off = st_find(aKey, aIndex);
            if (off != SIZE_MAX) {
                st_remove_at(off);
                result = OT_ERROR_NONE;
            }
        }
    }
    STORE_UNLOCK();
    return engaged ? result : __real_otPlatSettingsDelete(aInstance, aKey, aIndex);
}

void __wrap_otPlatSettingsWipe(otInstance *aInstance)
{
    STORE_LOCK();
    s_stats.settings_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        memset(s_settings, 0, sizeof(s_settings));
        s_settings_used = 0;
    }
    STORE_UNLOCK();
    if (!engaged) {
        __real_otPlatSettingsWipe(aInstance);
    }
}

/* ---- the ITS table: records of uid(8) | flags(4) | length(4) | data ---- */

#define ITS_HEADER 16u

static uint8_t s_its[PB_VOLATILE_STORE_ITS_BYTES];
static size_t s_its_used;

static uint64_t its_uid(size_t off)
{
    uint64_t v;
    memcpy(&v, &s_its[off], sizeof(v));
    return v;
}

static uint32_t its_flags(size_t off)
{
    uint32_t v;
    memcpy(&v, &s_its[off + 8], sizeof(v));
    return v;
}

static uint32_t its_len(size_t off)
{
    uint32_t v;
    memcpy(&v, &s_its[off + 12], sizeof(v));
    return v;
}

static size_t its_find(uint64_t uid)
{
    for (size_t off = 0; off < s_its_used; off += ITS_HEADER + its_len(off)) {
        if (its_uid(off) == uid) {
            return off;
        }
    }
    return SIZE_MAX;
}

static void its_remove_at(size_t off)
{
    size_t size = ITS_HEADER + its_len(off);
    memmove(&s_its[off], &s_its[off + size], s_its_used - off - size);
    s_its_used -= size;
    /* Key material: the copy left behind is cleared. */
    memset(&s_its[s_its_used], 0, size);
}

/* The answers follow ESP-IDF's esp_psa_its.c, which PSA's key store is
 * written against. */
psa_status_t __wrap_psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info)
{
    psa_status_t result = PSA_ERROR_DOES_NOT_EXIST;
    STORE_LOCK();
    s_stats.its_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        size_t off = its_find(uid);
        if (p_info == NULL) {
            result = PSA_ERROR_INVALID_ARGUMENT;
        } else if (off != SIZE_MAX) {
            p_info->size = its_len(off);
            p_info->flags = its_flags(off);
            result = PSA_SUCCESS;
        }
    }
    STORE_UNLOCK();
    return engaged ? result : __real_psa_its_get_info(uid, p_info);
}

psa_status_t __wrap_psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                                size_t *p_data_length)
{
    psa_status_t result = PSA_ERROR_DOES_NOT_EXIST;
    STORE_LOCK();
    s_stats.its_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        size_t off = its_find(uid);
        if (p_data == NULL && data_length != 0) {
            result = PSA_ERROR_INVALID_ARGUMENT;
        } else if (off != SIZE_MAX) {
            uint32_t size = its_len(off);
            if (data_offset > size || data_length > size - data_offset) {
                result = PSA_ERROR_INVALID_ARGUMENT;
            } else {
                if (data_length != 0) {
                    memcpy(p_data, &s_its[off + ITS_HEADER + data_offset], data_length);
                }
                if (p_data_length != NULL) {
                    *p_data_length = data_length;
                }
                result = PSA_SUCCESS;
            }
        }
    }
    STORE_UNLOCK();
    return engaged ? result : __real_psa_its_get(uid, data_offset, data_length, p_data, p_data_length);
}

psa_status_t __wrap_psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                                psa_storage_create_flags_t create_flags)
{
    psa_status_t result = PSA_SUCCESS;
    STORE_LOCK();
    s_stats.its_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        size_t off = its_find(uid);
        size_t freed = off == SIZE_MAX ? 0 : ITS_HEADER + its_len(off);
        if (uid == 0) {
            result = PSA_ERROR_INVALID_HANDLE;
        } else if (p_data == NULL && data_length != 0) {
            result = PSA_ERROR_INVALID_ARGUMENT;
        } else if (off != SIZE_MAX && (its_flags(off) & PSA_STORAGE_FLAG_WRITE_ONCE) != 0) {
            result = PSA_ERROR_NOT_PERMITTED;
        } else if (data_length > sizeof(s_its) || s_its_used - freed + ITS_HEADER + data_length > sizeof(s_its)) {
            s_stats.its_refused++;
            result = PSA_ERROR_INSUFFICIENT_STORAGE;
        } else {
            if (off != SIZE_MAX) {
                its_remove_at(off);
            }
            uint64_t uid64 = uid;
            uint32_t flags32 = create_flags;
            memcpy(&s_its[s_its_used], &uid64, sizeof(uid64));
            memcpy(&s_its[s_its_used + 8], &flags32, sizeof(flags32));
            memcpy(&s_its[s_its_used + 12], &data_length, sizeof(data_length));
            if (data_length != 0) {
                memcpy(&s_its[s_its_used + ITS_HEADER], p_data, data_length);
            }
            s_its_used += ITS_HEADER + data_length;
            s_stats.its_kept++;
        }
    }
    STORE_UNLOCK();
    if (engaged) {
        return result;
    }
    result = __real_psa_its_set(uid, data_length, p_data, create_flags);
    if (result == PSA_SUCCESS) {
        STORE_LOCK();
        if (s_watch.watching && uid == s_watch_uid) {
            s_watch.stored++;
            watch_forget_bytes();
            if (s_watch.stored == 1 && s_watch.removed == 0 && data_length <= sizeof(s_watch_bytes) &&
                (p_data != NULL || data_length == 0)) {
                if (data_length != 0) {
                    memcpy(s_watch_bytes, p_data, data_length);
                }
                s_watch_len = data_length;
                s_watch_kept = true;
            }
        }
        STORE_UNLOCK();
    }
    return result;
}

psa_status_t __wrap_psa_its_remove(psa_storage_uid_t uid)
{
    psa_status_t result = PSA_ERROR_DOES_NOT_EXIST;
    STORE_LOCK();
    s_stats.its_calls++;
    bool engaged = s_engaged;
    if (engaged) {
        size_t off = its_find(uid);
        if (off != SIZE_MAX) {
            if ((its_flags(off) & PSA_STORAGE_FLAG_WRITE_ONCE) != 0) {
                result = PSA_ERROR_NOT_PERMITTED;
            } else {
                its_remove_at(off);
                result = PSA_SUCCESS;
            }
        }
    }
    STORE_UNLOCK();
    if (engaged) {
        return result;
    }
    result = __real_psa_its_remove(uid);
    if (result == PSA_SUCCESS) {
        STORE_LOCK();
        if (s_watch.watching && uid == s_watch_uid) {
            s_watch.removed++;
            watch_forget_bytes();
        }
        STORE_UNLOCK();
    }
    return result;
}

void pb_volatile_store_its_watch_start(uint64_t uid)
{
    STORE_LOCK();
    s_watch_uid = (psa_storage_uid_t)uid;
    s_watch.watching = true;
    s_watch.stored = 0;
    s_watch.removed = 0;
    watch_forget_bytes();
    STORE_UNLOCK();
}

bool pb_volatile_store_its_watch_matches(const void *data, size_t length)
{
    bool same = false;
    STORE_LOCK();
    if (s_watch.watching && s_watch.stored == 1 && s_watch.removed == 0 && s_watch_kept && length == s_watch_len &&
        (data != NULL || length == 0)) {
        /* Every byte is looked at, whatever the first difference. */
        uint8_t diff = 0;
        for (size_t i = 0; i < length; i++) {
            diff |= (uint8_t)(((const uint8_t *)data)[i] ^ s_watch_bytes[i]);
        }
        same = diff == 0;
    }
    STORE_UNLOCK();
    return same;
}

void pb_volatile_store_its_watch_get(pb_volatile_store_its_watch_t *out)
{
    STORE_LOCK();
    *out = s_watch;
    STORE_UNLOCK();
}

/* ---- the switch ---- */

bool pb_volatile_store_is_engaged(void)
{
    return s_engaged;
}

void pb_volatile_store_get_stats(pb_volatile_store_stats_t *out)
{
    STORE_LOCK();
    *out = s_stats;
    STORE_UNLOCK();
}

bool pb_volatile_store_engage(void *ot_instance)
{
    pb_volatile_store_stats_t before;
    pb_volatile_store_stats_t after;
    struct psa_storage_info_t info;

    STORE_LOCK();
    s_engaged = true;
    before = s_stats;
    STORE_UNLOCK();

    /* Two reads under the PUBLIC names. In an image linked with the wrap
     * options they arrive in the functions above and are counted; in one that
     * was not, they go to the platform's own functions, find nothing, and the
     * counters stay where they were. */
    (void)otPlatSettingsGet((otInstance *)ot_instance, PROBE_SETTINGS_KEY, 0, NULL, NULL);
    (void)psa_its_get_info((psa_storage_uid_t)PROBE_ITS_UID, &info);

    pb_volatile_store_get_stats(&after);
    return after.settings_calls != before.settings_calls && after.its_calls != before.its_calls;
}

#if defined(PB_VOLATILE_STORE_TEST_HOOKS)
/* Host tests only, never compiled into a firmware: what a chip reset does to
 * this component's RAM. */
void pb_volatile_store_test_reset(void);
void pb_volatile_store_test_reset(void)
{
    s_engaged = false;
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_watch, 0, sizeof(s_watch));
    s_watch_uid = 0;
    watch_forget_bytes();
    memset(s_settings, 0, sizeof(s_settings));
    s_settings_used = 0;
    memset(s_its, 0, sizeof(s_its));
    s_its_used = 0;
}
#endif
