/*
 * Host-compiled regression test for components/sensor_transport/sensor_transport.c's
 * per-sample retry loop re-checking the topology gate before every attempt
 * (this follow-up's fix: item 1-4 of the "보정" request -- re-check the gate
 * right before each send, abort the CURRENT sample's remaining retries the
 * instant a 4.04/4.12 response closes the gate, never send a further "/s"
 * request while the gate stays closed, and log that abort at DEBUG rather
 * than the "dropped after N attempts" WARN a genuine delivery failure gets).
 *
 * Links against the REAL, unmodified sensor_transport.c,
 * components/collector_endpoint/collector_endpoint.c,
 * components/topology_gate/topology_gate.c, components/node_calc/node_calc.c
 * and components/sensor_channel/sensor_channel.c -- sensor_transport.c's own
 * retry/backoff/gate logic runs completely unmodified. The only things NOT
 * real here are the OpenThread CoAP client + esp_openthread instance/lock
 * themselves (tools/host_test_stubs/host_ot_coap_sim.c, a stand-in for the
 * Thread-radio hardware dependency, same category as
 * host_thread_manager_sim.c already uses for thread_manager.c) and the one
 * sensor_service.h symbol sensor_transport.c's UNUSED-by-this-test
 * ROOT-side handler references (tools/host_test_stubs/host_sensor_service_root_stub.c).
 * See docs/power-base.md and this repository's other host_*_sim.c files for
 * why sensor_transport.c itself (real OpenThread/FreeRTOS calls) cannot be
 * host-compiled any more directly than that.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I components/sensor_transport/include \
 *      -I components/sensor_channel/include -I components/sensor_service/include \
 *      -I components/collector_endpoint/include -I components/node_calc/include \
 *      -I components/topology_gate/include -I components/module_config/include \
 *      -I components/sensor_provider/include -I components/role_manager/include \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -I components/sensor_root_aggregator/include \
 *      -o /tmp/test_sensor_transport_gate_retry tools/test_sensor_transport_gate_retry.c \
 *      components/sensor_transport/sensor_transport.c \
 *      components/collector_endpoint/collector_endpoint.c \
 *      components/topology_gate/topology_gate.c components/node_calc/node_calc.c \
 *      components/sensor_channel/sensor_channel.c \
 *      tools/host_test_stubs/host_ot_coap_sim.c tools/host_test_stubs/host_sensor_service_root_stub.c \
 *   && /tmp/test_sensor_transport_gate_retry
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "sensor_transport/sensor_transport.h"
#include "sensor_channel/sensor_channel.h"
#include "collector_endpoint/collector_endpoint.h"
#include "topology_gate/topology_gate.h"
#include "host_ot_coap_sim.h"
#include "openthread/coap.h"
#include "openthread/error.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

static void make_ml_eid(uint8_t out[COLLECTOR_ENDPOINT_ADDR_LEN], uint8_t last_byte)
{
    memset(out, 0, COLLECTOR_ENDPOINT_ADDR_LEN);
    out[0] = 0xfd;
    out[1] = 0x99;
    out[15] = last_byte;
}

static sensor_sample_t make_sample(uint8_t uuid_tag, uint32_t seq)
{
    sensor_sample_t s;
    memset(&s, 0, sizeof(s));
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    for (int i = 0; i < SENSOR_UUID_LEN; i++) {
        s.module_uuid[i] = (uint8_t)(uuid_tag * 0x11 + i);
    }
    s.sample_seq = seq;
    s.uptime_ms = 1000;
    s.sensor_bitmap = 0x0001;
    s.channel_count = 0; /* an empty channel list packs fine -- irrelevant to this test */
    return s;
}

/* Fresh, valid state before every scenario: a real collector endpoint set
 * (send_one_attempt() fails fast with none), and sensor_transport_init()
 * called exactly once per process (creates s_send_done_sem) -- see this
 * file's own main(). */
static void reset_gate_and_sim(void)
{
    host_ot_coap_sim_reset();
    topology_gate_init(); /* re-creates the event group -- starts closed, no pending reregister request */
}

static void scenario_first_send_while_open(void)
{
    printf("\n-- scenario: gate open -- the first \"/s\" request is actually sent --\n");
    reset_gate_and_sim();
    topology_gate_open();
    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);

    sensor_sample_t sample = make_sample(1, 1);
    sensor_transport_send_sample(&sample);

    CHECK(host_ot_coap_sim_send_count() == 1, "exactly one \"/s\" request was actually sent");
    CHECK(topology_gate_is_open(), "a successful 2.xx leaves the gate open");
}

static void scenario_first_4_12_closes_gate_and_requests_reregister(void)
{
    printf("\n-- scenario: first response is 4.12 -- gate closes and reregistration is requested --\n");
    reset_gate_and_sim();
    topology_gate_open();
    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_PRECONDITION_FAILED);

    sensor_sample_t sample = make_sample(1, 1);
    sensor_transport_send_sample(&sample);

    CHECK(!topology_gate_is_open(), "the gate closes on the first 4.12 response");
    CHECK(topology_gate_take_reregister_request(), "a re-registration request was set by the 4.12 reaction");
}

static void scenario_remaining_retries_not_sent_after_4_12(void)
{
    printf("\n-- scenario: after a 4.12 on attempt 1, the remaining 3 retries for THIS sample are never sent --\n");
    reset_gate_and_sim();
    topology_gate_open();
    /* Only ONE outcome queued: if sensor_transport.c's retry loop tried to
     * send a 2nd/3rd/4th attempt for this same sample, host_ot_coap_sim
     * would abort() on the unqueued call -- the test would fail loudly
     * instead of silently passing. */
    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_PRECONDITION_FAILED);

    sensor_sample_t sample = make_sample(2, 5);
    sensor_transport_send_sample(&sample);

    CHECK(host_ot_coap_sim_send_count() == 1, "exactly one attempt was made -- the other 3 retries were skipped");
}

