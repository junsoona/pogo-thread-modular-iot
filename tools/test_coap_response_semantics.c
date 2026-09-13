/*
 * Host-compiled verification of the code review follow-up section 3 (CoAP
 * response code semantics + client-side response-code checking + safe
 * async callback lifetime) and section 4 (sensor data <-> topology
 * validation linkage).
 *
 * power_base_transport.c/topology_reporter.c/sensor_transport.c cannot be
 * host-compiled (real OpenThread/FreeRTOS calls) -- this mirrors their
 * decision logic exactly:
 *   - topology_result_to_coap_code() (power_base_transport.c)
 *   - the "/s" sensor handler's known/online/bitmap-subset decision
 *     (power_base_transport.c's coap_sensor_handler())
 *   - the token-based stale-callback rejection in coap_response_handler()
 *     (topology_reporter.c / sensor_transport.c, identical pattern in both)
 *   - the client-side "2.xx only" success check (send_one_attempt())
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_coap_response_semantics tools/test_coap_response_semantics.c \
 *   && /tmp/test_coap_response_semantics
 */
#include <stdio.h>
#include <stdint.h>
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

/* ---- mirrors topology_registry_result_t + topology_result_to_coap_code() ---- */
typedef enum {
    REG_OK = 0,
    REG_OK_DUPLICATE,
    REG_ERR_SELF_PARENT,
    REG_ERR_INVALID_DEPTH,
    REG_ERR_DEPTH1_BAD_PARENT,
    REG_ERR_UNKNOWN_PARENT,
    REG_ERR_PARENT_DEPTH_MISMATCH,
    REG_ERR_CYCLE,
    REG_ERR_STALE_SESSION,
    REG_ERR_DUPLICATE_SEQ,
    REG_ERR_FULL,
} reg_result_t;

/* Mirrors OT_COAP_CODE(c, d) = (c<<5)|d from openthread/coap.h. */
#define COAP_CODE(c, d) (((c) << 5) | (d))
#define COAP_CHANGED COAP_CODE(2, 4)
#define COAP_VALID COAP_CODE(2, 3)
#define COAP_BAD_REQUEST COAP_CODE(4, 0)
#define COAP_NOT_FOUND COAP_CODE(4, 4)
#define COAP_PRECONDITION_FAILED COAP_CODE(4, 12)
#define COAP_SERVICE_UNAVAILABLE COAP_CODE(5, 3)

static uint8_t topology_result_to_coap_code(reg_result_t r)
{
    switch (r) {
    case REG_OK:
        return COAP_CHANGED;
    case REG_OK_DUPLICATE:
        return COAP_VALID;
    case REG_ERR_FULL:
        return COAP_SERVICE_UNAVAILABLE;
    default:
        return COAP_PRECONDITION_FAILED;
    }
}

static bool coap_code_is_success(uint8_t code)
{
    return (code >> 5) == 2;
}

static void scenario_topology_response_code_mapping(void)
{
    printf("\n-- scenario: topology registry result -> CoAP response code mapping --\n");
    CHECK(topology_result_to_coap_code(REG_OK) == COAP_CHANGED, "newly-applied -> 2.04 Changed");
    CHECK(topology_result_to_coap_code(REG_OK_DUPLICATE) == COAP_VALID,
          "idempotent duplicate -> 2.03 Valid (a success code, not an error)");
    CHECK(topology_result_to_coap_code(REG_ERR_FULL) == COAP_SERVICE_UNAVAILABLE, "registry full -> 5.03 Service Unavailable");
    CHECK(topology_result_to_coap_code(REG_ERR_SELF_PARENT) == COAP_PRECONDITION_FAILED,
          "self-parent -> 4.12 Precondition Failed");
    CHECK(topology_result_to_coap_code(REG_ERR_CYCLE) == COAP_PRECONDITION_FAILED, "cycle -> 4.12 Precondition Failed");
    CHECK(topology_result_to_coap_code(REG_ERR_STALE_SESSION) == COAP_PRECONDITION_FAILED,
          "stale session -> 4.12 Precondition Failed");
    CHECK(topology_result_to_coap_code(REG_ERR_DUPLICATE_SEQ) == COAP_PRECONDITION_FAILED,
          "reordered/duplicate seq -> 4.12 Precondition Failed");

    CHECK(coap_code_is_success(COAP_CHANGED), "2.04 is recognized as success by the client-side check");
    CHECK(coap_code_is_success(COAP_VALID), "2.03 is recognized as success by the client-side check");
    CHECK(!coap_code_is_success(COAP_BAD_REQUEST), "4.00 is NOT success");
    CHECK(!coap_code_is_success(COAP_PRECONDITION_FAILED), "4.12 is NOT success");
    CHECK(!coap_code_is_success(COAP_SERVICE_UNAVAILABLE), "5.03 is NOT success");
}

/* ---- mirrors coap_sensor_handler()'s known/online/bitmap decision ---- */
typedef enum { SENSOR_STORED, SENSOR_UNKNOWN, SENSOR_OFFLINE, SENSOR_BITMAP_MISMATCH } sensor_outcome_t;

static sensor_outcome_t decide_sensor_sample(bool known, bool online, uint16_t node_bitmap, uint16_t sample_bitmap)
{
    if (!known) {
        return SENSOR_UNKNOWN;
    }
    if (!online) {
        return SENSOR_OFFLINE;
    }
    if ((sample_bitmap & (uint16_t)~node_bitmap) != 0) {
        return SENSOR_BITMAP_MISMATCH;
    }
    return SENSOR_STORED;
}

