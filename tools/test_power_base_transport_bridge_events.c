/*
 * Host-compiled regression test for the NEW bridge-observer wiring added to
 * components/power_base_transport/power_base_transport.c by this follow-up
 * (Power Base -> Linux OTBR northbound bridge transport):
 * power_base_transport_set_bridge_observer()'s on_sample_stored/
 * on_topology_applied/on_node_offline callbacks.
 *
 * Links against the REAL, unmodified power_base_transport.c,
 * topology_registry.c, topology_proto.c, sensor_channel.c, and
 * sensor_root_aggregator.c -- no mirrored logic. The only things NOT real
 * here are the OpenThread CoAP SERVER engine + esp_openthread instance/lock
 * themselves (tools/host_test_stubs/host_pb_transport_coap_sim.c, a
 * stand-in for the Thread-radio hardware dependency -- see that file's own
 * header comment for why it is a separate sim from host_ot_coap_sim.c).
 *
 * Covers:
 *   - a real (not duplicate) "/t" ATTACHED report fires on_topology_applied
 *     exactly once, with the correct entry snapshot; an exact-duplicate
 *     retransmit of the same report does NOT fire it again
 *   - a real (not duplicate) "/s" sample fires on_sample_stored exactly
 *     once; an exact-duplicate (same module_uuid+sample_seq) retransmit
 *     does NOT fire it again -- and this dedup NEVER changes the CoAP
 *     response code the sender sees (still 2.04 either way, per
 *     power_base_transport.h's own documented table)
 *   - a DETACHED report on a depth=1 parent that cascades a depth=2 child
 *     fires on_topology_applied ONCE for the parent (conveying its own
 *     online=false), on_node_removed ONCE for the parent itself (its own
 *     confirmed self-detach) and ONCE for the cascaded child -- NEVER
 *     on_node_offline for either (this follow-up: POGO auto-registration/
 *     removal lifecycle -- a confirmed detach is a removal, never a mere
 *     offline transition)
 *   - power_base_transport_on_direct_downstream_detached() (a hardware
 *     detach, not a report) fires on_node_removed for the directly-detached
 *     node AND its cascaded child, with no skip exception -- never
 *     on_node_offline
 *   - power_base_transport_sweep_stale() fires on_node_offline ONLY (never
 *     on_node_removed) for the node that actually goes stale, never for a
 *     node whose last_seen_ms is still within the timeout -- this is the
 *     one path that stays "offline", not "removed"
 *   - a rejected topology report (unknown parent) and a rejected sample
 *     (unknown uuid) fire NEITHER callback
 *   - registering no observer at all (the default) is a complete no-op --
 *     every existing CoAP response-code behavior is unaffected
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra \
 *      -I tools/host_test_stubs -I components/power_base_transport/include \
 *      -I components/topology_registry/include -I components/topology_proto/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -o /tmp/test_power_base_transport_bridge_events tools/test_power_base_transport_bridge_events.c \
 *      components/power_base_transport/power_base_transport.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *      components/sensor_channel/sensor_channel.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      tools/host_test_stubs/host_pb_transport_coap_sim.c \
 *   && /tmp/test_power_base_transport_bridge_events
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "power_base_transport/power_base_transport.h"
#include "host_pb_transport_coap_sim.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static const uint8_t NODE_A_UUID[TOPOLOGY_UUID_LEN] = {0xAA, 0, 0, 0, 0, 0, 0, 0x01}; /* depth=1 */
static const uint8_t NODE_B_UUID[TOPOLOGY_UUID_LEN] = {0xBB, 0, 0, 0, 0, 0, 0, 0x01}; /* depth=2, child of A */

/* ---- observer call recording ---- */

#define MAX_RECORDED 16

static int s_sample_stored_count;
static sensor_sample_t s_last_sample;

static int s_topology_applied_count;
static topology_registry_entry_t s_last_applied_entry;

