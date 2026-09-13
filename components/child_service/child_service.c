#include "child_service/child_service.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "sensor_provider/sensor_provider.h"
#include "thread_manager/thread_manager.h"

static const char *TAG = "child_service";

#define CHILD_FW_VERSION 1
#define CHILD_NVS_NAMESPACE "child_dyn"
#define POLL_PERIOD_MIN_MS 1u
#define POLL_PERIOD_MAX_MS 3600000u /* 1 hour: a sane upper sanity bound, not from the spec's RT table */

/*
 * Strict forward sequence (each message type is only accepted in the exact
 * preceding state; anything else is POGO_NACK_BAD_STATE -- see
 * handle_assign_uuid/handle_assign_slot/handle_sensor_hello/handle_poll_config/
 * handle_set_dataset below):
 *
 *   READY -> IDENTIFIED -> UUID_ASSIGNED -> SLOT_ASSIGNED -> SENSOR_DONE
 *         -> POLL_SET -> JOINING -> JOINED
 *
 * A retransmission of the exact same (type, seq) is still answered from the
 * dedup cache regardless of the current state (see dedup_resend_if_repeat) --
 * the strict ordering above only applies to a message that is NOT a cached
 * duplicate.
 */
typedef enum {
    CS_STATE_WAIT_PEER = 0,
    CS_STATE_READY,         /* PEER_READY: waiting for IDENTIFY_REQ */
    CS_STATE_IDENTIFIED,    /* IDENTIFY_RESP sent */
    CS_STATE_UUID_ASSIGNED, /* has uuid */
    CS_STATE_SLOT_ASSIGNED, /* has slot_id + topology_depth */
    CS_STATE_SENSOR_DONE,   /* SENSOR_INFO_ACK sent */
    CS_STATE_POLL_SET,      /* POLL_CONFIG acked */
    CS_STATE_JOINING,
    CS_STATE_JOINED,
    CS_STATE_ERROR,
} cs_state_t;

typedef struct {
    pogo_port_event_t event;
    pogo_port_event_data_t data;
} child_evt_t;

#define DEDUP_COUNT 5
typedef struct {
    bool valid;
    uint8_t seq;
    uint8_t resp_type;
    uint8_t resp_payload[8];
    size_t resp_len;
} dedup_entry_t;

static QueueHandle_t s_evt_queue;
static const module_config_t *s_cfg;
static const sensor_provider_ops_t *s_sensor_ops;
static sensor_provider_ctx_t *s_sensor_ctx;

static cs_state_t s_state = CS_STATE_WAIT_PEER;
static dedup_entry_t s_dedup[DEDUP_COUNT];
static uint8_t s_uuid[POGO_UUID_LEN];
static uint8_t s_slot_id, s_topology_depth;
static uint32_t s_polling_period_ms;
static uint8_t s_dataset[POGO_FRAME_MAX_PAYLOAD];
static uint16_t s_dataset_len;
static TickType_t s_join_deadline;
static volatile module_role_t s_role_for_join = MODULE_ROLE_UNASSIGNED;
static module_role_t s_last_joined_role = MODULE_ROLE_UNASSIGNED;

static int dedup_index_for(uint8_t msg_type)
{
    switch (msg_type) {
    case POGO_MSG_ASSIGN_UUID:
        return 0;
    case POGO_MSG_ASSIGN_SLOT:
        return 1;
    case POGO_MSG_SENSOR_HELLO:
        return 2;
    case POGO_MSG_POLL_CONFIG:
        return 3;
    case POGO_MSG_SET_DATASET:
        return 4;
    default:
        return -1;
    }
}

static bool dedup_resend_if_repeat(int idx, uint8_t seq)
{
    if (idx < 0 || !s_dedup[idx].valid || s_dedup[idx].seq != seq) {
        return false;
    }
    ESP_LOGI(TAG, "duplicate seq=%u for a write message -- resending cached response, not reapplying", seq);
    pogo_port_send(POGO_PORT_UPSTREAM, s_dedup[idx].resp_type, seq, s_dedup[idx].resp_payload, s_dedup[idx].resp_len);
    return true;
}

static void dedup_remember(int idx, uint8_t seq, uint8_t resp_type, const uint8_t *resp_payload, size_t resp_len)
{
    if (idx < 0 || resp_len > sizeof(s_dedup[0].resp_payload)) {
        return;
    }
    s_dedup[idx].valid = true;
    s_dedup[idx].seq = seq;
    s_dedup[idx].resp_type = resp_type;
    s_dedup[idx].resp_len = resp_len;
    memcpy(s_dedup[idx].resp_payload, resp_payload, resp_len);
}

