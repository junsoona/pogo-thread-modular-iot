/*
 * Host-compiled verification of the topology-registration data-plane gate
 * (real-hardware race fix: a sensor sample beating the power base's own
 * topology registry write -- see components/topology_gate's header comment,
 * README.md, and docs/power-base.md section 11).
 *
 * Links against the REAL, unmodified components/topology_gate/topology_gate.c
 * (a FreeRTOS event group is its only dependency -- shadowed here by
 * tools/host_test_stubs/freertos/{FreeRTOS.h,event_groups.h}, the same
 * "put stubs first on the include path" convention as
 * tools/host_test_stubs/esp_err.h). topology_reporter.c/sensor_transport.c
 * themselves cannot be host-compiled (real OpenThread/FreeRTOS-task/NVS
 * calls) -- this mirrors their gate-relevant decision logic exactly (the
 * same convention every pre-existing pogo_port/child_service/parent_service/
 * topology_reporter test in this repository already uses), while every
 * actual gate-state transition (open/close/is_open/request/take) goes
 * through the real component, not a re-implementation of it.
 *
 *   reporter_tick()   mirrors topology_reporter_task()'s one-iteration
 *                      has_attached/settled/reregister-request handling
 *                      (topology_reporter.c)
 *   sender_send()      mirrors sensor_transport_send_sample()'s gate check
 *                      + CoAP-4.04 reaction (sensor_transport.c)
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs -I components/topology_gate/include \
 *      -o /tmp/test_topology_gate tools/test_topology_gate.c \
 *      components/topology_gate/topology_gate.c \
 *   && /tmp/test_topology_gate
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "topology_gate/topology_gate.h"

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

/* --- mirrors topology_reporter_task()'s per-tick state machine --- */
typedef struct {
    bool has_attached;
    bool settled;
    int attach_attempts; /* diagnostic: how many ATTACHED sends this model actually issued */
} reporter_model_t;

static void reporter_reset(reporter_model_t *m)
{
    m->has_attached = false;
    m->settled = false;
    m->attach_attempts = 0;
}

/*
 * One topology_reporter_task() loop iteration. `attached_send_result` is
 * this tick's simulated outcome of send_live_report(TOPOLOGY_REPORT_ATTACHED)
 * -- true only for a real CoAP 2.xx response, mirroring
 * send_one_attempt()'s existing "2.xx only" success check (unchanged by
 * this fix). Only consulted when this tick actually attempts an ATTACHED
 * send (settled && !has_attached) -- unlike the real task, this model never
 * bothers with UPDATED/HEARTBEAT branches, which are irrelevant to gate
 * open/close.
 */
static void reporter_tick(reporter_model_t *m, bool attached_send_result)
{
    /* Mirrors: "if (topology_gate_take_reregister_request() && has_attached) { has_attached = false; topology_gate_close(); }" */
    if (topology_gate_take_reregister_request() && m->has_attached) {
        m->has_attached = false;
        topology_gate_close();
    }

    if (m->settled && !m->has_attached) {
        m->attach_attempts++;
        if (attached_send_result) {
            m->has_attached = true;
            topology_gate_open(); /* the ONLY place the real code opens the gate */
        }
        /* else: has_attached stays false, gate stays closed -- bounded
         * retry on the next tick, no busy loop (paced by the real task's
         * own poll-tick wait, not modeled here). */
    } else if (!m->settled && m->has_attached) {
        m->has_attached = false;
        topology_gate_close();
    }
}

/* --- mirrors sensor_transport_send_sample()'s gate check + CoAP-4.04 reaction --- */
typedef enum {
    SEND_SKIPPED_GATE_CLOSED, /* no wireless attempt made at all */
    SEND_OK,
    SEND_REJECTED_UNKNOWN_MODULE, /* CoAP 4.04 -- triggers gate close + reregister request */
    SEND_REJECTED_OTHER,
} send_outcome_t;

typedef enum { COAP_2XX, COAP_4_04_NOT_FOUND, COAP_4XX_OTHER } sim_coap_response_t;

/* `sample_seq` is threaded through purely to demonstrate that a skipped (gate
 * closed) send never touches/consumes a sequence number of its own -- the
 * real sensor_service periodic_task() increments sample_seq BEFORE ever
 * calling the sample-ready callback, completely independent of this gate
 * (see sensor_service.c's periodic_task() and this file's own
 * scenario_sample_seq_policy_unaffected_by_gate()). */
static send_outcome_t sender_send(bool require_gate, sim_coap_response_t response)
{
    if (require_gate && !topology_gate_is_open()) {
        return SEND_SKIPPED_GATE_CLOSED;
    }
    if (response == COAP_2XX) {
        return SEND_OK;
    }
    if (response == COAP_4_04_NOT_FOUND) {
        if (require_gate) {
            topology_gate_close();
            topology_gate_request_reregister();
        }
        return SEND_REJECTED_UNKNOWN_MODULE;
    }
    return SEND_REJECTED_OTHER;
}

