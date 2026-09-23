/*
 * Host-compiled test for the REAL periodic timers in
 * components/linux_bridge_exporter/linux_bridge_exporter.c (item 7: "실제
 * timer 테스트") -- unlike tools/test_linux_bridge_exporter.c (which
 * deliberately freezes exporter_task()'s own clock so periodic firing can
 * never perturb its exact send-count-delta assertions -- see that file's
 * own top comment), THIS file exists specifically to exercise the real
 * resync/heartbeat firing logic.
 *
 * Correction pass (this follow-up, item 6): every resync/heartbeat tick
 * below is now triggered EXPLICITLY, via linux_bridge_exporter_test_set_clock()'s
 * test-controlled fake clock (advance_fake_clock()), rather than by
 * sleeping some fixed real duration and hoping a tick happened to fire
 * inside that window. This is what makes every scenario's OUTCOME
 * independent of real wall-clock scheduling jitter and of how much
 * cumulative real time earlier scenarios in this same process consumed --
 * the old design's actual source of ~7% flakiness (a short, but still
 * real-time-dependent, window occasionally not being quite long enough on
 * a loaded machine). exporter_task() itself still polls on its own real,
 * bounded ~200ms tick (EXPORTER_POLL_TICK_MS) -- advancing the fake clock
 * only makes the interval COMPARISON true; the background task still needs
 * up to one real poll tick to notice and act on it, so every wait below is
 * still a genuine (bounded, short) real wait, never an assumption of
 * instantaneous effect.
 *
 * The ONE exception is the "no busy loop" scenario, which is inherently
 * about real elapsed time (how much periodic activity happens over a real
 * 1-second window) -- it explicitly restores the real clock
 * (linux_bridge_exporter_test_set_clock(NULL)) for its own duration, then
 * re-installs the fake one afterward.
 *
 * The per-uuid SAMPLE_MIN_INTERVAL_MS rate-limit gate (drain_pending_samples())
 * is a SEPARATE timer that deliberately stays on the REAL clock (see
 * linux_bridge_exporter_test_set_clock()'s own doc comment) -- scenarios
 * that depend on it still use real sleeps for THAT specific wait, but never
 * for "did a resync/heartbeat tick fire".
 *
 * Covers:
 *   - the periodic full TOPOLOGY resync actually re-sends a node's current
 *     state with no new report/event needed
 *   - the periodic full latest-SAMPLE resync actually re-sends a node's
 *     current sample with no new sample needed
 *   - the periodic HEARTBEAT actually fires
 *   - after a Linux-unresponsive episode exhausts one send's bounded
 *     retries, the NEXT periodic resync automatically succeeds once the
 *     simulated Linux host starts responding again -- with no further
 *     explicit trigger from the test
 *   - no busy loop: a generously (but boundedly) pre-queued outcome budget
 *     is never exhausted over a fixed real-time window (a busy loop would
 *     exhaust it almost immediately and abort() the whole process -- see
 *     tools/host_test_stubs/host_pb_bridge_full_sim.c's own
 *     under-queued-outcome policy)
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast_bridge_timers -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_timers tools/test_linux_bridge_exporter_timers.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_timers
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

/* This profile's own configured intervals (tools/host_test_stubs/pogo_fast_bridge_timers/sdkconfig.h) --
 * named here so every advance below is self-documenting about which real
 * Kconfig value it is deliberately exceeding. */
#define RESYNC_INTERVAL_MS 150u
#define HEARTBEAT_INTERVAL_MS 250u
#define SAMPLE_MIN_INTERVAL_MS 300u

/* Correction pass (this follow-up, item 6): the test-controlled fake clock
 * exporter_task() reads instead of the real xTaskGetTickCount() for its
 * own resync/heartbeat interval comparisons -- see
 * linux_bridge_exporter_test_set_clock()'s own doc comment. Starts at an
 * arbitrary non-zero value (never 0, so an accidental "since epoch 0"
 * assumption anywhere would be caught, not masked). */
static volatile uint32_t s_fake_now_ms = 1000;

static TickType_t fake_clock(void)
{
    return (TickType_t)s_fake_now_ms;
}

/* Bumps the fake clock forward by delta_ms -- makes exporter_task()'s next
 * (real, bounded ~200ms poll-tick-away) interval comparison see that much
 * elapsed time, deterministically, regardless of any real wall-clock time
 * actually elapsed. */
