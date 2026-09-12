/*
 * Thread adapter for the upper module: starts OpenThread as an FTD
 * (Leader/Router), forms a network from Kconfig defaults if none is
 * already commissioned, and exposes the Active Operational Dataset TLVs
 * for handoff to the lower module over POGO UART.
 */
#pragma once

#include "esp_err.h"
#include "openthread/thread.h"
#include "openthread/dataset.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the OpenThread stack and ensures it is running as Leader (forms
 * a new network from Kconfig OPENTHREAD_NETWORK_* defaults on first boot,
 * or resumes an already-commissioned dataset on subsequent boots). */
esp_err_t upper_thread_init(void);

/* Reads the current Active Operational Dataset as raw TLVs. Thread-safe
 * (acquires the OpenThread API lock internally). */
esp_err_t upper_thread_get_active_dataset_tlvs(otOperationalDatasetTlvs *out_tlvs);

/* Thread-safe wrapper around otThreadGetDeviceRole(), for logging. */
otDeviceRole upper_thread_get_role(void);

#ifdef __cplusplus
}
#endif
