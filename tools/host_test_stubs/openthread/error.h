/*
 * Host-only stand-in for OpenThread's openthread/error.h -- only the otError
 * values components/sensor_transport/sensor_transport.c actually references
 * (OT_ERROR_NONE, OT_ERROR_NO_BUFS, OT_ERROR_BUSY) plus otThreadErrorToString(),
 * whose real body is provided by tools/host_test_stubs/host_ot_coap_sim.c
 * (see that file's own header comment). Numeric values match the real
 * OpenThread enum (openthread/include/openthread/error.h) so nothing about
 * this stub could make a log/comparison mean something different from real
 * hardware.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OT_ERROR_NONE = 0,
    OT_ERROR_FAILED = 1,
    OT_ERROR_DROP = 2,
    OT_ERROR_NO_BUFS = 3,
    OT_ERROR_NO_ROUTE = 4,
    OT_ERROR_BUSY = 5,
    OT_ERROR_PARSE = 6,
    OT_ERROR_INVALID_ARGS = 7,
} otError;

const char *otThreadErrorToString(otError error);

#ifdef __cplusplus
}
#endif
