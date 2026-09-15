/*
 * Host-compiled verification of components/collector_endpoint/collector_endpoint.c
 * -- the FreeRTOS-task-safe holder of a sensor module's own collector
 * (power base) endpoint. Links against the REAL, unmodified component (a
 * portMUX_TYPE critical section is its only FreeRTOS dependency, shadowed
 * here by tools/host_test_stubs/freertos/FreeRTOS.h, the same convention
 * tools/test_topology_gate.c already uses for topology_gate.c's event
 * group).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs -I components/collector_endpoint/include \
 *      -o /tmp/test_collector_endpoint tools/test_collector_endpoint.c \
 *      components/collector_endpoint/collector_endpoint.c \
 *   && /tmp/test_collector_endpoint
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "collector_endpoint/collector_endpoint.h"

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

static void make_ml_eid(uint8_t out[COLLECTOR_ENDPOINT_ADDR_LEN], uint8_t last_byte)
{
    memset(out, 0, COLLECTOR_ENDPOINT_ADDR_LEN);
    out[0] = 0xfd;
    out[1] = 0x99;
    out[15] = last_byte;
}

static void scenario_init_starts_invalid(void)
{
    printf("\n-- scenario: collector_endpoint starts invalid after init --\n");
    CHECK(collector_endpoint_init() == ESP_OK, "collector_endpoint_init() succeeds");
    CHECK(!collector_endpoint_is_valid(), "no endpoint is valid immediately after init");

    collector_endpoint_snapshot_t snap;
    CHECK(collector_endpoint_get(&snap), "get() succeeds even with nothing set (fills a valid=false snapshot)");
    CHECK(!snap.valid, "the snapshot's own valid field is false");
}

static void scenario_set_get_round_trip(void)
{
    printf("\n-- scenario: set()/get() round trip and caller buffers are not retained --\n");
    collector_endpoint_init();

    uint8_t uuid[COLLECTOR_ENDPOINT_UUID_LEN];
    for (int i = 0; i < COLLECTOR_ENDPOINT_UUID_LEN; i++) uuid[i] = (uint8_t)(0x20 + i);
    uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN];
    make_ml_eid(addr, 0x07);

    esp_err_t err = collector_endpoint_set(uuid, addr, 5683, 42);
    CHECK(err == ESP_OK, "set() succeeds with a valid endpoint");
    CHECK(collector_endpoint_is_valid(), "is_valid() is true after a successful set()");

    collector_endpoint_snapshot_t snap;
    CHECK(collector_endpoint_get(&snap), "get() succeeds");
    CHECK(snap.valid, "snapshot.valid is true");
    CHECK(memcmp(snap.power_base_uuid, uuid, COLLECTOR_ENDPOINT_UUID_LEN) == 0, "uuid round-trips");
    CHECK(memcmp(snap.addr, addr, COLLECTOR_ENDPOINT_ADDR_LEN) == 0, "addr round-trips");
    CHECK(snap.port == 5683, "port round-trips");
    CHECK(snap.generation == 42, "generation round-trips");

    /* The caller's own buffers are copied, not retained by pointer -- mutate
     * them after set() returns and confirm the stored snapshot is
     * unaffected. */
    memset(uuid, 0xEE, sizeof(uuid));
    memset(addr, 0xEE, sizeof(addr));
    collector_endpoint_snapshot_t snap2;
    collector_endpoint_get(&snap2);
    CHECK(snap2.power_base_uuid[0] == 0x20, "stomping the caller's own uuid buffer after set() does not affect the stored copy");
    CHECK(snap2.addr[0] == 0xfd, "stomping the caller's own addr buffer after set() does not affect the stored copy");
}

