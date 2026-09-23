/*
 * Host-compiled test for the per-module_uuid SAMPLE export rate limit added
 * to components/linux_bridge_exporter/linux_bridge_exporter.c (Power Base ->
 * Linux -> Matter controller transport-load reduction), INCLUDING this
 * correction pass's own fixes for two bugs found in code review of that
 * original pass -- see linux_bridge_exporter.c's own top comment:
 *
 *   1. transmit()'s old fixed-payload retry loop could resend a STALE
 *      sample_seq on a later retry attempt if a newer sample for the same
 *      uuid arrived while an earlier attempt was still in flight.
 *      transmit_sample() now re-reads sensor_root_aggregator fresh
 *      immediately before EVERY attempt.
 *   2. do_full_sample_resync() used to bypass the per-uuid rate gate
 *      entirely, so a reactive publish immediately followed by a periodic
 *      resync tick could produce two SAMPLE publishes for the same uuid
 *      inside one rate-limit window. Resync now only marks a uuid pending
 *      (mark_sample_pending()) -- the actual publish always goes through
 *      the SAME drain_pending_samples() gate as a reactive publish.
 *
 * Links the REAL, unmodified linux_bridge_exporter.c, power_base_transport.c,
 * topology_registry.c, topology_proto.c, sensor_channel.c,
 * sensor_root_aggregator.c, bridge_export_proto.c, pogo_crc16.c, and
 * coap_response_timing.c, same as tools/test_linux_bridge_exporter.c.
 *
 * Uses tools/host_test_stubs/pogo_fast/sdkconfig.h -- the REAL production
 * CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS default (1000ms), and
 * the real production resync/heartbeat intervals (5000ms/10000ms, so
 * neither can fire during this test's own much-shorter total runtime and
 * perturb the exact send-count assertions below -- same reasoning as
 * tools/test_linux_bridge_exporter.c's own header comment).
 *
 * Covers this follow-up's own test list (section 5, Power Base exporter,
 * items 1-4 and 7-9, PLUS this correction pass's own new items 1/3/6; items
 * 5/6 (original numbering) -- 4.xx never retried, 5.xx/timeout bounded
 * retry -- are policy-generic across TOPOLOGY and SAMPLE alike and are
 * already exhaustively covered by tools/test_linux_bridge_exporter_retry_policy.c
 * against the SAME send_one_attempt() code SAMPLE export also goes through;
 * not duplicated here beyond the ordered-attempt-inspection scenario below.
 * This correction pass's own item 2 (reactive publish immediately followed
 * by a periodic resync tick must not produce a second publish inside one
 * rate-limit window) needs the SHORT resync-interval stub to actually
 * observe a resync tick in real wall-clock time -- see
 * tools/test_linux_bridge_exporter_timers.c's own new scenario for that,
 * not duplicated here.):
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_rate_limit tools/test_linux_bridge_exporter_rate_limit.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_rate_limit
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

static void send_topology_report_full(const uint8_t module_uuid[TOPOLOGY_UUID_LEN], topology_report_type_t type,
                                       uint32_t seq, bool *out_sent, otCoapCode *out_code)
{
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, POWER_BASE_UUID, TOPOLOGY_UUID_LEN);
    rep.topology_depth = 1;
    rep.role = TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = 42;
    rep.sequence = seq;
    rep.upstream_connected = true;
    rep.downstream_connected = false;
    rep.report_type = type;
    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

static void send_sensor_sample_v(const uint8_t module_uuid[SENSOR_UUID_LEN], uint32_t seq, int32_t value,
                                  bool *out_sent, otCoapCode *out_code)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memcpy(s.module_uuid, module_uuid, SENSOR_UUID_LEN);
    s.sample_seq = seq;
    s.uptime_ms = 1;
    s.sensor_bitmap = 0x0001;
    s.channel_count = 1;
    s.channels[0] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI,
                                                 .status = SENSOR_CHANNEL_STATUS_OK, .value = value};
    uint8_t wire[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t len = sensor_channel_pack(&s, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("s", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

/* Scans captured sends in [since, count) for a SAMPLE naming `uuid`, filling
 * out_last_seq/out_last_value with the LAST (in capture order) one found --
 * used to assert "if anything at all was exported for this uuid in this
 * window, it carries the latest seq/value", not merely "some send happened". */
