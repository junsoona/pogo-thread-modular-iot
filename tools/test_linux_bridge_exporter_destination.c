/*
 * Host-compiled test: linux_bridge_exporter_set_destination() -- the hub is at
 * another Thread address or port from now on (docs/power-base-management.md,
 * section 12; the caller got the new endpoint from the hub itself and has
 * already stored it in the enrollment record).
 *
 * The REAL exporter, power_base_transport, topology registry and aggregator
 * run; the test plays the Thread network's CoAP (every outgoing request is
 * captured with the address and port it was put to) and the POGO side.
 *
 * Scenarios (each in its own forked process):
 *   A. the move: everything sent after the call goes to the new endpoint,
 *      and the new endpoint is given a heartbeat, the whole topology and the
 *      latest samples at once -- not at the next periodic resync. Setting the
 *      same endpoint again sends nothing.
 *   B. a request on the air when the destination changes, answered 2.04 by
 *      the endpoint the exporter has left: not a delivery. The sample is sent
 *      again to the new endpoint, and a removal tombstone stays pending until
 *      the NEW endpoint has confirmed it.
 *   C. the old endpoint is dead (what really happens): nothing is delivered
 *      until the move; a tombstone that could not be delivered goes to the
 *      new endpoint.
 *   D. an endpoint the exporter would not have been started with (port 0,
 *      unspecified, multicast, link-local) changes nothing; an exporter that
 *      was never started refuses.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_destination tools/test_linux_bridge_exporter_destination.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_destination
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

typedef enum { K_SAMPLE, K_SNAPSHOT, K_REMOVED, K_HEARTBEAT } kind_t;

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


static const uint8_t OLD_ADDR[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79};
static const uint8_t NEW_ADDR[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x12, 0x9a, 0x77, 0x01, 0xbe, 0xef, 0x40, 0x02};
#define OLD_PORT 5684
#define NEW_PORT 5700

/* Captured sends at or after `since` that went to addr:port. */
static int sends_to(size_t since, const uint8_t addr[16], uint16_t port)
{
    int n = 0;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (host_pb_bridge_full_sim_get_captured(i, &cap) && cap.peer_port == port &&
            memcmp(cap.peer_addr, addr, 16) == 0) {
            n++;
        }
    }
    return n;
}

/* Exports of `kind` (naming `uuid`, unless a heartbeat) at or after `since`
 * that went to addr:port. */
static int exports_to(size_t since, kind_t kind, const uint8_t uuid[TOPOLOGY_UUID_LEN], const uint8_t addr[16],
                      uint16_t port)
{
    int n = 0;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap) || cap.peer_port != port ||
            memcmp(cap.peer_addr, addr, 16) != 0) {
            continue;
        }
        if (kind == K_HEARTBEAT) {
            n += strcmp(cap.uri_path, "pogo/v1/heartbeat") == 0 ? 1 : 0;
        } else {
            n += count_exports(i, kind, uuid, NULL) - count_exports(i + 1, kind, uuid, NULL);
        }
    }
    return n;
}

/* Index of the first captured send at or after `since`, or -1. */
static int first_heartbeat_index(size_t since)
{
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (host_pb_bridge_full_sim_get_captured(i, &cap) && strcmp(cap.uri_path, "pogo/v1/heartbeat") == 0) {
            return (int)i;
        }
    }
    return -1;
}

