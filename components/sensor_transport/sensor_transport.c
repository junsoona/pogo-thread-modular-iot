#include "sensor_transport/sensor_transport.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "openthread/coap.h"
#include "openthread/error.h"

#include "sensor_service/sensor_service.h"
#include "thread_manager/thread_manager.h"
#include "node_calc/node_calc.h"

static const char *TAG = "sensor_transport";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)
#define SENSOR_TRANSPORT_URI_PATH "s"
/* 1 initial send + up to 3 retries (section 8/13: "최대 3회"). */
#define SENSOR_TRANSPORT_MAX_RETRIES 3
#define SENSOR_TRANSPORT_MAX_ATTEMPTS (1 + SENSOR_TRANSPORT_MAX_RETRIES)
#define SENSOR_TRANSPORT_ACK_TIMEOUT_MS 1000

typedef struct {
    SemaphoreHandle_t done_sem;
    otError result;
} send_wait_ctx_t;

static void coap_response_handler(void *ctx_v, otMessage *message, const otMessageInfo *message_info, otError result)
{
    (void)message;
    (void)message_info;
    send_wait_ctx_t *ctx = (send_wait_ctx_t *)ctx_v;
    ctx->result = result;
    xSemaphoreGive(ctx->done_sem);
}

/*
 * ROOT-side resource handler. Runs from within the OpenThread task's own
 * context (every otCoapAddResource callback does), so -- per
 * esp_openthread_lock.h and the exact same reasoning as thread_manager.c's
 * ot_state_changed_cb -- the API lock must NOT be taken again here.
 */
static void coap_sample_handler(void *ctx, otMessage *message, const otMessageInfo *message_info)
{
    (void)ctx;
    uint16_t len = otMessageGetLength(message) - otMessageGetOffset(message);
    bool ok = false;
    if (len > 0 && len <= SENSOR_CHANNEL_MAX_WIRE_LEN) {
        uint8_t buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
        uint16_t read = otMessageRead(message, otMessageGetOffset(message), buf, len);
        sensor_sample_t sample;
        if (read == len && sensor_channel_unpack(buf, len, &sample)) {
            sensor_service_root_record_sample(&sample);
            ok = true;
        } else {
            ESP_LOGW(TAG, "malformed sensor sample payload (len=%u) -- dropped", (unsigned)len);
        }
    } else {
        ESP_LOGW(TAG, "sensor sample payload length %u out of bounds -- dropped", (unsigned)len);
    }
    /* A malformed sample is still CoAP-ACKed (Confirmable delivery just
     * means "the message arrived", not "the payload was valid") -- the
     * sender otherwise retries forever against a peer that already has the
     * bytes; the malformed content itself is only visible in this log. */
    (void)ok;

    if (otCoapMessageGetType(message) == OT_COAP_TYPE_CONFIRMABLE) {
        otMessage *response = otCoapNewMessage(esp_openthread_get_instance(), NULL);
        if (response == NULL) {
            return;
        }
        if (otCoapMessageInitResponse(response, message, OT_COAP_TYPE_ACKNOWLEDGMENT, OT_COAP_CODE_CHANGED) !=
                OT_ERROR_NONE ||
            otCoapSendResponse(esp_openthread_get_instance(), response, message_info) != OT_ERROR_NONE) {
            otMessageFree(response);
        }
    }
}

static otCoapResource s_resource = {
    .mUriPath = SENSOR_TRANSPORT_URI_PATH,
    .mHandler = coap_sample_handler,
    .mContext = NULL,
    .mNext = NULL,
};

esp_err_t sensor_transport_init(void)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        ESP_LOGE(TAG, "failed to acquire OpenThread lock during init");
        return ESP_ERR_TIMEOUT;
    }
    otError err = otCoapStart(instance, OT_DEFAULT_COAP_PORT);
    if (err == OT_ERROR_NONE) {
        otCoapAddResource(instance, &s_resource);
    }
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otCoapStart failed: %s", otThreadErrorToString(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CoAP sensor transport ready: resource='/%s' port=%u", SENSOR_TRANSPORT_URI_PATH,
             (unsigned)OT_DEFAULT_COAP_PORT);
    return ESP_OK;
}

/* One Confirmable POST attempt to the current Leader/ROOT. Fails fast
 * (no blocking wait) if Thread is not currently joined -- see this
 * function's callers for why that matters (section 9: no busy-retry loop
 * while detached). */
