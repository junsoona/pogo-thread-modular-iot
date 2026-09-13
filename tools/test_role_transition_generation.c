/*
 * Host-compiled test for generation-counter invalidation across a role
 * transition -- section 13 of the follow-up spec: "role transition
 * generation 무효화". Mirrors module/main/app_main.c's
 * s_root_watch_generation / root_ready_watcher_task() pattern: a watcher
 * (or, generically, any long-running async check spawned for one specific
 * role-attempt) only ever acts if the generation it captured at spawn time
 * still matches the current one; leaving that role, or spawning a newer
 * watcher, bumps the generation and makes every older watcher a no-op.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_role_transition_generation tools/test_role_transition_generation.c \
 *   && /tmp/test_role_transition_generation
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* --- model of app_main.c's generation bookkeeping --- */
static uint32_t g_generation;

static uint32_t start_watcher(void)
{
    /* Mirrors on_role_change()'s `uint32_t gen = ++s_root_watch_generation;`
     * right before xTaskCreate(root_ready_watcher_task, ...). */
    return ++g_generation;
}

static void leave_role(void)
{
    /* Mirrors on_role_change()'s `s_root_watch_generation++;` on leaving
     * ROOT, or entering UNASSIGNED/ERROR. */
    g_generation++;
}

/* Mirrors root_ready_watcher_task()'s own generation check before acting. */
static bool watcher_may_act(uint32_t my_gen)
{
    return my_gen == g_generation;
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s\n", msg);                                                                               \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

static void test_fresh_watcher_may_act(void)
{
    g_generation = 0;
    uint32_t gen = start_watcher();
    CHECK(watcher_may_act(gen), "a freshly-started watcher may act (nothing has superseded it yet)");
}

static void test_role_left_invalidates_watcher(void)
{
    g_generation = 0;
    uint32_t gen = start_watcher();
    leave_role(); /* e.g. this module left ROOT, or entered UNASSIGNED/ERROR */
    CHECK(!watcher_may_act(gen), "a watcher from a role we've since left may NOT act");
}

static void test_newer_watcher_supersedes_older(void)
{
    g_generation = 0;
    uint32_t gen1 = start_watcher();
    uint32_t gen2 = start_watcher(); /* e.g. ROOT -> UNASSIGNED -> ROOT again, quickly */
    CHECK(gen1 != gen2, "each new watcher gets a distinct generation");
    CHECK(!watcher_may_act(gen1), "the OLDER watcher may not act once a newer one exists");
    CHECK(watcher_may_act(gen2), "the NEWEST watcher may still act");
}

static void test_unrelated_role_reached_for_other_reasons(void)
{
    /* "e.g. this module later becomes INTERMEDIATE/LEAF and legitimately
     * reaches CHILD/ROUTER for a completely different, unrelated reason" --
     * the stale ROOT watcher must still be unable to act just because the
     * Thread role happened to change again afterward. */
    g_generation = 0;
    uint32_t root_gen = start_watcher();
    leave_role();      /* left ROOT */
    start_watcher();   /* a new watcher started under a completely different context, e.g. re-entering ROOT later, or none at all */
    CHECK(!watcher_may_act(root_gen), "an old ROOT watcher stays invalid regardless of what happens afterward");
}

static void test_at_most_one_watcher_result_ever_takes_effect(void)
{
    /* Simulates two watchers racing: only the currently-valid one's result
     * is ever allowed to flip application state. */
    g_generation = 0;
    uint32_t gen1 = start_watcher();
    uint32_t gen2 = start_watcher();
    int applied_count = 0;
    if (watcher_may_act(gen1)) applied_count++;
    if (watcher_may_act(gen2)) applied_count++;
    CHECK(applied_count == 1, "exactly one of two racing watchers is allowed to take effect");
}

int main(void)
{
    test_fresh_watcher_may_act();
    test_role_left_invalidates_watcher();
    test_newer_watcher_supersedes_older();
    test_unrelated_role_reached_for_other_reasons();
    test_at_most_one_watcher_result_ever_takes_effect();

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
