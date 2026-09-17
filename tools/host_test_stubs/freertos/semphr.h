/*
 * Host-only, pthread-mutex+condvar-backed stand-in for FreeRTOS's semphr.h.
 * Originally only the plain-mutex subset components/sensor_service/
 * sensor_service.c actually uses (xSemaphoreCreateMutex/xSemaphoreTake/
 * xSemaphoreGive to serialize sensor_service_measure_once() against its own
 * periodic sampling task -- see that file's s_measure_mutex). See
 * freertos/task.h's own header comment for why this directory provides real
 * concurrency.
 *
 * xSemaphoreCreateBinary() was added for
 * components/sensor_transport/sensor_transport.c's s_send_done_sem (see
 * tools/host_test_stubs/host_ot_coap_sim.c) -- unlike xSemaphoreCreateMutex(),
 * a binary semaphore starts EMPTY (unavailable: the first take blocks until
 * a give), matching real FreeRTOS's own distinction between the two. Both
 * share the same take/give implementation below; only the initial `locked`
 * value differs.
 */
#pragma once

#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#include "freertos/task.h"
#include "freertos/queue.h" /* host_deadline_from_ticks() */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t released;
    bool locked;
} host_semaphore_record_t;

typedef host_semaphore_record_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    host_semaphore_record_t *s = (host_semaphore_record_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->released, NULL);
    s->locked = false;
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    host_semaphore_record_t *s = (host_semaphore_record_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->released, NULL);
    s->locked = true; /* starts empty -- see this file's top comment */
    return s;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks_to_wait)
{
    if (s == NULL) {
        return pdFAIL;
    }
    struct timespec deadline;
    bool have_deadline = false;
    pthread_mutex_lock(&s->mutex);
    while (s->locked) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&s->mutex);
            return pdFAIL;
        }
        if (!have_deadline) {
            host_deadline_from_ticks(&deadline, ticks_to_wait);
            have_deadline = true;
        }
        if (pthread_cond_timedwait(&s->released, &s->mutex, &deadline) != 0) {
            pthread_mutex_unlock(&s->mutex);
            return pdFAIL;
        }
    }
    s->locked = true;
    pthread_mutex_unlock(&s->mutex);
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s == NULL) {
        return pdFAIL;
    }
    pthread_mutex_lock(&s->mutex);
    s->locked = false;
    pthread_cond_signal(&s->released);
    pthread_mutex_unlock(&s->mutex);
    return pdTRUE;
}

/* Added for components/linux_bridge_exporter's host test (this follow-up:
 * northbound bridge transport) -- its error-handling paths call this on a
 * resource-creation failure, which no host test actually triggers, but the
 * symbol must still exist for the real production .c file to link. */
static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    if (s == NULL) {
        return;
    }
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->released);
    free(s);
}

#ifdef __cplusplus
}
#endif
