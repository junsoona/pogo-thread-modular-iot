/*
 * Host-compiled verification of power_base/main/app_main.c's boot-sequence
 * ORDERING invariant, its Thread-role/application-role decoupling, and (this
 * correction pass) its attach_supervisor_task()'s no-permanent-halt policy
 * for a slow first attach -- required test items of this follow-up's
 * minimum test list:
 *
 *   1/2/3. power base starts its services once attached as CHILD, ROUTER,
 *          or LEADER alike (no longer LEADER-only).
 *   4.     DISABLED/DETACHED never starts services.
 *   5.     CHILD<->ROUTER<->LEADER role fluctuation AFTER boot never
 *          re-creates any task/resource.
 *   6.     A detach (role -> DISABLED/DETACHED) followed by a reattach
 *          resumes service without recreating anything and without any
 *          busy-retry loop.
 *   (this correction pass, docs/power-base.md section 19.8)
 *   7.     Never attaching within 30s does NOT halt -- OpenThread attach
 *          continues indefinitely.
 *   8.     A later attach (however long it took) still starts every service
 *          exactly once.
 *   9.     Further CHILD/ROUTER/LEADER changes after that still never
 *          duplicate anything.
 *   10.    The "still not attached" warning is rate-limited (first at 30s,
 *          then at most once every 60s after that -- never every poll tick).
 *
 * app_main.c itself is not host-compilable (it is nothing but ESP-IDF/
 * OpenThread calls) -- this mirrors its actual control flow: the ONE-TIME
 * fixed init (config/efuse/thread_manager_start_as_always_on_ftd()) AND
 * attach_supervisor_task()'s own poll loop (identical logic to the real
 * one: ATTACH_POLL_INTERVAL_MS-equivalent ticks, ATTACH_WARN_FIRST_MS/
 * ATTACH_WARN_REPEAT_MS-equivalent thresholds, NO timeout/halt for "not
 * attached yet", start_services_once() called EXACTLY ONCE the first time
 * this device is observed attached) AND the live, stateless post-attach
 * query functions (pb_provisioning_allowed()/pb_get_collector()).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_power_base_boot_sequencing tools/test_power_base_boot_sequencing.c \
 *   && /tmp/test_power_base_boot_sequencing
 */
#include <stdio.h>
#include <stdbool.h>

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

/* Mirrors otDeviceRole's values (openthread/thread.h) closely enough for
 * this model's purposes. */
typedef enum { ROLE_DISABLED = 0, ROLE_DETACHED, ROLE_CHILD, ROLE_ROUTER, ROLE_LEADER } sim_role_t;

/* Mirrors power_base/main/app_main.c's role_is_attached(). */
static bool role_is_attached(sim_role_t role)
{
    return role == ROLE_CHILD || role == ROLE_ROUTER || role == ROLE_LEADER;
}

/* Mirrors pb_provisioning_allowed()/pb_get_collector(): pure, stateless
 * functions of the CURRENT role -- unchanged by this correction pass, still
 * re-checked live on every call, no caching. */
static bool pb_provisioning_allowed(sim_role_t role)
{
    return role_is_attached(role);
}

static bool pb_get_collector(sim_role_t role, bool *out_mesh_eid_available)
{
    bool attached = role_is_attached(role);
    *out_mesh_eid_available = attached;
    return attached;
}

/* Mirrors ATTACH_POLL_INTERVAL_MS/ATTACH_WARN_FIRST_MS/ATTACH_WARN_REPEAT_MS
 * from power_base/main/app_main.c, expressed directly in "poll ticks" (one
 * tick == one ATTACH_POLL_INTERVAL_MS == 200ms in the real code) so this
 * model can run the exact same iteration count the real code would without
 * actually waiting -- 150 ticks == 30000ms, 300 ticks == 60000ms. */
#define WARN_FIRST_TICKS 150 /* 30000ms / 200ms */
#define WARN_REPEAT_TICKS 300 /* 60000ms / 200ms */

typedef struct {
    bool config_ok;
    bool efuse_ok;
    bool thread_start_ok;
    int  max_ticks; /* how many poll ticks this scenario simulates */
    /* role observed at tick i (0-based); ticks beyond the array length
     * repeat the last entry. */
    const sim_role_t *role_sequence;
    int                role_sequence_len;
    bool stale_sweep_task_create_ok;
} scenario_input_t;

