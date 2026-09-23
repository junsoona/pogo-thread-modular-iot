/*
 * Host-compiled integration test for this follow-up's correction pass on
 * components/linux_bridge_exporter/linux_bridge_exporter.c -- linked
 * against the REAL, unmodified linux_bridge_exporter.c/power_base_transport.c/
 * etc., exactly like tools/test_linux_bridge_exporter.c. Covers two related
 * fixes for the real-hardware "stale TOPOLOGY backlog" finding:
 *
 *   1. The explicit CoAP-response-based retry policy (send_one_attempt()/
 *      transmit()'s bare bool return replaced with export_send_result_t):
 *        - 2.xx (2.04 Changed, 2.03 Valid) -> EXPORT_SEND_DELIVERED_2XX, 1 attempt, success.
 *        - 4.xx (4.04 Not Found, 4.12 Precondition Failed) -> EXPORT_SEND_REJECTED_4XX, 1 attempt, FINAL, never retried.
 *        - 5.xx (5.03 Service Unavailable), timeout, OT send error -> EXPORT_SEND_TRANSIENT_FAILURE, bounded 1+2 retry.
 *   2. The topology queue carrying only a module_uuid (never a
 *      topology_registry_entry_t snapshot) plus its coalescing set: a
 *      queued event drains against whatever the registry's CURRENT state
 *      is at that moment, never a stale snapshot from when it was
 *      originally enqueued.
 *
 * A SEPARATE file (rather than more scenarios appended to
 * tools/test_linux_bridge_exporter.c) specifically so this file's own exact
 * send-count assertions ("exactly 1 attempt", "exactly 3 attempts, never a
 * 4th") get a full, fresh CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS/
 * HEARTBEAT_INTERVAL_MS (production 5000ms/10000ms, see
 * tools/host_test_stubs/pogo_fast/sdkconfig.h) budget of their own: this
 * file's scenarios below (each a short bounded-retry send plus a settle
 * sleep, or one ~1s bounded-retry blocker) together stay comfortably under
 * that 5000ms ceiling, so neither periodic timer can fire mid-scenario and
 * inject an extra, unexpected send that would corrupt an exact-count
 * assertion -- exactly the same reasoning tools/test_linux_bridge_exporter.c's
 * own top comment gives for its choice of intervals, just budgeted
 * separately since adding these scenarios to that file's own already-
 * substantial time budget (several ~1s bounded-retry blockers plus a 2.5s
 * settle sleep) would have pushed its total runtime past the 5000ms
 * ceiling.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_retry_policy tools/test_linux_bridge_exporter_retry_policy.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_retry_policy
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "linux_bridge_exporter/linux_bridge_exporter.h"
#include "power_base_transport/power_base_transport.h"
#include "bridge_export_proto/bridge_export_proto.h"
#include "host_pb_bridge_full_sim.h"

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

static const uint8_t POWER_BASE_UUID[TOPOLOGY_UUID_LEN] = {0xB0, 0, 0, 0, 0, 0, 0, 0x02};

static void make_uuid(uint8_t out[TOPOLOGY_UUID_LEN], uint8_t tag)
{
    memset(out, tag, TOPOLOGY_UUID_LEN);
}

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static bool wait_for_send_count(int expected, int timeout_ms)
{
    int waited = 0;
    while (host_pb_bridge_full_sim_send_count() < expected && waited < timeout_ms) {
        sleep_ms(5);
        waited += 5;
    }
    return host_pb_bridge_full_sim_send_count() >= expected;
}

static void send_topology_report_full(const uint8_t module_uuid[TOPOLOGY_UUID_LEN], topology_report_type_t type,
                                       uint32_t seq, bool downstream_connected, bool *out_sent, otCoapCode *out_code)
{
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, POWER_BASE_UUID, TOPOLOGY_UUID_LEN);
    rep.topology_depth = 1;
    rep.role = TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = 1;
    rep.sequence = seq;
    rep.upstream_connected = true;
    rep.downstream_connected = downstream_connected;
    rep.report_type = type;
    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

static void send_topology_report(const uint8_t module_uuid[TOPOLOGY_UUID_LEN], uint32_t seq, bool *out_sent,
                                  otCoapCode *out_code)
{
    send_topology_report_full(module_uuid, TOPOLOGY_REPORT_ATTACHED, seq, false, out_sent, out_code);
}

/* Decodes a captured send at `index` as a TOPOLOGY_SNAPSHOT and returns
 * true if it names `uuid` as its module_uuid, filling *out on success --
 * same contract as tools/test_linux_bridge_exporter.c's own helper of the
 * same name. */
