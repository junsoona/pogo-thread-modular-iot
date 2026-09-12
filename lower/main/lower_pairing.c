#include "lower_pairing.h"
#include "lower_storage.h"
#include "lower_thread.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "pogo_proto/pogo_transport.h"

static const char *TAG = "lower_pairing";

typedef enum {
    LOWER_STATE_UNPROVISIONED = 0,
    LOWER_STATE_IDENTIFIED,
    LOWER_STATE_ASSIGNED,
    LOWER_STATE_DATASET_STORED,
    LOWER_STATE_JOINING,
    LOWER_STATE_JOINED,
    LOWER_STATE_ERROR,
} lower_state_t;

/* Firmware build tag reported in IDENTIFY_RESP. Bump manually; this MVP has
 * no automated versioning. */
#define LOWER_FW_VERSION 1

/*
 * Dedup table for the three "write" message types, indexed by
 * dedup_index_for(). If the same (type, seq) is seen again -- e.g. the
 * upper module retransmitted after a lost ACK -- the cached response is
 * resent verbatim instead of re-running the side effect (re-assigning the
 * UUID/slot or rewriting NVS a second time).
 */
typedef struct {
    bool valid;
    uint8_t seq;
    uint8_t resp_type;
    uint8_t resp_payload[2];
    size_t resp_len;
} dedup_entry_t;

static dedup_entry_t s_dedup[3];