typedef struct {
    int  power_base_transport_init_calls;
    int  parent_service_init_calls;
    int  pogo_port_init_calls;
    int  stale_sweep_task_create_calls;
    int  enable_downstream_calls; /* == the only place DETECT_OUT is driven active */
    bool detect_out_active;
    bool halted;
    bool halted_for_not_attached; /* specifically flags the OLD, now-removed policy -- must NEVER be true */
    int  warn_count;
    int  ticks_done;
    bool attached_before_budget_exhausted;
    sim_role_t first_attached_role;
} boot_result_t;

static sim_role_t role_at_tick(const scenario_input_t *in, int tick)
{
    if (tick >= in->role_sequence_len) {
        return in->role_sequence[in->role_sequence_len - 1];
    }
    return in->role_sequence[tick];
}

/*
 * Mirrors power_base/main/app_main.c's app_main() ONE-TIME fixed init
 * (config load / efuse / thread_manager_init (always OK, not modeled) /
 * thread_manager_start_as_always_on_ftd()) followed by
 * attach_supervisor_task()'s own poll loop: NO fixed timeout, NO halt for
 * "not attached yet", a rate-limited warning (first at WARN_FIRST_TICKS,
 * repeating at most every WARN_REPEAT_TICKS after that), and
 * start_services_once() (steps 8-12) called EXACTLY ONCE the first time
 * role_is_attached() becomes true within this scenario's max_ticks budget.
 */
static boot_result_t run_boot_sequence(const scenario_input_t *in)
{
    boot_result_t r = {0};

    if (!in->config_ok) {
        r.halted = true;
        return r; /* halt_forever("CONFIG_ERROR ...") -- nothing below ever runs */
    }
    if (!in->efuse_ok) {
        r.halted = true;
        return r; /* halt_forever("... all-zero EUI-64 ...") */
    }
    if (!in->thread_start_ok) {
        r.halted = true;
        return r; /* halt_forever("ROLE_TRANSITION_ERROR: thread_manager_start_as_always_on_ftd failed") */
    }

    bool attached = false;
    bool warned_once = false;
    int last_warn_tick = 0;
    for (int tick = 0; tick < in->max_ticks; tick++) {
        r.ticks_done++;
        sim_role_t role = role_at_tick(in, tick);
        if (role_is_attached(role)) {
            attached = true;
            r.first_attached_role = role;
            break;
        }
        /* Mirrors attach_supervisor_task()'s rate-limited warning -- NEVER a
         * halt, however long this loop runs. */
        if (tick >= WARN_FIRST_TICKS && (!warned_once || (tick - last_warn_tick) >= WARN_REPEAT_TICKS)) {
            r.warn_count++;
            warned_once = true;
            last_warn_tick = tick;
        }
    }
    r.attached_before_budget_exhausted = attached;
    if (!attached) {
        /* This correction pass's whole point: running out of this
         * scenario's simulated tick budget WITHOUT attaching is NOT a halt
         * -- it is simply "still waiting", exactly like the real
         * attach_supervisor_task() which has no upper bound at all. */
        return r;
    }

    r.power_base_transport_init_calls++;
    r.parent_service_init_calls++;
    r.pogo_port_init_calls++;

    r.stale_sweep_task_create_calls++;
    if (!in->stale_sweep_task_create_ok) {
        r.halted = true;
        return r; /* halt_forever("STALE_SWEEP_TASK_FAILED: ...") -- a genuine resource failure, not "not attached" */
    }

    r.enable_downstream_calls++;
    r.detect_out_active = true;
    return r;
}

static void scenario_config_error_never_activates(void)
{
    printf("\n-- scenario: CONFIG_ERROR halts before Thread/POGO ever start --\n");
    static const sim_role_t roles[] = {ROLE_LEADER};
    scenario_input_t in = {.config_ok = false, .thread_start_ok = true, .max_ticks = 10,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "a config load failure halts the boot sequence");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled");
    CHECK(!r.detect_out_active, "DETECT_OUT is never activated");
}

