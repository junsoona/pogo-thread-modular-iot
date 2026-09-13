#include "parent_service/parent_service.h"

#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "pogo_proto/pogo_frame.h"
#include "child_service/child_service.h"
#include "role_manager/role_manager.h"
#include "thread_manager/thread_manager.h"
#include "node_calc/node_calc.h"

static const char *TAG = "parent_service";

typedef enum {
    PS_STATE_IDLE = 0,
    PS_STATE_PROVISIONING,
    PS_STATE_JOINED,
} ps_state_t;

typedef struct {
    uint8_t   uuid[POGO_UUID_LEN];
    uint8_t   mac[6];
    uint8_t   topology_depth;
    uint16_t  sensor_type_bitmap;
    uint32_t  response_time_ms;
    uint32_t  polling_period_ms;
    uint16_t  backoff_delay_ms;
    uint8_t   retry_count;
    otDeviceRole child_role;
    TickType_t last_seen;
    /* JOIN_RESULT reliability (section 7 of the follow-up spec): the seq of
     * the JOIN_RESULT this session has already processed and ACKed, kept
     * alive PAST run_provisioning_session() returning (unlike everything
     * else session-scoped above, which a NEW session simply overwrites) so
     * a late retransmit of the SAME JOIN_RESULT -- the child never saw our
     * first ACK -- gets re-ACKed without re-running provisioning or
     * touching this record again. Reset to false only by memset(&s_node,...)
     * at the start of a new run_provisioning_session() or on DETACHED --
     * i.e. only a new provisioning generation or an explicit session
     * teardown clears it, matching child_service's own retry-cancellation
     * policy. */
    bool      has_join_result;
    uint8_t   last_join_result_seq;
} node_record_t;

typedef struct {
    pogo_port_event_t event;
    pogo_port_event_data_t data;
} ps_evt_t;

static QueueHandle_t s_evt_queue;
static ps_state_t s_state = PS_STATE_IDLE;
static bool s_peer_currently_ready = false;
static bool s_peer_ready_pending = false;
static volatile bool s_session_aborted = false;
static node_record_t s_node;
static uint8_t s_seq;

esp_err_t parent_service_calc_polling_period_ms(uint32_t response_time_ms, uint32_t *out_period_ms)
{
    /* Thin wrapper -- see components/node_calc/node_calc.h's header comment
     * for why the actual implementation moved there. */
    return node_calc_polling_period_ms(response_time_ms, out_period_ms);
}

uint16_t parent_service_calc_backoff_ms(const uint8_t uuid[POGO_UUID_LEN])
{
    return node_calc_backoff_ms(uuid);
}

static void log_nack_if_any(const pogo_frame_t *frame)
{
    if (frame->type != POGO_MSG_NACK) {
        return;
    }
    pogo_nack_t nack;
    if (pogo_unpack_nack(frame->payload, frame->len, &nack)) {
        ESP_LOGW(TAG, "NACK for type=0x%02x reason=%u", nack.acked_type, nack.reason);
    }
}

/* Drains the event queue looking for a FRAME_RECEIVED with the given seq
 * and one of two accepted types (expect_b==0: "don't care"). Any
 * DETACHED/ERROR aborts the whole session immediately (section 6). */
static bool wait_for_frame(pogo_frame_t *out, uint8_t match_seq, uint8_t expect_a, uint8_t expect_b,
                            TickType_t timeout)
{
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) {
            return false;
        }
        ps_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, deadline - now) != pdTRUE) {
            continue;
        }
        if (evt.event == POGO_PORT_EVENT_DETACHED || evt.event == POGO_PORT_EVENT_ERROR) {
            /* This event is consumed here, inside run_provisioning_session(),
             * so the main task loop's own switch (which normally clears
             * these) never sees it -- clear them here instead. Otherwise
             * s_peer_currently_ready would stay stale-true and the caller
             * would retry the whole provisioning sequence against a peer
             * that is no longer physically there. */
            s_session_aborted = true;
            s_peer_currently_ready = false;
            s_peer_ready_pending = false;
            return false;
        }
        if (evt.event == POGO_PORT_EVENT_FRAME_RECEIVED && evt.data.frame.seq == match_seq &&
            (evt.data.frame.type == expect_a || (expect_b != 0 && evt.data.frame.type == expect_b))) {
            *out = evt.data.frame;
            return true;
        }
        /* stray/mismatched frame while waiting: ignore, keep waiting */
    }
}

/* Same idea but for the unsolicited JOIN_RESULT, which the child sends with
 * seq=0 (not a reply to any specific request) -- see pogo_protocol.h. */