static bool captured_is_topology_for(size_t index, const uint8_t uuid[TOPOLOGY_UUID_LEN],
                                      bridge_export_topology_node_t *out)
{
    host_pb_bridge_full_sim_sent_t cap;
    if (!host_pb_bridge_full_sim_get_captured(index, &cap)) {
        return false;
    }
    if (strcmp(cap.uri_path, "pogo/v1/topology") != 0) {
        return false;
    }
    bridge_export_msg_type_t type;
    const uint8_t *payload;
    uint16_t payload_len;
    if (!bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len)) {
        return false;
    }
    if (type != BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT) {
        return false;
    }
    bridge_export_topology_node_t node;
    if (!bridge_export_unpack_topology_node(payload, payload_len, &node)) {
        return false;
    }
    if (memcmp(node.module_uuid, uuid, TOPOLOGY_UUID_LEN) != 0) {
        return false;
    }
    *out = node;
    return true;
}

static bool find_topology_since(size_t since, const uint8_t uuid[TOPOLOGY_UUID_LEN],
                                 bridge_export_topology_node_t *out)
{
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        if (captured_is_topology_for(i, uuid, out)) {
            return true;
        }
    }
    return false;
}

/* Correction pass (this follow-up, item 6): freezes exporter_task()'s own
 * full-resync/heartbeat interval clock so neither can EVER fire
 * spontaneously during this file's run -- see
 * test_linux_bridge_exporter.c's own identical comment for the full
 * rationale (this file's scenarios rely on retry/backoff timing already
 * safely bounded well under the real resync interval; freezing it removes
 * that margin's dependence on cumulative real wall-clock time entirely). */
static TickType_t frozen_clock(void)
{
    return 1;
}

