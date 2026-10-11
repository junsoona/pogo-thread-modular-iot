/*
 * C glue for tools/ot_host_repro/pb_locate_nexus.cpp: HMAC-SHA-256 for the
 * management messages, from the same host implementation the other host
 * tests use (tools/host_test_stubs/host_sha256.h; the firmware uses PSA, the
 * hub OpenSSL -- the golden MACs in tools/test_power_base_locate.c tie the
 * three together).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host_sha256.h"

bool pb_locate_glue_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32],
                         void *ctx)
{
    (void)ctx;
    host_hmac_sha256(key, key_len, data, len, out);
    return true;
}