static bool send_one_attempt(const uint8_t *payload, size_t len)
{
    otIp6Address leader_addr;
    if (thread_manager_get_leader_address(&leader_addr) != ESP_OK) {
        ESP_LOGD(TAG, "no Leader/ROOT address available (not joined?) -- skipping this attempt");
        return false;
    }

    send_wait_ctx_t ctx = {.done_sem = xSemaphoreCreateBinary()};
    if (ctx.done_sem == NULL) {
        return false;
    }

    otInstance *instance = esp_openthread_get_instance();
    otError err = OT_ERROR_NO_BUFS;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        otMessage *message = otCoapNewMessage(instance, NULL);
        if (message != NULL) {
            otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
            err = otCoapMessageAppendUriPathOptions(message, SENSOR_TRANSPORT_URI_PATH);
            if (err == OT_ERROR_NONE) {
                err = otCoapMessageSetPayloadMarker(message);
            }
            if (err == OT_ERROR_NONE) {
                err = otMessageAppend(message, payload, (uint16_t)len);
            }
            if (err == OT_ERROR_NONE) {
                otMessageInfo info = {0};
                info.mPeerAddr = leader_addr;
                info.mPeerPort = OT_DEFAULT_COAP_PORT;
                /* mMaxRetransmit=0: exactly one CoAP-layer transmission per
                 * attempt -- the application-level retry loop in
                 * sensor_transport_send_sample() owns the retry/backoff
                 * schedule (section 8), so OpenThread's own CoAP
                 * retransmission must not also be retrying underneath it. */
                otCoapTxParameters tx = {
                    .mAckTimeout = SENSOR_TRANSPORT_ACK_TIMEOUT_MS,
                    .mAckRandomFactorNumerator = 3,
                    .mAckRandomFactorDenominator = 2,
                    .mMaxRetransmit = 0,
                };
                err = otCoapSendRequestWithParameters(instance, message, &info, coap_response_handler, &ctx, &tx);
            }
            if (err != OT_ERROR_NONE) {
                otMessageFree(message);
            }
        }
        esp_openthread_lock_release();
    } else {
        err = OT_ERROR_BUSY;
    }

    bool ok = false;
    if (err == OT_ERROR_NONE) {
        /* Generous relative to the tx params' own ack timeout above so we
         * always hear back from OpenThread's response handler one way or
         * another before giving up on this attempt. */
        if (xSemaphoreTake(ctx.done_sem, pdMS_TO_TICKS(SENSOR_TRANSPORT_ACK_TIMEOUT_MS * 3)) == pdTRUE &&
            ctx.result == OT_ERROR_NONE) {
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, "failed to send CoAP request: %s", otThreadErrorToString(err));
    }
    vSemaphoreDelete(ctx.done_sem);
    return ok;
}

void sensor_transport_send_sample(const sensor_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    uint8_t payload[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t len = sensor_channel_pack(sample, payload, sizeof(payload));
    if (len == 0) {
        ESP_LOGE(TAG, "failed to pack sensor sample (channel_count=%u) -- dropped", sample->channel_count);
        return;
    }

    /* UUID-hash backoff schedule (section 8/13): first retry 0..255ms,
     * second 0..511ms, third 0..1023ms -- all derived from the SAME
     * per-module hash (widening which bits are kept), never a second random
     * source, and always resending the SAME sample_seq's already-packed
     * bytes (section 8: "동일 sample sequence 재전송"). Index 0 (the
     * initial send, not a retry) is unused. */
    static const uint32_t retry_masks[SENSOR_TRANSPORT_MAX_ATTEMPTS] = {0, 0xFF, 0x1FF, 0x3FF};
    uint32_t hash = node_calc_uuid_hash(sample->module_uuid);

    for (int attempt = 0; attempt < SENSOR_TRANSPORT_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            uint32_t backoff_ms = hash & retry_masks[attempt];
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }
        if (send_one_attempt(payload, len)) {
            ESP_LOGD(TAG, "sample sent OK (uuid=..%02x%02x seq=%u attempt=%d/%d)", sample->module_uuid[6],
                      sample->module_uuid[7], (unsigned)sample->sample_seq, attempt + 1, SENSOR_TRANSPORT_MAX_ATTEMPTS);
            return; /* success -- no retry state to reset, each call starts fresh */
        }
    }
    ESP_LOGW(TAG, "sample dropped after %d attempts (uuid=..%02x%02x seq=%u)", SENSOR_TRANSPORT_MAX_ATTEMPTS,
             sample->module_uuid[6], sample->module_uuid[7], (unsigned)sample->sample_seq);
}
