#include "upper_pairing.h"
#include "upper_thread.h"

#include "sdkconfig.h"
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "pogo_proto/pogo_transport.h"

static const char *TAG = "upper_pairing";

/* Single fixed slot in this MVP -- multi-lower-module slot allocation is
 * explicitly out of scope (see PROJECT-NOTES.md). */
#define FIXED_SLOT_ID 1

typedef enum {
    UPPER_STATE_IDLE = 0,
    UPPER_STATE_IDENTIFYING,
    UPPER_STATE_ASSIGNING,
    UPPER_STATE_PROVISIONING,
    UPPER_STATE_WAITING_JOIN,
    UPPER_STATE_JOINED,
} upper_state_t;

/*
 * Blocks (up to timeout_ticks) reading bytes from the transport into a
 * fresh, call-scoped frame parser until one complete, CRC-valid frame is
 * assembled. Known MVP simplification: bytes of a frame that straddles the
 * boundary between two calls to this function are lost -- acceptable here
 * because the protocol is strictly half-duplex request/response with
 * generous timeouts, never overlapping sends.
 */
static bool recv_frame(pogo_frame_t *out, TickType_t timeout_ticks)
{
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);

    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        uint8_t byte;
        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t remaining = (elapsed < timeout_ticks) ? (timeout_ticks - elapsed) : 0;
        int n = pogo_transport_read(&byte, 1, remaining);
        if (n == 1) {
            if (pogo_frame_parser_feed(&parser, byte, out)) {
                return true;
            }
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return false;
}

/*
 * Sends one request (seq/type/payload) and waits for a same-seq response of
 * expect_type_a or expect_type_b (0 = "don't care", used when only one
 * response type is valid). Retries up to max_retry times on timeout or a
 * mismatched reply. Returns true and fills *out_frame on success.
 */
