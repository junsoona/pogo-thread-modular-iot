#include "parent_service/parent_service.h"

#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "pogo_proto/pogo_frame.h"
#include "thread_manager/thread_manager.h"
#include "node_calc/node_calc.h"

static const char *TAG = "parent_service";

/*
 * SENSOR_INFO_ACK.sensor_status is a raw wire byte (see pogo_protocol.h's
 * own field comment) -- parent_service deliberately does NOT #include
 * sensor_provider/sensor_provider.h for sensor_status_t itself: this
 * component is reused verbatim by power_base/main/app_main.c, which has no
 * sensor_provider/module_config component at all (see this file's own
 * top-of-file comment). These two values are duplicated from
 * sensor_provider.h's sensor_status_t on purpose, not #included, for that
 * same reason.
 *
 * SENSOR_STATUS_CONDITIONING (child's PMS5003 driver still warming up, see
 * components/pms5003/pms5003.c's CONFIG_POGO... warmup_ms handling) is not a
 * failure: unlike every other non-zero sensor_status, provisioning continues
 * normally with it -- see run_provisioning_session()'s own sensor_status
 * check below. Any OTHER non-zero value still abandons the session exactly
 * as before.
 */
#define PS_SENSOR_STATUS_OK          0
#define PS_SENSOR_STATUS_CONDITIONING 7

/* How long a wait inside a provisioning session may go without looking at
 * the upstream generation (parent_service_context_t.get_upstream_generation).
 * Only the JOIN_RESULT wait is long enough for this to matter. */
#define PS_GENERATION_CHECK_MS 100
/* A failed session is retried while the child stays docked, after
 * CONFIG_POGO_PING_INTERVAL_MS -- doubled for every further failure in a
 * row, up to 2^PS_RETRY_BACKOFF_MAX_SHIFT times that (16 s with the default
 * 1 s). A success, a new handshake, a detach or a new upstream generation
 * starts over at the short pause. */
#define PS_RETRY_BACKOFF_MAX_SHIFT 4

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
} node_record_t;

typedef struct {
    pogo_port_event_t event;
    pogo_port_event_data_t data;
} ps_evt_t;

static QueueHandle_t s_evt_queue;
static parent_service_context_t s_provider;
static ps_state_t s_state = PS_STATE_IDLE;
static bool s_peer_currently_ready = false;
static bool s_peer_ready_pending = false;
static volatile bool s_session_aborted = false;
static node_record_t s_node;
static uint8_t s_seq;

/*
 * Upstream generation (see parent_service_context_t.get_upstream_generation).
 * s_session_generation is the number the current -- or last -- DOWNSTREAM
 * session was started under; PS_STATE_JOINED means "joined for that number".
 * s_session_superseded is set by the session's own waits when the number
 * moved on under it: the session is dropped at once, without the failure
 * pause, and started again for the new number.
 */
static uint32_t s_session_generation;
static bool s_session_superseded;
/*
 * What is known of the docked child's JOIN_RESULTs. This belongs to the
 * HANDSHAKE with that child, not to a session: it is set by whatever
 * JOIN_RESULT arrives -- in a session's join wait, during a session's earlier
 * steps, between sessions -- and it is forgotten only when the handshake is
 * (ATTACHED / DETACHED / ERROR / PEER_READY, where a child may have
 * restarted and counts from 1 again). A session that starts, fails, times
 * out or is superseded changes nothing here.
 *
 * Why that is enough to tell an old result from a session's own, with the
 * frames as they are (JOIN_RESULT carries the child's own seq and nothing of
 * the session it answers):
 *   - the child numbers its JOIN_RESULTs with a counter that goes on across
 *     its sessions (child_service.c, s_join_result_seq_counter: +1 for every
 *     result, 0 skipped), and repeats only its LATEST one, with that same
 *     seq, until it is ACKed -- or until a new session's IDENTIFY_REQ, which
 *     cancels the repeat for good;
 *   - it does all of that in one task, and the UART and the event queue keep
 *     the order: whatever it sent before it answered this session's
 *     IDENTIFY_REQ arrives here before that answer.
 * So a result of an earlier session can only be the newest one seen here so
 * far (`seen_seq`), and this session's own result is always another number.
 * A JOIN_RESULT with the newest seq seen so far is therefore never taken as
 * a session's result: it is ACKed (its sender may have missed the ACK) and
 * the session keeps waiting for its own. Every other numbered JOIN_RESULT in
 * a join wait is this session's.
 *
 * What the frames cannot tell: a result this parent has NEVER seen arriving
 * for the first time after this session's SET_DATASET was ACKed. A child
 * running child_service cannot send one (see above); a sender that does not
 * follow those rules could, and nothing short of a session number in
 * JOIN_RESULT itself would show it.
 *
 * `processed_seq` is the result a session took as its own, for the re-ACK of
 * its repeats outside any session. seq 0 is not a number (the child never
 * uses it): it is neither recorded nor compared.
 */
