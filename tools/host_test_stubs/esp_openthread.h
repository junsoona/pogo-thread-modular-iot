/*
 * Host-only stand-in for ESP-IDF's esp_openthread.h -- only
 * esp_openthread_get_instance(), the one function
 * components/sensor_transport/sensor_transport.c calls from it. Real body in
 * tools/host_test_stubs/host_ot_coap_sim.c -- see openthread/coap.h's own
 * header comment for why this stands in for the OpenThread stack dependency
 * itself, never sensor_transport.c's own logic.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct otInstance otInstance;

otInstance *esp_openthread_get_instance(void);

#ifdef __cplusplus
}
#endif
