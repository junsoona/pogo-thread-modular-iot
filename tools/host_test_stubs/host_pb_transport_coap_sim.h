/*
 * Test-control API for tools/host_test_stubs/host_pb_transport_coap_sim.c --
 * the host stand-in for the real OpenThread CoAP SERVER side that
 * components/power_base_transport/power_base_transport.c registers its "/s"
 * and "/t" resources against. Unlike host_ot_coap_sim.c (a CLIENT-side sim
 * for sensor_transport.c/topology_reporter.c, whose otMessage carries no
 * real payload since those tests never need to read one back), this sim's
 * otMessage DOES carry a real payload buffer -- power_base_transport.c's own
 * handlers otMessageRead() the incoming request's bytes, which a
 * payload-less otMessage could never exercise. The two sims are never linked
 * into the same test binary (both define the same openthread/coap.h symbols).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "openthread/coap.h"
#include "openthread/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clears every registered resource and the last-captured response. Call at
 * the start of every scenario. */
void host_pb_coap_sim_reset(void);

/*
 * Simulates one incoming CoAP request of `type` (CONFIRMABLE/NON_CONFIRMABLE)
 * to `uri_path` ("s" or "t") carrying `payload`/`len` as its exact body,
 * dispatching to whichever resource power_base_transport_init() registered
 * for that path via the real otCoapAddResource() this sim backs. Returns
 * true if a resource for that path was found (and thus actually invoked)
 * -- false is a test-setup bug (path never registered), not a runtime
 * result to assert on.
 *
 * *out_response_sent is set to whether a CoAP response was actually sent
 * (false for a NON_CONFIRMABLE request the handler correctly never
 * responded to -- see power_base_transport.c's send_coap_response()).
 * *out_response_code is only meaningful when *out_response_sent is true.
 */
bool host_pb_coap_sim_dispatch(const char *uri_path, otCoapType type, const uint8_t *payload, uint16_t len,
                                bool *out_response_sent, otCoapCode *out_response_code);

#ifdef __cplusplus
}
#endif
