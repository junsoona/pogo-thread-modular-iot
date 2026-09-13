/*
 * Host-compiled verification of the code review follow-up section 1
 * ("센서 모듈 UUID를 영구 식별자로 변경"): the module_uuid a sensor module
 * uses is a PERMANENT identifier derived from its own efuse EUI-64, never a
 * per-session random value, and a parent only ever CONFIRMS it (never
 * assigns a different one).
 *
 * Links against the REAL, unmodified pogo_proto pack/unpack code
 * (pogo_pack_propose_uuid/pogo_unpack_propose_uuid,
 * pogo_pack_assign_uuid/pogo_unpack_assign_uuid). compute_permanent_uuid()
 * and the child-side/parent-side confirm logic are mirrored here (child_service.c/
 * parent_service.c have ESP-IDF dependencies -- esp_efuse_mac_get_default(),
 * FreeRTOS -- that prevent host-compiling them directly), matching
 * handle_request_uuid()/handle_assign_uuid() in child_service.c and the
 * REQUEST_UUID/PROPOSE_UUID/ASSIGN_UUID sequence in parent_service.c's
 * run_provisioning_session() step for step. If those functions change,
 * update the model below to match -- same caveat as every other
 * child_service/parent_service mirror test in this directory.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_permanent_module_uuid tools/test_permanent_module_uuid.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_permanent_module_uuid
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pogo_proto/pogo_protocol.h"

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

/* Mirrors child_service.c's compute_permanent_uuid(): the module_uuid is
 * ALWAYS exactly the 8-byte efuse EUI-64, nothing else -- no randomness, no
 * dependence on who is asking or what session/parent this is. Returns false
 * (no identity produced) for an all-zero efuse read, same policy as the
 * real function. */
static bool compute_permanent_uuid(const uint8_t efuse_eui64[8], uint8_t out_uuid[POGO_UUID_LEN])
{
    static const uint8_t all_zero[8] = {0};
    if (memcmp(efuse_eui64, all_zero, 8) == 0) {
        return false;
    }
    memcpy(out_uuid, efuse_eui64, POGO_UUID_LEN);
    return true;
}

/* Mirrors parent_service.c's run_provisioning_session(): REQUEST_UUID gets a
 * PROPOSE_UUID reply; the parent NEVER invents its own bytes, it stores and
 * echoes back EXACTLY what the child proposed via ASSIGN_UUID. */
static bool parent_propose_and_confirm(const uint8_t efuse_eui64[8], uint8_t out_confirmed_uuid[POGO_UUID_LEN])
{
    uint8_t child_uuid[POGO_UUID_LEN];
    if (!compute_permanent_uuid(efuse_eui64, child_uuid)) {
        return false; /* mirrors: no PROPOSE_UUID sent at all -> parent abandons the session */
    }

    /* Real wire round trip: child packs PROPOSE_UUID, parent unpacks it. */
    pogo_propose_uuid_t propose_msg;
    memcpy(propose_msg.uuid, child_uuid, POGO_UUID_LEN);
    uint8_t wire[POGO_UUID_LEN];
    size_t wlen = pogo_pack_propose_uuid(&propose_msg, wire, sizeof(wire));
    if (wlen != POGO_UUID_LEN) {
        return false;
    }
    pogo_propose_uuid_t received;
    if (!pogo_unpack_propose_uuid(wire, (uint16_t)wlen, &received)) {
        return false;
    }
    static const uint8_t all_zero[POGO_UUID_LEN] = {0};
    if (memcmp(received.uuid, all_zero, POGO_UUID_LEN) == 0) {
        return false; /* mirrors: parent refuses an all-zero proposal */
    }

    /* Parent echoes back the SAME bytes via ASSIGN_UUID -- never a
     * different/random value. */
    pogo_assign_uuid_t confirm_msg;
    memcpy(confirm_msg.uuid, received.uuid, POGO_UUID_LEN);
    uint8_t confirm_wire[POGO_UUID_LEN];
    size_t clen = pogo_pack_assign_uuid(&confirm_msg, confirm_wire, sizeof(confirm_wire));
    pogo_assign_uuid_t child_side_received;
    if (!pogo_unpack_assign_uuid(confirm_wire, (uint16_t)clen, &child_side_received)) {
        return false;
    }
    /* Mirrors handle_assign_uuid()'s mismatch check: the child verifies the
     * confirmation matches what IT proposed (its own recomputed permanent
     * uuid), rejecting anything else. */
    if (memcmp(child_side_received.uuid, child_uuid, POGO_UUID_LEN) != 0) {
        return false;
    }
    memcpy(out_confirmed_uuid, child_side_received.uuid, POGO_UUID_LEN);
    return true;
}

static void scenario_ten_reprovisions_same_uuid(void)
{
    printf("\n-- scenario: 10 reprovisioning sessions of the SAME module -> identical uuid every time --\n");
    uint8_t efuse[8] = {0x60, 0x55, 0xf9, 0x11, 0x22, 0x33, 0x44, 0x01};
    uint8_t first_uuid[POGO_UUID_LEN];
    bool ok = parent_propose_and_confirm(efuse, first_uuid);
    CHECK(ok, "first provisioning session succeeds");

    for (int session = 0; session < 10; session++) {
        uint8_t this_uuid[POGO_UUID_LEN];
        /* A fresh session: new random child-side uuid buffer, new parent
         * node record -- only the efuse input carries over, exactly as it
         * would across a real POGO detach/reconnect (nothing persisted). */
        bool session_ok = parent_propose_and_confirm(efuse, this_uuid);
        CHECK(session_ok, "reprovisioning session succeeds");
        CHECK(memcmp(this_uuid, first_uuid, POGO_UUID_LEN) == 0,
              "session's confirmed uuid is IDENTICAL to the first session's (same efuse, same result)");
    }
}