static bool wait_for_type(pogo_frame_t *out, uint8_t type, TickType_t timeout)
{
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) {
            return false;
        }
        ps_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, deadline - now) != pdTRUE) {
            continue;
        }
        if (evt.event == POGO_PORT_EVENT_DETACHED || evt.event == POGO_PORT_EVENT_ERROR) {
            /* This event is consumed here, inside run_provisioning_session(),
             * so the main task loop's own switch (which normally clears
             * these) never sees it -- clear them here instead. Otherwise
             * s_peer_currently_ready would stay stale-true and the caller
             * would retry the whole provisioning sequence against a peer
             * that is no longer physically there. */
            s_session_aborted = true;
            s_peer_currently_ready = false;
            s_peer_ready_pending = false;
            return false;
        }
        if (evt.event == POGO_PORT_EVENT_FRAME_RECEIVED && evt.data.frame.type == type) {
            *out = evt.data.frame;
            return true;
        }
    }
}

/* Sends one request (same seq for every retry, matching child_service's
 * dedup-by-seq -- section 6/16) and waits for a same-seq reply. */
static bool send_and_wait(uint8_t type, const uint8_t *payload, uint16_t len, uint8_t expect_a, uint8_t expect_b,
                           pogo_frame_t *out_frame, int max_retry, TickType_t per_try_timeout)
{
    s_seq++;
    uint8_t seq = s_seq;
    for (int attempt = 0; attempt <= max_retry; attempt++) {
        if (s_session_aborted) {
            return false;
        }
        esp_err_t err = pogo_port_send(POGO_PORT_DOWNSTREAM, type, seq, payload, len);
        if (err == ESP_OK && wait_for_frame(out_frame, seq, expect_a, expect_b, per_try_timeout)) {
            return true;
        }
        if (s_session_aborted) {
            return false;
        }
        if (attempt < max_retry) {
            ESP_LOGW(TAG, "retry %d/%d for request type=0x%02x", attempt + 1, max_retry, type);
        }
    }
    return false;
}

/* Runs the full section-12 sequence once (steps 2-13; PEER_HELLO/attach are
 * already done by pogo_port). Returns true only once JOIN_RESULT reports
 * success. */
