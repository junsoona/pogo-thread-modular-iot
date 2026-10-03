#include "topology_reporter/topology_reporter.h"

#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "openthread/coap.h"
#include "openthread/error.h"

#include "topology_proto/topology_proto.h"
#include "role_manager/role_manager.h"
#include "child_service/child_service.h"
#include "sensor_service/sensor_service.h"
#include "collector_endpoint/collector_endpoint.h"
#include "node_calc/node_calc.h"
#include "topology_gate/topology_gate.h"
#include "coap_response_timing/coap_response_timing.h"

static const char *TAG = "topology_reporter";

#define TOPOLOGY_REPORT_URI_PATH "t"
#define TOPOLOGY_REPORTER_MAX_RETRIES 3
#define TOPOLOGY_REPORTER_MAX_ATTEMPTS (1 + TOPOLOGY_REPORTER_MAX_RETRIES)
/* Shared with sensor_transport.c -- ONE Kconfig source of truth (this
 * follow-up: SED CoAP response timing correction, see
 * components/coap_response_timing's own header comment for the
 * real-hardware finding behind this and why it must stay comfortably
 * LONGER than a LEAF's own Thread data-poll period). */
#define TOPOLOGY_REPORTER_ACK_TIMEOUT_MS CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS
#define TOPOLOGY_REPORTER_POLL_TICK_MS 500
#define TOPOLOGY_REPORTER_NVS_NAMESPACE "topo_rep"
#define TOPOLOGY_REPORTER_NVS_KEY "boot_gen"
/* Once the local sequence counter gets this close to wrapping, roll over to
 * a fresh persisted generation instead (code review follow-up section 2:
 * "sequence wrap 또는 명시적인 한계 처리") -- chosen with a large safety
 * margin, never actually reachable in this component's real lifetime
 * (heartbeats every tens of seconds), but exercised directly by
 * tools/test_topology_reporter_session.c's mirror model. */
#define TOPOLOGY_REPORTER_SEQUENCE_ROLLOVER_THRESHOLD (UINT32_MAX - 4)

typedef struct {
    pogo_port_id_t port_id;
    pogo_port_event_t event;
} tr_port_evt_t;

static QueueHandle_t s_evt_queue;
static module_config_t s_cfg;
static uint32_t s_generation;
static uint32_t s_sequence;
static volatile bool s_downstream_connected;
static bool s_reporting_enabled;

/*
 * CoAP client send state -- code review follow-up section 3: an earlier
 * revision created/deleted a SemaphoreHandle_t per attempt on the caller's
 * own stack, so a callback that fired late (after the waiter already timed
 * out, deleted the semaphore, and returned) touched freed/stack-unwound
 * memory. This component now creates ONE semaphore for its whole lifetime
 * (never deleted) and versions each attempt with a monotonically-increasing
 * token instead: coap_response_handler() only acts (records the result,
 * gives the semaphore) if the token it captured at send time still matches
 * the CURRENT token -- a late callback for an attempt the waiter already
 * gave up on simply finds a mismatch and does nothing. s_send_lock is a
 * short critical section around the token compare-and-write on both sides
 * (the OpenThread task runs the callback; this component's own task runs
 * the waiter), so the two can never race on a half-updated token/result.
 */
static SemaphoreHandle_t s_send_done_sem;
static portMUX_TYPE s_send_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_send_token;
static otError s_send_result;
static otCoapCode s_send_response_code;
static bool s_send_got_message;

/* Snapshot of the last successfully-BUILT (not necessarily delivered)
 * non-DETACHED report's identity fields, so a best-effort DETACHED report
 * can still name a real module/parent uuid, depth and role even after
 * child_service has already cleared its own session state (which can race
 * ahead of this task's next poll tick -- see this component's header
 * comment). */
static uint8_t s_last_module_uuid[TOPOLOGY_UUID_LEN];
static uint8_t s_last_parent_uuid[TOPOLOGY_UUID_LEN];
static uint8_t s_last_depth;
static uint8_t s_last_role;
static uint16_t s_last_sensor_bitmap;

/*
 * Reads the persisted boot counter (0 if the namespace/key has never been
 * written), increments it, and durably commits the NEW value BEFORE
 * returning it -- so two consecutive boots can never end up claiming the
 * same generation number as long as this call itself succeeds (code review
 * follow-up section 2: NVS monotonic boot counter). Every NVS call's result
 * is checked; any failure (not just "commit") is reported to the caller,
 * which must NOT proceed as though a fresh, safely-persisted generation was
 * obtained (see topology_reporter_init()'s own handling of a non-ESP_OK
 * return here).
 */