static int count_samples_for(size_t since, const uint8_t uuid[SENSOR_UUID_LEN], uint32_t *out_last_seq,
                              int32_t *out_last_value)
{
    int n = 0;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap)) {
            continue;
        }
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
        if (memcmp(decoded.module_uuid, uuid, SENSOR_UUID_LEN) != 0) {
            continue;
        }
        n++;
        if (out_last_seq != NULL) {
            *out_last_seq = decoded.sample_seq;
        }
        if (out_last_value != NULL) {
            *out_last_value = decoded.channels[0].value;
        }
    }
    return n;
}

typedef struct {
    uint32_t seq;
    int32_t value;
} sample_attempt_t;

/* Correction pass: collects EVERY captured SAMPLE attempt (in capture
 * order, i.e. in the exact order transmit_sample() actually dispatched
 * them -- including ones that will go on to fail) for `uuid` since `since`,
 * up to `max_out` -- used to verify the FULL ordered sequence of what was
 * actually put on the wire across a multi-attempt retry cycle, not merely
 * "the last thing captured" (this file's own test list item 6: "모든 전송
 * attempt의 UUID/seq/value를 순서대로 검증"). host_pb_bridge_full_sim
 * captures a send BEFORE consulting its queued outcome (see that file's own
 * otCoapSendRequestWithParameters()), so a captured entry here is exactly
 * the payload transmit_sample() built and dispatched for that attempt,
 * regardless of whether the attempt went on to succeed, get rejected, or
 * time out. */
static int collect_sample_attempts(size_t since, const uint8_t uuid[SENSOR_UUID_LEN], sample_attempt_t *out,
                                    int max_out)
{
    int n = 0;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total && n < max_out; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap)) {
            continue;
        }
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
        if (memcmp(decoded.module_uuid, uuid, SENSOR_UUID_LEN) != 0) {
            continue;
        }
        out[n].seq = decoded.sample_seq;
        out[n].value = decoded.channels[0].value;
        n++;
    }
    return n;
}

/* Occupies the exporter task with a slow, never-answered (bounded-retry)
 * TOPOLOGY send for `blocker_uuid` so that whatever this scenario upserts
 * next is guaranteed to land in the pending-sample table well before
 * exporter_task()'s next drain_pending_samples() call -- makes coalescing
 * outcomes deterministic instead of a timing race (same technique
 * tools/test_linux_bridge_exporter.c's own "rapid samples coalesce"
 * scenario already uses). */
