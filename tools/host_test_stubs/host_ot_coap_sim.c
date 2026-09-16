/*
 * Host-only stand-in for the real OpenThread CoAP client + esp_openthread
 * instance/lock that components/sensor_transport/sensor_transport.c calls
 * into (esp_openthread_get_instance(), esp_openthread_lock_acquire/release(),
 * otCoapNewMessage/otCoapMessageInit/.../otCoapSendRequestWithParameters()).
 * This is a stand-in for the OpenThread/Thread-radio CoAP engine dependency
 * itself -- the same category as tools/host_test_stubs/host_thread_manager_sim.c
 * stands in for thread_manager.c's own OpenThread calls -- never a
 * reimplementation of sensor_transport.c's own retry/gate/backoff logic,
 * which links and runs completely UNMODIFIED against this file. See
 * tools/test_sensor_transport_gate_retry.c for the host test that drives it.
 *
 * Synchronous "async" delivery: on real hardware,
 * otCoapSendRequestWithParameters() returns immediately and the response
 * handler fires later from the OpenThread task once a reply (or ACK
 * timeout) arrives. This stub instead calls the handler SYNCHRONOUSLY,
 * before otCoapSendRequestWithParameters() itself returns. That is safe and
 * behaviorally equivalent here because sensor_transport.c's own
 * coap_response_handler() only records the result and gives
 * s_send_done_sem -- send_one_attempt() (the sole caller) does not touch
 * s_send_done_sem again until AFTER otCoapSendRequestWithParameters() has
 * already returned, so a synchronous give is already pending (the binary
 * semaphore is "available") by the time send_one_attempt() reaches its own
 * xSemaphoreTake() a few lines later. The token-matching logic
 * (coap_response_handler()'s stale-callback guard) is exercised exactly as
 * on real hardware either way, since the token compare only depends on
 * values already assigned before the send call, never on timing.
 *
 * A test queues exactly one outcome per otCoapSendRequestWithParameters()
 * call it expects the real retry loop to make (host_ot_coap_sim.h). Calling
 * this stub's send function with an empty queue is treated as a test bug
 * (under-provisioned scenario), not a runtime condition to recover from --
 * it aborts() loudly with a clear message rather than silently hanging (no
 * real callback would ever arrive to unblock sensor_transport.c's wait).
 */
#include "host_ot_coap_sim.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#define HOST_OT_COAP_SIM_MAX_QUEUED 32

typedef struct {
    bool is_send_failure;
    otError err;         /* send failure: the send call's own return value.
                             response: the transport result delivered to the
                             response handler (normally OT_ERROR_NONE). */
    otCoapCode code;      /* response code, meaningful only when !is_send_failure
                             && err == OT_ERROR_NONE */
} host_ot_coap_sim_entry_t;

static host_ot_coap_sim_entry_t s_queue[HOST_OT_COAP_SIM_MAX_QUEUED];
static int s_queue_len;
static int s_queue_pos;
static int s_send_count;

struct otMessage {
    otCoapType type;
    otCoapCode code;
};

void host_ot_coap_sim_reset(void)
{
    s_queue_len = 0;
    s_queue_pos = 0;
    s_send_count = 0;
}

void host_ot_coap_sim_queue_response(otError transport_result, otCoapCode response_code)
{
    if (s_queue_len >= HOST_OT_COAP_SIM_MAX_QUEUED) {
        fprintf(stderr, "host_ot_coap_sim: queue full -- raise HOST_OT_COAP_SIM_MAX_QUEUED\n");
        abort();
    }
    s_queue[s_queue_len].is_send_failure = false;
    s_queue[s_queue_len].err = transport_result;
    s_queue[s_queue_len].code = response_code;
    s_queue_len++;
}

void host_ot_coap_sim_queue_send_failure(otError send_result)
{
    if (s_queue_len >= HOST_OT_COAP_SIM_MAX_QUEUED) {
        fprintf(stderr, "host_ot_coap_sim: queue full -- raise HOST_OT_COAP_SIM_MAX_QUEUED\n");
        abort();
    }
    s_queue[s_queue_len].is_send_failure = true;
    s_queue[s_queue_len].err = send_result;
    s_queue[s_queue_len].code = 0;
    s_queue_len++;
}

int host_ot_coap_sim_send_count(void)
{
    return s_send_count;
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
    (void)resource;
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
    (void)message;
    return 0;
}

uint16_t otMessageGetOffset(const otMessage *message)
{
    (void)message;
    return 0;
}

uint16_t otMessageRead(const otMessage *message, uint16_t offset, void *buf, uint16_t length)
{
    (void)message;
    (void)offset;
    (void)buf;
    (void)length;
    return 0;
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
    (void)message;
    (void)messageInfo;
    return OT_ERROR_NONE;
}

otError otCoapSendRequestWithParameters(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo,
                                         otCoapResponseHandler handler, void *context,
                                         const otCoapTxParameters *txParameters)
{
    (void)instance;
    (void)txParameters;
    s_send_count++;

    if (s_queue_pos >= s_queue_len) {
        fprintf(stderr, "host_ot_coap_sim: otCoapSendRequestWithParameters() call #%d with no queued outcome -- "
                        "the test scenario under-queued responses for the real retry loop\n",
                s_send_count);
        abort();
    }
    host_ot_coap_sim_entry_t *e = &s_queue[s_queue_pos++];

    if (e->is_send_failure) {
        /* The real API never hands the message to the network on this path
         * -- sensor_transport.c's own send_one_attempt() frees it itself
         * (`if (err != OT_ERROR_NONE) { otMessageFree(message); }`), so this
         * stub must NOT free it here too. */
        return e->err;
    }

    /* "Dispatched" -- real OpenThread now owns `message` and frees it once
     * the exchange concludes; this stub does the same right after reading
     * what it needs from it (nothing, in this simplified client-only sim). */
    otMessageFree(message);

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
