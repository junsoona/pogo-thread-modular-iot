#include "power_base_transport/power_base_transport.h"

#include <string.h>
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "openthread/coap.h"
#include "openthread/error.h"

#include "sensor_channel/sensor_channel.h"
#include "sensor_root_aggregator/sensor_root_aggregator.h"

static const char *TAG = "power_base_transport";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)
#define SENSOR_URI_PATH "s"
#define TOPOLOGY_URI_PATH "t"
#define REGISTRY_MUTEX_WAIT pdMS_TO_TICKS(200)

static SemaphoreHandle_t s_registry_mutex;
static topology_registry_t s_registry;
static uint8_t s_power_base_uuid[TOPOLOGY_UUID_LEN];

static void send_coap_response(otMessage *request, const otMessageInfo *message_info, otCoapCode code)
{
    if (otCoapMessageGetType(request) != OT_COAP_TYPE_CONFIRMABLE) {
        return; /* Non-confirmable: no response expected or sent, regardless of outcome. */
    }
    otMessage *response = otCoapNewMessage(esp_openthread_get_instance(), NULL);
    if (response == NULL) {
        return;
    }
    if (otCoapMessageInitResponse(response, request, OT_COAP_TYPE_ACKNOWLEDGMENT, code) != OT_ERROR_NONE ||
        otCoapSendResponse(esp_openthread_get_instance(), response, message_info) != OT_ERROR_NONE) {
        otMessageFree(response);
    }
}

/* Runs from within the OpenThread task's own context, like every
 * otCoapAddResource callback -- the API lock must NOT be taken again here
 * (see thread_manager.c's ot_state_changed_cb / sensor_transport.c's
 * coap_sample_handler for the same reasoning). */
static void coap_sensor_handler(void *ctx, otMessage *message, const otMessageInfo *message_info)
{
    (void)ctx;
    uint16_t len = otMessageGetLength(message) - otMessageGetOffset(message);
    otCoapCode code;

    if (len > 0 && len <= SENSOR_CHANNEL_MAX_WIRE_LEN) {
        uint8_t buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
        uint16_t read = otMessageRead(message, otMessageGetOffset(message), buf, len);
        sensor_sample_t sample;
        if (read == len && sensor_channel_unpack(buf, len, &sample)) {
            bool known = false;
            bool online = false;
            uint16_t node_bitmap = 0;
            if (xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) == pdTRUE) {
                const topology_registry_entry_t *entry = topology_registry_find(&s_registry, sample.module_uuid);
                if (entry != NULL) {
                    known = true;
                    online = entry->online;
                    node_bitmap = entry->sensor_bitmap;
                }
                xSemaphoreGive(s_registry_mutex);
            } else {
                ESP_LOGW(TAG, "sensor sample: could not acquire registry mutex in time -- treating as unavailable");
                send_coap_response(message, message_info, OT_COAP_CODE_SERVICE_UNAVAILABLE);
                return;
            }

            /* section 10 (updated by the code review follow-up): a known
             * uuid's data is only stored once it is ALSO online, AND its
             * declared sensor_type_bitmap is a subset of (or equal to) the
             * bitmap that uuid's own topology reports have declared -- a
             * bitmap claiming a channel that module was never configured
             * with is rejected, not silently trusted. */
            if (!known) {
                ESP_LOGW(TAG, "sensor sample from unknown module uuid=..%02x%02x -- dropped, not stored",
                         sample.module_uuid[6], sample.module_uuid[7]);
                code = OT_COAP_CODE_NOT_FOUND;
            } else if (!online) {
                ESP_LOGW(TAG, "sensor sample from OFFLINE module uuid=..%02x%02x -- dropped, not stored",
                         sample.module_uuid[6], sample.module_uuid[7]);
                if (xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) == pdTRUE) {
                    topology_registry_record_sensor_error(&s_registry, sample.module_uuid);
                    xSemaphoreGive(s_registry_mutex);
                }
                code = OT_COAP_CODE_PRECONDITION_FAILED;
            } else if ((sample.sensor_bitmap & (uint16_t)~node_bitmap) != 0) {
                ESP_LOGW(TAG, "sensor sample bitmap 0x%04x is not a subset of the reported bitmap 0x%04x for "
                              "uuid=..%02x%02x -- dropped, not stored",
                         sample.sensor_bitmap, node_bitmap, sample.module_uuid[6], sample.module_uuid[7]);
                if (xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) == pdTRUE) {
                    topology_registry_record_sensor_error(&s_registry, sample.module_uuid);
                    xSemaphoreGive(s_registry_mutex);
                }
                code = OT_COAP_CODE_PRECONDITION_FAILED;
            } else {
                sensor_root_aggregator_record_sample(&sample);
                code = OT_COAP_CODE_CHANGED;
            }
        } else {
            ESP_LOGW(TAG, "malformed sensor sample payload (len=%u) -- dropped", (unsigned)len);
            code = OT_COAP_CODE_BAD_REQUEST;
        }
    } else {
        ESP_LOGW(TAG, "sensor sample payload length %u out of bounds -- dropped", (unsigned)len);
        code = OT_COAP_CODE_BAD_REQUEST;
    }
    send_coap_response(message, message_info, code);
}

