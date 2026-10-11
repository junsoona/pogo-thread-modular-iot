/*
 * Host-compiled test: linux_bridge_exporter_suspend().
 *
 * When a Power Base starts to unenroll (docs/power-base-management.md,
 * section 4, step B) it must stop producing new work for its hub before the
 * release is journalled: nothing is read, sent or persisted by the exporter
 * any more, whatever the POGO chain and the timers do afterwards. The hub
 * removes this Base's modules itself when it verifies the completion proof,
 * so a module detaching during the unenroll must not leave a tombstone that
 * a later enrollment would deliver to another hub.
 *
 * Scenarios (each in its own forked process):
 *   A. control: without suspend, new samples, a detach and the periodic
 *      resync all reach Linux (so B's "nothing" is not an artefact of the
 *      harness).
 *   B. after suspend: new samples, topology changes, a direct DOWNSTREAM
 *      detach and several resync intervals send nothing at all, and no
 *      tombstone is journalled.
 *   C. suspend while exports are failing (Linux unreachable): the pending
 *      work is not retried either.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_suspend tools/test_linux_bridge_exporter_suspend.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_suspend
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "linux_bridge_exporter/linux_bridge_exporter.h"
#include "power_base_transport/power_base_transport.h"
#include "bridge_export_proto/bridge_export_proto.h"
#include "sensor_channel/sensor_channel.h"
#include "host_pb_bridge_full_sim.h"
#include "nvs_sim.h"

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

static const uint8_t PB[TOPOLOGY_UUID_LEN] = {0xB0, 0, 0, 0, 0, 0, 0, 0x01};
#define RESYNC_INTERVAL_MS 5000u /* pogo_fast CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS */
#define SETTLE_QUIET_MS 1200     /* > one tombstone's full 1+2 attempt schedule, and > SAMPLE_MIN_INTERVAL_MS */

static volatile uint32_t s_fake_now_ms = 1000;
static TickType_t fake_clock(void)
{
    return (TickType_t)s_fake_now_ms;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void settle(int quiet_ms)
{
    int last = host_pb_bridge_full_sim_send_count();
    int quiet = 0;
    while (quiet < quiet_ms) {
        sleep_ms(20);
        int now = host_pb_bridge_full_sim_send_count();
        if (now != last) {
            last = now;
            quiet = 0;
        } else {
            quiet += 20;
        }
    }
}

static void drive_resync_ticks(int ticks)
{
    for (int i = 0; i < ticks; i++) {
        s_fake_now_ms += RESYNC_INTERVAL_MS + 10;
        settle(SETTLE_QUIET_MS);
    }
}

static void make_uuid(uint8_t out[TOPOLOGY_UUID_LEN], uint8_t tag)
{
    memset(out, 0, TOPOLOGY_UUID_LEN);
    out[6] = 0xD0;
    out[7] = tag;
}

static otCoapCode report(const uint8_t uuid[TOPOLOGY_UUID_LEN], const uint8_t parent[TOPOLOGY_UUID_LEN],
                         uint8_t depth, topology_report_type_t type, uint32_t gen, uint32_t seq, bool downstream)
{
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, parent, TOPOLOGY_UUID_LEN);
    rep.topology_depth = depth;
    rep.role = downstream ? TOPOLOGY_ROLE_INTERMEDIATE : TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = gen;
    rep.sequence = seq;
    rep.upstream_connected = (type != TOPOLOGY_REPORT_DETACHED);
    rep.downstream_connected = downstream;
    rep.report_type = type;
    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    bool sent = false;
    otCoapCode code = 0;
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, &sent, &code);
    return code;
}

static otCoapCode sample(const uint8_t uuid[TOPOLOGY_UUID_LEN], uint32_t seq)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memcpy(s.module_uuid, uuid, SENSOR_UUID_LEN);
    s.sample_seq = seq;
    s.uptime_ms = 100 + seq;
    s.sensor_bitmap = 0x0001;
    s.channel_count = 1;
    s.channels[0] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI,
                                                 .status = SENSOR_CHANNEL_STATUS_OK, .value = 21500};
    uint8_t wire[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t len = sensor_channel_pack(&s, wire, sizeof(wire));
    bool sent = false;
    otCoapCode code = 0;
    host_pb_bridge_full_sim_dispatch_incoming("s", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, &sent, &code);
    return code;
}

typedef enum { K_SAMPLE, K_SNAPSHOT, K_REMOVED } kind_t;

/* Counts captured exports of `kind` naming uuid at or after `since`;
 * *out_last_online gets the last SNAPSHOT's online flag. */
static int count_exports(size_t since, kind_t kind, const uint8_t uuid[TOPOLOGY_UUID_LEN], bool *out_last_online)
{
    int n = 0;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap) ||
            !bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len)) {
            continue;
        }
        if (kind == K_SAMPLE && type == BRIDGE_EXPORT_MSG_SAMPLE) {
            bridge_export_sample_t exp;
            sensor_sample_t s;
            if (bridge_export_unpack_sample(payload, payload_len, &exp) &&
                sensor_channel_unpack(exp.sensor_channel_payload, exp.sensor_channel_len, &s) &&
                memcmp(s.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
                n++;
            }
        } else if (kind == K_SNAPSHOT && type == BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT) {
            bridge_export_topology_node_t node;
            if (bridge_export_unpack_topology_node(payload, payload_len, &node) &&
                memcmp(node.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
                n++;
                if (out_last_online != NULL) {
                    *out_last_online = node.online;
                }
            }
        } else if (kind == K_REMOVED && type == BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED) {
            bridge_export_topology_removed_t rem;
            if (bridge_export_unpack_topology_removed(payload, payload_len, &rem) &&
                memcmp(rem.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
                n++;
            }
        }
    }
    return n;
}

typedef void (*scenario_fn_t)(void);

static int run_scenario(const char *title, scenario_fn_t fn)
{
    printf("\n-- %s --\n", title);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        nvs_sim_reset();
        host_pb_bridge_full_sim_reset();
        host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_CHANGED);
        power_base_transport_init(PB);
        linux_bridge_exporter_test_set_clock(fake_clock);
        power_base_bridge_config_t cfg = {0};
        cfg.enabled = true;
        memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
        cfg.port = 5684;
        CHECK(linux_bridge_exporter_init(PB, &cfg) == ESP_OK, "linux_bridge_exporter_init");
        sleep_ms(300); /* let exporter_task() capture its initial resync time from the fake clock */
        fn();
        fflush(stdout);
        _exit(g_failures > 255 ? 255 : g_failures);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        printf("FAIL: scenario process did not exit normally\n");
        return 1;
    }
    return WEXITSTATUS(status);
}