static void reset_session(void)
{
    s_state = CS_STATE_WAIT_PEER;
    memset(s_dedup, 0, sizeof(s_dedup));
    memset(s_uuid, 0, sizeof(s_uuid));
    s_slot_id = s_topology_depth = 0;
    s_polling_period_ms = 0;
    s_dataset_len = 0;
    s_join_deadline = 0;
}

/*
 * Persists the received uuid/slot/depth/polling_period/dataset. Every NVS
 * call's result is checked -- a failure anywhere (open, any set, or commit)
 * is reported to the caller so handle_set_dataset() can refuse to ACK and
 * refuse to start a Thread join on a dataset that was never durably saved.
 * Diagnostic/forward-compatibility record only -- see docs/state-machine.md:
 * child_service deliberately does not auto-rejoin from this on boot without
 * a fresh handshake (section 14: a changed upstream parent must not be
 * trusted silently).
 */
static esp_err_t save_dynamic_record(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CHILD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_dynamic_record: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    do {
        err = nvs_set_blob(h, "uuid", s_uuid, POGO_UUID_LEN);
        if (err != ESP_OK) break;
        err = nvs_set_u8(h, "slot", s_slot_id);
        if (err != ESP_OK) break;
        err = nvs_set_u8(h, "depth", s_topology_depth);
        if (err != ESP_OK) break;
        err = nvs_set_u32(h, "poll_ms", s_polling_period_ms);
        if (err != ESP_OK) break;
        err = nvs_set_u16(h, "ds_len", s_dataset_len);
        if (err != ESP_OK) break;
        err = nvs_set_blob(h, "ds_tlv", s_dataset, s_dataset_len);
        if (err != ESP_OK) break;
        err = nvs_commit(h);
    } while (0);

    nvs_close(h);
    if (err != ESP_OK) {
        /* Dataset bytes are never logged, only the fact that storage failed. */
        ESP_LOGE(TAG, "save_dynamic_record failed: %s -- dataset NOT applied, NACKing SET_DATASET", esp_err_to_name(err));
    }
    return err;
}

static void send_join_result(bool success, otDeviceRole role)
{
    pogo_join_result_t result = {.success = success ? 1 : 0, .role = (uint8_t)role};
    uint8_t payload[2];
    size_t len = pogo_pack_join_result(&result, payload, sizeof(payload));
    pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_JOIN_RESULT, 0, payload, len);
}

static void handle_identify_req(const pogo_frame_t *frame)
{
    pogo_identify_resp_t resp = {0};
    esp_efuse_mac_get_default(resp.mac);
    resp.fw_version = CHILD_FW_VERSION;
    resp.sensor_bitmap = (uint8_t)(s_cfg->sensor_type_bitmap & 0xFF);
    uint8_t payload[8];
    size_t len = pogo_pack_identify_resp(&resp, payload, sizeof(payload));
    pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_IDENTIFY_RESP, frame->seq, payload, len);
    if (s_state == CS_STATE_READY) {
        s_state = CS_STATE_IDENTIFIED;
    }
    /* A later re-IDENTIFY (state already past IDENTIFIED) is harmless --
     * no side effect to redo -- and intentionally still answered without
     * regressing state or NACKing. */
}