typedef void (*scenario_fn_t)(void);
static bool s_start_exporter = true;

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
        if (s_start_exporter) {
            power_base_bridge_config_t cfg = {0};
            cfg.enabled = true;
            memcpy(cfg.host_addr, OLD_ADDR, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
            cfg.port = OLD_PORT;
            CHECK(linux_bridge_exporter_init(PB, &cfg) == ESP_OK, "linux_bridge_exporter_init at the old endpoint");
            sleep_ms(300); /* let exporter_task() capture its initial resync time from the fake clock */
        }
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

static void setup_chain(void)
{
    CHECK(report(P, PB, 1, TOPOLOGY_REPORT_ATTACHED, 10, 1, true) == OT_COAP_CODE_CHANGED, "P attaches (depth 1)");
    CHECK(report(L, P, 2, TOPOLOGY_REPORT_ATTACHED, 20, 1, false) == OT_COAP_CODE_CHANGED, "L attaches below P");
    CHECK(sample(P, 1) == OT_COAP_CODE_CHANGED && sample(L, 1) == OT_COAP_CODE_CHANGED, "both store a sample");
    settle(SETTLE_QUIET_MS);
}

static void scenario_move(void)
{
    setup_chain();
    CHECK(exports_to(0, K_SAMPLE, L, OLD_ADDR, OLD_PORT) >= 1 && sends_to(0, NEW_ADDR, NEW_PORT) == 0,
          "before the move everything went to the old endpoint");

    size_t since = host_pb_bridge_full_sim_captured_count();
    CHECK(linux_bridge_exporter_set_destination(NEW_ADDR, NEW_PORT) == ESP_OK, "set_destination(new address, new port)");
    settle(SETTLE_QUIET_MS); /* the fake clock does not move: no periodic resync or heartbeat is due */
    CHECK(sends_to(since, OLD_ADDR, OLD_PORT) == 0, "nothing more is sent to the old endpoint");
    CHECK(exports_to(since, K_HEARTBEAT, NULL, NEW_ADDR, NEW_PORT) == 1, "the new endpoint gets a heartbeat at once");
    CHECK(first_heartbeat_index(since) == (int)since, "and it is the first thing sent there (it registers the Base)");
    CHECK(exports_to(since, K_SNAPSHOT, P, NEW_ADDR, NEW_PORT) >= 1 &&
              exports_to(since, K_SNAPSHOT, L, NEW_ADDR, NEW_PORT) >= 1,
          "the whole topology is sent to the new endpoint, without waiting for the resync interval");
    CHECK(exports_to(since, K_SAMPLE, P, NEW_ADDR, NEW_PORT) >= 1 && exports_to(since, K_SAMPLE, L, NEW_ADDR, NEW_PORT) >= 1,
          "and the latest sample of every module");

    size_t after = host_pb_bridge_full_sim_captured_count();
    CHECK(sample(P, 2) == OT_COAP_CODE_CHANGED, "a new sample is stored");
    settle(SETTLE_QUIET_MS);
    CHECK(exports_to(after, K_SAMPLE, P, NEW_ADDR, NEW_PORT) >= 1 && sends_to(after, OLD_ADDR, OLD_PORT) == 0,
          "later samples go to the new endpoint");
    after = host_pb_bridge_full_sim_captured_count();
    drive_resync_ticks(1);
    CHECK(sends_to(after, NEW_ADDR, NEW_PORT) > 0 && sends_to(after, OLD_ADDR, OLD_PORT) == 0,
          "so does the periodic resync");

    after = host_pb_bridge_full_sim_captured_count();
    CHECK(linux_bridge_exporter_set_destination(NEW_ADDR, NEW_PORT) == ESP_OK, "setting the same endpoint again is accepted");
    settle(SETTLE_QUIET_MS);
    CHECK(host_pb_bridge_full_sim_captured_count() == after, "and sends nothing");
}

/* The destination changes while the request is on the air. */
static volatile int s_switch_on_send; /* 1 = switch at the next send, then 0 */
static void switch_hook(const host_pb_bridge_full_sim_sent_t *sent)
{
    (void)sent;
    if (s_switch_on_send == 1) {
        s_switch_on_send = 0;
        (void)linux_bridge_exporter_set_destination(NEW_ADDR, NEW_PORT);
    }
}

static void scenario_on_the_air(void)
{
    setup_chain();
    host_pb_bridge_full_sim_set_send_hook(switch_hook);

    /* P and everything below it detach: tombstones for P and L. The first
     * of them is on the air, to the old endpoint, when the hub moves; the
     * old endpoint answers 2.04. */
    size_t since = host_pb_bridge_full_sim_captured_count();
    s_switch_on_send = 1;
    power_base_transport_on_direct_downstream_detached();
    settle(SETTLE_QUIET_MS);
    int old_removed = exports_to(since, K_REMOVED, P, OLD_ADDR, OLD_PORT) + exports_to(since, K_REMOVED, L, OLD_ADDR, OLD_PORT);
    CHECK(old_removed == 1, "one tombstone was on the air to the old endpoint when the destination changed");
    CHECK(exports_to(since, K_REMOVED, P, NEW_ADDR, NEW_PORT) >= 1 && exports_to(since, K_REMOVED, L, NEW_ADDR, NEW_PORT) >= 1,
          "both tombstones reach the NEW endpoint: the old endpoint's 2.04 did not count as a delivery");
    CHECK(sends_to(since + 1, OLD_ADDR, OLD_PORT) == 0, "nothing after that request went to the old endpoint");
}

static void scenario_old_endpoint_dead(void)
{
    host_pb_bridge_full_sim_set_default_no_response();
    CHECK(report(P, PB, 1, TOPOLOGY_REPORT_ATTACHED, 10, 1, true) == OT_COAP_CODE_CHANGED, "P attaches (depth 1)");
    CHECK(report(L, P, 2, TOPOLOGY_REPORT_ATTACHED, 20, 1, false) == OT_COAP_CODE_CHANGED, "L attaches below P");
    CHECK(sample(P, 1) == OT_COAP_CODE_CHANGED && sample(L, 1) == OT_COAP_CODE_CHANGED,
          "both store a sample (the Base itself works without its hub)");
    CHECK(report(L, P, 2, TOPOLOGY_REPORT_DETACHED, 20, 2, false) == OT_COAP_CODE_CHANGED, "L detaches");
    sleep_ms(1500); /* not settle(): against a dead endpoint the exporter keeps trying and never goes quiet */
    CHECK(sends_to(0, NEW_ADDR, NEW_PORT) == 0, "nothing was sent anywhere but the old endpoint, which does not answer");

    size_t since = host_pb_bridge_full_sim_captured_count();
    host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_CHANGED);
    CHECK(linux_bridge_exporter_set_destination(NEW_ADDR, OLD_PORT) == ESP_OK, "set_destination(new address, same port)");
    settle(SETTLE_QUIET_MS);
    CHECK(sends_to(since, OLD_ADDR, OLD_PORT) == 0, "nothing more is sent to the old address");
    CHECK(exports_to(since, K_HEARTBEAT, NULL, NEW_ADDR, OLD_PORT) == 1 &&
              exports_to(since, K_SNAPSHOT, P, NEW_ADDR, OLD_PORT) >= 1 &&
              exports_to(since, K_SAMPLE, P, NEW_ADDR, OLD_PORT) >= 1,
          "the new address gets heartbeat, topology and samples at once");
}

