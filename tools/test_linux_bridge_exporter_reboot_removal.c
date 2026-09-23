/*
 * Host-compiled regression test: a CONFIRMED POGO detach that has not yet
 * reached Linux must still converge to "removed" after the Power Base
 * reboots.
 *
 * Failure path this reproduces (before the fix):
 *   1. A module is physically detached; power_base_transport erases its
 *      topology entry and linux_bridge_exporter queues a TOPOLOGY_REMOVED.
 *   2. The Linux host is unreachable, so the tombstone is never confirmed.
 *   3. The Power Base reboots. The topology registry and
 *      s_pending_removed_* were RAM only, so the removal is forgotten.
 *   4. Linux keeps the module forever (its own stale timeout can only ever
 *      mark it offline, never removed).
 * The fix keeps the unconfirmed-tombstone set in the default "nvs"
 * partition (components/linux_bridge_exporter/removed_journal.c) and
 * reloads it at boot.
 *
 * Each "boot" runs in a freshly forked child process that never inherits
 * any initialized firmware state from the parent (the parent itself never
 * touches the components under test) -- the ONLY thing carried from one
 * boot to the next is the simulated flash file behind
 * tools/host_test_stubs/nvs_sim (nvs_sim_set_backing_file()). A child ends
 * with _exit(), i.e. no shutdown path runs: a power cut.
 *
 * Scenarios:
 *   A. direct downstream detach (parent + cascaded child), Linux down,
 *      reboot -> both tombstones delivered after reboot; a third boot
 *      sends none (confirmed delivery cleared the journal durably).
 *   B. stale timeout while Linux is down, reboot -> never a tombstone
 *      (offline is not removal).
 *   C. detach, reboot, SAME uuid re-docks before Linux is reachable ->
 *      no tombstone ever; the re-dock's online=true wins.
 *   D. detach, re-dock, reboot -> no tombstone (the cancel was durable).
 *   E. module DETACHED self-report, reboot, tombstone delivered, then the
 *      module re-docks -> the online=true SNAPSHOT is sent after the
 *      REMOVED (latest physical state last), no later REMOVED.
 *   F. the first journal write fails -> the exporter retries it, and the
 *      tombstone still survives the reboot.
 *   G. a detach while the exporter is busy retrying a long resync against
 *      an unreachable Linux is journaled before that resync finishes.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_reboot_removal tools/test_linux_bridge_exporter_reboot_removal.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_reboot_removal
 */
#include <stdio.h>
#include <stdlib.h>
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

static const uint8_t POWER_BASE_UUID[TOPOLOGY_UUID_LEN] = {0xB0, 0, 0, 0, 0, 0, 0, 0x01};
#define RESYNC_INTERVAL_MS 5000u /* pogo_fast profile's CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS */
/* Quiet window long enough to cover one tombstone's whole 1+2 attempt
 * schedule while Linux is unreachable (3 x 40ms ACK timeout + 200ms + 500ms
 * backoff) plus one exporter poll tick. */
#define SETTLE_QUIET_MS 1200

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

/* Waits until no new outgoing send has been made for quiet_ms. */
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

/* One periodic resync (fake clock), then waits for its sends to finish. */
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
    out[7] = tag;
}

static otCoapCode send_report(const uint8_t module_uuid[TOPOLOGY_UUID_LEN],
                              const uint8_t parent_uuid[TOPOLOGY_UUID_LEN], uint8_t depth,
                              topology_report_type_t type, uint32_t generation, uint32_t seq,
                              bool downstream_connected)
{
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, parent_uuid, TOPOLOGY_UUID_LEN);
    rep.topology_depth = depth;
    rep.role = TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = generation;
    rep.sequence = seq;
    rep.upstream_connected = (type != TOPOLOGY_REPORT_DETACHED);
    rep.downstream_connected = downstream_connected;
    rep.report_type = type;
    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    bool sent = false;
    otCoapCode code = 0;
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, &sent, &code);
    return code;
}

/* Index of the last captured TOPOLOGY_REMOVED naming uuid at or after
 * `since`, or -1. */
static int last_removed_index(size_t since, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    int found = -1;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        bridge_export_topology_removed_t rem;
        if (host_pb_bridge_full_sim_get_captured(i, &cap) && strcmp(cap.uri_path, "pogo/v1/topology") == 0 &&
            bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) &&
            type == BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED &&
            bridge_export_unpack_topology_removed(payload, payload_len, &rem) &&
            memcmp(rem.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0 &&
            memcmp(rem.power_base_uuid, POWER_BASE_UUID, TOPOLOGY_UUID_LEN) == 0) {
            found = (int)i;
        }
    }
    return found;
}