static void handle_assign_uuid(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    pogo_assign_uuid_t msg;
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;
    if (s_state != CS_STATE_IDENTIFIED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (pogo_unpack_assign_uuid(frame->payload, frame->len, &msg)) {
        memcpy(s_uuid, msg.uuid, POGO_UUID_LEN);
        pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_UUID};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_UUID_ASSIGNED;
    } else {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_assign_slot(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    pogo_assign_slot_t msg;
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;
    if (s_state != CS_STATE_UUID_ASSIGNED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_SLOT, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (pogo_unpack_assign_slot(frame->payload, frame->len, &msg)) {
        s_slot_id = msg.slot_id;
        s_topology_depth = msg.topology_depth;
        pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_SLOT};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_SLOT_ASSIGNED;
    } else {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_SLOT, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_sensor_hello(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    if (s_state != CS_STATE_SLOT_ASSIGNED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SENSOR_HELLO, .reason = POGO_NACK_BAD_STATE};
        uint8_t resp_payload[2];
        size_t resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
        pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_NACK, frame->seq, resp_payload, resp_len);
        dedup_remember(idx, frame->seq, POGO_MSG_NACK, resp_payload, resp_len);
        return;
    }

    uint8_t resp_type, resp_payload[8];
    size_t resp_len;

    /* Fixed, permanently-paired sensor: exactly one retry budget for THIS
     * driver, never a fallback to a different one (section 11). */
    esp_err_t err = s_sensor_ops->start_measurement(s_sensor_ctx);
    sensor_measurement_t meas = {0};
    if (err == ESP_OK) {
        err = s_sensor_ops->read_measurement(s_sensor_ctx, &meas, pdMS_TO_TICKS(s_cfg->measurement_timeout_ms));
    }

    if (err == ESP_OK) {
        pogo_sensor_info_ack_t ack = {
            .sensor_type_bitmap = meas.sensor_type_bitmap,
            .response_time_ms = meas.response_time_ms,
            .fw_version = CHILD_FW_VERSION,
            .sensor_status = (uint8_t)s_sensor_ops->get_status(s_sensor_ctx),
        };
        resp_type = POGO_MSG_SENSOR_INFO_ACK;
        resp_len = pogo_pack_sensor_info_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_SENSOR_DONE;
        ESP_LOGI(TAG, "sensor measurement OK: bitmap=0x%04x response_time=%ums", meas.sensor_type_bitmap,
                 (unsigned)meas.response_time_ms);
    } else {
        ESP_LOGW(TAG, "SENSOR_ERROR: %s", esp_err_to_name(err));
        pogo_nack_t nack = {.acked_type = POGO_MSG_SENSOR_HELLO, .reason = POGO_NACK_SENSOR_ERROR};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_poll_config(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;

    pogo_poll_config_t cfg_msg;
    if (s_state != CS_STATE_SENSOR_DONE) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_POLL_CONFIG, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!pogo_unpack_poll_config(frame->payload, frame->len, &cfg_msg)) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_POLL_CONFIG, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (cfg_msg.polling_period_ms < POLL_PERIOD_MIN_MS || cfg_msg.polling_period_ms > POLL_PERIOD_MAX_MS) {
        ESP_LOGW(TAG, "POLL_CONFIG_ERROR: polling_period_ms=%u out of range", (unsigned)cfg_msg.polling_period_ms);
        pogo_nack_t nack = {.acked_type = POGO_MSG_POLL_CONFIG, .reason = POGO_NACK_POLL_CONFIG_RANGE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else {
        s_polling_period_ms = cfg_msg.polling_period_ms;
        pogo_ack_t ack = {.acked_type = POGO_MSG_POLL_CONFIG};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_POLL_SET;
        ESP_LOGI(TAG, "polling_period_ms=%u accepted", (unsigned)s_polling_period_ms);
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_set_dataset(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;

    if (s_state != CS_STATE_POLL_SET) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!thread_manager_validate_dataset_tlvs(frame->payload, frame->len)) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_INVALID_DATASET};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else {
        /* Stage into the real fields first so save_dynamic_record() writes
         * exactly what we're about to (maybe) join with, but only ACK / only
         * start a join if the write actually succeeded. */
        memcpy(s_dataset, frame->payload, frame->len);
        s_dataset_len = frame->len;
        if (save_dynamic_record() != ESP_OK) {
            s_dataset_len = 0; /* don't leave a half-applied dataset staged */
            pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_STORAGE_FAILED};
            resp_type = POGO_MSG_NACK;
            resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
        } else {
            pogo_ack_t ack = {.acked_type = POGO_MSG_SET_DATASET};
            resp_type = POGO_MSG_ACK;
            resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
            s_state = CS_STATE_JOINING;
        }
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);

    if (s_state == CS_STATE_JOINING) {
        module_role_t role = role_manager_get_role();
        thread_manager_role_t tm_role =
            (role == MODULE_ROLE_INTERMEDIATE) ? THREAD_MANAGER_ROLE_INTERMEDIATE : THREAD_MANAGER_ROLE_LEAF;
        s_last_joined_role = role;
        /* Dataset TLVs are never logged: they contain the Network Key. */
        ESP_LOGI(TAG, "dataset stored (%u bytes) -- starting Thread join as %s", (unsigned)s_dataset_len,
                 role_manager_role_to_str(role));
        if (thread_manager_join_with_dataset(tm_role, s_dataset, s_dataset_len, s_polling_period_ms) == ESP_OK) {
            s_join_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_JOIN_TIMEOUT_MS);
        } else {
            ESP_LOGE(TAG, "failed to apply dataset / start Thread join");
            s_state = CS_STATE_ERROR;
        }
    }
}

static void handle_frame(const pogo_frame_t *frame)
{
    switch (frame->type) {
    case POGO_MSG_PING:
        pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_PONG, frame->seq, NULL, 0);
        break;
    case POGO_MSG_IDENTIFY_REQ:
        handle_identify_req(frame);
        break;
    case POGO_MSG_ASSIGN_UUID:
        handle_assign_uuid(frame);
        break;
    case POGO_MSG_ASSIGN_SLOT:
        handle_assign_slot(frame);
        break;
    case POGO_MSG_SENSOR_HELLO:
        handle_sensor_hello(frame);
        break;
    case POGO_MSG_POLL_CONFIG:
        handle_poll_config(frame);
        break;
    case POGO_MSG_SET_DATASET:
        handle_set_dataset(frame);
        break;
    default:
        ESP_LOGW(TAG, "ignoring well-formed frame with unexpected type=0x%02x seq=%u", frame->type, frame->seq);
        break;
    }
}

static void child_service_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_state == CS_STATE_JOINING) {
            otDeviceRole role;
            if (thread_manager_poll_role_change(&role) &&
                (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER)) {
                ESP_LOGI(TAG, "Thread join succeeded, otDeviceRole=%u", role);
                send_join_result(true, role);
                s_state = CS_STATE_JOINED;
                role_manager_set_upstream_ready(true);
            } else if (s_join_deadline != 0 && (int32_t)(xTaskGetTickCount() - s_join_deadline) >= 0) {
                otDeviceRole cur = thread_manager_get_role();
                ESP_LOGW(TAG, "Thread join timed out, otDeviceRole=%u", cur);
                send_join_result(false, cur);
                s_state = CS_STATE_ERROR;
                s_join_deadline = 0;
            }
        } else if (s_state == CS_STATE_JOINED && s_role_for_join != s_last_joined_role &&
                   (s_role_for_join == MODULE_ROLE_LEAF || s_role_for_join == MODULE_ROLE_INTERMEDIATE)) {
            /* role_manager flipped between LEAF/INTERMEDIATE while already
             * joined (e.g. a module docked/undocked downstream) -- re-apply
             * link mode with the same dataset, no re-provisioning needed. */
            thread_manager_role_t tm_role =
                (s_role_for_join == MODULE_ROLE_INTERMEDIATE) ? THREAD_MANAGER_ROLE_INTERMEDIATE
                                                               : THREAD_MANAGER_ROLE_LEAF;
            ESP_LOGI(TAG, "role changed to %s while joined -- re-applying Thread link mode",
                     role_manager_role_to_str(s_role_for_join));
            thread_manager_join_with_dataset(tm_role, s_dataset, s_dataset_len, s_polling_period_ms);
            s_last_joined_role = s_role_for_join;
        }

        child_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        switch (evt.event) {
        case POGO_PORT_EVENT_ATTACHED:
            reset_session();
            break;
        case POGO_PORT_EVENT_DETACHED:
            /* child_service only invalidates the pairing session/dedup
             * state here -- it does not call thread_manager itself.
             * Stopping Thread on an UPSTREAM detach is module main's
             * responsibility (role_manager transitions to UNASSIGNED, whose
             * on_role_change handler calls thread_manager_stop(); see
             * module/main/app_main.c and docs/state-machine.md). */
            reset_session();
            role_manager_set_upstream_ready(false);
            break;
        case POGO_PORT_EVENT_PEER_READY:
            if (s_state == CS_STATE_WAIT_PEER) {
                s_state = CS_STATE_READY;
            }
            break;
        case POGO_PORT_EVENT_FRAME_RECEIVED:
            handle_frame(&evt.data.frame);
            break;
        case POGO_PORT_EVENT_ERROR:
            ESP_LOGW(TAG, "UPSTREAM port error=%d -> ERROR", (int)evt.data.error_code);
            s_state = CS_STATE_ERROR;
            break;
        }
    }
}

esp_err_t child_service_init(const module_config_t *cfg)
{
    s_cfg = cfg;
    s_sensor_ops = sensor_provider_lookup(cfg->driver_id);
    if (s_sensor_ops == NULL) {
        ESP_LOGE(TAG, "CONFIG_ERROR: no sensor_provider for driver_id=%u", (unsigned)cfg->driver_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = s_sensor_ops->init(cfg, &s_sensor_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor_provider '%s' init failed: %s", s_sensor_ops->name, esp_err_to_name(err));
        return err;
    }

    s_evt_queue = xQueueCreate(16, sizeof(child_evt_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    reset_session();

    BaseType_t ok = xTaskCreate(child_service_task, "child_service", 4096, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void child_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    if (port_id != POGO_PORT_UPSTREAM || s_evt_queue == NULL) {
        return;
    }
    child_evt_t evt = {.event = event, .data = data ? *data : (pogo_port_event_data_t){0}};
    xQueueSend(s_evt_queue, &evt, 0);
}

void child_service_on_role_change(module_role_t new_role, module_role_t old_role)
{
    (void)old_role;
    s_role_for_join = new_role;
}

uint8_t child_service_get_own_topology_depth(void)
{
    return s_topology_depth;
}