static bool run_provisioning_session(void)
{
    s_session_aborted = false;
    memset(&s_node, 0, sizeof(s_node));

    const TickType_t ack_timeout = pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
    const TickType_t join_timeout = pdMS_TO_TICKS(CONFIG_POGO_JOIN_TIMEOUT_MS);
    const int max_retry = CONFIG_POGO_MAX_RETRY;
    pogo_frame_t frame = {0};

    if (!send_and_wait(POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, 0, &frame, max_retry, ack_timeout)) {
        ESP_LOGW(TAG, "IDENTIFY failed -> abandoning session");
        return false;
    }
    pogo_identify_resp_t ident;
    if (!pogo_unpack_identify_resp(frame.payload, frame.len, &ident)) {
        ESP_LOGE(TAG, "malformed IDENTIFY_RESP -> abandoning session");
        return false;
    }
    memcpy(s_node.mac, ident.mac, 6);
    ESP_LOGI(TAG, "child identified: mac=%02x:%02x:%02x:%02x:%02x:%02x fw=%u sensor_bitmap=0x%02x", ident.mac[0],
             ident.mac[1], ident.mac[2], ident.mac[3], ident.mac[4], ident.mac[5], ident.fw_version,
             ident.sensor_bitmap);
    esp_fill_random(s_node.uuid, POGO_UUID_LEN);

    pogo_assign_uuid_t uuid_msg;
    memcpy(uuid_msg.uuid, s_node.uuid, POGO_UUID_LEN);
    uint8_t uuid_payload[POGO_UUID_LEN];
    size_t uuid_len = pogo_pack_assign_uuid(&uuid_msg, uuid_payload, sizeof(uuid_payload));
    if (!send_and_wait(POGO_MSG_ASSIGN_UUID, uuid_payload, uuid_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame, max_retry,
                        ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "ASSIGN_UUID failed -> abandoning session");
        return false;
    }

    /* Exactly one DOWNSTREAM port on this hardware generation -> slot_id is
     * always 0, but topology_depth still reflects this module's own depth
     * (not hardcoded), so a chain of INTERMEDIATE modules is traceable. */
    uint8_t depth = (role_manager_get_role() == MODULE_ROLE_ROOT) ? 1 : (uint8_t)(child_service_get_own_topology_depth() + 1);
    pogo_assign_slot_t slot_msg = {.slot_id = 0, .topology_depth = depth};
    uint8_t slot_payload[2];
    size_t slot_len = pogo_pack_assign_slot(&slot_msg, slot_payload, sizeof(slot_payload));
    if (!send_and_wait(POGO_MSG_ASSIGN_SLOT, slot_payload, slot_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame, max_retry,
                        ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "ASSIGN_SLOT failed -> abandoning session");
        return false;
    }
    s_node.topology_depth = depth;
    s_node.backoff_delay_ms = parent_service_calc_backoff_ms(s_node.uuid);

    if (!send_and_wait(POGO_MSG_SENSOR_HELLO, NULL, 0, POGO_MSG_SENSOR_INFO_ACK, POGO_MSG_NACK, &frame, max_retry,
                        ack_timeout) ||
        frame.type != POGO_MSG_SENSOR_INFO_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "SENSOR_HELLO failed -> abandoning session");
        return false;
    }
    pogo_sensor_info_ack_t sinfo;
    if (!pogo_unpack_sensor_info_ack(frame.payload, frame.len, &sinfo)) {
        ESP_LOGE(TAG, "malformed SENSOR_INFO_ACK -> abandoning session");
        return false;
    }
    if (sinfo.sensor_status != 0) {
        ESP_LOGW(TAG, "SENSOR_ERROR: child reports sensor_status=%u -> abandoning session (no polling calc, no "
                      "dataset handoff)",
                 sinfo.sensor_status);
        return false;
    }
    s_node.sensor_type_bitmap = sinfo.sensor_type_bitmap;
    s_node.response_time_ms = sinfo.response_time_ms;
    ESP_LOGI(TAG, "child sensor: bitmap=0x%04x response_time=%ums status=%u", sinfo.sensor_type_bitmap,
             (unsigned)sinfo.response_time_ms, sinfo.sensor_status);

    uint32_t period_ms;
    if (parent_service_calc_polling_period_ms(sinfo.response_time_ms, &period_ms) != ESP_OK) {
        ESP_LOGE(TAG, "DATASET_ERROR: invalid response_time_ms=%u for polling calc -> abandoning session",
                 (unsigned)sinfo.response_time_ms);
        return false;
    }
    s_node.polling_period_ms = period_ms;

    pogo_poll_config_t poll_msg = {.polling_period_ms = period_ms};
    uint8_t poll_payload[4];
    size_t poll_len = pogo_pack_poll_config(&poll_msg, poll_payload, sizeof(poll_payload));
    if (!send_and_wait(POGO_MSG_POLL_CONFIG, poll_payload, poll_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame, max_retry,
                        ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "POLL_CONFIG failed -> abandoning session");
        return false;
    }
    ESP_LOGI(TAG, "polling_period_ms=%u sent (from response_time_ms=%u)", (unsigned)period_ms,
             (unsigned)sinfo.response_time_ms);

    otOperationalDatasetTlvs tlvs;
    if (thread_manager_get_active_dataset_tlvs(&tlvs) != ESP_OK) {
        ESP_LOGE(TAG, "DATASET_ERROR: failed to read our own active dataset TLVs -> abandoning session");
        return false;
    }
    if (!send_and_wait(POGO_MSG_SET_DATASET, tlvs.mTlvs, tlvs.mLength, POGO_MSG_ACK, POGO_MSG_NACK, &frame, max_retry,
                        ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "SET_DATASET failed -> abandoning session");
        return false;
    }
    /* Dataset TLVs are never logged: they contain the Network Key. */
    ESP_LOGI(TAG, "dataset accepted (%u bytes) -- waiting for join result (timeout %dms)", (unsigned)tlvs.mLength,
             CONFIG_POGO_JOIN_TIMEOUT_MS);

    if (!wait_for_type(&frame, POGO_MSG_JOIN_RESULT, join_timeout)) {
        ESP_LOGW(TAG, "THREAD_JOIN_TIMEOUT: no JOIN_RESULT from child -> abandoning session");
        return false;
    }
    /* Record + ACK this JOIN_RESULT's seq BEFORE deciding success/failure --
     * section 7: "parent는 성공/실패 결과를 처리한 뒤 ACK" (processed here
     * means "the frame was validated and its seq captured", not necessarily
     * "join succeeded"). Once has_join_result is set, a later retransmit of
     * this exact seq (our ACK got lost) is re-ACKed by the main task loop's
     * FRAME_RECEIVED handling below without ever re-entering this function
     * or touching s_node again. */
    s_node.has_join_result = true;
    s_node.last_join_result_seq = frame.seq;
    pogo_port_send(POGO_PORT_DOWNSTREAM, POGO_MSG_JOIN_RESULT_ACK, frame.seq, NULL, 0);

    pogo_join_result_t result;
    if (!pogo_unpack_join_result(frame.payload, frame.len, &result) || !result.success) {
        ESP_LOGW(TAG, "JOIN FAILED -- child otDeviceRole=%u", frame.len >= 2 ? frame.payload[1] : 0);
        return false;
    }
    s_node.child_role = (otDeviceRole)result.role;
    s_node.last_seen = xTaskGetTickCount();
    ESP_LOGI(TAG, "JOIN OK -- child otDeviceRole=%u uuid_backoff=%ums depth=%u polling=%ums", result.role,
             s_node.backoff_delay_ms, s_node.topology_depth, s_node.polling_period_ms);
    return true;
}