/* Index of the last captured TOPOLOGY_SNAPSHOT naming uuid at or after
 * `since` (its online flag via *out_online), or -1. */
static int last_snapshot_index(size_t since, const uint8_t uuid[TOPOLOGY_UUID_LEN], bool *out_online)
{
    int found = -1;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        bridge_export_topology_node_t node;
        if (host_pb_bridge_full_sim_get_captured(i, &cap) && strcmp(cap.uri_path, "pogo/v1/topology") == 0 &&
            bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) &&
            type == BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT &&
            bridge_export_unpack_topology_node(payload, payload_len, &node) &&
            memcmp(node.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
            found = (int)i;
            if (out_online != NULL) {
                *out_online = node.online;
            }
        }
    }
    return found;
}

static bool any_removed(size_t since)
{
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        if (host_pb_bridge_full_sim_get_captured(i, &cap) &&
            bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) &&
            type == BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED) {
            return true;
        }
    }
    return false;
}

static bool wait_for_removed(const uint8_t uuid[TOPOLOGY_UUID_LEN], int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (last_removed_index(0, uuid) >= 0) {
            return true;
        }
        sleep_ms(10);
    }
    return last_removed_index(0, uuid) >= 0;
}

/* ---- boot harness ---- */

static char g_nvs_path[512];

typedef void (*boot_fn_t)(void);

/* Runs one Power Base boot in a fresh child process. Returns the number of
 * failed checks in that boot (a crash counts as one). */
static int run_boot(const char *label, boot_fn_t fn)
{
    printf("\n  [%s]\n", label);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork() failed for %s\n", label);
        return 1;
    }
    if (pid == 0) {
        nvs_sim_reset();
        if (!nvs_sim_set_backing_file(g_nvs_path)) {
            printf("FAIL: could not read simulated flash %s\n", g_nvs_path);
            fflush(stdout);
            _exit(1);
        }
        host_pb_bridge_full_sim_reset();
        host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_CHANGED);
        power_base_transport_init(POWER_BASE_UUID);
        linux_bridge_exporter_test_set_clock(fake_clock);
        power_base_bridge_config_t cfg = {0};
        cfg.enabled = true;
        memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
        cfg.port = 5684;
        CHECK(linux_bridge_exporter_init(POWER_BASE_UUID, &cfg) == ESP_OK, "boot: linux_bridge_exporter_init");
        /* Let exporter_task() start and capture its initial last_resync
         * from the fake clock; advancing the clock before that would make
         * the first drive_resync_ticks() a no-op. */
        sleep_ms(300);
        fn();
        fflush(stdout);
        _exit(g_failures > 255 ? 255 : g_failures); /* power cut: no shutdown path runs */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        printf("FAIL: %s did not exit normally (status=0x%x)\n", label, status);
        return 1;
    }
    return WEXITSTATUS(status);
}

static void new_flash(const char *scenario)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_nvs_path, sizeof(g_nvs_path), "%s/pogo_reboot_removal_%d_%s.nvs", (tmp && *tmp) ? tmp : "/tmp",
             (int)getpid(), scenario);
    unlink(g_nvs_path);
}

/* ---- shared uuids ---- */

static uint8_t X[TOPOLOGY_UUID_LEN], C[TOPOLOGY_UUID_LEN];

static void attach_x_online(bool with_child)
{
    CHECK(send_report(X, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 100, 1, with_child) == OT_COAP_CODE_CHANGED,
          "X attaches (depth 1)");
    if (with_child) {
        CHECK(send_report(C, X, 2, TOPOLOGY_REPORT_ATTACHED, 200, 1, false) == OT_COAP_CODE_CHANGED,
              "C attaches below X (depth 2)");
    }
    settle(300);
}

/* ---- scenario A ---- */

static void a_boot1(void)
{
    attach_x_online(true);
    host_pb_bridge_full_sim_set_default_no_response(); /* Linux link lost */
    power_base_transport_on_direct_downstream_detached();
    CHECK(power_base_transport_get_node_count() == 0, "the confirmed detach erased X and C from the registry");
    CHECK(wait_for_removed(X, 3000) && wait_for_removed(C, 3000),
          "tombstones for X and C were attempted while Linux was unreachable");
    settle(SETTLE_QUIET_MS);
    printf("  (power cut before any tombstone was confirmed)\n");
}