static int s_node_offline_count;
static uint8_t s_offline_uuids[MAX_RECORDED][TOPOLOGY_UUID_LEN];

static int s_node_removed_count;
static uint8_t s_removed_uuids[MAX_RECORDED][TOPOLOGY_UUID_LEN];

static void reset_observer_recording(void)
{
    s_sample_stored_count = 0;
    s_topology_applied_count = 0;
    s_node_offline_count = 0;
    memset(s_offline_uuids, 0, sizeof(s_offline_uuids));
    s_node_removed_count = 0;
    memset(s_removed_uuids, 0, sizeof(s_removed_uuids));
}

static void on_sample_stored(const sensor_sample_t *sample, void *ctx)
{
    (void)ctx;
    s_sample_stored_count++;
    s_last_sample = *sample;
}

static void on_topology_applied(const topology_registry_entry_t *entry, void *ctx)
{
    (void)ctx;
    s_topology_applied_count++;
    s_last_applied_entry = *entry;
}

static void on_node_offline(const uint8_t uuid[TOPOLOGY_UUID_LEN], void *ctx)
{
    (void)ctx;
    if (s_node_offline_count < MAX_RECORDED) {
        memcpy(s_offline_uuids[s_node_offline_count], uuid, TOPOLOGY_UUID_LEN);
    }
    s_node_offline_count++;
}

