/*
 * Host-compiled integration test for
 * components/linux_bridge_exporter/linux_bridge_exporter.c (this follow-up:
 * Power Base -> Linux OTBR northbound bridge transport) linked against the
 * REAL, unmodified linux_bridge_exporter.c, power_base_transport.c,
 * topology_registry.c, topology_proto.c, sensor_channel.c,
 * sensor_root_aggregator.c, bridge_export_proto.c, pogo_crc16.c, and
 * coap_response_timing.c -- an incoming "/t"/"/s" report drives
 * power_base_transport.c's real bridge-observer callbacks, which
 * linux_bridge_exporter.c's real (background-task-driven) queue/retry/pack
 * logic then actually "sends" toward a simulated Linux host
 * (tools/host_test_stubs/host_pb_bridge_full_sim.c), whose captured bytes
 * this test decodes with the real bridge_export_proto_unpack_*() functions
 * to verify content, not just "a send happened".
 *
 * The exporter's own task is a REAL background pthread (see
 * tools/host_test_stubs/freertos/task.h) -- assertions that depend on it
 * having processed something poll with a bounded timeout (wait_for_send_count())
 * rather than assuming synchronous delivery.
 *
 * CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS/HEARTBEAT_INTERVAL_MS
 * (tools/host_test_stubs/pogo_fast/sdkconfig.h) are set to their real
 * production defaults (5000/10000ms) specifically so neither periodic timer
 * can fire during this test's own (much shorter) total runtime and
 * interfere with the exact send-count assertions below -- the periodic
 * resync/heartbeat TIMERS themselves are therefore NOT exercised by a live
 * wall-clock wait here (see this follow-up's final report for that gap);
 * the SEND LOGIC they call (send_topology_entry()/send_heartbeat()) is the
 * same code path already exercised by the report-driven scenarios below.
 *
 * linux_bridge_exporter_init() is called EXACTLY ONCE in this whole file
 * (it starts a real, never-terminated background task/queue -- calling it
 * twice in one process would race two tasks on the same static send state).
 * power_base_transport_init() is also called only once, using distinct
 * UUIDs per scenario to avoid cross-scenario state bleed, and assertions
 * track CUMULATIVE send_count/captured_count deltas rather than resetting
 * the sim between scenarios (resetting would also un-register the "s"/"t"
 * resources power_base_transport_init() only ever registers once).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter tools/test_linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter
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

static const uint8_t POWER_BASE_UUID[TOPOLOGY_UUID_LEN] = {0xB0, 0, 0, 0, 0, 0, 0, 0x01};

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

static uint16_t send_topology_report(const uint8_t module_uuid[TOPOLOGY_UUID_LEN],
                                      const uint8_t parent_uuid[TOPOLOGY_UUID_LEN], uint8_t depth,
                                      topology_report_type_t type, uint32_t generation, uint32_t seq,
                                      bool downstream_connected, bool *out_sent, otCoapCode *out_code)
{
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, parent_uuid, TOPOLOGY_UUID_LEN);
    rep.topology_depth = depth;
    rep.role = TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0003;
    rep.boot_session_generation = generation;
    rep.sequence = seq;
    rep.upstream_connected = true;
    rep.downstream_connected = downstream_connected;
    rep.report_type = type;

    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
    return (uint16_t)len;
}

static void send_sensor_sample(const uint8_t module_uuid[SENSOR_UUID_LEN], uint32_t seq, bool *out_sent,
                                otCoapCode *out_code)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memcpy(s.module_uuid, module_uuid, SENSOR_UUID_LEN);
    s.sample_seq = seq;
    s.uptime_ms = 100;
    s.sensor_bitmap = 0x0001;
    s.channel_count = 1;
    s.channels[0] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI,
                                                 .status = SENSOR_CHANNEL_STATUS_OK, .value = 21500};
    uint8_t wire[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t len = sensor_channel_pack(&s, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("s", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

/* Decodes a captured send at `index` as a TOPOLOGY_SNAPSHOT and returns
 * true if it names `uuid` as its module_uuid, filling *out with the decoded
 * node on success. */
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

/* Scans captured sends in [since, host_pb_bridge_full_sim_captured_count())
 * for a TOPOLOGY_SNAPSHOT naming `uuid`. */
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

