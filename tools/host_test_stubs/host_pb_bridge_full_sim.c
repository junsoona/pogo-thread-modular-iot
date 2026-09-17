/*
 * See host_pb_bridge_full_sim.h for the full design rationale. Never a
 * reimplementation of power_base_transport.c's or linux_bridge_exporter.c's
 * own logic -- both link and run completely UNMODIFIED against this.
 */
#include "host_pb_bridge_full_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#define MAX_RESOURCES 4
#define MAX_QUEUED 64

typedef enum {
    OUTCOME_SEND_FAILURE,
    OUTCOME_IMMEDIATE_RESPONSE,
    OUTCOME_NO_RESPONSE,
} outcome_kind_t;

typedef struct {
    outcome_kind_t kind;
    otError err;
    otCoapCode code;
} outcome_entry_t;

struct otMessage {
    otCoapType type;
    otCoapCode code;
    char uri_path[80];
    uint8_t payload[HOST_PB_BRIDGE_FULL_SIM_MAX_PAYLOAD];
    uint16_t payload_len;
};

static otCoapResource *s_resources[MAX_RESOURCES];
static int s_resource_count;

static bool s_response_sent;
static otCoapCode s_response_code;

static outcome_entry_t s_queue[MAX_QUEUED];
static int s_queue_len;
static int s_queue_pos;
static int s_send_count;

static host_pb_bridge_full_sim_sent_t s_captured[HOST_PB_BRIDGE_FULL_SIM_MAX_CAPTURED];
static size_t s_captured_count;

void host_pb_bridge_full_sim_reset(void)
{
    s_resource_count = 0;
    memset(s_resources, 0, sizeof(s_resources));
    s_response_sent = false;
    s_response_code = 0;
    s_queue_len = 0;
    s_queue_pos = 0;
    s_send_count = 0;
    s_captured_count = 0;
    memset(s_captured, 0, sizeof(s_captured));
}

bool host_pb_bridge_full_sim_dispatch_incoming(const char *uri_path, otCoapType type, const uint8_t *payload,
                                                uint16_t len, bool *out_response_sent, otCoapCode *out_response_code)
{
    otCoapResource *res = NULL;
    for (int i = 0; i < s_resource_count; i++) {
        if (strcmp(s_resources[i]->mUriPath, uri_path) == 0) {
            res = s_resources[i];
            break;
        }
    }
    if (res == NULL) {
        return false;
    }

    struct otMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    if (len > 0) {
        if (len > HOST_PB_BRIDGE_FULL_SIM_MAX_PAYLOAD) {
            fprintf(stderr, "host_pb_bridge_full_sim: incoming payload too large for the fixed sim buffer\n");
            abort();
        }
        memcpy(msg.payload, payload, len);
    }
    msg.payload_len = len;

    otMessageInfo info;
    memset(&info, 0, sizeof(info));

    s_response_sent = false;
    s_response_code = 0;
    res->mHandler(res->mContext, &msg, &info);

    *out_response_sent = s_response_sent;
    *out_response_code = s_response_code;
    return true;
}

static outcome_entry_t *push_outcome(void)
{
    if (s_queue_len >= MAX_QUEUED) {
        fprintf(stderr, "host_pb_bridge_full_sim: outcome queue full -- raise MAX_QUEUED\n");
        abort();
    }
    outcome_entry_t *e = &s_queue[s_queue_len++];
    e->err = OT_ERROR_NONE;
    e->code = 0;
    return e;
}

void host_pb_bridge_full_sim_queue_response(otError transport_result, otCoapCode response_code)
{
    outcome_entry_t *e = push_outcome();
    e->kind = OUTCOME_IMMEDIATE_RESPONSE;
    e->err = transport_result;
    e->code = response_code;
}

void host_pb_bridge_full_sim_queue_send_failure(otError send_result)
{
    outcome_entry_t *e = push_outcome();
    e->kind = OUTCOME_SEND_FAILURE;
    e->err = send_result;
}

void host_pb_bridge_full_sim_queue_no_response(void)
{
    push_outcome()->kind = OUTCOME_NO_RESPONSE;
}

int host_pb_bridge_full_sim_send_count(void)
{
    return s_send_count;
}

size_t host_pb_bridge_full_sim_captured_count(void)
{
    return s_captured_count;
}

bool host_pb_bridge_full_sim_get_captured(size_t index, host_pb_bridge_full_sim_sent_t *out)
{
    if (index >= s_captured_count) {
        return false;
    }
    *out = s_captured[index];
    return true;
}

/* ---- esp_openthread.h / esp_openthread_lock.h ---- */

otInstance *esp_openthread_get_instance(void)
{
    static int dummy_instance;
    return (otInstance *)&dummy_instance;
}

bool esp_openthread_lock_acquire(uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return true;
}

void esp_openthread_lock_release(void)
{
}

const char *otThreadErrorToString(otError error)
{
    switch (error) {
    case OT_ERROR_NONE:
        return "None";
    case OT_ERROR_FAILED:
        return "Failed";
    case OT_ERROR_DROP:
        return "Drop";
    case OT_ERROR_NO_BUFS:
        return "NoBufs";
    case OT_ERROR_NO_ROUTE:
        return "NoRoute";
    case OT_ERROR_BUSY:
        return "Busy";
    case OT_ERROR_PARSE:
        return "Parse";
    case OT_ERROR_INVALID_ARGS:
        return "InvalidArgs";
    default:
        return "UnknownError";
    }
}