static void scenario_clear_invalidates(void)
{
    printf("\n-- scenario: clear() invalidates immediately --\n");
    collector_endpoint_init();
    uint8_t uuid[COLLECTOR_ENDPOINT_UUID_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN];
    make_ml_eid(addr, 1);
    collector_endpoint_set(uuid, addr, 5683, 1);
    CHECK(collector_endpoint_is_valid(), "valid right after set()");

    collector_endpoint_clear();
    CHECK(!collector_endpoint_is_valid(), "invalid immediately after clear()");
    collector_endpoint_snapshot_t snap;
    collector_endpoint_get(&snap);
    CHECK(!snap.valid, "the returned snapshot's valid field is also false after clear()");
}

static void scenario_reject_invalid_inputs(void)
{
    printf("\n-- scenario: set() independently rejects bad inputs (defense in depth) --\n");
    collector_endpoint_init();

    uint8_t good_uuid[COLLECTOR_ENDPOINT_UUID_LEN] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t zero_uuid[COLLECTOR_ENDPOINT_UUID_LEN] = {0};
    uint8_t good_addr[COLLECTOR_ENDPOINT_ADDR_LEN];
    make_ml_eid(good_addr, 9);
    uint8_t zero_addr[COLLECTOR_ENDPOINT_ADDR_LEN] = {0};
    uint8_t multicast_addr[COLLECTOR_ENDPOINT_ADDR_LEN] = {0};
    multicast_addr[0] = 0xFF;
    uint8_t link_local_addr[COLLECTOR_ENDPOINT_ADDR_LEN] = {0};
    link_local_addr[0] = 0xFE;
    link_local_addr[1] = 0x80;

    CHECK(collector_endpoint_set(zero_uuid, good_addr, 5683, 1) == ESP_ERR_INVALID_ARG, "an all-zero uuid is rejected");
    CHECK(collector_endpoint_set(good_uuid, zero_addr, 5683, 1) == ESP_ERR_INVALID_ARG, "the unspecified address is rejected");
    CHECK(collector_endpoint_set(good_uuid, multicast_addr, 5683, 1) == ESP_ERR_INVALID_ARG, "a multicast address is rejected");
    CHECK(collector_endpoint_set(good_uuid, link_local_addr, 5683, 1) == ESP_ERR_INVALID_ARG, "a link-local address is rejected");
    CHECK(collector_endpoint_set(good_uuid, good_addr, 0, 1) == ESP_ERR_INVALID_ARG, "port 0 is rejected");
    CHECK(!collector_endpoint_is_valid(), "none of the above rejected calls left the endpoint valid");

    /* And a previously-valid endpoint is left UNTOUCHED by a later rejected
     * set() call -- rejecting bad input must not silently clobber good
     * state. */
    collector_endpoint_set(good_uuid, good_addr, 5683, 7);
    CHECK(collector_endpoint_is_valid(), "a good set() succeeds");
    collector_endpoint_snapshot_t before;
    collector_endpoint_get(&before);
    collector_endpoint_set(zero_uuid, good_addr, 5683, 99); /* rejected */
    collector_endpoint_snapshot_t after;
    collector_endpoint_get(&after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0, "a rejected set() call leaves the previous valid snapshot untouched");
}

static void scenario_reinit_resets_to_invalid(void)
{
    printf("\n-- scenario: a fresh collector_endpoint_init() resets to invalid (models a reboot) --\n");
    uint8_t uuid[COLLECTOR_ENDPOINT_UUID_LEN] = {9, 9, 9, 9, 9, 9, 9, 9};
    uint8_t addr[COLLECTOR_ENDPOINT_ADDR_LEN];
    make_ml_eid(addr, 3);
    collector_endpoint_set(uuid, addr, 5683, 1);
    CHECK(collector_endpoint_is_valid(), "set() succeeds before the (simulated) reboot");

    collector_endpoint_init();
    CHECK(!collector_endpoint_is_valid(), "invalid again right after re-init");
}

int main(void)
{
    scenario_init_starts_invalid();
    scenario_set_get_round_trip();
    scenario_clear_invalidates();
    scenario_reject_invalid_inputs();
    scenario_reinit_resets_to_invalid();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