static esp_err_t load_next_boot_generation(uint32_t *out_gen)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(TOPOLOGY_REPORTER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load_next_boot_generation: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t current = 0;
    err = nvs_get_u32(h, TOPOLOGY_REPORTER_NVS_KEY, &current);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "load_next_boot_generation: nvs_get_u32 failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }
    /* ESP_ERR_NVS_NOT_FOUND: first boot ever -- current stays 0, and the
     * very first generation used will be 1 (never 0), so 0 is always free
     * to mean "no boot generation observed yet" if a reader ever needs
     * that. */

    uint32_t next;
    if (current >= UINT32_MAX - 1) {
        /* Practically unreachable (4+ billion boots), but never silently
         * wrap back to a small value that a long-past boot already used --
         * see topology_registry.h's own "opaque token" design for why a
         * saturated (repeated) generation here is still safe, just no
         * longer distinguishing every individual boot from every other. */
        ESP_LOGE(TAG, "load_next_boot_generation: boot counter near exhaustion (%u) -- saturating, no longer "
                      "unique per boot",
                 (unsigned)current);
        next = UINT32_MAX;
    } else {
        next = current + 1;
    }

    err = nvs_set_u32(h, TOPOLOGY_REPORTER_NVS_KEY, next);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load_next_boot_generation: failed to persist generation %u: %s", (unsigned)next,
                 esp_err_to_name(err));
        return err;
    }

    *out_gen = next;
    return ESP_OK;
}

/*
 * Called before a sequence increment would ever reach
 * TOPOLOGY_REPORTER_SEQUENCE_ROLLOVER_THRESHOLD: obtains a FRESH persisted
 * generation and resets the sequence counter, so the local sequence space
 * never actually wraps (section 2: "sequence wrap ... 명시적인 한계
 * 처리"). If persistence fails at this point, reporting is disabled for the
 * rest of this boot -- continuing with the old, about-to-wrap generation
 * would risk exactly the ambiguity this component exists to avoid.
 */
static void maybe_roll_over_sequence(void)
{
    if (s_sequence < TOPOLOGY_REPORTER_SEQUENCE_ROLLOVER_THRESHOLD) {
        return;
    }
    uint32_t fresh_gen;
    esp_err_t err = load_next_boot_generation(&fresh_gen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sequence rollover: failed to persist a fresh generation (%s) -- disabling topology "
                      "reporting for the rest of this boot",
                 esp_err_to_name(err));
        s_reporting_enabled = false;
        return;
    }
    ESP_LOGW(TAG, "local sequence counter approached its limit -- rolled over to a fresh boot_session_generation=%u",
             (unsigned)fresh_gen);
    s_generation = fresh_gen;
    s_sequence = 0;
}

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
        /* Only the waiter that is still actually waiting on THIS token ever
         * gets the semaphore given to it -- a late callback for an attempt
         * the caller already gave up on (a new token is now current) never
         * touches the semaphore at all. */
        xSemaphoreGive(s_send_done_sem);
    }
}

/* One Confirmable POST attempt to the current Leader (the power base is
 * sent to the collector endpoint, same reasoning as sensor_transport.c).
 * Fails fast if no collector endpoint is currently valid. Success requires
 * BOTH the OpenThread transport to report OT_ERROR_NONE AND the power
 * base's actual CoAP response code to be 2.xx (code review follow-up
 * section 3) -- a 4.xx/5.xx response means the report was rejected, not
 * merely "arrived". */