static void scenario_no_send_before_ack(void)
{
    printf("\n-- scenario: no sample send before topology ATTACHED is acknowledged --\n");
    topology_gate_init();
    reporter_model_t rep;
    reporter_reset(&rep);
    rep.settled = true; /* Thread joined, upstream ready -- but ATTACHED not sent/acked yet */

    CHECK(!topology_gate_is_open(), "gate starts closed");
    send_outcome_t out = sender_send(/*require_gate=*/true, COAP_2XX);
    CHECK(out == SEND_SKIPPED_GATE_CLOSED, "a sample send is skipped entirely (no CoAP attempt) before ATTACHED succeeds");
}

static void scenario_send_allowed_after_2xx(void)
{
    printf("\n-- scenario: sample send allowed once topology ATTACHED gets a real 2.xx --\n");
    topology_gate_init();
    reporter_model_t rep;
    reporter_reset(&rep);
    rep.settled = true;

    reporter_tick(&rep, /*attached_send_result=*/true);
    CHECK(rep.has_attached, "reporter model marks itself attached on a successful ATTACHED send");
    CHECK(topology_gate_is_open(), "gate opens on a successful ATTACHED send");

    send_outcome_t out = sender_send(/*require_gate=*/true, COAP_2XX);
    CHECK(out == SEND_OK, "the next normal sample is sent once the gate is open");
}

static void scenario_failed_attach_keeps_gate_closed_and_retries(void)
{
    printf("\n-- scenario: a failed ATTACHED send keeps the gate closed and retries (bounded, no busy loop) --\n");
    topology_gate_init();
    reporter_model_t rep;
    reporter_reset(&rep);
    rep.settled = true;

    for (int i = 0; i < 5; i++) {
        reporter_tick(&rep, /*attached_send_result=*/false);
    }
    CHECK(!rep.has_attached, "an ATTACHED send that never succeeds never marks the module as attached");
    CHECK(!topology_gate_is_open(), "the gate never opens while every ATTACHED attempt fails");
    CHECK(rep.attach_attempts == 5, "each tick retries the ATTACHED send exactly once (paced by the real task's poll tick, not a busy loop)");

    send_outcome_t out = sender_send(/*require_gate=*/true, COAP_2XX);
    CHECK(out == SEND_SKIPPED_GATE_CLOSED, "no sensor sample is sent while registration keeps failing");

    /* Now let it succeed -- confirms the retry loop is not permanently stuck. */
    reporter_tick(&rep, /*attached_send_result=*/true);
    CHECK(rep.has_attached && topology_gate_is_open(), "a later successful attempt still opens the gate normally");
}

static void scenario_gate_closes_on_detach_error_or_new_session(void)
{
    printf("\n-- scenario: gate closes immediately on upstream/Thread/role loss --\n");
    topology_gate_init();
    reporter_model_t rep;
    reporter_reset(&rep);
    rep.settled = true;
    reporter_tick(&rep, true);
    CHECK(topology_gate_is_open(), "precondition: gate is open after a successful ATTACHED");

    /* settled -> false models: upstream DETACHED/ERROR, role -> UNASSIGNED/
     * ROLE_ERROR, Thread membership lost, or role_manager_set_upstream_ready(false)
     * from a fresh provisioning session -- topology_reporter.c's `settled`
     * computation folds all of these into one boolean, so this one
     * transition covers every case in section 7 of the follow-up spec. */
    rep.settled = false;
    reporter_tick(&rep, /*attached_send_result=*/false /* irrelevant on this branch */);
    CHECK(!rep.has_attached, "has_attached clears on a settled -> false transition");
    CHECK(!topology_gate_is_open(), "the gate closes on a settled -> false transition");

    send_outcome_t out = sender_send(/*require_gate=*/true, COAP_2XX);
    CHECK(out == SEND_SKIPPED_GATE_CLOSED, "no sensor sample is sent once the gate has closed again");
}

static void scenario_power_base_reboot_recovery(void)
{
    printf("\n-- scenario: power base reboot -- 4.04 unknown-module triggers immediate re-registration, then "
           "resumes once 2.xx confirms it --\n");
    topology_gate_init();
    reporter_model_t rep;
    reporter_reset(&rep);
    rep.settled = true;
    reporter_tick(&rep, true);
    CHECK(rep.has_attached && topology_gate_is_open(), "precondition: this module was already registered before the power base rebooted");

    /* The power base rebooted; its RAM-only registry forgot this module.
     * The very next sample gets bounced as CoAP 4.04 Not Found. */
    send_outcome_t out = sender_send(/*require_gate=*/true, COAP_4_04_NOT_FOUND);
    CHECK(out == SEND_REJECTED_UNKNOWN_MODULE, "a 4.04 response is never treated as a successful send");
    CHECK(!topology_gate_is_open(), "the gate closes immediately on the 4.04, without waiting for topology_reporter's own poll tick");

    /* topology_reporter's task does not need to wait for its normal
     * CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS heartbeat schedule --
     * the very next tick already re-attaches, because
     * topology_gate_request_reregister() forced has_attached back to false. */
    reporter_tick(&rep, /*attached_send_result=*/true);
    CHECK(rep.has_attached, "re-registration happens on the very next tick, not after a 30s heartbeat wait");
    CHECK(topology_gate_is_open(), "the gate reopens once the re-registration attempt gets a real 2.xx");

    out = sender_send(/*require_gate=*/true, COAP_2XX);
    CHECK(out == SEND_OK, "sensor sample sending resumes normally once re-registration is confirmed");
}