static struct {
    bool seen;
    uint8_t seen_seq;
    bool processed;
    uint8_t processed_seq;
} s_child_results;
/* The running session has sent its SET_DATASET. Until then a JOIN_RESULT can
 * only be an earlier session's; from then on one that arrives early -- while
 * the ACK of SET_DATASET is still awaited -- is this session's own, and is
 * left to be taken when it is repeated in the join wait. */
static bool s_session_dataset_sent;
/* Retry pacing for a failed session, see PS_RETRY_BACKOFF_MAX_SHIFT. */
static uint8_t s_failed_sessions_in_a_row;
static bool s_retry_gated;
static TickType_t s_retry_not_before;

static uint32_t upstream_generation(void)
{
    return (s_provider.get_upstream_generation != NULL) ? s_provider.get_upstream_generation(s_provider.ctx) : 0;
}

/* True (and the session marked as dropped) once the upstream generation is
 * no longer the one this session was started under. */
static bool session_superseded(void)
{
    if (upstream_generation() == s_session_generation) {
        return false;
    }
    s_session_superseded = true;
    s_session_aborted = true;
    return true;
}

static void forget_child_results(void)
{
    memset(&s_child_results, 0, sizeof(s_child_results));
}

/* Every JOIN_RESULT that arrives, wherever it arrives. */
static void note_join_result_seen(uint8_t seq)
{
    if (seq == 0) {
        return;
    }
    s_child_results.seen = true;
    s_child_results.seen_seq = seq;
}

/* True for the JOIN_RESULT this parent has already seen from the docked
 * child: an earlier session's, sent again. */
static bool join_result_is_known(uint8_t seq)
{
    return seq != 0 && s_child_results.seen && seq == s_child_results.seen_seq;
}

static void reset_retry_pacing(void)
{
    s_failed_sessions_in_a_row = 0;
    s_retry_gated = false;
}

/* DETACHED/ERROR seen by a session's own wait: what the main loop's switch
 * would have done, since it never sees an event consumed here. */
static void note_port_lost_during_session(void)
{
    s_session_aborted = true;
    s_peer_currently_ready = false;
    s_peer_ready_pending = false;
    forget_child_results();
    reset_retry_pacing();
}

/* Blocks on the event queue until `deadline`, in slices short enough to
 * notice a new upstream generation. Returns false at the deadline or when
 * the session was superseded. */
static bool receive_event_until(ps_evt_t *evt, TickType_t deadline)
{
    while (1) {
        if (session_superseded()) {
            return false;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) {
            return false;
        }
        TickType_t slice = deadline - now;
        if (s_provider.get_upstream_generation != NULL && slice > pdMS_TO_TICKS(PS_GENERATION_CHECK_MS)) {
            slice = pdMS_TO_TICKS(PS_GENERATION_CHECK_MS);
        }
        if (xQueueReceive(s_evt_queue, evt, slice) == pdTRUE) {
            return true;
        }
    }
}

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
 * DETACHED/ERROR aborts the whole session immediately (section 6), and so
 * does a new upstream generation. */
