/*
 * Host-only stand-in for openthread/platform/settings.h, for
 * tools/test_power_base_ot_store.c: the five settings functions
 * components/power_base_ot_store stands in front of, and the otError values it
 * returns. The numeric values are OpenThread's (include/openthread/error.h);
 * tools/test_power_base_release_session_openthread.sh compiles the same
 * component against the real headers.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct otInstance otInstance;

typedef enum {
    OT_ERROR_NONE = 0,
    OT_ERROR_NO_BUFS = 3,
    OT_ERROR_NOT_FOUND = 23,
} otError;

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength);
otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength);
otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex);
void otPlatSettingsWipe(otInstance *aInstance);

#ifdef __cplusplus
}
#endif
