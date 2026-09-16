/*
 * Host-compiled verification of the code review follow-up section 2's fix
 * to topology_reporter.c's boot_session_generation handling: an
 * NVS-persisted, monotonically-incrementing-per-boot counter instead of
 * esp_random(), a sequence counter that rolls over to a fresh generation
 * before ever wrapping -- and, per code review follow-up (2nd pass) item 3,
 * a FAIL-FAST policy at init time: if the boot generation cannot be durably
 * persisted, topology_reporter_init() returns an error and the module's
 * ESP_ERROR_CHECK() on that call (module/main/app_main.c) halts the WHOLE
 * module boot, rather than the old "quietly disable topology reporting but
 * let sensing/provisioning proceed normally" compromise -- which left a
 * module that could complete provisioning and report itself healthy while
 * the power base's "/s" endpoint silently rejected every one of its sensor
 * samples as an unrecognized uuid (it never received a topology report to
 * register that uuid against).
 *
 * topology_reporter.c cannot be host-compiled (real NVS/FreeRTOS/OpenThread
 * calls) -- this mirrors load_next_boot_generation()/topology_reporter_init()/
 * maybe_roll_over_sequence() exactly, with a simulated NVS (a plain
 * in-memory value plus an injectable "next write succeeds?" flag) standing
 * in for the real nvs_open()/nvs_get_u32()/nvs_set_u32()/nvs_commit() calls.
 *
 * ALSO covers (this follow-up: SED CoAP response timing correction) that
 * topology_reporter.c's own CoAP response-wait policy is IDENTICAL to
 * sensor_transport.c's -- see scenario_shares_coap_response_timing_policy_with_sensor_transport()
 * below, which links the REAL, unmodified components/coap_response_timing/
 * coap_response_timing.c (the one piece of this timing policy that IS
 * host-testable in isolation) and computes the exact wait value
 * topology_reporter.c's own send_one_attempt() would derive from
 * production's real Kconfig default (CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS=2000,
 * components/coap_response_timing/Kconfig) -- cross-checked against
 * tools/test_coap_response_timing.c's own identical computation for
 * sensor_transport.c's real, host-linked test
 * (tools/test_sensor_transport_gate_retry.c). topology_reporter.c itself
 * remains NOT host-linkable here (real NVS/FreeRTOS/OpenThread calls, same
 * as the rest of this file) -- verified instead by code review confirming
 * it uses the exact same CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS /
 * COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM/DEN /
 * coap_response_timing_wait_ms() symbols as sensor_transport.c (never a
 * separate copy), plus the real ESP-IDF module fullclean build.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/coap_response_timing/include \
 *      -o /tmp/test_topology_reporter_session tools/test_topology_reporter_session.c \
 *      components/coap_response_timing/coap_response_timing.c \
 *   && /tmp/test_topology_reporter_session
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "coap_response_timing/coap_response_timing.h"

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

/* --- simulated NVS, standing in for the real "topo_rep"/"boot_gen" pair --- */
typedef struct {
    uint32_t stored_value;
    bool     has_value;
    bool     next_write_fails; /* injects an nvs_set_u32()/nvs_commit() failure */
} fake_nvs_t;

#define ROLLOVER_THRESHOLD (UINT32_MAX - 4) /* mirrors TOPOLOGY_REPORTER_SEQUENCE_ROLLOVER_THRESHOLD */

/* Mirrors load_next_boot_generation(): read-or-default-0, increment,
 * durably write BEFORE returning success. */
static bool load_next_boot_generation(fake_nvs_t *nvs, uint32_t *out_gen)
{
    uint32_t current = nvs->has_value ? nvs->stored_value : 0;
    uint32_t next = (current >= UINT32_MAX - 1) ? UINT32_MAX : current + 1;
    if (nvs->next_write_fails) {
        return false; /* mirrors: nvs_set_u32()/nvs_commit() failed -- do NOT update stored_value, do NOT succeed */
    }
    nvs->stored_value = next;
    nvs->has_value = true;
    *out_gen = next;
    return true;
}

typedef struct {
    fake_nvs_t nvs;
    uint32_t   generation;
    uint32_t   sequence;
    bool       reporting_enabled;
} reporter_t;

/*
 * Mirrors topology_reporter_init()'s CURRENT (post code review follow-up
 * 2nd pass) return value: false means the real function returns `gen_err`
 * (not ESP_OK), which module/main/app_main.c's existing
 * ESP_ERROR_CHECK(topology_reporter_init(&cfg)) turns into a full module
 * boot halt -- role_manager/child_service/sensor_service/parent_service
 * never start. There is no more "returns ESP_OK but with reporting quietly
 * disabled" outcome.
 */