static void advance_fake_clock(uint32_t delta_ms)
{
    s_fake_now_ms += delta_ms;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void send_topology_report(const uint8_t module_uuid[TOPOLOGY_UUID_LEN], bool *out_sent, otCoapCode *out_code)
{
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, POWER_BASE_UUID, TOPOLOGY_UUID_LEN);
    rep.topology_depth = 1;
    rep.role = TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = 1000;
    rep.sequence = 1;
    rep.upstream_connected = true;
    rep.downstream_connected = false;
    rep.report_type = TOPOLOGY_REPORT_ATTACHED;
    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

static void send_sensor_sample(const uint8_t module_uuid[SENSOR_UUID_LEN], uint32_t seq, bool *out_sent,
                                otCoapCode *out_code)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memcpy(s.module_uuid, module_uuid, SENSOR_UUID_LEN);
    s.sample_seq = seq;
    s.uptime_ms = 1;
    s.sensor_bitmap = 0x0001;
    s.channel_count = 1;
    s.channels[0] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI,
                                                 .status = SENSOR_CHANNEL_STATUS_OK, .value = 12345};
    uint8_t wire[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t len = sensor_channel_pack(&s, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("s", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
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

/* Counts captured sends (since `since`) matching `uri_path`. */
static int count_since(size_t since, const char *uri_path)
{
    int n = 0;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (host_pb_bridge_full_sim_get_captured(i, &cap) && strcmp(cap.uri_path, uri_path) == 0) {
            n++;
        }
    }
    return n;
}

/* Bounded, real (but short) wait for count_since(since, uri_path) to reach
 * at least `expected` -- used AFTER an explicit advance_fake_clock() call,
 * so this is only ever waiting for the background task's own real
 * poll-tick + send latency, never for a resync/heartbeat interval itself
 * to elapse in real time. */
static bool wait_for_count_since(size_t since, const char *uri_path, int expected, int timeout_ms)
{
    int waited = 0;
    while (count_since(since, uri_path) < expected && waited < timeout_ms) {
        sleep_ms(5);
        waited += 5;
    }
    return count_since(since, uri_path) >= expected;
}

/* Correction pass: counts captured SAMPLE publishes (since `since`) for the
 * specific `uuid` -- unlike count_since() above (which counts across ALL
 * uuids), this is needed to check ONE uuid's own per-uuid rate-limit
 * window in isolation while other nodes attached earlier in this file keep
 * periodically resyncing in the background. */
static int count_samples_for_uuid(size_t since, const uint8_t uuid[SENSOR_UUID_LEN])
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
        if (memcmp(decoded.module_uuid, uuid, SENSOR_UUID_LEN) == 0) {
            n++;
        }
    }
    return n;
}