/* Decodes a captured send at `index` as a TOPOLOGY_REMOVED tombstone (this
 * follow-up: POGO auto-registration/removal lifecycle) and returns true if
 * it names `uuid` as its module_uuid. */
static bool captured_is_removed_for(size_t index, const uint8_t uuid[TOPOLOGY_UUID_LEN])
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
    if (type != BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED) {
        return false;
    }
    bridge_export_topology_removed_t rem;
    if (!bridge_export_unpack_topology_removed(payload, payload_len, &rem)) {
        return false;
    }
    return memcmp(rem.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0;
}

/* Scans captured sends in [since, host_pb_bridge_full_sim_captured_count())
 * for a TOPOLOGY_REMOVED tombstone naming `uuid`. */
static bool find_removed_since(size_t since, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        if (captured_is_removed_for(i, uuid)) {
            return true;
        }
    }
    return false;
}

/* Correction pass (this follow-up, item 6): this file's own top comment
 * already states its intent -- resync/heartbeat TIMERS themselves are not
 * exercised here at all (see test_linux_bridge_exporter_timers.c for that),
 * only the reactive SEND LOGIC they'd eventually call. A frozen fake clock
 * (installed via linux_bridge_exporter_test_set_clock(), always returning
 * the SAME value) makes that true structurally: (now - last_resync) is
 * always exactly 0, so a periodic resync/heartbeat tick can NEVER fire
 * spontaneously here, no matter how much real wall-clock time this
 * process's cumulative scenario runtime consumes. This is what eliminates
 * this file's own ~15-20% pre-existing flakiness (an unplanned resync tick
 * firing near the end of a long run, resending a SNAPSHOT for every uuid
 * ever attached so far and exhausting a scenario's fixed response queue) --
 * see this file's own git history for the failure this fixes. */
static TickType_t frozen_clock(void)
{
    return 1;
}

