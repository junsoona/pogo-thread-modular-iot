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
 * This follow-up (SED CoAP response timing correction) adds two more
 * queueable outcomes, both genuinely asynchronous in real wall-clock time
 * rather than synchronous: a DELAYED response (the handler fires from a
 * detached background thread after a real sleep -- exercises the caller's
 * xSemaphoreTake() wait for real) and NO response at all (the handler is
 * simply never invoked -- exercises the caller's own timeout as the sole
 * backstop). See host_ot_coap_sim_queue_delayed_response()/
 * host_ot_coap_sim_queue_no_response() in host_ot_coap_sim.h.
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
#include <pthread.h>
#include <time.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#define HOST_OT_COAP_SIM_MAX_QUEUED 32

typedef enum {
    HOST_OT_COAP_SIM_SEND_FAILURE,
    HOST_OT_COAP_SIM_IMMEDIATE_RESPONSE,
    HOST_OT_COAP_SIM_DELAYED_RESPONSE, /* this follow-up: SED CoAP response timing correction */
    HOST_OT_COAP_SIM_NO_RESPONSE,      /* this follow-up: genuinely never calls the handler */
} host_ot_coap_sim_kind_t;

typedef struct {
    host_ot_coap_sim_kind_t kind;
    otError err;         /* send failure: the send call's own return value.
                             response/delayed: the transport result delivered to the
                             response handler (normally OT_ERROR_NONE). unused for
                             no-response. */
    otCoapCode code;      /* response code, meaningful only for
                             IMMEDIATE_RESPONSE/DELAYED_RESPONSE with err == OT_ERROR_NONE */
    uint32_t delay_ms;    /* meaningful only for DELAYED_RESPONSE */
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

static host_ot_coap_sim_entry_t *push_entry(void)
{
    if (s_queue_len >= HOST_OT_COAP_SIM_MAX_QUEUED) {
        fprintf(stderr, "host_ot_coap_sim: queue full -- raise HOST_OT_COAP_SIM_MAX_QUEUED\n");
        abort();
    }
    host_ot_coap_sim_entry_t *e = &s_queue[s_queue_len++];
    e->err = OT_ERROR_NONE;
    e->code = 0;
    e->delay_ms = 0;
    return e;
}

void host_ot_coap_sim_queue_response(otError transport_result, otCoapCode response_code)
{
    host_ot_coap_sim_entry_t *e = push_entry();
    e->kind = HOST_OT_COAP_SIM_IMMEDIATE_RESPONSE;
    e->err = transport_result;
    e->code = response_code;
}

void host_ot_coap_sim_queue_send_failure(otError send_result)
{
    host_ot_coap_sim_entry_t *e = push_entry();
    e->kind = HOST_OT_COAP_SIM_SEND_FAILURE;
    e->err = send_result;
}

void host_ot_coap_sim_queue_delayed_response(otError transport_result, otCoapCode response_code, uint32_t delay_ms)
{
    host_ot_coap_sim_entry_t *e = push_entry();
    e->kind = HOST_OT_COAP_SIM_DELAYED_RESPONSE;
    e->err = transport_result;
    e->code = response_code;
    e->delay_ms = delay_ms;
}

void host_ot_coap_sim_queue_no_response(void)
{
    host_ot_coap_sim_entry_t *e = push_entry();
    e->kind = HOST_OT_COAP_SIM_NO_RESPONSE;
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

/* Thread-arg for HOST_OT_COAP_SIM_DELAYED_RESPONSE -- see that entry kind's
 * own doc comment (host_ot_coap_sim.h). message_info is deliberately NOT
 * carried across (the caller's own stack-allocated otMessageInfo is long
 * gone by the time this thread wakes); sensor_transport.c's real
 * coap_response_handler() never dereferences its message_info parameter
 * anyway (see that function's own `(void)message_info;`). */
typedef struct {
    otCoapResponseHandler handler;
    void *context;
    otError err;
    otCoapCode code;
    uint32_t delay_ms;
} delayed_response_arg_t;

static void *delayed_response_thread(void *arg_v)
{
    delayed_response_arg_t *arg = (delayed_response_arg_t *)arg_v;
    struct timespec ts = {.tv_sec = arg->delay_ms / 1000, .tv_nsec = (long)(arg->delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);

    otMessage *response = NULL;
    if (arg->err == OT_ERROR_NONE) {
        response = (otMessage *)calloc(1, sizeof(otMessage));
        response->code = arg->code;
    }
    arg->handler(arg->context, response, NULL, arg->err);
    if (response != NULL) {
        free(response);
    }
    free(arg);
    return NULL;
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

    if (e->kind == HOST_OT_COAP_SIM_SEND_FAILURE) {
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

    if (e->kind == HOST_OT_COAP_SIM_NO_RESPONSE) {
        /* Send succeeds; the handler is deliberately never invoked -- see
         * this entry kind's own doc comment. */
        return OT_ERROR_NONE;
    }

    if (e->kind == HOST_OT_COAP_SIM_DELAYED_RESPONSE) {
        delayed_response_arg_t *arg = (delayed_response_arg_t *)malloc(sizeof(*arg));
        if (arg == NULL) {
            fprintf(stderr, "host_ot_coap_sim: malloc failed for a delayed response -- test infrastructure OOM\n");
            abort();
        }
        arg->handler = handler;
        arg->context = context;
        arg->err = e->err;
        arg->code = e->code;
        arg->delay_ms = e->delay_ms;
        pthread_t t;
        if (pthread_create(&t, NULL, delayed_response_thread, arg) != 0) {
            fprintf(stderr, "host_ot_coap_sim: pthread_create failed for a delayed response\n");
            abort();
        }
        pthread_detach(t);
        return OT_ERROR_NONE;
    }

    /* HOST_OT_COAP_SIM_IMMEDIATE_RESPONSE: unchanged synchronous delivery
     * (see this file's own top comment for why that is safe/faithful). */
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
