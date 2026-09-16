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

/* How many times otCoapSendRequestWithParameters() has actually been called
 * since the last reset -- i.e. how many real "/s" (or "/t") wireless
 * attempts sensor_transport.c/topology_reporter.c actually made. */
int host_ot_coap_sim_send_count(void);

#ifdef __cplusplus
}
#endif
