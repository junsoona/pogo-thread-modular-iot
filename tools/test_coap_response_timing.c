/*
 * Host-compiled verification of the REAL, unmodified
 * components/coap_response_timing/coap_response_timing.c -- the shared
 * helper sensor_transport.c and topology_reporter.c both use instead of
 * separately hardcoding their own "how long do I wait for a CoAP response"
 * constant (this follow-up: SED CoAP response timing correction, see that
 * component's own header comment for the real-hardware finding behind it).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/coap_response_timing/include \
 *      -o /tmp/test_coap_response_timing tools/test_coap_response_timing.c \
 *      components/coap_response_timing/coap_response_timing.c \
 *   && /tmp/test_coap_response_timing
 */
#include <stdio.h>
#include <stdint.h>

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

int main(void)
{
    printf("\n-- scenario: this repository's actual defaults (ack_timeout=2000, factor 3/2, margin=ack_timeout) "
           "--\n");
    {
        uint32_t wait = coap_response_timing_wait_ms(2000, COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                                       COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN, 2000);
        /* worst_ack_wait = 2000*3/2 = 3000; + margin 2000 = 5000 */
        CHECK(wait == 5000, "2000ms ack timeout + 3/2 factor + 2000ms margin -> 5000ms wait");
        CHECK(wait > 2000u * COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM / COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN,
              "the returned wait always exceeds OpenThread's own worst-case ACK wait, never equal or less");
    }

    printf("\n-- scenario: default LEAF Thread data poll period (500ms) is comfortably shorter than the ACK "
           "wait, so a SED can poll several times within one CoAP exchange's own timeout window --\n");
    {
        uint32_t wait = coap_response_timing_wait_ms(2000, COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                                       COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN, 2000);
        uint32_t worst_ack_wait_only = 2000u * COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM /
                                        COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN;
        uint32_t leaf_data_poll_period_ms = 500;
        CHECK(worst_ack_wait_only / leaf_data_poll_period_ms >= 4,
              "a 500ms SED data-poll period fits at least 4 times inside OpenThread's own worst-case ACK wait "
              "window (3000ms) -- a queued response is virtually guaranteed to be picked up before either side "
              "gives up");
        CHECK(wait > leaf_data_poll_period_ms,
              "the full wait (with margin) is also, trivially, far longer than one data-poll period");
    }

    printf("\n-- scenario: zero margin still returns exactly the worst-case ACK wait --\n");
    {
        uint32_t wait = coap_response_timing_wait_ms(1000, 3, 2, 0);
        CHECK(wait == 1500, "1000ms * 3/2 + 0 margin = 1500ms");
    }

    printf("\n-- scenario: ack_random_factor_den == 0 is treated defensively as 1, never a divide-by-zero --\n");
    {
        uint32_t wait = coap_response_timing_wait_ms(1000, 3, 0, 0);
        CHECK(wait == 3000, "den=0 treated as den=1 -> 1000*3/1 = 3000ms, not a crash/UB");
    }

    printf("\n-- scenario: overflow-safe -- absurd inputs saturate to UINT32_MAX instead of wrapping --\n");
    {
        uint32_t wait = coap_response_timing_wait_ms(UINT32_MAX, UINT32_MAX, 1, UINT32_MAX);
        CHECK(wait == UINT32_MAX, "an input combination that would overflow a 32-bit intermediate saturates to "
                                   "UINT32_MAX rather than silently wrapping to a small/wrong value");
    }
    {
        /* A case that would overflow if computed naively in 32-bit
         * (ack_timeout_ms * num alone already overflows uint32_t here),
         * but must NOT overflow if done correctly in 64-bit before the
         * final saturation check. */
        uint32_t wait = coap_response_timing_wait_ms(3000000000u, 3, 2, 0);
        uint64_t expected = ((uint64_t)3000000000u * 3u) / 2u; /* = 4500000000, > UINT32_MAX */
        CHECK(expected > 0xFFFFFFFFull, "sanity: this scenario's own expected value does exceed uint32_t range");
        CHECK(wait == UINT32_MAX, "a case that overflows only in the final (not per-multiplicand) product still "
                                   "saturates correctly, proving the intermediate math is genuinely 64-bit");
    }

    printf("\n-- scenario: margin alone can also push an otherwise-small result past UINT32_MAX --\n");
    {
        uint32_t wait = coap_response_timing_wait_ms(100, 1, 1, UINT32_MAX);
        CHECK(wait == UINT32_MAX, "worst_ack_wait (100) + margin (UINT32_MAX) saturates instead of wrapping to 99");
    }

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