static void scenario_no_further_samples_while_gate_closed(void)
{
    printf("\n-- scenario: while the gate stays closed, a LATER sample is never sent at all --\n");
    reset_gate_and_sim();
    topology_gate_open();
    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_PRECONDITION_FAILED);

    sensor_sample_t first = make_sample(3, 1);
    sensor_transport_send_sample(&first);
    CHECK(host_ot_coap_sim_send_count() == 1, "precondition: the first sample made exactly one attempt");
    CHECK(!topology_gate_is_open(), "precondition: the gate is now closed");

    /* No response queued for this call on purpose: if sensor_transport.c
     * attempted a "/s" send here despite the closed gate, host_ot_coap_sim
     * would abort() (empty queue) -- the strongest possible assertion that
     * no further request was made. */
    sensor_sample_t second = make_sample(3, 2);
    sensor_transport_send_sample(&second);
    CHECK(host_ot_coap_sim_send_count() == 1, "a later sample makes NO additional \"/s\" attempt while the gate is closed");
}

static void scenario_resumes_after_reregistration_succeeds(void)
{
    printf("\n-- scenario: once re-registration gets a real 2.xx (gate reopens), the NEXT sample sends normally --\n");
    reset_gate_and_sim();
    topology_gate_open();
    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_PRECONDITION_FAILED);

    sensor_sample_t rejected = make_sample(4, 1);
    sensor_transport_send_sample(&rejected);
    CHECK(!topology_gate_is_open(), "precondition: gate closed after the 4.12");

    /* Mirrors topology_reporter's own re-ATTACH: only a real CoAP 2.xx ever
     * opens the gate (topology_gate_open() is the ONLY place that happens,
     * same invariant tools/test_topology_gate.c already asserts) -- modeled
     * here directly since topology_reporter.c itself is not linked by this
     * test (same "cannot be host-compiled" category as sensor_transport.c;
     * its own gate-open/close behavior is already covered for real by
     * tools/test_topology_gate.c). */
    topology_gate_open();

    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
    sensor_sample_t resumed = make_sample(4, 2);
    sensor_transport_send_sample(&resumed);

    CHECK(host_ot_coap_sim_send_count() == 2, "the next sample after re-registration makes exactly one more real attempt");
    CHECK(topology_gate_is_open(), "the gate stays open after the resumed sample's own 2.xx");
}

/*
 * Item 4: the mid-retry gate-close abort must be logged at DEBUG, never the
 * "sample dropped after N attempts" WARN a genuine (non-gate) delivery
 * failure gets. Captures this process's own stderr (esp_log.h's host stub
 * writes "<LEVEL> (<tag>) <message>" lines there -- see
 * tools/host_test_stubs/esp_log.h) around one abort and asserts on the
 * captured text directly, rather than trusting a source-level read.
 */
static void scenario_abort_is_logged_at_debug_not_warn(void)
{
    printf("\n-- scenario: the mid-retry gate-close abort is logged at DEBUG, not WARN --\n");
    reset_gate_and_sim();
    topology_gate_open();
    host_ot_coap_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_PRECONDITION_FAILED);

    char path[] = "/tmp/host_sensor_transport_gate_retry_stderr.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        CHECK(false, "mkstemp() for stderr capture failed -- cannot run this scenario");
        return;
    }
    close(fd);

    fflush(stderr);
    FILE *captured = freopen(path, "w+", stderr);
    CHECK(captured != NULL, "stderr was redirected to a capture file");

    sensor_sample_t sample = make_sample(5, 1);
    sensor_transport_send_sample(&sample);

    fflush(stderr);

    /* Restore stderr to the real terminal/pipe before printing any more
     * CHECK() results, or they would vanish into the capture file too. */
    FILE *restored = freopen("/dev/tty", "w", stderr);
    if (restored == NULL) {
        /* No controlling TTY (e.g. piped output) -- fall back to fd 2 by
         * path, which still works under a redirect/pipe. */
        freopen("/dev/stderr", "w", stderr);
    }

    FILE *f = fopen(path, "r");
    char buf[8192];
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    buf[n] = '\0';
    unlink(path);

    CHECK(strstr(buf, "sample dropped after") == NULL,
          "the noisy \"sample dropped after N attempts\" WARN is NOT logged for a gate-close abort");
    CHECK(strstr(buf, "D (sensor_transport)") != NULL && strstr(buf, "aborting remaining retries") != NULL,
          "a DEBUG-level line documenting the abort IS logged instead");
}

int main(void)
{
    CHECK(collector_endpoint_init() == ESP_OK, "collector_endpoint_init() succeeds");
    uint8_t power_base_uuid[COLLECTOR_ENDPOINT_UUID_LEN];
    for (int i = 0; i < COLLECTOR_ENDPOINT_UUID_LEN; i++) {
        power_base_uuid[i] = (uint8_t)(0xB0 + i);
    }
    uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN];
    make_ml_eid(addr, 0x01);
    CHECK(collector_endpoint_set(power_base_uuid, addr, 5683, 1) == ESP_OK, "a valid collector endpoint is set");

    CHECK(sensor_transport_init(/*require_topology_registration=*/true) == ESP_OK, "sensor_transport_init(true) succeeds");

    scenario_first_send_while_open();
    scenario_first_4_12_closes_gate_and_requests_reregister();
    scenario_remaining_retries_not_sent_after_4_12();
    scenario_no_further_samples_while_gate_closed();
    scenario_resumes_after_reregistration_succeeds();
    scenario_abort_is_logged_at_debug_not_warn();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