int main(void)
{
    host_pb_bridge_full_sim_reset();
    power_base_transport_init(POWER_BASE_UUID);

    /* Installed BEFORE init() -- exporter_task() captures its initial
     * last_resync/last_heartbeat via this same source almost immediately
     * once started, so installing the override any later risks a race
     * where that first capture uses the real clock while every later
     * comparison uses the fake one; TickType_t being unsigned, that
     * mismatch can wrap into a huge bogus "elapsed" value and fire a
     * resync immediately (found and fixed during this correction pass). */
    linux_bridge_exporter_test_set_clock(fake_clock);

    power_base_bridge_config_t cfg = {0};
    cfg.enabled = true;
    memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    cfg.port = 5684;
    /* Pre-queue a generous but BOUNDED budget of successes up front -- see
     * this file's own top comment: a real busy-loop bug would exhaust this
     * almost instantly and abort() the whole process well before any
     * timing-based CHECK() below even runs, which is itself a strong
     * "no busy loop" signal distinct from the explicit budget check at the
     * end of this test. */
    for (int i = 0; i < 200; i++) {
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
    }
    CHECK(linux_bridge_exporter_init(POWER_BASE_UUID, &cfg) == ESP_OK, "linux_bridge_exporter_init succeeds");

    uint8_t node[TOPOLOGY_UUID_LEN] = {0xA1, 0, 0, 0, 0, 0, 0, 0x01};
    bool sent;
    otCoapCode code;

    printf("\n-- setup: attach one node with topology + a sample, let the immediate exports settle --\n");
    send_topology_report(node, &sent, &code);
    CHECK(code == OT_COAP_CODE_CHANGED, "topology attach applied");
    CHECK(wait_for_send_count(1, 2000), "the immediate topology export settles");
    send_sensor_sample(node, 1, &sent, &code);
    CHECK(code == OT_COAP_CODE_CHANGED, "sample stored");
    CHECK(wait_for_send_count(2, 2000), "the immediate sample export settles");

    printf("\n-- scenario (correction pass): the periodic full TOPOLOGY resync fires with no new report, "
           "triggered by an EXPLICIT fake-clock advance past RESYNC_INTERVAL_MS -- never by hoping a real tick "
           "happens to land inside a fixed sleep window --\n");
    {
        size_t since = host_pb_bridge_full_sim_captured_count();
        advance_fake_clock(RESYNC_INTERVAL_MS + 10);
        CHECK(wait_for_count_since(since, "pogo/v1/topology", 1, 1000),
              "a periodic topology resync export appeared with no new topology report triggering it");
    }

    printf("\n-- scenario (correction pass): the periodic full latest-SAMPLE resync fires with no new sample "
           "--\n");
    {
        size_t since = host_pb_bridge_full_sim_captured_count();
        advance_fake_clock(RESYNC_INTERVAL_MS + 10);
        /* do_full_sample_resync() only MARKS this uuid pending (instant) --
         * the actual publish still waits for its own real
         * SAMPLE_MIN_INTERVAL_MS window (a SEPARATE, real-clock timer, see
         * this file's own top comment), which may not have reopened yet
         * since the setup sample above. A short real wait covers that
         * remaining real gate, bounded well above SAMPLE_MIN_INTERVAL_MS. */
        CHECK(wait_for_count_since(since, "pogo/v1/sample", 1, SAMPLE_MIN_INTERVAL_MS + 500),
              "a periodic sample resync export appeared with no new sample triggering it");
    }

    printf("\n-- scenario (correction pass): the periodic HEARTBEAT fires --\n");
    {
        size_t since = host_pb_bridge_full_sim_captured_count();
        advance_fake_clock(HEARTBEAT_INTERVAL_MS + 10);
        CHECK(wait_for_count_since(since, "pogo/v1/heartbeat", 1, 1000), "a periodic heartbeat export appeared");
    }

    printf("\n-- scenario (correction pass): after a Linux-unresponsive episode (3 consecutive bounded-retry "
           "failures somewhere in the send stream), periodic resync keeps automatically succeeding afterward -- "
           "each of the several resync ticks this checks for is triggered by its OWN explicit fake-clock "
           "advance, never by a fixed real sleep hoping enough ticks fired inside it --\n");
    {
        /* The exporter's topology/sample/heartbeat sends share ONE global
         * outcome FIFO in this sim, so exactly which message type consumes
         * these 3 failures is not pinned down here -- the property under
         * test is resilience regardless: whichever send(s) hit these 3
         * NO_RESPONSE outcomes exhaust their own bounded retry budget and
         * give up (see linux_bridge_exporter.c's transmit()), and every
         * later send -- for either message type -- still succeeds on its
         * own, with no further test-side intervention. */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        for (int i = 0; i < 100; i++) {
            host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        }

        size_t since = host_pb_bridge_full_sim_captured_count();
        /* Drive 4 explicit resync ticks, one at a time, each with its own
         * bounded real wait for the exporter's background task to notice
         * and act -- deterministic regardless of how the 3 injected
         * failures landed among them. The per-tick wait is generous enough
         * (900ms) to cover the WORST case where this very tick's own send
         * is one of the 3 injected failures and has to exhaust transmit()'s
         * full bounded retry+backoff schedule (0+200+500ms) before giving
         * up -- a fixed short sleep here (this correction pass's first
         * attempt used 50ms) can under-wait exactly that case and flake. */
        for (int tick = 0; tick < 4; tick++) {
            int total_before = host_pb_bridge_full_sim_send_count();
            advance_fake_clock(RESYNC_INTERVAL_MS + 10);
            int waited = 0;
            while (host_pb_bridge_full_sim_send_count() == total_before && waited < 900) {
                sleep_ms(10);
                waited += 10;
            }
        }
        int topo_count = count_since(since, "pogo/v1/topology");
        int sample_count = count_since(since, "pogo/v1/sample");
        CHECK(topo_count >= 2, "periodic TOPOLOGY resync keeps succeeding across multiple explicit ticks despite "
                                "the 3 injected failures somewhere in the stream -- TOPOLOGY has no per-uuid rate "
                                "limit, so it fires once per resync tick");
        /* Correction pass: SAMPLE resync no longer dispatches directly on
         * every resync tick -- it only marks pending, and the actual
         * publish is gated to at most once per this stub's own
         * SAMPLE_MIN_INTERVAL_MS (see the "test list item 2" scenario
         * further down for a focused, single-uuid proof of that gate). So
         * 4 resync ticks now yield roughly 1-2 SAMPLE publishes for this
         * node (not "one per tick", which is exactly the burst this
         * correction pass fixes) -- this assertion still proves recovery
         * is automatic (at least one publish happened) while also locking
         * in the new bounded-by-the-gate upper bound.
         */
        CHECK(sample_count >= 1 && sample_count <= 3,
              "periodic SAMPLE resync ALSO keeps succeeding after the injected failures -- recovery is automatic "
              "(at least 1 publish), but now bounded by the SAME per-uuid rate-limit gate a reactive publish "
              "uses, never one publish per resync tick like before this correction pass");
    }

    printf("\n-- scenario: no busy loop -- the generous 200-send budget is never exhausted over a REAL 1s window "
           "-- this property is inherently about real elapsed time, so the REAL clock is restored for this one "
           "check only, then the fake clock is re-installed afterward --\n");
    {
        linux_bridge_exporter_test_set_clock(NULL); /* real clock, for this real-time check only */
        int before = host_pb_bridge_full_sim_send_count();
        sleep_ms(1000);
        int after = host_pb_bridge_full_sim_send_count();
        linux_bridge_exporter_test_set_clock(fake_clock); /* restore the fake/explicit clock */
        int delta = after - before;
        CHECK(delta > 0, "some periodic activity happened over the real 1s window under the REAL production "
                          "intervals (150ms resync + 250ms heartbeat) -- the task is not stalled");
        CHECK(delta < 150,
              "the number of sends in a real 1s window is bounded and proportional to the configured periodic "
              "intervals -- not a busy loop, which would have exhausted the pre-queued outcome budget and "
              "aborted this process long before reaching this check");
    }

    printf("\n-- scenario (correction pass, item 2): observer_on_sample_stored() never blocks the OT-task "
           "context, even while contending with the exporter's own background task for "
           "s_pending_samples_mutex -- and the periodic sample resync still delivers the LATEST value "
           "afterward --\n");
    {
        uint8_t nb_node[TOPOLOGY_UUID_LEN] = {0x9B, 0, 0, 0, 0, 0, 0, 0x01};
        bool s2;
        otCoapCode c2;
        int base_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report(nb_node, &s2, &c2);
        CHECK(c2 == OT_COAP_CODE_CHANGED, "setup: the non-blocking-test node attaches OK");
        CHECK(wait_for_send_count(base_sends + 1, 2000), "setup: its topology export settles");

        /* A generous budget for whatever periodic resync/heartbeat activity
         * this scenario's own explicit clock advances (below) trigger --
         * this scenario's own point is call LATENCY, not exact counts. */
        for (int i = 0; i < 50; i++) {
            host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        }

        /* Fire a rapid, tight-loop burst of samples for the SAME uuid,
         * interleaving explicit fake-clock resync-tick advances throughout
         * so the exporter's own background task is genuinely also
         * touching s_pending_samples_mutex (via
         * drain_pending_samples()/do_full_sample_resync()) throughout,
         * i.e. this is REAL cross-thread contention on the exact mutex
         * upsert_pending_sample() takes, not merely a timing measurement in
         * isolation. Each call's wall-clock latency is measured: with the
         * 0-tick take (this correction pass), NO call can ever block
         * regardless of contention -- it either takes the mutex
         * immediately or gives up immediately. */
        enum { kBurstCount = 300 };
        long max_latency_ns = 0;
        for (int i = 0; i < kBurstCount; i++) {
            if (i % 20 == 0) {
                advance_fake_clock(RESYNC_INTERVAL_MS + 10); /* keep resync ticking throughout the burst */
            }
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            send_sensor_sample(nb_node, (uint32_t)(i + 1), &s2, &c2);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
            if (elapsed_ns > max_latency_ns) {
                max_latency_ns = elapsed_ns;
            }
        }
        printf("(worst-case single dispatch latency over %d rapid samples: %ld us)\n", kBurstCount,
               max_latency_ns / 1000);
        /* The OLD code's mutex take could block up to 20 ticks (20ms) under
         * contention -- this asserts comfortably under that bound (5ms),
         * which a genuinely non-blocking 0-tick take always satisfies
         * regardless of how much real contention actually occurred. */
        CHECK(max_latency_ns < 5000000L,
              "every single dispatch call (which synchronously runs observer_on_sample_stored() in this test's "
              "own thread) completes in well under 5ms -- never blocks on s_pending_samples_mutex even under "
              "real contention with the exporter's own background task");

        /* Regardless of how many of the 300 reactive upserts above were won
         * or dropped by contention, the periodic full sample resync must
         * still converge on the LATEST value (seq == kBurstCount) within a
         * bounded number of resync ticks -- explicitly driven here too,
         * rather than hoping enough real time passes. */
        size_t since = host_pb_bridge_full_sim_captured_count();
        bool found_latest = false;
        for (int tick = 0; tick < 20 && !found_latest; tick++) {
            advance_fake_clock(RESYNC_INTERVAL_MS + 10);
            int waited = 0;
            while (waited < 100 && !found_latest) {
                sleep_ms(5);
                waited += 5;
                size_t total = host_pb_bridge_full_sim_captured_count();
                for (size_t idx = since; idx < total; idx++) {
                    host_pb_bridge_full_sim_sent_t cap;
                    if (!host_pb_bridge_full_sim_get_captured(idx, &cap)) {
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
                    if (memcmp(decoded.module_uuid, nb_node, SENSOR_UUID_LEN) != 0) {
                        continue;
                    }
                    if (decoded.sample_seq == (uint32_t)kBurstCount) {
                        found_latest = true;
                        break;
                    }
                }
            }
        }
        CHECK(found_latest,
              "the periodic full sample resync eventually delivers the LATEST sample (seq=300) for this uuid, "
              "regardless of how many of the 300 rapid reactive upserts were dropped by mutex contention along "
              "the way -- aggregator state + periodic resync is the recovery path, not a busy retry");
    }

    printf("\n-- scenario (correction pass, test list item 2): a reactive SAMPLE publish immediately followed by "
           "one or more periodic full-resync ticks never produces a SECOND SAMPLE publish for the SAME uuid "
           "before its own real SAMPLE_MIN_INTERVAL_MS rate-limit window reopens -- resync now shares the exact "
           "same gate as a reactive publish (do_full_sample_resync() only marks pending, it never dispatches "
           "directly), instead of bypassing it --\n");
    {
        uint8_t r_node[TOPOLOGY_UUID_LEN] = {0xAB, 0, 0, 0, 0, 0, 0, 0x01};
        bool s3;
        otCoapCode c3;

        /* Generous extra budget for this scenario's own window. */
        for (int i = 0; i < 50; i++) {
            host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        }

        send_topology_report(r_node, &s3, &c3);
        CHECK(c3 == OT_COAP_CODE_CHANGED, "setup: the rate-limit-vs-resync test node attaches OK");

        send_sensor_sample(r_node, 1, &s3, &c3);
        CHECK(c3 == OT_COAP_CODE_CHANGED, "setup: its first sample is stored fine");

        size_t since;
        bool first_publish_seen = false;
        for (int tries = 0; tries < 200 && !first_publish_seen; tries++) {
            sleep_ms(5);
            if (count_samples_for_uuid(0, r_node) >= 1) {
                first_publish_seen = true;
            }
        }
        CHECK(first_publish_seen, "the first (reactive) SAMPLE publish for this uuid is observed");
        since = host_pb_bridge_full_sim_captured_count();

        /* Explicitly fire ONE resync tick (marks r_node pending again) --
         * still well before its own real SAMPLE_MIN_INTERVAL_MS window
         * (opened right when the first publish above was dispatched)
         * reopens. */
        advance_fake_clock(RESYNC_INTERVAL_MS + 10);
        sleep_ms(100); /* bounded real settle time for this one tick, well under SAMPLE_MIN_INTERVAL_MS */
        CHECK(count_samples_for_uuid(since, r_node) == 0,
              "even though a periodic full-resync tick just fired (marking this uuid pending again), NO second "
              "SAMPLE publish for it has been dispatched yet -- resync no longer bypasses the per-uuid "
              "rate-limit gate");

        /* Now let the real SAMPLE_MIN_INTERVAL_MS window genuinely reopen,
         * firing a couple more explicit resync ticks along the way too --
         * exactly one further publish should appear, never a burst of
         * one-per-tick. */
        sleep_ms(SAMPLE_MIN_INTERVAL_MS + 50);
        advance_fake_clock(RESYNC_INTERVAL_MS + 10);
        sleep_ms(100);
        int after = count_samples_for_uuid(since, r_node);
        CHECK(after == 1, "once the uuid's own rate-limit window reopens, exactly ONE further SAMPLE publish "
                            "appears -- not zero (resync did mark it pending, so it does eventually publish) and "
                            "not more than one (the extra resync tick during the wait never produced a burst)");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
