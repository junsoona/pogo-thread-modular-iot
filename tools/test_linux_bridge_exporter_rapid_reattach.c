/*
 * Host-compiled test for this correction pass's item 2 fix: the rapid
 * detach -> reattach race in components/linux_bridge_exporter/linux_bridge_exporter.c.
 *
 * The bug (found by code review, reproduced below BEFORE the fix -- see
 * this file's own git history): a REMOVED tombstone whose delivery failed
 * (or whose delivery confirmation never reached the Power Base) stayed
 * marked pending in s_pending_removed_* until a 2.xx response was seen for
 * it, and do_full_removed_resync() kept re-enqueuing it on every periodic
 * resync tick regardless of anything else that happened to that uuid in
 * the meantime. If the SAME module_uuid physically re-docked and
 * re-registered (a fresh, genuine TOPOLOGY_REPORT_ATTACHED) while that
 * tombstone was still pending, the stale REMOVED could still be sent
 * AFTER the reattach's own fresh SNAPSHOT, incorrectly deleting the
 * just-reattached module on the Linux side.
 *
 * The fix (linux_bridge_exporter.c, this correction pass):
 *   1. observer_on_topology_applied() now calls clear_removed_pending()
 *      for module_uuid FIRST, proactively, the instant a fresh real
 *      topology apply is known -- closing the window as early as possible.
 *   2. handle_removed_event() re-checks, AT THE MOMENT OF DRAIN/SEND
 *      (regardless of whether it reached there via the reactive fast path
 *      or a later resync retry), whether module_uuid is CURRENTLY findable
 *      in the Power Base's own topology_registry (the authoritative source
 *      of truth) -- if so, a genuine reattach has already happened, and
 *      the stale tombstone is skipped (and its pending mark cleared)
 *      instead of sent.
 * Together these two close the overwhelming majority of the race window,
 * using module_uuid presence/absence in topology_registry as the implicit
 * "lifecycle epoch" (rather than a new wire field -- SAMPLE/TOPOLOGY_SNAPSHOT/
 * TOPOLOGY_REMOVED's wire layouts are completely unchanged by this fix). A
 * narrow residual window remains ONLY while a stale tombstone's own
 * transmit() call is already blocking mid-retry when the reattach happens
 * (this file's own on_topology_applied callback must stay non-blocking, so
 * it cannot synchronously wait for that in-flight call to finish) -- even
 * then, ingest::Registry::apply_topology()'s own online=true re-approval
 * logic (see linux/matter_bridge/ingest/tests/test_registry_removal.cpp)
 * means Linux still converges back to "active" as soon as the reattach's
 * own SNAPSHOT arrives right after, so this residual window is a delayed
 * convergence, never a permanently lost module.
 *
 * Links the REAL, unmodified linux_bridge_exporter.c, power_base_transport.c,
 * topology_registry.c, topology_proto.c, sensor_channel.c,
 * sensor_root_aggregator.c, bridge_export_proto.c, pogo_crc16.c, and
 * coap_response_timing.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/linux_bridge_exporter/include -I components/power_base_transport/include \
 *      -I components/power_base_bridge_config/include -I components/bridge_export_proto/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/pogo_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_linux_bridge_exporter_rapid_reattach tools/test_linux_bridge_exporter_rapid_reattach.c \
 *      components/linux_bridge_exporter/linux_bridge_exporter.c \
 *      components/linux_bridge_exporter/removed_journal.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/pogo_proto/pogo_crc16.c components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_pb_bridge_full_sim.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_linux_bridge_exporter_rapid_reattach
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
#define RESYNC_INTERVAL_MS 5000u /* this file uses the pogo_fast profile's real production interval */

/* Test-controlled fake clock -- see linux_bridge_exporter_test_set_clock()'s
 * own doc comment. This file needs EXPLICIT resync ticks (to prove no
 * further REMOVED is ever sent across MULTIPLE resync retries after a
 * reattach), so unlike test_linux_bridge_exporter.c (which freezes this
 * clock entirely), this one advances it on demand. */
