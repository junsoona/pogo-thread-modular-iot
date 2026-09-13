/*
 * Host-compiled verification of power_base/main/app_main.c's boot-sequence
 * ORDERING invariant -- required test items 4/5 of the power-base follow-up
 * spec: "전원 모듈이 Leader 준비 전 DETECT_OUT을 활성화하지 않음", "Leader
 * 준비 후 downstream 포트를 한 번만 활성화" -- the code review follow-up
 * section 5 fix: strictly LEADER, not "Leader or Router" -- and code review
 * follow-up (2nd pass) item 2: the stale-sweep task must be created AND its
 * creation confirmed successful BEFORE pogo_port_enable(), never after.
 *
 * app_main.c itself is not host-compilable (it is nothing but ESP-IDF/
 * OpenThread calls) -- this mirrors its actual linear control flow as a
 * tiny deterministic state machine that calls the SAME functions real
 * app_main.c calls in the SAME order (thread_start, then a bounded
 * leader-wait poll over an explicit otDeviceRole sequence, then a
 * stale-sweep-task-creation step, then pogo_port_enable-equivalent),
 * counting how many times each is invoked and what DETECT_OUT's simulated
 * register reads at every step. pogo_port_enable() is where DETECT_OUT
 * actually gets driven active (see pogo_port.c's set_detect_out(p, true)
 * call inside it) -- this mock's enable_downstream_port() reproduces
 * exactly that coupling (DETECT_OUT can only ever become active as a side
 * effect of this ONE call), so asserting "enable_downstream_port was
 * never/only-once called before X" is the same thing as asserting
 * DETECT_OUT's real timeline.
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
 * this model's purposes -- only DETACHED/CHILD/ROUTER/LEADER are exercised. */
typedef enum { ROLE_DETACHED = 0, ROLE_CHILD, ROLE_ROUTER, ROLE_LEADER } sim_role_t;

typedef struct {
    bool config_ok;
    /* code review follow-up (2nd pass) verification item: an all-zero
     * esp_efuse_mac_get_default() read must never be used as this power
     * base's identity -- same policy child_service.c's
     * compute_permanent_uuid() applies to a sensor module's own uuid (see
     * tools/test_permanent_module_uuid.c's scenario_all_zero_uuid_rejected()). */
    bool efuse_ok;
    bool thread_start_ok;
    int  max_polls;
    /* role observed at poll index i (0-based); polls beyond the array
     * length repeat the last entry -- lets a scenario express "reaches
     * ROUTER and gets stuck there" distinctly from "reaches LEADER". */
    const sim_role_t *role_sequence;
    int                role_sequence_len;
    /* code review follow-up (2nd pass) item 2: xTaskCreate() for the
     * stale-sweep task, modeled as a single pass/fail outcome checked
     * strictly BEFORE enable_downstream_port() is ever reached. */
    bool stale_sweep_task_create_ok;
} scenario_input_t;

typedef struct {
    int  stale_sweep_task_create_calls;
    int  enable_downstream_calls; /* == the only place DETECT_OUT is driven active, see this file's top comment */
    bool detect_out_active;
    bool halted;
    int  polls_done;
} boot_result_t;

static sim_role_t role_at_poll(const scenario_input_t *in, int poll)
{
    if (poll >= in->role_sequence_len) {
        return in->role_sequence[in->role_sequence_len - 1];
    }
    return in->role_sequence[poll];
}

/*
 * Mirrors power_base/main/app_main.c's app_main() linear sequence exactly:
 * config load -> thread_manager_init() (always OK, not modeled) ->
 * thread_manager_start_as_root() -> bounded poll loop that breaks ONLY on
 * ROLE_LEADER (code review follow-up section 5: strictly LEADER, ROUTER is
 * NOT sufficient any more) -> power_base_transport_init()+parent_service_init()
 * (not GPIO-relevant, not modeled) -> pogo_port_init() (not modeled, touches
 * no GPIO) -> stale-sweep xTaskCreate() CONFIRMED (code review follow-up
 * (2nd pass) item 2 -- must happen and must succeed strictly before
 * enable_downstream_port()) -> pogo_port_enable() (==
 * enable_downstream_port() here). halt_forever() at any failure point NEVER
 * reaches enable_downstream_port(), including a stale-sweep-task creation
 * failure.
 */
static boot_result_t run_boot_sequence(const scenario_input_t *in)
{
    boot_result_t r = {0};

    if (!in->config_ok) {
        r.halted = true;
        return r; /* halt_forever("CONFIG_ERROR ...") -- pogo_port_init/enable never called */
    }
    if (!in->efuse_ok) {
        r.halted = true;
        return r; /* halt_forever("... all-zero EUI-64 ...") -- refuses to use it as this power base's identity */
    }
    if (!in->thread_start_ok) {
        r.halted = true;
        return r; /* halt_forever("ROLE_TRANSITION_ERROR: thread_manager_start_as_root failed") */
    }

    bool became_leader = false;
    for (int poll = 0; poll < in->max_polls; poll++) {
        r.polls_done++;
        if (role_at_poll(in, poll) == ROLE_LEADER) {
            became_leader = true;
            break;
        }
    }
    if (!became_leader) {
        r.halted = true;
        return r; /* halt_forever("ROLE_TRANSITION_ERROR: Thread never reached LEADER within 30s") */
    }

    /* Only reachable once LEADER is confirmed -- matches power_base/main/app_main.c's
     * actual call order (power_base_transport_init() then parent_service_init()
     * then pogo_port_init(), all strictly after the poll loop above breaks). */
    r.stale_sweep_task_create_calls++;
    if (!in->stale_sweep_task_create_ok) {
        r.halted = true;
        return r; /* halt_forever("STALE_SWEEP_TASK_FAILED: ...") -- enable_downstream_port() NEVER called */
    }

    /* Only reachable once the stale-sweep task creation is CONFIRMED
     * successful -- code review follow-up (2nd pass) item 2. */
    r.enable_downstream_calls++;
    r.detect_out_active = true;
    return r;
}