/* ---- openthread/coap.h ---- */

otError otCoapStart(otInstance *instance, uint16_t port)
{
    (void)instance;
    (void)port;
    return OT_ERROR_NONE;
}

void otCoapAddResource(otInstance *instance, otCoapResource *resource)
{
    (void)instance;
    if (s_resource_count >= MAX_RESOURCES) {
        fprintf(stderr, "host_pb_bridge_full_sim: too many resources registered -- raise MAX_RESOURCES\n");
        abort();
    }
    s_resources[s_resource_count++] = resource;
}

otMessage *otCoapNewMessage(otInstance *instance, const void *settings)
{
    (void)instance;
    (void)settings;
    return (otMessage *)calloc(1, sizeof(otMessage));
}

void otMessageFree(otMessage *message)
{
    free(message);
}

void otCoapMessageInit(otMessage *message, otCoapType type, otCoapCode code)
{
    message->type = type;
    message->code = code;
}

otError otCoapMessageAppendUriPathOptions(otMessage *message, const char *uriPath)
{
    strncpy(message->uri_path, uriPath, sizeof(message->uri_path) - 1);
    message->uri_path[sizeof(message->uri_path) - 1] = '\0';
    return OT_ERROR_NONE;
}

otError otCoapMessageSetPayloadMarker(otMessage *message)
{
    (void)message;
    return OT_ERROR_NONE;
}

otError otMessageAppend(otMessage *message, const void *data, uint16_t length)
{
    if ((size_t)message->payload_len + length > HOST_PB_BRIDGE_FULL_SIM_MAX_PAYLOAD) {
        fprintf(stderr, "host_pb_bridge_full_sim: outgoing payload too large for the fixed sim buffer\n");
        abort();
    }
    memcpy(message->payload + message->payload_len, data, length);
    message->payload_len += length;
    return OT_ERROR_NONE;
}

otCoapCode otCoapMessageGetCode(const otMessage *message)
{
    return message->code;
}

otCoapType otCoapMessageGetType(const otMessage *message)
{
    return message->type;
}

uint16_t otMessageGetLength(const otMessage *message)
{
    return message->payload_len;
}

uint16_t otMessageGetOffset(const otMessage *message)
{
    (void)message;
    return 0;
}

uint16_t otMessageRead(const otMessage *message, uint16_t offset, void *buf, uint16_t length)
{
    if (offset >= message->payload_len) {
        return 0;
    }
    uint16_t available = (uint16_t)(message->payload_len - offset);
    uint16_t to_copy = (length < available) ? length : available;
    memcpy(buf, message->payload + offset, to_copy);
    return to_copy;
}

otError otCoapMessageInitResponse(otMessage *response, const otMessage *request, otCoapType type, otCoapCode code)
{
    (void)request;
    response->type = type;
    response->code = code;
    return OT_ERROR_NONE;
}

otError otCoapSendResponse(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo)
{
    (void)instance;
    (void)messageInfo;
    s_response_sent = true;
    s_response_code = message->code;
    otMessageFree(message);
    return OT_ERROR_NONE;
}

otError otCoapSendRequestWithParameters(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo,
                                         otCoapResponseHandler handler, void *context,
                                         const otCoapTxParameters *txParameters)
{
    (void)instance;
    (void)messageInfo;
    (void)txParameters;
    s_send_count++;

    if (s_captured_count >= HOST_PB_BRIDGE_FULL_SIM_MAX_CAPTURED) {
        fprintf(stderr, "host_pb_bridge_full_sim: captured-send buffer full -- raise "
                        "HOST_PB_BRIDGE_FULL_SIM_MAX_CAPTURED\n");
        abort();
    }
    host_pb_bridge_full_sim_sent_t *cap = &s_captured[s_captured_count++];
    strncpy(cap->uri_path, message->uri_path, sizeof(cap->uri_path) - 1);
    cap->uri_path[sizeof(cap->uri_path) - 1] = '\0';
    memcpy(cap->payload, message->payload, message->payload_len);
    cap->payload_len = message->payload_len;

    if (s_queue_pos >= s_queue_len) {
        fprintf(stderr, "host_pb_bridge_full_sim: otCoapSendRequestWithParameters() call #%d with no queued "
                        "outcome -- the test scenario under-queued responses\n",
                s_send_count);
        abort();
    }
    outcome_entry_t *e = &s_queue[s_queue_pos++];

    if (e->kind == OUTCOME_SEND_FAILURE) {
        return e->err; /* caller (send_one_attempt()) frees the message itself on this path */
    }

    otMessageFree(message); /* "dispatched" -- real OpenThread now owns it */

    if (e->kind == OUTCOME_NO_RESPONSE) {
        return OT_ERROR_NONE;
    }

    /* OUTCOME_IMMEDIATE_RESPONSE: synchronous delivery, same reasoning as
     * host_ot_coap_sim.c's own identical design (see that file's header
     * comment for why this is safe/faithful to the real semaphore-based
     * waiter). */
    otMessage *response = NULL;
    if (e->err == OT_ERROR_NONE) {
        response = otCoapNewMessage(instance, NULL);
        response->code = e->code;
    }
    handler(context, response, messageInfo, e->err);
    if (response != NULL) {
        otMessageFree(response);
    }
    return OT_ERROR_NONE;
}