static void parent_service_task(void *arg)
{
    (void)arg;
    while (1) {
        ps_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (evt.event) {
            case POGO_PORT_EVENT_ATTACHED:
                s_state = PS_STATE_IDLE;
                break;
            case POGO_PORT_EVENT_DETACHED:
                s_state = PS_STATE_IDLE;
                s_peer_currently_ready = false;
                s_peer_ready_pending = false;
                memset(&s_node, 0, sizeof(s_node));
                break;
            case POGO_PORT_EVENT_PEER_READY:
                s_peer_currently_ready = true;
                s_peer_ready_pending = true;
                break;
            case POGO_PORT_EVENT_ERROR:
                ESP_LOGW(TAG, "DOWNSTREAM port error=%d", (int)evt.data.error_code);
                s_state = PS_STATE_IDLE;
                s_peer_currently_ready = false;
                s_peer_ready_pending = false;
                break;
            case POGO_PORT_EVENT_FRAME_RECEIVED:
                /* Mid-session frames are drained by run_provisioning_session()
                 * itself via wait_for_frame()/wait_for_type() and never reach
                 * here. A JOIN_RESULT arriving here (i.e. AFTER the session
                 * that processed it already returned) means the child never
                 * saw our JOIN_RESULT_ACK and is retrying with the same seq
                 * (section 7) -- re-ACK it without re-running provisioning or
                 * touching the node record again. Anything else arriving
                 * outside any session is a stray retransmit/protocol error
                 * and is simply ignored, as before. */
                if (evt.data.frame.type == POGO_MSG_JOIN_RESULT && s_node.has_join_result &&
                    evt.data.frame.seq == s_node.last_join_result_seq) {
                    ESP_LOGI(TAG, "duplicate JOIN_RESULT (seq=%u) -- re-sending ACK only", evt.data.frame.seq);
                    pogo_port_send(POGO_PORT_DOWNSTREAM, POGO_MSG_JOIN_RESULT_ACK, evt.data.frame.seq, NULL, 0);
                }
                break;
            }
        }

        if (s_peer_ready_pending && s_state == PS_STATE_IDLE) {
            module_role_t role = role_manager_get_role();
            bool role_ok = (role == MODULE_ROLE_ROOT || role == MODULE_ROLE_INTERMEDIATE);
            if (role_ok && role_manager_is_upstream_ready()) {
                s_peer_ready_pending = false;
                s_state = PS_STATE_PROVISIONING;
                ESP_LOGI(TAG, "starting downstream provisioning as %s", role_manager_role_to_str(role));
                bool joined = run_provisioning_session();
                if (joined) {
                    s_state = PS_STATE_JOINED;
                } else {
                    s_state = PS_STATE_IDLE;
                    if (s_peer_currently_ready) {
                        /* Peer is still physically docked: retry the whole
                         * sequence after a short cooldown rather than
                         * waiting forever for a PEER_READY that already
                         * happened once (section 16: bounded retries per
                         * step, but the outer sequence itself may be
                         * retried while still docked). */
                        vTaskDelay(pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS));
                        s_peer_ready_pending = true;
                    }
                }
            }
            /* else: role/upstream not ready yet -- keep waiting, re-checked
             * every loop iteration (section 7). */
        }
    }
}

esp_err_t parent_service_init(const module_config_t *cfg)
{
    (void)cfg;
    s_evt_queue = xQueueCreate(16, sizeof(ps_evt_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(parent_service_task, "parent_service", 4096, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void parent_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    if (port_id != POGO_PORT_DOWNSTREAM || s_evt_queue == NULL) {
        return;
    }
    ps_evt_t evt = {.event = event, .data = data ? *data : (pogo_port_event_data_t){0}};
    xQueueSend(s_evt_queue, &evt, 0);
}
