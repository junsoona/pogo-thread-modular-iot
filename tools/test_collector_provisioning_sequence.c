/*
 * Host-compiled verification of child_service.c's new SET_COLLECTOR
 * provisioning step and its ordering relative to SET_DATASET (see
 * child_service.h's header comment and docs/power-base.md), plus
 * parent_service.c's INTERMEDIATE-forwards-the-original-endpoint policy --
 * required test items 9/10/11/12 of this follow-up's minimum test list:
 *
 *   9.  "SET_COLLECTOR가 성공하기 전 SET_DATASET 수신 -> POGO_NACK_BAD_STATE"
 *   10. "새 provisioning session 시작 / upstream DETACHED/ERROR -> collector
 *        invalid 처리"
 *   11. "SET_COLLECTOR 저장 실패 -> POGO_NACK_STORAGE_FAILED", "NVS 저장
 *        실패 시 join 시작 안 함"
 *   12. "INTERMEDIATE가 원래 power base endpoint를 그대로 전달"
 *
 * Links against the REAL, unmodified pogo_pack_set_collector()/
 * pogo_unpack_set_collector()/pogo_collector_endpoint_is_valid() from
 * components/pogo_proto (no ESP-IDF/FreeRTOS dependency) -- only the
 * surrounding cs_state_t state machine of child_service.c (and a tiny
 * mirror of parent_service.c's forwarding call) is reimplemented here, same
 * convention as tools/test_child_provisioning_session.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_collector_provisioning_sequence tools/test_collector_provisioning_sequence.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_collector_provisioning_sequence
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

/* ---- minimal mirror of child_service.c's relevant cs_state_t slice ---- */
typedef enum {
    CS_STATE_POLL_SET,      /* POLL_CONFIG acked -- SET_COLLECTOR is valid from here */
    CS_STATE_COLLECTOR_SET, /* SET_COLLECTOR stored -- SET_DATASET is valid from here */
    CS_STATE_JOINING,
    CS_STATE_ERROR,
} cs_state_t;

/* Mirrors components/collector_endpoint's tiny in-RAM cache. */
typedef struct {
    pogo_collector_endpoint_t ep;
    bool valid;
} collector_cache_t;

static void cache_set(collector_cache_t *c, const pogo_collector_endpoint_t *ep)
{
    c->ep = *ep;
    c->valid = true;
}
static void cache_clear(collector_cache_t *c)
{
    c->valid = false;
}

typedef struct {
    cs_state_t state;
    collector_cache_t collector; /* mirrors this module's OWN components/collector_endpoint snapshot */
    bool nvs_write_should_fail;  /* simulated save_collector_dynamic_record() outcome */
} child_t;

static void child_init(child_t *c)
{
    memset(c, 0, sizeof(*c));
    c->state = CS_STATE_POLL_SET;
}

/* Mirrors clear_provisioning_temporaries()'s new collector_endpoint_clear()
 * call (this follow-up) -- shared by both reset_session() (ATTACHED/
 * DETACHED) and begin_new_provisioning_session() (PEER_READY / a fresh
 * IDENTIFY_REQ), plus the explicit call added to the POGO_PORT_EVENT_ERROR
 * case in child_service_task()'s switch. */
static void on_new_session_or_detach_or_error(child_t *c)
{
    cache_clear(&c->collector);
    c->state = CS_STATE_POLL_SET;
}

/* Mirrors handle_set_collector(). */
static uint8_t handle_set_collector(child_t *c, const uint8_t *wire, uint16_t wire_len)
{
    pogo_collector_endpoint_t ep;
    if (c->state != CS_STATE_POLL_SET) {
        return POGO_NACK_BAD_STATE;
    }
    if (!pogo_unpack_set_collector(wire, wire_len, &ep)) {
        return POGO_NACK_BAD_STATE;
    }
    if (!pogo_collector_endpoint_is_valid(&ep)) {
        return POGO_NACK_INVALID_COLLECTOR;
    }
    if (c->nvs_write_should_fail) {
        return POGO_NACK_STORAGE_FAILED; /* mirrors save_collector_dynamic_record() failing -- state unchanged */
    }
    cache_set(&c->collector, &ep);
    c->state = CS_STATE_COLLECTOR_SET;
    return 0; /* 0 == ACK, no NACK reason */
}

/* Mirrors handle_set_dataset()'s state-order gate (now CS_STATE_COLLECTOR_SET,
 * not CS_STATE_POLL_SET). */
static uint8_t handle_set_dataset(child_t *c)
{
    if (c->state != CS_STATE_COLLECTOR_SET) {
        return POGO_NACK_BAD_STATE;
    }
    c->state = CS_STATE_JOINING; /* Thread join actually starts here in the real code */
    return 0;
}

static void make_endpoint(pogo_collector_endpoint_t *ep, uint8_t uuid_seed, uint16_t port)
{
    memset(ep, 0, sizeof(*ep));
    for (int i = 0; i < POGO_UUID_LEN; i++) {
        ep->power_base_uuid[i] = (uint8_t)(uuid_seed + i);
    }
    ep->addr[0] = 0xfd;
    ep->addr[1] = 0x01;
    ep->addr[15] = 0x01;
    ep->port = port;
    ep->generation = 1;
}