static void scenario_refused(void)
{
    setup_chain();
    size_t since = host_pb_bridge_full_sim_captured_count();
    static const uint8_t unspecified[16] = {0};
    static const uint8_t multicast[16] = {0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t link_local[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(linux_bridge_exporter_set_destination(NEW_ADDR, 0) == ESP_ERR_INVALID_ARG, "port 0 is refused");
    CHECK(linux_bridge_exporter_set_destination(unspecified, NEW_PORT) == ESP_ERR_INVALID_ARG, "the unspecified address is refused");
    CHECK(linux_bridge_exporter_set_destination(multicast, NEW_PORT) == ESP_ERR_INVALID_ARG, "a multicast address is refused");
    CHECK(linux_bridge_exporter_set_destination(link_local, NEW_PORT) == ESP_ERR_INVALID_ARG, "a link-local address is refused");
    CHECK(linux_bridge_exporter_set_destination(NULL, NEW_PORT) == ESP_ERR_INVALID_ARG, "NULL is refused");
    CHECK(sample(P, 2) == OT_COAP_CODE_CHANGED, "a new sample is stored");
    settle(SETTLE_QUIET_MS);
    CHECK(exports_to(since, K_SAMPLE, P, OLD_ADDR, OLD_PORT) >= 1 && sends_to(since, NEW_ADDR, NEW_PORT) == 0 &&
              exports_to(since, K_HEARTBEAT, NULL, OLD_ADDR, OLD_PORT) == 0,
          "a refused endpoint changes nothing: the old one stays in use and no resync is triggered");
}

static void scenario_never_started(void)
{
    CHECK(linux_bridge_exporter_set_destination(NEW_ADDR, NEW_PORT) == ESP_ERR_INVALID_STATE,
          "an exporter that was never started refuses (the record holds the endpoint; the next boot starts it there)");
    CHECK(host_pb_bridge_full_sim_send_count() == 0, "and sends nothing");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    make_uuid(P, 0x57);
    make_uuid(L, 0x7A);
    int failures = 0;
    failures += run_scenario("A: the hub moved -- everything goes to the new endpoint, at once", scenario_move);
    failures += run_scenario("B: a request on the air when the destination changes", scenario_on_the_air);
    failures += run_scenario("C: the old endpoint does not answer any more", scenario_old_endpoint_dead);
    failures += run_scenario("D: endpoints that are refused", scenario_refused);
    s_start_exporter = false;
    failures += run_scenario("E: an exporter that was never started", scenario_never_started);
    printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