static bool offline_recorded_for(const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    int n = s_node_offline_count < MAX_RECORDED ? s_node_offline_count : MAX_RECORDED;
    for (int i = 0; i < n; i++) {
        if (memcmp(s_offline_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static void on_node_removed(const uint8_t uuid[TOPOLOGY_UUID_LEN], void *ctx)
{
    (void)ctx;
    if (s_node_removed_count < MAX_RECORDED) {
        memcpy(s_removed_uuids[s_node_removed_count], uuid, TOPOLOGY_UUID_LEN);
    }
    s_node_removed_count++;
}

static bool removed_recorded_for(const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    int n = s_node_removed_count < MAX_RECORDED ? s_node_removed_count : MAX_RECORDED;
    for (int i = 0; i < n; i++) {
        if (memcmp(s_removed_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- fixture builders ---- */

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
    rep.role = (depth == 1) ? TOPOLOGY_ROLE_LEAF : TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0003;
    rep.boot_session_generation = generation;
    rep.sequence = seq;
    rep.upstream_connected = true;
    rep.downstream_connected = downstream_connected;
    rep.report_type = type;

    uint8_t wire[TOPOLOGY_REPORT_WIRE_LEN];
    size_t len = topology_report_pack(&rep, wire, sizeof(wire));
    bool found = host_pb_coap_sim_dispatch("t", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
    (void)found;
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
                                                 .status = SENSOR_CHANNEL_STATUS_OK, .value = 21000};
    uint8_t wire[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t len = sensor_channel_pack(&s, wire, sizeof(wire));
    host_pb_coap_sim_dispatch("s", OT_COAP_TYPE_CONFIRMABLE, wire, (uint16_t)len, out_sent, out_code);
}

static void fresh_transport(void)
{
    host_pb_coap_sim_reset();
    power_base_transport_init(POWER_BASE_UUID);
}

int main(void)
{
    /* Registered ONCE -- power_base_transport_init() never clears the
     * observer (see power_base_transport.h's own doc comment: it is wired
     * once, at boot, like every other callback in this codebase), so this
     * stays effective across every fresh_transport() re-init used below to
     * get a clean registry/aggregator for each independent scenario. */
    power_base_transport_bridge_observer_t observer = {
        .ctx = NULL,
        .on_sample_stored = on_sample_stored,
        .on_topology_applied = on_topology_applied,
        .on_node_offline = on_node_offline,
        .on_node_removed = on_node_removed,
    };
    power_base_transport_set_bridge_observer(&observer);

    printf("\n-- scenario: a real ATTACHED report fires on_topology_applied exactly once --\n");
    {
        fresh_transport();
        reset_observer_recording();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 100, 1, false, &sent, &code);
        CHECK(sent, "a Confirmable request always gets a response");
        CHECK(code == OT_COAP_CODE_CHANGED, "a real ATTACHED report gets CoAP 2.04 Changed");
        CHECK(s_topology_applied_count == 1, "on_topology_applied fired exactly once");
        CHECK(memcmp(s_last_applied_entry.module_uuid, NODE_A_UUID, TOPOLOGY_UUID_LEN) == 0,
              "the applied entry's module_uuid matches the reporting node");
        CHECK(s_last_applied_entry.online == true, "the applied entry is online");
        CHECK(s_last_applied_entry.depth == 1, "the applied entry's depth matches the report");
        CHECK(s_node_offline_count == 0, "on_node_offline never fires for a brand-new node's own first report");

        printf("\n-- an exact-duplicate retransmit of the same report does NOT fire on_topology_applied again "
               "--\n");
        reset_observer_recording();
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 100, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_VALID, "the exact-duplicate retransmit gets CoAP 2.03 Valid (unchanged behavior)");
        CHECK(s_topology_applied_count == 0, "on_topology_applied does NOT fire for an exact-duplicate retransmit");
    }

    printf("\n-- scenario: a real sample fires on_sample_stored exactly once; an exact-duplicate retransmit does "
           "not, and the CoAP response code is unaffected either way --\n");
    {
        fresh_transport();
        reset_observer_recording();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 200, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: NODE_A is now online");

        reset_observer_recording();
        send_sensor_sample(NODE_A_UUID, 7, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "a real sample gets CoAP 2.04 Changed");
        CHECK(s_sample_stored_count == 1, "on_sample_stored fired exactly once");
        CHECK(s_last_sample.sample_seq == 7, "the stored sample's sample_seq matches");
        CHECK(memcmp(s_last_sample.module_uuid, NODE_A_UUID, SENSOR_UUID_LEN) == 0,
              "the stored sample's module_uuid matches");

        reset_observer_recording();
        send_sensor_sample(NODE_A_UUID, 7, &sent, &code);
        CHECK(code == OT_COAP_CODE_VALID,
              "an exact-duplicate (same seq) retransmit gets CoAP 2.03 Valid (correction pass: distinct from 2.04, "
              "matching the topology registry's own OK_DUPLICATE convention -- nothing was actually re-stored)");
        CHECK(s_sample_stored_count == 0, "on_sample_stored does NOT fire for an exact-duplicate retransmit");
    }

    printf("\n-- scenario: the aggregator reporting FULL never returns 2.04 and never fires on_sample_stored --\n");
    {
        fresh_transport();
        bool sent;
        otCoapCode code;
        /* Fill the aggregator to SENSOR_ROOT_AGGREGATOR_MAX_NODES (16) with
         * 16 distinct, topology-known, online, unrestricted-bitmap uuids. */
        uint8_t uuid[TOPOLOGY_UUID_LEN];
        for (int i = 0; i < 16; i++) {
            memset(uuid, 0, TOPOLOGY_UUID_LEN);
            uuid[7] = (uint8_t)(0xC0 + i);
            send_topology_report(uuid, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 900 + i, 1, false, &sent, &code);
            CHECK(code == OT_COAP_CODE_CHANGED, "setup: each of the 16 filler nodes attaches OK");
            send_sensor_sample(uuid, 1, &sent, &code);
            CHECK(code == OT_COAP_CODE_CHANGED, "setup: each of the 16 filler nodes' first sample is stored");
        }

        /* A 17th, brand-new uuid's topology can still attach (topology_registry's
         * own capacity is 32, independent of the aggregator's 16) ... */
        uint8_t uuid17[TOPOLOGY_UUID_LEN];
        memset(uuid17, 0, TOPOLOGY_UUID_LEN);
        uuid17[7] = 0xD0;
        send_topology_report(uuid17, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 999, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the 17th uuid's topology attach still succeeds (registry has room)");

        /* ... but its FIRST sample cannot be stored: the aggregator itself
         * (a SEPARATE, smaller 16-slot table) is now full. */
        reset_observer_recording();
        send_sensor_sample(uuid17, 1, &sent, &code);
        CHECK(code == OT_COAP_CODE_SERVICE_UNAVAILABLE,
              "a sample for a brand-new uuid once the aggregator is FULL gets CoAP 5.03, never 2.04");
        CHECK(s_sample_stored_count == 0, "on_sample_stored does NOT fire when the aggregator reports FULL");
    }

    printf("\n-- scenario (correction pass): a DETACHED report cascading a child ERASES both NODE_A itself and "
           "NODE_B entirely (topology_registry_erase(), see topology_registry.h's own top comment) -- "
           "on_topology_applied does NOT fire at all (NODE_A's entry no longer exists to convey), and "
           "on_node_removed fires for BOTH NODE_A (its own confirmed self-detach) and NODE_B (the cascaded "
           "child) -- NEVER on_node_offline --\n");
    {
        fresh_transport();
        reset_observer_recording();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 300, 1, true, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: NODE_A online, downstream_connected=true");
        send_topology_report(NODE_B_UUID, NODE_A_UUID, 2, TOPOLOGY_REPORT_ATTACHED, 301, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: NODE_B (child of NODE_A) online");

        reset_observer_recording();
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_DETACHED, 300, 2, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the DETACHED report itself is applied (2.04)");
        CHECK(s_topology_applied_count == 0, "on_topology_applied does NOT fire -- NODE_A erased itself, so there "
                                               "is no entry left to convey (its removal is fully conveyed via "
                                               "on_node_removed instead)");
        CHECK(s_node_offline_count == 0, "on_node_offline NEVER fires for a confirmed DETACHED report's cascade -- "
                                          "see on_node_removed below");
        CHECK(s_node_removed_count == 2, "on_node_removed fires exactly twice: NODE_A (its own self-detach) and "
                                          "NODE_B (the cascaded child)");
        CHECK(removed_recorded_for(NODE_A_UUID), "on_node_removed names NODE_A itself (its own confirmed detach)");
        CHECK(removed_recorded_for(NODE_B_UUID), "on_node_removed names NODE_B (the cascaded child)");

        CHECK(power_base_transport_get_node_count() == 0,
              "correction pass: NEITHER NODE_A NOR NODE_B remain findable in the Power Base's own "
              "topology_registry at all -- this is what makes 'removed' survive a Linux ingest restart (periodic "
              "full resync can never resend either of them again)");
    }

    printf("\n-- scenario: a direct DOWNSTREAM POGO detach (hardware event, not a report) fires on_node_removed "
           "for the directly-detached node AND its cascaded child -- never on_node_offline --\n");
    {
        fresh_transport();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 400, 1, true, &sent, &code);
        send_topology_report(NODE_B_UUID, NODE_A_UUID, 2, TOPOLOGY_REPORT_ATTACHED, 401, 1, false, &sent, &code);

        reset_observer_recording();
        power_base_transport_on_direct_downstream_detached();
        CHECK(s_node_removed_count == 2, "both NODE_A (the direct child) and NODE_B (its cascaded subtree) fire "
                                          "on_node_removed");
        CHECK(removed_recorded_for(NODE_A_UUID), "on_node_removed names NODE_A");
        CHECK(removed_recorded_for(NODE_B_UUID), "on_node_removed names NODE_B");
        CHECK(s_node_offline_count == 0, "on_node_offline never fires for a hardware detach -- this is a removal");
        CHECK(s_topology_applied_count == 0, "on_topology_applied never fires for a hardware detach (no report was "
                                              "involved)");

        printf("\n-- calling the direct-detach handler AGAIN (already offline) fires nothing further --\n");
        reset_observer_recording();
        power_base_transport_on_direct_downstream_detached();
        CHECK(s_node_removed_count == 0, "no further on_node_removed calls once every node is already offline");
    }

    printf("\n-- scenario: sweep_stale() fires on_node_offline only for the node that actually goes stale --\n");
    {
        fresh_transport();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 500, 1, false, &sent, &code);
        send_topology_report(NODE_B_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 501, 1, false, &sent, &code);

        reset_observer_recording();
        /* Both reports above were applied with last_seen_ms set from the
         * real xTaskGetTickCount() at call time -- read that same clock
         * again right now (never a fabricated absolute value: sweep_stale()'s
         * own elapsed=now_ms-last_seen_ms is an UNSIGNED wraparound-safe
         * subtraction, so passing now_ms < last_seen_ms would wrap into a
         * huge bogus "elapsed" instead of the intended "not stale yet"). */
        uint32_t now = xTaskGetTickCount();
        power_base_transport_sweep_stale(now, 1000000000u);
        CHECK(s_node_offline_count == 0, "sweeping with a huge timeout marks nothing stale yet");

        /* Comfortably in the future relative to the real last_seen_ms
         * captured above, with a tiny timeout: both nodes go stale. */
        power_base_transport_sweep_stale(now + 5000, 1u);
        CHECK(s_node_offline_count == 2, "sweeping with a now_ms far past both nodes' last_seen_ms marks both "
                                          "stale");
        CHECK(offline_recorded_for(NODE_A_UUID) && offline_recorded_for(NODE_B_UUID),
              "on_node_offline names both nodes");
        CHECK(s_node_removed_count == 0, "a stale-timeout sweep NEVER fires on_node_removed (this follow-up: "
                                          "timeout/comms-loss is offline, never a removal)");
    }

    printf("\n-- scenario: a rejected report/sample fires NEITHER callback --\n");
    {
        fresh_transport();
        reset_observer_recording();
        bool sent;
        otCoapCode code;
        /* NODE_B claims a parent (NODE_A) the registry has never heard of. */
        send_topology_report(NODE_B_UUID, NODE_A_UUID, 2, TOPOLOGY_REPORT_ATTACHED, 600, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_PRECONDITION_FAILED, "an unknown-parent report is rejected (4.12), unchanged "
                                                          "behavior");
        CHECK(s_topology_applied_count == 0, "on_topology_applied does not fire for a rejected report");
        CHECK(s_node_offline_count == 0, "on_node_offline does not fire for a rejected report");
        CHECK(s_node_removed_count == 0, "on_node_removed does not fire for a rejected report either");

        reset_observer_recording();
        send_sensor_sample(NODE_A_UUID, 1, &sent, &code); /* NODE_A was never reported to this fresh registry */
        CHECK(code == OT_COAP_CODE_NOT_FOUND, "a sample from an unknown module_uuid is rejected (4.04), unchanged "
                                                "behavior");
        CHECK(s_sample_stored_count == 0, "on_sample_stored does not fire for a rejected sample");
    }

    printf("\n-- scenario: registering NULL clears the observer -- every callback becomes a no-op again --\n");
    {
        fresh_transport();
        power_base_transport_set_bridge_observer(NULL);
        reset_observer_recording();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 700, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "the CoAP response is unaffected by having no observer registered");
        CHECK(s_topology_applied_count == 0, "no observer means on_topology_applied never fires");
        /* Restore for anything appended after this block in the future. */
        power_base_transport_set_bridge_observer(&observer);
    }

    printf("\n-- scenario (correction pass -- restart-survival bug, this follow-up's item 1): after a CONFIRMED "
           "detach, the Power Base's OWN topology_registry must no longer include this uuid AT ALL, so periodic "
           "full topology resync (linux_bridge_exporter's do_full_topology_resync(), simulated here by directly "
           "walking power_base_transport_get_node_count()/get_node() exactly as it does) can never again resend "
           "a stale online=false snapshot that a FRESH/restarted Linux ingest::Registry (whose RAM-only removed_ "
           "denylist reset to empty) would otherwise re-insert as an ordinary, non-removed node --\n");
    {
        fresh_transport();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 800, 1, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "setup: NODE_A attaches");

        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_DETACHED, 800, 2, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "NODE_A's confirmed self-detach is applied");

        bool found_after_removal = false;
        size_t count = power_base_transport_get_node_count();
        for (size_t i = 0; i < count; i++) {
            topology_registry_entry_t e;
            if (power_base_transport_get_node(i, &e) && memcmp(e.module_uuid, NODE_A_UUID, TOPOLOGY_UUID_LEN) == 0) {
                found_after_removal = true;
            }
        }
        CHECK(!found_after_removal, "a confirmed-detached uuid is COMPLETELY ABSENT from the Power Base's own "
                                      "topology_registry (never merely online=false) -- this is what makes "
                                      "'removed' survive a Linux ingest restart: Linux never receives anything "
                                      "to resurrect it with in the first place, regardless of its own denylist "
                                      "state");
    }

    printf("\n-- scenario (correction pass, item 1 continued): the SAME guarantee holds for a cascaded child "
           "(an intermediate module's own confirmed downstream detach), and for the power base's own direct "
           "hardware detach path -- neither the detached node NOR its subtree remain findable --\n");
    {
        fresh_transport();
        bool sent;
        otCoapCode code;
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 810, 1, true, &sent, &code);
        send_topology_report(NODE_B_UUID, NODE_A_UUID, 2, TOPOLOGY_REPORT_ATTACHED, 811, 1, false, &sent, &code);

        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_UPDATED, 810, 2, false, &sent, &code);
        CHECK(code == OT_COAP_CODE_CHANGED, "NODE_A reports its own downstream (NODE_B) just detached");

        bool node_a_found = false, node_b_found = false;
        size_t count = power_base_transport_get_node_count();
        for (size_t i = 0; i < count; i++) {
            topology_registry_entry_t e;
            if (!power_base_transport_get_node(i, &e)) {
                continue;
            }
            if (memcmp(e.module_uuid, NODE_A_UUID, TOPOLOGY_UUID_LEN) == 0) {
                node_a_found = true;
            }
            if (memcmp(e.module_uuid, NODE_B_UUID, TOPOLOGY_UUID_LEN) == 0) {
                node_b_found = true;
            }
        }
        CHECK(node_a_found, "NODE_A itself is UNCHANGED (still online, still reachable) -- only its child was "
                              "confirmed detached");
        CHECK(!node_b_found, "NODE_B (the cascaded, confirmed-detached child) is completely absent -- not merely "
                               "offline");

        printf("\n-- and: the power base's own direct hardware downstream detach path erases too --\n");
        fresh_transport();
        send_topology_report(NODE_A_UUID, POWER_BASE_UUID, 1, TOPOLOGY_REPORT_ATTACHED, 820, 1, true, &sent, &code);
        send_topology_report(NODE_B_UUID, NODE_A_UUID, 2, TOPOLOGY_REPORT_ATTACHED, 821, 1, false, &sent, &code);
        power_base_transport_on_direct_downstream_detached();

        bool any_found = false;
        count = power_base_transport_get_node_count();
        for (size_t i = 0; i < count; i++) {
            topology_registry_entry_t e;
            if (power_base_transport_get_node(i, &e) &&
                (memcmp(e.module_uuid, NODE_A_UUID, TOPOLOGY_UUID_LEN) == 0 ||
                 memcmp(e.module_uuid, NODE_B_UUID, TOPOLOGY_UUID_LEN) == 0)) {
                any_found = true;
            }
        }
        CHECK(!any_found, "after a direct hardware downstream detach, neither NODE_A nor NODE_B remain findable "
                            "at all");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