static void a_boot2(void)
{
    CHECK(power_base_transport_get_node_count() == 0, "after reboot the RAM topology registry is empty");
    drive_resync_ticks(1);
    CHECK(last_removed_index(0, X) >= 0, "after reboot, the undelivered TOPOLOGY_REMOVED for X is sent again");
    CHECK(last_removed_index(0, C) >= 0, "after reboot, the undelivered TOPOLOGY_REMOVED for cascaded child C is "
                                          "sent again");
}

static void no_tombstone_boot(void)
{
    drive_resync_ticks(2);
    CHECK(!any_removed(0), "no TOPOLOGY_REMOVED is sent in this boot");
}

/* ---- scenario B ---- */

static void b_boot1(void)
{
    attach_x_online(false);
    host_pb_bridge_full_sim_set_default_no_response();
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    power_base_transport_sweep_stale(now_ms + 600000u, 120000u);
    topology_registry_entry_t e;
    CHECK(power_base_transport_get_node(0, &e) && !e.online, "stale sweep marks X offline, but keeps it");
    settle(SETTLE_QUIET_MS);
    CHECK(!any_removed(0), "a stale timeout never produces a TOPOLOGY_REMOVED");
}

/* ---- scenario C ---- */

static void detach_x_undelivered(void)
{
    attach_x_online(false);
    host_pb_bridge_full_sim_set_default_no_response();
    power_base_transport_on_direct_downstream_detached();
    CHECK(wait_for_removed(X, 3000), "X's tombstone was attempted while Linux was unreachable");
    settle(SETTLE_QUIET_MS);
}

static void c_boot2(void)
{
    host_pb_bridge_full_sim_set_default_no_response(); /* Linux still unreachable */
    CHECK(send_report(X, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 300, 1, false) == OT_COAP_CODE_CHANGED,
          "X physically re-docks after the reboot, before Linux is reachable");
    settle(SETTLE_QUIET_MS);
    host_pb_bridge_full_sim_set_default_response(OT_COAP_CODE_CHANGED); /* Linux back */
    drive_resync_ticks(2);
    bool online = false;
    CHECK(last_removed_index(0, X) < 0, "the restored tombstone for X is never sent once X re-docked");
    CHECK(last_snapshot_index(0, X, &online) >= 0 && online, "Linux is sent X online=true");
}

/* ---- scenario D ---- */

static void d_boot1(void)
{
    detach_x_undelivered();
    size_t since = host_pb_bridge_full_sim_captured_count();
    CHECK(send_report(X, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 300, 1, false) == OT_COAP_CODE_CHANGED,
          "X re-docks before the reboot");
    settle(SETTLE_QUIET_MS);
    CHECK(last_snapshot_index(since, X, NULL) >= 0, "the re-dock SNAPSHOT was attempted");
    printf("  (power cut)\n");
}

/* ---- scenario E ---- */

static void e_boot1(void)
{
    attach_x_online(false);
    host_pb_bridge_full_sim_set_default_no_response();
    CHECK(send_report(X, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_DETACHED, 100, 2, false) == OT_COAP_CODE_CHANGED,
          "X's own DETACHED self-report is applied");
    CHECK(wait_for_removed(X, 3000), "X's tombstone was attempted while Linux was unreachable");
    settle(SETTLE_QUIET_MS);
}

static void e_boot2(void)
{
    drive_resync_ticks(1);
    int removed_at = last_removed_index(0, X);
    CHECK(removed_at >= 0, "after reboot, the tombstone for X is delivered");
    CHECK(send_report(X, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 400, 1, false) == OT_COAP_CODE_CHANGED,
          "X then physically re-docks");
    settle(300);
    drive_resync_ticks(2);
    bool online = false;
    int snap_at = last_snapshot_index(0, X, &online);
    CHECK(removed_at >= 0 && snap_at > removed_at && online, "the last message about X is SNAPSHOT online=true, after the REMOVED");
    CHECK(last_removed_index(0, X) == removed_at, "no further REMOVED for X after the re-dock");
}

/* ---- scenario F ---- */

static void f_boot1(void)
{
    attach_x_online(false);
    host_pb_bridge_full_sim_set_default_no_response();
    nvs_sim_force_next_commit_error(ESP_FAIL);
    power_base_transport_on_direct_downstream_detached();
    CHECK(wait_for_removed(X, 3000), "X's tombstone was attempted while Linux was unreachable");
    settle(SETTLE_QUIET_MS);
}

static void f_boot2(void)
{
    drive_resync_ticks(1);
    CHECK(last_removed_index(0, X) >= 0, "after reboot, the tombstone is still sent (the failed journal write was "
                                         "retried)");
}

