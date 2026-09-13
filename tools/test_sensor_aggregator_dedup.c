/*
 * Host-compiled verification of the ROOT-side sample aggregator's
 * (uuid, sample_seq) duplicate suppression -- section 8/13 of the follow-up
 * spec: "CoAP duplicate suppression". Mirrors
 * sensor_service_root_record_sample()/sensor_service_root_record_error()
 * (components/sensor_service/sensor_service.c) step for step -- the mutex
 * and esp_timer timestamping are the only things not reproduced here (this
 * model is single-threaded and uses a plain counter for "time"), since
 * neither changes the dedup decision itself. If sensor_service.c's dedup
 * logic changes, update the model below to match -- same caveat as
 * tools/test_peer_hello_handshake.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_sensor_aggregator_dedup tools/test_sensor_aggregator_dedup.c \
 *   && /tmp/test_sensor_aggregator_dedup
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_NODES 16
#define UUID_LEN 8

typedef struct {
    bool valid;
    uint8_t uuid[UUID_LEN];
    bool has_sample;
    uint32_t last_seq;
    uint32_t error_count;
    int record_count; /* diagnostic: how many times a NEW sample was actually stored (not a dedup no-op) */
} node_t;

static node_t g_nodes[MAX_NODES];

static void reset_aggregator(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
}

/* Mirrors sensor_service_root_record_sample(). Returns true if the sample
 * was actually (newly) recorded, false if it was dropped (dedup, or the
 * table is full and this is an unseen uuid). */
static bool record_sample(const uint8_t uuid[UUID_LEN], uint32_t seq)
{
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].valid && memcmp(g_nodes[i].uuid, uuid, UUID_LEN) == 0) {
            slot = i;
            break;
        }
        if (!g_nodes[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }
    if (slot < 0) {
        if (free_slot < 0) {
            return false; /* aggregator full */
        }
        slot = free_slot;
        g_nodes[slot].valid = true;
        memcpy(g_nodes[slot].uuid, uuid, UUID_LEN);
    }

    node_t *rec = &g_nodes[slot];
    if (rec->has_sample && rec->last_seq == seq) {
        return false; /* exact repeat of the last-recorded (uuid, seq) -- dropped */
    }
    rec->last_seq = seq;
    rec->has_sample = true;
    rec->record_count++;
    return true;
}

static void record_error(const uint8_t uuid[UUID_LEN])
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].valid && memcmp(g_nodes[i].uuid, uuid, UUID_LEN) == 0) {
            g_nodes[i].error_count++;
            return;
        }
    }
}

static int find_slot(const uint8_t uuid[UUID_LEN])
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].valid && memcmp(g_nodes[i].uuid, uuid, UUID_LEN) == 0) {
            return i;
        }
    }
    return -1;
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

static const uint8_t UUID_A[UUID_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t UUID_B[UUID_LEN] = {8, 7, 6, 5, 4, 3, 2, 1};

static void test_first_sample_always_recorded(void)
{
    reset_aggregator();
    CHECK(record_sample(UUID_A, 1) == true, "first-ever sample for a UUID is recorded");
    int slot = find_slot(UUID_A);
    CHECK(slot >= 0 && g_nodes[slot].record_count == 1, "exactly one record stored");
}

static void test_exact_duplicate_dropped(void)
{
    reset_aggregator();
    record_sample(UUID_A, 5);
    bool second = record_sample(UUID_A, 5); /* same seq -- e.g. a retransmitted CoAP POST */
    CHECK(second == false, "an exact (uuid, seq) repeat is dropped, not re-recorded");
    int slot = find_slot(UUID_A);
    CHECK(g_nodes[slot].record_count == 1, "duplicate does not increment the record count");
}

static void test_new_seq_from_same_module_is_recorded(void)
{
    reset_aggregator();
    record_sample(UUID_A, 1);
    bool ok = record_sample(UUID_A, 2); /* a genuinely new sample, not a retransmit */
    CHECK(ok == true, "a new sample_seq from the same module is recorded (not treated as duplicate)");
    int slot = find_slot(UUID_A);
    CHECK(g_nodes[slot].record_count == 2, "record count reflects both distinct samples");
    CHECK(g_nodes[slot].last_seq == 2, "last_seq tracks the most recent sample");
}

static void test_out_of_order_duplicate_of_older_seq_still_recorded(void)
{
    /* Dedup here is "exact repeat of the LAST recorded seq", not "any seq
     * <= something seen before" -- an out-of-order older seq arriving after
     * a newer one is not the scenario section 8 asks to suppress (that
     * would require a full seen-set, out of scope here); confirm the
     * simpler, documented behavior instead of silently assuming otherwise. */
    reset_aggregator();
    record_sample(UUID_A, 5);
    record_sample(UUID_A, 6);
    bool ok = record_sample(UUID_A, 5); /* seq 5 again, but last_seq is now 6 */
    CHECK(ok == true, "documents actual behavior: dedup only catches a repeat of the MOST RECENT seq, not any prior one");
}

static void test_two_modules_independent(void)
{
    reset_aggregator();
    record_sample(UUID_A, 1);
    record_sample(UUID_B, 1); /* same seq value, but a DIFFERENT module -- must not be treated as a duplicate of A's */
    int slot_a = find_slot(UUID_A);
    int slot_b = find_slot(UUID_B);
    CHECK(slot_a >= 0 && slot_b >= 0 && slot_a != slot_b, "two different modules get two different slots");
    CHECK(g_nodes[slot_a].record_count == 1 && g_nodes[slot_b].record_count == 1,
          "identical seq values from different modules are both recorded independently");
}

static void test_error_count_independent_of_dedup(void)
{
    reset_aggregator();
    record_sample(UUID_A, 1);
    record_error(UUID_A);
    record_error(UUID_A);
    int slot = find_slot(UUID_A);
    CHECK(g_nodes[slot].error_count == 2, "error_count accumulates independently of successful sample recording");
    CHECK(g_nodes[slot].last_seq == 1, "recording an error does not disturb the last recorded sample/seq");
}

static void test_aggregator_full_drops_new_module(void)
{
    reset_aggregator();
    uint8_t uuid[UUID_LEN] = {0};
    for (int i = 0; i < MAX_NODES; i++) {
        uuid[0] = (uint8_t)i;
        CHECK(record_sample(uuid, 1) == true, "filling the aggregator: each of the first MAX_NODES modules is accepted");
    }
    uuid[0] = (uint8_t)MAX_NODES; /* one more, brand new UUID -- table is now full */
    bool ok = record_sample(uuid, 1);
    CHECK(ok == false, "a (MAX_NODES+1)-th distinct module is dropped once the bounded aggregator is full");
}

int main(void)
{
    test_first_sample_always_recorded();
    test_exact_duplicate_dropped();
    test_new_seq_from_same_module_is_recorded();
    test_out_of_order_duplicate_of_older_seq_still_recorded();
    test_two_modules_independent();
    test_error_count_independent_of_dedup();
    test_aggregator_full_drops_new_module();

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