static uint8_t sensor_outcome_to_coap_code(sensor_outcome_t o)
{
    switch (o) {
    case SENSOR_STORED:
        return COAP_CHANGED;
    case SENSOR_UNKNOWN:
        return COAP_NOT_FOUND;
    case SENSOR_OFFLINE:
    case SENSOR_BITMAP_MISMATCH:
    default:
        return COAP_PRECONDITION_FAILED;
    }
}

static void scenario_sensor_data_topology_validation(void)
{
    printf("\n-- scenario: sensor sample validated against topology registry state (section 4) --\n");

    CHECK(decide_sensor_sample(true, true, 0x0003, 0x0001) == SENSOR_STORED,
          "known+online+subset bitmap (0x0001 subset of 0x0003) is stored");
    CHECK(decide_sensor_sample(true, true, 0x0003, 0x0003) == SENSOR_STORED,
          "known+online+EXACT matching bitmap is stored");
    CHECK(decide_sensor_sample(false, false, 0, 0x0001) == SENSOR_UNKNOWN,
          "a uuid never reported to the topology registry is UNKNOWN, not stored");
    CHECK(decide_sensor_sample(true, false, 0x0003, 0x0001) == SENSOR_OFFLINE,
          "a KNOWN but OFFLINE uuid's sample is rejected, not stored");
    CHECK(decide_sensor_sample(true, true, 0x0001, 0x0003) == SENSOR_BITMAP_MISMATCH,
          "a sample claiming a channel (0x0002) beyond the node's own reported bitmap (0x0001) is rejected");

    CHECK(sensor_outcome_to_coap_code(SENSOR_STORED) == COAP_CHANGED, "stored -> 2.04 Changed");
    CHECK(sensor_outcome_to_coap_code(SENSOR_UNKNOWN) == COAP_NOT_FOUND, "unknown uuid -> 4.04 Not Found");
    CHECK(sensor_outcome_to_coap_code(SENSOR_OFFLINE) == COAP_PRECONDITION_FAILED, "offline -> 4.12 Precondition Failed");
    CHECK(sensor_outcome_to_coap_code(SENSOR_BITMAP_MISMATCH) == COAP_PRECONDITION_FAILED,
          "bitmap mismatch -> 4.12 Precondition Failed");
}

/* ---- mirrors coap_response_handler()'s token-based stale-callback guard,
 * identical in topology_reporter.c and sensor_transport.c ---- */
/* A tiny stand-in for otError -- only NONE vs "something else" matters here. */
typedef enum { SIM_OT_ERROR_NONE, SIM_OT_ERROR_OTHER } otErrorSim;

typedef struct {
    uint32_t current_token;
    otErrorSim last_result_valid;
    uint8_t    last_response_code;
} client_state_t;

static bool fire_callback(client_state_t *st, uint32_t callback_token, otErrorSim result, uint8_t response_code,
                           bool has_message)
{
    /* Mirrors coap_response_handler(): only acts if the token still
     * matches -- a late callback for an attempt the waiter already gave up
     * on (a newer token is now current) does nothing at all. */
    if (callback_token != st->current_token) {
        return false; /* stale -- correctly ignored */
    }
    st->last_result_valid = (result == SIM_OT_ERROR_NONE && has_message) ? SIM_OT_ERROR_NONE : SIM_OT_ERROR_OTHER;
    st->last_response_code = response_code;
    return true; /* the semaphore would be given here */
}

static void scenario_stale_callback_after_timeout_is_ignored(void)
{
    printf("\n-- scenario: a callback that fires AFTER the waiter already gave up is ignored, not a crash --\n");
    client_state_t st = {.current_token = 0};

    /* Attempt 1: waiter bumps the token, sends, then times out (gives up)
     * WITHOUT the callback having fired yet. */
    st.current_token = 1;
    uint32_t attempt1_token = st.current_token;
    /* (waiter times out here -- no callback yet) */

    /* Attempt 2 begins: a NEW token is assigned before attempt 1's callback
     * ever fires. */
    st.current_token = 2;

    /* NOW attempt 1's callback finally fires, late. */
    bool acted = fire_callback(&st, attempt1_token, SIM_OT_ERROR_NONE, COAP_CHANGED, true);
    CHECK(!acted, "a late callback for a superseded (already-abandoned) token does nothing -- no stale semaphore give, "
                  "no corrupting attempt 2's in-flight result");

    /* Attempt 2's OWN callback fires normally afterward. */
    bool acted2 = fire_callback(&st, st.current_token, SIM_OT_ERROR_NONE, COAP_CHANGED, true);
    CHECK(acted2, "attempt 2's own (current-token) callback IS acted upon normally");
    CHECK(st.last_response_code == COAP_CHANGED, "attempt 2's result is what actually got recorded, not attempt 1's");
}

static void scenario_rejected_response_is_not_logged_as_sent_ok(void)
{
    printf("\n-- scenario: a rejected (non-2.xx) response must not be treated as 'sent OK' --\n");
    /* Mirrors send_one_attempt()'s final success check: OT transport success
     * AND a 2.xx response code, both required. */
    bool transport_ok = true;
    uint8_t response_code = COAP_PRECONDITION_FAILED; /* the power base rejected it */
    bool would_be_logged_sent_ok = transport_ok && coap_code_is_success(response_code);
    CHECK(!would_be_logged_sent_ok, "a rejected response is never logged as 'sent OK' even though the OpenThread "
                                    "transport itself succeeded");

    response_code = COAP_CHANGED;
    would_be_logged_sent_ok = transport_ok && coap_code_is_success(response_code);
    CHECK(would_be_logged_sent_ok, "an actually-accepted (2.xx) response IS logged as 'sent OK'");
}

int main(void)
{
    scenario_topology_response_code_mapping();
    scenario_sensor_data_topology_validation();
    scenario_stale_callback_after_timeout_is_ignored();
    scenario_rejected_response_is_not_logged_as_sent_ok();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