static uint8_t P[TOPOLOGY_UUID_LEN], L[TOPOLOGY_UUID_LEN];

/* Parent P (depth 1, INTERMEDIATE) with leaf L (depth 2) below it, both
 * with a stored sample that has already been exported. */
static void setup_chain(void)
{
    CHECK(report(P, PB, 1, TOPOLOGY_REPORT_ATTACHED, 10, 1, true) == OT_COAP_CODE_CHANGED, "P attaches (depth 1)");
    CHECK(report(L, P, 2, TOPOLOGY_REPORT_ATTACHED, 20, 1, false) == OT_COAP_CODE_CHANGED, "L attaches below P");
    CHECK(sample(P, 1) == OT_COAP_CODE_CHANGED && sample(L, 1) == OT_COAP_CODE_CHANGED, "both store a sample");
    settle(SETTLE_QUIET_MS);
    CHECK(count_exports(0, K_SAMPLE, L, NULL) >= 1, "L's sample reached Linux before the detach");
}

static void scenario_control(void)
{
    setup_chain();
    size_t since = host_pb_bridge_full_sim_captured_count();
    CHECK(sample(P, 2) == OT_COAP_CODE_CHANGED, "a new sample is stored");
    settle(SETTLE_QUIET_MS);
    CHECK(count_exports(since, K_SAMPLE, P, NULL) >= 1, "without suspend the new sample reaches Linux");
    size_t before_resync = host_pb_bridge_full_sim_captured_count();
    drive_resync_ticks(1);
    CHECK(host_pb_bridge_full_sim_captured_count() > before_resync, "without suspend the periodic resync sends");
    size_t before_detach = host_pb_bridge_full_sim_captured_count();
    power_base_transport_on_direct_downstream_detached();
    settle(SETTLE_QUIET_MS);
    CHECK(count_exports(before_detach, K_REMOVED, P, NULL) == 1, "without suspend a detach is tombstoned");
}

static void scenario_suspended(void)
{
    setup_chain();
    linux_bridge_exporter_suspend();
    settle(SETTLE_QUIET_MS);
    size_t since = host_pb_bridge_full_sim_captured_count();
    int sends = host_pb_bridge_full_sim_send_count();

    CHECK(sample(P, 2) == OT_COAP_CODE_CHANGED && sample(L, 2) == OT_COAP_CODE_CHANGED,
          "the transport still stores samples (the POGO side is stopped separately)");
    CHECK(report(L, P, 2, TOPOLOGY_REPORT_UPDATED, 20, 2, false) == OT_COAP_CODE_CHANGED, "a topology update is applied");
    settle(SETTLE_QUIET_MS);
    CHECK(host_pb_bridge_full_sim_captured_count() == since, "no SAMPLE or SNAPSHOT is exported after suspend");

    power_base_transport_on_direct_downstream_detached();
    settle(SETTLE_QUIET_MS);
    CHECK(count_exports(since, K_REMOVED, P, NULL) == 0 && count_exports(since, K_REMOVED, L, NULL) == 0,
          "a detach during the unenroll sends no tombstone");

    drive_resync_ticks(3);
    CHECK(host_pb_bridge_full_sim_captured_count() == since, "no resync and no heartbeat after suspend");
    CHECK(host_pb_bridge_full_sim_send_count() == sends, "nothing at all was handed to the transport");

    /* Suspending twice changes nothing. */
    linux_bridge_exporter_suspend();
    drive_resync_ticks(1);
    CHECK(host_pb_bridge_full_sim_captured_count() == since, "still nothing after a second suspend");
}

static void scenario_suspended_while_failing(void)
{
    setup_chain();
    /* Linux stops answering: the next exports stay pending and are retried. */
    host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_BAD_REQUEST);
    CHECK(sample(P, 2) == OT_COAP_CODE_CHANGED, "a sample is stored while Linux refuses");
    power_base_transport_on_direct_downstream_detached();
    settle(SETTLE_QUIET_MS);

    linux_bridge_exporter_suspend();
    settle(SETTLE_QUIET_MS);
    size_t since = host_pb_bridge_full_sim_captured_count();
    host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_CHANGED);
    drive_resync_ticks(3);
    CHECK(host_pb_bridge_full_sim_captured_count() == since,
          "work that was pending when the unenroll started is not retried either");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    make_uuid(P, 0x57);
    make_uuid(L, 0x7A);
    int failures = 0;
    failures += run_scenario("A: control -- without suspend everything is exported", scenario_control);
    failures += run_scenario("B: after suspend nothing is exported or journalled", scenario_suspended);
    failures += run_scenario("C: suspend while exports are failing", scenario_suspended_while_failing);
    printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