static bool wait_for_frame(pogo_frame_t *out, uint8_t match_seq, uint8_t expect_a, uint8_t expect_b,
                            TickType_t timeout)
{
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (1) {
        ps_evt_t evt;
        if (!receive_event_until(&evt, deadline)) {
            return false;
        }
        if (evt.event == POGO_PORT_EVENT_DETACHED || evt.event == POGO_PORT_EVENT_ERROR) {
            /* This event is consumed here, inside run_provisioning_session(),
             * so the main task loop's own switch (which normally clears
             * these) never sees it -- clear them here instead. Otherwise
             * s_peer_currently_ready would stay stale-true and the caller
             * would retry the whole provisioning sequence against a peer
             * that is no longer physically there. */
            note_port_lost_during_session();
            return false;
        }
        if (evt.event == POGO_PORT_EVENT_FRAME_RECEIVED && evt.data.frame.seq == match_seq &&
            (evt.data.frame.type == expect_a || (expect_b != 0 && evt.data.frame.type == expect_b))) {
            *out = evt.data.frame;
            return true;
        }
        /* stray/mismatched frame while waiting: ignore, keep waiting. A
         * JOIN_RESULT among them, before this session handed out its
         * Dataset, is an earlier session's: remembered as such, so that it
         * is not taken for this session's own if it comes again in the join
         * wait. */
        if (evt.event == POGO_PORT_EVENT_FRAME_RECEIVED && evt.data.frame.type == POGO_MSG_JOIN_RESULT &&
            !s_session_dataset_sent) {
            note_join_result_seen(evt.data.frame.seq);
        }
    }
}

/* Same idea but for the unsolicited JOIN_RESULT, which is not a reply to any
 * specific request (its seq is the child's own -- see pogo_protocol.h). A
 * JOIN_RESULT this parent has seen before is not this session's result: see
 * s_child_results. */
