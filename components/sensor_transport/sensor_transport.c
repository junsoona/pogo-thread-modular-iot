#include "sensor_transport/sensor_transport.h"

#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "openthread/coap.h"
#include "openthread/error.h"

#include "sensor_service/sensor_service.h"
#include "collector_endpoint/collector_endpoint.h"
#include "node_calc/node_calc.h"
#include "topology_gate/topology_gate.h"
#include "coap_response_timing/coap_response_timing.h"

static const char *TAG = "sensor_transport";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)
#define SENSOR_TRANSPORT_URI_PATH "s"
/* 1 initial send + up to 3 retries (section 8/13: "최대 3회"). */
#define SENSOR_TRANSPORT_MAX_RETRIES 3
#define SENSOR_TRANSPORT_MAX_ATTEMPTS (1 + SENSOR_TRANSPORT_MAX_RETRIES)
/* Shared with topology_reporter.c -- ONE Kconfig source of truth (this
 * follow-up: SED CoAP response timing correction, see
 * components/coap_response_timing's own header comment for the
 * real-hardware finding behind this and why it must stay comfortably
 * LONGER than a LEAF's own Thread data-poll period). */
#define SENSOR_TRANSPORT_ACK_TIMEOUT_MS CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS

/* true only for the AUTO path against an active power base -- see this
 * component's header comment and sensor_transport_init(). */
static bool s_require_topology_gate;

/*
 * CoAP client send state -- code review follow-up section 3 (the same fix
 * applied to topology_reporter.c, which sends to a sibling resource on the
 * same power base): an earlier revision created/deleted a
 * SemaphoreHandle_t per attempt on the caller's own stack, so a callback
 * firing late (after the waiter already timed out, deleted the semaphore,
 * and returned) touched freed/stack-unwound memory. This component now
 * creates ONE semaphore for its whole lifetime (never deleted) and versions
 * each attempt with a monotonically-increasing token instead:
 * coap_response_handler() only acts (records the result, gives the
 * semaphore) if the token it captured at send time still matches the
 * CURRENT token -- a late callback for an attempt the waiter already gave
 * up on simply finds a mismatch and does nothing. s_send_lock is a short
 * critical section around the token compare-and-write on both sides (the
 * OpenThread task runs the callback; the sensor_service periodic task runs
 * the waiter), so the two can never race on a half-updated token/result.
 */
static SemaphoreHandle_t s_send_done_sem;
static portMUX_TYPE s_send_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_send_token;
static otError s_send_result;
static otCoapCode s_send_response_code;
static bool s_send_got_message;

