/*
 * Thread adapter for the lower module: starts OpenThread as an MTD
 * configured as a Sleepy End Device (rx-off-when-idle), applies an Active
 * Operational Dataset received over POGO UART, and reports role changes
 * (e.g. reaching OT_DEVICE_ROLE_CHILD) so the pairing state machine can
 * detect a successful join without polling the OpenThread API on every
 * loop iteration.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "openthread/thread.h"
#include "openthread/dataset.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the OpenThread stack as MTD and configures the SED link mode
 * (rx-off-when-idle, cannot be promoted to Router). Does NOT bring up an
 * Active Dataset -- call lower_thread_apply_dataset_and_join() once a
 * dataset has been received (or restored from NVS). */
esp_err_t lower_thread_init(void);

/* Validates dataset_tlvs via otDatasetIsValid() and, if valid, sets it as
 * the Active Dataset and enables IPv6 + Thread (this is what actually
 * starts the join attempt). Returns ESP_ERR_INVALID_ARG if the TLVs don't
 * parse as a valid dataset. */
esp_err_t lower_thread_apply_dataset_and_join(const uint8_t *dataset_tlvs, uint16_t dataset_len);

/* Thread-safe wrapper around otThreadGetDeviceRole(). */
otDeviceRole lower_thread_get_role(void);

/* Thread-safe wrapper around otDatasetIsValid(&tlvs, true), for validating
 * a dataset before committing it to NVS. */
bool lower_thread_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len);

/* Non-blocking: returns true and fills *out_role if the role has changed
 * since the last call (backed by a small internal queue fed from the
 * OpenThread state-changed callback). Returns false if there is nothing
 * new. */
bool lower_thread_poll_role_change(otDeviceRole *out_role);

#ifdef __cplusplus
}
#endif
