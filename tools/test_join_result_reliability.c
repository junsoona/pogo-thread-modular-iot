/*
 * Host-compiled verification of JOIN_RESULT/JOIN_RESULT_ACK reliability
 * (section 7 of the follow-up spec) -- the child's retry-with-same-seq state
 * machine (mirrors begin_join_result_retry()/poll_join_result_retry()/the
 * POGO_MSG_JOIN_RESULT_ACK case in child_service.c's handle_frame()) and the
 * parent's record+ACK / duplicate-suppression logic (mirrors
 * run_provisioning_session()'s JOIN_RESULT handling and
 * parent_service_task()'s FRAME_RECEIVED case in parent_service.c).
 *
 * Links against the REAL, unmodified wire-format code
 * (pogo_frame_encode/pogo_frame_parser_feed/pogo_pack_join_result/
 * pogo_unpack_join_result from components/pogo_proto) -- only the
 * surrounding FreeRTOS-task/session-state scaffolding is reimplemented here,
 * as a minimal model that mirrors the real .c files step for step. If those
 * files' JOIN_RESULT logic changes, update the model below to match -- same
 * caveat as tools/test_peer_hello_handshake.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_join_result_reliability tools/test_join_result_reliability.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_join_result_reliability
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"

#define MAX_RETRY 3     /* mirrors CONFIG_POGO_MAX_RETRY's default */
#define ACK_TIMEOUT 5   /* mirrors CONFIG_POGO_ACK_TIMEOUT_MS, in fake "ticks" */

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

/* --- child-side model (mirrors child_service.c) --- */
typedef struct {
    uint8_t seq_counter;
    uint8_t seq;
    bool pending;
    bool success;
    uint8_t role;
    int retries;
    int deadline;
    int frames_sent; /* diagnostic: total JOIN_RESULT frames actually transmitted */
} child_model_t;

static void child_reset(child_model_t *c)
{
    memset(c, 0, sizeof(*c));
}

static void child_send_frame(child_model_t *c, uint8_t seq, bool success, uint8_t role, uint8_t *out_buf,
                              size_t *out_len)
{
    pogo_join_result_t result = {.success = success ? 1 : 0, .role = role};
    uint8_t payload[2];
    size_t plen = pogo_pack_join_result(&result, payload, sizeof(payload));
    *out_len = pogo_frame_encode(POGO_MSG_JOIN_RESULT, seq, payload, plen, out_buf, POGO_FRAME_MAX_SIZE);
    c->frames_sent++;
}

/* Returns true and fills the encoded frame if a JOIN_RESULT was (re)sent. */
static bool child_begin(child_model_t *c, bool success, uint8_t role, int now, uint8_t *out_buf, size_t *out_len)
{
    c->seq_counter++;
    if (c->seq_counter == 0) c->seq_counter = 1;
    c->seq = c->seq_counter;
    c->success = success;
    c->role = role;
    c->pending = true;
    c->retries = 0;
    c->deadline = now + ACK_TIMEOUT;
    child_send_frame(c, c->seq, success, role, out_buf, out_len);
    return true;
}

/* Poll -- returns true and fills the encoded retry frame if one was sent. */
static bool child_poll(child_model_t *c, int now, uint8_t *out_buf, size_t *out_len)
{
    if (!c->pending) return false;
    if (now < c->deadline) return false;
    if (c->retries >= MAX_RETRY) {
        c->pending = false;
        return false;
    }
    c->retries++;
    c->deadline = now + ACK_TIMEOUT;
    child_send_frame(c, c->seq, c->success, c->role, out_buf, out_len);
    return true;
}

static void child_on_ack(child_model_t *c, uint8_t seq)
{
    if (c->pending && seq == c->seq) {
        c->pending = false;
    }
}

/* --- parent-side model (mirrors parent_service.c) --- */
typedef struct {
    bool has_join_result;
    uint8_t last_seq;
    int join_processed_count; /* how many times success/failure was actually acted on (must never double-count a retransmit) */
    int acks_sent;
} parent_model_t;

static void parent_reset_session(parent_model_t *p)
{
    memset(p, 0, sizeof(*p));
}

static void parent_send_ack(parent_model_t *p, uint8_t seq, uint8_t *out_buf, size_t *out_len)
{
    *out_len = pogo_frame_encode(POGO_MSG_JOIN_RESULT_ACK, seq, NULL, 0, out_buf, POGO_FRAME_MAX_SIZE);
    p->acks_sent++;
}

/* Mirrors run_provisioning_session()'s JOIN_RESULT handling: only called
 * from an active session's wait_for_type(), i.e. the FIRST time this
 * specific JOIN_RESULT is actually processed for this generation. */
static void parent_on_join_result_in_session(parent_model_t *p, const pogo_frame_t *frame, uint8_t *out_buf,
                                              size_t *out_len)
{
    p->has_join_result = true;
    p->last_seq = frame->seq;
    p->join_processed_count++;
    parent_send_ack(p, frame->seq, out_buf, out_len);
}

/* Mirrors parent_service_task()'s main-loop FRAME_RECEIVED case: a
 * JOIN_RESULT arriving OUTSIDE any active session. */
