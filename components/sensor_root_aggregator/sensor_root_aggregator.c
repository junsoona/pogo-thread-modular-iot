#include "sensor_root_aggregator/sensor_root_aggregator.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sensor_root_aggregator";

static SemaphoreHandle_t s_agg_mutex;
static sensor_root_aggregator_node_t s_nodes[SENSOR_ROOT_AGGREGATOR_MAX_NODES];

esp_err_t sensor_root_aggregator_init(void)
{
    if (s_agg_mutex == NULL) {
        s_agg_mutex = xSemaphoreCreateMutex();
        if (s_agg_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_agg_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(s_nodes, 0, sizeof(s_nodes));
        xSemaphoreGive(s_agg_mutex);
    }
    return ESP_OK;
}

sensor_root_aggregator_result_t sensor_root_aggregator_record_sample_result(const sensor_sample_t *sample)
{
    if (sample == NULL || s_agg_mutex == NULL) {
        return SENSOR_ROOT_AGGREGATOR_INVALID;
    }
    if (xSemaphoreTake(s_agg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "aggregator busy -- dropping one sample (uuid suffix ..%02x%02x)", sample->module_uuid[6],
                 sample->module_uuid[7]);
        return SENSOR_ROOT_AGGREGATOR_BUSY;
    }

    int slot = -1;
    int free_slot = -1;
    for (int i = 0; i < SENSOR_ROOT_AGGREGATOR_MAX_NODES; i++) {
        if (s_nodes[i].valid && memcmp(s_nodes[i].module_uuid, sample->module_uuid, SENSOR_UUID_LEN) == 0) {
            slot = i;
            break;
        }
        if (!s_nodes[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }
    if (slot < 0) {
        if (free_slot < 0) {
            ESP_LOGW(TAG, "aggregator full (%d nodes) -- dropping sample from a new module (uuid suffix ..%02x%02x)",
                     SENSOR_ROOT_AGGREGATOR_MAX_NODES, sample->module_uuid[6], sample->module_uuid[7]);
            xSemaphoreGive(s_agg_mutex);
            return SENSOR_ROOT_AGGREGATOR_FULL;
        }
        slot = free_slot;
        s_nodes[slot].valid = true;
        memcpy(s_nodes[slot].module_uuid, sample->module_uuid, SENSOR_UUID_LEN);
    }

    sensor_root_aggregator_node_t *rec = &s_nodes[slot];
    if (rec->has_sample && rec->last_sample_seq == sample->sample_seq) {
        /* Exact repeat of the last-recorded (uuid, sample_seq): a
         * retransmitted CoAP request the sender made because it never saw
         * our earlier ACK -- dropped without re-storing or double-counting. */
        xSemaphoreGive(s_agg_mutex);
        return SENSOR_ROOT_AGGREGATOR_DUPLICATE;
    }

    rec->last_sample = *sample;
    rec->last_sample_seq = sample->sample_seq;
    rec->has_sample = true;
    rec->last_seen_us = esp_timer_get_time();
    xSemaphoreGive(s_agg_mutex);

    ESP_LOGI(TAG, "sample recorded: uuid=..%02x%02x seq=%u channels=%u", sample->module_uuid[6],
             sample->module_uuid[7], (unsigned)sample->sample_seq, sample->channel_count);
    return SENSOR_ROOT_AGGREGATOR_STORED;
}

void sensor_root_aggregator_record_sample(const sensor_sample_t *sample)
{
    (void)sensor_root_aggregator_record_sample_result(sample);
}

void sensor_root_aggregator_record_error(const uint8_t module_uuid[SENSOR_UUID_LEN])
{
    if (module_uuid == NULL || s_agg_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_agg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < SENSOR_ROOT_AGGREGATOR_MAX_NODES; i++) {
        if (s_nodes[i].valid && memcmp(s_nodes[i].module_uuid, module_uuid, SENSOR_UUID_LEN) == 0) {
            s_nodes[i].error_count++;
            break;
        }
    }
    xSemaphoreGive(s_agg_mutex);
}

size_t sensor_root_aggregator_get_node_count(void)
{
    size_t count = 0;
    if (s_agg_mutex == NULL || xSemaphoreTake(s_agg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    for (int i = 0; i < SENSOR_ROOT_AGGREGATOR_MAX_NODES; i++) {
        if (s_nodes[i].valid) {
            count++;
        }
    }
    xSemaphoreGive(s_agg_mutex);
    return count;
}

bool sensor_root_aggregator_get_node(size_t index, sensor_root_aggregator_node_t *out)
{
    if (out == NULL || index >= SENSOR_ROOT_AGGREGATOR_MAX_NODES || s_agg_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_agg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    bool valid = s_nodes[index].valid;
    if (valid) {
        *out = s_nodes[index];
    }
    xSemaphoreGive(s_agg_mutex);
    return valid;
}