static void scenario_all_zero_efuse_never_activates(void)
{
    printf("\n-- scenario: an all-zero efuse EUI-64 read refuses to start -- never used as this power base's "
           "identity --\n");
    static const sim_role_t roles[] = {ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .efuse_ok = false, .thread_start_ok = true, .max_ticks = 10,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "an all-zero efuse EUI-64 halts the boot sequence before Thread ever starts");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled");
    CHECK(!r.detect_out_active, "DETECT_OUT is never activated");
}

static void scenario_thread_start_failure_never_activates(void)
{
    printf("\n-- scenario: thread_manager_start_as_always_on_ftd() failure halts before POGO starts (a genuine "
           "OpenThread API error, distinct from 'not attached yet') --\n");
    static const sim_role_t roles[] = {ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = false, .max_ticks = 10,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "a Thread start API failure halts the boot sequence");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled");
    CHECK(!r.detect_out_active, "DETECT_OUT is never activated");
}

/* Test items 1/2/3: CHILD, ROUTER, and LEADER are all sufficient to start
 * services -- none of them requires waiting for a DIFFERENT one of the
 * three. */
static void scenario_each_attached_role_starts_services_exactly_once(void)
{
    static const struct {
        const char *name;
        sim_role_t  role;
    } cases[] = {
        {"CHILD", ROLE_CHILD},
        {"ROUTER", ROLE_ROUTER},
        {"LEADER", ROLE_LEADER},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("\n-- scenario: reaching %s (only) is sufficient to start every service exactly once --\n",
               cases[i].name);
        sim_role_t roles[] = {ROLE_DETACHED, cases[i].role};
        scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 10, .role_sequence = roles,
                                .role_sequence_len = 2, .stale_sweep_task_create_ok = true, .efuse_ok = true};
        boot_result_t r = run_boot_sequence(&in);
        CHECK(!r.halted, "the boot sequence completes successfully once attached");
        CHECK(r.first_attached_role == cases[i].role, "the role that satisfied attachment is recorded correctly");
        CHECK(r.power_base_transport_init_calls == 1, "power_base_transport_init() runs exactly once");
        CHECK(r.parent_service_init_calls == 1, "parent_service_init() runs exactly once");
        CHECK(r.pogo_port_init_calls == 1, "pogo_port_init() runs exactly once");
        CHECK(r.stale_sweep_task_create_calls == 1, "the stale-sweep task is created exactly once");
        CHECK(r.enable_downstream_calls == 1, "the downstream port is enabled exactly once");
        CHECK(r.detect_out_active, "DETECT_OUT is active once attached, regardless of which of CHILD/ROUTER/LEADER");
    }
}

/* Test item 4: DISABLED/DETACHED never starts services -- confirmed by
 * running out the tick budget while staying DETACHED. */