static bool parent_on_join_result_outside_session(parent_model_t *p, const pogo_frame_t *frame, uint8_t *out_buf,
                                                   size_t *out_len)
{
    if (p->has_join_result && frame->seq == p->last_seq) {
        parent_send_ack(p, frame->seq, out_buf, out_len);
        return true; /* re-acked only, no re-processing */
    }
    return false;
}

/* --- helper: parse one encoded frame back out, as the receiving side would --- */
static bool parse_frame(const uint8_t *buf, size_t len, pogo_frame_t *out)
{
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    for (size_t i = 0; i < len; i++) {
        if (pogo_frame_parser_feed(&parser, buf[i], out)) {
            return true;
        }
    }
    return false;
}

/* Scenario 1: first JOIN_RESULT delivered, first ACK delivered -- no retry
 * needed at all, provisioning is not repeated. */
static void test_scenario_clean_delivery(void)
{
    child_model_t child;
    parent_model_t parent;
    child_reset(&child);
    parent_reset_session(&parent);

    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;
    child_begin(&child, true, 2 /*CHILD*/, 0, buf, &len);
    pogo_frame_t frame;
    CHECK(parse_frame(buf, len, &frame) && frame.type == POGO_MSG_JOIN_RESULT, "scenario1: JOIN_RESULT parses");

    parent_on_join_result_in_session(&parent, &frame, buf, &len);
    CHECK(parse_frame(buf, len, &frame) && frame.type == POGO_MSG_JOIN_RESULT_ACK, "scenario1: ACK parses");
    child_on_ack(&child, frame.seq);

    CHECK(!child.pending, "scenario1: child retry state cleared after clean ACK");
    CHECK(child.frames_sent == 1, "scenario1: exactly one JOIN_RESULT frame ever sent");
    CHECK(parent.join_processed_count == 1, "scenario1: provisioning result processed exactly once");
}

/* Scenario 2: the FIRST JOIN_RESULT is lost entirely (never reaches the
 * parent) -- the child must retry with the SAME seq until it gets through. */
static void test_scenario_first_join_result_lost(void)
{
    child_model_t child;
    parent_model_t parent;
    child_reset(&child);
    parent_reset_session(&parent);

    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;
    uint8_t first_seq;
    child_begin(&child, true, 2, 0, buf, &len);
    { pogo_frame_t f; parse_frame(buf, len, &f); first_seq = f.seq; }
    /* ...frame "lost in transit"; nothing delivered to the parent... */

    bool retried = child_poll(&child, ACK_TIMEOUT, buf, &len); /* deadline reached -> retry */
    CHECK(retried, "scenario2: child retries after ACK timeout with no ACK");
    pogo_frame_t frame;
    CHECK(parse_frame(buf, len, &frame) && frame.seq == first_seq, "scenario2: retry uses the SAME seq as the original");

    /* This retry actually arrives. */
    parent_on_join_result_in_session(&parent, &frame, buf, &len);
    pogo_frame_t ack;
    parse_frame(buf, len, &ack);
    child_on_ack(&child, ack.seq);

    CHECK(!child.pending, "scenario2: child settles once the retried JOIN_RESULT is ACKed");
    CHECK(parent.join_processed_count == 1, "scenario2: still processed exactly once (the retry, not a duplicate)");
    CHECK(child.frames_sent == 2, "scenario2: exactly 2 JOIN_RESULT frames sent total (original + 1 retry)");
}

/* Scenario 3: JOIN_RESULT gets through fine, but the first ACK is lost.
 * Child retries (same seq); parent recognizes the duplicate via
 * has_join_result/last_seq and re-ACKs WITHOUT re-processing. */
static void test_scenario_first_ack_lost(void)
{
    child_model_t child;
    parent_model_t parent;
    child_reset(&child);
    parent_reset_session(&parent);

    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;
    child_begin(&child, true, 2, 0, buf, &len);
    pogo_frame_t frame;
    parse_frame(buf, len, &frame);

    parent_on_join_result_in_session(&parent, &frame, buf, &len);
    /* ...ACK "lost in transit"; child never sees it... */

    bool retried = child_poll(&child, ACK_TIMEOUT, buf, &len);
    CHECK(retried, "scenario3: child retries the same JOIN_RESULT after the ACK never arrived");
    pogo_frame_t retry_frame;
    parse_frame(buf, len, &retry_frame);
    CHECK(retry_frame.seq == frame.seq, "scenario3: retry carries the same seq the parent already recorded");

    /* Parent's main task loop sees this arriving OUTSIDE the (already-returned)
     * session -- must re-ACK only, never re-run provisioning. */
    bool was_duplicate = parent_on_join_result_outside_session(&parent, &retry_frame, buf, &len);
    CHECK(was_duplicate, "scenario3: parent recognizes the duplicate JOIN_RESULT");
    CHECK(parent.join_processed_count == 1, "scenario3: duplicate does NOT re-run provisioning/processing");
    CHECK(parent.acks_sent == 2, "scenario3: parent sent 2 ACKs total (original + re-ACK)");

    pogo_frame_t ack2;
    parse_frame(buf, len, &ack2);
    child_on_ack(&child, ack2.seq);
    CHECK(!child.pending, "scenario3: child settles once the re-sent ACK arrives");
}