static bool send_one_attempt(const uint8_t *payload, size_t len)
{
    collector_endpoint_snapshot_t collector;
    if (!collector_endpoint_get(&collector) || !collector.valid) {
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
     * attempt whose late callback still matched some earlier token before
     * this one was assigned -- keeps the semaphore's count at a known 0
     * before this attempt's own wait begins. */
    xSemaphoreTake(s_send_done_sem, 0);

    otInstance *instance = esp_openthread_get_instance();
    otError err = OT_ERROR_NO_BUFS;
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        otMessage *message = otCoapNewMessage(instance, NULL);
        if (message != NULL) {
            otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
            err = otCoapMessageAppendUriPathOptions(message, TOPOLOGY_REPORT_URI_PATH);
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
                /* mMaxRetransmit=0: this component's own retry/backoff loop
                 * (below) owns retries, same reasoning as sensor_transport.c. */
                otCoapTxParameters tx = {
                    .mAckTimeout = TOPOLOGY_REPORTER_ACK_TIMEOUT_MS,
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
        /* The request was never actually submitted to OpenThread, so
         * coap_response_handler() will never fire for my_token -- nothing
         * to wait for. */
        ESP_LOGD(TAG, "failed to send topology report CoAP request: %s", otThreadErrorToString(err));
        return false;
    }

    bool ok = false;
    /* Correctly derived (never a bare "ack_timeout * N" guess) from the SAME
     * tx params just above via components/coap_response_timing's shared
     * helper -- see sensor_transport.c's identical use of this same
     * function for the real-hardware finding this replaced. */
    uint32_t wait_ms = coap_response_timing_wait_ms(TOPOLOGY_REPORTER_ACK_TIMEOUT_MS,
                                                      COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                                      COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN,
                                                      TOPOLOGY_REPORTER_ACK_TIMEOUT_MS);
    if (xSemaphoreTake(s_send_done_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        if (s_send_result == OT_ERROR_NONE && s_send_got_message) {
            uint8_t code_class = (uint8_t)(s_send_response_code >> 5);
            if (code_class == 2) {
                ok = true;
            } else {
                ESP_LOGW(TAG, "topology report rejected by power base: CoAP %u.%02u", code_class,
                         (unsigned)(s_send_response_code & 0x1F));
            }
        }
    }
    /* Whether this timed out or succeeded, the NEXT attempt (if any) bumps
     * s_send_token again, which is what makes a still-pending callback for
     * THIS attempt recognizable as stale from then on -- no explicit
     * invalidation needed here. */
    return ok;
}

/* Packs + sends with the same bounded UUID-hash backoff schedule
 * sensor_transport uses (section 8/13): 1 initial + up to 3 retries. */
static bool transmit(const topology_report_t *rep)
{
    uint8_t payload[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(rep, payload, sizeof(payload));
    if (len == 0) {
        ESP_LOGE(TAG, "failed to pack topology report (role=%u report_type=%u) -- dropped", rep->role,
                 rep->report_type);
        return false;
    }

    static const uint32_t retry_masks[TOPOLOGY_REPORTER_MAX_ATTEMPTS] = {0, 0xFF, 0x1FF, 0x3FF};
    uint32_t hash = node_calc_uuid_hash(rep->module_uuid);
    /* ATTACHED/UPDATED/DETACHED are the events an operator actually needs
     * to see live at INFO level; a routine HEARTBEAT success stays at
     * DEBUG so a healthy, unchanging chain does not flood the log (code
     * review follow-up section 7: "반복 heartbeat 성공은 DEBUG로 유지"). */
    esp_log_level_t success_level = (rep->report_type == TOPOLOGY_REPORT_HEARTBEAT) ? ESP_LOG_DEBUG : ESP_LOG_INFO;

    for (int attempt = 0; attempt < TOPOLOGY_REPORTER_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(hash & retry_masks[attempt]));
        }
        if (send_one_attempt(payload, len)) {
            ESP_LOG_LEVEL_LOCAL(success_level, TAG, "topology report sent OK (type=%u seq=%u attempt=%d/%d)",
                                 rep->report_type, (unsigned)rep->sequence, attempt + 1,
                                 TOPOLOGY_REPORTER_MAX_ATTEMPTS);
            return true;
        }
    }
    /* NOT a claim that the report was actually lost -- only that THIS SENDER
     * never received a confirmed 2.xx response within
     * TOPOLOGY_REPORTER_MAX_ATTEMPTS attempts (same reasoning as
     * sensor_transport.c's identical wording change, this follow-up). The
     * power base may have already applied it. */
    ESP_LOGW(TAG, "topology report delivery unconfirmed after %d attempts (type=%u seq=%u)",
             TOPOLOGY_REPORTER_MAX_ATTEMPTS, rep->report_type, (unsigned)rep->sequence);
    return false;
}

/* Builds and sends a normal (ATTACHED/UPDATED/HEARTBEAT) report from CURRENT
 * live state, caching its identity fields for a later best-effort DETACHED. */
static bool send_live_report(topology_report_type_t type)
{
    if (!s_reporting_enabled) {
        return false;
    }
    maybe_roll_over_sequence();
    if (!s_reporting_enabled) {
        return false; /* the rollover attempt above may have just disabled it */
    }

    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    sensor_service_get_module_uuid(rep.module_uuid);
    child_service_get_parent_uuid(rep.parent_uuid);
    rep.topology_depth = child_service_get_own_topology_depth();
    /* module_role_t (role_manager.h) and topology_role_t (topology_proto.h)
     * share the exact same numeric values by design -- see topology_proto.h's
     * top comment. */
    rep.role = (uint8_t)role_manager_get_role();
    rep.sensor_type_bitmap = s_cfg.sensor_type_bitmap;
    rep.boot_session_generation = s_generation;
    rep.sequence = ++s_sequence;
    rep.upstream_connected = true; /* caller only invokes this once role_manager_is_upstream_ready() */
    rep.downstream_connected = s_downstream_connected;
    rep.report_type = type;

    memcpy(s_last_module_uuid, rep.module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(s_last_parent_uuid, rep.parent_uuid, TOPOLOGY_UUID_LEN);
    s_last_depth = rep.topology_depth;
    s_last_role = rep.role;
    s_last_sensor_bitmap = rep.sensor_type_bitmap;

    return transmit(&rep);
}

/* Best-effort report that this module just lost upstream readiness, built
 * from the LAST cached snapshot (never re-reads child_service, which may
 * already have cleared its own session state for the same detach -- see this
 * component's header comment). */
static bool send_detached_report(void)
{
    if (!s_reporting_enabled) {
        return false;
    }
    maybe_roll_over_sequence();
    if (!s_reporting_enabled) {
        return false;
    }

    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, s_last_module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, s_last_parent_uuid, TOPOLOGY_UUID_LEN);
    rep.topology_depth = s_last_depth;
    rep.role = s_last_role;
    rep.sensor_type_bitmap = s_last_sensor_bitmap;
    rep.boot_session_generation = s_generation;
    rep.sequence = ++s_sequence;
    rep.upstream_connected = false;
    rep.downstream_connected = false;
    rep.report_type = TOPOLOGY_REPORT_DETACHED;
    return transmit(&rep);
}

/*
 * Ground-truth reconciliation for s_downstream_connected, run every poll
 * tick regardless of whether an event arrived -- code review follow-up
 * section 6: "queue가 가득 찬 경우 조용히 이벤트를 버리지 말고 ... 재동기화
 * 방법을 둔다". If topology_reporter_on_port_event()'s xQueueSend() ever
 * dropped an event (the queue was full), this still notices the real state
 * within at most one TOPOLOGY_REPORTER_POLL_TICK_MS -- s_downstream_connected
 * can never permanently desync from reality just because one queued event
 * was lost.
 */
static void resync_downstream_connected(bool has_attached)
{
    /* "Connected" means the DOWNSTREAM port is PEER_READY with a MODULE right
     * now. pogo_port_is_attached() alone is not enough: it stays true (and
     * pogo_port_get_peer_type() keeps the old MODULE) all through a
     * DOCK_TIMEOUT/LIVENESS_LOST retry cycle, which would re-report a failed
     * link as connected on the very next tick. */
    bool ground_truth = pogo_port_is_peer_ready(POGO_PORT_DOWNSTREAM) &&
                         pogo_port_get_peer_type(POGO_PORT_DOWNSTREAM) == POGO_PEER_TYPE_MODULE;
    if (ground_truth == s_downstream_connected) {
        return;
    }
    ESP_LOGI(TAG, "downstream_connected resynchronized: %d -> %d (a queued port event may have been dropped)",
             s_downstream_connected, ground_truth);
    s_downstream_connected = ground_truth;
    if (has_attached) {
        send_live_report(TOPOLOGY_REPORT_UPDATED);
    }
}

static void topology_reporter_task(void *arg)
{
    (void)arg;
    bool has_attached = false;
    module_role_t last_role = MODULE_ROLE_UNASSIGNED;
    TickType_t last_heartbeat = xTaskGetTickCount();
    /* Rate-limits the "still not registered" warning below -- an ATTACHED
     * attempt that keeps failing (the power base unreachable, or -- in
     * DEV_ROOT/DEV_LEAF module-to-module regression -- a peer that never
     * runs power_base_transport and so has no "/t" resource at all) must
     * not flood the log every single TOPOLOGY_REPORTER_POLL_TICK_MS
     * (section 4 of the follow-up spec: "매 주기마다 경고를 출력하지
     * 않는다"). 0 means "never logged yet". */
    TickType_t last_attach_fail_log = 0;

    while (1) {
        tr_port_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(TOPOLOGY_REPORTER_POLL_TICK_MS)) == pdTRUE &&
            evt.port_id == POGO_PORT_DOWNSTREAM) {
            bool now_connected = (evt.event == POGO_PORT_EVENT_PEER_READY);
            if (evt.event == POGO_PORT_EVENT_PEER_READY || evt.event == POGO_PORT_EVENT_ATTACHED ||
                evt.event == POGO_PORT_EVENT_DETACHED || evt.event == POGO_PORT_EVENT_ERROR) {
                if (now_connected != s_downstream_connected) {
                    s_downstream_connected = now_connected;
                    if (has_attached) {
                        send_live_report(TOPOLOGY_REPORT_UPDATED);
                    }
                }
            }
            /* FRAME_RECEIVED etc: not a connectivity change, ignored. */
        }

        /* Runs every tick regardless of the branch above -- see this
         * function's own doc comment. */
        resync_downstream_connected(has_attached);

        /*
         * Power base reboot recovery (section 8 of the follow-up spec,
         * docs/power-base.md section 11) AND parent-offline recovery (this
         * follow-up, docs/power-base.md's "A rejected child" bullet):
         * sensor_transport requests an immediate fresh ATTACHED attempt the
         * moment the power base rejects a "/s" sample either as
         * unknown-module (CoAP 4.04 -- its RAM-only registry forgot this
         * module across a reboot) OR as a known-but-currently-offline
         * module (CoAP 4.12 -- TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE, this
         * module's own parent is offline). The gate/reregister request
         * mechanism cannot and does not distinguish which of the two
         * actually happened (see sensor_transport.c's own comment on this),
         * so the log line below stays deliberately generic rather than
         * naming just one. Forcing has_attached back to false here means
         * the `settled && !has_attached` branch below retries THIS SAME
         * tick, bounded by one poll tick (<=TOPOLOGY_REPORTER_POLL_TICK_MS)
         * -- never the up-to-CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS
         * wait a plain heartbeat schedule would otherwise impose ("30초
         * heartbeat만 기다리지 않는다"). Consuming the request when
         * has_attached is already false is harmless: the retry path below
         * is already active on its own.
         */
        if (topology_gate_take_reregister_request() && has_attached) {
            ESP_LOGW(TAG, "power base rejected this module's sample because its topology registration is "
                          "unknown or no longer valid -- closing the data-plane gate and re-registering now");
            has_attached = false;
            topology_gate_close();
        }

        bool ready = role_manager_is_upstream_ready();
        module_role_t role = role_manager_get_role();
        bool settled = ready && (role == MODULE_ROLE_LEAF || role == MODULE_ROLE_INTERMEDIATE);
        TickType_t now = xTaskGetTickCount();

        if (settled && !has_attached) {
            last_role = role;
            if (send_live_report(TOPOLOGY_REPORT_ATTACHED)) {
                has_attached = true;
                last_heartbeat = now;
                /* The ONLY place the data-plane gate ever opens (section 5):
                 * a real CoAP 2.xx response to THIS ATTACHED report, never
                 * merely "the OpenThread transport delivered the bytes" --
                 * send_live_report()/transmit()/send_one_attempt() already
                 * enforce that (code review follow-up section 3), unchanged
                 * here. */
                topology_gate_open();
            } else if (last_attach_fail_log == 0 ||
                       (now - last_attach_fail_log) > pdMS_TO_TICKS(5000)) {
                last_attach_fail_log = now;
                ESP_LOGW(TAG, "still not registered with the power base -- retrying (this module's own sensor "
                              "sample sending, if any, stays paused until this succeeds)");
            }
            /* has_attached stays false on failure (section 6: an ATTACHED
             * failure is never marked as registered): the next poll tick
             * tries again, paced by this task's own
             * TOPOLOGY_REPORTER_POLL_TICK_MS wait plus transmit()'s existing
             * bounded 1+3 CoAP attempt/backoff cycle -- never a tight busy
             * loop, and never more than one ATTACHED attempt cycle per
             * tick. The data-plane gate stays closed the whole time. */
        } else if (!settled && has_attached) {
            send_detached_report();
            has_attached = false;
            /* Section 7: upstream/Thread/role loss closes the gate within
             * at most one poll tick -- see this function's own doc comment
             * on topology_gate_close()'s "immediately" for what bound that
             * actually is. */
            topology_gate_close();
        } else if (settled && has_attached && role != last_role) {
            last_role = role;
            send_live_report(TOPOLOGY_REPORT_UPDATED);
            last_heartbeat = now;
        } else if (settled && has_attached &&
                   (now - last_heartbeat) >= pdMS_TO_TICKS(CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS)) {
            send_live_report(TOPOLOGY_REPORT_HEARTBEAT);
            last_heartbeat = now;
        }
    }
}