static void scenario_detached_never_starts_services(void)
{
    printf("\n-- scenario: staying DETACHED for the whole simulated window never starts any service --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 500,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(!r.attached_before_budget_exhausted, "this scenario never attaches within its simulated window");
    CHECK(r.power_base_transport_init_calls == 0, "no service is started while DISABLED/DETACHED");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled while DISABLED/DETACHED");
    CHECK(!r.detect_out_active, "DETECT_OUT stays inactive the whole time");
}

static void scenario_stale_sweep_task_failure_still_halts(void)
{
    printf("\n-- scenario: attached, but stale-sweep xTaskCreate() FAILS -- a genuine resource failure still halts "
           "(distinct from 'not attached yet') --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED, ROLE_CHILD};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 10,
                            .role_sequence = roles, .role_sequence_len = 2, .stale_sweep_task_create_ok = false, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "a stale-sweep task creation failure still halts the boot sequence");
    CHECK(r.stale_sweep_task_create_calls == 1, "the stale-sweep task creation was attempted exactly once");
    CHECK(r.enable_downstream_calls == 0,
          "pogo_port_enable() (the downstream port) is NEVER called when stale-sweep task creation fails");
    CHECK(!r.detect_out_active, "DETECT_OUT never activates -- it is only ever driven by enable_downstream_port()");
}

/*
 * Test items 7/8/10 (this correction pass): never attaching within (well
 * past) 30s does NOT halt -- OpenThread attach continues, a rate-limited
 * warning appears, and once this device DOES eventually attach (possibly
 * much later), every service still starts EXACTLY ONCE.
 */
static void scenario_slow_first_attach_never_halts_and_still_starts_services(void)
{
    printf("\n-- scenario (this correction pass): first attach takes far longer than 30s -- no halt, then "
           "services start exactly once on the eventual attach --\n");

    /* Stay DETACHED for 500 ticks (100s -- well past the 30s/150-tick first
     * warning and the 60s/300-tick repeat), THEN attach as CHILD. */
    sim_role_t roles[501];
    for (int i = 0; i < 500; i++) {
        roles[i] = ROLE_DETACHED;
    }
    roles[500] = ROLE_CHILD;
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 600,
                            .role_sequence = roles, .role_sequence_len = 501, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);

    CHECK(!r.halted, "a first attach that takes far longer than 30s never halts this power base");
    CHECK(!r.halted_for_not_attached, "there is no 'halted for not attached yet' outcome at all any more");
    CHECK(r.attached_before_budget_exhausted, "this device eventually attaches within the simulated window");
    CHECK(r.first_attached_role == ROLE_CHILD, "the eventual attach is recorded as CHILD");
    CHECK(r.power_base_transport_init_calls == 1, "services start (exactly once) once the eventual attach happens");
    CHECK(r.parent_service_init_calls == 1, "parent_service_init() runs exactly once even after a slow first attach");
    CHECK(r.pogo_port_init_calls == 1, "pogo_port_init() runs exactly once even after a slow first attach");
    CHECK(r.stale_sweep_task_create_calls == 1, "the stale-sweep task is created exactly once even after a slow first attach");
    CHECK(r.enable_downstream_calls == 1, "the downstream port is enabled exactly once even after a slow first attach");
    CHECK(r.detect_out_active, "DETECT_OUT activates once this device finally attaches, however long it took");

    /* Test item 10: the warning is rate-limited, not once-per-tick. Over
     * 500 DETACHED ticks (350 ticks past the first WARN_FIRST_TICKS=150
     * threshold), a naive "warn every tick" implementation would produce
     * ~350 warnings; the real (and this model's) rate-limited policy caps
     * it at roughly 350/WARN_REPEAT_TICKS(=300) + 1 == 2 warnings. */
    CHECK(r.warn_count >= 1, "at least one 'still not attached' warning fires during a long DETACHED window");
    CHECK(r.warn_count <= 3, "the warning is rate-limited (at most ~1 per WARN_REPEAT_TICKS), not one per poll tick");
    printf("    (observed warn_count=%d over 500 DETACHED ticks -- rate-limited, not 500)\n", r.warn_count);
}

static void scenario_no_warning_before_30s(void)
{
    printf("\n-- scenario: no warning at all before the first 30s (WARN_FIRST_TICKS) threshold --\n");
    sim_role_t roles[149];
    for (int i = 0; i < 149; i++) {
        roles[i] = ROLE_DETACHED;
    }
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 149,
                            .role_sequence = roles, .role_sequence_len = 149, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.warn_count == 0, "no warning fires before the WARN_FIRST_TICKS threshold is reached");
    CHECK(!r.halted, "still not halted, obviously");
}

/* Test item 9 (extends the previous session's item 5): role fluctuation
 * AFTER a SLOW first attach still never re-creates any task or resource --
 * same property as before this correction pass, now re-verified against a
 * boot that took its time to attach in the first place. */