static volatile uint32_t s_fake_now_ms = 1000;
static TickType_t fake_clock(void)
{
    return (TickType_t)s_fake_now_ms;
}
static void advance_fake_clock(uint32_t delta_ms)
{
    s_fake_now_ms += delta_ms;
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

static void make_uuid(uint8_t out[TOPOLOGY_UUID_LEN], uint8_t tag)
{
    memset(out, 0, TOPOLOGY_UUID_LEN);
    out[7] = tag;
}

static void send_topology_report_ex(const uint8_t module_uuid[TOPOLOGY_UUID_LEN],
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
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = generation;
    rep.sequence = seq;
    rep.upstream_connected = (type != TOPOLOGY_REPORT_DETACHED);
    rep.downstream_connected = downstream_connected;
    rep.report_type = type;
    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    host_pb_bridge_full_sim_dispatch_incoming("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

static void send_topology_report_full(const uint8_t module_uuid[TOPOLOGY_UUID_LEN],
                                       const uint8_t parent_uuid[TOPOLOGY_UUID_LEN], uint8_t depth,
                                       topology_report_type_t type, uint32_t generation, uint32_t seq, bool *out_sent,
                                       otCoapCode *out_code)
{
    send_topology_report_ex(module_uuid, parent_uuid, depth, type, generation, seq, false, out_sent, out_code);
}

/* Scans captured sends in [since, ...) for a TOPOLOGY_REMOVED tombstone
 * naming `uuid`. */
static bool find_removed_since(size_t since, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap) || strcmp(cap.uri_path, "pogo/v1/topology") != 0) {
            continue;
        }
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        if (!bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) ||
            type != BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED) {
            continue;
        }
        bridge_export_topology_removed_t rem;
        if (!bridge_export_unpack_topology_removed(payload, payload_len, &rem)) {
            continue;
        }
        if (memcmp(rem.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

/* Scans captured sends in [since, ...) for the LAST ordinary
 * TOPOLOGY_SNAPSHOT naming `uuid`, returning its online flag via *out_online.
 * Returns false if none was found. */
static bool find_last_snapshot_online_since(size_t since, const uint8_t uuid[TOPOLOGY_UUID_LEN], bool *out_online)
{
    bool found = false;
    size_t total = host_pb_bridge_full_sim_captured_count();
    for (size_t i = since; i < total; i++) {
        host_pb_bridge_full_sim_sent_t cap;
        if (!host_pb_bridge_full_sim_get_captured(i, &cap) || strcmp(cap.uri_path, "pogo/v1/topology") != 0) {
            continue;
        }
        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        if (!bridge_export_unpack_envelope(cap.payload, cap.payload_len, &type, &payload, &payload_len) ||
            type != BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT) {
            continue;
        }
        bridge_export_topology_node_t node;
        if (!bridge_export_unpack_topology_node(payload, payload_len, &node)) {
            continue;
        }
        if (memcmp(node.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
            *out_online = node.online;
            found = true;
        }
    }
    return found;
}

int main(void)
{
    host_pb_bridge_full_sim_reset();
    power_base_transport_init(POWER_BASE_UUID);

    /* Installed BEFORE init() -- see test_linux_bridge_exporter_timers.c's
     * own comment on why (a race with exporter_task()'s initial
     * last_resync/last_heartbeat capture otherwise wraps into a bogus
     * immediate "elapsed" and fires resync right away). */
    linux_bridge_exporter_test_set_clock(fake_clock);

    power_base_bridge_config_t cfg = {0};
    cfg.enabled = true;
    memset(cfg.host_addr, 0xFD, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    cfg.port = 5684;
    CHECK(linux_bridge_exporter_init(POWER_BASE_UUID, &cfg) == ESP_OK, "linux_bridge_exporter_init succeeds");

    printf("\n-- scenario (item 2, A+B): a REMOVED tombstone's delivery is never confirmed (transient failures "
           "exhaust its bounded retries, or -- indistinguishably from this layer's point of view -- Linux applied "
           "it but the ACK never made it back), the SAME module_uuid then physically re-docks and re-registers "
           "successfully, and NO stale REMOVED is EVER sent again afterward, across several subsequent resync "
           "ticks -- final state is active --\n");
    {
        uint8_t node[TOPOLOGY_UUID_LEN];
        make_uuid(node, 0xA1);
        bool sent;
        otCoapCode code;

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 100, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: node attaches");
        CHECK(wait_for_send_count(1, 2000), "setup: its ATTACHED export settles");

        int before_detach = host_pb_bridge_full_sim_send_count();
        /* Exhausts the REMOVED tombstone's own bounded 1+2 retries -- never
         * delivered (or delivered-but-unconfirmed; see this scenario's own
         * doc comment -- indistinguishable here). */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        send_topology_report_full(node, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_DETACHED, 100, 2, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the DETACHED report itself is applied (2.04)");
        CHECK(wait_for_send_count(before_detach + 3, 4000),
              "all 3 bounded attempts for the REMOVED tombstone are made and exhausted");

        /* The SAME module_uuid physically re-docks: a fresh ATTACHED with a
         * NEW boot_session_generation (a real re-dock power-cycles the
         * module). */
        size_t since_reattach = host_pb_bridge_full_sim_captured_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(node, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 200, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the reattach's fresh ATTACHED report is applied (2.04)");
        CHECK(wait_for_send_count(before_detach + 3 + 1, 2000), "the reattach's own SNAPSHOT export settles");

        bool online = false;
        CHECK(find_last_snapshot_online_since(since_reattach, node, &online) && online,
              "a SNAPSHOT with online=true was exported for the reattach");
        CHECK(!find_removed_since(since_reattach, node),
              "no REMOVED tombstone was sent immediately after the reattach");

        /* Drive several MORE explicit resync ticks -- if the OLD bug were
         * present, do_full_removed_resync() would keep re-enqueuing and
         * eventually re-sending the stale REMOVED on one of these. Extra
         * response budget covers node's own ordinary periodic SNAPSHOT
         * resync too (which keeps firing normally, unaffected). */
        for (int i = 0; i < 20; i++) {
            host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        }
        for (int tick = 0; tick < 5; tick++) {
            int before = host_pb_bridge_full_sim_send_count();
            advance_fake_clock(RESYNC_INTERVAL_MS + 10);
            int waited = 0;
            while (host_pb_bridge_full_sim_send_count() == before && waited < 900) {
                sleep_ms(10);
                waited += 10;
            }
        }
        CHECK(!find_removed_since(since_reattach, node),
              "STILL no REMOVED tombstone for this uuid across 5 further resync ticks after the reattach -- the "
              "stale pending mark was correctly cancelled, never resurrected by do_full_removed_resync()");
    }

    printf("\n-- scenario (item 2, D): a cascaded child's REMOVED tombstone and the parent's own re-attach "
           "exports interleave closely -- final captured state for the child, once it too physically re-docks, "
           "is active, with no lingering stale REMOVED afterward --\n");
    {
        uint8_t parent[TOPOLOGY_UUID_LEN], child[TOPOLOGY_UUID_LEN];
        make_uuid(parent, 0xC1);
        make_uuid(child, 0xC2);
        bool sent;
        otCoapCode code;

        int base = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        /* downstream_connected=true from the start -- the child below will
         * attach as its depth=2 downstream. */
        send_topology_report_ex(parent, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 300, 1, true, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: parent attaches");
        CHECK(wait_for_send_count(base + 1, 2000), "setup: parent attach export settles");
        base = host_pb_bridge_full_sim_send_count();

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(child, parent, 2, TOPOLOGY_REPORT_ATTACHED, 400, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: child attaches under parent");
        CHECK(wait_for_send_count(base + 1, 2000), "setup: child attach settles");

        /* Parent confirms its own downstream (the child) detached -- this
         * cascades an ERASE (this correction pass's item 1 fix) of the
         * child, producing a REMOVED tombstone for the child. Delivery
         * fails (exhausts retries). A flat, generous real wait covers the
         * parent's own SNAPSHOT send plus the child's REMOVED tombstone's
         * full bounded 1+2 retry/backoff schedule (up to ~700ms) -- no
         * exact send-count delta is asserted here (this scenario's own
         * point is the FINAL state after reattach, not the exact number of
         * intermediate attempts). */
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* parent's own UPDATED export */
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        host_pb_bridge_full_sim_queue_no_response();
        send_topology_report_full(parent, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_UPDATED, 300, 2, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "parent's downstream-lost UPDATED report is applied");
        sleep_ms(1500);

        /* The child physically re-docks under the SAME parent -- in reality
         * the PARENT's own downstream POGO port notices the redock first
         * (its own hardware DETECT_IN/PEER_READY) and reports
         * downstream_connected=true again BEFORE the child completes its
         * own handshake and reports ATTACHED (see topology_reporter.c's
         * resync_downstream_connected()) -- mirrored here as two reports,
         * close on the heels of the failed removal. */
        size_t since_reattach = host_pb_bridge_full_sim_captured_count();
        int before_reattach_sends = host_pb_bridge_full_sim_send_count();
        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED); /* parent's downstream-restored UPDATED */
        send_topology_report_ex(parent, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_UPDATED, 300, 3, true, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "parent reports its downstream restored");

        host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        send_topology_report_full(child, parent, 2, TOPOLOGY_REPORT_ATTACHED, 500, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the child's reattach is applied");
        CHECK(wait_for_send_count(before_reattach_sends + 2, 2000), "both the parent's restored-downstream and "
                                                                      "the child's reattach exports settle");

        bool online = false;
        CHECK(find_last_snapshot_online_since(since_reattach, child, &online) && online,
              "the child's reattach SNAPSHOT shows online=true");
        CHECK(!find_removed_since(since_reattach, child),
              "no REMOVED tombstone for the child was sent after its reattach");

        for (int i = 0; i < 20; i++) {
            host_pb_bridge_full_sim_queue_response(OT_ERROR_NONE, OT_COAP_CODE_CHANGED);
        }
        for (int tick = 0; tick < 5; tick++) {
            int before = host_pb_bridge_full_sim_send_count();
            advance_fake_clock(RESYNC_INTERVAL_MS + 10);
            int waited = 0;
            while (host_pb_bridge_full_sim_send_count() == before && waited < 900) {
                sleep_ms(10);
                waited += 10;
            }
        }
        CHECK(!find_removed_since(since_reattach, child),
              "STILL no REMOVED tombstone for the child across 5 further resync ticks -- final state stays "
              "active");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
