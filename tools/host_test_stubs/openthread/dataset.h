/*
 * Host-only stand-in for OpenThread's openthread/dataset.h -- only
 * otOperationalDatasetTlvs, used by components/thread_manager/include/
 * thread_manager/thread_manager.h's thread_manager_get_active_dataset_tlvs()
 * and, through it, components/parent_service/parent_service.c. Field names
 * and OT_OPERATIONAL_DATASET_MAX_LENGTH match the real OpenThread struct
 * (openthread/include/openthread/dataset.h) exactly.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OT_OPERATIONAL_DATASET_MAX_LENGTH 254

typedef struct {
    uint8_t mTlvs[OT_OPERATIONAL_DATASET_MAX_LENGTH];
    uint8_t mLength;
} otOperationalDatasetTlvs;

#ifdef __cplusplus
}
#endif
