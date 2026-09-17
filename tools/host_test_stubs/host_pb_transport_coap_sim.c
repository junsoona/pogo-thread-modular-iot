/*
 * Host-only stand-in for the real OpenThread CoAP SERVER engine that
 * components/power_base_transport/power_base_transport.c registers its "/s"
 * and "/t" resources against (otCoapAddResource()) and replies through
 * (otCoapMessageInitResponse()/otCoapSendResponse()). See this file's own
 * header comment for why this is a SEPARATE sim from host_ot_coap_sim.c
 * (client-only, payload-less there; this one carries a real payload so
 * power_base_transport.c's otMessageRead() calls have real bytes to read).
 *
 * Never a reimplementation of power_base_transport.c's own handler/
 * observer/registry-diff logic -- that file links and runs completely
 * UNMODIFIED against this. See tools/test_power_base_transport_bridge_events.c
 * for the host test that drives it.
 */
#include "host_pb_transport_coap_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#define HOST_PB_COAP_SIM_MAX_RESOURCES 4
#define HOST_PB_COAP_SIM_MAX_PAYLOAD 300

struct otMessage {
    otCoapType type;
    otCoapCode code;
    uint8_t payload[HOST_PB_COAP_SIM_MAX_PAYLOAD];
    uint16_t payload_len;
};

static otCoapResource *s_resources[HOST_PB_COAP_SIM_MAX_RESOURCES];
static int s_resource_count;

static bool s_response_sent;
static otCoapCode s_response_code;

void host_pb_coap_sim_reset(void)
{
    s_resource_count = 0;
    memset(s_resources, 0, sizeof(s_resources));
    s_response_sent = false;
    s_response_code = 0;
}

bool host_pb_coap_sim_dispatch(const char *uri_path, otCoapType type, const uint8_t *payload, uint16_t len,
                                bool *out_response_sent, otCoapCode *out_response_code)
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
        if (len > HOST_PB_COAP_SIM_MAX_PAYLOAD) {
            fprintf(stderr, "host_pb_coap_sim: payload too large for the fixed sim buffer -- raise "
                            "HOST_PB_COAP_SIM_MAX_PAYLOAD\n");
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

/* ---- esp_openthread.h / esp_openthread_lock.h ---- */

otInstance *esp_openthread_get_instance(void)
{
    static int dummy_instance;
    return (otInstance *)&dummy_instance;
}

bool esp_openthread_lock_acquire(uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return true; /* a host test process has no separate OT task to lock against */
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
    if (s_resource_count >= HOST_PB_COAP_SIM_MAX_RESOURCES) {
        fprintf(stderr, "host_pb_coap_sim: too many resources registered -- raise HOST_PB_COAP_SIM_MAX_RESOURCES\n");
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
    (void)message;
    (void)uriPath;
    return OT_ERROR_NONE;
}

otError otCoapMessageSetPayloadMarker(otMessage *message)
{
    (void)message;
    return OT_ERROR_NONE;
}

otError otMessageAppend(otMessage *message, const void *data, uint16_t length)
{
    (void)message;
    (void)data;
    (void)length;
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
    otMessageFree(message); /* real OpenThread takes ownership and frees it once sent */
    return OT_ERROR_NONE;
}
