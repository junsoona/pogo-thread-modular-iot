/*
 * Host-only stand-in for OpenThread's openthread/joiner.h -- only the
 * otJoinerCallback type that components/thread_manager/include/
 * thread_manager/thread_manager.h names in thread_manager_joiner_start().
 * Matches the real signature (openthread/include/openthread/joiner.h).
 */
#pragma once

#include "openthread/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*otJoinerCallback)(otError aError, void *aContext);

#ifdef __cplusplus
}
#endif