static bool wait_for_join_result(pogo_frame_t *out, TickType_t timeout)
{
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (1) {
        ps_evt_t evt;
        if (!receive_event_until(&evt, deadline)) {
            return false;
        }
        if (evt.event == POGO_PORT_EVENT_DETACHED || evt.event == POGO_PORT_EVENT_ERROR) {
            /* This event is consumed here, inside run_provisioning_session(),
             * so the main task loop's own switch (which normally clears
             * these) never sees it -- clear them here instead. Otherwise
             * s_peer_currently_ready would stay stale-true and the caller
             * would retry the whole provisioning sequence against a peer
             * that is no longer physically there. */
            note_port_lost_during_session();
            return false;
        }
        if (evt.event == POGO_PORT_EVENT_FRAME_RECEIVED && evt.data.frame.type == POGO_MSG_JOIN_RESULT) {
            if (join_result_is_known(evt.data.frame.seq)) {
                ESP_LOGI(TAG, "JOIN_RESULT of an earlier session again (seq=%u) -- re-sending ACK only, still "
                              "waiting for this session's own",
                         evt.data.frame.seq);
                pogo_port_send(POGO_PORT_DOWNSTREAM, POGO_MSG_JOIN_RESULT_ACK, evt.data.frame.seq, NULL, 0);
                continue;
            }
            note_join_result_seen(evt.data.frame.seq);
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
    s_session_superseded = false;
    s_session_dataset_sent = false;
    /* The node record is this session's. What is known of the child's
     * JOIN_RESULTs (s_child_results) is not, and is left alone. */
    memset(&s_node, 0, sizeof(s_node));

    const TickType_t ack_timeout =
        pdMS_TO_TICKS(s_provider.ack_timeout_ms != 0 ? s_provider.ack_timeout_ms : CONFIG_POGO_ACK_TIMEOUT_MS);
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

    /*
     * Code review follow-up, section 1: the child's module_uuid is a
     * PERMANENT identifier it owns (efuse-derived) -- this parent NEVER
     * invents one. REQUEST_UUID/PROPOSE_UUID retrieves it; ASSIGN_UUID (its
     * existing wire format, unchanged) is now used purely to CONFIRM the
     * exact same bytes back, never to assign something new.
     */
    if (!send_and_wait(POGO_MSG_REQUEST_UUID, NULL, 0, POGO_MSG_PROPOSE_UUID, 0, &frame, max_retry, ack_timeout)) {
        ESP_LOGW(TAG, "REQUEST_UUID failed -> abandoning session");
        return false;
    }
    pogo_propose_uuid_t proposed;
    if (!pogo_unpack_propose_uuid(frame.payload, frame.len, &proposed)) {
        ESP_LOGE(TAG, "malformed PROPOSE_UUID -> abandoning session");
        return false;
    }
    static const uint8_t all_zero_uuid[POGO_UUID_LEN] = {0};
    if (memcmp(proposed.uuid, all_zero_uuid, POGO_UUID_LEN) == 0) {
        /* section 1: "실패 시 all-zero UUID를 정상 식별자로 사용하지 않는다"
         * -- applies on the parent side too: never provision a child under
         * an identifier that could never legitimately belong to any real
         * device. */
        ESP_LOGE(TAG, "child proposed an all-zero UUID -- refusing to treat that as a valid permanent identifier, "
                      "abandoning session");
        return false;
    }
    memcpy(s_node.uuid, proposed.uuid, POGO_UUID_LEN);

    pogo_assign_uuid_t uuid_msg;
    memcpy(uuid_msg.uuid, s_node.uuid, POGO_UUID_LEN);
    uint8_t uuid_payload[POGO_UUID_LEN];
    size_t uuid_len = pogo_pack_assign_uuid(&uuid_msg, uuid_payload, sizeof(uuid_payload));
    if (!send_and_wait(POGO_MSG_ASSIGN_UUID, uuid_payload, uuid_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame, max_retry,
                        ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "ASSIGN_UUID (confirming the child's own proposed uuid) failed -> abandoning session");
        return false;
    }

    /* Exactly one DOWNSTREAM port on this hardware generation -> slot_id is
     * always 0, but topology_depth still reflects this PARENT's own depth+1
     * (not hardcoded), so a chain of INTERMEDIATE modules -- or a power base
     * at the very top -- is traceable. get_own_depth() is 0 for the true
     * network root (ROOT sensor-module fixture, or the power base itself). */
    uint8_t depth = (uint8_t)(s_provider.get_own_depth(s_provider.ctx) + 1);
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

    /* ASSIGN_PARENT_INFO (power-base follow-up section 7): tells the child
     * THIS parent's own uuid + whether this parent is the power base, so the
     * child can name a real parent in its own topology reports instead of
     * only knowing "my upstream peer_type". */
    pogo_assign_parent_info_t pinfo_msg = {.parent_is_power_base = s_provider.is_power_base ? 1 : 0};
    s_provider.get_own_uuid(pinfo_msg.parent_uuid, s_provider.ctx);
    uint8_t pinfo_payload[POGO_UUID_LEN + 1];
    size_t pinfo_len = pogo_pack_assign_parent_info(&pinfo_msg, pinfo_payload, sizeof(pinfo_payload));
    if (!send_and_wait(POGO_MSG_ASSIGN_PARENT_INFO, pinfo_payload, pinfo_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame,
                        max_retry, ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "ASSIGN_PARENT_INFO failed -> abandoning session");
        return false;
    }

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
    if (sinfo.sensor_status != PS_SENSOR_STATUS_OK && sinfo.sensor_status != PS_SENSOR_STATUS_CONDITIONING) {
        ESP_LOGW(TAG, "SENSOR_ERROR: child reports sensor_status=%u -> abandoning session (no polling calc, no "
                      "dataset handoff)",
                 sinfo.sensor_status);
        return false;
    }
    if (sinfo.sensor_status == PS_SENSOR_STATUS_CONDITIONING) {
        /* Not fatal -- the child's sensor driver (e.g. PMS5003) is still
         * warming up and will report SENSOR_STATUS_OK on its own once the
         * warm-up window elapses (no re-provisioning needed). Continue the
         * sequence normally: polling-period calc, SET_COLLECTOR, SET_DATASET
         * and the Thread join all proceed exactly as for sensor_status==OK. */
        ESP_LOGI(TAG, "child sensor still warming up (CONDITIONING) -- continuing provisioning; readings not yet "
                      "fully trustworthy");
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

    /*
     * SET_COLLECTOR (this follow-up, see docs/power-base.md): must succeed
     * BEFORE SET_DATASET -- child_service itself also enforces this
     * ordering (POGO_NACK_BAD_STATE for a SET_DATASET arriving before
     * COLLECTOR_SET), but abandoning the session here too avoids even
     * attempting SET_DATASET against a child that would only reject it.
     */
    pogo_collector_endpoint_t collector;
    if (!s_provider.get_collector(&collector, s_provider.ctx)) {
        ESP_LOGW(TAG, "collector endpoint not currently available -> abandoning session");
        return false;
    }
    uint8_t collector_payload[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t collector_len = pogo_pack_set_collector(&collector, collector_payload, sizeof(collector_payload));
    if (!send_and_wait(POGO_MSG_SET_COLLECTOR, collector_payload, collector_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame,
                        max_retry, ack_timeout) ||
        frame.type != POGO_MSG_ACK) {
        log_nack_if_any(&frame);
        ESP_LOGW(TAG, "SET_COLLECTOR failed -> abandoning session");
        return false;
    }

    otOperationalDatasetTlvs tlvs;
    if (thread_manager_get_active_dataset_tlvs(&tlvs) != ESP_OK) {
        ESP_LOGE(TAG, "DATASET_ERROR: failed to read our own active dataset TLVs -> abandoning session");
        return false;
    }
    s_session_dataset_sent = true;
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

    if (!wait_for_join_result(&frame, join_timeout)) {
        if (s_session_superseded) {
            ESP_LOGI(TAG, "upstream generation changed while waiting for the join result -> dropping this session");
        } else {
            ESP_LOGW(TAG, "THREAD_JOIN_TIMEOUT: no JOIN_RESULT from child -> abandoning session");
        }
        return false;
    }
    /* Record + ACK this JOIN_RESULT's seq BEFORE deciding success/failure --
     * section 7: "parent는 성공/실패 결과를 처리한 뒤 ACK" (processed here
     * means "the frame was validated and its seq captured", not necessarily
     * "join succeeded"). From here on a later retransmit of this exact seq
     * (our ACK got lost) is re-ACKed by the main task loop's FRAME_RECEIVED
     * handling below without ever re-entering this function or touching
     * s_node again -- and, should it come in a later session's join wait,
     * by wait_for_join_result(), which does not take it for that session's. */
    s_child_results.processed = true;
    s_child_results.processed_seq = frame.seq;
    pogo_port_send(POGO_PORT_DOWNSTREAM, POGO_MSG_JOIN_RESULT_ACK, frame.seq, NULL, 0);

    pogo_join_result_t result;
    if (!pogo_unpack_join_result(frame.payload, frame.len, &result) || !result.success) {
        otDeviceRole failed_role = (otDeviceRole)(frame.len >= 2 ? frame.payload[1] : 0);
        ESP_LOGW(TAG, "JOIN FAILED -- child otDeviceRole=%u(%s)", failed_role, thread_manager_role_to_str(failed_role));
        return false;
    }
    s_node.child_role = (otDeviceRole)result.role;
    s_node.last_seen = xTaskGetTickCount();
    ESP_LOGI(TAG, "JOIN OK -- child otDeviceRole=%u(%s) uuid_backoff=%ums depth=%u polling=%ums", result.role,
             thread_manager_role_to_str(s_node.child_role), s_node.backoff_delay_ms, s_node.topology_depth,
             s_node.polling_period_ms);
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
                forget_child_results();
                reset_retry_pacing();
                break;
            case POGO_PORT_EVENT_DETACHED:
                s_state = PS_STATE_IDLE;
                s_peer_currently_ready = false;
                s_peer_ready_pending = false;
                memset(&s_node, 0, sizeof(s_node));
                forget_child_results();
                reset_retry_pacing();
                break;
            case POGO_PORT_EVENT_PEER_READY:
                s_peer_currently_ready = true;
                s_peer_ready_pending = true;
                forget_child_results();
                reset_retry_pacing();
                break;
            case POGO_PORT_EVENT_ERROR:
                ESP_LOGW(TAG, "DOWNSTREAM port error=%d", (int)evt.data.error_code);
                s_state = PS_STATE_IDLE;
                s_peer_currently_ready = false;
                s_peer_ready_pending = false;
                forget_child_results();
                reset_retry_pacing();
                break;
            case POGO_PORT_EVENT_FRAME_RECEIVED:
                /* Mid-session frames are drained by run_provisioning_session()
                 * itself via wait_for_frame()/wait_for_join_result() and never reach
                 * here. A JOIN_RESULT arriving here (i.e. AFTER the session
                 * that processed it already returned) means the child never
                 * saw our JOIN_RESULT_ACK and is retrying with the same seq
                 * (section 7) -- re-ACK it without re-running provisioning or
                 * touching the node record again. Anything else arriving
                 * outside any session is a stray retransmit/protocol error
                 * and is simply ignored, as before -- except that a
                 * JOIN_RESULT no session waited for (it came after its
                 * session had given up) is remembered, so that no later
                 * session takes it for its own. It is not ACKed: nothing
                 * processed it. */
                if (evt.data.frame.type == POGO_MSG_JOIN_RESULT) {
                    if (s_child_results.processed && evt.data.frame.seq == s_child_results.processed_seq) {
                        ESP_LOGI(TAG, "duplicate JOIN_RESULT (seq=%u) -- re-sending ACK only", evt.data.frame.seq);
                        pogo_port_send(POGO_PORT_DOWNSTREAM, POGO_MSG_JOIN_RESULT_ACK, evt.data.frame.seq, NULL, 0);
                    }
                    note_join_result_seen(evt.data.frame.seq);
                }
                break;
            }
        }

        /* Read before the gate below: what the session then hands down can
         * only be this generation's or a newer one's, and a newer one is
         * caught by the comparisons that follow. */
        uint32_t generation = upstream_generation();

        if (s_state == PS_STATE_JOINED && generation != s_session_generation) {
            /* Everything the docked child was given came from an upstream
             * generation that no longer exists. It is still docked and
             * handshaken (nothing on the port changed), so it is simply
             * provisioned again from the first step. */
            ESP_LOGI(TAG, "upstream generation changed (%u -> %u) -- the docked child is provisioned again",
                     (unsigned)s_session_generation, (unsigned)generation);
            s_state = PS_STATE_IDLE;
            s_peer_ready_pending = s_peer_currently_ready;
            reset_retry_pacing();
        }

        bool retry_due = !s_retry_gated || (int32_t)(xTaskGetTickCount() - s_retry_not_before) >= 0;
        if (s_peer_ready_pending && s_state == PS_STATE_IDLE && s_retry_gated && generation != s_session_generation) {
            /* The pause was for a failure under a generation that is gone. */
            reset_retry_pacing();
            retry_due = true;
        }

        if (s_peer_ready_pending && s_state == PS_STATE_IDLE && retry_due) {
            if (s_provider.provisioning_allowed(s_provider.ctx)) {
                s_peer_ready_pending = false;
                s_retry_gated = false;
                s_state = PS_STATE_PROVISIONING;
                s_session_generation = generation;
                ESP_LOGI(TAG, "starting downstream provisioning as %s", s_provider.role_name(s_provider.ctx));
                bool joined = run_provisioning_session();
                if (joined) {
                    /* JOINED for s_session_generation. If the generation
                     * moved on in the meantime, the check above sends the
                     * child through provisioning again on the next pass. */
                    s_state = PS_STATE_JOINED;
                    reset_retry_pacing();
                } else {
                    s_state = PS_STATE_IDLE;
                    if (s_peer_currently_ready) {
                        /* Peer is still physically docked: retry the whole
                         * sequence rather than waiting forever for a
                         * PEER_READY that already happened once (section 16:
                         * bounded retries per step, but the outer sequence
                         * itself may be retried while still docked). */
                        s_peer_ready_pending = true;
                        if (s_session_superseded) {
                            reset_retry_pacing(); /* not a failure: start over for the new generation now */
                        } else {
                            /* A pause that grows with every failure in a
                             * row, kept as a deadline so this task goes on
                             * handling port events (a detach above all)
                             * while it lasts. */
                            uint8_t shift = s_failed_sessions_in_a_row;
                            if (shift > PS_RETRY_BACKOFF_MAX_SHIFT) {
                                shift = PS_RETRY_BACKOFF_MAX_SHIFT;
                            }
                            if (s_failed_sessions_in_a_row < UINT8_MAX) {
                                s_failed_sessions_in_a_row++;
                            }
                            s_retry_gated = true;
                            s_retry_not_before =
                                xTaskGetTickCount() + (pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS) << shift);
                        }
                    }
                }
            }
            /* else: role/upstream not ready yet -- keep waiting, re-checked
             * every loop iteration (section 7). */
        }
    }
}

esp_err_t parent_service_init(const parent_service_context_t *provider)
{
    if (provider == NULL || provider->provisioning_allowed == NULL || provider->get_own_depth == NULL ||
        provider->get_own_uuid == NULL || provider->role_name == NULL || provider->get_collector == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_provider = *provider; /* owned copy -- same reasoning as child_service's s_cfg */
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
