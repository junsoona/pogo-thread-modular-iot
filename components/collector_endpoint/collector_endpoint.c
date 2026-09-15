#include "collector_endpoint/collector_endpoint.h"

#include <string.h>
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static collector_endpoint_snapshot_t s_snapshot;
static bool s_inited;

esp_err_t collector_endpoint_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.valid = false;
    s_inited = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static bool addr_is_valid_collector(const uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN])
{
    static const uint8_t zero[COLLECTOR_ENDPOINT_ADDR_LEN] = {0};
    if (memcmp(addr, zero, COLLECTOR_ENDPOINT_ADDR_LEN) == 0) {
        return false; /* unspecified */
    }
    if (addr[0] == 0xFF) {
        return false; /* multicast, ff00::/8 */
    }
    if (addr[0] == 0xFE && (addr[1] & 0xC0) == 0x80) {
        return false; /* link-local, fe80::/10 */
    }
    return true;
}

esp_err_t collector_endpoint_set(const uint8_t power_base_uuid[COLLECTOR_ENDPOINT_UUID_LEN],
                                  const uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN], uint16_t port, uint32_t generation)
{
    if (power_base_uuid == NULL || addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint8_t zero_uuid[COLLECTOR_ENDPOINT_UUID_LEN] = {0};
    if (memcmp(power_base_uuid, zero_uuid, COLLECTOR_ENDPOINT_UUID_LEN) == 0) {
        return ESP_ERR_INVALID_ARG; /* an all-zero uuid is never a valid permanent identifier */
    }
    if (!addr_is_valid_collector(addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_lock);
    memcpy(s_snapshot.power_base_uuid, power_base_uuid, COLLECTOR_ENDPOINT_UUID_LEN);
    memcpy(s_snapshot.addr, addr, COLLECTOR_ENDPOINT_ADDR_LEN);
    s_snapshot.port = port;
    s_snapshot.generation = generation;
    s_snapshot.valid = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void collector_endpoint_clear(void)
{
    if (!s_inited) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_snapshot.valid = false;
    taskEXIT_CRITICAL(&s_lock);
}

bool collector_endpoint_get(collector_endpoint_snapshot_t *out)
{
    if (out == NULL || !s_inited) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

bool collector_endpoint_is_valid(void)
{
    if (!s_inited) {
        return false;
    }
    bool valid;
    taskENTER_CRITICAL(&s_lock);
    valid = s_snapshot.valid;
    taskEXIT_CRITICAL(&s_lock);
    return valid;
}