static const char *registry_result_str(topology_registry_result_t r)
{
    switch (r) {
    case TOPOLOGY_REGISTRY_OK:
        return "OK";
    case TOPOLOGY_REGISTRY_OK_DUPLICATE:
        return "OK_DUPLICATE";
    case TOPOLOGY_REGISTRY_ERR_SELF_PARENT:
        return "SELF_PARENT";
    case TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH:
        return "INVALID_DEPTH_OR_ROLE";
    case TOPOLOGY_REGISTRY_ERR_DEPTH1_BAD_PARENT:
        return "DEPTH1_BAD_PARENT";
    case TOPOLOGY_REGISTRY_ERR_UNKNOWN_PARENT:
        return "UNKNOWN_PARENT";
    case TOPOLOGY_REGISTRY_ERR_PARENT_DEPTH_MISMATCH:
        return "PARENT_DEPTH_MISMATCH";
    case TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE:
        return "PARENT_OFFLINE";
    case TOPOLOGY_REGISTRY_ERR_CYCLE:
        return "CYCLE";
    case TOPOLOGY_REGISTRY_ERR_STALE_SESSION:
        return "STALE_SESSION";
    case TOPOLOGY_REGISTRY_ERR_DUPLICATE_SEQ:
        return "DUPLICATE_OR_REORDERED_SEQ";
    case TOPOLOGY_REGISTRY_ERR_FULL:
        return "REGISTRY_FULL";
    default:
        return "UNKNOWN";
    }
}

/*
 * Maps a registry outcome to a CoAP response code -- see
 * power_base_transport.h's top comment for the full table. OpenThread's
 * CoAP implementation only defines the base RFC 7252 code set (no distinct
 * "Conflict"), so every well-formed-but-rejected registry result folds into
 * 4.12 Precondition Failed.
 */
static otCoapCode topology_result_to_coap_code(topology_registry_result_t r)
{
    switch (r) {
    case TOPOLOGY_REGISTRY_OK:
        return OT_COAP_CODE_CHANGED;
    case TOPOLOGY_REGISTRY_OK_DUPLICATE:
        return OT_COAP_CODE_VALID;
    case TOPOLOGY_REGISTRY_ERR_FULL:
        return OT_COAP_CODE_SERVICE_UNAVAILABLE;
    case TOPOLOGY_REGISTRY_ERR_SELF_PARENT:
    case TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH:
    case TOPOLOGY_REGISTRY_ERR_DEPTH1_BAD_PARENT:
    case TOPOLOGY_REGISTRY_ERR_UNKNOWN_PARENT:
    case TOPOLOGY_REGISTRY_ERR_PARENT_DEPTH_MISMATCH:
    case TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE:
    case TOPOLOGY_REGISTRY_ERR_CYCLE:
    case TOPOLOGY_REGISTRY_ERR_STALE_SESSION:
    case TOPOLOGY_REGISTRY_ERR_DUPLICATE_SEQ:
    default:
        return OT_COAP_CODE_PRECONDITION_FAILED;
    }
}