static void scenario_set_dataset_before_collector_set_rejected(void)
{
    printf("\n-- scenario: SET_DATASET before SET_COLLECTOR succeeds is rejected (POGO_NACK_BAD_STATE) --\n");
    child_t c;
    child_init(&c);
    CHECK(c.state == CS_STATE_POLL_SET, "starts at POLL_SET (POLL_CONFIG already acked)");

    uint8_t reason = handle_set_dataset(&c);
    CHECK(reason == POGO_NACK_BAD_STATE, "SET_DATASET arriving before SET_COLLECTOR is NACKed BAD_STATE");
    CHECK(c.state == CS_STATE_POLL_SET, "state does not advance to JOINING");
}

static void scenario_set_collector_then_set_dataset_succeeds(void)
{
    printf("\n-- scenario: normal order -- SET_COLLECTOR then SET_DATASET both succeed --\n");
    child_t c;
    child_init(&c);

    pogo_collector_endpoint_t ep;
    make_endpoint(&ep, 0x10, 5683);
    uint8_t wire[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len = pogo_pack_set_collector(&ep, wire, sizeof(wire));

    uint8_t reason = handle_set_collector(&c, wire, (uint16_t)len);
    CHECK(reason == 0, "SET_COLLECTOR is ACKed (no NACK reason)");
    CHECK(c.state == CS_STATE_COLLECTOR_SET, "state advances to COLLECTOR_SET");
    CHECK(c.collector.valid, "components/collector_endpoint's snapshot is now valid");
    CHECK(memcmp(c.collector.ep.power_base_uuid, ep.power_base_uuid, POGO_UUID_LEN) == 0,
          "the stored snapshot matches exactly what was sent");

    reason = handle_set_dataset(&c);
    CHECK(reason == 0, "SET_DATASET now succeeds (ACK)");
    CHECK(c.state == CS_STATE_JOINING, "state advances to JOINING -- the Thread join now actually starts");
}

static void scenario_invalid_collector_endpoint_rejected(void)
{
    printf("\n-- scenario: a structurally-fine but semantically-invalid endpoint is rejected (POGO_NACK_INVALID_COLLECTOR) --\n");
    child_t c;
    child_init(&c);

    pogo_collector_endpoint_t ep;
    make_endpoint(&ep, 0x20, 5683);
    memset(ep.addr, 0, sizeof(ep.addr)); /* unspecified address -- structurally fine, semantically invalid */
    uint8_t wire[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len = pogo_pack_set_collector(&ep, wire, sizeof(wire));

    uint8_t reason = handle_set_collector(&c, wire, (uint16_t)len);
    CHECK(reason == POGO_NACK_INVALID_COLLECTOR, "an unspecified address is rejected as POGO_NACK_INVALID_COLLECTOR");
    CHECK(c.state == CS_STATE_POLL_SET, "state does not advance");
    CHECK(!c.collector.valid, "components/collector_endpoint is never populated with the invalid endpoint");

    /* And a malformed (wrong length) payload is BAD_STATE, not
     * INVALID_COLLECTOR -- structural vs. semantic errors are distinct
     * NACK reasons, same as every other message type in this file. */
    child_t c2;
    child_init(&c2);
    uint8_t short_wire[POGO_COLLECTOR_ENDPOINT_WIRE_LEN - 1] = {0};
    reason = handle_set_collector(&c2, short_wire, sizeof(short_wire));
    CHECK(reason == POGO_NACK_BAD_STATE, "a wrong-length SET_COLLECTOR payload is NACKed BAD_STATE (structural)");
}

static void scenario_storage_failure_no_join(void)
{
    printf("\n-- scenario: SET_COLLECTOR NVS storage failure -> POGO_NACK_STORAGE_FAILED, no join starts --\n");
    child_t c;
    child_init(&c);
    c.nvs_write_should_fail = true;

    pogo_collector_endpoint_t ep;
    make_endpoint(&ep, 0x30, 5683);
    uint8_t wire[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len = pogo_pack_set_collector(&ep, wire, sizeof(wire));

    uint8_t reason = handle_set_collector(&c, wire, (uint16_t)len);
    CHECK(reason == POGO_NACK_STORAGE_FAILED, "an NVS write failure is NACKed STORAGE_FAILED");
    CHECK(c.state == CS_STATE_POLL_SET, "state stays at POLL_SET -- never advances to COLLECTOR_SET");
    CHECK(!c.collector.valid, "components/collector_endpoint is never populated on a storage failure");

    /* SET_DATASET is therefore still rejected -- the join never starts. */
    reason = handle_set_dataset(&c);
    CHECK(reason == POGO_NACK_BAD_STATE, "SET_DATASET is still rejected after the storage failure -- no Thread join begins");
    CHECK(c.state == CS_STATE_POLL_SET, "state never reaches JOINING");
}

static void scenario_new_session_and_detach_invalidate_collector(void)
{
    printf("\n-- scenario: a new provisioning session / detach / port ERROR invalidates the collector endpoint --\n");
    child_t c;
    child_init(&c);
    pogo_collector_endpoint_t ep;
    make_endpoint(&ep, 0x40, 5683);
    uint8_t wire[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len = pogo_pack_set_collector(&ep, wire, sizeof(wire));
    handle_set_collector(&c, wire, (uint16_t)len);
    CHECK(c.collector.valid, "collector is valid after a successful SET_COLLECTOR");

    /* A fresh PEER_READY (begin_new_provisioning_session()) or a physical
     * DETACHED (reset_session()) both route through
     * clear_provisioning_temporaries(), which now also calls
     * collector_endpoint_clear(). */
    on_new_session_or_detach_or_error(&c);
    CHECK(!c.collector.valid, "a new provisioning session immediately invalidates the OLD collector endpoint");
    CHECK(c.state == CS_STATE_POLL_SET, "state resets to (before) POLL_SET's successor state");

    /* Re-populate, then simulate a POGO_PORT_EVENT_ERROR on the UPSTREAM
     * port (child_service.c's switch case, which does NOT route through
     * clear_provisioning_temporaries() and so needed its own explicit
     * collector_endpoint_clear() call added). */
    child_init(&c);
    handle_set_collector(&c, wire, (uint16_t)len);
    CHECK(c.collector.valid, "collector is valid again after a fresh SET_COLLECTOR");
    on_new_session_or_detach_or_error(&c); /* models the ERROR-case explicit clear */
    CHECK(!c.collector.valid, "an UPSTREAM port ERROR also invalidates the collector endpoint");
}

/* ---- parent_service.c's INTERMEDIATE-forwards-unmodified mirror ---- */

/* Mirrors module_parent_get_collector() for the INTERMEDIATE branch: it
 * must return EXACTLY the snapshot this module itself received, never
 * substituting anything (e.g. its own address). */
static bool intermediate_get_collector(const collector_cache_t *own_snapshot, pogo_collector_endpoint_t *out)
{
    if (!own_snapshot->valid) {
        return false;
    }
    *out = own_snapshot->ep; /* forwarded VERBATIM -- no field is ever modified */
    return true;
}

static void scenario_intermediate_forwards_original_endpoint_unmodified(void)
{
    printf("\n-- scenario: an INTERMEDIATE module forwards the ORIGINAL power-base endpoint unmodified --\n");

    /* This module's own upstream (the power base, or another INTERMEDIATE
     * module) sent it a real endpoint. */
    pogo_collector_endpoint_t power_base_ep;
    make_endpoint(&power_base_ep, 0xA0, 5683);

    collector_cache_t own_snapshot = {0};
    cache_set(&own_snapshot, &power_base_ep);

    /* This module's own address -- what module_parent_get_collector() must
     * NEVER substitute in, per docs/power-base.md section 5: "자신의 IPv6
     * 주소로 바꾸면 안 된다". */
    pogo_collector_endpoint_t this_modules_own_identity;
    make_endpoint(&this_modules_own_identity, 0xB0, 5683);
    CHECK(memcmp(this_modules_own_identity.power_base_uuid, power_base_ep.power_base_uuid, POGO_UUID_LEN) != 0,
          "sanity: this module's own identity differs from the power base's");

    pogo_collector_endpoint_t forwarded;
    bool ok = intermediate_get_collector(&own_snapshot, &forwarded);
    CHECK(ok, "an INTERMEDIATE module with a valid own-snapshot can forward a collector endpoint");
    CHECK(memcmp(forwarded.power_base_uuid, power_base_ep.power_base_uuid, POGO_UUID_LEN) == 0,
          "the forwarded power_base_uuid is EXACTLY the original power base's, not this module's own");
    CHECK(memcmp(forwarded.addr, power_base_ep.addr, sizeof(forwarded.addr)) == 0,
          "the forwarded addr is EXACTLY the original power base's Mesh-Local EID, not this module's own");
    CHECK(forwarded.port == power_base_ep.port, "the forwarded port matches exactly");
    CHECK(forwarded.generation == power_base_ep.generation, "the forwarded generation matches exactly (unmodified passthrough)");

    /* Without a valid own-snapshot (e.g. this module's own SET_COLLECTOR
     * step has not completed yet, or was just invalidated), forwarding
     * fails cleanly rather than fabricating anything. */
    collector_cache_t empty = {0};
    pogo_collector_endpoint_t out2;
    CHECK(!intermediate_get_collector(&empty, &out2), "forwarding fails cleanly when this module has no valid endpoint of its own yet");
}

int main(void)
{
    scenario_set_dataset_before_collector_set_rejected();
    scenario_set_collector_then_set_dataset_succeeds();
    scenario_invalid_collector_endpoint_rejected();
    scenario_storage_failure_no_join();
    scenario_new_session_and_detach_invalidate_collector();
    scenario_intermediate_forwards_original_endpoint_unmodified();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