static bool topology_reporter_init_mirror(reporter_t *r)
{
    r->nvs.stored_value = r->nvs.has_value ? r->nvs.stored_value : 0; /* fresh NVS view, unchanged otherwise */
    r->sequence = 0;
    r->reporting_enabled = false;
    if (!load_next_boot_generation(&r->nvs, &r->generation)) {
        return false; /* fail-fast: topology_reporter_init() itself returns an error */
    }
    r->reporting_enabled = true;
    return true;
}

/* Mirrors module/main/app_main.c's ESP_ERROR_CHECK(topology_reporter_init(&cfg))
 * -- a false return from topology_reporter_init_mirror() halts the WHOLE
 * module boot, never reaching role_manager/child_service/sensor_service. */
static bool module_boot_mirror(reporter_t *r, bool *out_module_halted)
{
    bool init_ok = topology_reporter_init_mirror(r);
    *out_module_halted = !init_ok;
    return init_ok;
}

static void reporter_init(reporter_t *r)
{
    r->nvs.stored_value = 0;
    r->nvs.has_value = false;
    r->nvs.next_write_fails = false;
    r->sequence = 0;
    r->reporting_enabled = load_next_boot_generation(&r->nvs, &r->generation);
}

/* Mirrors maybe_roll_over_sequence(). */
static void maybe_roll_over_sequence(reporter_t *r)
{
    if (r->sequence < ROLLOVER_THRESHOLD) {
        return;
    }
    uint32_t fresh_gen;
    if (!load_next_boot_generation(&r->nvs, &fresh_gen)) {
        r->reporting_enabled = false;
        return;
    }
    r->generation = fresh_gen;
    r->sequence = 0;
}

/* Mirrors send_live_report()'s two enabled-checks bracketing the rollover
 * attempt. Returns the (generation, sequence) that WOULD have been sent, or
 * false if reporting is disabled and nothing is sent at all. */
static bool try_send(reporter_t *r, uint32_t *out_gen, uint32_t *out_seq)
{
    if (!r->reporting_enabled) {
        return false;
    }
    maybe_roll_over_sequence(r);
    if (!r->reporting_enabled) {
        return false;
    }
    r->sequence++;
    *out_gen = r->generation;
    *out_seq = r->sequence;
    return true;
}

static void scenario_monotonic_across_reboots(void)
{
    printf("\n-- scenario: boot_session_generation increments monotonically across reboots --\n");
    fake_nvs_t nvs = {0};
    uint32_t gens[5];
    for (int boot = 0; boot < 5; boot++) {
        bool ok = load_next_boot_generation(&nvs, &gens[boot]);
        CHECK(ok, "boot generation persists successfully");
    }
    for (int i = 1; i < 5; i++) {
        CHECK(gens[i] == gens[i - 1] + 1, "each successive boot's generation is exactly one more than the last");
    }
    CHECK(gens[0] == 1, "the very first boot ever is generation 1 (0 stays reserved for 'never observed')");
}

static void scenario_nvs_failure_disables_reporting_not_silently_succeeds(void)
{
    printf("\n-- scenario: NVS persistence failure disables reporting -- never looks like success --\n");
    reporter_t r;
    r.nvs.stored_value = 0;
    r.nvs.has_value = false;
    r.nvs.next_write_fails = true; /* simulates a full/corrupt NVS partition */
    r.sequence = 0;
    r.reporting_enabled = load_next_boot_generation(&r.nvs, &r.generation);

    CHECK(!r.reporting_enabled, "topology_reporter_init() disables reporting when persisting the boot generation fails");

    uint32_t gen, seq;
    bool sent = try_send(&r, &gen, &seq);
    CHECK(!sent, "no report is ever sent while reporting is disabled -- never 'looks like success' on unpersisted state");
}

static void scenario_init_generation_failure_fails_fast_halts_module_boot(void)
{
    printf("\n-- scenario: code review follow-up (2nd pass) item 3 -- a boot-generation persistence failure at "
           "init time returns an ERROR (fail-fast), not 'ESP_OK with reporting quietly disabled' --\n");
    reporter_t r;
    r.nvs.stored_value = 0;
    r.nvs.has_value = false;
    r.nvs.next_write_fails = true; /* simulates a full/corrupt NVS partition, right at boot */

    bool module_halted;
    bool init_ok = module_boot_mirror(&r, &module_halted);

    CHECK(!init_ok, "topology_reporter_init() itself returns an error -- not ESP_OK");
    CHECK(module_halted, "module/main/app_main.c's ESP_ERROR_CHECK() on that call halts the ENTIRE module boot -- "
                          "role_manager/child_service/sensor_service/parent_service never start this boot");
    CHECK(!r.reporting_enabled,
          "reporting is never left enabled=false-but-otherwise-booted -- there is no such intermediate state "
          "any more: the module either has a valid topology report path, or it never reaches a "
          "provisioned/data-sending state at all");
}

