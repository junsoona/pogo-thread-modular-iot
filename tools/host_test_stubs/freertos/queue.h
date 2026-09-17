/*
 * Host-only, pthread-mutex+condvar-backed stand-in for FreeRTOS's queue.h --
 * see freertos/task.h's own header comment for why this directory provides
 * REAL concurrency rather than just enough types to compile. Used by every
 * real production .c file a host test here links that owns an event queue
 * fed from a pogo_port callback and drained by its own task
 * (components/pogo_port/pogo_port.c's detect_queue,
 * components/child_service/child_service.c's s_evt_queue,
 * components/parent_service/parent_service.c's s_evt_queue).
 *
 * A fixed-capacity ring buffer under one mutex; xQueueReceive()/xQueueSend()
 * block via pthread_cond_timedwait() (a POSIX cancellation point -- see
 * freertos/task.h) up to the requested ticks_to_wait, or return immediately
 * (pdFALSE) for ticks_to_wait==0, matching real FreeRTOS's own non-blocking
 * convention.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "freertos/task.h" /* TickType_t, BaseType_t, UBaseType_t, pdTRUE/pdFALSE */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *buf;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
} host_queue_record_t;

typedef host_queue_record_t *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    host_queue_record_t *q = (host_queue_record_t *)calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->buf = (uint8_t *)malloc((size_t)length * (size_t)item_size);
    if (q->buf == NULL) {
        free(q);
        return NULL;
    }
    q->item_size = item_size;
    q->capacity = length;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

static inline void host_deadline_from_ticks(struct timespec *out, TickType_t ticks_to_wait)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t ns = (uint64_t)now.tv_nsec + (uint64_t)ticks_to_wait * 1000000ULL;
    out->tv_sec = now.tv_sec + (time_t)(ns / 1000000000ULL);
    out->tv_nsec = (long)(ns % 1000000000ULL);
}

static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks_to_wait)
{
    if (q == NULL) {
        return pdFAIL;
    }
    struct timespec deadline;
    bool have_deadline = false;
    pthread_mutex_lock(&q->mutex);
    while (q->count == q->capacity) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&q->mutex);
            return pdFAIL;
        }
        if (!have_deadline) {
            host_deadline_from_ticks(&deadline, ticks_to_wait);
            have_deadline = true;
        }
        if (pthread_cond_timedwait(&q->not_full, &q->mutex, &deadline) != 0) {
            pthread_mutex_unlock(&q->mutex);
            return pdFAIL;
        }
    }
    size_t tail = (q->head + q->count) % q->capacity;
    memcpy(q->buf + tail * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return pdTRUE;
}

/* Host has no separate ISR context (see freertos/task.h) -- this is simply
 * xQueueSend() with no wait, matching real xQueueSendFromISR()'s own
 * never-blocks contract. *higher_priority_task_woken is always set false:
 * there is no host equivalent of "yield to a just-unblocked higher-priority
 * task at the end of the ISR" to report. */
static inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *higher_priority_task_woken)
{
    if (higher_priority_task_woken != NULL) {
        *higher_priority_task_woken = pdFALSE;
    }
    return xQueueSend(q, item, 0);
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks_to_wait)
{
    if (q == NULL) {
        return pdFAIL;
    }
    struct timespec deadline;
    bool have_deadline = false;
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&q->mutex);
            return pdFAIL;
        }
        if (!have_deadline) {
            host_deadline_from_ticks(&deadline, ticks_to_wait);
            have_deadline = true;
        }
        if (pthread_cond_timedwait(&q->not_empty, &q->mutex, &deadline) != 0) {
            pthread_mutex_unlock(&q->mutex);
            return pdFAIL;
        }
    }
    memcpy(out, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return pdTRUE;
}

/* Added for components/linux_bridge_exporter's host test (this follow-up:
 * northbound bridge transport) -- its error-handling paths call this on a
 * queue-creation failure, which no host test actually triggers, but the
 * symbol must still exist for the real production .c file to link. */
static inline void vQueueDelete(QueueHandle_t q)
{
    if (q == NULL) {
        return;
    }
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
    free(q);
}

#ifdef __cplusplus
}
#endif