static void coap_response_handler(void *ctx_v, otMessage *message, const otMessageInfo *message_info, otError result)
{
    (void)message_info;
    uint32_t my_token = (uint32_t)(uintptr_t)ctx_v;
    bool stale;

    taskENTER_CRITICAL(&s_send_lock);
    stale = (my_token != s_send_token);
    if (!stale) {
        s_send_result = result;
        s_send_got_message = false;
        if (result == OT_ERROR_NONE && message != NULL) {
            s_send_response_code = otCoapMessageGetCode(message);
            s_send_got_message = true;
        }
    }
    taskEXIT_CRITICAL(&s_send_lock);

    if (!stale) {
        xSemaphoreGive(s_send_done_sem);
    }
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

esp_err_t sensor_transport_init(bool require_topology_registration)
{
    s_require_topology_gate = require_topology_registration;
    s_send_done_sem = xSemaphoreCreateBinary();
    if (s_send_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

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
    collector_endpoint_snapshot_t collector;
    if (!collector_endpoint_get(&collector) || !collector.valid) {
        ESP_LOGD(TAG, "no collector endpoint available yet -- skipping this attempt");
        return false;
    }
    otIp6Address dest_addr;
    memcpy(dest_addr.mFields.m8, collector.addr, sizeof(dest_addr.mFields.m8));

    uint32_t my_token;
    taskENTER_CRITICAL(&s_send_lock);
    my_token = ++s_send_token;
    s_send_got_message = false;
    taskEXIT_CRITICAL(&s_send_lock);
    /* Defensive: drain any stale "give" left over from a truly-ancient
     * attempt -- see coap_response_handler()'s doc comment. */
    xSemaphoreTake(s_send_done_sem, 0);

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
                info.mPeerAddr = dest_addr;
                info.mPeerPort = collector.port;
                /* mMaxRetransmit=0: exactly one CoAP-layer transmission per
                 * attempt -- the application-level retry loop in
                 * sensor_transport_send_sample() owns the retry/backoff
                 * schedule (section 8), so OpenThread's own CoAP
                 * retransmission must not also be retrying underneath it. */
                otCoapTxParameters tx = {
                    .mAckTimeout = SENSOR_TRANSPORT_ACK_TIMEOUT_MS,
                    .mAckRandomFactorNumerator = COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                    .mAckRandomFactorDenominator = COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN,
                    .mMaxRetransmit = 0,
                };
                err = otCoapSendRequestWithParameters(instance, message, &info, coap_response_handler,
                                                       (void *)(uintptr_t)my_token, &tx);
            }
            if (err != OT_ERROR_NONE) {
                otMessageFree(message);
            }
        }
        esp_openthread_lock_release();
    } else {
        err = OT_ERROR_BUSY;
    }

    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "failed to send CoAP request: %s", otThreadErrorToString(err));
        return false;
    }

    bool ok = false;
    /* Correctly derived (never a bare "ack_timeout * N" guess) from the SAME
     * tx params just above via components/coap_response_timing's shared
     * helper -- covers OpenThread's own worst-case ACK wait (ack_timeout *
     * random_factor) plus a margin, so this wait can never expire strictly
     * before OpenThread's response handler could still validly fire for
     * this attempt. See that component's header comment for the
     * real-hardware finding this replaced (a LEAF's own Thread data-poll
     * period used to race this timeout). */
    uint32_t wait_ms = coap_response_timing_wait_ms(SENSOR_TRANSPORT_ACK_TIMEOUT_MS,
                                                      COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                                      COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN,
                                                      SENSOR_TRANSPORT_ACK_TIMEOUT_MS);
    if (xSemaphoreTake(s_send_done_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        if (s_send_result == OT_ERROR_NONE && s_send_got_message) {
            /* code review follow-up section 3: a successful OpenThread
             * transport is not enough -- the power base's own CoAP response
             * code (now varied, see power_base_transport.c) must also be a
             * 2.xx success. */
            uint8_t code_class = (uint8_t)(s_send_response_code >> 5);
            if (code_class == 2) {
                ok = true;
            } else {
                ESP_LOGW(TAG, "sensor sample rejected by receiver: CoAP %u.%02u", code_class,
                         (unsigned)(s_send_response_code & 0x1F));
                /*
                 * Power base reboot recovery (docs/power-base.md section 11):
                 * its topology_registry is RAM-only, so a reboot makes it
                 * reject an already-registered module's sample as CoAP 4.04
                 * Not Found ("unknown module" -- power_base_transport.c's
                 * coap_sensor_handler() is the ONLY thing that ever produces
                 * this exact code for "/s", never any other meaning). Never
                 * treated as success (unchanged -- code_class != 2 above
                 * already handles that); the new part is reacting to it:
                 * close the gate right away and ask topology_reporter to
                 * re-attach on its very next poll tick instead of silently
                 * waiting for its normal heartbeat schedule (section 8:
                 * "30초 heartbeat만 기다리지 않는다"). A no-op when this
                 * module never uses the gate at all (DEV_ROOT/DEV_LEAF,
                 * whose peer never sends this code for "/s" in the first
                 * place -- see sensor_transport_init()).
                 */
                if (s_require_topology_gate && s_send_response_code == OT_COAP_CODE_NOT_FOUND) {
                    topology_gate_close();
                    topology_gate_request_reregister();
                }
                /*
                 * Parent-offline recovery (real-hardware finding: a Power
                 * Base DOWNSTREAM POGO detach -- or an intermediate node's
                 * own downstream loss -- cascades this module's registry
                 * entry offline; see topology_registry.c's new
                 * TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE). power_base_transport's
                 * "/s" handler returns this SAME CoAP 4.12 Precondition
                 * Failed for a known-but-offline module (and, unchanged from
                 * before, for a bitmap-mismatched sample) -- there is no
                 * wire-level way to tell those apart, and none is added
                 * here (item 8: wire format unchanged). Reacting the same
                 * way as the 4.04 case above -- close the gate immediately
                 * and ask topology_reporter to re-attach on its very next
                 * poll tick -- is safe either way: a genuine parent-offline
                 * condition needs exactly this to recover once the parent
                 * comes back, and a (rare, config-time) bitmap mismatch just
                 * re-sends the same idempotent ATTACHED report, bounded by
                 * this module's own sensor-sampling period -- never a tight
                 * busy loop (section 9 of the follow-up spec).
                 */
                if (s_require_topology_gate && s_send_response_code == OT_COAP_CODE_PRECONDITION_FAILED) {
                    topology_gate_close();
                    topology_gate_request_reregister();
                }
            }
        }
    }
    return ok;
}

