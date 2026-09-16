#include "coap_response_timing/coap_response_timing.h"

#include <stdint.h>

uint32_t coap_response_timing_wait_ms(uint32_t ack_timeout_ms, uint32_t ack_random_factor_num,
                                       uint32_t ack_random_factor_den, uint32_t margin_ms)
{
    if (ack_random_factor_den == 0) {
        ack_random_factor_den = 1;
    }

    uint64_t worst_ack_wait_ms =
        ((uint64_t)ack_timeout_ms * (uint64_t)ack_random_factor_num) / (uint64_t)ack_random_factor_den;
    uint64_t total_ms = worst_ack_wait_ms + (uint64_t)margin_ms;

    if (total_ms > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)total_ms;
}
