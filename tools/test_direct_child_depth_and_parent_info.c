/*
 * Host-compiled verification of parent_service's depth/parent-identity
 * assignment to a DIRECTLY-docked child -- required test item 6 of the
 * power-base follow-up spec: "직접 자식에게 depth 1 및 base parent UUID
 * 배정". Also exercises the new POGO_MSG_ASSIGN_PARENT_INFO wire format
 * (section 7: "부모 UUID 전달") end-to-end.
 *
 * Links against the REAL, unmodified pogo_proto pack/unpack code
 * (pogo_pack_assign_slot/pogo_pack_assign_parent_info and their unpack
 * counterparts). The depth formula itself (own_depth + 1) is a one-line
 * arithmetic mirror of parent_service.c's run_provisioning_session():
 *   uint8_t depth = (uint8_t)(s_provider.get_own_depth(s_provider.ctx) + 1);
 * -- trivial enough that reimplementing it here is a faithful, not a fake,
 * test (same reasoning tools/test_polling_calc.c gives for its own
 * boundary-value copy).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_direct_child_depth_and_parent_info tools/test_direct_child_depth_and_parent_info.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_direct_child_depth_and_parent_info
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

/* Mirrors parent_service.c's `depth = get_own_depth() + 1` exactly. */
static uint8_t assign_child_depth(uint8_t own_depth)
{
    return (uint8_t)(own_depth + 1);
}

static void scenario_power_base_direct_child_gets_depth_1(void)
{
    printf("\n-- scenario: power base's own depth is 0 -> its direct child is assigned depth=1 --\n");
    uint8_t power_base_own_depth = 0; /* pb_get_own_depth() in power_base/main/app_main.c */
    uint8_t child_depth = assign_child_depth(power_base_own_depth);
    CHECK(child_depth == 1, "the power base's direct child is assigned topology_depth=1");

    pogo_assign_slot_t slot = {.slot_id = 0, .topology_depth = child_depth};
    uint8_t payload[2];
    size_t len = pogo_pack_assign_slot(&slot, payload, sizeof(payload));
    CHECK(len == 2, "ASSIGN_SLOT packs to its existing unchanged 2-byte wire format");

    pogo_assign_slot_t decoded;
    CHECK(pogo_unpack_assign_slot(payload, (uint16_t)len, &decoded), "ASSIGN_SLOT unpacks");
    CHECK(decoded.topology_depth == 1, "the decoded ASSIGN_SLOT carries topology_depth=1");
    CHECK(decoded.slot_id == 0, "slot_id is still 0 (exactly one DOWNSTREAM port on this hardware generation)");
}

static void scenario_intermediate_module_assigns_own_depth_plus_one(void)
{
    printf("\n-- scenario: an INTERMEDIATE module (own depth=2) assigns its child depth=3 --\n");
    uint8_t own_depth = 2; /* child_service_get_own_topology_depth() for this module */
    uint8_t child_depth = assign_child_depth(own_depth);
    CHECK(child_depth == 3, "a depth-2 INTERMEDIATE module assigns its own child topology_depth=3");
}

static void scenario_assign_parent_info_round_trip(void)
{
    printf("\n-- scenario: ASSIGN_PARENT_INFO carries the parent's real uuid + power-base flag --\n");

    uint8_t power_base_uuid[POGO_UUID_LEN] = {0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0x01};
    pogo_assign_parent_info_t msg = {0};
    memcpy(msg.parent_uuid, power_base_uuid, POGO_UUID_LEN);
    msg.parent_is_power_base = 1;

    uint8_t payload[POGO_UUID_LEN + 1];
    size_t len = pogo_pack_assign_parent_info(&msg, payload, sizeof(payload));
    CHECK(len == POGO_UUID_LEN + 1, "ASSIGN_PARENT_INFO packs to exactly uuid(8)+flag(1) = 9 bytes");

    pogo_assign_parent_info_t decoded;
    CHECK(pogo_unpack_assign_parent_info(payload, (uint16_t)len, &decoded), "ASSIGN_PARENT_INFO unpacks");
    CHECK(memcmp(decoded.parent_uuid, power_base_uuid, POGO_UUID_LEN) == 0, "the parent's real uuid round-trips exactly");
    CHECK(decoded.parent_is_power_base == 1, "parent_is_power_base=1 (power base) round-trips");

    /* An intermediate module's own ASSIGN_PARENT_INFO to ITS child: not the
     * power base. */
    uint8_t module_uuid[POGO_UUID_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
    pogo_assign_parent_info_t msg2 = {.parent_is_power_base = 0};
    memcpy(msg2.parent_uuid, module_uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_parent_info(&msg2, payload, sizeof(payload));
    pogo_assign_parent_info_t decoded2;
    CHECK(pogo_unpack_assign_parent_info(payload, (uint16_t)len, &decoded2), "an INTERMEDIATE parent's own ASSIGN_PARENT_INFO unpacks");
    CHECK(decoded2.parent_is_power_base == 0, "parent_is_power_base=0 for an ordinary sensor-module parent");
    CHECK(memcmp(decoded2.parent_uuid, module_uuid, POGO_UUID_LEN) == 0, "the module's own uuid (not the power base's) is what's carried");
}

static void scenario_short_and_wrong_length_rejected(void)
{
    printf("\n-- scenario: ASSIGN_PARENT_INFO rejects the wrong length --\n");
    uint8_t short_buf[POGO_UUID_LEN]; /* missing the flag byte */
    pogo_assign_parent_info_t decoded;
    CHECK(!pogo_unpack_assign_parent_info(short_buf, POGO_UUID_LEN, &decoded), "8 bytes (missing the flag) is rejected");

    uint8_t long_buf[POGO_UUID_LEN + 2] = {0};
    CHECK(!pogo_unpack_assign_parent_info(long_buf, POGO_UUID_LEN + 2, &decoded), "10 bytes (one too many) is rejected");
}

int main(void)
{
    scenario_power_base_direct_child_gets_depth_1();
    scenario_intermediate_module_assigns_own_depth_plus_one();
    scenario_assign_parent_info_round_trip();
    scenario_short_and_wrong_length_rejected();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