esp_err_t topology_reporter_init(const module_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg; /* owned copy -- same reasoning as child_service's s_cfg */
    s_sequence = 0;
    s_downstream_connected = false;
    s_send_token = 0;

    s_send_done_sem = xSemaphoreCreateBinary();
    if (s_send_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t gen_err = load_next_boot_generation(&s_generation);
    if (gen_err != ESP_OK) {
        /*
         * Code review follow-up (2nd pass), fail-fast policy: the power
         * base's "/s" resource rejects sensor data from any module_uuid it
         * has never accepted a valid topology report for (see
         * power_base_transport.c's coap_sensor_handler() -- section 4 of
         * the first review pass). A module that can never send a report at
         * all (because it could not even persist a boot generation) would
         * therefore never be able to deliver normal sensor data either --
         * "quietly disable topology reporting but otherwise boot normally"
         * left the module in a state that could complete provisioning and
         * report itself healthy while every one of its sensor samples was
         * silently dropped as an unknown uuid. This is exactly the
         * inconsistency this fix removes: topology_reporter_init() now
         * fails outright (the same "no valid config, do not proceed" policy
         * module_config_load()/power_base_config_load() already use), so
         * module/main/app_main.c's existing ESP_ERROR_CHECK() on this call
         * halts the WHOLE boot before role_manager/child_service/
         * parent_service ever start -- never reaching a provisioned or
         * data-sending state on top of broken topology reporting.
         */
        ESP_LOGE(TAG, "failed to persist a fresh boot_session_generation (%s) -- refusing to start: this module "
                      "could never send a topology report, and the power base rejects sensor data from any uuid "
                      "it has no valid topology report for -- see topology_reporter.h",
                 esp_err_to_name(gen_err));
        vSemaphoreDelete(s_send_done_sem);
        s_send_done_sem = NULL;
        return gen_err;
    }
    s_reporting_enabled = true;
    ESP_LOGI(TAG, "boot_session_generation=%u (persisted)", (unsigned)s_generation);

    s_evt_queue = xQueueCreate(16, sizeof(tr_port_evt_t));
    if (s_evt_queue == NULL) {
        vSemaphoreDelete(s_send_done_sem);
        s_send_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(topology_reporter_task, "topology_reporter", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed -- topology reporting will not run this boot");
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
        vSemaphoreDelete(s_send_done_sem);
        s_send_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void topology_reporter_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event,
                                      const pogo_port_event_data_t *data)
{
    (void)data;
    if (port_id != POGO_PORT_DOWNSTREAM || s_evt_queue == NULL) {
        return;
    }
    tr_port_evt_t evt = {.port_id = port_id, .event = event};
    if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
        /* Rate-limited: a burst of dropped events must not flood the log.
         * resync_downstream_connected() (topology_reporter_task(), every
         * TOPOLOGY_REPORTER_POLL_TICK_MS) polls the real port state
         * unconditionally regardless of this drop, so this component's view
         * is never permanently wrong -- only delayed by at most one poll
         * tick (code review follow-up section 6). */
        static TickType_t s_last_drop_log;
        TickType_t now = xTaskGetTickCount();
        if (s_last_drop_log == 0 || (now - s_last_drop_log) > pdMS_TO_TICKS(5000)) {
            s_last_drop_log = now;
            ESP_LOGW(TAG, "port event queue full -- dropped one DOWNSTREAM event (will resync via poll within %dms)",
                     TOPOLOGY_REPORTER_POLL_TICK_MS);
        }
    }
}