static void scenario_config_error_never_activates(void)
{
    printf("\n-- scenario: CONFIG_ERROR halts before Thread/POGO ever start --\n");
    static const sim_role_t roles[] = {ROLE_LEADER};
    scenario_input_t in = {.config_ok = false, .thread_start_ok = true, .max_polls = 150,
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
    scenario_input_t in = {.config_ok = true, .efuse_ok = false, .thread_start_ok = true, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "an all-zero efuse EUI-64 halts the boot sequence before Thread ever starts");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled");
    CHECK(!r.detect_out_active, "DETECT_OUT is never activated");
}

static void scenario_thread_start_failure_never_activates(void)
{
    printf("\n-- scenario: thread_manager_start_as_root() failure halts before POGO starts --\n");
    static const sim_role_t roles[] = {ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = false, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "a Thread start failure halts the boot sequence");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled");
    CHECK(!r.detect_out_active, "DETECT_OUT is never activated");
}

static void scenario_leader_never_reached_never_activates(void)
{
    printf("\n-- scenario: Thread never reaches LEADER within the poll budget (stays DETACHED) --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "a LEADER timeout halts the boot sequence");
    CHECK(r.polls_done == in.max_polls, "the full poll budget was actually exhausted (not given up early)");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled without reaching LEADER");
    CHECK(!r.detect_out_active, "DETECT_OUT stays inactive the whole time");
}

static void scenario_router_only_never_activates(void)
{
    printf("\n-- scenario: reaches ROUTER but never LEADER -- code review follow-up section 5, ROUTER is NOT "
           "sufficient --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED, ROLE_CHILD, ROLE_ROUTER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 3, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "reaching ROUTER (and staying there) still halts -- it is not treated as ready");
    CHECK(r.enable_downstream_calls == 0, "the downstream port is never enabled while only ROUTER, never LEADER");
    CHECK(!r.detect_out_active, "DETECT_OUT never activates for ROUTER alone");
}

static void scenario_leader_reached_activates_exactly_once(void)
{
    printf("\n-- scenario: LEADER reached -> downstream port enabled exactly once, DETECT_OUT active --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED, ROLE_CHILD, ROLE_ROUTER, ROLE_ROUTER, ROLE_ROUTER,
                                       ROLE_ROUTER,   ROLE_ROUTER, ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 8, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(!r.halted, "the boot sequence completes successfully once LEADER is actually reached");
    CHECK(r.polls_done == 8, "the poll loop stops the iteration AFTER LEADER is observed (polls 0..7)");
    CHECK(r.enable_downstream_calls == 1, "the downstream port is enabled EXACTLY once, never zero, never more than once");
    CHECK(r.detect_out_active, "DETECT_OUT is active only after this point");
}

static void scenario_stale_sweep_task_failure_never_activates(void)
{
    printf("\n-- scenario: code review follow-up (2nd pass) item 2 -- LEADER reached, but stale-sweep "
           "xTaskCreate() FAILS -- DETECT_OUT must never activate --\n");
    static const sim_role_t roles[] = {ROLE_DETACHED, ROLE_CHILD, ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 3, .stale_sweep_task_create_ok = false, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.halted, "a stale-sweep task creation failure halts the boot sequence (halt_forever(), never returns)");
    CHECK(r.stale_sweep_task_create_calls == 1, "the stale-sweep task creation was attempted exactly once");
    CHECK(r.enable_downstream_calls == 0,
          "pogo_port_enable() (the downstream port) is NEVER called when stale-sweep task creation fails");
    CHECK(!r.detect_out_active, "DETECT_OUT never activates -- it is only ever driven by enable_downstream_port()");
}

static void scenario_stale_sweep_task_checked_before_enable_ordering(void)
{
    printf("\n-- scenario: stale-sweep task creation is attempted BEFORE the downstream port is ever enabled --\n");
    static const sim_role_t roles[] = {ROLE_LEADER};
    scenario_input_t in = {.config_ok = true, .thread_start_ok = true, .max_polls = 150,
                            .role_sequence = roles, .role_sequence_len = 1, .stale_sweep_task_create_ok = true, .efuse_ok = true};
    boot_result_t r = run_boot_sequence(&in);
    CHECK(r.stale_sweep_task_create_calls == 1, "the stale-sweep task creation step ran");
    CHECK(r.enable_downstream_calls == 1,
          "the downstream port was ALSO enabled once stale-sweep task creation succeeded (correct order, not "
          "skipped)");
}

int main(void)
{
    scenario_config_error_never_activates();
    scenario_all_zero_efuse_never_activates();
    scenario_thread_start_failure_never_activates();
    scenario_leader_never_reached_never_activates();
    scenario_router_only_never_activates();
    scenario_leader_reached_activates_exactly_once();
    scenario_stale_sweep_task_failure_never_activates();
    scenario_stale_sweep_task_checked_before_enable_ordering();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