int main(void)
{
    host_pb_bridge_full_sim_reset();
    power_base_transport_init(POWER_BASE_UUID);
    power_base_transport_bridge_observer_t no_observer = {0};
    (void)no_observer;

    /* Installed BEFORE init() -- linux_bridge_exporter_init() starts
     * exporter_task() on its own concurrent thread, which captures its
     * initial last_resync/last_heartbeat via exporter_now_ms() almost
     * immediately; if the override were installed only AFTER init()
     * returns, that first capture could race and use the REAL clock while
     * every later comparison uses the frozen one, and (TickType_t being
     * unsigned) that mismatch wraps into a huge bogus "elapsed" value that
     * fires resync immediately -- exactly the bug this ordering avoids. */
    linux_bridge_exporter_test_set_clock(frozen_clock);
    power_base_bridge_config_t cfg = {0};
    cfg.enabled = true;
    memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN); /* fabricated ULA-shaped test address */
    cfg.port = 5684;
    esp_err_t init_err = linux_bridge_exporter_init(POWER_BASE_UUID, &cfg);
    CHECK(init_err == ESP_OK, "linux_bridge_exporter_init succeeds");

    uint8_t node_a[TOPOLOGY_UUID_LEN];
    make_uuid(node_a, 0xA1);

    printf("\n-- scenario: a real ATTACHED report is exported as a TOPOLOGY_SNAPSHOT with matching fields --\n");
    {
        size_t since = host_pb_bridge_full_sim_captured_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        bool sent;
        otCoapCode code;
        send_topology_report(node_a, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 100, 1, false, &sent, &code);
        CHECK(sent && code == OT_COAP_CODE_CHANGED, "the Power Base's own '/t' response is unaffected: 2.04 Changed");
        CHECK(wait_for_send_count(1, 2000), "the exporter task actually sent one export within the timeout");

        bridge_export_topology_node_t node;
        CHECK(find_topology_since(since, node_a, &node), "a TOPOLOGY_SNAPSHOT naming node_a was captured");
        CHECK(node.online == true, "the exported node's online flag is true");
        CHECK(node.depth == 1, "the exported node's depth matches the report");
        CHECK(memcmp(node.parent_uuid, POWER_BASE_UUID, TOPOLOGY_UUID_LEN) == 0,
              "the exported node's parent_uuid matches the report");
    }

    printf("\n-- scenario: a real sample is exported as a SAMPLE with the embedded sensor_channel frame intact "
           "--\n");
    {
        int before_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        bool sent;
        otCoapCode code;
        send_sensor_sample(node_a, 42, &sent, &code);
        CHECK(sent && code == OT_COAP_CODE_CHANGED, "the Power Base's own '/s' response is 2.04 Changed");
        CHECK(wait_for_send_count(before_sends + 1, 2000), "the exporter sent exactly one more export");

        host_pb_bridge_full_sim_sent_t cap;
        CHECK(host_pb_bridge_full_sim_get_captured((size_t)(before_sends), &cap), "the send was captured");
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        CHECK(strcmp(cap.uri_path, "pogo/v1/sample") == 0, "the sample was sent to the correct resource path");
        CHECK(bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) &&
                  type == BRIDGE_EXPORT_MSG_SAMPLE,
              "the captured message decodes as a SAMPLE");
        bridge_export_sample_t exp_sample;
        CHECK(bridge_export_unpack_sample(payload, payload_len, &exp_sample), "the SAMPLE payload unpacks OK");
        sensor_sample_t decoded;
        CHECK(sensor_channel_unpack(exp_sample.sensor_channel_payload, exp_sample.sensor_channel_len, &decoded),
              "the embedded sensor_channel frame is valid");
        CHECK(decoded.sample_seq == 42, "the embedded frame's sample_seq matches the original sample");
        CHECK(decoded.channels[0].value == 21500, "the embedded frame's channel value matches the original sample");
    }

    printf("\n-- scenario: an exact-duplicate sample retransmit is never exported again --\n");
    {
        int before_sends = host_pb_bridge_full_sim_send_count();
        bool sent;
        otCoapCode code;
        send_sensor_sample(node_a, 42, &sent, &code); /* same seq as the previous scenario */
        CHECK(code == OT_COAP_CODE_VALID, "the Power Base ACKs an exact-duplicate '/s' retransmit with 2.03 Valid "
                                            "(correction pass: distinct from 2.04 -- nothing was actually re-stored)");
        sleep_ms(150); /* generous settle window -- no send should ever appear */
        CHECK(host_pb_bridge_full_sim_send_count() == before_sends,
              "no additional export was ever sent for the exact-duplicate sample");
    }

    printf("\n-- scenario: rapid samples for the SAME uuid coalesce to a single export of the LATEST value --\n");
    {
        uint8_t node_e[TOPOLOGY_UUID_LEN];
        make_uuid(node_e, 0xEE);
        bool sent;
        otCoapCode code;
        int setup_base = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report(node_e, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 800, 1, false, &sent, &code);
        CHECK(wait_for_send_count(setup_base + 1, 2000), "setup: node_e's topology attach settles");

        /* Occupy the exporter task with a slow (bounded-retry, never
         * answered) TOPOLOGY blocker so both samples below are upserted
         * into the pending-sample table well before the task's next
         * drain_pending_samples() call -- makes the coalescing outcome
         * deterministic instead of a timing race. */
        uint8_t blocker2[TOPOLOGY_UUID_LEN];
        make_uuid(blocker2, 0xEF);
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        int before_sends = host_pb_bridge_full_sim_send_count();
        send_topology_report(blocker2, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 801, 1, false, &sent, &code);

        send_sensor_sample(node_e, 1, &sent, &code); /* stale -- should never be exported on its own */
        CHECK(code == OT_COAP_CODE_CHANGED, "node_e's first sample (seq=1) is stored fine at the aggregator "
                                              "level -- coalescing only affects the EXPORT, never storage");
        send_sensor_sample(node_e, 2, &sent, &code); /* the only value that should ever be exported */
        CHECK(code == OT_COAP_CODE_CHANGED, "node_e's second sample (seq=2) is also stored fine");

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* for the coalesced sample */
        size_t since = host_pb_bridge_full_sim_captured_count();
        CHECK(wait_for_send_count(before_sends + 4, 3000),
              "the blocker's 3 bounded attempts complete, then exactly one coalesced sample export follows");

        int sample_sends_for_node_e = 0;
        uint32_t last_seen_seq = 0;
        size_t total = host_pb_bridge_full_sim_captured_count();
        for (size_t i = since; i < total; i++) {
            host_pb_bridge_full_sim_sent_t cap;
            host_pb_bridge_full_sim_get_captured(i, &cap);
            if (strcmp(cap.uri_path, "pogo/v1/sample") != 0) {
                continue;
            }
            bridge_export_msg_type_t type;
            const uint8_t *payload;
            uint16_t payload_len;
            if (!bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) ||
                type != BRIDGE_EXPORT_MSG_SAMPLE) {
                continue;
            }
            bridge_export_sample_t exp;
            if (!bridge_export_unpack_sample(payload, payload_len, &exp)) {
                continue;
            }
            sensor_sample_t decoded;
            if (!sensor_channel_unpack(exp.sensor_channel_payload, exp.sensor_channel_len, &decoded)) {
                continue;
            }
            if (memcmp(decoded.module_uuid, node_e, SENSOR_UUID_LEN) != 0) {
                continue;
            }
            sample_sends_for_node_e++;
            last_seen_seq = decoded.sample_seq;
        }
        CHECK(sample_sends_for_node_e == 1,
              "exactly ONE sample export was sent for node_e -- the first (seq=1) never went out on its own, it "
              "was coalesced away by the second (seq=2) before the task ever drained it");
        CHECK(last_seen_seq == 2, "the single exported sample carries the LATEST seq (2), never the stale seq=1");
    }

    printf("\n-- scenario (correction pass): a DETACHED report cascading a child ERASES both node_a and node_b "
           "from the Power Base's own topology_registry (topology_registry_erase()) -- so no ordinary SNAPSHOT "
           "is ever exported for either; ONLY a REMOVED tombstone for each --\n");
    {
        uint8_t node_b[TOPOLOGY_UUID_LEN];
        make_uuid(node_b, 0xB2);
        bool sent;
        otCoapCode code;

        int base_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report(node_a, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_UPDATED, 100, 2, true, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node_a downstream_connected=true");
        CHECK(wait_for_send_count(base_sends + 1, 2000), "setup: node_a's UPDATED export settles");

        base_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report(node_b, node_a, 2, TOPOLOGY_REPORT_ATTACHED, 200, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node_b (child of node_a) online");
        CHECK(wait_for_send_count(base_sends + 1, 2000), "setup: node_b's ATTACHED export settles");

        size_t since = host_pb_bridge_full_sim_captured_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* node_a's own REMOVED tombstone (on_node_removed) */
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* node_b's cascaded REMOVED tombstone (on_node_removed) */
        send_topology_report(node_a, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_DETACHED, 100, 3, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the DETACHED report itself is applied (2.04)");
        bool got_both = false;
        for (int tries = 0; tries < 400 && !got_both; tries++) {
            sleep_ms(5);
            if (host_pb_bridge_full_sim_captured_count() - since >= 2) {
                got_both = true;
            }
        }
        CHECK(got_both, "node_a's REMOVED and node_b's REMOVED tombstones both arrived within the timeout");

        CHECK(find_removed_since(since, node_a), "a REMOVED tombstone naming node_a itself was captured (its own "
                                                   "confirmed self-detach)");
        CHECK(find_removed_since(since, node_b), "a REMOVED tombstone naming node_b was captured (the cascaded "
                                                    "child)");
        bridge_export_topology_node_t a_node_should_not_exist, b_node_should_not_exist;
        CHECK(!find_topology_since(since, node_a, &a_node_should_not_exist),
              "node_a never gets an ordinary SNAPSHOT export for this cascade -- it erased itself, so there is "
              "no entry left for on_topology_applied to convey");
        CHECK(!find_topology_since(since, node_b, &b_node_should_not_exist),
              "node_b never gets an ordinary SNAPSHOT export for this cascade either -- only the REMOVED "
              "tombstone above");

        /* Exactly 2 sends total for this cascade -- never a 3rd (no wasted
         * SNAPSHOT round trip for the now-erased node_a). */
        CHECK(host_pb_bridge_full_sim_captured_count() - since == 2,
              "exactly two exports resulted from this cascade, never three");
    }

    printf("\n-- scenario: the simulated Linux host never responding does not affect the Power Base's own CoAP "
           "response, and the exporter keeps working afterward --\n");
    {
        uint8_t node_c[TOPOLOGY_UUID_LEN];
        make_uuid(node_c, 0xC3);
        int before_sends = host_pb_bridge_full_sim_send_count();

        /* transmit() makes 1 initial + 2 retries = 3 attempts total. */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();

        bool sent;
        otCoapCode code;
        send_topology_report(node_c, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 300, 1, false, &sent, &code);
        CHECK(sent && code == OT_COAP_CODE_CHANGED,
              "the Power Base's own '/t' response is STILL 2.04 even though the export will fail -- the two are "
              "structurally independent (the response was already sent before the exporter task even starts)");

        CHECK(wait_for_send_count(before_sends + 3, 3000),
              "the exporter made exactly 3 bounded attempts before giving up (never an infinite retry loop)");
        sleep_ms(100);
        CHECK(host_pb_bridge_full_sim_send_count() == before_sends + 3,
              "no further attempts are made once the bounded retry budget is exhausted");

        printf("\n-- the exporter keeps working normally for a brand-new event afterward --\n");
        uint8_t node_d[TOPOLOGY_UUID_LEN];
        make_uuid(node_d, 0xD4);
        size_t since = host_pb_bridge_full_sim_captured_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report(node_d, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 400, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "node_d's own '/t' response is 2.04");
        CHECK(wait_for_send_count(before_sends + 4, 2000), "the exporter is not stuck -- it sends node_d's export "
                                                            "normally");
        bridge_export_topology_node_t d_node;
        CHECK(find_topology_since(since, node_d, &d_node), "node_d's export was captured with correct content");
        CHECK(d_node.online == true, "node_d's exported entry shows online=true");
    }

    printf("\n-- scenario: rapidly firing more events than the bounded queue can hold never crashes or hangs, "
           "and the exporter still processes new events correctly afterward --\n");
    {
        /* The head-of-line event is forced to take the full bounded-retry
         * duration (~3 attempts, each up to the derived ack-timeout wait),
         * so the very next several events queue up behind it -- more than
         * CONFIG_LINUX_BRIDGE_EXPORTER_QUEUE_LEN (4) worth are fired to
         * guarantee at least one is dropped (exercising the overflow path)
         * without crashing or wedging the task. */
        uint8_t blocker[TOPOLOGY_UUID_LEN];
        make_uuid(blocker, 0xE5);
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        bool sent;
        otCoapCode code;
        send_topology_report(blocker, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 500, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the blocker event's own '/t' response is unaffected");

        for (int i = 0; i < 10; i++) {
            uint8_t burst_uuid[TOPOLOGY_UUID_LEN];
            make_uuid(burst_uuid, (uint8_t)(0xF0 + i));
            host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
            send_topology_report(burst_uuid, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 600 + i, 1, false, &sent,
                                  &code);
        }
        /* Every one of the 10 burst events still gets a normal Power Base
         * '/t' response regardless of whether ITS OWN export later gets
         * dropped by the exporter's bounded queue -- registry/CoAP handling
         * and export queuing are structurally independent. */
        CHECK(code == OT_COAP_CODE_CHANGED, "the last burst event's own '/t' response is unaffected");

        /* Let the blocker's 3 bounded attempts finish, then let whatever of
         * the burst made it into the queue drain. */
        sleep_ms(2500);

        uint8_t recovery_uuid[TOPOLOGY_UUID_LEN];
        make_uuid(recovery_uuid, 0x99);
        int before_sends = host_pb_bridge_full_sim_send_count();
        size_t since = host_pb_bridge_full_sim_captured_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report(recovery_uuid, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 700, 1, false, &sent,
                              &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the recovery event's own '/t' response is 2.04");
        CHECK(wait_for_send_count(before_sends + 1, 2000),
              "the exporter is neither crashed nor wedged after the overflow burst -- a brand-new event still "
              "gets exported");
        bridge_export_topology_node_t recovery_node;
        CHECK(find_topology_since(since, recovery_uuid, &recovery_node),
              "the recovery event's export was captured with correct content");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