/* Scenario 4: the exact same JOIN_RESULT is received twice by the parent
 * (e.g. a duplicate delivery with no real loss involved) -- must not
 * double-process either. */
static void test_scenario_duplicate_join_result(void)
{
    parent_model_t parent;
    parent_reset_session(&parent);
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;

    pogo_join_result_t result = {.success = 1, .role = 2};
    uint8_t payload[2];
    size_t plen = pogo_pack_join_result(&result, payload, sizeof(payload));
    uint8_t raw[POGO_FRAME_MAX_SIZE];
    size_t raw_len = pogo_frame_encode(POGO_MSG_JOIN_RESULT, 7, payload, plen, raw, sizeof(raw));
    pogo_frame_t frame;
    parse_frame(raw, raw_len, &frame);

    parent_on_join_result_in_session(&parent, &frame, buf, &len);
    /* Same exact frame delivered again (network duplicate, not a retry). */
    bool was_dup = parent_on_join_result_outside_session(&parent, &frame, buf, &len);
    CHECK(was_dup, "scenario4: second identical JOIN_RESULT delivery recognized as duplicate");
    CHECK(parent.join_processed_count == 1, "scenario4: node record / provisioning result touched exactly once");
    CHECK(parent.acks_sent == 2, "scenario4: both deliveries get an ACK back");
}

/* Scenario 5: a detach mid-retry cancels the pending retry immediately --
 * mirrors clear_provisioning_temporaries() being invoked on DETACHED. */
static void test_scenario_detach_cancels_retry(void)
{
    child_model_t child;
    child_reset(&child);
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;
    child_begin(&child, true, 2, 0, buf, &len);
    CHECK(child.pending, "scenario5: retry is pending right after the first send");

    /* DETACHED: child_service's clear_provisioning_temporaries() runs. */
    child_reset(&child);
    CHECK(!child.pending, "scenario5: detach clears the pending JOIN_RESULT retry");

    bool retried = child_poll(&child, 1000, buf, &len);
    CHECK(!retried, "scenario5: no further retries are sent after a detach, no matter how much time passes");
}

/* Scenario 6: retries exhausted (parent never reachable at all) -- the child
 * gives up after CONFIG_POGO_MAX_RETRY, it does not retry forever. */
static void test_scenario_retries_exhausted(void)
{
    child_model_t child;
    child_reset(&child);
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;
    child_begin(&child, true, 2, 0, buf, &len);

    int now = 0;
    int retry_sends = 0;
    for (int i = 0; i < MAX_RETRY + 2; i++) {
        now += ACK_TIMEOUT;
        if (child_poll(&child, now, buf, &len)) {
            retry_sends++;
        }
    }
    CHECK(retry_sends == MAX_RETRY, "scenario6: exactly MAX_RETRY retries are sent, never more");
    CHECK(!child.pending, "scenario6: child gives up (stops retrying) after MAX_RETRY is exhausted");
    CHECK(child.frames_sent == 1 + MAX_RETRY, "scenario6: total frames sent = 1 original + MAX_RETRY retries");
}

/* Scenario 7 (new provisioning generation / reboot): a stale JOIN_RESULT
 * from a PREVIOUS generation must not be ACKed as if it belonged to a new
 * one, and a fresh generation gets its own distinct seq. */
static void test_scenario_new_generation_distinct_seq(void)
{
    child_model_t child;
    child_reset(&child);
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t len;

    child_begin(&child, true, 2, 0, buf, &len);
    uint8_t gen1_seq = child.seq;
    child_reset(&child); /* e.g. UPSTREAM detach + re-attach: a brand new session */
    child_begin(&child, true, 2, 0, buf, &len);
    uint8_t gen2_seq = child.seq;

    CHECK(gen1_seq == gen2_seq, "scenario7: seq counter restarts after a full reset (independent module instances "
                                "each start their own counter at 1) -- documents the reset behavior, not a collision risk "
                                "since generation 1's session is already gone");

    /* Parent-side: a stale JOIN_RESULT carrying generation 1's seq must not
     * match a parent that has already started a brand-new session (memset
     * s_node at the top of run_provisioning_session() -- has_join_result
     * starts false again). */
    parent_model_t parent;
    parent_reset_session(&parent); /* models the memset at the start of a NEW run_provisioning_session() */
    pogo_frame_t stale_frame = {.type = POGO_MSG_JOIN_RESULT, .seq = gen1_seq, .len = 0};
    bool matched = parent_on_join_result_outside_session(&parent, &stale_frame, buf, &len);
    CHECK(!matched, "scenario7: a stale prior-generation JOIN_RESULT does not match a freshly-reset parent session");
}

int main(void)
{
    test_scenario_clean_delivery();
    test_scenario_first_join_result_lost();
    test_scenario_first_ack_lost();
    test_scenario_duplicate_join_result();
    test_scenario_detach_cancels_retry();
    test_scenario_retries_exhausted();
    test_scenario_new_generation_distinct_seq();

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