static int dedup_index_for(uint8_t msg_type)
{
    switch (msg_type) {
    case POGO_MSG_ASSIGN_UUID:
        return 0;
    case POGO_MSG_ASSIGN_SLOT:
        return 1;
    case POGO_MSG_SET_DATASET:
        return 2;
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
    pogo_transport_send(s_dedup[idx].resp_type, seq, s_dedup[idx].resp_payload, s_dedup[idx].resp_len);
    return true;
}

static void dedup_remember(int idx, uint8_t seq, uint8_t resp_type, const uint8_t *resp_payload, size_t resp_len)
{
    if (idx < 0) {
        return;
    }
    s_dedup[idx].valid = true;
    s_dedup[idx].seq = seq;
    s_dedup[idx].resp_type = resp_type;
    s_dedup[idx].resp_len = resp_len;
    memcpy(s_dedup[idx].resp_payload, resp_payload, resp_len);
}

static void send_join_result(bool success, otDeviceRole role)
{
    pogo_join_result_t result = {.success = success ? 1 : 0, .role = (uint8_t)role};
    uint8_t payload[2];
    size_t len = pogo_pack_join_result(&result, payload, sizeof(payload));
    /* Unsolicited notification (not a reply to any specific request), so
     * seq is not meaningful here; the upper side matches on type alone. */
    pogo_transport_send(POGO_MSG_JOIN_RESULT, 0, payload, len);
}

static void lower_pairing_task(void *arg)
{
    (void)arg;

    lower_state_t state = LOWER_STATE_UNPROVISIONED;
    bool has_uuid = false, has_slot = false;
    uint8_t uuid[POGO_UUID_LEN] = {0};
    uint8_t slot_id = 0;
    uint8_t dataset[POGO_FRAME_MAX_PAYLOAD];
    uint16_t dataset_len = 0;
    TickType_t join_deadline = 0;

    /* Recover from a reboot while already provisioned (e.g. power cycle
     * while still docked, or a reboot after a previous successful join). */
    if (lower_storage_load_provision(uuid, &slot_id, dataset, sizeof(dataset), &dataset_len) == ESP_OK) {
        has_uuid = true;
        has_slot = true;
        ESP_LOGI(TAG, "restored provisioning from NVS (slot=%u, dataset=%u bytes) -- rejoining", slot_id,
                 (unsigned)dataset_len);
        if (lower_thread_apply_dataset_and_join(dataset, dataset_len) == ESP_OK) {
            state = LOWER_STATE_JOINING;
            join_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_JOIN_TIMEOUT_MS);
        } else {
            ESP_LOGE(TAG, "failed to re-apply stored dataset");
            state = LOWER_STATE_ERROR;
        }
    }

    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);

    while (1) {
        /* Non-blocking checks that must keep running even while no UART
         * byte has arrived: role-change notifications and the join
         * timeout. */
        if (state == LOWER_STATE_JOINING) {
            otDeviceRole role;
            if (lower_thread_poll_role_change(&role) &&
                (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER)) {
                ESP_LOGI(TAG, "Thread join succeeded, otDeviceRole=%u", role);
                send_join_result(true, role);
                state = LOWER_STATE_JOINED;
            } else if (join_deadline != 0 && (int32_t)(xTaskGetTickCount() - join_deadline) >= 0) {
                otDeviceRole cur_role = lower_thread_get_role();
                ESP_LOGW(TAG, "Thread join timed out, otDeviceRole=%u", cur_role);
                send_join_result(false, cur_role);
                state = LOWER_STATE_ERROR;
                join_deadline = 0;
            }
        }

        uint8_t byte;
        int n = pogo_transport_read(&byte, 1, pdMS_TO_TICKS(20));
        if (n != 1) {
            continue; /* timeout: loop back to re-check join state above */
        }

        pogo_frame_t frame;
        if (!pogo_frame_parser_feed(&parser, byte, &frame)) {
            continue;
        }

        switch (frame.type) {

        case POGO_MSG_PING:
            /* Always answered, in any state: this is the docking-detection
             * heartbeat, not a provisioning step. */
            pogo_transport_send(POGO_MSG_PONG, frame.seq, NULL, 0);
            break;

        case POGO_MSG_IDENTIFY_REQ: {
            pogo_identify_resp_t resp = {0};
            esp_efuse_mac_get_default(resp.mac);
            resp.fw_version = LOWER_FW_VERSION;
            resp.sensor_bitmap = 0; /* reserved: sensor discovery is future work */
            uint8_t payload[8];
            size_t len = pogo_pack_identify_resp(&resp, payload, sizeof(payload));
            pogo_transport_send(POGO_MSG_IDENTIFY_RESP, frame.seq, payload, len);
            if (state == LOWER_STATE_UNPROVISIONED) {
                state = LOWER_STATE_IDENTIFIED;
            }
            break;
        }

        case POGO_MSG_ASSIGN_UUID: {
            int idx = dedup_index_for(frame.type);
            if (dedup_resend_if_repeat(idx, frame.seq)) {
                break;
            }
            pogo_assign_uuid_t msg;
            uint8_t resp_type, resp_payload[2];
            size_t resp_len;
            if (pogo_unpack_assign_uuid(frame.payload, frame.len, &msg)) {
                memcpy(uuid, msg.uuid, POGO_UUID_LEN);
                has_uuid = true;
                pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_UUID};
                resp_type = POGO_MSG_ACK;
                resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
                if (has_uuid && has_slot) {
                    state = LOWER_STATE_ASSIGNED;
                }
            } else {
                pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_BAD_STATE};
                resp_type = POGO_MSG_NACK;
                resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
            }
            pogo_transport_send(resp_type, frame.seq, resp_payload, resp_len);
            dedup_remember(idx, frame.seq, resp_type, resp_payload, resp_len);
            break;
        }

        case POGO_MSG_ASSIGN_SLOT: {
            int idx = dedup_index_for(frame.type);
            if (dedup_resend_if_repeat(idx, frame.seq)) {
                break;
            }
            pogo_assign_slot_t msg;
            uint8_t resp_type, resp_payload[2];
            size_t resp_len;
            if (pogo_unpack_assign_slot(frame.payload, frame.len, &msg)) {
                slot_id = msg.slot_id;
                has_slot = true;
                pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_SLOT};
                resp_type = POGO_MSG_ACK;
                resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
                if (has_uuid && has_slot) {
                    state = LOWER_STATE_ASSIGNED;
                }
            } else {
                pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_SLOT, .reason = POGO_NACK_BAD_STATE};
                resp_type = POGO_MSG_NACK;
                resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
            }
            pogo_transport_send(resp_type, frame.seq, resp_payload, resp_len);
            dedup_remember(idx, frame.seq, resp_type, resp_payload, resp_len);
            break;
        }

        case POGO_MSG_SET_DATASET: {
            int idx = dedup_index_for(frame.type);
            if (dedup_resend_if_repeat(idx, frame.seq)) {
                break;
            }
            uint8_t resp_type, resp_payload[2];
            size_t resp_len;

            if (!has_uuid || !has_slot) {
                pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_BAD_STATE};
                resp_type = POGO_MSG_NACK;
                resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
            } else if (!lower_thread_validate_dataset_tlvs(frame.payload, frame.len)) {
                pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_INVALID_DATASET};
                resp_type = POGO_MSG_NACK;
                resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
            } else if (lower_storage_save_provision(uuid, slot_id, frame.payload, frame.len) != ESP_OK) {
                pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_STORAGE_FAILED};
                resp_type = POGO_MSG_NACK;
                resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
            } else {
                memcpy(dataset, frame.payload, frame.len);
                dataset_len = frame.len;
                pogo_ack_t ack = {.acked_type = POGO_MSG_SET_DATASET};
                resp_type = POGO_MSG_ACK;
                resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
                state = LOWER_STATE_DATASET_STORED;
            }
            pogo_transport_send(resp_type, frame.seq, resp_payload, resp_len);
            dedup_remember(idx, frame.seq, resp_type, resp_payload, resp_len);

            if (state == LOWER_STATE_DATASET_STORED) {
                /* Dataset TLVs are never logged: they contain the Network Key. */
                ESP_LOGI(TAG, "dataset stored (%u bytes) -- starting Thread join as SED", (unsigned)dataset_len);
                if (lower_thread_apply_dataset_and_join(dataset, dataset_len) == ESP_OK) {
                    state = LOWER_STATE_JOINING;
                    join_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_JOIN_TIMEOUT_MS);
                } else {
                    ESP_LOGE(TAG, "failed to apply dataset / start Thread join");
                    state = LOWER_STATE_ERROR;
                }
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "ignoring well-formed frame with unknown type=0x%02x seq=%u", frame.type, frame.seq);
            break;
        }
    }
}

void lower_pairing_start(void)
{
    xTaskCreate(lower_pairing_task, "lower_pairing", 4096, NULL, 5, NULL);
}
