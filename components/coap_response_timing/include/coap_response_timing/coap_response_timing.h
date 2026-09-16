/*
 * Shared CoAP client-side response-timing policy for sensor_transport
 * ("/s") and topology_reporter ("/t") -- both send exactly one Confirmable
 * POST per attempt with `otCoapTxParameters.mMaxRetransmit=0` (their own
 * application-level retry/backoff loop owns retries, not OpenThread's), then
 * block on a semaphore waiting for the CoAP response callback.
 *
 * Real-hardware finding (Linux OTBR external Dataset join + 3-tier chain
 * test): a PMS5003 LEAF logged repeated `sample dropped after 4 attempts`/
 * topology-report timeouts even though the power base's own log showed the
 * exact same sample sequence already stored -- not data loss, but the
 * SENDING side giving up on ITS OWN wait before ever hearing back. Root
 * cause: this LEAF's Thread SED MAC data-poll period was (at the time)
 * accidentally coupled to its sensor measurement period (3528ms) -- a SED
 * only receives a queued response from its parent on its own next data
 * poll, so a multi-second poll period could easily land AFTER the CoAP
 * client's semaphore wait had already timed out, even though the power
 * base's response was sitting there the whole time. Fixed by (a)
 * decoupling the Thread data-poll period from any sensor period (see
 * components/thread_manager's thread_manager_join_with_dataset()) so a SED
 * polls far more often than any single CoAP exchange takes, and (b) this
 * component, which gives both CoAP clients ONE shared, correctly-derived
 * wait bound instead of two independently-copied magic numbers.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * otCoapTxParameters.mAckRandomFactorNumerator/Denominator -- RFC7252's
 * ACK_RANDOM_FACTOR default (1.5), unchanged from what both components
 * already used independently; kept here as ONE named source instead of two
 * separate copies of the same two integers.
 */
#define COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM 3u
#define COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN 2u

/*
 * Pure, host-testable (no ESP-IDF/OpenThread dependency): computes how long
 * a caller should block waiting for its CoAP response callback after
 * issuing ONE Confirmable request with `otCoapTxParameters{mAckTimeout =
 * ack_timeout_ms, mAckRandomFactorNumerator = ack_random_factor_num,
 * mAckRandomFactorDenominator = ack_random_factor_den, mMaxRetransmit =
 * 0}`.
 *
 * Per openthread/coap.h's own documented formula (with mMaxRetransmit=0,
 * i.e. exactly one transmission, no OpenThread-level retransmission),
 * OpenThread's own worst-case wait for that ACK is:
 *
 *     ack_timeout_ms * ack_random_factor_num / ack_random_factor_den
 *
 * This function returns that worst case PLUS `margin_ms` (a caller-chosen
 * safety margin, e.g. to also absorb scheduling jitter/task-wakeup delay --
 * both callers in this repository currently pass `ack_timeout_ms` itself
 * as a simple, generous margin), so a caller's own semaphore wait can never
 * expire strictly before OpenThread's response handler could still validly
 * fire for that same attempt.
 *
 * All intermediate arithmetic is done in 64-bit and the final result is
 * saturated to UINT32_MAX (never silently wrapped) if the inputs would
 * otherwise overflow a uint32_t -- see this component's own host test for
 * the overflow case. ack_random_factor_den == 0 is treated as 1 (defensive;
 * OpenThread itself requires the random factor to be >= 1.0 and never 0).
 */
uint32_t coap_response_timing_wait_ms(uint32_t ack_timeout_ms, uint32_t ack_random_factor_num,
                                       uint32_t ack_random_factor_den, uint32_t margin_ms);

#ifdef __cplusplus
}
#endif