/* ---- scenario G ---- */

static void g_boot1(void)
{
    /* Six more depth-1 nodes, so one periodic resync keeps the exporter
     * task busy for several seconds of timeouts while Linux is down. */
    uint8_t m[6][TOPOLOGY_UUID_LEN];
    for (int i = 0; i < 6; i++) {
        make_uuid(m[i], (uint8_t)(0xD0 + i));
        send_report(m[i], POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 500 + i, 1, i == 0);
    }
    CHECK(send_report(X, m[0], 2, TOPOLOGY_REPORT_ATTACHED, 600, 1, false) == OT_COAP_CODE_CHANGED,
          "X attaches below m0 (depth 2)");
    settle(300);
    host_pb_bridge_full_sim_set_default_no_response();
    s_fake_now_ms += RESYNC_INTERVAL_MS + 10; /* a full resync starts: ~0.8 s of timeouts per node */
    sleep_ms(1000);
    CHECK(send_report(m[0], POWER_BASE_UUID, 1, TOPOLOGY_REPORT_UPDATED, 500, 2, false) == OT_COAP_CODE_CHANGED,
          "m0 reports its downstream (X) physically lost, mid-resync");
    sleep_ms(1200);
    CHECK(last_removed_index(0, X) < 0, "(the exporter is still busy with the resync -- X's tombstone not sent yet)");
    printf("  (power cut while the resync is still running)\n");
}

static void g_boot2(void)
{
    drive_resync_ticks(1);
    CHECK(last_removed_index(0, X) >= 0, "after reboot, X's tombstone is sent -- it was journaled between resync "
                                         "attempts, not only once the busy loop iteration ended");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    make_uuid(X, 0xA1);
    make_uuid(C, 0xA2);
    int failures = 0;

    printf("\n-- scenario A: confirmed detach (X + cascaded child C), Linux unreachable, Power Base reboots -> the "
           "removal still reaches Linux --\n");
    new_flash("A");
    failures += run_boot("A boot 1: detach while Linux is down, then power cut", a_boot1);
    failures += run_boot("A boot 2: Linux reachable again", a_boot2);
    failures += run_boot("A boot 3: nothing left to deliver", no_tombstone_boot);
    unlink(g_nvs_path);

    printf("\n-- scenario B: stale timeout while Linux is down, reboot -> offline only, never removed --\n");
    new_flash("B");
    failures += run_boot("B boot 1: stale sweep, then power cut", b_boot1);
    failures += run_boot("B boot 2: Linux reachable", no_tombstone_boot);
    unlink(g_nvs_path);

    printf("\n-- scenario C: detach, reboot, the SAME uuid re-docks before Linux is reachable -> attached wins --\n");
    new_flash("C");
    failures += run_boot("C boot 1: detach while Linux is down, then power cut", detach_x_undelivered);
    failures += run_boot("C boot 2: re-dock, then Linux reachable", c_boot2);
    failures += run_boot("C boot 3: the cancel was durable", no_tombstone_boot);
    unlink(g_nvs_path);

    printf("\n-- scenario D: detach, re-dock, then reboot -> no stale tombstone after the reboot --\n");
    new_flash("D");
    failures += run_boot("D boot 1: detach, re-dock, power cut", d_boot1);
    failures += run_boot("D boot 2: Linux reachable", no_tombstone_boot);
    unlink(g_nvs_path);

    printf("\n-- scenario E: DETACHED self-report, reboot, tombstone delivered, then re-dock -> online=true is the "
           "last word --\n");
    new_flash("E");
    failures += run_boot("E boot 1: self-reported detach while Linux is down, then power cut", e_boot1);
    failures += run_boot("E boot 2: tombstone delivered, then re-dock", e_boot2);
    failures += run_boot("E boot 3: nothing left to deliver", no_tombstone_boot);
    unlink(g_nvs_path);

    printf("\n-- scenario F: the first journal write fails -> retried, the removal still survives the reboot --\n");
    new_flash("F");
    failures += run_boot("F boot 1: journal commit fails once, then power cut", f_boot1);
    failures += run_boot("F boot 2: Linux reachable", f_boot2);
    unlink(g_nvs_path);

    printf("\n-- scenario G: a detach while the exporter is busy retrying a resync against an unreachable Linux "
           "is still journaled promptly --\n");
    new_flash("G");
    failures += run_boot("G boot 1: detach mid-resync, power cut ~1 s later", g_boot1);
    failures += run_boot("G boot 2: Linux reachable", g_boot2);
    unlink(g_nvs_path);

    printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