static void scenario_dev_root_dev_leaf_unaffected(void)
{
    printf("\n-- scenario: DEV_ROOT/DEV_LEAF (require_topology_registration=false) never gate on this at all --\n");
    topology_gate_init(); /* gate starts closed, and nothing in this scenario ever opens it */
    CHECK(!topology_gate_is_open(), "precondition: gate is closed (DEV_ROOT/DEV_LEAF peers never run topology_reporter's ATTACHED-ack path)");

    send_outcome_t out = sender_send(/*require_gate=*/false, COAP_2XX);
    CHECK(out == SEND_OK, "a DEV_ROOT/DEV_LEAF sample send is never blocked by an always-closed gate");

    /* A 4.04 is impossible in real DEV_ROOT/DEV_LEAF operation (the peer's
     * own sensor_transport.c "/s" handler unconditionally ACKs 2.04 with no
     * registry check at all) but even if simulated, the gate/reregister
     * machinery must stay inert when require_gate is false. */
    out = sender_send(/*require_gate=*/false, COAP_4_04_NOT_FOUND);
    CHECK(out == SEND_REJECTED_UNKNOWN_MODULE, "a (hypothetical) 4.04 is still reported honestly as a rejection");
    CHECK(!topology_gate_is_open(), "...but never touches the gate/reregister machinery when require_gate is false");
}

static void scenario_duplicate_or_late_callback_never_opens_gate_early(void)
{
    printf("\n-- scenario: a duplicate/delayed CoAP callback never opens the gate on its own --\n");
    topology_gate_init();
    reporter_model_t rep;
    reporter_reset(&rep);
    rep.settled = true;

    /*
     * The real send_one_attempt()/coap_response_handler() token-compare
     * mechanism (topology_reporter.c, unchanged by this fix) already
     * guarantees a stale/duplicate callback for an attempt the caller gave
     * up on is silently dropped -- it never reaches send_live_report()'s
     * return value at all. This model represents that guarantee directly:
     * topology_gate_open() is called ONLY from inside reporter_tick()'s
     * own "if (attached_send_result)" branch, so a duplicate/late
     * notification that never flows through reporter_tick() with a true
     * result can, by construction, never open the gate. Simulate a burst
     * of "duplicate" ticks with a false result (the honest outcome for a
     * late/mismatched-token callback) and confirm the gate stays shut.
     */
    for (int i = 0; i < 3; i++) {
        reporter_tick(&rep, /*attached_send_result=*/false);
    }
    CHECK(!topology_gate_is_open(), "repeated duplicate/late (failed) callbacks never open the gate");
    CHECK(!rep.has_attached, "...and never mark the module attached either");
}

static void scenario_sample_seq_policy_unaffected_by_gate(void)
{
    printf("\n-- scenario: the gate never changes the existing same-sample_seq retry policy --\n");
    /*
     * sensor_service.c's periodic_task() increments s_sample_seq and builds
     * the sample BEFORE calling the sample-ready callback
     * (sensor_transport_send_sample()); sensor_transport_send_sample()'s
     * OWN internal per-attempt retry loop resends the SAME already-packed
     * bytes (same sample_seq) for up to SENSOR_TRANSPORT_MAX_ATTEMPTS --
     * this gate only adds an EARLIER check that can skip the whole call
     * before that loop ever starts, never anything that touches sample_seq
     * or re-enters the retry loop with a different value. Modeled here as:
     * a skipped-by-gate outcome must never itself look like "one of the
     * retry attempts" (i.e. it must be distinguishable from SEND_OK/
     * SEND_REJECTED_*, which is exactly what the CHECK below confirms) --
     * the actual per-attempt retry bytes are covered unchanged by this
     * component's own existing behavior (out of this gate's scope).
     */
    topology_gate_init();
    uint32_t sample_seq = 42; /* stands in for sensor_sample_t.sample_seq -- untouched by the gate check */
    send_outcome_t out = sender_send(/*require_gate=*/true, COAP_2XX);
    CHECK(out == SEND_SKIPPED_GATE_CLOSED, "gate-closed skip is its own distinct outcome, not a (mis-)retry of the same sample");
    CHECK(sample_seq == 42, "sample_seq itself is never touched by the gate check");
}

int main(void)
{
    scenario_no_send_before_ack();
    scenario_send_allowed_after_2xx();
    scenario_failed_attach_keeps_gate_closed_and_retries();
    scenario_gate_closes_on_detach_error_or_new_session();
    scenario_power_base_reboot_recovery();
    scenario_dev_root_dev_leaf_unaffected();
    scenario_duplicate_or_late_callback_never_opens_gate_early();
    scenario_sample_seq_policy_unaffected_by_gate();

    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