int main(void)
{
    host_pb_bridge_full_sim_reset();
    power_base_transport_init(POWER_BASE_UUID);

    /* Installed BEFORE init() -- see test_linux_bridge_exporter.c's own
     * comment on why. */
    linux_bridge_exporter_test_set_clock(frozen_clock);
    power_base_bridge_config_t cfg = {0};
    cfg.enabled = true;
    memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    cfg.port = 5684;
    CHECK(linux_bridge_exporter_init(POWER_BASE_UUID, &cfg) == ESP_OK, "linux_bridge_exporter_init succeeds");

    printf("\n-- scenario: Linux responding 4.12 Precondition Failed (e.g. stale_session, exactly the response "
           "a stale-backlog TOPOLOGY export gets rejected with) to a TOPOLOGY export is a FINAL, explicit "
           "rejection -- exactly one send attempt is made, never retried --\n");
    {
        uint8_t node_412[TOPOLOGY_UUID_LEN];
        make_uuid(node_412, 0x41);
        int before_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_PRECONDITION_FAILED);
        bool sent;
        otCoapCode code;
        send_topology_report(node_412, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node_412's own '/t' response is unaffected by what Linux "
                                              "later does with the export");
        CHECK(wait_for_send_count(before_sends + 1, 2000), "exactly one send attempt was made");
        sleep_ms(600); /* comfortably longer than the retry backoff schedule (0,200,500ms) would need */
        CHECK(host_pb_bridge_full_sim_send_count() == before_sends + 1,
              "no retry was made after the 4.12 rejection -- a 4.xx is a final outcome from Linux ingest, never "
              "retried with the same payload");
    }

    printf("\n-- scenario: Linux responding 4.04 Not Found to a TOPOLOGY export is likewise a FINAL rejection -- "
           "exactly one send attempt --\n");
    {
        uint8_t node_404[TOPOLOGY_UUID_LEN];
        make_uuid(node_404, 0x40);
        int before_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_NOT_FOUND);
        bool sent;
        otCoapCode code;
        send_topology_report(node_404, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node_404's own '/t' response is unaffected");
        CHECK(wait_for_send_count(before_sends + 1, 2000), "exactly one send attempt was made");
        sleep_ms(600);
        CHECK(host_pb_bridge_full_sim_send_count() == before_sends + 1,
              "no retry was made after the 4.04 rejection either -- 4.04/4.12 are both final, never mis-recorded "
              "as a bare success, and never retried");
    }

    printf("\n-- scenario: Linux responding 5.03 Service Unavailable to a TOPOLOGY export is a TRANSIENT "
           "failure -- the existing bounded 1+2 retry still applies (3 attempts total) --\n");
    {
        uint8_t node_503[TOPOLOGY_UUID_LEN];
        make_uuid(node_503, 0x50);
        int before_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_SERVICE_UNAVAILABLE);
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_SERVICE_UNAVAILABLE);
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_SERVICE_UNAVAILABLE);
        bool sent;
        otCoapCode code;
        send_topology_report(node_503, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node_503's own '/t' response is unaffected");
        CHECK(wait_for_send_count(before_sends + 3, 3000), "all 3 bounded attempts were made for a persistent "
                                                             "5.03 (unchanged from the pre-existing transient-"
                                                             "failure retry policy)");
        sleep_ms(600);
        CHECK(host_pb_bridge_full_sim_send_count() == before_sends + 3,
              "no further attempts are made once the bounded retry budget is exhausted for 5.03 either");
    }

    printf("\n-- scenario: Linux responding 2.03 Valid (an idempotent exact-duplicate) to a TOPOLOGY export is "
           "ALSO a success -- exactly one send attempt, no retry --\n");
    {
        uint8_t node_203[TOPOLOGY_UUID_LEN];
        make_uuid(node_203, 0x20);
        int before_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_VALID);
        bool sent;
        otCoapCode code;
        send_topology_report(node_203, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node_203's own '/t' response is unaffected");
        CHECK(wait_for_send_count(before_sends + 1, 2000), "exactly one send attempt was made");
        sleep_ms(600);
        CHECK(host_pb_bridge_full_sim_send_count() == before_sends + 1,
              "no retry was made after a 2.03 response -- treated as success, exactly like 2.04");
    }

    printf("\n-- scenario (stale TOPOLOGY backlog fix): a queued event enqueued while the registry held an OLD "
           "seq is coalesced with a later update for the SAME uuid before the exporter ever drains it -- when "
           "it finally drains, the payload carries the CURRENT (newest) seq, never the stale one it was "
           "originally queued for --\n");
    {
        uint8_t node_stale[TOPOLOGY_UUID_LEN];
        make_uuid(node_stale, 0x5A);
        uint8_t blocker[TOPOLOGY_UUID_LEN];
        make_uuid(blocker, 0x5B);
        bool sent;
        otCoapCode code;

        /* Occupy the exporter task with a slow (bounded-retry, never
         * answered) TOPOLOGY blocker so node_stale's own queued event sits
         * untouched while this test submits a SECOND, newer report for the
         * SAME uuid -- deterministic instead of a timing race (same
         * technique tools/test_linux_bridge_exporter.c's own "rapid samples
         * coalesce" scenario uses for the pending-sample table, applied
         * here to the topology queue instead). */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        int before_sends = host_pb_bridge_full_sim_send_count();
        send_topology_report_full(blocker, TOPOLOGY_REPORT_ATTACHED, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: the blocker's own '/t' response is unaffected");

        /* node_stale's FIRST report (seq=1) -- this is the one that gets
         * enqueued (and marked pending) into the topology queue. */
        send_topology_report_full(node_stale, TOPOLOGY_REPORT_ATTACHED, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "node_stale's first report (seq=1) is applied to the registry");

        /* node_stale's SECOND report (seq=2) -- applied to the registry
         * just like any other real change, but its OWN attempt to enqueue
         * into the topology queue is coalesced away (node_stale is already
         * pending from the first report above, and the blocker is still
         * occupying the exporter task, so this is deterministic, not a
         * race). */
        send_topology_report_full(node_stale, TOPOLOGY_REPORT_UPDATED, 2, true, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "node_stale's second report (seq=2) is ALSO applied to the registry "
                                              "(downstream_connected flips true) -- this is the CURRENT state by "
                                              "the time the queued event (still only naming node_stale's uuid, "
                                              "from the first report) is eventually drained");

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* for node_stale's drained export */
        size_t since = host_pb_bridge_full_sim_captured_count();
        CHECK(wait_for_send_count(before_sends + 4, 3000),
              "the blocker's 3 bounded attempts complete, then exactly one export for node_stale follows");

        bridge_export_topology_node_t stale_node;
        CHECK(find_topology_since(since, node_stale, &stale_node), "node_stale's drained export was captured");
        CHECK(stale_node.last_seq == 2, "the drained payload carries seq=2 (the CURRENT registry state), never "
                                          "the stale seq=1 the queued event was originally enqueued for");
        CHECK(stale_node.downstream_connected == true,
              "the drained payload also reflects the second report's downstream_connected=true -- the FULL "
              "current entry, not a partially-stale mix");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