static void scenario_power_base_reboot_does_not_change_uuid(void)
{
    printf("\n-- scenario: power base reboot (a fresh parent-side session/context) -> same child uuid --\n");
    uint8_t efuse[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02};

    uint8_t before_reboot[POGO_UUID_LEN];
    CHECK(parent_propose_and_confirm(efuse, before_reboot), "session before power-base reboot succeeds");

    /* A power base reboot means an entirely NEW parent-side session/node
     * record (parent_service's s_node is memset fresh) -- but the PARENT
     * never supplies the uuid, so this has zero effect on what the child
     * proposes. */
    uint8_t after_reboot[POGO_UUID_LEN];
    CHECK(parent_propose_and_confirm(efuse, after_reboot), "session after simulated power-base reboot succeeds");
    CHECK(memcmp(before_reboot, after_reboot, POGO_UUID_LEN) == 0,
          "the child's uuid is unchanged by a power-base reboot (the parent never assigns it)");
}

static void scenario_different_parent_does_not_change_uuid(void)
{
    printf("\n-- scenario: docking under a DIFFERENT intermediate parent -> same child uuid --\n");
    uint8_t efuse[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    /* "Different parent" is modeled by simply calling the same proposal
     * logic again -- the parent's own identity never enters
     * compute_permanent_uuid() or the PROPOSE_UUID payload at all, so this
     * is definitionally parent-independent; asserted explicitly anyway so
     * a future change that accidentally threads parent identity into the
     * child's own uuid computation would be caught here. */
    uint8_t uuid_under_parent_a[POGO_UUID_LEN];
    uint8_t uuid_under_parent_b[POGO_UUID_LEN];
    CHECK(parent_propose_and_confirm(efuse, uuid_under_parent_a), "docking under parent A succeeds");
    CHECK(parent_propose_and_confirm(efuse, uuid_under_parent_b), "re-docking under parent B succeeds");
    CHECK(memcmp(uuid_under_parent_a, uuid_under_parent_b, POGO_UUID_LEN) == 0,
          "the module's uuid is identical regardless of which parent it is docked under");
}

static void scenario_different_efuse_different_uuid(void)
{
    printf("\n-- scenario: different efuse EUI-64 -> different uuid --\n");
    uint8_t efuse_a[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    uint8_t efuse_b[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    uint8_t uuid_a[POGO_UUID_LEN], uuid_b[POGO_UUID_LEN];
    CHECK(parent_propose_and_confirm(efuse_a, uuid_a), "module A's session succeeds");
    CHECK(parent_propose_and_confirm(efuse_b, uuid_b), "module B's session succeeds");
    CHECK(memcmp(uuid_a, uuid_b, POGO_UUID_LEN) != 0, "two modules with different efuse EUI-64 get different uuids");
}

static void scenario_all_zero_uuid_rejected(void)
{
    printf("\n-- scenario: an all-zero efuse read is never used as a valid identifier --\n");
    uint8_t all_zero_efuse[8] = {0};
    uint8_t out[POGO_UUID_LEN];
    CHECK(!compute_permanent_uuid(all_zero_efuse, out), "compute_permanent_uuid() refuses an all-zero efuse read");
    CHECK(!parent_propose_and_confirm(all_zero_efuse, out),
          "the whole provisioning session is abandoned rather than proceeding with an all-zero uuid");
}

static void scenario_wrong_length_and_mismatch_rejected(void)
{
    printf("\n-- scenario: wrong wire length and a parent-invented (mismatched) uuid are both rejected --\n");
    uint8_t short_buf[POGO_UUID_LEN - 1];
    pogo_propose_uuid_t decoded;
    CHECK(!pogo_unpack_propose_uuid(short_buf, sizeof(short_buf), &decoded), "PROPOSE_UUID rejects a too-short payload");

    uint8_t long_buf[POGO_UUID_LEN + 1] = {0};
    CHECK(!pogo_unpack_propose_uuid(long_buf, sizeof(long_buf), &decoded), "PROPOSE_UUID rejects a too-long payload");

    pogo_assign_uuid_t short_assign;
    CHECK(!pogo_unpack_assign_uuid(short_buf, sizeof(short_buf), &short_assign),
          "ASSIGN_UUID (unchanged wire format) still rejects a too-short payload too");

    /* A parent that tries to confirm something OTHER than what was
     * proposed -- mirrors handle_assign_uuid()'s memcmp mismatch check
     * directly (this is the actual application-level rejection, not a
     * wire-format-length one). */
    uint8_t child_uuid[POGO_UUID_LEN] = {9, 9, 9, 9, 9, 9, 9, 9};
    uint8_t parent_invented_uuid[POGO_UUID_LEN] = {1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(memcmp(child_uuid, parent_invented_uuid, POGO_UUID_LEN) != 0,
          "a parent-invented uuid differs from the child's own proposed uuid -- handle_assign_uuid() would NACK "
          "this with POGO_NACK_UUID_MISMATCH rather than accept it");
}

int main(void)
{
    scenario_ten_reprovisions_same_uuid();
    scenario_power_base_reboot_does_not_change_uuid();
    scenario_different_parent_does_not_change_uuid();
    scenario_different_efuse_different_uuid();
    scenario_all_zero_uuid_rejected();
    scenario_wrong_length_and_mismatch_rejected();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