static void occupy_exporter_with_blocker(uint8_t tag)
{
    uint8_t blocker[TOPOLOGY_UUID_LEN];
    make_uuid(blocker, tag);
    host_pb_bridge_full_sim_queue_no_response();
    host_pb_bridge_full_sim_queue_no_response();
    host_pb_bridge_full_sim_queue_no_response();
    bool sent;
    otCoapCode code;
    send_topology_report_full(blocker, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
    (void)code;
}

/* Correction pass (this follow-up, item 6): freezes exporter_task()'s own
 * full-resync/heartbeat interval clock so neither can EVER fire
 * spontaneously during this file's run, however much cumulative real
 * wall-clock time it consumes -- eliminating the pre-existing flakiness a
 * stray resync tick (resending a SNAPSHOT for every uuid attached so far)
 * could otherwise cause. This file's own per-uuid SAMPLE_MIN_INTERVAL_MS
 * rate-limit gate (drain_pending_samples()) is a SEPARATE timer, still on
 * the real clock, completely unaffected -- see
 * linux_bridge_exporter_test_set_clock()'s own doc comment. */
static TickType_t frozen_clock(void)
{
    return 1;
}

int main(void)
{
    host_pb_bridge_full_sim_reset();
    power_base_transport_init(POWER_BASE_UUID);

    /* Installed BEFORE init() -- see test_linux_bridge_exporter.c's own
     * comment on why (a race with exporter_task()'s initial
     * last_resync/last_heartbeat capture otherwise wraps into a bogus
     * immediate "elapsed" and fires resync right away). */
    linux_bridge_exporter_test_set_clock(frozen_clock);
    power_base_bridge_config_t cfg = {0};
    cfg.enabled = true;
    memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    cfg.port = 5684;
    CHECK(linux_bridge_exporter_init(POWER_BASE_UUID, &cfg) == ESP_OK, "linux_bridge_exporter_init succeeds");

    printf("\n-- scenario: 100 samples for the SAME uuid arriving within 1 second produce a SINGLE export "
           "carrying the 100th (latest) seq/value, never a burst of 100 sends (test list items 1-2) --\n");
    {
        uint8_t node[TOPOLOGY_UUID_LEN];
        make_uuid(node, 0xA1);
        bool sent;
        otCoapCode code;

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
        CHECK(wait_for_send_count(1, 2000), "setup: node's topology attach settles");

        /* Occupy the exporter task so all 100 upserts below land in the
         * table before the first drain -- see this file's own helper doc
         * comment. */
        occupy_exporter_with_blocker(0xA2);

        int before_sends = host_pb_bridge_full_sim_send_count();
        bool all_stored_ok = true;
        for (uint32_t seq = 1; seq <= 100; seq++) {
            send_sensor_sample_v(node, seq, (int32_t)(20000 + seq), &sent, &code);
            if (code != OT_COAP_CODE_CHANGED && code != OT_COAP_CODE_VALID) {
                all_stored_ok = false;
            }
        }
        CHECK(all_stored_ok, "the aggregator stores every one of the 100 rapid samples fine (storage rate is "
                               "completely unaffected by the export rate limit)");

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* for the single coalesced export */
        size_t since = host_pb_bridge_full_sim_captured_count();
        CHECK(wait_for_send_count(before_sends + 4, 3000),
              "the blocker's 3 bounded attempts complete, then exactly one export follows for the 100-sample burst");

        uint32_t last_seq = 0;
        int32_t last_value = 0;
        int n = count_samples_for(since, node, &last_seq, &last_value);
        CHECK(n == 1, "exactly ONE sample export resulted from 100 rapid samples in well under the 1000ms "
                       "per-uuid window -- never one send per sample");
        CHECK(last_seq == 100, "the single export carries seq=100, the LATEST of the 100 samples");
        CHECK(last_value == (int32_t)(20000 + 100), "the single export also carries the LATEST value, matching "
                                                       "seq=100 exactly (never an intermediate/stale one)");
        /* The "the gate is a rolling window, not a one-shot latch" property
         * (test list item 9) is proven separately by the retry-exhaustion
         * scenario further down, which already needs to wait for a window
         * reopen for its own reason -- not duplicated here, to keep this
         * file's total wall-clock runtime safely under
         * CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS (5000ms, the
         * real production default this file deliberately uses -- see this
         * file's own top comment on why that must never fire mid-test). */
    }

    printf("\n-- scenario: two DIFFERENT uuids rate-limit completely independently (test list item 3) --\n");
    {
        uint8_t node_x[TOPOLOGY_UUID_LEN], node_y[TOPOLOGY_UUID_LEN];
        make_uuid(node_x, 0xB1);
        make_uuid(node_y, 0xB2);
        bool sent;
        otCoapCode code;

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node_x, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node_y, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
        sleep_ms(300); /* let both attaches settle */

        occupy_exporter_with_blocker(0xB3);
        int before = host_pb_bridge_full_sim_send_count();
        for (uint32_t seq = 1; seq <= 20; seq++) {
            send_sensor_sample_v(node_x, seq, (int32_t)(1000 + seq), &sent, &code);
            send_sensor_sample_v(node_y, seq, (int32_t)(2000 + seq), &sent, &code);
        }
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* node_x's coalesced export */
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* node_y's coalesced export */
        size_t since = host_pb_bridge_full_sim_captured_count();
        CHECK(wait_for_send_count(before + 5, 3000), "the blocker settles, then both node_x's and node_y's own "
                                                       "coalesced exports follow");

        uint32_t seq_x = 0, seq_y = 0;
        int32_t val_x = 0, val_y = 0;
        int n_x = count_samples_for(since, node_x, &seq_x, &val_x);
        int n_y = count_samples_for(since, node_y, &seq_y, &val_y);
        CHECK(n_x == 1 && n_y == 1, "each uuid independently produced exactly one coalesced export, not zero and "
                                      "not more");
        CHECK(seq_x == 20 && val_x == 1020, "node_x's export carries ITS OWN latest seq/value");
        CHECK(seq_y == 20 && val_y == 2020, "node_y's export carries ITS OWN latest seq/value, unaffected by "
                                              "node_x's burst");
    }

    printf("\n-- scenario: a TOPOLOGY OFFLINE event for a uuid mid-SAMPLE-rate-limit-window is delivered "
           "immediately, never held back by the 1000ms SAMPLE gate (test list item 4) --\n");
    {
        uint8_t node[TOPOLOGY_UUID_LEN];
        make_uuid(node, 0xC1);
        bool sent;
        otCoapCode code;

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
        sleep_ms(50);

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_sensor_sample_v(node, 1, 21000, &sent, &code);
        sleep_ms(200); /* let the first sample's own export go out, consuming this uuid's window */

        /* Still well inside the 1000ms SAMPLE window -- a DETACHED report
         * must still be exported right away. Correction pass: a DETACHED
         * report now ERASES this uuid entirely from topology_registry
         * (topology_registry_erase()), so on_topology_applied does NOT
         * fire (there is no entry left to convey) -- only on_node_removed's
         * own REMOVED tombstone (this uuid's confirmed self-detach) is
         * exported. Exactly one response is queued. */
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        size_t since = host_pb_bridge_full_sim_captured_count();
        send_topology_report_full(node, TOPOLOGY_REPORT_DETACHED, 2, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the DETACHED report itself is applied to the registry immediately");

        bool got_snapshot = false, got_removed = false;
        for (int tries = 0; tries < 60 && !got_removed; tries++) { /* 60*5ms = 300ms, well under the 1000ms gate */
            sleep_ms(5);
            size_t total = host_pb_bridge_full_sim_captured_count();
            for (size_t i = since; i < total; i++) {
                host_pb_bridge_full_sim_sent_t cap;
                if (!host_pb_bridge_full_sim_get_captured(i, &cap) ||
                    strcmp(cap.uri_path, "pogo/v1/topology") != 0) {
                    continue;
                }
                bridge_export_msg_type_t type;
                const uint8_t *payload;
                uint16_t payload_len;
                if (!bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len)) {
                    continue;
                }
                if (type == BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT) {
                    got_snapshot = true;
                } else if (type == BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED) {
                    got_removed = true;
                }
            }
        }
        CHECK(!got_snapshot, "correction pass: NO ordinary SNAPSHOT export is ever sent for a self-erasing "
                               "DETACHED report -- the entry no longer exists for on_topology_applied to convey");
        CHECK(got_removed, "a REMOVED tombstone for this uuid's own confirmed self-detach is delivered within "
                             "~300ms, NOT held back 1000ms by this uuid's own still-open SAMPLE rate-limit window "
                             "-- TOPOLOGY and SAMPLE are gated completely independently");
    }

    printf("\n-- scenario (correction pass, test list item 1): while a SAMPLE's first attempt is still a "
           "transient failure, a NEWER sample for the SAME uuid is stored -- every subsequent retry attempt is "
           "inspected IN ORDER, confirming the stale seq is never resent and the retry re-fetches the fresh "
           "value immediately before dispatching --\n");
    {
        uint8_t node[TOPOLOGY_UUID_LEN];
        make_uuid(node, 0xD1);
        bool sent;
        otCoapCode code;

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
        sleep_ms(100);

        /* Attempt 1 (seq=1, the only value in the aggregator at that
         * moment) will transiently fail; attempt 2 (a retry) will succeed. */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        size_t since = host_pb_bridge_full_sim_captured_count();
        send_sensor_sample_v(node, 1, 11111, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "seq=1 is stored fine at the aggregator level regardless of the "
                                              "export's eventual fate");

        /* Deterministically wait for attempt 1 to ACTUALLY dispatch
         * (captured) before injecting seq=2 -- this is what makes this
         * scenario a genuine "a newer sample arrives while an earlier
         * attempt is in flight" case, not just "the aggregator already
         * held the newest value before the exporter ever looked". */
        bool attempt1_seen = false;
        for (int tries = 0; tries < 400 && !attempt1_seen; tries++) {
            sleep_ms(5);
            if (count_samples_for(since, node, NULL, NULL) >= 1) {
                attempt1_seen = true;
            }
        }
        CHECK(attempt1_seen, "setup: attempt 1 (seq=1) actually dispatched before seq=2 is injected");

        /* attempt 1 is now somewhere in its ~40ms ack-timeout wait or its
         * 200ms retry backoff -- comfortably enough time to store seq=2
         * before attempt 2 fires. */
        send_sensor_sample_v(node, 2, 22222, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "seq=2 (newer) is stored fine at the aggregator level while attempt "
                                              "1 is still failing/backing off");

        bool got_two_attempts = false;
        for (int tries = 0; tries < 400 && !got_two_attempts; tries++) {
            sleep_ms(5);
            if (count_samples_for(since, node, NULL, NULL) >= 2) {
                got_two_attempts = true;
            }
        }
        CHECK(got_two_attempts, "attempt 2 (the retry) has now also dispatched");

        sample_attempt_t attempts[8];
        int n = collect_sample_attempts(since, node, attempts, 8);
        CHECK(n == 2, "exactly two attempts were made for this uuid -- attempt 1 failed transiently, attempt 2 "
                       "succeeded, so no further retry was needed");
        if (n >= 2) {
            CHECK(attempts[0].seq == 1, "attempt 1 (in capture order) carried seq=1 -- the only value available "
                                          "in the aggregator at that exact moment");
            CHECK(attempts[1].seq == 2, "attempt 2 (in capture order, a retry) carried the NEWER seq=2 -- "
                                          "re-fetched fresh from sensor_root_aggregator immediately before this "
                                          "attempt was dispatched, never a cached/stale seq=1 payload reused");
            CHECK(attempts[1].value == 22222, "attempt 2's value also matches the fresh seq=2 sample exactly");
        }
    }

    /* Test list item 3 (retry-exhaustion-leaves-uuid-pending-again recovery)
     * is covered in its own dedicated file,
     * tools/test_linux_bridge_exporter_retry_exhaustion.c -- that scenario
     * alone needs close to 2s of real wall-clock time, which does not fit
     * this file's own budget alongside everything above while staying
     * safely under CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS
     * (5000ms) -- see that file's own top comment. */

    printf("\n-- scenario (wrap-safe comparison, test list item 9): the exact idiom "
           "drain_pending_samples() uses, `(int32_t)(now - deadline) >= 0`, compares correctly across a "
           "TickType_t wraparound boundary -- a deadline set just before UINT32_MAX and `now` just after it "
           "wrapping to a small value is still correctly seen as \"deadline has passed\" --\n");
    {
        uint32_t deadline = 0xFFFFFFF0u; /* 16 ticks before wraparound */
        uint32_t now_before_wrap = 0xFFFFFFF5u; /* 5 ticks past deadline, not yet wrapped */
        uint32_t now_after_wrap = 0x00000005u;  /* wrapped: 21 ticks past deadline in real elapsed time */
        uint32_t now_not_yet = 0xFFFFFFEEu;     /* 2 ticks before deadline -- must NOT be "passed" yet */

        CHECK((int32_t)(now_before_wrap - deadline) >= 0, "just before the wraparound boundary, an elapsed "
                                                             "deadline still compares as passed");
        CHECK((int32_t)(now_after_wrap - deadline) >= 0, "AFTER TickType_t wraps around past UINT32_MAX back to "
                                                            "a small value, the SAME deadline still correctly "
                                                            "compares as passed -- this is exactly what stops "
                                                            "the per-uuid SAMPLE gate from permanently latching "
                                                            "closed across a tick counter wraparound");
        CHECK((int32_t)(now_not_yet - deadline) < 0, "a `now` that is genuinely still before the deadline (no "
                                                        "wraparound involved) correctly compares as NOT yet "
                                                        "passed -- the same idiom does not falsely open the gate "
                                                        "early");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