void sensor_transport_send_sample(const sensor_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    /*
     * Topology-registration gate (real-hardware first-sample/topology race
     * -- see components/topology_gate's header comment and this
     * component's own header comment): while required (AUTO path, an
     * active power base) and not yet open, no wireless attempt is made at
     * all -- not even the first CoAP try. sensor_service's periodic task
     * still measured this sample and advanced sample_seq/local state
     * exactly as before (see sensor_service.c's periodic_task()); only the
     * "/s" hand-off is skipped here. ESP_LOGD, not a warning: this is the
     * expected, routine state for every sample in the (usually short)
     * window between Thread join and this module's own ATTACHED report
     * being confirmed, and must not flood the log once per sample period
     * (section 4 of the follow-up spec: "매 주기마다 경고를 출력하지
     * 않는다").
     */
    if (s_require_topology_gate && !topology_gate_is_open()) {
        ESP_LOGD(TAG, "topology registration not yet confirmed -- sample uuid=..%02x%02x seq=%u held back",
                 sample->module_uuid[6], sample->module_uuid[7], (unsigned)sample->sample_seq);
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
        /*
         * Re-check the gate right before EVERY attempt, not just once
         * before this whole loop started: send_one_attempt() itself can
         * close the gate mid-retry (a 4.04/4.12 rejection -- see its own
         * "Power base reboot recovery" / "Parent-offline recovery" comments
         * below), and an unrelated event (topology_reporter noticing
         * upstream/Thread/role loss) can close it asynchronously during
         * this loop's own backoff sleep. Either way, once the gate is
         * closed no further "/s" attempt for this sample is made -- see
         * this function's header comment and topology_gate.h's own
         * "a sensor sample must never be sent wirelessly while this is
         * false" invariant.
         */
        if (s_require_topology_gate && !topology_gate_is_open()) {
            ESP_LOGD(TAG, "topology gate closed before attempt %d/%d -- aborting remaining retries for this "
                          "sample (uuid=..%02x%02x seq=%u)",
                     attempt + 1, SENSOR_TRANSPORT_MAX_ATTEMPTS, sample->module_uuid[6], sample->module_uuid[7],
                     (unsigned)sample->sample_seq);
            return;
        }
        if (send_one_attempt(payload, len)) {
            ESP_LOGD(TAG, "sample sent OK (uuid=..%02x%02x seq=%u attempt=%d/%d)", sample->module_uuid[6],
                      sample->module_uuid[7], (unsigned)sample->sample_seq, attempt + 1, SENSOR_TRANSPORT_MAX_ATTEMPTS);
            return; /* success -- no retry state to reset, each call starts fresh */
        }
        /*
         * This very attempt's own rejection may be exactly what just closed
         * the gate (send_one_attempt()'s 4.04/4.12 handling). Abort the
         * remaining retries for THIS sample immediately rather than paying
         * out this iteration's backoff delay only to have the check above
         * catch it on the next loop pass -- this is the expected recovery
         * path (topology_reporter is about to re-attach), not a genuinely
         * unconfirmed delivery, so it is logged at DEBUG, never the
         * "delivery unconfirmed after N attempts" WARN below.
         */
        if (s_require_topology_gate && !topology_gate_is_open()) {
            ESP_LOGD(TAG, "topology gate closed by this attempt's rejection -- aborting remaining retries for "
                          "this sample (uuid=..%02x%02x seq=%u, %d/%d attempts made)",
                     sample->module_uuid[6], sample->module_uuid[7], (unsigned)sample->sample_seq, attempt + 1,
                     SENSOR_TRANSPORT_MAX_ATTEMPTS);
            return;
        }
    }
    /*
     * NOT a claim that the sample was actually lost -- only that THIS
     * SENDER never received a confirmed 2.xx response within
     * SENSOR_TRANSPORT_MAX_ATTEMPTS attempts. The power base may have
     * already stored it (a real-hardware finding: an unconfirmed response
     * can arrive after this sender already gave up, most commonly because
     * of a slow/lost CoAP ACK rather than the request itself ever failing
     * to arrive) -- see components/coap_response_timing's header comment.
     * Whether it was actually stored is exactly what sample_seq dedup on
     * the power base's own side (sensor_root_aggregator) resolves; nothing
     * here can determine that on its own.
     */
    ESP_LOGW(TAG, "sample delivery unconfirmed after %d attempts (uuid=..%02x%02x seq=%u)",
             SENSOR_TRANSPORT_MAX_ATTEMPTS, sample->module_uuid[6], sample->module_uuid[7],
             (unsigned)sample->sample_seq);
}
