/*
 * Host-compiled verification of components/role_manager/role_manager.c's
 * AUTO-mode recompute() -- required test items 7/8/9/10 of the power-base
 * follow-up spec: "첫 센서 모듈이 ROOT가 되지 않음", "1단 체인: LEAF",
 * "2단 체인: INTERMEDIATE -> LEAF", "3단 체인: INTERMEDIATE ->
 * INTERMEDIATE -> LEAF".
 *
 * role_manager.c cannot be host-compiled directly (role_manager.h pulls in
 * pogo_port.h, which has an ESP-IDF driver/gpio.h + driver/uart.h
 * dependency) -- this mirrors recompute() step for step instead, same
 * approach as every other pogo_port/child_service/parent_service mirror
 * test in this directory. The fix under test: an earlier revision set
 * MODULE_ROLE_ROOT the moment the UPSTREAM peer was POWER_BASE; role_manager.c
 * (see its own comment on this) now treats POWER_BASE and MODULE upstream
 * peers identically -- LEAF/INTERMEDIATE decided purely by DOWNSTREAM state,
 * exactly mirrored below.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_role_manager_auto_chain tools/test_role_manager_auto_chain.c \
 *   && /tmp/test_role_manager_auto_chain
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum { PEER_UNKNOWN = 0, PEER_POWER_BASE = 1, PEER_MODULE = 2 } peer_type_t;
typedef enum { ROLE_UNASSIGNED = 0, ROLE_ROOT, ROLE_INTERMEDIATE, ROLE_LEAF, ROLE_ERROR } role_t;

typedef struct {
    bool attached;
    bool error;
    peer_type_t peer_type;
} port_state_t;

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

/* Mirrors the FIXED recompute() in role_manager.c exactly (s_forced == false,
 * i.e. dev_mode == AUTO -- the forced DEV_ROOT/DEV_LEAF paths never call
 * this function at all). */
static role_t recompute(port_state_t up, port_state_t down, role_t previous)
{
    if (up.error || down.error) {
        return ROLE_ERROR;
    }
    if (!up.attached || up.peer_type == PEER_UNKNOWN) {
        return ROLE_UNASSIGNED;
    }
    /* An AUTO-mode sensor module never becomes ROOT, whether the UPSTREAM
     * peer is the power base or another module -- see role_manager.c. */
    if (!down.attached) {
        return ROLE_LEAF;
    }
    if (down.peer_type == PEER_MODULE) {
        return ROLE_INTERMEDIATE;
    }
    return previous; /* DOWNSTREAM attached but not yet resolved -- keep previous role */
}

static void scenario_first_module_never_root(void)
{
    printf("\n-- scenario: the first sensor module's UPSTREAM peer is POWER_BASE -- never ROOT --\n");

    port_state_t up = {.attached = true, .error = false, .peer_type = PEER_POWER_BASE};
    port_state_t down_empty = {0};
    role_t role = recompute(up, down_empty, ROLE_UNASSIGNED);
    CHECK(role != ROLE_ROOT, "UPSTREAM=POWER_BASE never yields ROLE_ROOT any more");
    CHECK(role == ROLE_LEAF, "UPSTREAM=POWER_BASE, DOWNSTREAM empty -> LEAF (the first module's actual role)");

    port_state_t down_module = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    role = recompute(up, down_module, ROLE_LEAF);
    CHECK(role != ROLE_ROOT, "UPSTREAM=POWER_BASE with a module docked below still never yields ROLE_ROOT");
    CHECK(role == ROLE_INTERMEDIATE, "UPSTREAM=POWER_BASE, DOWNSTREAM=MODULE -> INTERMEDIATE");
}

static void scenario_one_stage_chain_leaf(void)
{
    printf("\n-- scenario: 1-stage chain (power base -- one module) -> LEAF --\n");
    port_state_t up = {.attached = true, .error = false, .peer_type = PEER_POWER_BASE};
    port_state_t down = {0};
    role_t role = recompute(up, down, ROLE_UNASSIGNED);
    CHECK(role == ROLE_LEAF, "the only module in a 1-stage chain is LEAF");
}

static void scenario_two_stage_chain_intermediate_then_leaf(void)
{
    printf("\n-- scenario: 2-stage chain -> INTERMEDIATE (module A) -> LEAF (module B) --\n");
    /* Module A: upstream=power base, downstream=module B. */
    port_state_t a_up = {.attached = true, .error = false, .peer_type = PEER_POWER_BASE};
    port_state_t a_down = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    role_t role_a = recompute(a_up, a_down, ROLE_UNASSIGNED);
    CHECK(role_a == ROLE_INTERMEDIATE, "module A (power base above, module B below) is INTERMEDIATE");

    /* Module B: upstream=module A, downstream=empty. */
    port_state_t b_up = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    port_state_t b_down = {0};
    role_t role_b = recompute(b_up, b_down, ROLE_UNASSIGNED);
    CHECK(role_b == ROLE_LEAF, "module B (module A above, nothing below) is LEAF");
}

static void scenario_three_stage_chain(void)
{
    printf("\n-- scenario: 3-stage chain -> INTERMEDIATE -> INTERMEDIATE -> LEAF --\n");
    /* A: power base above, B below. */
    port_state_t a_up = {.attached = true, .error = false, .peer_type = PEER_POWER_BASE};
    port_state_t a_down = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    CHECK(recompute(a_up, a_down, ROLE_UNASSIGNED) == ROLE_INTERMEDIATE, "module A is INTERMEDIATE");

    /* B: A above, C below. */
    port_state_t b_up = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    port_state_t b_down = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    CHECK(recompute(b_up, b_down, ROLE_UNASSIGNED) == ROLE_INTERMEDIATE, "module B is INTERMEDIATE");

    /* C: B above, nothing below. */
    port_state_t c_up = {.attached = true, .error = false, .peer_type = PEER_MODULE};
    port_state_t c_down = {0};
    CHECK(recompute(c_up, c_down, ROLE_UNASSIGNED) == ROLE_LEAF, "module C (the end of the chain) is LEAF");
}

static void scenario_unassigned_and_error(void)
{
    printf("\n-- scenario: UNASSIGNED and ROLE_ERROR are unaffected by this fix --\n");
    port_state_t nothing = {0};
    CHECK(recompute(nothing, nothing, ROLE_LEAF) == ROLE_UNASSIGNED, "no UPSTREAM attachment at all -> UNASSIGNED");

    port_state_t up_err = {.attached = true, .error = true, .peer_type = PEER_MODULE};
    port_state_t down_ok = {0};
    CHECK(recompute(up_err, down_ok, ROLE_LEAF) == ROLE_ERROR, "an UPSTREAM port error -> ROLE_ERROR regardless of role");
}

int main(void)
{
    scenario_first_module_never_root();
    scenario_one_stage_chain_leaf();
    scenario_two_stage_chain_intermediate_then_leaf();
    scenario_three_stage_chain();
    scenario_unassigned_and_error();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
