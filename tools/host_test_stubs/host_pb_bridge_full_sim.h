/*
 * Test-control API for tools/host_test_stubs/host_pb_bridge_full_sim.c -- a
 * host stand-in for OpenThread CoAP that plays BOTH roles at once: the
 * SERVER side components/power_base_transport/power_base_transport.c
 * registers "/s"/"/t" resources against (like
 * tools/host_test_stubs/host_pb_transport_coap_sim.c), AND the CLIENT side
 * components/linux_bridge_exporter/linux_bridge_exporter.c uses to send its
 * own outgoing CoAP requests toward the (simulated) Linux host (like
 * tools/host_test_stubs/host_ot_coap_sim.c). Neither of those two existing
 * sims can be reused directly here: both define the exact same
 * openthread/coap.h symbol set, so linking two sims that both provide a
 * server AND both provide a client would collide, and this integration test
 * (tools/test_linux_bridge_exporter.c) needs BOTH roles live in the SAME
 * process at once -- an incoming "/t"/"/s" report drives
 * power_base_transport.c's real observer callbacks, which enqueue into
 * linux_bridge_exporter.c's real queue, which its own task then actually
 * "sends" (captured here) toward the simulated Linux host.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openthread/coap.h"
#include "openthread/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_PB_BRIDGE_FULL_SIM_MAX_PAYLOAD 300
/* Raised alongside MAX_QUEUED (host_pb_bridge_full_sim.c) for this
 * follow-up's correction pass (item 7: real timer test) -- see that file's
 * own comment. */
#define HOST_PB_BRIDGE_FULL_SIM_MAX_CAPTURED 512

/* Clears every registered resource, queued client outcome, and captured
 * outgoing send. Call at the start of every scenario. */
void host_pb_bridge_full_sim_reset(void);

/*
 * SERVER side: simulates one incoming CoAP request of `type` to `uri_path`
 * ("s" or "t") carrying `payload`/`len`, dispatched to whichever resource
 * power_base_transport_init() registered for that path. Returns false if no
 * resource for that path was ever registered (a test-setup bug).
 * out_response_sent/out_response_code report what the real handler's own
 * send_coap_response() actually did -- see
 * tools/host_test_stubs/host_pb_transport_coap_sim.h's identical contract.
 */
bool host_pb_bridge_full_sim_dispatch_incoming(const char *uri_path, otCoapType type, const uint8_t *payload,
                                                uint16_t len, bool *out_response_sent, otCoapCode *out_response_code);

/*
 * CLIENT side: queues the outcome of the NEXT otCoapSendRequestWithParameters()
 * call linux_bridge_exporter.c's own send_one_attempt() makes toward the
 * simulated Linux host -- same semantics as
 * tools/host_test_stubs/host_ot_coap_sim.h's identically-named functions
 * (this sim deliberately does not support DELAYED_RESPONSE; this component
 * has no SED-timing scenario to exercise that way).
 */
void host_pb_bridge_full_sim_queue_response(otError transport_result, otCoapCode response_code);
void host_pb_bridge_full_sim_queue_send_failure(otError send_result);
void host_pb_bridge_full_sim_queue_no_response(void);

/* Outcome for every send made once the explicit queue above is exhausted:
 * "Linux host reachable, answers response_code" or "Linux host unreachable,
 * no response at all". Without either call, an exhausted queue aborts (an
 * under-queued scenario is a test bug) -- used by multi-phase tests
 * (tools/test_linux_bridge_exporter_reboot_removal.c) where the exact number
 * of periodic sends per phase is not the point. */
void host_pb_bridge_full_sim_set_default_response(otCoapCode response_code);
void host_pb_bridge_full_sim_set_default_no_response(void);

/* How many outgoing otCoapSendRequestWithParameters() calls have actually
 * been made since the last reset. */
int host_pb_bridge_full_sim_send_count(void);

typedef struct {
    char uri_path[80];
    uint8_t payload[HOST_PB_BRIDGE_FULL_SIM_MAX_PAYLOAD];
    uint16_t payload_len;
    /* Where the exporter addressed this send (otMessageInfo.mPeerAddr /
     * mPeerPort), so a test can tell WHICH host it would have reached. */
    uint8_t peer_addr[16];
    uint16_t peer_port;
} host_pb_bridge_full_sim_sent_t;

/* How many outgoing sends have been captured (bounded by
 * HOST_PB_BRIDGE_FULL_SIM_MAX_CAPTURED -- a test-setup bug if exceeded, not
 * a runtime condition, so this aborts rather than silently truncating). */
size_t host_pb_bridge_full_sim_captured_count(void);
/* Returns false if index is out of range. */
bool host_pb_bridge_full_sim_get_captured(size_t index, host_pb_bridge_full_sim_sent_t *out);

/* Called (when set) from inside every outgoing send, after it was captured
 * and BEFORE its outcome is delivered -- "while the request is on the air".
 * Added for tools/test_linux_bridge_exporter_destination.c; never set (NULL)
 * it changes nothing, which is every other test. */
void host_pb_bridge_full_sim_set_send_hook(void (*hook)(const host_pb_bridge_full_sim_sent_t *sent));

#ifdef __cplusplus
}
#endif
