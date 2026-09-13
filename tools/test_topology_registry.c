/*
 * Host-compiled test for the power base's topology registry
 * (components/topology_registry/topology_registry.c) -- required test items
 * 13-20 of the power-base follow-up spec: sequence dup/reorder rejection,
 * cycle/self-parent/depth-jump rejection, direct-child-detach and
 * intermediate-downstream-detach subtree offline, stale timeout + reconnect
 * recovery, base-reboot dataset/heartbeat re-population, and sensor-data
 * UUID <-> topology-entry linkage (via topology_registry_find(), the same
 * lookup components/power_base_transport uses for that check).
 *
 * Links against the REAL, unmodified topology_registry.c and
 * topology_proto.c (neither has any ESP-IDF/FreeRTOS dependency) -- no
 * mirrored/reimplemented logic at all, unlike the pogo_port/child_service/
 * parent_service mirror-model tests (which must reimplement FreeRTOS-task
 * scaffolding around real wire-format code) -- there is nothing here that
 * needs mirroring.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/topology_registry/include -I components/topology_proto/include \
 *      -o /tmp/test_topology_registry tools/test_topology_registry.c \
 *      components/topology_registry/topology_registry.c components/topology_proto/topology_proto.c \
 *   && /tmp/test_topology_registry
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "topology_registry/topology_registry.h"

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

static const uint8_t POWER_BASE_UUID[TOPOLOGY_UUID_LEN] = {0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0};

static void fill_uuid(uint8_t out[TOPOLOGY_UUID_LEN], uint8_t tag)
{
    for (int i = 0; i < TOPOLOGY_UUID_LEN; i++) {
        out[i] = (uint8_t)(tag * 0x11 + i);
    }
}

static topology_report_t make_report(uint8_t uuid_tag, uint8_t depth, const uint8_t parent[TOPOLOGY_UUID_LEN],
                                      uint32_t generation, uint32_t seq, topology_report_type_t type)
{
    topology_report_t r = {0};
    r.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    fill_uuid(r.module_uuid, uuid_tag);
    memcpy(r.parent_uuid, parent, TOPOLOGY_UUID_LEN);
    r.topology_depth = depth;
    r.role = (depth == 1 && type != TOPOLOGY_REPORT_DETACHED) ? TOPOLOGY_ROLE_LEAF : TOPOLOGY_ROLE_INTERMEDIATE;
    r.sensor_type_bitmap = 0x0001;
    r.boot_session_generation = generation;
    r.sequence = seq;
    r.upstream_connected = (type != TOPOLOGY_REPORT_DETACHED);
    r.downstream_connected = false;
    r.report_type = type;
    return r;
}

static void scenario_depth1_parent_and_self_parent(void)
{
    printf("\n-- scenario: depth==1 parent must be the power base; self-parent rejected --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    uint8_t bogus_parent[TOPOLOGY_UUID_LEN];
    fill_uuid(bogus_parent, 0x99);
    topology_report_t bad = make_report(1, 1, bogus_parent, 100, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_result_t r = topology_registry_apply_report(&reg, &bad, POWER_BASE_UUID, 1000);
    CHECK(r == TOPOLOGY_REGISTRY_ERR_DEPTH1_BAD_PARENT, "depth==1 with a non-power-base parent is rejected");

    topology_report_t good = make_report(1, 1, POWER_BASE_UUID, 100, 1, TOPOLOGY_REPORT_ATTACHED);
    r = topology_registry_apply_report(&reg, &good, POWER_BASE_UUID, 1000);
    CHECK(r == TOPOLOGY_REGISTRY_OK, "depth==1 with the power base as parent is accepted");
    CHECK(topology_registry_count(&reg) == 1, "registry now has exactly one node");

    uint8_t self_uuid[TOPOLOGY_UUID_LEN];
    fill_uuid(self_uuid, 2);
    topology_report_t self_parent = make_report(2, 2, self_uuid, 100, 1, TOPOLOGY_REPORT_ATTACHED);
    memcpy(self_parent.parent_uuid, self_parent.module_uuid, TOPOLOGY_UUID_LEN);
    r = topology_registry_apply_report(&reg, &self_parent, POWER_BASE_UUID, 1000);
    CHECK(r == TOPOLOGY_REGISTRY_ERR_SELF_PARENT, "a module claiming itself as its own parent is rejected");
}

static void scenario_cycle_and_depth_jump(void)
{
    printf("\n-- scenario: cycle and depth-jump rejection --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    uint8_t uuid1[TOPOLOGY_UUID_LEN], uuid2[TOPOLOGY_UUID_LEN], uuid3[TOPOLOGY_UUID_LEN];
    fill_uuid(uuid1, 1);
    fill_uuid(uuid2, 2);
    fill_uuid(uuid3, 3);

    topology_report_t r1 = make_report(1, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &r1, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_OK, "node1 (depth1) accepted");

    topology_report_t r2 = make_report(2, 2, uuid1, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &r2, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_OK, "node2 (depth2, parent=node1) accepted");

    /* A depth jump: node3 claims depth=4 with parent=node2 (whose real depth
     * is 2, so a valid child would be depth=3, not 4). */
    topology_report_t jump = make_report(3, 4, uuid2, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_result_t r = topology_registry_apply_report(&reg, &jump, POWER_BASE_UUID, 0);
    CHECK(r == TOPOLOGY_REGISTRY_ERR_PARENT_DEPTH_MISMATCH, "an impossible depth jump (parent depth+1 mismatch) is rejected");

    /* An unknown parent (depth>1 naming a DIFFERENT uuid the registry has
     * never seen at all -- uuid3 itself is never registered in this
     * scenario, and the reporting module (tag=4) is distinct from it, so
     * this is a genuine "unknown parent", not a self-parent case). */
    topology_report_t unknown_parent = make_report(4, 2, uuid3, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    r = topology_registry_apply_report(&reg, &unknown_parent, POWER_BASE_UUID, 0);
    CHECK(r == TOPOLOGY_REGISTRY_ERR_UNKNOWN_PARENT, "depth>1 naming an unknown parent uuid is rejected");

    /* A genuine cycle: try to make node1 (already depth1, parent=power_base)
     * claim node2 as ITS parent -- node2's ancestor chain already passes
     * through node1, so this would create a loop. */
    topology_report_t cycle = make_report(1, 3, uuid2, 2, 1, TOPOLOGY_REPORT_ATTACHED);
    r = topology_registry_apply_report(&reg, &cycle, POWER_BASE_UUID, 0);
    CHECK(r == TOPOLOGY_REGISTRY_ERR_CYCLE, "a topology cycle (node1 <- node2 <- node1) is rejected");
}

static void scenario_invalid_role_depth_combinations(void)
{
    printf("\n-- scenario: impossible role/depth combinations rejected --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    topology_report_t unassigned = make_report(1, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    unassigned.role = TOPOLOGY_ROLE_UNASSIGNED;
    CHECK(topology_registry_apply_report(&reg, &unassigned, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH,
          "a report claiming role=UNASSIGNED is rejected (UNASSIGNED never has a settled depth to report)");

    topology_report_t as_root = make_report(2, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    as_root.role = TOPOLOGY_ROLE_ROOT;
    CHECK(topology_registry_apply_report(&reg, &as_root, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH,
          "a sensor module reporting role=ROOT is rejected -- AUTO mode never produces this");

    topology_report_t zero_depth = make_report(3, 0, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &zero_depth, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH,
          "topology_depth==0 is rejected");
}

static void scenario_duplicate_and_reordered_sequence(void)
{
    printf("\n-- scenario: duplicate/reordered sequence, idempotent retransmit --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    topology_report_t r1 = make_report(1, 1, POWER_BASE_UUID, 5, 10, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &r1, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_OK, "first report (gen=5, seq=10) accepted");

    topology_report_t dup = make_report(1, 1, POWER_BASE_UUID, 5, 10, TOPOLOGY_REPORT_HEARTBEAT);
    CHECK(topology_registry_apply_report(&reg, &dup, POWER_BASE_UUID, 100) == TOPOLOGY_REGISTRY_OK_DUPLICATE,
          "an exact duplicate (same gen, same seq) is an idempotent OK_DUPLICATE, not an error "
          "(code review follow-up section 3: 허용한 idempotent duplicate)");

    topology_report_t older = make_report(1, 1, POWER_BASE_UUID, 5, 9, TOPOLOGY_REPORT_HEARTBEAT);
    CHECK(topology_registry_apply_report(&reg, &older, POWER_BASE_UUID, 100) == TOPOLOGY_REGISTRY_ERR_DUPLICATE_SEQ,
          "a genuinely reordered/older seq within the same generation is rejected");

    topology_report_t newer = make_report(1, 1, POWER_BASE_UUID, 5, 11, TOPOLOGY_REPORT_HEARTBEAT);
    CHECK(topology_registry_apply_report(&reg, &newer, POWER_BASE_UUID, 200) == TOPOLOGY_REGISTRY_OK,
          "a strictly greater seq in the same generation is accepted");
}

/*
 * Code review follow-up, section 2: boot_session_generation is an OPAQUE
 * per-boot token (topology_reporter may use a random nonce OR a persisted
 * counter -- see topology_reporter.c), NEVER assumed to be numerically
 * increasing. This exercises every required scenario from that follow-up:
 * a smaller generation is still accepted as a legitimate new session, a
 * delayed packet from the session JUST superseded is rejected, and (a
 * documented, bounded limitation) a packet from a session two-or-more
 * generations back is NOT guaranteed to be rejected -- only the
 * immediately-preceding one is remembered.
 */
static void scenario_session_transition_is_generation_agnostic(void)
{
    printf("\n-- scenario: session transitions do not assume increasing generation numbers --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    topology_report_t a1 = make_report(1, 1, POWER_BASE_UUID, 5, 10, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &a1, POWER_BASE_UUID, 0) == TOPOLOGY_REGISTRY_OK,
          "session A (gen=5) begins");

    /* Session B uses a NUMERICALLY LARGER generation (7 > 5) -- still just
     * "a different generation", not specially treated as "greater". */
    topology_report_t b1 = make_report(1, 1, POWER_BASE_UUID, 7, 1, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &b1, POWER_BASE_UUID, 1000) == TOPOLOGY_REGISTRY_OK,
          "session B (gen=7) is accepted as a new session, even with a low starting sequence");

    /* A delayed packet from session A (now superseded) must be rejected --
     * this is the exact bug this follow-up fixes: no longer "reject because
     * 5 < 7", but "reject because 5 is the generation we just left". */
    topology_report_t a_delayed = make_report(1, 1, POWER_BASE_UUID, 5, 999, TOPOLOGY_REPORT_HEARTBEAT);
    CHECK(topology_registry_apply_report(&reg, &a_delayed, POWER_BASE_UUID, 1100) == TOPOLOGY_REGISTRY_ERR_STALE_SESSION,
          "a delayed packet from superseded session A (gen=5) is rejected after session B began");

    /* Session C uses a SMALLER generation (3 < 7) than the current one --
     * the required "이전 generation보다 숫자가 작은 새 부팅 세션도 정상
     * 수용" case. 3 is neither the current generation (7) nor the
     * superseded one (5), so it is a legitimate new session. */
    topology_report_t c1 = make_report(1, 1, POWER_BASE_UUID, 3, 1, TOPOLOGY_REPORT_ATTACHED);
    CHECK(topology_registry_apply_report(&reg, &c1, POWER_BASE_UUID, 2000) == TOPOLOGY_REGISTRY_OK,
          "session C (gen=3, SMALLER than both prior generations) is still accepted as a new session");

    /* A delayed packet from session B (now superseded by C) is rejected. */
    topology_report_t b_delayed = make_report(1, 1, POWER_BASE_UUID, 7, 555, TOPOLOGY_REPORT_HEARTBEAT);
    CHECK(topology_registry_apply_report(&reg, &b_delayed, POWER_BASE_UUID, 2100) == TOPOLOGY_REGISTRY_ERR_STALE_SESSION,
          "a delayed packet from superseded session B (gen=7) is rejected after session C began");

    /* Documented, bounded limitation: only the IMMEDIATELY preceding
     * session is remembered as "superseded". A very old, delayed packet
     * from session A (gen=5, two transitions back) is no longer
     * recognized as stale -- it looks like just another "different
     * generation" and is accepted as yet another new session. This is a
     * deliberate tradeoff (see topology_registry.h's top comment and the
     * final report's "남은 단순화" section), not an oversight -- asserted
     * here explicitly so the boundary of the guarantee is documented and
     * would be caught by this test if it ever silently changed. */
    topology_report_t a_very_delayed = make_report(1, 1, POWER_BASE_UUID, 5, 1000, TOPOLOGY_REPORT_HEARTBEAT);
    CHECK(topology_registry_apply_report(&reg, &a_very_delayed, POWER_BASE_UUID, 3000) == TOPOLOGY_REGISTRY_OK,
          "KNOWN LIMITATION: a packet from TWO sessions back (gen=5, superseded was gen=7 by the time C began) "
          "is no longer recognized as stale and is accepted -- only the immediately-preceding session is guarded");
}

static void scenario_direct_child_detach_offlines_subtree(void)
{
    printf("\n-- scenario: direct-child detach offlines the whole subtree --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    uint8_t uuid1[TOPOLOGY_UUID_LEN], uuid2[TOPOLOGY_UUID_LEN], uuid3[TOPOLOGY_UUID_LEN];
    fill_uuid(uuid1, 1);
    fill_uuid(uuid2, 2);
    fill_uuid(uuid3, 3);

    topology_report_t r1 = make_report(1, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_apply_report(&reg, &r1, POWER_BASE_UUID, 0);
    topology_report_t r2 = make_report(2, 2, uuid1, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_apply_report(&reg, &r2, POWER_BASE_UUID, 0);
    topology_report_t r3 = make_report(3, 3, uuid2, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_apply_report(&reg, &r3, POWER_BASE_UUID, 0);

    /* Simulates power_base_transport_on_direct_downstream_detached(): the
     * power base's own physical DOWNSTREAM port lost contact with node1. */
    topology_registry_mark_offline(&reg, uuid1);

    const topology_registry_entry_t *e1 = topology_registry_find(&reg, uuid1);
    const topology_registry_entry_t *e2 = topology_registry_find(&reg, uuid2);
    const topology_registry_entry_t *e3 = topology_registry_find(&reg, uuid3);
    CHECK(e1 != NULL && !e1->online, "node1 (the direct child) itself is marked offline");
    CHECK(e2 != NULL && !e2->online, "node2 (grandchild via node1) is cascaded offline");
    CHECK(e3 != NULL && !e3->online, "node3 (great-grandchild) is cascaded offline too");
}

static void scenario_intermediate_downstream_detach_offlines_below_only(void)
{
    printf("\n-- scenario: an intermediate's downstream loss offlines only what's below it --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    uint8_t uuid1[TOPOLOGY_UUID_LEN], uuid2[TOPOLOGY_UUID_LEN];
    fill_uuid(uuid1, 1);
    fill_uuid(uuid2, 2);

    topology_report_t r1 = make_report(1, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    r1.downstream_connected = true;
    topology_registry_apply_report(&reg, &r1, POWER_BASE_UUID, 0);
    topology_report_t r2 = make_report(2, 2, uuid1, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_apply_report(&reg, &r2, POWER_BASE_UUID, 0);

    /* node1 (still itself online, still reachable through the power base)
     * reports that ITS OWN downstream just disconnected. */
    topology_report_t update = make_report(1, 1, POWER_BASE_UUID, 1, 2, TOPOLOGY_REPORT_UPDATED);
    update.downstream_connected = false;
    topology_registry_result_t r = topology_registry_apply_report(&reg, &update, POWER_BASE_UUID, 100);
    CHECK(r == TOPOLOGY_REGISTRY_OK, "node1's downstream-lost UPDATED report is accepted");

    const topology_registry_entry_t *e1 = topology_registry_find(&reg, uuid1);
    const topology_registry_entry_t *e2 = topology_registry_find(&reg, uuid2);
    CHECK(e1 != NULL && e1->online, "node1 itself stays ONLINE -- it successfully reported, only its child is gone");
    CHECK(e2 != NULL && !e2->online, "node2 (below the broken downstream link) is marked offline");
}

static void scenario_stale_timeout_and_reconnect(void)
{
    printf("\n-- scenario: stale timeout, then reconnect recovers the SAME uuid's entry --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    uint8_t uuid1[TOPOLOGY_UUID_LEN];
    fill_uuid(uuid1, 1);
    topology_report_t r1 = make_report(1, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    topology_registry_apply_report(&reg, &r1, POWER_BASE_UUID, 1000);

    topology_registry_sweep_stale(&reg, 1000 + 5000, 60000);
    CHECK(topology_registry_find(&reg, uuid1)->online, "well within the stale timeout: still online");

    topology_registry_sweep_stale(&reg, 1000 + 70000, 60000);
    CHECK(!topology_registry_find(&reg, uuid1)->online, "past the stale timeout with no new report: marked offline");

    /* A momentary radio blip should NOT delete the node -- it must still be
     * findable (with its history) so a reconnect updates the SAME entry. */
    CHECK(topology_registry_count(&reg) == 1, "the stale node is not deleted, only marked offline");

    topology_report_t reconnect = make_report(1, 1, POWER_BASE_UUID, 1, 2, TOPOLOGY_REPORT_UPDATED);
    topology_registry_result_t r = topology_registry_apply_report(&reg, &reconnect, POWER_BASE_UUID, 1000 + 71000);
    CHECK(r == TOPOLOGY_REGISTRY_OK, "a reconnect report (same uuid, next seq) is accepted");
    CHECK(topology_registry_find(&reg, uuid1)->online, "the SAME uuid's entry is back online after reconnecting");
    CHECK(topology_registry_count(&reg) == 1, "reconnecting does not create a second entry for the same uuid");
}

static void scenario_base_reboot_registry_repopulated_by_heartbeat(void)
{
    printf("\n-- scenario: base reboot (empty registry) is repopulated by ordinary heartbeats --\n");
    topology_registry_t reg;
    topology_registry_init(&reg); /* mirrors a fresh power_base_transport_init() after reboot -- RAM-only, see topology_registry.h */

    uint8_t uuid1[TOPOLOGY_UUID_LEN];
    fill_uuid(uuid1, 1);
    /* The module itself never rebooted -- its OWN generation/sequence just
     * keep counting from wherever they were. The now-empty registry has
     * never seen this uuid before, so it is accepted as a brand-new entry
     * regardless of how large its generation/sequence numbers already are. */
    topology_report_t heartbeat = make_report(1, 1, POWER_BASE_UUID, 77, 12345, TOPOLOGY_REPORT_HEARTBEAT);
    topology_registry_result_t r = topology_registry_apply_report(&reg, &heartbeat, POWER_BASE_UUID, 5000);
    CHECK(r == TOPOLOGY_REGISTRY_OK, "a HEARTBEAT into an empty (post-reboot) registry re-creates the node");
    CHECK(topology_registry_count(&reg) == 1, "exactly one node exists after the first post-reboot heartbeat");
    CHECK(topology_registry_find(&reg, uuid1)->online, "the re-created node is online");
}

static void scenario_sensor_data_uuid_linked_to_topology_entry(void)
{
    printf("\n-- scenario: sensor-sample UUID is linked to a topology entry (or correctly unknown) --\n");
    topology_registry_t reg;
    topology_registry_init(&reg);

    uint8_t known_uuid[TOPOLOGY_UUID_LEN], unknown_uuid[TOPOLOGY_UUID_LEN];
    fill_uuid(known_uuid, 1);
    fill_uuid(unknown_uuid, 0xEE);

    topology_report_t r1 = make_report(1, 1, POWER_BASE_UUID, 1, 1, TOPOLOGY_REPORT_ATTACHED);
    r1.sensor_type_bitmap = 0x0002;
    topology_registry_apply_report(&reg, &r1, POWER_BASE_UUID, 0);

    /* This is exactly the lookup components/power_base_transport's "/s" CoAP
     * handler performs before calling sensor_root_aggregator_record_sample():
     * a known module_uuid is linked to its topology entry; an unknown one is
     * distinguishable and must be handled as an error, never crash. */
    const topology_registry_entry_t *linked = topology_registry_find(&reg, known_uuid);
    CHECK(linked != NULL, "a sensor sample's module_uuid that DID report topology is found (linked)");
    CHECK(linked != NULL && linked->sensor_bitmap == 0x0002, "the linked entry carries the module's own sensor_type_bitmap");

    const topology_registry_entry_t *not_linked = topology_registry_find(&reg, unknown_uuid);
    CHECK(not_linked == NULL, "a sensor sample's module_uuid that never reported topology is correctly unknown (not a crash)");
}

int main(void)
{
    scenario_depth1_parent_and_self_parent();
    scenario_cycle_and_depth_jump();
    scenario_invalid_role_depth_combinations();
    scenario_duplicate_and_reordered_sequence();
    scenario_session_transition_is_generation_agnostic();
    scenario_direct_child_detach_offlines_subtree();
    scenario_intermediate_downstream_detach_offlines_below_only();
    scenario_stale_timeout_and_reconnect();
    scenario_base_reboot_registry_repopulated_by_heartbeat();
    scenario_sensor_data_uuid_linked_to_topology_entry();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
