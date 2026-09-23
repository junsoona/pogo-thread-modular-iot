/*
 * Host-compiled test for the SAMPLE retry-exhaustion recovery path added in
 * this correction pass to components/linux_bridge_exporter/linux_bridge_exporter.c
 * -- see that file's own top comment ("Per-attempt fresh payload" paragraph):
 * when transmit_sample()'s bounded EXPORTER_MAX_ATTEMPTS budget is fully
 * exhausted by transient failures with no new sample ever arriving,
 * handle_sample_event() marks the uuid pending again so the NEXT time its
 * rate-limit window opens, drain_pending_samples() republishes whatever is
 * CURRENT in the aggregator -- bounded, never a busy loop, never silently
 * dropped forever.
 *
 * Split into its OWN process/file (correction pass) rather than folded into
 * tools/test_linux_bridge_exporter_rate_limit.c: this scenario alone needs
 * roughly 2 seconds of real wall-clock time (a full 3-attempt bounded retry
 * sequence at production backoff values -- 0+200+500ms plus a ~40ms
 * ack-timeout wait per attempt -- followed by waiting for the 1000ms
 * rate-limit window to genuinely reopen), and this file's own production
 * CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS is 5000ms (see
 * tools/host_test_stubs/pogo_fast/sdkconfig.h) -- combining this scenario
 * with several OTHER scenarios that also each need a few hundred
 * milliseconds in the SAME process risked the cumulative total creeping
 * past 5000ms and letting an unaccounted periodic resync fire mid-test,
 * consuming an outcome this file did not queue for it (host_pb_bridge_full_sim
 * aborts on an under-queued call, by design -- see that file's own doc
 * comment). Giving this one scenario the full 5000ms budget entirely to
 * itself removes that risk with a comfortable margin.
 *
 * Links the REAL, unmodified linux_bridge_exporter.c, power_base_transport.c,
 * topology_registry.c, topology_proto.c, sensor_channel.c,
 * sensor_root_aggregator.c, bridge_export_proto.c, pogo_crc16.c, and
 * coap_response_timing.c, same as tools/test_linux_bridge_exporter.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_retry_exhaustion tools/test_linux_bridge_exporter_retry_exhaustion.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_retry_exhaustion
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

typedef struct {
    uint32_t seq;
    int32_t value;
} sample_attempt_t;

/* Collects EVERY captured SAMPLE attempt (in capture order) for `uuid`
 * since `since` -- see tools/test_linux_bridge_exporter_rate_limit.c's own
 * identical helper for the full rationale (verifying the FULL ordered
 * attempt sequence, not just the last capture). */
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

static int count_sample_attempts(size_t since, const uint8_t uuid[SENSOR_UUID_LEN])
{
    sample_attempt_t tmp[16];
    return collect_sample_attempts(since, uuid, tmp, 16);
}

/* Correction pass (this follow-up, item 6): freezes exporter_task()'s own
 * full-resync/heartbeat interval clock so neither can EVER fire
 * spontaneously during this file's run -- see
 * test_linux_bridge_exporter.c's own identical comment for the full
 * rationale. */
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

    printf("\n-- scenario (correction pass, test list item 3): when ALL bounded transient-failure retries for a "
           "SAMPLE publish are exhausted with no new callback arriving, the uuid is left pending again -- the "
           "NEXT allowed window (never sooner -- no busy loop) republishes whatever is CURRENT in the "
           "aggregator, even if that is the SAME unchanged value --\n");
    {
        uint8_t node[TOPOLOGY_UUID_LEN];
        make_uuid(node, 0xD2);
        bool sent;
        otCoapCode code;

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node, TOPOLOGY_REPORT_ATTACHED, 1, &sent, &code);
        sleep_ms(100);

        /* All 3 bounded attempts transiently fail -- no new sample ever
         * arrives for this uuid during or after this. */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        size_t since = host_pb_bridge_full_sim_captured_count();
        send_sensor_sample_v(node, 7, 77777, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "seq=7 is stored fine at the aggregator level");

        bool exhausted = false;
        for (int tries = 0; tries < 400 && !exhausted; tries++) {
            sleep_ms(5);
            if (count_sample_attempts(since, node) >= 3) {
                exhausted = true;
            }
        }
        CHECK(exhausted, "all 3 bounded attempts were made and exhausted (transient failure each time)");

        sample_attempt_t attempts[8];
        int n_exhausted = collect_sample_attempts(since, node, attempts, 8);
        CHECK(n_exhausted == 3, "exactly 3 attempts were made -- the bounded retry budget, never more");
        bool all_seq7 = true;
        for (int i = 0; i < n_exhausted; i++) {
            if (attempts[i].seq != 7) {
                all_seq7 = false;
            }
        }
        CHECK(all_seq7, "every one of the 3 exhausted attempts carried seq=7 -- nothing else was ever stored, so "
                          "the re-fetch-per-attempt behavior correctly kept using the one value that existed");

        /* No busy loop: the 3-attempt sequence itself (0 + 200 + 500ms
         * backoff, plus a ~40ms ack-timeout wait per attempt in this stub)
         * already consumes close to 1000ms of the SAME window it was
         * dispatched in -- so this check only has a short margin before
         * the window reopens. A brief, well-under-that-margin sleep is
         * enough to prove no EXTRA attempt appears immediately upon
         * exhaustion (which a busy-loop bug would produce). */
        sleep_ms(80);
        CHECK(count_sample_attempts(since, node) == 3, "no busy-loop extra attempt appears immediately upon retry "
                                                          "exhaustion -- still exactly the 3 attempts, nothing more "
                                                          "yet");

        /* Once the window genuinely reopens, the uuid -- left pending by
         * handle_sample_event()'s own retry-exhaustion path -- is
         * republished automatically, with no new callback needed, still
         * carrying the same (unchanged) seq=7. */
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        bool recovered = false;
        for (int tries = 0; tries < 400 && !recovered; tries++) {
            sleep_ms(5);
            if (count_sample_attempts(since, node) >= 4) {
                recovered = true;
            }
        }
        CHECK(recovered, "once the rate-limit window reopens (bounded, ~1000ms after the first attempt), a 4th "
                           "attempt automatically appears -- the uuid was left pending after retry exhaustion, "
                           "with no new sensor callback required to trigger this recovery");
        uint32_t recovered_seq = 0;
        int32_t recovered_val = 0;
        sample_attempt_t all_attempts[8];
        int n_all = collect_sample_attempts(since, node, all_attempts, 8);
        if (n_all >= 4) {
            recovered_seq = all_attempts[3].seq;
            recovered_val = all_attempts[3].value;
        }
        CHECK(recovered_seq == 7 && recovered_val == 77777, "the recovery publish carries whatever is CURRENT in "
                                                               "the aggregator (still seq=7, since nothing newer "
                                                               "ever arrived) -- bounded recovery with the correct "
                                                               "value, not a corrupted or arbitrary one");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