static void coap_topology_handler(void *ctx, otMessage *message, const otMessageInfo *message_info)
{
    (void)ctx;
    uint16_t len = otMessageGetLength(message) - otMessageGetOffset(message);
    otCoapCode code;

    if (len == TOPOLOGY_REPORT_WIRE_LEN) {
        uint8_t buf[TOPOLOGY_REPORT_WIRE_LEN];
        uint16_t read = otMessageRead(message, otMessageGetOffset(message), buf, len);
        topology_report_t report;
        if (read == len && topology_report_unpack(buf, len, &report)) {
            uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) == pdTRUE) {
                topology_registry_result_t result =
                    topology_registry_apply_report(&s_registry, &report, s_power_base_uuid, now_ms);
                xSemaphoreGive(s_registry_mutex);
                code = topology_result_to_coap_code(result);
                if (!topology_registry_result_is_ok(result)) {
                    ESP_LOGW(TAG, "topology report rejected: uuid=..%02x%02x depth=%u seq=%u reason=%s",
                             report.module_uuid[6], report.module_uuid[7], report.topology_depth,
                             (unsigned)report.sequence, registry_result_str(result));
                } else if (result == TOPOLOGY_REGISTRY_OK_DUPLICATE) {
                    ESP_LOGD(TAG, "topology report duplicate (idempotent): uuid=..%02x%02x seq=%u",
                             report.module_uuid[6], report.module_uuid[7], (unsigned)report.sequence);
                } else {
                    /* ATTACHED/UPDATED/DETACHED are real, operator-visible
                     * events -- INFO. A routine HEARTBEAT stays at DEBUG so
                     * a healthy, unchanging chain does not flood the log
                     * (code review follow-up section 7). */
                    esp_log_level_t lvl =
                        (report.report_type == TOPOLOGY_REPORT_HEARTBEAT) ? ESP_LOG_DEBUG : ESP_LOG_INFO;
                    ESP_LOG_LEVEL_LOCAL(lvl, TAG, "topology report applied: uuid=..%02x%02x depth=%u role=%u type=%u",
                                         report.module_uuid[6], report.module_uuid[7], report.topology_depth,
                                         report.role, report.report_type);
                }
            } else {
                ESP_LOGW(TAG, "topology report: could not acquire registry mutex in time -- treating as unavailable");
                code = OT_COAP_CODE_SERVICE_UNAVAILABLE;
            }
        } else {
            ESP_LOGW(TAG, "malformed topology report payload (len=%u) -- dropped", (unsigned)len);
            code = OT_COAP_CODE_BAD_REQUEST;
        }
    } else {
        ESP_LOGW(TAG, "topology report payload length %u != expected %u -- dropped", (unsigned)len,
                 TOPOLOGY_REPORT_WIRE_LEN);
        code = OT_COAP_CODE_BAD_REQUEST;
    }
    send_coap_response(message, message_info, code);
}

static otCoapResource s_sensor_resource = {
    .mUriPath = SENSOR_URI_PATH,
    .mHandler = coap_sensor_handler,
    .mContext = NULL,
    .mNext = NULL,
};

static otCoapResource s_topology_resource = {
    .mUriPath = TOPOLOGY_URI_PATH,
    .mHandler = coap_topology_handler,
    .mContext = NULL,
    .mNext = NULL,
};

esp_err_t power_base_transport_init(const uint8_t power_base_uuid[TOPOLOGY_UUID_LEN])
{
    if (power_base_uuid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_power_base_uuid, power_base_uuid, TOPOLOGY_UUID_LEN);
    topology_registry_init(&s_registry);

    s_registry_mutex = xSemaphoreCreateMutex();
    if (s_registry_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t agg_err = sensor_root_aggregator_init();
    if (agg_err != ESP_OK) {
        return agg_err;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        ESP_LOGE(TAG, "failed to acquire OpenThread lock during init");
        return ESP_ERR_TIMEOUT;
    }
    otError err = otCoapStart(instance, OT_DEFAULT_COAP_PORT);
    if (err == OT_ERROR_NONE) {
        otCoapAddResource(instance, &s_sensor_resource);
        otCoapAddResource(instance, &s_topology_resource);
    }
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otCoapStart failed: %s", otThreadErrorToString(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CoAP power-base transport ready: resources='/%s','/%s' port=%u", SENSOR_URI_PATH,
             TOPOLOGY_URI_PATH, (unsigned)OT_DEFAULT_COAP_PORT);
    return ESP_OK;
}

void power_base_transport_on_direct_downstream_detached(void)
{
    if (s_registry_mutex == NULL || xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) != pdTRUE) {
        return;
    }
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        topology_registry_entry_t e;
        if (topology_registry_get(&s_registry, i, &e) && e.depth == 1 && e.online) {
            topology_registry_mark_offline(&s_registry, e.module_uuid);
        }
    }
    xSemaphoreGive(s_registry_mutex);
    ESP_LOGW(TAG, "direct DOWNSTREAM POGO detach -- depth=1 node(s) and their whole subtree marked offline");
}

void power_base_transport_sweep_stale(uint32_t now_ms, uint32_t stale_timeout_ms)
{
    if (s_registry_mutex == NULL || xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) != pdTRUE) {
        return;
    }
    topology_registry_sweep_stale(&s_registry, now_ms, stale_timeout_ms);
    xSemaphoreGive(s_registry_mutex);
}

size_t power_base_transport_get_node_count(void)
{
    size_t count = 0;
    if (s_registry_mutex == NULL || xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) != pdTRUE) {
        return 0;
    }
    count = topology_registry_count(&s_registry);
    xSemaphoreGive(s_registry_mutex);
    return count;
}

bool power_base_transport_get_node(size_t index, topology_registry_entry_t *out)
{
    bool ok = false;
    if (s_registry_mutex == NULL || out == NULL || xSemaphoreTake(s_registry_mutex, REGISTRY_MUTEX_WAIT) != pdTRUE) {
        return false;
    }
    ok = topology_registry_get(&s_registry, index, out);
    xSemaphoreGive(s_registry_mutex);
    return ok;
}
