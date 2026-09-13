#include "node_calc/node_calc.h"

#include <stddef.h>
#include <stdint.h>

esp_err_t node_calc_polling_period_ms(uint32_t response_time_ms, uint32_t *out_period_ms)
{
    if (response_time_ms == 0 || out_period_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t period;
    if (response_time_ms < 50) {
        period = (uint64_t)response_time_ms * 15;
    } else if (response_time_ms < 100) {
        period = (uint64_t)response_time_ms * 10;
    } else if (response_time_ms < 500) {
        period = (uint64_t)response_time_ms * 8;
    } else if (response_time_ms <= 1000) {
        period = (uint64_t)response_time_ms * 3;
    } else if (response_time_ms <= 10000) {
        period = (uint64_t)response_time_ms * 3 / 2; /* M=1.5, integer-safe */
    } else {
        period = (uint64_t)response_time_ms * 1;
    }
    if (period > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_period_ms = (uint32_t)period;
    return ESP_OK;
}

uint32_t node_calc_uuid_hash(const uint8_t uuid[NODE_CALC_UUID_LEN])
{
    uint32_t h = 2166136261u; /* FNV-1a offset basis */
    for (int i = 0; i < NODE_CALC_UUID_LEN; i++) {
        h ^= uuid[i];
        h *= 16777619u; /* FNV-1a prime */
    }
    return h;
}

uint16_t node_calc_backoff_ms(const uint8_t uuid[NODE_CALC_UUID_LEN])
{
    return (uint16_t)(node_calc_uuid_hash(uuid) & 0xFF); /* low 8 bits -> 0..255 */
}