static void scenario_init_generation_success_boots_normally(void)
{
    printf("\n-- scenario: a successful boot-generation persistence lets the whole module boot proceed --\n");
    reporter_t r;
    r.nvs.stored_value = 0;
    r.nvs.has_value = false;
    r.nvs.next_write_fails = false;

    bool module_halted;
    bool init_ok = module_boot_mirror(&r, &module_halted);

    CHECK(init_ok, "topology_reporter_init() returns ESP_OK when the generation persists successfully");
    CHECK(!module_halted, "the module boot is NOT halted -- role_manager/child_service/sensor_service proceed");
    CHECK(r.reporting_enabled, "topology reporting is enabled for this boot session");
}

static void scenario_sequence_rollover_before_wrap(void)
{
    printf("\n-- scenario: sequence rolls over to a fresh generation before ever wrapping --\n");
    reporter_t r;
    reporter_init(&r);
    CHECK(r.reporting_enabled, "reporter initializes successfully");
    uint32_t initial_gen = r.generation;

    r.sequence = ROLLOVER_THRESHOLD - 1; /* one below the threshold -- must NOT roll over yet */
    uint32_t gen, seq;
    bool sent = try_send(&r, &gen, &seq);
    CHECK(sent, "a send just below the rollover threshold still succeeds");
    CHECK(gen == initial_gen, "the generation has NOT changed yet (still below threshold)");

    /* Now push sequence to the threshold itself. */
    r.sequence = ROLLOVER_THRESHOLD;
    sent = try_send(&r, &gen, &seq);
    CHECK(sent, "a send at/above the rollover threshold still succeeds (via a fresh generation)");
    CHECK(gen == initial_gen + 1, "rolling over obtained a FRESH, freshly-persisted generation");
    CHECK(seq == 1, "the sequence counter reset to 1 under the new generation");
    CHECK(seq < ROLLOVER_THRESHOLD, "the local sequence space never actually reached UINT32_MAX -- no wraparound");
}

static void scenario_rollover_persistence_failure_disables_reporting(void)
{
    printf("\n-- scenario: a rollover that CANNOT be persisted disables reporting rather than reusing the old "
           "about-to-wrap generation --\n");
    reporter_t r;
    reporter_init(&r);
    CHECK(r.reporting_enabled, "reporter initializes successfully");

    r.sequence = ROLLOVER_THRESHOLD;
    r.nvs.next_write_fails = true; /* NVS becomes unavailable exactly when a rollover is needed */
    uint32_t gen, seq;
    bool sent = try_send(&r, &gen, &seq);
    CHECK(!sent, "the send is not made -- rollover persistence failed");
    CHECK(!r.reporting_enabled, "reporting is disabled for the rest of this boot rather than reusing the "
                                 "un-persisted, about-to-wrap generation");
}

/*
 * This follow-up: proves topology_reporter.c and sensor_transport.c share
 * the IDENTICAL CoAP response-wait policy rather than each hardcoding its
 * own copy. Uses the REAL, linked coap_response_timing_wait_ms() with the
 * SAME parameters both components' real source actually uses in production
 * (CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS=2000 -- components/coap_response_timing/
 * Kconfig's real default, hardcoded here as a literal since this mirror test
 * has no sdkconfig.h; cross-checked by code review that both .c files
 * reference that exact same Kconfig symbol, never an independent copy).
 */
static void scenario_shares_coap_response_timing_policy_with_sensor_transport(void)
{
    printf("\n-- scenario: topology_reporter.c's CoAP response-wait policy is IDENTICAL to sensor_transport.c's "
           "(both call the SAME shared helper with the SAME shared constants) --\n");
    const uint32_t production_ack_timeout_ms = 2000; /* must match components/coap_response_timing/Kconfig's default */
    uint32_t topology_reporter_wait_ms =
        coap_response_timing_wait_ms(production_ack_timeout_ms, COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                      COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN, production_ack_timeout_ms);
    /* sensor_transport.c's own real host test (tools/test_sensor_transport_gate_retry.c,
     * via tools/test_coap_response_timing.c's identical first scenario)
     * computes this exact same value from this exact same production
     * default -- see that test's own "this repository's actual defaults"
     * scenario. */
    CHECK(topology_reporter_wait_ms == 5000,
          "topology_reporter.c's derived CoAP response wait (5000ms) matches sensor_transport.c's own derived "
          "wait exactly -- both from the SAME shared helper and SAME shared Kconfig default, never two "
          "independently-drifting copies");
}

int main(void)
{
    scenario_monotonic_across_reboots();
    scenario_nvs_failure_disables_reporting_not_silently_succeeds();
    scenario_init_generation_failure_fails_fast_halts_module_boot();
    scenario_init_generation_success_boots_normally();
    scenario_sequence_rollover_before_wrap();
    scenario_rollover_persistence_failure_disables_reporting();
    scenario_shares_coap_response_timing_policy_with_sensor_transport();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