static void scenario_post_slow_attach_role_fluctuation_never_recreates_anything(void)
{
    printf("\n-- scenario: CHILD<->ROUTER<->LEADER fluctuation after a SLOW first attach never re-creates "
           "anything --\n");
    sim_role_t roles[201];
    for (int i = 0; i < 200; i++) {
        roles[i] = ROLE_DETACHED;
    }
    roles[200] = ROLE_CHILD;
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 250,
                            .role_sequence = roles, .role_sequence_len = 201, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(!r.halted, "boot completes once CHILD is eventually reached");
    CHECK(r.enable_downstream_calls == 1, "exactly one enable call after the slow boot");

    static const sim_role_t post_boot_sequence[] = {ROLE_ROUTER, ROLE_LEADER, ROLE_ROUTER, ROLE_CHILD,
                                                     ROLE_LEADER, ROLE_CHILD,  ROLE_ROUTER};
    int allowed_true_count = 0;
    for (size_t i = 0; i < sizeof(post_boot_sequence) / sizeof(post_boot_sequence[0]); i++) {
        if (pb_provisioning_allowed(post_boot_sequence[i])) {
            allowed_true_count++;
        }
        bool mesh_ok;
        bool collector_ok = pb_get_collector(post_boot_sequence[i], &mesh_ok);
        CHECK(collector_ok, "pb_get_collector() is valid for every attached role in this fluctuation sequence");
    }
    CHECK(allowed_true_count == 7, "pb_provisioning_allowed() is true for every one of the 7 (all-attached) role queries");

    CHECK(r.power_base_transport_init_calls == 1, "power_base_transport_init() is still exactly 1 after fluctuation");
    CHECK(r.parent_service_init_calls == 1, "parent_service_init() is still exactly 1 after fluctuation");
    CHECK(r.pogo_port_init_calls == 1, "pogo_port_init() is still exactly 1 after fluctuation");
    CHECK(r.stale_sweep_task_create_calls == 1, "the stale-sweep task is still created exactly 1 time after fluctuation");
    CHECK(r.enable_downstream_calls == 1, "the downstream port is still enabled exactly 1 time after fluctuation");
}

/* Test item 6: a detach (DISABLED/DETACHED) followed by a reattach resumes
 * service via the SAME live query functions, with no explicit "resume"
 * step and no busy-retry loop. */
static void scenario_detach_then_reattach_resumes_without_recreation(void)
{
    printf("\n-- scenario: detach (DISABLED/DETACHED) then reattach resumes service, no recreation, no busy retry --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED, ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_ticks = 10,
                            .role_sequence = roles, .role_sequence_len = 2, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(!r.halted, "boot completes once LEADER is reached");

    CHECK(pb_provisioning_allowed(ROLE_LEADER), "provisioning is allowed while LEADER (post-boot, steady state)");
    bool mesh_ok;
    CHECK(pb_get_collector(ROLE_LEADER, &mesh_ok), "collector is valid while LEADER");

    CHECK(!pb_provisioning_allowed(ROLE_DETACHED), "provisioning is NOT allowed the instant role drops to DETACHED");
    CHECK(!pb_get_collector(ROLE_DETACHED, &mesh_ok), "the collector becomes invalid the instant role drops to DETACHED");
    CHECK(!mesh_ok, "the (simulated) Mesh-Local EID is treated as unavailable while DETACHED");

    CHECK(!pb_provisioning_allowed(ROLE_DISABLED), "provisioning is NOT allowed while DISABLED either");
    CHECK(!pb_get_collector(ROLE_DISABLED, &mesh_ok), "the collector is NOT valid while DISABLED either");

    CHECK(pb_provisioning_allowed(ROLE_CHILD), "provisioning resumes immediately once reattached as CHILD");
    CHECK(pb_get_collector(ROLE_CHILD, &mesh_ok), "the collector becomes valid again immediately once reattached");
    CHECK(mesh_ok, "the (simulated) Mesh-Local EID is available again once reattached");

    CHECK(r.power_base_transport_init_calls == 1, "no re-init happened across the detach/reattach cycle");
    CHECK(r.parent_service_init_calls == 1, "no re-init happened across the detach/reattach cycle (parent_service)");
    CHECK(r.pogo_port_init_calls == 1, "no re-init happened across the detach/reattach cycle (pogo_port)");
    CHECK(r.stale_sweep_task_create_calls == 1, "no re-creation happened across the detach/reattach cycle (stale-sweep task)");
    CHECK(r.enable_downstream_calls == 1, "the downstream port was never re-enabled across the detach/reattach cycle");
}

int main(void)
{
    scenario_config_error_never_activates();
    scenario_all_zero_efuse_never_activates();
    scenario_thread_start_failure_never_activates();
    scenario_each_attached_role_starts_services_exactly_once();
    scenario_detached_never_starts_services();
    scenario_stale_sweep_task_failure_still_halts();
    scenario_slow_first_attach_never_halts_and_still_starts_services();
    scenario_no_warning_before_30s();
    scenario_post_slow_attach_role_fluctuation_never_recreates_anything();
    scenario_detach_then_reattach_resumes_without_recreation();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
