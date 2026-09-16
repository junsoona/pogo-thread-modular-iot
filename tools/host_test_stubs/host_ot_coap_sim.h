/*
 * Test-control API for tools/host_test_stubs/host_ot_coap_sim.c -- the host
 * stand-in for the real OpenThread CoAP client/lock/instance that
 * components/sensor_transport/sensor_transport.c calls into
 * (esp_openthread_get_instance(), esp_openthread_lock_*(),
 * otCoapSendRequestWithParameters(), ...). See that .c file's own header
 * comment for the full design; this header is what a test includes to drive
 * it.
 *
 * A test queues exactly one outcome per otCoapSendRequestWithParameters()
 * call it expects sensor_transport.c to make (send_one_attempt() calls it
 * once per retry attempt) -- queueing too few for what the real retry loop
 * actually does is a test bug, not a runtime condition, so the sim aborts()
 * loudly rather than hanging on to a semaphore no callback will ever give.
 */
#pragma once

#include "openthread/coap.h"
#include "openthread/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clears the queue and the call counter. Call at the start of every scenario. */
void host_ot_coap_sim_reset(void);

/*
 * Queues a REAL response for the next otCoapSendRequestWithParameters()
 * call: the send itself "succeeds" (returns OT_ERROR_NONE, as it does on
 * real hardware once the request is handed to the radio), and the caller's
 * response handler is invoked synchronously -- see the .c file's header
 * comment for why synchronous delivery is faithful to sensor_transport.c's
 * own semaphore-based waiter -- with transport_result and this response
 * code. transport_result is normally OT_ERROR_NONE (a real CoAP response
 * arrived); a non-NONE value simulates the response handler firing with an
 * OpenThread-layer error and no message, exactly like a real ACK timeout
 * would report (response_code is ignored in that case).
 */
void host_ot_coap_sim_queue_response(otError transport_result, otCoapCode response_code);

/*
 * Queues a SEND-level failure for the next otCoapSendRequestWithParameters()
 * call: the function itself returns this otError immediately (never OT_ERROR_NONE)
 * and the response handler is never invoked at all -- mirrors
 * send_one_attempt()'s own "err != OT_ERROR_NONE" early-return path (e.g. a
 * real OT_ERROR_NO_BUFS from the radio driver being out of message buffers).
 */
void host_ot_coap_sim_queue_send_failure(otError send_result);

/*
 * This follow-up (SED CoAP response timing correction): queues a REAL
 * response for the next otCoapSendRequestWithParameters() call, same as
 * host_ot_coap_sim_queue_response() above, EXCEPT the response handler
 * fires from a detached background thread after `delay_ms` of REAL
 * wall-clock time has elapsed, instead of synchronously before the send
 * call even returns. This is what lets a test exercise the caller's own
 * xSemaphoreTake() wait for real (a genuine block-then-wake), including the
 * token-staleness guard against a callback that fires late (after the
 * caller already gave up and moved on to a NEXT attempt/token) -- see
 * sensor_transport.c's coap_response_handler() for that guard's own
 * comment. The send call itself still returns OT_ERROR_NONE immediately
 * (real hardware never blocks the caller's own send call on the response).
 */
void host_ot_coap_sim_queue_delayed_response(otError transport_result, otCoapCode response_code, uint32_t delay_ms);

/*
 * This follow-up: queues an attempt whose send() call succeeds (returns
 * OT_ERROR_NONE, call counted normally) but whose response handler is NEVER
 * invoked at all -- models a genuinely lost/unanswered response, so a test
 * can confirm the caller's OWN semaphore-wait timeout is what eventually
 * bounds this attempt (never a hang), independent of OpenThread ever
 * calling back with its own timeout error.
 */
void host_ot_coap_sim_queue_no_response(void);

/* How many times otCoapSendRequestWithParameters() has actually been called
 * since the last reset -- i.e. how many real "/s" (or "/t") wireless
 * attempts sensor_transport.c/topology_reporter.c actually made. */
int host_ot_coap_sim_send_count(void);

#ifdef __cplusplus
}
#endif