static bool send_and_wait(uint8_t seq, uint8_t send_type, const uint8_t *payload, uint16_t len,
                           uint8_t expect_type_a, uint8_t expect_type_b, pogo_frame_t *out_frame, int max_retry,
                           TickType_t per_try_timeout)
{
    for (int attempt = 0; attempt <= max_retry; attempt++) {
        esp_err_t send_err = pogo_transport_send(send_type, seq, payload, len);
        if (send_err != ESP_OK) {
            ESP_LOGW(TAG, "send(type=0x%02x) failed: %s", send_type, esp_err_to_name(send_err));
        } else if (recv_frame(out_frame, per_try_timeout)) {
            if (out_frame->seq == seq &&
                (out_frame->type == expect_type_a || (expect_type_b != 0 && out_frame->type == expect_type_b))) {
                return true;
            }
            ESP_LOGW(TAG, "unexpected reply type=0x%02x seq=%u while waiting for type=0x%02x/0x%02x seq=%u",
                     out_frame->type, out_frame->seq, expect_type_a, expect_type_b, seq);
        }
        if (attempt < max_retry) {
            ESP_LOGW(TAG, "retry %d/%d for request type=0x%02x", attempt + 1, max_retry, send_type);
        }
    }
    return false;
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

static void upper_pairing_task(void *arg)
{
    (void)arg;
    upper_state_t state = UPPER_STATE_IDLE;
    uint8_t seq = 0;
    uint8_t assigned_uuid[POGO_UUID_LEN] = {0};
    int join_ping_misses = 0;

    const TickType_t ack_timeout = pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
    const TickType_t join_timeout = pdMS_TO_TICKS(CONFIG_POGO_JOIN_TIMEOUT_MS);
    const int max_retry = CONFIG_POGO_MAX_RETRY;

    while (1) {
        switch (state) {

        case UPPER_STATE_IDLE: {
            pogo_frame_t frame;
            seq++;
            if (send_and_wait(seq, POGO_MSG_PING, NULL, 0, POGO_MSG_PONG, 0, &frame, /*max_retry=*/0, ack_timeout)) {
                ESP_LOGI(TAG, "PONG received -> DOCKED");
                state = UPPER_STATE_IDENTIFYING;
            } else {
                vTaskDelay(pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS));
            }
            break;
        }

        case UPPER_STATE_IDENTIFYING: {
            pogo_frame_t frame;
            seq++;
            if (send_and_wait(seq, POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, 0, &frame, max_retry,
                               ack_timeout)) {
                pogo_identify_resp_t resp;
                if (pogo_unpack_identify_resp(frame.payload, frame.len, &resp)) {
                    ESP_LOGI(TAG, "lower identified: mac=%02x:%02x:%02x:%02x:%02x:%02x fw=%u sensor_bitmap=0x%02x",
                             resp.mac[0], resp.mac[1], resp.mac[2], resp.mac[3], resp.mac[4], resp.mac[5],
                             resp.fw_version, resp.sensor_bitmap);
                }
                esp_fill_random(assigned_uuid, POGO_UUID_LEN);
                state = UPPER_STATE_ASSIGNING;
            } else {
                ESP_LOGW(TAG, "identify failed after %d retries -> IDLE", max_retry);
                state = UPPER_STATE_IDLE;
            }
            break;
        }

        case UPPER_STATE_ASSIGNING: {
            pogo_frame_t frame = {0};
            bool ok = false;

            pogo_assign_uuid_t uuid_msg;
            memcpy(uuid_msg.uuid, assigned_uuid, POGO_UUID_LEN);
            uint8_t uuid_payload[POGO_UUID_LEN];
            size_t uuid_len = pogo_pack_assign_uuid(&uuid_msg, uuid_payload, sizeof(uuid_payload));

            seq++;
            if (send_and_wait(seq, POGO_MSG_ASSIGN_UUID, uuid_payload, uuid_len, POGO_MSG_ACK, POGO_MSG_NACK, &frame,
                               max_retry, ack_timeout) &&
                frame.type == POGO_MSG_ACK) {
                pogo_assign_slot_t slot_msg = {.slot_id = FIXED_SLOT_ID, .topology_depth = 1};
                uint8_t slot_payload[2];
                size_t slot_len = pogo_pack_assign_slot(&slot_msg, slot_payload, sizeof(slot_payload));

                seq++;
                if (send_and_wait(seq, POGO_MSG_ASSIGN_SLOT, slot_payload, slot_len, POGO_MSG_ACK, POGO_MSG_NACK,
                                   &frame, max_retry, ack_timeout) &&
                    frame.type == POGO_MSG_ACK) {
                    ok = true;
                }
            }
            log_nack_if_any(&frame);

            if (ok) {
                ESP_LOGI(TAG, "uuid+slot assigned (slot=%d)", FIXED_SLOT_ID);
                state = UPPER_STATE_PROVISIONING;
            } else {
                ESP_LOGW(TAG, "uuid/slot assignment failed -> IDLE");
                state = UPPER_STATE_IDLE;
            }
            break;
        }

        case UPPER_STATE_PROVISIONING: {
            otOperationalDatasetTlvs tlvs;
            if (upper_thread_get_active_dataset_tlvs(&tlvs) != ESP_OK) {
                ESP_LOGE(TAG, "failed to read our own active dataset TLVs -> IDLE");
                state = UPPER_STATE_IDLE;
                break;
            }

            pogo_frame_t frame = {0};
            seq++;
            if (send_and_wait(seq, POGO_MSG_SET_DATASET, tlvs.mTlvs, tlvs.mLength, POGO_MSG_ACK, POGO_MSG_NACK,
                               &frame, max_retry, ack_timeout) &&
                frame.type == POGO_MSG_ACK) {
                /* Dataset bytes are not logged: they contain the Network Key. */
                ESP_LOGI(TAG, "dataset accepted (%u bytes) -- waiting for join result (timeout %dms)",
                         (unsigned)tlvs.mLength, CONFIG_POGO_JOIN_TIMEOUT_MS);
                state = UPPER_STATE_WAITING_JOIN;
            } else {
                log_nack_if_any(&frame);
                ESP_LOGW(TAG, "dataset provisioning failed -> IDLE");
                state = UPPER_STATE_IDLE;
            }
            break;
        }

        case UPPER_STATE_WAITING_JOIN: {
            pogo_frame_t frame;
            if (recv_frame(&frame, join_timeout) && frame.type == POGO_MSG_JOIN_RESULT) {
                pogo_join_result_t result;
                if (pogo_unpack_join_result(frame.payload, frame.len, &result) && result.success) {
                    ESP_LOGI(TAG, "JOIN OK -- lower otDeviceRole=%u", result.role);
                    join_ping_misses = 0;
                    state = UPPER_STATE_JOINED;
                } else {
                    ESP_LOGW(TAG, "JOIN FAILED -- lower otDeviceRole=%u -> IDLE", result.role);
                    state = UPPER_STATE_IDLE;
                }
            } else {
                ESP_LOGW(TAG, "timed out waiting for join result -> IDLE");
                state = UPPER_STATE_IDLE;
            }
            break;
        }

        case UPPER_STATE_JOINED: {
            /* Terminal state for this vertical slice. Keep pinging at the
             * normal interval so an undock can be noticed and a redock can
             * retrigger the whole flow; sensor reporting is future work. */
            vTaskDelay(pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS));
            pogo_frame_t frame;
            seq++;
            if (send_and_wait(seq, POGO_MSG_PING, NULL, 0, POGO_MSG_PONG, 0, &frame, /*max_retry=*/0, ack_timeout)) {
                join_ping_misses = 0;
            } else if (++join_ping_misses >= max_retry) {
                ESP_LOGW(TAG, "lost contact with lower module (%d missed pings) -- assuming undocked", join_ping_misses);
                join_ping_misses = 0;
                state = UPPER_STATE_IDLE;
            }
            break;
        }
        }
    }
}

void upper_pairing_start(void)
{
    xTaskCreate(upper_pairing_task, "upper_pairing", 4096, NULL, 5, NULL);
}
