/*
 * Persistent storage for the lower module's provisioning state: the
 * assigned UUID, slot id, and the Active Operational Dataset TLVs handed
 * over POGO UART. Stored in the default "nvs" partition (same partition
 * OpenThread itself uses for its own dataset copy, under a different
 * namespace).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "pogo_proto/pogo_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lower_storage_init(void);

esp_err_t lower_storage_save_provision(const uint8_t uuid[POGO_UUID_LEN], uint8_t slot_id,
                                        const uint8_t *dataset_tlvs, uint16_t dataset_len);

/*
 * Loads a previously saved provisioning record.
 * Returns ESP_ERR_NVS_NOT_FOUND if nothing has been stored yet.
 * dataset_buf_size is the capacity of dataset_out; *dataset_len_out is set
 * to the actual stored length on success.
 */
esp_err_t lower_storage_load_provision(uint8_t uuid_out[POGO_UUID_LEN], uint8_t *slot_id_out, uint8_t *dataset_out,
                                        size_t dataset_buf_size, uint16_t *dataset_len_out);

#ifdef __cplusplus
}
#endif
